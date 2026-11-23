// Copyright Joshua Eger. All Rights Reserved.
#include "MantleGlbWriter.h"
#include "MantlePlaceAssetConverter.h"
#include "MantlePlaceAssets.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant2Vector.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// A4 (publish test level, City Sample's road): a road material places its wet lanes and edges with a second UV set (UV1). The bake card has
// no UV1 (the engine reads it as UV0), so those masks were baked into every repeat of the first set's tile, and the road published wet.
// Now the bake holds every UV1 read at a point of the meshes' own UV1, at their 25th, 50th and 75th percentile, and keeps the bake most of
// the mesh agrees with: the masks bake at the value most of the surface has (dry), and the sharp tile stays. No renderer here: the
// flattener is scripted, and the real bake is proven with the test level's pictures.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleSecondUvHoldTestPrivate
{
	template <typename TExpression>
	TExpression* AddNode(UMaterial* Material)
	{
		TExpression* Expression = NewObject<TExpression>(Material);
		Expression->Material = Material;
		Material->GetExpressionCollection().AddExpression(Expression);
		return Expression;
	}

	/**
	 * Roughness = a mask's red read through Texture Coordinate 1 (U tiling 2), base color = a texture on its own coordinate index 1 with its
	 * UVs unwired: both kinds of second-UV read.
	 */
	UMaterial* NewLaneMaskMaterial()
	{
		UMaterial* Material = NewObject<UMaterial>(GetTransientPackage());
		UTexture2D* Mask = UTexture2D::CreateTransient(4, 4);
		UMaterialExpressionTextureCoordinate* Uv1 = AddNode<UMaterialExpressionTextureCoordinate>(Material);
		Uv1->CoordinateIndex = 1;
		Uv1->UTiling = 2.0f;
		UMaterialExpressionTextureSample* Lanes = AddNode<UMaterialExpressionTextureSample>(Material);
		Lanes->Texture = Mask;
		Lanes->Coordinates.Connect(0, Uv1);
		Material->GetEditorOnlyData()->Roughness.Connect(1, Lanes); // R
		UMaterialExpressionTextureSample* Dirt = AddNode<UMaterialExpressionTextureSample>(Material);
		Dirt->Texture = Mask;
		Dirt->ConstCoordinate = 1;
		Material->GetEditorOnlyData()->BaseColor.Connect(0, Dirt);
		Material->UpdateCachedExpressionData();
		return Material;
	}

	bool ReadsSecondUvNode(const UMaterial* Material)
	{
		for (const TObjectPtr<UMaterialExpression>& Expression : Material->GetExpressions())
		{
			const UMaterialExpressionTextureCoordinate* Coordinate = Cast<UMaterialExpressionTextureCoordinate>(Expression.Get());
			if (Coordinate && Coordinate->CoordinateIndex > 0)
			{
				return true;
			}
			const UMaterialExpressionTextureSample* Sample = Cast<UMaterialExpressionTextureSample>(Expression.Get());
			if (Sample && !Sample->Coordinates.Expression && Sample->ConstCoordinate > 0)
			{
				return true;
			}
		}
		return false;
	}

	/** A one-material document with only factors: what ChooseSecondUvBake compares. */
	FMantleGlbDocument FactorBake(double Roughness, double Gray)
	{
		TArray<TSharedPtr<FJsonValue>> Factor;
		for (const double Channel : { Gray, Gray, Gray, 1.0 })
		{
			Factor.Add(MakeShared<FJsonValueNumber>(Channel));
		}
		const TSharedRef<FJsonObject> Pbr = MakeShared<FJsonObject>();
		Pbr->SetArrayField(TEXT("baseColorFactor"), Factor);
		Pbr->SetNumberField(TEXT("metallicFactor"), 0.0);
		Pbr->SetNumberField(TEXT("roughnessFactor"), Roughness);
		const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("alphaMode"), TEXT("OPAQUE"));
		Entry->SetObjectField(TEXT("pbrMetallicRoughness"), Pbr);
		TArray<TSharedPtr<FJsonValue>> Materials;
		Materials.Add(MakeShared<FJsonValueObject>(Entry));
		FMantleGlbDocument Doc;
		Doc.Json = MakeShared<FJsonObject>();
		Doc.Json->SetArrayField(TEXT("materials"), Materials);
		return Doc;
	}

	/**
	 * Reads where the held copy it is handed holds UV1 (the Constant2Vector feeding the lane mask's coordinates) and bakes the road as a
	 * lane mask would: wet (roughness 0.1) where that point is a tire lane (U within 0.4 to 0.6 of the tiled UV1), dry (0.8) elsewhere.
	 */
	class FLaneFlattener : public IMantleMaterialFlattener
	{
	public:
		int32 Calls = 0;
		TArray<float> HeldU;

		virtual bool Flatten(UMaterialInterface* Material, const FMantleFlattenSettings& Settings, FMantleGlbDocument& OutDocument,
			TArray<FString>& OutMessages, FString& OutError) override
		{
			++Calls;
			const UMaterial* Base = Material ? Material->GetMaterial() : nullptr;
			const UMaterialExpressionTextureSample* Lanes = Base ? Cast<UMaterialExpressionTextureSample>(Base->GetEditorOnlyData()->Roughness.Expression) : nullptr;
			const UMaterialExpressionConstant2Vector* Held = Lanes ? Cast<UMaterialExpressionConstant2Vector>(Lanes->Coordinates.Expression) : nullptr;
			if (!Held)
			{
				OutError = TEXT("the scripted flattener expected the lane mask's UV1 held at a point");
				return false;
			}
			HeldU.Add(Held->R);
			const bool bTireLane = Held->R >= 0.8f && Held->R <= 1.2f; // UV1 U 0.4 to 0.6, times the U tiling of 2
			OutDocument = FactorBake(bTireLane ? 0.1 : 0.8, 0.4);
			return true;
		}

		virtual FString Describe() const override { return TEXT("scripted lane mask"); }
		virtual bool CanBakeTextures() const override { return true; }
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleSecondUvHeldCopyTest,
	"Mantle.Editor.SecondUv.HeldCopyReadsNoSecondUvSet",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleSecondUvHeldCopyTest::RunTest(const FString& Parameters)
{
	using namespace MantleSecondUvHoldTestPrivate;
	UMaterial* Road = NewLaneMaskMaterial();
	TestTrue(TEXT("the material reads a second UV set"), FMantlePlaceAssets::AnalyzeMaterial(Road).Has(EMantleMaterialLoss::SecondUv));

	const TStrongObjectPtr<UMaterialInterface> Copy = FMantlePlaceAssets::SecondUvHeldCopy(Road, FVector2D(0.3, 0.6));
	if (!TestTrue(TEXT("it gets a copy"), Copy.IsValid()) || !TestNotNull(TEXT("with a base"), Copy->GetMaterial()))
	{
		return false;
	}
	const UMaterial* Base = Copy->GetMaterial();
	TestFalse(TEXT("the copy reads no second UV set"), ReadsSecondUvNode(Base));
	const UMaterialExpressionTextureSample* Lanes = Cast<UMaterialExpressionTextureSample>(Base->GetEditorOnlyData()->Roughness.Expression);
	const UMaterialExpressionConstant2Vector* LanesAt = Lanes ? Cast<UMaterialExpressionConstant2Vector>(Lanes->Coordinates.Expression) : nullptr;
	if (TestNotNull(TEXT("the lane mask reads a constant point"), LanesAt))
	{
		TestEqual(TEXT("the point's U, times the coordinate's tiling of 2"), LanesAt->R, 0.6f, 1.0e-5f);
		TestEqual(TEXT("its V"), LanesAt->G, 0.6f, 1.0e-5f);
	}
	const UMaterialExpressionTextureSample* Dirt = Cast<UMaterialExpressionTextureSample>(Base->GetEditorOnlyData()->BaseColor.Expression);
	const UMaterialExpressionConstant2Vector* DirtAt = Dirt ? Cast<UMaterialExpressionConstant2Vector>(Dirt->Coordinates.Expression) : nullptr;
	if (TestNotNull(TEXT("the sample on its own coordinate index is wired to the point"), DirtAt))
	{
		TestEqual(TEXT("U"), DirtAt->R, 0.3f, 1.0e-5f);
		TestEqual(TEXT("V"), DirtAt->G, 0.6f, 1.0e-5f);
	}
	TestFalse(TEXT("the copy has no second-UV loss"), FMantlePlaceAssets::AnalyzeMaterial(Copy.Get()).Has(EMantleMaterialLoss::SecondUv));
	TestTrue(TEXT("the creator's material still reads UV1"), ReadsSecondUvNode(Road));

	UMaterial* Plain = NewObject<UMaterial>(GetTransientPackage());
	TestFalse(TEXT("a material reading no second UV set gets no copy"), FMantlePlaceAssets::SecondUvHeldCopy(Plain, FVector2D(0.5, 0.5)).IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleSecondUvVoteTest,
	"Mantle.Editor.SecondUv.VoteKeepsWhatMostOfTheSurfaceHas",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleSecondUvVoteTest::RunTest(const FString& Parameters)
{
	using namespace MantleSecondUvHoldTestPrivate;
	FString Why;
	{
		// The median fell on a tire lane (wet), the quartiles are dry road: a quartile.
		const FMantleGlbDocument Bakes[3] = { FactorBake(0.8, 0.4), FactorBake(0.1, 0.4), FactorBake(0.8, 0.4) };
		TestEqual(TEXT("the median is the odd one out: a quartile"), FMantlePlaceAssetConverter::ChooseSecondUvBake(Bakes, Why), 0);
	}
	{
		// The median agrees with one quartile: the median.
		const FMantleGlbDocument Bakes[3] = { FactorBake(0.8, 0.4), FactorBake(0.82, 0.4), FactorBake(0.1, 0.4) };
		TestEqual(TEXT("the median agrees with a quartile: the median"), FMantlePlaceAssetConverter::ChooseSecondUvBake(Bakes, Why), 1);
	}
	{
		// The color differs, not the roughness: still a different look.
		const FMantleGlbDocument Bakes[3] = { FactorBake(0.5, 0.2), FactorBake(0.5, 0.9), FactorBake(0.5, 0.2) };
		TestEqual(TEXT("a color that differs counts too"), FMantlePlaceAssetConverter::ChooseSecondUvBake(Bakes, Why), 0);
	}
	{
		// No two agree: the median.
		const FMantleGlbDocument Bakes[3] = { FactorBake(0.2, 0.4), FactorBake(0.5, 0.4), FactorBake(0.8, 0.4) };
		TestEqual(TEXT("no majority: the median"), FMantlePlaceAssetConverter::ChooseSecondUvBake(Bakes, Why), 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleSecondUvConverterTest,
	"Mantle.Editor.SecondUv.ConverterBakesAtTheMeshesUv1",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleSecondUvConverterTest::RunTest(const FString& Parameters)
{
	using namespace MantleSecondUvHoldTestPrivate;

	// The meshes' UV1 runs 0 to 1 across the road: its quartiles are 0.25, 0.5 and 0.75.
	FMantleSurfaceSamples Samples;
	for (int32 Index = 0; Index <= 100; ++Index)
	{
		Samples.SecondUv.Add(FVector2f(Index / 100.0f, 0.5f));
	}
	FVector2D Points[3];
	if (!TestTrue(TEXT("the samples give quartiles"), Samples.SecondUvQuartiles(Points)))
	{
		return false;
	}
	TestEqual(TEXT("25th percentile"), Points[0].X, 0.25, 1.0e-3);
	TestEqual(TEXT("median"), Points[1].X, 0.5, 1.0e-3);
	TestEqual(TEXT("75th percentile"), Points[2].X, 0.75, 1.0e-3);

	FMantleBakeSurface Surface;
	Surface.SecondUvPoints = { Points[0], Points[1], Points[2] };
	TSharedRef<FLaneFlattener> Flattener = MakeShared<FLaneFlattener>();
	FMantlePlaceAssetConverter Converter(Flattener, FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("MantleSecondUvHold")));
	UMaterial* Road = NewLaneMaskMaterial();
	FMantlePlaceAssetSet Set;
	if (!TestTrue(TEXT("the road converts"), Converter.ConvertMaterial(Road, Surface, Set)))
	{
		return false;
	}
	TestEqual(TEXT("it was baked three times, once at each point"), Flattener->Calls, 3);
	const FMantleConvertedMaterial* Converted = Set.FindMaterial(FMantlePlaceAssets::MaterialKey(Road));
	if (TestNotNull(TEXT("the road is in the set"), Converted))
	{
		// The median (U 0.5, tiled 1.0) is a tire lane; the quartiles (0.5 and 1.5 tiled) are dry road, as most of the surface is.
		TestEqual(TEXT("it publishes dry: the quartile's roughness"), Converted->Document.RoughnessFactor, 0.8, 1.0e-4);
	}

	// A material that reads no second UV set keeps one plain bake, whatever points the surface carries.
	TSharedRef<FLaneFlattener> Plain = MakeShared<FLaneFlattener>();
	FMantlePlaceAssetConverter PlainConverter(Plain, FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("MantleSecondUvPlain")));
	FMantlePlaceAssetSet PlainSet;
	PlainConverter.ConvertMaterial(NewObject<UMaterial>(GetTransientPackage()), Surface, PlainSet);
	TestEqual(TEXT("a plain material is baked once"), Plain->Calls, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleSecondUvSamplesEvenTest,
	"Mantle.Editor.SecondUv.SamplesCountEarlyAndLateMeshesAlike",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleSecondUvSamplesEvenTest::RunTest(const FString& Parameters)
{
	// Many meshes' points, more than are kept: those gathered first must count as much as those gathered last, or the percentiles lean
	// toward the last ones. Half the points offered are at U 0 (offered first), half at U 1 (offered last), one mesh at a time.
	FMantleSurfaceSamples Material;
	const int32 PerMesh = 1000;
	const int32 Meshes = (4 * FMantleSurfaceSamples::MaxSecondUv) / PerMesh;
	for (int32 Mesh = 0; Mesh < Meshes; ++Mesh)
	{
		FMantleSurfaceSamples One;
		for (int32 Index = 0; Index < PerMesh; ++Index)
		{
			One.AddSecondUv(FVector2f(Mesh < Meshes / 2 ? 0.0f : 1.0f, 0.5f));
		}
		Material.Append(One);
	}
	TestEqual(TEXT("it keeps the most it may"), Material.SecondUv.Num(), FMantleSurfaceSamples::MaxSecondUv);
	int32 Early = 0;
	for (const FVector2f& Point : Material.SecondUv)
	{
		Early += Point.X == 0.0f ? 1 : 0;
	}
	const double Share = static_cast<double>(Early) / Material.SecondUv.Num();
	TestTrue(*FString::Printf(TEXT("the early half keeps about half the points (%.3f)"), Share), Share > 0.45 && Share < 0.55);

	// The same points offered again give the same sample: the same level gives the same cache key.
	FMantleSurfaceSamples Again;
	for (int32 Mesh = 0; Mesh < Meshes; ++Mesh)
	{
		FMantleSurfaceSamples One;
		for (int32 Index = 0; Index < PerMesh; ++Index)
		{
			One.AddSecondUv(FVector2f(Mesh < Meshes / 2 ? 0.0f : 1.0f, 0.5f));
		}
		Again.Append(One);
	}
	TestTrue(TEXT("the same every time"), Again.SecondUv == Material.SecondUv);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
