// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceExporter.h"
#include "MantleActorDecompose.h"
#include "MantleApiManifest.h"
#include "MantleAssetLibrary.h"
#include "MantleConstraint.h"
#include "MantleCoverageSurvey.h"
#include "MantleCrowdSpawner.h"
#include "MantleEffect.h"
#include "MantleEffectExport.h"
#include "MantleClientFeatures.h"
#include "MantleDrawDistance.h"
#include "MantleTargetFeatures.h"
#include "MantleTextExport.h"
#include "MantleEnvironment.h"
#include "MantleEnvironmentExport.h"
#include "MantleEnvironmentOverride.h"
#include "MantlePhysicsProp.h"
#include "MantlePlaceAssetConverter.h"
#include "MantlePlaceAssets.h"
#include "MantlePlaceBuilder.h"
#include "MantlePlaceLimits.h"
#include "MantlePlaceSettingsActor.h"
#include "MantlePlaceStorage.h"
#include "MantlePlaceValidator.h"
#include "MantlePlaceWorldPartition.h"
#include "MantleQuickJSRuntime.h"
#include "MantleReflectionProbeExport.h"
#include "MantleSeatExport.h"
#include "MantleRmlDocumentRules.h"
#include "MantleRuntime.h"
#include "MantleScatterVolume.h"
#include "MantleScriptApiUsage.h"
#include "MantleScriptError.h"
#include "MantleSplineMeshBake.h"
#include "MantleTerrainBuilder.h"
#include "MantleTerrainExport.h"
#include "MantleTerrainGrassExport.h"
#include "MantleVehicleConverter.h"
#include "MantleVehicleSource.h"
#include "MantleVehicleSpawnPoint.h"
#include "MantleWaterBody.h"
#include "MantleWaterBodyExport.h"
#include "MantleWeaponSpawnPoint.h"
#include "MantleScriptPoint.h"
#include "MantleScriptPackages.h"
#include "MantleSkyMesh.h"
#include "Graph/MantleGraphCompiler.h"
#include "Graph/MantleGraphTypes.h"
#include "Graph/MantleScriptEdGraph.h"
#include "Graph/MantleScriptGraphAsset.h"
#include "Graph/MantleScriptGraphSerializer.h"

#include "ActorEditorUtils.h"
#include "Animation/AnimSequence.h"
#include "Animation/SkeletalMeshActor.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Camera/CameraActor.h"
#include "Components/ArrowComponent.h"
#include "Components/AudioComponent.h"
#include "Components/BillboardComponent.h"
#include "Components/BoxComponent.h"
#include "Components/DecalComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/LightComponentBase.h"
#include "Components/PointLightComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/RectLightComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SphereComponent.h"
#include "Components/SplineComponent.h"
#include "Components/SplineMeshComponent.h"
#include "Components/SpotLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "Engine/Brush.h"
#include "Engine/DecalActor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/Level.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/PointLight.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/RectLight.h"
#include "Engine/Scene.h"
#include "Engine/SkyLight.h"
#include "Engine/SpotLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/TextRenderActor.h"
#include "Engine/Texture.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Info.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/DefaultPhysicsVolume.h"
#include "GameFramework/Volume.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "LevelInstance/LevelInstanceInterface.h"
#include "LevelInstance/LevelInstanceSubsystem.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Modules/ModuleManager.h"
#include "Particles/ParticleSystemComponent.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/PhysicsConstraintActor.h"
#include "PhysicsEngine/PhysicsConstraintComponent.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Sound/AmbientSound.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundCue.h"
#include "UObject/EnumProperty.h"
#include "UObject/UnrealType.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantlePlaceExporterUploads
{
	/**
	 * The uploaded (non-library) assets one export references (F5 #339): ids from the upload queue's
	 * records when there are any, provisional ids otherwise, and the keys in first-use order for
	 * the manifest.
	 */
	struct FContext
	{
		const FMantlePlaceAssetSet* Assets = nullptr;
		FMantlePlaceAssetLimits Limits;
		TArray<FString> Keys;
		TMap<FString, EMantleAssetKind> KindOf;
		TMap<FString, FSoftObjectPath> SourceOf;

		/** Uploaded ids the level names as they are, with no local asset to convert (an imported spawn's vehicle, #435). */
		TMap<FString, EMantleAssetKind> NamedIds;

		FString IdFor(const FString& Key, EMantleAssetKind Kind, const UObject* Source)
		{
			if (!KindOf.Contains(Key))
			{
				Keys.Add(Key);
				KindOf.Add(Key, Kind);
				SourceOf.Add(Key, FSoftObjectPath(Source));
			}
			const FMantlePlaceUploadedRef* Ref = Assets ? Assets->Uploaded.Find(Key) : nullptr;
			return Ref && !Ref->Id.IsEmpty() ? Ref->Id : FMantlePlaceAssets::ProvisionalId(Key);
		}
	};
}

namespace
{
	using FWriter = TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>;
	using FWriterFactory = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>;


	FString SanitiseId(const FString& In)
	{
		FString Out;
		Out.Reserve(In.Len());
		for (const TCHAR Char : In)
		{
			// ASCII only: the schema's entity id is ^ent_[a-z0-9_], and FChar::IsAlnum also
			// accepts letters like 'é' (#267).
			if (Char < 128 && FChar::IsAlnum(Char))
			{
				Out.AppendChar(FChar::ToLower(Char));
			}
			else if (Out.Len() > 0 && Out[Out.Len() - 1] != TEXT('_'))
			{
				Out.AppendChar(TEXT('_'));
			}
		}
		Out.RemoveFromEnd(TEXT("_"));
		if (Out.IsEmpty())
		{
			Out = TEXT("entity");
		}
		// Schema allows ent_ plus up to 40 characters.
		return Out.Left(36);
	}

	EMantleScriptSide MapGraphSide(EMantleSide In)
	{
		return In == EMantleSide::Client ? EMantleScriptSide::Client : EMantleScriptSide::Server;
	}

	/**
	 * EntityIdFor, made unique within one export. Two actors only share a preferred id in
	 * the fallback cases EntityIdFor describes, or when two actors really do share an
	 * ActorGuid (actors streamed in from the same level instance twice). A duplicate id is
	 * rejected by the parser (#267), so the later actor gets a numeric suffix rather than
	 * failing the whole publish: the creator cannot edit an ActorGuid, so an error here
	 * would leave them with nothing to fix. The suffixed id is still schema-valid
	 * (ent_ + 32 hex + _N stays within 40 characters); it is only as stable as the level's
	 * actor order, which is why it is logged.
	 */
	FString MakeEntityId(const AActor* Actor, TSet<FString>& Used)
	{
		const FString Base = FMantlePlaceExporter::EntityIdFor(Actor);

		FString Candidate = Base;
		int32 Suffix = 2;
		while (Used.Contains(Candidate))
		{
			Candidate = FString::Printf(TEXT("%s_%d"), *Base.Left(36), Suffix++);
		}
		if (Candidate != Base)
		{
			UE_LOG(LogMantle, Warning,
				TEXT("'%s' shares entity id '%s' with another actor; exported as '%s', which may change between publishes"),
				*Actor->GetName(), *Base, *Candidate);
		}
		Used.Add(Candidate);
		return Candidate;
	}

	/** Maps a catalogue source language to what the schema/place file can express. */
	bool MapCatalogLanguage(EMantleScriptCatalogLanguage In, EMantleScriptLanguage& Out)
	{
		switch (In)
		{
		case EMantleScriptCatalogLanguage::Luau:       Out = EMantleScriptLanguage::Luau;       return true;
		case EMantleScriptCatalogLanguage::JavaScript: Out = EMantleScriptLanguage::JavaScript; return true;
		case EMantleScriptCatalogLanguage::TypeScript: Out = EMantleScriptLanguage::TypeScript; return true;
		case EMantleScriptCatalogLanguage::Python: Out = EMantleScriptLanguage::Python; return true;
		default:
			// Unknown, or a language the catalogue has learned that this schema/exporter
			// hasn't caught up with yet. Callers report this clearly rather than
			// guess at a mapping.
			return false;
		}
	}

	EMantleScriptSide MapCatalogSide(EMantleSide In)
	{
		switch (In)
		{
		case EMantleSide::Client: return EMantleScriptSide::Client;
		case EMantleSide::Both:   return EMantleScriptSide::Shared;
		case EMantleSide::Server:
		default:                 return EMantleScriptSide::Server;
		}
	}

	/** The plugin's vendored TypeScript compiler bundle, loaded once. */
	const FString& ExporterTypeScriptCompilerBundle()
	{
		static const FString Bundle = []
		{
			const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("MantleCore"));
			if (!Plugin.IsValid())
			{
				return FString();
			}

			FString Source;
			const FString Path = FPaths::Combine(
				Plugin->GetBaseDir(),
				TEXT("Source"),
				TEXT("ThirdParty"),
				TEXT("TypeScriptCompiler"),
				TEXT("typescript.js"));
			if (!FFileHelper::LoadFileToString(Source, *Path))
			{
				UE_LOG(LogMantle, Error, TEXT("Could not load bundled TypeScript compiler: %s"), *Path);
				return FString();
			}
			return Source;
		}();
		return Bundle;
	}

	/**
	 * scriptApi: the running plugin's major (S1.9b, #130; read from code, not the repo's schema/
	 * folder, since creators install the plugin from Fab and have no repo) and the lowest minor
	 * the place's scripts actually use (#394, "record what's used"), so a place that uses nothing
	 * new still runs on older clients. Need is every exported script's FMantleScriptApiUsage
	 * result merged; an unanalysable script has already raised it to CurrentMinor.
	 */
	FMantleScriptApiVersion RequiredScriptApiVersion(const FMantleScriptApiNeed& Need)
	{
		FMantleScriptApiVersion Version;
		Version.Major = FMantleApiManifest::Get().Major;
		Version.Minor = FMath::Clamp(Need.Minor, 0, FMantleApiManifest::Get().Minor);
		return Version;
	}

	void WriteVector(const TSharedRef<FWriter>& Writer, const TCHAR* Key, const FVector& Value)
	{
		Writer->WriteArrayStart(Key);
		Writer->WriteValue(Value.X);
		Writer->WriteValue(Value.Y);
		Writer->WriteValue(Value.Z);
		Writer->WriteArrayEnd();
	}

	void WriteColor(const TSharedRef<FWriter>& Writer, const TCHAR* Key, const FLinearColor& Value)
	{
		Writer->WriteArrayStart(Key);
		Writer->WriteValue(static_cast<double>(Value.R));
		Writer->WriteValue(static_cast<double>(Value.G));
		Writer->WriteValue(static_cast<double>(Value.B));
		Writer->WriteArrayEnd();
	}

	void WriteTransform(const TSharedRef<FWriter>& Writer, const FTransform& Transform)
	{
		Writer->WriteObjectStart(TEXT("transform"));
		WriteVector(Writer, TEXT("location"), Transform.GetLocation());
		const FRotator Rotation = Transform.GetRotation().Rotator();
		Writer->WriteArrayStart(TEXT("rotation"));
		Writer->WriteValue(Rotation.Pitch);
		Writer->WriteValue(Rotation.Yaw);
		Writer->WriteValue(Rotation.Roll);
		Writer->WriteArrayEnd();
		WriteVector(Writer, TEXT("scale"), Transform.GetScale3D());
		Writer->WriteObjectEnd();
	}

	/** The place-file unit for a light component's IntensityUnits (#385). */
	EMantleLightUnits ExporterMantleLightUnits(ELightUnits Units)
	{
		switch (Units)
		{
		case ELightUnits::Candelas: return EMantleLightUnits::Candelas;
		case ELightUnits::Lumens:   return EMantleLightUnits::Lumens;
		case ELightUnits::EV:       return EMantleLightUnits::EV;
		case ELightUnits::Nits:     return EMantleLightUnits::Nits;
		default:                    return EMantleLightUnits::Unitless;
		}
	}

	/**
	 * Writes intensityUnits only when it is not Unitless, the default an absent field means:
	 * clients from before #385 refuse unknown light properties, so writing the default would
	 * make every place with a unitless light unreadable to them.
	 */
	void WriteLightUnits(const TSharedRef<FWriter>& Writer, EMantleLightUnits Units)
	{
		if (Units != EMantleLightUnits::Unitless)
		{
			Writer->WriteValue(TEXT("intensityUnits"), MantleLightUnitsToString(Units));
		}
	}

	/**
	 * useTemperature and temperature (#472), only for a light that uses its temperature, and the
	 * temperature only when it is not the 6500 K default: clients from before #472 refuse both keys
	 * on a Point or Spot light, so a light without them stays readable to them.
	 */
	void WriteLightTemperature(const TSharedRef<FWriter>& Writer, bool bUseTemperature, double Temperature)
	{
		if (!bUseTemperature)
		{
			return;
		}
		Writer->WriteValue(TEXT("useTemperature"), true);
		if (Temperature != MANTLE_DEFAULT_LIGHT_TEMPERATURE)
		{
			Writer->WriteValue(TEXT("temperature"), Temperature);
		}
	}

	/** A Point or Spot light's source shape (#472), each key only when above zero, for the same reason. */
	void WriteLightSourceShape(const TSharedRef<FWriter>& Writer, const FMantleLightSourceShape& Source)
	{
		if (Source.SourceRadius > 0.0)
		{
			Writer->WriteValue(TEXT("sourceRadius"), Source.SourceRadius);
		}
		if (Source.SoftSourceRadius > 0.0)
		{
			Writer->WriteValue(TEXT("softSourceRadius"), Source.SoftSourceRadius);
		}
		if (Source.SourceLength > 0.0)
		{
			Writer->WriteValue(TEXT("sourceLength"), Source.SourceLength);
		}
	}

	/** An optional [min, max] pair, written only when it differs from the default. */
	void WriteCrowdRangePair(const TSharedRef<FWriter>& Writer, const TCHAR* Key, double Min, double Max, double DefaultMin, double DefaultMax)
	{
		if (Min == DefaultMin && Max == DefaultMax)
		{
			return;
		}
		Writer->WriteArrayStart(Key);
		Writer->WriteValue(Min);
		Writer->WriteValue(Max);
		Writer->WriteArrayEnd();
	}

	/**
	 * A CrowdSpawner's properties (#477). Every optional key is written only when it differs from
	 * the schema's default, the way the other optional keys here are, so a plain spawner's JSON
	 * stays short and within what the schema's CrowdSpawner branch allows.
	 */
	void WriteCrowdSpawnerProperties(const TSharedRef<FWriter>& Writer, const FMantleCrowdSpawnerProps& Props)
	{
		const FMantleCrowdSpawnerProps Defaults;
		const FMantleCrowdLane DefaultLane;

		Writer->WriteArrayStart(TEXT("lanes"));
		for (const FMantleCrowdLane& Lane : Props.Lanes)
		{
			Writer->WriteObjectStart();
			Writer->WriteArrayStart(TEXT("points"));
			for (const FVector& Point : Lane.Points)
			{
				Writer->WriteArrayStart();
				Writer->WriteValue(Point.X);
				Writer->WriteValue(Point.Y);
				Writer->WriteValue(Point.Z);
				Writer->WriteArrayEnd();
			}
			Writer->WriteArrayEnd();
			if (Lane.Width != DefaultLane.Width)
			{
				Writer->WriteValue(TEXT("width"), Lane.Width);
			}
			if (Lane.bTwoWay != DefaultLane.bTwoWay)
			{
				Writer->WriteValue(TEXT("twoWay"), Lane.bTwoWay);
			}
			Writer->WriteObjectEnd();
		}
		Writer->WriteArrayEnd();

		Writer->WriteValue(TEXT("count"), Props.Count);
		Writer->WriteValue(TEXT("seed"), Props.Seed);

		if (Props.Looks.Num() > 0)
		{
			Writer->WriteArrayStart(TEXT("looks"));
			for (const FMantleCrowdLook& Look : Props.Looks)
			{
				Writer->WriteObjectStart();
				Writer->WriteObjectStart(TEXT("body"));
				Writer->WriteValue(TEXT("avatarItemId"), Look.AvatarItemId);
				if (!Look.SkinToneId.IsEmpty())
				{
					Writer->WriteValue(TEXT("skinToneId"), Look.SkinToneId);
				}
				if (!Look.EyeColorId.IsEmpty())
				{
					Writer->WriteValue(TEXT("eyeColorId"), Look.EyeColorId);
				}
				if (Look.HeightPct != 0)
				{
					Writer->WriteValue(TEXT("heightPct"), Look.HeightPct);
				}
				Writer->WriteObjectEnd();
				if (Look.Wearing.Num() > 0)
				{
					Writer->WriteArrayStart(TEXT("wearing"));
					for (const FMantleCrowdWorn& Worn : Look.Wearing)
					{
						Writer->WriteObjectStart();
						Writer->WriteValue(TEXT("slot"), Worn.Slot);
						Writer->WriteValue(TEXT("itemId"), Worn.ItemId);
						Writer->WriteObjectEnd();
					}
					Writer->WriteArrayEnd();
				}
				Writer->WriteObjectEnd();
			}
			Writer->WriteArrayEnd();
		}

		const bool bDefaultHeights = Props.MinHeightPct == Defaults.MinHeightPct && Props.MaxHeightPct == Defaults.MaxHeightPct;
		if (!bDefaultHeights || Props.SkinTones.Num() > 0)
		{
			Writer->WriteObjectStart(TEXT("variation"));
			if (!bDefaultHeights)
			{
				Writer->WriteArrayStart(TEXT("heightPct"));
				Writer->WriteValue(Props.MinHeightPct);
				Writer->WriteValue(Props.MaxHeightPct);
				Writer->WriteArrayEnd();
			}
			if (Props.SkinTones.Num() > 0)
			{
				Writer->WriteArrayStart(TEXT("skinTones"));
				for (const FString& Tone : Props.SkinTones)
				{
					Writer->WriteValue(Tone);
				}
				Writer->WriteArrayEnd();
			}
			Writer->WriteObjectEnd();
		}

		WriteCrowdRangePair(Writer, TEXT("walkSpeed"), Props.MinWalkSpeed, Props.MaxWalkSpeed, Defaults.MinWalkSpeed, Defaults.MaxWalkSpeed);
		if (Props.IdleChance != Defaults.IdleChance)
		{
			Writer->WriteValue(TEXT("idleChance"), Props.IdleChance);
		}
		// #513: only when there are any, so a spawner without them still reaches clients from before the key.
		if (Props.PointsOfInterest.Num() > 0)
		{
			const FMantleCrowdPointOfInterest DefaultPoint;
			Writer->WriteArrayStart(TEXT("pointsOfInterest"));
			for (const FMantleCrowdPointOfInterest& Point : Props.PointsOfInterest)
			{
				Writer->WriteObjectStart();
				Writer->WriteValue(TEXT("entity"), Point.EntityId);
				if (Point.Chance != DefaultPoint.Chance)
				{
					Writer->WriteValue(TEXT("chance"), Point.Chance);
				}
				WriteCrowdRangePair(Writer, TEXT("lingerSeconds"), Point.MinLingerSeconds, Point.MaxLingerSeconds,
					DefaultPoint.MinLingerSeconds, DefaultPoint.MaxLingerSeconds);
				if (Point.Radius != DefaultPoint.Radius)
				{
					Writer->WriteValue(TEXT("radius"), Point.Radius);
				}
				Writer->WriteObjectEnd();
			}
			Writer->WriteArrayEnd();
		}
		if (Props.bEnabled != Defaults.bEnabled)
		{
			Writer->WriteValue(TEXT("enabled"), Props.bEnabled);
		}
	}

	const TCHAR* CollisionName(EMantleCollision Collision)
	{
		switch (Collision)
		{
		case EMantleCollision::None:    return TEXT("none");
		case EMantleCollision::Overlap: return TEXT("overlap");
		default:                        return TEXT("blocking");
		}
	}

	const TCHAR* AssetKindName(EMantleAssetKind Kind)
	{
		switch (Kind)
		{
		case EMantleAssetKind::Material: return TEXT("Material");
		case EMantleAssetKind::Texture:  return TEXT("Texture");
		case EMantleAssetKind::Sound:    return TEXT("Sound");
		case EMantleAssetKind::SkeletalMesh: return TEXT("SkeletalMesh");
		case EMantleAssetKind::AnimClip: return TEXT("AnimClip");
		case EMantleAssetKind::Heightfield: return TEXT("Heightfield");
		case EMantleAssetKind::Vehicle:  return TEXT("Vehicle");
		case EMantleAssetKind::Weapon:   return TEXT("Weapon");
		default:                         return TEXT("StaticMesh");
		}
	}

	/** The lowercase hex MD5 of Text's UTF-8 bytes. */
	FString ExporterMd5Hex(const FString& Text)
	{
		FMD5 Md5;
		const FTCHARToUTF8 Utf8(*Text);
		Md5.Update(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		uint8 Digest[16];
		Md5.Final(Digest);

		FString Hex;
		Hex.Reserve(32);
		for (const uint8 Byte : Digest)
		{
			Hex += FString::Printf(TEXT("%02x"), Byte);
		}
		return Hex;
	}

	/**
	 * A provisional content address for a local asset.
	 *
	 * The canonical hash is assigned by the backend after cooking, so this is metadata that
	 * will be replaced. Strictness here would block exports for no benefit — the strictness
	 * that matters applies to authored content, not to fields we recompute server-side.
	 */
	FString ProvisionalHash(const FSoftObjectPath& Path, int64& OutBytes)
	{
		OutBytes = 0;

		FString Filename;
		const FString PackageName = Path.GetLongPackageName();
		if (FPackageName::TryConvertLongPackageNameToFilename(
				PackageName, Filename, FPackageName::GetAssetPackageExtension()))
		{
			const FMD5Hash Hash = FMD5Hash::HashFile(*Filename);
			if (Hash.IsValid())
			{
				OutBytes = IFileManager::Get().FileSize(*Filename);
				return TEXT("md5:") + LexToString(Hash).ToLower();
			}
		}

		// Fall back to hashing the path itself so the field is at least deterministic.
		return TEXT("md5:") + ExporterMd5Hex(PackageName);
	}

	FString ProvisionalScriptSourceId(const FString& Source)
	{
		return TEXT("src_") + ExporterMd5Hex(Source);
	}

	/** A material's id: its library id, else the id of its upload. */
	FString ExporterMaterialId(const FMantleAssetLibrary& Library, TSet<FString>& ReferencedAssets,
		MantlePlaceExporterUploads::FContext& Uploads, const UMaterialInterface* Material)
	{
		const FString LibraryId = Library.FindIdByPath(FSoftObjectPath(Material));
		if (!LibraryId.IsEmpty())
		{
			ReferencedAssets.Add(LibraryId);
			return LibraryId;
		}
		return Uploads.IdFor(FMantlePlaceAssets::MaterialKey(Material), EMantleAssetKind::Material, Material);
	}

	/**
	 * A mesh component's mesh id, material ids by slot, collision and shadow: a StaticMesh actor's
	 * component, or an exported instance source (#467), which is a UStaticMeshComponent too.
	 */
	void ExporterReadMeshComponent(
		const UStaticMeshComponent* Component,
		const FString& Path,
		TSet<FString>& ReferencedAssets,
		MantlePlaceExporterUploads::FContext& Uploads,
		TArray<FMantleParseError>& Errors,
		FMantleStaticMeshProps& Props,
		const UStaticMesh* MeshOverride = nullptr)
	{
		const FMantleAssetLibrary& Library = FMantleAssetLibrary::Get();
		auto MaterialIdFor = [&Library, &ReferencedAssets, &Uploads](const UMaterialInterface* Material)
		{
			return ExporterMaterialId(Library, ReferencedAssets, Uploads, Material);
		};

		// #628: a negative scale publishes as the mirrored copy of the mesh (slot indices kept, so the overrides still line up).
		const UStaticMesh* Mesh = MeshOverride ? MeshOverride : Component->GetStaticMesh();
		if (!Mesh)
		{
			Errors.Add(FMantleParseError{ Path + TEXT("/properties/mesh"), TEXT("has no mesh assigned") });
		}
		else if (FMantlePlaceAssets::IsLibraryAsset(Mesh))
		{
			Props.Mesh = Library.FindIdByPath(FSoftObjectPath(Mesh));
			ReferencedAssets.Add(Props.Mesh);

			// Only explicit overrides are published. A material inherited from the mesh asset
			// is already implied by the mesh id, and treating it as content would make a
			// plain library cube unpublishable — the most basic action a creator can take.
			// Materials are applied by slot, so an unset slot is kept as an empty entry (written
			// as null) rather than dropped: dropping it moved an override on slot 2 onto slot 0
			// (#262). Trailing unset slots are trimmed. An override that is not a library
			// material is uploaded (F5 #339).
			for (int32 i = 0; i < Component->OverrideMaterials.Num(); ++i)
			{
				const UMaterialInterface* Material = Component->OverrideMaterials[i];
				Props.Materials.Add(Material ? MaterialIdFor(Material) : FString());
			}
			while (Props.Materials.Num() > 0 && Props.Materials.Last().IsEmpty())
			{
				Props.Materials.Pop();
			}
		}
		else
		{
			// A mesh from the creator's project (a Fab pack, say): uploaded, and so is every
			// slot's material. An uploaded mesh carries only its slots' names, so null has
			// nothing to fall back to: each slot gets its effective material (the override,
			// else the mesh's own), in the uploaded mesh's slot order (F1/F2 MRM2).
			TArray<FMantleValidationIssue> MeshIssues;
			FMantlePlaceAssets::CheckMesh(Mesh, FString(), Uploads.Limits, MeshIssues);
			for (const FMantleValidationIssue& Issue : MeshIssues)
			{
				if (Issue.Severity == EMantleIssueSeverity::Error)
				{
					Errors.Add(FMantleParseError{ Path + TEXT("/properties/mesh"), Issue.Problem + TEXT(" ") + Issue.Remedy });
				}
			}
			Props.Mesh = Uploads.IdFor(FMantlePlaceAssets::MeshKey(Mesh), EMantleAssetKind::StaticMesh, Mesh);
			for (const int32 Slot : FMantlePlaceAssets::UploadSlots(Mesh))
			{
				Props.Materials.Add(MaterialIdFor(FMantlePlaceAssets::EffectiveMaterial(Component, Slot)));
			}
		}

		switch (Component->GetCollisionEnabled())
		{
		case ECollisionEnabled::NoCollision: Props.Collision = EMantleCollision::None;    break;
		case ECollisionEnabled::QueryOnly:   Props.Collision = EMantleCollision::Overlap; break;
		default:                             Props.Collision = EMantleCollision::Blocking; break;
		}
		Props.bCastShadow = FMantleEnvironmentExport::CastsShadowInPlace(Component);
		// #596: the surface a creator picked with one of Mantle's surface physical materials. A physics
		// entity's caller clears it again: its physics.material carries the body's surface.
		Props.Surface = FMantlePlaceExporter::ReadSurface(Component);
		// GhzGarage/mantle#630: the component's cull distance, else a size-based default; nothing for a client that doesn't read them.
		FMantleDrawDistance::Apply(FMantleDrawDistance::ReadComponent(Component, Mesh), FMantleTargetFeatures::Current(), Props);
	}

	/**
	 * A Water Body entity's properties (GhzGarage/mantle#997), from the actor's water body: the numbers as FMantleWaterBodyExport::BuildProps
	 * holds them to the place format, and a custom body's mesh by its library or upload id.
	 */
	void ExporterReadWaterBody(
		const AActor* Actor,
		const FString& Path,
		TSet<FString>& ReferencedAssets,
		MantlePlaceExporterUploads::FContext& Uploads,
		TArray<FMantleParseError>& Errors,
		FMantleEntity& Entity)
	{
		FMantleWaterBodyData Body;
		FMantleWaterBodyProps Props;
		FString Why;
		TArray<FString> Notes;
		if (!FMantleWaterBodyExport::ReadBody(Actor, Body) || !FMantleWaterBodyExport::BuildProps(Body, Props, Why, Notes))
		{
			Errors.Add(FMantleParseError{ Path + TEXT("/properties"), FString::Printf(TEXT("'%s' %s"), *Entity.Name, Why.IsEmpty() ? TEXT("is not a water body Mantle can publish") : *Why) });
			Entity.Props.Set<FMantleWaterBodyProps>(FMantleWaterBodyProps());
			return;
		}
		if (Props.BodyType == EMantleWaterBodyType::Custom)
		{
			const UStaticMesh* Mesh = Body.Mesh.Get();
			if (!Mesh)
			{
				Errors.Add(FMantleParseError{ Path + TEXT("/properties/mesh"), TEXT("has no mesh assigned") });
			}
			else if (FMantlePlaceAssets::IsLibraryAsset(Mesh))
			{
				Props.MeshId = FMantleAssetLibrary::Get().FindIdByPath(FSoftObjectPath(Mesh));
				ReferencedAssets.Add(Props.MeshId);
			}
			else
			{
				TArray<FMantleValidationIssue> MeshIssues;
				FMantlePlaceAssets::CheckMesh(Mesh, FString(), Uploads.Limits, MeshIssues);
				for (const FMantleValidationIssue& Issue : MeshIssues)
				{
					if (Issue.Severity == EMantleIssueSeverity::Error)
					{
						Errors.Add(FMantleParseError{ Path + TEXT("/properties/mesh"), Issue.Problem + TEXT(" ") + Issue.Remedy });
					}
				}
				Props.MeshId = Uploads.IdFor(FMantlePlaceAssets::MeshKey(Mesh), EMantleAssetKind::StaticMesh, Mesh);
			}
		}
		Entity.Props.Set<FMantleWaterBodyProps>(MoveTemp(Props));
	}

	/**
	 * One mesh of a Mantle Scatter Volume (#530), named as a StaticMesh names its mesh: a library mesh by its
	 * id with only its material overrides; a project mesh by its upload's id, held to the upload limits, with
	 * every slot's effective material (the override, else the mesh's own), since an uploaded mesh carries
	 * none. A volume made from a place file whose mesh did not load keeps the ids it was given. MeshPath is
	 * the entry's pointer (/entities/N/properties/meshes/M).
	 */
	void ExporterReadScatterMesh(
		const FMantleScatterMeshSettings& Entry,
		const FString& MeshPath,
		TSet<FString>& ReferencedAssets,
		MantlePlaceExporterUploads::FContext& Uploads,
		TArray<FMantleParseError>& Errors,
		FMantleScatterMesh& Out)
	{
		const FMantleAssetLibrary& Library = FMantleAssetLibrary::Get();
		auto MaterialIdFor = [&Library, &ReferencedAssets, &Uploads](const UMaterialInterface* Material)
		{
			return ExporterMaterialId(Library, ReferencedAssets, Uploads, Material);
		};
		auto Override = [&Entry](int32 Slot) -> const UMaterialInterface*
		{
			return Entry.Materials.IsValidIndex(Slot) ? Entry.Materials[Slot].Get() : nullptr;
		};

		Out.Materials.Reset();
		const UStaticMesh* Mesh = Entry.Mesh;
		if (!Mesh)
		{
			// Only a volume made from a place file has ids without a mesh; ReadProps reports an empty entry.
			Out.MeshId = Entry.MeshId;
			Out.Materials = Entry.MaterialIds;
			TArray<FString> Ids = Entry.MaterialIds;
			Ids.Add(Entry.MeshId);
			for (int32 Index = 0; Index < Ids.Num(); ++Index)
			{
				const FString& Id = Ids[Index];
				if (Library.IsKnown(Id))
				{
					ReferencedAssets.Add(Id);
				}
				else if (FMantleVehicleConverter::IsUploadedId(Id))
				{
					Uploads.NamedIds.Add(Id, Index + 1 == Ids.Num() ? EMantleAssetKind::StaticMesh : EMantleAssetKind::Material);
				}
			}
			return;
		}

		if (FMantlePlaceAssets::IsLibraryAsset(Mesh))
		{
			Out.MeshId = Library.FindIdByPath(FSoftObjectPath(Mesh));
			ReferencedAssets.Add(Out.MeshId);
			// Only explicit overrides, by slot, trailing unset slots trimmed: as a StaticMesh writes them (#262).
			for (int32 Slot = 0; Slot < Entry.Materials.Num(); ++Slot)
			{
				const UMaterialInterface* Material = Override(Slot);
				Out.Materials.Add(Material ? MaterialIdFor(Material) : FString());
			}
			while (Out.Materials.Num() > 0 && Out.Materials.Last().IsEmpty())
			{
				Out.Materials.Pop();
			}
			return;
		}

		TArray<FMantleValidationIssue> MeshIssues;
		FMantlePlaceAssets::CheckMesh(Mesh, FString(), Uploads.Limits, MeshIssues);
		for (const FMantleValidationIssue& Issue : MeshIssues)
		{
			if (Issue.Severity == EMantleIssueSeverity::Error)
			{
				Errors.Add(FMantleParseError{ MeshPath + TEXT("/mesh"), Issue.Problem + TEXT(" ") + Issue.Remedy });
			}
		}
		Out.MeshId = Uploads.IdFor(FMantlePlaceAssets::MeshKey(Mesh), EMantleAssetKind::StaticMesh, Mesh);
		for (const int32 Slot : FMantlePlaceAssets::UploadSlots(Mesh))
		{
			const UMaterialInterface* Material = Override(Slot);
			Out.Materials.Add(MaterialIdFor(Material ? Material : FMantlePlaceAssets::MeshSlotMaterial(Mesh, Slot)));
		}
	}

	/**
	 * Lists a sound ReadSoundEmitter accepted for the manifest: a library sound by its id, a
	 * creator's own (#324) with the other uploads, by the same id. An AmbientSound's audio
	 * component, or a prefab's audio part (#468).
	 */
	void ExporterReferenceSound(
		const UAudioComponent* Audio,
		FMantleSoundEmitterProps& Props,
		TSet<FString>& ReferencedAssets,
		MantlePlaceExporterUploads::FContext& Uploads)
	{
		// A Sound Cue uploads as the sound wave it publishes as (#818), the one ReadSound named.
		const USoundBase* Sound = FMantlePlaceAssets::PublishedSound(Audio ? Audio->Sound.Get() : nullptr);
		if (Sound && FMantleAssetLibrary::Get().IsKnown(Props.Sound))
		{
			ReferencedAssets.Add(Props.Sound);
		}
		else if (Sound)
		{
			Props.Sound = Uploads.IdFor(FMantlePlaceAssets::SoundKey(Sound), EMantleAssetKind::Sound, Sound);
		}
	}

	/**
	 * Lists one Script Asset (Mantle Place Settings) that FMantlePlaceAssets::CheckScriptAsset
	 * accepted, as placed content is listed: a library asset by its id, anything else with the
	 * uploads, a mesh with each slot's own material. Returns the id scripts use for it;
	 * bOutProvisional when no upload is recorded for it yet.
	 */
	FString ExporterReferenceScriptAsset(
		const UObject* Object,
		const FString& Path,
		TSet<FString>& ReferencedAssets,
		MantlePlaceExporterUploads::FContext& Uploads,
		TArray<FMantleParseError>& Errors,
		bool& bOutProvisional)
	{
		bOutProvisional = false;
		const FMantleAssetLibrary& Library = FMantleAssetLibrary::Get();
		const FString LibraryId = Library.FindIdByPath(FSoftObjectPath(Object));
		if (!LibraryId.IsEmpty())
		{
			// Published by its permanent id; a library mesh brings its own materials.
			ReferencedAssets.Add(LibraryId);
			return LibraryId;
		}

		FString Key;
		EMantleAssetKind Kind = EMantleAssetKind::StaticMesh;
		if (const UStaticMesh* Mesh = Cast<UStaticMesh>(Object))
		{
			Key = FMantlePlaceAssets::MeshKey(Mesh);
			// A placed mesh has had its limits checked where it is placed; one only listed here is checked here.
			if (!Uploads.KindOf.Contains(Key))
			{
				TArray<FMantleValidationIssue> MeshIssues;
				FMantlePlaceAssets::CheckMesh(Mesh, FString(), Uploads.Limits, MeshIssues);
				for (const FMantleValidationIssue& Issue : MeshIssues)
				{
					if (Issue.Severity == EMantleIssueSeverity::Error)
					{
						Errors.Add(FMantleParseError{ Path, Issue.Problem + TEXT(" ") + Issue.Remedy });
					}
				}
			}
			for (const int32 Slot : FMantlePlaceAssets::UploadSlots(Mesh))
			{
				const UMaterialInterface* Material = FMantlePlaceAssets::MeshSlotMaterial(Mesh, Slot);
				const FString MaterialLibraryId = Library.FindIdByPath(FSoftObjectPath(Material));
				if (!MaterialLibraryId.IsEmpty())
				{
					ReferencedAssets.Add(MaterialLibraryId);
				}
				else
				{
					Uploads.IdFor(FMantlePlaceAssets::MaterialKey(Material), EMantleAssetKind::Material, Material);
				}
			}
		}
		else if (const UMaterialInterface* Material = Cast<UMaterialInterface>(Object))
		{
			Key = FMantlePlaceAssets::MaterialKey(Material);
			Kind = EMantleAssetKind::Material;
		}
		else
		{
			Key = FMantlePlaceAssets::SoundKey(Cast<USoundBase>(Object));
			Kind = EMantleAssetKind::Sound;
		}
		const FString Id = Uploads.IdFor(Key, Kind, Object);
		bOutProvisional = Id.Equals(FMantlePlaceAssets::ProvisionalId(Key), ESearchCase::CaseSensitive);
		return Id;
	}

	/**
	 * A SkeletalMesh actor's properties (#469): its mesh and every slot's effective material uploaded (an uploaded
	 * skeletal mesh carries only its slots' names), the sequence it plays uploaded as a clip sampled over the mesh's
	 * bones, and how it plays. What cannot publish is FMantlePlaceAssets::ReadSkeletalComponent's to say.
	 */
	void ExporterReadSkeletalMesh(
		const USkeletalMeshComponent* Component,
		const FString& Path,
		const FString& Label,
		TSet<FString>& ReferencedAssets,
		MantlePlaceExporterUploads::FContext& Uploads,
		TArray<FMantleParseError>& Errors,
		FMantleSkeletalMeshProps& Props)
	{
		FMantleSkeletalPlayback Playback;
		FString Why;
		FString Fix;
		if (!FMantlePlaceAssets::ReadSkeletalComponent(Component, Playback, Why, Fix))
		{
			Errors.Add(FMantleParseError{ Path + TEXT("/properties"), FString::Printf(TEXT("'%s' %s %s"), *Label, *Why, *Fix) });
			if (!Playback.Mesh)
			{
				return;
			}
		}
		const USkeletalMesh* Mesh = Playback.Mesh;

		TArray<FMantleValidationIssue> MeshIssues;
		FMantlePlaceAssets::CheckSkeletalMesh(Mesh, FString(), Uploads.Limits, MeshIssues);
		for (const FMantleValidationIssue& Issue : MeshIssues)
		{
			if (Issue.Severity == EMantleIssueSeverity::Error)
			{
				Errors.Add(FMantleParseError{ Path + TEXT("/properties/mesh"), Issue.Problem + TEXT(" ") + Issue.Remedy });
			}
		}
		Props.Mesh = Uploads.IdFor(FMantlePlaceAssets::SkeletalMeshKey(Mesh), EMantleAssetKind::SkeletalMesh, Mesh);

		const FMantleAssetLibrary& Library = FMantleAssetLibrary::Get();
		for (const int32 Slot : FMantlePlaceAssets::SkeletalUploadSlots(Mesh))
		{
			const UMaterialInterface* Material = FMantlePlaceAssets::EffectiveSkeletalMaterial(Component, Slot);
			const FString LibraryId = Material ? Library.FindIdByPath(FSoftObjectPath(Material)) : FString();
			if (!LibraryId.IsEmpty())
			{
				ReferencedAssets.Add(LibraryId);
				Props.Materials.Add(LibraryId);
			}
			else
			{
				Props.Materials.Add(Material ? Uploads.IdFor(FMantlePlaceAssets::MaterialKey(Material), EMantleAssetKind::Material, Material) : FString());
			}
		}
		Props.bCastShadow = FMantleEnvironmentExport::CastsShadowInPlace(Component);

		if (Playback.Sequence)
		{
			Props.Clip = Uploads.IdFor(FMantlePlaceAssets::ClipKey(Playback.Sequence, Mesh), EMantleAssetKind::AnimClip, Playback.Sequence);
			Props.bLoop = Playback.bLoop;
			Props.PlayRate = Playback.PlayRate;
			Props.StartOffset = Playback.StartOffset;
		}
	}

	void ExtractProperties(
		const AActor* Actor,
		EMantleEntityType Type,
		const FString& Path,
		FMantleEntity& Entity,
		TSet<FString>& ReferencedAssets,
		MantlePlaceExporterUploads::FContext& Uploads,
		TArray<FMantleParseError>& Errors,
		const UStaticMesh* MeshOverride = nullptr)
	{
		switch (Type)
		{
		case EMantleEntityType::SkeletalMesh:
		{
			FMantleSkeletalMeshProps Props;
			const ASkeletalMeshActor* SkeletalActor = Cast<ASkeletalMeshActor>(Actor);
			ExporterReadSkeletalMesh(SkeletalActor ? SkeletalActor->GetSkeletalMeshComponent() : nullptr, Path, Entity.Name, ReferencedAssets, Uploads, Errors, Props);
			Entity.Props.Set<FMantleSkeletalMeshProps>(MoveTemp(Props));
			break;
		}

		case EMantleEntityType::StaticMesh:
		{
			FMantleStaticMeshProps Props;
			const UStaticMeshComponent* Component = CastChecked<AStaticMeshActor>(Actor)->GetStaticMeshComponent();
			ExporterReadMeshComponent(Component, Path, ReferencedAssets, Uploads, Errors, Props, MeshOverride);
			// Simulate Physics makes it a physics entity (#456), which the runtime treats as dynamic.
			FMantlePhysicsProps Physics;
			if (FMantlePlaceExporter::ReadPhysics(Component, Physics))
			{
				Props.Physics = Physics;
				Props.Surface = EMantleSurface::Default; // #596: physics.material is its surface
				Entity.bDynamic = true;
			}
			Entity.Props.Set<FMantleStaticMeshProps>(Props);
			break;
		}

		case EMantleEntityType::PointLight:
		case EMantleEntityType::SpotLight:
		case EMantleEntityType::RectLight:
		case EMantleEntityType::DirectionalLight:
		case EMantleEntityType::TriggerVolume:
			// Shared with "Check this place", which holds these values to the schema's ranges (#393).
			FMantlePlaceExporter::ReadValueProperties(Actor, Type, Entity);
			break;

		case EMantleEntityType::Decal:
		{
			// The decal's textures upload as they are (#472): each is written by the id of the
			// converted texture its gathered key maps to, or a provisional id before the upload.
			FMantleDecalProps Props;
			UTexture* BaseColor = nullptr;
			UTexture* Normal = nullptr;
			FString Problem;
			if (FMantlePlaceExporter::ReadDecal(Actor, Props, BaseColor, Normal, Problem))
			{
				auto TextureIdFor = [&Uploads](UTexture* Texture, bool bNormal)
				{
					const FString Gathered = FMantlePlaceAssets::DecalTextureKey(Texture, bNormal);
					const FString* Converted = Uploads.Assets ? Uploads.Assets->DecalTextures.Find(Gathered) : nullptr;
					return Uploads.IdFor(Converted ? *Converted : Gathered, EMantleAssetKind::Texture, Texture);
				};
				Props.BaseColor = TextureIdFor(BaseColor, /*bNormal*/ false);
				if (Normal)
				{
					Props.Normal = TextureIdFor(Normal, /*bNormal*/ true);
				}
			}
			else
			{
				Errors.Add(FMantleParseError{
					Path + TEXT("/properties/baseColor"),
					FString::Printf(TEXT("'%s' %s"), *Entity.Name, *Problem) });
			}
			Entity.Props.Set<FMantleDecalProps>(Props);
			break;
		}

		case EMantleEntityType::SoundEmitter:
		{
			FMantleSoundEmitterProps Props;
			FString Problem;
			if (FMantlePlaceExporter::ReadSoundEmitter(Actor, Props, Problem, Uploads.Assets))
			{
				ExporterReferenceSound(CastChecked<AAmbientSound>(Actor)->GetAudioComponent(), Props, ReferencedAssets, Uploads);
			}
			else
			{
				Errors.Add(FMantleParseError{
					Path + TEXT("/properties/sound"),
					FString::Printf(TEXT("'%s' %s"), *Entity.Name, *Problem) });
			}
			Entity.Props.Set<FMantleSoundEmitterProps>(Props);
			break;
		}

		case EMantleEntityType::VehicleSpawn:
		{
			// #435: the spawn's vehicle is the converted vehicle's upload. That is the draft form of the place file: publish
			// stores the upload in the library and rewrites the spawn to its storageItem (#679 B1), and the parser refuses a
			// raw id, so no published place carries one.
			const AMantleVehicleSpawnPoint* Spawn = CastChecked<AMantleVehicleSpawnPoint>(Actor);
			FMantleVehicleSpawnProps Props;
			FString Problem;
			if (!Spawn->ReadProps(Props, Problem))
			{
				Errors.Add(FMantleParseError{
					Path + (Problem.Contains(TEXT("livery")) ? TEXT("/properties/livery") : TEXT("/properties/respawnSeconds")),
					FString::Printf(TEXT("'%s' %s"), *Entity.Name, *Problem) });
			}
			if (!Props.StorageItem.IsEmpty())
			{
				// #614 stage 4: a stored vehicle is not uploaded with the place; the spawn names the storage entry.
				// Whether the entry exists is checked once the whole place is known (the storage pass below).
				if (!Spawn->Vehicle.IsNull() || !Props.Vehicle.IsEmpty())
				{
					Errors.Add(FMantleParseError{
						Path + TEXT("/properties/storageItem"),
						FString::Printf(TEXT("'%s' sets both a Vehicle and a Storage Item; clear one of them"), *Entity.Name) });
				}
				Props.Vehicle.Reset();
			}
			else if (const UMantleVehicleSource* Source = Spawn->Vehicle.LoadSynchronous())
			{
				Props.Vehicle = Uploads.IdFor(FMantleVehicleConverter::VehicleKey(Source), EMantleAssetKind::Vehicle, Source);
			}
			else if (FMantleVehicleConverter::IsUploadedId(Props.Vehicle))
			{
				Uploads.NamedIds.Add(Props.Vehicle, EMantleAssetKind::Vehicle);
			}
			else
			{
				Errors.Add(FMantleParseError{
					Path + TEXT("/properties/vehicle"),
					FString::Printf(TEXT("'%s' has no vehicle. Put one in your level with Tools > Mantle > Build a vehicle, or set its Storage Item to a vehicle from your library (Tools > Mantle > Library)"), *Entity.Name) });
			}
			Entity.Props.Set<FMantleVehicleSpawnProps>(MoveTemp(Props));
			break;
		}

		case EMantleEntityType::WeaponSpawn:
		{
			// #789: the spawn's weapon is the built weapon's upload; a spawn from a place file keeps its id.
			const AMantleWeaponSpawnPoint* Spawn = CastChecked<AMantleWeaponSpawnPoint>(Actor);
			FMantleWeaponSpawnProps Props;
			FString Problem;
			if (!Spawn->ReadProps(Props, Problem))
			{
				Errors.Add(FMantleParseError{ Path + TEXT("/properties/respawnSeconds"), FString::Printf(TEXT("'%s' %s"), *Entity.Name, *Problem) });
			}
			if (!Props.StorageItem.IsEmpty())
			{
				// A stored weapon is not uploaded with the place; the spawn names the storage entry (checked once the place is known).
				if (!Spawn->Weapon.IsEmpty() || !Props.Weapon.IsEmpty())
				{
					Errors.Add(FMantleParseError{
						Path + TEXT("/properties/storageItem"),
						FString::Printf(TEXT("'%s' sets both a Weapon and a Storage Item; clear one of them"), *Entity.Name) });
				}
				Props.Weapon.Reset();
			}
			else if (AMantleWeaponSpawnPoint::IsBuilderName(Spawn->Weapon))
			{
				Props.Weapon = Uploads.IdFor(FMantlePlaceAssets::WeaponKey(Spawn->Weapon), EMantleAssetKind::Weapon, nullptr);
			}
			else if (!Spawn->Weapon.IsEmpty())
			{
				Errors.Add(FMantleParseError{
					Path + TEXT("/properties/weapon"),
					FString::Printf(TEXT("'%s' names the weapon '%s', which is not one the weapon builder made; pick a weapon from its Weapon list"), *Entity.Name, *Spawn->Weapon) });
			}
			else if (FMantleVehicleConverter::IsUploadedId(Props.Weapon))
			{
				Uploads.NamedIds.Add(Props.Weapon, EMantleAssetKind::Weapon);
			}
			else
			{
				Errors.Add(FMantleParseError{
					Path + TEXT("/properties/weapon"),
					FString::Printf(TEXT("'%s' has no weapon; pick one in its Weapon list (make one first with Tools > Mantle > Build a weapon)"), *Entity.Name) });
			}
			Entity.Props.Set<FMantleWeaponSpawnProps>(MoveTemp(Props));
			break;
		}

		case EMantleEntityType::ScriptPoint:
		{
			// #838: an anchor for a script package, by its storage name (or a Packages/ folder the publish stores); whether that
			// name is known is checked once the whole place is (the storage pass below).
			const AMantleScriptPoint* Point = CastChecked<AMantleScriptPoint>(Actor);
			FMantleScriptPointProps Props;
			FString Problem;
			if (!Point->ReadProps(Props, Problem))
			{
				Errors.Add(FMantleParseError{
					Path + (Problem.Contains(TEXT("parameter")) ? TEXT("/properties/parameters") : TEXT("/properties/storageItem")),
					FString::Printf(TEXT("'%s' %s"), *Entity.Name, *Problem) });
			}
			Entity.Props.Set<FMantleScriptPointProps>(MoveTemp(Props));
			break;
		}

		case EMantleEntityType::CrowdSpawner:
		{
			FMantleCrowdSpawnerProps Props;
			FString Problem;
			if (!FMantlePlaceExporter::ReadCrowdSpawner(Actor, Props, Problem))
			{
				Errors.Add(FMantleParseError{
					Path + TEXT("/properties"),
					FString::Printf(TEXT("'%s' %s"), *Entity.Name, *Problem) });
			}
			Entity.Props.Set<FMantleCrowdSpawnerProps>(MoveTemp(Props));
			break;
		}

		case EMantleEntityType::Effect:
		{
			// #460: a built-in archetype and its look; only a sprites effect's texture (#628) uploads, as a decal's does.
			FMantleEffectProps Props;
			FString Problem;
			const AMantleEffect* EffectActor = CastChecked<AMantleEffect>(Actor);
			if (!EffectActor->ReadProps(Props, Problem))
			{
				Errors.Add(FMantleParseError{
					Path + TEXT("/properties"),
					FString::Printf(TEXT("'%s' %s"), *Entity.Name, *Problem) });
			}
			else if (Props.Archetype == EMantleEffectArchetype::Sprites && EffectActor->SpritesTexture != nullptr)
			{
				const FString Gathered = FMantlePlaceAssets::DecalTextureKey(EffectActor->SpritesTexture.Get(), /*bNormal*/ false);
				const FString* Converted = Uploads.Assets ? Uploads.Assets->DecalTextures.Find(Gathered) : nullptr;
				Props.TextureId = Uploads.IdFor(Converted ? *Converted : Gathered, EMantleAssetKind::Texture, EffectActor->SpritesTexture.Get());
			}
			Entity.Props.Set<FMantleEffectProps>(Props);
			break;
		}

		case EMantleEntityType::Constraint:
			// #528: read in Export, where the entities it joins have their ids.
			Entity.Props.Set<FMantleConstraintProps>(FMantleConstraintProps());
			break;

		case EMantleEntityType::Scatter:
		{
			// #530: the numbers as they are, and each mesh by its library or upload id (ExporterReadScatterMesh).
			const AMantleScatterVolume* Volume = CastChecked<AMantleScatterVolume>(Actor);
			FMantleScatterParams Props;
			FString Problem;
			if (!Volume->ReadProps(Props, Problem))
			{
				Errors.Add(FMantleParseError{
					Path + TEXT("/properties"),
					FString::Printf(TEXT("'%s' %s"), *Entity.Name, *Problem) });
			}
			for (int32 Index = 0; Index < Volume->Meshes.Num() && Index < Props.Meshes.Num(); ++Index)
			{
				ExporterReadScatterMesh(Volume->Meshes[Index], FString::Printf(TEXT("%s/properties/meshes/%d"), *Path, Index),
					ReferencedAssets, Uploads, Errors, Props.Meshes[Index]);
			}
			Entity.Props.Set<FMantleScatterParams>(MoveTemp(Props));
			break;
		}

		case EMantleEntityType::ReflectionProbe:
			// #628: the capture's size and look, and its transform at scale 1 with the capture offset folded in.
			FMantleReflectionProbeExport::Extract(Actor, Entity);
			break;

		case EMantleEntityType::Text:
			// GhzGarage/mantle#724: the text component's look; its scale is folded into xScale and yScale, and the transform written at scale 1.
			FMantleTextExport::Extract(FMantleTextExport::ActorComponent(Actor), Entity);
			break;

		case EMantleEntityType::WaterBody:
			// GhzGarage/mantle#997: a Water plugin body, its spline, waves, look and collision.
			ExporterReadWaterBody(Actor, Path, ReferencedAssets, Uploads, Errors, Entity);
			break;

		default:
			Entity.Props.Set<FMantleEmptyProps>(FMantleEmptyProps{});
			break;
		}
	}
}

