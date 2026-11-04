// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceExporter.h"
#include "MantlePlaceParser.h"
#include "MantlePlaceSettingsActor.h"
#include "MantlePlaceTypes.h"
#include "MantlePlayStyle.h"

#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

// The play style (#232) from Place Settings to the place file and back: Default writes nothing, Top-Down writes its style
// and only the options changed, the runtime parser reads it back, and loading it into the actor keeps it. On a transient
// editor world. The Top-Down place's JSON is saved to Saved/MantleTests/PlayStyleTopDown.json, for mantle's schema/validate.py.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantlePlayStyleSettingExportTestPrivate
{
	/** The "settings.playStyle" object of a place's JSON, or null when the place file has none. */
	TSharedPtr<FJsonObject> PlayStyleObjectOf(const FString& Json)
	{
		TSharedPtr<FJsonObject> Root;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
		{
			return nullptr;
		}
		const TSharedPtr<FJsonObject>* Settings = nullptr;
		const TSharedPtr<FJsonObject>* PlayStyle = nullptr;
		if (!Root->TryGetObjectField(TEXT("settings"), Settings) || !(*Settings)->TryGetObjectField(TEXT("playStyle"), PlayStyle))
		{
			return nullptr;
		}
		return *PlayStyle;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlayStyleSettingExportTest,
	"Mantle.Editor.PlayStyleSettingExport.TopDownRoundTrips",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlayStyleSettingExportTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlayStyleSettingExportTestPrivate;
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld*/ false, TEXT("MantlePlayStyleSettingExportTestWorld"));
	if (!TestNotNull(TEXT("a test world"), World))
	{
		return false;
	}
	AMantlePlaceSettingsActor* Settings = AMantlePlaceSettingsActor::FindOrAdd(World);
	bool bOk = TestNotNull(TEXT("a settings actor"), Settings);
	if (bOk)
	{
		// Default: no playStyle key at all.
		FMantlePlace Untouched;
		AMantlePlaceSettingsActor::ApplyTo(World, Untouched.Settings);
		TestTrue(TEXT("the style defaults to Default"), Untouched.Settings.PlayStyle.Style == EMantlePlayStyle::Default);
		TestFalse(TEXT("no playStyle is written for Default"), PlayStyleObjectOf(FMantlePlaceExporter::ToJson(Untouched)).IsValid());

		// Top-Down with its defaults: the style alone.
		Settings->PlayStyle = EMantlePlacePlayStyle::TopDown;
		FMantlePlace Plain;
		AMantlePlaceSettingsActor::ApplyTo(World, Plain.Settings);
		const TSharedPtr<FJsonObject> PlainStyle = PlayStyleObjectOf(FMantlePlaceExporter::ToJson(Plain));
		if (TestTrue(TEXT("a playStyle is written for Top-Down"), PlainStyle.IsValid()))
		{
			TestEqual(TEXT("its style is TopDown"), PlainStyle->GetStringField(TEXT("style")), FString(TEXT("TopDown")));
			TestFalse(TEXT("default options are not written"), PlainStyle->HasField(TEXT("options")));
		}

		// Some options changed: only those are written, and the parser reads every one back.
		Settings->TopDown.Distance = 2000.0f;
		Settings->TopDown.MaxDistance = 3000.0f;
		Settings->TopDown.Pitch = 70.0f;
		Settings->TopDown.Yaw = 90.0f;
		Settings->TopDown.bClickToMove = true;
		Settings->TopDown.bHideWalls = false;
		TestTrue(TEXT("the changed options fit together"), Settings->PlayStyleError().IsEmpty());
		FMantlePlace Changed;
		AMantlePlaceSettingsActor::ApplyTo(World, Changed.Settings);
		const FString ChangedJson = FMantlePlaceExporter::ToJson(Changed);
		const TSharedPtr<FJsonObject> ChangedStyle = PlayStyleObjectOf(ChangedJson);
		const TSharedPtr<FJsonObject>* Options = nullptr;
		if (TestTrue(TEXT("changed options are written"), ChangedStyle.IsValid() && ChangedStyle->TryGetObjectField(TEXT("options"), Options)))
		{
			TestEqual(TEXT("Distance is written"), (*Options)->GetNumberField(TEXT("Distance")), 2000.0);
			TestEqual(TEXT("MaxDistance is written"), (*Options)->GetNumberField(TEXT("MaxDistance")), 3000.0);
			TestEqual(TEXT("Pitch is written"), (*Options)->GetNumberField(TEXT("Pitch")), 70.0);
			TestEqual(TEXT("Yaw is written"), (*Options)->GetNumberField(TEXT("Yaw")), 90.0);
			TestTrue(TEXT("ClickToMove is written"), (*Options)->GetBoolField(TEXT("ClickToMove")));
			TestFalse(TEXT("HideWalls is written"), (*Options)->GetBoolField(TEXT("HideWalls")));
			TestFalse(TEXT("an unchanged MinDistance is not written"), (*Options)->HasField(TEXT("MinDistance")));
			TestFalse(TEXT("an unchanged FieldOfView is not written"), (*Options)->HasField(TEXT("FieldOfView")));
		}
		const FMantleParseResult Parsed = FMantlePlaceParser::ParseFromString(ChangedJson);
		TestTrue(TEXT("the parser finds no play style error"), !Parsed.Errors.ContainsByPredicate([](const FMantleParseError& Error)
		{
			return Error.Path.StartsWith(TEXT("/settings/playStyle"));
		}));
		TestTrue(TEXT("the parser reads the style and options back"), Parsed.Place.Settings.PlayStyle == Changed.Settings.PlayStyle);

		// Saved for mantle's schema/validate.py.
		const FString ExamplePath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MantleTests"), TEXT("PlayStyleTopDown.json"));
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(ExamplePath), /*Tree*/ true);
		TestTrue(TEXT("the example is saved"), FFileHelper::SaveStringToFile(ChangedJson, *ExamplePath));
		AddInfo(FString::Printf(TEXT("Top-Down example: %s"), *FPaths::ConvertRelativePathToFull(ExamplePath)));

		// Loading the place file's settings into the actor and out again keeps them.
		Settings->PlayStyle = EMantlePlacePlayStyle::Default;
		Settings->TopDown = FMantleTopDownSetting();
		AMantlePlaceSettingsActor::SetFrom(World, Parsed.Place.Settings);
		TestTrue(TEXT("the actor is Top-Down again"), Settings->PlayStyle == EMantlePlacePlayStyle::TopDown);
		TestEqual(TEXT("the actor's Distance is loaded"), Settings->TopDown.Distance, 2000.0f);
		TestTrue(TEXT("the actor's ClickToMove is loaded"), Settings->TopDown.bClickToMove);
		TestTrue(TEXT("the actor's HideWalls is loaded"), !Settings->TopDown.bHideWalls);
		FMantlePlace Reloaded;
		AMantlePlaceSettingsActor::ApplyTo(World, Reloaded.Settings);
		TestTrue(TEXT("the reloaded actor exports the same play style"), Reloaded.Settings.PlayStyle == Changed.Settings.PlayStyle);

		// A Distance outside the zoom range is named before publishing.
		Settings->TopDown.Distance = 4000.0f;
		TestFalse(TEXT("a Distance past Max Distance is an error"), Settings->PlayStyleError().IsEmpty());
	}
	World->DestroyWorld(/*bInformEngineOfWorld*/ false);
	if (World->IsRooted())
	{
		World->RemoveFromRoot();
	}
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS
