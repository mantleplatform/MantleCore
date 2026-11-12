// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceExporter.h"
#include "MantlePlaceParser.h"
#include "MantlePlaceSettingsActor.h"
#include "MantlePlaceTypes.h"

#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

// The Health and Combat checkboxes (#332) from Place Settings to the place file and back: both are on in a new settings actor and
// written as true; a switch that is off is left out of the file entirely, each one on its own; the runtime parser reads them back,
// and loading a place file into the actor keeps them (a file that leaves them out unticks both). On a transient editor world.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleHealthCombatSettingExportTestPrivate
{
	/** The "settings" object of a place's JSON, or null when it cannot be read. */
	TSharedPtr<FJsonObject> SettingsObjectOf(const FString& Json)
	{
		TSharedPtr<FJsonObject> Root;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
		{
			return nullptr;
		}
		const TSharedPtr<FJsonObject>* Settings = nullptr;
		return Root->TryGetObjectField(TEXT("settings"), Settings) ? *Settings : nullptr;
	}

	/** The place file the settings actor of World exports. */
	FString ExportedJson(UWorld* World)
	{
		FMantlePlace Place;
		AMantlePlaceSettingsActor::ApplyTo(World, Place.Settings);
		return FMantlePlaceExporter::ToJson(Place);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleHealthCombatSettingExportTest,
	"Mantle.Editor.HealthCombatSettingExport.WritesOnlyWhatIsOn",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleHealthCombatSettingExportTest::RunTest(const FString& Parameters)
{
	using namespace MantleHealthCombatSettingExportTestPrivate;
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld*/ false, TEXT("MantleHealthCombatSettingExportTestWorld"));
	if (!TestNotNull(TEXT("a test world"), World))
	{
		return false;
	}
	AMantlePlaceSettingsActor* Settings = AMantlePlaceSettingsActor::FindOrAdd(World);
	bool bOk = TestNotNull(TEXT("a settings actor"), Settings);
	if (bOk)
	{
		// A freshly spawned settings actor has both ticked and exports both as true.
		TestTrue(TEXT("Health is ticked in a new settings actor"), Settings->bHealth);
		TestTrue(TEXT("Combat is ticked in a new settings actor"), Settings->bCombat);
		const FString FreshJson = ExportedJson(World);
		const TSharedPtr<FJsonObject> FreshSettings = SettingsObjectOf(FreshJson);
		if (TestTrue(TEXT("the fresh export has a settings object"), FreshSettings.IsValid()))
		{
			bool bValue = false;
			TestTrue(TEXT("a fresh actor writes health"), FreshSettings->TryGetBoolField(TEXT("health"), bValue) && bValue);
			bValue = false;
			TestTrue(TEXT("a fresh actor writes combat"), FreshSettings->TryGetBoolField(TEXT("combat"), bValue) && bValue);
		}

		// Both off: neither key is in the file.
		Settings->bHealth = false;
		Settings->bCombat = false;
		const FString OffJson = ExportedJson(World);
		const TSharedPtr<FJsonObject> OffSettings = SettingsObjectOf(OffJson);
		if (TestTrue(TEXT("the export with both off has a settings object"), OffSettings.IsValid()))
		{
			TestFalse(TEXT("health is left out when off"), OffSettings->HasField(TEXT("health")));
			TestFalse(TEXT("combat is left out when off"), OffSettings->HasField(TEXT("combat")));
		}
		const FMantleParseResult OffParsed = FMantlePlaceParser::ParseFromString(OffJson);
		TestFalse(TEXT("the parser reads health as off when left out"), OffParsed.Place.Settings.bHealth);
		TestFalse(TEXT("the parser reads combat as off when left out"), OffParsed.Place.Settings.bCombat);

		// Health on its own.
		Settings->bHealth = true;
		Settings->bCombat = false;
		const FString HealthJson = ExportedJson(World);
		const TSharedPtr<FJsonObject> HealthSettings = SettingsObjectOf(HealthJson);
		if (TestTrue(TEXT("the health-only export has a settings object"), HealthSettings.IsValid()))
		{
			bool bValue = false;
			TestTrue(TEXT("health is written true"), HealthSettings->TryGetBoolField(TEXT("health"), bValue) && bValue);
			TestFalse(TEXT("combat is left out when only health is on"), HealthSettings->HasField(TEXT("combat")));
		}
		const FMantleParseResult HealthParsed = FMantlePlaceParser::ParseFromString(HealthJson);
		TestTrue(TEXT("the parser reads health back as on"), HealthParsed.Place.Settings.bHealth);
		TestFalse(TEXT("the parser reads combat back as off"), HealthParsed.Place.Settings.bCombat);

		// Combat on its own.
		Settings->bHealth = false;
		Settings->bCombat = true;
		const FString CombatJson = ExportedJson(World);
		const TSharedPtr<FJsonObject> CombatSettings = SettingsObjectOf(CombatJson);
		if (TestTrue(TEXT("the combat-only export has a settings object"), CombatSettings.IsValid()))
		{
			bool bValue = false;
			TestTrue(TEXT("combat is written true"), CombatSettings->TryGetBoolField(TEXT("combat"), bValue) && bValue);
			TestFalse(TEXT("health is left out when only combat is on"), CombatSettings->HasField(TEXT("health")));
		}
		const FMantleParseResult CombatParsed = FMantlePlaceParser::ParseFromString(CombatJson);
		TestFalse(TEXT("the parser reads health back as off"), CombatParsed.Place.Settings.bHealth);
		TestTrue(TEXT("the parser reads combat back as on"), CombatParsed.Place.Settings.bCombat);

		// Loading a place file's settings into the actor keeps them; a file that leaves them out unticks both.
		Settings->bHealth = false;
		Settings->bCombat = false;
		AMantlePlaceSettingsActor::SetFrom(World, HealthParsed.Place.Settings);
		TestTrue(TEXT("the actor's Health is loaded on"), Settings->bHealth);
		TestFalse(TEXT("the actor's Combat is loaded off"), Settings->bCombat);
		AMantlePlaceSettingsActor::SetFrom(World, CombatParsed.Place.Settings);
		TestFalse(TEXT("the actor's Health is loaded off"), Settings->bHealth);
		TestTrue(TEXT("the actor's Combat is loaded on"), Settings->bCombat);
		Settings->bHealth = true;
		Settings->bCombat = true;
		AMantlePlaceSettingsActor::SetFrom(World, OffParsed.Place.Settings);
		TestFalse(TEXT("a file that leaves health out unticks Health"), Settings->bHealth);
		TestFalse(TEXT("a file that leaves combat out unticks Combat"), Settings->bCombat);
	}
	World->DestroyWorld(/*bInformEngineOfWorld*/ false);
	if (World->IsRooted())
	{
		World->RemoveFromRoot();
	}
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS
