// Copyright Joshua Eger. All Rights Reserved.
#include "MantleEnvironment.h"
#include "MantleEnvironmentSubsystem.h"
#include "MantlePlaceBuilder.h"
#include "MantlePlaceTypes.h"
#include "MantleRuntime.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/SkyLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

namespace
{
	/**
	 * Spawns one environment actor: always transient regardless of the build's own transience
	 * option (the sky is never authored content, so it is never worth saving), non-replicated
	 * (some of these classes — ASkyLight and AExponentialHeightFog among them — carry a
	 * replicated bEnabled by default for normal multiplayer level placement; that default would
	 * quietly violate "nothing the builder spawns may be a replicated actor" here), and Movable
	 * (unattached place actors are frozen Static once pass 3 has already run; the sky cluster
	 * spawns after that pass, so it must be made Movable explicitly).
	 */
	template <typename TActor>
	TActor* SpawnEnvironmentActor(UWorld* World, const TCHAR* Label)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Params.ObjectFlags |= RF_Transient;

		TActor* Actor = World->SpawnActor<TActor>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
		if (!Actor)
		{
			return nullptr;
		}

		Actor->SetFlags(RF_Transient);
		Actor->SetReplicates(false);
		Actor->Tags.Add(FName(FMantleEnvironment::EnvironmentActorTag));
		if (USceneComponent* Root = Actor->GetRootComponent())
		{
			Root->SetMobility(EComponentMobility::Movable);
		}
#if WITH_EDITOR
		Actor->SetActorLabel(Label);
#endif
		return Actor;
	}

	/**
	 * The place's first DirectionalLight entity, in authored entity-array order — not
	 * Result.Actors' TMap order, which is unordered and would make "first" meaningless.
	 */
	ADirectionalLight* FindPlaceSun(const FMantlePlace& Place, const FMantleBuildResult& Result)
	{
		for (const FMantleEntity& Entity : Place.Entities)
		{
			if (Entity.Type != EMantleEntityType::DirectionalLight)
			{
				continue;
			}
			if (AActor* const* Actor = Result.Actors.Find(Entity.Id))
			{
				if (ADirectionalLight* Sun = Cast<ADirectionalLight>(*Actor))
				{
					return Sun;
				}
			}
		}
		return nullptr;
	}

	/**
	 * A place with no DirectionalLight still needs a sun for the atmosphere and volumetric
	 * clouds to render against. This sun is never part of Place.Entities and never given an
	 * entity id — it exists only so the sky renders, matching the ticket's fixed defaults.
	 * Its angle is the environment subsystem's (#445): the sun's path turns around +X, so the
	 * rotation set here only matters in a world with no subsystem (an editor preview).
	 */
	ADirectionalLight* SpawnDefaultSun(UWorld* World)
	{
		ADirectionalLight* Sun = SpawnEnvironmentActor<ADirectionalLight>(World, TEXT("MantleDefaultSun"));
		if (!Sun)
		{
			return nullptr;
		}

		Sun->SetActorRotation(FRotator(-46.0, 35.0, 0.0));

		if (UDirectionalLightComponent* Light = Cast<UDirectionalLightComponent>(Sun->GetLightComponent()))
		{
			Light->SetIntensity(3.5f);
			Light->SetLightColor(FLinearColor(1.0f, 0.97f, 0.92f));
		}

		return Sun;
	}

	/**
	 * Loads one night-sky asset (#445 ticket 4) without the loader's own warnings, saying once
	 * per process per path when it is missing: the sky then does without that body. Named for
	 * the night sky, since unity builds merge this file with others.
	 */
	template <typename TAsset>
	TAsset* LoadNightSkyAsset(const TCHAR* Path)
	{
		TAsset* Asset = LoadObject<TAsset>(nullptr, Path, nullptr, LOAD_Quiet | LOAD_NoWarn);
		if (!Asset)
		{
			static TSet<FString> ReportedMissing;
			bool bAlreadyReported = false;
			ReportedMissing.Add(FString(Path), &bAlreadyReported);
			if (!bAlreadyReported)
			{
				UE_LOG(LogMantle, Warning,
					TEXT("Night sky: %s is missing; the sky goes without it (tools/environment/Create-MantleSkyMaterials.py authors the sky materials)."),
					Path);
			}
		}
		return Asset;
	}
}

