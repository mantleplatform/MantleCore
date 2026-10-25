// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleBodyShape.h"
#include "MantleGarmentClearance.h"
#include "MantleGarmentSkinTransfer.h"
#include "MantleSkinnedMesh.h"

/**
 * The deformation field from one body shape onto one avatar:
 * the engine's RBF sample deformations, computed once per (source shape, avatar) and applied to every
 * garment authored on that shape. Self-contained (it copies the shape's sample rest positions), so it
 * outlives the shape and can be cached and shared across threads read-only.
 */
struct MANTLERUNTIME_API FMantleWarpField
{
	/** FMantleBodyShape::PositionsHash of the source and ComputePositionsHash of the avatar: the cache key. */
	uint32 SourceHash = 0;
	uint32 AvatarHash = 0;

	/** The avatar is the source shape (within 0.001 cm everywhere): the warp is the identity and adds no noise. */
	bool bIdentity = false;

	/** Neck samples the field was computed with after the body (0: from the body alone). */
	int32 NumNeckSamples = 0;

	/** The source shape's samples (Weights.SampleRestPositions). Empty on the identity. */
	TArray<FVector3f> SampleRestPositions;

	/** The avatar's positions at the shape's SampleIndices, in sample order: what GenerateMeshSamples was given. */
	TArray<FVector3f> TargetSamples;

	/** GenerateMeshSamples' output: one deformation per sample, then the affine terms (constant, *X, *Y, *Z). */
	TArray<FVector3f> SampleDeformations;

	/** Time ComputeField took. */
	double Ms = 0.0;

	/** An identity, or S >= 1 samples with S + 4 deformations. */
	bool IsValid() const;

	/**
	 * One point through the field: the formula of the engine's DeformPoint (MeshResizing's
	 * RBFInterpolation.cpp, private), P + sum_i sqrt(|P - rest_i|^2 + 1) * D_i + D_S + D_S+1 * P.X +
	 * D_S+2 * P.Y + D_S+3 * P.Z. Re-implemented because the engine's DeformPoints recomputes the sample
	 * deformations on every call. P itself on the identity.
	 */
	FVector3f Evaluate(const FVector3f& P) const;
};

/** What one Fit did. */
struct FMantleGarmentFitStats
{
	double FitMs = 0.0;
	int32 NumVertices = 0;
	int32 NumTriangles = 0;
	double MeanMoveCm = 0.0;
	double MaxMoveCm = 0.0;

	/** The garment now carries the wearer's bone table and reference pose (always, on success). */
	bool bRebound = false;

	/** The tables differed, so the skin influences were remapped by bone name. */
	bool bBonesRemapped = false;

	/** Garment bones without weight the wearer does not have (dropped, harmless). */
	int32 UnusedBonesDropped = 0;

	/** Weighted helper-bone slots collapsed onto an existing ancestor (#641). */
	int32 AncestorRemappedInfluences = 0;
};

/**
 * What the plugin derives at upload from a garment and the body and face it was authored on (section
 * 0.1 items 12 and 13): the garment's clearance over its source body (the body restore's target and the
 * skin transfer's correspondence) and over its source face (the collar's; the face's open neck-base
 * boundary untracked). Every wearer shares the body's vertex order; faces need not share anything.
 */
struct MANTLERUNTIME_API FMantleGarmentAuthoredClearance
{
	/** LOD0's. */
	FMantleGarmentClearance Body;

	/** Empty (no garment entries) when the garment was uploaded without a source face. */
	FMantleGarmentClearance Face;

	/**
	 * LOD1, LOD2, ... of the garment, each over the same source body (and face, when Face is
	 * set; LowerFace is then one per lower LOD too). Every LOD of a garment has its own vertices, so
	 * each carries its own authored correspondence; empty for a one-LOD garment.
	 */
	TArray<FMantleGarmentClearance> LowerBody;
	TArray<FMantleGarmentClearance> LowerFace;

	/**
	 * #877: the skin the garment hides on its own source body (FMantleSkinMask::Derive, run by
	 * FMantleCatalogGarmentFit::DeriveAuthoredClearance); null when it could not be derived. Shared by every wearer:
	 * FMantleDressedBody ANDs it with each wearer's own coverage.
	 */
	TSharedPtr<const struct FMantleSkinMask> SkinMask;

	bool HasFace() const { return Face.Triangle.Num() > 0; }

