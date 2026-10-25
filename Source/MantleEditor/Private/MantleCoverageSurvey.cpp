// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCoverageSurvey.h"
#include "MantleActorDecompose.h"
#include "MantleAssetLibrary.h"
#include "MantleClientFeatures.h"
#include "MantleCoverage.h"
#include "MantleEnvironment.h"
#include "MantleEnvironmentExport.h"
#include "MantleHeightfieldReader.h"
#include "MantlePlaceAssets.h"
#include "MantlePlaceExporter.h"
#include "MantlePlaceLimits.h"
#include "MantleSkyMesh.h"
#include "MantleTargetFeatures.h"
#include "MantleTerrainExport.h"

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
#include "Engine/ExponentialHeightFog.h"
#include "Engine/Level.h"
#include "Engine/LevelScriptBlueprint.h"
#include "Engine/LevelStreaming.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/DefaultPhysicsVolume.h"
#include "GameFramework/Info.h"
#include "GameFramework/Volume.h"
#include "LandscapeGrassType.h"
#include "LandscapeInfo.h"
#include "LandscapeProxy.h"
#include "MaterialCachedData.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleCoverageSurveyPrivate
{
	namespace Cat = MantleCoverageCategory;
	using EOutcome = EMantleCoverageOutcome;

	FString CoverageLabelOf(const AActor* Actor)
	{
#if WITH_EDITOR
		return Actor->GetActorLabel();
#else
		return Actor->GetName();
#endif
	}

	/** Whether Class is, or derives from, a class of this short name (matching by name keeps MantleEditor free of the module that declares it). */
	bool CoverageIsOrDerives(const UClass* Class, const TCHAR* Name)
	{
		for (; Class; Class = Class->GetSuperClass())
		{
			if (Class->GetFName() == Name)
			{
				return true;
			}
		}
		return false;
	}

	/** A Niagara or Cascade particle component, by class name: MantleEditor depends on neither plugin. */
	bool CoverageIsParticleComponent(const UActorComponent* Component)
	{
		const UClass* Class = Component ? Component->GetClass() : nullptr;
		return CoverageIsOrDerives(Class, TEXT("NiagaraComponent")) || CoverageIsOrDerives(Class, TEXT("ParticleSystemComponent"));
	}

	int32 CoverageCountComponents(const AActor* Actor, bool (*Predicate)(const UActorComponent*))
	{
		TArray<UActorComponent*> Components;
		Actor->GetComponents(Components);
		int32 Count = 0;
		for (const UActorComponent* Component : Components)
		{
			Count += Component && !Component->IsEditorOnly() && !Component->HasAnyFlags(RF_Transient) && Predicate(Component) ? 1 : 0;
		}
		return Count;
	}

	/** Geometry or light: what a place loses when an actor has more of it than its entity type can express (the validator's definition). */
	bool CoverageIsContent(const UActorComponent* Component)
	{
		if (FMantleActorDecompose::IsVisualisation(Component) || Component->IsA(USplineMeshComponent::StaticClass())
			|| CoverageIsOrDerives(Component->GetClass(), TEXT("DynamicMeshComponent")))
		{
			return false;
		}
		return Component->IsA(UPrimitiveComponent::StaticClass()) || Component->IsA(ULightComponentBase::StaticClass());
	}

	/** How many content components an entity type can represent (the validator's ExpectedContentComponents). */
	int32 CoverageExpectedContent(EMantleEntityType Type)
	{
		switch (Type)
		{
		case EMantleEntityType::Group:
		case EMantleEntityType::SoundEmitter:
			return 0;
		default:
			return 1;
		}
	}

	/** The Mantle actors whose own components are theirs (lanes, effect and scatter components...): the validator special-cases each, so the survey leaves them alone. */
	bool CoverageIsMantleSpecial(EMantleEntityType Type)
	{
		switch (Type)
		{
		case EMantleEntityType::CrowdSpawner:
		case EMantleEntityType::VehicleSpawn:
		case EMantleEntityType::WeaponSpawn:
		case EMantleEntityType::ScriptPoint:
		case EMantleEntityType::Effect:
		case EMantleEntityType::Constraint:
		case EMantleEntityType::Scatter:
		case EMantleEntityType::ReflectionProbe: // a capture's preview boxes are the editor's; Export reports the probe itself
		case EMantleEntityType::WaterBody: // a water body's helper meshes are the plugin's; Export reports the body itself
		case EMantleEntityType::Text: // a text actor's sprite is the editor's; Export reports the text itself
			return true;
		default:
			return false;
		}
	}

	/** The category of a component Export drops. */
	const TCHAR* CoverageComponentCategory(const UActorComponent* Component)
	{
		if (CoverageIsParticleComponent(Component))
		{
			return Cat::Particles;
		}
		if (Component->IsA(UDecalComponent::StaticClass()))
		{
			return Cat::Decals;
		}
		if (Component->IsA(UAudioComponent::StaticClass()))
		{
			return Cat::Sounds;
		}
		if (Component->IsA(ULightComponentBase::StaticClass()))
		{
			return Cat::Lights;
		}
		if (Component->IsA(UStaticMeshComponent::StaticClass()))
		{
			return Cat::StaticMeshes;
		}
		return Cat::OtherComponents;
	}

	/** What an actor's components lose when Export writes it as Type: the extra content, the audio, camera or movement it has no entity for. */
	void CoverageSurveyComponents(const AActor* Actor, EMantleEntityType Type, const FString& Label, FMantleCoverage& Coverage)
	{
		if (CoverageIsMantleSpecial(Type))
		{
			return;
		}
		// A Blueprint prefab's parts publish as the Group's children (#468); what it could not publish is named with its category.
		if (Type == EMantleEntityType::Group)
		{
			TArray<FMantlePrefabPart> Parts;
			TArray<FMantleUnpublishedPart> Unsupported;
			FMantlePlaceExporter::CollectPrefabParts(Actor, Parts, &Unsupported);
			if (Parts.Num() > 0 || Unsupported.Num() > 0)
			{
				for (const FMantleUnpublishedPart& Part : Unsupported)
				{
					// A particle component's own losses were reported while Export translated it (FMantleEffectExport::Translate, #628).
					if (Part.Category == Cat::Particles)
					{
						continue;
					}
					Coverage.Add(Part.Category.IsEmpty() ? FString(Cat::OtherComponents) : Part.Category, EOutcome::Dropped, Label, Part.What);
				}
				return;
			}
		}

		const USceneComponent* Root = Actor->GetRootComponent();
		TArray<UActorComponent*> Components;
		Actor->GetComponents(Components);
		const int32 Allowed = CoverageExpectedContent(Type);
		int32 Seen = 0;
		for (const UActorComponent* Component : Components)
		{
			if (!Component || Component->HasAnyFlags(RF_Transient) || FMantleActorDecompose::IsVisualisation(Component))
			{
				continue;
			}
			// Spline and dynamic meshes are reported on their own (the spline mesh bake, DescribeUnpublishedParts).
			if (Component->IsA(USplineMeshComponent::StaticClass()) || CoverageIsOrDerives(Component->GetClass(), TEXT("DynamicMeshComponent")))
			{
				continue;
			}
			// Instances publish as InstancedMesh entities (#467); an empty source shows and blocks nothing.
			if (const UInstancedStaticMeshComponent* Instanced = Cast<UInstancedStaticMeshComponent>(Component))
			{
				if (Instanced->GetInstanceCount() == 0 || FMantlePlaceExporter::IsExportedInstanceSource(Instanced))
				{
					continue;
				}
			}
			if (CoverageIsContent(Component))
			{
				if (++Seen <= Allowed)
				{
					continue;
				}
			}
			else if (Component == Root || Component->GetClass() == USceneComponent::StaticClass())
			{
				continue;
			}
			Coverage.Add(CoverageComponentCategory(Component), EOutcome::Dropped,
				FString::Printf(TEXT("%s (%s)"), *Label, *Component->GetName()),
				FString::Printf(TEXT("a %s that Mantle does not publish."), *Component->GetClass()->GetName()));
		}
	}

	/** Every actor of the level, by what Export does with it (the validator's order, so the two never describe an actor two ways). */
	void CoverageSurveyActors(UWorld* World, FMantleExportResult& Result)
	{
		FMantleCoverage& Coverage = Result.Coverage;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			const AActor* Actor = *It;
			if (!IsValid(Actor) || Actor->HasAnyFlags(RF_Transient) || Actor->IsA(ADefaultPhysicsVolume::StaticClass()))
			{
				continue;
			}
			const FString Label = CoverageLabelOf(Actor);

			// The sky, fog, cloud and post-process actors: Export reports each as it maps it onto the place (#628).
			if (FMantleEnvironmentExport::IsReported(Actor))
			{
				continue;
			}

			// The four sky actors Mantle replaces with its own.
			if (FMantlePlaceExporter::IsProvidedByMantle(Actor))
			{
				const bool bFog = Actor->IsA(AExponentialHeightFog::StaticClass());
				// #63: a sky dome mesh placed on its own is left out whole.
				if (FMantleSkyMesh::IsSkyMeshActor(Actor))
				{
					Coverage.Add(Cat::Sky, EOutcome::Dropped, Label, FMantleSkyMesh::StandaloneDetail());
					continue;
				}
				Coverage.Add(bFog ? Cat::Fog : Cat::Sky, EOutcome::Dropped, Label,
					bFog ? TEXT("Mantle supplies its own fog; this actor's settings are not published.")
						 : TEXT("Mantle supplies its own sky; this actor's settings are not published."));
				continue;
			}

			// Parts that can't publish as they are (#486), landscape splines among them.
			TArray<FMantleUnpublishedPart> Parts;
			FMantlePlaceExporter::DescribeUnpublishedParts(Actor, Parts);
			for (const FMantleUnpublishedPart& Part : Parts)
			{
				if (Part.What.Contains(TEXT("spline mesh")))
				{
					const int32 Splines = CoverageCountComponents(Actor, [](const UActorComponent* Component) { return Component->IsA(USplineMeshComponent::StaticClass()); });
					Coverage.Add(Cat::SplineMeshes, EOutcome::Dropped, Label, TEXT("spline meshes have no place form yet."), FMath::Max(Splines, 1));
				}
				else
				{
					Coverage.Add(Part.Category.IsEmpty() ? FString(Cat::OtherComponents) : Part.Category, EOutcome::Dropped, Label, Part.What);
				}
			}

			// The landscape publishes as a Terrain; SurveyTerrain reports it.
			if (Actor->IsA(ALandscapeProxy::StaticClass()))
			{
				continue;
			}

			// #818: left out instead of refused (camera rigs, a mesh actor with no mesh, a decal with no texture, a Level Instance...):
			// a loss is Dropped; a Level Instance whose actors publish on their own is only a note.
			FMantlePlaceExporter::FSkippedActor Skipped;
			if (FMantlePlaceExporter::DescribeSkippedActor(Actor, Skipped))
			{
				Coverage.Add(Skipped.Category, Skipped.bLoss ? EOutcome::Dropped : EOutcome::Info, Label, Skipped.What);
				continue;
			}

			// Placed on purpose, named by the validator with a remedy.
			FString What;
			FString Remedy;
			if (FMantlePlaceExporter::DescribeUnpublishedContent(Actor, What, Remedy))
			{
				const TCHAR* Category = Actor->IsA(APostProcessVolume::StaticClass()) ? Cat::PostProcess
					: Actor->IsA(AVolume::StaticClass()) ? Cat::Volumes : Cat::OtherActors;
				Coverage.Add(Category, EOutcome::Dropped, Label, What);
				continue;
			}
			if (FMantleCoverageSurvey::IsEditorGeneratorVolume(Actor))
			{
				Coverage.Add(Cat::Volumes, EOutcome::Info, Label,
					TEXT("an editor-only generator: what it generated (the painted foliage) is exported, so leaving the volume out changes nothing."));
				continue;
			}

			if (FMantlePlaceExporter::IsEngineInfrastructure(Actor))
			{
				// A placed reflection capture is left out on purpose (#628): an Info line, not an error or a drop.
				if (FMantleActorDecompose::ReportIgnoredActor(Actor, Coverage))
				{
					continue;
				}
				// An info actor a creator placed (a wind source) is content nobody asked Mantle to publish; the engine's own are NotPlaceable.
				if (Actor->IsA(AInfo::StaticClass()) && !Actor->GetClass()->HasAnyClassFlags(CLASS_NotPlaceable)
					&& !Actor->ActorHasTag(FName(FMantleEnvironment::EnvironmentActorTag)))
				{
					Coverage.Add(Cat::OtherActors, EOutcome::Dropped, Label,
						FString::Printf(TEXT("is a %s, which Mantle does not publish."), *Actor->GetClass()->GetName()));
				}
				continue;
			}

			EMantleEntityType Type;
			if (!FMantlePlaceExporter::ClassifyActor(Actor, Type))
			{
				// Only instanced meshes, reported with the rest of them, or named above with its own reason.
				if (FMantlePlaceExporter::IsInstanceHost(Actor))
				{
					if (Actor->IsA(AVolume::StaticClass()))
					{
						Coverage.Add(Cat::Volumes, EOutcome::Info, Label, TEXT("a generator volume: its generated instances are exported on their own."));
					}
					continue;
				}
				const int32 Particles = CoverageCountComponents(Actor, &CoverageIsParticleComponent);
				if (Particles > 0)
				{
					Coverage.Add(Cat::Particles, EOutcome::Dropped, Label, TEXT("particle systems have no place form yet."), Particles);
				}
				else if (Parts.Num() == 0)
				{
					Coverage.Add(Cat::OtherActors, EOutcome::Dropped, Label,
						FString::Printf(TEXT("is a %s, which Mantle cannot publish."), *Actor->GetClass()->GetName()));
				}
				continue;
			}

			// Exported. A decal is exact only on Mantle's own decal master (#472).
			if (const ADecalActor* DecalActor = Cast<ADecalActor>(Actor))
			{
				if (FMantlePlaceExporter::IsMantleDecalMaterial(DecalActor->GetDecalMaterial()))
				{
					Coverage.Add(Cat::Decals, EOutcome::Kept, Label);
				}
				else
				{
					Coverage.Add(Cat::Decals, EOutcome::Approximated, Label, TEXT("published as its textures; the rest of its material is dropped."));
				}
			}

			// #628: its Blueprint behaviour (one Dropped line per Blueprint class, with the events that run), the editor icons
			// it carries, and the components of a prefab that no entity says (FMantleActorDecompose::ReportActor).
			FMantleActorDecompose::ReportActor(Actor, Type, Coverage);
			CoverageSurveyComponents(Actor, Type, Label, Coverage);
		}
	}

	/** What the place wrote, by entity (what the actors survey leaves to it: meshes, lights, sounds, groups and the Mantle actors). */
	void CoverageSurveyEntities(const FMantlePlace& Place, FMantleCoverage& Coverage)
	{
		bool bSun = false;
		for (const FMantleEntity& Entity : Place.Entities)
		{
			switch (Entity.Type)
			{
			case EMantleEntityType::StaticMesh:
				Coverage.Add(Cat::StaticMeshes, EOutcome::Kept, Entity.Name);
				break;
			case EMantleEntityType::SkeletalMesh:
				Coverage.Add(Cat::SkeletalMeshes, EOutcome::Kept, Entity.Name);
				break;
			case EMantleEntityType::PointLight:
			case EMantleEntityType::SpotLight:
			case EMantleEntityType::RectLight:
				Coverage.Add(Cat::Lights, EOutcome::Kept, Entity.Name);
				break;
			case EMantleEntityType::DirectionalLight:
				// #445: the sky clock sets the first one's angle. Export reports it (FMantleEnvironmentExport::Apply, #628),
				// with what of the level's sun it carried.
				if (!bSun)
				{
					bSun = true;
				}
				else
				{
					Coverage.Add(Cat::Lights, EOutcome::Kept, Entity.Name);
				}
				break;
			case EMantleEntityType::SoundEmitter:
				Coverage.Add(Cat::Sounds, EOutcome::Kept, Entity.Name);
				break;
			case EMantleEntityType::Effect:
				Coverage.Add(Cat::Particles, EOutcome::Kept, Entity.Name);
				break;
			case EMantleEntityType::Group:
			case EMantleEntityType::PlayerStart:
			case EMantleEntityType::TriggerVolume:
			case EMantleEntityType::CrowdSpawner:
			case EMantleEntityType::VehicleSpawn:
			case EMantleEntityType::WeaponSpawn:
			case EMantleEntityType::ScriptPoint:
			case EMantleEntityType::Constraint:
			case EMantleEntityType::Scatter:
				Coverage.Add(Cat::GameplayActors, EOutcome::Kept, Entity.Name);
				break;
			default:
				// Instanced meshes, decals and the terrain are reported by their own surveys.
				break;
			}
		}
	}

	/** Every instance source Export writes (#467), painted foliage apart from the rest. */
	void CoverageSurveyInstances(UWorld* World, FMantleExportResult& Result)
	{
		FMantleCoverage& Coverage = Result.Coverage;
		const TArray<UInstancedStaticMeshComponent*> Sources = FMantlePlaceExporter::CollectInstanceSources(World);
		int64 Total = 0;
		for (const UInstancedStaticMeshComponent* Source : Sources)
		{
			Total += Source->GetInstanceCount();
		}

		// GhzGarage/mantle#818: even thinning was asked for and removed instances. Each source keeps its share of its tier (painted
		// foliage, or the rest) and the removed ones are Dropped, so the report adds up to what the level has and says how many went.
		const FMantleThinReport& Thin = Result.Thinning;
		const bool bThinned = Thin.bApplied && Thin.RemovesAnything();
		const int64 OthersBefore = Thin.InstancesBefore - Thin.FoliageBefore;
		const int64 OthersAfter = Thin.InstancesAfter - Thin.FoliageAfter;
		int64 FoliageSeen = 0;
		int64 OthersSeen = 0;

		for (const UInstancedStaticMeshComponent* Source : Sources)
		{
			const AActor* Owner = Source->GetOwner();
			const bool bFoliage = CoverageIsOrDerives(Owner ? Owner->GetClass() : nullptr, TEXT("InstancedFoliageActor"));
			const FString Category = bFoliage ? Cat::PaintedFoliage : Cat::InstancedMeshes;
			const UStaticMesh* Mesh = Source->GetStaticMesh();
			const FString Name = Mesh ? Mesh->GetName() : FString(TEXT("(no mesh)"));
			const int64 Count = Source->GetInstanceCount();

			FString Blocked;
			FString Approx;
			TArray<FMantleUnpublishedPart> Parts;
			FMantlePlaceExporter::CheckInstanceSource(Source, Parts);
			for (const FMantleUnpublishedPart& Part : Parts)
			{
				(Part.bBlocksPublish ? Blocked : Approx) = Part.What;
			}
			if (!Blocked.IsEmpty())
			{
				Coverage.Add(Category, EOutcome::Dropped, Name, Blocked, Count);
			}
			else if (bThinned)
			{
				// This source's share of the kept instances, rounded along the running count so the shares add up exactly.
				const int64 Before = bFoliage ? Thin.FoliageBefore : OthersBefore;
				const int64 After = bFoliage ? Thin.FoliageAfter : OthersAfter;
				int64& Seen = bFoliage ? FoliageSeen : OthersSeen;
				const int64 KeptBefore = Before > 0 ? (Seen * After) / Before : 0;
				Seen += Count;
				const int64 Kept = FMath::Min(Count, Before > 0 ? (Seen * After) / Before - KeptBefore : Count);
				if (Kept > 0)
				{
					Coverage.Add(Category, Approx.IsEmpty() ? EOutcome::Kept : EOutcome::Approximated, Name, Approx, Kept);
				}
				if (Count - Kept > 0)
				{
					Coverage.Add(Category, EOutcome::Dropped, Name,
						FString::Printf(TEXT("thinned evenly so the place fits (%s)."), *FMantlePlaceInstancing::DescribeBytes(Thin.BytesAfter)), Count - Kept);
				}
			}
			else if (Total > FMantlePlaceLimits::MaxInstancesPerPlace)
			{
				Coverage.Add(Category, EOutcome::Dropped, Name,
					FString::Printf(TEXT("the level has %lld instanced meshes; a place holds at most %d."), Total, FMantlePlaceLimits::MaxInstancesPerPlace), Count);
			}
			else if (!Approx.IsEmpty())
			{
				Coverage.Add(Category, EOutcome::Approximated, Name, Approx, Count);
			}
			else
			{
				Coverage.Add(Category, EOutcome::Kept, Name, FString(), Count);
			}
		}
	}

	/** A loss as the report says it, in a few words. */
	FString CoverageLossName(EMantleMaterialLoss Loss, const FMantleMaterialAnalysis& Analysis)
	{
		switch (Loss)
		{
		case EMantleMaterialLoss::Animation: return TEXT("animation frozen");
		case EMantleMaterialLoss::WorldPositionOffset: return TEXT("wind / vertex animation");
		case EMantleMaterialLoss::WorldAligned: return TEXT("world-aligned mapping");
		case EMantleMaterialLoss::Refraction: return TEXT("refraction shown as smooth tinted glass with the reflectance of its index (the bending of the scene behind is not drawn)");
		case EMantleMaterialLoss::Subsurface: return TEXT("subsurface");
		case EMantleMaterialLoss::Parallax: return TEXT("parallax");
		case EMantleMaterialLoss::VertexColor: return TEXT("vertex-colour blending");
		case EMantleMaterialLoss::DetailNormal: return TEXT("detail normals baked");
		case EMantleMaterialLoss::PixelDepthOffset: return TEXT("pixel depth offset");
		case EMantleMaterialLoss::Displacement: return TEXT("displacement");
		case EMantleMaterialLoss::SpecialShading: return FString::Printf(TEXT("%s shading"), *Analysis.ShadingModelName);
		case EMantleMaterialLoss::Unlit: return TEXT("unlit");
		case EMantleMaterialLoss::BlendMode: return FString::Printf(TEXT("%s blending"), *Analysis.BlendModeName);
		case EMantleMaterialLoss::NotSurface: return TEXT("not a surface material");
		default: return FString();
		}
	}

	/** The losses that only soften or change a material rather than take a feature away: it is approximated, not lost. */
	constexpr EMantleMaterialLoss CoverageSoftLosses =
		EMantleMaterialLoss::DetailNormal | EMantleMaterialLoss::Unlit | EMantleMaterialLoss::BlendMode | EMantleMaterialLoss::NotSurface;

	/** The features the material document carries beyond a plain bake. */
	FString CoverageRecognised(const FMantleMaterialAnalysis& Analysis)
	{
		const FMantleMaterialFidelity2& Fidelity = Analysis.Fidelity2;
		TArray<FString> Features;
		for (int32 Slot = 0; Slot < 4; ++Slot)
		{
			if (Fidelity.HasSource(Slot))
			{
				Features.Add(TEXT("source textures"));
				break;
			}
		}
		if (Fidelity.bWind) { Features.Add(TEXT("wind")); }
		if (Fidelity.bFoliage) { Features.Add(TEXT("foliage shading")); }
		if (Fidelity.bWorldAligned) { Features.Add(TEXT("world-aligned mapping")); }
		if (Fidelity.bLayerBlend) { Features.Add(TEXT("layer blend")); }
		if (Fidelity.bPanning[0] || Fidelity.bPanning[1] || Fidelity.bPanning[2] || Fidelity.bPanning[3]) { Features.Add(TEXT("panning")); }
		const FMantleWaterBake& Water = Fidelity.Water;
		if (Water.bFound && Water.PublishesOpaque() && FMantleTargetFeatures::Current().Reads(MantleClientFeatures::MaterialFidelity))
		{
			// The water key (Single Layer Water): the surface, roughness and normal maps are kept; the fade becomes the volume.
			int32 Layers = 0;
			bool bApproximated = false;
			for (const FMantleWaterNormalLayer& Layer : Water.Normals)
			{
				if (Layer.Texture.IsValid() && Layers < 2)
				{
					++Layers;
					bApproximated = bApproximated || Layer.bApproximated;
				}
			}
			Features.Add(FString::Printf(TEXT("water shading (Single Layer Water; opacity %.2f), %s, the depth fade turned into absorption and scattering%s%s"), Water.Opacity,
				Layers == 0 ? TEXT("its normal baked") : (Layers == 1 ? TEXT("one normal map with its layout and pan") : TEXT("two normal maps with their layout and pan")),
				Water.bFadeDistance ? TEXT("") : TEXT(" (no fade distance in the graph, 100 cm assumed)"),
				bApproximated ? TEXT("; a world-laid normal is carried only roughly (one size on both axes, one sample of its function)") : TEXT("")));
		}
		else
		{
			if (Water.bNormalPanned)
			{
				Features.Add(Water.NormalSamples > 1 ? TEXT("two panned normals published as one (the fastest)") : TEXT("panned water normal"));
			}
			if (Water.bFound)
			{
				Features.Add(Water.PublishesOpaque()
					? FString::Printf(TEXT("near-opaque depth-fade water (opacity %.2f) published opaque, so it reflects; the see-through edge is lost"), Water.Opacity)
					: FString::Printf(TEXT("depth fade baked to a constant opacity (%.2f%s)"), Water.Opacity, Water.bOpacityRead ? TEXT("") : TEXT(", estimated")));
			}
		}
		if (Fidelity.bAdditive) { Features.Add(TEXT("additive blending (the glow only: the base colour has no part in it)")); }
		if (Fidelity.Glass.bFound)
		{
			Features.Add(FString::Printf(TEXT("scene-colour refraction drawn as tinted see-through glass (opacity %.2f, %s tint)"), FMantleSeeThroughGlass::Opacity,
				Fidelity.Glass.bTint ? TEXT("the source's") : TEXT("a neutral")));
		}
		if (Analysis.bCarPaint) { Features.Add(TEXT("car paint")); }
		if (Analysis.bVertexColorTintsBaseColor) { Features.Add(TEXT("vertex-colour tint")); }
		return FString::Join(Features, TEXT(", "));
	}

	/** Materials from Mantle's library are exact: the ones the place's meshes and terrain layers name. */
	void CoverageSurveyLibraryMaterials(const FMantlePlace& Place, FMantleCoverage& Coverage)
	{
		const FMantleAssetLibrary& Library = FMantleAssetLibrary::Get();
		TSet<FString> SeenIds;
		auto ReportLibrary = [&](const FString& Id)
		{
			const FMantleAssetEntry* Entry = Id.IsEmpty() ? nullptr : Library.Find(Id);
			bool bAlreadySeen = true;
			if (Entry && Entry->Kind == EMantleAssetKind::Material && (SeenIds.Add(Id, &bAlreadySeen), !bAlreadySeen))
			{
				Coverage.Add(Cat::Materials, EOutcome::Kept, Entry->Path.GetAssetName(), TEXT("Mantle library material."));
			}
		};
		for (const FMantleEntity& Entity : Place.Entities)
		{
			if (const FMantleStaticMeshProps* Mesh = MantleMeshPropsOf(Entity))
			{
				for (const FString& Id : Mesh->Materials)
				{
					ReportLibrary(Id);
				}
			}
			else if (const FMantleTerrainProps* Terrain = Entity.Props.TryGet<FMantleTerrainProps>())
			{
				for (const FMantleTerrainLayer& Layer : Terrain->Layers)
				{
					ReportLibrary(Layer.Material);
				}
			}
		}
	}

	/** Each project material (a Fab pack's) is recognised, baked or lost, by what AnalyzeMaterial finds in it. */
	void CoverageSurveyProjectMaterials(const FMantlePlaceDependencies& Dependencies, FMantleCoverage& Coverage)
	{
		for (const FMantleGatheredMaterial& Gathered : Dependencies.Materials)
		{
			const UMaterialInterface* Material = Gathered.Material.Get();
			if (!Material)
			{
				continue;
			}
			const FMantleMaterialAnalysis Analysis = FMantlePlaceAssets::AnalyzeMaterial(Material);
			TArray<FString> Losses;
			for (uint32 Bit = 1; Bit != 0 && Bit <= static_cast<uint32>(EMantleMaterialLoss::NotSurface); Bit <<= 1)
			{
				const EMantleMaterialLoss Loss = static_cast<EMantleMaterialLoss>(Bit);
				if (Analysis.Has(Loss))
				{
					Losses.Add(CoverageLossName(Loss, Analysis));
				}
			}
			// #628: what an additive surface gives up in becoming Mantle's: it is drawn unlit, and a Fresnel rim glow is dropped.
			if (Analysis.Fidelity2.bAdditiveLit)
			{
				Losses.Add(TEXT("lit additive drawn unlit (lighting on the surface lost)"));
			}
			if (Analysis.Fidelity2.bAdditiveRim)
			{
				Losses.Add(TEXT("Fresnel rim glow dropped"));
			}
			// #628: the bake has no instance, so per-instance variation is baked at its average.
			if (Analysis.bPerInstanceVariation)
			{
				Losses.Add(TEXT("per-instance colour variation averaged"));
			}
			const FString Recognised = CoverageRecognised(Analysis);
			if (Losses.Num() == 0)
			{
				if (Recognised.IsEmpty())
				{
					Coverage.Add(Cat::Materials, EOutcome::Approximated, Gathered.Name, TEXT("baked to PBR textures."));
				}
				else if (Analysis.Fidelity2.Water.bFound || Analysis.Fidelity2.Glass.bFound)
				{
					// Water's depth fade is a constant here, never the real fade, and see-through glass (#628) is the source's look drawn as
					// a tinted translucent surface: approximated, not kept.
					Coverage.Add(Cat::Materials, EOutcome::Approximated, Gathered.Name, FString::Printf(TEXT("%s."), *Recognised));
				}
				else
				{
					Coverage.Add(Cat::Materials, EOutcome::Kept, Gathered.Name, FString::Printf(TEXT("recognised: %s."), *Recognised));
				}
				continue;
			}
			// The material itself is exported either way, so it is approximated, never dropped; what it lost is the detail.
			FString Detail = FString::Printf(TEXT("%s: %s."), (Analysis.Losses & ~CoverageSoftLosses) == EMantleMaterialLoss::None ? TEXT("baked, changed") : TEXT("exported without"),
				*FString::Join(Losses, TEXT(", ")));
			if (!Recognised.IsEmpty())
			{
				Detail += FString::Printf(TEXT(" Recognised: %s."), *Recognised);
			}
			Coverage.Add(Cat::Materials, EOutcome::Approximated, Gathered.Name, Detail);
		}
	}

	/** The landscape: published as a Terrain or not, the landscape material (never used yet) and the grass its material grows (#628). */
	void CoverageSurveyTerrain(UWorld* World, FMantleExportResult& Result)
	{
		FMantleCoverage& Coverage = Result.Coverage;
		const TArray<AActor*> Landscapes = FMantleTerrainExport::CollectLandscapes(World);
		for (const AActor* Landscape : Landscapes)
		{
			const FString Label = CoverageLabelOf(Landscape);
			const FString Name = Label.Left(FMantlePlaceLimits::MaxEntityNameLength);
			const FMantleEntity* Published = Result.Place.Entities.FindByPredicate([&Name](const FMantleEntity& Entity)
			{
				return Entity.Type == EMantleEntityType::Terrain && Entity.Name == Name;
			});

			if (Published)
			{
				// A landscape wider than the heightfield format allows is resampled.
				bool bResampled = false;
				if (const ALandscapeProxy* Proxy = Cast<ALandscapeProxy>(Landscape))
				{
					int32 MinX = 0;
					int32 MinY = 0;
					int32 MaxX = 0;
					int32 MaxY = 0;
					const ULandscapeInfo* Info = Proxy->GetLandscapeInfo();
					bResampled = Info && Info->GetLandscapeExtent(MinX, MinY, MaxX, MaxY)
						&& (MaxX - MinX + 1 > FMantleHeightfieldReader::MaxSize || MaxY - MinY + 1 > FMantleHeightfieldReader::MaxSize);
				}
				if (bResampled)
				{
					Coverage.Add(Cat::Terrain, EOutcome::Approximated, Label, TEXT("resampled to the most samples a terrain holds."));
				}
				else
				{
					Coverage.Add(Cat::Terrain, EOutcome::Kept, Label);
				}
			}
			else
			{
				// Export refused it: its own sentences say why.
				const FString SourcePath = FString::Printf(TEXT("/actors/%s"), *Landscape->GetName());
				TArray<FString> Why;
				for (const FMantleParseError& Error : Result.Errors)
				{
					if (Error.Path == SourcePath)
					{
						Why.Add(Error.Message);
					}
				}
				Coverage.Add(Cat::Terrain, EOutcome::Dropped, Label,
					Why.Num() > 0 ? FString::Join(Why, TEXT(" ")) : FString(TEXT("is not in the place.")));
			}

			// The landscape's own material is never read: every layer takes a material from Mantle Place Settings.
			const ALandscapeProxy* Proxy = Cast<ALandscapeProxy>(Landscape);
			const UMaterialInterface* LandscapeMaterial = Proxy ? Proxy->GetLandscapeMaterial() : nullptr;
			if (LandscapeMaterial && !Coverage.HasCategory(Cat::LandscapeMaterial))
			{
				Coverage.Add(Cat::LandscapeMaterial, EOutcome::Dropped, Label,
					FString::Printf(TEXT("its material '%s' is not used: each layer takes a material from Mantle Place Settings."), *LandscapeMaterial->GetName()));
			}

			// The grass types its material grows. A Terrain that carries grass entries keeps them; Export reports what it did itself.
			const TArray<FMantleCoverageSurvey::FGrassType> GrassTypes = FMantleCoverageSurvey::FindLandscapeGrassTypes(Landscape);
			if (GrassTypes.Num() > 0 && !Coverage.HasCategory(Cat::LandscapeGrass))
			{
				const FMantleTerrainProps* Props = Published ? Published->Props.TryGet<FMantleTerrainProps>() : nullptr;
				if (Props && Props->Grass.Num() > 0)
				{
					Coverage.Add(Cat::LandscapeGrass, EOutcome::Kept, Label,
						FString::Printf(TEXT("%d grass entries from %d grass types."), Props->Grass.Num(), GrassTypes.Num()), Props->Grass.Num());
				}
				else
				{
					for (const FMantleCoverageSurvey::FGrassType& Type : GrassTypes)
					{
						Coverage.Add(Cat::LandscapeGrass, EOutcome::Dropped, Type.Name, Type.Varieties);
					}
				}
			}
		}
	}

	/** Sub-levels: loaded ones are part of the world Export walks; any other is not exported. */
	void CoverageSurveyLevels(UWorld* World, FMantleCoverage& Coverage)
	{
		for (const ULevelStreaming* Streaming : World->GetStreamingLevels())
		{
			if (!Streaming)
			{
				continue;
			}
			const FString Name = FPackageName::GetShortName(Streaming->GetWorldAssetPackageName());
			const ULevel* Loaded = Streaming->GetLoadedLevel();
			if (Loaded && Loaded->bIsVisible)
			{
				Coverage.Add(Cat::SubLevels, EOutcome::Kept, Name, TEXT("loaded in the editor: its actors are exported with the rest."));
			}
			else
			{
				Coverage.Add(Cat::SubLevels, EOutcome::Dropped, Name, TEXT("not loaded and visible in the editor, so its actors are not exported."));
			}
		}
	}

	/** The persistent level's Level Blueprint: nothing of its logic is published (#502). */
	void CoverageSurveyLevelBlueprint(UWorld* World, FMantleCoverage& Coverage)
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
			Coverage.Add(Cat::BlueprintLogic, EOutcome::Dropped, TEXT("Level Blueprint"), FString::Join(LogicWhere, TEXT(", ")));
		}
