// Copyright Joshua Eger. All Rights Reserved.
#include "Dom/JsonObject.h"
#include "MantleEngineSounds.h"
#include "MantleVehicleAsset.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

// engine.sound (#258): the reader takes the engine sound a creator picked as its id (any string: the cook checks it), absent is None (the type's
// default), and FMantleEngineSounds gives a car or a boat a car's engines, a two-wheeler a motorcycle's and handlebars on more wheels both, as the cook does.
namespace MantleVehicleEngineSoundDocTestPrivate
{
	/** A four-wheel car's document with this engine block; GearRatios one gear for an electric motor. */
	bool ReadEngine(const FString& EngineJson, FMantleVehicleDesc& Out, FString& OutError, const TCHAR* GearRatios = TEXT("[3.2,2.1,1.4,1.0]"))
	{
		const FString Doc = FString::Printf(TEXT("{\"schemaVersion\":1,\"engine\":%s,\"transmission\":{\"gearRatios\":%s,\"finalDrive\":3.5},")
			TEXT("\"wheels\":[{\"bone\":\"Wheel_FL\",\"steering\":true,\"driven\":false},{\"bone\":\"Wheel_FR\",\"steering\":true,\"driven\":false},")
			TEXT("{\"bone\":\"Wheel_BL\",\"handbrake\":true},{\"bone\":\"Wheel_BR\",\"handbrake\":true}]}"), *EngineJson, GearRatios);
		const FTCHARToUTF8 Utf8(*Doc);
		return FMantleVehicleAssetReader::ParseDocument(TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()), Out, OutError);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleEngineSoundDocTest,
	"Mantle.Vehicle.Document.EngineSound",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleEngineSoundDocTest::RunTest(const FString&)
{
	using namespace MantleVehicleEngineSoundDocTestPrivate;
	const FString Combustion = TEXT("\"maxTorque\":400,\"maxRPM\":6000,\"torqueCurve\":[[0,0.6],[3000,1],[6000,0.8]]");
	FMantleVehicleDesc D;
	FString Error;
	if (TestTrue(*FString::Printf(TEXT("a car with engine.sound reads (%s)"), *Error), ReadEngine(TEXT("{") + Combustion + TEXT(",\"sound\":\"v8\"}"), D, Error)))
	{
		TestEqual(TEXT("its engine sound"), D.EngineSound, FName(TEXT("v8")));
	}
	if (TestTrue(*FString::Printf(TEXT("a car without one reads (%s)"), *Error), ReadEngine(TEXT("{") + Combustion + TEXT("}"), D, Error)))
	{
		TestTrue(TEXT("and has none: the type's default"), D.EngineSound.IsNone());
	}
	// An id this client has no sound for still reads (it plays the default); a non-string is refused at the key.
	TestTrue(TEXT("an id this client does not know reads"), ReadEngine(TEXT("{") + Combustion + TEXT(",\"sound\":\"jet_fighter\"}"), D, Error));
	TestFalse(TEXT("a number is refused"), ReadEngine(TEXT("{") + Combustion + TEXT(",\"sound\":8}"), D, Error));
	TestTrue(*FString::Printf(TEXT("at /engine/sound (%s)"), *Error), Error.Contains(TEXT("/engine/sound")));
	// An electric motor's block reads the key too (the cook refuses it there, in words that say why).
	TestTrue(*FString::Printf(TEXT("an electric block with a sound reads (%s)"), *Error),
		ReadEngine(TEXT("{\"drive\":\"electric\",\"maxTorque\":377,\"maxRPM\":18447,\"peakPower\":213,\"sound\":\"v8\"}"), D, Error, TEXT("[1.0]")));

	// Which engines a vehicle may pick.
	TestTrue(TEXT("a wheeled vehicle on two wheels is a two-wheeler"), FMantleEngineSounds::IsTwoWheeler(true, 2));
	TestFalse(TEXT("a car is not"), FMantleEngineSounds::IsTwoWheeler(true, 4));
	TestFalse(TEXT("nor is anything not wheeled"), FMantleEngineSounds::IsTwoWheeler(false, 2));
	TArray<FName> Ids;
	FMantleEngineSounds::Options(/*bTwoWheeler*/ false, /*bHandlebars*/ false, Ids);
	TestEqual(TEXT("a car or a boat: a car's engines"), Ids, FMantleEngineSounds::Car());
	FMantleEngineSounds::Options(true, true, Ids);
	TestEqual(TEXT("a motorcycle: a motorcycle's"), Ids, FMantleEngineSounds::Bike());
	FMantleEngineSounds::Options(false, true, Ids);
	TestEqual(TEXT("an ATV or a jet ski: both"), Ids.Num(), FMantleEngineSounds::Car().Num() + FMantleEngineSounds::Bike().Num());
	TestTrue(TEXT("an ATV takes inline3"), FMantleEngineSounds::Fits(TEXT("inline3"), false, true));
	TestTrue(TEXT("an ATV takes bike_inline4"), FMantleEngineSounds::Fits(TEXT("bike_inline4"), false, true));
	TestFalse(TEXT("a car does not take bike_vtwin"), FMantleEngineSounds::Fits(TEXT("bike_vtwin"), false, false));
	TestFalse(TEXT("a motorcycle does not take v8"), FMantleEngineSounds::Fits(TEXT("v8"), true, true));
	TestFalse(TEXT("an unknown id fits nothing"), FMantleEngineSounds::Fits(TEXT("jet_fighter"), false, true));
	TestTrue(TEXT("a boat picks one"), FMantleEngineSounds::CanPick(true, false, false));
	TestFalse(TEXT("a pedalled vehicle picks none"), FMantleEngineSounds::CanPick(true, true, false));
	TestFalse(TEXT("an electric motor picks none"), FMantleEngineSounds::CanPick(true, false, true));
	TestFalse(TEXT("a plane picks none"), FMantleEngineSounds::CanPick(false, false, false));

	// The lists are the schema's engine.sound enum, car then motorcycle (only where the game's schema is beside the project; the plugin has none).
	const FString SchemaPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("../schema/vehicle.schema.json")));
	FString SchemaText;
	if (FFileHelper::LoadFileToString(SchemaText, *SchemaPath))
	{
		TSharedPtr<FJsonObject> Schema;
		const TSharedPtr<FJsonObject>* Properties = nullptr;
		const TSharedPtr<FJsonObject>* Engine = nullptr;
		const TSharedPtr<FJsonObject>* EngineProperties = nullptr;
		const TSharedPtr<FJsonObject>* Sound = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Enum = nullptr;
		if (TestTrue(TEXT("the schema has engine.sound's enum"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(SchemaText), Schema) && Schema.IsValid()
				&& Schema->TryGetObjectField(TEXT("properties"), Properties) && (*Properties)->TryGetObjectField(TEXT("engine"), Engine)
				&& (*Engine)->TryGetObjectField(TEXT("properties"), EngineProperties) && (*EngineProperties)->TryGetObjectField(TEXT("sound"), Sound)
				&& (*Sound)->TryGetArrayField(TEXT("enum"), Enum)))
		{
			TArray<FName> SchemaIds;
			for (const TSharedPtr<FJsonValue>& Value : *Enum)
			{
				SchemaIds.Add(FName(*Value->AsString()));
			}
			TArray<FName> Ours = FMantleEngineSounds::Car();
			Ours.Append(FMantleEngineSounds::Bike());
			TestEqual(TEXT("the schema's ids are ours"), SchemaIds, Ours);
		}
	}
	return true;
}

#endif
