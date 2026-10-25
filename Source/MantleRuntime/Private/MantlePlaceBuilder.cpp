// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceBuilder.h"
#include "MantleAnimClip.h"
#include "MantleAssetLibrary.h"
#include "MantleClipAnimInstance.h"
#include "MantleConstraint.h"
#include "MantleCrowdSpawner.h"
#include "MantleEffect.h"
#include "MantleEnvironment.h"
#include "MantleHoverChannel.h"
#include "MantlePhysicsProp.h"
#include "MantlePlaceAssets.h"
#include "MantlePlaceMaterial.h"
#include "MantleReflectionProbe.h"
#include "MantleRuntime.h"
#include "MantleScatterVolume.h"
#include "MantleScriptPoint.h"
#include "MantleShadowDistance.h"
#include "MantleSkeletonHash.h"
#include "MantleSoundWave.h"
#include "MantleSurface.h"
#include "MantleTerrainBuilder.h"
#include "MantleTerrainGrass.h"
#include "MantleTerrainSurface.h"
#include "MantleTrace.h"
#include "MantleVehicleSpawnPoint.h"
#include "MantleWeaponSpawnPoint.h"

#include "Animation/SkeletalMeshActor.h"
#include "Components/AudioComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Components/BoxComponent.h"
#include "Components/DecalComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/RectLightComponent.h"
#include "Components/SphereComponent.h"
#include "Components/SpotLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/DecalActor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/CollisionProfile.h"
#include "Engine/PointLight.h"
#include "Engine/RectLight.h"
#include "Engine/SpotLight.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/TextRenderActor.h"
#include "Engine/Texture.h"
#include "Engine/World.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformTime.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/PackageName.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "PhysicsEngine/PhysicsConstraintActor.h"
#include "Sound/AmbientSound.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundWave.h"
#include "TimerManager.h"

namespace
{
	FString EntityPath(int32 Index)
	{
		return FString::Printf(TEXT("/entities/%d"), Index);
	}

	/**
	 * The game-thread time one Build spent inside asset-library loads (#126), for the per-build
	 * MANTLE_PLACE_BUILD_TIMING line: a run without Insights still yields the load share and the
	 * longest single load. Every call is counted, including ones the asset was already loaded for.
	 */
	struct FMantleBuilderLoadStats
	{
		int32 Count = 0;
		double Seconds = 0.0;
		double LongestSeconds = 0.0;
		FString LongestId;

		void Add(const FString& AssetId, const double StartSeconds)
		{
			const double Elapsed = FPlatformTime::Seconds() - StartSeconds;
			++Count;
			Seconds += Elapsed;
			if (Elapsed > LongestSeconds)
			{
				LongestSeconds = Elapsed;
				LongestId = AssetId;
			}
		}
	};

