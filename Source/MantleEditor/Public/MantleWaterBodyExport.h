// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantlePlaceTypes.h"
#include "UObject/WeakObjectPtrTemplates.h"

class AActor;
class UStaticMesh;
class UWorld;
class FJsonObject;
class FMantleCoverage;

/** Which kind of Water Body a level has (the Water plugin's Ocean, Lake, River and Custom body actors). */
enum class EMantleWaterBodyKind : uint8
{
	Ocean,
	Lake,
	River,
	Custom
};

/** One spline point of a water body: where it is and what the body's spline metadata says about the water there. */
struct MANTLEEDITOR_API FMantleWaterSplineSample
{
	/** In the spline's own space (the body's transform turns it into the world), as are the tangents. */
	FVector Location = FVector::ZeroVector;
	FVector ArriveTangent = FVector::ZeroVector;
	FVector LeaveTangent = FVector::ZeroVector;
	FRotator Rotation = FRotator::ZeroRotator;
	FVector Scale = FVector::OneVector;

	/** Linear, Curve, Constant, CurveClamped or CurveCustomTangent. */
	FString Type;

	/** The river's width from its centre in each direction (rivers only), and the water's depth, current and audio intensity there. */
	float Width = 0.f;
	float Depth = 0.f;
	float Velocity = 0.f;
	float AudioIntensity = 0.f;
};

/** One Gerstner wave, as the wave asset stores it once its generator has run. */
struct MANTLEEDITOR_API FMantleWaterWaveSample
{
	/** The wave's phase offset as the wave asset holds it. */
	float PhaseOffset = 0.f;
	float WaveLength = 0.f;
	float Amplitude = 0.f;
	float Steepness = 0.f;
	FVector Direction = FVector::ForwardVector;
};

/** The body's wave set: the wave source's class, the generator's settings and the waves it made. */
struct MANTLEEDITOR_API FMantleWaterWaves
{
	/** False for a body without waves. */
	bool bHasWaves = false;

	/** The wave source's class ("GerstnerWaterWaves"), and the wave asset's path when the body points at one. */
	FString WavesClass;
	FString AssetPath;

	/** The generator's class ("GerstnerWaterWaveGeneratorSimple" or "...Spectrum") and every setting on it, as the editor shows them. */
	FString GeneratorClass;
	TSharedPtr<FJsonObject> Generator;

	/** The waves the generator made (per wave: length, amplitude, steepness, direction), and the highest the set can rise. */
	TArray<FMantleWaterWaveSample> Waves;
	float MaxWaveHeight = 0.f;

	/** The body's own wave settings: the depth the waves fade out at, and the offset added to the highest wave. */
	float AttenuationDepth = 0.f;
	float MaxWaveHeightOffset = 0.f;
};

/** One material parameter the water look is made of. */
struct MANTLEEDITOR_API FMantleWaterMaterialParam
{
	FName Name;
	bool bColour = false;
	float Scalar = 0.f;
	FLinearColor Colour = FLinearColor::Black;
};

/** The body's water material: its name, its master, and the parameters that make up the look (absorption, scattering, colour, foam...). */
struct MANTLEEDITOR_API FMantleWaterLook
{
	FString MaterialName;
	FString MaterialPath;

	/** The material the instance chain ends in (the water master). */
	FString MasterName;

	/** The scalar and colour parameters whose names say they shape the look; the number of parameters the material has in all. */
	TArray<FMantleWaterMaterialParam> Params;
	int32 TotalParams = 0;

	/** The water master's look parameters ("Water Albedo", "Scattering", "Absorption"), when the material has them as colours. */
	TOptional<FLinearColor> Albedo;
	TOptional<FLinearColor> Scattering;
	TOptional<FLinearColor> Absorption;

	/** The other materials the body names: the river's transitions, the underwater post process and the static mesh stand-in (path names). */
	TMap<FString, FString> OtherMaterials;
};

/** The body's collision as set on it. */
struct MANTLEEDITOR_API FMantleWaterCollision
{
	/** NoCollision, QueryOnly, PhysicsOnly, QueryAndPhysics or QueryAndProbe. */
	FString Enabled;
	FString Profile;
	bool bGenerateOverlapEvents = false;

	/** Where the collision sits relative to the surface, and an ocean's collision box. */
	float HeightOffset = 0.f;
	FVector OceanCollisionExtents = FVector::ZeroVector;
};

/** Everything the exporter will need from one Water Body actor. */
struct MANTLEEDITOR_API FMantleWaterBodyData
{
	EMantleWaterBodyKind Type = EMantleWaterBodyKind::Lake;
	FString Name;
	FString Path;

	/** The body actor's world transform, and the spline component's (the actor's root in the Water plugin, so the same in practice). */
	FTransform Transform = FTransform::Identity;
	FTransform SplineTransform = FTransform::Identity;

	bool bClosedLoop = false;
	TArray<FMantleWaterSplineSample> Points;

	/** The spline metadata exists, so Depth, Velocity and AudioIntensity of the points are the body's own. */
	bool bHasMetadata = false;

	FMantleWaterWaves Waves;
	FMantleWaterLook Look;
	FMantleWaterCollision Collision;

