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

/** How each instance is turned. The place schema's alignment enum, in this order. */
UENUM()
enum class EMantleScatterAlignment : uint8
{
	/** A random yaw only: every instance stands up straight. */
	Upright,
	/** A random yaw, plus pitch and roll each up to the max tilt either way. */
	Tilted,
	/** Any yaw, pitch and roll: rocks and debris with no up. */
	Tumbled
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

	/** Tilted only: the largest pitch and roll either way, in degrees (0 to 90). */
	double MaxTiltDegrees = 10.0;

	TArray<FMantleScatterMesh> Meshes;
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

	/** The place schema's spelling of an alignment: upright, tilted or tumbled. */
	static const TCHAR* AlignmentName(EMantleScatterAlignment Alignment);

	/** Reads the schema's spelling, case-exact. */
	static bool ParseAlignment(const FString& Name, EMantleScatterAlignment& Out);

	/**
	 * The number of instances Params asks for: the footprint in square metres (from the half extents
	 * in whole millimetres) times the density, rounded half up. -1 with OutError when a parameter is
	 * out of range or the count passes MaxInstances. validate.py and the backend count the same way.
	 */
	static int64 CountFor(const FMantleScatterParams& Params, FString& OutError);

	/**
	 * Fills OutInstances (replacing its contents) or returns false with OutError. Pure: no world,
	 * no asset loading, no console variable; the same Params always give the same instances.
	 */
	static bool Generate(const FMantleScatterParams& Params, TArray<FMantleScatterInstance>& OutInstances, FString& OutError);

	/** The instance as a transform in the scatter's space (cm, degrees, uniform scale). */
	static FTransform ToTransform(const FMantleScatterInstance& Instance);

	/** FNV-1a over every field of every instance: one number two clients' logs can be compared by. */
	static uint64 Hash(TConstArrayView<FMantleScatterInstance> Instances);

	/** Hash as 16 lowercase hex digits, the form logs and tests print. */
	static FString HashText(uint64 InHash);

	/** Whether World draws scatters: anything but a dedicated server. */
	static bool DrawsScatter(const UWorld* World);
};
