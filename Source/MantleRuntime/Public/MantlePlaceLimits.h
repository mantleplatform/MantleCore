// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/**
 * The limits schema/place.schema.json puts on values a creator controls, in one place for the
 * parser and the editor's "Check this place" (#393), so Check refuses what Publish refuses.
 *
 * The schema is the contract. The backend's TestPlaceLimitsMatchSchemaAndPlugin
 * (backend/internal/places) reads both this header and the schema, so the plugin, the parser and
 * the backend cannot drift apart. Keep each constant on one line in the form the Go test reads:
 * static constexpr <type> <Name> = <number>;
 */
struct MANTLERUNTIME_API FMantlePlaceLimits
{
	/** metadata.name: the place's name (the level's name when published from the editor), 1 to this many characters. */
	static constexpr int32 MaxPlaceNameLength = 80;

	/** revision.notes. */
	static constexpr int32 MaxRevisionNotesLength = 2000;

	/** entities[].name. */
	static constexpr int32 MaxEntityNameLength = 80;

	/** entities[].tags[]. */
	static constexpr int32 MaxTagLength = 40;

	/** An entity's interactable block (schema/place.schema.json): the parts, the options of a list (one per key),
	 *  the entities a part's outline draws, and the ranges. */
	static constexpr int32 MaxInteractableParts = 16;
	static constexpr int32 MaxInteractableOptions = 2;
	static constexpr int32 MaxInteractableHighlights = 16;
	static constexpr double MinInteractableRange = 50.0;
	static constexpr double MaxInteractableRange = 5000.0;

	/** StaticMesh, InstancedMesh and SkeletalMesh materials[]. */
	static constexpr int32 MaxMaterialSlots = 16;

	/** StaticMesh and InstancedMesh drawDistance (#630 stage 2): above 0 and at most this many cm (10 km). */
	static constexpr double MaxDrawDistance = 1000000.0;

	/** SkeletalMesh playRate, 0 (hold the pose at startOffset) to this many clip seconds per server second (#469). */
	static constexpr double MaxClipPlayRate = 10.0;

	/** SkeletalMesh startOffset, 0 to this many seconds into the clip (#469). */
	static constexpr double MaxClipStartOffsetSeconds = 3600.0;

	/** InstancedMesh instances[], 1 to this many per entity (#467). Export cuts larger groups into several entities. */
	static constexpr int32 MaxInstancesPerEntity = 4096;

	/**
	 * InstancedMesh instances across a whole place (#467). The schema cannot express a place-wide
	 * total, so schema/validate.py's MAX_INSTANCES_PER_PLACE states it; the backend's Go test holds
	 * the two together. At about 55 bytes an instance this stays well inside the 8 MiB place file.
	 */
	static constexpr int32 MaxInstancesPerPlace = 100000;

	/**
	 * The place file's size in bytes (GhzGarage/mantle#818): the backend's places.MaxPlaceFileBytes (8 MiB), which refuses a bigger
	 * file at the draft save, after every asset is uploaded. Export measures the file against it so Publish can refuse first.
	 */
	static constexpr int64 MaxPlaceFileBytes = 8 * 1024 * 1024;

	/** DirectionalLight intensity, 0 to this. */
	static constexpr double MaxDirectionalLightIntensity = 200.0;

	/** PointLight, SpotLight and RectLight intensity, 0 to this, in any intensityUnits (#385, #472). */
	static constexpr double MaxLocalLightIntensity = 100000.0;

	/** SpotLight innerCone and outerCone, 0 to this many degrees. */
	static constexpr double MaxSpotConeDegrees = 90.0;

	/** A light's temperature in kelvin (#472): $defs.lightTemperature, the range FLinearColor::MakeFromColorTemperature accepts. */
	static constexpr double MinLightTemperature = 1000.0;
	static constexpr double MaxLightTemperature = 15000.0;

	/** PointLight and SpotLight sourceRadius, softSourceRadius and sourceLength, 0 to this many cm (#472): $defs.lightSourceSize, the engine's clamp. */
	static constexpr double MaxLightSourceSize = 10000.0;

	/** RectLight sourceWidth and sourceHeight, 0 to this many cm (#472), the engine's clamp. */
	static constexpr double MaxRectLightSourceSize = 100000.0;

