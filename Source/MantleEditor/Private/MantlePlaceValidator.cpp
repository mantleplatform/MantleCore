// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceValidator.h"
#include "MantleActorDecompose.h"
#include "MantleAssetLibrary.h"
#include "MantleClientFeatures.h"
#include "Particles/ParticleSystemComponent.h"
#include "MantleCrowdSpawner.h"
#include "MantleEffect.h"
#include "MantleEnvironmentExport.h"
#include "MantlePhysicsProp.h"
#include "MantlePlaceAssets.h"
#include "MantlePlaceExporter.h"
#include "MantlePlaceLimits.h"
#include "MantlePlaceParser.h"
#include "MantlePlaceSettingsActor.h"
#include "MantlePlaceStorage.h"
#include "MantlePlaceWorldPartition.h"
#include "MantleReflectionProbeExport.h"
#include "MantleSeatExport.h"
#include "MantleRuntime.h"
#include "MantleSkyMesh.h"
#include "MantleScatterVolume.h"
#include "MantleScriptApiUsage.h"
#include "MantleScriptCatalogFiles.h"
#include "MantleScriptPackages.h"
#include "MantleScriptPoint.h"
#include "MantleSplineMeshBake.h"
#include "MantleTargetFeatures.h"
#include "MantleTextExport.h"
#include "MantleTerrainExport.h"
#include "MantleVehicleConverter.h"
#include "MantleVehicleSource.h"
#include "MantleVehicleSpawnPoint.h"
#include "MantleWeaponSpawnPoint.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"

#include "Animation/SkeletalMeshActor.h"
#include "Components/AudioComponent.h"
#include "Components/DecalComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/LightComponentBase.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SplineMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Engine/DecalActor.h"
#include "Engine/Level.h"
#include "Engine/LevelScriptBlueprint.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Internationalization/Regex.h"
#include "Materials/MaterialInterface.h"
#include "Misc/Paths.h"
#include "PhysicsEngine/BodySetup.h"
#include "Sound/AmbientSound.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundCue.h"
#include "UObject/SoftObjectPath.h"

namespace
{
	FString LabelOf(const AActor* Actor)
	{
#if WITH_EDITOR
		return Actor->GetActorLabel();
#else
		return Actor->GetName();
#endif
	}

	/**
	 * #456: a static mesh actor with Simulate Physics on publishes as a physics entity. Counted for
	 * the per-place budget, and warned about when it would not behave as it does in the editor: no
	 * physics collision at all (it will not move), or a mesh without simple collision (it simulates
	 * as a box the size of its bounds), or a physical material that matches none of the presets
	 * (#528: it publishes as default). True when the actor is a physics entity.
	 */
	bool CheckPhysicsForCheck(const AActor* Actor, EMantleEntityType Type, FMantleValidationResult& Result)
	{
		const AStaticMeshActor* MeshActor = Type == EMantleEntityType::StaticMesh ? Cast<AStaticMeshActor>(Actor) : nullptr;
		const UStaticMeshComponent* Component = MeshActor ? MeshActor->GetStaticMeshComponent() : nullptr;
		FMantlePhysicsProps Physics;
		FString MaterialNote;
		if (!FMantlePlaceExporter::ReadPhysics(Component, Physics, &MaterialNote))
		{
			if (Actor && Actor->ActorHasTag(MantlePhysicsAnchoredTag()))
			{
				// The tag only says how a physics entity starts; Simulate Physics makes one.
				Result.Issues.Add(FMantleValidationIssue{
					EMantleIssueSeverity::Warning,
					LabelOf(Actor),
					TEXT("has the MantleAnchored tag but does not simulate physics, so the tag does nothing."),
					TEXT("Tick Simulate Physics to make it a physics object that starts anchored, or remove the tag.") });
			}
			return false;
		}
		if (Component->GetCollisionEnabled() == ECollisionEnabled::NoCollision
			|| Component->GetCollisionEnabled() == ECollisionEnabled::QueryOnly)
		{
			Result.Issues.Add(FMantleValidationIssue{
				EMantleIssueSeverity::Warning,
				LabelOf(Actor),
				TEXT("simulates physics but has no physics collision, so it will not move in the published place."),
				TEXT("Set its collision to block (Collision Enabled: Query and Physics), or turn Simulate Physics off.") });
		}
		const UStaticMesh* Mesh = Component->GetStaticMesh();
		const UBodySetup* Body = Mesh ? Mesh->GetBodySetup() : nullptr;
		if (Mesh && (!Body || Body->AggGeom.GetElementCount() == 0))
		{
			Result.Issues.Add(FMantleValidationIssue{
				EMantleIssueSeverity::Warning,
				LabelOf(Actor),
				TEXT("simulates physics on a mesh with no simple collision, so it simulates as a box the size of its bounds."),
				TEXT("Add simple collision to the mesh (Static Mesh Editor, Collision menu) for a closer shape.") });
		}
		if (!MaterialNote.IsEmpty())
		{
			// #528: a place carries one of six surface presets, never a creator's own physical material.
			Result.Issues.Add(FMantleValidationIssue{
				EMantleIssueSeverity::Warning,
				LabelOf(Actor),
				MaterialNote,
				TEXT("Name its physical material after a preset: a word of the name picks it (\"PM_Rubber\", \"IceFloor\", \"Steel\" is metal, \"Concrete\" is stone; presets are rubber, ice, metal, wood and stone). To mark a surface for effects, name it after the surface (\"Concrete\" or \"PM_Concrete\").") });
		}
		return true;
	}

	/**
	 * Components that carry content — geometry or light. These are what a place can lose if
	 * an actor is published with more of them than its entity type can express.
	 */
	bool IsContentComponent(const UActorComponent* Component)
	{
		if (FMantleActorDecompose::IsVisualisation(Component))
		{
			return false;
		}
		// Baked into static meshes on its own (#628), checked with the level's spline meshes; not also an extra component.
		if (Component->IsA(USplineMeshComponent::StaticClass()))
		{
			return false;
		}
		for (const UClass* Class = Component->GetClass(); Class; Class = Class->GetSuperClass())
		{
			if (Class->GetFName() == TEXT("DynamicMeshComponent"))
			{
				return false;
			}
		}
		// Exported on its own as InstancedMesh entities (#467), never lost with the actor.
		if (const UInstancedStaticMeshComponent* Instanced = Cast<UInstancedStaticMeshComponent>(Component);
			Instanced && FMantlePlaceExporter::IsExportedInstanceSource(Instanced))
		{
			return false;
		}
		return Component->IsA(UPrimitiveComponent::StaticClass())
			|| Component->IsA(ULightComponentBase::StaticClass());
	}

	/** How many content components an entity type can represent. */
	int32 ExpectedContentComponents(EMantleEntityType Type)
	{
		switch (Type)
		{
		case EMantleEntityType::Group:
		case EMantleEntityType::SoundEmitter:
		case EMantleEntityType::CrowdSpawner: // its lanes are skipped in CheckComponents
		case EMantleEntityType::VehicleSpawn: // its arrow and icon are editor-only
		case EMantleEntityType::WeaponSpawn: // its arrow, icon and gun preview are editor-only
		case EMantleEntityType::ScriptPoint: // its arrow, icon and name labels are editor-only (#838)
		case EMantleEntityType::Effect: // its icon and preview are editor-only, and skipped in CheckComponents
		case EMantleEntityType::Constraint: // its root is the constraint itself, and its icon is editor-only
		case EMantleEntityType::Scatter: // its box is editor-only, and its instanced meshes are skipped in CheckComponents
			return 0;
		default:
			return 1;
		}
	}

	const TCHAR* SupportedTypeList()
	{
		return TEXT("Supported types are static meshes, instanced meshes and foliage, skeletal meshes playing one animation, groups, "
					"player starts, point/spot/rect/directional lights, decals, trigger volumes, ambient sounds, Mantle crowd spawners, Mantle vehicle spawns, "
					"Mantle weapon spawns, Mantle script points, Mantle effects, Mantle scatter volumes, box and sphere reflection captures, text render actors (world text) and physics constraints (a weld, hinge or rope between physics objects).");
	}

	void ValidatorCheckPrefabParts(const AActor* Actor, const TArray<FMantlePrefabPart>& Parts, FMantleValidationResult& Result);

	void CheckComponents(const AActor* Actor, EMantleEntityType Type, FMantleValidationResult& Result)
	{
		// A reflection capture (#628) publishes as its one capture component; its preview boxes are the editor's.
		if (Type == EMantleEntityType::ReflectionProbe || Type == EMantleEntityType::Text || Type == EMantleEntityType::WaterBody)
		{
			return;
		}

		// A Blueprint prefab (#468): its parts publish as the Group's children, and what can't
		// publish is a note, since the rest of the actor still reaches players. A group with no
		// parts is checked below as it always was.
		if (Type == EMantleEntityType::Group)
		{
			TArray<FMantlePrefabPart> Parts;
			TArray<FMantleUnpublishedPart> Unsupported;
			FMantlePlaceExporter::CollectPrefabParts(Actor, Parts, &Unsupported);
			// #628: a prefab whose only mesh was left out (a mirrored copy that could not be made) is still a prefab, not an extra-component error.
			if (Parts.Num() > 0 || Unsupported.Num() > 0)
			{
				for (const FMantleUnpublishedPart& Part : Unsupported)
				{
					Result.Issues.Add(FMantleValidationIssue{
						Part.bBlocksPublish ? EMantleIssueSeverity::Error : EMantleIssueSeverity::Warning,
						LabelOf(Actor),
						Part.What,
						Part.Remedy });
				}
				ValidatorCheckPrefabParts(Actor, Parts, Result);
				return;
			}
		}

		TArray<UActorComponent*> Components;
		Actor->GetComponents(Components);

		const int32 Allowed = ExpectedContentComponents(Type);
		int32 Seen = 0;

		for (const UActorComponent* Component : Components)
		{
			if (!IsContentComponent(Component))
			{
				continue;
			}
			// A crowd spawner's lanes are its content (#477), however many it has.
			if (Type == EMantleEntityType::CrowdSpawner && Component->IsA(UMantleCrowdLaneComponent::StaticClass()))
			{
				continue;
			}
			// An effect's own particle component (#460) is what it publishes as, not an extra.
			if (Type == EMantleEntityType::Effect && Component == CastChecked<AMantleEffect>(Actor)->GetEffectComponent())
			{
				continue;
			}
			// A scatter's instanced meshes (#530) are what it generates, not extras.
			if (Type == EMantleEntityType::Scatter && CastChecked<AMantleScatterVolume>(Actor)->IsScatterComponent(Component))
			{
				continue;
			}

			++Seen;
			if (Seen <= Allowed)
			{
				continue;
			}

			Result.Issues.Add(FMantleValidationIssue{
				EMantleIssueSeverity::Error,
				LabelOf(Actor),
				FString::Printf(
					TEXT("has an extra %s component ('%s') that Mantle cannot publish."),
					*Component->GetClass()->GetName(),
					*Component->GetName()),
				TEXT("Remove the component, or move it onto its own actor.") });
		}
	}

