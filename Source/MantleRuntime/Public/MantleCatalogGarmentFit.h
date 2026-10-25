// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleGarmentFitter.h"
#include "MantleNeckSamples.h"

struct FMantleBodyShape;
struct FMantleBodyShapeNeck;

/** Which fit a catalog garment got (FMantleCatalogGarmentFit::Fit). */
enum class EMantleCatalogFitPath : uint8
{
	/** FMantleGarmentFitter::FitToWearer: the warp, the body clearance restore, the wearer's skin weights, the rebind. */
	Wearer,
	/** FMantleGarmentFitter::Fit: the warp and the rebind only (hair, and garments the wearer path cannot take). */
	Warp,
};

/** What one FMantleCatalogGarmentFit::Fit did, for the log and the tests. */
struct FMantleCatalogFitResult
{
	EMantleCatalogFitPath Path = EMantleCatalogFitPath::Warp;

	/** Why the warp path was taken; empty on the wearer path. */
	FString WarpReason;

	/** The wearer path's report (default on the warp path). */
	FMantleGarmentFitReport Report;

	double Ms = 0.0;

	/** "wearer", or "warp (<why>)": the cosmetic cache's log word. */
	FString Describe() const
	{
		return Path == EMantleCatalogFitPath::Wearer ? FString(TEXT("wearer")) : FString::Printf(TEXT("warp (%s)"), *WarpReason);
	}
};

/**
 * The production fit of a catalog garment to a wearer (the pipeline is FMantleGarmentFitter::FitToWearer). The client's cosmetic cache runs both steps on background tasks and caches
 * them; the plugin's cosmetic preview runs the same steps, so a creator sees the fit players get:
 *
 *   DeriveAuthoredClearance(garment, source shape, topology)
 *       The garment's clearance over the body it was authored on. Everything it needs is already cooked: the
 *       garment's MSK1 and its source shape's MBS1 (the body's reference-topology positions, whose RBF weights
 *       the plugin computed at upload). The shape carries positions only, so the triangle list, and the vertex
 *       normals that orient each triangle, come from a wearer's reference-topology LOD0: every garment-compatible
 *       avatar has the same vertex order (UMantleBodyTopologyData), which is exactly what FitToWearer's restore
 *       and skin transfer already assume of the source body and the wearer. Once per (garment, source shape).
 *   Fit(garment, field, wearer, authored)
 *       FitToWearer when the garment tracks the body (the cache passes the authored clearance for top, bottom and
 *       footwear), the clearance covers every LOD of the garment (A3.8: DeriveAuthoredClearance measures each LOD)
 *       and the wearer's LOD0 has the triangles the clearance was measured over; otherwise the plain warp and
 *       rebind (FMantleGarmentFitter::Fit, which warps every LOD), with the reason. Hair and
 *       the face slots always take the warp: their vertices sit on the head, which the body does not have, and
 *       the transfer would hand them the neck's weights.
 *
 * The collar's half of the fit (section 0.1 item 12; #430) runs when the faces are there, each part on its own:
 *   - the neck samples: when the source shape carries a neck (MBS1's NECK block, the plugin's ComputeNeck at upload)
 *     and ComputeField is given the wearer's face, the field is computed through the neck samples of both faces
 *     (FMantleNeckSamples::BuildWearer), so a collar follows the wearer's neck instead of being extrapolated from
 *     the shoulders;
 *   - the face clearance restore: when DeriveAuthoredClearance is given the garment's source face (the item's
 *     faceMeshAssetId) and Fit the wearer's face, FitToWearer restores each collar vertex to the clearance it was
 *     authored with, measured on the wearer's face.
 * Without them (a garment without them, a wearer without a face), every step is exactly the body-only fit.
 *
 * No UObject. Any thread.
 */