	/** RectLight barnDoorAngle, 0 to this many degrees (#472). */
	static constexpr double MaxRectLightBarnDoorAngle = 90.0;

	/** RectLight barnDoorLength, 0 to this many cm (#472). */
	static constexpr double MaxRectLightBarnDoorLength = 100000.0;

	/** Decal sortOrder, from minus this to this (#472). */
	static constexpr int32 MaxDecalSortOrder = 1000;

	/** CrowdSpawner count, 1 to this many NPCs (#477). */
	static constexpr int32 MaxCrowdCount = 100;

	/** The sum of every CrowdSpawner's count in one place. Cross-entity: not in the schema itself. */
	static constexpr int32 MaxCrowdNpcsPerPlace = 200;

	/** CrowdSpawner entities in one place. Cross-entity: not in the schema itself. */
	static constexpr int32 MaxCrowdSpawnersPerPlace = 8;

	/** CrowdSpawner lanes[], 1 to this many. */
	static constexpr int32 MaxCrowdLanes = 16;

	/** One crowd lane's points[], 2 to this many. */
	static constexpr int32 MaxCrowdLanePoints = 64;

	/** A crowd lane's width, 0 to this many cm. */
	static constexpr double MaxCrowdLaneWidth = 2000.0;

	/** CrowdSpawner looks[], 0 to this many. */
	static constexpr int32 MaxCrowdLooks = 16;

	/** One crowd look's wearing[], 0 to this many (avatar descriptor v2's replicated budget). */
	static constexpr int32 MaxCrowdLookWearing = 24;

	/** CrowdSpawner variation.skinTones[], 1 to this many. */
	static constexpr int32 MaxCrowdSkinTones = 32;

	/** CrowdSpawner walkSpeed bounds, cm/s. */
	static constexpr double MinCrowdWalkSpeed = 50.0;
	static constexpr double MaxCrowdWalkSpeed = 300.0;

	/** Crowd heightPct bounds, avatar descriptor v2's body.heightPct. */
	static constexpr int32 MinCrowdHeightPct = 95;
	static constexpr int32 MaxCrowdHeightPct = 105;

	/** CrowdSpawner seed, 0 to this. */
	static constexpr int32 MaxCrowdSeed = 2147483647;

	/** CrowdSpawner pointsOfInterest[], 0 to this many (#513). */
	static constexpr int32 MaxCrowdPointsOfInterest = 16;

	/** A point of interest's lingerSeconds bounds, seconds. */
	static constexpr double MinCrowdPointLingerSeconds = 1.0;
	static constexpr double MaxCrowdPointLingerSeconds = 120.0;

	/** How far from a point of interest NPCs stand, 0 to this many cm. */
	static constexpr double MaxCrowdPointRadius = 1000.0;

	/** A StaticMesh's physics.mass, this many kg to MaxPhysicsMass (#456). The exporter clamps the editor's mass into range. */
	static constexpr double MinPhysicsMass = 0.1;
	static constexpr double MaxPhysicsMass = 100000.0;

	/** physics.linearDamping and physics.angularDamping, 0 to this. */
	static constexpr double MaxPhysicsDamping = 100.0;

	/**
	 * StaticMesh entities with a physics block in one place (#456). Each is a server-simulated,
	 * always-relevant replicated actor, so this bounds server physics and replication cost.
	 * Cross-entity: not in the schema itself; validate.py's MAX_PHYSICS_OBJECTS_PER_PLACE and the
	 * backend state it too. "Check this place" warns past WarnPhysicsObjectsPerPlace.
	 */
	static constexpr int32 MaxPhysicsObjectsPerPlace = 500;
	static constexpr int32 WarnPhysicsObjectsPerPlace = 200;

	/** Terrain layers[], 1 to this many (#466, #628): two weightmap textures' four channels each. */
	static constexpr int32 MaxTerrainLayers = 8;

	/** A Terrain layer's tiling, above 0 and at most this many cm per repeat. */
	static constexpr double MaxTerrainTilingCm = 100000.0;