void FMantleEnvironment::ApplyFogSettings(AExponentialHeightFog* Fog, const FMantleFogSettings& Settings)
{
	UExponentialHeightFogComponent* Component = Fog ? Fog->GetComponent() : nullptr;
	if (!Component)
	{
		return;
	}
	// Each key the place sets replaces the fog's own value; the others keep it. The fog's own values are the weather's
	// baselines (BindSky records them), so this runs before BindSky.
	if (Settings.Density.IsSet())
	{
		Component->SetFogDensity(static_cast<float>(*Settings.Density));
	}
	if (Settings.HeightFalloff.IsSet())
	{
		Component->SetFogHeightFalloff(static_cast<float>(*Settings.HeightFalloff));
	}
	if (Settings.InscatteringColor.IsSet())
	{
		Component->SetFogInscatteringColor(*Settings.InscatteringColor);
	}
	if (Settings.StartDistance.IsSet())
	{
		Component->SetStartDistance(static_cast<float>(*Settings.StartDistance));
	}
	if (Settings.DirectionalInscatteringColor.IsSet())
	{
		Component->SetDirectionalInscatteringColor(*Settings.DirectionalInscatteringColor);
	}
}

FMantleFogSettings FMantleEnvironment::DefaultFogSettings()
{
	FMantleFogSettings Settings;
	Settings.Density = MantleEnvironmentDefaults::DefaultFogDensity;
	Settings.HeightFalloff = MantleEnvironmentDefaults::DefaultFogHeightFalloff;
	Settings.StartDistance = MantleEnvironmentDefaults::DefaultFogStartDistance;
	return Settings;
}

AStaticMeshActor* FMantleEnvironment::SpawnNightSkySphere(UWorld* World, const TCHAR* MaterialPath, double Radius, const TCHAR* Label)
{
	// Cosmetic only, like the rest of the sky: a dedicated server never draws it.
	if (!World || !MaterialPath || World->GetNetMode() == NM_DedicatedServer)
	{
		return nullptr;
	}

	UStaticMesh* Sphere = LoadNightSkyAsset<UStaticMesh>(NightSkySphereMeshPath);
	UMaterialInterface* Material = LoadNightSkyAsset<UMaterialInterface>(MaterialPath);
	if (!Sphere || !Material)
	{
		return nullptr;
	}

	AStaticMeshActor* Body = SpawnEnvironmentActor<AStaticMeshActor>(World, Label);
	UStaticMeshComponent* Mesh = Body ? Body->GetStaticMeshComponent() : nullptr;
	if (!Mesh)
	{
		if (Body)
		{
			Body->Destroy();
		}
		return nullptr;
	}

	// Sky, not scenery: nothing touches it, walks on it, navigates around it or is shadowed or
	// lit by it. Set before the mesh, so a 40 km sphere never registers with any of them, and
	// the mesh asset's own collision profile is never taken up (bUseDefaultCollision).
	Mesh->bUseDefaultCollision = false;
	Mesh->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetGenerateOverlapEvents(false);
	Mesh->SetCanEverAffectNavigation(false);
	Mesh->CanCharacterStepUpOn = ECB_No;
	Mesh->SetCastShadow(false);
	Mesh->SetAffectDistanceFieldLighting(false);
	Mesh->SetAffectDynamicIndirectLighting(false);
	Mesh->SetReceivesDecals(false);
	Mesh->SetVisibleInRayTracing(false);
	// Hidden until the subsystem's first update says the body is up.
	Mesh->SetVisibility(false);

	Mesh->SetStaticMesh(Sphere);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetMaterial(0, UMaterialInstanceDynamic::Create(Material, Mesh));
	// The engine sphere is 100 cm across.
	Mesh->SetWorldScale3D(FVector(Radius / 50.0));
	return Body;
}

