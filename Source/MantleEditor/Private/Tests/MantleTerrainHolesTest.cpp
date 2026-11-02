// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCoverage.h"
#include "MantleHeightfieldReader.h"
#include "MantleLandscapeMaterialReader.h"
#include "MantlePlaceAssets.h"
#include "MantleTerrainExport.h"

#include "Materials/Material.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// A landscape with holes (painted in its visibility layer, or where no component covers its extent) publishes them as holes, in the Terrain's
// hole mask; one with nothing painted publishes with a warning and a coverage line instead of being refused. ChooseLayers takes no level (a null world means no Place Settings), so a source built in code is enough.

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

	FMantleLandscapeLayerLook MakeFlatLook(const TCHAR* Layer, const FLinearColor Color)
	{
		FMantleLandscapeLayerLook Look;
		Look.Layer = Layer;
		Look.bHasFlatColor = true;
		Look.FlatColor = Color;
		Look.bHasAverage = true;
		Look.Average = Color;
		return Look;
	}

	/** A material standing in for a landscape's own: what ChooseLayers bakes whole when its layers cannot be read. */
	UMaterialInterface* NewLandscapeMaterial()
	{
		return NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
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
	FMantleTerrainHolesAreKeptTest,
	"Mantle.Editor.Terrain.HolesAreKept",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainHolesAreKeptTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainHolesTestPrivate;

	FMantleTerrainSource Source = MakeFlatSource({ TEXT("Grass") });
	Source.bMaterialRead = true;
	Source.MaterialRead.Layers.Add(MakeFlatLook(TEXT("Grass"), FLinearColor(0.1f, 0.5f, 0.1f)));
	Source.MaterialRead.bFoundLayers = true;
	Source.bHasHoles = true;
	Source.HoleSamples = 3;
	Source.Holes.SetNumZeroed(16);
	Source.Holes[5] = Source.Holes[6] = Source.Holes[9] = 1;

	TArray<FMantleTerrainLayerChoice> Layers;
	TArray<FMantleValidationIssue> Issues;
	FMantleCoverage Coverage;
	TestTrue(TEXT("a landscape with holes is not refused"), FMantleTerrainExport::ChooseLayers(nullptr, Source, TEXT("Landscape"), Layers, Issues, &Coverage));
	TestEqual(TEXT("its layer is chosen as it is without holes"), Layers.Num(), 1);
	TestEqual(TEXT("with no error"), CountIssues(Issues, EMantleIssueSeverity::Error, TEXT("")), 0);
	TestEqual(TEXT("and no warning: the holes are kept"), CountIssues(Issues, EMantleIssueSeverity::Warning, TEXT("holes")), 0);
	TestEqual(TEXT("the coverage report notes them"), static_cast<int64>(Coverage.Count(MantleCoverageCategory::Terrain, EMantleCoverageOutcome::Info)), static_cast<int64>(1));
	TestEqual(TEXT("and approximates nothing"), static_cast<int64>(Coverage.Count(MantleCoverageCategory::Terrain, EMantleCoverageOutcome::Approximated)), static_cast<int64>(0));

	// The mask: red 0 at the holes, 255 elsewhere, one texel a sample, row 0 the first row of samples.
	TestTrue(TEXT("it has holes"), FMantleTerrainExport::HasHoles(Source));
	const TArray<FColor> Mask = FMantleTerrainExport::MakeHoleMask(Source, FIntPoint(4, 4));
	if (TestEqual(TEXT("a texel a sample"), Mask.Num(), 16))
	{
		for (int32 Index = 0; Index < 16; ++Index)
		{
			TestEqual(TEXT("red marks the holes"), static_cast<int32>(Mask[Index].R), Source.Holes[Index] ? 0 : 255);
		}
	}
	// A resampled grid takes the nearest sample's: corner to corner, so (1, 1) of a 2 x 2 is sample (3, 3), ground.
	const TArray<FColor> Small = FMantleTerrainExport::MakeHoleMask(Source, FIntPoint(2, 2));
	TestTrue(TEXT("a smaller mask maps by nearest sample"), Small.Num() == 4 && Small[0].R == 255 && Small[3].R == 255);
	FMantleHeightfield Field;
	Field.Width = 4;
	Field.Height = 4;
	FIntPoint Size;
	const TArray<uint8> Png = FMantleTerrainExport::EncodeHoleMask(Source, Field, 2048, Size);
	TestTrue(TEXT("the mask is a PNG at the heightfield's size"), !Png.IsEmpty() && Size == FIntPoint(4, 4));
	Field.Width = 2049;
	FMantleTerrainExport::EncodeHoleMask(Source, Field, 2048, Size);
	TestTrue(TEXT("capped to the texture limit"), Size == FIntPoint(2048, 4));

	// Without holes there is no mask and no line.
	Source.bHasHoles = false;
	Source.HoleSamples = 0;
	Source.Holes.Reset();
	Layers.Reset();
	Issues.Reset();
	FMantleCoverage Clean;
	TestTrue(TEXT("a landscape without holes is chosen"), FMantleTerrainExport::ChooseLayers(nullptr, Source, TEXT("Landscape"), Layers, Issues, &Clean));
	TestFalse(TEXT("and has no holes"), FMantleTerrainExport::HasHoles(Source));
	TestEqual(TEXT("so no mask"), FMantleTerrainExport::MakeHoleMask(Source, FIntPoint(4, 4)).Num(), 0);
	TestEqual(TEXT("and no terrain line"), static_cast<int64>(Clean.Count(MantleCoverageCategory::Terrain, EMantleCoverageOutcome::Info)), static_cast<int64>(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainMissingComponentsTest,
	"Mantle.Editor.Terrain.MissingComponentsAreHoles",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainMissingComponentsTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainHolesTestPrivate;

	// 4 x 3 samples: the right half of the middle row and all of the last row have no component (GetHeightDataFast left them 0).
	FMantleTerrainSource Source;
	Source.Width = 4;
	Source.Height = 3;
	Source.Heights = { 100, 200, 300, 400,   500, 600, 0, 0,   0, 0, 0, 0 };
	const TArray<uint8> Covered = { 1, 1, 1, 1,   1, 1, 0, 0,   0, 0, 0, 0 };
	FMantleTerrainExport::ApplyCoverage(Source, Covered);
	TestEqual(TEXT("six samples have no ground"), Source.MissingSamples, 6);
	TestTrue(TEXT("they are holes"), Source.Holes == TArray<uint8>{ 0, 0, 0, 0,   0, 0, 1, 1,   1, 1, 1, 1 });
	TestTrue(TEXT("so the Terrain has a mask"), FMantleTerrainExport::HasHoles(Source));
	TestTrue(TEXT("covered heights are kept"), Source.Heights[0] == 100 && Source.Heights[5] == 600);
	TestTrue(TEXT("a missing sample takes the nearest in its row"), Source.Heights[6] == 600 && Source.Heights[7] == 600);
	TestTrue(TEXT("a row with nothing covered takes the nearest row"), Source.Heights[8] == 500 && Source.Heights[11] == 600);

	// Everything covered: nothing changes.
	FMantleTerrainSource Whole = MakeFlatSource({ TEXT("Grass") });
	TArray<uint8> All;
	All.Init(1, 16);
	FMantleTerrainExport::ApplyCoverage(Whole, All);
	TestEqual(TEXT("a whole landscape has none missing"), Whole.MissingSamples, 0);
	TestFalse(TEXT("and no holes"), FMantleTerrainExport::HasHoles(Whole));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainUnpaintedGetsDefaultLayerTest,
	"Mantle.Editor.Terrain.UnpaintedGetsDefaultLayer",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainUnpaintedGetsDefaultLayerTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainHolesTestPrivate;

	// Nothing painted, no Place Settings entry, and the landscape's material could not be read: plain gray, with a warning.
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
	TestEqual(TEXT("a warning that says it is drawn plain gray"), CountIssues(Issues, EMantleIssueSeverity::Warning, TEXT("plain gray")), 1);
	TestEqual(TEXT("and an approximated layer line"), static_cast<int64>(Coverage.Count(MantleCoverageCategory::TerrainLayers, EMantleCoverageOutcome::Approximated)), static_cast<int64>(1));
	if (Layers.Num() == 1)
	{
		TestFalse(TEXT("a landscape with no material is not a landscape material bake"), Layers[0].bLandscapeMaterialBake);
	}

	// Nothing painted and a material that could not be read (an ordinary UV-mapped one, DowntownWestModularPack's): that material baked
	// whole, over the 10 quads its texture tiled by 0.1 takes to repeat, tiled every 10 quads of 100 cm.
	FMantleTerrainSource Unread = MakeFlatSource({});
	Unread.LandscapeMaterial = NewLandscapeMaterial();
	Unread.LandscapeMaterialUvRepeat = 10.0000001;
	Unread.SpacingX = 100.0;
	Layers.Reset();
	Issues.Reset();
	FMantleCoverage UnreadCoverage;
	TestTrue(TEXT("a landscape with nothing painted and an unreadable material is not refused"), FMantleTerrainExport::ChooseLayers(nullptr, Unread, TEXT("Landscape"), Layers, Issues, &UnreadCoverage));
	if (TestEqual(TEXT("one layer"), Layers.Num(), 1))
	{
		TestTrue(TEXT("drawn with the landscape's own material"), Layers[0].Material == Unread.LandscapeMaterial);
		TestTrue(TEXT("baked whole"), Layers[0].bLandscapeMaterialBake);
		TestEqual(TEXT("over 10 quads"), Layers[0].BakeUvRange, 10);
		TestEqual(TEXT("tiled every 10 quads"), Layers[0].TilingCm, 1000.0, 1.0e-6);
	}
	TestEqual(TEXT("no error"), CountIssues(Issues, EMantleIssueSeverity::Error, TEXT("")), 0);
	TestEqual(TEXT("the warning says it is baked from the landscape's material"), CountIssues(Issues, EMantleIssueSeverity::Warning, TEXT("baked whole")), 1);
	TestEqual(TEXT("and not that it is plain gray"), CountIssues(Issues, EMantleIssueSeverity::Warning, TEXT("plain gray")), 0);
	TestEqual(TEXT("with an approximated layer line"), static_cast<int64>(UnreadCoverage.Count(MantleCoverageCategory::TerrainLayers, EMantleCoverageOutcome::Approximated)), static_cast<int64>(1));

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

// The plain gray ground an unpainted landscape gets is not read from its material (bDerived false), yet Export names it: Gather must
// upload it, or Publish finds it "exported but not uploaded" until it gives up with "the level kept changing" (HellCage, ValleyofGuldur).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainPlainGrayLayerIsGatheredTest,
	"Mantle.Editor.Terrain.PlainGrayLayerIsGathered",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainPlainGrayLayerIsGatheredTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainHolesTestPrivate;

	FMantleTerrainSource Bare = MakeFlatSource({});
	TArray<FMantleTerrainLayerChoice> Layers;
	TArray<FMantleValidationIssue> Issues;
	TestTrue(TEXT("an unpainted landscape is chosen"), FMantleTerrainExport::ChooseLayers(nullptr, Bare, TEXT("Landscape"), Layers, Issues));
	if (!TestEqual(TEXT("one layer"), Layers.Num(), 1) || !TestNotNull(TEXT("with a material"), Layers[0].Material))
	{
		return false;
	}
	TestFalse(TEXT("which is not derived from a landscape material"), Layers[0].bDerived);

	FMantlePlaceDependencies Dependencies;
	Dependencies.AddTerrainLayerMaterials(Layers, TEXT("Landscape"));
	const FString Key = FMantlePlaceAssets::MaterialKey(Layers[0].Material);
	TestTrue(TEXT("the gray layer's material is gathered, as Export names it"),
		Dependencies.Materials.ContainsByPredicate([&Key](const FMantleGatheredMaterial& Gathered) { return Gathered.Key == Key; }));
	return true;
}

// A painted layer whose landscape material could not be read (MedievalVillage: no Landscape Layer Blend feeding Base Color) is drawn with that
// material baked whole, with a warning, instead of stopping the publish, and Gather uploads the material baked over the same UV range. With
// no landscape material it is plain gray, as before.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainUnreadableLayersBakeLandscapeMaterialTest,
	"Mantle.Editor.Terrain.UnreadableLayersBakeLandscapeMaterial",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainUnreadableLayersBakeLandscapeMaterialTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainHolesTestPrivate;

	FMantleTerrainSource Source = MakeFlatSource({ FName(TEXT("Grass")), FName(TEXT("Mud")) });
	Source.bMaterialRead = false;
	Source.MaterialProblem = TEXT("Its landscape material 'MI_Landscape' has no Landscape Layer Blend feeding its Base Color.");
	Source.LandscapeMaterial = NewLandscapeMaterial();
	Source.SpacingX = 50.0;
	// The material says nothing of its tiling: the most quads a bake may cover.
	Source.LandscapeMaterialUvRepeat = 0.0;
	TArray<FMantleTerrainLayerChoice> Layers;
	TArray<FMantleValidationIssue> Issues;
	FMantleCoverage Coverage;
	TestTrue(TEXT("painted layers with an unreadable material are not refused"), FMantleTerrainExport::ChooseLayers(nullptr, Source, TEXT("Landscape"), Layers, Issues, &Coverage));
	TestEqual(TEXT("no error"), CountIssues(Issues, EMantleIssueSeverity::Error, TEXT("")), 0);
	TestEqual(TEXT("a warning for each layer that it is baked from the landscape's material"), CountIssues(Issues, EMantleIssueSeverity::Warning, TEXT("baked whole")), 2);
	TestEqual(TEXT("none says plain gray"), CountIssues(Issues, EMantleIssueSeverity::Warning, TEXT("plain gray")), 0);
	TestEqual(TEXT("and an approximated layer line for each"), static_cast<int64>(Coverage.Count(MantleCoverageCategory::TerrainLayers, EMantleCoverageOutcome::Approximated)), static_cast<int64>(2));
	if (!TestEqual(TEXT("both layers are kept"), Layers.Num(), 2))
	{
		return false;
	}
	const int32 Range = FMantleTerrainExport::MaxLandscapeBakeUvRange;
	for (const FMantleTerrainLayerChoice& Layer : Layers)
	{
		TestTrue(TEXT("each drawn with the landscape's material"), Layer.Material == Source.LandscapeMaterial);
		TestTrue(TEXT("baked whole"), Layer.bLandscapeMaterialBake);
		TestEqual(TEXT("over the most quads a bake covers"), Layer.BakeUvRange, Range);
		TestEqual(TEXT("tiled every that many quads"), Layer.TilingCm, Range * 50.0, 1.0e-6);
	}

	// The landscape's material is gathered once, to be baked over that range, as Export names it.
	FMantlePlaceDependencies Dependencies;
	Dependencies.AddTerrainLayerMaterials(Layers, TEXT("Landscape"));
	const FString Key = FMantlePlaceAssets::MaterialKey(Source.LandscapeMaterial);
	const FMantleGatheredMaterial* Gathered = Dependencies.Materials.FindByPredicate([&Key](const FMantleGatheredMaterial& Entry) { return Entry.Key == Key; });
	if (TestNotNull(TEXT("the landscape's material is gathered"), Gathered))
	{
		TestEqual(TEXT("once"), Dependencies.Materials.Num(), 1);
		TestEqual(TEXT("baked over the layers' range"), Gathered->BakeSurface().UvRange, Range);
		TestTrue(TEXT("with white vertex colors: a landscape has none"), Gathered->BakeSurface().VertexColor == FColor::White);
	}

	// No landscape material: plain gray, and the gray materials are gathered, as Export names them.
	Source.LandscapeMaterial = nullptr;
	Layers.Reset();
	Issues.Reset();
	TestTrue(TEXT("without a landscape material it is still not refused"), FMantleTerrainExport::ChooseLayers(nullptr, Source, TEXT("Landscape"), Layers, Issues));
	TestEqual(TEXT("a plain gray warning for each layer"), CountIssues(Issues, EMantleIssueSeverity::Warning, TEXT("plain gray")), 2);
	if (!TestEqual(TEXT("both layers are kept"), Layers.Num(), 2))
	{
		return false;
	}
	FMantlePlaceDependencies Gray;
	Gray.AddTerrainLayerMaterials(Layers, TEXT("Landscape"));
	for (const FMantleTerrainLayerChoice& Layer : Layers)
	{
		TestNotNull(TEXT("each with a material"), Layer.Material);
		TestFalse(TEXT("which is not read from a landscape material"), Layer.bDerived);
		TestFalse(TEXT("nor the landscape's material baked whole"), Layer.bLandscapeMaterialBake);
		const FString GrayKey = FMantlePlaceAssets::MaterialKey(Layer.Material);
		TestTrue(TEXT("each gray material is gathered"),
			Gray.Materials.ContainsByPredicate([&GrayKey](const FMantleGatheredMaterial& Entry) { return Entry.Key == GrayKey; }));
	}
	return true;
}

// How many quads a bake of the landscape's own material covers: its coarsest texture's repeat, rounded up, at most the cap.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainLandscapeBakeUvRangeTest,
	"Mantle.Editor.Terrain.LandscapeBakeUvRange",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainLandscapeBakeUvRangeTest::RunTest(const FString& Parameters)
{
	FMantleTerrainSource Source;
	Source.LandscapeMaterialUvRepeat = 10.0000001;
	TestEqual(TEXT("a texture tiled by 0.1 (as a float) repeats in 10 quads"), FMantleTerrainExport::LandscapeBakeUvRange(Source), 10);
	Source.LandscapeMaterialUvRepeat = 6.46;
	TestEqual(TEXT("a repeat of 6.46 quads is covered by 7"), FMantleTerrainExport::LandscapeBakeUvRange(Source), 7);
	Source.LandscapeMaterialUvRepeat = 0.5;
	TestEqual(TEXT("a texture repeating within a quad bakes one quad"), FMantleTerrainExport::LandscapeBakeUvRange(Source), 1);
	Source.LandscapeMaterialUvRepeat = 1000.0;
	TestEqual(TEXT("a very coarse texture is capped"), FMantleTerrainExport::LandscapeBakeUvRange(Source), FMantleTerrainExport::MaxLandscapeBakeUvRange);
	Source.LandscapeMaterialUvRepeat = 0.0;
	TestEqual(TEXT("a material that does not say gets the cap"), FMantleTerrainExport::LandscapeBakeUvRange(Source), FMantleTerrainExport::MaxLandscapeBakeUvRange);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