	/** Terrain entities in one place (#466). Cross-entity: validate.py's MAX_TERRAINS_PER_PLACE states it. */
	static constexpr int32 MaxTerrainsPerPlace = 1;

	/** Terrain grass[], 0 to this many entries (#628). */
	static constexpr int32 MaxTerrainGrass = 32;

	/** A Terrain grass entry's density, above 0 and at most this many instances per square metre (#628). */
	static constexpr double MaxTerrainGrassDensity = 100.0;

	/** A Terrain grass entry's cullDistance, each at most this many cm (#628). */
	static constexpr double MaxTerrainGrassCullDistance = 50000.0;

	/** Distinct grass maps (textures) across one Terrain's grass[] (#628). Cross-entity: validate.py and the backend state it too. */
	static constexpr int32 MaxTerrainGrassMaps = 4;

	/** VehicleSpawn respawnSeconds, 0 to this (#435). */
	static constexpr double MaxVehicleRespawnSeconds = 600.0;

	/** VehicleSpawn entities in one place (#435): each is a simulated vehicle everywhere. Cross-entity: not in the schema itself. */
	static constexpr int32 MaxVehicleSpawnsPerPlace = 32;

	/** WeaponSpawn respawnSeconds, 0 to this (#789). */
	static constexpr double MaxWeaponRespawnSeconds = 600.0;

	/** WeaponSpawn entities in one place (#789). Cross-entity: the schema states it in prose. */
	static constexpr int32 MaxWeaponSpawnsPerPlace = 32;

	/** Seats in one place (GhzGarage/MantleCore#142). Cross-entity: the schema states it in prose. */
	static constexpr int32 MaxSeatsPerPlace = 256;

	/** A seat's hip position, each axis in centimetres from its entity's origin (#142). */
	static constexpr double MaxSeatOffset = 100000.0;

	/** ScriptPoint entities in one place (#838). Cross-entity: the schema states it in prose. */
	static constexpr int32 MaxScriptPoints = 64;

	/** A ScriptPoint's parameters (#838): at most this many keys. */
	static constexpr int32 MaxScriptPointParameters = 16;

	/** A ScriptPoint parameter key's length (#838): a letter, then up to 31 more. IsScriptPointParameterName implements it. */
	static constexpr int32 MaxScriptPointParameterNameLength = 32;

	/** A ScriptPoint parameter value's length in characters (#838). */
	static constexpr int32 MaxScriptPointParameterValueLength = 256;

	/** A ScriptPoint parameter key's pattern, as the schema and the backend write it. */
	static constexpr const TCHAR* ScriptPointParameterNamePattern = TEXT("^[A-Za-z][A-Za-z0-9_]{0,31}$");

	/** Files a stored script package may carry (#838), as the backend caps a version; the client keeps no more. */
	static constexpr int32 MaxScriptPackageFiles = 64;

	/**
	 * A storage entry's settings (#838 stage 6): at most this many keys, each a ScriptPointParameterNamePattern name, each value
	 * at most MaxStorageSettingValueLength characters. A manifest declares at most this many settings too.
	 */
	static constexpr int32 MaxStorageSettings = 16;
	static constexpr int32 MaxStorageSettingValueLength = 256;

	/** A choice option's options list (#838 stage 6): 2 to 32 entries, each 1 to 64 characters. */
	static constexpr int32 MinScriptChoiceOptions = 2;
	static constexpr int32 MaxScriptChoiceOptions = 32;
	static constexpr int32 MaxScriptChoiceOptionLength = 64;

	/** True when Name matches ScriptPointParameterNamePattern. */
	static bool IsScriptPointParameterName(const FString& Name);

	/** A weapon document's materials[] (#789): one name per mesh slot, at most this many. */
	static constexpr int32 MaxWeaponMaterials = 64;

	/** An Effect's size (#460): particle size, this many cm to MaxEffectSize. */
	static constexpr double MinEffectSize = 1.0;
	static constexpr double MaxEffectSize = 1000.0;

	/** An Effect's rate, 0 to this many particles a second (#460). With MaxEffectLifetime, at most 2000 live particles an effect. */
	static constexpr double MaxEffectRate = 200.0;

	/** An Effect's speed, 0 to this many cm/s (#460). */
	static constexpr double MaxEffectSpeed = 5000.0;