	template <typename TActor>
	TActor* SpawnBare(UWorld* World, const FString& Name)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		TActor* Actor = World->SpawnActor<TActor>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
		if (Actor && !Name.IsEmpty())
		{
#if WITH_EDITOR
			Actor->SetActorLabel(Name);
#endif
		}
		return Actor;
	}

	/**
	 * Place actors are moved and reparented after spawning, so everything must be Movable.
	 * Static mobility would silently refuse the transform and leave geometry at the origin.
	 */
	void MakeMovable(AActor* Actor)
	{
		if (Actor && Actor->GetRootComponent())
		{
			Actor->GetRootComponent()->SetMobility(EComponentMobility::Movable);
		}
	}

	/** The engine unit a place's intensityUnits names (#385). */
	ELightUnits BuilderEngineLightUnits(EMantleLightUnits Units)
	{
		switch (Units)
		{
		case EMantleLightUnits::Candelas: return ELightUnits::Candelas;
		case EMantleLightUnits::Lumens:   return ELightUnits::Lumens;
		case EMantleLightUnits::EV:       return ELightUnits::EV;
		case EMantleLightUnits::Nits:     return ELightUnits::Nits;
		default:                          return ELightUnits::Unitless;
		}
	}

	/** A light's temperature (#472). The value is kept even when unused, as the editor keeps it. */
	void BuilderApplyTemperature(ULightComponent* Light, bool bUseTemperature, double Temperature)
	{
		Light->SetTemperature(static_cast<float>(Temperature));
		Light->SetUseTemperature(bUseTemperature);
	}

	/** A Point or Spot light's source shape (#472); zeros, the default, leave a point source. */
	void BuilderApplySourceShape(UPointLightComponent* Light, const FMantleLightSourceShape& Source)
	{
		Light->SetSourceRadius(static_cast<float>(Source.SourceRadius));
		Light->SetSoftSourceRadius(static_cast<float>(Source.SoftSourceRadius));
		Light->SetSourceLength(static_cast<float>(Source.SourceLength));
	}

	/** Loads Props.Mesh through the asset library, reporting a failure at Path's mesh property. */
	UStaticMesh* LoadEntityMesh(
		const FMantleStaticMeshProps& Props,
		const FString& Path,
		TArray<FMantleParseError>& Errors,
		FMantleBuilderLoadStats& LoadStats)
	{
		FString Error;
		const double MeshLoadStart = FPlatformTime::Seconds();
		UStaticMesh* Mesh = FMantleAssetLibrary::Get().LoadMesh(Props.Mesh, Error);
		LoadStats.Add(Props.Mesh, MeshLoadStart);
		if (!Mesh)
		{
			Errors.Add(FMantleParseError{ Path + TEXT("/properties/mesh"), Error });
		}
		return Mesh;
	}

	/**
	 * Mesh, shadow, collision and material overrides, shared by a StaticMesh's component and an
	 * InstancedMesh's (#467), which is a UStaticMeshComponent too.
	 */
	void ApplyMeshProps(
		UWorld* World,
		UStaticMeshComponent* Component,
		UStaticMesh* Mesh,
		const FMantleStaticMeshProps& Props,
		const FString& Path,
		TArray<FMantleParseError>& Errors,
		FMantleBuilderLoadStats& LoadStats)
	{
		const FMantleAssetLibrary& Library = FMantleAssetLibrary::Get();

		Component->SetStaticMesh(Mesh);
		Component->SetCastShadow(Props.bCastShadow);
        // AStaticMeshActor defaults to inheriting collision from its mesh asset.
        // Select an explicit per-component profile before applying the place schema;
        // otherwise OnRegister restores mesh defaults after a mobility change/save
        // reload and turns authored overlap/no-collision geometry back into blocking.
        Component->SetCollisionProfileName(UCollisionProfile::CustomCollisionProfileName);

		switch (Props.Collision)
		{
		case EMantleCollision::None:
			Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			break;
		case EMantleCollision::Overlap:
			Component->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
			Component->SetCollisionResponseToAllChannels(ECR_Overlap);
			// #821: water a character wades through still holds a hovercraft up. The hover's pads trace their own channel (the client project's
			// "Hover", ECC_GameTraceChannel5) and keep only a blocking hit, so this one channel blocks. The surface is the place file's, which every
			// machine reads alike (a dedicated server builds no uploaded materials, so the material cannot say). Nothing else traces that channel.
			if (Props.Surface == EMantleSurface::Water)
			{
				Component->SetCollisionResponseToChannel(Mantle_TraceChannel_Hover, ECR_Block);
			}
			break;
		case EMantleCollision::Blocking:
		default:
			Component->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
			Component->SetCollisionResponseToAllChannels(ECR_Block);
			break;
		}

		// #596: what the mesh is made of, as its Phys Material Override, on every net mode: a server
		// traces the bullets, and each client draws what they hit. A physics entity never has one (its
		// physics.material sets the override), and a surface this build lacks leaves the mesh's own.
		if (Props.Surface != EMantleSurface::Default)
		{
			if (UPhysicalMaterial* Surface = MantleLoadSurfaceMaterial(Props.Surface))
			{
				Component->SetPhysMaterialOverride(Surface);
			}
		}

		// A dedicated server never builds uploaded materials (nothing renders there; FMantlePlaceAssets
		// resolved and downloaded only the meshes, for collision), so their slots are left as they are.
		const bool bDedicatedServer = World->GetNetMode() == NM_DedicatedServer;
		for (int32 i = 0; i < Props.Materials.Num(); ++i)
		{
			if (Props.Materials[i].IsEmpty())
			{
				continue; // an unset slot keeps the mesh's own material (#262)
			}
			if (bDedicatedServer && FMantlePlaceAssets::IsUploadedId(Props.Materials[i]))
			{
				continue;
			}
			FString MaterialError;
			const double MaterialLoadStart = FPlatformTime::Seconds();
			UMaterialInterface* Material = Library.LoadMaterial(Props.Materials[i], MaterialError);
			LoadStats.Add(Props.Materials[i], MaterialLoadStart);
			if (Material)
			{
				Component->SetMaterial(i, Material);
				// #533: an uploaded wind material sways only near the camera.
				if (FMantlePlaceMaterials::IsWindMaterial(Material))
				{
					Component->SetWorldPositionOffsetDisableDistance(FMantlePlaceMaterials::WindDisableDistance);
				}
			}
			else
			{
				Errors.Add(FMantleParseError{
					FString::Printf(TEXT("%s/properties/materials/%d"), *Path, i),
					MaterialError });
			}
		}
	}

	/**
	 * The share of an InstancedMesh's drawDistance (#630) at which its instances start to fade (dithered, per instance,
	 * as terrain grass does): they fade over the last fifth and are gone past the whole.
	 */
	constexpr double InstanceFadeStartShare = 0.8;

	/**
	 * A mesh with a drawDistance (#630) that casts shadows stops casting them past a share of it, decided per view by
	 * UMantleShadowDistanceSubsystem, which exists only where something draws in a played world.
	 */
	void TrackShadowDistance(UWorld* World, UPrimitiveComponent* Component, const FMantleStaticMeshProps& Props)
	{
		if (Props.DrawDistance <= 0.0 || !Props.bCastShadow)
		{
			return;
		}
		if (UMantleShadowDistanceSubsystem* Shadows = World->GetSubsystem<UMantleShadowDistanceSubsystem>())
		{
			Shadows->Track(Component, Props.DrawDistance);
		}
	}

	AActor* BuildStaticMesh(
		UWorld* World,
		const FMantleEntity& Entity,
		const FMantleStaticMeshProps& Props,
		const FMantleBuildOptions& Options,
		const FString& Path,
		TArray<FMantleParseError>& Errors,
		FMantleBuilderLoadStats& LoadStats)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(MantlePlaceBuilder_StaticMesh);

		UStaticMesh* Mesh = LoadEntityMesh(Props, Path, Errors, LoadStats);
		if (!Mesh)
		{
			return nullptr;
		}

		// A physics entity (#456) is the replicated, simulating prop wherever the place is played
		// with authority; a connected client builds a plain placeholder the server's prop takes
		// over; an authoring build (the importer) keeps a plain actor with the settings on it.
		const FMantlePhysicsProps* Physics = Props.Physics.IsSet() ? &Props.Physics.GetValue() : nullptr;
		const EMantlePhysicsBuild PhysicsBuild = Physics
			? AMantlePhysicsProp::ChooseBuild(Options.bTransient && World->IsGameWorld(), World->GetNetMode())
			: EMantlePhysicsBuild::Authoring;

		AStaticMeshActor* Actor = (Physics && PhysicsBuild == EMantlePhysicsBuild::Prop)
			? SpawnBare<AMantlePhysicsProp>(World, Entity.Name)
			: SpawnBare<AStaticMeshActor>(World, Entity.Name);
		if (!Actor)
		{
			Errors.Add(FMantleParseError{ Path, TEXT("failed to spawn static mesh actor") });
			return nullptr;
		}

		MakeMovable(Actor);
		ApplyMeshProps(World, Actor->GetStaticMeshComponent(), Mesh, Props, Path, Errors, LoadStats);

		// #630: drawn, and so shadowed, no farther than its drawDistance. Absent, never culled (0). The editor's Max Draw
		// Distance too, so an imported place exports it again. A physics entity keeps its shadow: its prop, which copies
		// the placeholder's draw distance and shadow when it takes over, moves where no distance check follows it.
		if (Props.DrawDistance > 0.0)
		{
			Actor->GetStaticMeshComponent()->SetCullDistance(static_cast<float>(Props.DrawDistance));
			if (!Physics)
			{
				TrackShadowDistance(World, Actor->GetStaticMeshComponent(), Props);
			}
		}

		if (Physics)
		{
			if (AMantlePhysicsProp* Prop = Cast<AMantlePhysicsProp>(Actor))
			{
				Prop->InitFromPlace(Entity.Id, *Physics);
			}
			else if (PhysicsBuild == EMantlePhysicsBuild::Authoring)
			{
				// What the exporter reads back: the editor's Simulate Physics and mass settings. Simulate
				// Physics is what makes it a physics entity there; one that starts anchored carries the
				// MantleAnchored actor tag as well, so the next export writes "simulate": false again.
				UStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
				Component->BodyInstance.bSimulatePhysics = true;
				if (!Physics->bSimulate)
				{
					Actor->Tags.AddUnique(MantlePhysicsAnchoredTag());
				}
				AMantlePhysicsProp::ApplyBodySettings(Component, *Physics);
			}
		}
		return Actor;
	}

	/**
	 * One actor whose root is one UInstancedStaticMeshComponent holding every instance (#467), in
	 * the entity's space: the actor takes the entity's transform in pass 3 like any other. Movable
	 * until then, and frozen to Static afterwards under the same rule as a StaticMesh.
	 */
	AActor* BuildInstancedMesh(
		UWorld* World,
		const FMantleEntity& Entity,
		const FMantleInstancedMeshProps& Props,
		const FString& Path,
		TArray<FMantleParseError>& Errors,
		FMantleBuilderLoadStats& LoadStats)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(MantlePlaceBuilder_InstancedMesh);

		UStaticMesh* Mesh = LoadEntityMesh(Props.Mesh, Path, Errors, LoadStats);
		if (!Mesh)
		{
			return nullptr;
		}

		AActor* Actor = SpawnBare<AActor>(World, Entity.Name);
		if (!Actor)
		{
			Errors.Add(FMantleParseError{ Path, TEXT("failed to spawn instanced mesh actor") });
			return nullptr;
		}

		UInstancedStaticMeshComponent* Instances = NewObject<UInstancedStaticMeshComponent>(Actor, TEXT("Instances"));
		Actor->SetRootComponent(Instances);
		// Listed as the actor's own, so a level a place is imported into shows it in Details.
		Actor->AddInstanceComponent(Instances);
		Instances->SetMobility(EComponentMobility::Movable);
		ApplyMeshProps(World, Instances, Mesh, Props.Mesh, Path, Errors, LoadStats);

		// #630: each instance fades out over the last fifth of the drawDistance and is gone past it, as terrain grass
		// culls. Per instance, not the component's Max Draw Distance: that is measured to the whole set's bounds, which
		// spread far past any one instance. Absent, never culled (0, 0). The editor's instance cull distances too, so an
		// imported place exports them again.
		if (Props.Mesh.DrawDistance > 0.0)
		{
			const int32 CullEnd = FMath::Max(FMath::RoundToInt32(Props.Mesh.DrawDistance), 1);
			const int32 CullStart = FMath::Min(FMath::RoundToInt32(Props.Mesh.DrawDistance * InstanceFadeStartShare), CullEnd);
			Instances->SetCullDistances(CullStart, CullEnd);
			TrackShadowDistance(World, Instances, Props.Mesh);
		}

		// Instances go in before the component registers, so its render state and per-instance
		// physics bodies are created once for all of them rather than grown one add at a time.
		// A dedicated server draws nothing, so instances nothing can collide with are left out there.
		const bool bNothingToDoOnServer =
			World->GetNetMode() == NM_DedicatedServer && Props.Mesh.Collision == EMantleCollision::None;
		if (!bNothingToDoOnServer)
		{
			Instances->PreAllocateInstancesMemory(Props.Instances.Num());
			Instances->AddInstances(Props.Instances, /*bShouldReturnIndices*/ false, /*bWorldSpace*/ false);
		}
		Instances->RegisterComponent();
		return Actor;
	}

	AActor* BuildGroup(UWorld* World, const FMantleEntity& Entity)
	{
		AActor* Actor = SpawnBare<AActor>(World, Entity.Name);
		if (!Actor)
		{
			return nullptr;
		}

		// A group is a pure transform node — it needs a root to attach children to.
		USceneComponent* Root = NewObject<USceneComponent>(Actor, TEXT("Root"));
		Actor->SetRootComponent(Root);
		Root->SetMobility(EComponentMobility::Movable);
		Root->RegisterComponent();
		return Actor;
	}

	AActor* BuildPointLight(UWorld* World, const FMantleEntity& Entity, const FMantlePointLightProps& Props)
	{
		APointLight* Actor = SpawnBare<APointLight>(World, Entity.Name);
		if (!Actor)
		{
			return nullptr;
		}
		MakeMovable(Actor);

		if (UPointLightComponent* Light = Cast<UPointLightComponent>(Actor->GetLightComponent()))
		{
			// Units before intensity (#385): the intensity is a number in those units, and a
			// spawned light starts Unitless whatever r.DefaultFeature.LightUnits says, so an
			// editor light authored in candelas would otherwise come back hundreds of times dimmer.
			Light->SetIntensityUnits(BuilderEngineLightUnits(Props.IntensityUnits));
			Light->SetIntensity(static_cast<float>(Props.Intensity));
			Light->SetLightColor(Props.Color);
			Light->SetAttenuationRadius(static_cast<float>(Props.Radius));
			Light->SetCastShadows(Props.bCastShadow);
			BuilderApplyTemperature(Light, Props.bUseTemperature, Props.Temperature);
			BuilderApplySourceShape(Light, Props.Source);
		}
		return Actor;
	}

	AActor* BuildSpotLight(UWorld* World, const FMantleEntity& Entity, const FMantleSpotLightProps& Props)
	{
		ASpotLight* Actor = SpawnBare<ASpotLight>(World, Entity.Name);
		if (!Actor)
		{
			return nullptr;
		}
		MakeMovable(Actor);

		if (USpotLightComponent* Light = Cast<USpotLightComponent>(Actor->GetLightComponent()))
		{
			Light->SetIntensityUnits(BuilderEngineLightUnits(Props.IntensityUnits));
			Light->SetIntensity(static_cast<float>(Props.Intensity));
			Light->SetLightColor(Props.Color);
			Light->SetAttenuationRadius(static_cast<float>(Props.Radius));
			Light->SetInnerConeAngle(static_cast<float>(Props.InnerCone));
			Light->SetOuterConeAngle(static_cast<float>(Props.OuterCone));
			Light->SetCastShadows(Props.bCastShadow);
			BuilderApplyTemperature(Light, Props.bUseTemperature, Props.Temperature);
			BuilderApplySourceShape(Light, Props.Source);
		}
		return Actor;
	}

	/** An area light (#472), in its units like a Point or Spot light (#385). */
	AActor* BuildRectLight(UWorld* World, const FMantleEntity& Entity, const FMantleRectLightProps& Props)
	{
		ARectLight* Actor = SpawnBare<ARectLight>(World, Entity.Name);
		if (!Actor)
		{
			return nullptr;
		}
		MakeMovable(Actor);

		if (URectLightComponent* Light = Cast<URectLightComponent>(Actor->GetLightComponent()))
		{
			Light->SetIntensityUnits(BuilderEngineLightUnits(Props.IntensityUnits));
			Light->SetIntensity(static_cast<float>(Props.Intensity));
			Light->SetLightColor(Props.Color);
			Light->SetAttenuationRadius(static_cast<float>(Props.Radius));
			Light->SetSourceWidth(static_cast<float>(Props.SourceWidth));
			Light->SetSourceHeight(static_cast<float>(Props.SourceHeight));
			Light->SetBarnDoorAngle(static_cast<float>(Props.BarnDoorAngle));
			Light->SetBarnDoorLength(static_cast<float>(Props.BarnDoorLength));
			Light->SetCastShadows(Props.bCastShadow);
			BuilderApplyTemperature(Light, Props.bUseTemperature, Props.Temperature);
		}
		return Actor;
	}

	/**
	 * A decal (#472): its uploaded textures, prepared by FMantlePlaceAssets before the build, on a
	 * dynamic instance of the cooked decal master. A dedicated server draws nothing and prepared
	 * neither texture, so it spawns the actor (the entity exists for scripts) without a material.
	 * A texture that did not load is an error at its property, and the decal is not built, as for a
	 * mesh; the master missing from this build is logged and leaves the decal without a material.
	 */
	AActor* BuildDecal(
		UWorld* World,
		const FMantleEntity& Entity,
		const FMantleDecalProps& Props,
		const FString& Path,
		TArray<FMantleParseError>& Errors,
		FMantleBuilderLoadStats& LoadStats)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(MantlePlaceBuilder_Decal);
		const FMantleAssetLibrary& Library = FMantleAssetLibrary::Get();
		const bool bDedicatedServer = World->GetNetMode() == NM_DedicatedServer;

		UTexture* BaseColor = nullptr;
		UTexture* Normal = nullptr;
		if (!bDedicatedServer)
		{
			FString Error;
			const double BaseColorLoadStart = FPlatformTime::Seconds();
			BaseColor = Library.LoadTexture(Props.BaseColor, EMantleTextureRole::Color, Error);
			LoadStats.Add(Props.BaseColor, BaseColorLoadStart);
			if (!BaseColor)
			{
				Errors.Add(FMantleParseError{ Path + TEXT("/properties/baseColor"), Error });
				return nullptr;
			}
			if (!Props.Normal.IsEmpty())
			{
				const double NormalLoadStart = FPlatformTime::Seconds();
				Normal = Library.LoadTexture(Props.Normal, EMantleTextureRole::Normal, Error);
				LoadStats.Add(Props.Normal, NormalLoadStart);
				if (!Normal)
				{
					Errors.Add(FMantleParseError{ Path + TEXT("/properties/normal"), Error });
					return nullptr;
				}
			}
		}

		ADecalActor* Actor = SpawnBare<ADecalActor>(World, Entity.Name);
		if (!Actor)
		{
			return nullptr;
		}
		MakeMovable(Actor);

		UDecalComponent* Decal = Actor->GetDecal();
		if (!Decal)
		{
			return Actor;
		}
		Decal->DecalSize = Props.Size;
		Decal->SetSortOrder(Props.SortOrder);
		Decal->SetFadeScreenSize(static_cast<float>(Props.FadeScreenSize));

		if (BaseColor)
		{
			// A normal map needs the master that writes normals; without that master in this build
			// the decal keeps its colour on the plain one and drops the normal map.
			UMaterialInterface* Parent = Normal ? FMantleDecalMaterials::Load(/*bNormal*/ true) : nullptr;
			const bool bWithNormal = Parent != nullptr;
			if (!Parent)
			{
				Parent = FMantleDecalMaterials::Load(/*bNormal*/ false);
			}
			if (Parent)
			{
				UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(Parent, Decal);
				Instance->SetTextureParameterValue(FName(FMantleDecalMaterials::BaseColorParameter), BaseColor);
				if (bWithNormal)
				{
					Instance->SetTextureParameterValue(FName(FMantleDecalMaterials::NormalParameter), Normal);
				}
				Instance->SetScalarParameterValue(FName(FMantleDecalMaterials::OpacityParameter), static_cast<float>(Props.Opacity));
				Decal->SetDecalMaterial(Instance);
			}
		}
		Decal->MarkRenderStateDirty();
		return Actor;
	}

	AActor* BuildDirectionalLight(
		UWorld* World,
		const FMantleEntity& Entity,
		const FMantleDirectionalLightProps& Props)
	{
		ADirectionalLight* Actor = SpawnBare<ADirectionalLight>(World, Entity.Name);
		if (!Actor)
		{
			return nullptr;
		}
		MakeMovable(Actor);

		if (UDirectionalLightComponent* Light = Cast<UDirectionalLightComponent>(Actor->GetLightComponent()))
		{
			Light->SetIntensity(static_cast<float>(Props.Intensity));
			Light->SetLightColor(Props.Color);
			Light->SetCastShadows(Props.bCastShadow);
		}
		return Actor;
	}

	AActor* BuildTriggerVolume(UWorld* World, const FMantleEntity& Entity, const FMantleTriggerVolumeProps& Props)
	{
		AActor* Actor = SpawnBare<AActor>(World, Entity.Name);
		if (!Actor)
		{
			return nullptr;
		}

		if (Props.Shape == EMantleTriggerShape::Sphere)
		{
			USphereComponent* Sphere = NewObject<USphereComponent>(Actor, TEXT("Trigger"));
			Actor->SetRootComponent(Sphere);
			Sphere->SetMobility(EComponentMobility::Movable);
			// A sphere has one radius; validation requires its three extents to match (#451).
			Sphere->SetSphereRadius(static_cast<float>(Props.Extent.X));
			Sphere->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
			Sphere->SetCollisionResponseToAllChannels(ECR_Overlap);
			Sphere->RegisterComponent();
		}
		else
		{
			UBoxComponent* Box = NewObject<UBoxComponent>(Actor, TEXT("Trigger"));
			Actor->SetRootComponent(Box);
			Box->SetMobility(EComponentMobility::Movable);
			Box->SetBoxExtent(Props.Extent);
			Box->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
			Box->SetCollisionResponseToAllChannels(ECR_Overlap);
			Box->RegisterComponent();
		}

		return Actor;
	}

	/**
	 * Gives an emitter's component its sound and, in a game world, starts it as the place file says
	 * (#263): shared by library sounds, bound as the place builds, and uploaded ones (#324), bound
	 * then or when their bytes arrive.
	 */
	void StartBuiltSoundEmitter(UWorld* World, UAudioComponent* Audio, USoundBase* Sound, const FMantleSoundEmitterProps& Props)
	{
		if (!Sound)
		{
			return;
		}

		// The component auto-activated when the actor spawned, before it had a sound, so it
		// never started. Playback is started explicitly below instead (#263).
		Audio->SetSound(Sound);

		// Only a game world plays: an editor level a place is imported into stays silent, and
		// keeps the actor's default auto-activation for whenever the creator plays it.
		if (!World->IsGameWorld())
		{
			return;
		}

		// A script stopped this emitter before its sound arrived (Entity:StopSound, #459), or the
		// place said not to autoplay it: bound, but left silent until a script plays it.
		if (Audio->ComponentHasTag(MantleSoundEmitterHeldTag()))
		{
			return;
		}

		if (const UMantleSoundWave* Uploaded = Cast<UMantleSoundWave>(Sound))
		{
			// An uploaded sound loops inside its own wave (gapless, #324), which was made for this
			// emitter's looping flag. A procedural wave never ends by itself, so a one-shot is
			// stopped once its samples have played, with a moment for the last buffer to drain.
			if (!Props.bLooping)
			{
				FTimerHandle StopAfterOnePass;
				World->GetTimerManager().SetTimer(
					StopAfterOnePass,
					FTimerDelegate::CreateWeakLambda(Audio, [Audio]() { Audio->Stop(); }),
					static_cast<float>(Uploaded->GetSourceDurationSeconds()) + 0.1f,
					/*bLoop*/ false);
			}
			Audio->Play();
			return;
		}

		// Looping comes from the place file, not the asset (the exporter copies the asset's flag,
		// but a hand-written or generated place file may say otherwise). A looping emitter whose
		// sound would end on its own is restarted when it does; a one-shot emitter whose sound
		// loops is stopped after one pass of the wave.
		if (Props.bLooping && !Sound->IsLooping())
		{
			Audio->OnAudioFinishedNative.AddWeakLambda(Audio, [](UAudioComponent* Finished)
			{
				// Stop() also lands here, so only a sound that ran out while its place is still
				// loaded is restarted, never one being torn down with the actor or the world.
				const AActor* Owner = Finished->GetOwner();
				const UWorld* OwnerWorld = Finished->GetWorld();
				if (IsValid(Owner) && !Owner->IsActorBeingDestroyed() && Finished->IsRegistered()
					&& OwnerWorld && !OwnerWorld->bIsTearingDown)
				{
					Finished->Play();
				}
			});
		}
		else if (!Props.bLooping && Sound->IsLooping())
		{
			if (const USoundWave* Wave = Cast<USoundWave>(Sound); Wave && Wave->Duration > 0.0f)
			{
				FTimerHandle StopAfterOnePass;
				World->GetTimerManager().SetTimer(
					StopAfterOnePass,
					FTimerDelegate::CreateWeakLambda(Audio, [Audio]() { Audio->Stop(); }),
					Wave->Duration,
					/*bLoop*/ false);
			}
		}

		Audio->Play();
	}

	AActor* BuildSoundEmitter(
		UWorld* World,
		const FMantleEntity& Entity,
		const FMantleSoundEmitterProps& Props,
		const FString& Path,
		TArray<FMantleParseError>& Errors,
		FMantleBuilderLoadStats& LoadStats)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(MantlePlaceBuilder_SoundEmitter);
		const FMantleAssetLibrary& Library = FMantleAssetLibrary::Get();

		// Nothing is heard on a dedicated server, so it neither loads the sound nor plays it.
		// A library id is still checked, so a place that fails on clients fails the same way
		// there, and the actor is still spawned so the entity exists for scripts. An uploaded
		// sound's kind was checked when the place was published (the backend refuses an id of
		// another kind), and the server resolves nothing it cannot hear.
		const bool bDedicatedServer = World->GetNetMode() == NM_DedicatedServer;
		// A library entry wins over the uploaded-id shape, so every sound id that built before #324 still does.
		const bool bUploaded = FMantlePlaceAssets::IsUploadedSoundId(Props.Sound);

		FString Error;
		USoundBase* Sound = nullptr;
		TSharedPtr<const FMantleSoundData> UploadedSound;
		bool bStillLoading = false;
		if (bDedicatedServer)
		{
			const FMantleAssetEntry* Entry = bUploaded ? nullptr : Library.Find(Props.Sound);
			if (!bUploaded && (!Entry || Entry->Kind != EMantleAssetKind::Sound))
			{
				Errors.Add(FMantleParseError{
					Path + TEXT("/properties/sound"),
					FString::Printf(TEXT("'%s' is not a known sound"), *Props.Sound) });
				return nullptr;
			}
		}
		else if (bUploaded)
		{
			// A creator's uploaded sound (#324), from the live place asset set. One still on its way
			// does not hold the place up: the emitter is built silent and bound when it arrives. One
			// that failed (missing, rejected, another kind of asset) is refused like a library miss.
			UploadedSound = FMantlePlaceAssets::FindLiveSound(Props.Sound, Error, &bStillLoading);
			if (!UploadedSound && !bStillLoading)
			{
				Errors.Add(FMantleParseError{ Path + TEXT("/properties/sound"), Error });
				return nullptr;
			}
		}
		else
		{
			const double SoundLoadStart = FPlatformTime::Seconds();
			Sound = Library.LoadSound(Props.Sound, Error);
			LoadStats.Add(Props.Sound, SoundLoadStart);
			if (!Sound)
			{
				Errors.Add(FMantleParseError{ Path + TEXT("/properties/sound"), Error });
				return nullptr;
			}
		}

		AAmbientSound* Actor = SpawnBare<AAmbientSound>(World, Entity.Name);
		if (!Actor)
		{
			return nullptr;
		}
		MakeMovable(Actor);

		UAudioComponent* Audio = Actor->GetAudioComponent();
		if (!Audio || bDedicatedServer)
		{
			return Actor;
		}

		Audio->SetVolumeMultiplier(static_cast<float>(Props.Volume));

		// Always spatialised, reaching silence at Radius: a 10% full-volume core and a linear
		// falloff over the rest, the proportions of UE's default attenuation (shared with the 3D
		// sounds scripts play, #459). The exporter reads the same GetMaxDimension() back, so an
		// imported place re-exports its own radius.
		Audio->bOverrideAttenuation = true;
		Audio->SetAttenuationOverrides(MantleSoundAttenuationForRadius(Props.Radius));

		if (!Props.bAutoplay)
		{
			// #459: silent until a script's Entity:PlaySound. An editor level a place is imported
			// into keeps it as the component's Auto Activate, which the exporter reads back; a game
			// holds it the way Entity:StopSound does, so its sound arriving later does not start it
			// either, and PlaySound lifts the hold. Written directly: the component is registered.
			Audio->bAutoActivate = false;
			if (World->IsGameWorld())
			{
				Audio->ComponentTags.AddUnique(MantleSoundEmitterHeldTag());
			}
		}

		if (Sound)
		{
			StartBuiltSoundEmitter(World, Audio, Sound, Props);
		}
		else if (UploadedSound)
		{
			// Each emitter its own wave over the shared samples, made for its own looping flag.
			StartBuiltSoundEmitter(World, Audio, UMantleSoundWave::Create(Audio, UploadedSound, Props.bLooping), Props);
		}
		else
		{
			const TWeakObjectPtr<UAudioComponent> WeakAudio(Audio);
			const FMantleSoundEmitterProps Bound = Props;
			const FString SoundPath = Path + TEXT("/properties/sound");
			FMantlePlaceAssets::WhenLiveSoundSettles(Props.Sound,
				[WeakAudio, Bound, SoundPath](TSharedPtr<const FMantleSoundData> Arrived, const FString& Why)
				{
					UAudioComponent* Late = WeakAudio.Get();
					const AActor* Owner = Late ? Late->GetOwner() : nullptr;
					UWorld* LateWorld = Late ? Late->GetWorld() : nullptr;
					if (!IsValid(Owner) || Owner->IsActorBeingDestroyed() || !LateWorld || LateWorld->bIsTearingDown)
					{
						return;
					}
					if (!Arrived)
					{
						// The place is already built and playing: said in the log, the emitter stays silent.
						UE_LOG(LogMantle, Error, TEXT("MANTLE_PLACE_BUILD %s: %s; that sound emitter stays silent"), *SoundPath, *Why);
						return;
					}
					StartBuiltSoundEmitter(LateWorld, Late, UMantleSoundWave::Create(Late, Arrived, Bound.bLooping), Bound);
					UE_LOG(LogMantle, Log, TEXT("MANTLE_PLACE_BUILD %s: uploaded sound %s bound as it arrived (%s)"),
						*SoundPath, *Bound.Sound, Bound.bLooping ? TEXT("looping") : TEXT("one-shot"));
				});
		}
		return Actor;
	}

	/**
	 * A SkeletalMesh (#469): an ASkeletalMeshActor showing the uploaded mesh, built on a skeleton of its own
	 * bones (FMantlePlaceAssets), playing its clip through UMantleClipAnimInstance at the server's clock
	 * (StartOffset + server time x PlayRate), so every client shows the same frame and nothing replicates. It
	 * has no collision, and ticks its pose only while rendered: its pose comes from the clock, not from what
	 * it did last frame. A clip on another skeleton than the mesh's is refused (publish refuses it first) and
	 * the mesh holds its reference pose. A dedicated server, which draws nothing, spawns only the entity's
	 * transform, so children and scripts still find it.
	 */
	AActor* BuildSkeletalMesh(
		UWorld* World,
		const FMantleEntity& Entity,
		const FMantleSkeletalMeshProps& Props,
		const FString& Path,
		TArray<FMantleParseError>& Errors,
		FMantleBuilderLoadStats& LoadStats)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(MantlePlaceBuilder_SkeletalMesh);

		if (World->GetNetMode() == NM_DedicatedServer)
		{
			AActor* Actor = SpawnBare<AActor>(World, Entity.Name);
			if (Actor)
			{
				USceneComponent* Root = NewObject<USceneComponent>(Actor, TEXT("Root"));
				Actor->SetRootComponent(Root);
				Root->SetMobility(EComponentMobility::Movable);
				Root->RegisterComponent();
			}
			return Actor;
		}

		// The Mantle library has no skeletal meshes: the mesh and clip are always uploads, from the live set.
		FString Error;
		const double MeshLoadStart = FPlatformTime::Seconds();
		USkeletalMesh* Mesh = FMantlePlaceAssets::IsUploadedId(Props.Mesh) ? FMantlePlaceAssets::FindLiveSkeletalMesh(Props.Mesh, Error) : nullptr;
		LoadStats.Add(Props.Mesh, MeshLoadStart);
		if (!Mesh)
		{
			Errors.Add(FMantleParseError{ Path + TEXT("/properties/mesh"),
				Error.IsEmpty() ? FString::Printf(TEXT("'%s' is not an uploaded skeletal mesh (the Mantle library has none)"), *Props.Mesh) : Error });
			return nullptr;
		}

		TSharedPtr<const FMantleAnimClip> Clip;
		if (!Props.Clip.IsEmpty())
		{
			FString ClipError;
			Clip = FMantlePlaceAssets::IsUploadedId(Props.Clip) ? FMantlePlaceAssets::FindLiveClip(Props.Clip, ClipError) : nullptr;
			const FString MeshSkeleton = FMantleSkeletonHash::Compute(Mesh->GetRefSkeleton());
			if (!Clip)
			{
				Errors.Add(FMantleParseError{ Path + TEXT("/properties/clip"),
					ClipError.IsEmpty() ? FString::Printf(TEXT("'%s' is not an uploaded animation clip"), *Props.Clip) : ClipError });
			}
			else if (!Clip->SkeletonHash.Equals(MeshSkeleton, ESearchCase::CaseSensitive))
			{
				Errors.Add(FMantleParseError{ Path + TEXT("/properties/clip"), FString::Printf(
					TEXT("clip %s was authored on skeleton %s, but mesh %s is skeleton %s; a clip plays only on the skeleton it was authored on"),
					*Props.Clip, *Clip->SkeletonHash, *Props.Mesh, *MeshSkeleton) });
				Clip.Reset();
			}
		}

		ASkeletalMeshActor* Actor = SpawnBare<ASkeletalMeshActor>(World, Entity.Name);
		if (!Actor)
		{
			Errors.Add(FMantleParseError{ Path, TEXT("failed to spawn skeletal mesh actor") });
			return nullptr;
		}
		MakeMovable(Actor);

		USkeletalMeshComponent* Component = Actor->GetSkeletalMeshComponent();
		Component->SetSkeletalMeshAsset(Mesh);
		Component->SetCastShadow(Props.bCastShadow);
		Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Component->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered;

		const FMantleAssetLibrary& Library = FMantleAssetLibrary::Get();
		for (int32 Slot = 0; Slot < Props.Materials.Num(); ++Slot)
		{
			if (Props.Materials[Slot].IsEmpty())
			{
				continue;
			}
			FString MaterialError;
			const double MaterialLoadStart = FPlatformTime::Seconds();
			UMaterialInterface* Material = Library.LoadMaterial(Props.Materials[Slot], MaterialError);
			LoadStats.Add(Props.Materials[Slot], MaterialLoadStart);
			if (Material)
			{
				Component->SetMaterial(Slot, Material);
			}
			else
			{
				Errors.Add(FMantleParseError{ FString::Printf(TEXT("%s/properties/materials/%d"), *Path, Slot), MaterialError });
			}
		}

		Component->SetAnimationMode(EAnimationMode::AnimationBlueprint);
		Component->SetAnimInstanceClass(UMantleClipAnimInstance::StaticClass());
		if (UMantleClipAnimInstance* Anim = Cast<UMantleClipAnimInstance>(Component->GetAnimInstance()))
		{
			FMantleClipPlayback Playback;
			Playback.bLoop = Props.bLoop;
			Playback.PlayRate = Props.PlayRate;
			Playback.StartOffset = Props.StartOffset;
			Anim->SetClip(Clip, Playback);
		}
		else if (Clip)
		{
			Errors.Add(FMantleParseError{ Path + TEXT("/properties/clip"), TEXT("the skeletal mesh could not start its clip player") });
		}
		return Actor;
	}

	/**
	 * A Terrain (#466): the heightfield from the live place asset set (a Terrain's heightfield is
	 * always uploaded) as chunk meshes with collision under one actor, and on a client its material,
	 * the layers' place materials blended by the weightmap in M_MantleTerrain. A dedicated server
	 * builds LOD 0 and collision only, and resolves no material or weightmap texture (nothing renders
	 * there). Where layers name surfaces (#596), every net mode adds a UMantleTerrainSurfaceComponent
	 * with the weightmap's heaviest layers, read on the CPU. Movable until pass 3 places it, then frozen
	 * to Static like a blocking mesh.
	 */
	AActor* BuildTerrainEntity(
		UWorld* World,
		const FMantleEntity& Entity,
		const FMantleTerrainProps& Props,
		const FString& Path,
		TArray<FMantleParseError>& Errors,
		FMantleBuilderLoadStats& LoadStats)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(MantlePlaceBuilder_Terrain);

		FString Error;
		const double HeightfieldStart = FPlatformTime::Seconds();
		const TSharedPtr<const FMantleHeightfield> Field = FMantlePlaceAssets::FindLiveHeightfield(Props.Heightfield, Error);
		LoadStats.Add(Props.Heightfield, HeightfieldStart);
		if (!Field.IsValid())
		{
			Errors.Add(FMantleParseError{ Path + TEXT("/properties/heightfield"), Error });
			return nullptr;
		}

		const bool bDedicatedServer = World->GetNetMode() == NM_DedicatedServer;
		UMaterialInterface* Material = nullptr;
		if (!bDedicatedServer)
		{
			TArray<FMantleTerrainLayerLook> Looks;
			for (int32 LayerIndex = 0; LayerIndex < Props.Layers.Num(); ++LayerIndex)
			{
				const FMantleTerrainLayer& Layer = Props.Layers[LayerIndex];
				FString MaterialError;
				const double MaterialLoadStart = FPlatformTime::Seconds();
				UMaterialInterface* LayerMaterial = FMantleAssetLibrary::Get().LoadMaterial(Layer.Material, MaterialError);
				LoadStats.Add(Layer.Material, MaterialLoadStart);
				if (!LayerMaterial)
				{
					Errors.Add(FMantleParseError{ FString::Printf(TEXT("%s/properties/layers/%d/material"), *Path, LayerIndex), MaterialError });
				}
				FMantleTerrainLayerLook& Look = Looks.AddDefaulted_GetRef();
				Look.Material = LayerMaterial;
				Look.TilingCm = Layer.Tiling;
			}
			UTexture* Weightmap = nullptr;
			if (!Props.Weightmap.IsEmpty())
			{
				FString WeightmapError;
				Weightmap = FMantlePlaceAssets::FindLiveTexture(Props.Weightmap, EMantleTextureRole::Data, WeightmapError);
				if (!Weightmap)
				{
					Errors.Add(FMantleParseError{ Path + TEXT("/properties/weightmap"), WeightmapError });
				}
			}
			// Layers 4 to 7 (#628) are weighed by the second one.
			UTexture* Weightmap2 = nullptr;
			if (!Props.Weightmap2.IsEmpty())
			{
				FString Weightmap2Error;
				Weightmap2 = FMantlePlaceAssets::FindLiveTexture(Props.Weightmap2, EMantleTextureRole::Data, Weightmap2Error);
				if (!Weightmap2)
				{
					Errors.Add(FMantleParseError{ Path + TEXT("/properties/weightmap2"), Weightmap2Error });
				}
			}
			FString Warning;
			Material = FMantleTerrainBuilder::BuildMaterial(*Field, Looks, Weightmap, Weightmap2, nullptr, &Warning);
			if (!Warning.IsEmpty())
			{
				UE_LOG(LogMantle, Warning, TEXT("MANTLE_PLACE_BUILD %s: %s"), *Path, *Warning);
			}
		}

		FMantleTerrainBuildParams Params;
		Params.bRenderLODs = !bDedicatedServer;
		FString BuildError;
		AActor* Actor = FMantleTerrainBuilder::Spawn(World, Entity.Name, *Field, Material, Props.bCastShadow, Params, &BuildError);
		if (!Actor)
		{
			Errors.Add(FMantleParseError{ Path + TEXT("/properties/heightfield"), BuildError });
			return nullptr;
		}

		// #596: the layers' surfaces, on every net mode, so a hit on the terrain resolves to the layer
		// that weighs most there (MantleSurfaceFromHit). A terrain whose layers name none gets nothing.
		if (UMantleTerrainSurfaceComponent::HasSurfaces(Props))
		{
			TSharedPtr<const FMantleTerrainWeights> Weights;
			TSharedPtr<const FMantleTerrainWeights> Weights2;
			if (UMantleTerrainSurfaceComponent::NeedsWeights(Props))
			{
				FString WeightsError;
				Weights = FMantlePlaceAssets::FindLiveTerrainWeights(Props.Weightmap, WeightsError);
				if (!Weights.IsValid())
				{
					UE_LOG(LogMantle, Warning, TEXT("MANTLE_PLACE_BUILD %s: the terrain's surfaces fall back to layer 0's: %s"), *Path, *WeightsError);
				}
				// Layers 4 to 7 (#628): the second weightmap's heaviest layers, weighed against the first's.
				if (!Props.Weightmap2.IsEmpty())
				{
					Weights2 = FMantlePlaceAssets::FindLiveTerrainWeights(Props.Weightmap2, WeightsError);
					if (!Weights2.IsValid())
					{
						UE_LOG(LogMantle, Warning, TEXT("MANTLE_PLACE_BUILD %s: the terrain's surfaces for layers 4 to 7 are not read: %s"), *Path, *WeightsError);
					}
				}
			}
			TArray<EMantleSurface> Surfaces;
			for (const FMantleTerrainLayer& Layer : Props.Layers)
			{
				Surfaces.Add(Layer.Surface);
			}
			UMantleTerrainSurfaceComponent* SurfaceComponent = NewObject<UMantleTerrainSurfaceComponent>(Actor, TEXT("Surfaces"));
			SurfaceComponent->Init(*Field, MoveTemp(Surfaces), Weights, Weights2);
			Actor->AddInstanceComponent(SurfaceComponent);
			SurfaceComponent->RegisterComponent();
		}

		// #628: the terrain's grass, where something draws and a game is played: a component that grows each
		// entry's grass around the local view. A dedicated server and an editor level (the importer's) get none.
		// An entry whose mesh, material or map cannot load is an error at its path and grows nothing (a bad
		// material leaves its slot on the mesh's own); the others still do.
		if (Props.Grass.Num() > 0 && FMantleScatter::DrawsScatter(World) && World->IsGameWorld())
		{
			TArray<FMantleGrassEntryAssets> Looks;
			TArray<TSharedPtr<const FMantleTerrainWeights>> Maps;
			Looks.SetNum(Props.Grass.Num());
			Maps.SetNum(Props.Grass.Num());
			for (int32 GrassIndex = 0; GrassIndex < Props.Grass.Num(); ++GrassIndex)
			{
				const FMantleTerrainGrass& Grass = Props.Grass[GrassIndex];
				const FString GrassPath = FString::Printf(TEXT("%s/properties/grass/%d"), *Path, GrassIndex);
				FString GrassError;
				const double MeshStart = FPlatformTime::Seconds();
				Looks[GrassIndex].Mesh = FMantleAssetLibrary::Get().LoadMesh(Grass.Mesh, GrassError);
				LoadStats.Add(Grass.Mesh, MeshStart);
				if (!Looks[GrassIndex].Mesh)
				{
					Errors.Add(FMantleParseError{ GrassPath + TEXT("/mesh"), FString::Printf(TEXT("'%s': %s"), *Grass.Mesh, *GrassError) });
					continue;
				}
				for (int32 Slot = 0; Slot < Grass.Materials.Num(); ++Slot)
				{
					UMaterialInterface* GrassMaterial = nullptr;
					if (!Grass.Materials[Slot].IsEmpty())
					{
						const double GrassMaterialStart = FPlatformTime::Seconds();
						GrassMaterial = FMantleAssetLibrary::Get().LoadMaterial(Grass.Materials[Slot], GrassError);
						LoadStats.Add(Grass.Materials[Slot], GrassMaterialStart);
						if (!GrassMaterial)
						{
							Errors.Add(FMantleParseError{ FString::Printf(TEXT("%s/materials/%d"), *GrassPath, Slot),
								FString::Printf(TEXT("'%s': %s"), *Grass.Materials[Slot], *GrassError) });
						}
					}
					Looks[GrassIndex].Materials.Add(GrassMaterial);
				}
				const double MapStart = FPlatformTime::Seconds();
				TSharedPtr<const FMantleTerrainWeights> Map = FMantlePlaceAssets::FindLiveTerrainWeights(Grass.Map, GrassError);
				LoadStats.Add(Grass.Map, MapStart);
				if (!Map.IsValid() || !Map->HasPixels())
				{
					Errors.Add(FMantleParseError{ GrassPath + TEXT("/map"),
						Map.IsValid() ? FString(TEXT("was read without its pixels")) : GrassError });
					Looks[GrassIndex].Mesh = nullptr;
					continue;
				}
				Maps[GrassIndex] = MoveTemp(Map);
			}
			UMantleTerrainGrassComponent* GrassComponent = NewObject<UMantleTerrainGrassComponent>(Actor, TEXT("Grass"));
			GrassComponent->Init(Field, Props.Grass, MoveTemp(Looks), MoveTemp(Maps));
			Actor->AddInstanceComponent(GrassComponent);
			GrassComponent->RegisterComponent();
		}
		return Actor;
	}

	/**
	 * A CrowdSpawner (#477) is data, built the same on every net mode: a hidden, unreplicated
	 * actor holding the spawner's settings with one linear spline per lane, for the editor's
	 * preview and import round trip. The NPCs themselves come from MantleNPC, on clients only,
	 * reading the retained FMantlePlace rather than this actor. Movable like every other non-mesh
	 * entity: pass 3 still has to place it, and the static freeze only concerns blocking meshes.
	 */
	AActor* BuildCrowdSpawner(UWorld* World, const FMantleEntity& Entity, const FMantleCrowdSpawnerProps& Props)
	{
		AMantleCrowdSpawner* Actor = SpawnBare<AMantleCrowdSpawner>(World, Entity.Name);
		if (!Actor)
		{
			return nullptr;
		}
		MakeMovable(Actor);
		Actor->ApplyProps(Props);
		return Actor;
	}

	/**
	 * A VehicleSpawn (#435) is data too: a hidden, unreplicated actor holding which vehicle and how soon it
	 * comes back, at the entity's transform. MantleGameplay's place vehicle bridge keeps a vehicle alive at
	 * it on the server; the vehicle itself replicates. Movable like every other non-mesh entity.
	 */
	AActor* BuildVehicleSpawn(UWorld* World, const FMantleEntity& Entity, const FMantleVehicleSpawnProps& Props)
	{
		AMantleVehicleSpawnPoint* Actor = SpawnBare<AMantleVehicleSpawnPoint>(World, Entity.Name);
		if (!Actor)
		{
			return nullptr;
		}
		MakeMovable(Actor);
		Actor->ApplyProps(Props);
		return Actor;
	}

	/**
	 * A WeaponSpawn (#789) is data too: a hidden, unreplicated actor holding which weapon, how soon it comes
	 * back and whether it is on, at the entity's transform. The pickup itself is MantleGameplay's.
	 */
	AActor* BuildWeaponSpawn(UWorld* World, const FMantleEntity& Entity, const FMantleWeaponSpawnProps& Props)
	{
		AMantleWeaponSpawnPoint* Actor = SpawnBare<AMantleWeaponSpawnPoint>(World, Entity.Name);
		if (!Actor)
		{
			return nullptr;
		}
		MakeMovable(Actor);
		Actor->ApplyProps(Props);
		return Actor;
	}

	/**
	 * A ScriptPoint (#838) is data too: a hidden, unreplicated actor at the entity's transform holding which stored
	 * script package it anchors and its parameters. The package's scripts read it as an entity (Script.Points).
	 */
	AActor* BuildScriptPoint(UWorld* World, const FMantleEntity& Entity, const FMantleScriptPointProps& Props)
	{
		AMantleScriptPoint* Actor = SpawnBare<AMantleScriptPoint>(World, Entity.Name);
		if (!Actor)
		{
			return nullptr;
		}
		MakeMovable(Actor);
		Actor->ApplyProps(Props);
		return Actor;
	}

	/**
	 * An Effect: an unreplicated AMantleEffect holding the archetype and its look, at the entity's
	 * transform (applied in pass 3; the effect's component follows its root). A machine that draws loads the
	 * archetype's cooked system and, when the effect autoplays, starts it; a dedicated server, or a build
	 * missing the system (logged once), keeps the actor and its state for scripts and draws nothing, and
	 * that is not an error. A sprites texture that fails to load is an error at its property, and the
	 * effect is not built.
	 */
	AActor* BuildEffect(UWorld* World, const FMantleEntity& Entity, const FMantleEffectProps& Props, const FString& Path,
		TArray<FMantleParseError>& Errors, FMantleBuilderLoadStats& LoadStats)
	{
		// A sprites effect's texture (#628), prepared by FMantlePlaceAssets before the build and not by a dedicated server,
		// which draws nothing. One that did not load is an error at its property, and the effect is not built, as a decal's.
		UTexture* Texture = nullptr;
		if (Props.Archetype == EMantleEffectArchetype::Sprites && !Props.TextureId.IsEmpty() && World->GetNetMode() != NM_DedicatedServer)
		{
			FString Error;
			const double LoadStart = FPlatformTime::Seconds();
			Texture = FMantleAssetLibrary::Get().LoadTexture(Props.TextureId, EMantleTextureRole::Color, Error);
			LoadStats.Add(Props.TextureId, LoadStart);
			if (!Texture)
			{
				Errors.Add(FMantleParseError{ Path + TEXT("/properties/texture"), Error });
				return nullptr;
			}
		}
		AMantleEffect* Actor = SpawnBare<AMantleEffect>(World, Entity.Name);
		if (!Actor)
		{
			return nullptr;
		}
		MakeMovable(Actor);
		Actor->ApplyProps(Props);
		Actor->SpritesTexture = Texture;
		Actor->BeginPlaceEffect();
		return Actor;
	}

	/**
	 * A Constraint (#528): an AMantleConstraint on every machine that plays the place, joined to its
	 * props after they start simulating (and only where they are the simulating props, the server's);
	 * an authoring build's is an engine Physics Constraint Actor the exporter reads back. Either is set
	 * up once every entity stands where the place puts it.
	 */
	AActor* BuildConstraint(UWorld* World, const FMantleEntity& Entity, const FMantleConstraintProps& Props, const FMantleBuildOptions& Options)
	{
		const bool bAuthoring = AMantlePhysicsProp::ChooseBuild(Options.bTransient && World->IsGameWorld(), World->GetNetMode())
			== EMantlePhysicsBuild::Authoring;
		if (bAuthoring)
		{
			APhysicsConstraintActor* Editor = SpawnBare<APhysicsConstraintActor>(World, Entity.Name);
			MakeMovable(Editor);
			return Editor;
		}
		AMantleConstraint* Actor = SpawnBare<AMantleConstraint>(World, Entity.Name);
		if (!Actor)
		{
			return nullptr;
		}
		MakeMovable(Actor);
		Actor->InitFromProps(Entity.Id, Props);
		return Actor;
	}

	/**
	 * A Scatter (#530): an unreplicated AMantleScatterVolume at the entity's transform (applied in pass 3; its
	 * instances are in its own space and follow its root). A machine that draws loads the meshes and generates
	 * and builds the instances now, through FMantleScatter; a dedicated server keeps the actor as the entity's
	 * transform node and builds nothing, since a scatter is visual only. A mesh or material that cannot load is
	 * an error at its path, and that mesh's instances are left out; the rest still draw.
	 */
	AActor* BuildScatter(UWorld* World, const FMantleEntity& Entity, const FMantleScatterParams& Props, const FString& Path,
		TArray<FMantleParseError>& Errors)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(MantlePlaceBuilder_Scatter);

		AMantleScatterVolume* Actor = SpawnBare<AMantleScatterVolume>(World, Entity.Name);
		if (!Actor)
		{
			return nullptr;
		}
		MakeMovable(Actor);
		Actor->ApplyProps(Props);
		if (!FMantleScatter::DrawsScatter(World))
		{
			return Actor;
		}

		const FString PropsPath = Path + TEXT("/properties");
		TArray<FMantleParseError> LoadErrors;
		Actor->LoadAssetsFromIds(LoadErrors);
		for (const FMantleParseError& LoadError : LoadErrors)
		{
			Errors.Add(FMantleParseError{ PropsPath + LoadError.Path, LoadError.Message });
		}
		const double BuildStart = FPlatformTime::Seconds();
		FString Error;
		if (!Actor->RefreshScatter(Error))
		{
			Errors.Add(FMantleParseError{ PropsPath, Error });
			return Actor;
		}
		// One line per scatter, so two clients' logs show they built the same instances.
		UE_LOG(LogMantle, Log, TEXT("MANTLE_SCATTER_BUILD entity=%s seed=%d instances=%d hash=%s build_ms=%.2f"),
			*Entity.Id, Props.Seed, Actor->GetInstanceCount(), *FMantleScatter::HashText(Actor->GetInstanceHash()),
			(FPlatformTime::Seconds() - BuildStart) * 1000.0);
		return Actor;
	}

	/**
	 * A ReflectionProbe (#628): an AMantleReflectionProbe at the entity's transform (applied in pass 3) holding
	 * its numbers. A machine that draws registers it with UMantleReflectionProbeSubsystem, which captures every
	 * probe once the build is over (NotifyPlaceBuilt); a dedicated server, a process that cannot render and a
	 * client with r.ReflectionCapture.Runtime off keep the actor and capture nothing, so the place still builds.
	 */
	AActor* BuildReflectionProbe(UWorld* World, const FMantleEntity& Entity, const FMantleReflectionProbeProps& Props)
	{
		AMantleReflectionProbe* Actor = SpawnBare<AMantleReflectionProbe>(World, Entity.Name);
		if (!Actor)
		{
			return nullptr;
		}
		MakeMovable(Actor);
		Actor->SetProbeProps(Props);
		if (FMantleReflectionProbes::WhyNotDrawn(World).IsEmpty())
		{
			if (UMantleReflectionProbeSubsystem* Probes = UMantleReflectionProbeSubsystem::Get(World))
			{
				Probes->Register(Actor);
			}
		}
		return Actor;
	}

	/** The engine's alignment for a Text's horizontalAlignment (#724). */
	EHorizTextAligment BuilderTextHorizontalAlignment(EMantleTextHorizontalAlignment Alignment)
	{
		switch (Alignment)
		{
		case EMantleTextHorizontalAlignment::Center: return EHTA_Center;
		case EMantleTextHorizontalAlignment::Right:  return EHTA_Right;
		default:                                     return EHTA_Left;
		}
	}

	/** The engine's alignment for a Text's verticalAlignment (#724): baseline is the engine's Quad Top. */
	EVerticalTextAligment BuilderTextVerticalAlignment(EMantleTextVerticalAlignment Alignment)
	{
		switch (Alignment)
		{
		case EMantleTextVerticalAlignment::Center:   return EVRTA_TextCenter;
		case EMantleTextVerticalAlignment::Bottom:   return EVRTA_TextBottom;
		case EMantleTextVerticalAlignment::Baseline: return EVRTA_QuadTop;
		default:                                     return EVRTA_TextTop;
		}
	}

	/**
	 * A Text (#724): an ATextRenderActor at the entity's transform (applied in pass 3). Its component keeps the
	 * engine's built-in font and text material, the class defaults, and takes every other key from the place. A
	 * dedicated server spawns the same actor, so the entity stands for attachment and scripts, and draws nothing.
	 */
	AActor* BuildText(UWorld* World, const FMantleEntity& Entity, const FMantleTextProps& Props)
	{
		ATextRenderActor* Actor = SpawnBare<ATextRenderActor>(World, Entity.Name);
		if (!Actor)
		{
			return nullptr;
		}
		MakeMovable(Actor);

		if (UTextRenderComponent* Text = Actor->GetTextRender())
		{
			// What a creator wrote, not a localizable string.
			Text->SetText(FText::AsCultureInvariant(Props.Text));
			// The place's colour is linear; the component's is an sRGB FColor, as the editor keeps it.
			Text->SetTextRenderColor(Props.Color.ToFColor(true));
			Text->SetWorldSize(static_cast<float>(Props.WorldSize));
			Text->SetHorizontalAlignment(BuilderTextHorizontalAlignment(Props.HorizontalAlignment));
			Text->SetVerticalAlignment(BuilderTextVerticalAlignment(Props.VerticalAlignment));
			Text->SetXScale(static_cast<float>(Props.XScale));
			Text->SetYScale(static_cast<float>(Props.YScale));
		}
		return Actor;
	}
}

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantlePlaceBuilderDecalPrivate
{
	/** Per master (0 colour, 1 normal): looked for once, and found missing. Game thread. */
	bool GDecalMasterChecked[2] = { false, false };
	bool GDecalMasterMissing[2] = { false, false };
}

