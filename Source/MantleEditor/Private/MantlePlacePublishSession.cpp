// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlacePublishSession.h"
#include "MantlePlaceThumbnail.h"
#include "MantleApiClient.h"
#include "MantleAttribution.h"
#include "MantleCoverageSurvey.h"
#include "MantleCreatorSignIn.h"
#include "MantlePlaceExporter.h"
#include "MantlePlaceSettingsActor.h"
#include "MantlePlaceWorldPartition.h"
#include "MantlePlaceStorage.h"
#include "MantlePublishCaches.h"
#include "MantleRuntime.h"
#include "MantleScriptCatalogFiles.h"
#include "MantleScriptError.h"
#include "MantleSettings.h"
#include "MantleSha256.h"
#include "MantleVehicleSource.h"
#include "MantleVehicleSpawnPoint.h"
#include "MantleWeaponSpawnPoint.h"

#include "Algo/Count.h"
#include "Animation/AnimSequence.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"
#include "Sound/SoundWave.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformTime.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/UObjectGlobals.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantlePlacePublishSessionPrivate
{
	/** #804 step 6: spill converted files to the conversion cache folder (see FMantlePlacePublishSession::FTiming::bSpillConvertedBytes). */
	TAutoConsoleVariable<int32> CVarSpillConvertedBytes(
		TEXT("Mantle.Publish.SpillConvertedBytes"), 0,
		TEXT("1: a publish writes each converted file of 64 KB or more to the conversion cache folder and sends it from there, ")
		TEXT("so a big level's converted files are not all held in memory. 0 (default): keep them in memory."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarAutoSpillMB(
		TEXT("Mantle.Publish.AutoSpillMB"), 0,
		TEXT("Megabytes of converted files a publish holds in memory before it starts spilling them to disk by itself. ")
		TEXT("0 (default): the built-in 2048. Below 0: never spill by itself."),
		ECVF_Default);

	TSharedPtr<FJsonObject> ParsePublishObject(const FString& Json)
	{
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
		{
			return nullptr;
		}
		return Object;
	}

	FString PublishStringField(const FString& Json, const TCHAR* Field)
	{
		FString Value;
		if (const TSharedPtr<FJsonObject> Object = ParsePublishObject(Json))
		{
			Object->TryGetStringField(Field, Value);
		}
		return Value;
	}

	int32 PublishNumberField(const FString& Json, const TCHAR* Field)
	{
		double Value = 0.0;
		if (const TSharedPtr<FJsonObject> Object = ParsePublishObject(Json))
		{
			Object->TryGetNumberField(Field, Value);
		}
		return static_cast<int32>(Value);
	}

	TArray<FString> PublishStringArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		TArray<FString> Values;
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (Object.IsValid() && Object->TryGetArrayField(Field, Array) && Array)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Array)
			{
				FString Text;
				if (Value.IsValid() && Value->TryGetString(Text))
				{
					Values.AddUnique(Text);
				}
			}
		}
		return Values;
	}

	FString CreatePlaceBody(const FString& Name)
	{
		FString Body;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Body);
		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("name"), Name);
		Writer->WriteObjectEnd();
		Writer->Close();
		return Body;
	}

	/** The value at a JSON pointer ("/entities/3/materials/1"), or null. */
	TSharedPtr<FJsonValue> ResolvePointer(const FString& Json, const FString& Pointer)
	{
		TSharedPtr<FJsonValue> Value;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		if (!FJsonSerializer::Deserialize(Reader, Value) || !Value.IsValid())
		{
			return nullptr;
		}
		TArray<FString> Parts;
		Pointer.ParseIntoArray(Parts, TEXT("/"), /*bCullEmpty*/ true);
		for (FString Part : Parts)
		{
			Part.ReplaceInline(TEXT("~1"), TEXT("/"));
			Part.ReplaceInline(TEXT("~0"), TEXT("~"));
			if (Value->Type == EJson::Object)
			{
				Value = Value->AsObject()->TryGetField(Part);
			}
			else if (Value->Type == EJson::Array && Part.IsNumeric())
			{
				const TArray<TSharedPtr<FJsonValue>>& Array = Value->AsArray();
				const int32 Index = FCString::Atoi(*Part);
				Value = Array.IsValidIndex(Index) ? Array[Index] : nullptr;
			}
			else
			{
				return nullptr;
			}
			if (!Value.IsValid())
			{
				return nullptr;
			}
		}
		return Value;
	}

	const TCHAR* SlotWords(const FString& Slot)
	{
		if (Slot == TEXT("baseColor")) return TEXT("base colour");
		if (Slot == TEXT("normal")) return TEXT("normal");
		if (Slot == TEXT("orm")) return TEXT("roughness, metal and occlusion");
		if (Slot == TEXT("emissive")) return TEXT("emissive");
		return TEXT("texture");
	}

	const TCHAR* CatalogLanguageName(EMantleScriptCatalogLanguage Language)
	{
		switch (Language)
		{
		case EMantleScriptCatalogLanguage::JavaScript: return TEXT("javascript");
		case EMantleScriptCatalogLanguage::TypeScript: return TEXT("typescript");
		case EMantleScriptCatalogLanguage::Python: return TEXT("python");
		case EMantleScriptCatalogLanguage::Luau: return TEXT("luau");
		default: return TEXT("");
		}
	}

	bool IsGateCode(const FString& Code)
	{
		return Code == TEXT("assets_rejected") || Code == TEXT("assets_not_owned") || Code == TEXT("assets_missing") || Code == TEXT("assets_pending")
			|| Code == TEXT("wrong_asset_kind") || Code == TEXT("material_slots") || Code == TEXT("compatibility_failed");
	}

	bool IsUploadedAssetId(const FString& Id)
	{
		return Id.StartsWith(TEXT("ast_u")) && Id.Len() == 5 + 32;
	}

	/** A converted asset's object path; a conversion always records it, and its local key ("mesh:" + the path) stands in if not. */
	FString ConvertedObjectPath(const FString& Path, const FString& Key)
	{
		if (!Path.IsEmpty())
		{
			return Path;
		}
		int32 Colon = INDEX_NONE;
		return Key.FindChar(TEXT(':'), Colon) ? Key.Mid(Colon + 1) : Key;
	}
}

// =============================================================================================
// Keeping what was gathered alive

void FMantleDependencyPins::Pin(const FMantlePlaceDependencies& Dependencies)
{
	Objects.Reset();
	TSet<UObject*> Seen;
	auto Add = [this, &Seen](UObject* Object)
	{
		if (Object && IsValid(Object) && !Seen.Contains(Object))
		{
			Seen.Add(Object);
			Objects.Add(Object);
		}
	};
	for (const FMantleGatheredMesh& Entry : Dependencies.Meshes)
	{
		Add(Entry.Mesh.Get());
	}
	for (const FMantleGatheredMaterial& Entry : Dependencies.Materials)
	{
		Add(Entry.Material.Get());
	}
	for (const FMantleGatheredTexture& Entry : Dependencies.Textures)
	{
		Add(Entry.Texture.Get());
	}
	for (const FMantleGatheredSound& Entry : Dependencies.Sounds)
	{
		Add(Entry.Sound.Get());
	}
	for (const FMantleGatheredSkeletalMesh& Entry : Dependencies.SkeletalMeshes)
	{
		Add(Entry.Mesh.Get());
	}
	for (const FMantleGatheredClip& Entry : Dependencies.Clips)
	{
		Add(Entry.Sequence.Get());
		Add(Entry.Mesh.Get());
	}
	for (const FMantleGatheredTerrain& Entry : Dependencies.Terrains)
	{
		Add(Entry.Landscape.Get());
	}
	for (const FMantleGatheredVehicle& Entry : Dependencies.Vehicles)
	{
		Add(Entry.Vehicle.Get());
	}
	for (const FMantleGatheredWeapon& Entry : Dependencies.Weapons)
	{
		Add(Entry.Mesh.Get());
	}
}

void FMantleDependencyPins::Release()
{
	Objects.Reset();
}

void FMantleDependencyPins::AddReferencedObjects(FReferenceCollector& Collector)
{
	Collector.AddReferencedObjects(Objects);
}

// =============================================================================================
// The open level

FMantleLevelPublishSource::FMantleLevelPublishSource(UWorld* InWorld, FString InNotes, TSharedPtr<FMantlePlaceAssetConverter> InConverter)
	: World(InWorld)
	, Notes(MoveTemp(InNotes))
	, Converter(MoveTemp(InConverter))
{
	if (InWorld)
	{
		// The short name is what the creator calls the level; the package path is what tells two
		// levels of the same name apart, so it keys the remembered place (#265).
		LevelName = InWorld->GetName();
		LevelKey = UMantleSettings::LevelKey(InWorld);
	}
}

FString FMantleLevelPublishSource::GetLevelName() const
{
	return LevelName;
}

bool FMantleLevelPublishSource::Prepare(FMantleValidationResult& OutValidation, FString& OutMessage)
{
	UWorld* Level = World.Get();
	if (!Level)
	{
		OutMessage = TEXT("The level being published was closed. Open it and choose Publish again; nothing is uploaded twice.");
		return false;
	}
	// GhzGarage/mantle#818: a World Partition level publishes whole. Every cell is loaded first (and let go in EndPublish), so the
	// check and the export see all of it instead of refusing the cells the editor had not loaded.
	if (!LoadedCells.IsValid() && LoadedLevelInstances.Num() == 0)
	{
		FMantlePlaceWorldPartition::FLoadResult Loaded = FMantlePlaceWorldPartition::LoadAll(Level);
		LoadedCells = Loaded.Adapter;
		LoadedLevelInstances = Loaded.LevelInstances;
		if (Loaded.bLoaded())
		{
			WorldPartitionNote = FMantlePlaceWorldPartition::DescribeLoad(Loaded);
		}
	}
	// Validation first. Uploading a level with unpublishable content would either lose it
	// silently or fail server-side with a worse message.
	OutValidation = FMantlePlaceValidator::Validate(Level, bThinInstances);
	if (!WorldPartitionNote.IsEmpty())
	{
		OutValidation.Issues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Warning, FString(), WorldPartitionNote,
			TEXT("They are unloaded again when the publish ends.") });
	}
	// The notes travel in the place file, and the backend refuses notes past the schema's limit
	// only at the draft save, after every upload (#393).
	FMantlePlaceValidator::CheckRevisionNotes(Notes, OutValidation);

	// A level's place is remembered per API and account (#394), so the publish has to know both
	// before it looks the place up. Only when the check passed: nothing is sent for a level that
	// cannot publish anyway.
	if (!OutValidation.HasErrors())
	{
		// The address requests actually go to (the editor points it at UMantleSettings).
		LinkApi = FMantleApiClient::BaseUrlResolver();
		FString WhyNot;
		if (!FMantleCreatorSignIn::ResolveAccountId(LinkAccount, WhyNot))
		{
			OutMessage = WhyNot + TEXT(" Sign in again (Tools > Mantle) and choose Publish again; nothing was uploaded.");
			return false;
		}
	}
	Dependencies = FMantlePlaceDependencies::Gather(Level);
	// #804: Gather holds its objects weakly, and some were loaded only to be gathered: hold them until the publish ends, so a
	// collection between conversion steps (the editor's own, or the headless publish's) cannot free what a later step converts.
	Pins.Pin(Dependencies);
	// #556 item 5: the UI/ folder's images take the texture upload a decal's texture takes. Check
	// this place (above) already refused a missing, oversized or unreadable one.
	UIImages.Reset();
	{
		TArray<FMantleUIDocumentFile> UIFiles;
		TArray<FMantleUIImageFile> UIImageFiles;
		FMantlePlaceExporter::CollectUIDocumentFiles(FMantlePlaceExporter::GetUIFolder(Level), UIFiles, UIImageFiles);
		for (const FMantleUIImageFile& Image : UIImageFiles)
		{
			UIImages.Emplace(Image.Path, Image.DiskPath);
		}
	}
	// #786: the catalogue picture, captured after everything else converts. Without a renderer (a headless publish)
	// the place publishes without one.
	FString NoCapture;
	// A custom image (#124) needs no renderer.
	bThumbnailStep = FMantlePlaceThumbnail::CanCapture(&NoCapture) || FMantlePlaceThumbnail::ChoiceFor(Level).Mode == EMantleThumbnailMode::Custom;
	if (!bThumbnailStep)
	{
		UE_LOG(LogMantle, Display, TEXT("MANTLE_PUBLISH thumbnail skipped (%s)"), *NoCapture);
	}
	if (!Converter.IsValid())
	{
		Converter = MakeShared<FMantlePlaceAssetConverter>();
	}
	return true;
}

int32 FMantleLevelPublishSource::NumConversionSteps() const
{
	return FMantlePlaceAssetConverter::NumDependencySteps(Dependencies) + UIImages.Num() + (bThumbnailStep ? 1 : 0);
}

FString FMantleLevelPublishSource::ConversionStepName(int32 Step) const
{
	if (Step < FMantlePlaceAssetConverter::NumDependencySteps(Dependencies))
	{
		return FMantlePlaceAssetConverter::DependencyStepName(Dependencies, Step);
	}
	// The UI/ folder's images (#556 item 5), after everything else.
	const int32 UIImage = Step - FMantlePlaceAssetConverter::NumDependencySteps(Dependencies);
	if (UIImages.IsValidIndex(UIImage))
	{
		return UIImages[UIImage].Key;
	}
	// The place's thumbnail (#786), last.
	return bThumbnailStep && UIImage == UIImages.Num() ? FString(TEXT("Place thumbnail")) : FString();
}

void FMantleLevelPublishSource::Convert(int32 Step, FMantlePlaceAssetSet& Set)
{
	if (!Converter.IsValid())
	{
		return;
	}
	if (Step < FMantlePlaceAssetConverter::NumDependencySteps(Dependencies))
	{
		Converter->ConvertDependencyStep(Dependencies, Step, Set);
		return;
	}
	// The UI/ folder's images (#556 item 5), each a texture upload.
	const int32 UIImage = Step - FMantlePlaceAssetConverter::NumDependencySteps(Dependencies);
	if (UIImages.IsValidIndex(UIImage))
	{
		Converter->ConvertUIImage(UIImages[UIImage].Key, UIImages[UIImage].Value, Set);
		return;
	}
	// The place's thumbnail (#786): captured from the level now, a texture upload like the rest. A failed capture is a
	// warning: the place publishes without a picture.
	// Taken once per publish: a regather (NeedsNewAssets) keeps the picture it already has rather than uploading another.
	if (bThumbnailStep && UIImage == UIImages.Num() && Set.Thumbnail.IsEmpty())
	{
		const FMantleThumbnailResult Picture = FMantlePlaceThumbnail::Resolve(World.Get());
		const FString Error = Picture.Error;
		if (Picture.Succeeded())
		{
			UE_LOG(LogMantle, Display, TEXT("MANTLE_PUBLISH %s"), *Picture.Source);
			if (!Picture.Warning.IsEmpty())
			{
				Set.Issues.Add({ EMantleIssueSeverity::Warning, FString(), Picture.Warning, FString() });
			}
			Converter->ConvertThumbnail(Picture.Pixels, FMantlePlaceThumbnail::Width, FMantlePlaceThumbnail::Height, Set);
		}
		else
		{
			Set.Issues.Add({ EMantleIssueSeverity::Warning, FString(),
				FString::Printf(TEXT("The place's thumbnail could not be taken (%s), so it publishes without a picture."), *Error),
				TEXT("Publish again; if it keeps happening, choose another thumbnail in the Publish window's Thumbnail section.") });
		}
	}
}

FString FMantleLevelPublishSource::DescribeConversion() const
{
	if (!Converter.IsValid())
	{
		return FString();
	}
	const FMantlePlaceConversionStats& Stats = Converter->GetStats();
	const int32 FromCache = Stats.MeshesFromCache + Stats.MaterialsFromCache;
	const int32 Converted = Stats.MeshesConverted + Stats.MaterialsConverted;
	UE_LOG(LogMantle, Log, TEXT("MANTLE_F6 converted %d asset(s), %d from the cache, %d GLTFExporter call(s)"),
		Converted + FromCache, FromCache, Stats.FlattenerCalls);
	if (Converted == 0 && FromCache > 0)
	{
		return TEXT("all from the local cache");
	}
	return FromCache > 0 ? FString::Printf(TEXT("%d from the local cache"), FromCache) : FString();
}