	/** An Effect's lifetime, this many seconds to MaxEffectLifetime (#460): how long each particle lives. */
	static constexpr double MinEffectLifetime = 0.05;
	static constexpr double MaxEffectLifetime = 10.0;

	/** Effect entities in one place (#460): each is a particle system on every client. Cross-entity: not in the schema itself. */
	static constexpr int32 MaxEffectsPerPlace = 64;

	/** A sprites Effect's sizeMin and sizeMax (#628): this many cm to MaxEffectSize. */
	static constexpr double MinSpriteSize = 0.1;

	/** A sprites Effect's sizeEndScale, 0 to this many times its starting size (#628). */
	static constexpr double MaxSpriteEndScale = 10.0;

	/** A sprites Effect's velocityMin, velocityMax and acceleration, each axis -this to this cm/s or cm/s^2 (#628). */
	static constexpr double MaxSpriteVector = 5000.0;

	/** A sprites Effect's maxParticles, 1 to this many alive at once (#628). */
	static constexpr int32 MaxSpriteParticles = 2000;

	/** A sprites Effect's subImages, each of columns and rows 1 to this many (#628). */
	static constexpr int32 MaxSpriteSubImages = 16;

	/** A sprites Effect's spawn shape: a half extent, radius or height of at most this many cm (#628). */
	static constexpr double MaxSpriteShapeLength = 10000.0;

	/** A Scatter's seed (#530), 0 to this. */
	static constexpr int32 MaxScatterSeed = 2147483647;

	/** A Scatter's halfExtent (#530): each axis at most this many cm (10 km); X and Y above 0, Z 0 or more. */
	static constexpr double MaxScatterHalfExtent = 1000000.0;

	/** A Scatter's scale [min, max] (#530): each this to MaxScatterScale, the fixed-point scale's range (thousandths in 16 bits). */
	static constexpr double MinScatterScale = 0.001;
	static constexpr double MaxScatterScale = 65.535;

	/** A Scatter's maxTilt, 0 to this many degrees (#530). */
	static constexpr double MaxScatterTilt = 90.0;

	/** A Scatter's meshes[], 1 to this many (#530). */
	static constexpr int32 MaxScatterMeshes = 16;

	/** A Scatter mesh's weight, 0 to this (#530); at least one mesh of a Scatter weighs more than 0. */
	static constexpr int32 MaxScatterMeshWeight = 1000;

	/**
	 * Instances one Scatter makes (#530): its footprint in square metres times its density, rounded.
	 * Cross-field (halfExtent times density), so validate.py's MAX_SCATTER_INSTANCES states it.
	 */
	static constexpr int32 MaxScatterInstances = 200000;

	/**
	 * Scatter instances across a whole place (#530). Measured on the build PC: 100,000 generate in
	 * 1.3 ms and build into instanced meshes in 12.7 ms, so a place at the cap costs a client about
	 * 30 ms once, while it builds the place. Cross-entity: validate.py's MAX_SCATTER_INSTANCES_PER_PLACE.
	 */
	static constexpr int32 MaxScatterInstancesPerPlace = 200000;

	/** VehicleSpawn livery, -1 (the vehicle's default) to this (#488): limits.json placeAssets.livery.maxLiveries - 1. */
	static constexpr int32 MaxVehicleLiveryIndex = 7;

	/**
	 * Constraint entities in one place (#528): each is a joint the server simulates. Cross-entity: not
	 * in the schema itself; validate.py's MAX_CONSTRAINTS_PER_PLACE and the backend state it too, and
	 * a server script's Constraints:Create counts toward it. "Check this place" warns past
	 * WarnConstraintsPerPlace.
	 */
	static constexpr int32 MaxConstraintsPerPlace = 200;
	static constexpr int32 WarnConstraintsPerPlace = 100;

	/** A hinge's limits (#528): [min, max] degrees with minus this <= min <= 0 <= max <= this. */
	static constexpr double MaxHingeAngle = 180.0;

	/** A rope's length (#528): above 0 and at most this many cm. */
	static constexpr double MaxRopeLength = 100000.0;

	/** A Constraint's breakForce (#528): above 0 and at most this many newtons. */
	static constexpr double MaxConstraintBreakForce = 10000000.0;

	/**
	 * ReflectionProbe entities in one place (#628): each is a cubemap (about 1 MiB at the project's 128
	 * pixel capture size) captured on every client. Cross-entity: not in the schema itself; validate.py's
	 * MAX_REFLECTION_PROBES_PER_PLACE and the backend state it too.
	 */
	static constexpr int32 MaxReflectionProbesPerPlace = 64;

	/** A box probe's halfExtent, each axis this many cm to MaxReflectionProbeExtent (#628). */
	static constexpr double MinReflectionProbeExtent = 1.0;
	static constexpr double MaxReflectionProbeExtent = 100000.0;

	/** A sphere probe's influenceRadius, cm (#628): the engine's own limits for a sphere capture. */
	static constexpr double MinReflectionProbeRadius = 8.0;
	static constexpr double MaxReflectionProbeRadius = 16384.0;

	/** A box probe's transitionDistance, cm (#628). */
	static constexpr double MinReflectionProbeTransition = 1.0;
	static constexpr double MaxReflectionProbeTransition = 1000.0;

	/** A probe's brightness, 0 to this (#628). skylightScale is 0 to 1. */
	static constexpr double MaxReflectionProbeBrightness = 8.0;

	/**
	 * A Text entity's text (GhzGarage/mantle#724), 1 to this many code points (FMantlePlaceParser::CodePointLength).
	 * Proposed by the plugin exporter; the schema's maxLength and the backend must state the same number.
	 */
	static constexpr int32 MaxTextLength = 512;

	/** Text entities in one place (#724). Cross-entity: not in the schema itself; validate.py and the backend state it too. */
	static constexpr int32 MaxTextsPerPlace = 256;

	/** A Text entity's worldSize, the glyph height in cm (#724): 1 cm to 20 m. */
	static constexpr double MinTextWorldSize = 1.0;
	static constexpr double MaxTextWorldSize = 2000.0;

	/** A Text entity's xScale and yScale (#724). */
	static constexpr double MinTextScale = 0.1;
	static constexpr double MaxTextScale = 10.0;

	/** WaterBody entities in one place (GhzGarage/mantle#997). Cross-entity: not in the schema itself; validate.py and the backend state it too. */
	static constexpr int32 MaxWaterBodiesPerPlace = 64;

	/** Spline points on one WaterBody (#997): the schema's maxItems. */
	static constexpr int32 MaxWaterSplinePointsPerBody = 256;

	/** Spline points across all of a place's WaterBody entities (#997). Cross-entity, like MaxWaterBodiesPerPlace. */
	static constexpr int32 MaxWaterSplinePointsPerPlace = 4096;

	/** Gerstner waves on one WaterBody (#997): the schema's maxItems. */
	static constexpr int32 MaxWaterWavesPerBody = 64;

	/** A river's spline is open and needs this many points; a lake's or ocean's is a closed loop and needs MinWaterLoopPoints (#997). */
	static constexpr int32 MinWaterRiverPoints = 2;
	static constexpr int32 MinWaterLoopPoints = 3;

	/** A spline point's depth, cm (#997). */
	static constexpr double MinWaterSplineDepth = 0.0;
	static constexpr double MaxWaterSplineDepth = 100000.0;

	/** A river spline point's width, cm, and velocity, cm/s (#997). */
	static constexpr double MinWaterRiverWidth = 1.0;
	static constexpr double MaxWaterRiverWidth = 100000.0;
	static constexpr double MaxWaterRiverVelocity = 10000.0;

	/** A Gerstner wave's wavelength and amplitude, cm, and phaseOffset's magnitude (#997). */
	static constexpr double MinWaterWavelength = 1.0;
	static constexpr double MaxWaterWavelength = 100000.0;
	static constexpr double MaxWaterAmplitude = 10000.0;
	static constexpr double MaxWaterPhaseOffset = 1000000.0;

	/** A WaterBody's collisionHeightOffset magnitude, cm (#997). */
	static constexpr double MaxWaterCollisionShift = 100000.0;

