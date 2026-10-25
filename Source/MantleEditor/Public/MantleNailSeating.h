// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleSkinnedMesh.h"

struct FMeshDescription;
class USkeletalMesh;

/** What FMantleNailSeating::Seat did, for the log, the notes and the tests. */
struct FMantleNailSeatingResult
{
	/** Separate pieces (connected shells) of the nail mesh with at least one triangle. */
	int32 NumIslands = 0;

	/** Islands moved to the seat; the others had no vertex within the search of the body. */
	int32 NumSeated = 0;

	/** The farthest any vertex moved in total (cm). */
	float MaxMoveCm = 0.0f;

	/** After the last pass, over the seated islands: the lowest and the highest of their closest vertices' clearance (cm, negative under the skin). */
	float LowestClearanceCm = 0.0f;
	float HighestClearanceCm = 0.0f;
};

/**
 * #740: seats shaped nails on the reference body they were refitted onto. A nail pack authored without the body (or
 * placed by its bones alone) lands sunk into the fingertips, and the warp-only fit every wearer
 * gets carries that placement unchanged, so it has to be right on the authoring body.
 *
 * Per island (a connected shell, one nail): its normal is the smallest principal axis of its vertices, flipped to
 * point away from the skin (against the mean oriented normal of the body triangles under it); the island is
 * translated rigidly along it until its closest vertex sits SeatCm above the body. Three passes, as the body under
 * the island changes as it moves. Not along the fingertip's surface normal: that slides nails off the top of
 * the finger. A rigid move keeps each nail's shape exactly.
 *
 * Pure over arrays except ReadBodySurface and SeatMeshDescription; may run on any thread.
 */
class MANTLEEDITOR_API FMantleNailSeating
{
public:
	/** The closest vertex of a seated nail is this far above the body (cm). */
	static constexpr float SeatCm = 0.05f;

	/** Passes. */
	static constexpr int32 Passes = 3;

	/** Vertices further than this from the body do not steer the move (cm); a nail is within a centimetre of its fingertip. */
	static constexpr float SearchCm = 10.0f;

	/**
	 * A nail carried onto the body by its finger bone (FMantleRefitPlan::bOnBones) is seated by at most this much (cm);
	 * more says it is not bound to the finger it sits on.
	 */
	static constexpr float MaxSeatMoveCm = 2.0f;

	/**
	 * The connected shells of a triangle list: OutIsland gets each vertex's island (0 up), INDEX_NONE for a vertex no
	 * triangle uses. Returns the number of islands. Pure (tested).
	 */
	static int32 FindIslands(TConstArrayView<uint32> Indices, int32 NumVertices, TArray<int32>& OutIsland);

	/** The unit direction of least spread of Points (the smallest principal axis); zero for fewer than three points. Pure (tested). */
	static FVector3f ThinAxis(TConstArrayView<FVector3f> Points);

	/**
	 * Moves Positions (the nail mesh's vertices, Indices its triangles) as the class comment says over Body. False
	 * with why on bad inputs. Pure (tested).
	 */
	static bool Seat(TArray<FVector3f>& Positions, TConstArrayView<uint32> Indices, const FMantleSkinnedMeshLOD& Body,
		FMantleNailSeatingResult& Out, FString& OutError);

	/** One LOD's mesh description seated over Body (vertex positions only). */
	static bool SeatMeshDescription(FMeshDescription& Mesh, const FMantleSkinnedMeshLOD& Body, FMantleNailSeatingResult& Out, FString& OutError);

	/**
	 * LOD LODIndex of Body as a surface (positions, normals, triangles) from its mesh description, else its imported
	 * model. False with why when it has neither. Editor data; game thread.
	 */
	static bool ReadBodySurface(const USkeletalMesh& Body, int32 LODIndex, FMantleSkinnedMeshLOD& Out, FString& OutError);
};
