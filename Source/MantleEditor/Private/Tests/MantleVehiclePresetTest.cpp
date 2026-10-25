// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehiclePreset.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

// Vehicle presets: loading the plugin's copies, parsing and its rejections, matching a concrete path against a
// range key with star segments, and the plugin copies staying in step with schema/vehicle-presets.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVehiclePresetTestPrivate
{
	/** A minimal valid preset; each rejection case below changes one thing in it. */
	FString Good(const FString& Version = TEXT("1"), const FString& Fit = TEXT("\"wheelRadius\": 30, \"trackWidth\": 160, \"minScale\": 0.5, \"maxScale\": 2"), const FString& Ranges = TEXT("\"mass\": [900, 2200]"), const FString& Identity = TEXT("\"id\": \"x\", \"name\": \"X\", \"movementType\": \"wheeled\","))
	{
		return FString::Printf(TEXT("{ \"schemaVersion\": %s, %s \"fit\": { %s }, \"ranges\": { %s } }"), *Version, *Identity, *Fit, *Ranges);
	}

	/** The text with CRLF folded to LF: git converts the JSON files to CRLF on checkout, so compare without line endings. */
	FString Normalized(FString Text)
	{
		Text.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
		return Text;
	}

	FString SchemaDirectory()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT(".."), TEXT("schema"), TEXT("vehicle-presets")));
	}

	TArray<FString> JsonFiles(const FString& Directory)
	{
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *FPaths::Combine(Directory, TEXT("*.json")), true, false);
		Files.Sort();
		return Files;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehiclePresetLoadTest,
	"Mantle.Vehicle.Presets.Load",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehiclePresetLoadTest::RunTest(const FString& Parameters)
{
	const FMantleVehiclePreset* Sedan = FMantleVehiclePresets::Find(TEXT("sedan"));
	const FMantleVehiclePreset* Pickup = FMantleVehiclePresets::Find(TEXT("pickup"));
	TestNotNull(TEXT("the sedan preset loads"), Sedan);
	TestNotNull(TEXT("the pickup preset loads"), Pickup);
	TestNull(TEXT("an unknown id finds nothing"), FMantleVehiclePresets::Find(TEXT("nope")));
	TestTrue(TEXT("All() holds at least the two presets"), FMantleVehiclePresets::All().Num() >= 2);
	if (Sedan)
	{
		TestEqual(TEXT("sedan id"), Sedan->Id, FString(TEXT("sedan")));
		TestEqual(TEXT("sedan movementType"), Sedan->MovementType, FString(TEXT("wheeled")));
		TestEqual(TEXT("sedan wheelRadius"), Sedan->FitWheelRadius, 32.0);
		TestEqual(TEXT("sedan trackWidth"), Sedan->FitTrackWidth, 165.0);
		TestTrue(TEXT("sedan has defaults"), Sedan->Defaults.IsValid());
		FVector2D Range;
		TestTrue(TEXT("sedan narrows wheels/*/radius"), FMantleVehiclePresets::RangeFor(*Sedan, TEXT("wheels/0/radius"), Range));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehiclePresetParseTest,
	"Mantle.Vehicle.Presets.Parse",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehiclePresetParseTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehiclePresetTestPrivate;
	FMantleVehiclePreset Preset;
	FString Error;
	TestTrue(TEXT("a minimal good preset parses"), FMantleVehiclePresets::Parse(Good(), Preset, Error));
	TestEqual(TEXT("the id is read"), Preset.Id, FString(TEXT("x")));
	TestEqual(TEXT("the range is read"), Preset.Ranges.FindRef(TEXT("mass")), FVector2D(900.0, 2200.0));

	FMantleVehiclePreset Bad;
	TestFalse(TEXT("not JSON is rejected"), FMantleVehiclePresets::Parse(TEXT("{ nope"), Bad, Error));
	TestFalse(TEXT("a wrong schemaVersion is rejected"), FMantleVehiclePresets::Parse(Good(TEXT("2")), Bad, Error));
	TestFalse(TEXT("a missing id is rejected"), FMantleVehiclePresets::Parse(Good(TEXT("1"), TEXT("\"wheelRadius\": 30, \"trackWidth\": 160, \"minScale\": 0.5, \"maxScale\": 2"), TEXT(""), TEXT("\"name\": \"X\", \"movementType\": \"wheeled\",")), Bad, Error));
	TestFalse(TEXT("a missing name is rejected"), FMantleVehiclePresets::Parse(Good(TEXT("1"), TEXT("\"wheelRadius\": 30, \"trackWidth\": 160, \"minScale\": 0.5, \"maxScale\": 2"), TEXT(""), TEXT("\"id\": \"x\", \"movementType\": \"wheeled\",")), Bad, Error));
	TestFalse(TEXT("a missing movementType is rejected"), FMantleVehiclePresets::Parse(Good(TEXT("1"), TEXT("\"wheelRadius\": 30, \"trackWidth\": 160, \"minScale\": 0.5, \"maxScale\": 2"), TEXT(""), TEXT("\"id\": \"x\", \"name\": \"X\",")), Bad, Error));
	TestFalse(TEXT("a missing fit is rejected"), FMantleVehiclePresets::Parse(TEXT("{ \"schemaVersion\": 1, \"id\": \"x\", \"name\": \"X\", \"movementType\": \"wheeled\" }"), Bad, Error));
	TestFalse(TEXT("a missing fit value is rejected"), FMantleVehiclePresets::Parse(Good(TEXT("1"), TEXT("\"wheelRadius\": 30, \"trackWidth\": 160, \"minScale\": 0.5")), Bad, Error));
	TestFalse(TEXT("a zero fit value is rejected"), FMantleVehiclePresets::Parse(Good(TEXT("1"), TEXT("\"wheelRadius\": 0, \"trackWidth\": 160, \"minScale\": 0.5, \"maxScale\": 2")), Bad, Error));
	TestFalse(TEXT("a negative fit value is rejected"), FMantleVehiclePresets::Parse(Good(TEXT("1"), TEXT("\"wheelRadius\": 30, \"trackWidth\": -160, \"minScale\": 0.5, \"maxScale\": 2")), Bad, Error));
	TestFalse(TEXT("maxScale below minScale is rejected"), FMantleVehiclePresets::Parse(Good(TEXT("1"), TEXT("\"wheelRadius\": 30, \"trackWidth\": 160, \"minScale\": 2, \"maxScale\": 0.5")), Bad, Error));
	TestFalse(TEXT("a range with one number is rejected"), FMantleVehiclePresets::Parse(Good(TEXT("1"), TEXT("\"wheelRadius\": 30, \"trackWidth\": 160, \"minScale\": 0.5, \"maxScale\": 2"), TEXT("\"mass\": [900]")), Bad, Error));
	TestFalse(TEXT("a range with three numbers is rejected"), FMantleVehiclePresets::Parse(Good(TEXT("1"), TEXT("\"wheelRadius\": 30, \"trackWidth\": 160, \"minScale\": 0.5, \"maxScale\": 2"), TEXT("\"mass\": [1, 2, 3]")), Bad, Error));
	TestFalse(TEXT("a range that is not an array is rejected"), FMantleVehiclePresets::Parse(Good(TEXT("1"), TEXT("\"wheelRadius\": 30, \"trackWidth\": 160, \"minScale\": 0.5, \"maxScale\": 2"), TEXT("\"mass\": 900")), Bad, Error));
	TestFalse(TEXT("a range with min above max is rejected"), FMantleVehiclePresets::Parse(Good(TEXT("1"), TEXT("\"wheelRadius\": 30, \"trackWidth\": 160, \"minScale\": 0.5, \"maxScale\": 2"), TEXT("\"mass\": [2200, 900]")), Bad, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehiclePresetRangeMatchTest,
	"Mantle.Vehicle.Presets.RangeMatch",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehiclePresetRangeMatchTest::RunTest(const FString& Parameters)
{
	FMantleVehiclePreset Preset;
	Preset.Ranges.Add(TEXT("wheels/*/radius"), FVector2D(22.0, 45.0));
	Preset.Ranges.Add(TEXT("mass"), FVector2D(900.0, 2200.0));
	FVector2D Range;
	TestTrue(TEXT("a wheel index matches the star"), FMantleVehiclePresets::RangeFor(Preset, TEXT("wheels/3/radius"), Range));
	TestEqual(TEXT("the star range value"), Range, FVector2D(22.0, 45.0));
	TestTrue(TEXT("an exact key matches exactly"), FMantleVehiclePresets::RangeFor(Preset, TEXT("mass"), Range));
	TestEqual(TEXT("the exact range value"), Range, FVector2D(900.0, 2200.0));
	TestFalse(TEXT("a missing segment does not match"), FMantleVehiclePresets::RangeFor(Preset, TEXT("wheels/radius"), Range));
	TestFalse(TEXT("an extra segment does not match"), FMantleVehiclePresets::RangeFor(Preset, TEXT("wheels/1/2/radius"), Range));
	TestFalse(TEXT("a different leaf does not match"), FMantleVehiclePresets::RangeFor(Preset, TEXT("wheels/1/width"), Range));
	TestFalse(TEXT("an empty segment does not match the star"), FMantleVehiclePresets::RangeFor(Preset, TEXT("wheels//radius"), Range));
	TestFalse(TEXT("an unknown path does not match"), FMantleVehiclePresets::RangeFor(Preset, TEXT("maxSpeed"), Range));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehiclePresetMatchSchemaTest,
	"Mantle.Vehicle.Presets.MatchSchema",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehiclePresetMatchSchemaTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehiclePresetTestPrivate;
	const FString SchemaDir = SchemaDirectory();
	if (!IFileManager::Get().DirectoryExists(*SchemaDir))
	{
		AddInfo(TEXT("No schema/vehicle-presets folder beside the project (an installed plugin has none), so there is nothing to compare."));
		return true;
	}
	const FString PluginDir = FMantleVehiclePresets::Directory();
	TestFalse(TEXT("the plugin's VehiclePresets folder is found"), PluginDir.IsEmpty());
	const TArray<FString> SchemaFiles = JsonFiles(SchemaDir);
	const TArray<FString> PluginFiles = JsonFiles(PluginDir);
	TestEqual(TEXT("the plugin and schema hold the same file names"), PluginFiles, SchemaFiles);
	for (const FString& File : SchemaFiles)
	{
		FString SchemaText;
		FString PluginText;
		if (!TestTrue(*FString::Printf(TEXT("%s reads from schema"), *File), FFileHelper::LoadFileToString(SchemaText, *FPaths::Combine(SchemaDir, File)))
			|| !TestTrue(*FString::Printf(TEXT("%s reads from the plugin"), *File), FFileHelper::LoadFileToString(PluginText, *FPaths::Combine(PluginDir, File))))
		{
			continue;
		}
		TestTrue(*FString::Printf(TEXT("%s: the plugin copy matches schema/vehicle-presets (line endings aside)"), *File), Normalized(SchemaText) == Normalized(PluginText));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