	/**
	 * What Export drops from an actor it does publish, without a word (#449). Export writes an
	 * entity from the actor's class and root component only, so a Blueprint's event graph and
	 * construction script never run in the place, and a component that is neither content (a
	 * primitive or a light, which CheckComponents counts) nor a bare transform (a plain scene
	 * component) is lost: audio, camera, spring arm, movement and the like. Warnings, not
	 * errors: the rest of the actor still publishes. One per actor for the logic, and one per
	 * actor listing every dropped component.
	 */
	void CheckDroppedBehaviourForCheck(const AActor* Actor, FMantleValidationResult& Result)
	{
		// The actor's own Blueprint and every Blueprint parent: a parent's graphs run too.
		TArray<FString> LogicWhere;
		if (FMantlePlaceExporter::DescribeBlueprintLogic(Actor, LogicWhere))
		{
			Result.Issues.Add(FMantleValidationIssue{
				EMantleIssueSeverity::Warning,
				LabelOf(Actor),
				FString::Printf(
					TEXT("has Blueprint logic (%s). Blueprint logic isn't published: the place gets this actor's components, not its behaviour."),
					*FString::Join(LogicWhere, TEXT(", "))),
				TEXT("Do this through a place script (a .luau, .js, .ts or .py file under the level's Scripts/Server or Scripts/Client folder).") });
		}

		const USceneComponent* Root = Actor->GetRootComponent();
		TArray<UActorComponent*> Components;
		Actor->GetComponents(Components);

		// A prefab's parts publish as entities of their own (#468), an audio component among them.
		// Its root is only the Group's transform, so a root that is something more is named too.
		TArray<FMantlePrefabPart> Parts;
		FMantlePlaceExporter::CollectPrefabParts(Actor, Parts);
		const bool bPrefab = Parts.Num() > 0;

		TArray<FString> Dropped;
		for (const UActorComponent* Component : Components)
		{
			// The root is what ClassifyActor published the actor as (an AmbientSound's audio
			// component included); content is CheckComponents' business; a plain scene
			// component carries only a transform; transient ones are the engine's own. And what
			// Export writes as entities of their own is not dropped: instance sources (#467) and a
			// prefab's parts (#468).
			// A prefab's audio component with a sound that can't publish has its own note, with the
			// reason (FMantlePlaceExporter::CollectPrefabParts).
			const UInstancedStaticMeshComponent* Instanced = Cast<UInstancedStaticMeshComponent>(Component);
			const UAudioComponent* Audio = Cast<UAudioComponent>(Component);
			if (!Component
				|| (bPrefab && Audio && Audio->Sound.Get())
				|| (Component == Root && !bPrefab)
				|| FMantleActorDecompose::IsVisualisation(Component)
				|| IsContentComponent(Component)
				|| Component->GetClass() == USceneComponent::StaticClass()
				|| Component->HasAnyFlags(RF_Transient)
				|| (Instanced && FMantlePlaceExporter::IsExportedInstanceSource(Instanced))
				|| Parts.ContainsByPredicate([Component](const FMantlePrefabPart& Part) { return Part.Component == Component; }))
			{
				continue;
			}
			Dropped.Add(FString::Printf(TEXT("'%s' (%s)"), *Component->GetName(), *Component->GetClass()->GetName()));
		}
		if (Dropped.Num() > 0)
		{
			Result.Issues.Add(FMantleValidationIssue{
				EMantleIssueSeverity::Warning,
				LabelOf(Actor),
				FString::Printf(
					TEXT("has components Mantle drops when publishing: %s. The rest of the actor publishes."),
					*FString::Join(Dropped, TEXT(", "))),
				TEXT("Remove them, or do what they did through a place script.") });
		}
	}

	/**
	 * The persistent level's Level Blueprint (#502). Its actor is engine infrastructure, so the
	 * per-actor check above never sees it, and Export publishes nothing of it. One place-wide
	 * warning naming the wired graphs; an untouched Level Blueprint says nothing.
	 */
	void CheckLevelBlueprintForCheck(UWorld* World, FMantleValidationResult& Result)
	{
#if WITH_EDITOR
		ULevel* Level = World->PersistentLevel;
		// bDontCreate: looking must not give the level a Level Blueprint it didn't have.
		const ULevelScriptBlueprint* Blueprint = Level ? Level->GetLevelScriptBlueprint(/*bDontCreate*/ true) : nullptr;
		if (!Blueprint)
		{
			return;
		}
		TArray<FString> LogicWhere;
		for (const UEdGraph* Graph : Blueprint->UbergraphPages)
		{
			if (FMantlePlaceExporter::GraphHasWiredNode(Graph))
			{
				LogicWhere.AddUnique(Graph->GetName());
			}
		}
		for (const UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			if (FMantlePlaceExporter::GraphHasWiredNode(Graph))
			{
				LogicWhere.AddUnique(FString::Printf(TEXT("function %s"), *Graph->GetName()));
			}
		}
		if (LogicWhere.Num() > 0)
		{
			Result.Issues.Add(FMantleValidationIssue{
				EMantleIssueSeverity::Warning,
				FString(),
				FString::Printf(
					TEXT("The level's Level Blueprint has logic (%s). Level Blueprint logic isn't published: the place gets the level's actors, not what its Level Blueprint does."),
					*FString::Join(LogicWhere, TEXT(", "))),
				TEXT("Use a place script under the level's Scripts/Server or Scripts/Client folder (a .luau, .js, .ts or .py file).") });
		}
#endif
	}

	void CheckAssetsAndTextures(const AActor* Actor, EMantleEntityType Type, FMantleValidationResult& Result)
	{
		if (Type != EMantleEntityType::StaticMesh)
		{
			return;
		}

		const AStaticMeshActor* MeshActor = CastChecked<AStaticMeshActor>(Actor);
		const UStaticMeshComponent* Component = MeshActor->GetStaticMeshComponent();

		const UStaticMesh* Mesh = Component->GetStaticMesh();
		if (!Mesh)
		{
			Result.Issues.Add(FMantleValidationIssue{
				EMantleIssueSeverity::Error,
				LabelOf(Actor),
				TEXT("has no mesh assigned."),
				TEXT("Pick a mesh, or delete the actor.") });
		}

		// A mesh or material that is not in the library is converted and uploaded with the place
		// (F5 #339); FMantlePlaceAssets::CheckLevel checks each of those once, after this loop,
		// textures over the size cap included (FMantlePlaceAssets::TextureDownscaleIssue).
		// Library materials have no texture check (#452): a place publishes only their id, and
		// every client loads Mantle's own cooked copy, so a creator's local textures never ship.
	}

	/** The same rule Export applies, through the same function, so Check cannot pass what Publish refuses (#263). */
	void CheckSound(const AActor* Actor, EMantleEntityType Type, FMantleValidationResult& Result)
	{
		if (Type != EMantleEntityType::SoundEmitter)
		{
			return;
		}

		FMantleSoundEmitterProps Props;
		FString Problem;
		if (!FMantlePlaceExporter::ReadSoundEmitter(Actor, Props, Problem))
		{
			// The problem names the sound, what is wrong with it and how to fix it (#324).
			const FMantlePlaceAssetLimits Limits = FMantlePlaceAssetLimits::FromContract();
			Result.Issues.Add(FMantleValidationIssue{
				EMantleIssueSeverity::Error,
				LabelOf(Actor),
				Problem,
				FString::Printf(TEXT("Your own sounds publish with your place: assign a sound wave imported from a WAV file (mono or stereo, %d to %d Hz, at most %g seconds), or delete this AmbientSound."),
					Limits.MinSoundSampleRate, Limits.MaxSoundSampleRate, Limits.MaxSoundSeconds) });
		}
		else if (const AAmbientSound* Ambient = Cast<AAmbientSound>(Actor);
			Ambient && Ambient->GetAudioComponent() && Ambient->GetAudioComponent()->Sound.Get() && Ambient->GetAudioComponent()->Sound->IsA<USoundCue>())
		{
			// #818: a Sound Cue publishes as the sound wave it plays first.
			const USoundBase* Wave = FMantlePlaceAssets::PublishedSound(Ambient->GetAudioComponent()->Sound.Get());
			Result.Issues.Add(FMantleValidationIssue{
				EMantleIssueSeverity::Warning,
				LabelOf(Actor),
				FString::Printf(TEXT("uses Sound Cue '%s'. Mantle publishes sound waves, so it publishes as the one wave the cue plays first%s; the cue's mixing and randomness are not kept."),
					*Ambient->GetAudioComponent()->Sound->GetName(), Wave ? *FString::Printf(TEXT(", '%s'"), *Wave->GetName()) : TEXT("")),
				TEXT("Fine if that wave is the sound you want. Otherwise assign the sound wave you want to the Ambient Sound.") });
		}
	}

	FString DescribeVector(const FVector& Value)
	{
		return FString::Printf(TEXT("(%g, %g, %g)"), Value.X, Value.Y, Value.Z);
	}

	bool HasNonPositiveComponent(const FVector& Value)
	{
		return Value.X <= 0.0 || Value.Y <= 0.0 || Value.Z <= 0.0;
	}

