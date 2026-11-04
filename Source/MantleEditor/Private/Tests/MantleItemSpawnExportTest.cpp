// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAssetLibrary.h"
#include "MantleItemSpawnModel.h"
#include "MantleItemSpawnPoint.h"
#include "MantlePlaceAssets.h"
#include "MantlePlaceExporter.h"
#include "MantlePlaceParser.h"
#include "MantlePlaceSettingsActor.h"
#include "MantlePlaceTypes.h"
#include "MantlePlaceValidator.h"

#include "Dom/JsonObject.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

// The editor's half of the Item Spawn (#235): a spawn exports as an ItemSpawn that writes only what differs from the schema's defaults,
// the runtime parser reads back what the exporter wrote, a Static Mesh picked as its model uploads with the place under the mesh's
// asset id, Check refuses an item id, count, respawn time or model that cannot publish, and Place Settings' Save World Items writes
// settings.worldItems only when it is on. On a transient editor world.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleItemSpawnExportTestPrivate
{
	const TCHAR* const CubePath = TEXT("/Engine/BasicShapes/Cube.Cube");
	const TCHAR* const AssetModel = TEXT("ast_u33333333333333333333333333333333");

	/** An empty editor world for one test, destroyed with it. */
	struct FItemSpawnWorld
	{
		UWorld* World = nullptr;

		FItemSpawnWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld*/ false, TEXT("MantleItemSpawnExportTestWorld"));
		}

		~FItemSpawnWorld()
		{
			if (World)
			{
				World->DestroyWorld(/*bInformEngineOfWorld*/ false);
				if (World->IsRooted())
				{
					World->RemoveFromRoot();
				}
			}
		}
	};

	AMantleItemSpawnPoint* SpawnItemSpawn(UWorld* World, const TCHAR* Label, const FString& Item)
	{
		AMantleItemSpawnPoint* Spawn = World->SpawnActor<AMantleItemSpawnPoint>(FVector(10.0, 20.0, 30.0), FRotator(0.0, 90.0, 0.0));
		if (Spawn)
		{
			Spawn->SetActorLabel(Label);
			Spawn->Item = Item;
		}
		return Spawn;
	}

	const FMantleEntity* FindItemSpawn(const FMantleExportResult& Result)
	{
		return Result.Place.Entities.FindByPredicate([](const FMantleEntity& Candidate) { return Candidate.Type == EMantleEntityType::ItemSpawn; });
	}

	/** The "properties" of the first entity of a place file's JSON, or null. */
	TSharedPtr<FJsonObject> FirstEntityProperties(const FString& Json)
	{
		TSharedPtr<FJsonObject> Root;
		const TArray<TSharedPtr<FJsonValue>>* Entities = nullptr;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid()
			|| !Root->TryGetArrayField(TEXT("entities"), Entities) || Entities->Num() == 0)
		{
			return nullptr;
		}
		const TSharedPtr<FJsonObject>* Properties = nullptr;
		return (*Entities)[0]->AsObject()->TryGetObjectField(TEXT("properties"), Properties) ? *Properties : nullptr;
	}

	/** The "settings.worldItems" object of a place's JSON, or null when the place file has none. */
	TSharedPtr<FJsonObject> WorldItemsObjectOf(const FString& Json)
	{
		TSharedPtr<FJsonObject> Root;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
		{
			return nullptr;
		}
		const TSharedPtr<FJsonObject>* Settings = nullptr;
		const TSharedPtr<FJsonObject>* WorldItems = nullptr;
		if (!Root->TryGetObjectField(TEXT("settings"), Settings) || !(*Settings)->TryGetObjectField(TEXT("worldItems"), WorldItems))
		{
			return nullptr;
		}
		return *WorldItems;
	}

	/** True when Result has an error at a path ending in Suffix. */
	bool HasErrorAt(const FMantleExportResult& Result, const TCHAR* Suffix)
	{
		return Result.Errors.ContainsByPredicate([Suffix](const FMantleParseError& Error) { return Error.Path.EndsWith(Suffix); });
	}

	/** True when Check has an error whose problem contains Text. */
	bool HasIssue(const FMantleValidationResult& Validation, const TCHAR* Text)
	{
		return Validation.Issues.ContainsByPredicate([Text](const FMantleValidationIssue& Issue)
		{
			return Issue.Severity == EMantleIssueSeverity::Error && Issue.Problem.Contains(Text);
		});
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleItemSpawnExportDefaultsTest,
	"Mantle.ItemSpawn.Editor.Export.DefaultsWriteOnlyTheItem",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleItemSpawnExportDefaultsTest::RunTest(const FString& Parameters)
{
	using namespace MantleItemSpawnExportTestPrivate;
	FItemSpawnWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	AMantleItemSpawnPoint* Spawn = SpawnItemSpawn(Fixture.World, TEXT("Potion"), TEXT("health_potion"));
	if (!TestNotNull(TEXT("the spawn"), Spawn))
	{
		return false;
	}
	EMantleEntityType Type = EMantleEntityType::Group;
	TestTrue(TEXT("the exporter classifies it as an ItemSpawn"), FMantlePlaceExporter::ClassifyActor(Spawn, Type) && Type == EMantleEntityType::ItemSpawn);

	const FMantleExportResult Result = FMantlePlaceExporter::Export(Fixture.World, FMantleExportOptions());
	TestFalse(TEXT("the export does not refuse the spawn"), HasErrorAt(Result, TEXT("/properties/item")) || HasErrorAt(Result, TEXT("/properties/count")));
	const FMantleEntity* Entity = FindItemSpawn(Result);
	if (!TestNotNull(TEXT("an ItemSpawn entity"), Entity))
	{
		return false;
	}
	const FMantleItemSpawnProps* Props = Entity->Props.TryGet<FMantleItemSpawnProps>();
	if (!TestNotNull(TEXT("with item properties"), Props))
	{
		return false;
	}
	TestEqual(TEXT("the item"), Props->Item, FString(TEXT("health_potion")));
	TestEqual(TEXT("one of it"), Props->Count, 1);
	TestEqual(TEXT("the default respawn time"), Props->RespawnSeconds, static_cast<double>(MANTLE_DEFAULT_ITEM_SPAWN_RESPAWN_SECONDS));
	TestTrue(TEXT("no model"), Props->Model.IsEmpty());
	TestTrue(TEXT("on"), Props->bEnabled);

	// Only the item is written, and the parser reads it back with the defaults.
	const FString Json = FMantlePlaceExporter::ToJson(Result.Place);
	const TSharedPtr<FJsonObject> Written = FirstEntityProperties(Json);
	if (TestTrue(TEXT("the entity writes properties"), Written.IsValid()))
	{
		TestEqual(TEXT("item is written"), Written->GetStringField(TEXT("item")), FString(TEXT("health_potion")));
		TestFalse(TEXT("count is left out at 1"), Written->HasField(TEXT("count")));
		TestFalse(TEXT("respawnSeconds is left out at its default"), Written->HasField(TEXT("respawnSeconds")));
		TestFalse(TEXT("model is left out when empty"), Written->HasField(TEXT("model")));
		TestFalse(TEXT("enabled is left out when on"), Written->HasField(TEXT("enabled")));
	}
	const FMantleParseResult Parsed = FMantlePlaceParser::ParseFromString(Json);
	if (TestTrue(TEXT("what the exporter wrote parses"), Parsed.bSuccess && Parsed.Place.Entities.Num() > 0))
	{
		const FMantleItemSpawnProps* Read = Parsed.Place.Entities[0].Props.TryGet<FMantleItemSpawnProps>();
		if (TestNotNull(TEXT("an ItemSpawn is read back"), Read))
		{
			TestEqual(TEXT("the item"), Read->Item, FString(TEXT("health_potion")));
			TestEqual(TEXT("the count"), Read->Count, 1);
			TestEqual(TEXT("the respawn time"), Read->RespawnSeconds, static_cast<double>(MANTLE_DEFAULT_ITEM_SPAWN_RESPAWN_SECONDS));
		}
	}
	else
	{
		AddError(Parsed.ErrorSummary());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleItemSpawnExportEveryValueTest,
	"Mantle.ItemSpawn.Editor.Export.EveryValueIsWrittenAndReadBack",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleItemSpawnExportEveryValueTest::RunTest(const FString& Parameters)
{
	using namespace MantleItemSpawnExportTestPrivate;
	// A place file's own spawn: every key set, its model an asset id the place lists.
	FMantlePlace Place;
	Place.SchemaVersion = MANTLE_PLACE_SCHEMA_VERSION;
	Place.PlaceId = TEXT("plc_item_roundtrip");
	Place.Revision.Number = 1;
	Place.Revision.State = EMantleRevisionState::Draft;
	Place.Metadata.Name = TEXT("Item round trip");
	Place.Metadata.AuthorId = TEXT("usr_test");
	FMantleAssetRef Asset;
	Asset.Id = AssetModel;
	Asset.Kind = EMantleAssetKind::StaticMesh;
	Asset.Hash = TEXT("sha256:aa");
	Asset.Bytes = 10;
	Place.Assets.Add(Asset);
	FMantleItemSpawnProps Props;
	Props.Item = TEXT("itm_u0123456789abcdef0123456789abcdef");
	Props.Count = 25;
	Props.RespawnSeconds = 120.0;
	Props.Model = AssetModel;
	Props.bEnabled = false;
	FMantleEntity Entity;
	Entity.Id = TEXT("everything");
	Entity.Name = TEXT("everything");
	Entity.Type = EMantleEntityType::ItemSpawn;
	Entity.Props.Set<FMantleItemSpawnProps>(MoveTemp(Props));
	Place.Entities.Add(MoveTemp(Entity));

	const FString Json = FMantlePlaceExporter::ToJson(Place);
	const TSharedPtr<FJsonObject> Written = FirstEntityProperties(Json);
	if (!TestTrue(TEXT("the entity writes properties"), Written.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("item"), Written->GetStringField(TEXT("item")), FString(TEXT("itm_u0123456789abcdef0123456789abcdef")));
	TestEqual(TEXT("count"), Written->GetNumberField(TEXT("count")), 25.0);
	TestEqual(TEXT("respawnSeconds"), Written->GetNumberField(TEXT("respawnSeconds")), 120.0);
	TestEqual(TEXT("model"), Written->GetStringField(TEXT("model")), FString(AssetModel));
	TestFalse(TEXT("enabled"), Written->GetBoolField(TEXT("enabled")));

	const FMantleParseResult Parsed = FMantlePlaceParser::ParseFromString(Json);
	if (!TestTrue(TEXT("what the exporter wrote parses"), Parsed.bSuccess && Parsed.Place.Entities.Num() == 1))
	{
		AddError(Parsed.ErrorSummary());
		return false;
	}
	const FMantleItemSpawnProps* Read = Parsed.Place.Entities[0].Props.TryGet<FMantleItemSpawnProps>();
	if (TestNotNull(TEXT("an ItemSpawn is read back"), Read))
	{
		TestEqual(TEXT("the item"), Read->Item, FString(TEXT("itm_u0123456789abcdef0123456789abcdef")));
		TestEqual(TEXT("the count"), Read->Count, 25);
		TestEqual(TEXT("the respawn time"), Read->RespawnSeconds, 120.0);
		TestEqual(TEXT("the model"), Read->Model, FString(AssetModel));
		TestFalse(TEXT("the Enabled box"), Read->bEnabled);
	}

	// Loading it into an actor and out again keeps every value, and an asset id is kept as the model.
	FItemSpawnWorld Fixture;
	if (TestNotNull(TEXT("a world"), Fixture.World))
	{
		AMantleItemSpawnPoint* Spawn = SpawnItemSpawn(Fixture.World, TEXT("Loaded"), FString());
		if (TestNotNull(TEXT("the spawn"), Spawn) && Read)
		{
			Spawn->ApplyProps(*Read);
			const FMantleExportResult Result = FMantlePlaceExporter::Export(Fixture.World, FMantleExportOptions());
			const FMantleEntity* Exported = FindItemSpawn(Result);
			const FMantleItemSpawnProps* Again = Exported ? Exported->Props.TryGet<FMantleItemSpawnProps>() : nullptr;
			if (TestNotNull(TEXT("exported again"), Again))
			{
				TestEqual(TEXT("the item"), Again->Item, Read->Item);
				TestEqual(TEXT("the count"), Again->Count, Read->Count);
				TestEqual(TEXT("the respawn time"), Again->RespawnSeconds, Read->RespawnSeconds);
				TestEqual(TEXT("the model"), Again->Model, Read->Model);
				TestFalse(TEXT("the Enabled box"), Again->bEnabled);
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleItemSpawnExportModelTest,
	"Mantle.ItemSpawn.Editor.Export.APickedMeshNamesItsUpload",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleItemSpawnExportModelTest::RunTest(const FString& Parameters)
{
	using namespace MantleItemSpawnExportTestPrivate;
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, CubePath);
	if (!TestNotNull(TEXT("the engine's cube"), Cube))
	{
		return false;
	}
	TestEqual(TEXT("the Details panel stores a mesh as its object path"), FMantleItemSpawnModel::PathOf(Cube), FString(CubePath));
	TestTrue(TEXT("a stored path is a mesh path"), FMantleItemSpawnModel::IsMeshPath(FMantleItemSpawnModel::PathOf(Cube)));
	TestTrue(TEXT("and loads the mesh back"), FMantleItemSpawnModel::LoadMesh(FString(CubePath)) == Cube);
	TestFalse(TEXT("an asset id is not a mesh path"), FMantleItemSpawnModel::IsMeshPath(AssetModel));

	FItemSpawnWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	AMantleItemSpawnPoint* Spawn = SpawnItemSpawn(Fixture.World, TEXT("Crate"), TEXT("crate"));
	if (!TestNotNull(TEXT("the spawn"), Spawn))
	{
		return false;
	}
	Spawn->ModelId = FMantleItemSpawnModel::PathOf(Cube);

	const FMantleExportResult Result = FMantlePlaceExporter::Export(Fixture.World, FMantleExportOptions());
	const FMantleEntity* Entity = FindItemSpawn(Result);
	const FMantleItemSpawnProps* Props = Entity ? Entity->Props.TryGet<FMantleItemSpawnProps>() : nullptr;
	if (!TestNotNull(TEXT("an ItemSpawn with item properties"), Props))
	{
		return false;
	}
	// The cube is either one of Mantle's library meshes (named by its library id) or uploaded with the place (named by the upload's id).
	const bool bLibrary = FMantlePlaceAssets::IsLibraryAsset(Cube);
	const FString Expected = bLibrary
		? FMantleAssetLibrary::Get().FindIdByPath(FSoftObjectPath(Cube))
		: FMantlePlaceAssets::ProvisionalId(FMantlePlaceAssets::MeshKey(Cube));
	TestEqual(TEXT("the model is the mesh's asset id, not its path"), Props->Model, Expected);
	if (!bLibrary)
	{
		TestTrue(TEXT("the mesh is listed in assets[] as a StaticMesh"), Result.Place.Assets.ContainsByPredicate(
			[&Expected](const FMantleAssetRef& Ref) { return Ref.Id == Expected && Ref.Kind == EMantleAssetKind::StaticMesh; }));
	}
	const TSharedPtr<FJsonObject> Written = FirstEntityProperties(FMantlePlaceExporter::ToJson(Result.Place));
	if (TestTrue(TEXT("the entity writes properties"), Written.IsValid()))
	{
		TestEqual(TEXT("model is written"), Written->GetStringField(TEXT("model")), Expected);
	}

	// A path that is no mesh is refused in plain words, naming the spawn.
	Spawn->ModelId = TEXT("/Game/Nowhere/SM_Missing.SM_Missing");
	const FMantleExportResult Refused = FMantlePlaceExporter::Export(Fixture.World, FMantleExportOptions());
	const FMantleParseError* Refusal = Refused.Errors.FindByPredicate([](const FMantleParseError& Error) { return Error.Path.EndsWith(TEXT("/properties/model")); });
	if (TestNotNull(TEXT("a model that is no mesh is refused"), Refusal))
	{
		TestTrue(TEXT("naming the spawn"), Refusal->Message.Contains(TEXT("Crate")));
	}
	TestFalse(TEXT("so the export does not succeed"), Refused.bSuccess);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleItemSpawnExportRefusalTest,
	"Mantle.ItemSpawn.Editor.Export.ValuesThatCannotPublishAreRefused",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleItemSpawnExportRefusalTest::RunTest(const FString& Parameters)
{
	using namespace MantleItemSpawnExportTestPrivate;
	FItemSpawnWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	AMantleItemSpawnPoint* Spawn = SpawnItemSpawn(Fixture.World, TEXT("Bad spawn"), TEXT("Not An Id"));
	if (!TestNotNull(TEXT("the spawn"), Spawn))
	{
		return false;
	}

	// Check and Export hold the spawn to the same rules, in the same words.
	FMantleValidationResult Validation = FMantlePlaceValidator::Validate(Fixture.World);
	TestTrue(TEXT("Check refuses an item id with capitals and spaces"), HasIssue(Validation, TEXT("names item 'Not An Id'")));
	FMantleExportResult Result = FMantlePlaceExporter::Export(Fixture.World, FMantleExportOptions());
	TestTrue(TEXT("Export refuses it"), HasErrorAt(Result, TEXT("/properties/item")));
	TestFalse(TEXT("so the export does not succeed"), Result.bSuccess);

	Spawn->Item = TEXT("_reserved");
	Validation = FMantlePlaceValidator::Validate(Fixture.World);
	TestTrue(TEXT("Check refuses a leading underscore"), HasIssue(Validation, TEXT("names item '_reserved'")));

	Spawn->Item = FString();
	Validation = FMantlePlaceValidator::Validate(Fixture.World);
	TestTrue(TEXT("Check refuses no item"), HasIssue(Validation, TEXT("names no item")));

	Spawn->Item = TEXT("ok_item");
	Spawn->Count = 0;
	Validation = FMantlePlaceValidator::Validate(Fixture.World);
	TestTrue(TEXT("Check refuses a count of 0"), HasIssue(Validation, TEXT("holds 0")));
	Result = FMantlePlaceExporter::Export(Fixture.World, FMantleExportOptions());
	TestTrue(TEXT("Export refuses it at count"), HasErrorAt(Result, TEXT("/properties/count")));
	Spawn->Count = 10000;
	Validation = FMantlePlaceValidator::Validate(Fixture.World);
	TestTrue(TEXT("Check refuses a count past 9999"), HasIssue(Validation, TEXT("holds 10000")));

	Spawn->Count = 9999;
	Spawn->RespawnSeconds = 4.0;
	Validation = FMantlePlaceValidator::Validate(Fixture.World);
	TestTrue(TEXT("Check refuses a respawn time under 5 seconds"), HasIssue(Validation, TEXT("respawns after 4 s")));
	Result = FMantlePlaceExporter::Export(Fixture.World, FMantleExportOptions());
	TestTrue(TEXT("Export refuses it at respawnSeconds"), HasErrorAt(Result, TEXT("/properties/respawnSeconds")));
	Spawn->RespawnSeconds = 86401.0;
	Validation = FMantlePlaceValidator::Validate(Fixture.World);
	TestTrue(TEXT("Check refuses a respawn time past a day"), HasIssue(Validation, TEXT("respawns after 86401 s")));

	Spawn->RespawnSeconds = 5.0;
	Validation = FMantlePlaceValidator::Validate(Fixture.World);
	TestFalse(TEXT("the limits themselves are accepted"),
		HasIssue(Validation, TEXT("holds")) || HasIssue(Validation, TEXT("respawns after")) || HasIssue(Validation, TEXT("names item")));

	Spawn->ModelId = TEXT("/Game/Nowhere/SM_Missing.SM_Missing");
	Validation = FMantlePlaceValidator::Validate(Fixture.World);
	TestTrue(TEXT("Check refuses a model that is no Static Mesh"), HasIssue(Validation, TEXT("uses the model '/Game/Nowhere/SM_Missing.SM_Missing'")));
	Spawn->ModelId = TEXT("not an id");
	Validation = FMantlePlaceValidator::Validate(Fixture.World);
	TestTrue(TEXT("and a model that is neither a mesh nor an asset id"), HasIssue(Validation, TEXT("uses the model 'not an id'")));
	Spawn->ModelId = AssetModel;
	Validation = FMantlePlaceValidator::Validate(Fixture.World);
	TestFalse(TEXT("but accepts an asset id"), HasIssue(Validation, TEXT("uses the model")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleItemSpawnSaveWorldItemsTest,
	"Mantle.ItemSpawn.Editor.PlaceSettings.SaveWorldItemsWritesPersistenceOnlyWhenOn",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleItemSpawnSaveWorldItemsTest::RunTest(const FString& Parameters)
{
	using namespace MantleItemSpawnExportTestPrivate;
	FItemSpawnWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	AMantlePlaceSettingsActor* Settings = AMantlePlaceSettingsActor::FindOrAdd(Fixture.World);
	if (!TestNotNull(TEXT("a settings actor"), Settings))
	{
		return false;
	}

	// Off (the default): no worldItems key at all.
	TestFalse(TEXT("Save World Items is off by default"), Settings->bSaveWorldItems);
	FMantlePlace Off;
	AMantlePlaceSettingsActor::ApplyTo(Fixture.World, Off.Settings);
	TestTrue(TEXT("nothing is saved"), Off.Settings.WorldItems.Persistence == EMantleWorldItemsPersistence::None);
	TestFalse(TEXT("no worldItems is written"), WorldItemsObjectOf(FMantlePlaceExporter::ToJson(Off)).IsValid());

	// On: persistence server, and the parser reads it back.
	Settings->bSaveWorldItems = true;
	FMantlePlace On;
	AMantlePlaceSettingsActor::ApplyTo(Fixture.World, On.Settings);
	const FString Json = FMantlePlaceExporter::ToJson(On);
	const TSharedPtr<FJsonObject> WorldItems = WorldItemsObjectOf(Json);
	if (TestTrue(TEXT("worldItems is written"), WorldItems.IsValid()))
	{
		TestEqual(TEXT("its persistence is server"), WorldItems->GetStringField(TEXT("persistence")), FString(TEXT("server")));
	}
	const FMantleParseResult Parsed = FMantlePlaceParser::ParseFromString(Json);
	TestTrue(TEXT("the parser finds no world items error"), !Parsed.Errors.ContainsByPredicate([](const FMantleParseError& Error)
	{
		return Error.Path.StartsWith(TEXT("/settings/worldItems"));
	}));
	TestTrue(TEXT("the parser reads server persistence back"), Parsed.Place.Settings.WorldItems.Persistence == EMantleWorldItemsPersistence::Server);

	// Loading the place file's settings into the actor and out again keeps them.
	Settings->bSaveWorldItems = false;
	AMantlePlaceSettingsActor::SetFrom(Fixture.World, Parsed.Place.Settings);
	TestTrue(TEXT("the actor saves world items again"), Settings->bSaveWorldItems);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