#endif
	}
}

FString FMantleCoverageSurvey::ComponentCategory(const UActorComponent* Component)
{
	return MantleCoverageSurveyPrivate::CoverageComponentCategory(Component);
}

bool FMantleCoverageSurvey::IsEditorGeneratorVolume(const AActor* Actor)
{
	using namespace MantleCoverageSurveyPrivate;
	const UClass* Class = Actor ? Actor->GetClass() : nullptr;
	return CoverageIsOrDerives(Class, TEXT("ProceduralFoliageVolume")) || CoverageIsOrDerives(Class, TEXT("ProceduralFoliageBlockingVolume"));
}

TArray<FMantleCoverageSurvey::FGrassType> FMantleCoverageSurvey::FindLandscapeGrassTypes(const AActor* Landscape)
{
	TArray<FGrassType> Found;
	const ALandscapeProxy* Proxy = Cast<ALandscapeProxy>(Landscape);
	if (!Proxy)
	{
		return Found;
	}

	TArray<const ALandscapeProxy*> Proxies;
	Proxies.Add(Proxy);
	if (const ULandscapeInfo* Info = Proxy->GetLandscapeInfo())
	{
		Info->ForEachLandscapeProxy([&Proxies](ALandscapeProxy* Other) -> bool
		{
			Proxies.AddUnique(Other);
			return true;
		});
	}

	TArray<const ULandscapeGrassType*> Types;
	for (const ALandscapeProxy* Each : Proxies)
	{
		const UMaterialInterface* Material = Each->GetLandscapeMaterial();
		const UMaterial* Base = Material ? Material->GetMaterial() : nullptr;
		if (!Base)
		{
			continue;
		}
		for (const TPair<FName, TObjectPtr<ULandscapeGrassType>>& Pair : Base->GetCachedExpressionData().NamedGrassTypes)
		{
			if (Pair.Value)
			{
				Types.AddUnique(Pair.Value.Get());
			}
		}
	}
	Types.Sort([](const ULandscapeGrassType& A, const ULandscapeGrassType& B) { return A.GetName() < B.GetName(); });

	for (const ULandscapeGrassType* Type : Types)
	{
		FGrassType& Entry = Found.AddDefaulted_GetRef();
		Entry.Name = Type->GetName();
		TArray<FString> Varieties;
		for (const FGrassVariety& Variety : Type->GrassVarieties)
		{
			Varieties.Add(FString::Printf(TEXT("%s (%g per 100 m2)"),
				Variety.GrassMesh ? *Variety.GrassMesh->GetName() : TEXT("no mesh"), Variety.GrassDensity.Default));
		}
		Entry.Varieties = Varieties.Num() > 0
			? FString::Printf(TEXT("%d variet%s: %s."), Varieties.Num(), Varieties.Num() == 1 ? TEXT("y") : TEXT("ies"), *FString::Join(Varieties, TEXT(", ")))
			: FString(TEXT("no varieties."));
	}
	return Found;
}

void FMantleCoverageSurvey::ReportTerrainLayers(FMantleCoverage& Out, const FString& Label, const FMantleTerrainSource& Source,
	const TArray<FMantleTerrainLayerChoice>& Layers)
{
	// Whoever chose the layers may have reported them already (ChooseLayers does, once it keeps layers it cannot name here).
	if (Out.HasCategory(MantleCoverageCategory::TerrainLayers))
	{
		return;
	}
	TSet<FName> Published;
	for (const FMantleTerrainLayerChoice& Choice : Layers)
	{
		Published.Add(Choice.Layer);
		// #628: a layer drawn from the landscape's own material is exact only when the material gave it no extra that is lost.
		const FMantleLandscapeLayerLook* Look = Choice.bDerived ? Source.MaterialRead.Find(Choice.Layer) : nullptr;
		const bool bApproximated = Look && (Look->bUnreadable || Look->Approximations.Num() > 0);
		FString Detail = FString::Printf(TEXT("on '%s', material %s, tiled every %g cm%s."), *Label,
			Choice.Material ? *Choice.Material->GetName() : TEXT("none"), Choice.TilingCm, Choice.bDerived ? TEXT(", read from the landscape's material") : TEXT(""));
		if (Look && Look->bUnreadable)
		{
			Detail += TEXT(" Its look is not a texture or colour the material states: plain grey.");
		}
		if (Look)
		{
			for (const FString& Note : Look->Approximations)
			{
				Detail += TEXT(" ") + Note;
			}
		}
		Out.Add(MantleCoverageCategory::TerrainLayers, bApproximated ? EMantleCoverageOutcome::Approximated : EMantleCoverageOutcome::Kept, Choice.Layer.ToString(), Detail);

		// More layers painted than Mantle publishes (#628): their paint went into this one.
		for (const int32 Folded : Choice.FoldedSources)
		{
			if (Source.LayerNames.IsValidIndex(Folded))
			{
				Published.Add(Source.LayerNames[Folded]);
				Out.Add(MantleCoverageCategory::TerrainLayers, EMantleCoverageOutcome::Approximated, Source.LayerNames[Folded].ToString(),
					FString::Printf(TEXT("painted on '%s', but Mantle publishes at most %d layers: its paint was folded into '%s'."), *Label,
						FMantlePlaceLimits::MaxTerrainLayers, *Choice.Layer.ToString()));
			}
		}
	}
	for (const FName& Painted : Source.LayerNames)
	{
		if (!Published.Contains(Painted))
		{
			const bool bUnused = Source.bMaterialRead && !Source.MaterialRead.Find(Painted);
			Out.Add(MantleCoverageCategory::TerrainLayers, EMantleCoverageOutcome::Dropped, Painted.ToString(),
				bUnused ? FString::Printf(TEXT("painted on '%s', but its landscape material does not use this layer, so nothing says what it looks like."), *Label)
					: FString::Printf(TEXT("painted on '%s' but not published."), *Label));
		}
	}
}

void FMantleCoverageSurvey::AddMaterials(const FMantlePlaceDependencies& Dependencies, FMantleCoverage& Out)
{
	MantleCoverageSurveyPrivate::CoverageSurveyProjectMaterials(Dependencies, Out);
	Out.Sort();
}

void FMantleCoverageSurvey::Run(UWorld* World, FMantleExportResult& Result)
{
	using namespace MantleCoverageSurveyPrivate;
	if (!World)
	{
		return;
	}
	CoverageSurveyActors(World, Result);
	CoverageSurveyEntities(Result.Place, Result.Coverage);
	CoverageSurveyInstances(World, Result);
	CoverageSurveyLibraryMaterials(Result.Place, Result.Coverage);
	CoverageSurveyTerrain(World, Result);
	CoverageSurveyLevels(World, Result.Coverage);
	CoverageSurveyLevelBlueprint(World, Result.Coverage);
	Result.Coverage.Sort();
}