	/** A custom body's mesh (asset path), and a lake's or custom body's override; empty for none. */
	FString MeshPath;
	TWeakObjectPtr<UStaticMesh> Mesh;

	/** The body carves the landscape (the Water brush) and the ground depth used where there is no spline depth. */
	bool bAffectsLandscape = false;
	double FixedWaterDepth = 0.0;

	/** An ocean's size on the water plane, and whether it follows the Water Zone. */
	FVector2D OceanExtents = FVector2D::ZeroVector;
	bool bOceanCentersOnZone = false;
};

/** The level's Water Zone: its extent and how finely the water mesh is tessellated. */
struct MANTLEEDITOR_API FMantleWaterZoneData
{
	FString Name;
	FString Path;
	FTransform Transform = FTransform::Identity;

	/** The zone's size in its own space, and the water info texture's resolution. */
	FVector2D Extent = FVector2D::ZeroVector;
	FIntPoint RenderTargetResolution = FIntPoint::ZeroValue;

	/** The water mesh: a tile's size and the tessellation of each, the level of detail scale, and the forced density level (-1 for none). */
	double TileSize = 0.0;
	int32 TessellationFactor = 0;
	float LodScale = 1.f;
	int32 ForceCollapseDensityLevel = -1;

	/** Tessellation only around the viewer, and the size of that window. */
	bool bLocalTessellation = false;
	FVector LocalTessellationExtent = FVector::ZeroVector;

	float CaptureZOffset = 0.f;
	bool bHalfPrecisionTexture = true;
	int32 VelocityBlurRadius = 0;
	int32 OverlapPriority = 0;
};

/** What a level's Water plugin actors hold. Pure data; the export writes it into the place as WaterBody entities and settings.water. */
struct MANTLEEDITOR_API FMantleWaterBodies
{
	/** The Water plugin's module is loaded in this editor. False means there is nothing to read, and not that the level has none. */
	bool bWaterPluginLoaded = false;

	TArray<FMantleWaterBodyData> Bodies;
	TArray<FMantleWaterZoneData> Zones;

	int32 Count(EMantleWaterBodyKind Type) const;

	/** "MANTLE_CHECK_WATER ocean=<n> lake=<n> river=<n> custom=<n> zones=<n> plugin=<1|0>", the line Mantle.Check logs. */
	FString SummaryLine() const;
};

/**
 * Reads the Water plugin's Water Bodies (Ocean, Lake, River, Custom) and Water Zone from a level (GhzGarage/mantle#997, stage 2).
 *
 * The Water plugin is Experimental and off by default, so MantleEditor does not link its module: a project without it must still
 * load Mantle. Every class and property is found by name through reflection (/Script/Water.*), and a level read with the plugin off
 * is simply empty (FMantleWaterBodies::bWaterPluginLoaded false). Nothing here is written into the place file until the place
 * format has a WaterBody entity.
 */
struct MANTLEEDITOR_API FMantleWaterBodyExport
{
	/** The Water plugin's classes are loaded in this editor. */
	static bool IsWaterPluginLoaded();

	/** Every Water Body actor and Water Zone in World. */
	static FMantleWaterBodies Read(UWorld* World);

	/** Whether Actor is a Water Body actor of the Water plugin (false when the plugin is not loaded). */
	static bool IsWaterBodyActor(const AActor* Actor);

	/** One Water Body actor; false when it is not one. */
	static bool ReadBody(const AActor* Actor, FMantleWaterBodyData& Out);

	/** The zone the place's settings.water describes: the highest overlap priority, else the first by path; null for none. */
	static const FMantleWaterZoneData* PrimaryZone(const FMantleWaterBodies& Water);

	/**
	 * Body as a WaterBody entity's properties (GhzGarage/mantle#997), in the actor's own space, held to the place format's ranges.
	 * Props.MeshId is left for the exporter (a custom body's mesh is an upload). False with OutWhy when the body cannot be carried
	 * (a custom body with no mesh, a spline with too few or too many points); OutNotes says what was clamped or left out.
	 */
	static bool BuildProps(const FMantleWaterBodyData& Body, FMantleWaterBodyProps& Props, FString& OutWhy, TArray<FString>& OutNotes);

	/** Zone as settings.water, its extent held to the format's range. */
	static FMantleWaterZone BuildZone(const FMantleWaterZoneData& Zone);

	/**
	 * Export's gate for one Water Body actor: reads it, and counts it against the place's limits (Bodies and SplinePoints are running
	 * totals). False, with a Dropped coverage line, when it is left out; otherwise an Approximated line says what it carries.
	 */
	static bool Admit(const AActor* Actor, const FString& Label, int32& Bodies, int32& SplinePoints, FMantleCoverage* Coverage);

	/** The data as one JSON object ({ "bodies": [...], "zones": [...] }), for the harness and for the writer to come. */
	static TSharedRef<FJsonObject> ToJson(const FMantleWaterBodies& Water);
	static FString ToJsonString(const FMantleWaterBodies& Water);

	/**
	 * How many Water Bodies carve Landscape: the body is set to affect the landscape (a Custom body never does) and its bounds
	 * overlap the landscape's on the ground plane. Zero when the Water plugin is not loaded.
	 */
	static int32 CountBodiesCarving(const AActor* Landscape);
};
