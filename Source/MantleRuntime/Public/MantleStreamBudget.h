// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/**
 * What a player's machine may hold of a place's cells at once (stage 8 of large places). The numbers are the S0-M2 / S0-M34 measurements on Epic and quarter steps down for the other graphics
 * settings (BP\gates\S8-A.budgets.md has the basis of each). They are limits on loading, never on publishing: a place over them loads less far, and is never refused.
 */
struct MANTLERUNTIME_API FMantleBudgetLimits
{
	/** Every instance of every InstancedMesh in the cells that stand or are being built (about 0.8 KB of RAM and 0.3 KB of video memory each, S0-M34 N11). */
	int64 Instances = MAX_int64;

	/** Solid models plus solid instances in those cells (about 20 to 25 KB of RAM each, S0-M2 and S0-M34 N11). */
	int64 SolidThings = MAX_int64;

	/** Instances standing in far sets (about 1 KB each, S3-B). */
	int64 FarInstances = MAX_int64;

	/**
	 * Nanite root pages the place's meshes have attached (32 KB of video memory each). Only a pressure: over it the cells load less far and give their meshes back. The asset set's own
	 * hard ceiling (FMantleNaniteRootPages::BudgetPages, past which a mesh draws plain) is unchanged, so a mesh is never drawn plain because a lower setting has a lower limit.
	 */
	int64 NaniteRootPages = MAX_int64;

	/** The share of the machine's RAM the game's process may use before it loads less far (0 or less: not checked). */
	double ProcessMemoryShare = 0.0;
};

/** What the cells of a place hold now, in the units of FMantleBudgetLimits. */
struct MANTLERUNTIME_API FMantleBudgetUse
{
	int64 Instances = 0;
	int64 SolidThings = 0;
	int64 FarInstances = 0;
	int64 NaniteRootPages = 0;
	/** The game's process memory over the machine's RAM, 0 to 1 (0 when unknown). */
	double ProcessMemoryShare = 0.0;
};

/** The machine's size, for scaling the counts: bytes of video memory and of RAM; zero or less is unknown (and counts as big enough). */
struct MANTLERUNTIME_API FMantleBudgetMachine
{
	int64 VideoBytes = 0;
	int64 PhysicalBytes = 0;
};

/** How the ladder moves (see FMantleStreamBudget::NextLadder). */
struct MANTLERUNTIME_API FMantleBudgetTuning
{
	/** The ladder is looked at this often, seconds. */
	double StepSeconds = 0.5;
	/** Pressure above this lowers the ladder, below RaiseBelow raises it; between the two it holds. */
	double LowerAbove = 1.0;
	double RaiseBelow = 0.8;
	/** How far one step moves it: 0.1 down (the whole range in 5 s), 0.02 up (25 s). */
	double LowerPerStep = 0.1;
	double RaisePerStep = 0.02;
	/** The real cells' reach never goes below this share of their drawDistance, nor below MinReachCm (or the drawDistance itself, if that is less): the join's 100 m, collision under vehicles and the walls round the player always stand. */
	double MinNearScale = 0.25;
	double MinReachCm = 20000.0;
	/**
	 * Memory the game's process uses comes back slowly (a released cell's actors go at the next garbage collection, about once a minute, and the allocator keeps pages), so over its limit
	 * alone it may lower the ladder only this often: pressing at every look would take the ladder to 0 before anything had been freed.
	 */
	double MemoryLowerEverySeconds = 10.0;
};

/**
 * The live budgets, as plain functions with no state (Mantle.Runtime.StreamBudget.*), and the three readers of the engine they are fed from.
 *
 * Pressure is the highest used / limit. The ladder is one number from 1 (everything loads) down to 0: it falls while the pressure is over 1 and rises slowly once it is
 * under 0.8. The far sets' reach drops first (ladder 1 to 0.5: scale 1 to 0), then the real cells' (ladder 0.5 to 0: scale 1 to MinNearScale).
 */
class MANTLERUNTIME_API FMantleStreamBudget
{
public:
	static constexpr int32 LevelCount = 4;

	/** A graphics setting as the four preset levels: Low 0, Balanced 1, High 2, Epic 3 (the lowest of the engine's quality categories; Cinematic counts as Epic). */
	static int32 ClampLevel(int32 Level);

	/** The smaller of (video memory / 16 GB) and (RAM / 32 GB) (what the numbers were measured on), at least 0.5 and at most 1. An unknown size counts as big enough. */
	static double MachineFactor(const FMantleBudgetMachine& Machine);

	/** The limits for a preset level, counts and Nanite pages times Factor. */
	static FMantleBudgetLimits LimitsFor(int32 Level, double Factor);

	/** The highest of used / limit over the counts only (a limit of 0 or less, or none, is not checked); 0 when nothing is limited. */
	static double CountPressure(const FMantleBudgetLimits& Limits, const FMantleBudgetUse& Use);

	/** The process's memory share over the limit for it (0 when either is unknown). */
	static double MemoryPressure(const FMantleBudgetLimits& Limits, const FMantleBudgetUse& Use);

	/** The higher of the two. */
	static double Pressure(const FMantleBudgetLimits& Limits, const FMantleBudgetUse& Use);

	/** The ladder after one step: lowered when Pressure is over Tuning.LowerAbove, raised when under RaiseBelow, else the same. Stays in 0 to 1. */
	static double NextLadder(double Ladder, double Pressure, const FMantleBudgetTuning& Tuning);

	/** The scale of the far sets' reach for a ladder: 1 down to 0 as the ladder goes from 1 to 0.5, and 0 below. */
	static double FarScale(double Ladder);

	/** The scale of the real cells' reach for a ladder: 1 while it is 0.5 or more, then down to Tuning.MinNearScale at 0. */
	static double NearScale(double Ladder, const FMantleBudgetTuning& Tuning);

	/** How far a cell is wanted from, cm: DrawDistance x Scale, but never less than FloorCm and never more than DrawDistance. With scale 1 it is the DrawDistance. */
	static double ScaledReach(double DrawDistanceCm, double Scale, double FloorCm);

	// ---- Engine readers (game thread) ----

	/** The graphics setting now: the lowest of the engine's ten quality levels, as a preset level. Epic when the engine cannot say. */
	static int32 CurrentLevel();

	/** The card's and the machine's size now (zero where the engine has no answer: a process with no RHI, a server). */
	static FMantleBudgetMachine CurrentMachine();

	/** The limits for the graphics setting and the machine now. The Nanite page limit can be asked for often: the machine is read once a second at most. */
	static FMantleBudgetLimits CurrentLimits();

	/** The game's process memory over the machine's RAM, 0 when unknown. */
	static double CurrentProcessMemoryShare();
};
