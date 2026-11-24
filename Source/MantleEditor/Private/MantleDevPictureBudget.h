// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/**
 * A safety net for the maintainer picture commands (Mantle.Dev.RenderAccessory, RenderLayers, RenderHairRule), on top of the
 * capture freeing what each picture made (FMantleCosmeticThumbnail::Capture). A run of pictures with no engine tick between them
 * once kept every picture's renderer scene: 31 pictures passed the 256 GB reserved-memory budget and the PC froze (2026-11-24).
 * So one editor launch takes at most MaxPicturesPerLaunch pictures, and stops early if the reserved GPU address space grows by
 * MaxReservedGrowthGB from where the launch's first picture found it. Past either, every later picture command stops with a
 * MANTLE_PICTURE_CAP line (what it has not drawn), and the runner relaunches the editor for the rest. Each picture logs
 * MANTLE_PICTURE_MEMORY before and after: the reserved virtual size (GRHIGlobals.ReservedResources.VirtualSize, the total the
 * RHI's budget warning checks) and the process's physical memory. Maintainer commands only: nothing a creator runs is capped.
 * Game thread only.
 */
struct FMantleDevPictureBudget
{
	/** Pictures one launch takes. The freeze came at 31 (and the PC locked up near 51); well under that, and over the 10 a proof needs. */
	static constexpr int32 MaxPicturesPerLaunch = 16;

	/** Growth of the reserved GPU address space from the launch's first picture, GB. About 8 GB a picture when it leaked. */
	static constexpr int64 MaxReservedGrowthGB = 48;

	/**
	 * Before a picture: false (logging MANTLE_PICTURE_CAP with Command and Picture, the one not drawn) when this launch is at
	 * the cap or the reserved memory has grown too far; else logs MANTLE_PICTURE_MEMORY "before" and true.
	 */
	static bool Begin(const TCHAR* Command, const FString& Picture);

	/** After a picture Begin allowed (drawn or not): counts it and logs MANTLE_PICTURE_MEMORY "after". */
	static void End(const TCHAR* Command, const FString& Picture);

	/**
	 * Whether this launch may still take pictures: false (logging MANTLE_PICTURE_CAP with Command and What, what is not drawn)
	 * once it is at the cap or the reserved memory has grown too far. A command checks it before making a body it would not draw.
	 */
	static bool Allows(const TCHAR* Command, const FString& What);

	/**
	 * At the end of a picture command, with nothing of its own left alive: a full collection, so its bodies, meshes, worlds and
	 * render targets go before the next command (a -ExecCmds batch never ticks, so the editor's own collection never runs).
	 */
	static void CollectAfterCommand(const TCHAR* Command);
};
