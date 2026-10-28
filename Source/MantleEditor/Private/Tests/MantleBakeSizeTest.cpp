// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceAssetConverter.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// DragonCave crashed the editor in MaterialBaking: a texture reported more than the bake's render target holds, GLTFExporter's
// auto-detect asked for that size, and the readback tripped SrcPitch > DstPitch. A bake needs a renderer, so this checks the
// decision that now keeps the size in range.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleBakeSizeTest, "Mantle.Editor.Place.Converter.BakeSizeStaysInRange",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMantleBakeSizeTest::RunTest(const FString& Parameters)
{
	FMantleFlattenSettings Settings;
	Settings.DefaultBakeSize = 1024;
	Settings.MaxBakeSize = 2048;

	int32 Size = 0;
	TestFalse(TEXT("textures within the cap keep auto-detect"), FMantleGltfMaterialFlattener::ChooseFixedBakeSize({ FIntPoint(2048, 512), FIntPoint(256, 256) }, Settings, Size));
	TestFalse(TEXT("no textures keeps auto-detect"), FMantleGltfMaterialFlattener::ChooseFixedBakeSize({}, Settings, Size));

	TestTrue(TEXT("a texture above the cap fixes the size"), FMantleGltfMaterialFlattener::ChooseFixedBakeSize({ FIntPoint(32768, 32768) }, Settings, Size));
	TestEqual(TEXT("at the cap"), Size, 2048);
	TestTrue(TEXT("one wide texture is enough"), FMantleGltfMaterialFlattener::ChooseFixedBakeSize({ FIntPoint(256, 256), FIntPoint(4096, 64) }, Settings, Size));
	TestTrue(TEXT("a texture with no size fixes it"), FMantleGltfMaterialFlattener::ChooseFixedBakeSize({ FIntPoint(512, 512), FIntPoint(0, 0) }, Settings, Size));

	TestEqual(TEXT("a texture with no size bakes at the default"), (FMantleGltfMaterialFlattener::ChooseFixedBakeSize({ FIntPoint(0, 0) }, Settings, Size), Size), 1024);
	Settings.MaxBakeSize = 3000;
	FMantleGltfMaterialFlattener::ChooseFixedBakeSize({ FIntPoint(40000, 8) }, Settings, Size);
	TestEqual(TEXT("an odd cap rounds down to a power of two"), Size, 2048);
	Settings.MaxBakeSize = 2048;
	Settings.DefaultBakeSize = 3000;
	FMantleGltfMaterialFlattener::ChooseFixedBakeSize({ FIntPoint(0, 0) }, Settings, Size);
	TestEqual(TEXT("a default above the cap is held to the cap"), Size, 2048);
	Settings.DefaultBakeSize = 1000;
	FMantleGltfMaterialFlattener::ChooseFixedBakeSize({ FIntPoint(0, 0) }, Settings, Size);
	TestEqual(TEXT("an odd default rounds down to a power of two"), Size, 512);
	return true;
}

#endif