	/**
	 * A decal (#472), through the exporter's reader, so Check cannot pass a decal Publish refuses:
	 * a decal material with a BaseColor texture set, and the schema's ranges on what Export writes.
	 * A decal material other than Mantle's publishes as its textures alone, which is a warning.
	 */
	void CheckDecal(const AActor* Actor, EMantleEntityType Type, FMantleValidationResult& Result)
	{
		if (Type != EMantleEntityType::Decal)
		{
			return;
		}

		const FString Label = LabelOf(Actor);
		FMantleDecalProps Props;
		UTexture* BaseColor = nullptr;
		UTexture* Normal = nullptr;
		FString Problem;
		if (!FMantlePlaceExporter::ReadDecal(Actor, Props, BaseColor, Normal, Problem))
		{
			// The problem says what to do.
			Result.Issues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Error, Label, Problem, FString() });
			return;
		}

		const ADecalActor* DecalActor = Cast<ADecalActor>(Actor);
		const UMaterialInterface* Material = DecalActor && DecalActor->GetDecal() ? DecalActor->GetDecal()->GetDecalMaterial() : nullptr;
		if (Material && !FMantlePlaceExporter::IsMantleDecalMaterial(Material))
		{
			Result.Issues.Add(FMantleValidationIssue{
				EMantleIssueSeverity::Warning,
				Label,
				FString::Printf(
					TEXT("uses its own decal material '%s'. Mantle publishes a decal as its BaseColor%s texture drawn by Mantle's decal material, so anything else this material does is dropped."),
					*Material->GetName(), Normal ? TEXT(" and Normal") : TEXT("")),
				TEXT("Fine if the material only shows that texture. Otherwise, only the BaseColor and Normal textures and the Opacity value are kept: move anything else into them.") });
		}

		if (HasNonPositiveComponent(Props.Size))
		{
			Result.Issues.Add(FMantleValidationIssue{
				EMantleIssueSeverity::Error,
				Label,
				FString::Printf(TEXT("has a decal size of %s; Mantle publishes decals above zero in size on every axis."), *DescribeVector(Props.Size)),
				TEXT("Give the decal's Decal Size a value above zero on every axis.") });
		}
		if (Props.SortOrder < -FMantlePlaceLimits::MaxDecalSortOrder || Props.SortOrder > FMantlePlaceLimits::MaxDecalSortOrder)
		{
			Result.Issues.Add(FMantleValidationIssue{
				EMantleIssueSeverity::Error,
				Label,
				FString::Printf(TEXT("has a sort order of %d; Mantle publishes sort orders from -%d to %d."),
					Props.SortOrder, FMantlePlaceLimits::MaxDecalSortOrder, FMantlePlaceLimits::MaxDecalSortOrder),
				FString::Printf(TEXT("Set the decal's Sort Order between -%d and %d."), FMantlePlaceLimits::MaxDecalSortOrder, FMantlePlaceLimits::MaxDecalSortOrder) });
		}
		if (Props.FadeScreenSize < 0.0 || Props.FadeScreenSize > 1.0)
		{
			Result.Issues.Add(FMantleValidationIssue{
				EMantleIssueSeverity::Error,
				Label,
				FString::Printf(TEXT("has a fade screen size of %g; Mantle publishes fade screen sizes from 0 to 1."), Props.FadeScreenSize),
				TEXT("Set the decal's Fade Screen Size between 0 (never fades) and 1.") });
		}
	}

	/**
	 * A skeletal mesh actor (#469) through the one rule Export applies (FMantlePlaceAssets::ReadSkeletalComponent), so
	 * Check cannot pass what Publish refuses: an Animation Blueprint, a custom mode, a montage or blend space, a sequence
	 * for another skeleton. The mesh and the clip's own limits are FMantlePlaceAssets::CheckLevel's, once each.
	 */
	void ValidatorCheckSkeletalMesh(const AActor* Actor, EMantleEntityType Type, FMantleValidationResult& Result)
	{
		if (Type != EMantleEntityType::SkeletalMesh)
		{
			return;
		}
		const ASkeletalMeshActor* SkeletalActor = Cast<ASkeletalMeshActor>(Actor);
		FMantleSkeletalPlayback Playback;
		FString Why;
		FString Fix;
		if (!FMantlePlaceAssets::ReadSkeletalComponent(SkeletalActor ? SkeletalActor->GetSkeletalMeshComponent() : nullptr, Playback, Why, Fix))
		{
			Result.Issues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Error, LabelOf(Actor), Why, Fix });
		}
	}

	/**
	 * A crowd spawner's own limits (#477), through the exporter's reader, so Check cannot pass a
	 * spawner Publish refuses. The place-wide totals are counted in Validate.
	 */
	void CheckCrowdSpawner(const AActor* Actor, EMantleEntityType Type, FMantleValidationResult& Result)
	{
		if (Type != EMantleEntityType::CrowdSpawner)
		{
			return;
		}

		FMantleCrowdSpawnerProps Props;
		FString Problem;
		if (!FMantlePlaceExporter::ReadCrowdSpawner(Actor, Props, Problem))
		{
			// The problem says what to fix; the remedy says where.
			Result.Issues.Add(FMantleValidationIssue{
				EMantleIssueSeverity::Error,
				LabelOf(Actor),
				Problem,
				TEXT("Change it in the spawner's Details panel, or on the lane component it names.") });
		}
	}

	/** Whether the place's settings list a storage entry called Name. */
	bool StorageDeclared(UWorld* World, const FString& Name)
	{
		const AMantlePlaceSettingsActor* PlaceSettings = AMantlePlaceSettingsActor::Find(World);
		return PlaceSettings && PlaceSettings->Storage.ContainsByPredicate([&Name](const FMantleStorageSetting& Entry)
		{
			return Entry.Name.Equals(Name, ESearchCase::CaseSensitive);
		});
	}

	/** The error for a spawn that names a storage entry the place does not list. */
	void AddUnlistedStorageIssue(const AActor* Actor, const FString& Name, FMantleValidationResult& Result)
	{
		Result.Issues.Add(FMantleValidationIssue{
			EMantleIssueSeverity::Error,
			LabelOf(Actor),
			FString::Printf(TEXT("names storage '%s', which Mantle Place Settings > Storage does not list."), *Name),
			TEXT("Add an entry with that name to the place's Storage, or set the spawn's Storage Item to one of its names.") });
	}

	/**
	 * A Mantle Vehicle Spawn (#435): a vehicle to park, a respawn time within the schema's limit. The vehicle's own
	 * rules (its body, wheels and tuning) are FMantlePlaceAssets::CheckLevel's, once per vehicle.
	 */
	void CheckVehicleSpawn(const AActor* Actor, EMantleEntityType Type, int32& VehicleSpawns, FMantleValidationResult& Result)
	{
		const AMantleVehicleSpawnPoint* Spawn = Type == EMantleEntityType::VehicleSpawn ? Cast<AMantleVehicleSpawnPoint>(Actor) : nullptr;
		if (!Spawn)
		{
			return;
		}
		if (++VehicleSpawns == FMantlePlaceLimits::MaxVehicleSpawnsPerPlace + 1)
		{
			Result.Issues.Add(FMantleValidationIssue{
				EMantleIssueSeverity::Error,
				LabelOf(Actor),
				FString::Printf(TEXT("is vehicle spawn %d; a place has at most %d."), VehicleSpawns, FMantlePlaceLimits::MaxVehicleSpawnsPerPlace),
				TEXT("Remove some vehicle spawns (a script can spawn more vehicles while the place runs).") });
		}
		FMantleVehicleSpawnProps Props;
		FString Problem;
		if (!Spawn->ReadProps(Props, Problem))
		{
			Result.Issues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Error, LabelOf(Actor), Problem, TEXT("Change it in the spawn's Details panel.") });
		}
		if (!Props.StorageItem.IsEmpty())
		{
			// #614 stage 4: a stored vehicle. The item's own rules (kind, ownership, existence) are the backend's at
			// publish; here only what the place itself can know.
			const bool bDeclared = StorageDeclared(Actor->GetWorld(), Props.StorageItem);
			if (!Spawn->Vehicle.IsNull() || !Props.Vehicle.IsEmpty())
			{
				Result.Issues.Add(FMantleValidationIssue{
					EMantleIssueSeverity::Error,
					LabelOf(Actor),
					TEXT("sets both a Vehicle and a Storage Item."),
					TEXT("Clear one of them: a spawn parks either a vehicle you built (publishing adds it to your library and the place's storage) or a vehicle already in the place's storage.") });
			}
			else if (!bDeclared)
			{
				AddUnlistedStorageIssue(Actor, Props.StorageItem, Result);
			}
		}
		else if (Spawn->Vehicle.IsNull() && !FMantleVehicleConverter::IsUploadedId(Props.Vehicle))
		{
			Result.Issues.Add(FMantleValidationIssue{
				EMantleIssueSeverity::Error,
				LabelOf(Actor),
				TEXT("has no vehicle to park."),
				TEXT("Put a vehicle in your level with Tools > Mantle > Build a vehicle, or add one from your library (Tools > Mantle > Library) to the place's Storage and set this spawn's Storage Item to its name.") });
		}
		else if (!Spawn->Vehicle.IsNull() && !Spawn->Vehicle.LoadSynchronous())
		{
			Result.Issues.Add(FMantleValidationIssue{
				EMantleIssueSeverity::Error,
				LabelOf(Actor),
				FString::Printf(TEXT("names a vehicle that cannot be loaded (%s)."), *Spawn->Vehicle.ToString()),
				TEXT("Set its Vehicle again, or clear it and pick another.") });
		}
		// #488: a picked livery is one of the vehicle's own (publish refuses one past its cooked list too).
		else if (const UMantleVehicleSource* Source = Spawn->Vehicle.LoadSynchronous(); Source && Props.Livery >= Source->Paint.Liveries.Num())
		{
			Result.Issues.Add(FMantleValidationIssue{
				EMantleIssueSeverity::Error,
				LabelOf(Actor),
				FString::Printf(TEXT("wears livery %d, but its vehicle '%s' has %d liveries."), Props.Livery, *Source->GetName(), Source->Paint.Liveries.Num()),
				TEXT("Set Livery to -1 (the vehicle's default) or to one of the vehicle's Liveries' index (the first is 0).") });
		}
	}

	/**
	 * A Mantle Weapon Spawn (#789): a weapon to leave lying (a built one, a stored one or, from a place file, an uploaded one) and a
	 * respawn time within the schema's limit. The weapon's own files are checked once per weapon by FMantlePlaceAssets::CheckLevel.
	 */
	void CheckWeaponSpawn(const AActor* Actor, EMantleEntityType Type, int32& WeaponSpawns, FMantleValidationResult& Result)
	{
		const AMantleWeaponSpawnPoint* Spawn = Type == EMantleEntityType::WeaponSpawn ? Cast<AMantleWeaponSpawnPoint>(Actor) : nullptr;
		if (!Spawn)
		{
			return;
		}
		if (++WeaponSpawns == FMantlePlaceLimits::MaxWeaponSpawnsPerPlace + 1)
		{
			Result.Issues.Add(FMantleValidationIssue{
				EMantleIssueSeverity::Error,
				LabelOf(Actor),
				FString::Printf(TEXT("is weapon spawn %d; a place has at most %d."), WeaponSpawns, FMantlePlaceLimits::MaxWeaponSpawnsPerPlace),
				TEXT("Remove some weapon spawns.") });
		}
		FMantleWeaponSpawnProps Props;
		FString Problem;
		if (!Spawn->ReadProps(Props, Problem))
		{
			Result.Issues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Error, LabelOf(Actor), Problem, TEXT("Change it in the spawn's Details panel.") });
		}
		if (!Props.StorageItem.IsEmpty())
		{
			const bool bDeclared = StorageDeclared(Actor->GetWorld(), Props.StorageItem);
			if (!Spawn->Weapon.IsEmpty() || !Props.Weapon.IsEmpty())
			{
				Result.Issues.Add(FMantleValidationIssue{
					EMantleIssueSeverity::Error,
					LabelOf(Actor),
					TEXT("sets both a Weapon and a Storage Item."),
					TEXT("Clear one of them: a spawn leaves either a weapon you built or a weapon from the place's storage.") });
			}
			else if (!bDeclared)
			{
				AddUnlistedStorageIssue(Actor, Props.StorageItem, Result);
			}
		}
		else if (!Spawn->Weapon.IsEmpty())
		{
			const bool bBuilt = AMantleWeaponSpawnPoint::IsBuilderName(Spawn->Weapon)
				&& IFileManager::Get().FileExists(*AMantleWeaponSpawnPoint::BuiltDocumentFile(Spawn->Weapon))
				&& FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(AMantleWeaponSpawnPoint::BuiltMeshObjectPath(Spawn->Weapon)));
			if (!bBuilt)
			{
				Result.Issues.Add(FMantleValidationIssue{
					EMantleIssueSeverity::Error,
					LabelOf(Actor),
					FString::Printf(TEXT("uses the weapon '%s', whose files are not in your project's MantleWeapons folder."), *Spawn->Weapon),
					TEXT("Pick a weapon from its Weapon list, or make the weapon again with Tools > Mantle > Build a weapon.") });
			}
		}
		else if (!FMantleVehicleConverter::IsUploadedId(Props.Weapon))
		{
			Result.Issues.Add(FMantleValidationIssue{
				EMantleIssueSeverity::Error,
				LabelOf(Actor),
				TEXT("has no weapon to leave."),
				TEXT("Pick one in its Weapon list in the Details panel. Make a weapon first with Tools > Mantle > Build a weapon.") });
		}
	}

	/**
	 * A Mantle Effect (#460): its look within the schema's limits, at most MaxEffectsPerPlace of them, and a
	 * warning when this Mantle Core has no system for its archetype (the effect then shows nowhere).
	 */
	void CheckEffect(const AActor* Actor, EMantleEntityType Type, int32& Effects, FMantleValidationResult& Result)
	{
		const AMantleEffect* Effect = Type == EMantleEntityType::Effect ? Cast<AMantleEffect>(Actor) : nullptr;
		if (!Effect)
		{
			return;
		}
		if (++Effects == FMantlePlaceLimits::MaxEffectsPerPlace + 1)
		{
			Result.Issues.Add(FMantleValidationIssue{
				EMantleIssueSeverity::Error,
				LabelOf(Actor),
				FString::Printf(TEXT("is effect %d; a place has at most %d."), Effects, FMantlePlaceLimits::MaxEffectsPerPlace),
				TEXT("Remove some effects (a script's Effects:Play can add short ones while the place runs).") });
		}
		FMantleEffectProps Props;
		FString Problem;
		if (!Effect->ReadProps(Props, Problem))
		{
			Result.Issues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Error, LabelOf(Actor), Problem, TEXT("Change it in the effect's Details panel.") });
		}
		const FSoftObjectPath System(FMantleEffects::SystemPath(Props.Archetype));
		if (!FPackageName::DoesPackageExist(System.GetLongPackageName()))
		{
			Result.Issues.Add(FMantleValidationIssue{
				EMantleIssueSeverity::Warning,
				LabelOf(Actor),
				FString::Printf(TEXT("is a %s effect, whose system (%s) this Mantle Core does not have yet, so it shows nothing here or for players."),
					FMantleEffects::ArchetypeDisplayName(Props.Archetype), *System.ToString()),
				TEXT("Update the Mantle Core plugin; the place still publishes, and the effect appears once players' clients have it.") });
		}
	}

	/**
	 * A Mantle Scatter Volume (#530): its settings within the schema's limits and its instance count under the
	 * per-scatter cap (AMantleScatterVolume::ReadProps), each mesh set and publishable (a library mesh, a project
	 * mesh, whose upload limits CheckLevel holds it to, or an id it was imported with that Mantle knows), and the
	 * place's scatter instances under MaxScatterInstancesPerPlace, named at the volume that crosses it.
	 */
	void ValidatorCheckScatter(const AActor* Actor, EMantleEntityType Type, int64& ScatterInstances, FMantleValidationResult& Result)
	{
		const AMantleScatterVolume* Volume = Type == EMantleEntityType::Scatter ? Cast<AMantleScatterVolume>(Actor) : nullptr;
		if (!Volume)
		{
			return;
		}
		FMantleScatterParams Props;
		FString Problem;
		if (!Volume->ReadProps(Props, Problem))
		{
			Result.Issues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Error, LabelOf(Actor), Problem,
				TEXT("Change it in the scatter volume's Details panel: a smaller box or a lower Density makes fewer instances.") });
			return;
		}
		for (int32 Index = 0; Index < Volume->Meshes.Num(); ++Index)
		{
			const FMantleScatterMeshSettings& Entry = Volume->Meshes[Index];
			if (Entry.Mesh || FMantleAssetLibrary::Get().IsKnown(Entry.MeshId) || FMantleVehicleConverter::IsUploadedId(Entry.MeshId))
			{
				continue;
			}
			Result.Issues.Add(FMantleValidationIssue{
				EMantleIssueSeverity::Error,
				LabelOf(Actor),
				FString::Printf(TEXT("names mesh %d as '%s', which is neither a Mantle library mesh nor an uploaded one."), Index + 1, *Entry.MeshId),
				TEXT("Pick a Static Mesh for that entry under Meshes.") });
		}
		FString Why;
		const int64 Count = FMantleScatter::CountFor(Props, Why);
		const int64 Before = ScatterInstances;
		ScatterInstances += FMath::Max<int64>(Count, 0);
		if (Before <= FMantlePlaceLimits::MaxScatterInstancesPerPlace && ScatterInstances > FMantlePlaceLimits::MaxScatterInstancesPerPlace)
		{
			Result.Issues.Add(FMantleValidationIssue{
				EMantleIssueSeverity::Error,
				LabelOf(Actor),
				FString::Printf(TEXT("brings the place to %lld scatter instances; a place holds at most %d."),
					static_cast<long long>(ScatterInstances), FMantlePlaceLimits::MaxScatterInstancesPerPlace),
				TEXT("Lower the Density or shrink the box of this or another scatter volume.") });
		}
	}

	/**
	 * A Physics Constraint Actor that publishes (#528): counted for the per-place cap, which the parser
	 * and backend refuse past MaxConstraintsPerPlace. One that can't publish never gets here: it is
	 * named, with what to change, by DescribeUnpublishedContent. The warning past
	 * WarnConstraintsPerPlace is Validate's, once for the level.
	 */
	void CheckConstraint(const AActor* Actor, EMantleEntityType Type, int32& Constraints, FMantleValidationResult& Result)
	{
		if (Type != EMantleEntityType::Constraint)
		{
			return;
		}
		if (++Constraints == FMantlePlaceLimits::MaxConstraintsPerPlace + 1)
		{
			Result.Issues.Add(FMantleValidationIssue{
				EMantleIssueSeverity::Error,
				LabelOf(Actor),
				FString::Printf(TEXT("is physics constraint %d; a place has at most %d."), Constraints, FMantlePlaceLimits::MaxConstraintsPerPlace),
				TEXT("Remove some constraints (a server script's Constraints:Create can join objects while the place runs, within the same cap).") });
		}
	}

	/**
	 * A mesh component's material overrides, which Export writes by slot with null padding for the
	 * unset slots before the last one set (#262): the count is the last overridden slot plus one,
	 * not how many are set. Subject is put before the problem, after the actor's label: empty for
	 * the actor itself, "(component 'Name') " for a prefab's part (#468).
	 */
	void ValidatorCheckMaterialSlots(const FString& Label, const FString& Subject, const UStaticMeshComponent* Component, FMantleValidationResult& Result)
	{
		int32 WrittenSlots = 0;
		for (int32 i = Component->OverrideMaterials.Num() - 1; i >= 0; --i)
		{
			if (Component->OverrideMaterials[i])
			{
				WrittenSlots = i + 1;
				break;
			}
		}
		if (WrittenSlots > FMantlePlaceLimits::MaxMaterialSlots)
		{
			Result.Issues.Add(FMantleValidationIssue{
				EMantleIssueSeverity::Error,
				Label,
				Subject + FString::Printf(
					TEXT("overrides material slot %d; Mantle publishes overrides on slots 0 to 15 only."),
					WrittenSlots - 1),
				TEXT("Clear the material overrides on slot 16 and above, or use a mesh with 16 material slots or fewer.") });
		}
	}

	/**
	 * The schema's size limits on what the exporter writes for an actor, counted as the parser
	 * and the backend count them, so "Check this place" fails where the upload would.
	 */
	void CheckPublishedSizeLimits(const AActor* Actor, EMantleEntityType Type, FMantleValidationResult& Result)
	{
		// The exporter writes the actor label as the entity name, untrimmed.
		const FString Label = LabelOf(Actor);
		const int32 LabelLength = FMantlePlaceParser::CodePointLength(Label);
		if (LabelLength > FMantlePlaceLimits::MaxEntityNameLength)
		{
			Result.Issues.Add(FMantleValidationIssue{
				EMantleIssueSeverity::Error,
				Label,
				FString::Printf(TEXT("has a name %d characters long; Mantle publishes names of at most 80."), LabelLength),
				TEXT("Rename the actor to 80 characters or fewer.") });
		}

		for (const FName& Tag : Actor->Tags)
		{
			const FString TagText = Tag.ToString();
			const int32 TagLength = FMantlePlaceParser::CodePointLength(TagText);
			if (TagLength > FMantlePlaceLimits::MaxTagLength)
			{
				Result.Issues.Add(FMantleValidationIssue{
					EMantleIssueSeverity::Error,
					Label,
					FString::Printf(
						TEXT("has tag '%s', %d characters long; Mantle publishes tags of at most 40."),
						*TagText,
						TagLength),
					TEXT("Shorten the tag to 40 characters or fewer.") });
			}
		}

		if (Type != EMantleEntityType::StaticMesh)
		{
			return;
		}

		ValidatorCheckMaterialSlots(Label, FString(), CastChecked<AStaticMeshActor>(Actor)->GetStaticMeshComponent(), Result);
	}

	/** The positive scale the schema requires of a published transform; Subject as for ValidatorCheckMaterialSlots. */
	void ValidatorCheckScale(const FString& Label, const FString& Subject, const FVector& Scale, FMantleValidationResult& Result)
	{
		if (HasNonPositiveComponent(Scale))
		{
			Result.Issues.Add(FMantleValidationIssue{
				EMantleIssueSeverity::Error,
				Label,
				Subject + FString::Printf(TEXT("has a scale of %s; Mantle publishes scales above zero on every axis."), *DescribeVector(Scale)),
				TEXT("Set every scale axis above zero. To mirror a mesh, use a mirrored copy of the mesh rather than a negative scale.") });
		}
	}

	/**
	 * The schema's ranges on the light and trigger values Export writes (#393), read by the exporter's
	 * own readers into Values. Subject as for ValidatorCheckMaterialSlots.
	 */
	void ValidatorCheckValueRanges(const FString& Label, const FString& Subject, const FMantleEntity& Values, FMantleValidationResult& Result)
	{
		auto IntensityIssue = [&Result, &Label, &Subject](double Intensity, double Max, const TCHAR* What)
		{
			if (Intensity < 0.0 || Intensity > Max)
			{
				Result.Issues.Add(FMantleValidationIssue{
					EMantleIssueSeverity::Error,
					Label,
					Subject + FString::Printf(TEXT("has an intensity of %g; Mantle publishes %s intensities from 0 to %g."), Intensity, What, Max),
					FString::Printf(TEXT("Set the light's intensity between 0 and %g."), Max) });
			}
		};
		auto RadiusIssue = [&Result, &Label, &Subject](double Radius)
		{
			if (Radius <= 0.0)
			{
				Result.Issues.Add(FMantleValidationIssue{
					EMantleIssueSeverity::Error,
					Label,
					Subject + FString::Printf(TEXT("has an attenuation radius of %g; Mantle publishes radii above zero."), Radius),
					TEXT("Set the light's attenuation radius above zero.") });
			}
		};
		auto ConeIssue = [&Result, &Label, &Subject](double Degrees, const TCHAR* Which)
		{
			if (Degrees < 0.0 || Degrees > FMantlePlaceLimits::MaxSpotConeDegrees)
			{
				Result.Issues.Add(FMantleValidationIssue{
					EMantleIssueSeverity::Error,
					Label,
					Subject + FString::Printf(TEXT("has an %s cone angle of %g; Mantle publishes cone angles from 0 to %g degrees."), Which, Degrees, FMantlePlaceLimits::MaxSpotConeDegrees),
					FString::Printf(TEXT("Set the %s cone angle between 0 and %g."), Which, FMantlePlaceLimits::MaxSpotConeDegrees) });
			}
		};

		// #472: a temperature is published only when used, so only then is it held to the range.
		auto TemperatureIssue = [&Result, &Label, &Subject](bool bUseTemperature, double Temperature)
		{
			if (bUseTemperature && (Temperature < FMantlePlaceLimits::MinLightTemperature || Temperature > FMantlePlaceLimits::MaxLightTemperature))
			{
				Result.Issues.Add(FMantleValidationIssue{
					EMantleIssueSeverity::Error,
					Label,
					Subject + FString::Printf(TEXT("has a temperature of %g K; Mantle publishes temperatures from %g to %g K."),
						Temperature, FMantlePlaceLimits::MinLightTemperature, FMantlePlaceLimits::MaxLightTemperature),
					FString::Printf(TEXT("Set the light's Temperature between %g and %g, or turn Use Temperature off."),
						FMantlePlaceLimits::MinLightTemperature, FMantlePlaceLimits::MaxLightTemperature) });
			}
		};
		// A size from 0 up to Max, in cm: a Point or Spot light's source, a RectLight's rectangle and barn doors.
		auto SizeIssue = [&Result, &Label, &Subject](double Value, double Max, const TCHAR* What, const TCHAR* Setting)
		{
			if (Value < 0.0 || Value > Max)
			{
				Result.Issues.Add(FMantleValidationIssue{
					EMantleIssueSeverity::Error,
					Label,
					Subject + FString::Printf(TEXT("has a %s of %g; Mantle publishes a %s from 0 to %g."), What, Value, What, Max),
					FString::Printf(TEXT("Set the light's %s between 0 and %g."), Setting, Max) });
			}
		};
		auto SourceIssues = [&SizeIssue](const FMantleLightSourceShape& Source)
		{
			SizeIssue(Source.SourceRadius, FMantlePlaceLimits::MaxLightSourceSize, TEXT("source radius"), TEXT("Source Radius"));
			SizeIssue(Source.SoftSourceRadius, FMantlePlaceLimits::MaxLightSourceSize, TEXT("soft source radius"), TEXT("Soft Source Radius"));
			SizeIssue(Source.SourceLength, FMantlePlaceLimits::MaxLightSourceSize, TEXT("source length"), TEXT("Source Length"));
		};

		if (const FMantlePointLightProps* Point = Values.Props.TryGet<FMantlePointLightProps>())
		{
			IntensityIssue(Point->Intensity, FMantlePlaceLimits::MaxLocalLightIntensity, TEXT("point light"));
			RadiusIssue(Point->Radius);
			TemperatureIssue(Point->bUseTemperature, Point->Temperature);
			SourceIssues(Point->Source);
		}
		else if (const FMantleSpotLightProps* Spot = Values.Props.TryGet<FMantleSpotLightProps>())
		{
			IntensityIssue(Spot->Intensity, FMantlePlaceLimits::MaxLocalLightIntensity, TEXT("spot light"));
			RadiusIssue(Spot->Radius);
			ConeIssue(Spot->InnerCone, TEXT("inner"));
			ConeIssue(Spot->OuterCone, TEXT("outer"));
			TemperatureIssue(Spot->bUseTemperature, Spot->Temperature);
			SourceIssues(Spot->Source);
		}
		else if (const FMantleRectLightProps* Rect = Values.Props.TryGet<FMantleRectLightProps>())
		{
			IntensityIssue(Rect->Intensity, FMantlePlaceLimits::MaxLocalLightIntensity, TEXT("rect light"));
			RadiusIssue(Rect->Radius);
			TemperatureIssue(Rect->bUseTemperature, Rect->Temperature);
			SizeIssue(Rect->SourceWidth, FMantlePlaceLimits::MaxRectLightSourceSize, TEXT("source width"), TEXT("Source Width"));
			SizeIssue(Rect->SourceHeight, FMantlePlaceLimits::MaxRectLightSourceSize, TEXT("source height"), TEXT("Source Height"));
			SizeIssue(Rect->BarnDoorAngle, FMantlePlaceLimits::MaxRectLightBarnDoorAngle, TEXT("barn door angle"), TEXT("Barn Door Angle"));
			SizeIssue(Rect->BarnDoorLength, FMantlePlaceLimits::MaxRectLightBarnDoorLength, TEXT("barn door length"), TEXT("Barn Door Length"));
		}
		else if (const FMantleDirectionalLightProps* Directional = Values.Props.TryGet<FMantleDirectionalLightProps>())
		{
			IntensityIssue(Directional->Intensity, FMantlePlaceLimits::MaxDirectionalLightIntensity, TEXT("directional light"));
		}
		else if (const FMantleTriggerVolumeProps* Trigger = Values.Props.TryGet<FMantleTriggerVolumeProps>())
		{
			if (HasNonPositiveComponent(Trigger->Extent))
			{
				Result.Issues.Add(FMantleValidationIssue{
					EMantleIssueSeverity::Error,
					Label,
					Subject + FString::Printf(TEXT("has a trigger extent of %s; Mantle publishes trigger volumes above zero in size on every axis."), *DescribeVector(Trigger->Extent)),
					TEXT("Give the trigger's box extent (or sphere radius) a size above zero.") });
			}
		}
	}

	/**
	 * The schema's numeric ranges on what Export writes for an actor: a positive scale, light
	 * intensities, radii and cones, and positive trigger extents (#393). The values come from the
	 * exporter's own readers and the limits from FMantlePlaceLimits, which the parser uses too, so
	 * Check fails here instead of the draft save failing after every upload has run.
	 */
	void CheckPublishedRanges(const AActor* Actor, EMantleEntityType Type, FMantleValidationResult& Result)
	{
		const FString Label = LabelOf(Actor);
		FVector PublishedScale = FMantlePlaceExporter::PublishedTransform(Actor).GetScale3D();
		if (Type == EMantleEntityType::StaticMesh)
		{
			// #628: a negative scale publishes as a mirrored copy of the mesh at the positive scale; no error. One that can't be
			// copied is left out of the place, which is a warning with the reason.
			const FMantleMirrorResult Mirror = FMantleActorDecompose::MirrorForActor(Actor);
			if (Mirror.bNeeded && !Mirror.bOk)
			{
				Result.Issues.Add(FMantleValidationIssue{
					EMantleIssueSeverity::Warning,
					Label,
					FString::Printf(TEXT("has a scale of %s and a mirrored copy of its mesh could not be made (%s), so it is left out of the place."),
						*DescribeVector(PublishedScale), *Mirror.Why),
					TEXT("Use a mesh mirrored in your modelling tool, or a positive scale.") });
				return;
			}
			if (Mirror.bNeeded)
			{
				PublishedScale = Mirror.PositiveScale;
			}
		}
		// #818: a decal with a negative scale publishes at the positive scale, unmirrored; a warning, not an error.
		if (Type == EMantleEntityType::Text && FMantleActorDecompose::HasNegativeAxis(PublishedScale))
		{
			// Mantle draws text unmirrored at the positive scale; the export's coverage report says so too.
			Result.Issues.Add(FMantleValidationIssue{
				EMantleIssueSeverity::Warning,
				Label,
				FString::Printf(TEXT("has a scale of %s; its negative scale mirrors the text, and Mantle draws it unmirrored."), *DescribeVector(PublishedScale)),
				TEXT("Fine if the mirroring does not matter. Otherwise use a positive scale.") });
			PublishedScale = PublishedScale.GetAbs();
		}
		if (Type == EMantleEntityType::Decal && FMantleActorDecompose::HasNegativeAxis(PublishedScale))
		{
			Result.Issues.Add(FMantleValidationIssue{
				EMantleIssueSeverity::Warning,
				Label,
				FString::Printf(TEXT("has a scale of %s; Mantle places a decal at the positive scale, so its picture is not mirrored."), *DescribeVector(PublishedScale)),
				TEXT("Fine if the mirroring does not matter. Otherwise mirror the decal's texture and use a positive scale.") });
			PublishedScale = PublishedScale.GetAbs();
		}
		ValidatorCheckScale(Label, FString(), PublishedScale, Result);

		FMantleEntity Values;
		if (FMantlePlaceExporter::ReadValueProperties(Actor, Type, Values))
		{
			ValidatorCheckValueRanges(Label, FString(), Values, Result);
		}
	}

	/**
	 * A prefab's parts (#468), held to the same limits as an actor of the part's type, through the
	 * exporter's readers: the scale of the transform Export writes for each, a mesh's material
	 * overrides, and light and trigger values. Each issue names the component after the actor.
	 */
	void ValidatorCheckPrefabParts(const AActor* Actor, const TArray<FMantlePrefabPart>& Parts, FMantleValidationResult& Result)
	{
		const FString Label = LabelOf(Actor);
		for (const FMantlePrefabPart& Part : Parts)
		{
			const FString Subject = FString::Printf(TEXT("(component '%s') "), *Part.Component->GetName());
			ValidatorCheckScale(Label, Subject, Part.Transform.GetScale3D(), Result);
			if (const UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Part.Component))
			{
				ValidatorCheckMaterialSlots(Label, Subject, Mesh, Result);
			}
			FMantleEntity Values;
			if (FMantlePlaceExporter::ReadComponentValueProperties(Part.Component, Part.Type, Values))
			{
				ValidatorCheckValueRanges(Label, Subject, Values, Result);
			}
		}
	}

	/**
	 * The backend's rules for a script's path and name (#393): the schema's path pattern, at most
	 * FMantlePlaceLimits::MaxScriptPathLength characters, and a name (the file name without its
	 * extension) of 1 to MaxScriptNameLength. RelativePath is relative to Scripts/.
	 */
	void CheckScriptNaming(const FString& RelativePath, const FString& Name, FMantleValidationResult& Result)
	{
		const int32 PathLength = FMantlePlaceParser::CodePointLength(RelativePath);
		if (PathLength > FMantlePlaceLimits::MaxScriptPathLength)
		{
			Result.Issues.Add(FMantleValidationIssue{
				EMantleIssueSeverity::Error,
				FString(),
				FString::Printf(TEXT("Scripts/%s has a path %d characters long; Mantle publishes script paths of at most %d."),
					*RelativePath, PathLength, FMantlePlaceLimits::MaxScriptPathLength),
				TEXT("Shorten the file name or its folders.") });
		}
		else if (!FMantlePlaceLimits::IsScriptPath(RelativePath))
		{
			Result.Issues.Add(FMantleValidationIssue{
				EMantleIssueSeverity::Error,
				FString(),
				FString::Printf(TEXT("Scripts/%s has a character Mantle does not publish in a script path."), *RelativePath),
				TEXT("Use only letters, digits, '_', '.' and '-' in script file and folder names (no spaces or brackets), for example Door_1.luau rather than 'Door (1).luau'.") });
		}

		const int32 NameLength = FMantlePlaceParser::CodePointLength(Name);
		if (NameLength == 0 || NameLength > FMantlePlaceLimits::MaxScriptNameLength)
		{
			Result.Issues.Add(FMantleValidationIssue{
				EMantleIssueSeverity::Error,
				FString(),
				FString::Printf(TEXT("Scripts/%s has a name %d characters long; Mantle publishes script names of 1 to %d."),
					*RelativePath, NameLength, FMantlePlaceLimits::MaxScriptNameLength),
				FString::Printf(TEXT("Rename the file so its name, without the extension, is 1 to %d characters."), FMantlePlaceLimits::MaxScriptNameLength) });
		}
	}

	/**
	 * Every require()/import path this scan can read without executing the source. Luau calls
	 * require("..."); the JS/TS module loader (MantleQuickJS.cpp) is a real ES module graph, so
	 * it is either `import ... from "..."` or a bare `import "...";`. This is intentionally a
	 * scan, not a parser -- good enough for "read statically" (#129); anything dynamically
	 * computed is, by construction, not statically readable.
	 */
	TArray<FString> CollectStaticRequires(const FString& Source, EMantleScriptCatalogLanguage Language)
	{
		TArray<FString> Out;

		if (Language == EMantleScriptCatalogLanguage::Luau || Language == EMantleScriptCatalogLanguage::Python)
		{
			static const FRegexPattern Pattern(TEXT("require\\s*\\(\\s*[\"']([^\"']*)[\"']\\s*\\)"));
			FRegexMatcher Matcher(Pattern, Source);
			while (Matcher.FindNext())
			{
				Out.Add(Matcher.GetCaptureGroup(1));
			}
		}
		else if (Language == EMantleScriptCatalogLanguage::JavaScript || Language == EMantleScriptCatalogLanguage::TypeScript)
		{
			static const FRegexPattern FromPattern(TEXT("\\bfrom\\s*[\"']([^\"']*)[\"']"));
			FRegexMatcher FromMatcher(FromPattern, Source);
			while (FromMatcher.FindNext())
			{
				Out.Add(FromMatcher.GetCaptureGroup(1));
			}

			static const FRegexPattern BarePattern(TEXT("\\bimport\\s*[\"']([^\"']*)[\"']"));
			FRegexMatcher BareMatcher(BarePattern, Source);
			while (BareMatcher.FindNext())
			{
				Out.Add(BareMatcher.GetCaptureGroup(1));
			}
		}

		return Out;
	}

	/**
	 * #549: UI/, beside Scripts/: every .rml and .rcss file Publish uploads, checked against the
	 * place format's rules (count, path, size, case-only duplicates) so Check refuses what Publish
	 * refuses. #556 item 5: the images there too (count, path, size, PNG or JPEG content), and
	 * image references in the documents that name no image there (a warning).
	 *
	 * #549 P6a: then each file's text, through the game's own scans (CheckUIDocumentContent, which
	 * Export applies too): what players' games refuse is an error, what only shows differently a warning.
	 */
	void CheckPlaceUIDocuments(UWorld* World, FMantleValidationResult& Result)
	{
		const FString UIFolder = FMantlePlaceExporter::GetUIFolder(World);
		if (UIFolder.IsEmpty())
		{
			return;
		}
		TArray<FMantleUIDocumentFile> Files;
		TArray<FMantleUIImageFile> Images;
		FMantlePlaceExporter::CollectUIDocumentFiles(UIFolder, Files, Images);
		for (const FMantleUIDocumentProblem& Problem : FMantlePlaceExporter::CheckUIDocumentFiles(Files))
		{
			Result.Issues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Error, FString(), Problem.Problem, Problem.Remedy });
		}
		for (const FMantleUIDocumentProblem& Problem : FMantlePlaceExporter::CheckUIImageFiles(Images, Files))
		{
			Result.Issues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Error, FString(), Problem.Problem, Problem.Remedy });
		}
		for (const FMantleUIDocumentProblem& Reference : FMantlePlaceExporter::CheckUIImageReferences(Files, Images))
		{
			Result.Issues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Warning, FString(), Reference.Problem, Reference.Remedy });
		}
		TArray<FMantleUIDocumentProblem> ContentWarnings;
		for (const FMantleUIDocumentProblem& Problem : FMantlePlaceExporter::CheckUIDocumentContent(Files, &ContentWarnings))
		{
			Result.Issues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Error, FString(), Problem.Problem, Problem.Remedy });
		}
		for (const FMantleUIDocumentProblem& Warning : ContentWarnings)
		{
			Result.Issues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Warning, FString(), Warning.Problem, Warning.Remedy });
		}
	}

	/**
	 * Scripts/, collected once per level rather than per actor -- unlike every other check in
	 * this file, a script definition is not tied to any specific entity.
	 */
	void CheckScripts(UWorld* World, const TArray<FMantleLocalScriptPackage>& Packages, const TArray<FString>& StoredPackages, FMantleValidationResult& Result)
	{
		const FString ScriptsFolder = FMantlePlaceExporter::GetScriptsFolder(World);
		if (ScriptsFolder.IsEmpty())
		{
			return;
		}

		TArray<FMantleScriptCatalogEntry> Entries;
		TArray<FMantleScriptCollectionIssue> Issues;
		FMantlePlaceExporter::CollectScriptFiles(ScriptsFolder, Entries, Issues);
		TArray<FMantleGraphScriptEntry> GraphEntries;
		TArray<FMantleParseError> GraphErrors;
		TArray<FString> GraphPaths;
		FMantlePlaceExporter::CollectGraphScripts(ScriptsFolder, GraphEntries, GraphErrors, GraphPaths);
		// A graph asset's .uasset and any generated .luau sibling are implementation details. The
		// freshly compiled graph definition is the only script the validator should reason about.
		FMantlePlaceExporter::FilterGraphGeneratedFiles(Entries, Issues, GraphPaths);

		for (const FMantleParseError& Error : GraphErrors)
		{
			FString DisplayPath = Error.Path;
			DisplayPath.RemoveFromStart(TEXT("/scripts/"));
			Result.Issues.Add(FMantleValidationIssue{
				EMantleIssueSeverity::Error,
				FString(),
				FString::Printf(TEXT("Graph script %s: %s"), *DisplayPath, *Error.Message),
				TEXT("Open the graph and fix the highlighted node before publishing.") });
		}

		for (const FMantleScriptCollectionIssue& Issue : Issues)
		{
			// #549: a UI file under Scripts/ is still refused, with where it belongs.
			const FString Extension = FPaths::GetExtension(Issue.RelativePath);
			const bool bUIFile = Extension.Equals(TEXT("rml"), ESearchCase::IgnoreCase) || Extension.Equals(TEXT("rcss"), ESearchCase::IgnoreCase);
			Result.Issues.Add(FMantleValidationIssue{
				EMantleIssueSeverity::Error,
				FString(),
				Issue.bLanguageUnknown
					? FString::Printf(TEXT("Scripts/%s has an extension Mantle cannot map to a script language."), *Issue.RelativePath)
					: FString::Printf(TEXT("Scripts/%s is not inside Server/, Client/ or Shared/ (case-exact)."), *Issue.RelativePath),
				Issue.bLanguageUnknown
					? (bUIFile
						? FString(TEXT("It is a UI file: move it to the UI/ folder beside Scripts/."))
						: FString(TEXT("Use .luau, .js, .ts or .py, or remove the file.")))
					: FString(TEXT("Rename the folder to match Server, Client or Shared exactly.")) });
		}

		// The backend's path and name rules, for each script Export writes (#393).
		for (const FMantleScriptCatalogEntry& Entry : Entries)
		{
			CheckScriptNaming(Entry.Path, Entry.Name, Result);
		}
		for (const FMantleGraphScriptEntry& Entry : GraphEntries)
		{
			CheckScriptNaming(Entry.Path, Entry.Name, Result);
		}

		// #394: a place records the lowest script API version its scripts use. A script the scan
		// can't follow makes Export record this editor's newest instead; say which one, and why.
		for (const FMantleScriptCatalogEntry& Entry : Entries)
		{
			const FMantleScriptApiNeed Need = FMantleScriptApiUsage::ForSource(Entry.Source, Entry.Language);
			if (!Need.bAnalysed)
			{
				FMantleValidationIssue& Note = Result.Issues.AddDefaulted_GetRef();
				Note.Severity = EMantleIssueSeverity::Warning;
				FMantleScriptApiUsage::DescribeUnanalysable(Entry.Path, Need, Note.Problem, Note.Remedy);
			}
		}

		// Resolve every require()/import this ticket can read statically (#129): a wrong-case
		// target is rejected naming the real spelling, and one that crosses sides (a client
		// script reaching into Server/, or vice versa) is rejected outright, because a
		// published place never sends a client the server's sources or the reverse.
		// #838: the level's script packages are mounted beside the place's scripts, as the runtime mounts them, so a place script's
		// require("@Name/Thing") finds a local package's Shared/Thing; one naming a stored package is accepted unseen.
		TArray<FMantleScriptCatalogEntry> ResolveEntries = Entries;
		for (const FMantleLocalScriptPackage& Package : Packages)
		{
			ResolveEntries.Append(Package.Entries);
		}
		TSet<FString> ReferencedPaths;
		for (const FMantleScriptCatalogEntry& Entry : Entries)
		{
			for (const FString& Request : CollectStaticRequires(Entry.Source, Entry.Language))
			{
				FString RequireError;
				const FMantleScriptCatalogEntry* Target = nullptr;
				if (FMantleScriptPackages::ResolveStaticRequire(ResolveEntries, Entry, Request, StoredPackages, Target, RequireError))
				{
					if (Target)
					{
						ReferencedPaths.Add(Target->Path);
					}
					continue;
				}
				Result.Issues.Add(FMantleValidationIssue{
					EMantleIssueSeverity::Error,
					FString(),
					FString::Printf(TEXT("Scripts/%s: %s"), *Entry.Path, *RequireError),
					FString() });
			}
		}

		// A Shared/ file nothing requires is very likely dead or half-wired, but it costs
		// nothing to publish, so this is a warning rather than an error.
		for (const FMantleScriptCatalogEntry& Entry : Entries)
		{
			if (Entry.Side == EMantleSide::Both && !ReferencedPaths.Contains(Entry.Path))
			{
				Result.Issues.Add(FMantleValidationIssue{
					EMantleIssueSeverity::Warning,
					FString(),
					FString::Printf(TEXT("Scripts/%s is a Shared module nothing requires."), *Entry.Path),
					TEXT("Delete it, or require it from a Server or Client script.") });
			}
		}
	}

	/** #838: the storage names a place script's require("@Name/...") may reach with no local package: entries that are, or may be, script packages. */
	TArray<FString> ValidatorStoredPackageNames(const AMantlePlaceSettingsActor* Settings)
	{
		TArray<FString> Names;
		if (Settings)
		{
			for (const FMantleStorageSetting& Entry : Settings->Storage)
			{
				if (Entry.Kind.IsEmpty() || Entry.Kind.Equals(FMantleStorage::KindScript, ESearchCase::CaseSensitive))
				{
					Names.Add(Entry.Name);
				}
			}
		}
		return Names;
	}

	/**
	 * #838: the level's script packages (Packages/, beside Scripts/) and its Script Points. A malformed package, a package whose name a
	 * storage entry of another kind holds, more storage than a place may have once the packages are stored, a point naming nothing it
	 * can run, a point past the cap, a parameter the package does not declare (a warning), and a require() inside a package that the
	 * runtime would refuse.
	 */
	void ValidatorCheckScriptPackages(UWorld* World, const TArray<FMantleLocalScriptPackage>& Packages, const TArray<FMantleScriptPackageProblem>& Problems,
		const TArray<const AMantleScriptPoint*>& Points, const TArray<FString>& StoredPackages, FMantleValidationResult& Result)
	{
		for (const FMantleScriptPackageProblem& Problem : Problems)
		{
			Result.Issues.Add(FMantleValidationIssue{
				Problem.bError ? EMantleIssueSeverity::Error : EMantleIssueSeverity::Warning, FString(), Problem.Problem, Problem.Remedy });
		}

		const AMantlePlaceSettingsActor* Settings = AMantlePlaceSettingsActor::Find(World);
		int32 NewEntries = 0;
		for (const FMantleLocalScriptPackage& Package : Packages)
		{
			const FMantleStorageSetting* Stored = Settings ? Settings->FindStorageEntry(Package.Name) : nullptr;
			if (!Stored)
			{
				++NewEntries;
			}
			else if (!Stored->Kind.IsEmpty() && !Stored->Kind.Equals(FMantleStorage::KindScript, ESearchCase::CaseSensitive))
			{
				Result.Issues.Add(FMantleValidationIssue{
					EMantleIssueSeverity::Error,
					FString(),
					FString::Printf(TEXT("Packages/%s has the same name as the %s '%s' in Mantle Place Settings > Storage."), *Package.Name, *Stored->Kind, *Stored->Name),
					TEXT("Rename the package folder or the storage entry: publishing stores the package under its folder's name.") });
			}
		}
		const int32 StoredCount = Settings ? Settings->Storage.Num() : 0;
		if (NewEntries > 0 && StoredCount + NewEntries > FMantlePlaceLimits::MaxStorageEntries)
		{
			Result.Issues.Add(FMantleValidationIssue{
				EMantleIssueSeverity::Error,
				FString(),
				FString::Printf(TEXT("This place would store %d library items once its %d new package(s) are stored; a place stores at most %d."),
					StoredCount + NewEntries, NewEntries, FMantlePlaceLimits::MaxStorageEntries),
				TEXT("Remove entries from Mantle Place Settings > Storage, or combine packages.") });
		}

		int32 Count = 0;
		for (const AMantleScriptPoint* Point : Points)
		{
			if (++Count == FMantlePlaceLimits::MaxScriptPoints + 1)
			{
				Result.Issues.Add(FMantleValidationIssue{
					EMantleIssueSeverity::Error,
					LabelOf(Point),
					FString::Printf(TEXT("is Script Point %d; a place has at most %d."), Count, FMantlePlaceLimits::MaxScriptPoints),
					TEXT("Remove some Script Points: a package's scripts can loop over fewer anchors.") });
			}
			FMantleScriptPointProps Props;
			FString Problem;
			if (!Point->ReadProps(Props, Problem))
			{
				Result.Issues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Error, LabelOf(Point), Problem, TEXT("Change it in the point's Details panel.") });
				continue;
			}
			const FString& Name = Props.StorageItem;
			const FMantleLocalScriptPackage* Local = Packages.FindByPredicate([&Name](const FMantleLocalScriptPackage& Package)
			{
				return Package.Name.Equals(Name, ESearchCase::CaseSensitive);
			});
			const FMantleStorageSetting* Stored = Settings ? Settings->FindStorageEntry(Name) : nullptr;
			if (!Local && !Stored)
			{
				Result.Issues.Add(FMantleValidationIssue{
					EMantleIssueSeverity::Error,
					LabelOf(Point),
					FString::Printf(TEXT("names the script package '%s', which neither Mantle Place Settings > Storage nor the level's Packages/ folder has."), *Name),
					TEXT("Add the package with Tools > Mantle > Library (Add to storage), or pick one in the point's Storage Item.") });
				continue;
			}
			if (!Local && !Stored->Kind.IsEmpty() && !Stored->Kind.Equals(FMantleStorage::KindScript, ESearchCase::CaseSensitive))
			{
				Result.Issues.Add(FMantleValidationIssue{
					EMantleIssueSeverity::Error,
					LabelOf(Point),
					FString::Printf(TEXT("names storage '%s', which is a %s, not a script package."), *Name, *Stored->Kind),
					TEXT("Pick a script package in the point's Storage Item.") });
				continue;
			}
			if (!Local && Stored->Kind.IsEmpty())
			{
				Result.Issues.Add(FMantleValidationIssue{
					EMantleIssueSeverity::Warning,
					LabelOf(Point),
					FString::Printf(TEXT("names storage '%s', which was typed in by hand, so this editor does not know it is a script package. Mantle checks that when you publish."), *Name),
					TEXT("Add the package again from Tools > Mantle > Library to record what it is, or leave it.") });
			}
			// #838 stage 6: the point's parameters against the package's declarations (an unknown one, a value that does not fit its
			// type, range or options, a required one left out), when they are known: the export refuses the same.
			FMantleScriptPackageManifest Manifest;
			if (FMantleScriptPackages::KnownManifest(Name, Packages, Stored ? Stored->Manifest : FString(), Manifest))
			{
				TMap<FString, FString> Values;
				for (const FMantleScriptPointParameter& Parameter : Props.Parameters)
				{
					Values.Add(Parameter.Name, Parameter.Value);
				}
				for (const FMantleScriptOptionIssue& Issue : FMantleScriptPackages::CheckOptionValues(Name, Manifest.Parameters, Values, TEXT("parameter")))
				{
					Result.Issues.Add(FMantleValidationIssue{
						EMantleIssueSeverity::Error,
						LabelOf(Point),
						Issue.Message,
						Issue.Problem == EMantleScriptOptionProblem::Unknown
							? FString(TEXT("Check the spelling against the parameters the point's Details panel lists, or remove it."))
							: FString(TEXT("Set it in the point's Details panel.")) });
				}
			}
		}

		// #838 stage 6: each storage entry's settings against its package's declarations, as the export and the backend check them.
		if (Settings)
		{
			for (const FMantleStorageSetting& Entry : Settings->Storage)
			{
				const FString& EntryName = Entry.Name;
				const bool bLocal = Packages.ContainsByPredicate([&EntryName](const FMantleLocalScriptPackage& Package)
				{
					return Package.Name.Equals(EntryName, ESearchCase::CaseSensitive);
				});
				if (!bLocal && !Entry.Kind.IsEmpty() && !Entry.Kind.Equals(FMantleStorage::KindScript, ESearchCase::CaseSensitive))
				{
					if (Entry.Settings.Num() > 0)
					{
						Result.Issues.Add(FMantleValidationIssue{
							EMantleIssueSeverity::Error,
							LabelOf(Settings),
							FString::Printf(TEXT("%s is a %s, and only a script package takes settings."), *Entry.Name, *Entry.Kind),
							TEXT("Remove its settings in Mantle Place Settings > Storage.") });
					}
					continue;
				}
				FMantleScriptPackageManifest Manifest;
				if (!FMantleScriptPackages::KnownManifest(Entry.Name, Packages, Entry.Manifest, Manifest))
				{
					continue;
				}
				for (const FMantleScriptOptionIssue& Issue : FMantleScriptPackages::CheckOptionValues(Entry.Name, Manifest.Settings, Entry.Settings, TEXT("setting")))
				{
					Result.Issues.Add(FMantleValidationIssue{
						EMantleIssueSeverity::Error,
						LabelOf(Settings),
						Issue.Message,
						Issue.Problem == EMantleScriptOptionProblem::Unknown
							? FString::Printf(TEXT("Remove it from %s's settings in Mantle Place Settings > Storage."), *Entry.Name)
							: FString::Printf(TEXT("Set it on %s's row in Mantle Place Settings > Storage."), *Entry.Name) });
				}
			}
		}
		// A package folder not stored yet is stored with no settings on its first publish.
		for (const FMantleLocalScriptPackage& Package : Packages)
		{
			if (!Package.bHasManifest || (Settings && Settings->FindStorageEntry(Package.Name)))
			{
				continue;
			}
			for (const FMantleScriptOptionIssue& Issue : FMantleScriptPackages::CheckOptionValues(Package.Name, Package.Manifest.Settings, TMap<FString, FString>(), TEXT("setting")))
			{
				Result.Issues.Add(FMantleValidationIssue{
					EMantleIssueSeverity::Error,
					FString(),
					Issue.Message,
					FString::Printf(TEXT("Packages/%s is stored with no settings when the place first publishes it: give that setting a default in its package.json."), *Package.Name) });
			}
		}

		// require() inside each package, by the runtime's rules (a package reaches only its own files).
		TArray<FMantleScriptCatalogEntry> Entries;
		{
			TArray<FMantleScriptCollectionIssue> Ignored;
			FMantlePlaceExporter::CollectScriptFiles(FMantlePlaceExporter::GetScriptsFolder(World), Entries, Ignored);
		}
		for (const FMantleLocalScriptPackage& Package : Packages)
		{
			Entries.Append(Package.Entries);
		}
		for (const FMantleLocalScriptPackage& Package : Packages)
		{
			const FString OwnAlias = FString::Printf(TEXT("@%s/"), *Package.Name);
			for (const FMantleScriptCatalogEntry& Entry : Package.Entries)
			{
				for (const FString& Request : CollectStaticRequires(Entry.Source, Entry.Language))
				{
					if (Request.StartsWith(OwnAlias, ESearchCase::CaseSensitive))
					{
						Result.Issues.Add(FMantleValidationIssue{
							EMantleIssueSeverity::Warning,
							FString(),
							FString::Printf(TEXT("%s requires \"%s\": that works only in a place that stores the package under the name %s."), *Entry.Path, *Request, *Package.Name),
							FString::Printf(TEXT("Use a bare name for the package's own modules, like require(\"%s\")."), *Request.RightChop(OwnAlias.Len())) });
					}
					const FMantleScriptCatalogEntry* Target = nullptr;
					FString RequireError;
					if (!FMantleScriptPackages::ResolveStaticRequire(Entries, Entry, Request, StoredPackages, Target, RequireError))
					{
						Result.Issues.Add(FMantleValidationIssue{
							EMantleIssueSeverity::Error,
							FString(),
							FString::Printf(TEXT("%s: %s"), *Entry.Path, *RequireError),
							FString() });
					}
				}
			}
		}
	}
}

