// Copyright Joshua Eger. All Rights Reserved.
#include "Dom/JsonObject.h"
#include "MantleVehicleAsset.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#if WITH_DEV_AUTOMATION_TESTS

// A vehicle's liveries can light up with one of its lights (the document's liveryGlow, one entry for each livery in the same order, null for one
// that does not). The cook's own fixture (backend TestVehicleLiveryGlowCookAndRoundTrip) is the livery car whose second livery glows with the headlights.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleLiveryGlowAssetTest,
	"Mantle.Vehicle.Asset.LiveryGlow",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleLiveryGlowAssetTest::RunTest(const FString&)
{
	const FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("../schema/fixtures/vehicle-livery-glow-v1.mvh")));
	TArray<uint8> Bytes;
	if (!TestTrue(*FString::Printf(TEXT("the livery glow fixture exists (%s)"), *Path), FFileHelper::LoadFileToArray(Bytes, *Path)))
	{
		return false;
	}
	FMantleVehicleAsset Asset;
	FString Error;
	if (!TestTrue(*FString::Printf(TEXT("the reader takes a vehicle with a glowing livery (%s)"), *Error), FMantleVehicleAssetReader::Read(Bytes, Asset, Error)))
	{
		return false;
	}
	const FMantleVehicleDesc& D = Asset.Desc;
	TestEqual(TEXT("two liveries"), D.Liveries.Num(), 2);
	if (TestEqual(TEXT("with a glow entry each"), D.LiveryGlows.Num(), 2))
	{
		TestTrue(TEXT("the first does not glow"), D.LiveryGlows[0].Light.IsEmpty());
		TestEqual(TEXT("the second glows with the headlights"), D.LiveryGlows[1].Light, FString(TEXT("head")));
		TestEqual(TEXT("at the cook's default strength"), D.LiveryGlows[1].Strength, 1.0);
	}

	// The earlier livery fixture has no glow and still reads.
	const FString OldPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("../schema/fixtures/vehicle-livery-v1.mvh")));
	TArray<uint8> Old;
	FMantleVehicleAsset OldCar;
	if (TestTrue(TEXT("the plain livery fixture exists"), FFileHelper::LoadFileToArray(Old, *OldPath)) && TestTrue(TEXT("and reads"), FMantleVehicleAssetReader::Read(Old, OldCar, Error)))
	{
		TestEqual(TEXT("and no livery glows"), OldCar.Desc.LiveryGlows.Num(), 0);
	}

	// The reader holds the entries to the cook's rules: one per livery, a light the lights block has, a strength in its range.
	const uint32 DocumentBytes = *reinterpret_cast<const uint32*>(Bytes.GetData() + 8);
	FString Json;
	FFileHelper::BufferToString(Json, Bytes.GetData() + 12, static_cast<int32>(DocumentBytes));
	TSharedPtr<FJsonObject> Root;
	if (!TestTrue(TEXT("the fixture's document parses as JSON"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) && Root.IsValid()))
	{
		return false;
	}
	const auto ReadWith = [&](const TArray<TSharedPtr<FJsonValue>>& Glows)
	{
		const TSharedRef<FJsonObject> Doc = MakeShared<FJsonObject>();
		Doc->Values = Root->Values;
		Doc->SetArrayField(TEXT("liveryGlow"), Glows);
		FString Out;
		FJsonSerializer::Serialize(Doc, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out));
		const FTCHARToUTF8 Utf8(*Out);
		FMantleVehicleDesc Desc;
		FString Why;
		return FMantleVehicleAssetReader::ParseDocument(TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()), Desc, Why);
	};
	const auto Entry = [](const TCHAR* Light, const double Strength)
	{
		const TSharedRef<FJsonObject> Glow = MakeShared<FJsonObject>();
		Glow->SetStringField(TEXT("light"), Light);
		Glow->SetNumberField(TEXT("strength"), Strength);
		return MakeShared<FJsonValueObject>(Glow);
	};
	const TSharedPtr<FJsonValue> None = MakeShared<FJsonValueNull>();
	TestTrue(TEXT("the rear lights at strength 3 are taken"), ReadWith({ None, Entry(TEXT("rear"), 3.0) }));
	TestFalse(TEXT("one entry for two liveries is refused"), ReadWith({ Entry(TEXT("head"), 1.0) }));
	TestFalse(TEXT("a light Mantle does not have is refused"), ReadWith({ None, Entry(TEXT("headlights"), 1.0) }));
	TestFalse(TEXT("a strength past 100 is refused"), ReadWith({ None, Entry(TEXT("head"), 101.0) }));
	return true;
}

#endif
