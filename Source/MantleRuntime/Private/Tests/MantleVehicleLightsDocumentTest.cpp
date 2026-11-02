#include "MantleVehicleAsset.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

// The vehicle document's lights block as the client reads it: the cook's own fixture (backend TestVehicleLightsFixtureMatchesWhatWeWriteToday)
// with a head and a rear section and two beams.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleLightsDocumentTest,
	"Mantle.Vehicle.Lights.Document",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleLightsDocumentTest::RunTest(const FString&)
{
	const FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("../schema/fixtures/vehicle-lights-v1.mvh")));
	TArray<uint8> Bytes;
	if (!TestTrue(*FString::Printf(TEXT("the lights fixture exists (%s)"), *Path), FFileHelper::LoadFileToArray(Bytes, *Path)))
	{
		return false;
	}
	FMantleVehicleAsset Asset;
	FString Error;
	if (!TestTrue(*FString::Printf(TEXT("the reader takes a vehicle with lights (%s)"), *Error), FMantleVehicleAssetReader::Read(Bytes, Asset, Error)))
	{
		return false;
	}
	const FMantleVehicleDesc& D = Asset.Desc;
	TestTrue(TEXT("the document has lights"), D.bLights);
	TestEqual(TEXT("one head section"), D.LightHead.Num(), 1);
	TestEqual(TEXT("the head section"), D.LightHead.IsValidIndex(0) ? D.LightHead[0] : FString(), FString(TEXT("M_Body")));
	TestEqual(TEXT("one rear section"), D.LightRear.Num(), 1);
	TestEqual(TEXT("no reverse sections"), D.LightReverse.Num(), 0);
	TestEqual(TEXT("two beams"), D.LightBeams.Num(), 2);
	if (D.LightBeams.Num() == 2)
	{
		TestEqual(TEXT("the second beam's side"), D.LightBeams[1].Position.Y, 40.0);
		TestTrue(TEXT("a beam direction has unit length"), FMath::IsNearlyEqual(D.LightBeams[0].Direction.Size(), 1.0, 1e-6));
		TestTrue(TEXT("the beam points forward and a little down"), D.LightBeams[0].Direction.X > 0.99 && D.LightBeams[0].Direction.Z < 0.0);
	}
	return true;
}

#endif
