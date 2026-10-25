// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleSkinnedMesh.h"

struct FMantleGarmentClearance;

/** What TransferSkinWeights did (the skin fields of the fit stats). */
struct MANTLERUNTIME_API FMantleSkinTransferStats
{
	bool bRan = false;

	/** Vertices weighted from the wearer's skin at their closest point (the body's, the face's or both) ... */
	int32 FromBody = 0;

	/** ... of those, ones that took some of the face's weights (the collar and the neck opening) ... */
	int32 FromFace = 0;

	/** ... untracked ones that copied their nearest tracked vertex's new weights ... */
	int32 FromNearest = 0;

	/** ... and ones that kept the garment's own weights (no tracked vertex at all). */
	int32 Kept = 0;

	/** Influence slots per vertex before and after (grown to the skin's slot count, at most MaxInfluences, then cut to the most any vertex uses). */
	int32 SlotsBefore = 0;
	int32 Influences = 0;

	/** Skin influences dropped because the garment's bone table has no bone of that name (nor, for the face, an ancestor of it). */
	int32 DroppedInfluences = 0;

	/** Per changed vertex, the largest per-bone |new - old| weight (0..1): mean and max; changed vertices whose dominant bone moved. */
	double MeanChange = 0.0;
	double MaxChange = 0.0;
	int32 DominantChanged = 0;
	double Ms = 0.0;
};

/**
 * The wearer's face for the skin transfer: the collar and the neck opening lie over the face mesh (the neck is the
 * face's), not the body, so they take the face's weights there. Surface is the face LOD Clearance was measured
 * over (the fitted garment's clearance within FMantleGarmentClearance::FaceSearchCm, its open boundary
 * untracked) and Bones the face's bone table.
 */
struct MANTLERUNTIME_API FMantleSkinTransferFace
{
	const FMantleSkinnedMeshLOD* Surface = nullptr;
	TConstArrayView<FMantleSkinnedMeshBone> Bones;
	const FMantleGarmentClearance* Clearance = nullptr;

	bool IsSet() const { return Surface && Clearance && Bones.Num() > 0; }
};

/** What SmoothSkinWeights did. */
struct MANTLERUNTIME_API FMantleSkinSmoothStats
{
	bool bRan = false;
	int32 Iterations = 0;

	/** Welded points, the split copies beyond the first, points faded by their clearance, and of those the ones not smoothed at all. */
	int32 Points = 0;
	int32 SplitCopies = 0;
	int32 FadedPoints = 0;
	int32 ExactPoints = 0;

	double MeanChange = 0.0;
	double MaxChange = 0.0;
	int32 DominantChanged = 0;
	double Ms = 0.0;
};

/**
 * The wearer's skin weights transferred onto a fitted garment, so fabric and skin follow the same bones
 * once the pose leaves the reference pose, as Roblox layered clothing does. Weights are MSK1's 8 bits
 * summing to exactly 255.
 *
 * Holds no UObjects; may run on any thread.
 */
struct MANTLERUNTIME_API FMantleGarmentSkinTransfer
{
	/**
	 * Influences kept per vertex, at most. 12, the MetaHuman body's slot count (the blend of three
	 * corners' influences has more bones; cut to 8 it dropped weight the skin still has).
	 */
	static constexpr int32 MaxInfluences = 12;

	/**
	 * The face's share of a vertex's weights: dB / (dB + dF) over the distances to the body and to the
	 * face (each floored at MinShareDistanceCm), faded to 0 over the last FaceFadeCm of the face's reach,
	 * so the share falls smoothly to the body's weights where the face stops tracking.
	 */
	static constexpr float MinShareDistanceCm = 0.1f;
	static constexpr float FaceFadeCm = 2.0f;

	/**
	 * The face's share is then smoothed over the garment's mesh (FaceShareSmoothIters passes over its edges) as an
	 * average over the face-tracked points, divided by their smoothed fraction floored at FaceShareMinCoverage:
	 * neighbouring collar vertices get about the same neck / chest mix (from each vertex's own distances it jumps
	 * around the ring, and the face's untracked boundary leaves holes), and the share fades out evenly where the
	 * face stops tracking. Shares under half an 8-bit weight step are 0.
	 */
	static constexpr int32 FaceShareSmoothIters = 6;
	static constexpr float FaceShareMinCoverage = 0.5f;

	/** Smoothing passes, and the authored clearances the smoothing fades between (none at or below Tight, full at or above Loose). */
	static constexpr int32 DefaultSmoothIters = 3;
	static constexpr int32 MaxSmoothIters = 50;
	static constexpr float DefaultSmoothTightCm = 1.0f;
	static constexpr float DefaultSmoothLooseCm = 3.0f;

	/**
	 * The smoothing factor's floor where a vertex took the face's weights: FaceSmoothFactor from a face share
	 * of FaceSmoothFullShare up, scaled down below it. A snug collar is smoothed too (the face and the body
	 * meet under it), the rest of the tight fabric still keeps the skin's exact weights.
	 */
	static constexpr float FaceSmoothFactor = 0.5f;
	static constexpr float FaceSmoothFullShare = 0.25f;

	/**
	 * Garment (LOD with GarmentBones as its bone table) takes the wearer's weights at the authored
	 * correspondence (Authored: the garment's clearance over its source body, whose vertex order Wearer's
	 * LOD0 shares):
	 *   - tracked vertex: the (bone, weight) lists of the three wearer vertices of its authored closest
	 *     triangle blended with its barycentrics; with Face set and the vertex within its reach, blended
	 *     with the face's three corners at its closest face point by the face's smoothed share (FaceFadeCm,
	 *     FaceShareSmoothIters; only the face's when the body does not track it; a vertex the face does not
	 *     track but whose neighbours' share reaches it samples the face at the nearest face-tracked one);
	 *     the top N kept, renormalised and quantised to sum exactly 255;
	 *   - untracked vertex: its nearest tracked vertex's new weights (fitted positions); none at all: kept.
	 * N is the skin's slot count (the body's, or the face's when larger), at most MaxInfluences, and never
	 * below the garment's own: an unrigged garment (one slot) takes the skin's blend whole. The garment's
	 * slots grow to N and are then cut to the most any vertex uses.
	 * Wearer bones map to GarmentBones by name (identity when the tables match); face bones by name, or a
	 * face bone the garment lacks (the face's FACIAL_ neck joints) to its nearest face ancestor the garment
	 * has (neck_01, neck_02, head); influences with no such bone are dropped and counted. False (Garment
	 * unchanged), with the reason in OutError, on mismatched inputs. OutFaceShare, when given, gets each
	 * vertex's face share (0..1; all 0 without Face), for SmoothFactor.
	 */
	static bool Transfer(FMantleSkinnedMeshLOD& Garment, TConstArrayView<FMantleSkinnedMeshBone> GarmentBones, const FMantleGarmentClearance& Authored,
		const FMantleSkinnedMesh& Wearer, const FMantleSkinTransferFace& Face = FMantleSkinTransferFace(), FMantleSkinTransferStats* OutStats = nullptr,
		FString* OutError = nullptr, TArray<float>* OutFaceShare = nullptr);

	/**
	 * The per-vertex smoothing factor from the authored clearance: 0 at or below TightCm (fabric lying
	 * on the skin keeps the skin's exact weights), 1 at or above LooseCm, linear between; untracked vertices 1.
	 * With FaceShare (Transfer's, one per vertex), floored at FaceSmoothFactor where the vertex took the
	 * face's weights (FaceSmoothFullShare).
	 */
	static void SmoothFactor(const FMantleGarmentClearance& Authored, float TightCm, float LooseCm, TArray<float>& Out, const TArray<float>* FaceShare = nullptr);

	/**
	 * Laplacian smoothing of the skin weights over the welded points' edge adjacency (split copies end
	 * with identical weights; sections are never smoothed into each other). Each pass over the union of
	 * bones: w'(p) = (1 - 0.5 f) w(p) + 0.5 f * the neighbours' mean, the top N kept and renormalised; f is
	 * the point's mean Factor (1 everywhere when Factor is null). Iterations <= 0 changes nothing. False
	 * (Garment unchanged) on bad skin data.
	 */
	static bool Smooth(FMantleSkinnedMeshLOD& Garment, int32 Iterations, const TArray<float>* Factor = nullptr, FMantleSkinSmoothStats* OutStats = nullptr);

	/**
	 * The dominant-bone agreement check: over Garment's tracked vertices, how many have
	 * the dominant bone (by name) of Wearer's LOD0 vertex at the corner of their authored closest triangle
	 * with the largest barycentric. False when the sizes do not match.
	 */
	static bool DominantMatch(const FMantleSkinnedMeshLOD& Garment, TConstArrayView<FMantleSkinnedMeshBone> GarmentBones, const FMantleGarmentClearance& Authored,
		const FMantleSkinnedMesh& Wearer, int32& OutMatches, int32& OutTracked);
};