// Named, not anonymous: MantleEditor builds as a unity build. Defined with ClassifyActor, which needs the prefab readers.
namespace MantlePlaceExporterSkips
{
	/** Whether Actor is a placed particle actor (an Emitter, a Niagara Actor) that FMantleEffectExport::Translate carries at least one emitter of. */
	bool IsTranslatableParticleActor(const AActor* Actor);
}

/**
 * Engine infrastructure rather than authored content — skipped silently.
 *
 * Deliberately not a list of class names. The engine injects managers into every world
 * (buoyancy, gameplay debugger, Chaos debug draw, and more with each enabled plugin), so a
 * hand-written list goes stale. Two properties actually separate infrastructure from
 * content: a class marked NotPlaceable can never be placed by a creator, and an actor with
 * no root component has no transform and so cannot be a place entity.
 */
bool FMantlePlaceExporter::IsEngineInfrastructure(const AActor* Actor)
{
	if (Actor->GetClass()->HasAnyClassFlags(CLASS_NotPlaceable))
	{
		return true;
	}

	if (!Actor->GetRootComponent())
	{
		return true;
	}

	// A Post Process Volume is a brush, so it ends here as infrastructure and is left out of the place. It is
	// content a creator placed on purpose, so DescribeUnpublishedContent names it as a warning and the coverage
	// report lists it as dropped (#628, #45): the place uses Mantle's own post-process.

	// What FMantleEnvironment spawned into a built place (the star dome and moon are Static Mesh
	// Actors): runtime output, never content, even in a world a place was built into.
	if (Actor->ActorHasTag(FName(FMantleEnvironment::EnvironmentActorTag)))
	{
		return true;
	}

	// A Terrain rebuilt from a place file (#466, the importer's): runtime output, not a landscape.
	// DescribeUnpublishedContent names it, so it is not dropped without a word.
	if (Actor->ActorHasTag(FName(FMantleTerrainBuilder::TerrainActorTag)))
	{
		return true;
	}

	// A Physics Constraint Actor (#528) publishes as a Constraint when it is a weld, a hinge or a rope
	// between physics objects this level publishes; any other is left out, and
	// DescribeUnpublishedContent names it with the reason.
	if (Actor->IsA(APhysicsConstraintActor::StaticClass()))
	{
		FMantleConstraintProps Props;
		const AActor* JoinedA = nullptr;
		const AActor* JoinedB = nullptr;
		FString Problem;
		return !ReadConstraint(Actor, Props, JoinedA, JoinedB, Problem);
	}

	// A placed camera has a plain scene-component root, so ClassifyActor would publish it as an
	// empty Group. Mantle drives every player's camera itself: skip it here, and let
	// DescribeUnpublishedContent name it (#449).
	// A placed Plane reflection capture (#628) has no place form, so it is left out, with a Dropped line in the coverage
	// report (FMantleActorDecompose::ReportIgnoredActor) rather than an error. A Box or Sphere one publishes as a
	// ReflectionProbe (FMantleReflectionProbeExport).
	// #818: content Export leaves out with a warning instead of refusing it (DescribeSkippedActor): camera rigs, level
	// sequences, notes, target points, runtime virtual texture volumes, a mesh actor with no mesh, a decal with no texture,
	// a Sound Cue that plays nothing, particles Mantle cannot carry, Level Instances.
	FSkippedActor Skipped;
	if (DescribeSkippedActor(Actor, Skipped))
	{
		return true;
	}
	return Actor->IsA(AInfo::StaticClass())
		|| FMantleActorDecompose::IsUnpublishedReflectionCapture(Actor)
		|| Actor->IsA(ABrush::StaticClass())
		|| Actor->IsA(AController::StaticClass())
		|| Actor->IsA(APawn::StaticClass())
		|| Actor->IsA(ACameraActor::StaticClass())
		|| (Actor->GetClass() == ATextRenderActor::StaticClass() && !FMantleTargetFeatures::Current().Reads(MantleClientFeatures::WorldText));
}

namespace
{
	/** Whether Class is, or derives from, a class of this short name. Matching by name keeps
	 *  MantleEditor free of module dependencies just to recognise a class (PCG's among them). */
	bool IsOrDerivesFromClassNamed(const UClass* Class, const TCHAR* Name)
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
}

bool FMantlePlaceExporter::DescribeUnpublishedContent(const AActor* Actor, FString& OutWhat, FString& OutRemedy)
{
	// Only what a creator placed: the engine spawns some actors of these kinds itself, like
	// every world's DefaultPhysicsVolume, and those are transient.
	if (Actor->HasAnyFlags(RF_Transient) || Actor->IsA(ADefaultPhysicsVolume::StaticClass()))
	{
		return false;
	}
	// A Physics Constraint Actor that is not a weld, a hinge or a rope between published physics
	// objects (#528): left out of the place, with what to change.
	if (Actor->IsA(APhysicsConstraintActor::StaticClass()))
	{
		FMantleConstraintProps Props;
		const AActor* JoinedA = nullptr;
		const AActor* JoinedB = nullptr;
		FString Problem;
		if (ReadConstraint(Actor, Props, JoinedA, JoinedB, Problem))
		{
			return false;
		}
		OutWhat = Problem;
		OutRemedy = TEXT("Until then the place publishes without this constraint.");
		return true;
	}
	const UClass* Class = Actor->GetClass();
	// A landscape publishes as a Terrain (#466), its streaming proxies with it; what of it can't is
	// named by FMantleTerrainExport::CheckLevel.
	if (IsOrDerivesFromClassNamed(Class, TEXT("LandscapeProxy")))
	{
		return false;
	}
	// #818: left out with a warning, not refused. A Level Instance whose actors publish on their own is only a note.
	FSkippedActor Skipped;
	if (DescribeSkippedActor(Actor, Skipped))
	{
		if (!Skipped.bLoss)
		{
			return false;
		}
		OutWhat = Skipped.What;
		OutRemedy = Skipped.Remedy;
		return true;
	}
	if (Actor->ActorHasTag(FName(FMantleTerrainBuilder::TerrainActorTag)))
	{
		OutWhat = TEXT("is a terrain rebuilt from a place file, which Mantle cannot publish back as it is.");
		OutRemedy = TEXT("Publish the level's original landscape instead; this terrain would be dropped.");
		return true;
	}
	// Painted foliage publishes (#467): an InstancedFoliageActor's components are instanced static
	// meshes, which Export writes as InstancedMesh entities like any other instance source.
	// A Post Process Volume (#628, #45): its saturation, bloom, white balance and exposure travel as the place's settings
	// (FMantleEnvironmentExport); the rest is left out, with a warning. The one FMantleEnvironment spawns into a
	// built place is infrastructure, not content.
	if (Actor->IsA(APostProcessVolume::StaticClass()))
	{
		if (Actor->ActorHasTag(FName(FMantleEnvironment::EnvironmentActorTag)))
		{
			return false;
		}
		OutWhat = TEXT("is a Post Process Volume: Mantle carries its saturation, bloom, white balance and exposure, and uses Mantle's own post-process for the rest.");
		OutRemedy = TEXT("Nothing to change: the export reports what it carries and what it leaves out.");
		return true;
	}
	// A volume that only drives editor-time generation (a Procedural Foliage Volume): what it grows, the painted
	// foliage, is exported on its own, so dropping the volume changes nothing. The coverage report lists it as such;
	// the volume message below ("for a trigger, use a Trigger Box") would be the wrong advice (#628).
	if (FMantleCoverageSurvey::IsEditorGeneratorVolume(Actor))
	{
		return false;
	}
	// A PCG volume or actor: its shape is authoring input, its generated instances publish (#467),
	// and whatever of it can't publish is named by DescribeUnpublishedParts (#486). The volume
	// message below ("for a trigger, use a Trigger Box") would be the wrong advice.
	TArray<UActorComponent*> Components;
	Actor->GetComponents(Components);
	if (Components.ContainsByPredicate([](const UActorComponent* Component)
		{ return Component && IsOrDerivesFromClassNamed(Component->GetClass(), TEXT("PCGComponent")); }))
	{
		return false;
	}
	if (Actor->IsA(AVolume::StaticClass()))
	{
		OutWhat = FString::Printf(TEXT("is a %s, which Mantle does not publish."), *Class->GetName());
		OutRemedy = TEXT("For a trigger, use a Trigger Box; for a wall or floor, use a library mesh. The volume would be dropped.");
		return true;
	}
	if (const ABrush* Brush = Cast<ABrush>(Actor))
	{
		// The builder brush is the editor's own tool, present in every level.
		if (FActorEditorUtils::IsABuilderBrush(Brush) || (Brush->GetWorld() && Brush->GetWorld()->GetDefaultBrush() == Brush))
		{
			return false;
		}
		OutWhat = TEXT("is BSP geometry, which Mantle does not publish.");
		OutRemedy = TEXT("Rebuild it from Mantle library meshes; the brush would be dropped.");
		return true;
	}
	if (Actor->IsA(APawn::StaticClass()))
	{
		OutWhat = FString::Printf(TEXT("is a placed %s, which Mantle does not publish."), *Class->GetName());
		OutRemedy = TEXT("Remove it. Players spawn at Player Starts; placed characters and pawns would be dropped.");
		return true;
	}
	if (Actor->IsA(ACameraActor::StaticClass()))
	{
		OutWhat = FString::Printf(TEXT("is a %s, which Mantle does not publish."), *Class->GetName());
		OutRemedy = TEXT("Remove it. Mantle runs every player's camera itself, so the camera would be dropped.");
		return true;
	}
	// A Text Render Actor publishes as a Text entity (GhzGarage/mantle#724). Only for a target client without worldText is it a
	// label with no place form (#628): left out, with a warning, rather than refused. Exactly the engine's class: a Blueprint
	// derived from it may carry logic, and publishes its text component as a part.
	if (Class == ATextRenderActor::StaticClass() && !FMantleTargetFeatures::Current().Reads(MantleClientFeatures::WorldText))
	{
		OutWhat = FString::Printf(TEXT("is a %s, which Mantle leaves out: %s."), *Class->GetName(), FMantleTargetFeatures::NeedsNewerClient);
		OutRemedy = TEXT("Nothing to change: the text would be dropped.");
		return true;
	}
	return false;
}

namespace
{
	/** PCG's tag on the actors its Spawn Actor node creates (PCGHelpers::DefaultPCGActorTag), matched by name. */
	const FName GExporterPCGActorTag(TEXT("PCG Generated Actor"));

	/** A bool UPROPERTY of Object read by name; false, with Out untouched, when there is none. */
	bool ExporterReadBoolProperty(const UObject* Object, const TCHAR* Name, bool& Out)
	{
		const FBoolProperty* Property = FindFProperty<FBoolProperty>(Object->GetClass(), FName(Name));
		if (!Property)
		{
			return false;
		}
		Out = Property->GetPropertyValue_InContainer(Object);
		return true;
	}

	/** An enum UPROPERTY of Object read by name, as its value's name ("GenerateAtRuntime"); false when there is none. */
	bool ExporterReadEnumProperty(const UObject* Object, const TCHAR* Name, FString& Out)
	{
		const FProperty* Property = FindFProperty<FProperty>(Object->GetClass(), FName(Name));
		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			const void* Value = EnumProperty->ContainerPtrToValuePtr<void>(Object);
			Out = EnumProperty->GetEnum()->GetNameStringByValue(EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(Value));
			return true;
		}
		if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property); ByteProperty && ByteProperty->Enum)
		{
			Out = ByteProperty->Enum->GetNameStringByValue(ByteProperty->GetPropertyValue_InContainer(Object));
			return true;
		}
		return false;
	}

	/**
	 * Whether any local PCG component on a partition actor in World has generated. A partitioned
	 * PCG component never sets its own bGenerated; its grid cells' local components do.
	 */
	bool ExporterAnyPartitionGenerated(const UWorld* World)
	{
		if (!World)
		{
			return false;
		}
		for (TActorIterator<AActor> It(const_cast<UWorld*>(World)); It; ++It)
		{
			const AActor* Actor = *It;
			if (!IsValid(Actor) || !IsOrDerivesFromClassNamed(Actor->GetClass(), TEXT("PCGPartitionActor")))
			{
				continue;
			}
			TArray<UActorComponent*> Components;
			Actor->GetComponents(Components);
			for (const UActorComponent* Component : Components)
			{
				bool bGenerated = false;
				if (Component && IsOrDerivesFromClassNamed(Component->GetClass(), TEXT("PCGComponent"))
					&& ExporterReadBoolProperty(Component, TEXT("bGenerated"), bGenerated) && bGenerated)
				{
					return true;
				}
			}
		}
		return false;
	}

	/** The PCG component checks of DescribeUnpublishedParts, for one component of Actor. */
	void ExporterDescribePCGComponent(const AActor* Actor, const UActorComponent* Component, TArray<FMantleUnpublishedPart>& OutParts)
	{
		// A partition actor's local components carry an original component's generation; that
		// original, on the volume or actor the creator placed, is the one checked.
		bool bLocal = false;
		if (IsOrDerivesFromClassNamed(Actor->GetClass(), TEXT("PCGPartitionActor"))
			|| (ExporterReadBoolProperty(Component, TEXT("bIsComponentLocal"), bLocal) && bLocal))
		{
			return;
		}
		bool bActivated = true;
		if (ExporterReadBoolProperty(Component, TEXT("bActivated"), bActivated) && !bActivated)
		{
			return; // switched off: it generates nothing, in the editor or anywhere else
		}

		FString Trigger;
		if (ExporterReadEnumProperty(Component, TEXT("GenerationTrigger"), Trigger) && Trigger == TEXT("GenerateAtRuntime"))
		{
			OutParts.Add(FMantleUnpublishedPart{
				FString::Printf(TEXT("has a PCG component ('%s') set to Generate At Runtime. Players' clients never run PCG graphs, so in Mantle it would generate nothing."),
					*Component->GetName()),
				TEXT("Set its Generation Trigger to Generate On Load or Generate On Demand, click Generate in the editor, then publish. What it generates is published as place data.") });
			return;
		}

		FString Mode;
		if (ExporterReadEnumProperty(Component, TEXT("CurrentEditingMode"), Mode) && Mode == TEXT("Preview"))
		{
			OutParts.Add(FMantleUnpublishedPart{
				FString::Printf(TEXT("has a PCG component ('%s') in Preview editing mode, so what it generates isn't saved with the level."),
					*Component->GetName()),
				TEXT("Set its Editing Mode to Normal, click Generate, save the level, then publish.") });
			return;
		}

		bool bPartitioned = false;
		ExporterReadBoolProperty(Component, TEXT("bIsComponentPartitioned"), bPartitioned);
		bool bGenerated = false;
		const bool bKnown = ExporterReadBoolProperty(Component, TEXT("bGenerated"), bGenerated);
		if (bPartitioned)
		{
			bGenerated = ExporterAnyPartitionGenerated(Actor->GetWorld());
		}
		if (bKnown && !bGenerated)
		{
			OutParts.Add(FMantleUnpublishedPart{
				FString::Printf(TEXT("has a PCG component ('%s') that hasn't been generated, so there is nothing to publish from it."),
					*Component->GetName()),
				TEXT("Click Generate on the PCG component in the editor, then publish. Mantle never generates PCG for you, in the editor or in players' clients.") });
		}
	}
}

void FMantlePlaceExporter::DescribeUnpublishedParts(const AActor* Actor, TArray<FMantleUnpublishedPart>& OutParts)
{
	if (!IsValid(Actor) || Actor->HasAnyFlags(RF_Transient) || IsProvidedByMantle(Actor))
	{
		return;
	}

	TArray<UActorComponent*> Components;
	Actor->GetComponents(Components);
	for (const UActorComponent* Component : Components)
	{
		if (!Component || Component->HasAnyFlags(RF_Transient))
		{
			continue;
		}
		const UClass* Class = Component->GetClass();
		if (IsOrDerivesFromClassNamed(Class, TEXT("PCGComponent")))
		{
			ExporterDescribePCGComponent(Actor, Component, OutParts);
		}
		else if (Component->IsEditorOnly())
		{
			continue;
		}
		else if (IsOrDerivesFromClassNamed(Class, TEXT("DynamicMeshComponent")))
		{
			OutParts.Add(FMantleUnpublishedPart{
				FString::Printf(TEXT("has a dynamic mesh ('%s'), for example PCG or Geometry Script output, that isn't saved as a static mesh asset."),
					*Component->GetName()),
				TEXT("Save it as a Static Mesh asset (PCG's Save Dynamic Mesh To Asset node, or Modeling Mode), place that mesh instead, then publish. Meshes from your project upload with the place.") });
		}
	}
	// Spline meshes (a landscape's roads, PCG's Spawn Spline Mesh) are baked into static meshes (#628)
	// at export, by FMantleSplineMeshBake: nothing of them is unpublishable here.

	// An actor a PCG graph spawned, of a type Mantle can't express. A publishable one (a Static
	// Mesh Actor, a light, only instanced meshes) exports like any other and needs no message.
	EMantleEntityType Type;
	if (Actor->ActorHasTag(GExporterPCGActorTag) && !IsEngineInfrastructure(Actor)
		&& !ClassifyActor(Actor, Type) && !IsInstanceHost(Actor) && OutParts.Num() == 0)
	{
		const UClass* Class = Actor->GetClass();
		const bool bBlueprint = Class->HasAnyClassFlags(CLASS_CompiledFromBlueprint);
		OutParts.Add(FMantleUnpublishedPart{
			FString::Printf(TEXT("was spawned by a PCG graph (Spawn Actor) as a %s%s, which Mantle can't publish: only the actor types Mantle knows travel, and Blueprint logic never does."),
				bBlueprint ? TEXT("Blueprint ") : TEXT(""), *Class->GetName()),
			TEXT("Spawn static meshes with a Static Mesh Spawner instead, or spawn supported actors (Static Mesh Actors, lights). For behaviour, use a place script.") });
	}
}

bool FMantlePlaceExporter::IsProvidedByMantle(const AActor* Actor)
{
	// All four derive AInfo, so IsEngineInfrastructure already declines to publish them; this
	// check exists to name them explicitly rather than let them fall into that silent bucket,
	// so the validator can tell a creator why their sky settings did not survive publishing.
	return Actor->IsA(ASkyAtmosphere::StaticClass())
		|| Actor->IsA(ASkyLight::StaticClass())
		|| Actor->IsA(AVolumetricCloud::StaticClass())
		|| Actor->IsA(AExponentialHeightFog::StaticClass())
		// #63: a level's own sky dome, a Static Mesh Actor placed on its own (a Blueprint's dome is left out as a part, in CollectPrefabParts).
		|| FMantleSkyMesh::IsSkyMeshActor(Actor);
}

// ---- Blueprint prefabs (#468) ---------------------------------------------------------------------