void FMantlePlaceValidator::CheckRevisionNotes(const FString& Notes, FMantleValidationResult& Result)
{
	const int32 Length = FMantlePlaceParser::CodePointLength(Notes);
	if (Length > FMantlePlaceLimits::MaxRevisionNotesLength)
	{
		Result.Issues.Add(FMantleValidationIssue{
			EMantleIssueSeverity::Error,
			FString(),
			FString::Printf(TEXT("The revision notes are %d characters long; Mantle keeps notes of at most %d."),
				Length, FMantlePlaceLimits::MaxRevisionNotesLength),
			FString::Printf(TEXT("Shorten the notes to %d characters or fewer and publish again."), FMantlePlaceLimits::MaxRevisionNotesLength) });
	}
}

void FMantlePlaceValidator::CheckPlaceName(const FString& Name, FMantleValidationResult& Result)
{
	const int32 Length = FMantlePlaceParser::CodePointLength(Name);
	if (Length > FMantlePlaceLimits::MaxPlaceNameLength)
	{
		Result.Issues.Add(FMantleValidationIssue{
			EMantleIssueSeverity::Error,
			FString(),
			FString::Printf(TEXT("The level's name is %d characters long; Mantle names a place after its level, and place names are at most %d."),
				Length, FMantlePlaceLimits::MaxPlaceNameLength),
			FString::Printf(TEXT("Rename the level to %d characters or fewer."), FMantlePlaceLimits::MaxPlaceNameLength) });
	}
}

