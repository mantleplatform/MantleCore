// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleConverter.h"
#include "MantlePlaceValidator.h"
#include "MantleVehicleAsset.h"
#include "MantleVehicleSource.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#if WITH_DEV_AUTOMATION_TESTS

// A vehicle built from a preset carries its id into the document, the runtime reader accepts the key, and
// CheckSource holds the vehicle to the preset's narrower ranges.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVehiclePresetCheckTestPrivate
{
	UMantleVehicleSource* NewSource(const TCHAR* Preset, float Mass)
	{
		UMantleVehicleSource* Source = NewObject<UMantleVehicleSource>(GetTransientPackage());
		Source->Preset = Preset;
		Source->Tuning.Mass = Mass;
		return Source;
	}

	/** The text of every issue CheckSource reports for the source, one per line. */
	FString CheckText(const UMantleVehicleSource* Source)
	{
		TArray<FMantleValidationIssue> Issues;
		FMantleVehicleConverter::CheckSource(Source, FString(), Issues);
		FString Text;
		for (const FMantleValidationIssue& Issue : Issues)
		{
			Text += Issue.ToString() + TEXT("\n");
		}
		return Text;
	}

	/** The document MakeDocument writes for a default source, with one extra key set, as the bytes the reader takes. */
	TArray<uint8> DocumentBytes(const TSharedPtr<FJsonValue>& Extra, const FString& ExtraKey)
	{
		UMantleVehicleSource* Source = NewObject<UMantleVehicleSource>(GetTransientPackage());
		const TSharedRef<FJsonObject> Doc = FMantleVehicleConverter::MakeDocument(*Source, TMap<FString, FString>());
		Doc->SetField(ExtraKey, Extra);
		FString Text;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Doc, Writer);
		const FTCHARToUTF8 Utf8(*Text);
		return TArray<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehiclePresetDocumentTest,
	"Mantle.Vehicle.Presets.DocumentCarriesPreset",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehiclePresetDocumentTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehiclePresetCheckTestPrivate;
	const TSharedRef<FJsonObject> WithPreset = FMantleVehicleConverter::MakeDocument(*NewSource(TEXT("sedan"), 1500.f), TMap<FString, FString>());
	FString Id;
	TestTrue(TEXT("a vehicle built from a preset writes the preset key"), WithPreset->TryGetStringField(TEXT("preset"), Id));
	TestEqual(TEXT("the preset key is the preset id"), Id, FString(TEXT("sedan")));

	const TSharedRef<FJsonObject> Without = FMantleVehicleConverter::MakeDocument(*NewSource(TEXT(""), 1500.f), TMap<FString, FString>());
	TestFalse(TEXT("a vehicle with no preset writes no preset key"), Without->HasField(TEXT("preset")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehiclePresetParseTest2,
	"Mantle.Vehicle.Presets.ParseAcceptsPreset",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehiclePresetParseTest2::RunTest(const FString& Parameters)
{
	using namespace MantleVehiclePresetCheckTestPrivate;
	// The reader may refuse this document for other reasons (a default vehicle has no wheels); what matters is that it is
	// never the preset key, and that a key it does not know is still refused by name.
	FMantleVehicleDesc Desc;
	FString Error;
	FMantleVehicleAssetReader::ParseDocument(DocumentBytes(MakeShared<FJsonValueString>(TEXT("sedan")), TEXT("preset")), Desc, Error);
	TestFalse(TEXT("a string preset key is accepted"), Error.Contains(TEXT("preset")));

	Error.Reset();
	TestFalse(TEXT("a preset key that is not a string is refused"),
		FMantleVehicleAssetReader::ParseDocument(DocumentBytes(MakeShared<FJsonValueNumber>(3.0), TEXT("preset")), Desc, Error));
	TestTrue(TEXT("and the refusal names the preset key"), Error.Contains(TEXT("preset")));

	Error.Reset();
	TestFalse(TEXT("another unknown key is still refused"),
		FMantleVehicleAssetReader::ParseDocument(DocumentBytes(MakeShared<FJsonValueString>(TEXT("x")), TEXT("bogus")), Desc, Error));
	TestTrue(TEXT("and the refusal names it"), Error.Contains(TEXT("bogus")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehiclePresetEnforceTest,
	"Mantle.Vehicle.Presets.CheckEnforcesRanges",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehiclePresetEnforceTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehiclePresetCheckTestPrivate;
	// CheckSource runs every number check before it reads the body, so a source with no body is enough for these.
	const FString Heavy = CheckText(NewSource(TEXT("sedan"), 3000.f));
	TestTrue(TEXT("a 3000 kg sedan is refused, naming the preset"), Heavy.Contains(TEXT("Sedan")));
	TestTrue(TEXT("with the preset's range"), Heavy.Contains(TEXT("900 to 2200")));

	TestTrue(TEXT("a too-heavy sedan is told to lower it"), Heavy.Contains(TEXT("Lower it")));

	const FString Light = CheckText(NewSource(TEXT("sedan"), 500.f));
	TestTrue(TEXT("a 500 kg sedan is refused with the preset's range"), Light.Contains(TEXT("900 to 2200")));
	TestTrue(TEXT("a too-light sedan is told to raise it"), Light.Contains(TEXT("Raise it")));
	TestFalse(TEXT("and not to lower it"), Light.Contains(TEXT("Lower it")));

	const FString Fine = CheckText(NewSource(TEXT("sedan"), 1500.f));
	TestFalse(TEXT("a 1500 kg sedan is within the preset"), Fine.Contains(TEXT("900 to 2200")));

	const FString Unknown = CheckText(NewSource(TEXT("no-such-preset"), 1500.f));
	TestTrue(TEXT("an unknown preset is named"), Unknown.Contains(TEXT("no-such-preset")));
	TestTrue(TEXT("and said to be unknown to this plugin"), Unknown.Contains(TEXT("doesn't know")));

	const FString NoPreset = CheckText(NewSource(TEXT(""), 3000.f));
	TestFalse(TEXT("with no preset only the absolute limits apply"), NoPreset.Contains(TEXT("Sedan")) || NoPreset.Contains(TEXT("preset")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
