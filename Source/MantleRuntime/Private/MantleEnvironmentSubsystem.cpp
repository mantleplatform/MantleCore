// Copyright Joshua Eger. All Rights Reserved.
#include "MantleEnvironmentSubsystem.h"
#include "MantleEnvironment.h"
#include "MantlePlaceTypes.h"
#include "MantleRuntime.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/SkyLight.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/CommandLine.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"

namespace
{
	/** The cloud material parameters the weather drives, as bits of CloudParameters. */
	constexpr uint8 WeatherCloudCoverageBit = 1 << 0;
	constexpr uint8 WeatherCloudDensityBit = 1 << 1;
	constexpr uint8 WeatherCloudStormBit = 1 << 2;
	constexpr uint8 WeatherCloudLightningColorBit = 1 << 3;
	constexpr uint8 WeatherCloudLightningAnimBit = 1 << 4;
	constexpr uint8 WeatherCloudAllBits = (1 << 5) - 1;

	/**
	 * Whether a weather value is worth writing: the first write, a move of more than Threshold,
	 * or, once the weather has settled, any difference left, so it lands exactly.
	 */
	bool WeatherSkyValueDue(float Target, float Applied, double Threshold, bool bFirst, bool bSettled)
	{
		return bFirst
			|| FMath::Abs(Target - Applied) > Threshold
			|| (bSettled && Target != Applied);
	}

	/** Says once per process per material which driven cloud parameters it lacks. */
	void ReportWeatherCloudParameters(const UMaterialInterface* Material, uint8 Found)
	{
		if (Found == WeatherCloudAllBits)
		{
			return;
		}
		const FString Path = Material ? Material->GetPathName() : FString(TEXT("(no material)"));
		static TSet<FString> Reported;
		bool bAlreadyReported = false;
		Reported.Add(Path, &bAlreadyReported);
		if (bAlreadyReported)
		{
			return;
		}

		struct FWeatherCloudParameterName
		{
			uint8 Bit;
			const TCHAR* Name;
		};
		const FWeatherCloudParameterName Parameters[] = {
			{ WeatherCloudCoverageBit, FMantleEnvironment::CloudCoverageParameter },
			{ WeatherCloudDensityBit, FMantleEnvironment::CloudDensityParameter },
			{ WeatherCloudStormBit, FMantleEnvironment::CloudStormParameter },
			{ WeatherCloudLightningColorBit, FMantleEnvironment::CloudLightningColorParameter },
			{ WeatherCloudLightningAnimBit, FMantleEnvironment::CloudLightningAnimParameter },
		};
		FString Missing;
		for (const FWeatherCloudParameterName& Parameter : Parameters)
		{
			if ((Found & Parameter.Bit) == 0)
			{
				Missing += Missing.IsEmpty() ? FString(Parameter.Name) : FString(TEXT(", ")) + Parameter.Name;
			}
		}
		UE_LOG(LogMantle, Warning,
			TEXT("Weather clouds: %s lacks %s; the weather leaves %s."),
			*Path, *Missing, Found == 0 ? TEXT("the clouds alone") : TEXT("those values alone"));
	}

	/**
	 * Whether a night-sky fade is worth writing: never written yet, moved by more than the
	 * epsilon, or landing exactly on either end. Named for the night sky: unity builds merge
	 * this file with others.
	 */
	bool NightSkyFadeDue(float Target, float Applied)
	{
		return Applied < 0.0f
			|| FMath::Abs(Target - Applied) > static_cast<float>(MantleEnvironmentDefaults::NightSkyFadeEpsilon)
			|| ((Target <= 0.0f || Target >= 1.0f) && Target != Applied);
	}

	/**
	 * The angle between two rotations, in radians. FQuat::AngularDistance can take the arc
	 * cosine of a value rounded just past 1 (NaN) for equal rotations; this clamps.
	 */
	double NightSkyTurnRadians(const FQuat& A, const FQuat& B)
	{
		const double Dot = FMath::Min(FMath::Abs(A.X * B.X + A.Y * B.Y + A.Z * B.Z + A.W * B.W), 1.0);
		return 2.0 * FMath::Acos(Dot);
	}

	/** A script override's next revision: never zero, which means "never written". */
	int32 NextEnvironmentOverrideRevision(int32 Revision)
	{
		return (Revision <= 0 || Revision == MAX_int32) ? 1 : Revision + 1;
	}

	/** Shows or hides a night-sky body. Returns true when it has just been shown. */
	bool SetNightSkyBodyShown(UStaticMeshComponent& Body, bool bShown)
	{
		if (Body.GetVisibleFlag() == bShown)
		{
			return false;
		}
		Body.SetVisibility(bShown);
		return bShown;
	}
}

bool UMantleEnvironmentSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	// Every net mode, the dedicated server included (scripts there read the hour), and the editor
	// preview's Game world. Editor worlds are left out: nothing may tick or re-aim a light in a
	// level a creator is editing and will save. The importer's build, whatever its world type,
	// never binds a sky at all (FMantleBuildOptions::bRuntimeEnvironment).
	const UWorld* World = Cast<UWorld>(Outer);
	return World && (World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE);
}

void UMantleEnvironmentSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	Clock = FMantleSkyClock();
}

void UMantleEnvironmentSubsystem::Deinitialize()
{
	TimeSource.Reset();
	bClockStarted = false;
	Weather.Reset();
	OnLightningStrike.Clear();
	LiveFlashes.Reset();
	LightningFlash = 0.0;
	LightningCloudFrame = 0.0;
	bLightningStarted = false;
	SunLight.Reset();
	SkyLightComponent.Reset();
	FogComponent.Reset();
	CloudComponent.Reset();
	CloudMaterial.Reset();
	CloudParameters = 0;
	StarDomeMesh.Reset();
	StarDomeMaterial.Reset();
	MoonMesh.Reset();
	MoonMaterial.Reset();
	Override = FMantleEnvironmentOverride();
	LocalLook = FMantleEnvironmentLocalLook();
	PostProcessVolume.Reset();
	ColorGradingLutMissing = 0;
	Super::Deinitialize();
}

void UMantleEnvironmentSubsystem::Tick(float DeltaTime)
{
	// IsTickable already says this, unless the engine ever ticks unconditionally: a world with
	// no sky (a dedicated server) has nothing to flash, aim or fade.
	if (SunLight.IsValid() || GetLiveCloudMaterial())
	{
		UpdateLightning();
	}
	if (SunLight.IsValid())
	{
		UpdateSky();
	}
	UpdateWeatherSky();
	UpdateNightSky();
}

bool UMantleEnvironmentSubsystem::IsTickable() const
{
	// Only a client with a live sky has anything to drive; the clock itself is computed on demand.
	return SunLight.IsValid() || HasNightSky() || HasWeatherSky();
}

void UMantleEnvironmentSubsystem::StartClock()
{
	bClockStarted = true;

	const UWorld* World = GetWorld();
	UE_LOG(LogMantle, Log,
		TEXT("MANTLE_ENV_CLOCK net_mode=%d server_seconds=%.3f hour=%.4f start_hour=%.4f time_scale=%.4f day_seconds=%.1f"),
		World ? static_cast<int32>(World->GetNetMode()) : -1,
		GetServerSeconds(),
		GetTimeOfDay(),
		Clock.StartHour,
		Clock.TimeScale,
		Clock.DayLengthSeconds);
}

void UMantleEnvironmentSubsystem::SetTimeSource(TFunction<double()> InTimeSource)
{
	TimeSource = MoveTemp(InTimeSource);
}

