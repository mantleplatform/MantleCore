// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class AExponentialHeightFog;
class AStaticMeshActor;
class UWorld;
struct FMantleFogSettings;
struct FMantlePlace;
struct FMantleBuildResult;

/**
 * Gives every place a sky.
 *
 * FMantlePlaceBuilder spawns exactly the eight schema entity types, so a place with no
 * creator-authored sky content rendered against a black void. This adds the same
 * SkyAtmosphere/SkyLight/VolumetricCloud/ExponentialHeightFog cluster UE's Default level
 * template ships, purely as a runtime default — never serialized, never part of the schema.
 * `settings.skybox` stays reserved for a future creator-configurable sky; until then a place
 * that sets it gets this default sky too (#445).
 */
class MANTLERUNTIME_API FMantleEnvironment
{
public:
	/**
	 * Called by FMantlePlaceBuilder::Build once every entity's transform has been applied
	 * (after pass 3), unless the build opts out through FMantleBuildOptions::bRuntimeEnvironment
	 * (the importer, whose editable level keeps its authored sun). Starts the world's shared clock (UMantleEnvironmentSubsystem::StartClock)
	 * on every net mode, then, except on a dedicated server, spawns one transient,
	 * non-replicated, Movable set of the four sky actors into World and records them in
	 * Result.EnvironmentActors so FMantlePlaceBuilder::Teardown removes them with the place.
	 *
	 * Marks the place's first DirectionalLight entity — in entity array order, resolved through
	 * Result.Actors — as the atmosphere sun. A place with no DirectionalLight gets a synthesized
	 * default sun instead (also recorded in Result.EnvironmentActors), so the sky still renders.
	 * Either way the sun is then bound to the environment subsystem, which owns its angle from
	 * then on (the day/night cycle); the place's own sun keeps its intensity, colour and yaw.
	 *
	 * Also spawns the night sky (#445 ticket 4), the star dome and the moon (SpawnNightSkySphere),
	 * recorded in Result.EnvironmentActors too and handed to the subsystem with the sun, which
	 * turns, places and fades them with the clock. Either is left out when its assets are missing.
	 *
	 * On a dedicated server, where the sky is never rendered, only the clock is started.
	 */
	static void Apply(UWorld* World, const FMantlePlace& Place, FMantleBuildResult& Result);

	/**
	 * Sets the fog's density, height falloff, inscattering colour, start distance and directional inscattering colour from
	 * the keys the place sets (settings.environment.fog, #628); a key left unset keeps the fog's own value. Apply does it to
	 * the fog it spawns before the sky is bound, so the weather scales from these values. Does nothing for a null fog.
	 */
	static void ApplyFogSettings(AExponentialHeightFog* Fog, const FMantleFogSettings& Settings);

	/**
	 * Mantle's default fog, for a place that sets no fog of its own (MantleEnvironmentDefaults::DefaultFog*): thinner and
	 * taller than the engine's, starting a little way out.
	 */
	static FMantleFogSettings DefaultFogSettings();

	/**
	 * Spawns one body of the night sky: the engine sphere with a dynamic instance of
	 * MaterialPath, scaled to Radius (cm) and centred on the world. Like the rest of the sky it
	 * is transient, non-replicated and Movable; it also has no collision, casts no shadow, takes
	 * no part in distance-field or indirect lighting, and starts hidden until the environment
	 * subsystem gives it a fade above zero. The materials are two-sided, so the dome's inside
	 * draws with the camera within it.
	 *
	 * Returns null, having spawned nothing, on a dedicated server or when the sphere or the
	 * material cannot be loaded; each missing path is logged once per process.
	 */
	static AStaticMeshActor* SpawnNightSkySphere(UWorld* World, const TCHAR* MaterialPath, double Radius, const TCHAR* Label);

	/**
	 * The night sky's materials, unlit, additive and textureless, in /Game/Mantle/Materials/Sky
	 * (always cooked, so the editor preview shows the same sky). Created by
	 * tools/environment/Create-MantleSkyMaterials.py, whose docstring lists every parameter.
	 */
	static constexpr const TCHAR* StarDomeMaterialPath =
		TEXT("/Game/Mantle/Materials/Sky/M_MantleStarDome.M_MantleStarDome");
	static constexpr const TCHAR* MoonMaterialPath =
		TEXT("/Game/Mantle/Materials/Sky/M_MantleMoon.M_MantleMoon");

	/** The engine sphere both bodies use: 100 cm across. */
	static constexpr const TCHAR* NightSkySphereMeshPath = TEXT("/Engine/BasicShapes/Sphere.Sphere");

	/**
	 * The tag on every actor the environment spawns (the default sky, sun, star dome and moon), so
	 * the exporter never mistakes one for place content: the star dome and moon are Static Mesh
	 * Actors, which it would otherwise publish like any other.
	 */
	static constexpr const TCHAR* EnvironmentActorTag = TEXT("MantleEnvironment");

	/** The parameters the environment subsystem drives; the rest are look-dev values. */
	static constexpr const TCHAR* StarFadeParameter = TEXT("StarFade");
	static constexpr const TCHAR* MoonFadeParameter = TEXT("MoonFade");
	static constexpr const TCHAR* PhaseLightDirectionParameter = TEXT("PhaseLightDirection");

	/**
	 * The material Apply gives the default cloud: a child of the engine's default cloud
	 * instance with no overrides, so it looks the same. The engine component only soft-references
	 * its default, so nothing cooked the engine material and cooked clients had no clouds. This
	 * asset lives in /Game/Mantle/Materials, which is always cooked, and its hard parent reference
	 * cooks the engine material with it. Created by
	 * tools/environment/Create-MantleDefaultCloudMaterial.py.
	 */
	static constexpr const TCHAR* DefaultCloudMaterialPath =
		TEXT("/Game/Mantle/Materials/MI_MantleDefaultCloud.MI_MantleDefaultCloud");

	/**
	 * The default cloud material's parameters the weather drives (#446 ticket 2), read from the
	 * engine's m_SimpleVolumetricCloud asset, which MI_MantleDefaultCloud inherits unchanged:
	 * - Cloud_GlobalCoverage, scalar, default -0.2 (slider -2 to 2): added to the cloud layout;
	 * - Cloud_GlobalDensity, scalar, default 0.008: how opaque the clouds are;
	 * - StormClouds, scalar, default 0 (0 to 1): the blend to storm clouds, and the lerp alpha of
	 *   the lightning glow, so there is no glow at 0;
	 * - Storm_LightningColor, vector (2, 1.8, 2, 10): RGB the flash colour, A its brightness;
	 * - Storm_LightningAnim, vector (0, 0.1, 0, 1): R LightningFlicker, G DynamicLightingAnim (the
	 *   frames per second of the material's own lightning, on each machine's own clock), B
	 *   ManualLightningAnim (the frame, added to time times G), A unused.
	 */
	static constexpr const TCHAR* CloudCoverageParameter = TEXT("Cloud_GlobalCoverage");
	static constexpr const TCHAR* CloudDensityParameter = TEXT("Cloud_GlobalDensity");
	static constexpr const TCHAR* CloudStormParameter = TEXT("StormClouds");
	static constexpr const TCHAR* CloudLightningColorParameter = TEXT("Storm_LightningColor");
	static constexpr const TCHAR* CloudLightningAnimParameter = TEXT("Storm_LightningAnim");
};
