// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

struct FMantleGlbJoint;
class UMantleVehicleSource;

/**
 * The attached parts' collision triangles of a vehicle upload. A car split into a body and parts (FMantleVehicleBodySplit: a model with more materials than a body
 * may draw) has most of its surface in its parts, and a dedicated server loads no part meshes, so the upload carries those triangles itself (the glTF root's
 * extras.mantleVehicleCollision, backend cook vehicle_collision.go; MVH1 version 2). The client takes the body's own triangles from its LOD0 as it always has and adds
 * these to make the shape characters, cameras and weapon traces meet.
 *
 * Each part's LOD0 triangles (the ones its upload carries), in the body's component space at the reference pose: the part's transform on its bone, then the bone's
 * rest pose. A part on a wheel bone is left out (a wheel moves, spins and steers, as the body's wheel triangles are left out). A part on a door, the trunk lid or the
 * steering wheel sits at its rest pose, as the body's own triangles on those bones do. Vertices are welded, triangles with no area dropped.
 */
struct MANTLEEDITOR_API FMantleVehicleCollision
{
	/** Unreal space, centimeters, the body's component space (the writer converts to glTF's axes and meters like every position). */
	TArray<FVector3f> Positions;
	TArray<uint32> Indices;

	int32 NumTriangles() const { return Indices.Num() / 3; }
	bool IsEmpty() const { return Indices.Num() == 0; }

	/**
	 * Adds one part: MeshPositions and MeshIndices (whole triangles) in the part mesh's own space, placed by ToComponent. A vertex within a thousandth of a
	 * centimeter of one already here is the same vertex; a triangle that is not three vertices or has no area is dropped. Returns the triangles added.
	 */
	int32 AddTriangles(TConstArrayView<FVector3f> MeshPositions, TConstArrayView<uint32> MeshIndices, const FTransform& ToComponent);

	/** The rest pose of the joint named Bone (any case) in the component space of Joints (parent-first, local transforms, as the upload writes them). False when none. */
	static bool BoneRestPose(TConstArrayView<FMantleGlbJoint> Joints, FName Bone, FTransform& OutComponent);

	/**
	 * Every part of Source (its Size already applied: the export view, so the placement is the one the upload's document carries) on a body with Joints, off the wheel
	 * bones. Each part's mesh is read at LOD0 as its own upload is (FMantleCosmeticExport::ReadStaticMesh). A part that cannot be read is left out and named in
	 * OutNotes, never refused: the car works without it, only its surface is missing for the shape.
	 */
	static FMantleVehicleCollision ForParts(const UMantleVehicleSource& Source, TConstArrayView<FMantleGlbJoint> Joints, TArray<FString>& OutNotes);

private:
	/** Welded vertex by position rounded to a thousandth of a centimeter. */
	TMap<FIntVector, int32> Welded;
};