double UMantleEnvironmentSubsystem::GetServerSeconds() const
{
	if (TimeSource)
	{
		return TimeSource();
	}

	const UWorld* World = GetWorld();
	if (!World)
	{
		return 0.0;
	}
	if (const AGameStateBase* GameState = World->GetGameState())
	{
		return GameState->GetServerWorldTimeSeconds();
	}
	return World->GetTimeSeconds();
}

double UMantleEnvironmentSubsystem::GetTimeOfDay() const
{
	return FMantleSkyClock::Wrap24(GetTotalHours());
}

double UMantleEnvironmentSubsystem::GetTotalHours() const
{
	return TotalHoursAt(GetServerSeconds());
}

double UMantleEnvironmentSubsystem::TotalHoursAt(double ServerSeconds) const
{
	if (!Override.bClockSet)
	{
		return Clock.TotalHoursAt(ServerSeconds);
	}
	// A script's clock: from where it was set, at its own share of the default rate (which
	// still carries the test-only time scale). Frozen, it stays exactly on the anchor.
	return Override.ClockAnchorTotalHours
		+ (ServerSeconds - Override.ClockAnchorServerSeconds) * Clock.HoursPerSecond() * static_cast<double>(Override.TimeScale);
}

bool UMantleEnvironmentSubsystem::IsNight() const
{
	return FMantleSkyClock::IsNight(GetTimeOfDay());
}

void UMantleEnvironmentSubsystem::BindSky(const FMantleSkyBinding& Binding)
{
	SunLight = Binding.Sun ? Cast<UDirectionalLightComponent>(Binding.Sun->GetLightComponent()) : nullptr;
	SkyLightComponent = Binding.SkyLight ? Binding.SkyLight->GetLightComponent() : nullptr;
	FogComponent = Binding.Fog ? Binding.Fog->GetComponent() : nullptr;
	CloudComponent = Binding.Cloud ? Binding.Cloud->FindComponentByClass<UVolumetricCloudComponent>() : nullptr;

	// The night sky's dynamic material instances, which SpawnNightSkySphere put on slot 0.
	UStaticMeshComponent* Dome = Binding.StarDome ? Binding.StarDome->GetStaticMeshComponent() : nullptr;
	UStaticMeshComponent* MoonBody = Binding.Moon ? Binding.Moon->GetStaticMeshComponent() : nullptr;
	StarDomeMesh = Dome;
	StarDomeMaterial = Dome ? Cast<UMaterialInstanceDynamic>(Dome->GetMaterial(0)) : nullptr;
	MoonMesh = MoonBody;
	MoonMaterial = MoonBody ? Cast<UMaterialInstanceDynamic>(MoonBody->GetMaterial(0)) : nullptr;

	bSkyApplied = false;
	bAppliedMoon = false;
	AppliedDirection = FVector::ZeroVector;
	AppliedIntensity = -1.0f;
	bAppliedFlash = false;

	AppliedStarFade = -1.0f;
	AppliedMoonFade = -1.0f;
	bDomeTurned = false;
	bMoonPlaced = false;

	// The weather on the sky. Every value is scaled from what the component has
	// now, the same rule as the sun's own intensity: the environment spawned these, but nothing
	// here assumes their engine defaults.
	CloudMaterial.Reset();
	CloudParameters = 0;
	if (UVolumetricCloudComponent* Cloud = CloudComponent.Get())
	{
		BindCloudMaterial(*Cloud);
	}
	const UExponentialHeightFogComponent* BoundFog = FogComponent.Get();
	BaseFogDensity = BoundFog ? BoundFog->FogDensity : 0.0f;
	// The fog's own colour, which a script's Environment.FogColor replaces and nil restores.
	BaseFogColor = BoundFog ? BoundFog->FogInscatteringLuminance : FLinearColor::Black;
	const USkyLightComponent* BoundSkyLight = SkyLightComponent.Get();
	// The place's own scale (#722) is part of the base, so the weather scales from it.
	const float SkyLightScale = FMath::Clamp(Binding.SkyLightIntensityScale, 0.0f, 1.0f);
	BaseSkyLightIntensity = BoundSkyLight ? BoundSkyLight->Intensity * SkyLightScale : 0.0f;
	bWeatherSkyApplied = false;
	UpdateWeatherSky(/*bForce*/ true);
	if (HasWeatherSky())
	{
		const FMantleWeatherLook Look = GetWeatherLook();
		UE_LOG(LogMantle, Log,
			TEXT("MANTLE_ENV_WEATHER_SKY bound cloud=%d cloud_parameters=0x%02x fog=%d sky_light=%d coverage=%.3f fog_scale=%.3f"),
			CloudMaterial.IsValid() ? 1 : 0, static_cast<uint32>(CloudParameters), BoundFog ? 1 : 0, BoundSkyLight ? 1 : 0,
			Look.CloudCoverage, Look.FogDensityScale);
	}

	// The place's own sun keeps its intensity, colour and heading; only its angle is Mantle's.
	// The night sky turns around the same heading, so it is set even without a light.
	UDirectionalLightComponent* Light = SunLight.Get();
	HeadingYawDegrees = (Light && Binding.bPlaceSun) ? Binding.Sun->GetActorRotation().Yaw : 0.0;
	bNoDirectSun = Binding.bNoDirectSun;
	if (Light)
	{
		DayIntensity = Light->Intensity;
		DayColor = Light->LightColor;
		if (bNoDirectSun)
		{
			// A sealed level's sun (#722). The atmosphere and the clouds read the light's intensity and colour (its
			// outer-space illuminance), so those stay as they are and the sky light still captures a lit sky. What
			// the light adds to the place goes: its direct diffuse and specular light (Lumen's surface cache reads
			// the same scales), its indirect light and its light in the volumetric fog.
			Light->SetDiffuseScale(0.0f);
			Light->SetSpecularScale(0.0f);
			Light->SetIndirectLightingIntensity(0.0f);
			Light->SetVolumetricScatteringIntensity(0.0f);
		}

		UpdateSky(/*bForce*/ true);

		UE_LOG(LogMantle, Log,
			TEXT("MANTLE_ENV_SKY bound place_sun=%d heading=%.2f day_intensity=%.3f direct_sun=%d sky_light_scale=%.3f hour=%.4f"),
			Binding.bPlaceSun ? 1 : 0, HeadingYawDegrees, DayIntensity, bNoDirectSun ? 0 : 1, SkyLightScale, GetTimeOfDay());
	}

	UpdateNightSky(/*bForce*/ true);

	UE_LOG(LogMantle, Log,
		TEXT("MANTLE_ENV_NIGHT_SKY bound stars=%d moon=%d star_fade=%.3f moon_fade=%.3f lunation=%.3f hour=%.4f"),
		StarDomeMesh.IsValid() ? 1 : 0, MoonMesh.IsValid() ? 1 : 0,
		FMath::Max(AppliedStarFade, 0.0f), FMath::Max(AppliedMoonFade, 0.0f),
		FMantleSkyClock::MoonLunationAtTotalHours(GetTotalHours()), GetTimeOfDay());
}

bool UMantleEnvironmentSubsystem::HasNightSky() const
{
	return StarDomeMesh.IsValid() || MoonMesh.IsValid();
}

bool UMantleEnvironmentSubsystem::HasWeatherSky() const
{
	return GetLiveCloudMaterial() || FogComponent.IsValid() || SkyLightComponent.IsValid();
}

UMaterialInstanceDynamic* UMantleEnvironmentSubsystem::GetLiveCloudMaterial() const
{
	// The instance outlives a destroyed cloud until garbage collection (Destroy marks the actor
	// and its components, not objects created under them), so the cloud itself must be alive.
	return CloudComponent.IsValid() ? CloudMaterial.Get() : nullptr;
}

void UMantleEnvironmentSubsystem::BindCloudMaterial(UVolumetricCloudComponent& Cloud)
{
	UMaterialInterface* Current = Cloud.GetMaterial();
	// A cloud bound before already draws with the weather's instance; its parent still holds
	// the material's own values.
	UMaterialInstanceDynamic* Existing = Cast<UMaterialInstanceDynamic>(Current);
	const UMaterialInterface* Source = Existing ? Existing->Parent.Get() : Current;
	if (!Current || !Source)
	{
		ReportWeatherCloudParameters(Source, 0);
		return;
	}

	uint8 Found = 0;
	float Unused = 0.0f;
	if (Source->GetScalarParameterValue(FHashedMaterialParameterInfo(FName(FMantleEnvironment::CloudCoverageParameter)), Unused))
	{
		Found |= WeatherCloudCoverageBit;
	}
	if (Source->GetScalarParameterValue(FHashedMaterialParameterInfo(FName(FMantleEnvironment::CloudDensityParameter)), Unused))
	{
		Found |= WeatherCloudDensityBit;
	}
	if (Source->GetScalarParameterValue(FHashedMaterialParameterInfo(FName(FMantleEnvironment::CloudStormParameter)), Unused))
	{
		Found |= WeatherCloudStormBit;
	}
	if (Source->GetVectorParameterValue(FHashedMaterialParameterInfo(FName(FMantleEnvironment::CloudLightningColorParameter)), CloudLightningColorBase))
	{
		Found |= WeatherCloudLightningColorBit;
	}
	if (Source->GetVectorParameterValue(FHashedMaterialParameterInfo(FName(FMantleEnvironment::CloudLightningAnimParameter)), CloudLightningAnimBase))
	{
		Found |= WeatherCloudLightningAnimBit;
	}
	ReportWeatherCloudParameters(Source, Found);
	if (Found == 0)
	{
		return;
	}

	UMaterialInstanceDynamic* Material = Existing ? Existing : UMaterialInstanceDynamic::Create(Current, &Cloud);
	if (!Material)
	{
		return;
	}
	if (!Existing)
	{
		Cloud.SetMaterial(Material);
	}
	CloudMaterial = Material;
	CloudParameters = Found;
}

FMantleWeatherLook UMantleEnvironmentSubsystem::GetWeatherLook(bool* bOutSettled) const
{
	const FMantleWeatherSample Sample = GetWeather();
	if (bOutSettled)
	{
		*bOutSettled = Sample.Blend >= 1.0;
	}
	return FMantleWeatherLook::FromValues(Sample.Values, FMantleSkyClock::SunElevationDegrees(GetTimeOfDay()));
}

void UMantleEnvironmentSubsystem::UpdateWeatherSky(bool bForce)
{
	UMaterialInstanceDynamic* Cloud = GetLiveCloudMaterial();
	UExponentialHeightFogComponent* Fog = FogComponent.Get();
	USkyLightComponent* SkyLight = SkyLightComponent.Get();
	if (!Cloud && !Fog && !SkyLight)
	{
		return;
	}

	bool bSettled = true;
	const FMantleWeatherLook Look = GetWeatherLook(&bSettled);
	const bool bFirst = bForce || !bWeatherSkyApplied;
	const double Epsilon = MantleEnvironmentDefaults::WeatherLookEpsilon;

	if (Cloud)
	{
		auto WriteScalar = [this, Cloud, bFirst, bSettled](uint8 Bit, const TCHAR* Name, float Target, float& Applied, double Threshold)
		{
			if ((CloudParameters & Bit) && WeatherSkyValueDue(Target, Applied, Threshold, bFirst, bSettled))
			{
				Cloud->SetScalarParameterValue(FName(Name), Target);
				Applied = Target;
			}
		};
		WriteScalar(WeatherCloudCoverageBit, FMantleEnvironment::CloudCoverageParameter, Look.CloudCoverage, AppliedCloudCoverage,
			Epsilon * FMath::Abs(MantleEnvironmentDefaults::CloudCoverageAtFullCover - MantleEnvironmentDefaults::CloudCoverageAtNoCover));
		WriteScalar(WeatherCloudDensityBit, FMantleEnvironment::CloudDensityParameter, Look.CloudDensity, AppliedCloudDensity,
			Epsilon * MantleEnvironmentDefaults::CloudDensityAtFullCover);
		WriteScalar(WeatherCloudStormBit, FMantleEnvironment::CloudStormParameter, Look.StormClouds, AppliedStormClouds, Epsilon);

		// The lightning pulse: the same flash that brightens the light, on every
		// frame of it, and exactly zero the rest of the time, so the clouds never glow on their own.
		if (CloudParameters & WeatherCloudLightningColorBit)
		{
			const float Brightness = CloudLightningColorBase.A * FMantleWeatherLook::CloudLightningScale(LightningFlash);
			if (bFirst || Brightness != AppliedCloudLightning)
			{
				Cloud->SetVectorParameterValue(FName(FMantleEnvironment::CloudLightningColorParameter),
					FLinearColor(CloudLightningColorBase.R, CloudLightningColorBase.G, CloudLightningColorBase.B, Brightness));
				AppliedCloudLightning = Brightness;
			}
		}
		// The material animates its own lightning on each machine's own clock (time times
		// DynamicLightingAnim), which no two clients share, so that rate is held at zero and the
		// newest strike picks the frame (ManualLightningAnim) instead.
		if (CloudParameters & WeatherCloudLightningAnimBit)
		{
			const float Frame = static_cast<float>(LightningCloudFrame);
			if (bFirst || Frame != AppliedCloudFrame)
			{
				Cloud->SetVectorParameterValue(FName(FMantleEnvironment::CloudLightningAnimParameter),
					FLinearColor(CloudLightningAnimBase.R, 0.0f, Frame, CloudLightningAnimBase.A));
				AppliedCloudFrame = Frame;
			}
		}
	}

	if (Fog)
	{
		// A script's Environment.FogDensity multiplies the weather's fog.
		const float Density = BaseFogDensity * Look.FogDensityScale * Override.FogDensity;
		if (WeatherSkyValueDue(Density, AppliedFogDensity, Epsilon * BaseFogDensity, bFirst, bSettled))
		{
			Fog->SetFogDensity(Density);
			AppliedFogDensity = Density;
		}

		// Environment.FogColor replaces the fog's own colour while it is set; nil restores it. The weather's tint (dust)
		// moves the fog's own colour toward its own.
		const FLinearColor OwnColor = WeatherFogTintAmount > 0.0f ? FMath::Lerp(BaseFogColor, WeatherFogTint, WeatherFogTintAmount) : BaseFogColor;
		const FLinearColor Color = Override.bFogColorSet ? FMantleEnvironmentOverride::LinearFromRgb(Override.FogColor) : OwnColor;
		if (bFirst || !Color.Equals(AppliedFogColor, 1.0e-3f))
		{
			Fog->SetFogInscatteringColor(Color);
			AppliedFogColor = Color;
		}
	}

	if (SkyLight)
	{
		const float Intensity = BaseSkyLightIntensity * Look.SkyLightIntensityScale;
		if (WeatherSkyValueDue(Intensity, AppliedSkyLightIntensity, Epsilon * BaseSkyLightIntensity, bFirst, bSettled))
		{
			SkyLight->SetIntensity(Intensity);
			AppliedSkyLightIntensity = Intensity;
		}
	}

	bWeatherSkyApplied = true;
}

