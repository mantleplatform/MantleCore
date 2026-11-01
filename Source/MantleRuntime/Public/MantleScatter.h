// Copyright Joshua Eger. All Rights Reserved.
// GhzGarage/mantle#530: the Scatter place entity's generator. One fixed, seeded CPU scatter, driven by
// a closed parameter set (seed, half extent, density, scale range, alignment and tilt, weighted meshes)
// and generated on each client into instanced static meshes (AMantleScatterVolume, MantleScatterVolume.h).
// Visual only: no collision, nothing replicated, nothing on a dedicated server.
//
// Plain C++, not the engine's PCG plugin: Mantle.uproject allows PCG in the Editor target only, so the
// shipped client has no PCG to run. It lives in MantleCore because the creator's editor previews a
// scatter with this same code.
//
// Determinism: every instance comes from its own counter-based random stream (SplitMix64 over the seed
// and the instance's index) and is computed in integer fixed point, so the same parameters give the
// same instances bit for bit on any machine, in any order, whatever the compiler does with floating
// point. Floats appear only when an instance becomes a transform for its mesh.

#pragma once

#include "CoreMinimal.h"
#include "MantlePlaceLimits.h"

#include "MantleScatter.generated.h"

class UWorld;
class FJsonObject;

/** How each instance is turned. The place schema's alignment enum, in this order. */
UENUM()
enum class EMantleScatterAlignment : uint8
{
	/** A random yaw only: every instance stands up straight. */
	Upright,
	/** A random yaw, plus pitch and roll each up to the max tilt either way. */
	Tilted,
	/** Any yaw, pitch and roll: rocks and debris with no up. */
	Tumbled,
	/**
	 * Terrain placement only (#188): each instance stands on the ground's slope, its up following the surface normal, with a random turn
	 * about that normal and a lean of up to the max tilt either way off it. On flat ground this is the same as Tilted.
	 */
	Ground
};

/** Where instances go: the box as it always was, or dropped onto the Terrain below each one (#169). The place schema's placement enum. */
UENUM()
enum class EMantleScatterPlacement : uint8
{
	Box,
	Terrain
};

/** The footprint instances are spread over (#169). The place schema's shape enum, in this order. */
UENUM()
enum class EMantleScatterShape : uint8
{
	/** The halfExtent box, as always. */
	Box,
	/** A disc of Radius about the origin. */
	Disc,
	/** A strip Width wide along a polyline of Path points. */
	Path
};

/** A box (X-Y only) no instance is kept inside (#169), cm in the scatter's own space. */
struct FMantleScatterExclusion
{
	FVector2D Center = FVector2D::ZeroVector;
	FVector2D HalfExtent = FVector2D(100.0, 100.0);
};

/** A strip (#190) no instance is kept inside: every point within half the Width of the polyline, cm in the scatter's own space. */
struct FMantleScatterPathExclusion
{
	TArray<FVector2D> Path;
	double Width = 100.0;
};

/**
 * One mesh a scatter draws from: a library or uploaded (#252) mesh id, its relative weight, and its
 * materials by slot as on a StaticMesh (an empty entry leaves the slot on the mesh's own material; an
 * uploaded mesh names one for every slot, since it carries none of its own).
 */
struct FMantleScatterMesh
{
	FString MeshId;
	int32 Weight = 1;
	TArray<FString> Materials;
};

/** The closed parameter set: the Scatter entity's properties. Creators pick these numbers and meshes, never a graph. */
struct FMantleScatterParams
{
	/** 0 to FMantlePlaceLimits::MaxScatterSeed. */
	int32 Seed = 0;

	/** Half the box instances are placed in, in cm, about the scatter's origin. Z may be 0 (a flat layer). */
	FVector HalfExtent = FVector(5000.0, 5000.0, 0.0);

	/** Instances per square metre of the box's X-Y footprint. */
	double Density = 0.2;

	/** Uniform scale, picked evenly between these. */
	double MinScale = 0.8;
	double MaxScale = 1.2;

	EMantleScatterAlignment Alignment = EMantleScatterAlignment::Upright;

	/** Tilted and Ground only: the largest pitch and roll either way, in degrees (0 to 90). For Ground the lean is off the surface normal. */
	double MaxTiltDegrees = 10.0;

	TArray<FMantleScatterMesh> Meshes;

	/** cm from the camera past which instances are not drawn (they fade over the last fifth); 0 never culls (#169). */
	double DrawDistance = 0.0;

	/** Terrain drops each instance onto the Terrain below its X-Y and drops those with none (a hole, or no Terrain). */
	EMantleScatterPlacement Placement = EMantleScatterPlacement::Box;

	/** Terrain only: instances on ground steeper than this many degrees from flat are dropped. 90 filters nothing. */
	double MaxSlopeDegrees = 90.0;

	/** Terrain only (#188): instances on ground flatter than this many degrees from flat are dropped, 0 to MaxSlopeDegrees. 0 filters nothing. */
	double MinSlopeDegrees = 0.0;