namespace
{
	bool ExporterIsGameContent(const UActorComponent* Component); // with the instanced meshes, below
}

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantlePlaceExporterPrefab
{
	/**
	 * An audio component's SoundEmitter properties: ReadSoundEmitter's reader, for an AmbientSound's
	 * audio component and for a prefab's audio parts (#468) alike.
	 */
	bool ReadSound(const UAudioComponent* Audio, FMantleSoundEmitterProps& OutProps, FString& OutProblem,
		const FMantlePlaceAssetSet* Assets)
	{
		OutProps = FMantleSoundEmitterProps();
		OutProblem.Reset();

		const USoundBase* Placed = Audio ? Audio->Sound.Get() : nullptr;
		if (!Placed)
		{
			OutProblem = TEXT("has no sound assigned.");
			return false;
		}
		// #818: a Sound Cue is a graph players' clients don't run, so it publishes as the first sound wave it plays.
		const USoundBase* Sound = FMantlePlaceAssets::PublishedSound(Placed);
		if (!Sound)
		{
			OutProblem = FString::Printf(TEXT("uses Sound Cue '%s', which plays no sound wave Mantle can publish."), *Placed->GetName());
			return false;
		}

		const FMantleAssetLibrary& Library = FMantleAssetLibrary::Get();
		const FString LibraryId = Library.FindIdByPath(FSoftObjectPath(Sound));
		const FMantleAssetEntry* Entry = LibraryId.IsEmpty() ? nullptr : Library.Find(LibraryId);
		if (Entry && Entry->Kind == EMantleAssetKind::Sound)
		{
			OutProps.Sound = LibraryId;
		}
		else
		{
			// The creator's own sound (#324; Mantle ships none): uploaded with the place as a WAV, and
			// written by the id the upload queue recorded for it, or a provisional one before that
			// (offline export, preview, Check), which the server refuses as missing if it is published.
			FString Why, Fix;
			if (!FMantlePlaceAssets::CheckSoundRule(Sound, FMantlePlaceAssetLimits::FromContract(), Why, Fix))
			{
				OutProblem = FString::Printf(TEXT("uses sound '%s', which %s. %s"), *Sound->GetName(), *Why, *Fix);
				return false;
			}
			const FString Key = FMantlePlaceAssets::SoundKey(Sound);
			const FMantlePlaceUploadedRef* Uploaded = Assets ? Assets->Uploaded.Find(Key) : nullptr;
			OutProps.Sound = Uploaded && !Uploaded->Id.IsEmpty() ? Uploaded->Id : FMantlePlaceAssets::ProvisionalId(Key);
		}

		// UE allows a multiplier above 1; the schema does not, and clamping keeps Check and
		// Publish in agreement rather than failing the place on a loudness setting.
		OutProps.Volume = FMath::Clamp(static_cast<double>(Audio->VolumeMultiplier), 0.0, 1.0);
		// An AmbientSound has no looping switch of its own: the sound asset decides, in the
		// editor as in UE, and the place file carries that decision to the builder.
		OutProps.bLooping = Placed->IsLooping();
		// #459: Auto Activate off is the creator saying "a script starts this one" (UE's own
		// meaning for an AmbientSound), carried to the builder as autoplay false.
		OutProps.bAutoplay = Audio->bAutoActivate;

		// Whatever attenuation applies (the component's override, its attenuation asset, or the
		// sound's own), collapsed to one distance: where it reaches silence. Nothing attenuating
		// it would make it audible everywhere, which a place cannot express; it gets the default.
		if (const FSoundAttenuationSettings* Attenuation = Audio->GetAttenuationSettingsToApply())
		{
			const double Reach = Attenuation->GetMaxDimension();
			if (Attenuation->bAttenuate && Reach > 0.0)
			{
				OutProps.Radius = Reach;
			}
		}
		return true;
	}

	bool IsShownInGame(const USceneComponent* Component)
	{
		return Component->IsVisible() && !Component->bHiddenInGame;
	}

	/**
	 * The entity type Export writes Component as when it is part of a prefab, or false. Only what
	 * the exporter already writes as an actor of its own, read the same way, and only what a player
	 * would see or touch: a mesh component without a mesh, or a light hidden in game, is nothing.
	 */
	bool PartType(const UActorComponent* Component, EMantleEntityType& OutType)
	{
		if (!Component || Component->IsEditorOnly() || Component->HasAnyFlags(RF_Transient))
		{
			return false;
		}
		// Instanced meshes publish on their own (#467), and spline meshes are baked into static meshes on their own (#628).
		if (Component->IsA(UInstancedStaticMeshComponent::StaticClass()) || Component->IsA(USplineMeshComponent::StaticClass()))
		{
			return false;
		}
		if (const UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Component))
		{
			if (Mesh->GetStaticMesh() && IsShownInGame(Mesh))
			{
				OutType = EMantleEntityType::StaticMesh;
				return true;
			}
			return false;
		}
		// #628: an area light, and a skeletal mesh playing one animation (the rule a SkeletalMesh actor is held to).
		if (const URectLightComponent* Rect = Cast<URectLightComponent>(Component))
		{
			if (IsShownInGame(Rect))
			{
				OutType = EMantleEntityType::RectLight;
				return true;
			}
			return false;
		}
		if (const USkeletalMeshComponent* Skeletal = Cast<USkeletalMeshComponent>(Component))
		{
			FMantleSkeletalPlayback Playback;
			FString Why;
			FString Fix;
			if (IsShownInGame(Skeletal) && FMantlePlaceAssets::ReadSkeletalComponent(Skeletal, Playback, Why, Fix))
			{
				OutType = EMantleEntityType::SkeletalMesh;
				return true;
			}
			return false;
		}
		// A spot light is a point light too, so it is tested first.
		if (const USpotLightComponent* Spot = Cast<USpotLightComponent>(Component))
		{
			if (IsShownInGame(Spot))
			{
				OutType = EMantleEntityType::SpotLight;
				return true;
			}
			return false;
		}
		if (const UPointLightComponent* Point = Cast<UPointLightComponent>(Component))
		{
			if (IsShownInGame(Point))
			{
				OutType = EMantleEntityType::PointLight;
				return true;
			}
			return false;
		}
		// GhzGarage/mantle#724: world text, on a target client that reads it.
		if (const UTextRenderComponent* Text = Cast<UTextRenderComponent>(Component))
		{
			if (FMantleTextExport::IsShownText(Text) && FMantleTargetFeatures::Current().Reads(MantleClientFeatures::WorldText))
			{
				OutType = EMantleEntityType::Text;
				return true;
			}
			return false;
		}
		if ((Component->IsA(UBoxComponent::StaticClass()) || Component->IsA(USphereComponent::StaticClass())) && ExporterIsGameContent(Component))
		{
			OutType = EMantleEntityType::TriggerVolume;
			return true;
		}
		if (const UAudioComponent* Audio = Cast<UAudioComponent>(Component))
		{
			// Only a sound Mantle can publish (a library sound, or a creator's own wave the upload
			// rule accepts, #324); any other stays with the dropped-component warning (#449).
			FMantleSoundEmitterProps Props;
			FString Problem;
			if (ReadSound(Audio, Props, Problem, nullptr))
			{
				OutType = EMantleEntityType::SoundEmitter;
				return true;
			}
		}
		return false;
	}

	/** Whether any component of Actor other than Except is a part PartType accepts. */
	bool HasPart(const AActor* Actor, const UActorComponent* Except)
	{
		TArray<UActorComponent*> Components;
		Actor->GetComponents(Components);
		EMantleEntityType Type;
		for (const UActorComponent* Component : Components)
		{
			if (Component != Except && PartType(Component, Type))
			{
				return true;
			}
		}
		return false;
	}

	/**
	 * Whether a static mesh or skeletal mesh actor (any class, a Blueprint's included) has a component beside its own that
	 * is a part, or content a player could see or touch (#628): it then publishes as a Group of parts, not as one entity
	 * that would lose the rest. The actor's own mesh component is one of the parts.
	 */
	bool NeedsGroup(const AActor* Actor)
	{
		// The common case, a placed mesh with its one component, costs nothing.
		if (Actor->GetComponents().Num() <= 1)
		{
			return false;
		}
		const UActorComponent* Main = nullptr;
		if (const AStaticMeshActor* MeshActor = Cast<AStaticMeshActor>(Actor))
		{
			Main = MeshActor->GetStaticMeshComponent();
		}
		else if (const ASkeletalMeshActor* SkeletalActor = Cast<ASkeletalMeshActor>(Actor))
		{
			Main = SkeletalActor->GetSkeletalMeshComponent();
		}
		else
		{
			return false;
		}
		TArray<UActorComponent*> Components;
		Actor->GetComponents(Components);
		EMantleEntityType Type;
		for (const UActorComponent* Component : Components)
		{
			if (!Component || Component == Main || Component->IsA(UInstancedStaticMeshComponent::StaticClass()))
			{
				continue;
			}
			if (PartType(Component, Type) || ExporterIsGameContent(Component))
			{
				return true;
			}
		}
		return false;
	}

	/** One part's properties, read as ExtractProperties reads an actor of the same type. */
	void ExtractPartProperties(
		const FMantlePrefabPart& Part,
		const FString& Path,
		FMantleEntity& Entity,
		TSet<FString>& ReferencedAssets,
		MantlePlaceExporterUploads::FContext& Uploads,
		TArray<FMantleParseError>& Errors)
	{
		switch (Part.Type)
		{
		case EMantleEntityType::StaticMesh:
		{
			FMantleStaticMeshProps Props;
			if (const UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Part.Component))
			{
				ExporterReadMeshComponent(Mesh, Path, ReferencedAssets, Uploads, Errors, Props, Part.MirroredMesh);
			}
			Entity.Props.Set<FMantleStaticMeshProps>(Props);
			break;
		}

		case EMantleEntityType::SkeletalMesh:
		{
			// #628: a skeletal mesh playing one animation, written as a SkeletalMesh actor's is.
			FMantleSkeletalMeshProps Props;
			ExporterReadSkeletalMesh(Cast<USkeletalMeshComponent>(Part.Component), Path, Entity.Name, ReferencedAssets, Uploads, Errors, Props);
			Entity.Props.Set<FMantleSkeletalMeshProps>(MoveTemp(Props));
			break;
		}

		case EMantleEntityType::Text:
			// GhzGarage/mantle#724: a text component's look; its scale is folded into xScale and yScale.
			FMantleTextExport::Extract(Cast<UTextRenderComponent>(Part.Component), Entity);
			break;

		case EMantleEntityType::Effect:
		{
			// #628: one emitter of a particle component, as FMantleEffectExport::Translate described it.
			FMantleEffectProps Props;
			if (!Part.EffectProperties.IsValid() || !FMantleActorDecompose::EffectPropsFromJson(*Part.EffectProperties, Props))
			{
				Errors.Add(FMantleParseError{
					Path + TEXT("/properties"),
					FString::Printf(TEXT("'%s' has an effect Mantle could not read."), *Entity.Name) });
			}
			else if (FMantleEffectExport::IsTexturePath(Props.TextureId))
			{
				// Translate named the sprite texture by object path; it publishes by its uploaded id, as a decal's does.
				UTexture* Texture = FMantleEffectExport::TextureForPath(Props.TextureId);
				const FString Gathered = FMantlePlaceAssets::DecalTextureKey(Texture, /*bNormal*/ false);
				const FString* Converted = Uploads.Assets ? Uploads.Assets->DecalTextures.Find(Gathered) : nullptr;
				Props.TextureId = Texture ? Uploads.IdFor(Converted ? *Converted : Gathered, EMantleAssetKind::Texture, Texture) : FString();
			}
			Entity.Props.Set<FMantleEffectProps>(Props);
			break;
		}

		case EMantleEntityType::SoundEmitter:
		{
			FMantleSoundEmitterProps Props;
			FString Problem;
			const UAudioComponent* Audio = Cast<UAudioComponent>(Part.Component);
			if (ReadSound(Audio, Props, Problem, Uploads.Assets))
			{
				ExporterReferenceSound(Audio, Props, ReferencedAssets, Uploads);
			}
			else
			{
				Errors.Add(FMantleParseError{
					Path + TEXT("/properties/sound"),
					FString::Printf(TEXT("'%s' %s"), *Entity.Name, *Problem) });
			}
			Entity.Props.Set<FMantleSoundEmitterProps>(Props);
			break;
		}

		default:
			if (!FMantlePlaceExporter::ReadComponentValueProperties(Part.Component, Part.Type, Entity))
			{
				Entity.Props.Set<FMantleEmptyProps>(FMantleEmptyProps{});
			}
			break;
		}
	}

	/** One part of a prefab being exported, with the entity id pass 1 gave it. */
	struct FExportedPart
	{
		FMantlePrefabPart Part;
		FString Id;
	};
}

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantlePlaceExporterSkips
{
	/** A class of actor that exists for the editor or a cinematic and shows players nothing. Matched by name, with its derived classes, so MantleEditor needs no module for it. */
	struct FNonVisualClass
	{
		const TCHAR* ClassName;
		const TCHAR* Kind;
		const TCHAR* Category;
	};

	const FNonVisualClass* FindNonVisual(const AActor* Actor)
	{
		static const FNonVisualClass Classes[] = {
			{ TEXT("CameraRig_Rail"), TEXT("a camera rig"), MantleCoverageCategory::OtherActors },
			{ TEXT("CameraRig_Crane"), TEXT("a camera rig"), MantleCoverageCategory::OtherActors },
			{ TEXT("LevelSequenceActor"), TEXT("a level sequence"), MantleCoverageCategory::OtherActors },
			{ TEXT("TemplateSequenceActor"), TEXT("a template sequence"), MantleCoverageCategory::OtherActors },
			{ TEXT("Note"), TEXT("a note"), MantleCoverageCategory::OtherActors },
			{ TEXT("TargetPoint"), TEXT("a target point"), MantleCoverageCategory::OtherActors },
			{ TEXT("NavLinkProxy"), TEXT("a navigation link"), MantleCoverageCategory::OtherActors },
			{ TEXT("LevelBounds"), TEXT("a level bounds marker"), MantleCoverageCategory::OtherActors },
			{ TEXT("LightmassPortal"), TEXT("a Lightmass portal"), MantleCoverageCategory::Lights },
			{ TEXT("RuntimeVirtualTextureVolume"), TEXT("a runtime virtual texture volume"), MantleCoverageCategory::Volumes },
		};
		for (const FNonVisualClass& Entry : Classes)
		{
			if (IsOrDerivesFromClassNamed(Actor->GetClass(), Entry.ClassName))
			{
				return &Entry;
			}
		}
		return nullptr;
	}

	/** A placed particle actor's system component: the root, as an Emitter's and a Niagara Actor's is. */
	const UFXSystemComponent* ParticleRoot(const AActor* Actor)
	{
		const UFXSystemComponent* Fx = Cast<UFXSystemComponent>(Actor->GetRootComponent());
		return Fx && !Fx->IsEditorOnly() && !Fx->HasAnyFlags(RF_Transient) ? Fx : nullptr;
	}

	bool IsTranslatableParticleActor(const AActor* Actor)
	{
		const UFXSystemComponent* Fx = ParticleRoot(Actor);
		if (!Fx)
		{
			return false;
		}
		TArray<TSharedPtr<FJsonObject>> Emitters;
		FMantleCoverage Discarded;
		FMantleEffectExport::Translate(Fx, FString(), Discarded, Emitters);
		return Emitters.Num() > 0;
	}
}

bool FMantlePlaceExporter::DescribeSkippedActor(const AActor* Actor, FSkippedActor& Out)
{
	using namespace MantlePlaceExporterSkips;
	Out = FSkippedActor();
	if (!IsValid(Actor))
	{
		return false;
	}
	const UClass* Class = Actor->GetClass();

	// Editor-only and cinematic actors: nothing a player sees or touches.
	if (const FNonVisualClass* NonVisual = FindNonVisual(Actor))
	{
		Out.What = FString::Printf(TEXT("is %s (%s), which only matters in the editor, so it is left out of the place."), NonVisual->Kind, *Class->GetName());
		Out.Remedy = TEXT("Nothing to change: the rest of the place publishes without it.");
		Out.Category = NonVisual->Category;
		return true;
	}

	// A Level Instance (a Packed Level Actor is not one: it publishes as instanced meshes). Its actors, when it is loaded in
	// the editor, are in the world and publish on their own at their world transforms; only the wrapper is left out.
	if (!IsOrDerivesFromClassNamed(Class, TEXT("PackedLevelActor")))
	{
		if (const ILevelInstanceInterface* Instance = Cast<ILevelInstanceInterface>(const_cast<AActor*>(Actor)))
		{
			const UWorld* World = Actor->GetWorld();
			const ULevelInstanceSubsystem* Subsystem = World ? World->GetSubsystem<ULevelInstanceSubsystem>() : nullptr;
			const ULevel* Level = Subsystem ? Subsystem->GetLevelInstanceLevel(Instance) : nullptr;
			const FString Asset = Instance->GetWorldAsset().GetAssetName();
			Out.Category = MantleCoverageCategory::SubLevels;
			if (Level)
			{
				int32 Actors = 0;
				for (const AActor* Inside : Level->Actors)
				{
					Actors += IsValid(Inside) ? 1 : 0;
				}
				Out.bLoss = false;
				Out.What = FString::Printf(TEXT("is a Level Instance of '%s': its %d actors publish as ordinary items at their places in the level; the Level Instance itself is not kept."), *Asset, Actors);
			}
			else
			{
				Out.What = FString::Printf(TEXT("is a Level Instance of '%s' that isn't loaded in the editor, so what is inside it is left out of the place."), *Asset);
				Out.Remedy = TEXT("Load it (right-click the Level Instance > Load) and publish again.");
			}
			return true;
		}
	}

	// World text (GhzGarage/mantle#724) that nothing on this target can show: an actor rooted on a text component with nothing
	// else to publish (a Blueprint derived from the Text Render Actor; the engine's own class is DescribeUnpublishedContent's),
	// on a client without worldText. Nothing is lost on a client that reads it, where the text publishes.
	if (const USceneComponent* TextRoot = Class != ATextRenderActor::StaticClass() ? Actor->GetRootComponent() : nullptr;
		TextRoot && TextRoot->IsA(UTextRenderComponent::StaticClass()) && !FMantleTargetFeatures::Current().Reads(MantleClientFeatures::WorldText)
		&& !MantlePlaceExporterPrefab::HasPart(Actor, TextRoot))
	{
		Out.What = FString::Printf(TEXT("is world text (%s) that Mantle leaves out: %s."), *Class->GetName(), FMantleTargetFeatures::NeedsNewerClient);
		Out.Remedy = TEXT("Nothing to change: the rest of the place publishes without it.");
		Out.Category = MantleCoverageCategory::Text;
		return true;
	}

	// A Static Mesh Actor with no mesh shows nothing (unless it carries other parts, which make it a Group).
	if (const AStaticMeshActor* MeshActor = Cast<AStaticMeshActor>(Actor))
	{
		const UStaticMeshComponent* Component = MeshActor->GetStaticMeshComponent();
		if (Component && !Component->GetStaticMesh() && !MantlePlaceExporterPrefab::NeedsGroup(Actor))
		{
			Out.What = TEXT("has no mesh assigned, so there is nothing to show and it is left out of the place.");
			Out.Remedy = TEXT("Pick a mesh for it to keep it.");
			Out.Category = MantleCoverageCategory::StaticMeshes;
			return true;
		}
		return false;
	}

	// A decal whose material gives Mantle no texture to draw, or that has no size to show it at.
	if (Actor->IsA(ADecalActor::StaticClass()))
	{
		FMantleDecalProps Props;
		UTexture* BaseColor = nullptr;
		UTexture* Normal = nullptr;
		FString Problem;
		if (!ReadDecal(Actor, Props, BaseColor, Normal, Problem))
		{
			Out.What = Problem + TEXT(" It is left out of the place.");
			Out.Remedy = TEXT("The rest of the place publishes without this decal.");
			Out.Category = MantleCoverageCategory::Decals;
			return true;
		}
		const FVector Scale = PublishedTransform(Actor).GetScale3D();
		if (FMath::IsNearlyZero(Scale.X) || FMath::IsNearlyZero(Scale.Y) || FMath::IsNearlyZero(Scale.Z))
		{
			Out.What = FString::Printf(TEXT("has a scale of (%g, %g, %g), so it covers nothing and is left out of the place."), Scale.X, Scale.Y, Scale.Z);
			Out.Remedy = TEXT("Give it a scale above zero on every axis to keep it.");
			Out.Category = MantleCoverageCategory::Decals;
			return true;
		}
		return false;
	}

	// An Ambient Sound whose Sound Cue plays no sound wave Mantle can publish. (One that plays a wave publishes as that wave.)
	if (const AAmbientSound* Ambient = Cast<AAmbientSound>(Actor))
	{
		const UAudioComponent* Audio = Ambient->GetAudioComponent();
		const USoundBase* Placed = Audio ? Audio->Sound.Get() : nullptr;
		if (Placed && Placed->IsA<USoundCue>() && !FMantlePlaceAssets::PublishedSound(Placed))
		{
			Out.What = FString::Printf(TEXT("uses Sound Cue '%s', which plays no sound wave Mantle can publish, so it is left out of the place."), *Placed->GetName());
			Out.Remedy = TEXT("Assign a sound wave imported from a WAV file to the Ambient Sound to keep it.");
			Out.Category = MantleCoverageCategory::Sounds;
			return true;
		}
		return false;
	}

	// A placed particle actor whose system Mantle cannot carry (every Niagara system today, and a Cascade one with nothing it maps).
	if (const UFXSystemComponent* Fx = ParticleRoot(Actor))
	{
		if (!IsTranslatableParticleActor(Actor))
		{
			const bool bNiagara = IsOrDerivesFromClassNamed(Fx->GetClass(), TEXT("NiagaraComponent"));
			Out.What = FString::Printf(TEXT("is a particle effect (%s) whose particles Mantle can't carry%s, so it is left out of the place."),
				*Class->GetName(), bNiagara ? TEXT(" yet (a Niagara system)") : TEXT(""));
			Out.Remedy = TEXT("Fine if players won't miss it. Otherwise use a Mantle Effect actor.");
			Out.Category = MantleCoverageCategory::Particles;
			return true;
		}
	}
	return false;
}

bool FMantlePlaceExporter::ClassifyActor(const AActor* Actor, EMantleEntityType& OutType)
{
	// GhzGarage/mantle#997: a Water plugin body is a WaterBody entity.
	if (FMantleWaterBodyExport::IsWaterBodyActor(Actor)) { OutType = EMantleEntityType::WaterBody; return true; }
	// #628: a mesh actor made of several components (a Blueprint's mesh, doors, lights, particles) is a Group of parts.
	if (MantlePlaceExporterPrefab::NeedsGroup(Actor)) { OutType = EMantleEntityType::Group;            return true; }
	// #818: a placed particle actor (an Emitter, a Niagara Actor) is a Group of Effect parts, as a Blueprint's particle components are.
	if (MantlePlaceExporterSkips::IsTranslatableParticleActor(Actor)) { OutType = EMantleEntityType::Group; return true; }
	// #628: a Box or Sphere reflection capture, before the root checks (its preview boxes would read as trigger parts).
	if (FMantleReflectionProbeExport::IsProbeActor(Actor)) { OutType = EMantleEntityType::ReflectionProbe; return true; }
	// GhzGarage/mantle#724: a placed Text Render Actor (the engine's own class) is a Text entity where the target reads it.
	if (FMantleTextExport::IsTextActor(Actor) && FMantleTargetFeatures::Current().Reads(MantleClientFeatures::WorldText)) { OutType = EMantleEntityType::Text; return true; }
	if (Actor->IsA(AStaticMeshActor::StaticClass()))  { OutType = EMantleEntityType::StaticMesh;       return true; }
	if (Actor->IsA(APlayerStart::StaticClass()))      { OutType = EMantleEntityType::PlayerStart;      return true; }
	if (Actor->IsA(APointLight::StaticClass()))       { OutType = EMantleEntityType::PointLight;       return true; }
	if (Actor->IsA(ASpotLight::StaticClass()))        { OutType = EMantleEntityType::SpotLight;        return true; }
	if (Actor->IsA(ADirectionalLight::StaticClass())) { OutType = EMantleEntityType::DirectionalLight; return true; }
	if (Actor->IsA(ARectLight::StaticClass()))        { OutType = EMantleEntityType::RectLight;        return true; }
	if (Actor->IsA(ADecalActor::StaticClass()))       { OutType = EMantleEntityType::Decal;            return true; }
	if (Actor->IsA(AAmbientSound::StaticClass()))     { OutType = EMantleEntityType::SoundEmitter;     return true; }
	if (Actor->IsA(ASkeletalMeshActor::StaticClass())) { OutType = EMantleEntityType::SkeletalMesh;    return true; }
	// Before the root-component checks: a spawner's root is a plain scene component, like a group's.
	if (Actor->IsA(AMantleCrowdSpawner::StaticClass())) { OutType = EMantleEntityType::CrowdSpawner; return true; }
	if (Actor->IsA(AMantleVehicleSpawnPoint::StaticClass())) { OutType = EMantleEntityType::VehicleSpawn; return true; }
	if (Actor->IsA(AMantleWeaponSpawnPoint::StaticClass())) { OutType = EMantleEntityType::WeaponSpawn; return true; }
	if (Actor->IsA(AMantleScriptPoint::StaticClass())) { OutType = EMantleEntityType::ScriptPoint; return true; }
	if (Actor->IsA(AMantleEffect::StaticClass())) { OutType = EMantleEntityType::Effect; return true; }
	if (Actor->IsA(AMantleScatterVolume::StaticClass())) { OutType = EMantleEntityType::Scatter; return true; }
	// #528: one that can't publish never gets here (IsEngineInfrastructure, DescribeUnpublishedContent).
	if (Actor->IsA(APhysicsConstraintActor::StaticClass())) { OutType = EMantleEntityType::Constraint; return true; }

	// Trigger volumes and groups are recognised by their root component rather than their
	// class, so both engine trigger actors and plain actors round-trip.
	if (const USceneComponent* Root = Actor->GetRootComponent())
	{
		if (Root->IsA(UBoxComponent::StaticClass()) || Root->IsA(USphereComponent::StaticClass()))
		{
			// A trigger with parts beside it is a prefab (#468): a Group, the trigger one of its parts.
			OutType = MantlePlaceExporterPrefab::HasPart(Actor, Root) ? EMantleEntityType::Group : EMantleEntityType::TriggerVolume;
			return true;
		}
		if (Root->GetClass() == USceneComponent::StaticClass())
		{
			OutType = EMantleEntityType::Group;
			return true;
		}
		// A Blueprint prefab rooted on a mesh, or on anything else, with parts Export can write (#468).
		if (MantlePlaceExporterPrefab::HasPart(Actor, nullptr))
		{
			OutType = EMantleEntityType::Group;
			return true;
		}
	}

	return false;
}

FString FMantlePlaceExporter::EntityIdFor(const AActor* Actor)
{
	if (!Actor)
	{
		return FString();
	}

#if WITH_EDITOR
	// The id comes from the actor's own ActorGuid, not its label, so renaming an actor keeps
	// its id (#267) and nothing extra is written into the creator's level. The engine saves
	// ActorGuid with the level, gives an actor loaded without one a guid derived from its path
	// (stable across loads), and gives a fresh one to a duplicated or copy-pasted actor.
	const FGuid& Guid = Actor->GetActorGuid();
	if (Guid.IsValid())
	{
		return TEXT("ent_") + Guid.ToString(EGuidFormats::Digits).ToLower();
	}

	// An editor actor normally always has a guid (SpawnActor mints one, loading fixes up a
	// missing one). It is invalid only for templates/CDOs, which are never exported, and for
	// actors created without going through SpawnActor or a persistent load. Those fall back to
	// the older label-derived id, which is valid but does not survive a rename.
	const FString Label = Actor->GetActorLabel();
#else
	// ActorGuid is editor-only data; outside the editor only the object name is available.
	const FString Label = Actor->GetName();
#endif
	return TEXT("ent_") + SanitiseId(Label);
}

bool FMantlePlaceExporter::ReadSoundEmitter(const AActor* Actor, FMantleSoundEmitterProps& OutProps, FString& OutProblem,
	const FMantlePlaceAssetSet* Assets)
{
	// The audio component's reader, shared with a prefab's audio components (#468).
	const AAmbientSound* AmbientSound = Cast<AAmbientSound>(Actor);
	return MantlePlaceExporterPrefab::ReadSound(AmbientSound ? AmbientSound->GetAudioComponent() : nullptr, OutProps, OutProblem, Assets);
}

void FMantleCrowdBudget::Add(int32 Count, FString& OutSpawnerProblem, FString& OutNpcProblem)
{
	OutSpawnerProblem.Reset();
	OutNpcProblem.Reset();
	if (++Spawners == FMantlePlaceLimits::MaxCrowdSpawnersPerPlace + 1)
	{
		OutSpawnerProblem = FString::Printf(
			TEXT("is crowd spawner number %d; a place may have at most %d. Delete a crowd spawner, or move its lanes onto another one."),
			Spawners, FMantlePlaceLimits::MaxCrowdSpawnersPerPlace);
	}
	const int32 Before = Npcs;
	Npcs += FMath::Max(Count, 0);
	if (Before <= FMantlePlaceLimits::MaxCrowdNpcsPerPlace && Npcs > FMantlePlaceLimits::MaxCrowdNpcsPerPlace)
	{
		OutNpcProblem = FString::Printf(
			TEXT("brings the place to %d crowd NPCs; a place may have at most %d. Lower this spawner's Count, or another spawner's."),
			Npcs, FMantlePlaceLimits::MaxCrowdNpcsPerPlace);
	}
}

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantlePlaceExporterPhysicsMaterial
{
	/** The words of an asset name, lowercased: split at anything not a letter or digit and where a capital follows a lowercase letter. */
	TArray<FString> NameWords(const FString& Name)
	{
		TArray<FString> Words;
		FString Current;
		for (int32 Index = 0; Index < Name.Len(); ++Index)
		{
			const TCHAR Char = Name[Index];
			if (!FChar::IsAlnum(Char))
			{
				if (!Current.IsEmpty())
				{
					Words.Add(Current.ToLower());
					Current.Reset();
				}
				continue;
			}
			if (FChar::IsUpper(Char) && Index > 0 && FChar::IsLower(Name[Index - 1]) && !Current.IsEmpty())
			{
				Words.Add(Current.ToLower());
				Current.Reset();
			}
			Current.AppendChar(Char);
		}
		if (!Current.IsEmpty())
		{
			Words.Add(Current.ToLower());
		}
		return Words;
	}

	/** The words that name each preset in an asset's name, in EMantlePhysicsMaterial order. */
	TArray<const TCHAR*> PresetWords(EMantlePhysicsMaterial Preset)
	{
		switch (Preset)
		{
		case EMantlePhysicsMaterial::Rubber: return { TEXT("rubber"), TEXT("bouncy"), TEXT("bounce") };
		case EMantlePhysicsMaterial::Ice:    return { TEXT("ice"), TEXT("icy"), TEXT("slippery") };
		case EMantlePhysicsMaterial::Metal:  return { TEXT("metal"), TEXT("metallic"), TEXT("steel"), TEXT("iron"), TEXT("aluminum"), TEXT("aluminium") };
		case EMantlePhysicsMaterial::Wood:   return { TEXT("wood"), TEXT("wooden"), TEXT("timber"), TEXT("plank") };
		case EMantlePhysicsMaterial::Stone:  return { TEXT("stone"), TEXT("rock"), TEXT("concrete"), TEXT("brick"), TEXT("marble") };
		case EMantlePhysicsMaterial::Default:
		default:                             return { TEXT("default") };
		}
	}

	/** How far apart two surfaces are, in friction and restitution, for the match by values. */
	constexpr float MaxValueDistance = 0.1f;
}

bool FMantlePlaceExporter::MatchPhysicsMaterial(const UPhysicalMaterial* Material, EMantlePhysicsMaterial& OutPreset)
{
	using namespace MantlePlaceExporterPhysicsMaterial;
	OutPreset = EMantlePhysicsMaterial::Default;
	if (!Material)
	{
		return true;
	}

	// One of Mantle's own, by path.
	const FString Path = Material->GetPathName();
	for (int32 Index = 0; Index < MantlePhysicsMaterialCount; ++Index)
	{
		const EMantlePhysicsMaterial Preset = static_cast<EMantlePhysicsMaterial>(Index);
		if (Path.Equals(MantlePhysicsMaterialAssetPath(Preset), ESearchCase::CaseSensitive))
		{
			OutPreset = Preset;
			return true;
		}
	}

	// A word of its name names exactly one preset.
	const TArray<FString> Words = NameWords(Material->GetName());
	int32 NamedCount = 0;
	EMantlePhysicsMaterial Named = EMantlePhysicsMaterial::Default;
	for (int32 Index = 0; Index < MantlePhysicsMaterialCount; ++Index)
	{
		const EMantlePhysicsMaterial Preset = static_cast<EMantlePhysicsMaterial>(Index);
		const TArray<const TCHAR*> Keywords = PresetWords(Preset);
		const bool bNamed = Words.ContainsByPredicate([&Keywords](const FString& Word)
		{
			return Keywords.ContainsByPredicate([&Word](const TCHAR* Keyword) { return Word.Equals(Keyword, ESearchCase::CaseSensitive); });
		});
		if (bNamed)
		{
			++NamedCount;
			Named = Preset;
		}
	}
	if (NamedCount == 1)
	{
		OutPreset = Named;
		return true;
	}

	// The preset nearest in friction and restitution, when it is near in both.
	float Nearest = TNumericLimits<float>::Max();
	bool bFound = false;
	for (int32 Index = 0; Index < MantlePhysicsMaterialCount; ++Index)
	{
		const EMantlePhysicsMaterial Preset = static_cast<EMantlePhysicsMaterial>(Index);
		const FMantlePhysicsMaterialPreset& Values = MantlePhysicsMaterialPreset(Preset);
		const float FrictionDelta = FMath::Abs(Material->Friction - Values.Friction);
		const float RestitutionDelta = FMath::Abs(Material->Restitution - Values.Restitution);
		if (FrictionDelta > MaxValueDistance || RestitutionDelta > MaxValueDistance)
		{
			continue;
		}
		const float Distance = FMath::Square(FrictionDelta) + FMath::Square(RestitutionDelta);
		if (Distance < Nearest)
		{
			Nearest = Distance;
			OutPreset = Preset;
			bFound = true;
		}
	}
	if (!bFound)
	{
		OutPreset = EMantlePhysicsMaterial::Default;
	}
	return bFound;
}

bool FMantlePlaceExporter::ReadPhysics(const UStaticMeshComponent* Component, FMantlePhysicsProps& OutPhysics, FString* OutMaterialNote)
{
	if (!Component || !Component->BodyInstance.bSimulatePhysics)
	{
		return false;
	}
	const FBodyInstance& Body = Component->BodyInstance;
	OutPhysics = FMantlePhysicsProps();
	// Starts anchored when its actor carries the MantleAnchored tag (MantlePhysicsAnchoredTag).
	const AActor* Owner = Component->GetOwner();
	OutPhysics.bSimulate = !(Owner && Owner->ActorHasTag(MantlePhysicsAnchoredTag()));
	// A mass only when the creator set Mass Override. Otherwise it stays unset so the game computes it
	// from the collision volume and the physical material's density, and floats or sinks by that density.
	// The editor's CalculateMass is lightened (a 1 m metal crate reads 826 kg), so sending it would
	// make the game float things that should sink.
	if (Body.bOverrideMass)
	{
		const double Mass = static_cast<double>(Body.GetMassOverride());
		if (Mass > 0.0)
		{
			OutPhysics.Mass = FMath::Clamp(Mass, FMantlePlaceLimits::MinPhysicsMass, FMantlePlaceLimits::MaxPhysicsMass);
		}
	}
	OutPhysics.LinearDamping = FMath::Clamp(static_cast<double>(Body.LinearDamping), 0.0, FMantlePlaceLimits::MaxPhysicsDamping);
	OutPhysics.AngularDamping = FMath::Clamp(static_cast<double>(Body.AngularDamping), 0.0, FMantlePlaceLimits::MaxPhysicsDamping);
	OutPhysics.bGravity = Body.bEnableGravity;

	// The surface (#528): the component's Phys Material Override, else the one on the mesh's simple
	// collision, which is what a physics body simulates on.
	const UPhysicalMaterial* Surface = Body.GetPhysMaterialOverride();
	if (!Surface)
	{
		const UStaticMesh* Mesh = Component->GetStaticMesh();
		const UBodySetup* Setup = Mesh ? Mesh->GetBodySetup() : nullptr;
		Surface = Setup ? Setup->PhysMaterial.Get() : nullptr;
	}
	if (!MatchPhysicsMaterial(Surface, OutPhysics.Material) && OutMaterialNote)
	{
		*OutMaterialNote = FString::Printf(
			TEXT("simulates physics on the physical material %s, which matches none of Mantle's presets, so it publishes as default."),
			*Surface->GetName());
	}
	return true;
}

EMantleSurface FMantlePlaceExporter::MatchSurface(const UPhysicalMaterial* Material)
{
	if (!Material)
	{
		return EMantleSurface::Default;
	}
	// The game's own by path, or by name, since a creator project has none of the game's: the surface's own asset
	// name (PM_MantleSurface_Concrete, or a preset's PM_MantleMetal), or the surface's name with or without "PM_" in
	// front (Concrete, PM_Concrete), in any case. Unlike physics.material there is no guess from other words or
	// values, since a surface only picks effects and a wrong guess would show the wrong ones everywhere.
	const FString Path = Material->GetPathName();
	const FString Name = Material->GetName();
	FString Bare = Name;
	Bare.RemoveFromStart(TEXT("PM_"), ESearchCase::IgnoreCase);
	for (int32 Index = 1; Index < MantleSurfaceCount; ++Index)
	{
		const EMantleSurface Surface = static_cast<EMantleSurface>(Index);
		const FString AssetPath = MantleSurfaceMaterialAssetPath(Surface);
		if (Path.Equals(AssetPath, ESearchCase::CaseSensitive) || Name.Equals(FPackageName::ObjectPathToObjectName(AssetPath), ESearchCase::IgnoreCase)
			|| Bare.Equals(MantleSurfaceToString(Surface), ESearchCase::IgnoreCase))
		{
			return Surface;
		}
	}
	return EMantleSurface::Default;
}

EMantleSurface FMantlePlaceExporter::ReadSurface(const UStaticMeshComponent* Component)
{
	if (!Component)
	{
		return EMantleSurface::Default;
	}
	// The component's Phys Material Override wins, as it does for the engine's own traces; else the
	// physical material of its first material, the override or the mesh's own.
	if (const UPhysicalMaterial* Override = Component->BodyInstance.GetPhysMaterialOverride())
	{
		return MatchSurface(Override);
	}
	// Not gated on GetNumMaterials: a static mesh component counts only its mesh's slots there, and a
	// mesh with none (the runtime-built library cube) would hide a slot-0 override. GetMaterial(0) is
	// the override, else the mesh's own, else null.
	const UMaterialInterface* First = Component->GetMaterial(0);
	return First ? MatchSurface(First->GetPhysicalMaterial()) : EMantleSurface::Default;
}