void UMantleEnvironmentSubsystem::UpdateNightSky(bool bForce)
{
	UStaticMeshComponent* Dome = StarDomeMesh.Get();
	UStaticMeshComponent* MoonBody = MoonMesh.Get();
	if (!Dome && !MoonBody)
	{
		return;
	}

	// The script's clock when one set it: the stars turn and the moon waxes with its days.
	const double TotalHours = GetTotalHours();
	const double Hour = FMantleSkyClock::Wrap24(TotalHours);
	const double SunElevation = FMantleSkyClock::SunElevationDegrees(Hour);
	const double StepRadians = FMath::DegreesToRadians(MantleEnvironmentDefaults::NightSkyStepDegrees);
	const double StepCos = FMath::Cos(StepRadians);
	// The night sky is additive and draws over the clouds, so it fades with the cover.
	const double CloudVisibility = GetWeatherLook().StarVisibility;

	if (Dome)
	{
		// Environment.StarsVisible replaces the whole fade: shown in full, even by day, or never.
		float Fade = static_cast<float>(FMantleSkyClock::StarFade(SunElevation) * CloudVisibility);
		if (Override.Stars == MantleEnvironmentOverrideValues::StarsShown)
		{
			Fade = 1.0f;
		}
		else if (Override.Stars == MantleEnvironmentOverrideValues::StarsHidden)
		{
			Fade = 0.0f;
		}
		if (bForce || NightSkyFadeDue(Fade, AppliedStarFade))
		{
			if (UMaterialInstanceDynamic* Material = StarDomeMaterial.Get())
			{
				Material->SetScalarParameterValue(FName(FMantleEnvironment::StarFadeParameter), Fade);
			}
			AppliedStarFade = Fade;
		}

		// By day the dome is hidden and left where it was; it catches up the moment it shows.
		const bool bShown = AppliedStarFade > 0.0f;
		const bool bJustShown = SetNightSkyBodyShown(*Dome, bShown);
		if (bShown)
		{
			const FQuat Target = FMantleSkyClock::StarDomeRotation(Hour, HeadingYawDegrees);
			if (bForce || bJustShown || !bDomeTurned || NightSkyTurnRadians(AppliedDomeRotation, Target) >= StepRadians)
			{
				Dome->SetWorldRotation(Target);
				AppliedDomeRotation = Target;
				bDomeTurned = true;
			}
		}
	}

	if (MoonBody)
	{
		const float Fade = static_cast<float>(FMantleSkyClock::MoonFade(SunElevation) * CloudVisibility);
		if (bForce || NightSkyFadeDue(Fade, AppliedMoonFade))
		{
			if (UMaterialInstanceDynamic* Material = MoonMaterial.Get())
			{
				Material->SetScalarParameterValue(FName(FMantleEnvironment::MoonFadeParameter), Fade);
			}
			AppliedMoonFade = Fade;
		}

		const bool bShown = AppliedMoonFade > 0.0f;
		const bool bJustShown = SetNightSkyBodyShown(*MoonBody, bShown);
		if (bShown)
		{
			// Opposite the sun, where the moonlight comes from, always turned the same face to
			// the world. The phase light moves with it, and drifts through the lunation.
			const FVector Direction = FMantleSkyClock::MoonDirection(Hour, HeadingYawDegrees);
			const FVector PhaseLight = FMantleSkyClock::MoonPhaseLightDirection(
				Hour, HeadingYawDegrees, FMantleSkyClock::MoonLunationAtTotalHours(TotalHours));
			if (bForce || bJustShown || !bMoonPlaced
				|| (AppliedMoonDirection | Direction) <= StepCos
				|| (AppliedPhaseLight | PhaseLight) <= StepCos)
			{
				MoonBody->SetWorldLocationAndRotation(
					Direction * MantleEnvironmentDefaults::MoonDistance, (-Direction).Rotation());
				if (UMaterialInstanceDynamic* Material = MoonMaterial.Get())
				{
					Material->SetVectorParameterValue(FName(FMantleEnvironment::PhaseLightDirectionParameter),
						FLinearColor(static_cast<float>(PhaseLight.X), static_cast<float>(PhaseLight.Y),
							static_cast<float>(PhaseLight.Z), 0.0f));
				}
				AppliedMoonDirection = Direction;
				AppliedPhaseLight = PhaseLight;
				bMoonPlaced = true;
			}
		}
	}
}

FMantleSkyLightState UMantleEnvironmentSubsystem::GetTargetLightState() const
{
	return FMantleSkyClock::EvaluateLight(GetTimeOfDay(), HeadingYawDegrees);
}

float UMantleEnvironmentSubsystem::GetSunIntensity() const
{
	return SunLight.IsValid() ? DayIntensity * GetWeatherLook().SunIntensityScale * Override.SunIntensityScale : 0.0f;
}

void UMantleEnvironmentSubsystem::UpdateSky(bool bForce)
{
	UDirectionalLightComponent* Light = SunLight.Get();
	if (!Light)
	{
		return;
	}

	const FMantleSkyLightState Target = GetTargetLightState();
	const bool bSwapped = bSkyApplied && Target.bMoon != bAppliedMoon;

	// Stepped, not continuous: Lumen and virtual shadow maps re-light on every re-aim, so the
	// light only moves once the clock has carried it a whole step (or it swaps bodies, which
	// happens at zero intensity where nothing can see the jump).
	const double StepCos = FMath::Cos(FMath::DegreesToRadians(MantleEnvironmentDefaults::SunStepDegrees));
	const bool bStepDue = !bSkyApplied || (AppliedDirection | Target.Direction) <= StepCos;
	if (bForce || bSwapped || bStepDue)
	{
		Light->SetWorldRotation(Target.Direction.Rotation());
		AppliedDirection = Target.Direction;
	}

	if (!bSkyApplied || bSwapped)
	{
		if (Target.bMoon)
		{
			Light->SetLightColor(MantleEnvironmentDefaults::MoonColor);
		}
		else
		{
			Light->SetLightFColor(DayColor);
		}
	}

	// Clouds dim the sun and the moon alike: the ground is not shadowed by them otherwise.
	bool bWeatherSettled = true;
	const float CloudDim = GetWeatherLook(&bWeatherSettled).SunIntensityScale;
	// A script's Environment.SunIntensityScale scales the sun and the moon alike.
	// GetSunIntensity is the sun's part of this, for the game client's twilight sky.
	const float BodyIntensity = (Target.bMoon ? DayIntensity * MantleEnvironmentDefaults::MoonIntensityScale : DayIntensity)
		* CloudDim * Override.SunIntensityScale;
	// A lightning flash only brightens the light; it never re-aims it.
	const float Flash = bLightningLightsSun
		? DayIntensity * MantleEnvironmentDefaults::LightningFlashIntensityScale * static_cast<float>(LightningFlash)
		: 0.0f;
	const float Intensity = BodyIntensity * static_cast<float>(Target.Fade) + Flash;
	const float Epsilon = DayIntensity * MantleEnvironmentDefaults::IntensityEpsilonScale;
	const bool bFlashEnded = bAppliedFlash && LightningFlash <= 0.0;
	const bool bIntensityDue = bForce || !bSkyApplied || bSwapped || bFlashEnded
		|| FMath::Abs(Intensity - AppliedIntensity) > Epsilon
		// Always land exactly on the ends of a fade, however small the last change, once the
		// weather's dimming has stopped moving too (while it blends, the epsilon above applies).
		|| ((Target.Fade <= 0.0 || Target.Fade >= 1.0) && bWeatherSettled && Intensity != AppliedIntensity);
	if (bIntensityDue)
	{
		Light->SetIntensity(Intensity);
		AppliedIntensity = Intensity;
		bAppliedFlash = LightningFlash > 0.0;
	}

	bAppliedMoon = Target.bMoon;
	bSkyApplied = true;
}

