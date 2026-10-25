// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlacePreview.h"
#include "MantleApiClient.h"
#include "MantleAssetLibrary.h"
#include "MantlePlaceAssetConverter.h"
#include "MantlePlaceParser.h"
#include "MantleRuntime.h"
#include "MantleSha256.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantlePlacePreviewPrivate
{
	/** One converted file to ask Mantle about. */
	struct FPreviewUploadProbe
	{
		FString Key;
		FString Hash;
		int64 Bytes = 0;
	};

	/** Asks Diff about Probes and records every approved one Mantle holds under its key. */
	bool PreviewAskAndRecord(FMantlePlaceAssetSet& Set, const TArray<FPreviewUploadProbe>& Probes, const FMantlePreviewDiff& Diff, FString& OutProblem)
	{
		if (Probes.Num() == 0)
		{
			return true;
		}
		TArray<FString> Hashes;
		for (const FPreviewUploadProbe& Probe : Probes)
		{
			Hashes.AddUnique(Probe.Hash);
		}
		TArray<FMantleUploadedAsset> Known;
		if (!Diff(Hashes, Known, OutProblem))
		{
			return false;
		}
		for (const FMantleUploadedAsset& Asset : Known)
		{
			// Only what a player could download: an asset still being screened, or refused, is not.
			if (Asset.Status != TEXT("approved") || !FMantlePreparedPlaceAssets::IsUploadedId(Asset.Id))
			{
				continue;
			}
			for (const FPreviewUploadProbe& Probe : Probes)
			{
				if (Probe.Hash == Asset.Hash)
				{
					Set.RecordUpload(Probe.Key, Asset.Id, Asset.Hash, Probe.Bytes);
				}
			}
		}
		return true;
	}

	/** Whether an entity uses an uploaded mesh, material, decal texture (#472) or sound (#324) Mantle does not hold (not in Recorded). */
	bool PreviewUsesAssetNotOnMantle(const FMantleEntity& Entity, const TSet<FString>& Recorded)
	{
		auto NotOnMantle = [&Recorded](const FString& Id)
		{
			return FMantlePreparedPlaceAssets::IsUploadedId(Id) && !Recorded.Contains(Id);
		};
		if (const FMantleTerrainProps* Terrain = Entity.Props.TryGet<FMantleTerrainProps>())
		{
			// A Terrain (#466): its heights, its weights (two maps past four layers, #628) and every layer's material.
			if (NotOnMantle(Terrain->Heightfield) || NotOnMantle(Terrain->Weightmap) || NotOnMantle(Terrain->Weightmap2))
			{
				return true;
			}
			return Terrain->Layers.ContainsByPredicate([&NotOnMantle](const FMantleTerrainLayer& Layer) { return NotOnMantle(Layer.Material); });
		}
		if (const FMantleSoundEmitterProps* Emitter = Entity.Props.TryGet<FMantleSoundEmitterProps>())
		{
			// A sound id the library knows is the library's, whatever its shape (the builder's rule).
			return NotOnMantle(Emitter->Sound) && !FMantleAssetLibrary::Get().IsKnown(Emitter->Sound);
		}
		// A SkeletalMesh's mesh, materials and clip (#469).
		if (const FMantleSkeletalMeshProps* Skeletal = Entity.Props.TryGet<FMantleSkeletalMeshProps>())
		{
			return NotOnMantle(Skeletal->Mesh) || NotOnMantle(Skeletal->Clip) || Skeletal->Materials.ContainsByPredicate(NotOnMantle);
		}
		// A Decal's textures (#472).
		if (const FMantleDecalProps* Decal = Entity.Props.TryGet<FMantleDecalProps>())
		{
			return NotOnMantle(Decal->BaseColor) || NotOnMantle(Decal->Normal);
		}
		// A sprites Effect's texture (#628).
		if (const FMantleEffectProps* Effect = Entity.Props.TryGet<FMantleEffectProps>())
		{
			return NotOnMantle(Effect->TextureId);
		}
		// A Scatter's meshes and their materials (#530).
		if (const FMantleScatterParams* Scatter = Entity.Props.TryGet<FMantleScatterParams>())
		{
			return Scatter->Meshes.ContainsByPredicate([&NotOnMantle](const FMantleScatterMesh& ScatterMesh)
			{
				return NotOnMantle(ScatterMesh.MeshId) || ScatterMesh.Materials.ContainsByPredicate(NotOnMantle);
			});
		}
		const FMantleStaticMeshProps* Mesh = MantleMeshPropsOf(Entity); // StaticMesh or InstancedMesh (#467)
		if (!Mesh)
		{
			return false;
		}
		if (NotOnMantle(Mesh->Mesh))
		{
			return true;
		}
		for (const FString& Material : Mesh->Materials)
		{
			if (NotOnMantle(Material))
			{
				return true;
			}
		}
		return false;
	}
}

