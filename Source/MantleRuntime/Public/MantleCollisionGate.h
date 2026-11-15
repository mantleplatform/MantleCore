// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class ACharacter;
class UWorld;

/**
 * The collision gate (stage 2 of large places): a player's machine simulates a body only while the solid cells under it are built there.
 *
 * A client builds the scenery around the player and takes it down behind (cells, by distance), so the ground under a car 400 m away, a physics prop that sleeps
 * on a rooftop, or another player's character 150 m off may not exist on that machine. A body that is simulated there without ground falls out of the world
 * (and nothing brings it back: the server's body sleeps and sends nothing). So while the solid cells around a body are not built, that copy does not simulate: it
 * takes the server's pose, which is the truth, and simulates again once the ground stands. The server (which merges every cell) and a standalone game (whose solid
 * streamer keeps every solid thing) never close the gate.
 *
 * Each body that is simulated locally holds one of these and asks it every frame (Update). One question beside the streamer (FMantleCellArea) serves every body type.
 * Log: MANTLE_COL_GATE (once per change of one body) and MANTLE_COL_GATE_STATS (once a second: bodies, how many are closed, what asking cost).
 */
class MANTLERUNTIME_API FMantleCollisionGate
{
public:
	FMantleCollisionGate() = default;
	~FMantleCollisionGate();
	UE_NONCOPYABLE(FMantleCollisionGate);

	/**
	 * The cells around a body are asked about at most this often, and sooner when it has moved ReaskDistanceCm since the last ask, and at once when the streamer
	 * hands a cell to its take-down or one fails (FMantleCellArea::TakeDownSerial): the gate closes in the frame the ground is released.
	 */
	static constexpr double AskIntervalSeconds = 0.1;
	static constexpr double ReaskDistanceCm = 200.0;

	/** The gate stays closed this long after the ground is back, so a body at the edge of a cell does not flap between the two. */
	static constexpr double ReopenSeconds = 0.5;

	/** Cells this far past the body's own size count as under it. */
	static constexpr double MarginCm = 500.0;

	/** And as far again as the body travels in this long (a moving body reaches ground that is not under it yet). */
	static constexpr double LookaheadSeconds = 0.5;

	/** How far from a body's center the solid cells count: its own radius, the margin, and half a second of travel at SpeedCmPerSecond. */
	static double RadiusCm(double BodyRadiusCm, double SpeedCmPerSecond);

	/**
	 * The gate's rule, pure. It closes at once when the ground does not stand; it opens when the ground has stood for ReopenSeconds in a row (StandingSince is the
	 * time the ground first stood in this run of asks, or negative; the function keeps it). Returns the new state.
	 */
	static bool NextClosed(bool bClosed, bool bGroundStands, double NowSeconds, double& InOutStandingSince);

	/**
	 * Asks about the cells around Point (a body of BodyRadiusCm moving at SpeedCmPerSecond), at most every AskIntervalSeconds, and returns true when this
	 * changed the gate (the body then stops simulating or starts again: IsClosed says which). Called only for a body another machine owns (a client's copy); with no
	 * streamer in the world (a place with no cells) the ground always stands. Kind and Name are for the log.
	 */
	bool Update(UWorld* World, const FVector& Point, double BodyRadiusCm, double SpeedCmPerSecond, const TCHAR* Kind, const FString& Name);

	/** The same as Update (it is kept for the tests that name it). */
	bool UpdateWhenClient(UWorld* World, const FVector& Point, double BodyRadiusCm, double SpeedCmPerSecond, const TCHAR* Kind, const FString& Name);

	bool IsClosed() const { return bClosed; }

	/**
	 * For a character that another machine owns (a simulated proxy: another player, an NPC): asks the gate for where it stands and, while closed, turns off its gravity
	 * (the engine's own flag for a simulated character that must not fall: it stays where the server's updates put it). Called every tick; does nothing for any
	 * other role. The flag is given back when the gate opens.
	 */
	void UpdateSimulatedCharacter(ACharacter& Character);

private:
	/** UpdateSimulatedCharacter set ACharacter::bSimGravityDisabled (so it clears it, and only then). */
	bool bOwnsSimGravityFlag = false;
	bool bClosed = false;
	/** The body was counted in the stats (so its end counts it out). */
	bool bCounted = false;
	double StandingSince = -1.0;
	double LastAskSeconds = -1.0e9;
	uint64 LastTakeDownSerial = 0;
	FVector LastAskPoint = FVector::ZeroVector;
	bool bLastGroundStands = true;
};