UMaterialInterface* FMantleDecalMaterials::Load(const bool bNormal)
{
	using namespace MantlePlaceBuilderDecalPrivate;
	const int32 Index = bNormal ? 1 : 0;
	if (GDecalMasterMissing[Index])
	{
		return nullptr;
	}
	const FSoftObjectPath Path(bNormal ? NormalPath : ColorPath);
	UMaterialInterface* Loaded = Cast<UMaterialInterface>(Path.ResolveObject());
	if (!Loaded && (GDecalMasterChecked[Index] || FPackageName::DoesPackageExist(Path.GetLongPackageName())))
	{
		// Synchronous, once per master per process: the first decal pays it (#126).
		MANTLE_TRACE_SCOPE_DETAIL("MantleAssetLoad", Path.ToString());
		Loaded = LoadObject<UMaterialInterface>(nullptr, *Path.ToString(), nullptr, LOAD_NoWarn | LOAD_Quiet);
	}
	GDecalMasterChecked[Index] = true;
	if (!Loaded)
	{
		GDecalMasterMissing[Index] = true;
		UE_LOG(LogMantle, Error,
			TEXT("MANTLE_PLACE_DECAL %s is missing from this build (tools/materials/Create-DecalMaterial.py makes it): decals that need it draw nothing"),
			*Path.ToString());
	}
	return Loaded;
}

