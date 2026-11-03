// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceAssetConverter.h"
#include "MantlePlaceAssets.h"
#include "MantleVehicleConverter.h"

#include "Materials/Material.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "MantleVehicleSource.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS

// A GTA model's tint layers (PRIMARY, SECONDARY, WHEEL) draw with pictures the game is meant to tint: their color is a mask. A black leather picture would keep any tint black, so
// the converter publishes such a picture, on a painted section only, as a light gray version of itself with its variation kept.

namespace MantleGtaTintMaskTestPrivate
{
	double Average(const TArray<FColor>& Pixels)
	{
		double Sum = 0.0;
		for (const FColor& Pixel : Pixels)
		{
			Sum += Pixel.R / 255.0;
		}
		return Pixels.Num() > 0 ? Sum / Pixels.Num() : 0.0;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleGtaTintMaskPicturesTest, "Mantle.Vehicle.PaintLayers.TintMaskPicture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleGtaTintMaskPicturesTest::RunTest(const FString&)
{
	using namespace MantleGtaTintMaskTestPrivate;

	// A dark picture with a grain: gray values 4 to 40, in a coloured tone (leather is dark brown).
	TArray<FColor> Dark;
	for (int32 Index = 0; Index < 64; ++Index)
	{
		const uint8 Level = static_cast<uint8>(4 + Index * 36 / 63);
		Dark.Add(FColor(Level, static_cast<uint8>(Level * 0.8f), static_cast<uint8>(Level * 0.6f), 255));
	}
	TArray<FColor> Mask = Dark;
	FMantlePlaceAssetConverter::NormalizeTintMask(Mask);
	// The scale stops at 12 times, and the brightest grain clips at white, so the average lands a little under the 0.92 target.
	TestTrue(TEXT("a dark picture now reads light: its average is far above where it was"), Average(Mask) > 0.7);
	bool bGray = true;
	bool bOrdered = true;
	for (int32 Index = 0; Index < Mask.Num(); ++Index)
	{
		bGray = bGray && Mask[Index].R == Mask[Index].G && Mask[Index].G == Mask[Index].B;
		bOrdered = bOrdered && (Index == 0 || Mask[Index].R >= Mask[Index - 1].R);
	}
	TestTrue(TEXT("it is gray (the color is dropped)"), bGray);
	TestTrue(TEXT("and the grain is still there: darker stays darker"), bOrdered && Mask[0].R < Mask.Last().R);
	TestTrue(TEXT("the grain is not flattened"), Mask.Last().R - Mask[0].R > 20);

	// Alpha is untouched, and a see-through pixel does not weigh in.
	TArray<FColor> WithAlpha = { FColor(10, 10, 10, 255), FColor(10, 10, 10, 128), FColor(250, 250, 250, 0) };
	FMantlePlaceAssetConverter::NormalizeTintMask(WithAlpha);
	TestTrue(TEXT("alpha is kept"), WithAlpha[0].A == 255 && WithAlpha[1].A == 128 && WithAlpha[2].A == 0);
	TestTrue(TEXT("the pixels that show are lightened (the 12 times cap), the invisible one did not count"), WithAlpha[0].R > 100);

	// A light picture stays as it is; a flat black one becomes a flat light mask; an empty one is fine.
	TArray<FColor> Light = { FColor(240, 240, 240, 255), FColor(220, 230, 235, 255), FColor(255, 255, 255, 255) };
	const TArray<FColor> LightBefore = Light;
	FMantlePlaceAssetConverter::NormalizeTintMask(Light);
	TestTrue(TEXT("a light picture is left alone"), Light == LightBefore);
	TArray<FColor> Black;
	Black.Init(FColor(0, 0, 0, 255), 16);
	FMantlePlaceAssetConverter::NormalizeTintMask(Black);
	TestTrue(TEXT("an all-black picture becomes a flat light mask"), Black[0].R > 200 && Black[0].R == Black[15].R);
	TArray<FColor> Empty;
	FMantlePlaceAssetConverter::NormalizeTintMask(Empty);
	TestEqual(TEXT("an empty picture stays empty"), Empty.Num(), 0);

	// The cache keys on it, and the plain surface keeps its key.
	FMantleBakeSurface Plain;
	TestTrue(TEXT("a plain surface is plain, with no cache tag"), Plain.IsPlain() && Plain.CacheTag().IsEmpty());
	FMantleBakeSurface Masked;
	Masked.bTintMask = true;
	TestTrue(TEXT("a tint mask keeps the plain bake but has its own cache tag"), Masked.IsPlain() && Masked.CacheTag().Contains(TEXT("tintmask")));
	const FString MaterialKey = TEXT("material:/Game/Car/M_Body");
	TestTrue(TEXT("the tint-mask variant has its own key, and the plain key is not it"), FMantlePlaceAssets::TintMaskKey(MaterialKey) != MaterialKey && FMantlePlaceAssets::TintMaskKey(MaterialKey).StartsWith(MaterialKey));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleGtaTintMaskNamesTest, "Mantle.Vehicle.PaintLayers.TintMaskNames",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleGtaTintMaskNamesTest::RunTest(const FString&)
{
	const TStrongObjectPtr<UMaterial> Plain(NewObject<UMaterial>(GetTransientPackage()));
	// A GTA export's tint layers, by name.
	for (const TCHAR* Name : { TEXT("vehicle_generic_smallspecmap__PRIMARY_"), TEXT("leather_1__SECONDARY_"), TEXT("vehicle_generic_smallspecmap__WHEEL_"), TEXT("amg_dayinf__SECONDARY__001") })
	{
		TestTrue(*FString::Printf(TEXT("%s is a GTA tint layer"), Name), FMantleVehicleConverter::IsGtaTintLayer(Name, Plain.Get()));
	}
	// Nothing else is: not a body, a rim by another name, glass or a tire that carries the word.
	for (const TCHAR* Name : { TEXT("body"), TEXT("alloy_rim"), TEXT("carbon_fiber"), TEXT("glass__PRIMARY_"), TEXT("tire__WHEEL_") })
	{
		TestFalse(*FString::Printf(TEXT("%s is not a GTA tint layer"), Name), FMantleVehicleConverter::IsGtaTintLayer(Name, Plain.Get()));
	}
	TestFalse(TEXT("no material and no name: not one"), FMantleVehicleConverter::IsGtaTintLayer(FString(), nullptr));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleGtaTintMaskSectionsTest, "Mantle.Vehicle.PaintLayers.TintMaskSections",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleGtaTintMaskSectionsTest::RunTest(const FString&)
{
	const TStrongObjectPtr<UMaterial> Gta(NewObject<UMaterial>(GetTransientPackage(), TEXT("vehicle_generic__PRIMARY_")));
	// A real paint material (the built-in cars' contract: a Paint Tint parameter) named like a tint layer.
	const TStrongObjectPtr<UMaterial> RealPaint(NewObject<UMaterial>(GetTransientPackage(), TEXT("car_PRIMARY_paint")));
	UMaterialExpressionVectorParameter* Tint = NewObject<UMaterialExpressionVectorParameter>(RealPaint.Get());
	Tint->ParameterName = TEXT("Paint Tint");
	Tint->DefaultValue = FLinearColor::White;
	RealPaint->GetExpressionCollection().AddExpression(Tint);
	RealPaint->UpdateCachedExpressionData();

	const TStrongObjectPtr<UMantleVehicleSource> Source(NewObject<UMantleVehicleSource>(GetTransientPackage()));
	Source->Paint.Sections = { TEXT("body_PRIMARY_") };
	FMantleVehicleSourcePaintLayer Layer;
	Layer.Sections = { TEXT("seat__SECONDARY_") };
	Source->Paint.Layers.Add(Layer);

	TestTrue(TEXT("a ticked GTA tint section is a tint mask"), FMantleVehicleConverter::BodySectionIsTintMask(*Source, TEXT("body_PRIMARY_"), Gta.Get()));
	TestTrue(TEXT("so is one in a paint layer"), FMantleVehicleConverter::BodySectionIsTintMask(*Source, TEXT("seat__SECONDARY_"), Gta.Get()));
	TestFalse(TEXT("the same material on an unticked section is not (it keeps its plain upload)"), FMantleVehicleConverter::BodySectionIsTintMask(*Source, TEXT("trim_PRIMARY_"), Gta.Get()));
	TestFalse(TEXT("a real paint material named like a tint layer is left alone"), FMantleVehicleConverter::BodySectionIsTintMask(*Source, TEXT("body_PRIMARY_"), RealPaint.Get()));
	TestFalse(TEXT("no material, no mask"), FMantleVehicleConverter::BodySectionIsTintMask(*Source, TEXT("body_PRIMARY_"), nullptr));
	return true;
}

#endif
