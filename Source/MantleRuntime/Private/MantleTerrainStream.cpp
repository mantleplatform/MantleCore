// Copyright Joshua Eger. All Rights Reserved.
#include "MantleTerrainStream.h"

#include "Async/Async.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformTime.h"
#include "MantleRuntime.h"
#include "Materials/MaterialInterface.h"
#include "PhysicsEngine/BodySetup.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "UObject/UObjectGlobals.h"

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleTerrainStreamPrivate
{
	/** A view that has moved this far from where the last plan was made (terrain space, cm), or a plan this old, plans again. */
	constexpr double ReplanDistanceCm = 300.0;
	constexpr double ReplanSeconds = 0.5;

	/** The whole grid is audited this often (and after every swap). */
	constexpr double AuditSeconds = 1.0;

	/** A released chunk comes down this many frames after it stopped counting as standing, so every body that asks the gate has had a whole frame to stop. */
	constexpr uint64 ReleaseDelayFrames = 2;

	/** Weak, so a stream the world took away is simply gone. */
	TArray<TWeakObjectPtr<UMantleTerrainStreamComponent>> GStreams;

	double ScaleOf(const AActor* Owner)
	{
		return Owner ? FMath::Max(static_cast<double>(Owner->GetActorScale3D().X), 0.01) : 1.0;
	}
}

// ---- The grid and the plan (pure) --------------------------------------------------------------------

FMantleTerrainChunkGrid FMantleTerrainChunkGrid::Make(const FMantleHeightfield& Field, const int32 InChunkQuads)
{
	FMantleTerrainChunkGrid Out;
	Out.ChunkQuads = FMath::Max(InChunkQuads, 1);
	Out.ChunksX = FMantleTerrainBuilder::ChunkCount(Field.Width, Out.ChunkQuads);
	Out.ChunksY = FMantleTerrainBuilder::ChunkCount(Field.Height, Out.ChunkQuads);
	Out.Width = Field.Width;
	Out.Height = Field.Height;
	Out.SpacingX = Field.SpacingX;
	Out.SpacingY = Field.SpacingY;
	return Out;
}

void FMantleTerrainChunkGrid::RectOf(const int32 Index, FVector2D& OutMin, FVector2D& OutMax) const
{
	const FIntPoint Coord = CoordOf(Index);
	int32 FirstX = 0;
	int32 LastX = 0;
	int32 FirstY = 0;
	int32 LastY = 0;
	FMantleTerrainBuilder::ChunkSpan(Width, ChunkQuads, Coord.X, FirstX, LastX);
	FMantleTerrainBuilder::ChunkSpan(Height, ChunkQuads, Coord.Y, FirstY, LastY);
	OutMin = FVector2D(static_cast<double>(FirstX) * SpacingX, static_cast<double>(FirstY) * SpacingY);
	OutMax = FVector2D(static_cast<double>(LastX) * SpacingX, static_cast<double>(LastY) * SpacingY);
}

double FMantleTerrainChunkGrid::DistanceTo(const int32 Index, const FVector2D& Point) const
{
	FVector2D Min;
	FVector2D Max;
	RectOf(Index, Min, Max);
	const double Dx = FMath::Max3(Min.X - Point.X, 0.0, Point.X - Max.X);
	const double Dy = FMath::Max3(Min.Y - Point.Y, 0.0, Point.Y - Max.Y);
	return FMath::Sqrt(Dx * Dx + Dy * Dy);
}

double FMantleTerrainStreamPlan::NearRadiusFor(const double TopLevelDrawDistanceCm)
{
	return FMath::Clamp(TopLevelDrawDistanceCm, MinNearRadiusCm, MaxNearRadiusCm);
}

bool FMantleTerrainStreamPlan::Wants(const double DistanceCm, const bool bNearNow, const double RadiusCm, const double MarginCm)
{
	return RadiusCm > 0.0 && DistanceCm <= (bNearNow ? RadiusCm + FMath::Max(MarginCm, 0.0) : RadiusCm);
}

