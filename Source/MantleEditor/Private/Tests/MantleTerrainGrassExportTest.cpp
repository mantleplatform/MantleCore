// Copyright Joshua Eger. All Rights Reserved.
#include "MantleTerrainGrassExport.h"

#include "MantlePlaceLimits.h"

#include "Engine/StaticMesh.h"
#include "LandscapeGrassType.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// A landscape's grass as a Terrain's grass[]: the density conversion, the map packing and the entries
// a grass type makes, all without a landscape or a renderer.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleTerrainGrassExportTestPrivate
{
	/** A variety with a mesh and the numbers the tests set; everything else is the engine's default. */
	FGrassVariety MakeGrassTestVariety(UStaticMesh* Mesh, const float EngineDensity)
	{
		FGrassVariety Variety;
		Variety.GrassMesh = Mesh;
		Variety.GrassDensity.Default = EngineDensity;
		return Variety;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainGrassDensityTest,
	"Mantle.Editor.TerrainGrass.DensityIsPerSquareMetre",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainGrassDensityTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("100 per 100 m2 is 1 per m2"), FMantleTerrainGrassExport::DensityPerSquareMetre(100.0f), 1.0);
	TestEqual(TEXT("the engine default, 400, is 4"), FMantleTerrainGrassExport::DensityPerSquareMetre(400.0f), 4.0);
	TestEqual(TEXT("the engine's top, 1000, is 10"), FMantleTerrainGrassExport::DensityPerSquareMetre(1000.0f), 10.0);
	TestEqual(TEXT("0 stays 0"), FMantleTerrainGrassExport::DensityPerSquareMetre(0.0f), 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainGrassPackingTest,
	"Mantle.Editor.TerrainGrass.PacksFourTypesPerMapLikeTheWeightmap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainGrassPackingTest::RunTest(const FString& Parameters)
{
	// Slots: four types to a map, in channel order.
	const int32 Expected[][2] = { { 0, 0 }, { 0, 1 }, { 0, 2 }, { 0, 3 }, { 1, 0 }, { 3, 3 } };
	const int32 Slots[] = { 0, 1, 2, 3, 4, 15 };
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Slots); ++Index)
	{
		int32 Map = -1;
		int32 Channel = -1;
		FMantleTerrainGrassExport::SlotToMapChannel(Slots[Index], Map, Channel);
		TestEqual(FString::Printf(TEXT("slot %d's map"), Slots[Index]), Map, Expected[Index][0]);
		TestEqual(FString::Printf(TEXT("slot %d's channel"), Slots[Index]), Channel, Expected[Index][1]);
	}
	TestEqual(TEXT("channel names"), FString(FMantleTerrainGrassExport::ChannelName(0)) + FMantleTerrainGrassExport::ChannelName(1)
		+ FMantleTerrainGrassExport::ChannelName(2) + FMantleTerrainGrassExport::ChannelName(3), FString(TEXT("rgba")));
	TestEqual(TEXT("the map's key"), FMantleTerrainGrassExport::GrassMapKey(nullptr, 2), FString(TEXT("grassmap::2")));

	// Two constant types in R and G; B is not a type (INDEX_NONE) and A names a grid that is not there.
	TArray<TArray<uint8>> Constant;
	Constant.Add(TArray<uint8>());
	Constant[0].Init(200, 9);
	Constant.Add(TArray<uint8>());
	Constant[1].Init(50, 9);
	const int32 ConstantIndices[] = { 0, 1, INDEX_NONE, 7 };
	const TArray<FColor> ConstantPixels = FMantleTerrainGrassExport::MakeMap(Constant, ConstantIndices, 3, 3, FIntPoint(2, 2));
	if (TestEqual(TEXT("a 2 x 2 map has 4 pixels"), ConstantPixels.Num(), 4))
	{
		TestEqual(TEXT("R is the first type"), static_cast<int32>(ConstantPixels[3].R), 200);
		TestEqual(TEXT("G is the second type"), static_cast<int32>(ConstantPixels[3].G), 50);
		TestEqual(TEXT("B is empty"), static_cast<int32>(ConstantPixels[3].B), 0);
		TestEqual(TEXT("A is empty"), static_cast<int32>(ConstantPixels[3].A), 0);
	}

	// Texel centres, as the weightmap: 3 samples across into 3 texels land at 1/3, 1 and 5/3 of the way along 0..200.
	TArray<TArray<uint8>> Gradient;
	Gradient.Add(TArray<uint8>());
	Gradient[0] = { 0, 100, 200, 0, 100, 200 };
	const int32 One[] = { 0 };
	const TArray<FColor> AcrossPixels = FMantleTerrainGrassExport::MakeMap(Gradient, One, 3, 2, FIntPoint(3, 1));
	if (TestEqual(TEXT("a 3 x 1 map has 3 pixels"), AcrossPixels.Num(), 3))
	{
		TestEqual(TEXT("first texel centre is 1/3 of a sample in"), static_cast<int32>(AcrossPixels[0].R), 33);
		TestEqual(TEXT("middle texel centre is the middle sample"), static_cast<int32>(AcrossPixels[1].R), 100);
		TestEqual(TEXT("last texel centre is 5/3 of a sample in"), static_cast<int32>(AcrossPixels[2].R), 167);
	}

	// Row order: row 0 of the map is the terrain's first row of samples.
	TArray<TArray<uint8>> Rows;
	Rows.Add(TArray<uint8>());
	Rows[0] = { 0, 0, 0, 255, 255, 255 };
	const TArray<FColor> DownPixels = FMantleTerrainGrassExport::MakeMap(Rows, One, 3, 2, FIntPoint(1, 2));
	if (TestEqual(TEXT("a 1 x 2 map has 2 pixels"), DownPixels.Num(), 2))
	{
		TestEqual(TEXT("the top row is mostly the first row of samples"), static_cast<int32>(DownPixels[0].R), 64);
		TestEqual(TEXT("the bottom row is mostly the last"), static_cast<int32>(DownPixels[1].R), 191);
	}
	TestEqual(TEXT("a grid that is not Width x Height packs as empty"), static_cast<int32>(FMantleTerrainGrassExport::MakeMap(Constant, One, 4, 4, FIntPoint(1, 1))[0].R), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainGrassEntriesTest,
	"Mantle.Editor.TerrainGrass.BuildsEntriesFromAGrassType",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainGrassEntriesTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainGrassExportTestPrivate;

	UStaticMesh* Mesh = NewObject<UStaticMesh>(GetTransientPackage(), NAME_None, RF_Transient);
	ULandscapeGrassType* Type = NewObject<ULandscapeGrassType>(GetTransientPackage(), NAME_None, RF_Transient);

	// 0: a plain one with every number set.
	FGrassVariety Plain = MakeGrassTestVariety(Mesh, 100.0f);
	Plain.ScaleX = FFloatInterval(0.5f, 1.5f);
	Plain.StartCullDistance.Default = 1000;
	Plain.EndCullDistance.Default = 5000;
	Plain.AlignToSurface = false;
	Plain.RandomRotation = true;
	Plain.bCastDynamicShadow = false;
	Type->GrassVarieties.Add(Plain);
	// 1: no mesh. 2: no density. Both are left out.
	Type->GrassVarieties.Add(MakeGrassTestVariety(nullptr, 100.0f));
	Type->GrassVarieties.Add(MakeGrassTestVariety(Mesh, 0.0f));
	// 3: no end cull distance, and a free scale: kept, with a warning each.
	FGrassVariety Free = MakeGrassTestVariety(Mesh, 50.0f);
	Free.Scaling = EGrassScaling::Free;
	Free.ScaleY = FFloatInterval(2.0f, 3.0f);
	Free.EndCullDistance.Default = 0;
	Type->GrassVarieties.Add(Free);

	FMantleTerrainGrassResult Result;
	const int32 Added = FMantleTerrainGrassExport::AppendEntries(Type, TEXT("LGT_Test"), 5, Result);
	TestEqual(TEXT("two varieties are usable"), Added, 2);
	if (TestEqual(TEXT("two entries"), Result.Entries.Num(), 2))
	{
		const FMantleGrassEntryDraft& First = Result.Entries[0];
		TestTrue(TEXT("the mesh is the variety's"), First.Mesh == Mesh);
		TestEqual(TEXT("slot 5 is map 1"), First.Map, 1);
		TestEqual(TEXT("slot 5 is channel 1"), First.Channel, 1);
		TestEqual(TEXT("the look names the channel too"), First.Look.Channel, 1);
		TestEqual(TEXT("density is per m2"), First.Look.Density, 1.0);
		TestEqual(TEXT("scale minimum is ScaleX's"), First.Look.MinScale, 0.5);
		TestEqual(TEXT("scale maximum is ScaleX's"), First.Look.MaxScale, 1.5);
		TestEqual(TEXT("cull start"), First.Look.CullStart, 1000.0);
		TestEqual(TEXT("cull end"), First.Look.CullEnd, 5000.0);
		TestFalse(TEXT("align to surface"), First.Look.bAlignToSurface);
		TestTrue(TEXT("random yaw"), First.Look.bRandomYaw);
		TestFalse(TEXT("cast shadow"), First.Look.bCastShadow);

		const FMantleGrassEntryDraft& Second = Result.Entries[1];
		TestEqual(TEXT("density 50 is 0.5 per m2"), Second.Look.Density, 0.5);
		TestEqual(TEXT("no end cull distance is the default 10000"), Second.Look.CullEnd, 10000.0);
		TestTrue(TEXT("a start of 0 is a cut just short of the end"), Second.Look.CullStart > 0.0 && Second.Look.CullStart < Second.Look.CullEnd);
	}
	// A warning each for the missing mesh, the zero density, the missing end distance and the free scale.
	TestEqual(TEXT("four warnings"), Result.Warnings.Num(), 4);

	// The cap: a grass type with more varieties than a Terrain holds is cut, with one warning naming them.
	ULandscapeGrassType* Many = NewObject<ULandscapeGrassType>(GetTransientPackage(), NAME_None, RF_Transient);
	for (int32 Index = 0; Index < FMantlePlaceLimits::MaxTerrainGrass + 3; ++Index)
	{
		Many->GrassVarieties.Add(MakeGrassTestVariety(Mesh, 100.0f));
	}
	FMantleTerrainGrassResult Capped;
	TestEqual(TEXT("the cap is MaxTerrainGrass"), FMantleTerrainGrassExport::AppendEntries(Many, TEXT("LGT_Many"), 0, Capped), FMantlePlaceLimits::MaxTerrainGrass);
	TestEqual(TEXT("one warning for what was left out"), Capped.Warnings.Num(), 1);
	TestEqual(TEXT("a slot past the last map adds nothing"), FMantleTerrainGrassExport::AppendEntries(Type, TEXT("LGT_Test"), FMantlePlaceLimits::MaxTerrainGrassMaps * 4, Capped), 0);

	// What the exporter writes for an entry once its ids are known.
	if (Result.Entries.Num() > 0)
	{
		const FMantleTerrainGrass Grass = FMantleTerrainGrassExport::MakeGrass(Result.Entries[0], TEXT("tex_map"), TEXT("mesh_a"), { TEXT("mat_a"), FString() });
		TestEqual(TEXT("the map id"), Grass.Map, FString(TEXT("tex_map")));
		TestEqual(TEXT("the mesh id"), Grass.Mesh, FString(TEXT("mesh_a")));
		TestEqual(TEXT("the material ids"), Grass.Materials.Num(), 2);
		TestEqual(TEXT("the channel"), Grass.Channel, 1);
		TestEqual(TEXT("the density"), Grass.Density, 1.0);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
