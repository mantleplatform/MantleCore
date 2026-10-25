// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleEnvironmentOverride.h"
#include "MantlePlaceTypes.h"
#include "MantleSkyClock.h"
#include "MantleWeather.h"
#include "Subsystems/WorldSubsystem.h"
#include "MantleEnvironmentSubsystem.generated.h"

class ADirectionalLight;
class AExponentialHeightFog;
class APostProcessVolume;
class ASkyLight;
class AStaticMeshActor;
class AVolumetricCloud;
class UDirectionalLightComponent;
class UExponentialHeightFogComponent;
class UMaterialInstanceDynamic;
class USkyLightComponent;
class UStaticMeshComponent;
class UTexture;
class UVolumetricCloudComponent;
struct FMantleEnvironmentSettings;
struct FPostProcessSettings;

/** One lightning strike, as every client computes it from the weather seed (#446). */
DECLARE_MULTICAST_DELEGATE_OneParam(FMantleLightningStrikeDelegate, const FMantleLightningStrike& /*Strike*/);

/**
 * The sky actors FMantleEnvironment::Apply spawned (or found) for one place, handed to the
 * environment subsystem to drive. Clients only: a dedicated server spawns no sky.
 */
struct FMantleSkyBinding
{
	/** The atmosphere light: the place's own first DirectionalLight, or the synthesized default. */
	ADirectionalLight* Sun = nullptr;

	/** True when Sun is the place's own light: its yaw is the sun's heading. Otherwise +X. */
	bool bPlaceSun = false;

	/**
	 * The rest of the default sky cluster, which the weather drives (#446 ticket 2): the sky
	 * light's intensity, the fog's density and the cloud material, each relative to what it had
	 * when bound. Any of them may be null; the rest are still driven.
	 */
	ASkyLight* SkyLight = nullptr;
	AExponentialHeightFog* Fog = nullptr;
	AVolumetricCloud* Cloud = nullptr;

	/**
	 * The night sky (#445 ticket 4), from FMantleEnvironment::SpawnNightSkySphere: each carries a
	 * dynamic instance of its material. Either is null when its assets are missing.
	 */
	AStaticMeshActor* StarDome = nullptr;
	AStaticMeshActor* Moon = nullptr;

	/**
	 * The place's settings.environment.skyLight.intensity (#722): a scale, 0 to 1, on the sky light's own intensity,
	 * applied to its base before the weather's. An indoor level lowers it so its walls are not lit by the open sky.
	 */
	float SkyLightIntensityScale = 1.0f;

	/**
	 * The place's settings.environment.sun.direct false (#722): the sun and the moon light nothing in the place. The
	 * light keeps its intensity, so the atmosphere, the clouds and the sky light's capture of them are unchanged.
	 */
	bool bNoDirectSun = false;
};

/**
 * The shared environment of one world (#445): the time of day every player on a server sees.
 *
 * Exists on every net mode, the dedicated server included, so a script there can read the hour.
 * FMantleEnvironment::Apply starts the clock for the place it builds, before its own
 * dedicated-server early return; nothing replicates for it (see FMantleSkyClock).
 *
 * The time source is the synced server time. By default that is the game state's replicated
 * server world time; the script host replaces it with its own ServerNow(), whose settled sync
 * error is about 150 ms (0.03 degrees of sun), so scripts and the sky read the same clock.
 *
 * On clients, FMantleEnvironment::Apply also binds the sky (BindSky), and every tick this aims
 * and fades the one environment light: the sun by day, the moon by night. The light stays the
 * atmosphere light, so the real-time sky light capture and the height fog follow it on their own.
 * It also turns and fades the star dome and places the moon's disc opposite the sun (the night
 * sky, UpdateNightSky), all derived from the same clock, so nothing replicates for them either.
 * The weather (#446) is derived from the one replicated seed and the same clock: it drives the
 * clouds, the fog and the sky light (UpdateWeatherSky), dims the sun and fades the night sky.
 *
 * Server scripts change all of this through one replicated FMantleEnvironmentOverride (#454,
 * script API 1.11, SetOverride): the clock's hour and rate, a forced weather, the cloud cover, the
 * fog, the stars, the sun, and a post-process subset applied through one unbound volume this
 * subsystem owns on clients. Every getter here already includes the override. A client script can
 * lay its own look over that post-process on its own machine (SetLocalLook, #532 ticket 4).
 */
