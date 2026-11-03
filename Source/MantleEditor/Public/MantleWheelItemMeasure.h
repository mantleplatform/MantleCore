// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class UStaticMesh;

/** What measuring a wheel item's mesh found (vehicle-customization-plan.md section 3, "The wheel item"). */
struct MANTLEEDITOR_API FMantleWheelItemMeasure
{
	bool bOk = false;

	/** cm: how far the rim reaches from its axle (the outer edge), and its length along the axle. The item records these; the game scales the rim by car radius / ReferenceRadius. */
	double ReferenceRadius = 0.0;
	double ReferenceWidth = 0.0;

	/** cm: where the middle of the rim sits along the axle, and how far the axle runs from the origin sideways (both want to be 0: the rim is centered on its origin). */
	double AxialCenter = 0.0;
	double AxleOffset = 0.0;

	/** Why it could not be used, when it could not, in plain words with what to do. */
	FString Problem;
};

/**
 * Measures a wheel item: one static mesh modeled at any size with its axle along +X and its face toward +X, centered on its origin. The
 * radius is the farthest a vertex is from the X axis, the width the mesh's length along X. A wheel that is on its side, off the axle or off-center is refused
 * with what to do (Problem). Which side is the face is not guessed: a deep-dish rim cannot be told from its back by its shape.
 */
struct MANTLEEDITOR_API FMantleWheelItem
{
	/** Fewest vertices a wheel item may measure from. */
	static constexpr int32 MinVertices = 8;

	static FMantleWheelItemMeasure Measure(TConstArrayView<FVector3f> Positions);

	/** Measure over Mesh's first level of detail, in the mesh's own unscaled space. */
	static FMantleWheelItemMeasure MeasureMesh(const UStaticMesh& Mesh);
};