bool FMantleLevelPublishSource::CollectScripts(TArray<FMantlePlaceScriptUpload>& OutScripts, FString& OutMessage)
{
	using namespace MantlePlacePublishSessionPrivate;
	OutScripts.Reset();
	UWorld* Level = World.Get();
	if (!Level)
	{
		OutMessage = TEXT("The level being published was closed. Open it and choose Publish again; nothing is uploaded twice.");
		return false;
	}

	// Source is a separate content-addressed upload. The place remains inert JSON and receives
	// only the returned src_ ids. Every unique definition is uploaded; the backend deduplicates
	// unchanged source, so a repeat publish is still cheap and needs no local cache.
	//
	// Scripts come from the level's Scripts/ folder on disk, the same collection Export()
	// independently redoes from the same files: MakeScriptIds is deterministic given the same
	// entries in the same order, so the ids used as upload keys here are exactly the ids Export()
	// assigns to each FMantleScript it writes.
	const FString ScriptsFolder = FMantlePlaceExporter::GetScriptsFolder(Level);
	TArray<FMantleScriptCatalogEntry> Entries;
	TArray<FMantleScriptCollectionIssue> Issues;
	FMantlePlaceExporter::CollectScriptFiles(ScriptsFolder, Entries, Issues);
	TArray<FMantleGraphScriptEntry> GraphEntries;
	TArray<FMantleParseError> GraphErrors;
	TArray<FString> GraphPaths;
	FMantlePlaceExporter::CollectGraphScripts(ScriptsFolder, GraphEntries, GraphErrors, GraphPaths);
	FMantlePlaceExporter::FilterGraphGeneratedFiles(Entries, Issues, GraphPaths);
	if (GraphErrors.Num() > 0)
	{
		OutMessage = FString::Printf(TEXT("Could not compile graph '%s': %s"),
			*GraphErrors[0].Path.Mid(FString(TEXT("/scripts/")).Len()), *GraphErrors[0].Message);
		return false;
	}
	TArray<FString> ScriptPaths;
	for (const FMantleScriptCatalogEntry& Entry : Entries) ScriptPaths.Add(Entry.Path);
	for (const FMantleGraphScriptEntry& Entry : GraphEntries) ScriptPaths.Add(Entry.Path);
	ScriptPaths.Sort();
	const TArray<FString> ScriptIds = FMantlePlaceExporter::MakeScriptIds(ScriptPaths);
	TMap<FString, FString> ScriptIdByPath;
	for (int32 Index = 0; Index < ScriptPaths.Num(); ++Index) ScriptIdByPath.Add(ScriptPaths[Index], ScriptIds[Index]);

	auto AddScript = [&OutScripts](const FString& ScriptId, bool bAuthorSource, const FString& Source, const FString& Language,
		const FString& Path, bool bGraph)
	{
		FMantlePlaceScriptUpload& Script = OutScripts.AddDefaulted_GetRef();
		Script.ScriptId = ScriptId;
		Script.bAuthorSource = bAuthorSource;
		Script.Source = Source;
		Script.Language = Language;
		Script.Path = Path;
		Script.bGraph = bGraph;
	};
	for (const FMantleScriptCatalogEntry& Entry : Entries)
	{
		const FString& ScriptId = ScriptIdByPath.FindChecked(Entry.Path);
		if (Entry.Language != EMantleScriptCatalogLanguage::Luau
			&& Entry.Language != EMantleScriptCatalogLanguage::JavaScript
			&& Entry.Language != EMantleScriptCatalogLanguage::TypeScript
			&& Entry.Language != EMantleScriptCatalogLanguage::Python)
		{
			// Export() would refuse this the same way; fail before uploading anything mislabeled.
			OutMessage = FString::Printf(TEXT("'%s' has a language Mantle cannot publish yet."), *Entry.Path);
			return false;
		}
		if (Entry.Language == EMantleScriptCatalogLanguage::TypeScript)
		{
			FString JavaScript;
			FMantleScriptError CompileError;
			if (!FMantlePlaceExporter::CompileTypeScript(Entry.Source, JavaScript, CompileError))
			{
				OutMessage = FString::Printf(TEXT("Could not compile TypeScript '%s': %s"), *Entry.Path, *CompileError.Message);
				return false;
			}
			AddScript(ScriptId, false, JavaScript, TEXT("javascript"), Entry.Path, false);
			AddScript(ScriptId, true, Entry.Source, TEXT("typescript"), Entry.Path, false);
			continue;
		}
		AddScript(ScriptId, false, Entry.Source, CatalogLanguageName(Entry.Language), Entry.Path, false);
	}
	for (const FMantleGraphScriptEntry& Entry : GraphEntries)
	{
		const FString& ScriptId = ScriptIdByPath.FindChecked(Entry.Path);
		AddScript(ScriptId, false, Entry.CompiledLuau, TEXT("luau"), Entry.Path, true);
		AddScript(ScriptId, true, Entry.AuthorJson, TEXT("graph"), Entry.Path, true);
	}

	// #549: the level's UI files (UI/, beside Scripts/) go up the same way, as rml or rcss text,
	// keyed by place path: Export writes them in the same path order with the same uid_ ids.
	TArray<FMantleUIDocumentFile> UIFiles;
	TArray<FMantleUIImageFile> UIImageFiles;
	FMantlePlaceExporter::CollectUIDocumentFiles(FMantlePlaceExporter::GetUIFolder(Level), UIFiles, UIImageFiles);
	const TArray<FMantleUIDocumentProblem> UIProblems = FMantlePlaceExporter::CheckUIDocumentFiles(UIFiles);
	if (UIProblems.Num() > 0)
	{
		// Check this place refused it first; the level changed since.
		OutMessage = UIProblems[0].Problem + TEXT(" ") + UIProblems[0].Remedy;
		return false;
	}
	for (const FMantleUIDocumentFile& File : UIFiles)
	{
		FMantlePlaceScriptUpload& Upload = OutScripts.AddDefaulted_GetRef();
		Upload.ScriptId = File.Path;
		Upload.Source = File.Text;
		Upload.Language = File.Language;
		Upload.Path = File.Path;
		Upload.bUIDocument = true;
	}
	return true;
}

bool FMantleLevelPublishSource::CheckPlaceSize(FMantlePlaceSizeCheck& OutCheck)
{
	UWorld* Level = World.Get();
	if (!Level)
	{
		return true;
	}
	// GhzGarage/mantle#818: the place file as it would be written, with the ids an upload will give replaced by provisional ones of
	// the same length (nothing is converted or uploaded yet). The backend refuses a file over 8 MiB only at the draft save, after
	// every upload; this finds out first.
	FMantleExportOptions Options;
	Options.Name = LevelName;
	Options.RevisionNotes = Notes;
	Options.bThinInstances = bThinInstances;
	const FMantleExportResult Exported = FMantlePlaceExporter::Export(Level, Options);
	OutCheck.Bytes = Exported.PlaceBytes;
	OutCheck.Instances = Exported.InstanceTotal;
	UE_LOG(LogMantle, Log, TEXT("Place file before upload: %s, %lld instanced meshes%s"), *FMantlePlaceInstancing::DescribeBytes(Exported.PlaceBytes),
		Exported.InstanceTotal, Exported.Thinning.RemovesAnything() ? *FString::Printf(TEXT(" (%s)"), *Exported.Thinning.Describe()) : TEXT(""));
	if (!Exported.bOverBudget)
	{
		return true;
	}
	OutCheck.bOver = true;
	OutCheck.Issues.Add({ EMantleIssueSeverity::Error, FString(), Exported.BudgetProblem, FString() });
	if (!bThinInstances && Exported.ThinPreview.RemovesAnything() && Exported.ThinPreview.bFits)
	{
		OutCheck.ThinOffer = Exported.ThinPreview.Describe();
	}
	return false;
}

EMantlePlaceExportOutcome FMantleLevelPublishSource::Export(const FMantlePlaceAssetSet& Set, const TMap<FString, FString>& ScriptSourceIds,
	const TMap<FString, FString>& ScriptAuthorSourceIds, FString& OutPlaceJson, TArray<FMantleValidationIssue>& OutIssues)
{
	UWorld* Level = World.Get();
	if (!Level)
	{
		OutIssues.Add({ EMantleIssueSeverity::Error, FString(), TEXT("The level being published was closed."),
			TEXT("Open it and choose Publish again; nothing is uploaded twice.") });
		return EMantlePlaceExportOutcome::Failed;
	}
	FMantleExportOptions Options;
	Options.Name = LevelName;
	Options.RevisionNotes = Notes;
	Options.ScriptSourceIds = ScriptSourceIds;
	Options.ScriptAuthorSourceIds = ScriptAuthorSourceIds;
	Options.UIDocumentSourceIds = UIDocumentSourceIds;
	Options.Assets = &Set;
	Options.bThinInstances = bThinInstances;
	const FMantleExportResult Exported = FMantlePlaceExporter::Export(Level, Options);
	// #628: what the export kept, approximated and dropped, as MANTLE_COVERAGE lines in the log (the publish log gets it below).
	// The materials gathered for the upload, recognised, baked or lost, join the report here: Export leaves them to whoever gathered.
	FMantleCoverage Coverage = Exported.Coverage;
	FMantleCoverageSurvey::AddMaterials(Dependencies, Coverage);
	Coverage.Log();
	if (!Exported.bSuccess)
	{
		for (const FMantleParseError& Error : Exported.Errors)
		{
			OutIssues.Add({ EMantleIssueSeverity::Error, Error.Path, Error.Message, FString() });
		}
		return EMantlePlaceExportOutcome::Failed;
	}
	// The creator kept working while this ran: a mesh or material added since the upload has only
	// a provisional id, which the server would refuse as missing. Gather again instead.
	TSet<FString> Recorded;
	for (const TPair<FString, FMantlePlaceUploadedRef>& Pair : Set.Uploaded)
	{
		Recorded.Add(Pair.Value.Id);
	}
	for (const FMantleAssetRef& Ref : Exported.Place.Assets)
	{
		if (MantlePlacePublishSessionPrivate::IsUploadedAssetId(Ref.Id) && !Recorded.Contains(Ref.Id))
		{
			return EMantlePlaceExportOutcome::NeedsNewAssets;
		}
	}
	OutPlaceJson = FMantlePlaceExporter::ToJson(Exported.Place);

	// The ids scripts use for the Script Assets, which exist only once they are uploaded: the
	// publish log names each one, and so does the settings actor's read-only list.
	ExportNotes.Reset();
	// GhzGarage/mantle#818: what was done to fit the place, never silently.
	if (Exported.Grouping.Actors > 0)
	{
		ExportNotes.Add(FString::Printf(TEXT("Grouped %d identical mesh actors into %d instance sets"), Exported.Grouping.Actors, Exported.Grouping.Sets));
	}
	if (Exported.Thinning.bApplied && Exported.Thinning.RemovesAnything())
	{
		ExportNotes.Add(Exported.Thinning.Describe());
	}
	ExportNotes.Add(FString::Printf(TEXT("Place file: %s, %lld instanced meshes"), *FMantlePlaceInstancing::DescribeBytes(Exported.PlaceBytes), Exported.InstanceTotal));
	TArray<FString> Ids;
	for (const FMantleScriptAssetId& Asset : Exported.ScriptAssets)
	{
		ExportNotes.Add(FString::Printf(TEXT("Script asset %s = %s"), *Asset.Path, *Asset.Id));
		Ids.Add(FString::Printf(TEXT("%s = %s"), *Asset.Path, *Asset.Id));
	}
	// #628: the coverage report in the publish log: what the place kept, approximated and dropped, by category.
	if (!Coverage.IsEmpty())
	{
		ExportNotes.Add(FString::Printf(TEXT("Export coverage: %s"), *Coverage.ToSummary()));
		ExportNotes.Append(Coverage.ToDisplayLines());
	}
	if (AMantlePlaceSettingsActor* Settings = AMantlePlaceSettingsActor::Find(Level))
	{
		// Transient, so setting it does not mark the level as changed.
		Settings->PublishedScriptAssetIds = MoveTemp(Ids);
	}
	return EMantlePlaceExportOutcome::Exported;
}

FString FMantleLevelPublishSource::FindPlaceId()
{
	return UMantleSettings::Get()->FindPlaceForLevel(LinkApi, LinkAccount, LevelKey);
}

void FMantleLevelPublishSource::RememberPlaceId(const FString& PlaceId)
{
	UMantleSettings::GetMutable()->RememberPlaceForLevel(LinkApi, LinkAccount, LevelKey, PlaceId);
	UE_LOG(LogMantle, Log, TEXT("Level '%s' publishes to place %s (account %s at %s)"), *LevelKey, *PlaceId, *LinkAccount, *LinkApi);
}

void FMantleLevelPublishSource::ForgetPlaceId()
{
	UE_LOG(LogMantle, Warning, TEXT("The place for level '%s' is not one account %s can publish to at %s; unlinking it for that account"),
		*LevelKey, *LinkAccount, *LinkApi);
	UMantleSettings::GetMutable()->UnlinkPlaceForLevel(LinkApi, LinkAccount, LevelKey);
}

FString FMantleLevelPublishSource::FindLibraryItem(const FString& SourcePath)
{
	return UMantleSettings::Get()->FindLibraryItemForVehicle(LinkApi, LinkAccount, SourcePath);
}

void FMantleLevelPublishSource::RememberLibraryItem(const FString& SourcePath, const FString& ItemId)
{
	UMantleSettings::GetMutable()->RememberLibraryItemForVehicle(LinkApi, LinkAccount, SourcePath, ItemId);
}

void FMantleLevelPublishSource::ForgetLibraryItem(const FString& SourcePath)
{
	UMantleSettings::GetMutable()->ForgetLibraryItemForVehicle(LinkApi, LinkAccount, SourcePath);
}

int32 FMantleLevelPublishSource::LibraryGeneration(const FString& SourcePath)
{
	return UMantleSettings::Get()->FindLibraryGenerationForVehicle(LinkApi, LinkAccount, SourcePath);
}

void FMantleLevelPublishSource::ApplyPublishedVehicles(TArray<FMantleStoredVehicle>& Stored)
{
	UWorld* Level = World.Get();
	if (!Level || Stored.Num() == 0)
	{
		return;
	}
	const FScopedTransaction Transaction(NSLOCTEXT("MantlePublish", "StoredVehiclesUndo", "Store published vehicles in place storage"));
	const TCHAR* const StoredKind = FMantleStorage::KindVehicle;
	AMantlePlaceSettingsActor* Settings = AMantlePlaceSettingsActor::FindOrAdd(Level);
	if (!Settings)
	{
		return;
	}
	{
		// The Details panel (and the Library panel) refresh from this, as for an edit made in the panel itself.
		FProperty* Property = FindFProperty<FProperty>(AMantlePlaceSettingsActor::StaticClass(), GET_MEMBER_NAME_CHECKED(AMantlePlaceSettingsActor, Storage));
		Settings->Modify();
		Settings->PreEditChange(Property);
		TArray<FMantleStorageEntry> Entries;
		for (const FMantleStorageSetting& Setting : Settings->Storage)
		{
			FMantleStorageEntry& Entry = Entries.AddDefaulted_GetRef();
			Entry.Name = Setting.Name;
			Entry.Item = Setting.Item;
			Entry.Version = Setting.Version;
			Entry.Settings = Setting.Settings; // #838 stage 6: a stored package's settings survive a vehicle or weapon publish
		}
		FMantleLibraryPublish::MergeIntoStorage(Entries, Stored);
		// #838: every entry keeps the kind it had; the ones this publish stored are recorded as what they are.
		TMap<FString, FString> KindByItem;
		for (const FMantleStoredVehicle& StoredItem : Stored)
		{
			KindByItem.Add(StoredItem.ItemId, StoredKind);
		}
		Settings->ReplaceStorageKeepingKinds(Entries, KindByItem);
		FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
		Settings->PostEditChangeProperty(Event);
	}
	// The spawns of those vehicles now park the stored item: what the published place says. Exporting them again
	// uploads nothing for the vehicle (a spawn with no Vehicle gathers no conversion).
	for (TActorIterator<AMantleVehicleSpawnPoint> It(Level); It; ++It)
	{
		AMantleVehicleSpawnPoint* Spawn = *It;
		const FString SpawnPath = Spawn->Vehicle.ToSoftObjectPath().ToString();
		const FMantleStoredVehicle* Match = Stored.FindByPredicate([&SpawnPath](const FMantleStoredVehicle& Vehicle)
		{
			// Identical vehicles share one upload, so one stored vehicle answers for every source path that made it.
			return !SpawnPath.IsEmpty() && Vehicle.Paths.ContainsByPredicate([&SpawnPath](const FString& Path) { return Path.Equals(SpawnPath, ESearchCase::CaseSensitive); });
		});
		if (!Match)
		{
			continue;
		}
		FProperty* VehicleProperty = FindFProperty<FProperty>(AMantleVehicleSpawnPoint::StaticClass(), GET_MEMBER_NAME_CHECKED(AMantleVehicleSpawnPoint, Vehicle));
		Spawn->Modify();
		Spawn->PreEditChange(VehicleProperty);
		Spawn->Vehicle.Reset();
		Spawn->VehicleId.Reset();
		Spawn->StorageItem = Match->StorageName;
		FPropertyChangedEvent Event(VehicleProperty, EPropertyChangeType::ValueSet);
		// Protected on the spawn point; UObject's is public and virtual.
		static_cast<UObject*>(Spawn)->PostEditChangeProperty(Event);
	}
	UE_LOG(LogMantle, Log, TEXT("Level '%s': %d published vehicle(s) are now stored in Place Settings > Storage"), *LevelKey, Stored.Num());
}