bool FMantlePlacePreview::RecordKnownUploads(FMantlePlaceAssetSet& Set, const FMantlePreviewDiff& Diff, FString& OutProblem)
{
	using namespace MantlePlacePreviewPrivate;
	if (!Diff)
	{
		OutProblem = TEXT("there is no way to ask Mantle");
		return false;
	}

	// Textures first: a material's bytes name its textures' ids, so its hash is known only then.
	TArray<FPreviewUploadProbe> Probes;
	for (const FMantleConvertedTexture& Texture : Set.Textures)
	{
		Probes.Add({ Texture.Key, Texture.Hash, Texture.Bytes.Num() });
	}
	if (!PreviewAskAndRecord(Set, Probes, Diff, OutProblem))
	{
		return false;
	}

	Probes.Reset();
	for (const FMantleConvertedMaterial& Material : Set.Materials)
	{
		TArray<uint8> Bytes;
		FString NotYet;
		// False when one of its textures is not on Mantle: then the material cannot be either.
		if (Set.MaterialBytes(Material, Bytes, NotYet))
		{
			Probes.Add({ Material.Key, FMantleSha256::AssetHash(Bytes), Bytes.Num() });
		}
	}
	if (!PreviewAskAndRecord(Set, Probes, Diff, OutProblem))
	{
		return false;
	}

	Probes.Reset();
	for (const FMantleConvertedMesh& Mesh : Set.Meshes)
	{
		Probes.Add({ Mesh.Key, Mesh.Hash, Mesh.Bytes.Num() });
	}
	// Sounds (#324), skeletal meshes and clips (#469) and heightfields (#466) name nothing else, so they are asked with the meshes.
	for (const FMantleConvertedSound& Sound : Set.Sounds)
	{
		Probes.Add({ Sound.Key, Sound.Hash, Sound.Bytes.Num() });
	}
	for (const FMantleConvertedSkeletalMesh& Skeletal : Set.SkeletalMeshes)
	{
		Probes.Add({ Skeletal.Key, Skeletal.Hash, Skeletal.Bytes.Num() });
	}
	for (const FMantleConvertedClip& Clip : Set.Clips)
	{
		Probes.Add({ Clip.Key, Clip.Hash, Clip.Bytes.Num() });
	}
	for (const FMantleConvertedHeightfield& Heightfield : Set.Heightfields)
	{
		Probes.Add({ Heightfield.Key, Heightfield.Hash, Heightfield.Bytes.Num() });
	}
	return PreviewAskAndRecord(Set, Probes, Diff, OutProblem);
}

bool FMantlePlacePreview::FindUploadedAssets(UWorld* SourceWorld, FMantlePlaceAssetSet& OutSet, const FMantlePreviewDiff& Diff, FString& OutProblem)
{
	if (!SourceWorld)
	{
		OutProblem = TEXT("there is no level");
		return false;
	}
	const FMantlePlaceDependencies Dependencies = FMantlePlaceDependencies::Gather(SourceWorld);
	if (!Dependencies.NeedsUploads())
	{
		return true;
	}
	// The same conversion Publish runs, from the same cache, so the hashes are the ones it uploads.
	FMantlePlaceAssetConverter Converter;
	Converter.Convert(Dependencies, OutSet);
	return RecordKnownUploads(OutSet, Diff, OutProblem);
}