double UMantleEnvironmentSubsystem::GetWeatherSeconds() const
{
	return GetServerSeconds() * Clock.TimeScale;
}

void UMantleEnvironmentSubsystem::SetWeatherSeed(int32 Seed)
{
	if (Seed == 0 || (Weather.IsSet() && Weather->GetSeed() == Seed))
	{
		return;
	}
	Weather.Emplace(Seed);

	const FMantleWeatherSample Sample = GetWeather();
	const UWorld* World = GetWorld();
	UE_LOG(LogMantle, Log,
		TEXT("MANTLE_ENV_WEATHER net_mode=%d seed=%d weather_seconds=%.3f slot=%lld weather=%s blend=%.3f"),
		World ? static_cast<int32>(World->GetNetMode()) : -1,
		Seed,
		GetWeatherSeconds(),
		Sample.SlotIndex,
		MantleWeatherStateName(Sample.State),
		Sample.Blend);
}

FMantleWeatherSample UMantleEnvironmentSubsystem::GetWeather() const
{
	FMantleWeatherSample Sample = GetWeatherBeforeCloudCover();
	// Environment:SetCloudCover: the cover a script chose, over whatever the weather is.
	if (Override.CloudCover >= 0.0f)
	{
		Sample.Values.CloudCover = Override.CloudCover;
	}
	return Sample;
}

FMantleWeatherSample UMantleEnvironmentSubsystem::GetWeatherBeforeCloudCover() const
{
	FMantleWeatherSample Natural;
	if (Weather.IsSet())
	{
		Natural = Weather->Sample(GetWeatherSeconds());
	}
	else
	{
		Natural.Values = FMantleWeatherSequence::ValuesFor(EMantleWeatherState::Clear);
	}
	if (Override.WeatherRevision == 0)
	{
		return Natural;
	}

	// A script's weather: from the values the weather had when the script wrote, to the
	// state it forced, or back to the seeded weather, which keeps moving underneath.
	const double Alpha = OverrideWeatherAlpha(GetServerSeconds());
	const FMantleWeatherValues From = Override.GetFromValues();
	FMantleWeatherSample Out = Natural;
	if (Override.IsWeatherForced())
	{
		const EMantleWeatherState State = Override.GetForcedWeather();
		Out.State = State;
		Out.PreviousState = Override.GetWeatherFrom();
		Out.Blend = Alpha;
		// Settled, it is exactly the forced state's look, as a settled seeded slot is.
		Out.Values = Alpha >= 1.0
			? FMantleWeatherSequence::ValuesFor(State)
			: FMantleWeatherValues::Lerp(From, FMantleWeatherSequence::ValuesFor(State), Alpha);
	}
	else if (Alpha < 1.0)
	{
		Out.PreviousState = Override.GetWeatherFrom();
		Out.Blend = FMath::Min(Alpha, Natural.Blend);
		Out.Values = FMantleWeatherValues::Lerp(From, Natural.Values, Alpha);
	}
	return Out;
}

double UMantleEnvironmentSubsystem::OverrideWeatherAlpha(double ServerSeconds) const
{
	if (Override.WeatherBlendSeconds <= 0.0f)
	{
		return 1.0;
	}
	// Weather time, as the seeded blend runs in: the test-only time scale speeds both up.
	const double Elapsed = (ServerSeconds - Override.WeatherChangeServerSeconds) * Clock.TimeScale;
	return FMath::SmoothStep(0.0, static_cast<double>(Override.WeatherBlendSeconds), Elapsed);
}

void UMantleEnvironmentSubsystem::CollectStrikes(double FromSeconds, double ToSeconds, TArray<FMantleLightningStrike>& Out) const
{
	if (!Weather.IsSet())
	{
		return;
	}
	if (Override.WeatherRevision == 0)
	{
		Weather->CollectStrikes(FromSeconds, ToSeconds, Out);
		return;
	}

	// The last script weather write, in weather time, and the end of its blend.
	const double ChangeSeconds = Override.WeatherChangeServerSeconds * Clock.TimeScale;
	const double BlendSeconds = static_cast<double>(Override.WeatherBlendSeconds);
	const double BlendEnd = ChangeSeconds + BlendSeconds;
	const bool bForced = Override.IsWeatherForced();

	// The script's own strikes: from the lightning the weather had when it wrote, to the forced
	// state's (none after a hand-back). Earlier than any seeded strike below, so Out stays in order.
	const double ScriptFrom = FMath::Max(FromSeconds, ChangeSeconds);
	const double ScriptTo = bForced ? ToSeconds : FMath::Min(ToSeconds, BlendEnd);
	if (ScriptTo > ScriptFrom)
	{
		const double FromRate = Override.FromLightningPerMinute;
		const double ToRate = bForced ? FMantleWeatherSequence::ValuesFor(Override.GetForcedWeather()).LightningPerMinute : 0.0;
		FMantleWeatherSequence::CollectScriptStrikes(Weather->GetSeed(), Override.WeatherRevision, ChangeSeconds, ScriptFrom, ScriptTo,
			[FromRate, ToRate, ChangeSeconds, BlendSeconds](double Seconds)
			{
				const double Alpha = BlendSeconds <= 0.0 ? 1.0 : FMath::SmoothStep(0.0, BlendSeconds, Seconds - ChangeSeconds);
				return FMath::Lerp(FromRate, ToRate, Alpha);
			},
			Out);
	}

	// The seeded sequence's own: never while forced, and after a hand-back only once it is whole.
	if (!bForced && ToSeconds > BlendEnd)
	{
		Weather->CollectStrikes(FMath::Max(FromSeconds, BlendEnd), ToSeconds, Out);
	}
}

int32 UMantleEnvironmentSubsystem::MakeServerWeatherSeed()
{
#if !UE_BUILD_SHIPPING
	int32 TestSeed = 0;
	if (FMantleWeatherSequence::ParseTestSeed(FCommandLine::Get(), TestSeed))
	{
		UE_LOG(LogMantle, Warning, TEXT("Environment test switch in use: weather seed %d"), TestSeed);
		return TestSeed;
	}
#endif
	const FGuid Guid = FGuid::NewGuid();
	const int32 Seed = static_cast<int32>(Guid.A ^ Guid.B ^ Guid.C ^ Guid.D);
	return Seed != 0 ? Seed : 1;
}