TArray<FMantleLibraryScriptFile> FMantlePackagePublish::LibraryFiles() const
{
	TArray<FMantleLibraryScriptFile> Out;
	if (SourceIds.Num() != Files.Num())
	{
		return Out;
	}
	for (int32 Index = 0; Index < Files.Num(); ++Index)
	{
		if (SourceIds[Index].IsEmpty())
		{
			Out.Reset();
			return Out;
		}
		FMantleLibraryScriptFile& File = Out.AddDefaulted_GetRef();
		File.Path = Files[Index].Path;
		File.Side = Files[Index].Side;
		File.Language = Files[Index].Language;
		File.Source = SourceIds[Index];
	}
	return Out;
}

bool FMantleLevelPublishSource::CollectPackages(TArray<FMantlePackagePublish>& OutPackages, FString& OutMessage)
{
	OutPackages.Reset();
	UWorld* Level = World.Get();
	if (!Level)
	{
		OutMessage = TEXT("The level being published was closed. Open it and choose Publish again; nothing is uploaded twice.");
		return false;
	}
	TArray<FMantleLocalScriptPackage> Local;
	TArray<FMantleScriptPackageProblem> Problems;
	FMantleScriptPackages::Collect(FMantleScriptPackages::GetPackagesFolder(Level), Local, Problems);
	for (const FMantleScriptPackageProblem& Problem : Problems)
	{
		if (Problem.bError)
		{
			// Check this place refused it first; the folder changed since.
			OutMessage = Problem.Problem + TEXT(" ") + Problem.Remedy;
			return false;
		}
	}
	for (const FMantleLocalScriptPackage& Package : Local)
	{
		FMantlePackagePublish& Publish = OutPackages.AddDefaulted_GetRef();
		Publish.Name = Package.Name;
		Publish.MemoryKey = FMantleScriptPackages::MemoryKey(Package);
		Publish.Manifest = Package.Manifest;
		if (!FMantleScriptPackages::PrepareFiles(Package, Publish.Files, OutMessage))
		{
			OutPackages.Reset();
			return false;
		}
		Publish.SourceIds.SetNum(Publish.Files.Num());
	}
	return true;
}

void FMantleLevelPublishSource::ApplyPublishedPackages(const TArray<FMantleLibraryPackageRef>& Stored)
{
	UWorld* Level = World.Get();
	if (!Level || Stored.Num() == 0)
	{
		return;
	}
	const FScopedTransaction Transaction(NSLOCTEXT("MantlePublish", "StoredPackagesUndo", "Store published script packages in place storage"));
	AMantlePlaceSettingsActor* Settings = AMantlePlaceSettingsActor::FindOrAdd(Level);
	if (!Settings)
	{
		return;
	}
	FProperty* Property = FindFProperty<FProperty>(AMantlePlaceSettingsActor::StaticClass(), GET_MEMBER_NAME_CHECKED(AMantlePlaceSettingsActor, Storage));
	Settings->Modify();
	Settings->PreEditChange(Property);
	TArray<FMantleStorageEntry> Entries;
	AMantlePlaceSettingsActor::ApplyStorageTo(Level, Entries);
	FMantleLibraryPublish::MergePackagesIntoStorage(Entries, Stored);
	TMap<FString, FString> KindByItem;
	for (const FMantleLibraryPackageRef& Package : Stored)
	{
		KindByItem.Add(Package.ItemId, FMantleStorage::KindScript);
	}
	Settings->ReplaceStorageKeepingKinds(Entries, KindByItem);
	// The level keeps each package's manifest, so a Script Point's Details panel can show its parameters from storage too.
	for (const FMantleLibraryPackageRef& Package : Stored)
	{
		for (FMantleStorageSetting& Setting : Settings->Storage)
		{
			if (Setting.Name.Equals(Package.Name, ESearchCase::CaseSensitive))
			{
				Setting.Manifest = Package.Manifest;
			}
		}
	}
	FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
	Settings->PostEditChangeProperty(Event);
	UE_LOG(LogMantle, Log, TEXT("Level '%s': %d published script package(s) are now stored in Place Settings > Storage"), *LevelKey, Stored.Num());
}

void FMantleLevelPublishSource::ApplyPublishedWeapons(TArray<FMantleStoredVehicle>& Stored)
{
	UWorld* Level = World.Get();
	if (!Level || Stored.Num() == 0)
	{
		return;
	}
	const FScopedTransaction Transaction(NSLOCTEXT("MantlePublish", "StoredWeaponsUndo", "Store published weapons in place storage"));
	const TCHAR* const StoredKind = FMantleStorage::KindWeapon;
	AMantlePlaceSettingsActor* Settings = AMantlePlaceSettingsActor::FindOrAdd(Level);
	if (!Settings)
	{
		return;
	}
	{
		FProperty* Property = FindFProperty<FProperty>(AMantlePlaceSettingsActor::StaticClass(), GET_MEMBER_NAME_CHECKED(AMantlePlaceSettingsActor, Storage));
		Settings->Modify();
		Settings->PreEditChange(Property);
		TArray<FMantleStorageEntry> Entries;
		for (const FMantleStorageSetting& Setting : Settings->Storage)
		{
			FMantleStorageEntry& Entry = Entries.AddDefaulted_GetRef();
			Entry.Name = Setting.Name;
			Entry.Item = Setting.Item;
			Entry.Version = Setting.Version;
			Entry.Settings = Setting.Settings; // #838 stage 6: a stored package's settings survive a vehicle or weapon publish
		}
		FMantleLibraryPublish::MergeIntoStorage(Entries, Stored);
		// #838: every entry keeps the kind it had; the ones this publish stored are recorded as what they are.
		TMap<FString, FString> KindByItem;
		for (const FMantleStoredVehicle& StoredItem : Stored)
		{
			KindByItem.Add(StoredItem.ItemId, StoredKind);
		}
		Settings->ReplaceStorageKeepingKinds(Entries, KindByItem);
		FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
		Settings->PostEditChangeProperty(Event);
	}
	// The spawns of those weapons now name the stored item: what the published place says.
	for (TActorIterator<AMantleWeaponSpawnPoint> It(Level); It; ++It)
	{
		AMantleWeaponSpawnPoint* Spawn = *It;
		const FString SpawnPath = AMantleWeaponSpawnPoint::IsBuilderName(Spawn->Weapon) ? TEXT("/Game/MantleWeapons/") + Spawn->Weapon : FString();
		const FMantleStoredVehicle* Match = Stored.FindByPredicate([&SpawnPath](const FMantleStoredVehicle& Weapon)
		{
			return !SpawnPath.IsEmpty() && Weapon.Paths.ContainsByPredicate([&SpawnPath](const FString& Path) { return Path.Equals(SpawnPath, ESearchCase::CaseSensitive); });
		});
		if (!Match)
		{
			continue;
		}
		FProperty* WeaponProperty = FindFProperty<FProperty>(AMantleWeaponSpawnPoint::StaticClass(), GET_MEMBER_NAME_CHECKED(AMantleWeaponSpawnPoint, Weapon));
		Spawn->Modify();
		Spawn->PreEditChange(WeaponProperty);
		Spawn->Weapon.Reset();
		Spawn->WeaponId.Reset();
		Spawn->StorageItem = Match->StorageName;
		FPropertyChangedEvent Event(WeaponProperty, EPropertyChangeType::ValueSet);
		// Protected on the spawn point; UObject's is public and virtual.
		static_cast<UObject*>(Spawn)->PostEditChangeProperty(Event);
	}
	UE_LOG(LogMantle, Log, TEXT("Level '%s': %d published weapon(s) are now stored in Place Settings > Storage"), *LevelKey, Stored.Num());
}

FString FMantleLevelPublishSource::AttributionFor(const FString& ObjectPath) const
{
	return FMantleAttribution::ForObjectPath(ObjectPath);
}

FString FMantleLevelPublishSource::GetSpillDirectory() const
{
	// The folder is known before Prepare makes the converter (the session sweeps old spill folders at Start).
	return FPaths::Combine(Converter.IsValid() ? Converter->GetCacheDirectory() : FMantlePlaceAssetConverter::DefaultCacheDirectory(), TEXT("Spill"));
}

void FMantleLevelPublishSource::ConversionFinished()
{
	Pins.Release();
}

void FMantleLevelPublishSource::EndPublish()
{
	Pins.Release();
	// GhzGarage/mantle#818: the cells this publish loaded go again; the ones the creator loaded stay.
	if (LoadedCells.IsValid() || LoadedLevelInstances.Num() > 0)
	{
		FMantlePlaceWorldPartition::FLoadResult Loaded;
		Loaded.Adapter = LoadedCells;
		Loaded.LevelInstances = LoadedLevelInstances;
		FMantlePlaceWorldPartition::ReleaseLoaded(World.Get(), Loaded);
		LoadedCells.Reset();
		LoadedLevelInstances.Reset();
	}
	WorldPartitionNote.Reset();
}

// =============================================================================================
// The session

TArray<TPair<FString, FString>> FMantlePlacePublishSession::CollectAttributions(const FMantlePlaceAssetSet& AssetSet,
	const TFunction<FString(const FString& ObjectPath)>& ForPath)
{
	using MantlePlacePublishSessionPrivate::ConvertedObjectPath;
	TArray<TPair<FString, FString>> Out;
	TMap<FString, int32> IndexById;
	auto Add = [&Out, &IndexById, &AssetSet](const FString& Key, const FString& Attribution)
	{
		const FMantlePlaceUploadedRef* Ref = AssetSet.Uploaded.Find(Key);
		if (!Ref || Ref->Id.IsEmpty())
		{
			return;
		}
		if (const int32* Existing = IndexById.Find(Ref->Id))
		{
			// The same bytes under two keys (one texture in two materials): the first credit wins.
			if (Out[*Existing].Value.IsEmpty())
			{
				Out[*Existing].Value = Attribution;
			}
			return;
		}
		IndexById.Add(Ref->Id, Out.Num());
		Out.Emplace(Ref->Id, Attribution);
	};
	for (const FMantleConvertedMesh& Mesh : AssetSet.Meshes)
	{
		Add(Mesh.Key, FMantleAttribution::Normalize(ForPath(ConvertedObjectPath(Mesh.Path, Mesh.Key))));
	}
	for (const FMantleConvertedMaterial& Material : AssetSet.Materials)
	{
		// A material's textures are baked from it, so they carry its credit.
		const FString Attribution = FMantleAttribution::Normalize(ForPath(ConvertedObjectPath(Material.Path, Material.Key)));
		Add(Material.Key, Attribution);
		TArray<FString> Slots;
		Material.Document.Textures.GetKeys(Slots);
		Slots.Sort();
		for (const FString& Slot : Slots)
		{
			Add(Material.Document.Textures.FindChecked(Slot), Attribution);
		}
	}
	for (const FMantleConvertedSound& Sound : AssetSet.Sounds)
	{
		Add(Sound.Key, FMantleAttribution::Normalize(ForPath(ConvertedObjectPath(Sound.Path, Sound.Key))));
	}
	for (const FMantleConvertedSkeletalMesh& Mesh : AssetSet.SkeletalMeshes)
	{
		Add(Mesh.Key, FMantleAttribution::Normalize(ForPath(ConvertedObjectPath(Mesh.Path, Mesh.Key))));
	}
	for (const FMantleConvertedClip& Clip : AssetSet.Clips)
	{
		// A clip's path is its sequence's; its key also names the mesh it was sampled on.
		Add(Clip.Key, FMantleAttribution::Normalize(ForPath(Clip.Path)));
	}
	for (const FMantleConvertedVehicle& Vehicle : AssetSet.Vehicles)
	{
		Add(Vehicle.Key, FMantleAttribution::Normalize(ForPath(ConvertedObjectPath(Vehicle.Path, Vehicle.Key))));
	}
	return Out;
}

FMantlePlacePublishSession::FMantlePlacePublishSession(TSharedRef<IMantlePlacePublishSource> InSource, TSharedRef<IMantleUploadTransport> InTransport,
	bool bInGoLive, FString InNotes, TFunction<double()> InClock)
	: Source(InSource)
	, Transport(InTransport)
	, bGoLive(bInGoLive)
	, Notes(MoveTemp(InNotes))
	, Clock(InClock ? MoveTemp(InClock) : TFunction<double()>([] { return FPlatformTime::Seconds(); }))
{
}

FMantlePlacePublishSession::~FMantlePlacePublishSession()
{
	// A session dropped before it ended (the panel starts another) still gives back its spilled files and its hold on the caches.
	// Not while the editor is shutting down: static destruction runs after the file manager and the object system are gone.
	if (!IsEngineExitRequested())
	{
		ReleaseHeldMemory();
	}
}

bool FMantlePlacePublishSession::SpillRequested()
{
	return MantlePlacePublishSessionPrivate::CVarSpillConvertedBytes.GetValueOnGameThread() != 0;
}

int32 FMantlePlacePublishSession::SweepStaleSpillFolders(const FString& Root, const FTimespan& MaxAge)
{
	if (Root.IsEmpty())
	{
		return 0;
	}
	IFileManager& Files = IFileManager::Get();
	TArray<FString> Folders;
	Files.FindFiles(Folders, *FPaths::Combine(Root, TEXT("*")), /*Files*/ false, /*Directories*/ true);
	const FDateTime Now = FDateTime::UtcNow();
	int32 Deleted = 0;
	for (const FString& Folder : Folders)
	{
		const FString Path = FPaths::Combine(Root, Folder);
		// A publish writes its files as it converts, so the newest file says when it was last at work.
		TArray<FString> Names;
		Files.FindFiles(Names, *FPaths::Combine(Path, TEXT("*")), /*Files*/ true, /*Directories*/ false);
		FDateTime Newest = Names.Num() > 0 ? FDateTime::MinValue() : Files.GetTimeStamp(*Path);
		for (const FString& Name : Names)
		{
			Newest = FMath::Max(Newest, Files.GetTimeStamp(*FPaths::Combine(Path, Name)));
		}
		if (Now - Newest > MaxAge && Files.DeleteDirectory(*Path, /*RequireExists*/ false, /*Tree*/ true))
		{
			++Deleted;
		}
	}
	return Deleted;
}

void FMantlePlacePublishSession::ReleaseHeldMemory()
{
	if (Queue.IsValid())
	{
		if (Queue->IsRunning())
		{
			// The session has ended; nothing will tick it again.
			Queue->Cancel();
		}
		Queue->ReleaseBytes();
	}
	Set.ReleaseBytes();
	if (!SpillFolder.IsEmpty())
	{
		IFileManager::Get().DeleteDirectory(*SpillFolder, /*RequireExists*/ false, /*Tree*/ true);
		SpillFolder.Reset();
	}
	if (!bMemoryReleased)
	{
		bMemoryReleased = true;
		Source->EndPublish();
		if (bCachesEntered)
		{
			bCachesEntered = false;
			// The last publish or preview export to end drops the baked and reduced copies (never while another is running).
			FMantlePublishCaches::Leave();
		}
	}
}

