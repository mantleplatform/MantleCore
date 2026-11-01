// Copyright Joshua Eger. All Rights Reserved.
#include "MantleScatterCells.h"

#include "MantleRuntime.h"
#include "MantleShadowDistance.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Containers/Queue.h"
#include "Engine/CollisionProfile.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Materials/MaterialInterface.h"
#include "Tasks/Task.h"

#include <atomic>

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleScatterCellsPrivate
{
	/** Tasks in flight at once, and finished cells waiting for the game thread past which no more are asked for (over every cell Scatter). */
	constexpr int32 MaxCellsInFlight = 16;
	constexpr int32 MaxCellsWaiting = 32;

	/**
	 * What every cell Scatter shares (game thread only): the live-instance cap, the cells in flight and waiting, and the frame's instance
	 * and component budget are checked against these, not against one Scatter's own counts.
	 */
	struct FScatterCellBudget
	{
		int64 LiveInstances = 0;
		int32 InFlight = 0;
		int32 Waiting = 0;
		uint64 Frame = 0;
		int64 AddedThisFrame = 0;
		int32 ComponentsThisFrame = 0;
	};
	FScatterCellBudget GBudget;

	/**
	 * Components made in one frame, whatever the instance budget: each registers a render proxy of its own, and a cell makes one per mesh it has
	 * (a Scatter has up to 16), so a cell of an ordinary Scatter fits in a frame and a cell over the budget still goes in alone.
	 */
	constexpr int32 MaxComponentsPerFrame = 24;

	/** Seconds between looks at where the view is, and between summary lines. */
	constexpr double ScanIntervalSeconds = 0.2;
	constexpr double LogIntervalSeconds = 60.0;

	/** The start line is written even if the first fill has not finished this long after the first look. */
	constexpr double StartLogDeadlineSeconds = 15.0;

	/**
	 * The live-instance cap. A Scatter's meshes are ordinary static meshes (trees, rocks, shrubs), not blades of grass, so the default is below
	 * terrain grass's 1,500,000: about 1,000,000 standing (the draw distance decides how many that is) costs on the order of 100 MB of
	 * instance data. Tune it with this variable on the target machine.
	 */
	TAutoConsoleVariable<int32> CVarScatterMaxLiveInstances(
		TEXT("mantle.Scatter.MaxLiveInstances"), 1000000,
		TEXT("Scatters built in cells: the most instances standing at once, over every such Scatter. Nearer cells are built first; a cell that would pass this waits until some are released."),
		ECVF_Default);

	/**
	 * The most instances added to components in one frame. 100,000 instances were measured to build into instanced meshes in 12.7 ms (#530), so
	 * 20,000 is about 2.5 ms a frame: walking into new ground never hitches. A cell larger than this still goes in, alone.
	 */
	TAutoConsoleVariable<int32> CVarScatterMaxInstancesPerFrame(
		TEXT("mantle.Scatter.MaxInstancesPerFrame"), 20000,
		TEXT("Scatters built in cells: the most instances added to components in one frame, over every such Scatter. A cell larger than this still goes in, alone."),
		ECVF_Default);

	uint64 CellCoordKey(const FIntPoint& Coord)
	{
		return (static_cast<uint64>(static_cast<uint32>(Coord.X)) << 32) | static_cast<uint64>(static_cast<uint32>(Coord.Y));
	}

	/** Squared X-Y distance from a point to a box (0 inside it), cm. */
	double DistanceSquaredToBox(const FVector& Point, const FBox2D& Box)
	{
		const double Dx = FMath::Max3(Box.Min.X - Point.X, 0.0, Point.X - Box.Max.X);
		const double Dy = FMath::Max3(Box.Min.Y - Point.Y, 0.0, Point.Y - Box.Max.Y);
		return Dx * Dx + Dy * Dy;
	}
}

/** What a worker holds: the immutable inputs, what it hands back, and whether anyone still wants it. */
struct UMantleScatterCellsComponent::FShared
{
	FMantleScatterParams Params;
	FMantleScatterGenerateOptions Options;
	std::atomic<bool> bCancelled{ false };