void UMantleEnvironmentSubsystem::UpdateLightning()
{
	if (!Weather.IsSet())
	{
		LightningFlash = 0.0;
		return;
	}

	const double Now = GetWeatherSeconds();
	if (!bLightningStarted)
	{
		// The first call starts the schedule here: strikes from before this client was
		// watching are not replayed.
		bLightningStarted = true;
		LastLightningSeconds = Now;
	}
	else if (Now > LastLightningSeconds)
	{
		const double From = FMath::Max(LastLightningSeconds, Now - MantleEnvironmentDefaults::LightningCatchUpSeconds);
		TArray<FMantleLightningStrike> Passed;
		CollectStrikes(From, Now, Passed);
		LastLightningSeconds = Now;
		for (const FMantleLightningStrike& Strike : Passed)
		{
			LiveFlashes.Add(Strike);
			OnLightningStrike.Broadcast(Strike);
		}
	}
	// Otherwise the clock stood still or stepped back (a better server-time estimate arrived).
	// LastLightningSeconds stays a high-water mark, so no strike ever fires twice.

	// Flashes last real seconds, not environment seconds, so a sped-up test day still flashes
	// the way a real one does.
	const double TimeScale = FMath::Max(Clock.TimeScale, UE_DOUBLE_SMALL_NUMBER);
	LightningFlash = 0.0;
	double NewestSeconds = -UE_DOUBLE_BIG_NUMBER;
	for (int32 Index = LiveFlashes.Num() - 1; Index >= 0; --Index)
	{
		const double RealAge = (Now - LiveFlashes[Index].Seconds) / TimeScale;
		if (RealAge >= MantleEnvironmentDefaults::LightningFlashSeconds || RealAge < 0.0)
		{
			LiveFlashes.RemoveAtSwap(Index);
			continue;
		}
		const double Remaining = 1.0 - RealAge / MantleEnvironmentDefaults::LightningFlashSeconds;
		LightningFlash += LiveFlashes[Index].Brightness * Remaining * Remaining;
		// The clouds show the newest strike's frame; it stays put once the flash is over.
		if (LiveFlashes[Index].Seconds > NewestSeconds)
		{
			NewestSeconds = LiveFlashes[Index].Seconds;
			LightningCloudFrame = LiveFlashes[Index].CloudFrame;
		}
	}
}

void UMantleEnvironmentSubsystem::SetOverride(const FMantleEnvironmentOverride& InOverride)
{
	FMantleEnvironmentOverride Next = InOverride;
	Next.Sanitize();
	if (Next == Override)
	{
		return;
	}
	Override = Next;

	const FMantleWeatherSample Sample = GetWeather();
	const UWorld* World = GetWorld();
	UE_LOG(LogMantle, Log,
		TEXT("MANTLE_ENV_OVERRIDE net_mode=%d hour=%.4f time_scale=%.3f weather=%s forced=%d weather_revision=%d blend=%.3f cloud_cover=%.3f ")
		TEXT("fog_density=%.3f fog_color=%s stars=%d sun_scale=%.3f exposure=%.3f saturation=%.3f contrast=%.3f vignette=%.3f tint=%s ")
		TEXT("bloom=%.3f grading=%s grading_intensity=%.3f"),
		World ? static_cast<int32>(World->GetNetMode()) : -1,
		GetTimeOfDay(), Override.TimeScale, MantleWeatherStateName(Sample.State), Override.IsWeatherForced() ? 1 : 0,
		Override.WeatherRevision, Sample.Blend, Sample.Values.CloudCover,
		Override.FogDensity, Override.bFogColorSet ? *FMantleEnvironmentOverride::RgbToHex(Override.FogColor) : TEXT("none"),
		static_cast<int32>(Override.Stars), Override.SunIntensityScale,
		Override.ExposureBias, Override.Saturation, Override.Contrast, Override.Vignette,
		*FMantleEnvironmentOverride::RgbToHex(Override.ColorTint),
		Override.Bloom, FMantleEnvironmentOverride::ColorGradingName(Override.ColorGrading)
			? FMantleEnvironmentOverride::ColorGradingName(Override.ColorGrading) : TEXT("none"),
		Override.ColorGradingIntensity);

	// The bound sky catches up now, not on its next step; each update still writes only what moved.
	if (SunLight.IsValid())
	{
		UpdateSky();
	}
	UpdateWeatherSky();
	UpdateNightSky();
	UpdatePostProcess();
}

FMantleEnvironmentOverride UMantleEnvironmentSubsystem::MakeClockTimeOverride(double Hour) const
{
	FMantleEnvironmentOverride Next = Override;
	if (!FMath::IsFinite(Hour))
	{
		return Next;
	}
	const double Now = GetServerSeconds();
	const double Total = TotalHoursAt(Now);
	const double Current = FMantleSkyClock::Wrap24(Total);
	const double Target = FMantleSkyClock::Wrap24(Hour);
	if (FMath::IsNearlyEqual(Current, Target, 1e-9))
	{
		return Next;
	}
	Next.bClockSet = true;
	Next.ClockAnchorServerSeconds = Now;
	// The same day: only the hour moves, so the moon's phase carries on from where it was.
	Next.ClockAnchorTotalHours = (Total - Current) + Target;
	Next.ClockRevision = NextEnvironmentOverrideRevision(Override.ClockRevision);
	return Next;
}

FMantleEnvironmentOverride UMantleEnvironmentSubsystem::MakeTimeScaleOverride(double Scale) const
{
	FMantleEnvironmentOverride Next = Override;
	if (!FMath::IsFinite(Scale))
	{
		return Next;
	}
	const float Clamped = static_cast<float>(FMath::Clamp(Scale, 0.0, MantleEnvironmentDefaults::MaxScriptTimeScale));
	if (Clamped == Override.TimeScale)
	{
		return Next;
	}
	// Re-anchored where the clock is now, so a change of rate never jumps the hour.
	const double Now = GetServerSeconds();
	Next.bClockSet = true;
	Next.ClockAnchorTotalHours = TotalHoursAt(Now);
	Next.ClockAnchorServerSeconds = Now;
	Next.TimeScale = Clamped;
	return Next;
}

FMantleEnvironmentOverride UMantleEnvironmentSubsystem::MakeForcedWeatherOverride(EMantleWeatherState State, double BlendSeconds) const
{
	FMantleEnvironmentOverride Next = Override;
	if (Override.IsWeatherForced() && Override.GetForcedWeather() == State)
	{
		return Next;
	}
	const FMantleWeatherSample Now = GetWeatherBeforeCloudCover();
	Next.SetFromValues(Now.Values);
	Next.WeatherFrom = static_cast<uint8>(Now.State);
	Next.WeatherMode = MantleEnvironmentOverrideValues::WeatherForced;
	Next.ForcedWeather = static_cast<uint8>(State);
	Next.WeatherChangeServerSeconds = GetServerSeconds();
	Next.WeatherBlendSeconds = static_cast<float>(FMath::IsFinite(BlendSeconds)
		? FMath::Clamp(BlendSeconds, 0.0, MantleEnvironmentDefaults::MaxScriptWeatherBlendSeconds)
		: MantleEnvironmentDefaults::DefaultScriptWeatherBlendSeconds);
	Next.WeatherRevision = NextEnvironmentOverrideRevision(Override.WeatherRevision);
	return Next;
}

FMantleEnvironmentOverride UMantleEnvironmentSubsystem::MakeNaturalWeatherOverride(double BlendSeconds) const
{
	FMantleEnvironmentOverride Next = Override;
	if (!Override.IsWeatherForced())
	{
		return Next;
	}
	const FMantleWeatherSample Now = GetWeatherBeforeCloudCover();
	Next.SetFromValues(Now.Values);
	Next.WeatherFrom = static_cast<uint8>(Now.State);
	Next.WeatherMode = MantleEnvironmentOverrideValues::WeatherNatural;
	Next.WeatherChangeServerSeconds = GetServerSeconds();
	Next.WeatherBlendSeconds = static_cast<float>(FMath::IsFinite(BlendSeconds)
		? FMath::Clamp(BlendSeconds, 0.0, MantleEnvironmentDefaults::MaxScriptWeatherBlendSeconds)
		: MantleEnvironmentDefaults::DefaultScriptWeatherBlendSeconds);
	Next.WeatherRevision = NextEnvironmentOverrideRevision(Override.WeatherRevision);
	return Next;
}