bool FMantlePlaceExporter::ReadConstraint(const AActor* Actor, FMantleConstraintProps& OutProps, const AActor*& OutA, const AActor*& OutB,
	FString& OutProblem)
{
	OutProps = FMantleConstraintProps();
	OutA = nullptr;
	OutB = nullptr;
	OutProblem.Reset();
	const APhysicsConstraintActor* ConstraintActor = Cast<APhysicsConstraintActor>(Actor);
	const UPhysicsConstraintComponent* Component = ConstraintActor ? ConstraintActor->GetConstraintComp() : nullptr;
	if (!Component)
	{
		OutProblem = TEXT("is not a Physics Constraint Actor.");
		return false;
	}
	const AActor* A = Component->ConstraintActor1;
	const AActor* B = Component->ConstraintActor2;
	if (!A)
	{
		OutProblem = B
			? TEXT("joins only through Constraint Actor 2. Set the physics object it holds as Constraint Actor 1, and leave Constraint Actor 2 empty to join it to the world.")
			: TEXT("joins nothing. Set its Constraint Actor 1 to the physics object it holds (and Constraint Actor 2 to another, or leave it empty for the world).");
		return false;
	}
	if (A == B)
	{
		OutProblem = TEXT("joins an object to itself. Set Constraint Actor 2 to another physics object, or leave it empty for the world.");
		return false;
	}
	// Each end is a static mesh actor this level publishes with Simulate Physics on: the joint is
	// between two whole physics entities, never a component of one.
	struct FConstraintEnd
	{
		const AActor* EndActor;
		FName ComponentName;
	};
	const FConstraintEnd Ends[] = { { A, Component->ComponentName1.ComponentName }, { B, Component->ComponentName2.ComponentName } };
	for (const FConstraintEnd& End : Ends)
	{
		if (!End.EndActor)
		{
			continue;
		}
		const AStaticMeshActor* MeshActor = Cast<AStaticMeshActor>(End.EndActor);
		FMantlePhysicsProps Physics;
		EMantleEntityType Type;
		if (!MeshActor || !IsExportedActor(End.EndActor) || !ClassifyActor(End.EndActor, Type) || Type != EMantleEntityType::StaticMesh
			|| !ReadPhysics(MeshActor->GetStaticMeshComponent(), Physics))
		{
			OutProblem = FString::Printf(
				TEXT("joins '%s', which does not publish as a physics object. Tick Simulate Physics on it (a Static Mesh Actor), or join another."),
				*End.EndActor->GetActorNameOrLabel());
			return false;
		}
		if (End.ComponentName != NAME_None && End.ComponentName != MeshActor->GetStaticMeshComponent()->GetFName())
		{
			OutProblem = FString::Printf(
				TEXT("joins the component '%s' of '%s'. Mantle joins whole physics objects: clear the Component Name."),
				*End.ComponentName.ToString(), *End.EndActor->GetActorNameOrLabel());
			return false;
		}
	}
	if (!FMantleConstraints::ReadEditorConstraint(Component, A, B, OutProps, OutProblem))
	{
		return false;
	}
	OutA = A;
	OutB = B;
	return true;
}

bool FMantlePlaceExporter::ReadCrowdSpawner(const AActor* Actor, FMantleCrowdSpawnerProps& OutProps, FString& OutProblem)
{
	const AMantleCrowdSpawner* Spawner = Cast<AMantleCrowdSpawner>(Actor);
	if (!Spawner)
	{
		OutProps = FMantleCrowdSpawnerProps();
		OutProblem = TEXT("is not a Mantle Crowd Spawner.");
		return false;
	}
	return Spawner->ReadProps(OutProps, OutProblem);
}

bool FMantlePlaceExporter::ResolveCrowdPoints(const AActor* Actor, const TMap<const AActor*, FString>& IdByActor,
	FMantleCrowdSpawnerProps& InOutProps, FString& OutProblem)
{
	OutProblem.Reset();
	const AMantleCrowdSpawner* Spawner = Cast<AMantleCrowdSpawner>(Actor);
	if (!Spawner)
	{
		return true;
	}
	// ReadProps writes one point per setting, in order.
	const int32 Num = FMath::Min(Spawner->PointsOfInterest.Num(), InOutProps.PointsOfInterest.Num());
	for (int32 i = 0; i < Num; ++i)
	{
		const TSoftObjectPtr<AActor>& Target = Spawner->PointsOfInterest[i].Actor;
		if (Target.IsNull())
		{
			continue; // kept by entity id (a spawner made from a place file)
		}
		const AActor* TargetActor = Target.Get();
		const FString* Id = TargetActor ? IdByActor.Find(TargetActor) : nullptr;
		if (!Id)
		{
			if (OutProblem.IsEmpty())
			{
				OutProblem = FString::Printf(
					TEXT("has PointsOfInterest[%d] on '%s', which is not published with this level. Pick an actor in this level that Mantle publishes."),
					i, TargetActor ? *TargetActor->GetActorNameOrLabel() : *Target.ToSoftObjectPath().ToString());
			}
			continue;
		}
		InOutProps.PointsOfInterest[i].EntityId = *Id;
	}
	return OutProblem.IsEmpty();
}

bool FMantlePlaceExporter::IsMantleDecalMaterial(const UMaterialInterface* Material)
{
	// Up the instance chain to the master: an instance of the game's decal material, however deep, counts.
	for (const UMaterialInterface* Current = Material; Current;)
	{
		const FSoftObjectPath Path(Current);
		if (Path == FSoftObjectPath(FMantleDecalMaterials::ColorPath) || Path == FSoftObjectPath(FMantleDecalMaterials::NormalPath))
		{
			return true;
		}
		const UMaterialInstance* Instance = Cast<UMaterialInstance>(Current);
		Current = Instance ? Instance->Parent.Get() : nullptr;
	}
	return false;
}

bool FMantlePlaceExporter::ReadDecal(const AActor* Actor, FMantleDecalProps& OutProps, UTexture*& OutBaseColor, UTexture*& OutNormal, FString& OutProblem)
{
	OutProps = FMantleDecalProps();
	OutBaseColor = nullptr;
	OutNormal = nullptr;
	OutProblem.Reset();

	const ADecalActor* DecalActor = Cast<ADecalActor>(Actor);
	const UDecalComponent* Decal = DecalActor ? DecalActor->GetDecal() : nullptr;
	if (!Decal)
	{
		OutProblem = TEXT("is not a Decal Actor.");
		return false;
	}
	OutProps.Size = Decal->DecalSize;
	OutProps.SortOrder = Decal->SortOrder;
	OutProps.FadeScreenSize = Decal->FadeScreenSize;

	// What to do, whatever is wrong with the material: a published decal is a texture Mantle draws.
	const TCHAR* const Remedy = TEXT("Give the decal a decal material (Material Domain: Deferred Decal) with a texture parameter named BaseColor: its alpha is the decal's shape. Add a texture parameter named Normal for a normal map and a scalar parameter named Opacity to fade it.");
	const UMaterialInterface* Material = Decal->GetDecalMaterial();
	if (!Material)
	{
		OutProblem = FString::Printf(TEXT("has no decal material. %s"), Remedy);
		return false;
	}
	const UMaterial* Base = Material->GetMaterial();
	if (!Base || Base->MaterialDomain != MD_DeferredDecal)
	{
		OutProblem = FString::Printf(TEXT("uses '%s', which is not a decal material. %s"), *Material->GetName(), Remedy);
		return false;
	}

	UTexture* BaseColor = nullptr;
	Material->GetTextureParameterValue(FHashedMaterialParameterInfo(FName(FMantleDecalMaterials::BaseColorParameter)), BaseColor);
	UTexture* MasterDefault = nullptr;
	if (IsMantleDecalMaterial(Material))
	{
		Base->GetTextureParameterDefaultValue(FHashedMaterialParameterInfo(FName(FMantleDecalMaterials::BaseColorParameter)), MasterDefault);
	}
	if (!BaseColor || BaseColor == MasterDefault)
	{
		// Mantle publishes a decal as its texture, so a material without one (or still on the
		// master's placeholder) has nothing to publish.
		OutProblem = FString::Printf(
			TEXT("uses decal material '%s', which Mantle can't publish: a published decal is a texture, and this material sets no BaseColor texture. %s"),
			*Material->GetName(), Remedy);
		return false;
	}
	OutBaseColor = BaseColor;

	// The normal map, when the material has one set that is not the master's flat placeholder.
	UTexture* Normal = nullptr;
	if (Material->GetTextureParameterValue(FHashedMaterialParameterInfo(FName(FMantleDecalMaterials::NormalParameter)), Normal) && Normal)
	{
		UTexture* FlatDefault = nullptr;
		if (IsMantleDecalMaterial(Material))
		{
			Base->GetTextureParameterDefaultValue(FHashedMaterialParameterInfo(FName(FMantleDecalMaterials::NormalParameter)), FlatDefault);
		}
		OutNormal = Normal == FlatDefault ? nullptr : Normal;
	}

	float Opacity = 1.0f;
	if (Material->GetScalarParameterValue(FHashedMaterialParameterInfo(FName(FMantleDecalMaterials::OpacityParameter)), Opacity))
	{
		OutProps.Opacity = FMath::Clamp(static_cast<double>(Opacity), 0.0, 1.0);
	}
	return true;
}

bool FMantlePlaceExporter::ReadValueProperties(const AActor* Actor, EMantleEntityType Type, FMantleEntity& OutEntity)
{
	if (!Actor)
	{
		return false;
	}

	// The component the values live on: a light actor's light, a trigger's root.
	const USceneComponent* Source = nullptr;
	switch (Type)
	{
	case EMantleEntityType::PointLight:
	{
		const APointLight* LightActor = Cast<APointLight>(Actor);
		Source = LightActor ? LightActor->GetLightComponent() : nullptr;
		break;
	}
	case EMantleEntityType::SpotLight:
	{
		const ASpotLight* LightActor = Cast<ASpotLight>(Actor);
		Source = LightActor ? LightActor->GetLightComponent() : nullptr;
		break;
	}
	case EMantleEntityType::RectLight:
	{
		const ARectLight* LightActor = Cast<ARectLight>(Actor);
		Source = LightActor ? LightActor->GetLightComponent() : nullptr;
		break;
	}
	case EMantleEntityType::DirectionalLight:
	{
		const ADirectionalLight* LightActor = Cast<ADirectionalLight>(Actor);
		Source = LightActor ? LightActor->GetLightComponent() : nullptr;
		break;
	}
	case EMantleEntityType::TriggerVolume:
		Source = Actor->GetRootComponent();
		break;
	default:
		return false;
	}
	return ReadComponentValueProperties(Source, Type, OutEntity);
}

bool FMantlePlaceExporter::ReadComponentValueProperties(const USceneComponent* Component, EMantleEntityType Type, FMantleEntity& OutEntity)
{
	switch (Type)
	{
	case EMantleEntityType::PointLight:
	{
		FMantlePointLightProps Props;
		if (const UPointLightComponent* Light = Cast<UPointLightComponent>(Component))
		{
			// Intensity is a number in IntensityUnits; the two travel together (#385).
			Props.Intensity = Light->Intensity;
			Props.IntensityUnits = ExporterMantleLightUnits(Light->IntensityUnits);
			Props.Color = Light->GetLightColor();
			Props.Radius = Light->AttenuationRadius;
			Props.bCastShadow = Light->CastShadows;
			// #472: temperature and source shape.
			Props.bUseTemperature = Light->bUseTemperature;
			Props.Temperature = Light->Temperature;
			Props.Source.SourceRadius = Light->SourceRadius;
			Props.Source.SoftSourceRadius = Light->SoftSourceRadius;
			Props.Source.SourceLength = Light->SourceLength;
		}
		OutEntity.Props.Set<FMantlePointLightProps>(Props);
		return true;
	}

	case EMantleEntityType::RectLight:
	{
		FMantleRectLightProps Props;
		if (const URectLightComponent* Light = Cast<URectLightComponent>(Component))
		{
			Props.Intensity = Light->Intensity;
			Props.IntensityUnits = ExporterMantleLightUnits(Light->IntensityUnits);
			Props.Color = Light->GetLightColor();
			Props.Radius = Light->AttenuationRadius;
			Props.SourceWidth = Light->SourceWidth;
			Props.SourceHeight = Light->SourceHeight;
			Props.BarnDoorAngle = Light->BarnDoorAngle;
			Props.BarnDoorLength = Light->BarnDoorLength;
			Props.bUseTemperature = Light->bUseTemperature;
			Props.Temperature = Light->Temperature;
			Props.bCastShadow = Light->CastShadows;
		}
		OutEntity.Props.Set<FMantleRectLightProps>(Props);
		return true;
	}

	case EMantleEntityType::SpotLight:
	{
		FMantleSpotLightProps Props;
		if (const USpotLightComponent* Light = Cast<USpotLightComponent>(Component))
		{
			// Intensity is a number in IntensityUnits; the two travel together (#385).
			Props.Intensity = Light->Intensity;
			Props.IntensityUnits = ExporterMantleLightUnits(Light->IntensityUnits);
			Props.Color = Light->GetLightColor();
			Props.Radius = Light->AttenuationRadius;
			Props.InnerCone = Light->InnerConeAngle;
			Props.OuterCone = Light->OuterConeAngle;
			Props.bCastShadow = Light->CastShadows;
			// #472: temperature and source shape.
			Props.bUseTemperature = Light->bUseTemperature;
			Props.Temperature = Light->Temperature;
			Props.Source.SourceRadius = Light->SourceRadius;
			Props.Source.SoftSourceRadius = Light->SoftSourceRadius;
			Props.Source.SourceLength = Light->SourceLength;
		}
		OutEntity.Props.Set<FMantleSpotLightProps>(Props);
		return true;
	}

	case EMantleEntityType::DirectionalLight:
	{
		FMantleDirectionalLightProps Props;
		if (const UDirectionalLightComponent* Light = Cast<UDirectionalLightComponent>(Component))
		{
			Props.Intensity = Light->Intensity;
			Props.Color = Light->GetLightColor();
			Props.bCastShadow = Light->CastShadows;
		}
		OutEntity.Props.Set<FMantleDirectionalLightProps>(Props);
		return true;
	}

	case EMantleEntityType::TriggerVolume:
	{
		FMantleTriggerVolumeProps Props;
		if (const UBoxComponent* Box = Cast<UBoxComponent>(Component))
		{
			Props.Shape = EMantleTriggerShape::Box;
			Props.Extent = Box->GetUnscaledBoxExtent();
		}
		else if (const USphereComponent* Sphere = Cast<USphereComponent>(Component))
		{
			Props.Shape = EMantleTriggerShape::Sphere;
			const double Radius = Sphere->GetUnscaledSphereRadius();
			Props.Extent = FVector(Radius, Radius, Radius);
		}
		OutEntity.Props.Set<FMantleTriggerVolumeProps>(Props);
		return true;
	}

	default:
		return false;
	}
}

bool FMantlePlaceExporter::IsExportedActor(const AActor* Actor)
{
	EMantleEntityType Type;
	return IsValid(Actor) && !IsEngineInfrastructure(Actor) && !IsProvidedByMantle(Actor) && ClassifyActor(Actor, Type);
}

FTransform FMantlePlaceExporter::PublishedTransform(const AActor* Actor)
{
	if (!Actor || !Actor->GetRootComponent())
	{
		return FTransform::Identity;
	}
	// Export parents an entity only to an actor it exports too, and then writes the relative transform.
	const AActor* ParentActor = Actor->GetAttachParentActor();
	if (ParentActor && IsExportedActor(ParentActor))
	{
		return Actor->GetRootComponent()->GetRelativeTransform();
	}
	return Actor->GetActorTransform();
}

// ---- Instanced meshes (#467) --------------------------------------------------------------------

namespace
{
	/** PCG's tag on the debug display components it generates (PCGHelpers::DefaultPCGDebugTag), matched by name. */
	const FName GExporterPCGDebugTag(TEXT("PCG Generated Debug Component"));

	/** Below this an instance's scale is written as zero, which the schema refuses. */
	constexpr double GExporterMinInstanceScale = 0.00005;

	/**
	 * Whether Owner is the level's painted foliage (an InstancedFoliageActor, by class name: MantleEditor does not link the Foliage
	 * module). Thinning takes these instances first (GhzGarage/mantle#818).
	 */
	bool ExporterIsFoliageOwner(const AActor* Owner)
	{
		for (const UClass* Class = Owner ? Owner->GetClass() : nullptr; Class; Class = Class->GetSuperClass())
		{
			if (Class->GetFName() == TEXT("InstancedFoliageActor"))
			{
				return true;
			}
		}
		return false;
	}

	FString ExporterLabelOf(const AActor* Actor)
	{
		return Actor->GetActorLabel();
	}

	/**
	 * An instance with an even number of negative scale axes is a rotation, not a mirror: decomposing
	 * its matrix gives the same instance with a positive scale. An odd number stays mirrored (one
	 * axis negative after decomposition), which a place can't express.
	 */
	FTransform ExporterNormalizeInstance(const FTransform& Instance)
	{
		const FVector Scale = Instance.GetScale3D();
		if (Scale.X >= 0.0 && Scale.Y >= 0.0 && Scale.Z >= 0.0)
		{
			return Instance;
		}
		FTransform Normalized;
		Normalized.SetFromMatrix(Instance.ToMatrixWithScale());
		return Normalized;
	}

	/** Content in game: something a player could see (a visible primitive or light) or touch. */
	bool ExporterIsGameContent(const UActorComponent* Component)
	{
		if (Component->IsEditorOnly() || Component->IsA(UBillboardComponent::StaticClass()) || Component->IsA(UArrowComponent::StaticClass()))
		{
			return false;
		}
		// A spline is authoring data: nothing of it renders or collides in a game.
		if (Component->IsA(USplineComponent::StaticClass()))
		{
			return false;
		}
		// Spline meshes are baked into static meshes on their own (#628) and dynamic meshes are named by
		// DescribeUnpublishedParts (#486), so neither also makes their actor an unpublishable one.
		if (Component->IsA(USplineMeshComponent::StaticClass()) || IsOrDerivesFromClassNamed(Component->GetClass(), TEXT("DynamicMeshComponent")))
		{
			return false;
		}
		if (const UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Component))
		{
			const bool bVisible = Primitive->IsVisible() && !Primitive->bHiddenInGame;
			return bVisible || Primitive->GetCollisionEnabled() != ECollisionEnabled::NoCollision;
		}
		if (const ULightComponentBase* Light = Cast<ULightComponentBase>(Component))
		{
			return Light->IsVisible() && !Light->bHiddenInGame;
		}
		return false;
	}

	/** Spreads the low 21 bits of Value three apart, for a 63-bit Morton code. */
	uint64 ExporterSpreadBits(uint64 Value)
	{
		Value &= 0x1fffff;
		Value = (Value | (Value << 32)) & 0x1f00000000ffffULL;
		Value = (Value | (Value << 16)) & 0x1f0000ff0000ffULL;
		Value = (Value | (Value << 8)) & 0x100f00f00f00f00fULL;
		Value = (Value | (Value << 4)) & 0x10c30c30c30c30c3ULL;
		Value = (Value | (Value << 2)) & 0x1249249249249249ULL;
		return Value;
	}

	/** Location's position along a Morton curve through Bounds, 21 bits an axis. */
	uint64 ExporterMortonCode(const FVector& Location, const FBox& Bounds)
	{
		const FVector Size = Bounds.GetSize();
		auto Quantize = [](double Value, double Min, double Extent) -> uint64
		{
			if (Extent <= UE_SMALL_NUMBER)
			{
				return 0;
			}
			const double Unit = FMath::Clamp((Value - Min) / Extent, 0.0, 1.0);
			return static_cast<uint64>(Unit * 2097151.0);
		};
		return ExporterSpreadBits(Quantize(Location.X, Bounds.Min.X, Size.X))
			| (ExporterSpreadBits(Quantize(Location.Y, Bounds.Min.Y, Size.Y)) << 1)
			| (ExporterSpreadBits(Quantize(Location.Z, Bounds.Min.Z, Size.Z)) << 2);
	}

	/** A number rounded to Decimals places, without trailing zeros, and never "-0". */
	FString ExporterFormatNumber(double Value, int32 Decimals)
	{
		FString Text;
		switch (Decimals)
		{
		case 2:  Text = FString::Printf(TEXT("%.2f"), Value); break;
		case 3:  Text = FString::Printf(TEXT("%.3f"), Value); break;
		default: Text = FString::Printf(TEXT("%.4f"), Value); break;
		}
		if (Text.Contains(TEXT(".")))
		{
			while (Text.EndsWith(TEXT("0")))
			{
				Text.LeftChopInline(1);
			}
			Text.RemoveFromEnd(TEXT("."));
		}
		if (Text == TEXT("-0"))
		{
			Text = TEXT("0");
		}
		return Text;
	}

	/**
	 * One instance as the place file writes it (#467): [x, y, z, pitch, yaw, roll, sx, sy, sz] on
	 * one line, location to 0.01 cm, rotation to 0.001 degrees, scale to 0.0001 and never below it.
	 */
	FString ExporterFormatInstance(const FTransform& Instance)
	{
		const FVector Location = Instance.GetLocation();
		const FRotator Rotation = Instance.GetRotation().Rotator();
		const FVector Scale = Instance.GetScale3D();
		auto ScaleText = [](double Value)
		{
			return ExporterFormatNumber(FMath::Max(Value, 0.0001), 4);
		};
		return FString::Printf(TEXT("[%s,%s,%s,%s,%s,%s,%s,%s,%s]"),
			*ExporterFormatNumber(Location.X, 2), *ExporterFormatNumber(Location.Y, 2), *ExporterFormatNumber(Location.Z, 2),
			*ExporterFormatNumber(Rotation.Pitch, 3), *ExporterFormatNumber(Rotation.Yaw, 3), *ExporterFormatNumber(Rotation.Roll, 3),
			*ScaleText(Scale.X), *ScaleText(Scale.Y), *ScaleText(Scale.Z));
	}
}

bool FMantlePlaceExporter::IsExportedInstanceSource(const UInstancedStaticMeshComponent* Component)
{
	if (!IsValid(Component) || Component->IsEditorOnly() || Component->HasAnyFlags(RF_Transient))
	{
		return false;
	}
	const AActor* Owner = Component->GetOwner();
	if (!IsValid(Owner) || Owner->HasAnyFlags(RF_Transient) || IsProvidedByMantle(Owner))
	{
		return false;
	}
	// A Mantle Scatter Volume's instances are generated from its Scatter entity on every client (#530),
	// never published one by one.
	if (Owner->IsA(AMantleScatterVolume::StaticClass()))
	{
		return false;
	}
	if (Component->ComponentHasTag(GExporterPCGDebugTag) || Component->GetInstanceCount() == 0)
	{
		return false;
	}
	const bool bVisible = Component->IsVisible() && !Component->bHiddenInGame;
	return bVisible || Component->GetCollisionEnabled() != ECollisionEnabled::NoCollision;
}

TArray<UInstancedStaticMeshComponent*> FMantlePlaceExporter::CollectInstanceSources(UWorld* World)
{
	TArray<UInstancedStaticMeshComponent*> Sources;
	if (!World)
	{
		return Sources;
	}
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		const AActor* Actor = *It;
		if (!IsValid(Actor))
		{
			continue;
		}
		TArray<UInstancedStaticMeshComponent*> Components;
		Actor->GetComponents(Components);
		for (UInstancedStaticMeshComponent* Component : Components)
		{
			if (IsExportedInstanceSource(Component))
			{
				Sources.Add(Component);
			}
		}
	}
	return Sources;
}

void FMantlePlaceExporter::CollectPrefabParts(
	const AActor* Actor,
	TArray<FMantlePrefabPart>& OutParts,
	TArray<FMantleUnpublishedPart>* OutUnsupported,
	FMantleCoverage* OutCoverage)
{
	OutParts.Reset();
	if (OutUnsupported)
	{
		OutUnsupported->Reset();
	}
	EMantleEntityType ActorType;
	const USceneComponent* Root = IsValid(Actor) ? Actor->GetRootComponent() : nullptr;
	if (!Root || !ClassifyActor(Actor, ActorType) || ActorType != EMantleEntityType::Group)
	{
		return;
	}

	// Relative to the root's world transform rather than composed from each attach parent's
	// relative one: FTransform composition isn't associative under non-uniform scale, and the
	// builder puts a child at (its transform) * (its group's), so this is what lands it where the
	// editor shows it, whatever sockets and scales lie between.
	const FTransform RootWorld = Root->GetComponentTransform();
	TArray<UActorComponent*> Components;
	Actor->GetComponents(Components);
	for (const UActorComponent* Component : Components)
	{
		EMantleEntityType Type;
		const USceneComponent* Scene = Cast<USceneComponent>(Component);

		// #628: a Cascade or Niagara component is one Effect part per emitter Translate carries, at the component's
		// transform. What it cannot carry is reported by Translate on the coverage it is given.
		const UFXSystemComponent* Fx = Cast<UFXSystemComponent>(Component);
		if (Fx && Scene && !Fx->IsEditorOnly() && !Fx->HasAnyFlags(RF_Transient))
		{
			TArray<TSharedPtr<FJsonObject>> Emitters;
			FMantleCoverage Discarded;
			FMantleEffectExport::Translate(Fx, ExporterLabelOf(Actor) + TEXT("/") + Fx->GetName(), OutCoverage ? *OutCoverage : Discarded, Emitters);
			for (int32 EmitterIndex = 0; EmitterIndex < Emitters.Num(); ++EmitterIndex)
			{
				FMantlePrefabPart& Part = OutParts.AddDefaulted_GetRef();
				Part.Component = Scene;
				Part.Type = EMantleEntityType::Effect;
				Part.Transform = Scene == Root ? FTransform::Identity : Scene->GetComponentTransform().GetRelativeTransform(RootWorld);
				Part.EffectProperties = Emitters[EmitterIndex];
				Part.EffectIndex = EmitterIndex;
			}
			if (Emitters.Num() == 0 && OutUnsupported)
			{
				// Translate has said what it lost; this is Check's note (the coverage survey leaves the Particles category to it).
				OutUnsupported->Add(FMantleUnpublishedPart{
					FString::Printf(TEXT("has a %s component ('%s') whose particles Mantle can't carry, so it is left out; the rest of the actor publishes."),
						*Fx->GetClass()->GetName(), *Fx->GetName()),
					TEXT("Fine if players won't miss it. Otherwise use a Mantle Effect actor."),
					/*bBlocksPublish*/ false, MantleCoverageCategory::Particles });
			}
			continue;
		}

		// GhzGarage/mantle#724: world text is a Text part. Text with nothing to read, or hidden in game, is nothing; on a target
		// client without worldText it is left out and said so (Check's note; the coverage survey lists it as dropped).
		if (const UTextRenderComponent* Text = Cast<UTextRenderComponent>(Component))
		{
			if (Text->IsEditorOnly() || Text->HasAnyFlags(RF_Transient) || !FMantleTextExport::IsShownText(Text))
			{
				continue;
			}
			if (!FMantleTargetFeatures::Current().Reads(MantleClientFeatures::WorldText))
			{
				if (OutUnsupported)
				{
					OutUnsupported->Add(FMantleUnpublishedPart{
						FString::Printf(TEXT("has a text render component ('%s') that Mantle leaves out: %s; the rest of the actor publishes."),
							*Text->GetName(), FMantleTargetFeatures::NeedsNewerClient),
						TEXT("Nothing to change: the text would be dropped."),
						/*bBlocksPublish*/ false, MantleCoverageCategory::Text });
				}
				continue;
			}
		}

		// GhzGarage/mantle#997: the mesh component the Water plugin makes for a body (a lake's surface, a custom body's mesh) is the body's
		// own; the body publishes as a WaterBody entity, so it is not a mesh of its own.
		if (Scene && !Scene->IsEditorOnly() && !Scene->HasAnyFlags(RF_Transient) && FMantleWaterBody::IsBodyMeshComponent(Cast<UStaticMeshComponent>(Scene)))
		{
			continue;
		}

		// #63: a sky dome mesh of a sky Blueprint (BP_Sky_Sphere, a Fab map's own) is left out: Mantle supplies its own sky.
		if (const UStaticMeshComponent* SkyMesh = Scene && MantlePlaceExporterPrefab::PartType(Scene, Type) && Type == EMantleEntityType::StaticMesh
				? Cast<UStaticMeshComponent>(Scene) : nullptr;
			SkyMesh && FMantleSkyMesh::IsSkyMesh(SkyMesh))
		{
			if (OutUnsupported)
			{
				OutUnsupported->Add(FMantleUnpublishedPart{
					FMantleSkyMesh::ComponentDetail(SkyMesh->GetName()),
					TEXT("Nothing to do: Mantle's own sky shows in its place. The rest of the actor publishes."),
					/*bBlocksPublish*/ false, MantleCoverageCategory::Sky });
			}
			continue;
		}

		if (Scene && MantlePlaceExporterPrefab::PartType(Scene, Type))
		{
			FMantlePrefabPart Part;
			Part.Component = Scene;
			Part.Type = Type;
			Part.Transform = Scene == Root ? FTransform::Identity : Scene->GetComponentTransform().GetRelativeTransform(RootWorld);
			// #628: a mirrored mesh publishes as the mirrored copy at the positive scale; one that can't be copied is left out.
			if (const UStaticMeshComponent* MirroredSource = Type == EMantleEntityType::StaticMesh ? Cast<UStaticMeshComponent>(Scene) : nullptr;
				MirroredSource && FMantleActorDecompose::HasNegativeAxis(Part.Transform.GetScale3D()))
			{
				const FMantleMirrorResult Mirror = FMantleActorDecompose::ResolveMirror(MirroredSource->GetStaticMesh(), Part.Transform.GetScale3D());
				if (!Mirror.bOk)
				{
					if (OutUnsupported)
					{
						OutUnsupported->Add(FMantleUnpublishedPart{
							FString::Printf(TEXT("has a static mesh component ('%s') with a scale of %s, and a mirrored copy of its mesh could not be made (%s), so it is left out."),
								*Scene->GetName(), *Part.Transform.GetScale3D().ToString(), *Mirror.Why),
							TEXT("Use a mesh mirrored in your modelling tool, or a positive scale."),
							/*bBlocksPublish*/ false, MantleCoverageCategory::StaticMeshes });
					}
					continue;
				}
				Part.MirroredMesh = Mirror.Mesh;
				Part.Transform.SetScale3D(Mirror.PositiveScale);
			}
			OutParts.Add(MoveTemp(Part));
			continue;
		}

		// A skeletal mesh that plays more than one animation (an Animation Blueprint, a montage...) cannot publish: its reason.
		if (OutUnsupported && Scene && !Scene->IsEditorOnly() && !Scene->HasAnyFlags(RF_Transient) && Scene->IsA(USkeletalMeshComponent::StaticClass())
			&& MantlePlaceExporterPrefab::IsShownInGame(Scene))
		{
			FMantleSkeletalPlayback Playback;
			FString Why;
			FString Fix;
			FMantlePlaceAssets::ReadSkeletalComponent(Cast<USkeletalMeshComponent>(Scene), Playback, Why, Fix);
			OutUnsupported->Add(FMantleUnpublishedPart{
				FString::Printf(TEXT("has a skeletal mesh component ('%s') that %s"), *Scene->GetName(), *Why),
				TEXT("Until then it is left out; the rest of the actor publishes. ") + Fix,
				/*bBlocksPublish*/ false, MantleCoverageCategory::SkeletalMeshes });
			continue;
		}

		// A sound a player would miss: one the upload rule refuses (#324), named with its reason.
		const UAudioComponent* Audio = Cast<UAudioComponent>(Component);
		if (OutUnsupported && Audio && Audio->Sound.Get() && !Audio->IsEditorOnly() && !Audio->HasAnyFlags(RF_Transient))
		{
			FMantleSoundEmitterProps Refused;
			FString Problem;
			MantlePlaceExporterPrefab::ReadSound(Audio, Refused, Problem, nullptr);
			OutUnsupported->Add(FMantleUnpublishedPart{
				FString::Printf(TEXT("has an audio component ('%s') that %s"), *Component->GetName(), *Problem),
				TEXT("Until then it is left out; the rest of the actor publishes."),
				/*bBlocksPublish*/ false, MantleCoverageCategory::Sounds });
			continue;
		}

		// What a player would miss: content in game that isn't a part. Instanced meshes publish on
		// their own (#467), and an empty mesh component shows and blocks nothing.
		if (!OutUnsupported || !Component || Component->HasAnyFlags(RF_Transient)
			|| Component->IsA(UInstancedStaticMeshComponent::StaticClass()) || !ExporterIsGameContent(Component))
		{
			continue;
		}
		const UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Component);
		if (Mesh && !Mesh->GetStaticMesh())
		{
			continue;
		}
		if (Mesh)
		{
			OutUnsupported->Add(FMantleUnpublishedPart{
				FString::Printf(TEXT("has a static mesh component ('%s') that is hidden in game but still collides. Mantle publishes a mesh only as one players can see, so it is left out."),
					*Component->GetName()),
				TEXT("Show it in game, or turn its collision off. For an invisible area that reacts to players, use a Box Collision component instead."),
				/*bBlocksPublish*/ false, MantleCoverageCategory::StaticMeshes });
		}
		else if (Component->IsA(UDirectionalLightComponent::StaticClass()))
		{
			OutUnsupported->Add(FMantleUnpublishedPart{
				FString::Printf(TEXT("has a directional light component ('%s'). Mantle publishes a directional light only as an actor of its own, so it is left out."),
					*Component->GetName()),
				TEXT("Place a Directional Light actor in the level instead."),
				/*bBlocksPublish*/ false, MantleCoverageCategory::Lights });
		}
		else
		{
			OutUnsupported->Add(FMantleUnpublishedPart{
				FString::Printf(TEXT("has a %s component ('%s') that Mantle can't publish, so it is left out; the rest of the actor publishes."),
					*Component->GetClass()->GetName(), *Component->GetName()),
				TEXT("Fine if players won't miss it. Otherwise remove it, or build it from parts Mantle publishes: static meshes, point and spot lights, and box or sphere collision."),
				/*bBlocksPublish*/ false, FMantleCoverageSurvey::ComponentCategory(Component) });
		}
	}

	// By name, so the export's entity order doesn't depend on the order components were created in.
	OutParts.Sort([](const FMantlePrefabPart& A, const FMantlePrefabPart& B)
	{
		const int32 ByName = A.Component->GetName().Compare(B.Component->GetName(), ESearchCase::CaseSensitive);
		return ByName != 0 ? ByName < 0 : A.EffectIndex < B.EffectIndex;
	});
}

FString FMantlePlaceExporter::PrefabPartEntityId(const FString& GroupId, const UActorComponent* Component, int32 EffectIndex)
{
	// The first (or only) part of a component keeps the id it had before components could make several (#628).
	const FString Name = (Component ? Component->GetName() : FString()) + (EffectIndex > 0 ? FString::Printf(TEXT("#%d"), EffectIndex) : FString());
	return TEXT("ent_") + ExporterMd5Hex(GroupId + TEXT("/") + Name);
}

bool FMantlePlaceExporter::IsPrefabDynamic(const AActor* Actor)
{
	if (!IsValid(Actor))
	{
		return false;
	}
	// Logic could move it, even though it isn't published: a place script taking its place needs a
	// dynamic entity to move.
	TArray<FString> LogicWhere;
	if (DescribeBlueprintLogic(Actor, LogicWhere))
	{
		return true;
	}

	const USceneComponent* Root = Actor->GetRootComponent();
	TArray<USceneComponent*> Scenes;
	Actor->GetComponents(Scenes);
	for (const USceneComponent* Scene : Scenes)
	{
		if (!Scene || Scene->IsEditorOnly() || Scene->HasAnyFlags(RF_Transient) || Scene->Mobility != EComponentMobility::Movable)
		{
			continue;
		}
		// A new Blueprint's root: Movable because that is USceneComponent's default, not a choice.
		const bool bAutomaticRoot = Scene == Root && Scene->GetClass() == USceneComponent::StaticClass()
			&& Scene->GetFName() == USceneComponent::GetDefaultSceneRootVariableName();
		// Movable because its parent is: UE refuses a Static child under a Movable parent.
		const USceneComponent* Parent = Scene->GetAttachParent();
		const bool bInherited = Parent && Parent->Mobility == EComponentMobility::Movable;
		if (!bAutomaticRoot && !bInherited)
		{
			return true;
		}
	}
	return false;
}

bool FMantlePlaceExporter::GraphHasWiredNode(const UEdGraph* Graph)
{
	if (!Graph)
	{
		return false;
	}
	for (const UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node || !Node->IsNodeEnabled())
		{
			continue;
		}
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->LinkedTo.Num() > 0)
			{
				return true;
			}
		}
	}
	return false;
}

bool FMantlePlaceExporter::DescribeBlueprintLogic(const AActor* Actor, TArray<FString>& OutWhere)
{
	OutWhere.Reset();
#if WITH_EDITORONLY_DATA
	if (!Actor)
	{
		return false;
	}
	// The actor's own Blueprint and every Blueprint parent: a parent's graphs run too.
	TArray<UBlueprint*> Blueprints;
	UBlueprint::GetBlueprintHierarchyFromClass(Actor->GetClass(), Blueprints);
	for (const UBlueprint* Blueprint : Blueprints)
	{
		if (!Blueprint)
		{
			continue;
		}
		for (const UEdGraph* Graph : Blueprint->UbergraphPages)
		{
			if (GraphHasWiredNode(Graph))
			{
				OutWhere.AddUnique(FString::Printf(TEXT("%s's %s"), *Blueprint->GetName(), *Graph->GetName()));
			}
		}
		for (const UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			if (GraphHasWiredNode(Graph))
			{
				const bool bConstruction = Graph->GetFName() == FName(TEXT("UserConstructionScript"));
				OutWhere.AddUnique(bConstruction
					? FString::Printf(TEXT("%s's construction script"), *Blueprint->GetName())
					: FString::Printf(TEXT("%s's function %s"), *Blueprint->GetName(), *Graph->GetName()));
			}
		}
	}
#endif
	return OutWhere.Num() > 0;
}