FMantleBuildResult FMantlePlaceBuilder::Build(
	UWorld* World,
	const FMantlePlace& Place,
	const FMantleBuildOptions& Options)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(MantlePlaceBuilder_Build);
	MANTLE_TRACE_SCOPE_DETAIL("MantlePlaceBuild", Place.PlaceId);
	const double BuildStartSeconds = FPlatformTime::Seconds();
	FMantleBuilderLoadStats LoadStats;

	FMantleBuildResult Result;

	if (!World)
	{
		Result.Errors.Add(FMantleParseError{ TEXT(""), TEXT("no world to build into") });
		return Result;
	}

	// Pass 1 — spawn everything. Children may appear before their parents in the array,
	// so nothing is attached until all actors exist.
	for (int32 i = 0; i < Place.Entities.Num(); ++i)
	{
		const FMantleEntity& Entity = Place.Entities[i];
		const FString Path = EntityPath(i);
		AActor* Actor = nullptr;

		switch (Entity.Type)
		{
		case EMantleEntityType::StaticMesh:
			if (const FMantleStaticMeshProps* Props = Entity.Props.TryGet<FMantleStaticMeshProps>())
			{
				Actor = BuildStaticMesh(World, Entity, *Props, Options, Path, Result.Errors, LoadStats);
			}
			break;

		case EMantleEntityType::Group:
			Actor = BuildGroup(World, Entity);
			break;

		case EMantleEntityType::PlayerStart:
			Actor = SpawnBare<APlayerStart>(World, Entity.Name);
			MakeMovable(Actor);
			break;

		case EMantleEntityType::PointLight:
			if (const FMantlePointLightProps* Props = Entity.Props.TryGet<FMantlePointLightProps>())
			{
				Actor = BuildPointLight(World, Entity, *Props);
			}
			break;

		case EMantleEntityType::SpotLight:
			if (const FMantleSpotLightProps* Props = Entity.Props.TryGet<FMantleSpotLightProps>())
			{
				Actor = BuildSpotLight(World, Entity, *Props);
			}
			break;

		case EMantleEntityType::DirectionalLight:
			if (const FMantleDirectionalLightProps* Props = Entity.Props.TryGet<FMantleDirectionalLightProps>())
			{
				Actor = BuildDirectionalLight(World, Entity, *Props);
			}
			break;

		case EMantleEntityType::TriggerVolume:
			if (const FMantleTriggerVolumeProps* Props = Entity.Props.TryGet<FMantleTriggerVolumeProps>())
			{
				Actor = BuildTriggerVolume(World, Entity, *Props);
			}
			break;

		case EMantleEntityType::SoundEmitter:
			if (const FMantleSoundEmitterProps* Props = Entity.Props.TryGet<FMantleSoundEmitterProps>())
			{
				Actor = BuildSoundEmitter(World, Entity, *Props, Path, Result.Errors, LoadStats);
			}
			break;

		case EMantleEntityType::InstancedMesh:
			if (const FMantleInstancedMeshProps* Props = Entity.Props.TryGet<FMantleInstancedMeshProps>())
			{
				Actor = BuildInstancedMesh(World, Entity, *Props, Path, Result.Errors, LoadStats);
			}
			break;

		case EMantleEntityType::CrowdSpawner:
			if (const FMantleCrowdSpawnerProps* Props = Entity.Props.TryGet<FMantleCrowdSpawnerProps>())
			{
				Actor = BuildCrowdSpawner(World, Entity, *Props);
			}
			break;

		case EMantleEntityType::SkeletalMesh:
			if (const FMantleSkeletalMeshProps* Props = Entity.Props.TryGet<FMantleSkeletalMeshProps>())
			{
				Actor = BuildSkeletalMesh(World, Entity, *Props, Path, Result.Errors, LoadStats);
			}
			break;

		case EMantleEntityType::RectLight:
			if (const FMantleRectLightProps* Props = Entity.Props.TryGet<FMantleRectLightProps>())
			{
				Actor = BuildRectLight(World, Entity, *Props);
			}
			break;

		case EMantleEntityType::Decal:
			if (const FMantleDecalProps* Props = Entity.Props.TryGet<FMantleDecalProps>())
			{
				Actor = BuildDecal(World, Entity, *Props, Path, Result.Errors, LoadStats);
			}
			break;

		case EMantleEntityType::Terrain:
			if (const FMantleTerrainProps* Props = Entity.Props.TryGet<FMantleTerrainProps>())
			{
				Actor = BuildTerrainEntity(World, Entity, *Props, Path, Result.Errors, LoadStats);
			}
			break;

		case EMantleEntityType::VehicleSpawn:
			if (const FMantleVehicleSpawnProps* Props = Entity.Props.TryGet<FMantleVehicleSpawnProps>())
			{
				Actor = BuildVehicleSpawn(World, Entity, *Props);
			}
			break;

		case EMantleEntityType::Effect:
			if (const FMantleEffectProps* Props = Entity.Props.TryGet<FMantleEffectProps>())
			{
				Actor = BuildEffect(World, Entity, *Props, Path, Result.Errors, LoadStats);
			}
			break;

		case EMantleEntityType::Constraint:
			if (const FMantleConstraintProps* Props = Entity.Props.TryGet<FMantleConstraintProps>())
			{
				Actor = BuildConstraint(World, Entity, *Props, Options);
			}
			break;

		case EMantleEntityType::Scatter:
			if (const FMantleScatterParams* Props = Entity.Props.TryGet<FMantleScatterParams>())
			{
				Actor = BuildScatter(World, Entity, *Props, Path, Result.Errors);
			}
			break;

		case EMantleEntityType::ReflectionProbe:
			if (const FMantleReflectionProbeProps* Props = Entity.Props.TryGet<FMantleReflectionProbeProps>())
			{
				Actor = BuildReflectionProbe(World, Entity, *Props);
			}
			break;

		case EMantleEntityType::WeaponSpawn:
			if (const FMantleWeaponSpawnProps* Props = Entity.Props.TryGet<FMantleWeaponSpawnProps>())
			{
				Actor = BuildWeaponSpawn(World, Entity, *Props);
			}
			break;

		case EMantleEntityType::ScriptPoint:
			if (const FMantleScriptPointProps* Props = Entity.Props.TryGet<FMantleScriptPointProps>())
			{
				Actor = BuildScriptPoint(World, Entity, *Props);
			}
			break;

		case EMantleEntityType::Text:
			if (const FMantleTextProps* Props = Entity.Props.TryGet<FMantleTextProps>())
			{
				Actor = BuildText(World, Entity, *Props);
			}
			break;

		case EMantleEntityType::WaterBody:
			// The parser reads and checks it (GhzGarage/mantle#997); the water is built by the runtime stage after this one,
			// so nothing stands for the entity yet and it is not an error.
			continue;
		}

		if (Actor)
		{
			if (Options.bTransient)
			{
				Actor->SetFlags(RF_Transient);
			}
			Result.Actors.Add(Entity.Id, Actor);
		}
		else
		{
			Result.Errors.Add(FMantleParseError{
				Path,
				FString::Printf(TEXT("could not build %s entity '%s'"),
					MantleEntityTypeToString(Entity.Type),
					*Entity.Id) });
		}
	}

	// Pass 2 — attach. Every actor exists by now, so order does not matter.
	for (int32 i = 0; i < Place.Entities.Num(); ++i)
	{
		const FMantleEntity& Entity = Place.Entities[i];
		if (Entity.Parent.IsEmpty())
		{
			continue;
		}

		AActor* const* Child = Result.Actors.Find(Entity.Id);
		AActor* const* Parent = Result.Actors.Find(Entity.Parent);
		if (!Child || !Parent)
		{
			// The parser already rejects dangling parents; reaching here means the parent
			// itself failed to build.
			continue;
		}

		(*Child)->AttachToActor(*Parent, FAttachmentTransformRules::KeepRelativeTransform);
	}

	// Pass 3 — apply transforms, which the schema defines as relative to parent.
	for (const FMantleEntity& Entity : Place.Entities)
	{
		AActor* const* Actor = Result.Actors.Find(Entity.Id);
		if (!Actor)
		{
			continue;
		}

		if (Entity.Parent.IsEmpty())
		{
			(*Actor)->SetActorTransform(Entity.Transform);
		}
		else
		{
			(*Actor)->SetActorRelativeTransform(Entity.Transform);
		}
	}

	// Physics props (#456) start once they stand where the place puts them: each detaches from its
	// parent (keeping the world transform its Group composed for it) and, unless it starts anchored,
	// begins simulating. It no longer follows that Group afterwards.
	for (const FMantleEntity& Entity : Place.Entities)
	{
		AActor* const* Actor = MantlePhysicsOf(Entity) ? Result.Actors.Find(Entity.Id) : nullptr;
		if (AMantlePhysicsProp* Prop = Actor ? Cast<AMantlePhysicsProp>(*Actor) : nullptr)
		{
			Prop->BeginSimulation();
		}
	}

	// Constraints (#528) join their props once those stand where the place puts them and simulate:
	// only where the props are the simulating AMantlePhysicsProps (the server, standalone play, the
	// preview). A connected client's are placeholders the server's props take over, so its
	// constraints stay unjoined: the server's joint moves the props every player sees. An authoring
	// build's engine constraint is pointed at its entities' actors.
	for (const FMantleEntity& Entity : Place.Entities)
	{
		const FMantleConstraintProps* Joint = Entity.Props.TryGet<FMantleConstraintProps>();
		AActor* const* Actor = Joint ? Result.Actors.Find(Entity.Id) : nullptr;
		if (!Actor)
		{
			continue;
		}
		AActor* const* ActorA = Result.Actors.Find(Joint->EntityA);
		AActor* const* ActorB = Joint->EntityB.IsEmpty() ? nullptr : Result.Actors.Find(Joint->EntityB);
		if (AMantleConstraint* Constraint = Cast<AMantleConstraint>(*Actor))
		{
			AMantlePhysicsProp* PropA = ActorA ? Cast<AMantlePhysicsProp>(*ActorA) : nullptr;
			AMantlePhysicsProp* PropB = ActorB ? Cast<AMantlePhysicsProp>(*ActorB) : nullptr;
			if (!PropA || (!Joint->EntityB.IsEmpty() && !PropB))
			{
				continue;
			}
			FString JoinError;
			if (!Constraint->Join(PropA->GetStaticMeshComponent(), PropB ? PropB->GetStaticMeshComponent() : nullptr, JoinError))
			{
				// The parser has checked what it names; this is a prop without a body (its mesh failed,
				// which is reported at the mesh). The place still plays, without this joint.
				UE_LOG(LogMantle, Warning, TEXT("MANTLE_CONSTRAINT entity=%s is not joined: %s"), *Entity.Id, *JoinError);
			}
		}
		else if (APhysicsConstraintActor* Editor = Cast<APhysicsConstraintActor>(*Actor))
		{
			FMantleConstraints::ConfigureEditorConstraint(Editor->GetConstraintComp(), *Joint, ActorA ? *ActorA : nullptr,
				ActorB ? *ActorB : nullptr);
		}
	}

	// ACharacter sends corrections relative to a Movable floor. Place geometry deliberately
	// does not replicate, so that movement base has no network identity on a client and the
	// correction is rejected. The first real two-client run exposed the result: both pawns
	// fell through their still-empty local worlds while every server correction named an
	// unresolvable base. Unparented blocking meshes are immutable place geometry in schema v1,
	// so freeze them only after their transform has been applied. Character movement then
	// treats them as static bases and sends absolute corrections.
	//
	// A blocking mesh under a Group is the same place geometry (a Blueprint prefab's floor or
	// walls, #468) when nothing above it can move: every ancestor a Group that is not dynamic.
	// Those Groups are frozen too, before anything under them, since a Static child cannot
	// follow a Movable parent. A mesh under anything else (a dynamic Group, a light, another
	// mesh) stays Movable, as every parented mesh did before.
	//
	// An entity marked "dynamic" opts out of this freeze even when it would otherwise
	// qualify: a script needs a real Movable actor to move, and a Static one
	// would silently refuse the transform exactly as the comment above describes. A dynamic
	// entity under a frozen Group stays Movable, which UE allows.
	//
	// An InstancedMesh (#467) is the same place geometry many times over, so it follows the same
	// rule: blocking, not dynamic and under nothing that can move means Static. So does a Terrain
	// (#466), which always blocks: its root and every chunk under it, the floor players stand on.
	// A Scatter (#530) blocks nothing, but it is immovable scenery all the same: frozen by the same
	// rule, its root and every instanced mesh under it, so its instances draw as static geometry.
	{
		TMap<FString, const FMantleEntity*> EntityById;
		for (const FMantleEntity& Entity : Place.Entities)
		{
			EntityById.Add(Entity.Id, &Entity);
		}

		TArray<const FMantleEntity*> GroupsToFreeze;
		TArray<const FMantleEntity*> MeshesToFreeze;
		for (const FMantleEntity& Entity : Place.Entities)
		{
			const bool bMesh = Entity.Type == EMantleEntityType::StaticMesh || Entity.Type == EMantleEntityType::InstancedMesh;
			const bool bTerrain = Entity.Type == EMantleEntityType::Terrain;
			const bool bScatter = Entity.Type == EMantleEntityType::Scatter;
			const FMantleStaticMeshProps* Props = bMesh ? MantleMeshPropsOf(Entity) : nullptr;
			const bool bBlocking = bTerrain || bScatter || (Props && Props->Collision == EMantleCollision::Blocking);
			if (!bBlocking || Entity.bDynamic)
			{
				continue;
			}

			// Every ancestor, innermost first, as long as each is a Group that isn't dynamic. The
			// parser refuses parent loops; the Contains check only keeps a bad place from hanging.
			TArray<const FMantleEntity*> Ancestors;
			bool bStaticChain = true;
			for (FString ParentId = Entity.Parent; !ParentId.IsEmpty(); )
			{
				const FMantleEntity* const* Parent = EntityById.Find(ParentId);
				if (!Parent || (*Parent)->Type != EMantleEntityType::Group || (*Parent)->bDynamic
					|| *Parent == &Entity || Ancestors.Contains(*Parent))
				{
					bStaticChain = false;
					break;
				}
				Ancestors.Add(*Parent);
				ParentId = (*Parent)->Parent;
			}
			if (!bStaticChain)
			{
				continue;
			}
			MeshesToFreeze.Add(&Entity);
			// Outermost first. A Group already listed keeps its place, so every Group comes after
			// all of its own ancestors.
			for (int32 Index = Ancestors.Num() - 1; Index >= 0; --Index)
			{
				GroupsToFreeze.AddUnique(Ancestors[Index]);
			}
		}

		auto FreezeEntity = [&Result](const FMantleEntity* Entity)
		{
			AActor* const* Actor = Result.Actors.Find(Entity->Id);
			if (Actor && (*Actor)->GetRootComponent())
			{
				(*Actor)->GetRootComponent()->SetMobility(EComponentMobility::Static);
				// A Terrain's chunks are components under its root (#466), each one place geometry; so
				// are a Scatter's instanced meshes (#530).
				if (Entity->Type == EMantleEntityType::Terrain || Entity->Type == EMantleEntityType::Scatter)
				{
					TInlineComponentArray<USceneComponent*> Parts(*Actor);
					for (USceneComponent* Part : Parts)
					{
						Part->SetMobility(EComponentMobility::Static);
					}
				}
			}
		};
		for (const FMantleEntity* Group : GroupsToFreeze)
		{
			FreezeEntity(Group);
		}
		for (const FMantleEntity* Mesh : MeshesToFreeze)
		{
			FreezeEntity(Mesh);
		}
	}

	// World gravity (#453). settings.gravity is a downward magnitude in cm/s^2 (980 is Earth), so
	// the engine's gravity Z is its negative. Every net mode sets it: the server and each client
	// both build the place, and character movement and physics read it on whichever side
	// simulates. A creator's editor level (the importer) is left alone: its gravity lives on the
	// settings actor, and the level's own world settings are not the place's to rewrite.
	if (World->IsGameWorld())
	{
		if (AWorldSettings* WorldSettings = World->GetWorldSettings())
		{
			const float GravityZ = static_cast<float>(-Place.Settings.Gravity);
			WorldSettings->bGlobalGravitySet = true;
			WorldSettings->GlobalGravityZ = GravityZ;
			// Also the cached value GetGravityZ returns, which is what the server replicates.
			WorldSettings->WorldGravityZ = GravityZ;
			WorldSettings->bWorldGravitySet = true;
		}
	}

	// Environment last: it needs pass 3's transforms resolved (to find the sun among built
	// DirectionalLight actors) and never itself becomes place geometry. An authoring build (the
	// importer) gets none of it, so the place's sun keeps its authored rotation.
	if (Options.bRuntimeEnvironment)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(MantlePlaceBuilder_Environment);
		FMantleEnvironment::Apply(World, Place, Result);
	}

	// Reflection probes (#628) are captured once the place stands and the sky light has had a few frames:
	// the builder only hands them over (UMantleReflectionProbeSubsystem), and says so when this world draws none.
	UMantleReflectionProbeSubsystem::NotifyPlaceBuilt(World, Place);

	Result.bSuccess = Result.Errors.Num() == 0;

	UE_LOG(LogMantle, Log,
		TEXT("Built place '%s' revision %d: %d of %d entities, %d error(s)"),
		*Place.PlaceId,
		Place.Revision.Number,
		Result.Actors.Num(),
		Place.Entities.Num(),
		Result.Errors.Num());

	// One line per build (#126), so a run without Insights still yields the numbers: the whole
	// Build on the game thread, the part of it spent in asset-library loads, and the longest single load.
	UE_LOG(LogMantle, Log,
		TEXT("MANTLE_PLACE_BUILD_TIMING place=%s revision=%d entities=%d total_ms=%.2f load_ms=%.2f loads=%d longest_load_ms=%.2f longest_load=%s"),
		*Place.PlaceId,
		Place.Revision.Number,
		Place.Entities.Num(),
		(FPlatformTime::Seconds() - BuildStartSeconds) * 1000.0,
		LoadStats.Seconds * 1000.0,
		LoadStats.Count,
		LoadStats.LongestSeconds * 1000.0,
		LoadStats.LongestId.IsEmpty() ? TEXT("-") : *LoadStats.LongestId);

	// The process's memory once the place stands (#630), for the large-place benchmark (tools/bench) on a
	// client and a dedicated server alike: what is in use now, and the most the process has held so far.
	const FPlatformMemoryStats MemoryStats = FPlatformMemory::GetStats();
	UE_LOG(LogMantle, Log,
		TEXT("MANTLE_PLACE_BUILD_MEMORY place=%s used_physical_mb=%.1f peak_physical_mb=%.1f used_virtual_mb=%.1f peak_virtual_mb=%.1f"),
		*Place.PlaceId,
		static_cast<double>(MemoryStats.UsedPhysical) / (1024.0 * 1024.0),
		static_cast<double>(MemoryStats.PeakUsedPhysical) / (1024.0 * 1024.0),
		static_cast<double>(MemoryStats.UsedVirtual) / (1024.0 * 1024.0),
		static_cast<double>(MemoryStats.PeakUsedVirtual) / (1024.0 * 1024.0));

	return Result;
}

void FMantlePlaceBuilder::Teardown(FMantleBuildResult& Result)
{
	for (const TPair<FString, AActor*>& Pair : Result.Actors)
	{
		if (IsValid(Pair.Value))
		{
			Pair.Value->Destroy();
		}
	}
	Result.Actors.Empty();

	for (AActor* Actor : Result.EnvironmentActors)
	{
		if (IsValid(Actor))
		{
			Actor->Destroy();
		}
	}
	Result.EnvironmentActors.Empty();

	Result.Errors.Empty();
	Result.bSuccess = false;
}