FMantleEnvironmentOverride UMantleEnvironmentSubsystem::MakeSettingsOverride(const FMantleEnvironmentSettings& Settings) const
{
	FMantleEnvironmentOverride Next;
	const double Now = GetServerSeconds();
	if (Settings.ClockTime.IsSet() || Settings.TimeScale.IsSet())
	{
		// The default clock's reading now, not the current override's: this is the starting point.
		const double Total = Clock.TotalHoursAt(Now);
		Next.bClockSet = true;
		Next.ClockAnchorServerSeconds = Now;
		Next.ClockAnchorTotalHours = Total;
		if (Settings.ClockTime.IsSet() && FMath::IsFinite(*Settings.ClockTime))
		{
			// The same day, as MakeClockTimeOverride keeps it.
			Next.ClockAnchorTotalHours = (Total - FMantleSkyClock::Wrap24(Total)) + FMantleSkyClock::Wrap24(*Settings.ClockTime);
			Next.ClockRevision = 1;
		}
		if (Settings.TimeScale.IsSet())
		{
			Next.TimeScale = static_cast<float>(*Settings.TimeScale);
		}
	}
	if (Settings.Weather.IsSet())
	{
		// Held from the start with no blend: exactly the state's look, as a settled forced weather.
		const EMantleWeatherState State = *Settings.Weather;
		Next.WeatherMode = MantleEnvironmentOverrideValues::WeatherForced;
		Next.ForcedWeather = static_cast<uint8>(State);
		Next.WeatherFrom = static_cast<uint8>(State);
		Next.SetFromValues(FMantleWeatherSequence::ValuesFor(State));
		Next.WeatherChangeServerSeconds = Now;
		Next.WeatherBlendSeconds = 0.0f;
		Next.WeatherRevision = 1;
	}
	if (Settings.ColorTint.IsSet())
	{
		Next.ColorTint = *Settings.ColorTint;
	}
	if (Settings.Saturation.IsSet())
	{
		Next.Saturation = static_cast<float>(*Settings.Saturation);
	}
	if (Settings.Bloom.IsSet() && FMath::IsFinite(*Settings.Bloom))
	{
		// Clamped here, as the Environment.Bloom setter does: Sanitize reads below zero as "unset".
		Next.Bloom = static_cast<float>(FMath::Clamp(*Settings.Bloom, 0.0, MantleEnvironmentDefaults::MaxScriptBloom));
	}
	if (Settings.ColorGrading.IsSet())
	{
		Next.ColorGrading = *Settings.ColorGrading;
	}
	if (Settings.ColorGradingIntensity.IsSet())
	{
		Next.ColorGradingIntensity = static_cast<float>(*Settings.ColorGradingIntensity);
	}
	Next.Sanitize();
	return Next;
}

void UMantleEnvironmentSubsystem::SetLocalLook(const FMantleEnvironmentLocalLook& InLook)
{
	FMantleEnvironmentLocalLook Next = InLook;
	Next.Sanitize();
	if (Next == LocalLook)
	{
		return;
	}
	LocalLook = Next;

	// Only the fields the look sets: "-" follows the server's override.
	auto NumberText = [](const TOptional<float>& Value)
	{
		return Value.IsSet() ? FString::Printf(TEXT("%.3f"), Value.GetValue()) : FString(TEXT("-"));
	};
	const TCHAR* GradingName = LocalLook.ColorGrading.IsSet()
		? FMantleEnvironmentOverride::ColorGradingName(LocalLook.ColorGrading.GetValue())
		: nullptr;
	const UWorld* World = GetWorld();
	UE_LOG(LogMantle, Log,
		TEXT("MANTLE_ENV_LOCAL_LOOK net_mode=%d %s exposure=%s saturation=%s contrast=%s vignette=%s tint=%s bloom=%s grading=%s grading_intensity=%s"),
		World ? static_cast<int32>(World->GetNetMode()) : -1,
		LocalLook.IsEmpty() ? TEXT("cleared") : TEXT("set"),
		*NumberText(LocalLook.ExposureBias), *NumberText(LocalLook.Saturation), *NumberText(LocalLook.Contrast),
		*NumberText(LocalLook.Vignette),
		LocalLook.ColorTint.IsSet() ? *FMantleEnvironmentOverride::RgbToHex(LocalLook.ColorTint.GetValue()) : TEXT("-"),
		*NumberText(LocalLook.Bloom),
		LocalLook.ColorGrading.IsSet() ? (GradingName ? GradingName : TEXT("none")) : TEXT("-"),
		*NumberText(LocalLook.ColorGradingIntensity));

	UpdatePostProcess();
}

FMantleEnvironmentOverride UMantleEnvironmentSubsystem::GetPostProcessLook() const
{
	FMantleEnvironmentOverride Look = Override;
	LocalLook.ApplyTo(Look);
	return Look;
}

void UMantleEnvironmentSubsystem::UpdatePostProcess()
{
	UWorld* World = GetWorld();
	// Nothing is drawn on a dedicated server, and a world going away gets nothing new.
	if (!World || World->GetNetMode() == NM_DedicatedServer || World->bIsTearingDown)
	{
		return;
	}

	// This player's own look over the replicated one, field by field, in one volume.
	const FMantleEnvironmentOverride Look = GetPostProcessLook();
	// The place's exposure needs the volume too, unless it is the game's own (fixed, no bias).
	const bool bNeutral = Look.IsPostProcessNeutral() && IsPlaceExposureNeutral(PlaceExposure) && ExposureLift == 0.0f;
	APostProcessVolume* Volume = PostProcessVolume.Get();
	if (!Volume)
	{
		if (bNeutral)
		{
			return;
		}
		FActorSpawnParameters Params;
		Params.Name = MakeUniqueObjectName(World->PersistentLevel, APostProcessVolume::StaticClass(), TEXT("MantleScriptPostProcess"));
		Params.ObjectFlags |= RF_Transient;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Volume = World->SpawnActor<APostProcessVolume>(Params);
		if (!Volume)
		{
			return;
		}
		// Every machine spawns its own from the replicated override; this one never replicates.
		Volume->SetReplicates(false);
		Volume->bUnbound = true;
		Volume->BlendWeight = 1.0f;
		Volume->Priority = MantleEnvironmentDefaults::ScriptPostProcessPriority;
		PostProcessVolume = Volume;
	}

	// Only what a script moved off neutral is overridden; the rest stays the project's own look.
	FPostProcessSettings& Settings = Volume->Settings;
	ApplyExposureSettings(Settings, Look.ExposureBias, PlaceExposure, GetEngineExposureBias(), ExposureLift);
	Settings.bOverride_ColorSaturation = Look.Saturation != 1.0f;
	Settings.ColorSaturation = FVector4(Look.Saturation, Look.Saturation, Look.Saturation, 1.0f);
	Settings.bOverride_ColorContrast = Look.Contrast != 1.0f;
	Settings.ColorContrast = FVector4(Look.Contrast, Look.Contrast, Look.Contrast, 1.0f);
	Settings.bOverride_VignetteIntensity = Look.Vignette >= 0.0f;
	Settings.VignetteIntensity = FMath::Max(Look.Vignette, 0.0f);
	const FLinearColor Tint = FMantleEnvironmentOverride::LinearFromRgb(Look.ColorTint);
	Settings.bOverride_ColorGain = Look.ColorTint != MantleEnvironmentOverrideValues::White;
	Settings.ColorGain = FVector4(Tint.R, Tint.G, Tint.B, 1.0f);
	// Bloom, and a preset's LUT. A missing LUT grades nothing (it was logged once).
	Settings.bOverride_BloomIntensity = Look.Bloom >= 0.0f;
	Settings.BloomIntensity = FMath::Max(Look.Bloom, 0.0f);
	UTexture* Lut = Look.ColorGrading != MantleEnvironmentOverrideValues::ColorGradingNone
		? ResolveColorGradingLut(Look.ColorGrading)
		: nullptr;
	Settings.bOverride_ColorGradingLUT = Lut != nullptr;
	Settings.ColorGradingLUT = Lut;
	Settings.bOverride_ColorGradingIntensity = Lut != nullptr;
	Settings.ColorGradingIntensity = Lut ? Look.ColorGradingIntensity : 0.0f;
	Volume->bEnabled = !bNeutral;
}

