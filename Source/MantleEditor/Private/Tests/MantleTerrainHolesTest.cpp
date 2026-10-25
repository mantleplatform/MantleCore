// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCoverage.h"
#include "MantleLandscapeMaterialReader.h"
#include "MantleTerrainExport.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// A landscape with holes painted in its visibility layer, and one with nothing painted, publish with a warning and a coverage line instead
// of being refused. ChooseLayers takes no level (a null world means no Place Settings), so a source built in code is enough.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleTerrainHolesTestPrivate
{
	/** A 4 x 4 flat source with the layers painted over all of it (one array of 255 each). */
	FMantleTerrainSource MakeFlatSource(const TArray<FName>& Layers)
	{
		FMantleTerrainSource Source;
		Source.Width = 4;
		Source.Height = 4;
		Source.Heights.Init(32768, 16);
		for (const FName& Layer : Layers)
		{
			Source.LayerNames.Add(Layer);
			Source.LayerWeights.Emplace();
			Source.LayerWeights.Last().Init(255, 16);
		}
		return Source;
	}

	FMantleLandscapeLayerLook MakeFlatLook(const TCHAR* Layer, const FLinearColor Colour)
	{
		FMantleLandscapeLayerLook Look;
		Look.Layer = Layer;
		Look.bHasFlatColour = true;
		Look.FlatColour = Colour;
		Look.bHasAverage = true;
		Look.Average = Colour;
		return Look;
	}

	int32 CountIssues(const TArray<FMantleValidationIssue>& Issues, const EMantleIssueSeverity Severity, const TCHAR* Word)
	{
		int32 Count = 0;
		for (const FMantleValidationIssue& Issue : Issues)
		{
			Count += Issue.Severity == Severity && Issue.Problem.Contains(Word) ? 1 : 0;
		}
		return Count;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainHolesAreFilledTest,
	"Mantle.Editor.Terrain.HolesAreFilled",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainHolesAreFilledTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainHolesTestPrivate;

	FMantleTerrainSource Source = MakeFlatSource({ TEXT("Grass") });
	Source.bMaterialRead = true;
	Source.MaterialRead.Layers.Add(MakeFlatLook(TEXT("Grass"), FLinearColor(0.1f, 0.5f, 0.1f)));
	Source.MaterialRead.bFoundLayers = true;
	Source.bHasHoles = true;
	Source.HoleSamples = 3;

	TArray<FMantleTerrainLayerChoice> Layers;
	TArray<FMantleValidationIssue> Issues;
	FMantleCoverage Coverage;
	TestTrue(TEXT("a landscape with holes is not refused"), FMantleTerrainExport::ChooseLayers(nullptr, Source, TEXT("Landscape"), Layers, Issues, &Coverage));
	TestEqual(TEXT("its layer is chosen as it is without holes"), Layers.Num(), 1);
	TestEqual(TEXT("with no error"), CountIssues(Issues, EMantleIssueSeverity::Error, TEXT("")), 0);
	TestEqual(TEXT("and a warning that the holes are filled"), CountIssues(Issues, EMantleIssueSeverity::Warning, TEXT("holes")), 1);
	TestEqual(TEXT("which says how many samples (3 of 16)"), CountIssues(Issues, EMantleIssueSeverity::Warning, TEXT("3 of its 16 samples")), 1);
	TestEqual(TEXT("and an approximated terrain line in the coverage report"), static_cast<int64>(Coverage.Count(MantleCoverageCategory::Terrain, EMantleCoverageOutcome::Approximated)), static_cast<int64>(1));

	// The published data is the landscape's own heights and weights: nothing is rewritten to fill the hole.
	const FMantleHeightfield Field = FMantleTerrainExport::MakeHeightfield(Source);
	TestEqual(TEXT("the heightfield keeps every sample"), Field.Heights.Num(), 16);
	TestTrue(TEXT("as the landscape has them"), Field.Heights == Source.Heights);

	// Without holes there is neither the warning nor the line.
	Source.bHasHoles = false;
	Source.HoleSamples = 0;
	Layers.Reset();
	Issues.Reset();
	FMantleCoverage Clean;
	TestTrue(TEXT("a landscape without holes is chosen"), FMantleTerrainExport::ChooseLayers(nullptr, Source, TEXT("Landscape"), Layers, Issues, &Clean));
	TestEqual(TEXT("with no hole warning"), CountIssues(Issues, EMantleIssueSeverity::Warning, TEXT("holes")), 0);
	TestEqual(TEXT("and no terrain line"), static_cast<int64>(Clean.Count(MantleCoverageCategory::Terrain, EMantleCoverageOutcome::Approximated)), static_cast<int64>(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainUnpaintedGetsDefaultLayerTest,
	"Mantle.Editor.Terrain.UnpaintedGetsDefaultLayer",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainUnpaintedGetsDefaultLayerTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainHolesTestPrivate;

	// Nothing painted, no Place Settings entry, and the landscape's material could not be read: plain grey, with a warning.
	FMantleTerrainSource Bare = MakeFlatSource({});
	TArray<FMantleTerrainLayerChoice> Layers;
	TArray<FMantleValidationIssue> Issues;
	FMantleCoverage Coverage;
	TestTrue(TEXT("a landscape with nothing painted is not refused"), FMantleTerrainExport::ChooseLayers(nullptr, Bare, TEXT("Landscape"), Layers, Issues, &Coverage));
	if (TestEqual(TEXT("it gets one layer"), Layers.Num(), 1))
	{
		TestNotNull(TEXT("with a material"), Layers[0].Material);
		TestFalse(TEXT("which is not read from a landscape material"), Layers[0].bDerived);
	}
	TestEqual(TEXT("no error"), CountIssues(Issues, EMantleIssueSeverity::Error, TEXT("")), 0);
	TestEqual(TEXT("a warning that says it is drawn plain grey"), CountIssues(Issues, EMantleIssueSeverity::Warning, TEXT("plain grey")), 1);
	TestEqual(TEXT("and an approximated layer line"), static_cast<int64>(Coverage.Count(MantleCoverageCategory::TerrainLayers, EMantleCoverageOutcome::Approximated)), static_cast<int64>(1));

	// With the landscape's material read, the first look it states is the ground.
	FMantleTerrainSource Read = MakeFlatSource({});
	Read.bMaterialRead = true;
	Read.MaterialRead.Layers.Add(MakeFlatLook(TEXT("Moss"), FLinearColor(0.1f, 0.4f, 0.1f)));
	Read.MaterialRead.Layers.Add(MakeFlatLook(TEXT("Rock"), FLinearColor(0.5f, 0.5f, 0.5f)));
	Read.MaterialRead.bFoundLayers = true;
	Layers.Reset();
	Issues.Reset();
	TestTrue(TEXT("a landscape with nothing painted and a readable material is not refused"), FMantleTerrainExport::ChooseLayers(nullptr, Read, TEXT("Landscape"), Layers, Issues));
	if (TestEqual(TEXT("one layer"), Layers.Num(), 1))
	{
		TestTrue(TEXT("the first look the material states"), Layers[0].Layer == FName(TEXT("Moss")));
		TestTrue(TEXT("read from the landscape's material"), Layers[0].bDerived);
		TestNotNull(TEXT("with a material"), Layers[0].Material);
	}
	TestEqual(TEXT("still only a warning"), CountIssues(Issues, EMantleIssueSeverity::Error, TEXT("")), 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
