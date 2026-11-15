// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "MantlePlaceBuilder.h"

/**
 * Builds a place a few entities per frame instead of all in one.
 *
 * It owns a FMantlePlaceBuildSession and gives it a slice of game-thread time on every engine tick (the
 * engine's core ticker, so it runs on a server, a client and in a loading screen alike) until the build is
 * finished, then hands the result to the caller once. A big place used to be one frame of many seconds; this
 * is the same build in frames of at most about FrameBudgetSeconds plus the cost of the one entity that crosses it.
 *
 * Destroying the runner (or Cancel) before it finishes stops the build and removes the actors it made so far,
 * and the callback never runs: a newer place replaces an older one still building by dropping its runner.
 */
class MANTLERUNTIME_API FMantlePlaceBuildRunner : public TSharedFromThis<FMantlePlaceBuildRunner>
{
public:
	using FOnBuilt = TFunction<void(FMantleBuildResult&& Result)>;

	/** Game-thread time a frame gives the build: a quarter of a 60 frames-a-second frame. At least one entity is built per frame. */
	static constexpr double FrameBudgetSeconds = 0.004;

	/**
	 * Starts building Place into World on the next engine tick. OnBuilt runs once on the game thread, from a tick, with
	 * the finished result (its Errors say what could not be built), unless the runner is cancelled or the world goes first.
	 */
	static TSharedRef<FMantlePlaceBuildRunner> Start(
		UWorld* World,
		TSharedRef<const FMantlePlace> Place,
		const FMantleBuildOptions& Options,
		FOnBuilt OnBuilt,
		double InFrameBudgetSeconds = FrameBudgetSeconds);

	~FMantlePlaceBuildRunner();

	/** Stops the build and destroys what it has made; does nothing once it finished. */
	void Cancel();

	bool IsRunning() const { return Session.IsValid() && TickerHandle.IsValid(); }

	/** How many entities are still to spawn, for a progress display. Zero when finished or cancelled. */
	int32 EntitiesLeftToSpawn() const { return Session.IsValid() ? Session->EntitiesLeftToSpawn() : 0; }

	/** The entities in the place being built, and the frames the build has had so far: a join screen counts a tick as progress. */
	int32 GetTotalEntities() const { return TotalEntities; }
	int32 GetTickCount() const { return Ticks; }

private:
	FMantlePlaceBuildRunner() = default;

	bool Tick(float DeltaSeconds);

	/** What the frames the build ran in cost, for the one MANTLE_PLACE_BUILD_FRAMES line a finished build logs. */
	FString PlaceId;
	double StartSeconds = 0.0;
	/** The engine's own time between two ticks while this build ran: the frames a player would have seen. The first tick has no frame before it. */
	double WorstFrameSeconds = 0.0;
	int32 Ticks = 0;
	int32 TotalEntities = 0;
	int32 FramesOver50Ms = 0;
	int32 FramesOver100Ms = 0;
	int32 SlowFramesLogged = 0;
	double LastStepSeconds = 0.0;

	TWeakObjectPtr<UWorld> World;
	TSharedPtr<FMantlePlaceBuildSession> Session;
	FOnBuilt OnBuilt;
	double FrameBudget = FrameBudgetSeconds;
	FTSTicker::FDelegateHandle TickerHandle;
};