	TQueue<FBuiltCell, EQueueMode::Mpsc> Results;
};

UMantleScatterCellsComponent::UMantleScatterCellsComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
	bAutoActivate = true;
}

uint64 UMantleScatterCellsComponent::CellKey(const FIntPoint& Coord)
{
	return MantleScatterCellsPrivate::CellCoordKey(Coord);
}

int64 UMantleScatterCellsComponent::GetWorldLiveInstances()
{
	return MantleScatterCellsPrivate::GBudget.LiveInstances;
}

void UMantleScatterCellsComponent::Init(const FMantleScatterParams& InParams, const FMantleScatterGenerateOptions& InOptions,
	TArray<FMantleGrassEntryAssets> InAssets, const double InReachCm, UE::Tasks::TTask<TSharedPtr<const FMantleScatterCellIndex>> InIndexTask)
{
	Params = InParams;
	Options = InOptions;
	Assets = MoveTemp(InAssets);
	Assets.SetNum(Params.Meshes.Num());
	ReachCm = InReachCm;

	Shared = MakeShared<FShared, ESPMode::ThreadSafe>();
	Shared->Params = Params;
	Shared->Options = Options;

	// The index is the one thing that reads the whole stream, and it is cheap (a place and no filter per candidate): the volume builds it on a worker.
	IndexTask = MoveTemp(InIndexTask);
}

void UMantleScatterCellsComponent::OnComponentDestroyed(const bool bDestroyingHierarchy)
{
	if (Shared.IsValid())
	{
		Shared->bCancelled.store(true);
	}
	// The cells' mesh components belong to the actor, which outlives this component (ClearScatter, a settings change), so they are taken
	// down here; the ones still being read come back to a cancelled task and are never counted.
	ReleaseAll();
	MantleScatterCellsPrivate::GBudget.LiveInstances -= LiveInstances;
	MantleScatterCellsPrivate::GBudget.InFlight -= InFlight;
	MantleScatterCellsPrivate::GBudget.Waiting -= Ready.Num();
	LiveInstances = 0;
	InFlight = 0;
	Ready.Reset();
	Super::OnComponentDestroyed(bDestroyingHierarchy);
}

bool UMantleScatterCellsComponent::GetViewLocal(FVector& OutLocal) const
{
	const AActor* Owner = GetOwner();
	if (!Owner)
	{
		return false;
	}
	if (ViewOverride.IsSet())
	{
		OutLocal = ViewOverride.GetValue();
		return true;
	}
	const UWorld* World = GetWorld();
	if (!World || !GEngine)
	{
		return false;
	}
	const APlayerController* Controller = GEngine->GetFirstLocalPlayerController(World);
	if (!Controller)
	{
		return false;
	}
	FVector Location = FVector::ZeroVector;
	FRotator Rotation = FRotator::ZeroRotator;
	Controller->GetPlayerViewPoint(Location, Rotation);
	OutLocal = Owner->GetActorTransform().InverseTransformPosition(Location);
	return true;
}

void UMantleScatterCellsComponent::TickComponent(const float DeltaTime, const ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	using namespace MantleScatterCellsPrivate;

	if (!Shared.IsValid())
	{
		return;
	}

	// The index arrives once; without one nothing is read (and that is logged, once).
	if (!Index.IsValid() && !bIndexFailed && IndexTask.IsValid() && IndexTask.IsCompleted())
	{
		Index = IndexTask.GetResult();
		if (!Index.IsValid())
		{
			bIndexFailed = true;
			UE_LOG(LogMantle, Warning, TEXT("MANTLE_SCATTER_CELLS %s: no cells: the Scatter could not be indexed"), *GetOwner()->GetName());
		}
	}
	if (!Index.IsValid())
	{
		return;
	}

	// What the workers finished: new work waits at most MaxCellsWaiting cells for the game thread.
	FBuiltCell Finished;
	while (Shared->Results.Dequeue(Finished))
	{
		--InFlight;
		--GBudget.InFlight;
		++GBudget.Waiting;
		Ready.Add(MoveTemp(Finished));
	}

	const double Now = FPlatformTime::Seconds();
	if (Now >= NextScanSeconds)
	{
		NextScanSeconds = Now + ScanIntervalSeconds;
		FVector View = FVector::ZeroVector;
		if (GetViewLocal(View))
		{
			if (FirstScanSeconds == 0.0)
			{
				FirstScanSeconds = Now;
			}
			Scan(View);
		}
	}

	// Ask for the nearest cells first.
	while (GBudget.InFlight < MaxCellsInFlight && GBudget.Waiting + GBudget.InFlight < MaxCellsWaiting && Wanted.Num() > 0)
	{
		const FWanted Want = Wanted.Pop(EAllowShrinking::No);
		if (!Cells.Contains(CellKey(Want.Coord)))
		{
			StartGenerating(Want);
		}
	}

	// Give finished cells their components, within the frame's budget. A cell over the budget goes in alone, so none starves.
	const int64 Budget = FMath::Max(CVarScatterMaxInstancesPerFrame.GetValueOnGameThread(), 1);
	// The frame's budget is shared by every Scatter's cells: the components tick one after another in a frame.
	if (GBudget.Frame != GFrameCounter)
	{
		GBudget.Frame = GFrameCounter;
		GBudget.AddedThisFrame = 0;
		GBudget.ComponentsThisFrame = 0;
	}
	const double ApplyStart = FPlatformTime::Seconds();
	int32 Consumed = 0;
	for (; Consumed < Ready.Num() && GBudget.ComponentsThisFrame < MaxComponentsPerFrame; ++Consumed)
	{
		const int64 Count = Ready[Consumed].Instances.Num();
		if (GBudget.AddedThisFrame > 0 && GBudget.AddedThisFrame + Count > Budget)
		{
			break;
		}
		GBudget.AddedThisFrame += Count;
		Apply(MoveTemp(Ready[Consumed]));
	}
	Ready.RemoveAt(0, Consumed, EAllowShrinking::No);
	GBudget.Waiting -= Consumed;
	if (Consumed > 0)
	{
		const double Ms = (FPlatformTime::Seconds() - ApplyStart) * 1000.0;
		ApplyMsSinceLog += Ms;
		ApplyMsMaxFrame = FMath::Max(ApplyMsMaxFrame, Ms);
		++ApplyFramesSinceLog;
	}

	if (!bStartLogged && FirstScanSeconds > 0.0 && ((Cells.Num() > 0 && IsSettled()) || Now - FirstScanSeconds > StartLogDeadlineSeconds))
	{
		bStartLogged = true;
		NextLogSeconds = Now + LogIntervalSeconds;
		LogSummary(true);
	}
	else if (bStartLogged && Now >= NextLogSeconds)
	{
		NextLogSeconds = Now + LogIntervalSeconds;
		if (BuiltSinceLog > 0 || ReleasedSinceLog > 0 || SkippedSinceLog > 0)
		{
			LogSummary(false);
		}
	}
}

void UMantleScatterCellsComponent::Scan(const FVector& View)
{
	using namespace MantleScatterCellsPrivate;

	// A scaled actor measures the draw distance in world cm, the cells in its own space.
	const AActor* Owner = GetOwner();
	const double Scale = Owner ? FMath::Max(FMath::Max(FMath::Abs(Owner->GetActorScale3D().X), FMath::Abs(Owner->GetActorScale3D().Y)), UE_KINDA_SMALL_NUMBER) : 1.0;
	const double Reach = ReachCm / Scale;
	const double Side = static_cast<double>(Index->CellMm) * 0.1;

	// Release what the view has left, with a cell's width of hysteresis past the reach.
	TArray<uint64> ToRelease;
	for (const TPair<uint64, FCell>& Pair : Cells)
	{
		const double Past = Reach + Side;
		if (DistanceSquaredToBox(View, Index->CellBoundsCm(Pair.Value.Coord)) > Past * Past)
		{
			ToRelease.Add(Pair.Key);
		}
	}
	for (const uint64 Key : ToRelease)
	{
		ReleaseCell(Key);
	}

	// A cell the live-instance cap turned away is tried again only once that many instances fit. One over the cap on its own never is, so
	// it is not read again every scan.
	const int64 MaxLive = FMath::Max(CVarScatterMaxLiveInstances.GetValueOnGameThread(), 1);
	TArray<uint64> Skipped;
	for (const TPair<uint64, FCell>& Pair : Cells)
	{
		if (Pair.Value.State == ECellState::Skipped && GBudget.LiveInstances + Pair.Value.Needed <= MaxLive)
		{
			Skipped.Add(Pair.Key);
		}
	}
	for (const uint64 Key : Skipped)
	{
		Cells.Remove(Key);
	}

	Wanted.Reset();
	if (Index->NumCells() == 0)
	{
		return;
	}
	const FIntPoint First = Index->CellAt(FMath::FloorToInt64((View.X - Reach) * 10.0), FMath::FloorToInt64((View.Y - Reach) * 10.0));
	const FIntPoint Last = Index->CellAt(FMath::CeilToInt64((View.X + Reach) * 10.0), FMath::CeilToInt64((View.Y + Reach) * 10.0));
	for (int32 CellY = First.Y; CellY <= Last.Y; ++CellY)
	{
		for (int32 CellX = First.X; CellX <= Last.X; ++CellX)
		{
			const FIntPoint Coord(CellX, CellY);
			uint32 Begin = 0;
			uint32 End = 0;
			Index->SlotRange(Coord, Begin, End);
			if (Begin == End)
			{
				continue;
			}
			const double DistanceSquared = DistanceSquaredToBox(View, Index->CellBoundsCm(Coord));
			if (DistanceSquared > Reach * Reach || Cells.Contains(CellKey(Coord)))
			{
				continue;
			}
			FWanted& Want = Wanted.AddDefaulted_GetRef();
			Want.Coord = Coord;
			Want.DistanceSquared = DistanceSquared;
		}
	}
	// Farthest first, so Pop takes the nearest.
	Wanted.Sort([](const FWanted& A, const FWanted& B) { return A.DistanceSquared > B.DistanceSquared; });
}

void UMantleScatterCellsComponent::StartGenerating(const FWanted& Want)
{
	using namespace MantleScatterCellsPrivate;

	FCell& Cell = Cells.Add(CellKey(Want.Coord));
	Cell.State = ECellState::Generating;
	Cell.Coord = Want.Coord;
	++InFlight;
	++GBudget.InFlight;

	const TSharedPtr<FShared, ESPMode::ThreadSafe> Work = Shared;
	const TSharedPtr<const FMantleScatterCellIndex> Grid = Index;
	const FIntPoint Coord = Want.Coord;
	const int32 ForGeneration = Generation;
	UE::Tasks::Launch(TEXT("MantleScatterCell"), [Work, Grid, Coord, ForGeneration]()
	{
		if (Work->bCancelled.load())
		{
			return;
		}
		FBuiltCell Built;
		Built.Coord = Coord;
		Built.Generation = ForGeneration;
		const double Start = FPlatformTime::Seconds();
		FString Error;
		// Reads only this task's own copies and the immutable inputs: no object of the engine is touched off the game thread.
		FMantleScatter::GenerateCell(Work->Params, *Grid, Coord, Built.Instances, Error, Work->Options);
		Built.GenerateMs = (FPlatformTime::Seconds() - Start) * 1000.0;
		Work->Results.Enqueue(MoveTemp(Built));
	}, LowLevelTasks::ETaskPriority::BackgroundNormal);
}

void UMantleScatterCellsComponent::Apply(FBuiltCell&& Built)
{
	using namespace MantleScatterCellsPrivate;

	// A result from before everything was released, or for a cell released while it was made, is dropped.
	if (Built.Generation != Generation)
	{
		return;
	}
	FCell* Cell = Cells.Find(CellKey(Built.Coord));
	if (!Cell || Cell->State != ECellState::Generating)
	{
		return;
	}
	++BuiltTotal;
	++BuiltSinceLog;
	GenerateMsSinceLog += Built.GenerateMs;

	const int32 Count = Built.Instances.Num();
	if (Count == 0)
	{
		// Nothing stands here: remembered, so it is not asked for again until the view leaves and returns.
		Cell->State = ECellState::Live;
		return;
	}
	const int64 MaxLive = FMath::Max(CVarScatterMaxLiveInstances.GetValueOnGameThread(), 1);
	AActor* Owner = GetOwner();
	USceneComponent* Root = Owner ? Owner->GetRootComponent() : nullptr;
	if (!Root || GBudget.LiveInstances + Count > MaxLive)
	{
		Cell->State = ECellState::Skipped;
		Cell->Needed = Count;
		++SkippedSinceLog;
		return;
	}

	// One transform list per mesh, relative to the cell's corner (a float holds a position near it exactly; a whole Scatter's far corner would not).
	const FBox2D Bounds = Index->CellBoundsCm(Built.Coord);
	const FVector Corner(Bounds.Min.X, Bounds.Min.Y, 0.0);
	TArray<TArray<FTransform>> Buckets;
	Buckets.SetNum(Assets.Num());
	for (const FMantleScatterInstance& Instance : Built.Instances)
	{
		if (Assets.IsValidIndex(Instance.Mesh) && Assets[Instance.Mesh].Mesh)
		{
			FTransform Transform = FMantleScatter::ToTransform(Instance);
			Transform.AddToTranslation(-Corner);
			Buckets[Instance.Mesh].Add(Transform);
		}
	}

	UWorld* World = GetWorld();
	UMantleShadowDistanceSubsystem* Shadows = World ? World->GetSubsystem<UMantleShadowDistanceSubsystem>() : nullptr;
	// A drawDistance fades each instance out over its last fifth and draws none past it, as a whole Scatter's does (per instance, not the
	// component's Max Draw Distance, which is measured to the whole set's bounds).
	const int32 CullEnd = FMath::Max(FMath::RoundToInt32(ReachCm), 1);
	const int32 CullStart = FMath::Min(FMath::RoundToInt32(ReachCm * FadeStartShare), CullEnd);
	int32 Stood = 0;
	for (int32 MeshIndex = 0; MeshIndex < Buckets.Num(); ++MeshIndex)
	{
		if (Buckets[MeshIndex].Num() == 0)
		{
			continue;
		}
		const FMantleGrassEntryAssets& Looks = Assets[MeshIndex];
		UInstancedStaticMeshComponent* Ism = NewObject<UInstancedStaticMeshComponent>(Owner, NAME_None,
			RF_Transient | RF_TextExportTransient | RF_DuplicateTransient);
		Ism->SetupAttachment(Root);
		Ism->SetMobility(Root->GetMobility());
		Ism->SetRelativeLocation(Corner);
		Ism->SetStaticMesh(Looks.Mesh);
		Ism->SetCullDistances(CullStart, CullEnd);
		if (Shadows && ReachCm > 0.0)
		{
			Shadows->Track(Ism, ReachCm);
		}
		for (int32 Slot = 0; Slot < Looks.Materials.Num(); ++Slot)
		{
			if (Looks.Materials[Slot])
			{
				Ism->SetMaterial(Slot, Looks.Materials[Slot]);
			}
		}
		// Visual only: no physics bodies, no overlaps, nothing for navigation.
		Ism->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
		Ism->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Ism->SetGenerateOverlapEvents(false);
		Ism->SetCanEverAffectNavigation(false);
		// Instances go in before the component registers, so its render state is built once for all of them.
		Ism->PreAllocateInstancesMemory(Buckets[MeshIndex].Num());
		Ism->AddInstances(Buckets[MeshIndex], /*bShouldReturnIndices*/ false, /*bWorldSpace*/ false, /*bUpdateNavigation*/ false);
		Ism->RegisterComponent();
		Owner->AddInstanceComponent(Ism);
		Cell->Components.Add(Ism);
		Stood += Buckets[MeshIndex].Num();
		++GBudget.ComponentsThisFrame;
	}

	Cell->State = ECellState::Live;
	Cell->Instances = Stood;
	LiveInstances += Stood;
	GBudget.LiveInstances += Stood;
	if (Stood > 0)
	{
		++LiveCells;
	}
}