void FMantlePlacePublishSession::SpillConverted()
{
	if (!Timing.bSpillConvertedBytes)
	{
		// A big level turns it on by itself: past the threshold the converted files would not all fit in memory comfortably.
		if (Timing.AutoSpillBytes <= 0 || Set.HeldBytes() <= Timing.AutoSpillBytes)
		{
			return;
		}
		Timing.bSpillConvertedBytes = true;
		AddLog(FString::Printf(TEXT("The prepared files are over %lld MB, so they are kept on disk from here"), Timing.AutoSpillBytes / (1024 * 1024)));
	}
	if (SpillFolder.IsEmpty())
	{
		const FString Root = Source->GetSpillDirectory();
		if (Root.IsEmpty())
		{
			return;
		}
		SpillFolder = FPaths::Combine(Root, FGuid::NewGuid().ToString(EGuidFormats::Digits));
	}
	FString Error;
	Set.SpillBytes(SpillFolder, Timing.SpillMinBytes, Error);
	if (!Error.IsEmpty() && !bSpillFailureLogged)
	{
		// Not worth stopping for: the file stays in memory and uploads from there.
		bSpillFailureLogged = true;
		AddLog(FString::Printf(TEXT("Some prepared files could not be kept on disk (%s), so they stay in memory"), *Error));
	}
}

FString FMantlePlacePublishSession::GetLevelName() const
{
	return Source->GetLevelName();
}

void FMantlePlacePublishSession::AddLog(const FString& Line)
{
	Log.Add(Line);
	UE_LOG(LogMantle, Log, TEXT("MANTLE_F6 %s"), *Line);
}

void FMantlePlacePublishSession::AddAttention(EMantleIssueSeverity Severity, const FString& Problem, const FString& Remedy)
{
	for (const FMantleValidationIssue& Existing : Result.Attention)
	{
		if (Existing.Problem == Problem)
		{
			return;
		}
	}
	Result.Attention.Add({ Severity, FString(), Problem, Remedy });
}

void FMantlePlacePublishSession::Fail(const FString& Why)
{
	if (IsFinished())
	{
		return;
	}
	FailedPhase = Phase;
	Phase = EMantlePublishPhase::Failed;
	bRequestInFlight = false;
	Result.bSuccess = false;
	Result.Message = Why;
	AddLog(TEXT("Stopped: ") + Why);
	UE_LOG(LogMantle, Log, TEXT("MANTLE_F6 FAILED level=%s"), *GetLevelName());
	ReleaseHeldMemory();
}

void FMantlePlacePublishSession::Finish(const FString& Message)
{
	Phase = EMantlePublishPhase::Done;
	Result.bSuccess = true;
	Result.Message = Message;
	AddLog(Message);
	// #804: the converted files, the queue's bytes, the gathered objects and the bake caches go before the memory is read, so usedMB
	// is what the editor keeps after this publish. The peaks are the process's whole life (FMantlePublishMemory).
	ReleaseHeldMemory();
	UE_LOG(LogMantle, Log, TEXT("MANTLE_F6 %s level=%s place=%s revision=%d%s"), bGoLive ? TEXT("PUBLISHED") : TEXT("SAVED"),
		*GetLevelName(), *Result.PlaceId, Result.RevisionNumber, *FMantlePublishMemory::Sample().ToFields(/*bWithUsed*/ true));
}

void FMantlePlacePublishSession::Start()
{
	if (Phase != EMantlePublishPhase::Idle)
	{
		return;
	}
	// #804: from here to the end, the bake caches belong to this publish (FMantlePublishCaches).
	FMantlePublishCaches::Enter();
	bCachesEntered = true;
	Timing.bSpillConvertedBytes = Timing.bSpillConvertedBytes || SpillRequested();
	if (const int32 AutoSpillMB = MantlePlacePublishSessionPrivate::CVarAutoSpillMB.GetValueOnGameThread(); AutoSpillMB != 0)
	{
		// Below 0: never by itself.
		Timing.AutoSpillBytes = AutoSpillMB > 0 ? static_cast<int64>(AutoSpillMB) * 1024 * 1024 : 0;
	}
	// What a publish that was killed left on disk.
	SweepStaleSpillFolders(Source->GetSpillDirectory(), FTimespan::FromDays(StaleSpillDays));
	Phase = EMantlePublishPhase::Checking;
	AddLog(FString::Printf(TEXT("%s '%s'"), bGoLive ? TEXT("Publishing") : TEXT("Saving"), *GetLevelName()));
	Tick();
}

bool FMantlePlacePublishSession::CanCancel() const
{
	// The publish request is one atomic call: once it is sent, the place either goes live or
	// does not, and a Cancel could only lie about which.
	return !IsFinished() && Phase != EMantlePublishPhase::Idle && !bPublishSent;
}

bool FMantlePlacePublishSession::Cancel()
{
	if (!CanCancel())
	{
		return false;
	}
	++Generation;
	bRequestInFlight = false;
	if (Queue.IsValid())
	{
		Queue->Cancel();
	}
	FailedPhase = Phase;
	Phase = EMantlePublishPhase::Cancelled;
	Result.bSuccess = false;
	Result.bCancelled = true;
	Result.Message = bDraftSaved
		? FString(TEXT("Cancelled. Nothing went live (only your private draft on Mantle was updated). Everything uploaded stays, so Publish again continues from here."))
		: FString(TEXT("Cancelled. Nothing was published. Everything already uploaded stays on Mantle, so Publish again continues from here."));
	AddLog(Result.Message);
	ReleaseHeldMemory();
	return true;
}

void FMantlePlacePublishSession::Tick()
{
	if (IsFinished() || Phase == EMantlePublishPhase::Idle)
	{
		return;
	}
	if (Phase == EMantlePublishPhase::Uploading)
	{
		TickUploading();
		return;
	}
	if (bRequestInFlight || Clock() < NextActionAt)
	{
		return;
	}
	switch (Phase)
	{
	case EMantlePublishPhase::Checking: TickChecking(); return;
	case EMantlePublishPhase::Converting: TickConverting(); return;
	case EMantlePublishPhase::Crediting: TickCrediting(); return;
	case EMantlePublishPhase::Scripts: TickScripts(); return;
	case EMantlePublishPhase::Exporting: TickExporting(); return;
	case EMantlePublishPhase::Library: TickLibrary(); return;
	case EMantlePublishPhase::CreatingPlace:
		{
			AddLog(TEXT("Creating the place on Mantle"));
			FMantleUploadCallRequest Request;
			Request.Call = EMantleUploadCall::CreatePlace;
			Request.Json = MantlePlacePublishSessionPrivate::CreatePlaceBody(GetLevelName());
			SendPlaceCall(Request);
		}
		return;
	case EMantlePublishPhase::Saving:
		{
			AddLog(FString::Printf(TEXT("Saving the place file to %s"), *Result.PlaceId));
			FMantleUploadCallRequest Request;
			Request.Call = EMantleUploadCall::SaveDraft;
			Request.Id = Result.PlaceId;
			Request.Json = Result.PlaceJson;
			SendPlaceCall(Request);
		}
		return;
	case EMantlePublishPhase::Publishing:
		{
			AddLog(FString::Printf(TEXT("Publishing %s"), *Result.PlaceId));
			FMantleUploadCallRequest Request;
			Request.Call = EMantleUploadCall::PublishPlace;
			Request.Id = Result.PlaceId;
			Request.Json = FMantlePublisher::MakePublishBody(Notes);
			bPublishSent = true;
			SendPlaceCall(Request);
		}
		return;
	default:
		return;
	}
}

void FMantlePlacePublishSession::TickChecking()
{
	FString WhyNot;
	FMantleValidationResult Validation;
	if (!Source->Prepare(Validation, WhyNot))
	{
		Fail(WhyNot.IsEmpty() ? FString(TEXT("There is no level open to publish.")) : WhyNot);
		return;
	}
	Result.Validation = MoveTemp(Validation);
	SetIssuesCopied = 0;
	// Too many instanced meshes is the one error thinning can fix, so it goes on to the size check below, which makes the offer.
	const bool bBudgetOnly = !bSizeChecked && Result.Validation.OnlyInstanceBudgetErrors();
	if (Result.Validation.HasErrors() && !bBudgetOnly)
	{
		Fail(FString::Printf(TEXT("This place has %d problem(s) to fix first."), Result.Validation.NumErrors()));
		return;
	}
	// GhzGarage/mantle#818: a place the backend would refuse at the end is refused here, before anything is converted or uploaded.
	if (!bSizeChecked)
	{
		bSizeChecked = true;
		FMantlePlaceSizeCheck Size;
		if (!Source->CheckPlaceSize(Size))
		{
			Result.Validation.Issues.RemoveAll([](const FMantleValidationIssue& Issue) { return Issue.bInstanceBudget; });
			Result.Validation.Issues.Append(Size.Issues);
			Result.ThinOffer = Size.ThinOffer;
			AddLog(FString::Printf(TEXT("The place file would be %s with %lld instanced meshes"), *FMantlePlaceInstancing::DescribeBytes(Size.Bytes), Size.Instances));
			Fail(FString::Printf(TEXT("This place is too big for Mantle (%s, %lld instanced meshes). Nothing was prepared or uploaded.%s"),
				*FMantlePlaceInstancing::DescribeBytes(Size.Bytes), Size.Instances,
				Size.ThinOffer.IsEmpty() ? TEXT("") : TEXT(" Thinning the foliage evenly would fix it; the list below says how many go.")));
			return;
		}
		AddLog(FString::Printf(TEXT("Place file size checked: %s, %lld instanced meshes"), *FMantlePlaceInstancing::DescribeBytes(Size.Bytes), Size.Instances));
	}
	if (Result.Validation.HasErrors())
	{
		Fail(FString::Printf(TEXT("This place has %d problem(s) to fix first."), Result.Validation.NumErrors()));
		return;
	}
	ConversionTotal = Source->NumConversionSteps();
	ConversionDone = 0;
	const int32 ContentSteps = ConversionTotal - Source->NumNonContentSteps();
	AddLog(ContentSteps > 0
		? FString::Printf(TEXT("Checked: %d mesh(es), material(s) and sound(s) from your project to prepare"), ContentSteps)
		: FString(TEXT("Checked: built from Mantle library assets only")));
	Phase = EMantlePublishPhase::Converting;
	TickConverting();
}

