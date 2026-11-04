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

// The play style (#232, #229) from Place Settings to the place file and back: Default writes nothing, Top-Down and Side-Scroll
// write their style and only the options changed, the runtime parser reads it back, and loading it into the actor keeps it. On a
// transient editor world. Each style's JSON is saved to Saved/MantleTests/PlayStyle<Style>.json, for mantle's schema/validate.py.

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlayStyleSideScrollExportTest,
	"Mantle.Editor.PlayStyleSettingExport.SideScrollRoundTrips",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlayStyleSideScrollExportTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlayStyleSettingExportTestPrivate;
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld*/ false, TEXT("MantlePlayStyleSideScrollExportTestWorld"));
	if (!TestNotNull(TEXT("a test world"), World))
	{
		return false;
	}
	AMantlePlaceSettingsActor* Settings = AMantlePlaceSettingsActor::FindOrAdd(World);
	bool bOk = TestNotNull(TEXT("a settings actor"), Settings);
	if (bOk)
	{
		// Side-Scroll with its defaults: the style alone, and no path (each player's own straight line).
		Settings->PlayStyle = EMantlePlacePlayStyle::SideScroll;
		FMantlePlace Plain;
		AMantlePlaceSettingsActor::ApplyTo(World, Plain.Settings);
		const TSharedPtr<FJsonObject> PlainStyle = PlayStyleObjectOf(FMantlePlaceExporter::ToJson(Plain));
		if (TestTrue(TEXT("a playStyle is written for Side-Scroll"), PlainStyle.IsValid()))
		{
			TestEqual(TEXT("its style is SideScroll"), PlainStyle->GetStringField(TEXT("style")), FString(TEXT("SideScroll")));
			TestFalse(TEXT("default options are not written"), PlainStyle->HasField(TEXT("options")));
		}

		// A path with one corner (the first point's Z is dropped), two changed numbers and Bounds: only those are written.
		Settings->SideScroll.Path = { FVector(0.0, 0.0, 50.0), FVector(1000.0, 0.0, 0.0), FVector(1000.0, 800.0, 0.0) };
		Settings->SideScroll.Distance = 1500.0f;
		Settings->SideScroll.LookAhead = 0.0f;
		Settings->SideScroll.bSetBounds = true;
		Settings->SideScroll.Bounds.Min = 100.0f;
		Settings->SideScroll.Bounds.Max = 1800.0f;
		TestTrue(TEXT("the path and Bounds fit the rules"), Settings->PlayStyleError().IsEmpty());
		FMantlePlace Changed;
		AMantlePlaceSettingsActor::ApplyTo(World, Changed.Settings);
		const FString ChangedJson = FMantlePlaceExporter::ToJson(Changed);
		const TSharedPtr<FJsonObject> ChangedStyle = PlayStyleObjectOf(ChangedJson);
		const TSharedPtr<FJsonObject>* Options = nullptr;
		if (TestTrue(TEXT("changed options are written"), ChangedStyle.IsValid() && ChangedStyle->TryGetObjectField(TEXT("options"), Options)))
		{
			const TArray<TSharedPtr<FJsonValue>>* Path = nullptr;
			if (TestTrue(TEXT("Path is written"), (*Options)->TryGetArrayField(TEXT("Path"), Path)) && TestEqual(TEXT("Path has every point"), Path->Num(), 3))
			{
				const TArray<TSharedPtr<FJsonValue>>& First = (*Path)[0]->AsArray();
				TestEqual(TEXT("a point is [x, y, z]"), First.Num(), 3);
				TestEqual(TEXT("the path is flat"), First.Num() == 3 ? First[2]->AsNumber() : -1.0, 0.0);
				TestEqual(TEXT("the corner's y is written"), (*Path)[2]->AsArray()[1]->AsNumber(), 800.0);
			}
			TestEqual(TEXT("Distance is written"), (*Options)->GetNumberField(TEXT("Distance")), 1500.0);
			TestEqual(TEXT("LookAhead is written"), (*Options)->GetNumberField(TEXT("LookAhead")), 0.0);
			TestFalse(TEXT("an unchanged Height is not written"), (*Options)->HasField(TEXT("Height")));
			TestFalse(TEXT("an unchanged FieldOfView is not written"), (*Options)->HasField(TEXT("FieldOfView")));
			const TSharedPtr<FJsonObject>* Bounds = nullptr;
			if (TestTrue(TEXT("Bounds is written"), (*Options)->TryGetObjectField(TEXT("Bounds"), Bounds)))
			{
				TestEqual(TEXT("Bounds Min is written"), (*Bounds)->GetNumberField(TEXT("Min")), 100.0);
				TestEqual(TEXT("Bounds Max is written"), (*Bounds)->GetNumberField(TEXT("Max")), 1800.0);
			}
		}
		const FMantleParseResult Parsed = FMantlePlaceParser::ParseFromString(ChangedJson);
		TestTrue(TEXT("the parser finds no play style error"), !Parsed.Errors.ContainsByPredicate([](const FMantleParseError& Error)
		{
			return Error.Path.StartsWith(TEXT("/settings/playStyle"));
		}));
		TestTrue(TEXT("the parser reads the style, path and options back"), Parsed.Place.Settings.PlayStyle == Changed.Settings.PlayStyle);

		// Saved for mantle's schema/validate.py.
		const FString ExamplePath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MantleTests"), TEXT("PlayStyleSideScroll.json"));
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(ExamplePath), /*Tree*/ true);
		TestTrue(TEXT("the example is saved"), FFileHelper::SaveStringToFile(ChangedJson, *ExamplePath));
		AddInfo(FString::Printf(TEXT("Side-Scroll example: %s"), *FPaths::ConvertRelativePathToFull(ExamplePath)));

		// Loading the place file's settings into the actor and out again keeps them; the dropped Z comes back as 0.
		Settings->PlayStyle = EMantlePlacePlayStyle::Default;
		Settings->SideScroll = FMantleSideScrollSetting();
		AMantlePlaceSettingsActor::SetFrom(World, Parsed.Place.Settings);
		TestTrue(TEXT("the actor is Side-Scroll again"), Settings->PlayStyle == EMantlePlacePlayStyle::SideScroll);
		if (TestEqual(TEXT("the actor's path is loaded"), Settings->SideScroll.Path.Num(), 3))
		{
			TestEqual(TEXT("a loaded point is flat"), Settings->SideScroll.Path[0], FVector::ZeroVector);
		}
		TestTrue(TEXT("the actor's Bounds are loaded"), Settings->SideScroll.bSetBounds && Settings->SideScroll.Bounds.Max == 1800.0f);
		FMantlePlace Reloaded;
		AMantlePlaceSettingsActor::ApplyTo(World, Reloaded.Settings);
		TestTrue(TEXT("the reloaded actor exports the same play style"), Reloaded.Settings.PlayStyle == Changed.Settings.PlayStyle);

		// What the rules refuse is named before publishing, and left out of what would be written.
		Settings->SideScroll.Bounds.Max = 2500.0f;
		TestFalse(TEXT("Bounds past the path's end is an error"), Settings->PlayStyleError().IsEmpty());
		Settings->SideScroll.Bounds.Max = 1800.0f;
		Settings->SideScroll.Path = { FVector(0.0, 0.0, 0.0), FVector(20.0, 0.0, 0.0) };
		TestFalse(TEXT("a line under 50 cm is an error"), Settings->PlayStyleError().IsEmpty());
		Settings->SideScroll.Path = { FVector(0.0, 0.0, 0.0) };
		TestFalse(TEXT("a one-point path is an error"), Settings->PlayStyleError().IsEmpty());
		Settings->SideScroll.Path.Reset();
		TestFalse(TEXT("Bounds without a Path is an error"), Settings->PlayStyleError().IsEmpty());
		FMantlePlace Refused;
		AMantlePlaceSettingsActor::ApplyTo(World, Refused.Settings);
		TestFalse(TEXT("Bounds without a Path is not written"), Refused.Settings.PlayStyle.SideScroll.bHasBounds);
		Settings->SideScroll.bSetBounds = false;
		TestTrue(TEXT("no path and no Bounds is fine"), Settings->PlayStyleError().IsEmpty());
	}
	World->DestroyWorld(/*bInformEngineOfWorld*/ false);
	if (World->IsRooted())
	{
		World->RemoveFromRoot();
	}
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS
