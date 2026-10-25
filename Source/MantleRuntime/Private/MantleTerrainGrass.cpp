// Copyright Joshua Eger. All Rights Reserved.
#include "MantleTerrainGrass.h"

#include "MantleHeightfieldReader.h"
#include "MantleRuntime.h"
#include "MantleTerrainBuilder.h"
#include "MantleTerrainSurface.h"

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
namespace MantleTerrainGrassPrivate
{
	constexpr uint64 GrassGoldenGamma = 0x9E3779B97F4A7C15ull;

	/** SplitMix64's output function: a bijective 64-bit mix (MantleScatter's, which keeps its own copy private). */
	uint64 GrassMix64(uint64 Z)
	{
		Z = (Z ^ (Z >> 30)) * 0xBF58476D1CE4E5B9ull;
		Z = (Z ^ (Z >> 27)) * 0x94D049BB133111EBull;
		return Z ^ (Z >> 31);
	}

	/** One candidate's own random stream: SplitMix64 started from the cell's seed and the candidate's index. */
	struct FGrassStream
	{
		uint64 State;

		FGrassStream(const uint64 Seed, const uint32 Index)
			: State(GrassMix64(Seed + static_cast<uint64>(Index) * GrassGoldenGamma))
		{
		}

		uint64 Next()
		{
			State += GrassGoldenGamma;
			return GrassMix64(State);
		}

		/** 0 up to, never reaching, 1. */
		double NextUnit()
		{
			return static_cast<double>(Next() >> 11) * (1.0 / 9007199254740992.0);
		}
	};

	/** Squared distance in X and Y from a point to the cell Coord of side Side (0 inside it). */
	double DistanceSquaredToCell(const FVector& Point, const FIntPoint& Coord, const double Side)
	{
		const double MinX = static_cast<double>(Coord.X) * Side;
		const double MinY = static_cast<double>(Coord.Y) * Side;
		const double Dx = FMath::Max3(MinX - Point.X, 0.0, Point.X - (MinX + Side));
		const double Dy = FMath::Max3(MinY - Point.Y, 0.0, Point.Y - (MinY + Side));
		return Dx * Dx + Dy * Dy;
	}

	/** Tasks in flight at once, and finished cells waiting for the game thread past which no more are asked for. */
	constexpr int32 MaxCellsInFlight = 32;
	constexpr int32 MaxCellsWaiting = 64;

	/** Components made in one frame, whatever the instance budget: each registers a render proxy of its own. */
	constexpr int32 MaxComponentsPerFrame = 12;

	/** Seconds between looks at where the view is, and between summary lines. */
	constexpr double ScanIntervalSeconds = 0.2;
	constexpr double LogIntervalSeconds = 60.0;

	/** The start line is written even if the first fill has not finished this long after the first look. */
	constexpr double StartLogDeadlineSeconds = 15.0;

