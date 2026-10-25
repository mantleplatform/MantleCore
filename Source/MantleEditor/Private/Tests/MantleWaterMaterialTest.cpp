// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCoverage.h"
#include "MantleGlbWriter.h"
#include "MantlePlaceAssetConverter.h"
#include "MantlePlaceAssets.h"
#include "MantlePng.h"
#include "MantleTargetFeatures.h"
#include "MantleWaterMaterial.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant2Vector.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionDepthFade.h"
#include "Materials/MaterialExpressionDivide.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionPanner.h"
#include "Materials/MaterialExpressionPixelDepth.h"
#include "Materials/MaterialExpressionSaturate.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSetMaterialAttributes.h"
#include "Materials/MaterialExpressionSceneDepth.h"
#include "Materials/MaterialExpressionSingleLayerWaterMaterialOutput.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionTime.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// Depth-faded translucent water: GLTFExporter bakes the fade's near end (opacity 0), so the graph's far end is read
// instead. The materials here are small graphs built in code, so no level or asset is needed.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleWaterMaterialTestPrivate
{
	template <typename TExpression>
	TExpression* AddExpression(UMaterial* Material)
	{
		TExpression* Expression = NewObject<TExpression>(Material);
		Expression->Material = Material;
		Material->GetExpressionCollection().AddExpression(Expression);
		return Expression;
	}

	/** A node in a material or in a material function, so one builder fills either. */
	template <typename TExpression>
	TExpression* AddNode(UMaterial* Material)
	{
		return AddExpression<TExpression>(Material);
	}

	template <typename TExpression>
	TExpression* AddNode(UMaterialFunction* Function)
	{
		TExpression* Expression = NewObject<TExpression>(Function);
		Function->GetExpressionCollection().AddExpression(Expression);
		return Expression;
	}

	template <typename TOwner>
	UMaterialExpressionConstant* AddNumber(TOwner* Owner, const float Value)
	{
		UMaterialExpressionConstant* Constant = AddNode<UMaterialExpressionConstant>(Owner);
		Constant->R = Value;
		return Constant;
	}

	UMaterialExpressionConstant3Vector* AddColour(UMaterial* Material, const FLinearColor& Colour)
	{
		UMaterialExpressionConstant3Vector* Constant = AddExpression<UMaterialExpressionConstant3Vector>(Material);
		Constant->Constant = Colour;
		return Constant;
	}

	UMaterial* NewTranslucent()
	{
		UMaterial* Material = NewObject<UMaterial>(GetTransientPackage());
		Material->BlendMode = BLEND_Translucent;
		return Material;
	}

	/** GLTFExporter's bake of a translucent water material: a constant teal with opacity 0, the fade's near end. */
	FMantleGlbDocument BakedWater(const double Roughness = 0.01, const double Metallic = 0.0, const TArray<double>* Emissive = nullptr)
	{
		const TSharedRef<FJsonObject> Pbr = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Factor;
		for (const double Value : { 0.0, 0.024158, 0.025187, 0.0 })
		{
			Factor.Add(MakeShared<FJsonValueNumber>(Value));
		}
		Pbr->SetArrayField(TEXT("baseColorFactor"), Factor);
		Pbr->SetNumberField(TEXT("metallicFactor"), Metallic);
		Pbr->SetNumberField(TEXT("roughnessFactor"), Roughness);
		const TSharedRef<FJsonObject> Baked = MakeShared<FJsonObject>();
		Baked->SetStringField(TEXT("alphaMode"), TEXT("BLEND"));
		Baked->SetObjectField(TEXT("pbrMetallicRoughness"), Pbr);
		if (Emissive)
		{
			TArray<TSharedPtr<FJsonValue>> Glow;
			for (const double Value : *Emissive)
			{
				Glow.Add(MakeShared<FJsonValueNumber>(Value));
			}
			Baked->SetArrayField(TEXT("emissiveFactor"), Glow);
		}
		TArray<TSharedPtr<FJsonValue>> Materials;
		Materials.Add(MakeShared<FJsonValueObject>(Baked));
		FMantleGlbDocument Document;
		Document.Json = MakeShared<FJsonObject>();
		Document.Json->SetArrayField(TEXT("materials"), Materials);
		return Document;
	}

	/** A normal-map sample whose UVs are Texture Coordinate 0 times Tiling, panned by Speed (a vector times a scalar, as the Spruce Forest water does it). */
	UMaterialExpressionTextureSample* AddPannedNormal(UMaterial* Material, UTexture* Texture, const float Tiling, const FLinearColor& Direction, const float Speed)
	{
		UMaterialExpressionMultiply* Scaled = AddExpression<UMaterialExpressionMultiply>(Material);
		Scaled->A.Connect(0, AddExpression<UMaterialExpressionTextureCoordinate>(Material));
		Scaled->B.Connect(0, AddNumber(Material, Tiling));
		UMaterialExpressionMultiply* Velocity = AddExpression<UMaterialExpressionMultiply>(Material);
		Velocity->A.Connect(0, AddColour(Material, Direction));
		Velocity->B.Connect(0, AddNumber(Material, Speed));
		UMaterialExpressionPanner* Panner = AddExpression<UMaterialExpressionPanner>(Material);
		Panner->Coordinate.Connect(0, Scaled);
		Panner->Speed.Connect(0, Velocity);
		UMaterialExpressionTextureSample* Sample = AddExpression<UMaterialExpressionTextureSample>(Material);
		Sample->Texture = Texture;
		Sample->Coordinates.Connect(0, Panner);
		return Sample;
	}

	/** A Depth Fade whose opacity and fade distance are the constants given. */
	UMaterialExpressionDepthFade* AddDepthFadeAt(UMaterial* Material, const float Opacity, const float DistanceCm)
	{
		UMaterialExpressionDepthFade* Fade = AddExpression<UMaterialExpressionDepthFade>(Material);
		Fade->InOpacity.Connect(0, AddNumber(Material, Opacity));
		Fade->FadeDistance.Connect(0, AddNumber(Material, DistanceCm));
		return Fade;
	}

	/**
	 * The UVs of the Spruce Forest lake's ripples (MF_Wind_Ripples): World Position XY over a size in centimetres on each axis, plus
	 * Time times a pan in repeats a second.
	 */
	template <typename TOwner>
	UMaterialExpression* AddWorldUvs(TOwner* Owner, const FVector2D& SizeCm, const FVector2D& Pan)
	{
		UMaterialExpressionComponentMask* Xy = AddNode<UMaterialExpressionComponentMask>(Owner);
		Xy->R = true;
		Xy->G = true;
		Xy->B = false;
		Xy->A = false;
		Xy->Input.Connect(0, AddNode<UMaterialExpressionWorldPosition>(Owner));
		UMaterialExpressionConstant2Vector* Size = AddNode<UMaterialExpressionConstant2Vector>(Owner);
		Size->R = static_cast<float>(SizeCm.X);
		Size->G = static_cast<float>(SizeCm.Y);
		UMaterialExpressionDivide* Over = AddNode<UMaterialExpressionDivide>(Owner);
		Over->A.Connect(0, Xy);
		Over->B.Connect(0, Size);
		UMaterialExpressionConstant2Vector* Velocity = AddNode<UMaterialExpressionConstant2Vector>(Owner);
		Velocity->R = static_cast<float>(Pan.X);
		Velocity->G = static_cast<float>(Pan.Y);
		UMaterialExpressionMultiply* Moved = AddNode<UMaterialExpressionMultiply>(Owner);
		Moved->A.Connect(0, AddNode<UMaterialExpressionTime>(Owner));
		Moved->B.Connect(0, Velocity);
		UMaterialExpressionAdd* Sum = AddNode<UMaterialExpressionAdd>(Owner);
		Sum->A.Connect(0, Over);
		Sum->B.Connect(0, Moved);
		return Sum;
	}

	/** A normal-map sample laid out by world position (AddWorldUvs). */
	UMaterialExpressionTextureSample* AddWorldNormal(UMaterial* Material, UTexture* Texture, const FVector2D& SizeCm, const FVector2D& Pan)
	{
		UMaterialExpressionTextureSample* Sample = AddExpression<UMaterialExpressionTextureSample>(Material);
		Sample->Texture = Texture;
		Sample->Coordinates.Connect(0, AddWorldUvs(Material, SizeCm, Pan));
		return Sample;
	}

	/** The Spruce lake's MF_Wind_Ripples, cut down: a normal sample laid out by world position, times a constant strength, as the function's output. */
	UMaterialFunction* MakeRipplesFunction(UTexture* Texture, const FVector2D& SizeCm, const FVector2D& Pan, const float Strength)
	{
		UMaterialFunction* Function = NewObject<UMaterialFunction>(GetTransientPackage());
		UMaterialExpressionTextureSample* Sample = AddNode<UMaterialExpressionTextureSample>(Function);
		Sample->Texture = Texture;
		Sample->Coordinates.Connect(0, AddWorldUvs(Function, SizeCm, Pan));
		UMaterialExpressionMultiply* Scaled = AddNode<UMaterialExpressionMultiply>(Function);
		Scaled->A.Connect(0, AddNumber(Function, Strength));
		Scaled->B.Connect(0, Sample);
		UMaterialExpressionFunctionOutput* Output = AddNode<UMaterialExpressionFunctionOutput>(Function);
		Output->OutputName = TEXT("Normal");
		Output->Id = FGuid::NewGuid();
		Output->A.Connect(0, Scaled);
		return Function;
	}

	/** A call of a function whose inputs are named: BlendAngleCorrectedNormals' BaseNormal and AdditionalNormal, FlattenNormal's Normal and Flatness. */
	UMaterialExpressionMaterialFunctionCall* AddCall(UMaterial* Material, UMaterialFunction* Function, const TCHAR* FirstName, UMaterialExpression* First, const TCHAR* SecondName, UMaterialExpression* Second)
	{
		UMaterialExpressionMaterialFunctionCall* Call = AddExpression<UMaterialExpressionMaterialFunctionCall>(Material);
		Call->SetMaterialFunction(Function);
		for (FFunctionExpressionInput& Input : Call->FunctionInputs)
		{
			if (FirstName && First && Input.Input.InputName == FName(FirstName))
			{
				Input.Input.Connect(0, First);
			}
			else if (SecondName && Second && Input.Input.InputName == FName(SecondName))
			{
				Input.Input.Connect(0, Second);
			}
		}
		return Call;
	}

	UMaterialFunction* LoadEngineFunction(const TCHAR* Path)
	{
		return LoadObject<UMaterialFunction>(nullptr, Path);
	}

	/** Near-opaque, depth-faded water: opacity 0.96 over the fade distance given. */
	UMaterial* NewWater(const float FadeDistanceCm)
	{
		UMaterial* Material = NewTranslucent();
		Material->GetEditorOnlyData()->Opacity.Connect(0, AddDepthFadeAt(Material, 0.96f, FadeDistanceCm));
		return Material;
	}

	/**
	 * The Spruce river: near-opaque water (fade distance 3 cm) whose Normal is BlendAngleCorrectedNormals of two panned samples of Normal,
	 * tiling 0.25 panned (-0.0025, 0) and tiling 0.5 panned (-0.0125, 0). Null when the engine function is not there.
	 */
	UMaterial* NewRiver(UTexture* Normal, const float Opacity = 0.96f)
	{
		UMaterialFunction* Blend = LoadEngineFunction(TEXT("/Engine/Functions/Engine_MaterialFunctions02/Utility/BlendAngleCorrectedNormals.BlendAngleCorrectedNormals"));
		if (!Blend)
		{
			return nullptr;
		}
		UMaterial* River = NewTranslucent();
		River->GetEditorOnlyData()->Opacity.Connect(0, AddDepthFadeAt(River, Opacity, 3.0f));
		River->GetEditorOnlyData()->Normal.Connect(0, AddCall(River, Blend, TEXT("BaseNormal"), AddPannedNormal(River, Normal, 0.25f, FLinearColor(-0.5f, 0.0f, -0.5f), 0.005f),
			TEXT("AdditionalNormal"), AddPannedNormal(River, Normal, 0.5f, FLinearColor(-2.5f, 0.0f, -0.5f), 0.005f)));
		return River;
	}

	/** A small PNG, the source image of a normal map. */
	TArray<uint8> NormalPng()
	{
		TArray<FColor> Pixels;
		Pixels.Init(FColor(128, 128, 255, 255), 16);
		Pixels[5] = FColor(140, 120, 250, 255);
		return FMantlePng::EncodeRGBA8(4, 4, Pixels);
	}

	/** An uploaded asset id for the nth texture. */
	FString FakeId(const int32 Number)
	{
		return FString::Printf(TEXT("ast_u%032x"), Number);
	}

	TArray<double> Numbers(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		TArray<double> Out;
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (Object.IsValid() && Object->TryGetArrayField(Field, Values))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				double Number = 0.0;
				if (Value.IsValid() && Value->TryGetNumber(Number))
				{
					Out.Add(Number);
				}
			}
		}
		return Out;
	}

	TSharedPtr<FJsonObject> ObjectOf(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		const TSharedPtr<FJsonObject>* Found = nullptr;
		return Object.IsValid() && Object->TryGetObjectField(Field, Found) && Found ? *Found : TSharedPtr<FJsonObject>();
	}

	bool ParseJsonText(const FString& Text, TSharedPtr<FJsonObject>& Out)
	{
		return FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Out) && Out.IsValid();
	}

	/** Whether Values (a JSON array read as numbers) is Expected, each within a tolerance. */
	bool NumbersMatch(const TArray<double>& Values, std::initializer_list<double> Expected, const double Tolerance)
	{
		if (Values.Num() != static_cast<int32>(Expected.size()))
		{
			return false;
		}
		int32 Index = 0;
		for (const double Want : Expected)
		{
			if (FMath::Abs(Values[Index++] - Want) > Tolerance)
			{
				return false;
			}
		}
		return true;
	}

	/**
	 * What the converter makes of the river: the analysis of NewRiver and MaterialFromGltf over GLTFExporter's bake (a constant teal),
	 * with the source normal image supplied for both layers (the converter reads them from the textures; a transient texture has no source).
	 * False when the engine function the river needs is not there.
	 */
	bool ConvertRiver(FAutomationTestBase& Test, FMantlePlaceAssetSet& Set, FMantlePlaceMaterialDocument& OutDocument)
	{
		UTexture2D* Normal = UTexture2D::CreateTransient(4, 4);
		UMaterial* River = NewRiver(Normal);
		if (!River)
		{
			Test.AddWarning(TEXT("BlendAngleCorrectedNormals could not be loaded; the river case is skipped."));
			return false;
		}
		const FMantleMaterialAnalysis Analysis = FMantlePlaceAssets::AnalyzeMaterial(River);
		FMantleMaterialExtras Extras;
		Extras.Fidelity = Analysis.Fidelity2;
		Extras.SourceImages[1] = NormalPng();
		Extras.SourceNames[1] = TEXT("T_Water_Normal");
		Extras.SourceImages[7] = Extras.SourceImages[1];
		Extras.SourceNames[7] = TEXT("T_Water_Normal");
		OutDocument = FMantlePlaceAssetConverter::MaterialFromGltf(BakedWater(), 0, TEXT("MI_River"), FMantlePlaceAssetLimits(), false, Set, Extras);
		return true;
	}

	FString WaterBytesText(const TArray<uint8>& Bytes)
	{
		const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
		return FString(Converted.Length(), Converted.Get());
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWaterDepthFadeOpacityTest,
	"Mantle.Editor.Water.DepthFadeOpacity",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWaterDepthFadeOpacityTest::RunTest(const FString& Parameters)
{
	using namespace MantleWaterMaterialTestPrivate;

	// A Depth Fade (opacity 0.85) into Opacity, and a base colour that is a Lerp from a shallow to a deep colour by that fade.
	UMaterial* Material = NewTranslucent();
	UMaterialExpressionDepthFade* Fade = AddDepthFadeAt(Material, 0.85f, 100.0f);
	UMaterialExpressionDepthFade* ColourFade = AddDepthFadeAt(Material, 1.0f, 100.0f);
	UMaterialExpressionLinearInterpolate* Colour = AddExpression<UMaterialExpressionLinearInterpolate>(Material);
	Colour->A.Connect(0, AddColour(Material, FLinearColor(0.0f, 0.30f, 0.30f)));
	Colour->B.Connect(0, AddColour(Material, FLinearColor(0.0f, 0.02f, 0.03f)));
	Colour->Alpha.Connect(0, ColourFade);
	Material->GetEditorOnlyData()->Opacity.Connect(0, Fade);
	Material->GetEditorOnlyData()->BaseColor.Connect(0, Colour);

	const FMantleMaterialAnalysis Analysis = FMantlePlaceAssets::AnalyzeMaterial(Material);
	const FMantleWaterBake& Water = Analysis.Fidelity2.Water;
	if (!TestTrue(TEXT("the depth fade is found"), Water.bFound))
	{
		return false;
	}
	TestTrue(TEXT("the opacity is read from the graph"), Water.bOpacityRead);
	TestEqual(TEXT("it is the far opacity, not the near end's 0"), Water.Opacity, 0.85, 1.0e-4);
	if (TestTrue(TEXT("the deep colour is read"), Water.bColour))
	{
		TestEqual(TEXT("deep green"), Water.Colour[1], 0.02, 1.0e-4);
		TestEqual(TEXT("deep blue"), Water.Colour[2], 0.03, 1.0e-4);
	}
	TestFalse(TEXT("nothing about the fade is a loss"), Analysis.Has(EMantleMaterialLoss::Animation));

	// The document: GLTFExporter's bake of the same material has opacity 0.
	const FMantleGlbDocument Flattened = BakedWater(0.2);

	FMantleMaterialExtras Extras;
	Extras.Fidelity = Analysis.Fidelity2;
	FMantlePlaceAssetSet Set;
	const FMantlePlaceMaterialDocument Doc = FMantlePlaceAssetConverter::MaterialFromGltf(Flattened, 0, TEXT("MI_Water"), FMantlePlaceAssetLimits(), false, Set, Extras);
	TestEqual(TEXT("it stays translucent"), Doc.AlphaMode, FString(TEXT("blend")));
	TestEqual(TEXT("the exported opacity is the far one"), Doc.BaseColorFactor[3], 0.85, 1.0e-4);
	TestEqual(TEXT("the exported colour is the deep one (green)"), Doc.BaseColorFactor[1], 0.02, 1.0e-4);
	TestEqual(TEXT("the exported colour is the deep one (blue)"), Doc.BaseColorFactor[2], 0.03, 1.0e-4);

	// Without the extras the bake's opacity stands: the fix is the recognition, not a change to every translucent material.
	const FMantlePlaceMaterialDocument Plain = FMantlePlaceAssetConverter::MaterialFromGltf(Flattened, 0, TEXT("MI_Plain"), FMantlePlaceAssetLimits(), false, Set);
	TestEqual(TEXT("a material that is not depth-faded keeps the bake's opacity"), Plain.BaseColorFactor[3], 0.0, 1.0e-6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWaterSceneDepthOpacityTest,
	"Mantle.Editor.Water.SceneDepthOpacity",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWaterSceneDepthOpacityTest::RunTest(const FString& Parameters)
{
	using namespace MantleWaterMaterialTestPrivate;

	// Opacity = Saturate((SceneDepth - PixelDepth) / 200) * 0.6: far from the surface it is 0.6, at it 0.
	UMaterial* Material = NewTranslucent();
	UMaterialExpressionSubtract* Gap = AddExpression<UMaterialExpressionSubtract>(Material);
	Gap->A.Connect(0, AddExpression<UMaterialExpressionSceneDepth>(Material));
	Gap->B.Connect(0, AddExpression<UMaterialExpressionPixelDepth>(Material));
	UMaterialExpressionDivide* Scaled = AddExpression<UMaterialExpressionDivide>(Material);
	Scaled->A.Connect(0, Gap);
	Scaled->B.Connect(0, AddNumber(Material, 200.0f));
	UMaterialExpressionSaturate* Fade = AddExpression<UMaterialExpressionSaturate>(Material);
	Fade->Input.Connect(0, Scaled);
	UMaterialExpressionMultiply* Opacity = AddExpression<UMaterialExpressionMultiply>(Material);
	Opacity->A.Connect(0, Fade);
	Opacity->B.Connect(0, AddNumber(Material, 0.6f));
	Material->GetEditorOnlyData()->Opacity.Connect(0, Opacity);

	FMantleWaterBake Water;
	TestTrue(TEXT("a Scene Depth fade is found"), FMantleWaterMaterial::Recognise(Material, &Material->GetEditorOnlyData()->Opacity, nullptr, nullptr, Water));
	TestTrue(TEXT("and read"), Water.bOpacityRead);
	TestEqual(TEXT("its far opacity is the multiplier"), Water.Opacity, 0.6, 1.0e-4);
	TestFalse(TEXT("no colour was asked for"), Water.bColour);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWaterEstimatedOpacityTest,
	"Mantle.Editor.Water.EstimatedOpacity",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWaterEstimatedOpacityTest::RunTest(const FString& Parameters)
{
	using namespace MantleWaterMaterialTestPrivate;

	// A depth fade multiplied by a texture's alpha cannot be evaluated: the estimate stands in, and the report says so.
	UMaterial* Material = NewTranslucent();
	UMaterialExpressionTextureSample* Sample = AddExpression<UMaterialExpressionTextureSample>(Material);
	Sample->Texture = UTexture2D::CreateTransient(4, 4);
	UMaterialExpressionMultiply* Opacity = AddExpression<UMaterialExpressionMultiply>(Material);
	Opacity->A.Connect(0, AddDepthFadeAt(Material, 1.0f, 100.0f));
	Opacity->B.Connect(0, Sample);
	Material->GetEditorOnlyData()->Opacity.Connect(0, Opacity);

	FMantleWaterBake Water;
	TestTrue(TEXT("the fade is found"), FMantleWaterMaterial::Recognise(Material, &Material->GetEditorOnlyData()->Opacity, nullptr, nullptr, Water));
	TestFalse(TEXT("the opacity could not be read"), Water.bOpacityRead);
	TestEqual(TEXT("so it is the estimate"), Water.Opacity, FMantleWaterMaterial::EstimatedOpacity, 1.0e-6);
	TestFalse(TEXT("and the reason is kept"), Water.Why.IsEmpty());

	// Opacity with no depth in it at all is not water.
	UMaterial* Plain = NewTranslucent();
	Plain->GetEditorOnlyData()->Opacity.Connect(0, AddNumber(Plain, 0.4f));
	FMantleWaterBake None;
	TestFalse(TEXT("a constant opacity is not a depth fade"), FMantleWaterMaterial::Recognise(Plain, &Plain->GetEditorOnlyData()->Opacity, nullptr, nullptr, None));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWaterNearOpaqueTest,
	"Mantle.Editor.Water.NearOpaquePublishesOpaque",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWaterNearOpaqueTest::RunTest(const FString& Parameters)
{
	using namespace MantleWaterMaterialTestPrivate;

	// A far opacity of 0.96 publishes opaque (so it reflects), keeping the bake's colour and roughness; 0.85 stays translucent.
	for (const float Opacity : { 0.96f, 0.85f })
	{
		UMaterial* Material = NewTranslucent();
		Material->GetEditorOnlyData()->Opacity.Connect(0, AddDepthFadeAt(Material, Opacity, 100.0f));
		const FMantleMaterialAnalysis Analysis = FMantlePlaceAssets::AnalyzeMaterial(Material);
		FMantleMaterialExtras Extras;
		Extras.Fidelity = Analysis.Fidelity2;
		FMantlePlaceAssetSet Set;
		const FMantlePlaceMaterialDocument Doc = FMantlePlaceAssetConverter::MaterialFromGltf(BakedWater(), 0, TEXT("MI_Water"), FMantlePlaceAssetLimits(), false, Set, Extras);
		if (Opacity > 0.9f)
		{
			TestTrue(TEXT("near-opaque water is flagged to publish opaque"), Analysis.Fidelity2.Water.PublishesOpaque());
			TestEqual(TEXT("it is opaque"), Doc.AlphaMode, FString(TEXT("opaque")));
			TestEqual(TEXT("with full alpha"), Doc.BaseColorFactor[3], 1.0, 1.0e-6);
		}
		else
		{
			TestFalse(TEXT("lower opacity is not"), Analysis.Fidelity2.Water.PublishesOpaque());
			TestEqual(TEXT("it stays translucent"), Doc.AlphaMode, FString(TEXT("blend")));
			TestEqual(TEXT("with the far opacity"), Doc.BaseColorFactor[3], static_cast<double>(Opacity), 1.0e-4);
		}
		TestEqual(TEXT("the bake's colour stands"), Doc.BaseColorFactor[1], 0.024158, 1.0e-6);
		TestEqual(TEXT("the bake's roughness stands"), Doc.RoughnessFactor, 0.01, 1.0e-6);
		TestEqual(TEXT("metallic is 0"), Doc.MetallicFactor, 0.0, 1.0e-6);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWaterPannedNormalTest,
	"Mantle.Editor.Water.PannedNormal",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWaterPannedNormalTest::RunTest(const FString& Parameters)
{
	using namespace MantleWaterMaterialTestPrivate;

	UTexture2D* Normal = UTexture2D::CreateTransient(4, 4);

	// One Panner into a normal sample: Normal = sample(UV x 0.25, panned by (-0.5, 0, -0.5) x 0.005 = (-0.0025, 0)).
	UMaterial* Single = NewTranslucent();
	Single->GetEditorOnlyData()->Opacity.Connect(0, AddDepthFadeAt(Single, 0.96f, 100.0f));
	Single->GetEditorOnlyData()->Normal.Connect(0, AddPannedNormal(Single, Normal, 0.25f, FLinearColor(-0.5f, 0.0f, -0.5f), 0.005f));
	const FMantleMaterialAnalysis One = FMantlePlaceAssets::AnalyzeMaterial(Single);
	if (TestTrue(TEXT("the panned normal is found"), One.Fidelity2.Water.bNormalPanned))
	{
		// Near-opaque water publishes with the water key, which carries the sample as a layer (the converter reads its image).
		if (TestEqual(TEXT("it is the one normal layer"), One.Fidelity2.Water.Normals.Num(), 1))
		{
			const FMantleWaterNormalLayer& Layer = One.Fidelity2.Water.Normals[0];
			TestTrue(TEXT("of the source texture"), Layer.Texture.Get() == Normal);
			TestFalse(TEXT("on the first UV set"), Layer.bWorld);
			TestEqual(TEXT("with its tiling"), Layer.Tiling[0], 0.25, 1.0e-6);
			TestEqual(TEXT("and its pan u"), Layer.Speed[0], -0.0025, 1.0e-6);
			TestEqual(TEXT("and pan v"), Layer.Speed[1], 0.0, 1.0e-6);
		}
		TestFalse(TEXT("the water key's normal is not the generic panned slot"), One.Fidelity2.HasSource(1));
	}
	TestFalse(TEXT("a panned normal is not a frozen animation"), One.Has(EMantleMaterialLoss::Animation));

	// Translucent water (0.85) has no water key: its panned normal publishes as the source texture with the pan.
	UMaterial* Translucent = NewTranslucent();
	Translucent->GetEditorOnlyData()->Opacity.Connect(0, AddDepthFadeAt(Translucent, 0.85f, 100.0f));
	Translucent->GetEditorOnlyData()->Normal.Connect(0, AddPannedNormal(Translucent, Normal, 0.25f, FLinearColor(-0.5f, 0.0f, -0.5f), 0.005f));
	const FMantleMaterialAnalysis Seen = FMantlePlaceAssets::AnalyzeMaterial(Translucent);
	if (TestTrue(TEXT("the translucent panned normal is found"), Seen.Fidelity2.Water.bNormalPanned))
	{
		TestTrue(TEXT("it publishes the source texture"), Seen.Fidelity2.HasSource(1) && Seen.Fidelity2.SourceTextures[1].Get() == Normal);
		TestTrue(TEXT("with its pan"), Seen.Fidelity2.bPanning[1]);
		TestEqual(TEXT("pan u"), Seen.Fidelity2.Panning[1][0], -0.0025, 1.0e-6);
		TestEqual(TEXT("pan v"), Seen.Fidelity2.Panning[1][1], 0.0, 1.0e-6);
		TestEqual(TEXT("and its tiling"), Seen.Fidelity2.SourceTiling[1][0], 0.25, 1.0e-6);
	}

	// Two samples through the engine's BlendAngleCorrectedNormals: the water key carries both, in the graph's order; the
	// generic path (translucent water) keeps the faster one for both.
	UMaterial* Double = NewRiver(Normal);
	if (!Double)
	{
		AddError(TEXT("BlendAngleCorrectedNormals could not be loaded."));
		return false;
	}
	const FMantleMaterialAnalysis Two = FMantlePlaceAssets::AnalyzeMaterial(Double);
	if (TestTrue(TEXT("two blended panned normals are found"), Two.Fidelity2.Water.bNormalPanned))
	{
		const FMantleWaterBake& Water = Two.Fidelity2.Water;
		TestEqual(TEXT("both samples are counted"), Water.NormalSamples, 2);
		if (TestEqual(TEXT("both are layers"), Water.Normals.Num(), 2))
		{
			TestEqual(TEXT("the base normal is the first (tiling)"), Water.Normals[0].Tiling[0], 0.25, 1.0e-6);
			TestEqual(TEXT("the base normal's pan"), Water.Normals[0].Speed[0], -0.0025, 1.0e-6);
			TestEqual(TEXT("the blended normal is the second (tiling)"), Water.Normals[1].Tiling[0], 0.5, 1.0e-6);
			TestEqual(TEXT("the blended normal's pan"), Water.Normals[1].Speed[0], -0.0125, 1.0e-6);
		}
		TestEqual(TEXT("the faster pan stands for both without the key"), Water.NormalSpeed[0], -0.0125, 1.0e-6);
		TestEqual(TEXT("with its own tiling"), Water.NormalTiling[0], 0.5, 1.0e-6);
		TestEqual(TEXT("and the fade distance is read for the coefficients"), Water.FadeDistanceCm, 3.0, 1.0e-6);
	}
	UMaterial* DoubleTranslucent = NewRiver(Normal, 0.85f);
	const FMantleMaterialAnalysis Generic = FMantlePlaceAssets::AnalyzeMaterial(DoubleTranslucent);
	TestEqual(TEXT("translucent: the faster pan stands for both"), Generic.Fidelity2.Panning[1][0], -0.0125, 1.0e-6);
	TestEqual(TEXT("translucent: with its own tiling"), Generic.Fidelity2.SourceTiling[1][0], 0.5, 1.0e-6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWaterCoefficientsTest,
	"Mantle.Editor.Water.Coefficients",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWaterCoefficientsTest::RunTest(const FString& Parameters)
{
	// The starting-point heuristic: sigma = min(3 / D, 1); albedo = clamp(C, 0, 0.9); scattering = sigma albedo; absorption = sigma (1 - albedo).
	const double Teal[3] = { 0.0, 0.024158, 0.025187 };

	// A 100 cm fade: sigma 0.03, and the deep colour is the albedo.
	const FMantleWaterCoefficients Clear = FMantleWaterMaterial::Coefficients(Teal, 100.0);
	TestEqual(TEXT("scattering red"), Clear.Scattering[0], 0.0, 1.0e-9);
	TestEqual(TEXT("scattering green"), Clear.Scattering[1], 0.00072474, 1.0e-8);
	TestEqual(TEXT("scattering blue"), Clear.Scattering[2], 0.00075561, 1.0e-8);
	TestEqual(TEXT("absorption red"), Clear.Absorption[0], 0.03, 1.0e-9);
	TestEqual(TEXT("absorption green"), Clear.Absorption[1], 0.02927526, 1.0e-8);
	TestEqual(TEXT("absorption blue"), Clear.Absorption[2], 0.02924439, 1.0e-8);

	// The Spruce river's 3 cm fade: sigma 1, the most the schema allows (near-opaque past a few centimetres, as in the editor).
	const FMantleWaterCoefficients River = FMantleWaterMaterial::Coefficients(Teal, 3.0);
	TestEqual(TEXT("river scattering green"), River.Scattering[1], 0.024158, 1.0e-9);
	TestEqual(TEXT("river absorption green"), River.Absorption[1], 0.975842, 1.0e-9);

	// A shorter fade cannot go past the schema's 1.
	const FMantleWaterCoefficients Short = FMantleWaterMaterial::Coefficients(Teal, 0.5);
	TestEqual(TEXT("a very short fade is clamped to sigma 1"), Short.Scattering[1] + Short.Absorption[1], 1.0, 1.0e-9);
	const FMantleWaterCoefficients Unknown = FMantleWaterMaterial::Coefficients(Teal, 0.0);
	TestEqual(TEXT("no fade distance is 100 cm"), Unknown.Scattering[1], Clear.Scattering[1], 1.0e-12);

	// A bright deep colour keeps the albedo at 0.9, so the volume still absorbs a tenth.
	const double White[3] = { 1.0, 1.0, 1.0 };
	const FMantleWaterCoefficients Bright = FMantleWaterMaterial::Coefficients(White, 100.0);
	TestEqual(TEXT("albedo is capped at 0.9"), Bright.Scattering[0], 0.027, 1.0e-9);
	TestEqual(TEXT("so a tenth is absorbed"), Bright.Absorption[0], 0.003, 1.0e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWaterWorldNormalTest,
	"Mantle.Editor.Water.WorldNormalLayer",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWaterWorldNormalTest::RunTest(const FString& Parameters)
{
	using namespace MantleWaterMaterialTestPrivate;
	UTexture2D* Normal = UTexture2D::CreateTransient(4, 4);

	// The Spruce lake's ripples: World Position XY over a size, plus Time times a pan, straight into a normal sample.
	UMaterial* Lake = NewWater(3.0f);
	Lake->GetEditorOnlyData()->Normal.Connect(0, AddWorldNormal(Lake, Normal, FVector2D(500.0, 500.0), FVector2D(-0.0025, 0.0011)));
	const FMantleMaterialAnalysis Analysis = FMantlePlaceAssets::AnalyzeMaterial(Lake);
	const FMantleWaterBake& Water = Analysis.Fidelity2.Water;
	if (TestEqual(TEXT("one layer is found"), Water.Normals.Num(), 1))
	{
		const FMantleWaterNormalLayer& Layer = Water.Normals[0];
		TestTrue(TEXT("a world position layout is space world"), Layer.bWorld);
		TestEqual(TEXT("its size is the divisor"), Layer.WorldSizeCm, 500.0, 1.0e-6);
		TestEqual(TEXT("pan u"), Layer.Speed[0], -0.0025, 1.0e-6);
		TestEqual(TEXT("pan v"), Layer.Speed[1], 0.0011, 1.0e-6);
		TestFalse(TEXT("an even size is exact"), Layer.bApproximated);
		TestTrue(TEXT("it is the source texture"), Layer.Texture.Get() == Normal);
	}
	TestFalse(TEXT("a world layer is not a UV pan the generic path could carry"), Water.bNormalPanned);

	// A size that differs on the two axes, and a mirrored axis, are carried as their mean and flagged.
	UMaterial* Uneven = NewWater(3.0f);
	Uneven->GetEditorOnlyData()->Normal.Connect(0, AddWorldNormal(Uneven, Normal, FVector2D(500.0, 1000.0), FVector2D(0.0, 0.01)));
	const FMantleMaterialAnalysis UnevenAnalysis = FMantlePlaceAssets::AnalyzeMaterial(Uneven);
	if (TestEqual(TEXT("an uneven layout is a layer"), UnevenAnalysis.Fidelity2.Water.Normals.Num(), 1))
	{
		const FMantleWaterNormalLayer& Layer = UnevenAnalysis.Fidelity2.Water.Normals[0];
		TestTrue(TEXT("flagged as approximated"), Layer.bApproximated);
		TestEqual(TEXT("at the mean size"), Layer.WorldSizeCm, 750.0, 1.0e-6);
	}

	// The layout in a called function of its own, with the function's strength: a Multiply of the sample by 0.03 at its output.
	UMaterialFunction* Ripples = MakeRipplesFunction(Normal, FVector2D(500.0, 500.0), FVector2D(-0.0025, 0.0011), 0.03f);
	UMaterial* Called = NewWater(3.0f);
	UMaterialExpressionMaterialFunctionCall* Call = AddCall(Called, Ripples, nullptr, nullptr, nullptr, nullptr);
	Called->GetEditorOnlyData()->Normal.Connect(0, Call);
	const FMantleMaterialAnalysis CalledAnalysis = FMantlePlaceAssets::AnalyzeMaterial(Called);
	const FMantleWaterBake& CalledWater = CalledAnalysis.Fidelity2.Water;
	if (TestEqual(TEXT("a function's world layer is found, whatever it is called"), CalledWater.Normals.Num(), 1))
	{
		const FMantleWaterNormalLayer& Layer = CalledWater.Normals[0];
		TestTrue(TEXT("space world"), Layer.bWorld);
		TestEqual(TEXT("with its size"), Layer.WorldSizeCm, 500.0, 1.0e-6);
		TestEqual(TEXT("and its pan"), Layer.Speed[1], 0.0011, 1.0e-6);
		TestEqual(TEXT("the function's own strength is read"), Layer.Strength, 0.03, 1.0e-6);
		TestTrue(TEXT("only one of the function's samples is carried, which is said"), Layer.bApproximated);
		TestEqual(TEXT("and it is the normal scale"), CalledWater.NormalScale, 0.03, 1.0e-6);
	}

	// A function that does not lay anything out by world position is not recognised (and the reason is kept).
	UMaterialFunction* Plain = NewObject<UMaterialFunction>(GetTransientPackage());
	UMaterialExpressionTextureSample* Sample = AddNode<UMaterialExpressionTextureSample>(Plain);
	Sample->Texture = Normal;
	UMaterialExpressionFunctionOutput* PlainOutput = AddNode<UMaterialExpressionFunctionOutput>(Plain);
	PlainOutput->OutputName = TEXT("Normal");
	PlainOutput->Id = FGuid::NewGuid();
	PlainOutput->A.Connect(0, Sample);
	UMaterial* Unread = NewWater(3.0f);
	Unread->GetEditorOnlyData()->Normal.Connect(0, AddCall(Unread, Plain, nullptr, nullptr, nullptr, nullptr));
	const FMantleMaterialAnalysis UnreadAnalysis = FMantlePlaceAssets::AnalyzeMaterial(Unread);
	TestEqual(TEXT("a function with no world layout gives no layer"), UnreadAnalysis.Fidelity2.Water.Normals.Num(), 0);
	TestTrue(TEXT("and says why"), UnreadAnalysis.Fidelity2.Water.Why.Contains(TEXT("the normal calls")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWaterKeyRiverTest,
	"Mantle.Editor.Water.WaterKeyRiver",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWaterKeyRiverTest::RunTest(const FString& Parameters)
{
	using namespace MantleWaterMaterialTestPrivate;

	FMantlePlaceAssetSet Set;
	FMantlePlaceMaterialDocument Document;
	if (!ConvertRiver(*this, Set, Document))
	{
		return true;
	}
	if (!TestTrue(TEXT("near-opaque depth-fade water is a water document"), Document.WritesWater()))
	{
		return false;
	}
	TestTrue(TEXT("its normal map is uploaded as a texture"), Document.Textures.Contains(TEXT("normal")));
	TestTrue(TEXT("and its second normal map"), Document.Textures.Contains(TEXT("waterNormal2")));
	TestFalse(TEXT("it has no base colour texture"), Document.Textures.Contains(TEXT("baseColor")));
	TestEqual(TEXT("the one image is one texture"), Set.Textures.Num(), 1);

	// The JSON, with an uploaded id for each texture key.
	TMap<FString, FString> Ids;
	for (const FMantleConvertedTexture& Texture : Set.Textures)
	{
		Ids.Add(Texture.Key, FakeId(7));
	}
	FString Json;
	FString Error;
	if (!TestTrue(TEXT("the document is written"), Document.ToJson(Ids, Json, Error)))
	{
		AddError(Error);
		return false;
	}
	TSharedPtr<FJsonObject> Object;
	if (!TestTrue(TEXT("as JSON"), ParseJsonText(Json, Object)))
	{
		return false;
	}
	FString AlphaMode;
	Object->TryGetStringField(TEXT("alphaMode"), AlphaMode);
	TestEqual(TEXT("opaque"), AlphaMode, FString(TEXT("opaque")));
	bool bTwoSided = true;
	TestTrue(TEXT("with doubleSided"), Object->TryGetBoolField(TEXT("doubleSided"), bTwoSided));
	TestFalse(TEXT("which does nothing on water, so false"), bTwoSided);
	TestTrue(TEXT("the surface colour is the bake's, opaque"), NumbersMatch(Numbers(Object, TEXT("baseColorFactor")), { 0.0, 0.024158, 0.025187, 1.0 }, 1.0e-6));
	double Roughness = -1.0;
	Object->TryGetNumberField(TEXT("roughnessFactor"), Roughness);
	TestEqual(TEXT("roughness"), Roughness, 0.01, 1.0e-6);
	for (const TCHAR* Ignored : { TEXT("metallicFactor"), TEXT("emissiveFactor"), TEXT("emissiveStrength"), TEXT("uvScale"), TEXT("uvOffset"), TEXT("uvTransforms"),
			 TEXT("uvPanning"), TEXT("vertexColor"), TEXT("occlusionStrength"), TEXT("normalScale"), TEXT("wind"), TEXT("foliage") })
	{
		TestFalse(FString::Printf(TEXT("%s is not written on water"), Ignored), Object->HasField(Ignored));
	}

	// The volume: the 3 cm fade is sigma 1, and the deep colour is the albedo.
	const TSharedPtr<FJsonObject> Water = ObjectOf(Object, TEXT("water"));
	if (!TestTrue(TEXT("the water object"), Water.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("scattering"), NumbersMatch(Numbers(Water, TEXT("scattering")), { 0.0, 0.024158, 0.025187 }, 1.0e-5));
	TestTrue(TEXT("absorption"), NumbersMatch(Numbers(Water, TEXT("absorption")), { 1.0, 0.975842, 0.974813 }, 1.0e-5));
	TestFalse(TEXT("phase g is its default, not written"), Water->HasField(TEXT("phaseG")));

	// Both normal layers, in the graph's order, each with its layout and pan; the second with its texture.
	const TSharedPtr<FJsonObject> First = ObjectOf(Water, TEXT("normal"));
	if (TestTrue(TEXT("the first layer"), First.IsValid()))
	{
		FString Space;
		First->TryGetStringField(TEXT("space"), Space);
		TestEqual(TEXT("on the mesh's UVs"), Space, FString(TEXT("uv")));
		TestTrue(TEXT("tiling"), NumbersMatch(Numbers(First, TEXT("tiling")), { 0.25, 0.25 }, 1.0e-6));
		TestTrue(TEXT("panning"), NumbersMatch(Numbers(First, TEXT("panning")), { -0.0025, 0.0 }, 1.0e-6));
		TestFalse(TEXT("no worldSizeCm with uv"), First->HasField(TEXT("worldSizeCm")));
		TestFalse(TEXT("its texture is textures.normal's"), First->HasField(TEXT("texture")));
	}
	const TSharedPtr<FJsonObject> Second = ObjectOf(Water, TEXT("normal2"));
	if (TestTrue(TEXT("the second layer"), Second.IsValid()))
	{
		FString Texture;
		Second->TryGetStringField(TEXT("texture"), Texture);
		TestEqual(TEXT("its texture id"), Texture, FakeId(7));
		TestTrue(TEXT("tiling"), NumbersMatch(Numbers(Second, TEXT("tiling")), { 0.5, 0.5 }, 1.0e-6));
		TestTrue(TEXT("panning"), NumbersMatch(Numbers(Second, TEXT("panning")), { -0.0125, 0.0 }, 1.0e-6));
	}
	const TSharedPtr<FJsonObject> Textures = ObjectOf(Object, TEXT("textures"));
	if (TestTrue(TEXT("textures"), Textures.IsValid()))
	{
		FString NormalId;
		Textures->TryGetStringField(TEXT("normal"), NormalId);
		TestEqual(TEXT("textures.normal"), NormalId, FakeId(7));
		TestEqual(TEXT("and nothing else"), Textures->Values.Num(), 1);
	}

	// The cache's template round-trips what the document holds.
	FMantlePlaceMaterialDocument Reloaded;
	if (TestTrue(TEXT("the template reads back"), FMantlePlaceMaterialDocument::FromTemplateJson(Document.ToTemplateJson(), Reloaded)))
	{
		TestTrue(TEXT("still water"), Reloaded.WritesWater());
		TestEqual(TEXT("with its second normal map"), Reloaded.Textures.FindRef(TEXT("waterNormal2")), Document.Textures.FindRef(TEXT("waterNormal2")));
		TestEqual(TEXT("its second layer's tiling"), Reloaded.WaterNormal2.Tiling[0], 0.5, 1.0e-6);
		TestEqual(TEXT("its first layer's pan"), Reloaded.WaterNormal.Panning[0], -0.0025, 1.0e-6);
		TestEqual(TEXT("its scattering"), Reloaded.WaterScattering[1], 0.024158, 1.0e-5);
		FString Again;
		TestTrue(TEXT("and writes the same bytes"), Reloaded.ToJson(Ids, Again, Error));
		TestEqual(TEXT("exactly"), Again, Json);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWaterKeyLakeTest,
	"Mantle.Editor.Water.WaterKeyLake",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWaterKeyLakeTest::RunTest(const FString& Parameters)
{
	using namespace MantleWaterMaterialTestPrivate;

	// The lake: FlattenNormal (Normal Intensity 10, so Flatness -9) over the ripples function (world layout, strength 0.03).
	UMaterialFunction* Flatten = LoadEngineFunction(TEXT("/Engine/Functions/Engine_MaterialFunctions01/Texturing/FlattenNormal.FlattenNormal"));
	if (!Flatten)
	{
		AddError(TEXT("FlattenNormal could not be loaded."));
		return false;
	}
	UTexture2D* Normal = UTexture2D::CreateTransient(4, 4);
	UMaterialFunction* Ripples = MakeRipplesFunction(Normal, FVector2D(500.0, 500.0), FVector2D(-0.0025, 0.0011), 0.03f);
	UMaterial* Lake = NewWater(3.0f);
	UMaterialExpressionMaterialFunctionCall* Called = AddCall(Lake, Ripples, nullptr, nullptr, nullptr, nullptr);
	Lake->GetEditorOnlyData()->Normal.Connect(0, AddCall(Lake, Flatten, TEXT("Normal"), Called, TEXT("Flatness"), AddNumber(Lake, -9.0f)));
	const FMantleMaterialAnalysis Analysis = FMantlePlaceAssets::AnalyzeMaterial(Lake);
	TestEqual(TEXT("Normal Intensity 10 times the function's 0.03"), Analysis.Fidelity2.Water.NormalScale, 0.3, 1.0e-6);

	FMantleMaterialExtras Extras;
	Extras.Fidelity = Analysis.Fidelity2;
	Extras.SourceImages[1] = NormalPng();
	Extras.SourceNames[1] = TEXT("T_Water_Normal");
	FMantlePlaceAssetSet Set;
	const FMantlePlaceMaterialDocument Document = FMantlePlaceAssetConverter::MaterialFromGltf(BakedWater(), 0, TEXT("MI_Lake"), FMantlePlaceAssetLimits(), false, Set, Extras);
	if (!TestTrue(TEXT("the lake is a water document"), Document.WritesWater()))
	{
		return false;
	}
	TestFalse(TEXT("one normal map: no second"), Document.Textures.Contains(TEXT("waterNormal2")));

	TMap<FString, FString> Ids;
	for (const FMantleConvertedTexture& Texture : Set.Textures)
	{
		Ids.Add(Texture.Key, FakeId(9));
	}
	FString Json;
	FString Error;
	if (!TestTrue(TEXT("written"), Document.ToJson(Ids, Json, Error)))
	{
		AddError(Error);
		return false;
	}
	TSharedPtr<FJsonObject> Object;
	if (!TestTrue(TEXT("as JSON"), ParseJsonText(Json, Object)))
	{
		return false;
	}
	double NormalScale = 0.0;
	TestTrue(TEXT("normalScale is written (not 1)"), Object->TryGetNumberField(TEXT("normalScale"), NormalScale));
	TestEqual(TEXT("at 10 x 0.03"), NormalScale, 0.3, 1.0e-6);
	const TSharedPtr<FJsonObject> Water = ObjectOf(Object, TEXT("water"));
	const TSharedPtr<FJsonObject> First = ObjectOf(Water, TEXT("normal"));
	if (TestTrue(TEXT("the layer"), First.IsValid()))
	{
		FString Space;
		First->TryGetStringField(TEXT("space"), Space);
		TestEqual(TEXT("space world"), Space, FString(TEXT("world")));
		double Size = 0.0;
		First->TryGetNumberField(TEXT("worldSizeCm"), Size);
		TestEqual(TEXT("worldSizeCm is the scale"), Size, 500.0, 1.0e-6);
		TestFalse(TEXT("no tiling with space world"), First->HasField(TEXT("tiling")));
		TestTrue(TEXT("its pan"), NumbersMatch(Numbers(First, TEXT("panning")), { -0.0025, 0.0011 }, 1.0e-6));
	}
	TestFalse(TEXT("no second layer"), Water.IsValid() && Water->HasField(TEXT("normal2")));

	// The function's other samples are not carried, and the report says so.
	bool bSaid = false;
	for (const FMantleValidationIssue& Issue : Set.Issues)
	{
		bSaid = bSaid || Issue.Problem.Contains(TEXT("laid out by world position"));
	}
	TestTrue(TEXT("a layout carried only roughly is warned about"), bSaid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWaterKeyDroppedTest,
	"Mantle.Editor.Water.WaterKeyDropsWhatWaterIgnores",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWaterKeyDroppedTest::RunTest(const FString& Parameters)
{
	using namespace MantleWaterMaterialTestPrivate;

	// The bake carries a metallic factor and an emissive colour; the schema says neither does anything on water, so neither is written.
	UMaterial* Material = NewWater(100.0f);
	const FMantleMaterialAnalysis Analysis = FMantlePlaceAssets::AnalyzeMaterial(Material);
	const TArray<double> Glow = { 0.1, 0.0, 0.0 };
	const FMantleGlbDocument Flattened = BakedWater(0.2, 0.8, &Glow);

	FMantleMaterialExtras Extras;
	Extras.Fidelity = Analysis.Fidelity2;
	FMantlePlaceAssetSet Set;
	const FMantlePlaceMaterialDocument Document = FMantlePlaceAssetConverter::MaterialFromGltf(Flattened, 0, TEXT("MI_Glowing"), FMantlePlaceAssetLimits(), false, Set, Extras);
	TestTrue(TEXT("still water"), Document.WritesWater());
	TestEqual(TEXT("not metallic"), Document.MetallicFactor, 0.0, 1.0e-9);
	TestEqual(TEXT("the source's roughness stands"), Document.RoughnessFactor, 0.2, 1.0e-6);
	bool bMetallic = false;
	bool bEmissive = false;
	for (const FMantleValidationIssue& Issue : Set.Issues)
	{
		bMetallic = bMetallic || Issue.Problem.Contains(TEXT("metallic is dropped"));
		bEmissive = bEmissive || Issue.Problem.Contains(TEXT("emissive is dropped"));
	}
	TestTrue(TEXT("the metallic that water ignores is reported"), bMetallic);
	TestTrue(TEXT("and the emissive"), bEmissive);

	FString Json;
	FString Error;
	TestTrue(TEXT("written"), Document.ToJson(TMap<FString, FString>(), Json, Error));
	TestFalse(TEXT("no metallic key"), Json.Contains(TEXT("metallicFactor")));
	TestFalse(TEXT("no emissive key"), Json.Contains(TEXT("emissive")));
	TestFalse(TEXT("no normal layers without a normal map"), Json.Contains(TEXT("\"normal\"")));
	TestTrue(TEXT("the deep colour is the albedo at the 100 cm default"), Json.Contains(TEXT("\"absorption\":[0.03,")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWaterDowngradeTest,
	"Mantle.Editor.Water.DowngradesWithoutMaterialFidelity",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWaterDowngradeTest::RunTest(const FString& Parameters)
{
	using namespace MantleWaterMaterialTestPrivate;

	FMantlePlaceAssetSet Set;
	FMantlePlaceMaterialDocument Document;
	if (!ConvertRiver(*this, Set, Document))
	{
		return true;
	}
	if (!TestTrue(TEXT("the river is a water document"), Document.WritesWater()))
	{
		return false;
	}
	TMap<FString, FString> Ids;
	for (const FMantleConvertedTexture& Texture : Set.Textures)
	{
		Ids.Add(Texture.Key, FakeId(7));
	}

	// A client that reads materialFidelity changes nothing.
	FMantlePlaceMaterialDocument Kept = Document;
	TestFalse(TEXT("a client that reads the key downgrades nothing"), FMantleTargetFeatures::All().DowngradeMaterial(Kept));
	TestTrue(TEXT("and the document is still water"), Kept.WritesWater());

	// One that does not gets the opaque material the river published as before the key: the faster normal, panned, no second map.
	FMantlePlaceMaterialDocument Old = Document;
	TestTrue(TEXT("a client without it downgrades"), FMantleTargetFeatures::None().DowngradeMaterial(Old));
	TestFalse(TEXT("the water key is off"), Old.bWater);
	TestFalse(TEXT("it is not written as water"), Old.WritesWater());
	TestEqual(TEXT("opaque, as before"), Old.AlphaMode, FString(TEXT("opaque")));
	TestTrue(TEXT("with its normal map"), Old.Textures.Contains(TEXT("normal")));
	TestFalse(TEXT("and no second"), Old.Textures.Contains(TEXT("waterNormal2")));
	const FMantlePlaceDocumentUvTransform* Transform = Old.UvTransforms.Find(TEXT("normal"));
	if (TestNotNull(TEXT("the faster layer's tiling"), Transform))
	{
		TestEqual(TEXT("0.5"), Transform->Scale[0], 0.5, 1.0e-6);
	}
	const FVector2D* Pan = Old.UvPanning.Find(TEXT("normal"));
	if (TestNotNull(TEXT("and its pan"), Pan))
	{
		TestEqual(TEXT("-0.0125"), Pan->X, -0.0125, 1.0e-6);
	}
	TestFalse(TEXT("the downgrade is only once"), FMantleTargetFeatures::None().DowngradeMaterial(Old));

	FString Json;
	FString Error;
	TestTrue(TEXT("the downgraded document writes"), Old.ToJson(Ids, Json, Error));
	TestFalse(TEXT("without the water key"), Json.Contains(TEXT("\"water\"")));
	TestTrue(TEXT("opaque"), Json.Contains(TEXT("\"alphaMode\":\"opaque\"")));
	TestTrue(TEXT("with the pan on the normal"), Json.Contains(TEXT("\"uvPanning\":{\"normal\":[-0.0125,0]}")));
	TestTrue(TEXT("and the colour the bake gave"), Json.Contains(TEXT("\"baseColorFactor\":[0,0.024158,0.025187,1]")));
	TestTrue(TEXT("and not metallic"), Json.Contains(TEXT("\"metallicFactor\":0")));

	// The asset set writes it through the export's target, and says what it left out.
	FMantleConvertedMaterial& Converted = Set.Materials.AddDefaulted_GetRef();
	Converted.Key = TEXT("material:MI_River");
	Converted.Name = TEXT("MI_River");
	Converted.Document = Document;
	for (const FMantleConvertedTexture& Texture : Set.Textures)
	{
		Set.RecordUpload(Texture.Key, FakeId(7), TEXT("sha256:00"), 1);
	}
	TArray<uint8> Bytes;
	TestTrue(TEXT("no export in progress: the set writes the water"), Set.MaterialBytes(Converted, Bytes, Error));
	TestTrue(TEXT("with the key"), WaterBytesText(Bytes).Contains(TEXT("\"water\"")));
	{
		const FMantleTargetFeaturesScope Scope(FMantleTargetFeatures::None());
		TArray<uint8> OlderBytes;
		TestTrue(TEXT("the set writes it for the older client"), Set.MaterialBytes(Converted, OlderBytes, Error));
		TestFalse(TEXT("without the key"), WaterBytesText(OlderBytes).Contains(TEXT("\"water\"")));
		TestTrue(TEXT("the source document still has its water"), Converted.Document.WritesWater());

		FMantleCoverage Coverage;
		FMantleTargetFeatures::Current().ReportMaterialDowngrades(Set, Coverage);
		TestEqual(TEXT("one approximated material"), Coverage.Count(MantleCoverageCategory::Materials, EMantleCoverageOutcome::Approximated), static_cast<int64>(1));
		const FMantleCoverageCategory* Category = Coverage.Find(MantleCoverageCategory::Materials);
		if (TestNotNull(TEXT("the materials category"), Category) && Category->Get(EMantleCoverageOutcome::Approximated).Examples.Num() == 1)
		{
			const FMantleCoverageExample& Example = Category->Get(EMantleCoverageOutcome::Approximated).Examples[0];
			TestEqual(TEXT("it names the material"), Example.Name, FString(TEXT("MI_River")));
			TestTrue(TEXT("and says why"), Example.Detail.Contains(TEXT("needs a newer Mantle client")));
		}
	}
	FMantleCoverage Quiet;
	FMantleTargetFeatures::All().ReportMaterialDowngrades(Set, Quiet);
	TestTrue(TEXT("a client that reads it reports nothing"), Quiet.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWaterMasterChainTest,
	"Mantle.Editor.Water.SpruceForestMasterChain",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWaterMasterChainTest::RunTest(const FString& Parameters)
{
	using namespace MantleWaterMaterialTestPrivate;

	// The Stylized Spruce Forest's M_Water (read from its graph): Opacity is a Depth Fade whose opacity and fade
	// distance are the scalar parameters "Opacity" (default 1) and "Fade Distance"; Base Color is a static switch
	// (ProceduralWinter, true) onto a Lerp from the "Base Color" vector parameter to an ice texture by a collection parameter.
	UMaterial* Master = NewTranslucent();
	UMaterialExpressionScalarParameter* OpacityParameter = AddExpression<UMaterialExpressionScalarParameter>(Master);
	OpacityParameter->ParameterName = TEXT("Opacity");
	OpacityParameter->DefaultValue = 1.0f;
	UMaterialExpressionScalarParameter* DistanceParameter = AddExpression<UMaterialExpressionScalarParameter>(Master);
	DistanceParameter->ParameterName = TEXT("Fade Distance");
	DistanceParameter->DefaultValue = 100.0f;
	UMaterialExpressionDepthFade* Fade = AddExpression<UMaterialExpressionDepthFade>(Master);
	Fade->InOpacity.Connect(0, OpacityParameter);
	Fade->FadeDistance.Connect(0, DistanceParameter);
	Master->GetEditorOnlyData()->Opacity.Connect(0, Fade);

	UMaterialExpressionVectorParameter* ColourParameter = AddExpression<UMaterialExpressionVectorParameter>(Master);
	ColourParameter->ParameterName = TEXT("Base Color");
	ColourParameter->DefaultValue = FLinearColor(0.0f, 0.725731f, 0.75f);
	UMaterialExpressionTextureSample* Ice = AddExpression<UMaterialExpressionTextureSample>(Master);
	Ice->Texture = UTexture2D::CreateTransient(4, 4);
	UMaterialExpressionLinearInterpolate* Winter = AddExpression<UMaterialExpressionLinearInterpolate>(Master);
	Winter->A.Connect(0, ColourParameter);
	Winter->B.Connect(0, Ice);
	UMaterialExpressionStaticSwitchParameter* Switch = AddExpression<UMaterialExpressionStaticSwitchParameter>(Master);
	Switch->ParameterName = TEXT("ProceduralWinter");
	Switch->DefaultValue = true;
	Switch->A.Connect(0, Winter);
	Switch->B.Connect(0, ColourParameter);
	Master->GetEditorOnlyData()->BaseColor.Connect(0, Switch);

	// MI_River and MI_Lake: Opacity 0.96 and 0.99.
	for (const float Value : { 0.96f, 0.99f })
	{
		UMaterialInstanceConstant* Instance = NewObject<UMaterialInstanceConstant>(GetTransientPackage());
		Instance->SetParentEditorOnly(Master, false);
		// SetParent leaves the instance's cached blend mode at its default (opaque); a loaded instance has it from PostLoad.
		Instance->UpdateOverridableBaseProperties();
		Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Opacity")), Value);

		const FMantleMaterialAnalysis Analysis = FMantlePlaceAssets::AnalyzeMaterial(Instance);
		const FMantleWaterBake& Water = Analysis.Fidelity2.Water;
		if (TestTrue(TEXT("the instance's depth fade is found"), Water.bFound))
		{
			TestTrue(TEXT("its opacity is read from the instance's parameter"), Water.bOpacityRead);
			TestEqual(TEXT("it is the instance's Opacity"), Water.Opacity, static_cast<double>(Value), 1.0e-4);
			// The ice texture on the way to the colour stops that read: the bake's colour stands.
			TestFalse(TEXT("the textured colour is not read"), Water.bColour);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWaterSingleLayerTest,
	"Mantle.Editor.Water.SingleLayerWaterMaterialPublishesAsWater",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWaterSingleLayerTest::RunTest(const FString& Parameters)
{
	using namespace MantleWaterMaterialTestPrivate;

	// A Water plugin body's material: Single Layer Water shading, opaque blend, coefficients on the output node, no depth fade.
	UMaterial* Material = NewObject<UMaterial>(GetTransientPackage());
	Material->SetShadingModel(MSM_SingleLayerWater);
	UMaterialExpressionSingleLayerWaterMaterialOutput* Output = AddExpression<UMaterialExpressionSingleLayerWaterMaterialOutput>(Material);
	Output->ScatteringCoefficients.Connect(0, AddColour(Material, FLinearColor(0.01f, 0.02f, 0.03f)));
	Output->AbsorptionCoefficients.Connect(0, AddColour(Material, FLinearColor(0.2f, 0.1f, 0.05f)));
	Material->GetEditorOnlyData()->BaseColor.Connect(0, AddColour(Material, FLinearColor(0.0f, 0.2f, 0.25f)));

	const FMantleMaterialAnalysis Analysis = FMantlePlaceAssets::AnalyzeMaterial(Material);
	const FMantleWaterBake& Water = Analysis.Fidelity2.Water;
	if (!TestTrue(TEXT("a Single Layer Water material is found as water"), Water.bFound && Water.bSingleLayer))
	{
		return false;
	}
	TestTrue(TEXT("it publishes opaque with the water key"), Water.PublishesOpaque());
	TestTrue(TEXT("its own coefficients are read"), Water.bVolume);
	TestFalse(TEXT("and its shading is no longer a loss"), Analysis.Has(EMantleMaterialLoss::SpecialShading));

	// The bake of an opaque water surface has no alpha blend; the document is still a water document, with the node's coefficients.
	FMantleGlbDocument Flattened = BakedWater();
	Flattened.Json->GetArrayField(TEXT("materials"))[0]->AsObject()->SetStringField(TEXT("alphaMode"), TEXT("OPAQUE"));
	FMantleMaterialExtras Extras;
	Extras.Fidelity = Analysis.Fidelity2;
	FMantlePlaceAssetSet Set;
	const FMantlePlaceMaterialDocument Doc = FMantlePlaceAssetConverter::MaterialFromGltf(Flattened, 0, TEXT("WaterMID_0"), FMantlePlaceAssetLimits(), false, Set, Extras);
	TestTrue(TEXT("the document writes the water key"), Doc.WritesWater());
	TestEqual(TEXT("with the node's absorption"), Doc.WaterAbsorption[0], 0.2, 1.0e-4);
	TestEqual(TEXT("and its scattering"), Doc.WaterScattering[2], 0.03, 1.0e-4);
	TestEqual(TEXT("and the deep colour"), Doc.BaseColorFactor[2], 0.25, 1.0e-4);

	// An ordinary lit material stays ordinary.
	UMaterial* Plain = NewObject<UMaterial>(GetTransientPackage());
	TestFalse(TEXT("a plain material is not water"), FMantlePlaceAssets::AnalyzeMaterial(Plain).Fidelity2.Water.bFound);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWaterSingleLayerWavesTest,
	"Mantle.Editor.Water.SingleLayerWaterWithWaves",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWaterSingleLayerWavesTest::RunTest(const FString& Parameters)
{
	using namespace MantleWaterMaterialTestPrivate;

	// The Water plugin's material animates its waves with world position offset and keeps its coefficients in a graph this cannot read.
	UMaterial* Material = NewObject<UMaterial>(GetTransientPackage());
	Material->SetShadingModel(MSM_SingleLayerWater);
	UMaterialExpressionTime* Time = AddExpression<UMaterialExpressionTime>(Material);
	UMaterialExpressionMultiply* Wave = AddExpression<UMaterialExpressionMultiply>(Material);
	Wave->A.Connect(0, Time);
	Wave->B.Connect(0, AddColour(Material, FLinearColor(0.0f, 0.0f, 50.0f)));
	Material->GetEditorOnlyData()->WorldPositionOffset.Connect(0, Wave);
	Material->GetEditorOnlyData()->BaseColor.Connect(0, AddColour(Material, FLinearColor(0.0f, 0.2f, 0.25f)));

	const FMantleMaterialAnalysis Analysis = FMantlePlaceAssets::AnalyzeMaterial(Material);
	const FMantleWaterBake& Water = Analysis.Fidelity2.Water;
	if (!TestTrue(TEXT("water with waves is still found as water"), Water.bFound && Water.bSingleLayer))
	{
		return false;
	}
	TestTrue(TEXT("its waves are said to be dropped"), Water.bWavesDropped);
	TestFalse(TEXT("and it is not wind"), Analysis.Fidelity2.bWind);
	TestFalse(TEXT("with no output node, the coefficients derive from the colour"), Water.bVolume);

	FMantleGlbDocument Flattened = BakedWater();
	Flattened.Json->GetArrayField(TEXT("materials"))[0]->AsObject()->SetStringField(TEXT("alphaMode"), TEXT("OPAQUE"));
	FMantleMaterialExtras Extras;
	Extras.Fidelity = Analysis.Fidelity2;
	FMantlePlaceAssetSet Set;
	const FMantlePlaceMaterialDocument Doc = FMantlePlaceAssetConverter::MaterialFromGltf(Flattened, 0, TEXT("WaterMID_1"), FMantlePlaceAssetLimits(), false, Set, Extras);
	TestTrue(TEXT("it writes the water key"), Doc.WritesWater());
	TestTrue(TEXT("with coefficients above zero"), Doc.WaterAbsorption[0] + Doc.WaterAbsorption[1] + Doc.WaterAbsorption[2] > 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWaterPluginChainTest,
	"Mantle.Editor.Water.WaterPluginBodyChainPublishesAsWater",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWaterPluginChainTest::RunTest(const FString& Parameters)
{
	using namespace MantleWaterMaterialTestPrivate;

	// The Water plugin's Water_Material as a Custom body draws it (Babylon Temple, GhzGarage/mantle#818): Single Layer Water, masked,
	// built with Set Material Attributes (no output root is connected), its waves a world position offset of Time, its scattering on
	// the output node behind a foam Lerp this cannot read, and "Absorption", "Scattering" and "Water Albedo" vector parameters.
	UMaterial* Master = NewObject<UMaterial>(GetTransientPackage());
	Master->SetShadingModel(MSM_SingleLayerWater);
	Master->BlendMode = BLEND_Masked;
	Master->bUseMaterialAttributes = true;
	auto AddParameter = [Master](const TCHAR* Name, const FLinearColor& Default)
	{
		UMaterialExpressionVectorParameter* Parameter = AddExpression<UMaterialExpressionVectorParameter>(Master);
		Parameter->ParameterName = Name;
		Parameter->DefaultValue = Default;
		return Parameter;
	};
	UMaterialExpressionVectorParameter* Absorption = AddParameter(TEXT("Absorption"), FLinearColor(0.1f, 0.05f, 0.02f));
	UMaterialExpressionVectorParameter* Scattering = AddParameter(TEXT("Scattering"), FLinearColor(0.01f, 0.02f, 0.03f));
	UMaterialExpressionVectorParameter* Albedo = AddParameter(TEXT("Water Albedo"), FLinearColor(0.0f, 0.2f, 0.25f));
	UMaterialExpressionTextureSample* Foam = AddExpression<UMaterialExpressionTextureSample>(Master);
	Foam->Texture = UTexture2D::CreateTransient(4, 4);
	UMaterialExpressionLinearInterpolate* FoamScattering = AddExpression<UMaterialExpressionLinearInterpolate>(Master);
	FoamScattering->A.Connect(0, Scattering);
	FoamScattering->B.Connect(0, AddColour(Master, FLinearColor::White));
	FoamScattering->Alpha.Connect(0, Foam);
	UMaterialExpressionSingleLayerWaterMaterialOutput* Output = AddExpression<UMaterialExpressionSingleLayerWaterMaterialOutput>(Master);
	Output->ScatteringCoefficients.Connect(0, FoamScattering);
	Output->AbsorptionCoefficients.Connect(0, Absorption);
	UMaterialExpressionMultiply* Wave = AddExpression<UMaterialExpressionMultiply>(Master);
	Wave->A.Connect(0, AddExpression<UMaterialExpressionTime>(Master));
	Wave->B.Connect(0, AddColour(Master, FLinearColor(0.0f, 0.0f, 50.0f)));
	UMaterialExpressionSetMaterialAttributes* Attributes = AddExpression<UMaterialExpressionSetMaterialAttributes>(Master);
	Attributes->ConnectInputAttribute(MP_BaseColor, Albedo);
	Attributes->ConnectInputAttribute(MP_OpacityMask, AddNumber(Master, 1.0f));
	Attributes->ConnectInputAttribute(MP_WorldPositionOffset, Wave);
	Master->GetEditorOnlyData()->MaterialAttributes.Connect(0, Attributes);
	// A loaded material has its parameters cached; one built here caches them now, so the instances below find the defaults.
	Master->UpdateCachedExpressionData();

	// Water_Material_CustomMesh: an instance overriding the absorption; WaterMID_0: the body's transient dynamic instance on its mesh.
	UMaterialInstanceConstant* Instance = NewObject<UMaterialInstanceConstant>(GetTransientPackage());
	Instance->SetParentEditorOnly(Master, false);
	Instance->UpdateOverridableBaseProperties();
	Instance->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Absorption")), FLinearColor(0.2f, 0.1f, 0.05f));
	UMaterialInstanceDynamic* Dynamic = UMaterialInstanceDynamic::Create(Instance, GetTransientPackage(), TEXT("WaterMID_0"));
	Dynamic->UpdateOverridableBaseProperties();
	Dynamic->SetScalarParameterValue(TEXT("WaterBodyIndex"), 0.0f);
	TestEqual(TEXT("the chain is masked"), static_cast<int32>(Dynamic->GetBlendMode()), static_cast<int32>(BLEND_Masked));

	const FMantleMaterialAnalysis Analysis = FMantlePlaceAssets::AnalyzeMaterial(Dynamic);
	const FMantleWaterBake& Water = Analysis.Fidelity2.Water;
	if (!TestTrue(TEXT("the body's dynamic instance is found as Single Layer Water"), Water.bFound && Water.bSingleLayer))
	{
		return false;
	}
	TestFalse(TEXT("its shading is no longer a loss (no 'becomes standard shading')"), Analysis.Has(EMantleMaterialLoss::SpecialShading));
	TestTrue(TEXT("it publishes opaque with the water key"), Water.PublishesOpaque());
	TestTrue(TEXT("its waves are said to be dropped"), Water.bWavesDropped);
	TestFalse(TEXT("and are not a world position offset loss"), Analysis.Has(EMantleMaterialLoss::WorldPositionOffset));
	TestFalse(TEXT("nor wind"), Analysis.Fidelity2.bWind);
	if (TestTrue(TEXT("the volume comes from the named parameters"), Water.bVolume))
	{
		TestEqual(TEXT("the instance's absorption"), Water.Volume.Absorption[0], 0.2, 1.0e-4);
		TestEqual(TEXT("and the master's scattering"), Water.Volume.Scattering[2], 0.03, 1.0e-4);
	}
	if (TestTrue(TEXT("the colour comes from Water Albedo"), Water.bColour))
	{
		TestEqual(TEXT("as the parameter has it"), Water.Colour[2], 0.25, 1.0e-4);
	}

	// GLTFExporter's bake of a masked material; the document is a water document, opaque, with the parameters' coefficients.
	FMantleGlbDocument Flattened = BakedWater();
	Flattened.Json->GetArrayField(TEXT("materials"))[0]->AsObject()->SetStringField(TEXT("alphaMode"), TEXT("MASK"));
	FMantleMaterialExtras Extras;
	Extras.Fidelity = Analysis.Fidelity2;
	FMantlePlaceAssetSet Set;
	const FMantlePlaceMaterialDocument Doc = FMantlePlaceAssetConverter::MaterialFromGltf(Flattened, 0, TEXT("WaterMID_0"), FMantlePlaceAssetLimits(), false, Set, Extras);
	TestTrue(TEXT("the document writes the water key"), Doc.WritesWater());
	TestEqual(TEXT("masked water publishes opaque"), Doc.AlphaMode, FString(TEXT("opaque")));
	TestEqual(TEXT("with the instance's absorption"), Doc.WaterAbsorption[0], 0.2, 1.0e-4);
	TestEqual(TEXT("and the albedo as the surface colour"), Doc.BaseColorFactor[2], 0.25, 1.0e-4);

	// A Single Layer Water material with nothing to read still has a volume and a colour: a clear lake's.
	UMaterial* Bare = NewObject<UMaterial>(GetTransientPackage());
	Bare->SetShadingModel(MSM_SingleLayerWater);
	Bare->bUseMaterialAttributes = true;
	const FMantleWaterBake& Plain = FMantlePlaceAssets::AnalyzeMaterial(Bare).Fidelity2.Water;
	TestTrue(TEXT("a bare Single Layer Water material is water"), Plain.bFound && Plain.bSingleLayer);
	TestTrue(TEXT("with the clear lake's colour"), Plain.bColour && FMath::IsNearlyEqual(Plain.Colour[1], FMantleWaterMaterial::ClearLakeColour[1], 1.0e-6));
	TestFalse(TEXT("and no coefficients of its own (they derive from the colour)"), Plain.bVolume);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