double FMantleTerrainStreamPlan::NearestDistance(const FMantleTerrainChunkGrid& Grid, const int32 Index, TConstArrayView<FVector2D> Points)
{
	double Nearest = 1.0e30;
	for (const FVector2D& Point : Points)
	{
		Nearest = FMath::Min(Nearest, Grid.DistanceTo(Index, Point));
	}
	return Nearest;
}

void FMantleTerrainStreamPlan::Make(const FMantleTerrainChunkGrid& Grid, TConstArrayView<FVector2D> Points, const double RadiusCm, const double MarginCm,
	TConstArrayView<EMantleTerrainChunkState> States, TArray<int32>& OutBuild, TArray<int32>& OutRelease)
{
	OutBuild.Reset();
	OutRelease.Reset();
	TArray<TPair<double, int32>> Wanted;
	for (int32 Index = 0; Index < Grid.Num() && Index < States.Num(); ++Index)
	{
		const EMantleTerrainChunkState State = States[Index];
		if (State != EMantleTerrainChunkState::Far && State != EMantleTerrainChunkState::Near)
		{
			continue;
		}
		const double Distance = NearestDistance(Grid, Index, Points);
		const bool bWanted = Wants(Distance, State == EMantleTerrainChunkState::Near, RadiusCm, MarginCm);
		if (State == EMantleTerrainChunkState::Far && bWanted)
		{
			Wanted.Emplace(Distance, Index);
		}
		else if (State == EMantleTerrainChunkState::Near && !bWanted)
		{
			OutRelease.Add(Index);
		}
	}
	Wanted.Sort([](const TPair<double, int32>& A, const TPair<double, int32>& B)
	{
		return A.Key != B.Key ? A.Key < B.Key : A.Value < B.Value;
	});
	OutBuild.Reserve(Wanted.Num());
	for (const TPair<double, int32>& Pair : Wanted)
	{
		OutBuild.Add(Pair.Value);
	}
}

FMantleTerrainHold::EChange FMantleTerrainHold::Next(FMantleTerrainHoldState& State, const bool bGroundStands, const double HeldSeconds)
{
	if (bGroundStands)
	{
		State.bGaveUp = false;
		if (State.bHolding)
		{
			State.bHolding = false;
			return EChange::End;
		}
		return EChange::None;
	}
	if (!State.bHolding)
	{
		if (State.bGaveUp)
		{
			return EChange::None;
		}
		State.bHolding = true;
		return EChange::Begin;
	}
	if (HeldSeconds > TimeoutSeconds)
	{
		State.bHolding = false;
		State.bGaveUp = true;
		return EChange::GiveUp;
	}
	return EChange::None;
}

// ---- The component -----------------------------------------------------------------------------------

UMantleTerrainStreamComponent::FViewsResolver UMantleTerrainStreamComponent::ViewsResolver = nullptr;

UMantleTerrainStreamComponent::UMantleTerrainStreamComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
	bAutoActivate = true;
}

void UMantleTerrainStreamComponent::ForEach(const UWorld* World, TFunctionRef<void(UMantleTerrainStreamComponent&)> Visit)
{
	using namespace MantleTerrainStreamPrivate;
	GStreams.RemoveAll([](const TWeakObjectPtr<UMantleTerrainStreamComponent>& Stream) { return !Stream.IsValid(); });
	for (const TWeakObjectPtr<UMantleTerrainStreamComponent>& Stream : GStreams)
	{
		if (UMantleTerrainStreamComponent* Live = Stream.Get(); Live && Live->GetWorld() == World)
		{
			Visit(*Live);
		}
	}
}

void UMantleTerrainStreamComponent::Init(TSharedPtr<const FMantleHeightfield> InField, const FMantleTerrainBuildParams& NearParams, UMaterialInterface* InMaterial,
	const bool bInStatic, const bool bInCastShadow, TArray<TWeakObjectPtr<UStaticMeshComponent>>&& FarComponents)
{
	using namespace MantleTerrainStreamPrivate;
	Field = MoveTemp(InField);
	Params = NearParams;
	Params.bFarOnly = false;
	Params.bCollision = true;
	Params.bRenderLODs = true;
	Material = InMaterial;
	bStatic = bInStatic;
	bCastShadow = bInCastShadow;
	Grid = FMantleTerrainChunkGrid::Make(*Field, Params.ChunkQuads);
	Chunks.Reset();
	Chunks.SetNum(Grid.Num());
	for (int32 Index = 0; Index < Chunks.Num(); ++Index)
	{
		FChunk& Chunk = Chunks[Index];
		if (FarComponents.IsValidIndex(Index) && FarComponents[Index].IsValid())
		{
			Chunk.FarComponent = FarComponents[Index];
			Chunk.bHasFarMesh = true;
		}
		else
		{
			// The far chunk is the coarsest LOD that still has a quad, down to LOD 0 (PrepareChunk), and both use the same hole grid: no far mesh means nothing at LOD 0 either, a
			// chunk lost to holes. There is nothing to build or to stand on.
			Chunk.State = EMantleTerrainChunkState::Empty;
		}
	}
	Stats = FMantleTerrainStreamStats();
	Stats.Chunks = Chunks.Num();
	TerrainName = GetOwner() ? GetOwner()->GetName() : FString();
	GStreams.Add(this);
	int32 FarMeshes = 0;
	for (const FChunk& Chunk : Chunks)
	{
		FarMeshes += Chunk.bHasFarMesh ? 1 : 0;
	}
	UE_LOG(LogMantle, Log, TEXT("MANTLE_TERRAIN_STREAM start terrain=%s chunks=%d x %d far_meshes=%d samples=%d x %d"), *TerrainName, Grid.ChunksX, Grid.ChunksY, FarMeshes,
		Field->Width, Field->Height);
}

void UMantleTerrainStreamComponent::WorldToTerrain(const TArray<FVector>& WorldPoints, TArray<FVector2D>& OutLocal) const
{
	OutLocal.Reset();
	const AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}
	const FTransform Transform = Owner->GetActorTransform();
	for (const FVector& Point : WorldPoints)
	{
		const FVector Local = Transform.InverseTransformPosition(Point);
		OutLocal.Add(FVector2D(Local.X, Local.Y));
	}
}

void UMantleTerrainStreamComponent::TickComponent(const float DeltaTime, const ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (!ViewsResolver)
	{
		return;
	}
	TArray<FVector> Views;
	double Radius = 0.0;
	if (!ViewsResolver(GetWorld(), Views, Radius))
	{
		return;
	}
	StepStream(Views, Radius);
}

