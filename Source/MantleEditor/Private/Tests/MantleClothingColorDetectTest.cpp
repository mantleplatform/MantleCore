// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAvatarContract.h"
#include "MantleClothingColorDetect.h"
#include "MantleClothingColorMeasure.h"
#include "MantleCosmeticExport.h"
#include "MantleCosmeticPackage.h"
#include "MantleGlbWriter.h"
#include "MantlePng.h"

#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// Colour areas found from a clothing material's own mask texture. The masks are synthetic and the mesh is one quad
// over the whole UV tile, so no asset is needed.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleClothingColorDetectTestPrivate
{
	constexpr int32 Size = 64;

	/** One quad over the whole UV tile on material 0. */
	FMantleGlbMesh Quad()
	{
		FMantleGlbMesh Mesh;
		Mesh.Positions = { FVector3f(0, 0, 0), FVector3f(1, 0, 0), FVector3f(1, 1, 0), FVector3f(0, 1, 0) };
		Mesh.UVs = { FVector2f(0, 0), FVector2f(1, 0), FVector2f(1, 1), FVector2f(0, 1) };
		Mesh.MaterialNames = { TEXT("Shirt") };
		FMantleGlbPrimitive& Primitive = Mesh.Primitives.AddDefaulted_GetRef();
		Primitive.MaterialIndex = 0;
		Primitive.FirstVertex = 0;
		Primitive.NumVertices = 4;
		Primitive.Indices = { 0, 1, 2, 0, 2, 3 };
		return Mesh;
	}

	bool InGreen(int32 X, int32 Y) { return X < 20; }
	bool InBlue(int32 X, int32 Y) { return X >= 20 && Y >= 44; }

	/**
	 * The Parametric Outfit layout: R a smooth greyscale shading ramp (never an area), G and B clean hard-edged areas that
	 * do not overlap, A opaque.
	 */
	TArray<FColor> PackLikeMask()
	{
		TArray<FColor> Pixels;
		Pixels.SetNumUninitialized(Size * Size);
		for (int32 Y = 0; Y < Size; ++Y)
		{
			for (int32 X = 0; X < Size; ++X)
			{
				const uint8 Shade = static_cast<uint8>(40 + (X + Y) * 170 / (2 * (Size - 1)));
				Pixels[Y * Size + X] = FColor(Shade, InGreen(X, Y) ? 255 : 0, InBlue(X, Y) ? 255 : 0, 255);
			}
		}
		return Pixels;
	}

	UMaterial* MaterialWithMask(const TArray<FColor>& Pixels, const TCHAR* Parameter = TEXT("Masks"))
	{
		UTexture2D* Texture = NewObject<UTexture2D>(GetTransientPackage(), NAME_None, RF_Transient);
		Texture->SRGB = false;
		Texture->Source.Init(Size, Size, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Pixels.GetData()));
		UMaterial* Material = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
		UMaterialExpressionTextureSampleParameter2D* Sample = NewObject<UMaterialExpressionTextureSampleParameter2D>(Material);
		Sample->Material = Material;
		Sample->ParameterName = Parameter;
		Sample->Texture = Texture;
		Sample->SamplerType = SAMPLERTYPE_LinearColor;
		Sample->UpdateParameterGuid(true, false);
		Sample->UpdateMaterialExpressionGuid(true, false);
		Material->GetExpressionCollection().AddExpression(Sample);
		Material->UpdateCachedExpressionData();
		return Material;
	}

	/** A garment package as the export has it just before detection: one material with an exported grey albedo. */
	FMantleCosmeticPackage GreyGarmentPackage()
	{
		FMantleCosmeticPackage Package;
		Package.Class = EMantleCosmeticClass::Garment;
		Package.Slot = TEXT("top");
		TArray<FColor> Albedo;
		Albedo.Init(FColor(128, 128, 128, 255), Size * Size);
		FMantleAssetUploadOptions Options;
		Options.Kind = TEXT("texture");
		Options.Role = TEXT("color");
		Package.AddAsset(TEXT("albedo"), TEXT("Albedo"), Options, FMantlePng::EncodeRGBA8(Size, Size, Albedo));
		FMantleCosmeticMaterialRef& Ref = Package.Materials.AddDefaulted_GetRef();
		Ref.AlbedoKey = TEXT("albedo");
		Package.MaterialNames = { TEXT("Shirt") };
		return Package;
	}

	FMantleCosmeticDraft GarmentDraft()
	{
		FMantleCosmeticDraft Draft;
		Draft.Class = EMantleCosmeticClass::Garment;
		Draft.Slot = TEXT("top");
		Draft.Name = TEXT("Test shirt");
		return Draft;
	}

	TArray<FString> RegionIds(const FMantleCosmeticMaterialRef& Ref)
	{
		TArray<FString> Ids;
		for (const FMantleClothingColorRegion& Region : Ref.ColorRegions) Ids.Add(Region.Id);
		return Ids;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleClothingColorDetectSyntheticTest,
	"Mantle.ClothingColor.Detect.SmoothRedRejectedGreenAndBlueAccepted",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleClothingColorDetectSyntheticTest::RunTest(const FString& Parameters)
{
	using namespace MantleClothingColorDetectTestPrivate;
	const FMantleGlbMesh Mesh = Quad();
	const TArray<FColor> Mask = PackLikeMask();
	FMantleClothingColorDetection Detection;
	Detection.MaterialName = TEXT("Shirt");
	TArray<FColor> Clean;
	TArray<uint8> Used;
	int32 MainNumber = 1, AccentNumber = 1;
	FString Error;
	if (!TestTrue(TEXT("detection runs over the quad"), FMantleClothingColorDetect::Detect(Mesh, 0, FIntPoint(Size, Size), Mask, Detection, Clean, Used, MainNumber, AccentNumber, Error)))
	{
		AddError(Error);
		return false;
	}
	if (TestEqual(TEXT("three channels scored"), Detection.Scores.Num(), 3))
	{
		TestFalse(TEXT("the smooth red shading is not an area"), Detection.Scores[0].bAccepted);
		TestTrue(TEXT("green is an area"), Detection.Scores[1].bAccepted);
		TestTrue(TEXT("blue is an area"), Detection.Scores[2].bAccepted);
	}
	TestEqual(TEXT("exactly green and blue are taken, in that order"), Detection.SourceChannels, TArray<uint8>({ 1, 2 }));
	if (TestEqual(TEXT("Main plus two accents"), Detection.Areas.Num(), 3))
	{
		TestEqual(TEXT("Main id"), Detection.Areas[0].Id, FString(TEXT("main")));
		TestEqual(TEXT("Main name"), Detection.Areas[0].Label, FString(TEXT("Main")));
		TestEqual(TEXT("Main is red in the clean map"), static_cast<int32>(Detection.Areas[0].Channel), 0);
		TestEqual(TEXT("Accent 1 id"), Detection.Areas[1].Id, FString(TEXT("accent_1")));
		TestEqual(TEXT("Accent 1 name"), Detection.Areas[1].Label, FString(TEXT("Accent 1")));
		TestEqual(TEXT("Accent 1 is green"), static_cast<int32>(Detection.Areas[1].Channel), 1);
		TestEqual(TEXT("Accent 2 id"), Detection.Areas[2].Id, FString(TEXT("accent_2")));
		TestEqual(TEXT("Accent 2 is blue"), static_cast<int32>(Detection.Areas[2].Channel), 2);
	}
	TestTrue(TEXT("nothing to explain when found"), Detection.Why.IsEmpty());
	// The clean map: every texel in exactly one area, the shading gone.
	bool bPartition = Clean.Num() == Size * Size;
	for (int32 Y = 0; Y < Size && bPartition; ++Y)
	{
		for (int32 X = 0; X < Size && bPartition; ++X)
		{
			const FColor Expected = InGreen(X, Y) ? FColor(0, 255, 0, 255) : InBlue(X, Y) ? FColor(0, 0, 255, 255) : FColor(255, 0, 0, 255);
			bPartition = Clean[Y * Size + X] == Expected;
			if (!bPartition) AddError(FString::Printf(TEXT("clean map at %d,%d is %s, expected %s"), X, Y, *Clean[Y * Size + X].ToString(), *Expected.ToString()));
		}
	}
	TestTrue(TEXT("the clean map is Main red, Accent 1 green, Accent 2 blue"), bPartition);
	TestEqual(TEXT("numbering moves on for a second material"), MainNumber, 2);
	TestEqual(TEXT("accent numbering moves on"), AccentNumber, 3);

	// A mostly-binary channel with a wide soft ramp (a gradient, not an edge) is rejected on its edges alone.
	TArray<FColor> Ramp = Mask;
	for (int32 Y = 0; Y < Size; ++Y)
	{
		for (int32 X = 0; X < Size; ++X)
		{
			const int32 Value = FMath::Clamp((X - 28) * 255 / 7, 0, 255);
			Ramp[Y * Size + X].R = static_cast<uint8>(Value);
		}
	}
	const FMantleAreaChannelScore Soft = FMantleClothingColorDetect::ScoreChannel(Ramp, Used, FIntPoint(Size, Size), 0);
	TestTrue(TEXT("the ramp is mostly near 0 or 1"), Soft.Binary >= FMantleClothingColorDetect::MinBinary);
	TestFalse(TEXT("but its soft edge keeps it from being an area"), Soft.bAccepted);

	// Three clean channels look like a many-colour ID map: not split in v1.
	TArray<FColor> ThreeWay = Mask;
	for (int32 Y = 0; Y < Size; ++Y) for (int32 X = 0; X < Size; ++X) ThreeWay[Y * Size + X].R = Y < 10 ? 255 : 0;
	FMantleClothingColorDetection Many;
	MainNumber = AccentNumber = 1;
	FMantleClothingColorDetect::Detect(Mesh, 0, FIntPoint(Size, Size), ThreeWay, Many, Clean, Used, MainNumber, AccentNumber, Error);
	TestEqual(TEXT("a three-channel map finds nothing"), Many.Areas.Num(), 0);
	TestFalse(TEXT("and says why"), Many.Why.IsEmpty());

	TestTrue(TEXT("Masks names an area map"), FMantleClothingColorDetect::IsMaskParameterName(TEXT("Masks")));
	TestTrue(TEXT("Region ID mask names one"), FMantleClothingColorDetect::IsMaskParameterName(TEXT("Region_ID_Mask")));
	TestTrue(TEXT("ID names one"), FMantleClothingColorDetect::IsMaskParameterName(TEXT("id")));
	TestFalse(TEXT("an opacity mask does not"), FMantleClothingColorDetect::IsMaskParameterName(TEXT("OpacityMask")));
	TestFalse(TEXT("a base colour does not"), FMantleClothingColorDetect::IsMaskParameterName(TEXT("BaseColor")));

	// A greyscale map (one area repeated in every channel) is one area, not a many-colour ID map.
	TArray<FColor> Grey = Mask;
	for (int32 Y = 0; Y < Size; ++Y) for (int32 X = 0; X < Size; ++X) { const uint8 On = InGreen(X, Y) ? 255 : 0; Grey[Y * Size + X] = FColor(On, On, On, 255); }
	FMantleClothingColorDetection GreyDetection;
	MainNumber = AccentNumber = 1;
	FMantleClothingColorDetect::Detect(Mesh, 0, FIntPoint(Size, Size), Grey, GreyDetection, Clean, Used, MainNumber, AccentNumber, Error);
	TestEqual(TEXT("a greyscale map takes one channel"), GreyDetection.SourceChannels, TArray<uint8>({ 0 }));
	TestEqual(TEXT("Main plus one accent"), GreyDetection.Areas.Num(), 2);

	// Tiled UVs (the quad one tile over) read the same texels: textures repeat at runtime.
	FMantleGlbMesh Tiled = Mesh;
	for (FVector2f& UV : Tiled.UVs) UV += FVector2f(1.0f, -2.0f);
	TArray<uint8> TiledUsed;
	TestTrue(TEXT("tiled UVs are measured"), FMantleClothingColorMeasure::MarkUsedTexels(Tiled, 0, FIntPoint(Size, Size), TiledUsed, Error));
	TestEqual(TEXT("and cover the same texels"), TiledUsed, Used);

	// Sellers' own names for area maps are tried; the map's content decides.
	for (const TCHAR* Name : { TEXT("Mask_01"), TEXT("PatternMask"), TEXT("Albedo_RGB_Mask"), TEXT("Masks 01to04"), TEXT("ColorID") })
	{
		TestTrue(FString::Printf(TEXT("%s is tried"), Name), FMantleClothingColorDetect::IsLooseMaskParameterName(Name));
	}
	for (const TCHAR* Name : { TEXT("OpacityMask"), TEXT("BodyMask"), TEXT("Alpha_Mask"), TEXT("BaseColor"), TEXT("Normal") })
	{
		TestFalse(FString::Printf(TEXT("%s is not"), Name), FMantleClothingColorDetect::IsLooseMaskParameterName(Name));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleClothingColorDetectBlackTest,
	"Mantle.ClothingColor.Detect.AllBlackFindsNothing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleClothingColorDetectBlackTest::RunTest(const FString& Parameters)
{
	using namespace MantleClothingColorDetectTestPrivate;
	TArray<FColor> Black;
	Black.Init(FColor(0, 0, 0, 255), Size * Size);
	FMantleClothingColorDetection Detection;
	TArray<FColor> Clean;
	TArray<uint8> Used;
	int32 MainNumber = 1, AccentNumber = 1;
	FString Error;
	TestTrue(TEXT("detection runs"), FMantleClothingColorDetect::Detect(Quad(), 0, FIntPoint(Size, Size), Black, Detection, Clean, Used, MainNumber, AccentNumber, Error));
	TestEqual(TEXT("no channel taken"), Detection.SourceChannels.Num(), 0);
	TestEqual(TEXT("no areas"), Detection.Areas.Num(), 0);
	TestFalse(TEXT("a plain reason"), Detection.Why.IsEmpty());
	TestEqual(TEXT("numbering untouched"), MainNumber, 1);

	// Through the export step: a map with no areas falls back to the whole material as one colour area, with no error.
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get(&Error);
	if (!TestNotNull(TEXT("the avatar contract loads"), Contract)) { AddError(Error); return false; }
	FMantleCosmeticPackage Package = GreyGarmentPackage();
	UMaterial* Material = MaterialWithMask(Black);
	TArray<UMaterialInterface*> Materials = { Material };
	FMantleCosmeticExport::AddDetectedClothingColors(Package, Quad(), Materials, GarmentDraft(), 2048, *Contract);
	TestFalse(TEXT("no error"), Package.HasErrors());
	TestEqual(TEXT("the whole material is one region"), RegionIds(Package.Materials[0]), TArray<FString>({ TEXT("main") }));
	TestFalse(TEXT("its area map uploads"), Package.Materials[0].ColorMaskKey.IsEmpty());
	if (TestEqual(TEXT("one material looked at"), Package.DetectedClothingColors.Num(), 1))
	{
		TestTrue(TEXT("no map is named as the source"), Package.DetectedClothingColors[0].Parameter.IsEmpty());
		TestEqual(TEXT("one whole-material area"), Package.DetectedClothingColors[0].Areas.Num(), 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleClothingColorDetectExportTest,
	"Mantle.ClothingColor.Detect.ExportUsesFoundAreasAndChoices",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleClothingColorDetectExportTest::RunTest(const FString& Parameters)
{
	using namespace MantleClothingColorDetectTestPrivate;
	FString Error;
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get(&Error);
	if (!TestNotNull(TEXT("the avatar contract loads"), Contract)) { AddError(Error); return false; }
	UMaterial* Material = MaterialWithMask(PackLikeMask());
	TArray<UMaterialInterface*> Materials = { Material };
	const FMantleGlbMesh Mesh = Quad();

	// Nothing changed by the creator: every area found uploads.
	FMantleCosmeticPackage Package = GreyGarmentPackage();
	FMantleCosmeticExport::AddDetectedClothingColors(Package, Mesh, Materials, GarmentDraft(), 2048, *Contract);
	TestFalse(TEXT("no error"), Package.HasErrors());
	const FMantleCosmeticMaterialRef& Ref = Package.Materials[0];
	TestEqual(TEXT("all three areas upload by default"), RegionIds(Ref), TArray<FString>({ TEXT("main"), TEXT("accent_1"), TEXT("accent_2") }));
	const FMantleCosmeticUploadAsset* MaskAsset = Package.FindAsset(Ref.ColorMaskKey);
	if (TestNotNull(TEXT("the clean area map is an upload asset"), MaskAsset))
	{
		TestEqual(TEXT("as data, not colour"), MaskAsset->Options.Role, FString(TEXT("data")));
	}
	for (const FMantleClothingColorRegion& Region : Ref.ColorRegions)
	{
		TestTrue(FString::Printf(TEXT("%s is measured"), *Region.Id), Region.ReferenceLuminance > 0.0f);
		TestEqual(FString::Printf(TEXT("%s's default is the item's own grey"), *Region.Id), Region.DefaultColor, FColor(128, 128, 128, 255));
	}

	// Unticking Accent 1 leaves it out of the package; renaming Accent 2 carries the name.
	FMantleCosmeticDraft Draft = GarmentDraft();
	FMantleClothingColorChoice& Untick = Draft.ClothingColorChoices.AddDefaulted_GetRef();
	Untick.MaterialName = TEXT("Shirt"); Untick.RegionId = TEXT("accent_1"); Untick.bEnabled = false;
	FMantleClothingColorChoice& Rename = Draft.ClothingColorChoices.AddDefaulted_GetRef();
	Rename.MaterialName = TEXT("Shirt"); Rename.RegionId = TEXT("accent_2"); Rename.Label = TEXT("Laces");
	FMantleCosmeticPackage Chosen = GreyGarmentPackage();
	FMantleCosmeticExport::AddDetectedClothingColors(Chosen, Mesh, Materials, Draft, 2048, *Contract);
	TestFalse(TEXT("no error with choices"), Chosen.HasErrors());
	TestEqual(TEXT("the unticked area is omitted"), RegionIds(Chosen.Materials[0]), TArray<FString>({ TEXT("main"), TEXT("accent_2") }));
	if (Chosen.Materials[0].ColorRegions.Num() == 2)
	{
		TestEqual(TEXT("the rename carries"), Chosen.Materials[0].ColorRegions[1].Label, FString(TEXT("Laces")));
	}
	if (TestEqual(TEXT("the panel still lists every area found"), Chosen.DetectedClothingColors.Num(), 1))
	{
		TestEqual(TEXT("three of them"), Chosen.DetectedClothingColors[0].Areas.Num(), 3);
	}

	// Hand-made colours override detection for the whole item.
	FMantleCosmeticDraft HandMade = GarmentDraft();
	HandMade.ClothingColors.AddDefaulted();
	FMantleCosmeticPackage Manual = GreyGarmentPackage();
	FMantleCosmeticExport::AddDetectedClothingColors(Manual, Mesh, Materials, HandMade, 2048, *Contract);
	TestEqual(TEXT("hand-made colours turn detection off"), Manual.Materials[0].ColorRegions.Num(), 0);

	// A seller's own name for the map ("PatternMask") finds the same areas.
	TArray<UMaterialInterface*> Named = { MaterialWithMask(PackLikeMask(), TEXT("PatternMask")) };
	FMantleCosmeticPackage NamedPackage = GreyGarmentPackage();
	FMantleCosmeticExport::AddDetectedClothingColors(NamedPackage, Mesh, Named, GarmentDraft(), 2048, *Contract);
	TestEqual(TEXT("a PatternMask map finds all three areas"), RegionIds(NamedPackage.Materials[0]), TArray<FString>({ TEXT("main"), TEXT("accent_1"), TEXT("accent_2") }));

	// One material drawn by two sections (a left and right shoe): the second takes the first's ids, one set of controls.
	FMantleGlbMesh Pair = Mesh;
	Pair.MaterialNames = { TEXT("Shoe"), TEXT("Shoe_2") };
	FMantleGlbPrimitive Second = Pair.Primitives[0];
	Second.MaterialIndex = 1;
	Pair.Primitives.Add(Second);
	FMantleCosmeticPackage PairPackage = GreyGarmentPackage();
	const FMantleCosmeticMaterialRef FirstShoe = PairPackage.Materials[0];
	PairPackage.Materials.Add(FirstShoe);
	PairPackage.MaterialNames = { TEXT("Shoe"), TEXT("Shoe_2") };
	TArray<UMaterialInterface*> Shared = { Material, Material };
	FMantleCosmeticExport::AddDetectedClothingColors(PairPackage, Pair, Shared, GarmentDraft(), 2048, *Contract);
	TestEqual(TEXT("the first shoe's areas"), RegionIds(PairPackage.Materials[0]), TArray<FString>({ TEXT("main"), TEXT("accent_1"), TEXT("accent_2") }));
	TestEqual(TEXT("the second shoe shares them"), RegionIds(PairPackage.Materials[1]), TArray<FString>({ TEXT("main"), TEXT("accent_1"), TEXT("accent_2") }));

	// Off for anything that is not a top, bottom or footwear.
	FMantleCosmeticDraft Hat = GarmentDraft();
	Hat.Slot = TEXT("hat");
	FMantleCosmeticPackage HatPackage = GreyGarmentPackage();
	FMantleCosmeticExport::AddDetectedClothingColors(HatPackage, Mesh, Materials, Hat, 2048, *Contract);
	TestEqual(TEXT("no areas on other slots"), HatPackage.Materials[0].ColorRegions.Num(), 0);

	// A rigid accessory (a baseball cap) gets its colour areas as clothing does.
	FMantleCosmeticDraft Cap = GarmentDraft();
	Cap.Class = EMantleCosmeticClass::Accessory;
	Cap.Slot = TEXT("acc_hat");
	FMantleCosmeticPackage CapPackage = GreyGarmentPackage();
	FMantleCosmeticExport::AddDetectedClothingColors(CapPackage, Mesh, Materials, Cap, 2048, *Contract);
	TestFalse(TEXT("no error on an accessory"), CapPackage.HasErrors());
	TestEqual(TEXT("an accessory finds all three areas"), RegionIds(CapPackage.Materials[0]), TArray<FString>({ TEXT("main"), TEXT("accent_1"), TEXT("accent_2") }));
	TestFalse(TEXT("its area map uploads"), CapPackage.Materials[0].ColorMaskKey.IsEmpty());
	return true;
}

#endif