	/** An ocean's collisionExtent and oceanExtent, and the Water Zone's zoneExtent: each axis, cm (#997). */
	static constexpr double MinWaterExtent = 1.0;
	static constexpr double MaxWaterExtent = 409600.0;

	/** The Water Zone's size when settings.water leaves zoneExtent out, each axis, cm (#997). */
	static constexpr double DefaultWaterZoneExtent = 51200.0;

	/** scripts[].name, 1 to this many characters. */
	static constexpr int32 MaxScriptNameLength = 80;

	/** scripts[].path, relative to Scripts/. */
	static constexpr int32 MaxScriptPathLength = 200;

	/** scripts[].path's pattern, as the schema and the backend write it. IsScriptPath implements it. */
	static constexpr const TCHAR* ScriptPathPattern = TEXT("^(Server|Client|Shared)/[A-Za-z0-9_.-]+(?:/[A-Za-z0-9_.-]+)*$");

	/**
	 * True when Path matches ScriptPathPattern: exactly Server/, Client/ or Shared/, then one or
	 * more non-empty segments of ASCII letters, digits, '_', '.' and '-'. On success OutTopFolder
	 * is the top folder. The length limit is separate (MaxScriptPathLength).
	 */
	static bool IsScriptPath(const FString& Path, FString* OutTopFolder = nullptr);

	/** A character a script path segment may hold. */
	static bool IsScriptPathCharacter(TCHAR Character);

	/** "src_" and 32 lowercase hex digits: a script source id, as a place's scripts[] carries. */
	static bool IsScriptSourceId(const FString& Id);

	/** uiDocuments[] (#549): RmlUi documents and styles a place ships (entries with a source), 0 to this many. */
	static constexpr int32 MaxUIDocuments = 64;

	/** uiDocuments[] (#556 item 5): images a place's documents show (entries with an asset), 0 to this many. */
	static constexpr int32 MaxUIImages = 64;

	/** uiDocuments[] as a whole, the schema's maxItems: MaxUIDocuments + MaxUIImages. */
	static constexpr int32 MaxUIEntries = 128;

	/** uiDocuments[].path, relative to the level folder ("UI/..."), 1 to this many characters. */
	static constexpr int32 MaxUIDocumentPathLength = 200;

	/** One UI file's source, at most this many bytes (256 KiB), as the scripts store caps rml and rcss. */
	static constexpr int32 MaxUIDocumentBytes = 262144;

	/** One UI image file (#556 item 5), PNG or JPEG, at most this many bytes (2 MiB) before upload. */
	static constexpr int32 MaxUIImageBytes = 2097152;

	/**
	 * True when Path is a UI document path (#549): exactly "UI/", then one or more non-empty
	 * segments of script path characters, no "..", no backslash, and a file name ending in
	 * ".rml" or ".rcss" (case-exact) with something before the extension. The length limit is
	 * separate (MaxUIDocumentPathLength).
	 */
	static bool IsUIDocumentPath(const FString& Path);

	/** IsUIDocumentPath's rule for an image (#556 item 5): the file name ends in ".png" or ".jpg" (case-exact). */
	static bool IsUIImagePath(const FString& Path);

	/** The shared rule behind both: ExtensionLength is the length of the matched extension, 0 when none matched. */
	static bool IsUIPathWithExtension(const FString& Path, int32 ExtensionLength);

	/** True when Id is "uid_" followed by 1 to 40 of a-z, 0-9 and '_' (#549). */
	static bool IsUIDocumentId(const FString& Id);

	/** storage[] (#614 stage 4): the library items a place holds, 0 to this many. Mirrors the schema's maxItems. */
	static constexpr int32 MaxStorageEntries = 32;

	/** storage[].name's pattern, as the schema and the backend write it. IsStorageName implements it. */
	static constexpr const TCHAR* StorageNamePattern = TEXT("^[A-Za-z][A-Za-z0-9_]{0,63}$");

	/** True when Name matches StorageNamePattern: an ASCII letter, then up to 63 ASCII letters, digits or '_'. */
	static bool IsStorageName(const FString& Name);

	/** True when Id is a library item's id: "itm_u" followed by 32 lowercase hexadecimal digits (#614 stage 4). */
	static bool IsLibraryItemId(const FString& Id);
};