void UMantleTerrainStreamComponent::StepStream(const TArray<FVector>& WorldViews, const double NearRadiusCm)
{
	using namespace MantleTerrainStreamPrivate;
	TRACE_CPUPROFILER_EVENT_SCOPE(MantleTerrainStream_Step);
	AActor* Owner = GetOwner();
	if (!Owner || Chunks.Num() == 0 || !Field.IsValid())
	{
		return;
	}
	const double Now = FPlatformTime::Seconds();
	bConfigured = true;
	LastRadiusCm = NearRadiusCm;

	TArray<FVector2D> Local;
	WorldToTerrain(WorldViews, Local);
	const double Radius = NearRadiusCm / ScaleOf(Owner);
	const double Margin = Grid.ChunkSizeCm();

	// What is wanted now, for every chunk (the gate asks, and a finished job is dropped when it is no longer).
	bool bSwapped = false;
	for (int32 Index = 0; Index < Chunks.Num(); ++Index)
	{
		FChunk& Chunk = Chunks[Index];
		if (Chunk.State == EMantleTerrainChunkState::Empty || Chunk.State == EMantleTerrainChunkState::Failed)
		{
			Chunk.bWanted = false;
			continue;
		}
		const bool bNearNow = Chunk.State != EMantleTerrainChunkState::Far;
		Chunk.bWanted = FMantleTerrainStreamPlan::Wants(FMantleTerrainStreamPlan::NearestDistance(Grid, Index, Local), bNearNow, Radius, Margin);
	}

	// Jobs: at most one mesh made and a few chunks swapped in a step.
	int32 Finished = 0;
	int32 Swaps = 0;
	for (int32 Index = 0; Index < Chunks.Num(); ++Index)
	{
		FChunk& Chunk = Chunks[Index];
		if (Chunk.State == EMantleTerrainChunkState::Releasing)
		{
			if (Chunk.bWanted)
			{
				// The view came back before the chunk came down: it stands again.
				Chunk.State = EMantleTerrainChunkState::Near;
				++AreaSerial;
			}
			else if (GFrameCounter >= Chunk.ReleaseAtFrame)
			{
				EndRelease(Index);
				bSwapped = true;
			}
			continue;
		}
		if (Chunk.State != EMantleTerrainChunkState::Building || !Chunk.Job.IsValid())
		{
			continue;
		}
		FJob& Job = *Chunk.Job;
		if (Job.Phase == FJob::EPhase::Preparing && Job.Prepared.IsReady())
		{
			if (!Chunk.bWanted)
			{
				Discard(Index, TEXT("no longer wanted before its mesh was made"));
			}
			else if (Finished < 1)
			{
				FinishPrepared(Index);
				++Finished;
			}
		}
		else if (Job.Phase == FJob::EPhase::Cooking && Job.bCookDone)
		{
			if (!Job.bCookOk)
			{
				Fail(Index, TEXT("the collision cook failed"));
			}
			else if (!Chunk.bWanted)
			{
				Discard(Index, TEXT("no longer wanted before it was swapped in"));
			}
			else if (Swaps < MaxSwapsPerStep)
			{
				SwapIn(Index);
				++Swaps;
				bSwapped = true;
			}
		}
	}

	// The plan: when the views have moved, the radius changed or a while has passed.
	bool bReplan = bPlanDirty || NearRadiusCm != PlannedRadiusCm || Now - LastPlanSeconds >= ReplanSeconds || PlannedViews.Num() != Local.Num();
	if (!bReplan)
	{
		for (int32 View = 0; View < Local.Num(); ++View)
		{
			if (FVector2D::Distance(Local[View], PlannedViews[View]) >= ReplanDistanceCm)
			{
				bReplan = true;
				break;
			}
		}
	}
	if (bReplan)
	{
		TArray<EMantleTerrainChunkState> States;
		States.Reserve(Chunks.Num());
		for (const FChunk& Chunk : Chunks)
		{
			States.Add(Chunk.State);
		}
		TArray<int32> ToBuild;
		TArray<int32> ToRelease;
		FMantleTerrainStreamPlan::Make(Grid, Local, Radius, Margin, States, ToBuild, ToRelease);
		PlannedViews = Local;
		PlannedRadiusCm = NearRadiusCm;
		LastPlanSeconds = Now;
		bPlanDirty = false;
		int32 Released = 0;
		for (const int32 Index : ToRelease)
		{
			if (Released++ >= MaxReleasesPerStep)
			{
				bPlanDirty = true;
				break;
			}
			BeginRelease(Index);
		}
		for (const int32 Index : ToBuild)
		{
			if (BuildingCount >= MaxBuilding)
			{
				bPlanDirty = true;
				break;
			}
			StartBuild(Index);
		}
	}
	else if (BuildingCount < MaxBuilding)
	{
		// Jobs finished and left room: the plan asks again at once for the next nearest.
		for (const FChunk& Chunk : Chunks)
		{
			if (Chunk.State == EMantleTerrainChunkState::Far && Chunk.bWanted)
			{
				bPlanDirty = true;
				break;
			}
		}
	}

	if (bSwapped || Now - LastAuditSeconds >= AuditSeconds)
	{
		LastAuditSeconds = Now;
		Audit();
	}
}

