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

// A vehicle's liveries can be named (the document's liveryNames, one for each livery in the same order). The cook's own fixture (backend
// TestVehicleLiveryNamesFixtureMatchesWhatWeWriteToday) is the livery car with its first livery named "Racing stripe" and its second left without a name.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleLiveryNamesAssetTest,
	"Mantle.Vehicle.Asset.LiveryNames",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleLiveryNamesAssetTest::RunTest(const FString&)
{
	const FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("../schema/fixtures/vehicle-livery-names-v1.mvh")));
	TArray<uint8> Bytes;
	if (!TestTrue(*FString::Printf(TEXT("the livery names fixture exists (%s)"), *Path), FFileHelper::LoadFileToArray(Bytes, *Path)))
	{
		return false;
	}
	FMantleVehicleAsset Asset;
	FString Error;
	if (!TestTrue(*FString::Printf(TEXT("the reader takes a vehicle with livery names (%s)"), *Error), FMantleVehicleAssetReader::Read(Bytes, Asset, Error)))
	{
		return false;
	}
	const FMantleVehicleDesc& D = Asset.Desc;
	TestEqual(TEXT("two liveries"), D.Liveries.Num(), 2);
	if (TestEqual(TEXT("with a name each"), D.LiveryNames.Num(), 2))
	{
		TestEqual(TEXT("the first is named"), D.LiveryNames[0], FString(TEXT("Racing stripe")));
		TestEqual(TEXT("the second has no name"), D.LiveryNames[1], FString());
	}

	// The earlier livery fixture names nothing and still reads.
	const FString OldPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("../schema/fixtures/vehicle-livery-v1.mvh")));
	TArray<uint8> Old;
	FMantleVehicleAsset OldCar;
	if (TestTrue(TEXT("the unnamed livery fixture exists"), FFileHelper::LoadFileToArray(Old, *OldPath)) && TestTrue(TEXT("and reads"), FMantleVehicleAssetReader::Read(Old, OldCar, Error)))
	{
		TestEqual(TEXT("two liveries"), OldCar.Desc.Liveries.Num(), 2);
		TestEqual(TEXT("and no names"), OldCar.Desc.LiveryNames.Num(), 0);
	}

	// A name's length is counted in code points like the cook and the schema do, not UTF-16 units: 40 emoji (80 units) are accepted, 41 refused.
	const uint32 DocumentBytes = *reinterpret_cast<const uint32*>(Bytes.GetData() + 8);
	FString Json;
	FFileHelper::BufferToString(Json, Bytes.GetData() + 12, static_cast<int32>(DocumentBytes));
	TSharedPtr<FJsonObject> Root;
	if (!TestTrue(TEXT("the fixture's document parses as JSON"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) && Root.IsValid()))
	{
		return false;
	}
	const auto ReadWithName = [&](const FString& Name)
	{
		const TSharedRef<FJsonObject> Doc = MakeShared<FJsonObject>();
		Doc->Values = Root->Values;
		Doc->SetArrayField(TEXT("liveryNames"), { MakeShared<FJsonValueString>(Name), MakeShared<FJsonValueString>(FString()) });
		FString Out;
		FJsonSerializer::Serialize(Doc, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out));
		const FTCHARToUTF8 Utf8(*Out);
		FMantleVehicleDesc Desc;
		FString Why;
		return FMantleVehicleAssetReader::ParseDocument(TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()), Desc, Why);
	};
	FString Emoji;
	for (int32 Count = 0; Count < 40; ++Count)
	{
		Emoji += TEXT("😀");
	}
	TestTrue(TEXT("a name of 40 emoji is accepted"), ReadWithName(Emoji));
	TestFalse(TEXT("a name of 41 emoji is refused"), ReadWithName(Emoji + TEXT("😀")));
	return true;
}

#endif
