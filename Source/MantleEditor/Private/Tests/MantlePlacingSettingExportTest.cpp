// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceExporter.h"
#include "MantlePlaceLimits.h"
#include "MantlePlaceParser.h"
#include "MantlePlaceSettingsActor.h"
#include "MantlePlaceTypes.h"

#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

// Placing mode's settings (#184) from Place Settings to the place file: nothing is written until the creator sets
// something, then only what they set, and the runtime parser reads it back. On a transient editor world.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantlePlacingSettingExportTestPrivate
{
	/** The "settings.placing" object of a place's JSON, or null when the place file has none. */
	TSharedPtr<FJsonObject> PlacingObjectOf(const FString& Json)
	{
		TSharedPtr<FJsonObject> Root;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
		{
			return nullptr;
		}
		const TSharedPtr<FJsonObject>* Settings = nullptr;
		const TSharedPtr<FJsonObject>* Placing = nullptr;
		if (!Root->TryGetObjectField(TEXT("settings"), Settings) || !(*Settings)->TryGetObjectField(TEXT("placing"), Placing))
		{
			return nullptr;
		}
		return *Placing;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlacingSettingExportTest,
	"Mantle.Editor.PlacingSettingExport.WrittenOnlyWhenSet",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlacingSettingExportTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlacingSettingExportTestPrivate;
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld*/ false, TEXT("MantlePlacingSettingExportTestWorld"));
	if (!TestNotNull(TEXT("a test world"), World))
	{
		return false;
	}
	AMantlePlaceSettingsActor* Settings = AMantlePlaceSettingsActor::FindOrAdd(World);
	bool bOk = TestNotNull(TEXT("a settings actor"), Settings);
	if (bOk)
	{
		// Untouched: the block is left out, so a place that does not use placing exports as it always did.
		FMantlePlace Untouched;
		AMantlePlaceSettingsActor::ApplyTo(World, Untouched.Settings);
		TestTrue(TEXT("persistence defaults to none"), Untouched.Settings.Placing.Persistence == EMantlePlacingPersistence::None);
		TestEqual(TEXT("the cap defaults to the most a place may have"), Untouched.Settings.Placing.MaxPieces, FMantlePlaceLimits::MaxPlacedPieces);
		TestFalse(TEXT("no placing block is written"), PlacingObjectOf(FMantlePlaceExporter::ToJson(Untouched)).IsValid());

		// Persistence alone: only persistence is written.
		Settings->PlacedPieces = EMantlePlacePersistence::Server;
		FMantlePlace Saved;
		AMantlePlaceSettingsActor::ApplyTo(World, Saved.Settings);
		const FString SavedJson = FMantlePlaceExporter::ToJson(Saved);
		const TSharedPtr<FJsonObject> SavedPlacing = PlacingObjectOf(SavedJson);
		if (TestTrue(TEXT("a placing block is written"), SavedPlacing.IsValid()))
		{
			TestEqual(TEXT("persistence is server"), SavedPlacing->GetStringField(TEXT("persistence")), FString(TEXT("server")));
			TestFalse(TEXT("an unset cap is not written"), SavedPlacing->HasField(TEXT("maxPieces")));
		}
		const FMantleParseResult Parsed = FMantlePlaceParser::ParseFromString(SavedJson);
		TestTrue(TEXT("the parser reads it back"), Parsed.Place.Settings.Placing.Persistence == EMantlePlacingPersistence::Server);

		// A cap alone, then both.
		Settings->PlacedPieces = EMantlePlacePersistence::None;
		Settings->bSetMaxPlacedPieces = true;
		Settings->MaxPlacedPieces = 500;
		FMantlePlace Capped;
		AMantlePlaceSettingsActor::ApplyTo(World, Capped.Settings);
		const FString CappedJson = FMantlePlaceExporter::ToJson(Capped);
		const TSharedPtr<FJsonObject> CappedPlacing = PlacingObjectOf(CappedJson);
		if (TestTrue(TEXT("a cap alone writes a placing block"), CappedPlacing.IsValid()))
		{
			TestEqual(TEXT("maxPieces is written"), static_cast<int32>(CappedPlacing->GetNumberField(TEXT("maxPieces"))), 500);
			TestFalse(TEXT("persistence none is not written"), CappedPlacing->HasField(TEXT("persistence")));
		}
		TestEqual(TEXT("the parser reads the cap back"), FMantlePlaceParser::ParseFromString(CappedJson).Place.Settings.Placing.MaxPieces, 500);

		// Loading a place file's settings into the actor and out again keeps them.
		AMantlePlaceSettingsActor::SetFrom(World, Capped.Settings);
		TestTrue(TEXT("the cap switch follows a loaded cap"), Settings->bSetMaxPlacedPieces && Settings->MaxPlacedPieces == 500);
	}
	World->DestroyWorld(/*bInformEngineOfWorld*/ false);
	if (World->IsRooted())
	{
		World->RemoveFromRoot();
	}
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS
