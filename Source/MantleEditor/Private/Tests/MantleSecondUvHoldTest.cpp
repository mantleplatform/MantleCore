// Copyright Joshua Eger. All Rights Reserved.
#include "MantleGlbWriter.h"
#include "MantlePlaceAssetConverter.h"
#include "MantlePlaceAssets.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionConstant2Vector.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "ImageUtils.h"
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

	/**
	 * Whether anything the material's Roughness or Base Color reads (however deep) reads a second UV set: a node replaced in a bake copy
	 * stays in its graph, read by nothing, so it is what the outputs reach that counts.
	 */
	bool ReadsSecondUvNode(const UMaterial* Material)
	{
		TArray<UMaterialExpression*> Open = { Material->GetEditorOnlyData()->Roughness.Expression, Material->GetEditorOnlyData()->BaseColor.Expression };
		TSet<UMaterialExpression*> Seen;
		while (Open.Num() > 0)
		{
			UMaterialExpression* Expression = Open.Pop();
			if (!Expression || Seen.Contains(Expression))
			{
				continue;
			}
			Seen.Add(Expression);
			const UMaterialExpressionTextureCoordinate* Coordinate = Cast<UMaterialExpressionTextureCoordinate>(Expression);
			if (Coordinate && Coordinate->CoordinateIndex > 0)
			{
				return true;
			}
			const UMaterialExpressionTextureSample* Sample = Cast<UMaterialExpressionTextureSample>(Expression);
			if (Sample && !Sample->Coordinates.Expression && Sample->ConstCoordinate > 0)
			{
				return true;
			}
			for (FExpressionInputIterator It{ Expression }; It; ++It)
			{
				Open.Add(It.Input->Expression);
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

	/** A one-material opaque document whose base color is a 2x2 image of one gray: what the world-tile check reads. */
	FMantleGlbDocument GrayImageBake(uint8 Gray)
	{
		TArray<FColor> Pixels;
		Pixels.Init(FColor(Gray, Gray, Gray, 255), 4);
		TArray64<uint8> Png;
		FImageUtils::PNGCompressImageArray(2, 2, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
		FMantleGlbDocument Doc = FactorBake(0.8, 1.0);
		Doc.Bin = TArray<uint8>(Png.GetData(), static_cast<int32>(Png.Num()));
		auto One = [](const TSharedRef<FJsonObject>& Only)
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			Values.Add(MakeShared<FJsonValueObject>(Only));
			return Values;
		};
		const TSharedRef<FJsonObject> View = MakeShared<FJsonObject>();
		View->SetNumberField(TEXT("buffer"), 0);
		View->SetNumberField(TEXT("byteOffset"), 0);
		View->SetNumberField(TEXT("byteLength"), static_cast<double>(Png.Num()));
		Doc.Json->SetArrayField(TEXT("bufferViews"), One(View));
		const TSharedRef<FJsonObject> Image = MakeShared<FJsonObject>();
		Image->SetNumberField(TEXT("bufferView"), 0);
		Image->SetStringField(TEXT("mimeType"), TEXT("image/png"));
		Doc.Json->SetArrayField(TEXT("images"), One(Image));
		const TSharedRef<FJsonObject> Texture = MakeShared<FJsonObject>();
		Texture->SetNumberField(TEXT("source"), 0);
		Doc.Json->SetArrayField(TEXT("textures"), One(Texture));
		const TSharedRef<FJsonObject> BaseColor = MakeShared<FJsonObject>();
		BaseColor->SetNumberField(TEXT("index"), 0);
		const TArray<TSharedPtr<FJsonValue>>* Materials = nullptr;
		Doc.Json->TryGetArrayField(TEXT("materials"), Materials);
		(*Materials)[0]->AsObject()->GetObjectField(TEXT("pbrMetallicRoughness"))->SetObjectField(TEXT("baseColorTexture"), BaseColor);
		return Doc;
	}

	/**
	 * A road whose look varies by world position as well as UV1: the material as it is changes when its card moves, but its UV1-held copy
	 * does not (holding UV1 holds the part that varied). Gives a gray image: 100 on the card where it is, 230 on the moved card for the
	 * material as it is, 100 everywhere for a held copy.
	 */
	class FWorldRoadFlattener : public IMantleMaterialFlattener
	{
	public:
		virtual bool Flatten(UMaterialInterface* Material, const FMantleFlattenSettings& Settings, FMantleGlbDocument& OutDocument,
			TArray<FString>& OutMessages, FString& OutError) override
		{
			const UMaterial* Base = Material ? Material->GetMaterial() : nullptr;
			const UMaterialExpressionTextureSample* Lanes = Base ? Cast<UMaterialExpressionTextureSample>(Base->GetEditorOnlyData()->Roughness.Expression) : nullptr;
			const bool bHeld = Lanes && Cast<UMaterialExpressionConstant2Vector>(Lanes->Coordinates.Expression) != nullptr;
			const bool bMoved = Settings.Surface.WorldShiftCm > 0.0;
			OutDocument = GrayImageBake(!bHeld && bMoved ? 230 : 100);
			return true;
		}

		virtual FString Describe() const override { return TEXT("scripted world road"); }
		virtual bool CanBakeTextures() const override { return true; }
	};

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleSecondUvWorldTileTest,
	"Mantle.Editor.SecondUv.WorldTileIsJudgedBeforeTheHold",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleSecondUvWorldTileTest::RunTest(const FString& Parameters)
{
	using namespace MantleSecondUvHoldTestPrivate;
	// The lane-mask road, with world position in its base color as well (City Sample's puddles and breakup).
	UMaterial* Road = NewLaneMaskMaterial();
	UMaterialExpressionWorldPosition* World = AddNode<UMaterialExpressionWorldPosition>(Road);
	UMaterialExpressionMultiply* Breakup = AddNode<UMaterialExpressionMultiply>(Road);
	Breakup->A.Connect(0, Road->GetEditorOnlyData()->BaseColor.Expression);
	Breakup->B.Connect(0, World);
	Road->GetEditorOnlyData()->BaseColor.Connect(0, Breakup);
	Road->UpdateCachedExpressionData();
	const FMantleMaterialAnalysis Analysis = FMantlePlaceAssets::AnalyzeMaterial(Road);
	if (!TestTrue(TEXT("it reads a second UV set"), Analysis.Has(EMantleMaterialLoss::SecondUv))
		|| !TestTrue(TEXT("and world position"), Analysis.Has(EMantleMaterialLoss::WorldAligned)))
	{
		return false;
	}

	FMantleBakeSurface Surface;
	Surface.SecondUvPoints = { FVector2D(0.25, 0.5), FVector2D(0.5, 0.5), FVector2D(0.75, 0.5) };
	TSharedRef<FWorldRoadFlattener> Flattener = MakeShared<FWorldRoadFlattener>();
	FMantlePlaceAssetConverter Converter(Flattener, FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("MantleSecondUvWorldTile")));
	FMantlePlaceAssetSet Set;
	if (!TestTrue(TEXT("the road converts"), Converter.ConvertMaterial(Road, Surface, Set)))
	{
		return false;
	}
	const FMantleConvertedMaterial* Converted = Set.FindMaterial(FMantlePlaceAssets::MaterialKey(Road));
	if (TestNotNull(TEXT("the road is in the set"), Converted))
	{
		// Judged on the held copy, it would not move with its card and would leave its world tile for the mesh's UVs (visible squares).
		TestTrue(TEXT("it keeps its world tile: judged on the material before UV1 is held"), Converted->Document.bWorldAligned);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleSecondUvCacheTagTest,
	"Mantle.Editor.SecondUv.WorldTileJudgementIsInTheCacheKey",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleSecondUvCacheTagTest::RunTest(const FString& Parameters)
{
	// Build 3 reused build 2's bake of the road: the world tile is now judged before the hold, but its key (the same held points) had not
	// changed. A held material judged for a world tile says so in its key; every other material keeps the key it had.
	FMantleBakeSurface Held;
	Held.SecondUvPoints = { FVector2D(0.25, 0.5), FVector2D(0.5, 0.5), FVector2D(0.75, 0.5) };
	Held.WorldTileCm = FMantleBakeSurface::DefaultWorldTileCm;
	TestTrue(TEXT("a held, world-judged material's key names the judgement"), Held.CacheTag().Contains(TEXT("worldtile=beforehold")));

	FMantleBakeSurface WorldOnly;
	WorldOnly.WorldTileCm = FMantleBakeSurface::DefaultWorldTileCm;
	TestFalse(TEXT("a world-mapped material that reads no second UV set keeps its key"), WorldOnly.CacheTag().Contains(TEXT("beforehold")));

	FMantleBakeSurface HeldOnly;
	HeldOnly.SecondUvPoints = Held.SecondUvPoints;
	TestFalse(TEXT("a held material with no world tile to judge keeps its key"), HeldOnly.CacheTag().Contains(TEXT("beforehold")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
