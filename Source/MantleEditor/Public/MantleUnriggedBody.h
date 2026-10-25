// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleAvatarConvertFixer.h"

class UDynamicMesh;
class USkeletalMesh;
class UStaticMesh;
struct FSkelMeshSection;

/**
 * #776: where the arms of a T-posed model without a skeleton bend, estimated from its vertices (FMantleUnriggedBody::
 * EstimateArmBend). The A-pose commit tears T-pose arms at the shoulders, so the arms are bent down before the conform.
 * Z is up and X runs shoulder to shoulder, as the conform reads a body; sizes are cm at a figure about ReferenceHeightCm tall
 * and follow the figure's height otherwise.
 */
struct MANTLEEDITOR_API FMantleArmBend
{
	/** The estimate could be made (the points have a height and a width). */
	bool bValid = false;

	/** The arms stick out sideways at shoulder height (FMantleUnriggedBody::TPoseMinSpan, TPoseTipMin, TPoseTipMax): bend them. */
	bool bTPose = false;

	/** The figure's lowest and highest points and the difference. */
	double Bottom = 0.0;
	double Top = 0.0;
	double Height = 0.0;

	/** The widest |X| of any point (a fingertip in a T-pose). */
	double MaxX = 0.0;

	/** The height of the arms: the median height of the points near that width. The bend pivots at this height. */
	double ArmZ = 0.0;

	/** The shoulder pivot's distance from the middle, either side. */
	double PivotX = 0.0;

	/** Height over FMantleUnriggedBody::ReferenceHeightCm: the falloff widths scale with it. */
	double Unit = 1.0;

	/** The arm height as a share of the figure's height (0 at its lowest point, 1 at its highest). */
	double ArmShare() const { return Height > 0.0 ? (ArmZ - Bottom) / Height : 0.0; }

	/** "T-pose: arms 0.78 of the height, span 0.46 of the height". */
	FString ToString() const;
};

/**
 * #776: what FMantleUnriggedBody::WeldVertices did to a model's surface. A triangle soup (every triangle its own three vertices,
 * as an OBJ round trip can leave a model) tears in the conform, which reads the surface's connections, so coincident vertices
 * are merged first; what is still loose after that is counted.
 */
struct MANTLEEDITOR_API FMantleUnriggedWeld
{
	/** The vertex count before and after the weld, and the triangle count (the weld keeps every triangle). */
	int32 VerticesBefore = 0;
	int32 VerticesAfter = 0;
	int32 Triangles = 0;

	/** Triangles that share no edge with any other after the weld, and the vertices they use. */
	int32 LooseTriangles = 0;
	int32 LooseVertices = 0;

	/** #165: vertices no triangle used, removed before the weld (FMantleUnriggedBody::RemoveUnusedVertices); not in VerticesBefore. */
	int32 UnusedRemoved = 0;

	/** More than FMantleUnriggedBody::MaxLooseVertexShare of the vertices are in loose triangles: the surface is loose pieces. */
	bool bLoosePieces = false;

	/** LooseVertices over VerticesAfter (0 for an empty mesh). */
	double LooseShare() const { return VerticesAfter > 0 ? static_cast<double>(LooseVertices) / VerticesAfter : 0.0; }
};

/**
 * What FMantleUnriggedBody::ReadSkinWeights read from a skeletal mesh's LOD 0 skin weights: whether its skeleton carries the
 * surface, or the mesh is a model with no usable weights (fitted to a MetaHuman body as a static mesh is).
 */
struct MANTLEEDITOR_API FMantleSkinWeightRead
{
	/** The vertices read (every section's soft vertices). */
	int32 Vertices = 0;

	/** Bones with a non-zero weight on any vertex. */
	int32 WeightedBones = 0;

	/** Bones that are some vertex's dominant influence (its largest weight; the first on a tie). */
	int32 DominantBones = 0;

	/** "1200 vertices, 3 bones weighted, 1 dominant". */
	FString ToString() const;
};

/** What FMantleUnriggedBody::MakePosedMesh made. */
struct MANTLEEDITOR_API FMantleUnriggedBodyResult
{
	/** The model frozen as the conform's input: scaled to the default height, arms bent down when it was in a T-pose. */
	TWeakObjectPtr<UStaticMesh> PosedMesh;