UCLASS()
class MANTLERUNTIME_API UMantleEnvironmentSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin UWorldSubsystem / FTickableGameObject
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual bool IsTickable() const override;
	virtual TStatId GetStatId() const override
	{
		RETURN_QUICK_DECLARE_CYCLE_STAT(UMantleEnvironmentSubsystem, STATGROUP_Tickables);
	}
	//~ End UWorldSubsystem / FTickableGameObject

	// ---- Clock ------------------------------------------------------------------------------

	/**
	 * Called by FMantleEnvironment::Apply for every place built into this world, on every net
	 * mode. Logs MANTLE_ENV_CLOCK once per call with the hour and the clock's settings, the line
	 * a harness reads to confirm every process runs the same clock.
	 */
	void StartClock();

	/** True once StartClock has run for this world. */
	bool HasClock() const { return bClockStarted; }

	/**
	 * Replaces the default time source (the game state's server world time). The script host
	 * sets its ServerNow() here. An unset function restores the default.
	 */
	void SetTimeSource(TFunction<double()> InTimeSource);

	/** The server time the clock reads, in seconds. */
	double GetServerSeconds() const;

	/** 0 to under 24: the clock, or a script's (#454). */
	double GetTimeOfDay() const;

	/** Hours since day zero, not wrapped: what the hour and the moon's phase come from. */
	double GetTotalHours() const;

	/** True while the sun is below the horizon. */
	bool IsNight() const;

	/** One day's length in real minutes, before any test time scale: 30. */
	static double GetDayLengthMinutes() { return MantleEnvironmentDefaults::DayLengthSeconds / 60.0; }

	const FMantleSkyClock& GetClock() const { return Clock; }

	// ---- Sky (clients only) -----------------------------------------------------------------

	/**
	 * Takes over the sun (and registers the rest of the sky) for the place FMantleEnvironment
	 * just built, replacing any earlier binding. Records the sun's own intensity and colour as
	 * the day values and its yaw as the heading, then aims and fades it for the current hour at
	 * once, so the first rendered frame is already right. Never called on a dedicated server.
	 */
	void BindSky(const FMantleSkyBinding& Binding);

	/** The heading the sun's path turns around, in degrees of yaw. */
	double GetHeadingYawDegrees() const { return HeadingYawDegrees; }

	/** Where the one light should point, and how brightly, at the current hour. */
	FMantleSkyLightState GetTargetLightState() const;

	/**
	 * Aims and fades the bound light for the current hour. The light is re-aimed only once the
	 * target has moved MantleEnvironmentDefaults::SunStepDegrees from where it was last aimed,
	 * or when it swaps between sun and moon; bForce re-aims regardless. Its intensity is also
	 * dimmed by the weather's cloud cover (FMantleWeatherLook::SunIntensityScale) and brightened
	 * by lightning. Called every tick.
	 */
	void UpdateSky(bool bForce = false);

	// ---- Night sky (clients only, #445 ticket 4) --------------------------------------------

	/** True while a bound star dome or moon is still alive. */
	bool HasNightSky() const;

	/**
	 * Fades, turns and places the star dome and the moon for the current hour. StarFade and
	 * MoonFade come from the sun's elevation (FMantleSkyClock::StarFade, MoonFade), times the
	 * weather's FMantleWeatherLook::StarVisibility, so the night sky fades out under cloud; the dome
	 * turns with the hour (StarDomeRotation); the moon sits opposite the sun, facing the world,
	 * with its phase from the lunation (MoonPhaseLightDirection).
	 *
	 * A body whose fade is zero is hidden and left alone, so by day neither costs a draw. A
	 * shown body moves only once the clock has carried it NightSkyStepDegrees, and a fade is
	 * written only once it has changed by NightSkyFadeEpsilon or reached either end. bForce
	 * applies everything regardless. Called every tick, and by BindSky.
	 */
	void UpdateNightSky(bool bForce = false);

	// ---- Weather (#446) ---------------------------------------------------------------------

	/**
	 * Weather time: the server seconds the clock reads, times the clock's time scale.
	 */
	double GetWeatherSeconds() const;

	/**
	 * Sets the one number the weather is derived from (see FMantleWeatherSequence). The server
	 * calls this once, when it spawns the script replicator that carries the seed; a client
	 * calls it when the replicated seed arrives. Zero is "no seed" and is ignored; setting the
	 * seed already in use changes nothing. Logs MANTLE_ENV_WEATHER with the current slot.
	 */
	void SetWeatherSeed(int32 Seed);

	/** True once a seed has been set. */
	bool HasWeatherSeed() const { return Weather.IsSet(); }

	/** The seed in use, or 0 before one is set. */
	int32 GetWeatherSeed() const { return Weather.IsSet() ? Weather->GetSeed() : 0; }

	/**
	 * The weather now: the seeded sequence's, or what a script forced (#454) and the blend into
	 * or back out of it, with a script's cloud cover on top. Clear, fully blended, until the seed
	 * is known (a forced weather applies without it). SlotIndex is always the seeded slot's.
	 */
	FMantleWeatherSample GetWeather() const;

	/**
	 * The seed a server starts with: -MantleEnvWeatherSeed=<n> (non-Shipping, non-zero), and
	 * otherwise a random non-zero number.
	 */
	static int32 MakeServerWeatherSeed();

	/**
	 * Clients only: advances the lightning schedule to the current weather time. Every strike
	 * passed since the last call is broadcast on OnLightningStrike and starts a flash; strikes
	 * more than LightningCatchUpSeconds back (a hitch, a clock jump) are skipped, the first call
	 * only starts the schedule, and a clock that steps back never fires a strike twice. Called
	 * every tick, before UpdateSky.
	 */
	void UpdateLightning();

	/**
	 * 0 and up: the sum of the live flashes' brightness, each fading out over
	 * LightningFlashSeconds of real time. UpdateSky adds it to the light's intensity, and
	 * UpdateWeatherSky pulses the clouds' lightning with it.
	 */
	double GetLightningFlash() const { return LightningFlash; }

	/**
	 * Whether a flash brightens the sun (the default). The game client turns it off when it lights its strikes itself
	 * (its own lightning light), so a flash is not lit twice; strikes still fire on OnLightningStrike either way.
	 */
	void SetLightningLightsSun(bool bLightsSun) { bLightningLightsSun = bLightsSun; }

	// ---- Weather on the sky (clients only, #446 ticket 2) -----------------------------------

	/** True while a bound cloud material, fog or sky light is still alive. */
	bool HasWeatherSky() const;

	/**
	 * What the current weather does to the sky (FMantleWeatherLook::FromValues of GetWeather).
	 * bOutSettled, when given, is set to whether the weather has finished blending, so the
	 * look is not moving.
	 */
	FMantleWeatherLook GetWeatherLook(bool* bOutSettled = nullptr) const;

	/**
	 * Writes the weather to the bound sky: the cloud material's coverage, density and storm
	 * values, the height fog's density and the sky light's intensity, each scaled from what the
	 * component had when bound. A value is written only once it has moved by WeatherLookEpsilon
	 * of its scale, or when the weather has settled and it is not yet exact; bForce writes
	 * everything. The clouds' lightning is pulsed with GetLightningFlash on every frame of a
	 * flash, and is zero otherwise. Anything not bound (or a parameter the cloud material lacks)
	 * is skipped. Called every tick, and by BindSky.
	 */
	void UpdateWeatherSky(bool bForce = false);

	/**
	 * Fires on clients for every strike, at the same environment time on every client, with no
	 * message per strike. The hook for the script API's LightningStrike signal (#446 ticket 4).
	 */
	FMantleLightningStrikeDelegate OnLightningStrike;

	// ---- Script overrides (#454, script API 1.11) -------------------------------------------

	/** What server scripts have changed: the default (which changes nothing) until one writes. */
	const FMantleEnvironmentOverride& GetOverride() const { return Override; }

	/**
	 * Takes a whole override: on the server from the script host (by way of the script
	 * replicator), on a client from the replicated copy, a late joiner's initial one included.
	 * Sanitized first; an override equal to the current one changes nothing. Logs
	 * MANTLE_ENV_OVERRIDE, then re-applies the bound sky and the post-process at once, so the
	 * change shows on the next frame rather than on the next step of the sun.
	 */
	void SetOverride(const FMantleEnvironmentOverride& InOverride);

	/**
	 * The override with the clock jumped to Hour (wrapped into 0-24) now, at the rate it runs at.
	 * The day is kept, so the moon's phase does not jump. Unchanged when the hour already is Hour;
	 * a non-finite hour changes nothing.
	 */
	FMantleEnvironmentOverride MakeClockTimeOverride(double Hour) const;

	/**
	 * The override with the clock re-anchored at the hour it shows now and running at Scale times
	 * the default rate (clamped to 0-MaxScriptTimeScale; 0 freezes it). Unchanged when it already
	 * runs at that rate; a non-finite scale changes nothing.
	 */
	FMantleEnvironmentOverride MakeTimeScaleOverride(double Scale) const;

	/**
	 * The override with the weather forced to State, blending from the weather now over
	 * BlendSeconds (clamped to 0-MaxScriptWeatherBlendSeconds). Unchanged when State is already
	 * forced.
	 */
	FMantleEnvironmentOverride MakeForcedWeatherOverride(EMantleWeatherState State, double BlendSeconds) const;

	/**
	 * The override with the seeded weather back, blending from the weather now over BlendSeconds.
	 * Unchanged when no weather is forced.
	 */
	FMantleEnvironmentOverride MakeNaturalWeatherOverride(double BlendSeconds) const;

	/**
	 * A place's default look (#532, settings.environment) as an override built from the default,
	 * never from the current one: the clock anchored now at its clockTime (on the default clock's
	 * day, so the moon's phase carries on) and running at its timeScale, its weather held with no
	 * blend, its tint, saturation, bloom and colour-grading preset and intensity. Each key the place
	 * leaves out stays at the default. A set hour
	 * or weather takes revision 1, so every machine building it from the same settings agrees on
	 * the revisions whatever time it builds at: the server seeds the replicated override with it as
	 * the place loads, and each machine's script host counts it as its starting point rather than
	 * as a script's change (no PropertyChanged). The default override when the place sets nothing.
	 */
	FMantleEnvironmentOverride MakeSettingsOverride(const FMantleEnvironmentSettings& Settings) const;

	// ---- This player's own look (#532 ticket 4, script API 1.29) -----------------------------

	/**
	 * Replaces this machine's local look (Environment:SetLocalLook): sanitized, then merged field
	 * by field over the replicated override in UpdatePostProcess. Never replicated and never part
	 * of GetOverride, so scripts' Environment getters and PropertyChanged keep reporting the
	 * server's values. A look equal to the current one changes nothing; otherwise it logs
	 * MANTLE_ENV_LOCAL_LOOK and re-applies the post-process at once. An empty look clears it.
	 */
	void SetLocalLook(const FMantleEnvironmentLocalLook& InLook);

	/** Environment:ClearLocalLook, and the script host when the script that set it stops. */
	void ClearLocalLook() { SetLocalLook(FMantleEnvironmentLocalLook()); }

	const FMantleEnvironmentLocalLook& GetLocalLook() const { return LocalLook; }

	/** The post-process this machine draws: the replicated override with the local look over it. */
	FMantleEnvironmentOverride GetPostProcessLook() const;

	/**
	 * Clients: applies the post-process subset of GetPostProcessLook (the replicated override, with
	 * this player's local look over it) through one unbound post-process volume this subsystem
	 * spawns the first time a value is not neutral (and never on a dedicated server), and disables
	 * it again when every value is back to neutral. Called by SetOverride and SetLocalLook.
	 */
	void UpdatePostProcess();

	// ---- The place's exposure (#628, settings.environment.exposure) -------------------------------

	/**
	 * Clients: sets the place's exposure (FMantleEnvironment::Apply, from settings.environment.exposure; unset clears it) and
	 * applies it through the same one post-process volume as the script look. Not replicated: every client has the place.
	 * Mantle fixes its exposure (DefaultEngine.ini turns r.DefaultFeature.AutoExposure off, which only sets the base
	 * brightness range to 1 to 1; a volume's own override of the range wins over that base). So:
	 * - Fixed with a bias of 0, or unset: no override; the game's own exposure, as before.
	 * - Fixed with a bias: the volume overrides the exposure compensation only, to the game's own plus the bias.
	 * - Auto: the volume overrides the metering method (histogram), the brightness range (the place's, else the engine's
	 *   default one) and the compensation, so the exposure adapts as the editor's does.
	 * A script's non-zero Environment.ExposureBias is added to the game's own exposure, in place of the place's bias.
	 */
	void SetPlaceExposure(const TOptional<FMantleExposureSettings>& Exposure);

	/**
	 * The game's own exposure compensation in stops: the project's r.DefaultFeature.AutoExposure.Bias (1 unless the
	 * project changes it), which every post-process setting starts from. A place's exposure bias is added to it.
	 */
	static float GetEngineExposureBias();

	/** True when the place's exposure changes nothing: none, or fixed with no bias (the game's own). */
	static bool IsPlaceExposureNeutral(const TOptional<FMantleExposureSettings>& Exposure);

	/**
	 * Writes the exposure part of a post-process volume's Settings (UpdatePostProcess's, split out so it can be tested without
	 * a world): the compensation (EngineBias plus ScriptBias when a script set one, else the place's bias) and, for an
	 * auto place exposure, the histogram metering and the brightness range. Overrides nothing a fixed, unbiased exposure
	 * does not change.
	 */
	static void ApplyExposureSettings(FPostProcessSettings& Settings, float ScriptBias, const TOptional<FMantleExposureSettings>& Exposure,
		float EngineBias, float Lift = 0.0f);

	/**
	 * Clients: stops added to a fixed exposure's compensation, whatever set it (the game, the place or a script), through the
	 * same one volume; an auto place exposure adapts on its own and takes none. The game client lifts the night with it (an
	 * auto exposure's brightening, without its pumping); nothing else sets it, so it is 0 in the editor and on servers.
	 */
	void SetExposureLift(float Stops);

	/**
	 * Clients: the weather's tint for the fog (#930): its own colour moves Amount (0-1) of the way to Tint, as Ultra
	 * Dynamic Sky's fog turns to its dust colour in dust. A script's Environment.FogColor still replaces it. The game
	 * client sets it; nothing else does, so the fog keeps its own colour in the editor and on servers.
	 */
	void SetWeatherFogTint(const FLinearColor& Tint, float Amount);

	/**
	 * Clients (#532): the LUT texture of a ColorGrading preset, loaded by path
	 * (FMantleEnvironmentOverride::ColorGradingLutPath). Null for ColorGradingNone, and for a
	 * preset whose texture is missing or is not a texture: that is logged once per preset per
	 * world ("MANTLE_LUT missing ..."), and the picture keeps the project's own grading.
	 */
	UTexture* ResolveColorGradingLut(uint8 Preset);

	/**
	 * The sun's own intensity now, before its fade at the horizon and any lightning: the day light's (the place sun's or
	 * the default's), dimmed by the weather's cloud cover and a script's Environment.SunIntensityScale, as UpdateSky lights
	 * it. Zero until the sky is bound. The game client lights its twilight sky with it after the light has gone down.
	 */
	float GetSunIntensity() const;

	/** The day light's colour (the place sun's, or the default's), which the light takes back at sunrise. */
	FColor GetSunColor() const { return DayColor; }

