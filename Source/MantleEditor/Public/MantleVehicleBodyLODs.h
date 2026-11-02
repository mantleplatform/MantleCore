// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleGlbWriter.h"

class UMantleVehicleSource;

/**
 * A vehicle body's lower LODs (limits.json lods, vehicle_body): what the game draws a car with far away. Made at upload from the body as it
 * uploads (skinned, its tires merged on, sections named by material), with the reducer that fits a heavy body to the limit (FMantleVehicleBodyReduce):
 * each bone's triangles, tires and all, reduced as a piece of their own, so the wheels, doors and parts keep their places on every LOD.
 *
 * Every LOD keeps LOD0's joints and draws only LOD0's materials. A light or paint section the reduction loses is put back whole from LOD0, so the
 * lights still show and the paint still changes far away. A LOD that would break the cook's rules (more triangles than the one before it) ends the
 * list there: one LOD is a valid upload, it only costs more to draw far away.
 */
struct MANTLEEDITOR_API FMantleVehicleBodyLODs
{
	/**
	 * LOD1, LOD2, ... of Body at Fractions of its triangles (limits.json lods.targetFractions after LOD0's). KeepSections are the material names that
	 * must be drawn on every LOD (the body's light and paint sections). Each lower LOD rides one joint per vertex (the heaviest of LOD0's), with
	 * tangents worked out again; LOD0's second UVs and vertex colors are not carried (the skinned cook keeps neither). OutNotes says what was made;
	 * OutWarning, in plain words for the creator, why fewer LODs were made than asked for (empty when all were).
	 */
	static TArray<FMantleGlbMesh> Make(const FMantleGlbMesh& Body, TConstArrayView<float> Fractions, TConstArrayView<FString> KeepSections,
		TArray<FString>& OutNotes, FString& OutWarning);

	/**
	 * Make for Source's body as it uploads (Body: FMantleVehicleConverter::ReadBody, sections named): at limits.json lods.targetFractions, keeping
	 * Source's light and paint sections. What publishing and Mantle.Vehicle.WriteUpload upload.
	 */
	static TArray<FMantleGlbMesh> MakeFor(const UMantleVehicleSource& Source, const FMantleGlbMesh& Body, TArray<FString>& OutNotes, FString& OutWarning);

	/**
	 * A vehicle LOD as it uploads, slimmed without changing what the cook stores: vertices that are the same in every stream (position, normal,
	 * tangent, UVs, color, skin) are written once per primitive, and the skin and indices in their compact forms (FMantleGlbMesh::bCompactStreams).
	 */
	static void Slim(FMantleGlbMesh& Mesh);

	/** Triangles in a GLB mesh, every primitive. */
	static int32 CountTriangles(const FMantleGlbMesh& Mesh);
};