	/** Terrain only: the surface's world height, cm, must be at least MinHeight and at most MaxHeight when these are set. */
	TOptional<double> MinHeight;
	TOptional<double> MaxHeight;

	/**
	 * Terrain only (#187): the paint layer the filter reads, 0 to MaxTerrainLayers - 1 (the Terrain's layers[] index: weightmap channel
	 * R, G, B, A are layers 0 to 3, weightmap2's are 4 to 7); -1 for none. Needs MinLayerWeight or MaxLayerWeight. A layer the Terrain
	 * does not have weighs 0 everywhere, and a Terrain without a weightmap weighs layer 0 at 1 and every other at 0.
	 */
	int32 PaintLayer = -1;

	/** The layer's weight (0 to 1) must be at least MinLayerWeight and at most MaxLayerWeight when these are set. */
	TOptional<double> MinLayerWeight;
	TOptional<double> MaxLayerWeight;

	EMantleScatterShape Shape = EMantleScatterShape::Box;

	/** Disc: the radius, cm. */
	double Radius = 0.0;

	/** Path: the polyline's points, cm in the scatter's space, and the strip's full width, cm. */
	TArray<FVector2D> Path;
	double Width = 0.0;

	/** Path shape only (#190): above 0, instances sit every PathStep cm along the path (the first at its start), each turned to face along the line; Width then only spreads them across the strip, and Density and clusters are not used. 0 keeps them random along the strip. */
	double PathStep = 0.0;

	/** No instance is kept inside any of these. */
	TArray<FMantleScatterExclusion> Exclusions;

	/** No instance is kept inside any of these strips either (#190). */
	TArray<FMantleScatterPathExclusion> PathExclusions;

	/**
	 * Minimum distance between instances (#190), cm in X-Y; 0 keeps none apart. Instances are taken in index order and one is dropped when an
	 * earlier kept one is closer than this (exactly this apart is kept). Only the other filters' survivors count.
	 */
	double MinSpacing = 0.0;

	/** The instances block movement and shape navigation, on the server and every client (#169). Off: visual only. */
	bool bCollision = false;

	/** 0 spreads instances evenly; above 0 gathers them into this many clusters of ClusterRadius cm about centres picked in the shape. */
	int32 ClusterCount = 0;
	double ClusterRadius = 0.0;
};

/** What a Terrain says about the ground under a point (#169), in the scatter's own space. */
struct FMantleScatterGround
{
	/** The surface height, millimetres, rounded to whole mm. */
	int64 ZMm = 0;
	/** The surface normal's up component in world space, 0 to 1 (1 is flat): what the slope limits read. */
	double NormalZ = 1.0;
	/**
	 * The surface normal in the scatter's own space (unit length), what Ground alignment follows; the same surface as NormalZ. Each
	 * component is rounded to millionths before use, as NormalZ is. Up for a Ground function that does not set it.
	 */
	double LocalNormalX = 0.0;
	double LocalNormalY = 0.0;
	double LocalNormalZ = 1.0;
	/** The surface's height in world space, cm. */
	double WorldZCm = 0.0;
	/** The Scatter's paint layer's weight there, 0 to 255 (the weightmap's byte, nearest texel); only read when PaintLayer is set. */
	int32 LayerWeight = 0;
};

/** The ground under (X, Y) millimetres in the scatter's own space; false where there is none (a hole, off every Terrain). */
using FMantleScatterGroundFn = TFunction<bool(int64 XMm, int64 YMm, FMantleScatterGround& Out)>;

/** What a Generate call adds to the parameters. */
struct FMantleScatterGenerateOptions
{
	/** Terrain placement needs this; without it every instance of a Terrain scatter is dropped. */
	FMantleScatterGroundFn Ground;

	/** Only the first this many of the scatter's instance streams are generated (-1: all): the Foliage tier's prefix, so a lower tier is a subset of a higher one. */
	int64 MaxCandidates = -1;
};

/**
 * One generated instance in fixed point, in the scatter's own space: what two machines compare
 * instance by instance.
 */
struct FMantleScatterInstance
{
	/** Location in millimetres. */
	int32 X = 0;
	int32 Y = 0;
	int32 Z = 0;
	/** Hundredths of a degree: yaw 0 to 35999, pitch -9000 to 9000, roll -18000 to 17999. */
	uint16 Yaw = 0;
	int16 Pitch = 0;
	int16 Roll = 0;
	/** Thousandths: 1000 is scale 1. */
	uint16 Scale = 1000;
	/** Index into FMantleScatterParams::Meshes. */
	uint16 Mesh = 0;

	bool operator==(const FMantleScatterInstance& Other) const
	{
		return X == Other.X && Y == Other.Y && Z == Other.Z && Yaw == Other.Yaw && Pitch == Other.Pitch
			&& Roll == Other.Roll && Scale == Other.Scale && Mesh == Other.Mesh;
	}
	bool operator!=(const FMantleScatterInstance& Other) const { return !(*this == Other); }
};

