// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceImporter.h"
#include "MantleApiClient.h"
#include "MantleCrowdSpawner.h"
#include "MantlePlaceParser.h"
#include "MantlePlaceSettingsActor.h"
#include "MantleRuntime.h"

#include "Engine/World.h"
#include "Components/SceneComponent.h"
#include "GameFramework/Actor.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "Mantle"

FString FMantlePlaceImporter::TemplatePath()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("MantleCore"));
	if (!Plugin.IsValid()) return FString();
	return FPaths::ConvertRelativePathToFull(FPaths::Combine(
		Plugin->GetBaseDir(), TEXT("Resources"), TEXT("Templates"), TEXT("new-place.json")));
}

FMantleBuildResult FMantlePlaceImporter::Import(
	UWorld* World,
	const FMantlePlace& Place,
	const TFunction<FMantleApiResult(const FString&)>& ScriptSourceResolver)
{
	FMantleBuildResult Failed;
	if (!World)
	{
		Failed.Errors.Add(FMantleParseError{ TEXT(""), TEXT("no world to import into") });
		return Failed;
	}
    // The native editor represents the authored dynamic flag through root mobility.
    // UE cannot preserve a static child beneath a movable ancestor. Refuse that shape
    // before resolving scripts, starting a transaction or spawning any actors, rather
    // than silently publishing a different dynamic flag on the next export.
    TMap<FString, const FMantleEntity*> EntityById;
    for (const FMantleEntity& Entity : Place.Entities) EntityById.Add(Entity.Id, &Entity);
    for (int32 Index = 0; Index < Place.Entities.Num(); ++Index)
    {
        const FMantleEntity& Entity = Place.Entities[Index];
        if (Entity.bDynamic) continue;
        FString ParentId = Entity.Parent;
        TSet<FString> Visited;
        while (!ParentId.IsEmpty() && !Visited.Contains(ParentId))
        {
            Visited.Add(ParentId);
            const FMantleEntity* const* Parent = EntityById.Find(ParentId);
            if (!Parent) break;
            if ((*Parent)->bDynamic)
            {
                Failed.Errors.Add(FMantleParseError{
                    FString::Printf(TEXT("/entities/%d/dynamic"), Index),
                    FString::Printf(TEXT("cannot import non-dynamic entity '%s' beneath dynamic ancestor '%s': native editor mobility cannot preserve this hierarchy"),
                        *Entity.Id, *(*Parent)->Id) });
                break;
            }
            ParentId = (*Parent)->Parent;
        }
    }
    if (Failed.Errors.Num() > 0) return Failed;
	if ((Place.Scripts.Num() > 0 || Place.UIDocuments.Num() > 0) && !ScriptSourceResolver)
	{
		FString WhyNot;
		if (!FMantleApiClient::IsConfigured(WhyNot))
		{
			Failed.Errors.Add(FMantleParseError{ TEXT("/scripts"), WhyNot });
			return Failed;
		}
	}

	// A place carries content-addressed source references, while the editor needs the actual
	// text. Resolve every distinct source before changing the level so a failed download
	// never leaves the creator with a half-imported place.
	TMap<FString, FString> SourceById;
	auto ResolveSource = [&ScriptSourceResolver](const FString& SourceId)
	{
		return ScriptSourceResolver ? ScriptSourceResolver(SourceId) : FMantleApiClient::GetScriptSource(SourceId);
	};
	for (int32 ScriptIndex = 0; ScriptIndex < Place.Scripts.Num(); ++ScriptIndex)
	{
		const FMantleScript& Script = Place.Scripts[ScriptIndex];
		const FString& EditableSourceId = Script.AuthorSource.IsEmpty()
			? Script.Source
			: Script.AuthorSource;
		if (SourceById.Contains(EditableSourceId))
		{
			continue;
		}

		const FMantleApiResult Downloaded = ResolveSource(EditableSourceId);
		if (!Downloaded.bSuccess)
		{
			Failed.Errors.Add(FMantleParseError{
				FString::Printf(TEXT("/scripts/%d/source"), ScriptIndex),
				FString::Printf(
					TEXT("could not fetch source '%s': %s"),
					*EditableSourceId,
					*Downloaded.Message) });
			continue;
		}
		SourceById.Add(EditableSourceId, Downloaded.Body);
	}
	// #549: UI files are stored and served exactly like script source, so they resolve the same
	// way, before anything changes. Like scripts, they are not written back to disk yet.
	for (int32 DocumentIndex = 0; DocumentIndex < Place.UIDocuments.Num(); ++DocumentIndex)
	{
		const FString& DocumentSourceId = Place.UIDocuments[DocumentIndex].Source;
		// #556 item 5: an image is a Texture asset, not source text; nothing to fetch here.
		if (Place.UIDocuments[DocumentIndex].IsImage() || SourceById.Contains(DocumentSourceId))
		{
			continue;
		}
		const FMantleApiResult Downloaded = ResolveSource(DocumentSourceId);
		if (!Downloaded.bSuccess)
		{
			Failed.Errors.Add(FMantleParseError{
				FString::Printf(TEXT("/uiDocuments/%d/source"), DocumentIndex),
				FString::Printf(TEXT("could not fetch UI file '%s' (%s): %s"),
					*Place.UIDocuments[DocumentIndex].Path, *DocumentSourceId, *Downloaded.Message) });
			continue;
		}
		SourceById.Add(DocumentSourceId, Downloaded.Body);
	}
	if (Failed.Errors.Num() > 0)
	{
		return Failed;
	}

	// One undo step for the whole import — a creator who changes their mind should not have
	// to press undo once per object.
	const FScopedTransaction Transaction(LOCTEXT("ImportPlace", "Import Mantle place"));

	// Persistent, not transient: these actors must survive a save, unlike the client's,
	// which are rebuilt from data on every join.
	FMantleBuildOptions Options;
	Options.bTransient = false;

	// No runtime default environment (T5.30, #445). Its sky actors are transient and not part
	// of the creator's level: left in place they would show in the outliner, vanish on reload
	// and be reported by the validator as unpublished sky settings. Its day/night cycle would
	// also take over the place's own DirectionalLight and re-aim it for the hour of the import,
	// and the next export would publish that angle instead of the authored one. The sky belongs
	// to players; the editor level gets exactly what the place file says.
	Options.bRuntimeEnvironment = false;

	// Entity ids are not carried into the level. The exporter derives an id from each actor's
	// ActorGuid (#267), and these freshly spawned actors get new guids, so an imported level
	// publishes new entity ids the first time and keeps them from then on. Writing the file's
	// ids back would mean forcing ActorGuids: the engine only exposes that through
	// FActorSpawnParameters::OverrideActorGuid (meant for Blueprint reinstancing, and the
	// spawn happens in the runtime builder), and importing the same place twice into one level
	// would then give two actors the same guid, which the engine assumes never happens (World
	// Partition names external actor packages after it). Nothing reads entity ids across
	// revisions today (scripts use names, tags and handles), so the one-time change is harmless.
	FMantleBuildResult Result = FMantlePlaceBuilder::Build(World, Place, Options);
	// The place's settings go onto the level's settings actor, so a round trip keeps them
	// rather than falling back to the defaults on the next export (#266).
	AMantlePlaceSettingsActor::SetFrom(World, Place.Settings);
	AMantlePlaceSettingsActor::SetFrom(World, Place.Audience);
	// #614 stage 4: and its storage, which the stored vehicles' spawns (built above with their Storage Item) name.
	AMantlePlaceSettingsActor::SetStorageFrom(World, Place.Storage);
    // The runtime builder chooses mobility for gameplay (including movable overlap
    // meshes, lights and spawn points) and its retained model owns script tags. An
    // editor level instead stores authoring data in native actor tags/mobility, which
    // the exporter reads. Restore that data before a creator saves or republishes.
    for (const FMantleEntity& Entity : Place.Entities)
    {
        AActor* const* Actor = Result.Actors.Find(Entity.Id);
        if (!Actor || !IsValid(*Actor)) continue;
        (*Actor)->Modify();
        (*Actor)->Tags.Reset(Entity.Tags.Num());
        for (const FString& Tag : Entity.Tags) (*Actor)->Tags.Add(FName(*Tag));
        if (USceneComponent* Root = (*Actor)->GetRootComponent())
        {
            Root->Modify();
            Root->SetMobility(Entity.bDynamic ? EComponentMobility::Movable : EComponentMobility::Static);
        }
    }

	// #513: a crowd spawner's points of interest name entities by the ids above, which the level does not
	// keep; they point at the actors those entities became instead, so the next export names their new ids.
	for (const FMantleEntity& Entity : Place.Entities)
	{
		AActor* const* Built = Result.Actors.Find(Entity.Id);
		AMantleCrowdSpawner* Spawner = Built && IsValid(*Built) ? Cast<AMantleCrowdSpawner>(*Built) : nullptr;
		if (!Spawner)
		{
			continue;
		}
		for (FMantleCrowdPointOfInterestSettings& Point : Spawner->PointsOfInterest)
		{
			AActor* const* Target = Result.Actors.Find(Point.EntityId);
			if (Target && IsValid(*Target))
			{
				Spawner->Modify();
				Point.Actor = *Target;
			}
		}
	}

	// SourceById above validates that every script definition's source resolves
	// before anything is built.
	Result.bSuccess = Result.Errors.Num() == 0;

	World->MarkPackageDirty();

	UE_LOG(LogMantle, Log,
		TEXT("Imported place '%s': %d actor(s)"),
		*Place.Metadata.Name,
		Result.Actors.Num());

	return Result;
}

FMantleBuildResult FMantlePlaceImporter::ImportFromFile(UWorld* World, const FString& FilePath)
{
	const FMantleParseResult Parsed = FMantlePlaceParser::ParseFromFile(FilePath);
	if (!Parsed.bSuccess)
	{
		FMantleBuildResult Failed;
		Failed.Errors = Parsed.Errors;
		UE_LOG(LogMantle, Error,
			TEXT("Could not import '%s':\n%s"), *FilePath, *Parsed.ErrorSummary());
		return Failed;
	}

	return Import(World, Parsed.Place);
}

#undef LOCTEXT_NAMESPACE