bool FMantlePlaceExporter::IsInstanceHost(const AActor* Actor)
{
	if (!IsValid(Actor))
	{
		return false;
	}
	TArray<UActorComponent*> Components;
	Actor->GetComponents(Components);
	bool bHasInstances = false;
	for (const UActorComponent* Component : Components)
	{
		if (const UInstancedStaticMeshComponent* Instanced = Cast<UInstancedStaticMeshComponent>(Component))
		{
			// An instance source is exported on its own; one Export skips (empty, PCG debug output,
			// invisible and not colliding) is nothing a player would miss.
			bHasInstances = bHasInstances || IsExportedInstanceSource(Instanced);
			continue;
		}
		if (const USplineMeshComponent* Spline = Cast<USplineMeshComponent>(Component))
		{
			// Baked into static meshes on their own (#628), like instanced meshes: nothing else to lose.
			bHasInstances = bHasInstances || FMantleSplineMeshBake::IsBakeSource(Spline);
			continue;
		}
		if (ExporterIsGameContent(Component))
		{
			return false;
		}
	}
	return bHasInstances;
}

void FMantlePlaceExporter::CheckInstanceSource(const UInstancedStaticMeshComponent* Component, TArray<FMantleUnpublishedPart>& OutParts)
{
	if (!Component)
	{
		return;
	}
	const UStaticMesh* Mesh = Component->GetStaticMesh();
	if (!Mesh)
	{
		OutParts.Add(FMantleUnpublishedPart{
			FString::Printf(TEXT("has instanced meshes ('%s') with no mesh assigned."), *Component->GetName()),
			TEXT("Pick a mesh for the component, or remove it.") });
		return;
	}

	int32 Mirrored = 0;
	int32 Flat = 0;
	for (int32 InstanceIndex = 0; InstanceIndex < Component->GetInstanceCount(); ++InstanceIndex)
	{
		FTransform Instance;
		if (!Component->GetInstanceTransform(InstanceIndex, Instance, /*bWorldSpace*/ true))
		{
			continue;
		}
		const double Smallest = ExporterNormalizeInstance(Instance).GetScale3D().GetMin();
		if (Smallest < 0.0)
		{
			++Mirrored;
		}
		else if (Smallest < GExporterMinInstanceScale)
		{
			++Flat;
		}
	}
	// GhzGarage/mantle#818: neither refuses the place. A mirrored instance publishes as an instance of a mirrored copy of the
	// mesh (#628), and one scaled to zero is nothing to see, so Export leaves it out and says so in the coverage report.
	if (Mirrored > 0)
	{
		OutParts.Add(FMantleUnpublishedPart{
			FString::Printf(TEXT("has %d mirrored instance(s) of %s (a negative scale on one axis); they publish as a mirrored copy of the mesh."),
				Mirrored, *Mesh->GetName()),
			TEXT("Nothing to do. A mesh that can't be read for the copy is left out, and the coverage report says so."),
			/*bBlocksPublish*/ false });
	}
	if (Flat > 0)
	{
		OutParts.Add(FMantleUnpublishedPart{
			FString::Printf(TEXT("has %d instance(s) of %s scaled to zero; they are left out."),
				Flat, *Mesh->GetName()),
			TEXT("Nothing to do: an instance with no size can't be seen or touched."),
			/*bBlocksPublish*/ false });
	}

	// Overrides are written by slot with null padding (#262), as for a StaticMesh actor. A mesh from
	// the creator's project writes its own slots instead, which FMantlePlaceAssets::CheckMesh limits.
	if (FMantlePlaceAssets::IsLibraryAsset(Mesh))
	{
		int32 WrittenSlots = 0;
		for (int32 Slot = Component->OverrideMaterials.Num() - 1; Slot >= 0; --Slot)
		{
			if (Component->OverrideMaterials[Slot])
			{
				WrittenSlots = Slot + 1;
				break;
			}
		}
		if (WrittenSlots > FMantlePlaceLimits::MaxMaterialSlots)
		{
			OutParts.Add(FMantleUnpublishedPart{
				FString::Printf(TEXT("overrides material slot %d on its instances of %s; Mantle publishes overrides on slots 0 to 15 only."),
					WrittenSlots - 1, *Mesh->GetName()),
				TEXT("Clear the material overrides on slot 16 and above.") });
		}
	}

	if (Component->NumCustomDataFloats > 0)
	{
		OutParts.Add(FMantleUnpublishedPart{
			FString::Printf(TEXT("gives its instances of %s per-instance custom data, which Mantle doesn't publish; a material reading it will see 0."),
				*Mesh->GetName()),
			TEXT("Fine if the material doesn't read it. Otherwise, bake the variation into separate meshes or materials."),
			/*bBlocksPublish*/ false });
	}
}

FString FMantlePlaceExporter::InstancedGroupKey(const FMantleStaticMeshProps& Mesh)
{
	FString Key = Mesh.Mesh;
	for (const FString& Material : Mesh.Materials)
	{
		Key += TEXT("|") + (Material.IsEmpty() ? FString(TEXT("-")) : Material);
	}
	Key += FString::Printf(TEXT("|%d|%d"), static_cast<int32>(Mesh.Collision), Mesh.bCastShadow ? 1 : 0);
	// GhzGarage/mantle#630: only a distance that is set joins the key, so every key (and the entity ids hashed from it) of a
	// mesh without one stays as it was, and meshes with different distances stay in different sets.
	if (Mesh.DrawDistance > 0.0)
	{
		Key += FString::Printf(TEXT("|d%g"), Mesh.DrawDistance);
	}
	// #596: only a surface other than default joins the key, so every key (and the entity ids hashed
	// from it) from before surfaces stays as it was.
	if (Mesh.Surface != EMantleSurface::Default)
	{
		Key += FString::Printf(TEXT("|%s"), MantleSurfaceToString(Mesh.Surface));
	}
	return Key;
}

FString FMantlePlaceExporter::InstancedEntityIdPrefix(const FMantleStaticMeshProps& Mesh)
{
	return FString::Printf(TEXT("ent_ism_%s_"), *ExporterMd5Hex(InstancedGroupKey(Mesh)).Left(24));
}

TArray<FMantleEntity> FMantlePlaceExporter::ChunkInstancedMeshes(const TArray<FMantleInstancedMeshGroup>& Groups, int32 MaxPerEntity)
{
	TArray<FMantleEntity> Entities;
	MaxPerEntity = FMath::Max(MaxPerEntity, 1);

	TArray<const FMantleInstancedMeshGroup*> Ordered;
	for (const FMantleInstancedMeshGroup& Group : Groups)
	{
		if (Group.WorldInstances.Num() > 0)
		{
			Ordered.Add(&Group);
		}
	}
	Ordered.StableSort([](const FMantleInstancedMeshGroup& A, const FMantleInstancedMeshGroup& B)
	{
		return InstancedGroupKey(A.Mesh) < InstancedGroupKey(B.Mesh);
	});

	for (const FMantleInstancedMeshGroup* Group : Ordered)
	{
		const TArray<FTransform>& Instances = Group->WorldInstances;

		FBox Bounds(ForceInit);
		for (const FTransform& Instance : Instances)
		{
			Bounds += Instance.GetLocation();
		}

		// Morton order keeps each entity's instances close together, so an entity's bounds (what
		// the renderer culls first) stay small. Equal codes keep their level order.
		TArray<TPair<uint64, int32>> Order;
		Order.Reserve(Instances.Num());
		for (int32 InstanceIndex = 0; InstanceIndex < Instances.Num(); ++InstanceIndex)
		{
			Order.Emplace(ExporterMortonCode(Instances[InstanceIndex].GetLocation(), Bounds), InstanceIndex);
		}
		Order.StableSort([](const TPair<uint64, int32>& A, const TPair<uint64, int32>& B) { return A.Key < B.Key; });

		const FString IdPrefix = InstancedEntityIdPrefix(Group->Mesh);
		const int32 Chunks = (Instances.Num() + MaxPerEntity - 1) / MaxPerEntity;
		for (int32 Chunk = 0; Chunk < Chunks; ++Chunk)
		{
			const int32 First = Chunk * MaxPerEntity;
			const int32 Count = FMath::Min(MaxPerEntity, Instances.Num() - First);

			FBox ChunkBounds(ForceInit);
			for (int32 Offset = 0; Offset < Count; ++Offset)
			{
				ChunkBounds += Instances[Order[First + Offset].Value].GetLocation();
			}
			const FVector Centre = ChunkBounds.GetCenter();
			const FVector Anchor(FMath::RoundToDouble(Centre.X), FMath::RoundToDouble(Centre.Y), FMath::RoundToDouble(Centre.Z));

			FMantleInstancedMeshProps Props;
			Props.Mesh = Group->Mesh;
			Props.Instances.Reserve(Count);
			for (int32 Offset = 0; Offset < Count; ++Offset)
			{
				const FTransform& Placed = Instances[Order[First + Offset].Value];
				Props.Instances.Emplace(Placed.GetRotation(), Placed.GetLocation() - Anchor, Placed.GetScale3D());
			}

			FMantleEntity Entity;
			Entity.Id = FString::Printf(TEXT("%s%d"), *IdPrefix, Chunk);
			Entity.Type = EMantleEntityType::InstancedMesh;
			Entity.Name = FString::Printf(TEXT("%s (instanced %d/%d)"), *Group->MeshName, Chunk + 1, Chunks)
				.Left(FMantlePlaceLimits::MaxEntityNameLength);
			Entity.Transform = FTransform(Anchor);
			Entity.Props.Set<FMantleInstancedMeshProps>(MoveTemp(Props));
			Entities.Add(MoveTemp(Entity));
		}
	}
	return Entities;
}