void UMantleTerrainStreamComponent::StartBuild(const int32 Index)
{
	FChunk& Chunk = Chunks[Index];
	check(Chunk.State == EMantleTerrainChunkState::Far);
	const TSharedPtr<const FMantleHeightfield> SharedField = Field;
	const FMantleTerrainBuildParams WorkerParams = Params;
	const FIntPoint Coord = Grid.CoordOf(Index);
	TSharedPtr<FJob> Job = MakeShared<FJob>();
	Job->StartedSeconds = FPlatformTime::Seconds();
	// The descriptions are made off the game thread: they touch only the heightfield, which the job holds.
	Job->Prepared = Async(EAsyncExecution::ThreadPool, [SharedField, WorkerParams, Coord]() -> TUniquePtr<FMantleTerrainPreparedChunk>
	{
		TUniquePtr<FMantleTerrainPreparedChunk> Prepared = MakeUnique<FMantleTerrainPreparedChunk>();
		FMantleTerrainBuilder::PrepareChunk(*SharedField, WorkerParams, Coord.X, Coord.Y, EMantleTerrainChunkDetail::Full, *Prepared);
		return Prepared;
	});
	Chunk.Job = MoveTemp(Job);
	Chunk.State = EMantleTerrainChunkState::Building;
	++BuildingCount;
	++Stats.BuildsStarted;
	++Stats.BuildingNow;
}

void UMantleTerrainStreamComponent::FinishPrepared(const int32 Index)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(MantleTerrainStream_FinishChunk);
	FChunk& Chunk = Chunks[Index];
	FJob& Job = *Chunk.Job;
	TUniquePtr<FMantleTerrainPreparedChunk> Prepared = Job.Prepared.Consume();
	const double Start = FPlatformTime::Seconds();
	if (!Prepared || Prepared->bEmpty)
	{
		// All hole: there is nothing to stand here, and the far chunk has nothing either.
		Chunk.Job.Reset();
		Chunk.State = EMantleTerrainChunkState::Empty;
		--BuildingCount;
		--Stats.BuildingNow;
		++AreaSerial;
		return;
	}
	int64 Vertices = 0;
	bool bEmpty = false;
	FString Error;
	if (!FMantleTerrainBuilder::FinishChunk(Params, MoveTemp(*Prepared), /*bCookCollision*/ false, Vertices, Job.Built, bEmpty, &Error) || !Job.Built.Mesh)
	{
		Fail(Index, Error.IsEmpty() ? FString(TEXT("the mesh could not be made")) : Error);
		return;
	}
	UBodySetup* Body = Job.Built.Mesh->GetBodySetup();
	if (!Body)
	{
		Fail(Index, TEXT("the mesh has no body setup"));
		return;
	}
	CookingMeshes.Add(Index, Job.Built.Mesh);
	Job.Phase = FJob::EPhase::Cooking;
	// The collision is cooked off the game thread; the chunk swaps in when it is done. The callback holds the job weakly: a job that was dropped (a chunk no longer wanted, the
	// world ending) is not kept alive by an engine task that has not run yet.
	const TWeakPtr<FJob> WeakJob = Chunk.Job;
	Body->CreatePhysicsMeshesAsync(FOnAsyncPhysicsCookFinished::CreateLambda([WeakJob](const bool bOk)
	{
		if (const TSharedPtr<FJob> Live = WeakJob.Pin())
		{
			Live->bCookOk = bOk;
			Live->bCookDone = true;
		}
	}));
	const double Seconds = FPlatformTime::Seconds() - Start;
	Stats.FinishSeconds += Seconds;
	Stats.WorstFinishSeconds = FMath::Max(Stats.WorstFinishSeconds, Seconds);
}