FMantlePreviewDiff FMantlePlacePreview::ApiDiff()
{
	return [](const TArray<FString>& Hashes, TArray<FMantleUploadedAsset>& OutKnown, FString& OutError)
	{
		// The upload queue's batch size (FMantleAssetUploadQueue::FSettings::DiffBatchSize).
		constexpr int32 BatchSize = 500;
		for (int32 From = 0; From < Hashes.Num(); From += BatchSize)
		{
			const TConstArrayView<FString> Batch(Hashes.GetData() + From, FMath::Min(BatchSize, Hashes.Num() - From));
			const FMantleApiResult Response = FMantleApiClient::Send(TEXT("POST"), TEXT("/assets/diff"), FMantleApiClient::MakeDiffAssetsBody(Batch));
			TArray<FMantleUploadedAsset> Known;
			TArray<FString> Unknown;
			if (!Response.bSuccess || !FMantleApiClient::ParseDiffAssets(Response.Body, Known, Unknown))
			{
				OutError = Response.bSuccess ? FString(TEXT("Mantle's answer could not be read")) : Response.Message;
				return false;
			}
			OutKnown.Append(MoveTemp(Known));
		}
		return true;
	};
}

FMantlePreviewResult FMantlePlacePreview::Build(UWorld* SourceWorld, const FMantlePreviewOptions& PreviewOptions)
{
	using namespace MantlePlacePreviewPrivate;
	FMantlePreviewResult Result;

	if (!SourceWorld)
	{
		Result.Validation.Issues.Add(FMantleValidationIssue{
			EMantleIssueSeverity::Error,
			FString(),
			TEXT("There is no level to preview."),
			TEXT("Open a level first.") });
		return Result;
	}

	Result.Validation = FMantlePlaceValidator::Validate(SourceWorld);
	Result.LeftOut = ListContentLeftOut(SourceWorld);

	FMantleExportOptions Options;
	Options.PlaceId = TEXT("plc_preview01");
	Options.Name = SourceWorld->GetName();
	Options.AuthorId = TEXT("usr_preview");
	// The ids Mantle holds the level's own meshes and materials under, when looked up (#394).
	Options.Assets = PreviewOptions.Assets;

	// Export even when validation failed. A preview with the unsupported actors missing
	// tells the creator far more than refusing to preview would.
	FMantleExportResult Exported = FMantlePlaceExporter::Export(SourceWorld, Options);

	// A vehicle spawn the creator has not stored in the library yet is exported as a draft (a raw uploaded id), which
	// the parser refuses (#679 B1): a published place only parks stored vehicles. Publish stores the vehicle first, so
	// the preview leaves the spawn out and says so.
	for (int32 EntityIndex = Exported.Place.Entities.Num() - 1; EntityIndex >= 0; --EntityIndex)
	{
		const FMantleVehicleSpawnProps* Draft = Exported.Place.Entities[EntityIndex].Props.TryGet<FMantleVehicleSpawnProps>();
		if (Draft && Draft->StorageItem.IsEmpty())
		{
			Result.LeftOut.Add(FString::Printf(TEXT("'%s' is a vehicle spawn whose vehicle is not in your library yet (Publish adds it, then the spawn parks it)"),
				*Exported.Place.Entities[EntityIndex].Name));
			Exported.Place.Entities.RemoveAt(EntityIndex);
		}
	}
	Result.SourceEntityCount = Exported.Place.Entities.Num();

	// The long way round on purpose: serialise, then parse back. Anything that does not
	// survive the data path is absent from the preview, exactly as it would be for players.
	Result.PlaceJson = FMantlePlaceExporter::ToJson(Exported.Place);

	const FMantleParseResult Parsed = FMantlePlaceParser::ParseFromString(Result.PlaceJson);
	if (!Parsed.bSuccess)
	{
		// The exporter produced something the parser rejects. Players would see nothing.
		for (const FMantleParseError& Error : Parsed.Errors)
		{
			Result.Validation.Issues.Add(FMantleValidationIssue{
				EMantleIssueSeverity::Error,
				Error.Path,
				Error.Message,
				TEXT("This is a Mantle bug rather than a problem with your level. Please report it.") });
		}
		return Result;
	}

	Result.World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false);
	if (!Result.World)
	{
		Result.Validation.Issues.Add(FMantleValidationIssue{
			EMantleIssueSeverity::Error,
			FString(),
			TEXT("Could not create a preview world."),
			TEXT("Try again, or restart the editor.") });
		return Result;
	}

	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(Result.World);

	// Uploaded (ast_u) meshes and materials load the way a client loads them (#394): prepared
	// first, then found by the builder through FMantleAssetLibrary. One that Mantle does not hold
	// yet (never published, or changed since) has only a provisional id nothing can resolve, so
	// the objects using it are left out and named as waiting for Publish, not as failures.
	TSet<FString> Recorded;
	if (PreviewOptions.Assets)
	{
		for (const TPair<FString, FMantlePlaceUploadedRef>& Pair : PreviewOptions.Assets->Uploaded)
		{
			Recorded.Add(Pair.Value.Id);
		}
	}
	FMantlePlace ToBuild = Parsed.Place;
	ToBuild.Entities.RemoveAll([&Recorded](const FMantleEntity& Entity) { return PreviewUsesAssetNotOnMantle(Entity, Recorded); });
	Result.NotUploadedCount = Parsed.Place.Entities.Num() - ToBuild.Entities.Num();
	if (Result.NotUploadedCount > 0)
	{
		Result.Validation.Issues.Add(FMantleValidationIssue{
			EMantleIssueSeverity::Warning,
			FString(),
			FString::Printf(TEXT("%d object(s) use meshes or materials from your project that are not on Mantle yet, so this check leaves them out."),
				Result.NotUploadedCount),
			TEXT("Publish uploads them. Check again afterwards to see them as players will.") });
	}

	Result.PreparedAssets = FMantlePreparedPlaceAssets::PrepareAndWait(ToBuild, PreviewOptions.AssetTimeoutSeconds);
	for (const FMantleParseError& Error : Result.PreparedAssets.Errors)
	{
		Result.Validation.Issues.Add(FMantleValidationIssue{
			EMantleIssueSeverity::Error,
			Error.Path,
			FString::Printf(TEXT("Players could not load an uploaded mesh or material: %s"), *Error.Message),
			TEXT("Publish again so it is uploaded afresh; if it keeps failing, report it.") });
	}

	Result.Build = FMantlePlaceBuilder::Build(Result.World, ToBuild);
	Result.PreviewActorCount = Result.Build.Actors.Num();
	Result.bSuccess = Result.Build.bSuccess;

	UE_LOG(LogMantle, Log,
		TEXT("Preview built: %d of %d entities reached the preview (%d wait for Publish to upload their files), %d validation issue(s)"),
		Result.PreviewActorCount,
		Result.SourceEntityCount,
		Result.NotUploadedCount,
		Result.Validation.Issues.Num());

	return Result;
}

