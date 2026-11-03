// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleAsset.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

// A car's paint can carry a pearlescent color and layers beyond the body paint (the document's paint.pearl and paint.layers, and a part's layerSlots). The cook's
// own fixture (backend TestVehiclePaintLayersFixtureMatchesWhatWeWriteToday) is the parts car with a secondary layer on a body section, a wheels layer on the hub
// cap's first slot and a pearl.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehiclePaintLayersAssetTest,
	"Mantle.Vehicle.Asset.PaintLayers",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehiclePaintLayersAssetTest::RunTest(const FString&)
{
	const FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("../schema/fixtures/vehicle-paint-layers-v1.mvh")));
	TArray<uint8> Bytes;
	if (!TestTrue(*FString::Printf(TEXT("the paint layers fixture exists (%s)"), *Path), FFileHelper::LoadFileToArray(Bytes, *Path)))
	{
		return false;
	}
	FMantleVehicleAsset Asset;
	FString Error;
	if (!TestTrue(*FString::Printf(TEXT("the reader takes a vehicle with paint layers (%s)"), *Error), FMantleVehicleAssetReader::Read(Bytes, Asset, Error)))
	{
		return false;
	}
	const FMantleVehicleDesc& D = Asset.Desc;
	TestTrue(TEXT("the car has paint"), D.HasPaint());
	TestTrue(TEXT("with a pearl"), D.bPaintPearl);
	TestTrue(TEXT("whose color is the document's"), D.PaintPearlColor.Equals(FLinearColor(0.2f, 0.5f, 1.f), 0.001f));
	TestTrue(TEXT("and whose strength is the default 0.6"), FMath::IsNearlyEqual(D.PaintPearlStrength, 0.6, 1.e-6));
	if (!TestEqual(TEXT("two other layers"), D.PaintLayers.Num(), 2))
	{
		return false;
	}
	const FMantleVehiclePaintLayerDesc& Secondary = D.PaintLayers[0];
	TestEqual(TEXT("the first is the secondary"), Secondary.Layer, FString(TEXT("secondary")));
	TestTrue(TEXT("on M_Body"), Secondary.Sections.Num() == 1 && Secondary.Sections[0] == TEXT("M_Body"));
	TestTrue(TEXT("in the document's color"), Secondary.Color.Equals(FLinearColor(0.9f, 0.9f, 0.2f), 0.001f));
	TestEqual(TEXT("and finish"), Secondary.Finish, FString(TEXT("metallic")));
	const FMantleVehiclePaintLayerDesc& Wheels = D.PaintLayers[1];
	TestEqual(TEXT("the second is the wheels'"), Wheels.Layer, FString(TEXT("wheels")));
	TestEqual(TEXT("with no body section"), Wheels.Sections.Num(), 0);
	TestTrue(TEXT("white until a script says otherwise"), Wheels.Color.Equals(FLinearColor::White, 0.001f));
	TestEqual(TEXT("in chrome"), Wheels.Finish, FString(TEXT("chrome")));
	if (!TestEqual(TEXT("the parts"), D.Parts.Num(), 2))
	{
		return false;
	}
	TestEqual(TEXT("the hub cap's first slot wears one layer"), D.Parts[1].LayerSlots.Num(), 1);
	if (D.Parts[1].LayerSlots.Num() == 1)
	{
		TestEqual(TEXT("the wheels'"), D.Parts[1].LayerSlots[0].Layer, FString(TEXT("wheels")));
		TestTrue(TEXT("on slot 0"), D.Parts[1].LayerSlots[0].Slots.Num() == 1 && D.Parts[1].LayerSlots[0].Slots[0] == 0);
	}
	TestTrue(TEXT("a part with layer slots paints"), D.HasPartPaint());

	// A version of the file with no pearl or layers still reads: the livery car carries neither.
	const FString OldPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("../schema/fixtures/vehicle-livery-v1.mvh")));
	TArray<uint8> Old;
	FMantleVehicleAsset OldCar;
	if (TestTrue(TEXT("the livery fixture exists"), FFileHelper::LoadFileToArray(Old, *OldPath)) && TestTrue(TEXT("and reads"), FMantleVehicleAssetReader::Read(Old, OldCar, Error)))
	{
		TestFalse(TEXT("with no pearl"), OldCar.Desc.bPaintPearl);
		TestEqual(TEXT("and no other layer"), OldCar.Desc.PaintLayers.Num(), 0);
	}

	TestTrue(TEXT("secondary is a layer"), FMantleVehiclePaintNames::IsLayer(TEXT("secondary")));
	TestFalse(TEXT("primary is not one of the other layers"), FMantleVehiclePaintNames::IsLayer(TEXT("primary")));
	TestFalse(TEXT("roof is not a layer"), FMantleVehiclePaintNames::IsLayer(TEXT("roof")));
	TestTrue(TEXT("chrome is a finish"), FMantleVehiclePaintNames::IsFinish(TEXT("chrome")));
	TestFalse(TEXT("glitter is not a finish"), FMantleVehiclePaintNames::IsFinish(TEXT("glitter")));
	return true;
}

#endif