void UMantleTerrainStreamComponent::SwapIn(const int32 Index)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(MantleTerrainStream_SwapIn);
	FChunk& Chunk = Chunks[Index];
	AActor* Owner = GetOwner();
	const double Start = FPlatformTime::Seconds();
	FJob& Job = *Chunk.Job;
	UStaticMeshComponent* NearComponent = FMantleTerrainBuilder::AddChunkComponent(Owner, Owner->GetRootComponent(), Job.Built, Material, bStatic, bCastShadow,
		/*bCollision*/ true, /*bFar*/ false, /*bRegister*/ false);
	// One step: the full chunk is registered and the far one is not, with no frame between, so a frame never draws both or neither.
	NearComponent->RegisterComponent();
	if (UStaticMeshComponent* Far = Chunk.FarComponent.Get())
	{
		Far->UnregisterComponent();
	}
	Chunk.NearComponent = NearComponent;
	CookingMeshes.Remove(Index);
	const double BuildSeconds = FPlatformTime::Seconds() - Job.StartedSeconds;
	Chunk.Job.Reset();
	Chunk.State = EMantleTerrainChunkState::Near;
	--BuildingCount;
	--Stats.BuildingNow;
	++Stats.NearNow;
	++Stats.SwapsToNear;
	++AreaSerial;
	const double Seconds = FPlatformTime::Seconds() - Start;
	Stats.SwapInSeconds += Seconds;
	Stats.WorstSwapInSeconds = FMath::Max(Stats.WorstSwapInSeconds, Seconds);
	Stats.LongestBuildSeconds = FMath::Max(Stats.LongestBuildSeconds, BuildSeconds);
	const FIntPoint Coord = Grid.CoordOf(Index);
	UE_LOG(LogMantle, Log, TEXT("MANTLE_TERRAIN_SWAP terrain=%s chunk=%d,%d to=near frame=%llu far_drawn=%d near_drawn=%d swap_ms=%.2f build_ms=%.1f near=%d"),
		*TerrainName, Coord.X, Coord.Y, static_cast<uint64>(GFrameCounter), Chunk.FarComponent.IsValid() && Chunk.FarComponent->IsRegistered() ? 1 : 0,
		NearComponent->IsRegistered() ? 1 : 0, Seconds * 1000.0, BuildSeconds * 1000.0, Stats.NearNow);
}

void UMantleTerrainStreamComponent::BeginRelease(const int32 Index)
{
	FChunk& Chunk = Chunks[Index];
	Chunk.State = EMantleTerrainChunkState::Releasing;
	Chunk.ReleaseAtFrame = GFrameCounter + MantleTerrainStreamPrivate::ReleaseDelayFrames;
	// It stops counting as standing now: a body that asks the collision gate sees this at once and stops simulating before the collision goes.
	++TakeDownSerial;
	++AreaSerial;
}

void UMantleTerrainStreamComponent::EndRelease(const int32 Index)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(MantleTerrainStream_SwapOut);
	FChunk& Chunk = Chunks[Index];
	const double Start = FPlatformTime::Seconds();
	// One step, the other way round: the far chunk is registered again and the full one goes.
	if (UStaticMeshComponent* Far = Chunk.FarComponent.Get())
	{
		Far->RegisterComponent();
	}
	if (UStaticMeshComponent* Full = Chunk.NearComponent.Get())
	{
		Full->DestroyComponent();
	}
	Chunk.NearComponent.Reset();
	Chunk.State = EMantleTerrainChunkState::Far;
	--Stats.NearNow;
	++Stats.SwapsToFar;
	++AreaSerial;
	const double Seconds = FPlatformTime::Seconds() - Start;
	Stats.SwapOutSeconds += Seconds;
	Stats.WorstSwapOutSeconds = FMath::Max(Stats.WorstSwapOutSeconds, Seconds);
	const FIntPoint Coord = Grid.CoordOf(Index);
	UE_LOG(LogMantle, Log, TEXT("MANTLE_TERRAIN_SWAP terrain=%s chunk=%d,%d to=far frame=%llu far_drawn=%d near_drawn=%d swap_ms=%.2f near=%d"),
		*TerrainName, Coord.X, Coord.Y, static_cast<uint64>(GFrameCounter), Chunk.FarComponent.IsValid() && Chunk.FarComponent->IsRegistered() ? 1 : 0, 0, Seconds * 1000.0,
		Stats.NearNow);
}

void UMantleTerrainStreamComponent::Discard(const int32 Index, const TCHAR* Why)
{
	FChunk& Chunk = Chunks[Index];
	AbortCook(Index);
	Chunk.Job.Reset();
	Chunk.State = EMantleTerrainChunkState::Far;
	--BuildingCount;
	--Stats.BuildingNow;
	++Stats.Discarded;
	bPlanDirty = true;
	const FIntPoint Coord = Grid.CoordOf(Index);
	UE_LOG(LogMantle, Verbose, TEXT("MANTLE_TERRAIN_STREAM discarded terrain=%s chunk=%d,%d: %s"), *TerrainName, Coord.X, Coord.Y, Why);
}