FMantleValidationResult FMantlePlaceValidator::Validate(UWorld* World, const bool bThinInstances)
{
	FMantleValidationResult Result;

	if (!World)
	{
		Result.Issues.Add(FMantleValidationIssue{
			EMantleIssueSeverity::Error,
			FString(),
			TEXT("There is no level to publish."),
			TEXT("Open a level first.") });
		return Result;
	}

	// #450: the loop below only sees loaded actors; a World Partition level with unloaded cells is refused.
	FMantlePlaceWorldPartition::CheckLevel(World, Result.Issues);

	int32 PlayerStarts = 0;
	int32 Publishable = 0;
	int32 PhysicsObjects = 0;
	FMantleCrowdBudget CrowdBudget;
	int32 VehicleSpawns = 0;
	int32 WeaponSpawns = 0;
	int32 Effects = 0;
	int64 ScatterInstances = 0;
	int32 Constraints = 0;
	int32 ReflectionProbes = 0;
	int32 Texts = 0;
	bool bWarnedSun = false;
	// #838: checked after the loop, once the level's packages and storage are known.
	TArray<const AMantleScriptPoint*> ScriptPoints;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		const AActor* Actor = *It;
		if (!IsValid(Actor))
		{
			continue;
		}

		if (FMantlePlaceExporter::IsProvidedByMantle(Actor))
		{
			// Named explicitly rather than falling silently into the IsEngineInfrastructure
			// skip below — a level built from UE's Default template carries these, and a
			// creator running "Check this place" should see why their settings on them will
			// not be published, not nothing at all.
			Result.Issues.Add(FMantleValidationIssue{
				EMantleIssueSeverity::Warning,
				LabelOf(Actor),
				FMantleSkyMesh::IsSkyMeshActor(Actor) ? FMantleSkyMesh::StandaloneDetail()
					: FString(TEXT("Mantle provides the sky; this actor's settings are not published.")),
				FString() });
			continue;
		}

		// Parts that can't publish as they are (#486): PCG left ungenerated or set to generate at
		// runtime, PCG-spawned actors Mantle can't express, unsaved dynamic meshes.
		// Errors, with the fix, for every actor, infrastructure included (a PCG volume is a brush).
		TArray<FMantleUnpublishedPart> Parts;
		FMantlePlaceExporter::DescribeUnpublishedParts(Actor, Parts);
		for (const FMantleUnpublishedPart& Part : Parts)
		{
			Result.Issues.Add(FMantleValidationIssue{
				Part.bBlocksPublish ? EMantleIssueSeverity::Error : EMantleIssueSeverity::Warning,
				LabelOf(Actor),
				Part.What,
				Part.Remedy });
		}

		// Placed on purpose but not publishable: named with a remedy rather than falling into
		// the silent infrastructure skip below (#264). A warning, not an error: the rest of the
		// place still publishes, without these.
		FString UnpublishedWhat;
		FString UnpublishedRemedy;
		if (FMantlePlaceExporter::DescribeUnpublishedContent(Actor, UnpublishedWhat, UnpublishedRemedy))
		{
			Result.Issues.Add(FMantleValidationIssue{
				EMantleIssueSeverity::Warning,
				LabelOf(Actor),
				UnpublishedWhat,
				UnpublishedRemedy });
			continue;
		}

		if (FMantlePlaceExporter::IsEngineInfrastructure(Actor))
		{
			continue;
		}

		EMantleEntityType Type;
		if (!FMantlePlaceExporter::ClassifyActor(Actor, Type))
		{
			// Only instanced meshes, which publish on their own (#467), checked after this loop; or
			// already named above with its specific reason (#486).
			if (FMantlePlaceExporter::IsInstanceHost(Actor) || Parts.Num() > 0)
			{
				continue;
			}
			// (A Post Process Volume never gets here: DescribeUnpublishedContent warns about it above, #628.)
			Result.Issues.Add(FMantleValidationIssue{
				EMantleIssueSeverity::Error,
				LabelOf(Actor),
				FString::Printf(
					TEXT("is a %s, which Mantle cannot publish."),
					*Actor->GetClass()->GetName()),
				FString::Printf(TEXT("Remove it or replace it. %s"), SupportedTypeList()) });
			continue;
		}

		++Publishable;
		PlayerStarts += (Type == EMantleEntityType::PlayerStart) ? 1 : 0;

		// #445: the place's first directional light (in the exporter's actor order) becomes the
		// sun of the day/night cycle, which sets its angle. Other directional lights stay put.
		if (Type == EMantleEntityType::DirectionalLight && !bWarnedSun)
		{
			bWarnedSun = true;
			// The exporter keeps the pitch as a stopped clock only when (FMantleEnvironmentExport MapSun) the light is not attached to
			// another exported actor, the settings set no Clock Time or Time Scale, the sun is at least 1 degree up
			// (FMantleEnvironmentExport::SolveSunClock) and the target client reads the environment settings.
			const AMantlePlaceSettingsActor* SunSettings = AMantlePlaceSettingsActor::Find(World);
			const FMantleEnvironmentSettings SunEnvironment = SunSettings ? SunSettings->MakeEnvironmentSettings() : FMantleEnvironmentSettings();
			FMantleSunClock SunClock;
			const bool bClockSet = SunEnvironment.ClockTime.IsSet() || SunEnvironment.TimeScale.IsSet();
			// The exporter only counts a parent it exports itself; this approximates that with ClassifyActor and IsEngineInfrastructure.
			const AActor* SunParent = Actor->GetAttachParentActor();
			EMantleEntityType ParentType;
			const bool bAttached = SunParent && !FMantlePlaceExporter::IsEngineInfrastructure(SunParent)
				&& FMantlePlaceExporter::ClassifyActor(SunParent, ParentType);
			const bool bSolved = FMantleEnvironmentExport::SolveSunClock(-Actor->GetActorForwardVector(), Actor->GetActorRotation().Yaw, SunClock);
			const bool bClientReads = FMantleTargetFeatures::Current().Reads(MantleClientFeatures::EnvironmentDefaults);
			const bool bPitchKept = !bClockSet && !bAttached && bSolved && bClientReads;
			if (!bPitchKept)
			{
				Result.Issues.Add(FMantleValidationIssue{
					EMantleIssueSeverity::Warning,
					LabelOf(Actor),
					bClockSet ? FString(TEXT("Mantle Place Settings sets the clock, so this light's pitch is not kept."))
						: bAttached ? TEXT("This light is attached to another actor, so its pitch is not kept.")
						: !bSolved ? TEXT("This sun is at or below the horizon, so its pitch is not kept.")
						: FString::Printf(TEXT("This light's pitch is not kept: %s."), FMantleTargetFeatures::NeedsNewerClient),
					TEXT("Its intensity, colour and heading (yaw) are kept; Mantle's sky clock sets its angle. "
						"To keep the pitch, leave Clock Time and Time Scale unset, keep the light unattached and at least 1 degree above the horizon, "
						"and target a Mantle client that reads environment settings.") });
			}
		}

		CheckComponents(Actor, Type, Result);
		CheckAssetsAndTextures(Actor, Type, Result);
		CheckSound(Actor, Type, Result);
		ValidatorCheckSkeletalMesh(Actor, Type, Result);
		CheckDecal(Actor, Type, Result);
		CheckCrowdSpawner(Actor, Type, Result);
		CheckVehicleSpawn(Actor, Type, VehicleSpawns, Result);
		CheckWeaponSpawn(Actor, Type, WeaponSpawns, Result);
		if (const AMantleScriptPoint* ScriptPoint = Type == EMantleEntityType::ScriptPoint ? Cast<AMantleScriptPoint>(Actor) : nullptr)
		{
			ScriptPoints.Add(ScriptPoint);
		}
		CheckEffect(Actor, Type, Effects, Result);
		ValidatorCheckScatter(Actor, Type, ScatterInstances, Result);
		CheckConstraint(Actor, Type, Constraints, Result);
		FMantleReflectionProbeExport::Check(Actor, Type, ReflectionProbes, Result);
		FMantleTextExport::Check(Actor, Type, Texts, Result);
		if (const AMantleCrowdSpawner* Spawner = Type == EMantleEntityType::CrowdSpawner ? Cast<AMantleCrowdSpawner>(Actor) : nullptr)
		{
			// The per-place totals, counted in the exporter's order so the same spawner is named.
			FString SpawnerProblem;
			FString NpcProblem;
			CrowdBudget.Add(Spawner->Count, SpawnerProblem, NpcProblem);
			for (const FString* Problem : { &SpawnerProblem, &NpcProblem })
			{
				if (!Problem->IsEmpty())
				{
					Result.Issues.Add(FMantleValidationIssue{
						EMantleIssueSeverity::Error,
						LabelOf(Actor),
						*Problem,
						FString() });
				}
			}
		}
		CheckPublishedSizeLimits(Actor, Type, Result);
		CheckPublishedRanges(Actor, Type, Result);
		CheckDroppedBehaviourForCheck(Actor, Result);
		PhysicsObjects += CheckPhysicsForCheck(Actor, Type, Result) ? 1 : 0;
	}

	// GhzGarage/MantleCore#142: Mantle Seats, on any prop (an unpublished prop included, which the loop above may have skipped).
	FMantleSeatExport::Check(World, Result);

	// #456: every physics entity is simulated on the server and replicated to every player. Past
	// the hard cap the place is refused (the parser and backend refuse it too); past the soft one,
	// the creator hears about the cost before players feel it.
	if (PhysicsObjects > FMantlePlaceLimits::MaxPhysicsObjectsPerPlace)
	{
		Result.Issues.Add(FMantleValidationIssue{
			EMantleIssueSeverity::Error,
			FString(),
			FString::Printf(TEXT("This level has %d actors that simulate physics; a place holds at most %d."),
				PhysicsObjects, FMantlePlaceLimits::MaxPhysicsObjectsPerPlace),
			TEXT("Turn Simulate Physics off on the ones that don't need to move.") });
	}
	else if (PhysicsObjects > FMantlePlaceLimits::WarnPhysicsObjectsPerPlace)
	{
		Result.Issues.Add(FMantleValidationIssue{
			EMantleIssueSeverity::Warning,
			FString(),
			FString::Printf(TEXT("This level has %d actors that simulate physics. Past %d, the server's physics and every player's network traffic grow noticeably."),
				PhysicsObjects, FMantlePlaceLimits::WarnPhysicsObjectsPerPlace),
			TEXT("Keep Simulate Physics for the objects players actually push around.") });
	}

	// #528: every joint is simulated on the server. Past the hard cap CheckConstraint refuses the place;
	// past the soft one, the creator hears about it first.
	if (Constraints > FMantlePlaceLimits::WarnConstraintsPerPlace && Constraints <= FMantlePlaceLimits::MaxConstraintsPerPlace)
	{
		Result.Issues.Add(FMantleValidationIssue{
			EMantleIssueSeverity::Warning,
			FString(),
			FString::Printf(TEXT("This level has %d physics constraints. Past %d, the server's physics grows noticeably; a place holds at most %d."),
				Constraints, FMantlePlaceLimits::WarnConstraintsPerPlace, FMantlePlaceLimits::MaxConstraintsPerPlace),
			TEXT("Keep constraints for the objects players actually move: weld a heap of parts into fewer objects instead.") });
	}

	CheckLevelBlueprintForCheck(World, Result);

	// Instanced meshes (#467): every instance source Export writes, wherever it sits (foliage, PCG
	// output, instanced components on any actor), through the exporter's own checks.
	{
		int64 InstanceTotal = 0;
		for (const UInstancedStaticMeshComponent* Component : FMantlePlaceExporter::CollectInstanceSources(World))
		{
			++Publishable;
			InstanceTotal += Component->GetInstanceCount();
			TArray<FMantleUnpublishedPart> Parts;
			FMantlePlaceExporter::CheckInstanceSource(Component, Parts);
			for (const FMantleUnpublishedPart& Part : Parts)
			{
				Result.Issues.Add(FMantleValidationIssue{
					Part.bBlocksPublish ? EMantleIssueSeverity::Error : EMantleIssueSeverity::Warning,
					LabelOf(Component->GetOwner()),
					Part.What,
					Part.Remedy });
			}
		}
		if (InstanceTotal > FMantlePlaceLimits::MaxInstancesPerPlace)
		{
			// GhzGarage/mantle#818: Publish offers even thinning, with its numbers; when it was chosen this is only a note, and Export
			// removes the instances and reports how many.
			Result.Issues.Add(FMantleValidationIssue{
				bThinInstances ? EMantleIssueSeverity::Warning : EMantleIssueSeverity::Error,
				FString(),
				FString::Printf(TEXT("This level has %lld instanced meshes (foliage, PCG output, instanced components); a place holds at most %d."),
					InstanceTotal, FMantlePlaceLimits::MaxInstancesPerPlace),
				bThinInstances ? TEXT("They are thinned evenly when the place is exported, foliage first; the coverage report says how many.")
					: TEXT("Publish can thin them evenly and shows how many go before you choose it (Mantle.Publish -Thin=auto does it without the window), or split the world into more than one place."),
				true });
		}
	}

	// Spline meshes (#628), which Export bakes into static meshes: whatever can't be baked is an error with
	// its fix. The baked meshes themselves are held to the mesh limits with the level's other uploads, below.
	{
		const FMantleSplineBakeResult Bake = FMantleSplineMeshBake::Bake(World, FMantlePlaceAssetLimits::FromContract());
		Publishable += Bake.Meshes.Num();
		for (const FMantleSplineBakeProblem& Problem : Bake.Problems)
		{
			Result.Issues.Add(FMantleValidationIssue{
				EMantleIssueSeverity::Error,
				Problem.OwnerLabel,
				Problem.What,
				Problem.Remedy });
		}
	}

	// The landscape (#466), which Export writes as a Terrain, held to the same rules: one per place,
	// no holes, its painted layers given materials in Mantle Place Settings.
	{
		const TArray<AActor*> Landscapes = FMantleTerrainExport::CollectLandscapes(World);
		Publishable += FMath::Min(Landscapes.Num(), FMantlePlaceLimits::MaxTerrainsPerPlace);
		FMantleTerrainExport::CheckLevel(World, Result.Issues);
	}

	// Publish names the place after the level (FMantleLevelPublishSource), and the API refuses a
	// name past the schema's limit when it creates the place (#393).
	CheckPlaceName(World->GetName(), Result);

	// Meshes and materials from the creator's project (a Fab pack): limits as errors, and every
	// loss flattening a material brings as a warning, each asset once however many actors use it.
	FMantlePlaceAssets::CheckLevel(World, Result.Issues);

	// The Script Assets (Mantle Place Settings): CheckLevel above gathered them with the level's own
	// uploads, so their mesh limits, material losses and share of the sound budget are checked there.
	// What else can refuse one, Export refuses with the same words.
	if (const AMantlePlaceSettingsActor* PlaceSettings = AMantlePlaceSettingsActor::Find(World))
	{
		const FMantlePlaceAssetLimits Limits = FMantlePlaceAssetLimits::FromContract();
		for (const FMantleScriptAsset& Asset : FMantlePlaceAssets::ScriptAssets(World))
		{
			FString Problem;
			FString Remedy;
			if (!FMantlePlaceAssets::CheckScriptAsset(Asset, Limits, Problem, Remedy))
			{
				Result.Issues.Add(FMantleValidationIssue{
					EMantleIssueSeverity::Error,
					LabelOf(PlaceSettings),
					FString::Printf(TEXT("has Script asset '%s', which %s."), *Asset.Path.ToString(), *Problem),
					Remedy });
			}
		}
	}

	// #838: the level's script packages, read once for the place scripts' requires and their own checks.
	TArray<FMantleLocalScriptPackage> LocalPackages;
	TArray<FMantleScriptPackageProblem> PackageProblems;
	FMantleScriptPackages::Collect(FMantleScriptPackages::GetPackagesFolder(World), LocalPackages, PackageProblems);
	const TArray<FString> StoredPackages = ValidatorStoredPackageNames(AMantlePlaceSettingsActor::Find(World));
	CheckScripts(World, LocalPackages, StoredPackages, Result);
	ValidatorCheckScriptPackages(World, LocalPackages, PackageProblems, ScriptPoints, StoredPackages, Result);
	CheckPlaceUIDocuments(World, Result);

	// #614 stage 4: the storage entries in Mantle Place Settings, by the schema's rules (the parser's and the
	// exporter's): at most 32, unique letter-first names, itm_u item ids, versions from 1.
	if (const AMantlePlaceSettingsActor* PlaceSettings = AMantlePlaceSettingsActor::Find(World))
	{
		TArray<FMantleStorageEntry> StorageEntries;
		AMantlePlaceSettingsActor::ApplyStorageTo(World, StorageEntries);
		for (const FMantleStorageProblem& Problem : FMantleStorage::CheckEntries(StorageEntries))
		{
			Result.Issues.Add(FMantleValidationIssue{
				EMantleIssueSeverity::Error,
				LabelOf(PlaceSettings),
				FString::Printf(TEXT("has a storage problem at %s: %s."), *Problem.Path, *Problem.Message),
				TEXT("Fix it under Storage in Mantle Place Settings.") });
		}
	}

	// #320: restricting a place to some platforms but selecting none. The exporter refuses it
	// too; here it is named with the remedy before a publish is attempted.
	if (const AMantlePlaceSettingsActor* PlaceSettings = AMantlePlaceSettingsActor::Find(World);
		PlaceSettings && PlaceSettings->HasNoPlatformSelected())
	{
		Result.Issues.Add(FMantleValidationIssue{
			EMantleIssueSeverity::Error,
			LabelOf(PlaceSettings),
			TEXT("limits the place to some platforms but selects none."),
			TEXT("Select at least one platform, or turn All Platforms back on.") });
	}

	if (Publishable == 0)
	{
		Result.Issues.Add(FMantleValidationIssue{
			EMantleIssueSeverity::Error,
			FString(),
			TEXT("This level is empty."),
			TEXT("Add something before publishing.") });
	}
	else if (PlayerStarts == 0)
	{
		// Not fatal, but almost never intended. Every place is built into /Engine/Maps/Entry,
		// whose own PlayerStart sits about 2 m above the origin. With none in the place,
		// AMantleGameMode::PlaceStarts falls back to that one, for joining and respawning
		// alike (#265).
		Result.Issues.Add(FMantleValidationIssue{
			EMantleIssueSeverity::Warning,
			FString(),
			TEXT("This place has no player start, so everyone will arrive, and respawn, at Mantle's default spawn point about 2 m above the world origin."),
			TEXT("Add a Player Start where you want people to arrive.") });
	}

	UE_LOG(LogMantle, Log,
		TEXT("Validated level: %d publishable actor(s), %d issue(s), %d error(s)"),
		Publishable,
		Result.Issues.Num(),
		Result.NumErrors());

	return Result;
}
