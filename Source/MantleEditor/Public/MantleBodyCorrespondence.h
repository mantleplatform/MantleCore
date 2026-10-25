// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class USkeletalMesh;

/** Geometry-free MetaHuman body layout. No shape, skin, bone transforms or materials. Editor only. */
struct MANTLEEDITOR_API FMantleBodyCorrespondence
{
	TArray<FIntPoint> QuantizedUVs;
	TArray<uint32> Indices;
	uint32 UVHash = 0;
	/** Enabled only on lower layouts by exact policy agreement in both reference resources. */
	bool bLowerCornerTopology = false;

	/** Plugin's versioned canonical layout; null when absent or invalid. */
	static const FMantleBodyCorrespondence* Get();
	/** Native Optimized Medium body exports carry these complete LODs. */
	static constexpr int32 ExpectedNativeLODs = 3;
	static const FMantleBodyCorrespondence* GetLOD(int32 LODIndex);
	/** Missing policy keeps indexed matching; unsupported or disagreement fails closed. */
	static bool ReadLowerLODPolicy(const FJsonObject& Layout, const FJsonObject& Topology, bool& OutEnabled, FString& OutError);

	/**
	 * Complete reference -> input bijection, distinguished by UV and oriented incident triangles.
	 * Checks the entire mapped triangle multiset, including winding. Refuses ambiguity, missing faces,
	 * changed connectivity and non-finite UVs; never uses positions or a nearest-point guess.
	 */
	bool Match(TConstArrayView<FVector2f> UVs, TConstArrayView<uint32> InputIndices,
		TArray<int32>& OutInputOfReference, FString& OutError) const;

	/** Exact-twin quotient mapping for the body-export merge; leaves the input streams intact.
	 * Exact position+UV copies may share a logical reference only after the existing complete
	 * indexed UV/connectivity match and exact effective skin-copy validation both succeed.
	 * Leaves all physical streams/triangles/tangents intact; outputs both mapping directions.
	 */
	bool MatchNativeSplits(TConstArrayView<FVector2f> UVs, TConstArrayView<uint32> InputIndices,
		TConstArrayView<FVector3f> Positions, TConstArrayView<uint16> GlobalBones,
		TConstArrayView<float> Weights, int32 NumInfluences, TArray<int32>& OutReferenceOfInput,
		TArray<int32>& OutInputOfReference, FString& OutError) const;

	/** Lower-LOD (and general) merge of render vertices that differ ONLY in tangent. Unreal's mesh build
	 * splits a vertex at a UV seam when the computed tangent differs on each side; Epic's source data
	 * still has the reference counts. Vertices merge only when position, every UV channel, normal, colour
	 * and every skin slot (bone and weight) agree within a tight tolerance, so genuine hard edges and UV
	 * seams are never collapsed. The lowest physical ID survives. The merged set must then pass the
	 * unchanged complete Match. OutReferenceOfInput maps EVERY physical vertex (duplicates too) to its
	 * reference vertex; OutInputOfReference gives each reference vertex's surviving physical vertex.
	 * UV1s, Colors may be empty (absent channel). Any other mismatch is refused with a reason. */
	bool MatchTangentSplits(TConstArrayView<FVector2f> UVs, TConstArrayView<FVector2f> UV1s,
		TConstArrayView<uint32> InputIndices, TConstArrayView<FVector3f> Positions, TConstArrayView<FVector3f> Normals,
		TConstArrayView<FColor> Colors, TConstArrayView<uint16> GlobalBones, TConstArrayView<float> Weights,
		int32 NumInfluences, TArray<int32>& OutReferenceOfInput, TArray<int32>& OutInputOfReference, FString& OutError) const;

	/**
	 * Lower-LOD semantic corner correspondence, permitting native shading splits without merging them.
	 * Each complete oriented UV triangle must identify one unambiguous reference face. Every packed
	 * input vertex must map consistently to one logical reference vertex; all faces/vertices participate.
	 * Outputs input -> reference IDs, never a reordered or reduced stream. Callers must additionally
	 * check that packed copies of one reference vertex share position and effective skin bindings.
	 * Does not replace the canonical LOD0/MBS bijection. Runs only for lower LODs whose policy sets bLowerCornerTopology (ReadLowerLODPolicy).
	 */
	bool MatchCorners(TConstArrayView<FVector2f> UVs, TConstArrayView<uint32> InputIndices,
		TArray<int32>& OutReferenceOfInput, FString& OutError) const;

	/** Exact clone positions and normalized global bindings. Export uses the cook's 255 largest-remainder rule. */
	bool ValidateCornerCopies(TConstArrayView<int32> ReferenceOfInput, TConstArrayView<FVector3f> Positions,
		TConstArrayView<uint16> GlobalBones, TConstArrayView<float> Weights, int32 NumInfluences, FString& OutError,
		bool bCookQuantization = false) const;
	/** Validate one original attached-DNA target, including sparse index uniqueness and all offsets. */
	static bool ValidateDNABodyTarget(uint32 PositionCount, uint32 DeltaCount,
		TConstArrayView<float> Xs, TConstArrayView<float> Ys, TConstArrayView<float> Zs,
		TConstArrayView<uint32> PositionIDs, FString& OutError);

	/** DNA's raw UVs use its V basis. Select only the unique reference-proved diagonal of
	 * each raw triangle/quad, then run the unchanged complete indexed matcher. No GLB
	 * conversion, guessed import-map domain or nearest-position correspondence. */
	bool MatchDNABodyPolygons(TConstArrayView<FVector2f> RawUVs, TConstArrayView<TArray<uint32>> Faces,
		TArray<int32>& OutLayoutOfReference, FString& OutError) const;

	/**
	 * A body with the reference UV layout whose triangulation differs from the reference only where Epic's Optimized build cut a quad
	 * the other way (MetaHuman Creator's in-memory body, which has not been through that build). Vertices are matched by UV; a UV the
	 * reference repeats is told apart by the UVs of its neighbours. Every reference triangle must then be an input triangle, except
	 * flipped-diagonal pairs (two triangles sharing an edge, replaced by the two on the other diagonal), at most MaxFlippedPairs of them (the body: 8).
	 * A UV the reference repeats up to 6 times is told apart by neighbours. OutFlippedPairs: how many pairs there were.
	 * OutInputOfReference is as Match's; OutNote says how many repeated UVs and flipped diagonals there were. No positions are read.
	 */
	bool MatchFlippedDiagonals(TConstArrayView<FVector2f> UVs, TConstArrayView<uint32> InputIndices,
		TArray<int32>& OutInputOfReference, FString& OutNote, FString& OutError, int32 MaxFlippedPairs = 8, int32* OutFlippedPairs = nullptr) const;

	/** Body-export-only LOD0 merge proof. Original asset/streams are never changed.
	 * Exact position/all UV channels/raw global skin/colour and every morph delta must agree.
	 * Static normals/tangents may differ; lowest physical ID survives. Strict indexed Match
	 * proves the complete quotient; backend, fitting and lower-LOD contracts remain unchanged. */
	bool MatchMergedBodyExport(const USkeletalMesh* Body, TArray<int32>& OutReferenceOfInput,
		TArray<int32>& OutInputOfReference, TArray<FString>& OutMergedSplits, FString& OutError) const;
};