void UMantleTerrainStreamComponent::Fail(const int32 Index, const FString& Why)
{
	FChunk& Chunk = Chunks[Index];
	AbortCook(Index);
	const bool bWasBuilding = Chunk.State == EMantleTerrainChunkState::Building;
	Chunk.Job.Reset();
	Chunk.State = EMantleTerrainChunkState::Failed;
	if (bWasBuilding)
	{
		--BuildingCount;
		--Stats.BuildingNow;
	}
	++Stats.FailedNow;
	++AreaSerial;
	const FIntPoint Coord = Grid.CoordOf(Index);
	UE_LOG(LogMantle, Warning, TEXT("MANTLE_TERRAIN_STREAM FAIL terrain=%s chunk=%d,%d: %s (the far chunk stays; nothing stands there)"), *TerrainName, Coord.X, Coord.Y, *Why);
}

void UMantleTerrainStreamComponent::AbortCook(const int32 Index)
{
	// The cook of this chunk's mesh, if one runs: asked to stop (the engine drops its result), and the mesh let go of.
	if (const TObjectPtr<UStaticMesh>* Mesh = CookingMeshes.Find(Index))
	{
		if (*Mesh)
		{
			if (UBodySetup* Body = (*Mesh)->GetBodySetup())
			{
				Body->AbortPhysicsMeshAsyncCreation();
			}
		}
		CookingMeshes.Remove(Index);
	}
}

void UMantleTerrainStreamComponent::CancelAllJobs()
{
	for (int32 Index = 0; Index < Chunks.Num(); ++Index)
	{
		AbortCook(Index);
		Chunks[Index].Job.Reset();
	}
	CookingMeshes.Reset();
	BuildingCount = 0;
	Stats.BuildingNow = 0;
}

bool UMantleTerrainStreamComponent::AreaStands(const TArray<FVector>& WorldPoints, const double RadiusCm) const
{
	if (!bConfigured)
	{
		return false;
	}
	TArray<FVector2D> Local;
	WorldToTerrain(WorldPoints, Local);
	const double Radius = RadiusCm / MantleTerrainStreamPrivate::ScaleOf(GetOwner());
	for (int32 Index = 0; Index < Chunks.Num(); ++Index)
	{
		const EMantleTerrainChunkState State = Chunks[Index].State;
		if (State == EMantleTerrainChunkState::Near || State == EMantleTerrainChunkState::Empty || State == EMantleTerrainChunkState::Failed)
		{
			continue;
		}
		if (FMantleTerrainStreamPlan::NearestDistance(Grid, Index, Local) <= Radius)
		{
			return false;
		}
	}
	return true;
}

void UMantleTerrainStreamComponent::CountArea(const FVector& WorldPoint, const double RadiusCm, int32& OutCells, int32& OutBuilt, int32& OutFailed, int32& OutPending) const
{
	OutCells = 0;
	OutBuilt = 0;
	OutFailed = 0;
	OutPending = 0;
	TArray<FVector2D> Local;
	WorldToTerrain(TArray<FVector>{ WorldPoint }, Local);
	if (Local.Num() == 0)
	{
		return;
	}
	const double Radius = RadiusCm / MantleTerrainStreamPrivate::ScaleOf(GetOwner());
	for (int32 Index = 0; Index < Chunks.Num(); ++Index)
	{
		const FChunk& Chunk = Chunks[Index];
		if (Chunk.State == EMantleTerrainChunkState::Empty || Grid.DistanceTo(Index, Local[0]) > Radius)
		{
			continue;
		}
		++OutCells;
		switch (Chunk.State)
		{
		case EMantleTerrainChunkState::Near:
			++OutBuilt;
			break;
		case EMantleTerrainChunkState::Failed:
			++OutFailed;
			break;
		default:
			// Far, Building or on its way out: it does not stand; a chunk the plan wants is on its way.
			if (Chunk.bWanted || Chunk.State == EMantleTerrainChunkState::Building)
			{
				++OutPending;
			}
			break;
		}
	}
}

