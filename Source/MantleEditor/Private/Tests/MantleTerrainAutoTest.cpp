// Copyright Joshua Eger. All Rights Reserved.
#include "MantleLandscapeMaterialReader.h"
#include "MantleTerrainExport.h"

#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionLandscapeLayerBlend.h"
#include "Materials/MaterialExpressionLandscapeLayerCoords.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionOneMinus.h"
#include "Materials/MaterialExpressionPixelNormalWS.h"
#include "Materials/MaterialExpressionPower.h"
#include "Materials/MaterialExpressionSaturate.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialFunction.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// A landscape material with no paint layers that picks its ground from the slope and height of the surface: the chain of
// blends is recognised, its masks compiled and run per heightfield sample, and its layers' weights baked. Like the other terrain
// tests, the material graphs are built in code, so no level or asset is needed (except the engine's own SlopeMask function).

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleTerrainAutoTestPrivate
{
	template <typename TExpression>
	TExpression* AddNode(UMaterial* Material)
	{
		TExpression* Expression = NewObject<TExpression>(Material);
		Expression->Material = Material;
		Material->GetExpressionCollection().AddExpression(Expression);
		return Expression;
	}

	template <typename TExpression>
	TExpression* AddFunctionNode(UMaterialFunction* Function)
	{
		TExpression* Expression = NewObject<TExpression>(Function);
		Function->GetExpressionCollection().AddExpression(Expression);
		return Expression;
	}

	UMaterialExpressionVectorParameter* AddColour(UMaterial* Material, const TCHAR* Name, const FLinearColor Value)
	{
		UMaterialExpressionVectorParameter* Parameter = AddNode<UMaterialExpressionVectorParameter>(Material);
		Parameter->ParameterName = Name;
		Parameter->DefaultValue = Value;
		return Parameter;
	}

	UMaterialExpressionScalarParameter* AddScalar(UMaterial* Material, const TCHAR* Name, const float Value)
	{
		UMaterialExpressionScalarParameter* Parameter = AddNode<UMaterialExpressionScalarParameter>(Material);
		Parameter->ParameterName = Name;
		Parameter->DefaultValue = Value;
		return Parameter;
	}

	/** The B component of Source (the normal's Z, for a normal). */
	UMaterialExpressionComponentMask* AddMaskB(UMaterial* Material, UMaterialExpression* Source)
	{
		UMaterialExpressionComponentMask* Mask = AddNode<UMaterialExpressionComponentMask>(Material);
		Mask->Input.Connect(0, Source);
		Mask->R = false;
		Mask->G = false;
		Mask->B = true;
		Mask->A = false;
		return Mask;
	}

	UMaterialExpressionSaturate* AddSaturate(UMaterial* Material, UMaterialExpression* Source)
	{
		UMaterialExpressionSaturate* Saturate = AddNode<UMaterialExpressionSaturate>(Material);
		Saturate->Input.Connect(0, Source);
		return Saturate;
	}

	UMaterialExpressionLinearInterpolate* AddBlend(UMaterial* Material, UMaterialExpression* A, UMaterialExpression* B, UMaterialExpression* Alpha)
	{
		UMaterialExpressionLinearInterpolate* Lerp = AddNode<UMaterialExpressionLinearInterpolate>(Material);
		Lerp->A.Connect(0, A);
		Lerp->B.Connect(0, B);
		Lerp->Alpha.Connect(0, Alpha);
		return Lerp;
	}

	/** The mask a material blends its steep ground in by: (1 - the normal's Z) times Scale, saturated. */
	UMaterialExpression* AddSteepness(UMaterial* Material, const float Scale)
	{
		UMaterialExpressionOneMinus* Flatness = AddNode<UMaterialExpressionOneMinus>(Material);
		Flatness->Input.Connect(0, AddMaskB(Material, AddNode<UMaterialExpressionVertexNormalWS>(Material)));
		UMaterialExpressionMultiply* Scaled = AddNode<UMaterialExpressionMultiply>(Material);
		Scaled->A.Connect(0, Flatness);
		Scaled->ConstB = Scale;
		return AddSaturate(Material, Scaled);
	}

	/** A material function "Blend" of two colour inputs, "Ground Color" and "Rock Color", by the steepness mask above, one output "Result". */
	UMaterialFunction* MakeBlendFunction()
	{
		UMaterialFunction* Function = NewObject<UMaterialFunction>(GetTransientPackage());
		auto AddInput = [Function](const TCHAR* Name, const int32 Priority)
		{
			UMaterialExpressionFunctionInput* Input = AddFunctionNode<UMaterialExpressionFunctionInput>(Function);
			Input->InputName = Name;
			Input->Id = FGuid::NewGuid();
			Input->InputType = FunctionInput_Vector3;
			Input->SortPriority = Priority;
			return Input;
		};
		UMaterialExpressionFunctionInput* Ground = AddInput(TEXT("Ground Color"), 0);
		UMaterialExpressionFunctionInput* Rock = AddInput(TEXT("Rock Color"), 1);

		UMaterialExpressionVertexNormalWS* Normal = AddFunctionNode<UMaterialExpressionVertexNormalWS>(Function);
		UMaterialExpressionComponentMask* NormalZ = AddFunctionNode<UMaterialExpressionComponentMask>(Function);
		NormalZ->Input.Connect(0, Normal);
		NormalZ->R = false;
		NormalZ->G = false;
		NormalZ->B = true;
		NormalZ->A = false;
		UMaterialExpressionOneMinus* Steep = AddFunctionNode<UMaterialExpressionOneMinus>(Function);
		Steep->Input.Connect(0, NormalZ);
		UMaterialExpressionLinearInterpolate* Lerp = AddFunctionNode<UMaterialExpressionLinearInterpolate>(Function);
		Lerp->A.Connect(0, Ground);
		Lerp->B.Connect(0, Rock);
		Lerp->Alpha.Connect(0, Steep);

		UMaterialExpressionFunctionOutput* Output = AddFunctionNode<UMaterialExpressionFunctionOutput>(Function);
		Output->OutputName = TEXT("Result");
		Output->Id = FGuid::NewGuid();
		Output->A.Connect(0, Lerp);
		return Function;
	}

	/** A function input of a call, and what feeds it. */
	struct FCallInput
	{
		FName Name;
		UMaterialExpression* Source = nullptr;
	};

	/** One call of Function, each input named in Inputs fed by the expression after its name. */
	UMaterialExpressionMaterialFunctionCall* AddCall(UMaterial* Material, UMaterialFunction* Function, const TArray<FCallInput>& Inputs)
	{
		UMaterialExpressionMaterialFunctionCall* Call = AddNode<UMaterialExpressionMaterialFunctionCall>(Material);
		Call->SetMaterialFunction(Function);
		for (FFunctionExpressionInput& Entry : Call->FunctionInputs)
		{
			const FName Name = Entry.ExpressionInput ? Entry.ExpressionInput->InputName : FName();
			for (const FCallInput& Input : Inputs)
			{
				if (Input.Name == Name)
				{
					Entry.Input.Connect(0, Input.Source);
				}
			}
		}
		return Call;
	}

	bool HasNote(const TArray<FString>& Notes, const TCHAR* Word)
	{
		for (const FString& Note : Notes)
		{
			if (Note.Contains(Word, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	/** A 4 x 4 transient texture, set to normal-map compression when it is a normal map. */
	UTexture2D* MakeTexture(const bool bNormalMap)
	{
		UTexture2D* Texture = UTexture2D::CreateTransient(4, 4);
		if (bNormalMap)
		{
			Texture->CompressionSettings = TC_Normalmap;
		}
		return Texture;
	}

	UMaterialExpressionTextureSample* AddSample(UMaterial* Material, UTexture* Texture)
	{
		UMaterialExpressionTextureSample* Sample = AddNode<UMaterialExpressionTextureSample>(Material);
		Sample->Texture = Texture;
		return Sample;
	}

	/** The mask snow is blended in by: (the world height - AboveCm) at 0.001 a cm, saturated. Each call is a new node, though two compute the same thing. */
	UMaterialExpression* AddHeightMask(UMaterial* Material, const float AboveCm)
	{
		UMaterialExpressionSubtract* Above = AddNode<UMaterialExpressionSubtract>(Material);
		Above->A.Connect(2, AddNode<UMaterialExpressionWorldPosition>(Material));
		Above->ConstB = AboveCm;
		UMaterialExpressionMultiply* Scaled = AddNode<UMaterialExpressionMultiply>(Material);
		Scaled->A.Connect(0, Above);
		Scaled->ConstB = 0.001f;
		return AddSaturate(Material, Scaled);
	}

	/** A 2 x 2 linear texture whose red is 0 and 255 on the top row, 255 and 0 on the bottom: a checker, for a mask to sample. */
	UTexture2D* MakeCheckerTexture()
	{
		UTexture2D* Texture = NewObject<UTexture2D>(GetTransientPackage());
		const uint8 Bgra[16] = { 0, 0, 0, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 0, 255 };
		Texture->Source.Init(2, 2, 1, 1, TSF_BGRA8, Bgra);
		Texture->SRGB = false;
		Texture->CompressionSettings = TC_Masks;
		Texture->AddressX = TA_Wrap;
		Texture->AddressY = TA_Wrap;
		return Texture;
	}

	/** The material: ground, then rock over it by the red of the checker sampled at Uv. */
	UMaterial* MakeCheckerMaterial(const TFunctionRef<UMaterialExpression*(UMaterial*)>& MakeUv, const bool bConnectUv)
	{
		UMaterial* Material = NewObject<UMaterial>(GetTransientPackage());
		UMaterialExpressionTextureSample* Sample = AddSample(Material, MakeCheckerTexture());
		if (bConnectUv)
		{
			Sample->Coordinates.Connect(0, MakeUv(Material));
		}
		UMaterialExpressionComponentMask* Red = AddNode<UMaterialExpressionComponentMask>(Material);
		Red->Input.Connect(0, Sample);
		Red->R = true;
		Red->G = false;
		Red->B = false;
		Red->A = false;
		// Reads the world position too, so the blend is a mask of the surface's shape.
		UMaterialExpressionAdd* Mask = AddNode<UMaterialExpressionAdd>(Material);
		Mask->A.Connect(0, Red);
		UMaterialExpressionMultiply* Zero = AddNode<UMaterialExpressionMultiply>(Material);
		Zero->A.Connect(2, AddNode<UMaterialExpressionWorldPosition>(Material));
		Zero->ConstB = 0.0f;
		Mask->B.Connect(0, Zero);
		Material->GetEditorOnlyData()->BaseColor.Connect(0,
			AddBlend(Material, AddColour(Material, TEXT("GroundColor"), FLinearColor::Green), AddColour(Material, TEXT("RockColor"), FLinearColor::Gray), AddSaturate(Material, Mask)));
		return Material;
	}

	/** The surface where the ground rises SlopeX in X and SlopeY in Y (cm per cm), at the world position Position: the frame BakeAutoWeights builds. */
	FMantleSurfaceSample MakeSample(const double SlopeX, const double SlopeY, const FVector& Position = FVector::ZeroVector)
	{
		FMantleSurfaceSample Sample;
		Sample.WorldPosition = Position;
		Sample.Normal = FVector(-SlopeX, -SlopeY, 1.0).GetSafeNormal();
		Sample.Tangent = FVector(1.0, 0.0, SlopeX).GetSafeNormal();
		Sample.Binormal = FVector(0.0, 1.0, SlopeY).GetSafeNormal();
		return Sample;
	}

	/** The engine SlopeMask function's closed form for a normal turned from tangent space by the surface's frame: CheapContrast(Power(saturate(T2W(Tangent normal) . up), Falloff), Contrast). */
	double SlopeMaskOf(const FMantleSurfaceSample& Sample, const FVector& TangentNormal, const double Falloff, const double Contrast)
	{
		const FVector World = Sample.Tangent * TangentNormal.X + Sample.Binormal * TangentNormal.Y + Sample.Normal * TangentNormal.Z;
		const double Power = FMath::Pow(FMath::Clamp(World.Z, 0.0, 1.0), Falloff);
		return FMath::Clamp(Power * (1.0 + 2.0 * Contrast) - Contrast, 0.0, 1.0);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainAutoRecognisesChainTest,
	"Mantle.Editor.Terrain.AutoRecognisesChain",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainAutoRecognisesChainTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainAutoTestPrivate;

	// A function blends "Ground Color" and "Rock Color" by the surface's steepness; a variation scalar multiplies that, and
	// a snow colour is blended over it by height: three layers, base first.
	UMaterial* Material = NewObject<UMaterial>(GetTransientPackage());
	UMaterialExpressionMaterialFunctionCall* Call = AddCall(Material, MakeBlendFunction(),
		{ { TEXT("Ground Color"), AddColour(Material, TEXT("GroundTint"), FLinearColor(0.2f, 0.3f, 0.1f)) },
			{ TEXT("Rock Color"), AddColour(Material, TEXT("RockTint"), FLinearColor(0.5f, 0.5f, 0.5f)) } });
	UMaterialExpressionMultiply* Tinted = AddNode<UMaterialExpressionMultiply>(Material);
	Tinted->A.Connect(0, Call);
	Tinted->B.Connect(0, AddScalar(Material, TEXT("Variation"), 1.2f));

	UMaterialExpressionWorldPosition* Position = AddNode<UMaterialExpressionWorldPosition>(Material);
	UMaterialExpressionMultiply* Height = AddNode<UMaterialExpressionMultiply>(Material);
	Height->A.Connect(2, Position);
	Height->B.Connect(0, AddScalar(Material, TEXT("SnowHeightScale"), 0.001f));
	UMaterialExpressionLinearInterpolate* Top = AddBlend(Material, Tinted, AddColour(Material, TEXT("SnowBaseColor"), FLinearColor(0.9f, 0.9f, 0.95f)), AddSaturate(Material, Height));
	Material->GetEditorOnlyData()->BaseColor.Connect(0, Top);

	FMantleLandscapeMaterialRead Read;
	FString Problem;
	if (!TestTrue(TEXT("a material with no paint layer but a slope and height chain is read"), FMantleLandscapeMaterialReader::Read(Material, 100.0, Read, Problem)))
	{
		AddError(Problem);
		return false;
	}
	if (!TestTrue(TEXT("its chain is found"), Read.AutoChain.bFound) || !TestTrue(TEXT("with a program"), Read.AutoChain.Program.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("three layers"), Read.AutoChain.LayerNames.Num(), 3);
	TestEqual(TEXT("one mask for each layer after the base"), Read.AutoChain.Program->NumOutputs(), 2);
	TestEqual(TEXT("a look for each layer"), Read.Layers.Num(), 3);
	if (Read.AutoChain.LayerNames.Num() == 3 && Read.Layers.Num() == 3)
	{
		TestTrue(TEXT("the base is named by the function input, without its Color"), Read.AutoChain.LayerNames[0] == FName(TEXT("Ground")));
		TestTrue(TEXT("the first layer over it by its function input"), Read.AutoChain.LayerNames[1] == FName(TEXT("Rock")));
		TestTrue(TEXT("the last by its parameter, without its BaseColor"), Read.AutoChain.LayerNames[2] == FName(TEXT("Snow")));
		for (int32 Index = 0; Index < 3; ++Index)
		{
			TestTrue(TEXT("the looks are in the chain's order"), Read.Layers[Index].Layer == Read.AutoChain.LayerNames[Index]);
			TestTrue(TEXT("each look says it is baked"), HasNote(Read.Layers[Index].Approximations, TEXT("baked from the material's slope and height")));
		}
		TestTrue(TEXT("a flat colour look is a look"), Read.Layers[0].bHasFlatColour && FMath::IsNearlyEqual(Read.Layers[0].FlatColour.G, 0.3f, 0.001f));
		TestTrue(TEXT("the snow's too"), Read.Layers[2].bHasFlatColour && FMath::IsNearlyEqual(Read.Layers[2].FlatColour.B, 0.95f, 0.001f));
	}
	TestTrue(TEXT("the variation over the layers is said to be lost"), HasNote(Read.Approximations, TEXT("after the layers")));
	TestFalse(TEXT("the masks that are baked are not reported as lost"), HasNote(Read.Approximations, TEXT("surface normal")) || HasNote(Read.Approximations, TEXT("by height")));
	TestFalse(TEXT("it is not a slope overlay"), Read.SlopeOverlay.bFound);

	// The masks: the steepness of the normal Z 0.5 is 0.5; the height Z 500 cm at 0.001 a cm is 0.5.
	double Masks[2] = {};
	Read.AutoChain.Program->Evaluate(MakeSample(0.0, 0.0, FVector(0.0, 0.0, 500.0)), Masks);
	TestNearlyEqual(TEXT("flat ground is not steep"), Masks[0], 0.0, 1.0e-6);
	TestNearlyEqual(TEXT("500 cm is half way up"), Masks[1], 0.5, 1.0e-5);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainAutoEvaluatesSlopeMaskTest,
	"Mantle.Editor.Terrain.AutoEvaluatesSlopeMask",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainAutoEvaluatesSlopeMaskTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainAutoTestPrivate;

	UMaterialFunction* SlopeMask = LoadObject<UMaterialFunction>(nullptr, TEXT("/Engine/Functions/Engine_MaterialFunctions01/Texturing/SlopeMask.SlopeMask"));
	if (!TestNotNull(TEXT("the engine's SlopeMask function"), SlopeMask))
	{
		return false;
	}

	// The two calls of a landscape slope mask: one fed the pixel normal as its tangent-space normal with a falloff of 2 and a contrast of
	// 1, one with every input left at the function's own default (the normal (0, 0, 1), a falloff of 0.5, a contrast of 0.7).
	UMaterial* Material = NewObject<UMaterial>(GetTransientPackage());
	UMaterialExpressionConstant* Falloff = AddNode<UMaterialExpressionConstant>(Material);
	Falloff->R = 2.0f;
	UMaterialExpressionConstant* Contrast = AddNode<UMaterialExpressionConstant>(Material);
	Contrast->R = 1.0f;
	UMaterialExpressionMaterialFunctionCall* Pixel = AddCall(Material, SlopeMask,
		{ { TEXT("TangentNormal"), AddNode<UMaterialExpressionPixelNormalWS>(Material) }, { TEXT("FalloffPower"), Falloff }, { TEXT("CheapContrast"), Contrast } });
	UMaterialExpressionMaterialFunctionCall* Plain = AddCall(Material, SlopeMask, {});
	UMaterialExpressionAdd* Sum = AddNode<UMaterialExpressionAdd>(Material);
	Sum->A.Connect(0, Pixel);
	Sum->B.Connect(0, Plain);
	UMaterialExpressionLinearInterpolate* Blend = AddBlend(Material, AddColour(Material, TEXT("A"), FLinearColor::Red), AddColour(Material, TEXT("B"), FLinearColor::Blue), Sum);
	Material->GetEditorOnlyData()->BaseColor.Connect(0, Blend);

	FMantleLandscapeMaterialRead Read;
	FString Problem;
	if (!TestTrue(TEXT("the material is read"), FMantleLandscapeMaterialReader::Read(Material, 100.0, Read, Problem)))
	{
		AddError(Problem);
		return false;
	}
	if (!TestTrue(TEXT("its chain is found"), Read.AutoChain.bFound) || !TestEqual(TEXT("with one mask"), Read.AutoChain.Program->NumOutputs(), 1))
	{
		return false;
	}

	const FVector Up(0.0, 0.0, 1.0);
	const TArray<TPair<double, double>> Slopes = { { 0.0, 0.0 }, { 0.3, -0.2 }, { 0.6, 0.0 }, { -0.1, 0.9 }, { 2.0, 1.5 } };
	for (const TPair<double, double>& Slope : Slopes)
	{
		const FMantleSurfaceSample Sample = MakeSample(Slope.Key, Slope.Value);
		double Value = 0.0;
		Read.AutoChain.Program->Evaluate(Sample, &Value);

		// The pixel call turns the normal itself from tangent space to world; the default one turns (0, 0, 1), which is the normal.
		const double Expected = SlopeMaskOf(Sample, Sample.Normal, 2.0, 1.0) + SlopeMaskOf(Sample, Up, 0.5, 0.7);
		TestNearlyEqual(*FString::Printf(TEXT("the slope mask at a slope of (%g, %g)"), Slope.Key, Slope.Value), Value, Expected, 1.0e-4);

		// The default call is the closed form the engine function reduces to: saturate(pow(saturate(Nz), P) * (1 + 2C) - C).
		const double Closed = FMath::Clamp(FMath::Pow(FMath::Clamp(Sample.Normal.Z, 0.0, 1.0), 0.5) * 2.4 - 0.7, 0.0, 1.0);
		TestNearlyEqual(TEXT("the default call is the closed form"), SlopeMaskOf(Sample, Up, 0.5, 0.7), Closed, 1.0e-9);
	}

	// Flat ground is all of both: 1 + 1.
	double Flat = 0.0;
	Read.AutoChain.Program->Evaluate(MakeSample(0.0, 0.0), &Flat);
	TestNearlyEqual(TEXT("flat ground gives both calls' full mask"), Flat, 2.0, 1.0e-4);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainAutoBakesWeightsTest,
	"Mantle.Editor.Terrain.AutoBakesWeights",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainAutoBakesWeightsTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainAutoTestPrivate;

	// Ground, then rock over it by steepness (4 times 1 - the normal's Z), then snow over both by height ((Z - 1000 cm) at 0.001 a cm).
	UMaterial* Material = NewObject<UMaterial>(GetTransientPackage());
	UMaterialExpressionLinearInterpolate* Rocky = AddBlend(Material, AddColour(Material, TEXT("GroundColor"), FLinearColor(0.2f, 0.3f, 0.1f)),
		AddColour(Material, TEXT("RockColor"), FLinearColor(0.5f, 0.5f, 0.5f)), AddSteepness(Material, 4.0f));
	UMaterialExpressionSubtract* Above = AddNode<UMaterialExpressionSubtract>(Material);
	Above->A.Connect(2, AddNode<UMaterialExpressionWorldPosition>(Material));
	Above->ConstB = 1000.0f;
	UMaterialExpressionMultiply* Snowy = AddNode<UMaterialExpressionMultiply>(Material);
	Snowy->A.Connect(0, Above);
	Snowy->ConstB = 0.001f;
	UMaterialExpressionLinearInterpolate* Top = AddBlend(Material, Rocky, AddColour(Material, TEXT("SnowColor"), FLinearColor(0.9f, 0.9f, 0.95f)), AddSaturate(Material, Snowy));
	Material->GetEditorOnlyData()->BaseColor.Connect(0, Top);

	FMantleLandscapeMaterialRead Read;
	FString Problem;
	if (!TestTrue(TEXT("the material is read"), FMantleLandscapeMaterialReader::Read(Material, 100.0, Read, Problem)))
	{
		AddError(Problem);
		return false;
	}
	if (!TestEqual(TEXT("three layers"), Read.AutoChain.LayerNames.Num(), 3))
	{
		return false;
	}

	// 9 x 3 samples, 100 cm apart, 1 cm a height value, the same along every row: flat low ground, a rise, a steep face up to a high plateau.
	const uint16 Profile[9] = { 0, 0, 0, 50, 100, 1400, 3000, 3000, 3000 };
	FMantleTerrainSource Source;
	Source.Width = 9;
	Source.Height = 3;
	Source.SpacingX = 100.0;
	Source.SpacingY = 100.0;
	Source.HeightScale = 1.0;
	Source.HeightOffset = 0.0;
	for (int32 Y = 0; Y < Source.Height; ++Y)
	{
		for (int32 X = 0; X < Source.Width; ++X)
		{
			Source.Heights.Add(Profile[X]);
		}
	}
	Source.MaterialRead = Read;
	Source.bMaterialRead = true;

	// {ground, rock, snow} at columns 1, 3, 4, 5, 6 and 8, worked out by hand:
	//   1: flat, 0 cm                   -> all ground
	//   3: slope 0.5 (normal Z 0.894), 50 cm   -> rock 4 * (1 - 0.894) = 0.4223: 108, ground 147
	//   4: slope 6.75, 100 cm           -> all rock
	//   5: slope 14.5, 1400 cm          -> snow 0.4 (102), rock the rest (153)
	//   6: slope 8, 3000 cm             -> all snow
	//   8: flat, 3000 cm                -> all snow
	struct FExpected
	{
		int32 Column;
		int32 Weights[3];
	};
	const FExpected Expected[] = { { 1, { 255, 0, 0 } }, { 3, { 147, 108, 0 } }, { 4, { 0, 255, 0 } }, { 5, { 0, 153, 102 } }, { 6, { 0, 0, 255 } }, { 8, { 0, 0, 255 } } };
	const TArray<TArray<uint8>> Weights = FMantleTerrainExport::BakeAutoWeights(Source);
	if (!TestEqual(TEXT("a weight array for each layer"), Weights.Num(), 3))
	{
		return false;
	}
	for (int32 Index = 0; Index < Source.Width * Source.Height; ++Index)
	{
		TestEqual(*FString::Printf(TEXT("texel %d sums to 255"), Index), static_cast<int32>(Weights[0][Index]) + Weights[1][Index] + Weights[2][Index], 255);
	}
	for (const FExpected& Want : Expected)
	{
		for (int32 RowIndex = 0; RowIndex < Source.Height; ++RowIndex)
		{
			for (int32 Layer = 0; Layer < 3; ++Layer)
			{
				TestEqual(*FString::Printf(TEXT("column %d row %d layer %d"), Want.Column, RowIndex, Layer), static_cast<int32>(Weights[Layer][RowIndex * Source.Width + Want.Column]), Want.Weights[Layer]);
			}
		}
	}

	// The landscape's transform puts it 400 cm higher: column 5 is at 1800 cm, which is 0.8 snow (204) over 0.2 rock (51).
	Source.Transform = FTransform(FQuat::Identity, FVector(0.0, 0.0, 400.0));
	const TArray<TArray<uint8>> Raised = FMantleTerrainExport::BakeAutoWeights(Source);
	if (Raised.Num() == 3)
	{
		TestEqual(TEXT("raised: column 5 is mostly snow"), static_cast<int32>(Raised[2][5]), 204);
		TestEqual(TEXT("raised: column 5's rock"), static_cast<int32>(Raised[1][5]), 51);
		TestEqual(TEXT("raised: column 4 is still all rock"), static_cast<int32>(Raised[1][4]), 255);
	}
	Source.Transform = FTransform::Identity;

	// Baked into the source, the layers go where painted layers go: three layers, one weightmap, texels summing to 255.
	TestTrue(TEXT("the chain's layers are added"), FMantleTerrainExport::BakeAutoChain(Source));
	TestEqual(TEXT("as three layers"), Source.LayerNames.Num(), 3);
	TestEqual(TEXT("with a weight array each"), Source.LayerWeights.Num(), 3);
	TestTrue(TEXT("named as the chain names them"), Source.LayerNames == Read.AutoChain.LayerNames);
	TArray<FMantleTerrainLayerChoice> Layers;
	for (int32 Index = 0; Index < Source.LayerNames.Num(); ++Index)
	{
		FMantleTerrainLayerChoice& Choice = Layers.AddDefaulted_GetRef();
		Choice.Layer = Source.LayerNames[Index];
		Choice.SourceIndex = Index;
	}
	TArray<FColor> First;
	TArray<FColor> Second;
	FMantleTerrainExport::MakeWeightmaps(Source, Layers, FIntPoint(9, 3), First, Second);
	TestEqual(TEXT("one weightmap"), First.Num(), 27);
	TestEqual(TEXT("and no second"), Second.Num(), 0);
	for (const FColor& Texel : First)
	{
		TestEqual(TEXT("each texel of the weightmap sums to 255"), static_cast<int32>(Texel.R) + Texel.G + Texel.B + Texel.A, 255);
	}

	// A layer that weighs nothing anywhere is left out, as an unpainted layer is: flat ground at 0 cm is all the base.
	FMantleTerrainSource Flat = Source;
	Flat.Heights.Init(0, Flat.Width * Flat.Height);
	Flat.LayerNames.Reset();
	Flat.LayerWeights.Reset();
	TestTrue(TEXT("flat low ground bakes"), FMantleTerrainExport::BakeAutoChain(Flat));
	TestEqual(TEXT("as the base alone"), Flat.LayerNames.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainAutoLeavesPaintedMaterialsAloneTest,
	"Mantle.Editor.Terrain.AutoLeavesPaintedMaterialsAlone",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainAutoLeavesPaintedMaterialsAloneTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainAutoTestPrivate;

	// A Landscape Layer Blend of Grass and Dirt: the paint layers it names are its layers, and nothing is baked.
	UMaterial* Painted = NewObject<UMaterial>(GetTransientPackage());
	UMaterialExpressionLandscapeLayerBlend* Blend = AddNode<UMaterialExpressionLandscapeLayerBlend>(Painted);
	FLayerBlendInput& Grass = Blend->Layers.AddDefaulted_GetRef();
	Grass.LayerName = TEXT("Grass");
	Grass.LayerInput.Connect(0, AddColour(Painted, TEXT("GrassColor"), FLinearColor(0.1f, 0.5f, 0.1f)));
	FLayerBlendInput& Dirt = Blend->Layers.AddDefaulted_GetRef();
	Dirt.LayerName = TEXT("Dirt");
	Dirt.LayerInput.Connect(0, AddColour(Painted, TEXT("DirtColor"), FLinearColor(0.4f, 0.3f, 0.2f)));
	Painted->GetEditorOnlyData()->BaseColor.Connect(0, Blend);

	FMantleLandscapeMaterialRead Read;
	FString Problem;
	if (!TestTrue(TEXT("a painted material is read"), FMantleLandscapeMaterialReader::Read(Painted, 100.0, Read, Problem)))
	{
		AddError(Problem);
		return false;
	}
	TestFalse(TEXT("it has no auto chain"), Read.AutoChain.bFound);
	TestFalse(TEXT("no program"), Read.AutoChain.Program.IsValid());
	TestEqual(TEXT("its layers are the painted ones"), Read.Layers.Num(), 2);
	TestNotNull(TEXT("Grass"), Read.Find(TEXT("Grass")));
	TestNotNull(TEXT("Dirt"), Read.Find(TEXT("Dirt")));

	// The same with a rock overlay by steepness over the painted layers: the overlay is the old path's, not a chain.
	UMaterial* Overlaid = NewObject<UMaterial>(GetTransientPackage());
	UMaterialExpressionLandscapeLayerBlend* OverlaidBlend = AddNode<UMaterialExpressionLandscapeLayerBlend>(Overlaid);
	FLayerBlendInput& OverlaidGrass = OverlaidBlend->Layers.AddDefaulted_GetRef();
	OverlaidGrass.LayerName = TEXT("Grass");
	OverlaidGrass.LayerInput.Connect(0, AddColour(Overlaid, TEXT("GrassColor"), FLinearColor(0.1f, 0.5f, 0.1f)));
	FLayerBlendInput& OverlaidDirt = OverlaidBlend->Layers.AddDefaulted_GetRef();
	OverlaidDirt.LayerName = TEXT("Dirt");
	OverlaidDirt.LayerInput.Connect(0, AddColour(Overlaid, TEXT("DirtColor"), FLinearColor(0.4f, 0.3f, 0.2f)));
	UMaterialExpressionLinearInterpolate* Overlay = AddBlend(Overlaid, OverlaidBlend, AddColour(Overlaid, TEXT("RockColor"), FLinearColor(0.5f, 0.5f, 0.5f)), AddSteepness(Overlaid, 4.0f));
	Overlaid->GetEditorOnlyData()->BaseColor.Connect(0, Overlay);

	FMantleLandscapeMaterialRead OverlaidRead;
	if (!TestTrue(TEXT("a painted material with a slope overlay is read"), FMantleLandscapeMaterialReader::Read(Overlaid, 100.0, OverlaidRead, Problem)))
	{
		AddError(Problem);
		return false;
	}
	TestFalse(TEXT("it has no auto chain"), OverlaidRead.AutoChain.bFound);
	TestTrue(TEXT("it has its slope overlay"), OverlaidRead.SlopeOverlay.bFound);
	TestEqual(TEXT("the painted layers and the overlay"), OverlaidRead.Layers.Num(), 3);

	// A material that blends by something this cannot evaluate (a texture coordinate in the mask) is not a chain, and says why.
	UMaterial* Unreadable = NewObject<UMaterial>(GetTransientPackage());
	UMaterialExpressionMultiply* Mask = AddNode<UMaterialExpressionMultiply>(Unreadable);
	Mask->A.Connect(0, AddSteepness(Unreadable, 4.0f));
	Mask->B.Connect(0, AddNode<UMaterialExpressionTextureCoordinate>(Unreadable));
	Unreadable->GetEditorOnlyData()->BaseColor.Connect(0,
		AddBlend(Unreadable, AddColour(Unreadable, TEXT("GroundColor"), FLinearColor::Green), AddColour(Unreadable, TEXT("RockColor"), FLinearColor::Gray), AddSaturate(Unreadable, Mask)));
	FMantleLandscapeMaterialRead UnreadableRead;
	TestFalse(TEXT("a blend by a mask it cannot evaluate is not read"), FMantleLandscapeMaterialReader::Read(Unreadable, 100.0, UnreadableRead, Problem));
	TestTrue(TEXT("and says what it could not evaluate"), Problem.Contains(TEXT("TextureCoordinate")));
	TestFalse(TEXT("with no chain"), UnreadableRead.AutoChain.bFound);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainAutoReadsLayerNormalsTest,
	"Mantle.Editor.Terrain.AutoReadsLayerNormals",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainAutoReadsLayerNormalsTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainAutoTestPrivate;

	// Ground, rock over it by steepness, snow over both by height: in the Base Color and, as a parallel chain of normal maps, in the Normal.
	// The normal chain shares the steepness node with the colour chain, so rock and the ground under it match; its snow blend is by a mask of
	// its own that merely computes the same thing, so the snow has no normal.
	UMaterial* Material = NewObject<UMaterial>(GetTransientPackage());
	UTexture2D* GroundColour = MakeTexture(false);
	UTexture2D* RockColour = MakeTexture(false);
	UTexture2D* SnowColour = MakeTexture(false);
	UTexture2D* GroundNormal = MakeTexture(true);
	UTexture2D* RockNormal = MakeTexture(true);
	UTexture2D* SnowNormal = MakeTexture(true);

	UMaterialExpression* Steep = AddSteepness(Material, 4.0f);
	UMaterialExpressionLinearInterpolate* Rocky = AddBlend(Material, AddSample(Material, GroundColour), AddSample(Material, RockColour), Steep);
	Material->GetEditorOnlyData()->BaseColor.Connect(0, AddBlend(Material, Rocky, AddSample(Material, SnowColour), AddHeightMask(Material, 1000.0f)));
	UMaterialExpressionLinearInterpolate* RockyNormal = AddBlend(Material, AddSample(Material, GroundNormal), AddSample(Material, RockNormal), Steep);
	Material->GetEditorOnlyData()->Normal.Connect(0, AddBlend(Material, RockyNormal, AddSample(Material, SnowNormal), AddHeightMask(Material, 1000.0f)));

	FMantleLandscapeMaterialRead Read;
	FString Problem;
	if (!TestTrue(TEXT("the material is read"), FMantleLandscapeMaterialReader::Read(Material, 100.0, Read, Problem)))
	{
		AddError(Problem);
		return false;
	}
	if (!TestEqual(TEXT("three layers"), Read.Layers.Num(), 3))
	{
		return false;
	}
	TestTrue(TEXT("the ground's colour"), Read.Layers[0].BaseColor == GroundColour);
	TestTrue(TEXT("the rock's colour"), Read.Layers[1].BaseColor == RockColour);
	TestTrue(TEXT("the snow's colour"), Read.Layers[2].BaseColor == SnowColour);
	TestTrue(TEXT("the base takes the normal under the first blend"), Read.Layers[0].Normal == GroundNormal);
	TestTrue(TEXT("the layer over it takes the normal that shares its mask"), Read.Layers[1].Normal == RockNormal);
	TestNull(TEXT("a layer whose mask no normal blend shares keeps a flat normal"), Read.Layers[2].Normal);
	TestTrue(TEXT("a normal's tiling is read as a colour's is"), Read.Layers[1].NormalTilingCm > 0.0);

	// A material with no Normal chain gives no normals.
	UMaterial* ColourOnly = NewObject<UMaterial>(GetTransientPackage());
	ColourOnly->GetEditorOnlyData()->BaseColor.Connect(0, AddBlend(ColourOnly, AddSample(ColourOnly, GroundColour), AddSample(ColourOnly, RockColour), AddSteepness(ColourOnly, 4.0f)));
	FMantleLandscapeMaterialRead ColourOnlyRead;
	if (TestTrue(TEXT("a colour chain alone is read"), FMantleLandscapeMaterialReader::Read(ColourOnly, 100.0, ColourOnlyRead, Problem)) && TestEqual(TEXT("two layers"), ColourOnlyRead.Layers.Num(), 2))
	{
		TestNull(TEXT("the first has no normal"), ColourOnlyRead.Layers[0].Normal);
		TestNull(TEXT("nor the second"), ColourOnlyRead.Layers[1].Normal);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainAutoCoverageTest,
	"Mantle.Editor.Terrain.AutoCoverage",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainAutoCoverageTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainAutoTestPrivate;

	// A chain of textured layers under a tint texture, with the normal chain over the same masks: what the chain bakes (the slope, the height, the
	// layers' textures) is not reported as lost, and the tint is.
	UMaterial* Material = NewObject<UMaterial>(GetTransientPackage());
	UMaterialExpression* Steep = AddSteepness(Material, 4.0f);
	UMaterialExpression* Snowy = AddHeightMask(Material, 1000.0f);
	UMaterialExpressionLinearInterpolate* Rocky = AddBlend(Material, AddSample(Material, MakeTexture(false)), AddSample(Material, MakeTexture(false)), Steep);
	UMaterialExpressionMultiply* Tinted = AddNode<UMaterialExpressionMultiply>(Material);
	Tinted->A.Connect(0, Rocky);
	Tinted->B.Connect(0, AddSample(Material, MakeTexture(false)));
	Material->GetEditorOnlyData()->BaseColor.Connect(0, AddBlend(Material, Tinted, AddSample(Material, MakeTexture(false)), Snowy));
	UMaterialExpressionLinearInterpolate* RockyNormal = AddBlend(Material, AddSample(Material, MakeTexture(true)), AddSample(Material, MakeTexture(true)), Steep);
	Material->GetEditorOnlyData()->Normal.Connect(0, AddBlend(Material, RockyNormal, AddSample(Material, MakeTexture(true)), Snowy));

	FMantleLandscapeMaterialRead Read;
	FString Problem;
	if (!TestTrue(TEXT("the material is read"), FMantleLandscapeMaterialReader::Read(Material, 100.0, Read, Problem)))
	{
		AddError(Problem);
		return false;
	}
	TestEqual(TEXT("three layers"), Read.Layers.Num(), 3);
	TestFalse(TEXT("the slope is baked, not lost"), HasNote(Read.Approximations, TEXT("reads the surface normal")));
	TestFalse(TEXT("so is the height"), HasNote(Read.Approximations, TEXT("blends the ground by height")));
	TestFalse(TEXT("the layers' textures are kept"), HasNote(Read.Approximations, TEXT("outside the paint layers")));
	TestTrue(TEXT("the tint is still said to be lost"), HasNote(Read.Approximations, TEXT("after the layers")));

	// Something outside the chain that reads the surface normal is still reported: the Normal output multiplied by the normal's Z.
	UMaterial* Other = NewObject<UMaterial>(GetTransientPackage());
	UMaterialExpression* OtherSteep = AddSteepness(Other, 4.0f);
	Other->GetEditorOnlyData()->BaseColor.Connect(0, AddBlend(Other, AddSample(Other, MakeTexture(false)), AddSample(Other, MakeTexture(false)), OtherSteep));
	UMaterialExpressionMultiply* Faded = AddNode<UMaterialExpressionMultiply>(Other);
	Faded->A.Connect(0, AddBlend(Other, AddSample(Other, MakeTexture(true)), AddSample(Other, MakeTexture(true)), OtherSteep));
	Faded->B.Connect(0, AddMaskB(Other, AddNode<UMaterialExpressionPixelNormalWS>(Other)));
	Other->GetEditorOnlyData()->Normal.Connect(0, Faded);
	FMantleLandscapeMaterialRead OtherRead;
	if (TestTrue(TEXT("a material with a normal fade is read"), FMantleLandscapeMaterialReader::Read(Other, 100.0, OtherRead, Problem)))
	{
		TestTrue(TEXT("what the chain does not cover is still reported"), HasNote(OtherRead.Approximations, TEXT("reads the surface normal")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainAutoSamplesMaskTexturesTest,
	"Mantle.Editor.Terrain.AutoSamplesMaskTextures",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainAutoSamplesMaskTexturesTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainAutoTestPrivate;

	auto Evaluate = [this](UMaterial* Material, const FMantleSurfaceSample& Sample, double& OutMask) -> bool
	{
		FMantleLandscapeMaterialRead Read;
		FString Problem;
		if (!FMantleLandscapeMaterialReader::Read(Material, 100.0, Read, Problem))
		{
			AddError(Problem);
			return false;
		}
		if (!Read.AutoChain.bFound || Read.AutoChain.Program->NumOutputs() != 1)
		{
			AddError(TEXT("the material's chain was not found"));
			return false;
		}
		TestFalse(TEXT("a sampled texture is not said to be taken as its average"), HasNote(Read.Approximations, TEXT("edge noise")));
		Read.AutoChain.Program->Evaluate(Sample, &OutMask);
		return true;
	};

	// UVs from the world position: 0.005 a cm, so the 2 x 2 texture repeats every 200 cm and a texel is 100 cm.
	UMaterial* World = MakeCheckerMaterial([](UMaterial* Material) -> UMaterialExpression*
	{
		UMaterialExpressionMultiply* Scaled = AddNode<UMaterialExpressionMultiply>(Material);
		Scaled->A.Connect(0, AddNode<UMaterialExpressionWorldPosition>(Material));
		Scaled->ConstB = 0.005f;
		UMaterialExpressionComponentMask* Xy = AddNode<UMaterialExpressionComponentMask>(Material);
		Xy->Input.Connect(0, Scaled);
		Xy->R = true;
		Xy->G = true;
		Xy->B = false;
		Xy->A = false;
		return Xy;
	}, true);
	// Texel centres are at 50 and 150 cm: the top row is 0 then 1, the bottom 1 then 0; the texture wraps; between centres it is bilinear.
	const struct { double X; double Y; double Expected; const TCHAR* What; } Cases[] = {
		{ 50.0, 50.0, 0.0, TEXT("texel (0, 0)") },
		{ 150.0, 50.0, 1.0, TEXT("texel (1, 0)") },
		{ 50.0, 150.0, 1.0, TEXT("texel (0, 1)") },
		{ 150.0, 150.0, 0.0, TEXT("texel (1, 1)") },
		{ 100.0, 50.0, 0.5, TEXT("half way between two texels") },
		{ 100.0, 100.0, 0.5, TEXT("the middle of four") },
		{ 250.0, 50.0, 0.0, TEXT("wrapped past the right edge") },
		{ -50.0, 50.0, 1.0, TEXT("wrapped past the left edge") },
	};
	for (const auto& Case : Cases)
	{
		double Mask = -1.0;
		if (Evaluate(World, MakeSample(0.0, 0.0, FVector(Case.X, Case.Y, 0.0)), Mask))
		{
			TestNearlyEqual(Case.What, Mask, Case.Expected, 0.01);
		}
	}

	// UVs from the landscape's own: one unit a sample, a mapping scale of 2 makes the texture repeat every 2 samples (a texel a sample).
	UMaterial* Landscape = MakeCheckerMaterial([](UMaterial* Material) -> UMaterialExpression*
	{
		UMaterialExpressionLandscapeLayerCoords* Coords = AddNode<UMaterialExpressionLandscapeLayerCoords>(Material);
		Coords->MappingScale = 2.0f;
		return Coords;
	}, true);
	for (const auto& Case : { TPair<double, double>(0.5, 0.5), TPair<double, double>(1.5, 0.5), TPair<double, double>(0.5, 1.5), TPair<double, double>(1.5, 1.5) })
	{
		FMantleSurfaceSample Sample = MakeSample(0.0, 0.0);
		Sample.LandscapeSample = FVector2D(Case.Key, Case.Value);
		double Mask = -1.0;
		if (Evaluate(Landscape, Sample, Mask))
		{
			TestNearlyEqual(*FString::Printf(TEXT("landscape sample (%g, %g)"), Case.Key, Case.Value), Mask, (Case.Key < 1.0) == (Case.Value < 1.0) ? 0.0 : 1.0, 0.01);
		}
	}

	// A sample with no coordinates connected reads the landscape's first UV set the same way: one repeat a sample.
	UMaterial* Plain = MakeCheckerMaterial([](UMaterial*) -> UMaterialExpression* { return nullptr; }, false);
	FMantleSurfaceSample Quarter = MakeSample(0.0, 0.0);
	Quarter.LandscapeSample = FVector2D(7.75, 3.25);
	double Mask = -1.0;
	if (Evaluate(Plain, Quarter, Mask))
	{
		TestNearlyEqual(TEXT("the first texture coordinate set is the landscape's: (0.75, 0.25) is texel (1, 0)"), Mask, 1.0, 0.01);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainAutoReadsLayerRoughnessTest,
	"Mantle.Editor.Terrain.AutoReadsLayerRoughness",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainAutoReadsLayerRoughnessTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainAutoTestPrivate;

	auto Scalar = [](UMaterial* Material, const float Value)
	{
		UMaterialExpressionConstant* Constant = AddNode<UMaterialExpressionConstant>(Material);
		Constant->R = Value;
		return Constant;
	};

	// Through a chain: ground, rock over it by steepness, snow over both by height, in the colour and, by the same two mask nodes, in the roughness.
	UMaterial* Chained = NewObject<UMaterial>(GetTransientPackage());
	UMaterialExpression* Steep = AddSteepness(Chained, 4.0f);
	UMaterialExpression* Snowy = AddHeightMask(Chained, 1000.0f);
	Chained->GetEditorOnlyData()->BaseColor.Connect(0, AddBlend(Chained,
		AddBlend(Chained, AddColour(Chained, TEXT("GroundColor"), FLinearColor(0.2f, 0.3f, 0.1f)), AddColour(Chained, TEXT("RockColor"), FLinearColor(0.5f, 0.5f, 0.5f)), Steep),
		AddColour(Chained, TEXT("SnowColor"), FLinearColor(0.9f, 0.9f, 0.95f)), Snowy));
	Chained->GetEditorOnlyData()->Roughness.Connect(0, AddBlend(Chained, AddBlend(Chained, Scalar(Chained, 0.9f), Scalar(Chained, 0.6f), Steep), Scalar(Chained, 0.3f), Snowy));

	FMantleLandscapeMaterialRead Read;
	FString Problem;
	if (!TestTrue(TEXT("the chained material is read"), FMantleLandscapeMaterialReader::Read(Chained, 100.0, Read, Problem)) || !TestEqual(TEXT("three layers"), Read.Layers.Num(), 3))
	{
		AddError(Problem);
		return false;
	}
	const float Expected[3] = { 0.9f, 0.6f, 0.3f };
	for (int32 Index = 0; Index < 3; ++Index)
	{
		TestTrue(TEXT("each layer has a roughness"), Read.Layers[Index].bHasRoughness);
		TestNearlyEqual(TEXT("and it is the one the roughness blend gives that layer"), Read.Layers[Index].Roughness, Expected[Index], 1.0e-4f);
	}
	// The layer's material carries it as its roughness (what the converter writes as roughnessFactor and the client reads).
	if (UMaterial* Rock = Cast<UMaterial>(FMantleLandscapeMaterialReader::MakeLayerMaterial(Read.Layers[1])))
	{
		TestNearlyEqual(TEXT("the layer material's roughness"), Rock->GetEditorOnlyData()->Roughness.Constant, 0.6f, 1.0e-4f);
	}
	else
	{
		AddError(TEXT("the rock layer has no material"));
	}
	FMantleLandscapeLayerLook Plain = Read.Layers[1];
	Plain.bHasRoughness = false;
	TestNotEqual(TEXT("a stated roughness is a material of its own"), FMantleLandscapeMaterialReader::LayerMaterialName(Plain), FMantleLandscapeMaterialReader::LayerMaterialName(Read.Layers[1]));
	if (UMaterial* Default = Cast<UMaterial>(FMantleLandscapeMaterialReader::MakeLayerMaterial(Plain)))
	{
		TestNearlyEqual(TEXT("with none stated the default stands"), Default->GetEditorOnlyData()->Roughness.Constant, 0.85f, 1.0e-4f);
	}

	// By multiplying: the ground's roughness times (1 - wetness)^4, as the water of a landscape pack does. Where the water is full the roughness is 0,
	// which is as glossy as the client draws (0.01); the ground is 0.8.
	UMaterial* Wet = NewObject<UMaterial>(GetTransientPackage());
	UMaterialExpression* Water = AddHeightMask(Wet, 1000.0f);
	Wet->GetEditorOnlyData()->BaseColor.Connect(0, AddBlend(Wet, AddColour(Wet, TEXT("GroundColor"), FLinearColor(0.2f, 0.3f, 0.1f)), AddColour(Wet, TEXT("WaterColor"), FLinearColor(0.0f, 0.1f, 0.2f)), Water));
	UMaterialExpressionOneMinus* Dry = AddNode<UMaterialExpressionOneMinus>(Wet);
	Dry->Input.Connect(0, Water);
	UMaterialExpressionPower* Fourth = AddNode<UMaterialExpressionPower>(Wet);
	Fourth->Base.Connect(0, Dry);
	Fourth->ConstExponent = 4.0f;
	UMaterialExpressionMultiply* Glossy = AddNode<UMaterialExpressionMultiply>(Wet);
	Glossy->A.Connect(0, Scalar(Wet, 0.8f));
	Glossy->B.Connect(0, Fourth);
	Wet->GetEditorOnlyData()->Roughness.Connect(0, Glossy);

	FMantleLandscapeMaterialRead WetRead;
	if (!TestTrue(TEXT("the wet material is read"), FMantleLandscapeMaterialReader::Read(Wet, 100.0, WetRead, Problem)) || !TestEqual(TEXT("two layers"), WetRead.Layers.Num(), 2))
	{
		AddError(Problem);
		return false;
	}
	TestNearlyEqual(TEXT("the ground keeps its roughness"), WetRead.Layers[0].Roughness, 0.8f, 1.0e-4f);
	TestTrue(TEXT("the water has one"), WetRead.Layers[1].bHasRoughness);
	TestTrue(TEXT("at or below 0.1, so it reads as water"), WetRead.Layers[1].Roughness <= 0.1f);

	// A roughness that reads the world position outside any mask is not a number per layer: the default stands, and the layer says so.
	UMaterial* Unreadable = NewObject<UMaterial>(GetTransientPackage());
	UMaterialExpression* UnreadableMask = AddHeightMask(Unreadable, 1000.0f);
	Unreadable->GetEditorOnlyData()->BaseColor.Connect(0, AddBlend(Unreadable, AddColour(Unreadable, TEXT("GroundColor"), FLinearColor::Green), AddColour(Unreadable, TEXT("RockColor"), FLinearColor::Gray), UnreadableMask));
	Unreadable->GetEditorOnlyData()->Roughness.Connect(2, AddNode<UMaterialExpressionWorldPosition>(Unreadable));
	FMantleLandscapeMaterialRead UnreadableRead;
	if (TestTrue(TEXT("the material is read"), FMantleLandscapeMaterialReader::Read(Unreadable, 100.0, UnreadableRead, Problem)) && TestEqual(TEXT("two layers"), UnreadableRead.Layers.Num(), 2))
	{
		TestFalse(TEXT("no roughness is stated"), UnreadableRead.Layers[0].bHasRoughness);
		TestTrue(TEXT("and the layer says the default stands"), HasNote(UnreadableRead.Layers[0].Approximations, TEXT("default roughness")));
	}

	// No Roughness output: no roughness, and no note.
	UMaterial* Bare = NewObject<UMaterial>(GetTransientPackage());
	Bare->GetEditorOnlyData()->BaseColor.Connect(0, AddBlend(Bare, AddColour(Bare, TEXT("GroundColor"), FLinearColor::Green), AddColour(Bare, TEXT("RockColor"), FLinearColor::Gray), AddSteepness(Bare, 4.0f)));
	FMantleLandscapeMaterialRead BareRead;
	if (TestTrue(TEXT("a material without roughness is read"), FMantleLandscapeMaterialReader::Read(Bare, 100.0, BareRead, Problem)) && TestEqual(TEXT("two layers"), BareRead.Layers.Num(), 2))
	{
		TestFalse(TEXT("none is stated"), BareRead.Layers[1].bHasRoughness);
		TestFalse(TEXT("and none is said to be lost"), HasNote(BareRead.Layers[1].Approximations, TEXT("roughness")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainAutoLayerNamesTest,
	"Mantle.Editor.Terrain.AutoLayerNames",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainAutoLayerNamesTest::RunTest(const FString& Parameters)
{
	// A short leading token and a trailing colour word come off a name taken from a parameter or texture.
	auto Name = [](const TCHAR* From) { return FMantleLandscapeMaterialReader::LayerNameFromParameter(From); };
	TestEqual(TEXT("MW_WaterColor"), Name(TEXT("MW_WaterColor")), FString(TEXT("Water")));
	TestEqual(TEXT("M_GrassBaseColor"), Name(TEXT("M_GrassBaseColor")), FString(TEXT("Grass")));
	TestEqual(TEXT("T_Rock"), Name(TEXT("T_Rock")), FString(TEXT("Rock")));
	TestEqual(TEXT("TEX_Sand_Color"), Name(TEXT("TEX_Sand_Color")), FString(TEXT("Sand")));
	TestEqual(TEXT("a name with no prefix"), Name(TEXT("SnowBaseColor")), FString(TEXT("Snow")));
	TestEqual(TEXT("a prefix too long to be one stays"), Name(TEXT("Grass_Moss")), FString(TEXT("Grass_Moss")));
	TestEqual(TEXT("a token with a digit stays"), Name(TEXT("T2_Rock")), FString(TEXT("T2_Rock")));
	TestEqual(TEXT("a trailing colour word alone is taken off"), Name(TEXT("Dirt_Color")), FString(TEXT("Dirt")));
	TestEqual(TEXT("a prefix is not taken off when nothing would be left"), Name(TEXT("MW_")), FString(TEXT("MW")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
