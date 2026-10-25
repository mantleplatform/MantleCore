// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "Engine/EngineTypes.h"

/**
 * The hover trace channel, defined once (#821, #820). DefaultEngine.ini of the client project names ECC_GameTraceChannel5 "Hover",
 * blocking by default; a hovercraft's pads trace it (MantleGameplay) and a place's water surface answers Block to it
 * (MantlePlaceBuilder), so the two ends must name the same channel. The place builder lives in the creator plugin, which cannot
 * include the client's MantleCollisionChannels.h, so the definition lives here and that header refers to this one. A creator
 * project that uses trace channel 5 for something of its own would see a water part block it: name this channel in the project's
 * Collision settings (as "Hover") or move the definition, which is the one line below.
 */
#define Mantle_TraceChannel_Hover ECC_GameTraceChannel5
