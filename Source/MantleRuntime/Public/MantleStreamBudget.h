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

	/**
	 * #189: the video memory the place's textures may hold (FMantlePlaceAssets::GetLiveTextureBytes) before the cells load less far: a share of the card by graphics
	 * setting (TextureShareOfVideo). City Sample's spawn wanted 23 GB of them on a 16 GB card.
	 */
	int64 TextureBytes = MAX_int64;

	/**
	 * #189: the game's process memory, in bytes, beside the share: the smaller of the two decides. 9 GB on Joshua's minimum PC (16 GB of RAM), 17 GB on 32 GB, 24 GB on 64 GB
	 * (ProcessMemoryCapBytes). The share alone let City Sample take 44 GB of a 63 GB machine, and would leave a 16 GB one too little for the system and other programs.
	 */
	int64 ProcessMemoryBytes = MAX_int64;

	/**
	 * #189: the card as the driver sees it. The textures' limit is a fixed share of the card, and avatars, vehicles, weapons, Lumen, shadows and other programs use the rest with nothing
	 * watching. So what the driver says is in use (FMantleBudgetUse::VideoUsedBytes) counts against this share of the budget it gives the game (VideoBudgetBytes): at 90% the pressure
	 * is 1, a cushion before the driver starts moving memory out to RAM (City Sample's freeze: 13.5 GB on a 16 GB card, 1.9 GB more spilled).
	 */
	double VideoUseShare = 0.9;
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
	/** The place's textures' video memory, bytes. */
	int64 TextureBytes = 0;
	/** The game's process memory, bytes (FMantleStreamBudget::CurrentProcessMemoryBytes: the larger of working set and private bytes); 0 when unknown. */
	int64 ProcessMemoryBytes = 0;
	/** #189: the video memory the driver says the game uses now, and the budget it gives the game (FMantleStreamBudget::CurrentVideoMemory); 0 when unknown. */
	int64 VideoUsedBytes = 0;
	int64 VideoBudgetBytes = 0;
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
	/**
	 * #189: below the ladder. Textures still over their limit at ladder 0 (the real cells already at their shortest reach) shorten how near full texture detail
	 * reaches (FMantlePlaceAssets::SetTextureDetailReach): x TextureDetailStep at most every TextureDetailLowerEverySeconds, from TextureDetailMaxCm (150 m) down to
	 * TextureDetailMinCm. Past it the 512 copy draws. At 25 m on a 1080p screen with a 90 degree view a metre is about 38 pixels, and a 512 copy spanning even 4 m gives it
	 * 128 texels: nothing past the floor can show more than the 512 copy does, and the full files left are the few things a player is next to. It comes back the same way,
	 * a step at most every TextureDetailRaiseEverySeconds (textures take a while to build, so their bytes lag), once the pressure is under RaiseBelow; the ladder rises only
	 * after the reach is whole again (sharpness first, then reach: the order it went down in, reversed).
	 */
	double TextureDetailMaxCm = 15000.0;
	double TextureDetailMinCm = 2500.0;
	double TextureDetailStep = 0.75;
	double TextureDetailLowerEverySeconds = 1.0;
	double TextureDetailRaiseEverySeconds = 5.0;
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

	/** LimitsFor, plus the limits that come from the machine's own size: the textures' share of its card (times Scale) and the process memory cap. */
	static FMantleBudgetLimits LimitsFor(int32 Level, const FMantleBudgetMachine& Machine, double Scale);

	/** The share of the card the place's textures may hold at a preset level: Low 0.25, Balanced 0.33, High 0.42, Epic 0.5 (8 GB of a 16 GB card on Epic, about 2 GB of a 6 to 8 GB one on Low or Balanced). */
	static double TextureShareOfVideo(int32 Level);

	/**
	 * The process memory cap for a machine with PhysicalBytes of RAM: 9 GB at 16 GB, 17 GB at 32 GB, 24 GB at 64 GB, in a straight line between those, in proportion below 16 GB,
	 * and on at 64 GB's slope above it. MAX_int64 when the RAM is unknown.
	 */
	static int64 ProcessMemoryCapBytes(int64 PhysicalBytes);

	/** The highest of used / limit over the counts only, the textures' bytes included (a limit of 0 or less, or none, is not checked); 0 when nothing is limited. */
	static double CountPressure(const FMantleBudgetLimits& Limits, const FMantleBudgetUse& Use);

	/** The process's memory over the limits for it: the higher of share / share limit and bytes / byte cap (each 0 when either side is unknown). */
	static double MemoryPressure(const FMantleBudgetLimits& Limits, const FMantleBudgetUse& Use);

	/** #189: the card's use over its cushioned budget: VideoUsedBytes / (VideoBudgetBytes x VideoUseShare); 0 when either is unknown. Comes back slowly, like the process's memory. */
	static double VideoPressure(const FMantleBudgetLimits& Limits, const FMantleBudgetUse& Use);

	/** The highest of the three. */
	static double Pressure(const FMantleBudgetLimits& Limits, const FMantleBudgetUse& Use);

	/** The ladder after one step: lowered when Pressure is over Tuning.LowerAbove, raised when under RaiseBelow, else the same. Stays in 0 to 1. */
	static double NextLadder(double Ladder, double Pressure, const FMantleBudgetTuning& Tuning);

	/** The scale of the far sets' reach for a ladder: 1 down to 0 as the ladder goes from 1 to 0.5, and 0 below. */
	static double FarScale(double Ladder);

	/** The scale of the real cells' reach for a ladder: 1 while it is 0.5 or more, then down to Tuning.MinNearScale at 0. */
	static double NearScale(double Ladder, const FMantleBudgetTuning& Tuning);

	/**
	 * #189: the full-texture-detail reach after one step (see FMantleBudgetTuning::TextureDetailMaxCm): shorter by TextureDetailStep when the ladder is at 0 and the textures'
	 * pressure (texture bytes / their limit) or the card's (VideoPressure) is over LowerAbove; longer by the same step (to the max) when Pressure (everything) is under RaiseBelow
	 * and both would still fit after it (the circle's area grown by 1 / Step^2: the textures' pressure that much higher, and the card's higher by what the textures would add,
	 * TexturesOfVideo being their bytes over the card's cushioned budget); else the same.
	 */
	static double NextTextureDetailReach(double ReachCm, double Ladder, double TexturePressure, double VideoPressure, double TexturesOfVideo, double Pressure,
		const FMantleBudgetTuning& Tuning);

	/** How far a cell is wanted from, cm: DrawDistance x Scale, but never less than FloorCm and never more than DrawDistance. With scale 1 it is the DrawDistance. */
	static double ScaledReach(double DrawDistanceCm, double Scale, double FloorCm);

	// ---- Engine readers (game thread) ----

	/** The graphics setting now: the lowest of the engine's ten quality levels, as a preset level. Epic when the engine cannot say. */
	static int32 CurrentLevel();

	/**
	 * The card's and the machine's size now (zero where the engine has no answer: a process with no RHI, a server). A test switch pretends to be another machine:
	 * -MantleBudgetRamGB=16 -MantleBudgetVideoGB=8 is Joshua's minimum PC (either alone replaces only its own half).
	 */
	static FMantleBudgetMachine CurrentMachine();

	/** The limits for the graphics setting and the machine now. The Nanite page limit can be asked for often: the machine is read once a second at most. */
	static FMantleBudgetLimits CurrentLimits();

	/** The game's process memory (CurrentProcessMemoryBytes) over the machine's RAM (CurrentMachine's, so the test switch counts), 0 when unknown. */
	static double CurrentProcessMemoryShare();

	/** The game's process memory, bytes: the larger of its working set and its private (committed) bytes (Windows trims the first while the second grows). */
	static int64 CurrentProcessMemoryBytes();

	/**
	 * #189: the video memory the driver says the game uses, and the budget it gives the game (RHIGetMemoryStats: DXGI's QueryVideoMemoryInfo on D3D12 and D3D11, Vulkan's memory
	 * budget), both 0 when the RHI cannot say. -MantleBudgetVideoGB caps the budget, so a run on a big card behaves as a small one would.
	 */
	static void CurrentVideoMemory(int64& OutUsedBytes, int64& OutBudgetBytes);

	/** #189: the smallest pool the streamed textures are given, whatever the whole textures take of the share. */
	static constexpr int64 MinTexturePoolBytes = 256ll * 1024 * 1024;

	/**
	 * #189: the engine's texture pool, which holds the top mips of the textures it streams (FMantleRuntimeTextureBuilder::BuildStreamed) and drops the least needed ones
	 * when full: what the textures' share of the card (FMantleBudgetLimits::TextureBytes) leaves after the textures counted in TextureBytesHeld (built whole, and the
	 * streamed ones' resident tails), in whole 64 MB, at least MinTexturePoolBytes. Without it each graphics setting fixes the pool at 400 to 1000 MB (BaseScalability.ini,
	 * r.Streaming.PoolSize), whatever the card. Pure.
	 */
	static int64 TexturePoolBytes(const FMantleBudgetLimits& Limits, int64 TextureBytesHeld);

	/** #189: how much the pool may grow back in one budget look once the card is under its cushion by at least as much. */
	static constexpr int64 TexturePoolGrowBytes = 256ll * 1024 * 1024;

	/**
	 * #189: the pool at the next budget look. Current is the pool now (0: not set yet), Wanted what the share leaves (TexturePoolBytes), OverBytes how far the
	 * driver's use is over the card's cushion (negative: under it). Over the cushion the pool shrinks by what is over, to MinTexturePoolBytes at least: it gives
	 * way before the ladder and the full-detail radius do. Under it by TexturePoolGrowBytes or more it grows back by that much, never past Wanted; in between it
	 * holds. Never more than Wanted. Whole 64 MB. Pure.
	 */
	static int64 NextTexturePool(int64 Current, int64 Wanted, int64 OverBytes);

	/** Sets r.Streaming.PoolSize to Bytes (in MB, as code, over the graphics setting's value) when it differs from what this last set. Game thread. */
	static void ApplyTexturePool(int64 Bytes);
};