void FMantlePlacePreview::Destroy(FMantlePreviewResult& Result)
{
	FMantlePlaceBuilder::Teardown(Result.Build);

	if (Result.World)
	{
		GEngine->DestroyWorldContext(Result.World);
		Result.World->DestroyWorld(false);
		Result.World = nullptr;
	}
	// After the actors using them are gone.
	Result.PreparedAssets.Release();

	Result.bSuccess = false;
	Result.PreviewActorCount = 0;
}

TArray<FString> FMantlePlacePreview::ListContentLeftOut(UWorld* SourceWorld)
{
	TArray<FString> LeftOut;
	if (!SourceWorld)
	{
		return LeftOut;
	}

	for (TActorIterator<AActor> It(SourceWorld); It; ++It)
	{
		const AActor* Actor = *It;
		if (!IsValid(Actor))
		{
			continue;
		}

#if WITH_EDITOR
		const FString Label = Actor->GetActorLabel();
#else
		const FString Label = Actor->GetName();
#endif

		// Same order as FMantlePlaceValidator::Validate, so an actor is never described two
		// different ways by the two commands.
		if (FMantlePlaceExporter::IsProvidedByMantle(Actor))
		{
			LeftOut.Add(FString::Printf(TEXT("'%s' is a sky actor. Mantle supplies its own sky, so its settings are not sent."), *Label));
			continue;
		}

		FString What;
		FString Remedy;
		if (FMantlePlaceExporter::DescribeUnpublishedContent(Actor, What, Remedy))
		{
			LeftOut.Add(FString::Printf(TEXT("'%s' %s"), *Label, *What));
			continue;
		}

		if (FMantlePlaceExporter::IsEngineInfrastructure(Actor))
		{
			continue;
		}

		EMantleEntityType Type;
		if (!FMantlePlaceExporter::ClassifyActor(Actor, Type))
		{
			LeftOut.Add(FString::Printf(
				TEXT("'%s' is a %s, which Mantle cannot publish."), *Label, *Actor->GetClass()->GetName()));
		}
		else if (Type == EMantleEntityType::Group)
		{
			// A Blueprint prefab's components that can't publish (#468); the rest of it is sent.
			TArray<FMantlePrefabPart> Parts;
			TArray<FMantleUnpublishedPart> Unsupported;
			FMantlePlaceExporter::CollectPrefabParts(Actor, Parts, &Unsupported);
			for (const FMantleUnpublishedPart& Part : Unsupported)
			{
				if (Parts.Num() > 0)
				{
					LeftOut.Add(FString::Printf(TEXT("'%s' %s"), *Label, *Part.What));
				}
			}
		}
	}

	return LeftOut;
}