void FMantleEnvironment::Apply(UWorld* World, const FMantlePlace& Place, FMantleBuildResult& Result)
{
	if (!World)
	{
		return;
	}

	// The clock runs on every net mode (#445): a dedicated server renders nothing, but its
	// scripts still read the hour. Started before the early return below for exactly that reason.
	UMantleEnvironmentSubsystem* Environment = World->GetSubsystem<UMantleEnvironmentSubsystem>();
	if (Environment)
	{
		Environment->StartClock();
	}

	if (World->GetNetMode() == NM_DedicatedServer)
	{
		// Cosmetic only — a dedicated server never renders anything, so there is nothing to
		// give a sky to.
		return;
	}

	// `settings.skybox` is not read yet: every place gets the default sky.

	FMantleSkyBinding Binding;
	// Indoor levels (#722): the place's scale on the sky light, and a sun that never reaches it.
	Binding.SkyLightIntensityScale = static_cast<float>(Place.Settings.Environment.SkyLightIntensity.Get(1.0));
	Binding.bNoDirectSun = Place.Settings.Environment.bNoDirectSun;

	if (AActor* Atmosphere = SpawnEnvironmentActor<ASkyAtmosphere>(World, TEXT("MantleDefaultSkyAtmosphere")))
	{
		Result.EnvironmentActors.Add(Atmosphere);
	}

	if (ASkyLight* SkyLight = SpawnEnvironmentActor<ASkyLight>(World, TEXT("MantleDefaultSkyLight")))
	{
		// Nothing is baked, so the sky light has to capture the scene itself.
		if (USkyLightComponent* Light = SkyLight->GetLightComponent())
		{
			Light->SetRealTimeCapture(true);
		}
		Result.EnvironmentActors.Add(SkyLight);
		Binding.SkyLight = SkyLight;
	}

	if (AVolumetricCloud* Cloud = SpawnEnvironmentActor<AVolumetricCloud>(World, TEXT("MantleDefaultVolumetricCloud")))
	{
		// Without this the component falls back to an engine material that is never cooked.
		UVolumetricCloudComponent* CloudComponent = Cloud->FindComponentByClass<UVolumetricCloudComponent>();
		UMaterialInterface* CloudMaterial = LoadObject<UMaterialInterface>(nullptr, FMantleEnvironment::DefaultCloudMaterialPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (CloudComponent && CloudMaterial)
		{
			CloudComponent->SetMaterial(CloudMaterial);
		}
		Result.EnvironmentActors.Add(Cloud);
		Binding.Cloud = Cloud;
	}

	if (AExponentialHeightFog* Fog = SpawnEnvironmentActor<AExponentialHeightFog>(World, TEXT("MantleDefaultHeightFog")))
	{
		// The place's own fog look (#628), before the weather records the fog's density and colour as its baselines.
		// A place with no fog of its own gets Mantle's default fog; a place with a fog keeps the engine's value for
		// every key it left alone, as its creator saw it in the editor.
		if (Place.Settings.Environment.Fog.IsSet())
		{
			ApplyFogSettings(Fog, *Place.Settings.Environment.Fog);
		}
		else
		{
			ApplyFogSettings(Fog, DefaultFogSettings());
		}
		Result.EnvironmentActors.Add(Fog);
		Binding.Fog = Fog;
	}

	ADirectionalLight* Sun = FindPlaceSun(Place, Result);
	Binding.bPlaceSun = Sun != nullptr;
	if (!Sun)
	{
		Sun = SpawnDefaultSun(World);
		if (Sun)
		{
			Result.EnvironmentActors.Add(Sun);
		}
	}

	if (Sun)
	{
		if (UDirectionalLightComponent* Light = Cast<UDirectionalLightComponent>(Sun->GetLightComponent()))
		{
			Light->SetAtmosphereSunLight(true);
		}
	}
	Binding.Sun = Sun;

	// The night sky (#445 ticket 4): a star dome around the world and a moon inside it, turned,
	// placed and faded by the subsystem. Either is simply absent when its assets are missing.
	if (AStaticMeshActor* StarDome = SpawnNightSkySphere(World, StarDomeMaterialPath,
		MantleEnvironmentDefaults::StarDomeRadius, TEXT("MantleStarDome")))
	{
		Result.EnvironmentActors.Add(StarDome);
		Binding.StarDome = StarDome;
	}
	const double MoonRadius = MantleEnvironmentDefaults::MoonDistance
		* FMath::Tan(FMath::DegreesToRadians(MantleEnvironmentDefaults::MoonAngularDiameterDegrees * 0.5));
	if (AStaticMeshActor* Moon = SpawnNightSkySphere(World, MoonMaterialPath, MoonRadius, TEXT("MantleMoon")))
	{
		Result.EnvironmentActors.Add(Moon);
		Binding.Moon = Moon;
	}

	// The day/night cycle (#445): from here on the subsystem owns the sun's angle, and aims it
	// for the current hour before the first frame renders.
	if (Environment)
	{
		Environment->BindSky(Binding);
		// The place's exposure (#628), or none: a place without one clears the last place's.
		Environment->SetPlaceExposure(Place.Settings.Environment.Exposure);
	}
}