	/** The garment LODs this clearance covers: 1 plus the lower ones. */
	int32 NumLODs() const { return 1 + LowerBody.Num(); }

	/** LOD's body clearance (0 is Body); LOD must be below NumLODs(). */
	const FMantleGarmentClearance& BodyOf(const int32 LOD) const { return LOD == 0 ? Body : LowerBody[LOD - 1]; }

	/** LOD's face clearance, null without a face (or for a LOD it does not cover). */
	const FMantleGarmentClearance* FaceOf(const int32 LOD) const
	{
		return !HasFace() ? nullptr : (LOD == 0 ? &Face : (LowerFace.IsValidIndex(LOD - 1) ? &LowerFace[LOD - 1] : nullptr));
	}
};

/** What one FitToWearer did: every stage's numbers, for the logs and the quality checks. */
struct MANTLERUNTIME_API FMantleGarmentFitReport
{
	/** The warp was the identity (the garment on its own body): no restore runs, the positions stay as authored. */
	bool bIdentity = false;

	double WarpMs = 0.0;
	double TotalMs = 0.0;
	double MeanMoveCm = 0.0;
	double MaxMoveCm = 0.0;

	/** Triangles turned against the authored garment after the warp (INFO: the warp's own), after each restore, and at the end. */
	FMantleFaceFlips FlipsWarp;
	FMantleFaceFlips FlipsBodyRestore;
	FMantleFaceFlips FlipsFaceRestore;
	FMantleFaceFlips FlipsFinal;

	/** Triangles flipped at the end that the warp had not flipped: what the restores added (0 is the pass bar). */
	int32 FlipsAddedByRestores = 0;

	/** Body-tracked vertices the source face tracks too, left out of the body restore and to the face restore. */
	int32 LeftToFace = 0;

	FMantleClearanceRestoreStats BodyRestore;
	FMantleClearanceRestoreStats FaceRestore;

	/** The face restore: vertices within reach of the wearer's face (its open boundary excluded), and of those tracked on the source face too (-1: not run). */
	int32 FaceWithin = 0;
	int32 FaceTracked = -1;

	/** After the face restore: tracked vertices still short of their authored face clearance by more than the slack, and the worst, cm. */
	int32 FaceStillBelow = 0;
	float FaceStillWorstCm = 0.0f;

	FMantleSkinTransferStats Transfer;
	FMantleSkinSmoothStats Smooth;

	/** Tracked vertices whose dominant bone is the wearer's at their authored closest corner, percent, after the transfer and after smoothing (-1: not measured). */
	double DominantMatchPct = -1.0;
	double SmoothDominantMatchPct = -1.0;

	/** The rebind. */
	FMantleGarmentFitStats Rebind;

	/**
	 * The LODs fitted (every LOD of the garment), and what the lower ones added up to: their
	 * time, the flips their restores added and their dominant-bone match after smoothing (mean over
	 * the lower LODs, -1 when there are none). Every other field above is LOD0's.
	 */
	int32 NumLODs = 1;
	double LowerLODsMs = 0.0;
	int32 LowerLODsFlipsAdded = 0;
	double LowerLODsSmoothDominantMatchPct = -1.0;
};

/**
 * The garment fitter. Epic's MeshResizing RBF warp, vendored as FMantleRBFInterpolation:
 *
 *   ComputeSourceWeights(shape, points)  FMantleRBFInterpolation::GenerateWeights. Editor and plugin side,
 *                                        at upload: cubic in the point count, seconds per shape, so
 *                                        never on a client at load. The weights ride with MBS1.
 *   ComputeField(shape, avatar)          FMantleRBFInterpolation::GenerateMeshSamples fed with the avatar's
 *                                        positions at the shape's SampleIndices, the contract the
 *                                        engine asserts on. About a millisecond; once per (shape,
 *                                        avatar), cached by the caller.
 *   Fit(garment, field, wearer)          The field applied to positions, normals and tangents (the way
 *                                        the engine's DeformPoints moves normals: warp a 1 cm offset
 *                                        along them); indices, UVs, sections and skin weights copy
 *                                        through; then the mandatory rebind to the wearer.
 *
 * Shapes and avatars must have the reference topology's vertex count (UMantleBodyTopologyData), and
 * a mismatch is refused. Field and Fit touch no UObjects and may run on any thread.
 *
 * The garment side of the pipeline:
 *
 *   ExtendShape(shape, neck samples)     The fit's shapes carry neck samples from the faces after the
 *                                        body (FMantleNeckSamples): the neck is the face mesh's.
 *   ComputeAuthoredClearance(...)        Upload side: the garment's clearance over its source body and face.
 *   FitToWearer(garment, field, wearer)  The warp, the body clearance restore (collar vertices left to
 *                                        the face), the face clearance restore, the wearer's skin weights
 *                                        transferred and smoothed faded by clearance, then the rebind.
 *
 * Not here: per-wearer coverage, the seam shrink and the body sink.
 *
 * Failures log "MANTLE_GARMENTFIT FAIL <step>: <reason>" and return the reason in OutError.
 */