bool UMantleEnvironmentSubsystem::IsPlaceExposureNeutral(const TOptional<FMantleExposureSettings>& Exposure)
{
	return !Exposure.IsSet() || (Exposure->Mode == EMantleExposureMode::Fixed && Exposure->Bias == 0.0);
}

void UMantleEnvironmentSubsystem::ApplyExposureSettings(FPostProcessSettings& Settings, const float ScriptBias,
	const TOptional<FMantleExposureSettings>& Exposure, const float EngineBias, const float Lift)
{
	const bool bAuto = Exposure.IsSet() && Exposure->Mode == EMantleExposureMode::Auto;
	const bool bBias = Exposure.IsSet() && Exposure->Bias != 0.0;
	// A fixed exposure's lift (SetExposureLift); an auto one adapts by itself.
	const float FixedLift = bAuto ? 0.0f : Lift;

	// The exposure compensation: the game's own plus a script's non-zero value (which replaces the place's bias while set,
	// else the place's bias); then the lift. A script's 0 is the game's own, so +1 doubles the brightness.
	const float PlaceBias = Exposure.IsSet() ? static_cast<float>(Exposure->Bias) : 0.0f;
	Settings.bOverride_AutoExposureBias = ScriptBias != 0.0f || bBias || bAuto || FixedLift != 0.0f;
	Settings.AutoExposureBias = EngineBias + (ScriptBias != 0.0f ? ScriptBias : PlaceBias) + FixedLift;

	// An auto exposure names its metering and range: r.DefaultFeature.AutoExposure=False only sets the base range to
	// 1 to 1 (SceneView.cpp), which an override here replaces. Fixed leaves all three to that base, as every place did.
	Settings.bOverride_AutoExposureMethod = bAuto;
	Settings.AutoExposureMethod = EAutoExposureMethod::AEM_Histogram;
	Settings.bOverride_AutoExposureMinBrightness = bAuto;
	Settings.bOverride_AutoExposureMaxBrightness = bAuto;
	if (bAuto)
	{
		const float Min = static_cast<float>(Exposure->MinBrightness.Get(MantleEnvironmentDefaults::DefaultAutoExposureMin));
		const float Max = static_cast<float>(Exposure->MaxBrightness.Get(MantleEnvironmentDefaults::DefaultAutoExposureMax));
		Settings.AutoExposureMinBrightness = Min;
		// A range with its ends the wrong way round would be a fixed exposure, which is not what auto asked for.
		Settings.AutoExposureMaxBrightness = FMath::Max(Max, Min);
	}
}

void UMantleEnvironmentSubsystem::SetPlaceExposure(const TOptional<FMantleExposureSettings>& Exposure)
{
	PlaceExposure = Exposure;
	if (Exposure.IsSet())
	{
		UE_LOG(LogMantle, Log, TEXT("MANTLE_ENV_EXPOSURE mode=%s bias=%.3f engine_bias=%.3f min=%.3f max=%.3f"),
			Exposure->Mode == EMantleExposureMode::Auto ? TEXT("auto") : TEXT("fixed"), Exposure->Bias, GetEngineExposureBias(),
			Exposure->MinBrightness.Get(MantleEnvironmentDefaults::DefaultAutoExposureMin),
			Exposure->MaxBrightness.Get(MantleEnvironmentDefaults::DefaultAutoExposureMax));
	}
	UpdatePostProcess();
}

void UMantleEnvironmentSubsystem::SetExposureLift(float Stops)
{
	const float Lift = FMath::IsFinite(Stops) ? FMath::Clamp(Stops, -MantleEnvironmentDefaults::MaxExposureBias, MantleEnvironmentDefaults::MaxExposureBias) : 0.0f;
	// Within a thousandth of a stop is the same: the weather's lift moves a little every frame while fog blends in.
	if (FMath::IsNearlyEqual(Lift, ExposureLift, 1.0e-3f))
	{
		return;
	}
	ExposureLift = Lift;
	UpdatePostProcess();
}

void UMantleEnvironmentSubsystem::SetWeatherFogTint(const FLinearColor& Tint, float Amount)
{
	const float Share = FMath::IsFinite(Amount) ? FMath::Clamp(Amount, 0.0f, 1.0f) : 0.0f;
	if (Share == WeatherFogTintAmount && (Share == 0.0f || Tint.Equals(WeatherFogTint, 1.0e-4f)))
	{
		return;
	}
	WeatherFogTint = Tint;
	WeatherFogTintAmount = Share;
}

float UMantleEnvironmentSubsystem::GetEngineExposureBias()
{
	// The project setting every FPostProcessSettings starts from (Scene.cpp); 1 is the engine's own default.
	static const TConsoleVariableData<float>* Variable =
		IConsoleManager::Get().FindTConsoleVariableDataFloat(TEXT("r.DefaultFeature.AutoExposure.Bias"));
	return Variable ? Variable->GetValueOnGameThread() : 1.0f;
}

UTexture* UMantleEnvironmentSubsystem::ResolveColorGradingLut(uint8 Preset)
{
	const FSoftObjectPath Path = FMantleEnvironmentOverride::ColorGradingLutPath(Preset);
	if (Path.IsNull())
	{
		return nullptr;
	}
	const uint8 Bit = static_cast<uint8>(1u << Preset);
	if ((ColorGradingLutMissing & Bit) != 0)
	{
		return nullptr;
	}
	if (UTexture* Loaded = Cast<UTexture>(Path.ResolveObject()))
	{
		return Loaded;
	}

	// Loaded by path, so nothing references it: DefaultGame.ini's DirectoriesToAlwaysCook cooks
	// /Game/Mantle/Materials. A 256x16 texture, so a blocking load on the write that first asks is cheap.
	const TCHAR* Name = FMantleEnvironmentOverride::ColorGradingName(Preset);
	const FString PackageName = Path.GetLongPackageName();
	if (!FPackageName::DoesPackageExist(PackageName))
	{
		ColorGradingLutMissing |= Bit;
		UE_LOG(LogMantle, Warning,
			TEXT("MANTLE_LUT missing %s: no %s colour grading on this client (tools/materials/Create-MantleLUTs.py makes it)"),
			*Path.ToString(), Name);
		return nullptr;
	}
	UTexture* Lut = LoadObject<UTexture>(nullptr, *Path.ToString(), nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!Lut)
	{
		ColorGradingLutMissing |= Bit;
		UE_LOG(LogMantle, Warning, TEXT("MANTLE_LUT could not load %s as a texture: no %s colour grading on this client"),
			*Path.ToString(), Name);
		return nullptr;
	}
	UE_LOG(LogMantle, Log, TEXT("MANTLE_LUT loaded %s"), *Path.ToString());
	return Lut;
}