void FMantlePlacePublishSession::TickConverting()
{
	// Real time, not the session's clock: this is the editor's frame budget.
	const double Until = FPlatformTime::Seconds() + Timing.ConvertBudgetSeconds;
	while (ConversionDone < ConversionTotal)
	{
		ConversionName = Source->ConversionStepName(ConversionDone);
		Source->Convert(ConversionDone, Set);
		++ConversionDone;
		// #804: between steps, nothing of the step just done is on the stack, and the source pins what later steps still read.
		// Every so many steps, and when the commit has grown a lot since the last collection (a few big steps can do that).
		// GARBAGE_COLLECTION_KEEPFLAGS, never RF_NoFlags: in the editor that frees standalone assets, such as a creator's unsaved edits.
		if (Timing.CollectGarbageEverySteps > 0)
		{
			if (CommitAtLastCollection < 0)
			{
				CommitAtLastCollection = static_cast<int64>(FPlatformMemory::GetStats().UsedVirtual);
			}
			const bool bDueByCount = ConversionDone % Timing.CollectGarbageEverySteps == 0;
			const bool bDueByGrowth = Timing.CollectGarbageGrowthBytes > 0
				&& static_cast<int64>(FPlatformMemory::GetStats().UsedVirtual) - CommitAtLastCollection >= Timing.CollectGarbageGrowthBytes;
			if (bDueByCount || bDueByGrowth)
			{
				CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
				CommitAtLastCollection = static_cast<int64>(FPlatformMemory::GetStats().UsedVirtual);
			}
		}
		if (FPlatformTime::Seconds() >= Until)
		{
			SpillConverted();
			return;
		}
	}
	SpillConverted();
	ConversionName.Reset();
	// #804: every step has run. The gathered objects are let go now (a regather's Prepare gathers and holds afresh).
	Source->ConversionFinished();
	// Conversion's own findings (F5): shown with Check this place's; errors stop the publish.
	for (; SetIssuesCopied < Set.Issues.Num(); ++SetIssuesCopied)
	{
		Result.Validation.Issues.Add(Set.Issues[SetIssuesCopied]);
	}
	if (const int32 ContentSteps = ConversionTotal - Source->NumNonContentSteps(); ContentSteps > 0)
	{
		const FString Note = Source->DescribeConversion();
		AddLog(FString::Printf(TEXT("Prepared %d mesh(es), material(s) and sound(s)%s: %d texture(s)"), ContentSteps,
			Note.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%s)"), *Note), Set.Textures.Num()));
	}
	if (Set.HasErrors())
	{
		Fail(FString::Printf(TEXT("%d mesh(es), material(s) or sound(s) could not be prepared for Mantle. The list below says why."),
			Set.Issues.FilterByPredicate([](const FMantleValidationIssue& Issue) { return Issue.Severity == EMantleIssueSeverity::Error; }).Num()));
		return;
	}
	if (Timing.CollectGarbageEverySteps > 0)
	{
		// The blocking publish: what the conversion left behind is freed before the uploads hold their share of memory.
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		CommitAtLastCollection = static_cast<int64>(FPlatformMemory::GetStats().UsedVirtual);
	}
	StartUploads();
}

bool FMantlePlacePublishSession::PrepareMaterial(const FString& Key, FMantleQueuedUpload& Item, FString& OutWhyNot)
{
	using namespace MantlePlacePublishSessionPrivate;
	FMantleConvertedMaterial* Material = Set.Materials.FindByPredicate([&Key](const FMantleConvertedMaterial& Each) { return Each.Key == Key; });
	if (!Material)
	{
		OutWhyNot = FString::Printf(TEXT("Material '%s' was not prepared."), *Item.Name);
		return false;
	}
	TArray<FString> Slots;
	Material->Document.Textures.GetKeys(Slots);
	Slots.Sort();
	for (const FString& Slot : Slots)
	{
		const FString TextureKey = Material->Document.Textures.FindChecked(Slot);
		const FMantlePlaceUploadedRef* Ref = Set.Uploaded.Find(TextureKey);
		if (Ref && !Ref->Id.IsEmpty())
		{
			continue;
		}
		const FMantleQueuedUploadStatus* Status = Queue.IsValid() ? Queue->FindStatus(TextureKey) : nullptr;
		const FMantleConvertedTexture* Texture = Set.FindTexture(TextureKey);
		const FString TextureName = Texture ? Texture->Name : FString(TEXT("one of its textures"));
		if (Status && Status->Code == TEXT("uploaded_differently"))
		{
			// Not worth stopping a publish for: the material keeps its flat values for this map.
			Material->Document.Textures.Remove(Slot);
			DroppedTextures.Add(TextureKey);
			AddAttention(EMantleIssueSeverity::Warning,
				FString::Printf(TEXT("Material '%s' goes live without its %s map. %s"), *Material->Name, SlotWords(Slot), *Status->Message),
				FString::Printf(TEXT("To keep the map, give '%s' pixels that differ from the other image (a copy with a small change is enough), then Publish again."),
					*TextureName));
			AddLog(FString::Printf(TEXT("%s goes live without its %s map"), *Material->Name, SlotWords(Slot)));
			continue;
		}
		OutWhyNot = FString::Printf(TEXT("Material '%s' was not uploaded because its %s map ('%s') could not be."),
			*Material->Name, SlotWords(Slot), *TextureName);
		return false;
	}
	FString Error;
	if (!Set.MaterialBytes(*Material, Item.Bytes, Error))
	{
		OutWhyNot = Error;
		return false;
	}
	Item.Hash = FMantleSha256::AssetHash(Item.Bytes);
	return true;
}

bool FMantlePlacePublishSession::PrepareVehicle(const FString& Key, FMantleQueuedUpload& Item, FString& OutWhyNot)
{
	const FMantleConvertedVehicle* Vehicle = Set.FindVehicle(Key);
	if (!Vehicle)
	{
		OutWhyNot = FString::Printf(TEXT("Vehicle '%s' was not prepared."), *Item.Name);
		return false;
	}
	// Its document names its materials' ids, which the material stage has just recorded.
	if (!Set.VehicleBytes(*Vehicle, Item.Bytes, OutWhyNot))
	{
		return false;
	}
	Item.Hash = FMantleSha256::AssetHash(Item.Bytes);
	return true;
}

bool FMantlePlacePublishSession::PrepareWeapon(const FString& Key, FMantleQueuedUpload& Item, FString& OutWhyNot)
{
	const FMantleConvertedWeapon* Weapon = Set.FindWeapon(Key);
	if (!Weapon)
	{
		OutWhyNot = FString::Printf(TEXT("Weapon '%s' was not prepared."), *Item.Name);
		return false;
	}
	// Its document names its 3D model's and materials' ids, which the earlier stages have just recorded.
	if (!Set.WeaponBytes(*Weapon, Item.Bytes, OutWhyNot))
	{
		return false;
	}
	Item.Hash = FMantleSha256::AssetHash(Item.Bytes);
	return true;
}

void FMantlePlacePublishSession::StartUploads()
{
	// A credit Mantle would refuse is said now, before anything is sent, rather than after the uploads.
	auto CheckCredit = [this](const FString& Name, const FString& Path, const FString& Key)
	{
		if (FMantleAttribution::IsWithinLimit(Source->AttributionFor(MantlePlacePublishSessionPrivate::ConvertedObjectPath(Path, Key))))
		{
			return true;
		}
		Fail(FString::Printf(TEXT("The credit for '%s' is longer than the %d characters Mantle shows. Shorten its Credits rule ")
			TEXT("(Project Settings > Plugins > Mantle Credits) or its MantleAttribution tag, then Publish again."),
			*Name, FMantleAttribution::MaxCharacters));
		return false;
	};
	for (const FMantleConvertedMesh& Mesh : Set.Meshes)
	{
		if (!CheckCredit(Mesh.Name, Mesh.Path, Mesh.Key))
		{
			return;
		}
	}
	for (const FMantleConvertedMaterial& Material : Set.Materials)
	{
		if (!CheckCredit(Material.Name, Material.Path, Material.Key))
		{
			return;
		}
	}
	for (const FMantleConvertedSound& Sound : Set.Sounds)
	{
		if (!CheckCredit(Sound.Name, Sound.Path, Sound.Key))
		{
			return;
		}
	}
	for (const FMantleConvertedSkeletalMesh& Mesh : Set.SkeletalMeshes)
	{
		if (!CheckCredit(Mesh.Name, Mesh.Path, Mesh.Key))
		{
			return;
		}
	}
	for (const FMantleConvertedClip& Clip : Set.Clips)
	{
		if (!CheckCredit(Clip.Name, Clip.Path, Clip.Key))
		{
			return;
		}
	}
	for (const FMantleConvertedVehicle& Vehicle : Set.Vehicles)
	{
		if (!CheckCredit(Vehicle.Name, Vehicle.Path, Vehicle.Key))
		{
			return;
		}
	}

	TArray<FMantleQueuedUpload> Items;
	FString Error;
	for (const EMantlePlaceUploadStage Stage : { EMantlePlaceUploadStage::Texture, EMantlePlaceUploadStage::Mesh, EMantlePlaceUploadStage::SkeletalMesh,
			 EMantlePlaceUploadStage::AnimClip, EMantlePlaceUploadStage::Sound, EMantlePlaceUploadStage::Heightfield })
	{
		TArray<FMantlePlaceUpload> Uploads;
		// #804: the set's bytes move into the queue's items (one copy, not two); a key an earlier call took (this is a
		// regather) is left out, because its upload was recorded or refused by then.
		if (!Set.TakeUploads(Stage, Uploads, Error))
		{
			Fail(Error);
			return;
		}
		for (FMantlePlaceUpload& Upload : Uploads)
		{
			FMantleQueuedUpload& Item = Items.AddDefaulted_GetRef();
			Item.Key = Upload.Key;
			Item.Stage = static_cast<int32>(Upload.Stage);
			Item.Name = Upload.SourceName;
			Item.Options = Upload.Options;
			Item.Bytes = MoveTemp(Upload.Bytes);
			Item.File = Upload.File;
			Item.FileSize = Upload.FileSize;
			Item.Hash = Upload.Hash;
		}
	}
	// Materials name their textures' ids, so their bytes are built when their stage opens.
	const TWeakPtr<FMantlePlacePublishSession> WeakThis = AsShared();
	for (const FMantleConvertedMaterial& Material : Set.Materials)
	{
		FMantleQueuedUpload& Item = Items.AddDefaulted_GetRef();
		Item.Key = Material.Key;
		Item.Stage = static_cast<int32>(EMantlePlaceUploadStage::Material);
		Item.Name = Material.Name;
		Item.Options.Kind = TEXT("material");
		const FString Key = Material.Key;
		Item.Prepare = [WeakThis, Key](FMantleQueuedUpload& Out, FString& OutWhyNot)
		{
			const TSharedPtr<FMantlePlacePublishSession> This = WeakThis.Pin();
			return This.IsValid() && This->PrepareMaterial(Key, Out, OutWhyNot);
		};
	}
	// Creator vehicles (#435), last: their documents name their materials' ids.
	for (const FMantleConvertedVehicle& Vehicle : Set.Vehicles)
	{
		FMantleQueuedUpload& Item = Items.AddDefaulted_GetRef();
		Item.Key = Vehicle.Key;
		Item.Stage = static_cast<int32>(EMantlePlaceUploadStage::Vehicle);
		Item.Name = Vehicle.Name;
		Item.Options.Kind = TEXT("vehicle");
		const FString Key = Vehicle.Key;
		Item.Prepare = [WeakThis, Key](FMantleQueuedUpload& Out, FString& OutWhyNot)
		{
			const TSharedPtr<FMantlePlacePublishSession> This = WeakThis.Pin();
			return This.IsValid() && This->PrepareVehicle(Key, Out, OutWhyNot);
		};
	}
	// Built weapons (#789), after them: their documents name their model's and materials' ids.
	for (const FMantleConvertedWeapon& Weapon : Set.Weapons)
	{
		FMantleQueuedUpload& Item = Items.AddDefaulted_GetRef();
		Item.Key = Weapon.Key;
		Item.Stage = static_cast<int32>(EMantlePlaceUploadStage::Weapon);
		Item.Name = Weapon.Name;
		Item.Options.Kind = TEXT("weapon");
		const FString Key = Weapon.Key;
		Item.Prepare = [WeakThis, Key](FMantleQueuedUpload& Out, FString& OutWhyNot)
		{
			const TSharedPtr<FMantlePlacePublishSession> This = WeakThis.Pin();
			return This.IsValid() && This->PrepareWeapon(Key, Out, OutWhyNot);
		};
	}
	if (Items.Num() == 0)
	{
		EnterScriptsOrLibrary();
		return;
	}
	bHadUploads = true;
	Queue = MakeShared<FMantleAssetUploadQueue>(Transport, Clock);
	Queue->Settings = QueueSettings;
	Queue->Settings.bWaitForApproval = bGoLive;
	Queue->OnAssetId = [WeakThis](const FMantleQueuedUploadStatus& Status)
	{
		if (const TSharedPtr<FMantlePlacePublishSession> This = WeakThis.Pin())
		{
			This->Set.RecordUpload(Status.Key, Status.AssetId, Status.Hash, Status.Bytes);
			This->NameById.Add(Status.AssetId, Status.Name);
		}
	};
	AddLog(FString::Printf(TEXT("Uploading: %d texture(s), %d material(s), %d mesh(es), %d skeletal mesh(es), %d animation clip(s), %d sound(s), %d terrain heightfield(s), %d vehicle(s), %d weapon(s)"),
		Set.Textures.Num(), Set.Materials.Num(), Set.Meshes.Num(), Set.SkeletalMeshes.Num(), Set.Clips.Num(), Set.Sounds.Num(), Set.Heightfields.Num(), Set.Vehicles.Num(), Set.Weapons.Num()));
	Phase = EMantlePublishPhase::Uploading;
	Queue->Start(MoveTemp(Items));
	TickUploading();
}

void FMantlePlacePublishSession::TickUploading()
{
	if (!Queue.IsValid())
	{
		return;
	}
	Queue->Tick();
	if (!Queue->IsFinished() || IsFinished())
	{
		return;
	}
	// #804: the run is over, so no file is sent again (a save ends with files Mantle is still processing, whose bytes the queue kept).
	Queue->ReleaseBytes();
	switch (Queue->GetOutcome())
	{
	case EMantleUploadQueueOutcome::Failed:
		Fail(Queue->GetFailure());
		return;
	case EMantleUploadQueueOutcome::Cancelled:
		return;
	case EMantleUploadQueueOutcome::WaitingForReview:
		{
			TArray<FString> Names;
			for (const FMantleQueuedUploadStatus& Status : Queue->GetStatuses())
			{
				if (Status.bInReview && !Status.IsFinal())
				{
					Names.Add(Status.Name);
					AddAttention(EMantleIssueSeverity::Warning,
						FString::Printf(TEXT("'%s' is waiting for a Mantle moderator to look at it."), *Status.Name),
						TEXT("Nothing to do but wait: choose Publish again once it is approved."));
				}
			}
			Result.bWaitingForReview = true;
			Fail(FString::Printf(TEXT("Everything is uploaded, but Mantle's moderators have to look at %d file(s) first, such as '%s'. ")
				TEXT("Nothing went live yet. Choose Publish again once they are approved; nothing is uploaded twice."),
				Names.Num(), Names.Num() > 0 ? *Names[0] : TEXT("?")));
		}
		return;
	default:
		break;
	}

	int32 Blocking = 0;
	TArray<const FMantleQueuedUploadStatus*> Blocked;
	for (const FMantleQueuedUploadStatus& Status : Queue->GetStatuses())
	{
		if (Status.State != EMantleQueuedUploadState::Refused)
		{
			continue;
		}
		if (Status.Code == TEXT("uploaded_differently") && Status.Stage == static_cast<int32>(EMantlePlaceUploadStage::Texture))
		{
			// Its material went live without it and says so (PrepareMaterial).
			if (!DroppedTextures.Contains(Status.Key))
			{
				AddAttention(EMantleIssueSeverity::Warning, Status.Message, TEXT("Nothing that goes live uses it."));
			}
			continue;
		}
		if (Status.Code == TEXT("blocked"))
		{
			Blocked.Add(&Status);
			continue;
		}
		// A prepared file that was kept on disk and is gone or changed (#804): preparing it again is the fix.
		AddAttention(EMantleIssueSeverity::Error, Status.Message,
			Status.Code.Equals(TEXT("unreadable"), ESearchCase::CaseSensitive) ? FString(TEXT("Publish again; it prepares the file again.")) : FString());
		++Blocking;
	}
	// A material held back by its texture is said by the texture's own row; only when nothing
	// else explains it does it get one.
	if (Blocking == 0)
	{
		for (const FMantleQueuedUploadStatus* Status : Blocked)
		{
			AddAttention(EMantleIssueSeverity::Error, Status->Message, FString());
			++Blocking;
		}
	}
	if (Blocking > 0)
	{
		Fail(FString::Printf(TEXT("%d file(s) this place uses could not be published, so nothing went live. The list below says why and what to do. ")
			TEXT("Everything else is uploaded and kept: the next Publish only sends what changed."), Blocking));
		return;
	}
	const FMantleUploadQueueProgress& Progress = Queue->GetProgress();
	AddLog(FString::Printf(TEXT("Every file is on Mantle (%d file(s), %.1f MB)"), Progress.AssetsTotal, Progress.BytesTotal / (1024.0 * 1024.0)));
	Phase = EMantlePublishPhase::Crediting;
	bCreditsCollected = false;
	NextActionAt = 0.0;
	Tick();
}

void FMantlePlacePublishSession::TickCrediting()
{
	if (!bCreditsCollected)
	{
		// Every uploaded asset is sent, uncredited ones empty, so a credit removed from the project
		// is removed from Mantle too; the publish seals what the claims say into the revision.
		const TSharedRef<IMantlePlacePublishSource> From = Source;
		const TArray<TPair<FString, FString>> Entries = CollectAttributions(Set, [From](const FString& ObjectPath) { return From->AttributionFor(ObjectPath); });
		CreditBatches.Reset();
		CreditIndex = 0;
		CreditedAssets = 0;
		Attempts = 0;
		TArray<TPair<FString, FString>> Batch;
		for (const TPair<FString, FString>& Entry : Entries)
		{
			CreditedAssets += Entry.Value.IsEmpty() ? 0 : 1;
			Batch.Add(Entry);
			if (Batch.Num() >= AttributionBatchSize)
			{
				CreditBatches.Add(FMantleAttribution::MakeRequestBody(Batch));
				Batch.Reset();
			}
		}
		if (Batch.Num() > 0)
		{
			CreditBatches.Add(FMantleAttribution::MakeRequestBody(Batch));
		}
		bCreditsCollected = true;
		if (CreditedAssets > 0)
		{
			AddLog(FString::Printf(TEXT("Crediting %d asset(s) on the world page"), CreditedAssets));
		}
	}
	if (CreditBatches.IsValidIndex(CreditIndex))
	{
		FMantleUploadCallRequest Request;
		Request.Call = EMantleUploadCall::SetAttributions;
		Request.Json = CreditBatches[CreditIndex];
		SendPlaceCall(Request);
		return;
	}
	EnterScriptsOrLibrary();
}

void FMantlePlacePublishSession::EnterScriptsOrLibrary()
{
	if (Source->IsLibraryOnly())
	{
		// #840: no place. Every upload is approved by now (the queue waited), which is what a library item needs.
		Attempts = 0;
		bLibraryCollected = false;
		Phase = EMantlePublishPhase::Library;
		TickLibrary();
		return;
	}
	Phase = EMantlePublishPhase::Scripts;
	TickScripts();
}

void FMantlePlacePublishSession::HandleCredited(const FMantleApiResult& Response)
{
	if (!Response.bSuccess)
	{
		if (FMantleCosmeticUploadSession::IsRetryable(Response) && Response.StatusCode != 401 && Timing.RetryDelays.IsValidIndex(Attempts))
		{
			// The same attributions again: writing them twice is harmless.
			NextActionAt = Clock() + FMath::Max(Timing.RetryDelays[Attempts++], Response.RetryAfterSeconds);
			return;
		}
		Fail(FString::Printf(TEXT("Could not save the credits for this place's assets: %s"), *FMantlePublisher::DescribeApiFailure(Response)));
		return;
	}
	++CreditIndex;
	Attempts = 0;
	Tick();
}

void FMantlePlacePublishSession::TickScripts()
{
	if (!bScriptsCollected)
	{
		FString WhyNot;
		if (!Source->CollectScripts(Scripts, WhyNot))
		{
			Fail(WhyNot);
			return;
		}
		// #838: the level's script packages. A go-live uploads their files here too (each becomes a library item version in the
		// Library step, once its sources are on Mantle); a Save keeps them out of the draft.
		if (!Source->CollectPackages(Packages, WhyNot))
		{
			Fail(WhyNot);
			return;
		}
		int32 PackageFiles = 0;
		if (bGoLive)
		{
			for (int32 PackageIndex = 0; PackageIndex < Packages.Num(); ++PackageIndex)
			{
				const FMantlePackagePublish& Package = Packages[PackageIndex];
				for (int32 FileIndex = 0; FileIndex < Package.Files.Num(); ++FileIndex)
				{
					FMantlePlaceScriptUpload& Upload = Scripts.AddDefaulted_GetRef();
					Upload.ScriptId = Package.Files[FileIndex].Path;
					Upload.Source = Package.Files[FileIndex].Source;
					Upload.Language = Package.Files[FileIndex].UploadLanguage;
					Upload.Path = FString::Printf(TEXT("Packages/%s/%s"), *Package.Name, *Package.Files[FileIndex].Path);
					Upload.PackageIndex = PackageIndex;
					Upload.PackageFileIndex = FileIndex;
					++PackageFiles;
				}
			}
		}
		bScriptsCollected = true;
		ScriptIndex = 0;
		Attempts = 0;
		if (Scripts.Num() > 0)
		{
			const int32 UIFiles = static_cast<int32>(Algo::CountIf(Scripts, [](const FMantlePlaceScriptUpload& Row) { return Row.bUIDocument; }));
			AddLog(UIFiles > 0
				? FString::Printf(TEXT("Uploading %d script source(s), %d of them UI file(s)"), Scripts.Num(), UIFiles)
				: FString::Printf(TEXT("Uploading %d script source(s)"), Scripts.Num()));
		}
		if (PackageFiles > 0)
		{
			AddLog(FString::Printf(TEXT("%d of them belong to %d script package(s), which become library items when the place goes live"), PackageFiles, Packages.Num()));
		}
	}
	if (Scripts.IsValidIndex(ScriptIndex))
	{
		const FMantlePlaceScriptUpload& Script = Scripts[ScriptIndex];
		FMantleUploadCallRequest Request;
		Request.Call = EMantleUploadCall::UploadScript;
		Request.Json = Script.Source;
		Request.Language = Script.Language;
		SendPlaceCall(Request);
		return;
	}
	Phase = EMantlePublishPhase::Exporting;
	TickExporting();
}

void FMantlePlacePublishSession::TickExporting()
{
	FString Json;
	TArray<FMantleValidationIssue> Issues;
	Source->SetUIDocumentSourceIds(UIDocumentSourceIds);
	switch (Source->Export(Set, ScriptSourceIds, ScriptAuthorSourceIds, Json, Issues))
	{
	case EMantlePlaceExportOutcome::Failed:
		Result.Validation.Issues.Append(Issues);
		Fail(TEXT("This place could not be exported."));
		return;
	case EMantlePlaceExportOutcome::NeedsNewAssets:
		if (++Regathers > Timing.MaxRegathers)
		{
			Fail(TEXT("The level kept changing while it was being published. Pause editing for a moment and choose Publish again; nothing is uploaded twice."));
			return;
		}
		AddLog(TEXT("The level changed while publishing; preparing and uploading what is new"));
		bCreditsCollected = false;
		bScriptsCollected = false;
		ScriptSourceIds.Reset();
		ScriptAuthorSourceIds.Reset();
		UIDocumentSourceIds.Reset();
		Phase = EMantlePublishPhase::Checking;
		Tick();
		return;
	default:
		break;
	}
	for (const FString& Note : Source->GetExportNotes())
	{
		AddLog(Note);
	}
	Result.PlaceJson = MoveTemp(Json);
	if (Set.Vehicles.Num() > 0 || Set.Weapons.Num() > 0 || Packages.Num() > 0)
	{
		if (bGoLive)
		{
			// Every upload is approved by now (the queue waited), which is what a library item needs.
			Attempts = 0;
			bLibraryCollected = false;
			Phase = EMantlePublishPhase::Library;
			Tick();
			return;
		}
		if (Set.Vehicles.Num() > 0 || Set.Weapons.Num() > 0)
		{
			AddLog(TEXT("Vehicles and weapons are uploaded and saved in the draft by their upload id; they become library items when you publish"));
		}
		if (Packages.Num() > 0)
		{
			// #838: a package is stored only when the place goes live, so a draft's Script Points for one would name nothing.
			FString Kept;
			TArray<FString> Dropped;
			FMantleLibraryPublish::DropUnstoredScriptPoints(Result.PlaceJson, Kept, Dropped);
			Result.PlaceJson = MoveTemp(Kept);
			AddLog(Dropped.Num() > 0
				? FString::Printf(TEXT("Script packages become library items when you publish; until then the draft leaves out %d Script Point(s) that use them (%s)"),
					Dropped.Num(), *FString::Join(Dropped, TEXT(", ")))
				: FString(TEXT("Script packages become library items when you publish; the draft does not store them yet")));
		}
	}
	EnterSaving();
}

void FMantlePlacePublishSession::EnterSaving()
{
	// A level keeps its place across publishes. Creating a new one each time would fill the
	// creator's list with duplicates and break whatever link they already shared.
	Result.PlaceId = Source->FindPlaceId();
	Attempts = 0;
	Phase = Result.PlaceId.IsEmpty() ? EMantlePublishPhase::CreatingPlace : EMantlePublishPhase::Saving;
	Tick();
}

void FMantlePlacePublishSession::TickLibrary()
{
	if (!bLibraryCollected)
	{
		// Only the vehicles the exported place file uses (a level's vehicles all are, but a file is the truth). #840: a library-only
		// publish has no place file: every vehicle and weapon it uploaded is the point.
		const bool bLibraryOnly = Source->IsLibraryOnly();
		const TArray<FString> Used = FMantleLibraryPublish::VehicleAssetIdsIn(Result.PlaceJson);
		LibraryJobs.Reset();
		LibraryRefs.Reset();
		auto FindJobByAsset = [this](const FString& AssetId) -> FLibraryJob*
		{
			return LibraryJobs.FindByPredicate([&AssetId](const FLibraryJob& Job) { return Job.AssetId.Equals(AssetId, ESearchCase::CaseSensitive); });
		};
		for (const FMantleConvertedVehicle& Vehicle : Set.Vehicles)
		{
			const FMantlePlaceUploadedRef* Uploaded = Set.Uploaded.Find(Vehicle.Key);
			if (!Uploaded || (!bLibraryOnly && !Used.Contains(Uploaded->Id)))
			{
				continue;
			}
			const FString SourcePath = MantlePlacePublishSessionPrivate::ConvertedObjectPath(Vehicle.Path, Vehicle.Key);
			// The card's picture (#821): the texture the vehicle uploaded beside it, once it has an id (a vehicle without one has a card without a picture).
			const FMantlePlaceUploadedRef* Picture = Vehicle.ThumbnailKey.IsEmpty() ? nullptr : Set.Uploaded.Find(Vehicle.ThumbnailKey);
			if (FLibraryJob* Same = FindJobByAsset(Uploaded->Id))
			{
				// Identical vehicle bytes are one upload and so one item, but the level has a spawn path for each.
				Same->Paths.AddUnique(SourcePath);
				if (Same->Provenance.ImportFilename.IsEmpty())
				{
					Same->Provenance.ImportFilename = FMantleProvenance::CleanFilename(Vehicle.ImportFilename);
				}
				if (Same->ThumbnailId.IsEmpty() && Picture)
				{
					Same->ThumbnailId = Picture->Id;
				}
				continue;
			}
			FLibraryJob& Job = LibraryJobs.AddDefaulted_GetRef();
			Job.Name = Vehicle.Name;
			Job.Path = SourcePath;
			Job.Paths.Add(SourcePath);
			Job.AssetId = Uploaded->Id;
			if (Picture)
			{
				Job.ThumbnailId = Picture->Id;
			}
			Job.Provenance = FMantleProvenance::Make(SourcePath, Vehicle.ImportFilename);
		}
		// #789: the weapons the place file uses, the same way.
		const TArray<FString> UsedWeapons = FMantleLibraryPublish::WeaponAssetIdsIn(Result.PlaceJson);
		for (const FMantleConvertedWeapon& Weapon : Set.Weapons)
		{
			const FMantlePlaceUploadedRef* Uploaded = Set.Uploaded.Find(Weapon.Key);
			if (!Uploaded || (!bLibraryOnly && !UsedWeapons.Contains(Uploaded->Id)))
			{
				continue;
			}
			const FString SourcePath = MantlePlacePublishSessionPrivate::ConvertedObjectPath(Weapon.Path, Weapon.Key);
			if (FLibraryJob* Same = FindJobByAsset(Uploaded->Id))
			{
				Same->Paths.AddUnique(SourcePath);
				continue;
			}
			FLibraryJob& Job = LibraryJobs.AddDefaulted_GetRef();
			Job.bWeapon = true;
			Job.Name = Weapon.Name;
			Job.Path = SourcePath;
			Job.Paths.Add(SourcePath);
			Job.AssetId = Uploaded->Id;
			Job.Provenance = FMantleProvenance::Make(SourcePath, Weapon.ImportFilename);
			// #124: the card's picture, the texture the weapon uploaded beside it.
			if (const FMantlePlaceUploadedRef* WeaponPicture = Weapon.ThumbnailKey.IsEmpty() ? nullptr : Set.Uploaded.Find(Weapon.ThumbnailKey))
			{
				Job.ThumbnailId = WeaponPicture->Id;
			}
		}
		// #838: each script package, once all its files are on Mantle; remembered by its folder like a vehicle by its asset.
		PackageRefs.Reset();
		for (int32 PackageIndex = 0; PackageIndex < Packages.Num(); ++PackageIndex)
		{
			const FMantlePackagePublish& Package = Packages[PackageIndex];
			if (Package.LibraryFiles().Num() != Package.Files.Num() || Package.Files.Num() == 0)
			{
				Fail(FString::Printf(TEXT("The scripts of package '%s' did not all upload. Choose Publish again; nothing is uploaded twice."), *Package.Name));
				return;
			}
			FLibraryJob& Job = LibraryJobs.AddDefaulted_GetRef();
			Job.bPackage = true;
			Job.PackageIndex = PackageIndex;
			Job.Name = Package.Name;
			Job.Path = Package.MemoryKey;
			Job.Paths.Add(Package.MemoryKey);
		}
		LibraryIndex = 0;
		bLibraryCollected = true;
	}
	if (LibraryJobs.Num() == 0)
	{
		FinishLibrary();
		return;
	}
	if (!VehicleChoice.IsReady())
	{
		// Nothing is sent until the creator has chosen: the panel says so and offers the choice.
		return;
	}
	if (!LibraryJobs.IsValidIndex(LibraryIndex))
	{
		FinishLibrary();
		return;
	}
	StartLibraryJob();
}

void FMantlePlacePublishSession::StartLibraryJob()
{
	FLibraryJob& Job = LibraryJobs[LibraryIndex];
	if (Job.Step == FLibraryJob::EStep::Start)
	{
		// Each job starts from the choice; an existing item may keep its own visibility instead (see the Looking answer).
		bLibraryPublic = VehicleChoice.bPublic;
		for (const FString& SourcePath : Job.Paths)
		{
			Job.ItemId = Source->FindLibraryItem(SourcePath);
			if (!Job.ItemId.IsEmpty())
			{
				break;
			}
		}
		Job.Step = Job.ItemId.IsEmpty() ? FLibraryJob::EStep::Creating : FLibraryJob::EStep::Looking;
	}
	FMantleUploadCallRequest Request;
	Request.Id = Job.ItemId;
	if (Job.bPackage && Packages.IsValidIndex(Job.PackageIndex))
	{
		// #838: a script package's item is its files and manifest; it has no asset, picture or provenance.
		const FMantlePackagePublish& Package = Packages[Job.PackageIndex];
		const TArray<FMantleLibraryScriptFile> Files = Package.LibraryFiles();
		switch (Job.Step)
		{
		case FLibraryJob::EStep::Looking:
			Request.Call = EMantleUploadCall::GetLibraryItem;
			break;
		case FLibraryJob::EStep::Patching:
			Request.Call = EMantleUploadCall::UpdateLibraryItem;
			Request.Json = FMantleLibraryPublish::MakeVisibilityPatchBody(bLibraryPublic);
			break;
		case FLibraryJob::EStep::Versioning:
			Request.Call = EMantleUploadCall::CreateLibraryVersion;
			Request.Json = FMantleLibraryPublish::MakeScriptVersionBody(Package.Manifest, Files);
			break;
		default:
			AddLog(FString::Printf(TEXT("Adding script package '%s' to your library (%s)"), *Job.Name, bLibraryPublic ? TEXT("public") : TEXT("private")));
			Request.Call = EMantleUploadCall::CreateLibraryItem;
			Request.Json = FMantleLibraryPublish::MakeScriptItemBody(Package.Manifest, bLibraryPublic, Files);
			Request.IdempotencyKey = FMantleLibraryPublish::MakeScriptIdempotencyKey(Package.MemoryKey, Package.Manifest, Files, bLibraryPublic,
				Source->LibraryGeneration(Job.Path));
			break;
		}
		SendPlaceCall(Request);
		return;
	}
	switch (Job.Step)
	{
	case FLibraryJob::EStep::Looking:
		Request.Call = EMantleUploadCall::GetLibraryItem;
		break;
	case FLibraryJob::EStep::Patching:
		Request.Call = EMantleUploadCall::UpdateLibraryItem;
		// Keeping the item's visibility: the edit is the card's picture alone (a public body would also restate a rights declaration no one made here).
		Request.Json = VehicleChoice.bKeepExistingVisibility
			? FMantleLibraryPublish::MakeThumbnailPatchBody(Job.ThumbnailId)
			: FMantleLibraryPublish::MakeVisibilityPatchBody(bLibraryPublic, Job.ThumbnailId);
		break;
	case FLibraryJob::EStep::Versioning:
		Request.Call = EMantleUploadCall::CreateLibraryVersion;
		Request.Json = FMantleLibraryPublish::MakeVersionBody(Job.AssetId, bLibraryPublic, Job.Provenance);
		break;
	default:
		AddLog(FString::Printf(TEXT("Adding %s '%s' to your library (%s)"), Job.bWeapon ? TEXT("weapon") : TEXT("vehicle"), *Job.Name, bLibraryPublic ? TEXT("public") : TEXT("private")));
		Request.Call = EMantleUploadCall::CreateLibraryItem;
		Request.Json = FMantleLibraryPublish::MakeItemBody(Job.Name, bLibraryPublic, Job.AssetId, Job.Provenance, Job.bWeapon ? TEXT("weapon") : TEXT("vehicle"), Job.ThumbnailId);
		Request.IdempotencyKey = FMantleLibraryPublish::MakeIdempotencyKey(Job.Path, Job.AssetId, bLibraryPublic, Source->LibraryGeneration(Job.Path));
		break;
	}
	SendPlaceCall(Request);
}

void FMantlePlacePublishSession::HandleLibraryResponse(const FMantleApiResult& Response)
{
	if (!LibraryJobs.IsValidIndex(LibraryIndex))
	{
		return;
	}
	FLibraryJob& Job = LibraryJobs[LibraryIndex];
	using EStep = FLibraryJob::EStep;
	if (!Response.bSuccess)
	{
		const bool bExistingItemStep = Job.Step == EStep::Looking || Job.Step == EStep::Patching || Job.Step == EStep::Versioning;
		if (!Job.bForgotten && bExistingItemStep && Response.StatusCode == 404)
		{
			// The remembered item is gone, or not this account's: forget it and make a new one.
			RestartLibraryJobAsNew(TEXT("no longer exists"));
			return;
		}
		if (!Job.bForgotten && bExistingItemStep && Response.StatusCode == 409 && Response.Code.Equals(TEXT("item_delisted"), ESearchCase::CaseSensitive))
		{
			// Delisted between the read and this request.
			RestartLibraryJobAsNew(TEXT("was delisted"));
			return;
		}
		if (Job.bPackage && Response.StatusCode == 409 && Response.Code.Equals(TEXT("script_unavailable"), ESearchCase::CaseSensitive)
			&& !Response.Message.Contains(TEXT("refused by moderation")))
		{
			// #838: a package's scripts uploaded a moment ago start pending moderation, and a library version takes only approved
			// ones. Nothing was made (409), so the same request is sent again shortly, as a scripts_pending publish is.
			if (Timing.ScriptsPendingDelays.IsValidIndex(Attempts))
			{
				AddLog(TEXT("Mantle is still screening the package's scripts; trying again shortly"));
				NextActionAt = Clock() + Timing.ScriptsPendingDelays[Attempts++];
				return;
			}
			Fail(FString::Printf(TEXT("Mantle is still screening the scripts of package '%s', so nothing went live yet. Choose Publish again in a minute; nothing is uploaded twice."), *Job.Name));
			return;
		}
		if (Job.bPackage && Response.Code.Equals(TEXT("too_many_versions"), ESearchCase::CaseSensitive))
		{
			Fail(FString::Printf(TEXT("The package '%s' already has as many versions in your library as Mantle allows, so nothing went live. Copy it to a package folder with a new name (a new name makes a new library item), then publish again."), *Job.Name));
			return;
		}
		// The read, the edit and the create are repeated (the create carries its Idempotency-Key, so a repeat names the item
		// the first made); a new version that may have reached Mantle is repeated only when Mantle said 429, which means
		// it was not processed.
		if (FMantleLibraryPublish::ShouldRetry(Response, Job.Step != EStep::Versioning) && Timing.RetryDelays.IsValidIndex(Attempts))
		{
			NextActionAt = Clock() + FMath::Max(Timing.RetryDelays[Attempts++], Response.RetryAfterSeconds);
			return;
		}
		Fail(FMantleLibraryPublish::DescribeFailure(Response, Job.Name));
		return;
	}
	if (Job.Step == EStep::Looking)
	{
		FString Visibility;
		FString Status;
		FMantleLibraryPublish::ReadItemState(Response.Body, Visibility, Status);
		if (FMantleLibraryPublish::IsTakenDown(Status))
		{
			// A moderator removed it. Unlike a delisted item it is not replaced by a new one for the same content.
			Fail(FString::Printf(TEXT("'%s' was taken down by a Mantle moderator after a complaint, so it cannot be published again. Use different content."), *Job.Name));
			return;
		}
		if (FMantleLibraryPublish::IsDelisted(Status))
		{
			// A delisted item takes no versions and no edits: it stays delisted, and this vehicle gets a new item.
			RestartLibraryJobAsNew(TEXT("was delisted"));
			return;
		}
		Job.ExistingVersion = 0;
		if (Job.bPackage && Packages.IsValidIndex(Job.PackageIndex))
		{
			// #838: the same files and manifest are already a version (a republish with the package unchanged).
			const FMantlePackagePublish& Package = Packages[Job.PackageIndex];
			FMantleLibraryPublish::FindVersionWithScripts(Response.Body, Package.Manifest, Package.LibraryFiles(), Job.ExistingVersion);
		}
		else
		{
			FMantleLibraryPublish::FindVersionWithAsset(Response.Body, Job.AssetId, Job.ExistingVersion);
		}
		// An agent's or blocking publish is private only because no one can declare rights: it must not undo a public choice the creator made.
		bLibraryPublic = FMantleLibraryPublish::ResolveExistingItemPublic(Visibility, VehicleChoice);
		const bool bVisibilityDiffers = FMantleLibraryPublish::VisibilityDiffers(Visibility, bLibraryPublic);
		if (bVisibilityDiffers || !Job.ThumbnailId.IsEmpty())
		{
			// The creator's choice decides who can use the vehicle, also for an item an earlier publish made. Its card's picture (#821) goes with the same edit: the vehicle
			// may have been built again since, and naming the picture an item already has changes nothing.
			if (bVisibilityDiffers)
			{
				AddLog(FString::Printf(TEXT("Library: changing '%s' from %s to %s as you chose"), *Job.Name,
					bLibraryPublic ? TEXT("private") : TEXT("public"), bLibraryPublic ? TEXT("public") : TEXT("private")));
			}
			else
			{
				AddLog(FString::Printf(TEXT("Library: setting the card picture of '%s'"), *Job.Name));
			}
			Job.Step = EStep::Patching;
			Attempts = 0;
			Tick();
			return;
		}
		ContinueLibraryJobAfterLooking();
		return;
	}
	if (Job.Step == EStep::Patching)
	{
		ContinueLibraryJobAfterLooking();
		return;
	}
	FString ItemId;
	int32 Version = 0;
	if (!FMantleLibraryPublish::ReadCreated(Response.Body, ItemId, Version))
	{
		Fail(FString::Printf(TEXT("Mantle added '%s' to your library but did not say which item. Choose Publish again."), *Job.Name));
		return;
	}
	FinishLibraryJob(ItemId, Version, Job.Step == EStep::Creating ? TEXT("created") : TEXT("added a version to"));
}

void FMantlePlacePublishSession::ContinueLibraryJobAfterLooking()
{
	FLibraryJob& Job = LibraryJobs[LibraryIndex];
	if (Job.ExistingVersion >= 1)
	{
		// The same upload is already a version of this item (a republish with nothing changed).
		FinishLibraryJob(Job.ItemId, Job.ExistingVersion, TEXT("already holds"));
		return;
	}
	Job.Step = FLibraryJob::EStep::Versioning;
	Attempts = 0;
	AddLog(FString::Printf(TEXT("Adding a new version of '%s' to its library item"), *Job.Name));
	Tick();
}

void FMantlePlacePublishSession::RestartLibraryJobAsNew(const TCHAR* Why)
{
	FLibraryJob& Job = LibraryJobs[LibraryIndex];
	AddLog(FString::Printf(TEXT("The library item for '%s' %s; making a new one"), *Job.Name, Why));
	for (const FString& SourcePath : Job.Paths)
	{
		Source->ForgetLibraryItem(SourcePath);
	}
	Job.ItemId.Reset();
	Job.bForgotten = true;
	Job.ExistingVersion = 0;
	Job.Step = FLibraryJob::EStep::Creating;
	Attempts = 0;
	Tick();
}

void FMantlePlacePublishSession::FinishLibraryJob(const FString& ItemId, int32 Version, const TCHAR* What)
{
	const FLibraryJob& Job = LibraryJobs[LibraryIndex];
	for (const FString& SourcePath : Job.Paths)
	{
		Source->RememberLibraryItem(SourcePath, ItemId);
	}
	if (Job.bPackage && Packages.IsValidIndex(Job.PackageIndex))
	{
		// #838: stored under the folder's name, which its Script Points and require("@Name/...") already use.
		FMantleLibraryPackageRef& PackageRef = PackageRefs.AddDefaulted_GetRef();
		PackageRef.Name = Packages[Job.PackageIndex].Name;
		PackageRef.ItemId = ItemId;
		PackageRef.Version = Version;
		PackageRef.Manifest = FMantleScriptPackages::ManifestToJson(Packages[Job.PackageIndex].Manifest);
	}
	else
	{
		FMantleLibraryVehicleRef& Ref = LibraryRefs.AddDefaulted_GetRef();
		Ref.AssetId = Job.AssetId;
		Ref.Name = Job.Name;
		Ref.ItemId = ItemId;
		Ref.Version = Version;
	}
	AddLog(FString::Printf(TEXT("Library: %s %s %s version %d"), *Job.Name, What, *ItemId, Version));
	++LibraryIndex;
	Attempts = 0;
	Tick();
}

void FMantlePlacePublishSession::FinishLibrary()
{
	if (Source->IsLibraryOnly())
	{
		// #840: nothing else to send. One line per item for harnesses and the log.
		for (const FMantleLibraryVehicleRef& Ref : LibraryRefs)
		{
			const FLibraryJob* Job = LibraryJobs.FindByPredicate([&Ref](const FLibraryJob& Candidate) { return Candidate.AssetId.Equals(Ref.AssetId, ESearchCase::CaseSensitive); });
			UE_LOG(LogMantle, Log, TEXT("MANTLE_LIBRARY_PUBLISHED kind=%s name=%s item=%s version=%d asset=%s visibility=%s"), Job && Job->bWeapon ? TEXT("weapon") : TEXT("vehicle"),
				*Ref.Name, *Ref.ItemId, Ref.Version, *Ref.AssetId, bLibraryPublic ? TEXT("public") : TEXT("private"));
		}
		if (LibraryRefs.Num() == 0)
		{
			Fail(FString::Printf(TEXT("'%s' was uploaded, but nothing was added to your library. Publish again; nothing is uploaded twice."), *GetLevelName()));
			return;
		}
		Finish(LibraryRefs.Num() == 1
			? FString::Printf(TEXT("'%s' is in your library (%s, version %d). Add it to a place from Place Settings > Storage."), *LibraryRefs[0].Name,
				bLibraryPublic ? TEXT("public") : TEXT("private"), LibraryRefs[0].Version)
			: FString::Printf(TEXT("%d items are in your library (%s). Add them to a place from Place Settings > Storage."), LibraryRefs.Num(),
				bLibraryPublic ? TEXT("public") : TEXT("private")));
		return;
	}
	FString Rewritten;
	FString Error;
	if (!FMantleLibraryPublish::RewritePlace(Result.PlaceJson, LibraryRefs, Rewritten, Error, &StoredVehicles))
	{
		Fail(Error);
		return;
	}
	Result.PlaceJson = MoveTemp(Rewritten);
	// #789: the weapons through the same rewrite; the refs of the vehicles above are not named by any WeaponSpawn, so they are ignored.
	if (!FMantleLibraryPublish::RewriteWeaponSpawns(Result.PlaceJson, LibraryRefs, Rewritten, Error, &StoredWeapons))
	{
		Fail(Error);
		return;
	}
	Result.PlaceJson = MoveTemp(Rewritten);
	for (TArray<FMantleStoredVehicle>* List : { &StoredVehicles, &StoredWeapons })
	{
		for (FMantleStoredVehicle& Stored : *List)
		{
			const FLibraryJob* Job = LibraryJobs.FindByPredicate([&Stored](const FLibraryJob& Candidate) { return Candidate.AssetId.Equals(Stored.AssetId, ESearchCase::CaseSensitive); });
			Stored.Paths = Job ? Job->Paths : TArray<FString>();
		}
	}
	if (LibraryRefs.Num() > 0)
	{
		AddLog(FString::Printf(TEXT("%d vehicle(s) and weapon(s) are in this place's storage; its spawns use them from there"), LibraryRefs.Num()));
	}
	// #838: the script packages, each under its folder's name.
	if (!FMantleLibraryPublish::RewriteScriptPackages(Result.PlaceJson, PackageRefs, Rewritten, Error))
	{
		Fail(Error);
		return;
	}
	Result.PlaceJson = MoveTemp(Rewritten);
	if (PackageRefs.Num() > 0)
	{
		AddLog(FString::Printf(TEXT("%d script package(s) are in this place's storage; they run in the place and its Script Points use them"), PackageRefs.Num()));
	}
	EnterSaving();
}

void FMantlePlacePublishSession::SendPlaceCall(const FMantleUploadCallRequest& Request)
{
	bRequestInFlight = true;
	const uint32 ForGeneration = Generation;
	const TWeakPtr<FMantlePlacePublishSession> WeakThis = AsShared();
	Transport->Send(Request, FMantleApiCallback::CreateLambda([WeakThis, ForGeneration](const FMantleApiResult& Response)
	{
		if (const TSharedPtr<FMantlePlacePublishSession> This = WeakThis.Pin())
		{
			This->OnPlaceResponse(ForGeneration, Response);
		}
	}));
}

void FMantlePlacePublishSession::OnPlaceResponse(uint32 ForGeneration, const FMantleApiResult& Response)
{
	if (ForGeneration != Generation || !bRequestInFlight || IsFinished())
	{
		return;
	}
	bRequestInFlight = false;
	switch (Phase)
	{
	case EMantlePublishPhase::Crediting: HandleCredited(Response); break;
	case EMantlePublishPhase::Scripts: HandleScriptUploaded(Response); break;
	case EMantlePublishPhase::Library: HandleLibraryResponse(Response); break;
	case EMantlePublishPhase::CreatingPlace: HandleCreated(Response); break;
	case EMantlePublishPhase::Saving: HandleSaved(Response); break;
	case EMantlePublishPhase::Publishing: HandlePublished(Response); break;
	default: break;
	}
}

void FMantlePlacePublishSession::HandleScriptUploaded(const FMantleApiResult& Response)
{
	if (!Scripts.IsValidIndex(ScriptIndex))
	{
		return;
	}
	const FMantlePlaceScriptUpload& Script = Scripts[ScriptIndex];
	const TCHAR* What = Script.bUIDocument ? TEXT("UI file") : Script.bGraph ? TEXT("graph") : TEXT("script");
	if (!Response.bSuccess)
	{
		if (FMantleCosmeticUploadSession::IsRetryable(Response) && Response.StatusCode != 401 && Timing.RetryDelays.IsValidIndex(Attempts))
		{
			NextActionAt = Clock() + FMath::Max(Timing.RetryDelays[Attempts++], Response.RetryAfterSeconds);
			return;
		}
		Fail(FString::Printf(TEXT("Could not upload %s '%s': %s"), What, *Script.Path, *FMantlePublisher::DescribeApiFailure(Response)));
		return;
	}
	const FString Id = MantlePlacePublishSessionPrivate::PublishStringField(Response.Body, TEXT("id"));
	if (!Id.StartsWith(TEXT("src_")))
	{
		Fail(FString::Printf(TEXT("Mantle stored %s '%s' but returned an invalid source id."), What, *Script.Path));
		return;
	}
	if (Script.PackageIndex != INDEX_NONE)
	{
		// #838: a package's file: its id goes into the package's library item, never into the place file.
		if (Packages.IsValidIndex(Script.PackageIndex) && Packages[Script.PackageIndex].SourceIds.IsValidIndex(Script.PackageFileIndex))
		{
			Packages[Script.PackageIndex].SourceIds[Script.PackageFileIndex] = Id;
		}
	}
	else if (Script.bUIDocument)
	{
		UIDocumentSourceIds.Add(Script.ScriptId, Id);
	}
	else
	{
		(Script.bAuthorSource ? ScriptAuthorSourceIds : ScriptSourceIds).Add(Script.ScriptId, Id);
	}
	++ScriptIndex;
	Attempts = 0;
	Tick();
}

void FMantlePlacePublishSession::HandleCreated(const FMantleApiResult& Response)
{
	if (!Response.bSuccess)
	{
		// Not retried: a request that reached the server before the line dropped may have made
		// the place, and a second one would be a duplicate in the creator's list.
		Fail(FMantlePublisher::DescribeApiFailure(Response));
		return;
	}
	Result.PlaceId = MantlePlacePublishSessionPrivate::PublishStringField(Response.Body, TEXT("id"));
	if (Result.PlaceId.IsEmpty())
	{
		Fail(TEXT("Mantle created the place but did not say which one."));
		return;
	}
	Source->RememberPlaceId(Result.PlaceId);
	AddLog(FString::Printf(TEXT("Created place %s"), *Result.PlaceId));
	Phase = EMantlePublishPhase::Saving;
	Attempts = 0;
	Tick();
}

void FMantlePlacePublishSession::ForgetGonePlace()
{
	// The remembered place answered 404 for this account at this API. Left in place, the link
	// would fail every save from now on; unlinked (for this account and API only), the next
	// save creates a new place. Not in this same click, so the creator learns why rather than
	// being handed a new place without a word.
	Source->ForgetPlaceId();
	const FString Gone = Result.PlaceId;
	Result.PlaceId.Reset();
	// This account made or saved to it here, so the only way it can be missing is deletion.
	Fail(FString::Printf(
		TEXT("The Mantle place this level used to publish to (%s) no longer exists, so the level ")
		TEXT("has been unlinked from it. Save or publish again to create a new place; links to ")
		TEXT("the old one will not work."),
		*Gone));
}

void FMantlePlacePublishSession::HandleSaved(const FMantleApiResult& Response)
{
	if (!Response.bSuccess)
	{
		if (FMantlePublisher::IsPlaceGone(Response))
		{
			ForgetGonePlace();
			return;
		}
		if (FMantleCosmeticUploadSession::IsRetryable(Response) && Timing.RetryDelays.IsValidIndex(Attempts))
		{
			// Saving the draft is a PUT of the whole file: sending it again is harmless.
			NextActionAt = Clock() + FMath::Max(Timing.RetryDelays[Attempts++], Response.RetryAfterSeconds);
			return;
		}
		Fail(MantlePlacePublishSessionPrivate::IsGateCode(Response.Code) ? DescribePlaceRefusal(Response) : FMantlePublisher::DescribeApiFailure(Response));
		return;
	}
	bDraftSaved = true;
	Attempts = 0;
	if (!bGoLive)
	{
		Result.RevisionNumber = MantlePlacePublishSessionPrivate::PublishNumberField(Response.Body, TEXT("number"));
		Finish(TEXT("Saved to Mantle. Nobody can see it until you publish."));
		return;
	}
	Phase = EMantlePublishPhase::Publishing;
	Tick();
}

void FMantlePlacePublishSession::HandlePublished(const FMantleApiResult& Response)
{
	bPublishSent = false;
	if (!Response.bSuccess)
	{
		// A script uploaded a moment ago starts pending moderation, so the first publish that
		// carries one is usually refused as scripts_pending; a material's textures can race their
		// approval the same way. Wait a little and try again rather than hand the creator a
		// moderation state to retry by hand.
		const bool bScripts = FMantlePublisher::IsScriptsPending(Response);
		const bool bAssets = FMantlePublisher::IsAssetsPending(Response);
		const TArray<float>& Delays = bScripts ? Timing.ScriptsPendingDelays : Timing.AssetsPendingDelays;
		if ((bScripts || bAssets) && Delays.IsValidIndex(Attempts))
		{
			AddLog(bScripts ? TEXT("Mantle is still screening new scripts; trying again shortly") : TEXT("Mantle is still processing some files; trying again shortly"));
			NextActionAt = Clock() + Delays[Attempts++];
			return;
		}
		if (bScripts)
		{
			Fail(TEXT("Your changes are saved, but Mantle is still screening this place's new scripts, so it has not gone live yet. Choose Publish again in a minute."));
			return;
		}
		if (FMantlePublisher::IsPlaceGone(Response))
		{
			ForgetGonePlace();
			return;
		}
		Fail(MantlePlacePublishSessionPrivate::IsGateCode(Response.Code) ? DescribePlaceRefusal(Response) : FMantlePublisher::DescribeApiFailure(Response));
		return;
	}
	Result.bWentLive = true;
	// The place is live: the level adopts the storage the published file carries (its spawns name stored items now).
	Source->ApplyPublishedVehicles(StoredVehicles);
	Source->ApplyPublishedWeapons(StoredWeapons);
	Source->ApplyPublishedPackages(PackageRefs);
	Result.RevisionNumber = MantlePlacePublishSessionPrivate::PublishNumberField(Response.Body, TEXT("number"));
	Finish(FString::Printf(TEXT("Published revision %d. Anyone can join it now."), Result.RevisionNumber));
}

FString FMantlePlacePublishSession::NameForId(const FString& Id) const
{
	if (const FString* Name = NameById.Find(Id))
	{
		return *Name;
	}
	return FString();
}

FString FMantlePlacePublishSession::DescribePlaceRefusal(const FMantleApiResult& Refused)
{
	using namespace MantlePlacePublishSessionPrivate;
	const TSharedPtr<FJsonObject> Body = ParsePublishObject(Refused.Body);
	const FString& Code = Refused.Code;

	// The ids the gate names, as Unreal assets; ids this session never uploaded (an asset from an
	// earlier revision, say) are counted, never shown.
	auto Rows = [this](const TArray<FString>& Ids, EMantleIssueSeverity Severity, TFunctionRef<FString(const FString&)> Problem,
		const TCHAR* Remedy, FString& OutFirst)
	{
		int32 Unnamed = 0;
		for (const FString& Id : Ids)
		{
			const FString Name = NameForId(Id);
			if (Name.IsEmpty())
			{
				++Unnamed;
				continue;
			}
			OutFirst = OutFirst.IsEmpty() ? Name : OutFirst;
			AddAttention(Severity, Problem(FString::Printf(TEXT("'%s'"), *Name)), Remedy);
		}
		if (Unnamed > 0)
		{
			AddAttention(Severity, Problem(FString::Printf(TEXT("%d file(s) from an earlier publish"), Unnamed)), Remedy);
		}
		return Ids.Num();
	};

	if (Code == TEXT("compatibility_failed"))
	{
		// The release gate (#572): the API's message stays the headline, followed by the first
		// failed check, and every failed check gets its own row in the list.
		const TArray<FMantleCompatibilityFailure> Failures = FMantleApiClient::ParseCompatibilityFailures(Refused.Body);
		for (const FMantleCompatibilityFailure& Failure : Failures)
		{
			AddAttention(EMantleIssueSeverity::Error, Failure.Problem, Failure.Remedy);
		}
		if (Failures.Num() == 0)
		{
			return Refused.Message;
		}
		return Failures.Num() == 1
			? FString::Printf(TEXT("%s %s"), *Refused.Message, *Failures[0].Problem)
			: FString::Printf(TEXT("%s %s (and %d more; the list below names them)"), *Refused.Message, *Failures[0].Problem, Failures.Num() - 1);
	}

	FString First;
	if (Code == TEXT("assets_rejected"))
	{
		const int32 Count = Rows(PublishStringArray(Body, TEXT("rejectedAssets")), EMantleIssueSeverity::Error,
			[](const FString& What) { return FString::Printf(TEXT("%s did not pass Mantle's review, so this place cannot go live with it."), *What); },
			TEXT("Replace it or remove what uses it, then Publish again."), First);
		return FString::Printf(TEXT("Mantle's review rejected %d file(s) this place uses, so nothing went live. The list below names them."), Count);
	}
	if (Code == TEXT("assets_not_owned"))
	{
		const int32 Count = Rows(PublishStringArray(Body, TEXT("notOwnedAssets")), EMantleIssueSeverity::Error,
			[](const FString& What) { return FString::Printf(TEXT("%s is not held by your account on Mantle (a moderator removed it after a complaint, or only someone else ever uploaded it)."), *What); },
			TEXT("Publish again uploads it from your project; if a moderator removed it, replace it."), First);
		return FString::Printf(TEXT("%d file(s) this place uses are not yours on Mantle, so nothing went live. The list below names them."), Count);
	}
	if (Code == TEXT("assets_missing"))
	{
		const int32 Count = Rows(PublishStringArray(Body, TEXT("missingAssets")), EMantleIssueSeverity::Error,
			[](const FString& What) { return FString::Printf(TEXT("Mantle does not have %s."), *What); },
			TEXT("Choose Publish again: it uploads it."), First);
		return FString::Printf(TEXT("Mantle is missing %d file(s) this place uses, so nothing went live. Choose Publish again: it uploads them."), Count);
	}
	if (Code == TEXT("assets_pending"))
	{
		const int32 Count = Rows(PublishStringArray(Body, TEXT("pendingAssets")), EMantleIssueSeverity::Warning,
			[](const FString& What) { return FString::Printf(TEXT("Mantle is still processing %s."), *What); },
			TEXT("Nothing to do but wait a minute."), First);
		return FString::Printf(TEXT("Your changes are saved, but Mantle is still processing %d file(s)%s, so it has not gone live yet. ")
			TEXT("Choose Publish again in a minute; nothing is uploaded twice."),
			Count, First.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" such as '%s'"), *First));
	}
	if (Code == TEXT("wrong_asset_kind") || Code == TEXT("material_slots"))
	{
		const TArray<TSharedPtr<FJsonValue>>* Issues = nullptr;
		if (Body.IsValid() && Body->TryGetArrayField(TEXT("issues"), Issues) && Issues)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Issues)
			{
				const TSharedPtr<FJsonObject> Issue = Value.IsValid() ? Value->AsObject() : nullptr;
				if (!Issue.IsValid())
				{
					continue;
				}
				FString Path, Message;
				Issue->TryGetStringField(TEXT("path"), Path);
				Issue->TryGetStringField(TEXT("message"), Message);
				FString Name;
				if (const TSharedPtr<FJsonValue> At = ResolvePointer(Result.PlaceJson, Path))
				{
					FString Id;
					if (At->TryGetString(Id))
					{
						Name = NameForId(Id);
					}
					else if (At->Type == EJson::Object && At->AsObject()->TryGetStringField(TEXT("mesh"), Id))
					{
						Name = NameForId(Id);
					}
				}
				AddAttention(EMantleIssueSeverity::Error,
					Name.IsEmpty() ? FString::Printf(TEXT("Mantle refused the place file at %s: %s."), *Path, *Message)
						: FString::Printf(TEXT("Mantle refused how the place file uses '%s': %s."), *Name, *Message),
					TEXT("This is a problem in the plugin, not in your content: please report it with the editor log."));
			}
		}
		return FString::Printf(TEXT("Mantle refused the place file (%s). This is a problem in the plugin, not in your content: please report it with the editor log."),
			*Refused.Message.TrimChar(TEXT('.')));
	}
	return FMantlePublisher::DescribeApiFailure(Refused);
}