	TAutoConsoleVariable<float> CVarGrassDensityScale(
		TEXT("mantle.Grass.DensityScale"), 1.0f,
		TEXT("Terrain grass: scales every grass entry's density. 1 is what the place says, 0 turns grass off, at most 2. Standing grass is rebuilt when it changes."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarGrassMaxInstancesPerFrame(
		TEXT("mantle.Grass.MaxInstancesPerFrame"), 20000,
		TEXT("Terrain grass: the most instances added to grass components in one frame, so walking into new ground never hitches. A cell larger than this still goes in, alone."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarGrassMaxLiveInstances(
		TEXT("mantle.Grass.MaxLiveInstances"), 1500000,
		TEXT("Terrain grass: the most grass instances standing at once. Nearer cells are grown first; a cell that would pass this waits until some are released."),
		ECVF_Default);
}

// ---- FMantleTerrainGrassGenerator ---------------------------------------------------------------

double FMantleTerrainGrassGenerator::CellSizeCm(const FMantleTerrainGrass& Entry)
{
	double Side = BaseCellCm;
	while (Side > MinCellCm && Entry.Density * (Side / 100.0) * (Side / 100.0) > static_cast<double>(MaxCandidatesPerCell))
	{
		Side *= 0.5;
	}
	return Side;
}

FIntPoint FMantleTerrainGrassGenerator::CellCount(const FMantleHeightfield& Field, const double Side)
{
	if (Side <= 0.0)
	{
		return FIntPoint(1, 1);
	}
	return FIntPoint(
		FMath::Max(1, FMath::CeilToInt32(Field.ExtentX() / Side)),
		FMath::Max(1, FMath::CeilToInt32(Field.ExtentY() / Side)));
}

int32 FMantleTerrainGrassGenerator::CandidateCount(const FMantleTerrainGrass& Entry, const double DensityScale)
{
	const double Side = CellSizeCm(Entry) / 100.0;
	const double Asked = Entry.Density * Side * Side * DensityScale;
	if (!(Asked > 0.0))
	{
		return 0;
	}
	return FMath::RoundToInt32(FMath::Min(Asked, static_cast<double>(MaxCandidatesPerCell) * 2.0));
}

uint64 FMantleTerrainGrassGenerator::CellSeed(const int32 EntryIndex, const FIntPoint& Cell)
{
	using namespace MantleTerrainGrassPrivate;
	const uint64 ForEntry = GrassMix64(GrassGoldenGamma * (static_cast<uint64>(static_cast<uint32>(EntryIndex)) + 1));
	const uint64 Packed = (static_cast<uint64>(static_cast<uint32>(Cell.X)) << 32) | static_cast<uint64>(static_cast<uint32>(Cell.Y));
	return GrassMix64(ForEntry ^ Packed);
}

bool FMantleTerrainGrassGenerator::SurfaceAt(const FMantleHeightfield& Field, const double X, const double Y, float& OutZ, FVector3f& OutNormal)
{
	if (Field.Width < 2 || Field.Height < 2 || !(Field.SpacingX > 0.0f) || !(Field.SpacingY > 0.0f)
		|| Field.Heights.Num() != Field.Width * Field.Height)
	{
		return false;
	}
	const double GridX = FMath::Clamp(X / Field.SpacingX, 0.0, static_cast<double>(Field.Width - 1));
	const double GridY = FMath::Clamp(Y / Field.SpacingY, 0.0, static_cast<double>(Field.Height - 1));
	const int32 X0 = FMath::Min(FMath::FloorToInt32(GridX), Field.Width - 2);
	const int32 Y0 = FMath::Min(FMath::FloorToInt32(GridY), Field.Height - 2);
	const float FX = static_cast<float>(GridX - X0);
	const float FY = static_cast<float>(GridY - Y0);

	// The chunk meshes split each quad from (x + 1, y) to (x, y + 1): triangles (x, y), (x, y + 1), (x + 1, y)
	// and (x + 1, y), (x, y + 1), (x + 1, y + 1). The height is on whichever one holds the point.
	const float H00 = Field.HeightAt(X0, Y0);
	const float H10 = Field.HeightAt(X0 + 1, Y0);
	const float H01 = Field.HeightAt(X0, Y0 + 1);
	const float H11 = Field.HeightAt(X0 + 1, Y0 + 1);
	OutZ = FX + FY <= 1.0f
		? H00 + FX * (H10 - H00) + FY * (H01 - H00)
		: H11 + (1.0f - FX) * (H01 - H11) + (1.0f - FY) * (H10 - H11);

	const FVector3f N00 = FMantleTerrainBuilder::SampleNormal(Field, X0, Y0);
	const FVector3f N10 = FMantleTerrainBuilder::SampleNormal(Field, X0 + 1, Y0);
	const FVector3f N01 = FMantleTerrainBuilder::SampleNormal(Field, X0, Y0 + 1);
	const FVector3f N11 = FMantleTerrainBuilder::SampleNormal(Field, X0 + 1, Y0 + 1);
	const FVector3f Blended = FMath::Lerp(FMath::Lerp(N00, N10, FX), FMath::Lerp(N01, N11, FX), FY);
	OutNormal = Blended.GetSafeNormal(UE_SMALL_NUMBER, FVector3f::UpVector);
	return true;
}

void FMantleTerrainGrassGenerator::GenerateCell(const FMantleTerrainGrass& Entry, const int32 EntryIndex, const FIntPoint& Cell,
	const FMantleHeightfield& Field, const FMantleTerrainWeights& Map, const double DensityScale, FMantleGrassCellInstances& Out)
{
	using namespace MantleTerrainGrassPrivate;

	const double Side = CellSizeCm(Entry);
	Out.Entry = EntryIndex;
	Out.Cell = Cell;
	Out.DensityScale = DensityScale;
	Out.Origin = FVector(static_cast<double>(Cell.X) * Side, static_cast<double>(Cell.Y) * Side, 0.0);
	Out.Instances.Reset();
	Out.Candidates = 0;

	const double ExtentX = Field.ExtentX();
	const double ExtentY = Field.ExtentY();
	if (!Map.HasPixels() || !(ExtentX > 0.0) || !(ExtentY > 0.0) || !(DensityScale > 0.0))
	{
		return;
	}
	const int32 Candidates = CandidateCount(Entry, DensityScale);
	Out.Candidates = Candidates;

	const uint64 Seed = CellSeed(EntryIndex, Cell);
	const double ScaleLow = Entry.MinScale;
	const double ScaleHigh = FMath::Max(Entry.MaxScale, Entry.MinScale);
	Out.Instances.Reserve(FMath::Min(Candidates, 4096));
	for (int32 Candidate = 0; Candidate < Candidates; ++Candidate)
	{
		// Every candidate draws the same five numbers in the same order, kept or not.
		FGrassStream Stream(Seed, static_cast<uint32>(Candidate));
		const double DrawX = Stream.NextUnit();
		const double DrawY = Stream.NextUnit();
		const double DrawKeep = Stream.NextUnit();
		const double DrawYaw = Stream.NextUnit();
		const double DrawScale = Stream.NextUnit();

		const double X = Out.Origin.X + DrawX * Side;
		const double Y = Out.Origin.Y + DrawY * Side;
		if (X < 0.0 || Y < 0.0 || X > ExtentX || Y > ExtentY)
		{
			continue;
		}
		// The weight is the chance this candidate stays: 0 keeps none (a draw is never below 0), 1 keeps all (a draw is always below 1).
		const double Weight = Map.SampleChannel(Entry.Channel, X / ExtentX, Y / ExtentY);
		if (!(DrawKeep < Weight))
		{
			continue;
		}

		float Z = 0.0f;
		FVector3f Normal = FVector3f::UpVector;
		if (!SurfaceAt(Field, X, Y, Z, Normal))
		{
			return;
		}

		FQuat Rotation = Entry.bRandomYaw ? FQuat(FVector::UpVector, DrawYaw * UE_DOUBLE_TWO_PI) : FQuat::Identity;
		if (Entry.bAlignToSurface)
		{
			// Turn about the instance's own up first, then stand that up on the surface.
			Rotation = FQuat::FindBetweenNormals(FVector::UpVector, FVector(Normal)) * Rotation;
		}
		const double Scale = FMath::Lerp(ScaleLow, ScaleHigh, DrawScale);
		Out.Instances.Emplace(Rotation, FVector(X - Out.Origin.X, Y - Out.Origin.Y, static_cast<double>(Z)), FVector(Scale));
	}
}

// ---- UMantleTerrainGrassComponent ---------------------------------------------------------------

/** What a worker holds: the immutable inputs, the finished cells it hands back, and whether anyone still wants them. */
struct UMantleTerrainGrassComponent::FShared
{
	TSharedPtr<const FMantleHeightfield> Field;
	TArray<FMantleTerrainGrass> Entries;
	TArray<TSharedPtr<const FMantleTerrainWeights>> Maps;
	std::atomic<bool> bCancelled{ false };
	TQueue<FMantleGrassCellInstances, EQueueMode::Mpsc> Results;
};

UMantleTerrainGrassComponent::UMantleTerrainGrassComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
	bAutoActivate = true;
}

uint64 UMantleTerrainGrassComponent::CellKey(const int32 Entry, const FIntPoint& Coord)
{
	// 16 bits of entry, 24 of each coordinate (a terrain is at most 2049 samples across, so at most that many cells).
	return (static_cast<uint64>(Entry & 0xFFFF) << 48) | (static_cast<uint64>(Coord.X & 0xFFFFFF) << 24) | static_cast<uint64>(Coord.Y & 0xFFFFFF);
}

void UMantleTerrainGrassComponent::Init(TSharedPtr<const FMantleHeightfield> InField, TArray<FMantleTerrainGrass> InEntries,
	TArray<FMantleGrassEntryAssets> InAssets, TArray<TSharedPtr<const FMantleTerrainWeights>> InMaps)
{
	using namespace MantleTerrainGrassPrivate;

	Field = MoveTemp(InField);
	Entries = MoveTemp(InEntries);
	Assets = MoveTemp(InAssets);
	Maps = MoveTemp(InMaps);
	// One of each per entry, whatever the caller passed: an entry without a mesh or map grows nothing.
	Assets.SetNum(Entries.Num());
	Maps.SetNum(Entries.Num());

	CellSizes.Reset(Entries.Num());
	ActiveEntries = 0;
	TSet<const FMantleTerrainWeights*> DistinctMaps;
	for (int32 EntryIndex = 0; EntryIndex < Entries.Num(); ++EntryIndex)
	{
		CellSizes.Add(FMantleTerrainGrassGenerator::CellSizeCm(Entries[EntryIndex]));
		const bool bActive = Assets.IsValidIndex(EntryIndex) && Assets[EntryIndex].Mesh && Maps.IsValidIndex(EntryIndex)
			&& Maps[EntryIndex].IsValid() && Maps[EntryIndex]->HasPixels();
		if (bActive)
		{
			++ActiveEntries;
			DistinctMaps.Add(Maps[EntryIndex].Get());
		}
		else if (Maps.IsValidIndex(EntryIndex))
		{
			// An entry that cannot grow is left out for good; the builder has already said why.
			Maps[EntryIndex].Reset();
		}
	}
	MapCount = DistinctMaps.Num();

	Shared = MakeShared<FShared, ESPMode::ThreadSafe>();
	Shared->Field = Field;
	Shared->Entries = Entries;
	Shared->Maps = Maps;
	AppliedDensityScale = FMath::Clamp(static_cast<double>(CVarGrassDensityScale.GetValueOnGameThread()), 0.0, 2.0);
}

void UMantleTerrainGrassComponent::OnComponentDestroyed(const bool bDestroyingHierarchy)
{
	if (Shared.IsValid())
	{
		Shared->bCancelled.store(true);
	}
	Super::OnComponentDestroyed(bDestroyingHierarchy);
}

bool UMantleTerrainGrassComponent::GetViewLocal(FVector& OutLocal) const
{
	const AActor* Owner = GetOwner();
	const UWorld* World = GetWorld();
	if (!Owner || !World || !GEngine)
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

void UMantleTerrainGrassComponent::TickComponent(const float DeltaTime, const ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	using namespace MantleTerrainGrassPrivate;

	if (!Field.IsValid() || !Shared.IsValid() || ActiveEntries == 0)
	{
		return;
	}

	// The density scale: a change rebuilds everything, and 0 stands none.
	const double Scale = FMath::Clamp(static_cast<double>(CVarGrassDensityScale.GetValueOnGameThread()), 0.0, 2.0);
	if (!FMath::IsNearlyEqual(Scale, AppliedDensityScale))
	{
		ReleaseAll();
		AppliedDensityScale = Scale;
	}

	// What the workers finished: new work waits at most MaxCellsWaiting cells for the game thread.
	FMantleGrassCellInstances Finished;
	while (Shared->Results.Dequeue(Finished))
	{
		--InFlight;
		Ready.Add(MoveTemp(Finished));
	}

	const double Now = FPlatformTime::Seconds();
	if (AppliedDensityScale <= 0.0)
	{
		return;
	}

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
	while (InFlight < MaxCellsInFlight && Ready.Num() + InFlight < MaxCellsWaiting && Wanted.Num() > 0)
	{
		const FWanted Want = Wanted.Pop(EAllowShrinking::No);
		if (!Cells.Contains(Want.Key))
		{
			StartGenerating(Want);
		}
	}

	// Give finished cells components, within the frame's budget. A cell over the budget goes in alone, so none starves.
	const int64 Budget = FMath::Max(CVarGrassMaxInstancesPerFrame.GetValueOnGameThread(), 1);
	int64 Added = 0;
	int32 Components = 0;
	int32 Consumed = 0;
	for (; Consumed < Ready.Num() && Components < MaxComponentsPerFrame; ++Consumed)
	{
		const int64 Count = Ready[Consumed].Instances.Num();
		if (Added > 0 && Added + Count > Budget)
		{
			break;
		}
		Added += Count;
		Components += Count > 0 ? 1 : 0;
		Apply(MoveTemp(Ready[Consumed]));
	}
	Ready.RemoveAt(0, Consumed, EAllowShrinking::No);

	if (!bStartLogged && FirstScanSeconds > 0.0
		&& ((Cells.Num() > 0 && Wanted.IsEmpty() && InFlight == 0 && Ready.IsEmpty()) || Now - FirstScanSeconds > StartLogDeadlineSeconds))
	{
		bStartLogged = true;
		NextLogSeconds = Now + LogIntervalSeconds;
		LogSummary(true);
	}
	else if (bStartLogged && Now >= NextLogSeconds)
	{
		NextLogSeconds = Now + LogIntervalSeconds;
		if (GeneratedSinceLog > 0 || ReleasedSinceLog > 0 || SkippedSinceLog > 0)
		{
			LogSummary(false);
		}
	}
}

void UMantleTerrainGrassComponent::Scan(const FVector& View)
{
	using namespace MantleTerrainGrassPrivate;

	// Release what the view has left, with a cell's width of hysteresis past the cull distance.
	TArray<uint64> ToRelease;
	for (const TPair<uint64, FCell>& Pair : Cells)
	{
		const FCell& Cell = Pair.Value;
		const double Reach = Entries[Cell.Entry].CullEnd + CellSizes[Cell.Entry];
		if (DistanceSquaredToCell(View, Cell.Coord, CellSizes[Cell.Entry]) > Reach * Reach)
		{
			ToRelease.Add(Pair.Key);
		}
	}
	for (const uint64 Key : ToRelease)
	{
		ReleaseCell(Key);
	}

	// A cell the live-instance cap turned away is tried again once there is room for it.
	const int64 MaxLive = FMath::Max(CVarGrassMaxLiveInstances.GetValueOnGameThread(), 1);
	if (LiveInstances * 10 < MaxLive * 9)
	{
		TArray<uint64> Skipped;
		for (const TPair<uint64, FCell>& Pair : Cells)
		{
			if (Pair.Value.State == ECellState::Skipped)
			{
				Skipped.Add(Pair.Key);
			}
		}
		for (const uint64 Key : Skipped)
		{
			Cells.Remove(Key);
		}
	}

	Wanted.Reset();
	for (int32 EntryIndex = 0; EntryIndex < Entries.Num(); ++EntryIndex)
	{
		if (!Maps[EntryIndex].IsValid())
		{
			continue;
		}
		const double Side = CellSizes[EntryIndex];
		const double Reach = Entries[EntryIndex].CullEnd;
		const FIntPoint Count = FMantleTerrainGrassGenerator::CellCount(*Field, Side);
		const int32 FirstX = FMath::Max(0, FMath::FloorToInt32((View.X - Reach) / Side));
		const int32 LastX = FMath::Min(Count.X - 1, FMath::FloorToInt32((View.X + Reach) / Side));
		const int32 FirstY = FMath::Max(0, FMath::FloorToInt32((View.Y - Reach) / Side));
		const int32 LastY = FMath::Min(Count.Y - 1, FMath::FloorToInt32((View.Y + Reach) / Side));
		for (int32 CellY = FirstY; CellY <= LastY; ++CellY)
		{
			for (int32 CellX = FirstX; CellX <= LastX; ++CellX)
			{
				const FIntPoint Coord(CellX, CellY);
				const double DistanceSquared = DistanceSquaredToCell(View, Coord, Side);
				const uint64 Key = CellKey(EntryIndex, Coord);
				if (DistanceSquared > Reach * Reach || Cells.Contains(Key))
				{
					continue;
				}
				FWanted& Want = Wanted.AddDefaulted_GetRef();
				Want.Key = Key;
				Want.Entry = EntryIndex;
				Want.Coord = Coord;
				Want.DistanceSquared = DistanceSquared;
			}
		}
	}
	// Farthest first, so Pop takes the nearest.
	Wanted.Sort([](const FWanted& A, const FWanted& B) { return A.DistanceSquared > B.DistanceSquared; });
}

void UMantleTerrainGrassComponent::StartGenerating(const FWanted& Want)
{
	using namespace MantleTerrainGrassPrivate;

	FCell& Cell = Cells.Add(Want.Key);
	Cell.State = ECellState::Generating;
	Cell.Entry = Want.Entry;
	Cell.Coord = Want.Coord;
	++InFlight;

	const TSharedPtr<FShared, ESPMode::ThreadSafe> Work = Shared;
	const int32 Entry = Want.Entry;
	const FIntPoint Coord = Want.Coord;
	const double Scale = AppliedDensityScale;
	UE::Tasks::Launch(TEXT("MantleGrassCell"), [Work, Entry, Coord, Scale]()
	{
		if (Work->bCancelled.load())
		{
			return;
		}
		FMantleGrassCellInstances Generated;
		FMantleTerrainGrassGenerator::GenerateCell(Work->Entries[Entry], Entry, Coord, *Work->Field, *Work->Maps[Entry], Scale, Generated);
		Work->Results.Enqueue(MoveTemp(Generated));
	}, LowLevelTasks::ETaskPriority::BackgroundNormal);
}

void UMantleTerrainGrassComponent::Apply(FMantleGrassCellInstances&& Generated)
{
	using namespace MantleTerrainGrassPrivate;

	// A result from before the density scale changed, or for a cell released while it was made, is dropped.
	if (!FMath::IsNearlyEqual(Generated.DensityScale, AppliedDensityScale))
	{
		return;
	}
	FCell* Cell = Cells.Find(CellKey(Generated.Entry, Generated.Cell));
	if (!Cell || Cell->State != ECellState::Generating)
	{
		return;
	}
	++GeneratedSinceLog;

	const int32 Count = Generated.Instances.Num();
	if (Count == 0)
	{
		// Nothing grows here: remembered, so it is not asked for again until the view leaves and returns.
		Cell->State = ECellState::Live;
		return;
	}
	const int64 MaxLive = FMath::Max(CVarGrassMaxLiveInstances.GetValueOnGameThread(), 1);
	AActor* Owner = GetOwner();
	USceneComponent* Root = Owner ? Owner->GetRootComponent() : nullptr;
	const FMantleTerrainGrass& Entry = Entries[Generated.Entry];
	const FMantleGrassEntryAssets& Looks = Assets[Generated.Entry];
	if (!Root || !Looks.Mesh || LiveInstances + Count > MaxLive)
	{
		Cell->State = ECellState::Skipped;
		++SkippedSinceLog;
		return;
	}

	UInstancedStaticMeshComponent* Ism = NewObject<UInstancedStaticMeshComponent>(Owner, NAME_None,
		RF_Transient | RF_TextExportTransient | RF_DuplicateTransient);
	Ism->SetupAttachment(Root);
	// As the terrain's own chunks are by now: Static once the place builder has frozen it.
	Ism->SetMobility(Root->GetMobility());
	Ism->SetRelativeLocation(Generated.Origin);
	Ism->SetStaticMesh(Looks.Mesh);
	for (int32 Slot = 0; Slot < Looks.Materials.Num(); ++Slot)
	{
		if (Looks.Materials[Slot])
		{
			Ism->SetMaterial(Slot, Looks.Materials[Slot]);
		}
	}
	// Visual only: no physics bodies, no overlaps, nothing for navigation, never an occluder.
	Ism->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	Ism->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Ism->SetGenerateOverlapEvents(false);
	Ism->SetCanEverAffectNavigation(false);
	Ism->bUseAsOccluder = false;
	Ism->bAffectDistanceFieldLighting = false;
	Ism->SetCastShadow(Entry.bCastShadow);
	// Each instance fades out between the two, and is gone past the end.
	Ism->SetCullDistances(FMath::RoundToInt32(Entry.CullStart), FMath::RoundToInt32(Entry.CullEnd));
	// Instances go in before the component registers, so its render state is built once for all of them.
	Ism->PreAllocateInstancesMemory(Count);
	Ism->AddInstances(Generated.Instances, /*bShouldReturnIndices*/ false, /*bWorldSpace*/ false, /*bUpdateNavigation*/ false);
	Ism->RegisterComponent();
	Owner->AddInstanceComponent(Ism);

	Cell->State = ECellState::Live;
	Cell->Component = Ism;
	Cell->Instances = Count;
	LiveInstances += Count;
	++LiveCells;
}

void UMantleTerrainGrassComponent::ReleaseCell(const uint64 Key)
{
	FCell* Cell = Cells.Find(Key);
	if (!Cell)
	{
		return;
	}
	if (UInstancedStaticMeshComponent* Ism = Cell->Component.Get())
	{
		if (AActor* Owner = GetOwner())
		{
			Owner->RemoveInstanceComponent(Ism);
		}
		Ism->DestroyComponent();
		LiveInstances -= Cell->Instances;
		--LiveCells;
		++ReleasedSinceLog;
	}
	Cells.Remove(Key);
}

void UMantleTerrainGrassComponent::ReleaseAll()
{
	TArray<uint64> Keys;
	Cells.GetKeys(Keys);
	for (const uint64 Key : Keys)
	{
		ReleaseCell(Key);
	}
	Cells.Reset();
	Wanted.Reset();
	Ready.Reset();
	LiveCells = 0;
	LiveInstances = 0;
	// Cells still being made come back with the old scale (or for a cell no longer there) and are dropped.
}

void UMantleTerrainGrassComponent::LogSummary(const bool bStarted)
{
	if (bStarted)
	{
		UE_LOG(LogMantle, Log, TEXT("MANTLE_GRASS entries=%d maps=%d cells=%d instances=%lld"), ActiveEntries, MapCount, LiveCells, LiveInstances);
	}
	else
	{
		UE_LOG(LogMantle, Log, TEXT("MANTLE_GRASS entries=%d maps=%d cells=%d instances=%lld generated=%d released=%d skipped=%d pending=%d"),
			ActiveEntries, MapCount, LiveCells, LiveInstances, GeneratedSinceLog, ReleasedSinceLog, SkippedSinceLog, Wanted.Num() + InFlight + Ready.Num());
	}
	GeneratedSinceLog = 0;
	ReleasedSinceLog = 0;
	SkippedSinceLog = 0;
}
