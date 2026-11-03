// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleAsset.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

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
	return true;
}

#endif