private:
	/** The hours since day zero at ServerSeconds: the clock's, or from a script's anchor and rate. */
	double TotalHoursAt(double ServerSeconds) const;

	/** GetWeather before a script's cloud cover: where a new weather write blends from. */
	FMantleWeatherSample GetWeatherBeforeCloudCover() const;

	/** 0-1 through the last script weather write's blend, at ServerSeconds (1 without one). */
	double OverrideWeatherAlpha(double ServerSeconds) const;

	/**
	 * Every strike from FromSeconds (exclusive) to ToSeconds of weather time: the seeded
	 * sequence's, none of them while a script forces the weather (it has its own, from
	 * FMantleWeatherSequence::CollectScriptStrikes), and after a script hands the weather back,
	 * the seeded ones again once that blend is over.
	 */
	void CollectStrikes(double FromSeconds, double ToSeconds, TArray<FMantleLightningStrike>& Out) const;

	/**
	 * Gives the weather a dynamic instance of the cloud's current material (GetMaterial: the
	 * MI_MantleDefaultCloud FMantleEnvironment::Apply set, never a path loaded here, #507), and
	 * records which of the driven parameters it really has. Leaves the cloud's material alone
	 * when it has none of them.
	 */
	void BindCloudMaterial(UVolumetricCloudComponent& Cloud);

	/** The weather's cloud material while the bound cloud is still alive; null otherwise. */
	UMaterialInstanceDynamic* GetLiveCloudMaterial() const;

	FMantleSkyClock Clock;
	TFunction<double()> TimeSource;
	bool bClockStarted = false;

	/** Unset until the seed is known. Mutable: sampling extends the sequence's slot cache. */
	mutable TOptional<FMantleWeatherSequence> Weather;

	TWeakObjectPtr<UDirectionalLightComponent> SunLight;
	TWeakObjectPtr<USkyLightComponent> SkyLightComponent;
	TWeakObjectPtr<UExponentialHeightFogComponent> FogComponent;
	TWeakObjectPtr<UVolumetricCloudComponent> CloudComponent;

	double HeadingYawDegrees = 0.0;
	float DayIntensity = 0.0f;
	FColor DayColor = FColor::White;
	/** FMantleSkyBinding::bNoDirectSun for the bound place (#722). */
	bool bNoDirectSun = false;

	/** What UpdateSky last applied, so it can step instead of re-aiming every frame. */
	bool bSkyApplied = false;
	bool bAppliedMoon = false;
	FVector AppliedDirection = FVector::ZeroVector;
	float AppliedIntensity = -1.0f;
	bool bAppliedFlash = false;
	bool bLightningLightsSun = true;

	/** The night sky, and what UpdateNightSky last applied to it (a fade below zero: never). */
	TWeakObjectPtr<UStaticMeshComponent> StarDomeMesh;
	TWeakObjectPtr<UMaterialInstanceDynamic> StarDomeMaterial;
	TWeakObjectPtr<UStaticMeshComponent> MoonMesh;
	TWeakObjectPtr<UMaterialInstanceDynamic> MoonMaterial;
	float AppliedStarFade = -1.0f;
	float AppliedMoonFade = -1.0f;
	bool bDomeTurned = false;
	FQuat AppliedDomeRotation = FQuat::Identity;
	bool bMoonPlaced = false;
	FVector AppliedMoonDirection = FVector::ZeroVector;
	FVector AppliedPhaseLight = FVector::ZeroVector;

	/** Lightning: the weather time UpdateLightning last reached, and the flashes still fading. */
	bool bLightningStarted = false;
	double LastLightningSeconds = 0.0;
	TArray<FMantleLightningStrike> LiveFlashes;
	double LightningFlash = 0.0;
	double LightningCloudFrame = 0.0;

	/**
	 * The weather on the sky: the cloud's dynamic material and which driven parameters it has
	 * (a mask of the flags in the .cpp), the lightning values the material came with, the fog's
	 * and sky light's bound values, and what UpdateWeatherSky last wrote.
	 */
	TWeakObjectPtr<UMaterialInstanceDynamic> CloudMaterial;
	uint8 CloudParameters = 0;
	FLinearColor CloudLightningColorBase = FLinearColor::Black;
	FLinearColor CloudLightningAnimBase = FLinearColor::Black;
	float BaseFogDensity = 0.0f;
	float BaseSkyLightIntensity = 0.0f;
	bool bWeatherSkyApplied = false;
	float AppliedCloudCoverage = 0.0f;
	float AppliedCloudDensity = 0.0f;
	float AppliedStormClouds = 0.0f;
	float AppliedCloudLightning = 0.0f;
	float AppliedCloudFrame = 0.0f;
	float AppliedFogDensity = 0.0f;
	float AppliedSkyLightIntensity = 0.0f;

	/** #454: what server scripts changed, the fog colour it replaces, and the post-process volume. */
	FMantleEnvironmentOverride Override;
	/** #532 ticket 4: this player's own look, over Override's post-process only. */
	FMantleEnvironmentLocalLook LocalLook;
	FLinearColor BaseFogColor = FLinearColor::Black;
	FLinearColor AppliedFogColor = FLinearColor::Black;
	TWeakObjectPtr<APostProcessVolume> PostProcessVolume;
	/** #628: the place's exposure, which UpdatePostProcess puts in that same volume. */
	TOptional<FMantleExposureSettings> PlaceExposure;
	/** SetExposureLift's stops. */
	float ExposureLift = 0.0f;

	/** SetWeatherFogTint's colour and share. */
	FLinearColor WeatherFogTint = FLinearColor::Black;
	float WeatherFogTintAmount = 0.0f;

	/**
	 * #532: a bit per colour-grading preset (1 << preset) already found missing, so each is looked for and logged once.
	 * A found LUT needs no cache: the volume's settings hold it, and after that it resolves in memory.
	 */
	uint8 ColorGradingLutMissing = 0;
};
