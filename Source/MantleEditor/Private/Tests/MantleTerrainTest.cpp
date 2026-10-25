// Copyright Joshua Eger. All Rights Reserved.
#include "MantleLandscapeMaterialReader.h"
#include "MantlePlaceLimits.h"
#include "MantlePng.h"
#include "MantleTerrainExport.h"

#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionClamp.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionLandscapeLayerBlend.h"
#include "Materials/MaterialExpressionLandscapeLayerCoords.h"
#include "Materials/MaterialExpressionLandscapeLayerSample.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionPower.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// A landscape with more paint layers than Mantle publishes, and a landscape material read for what its layers look like.
// The fold is pure data; the reader runs on small material graphs built in code, so no level or asset is needed.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleTerrainTestPrivate
{
	/** A 2 x 2 landscape source whose layer i has the same weight Weights[i] on every sample. */
	FMantleTerrainSource MakeSource(const TArray<FName>& Names, const TArray<int32>& Weights)
	{
		FMantleTerrainSource Source;
		Source.Width = 2;
		Source.Height = 2;
		Source.Heights.SetNumZeroed(4);
		Source.LayerNames = Names;
		for (const int32 Weight : Weights)
		{
			TArray<uint8>& Layer = Source.LayerWeights.AddDefaulted_GetRef();
			Layer.Init(static_cast<uint8>(Weight), 4);
		}
		return Source;
	}

	template <typename TExpression>
	TExpression* AddExpression(UMaterial* Material)
	{
		TExpression* Expression = NewObject<TExpression>(Material);
		Expression->Material = Material;
		Material->GetExpressionCollection().AddExpression(Expression);
		return Expression;
	}

	UMaterialExpressionTextureSample* AddSample(UMaterial* Material, UTexture* Texture, UMaterialExpression* Coordinates)
	{
		UMaterialExpressionTextureSample* Sample = AddExpression<UMaterialExpressionTextureSample>(Material);
		Sample->Texture = Texture;
		if (Coordinates)
		{
			Sample->Coordinates.Connect(0, Coordinates);
		}
		return Sample;
	}

	UMaterialExpressionLandscapeLayerCoords* AddLayerCoords(UMaterial* Material, const float MappingScale)
	{
		UMaterialExpressionLandscapeLayerCoords* Coordinates = AddExpression<UMaterialExpressionLandscapeLayerCoords>(Material);
		Coordinates->MappingScale = MappingScale;
		return Coordinates;
	}

	/** A Landscape Layer Blend of two layers, each fed by the expression after its name. */
	UMaterialExpressionLandscapeLayerBlend* AddBlend(UMaterial* Material, const FName FirstLayer, UMaterialExpression* FirstInput, const FName SecondLayer,
		UMaterialExpression* SecondInput)
	{
		UMaterialExpressionLandscapeLayerBlend* Blend = AddExpression<UMaterialExpressionLandscapeLayerBlend>(Material);
		FLayerBlendInput& First = Blend->Layers.AddDefaulted_GetRef();
		First.LayerName = FirstLayer;
		First.LayerInput.Connect(0, FirstInput);
		FLayerBlendInput& Second = Blend->Layers.AddDefaulted_GetRef();
		Second.LayerName = SecondLayer;
		Second.LayerInput.Connect(0, SecondInput);
		return Blend;
	}

	bool HasApproximation(const FMantleLandscapeMaterialRead& Read, const TCHAR* Word)
	{
		for (const FString& Text : Read.Approximations)
		{
			if (Text.Contains(Word, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	template <typename TExpression>
	TExpression* AddFunctionExpression(UMaterialFunction* Function)
	{
		TExpression* Expression = NewObject<TExpression>(Function);
		Function->GetExpressionCollection().AddExpression(Expression);
		return Expression;
	}

	/**
	 * A cut-down MF_Landscape_Layer_Base: texture inputs "Base Color" and "Normal" and a scalar "UV Near Tilling", the
	 * landscape layer coordinates times the tiling as the coordinates of a sample of each, and two outputs, "Result" and "Normal".
	 */
	UMaterialFunction* MakeLayerFunction()
	{
		UMaterialFunction* Function = NewObject<UMaterialFunction>(GetTransientPackage());
		auto AddInput = [Function](const TCHAR* Name, const EFunctionInputType Type, const int32 Priority)
		{
			UMaterialExpressionFunctionInput* Input = AddFunctionExpression<UMaterialExpressionFunctionInput>(Function);
			Input->InputName = Name;
			Input->Id = FGuid::NewGuid();
			Input->InputType = Type;
			Input->SortPriority = Priority;
			return Input;
		};
		auto AddOutput = [Function](const TCHAR* Name, UMaterialExpression* Source, const int32 Priority)
		{
			UMaterialExpressionFunctionOutput* Output = AddFunctionExpression<UMaterialExpressionFunctionOutput>(Function);
			Output->OutputName = Name;
			Output->Id = FGuid::NewGuid();
			Output->SortPriority = Priority;
			Output->A.Connect(0, Source);
		};
		UMaterialExpressionFunctionInput* ColorInput = AddInput(TEXT("Base Color"), FunctionInput_Texture2D, 1);
		UMaterialExpressionFunctionInput* NormalInput = AddInput(TEXT("Normal"), FunctionInput_Texture2D, 2);
		UMaterialExpressionFunctionInput* NearInput = AddInput(TEXT("UV Near Tilling"), FunctionInput_Scalar, 0);

		UMaterialExpressionLandscapeLayerCoords* Coordinates = AddFunctionExpression<UMaterialExpressionLandscapeLayerCoords>(Function);
		Coordinates->MappingScale = 1.0f;
		UMaterialExpressionMultiply* Uv = AddFunctionExpression<UMaterialExpressionMultiply>(Function);
		Uv->A.Connect(0, Coordinates);
		Uv->B.Connect(0, NearInput);
		UMaterialExpressionTextureSample* ColorSample = AddFunctionExpression<UMaterialExpressionTextureSample>(Function);
		ColorSample->Coordinates.Connect(0, Uv);
		ColorSample->TextureObject.Connect(0, ColorInput);
		UMaterialExpressionTextureSample* NormalSample = AddFunctionExpression<UMaterialExpressionTextureSample>(Function);
		NormalSample->Coordinates.Connect(0, Uv);
		NormalSample->TextureObject.Connect(0, NormalInput);
		AddOutput(TEXT("Result"), ColorSample, 0);
		AddOutput(TEXT("Normal"), NormalSample, 1);
		return Function;
	}

	int32 OutputIndexOf(const UMaterialExpressionMaterialFunctionCall* Call, const TCHAR* Name)
	{
		for (int32 Index = 0; Index < Call->FunctionOutputs.Num(); ++Index)
		{
			if (Call->FunctionOutputs[Index].Output.OutputName == FName(Name))
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	/** One call of MakeLayerFunction for Layer, its inputs the parameters "<Layer> Base Color", "<Layer> Normal" and "<Layer> UV Near Tilling" (as the Spruce master material names them). */
	UMaterialExpressionMaterialFunctionCall* AddLayerCall(UMaterial* Material, UMaterialFunction* Function, const FString& Layer, UTexture* DefaultColor, UTexture* DefaultNormal,
		const float DefaultNear)
	{
		auto AddTextureParameter = [Material](const FString& Name, UTexture* Texture)
		{
			UMaterialExpressionTextureObjectParameter* Parameter = AddExpression<UMaterialExpressionTextureObjectParameter>(Material);
			Parameter->ParameterName = FName(*Name);
			Parameter->Texture = Texture;
			return Parameter;
		};
		UMaterialExpressionMaterialFunctionCall* Call = AddExpression<UMaterialExpressionMaterialFunctionCall>(Material);
		Call->SetMaterialFunction(Function);
		UMaterialExpressionScalarParameter* Near = AddExpression<UMaterialExpressionScalarParameter>(Material);
		Near->ParameterName = FName(*(Layer + TEXT(" UV Near Tilling")));
		Near->DefaultValue = DefaultNear;
		UMaterialExpressionTextureObjectParameter* Color = AddTextureParameter(Layer + TEXT(" Base Color"), DefaultColor);
		UMaterialExpressionTextureObjectParameter* Normal = AddTextureParameter(Layer + TEXT(" Normal"), DefaultNormal);
		for (FFunctionExpressionInput& Entry : Call->FunctionInputs)
		{
			const FName Name = Entry.ExpressionInput ? Entry.ExpressionInput->InputName : FName();
			if (Name == FName(TEXT("Base Color")))
			{
				Entry.Input.Connect(0, Color);
			}
			else if (Name == FName(TEXT("Normal")))
			{
				Entry.Input.Connect(0, Normal);
			}
			else if (Name == FName(TEXT("UV Near Tilling")))
			{
				Entry.Input.Connect(0, Near);
			}
		}
		return Call;
	}

	/**
	 * The Spruce material's slope overlay, built in code: a layer blend of Grass and Dirt (coordinates 4 and 2), then a Lerp
	 * from Rock (the overlay, coordinates 4) to the blend by Clamp((Rock.R + Mask) * Mask), where Mask is the world normal's Z
	 * times "Slope Mask Multiplier" (1.3) to the "Slope Mask Power" (10): 13.8 on flat ground, nearly 0 on a steep slope.
	 */
	UMaterial* MakeSlopeOverlayMaterial(UTexture2D*& OutRock)
	{
		UMaterial* Material = NewObject<UMaterial>(GetTransientPackage());
		UTexture2D* Grass = UTexture2D::CreateTransient(4, 4);
		UTexture2D* Dirt = UTexture2D::CreateTransient(4, 4);
		OutRock = UTexture2D::CreateTransient(4, 4);
		UMaterialExpressionLandscapeLayerBlend* Blend = AddBlend(Material, TEXT("Grass"), AddSample(Material, Grass, AddLayerCoords(Material, 4.0f)),
			TEXT("Dirt"), AddSample(Material, Dirt, AddLayerCoords(Material, 2.0f)));

		UMaterialExpressionVertexNormalWS* Normal = AddExpression<UMaterialExpressionVertexNormalWS>(Material);
		UMaterialExpressionComponentMask* NormalZ = AddExpression<UMaterialExpressionComponentMask>(Material);
		NormalZ->Input.Connect(0, Normal);
		NormalZ->R = false;
		NormalZ->G = false;
		NormalZ->B = true;
		NormalZ->A = false;
		UMaterialExpressionScalarParameter* Multiplier = AddExpression<UMaterialExpressionScalarParameter>(Material);
		Multiplier->ParameterName = TEXT("Slope Mask Multiplier");
		Multiplier->DefaultValue = 1.3f;
		UMaterialExpressionMultiply* Scaled = AddExpression<UMaterialExpressionMultiply>(Material);
		Scaled->A.Connect(0, NormalZ);
		Scaled->B.Connect(0, Multiplier);
		UMaterialExpressionScalarParameter* Exponent = AddExpression<UMaterialExpressionScalarParameter>(Material);
		Exponent->ParameterName = TEXT("Slope Mask Power");
		Exponent->DefaultValue = 10.0f;
		UMaterialExpressionPower* Mask = AddExpression<UMaterialExpressionPower>(Material);
		Mask->Base.Connect(0, Scaled);
		Mask->Exponent.Connect(0, Exponent);

		// The overlay layer's own height (its colour's red), added to the mask and multiplied by it, as the Spruce master does.
		UMaterialExpressionTextureSample* RockSample = AddSample(Material, OutRock, AddLayerCoords(Material, 4.0f));
		UMaterialExpressionComponentMask* RockHeight = AddExpression<UMaterialExpressionComponentMask>(Material);
		RockHeight->Input.Connect(0, RockSample);
		RockHeight->R = true;
		RockHeight->G = false;
		RockHeight->B = false;
		RockHeight->A = false;
		UMaterialExpressionAdd* Sum = AddExpression<UMaterialExpressionAdd>(Material);
		Sum->A.Connect(0, RockHeight);
		Sum->B.Connect(0, Mask);
		UMaterialExpressionMultiply* Product = AddExpression<UMaterialExpressionMultiply>(Material);
		Product->A.Connect(0, Sum);
		Product->B.Connect(0, Mask);
		UMaterialExpressionClamp* Alpha = AddExpression<UMaterialExpressionClamp>(Material);
		Alpha->Input.Connect(0, Product);

		UMaterialExpressionLinearInterpolate* Overlay = AddExpression<UMaterialExpressionLinearInterpolate>(Material);
		Overlay->A.Connect(0, RockSample);
		Overlay->B.Connect(0, Blend);
		Overlay->Alpha.Connect(0, Alpha);
		Material->GetEditorOnlyData()->BaseColor.Connect(0, Overlay);
		return Material;
	}

	/** A Width x Height source whose height at column X is Base + X * Rise (a ramp along X, flat when Rise is 0), 100 cm between samples and 1 cm per height value. */
	FMantleTerrainSource MakeRampSource(const int32 Width, const int32 Height, const uint16 Base, const uint16 Rise)
	{
		FMantleTerrainSource Source;
		Source.Width = Width;
		Source.Height = Height;
		Source.SpacingX = 100.0;
		Source.SpacingY = 100.0;
		Source.HeightScale = 1.0;
		Source.Heights.SetNumUninitialized(Width * Height);
		for (int32 Y = 0; Y < Height; ++Y)
		{
			for (int32 X = 0; X < Width; ++X)
			{
				Source.Heights[Y * Width + X] = static_cast<uint16>(Base + X * Rise);
			}
		}
		return Source;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainFoldKeepsMostPaintedTest,
	"Mantle.Editor.Terrain.FoldKeepsMostPainted",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainFoldKeepsMostPaintedTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainTestPrivate;

	// Six layers, the 4 most painted are D (200), B (100), F (90) and A (50); C (10) and E (5) are folded.
	FMantleTerrainSource Source = MakeSource(
		{ TEXT("A"), TEXT("B"), TEXT("C"), TEXT("D"), TEXT("E"), TEXT("F") }, { 50, 100, 10, 200, 5, 90 });
	Source.LayerColours.Add(TEXT("A"), FLinearColor(0.1f, 0.5f, 0.1f));	// grass
	Source.LayerColours.Add(TEXT("B"), FLinearColor(0.4f, 0.3f, 0.2f));	// dirt
	Source.LayerColours.Add(TEXT("C"), FLinearColor(0.12f, 0.45f, 0.1f));	// nearest to A
	Source.LayerColours.Add(TEXT("D"), FLinearColor(0.5f, 0.5f, 0.5f));	// rock
	Source.LayerColours.Add(TEXT("E"), FLinearColor(0.45f, 0.28f, 0.2f));	// nearest to B
	Source.LayerColours.Add(TEXT("F"), FLinearColor(0.9f, 0.9f, 0.95f));	// snow

	const TArray<int32> All = { 0, 1, 2, 3, 4, 5 };
	const FMantleTerrainFold Fold = FMantleTerrainExport::PlanFold(Source, All, 4);
	TestEqual(TEXT("four layers are kept"), Fold.Kept.Num(), 4);
	TestTrue(TEXT("the most painted are kept, in the landscape's order"), Fold.Kept == TArray<int32>({ 0, 1, 3, 5 }));
	TestEqual(TEXT("a kept layer keeps its own slot"), Fold.SlotOfSource[3], 2);
	TestTrue(TEXT("C is folded"), Fold.IsFolded(2));
	TestTrue(TEXT("E is folded"), Fold.IsFolded(4));
	TestFalse(TEXT("D is not"), Fold.IsFolded(3));
	TestEqual(TEXT("C goes into A, the nearest colour"), Fold.Kept[Fold.SlotOfSource[2]], 0);
	TestEqual(TEXT("E goes into B, the nearest colour"), Fold.Kept[Fold.SlotOfSource[4]], 1);

	// Colours not known: a folded layer goes into the most painted one (D).
	FMantleTerrainSource NoColours = Source;
	NoColours.LayerColours.Reset();
	const FMantleTerrainFold Plain = FMantleTerrainExport::PlanFold(NoColours, All, 4);
	TestEqual(TEXT("without colours C goes into the most painted layer"), Plain.Kept[Plain.SlotOfSource[2]], 3);
	TestEqual(TEXT("and so does E"), Plain.Kept[Plain.SlotOfSource[4]], 3);

	// No more layers than the limit: nothing is folded; a candidate list leaves a layer out.
	const FMantleTerrainFold Few = FMantleTerrainExport::PlanFold(Source, TArray<int32>({ 0, 1, 3 }), 4);
	TestTrue(TEXT("three candidates are all kept"), Few.Kept == TArray<int32>({ 0, 1, 3 }));
	TestEqual(TEXT("a layer that was not a candidate has no slot"), Few.SlotOfSource[5], static_cast<int32>(INDEX_NONE));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainFoldedWeightmapTest,
	"Mantle.Editor.Terrain.FoldedWeightmap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainFoldedWeightmapTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainTestPrivate;

	// A (kept) 100, B (kept) 100, C 100 folded into B: the weightmap is half A, half B, with nothing in C's own channel.
	const FMantleTerrainSource Source = MakeSource({ TEXT("A"), TEXT("B"), TEXT("C") }, { 100, 100, 100 });
	TArray<FMantleTerrainLayerChoice> Layers;
	FMantleTerrainLayerChoice& First = Layers.AddDefaulted_GetRef();
	First.Layer = TEXT("A");
	First.SourceIndex = 0;
	FMantleTerrainLayerChoice& Second = Layers.AddDefaulted_GetRef();
	Second.Layer = TEXT("B");
	Second.SourceIndex = 1;
	Second.FoldedSources.Add(2);

	const TArray<int32> Channels = FMantleTerrainExport::FoldChannels(Layers, 3);
	TestEqual(TEXT("A paints channel 0"), Channels[0], 0);
	TestEqual(TEXT("B paints channel 1"), Channels[1], 1);
	TestEqual(TEXT("C, folded into B, paints channel 1 too"), Channels[2], 1);

	const TArray<int32> SourceLayers = { 0, 1 };
	const TArray<FColor> Pixels = FMantleTerrainExport::MakeWeightmap(Source, SourceLayers, FIntPoint(2, 2), Channels);
	if (!TestEqual(TEXT("a pixel per texel"), Pixels.Num(), 4))
	{
		return false;
	}
	for (const FColor& Pixel : Pixels)
	{
		TestEqual(TEXT("A keeps a third of the paint"), static_cast<int32>(Pixel.R), 85);
		TestEqual(TEXT("B takes its own and C's"), static_cast<int32>(Pixel.G), 170);
		TestEqual(TEXT("nothing is left in the unused channels"), static_cast<int32>(Pixel.B) + Pixel.A, 0);
		TestEqual(TEXT("and the weights sum to 255"), static_cast<int32>(Pixel.R) + Pixel.G, 255);
	}

	// Without the fold map C's paint is lost, as before: A and B share the whole weightmap.
	const TArray<FColor> Plain = FMantleTerrainExport::MakeWeightmap(Source, SourceLayers, FIntPoint(2, 2));
	TestTrue(TEXT("without the map A and B share the whole weightmap"), Plain[0].R >= 127 && Plain[0].R <= 128 && Plain[0].R + Plain[0].G == 255);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleLandscapeMaterialReaderLayerBlendTest,
	"Mantle.Editor.Terrain.LandscapeMaterialLayerBlend",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleLandscapeMaterialReaderLayerBlendTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainTestPrivate;

	// A Landscape Layer Blend over two texture samples, each on its own layer coordinates.
	UMaterial* Material = NewObject<UMaterial>(GetTransientPackage());
	UTexture2D* Grass = UTexture2D::CreateTransient(4, 4);
	UTexture2D* Rock = UTexture2D::CreateTransient(4, 4);
	UMaterialExpressionTextureSample* GrassSample = AddSample(Material, Grass, AddLayerCoords(Material, 4.0f));
	UMaterialExpressionTextureSample* RockSample = AddSample(Material, Rock, AddLayerCoords(Material, 8.0f));
	UMaterialExpressionLandscapeLayerBlend* Blend = AddBlend(Material, TEXT("Grass"), GrassSample, TEXT("Rock"), RockSample);
	Material->GetEditorOnlyData()->BaseColor.Connect(0, Blend);

	FMantleLandscapeMaterialRead Read;
	FString Problem;
	if (!TestTrue(TEXT("the material is read"), FMantleLandscapeMaterialReader::Read(Material, 100.0, Read, Problem)))
	{
		AddError(Problem);
		return false;
	}
	if (!TestEqual(TEXT("both layers are found"), Read.Layers.Num(), 2))
	{
		return false;
	}
	const FMantleLandscapeLayerLook* GrassLook = Read.Find(TEXT("Grass"));
	const FMantleLandscapeLayerLook* RockLook = Read.Find(TEXT("Rock"));
	if (TestNotNull(TEXT("Grass"), GrassLook) && TestNotNull(TEXT("Rock"), RockLook))
	{
		TestTrue(TEXT("Grass reads its texture"), GrassLook->BaseColor == Grass);
		TestTrue(TEXT("Rock reads its texture"), RockLook->BaseColor == Rock);
		// One UV unit is a landscape sample (100 cm here): a mapping scale of 4 repeats every 400 cm.
		TestEqual(TEXT("Grass repeats every 400 cm"), GrassLook->TilingCm(), 400.0);
		TestEqual(TEXT("Rock repeats every 800 cm"), RockLook->TilingCm(), 800.0);
		TestEqual(TEXT("a plain layer approximates nothing"), GrassLook->Approximations.Num(), 0);

		// What the same look is called, and that another layer has another name.
		const FMantleLandscapeLayerLook Copy = *GrassLook;
		TestEqual(TEXT("the same look has the same material name"), FMantleLandscapeMaterialReader::LayerMaterialName(Copy),
			FMantleLandscapeMaterialReader::LayerMaterialName(*GrassLook));
		TestNotEqual(TEXT("another layer has another name"), FMantleLandscapeMaterialReader::LayerMaterialName(*RockLook),
			FMantleLandscapeMaterialReader::LayerMaterialName(*GrassLook));
	}
	TestEqual(TEXT("and nothing is said about the landscape"), Read.Approximations.Num(), 0);

	// The landscape's own spacing scales it.
	FMantleLandscapeMaterialRead Scaled;
	FMantleLandscapeMaterialReader::Read(Material, 200.0, Scaled, Problem);
	if (const FMantleLandscapeLayerLook* ScaledGrass = Scaled.Find(TEXT("Grass")))
	{
		TestEqual(TEXT("at 200 cm a sample, Grass repeats every 800 cm"), ScaledGrass->TilingCm(), 800.0);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleLandscapeMaterialReaderApproximationsTest,
	"Mantle.Editor.Terrain.LandscapeMaterialApproximations",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleLandscapeMaterialReaderApproximationsTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainTestPrivate;

	// A layer blend, then a Lerp over it by the world normal to a third texture: a slope overlay the exporter cannot bake (it
	// reads the normal's Y, not its Z, so it is reported, as before the Z overlay was baked). A layer whose coordinates
	// are Texture Coordinate 0 times 0.25 (a repeat every 4 samples).
	UMaterial* Material = NewObject<UMaterial>(GetTransientPackage());
	UTexture2D* Grass = UTexture2D::CreateTransient(4, 4);
	UTexture2D* Dirt = UTexture2D::CreateTransient(4, 4);
	UTexture2D* Snow = UTexture2D::CreateTransient(4, 4);

	UMaterialExpressionTextureCoordinate* TexCoord = AddExpression<UMaterialExpressionTextureCoordinate>(Material);
	UMaterialExpressionMultiply* Scaled = AddExpression<UMaterialExpressionMultiply>(Material);
	Scaled->A.Connect(0, TexCoord);
	Scaled->ConstB = 0.25f;
	UMaterialExpressionTextureSample* GrassSample = AddSample(Material, Grass, Scaled);
	UMaterialExpressionTextureSample* DirtSample = AddSample(Material, Dirt, AddLayerCoords(Material, 2.0f));
	UMaterialExpressionLandscapeLayerBlend* Blend = AddBlend(Material, TEXT("Grass"), GrassSample, TEXT("Dirt"), DirtSample);

	UMaterialExpressionVertexNormalWS* Normal = AddExpression<UMaterialExpressionVertexNormalWS>(Material);
	UMaterialExpressionComponentMask* Up = AddExpression<UMaterialExpressionComponentMask>(Material);
	Up->Input.Connect(0, Normal);
	Up->R = false;
	Up->G = true;
	Up->B = false;
	Up->A = false;
	UMaterialExpressionTextureSample* SnowSample = AddSample(Material, Snow, nullptr);
	UMaterialExpressionLinearInterpolate* Overlay = AddExpression<UMaterialExpressionLinearInterpolate>(Material);
	Overlay->A.Connect(0, Blend);
	Overlay->B.Connect(0, SnowSample);
	Overlay->Alpha.Connect(0, Up);
	Material->GetEditorOnlyData()->BaseColor.Connect(0, Overlay);

	FMantleLandscapeMaterialRead Read;
	FString Problem;
	if (!TestTrue(TEXT("the material is read"), FMantleLandscapeMaterialReader::Read(Material, 100.0, Read, Problem)))
	{
		AddError(Problem);
		return false;
	}
	TestEqual(TEXT("both layers are found under the overlay"), Read.Layers.Num(), 2);
	TestFalse(TEXT("an overlay on the normal's Y is not baked"), Read.SlopeOverlay.bFound);
	if (const FMantleLandscapeLayerLook* GrassLook = Read.Find(TEXT("Grass")))
	{
		TestTrue(TEXT("Grass reads its texture"), GrassLook->BaseColor == Grass);
		TestEqual(TEXT("Texture Coordinate times 0.25 repeats every 400 cm"), GrassLook->TilingCm(), 400.0);
	}
	if (const FMantleLandscapeLayerLook* DirtLook = Read.Find(TEXT("Dirt")))
	{
		TestEqual(TEXT("a mapping scale of 2 repeats every 200 cm"), DirtLook->TilingCm(), 200.0);
	}
	// Never silent: the slope overlay, and the snow texture that sits outside every layer.
	TestTrue(TEXT("the slope overlay is named"), HasApproximation(Read, TEXT("slope")));
	TestTrue(TEXT("the texture outside the layers is named"), HasApproximation(Read, TEXT("outside the paint layers")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleLandscapeMaterialReaderLayerSampleTest,
	"Mantle.Editor.Terrain.LandscapeMaterialLayerSample",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleLandscapeMaterialReaderLayerSampleTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainTestPrivate;

	// A Lerp whose alpha is a Layer Sample: its B is that layer's look, its A is the rest.
	UMaterial* Material = NewObject<UMaterial>(GetTransientPackage());
	UTexture2D* Base = UTexture2D::CreateTransient(4, 4);
	UTexture2D* Path = UTexture2D::CreateTransient(4, 4);
	UMaterialExpressionTextureSample* BaseSample = AddSample(Material, Base, AddLayerCoords(Material, 4.0f));
	UMaterialExpressionTextureSample* PathSample = AddSample(Material, Path, AddLayerCoords(Material, 1.0f));
	UMaterialExpressionLandscapeLayerSample* PathWeight = AddExpression<UMaterialExpressionLandscapeLayerSample>(Material);
	PathWeight->ParameterName = TEXT("Path");
	UMaterialExpressionLinearInterpolate* Lerp = AddExpression<UMaterialExpressionLinearInterpolate>(Material);
	Lerp->A.Connect(0, BaseSample);
	Lerp->B.Connect(0, PathSample);
	Lerp->Alpha.Connect(0, PathWeight);
	Material->GetEditorOnlyData()->BaseColor.Connect(0, Lerp);

	FMantleLandscapeMaterialRead Read;
	FString Problem;
	if (!TestTrue(TEXT("the material is read"), FMantleLandscapeMaterialReader::Read(Material, 100.0, Read, Problem)))
	{
		AddError(Problem);
		return false;
	}
	const FMantleLandscapeLayerLook* PathLook = Read.Find(TEXT("Path"));
	if (TestNotNull(TEXT("the layer the Layer Sample names"), PathLook))
	{
		TestTrue(TEXT("its look is the Lerp's B"), PathLook->BaseColor == Path);
		TestEqual(TEXT("repeating every 100 cm"), PathLook->TilingCm(), 100.0);
	}
	TestTrue(TEXT("the unmasked base texture is said to be outside the layers"), HasApproximation(Read, TEXT("outside the paint layers")));

	// A material with no layer blend of any kind says so, as a problem rather than a guess.
	UMaterial* Plain = NewObject<UMaterial>(GetTransientPackage());
	Plain->GetEditorOnlyData()->BaseColor.Connect(0, AddSample(Plain, Base, nullptr));
	FMantleLandscapeMaterialRead Nothing;
	TestFalse(TEXT("a material that names no layer reads as nothing"), FMantleLandscapeMaterialReader::Read(Plain, 100.0, Nothing, Problem));
	TestFalse(TEXT("with a reason"), Problem.IsEmpty());
	TestEqual(TEXT("and no layers"), Nothing.Layers.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleLandscapeMaterialReaderInstanceLayersTest,
	"Mantle.Editor.Terrain.LandscapeMaterialInstanceLayers",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleLandscapeMaterialReaderInstanceLayersTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainTestPrivate;

	// The Spruce pattern: five layers, each a call of one layer function whose textures are parameters of the master material
	// (all defaulting to Grass), a material instance that overrides the textures of L4 and L5 and the tiling of L5 and leaves
	// L1 to L3 alone. L5 must read like L4 does: its own albedo and normal, and its own tiling.
	UMaterial* Material = NewObject<UMaterial>(GetTransientPackage());
	UTexture2D* Grass = UTexture2D::CreateTransient(4, 4);
	UTexture2D* GrassNormal = UTexture2D::CreateTransient(4, 4);
	GrassNormal->CompressionSettings = TC_Normalmap;
	UTexture2D* MudAlbedo = UTexture2D::CreateTransient(4, 4);
	UTexture2D* MudNormal = UTexture2D::CreateTransient(4, 4);
	MudNormal->CompressionSettings = TC_Normalmap;
	UTexture2D* RiverAlbedo = UTexture2D::CreateTransient(4, 4);
	UTexture2D* RiverNormal = UTexture2D::CreateTransient(4, 4);
	RiverNormal->CompressionSettings = TC_Normalmap;

	UMaterialFunction* Function = MakeLayerFunction();
	UMaterialExpressionLandscapeLayerBlend* ColourBlend = AddExpression<UMaterialExpressionLandscapeLayerBlend>(Material);
	UMaterialExpressionLandscapeLayerBlend* NormalBlend = AddExpression<UMaterialExpressionLandscapeLayerBlend>(Material);
	for (int32 Layer = 1; Layer <= 5; ++Layer)
	{
		const FString Name = FString::Printf(TEXT("L%d"), Layer);
		UMaterialExpressionMaterialFunctionCall* Call = AddLayerCall(Material, Function, Name, Grass, GrassNormal, 1.0f);
		const int32 ColourOutput = OutputIndexOf(Call, TEXT("Result"));
		const int32 NormalOutput = OutputIndexOf(Call, TEXT("Normal"));
		if (!TestTrue(TEXT("the function call has both outputs"), ColourOutput != INDEX_NONE && NormalOutput != INDEX_NONE))
		{
			return false;
		}
		FLayerBlendInput& ColourLayer = ColourBlend->Layers.AddDefaulted_GetRef();
		ColourLayer.LayerName = FName(*Name);
		ColourLayer.LayerInput.Connect(ColourOutput, Call);
		FLayerBlendInput& NormalLayer = NormalBlend->Layers.AddDefaulted_GetRef();
		NormalLayer.LayerName = FName(*Name);
		NormalLayer.LayerInput.Connect(NormalOutput, Call);
	}
	Material->GetEditorOnlyData()->BaseColor.Connect(0, ColourBlend);
	Material->GetEditorOnlyData()->Normal.Connect(0, NormalBlend);
	UMaterialInstanceConstant* Instance = NewObject<UMaterialInstanceConstant>(GetTransientPackage());
	Instance->SetParentEditorOnly(Material, false);
	Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("L4 Base Color")), MudAlbedo);
	Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("L4 Normal")), MudNormal);
	Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("L4 UV Near Tilling")), 0.5f);
	Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("L5 Base Color")), RiverAlbedo);
	Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("L5 Normal")), RiverNormal);
	Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("L5 UV Near Tilling")), 0.25f);

	FMantleLandscapeMaterialRead Read;
	FString Problem;
	if (!TestTrue(TEXT("the instance is read"), FMantleLandscapeMaterialReader::Read(Instance, 100.0, Read, Problem)))
	{
		AddError(Problem);
		return false;
	}
	if (!TestEqual(TEXT("all five layers are found"), Read.Layers.Num(), 5))
	{
		return false;
	}
	const FMantleLandscapeLayerLook* L1 = Read.Find(TEXT("L1"));
	const FMantleLandscapeLayerLook* L4 = Read.Find(TEXT("L4"));
	const FMantleLandscapeLayerLook* L5 = Read.Find(TEXT("L5"));
	if (TestNotNull(TEXT("L1"), L1) && TestNotNull(TEXT("L4"), L4) && TestNotNull(TEXT("L5"), L5))
	{
		TestTrue(TEXT("L1 keeps the master's default albedo"), L1->BaseColor == Grass);
		TestEqual(TEXT("and repeats every 100 cm"), L1->TilingCm(), 100.0);
		TestTrue(TEXT("L4 reads the instance's mud"), L4->BaseColor == MudAlbedo && L4->Normal == MudNormal);
		TestEqual(TEXT("L4 repeats every 200 cm"), L4->TilingCm(), 200.0);
		TestTrue(TEXT("L5 reads the instance's river albedo"), L5->BaseColor == RiverAlbedo);
		TestTrue(TEXT("and its river normal"), L5->Normal == RiverNormal);
		TestEqual(TEXT("L5 repeats every 400 cm"), L5->TilingCm(), 400.0);
		TestFalse(TEXT("L5 is readable"), L5->bUnreadable);
		TestEqual(TEXT("L5 approximates nothing"), L5->Approximations.Num(), 0);
	}
	TestEqual(TEXT("the material says nothing about the landscape"), Read.Approximations.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleLandscapeMaterialReaderSlopeOverlayTest,
	"Mantle.Editor.Terrain.LandscapeMaterialSlopeOverlay",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleLandscapeMaterialReaderSlopeOverlayTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainTestPrivate;

	UTexture2D* Rock = nullptr;
	UMaterial* Material = MakeSlopeOverlayMaterial(Rock);
	FMantleLandscapeMaterialRead Read;
	FString Problem;
	if (!TestTrue(TEXT("the material is read"), FMantleLandscapeMaterialReader::Read(Material, 100.0, Read, Problem)))
	{
		AddError(Problem);
		return false;
	}
	if (!TestTrue(TEXT("the slope overlay is found"), Read.SlopeOverlay.bFound))
	{
		return false;
	}
	TestEqual(TEXT("it is the layer 'Slope'"), Read.SlopeOverlay.Layer, FName(TEXT("Slope")));
	TestEqual(TEXT("the painted layers and the overlay"), Read.Layers.Num(), 3);
	const FMantleLandscapeLayerLook* Look = Read.Find(TEXT("Slope"));
	if (TestNotNull(TEXT("the overlay's look"), Look))
	{
		TestTrue(TEXT("it is the rock texture"), Look->BaseColor == Rock);
		TestEqual(TEXT("repeating every 400 cm"), Look->TilingCm(), 400.0);
		TestTrue(TEXT("it says it is baked"), Look->Approximations.Contains(FMantleLandscapeSlopeOverlay::BakedNote()));
	}
	TestTrue(TEXT("Grass is a paint layer"), Read.Find(TEXT("Grass")) != nullptr);
	TestTrue(TEXT("Dirt is a paint layer"), Read.Find(TEXT("Dirt")) != nullptr);

	// Baked, so no longer a loss: nothing about the slope or a texture outside the layers is said of the landscape.
	TestFalse(TEXT("the slope is not reported as lost"), HasApproximation(Read, TEXT("slope")));
	TestFalse(TEXT("nor the overlay's texture as outside the layers"), HasApproximation(Read, TEXT("outside the paint layers")));

	// The mask the material makes, (1.3 * Z) ^ 10 added to the height (0.5, no source to average) and multiplied by itself, clamped, then
	// taken from the overlay: no overlay on flat ground, all of it on a steep slope.
	const FMantleLandscapeSlopeOverlay& Overlay = Read.SlopeOverlay;
	TestEqual(TEXT("one table entry a 1/1024"), Overlay.WeightByNormalZ.Num(), FMantleLandscapeSlopeOverlay::LutSize);
	TestTrue(TEXT("flat ground shows none of it"), Overlay.WeightAt(1.0) < 0.001f);
	TestTrue(TEXT("a slope of 60 degrees (Z 0.5) shows nearly all of it"), Overlay.WeightAt(0.5) > 0.98f);
	TestTrue(TEXT("a cliff shows all of it"), Overlay.WeightAt(0.0) > 0.99f);
	// Z 0.7: mask 0.91^10 = 0.389, (0.5 + 0.389) * 0.389 = 0.346, so 0.654 of the overlay.
	TestTrue(TEXT("Z 0.7 shows about two thirds"), FMath::IsNearlyEqual(Overlay.WeightAt(0.7), 0.654f, 0.02f));
	TestTrue(TEXT("steeper shows more"), Overlay.WeightAt(0.6) > Overlay.WeightAt(0.7) && Overlay.WeightAt(0.7) > Overlay.WeightAt(0.8));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainSlopeMaskBakeTest,
	"Mantle.Editor.Terrain.SlopeMaskBake",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainSlopeMaskBakeTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainTestPrivate;

	UTexture2D* Rock = nullptr;
	UMaterial* Material = MakeSlopeOverlayMaterial(Rock);
	FMantleLandscapeMaterialRead Read;
	FString Problem;
	if (!TestTrue(TEXT("the material is read"), FMantleLandscapeMaterialReader::Read(Material, 100.0, Read, Problem)) || !TestTrue(TEXT("with its overlay"), Read.SlopeOverlay.bFound))
	{
		return false;
	}

	// Flat: no overlay anywhere.
	const FMantleTerrainSource Flat = MakeRampSource(8, 8, 1000, 0);
	const TArray<uint8> FlatMask = FMantleTerrainExport::BakeSlopeMask(Flat, Read.SlopeOverlay);
	if (TestEqual(TEXT("a weight per sample"), FlatMask.Num(), 64))
	{
		int32 Most = 0;
		for (const uint8 Weight : FlatMask)
		{
			Most = FMath::Max<int32>(Most, Weight);
		}
		TestEqual(TEXT("flat ground has no overlay"), Most, 0);
	}

	// A ramp that rises 173 cm over 100 cm (atan 1.73 = 60 degrees) along X: the normal's Z is 0.5 on every sample, the edges too.
	const FMantleTerrainSource Ramp = MakeRampSource(8, 8, 1000, 173);
	const TArray<uint8> RampMask = FMantleTerrainExport::BakeSlopeMask(Ramp, Read.SlopeOverlay);
	if (TestEqual(TEXT("a weight per sample on the ramp"), RampMask.Num(), 64))
	{
		int32 Least = 255;
		for (const uint8 Weight : RampMask)
		{
			Least = FMath::Min<int32>(Least, Weight);
		}
		TestTrue(TEXT("a 60 degree slope has about all of the overlay"), Least >= 250);
	}

	// A gentler 30 degree ramp (rise 58 over 100 cm, Z 0.866) is still flat ground to the material's mask: Z * 1.3 is above 1.
	const FMantleTerrainSource Gentle = MakeRampSource(8, 8, 1000, 58);
	const TArray<uint8> GentleMask = FMantleTerrainExport::BakeSlopeMask(Gentle, Read.SlopeOverlay);
	if (TestEqual(TEXT("a weight per sample on the gentle ramp"), GentleMask.Num(), 64))
	{
		TestEqual(TEXT("a 30 degree slope has none"), static_cast<int32>(GentleMask[3 * 8 + 3]), 0);
	}

	// No overlay, or no heights: nothing is baked.
	TestEqual(TEXT("no overlay, no mask"), FMantleTerrainExport::BakeSlopeMask(Ramp, FMantleLandscapeSlopeOverlay()).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainTwoWeightmapsTest,
	"Mantle.Editor.Terrain.TwoWeightmaps",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainTwoWeightmapsTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainTestPrivate;

	TestEqual(TEXT("one layer has no weightmap"), FMantleTerrainExport::WeightmapCount(1), 0);
	TestEqual(TEXT("four layers have one"), FMantleTerrainExport::WeightmapCount(4), 1);
	TestEqual(TEXT("five layers have two"), FMantleTerrainExport::WeightmapCount(5), 2);
	TestEqual(TEXT("six layers have two"), FMantleTerrainExport::WeightmapCount(6), 2);
	TestEqual(TEXT("eight layers have two"), FMantleTerrainExport::WeightmapCount(8), 2);

	UTexture2D* Rock = nullptr;
	UMaterial* Material = MakeSlopeOverlayMaterial(Rock);
	FMantleLandscapeMaterialRead Read;
	FString Problem;
	if (!TestTrue(TEXT("the material is read"), FMantleLandscapeMaterialReader::Read(Material, 100.0, Read, Problem)))
	{
		return false;
	}

	// Demo_01's shape: five painted layers (weights 50, 40, 30, 20, 10 everywhere) and the slope overlay, six layers in all.
	auto WithPaint = [&Read](FMantleTerrainSource Source)
	{
		Source.LayerNames = { TEXT("L1"), TEXT("L2"), TEXT("L3"), TEXT("L4"), TEXT("L5") };
		for (const int32 Weight : { 50, 40, 30, 20, 10 })
		{
			Source.LayerWeights.AddDefaulted_GetRef().Init(static_cast<uint8>(Weight), Source.Width * Source.Height);
		}
		Source.MaterialRead = Read;
		Source.bMaterialRead = true;
		return Source;
	};
	auto MakeLayers = [](const int32 Painted, const bool bOverlay)
	{
		TArray<FMantleTerrainLayerChoice> Layers;
		for (int32 Index = 0; Index < Painted; ++Index)
		{
			FMantleTerrainLayerChoice& Choice = Layers.AddDefaulted_GetRef();
			Choice.Layer = FName(*FString::Printf(TEXT("L%d"), Index + 1));
			Choice.SourceIndex = Index;
		}
		if (bOverlay)
		{
			FMantleTerrainLayerChoice& Overlay = Layers.AddDefaulted_GetRef();
			Overlay.Layer = TEXT("Slope");
			Overlay.bDerived = true;
			Overlay.bSlopeOverlay = true;
		}
		return Layers;
	};
	const TArray<FMantleTerrainLayerChoice> Six = MakeLayers(5, true);
	TestEqual(TEXT("six layers"), Six.Num(), 6);

	// Flat ground: the painted layers share the 255 as their weights say, the overlay none, and the 5th and 6th layers are in the second map.
	{
		const FMantleTerrainSource Source = WithPaint(MakeRampSource(8, 8, 1000, 0));
		TArray<FColor> First;
		TArray<FColor> Second;
		FMantleTerrainExport::MakeWeightmaps(Source, Six, FIntPoint(8, 8), First, Second);
		if (TestEqual(TEXT("a texel each in the first map"), First.Num(), 64) && TestEqual(TEXT("and in the second"), Second.Num(), 64))
		{
			for (int32 Texel = 0; Texel < 64; ++Texel)
			{
				const FColor& A = First[Texel];
				const FColor& B = Second[Texel];
				TestEqual(TEXT("the weights sum to 255 across both maps"), static_cast<int32>(A.R) + A.G + A.B + A.A + B.R + B.G + B.B + B.A, 255);
				TestEqual(TEXT("L1 is a third"), static_cast<int32>(A.R), 85);
				TestEqual(TEXT("L2"), static_cast<int32>(A.G), 68);
				TestEqual(TEXT("L3"), static_cast<int32>(A.B), 51);
				TestEqual(TEXT("L4"), static_cast<int32>(A.A), 34);
				TestEqual(TEXT("L5 is the second map's first channel"), static_cast<int32>(B.R), 17);
				TestEqual(TEXT("the slope overlay is flat ground's none"), static_cast<int32>(B.G), 0);
				TestEqual(TEXT("and the rest of the second map is empty"), static_cast<int32>(B.B) + B.A, 0);
			}
		}
	}

	// A 60 degree ramp: the overlay takes about all of it, the painted layers what is left, the sum still 255.
	{
		const FMantleTerrainSource Source = WithPaint(MakeRampSource(8, 8, 1000, 173));
		TArray<FColor> First;
		TArray<FColor> Second;
		FMantleTerrainExport::MakeWeightmaps(Source, Six, FIntPoint(8, 8), First, Second);
		if (TestEqual(TEXT("a texel each on the ramp"), Second.Num(), 64))
		{
			for (int32 Texel = 0; Texel < 64; ++Texel)
			{
				const FColor& A = First[Texel];
				const FColor& B = Second[Texel];
				TestEqual(TEXT("on the ramp the weights sum to 255"), static_cast<int32>(A.R) + A.G + A.B + A.A + B.R + B.G + B.B + B.A, 255);
				TestTrue(TEXT("on the ramp the overlay has nearly all"), B.G >= 245);
			}
		}
	}

	// Four layers and no overlay: one map, as before, and the second is left out.
	{
		const FMantleTerrainSource Source = WithPaint(MakeRampSource(8, 8, 1000, 0));
		TArray<FColor> First;
		TArray<FColor> Second;
		FMantleTerrainExport::MakeWeightmaps(Source, MakeLayers(4, false), FIntPoint(8, 8), First, Second);
		TestEqual(TEXT("four layers make the first map"), First.Num(), 64);
		TestEqual(TEXT("and no second"), Second.Num(), 0);
	}

	// Eight layers fill both maps; a texel's weights sum to 255 with a folded layer's paint added to the layer it went into.
	{
		FMantleTerrainSource Source = WithPaint(MakeRampSource(8, 8, 1000, 0));
		Source.LayerNames.Add(TEXT("L6"));
		Source.LayerWeights.AddDefaulted_GetRef().Init(static_cast<uint8>(60), 64);
		TArray<FMantleTerrainLayerChoice> Eight = MakeLayers(8, false);
		for (int32 Index = 5; Index < 8; ++Index)
		{
			Eight[Index].SourceIndex = INDEX_NONE;
		}
		Eight[4].FoldedSources.Add(5);
		TArray<FColor> First;
		TArray<FColor> Second;
		FMantleTerrainExport::MakeWeightmaps(Source, Eight, FIntPoint(8, 8), First, Second);
		if (TestEqual(TEXT("eight layers fill both maps"), Second.Num(), 64))
		{
			const FColor& A = First[10];
			const FColor& B = Second[10];
			TestEqual(TEXT("eight layers sum to 255"), static_cast<int32>(A.R) + A.G + A.B + A.A + B.R + B.G + B.B + B.A, 255);
			// L1 to L4 are 50, 40, 30, 20 of 210; L5 (10) has L6 (60) folded in: 70.
			TestEqual(TEXT("L5 holds the paint folded into it"), static_cast<int32>(B.R), 85);
			TestEqual(TEXT("L1"), static_cast<int32>(A.R), 61);
		}
	}

	// The upload form: PNGs of the same size, the second only with more than four layers.
	{
		const FMantleTerrainSource Source = WithPaint(MakeRampSource(8, 8, 1000, 0));
		FIntPoint Size;
		TArray<uint8> FirstPng;
		TArray<uint8> SecondPng;
		if (TestTrue(TEXT("six layers encode"), FMantleTerrainExport::EncodeWeightmaps(Source, Six, 2048, 1 << 20, Size, FirstPng, SecondPng)))
		{
			TestTrue(TEXT("at the grid's size"), Size == FIntPoint(8, 8));
			int32 Width = 0;
			int32 Height = 0;
			bool bAlpha = false;
			TestTrue(TEXT("the first is a PNG"), FMantlePng::ReadHeader(FirstPng, Width, Height, bAlpha) && Width == 8 && Height == 8);
			TestTrue(TEXT("so is the second"), FMantlePng::ReadHeader(SecondPng, Width, Height, bAlpha) && Width == 8 && Height == 8);
		}
		TArray<uint8> OnlyPng;
		TArray<uint8> NoSecond;
		if (TestTrue(TEXT("four layers encode"), FMantleTerrainExport::EncodeWeightmaps(Source, MakeLayers(4, false), 2048, 1 << 20, Size, OnlyPng, NoSecond)))
		{
			TestTrue(TEXT("with no second"), NoSecond.IsEmpty() && !OnlyPng.IsEmpty());
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainFoldBeyondEightTest,
	"Mantle.Editor.Terrain.FoldBeyondEight",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainFoldBeyondEightTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainTestPrivate;

	// Ten painted layers against the limit: the 8 most painted are kept, and the two least painted fold into the nearest by colour.
	TArray<FName> Names;
	TArray<int32> Weights;
	for (int32 Index = 0; Index < 10; ++Index)
	{
		Names.Add(FName(*FString::Printf(TEXT("P%d"), Index)));
		Weights.Add(100 + Index * 10);	// P0 is the least painted, P9 the most
	}
	FMantleTerrainSource Source = MakeSource(Names, Weights);
	for (int32 Index = 0; Index < 10; ++Index)
	{
		// P0 and P1 (folded) are near P8 and P9 in colour.
		const float Grey = Index == 0 ? 0.81f : Index == 1 ? 0.91f : Index * 0.1f;
		Source.LayerColours.Add(Names[Index], FLinearColor(Grey, Grey, Grey));
	}
	TArray<int32> All;
	for (int32 Index = 0; Index < 10; ++Index)
	{
		All.Add(Index);
	}
	const FMantleTerrainFold Fold = FMantleTerrainExport::PlanFold(Source, All, FMantlePlaceLimits::MaxTerrainLayers);
	TestEqual(TEXT("the limit's layers are kept"), Fold.Kept.Num(), FMantlePlaceLimits::MaxTerrainLayers);
	TestTrue(TEXT("P0 is folded"), Fold.IsFolded(0));
	TestTrue(TEXT("P1 is folded"), Fold.IsFolded(1));
	TestFalse(TEXT("P9 is kept"), Fold.IsFolded(9));
	TestEqual(TEXT("P0 goes into P8, the nearest colour"), Fold.Kept[Fold.SlotOfSource[0]], 8);
	TestEqual(TEXT("P1 goes into P9"), Fold.Kept[Fold.SlotOfSource[1]], 9);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
