// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCoverage.h"
#include "MantleCoverageSurvey.h"
#include "MantleHeightfieldReader.h"
#include "MantlePlaceLimits.h"
#include "MantlePlaceSettingsActor.h"
#include "MantleTerrainExport.h"

#include "Engine/World.h"
#include "Materials/Material.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// A level with several landscapes publishes each one as a Terrain: as many as FMantlePlaceLimits allows, in number and in height
// samples together, with Mantle Place Settings entries that can be for one landscape. A real landscape's layers merge on the GPU,
// which an automation run may not have, so these run on landscape sources and sample counts built in code, as the other terrain tests do.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleMultipleTerrainsTestPrivate
{
	/** An empty editor world for one test, destroyed with it. */
	struct FTerrainsWorld
	{
		UWorld* World = nullptr;

		FTerrainsWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld*/ false, TEXT("MantleMultipleTerrainsTestWorld"));
		}

		~FTerrainsWorld()
		{
			if (World)
			{
				World->DestroyWorld(/*bInformEngineOfWorld*/ false);
				if (World->IsRooted())
				{
					World->RemoveFromRoot();
				}
			}
		}
	};

	UMaterialInterface* MakeMaterial(const TCHAR* Name)
	{
		return NewObject<UMaterial>(GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UMaterial::StaticClass(), FName(Name)), RF_Transient);
	}

	FMantleTerrainLayerSetting Entry(const TCHAR* Layer, UMaterialInterface* Material, const TCHAR* Landscape = nullptr)
	{
		FMantleTerrainLayerSetting Out;
		Out.Layer = Layer;
		Out.Material = Material;
		if (Landscape)
		{
			Out.Landscape = TSoftObjectPtr<AActor>(FSoftObjectPath(Landscape));
		}
		return Out;
	}

	/** A 4 x 4 source painted all over with Layer, read from the landscape at Path. */
	FMantleTerrainSource PaintedSource(const TCHAR* Layer, const TCHAR* Path)
	{
		FMantleTerrainSource Source;
		Source.Width = 4;
		Source.Height = 4;
		Source.Heights.Init(32768, 16);
		Source.LayerNames.Add(Layer);
		Source.LayerWeights.AddDefaulted_GetRef().Init(255, 16);
		Source.LandscapePath = FSoftObjectPath(Path);
		return Source;
	}

	const TCHAR* const Village = TEXT("/Game/Maps/Village.Village:PersistentLevel.Landscape1");
	const TCHAR* const Forest = TEXT("/Game/Maps/Village.Village:PersistentLevel.Landscape3");
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainFitTerrainsTest,
	"Mantle.Editor.Terrain.SeveralLandscapesFitThePlace",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainFitTerrainsTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("a place holds several terrains"), FMantlePlaceLimits::MaxTerrainsPerPlace > 1);

	// The samples counted are the published heightfield's: a side past the heightfield's limit is resampled to it.
	const int32 Max = FMantleHeightfieldReader::MaxSize;
	TestEqual(TEXT("a landscape that fits is counted as it is"), FMantleTerrainExport::PublishedSamples(1017, 1144), static_cast<int64>(1017 * 1144));
	TestEqual(TEXT("a wider one as it is published"), FMantleTerrainExport::PublishedSamples(2521, 1891), static_cast<int64>(Max) * 1891);
	TestEqual(TEXT("no ground counts nothing"), FMantleTerrainExport::PublishedSamples(1, 50), static_cast<int64>(0));
	TestEqual(TEXT("nor does a landscape with nothing loaded"), FMantleTerrainExport::PublishedSamples(static_cast<const AActor*>(nullptr)), static_cast<int64>(0));

	// MedievalVillageDay's eight landscapes, as measured: every one publishes.
	{
		const TArray<int64> Samples = { FMantleTerrainExport::PublishedSamples(2521, 1891), FMantleTerrainExport::PublishedSamples(8, 8),
			FMantleTerrainExport::PublishedSamples(1144, 1017), FMantleTerrainExport::PublishedSamples(1017, 1017), FMantleTerrainExport::PublishedSamples(1144, 1144),
			FMantleTerrainExport::PublishedSamples(1017, 1144), FMantleTerrainExport::PublishedSamples(509, 763), FMantleTerrainExport::PublishedSamples(382, 763) };
		TArray<TPair<int32, FMantleValidationIssue>> LeftOut;
		const TArray<int32> Kept = FMantleTerrainExport::FitTerrains(Samples, TArray<FString>(), LeftOut);
		TestEqual(TEXT("MedievalVillageDay's eight landscapes all publish"), Kept.Num(), 8);
		TestEqual(TEXT("and none is left out"), LeftOut.Num(), 0);
	}

	// One past the most a place holds: the last is left out, with an error under its label that names the limit and asks for no merge.
	{
		TArray<int64> Samples;
		TArray<FString> Labels;
		for (int32 Index = 0; Index <= FMantlePlaceLimits::MaxTerrainsPerPlace; ++Index)
		{
			Samples.Add(100);
			Labels.Add(FString::Printf(TEXT("Landscape%d"), Index));
		}
		TArray<TPair<int32, FMantleValidationIssue>> LeftOut;
		const TArray<int32> Kept = FMantleTerrainExport::FitTerrains(Samples, Labels, LeftOut);
		TestEqual(TEXT("the most a place holds publish"), Kept.Num(), FMantlePlaceLimits::MaxTerrainsPerPlace);
		if (TestEqual(TEXT("one is left out"), LeftOut.Num(), 1))
		{
			const FMantleValidationIssue& Issue = LeftOut[0].Value;
			TestEqual(TEXT("the last one"), LeftOut[0].Key, FMantlePlaceLimits::MaxTerrainsPerPlace);
			TestEqual(TEXT("an error"), Issue.Severity, EMantleIssueSeverity::Error);
			TestEqual(TEXT("under its label"), Issue.Actor, Labels.Last());
			TestTrue(TEXT("naming its number and the limit"), Issue.Problem.Contains(FString::Printf(TEXT("is landscape number %d; a place publishes at most %d landscapes"),
				FMantlePlaceLimits::MaxTerrainsPerPlace + 1, FMantlePlaceLimits::MaxTerrainsPerPlace)));
			TestFalse(TEXT("not asking for the landscapes to be merged"), Issue.Remedy.Contains(TEXT("Merge")) || Issue.Problem.Contains(TEXT("Merge")));
		}
	}

	// Past the samples budget: the one that does not fit is left out, and a smaller one after it still publishes.
	{
		const int64 Budget = FMantlePlaceLimits::MaxTerrainSamplesPerPlace;
		const TArray<int64> Samples = { Budget - 10, 20, 5 };
		const TArray<FString> Labels = { TEXT("Big"), TEXT("Middle"), TEXT("Small") };
		TArray<TPair<int32, FMantleValidationIssue>> LeftOut;
		const TArray<int32> Kept = FMantleTerrainExport::FitTerrains(Samples, Labels, LeftOut);
		TestTrue(TEXT("the first and the last publish"), Kept == TArray<int32>{ 0, 2 });
		if (TestEqual(TEXT("the middle one is left out"), LeftOut.Num(), 1))
		{
			TestEqual(TEXT("by its index"), LeftOut[0].Key, 1);
			TestEqual(TEXT("under its label"), LeftOut[0].Value.Actor, FString(TEXT("Middle")));
			TestTrue(TEXT("saying it is left out over the budget"), LeftOut[0].Value.Problem.Contains(TEXT("have at most")) && LeftOut[0].Value.Problem.Contains(TEXT("left out")));
		}
	}
	TestEqual(TEXT("a world with no landscapes publishes none"), FMantleTerrainExport::TerrainLandscapes(nullptr).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainLayerEntriesPerLandscapeTest,
	"Mantle.Editor.Terrain.LayerEntriesPerLandscape",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainLayerEntriesPerLandscapeTest::RunTest(const FString& Parameters)
{
	using namespace MantleMultipleTerrainsTestPrivate;

	FTerrainsWorld Scope;
	if (!TestNotNull(TEXT("a world"), Scope.World))
	{
		return false;
	}
	AMantlePlaceSettingsActor* Settings = Scope.World->SpawnActor<AMantlePlaceSettingsActor>();
	if (!TestNotNull(TEXT("a settings actor"), Settings))
	{
		return false;
	}
	UMaterialInterface* Meadow = MakeMaterial(TEXT("TerrainsMeadow"));
	UMaterialInterface* Moss = MakeMaterial(TEXT("TerrainsMoss"));
	UMaterialInterface* Cliff = MakeMaterial(TEXT("TerrainsCliff"));
	// Grass for every landscape, a different Grass for the forest, and Rock only for the forest.
	Settings->TerrainLayers = { Entry(TEXT("Grass"), Meadow), Entry(TEXT("Grass"), Moss, Forest), Entry(TEXT("Rock"), Cliff, Forest) };

	const FSoftObjectPath VillagePath(Village);
	const FSoftObjectPath ForestPath(Forest);
	auto MaterialOf = [](const FMantleTerrainLayerSetting* Found) { return Found ? Found->Material.Get() : nullptr; };
	TestTrue(TEXT("the forest's own Grass wins"), MaterialOf(Settings->FindTerrainLayer(TEXT("Grass"), ForestPath)) == Moss);
	TestTrue(TEXT("whatever the case of the name"), MaterialOf(Settings->FindTerrainLayer(TEXT("grass"), ForestPath)) == Moss);
	TestTrue(TEXT("the village takes the entry for every landscape"), MaterialOf(Settings->FindTerrainLayer(TEXT("Grass"), VillagePath)) == Meadow);
	TestTrue(TEXT("so does no landscape at all"), MaterialOf(Settings->FindTerrainLayer(TEXT("Grass"))) == Meadow);
	TestTrue(TEXT("the forest's Rock is its own"), MaterialOf(Settings->FindTerrainLayer(TEXT("Rock"), ForestPath)) == Cliff);
	TestNull(TEXT("and not the village's"), Settings->FindTerrainLayer(TEXT("Rock"), VillagePath));
	TestTrue(TEXT("a landscape with nothing painted takes the first entry for it"), MaterialOf(Settings->FirstTerrainLayer(VillagePath)) == Meadow);

	// ChooseLayers finds the entry for the landscape it reads.
	{
		TArray<FMantleTerrainLayerChoice> Layers;
		TArray<FMantleValidationIssue> Issues;
		TestTrue(TEXT("the forest's layers are chosen"), FMantleTerrainExport::ChooseLayers(Scope.World, PaintedSource(TEXT("Grass"), Forest), TEXT("Forest"), Layers, Issues));
		TestTrue(TEXT("its Grass drawn with its own entry"), Layers.Num() == 1 && Layers[0].Material == Moss);
	}
	{
		TArray<FMantleTerrainLayerChoice> Layers;
		TArray<FMantleValidationIssue> Issues;
		TestTrue(TEXT("the village's layers are chosen"), FMantleTerrainExport::ChooseLayers(Scope.World, PaintedSource(TEXT("Grass"), Village), TEXT("Village"), Layers, Issues));
		TestTrue(TEXT("its Grass drawn with the entry for every landscape"), Layers.Num() == 1 && Layers[0].Material == Meadow);
	}

	// Only entries for one landscape: another landscape has none to draw it with.
	Settings->TerrainLayers = { Entry(TEXT("Grass"), Moss, Forest) };
	TestNull(TEXT("the village has no first entry"), Settings->FirstTerrainLayer(VillagePath));
	TestTrue(TEXT("the forest does"), MaterialOf(Settings->FirstTerrainLayer(ForestPath)) == Moss);
	Settings->Destroy();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainCoveragePerLandscapeTest,
	"Mantle.Editor.Terrain.CoverageLinesPerLandscape",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainCoveragePerLandscapeTest::RunTest(const FString& Parameters)
{
	using namespace MantleMultipleTerrainsTestPrivate;

	// Two Terrains each report their layers: the second is not taken as reported already.
	UMaterialInterface* Meadow = MakeMaterial(TEXT("TerrainsMeadow"));
	FMantleTerrainLayerChoice Grass;
	Grass.Layer = TEXT("Grass");
	Grass.Material = Meadow;
	Grass.SourceIndex = 0;
	FMantleCoverage Coverage;
	FMantleCoverageSurvey::ReportTerrainLayers(Coverage, TEXT("Village"), PaintedSource(TEXT("Grass"), Village), { Grass });
	FMantleCoverageSurvey::ReportTerrainLayers(Coverage, TEXT("Forest"), PaintedSource(TEXT("Grass"), Forest), { Grass });
	TestEqual(TEXT("a kept line for each landscape's layer"), Coverage.Count(MantleCoverageCategory::TerrainLayers, EMantleCoverageOutcome::Kept), static_cast<int64>(2));
	const FMantleCoverageCategory* Category = Coverage.Find(MantleCoverageCategory::TerrainLayers);
	const TArray<FMantleCoverageExample>* Examples = Category ? &Category->Get(EMantleCoverageOutcome::Kept).Examples : nullptr;
	TestTrue(TEXT("each naming its landscape"), Examples && Examples->Num() == 2 && (*Examples)[0].Detail.Contains(TEXT("'Village'")) && (*Examples)[1].Detail.Contains(TEXT("'Forest'")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
