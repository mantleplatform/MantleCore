// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/**
 * The colours the plugin's viewport gizmos share, so every authoring marker reads the same way: the overlay placement
 * (#54) and a Mantle Seat's hip marker use these.
 */
namespace MantleGizmoStyle
{
	/** The reach or volume a gizmo covers (a wire box). */
	inline const FLinearColor Reach(0.2f, 0.7f, 1.f);
	/** An outline where the gizmo meets a surface, and a measured distance. */
	inline const FLinearColor Outline(1.f, 0.9f, 0.1f);
	/** Which way it looks or faces (an arrow). */
	inline const FLinearColor Facing(1.f, 0.5f, 0.05f);
	/** Which way is up. */
	inline const FLinearColor Up(0.1f, 0.85f, 0.2f);
}