struct MANTLERUNTIME_API FMantleCatalogGarmentFit
{
	/**
	 * Out = Garment's authored clearance over Source's positions on Topology's LOD0 triangle list and vertex normals,
	 * for every LOD of the garment (A3.8), and over SourceFace when given (#430: the face the garment was authored
	 * with, the item's faceMeshAssetId at its FMantleFaceTopology::FitLOD; the collar's clearance, Out.Face).
	 * Also Out.SkinMask (#877: FMantleSkinMask::Derive on the source body; null, with a warning, when it cannot be derived). False,
	 * with the reason, when Topology has no LOD0, its vertex count or normals do not match Source's positions, or the
	 * clearance refuses the garment.
	 */
	static bool DeriveAuthoredClearance(const FMantleSkinnedMesh& Garment, const FMantleBodyShape& Source, const FMantleSkinnedMesh& Topology,
		FMantleGarmentAuthoredClearance& Out, FString* OutError = nullptr, const FMantleSkinnedMeshLOD* SourceFace = nullptr);

	/** Authored was measured over WearerBody's LOD0 triangle list (the same vertex count and indices). */
	static bool SharesSurface(const FMantleGarmentAuthoredClearance& Authored, const FMantleSkinnedMesh& WearerBody);

	/**
	 * Garment fitted to WearerBody through Field (from the garment's source shape onto WearerBody's LOD0), rebound
	 * to WearerBody's reference pose. Authored null: the warp path. WearerFace (#430, the wearer's face at its
	 * FitLOD): on the wearer path, with an Authored that has its face clearance, the collar is restored to that
	 * clearance on it; with WearerFaceBones (the face's bone table) the collar also takes the face's skin weights
	 * (FMantleGarmentSkinTransfer::Transfer). False (OutFitted unspecified), with the reason, when the chosen path fails; the wearer path
	 * never falls back to the warp on a failure.
	 */
	static bool Fit(const FMantleSkinnedMesh& Garment, const FMantleWarpField& Field, const FMantleSkinnedMesh& WearerBody,
		const FMantleGarmentAuthoredClearance* Authored, FMantleSkinnedMesh& OutFitted, FString* OutError = nullptr,
		FMantleCatalogFitResult* OutResult = nullptr, const FMantleSkinnedMeshLOD* WearerFace = nullptr,
		TConstArrayView<FMantleSkinnedMeshBone> WearerFaceBones = {});

	/**
	 * The warp field from Source onto WearerBody's LOD0 positions (both the reference topology; the fitter refuses
	 * otherwise): the cosmetic cache's default field. #430: when Source carries a neck and WearerFace is given, the
	 * field runs through the neck samples of both faces (Field->NumNeckSamples; OutNeck, when given, says how they
	 * were made); when the wearer's face cannot take the shape's neck (FMantleNeckSamples::BuildWearer), from the
	 * body alone, as without either. Null with the reason when WearerBody has no LOD0 or the fitter refuses.
	 */
	static TSharedPtr<const FMantleWarpField> ComputeField(const FMantleBodyShape& Source, const FMantleSkinnedMesh& WearerBody, FString& OutError,
		const FMantleSkinnedMeshLOD* WearerFace = nullptr, FMantleNeckSamples::FResult* OutNeck = nullptr);

	/**
	 * #430, the upload side (the plugin's export and preview): Out = the neck of Shape (reference-topology positions)
	 * measured on SourceFace, the face the body was authored with (FMantleNeckSamples::BuildSource), with the RBF
	 * weights over Shape's positions followed by the samples at PointCount interpolation points (seconds: never on a
	 * client). False, with the reason, when the face gives no samples or the weights do not generate.
	 */
	static bool ComputeNeck(const FMantleBodyShape& Shape, const FMantleSkinnedMeshLOD& SourceFace, int32 PointCount, FMantleBodyShapeNeck& Out,
		FString* OutError = nullptr, double* OutMs = nullptr);

	/**
	 * True for the API slots whose garment is fitted to the body (FitToWearer, with the authored clearance): "top",
	 * "bottom" and "footwear". Hair and the face slots sit on the head, which the body does not have, and only warp.
	 */
	static bool TracksBody(const FString& ApiSlot);
};
