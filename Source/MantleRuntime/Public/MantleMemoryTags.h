// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "HAL/LowLevelMemTracker.h"

/**
 * Mantle's own tags for Unreal's low-level memory tracker (LLM, -llm on the command line; server plan step 4), so a memory breakdown splits what Mantle holds
 * instead of showing it all as the engine's "EngineMisc". Each is put on the code that allocates the data (an LLM_SCOPE_BYTAG), on whichever thread does it:
 * memory is counted where it was allocated, so a place moved from one holder to another stays under the tag it was read under.
 *
 *   Mantle_PlaceParse    a place's JSON read into FMantlePlace (FMantlePlaceParser), the root of a large place included
 *   Mantle_PlaceCells    a large place's cell files read and merged into the place (FMantleCellLoader's reads, FMantlePlaceCells::Merge)
 *   Mantle_PlaceAssets   uploaded assets decoded and built (FMantlePlaceAssets: meshes and their collision cook inputs, materials, textures)
 *   Mantle_PlaceBuild    the place's actors and components as they are built (FMantlePlaceBuildSession); physics keeps its own engine tags inside it
 *   Mantle_ScriptWorld   the script world's records of the place's entities (UMantleScriptWorldSubsystem's seeding)
 *
 * Compiled out with the tracker (Shipping, or ENABLE_LOW_LEVEL_MEM_TRACKER off).
 */
LLM_DECLARE_TAG_API(Mantle_PlaceParse, MANTLERUNTIME_API);
LLM_DECLARE_TAG_API(Mantle_PlaceCells, MANTLERUNTIME_API);
LLM_DECLARE_TAG_API(Mantle_PlaceAssets, MANTLERUNTIME_API);
LLM_DECLARE_TAG_API(Mantle_PlaceBuild, MANTLERUNTIME_API);
LLM_DECLARE_TAG_API(Mantle_ScriptWorld, MANTLERUNTIME_API);