	/** The weld of coincident vertices, before the arm estimate. */
	FMantleUnriggedWeld Weld;

	/** The arm estimate, and how many vertices the bend moved (0 when the model was not in a T-pose). */
	FMantleArmBend Bend;
	int32 BentVertices = 0;

	/** The model's height before the scale (after the bend), the scale k about the origin, and the height after. */
	double HeightCm = 0.0;
	double InputScale = 1.0;
	double ScaledHeightCm = 0.0;

	/** Every asset made, for the converter to record, or to discard on failure. */
	TArray<TWeakObjectPtr<UObject>> Created;
};

/**
 * The "unrigged to Convert" route: a plain static mesh (or a skeletal mesh with no usable weights) has no skeleton for the
 * A-pose step to pose, so its geometry is bent into the conform's pose here instead (arms bent down), scaled to the default height, and handed to the conform as the posed mesh. Strip and the A-pose step skip it.
 */
class MANTLEEDITOR_API FMantleUnriggedBody
{
public:
	/** How far each arm bends down about its shoulder pivot (the T-pose to the conform template's A-pose). */
	static constexpr double BendDegrees = 45.0;

	/** The figure height the cm constants below were proven on; others scale by height over this. */
	static constexpr double ReferenceHeightCm = 165.0;

	/** The shoulder pivot's half-width as a share of the figure's height. */
	static constexpr double PivotShare = 0.10;

	/** Points wider than this share of the widest |X| are the arms' ends. */
	static constexpr double TipShare = 0.75;

	/** The bend fades in across the shoulder: from PivotX - FadeInnerCm to PivotX + FadeInnerCm, i.e. over 2 * FadeInnerCm. */
	static constexpr double FadeInnerCm = 7.0;

	/** Nothing at or below ArmpitDropCm under the arm height moves; the bend is whole from ArmpitDropCm - ArmpitFadeCm under it. */
	static constexpr double ArmpitDropCm = 17.0;
	static constexpr double ArmpitFadeCm = 8.0;

	/** A T-pose: the arms reach at least this share of the height either side of the middle... */
	static constexpr double TPoseMinSpan = 0.36;

	/** ...at a height between these shares of the figure's height (the shoulders sit near 0.8; a lowered or raised arm is not a T). */
	static constexpr double TPoseTipMin = 0.68;
	static constexpr double TPoseTipMax = 0.88;

	/** Vertices closer than this (cm, at the default height: 0.01 mm) are one vertex. Position alone decides; UVs and normals don't. */
	static constexpr double WeldToleranceCm = 0.001;

	/**
	 * After the weld, a model with more than this share of its vertices in triangles that share no edge with any other is loose
	 * pieces and is refused: the conform would tear it.
	 * Only a surface that is mostly loose is refused.
	 */
	static constexpr double MaxLooseVertexShare = 0.10;

	/**
	 * Merges Mesh's coincident vertices by position (Tolerance apart at most), joining every pair of matching open edges (Geometry
	 * Core's FMergeCoincidentMeshEdges, unambiguous pairs only, so no edge gets three triangles), then counts the triangles still
	 * loose. Positions don't move. Tested.
	 */
	static FMantleUnriggedWeld WeldVertices(UDynamicMesh& Mesh, double Tolerance = WeldToleranceCm);

	/**
	 * #165: removes Mesh's vertices that no triangle uses; how many. Epic's conform reads every vertex of the posed mesh, its render
	 * only the drawn ones, so a left-over one fails the conform's input check. Positions and triangles don't change. Tested.
	 */
	static int32 RemoveUnusedVertices(UDynamicMesh& Mesh);

	/** "This model's surface is made of loose pieces, so it can't be fitted. Re-export it with its vertices merged." */
	static FString LoosePiecesMessage();

	/** Smoothstep of T clamped to 0..1. Pure (tested). */
	static double SmoothStep(double T);

	/** The pivot, arm height and T-pose test for Points (a model's vertices, Z up). Pure (tested). */
	static FMantleArmBend EstimateArmBend(TConstArrayView<FVector> Points);

