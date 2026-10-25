// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantlePlaceTypes.h"

class AActor;
class FMantleCoverage;
class UObject;
class UPrimitiveComponent;
class USkyLightComponent;
class UTextureCube;
class UWorld;
struct FPostProcessSettings;

/**
 * Where the sun sits on Mantle's sky clock, for one level light (#628): the hour and heading that put the clock's sun
 * where the level's was.
 */
struct MANTLEEDITOR_API FMantleSunClock
{
	/** The hour (0 to 24) at which the clock's sun is at the level sun's elevation. */
	double Hour = 12.0;

	/** The heading's yaw in degrees (-180 to 180): turns the clock's sun to the level sun's side of the sky. */
	double HeadingYaw = 0.0;

	/** The level's sun is higher than the clock's sun ever gets, so the clock's highest was used. */
	bool bElevationClamped = false;

	/** The level sun's elevation above the horizon, as read, in degrees. */
	double ElevationDegrees = 0.0;
};

/**
 * What the editor's project says about exposure, which a level's post-process volume overrides (#628): whether the project's
 * default is an auto exposure (r.DefaultFeature.AutoExposure), the brightness range's units (r.DefaultFeature.AutoExposure.
 * ExtendDefaultLuminanceRange: EV100 when on, which the place does not carry) and the exposure compensation every
 * post-process setting starts from (r.DefaultFeature.AutoExposure.Bias), which a place's exposure bias is relative to.
 */
struct MANTLEEDITOR_API FMantleExposureProject
{
	bool bAuto = true;
	bool bExtendedRange = false;
	double EngineBias = 1.0;

	/** The running editor's project. */
	static FMantleExposureProject Current();
};

/**
 * Maps a level's sun, sky, fog, clouds and post-process onto what the place format already carries (#628), so the
 * preview and a published place keep the level's look instead of replacing it with Mantle's defaults.
 *
 * What the place can say about its look is little: the first DirectionalLight entity's intensity, colour and heading
 * (the sky clock owns its pitch), and settings.environment (the clock's hour and rate, a held weather, a colour tint,
 * saturation and bloom). Export calls Apply once the entities are built and before the coverage survey. It writes:
 *
 * - the sun's temperature into the colour of its entity (the place has no temperature for a directional light);
 * - its pitch as a clock hour and its yaw as the heading, with the clock stopped there, so the sun stands where the
 *   level's did (a level's sun does not move);
 * - the post-process volume's saturation, bloom and white balance as the place's saturation, bloom and colour tint;
 * - its exposure (metering method, compensation and brightness range) as the place's exposure: mode auto when the level
 *   meters automatically, which is the engine's default for a level with no volume or no override, with the bias relative to
 *   the project's own compensation and the range when the volume overrides one;
 * - the level's first height fog's density, height falloff, inscattering colours and start distance as the place's fog,
 *   the keys that differ from Mantle's own fog;
 * - the level's sky light as a scale on Mantle's own (settings.environment.skyLight.intensity, #722): the captured scene's
 *   average brightness against Mantle's sky at the clock's hour (SkyLightScaleFor), so a closed room is not lit by an
 *   outdoor sky;
 * - no direct sun (settings.environment.sun.direct false, #722) for a level whose spawn points are all in the shade of its own
 *   shadow-casting meshes (IsShadedFromSun): a sealed room with a baked sun, which the editor shows with no sun inside;
 * - the weather, held: the volumetric cloud's coverage as Clear, PartlyCloudy or Overcast, or Clear when the level has a sky
 *   (atmosphere, sky light or sun) but no cloud. A level with a sky of its own is never left to the natural weather, which
 *   would rain on a level that never did; the creator chooses another Weather in Mantle Place Settings.
 *
 * A value Mantle Place Settings sets explicitly wins over the level's actor: Place.Settings.Environment already holds
 * what the settings actor set (only what differs from the default is set), and Apply fills only the keys it left
 * unset. The sky light's colour, atmosphere, the fog's other settings and the post-process's contrast and the rest have no key in
 * the place, so they are not carried; the report says so. The exposure is written only for a level with an environment
 * of its own (a sun, sky, fog or post-process volume), as the weather is. Nothing is written for a target client that does not read
 * settings.environment (FMantleTargetFeatures).
 *
 * Every actor it looks at is reported in the coverage report: Kept when Mantle's own matches it (the actor is at its
 * defaults), Approximated with what was carried and what was not, Dropped when nothing of it could be. The coverage
 * survey leaves these actors, and the first sun's entity, to it (IsReported).
 */
class MANTLEEDITOR_API FMantleEnvironmentExport
{
public:
	/**
	 * Maps World's environment actors onto Place and reports them to Coverage. IdByActor is the export's actor to
	 * entity id table, which finds the actor behind the place's first DirectionalLight entity. Does nothing to the
	 * place when the level has none of these actors; the first sun's entity is still reported.
	 */
	static void Apply(UWorld* World, const TMap<const AActor*, FString>& IdByActor, FMantlePlace& Place, FMantleCoverage& Coverage);

	/**
	 * True for an actor Apply reports itself: a sky atmosphere, sky light, volumetric cloud, height fog or post-process
	 * volume the level placed (not one the environment spawned, which is transient and tagged). The coverage survey skips them.
	 */
	static bool IsReported(const AActor* Actor);

	/**
	 * The hour and heading that put the clock's sun in the direction the level's light shines toward the sun
	 * (TowardSun, a unit vector from the world to the sun). AuthoredYaw, the light's own yaw, picks between the two
	 * hours (morning and afternoon) that share an elevation: the one whose heading stays clearly nearer it, the morning when
	 * they are as near (which they are for a light whose yaw is the sun's azimuth). False when the sun
	 * is within a degree of the horizon or below it, which the clock's night owns.
	 */
	static bool SolveSunClock(const FVector& TowardSun, double AuthoredYaw, FMantleSunClock& Out);

