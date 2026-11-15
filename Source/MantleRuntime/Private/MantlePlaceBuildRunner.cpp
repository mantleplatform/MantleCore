// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceBuildRunner.h"

#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "RHI.h"
#include "RenderTimer.h"
#include "MantleRuntime.h"

TSharedRef<FMantlePlaceBuildRunner> FMantlePlaceBuildRunner::Start(
	UWorld* InWorld,
	TSharedRef<const FMantlePlace> Place,
	const FMantleBuildOptions& Options,
	FOnBuilt InOnBuilt,
	const double InFrameBudgetSeconds)
{
	TSharedRef<FMantlePlaceBuildRunner> Runner = MakeShareable(new FMantlePlaceBuildRunner());
	Runner->World = InWorld;
	Runner->Session = FMantlePlaceBuilder::Begin(InWorld, Place, Options);
	Runner->OnBuilt = MoveTemp(InOnBuilt);
	Runner->FrameBudget = InFrameBudgetSeconds;
	Runner->PlaceId = Place->PlaceId;
	Runner->TotalEntities = Place->Entities.Num();
	Runner->StartSeconds = FPlatformTime::Seconds();
	Runner->TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateSP(Runner, &FMantlePlaceBuildRunner::Tick), 0.0f);
	return Runner;
}

FMantlePlaceBuildRunner::~FMantlePlaceBuildRunner()
{
	Cancel();
}

void FMantlePlaceBuildRunner::Cancel()
{
	if (TickerHandle.IsValid())
	{
		FTSTicker::RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}
	if (Session.IsValid())
	{
		// Whatever was built so far goes with the world it was built in. The result holds raw actor pointers, so they are only
		// touched while that world is alive; once it is gone or tearing down its actors went with it (and may be collected).
		const UWorld* Held = World.Get();
		if (Held && !Held->bIsTearingDown)
		{
			FMantlePlaceBuilder::Teardown(Session->GetResult());
		}
		Session.Reset();
	}
	OnBuilt = nullptr;
}

bool FMantlePlaceBuildRunner::Tick(const float DeltaSeconds)
{
	if (!Session.IsValid())
	{
		return false;
	}
	const UWorld* const BuildWorld = World.Get();
	if (!BuildWorld || BuildWorld->bIsTearingDown)
	{
		// The world went away mid-build: nothing to build into, nobody to tell.
		// (Cancel leaves its actors to the world.)
		UE_LOG(LogMantle, Warning, TEXT("A place build was stopped because its world went away"));
		TickerHandle.Reset();
		Cancel();
		return false;
	}
	if (Ticks > 0)
	{
		WorstFrameSeconds = FMath::Max(WorstFrameSeconds, static_cast<double>(DeltaSeconds));
		FramesOver50Ms += DeltaSeconds > 0.05f ? 1 : 0;
		FramesOver100Ms += DeltaSeconds > 0.1f ? 1 : 0;
		// A long frame says where its time went: the step before it, and the engine's own measure of the last frame's threads.
		if (DeltaSeconds > 0.1f && SlowFramesLogged < 25)
		{
			++SlowFramesLogged;
			UE_LOG(LogMantle, Log, TEXT("MANTLE_PLACE_BUILD_SLOW_FRAME place=%s tick=%d frame_ms=%.1f last_step_ms=%.1f game_ms=%.1f render_ms=%.1f gpu_ms=%.1f"),
				*PlaceId, Ticks, DeltaSeconds * 1000.0, LastStepSeconds * 1000.0,
				FPlatformTime::ToMilliseconds(GGameThreadTime), FPlatformTime::ToMilliseconds(GRenderThreadTime),
				FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles()));
		}
	}
	++Ticks;
	const double StepStartSeconds = FPlatformTime::Seconds();
	const bool bStepDone = Session->Step(FrameBudget);
	LastStepSeconds = FPlatformTime::Seconds() - StepStartSeconds;
	if (!bStepDone)
	{
		return true;
	}
	UE_LOG(LogMantle, Log,
		TEXT("MANTLE_PLACE_BUILD_FRAMES place=%s ticks=%d wall_ms=%.1f worst_frame_ms=%.1f frames_over_50ms=%d frames_over_100ms=%d budget_ms=%.1f"),
		*PlaceId, Ticks, (FPlatformTime::Seconds() - StartSeconds) * 1000.0, WorstFrameSeconds * 1000.0, FramesOver50Ms, FramesOver100Ms,
		FrameBudget * 1000.0);

	// Finished: the callback may drop the runner, so keep it alive until it returns and touch nothing of ours afterwards.
	TSharedRef<FMantlePlaceBuildRunner> KeepAlive = AsShared();
	TickerHandle.Reset();
	FMantleBuildResult Result = MoveTemp(Session->GetResult());
	Session.Reset();
	FOnBuilt Done = MoveTemp(OnBuilt);
	OnBuilt = nullptr;
	if (Done)
	{
		Done(MoveTemp(Result));
	}
	return false;
}