	/** How much of the bend P takes, 0 to 1: across the shoulder (X) times up from the armpit (Z). Pure (tested). */
	static double BendWeight(const FMantleArmBend& Bend, const FVector& P);

	/** P rotated Degrees * BendWeight down about its side's shoulder pivot, in the XZ plane; P itself when its weight is 0. Pure (tested). */
	static FVector BendPoint(const FMantleArmBend& Bend, const FVector& P, double Degrees = BendDegrees);

	/** Bends every vertex of Mesh with BendPoint; how many moved. */
	static int32 BendArms(UDynamicMesh& Mesh, const FMantleArmBend& Bend, double Degrees = BendDegrees);

	/** Whether Context.Parts[PartIndex] is a model without a skeleton Convert fits to a MetaHuman body (FMantleConvertPart::bUnrigged). */
	static bool IsUnriggedBody(const FMantleConvertContext& Context, int32 PartIndex);

	/** "This model has no skeleton, so we'll fit it to a MetaHuman body." */
	static FString PlainMessage();

	/** "Skeleton found, but no usable weights: we'll fit it to a MetaHuman body." */
	static FString NoUsableWeightsMessage();

	/**
	 * Fewer weighted bones than this and the skeleton can't carry a body: the bones the converter never inserts and every
	 * supported skeleton requires (FMantleAvatarRigMap::MustMapBones: pelvis, head, each side's arm, hand and leg bones).
	 */
	static int32 MinWeightedBones();

	/** Sections' skin weights (LOD 0's, NumBones the reference skeleton's raw bone count). Pure (tested). */
	static FMantleSkinWeightRead ReadSkinWeights(TConstArrayView<FSkelMeshSection> Sections, int32 NumBones);

	/** Mesh's LOD 0 skin weights (its imported model); empty when it has none. */
	static FMantleSkinWeightRead ReadSkinWeights(const USkeletalMesh& Mesh);

	/**
	 * Weights, not bone count: Read is of a mesh with no usable weights when every vertex's dominant influence is one bone,
	 * or fewer than MinWeights bones are weighted. Nothing read (no vertices) is not: there is nothing to decide on. Pure (tested).
	 */
	static bool HasNoUsableWeights(const FMantleSkinWeightRead& Read, int32 MinWeights = MinWeightedBones());

	/**
	 * The posed mesh for Part: its geometry (the static mesh, else the skeletal mesh's LOD 0 source), its coincident vertices
	 * welded (WeldVertices, WeldToleranceCm at the default height; refused with LoosePiecesMessage when still loose), arms bent when it is in a
	 * T-pose, scaled about the origin to DefaultHeightCm (k = FMantleAvatarHeight::ConformInputScale), into a new static mesh in
	 * Folder, each slot the source's material, k stored on it (FMantleAvatarHeight::InputScaleTag). False with why
	 * (OutResult.Created lists what was made).
	 */
	static bool MakePosedMesh(const FMantleConvertPart& Part, const FString& Folder, const FString& Name, double DefaultHeightCm,
		FMantleUnriggedBodyResult& OutResult, FString& OutError);
};

/**
 * MantleAvatarFixerIds::MHUnrigged (the Convert route): one line for a model without a skeleton; it takes Strip's and the
 * A-pose step's place (both skip it): Apply makes the posed mesh (FMantleUnriggedBody::MakePosedMesh) into <output>/Work and
 * records the work for the conform (FMantleConvertContext::MetaHuman, with the scale k and no head joint). A failed Apply
 * deletes what it made and leaves the context as it was; so does one that dirtied a protected package outside <output>/Work.
 */
class MANTLEEDITOR_API FMantleMetaHumanUnriggedFixer final : public IMantleAvatarFixer
{
public:
	virtual FName GetId() const override { return MantleAvatarFixerIds::MHUnrigged; }
	virtual void Analyze(const FMantleConvertContext& Context, TArray<FMantleConvertFix>& OutFixes) const override;
	virtual bool Apply(const FMantleConvertFix& Fix, FMantleConvertContext& Context, FString& OutError) const override;
};

/** Registers FMantleMetaHumanUnriggedFixer with FMantleAvatarFixers (MantleEditor's StartupModule). */
MANTLEEDITOR_API void RegisterMantleMetaHumanUnriggedFixer();