FString FMantlePlacePreview::DescribeForCreator(const FMantlePreviewResult& Result, int32 MaxListed)
{
	TArray<FString> Lines;
	Lines.Add(FString::Printf(
		TEXT("%d object(s) in this level will be sent to players."), Result.SourceEntityCount));

	// Waiting for Publish to upload a mesh or material from the project: not a failure (#394).
	if (Result.NotUploadedCount > 0)
	{
		Lines.Add(FString::Printf(
			TEXT("%d of them use files from your project that Publish has not uploaded yet, so this check could not load them."),
			Result.NotUploadedCount));
	}

	// Exported but not rebuilt: lost between the place file and the client's builder.
	const int32 Failed = Result.SourceEntityCount - Result.NotUploadedCount - Result.PreviewActorCount;
	if (Failed > 0)
	{
		Lines.Add(FString::Printf(
			TEXT("%d of them failed to rebuild the way the Mantle client builds them; see the issues below."),
			Failed));
	}

	if (Result.LeftOut.Num() == 0)
	{
		Lines.Add(TEXT("Nothing in this level is left out."));
		return FString::Join(Lines, TEXT("\n"));
	}

	Lines.Add(FString());
	Lines.Add(FString::Printf(TEXT("%d object(s) will NOT reach players:"), Result.LeftOut.Num()));
	for (int32 Index = 0; Index < Result.LeftOut.Num(); ++Index)
	{
		if (Index >= MaxListed)
		{
			Lines.Add(FString::Printf(TEXT("...and %d more."), Result.LeftOut.Num() - MaxListed));
			break;
		}
		Lines.Add(TEXT("- ") + Result.LeftOut[Index]);
	}
	return FString::Join(Lines, TEXT("\n"));
}