void UMantleScatterCellsComponent::ReleaseCell(const uint64 Key)
{
	FCell* Cell = Cells.Find(Key);
	if (!Cell)
	{
		return;
	}
	bool bStood = false;
	for (const TWeakObjectPtr<UInstancedStaticMeshComponent>& Component : Cell->Components)
	{
		if (UInstancedStaticMeshComponent* Ism = Component.Get())
		{
			if (AActor* Owner = GetOwner())
			{
				Owner->RemoveInstanceComponent(Ism);
			}
			Ism->DestroyComponent();
			bStood = true;
		}
	}
	if (bStood || Cell->Instances > 0)
	{
		LiveInstances -= Cell->Instances;
		MantleScatterCellsPrivate::GBudget.LiveInstances -= Cell->Instances;
		--LiveCells;
		++ReleasedSinceLog;
	}
	Cells.Remove(Key);
}

void UMantleScatterCellsComponent::ReleaseAll()
{
	TArray<uint64> Keys;
	Cells.GetKeys(Keys);
	for (const uint64 Key : Keys)
	{
		ReleaseCell(Key);
	}
	Cells.Reset();
	Wanted.Reset();
	MantleScatterCellsPrivate::GBudget.Waiting -= Ready.Num();
	Ready.Reset();
	LiveCells = 0;
	// ReleaseCell took each live cell's instances off both counts; clear any a lost component left.
	MantleScatterCellsPrivate::GBudget.LiveInstances -= LiveInstances;
	LiveInstances = 0;
	// Cells still being read come back with the old generation and are dropped.
	++Generation;
}

void UMantleScatterCellsComponent::LogSummary(const bool bStarted)
{
	const AActor* Owner = GetOwner();
	const FString Name = Owner ? Owner->GetName() : FString();
	const double IndexMb = Index.IsValid() ? static_cast<double>(Index->MemoryBytes()) / (1024.0 * 1024.0) : 0.0;
	if (bStarted)
	{
		UE_LOG(LogMantle, Log,
			TEXT("MANTLE_SCATTER_CELLS entity=%s candidates=%d grid=%dx%d cell_m=%.1f cells=%d instances=%lld index_mb=%.1f built=%d gen_ms_avg=%.2f build_ms_frame_avg=%.2f build_ms_frame_max=%.2f"),
			*Name, Index.IsValid() ? Index->NumSlots() : 0, Index.IsValid() ? Index->CellsX : 0, Index.IsValid() ? Index->CellsY : 0,
			Index.IsValid() ? static_cast<double>(Index->CellMm) / 1000.0 : 0.0, LiveCells, LiveInstances, IndexMb, BuiltSinceLog,
			BuiltSinceLog > 0 ? GenerateMsSinceLog / BuiltSinceLog : 0.0,
			ApplyFramesSinceLog > 0 ? ApplyMsSinceLog / ApplyFramesSinceLog : 0.0, ApplyMsMaxFrame);
	}
	else
	{
		UE_LOG(LogMantle, Log,
			TEXT("MANTLE_SCATTER_CELLS entity=%s cells=%d instances=%lld built=%d released=%d skipped=%d pending=%d gen_ms_avg=%.2f build_ms_frame_avg=%.2f build_ms_frame_max=%.2f world_instances=%lld"),
			*Name, LiveCells, LiveInstances, BuiltSinceLog, ReleasedSinceLog, SkippedSinceLog, GetPendingCells(),
			BuiltSinceLog > 0 ? GenerateMsSinceLog / BuiltSinceLog : 0.0,
			ApplyFramesSinceLog > 0 ? ApplyMsSinceLog / ApplyFramesSinceLog : 0.0, ApplyMsMaxFrame, MantleScatterCellsPrivate::GBudget.LiveInstances);
	}
	BuiltSinceLog = 0;
	ReleasedSinceLog = 0;
	SkippedSinceLog = 0;
	GenerateMsSinceLog = 0.0;
	ApplyMsSinceLog = 0.0;
	ApplyMsMaxFrame = 0.0;
	ApplyFramesSinceLog = 0;
}