	/**
	 * The place's exposure that a level's post-process settings amount to (Settings null: the level has no volume, so the
	 * project's default stands). False when there is nothing to write: a fixed exposure with no bias, which is the game's
	 * own. Carried says what Out holds, in plain words, and Notes what of the exposure was not carried and why.
	 *
	 * Auto when the volume overrides the metering method to anything but manual, or leaves it and the project's default is
	 * auto; fixed otherwise. The bias is the volume's compensation less the project's own (a volume that overrides none has
	 * the project's, so 0), clamped to the place's range. The brightness range is carried with auto, for each end the volume
	 * overrides, unless the project's units are EV100.
	 */
	static bool ExposureFor(const FPostProcessSettings* Settings, const FMantleExposureProject& Project, FMantleExposureSettings& Out,
		TArray<FString>& Carried, TArray<FString>& Notes);

	/**
	 * The scale (0 to 1) on Mantle's sky light that makes it as bright as a level's own, from the level's sky light's average
	 * brightness (LevelMeanLuminance: the captured sky's mean radiance, times the light's intensity and colour) and Mantle's sky
	 * at the clock's Hour: Mantle's mean is MantleSkyMeanLuminanceAtNoon at noon, falling with the sun's elevation to a
	 * floor at night. The ratio, clamped to 0..1: a sky light as bright as Mantle's open sky (or brighter) is 1, Mantle's own.
	 */
	static double SkyLightScaleFor(double LevelMeanLuminance, double Hour);

	/** Mantle's sky's mean luminance at Hour, in the units of SkyLightMeanLuminance. */
	static double MantleSkyMeanLuminance(double Hour);

	/**
	 * A sky light's average brightness: for a captured scene the mean radiance over the sphere (the zeroth spherical harmonic of
	 * the capture, taken from a one-off capture when the light's own is not readable), for a specified cubemap that cubemap's
	 * mean (CubemapMeanLuminance), as a luminance, times the light's intensity, indirect lighting intensity and colour. OutSource, when
	 * given, says in words where the mean was read from. Negative
	 * when it cannot be read (a capture is a render-thread result a headless export may not have).
	 */
	static double SkyLightMeanLuminance(USkyLightComponent* Component, FString* OutSource = nullptr);

	/**
	 * The mean luminance over the sphere of a cubemap (#722), linear (an sRGB 8-bit source is converted; RGBE, float and 16-bit
	 * sources are read as they are). Read from the texture's source data when it has it: six square faces (sampled on a grid of about 32
	 * texels a side, each weighted by the solid angle it covers) or one long-lat image, as an imported HDR panorama is kept (each row
	 * weighted by cos(latitude)), in G8, BGRA8, BGRE8, G16, RGBA16, R16F, RGBA16F, R32F or RGBA32F. Failing that, from a small mip of
	 * the built cube when it is uncompressed. The mean does not depend on the sky light's cubemap angle. With bLowerHemisphereBlack
	 * the lower half of the sphere is counted as black, as the sky light's Lower Hemisphere Is Black does. OutSource, when given, says
	 * where it was read from. Negative when it cannot be read: no cubemap, no readable data, or a block-compressed build alone.
	 */
	static double CubemapMeanLuminance(UTextureCube* Cubemap, bool bLowerHemisphereBlack, FString* OutSource = nullptr);

	/**
	 * True when the sun, shining from TowardSun (a unit vector from the world to the sun), is blocked at every one of
	 * Points (a point a player stands at; each is raised a little) by a mesh that casts a shadow in the place and is visible:
	 * the sun reaches none of them. False for no points, and for a point whose way to the sun is open, or blocked by
	 * something that would not shade it in Mantle (an invisible collision volume): a sun is never turned off on a doubt.
	 */
	static bool IsShadedFromSun(UWorld* World, const FVector& TowardSun, const TArray<FVector>& Points);

	/**
	 * Whether a mesh component casts a shadow in a place. Mantle lights everything dynamically, so a mesh the editor shades
	 * with a light's baked (static) shadow alone still casts one there: the flag is Cast Shadow with either of the dynamic or
	 * static shadows on. Without this a sealed room's ceiling could let the sun in.
	 */
	static bool CastsShadowInPlace(const UPrimitiveComponent* Component);

	/** The place's weather that has the cloud cover a cloud material's coverage value gives: Clear, PartlyCloudy or Overcast. */
	static EMantleWeatherState WeatherForCloudCoverage(double CloudGlobalCoverage);

	/**
	 * The sRGB colour tint (0xRRGGBB) that a post-process white balance at Kelvin amounts to: a temperature above the
	 * neutral 6500 K gives a warmer picture, below it a cooler one. 0xFFFFFF at the neutral temperature.
	 */
	static uint32 TintForWhiteBalance(double Kelvin);

	/**
	 * The names of Object's editable properties that differ from its archetype's (the actor class's defaults), as the
	 * editor shows them. Properties every scene component has (its place, mobility, visibility) are left out, and so are
	 * any in IgnoredNames.
	 */
	static TArray<FString> TunedProperties(const UObject* Object, const TArray<FName>& IgnoredNames = TArray<FName>());

	/** The post-process settings whose bOverride_ flag is set, by property name without the prefix ("AutoExposureBias"). */
	static TArray<FString> OverriddenSettings(const FPostProcessSettings& Settings);
};