class MANTLERUNTIME_API FMantleGarmentFitter
{
public:
	/** Interpolation points when a caller has no reason to choose (limits.json's starting value). */
	static constexpr int32 DefaultPoints = 750;

	/**
	 * Part of the cosmetic cache's fit key, so fits made by an older FitToWearer are not reused. 2: the
	 * collar takes the face's skin weights and the garment's slots grow to the skin's. 3: the face's share
	 * is smoothed around the collar, and the weight smoothing covers the vertices that took it.
	 */
	static constexpr int32 WearerFitVersion = 3;

	/**
	 * Fills Shape.Weights with the engine's RBF weights over Shape.Positions using PointCount
	 * interpolation points, chosen by the engine's farthest-point sampler (GroomBinding_RBFWeighting::
	 * FPointsSampler, starting at vertex 0). False (weights left empty)
	 * when Shape does not have ReferenceVertexCount positions, PointCount is below 1, or the result
	 * does not ValidateWeights. OutMs, when given, gets the time taken.
	 */
	static bool ComputeSourceWeights(FMantleBodyShape& Shape, int32 PointCount, int32 ReferenceVertexCount,
		FString* OutError = nullptr, double* OutMs = nullptr);

	/**
	 * The same over a shape extended with NumNeckSamples neck samples after its ReferenceVertexCount
	 * body positions (ExtendShape): Shape must have exactly ReferenceVertexCount + NumNeckSamples
	 * positions. The farthest-point sampler picks the RBF centers among all of them (49 of 1,000
	 * land on the neck).
	 */
	static bool ComputeSourceWeights(FMantleBodyShape& Shape, int32 PointCount, int32 ReferenceVertexCount, int32 NumNeckSamples,
		FString* OutError = nullptr, double* OutMs = nullptr);

	/**
	 * The field from Shape (which must have weights) onto an avatar whose reference-topology positions
	 * are AvatarPositions. Null when Shape or the avatar does not have ReferenceVertexCount positions or
	 * the weights do not ValidateWeights against Shape.
	 */
	static TSharedPtr<const FMantleWarpField> ComputeField(const FMantleBodyShape& Shape, TConstArrayView<FVector3f> AvatarPositions,
		int32 ReferenceVertexCount, FString* OutError = nullptr);

	/**
	 * The same with NumNeckSamples neck samples after the body on both sides: Shape (extended by
	 * ExtendShape with the source face's samples) and AvatarPositions (the avatar's body positions, then
	 * the wearer face's corresponding samples, FMantleNeckSamples::FResult::Wearer) each have
	 * ReferenceVertexCount + NumNeckSamples positions.
	 */
	static TSharedPtr<const FMantleWarpField> ComputeField(const FMantleBodyShape& Shape, TConstArrayView<FVector3f> AvatarPositions,
		int32 ReferenceVertexCount, int32 NumNeckSamples, FString* OutError = nullptr);

	/**
	 * Body (reference-topology positions, no neck samples yet) with NeckSamples appended after its
	 * positions (the body stays first, so every body index holds), the positions hash recomputed and no
	 * weights: the fit's source shape once ComputeSourceWeights has run on it.
	 */
	static FMantleBodyShape ExtendShape(const FMantleBodyShape& Body, TConstArrayView<FVector3f> NeckSamples);

	/**
	 * Upload side: Out.Body = the garment's LOD0 clearance over SourceBody (the source body's reference
	 * topology surface, FMantleGarmentClearance::DefaultSearchCm); Out.Face, when SourceFace is given, its
	 * clearance over the source face within FMantleGarmentClearance::FaceSearchCm with the face's open
	 * boundary untracked). Every lower LOD of the garment gets the same in Out.LowerBody (and
	 * Out.LowerFace). False, with the reason, on bad inputs.
	 */
	static bool ComputeAuthoredClearance(const FMantleSkinnedMesh& Garment, const FMantleSkinnedMeshLOD& SourceBody, const FMantleSkinnedMeshLOD* SourceFace,
		FMantleGarmentAuthoredClearance& Out, FString* OutError = nullptr);

