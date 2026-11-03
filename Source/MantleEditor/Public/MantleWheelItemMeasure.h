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

	/** +1: the face (the side the spokes sit toward) is toward +X, as a wheel item wants; -1: toward -X, so the rim would be fitted back to front; 0: cannot tell (a flat face). */
	int32 FaceSign = 0;

	/** What the creator should know, in plain words: a thing to fix. */
	TArray<FString> Notes;

	/** Why it could not be measured, when it could not. */
	FString Problem;
};

/**
 * Measures a wheel item: one static mesh modeled at any size with its axle along +X and its face toward +X, centered on its origin. The
 * radius is the farthest a vertex is from the X axis, the width the mesh's length along X. FaceSign reads which side the middle of the rim
 * (inside half the radius) sits toward, since a rim's spokes stand toward the face.
 */
struct MANTLEEDITOR_API FMantleWheelItem
{
	/** Fewest vertices a wheel item may measure from. */
	static constexpr int32 MinVertices = 8;

	/** The share of a rim's width the middle of the rim must sit off-center for the face to be read. */
	static constexpr double FaceShare = 0.05;

	static FMantleWheelItemMeasure Measure(TConstArrayView<FVector3f> Positions);

	/** Measure over Mesh's first level of detail, in the mesh's own unscaled space. */
	static FMantleWheelItemMeasure MeasureMesh(const UStaticMesh& Mesh);
};