// ---------------------------------------------------------------------------------------------
// For the panel

float FMantlePlacePublishSession::GetProgress() const
{
	float Value = ShownProgress;
	switch (Phase)
	{
	case EMantlePublishPhase::Idle: Value = 0.0f; break;
	case EMantlePublishPhase::Checking: Value = 0.02f; break;
	case EMantlePublishPhase::Converting:
		Value = 0.02f + 0.18f * (ConversionTotal > 0 ? static_cast<float>(ConversionDone) / ConversionTotal : 1.0f);
		break;
	case EMantlePublishPhase::Uploading:
		if (Queue.IsValid())
		{
			const FMantleUploadQueueProgress& Progress = Queue->GetProgress();
			const float Assets = Progress.AssetsTotal > 0 ? static_cast<float>(Progress.AssetsSent) / Progress.AssetsTotal : 1.0f;
			const float Bytes = Progress.BytesTotal > 0 ? static_cast<float>(static_cast<double>(Progress.BytesDone) / Progress.BytesTotal) : Assets;
			const float Ready = Progress.AssetsTotal > 0 ? static_cast<float>(Progress.AssetsFinished) / Progress.AssetsTotal : 1.0f;
			const float Sent = 0.5f * (Assets + Bytes);
			Value = 0.20f + 0.70f * (bGoLive ? 0.6f * Sent + 0.4f * Ready : Sent);
		}
		break;
	case EMantlePublishPhase::Crediting: Value = 0.90f; break;
	case EMantlePublishPhase::Scripts:
		Value = 0.90f + 0.03f * (Scripts.Num() > 0 ? static_cast<float>(ScriptIndex) / Scripts.Num() : 0.0f);
		break;
	case EMantlePublishPhase::Exporting: Value = 0.94f; break;
	case EMantlePublishPhase::Library: Value = 0.945f; break;
	case EMantlePublishPhase::CreatingPlace: Value = 0.95f; break;
	case EMantlePublishPhase::Saving: Value = 0.96f; break;
	case EMantlePublishPhase::Publishing: Value = 0.98f; break;
	case EMantlePublishPhase::Done: Value = 1.0f; break;
	default: break;
	}
	ShownProgress = FMath::Max(ShownProgress, FMath::Clamp(Value, 0.0f, 1.0f));
	return ShownProgress;
}