FMantleTerrainAudit UMantleTerrainStreamComponent::Audit()
{
	FMantleTerrainAudit Out;
	Out.Chunks = Chunks.Num();
	for (int32 Index = 0; Index < Chunks.Num(); ++Index)
	{
		const FChunk& Chunk = Chunks[Index];
		const bool bFarDrawn = Chunk.FarComponent.IsValid() && Chunk.FarComponent->IsRegistered();
		const bool bFullDrawn = Chunk.NearComponent.IsValid() && Chunk.NearComponent->IsRegistered();
		const bool bShouldDraw = Chunk.bHasFarMesh || Chunk.NearComponent.IsValid() || Chunk.State == EMantleTerrainChunkState::Near || Chunk.State == EMantleTerrainChunkState::Releasing;
		if (!bShouldDraw)
		{
			continue;
		}
		++Out.WithMesh;
		if (bFarDrawn && bFullDrawn)
		{
			++Out.DrawnBoth;
		}
		else if (bFullDrawn)
		{
			++Out.DrawnFull;
		}
		else if (bFarDrawn)
		{
			++Out.DrawnFar;
		}
		else
		{
			++Out.DrawnNeither;
		}
		if (bFarDrawn && bFullDrawn)
		{
			const FIntPoint Coord = Grid.CoordOf(Index);
			UE_LOG(LogMantle, Error, TEXT("MANTLE_TERRAIN_AUDIT_FAIL terrain=%s chunk=%d,%d drawn twice"), *TerrainName, Coord.X, Coord.Y);
		}
		else if (!bFarDrawn && !bFullDrawn)
		{
			const FIntPoint Coord = Grid.CoordOf(Index);
			UE_LOG(LogMantle, Error, TEXT("MANTLE_TERRAIN_AUDIT_FAIL terrain=%s chunk=%d,%d not drawn"), *TerrainName, Coord.X, Coord.Y);
		}
	}
	++Stats.AuditsRun;
	if (!Out.IsClean())
	{
		++Stats.AuditFailures;
	}
	return Out;
}

void UMantleTerrainStreamComponent::LogSummary(const TCHAR* Why) const
{
	UE_LOG(LogMantle, Log, TEXT("MANTLE_TERRAIN_STREAM_SUMMARY %s terrain=%s chunks=%d near_now=%d building_now=%d failed_now=%d builds=%lld swaps_to_near=%lld swaps_to_far=%lld discarded=%lld audits=%lld audit_failures=%lld finish_ms_total=%.1f finish_ms_worst=%.2f swap_in_ms_worst=%.2f swap_out_ms_worst=%.2f longest_build_ms=%.1f"),
		Why, *TerrainName, Stats.Chunks, Stats.NearNow, Stats.BuildingNow, Stats.FailedNow, Stats.BuildsStarted, Stats.SwapsToNear, Stats.SwapsToFar, Stats.Discarded, Stats.AuditsRun,
		Stats.AuditFailures, Stats.FinishSeconds * 1000.0, Stats.WorstFinishSeconds * 1000.0, Stats.WorstSwapInSeconds * 1000.0, Stats.WorstSwapOutSeconds * 1000.0,
		Stats.LongestBuildSeconds * 1000.0);
}

void UMantleTerrainStreamComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// A world that ends (the player leaves the place) lets go of every cook before it is collected.
	CancelAllJobs();
	Super::EndPlay(EndPlayReason);
}

void UMantleTerrainStreamComponent::OnComponentDestroyed(const bool bDestroyingHierarchy)
{
	CancelAllJobs();
	Super::OnComponentDestroyed(bDestroyingHierarchy);
}

void UMantleTerrainStreamComponent::BeginDestroy()
{
	if (Chunks.Num() > 0)
	{
		LogSummary(TEXT("destroyed"));
		CancelAllJobs();
	}
	Super::BeginDestroy();
}