	/**
	 * The garment side of the fit, in this order:
	 *   1. the warp (Field: the source shape and the avatar extended with the neck samples);
	 *   2. the body clearance restore against WearerBody's LOD0 positions (Authored.Body's targets, less
	 *      every vertex the source face tracks, those are the face's);
	 *   3. the face clearance restore, when WearerFace and Authored.Face are given: the fitted garment's
	 *      clearance over the wearer's face within FaceSearchCm (its open boundary untracked), each vertex
	 *      the source face tracks too restored to its authored face clearance (the restore's floor
	 *      is for the outer layer only, so the folded collar's inner layer, authored inside the neck,
	 *      stays inside);
	 *   4. the wearer's skin weights at the authored closest point (FMantleGarmentSkinTransfer::Transfer),
	 *      blended with the face's within its reach when WearerFace and WearerFaceBones (the face's bone
	 *      table) are given, the garment's slots grown to the skin's; then smoothed faded by the authored
	 *      clearance;
	 *   5. the rebind to WearerBody's bone table and reference pose.
	 * The restores work per section, so the vertices that coincide as authored in two or more
	 * sections (the seams between sections) are found once per fit, by position before the warp, and each
	 * restore pushes a seam's copies as one (FMantleClearanceRestore::Restore's Seams), so the seams stay
	 * closed. Weights, smoothing and normals are untouched. The cost is logged as "MANTLE_FIT seam weld".
	 * The identity warp (the garment on its own body) skips both restores and keeps the positions
	 * bit-exact. Stages 1 to 4 run on every LOD of the garment, each LOD warped by the same
	 * field and restored and re-skinned against the wearer's LOD0 through its own authored clearance, so
	 * Authored must cover every LOD (Authored.NumLODs() == the garment's LOD count); the rebind is the
	 * mesh's, once. WearerBody must have the reference topology at LOD0 (Authored.Body's surface count)
	 * and its own reference pose; its lower LODs play no part. False (OutFitted unspecified), with the
	 * reason, when an input does not match or a stage fails on any LOD.
	 */
	static bool FitToWearer(const FMantleSkinnedMesh& Garment, const FMantleGarmentAuthoredClearance& Authored, const FMantleWarpField& Field,
		const FMantleSkinnedMesh& WearerBody, const FMantleSkinnedMeshLOD* WearerFace, FMantleSkinnedMesh& OutFitted,
		FString* OutError = nullptr, FMantleGarmentFitReport* OutReport = nullptr, TConstArrayView<FMantleSkinnedMeshBone> WearerFaceBones = {});

	/**
	 * Garment warped by Field, every LOD, and rebound to Wearer (Rebind). Normals and tangents: the
	 * field applied to a 1 cm offset along them, the tangent re-orthogonalised to the new normal, the
	 * binormal sign kept; a degenerate result keeps the original direction. False (OutFitted
	 * unspecified) when the garment does not Validate, the field is not valid, or the rebind fails.
	 */
	static bool Fit(const FMantleSkinnedMesh& Garment, const FMantleWarpField& Field, const FMantleReferencePose& Wearer,
		FMantleSkinnedMesh& OutFitted, FString* OutError = nullptr, FMantleGarmentFitStats* OutStats = nullptr);

	/**
	 * The rebind (mandatory): Garment takes Wearer's bone table and reference pose.
	 * Leader pose hands the wearer's bone transforms to the garment, which skins them against its own
	 * inverse reference pose, so a garment that kept its authoring body's pose would sit off by the
	 * joint differences between the two bodies. Identical tables
	 * (names and parents in order) keep every influence index; otherwise each weighted influence is
	 * remapped by bone name and unweighted slots point at bone 0. Missing weighted helper bones
	 * collapse onto their nearest retained ancestor; duplicate influence slots are merged exactly.
	 * Invalid data or a missing root/main humanoid-chain bone fails with Garment unchanged.
	 * Missing unweighted helper bones are dropped and counted. This does not retarget unrelated rigs.
	 */
	static bool Rebind(FMantleSkinnedMesh& Garment, const FMantleReferencePose& Wearer, FString* OutError = nullptr,
		FMantleGarmentFitStats* OutStats = nullptr);
};