FString FMantlePlacePublishSession::GetStatusText() const
{
	switch (Phase)
	{
	case EMantlePublishPhase::Idle: return TEXT("Ready");
	case EMantlePublishPhase::Checking:
		return Source->IsLibraryOnly() ? FString::Printf(TEXT("Checking '%s'..."), *GetLevelName()) : FString(TEXT("Checking this place..."));
	case EMantlePublishPhase::Converting:
		return FString::Printf(TEXT("Preparing meshes and materials for Mantle: %d of %d%s..."), ConversionDone, ConversionTotal,
			ConversionName.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" ('%s')"), *ConversionName));
	case EMantlePublishPhase::Uploading: return Queue.IsValid() ? Queue->GetStatusText() : FString(TEXT("Uploading..."));
	case EMantlePublishPhase::Crediting: return TEXT("Saving the credits for your assets...");
	case EMantlePublishPhase::Scripts:
		return Scripts.Num() > 0 ? FString::Printf(TEXT("Uploading scripts: %d of %d..."), ScriptIndex, Scripts.Num()) : FString(TEXT("Collecting scripts..."));
	case EMantlePublishPhase::Exporting: return TEXT("Writing the place file...");
	case EMantlePublishPhase::Library:
		if (Source->IsLibraryOnly())
		{
			return IsWaitingForVehicleChoice()
				? FString::Printf(TEXT("'%s' is uploaded. Choose who can use it (a public item needs the rights declaration) and publishing continues."), *GetLevelName())
				: FString::Printf(TEXT("Adding '%s' to your library..."), *GetLevelName());
		}
		if (IsWaitingForVehicleChoice())
		{
			return TEXT("Your vehicles, weapons and script packages are uploaded. Choose who can use them below (a public item needs the rights declaration) and publishing continues.");
		}
		return FString::Printf(TEXT("Adding your vehicles, weapons and script packages to your library: %d of %d..."), FMath::Min(LibraryIndex + 1, LibraryJobs.Num()), LibraryJobs.Num());
	case EMantlePublishPhase::CreatingPlace: return TEXT("Creating the place on Mantle...");
	case EMantlePublishPhase::Saving: return TEXT("Saving your changes to Mantle...");
	case EMantlePublishPhase::Publishing:
		return Attempts > 0 ? FString(TEXT("Waiting for Mantle to finish screening, then going live...")) : FString(TEXT("Going live..."));
	default: return Result.Message;
	}
}

EMantlePublishStepState FMantlePlacePublishSession::GetStepState(EMantlePublishStep Step) const
{
	auto StepOf = [this](EMantlePublishPhase InPhase)
	{
		switch (InPhase)
		{
		case EMantlePublishPhase::Idle:
		case EMantlePublishPhase::Checking: return EMantlePublishStep::Check;
		case EMantlePublishPhase::Converting: return EMantlePublishStep::Prepare;
		case EMantlePublishPhase::Uploading:
			return Queue.IsValid() && Queue->GetProgress().AssetsSent >= Queue->GetProgress().AssetsTotal ? EMantlePublishStep::Process : EMantlePublishStep::Upload;
		default: return EMantlePublishStep::GoLive;
		}
	};
	const bool bSkipped = (Step == EMantlePublishStep::Prepare && ConversionTotal == 0 && Phase > EMantlePublishPhase::Converting)
		|| ((Step == EMantlePublishStep::Upload || Step == EMantlePublishStep::Process) && !bHadUploads && Phase > EMantlePublishPhase::Uploading)
		|| (Step == EMantlePublishStep::Process && !bGoLive && Phase > EMantlePublishPhase::Uploading);
	const bool bNotes = (Step == EMantlePublishStep::Check && Result.Validation.Issues.Num() > 0)
		|| ((Step == EMantlePublishStep::Upload || Step == EMantlePublishStep::Process) && Result.Attention.Num() > 0);

	if (Phase == EMantlePublishPhase::Done)
	{
		return bSkipped ? EMantlePublishStepState::Skipped : bNotes ? EMantlePublishStepState::DoneWithNotes : EMantlePublishStepState::Done;
	}
	const bool bStopped = Phase == EMantlePublishPhase::Failed || Phase == EMantlePublishPhase::Cancelled;
	const EMantlePublishStep Current = StepOf(bStopped ? FailedPhase : Phase);
	if (Step < Current)
	{
		return bSkipped ? EMantlePublishStepState::Skipped : bNotes ? EMantlePublishStepState::DoneWithNotes : EMantlePublishStepState::Done;
	}
	if (Step == Current)
	{
		return bStopped ? EMantlePublishStepState::Failed : EMantlePublishStepState::Running;
	}
	return EMantlePublishStepState::NotStarted;
}

FString FMantlePlacePublishSession::GetStepDetail(EMantlePublishStep Step) const
{
	auto Size = [](int64 Bytes)
	{
		return Bytes >= 1024 * 1024 ? FString::Printf(TEXT("%.1f MB"), Bytes / (1024.0 * 1024.0)) : FString::Printf(TEXT("%.0f KB"), Bytes / 1024.0);
	};
	switch (Step)
	{
	case EMantlePublishStep::Check:
		{
			const int32 Errors = Result.Validation.NumErrors();
			const int32 Warnings = Result.Validation.Issues.Num() - Errors;
			if (Phase <= EMantlePublishPhase::Checking)
			{
				return FString();
			}
			if (Errors > 0)
			{
				return FString::Printf(TEXT("%d problem(s) to fix"), Errors);
			}
			return Warnings > 0 ? FString::Printf(TEXT("%d note(s), nothing blocking"), Warnings) : FString(TEXT("No problems"));
		}
	case EMantlePublishStep::Prepare:
		if (Phase < EMantlePublishPhase::Converting)
		{
			return FString();
		}
		if (ConversionTotal - Source->NumNonContentSteps() == 0)
		{
			return TEXT("Nothing to prepare: only Mantle library assets");
		}
		return FString::Printf(TEXT("%d of %d meshes and materials, %d texture(s)"), ConversionDone, ConversionTotal, Set.Textures.Num());
	case EMantlePublishStep::Upload:
		if (!Queue.IsValid())
		{
			return Phase > EMantlePublishPhase::Uploading && Phase != EMantlePublishPhase::Failed && Phase != EMantlePublishPhase::Cancelled
				? FString(TEXT("Nothing to upload")) : FString();
		}
		{
			const FMantleUploadQueueProgress& Progress = Queue->GetProgress();
			int32 AlreadyThere = 0;
			for (const FMantleQueuedUploadStatus& Status : Queue->GetStatuses())
			{
				AlreadyThere += Status.bAlreadyOnMantle ? 1 : 0;
			}
			FString Detail = FString::Printf(TEXT("%d of %d files on Mantle, %s of %s"), Progress.AssetsSent, Progress.AssetsTotal,
				*Size(Progress.BytesDone), *Size(Progress.BytesTotal));
			if (AlreadyThere > 0)
			{
				Detail += FString::Printf(TEXT(" (%d already there)"), AlreadyThere);
			}
			return Detail;
		}
	case EMantlePublishStep::Process:
		if (!bGoLive)
		{
			return TEXT("Mantle finishes these in the background; they are checked again when you publish");
		}
		if (!Queue.IsValid())
		{
			return FString();
		}
		{
			const FMantleUploadQueueProgress& Progress = Queue->GetProgress();
			int32 InReview = 0;
			for (const FMantleQueuedUploadStatus& Status : Queue->GetStatuses())
			{
				InReview += Status.bInReview && !Status.IsFinal() ? 1 : 0;
			}
			FString Detail = FString::Printf(TEXT("%d of %d ready"), Progress.AssetsFinished, Progress.AssetsTotal);
			if (InReview > 0)
			{
				Detail += FString::Printf(TEXT(", %d waiting for a moderator"), InReview);
			}
			return Detail;
		}
	case EMantlePublishStep::GoLive:
		if (Phase == EMantlePublishPhase::Done)
		{
			return bGoLive ? FString::Printf(TEXT("Revision %d is live"), Result.RevisionNumber) : FString(TEXT("Draft saved"));
		}
		return Phase >= EMantlePublishPhase::Crediting && Phase <= EMantlePublishPhase::Publishing ? GetStatusText() : FString();
	default:
		return FString();
	}
}
