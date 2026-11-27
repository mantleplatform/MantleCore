// Copyright Joshua Eger. All Rights Reserved.
#include "Dom/JsonObject.h"
#include "MantleEngineSounds.h"
#include "MantleVehicleAsset.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#if WITH_DEV_AUTOMATION_TESTS

// engine.sound (#258): the reader takes the engine sound a creator picked as its id (any string: the cook checks it), absent is None (the type's
// default), and FMantleEngineSounds gives a car or a boat a car's engines, a two-wheeler a motorcycle's and handlebars on more wheels both, as the cook does.
namespace MantleVehicleEngineSoundDocTestPrivate
{
	/** The cook's own complete document (schema/fixtures/vehicle-v1.mvh, a car with no engine.sound) as JSON, as the other document tests read it. */
	TSharedPtr<FJsonObject> CookedCar(FString& OutError)
	{
		const FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("../schema/fixtures/vehicle-v1.mvh")));
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *Path) || Bytes.Num() < 12)
		{
			OutError = FString::Printf(TEXT("no cooked vehicle at %s"), *Path);
			return nullptr;
		}
		const uint32 DocumentBytes = *reinterpret_cast<const uint32*>(Bytes.GetData() + 8);
		FString Json;
		FFileHelper::BufferToString(Json, Bytes.GetData() + 12, static_cast<int32>(DocumentBytes));
		TSharedPtr<FJsonObject> Root;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
		{
			OutError = TEXT("the cooked vehicle's document is not JSON");
			return nullptr;
		}
		return Root;
	}

	/** Reads Cooked with its engine.sound set to Sound (removed when null), through the client's reader. */
	bool ReadWithSound(const TSharedPtr<FJsonObject>& Cooked, const TSharedPtr<FJsonValue>& Sound, FMantleVehicleDesc& Out, FString& OutError)
	{
		const TSharedRef<FJsonObject> Doc = MakeShared<FJsonObject>();
		Doc->Values = Cooked->Values;
		const TSharedRef<FJsonObject> Engine = MakeShared<FJsonObject>();
		Engine->Values = Cooked->GetObjectField(TEXT("engine"))->Values;
		Engine->RemoveField(TEXT("sound"));
		if (Sound.IsValid())
		{
			Engine->SetField(TEXT("sound"), Sound);
		}
		Doc->SetObjectField(TEXT("engine"), Engine);
		FString Text;
		FJsonSerializer::Serialize(Doc, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text));
		const FTCHARToUTF8 Utf8(*Text);
		OutError.Reset();
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
	FString Error;
	const TSharedPtr<FJsonObject> Cooked = CookedCar(Error);
	if (!TestTrue(*FString::Printf(TEXT("the cooked car fixture reads (%s)"), *Error), Cooked.IsValid() && Cooked->HasTypedField<EJson::Object>(TEXT("engine"))))
	{
		return false;
	}
	FMantleVehicleDesc D;
	bool bRead = ReadWithSound(Cooked, MakeShared<FJsonValueString>(TEXT("v8")), D, Error);
	if (TestTrue(*FString::Printf(TEXT("the cooked car with engine.sound v8 reads (%s)"), *Error), bRead))
	{
		TestEqual(TEXT("its engine sound"), D.EngineSound, FName(TEXT("v8")));
	}
	bRead = ReadWithSound(Cooked, nullptr, D, Error);
	if (TestTrue(*FString::Printf(TEXT("the cooked car without one reads (%s)"), *Error), bRead))
	{
		TestTrue(TEXT("and has none: the type's default"), D.EngineSound.IsNone());
	}
	// An id this client has no sound for still reads (it plays the default); a non-string is refused at the key.
	bRead = ReadWithSound(Cooked, MakeShared<FJsonValueString>(TEXT("jet_fighter")), D, Error);
	TestTrue(*FString::Printf(TEXT("an id this client does not know reads (%s)"), *Error), bRead && D.EngineSound == FName(TEXT("jet_fighter")));
	bRead = ReadWithSound(Cooked, MakeShared<FJsonValueNumber>(8.0), D, Error);
	TestFalse(TEXT("a number is refused"), bRead);
	TestTrue(*FString::Printf(TEXT("... at /engine/sound (%s)"), *Error), Error.Contains(TEXT("/engine/sound")));

	// Which engines a vehicle may pick.
	TestTrue(TEXT("a wheeled vehicle on two wheels is a two-wheeler"), FMantleEngineSounds::IsTwoWheeler(true, 2));
	TestFalse(TEXT("a car is not"), FMantleEngineSounds::IsTwoWheeler(true, 4));
	TestFalse(TEXT("nor is anything not wheeled"), FMantleEngineSounds::IsTwoWheeler(false, 2));
	TArray<FName> Ids;
	FMantleEngineSounds::Options(/*bFlier*/ false, /*bTwoWheeler*/ false, /*bHandlebars*/ false, Ids);
	TestEqual(TEXT("a car or a boat: a car's engines"), Ids, FMantleEngineSounds::Car());
	FMantleEngineSounds::Options(false, true, true, Ids);
	TestEqual(TEXT("a motorcycle: a motorcycle's"), Ids, FMantleEngineSounds::Bike());
	FMantleEngineSounds::Options(false, false, true, Ids);
	TestEqual(TEXT("an ATV or a jet ski: both"), Ids.Num(), FMantleEngineSounds::Car().Num() + FMantleEngineSounds::Bike().Num());
	FMantleEngineSounds::Options(/*bFlier*/ true, false, false, Ids);
	TestEqual(TEXT("a plane, a helicopter or a hovercraft: an aircraft's"), Ids, FMantleEngineSounds::Aircraft());
	TestTrue(TEXT("an ATV takes inline3"), FMantleEngineSounds::Fits(TEXT("inline3"), false, false, true));
	TestTrue(TEXT("an ATV takes bike_inline4"), FMantleEngineSounds::Fits(TEXT("bike_inline4"), false, false, true));
	TestFalse(TEXT("a car does not take bike_vtwin"), FMantleEngineSounds::Fits(TEXT("bike_vtwin"), false, false, false));
	TestFalse(TEXT("a motorcycle does not take v8"), FMantleEngineSounds::Fits(TEXT("v8"), false, true, true));
	TestFalse(TEXT("an unknown id fits nothing"), FMantleEngineSounds::Fits(TEXT("jet_fighter"), false, false, true));
	TestTrue(TEXT("a plane takes aircraft_fighter"), FMantleEngineSounds::Fits(TEXT("aircraft_fighter"), true, false, false));
	TestTrue(TEXT("a helicopter takes helicopter"), FMantleEngineSounds::Fits(TEXT("helicopter"), true, false, false));
	TestFalse(TEXT("a plane does not take v8"), FMantleEngineSounds::Fits(TEXT("v8"), true, false, false));
	TestFalse(TEXT("... nor bike_vtwin, even on two wheels with handlebars"), FMantleEngineSounds::Fits(TEXT("bike_vtwin"), true, true, true));
	TestFalse(TEXT("a car does not take hovercraft"), FMantleEngineSounds::Fits(TEXT("hovercraft"), false, false, false));
	TestFalse(TEXT("an ATV does not take aircraft_airliner"), FMantleEngineSounds::Fits(TEXT("aircraft_airliner"), false, false, true));
	TestTrue(TEXT("a boat picks one"), FMantleEngineSounds::CanPick(true, false, false, false));
	TestFalse(TEXT("a pedalled vehicle picks none"), FMantleEngineSounds::CanPick(true, false, true, false));
	TestFalse(TEXT("an electric motor picks none"), FMantleEngineSounds::CanPick(true, false, false, true));
	TestTrue(TEXT("a plane picks one"), FMantleEngineSounds::CanPick(false, true, false, false));
	TestFalse(TEXT("a trailer picks none"), FMantleEngineSounds::CanPick(false, false, false, false));

	// The lists are the schema's engine.sound enum, car, motorcycle, then aircraft (only where the game's schema is beside the project; the plugin has none).
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
			Ours.Append(FMantleEngineSounds::Aircraft());
			TestEqual(TEXT("the schema's ids are ours"), SchemaIds, Ours);
		}
	}
	return true;
}

#endif