FMantleExportResult FMantlePlaceExporter::Export(UWorld* World, const FMantleExportOptions& Options)
{
	FMantleExportResult Result;

	if (!World)
	{
		Result.Errors.Add(FMantleParseError{ TEXT(""), TEXT("no world to export") });
		return Result;
	}

	// #450: actors in unloaded World Partition cells are not in the world; refuse rather than export part of it.
	FMantlePlaceWorldPartition::CheckExport(World, Result.Errors);

	FMantlePlace& Place = Result.Place;
	Place.SchemaVersion = MANTLE_PLACE_SCHEMA_VERSION;
	Place.PlaceId = Options.PlaceId.IsEmpty() ? TEXT("plc_unassigned") : Options.PlaceId;
	Place.Revision.Number = Options.RevisionNumber;
	Place.Revision.Notes = Options.RevisionNotes;
	Place.Revision.State = EMantleRevisionState::Draft;
	Place.Metadata.Name = Options.Name.IsEmpty() ? World->GetName() : Options.Name;
	Place.Metadata.AuthorId = Options.AuthorId.IsEmpty() ? TEXT("usr_unassigned") : Options.AuthorId;
	// The level's Mantle Place Settings actor, if it has one; otherwise the defaults (#266).
	AMantlePlaceSettingsActor::ApplyTo(World, Place.Settings);
	// And its audience (#320). Restricting platforms without naming one would publish as every
	// platform, the opposite of what was asked, so it is refused instead.
	AMantlePlaceSettingsActor::ApplyTo(World, Place.Audience);
	if (const AMantlePlaceSettingsActor* SettingsActor = AMantlePlaceSettingsActor::Find(World);
		SettingsActor && SettingsActor->HasNoPlatformSelected())
	{
		Result.Errors.Add(FMantleParseError{
			TEXT("/audience/platforms"),
			TEXT("Mantle Place Settings limits the place to some platforms but selects none. Select at least one, or turn All Platforms back on.") });
	}

	// Pass 1 — classify and assign ids. Parents cannot be resolved until every actor has one.
	TSet<FString> UsedIds;
	TMap<const AActor*, FString> IdByActor;
	TArray<const AActor*> Exportable;
	TMap<const AActor*, TArray<MantlePlaceExporterPrefab::FExportedPart>> PartsByActor;
	TMap<const AActor*, FMantleMirrorResult> MirrorByActor;
	// #818: decals published at the positive scale of the negative one they have.
	TSet<const AActor*> UnmirroredDecals;
	int32 EffectParts = 0;
	int32 ReflectionProbes = 0;
	int32 Texts = 0;
	int32 WaterBodies = 0;
	int32 WaterSplinePoints = 0;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		const AActor* Actor = *It;
		if (!IsValid(Actor))
		{
			continue;
		}

		// What of it can't publish (#486), infrastructure included: a PCG volume is a brush and a
		// partition actor NotPlaceable, so the skip below would otherwise drop them without a word.
		TArray<FMantleUnpublishedPart> Parts;
		DescribeUnpublishedParts(Actor, Parts);
		for (const FMantleUnpublishedPart& Part : Parts)
		{
			if (Part.bBlocksPublish)
			{
				Result.Errors.Add(FMantleParseError{
					FString::Printf(TEXT("/actors/%s"), *Actor->GetName()),
					FString::Printf(TEXT("'%s' %s %s"), *ExporterLabelOf(Actor), *Part.What, *Part.Remedy) });
			}
		}

		if (IsEngineInfrastructure(Actor) || IsProvidedByMantle(Actor))
		{
			continue;
		}

		EMantleEntityType Type;
		if (!ClassifyActor(Actor, Type))
		{
			// Its instanced meshes are all it holds: they are written below as InstancedMesh
			// entities (#467), and the actor itself has nothing else to lose. Or it was named above
			// with a specific reason, which says more than the generic message would.
			if (IsInstanceHost(Actor) || Parts.Num() > 0)
			{
				continue;
			}
			Result.Errors.Add(FMantleParseError{
				FString::Printf(TEXT("/actors/%s"), *Actor->GetName()),
				FString::Printf(
					TEXT("'%s' is a %s, which Mantle cannot publish. Remove it or replace it with a supported type."),
					*Actor->GetName(),
					*Actor->GetClass()->GetName()) });
			continue;
		}

		// A mirrored mesh (#628, a negative scale) publishes as the mirrored copy at the positive scale; one that can't
		// be copied is left out, and reported rather than refused.
		FMantleMirrorResult Mirror;
		if (Type == EMantleEntityType::StaticMesh)
		{
			Mirror = FMantleActorDecompose::MirrorForActor(Actor);
			if (Mirror.bNeeded && !Mirror.bOk)
			{
				Result.Coverage.Add(MantleCoverageCategory::StaticMeshes, EMantleCoverageOutcome::Dropped, ExporterLabelOf(Actor),
					FString::Printf(TEXT("left out: it has a negative scale and a mirrored copy of its mesh could not be made (%s)."), *Mirror.Why));
				continue;
			}
		}

		// A sprites Effect (#628) on a target client that does not read the archetype is left out, and said so.
		if (const AMantleEffect* SpritesEffect = Type == EMantleEntityType::Effect ? Cast<AMantleEffect>(Actor) : nullptr;
			SpritesEffect && SpritesEffect->Archetype == EMantleEffectArchetype::Sprites && !FMantleTargetFeatures::Current().Reads(MantleClientFeatures::EffectSprites))
		{
			Result.Coverage.Add(MantleCoverageCategory::Particles, EMantleCoverageOutcome::Dropped, ExporterLabelOf(Actor),
				FString::Printf(TEXT("a sprites effect left out: %s"), FMantleTargetFeatures::NeedsNewerClient));
			continue;
		}

		// A reflection probe (#628) left out on a target client that does not read them, hidden, or past the per-place cap: said so
		// there, and its coverage line (kept or approximated) written when it publishes.
		if (Type == EMantleEntityType::ReflectionProbe && !FMantleReflectionProbeExport::AdmitForExport(Actor, ReflectionProbes, Result.Coverage))
		{
			continue;
		}

		// A Water plugin body (GhzGarage/mantle#997) the place format cannot carry, or past the place's limits, is left out and said so.
		if (Type == EMantleEntityType::WaterBody && !FMantleWaterBodyExport::Admit(Actor, ExporterLabelOf(Actor), WaterBodies, WaterSplinePoints, &Result.Coverage))
		{
			continue;
		}

		// World text (GhzGarage/mantle#724): text with nothing to read or hidden in game is left out, said so; text too long, or
		// past the per-place cap, refuses the export; its coverage line (kept or approximated) is written here.
		if (Type == EMantleEntityType::Text)
		{
			const UTextRenderComponent* TextComponent = FMantleTextExport::ActorComponent(Actor);
			if (!FMantleTextExport::IsShownText(TextComponent))
			{
				Result.Coverage.Add(MantleCoverageCategory::Text, EMantleCoverageOutcome::Dropped, ExporterLabelOf(Actor),
					TEXT("a text render actor with no text to read, or hidden in game, is not published."));
				continue;
			}
			FMantleTextExport::Admit(TextComponent, ExporterLabelOf(Actor), FString::Printf(TEXT("/actors/%s"), *Actor->GetName()),
				TextComponent->GetComponentScale(), Texts, &Result.Coverage, Result.Errors);
		}

		const FString ActorId = MakeEntityId(Actor, UsedIds);
		IdByActor.Add(Actor, ActorId);
		Exportable.Add(Actor);
		if (Mirror.bNeeded)
		{
			MirrorByActor.Add(Actor, Mirror);
			Result.Coverage.Add(MantleCoverageCategory::StaticMeshes, EMantleCoverageOutcome::Approximated, ExporterLabelOf(Actor),
				TEXT("baked a mirrored copy of its mesh (a negative scale), placed at the positive scale."));
		}

		// #818: a decal with a negative scale publishes at the positive scale: Mantle places it unmirrored.
		if (Type == EMantleEntityType::Decal && FMantleActorDecompose::HasNegativeAxis(PublishedTransform(Actor).GetScale3D()))
		{
			UnmirroredDecals.Add(Actor);
			Result.Coverage.Add(MantleCoverageCategory::Decals, EMantleCoverageOutcome::Approximated, ExporterLabelOf(Actor),
				TEXT("has a negative scale, so it is placed at the positive scale: its picture is not mirrored."));
		}

		// #818: a Sound Cue publishes as the sound wave it plays first.
		if (Type == EMantleEntityType::SoundEmitter)
		{
			const UAudioComponent* Audio = CastChecked<AAmbientSound>(Actor)->GetAudioComponent();
			if (Audio && Audio->Sound.Get() && Audio->Sound->IsA<USoundCue>())
			{
				if (const USoundBase* Wave = FMantlePlaceAssets::PublishedSound(Audio->Sound.Get()))
				{
					Result.Coverage.Add(MantleCoverageCategory::Sounds, EMantleCoverageOutcome::Approximated, ExporterLabelOf(Actor),
						FString::Printf(TEXT("uses Sound Cue '%s', published as the one sound wave it plays first, '%s': the cue's mixing and randomness are not kept."),
							*Audio->Sound->GetName(), *Wave->GetName()));
				}
			}
		}

		// A Blueprint prefab's parts (#468), each a child entity of the actor's Group. A hashed id
		// that collides is suffixed rather than failing the publish, as MakeEntityId does.
		if (Type == EMantleEntityType::Group)
		{
			TArray<FMantlePrefabPart> PrefabParts;
			CollectPrefabParts(Actor, PrefabParts, nullptr, &Result.Coverage);
			if (PrefabParts.Num() > 0)
			{
				TArray<MantlePlaceExporterPrefab::FExportedPart>& Exported = PartsByActor.Add(Actor);
				for (const FMantlePrefabPart& Part : PrefabParts)
				{
					// A place holds at most MaxEffectsPerPlace effects (the parser refuses more): the rest are left out, and said so.
					if (Part.Type == EMantleEntityType::Effect && ++EffectParts > FMantlePlaceLimits::MaxEffectsPerPlace)
					{
						Result.Coverage.Add(MantleCoverageCategory::Particles, EMantleCoverageOutcome::Dropped, ExporterLabelOf(Actor),
							FString::Printf(TEXT("left out: a place holds at most %d effects."), FMantlePlaceLimits::MaxEffectsPerPlace));
						continue;
					}
					if (Part.Type == EMantleEntityType::Text)
					{
						FMantleTextExport::Admit(Cast<UTextRenderComponent>(Part.Component),
							FString::Printf(TEXT("%s (%s)"), *ExporterLabelOf(Actor), *Part.Component->GetName()),
							FString::Printf(TEXT("/actors/%s/%s"), *Actor->GetName(), *Part.Component->GetName()),
							Part.Transform.GetScale3D(), Texts, &Result.Coverage, Result.Errors);
					}
					if (Part.MirroredMesh)
					{
						Result.Coverage.Add(MantleCoverageCategory::StaticMeshes, EMantleCoverageOutcome::Approximated,
							FString::Printf(TEXT("%s (%s)"), *ExporterLabelOf(Actor), *Part.Component->GetName()),
							TEXT("baked a mirrored copy of its mesh (a negative scale), placed at the positive scale."));
					}
					const FString Base = PrefabPartEntityId(ActorId, Part.Component, Part.EffectIndex);
					FString Id = Base;
					int32 Suffix = 2;
					while (UsedIds.Contains(Id))
					{
						Id = FString::Printf(TEXT("%s_%d"), *Base.Left(36), Suffix++);
					}
					UsedIds.Add(Id);
					Exported.Add(MantlePlaceExporterPrefab::FExportedPart{ Part, Id });
				}
			}
		}
	}

	// The Water Zone (GhzGarage/mantle#997): settings.water, when the place has a body to put in it.
	if (WaterBodies > 0)
	{
		const FMantleWaterBodies Water = FMantleWaterBodyExport::Read(World);
		if (const FMantleWaterZoneData* Zone = FMantleWaterBodyExport::PrimaryZone(Water))
		{
			Place.Settings.Water = FMantleWaterBodyExport::BuildZone(*Zone);
		}
	}

	// Pass 2 — build entities now that every id exists.
	TSet<FString> ReferencedAssets;
	MantlePlaceExporterUploads::FContext Uploads;
	Uploads.Assets = Options.Assets;
	Uploads.Limits = FMantlePlaceAssetLimits::FromContract();
	FMantleCrowdBudget CrowdBudget;
	int32 Constraints = 0;

	for (int32 i = 0; i < Exportable.Num(); ++i)
	{
		const AActor* Actor = Exportable[i];
		// The entity's index, which a prefab's parts before it (#468) put past the actor's.
		const FString Path = FString::Printf(TEXT("/entities/%d"), Place.Entities.Num());

		FMantleEntity Entity;
		Entity.Id = IdByActor[Actor];
		ClassifyActor(Actor, Entity.Type);

		Entity.Name = ExporterLabelOf(Actor);

		// Only treat an attachment as parenting when the parent is itself being exported.
		const AActor* ParentActor = Actor->GetAttachParentActor();
		if (ParentActor && IdByActor.Contains(ParentActor))
		{
			Entity.Parent = IdByActor[ParentActor];
			Entity.Transform = Actor->GetRootComponent()->GetRelativeTransform();
		}
		else
		{
			Entity.Transform = Actor->GetActorTransform();
		}

		// Movable is the opt-in a script needs to move this entity later (S1.2b onward); a
		// creator sets it on the actor directly, and export simply reads it back out.
		if (const USceneComponent* Root = Actor->GetRootComponent())
		{
			Entity.bDynamic = Root->Mobility == EComponentMobility::Movable;
		}
		// A prefab (#468) only when something could move it: a new Blueprint's root is Movable by
		// default, and reading that as the opt-in would make every Fab prefab dynamic.
		if (PartsByActor.Contains(Actor))
		{
			Entity.bDynamic = IsPrefabDynamic(Actor);
		}

		// #628: a mirrored mesh at the positive scale, with the copy of its mesh (pass 1 made it).
		const FMantleMirrorResult* ActorMirror = MirrorByActor.Find(Actor);
		if (ActorMirror)
		{
			Entity.Transform.SetScale3D(ActorMirror->PositiveScale);
		}
		if (UnmirroredDecals.Contains(Actor))
		{
			Entity.Transform.SetScale3D(Entity.Transform.GetScale3D().GetAbs());
		}
		ExtractProperties(Actor, Entity.Type, Path, Entity, ReferencedAssets, Uploads, Result.Errors, ActorMirror ? ActorMirror->Mesh : nullptr);

		// The crowd budgets span the level (#477), so they are counted here, at the paths the
		// parser reports them. So do points of interest (#513): the actors they name have ids now.
		if (FMantleCrowdSpawnerProps* Crowd = Entity.Props.TryGet<FMantleCrowdSpawnerProps>())
		{
			FString PointProblem;
			if (!ResolveCrowdPoints(Actor, IdByActor, *Crowd, PointProblem))
			{
				Result.Errors.Add(FMantleParseError{
					Path + TEXT("/properties/pointsOfInterest"), FString::Printf(TEXT("'%s' %s"), *Entity.Name, *PointProblem) });
			}
			FString SpawnerProblem;
			FString NpcProblem;
			CrowdBudget.Add(Crowd->Count, SpawnerProblem, NpcProblem);
			if (!SpawnerProblem.IsEmpty())
			{
				Result.Errors.Add(FMantleParseError{
					Path + TEXT("/type"), FString::Printf(TEXT("'%s' %s"), *Entity.Name, *SpawnerProblem) });
			}
			if (!NpcProblem.IsEmpty())
			{
				Result.Errors.Add(FMantleParseError{
					Path + TEXT("/properties/count"), FString::Printf(TEXT("'%s' %s"), *Entity.Name, *NpcProblem) });
			}
		}

		// A joint (#528) names the entities it joins, which have their ids now. The cap spans the level,
		// so it is counted here, at the path the parser reports it.
		if (Entity.Type == EMantleEntityType::Constraint)
		{
			FMantleConstraintProps Joint;
			const AActor* JointA = nullptr;
			const AActor* JointB = nullptr;
			FString Problem;
			const FString* IdA = nullptr;
			const FString* IdB = nullptr;
			if (ReadConstraint(Actor, Joint, JointA, JointB, Problem))
			{
				IdA = IdByActor.Find(JointA);
				IdB = JointB ? IdByActor.Find(JointB) : nullptr;
			}
			if (IdA && (!JointB || IdB))
			{
				Joint.EntityA = *IdA;
				Joint.EntityB = IdB ? *IdB : FString();
			}
			else
			{
				// ReadConstraint agreed when pass 1 took the actor; this is a level that changed since.
				Result.Errors.Add(FMantleParseError{
					Path + TEXT("/properties"),
					FString::Printf(TEXT("'%s' %s"), *Entity.Name, Problem.IsEmpty() ? TEXT("joins an object that is not published.") : *Problem) });
			}
			Entity.Props.Set<FMantleConstraintProps>(MoveTemp(Joint));
			// Where the editor shows it; the joint is where its anchors are, and nothing moves it.
			Entity.bDynamic = false;
			if (++Constraints == FMantlePlaceLimits::MaxConstraintsPerPlace + 1)
			{
				Result.Errors.Add(FMantleParseError{
					Path + TEXT("/type"),
					FString::Printf(TEXT("'%s' is physics constraint number %d; a place may have at most %d. Remove some, or join them from a place script."),
						*Entity.Name, Constraints, FMantlePlaceLimits::MaxConstraintsPerPlace) });
			}
		}

		for (const FName& Tag : Actor->Tags)
		{
			// MantleAnchored is read into the physics block (ReadPhysics), never an entity tag.
			if (Tag != MantlePhysicsAnchoredTag())
			{
				Entity.Tags.Add(Tag.ToString());
			}
		}

		const FString GroupId = Entity.Id;
		const bool bGroupDynamic = Entity.bDynamic;
		Place.Entities.Add(MoveTemp(Entity));

		// Its parts (#468), right after it: named after their components, placed relative to the
		// group, and dynamic exactly when the group is (IsPrefabDynamic decides for the whole prefab).
		if (const TArray<MantlePlaceExporterPrefab::FExportedPart>* PrefabParts = PartsByActor.Find(Actor))
		{
			for (const MantlePlaceExporterPrefab::FExportedPart& Exported : *PrefabParts)
			{
				const FString PartPath = FString::Printf(TEXT("/entities/%d"), Place.Entities.Num());
				FMantleEntity Child;
				Child.Id = Exported.Id;
				Child.Type = Exported.Part.Type;
				// A component's second and later effects are numbered after it (#628).
				const FString PartSuffix = Exported.Part.EffectIndex > 0 ? FString::Printf(TEXT("_%d"), Exported.Part.EffectIndex + 1) : FString();
				Child.Name = Exported.Part.Component->GetName().Left(FMantlePlaceLimits::MaxEntityNameLength - PartSuffix.Len()) + PartSuffix;
				Child.Parent = GroupId;
				Child.Transform = Exported.Part.Transform;
				Child.bDynamic = bGroupDynamic;
				MantlePlaceExporterPrefab::ExtractPartProperties(Exported.Part, PartPath, Child, ReferencedAssets, Uploads, Result.Errors);
				Place.Entities.Add(MoveTemp(Child));
			}
		}
	}

	// Pass 2a — instanced meshes (#467), from every exported instance source on any actor: PCG
	// output generated in the editor, painted foliage, HISM/ISM components. Grouped by mesh and
	// material set across the whole level in world space, then cut into entities of at most
	// MaxInstancesPerEntity. Meshes and materials go through the library or the upload path exactly
	// as a StaticMesh's do.
	// Whether the place is over its instance limit is decided at the end (GhzGarage/mantle#818), once thinning, if asked for, has run.
	// GhzGarage/mantle#818: the ids of the instanced entities painted foliage made, which thinning takes from first.
	TSet<FString> ThinFirstIds;
	{
		TArray<FMantleInstancedMeshGroup> Groups;
		TMap<FString, int32> GroupByKey;
		for (const UInstancedStaticMeshComponent* Component : CollectInstanceSources(World))
		{
			const AActor* Owner = Component->GetOwner();
			const FString SourcePath = FString::Printf(TEXT("/actors/%s/%s"), *Owner->GetName(), *Component->GetName());

			TArray<FMantleUnpublishedPart> Parts;
			CheckInstanceSource(Component, Parts);
			bool bBlocked = false;
			for (const FMantleUnpublishedPart& Part : Parts)
			{
				if (Part.bBlocksPublish)
				{
					Result.Errors.Add(FMantleParseError{
						SourcePath, FString::Printf(TEXT("'%s' %s %s"), *ExporterLabelOf(Owner), *Part.What, *Part.Remedy) });
					bBlocked = true;
				}
			}
			if (bBlocked)
			{
				continue;
			}

			// The group for this component's mesh, or for a mirrored copy of it (MeshOverride); same key, same group, level-wide.
			const bool bFoliage = ExporterIsFoliageOwner(Owner);
			auto GroupFor = [&](const UStaticMesh* MeshOverride)
			{
				FMantleStaticMeshProps Mesh;
				ExporterReadMeshComponent(Component, SourcePath, ReferencedAssets, Uploads, Result.Errors, Mesh, MeshOverride);
				const FString Key = InstancedGroupKey(Mesh);
				if (const int32* Found = GroupByKey.Find(Key))
				{
					Groups[*Found].bThinFirst = Groups[*Found].bThinFirst && bFoliage;
					return *Found;
				}
				const int32 Index = Groups.Num();
				GroupByKey.Add(Key, Index);
				FMantleInstancedMeshGroup& Added = Groups.AddDefaulted_GetRef();
				Added.Mesh = Mesh;
				Added.MeshName = Component->GetStaticMesh()->GetName();
				Added.bThinFirst = bFoliage;
				return Index;
			};
			const int32 GroupIndex = GroupFor(nullptr);

			// GhzGarage/mantle#818: a mirrored instance is placed as an instance of the mesh's mirrored copy (one per set of negative
			// axes, cached by FMantleMirrorBake) at the positive scale; one scaled to zero is left out.
			struct FMirroredGroup { int32 Group = INDEX_NONE; UStaticMesh* Mesh = nullptr; };
			TMap<int32, FMirroredGroup> MirrorGroups;
			TMap<int32, FString> MirrorFailures;
			int32 MirroredCount = 0;
			int32 FlatCount = 0;
			int32 UnmirroredCount = 0;
			FString UnmirroredWhy;

			const int32 Count = Component->GetInstanceCount();
			Groups[GroupIndex].WorldInstances.Reserve(Groups[GroupIndex].WorldInstances.Num() + Count);
			for (int32 InstanceIndex = 0; InstanceIndex < Count; ++InstanceIndex)
			{
				FTransform Instance;
				if (!Component->GetInstanceTransform(InstanceIndex, Instance, /*bWorldSpace*/ true))
				{
					continue;
				}
				const FTransform Normalized = ExporterNormalizeInstance(Instance);
				const FVector Scale = Normalized.GetScale3D();
				if (Scale.GetMin() < 0.0)
				{
					const int32 Mask = (Scale.X < 0.0 ? 1 : 0) | (Scale.Y < 0.0 ? 2 : 0) | (Scale.Z < 0.0 ? 4 : 0);
					FMirroredGroup* Mirrored = MirrorGroups.Find(Mask);
					if (!Mirrored && !MirrorFailures.Contains(Mask))
					{
						const FMantleMirrorResult Mirror = FMantleActorDecompose::ResolveMirror(Component->GetStaticMesh(), Scale);
						if (Mirror.bOk)
						{
							Mirrored = &MirrorGroups.Add(Mask, FMirroredGroup{ GroupFor(Mirror.Mesh), Mirror.Mesh });
						}
						else
						{
							MirrorFailures.Add(Mask, Mirror.Why);
						}
					}
					if (!Mirrored)
					{
						++UnmirroredCount;
						UnmirroredWhy = MirrorFailures.FindRef(Mask);
						continue;
					}
					FTransform Placed = Normalized;
					Placed.SetScale3D(Scale.GetAbs());
					Groups[Mirrored->Group].WorldInstances.Add(Placed);
					++MirroredCount;
				}
				else if (Scale.GetMin() < GExporterMinInstanceScale)
				{
					++FlatCount;
				}
				else
				{
					Groups[GroupIndex].WorldInstances.Add(Normalized);
				}
			}

			const FString MeshName = Component->GetStaticMesh()->GetName();
			if (MirroredCount > 0)
			{
				Result.Coverage.Add(MantleCoverageCategory::StaticMeshes, EMantleCoverageOutcome::Approximated, MeshName,
					TEXT("instances with a negative scale are published as instances of a mirrored copy of the mesh, at the positive scale."), MirroredCount);
			}
			if (FlatCount > 0)
			{
				Result.Coverage.Add(MantleCoverageCategory::StaticMeshes, EMantleCoverageOutcome::Dropped, MeshName,
					TEXT("instances scaled to zero are left out: they have no size, so nothing of them can be seen or touched."), FlatCount);
			}
			if (UnmirroredCount > 0)
			{
				Result.Coverage.Add(MantleCoverageCategory::StaticMeshes, EMantleCoverageOutcome::Dropped, MeshName,
					FString::Printf(TEXT("instances with a negative scale are left out: a mirrored copy of the mesh could not be made (%s)."), *UnmirroredWhy), UnmirroredCount);
			}
		}

		// GhzGarage/mantle#818: static mesh actors that are the same mesh with the same materials become one instance set (the
		// scratch grouping pass of Medieval Village), so a prop-heavy map is a few hundred entities, not thousands.
		if (Options.bGroupStaticMeshes)
		{
			Result.Grouping = FMantlePlaceInstancing::GroupStaticMeshes(Place, Groups);
			if (Result.Grouping.Actors > 0)
			{
				Result.Coverage.Add(MantleCoverageCategory::StaticMeshes, EMantleCoverageOutcome::Approximated, TEXT("identical mesh actors"),
					FString::Printf(TEXT("%d actors that share a mesh and materials were published as %d instance sets: each keeps its place, rotation and scale, but they are no longer separate named objects."),
						Result.Grouping.Actors, Result.Grouping.Sets),
					Result.Grouping.Actors);
				UE_LOG(LogMantle, Log, TEXT("Grouped %d identical static mesh actors into %d instance sets"), Result.Grouping.Actors, Result.Grouping.Sets);
			}
		}

		TSet<FString> FoliagePrefixes;
		for (const FMantleInstancedMeshGroup& Group : Groups)
		{
			if (Group.bThinFirst)
			{
				FoliagePrefixes.Add(InstancedEntityIdPrefix(Group.Mesh));
			}
		}
		for (FMantleEntity& Entity : ChunkInstancedMeshes(Groups, FMantlePlaceLimits::MaxInstancesPerEntity))
		{
			// Hashed ids don't collide with guid-based ones in practice; if one ever does, the
			// later entity is suffixed rather than failing the publish, as MakeEntityId does.
			const FString Base = Entity.Id;
			int32 Suffix = 2;
			while (UsedIds.Contains(Entity.Id))
			{
				Entity.Id = FString::Printf(TEXT("%s_%d"), *Base.Left(36), Suffix++);
			}
			UsedIds.Add(Entity.Id);
			for (const FString& Prefix : FoliagePrefixes)
			{
				if (Base.StartsWith(Prefix))
				{
					ThinFirstIds.Add(Entity.Id);
					break;
				}
			}
			Place.Entities.Add(MoveTemp(Entity));
		}
	}

	// Pass 2b — spline meshes (#628): a landscape's road and river-bank splines, PCG's Spawn Spline Mesh
	// output and Blueprint spline meshes. FMantleSplineMeshBake merges them by owning actor and material
	// set into static meshes, deformed along their splines as the components render, and cuts them within
	// the place mesh limits. Each is written as a StaticMesh entity in world space, and its mesh and
	// materials go through the upload path exactly as a project mesh's do.
	{
		const FMantleAssetLibrary& Library = FMantleAssetLibrary::Get();
		const FMantleSplineBakeResult Bake = FMantleSplineMeshBake::Bake(World, Uploads.Limits);
		for (const FMantleSplineBakeProblem& Problem : Bake.Problems)
		{
			Result.Errors.Add(FMantleParseError{
				FString::Printf(TEXT("/actors/%s"), *Problem.OwnerName),
				FString::Printf(TEXT("'%s' %s %s"), *Problem.OwnerLabel, *Problem.What, *Problem.Remedy) });
			Result.Coverage.Add(MantleCoverageCategory::SplineMeshes, EMantleCoverageOutcome::Dropped, Problem.OwnerLabel, Problem.What);
		}
		// Baked, not exact: the geometry is the deformed triangles, merged and with the triangles as its collision.
		for (const FMantleSplineBakeReport& Report : Bake.Reports)
		{
			if (Report.Components > 0)
			{
				Result.Coverage.Add(MantleCoverageCategory::SplineMeshes, EMantleCoverageOutcome::Approximated, Report.OwnerLabel,
					FString::Printf(TEXT("baked into %d static mesh%s, merged by material."), Report.Meshes, Report.Meshes == 1 ? TEXT("") : TEXT("es")),
					Report.Components);
			}
			if (Report.Skipped > 0)
			{
				Result.Coverage.Add(MantleCoverageCategory::SplineMeshes, EMantleCoverageOutcome::Info, Report.OwnerLabel,
					TEXT("nothing a player could see or touch (hidden in game and not colliding, or an empty mesh), so nothing of them is published."), Report.Skipped);
			}
		}
		for (const FMantleBakedSplineMesh& Baked : Bake.Meshes)
		{
			const FString Path = FString::Printf(TEXT("/entities/%d"), Place.Entities.Num());
			TArray<FMantleValidationIssue> MeshIssues;
			FMantlePlaceAssets::CheckMesh(Baked.Mesh, FString(), Uploads.Limits, MeshIssues);
			for (const FMantleValidationIssue& Issue : MeshIssues)
			{
				if (Issue.Severity == EMantleIssueSeverity::Error)
				{
					Result.Errors.Add(FMantleParseError{ Path + TEXT("/properties/mesh"), Issue.Problem + TEXT(" ") + Issue.Remedy });
				}
			}

			FMantleStaticMeshProps Props;
			Props.Mesh = Uploads.IdFor(FMantlePlaceAssets::MeshKey(Baked.Mesh), EMantleAssetKind::StaticMesh, Baked.Mesh);
			// A baked mesh carries only its slots' names: each slot gets the material its spline meshes drew with.
			for (const int32 Slot : FMantlePlaceAssets::UploadSlots(Baked.Mesh))
			{
				const UMaterialInterface* Material = FMantlePlaceAssets::MeshSlotMaterial(Baked.Mesh, Slot);
				const FString LibraryId = Library.FindIdByPath(FSoftObjectPath(Material));
				if (!LibraryId.IsEmpty())
				{
					ReferencedAssets.Add(LibraryId);
					Props.Materials.Add(LibraryId);
				}
				else
				{
					Props.Materials.Add(Uploads.IdFor(FMantlePlaceAssets::MaterialKey(Material), EMantleAssetKind::Material, Material));
				}
			}
			Props.Collision = Baked.Collision;
			Props.bCastShadow = Baked.bCastShadow;
			Props.Surface = Baked.Surface;

			FMantleEntity Entity;
			// Hashed from the owner and material set, not the geometry, so editing a road keeps its id. A collision
			// (it never happens in practice) is suffixed rather than failing the publish, as the instanced ids are.
			Entity.Id = FString::Printf(TEXT("ent_spl_%s"), *ExporterMd5Hex(Baked.OwnerName + TEXT("|") + Baked.IdSeed).Left(24));
			const FString Base = Entity.Id;
			int32 Suffix = 2;
			while (UsedIds.Contains(Entity.Id))
			{
				Entity.Id = FString::Printf(TEXT("%s_%d"), *Base.Left(36), Suffix++);
			}
			UsedIds.Add(Entity.Id);
			Entity.Type = EMantleEntityType::StaticMesh;
			Entity.Name = Baked.Name.Left(FMantlePlaceLimits::MaxEntityNameLength);
			Entity.Transform = Baked.Transform;
			Entity.Props.Set<FMantleStaticMeshProps>(MoveTemp(Props));
			Place.Entities.Add(MoveTemp(Entity));
		}
	}

	// Pass 2c — the landscape as a Terrain (#466). An ALandscape is NotPlaceable, so pass 1 skipped
	// it as infrastructure; it is read here, with the same rules Check and the converter apply
	// (FMantleTerrainExport): one per place, its painted layers mapped to materials in Mantle Place
	// Settings, no holes. Its heightfield and weightmap go through the upload path by their keys.
	{
		const FMantleAssetLibrary& Library = FMantleAssetLibrary::Get();
		const TArray<AActor*> Landscapes = FMantleTerrainExport::CollectLandscapes(World);
		for (int32 LandscapeIndex = 0; LandscapeIndex < Landscapes.Num(); ++LandscapeIndex)
		{
			const AActor* Landscape = Landscapes[LandscapeIndex];
			const FString SourcePath = FString::Printf(TEXT("/actors/%s"), *Landscape->GetName());
			const FString Label = ExporterLabelOf(Landscape);
			if (LandscapeIndex >= FMantlePlaceLimits::MaxTerrainsPerPlace)
			{
				Result.Errors.Add(FMantleParseError{ SourcePath, FString::Printf(
					TEXT("'%s' is landscape number %d; a place publishes at most %d. Merge the landscapes into one, or remove the extra ones."),
					*Label, LandscapeIndex + 1, FMantlePlaceLimits::MaxTerrainsPerPlace) });
				continue;
			}
			FMantleTerrainSource Source;
			FString Problem;
			if (!FMantleTerrainExport::ReadLandscape(Landscape, Source, Problem))
			{
				Result.Errors.Add(FMantleParseError{ SourcePath, FString::Printf(TEXT("'%s' %s"), *Label, *Problem) });
				continue;
			}
			TArray<FMantleTerrainLayerChoice> Layers;
			TArray<FMantleValidationIssue> LayerIssues;
			if (!FMantleTerrainExport::ChooseLayers(World, Source, Label, Layers, LayerIssues, &Result.Coverage))
			{
				for (const FMantleValidationIssue& Issue : LayerIssues)
				{
					Result.Errors.Add(FMantleParseError{ SourcePath, FString::Printf(TEXT("'%s' %s %s"), *Label, *Issue.Problem, *Issue.Remedy) });
				}
				continue;
			}
			FMantleCoverageSurvey::ReportTerrainLayers(Result.Coverage, Label, Source, Layers);

			FMantleEntity Entity;
			Entity.Id = MakeEntityId(Landscape, UsedIds);
			Entity.Type = EMantleEntityType::Terrain;
			Entity.Name = Label.Left(FMantlePlaceLimits::MaxEntityNameLength);
			Entity.Transform = Source.Transform;
			FMantleTerrainProps Props;
			Props.Heightfield = Uploads.IdFor(FMantleTerrainExport::HeightfieldKey(Landscape), EMantleAssetKind::Heightfield, Landscape);
			if (Layers.Num() > 1)
			{
				Props.Weightmap = Uploads.IdFor(FMantleTerrainExport::WeightmapKey(Landscape), EMantleAssetKind::Texture, Landscape);
			}
			// Layers 4 to 7 (#628) are in a second weightmap, uploaded by the converter under its own key.
			if (FMantleTerrainExport::WeightmapCount(Layers.Num()) > 1)
			{
				Props.Weightmap2 = Uploads.IdFor(FMantleTerrainExport::Weightmap2Key(Landscape), EMantleAssetKind::Texture, Landscape);
			}
			for (const FMantleTerrainLayerChoice& Choice : Layers)
			{
				FMantleTerrainLayer& Layer = Props.Layers.AddDefaulted_GetRef();
				Layer.Tiling = Choice.TilingCm;
				// #596: the layer info's Phys Material, when it is one of Mantle's surface physical materials.
				Layer.Surface = MatchSurface(Choice.PhysMaterial);
				// A library material by its id, anything else through the upload path, as a mesh slot's is.
				const FString LibraryId = Library.FindIdByPath(FSoftObjectPath(Choice.Material));
				if (!LibraryId.IsEmpty())
				{
					ReferencedAssets.Add(LibraryId);
					Layer.Material = LibraryId;
				}
				else
				{
					Layer.Material = Uploads.IdFor(FMantlePlaceAssets::MaterialKey(Choice.Material), EMantleAssetKind::Material, Choice.Material);
				}
			}
			// Landscape grass (#628, FMantleTerrainGrassExport): an entry per grass type variety, its mesh and
			// material overrides named as a Scatter mesh's, its map by the upload key the converter makes.
			const FMantleTerrainGrassResult Grass = FMantleTerrainGrassExport::Gather(Landscape, Source, /*bLog*/ false);
			for (int32 GrassIndex = 0; GrassIndex < Grass.Entries.Num(); ++GrassIndex)
			{
				const FMantleGrassEntryDraft& Draft = Grass.Entries[GrassIndex];
				FMantleScatterMeshSettings GrassMesh;
				GrassMesh.Mesh = const_cast<UStaticMesh*>(Draft.Mesh);
				for (UMaterialInterface* GrassMaterial : Draft.Materials)
				{
					GrassMesh.Materials.Add(GrassMaterial);
				}
				FMantleScatterMesh GrassNamed;
				ExporterReadScatterMesh(GrassMesh, FString::Printf(TEXT("%s/properties/grass/%d"), *SourcePath, GrassIndex),
					ReferencedAssets, Uploads, Result.Errors, GrassNamed);
				const FString GrassMap = Uploads.IdFor(FMantleTerrainGrassExport::GrassMapKey(Landscape, Draft.Map), EMantleAssetKind::Texture, Landscape);
				Props.Grass.Add(FMantleTerrainGrassExport::MakeGrass(Draft, GrassMap, GrassNamed.MeshId, GrassNamed.Materials));
			}
			FMantleTerrainGrassExport::ReportCoverage(Result.Coverage, Grass);
			Entity.Props.Set<FMantleTerrainProps>(MoveTemp(Props));
			for (const FName& Tag : Landscape->Tags)
			{
				Entity.Tags.Add(Tag.ToString());
			}
			Place.Entities.Add(MoveTemp(Entity));
		}
	}

	// Pass 2d — scripts, collected from the level's Scripts/ folder on disk rather than from
	// any per-entity association; S1.8 already dropped the entity-level "scripts" array.
	{
		const FString ScriptsFolder = GetScriptsFolder(World);
		TArray<FMantleScriptCatalogEntry> Entries;
		TArray<FMantleScriptCollectionIssue> Issues;
		CollectScriptFiles(ScriptsFolder, Entries, Issues);
		TArray<FMantleGraphScriptEntry> GraphEntries;
		TArray<FMantleParseError> GraphErrors;
		TArray<FString> GraphPaths;
		CollectGraphScripts(ScriptsFolder, GraphEntries, GraphErrors, GraphPaths);
		FilterGraphGeneratedFiles(Entries, Issues, GraphPaths);

		for (const FMantleScriptCollectionIssue& Issue : Issues)
		{
			// Wasm is a dropped language (S1.6), not merely an unrecognised extension -- a
			// creator who still has a .wasm file under Scripts/ gets a message that says so
			// exactly, instead of the generic "unknown extension" one below.
			if (Issue.bLanguageUnknown &&
				FPaths::GetExtension(Issue.RelativePath).Equals(TEXT("wasm"), ESearchCase::IgnoreCase))
			{
				Result.Errors.Add(FMantleParseError{
					FString::Printf(TEXT("/scripts/%s"), *Issue.RelativePath),
					TEXT("Wasm scripts aren't supported. Use Luau, JavaScript, TypeScript or Python.") });
				continue;
			}
			// #549: UI files are never scripts. Still refused here, with where they belong.
			if (Issue.bLanguageUnknown &&
				(FPaths::GetExtension(Issue.RelativePath).Equals(TEXT("rml"), ESearchCase::IgnoreCase)
					|| FPaths::GetExtension(Issue.RelativePath).Equals(TEXT("rcss"), ESearchCase::IgnoreCase)))
			{
				Result.Errors.Add(FMantleParseError{
					FString::Printf(TEXT("/scripts/%s"), *Issue.RelativePath),
					FString::Printf(TEXT("'%s' is a UI file, not a script. Move it to the UI/ folder beside Scripts/."), *Issue.RelativePath) });
				continue;
			}

			Result.Errors.Add(FMantleParseError{
				FString::Printf(TEXT("/scripts/%s"), *Issue.RelativePath),
				Issue.bLanguageUnknown
					? FString::Printf(
						TEXT("'%s' has an extension Mantle cannot map to a script language. Use .luau, .js, .ts or .py."),
						*Issue.RelativePath)
					: FString::Printf(
						TEXT("'%s' is not inside Server/, Client/ or Shared/ (case-exact), so it was not exported."),
						*Issue.RelativePath) });
		}
		Result.Errors.Append(GraphErrors);

		TArray<FString> ScriptPaths;
		for (const FMantleScriptCatalogEntry& Entry : Entries) ScriptPaths.Add(Entry.Path);
		for (const FMantleGraphScriptEntry& Entry : GraphEntries) ScriptPaths.Add(Entry.Path);
		ScriptPaths.Sort();
		const TArray<FString> ScriptIds = MakeScriptIds(ScriptPaths);
		TMap<FString, FString> ScriptIdByPath;
		for (int32 i = 0; i < ScriptPaths.Num(); ++i) ScriptIdByPath.Add(ScriptPaths[i], ScriptIds[i]);
		for (int32 i = 0; i < Entries.Num(); ++i)
		{
			const FMantleScriptCatalogEntry& Entry = Entries[i];

			EMantleScriptLanguage MappedLanguage;
			if (!MapCatalogLanguage(Entry.Language, MappedLanguage))
			{
				Result.Errors.Add(FMantleParseError{
					FString::Printf(TEXT("/scripts/%s"), *Entry.Path),
					TEXT("this script's language is not supported for publishing yet.") });
				continue;
			}

			// #394: scanned as authored (TypeScript before transpiling), exactly as Check scans it,
			// so Check's note and the recorded version always agree.
			Result.ScriptApiNeed.Merge(FMantleScriptApiUsage::ForSource(Entry.Source, Entry.Language));

			FMantleScript Script;
			Script.Id = ScriptIdByPath.FindChecked(Entry.Path);
			Script.Name = Entry.Name;
			Script.Path = Entry.Path;
			Script.Side = MapCatalogSide(Entry.Side);
			Script.Language = MappedLanguage;

			FString ExecutableSource = Entry.Source;
			if (Script.Language == EMantleScriptLanguage::TypeScript)
			{
				FMantleScriptError CompileError;
				if (!CompileTypeScript(Entry.Source, ExecutableSource, CompileError))
				{
					Result.Errors.Add(FMantleParseError{
						FString::Printf(TEXT("/scripts/%s"), *Entry.Path),
						CompileError.Message });
					// Keep the failed result structurally serialisable for diagnostics; bSuccess remains false.
					ExecutableSource = Entry.Source;
				}
			}

			Script.Source = Options.ScriptSourceIds.FindRef(Script.Id);
			if (Script.Source.IsEmpty())
			{
				Script.Source = ProvisionalScriptSourceId(ExecutableSource);
			}
			if (Script.Language == EMantleScriptLanguage::TypeScript)
			{
				Script.AuthorSource = Options.ScriptAuthorSourceIds.FindRef(Script.Id);
				if (Script.AuthorSource.IsEmpty())
				{
					Script.AuthorSource = ProvisionalScriptSourceId(Entry.Source);
				}
			}
			Place.Scripts.Add(MoveTemp(Script));
		}

		for (const FMantleGraphScriptEntry& Entry : GraphEntries)
		{
			FMantleScript Script;
			Script.Id = ScriptIdByPath.FindChecked(Entry.Path);
			Script.Name = Entry.Name;
			Script.Path = Entry.Path;
			Script.Side = Entry.Side;
			Script.Language = EMantleScriptLanguage::Luau;
			Script.bAuthoredAsGraph = true;
			Script.Source = Options.ScriptSourceIds.FindRef(Script.Id);
			if (Script.Source.IsEmpty()) Script.Source = ProvisionalScriptSourceId(Entry.CompiledLuau);
			Script.AuthorSource = Options.ScriptAuthorSourceIds.FindRef(Script.Id);
			if (Script.AuthorSource.IsEmpty()) Script.AuthorSource = ProvisionalScriptSourceId(Entry.AuthorJson);
			Place.Scripts.Add(MoveTemp(Script));

			Result.ScriptApiNeed.Merge(Entry.ApiNeed);
		}
		Place.Scripts.Sort([](const FMantleScript& A, const FMantleScript& B) { return A.Path < B.Path; });

		if (Place.Scripts.Num() > 0)
		{
			Place.ScriptApi = RequiredScriptApiVersion(Result.ScriptApiNeed);
		}
	}

	// Pass 2e — UI documents and stylesheets from the level's UI/ folder, beside Scripts/.
	// Like scripts, the place file carries only ids: uid_ over the sorted paths, and the uploaded
	// src_ id (or, offline, a provisional one from the text).
	// #556 item 5: then the images there, each by its uploaded Texture asset's id (listed in assets[]
	// by Pass 3), or offline a provisional one.
	{
		TArray<FMantleUIDocumentFile> UIFiles;
		TArray<FMantleUIImageFile> UIImages;
		CollectUIDocumentFiles(GetUIFolder(World), UIFiles, UIImages);
		TArray<FMantleUIDocumentProblem> UIProblems = CheckUIDocumentFiles(UIFiles);
		UIProblems.Append(CheckUIImageFiles(UIImages, UIFiles));
		// #549 P6a: and what players' games refuse in a document's text, so Publish cannot skip Check's scans.
		UIProblems.Append(CheckUIDocumentContent(UIFiles));
		for (const FMantleUIDocumentProblem& Problem : UIProblems)
		{
			Result.Errors.Add(FMantleParseError{
				Problem.Path.IsEmpty() ? FString(TEXT("/uiDocuments")) : TEXT("/uiDocuments/") + Problem.Path,
				Problem.Problem + TEXT(" ") + Problem.Remedy });
		}

		// Documents first, so their ids are the ones they had before images were listed.
		TArray<FString> UIPaths;
		for (const FMantleUIDocumentFile& File : UIFiles) UIPaths.Add(File.Path);
		for (const FMantleUIImageFile& Image : UIImages) UIPaths.Add(Image.Path);
		const TArray<FString> UIIds = MakeUIDocumentIds(UIPaths);
		for (int32 i = 0; i < UIFiles.Num(); ++i)
		{
			FMantleUIDocument Document;
			Document.Id = UIIds[i];
			Document.Path = UIFiles[i].Path;
			Document.Source = Options.UIDocumentSourceIds.FindRef(Document.Path);
			if (Document.Source.IsEmpty())
			{
				Document.Source = ProvisionalScriptSourceId(UIFiles[i].Text);
			}
			Place.UIDocuments.Add(MoveTemp(Document));
		}
		for (int32 i = 0; i < UIImages.Num(); ++i)
		{
			FMantleUIDocument Image;
			Image.Id = UIIds[UIFiles.Num() + i];
			Image.Path = UIImages[i].Path;
			// The converted texture's key when Publish prepared it (FMantlePlaceAssetConverter::ConvertUIImage), else one
			// from the path, whose provisional id stands in as a decal texture's does before its upload.
			const FString* Converted = Options.Assets ? Options.Assets->UIImages.Find(Image.Path) : nullptr;
			const FString Key = Converted ? *Converted : TEXT("uiimage:") + Image.Path;
			Image.Asset = Uploads.IdFor(Key, EMantleAssetKind::Texture, nullptr);
			Place.UIDocuments.Add(MoveTemp(Image));
		}
	}

	// #786: the catalogue picture Publish captured and prepared, a texture upload like any other. Only a publish has
	// one; an export for a preview or a file leaves metadata.thumbnail out.
	if (Options.Assets && !Options.Assets->Thumbnail.IsEmpty())
	{
		Place.Metadata.Thumbnail = Uploads.IdFor(Options.Assets->Thumbnail, EMantleAssetKind::Texture, nullptr);
	}

	// GhzGarage/MantleCore#142: Mantle Seats, each owned by the entity of the prop it is on.
	FMantleSeatExport::Export(Exportable, IdByActor, Place.Seats, Result.Errors);

	// Pass 2f — the level's storage, from Mantle Place Settings: the library items the place pins by
	// name. The schema's rules are the parser's, and every VehicleSpawn's Storage Item must name an entry. The
	// place file carries references only: nothing is uploaded or converted for a stored item.
	{
		AMantlePlaceSettingsActor::ApplyStorageTo(World, Place.Storage);
		for (const FMantleStorageProblem& Problem : FMantleStorage::CheckEntries(Place.Storage))
		{
			Result.Errors.Add(FMantleParseError{ Problem.Path, Problem.Message });
		}
		for (int32 i = 0; i < Place.Entities.Num(); ++i)
		{
			const FMantleVehicleSpawnProps* VehicleSpawn = Place.Entities[i].Props.TryGet<FMantleVehicleSpawnProps>();
			const FMantleWeaponSpawnProps* WeaponSpawn = Place.Entities[i].Props.TryGet<FMantleWeaponSpawnProps>();
			const FString* Stored = VehicleSpawn ? &VehicleSpawn->StorageItem : WeaponSpawn ? &WeaponSpawn->StorageItem : nullptr;
			if (!Stored || Stored->IsEmpty())
			{
				continue;
			}
			const FString& StoredName = *Stored;
			const bool bDeclared = Place.Storage.ContainsByPredicate([&StoredName](const FMantleStorageEntry& Entry)
			{
				return Entry.Name.Equals(StoredName, ESearchCase::CaseSensitive);
			});
			if (!bDeclared)
			{
				Result.Errors.Add(FMantleParseError{
					FString::Printf(TEXT("/entities/%d/properties/storageItem"), i),
					FString::Printf(TEXT("'%s' names storage '%s', which Mantle Place Settings > Storage does not list. Add an entry with that name, or fix the spawn's Storage Item."),
						*Place.Entities[i].Name, *StoredName) });
			}
		}

		// #838: the level's script packages (Packages/, beside Scripts/). A package is never a place script: going live stores it as a
		// library item under its folder's name, so a Script Point may name a package folder before it is in storage.
		TArray<FMantleLocalScriptPackage> LocalPackages;
		TArray<FMantleScriptPackageProblem> PackageProblems;
		FMantleScriptPackages::Collect(FMantleScriptPackages::GetPackagesFolder(World), LocalPackages, PackageProblems);
		for (const FMantleScriptPackageProblem& Problem : PackageProblems)
		{
			if (Problem.bError)
			{
				Result.Errors.Add(FMantleParseError{
					Problem.Package.IsEmpty() ? FString(TEXT("/packages")) : TEXT("/packages/") + Problem.Package,
					Problem.Problem + TEXT(" ") + Problem.Remedy });
			}
		}
		const AMantlePlaceSettingsActor* StorageSettings = AMantlePlaceSettingsActor::Find(World);
		bool bUsesPackages = LocalPackages.Num() > 0;
		if (StorageSettings)
		{
			bUsesPackages = bUsesPackages || StorageSettings->Storage.ContainsByPredicate([](const FMantleStorageSetting& Entry)
			{
				return Entry.Kind.Equals(FMantleStorage::KindScript, ESearchCase::CaseSensitive);
			});
		}
		int32 ScriptPoints = 0;
		for (int32 i = 0; i < Place.Entities.Num(); ++i)
		{
			const FMantleScriptPointProps* Point = Place.Entities[i].Props.TryGet<FMantleScriptPointProps>();
			if (!Point)
			{
				continue;
			}
			bUsesPackages = true;
			const FString PointPath = FString::Printf(TEXT("/entities/%d/properties/storageItem"), i);
			if (++ScriptPoints == FMantlePlaceLimits::MaxScriptPoints + 1)
			{
				Result.Errors.Add(FMantleParseError{ FString::Printf(TEXT("/entities/%d"), i),
					FString::Printf(TEXT("'%s' is Script Point number %d; a place has at most %d. Remove some."),
						*Place.Entities[i].Name, ScriptPoints, FMantlePlaceLimits::MaxScriptPoints) });
			}
			if (Point->StorageItem.IsEmpty() || !FMantlePlaceLimits::IsStorageName(Point->StorageItem))
			{
				continue; // refused where it was read
			}
			const FString& Name = Point->StorageItem;
			const bool bLocal = LocalPackages.ContainsByPredicate([&Name](const FMantleLocalScriptPackage& Package)
			{
				return Package.Name.Equals(Name, ESearchCase::CaseSensitive);
			});
			const FMantleStorageSetting* Stored = StorageSettings ? StorageSettings->FindStorageEntry(Name) : nullptr;
			if (!bLocal && !Stored)
			{
				Result.Errors.Add(FMantleParseError{ PointPath,
					FString::Printf(TEXT("'%s' names the script package '%s', which neither Mantle Place Settings > Storage nor the level's Packages/ folder has. Add the package to storage with Tools > Mantle > Library, or fix the point's Storage Item."),
						*Place.Entities[i].Name, *Name) });
			}
			else if (!bLocal && !Stored->Kind.IsEmpty() && !Stored->Kind.Equals(FMantleStorage::KindScript, ESearchCase::CaseSensitive))
			{
				Result.Errors.Add(FMantleParseError{ PointPath,
					FString::Printf(TEXT("'%s' names storage '%s', which is a %s, not a script package. Pick a script package in the point's Storage Item."),
						*Place.Entities[i].Name, *Name, *Stored->Kind) });
			}
			else
			{
				// #838 stage 6: the point's parameters against the package's declarations, when they are known.
				FMantleScriptPackageManifest Manifest;
				if (FMantleScriptPackages::KnownManifest(Name, LocalPackages, Stored ? Stored->Manifest : FString(), Manifest))
				{
					TMap<FString, FString> Values;
					for (const FMantleScriptPointParameter& Parameter : Point->Parameters)
					{
						Values.Add(Parameter.Name, Parameter.Value);
					}
					for (const FMantleScriptOptionIssue& Issue : FMantleScriptPackages::CheckOptionValues(Name, Manifest.Parameters, Values, TEXT("parameter")))
					{
						Result.Errors.Add(FMantleParseError{ FString::Printf(TEXT("/entities/%d/properties/parameters/%s"), i, *Issue.Key),
							FString::Printf(TEXT("Script Point '%s': %s"), *Place.Entities[i].Name, *Issue.Message) });
					}
				}
			}
		}

		// #838 stage 6: each storage entry's settings against its package's declarations: none on a vehicle or a weapon, and on a
		// script package only what its manifest declares, fitting each setting's type, with every required one that has no default.
		for (int32 i = 0; i < Place.Storage.Num(); ++i)
		{
			const FMantleStorageEntry& Entry = Place.Storage[i];
			const FString SettingsPath = FString::Printf(TEXT("/storage/%d/settings"), i);
			const FMantleStorageSetting* Stored = StorageSettings ? StorageSettings->FindStorageEntry(Entry.Name) : nullptr;
			const FString& EntryName = Entry.Name;
			const bool bLocal = LocalPackages.ContainsByPredicate([&EntryName](const FMantleLocalScriptPackage& Package)
			{
				return Package.Name.Equals(EntryName, ESearchCase::CaseSensitive);
			});
			if (!bLocal && Stored && !Stored->Kind.IsEmpty() && !Stored->Kind.Equals(FMantleStorage::KindScript, ESearchCase::CaseSensitive))
			{
				if (Entry.Settings.Num() > 0)
				{
					Result.Errors.Add(FMantleParseError{ SettingsPath,
						FString::Printf(TEXT("%s is a %s, and only a script package takes settings. Remove its settings in Mantle Place Settings > Storage."),
							*Entry.Name, *Stored->Kind) });
				}
				continue;
			}
			FMantleScriptPackageManifest Manifest;
			if (!FMantleScriptPackages::KnownManifest(Entry.Name, LocalPackages, Stored ? Stored->Manifest : FString(), Manifest))
			{
				continue;
			}
			for (const FMantleScriptOptionIssue& Issue : FMantleScriptPackages::CheckOptionValues(Entry.Name, Manifest.Settings, Entry.Settings, TEXT("setting")))
			{
				Result.Errors.Add(FMantleParseError{ SettingsPath + TEXT("/") + Issue.Key, Issue.Message });
			}
		}
		// A package folder not stored yet is stored with no settings on its first publish, which a required setting without a default refuses.
		for (const FMantleLocalScriptPackage& Package : LocalPackages)
		{
			const FString& PackageName = Package.Name;
			const bool bStored = Place.Storage.ContainsByPredicate([&PackageName](const FMantleStorageEntry& Entry)
			{
				return Entry.Name.Equals(PackageName, ESearchCase::CaseSensitive);
			});
			if (bStored || !Package.bHasManifest)
			{
				continue;
			}
			for (const FMantleScriptOptionIssue& Issue : FMantleScriptPackages::CheckOptionValues(Package.Name, Package.Manifest.Settings, TMap<FString, FString>(), TEXT("setting")))
			{
				Result.Errors.Add(FMantleParseError{ TEXT("/packages/") + Package.Name,
					Issue.Message + TEXT(" Its first publish stores it with no settings: give that setting a default in package.json.") });
			}
		}
		if (bUsesPackages)
		{
			// What the packages' own scripts use counts as the place's own does; running packages at all needs 1.35.
			for (const FMantleLocalScriptPackage& Package : LocalPackages)
			{
				for (const FMantleScriptCatalogEntry& Entry : Package.Entries)
				{
					Result.ScriptApiNeed.Merge(FMantleScriptApiUsage::ForSource(Entry.Source, Entry.Language));
				}
			}
			Place.ScriptApi.Major = FMantleApiManifest::Get().Major;
			Place.ScriptApi.Minor = FMantleScriptPackages::RequiredScriptApiMinor(Result.ScriptApiNeed.Minor, /*bUsesPackages*/ true);
		}
	}

	// Pass 2g — the level's Script Assets (Mantle Place Settings): meshes, materials and sounds its
	// scripts spawn or play by id (#462, #459) though no entity uses them. Listed in assets[] below
	// exactly as placed content is, and only once when something placed uses the same asset.
	{
		const AMantlePlaceSettingsActor* SettingsActor = AMantlePlaceSettingsActor::Find(World);
		const FString SettingsName = SettingsActor ? SettingsActor->GetName() : FString();
		for (const FMantleScriptAsset& Asset : FMantlePlaceAssets::ScriptAssets(World))
		{
			const FString AssetPath = FString::Printf(TEXT("/actors/%s/ScriptAssets/%d"), *SettingsName, Asset.Index);
			FString Problem;
			FString Remedy;
			if (!FMantlePlaceAssets::CheckScriptAsset(Asset, Uploads.Limits, Problem, Remedy))
			{
				Result.Errors.Add(FMantleParseError{
					AssetPath, FString::Printf(TEXT("Script asset '%s' %s. %s"), *Asset.Path.ToString(), *Problem, *Remedy) });
				continue;
			}
			FMantleScriptAssetId& Listed = Result.ScriptAssets.AddDefaulted_GetRef();
			Listed.Path = Asset.Path.ToString();
			Listed.Id = ExporterReferenceScriptAsset(Asset.Object, AssetPath, ReferencedAssets, Uploads, Result.Errors, Listed.bProvisional);
		}
	}

	// Pass 3 — asset manifest for everything actually referenced.
	TArray<FString> SortedAssets = ReferencedAssets.Array();
	SortedAssets.Sort();

	const FMantleAssetLibrary& Library = FMantleAssetLibrary::Get();
	for (const FString& AssetId : SortedAssets)
	{
		const FMantleAssetEntry* Entry = Library.Find(AssetId);
		if (!Entry)
		{
			continue;
		}

		FMantleAssetRef Ref;
		Ref.Id = AssetId;
		Ref.Kind = Entry->Kind;
		Ref.Hash = ProvisionalHash(Entry->Path, Ref.Bytes);
		Place.Assets.Add(MoveTemp(Ref));
	}

	// Uploaded assets (F5 #339), with a material's textures: the client preflights all of them.
	// The upload's own hash and size once the queue has recorded it; before that, the converted
	// file's, or the source package's provisional digest when nothing was converted.
	{
		TSet<FString> Listed;
		for (const FMantleAssetRef& Existing : Place.Assets)
		{
			Listed.Add(Existing.Id);
		}
		const FMantlePlaceAssetSet* Assets = Options.Assets;
		auto AddUpload = [&Place, &Listed, &Uploads, Assets](const FString& Key, EMantleAssetKind Kind)
		{
			FMantleAssetRef Ref;
			Ref.Kind = Kind;
			const FMantlePlaceUploadedRef* Uploaded = Assets ? Assets->Uploaded.Find(Key) : nullptr;
			if (Uploaded && !Uploaded->Id.IsEmpty())
			{
				Ref.Id = Uploaded->Id;
				Ref.Hash = Uploaded->Hash;
				Ref.Bytes = Uploaded->Bytes;
			}
			else
			{
				Ref.Id = FMantlePlaceAssets::ProvisionalId(Key);
				const FMantleConvertedMesh* Mesh = Assets ? Assets->FindMesh(Key) : nullptr;
				const FMantleConvertedTexture* Texture = Assets ? Assets->FindTexture(Key) : nullptr;
				const FMantleConvertedSound* Sound = Assets ? Assets->FindSound(Key) : nullptr;
				const FMantleConvertedSkeletalMesh* SkeletalMesh = Assets ? Assets->FindSkeletalMesh(Key) : nullptr;
				const FMantleConvertedClip* Clip = Assets ? Assets->FindClip(Key) : nullptr;
				const FMantleConvertedHeightfield* Heightfield = Assets ? Assets->FindHeightfield(Key) : nullptr;
				if (Heightfield)
				{
					Ref.Hash = Heightfield->Hash;
					Ref.Bytes = Assets->UploadSize(Key, Heightfield->Bytes.Num());
				}
				else if (Mesh)
				{
					Ref.Hash = Mesh->Hash;
					Ref.Bytes = Assets->UploadSize(Key, Mesh->Bytes.Num());
				}
				else if (SkeletalMesh)
				{
					Ref.Hash = SkeletalMesh->Hash;
					Ref.Bytes = Assets->UploadSize(Key, SkeletalMesh->Bytes.Num());
				}
				else if (Clip)
				{
					Ref.Hash = Clip->Hash;
					Ref.Bytes = Assets->UploadSize(Key, Clip->Bytes.Num());
				}
				else if (Texture)
				{
					Ref.Hash = Texture->Hash;
					Ref.Bytes = Assets->UploadSize(Key, Texture->Bytes.Num());
				}
				else if (Sound)
				{
					Ref.Hash = Sound->Hash;
					Ref.Bytes = Assets->UploadSize(Key, Sound->Bytes.Num());
				}
				else if (const FSoftObjectPath* Source = Uploads.SourceOf.Find(Key))
				{
					Ref.Hash = ProvisionalHash(*Source, Ref.Bytes);
				}
				else
				{
					Ref.Hash = ProvisionalHash(FSoftObjectPath(), Ref.Bytes);
				}
			}
			if (!Listed.Contains(Ref.Id))
			{
				Listed.Add(Ref.Id);
				Place.Assets.Add(MoveTemp(Ref));
			}
		};
		for (const FString& Key : Uploads.Keys)
		{
			const EMantleAssetKind Kind = Uploads.KindOf.FindChecked(Key);
			AddUpload(Key, Kind);
			const FMantleConvertedMaterial* Material = Kind == EMantleAssetKind::Material && Assets ? Assets->FindMaterial(Key) : nullptr;
			if (Material)
			{
				TArray<FString> TextureKeys;
				Material->Document.Textures.GenerateValueArray(TextureKeys);
				TextureKeys.Sort();
				for (const FString& TextureKey : TextureKeys)
				{
					AddUpload(TextureKey, EMantleAssetKind::Texture);
				}
			}
			// A creator vehicle's materials (#435), with their textures: the client fetches them with it.
			const FMantleConvertedVehicle* Vehicle = Kind == EMantleAssetKind::Vehicle && Assets ? Assets->FindVehicle(Key) : nullptr;
			if (Vehicle)
			{
				TArray<FString> MaterialKeys;
				Vehicle->SectionMaterialKeys.GenerateValueArray(MaterialKeys);
				// Its attached parts' meshes and materials too (#435 item 2).
				for (const FMantleConvertedVehicle::FPart& Part : Vehicle->Parts)
				{
					AddUpload(Part.MeshKey, EMantleAssetKind::StaticMesh);
					for (const FString& PartMaterialKey : Part.MaterialKeys)
					{
						if (!PartMaterialKey.IsEmpty())
						{
							MaterialKeys.AddUnique(PartMaterialKey);
						}
					}
				}
				MaterialKeys.Sort();
				for (const FString& MaterialKey : MaterialKeys)
				{
					AddUpload(MaterialKey, EMantleAssetKind::Material);
					if (const FMantleConvertedMaterial* VehicleMaterial = Assets->FindMaterial(MaterialKey))
					{
						TArray<FString> TextureKeys;
						VehicleMaterial->Document.Textures.GenerateValueArray(TextureKeys);
						TextureKeys.Sort();
						for (const FString& TextureKey : TextureKeys)
						{
							AddUpload(TextureKey, EMantleAssetKind::Texture);
						}
					}
				}
				// And its liveries (#488), textures uploaded as a decal's are.
				for (const FString& LiveryKey : Vehicle->LiveryTextureKeys)
				{
					if (const FString* TextureKey = Assets->DecalTextures.Find(LiveryKey))
					{
						AddUpload(*TextureKey, EMantleAssetKind::Texture);
					}
				}
				// And its horn's sound (#546), uploaded as a SoundEmitter's is.
				if (!Vehicle->HornSoundKey.IsEmpty())
				{
					AddUpload(Vehicle->HornSoundKey, EMantleAssetKind::Sound);
				}
			}

			// A built weapon's 3D model and materials (#789), with their textures: the client fetches them with it.
			const FMantleConvertedWeapon* Weapon = Kind == EMantleAssetKind::Weapon && Assets ? Assets->FindWeapon(Key) : nullptr;
			if (Weapon)
			{
				AddUpload(Weapon->MeshKey, EMantleAssetKind::SkeletalMesh);
				TArray<FString> WeaponMaterialKeys;
				for (const FMantleConvertedWeapon::FSlot& Slot : Weapon->Slots)
				{
					if (!Slot.MaterialKey.IsEmpty())
					{
						WeaponMaterialKeys.AddUnique(Slot.MaterialKey);
					}
				}
				WeaponMaterialKeys.Sort();
				for (const FString& MaterialKey : WeaponMaterialKeys)
				{
					AddUpload(MaterialKey, EMantleAssetKind::Material);
					if (const FMantleConvertedMaterial* WeaponMaterial = Assets->FindMaterial(MaterialKey))
					{
						TArray<FString> TextureKeys;
						WeaponMaterial->Document.Textures.GenerateValueArray(TextureKeys);
						TextureKeys.Sort();
						for (const FString& TextureKey : TextureKeys)
						{
							AddUpload(TextureKey, EMantleAssetKind::Texture);
						}
					}
				}
			}
		}
		// Uploaded ids the level names as they are (#435): listed with a provisional digest, like a converted asset before its upload.
		TArray<FString> NamedIds;
		Uploads.NamedIds.GetKeys(NamedIds);
		NamedIds.Sort();
		for (const FString& Id : NamedIds)
		{
			if (!Listed.Contains(Id))
			{
				FMantleAssetRef Ref;
				Ref.Id = Id;
				Ref.Kind = Uploads.NamedIds.FindChecked(Id);
				Ref.Hash = ProvisionalHash(FSoftObjectPath(), Ref.Bytes);
				Listed.Add(Id);
				Place.Assets.Add(MoveTemp(Ref));
			}
		}
		Place.Assets.Sort([](const FMantleAssetRef& A, const FMantleAssetRef& B) { return A.Id < B.Id; });
	}

	Result.bSuccess = Result.Errors.Num() == 0;

	// #628: the level's sun, sky, fog, clouds and post-process, mapped onto what the place can say about its look, and
	// reported. Before the survey, which leaves those actors and the first sun to it.
	FMantleEnvironmentExport::Apply(World, IdByActor, Place, Result.Coverage);

	// GhzGarage/mantle#818: the place file's size and instance count against what the backend takes. Over a limit, the creator is
	// told with the fix (even thinning, with its numbers); the instances are removed only when thinning was asked for.
	{
		Result.InstanceTotal = FMantlePlaceInstancing::CountInstances(Place);
		Result.DrawDistanceEntities = FMantleDrawDistance::CountEntities(Place);
		if (!FMantleTargetFeatures::Current().Reads(MantleClientFeatures::DrawDistance) && FMantleDrawDistance::HasMeshEntities(Place))
		{
			Result.Coverage.Add(MantleCoverageCategory::StaticMeshes, EMantleCoverageOutcome::Approximated, TEXT("draw distances"),
				FString::Printf(TEXT("left out, so meshes are drawn from any distance: %s"), FMantleTargetFeatures::NeedsNewerClient));
		}
		Result.PlaceBytes = FMantlePlaceInstancing::PlaceBytes(Place);
		if (Result.InstanceTotal > FMantlePlaceLimits::MaxInstancesPerPlace || Result.PlaceBytes > FMantlePlaceLimits::MaxPlaceFileBytes)
		{
			const int64 InstancesBefore = Result.InstanceTotal;
			const int64 BytesBefore = Result.PlaceBytes;
			const FMantleThinReport Thin = FMantlePlaceInstancing::ThinToFit(
				Place, ThinFirstIds, FMantlePlaceLimits::MaxInstancesPerPlace, FMantlePlaceLimits::MaxPlaceFileBytes, Options.bThinInstances);
			if (Options.bThinInstances)
			{
				Result.Thinning = Thin;
				Result.InstanceTotal = Thin.InstancesAfter;
				Result.PlaceBytes = Thin.BytesAfter;
				UE_LOG(LogMantle, Log, TEXT("%s"), *Thin.Describe());
			}
			else
			{
				Result.ThinPreview = Thin;
			}
			if (!Options.bThinInstances || !Thin.bFits)
			{
				Result.bOverBudget = true;
				Result.BudgetProblem = FMantlePlaceInstancing::DescribeOverBudget(InstancesBefore, FMantlePlaceLimits::MaxInstancesPerPlace,
					BytesBefore, FMantlePlaceLimits::MaxPlaceFileBytes, Thin);
				Result.Errors.Add(FMantleParseError{ TEXT("/entities"), Result.BudgetProblem });
			}
		}
		Result.bSuccess = Result.Errors.Num() == 0;
	}

	// #628: the coverage report, last, errors or not: what was kept, approximated and dropped, by category.
	FMantleCoverageSurvey::Run(World, Result);

	UE_LOG(LogMantle, Log,
		TEXT("Exported '%s': %d entities, %d assets, %d problem(s)"),
		*Place.Metadata.Name,
		Place.Entities.Num(),
		Place.Assets.Num(),
		Result.Errors.Num());

	return Result;
}

FString FMantlePlaceExporter::ToJson(const FMantlePlace& Place)
{
	FString Output;
	const TSharedRef<FWriter> Writer = FWriterFactory::Create(&Output);

	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("schemaVersion"), Place.SchemaVersion);
	Writer->WriteValue(TEXT("placeId"), Place.PlaceId);

	Writer->WriteObjectStart(TEXT("revision"));
	Writer->WriteValue(TEXT("number"), Place.Revision.Number);
	if (!Place.Revision.Notes.IsEmpty())
	{
		Writer->WriteValue(TEXT("notes"), Place.Revision.Notes);
	}
	Writer->WriteValue(TEXT("state"), TEXT("draft"));
	Writer->WriteObjectEnd();

	Writer->WriteObjectStart(TEXT("metadata"));
	Writer->WriteValue(TEXT("name"), Place.Metadata.Name);
	if (!Place.Metadata.Description.IsEmpty())
	{
		Writer->WriteValue(TEXT("description"), Place.Metadata.Description);
	}
	Writer->WriteValue(TEXT("authorId"), Place.Metadata.AuthorId);
	if (!Place.Metadata.Thumbnail.IsEmpty())
	{
		Writer->WriteValue(TEXT("thumbnail"), Place.Metadata.Thumbnail);
	}
	Writer->WriteObjectEnd();

	Writer->WriteObjectStart(TEXT("settings"));
	Writer->WriteValue(TEXT("maxPlayers"), Place.Settings.MaxPlayers);
	Writer->WriteValue(TEXT("gravity"), Place.Settings.Gravity);
	Writer->WriteValue(TEXT("respawnMode"), Place.Settings.RespawnMode);
	// Only when opted in: clients from before #178 refuse unknown settings keys, so writing
	// the default would make every new place unreadable to them.
	if (Place.Settings.bAllowCommunityServers)
	{
		Writer->WriteValue(TEXT("allowCommunityServers"), true);
	}
	// The same for #416's opt-in: clients from before it refuse the key.
	if (Place.Settings.bAllowPrivateServers)
	{
		Writer->WriteValue(TEXT("allowPrivateServers"), true);
	}
	// Only when carjacking is off (#546): it is on by default, and clients from before it refuse the key.
	if (!Place.Settings.bCarjacking)
	{
		Writer->WriteValue(TEXT("carjacking"), false);
	}
	// The place's default look (#532), only when it sets something, and only the keys it sets:
	// clients from before it refuse the key, so a place that keeps the default look still reaches them.
	const FMantleEnvironmentSettings& Environment = Place.Settings.Environment;
	if (!Environment.IsEmpty())
	{
		Writer->WriteObjectStart(TEXT("environment"));
		if (Environment.ClockTime.IsSet())
		{
			Writer->WriteValue(TEXT("clockTime"), *Environment.ClockTime);
		}
		if (Environment.TimeScale.IsSet())
		{
			Writer->WriteValue(TEXT("timeScale"), *Environment.TimeScale);
		}
		if (Environment.Weather.IsSet())
		{
			Writer->WriteValue(TEXT("weather"), MantleWeatherStateName(*Environment.Weather));
		}
		if (Environment.ColorTint.IsSet())
		{
			Writer->WriteValue(TEXT("colorTint"), FMantleEnvironmentOverride::RgbToHex(*Environment.ColorTint));
		}
		if (Environment.Saturation.IsSet())
		{
			Writer->WriteValue(TEXT("saturation"), *Environment.Saturation);
		}
		if (Environment.Bloom.IsSet())
		{
			Writer->WriteValue(TEXT("bloom"), *Environment.Bloom);
		}
		if (Environment.ColorGrading.IsSet())
		{
			// Null for ColorGradingNone: no preset is the key left out.
			if (const TCHAR* GradingName = FMantleEnvironmentOverride::ColorGradingName(*Environment.ColorGrading))
			{
				Writer->WriteValue(TEXT("colorGrading"), GradingName);
			}
		}
		if (Environment.ColorGradingIntensity.IsSet())
		{
			Writer->WriteValue(TEXT("colorGradingIntensity"), *Environment.ColorGradingIntensity);
		}
		// The exposure and the height fog's look (#628): only the keys that differ from the game's own.
		if (Environment.Exposure.IsSet() && (Environment.Exposure->Mode == EMantleExposureMode::Auto || Environment.Exposure->Bias != 0.0))
		{
			const FMantleExposureSettings& Exposure = *Environment.Exposure;
			Writer->WriteObjectStart(TEXT("exposure"));
			if (Exposure.Mode == EMantleExposureMode::Auto)
			{
				Writer->WriteValue(TEXT("mode"), TEXT("auto"));
			}
			if (Exposure.Bias != 0.0)
			{
				Writer->WriteValue(TEXT("bias"), Exposure.Bias);
			}
			// The brightness range is read only with mode auto.
			if (Exposure.Mode == EMantleExposureMode::Auto && Exposure.MinBrightness.IsSet())
			{
				Writer->WriteValue(TEXT("minBrightness"), *Exposure.MinBrightness);
			}
			if (Exposure.Mode == EMantleExposureMode::Auto && Exposure.MaxBrightness.IsSet())
			{
				Writer->WriteValue(TEXT("maxBrightness"), *Exposure.MaxBrightness);
			}
			Writer->WriteObjectEnd();
		}
		if (Environment.Fog.IsSet() && !Environment.Fog->IsEmpty())
		{
			const FMantleFogSettings& Fog = *Environment.Fog;
			auto WriteLuminance = [&Writer](const TCHAR* Key, const FLinearColor& Value)
			{
				// Rounded, so a float's own digits (0.447 as 0.4469999969) are not written out.
				Writer->WriteArrayStart(Key);
				for (const float Channel : { Value.R, Value.G, Value.B })
				{
					Writer->WriteValue(FMath::RoundToDouble(static_cast<double>(Channel) * 10000.0) / 10000.0);
				}
				Writer->WriteArrayEnd();
			};
			Writer->WriteObjectStart(TEXT("fog"));
			if (Fog.Density.IsSet())
			{
				Writer->WriteValue(TEXT("density"), *Fog.Density);
			}
			if (Fog.HeightFalloff.IsSet())
			{
				Writer->WriteValue(TEXT("heightFalloff"), *Fog.HeightFalloff);
			}
			if (Fog.InscatteringColor.IsSet())
			{
				WriteLuminance(TEXT("inscatteringColor"), *Fog.InscatteringColor);
			}
			if (Fog.StartDistance.IsSet())
			{
				Writer->WriteValue(TEXT("startDistance"), *Fog.StartDistance);
			}
			if (Fog.DirectionalInscatteringColor.IsSet())
			{
				WriteLuminance(TEXT("directionalInscatteringColor"), *Fog.DirectionalInscatteringColor);
			}
			Writer->WriteObjectEnd();
		}
		// The sky light's scale and a sun that never reaches the place (#722): only when they differ from Mantle's own.
		if (Environment.SkyLightIntensity.IsSet())
		{
			Writer->WriteObjectStart(TEXT("skyLight"));
			Writer->WriteValue(TEXT("intensity"), *Environment.SkyLightIntensity);
			Writer->WriteObjectEnd();
		}
		if (Environment.bNoDirectSun)
		{
			Writer->WriteObjectStart(TEXT("sun"));
			Writer->WriteValue(TEXT("direct"), false);
			Writer->WriteObjectEnd();
		}
		Writer->WriteObjectEnd();
	}
	// The Water Zone (GhzGarage/mantle#997), only when the place has one: clients from before it refuse the key.
	if (Place.Settings.Water.IsSet())
	{
		const FMantleWaterZone& Zone = Place.Settings.Water.GetValue();
		Writer->WriteObjectStart(TEXT("water"));
		Writer->WriteArrayStart(TEXT("zoneExtent"));
		Writer->WriteValue(Zone.ZoneExtent.X);
		Writer->WriteValue(Zone.ZoneExtent.Y);
		Writer->WriteArrayEnd();
		WriteVector(Writer, TEXT("zoneLocation"), Zone.ZoneLocation);
		Writer->WriteValue(TEXT("zoneYawDeg"), Zone.ZoneYawDeg);
		Writer->WriteObjectEnd();
	}
	Writer->WriteObjectEnd();

	// The audience block (#320), only when it says something: clients from before it refuse
	// unknown top-level keys, so writing the default would make every new place unreadable to
	// them. Absent reads as unrated, every platform.
	if (!Place.Audience.IsDefault())
	{
		Writer->WriteObjectStart(TEXT("audience"));
		Writer->WriteValue(TEXT("maturity"), MantleMaturityToString(Place.Audience.Maturity));
		if (Place.Audience.Platforms.Num() > 0)
		{
			Writer->WriteArrayStart(TEXT("platforms"));
			for (const EMantlePlatform Platform : Place.Audience.Platforms)
			{
				Writer->WriteValue(MantlePlatformToString(Platform));
			}
			Writer->WriteArrayEnd();
		}
		Writer->WriteObjectEnd();
	}

	Writer->WriteArrayStart(TEXT("assets"));
	for (const FMantleAssetRef& Asset : Place.Assets)
	{
		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("id"), Asset.Id);
		Writer->WriteValue(TEXT("kind"), AssetKindName(Asset.Kind));
		Writer->WriteValue(TEXT("hash"), Asset.Hash);
		Writer->WriteValue(TEXT("bytes"), static_cast<double>(Asset.Bytes));
		Writer->WriteObjectEnd();
	}
	Writer->WriteArrayEnd();

	Writer->WriteArrayStart(TEXT("entities"));
	for (const FMantleEntity& Entity : Place.Entities)
	{
		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("id"), Entity.Id);
		Writer->WriteValue(TEXT("type"), MantleEntityTypeToString(Entity.Type));
		if (!Entity.Name.IsEmpty())
		{
			Writer->WriteValue(TEXT("name"), Entity.Name);
		}

		if (Entity.Parent.IsEmpty())
		{
			Writer->WriteNull(TEXT("parent"));
		}
		else
		{
			Writer->WriteValue(TEXT("parent"), Entity.Parent);
		}

		WriteTransform(Writer, Entity.Transform);

		Writer->WriteObjectStart(TEXT("properties"));
		// A StaticMesh's, or an InstancedMesh's (#467), mesh, materials, collision and shadow.
		if (const FMantleStaticMeshProps* P = MantleMeshPropsOf(Entity))
		{
			Writer->WriteValue(TEXT("mesh"), P->Mesh);
			Writer->WriteArrayStart(TEXT("materials"));
			for (const FString& Material : P->Materials)
			{
				if (Material.IsEmpty())
				{
					Writer->WriteNull(); // an unset slot (#262)
				}
				else
				{
					Writer->WriteValue(Material);
				}
			}
			Writer->WriteArrayEnd();
			Writer->WriteValue(TEXT("collision"), CollisionName(P->Collision));
			Writer->WriteValue(TEXT("castShadow"), P->bCastShadow);
			// GhzGarage/mantle#630: only above 0 (0 is never written: absent means no limit), and never without the drawDistance feature.
			if (P->DrawDistance > 0.0)
			{
				Writer->WriteValue(TEXT("drawDistance"), P->DrawDistance);
			}
			// #596: only when not default, so every other mesh still reaches a client that predates the
			// key (the backend gates it, SurfaceReleased), and never beside a physics block.
			if (P->Surface != EMantleSurface::Default && !P->Physics.IsSet())
			{
				Writer->WriteValue(TEXT("surface"), FString(MantleSurfaceToString(P->Surface)));
			}

			// A StaticMesh's physics block (#456); an absent mass is computed by the runtime.
			if (P->Physics.IsSet())
			{
				const FMantlePhysicsProps& Physics = P->Physics.GetValue();
				Writer->WriteObjectStart(TEXT("physics"));
				Writer->WriteValue(TEXT("simulate"), Physics.bSimulate);
				if (Physics.Mass.IsSet())
				{
					Writer->WriteValue(TEXT("mass"), Physics.Mass.GetValue());
				}
				Writer->WriteValue(TEXT("linearDamping"), Physics.LinearDamping);
				Writer->WriteValue(TEXT("angularDamping"), Physics.AngularDamping);
				Writer->WriteValue(TEXT("gravity"), Physics.bGravity);
				// #528: only when not default, so a physics block without one still reaches a client
				// that predates the key (the backend gates it, PhysicsMaterialReleased).
				if (Physics.Material != EMantlePhysicsMaterial::Default)
				{
					Writer->WriteValue(TEXT("material"), FString(MantlePhysicsMaterialToString(Physics.Material)));
				}
				Writer->WriteObjectEnd();
			}

			// One compact line per instance: the pretty printer would put each of the nine numbers
			// on its own indented line, several times the bytes, and a place's file is capped at 8 MiB.
			if (const FMantleInstancedMeshProps* Instanced = Entity.Props.TryGet<FMantleInstancedMeshProps>())
			{
				Writer->WriteArrayStart(TEXT("instances"));
				for (const FTransform& Instance : Instanced->Instances)
				{
					Writer->WriteRawJSONValue(FStringView(ExporterFormatInstance(Instance)));
				}
				Writer->WriteArrayEnd();
			}
		}
		else if (const FMantlePointLightProps* P2 = Entity.Props.TryGet<FMantlePointLightProps>())
		{
			Writer->WriteValue(TEXT("intensity"), P2->Intensity);
			WriteLightUnits(Writer, P2->IntensityUnits);
			WriteColor(Writer, TEXT("color"), P2->Color);
			Writer->WriteValue(TEXT("radius"), P2->Radius);
			Writer->WriteValue(TEXT("castShadow"), P2->bCastShadow);
			WriteLightTemperature(Writer, P2->bUseTemperature, P2->Temperature);
			WriteLightSourceShape(Writer, P2->Source);
		}
		else if (const FMantleSpotLightProps* P3 = Entity.Props.TryGet<FMantleSpotLightProps>())
		{
			Writer->WriteValue(TEXT("intensity"), P3->Intensity);
			WriteLightUnits(Writer, P3->IntensityUnits);
			WriteColor(Writer, TEXT("color"), P3->Color);
			Writer->WriteValue(TEXT("radius"), P3->Radius);
			Writer->WriteValue(TEXT("innerCone"), P3->InnerCone);
			Writer->WriteValue(TEXT("outerCone"), P3->OuterCone);
			Writer->WriteValue(TEXT("castShadow"), P3->bCastShadow);
			WriteLightTemperature(Writer, P3->bUseTemperature, P3->Temperature);
			WriteLightSourceShape(Writer, P3->Source);
		}
		else if (const FMantleRectLightProps* P8 = Entity.Props.TryGet<FMantleRectLightProps>())
		{
			// A new type (#472): every key ships with it, so its shape is always written.
			Writer->WriteValue(TEXT("intensity"), P8->Intensity);
			WriteLightUnits(Writer, P8->IntensityUnits);
			WriteColor(Writer, TEXT("color"), P8->Color);
			Writer->WriteValue(TEXT("radius"), P8->Radius);
			Writer->WriteValue(TEXT("sourceWidth"), P8->SourceWidth);
			Writer->WriteValue(TEXT("sourceHeight"), P8->SourceHeight);
			Writer->WriteValue(TEXT("barnDoorAngle"), P8->BarnDoorAngle);
			Writer->WriteValue(TEXT("barnDoorLength"), P8->BarnDoorLength);
			WriteLightTemperature(Writer, P8->bUseTemperature, P8->Temperature);
			Writer->WriteValue(TEXT("castShadow"), P8->bCastShadow);
		}
		else if (const FMantleDecalProps* P9 = Entity.Props.TryGet<FMantleDecalProps>())
		{
			Writer->WriteValue(TEXT("baseColor"), P9->BaseColor);
			if (!P9->Normal.IsEmpty())
			{
				Writer->WriteValue(TEXT("normal"), P9->Normal);
			}
			WriteVector(Writer, TEXT("size"), P9->Size);
			Writer->WriteValue(TEXT("opacity"), P9->Opacity);
			Writer->WriteValue(TEXT("sortOrder"), P9->SortOrder);
			Writer->WriteValue(TEXT("fadeScreenSize"), P9->FadeScreenSize);
		}
		else if (const FMantleDirectionalLightProps* P4 = Entity.Props.TryGet<FMantleDirectionalLightProps>())
		{
			Writer->WriteValue(TEXT("intensity"), P4->Intensity);
			WriteColor(Writer, TEXT("color"), P4->Color);
			Writer->WriteValue(TEXT("castShadow"), P4->bCastShadow);
		}
		else if (const FMantleTriggerVolumeProps* P5 = Entity.Props.TryGet<FMantleTriggerVolumeProps>())
		{
			Writer->WriteValue(TEXT("shape"), P5->Shape == EMantleTriggerShape::Sphere ? TEXT("sphere") : TEXT("box"));
			WriteVector(Writer, TEXT("extent"), P5->Extent);
		}
		else if (const FMantleSoundEmitterProps* P6 = Entity.Props.TryGet<FMantleSoundEmitterProps>())
		{
			Writer->WriteValue(TEXT("sound"), P6->Sound);
			Writer->WriteValue(TEXT("volume"), P6->Volume);
			Writer->WriteValue(TEXT("radius"), P6->Radius);
			Writer->WriteValue(TEXT("looping"), P6->bLooping);
			// #459: only when it is off, so a place that never turns it off reaches every client.
			if (!P6->bAutoplay)
			{
				Writer->WriteValue(TEXT("autoplay"), false);
			}
		}
		else if (const FMantleCrowdSpawnerProps* P7 = Entity.Props.TryGet<FMantleCrowdSpawnerProps>())
		{
			WriteCrowdSpawnerProperties(Writer, *P7);
		}
		else if (const FMantleSkeletalMeshProps* P10 = Entity.Props.TryGet<FMantleSkeletalMeshProps>())
		{
			// #469: the mesh and its slots; the clip and how it plays only when it plays one.
			Writer->WriteValue(TEXT("mesh"), P10->Mesh);
			Writer->WriteArrayStart(TEXT("materials"));
			for (const FString& Material : P10->Materials)
			{
				if (Material.IsEmpty())
				{
					Writer->WriteNull();
				}
				else
				{
					Writer->WriteValue(Material);
				}
			}
			Writer->WriteArrayEnd();
			Writer->WriteValue(TEXT("castShadow"), P10->bCastShadow);
			if (!P10->Clip.IsEmpty())
			{
				Writer->WriteValue(TEXT("clip"), P10->Clip);
				Writer->WriteValue(TEXT("loop"), P10->bLoop);
				Writer->WriteValue(TEXT("playRate"), P10->PlayRate);
				Writer->WriteValue(TEXT("startOffset"), P10->StartOffset);
			}
		}
		else if (const FMantleTerrainProps* P11 = Entity.Props.TryGet<FMantleTerrainProps>())
		{
			// A Terrain (#466): its heights, weights and layers. weightmap only when it has one.
			Writer->WriteValue(TEXT("heightfield"), P11->Heightfield);
			if (!P11->Weightmap.IsEmpty())
			{
				Writer->WriteValue(TEXT("weightmap"), P11->Weightmap);
			}
			if (!P11->Weightmap2.IsEmpty())
			{
				Writer->WriteValue(TEXT("weightmap2"), P11->Weightmap2);
			}
			Writer->WriteArrayStart(TEXT("layers"));
			for (const FMantleTerrainLayer& Layer : P11->Layers)
			{
				Writer->WriteObjectStart();
				Writer->WriteValue(TEXT("material"), Layer.Material);
				Writer->WriteValue(TEXT("tiling"), Layer.Tiling);
				// #596: only when not default, as on a mesh.
				if (Layer.Surface != EMantleSurface::Default)
				{
					Writer->WriteValue(TEXT("surface"), FString(MantleSurfaceToString(Layer.Surface)));
				}
				Writer->WriteObjectEnd();
			}
			Writer->WriteArrayEnd();
			Writer->WriteValue(TEXT("castShadow"), P11->bCastShadow);
			FMantleTerrainGrassExport::WriteJson(Writer, P11->Grass);
		}
		else if (const FMantleVehicleSpawnProps* P12 = Entity.Props.TryGet<FMantleVehicleSpawnProps>())
		{
			// A new type (#435): every key ships with it. A stored vehicle (#614 stage 4) names its storage entry
			// instead of an uploaded id; exactly one of the two is written, and only a place that uses storage
			// ever carries the new key.
			if (!P12->StorageItem.IsEmpty())
			{
				Writer->WriteValue(TEXT("storageItem"), P12->StorageItem);
			}
			else
			{
				Writer->WriteValue(TEXT("vehicle"), P12->Vehicle);
			}
			Writer->WriteValue(TEXT("respawnSeconds"), P12->RespawnSeconds);
			Writer->WriteValue(TEXT("enabled"), P12->bEnabled);
			// #488: only a picked livery; -1 (the vehicle's default) is the schema's default.
			if (P12->Livery >= 0)
			{
				Writer->WriteValue(TEXT("livery"), P12->Livery);
			}
		}
		else if (const FMantleWeaponSpawnProps* P12W = Entity.Props.TryGet<FMantleWeaponSpawnProps>())
		{
			// A new type (#789): every key ships with it. A stored weapon names its storage entry instead of an uploaded id;
			// exactly one of the two is written.
			if (!P12W->StorageItem.IsEmpty())
			{
				Writer->WriteValue(TEXT("storageItem"), P12W->StorageItem);
			}
			else
			{
				Writer->WriteValue(TEXT("weapon"), P12W->Weapon);
			}
			Writer->WriteValue(TEXT("respawnSeconds"), P12W->RespawnSeconds);
			Writer->WriteValue(TEXT("enabled"), P12W->bEnabled);
		}
		else if (const FMantleScriptPointProps* P12S = Entity.Props.TryGet<FMantleScriptPointProps>())
		{
			// A new type (#838): every key ships with it. Parameters are written sorted by key (case-sensitive), so a repeat
			// export of the same point is byte-identical.
			Writer->WriteValue(TEXT("storageItem"), P12S->StorageItem);
			TArray<FMantleScriptPointParameter> Sorted = P12S->Parameters;
			Sorted.Sort([](const FMantleScriptPointParameter& A, const FMantleScriptPointParameter& B)
			{
				return A.Name.Compare(B.Name, ESearchCase::CaseSensitive) < 0;
			});
			Writer->WriteObjectStart(TEXT("parameters"));
			for (const FMantleScriptPointParameter& Parameter : Sorted)
			{
				Writer->WriteValue(Parameter.Name, Parameter.Value);
			}
			Writer->WriteObjectEnd();
			Writer->WriteValue(TEXT("enabled"), P12S->bEnabled);
		}
		else if (const FMantleEffectProps* P13 = Entity.Props.TryGet<FMantleEffectProps>())
		{
			// A new type (#460): every key ships with it, so the whole look is always written.
			Writer->WriteValue(TEXT("archetype"), FMantleEffects::ArchetypeName(P13->Archetype));
			WriteColor(Writer, TEXT("color"), P13->Look.Color);
			if (P13->Archetype == EMantleEffectArchetype::Sprites)
			{
				// #628: the sprites keys in place of size, speed and lifetime; the texture only when it names one.
				const FMantleSpritesSettings& S = P13->Sprites;
				if (!P13->TextureId.IsEmpty())
				{
					Writer->WriteValue(TEXT("texture"), P13->TextureId);
				}
				Writer->WriteValue(TEXT("blend"), FString(FMantleEffects::BlendName(S.Blend)));
				WriteColor(Writer, TEXT("colorEnd"), S.ColorEnd);
				Writer->WriteValue(TEXT("alpha"), S.Alpha);
				Writer->WriteValue(TEXT("alphaEnd"), S.AlphaEnd);
				Writer->WriteValue(TEXT("fadeIn"), S.FadeIn);
				Writer->WriteValue(TEXT("sizeMin"), S.SizeMin);
				Writer->WriteValue(TEXT("sizeMax"), S.SizeMax);
				Writer->WriteValue(TEXT("sizeEndScale"), S.SizeEndScale);
				Writer->WriteValue(TEXT("rate"), P13->Look.Rate);
				Writer->WriteValue(TEXT("lifetimeMin"), S.LifetimeMin);
				Writer->WriteValue(TEXT("lifetimeMax"), S.LifetimeMax);
				WriteVector(Writer, TEXT("velocityMin"), S.VelocityMin);
				WriteVector(Writer, TEXT("velocityMax"), S.VelocityMax);
				WriteVector(Writer, TEXT("acceleration"), S.Acceleration);
				Writer->WriteObjectStart(TEXT("shape"));
				Writer->WriteValue(TEXT("type"), FString(FMantleEffects::ShapeName(S.Shape)));
				if (S.Shape == EMantleSpritesShape::Box)
				{
					WriteVector(Writer, TEXT("halfExtent"), S.BoxHalfExtent);
				}
				if (S.Shape == EMantleSpritesShape::Sphere)
				{
					Writer->WriteValue(TEXT("radius"), S.SphereRadius);
				}
				if (S.Shape == EMantleSpritesShape::Cylinder)
				{
					Writer->WriteValue(TEXT("radius"), S.CylinderRadius);
					Writer->WriteValue(TEXT("height"), S.CylinderHeight);
				}
				Writer->WriteObjectEnd();
				Writer->WriteValue(TEXT("maxParticles"), S.MaxParticles);
				// #628: a flipbook sheet only when there is one (the schema's default is the whole texture).
				if (S.SubImageColumns > 1 || S.SubImageRows > 1)
				{
					Writer->WriteArrayStart(TEXT("subImages"));
					Writer->WriteValue(S.SubImageColumns);
					Writer->WriteValue(S.SubImageRows);
					Writer->WriteArrayEnd();
					Writer->WriteValue(TEXT("subImageMode"), FString(FMantleEffects::SubImageModeName(S.SubImageMode)));
				}
			}
			else
			{
				Writer->WriteValue(TEXT("size"), P13->Look.Size);
				Writer->WriteValue(TEXT("rate"), P13->Look.Rate);
				Writer->WriteValue(TEXT("speed"), P13->Look.Speed);
				Writer->WriteValue(TEXT("lifetime"), P13->Look.Lifetime);
			}
			Writer->WriteValue(TEXT("autoplay"), P13->bAutoplay);
			Writer->WriteValue(TEXT("loop"), P13->bLoop);
		}
		else if (const FMantleScatterParams* P15 = Entity.Props.TryGet<FMantleScatterParams>())
		{
			// A new type (#530): every key ships with it, so every key is written, at full precision, so
			// the client generates exactly the instances the editor previewed. A mesh's materials only
			// when it names any.
			Writer->WriteValue(TEXT("seed"), P15->Seed);
			WriteVector(Writer, TEXT("halfExtent"), P15->HalfExtent);
			Writer->WriteValue(TEXT("density"), P15->Density);
			Writer->WriteArrayStart(TEXT("scale"));
			Writer->WriteValue(P15->MinScale);
			Writer->WriteValue(P15->MaxScale);
			Writer->WriteArrayEnd();
			Writer->WriteValue(TEXT("alignment"), FString(FMantleScatter::AlignmentName(P15->Alignment)));
			Writer->WriteValue(TEXT("maxTilt"), P15->MaxTiltDegrees);
			Writer->WriteArrayStart(TEXT("meshes"));
			for (const FMantleScatterMesh& Mesh : P15->Meshes)
			{
				Writer->WriteObjectStart();
				Writer->WriteValue(TEXT("mesh"), Mesh.MeshId);
				Writer->WriteValue(TEXT("weight"), Mesh.Weight);
				if (Mesh.Materials.Num() > 0)
				{
					Writer->WriteArrayStart(TEXT("materials"));
					for (const FString& Material : Mesh.Materials)
					{
						if (Material.IsEmpty())
						{
							Writer->WriteNull(); // an unset slot (#262)
						}
						else
						{
							Writer->WriteValue(Material);
						}
					}
					Writer->WriteArrayEnd();
				}
				Writer->WriteObjectEnd();
			}
			Writer->WriteArrayEnd();
		}
		else if (const FMantleReflectionProbeProps* P16 = Entity.Props.TryGet<FMantleReflectionProbeProps>())
		{
			// A new type (#628): its keys ship with it, so each is written. A box's own keys only for a box, a sphere's for a
			// sphere, as the schema holds them.
			Writer->WriteValue(TEXT("shape"), FString(MantleReflectionProbeShapeToString(P16->Shape)));
			if (P16->Shape == EMantleReflectionProbeShape::Box)
			{
				WriteVector(Writer, TEXT("halfExtent"), P16->HalfExtent);
				Writer->WriteValue(TEXT("transitionDistance"), P16->TransitionDistance);
			}
			else
			{
				Writer->WriteValue(TEXT("influenceRadius"), P16->InfluenceRadius);
			}
			Writer->WriteValue(TEXT("brightness"), P16->Brightness);
			Writer->WriteValue(TEXT("skylightScale"), P16->SkylightScale);
		}
		else if (const FMantleWaterBodyProps* P21 = Entity.Props.TryGet<FMantleWaterBodyProps>())
		{
			// A new type (GhzGarage/mantle#997): its keys ship with it. A custom body writes its mesh, the others their spline; a river no waves.
			Writer->WriteValue(TEXT("bodyType"), FString(MantleWaterBodyTypeToString(P21->BodyType)));
			if (P21->BodyType == EMantleWaterBodyType::Custom)
			{
				Writer->WriteValue(TEXT("mesh"), P21->MeshId);
			}
			else
			{
				Writer->WriteObjectStart(TEXT("spline"));
				Writer->WriteArrayStart(TEXT("points"));
				for (const FMantleWaterSplinePoint& Point : P21->SplinePoints)
				{
					Writer->WriteObjectStart();
					WriteVector(Writer, TEXT("location"), Point.Location);
					WriteVector(Writer, TEXT("arriveTangent"), Point.ArriveTangent);
					WriteVector(Writer, TEXT("leaveTangent"), Point.LeaveTangent);
					Writer->WriteValue(TEXT("depth"), Point.Depth);
					if (P21->BodyType == EMantleWaterBodyType::River)
					{
						Writer->WriteValue(TEXT("width"), Point.Width);
						Writer->WriteValue(TEXT("velocity"), Point.Velocity);
					}
					if (Point.Interp != EMantleWaterSplinePointType::Curve)
					{
						Writer->WriteValue(TEXT("interp"), FString(MantleWaterSplinePointTypeToString(Point.Interp)));
					}
					Writer->WriteObjectEnd();
				}
				Writer->WriteArrayEnd();
				Writer->WriteObjectEnd();
			}
			if (P21->Waves.Num() > 0)
			{
				Writer->WriteObjectStart(TEXT("waves"));
				Writer->WriteArrayStart(TEXT("list"));
				for (const FMantleWaterWave& Wave : P21->Waves)
				{
					Writer->WriteObjectStart();
					Writer->WriteValue(TEXT("wavelength"), Wave.Wavelength);
					Writer->WriteValue(TEXT("amplitude"), Wave.Amplitude);
					Writer->WriteValue(TEXT("steepness"), Wave.Steepness);
					Writer->WriteValue(TEXT("directionDeg"), Wave.DirectionDeg);
					if (!FMath::IsNearlyZero(Wave.PhaseOffset))
					{
						Writer->WriteValue(TEXT("phaseOffset"), Wave.PhaseOffset);
					}
					Writer->WriteObjectEnd();
				}
				Writer->WriteArrayEnd();
				Writer->WriteObjectEnd();
			}
			if (P21->Albedo.IsSet() || P21->Scattering.IsSet() || P21->Absorption.IsSet())
			{
				Writer->WriteObjectStart(TEXT("look"));
				if (P21->Albedo.IsSet()) { WriteColor(Writer, TEXT("albedo"), *P21->Albedo); }
				if (P21->Scattering.IsSet()) { WriteColor(Writer, TEXT("scattering"), *P21->Scattering); }
				if (P21->Absorption.IsSet()) { WriteColor(Writer, TEXT("absorption"), *P21->Absorption); }
				Writer->WriteObjectEnd();
			}
			Writer->WriteValue(TEXT("collision"), P21->bCollision);
			if (P21->CollisionHeightOffset.IsSet())
			{
				Writer->WriteValue(TEXT("collisionHeightOffset"), *P21->CollisionHeightOffset);
			}
			if (P21->CollisionExtent.IsSet())
			{
				WriteVector(Writer, TEXT("collisionExtent"), *P21->CollisionExtent);
			}
			if (P21->OceanExtent.IsSet())
			{
				Writer->WriteArrayStart(TEXT("oceanExtent"));
				Writer->WriteValue(P21->OceanExtent->X);
				Writer->WriteValue(P21->OceanExtent->Y);
				Writer->WriteArrayEnd();
			}
			if (P21->CenterOnZone.IsSet())
			{
				Writer->WriteValue(TEXT("centerOnZone"), *P21->CenterOnZone);
			}
		}
		else if (const FMantleTextProps* P20 = Entity.Props.TryGet<FMantleTextProps>())
		{
			// A new type (GhzGarage/mantle#724): its keys ship with it, so each is written. The colour is linear RGBA.
			Writer->WriteValue(TEXT("text"), P20->Text);
			Writer->WriteArrayStart(TEXT("color"));
			Writer->WriteValue(static_cast<double>(P20->Color.R));
			Writer->WriteValue(static_cast<double>(P20->Color.G));
			Writer->WriteValue(static_cast<double>(P20->Color.B));
			Writer->WriteValue(static_cast<double>(P20->Color.A));
			Writer->WriteArrayEnd();
			Writer->WriteValue(TEXT("worldSize"), P20->WorldSize);
			Writer->WriteValue(TEXT("horizontalAlignment"), FString(MantleTextHorizontalAlignmentToString(P20->HorizontalAlignment)));
			Writer->WriteValue(TEXT("verticalAlignment"), FString(MantleTextVerticalAlignmentToString(P20->VerticalAlignment)));
			Writer->WriteValue(TEXT("xScale"), P20->XScale);
			Writer->WriteValue(TEXT("yScale"), P20->YScale);
		}
		else if (const FMantleConstraintProps* P14 = Entity.Props.TryGet<FMantleConstraintProps>())
		{
			// A new type (#528): its keys ship with it. A hinge's and a rope's own keys only for that
			// type, as the schema holds them; the optional ones only when set.
			Writer->WriteValue(TEXT("type"), MantleConstraintTypeToString(P14->Type));
			Writer->WriteValue(TEXT("entityA"), P14->EntityA);
			if (!P14->EntityB.IsEmpty())
			{
				Writer->WriteValue(TEXT("entityB"), P14->EntityB);
			}
			WriteVector(Writer, TEXT("anchorA"), P14->AnchorA);
			if (P14->AnchorB.IsSet())
			{
				WriteVector(Writer, TEXT("anchorB"), P14->AnchorB.GetValue());
			}
			if (P14->Type == EMantleConstraintType::Hinge)
			{
				WriteVector(Writer, TEXT("axis"), P14->Axis);
				if (P14->bLimited)
				{
					Writer->WriteArrayStart(TEXT("limits"));
					Writer->WriteValue(P14->MinAngle);
					Writer->WriteValue(P14->MaxAngle);
					Writer->WriteArrayEnd();
				}
			}
			if (P14->Type == EMantleConstraintType::Rope && P14->Length.IsSet())
			{
				Writer->WriteValue(TEXT("length"), P14->Length.GetValue());
			}
			if (P14->BreakForce.IsSet())
			{
				Writer->WriteValue(TEXT("breakForce"), P14->BreakForce.GetValue());
			}
		}
		Writer->WriteObjectEnd();

		if (Entity.Tags.Num() > 0)
		{
			Writer->WriteArrayStart(TEXT("tags"));
			for (const FString& Tag : Entity.Tags)
			{
				Writer->WriteValue(Tag);
			}
			Writer->WriteArrayEnd();
		}

		if (Entity.bDynamic)
		{
			Writer->WriteValue(TEXT("dynamic"), true);
		}

		Writer->WriteObjectEnd();
	}
	Writer->WriteArrayEnd();

	if (Place.Scripts.Num() > 0)
	{
		Writer->WriteArrayStart(TEXT("scripts"));
		for (const FMantleScript& Script : Place.Scripts)
		{
			Writer->WriteObjectStart();
			Writer->WriteValue(TEXT("id"), Script.Id);
			Writer->WriteValue(TEXT("name"), Script.Name);
			Writer->WriteValue(TEXT("path"), Script.Path);
			Writer->WriteValue(TEXT("side"), MantleScriptSideToString(Script.Side));
			Writer->WriteValue(TEXT("language"), MantleScriptLanguageToString(Script.Language));
			if (Script.bAuthoredAsGraph)
			{
				Writer->WriteValue(TEXT("authoredAs"), TEXT("graph"));
			}
			Writer->WriteValue(TEXT("source"), Script.Source);
			if (!Script.AuthorSource.IsEmpty())
			{
				Writer->WriteValue(TEXT("authorSource"), Script.AuthorSource);
			}
			Writer->WriteObjectEnd();
		}
		Writer->WriteArrayEnd();
	}

	// #838: also a place with no scripts of its own that runs script packages records the version they need (Export sets
	// ScriptApi then); a place with neither leaves Major at 0 and stays byte-identical.
	if (Place.Scripts.Num() > 0 || Place.ScriptApi.Major > 0)
	{
		Writer->WriteObjectStart(TEXT("scriptApi"));
		Writer->WriteValue(TEXT("major"), Place.ScriptApi.Major);
		Writer->WriteValue(TEXT("minor"), Place.ScriptApi.Minor);
		Writer->WriteObjectEnd();
	}

	// #549: only when the level has UI files, so a place without them stays byte-identical and
	// is never sent a key an older client refuses.
	if (Place.UIDocuments.Num() > 0)
	{
		Writer->WriteArrayStart(TEXT("uiDocuments"));
		for (const FMantleUIDocument& Document : Place.UIDocuments)
		{
			Writer->WriteObjectStart();
			Writer->WriteValue(TEXT("id"), Document.Id);
			Writer->WriteValue(TEXT("path"), Document.Path);
			// #556 item 5: an image names its Texture asset instead of a source.
			if (Document.IsImage())
			{
				Writer->WriteValue(TEXT("asset"), Document.Asset);
			}
			else
			{
				Writer->WriteValue(TEXT("source"), Document.Source);
			}
			Writer->WriteObjectEnd();
		}
		Writer->WriteArrayEnd();
	}

	// #614 stage 4: only when the place stores library items, so a place without them stays byte-identical
	// and is never sent a key an older client refuses.
	if (Place.Storage.Num() > 0)
	{
		Writer->WriteArrayStart(TEXT("storage"));
		for (const FMantleStorageEntry& Entry : Place.Storage)
		{
			Writer->WriteObjectStart();
			Writer->WriteValue(TEXT("name"), Entry.Name);
			Writer->WriteValue(TEXT("item"), Entry.Item);
			Writer->WriteValue(TEXT("version"), Entry.Version);
			// #838 stage 6: only an entry with settings writes the key, keys sorted (case-sensitively) so a file is stable.
			if (Entry.Settings.Num() > 0)
			{
				TArray<FString> Keys;
				Entry.Settings.GetKeys(Keys);
				Keys.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::CaseSensitive) < 0; });
				Writer->WriteObjectStart(TEXT("settings"));
				for (const FString& Key : Keys)
				{
					Writer->WriteValue(Key, Entry.Settings.FindChecked(Key));
				}
				Writer->WriteObjectEnd();
			}
			Writer->WriteObjectEnd();
		}
		Writer->WriteArrayEnd();
	}

	// GhzGarage/MantleCore#142: only when the level has seats, so a place without them stays byte-identical and is
	// never sent a key an older client refuses.
	if (Place.Seats.Num() > 0)
	{
		Writer->WriteArrayStart(TEXT("seats"));
		for (const FMantleSeat& Seat : Place.Seats)
		{
			Writer->WriteObjectStart();
			Writer->WriteValue(TEXT("entity"), Seat.Entity);
			Writer->WriteArrayStart(TEXT("position"));
			Writer->WriteValue(Seat.Position.X);
			Writer->WriteValue(Seat.Position.Y);
			Writer->WriteValue(Seat.Position.Z);
			Writer->WriteArrayEnd();
			Writer->WriteValue(TEXT("yaw"), Seat.Yaw);
			Writer->WriteValue(TEXT("style"), FString(MantleSeatStyleToString(Seat.Style)));
			Writer->WriteObjectEnd();
		}
		Writer->WriteArrayEnd();
	}

	Writer->WriteObjectEnd();
	Writer->Close();

	return Output;
}

namespace
{
	/** The folder named Folder beside World's level package on disk, or empty (GetScriptsFolder's rules). */
	FString ExporterLevelSiblingFolder(UWorld* World, const TCHAR* Folder)
	{
		if (!World)
		{
			return FString();
		}

		const UPackage* Package = World->GetOutermost();
		if (!Package)
		{
			return FString();
		}

		const FString PackageName = Package->GetName();
		if (!PackageName.StartsWith(TEXT("/Game/")))
		{
			// A scratch/transient world (every other exporter test, and a level that has never
			// been saved) has no Content-relative folder to look beside -- there is simply no
			// Scripts/ (or UI/) folder for it, exactly as if the level carried none.
			return FString();
		}

		FString Filename;
		if (!FPackageName::TryConvertLongPackageNameToFilename(
				PackageName, Filename, FPackageName::GetMapPackageExtension()))
		{
			return FString();
		}

		const FString Candidate = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::GetPath(Filename), Folder));
		return IFileManager::Get().DirectoryExists(*Candidate) ? Candidate : FString();
	}
}

FString FMantlePlaceExporter::GetScriptsFolder(UWorld* World)
{
	return ExporterLevelSiblingFolder(World, TEXT("Scripts"));
}

FString FMantlePlaceExporter::GetUIFolder(UWorld* World)
{
	return ExporterLevelSiblingFolder(World, TEXT("UI"));
}

FString FMantlePlaceExporter::GetPackagesFolder(UWorld* World)
{
	return ExporterLevelSiblingFolder(World, TEXT("Packages"));
}

void FMantlePlaceExporter::CollectUIDocumentFiles(
	const FString& UIFolder,
	TArray<FMantleUIDocumentFile>& OutFiles,
	TArray<FMantleUIImageFile>& OutImages)
{
	OutFiles.Reset();
	OutImages.Reset();
	if (UIFolder.IsEmpty())
	{
		return;
	}

	TArray<FString> Found;
	IFileManager::Get().FindFilesRecursive(Found, *UIFolder, TEXT("*.*"), true, false);
	for (const FString& File : Found)
	{
		FString Relative = File;
		FPaths::MakePathRelativeTo(Relative, *(UIFolder / TEXT("")));
		Relative.ReplaceInline(TEXT("\\"), TEXT("/"));
		const FString CleanName = FPaths::GetCleanFilename(Relative);
		if (CleanName.IsEmpty() || CleanName.StartsWith(TEXT(".")))
		{
			continue;
		}

		const FString PlacePath = TEXT("UI/") + Relative;
		const FString Extension = FPaths::GetExtension(Relative);
		if (Extension.Equals(TEXT("png"), ESearchCase::IgnoreCase) || Extension.Equals(TEXT("jpg"), ESearchCase::IgnoreCase)
			|| Extension.Equals(TEXT("jpeg"), ESearchCase::IgnoreCase))
		{
			// #556 item 5: an image a document shows. Only its first bytes are read here, for the format.
			FMantleUIImageFile& Image = OutImages.AddDefaulted_GetRef();
			Image.Path = PlacePath;
			Image.DiskPath = FPaths::ConvertRelativePathToFull(File);
			Image.Bytes = IFileManager::Get().FileSize(*File);
			TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*File, FILEREAD_Silent));
			if (Reader.IsValid())
			{
				uint8 Head[8] = {};
				const int64 HeadBytes = FMath::Min<int64>(Reader->TotalSize(), UE_ARRAY_COUNT(Head));
				Reader->Serialize(Head, HeadBytes);
				Image.Format = SniffUIImageFormat(TConstArrayView<uint8>(Head, static_cast<int32>(HeadBytes)));
			}
			continue;
		}
		const bool bDocument = Extension.Equals(TEXT("rml"), ESearchCase::IgnoreCase);
		if (!bDocument && !Extension.Equals(TEXT("rcss"), ESearchCase::IgnoreCase))
		{
			continue; // anything else under UI/ is the creator's own business
		}

		FMantleUIDocumentFile& Out = OutFiles.AddDefaulted_GetRef();
		Out.Path = PlacePath;
		Out.Language = bDocument ? TEXT("rml") : TEXT("rcss");
		Out.Bytes = IFileManager::Get().FileSize(*File);
		if (Out.Bytes >= 0 && Out.Bytes <= FMantlePlaceLimits::MaxUIDocumentBytes)
		{
			FFileHelper::LoadFileToString(Out.Text, *File);
		}
	}

	OutFiles.Sort([](const FMantleUIDocumentFile& A, const FMantleUIDocumentFile& B)
	{
		return A.Path.Compare(B.Path, ESearchCase::CaseSensitive) < 0;
	});
	OutImages.Sort([](const FMantleUIImageFile& A, const FMantleUIImageFile& B)
	{
		return A.Path.Compare(B.Path, ESearchCase::CaseSensitive) < 0;
	});
}

FString FMantlePlaceExporter::SniffUIImageFormat(TConstArrayView<uint8> Head)
{
	static const uint8 PngSignature[8] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
	if (Head.Num() >= 8 && FMemory::Memcmp(Head.GetData(), PngSignature, 8) == 0)
	{
		return TEXT("png");
	}
	// Every JPEG starts with a start-of-image marker followed by another marker.
	if (Head.Num() >= 3 && Head[0] == 0xFF && Head[1] == 0xD8 && Head[2] == 0xFF)
	{
		return TEXT("jpeg");
	}
	return FString();
}

TArray<FMantleUIDocumentProblem> FMantlePlaceExporter::CheckUIImageFiles(const TArray<FMantleUIImageFile>& Images, const TArray<FMantleUIDocumentFile>& Files)
{
	TArray<FMantleUIDocumentProblem> Problems;
	if (Images.Num() > FMantlePlaceLimits::MaxUIImages)
	{
		Problems.Add({ FString(),
			FString::Printf(TEXT("UI/ holds %d images; a place can have at most %d."), Images.Num(), FMantlePlaceLimits::MaxUIImages),
			TEXT("Combine icons into fewer images, or remove the ones this place doesn't use.") });
	}
	TMap<FString, FString> FirstPathByLowercase;
	for (const FMantleUIDocumentFile& File : Files)
	{
		FirstPathByLowercase.Add(File.Path.ToLower(), File.Path);
	}
	for (const FMantleUIImageFile& Image : Images)
	{
		if (Image.Path.Len() > FMantlePlaceLimits::MaxUIDocumentPathLength)
		{
			Problems.Add({ Image.Path,
				FString::Printf(TEXT("%s is %d characters long; a UI file's path can be at most %d."), *Image.Path, Image.Path.Len(),
					FMantlePlaceLimits::MaxUIDocumentPathLength),
				TEXT("Shorten its name or its folders.") });
		}
		else if (!FMantlePlaceLimits::IsUIImagePath(Image.Path))
		{
			const FString Extension = FPaths::GetExtension(Image.Path);
			const bool bExtension = !Extension.Equals(TEXT("png"), ESearchCase::CaseSensitive) && !Extension.Equals(TEXT("jpg"), ESearchCase::CaseSensitive);
			Problems.Add({ Image.Path,
				bExtension
					? FString::Printf(TEXT("%s must end in .png or .jpg, in lower case."), *Image.Path)
					: FString::Printf(TEXT("%s has a name Mantle can't publish: names and folders may use only letters, digits, '_', '.' and '-', with no '..'."), *Image.Path),
				bExtension ? FString(TEXT("Rename the file's extension to .png or .jpg (a .jpeg file is a .jpg).")) : FString(TEXT("Rename the file or its folders.")) });
		}
		if (Image.Bytes < 0)
		{
			Problems.Add({ Image.Path, FString::Printf(TEXT("%s could not be read."), *Image.Path), TEXT("Check the file still exists and is not open elsewhere.") });
		}
		else if (Image.Bytes > FMantlePlaceLimits::MaxUIImageBytes)
		{
			Problems.Add({ Image.Path,
				FString::Printf(TEXT("%s is %lld KiB; a UI image can be at most %d KiB."), *Image.Path, Image.Bytes / 1024,
					FMantlePlaceLimits::MaxUIImageBytes / 1024),
				TEXT("Save it smaller: fewer pixels, or JPEG for a photo.") });
		}
		else if (Image.Format.IsEmpty())
		{
			Problems.Add({ Image.Path,
				FString::Printf(TEXT("%s is not a PNG or JPEG file, whatever its extension says."), *Image.Path),
				TEXT("Export it again as PNG or JPEG.") });
		}
		const FString Lowercase = Image.Path.ToLower();
		if (const FString* First = FirstPathByLowercase.Find(Lowercase))
		{
			Problems.Add({ Image.Path,
				FString::Printf(TEXT("%s differs from %s only in case; UI file paths must differ by more than case."), *Image.Path, **First),
				TEXT("Rename one of them.") });
		}
		else
		{
			FirstPathByLowercase.Add(Lowercase, Image.Path);
		}
	}
	return Problems;
}

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantlePlaceExporterUIImages
{
	bool IsUIReferenceCharacter(const TCHAR Character)
	{
		return FMantlePlaceLimits::IsScriptPathCharacter(Character) || Character == TEXT('/');
	}

	/**
	 * Reference, as written in the UI file at FromPath, joined the way the game joins it
	 * (MantleRml::JoinVirtualPath): from the file's folder, or from the place's root with a leading
	 * '/'. "." and ".." collapse; empty when it climbs above the root.
	 */
	FString ResolveUIReference(const FString& FromPath, const FString& Reference)
	{
		FString Base;
		FString Relative = Reference;
		if (Relative.StartsWith(TEXT("/"), ESearchCase::CaseSensitive))
		{
			Relative.RightChopInline(1);
		}
		else
		{
			int32 LastSlash = INDEX_NONE;
			if (FromPath.FindLastChar(TEXT('/'), LastSlash))
			{
				Base = FromPath.Left(LastSlash + 1);
			}
		}
		TArray<FString> Parts;
		(Base + Relative).ParseIntoArray(Parts, TEXT("/"), true);
		TArray<FString> Collapsed;
		for (const FString& Part : Parts)
		{
			if (Part.Equals(TEXT("."), ESearchCase::CaseSensitive))
			{
				continue;
			}
			if (Part.Equals(TEXT(".."), ESearchCase::CaseSensitive))
			{
				if (Collapsed.IsEmpty())
				{
					return FString();
				}
				Collapsed.RemoveAt(Collapsed.Num() - 1);
				continue;
			}
			Collapsed.Add(Part);
		}
		return FString::Join(Collapsed, TEXT("/"));
	}
}

TArray<FMantleUIDocumentProblem> FMantlePlaceExporter::CheckUIImageReferences(const TArray<FMantleUIDocumentFile>& Files, const TArray<FMantleUIImageFile>& Images)
{
	using namespace MantlePlaceExporterUIImages;
	TArray<FMantleUIDocumentProblem> Warnings;
	for (const FMantleUIDocumentFile& File : Files)
	{
		const FString& Text = File.Text;
		TSet<FString> Reported;
		for (const TCHAR* Extension : { TEXT(".png"), TEXT(".jpg"), TEXT(".jpeg") })
		{
			const int32 ExtensionLength = FCString::Strlen(Extension);
			int32 From = 0;
			while (true)
			{
				const int32 At = Text.Find(Extension, ESearchCase::IgnoreCase, ESearchDir::FromStart, From);
				if (At == INDEX_NONE)
				{
					break;
				}
				From = At + ExtensionLength;
				// A whole path: no name character right after the extension ("a.pngx" names no image).
				if (From < Text.Len() && FMantlePlaceLimits::IsScriptPathCharacter(Text[From]))
				{
					continue;
				}
				int32 Start = At;
				while (Start > 0 && IsUIReferenceCharacter(Text[Start - 1]))
				{
					--Start;
				}
				// Nothing before the extension: a name a data expression builds ('...' + '.png').
				if (Start == At || Text[At - 1] == TEXT('/'))
				{
					continue;
				}
				const FString Reference = Text.Mid(Start, From - Start);
				if (Reported.Contains(Reference))
				{
					continue;
				}
				Reported.Add(Reference);
				const FString Resolved = ResolveUIReference(File.Path, Reference);
				const bool bFound = Images.ContainsByPredicate([&Resolved](const FMantleUIImageFile& Image)
				{
					return Image.Path.Equals(Resolved, ESearchCase::CaseSensitive);
				});
				if (bFound)
				{
					continue;
				}
				const FMantleUIImageFile* CaseOnly = Resolved.IsEmpty() ? nullptr : Images.FindByPredicate([&Resolved](const FMantleUIImageFile& Image)
				{
					return Image.Path.Equals(Resolved, ESearchCase::IgnoreCase);
				});
				FString Problem;
				if (CaseOnly)
				{
					Problem = FString::Printf(TEXT("%s shows %s, which is %s on disk; a place's paths are case-sensitive, so match the case exactly."),
						*File.Path, *Reference, *CaseOnly->Path);
				}
				else if (Resolved.IsEmpty())
				{
					Problem = FString::Printf(TEXT("%s shows %s, which climbs out of the place's UI files; it will not draw."), *File.Path, *Reference);
				}
				else
				{
					Problem = FString::Printf(TEXT("%s shows %s, but there is no %s in the level's UI/ folder; it will not draw."),
						*File.Path, *Reference, *Resolved);
				}
				Warnings.Add({ File.Path, MoveTemp(Problem), TEXT("Add the image under UI/, or fix the path (relative to the file that uses it).") });
			}
		}
	}
	return Warnings;
}

TArray<FMantleUIDocumentProblem> FMantlePlaceExporter::CheckUIDocumentFiles(const TArray<FMantleUIDocumentFile>& Files)
{
	TArray<FMantleUIDocumentProblem> Problems;
	if (Files.Num() > FMantlePlaceLimits::MaxUIDocuments)
	{
		Problems.Add({ FString(),
			FString::Printf(TEXT("UI/ holds %d .rml and .rcss files; a place can have at most %d."), Files.Num(), FMantlePlaceLimits::MaxUIDocuments),
			TEXT("Combine documents or stylesheets, or remove the ones this place doesn't use.") });
	}
	TMap<FString, FString> FirstPathByLowercase;
	for (const FMantleUIDocumentFile& File : Files)
	{
		if (File.Path.Len() > FMantlePlaceLimits::MaxUIDocumentPathLength)
		{
			Problems.Add({ File.Path,
				FString::Printf(TEXT("%s is %d characters long; a UI file's path can be at most %d."), *File.Path, File.Path.Len(),
					FMantlePlaceLimits::MaxUIDocumentPathLength),
				TEXT("Shorten its name or its folders.") });
		}
		else if (!FMantlePlaceLimits::IsUIDocumentPath(File.Path))
		{
			const FString Extension = FPaths::GetExtension(File.Path);
			const bool bExtensionCase = !Extension.Equals(TEXT("rml"), ESearchCase::CaseSensitive)
				&& !Extension.Equals(TEXT("rcss"), ESearchCase::CaseSensitive);
			Problems.Add({ File.Path,
				bExtensionCase
					? FString::Printf(TEXT("%s must end in .rml or .rcss, in lower case."), *File.Path)
					: FString::Printf(TEXT("%s has a name Mantle can't publish: names and folders may use only letters, digits, '_', '.' and '-', with no '..'."), *File.Path),
				bExtensionCase ? FString(TEXT("Rename the file's extension to lower case.")) : FString(TEXT("Rename the file or its folders.")) });
		}
		if (File.Bytes > FMantlePlaceLimits::MaxUIDocumentBytes)
		{
			Problems.Add({ File.Path,
				FString::Printf(TEXT("%s is %lld KiB; a UI file can be at most %d KiB."), *File.Path, File.Bytes / 1024,
					FMantlePlaceLimits::MaxUIDocumentBytes / 1024),
				TEXT("Split it into smaller documents or stylesheets.") });
		}
		const FString Lowercase = File.Path.ToLower();
		if (const FString* First = FirstPathByLowercase.Find(Lowercase))
		{
			Problems.Add({ File.Path,
				FString::Printf(TEXT("%s differs from %s only in case; UI file paths must differ by more than case."), *File.Path, **First),
				TEXT("Rename one of them.") });
		}
		else
		{
			FirstPathByLowercase.Add(Lowercase, File.Path);
		}
	}
	return Problems;
}

TArray<FMantleUIDocumentProblem> FMantlePlaceExporter::CheckUIDocumentContent(
	const TArray<FMantleUIDocumentFile>& Files,
	TArray<FMantleUIDocumentProblem>* OutWarnings)
{
	TArray<FMantleUIDocumentProblem> Problems;
	const TCHAR* const Refused = TEXT("Players' games refuse to open it until this is fixed.");
	const TCHAR* const Opens = TEXT("Players' games still open it.");
	for (const FMantleUIDocumentFile& File : Files)
	{
		// A file over the limit was never read, and CheckUIDocumentFiles already refuses it.
		if (File.Bytes > FMantlePlaceLimits::MaxUIDocumentBytes)
		{
			continue;
		}
		// The game's own scans, one file at a time so each file gets its own verdict. The game also
		// limits a document's load time and element count when it opens; those show in Test locally.
		TMap<FString, FString> Single;
		Single.Add(File.Path, File.Text);
		FString ScanError;
		const bool bScanned = MantleRmlDocumentRules::CheckFileSet(Single, ScanError);
		if (!bScanned)
		{
			ScanError.TrimEndInline();
			if (!ScanError.EndsWith(TEXT("."), ESearchCase::CaseSensitive))
			{
				ScanError += TEXT(".");
			}
			Problems.Add({ File.Path, ScanError, Refused });
		}
		if (!OutWarnings)
		{
			continue;
		}
		TArray<FString> Undrawn;
		MantleRmlDocumentRules::FindUndrawnProperties(File.Text, Undrawn);
		for (const FString& Property : Undrawn)
		{
			OutWarnings->Add({ File.Path, FString::Printf(
				TEXT("%s uses %s, which does not draw in Mantle yet. Players see those elements without it."), *File.Path, *Property), Opens });
		}
		if (bScanned && File.Path.EndsWith(TEXT(".rml"), ESearchCase::IgnoreCase))
		{
			FString OtherModel;
			MantleRmlDocumentRules::PrepareMarkup(File.Text, OtherModel);
			if (!OtherModel.IsEmpty())
			{
				OutWarnings->Add({ File.Path, FString::Printf(
					TEXT("%s binds data-model=\"%s\", so script data (SetData) will not reach it. Use data-model=\"%s\", or leave the attribute out."),
					*File.Path, *OtherModel, MantleRmlDocumentRules::DataModelName), Opens });
			}
		}
	}
	return Problems;
}

TArray<FString> FMantlePlaceExporter::MakeUIDocumentIds(const TArray<FString>& Paths)
{
	TArray<FString> Ids;
	Ids.Reserve(Paths.Num());
	TSet<FString> Used;
	for (const FString& Path : Paths)
	{
		FString UnderUI = Path;
		UnderUI.RemoveFromStart(TEXT("UI/"), ESearchCase::CaseSensitive);
		// SanitiseId keeps at most 36 characters, so "_" and a suffix up to 64 still fit uid_'s 40.
		const FString Base = TEXT("uid_") + SanitiseId(UnderUI);
		FString Candidate = Base;
		int32 Suffix = 2;
		while (Used.Contains(Candidate)) Candidate = FString::Printf(TEXT("%s_%d"), *Base, Suffix++);
		Used.Add(Candidate);
		Ids.Add(MoveTemp(Candidate));
	}
	return Ids;
}

void FMantlePlaceExporter::CollectScriptFiles(
	const FString& ScriptsFolder,
	TArray<FMantleScriptCatalogEntry>& OutEntries,
	TArray<FMantleScriptCollectionIssue>& OutIssues)
{
	OutEntries.Reset();
	OutIssues.Reset();
	if (ScriptsFolder.IsEmpty())
	{
		return;
	}

	// FMantleDiskScriptCatalogSource is the runtime's own discovery (the same code a
	// -MantlePlace dev run uses to load Scripts/ from disk) -- reusing it here is what
	// guarantees the exporter classifies Server/Client/Shared and Modules/ exactly as the
	// runtime would. Its constructor takes the place file's path and looks beside it, so a
	// synthetic sibling name is enough to point it at ScriptsFolder's parent.
	const FString LevelDir = FPaths::GetPath(ScriptsFolder);
	FMantleDiskScriptCatalogSource Source(FPaths::Combine(LevelDir, TEXT("place.json")));
	OutEntries = Source.GetEntries();

	// The catalogue silently skips a file it cannot classify (a UE_LOG warning only, per
	// MantleScriptCatalog.cpp). Walk the same directory by hand so nothing a creator put in
	// Scripts/ is ever dropped without a word (#129).
	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *ScriptsFolder, TEXT("*.*"), true, false);
	for (const FString& File : Files)
	{
		FString Relative = File;
		FPaths::MakePathRelativeTo(Relative, *(ScriptsFolder / TEXT("")));
		Relative.ReplaceInline(TEXT("\\"), TEXT("/"));

		const FString CleanName = FPaths::GetCleanFilename(Relative);
		if (CleanName.IsEmpty() || CleanName.StartsWith(TEXT(".")))
		{
			continue;
		}

		const bool bClassified = OutEntries.ContainsByPredicate(
			[&Relative](const FMantleScriptCatalogEntry& Entry)
			{
				return Entry.Path.Equals(Relative, ESearchCase::CaseSensitive);
			});
		if (bClassified)
		{
			continue;
		}

		const EMantleScriptCatalogLanguage Language =
			MantleScriptLanguageFromExtension(FPaths::GetExtension(Relative));
		OutIssues.Add(FMantleScriptCollectionIssue{ Relative, Language == EMantleScriptCatalogLanguage::Unknown });
	}

	OutIssues.Sort([](const FMantleScriptCollectionIssue& A, const FMantleScriptCollectionIssue& B)
	{
		return A.RelativePath < B.RelativePath;
	});
}

void FMantlePlaceExporter::CollectGraphScripts(
	const FString& ScriptsFolder,
	TArray<FMantleGraphScriptEntry>& OutEntries,
	TArray<FMantleParseError>& OutErrors,
	TArray<FString>& OutGraphPaths)
{
	OutEntries.Reset();
	OutErrors.Reset();
	OutGraphPaths.Reset();
	if (ScriptsFolder.IsEmpty())
	{
		return;
	}

	FString ScriptsPackageRoot;
	if (!FPackageName::TryConvertFilenameToLongPackageName(ScriptsFolder, ScriptsPackageRoot))
	{
		OutErrors.Add(FMantleParseError{ TEXT("/scripts"), TEXT("could not locate the Scripts package for graph assets") });
		return;
	}

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	TArray<FAssetData> Assets;
	AssetRegistry.GetAssetsByPath(FName(*ScriptsPackageRoot), Assets, /*bRecursive*/ true, /*bIncludeOnlyOnDiskAssets*/ false);
	Assets.Sort([](const FAssetData& A, const FAssetData& B) { return A.PackageName.LexicalLess(B.PackageName); });

	for (const FAssetData& Asset : Assets)
	{
		if (Asset.AssetClassPath != UMantleScriptGraph::StaticClass()->GetClassPathName())
		{
			continue;
		}

		const FString PackageName = Asset.PackageName.ToString();
		FString Relative = PackageName;
		Relative.RemoveFromStart(ScriptsPackageRoot + TEXT("/"));
		const FString GraphPath = Relative + TEXT(".graph");
		const FString ErrorPath = TEXT("/scripts/") + GraphPath;
		OutGraphPaths.Add(GraphPath);

		UMantleScriptGraph* GraphAsset = Cast<UMantleScriptGraph>(Asset.GetAsset());
		if (!GraphAsset)
		{
			OutErrors.Add(FMantleParseError{ ErrorPath, TEXT("couldn't load the graph asset") });
			continue;
		}
		GraphAsset->EnsureEdGraph();
		const UMantleScriptEdGraph* EdGraph = Cast<UMantleScriptEdGraph>(GraphAsset->EdGraph);
		if (!EdGraph)
		{
			OutErrors.Add(FMantleParseError{ ErrorPath, TEXT("graph asset has no editable graph") });
			continue;
		}

		EMantleSide GraphSide;
		FString FolderWarning;
		if (!FMantleScriptGraphSerializer::ClassifySide(PackageName, GraphSide, FolderWarning))
		{
			OutErrors.Add(FMantleParseError{ ErrorPath, FolderWarning });
			continue;
		}

		FMantleGraph ParsedGraph;
		FString BuildError;
		if (!FMantleScriptGraphSerializer::BuildGraph(*EdGraph, FPackageName::GetShortName(PackageName), GraphSide, ParsedGraph, BuildError))
		{
			OutErrors.Add(FMantleParseError{ ErrorPath, BuildError });
			continue;
		}
		const FMantleGraphCompileResult Compiled = FMantleGraphCompiler::Compile(ParsedGraph);
		if (!Compiled.bSuccess)
		{
			OutErrors.Add(FMantleParseError{ ErrorPath, Compiled.Error });
			continue;
		}

		FMantleGraphScriptEntry Entry;
		Entry.PackageName = PackageName;
		Entry.Name = FPackageName::GetShortName(PackageName);
		Entry.Path = GraphPath;
		Entry.Side = MapGraphSide(GraphSide);
		Entry.CompiledLuau = Compiled.Luau;
		Entry.AuthorJson = FMantleGraph::ToJson(ParsedGraph);
		Entry.ApiNeed = FMantleScriptApiUsage::ForGraph(ParsedGraph);
		OutEntries.Add(MoveTemp(Entry));
	}

	OutEntries.Sort([](const FMantleGraphScriptEntry& A, const FMantleGraphScriptEntry& B) { return A.Path < B.Path; });
	OutGraphPaths.Sort();
}

void FMantlePlaceExporter::FilterGraphGeneratedFiles(
	TArray<FMantleScriptCatalogEntry>& InOutEntries,
	TArray<FMantleScriptCollectionIssue>& InOutIssues,
	const TArray<FString>& GraphPaths)
{
	TSet<FString> GeneratedPaths;
	for (const FString& GraphPath : GraphPaths)
	{
		GeneratedPaths.Add(FPaths::ChangeExtension(GraphPath, TEXT("luau")));
		GeneratedPaths.Add(FPaths::ChangeExtension(GraphPath, TEXT("uasset")));
	}
	InOutEntries.RemoveAll([&GeneratedPaths](const FMantleScriptCatalogEntry& Entry)
	{
		return GeneratedPaths.Contains(Entry.Path);
	});
	InOutIssues.RemoveAll([&GeneratedPaths](const FMantleScriptCollectionIssue& Issue)
	{
		return GeneratedPaths.Contains(Issue.RelativePath);
	});
}

TArray<FString> FMantlePlaceExporter::MakeScriptIds(const TArray<FString>& Paths)
{
	TArray<FString> Ids;
	Ids.Reserve(Paths.Num());
	TSet<FString> Used;
	for (const FString& Path : Paths)
	{
		const FString Base = TEXT("scr_") + SanitiseId(FPaths::GetBaseFilename(Path, /*bRemovePath*/ false));
		FString Candidate = Base;
		int32 Suffix = 2;
		while (Used.Contains(Candidate)) Candidate = FString::Printf(TEXT("%s_%d"), *Base, Suffix++);
		Used.Add(Candidate);
		Ids.Add(MoveTemp(Candidate));
	}
	return Ids;
}

bool FMantlePlaceExporter::CompileTypeScript(const FString& Source, FString& OutJavaScript, FMantleScriptError& OutError)
{
	return FMantleQuickJSRuntime::TranspileTypeScript(ExporterTypeScriptCompilerBundle(), Source, OutJavaScript, OutError);
}