class MANTLERUNTIME_API FMantleScatter
{
public:
	/** The caps: a request past any is refused with a message, never trimmed. The place limits' own. */
	static constexpr int32 MaxInstances = FMantlePlaceLimits::MaxScatterInstances;
	static constexpr int32 MaxMeshes = FMantlePlaceLimits::MaxScatterMeshes;
	static constexpr int32 MaxMeshWeight = FMantlePlaceLimits::MaxScatterMeshWeight;
	static constexpr double MaxHalfExtentCm = FMantlePlaceLimits::MaxScatterHalfExtent;
	static constexpr double MinScaleLimit = FMantlePlaceLimits::MinScatterScale;
	static constexpr double MaxScaleLimit = FMantlePlaceLimits::MaxScatterScale;
	static constexpr double MaxTiltLimit = FMantlePlaceLimits::MaxScatterTilt;
	static constexpr double MaxDrawDistanceCm = FMantlePlaceLimits::MaxDrawDistance;
	static constexpr int32 MaxPathPoints = FMantlePlaceLimits::MaxScatterPathPoints;
	static constexpr int32 MaxExclusions = FMantlePlaceLimits::MaxScatterExclusions;
	static constexpr int32 MaxClusters = FMantlePlaceLimits::MaxScatterClusters;
	static constexpr double MaxMinSpacingCm = FMantlePlaceLimits::MaxScatterMinSpacing;
	static constexpr double MinPathStepCm = FMantlePlaceLimits::MinScatterPathStep;
	static constexpr double MaxCoordinateCm = FMantlePlaceLimits::MaxScatterCoordinate;

	/** The place schema's spelling of an alignment: upright, tilted or tumbled. */
	static const TCHAR* AlignmentName(EMantleScatterAlignment Alignment);

	/** Reads the schema's spelling, case-exact. */
	static bool ParseAlignment(const FString& Name, EMantleScatterAlignment& Out);

	/** The schema's spelling of a placement (box, terrain) or a shape (box, disc, path), and the way back, case-exact. */
	static const TCHAR* PlacementName(EMantleScatterPlacement Placement);
	static bool ParsePlacement(const FString& Name, EMantleScatterPlacement& Out);
	static const TCHAR* ShapeName(EMantleScatterShape Shape);
	static bool ParseShape(const FString& Name, EMantleScatterShape& Out);

	/**
	 * What Settings > Foliage (sg.FoliageQuality, 0 Low to 3 Epic) does to a scatter, and to terrain grass: the share of
	 * each instance stream kept (a prefix of it) and the share of the drawDistance used. Never none at any tier.
	 */
	static double FoliageTierDensity(int32 FoliageQuality);
	static double FoliageTierDistance(int32 FoliageQuality);

	/**
	 * The number of instances Params asks for: the footprint in square metres times the density, rounded
	 * half up. The footprint is the box's (from the half extents in whole millimetres), a disc's (pi as
	 * 3141593 / 1000000 times the radius in whole mm squared) or a path's (the sum of its segments' lengths,
	 * each rounded to whole mm, times the width in whole mm). A path with a PathStep (#190) has the whole steps that fit in its length
	 * (in whole mm), plus 1, whatever the density. -1 with OutError when a parameter is
	 * out of range or the count passes MaxInstances. validate.py and the backend count the same way.
	 */
	static int64 CountFor(const FMantleScatterParams& Params, FString& OutError);

	/**
	 * Fills OutInstances (replacing its contents) or returns false with OutError. Pure: no world,
	 * no asset loading, no console variable; the same Params always give the same instances.
	 */
	static bool Generate(const FMantleScatterParams& Params, TArray<FMantleScatterInstance>& OutInstances, FString& OutError,
		const FMantleScatterGenerateOptions& Options = FMantleScatterGenerateOptions());

	/** The instance as a transform in the scatter's space (cm, degrees, uniform scale). */
	static FTransform ToTransform(const FMantleScatterInstance& Instance);

	/** FNV-1a over every field of every instance: one number two clients' logs can be compared by. */
	static uint64 Hash(TConstArrayView<FMantleScatterInstance> Instances);

	/**
	 * Params as a Scatter's place-file properties object, every key written (so FMantlePlaceParser::ParseScatterProperties
	 * reads it back to the same parameters). The way a server script's change of settings is overlaid and sent to clients.
	 */
	static TSharedRef<FJsonObject> ToJson(const FMantleScatterParams& Params);

	/**
	 * The place-wide Scatter caps over every Scatter of a place (null entries are skipped; one that cannot count is not counted):
	 * at most MaxScatterInstancesPerPlace instances in all, and at most MaxScatterCollidingInstancesPerPlace among the ones whose
	 * collision is on. False at the first Scatter that crosses either, its index in OutBadIndex and the parser's message in
	 * OutError. The place parser and a script's change of a Scatter's settings both use it, so they refuse alike.
	 */
	static bool CheckPlaceTotals(TConstArrayView<const FMantleScatterParams*> All, int32& OutBadIndex, FString& OutError);

	/** Hash as 16 lowercase hex digits, the form logs and tests print. */
	static FString HashText(uint64 InHash);

	/** Whether World draws scatters: anything but a dedicated server. */
	static bool DrawsScatter(const UWorld* World);
};
