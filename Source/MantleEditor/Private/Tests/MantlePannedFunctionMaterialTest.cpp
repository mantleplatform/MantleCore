// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceAssets.h"

#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionAppendVector.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant2Vector.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionNormalize.h"
#include "Materials/MaterialExpressionPanner.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionStaticBool.h"
#include "Materials/MaterialExpressionStaticBoolParameter.h"
#include "Materials/MaterialExpressionStaticSwitch.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionTime.h"
#include "Materials/MaterialExpressionTransform.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// A moving texture read through the nodes a creator's graph puts between the Panner and the output: a Panner whose speed is a function
// input, a texture a function input gives, a tiling that is scalar parameters times the UVs, Time times a speed, a Normalize and a
// Lerp against a flat normal. The Stylized Village waterfall's normal (MM_Waterfall over MF_RiverMotionNormal) is that shape, cut
// down: the graphs are built in code, so no asset is needed.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantlePannedFunctionTestPrivate
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

	template <typename TOwner>
	UMaterialExpressionConstant2Vector* AddPair(TOwner* Owner, const float X, const float Y)
	{
		UMaterialExpressionConstant2Vector* Constant = AddNode<UMaterialExpressionConstant2Vector>(Owner);
		Constant->R = X;
		Constant->G = Y;
		return Constant;
	}

	UMaterialExpressionScalarParameter* AddScalar(UMaterial* Material, const TCHAR* Name, const float Value)
	{
		UMaterialExpressionScalarParameter* Parameter = AddNode<UMaterialExpressionScalarParameter>(Material);
		Parameter->ParameterName = Name;
		Parameter->DefaultValue = Value;
		return Parameter;
	}

	UMaterialExpressionConstant3Vector* AddFlatNormal(UMaterial* Material)
	{
		UMaterialExpressionConstant3Vector* Flat = AddNode<UMaterialExpressionConstant3Vector>(Material);
		Flat->Constant = FLinearColor(0.0f, 0.0f, 1.0f);
		return Flat;
	}

	UMaterialExpressionFunctionInput* AddInput(UMaterialFunction* Function, const TCHAR* Name, const EFunctionInputType Type, const float Preview)
	{
		UMaterialExpressionFunctionInput* Input = AddNode<UMaterialExpressionFunctionInput>(Function);
		Input->InputName = Name;
		Input->InputType = Type;
		Input->PreviewValue = FVector4f(Preview, 0.0f, 0.0f, 0.0f);
		Input->bUsePreviewValueAsDefault = true;
		Input->Id = FGuid::NewGuid();
		return Input;
	}

	/**
	 * MF_RiverMotionNormal, cut down: the output is the texture the "Normal Map" input gives, sampled at the "UV" input panned along V
	 * by the "Speed" input times 0.25. "Speed" defaults to 0.4 when nothing is connected.
	 */
	UMaterialFunction* MakeFlowFunction()
	{
		UMaterialFunction* Function = NewObject<UMaterialFunction>(GetTransientPackage());
		UMaterialExpressionFunctionInput* Speed = AddInput(Function, TEXT("Speed"), FunctionInput_Scalar, 0.4f);
		UMaterialExpressionFunctionInput* Map = AddInput(Function, TEXT("Normal Map"), FunctionInput_Texture2D, 0.0f);
		UMaterialExpressionFunctionInput* Uv = AddInput(Function, TEXT("UV"), FunctionInput_Vector2, 0.0f);
		UMaterialExpressionMultiply* Velocity = AddNode<UMaterialExpressionMultiply>(Function);
		Velocity->A.Connect(0, AddPair(Function, 0.0f, 0.25f));
		Velocity->B.Connect(0, Speed);
		UMaterialExpressionPanner* Panner = AddNode<UMaterialExpressionPanner>(Function);
		Panner->Coordinate.Connect(0, Uv);
		Panner->Speed.Connect(0, Velocity);
		UMaterialExpressionTextureSample* Sample = AddNode<UMaterialExpressionTextureSample>(Function);
		Sample->TextureObject.Connect(0, Map);
		Sample->Coordinates.Connect(0, Panner);
		UMaterialExpressionFunctionOutput* Output = AddNode<UMaterialExpressionFunctionOutput>(Function);
		Output->OutputName = TEXT("Normal");
		Output->Id = FGuid::NewGuid();
		Output->A.Connect(0, Sample);
		return Function;
	}

	/** A function whose output is a call of Inner (the nesting a depth limit and a recursion check are for). */
	UMaterialFunction* MakeWrapper(UMaterialFunction* Inner)
	{
		UMaterialFunction* Function = NewObject<UMaterialFunction>(GetTransientPackage());
		UMaterialExpressionMaterialFunctionCall* Call = AddNode<UMaterialExpressionMaterialFunctionCall>(Function);
		Call->SetMaterialFunction(Inner);
		UMaterialExpressionFunctionOutput* Output = AddNode<UMaterialExpressionFunctionOutput>(Function);
		Output->OutputName = TEXT("Result");
		Output->Id = FGuid::NewGuid();
		Output->A.Connect(0, Call);
		return Function;
	}

	UMaterialExpressionMaterialFunctionCall* AddCall(UMaterial* Material, UMaterialFunction* Function)
	{
		UMaterialExpressionMaterialFunctionCall* Call = AddNode<UMaterialExpressionMaterialFunctionCall>(Material);
		Call->SetMaterialFunction(Function);
		return Call;
	}

	void ConnectCallInput(UMaterialExpressionMaterialFunctionCall* Call, const TCHAR* Name, UMaterialExpression* Source)
	{
		for (FFunctionExpressionInput& Input : Call->FunctionInputs)
		{
			if (Input.Input.InputName == FName(Name))
			{
				Input.Input.Connect(0, Source);
			}
		}
	}

	/** A call of the flow function as MM_Waterfall makes it: Speed = Flow Speed x -1, the Normal Map parameter, UV = (X Tile, Y Tile) x Texture Coordinate. */
	UMaterialExpressionMaterialFunctionCall* AddFlowCall(UMaterial* Material, UTexture* Texture, UMaterialFunction* Function)
	{
		UMaterialExpressionMaterialFunctionCall* Call = AddCall(Material, Function);
		UMaterialExpressionMultiply* Reversed = AddNode<UMaterialExpressionMultiply>(Material);
		Reversed->A.Connect(0, AddScalar(Material, TEXT("Flow Speed"), 1.0f));
		Reversed->B.Connect(0, AddNumber(Material, -1.0f));
		UMaterialExpressionTextureObjectParameter* Map = AddNode<UMaterialExpressionTextureObjectParameter>(Material);
		Map->ParameterName = TEXT("Normal Map");
		Map->Texture = Texture;
		UMaterialExpressionAppendVector* Tiles = AddNode<UMaterialExpressionAppendVector>(Material);
		Tiles->A.Connect(0, AddScalar(Material, TEXT("Normal X Tile"), 2.0f));
		Tiles->B.Connect(0, AddScalar(Material, TEXT("Normal Y Tile"), 3.0f));
		UMaterialExpressionMultiply* Scaled = AddNode<UMaterialExpressionMultiply>(Material);
		Scaled->A.Connect(0, Tiles);
		Scaled->B.Connect(0, AddNode<UMaterialExpressionTextureCoordinate>(Material));
		ConnectCallInput(Call, TEXT("Speed"), Reversed);
		ConnectCallInput(Call, TEXT("Normal Map"), Map);
		ConnectCallInput(Call, TEXT("UV"), Scaled);
		return Call;
	}

	/** A sample of Texture whose UVs are Coordinates (null: the first UV set as it is). */
	UMaterialExpressionTextureSample* AddSample(UMaterial* Material, UTexture* Texture, UMaterialExpression* Coordinates)
	{
		UMaterialExpressionTextureSample* Sample = AddNode<UMaterialExpressionTextureSample>(Material);
		Sample->Texture = Texture;
		if (Coordinates)
		{
			Sample->Coordinates.Connect(0, Coordinates);
		}
		return Sample;
	}

	UMaterialExpressionPanner* AddPanner(UMaterial* Material, const float SpeedX, const float SpeedY)
	{
		UMaterialExpressionPanner* Panner = AddNode<UMaterialExpressionPanner>(Material);
		Panner->SpeedX = SpeedX;
		Panner->SpeedY = SpeedY;
		return Panner;
	}

	bool DependsOn(const FMantleMaterialAnalysis& Analysis, const UObject* Object)
	{
		return Analysis.Dependencies.ContainsByPredicate([Object](const TWeakObjectPtr<UObject>& Weak) { return Weak.Get() == Object; });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePannedThroughFunctionTest,
	"Mantle.Editor.PannedMaterial.ThroughFunctionCall",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePannedThroughFunctionTest::RunTest(const FString& Parameters)
{
	using namespace MantlePannedFunctionTestPrivate;

	UTexture2D* Normal = UTexture2D::CreateTransient(4, 4);
	UMaterialFunction* Flow = MakeFlowFunction();

	// MM_Waterfall's normal: Lerp(Normalize(Lerp(flat, flow call, Normal Intensity)), Transform(flat), a foam mask that is not a constant).
	UMaterial* Material = NewObject<UMaterial>(GetTransientPackage());
	UMaterialExpressionLinearInterpolate* Strength = AddNode<UMaterialExpressionLinearInterpolate>(Material);
	Strength->A.Connect(0, AddFlatNormal(Material));
	Strength->B.Connect(0, AddFlowCall(Material, Normal, Flow));
	Strength->Alpha.Connect(0, AddScalar(Material, TEXT("Normal Intensity"), 0.5f));
	UMaterialExpressionNormalize* Unit = AddNode<UMaterialExpressionNormalize>(Material);
	Unit->VectorInput.Connect(0, Strength);
	UMaterialExpressionTransform* Up = AddNode<UMaterialExpressionTransform>(Material);
	Up->Input.Connect(0, AddFlatNormal(Material));
	UMaterialExpressionMultiply* Foam = AddNode<UMaterialExpressionMultiply>(Material);
	Foam->A.Connect(0, AddNode<UMaterialExpressionTime>(Material));
	Foam->B.Connect(0, AddNumber(Material, 0.1f));
	UMaterialExpressionLinearInterpolate* Outer = AddNode<UMaterialExpressionLinearInterpolate>(Material);
	Outer->A.Connect(0, Unit);
	Outer->B.Connect(0, Up);
	Outer->Alpha.Connect(0, Foam);
	Material->GetEditorOnlyData()->Normal.Connect(0, Outer);

	const FMantleMaterialAnalysis Seen = FMantlePlaceAssets::AnalyzeMaterial(Material);
	if (TestTrue(TEXT("the normal is a panned source texture"), Seen.Fidelity2.HasSource(1) && Seen.Fidelity2.bPanning[1]))
	{
		TestTrue(TEXT("of the texture the function's input gives"), Seen.Fidelity2.SourceTextures[1].Get() == Normal);
		TestEqual(TEXT("tiling u (X Tile x the first UV set)"), Seen.Fidelity2.SourceTiling[1][0], 2.0, 1.0e-6);
		TestEqual(TEXT("tiling v"), Seen.Fidelity2.SourceTiling[1][1], 3.0, 1.0e-6);
		TestEqual(TEXT("it does not pan along u"), Seen.Fidelity2.Panning[1][0], 0.0, 1.0e-6);
		TestEqual(TEXT("it pans along v at 0.25 x (1 x -1)"), Seen.Fidelity2.Panning[1][1], -0.25, 1.0e-6);
		TestEqual(TEXT("Normal Intensity is the normal scale"), Seen.Fidelity2.NormalScale, 0.5, 1.0e-6);
		TestTrue(TEXT("the foam mask is said to be dropped"), Seen.Fidelity2.bNormalFoamDropped);
	}
	TestFalse(TEXT("a moving normal is not a frozen animation"), Seen.Has(EMantleMaterialLoss::Animation));
	TestTrue(TEXT("the function is a dependency (editing it changes the result)"), DependsOn(Seen, Flow));
	TestTrue(TEXT("and so is the texture"), Seen.Textures.ContainsByPredicate([Normal](const TWeakObjectPtr<UTexture>& Weak) { return Weak.Get() == Normal; }));

	// An instance sets the parameters: Flow Speed 2 doubles the pan and X Tile 4 doubles the tiling.
	UMaterialInstanceConstant* Instance = NewObject<UMaterialInstanceConstant>(GetTransientPackage());
	Instance->SetParentEditorOnly(Material, false);
	Instance->UpdateOverridableBaseProperties();
	Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Flow Speed")), 2.0f);
	Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Normal X Tile")), 4.0f);
	const FMantleMaterialAnalysis Set = FMantlePlaceAssets::AnalyzeMaterial(Instance);
	if (TestTrue(TEXT("the instance's normal is a panned source texture"), Set.Fidelity2.HasSource(1) && Set.Fidelity2.bPanning[1]))
	{
		TestEqual(TEXT("its pan is the instance's"), Set.Fidelity2.Panning[1][1], -0.5, 1.0e-6);
		TestEqual(TEXT("its tiling is the instance's"), Set.Fidelity2.SourceTiling[1][0], 4.0, 1.0e-6);
	}

	// The function's own default for an input nothing is connected to: Speed 0.4 x 0.25 = 0.1 (no Lerp, no Normalize), UVs as they are.
	UMaterial* Defaulted = NewObject<UMaterial>(GetTransientPackage());
	UMaterialExpressionMaterialFunctionCall* Bare = AddCall(Defaulted, Flow);
	UMaterialExpressionTextureObjectParameter* Map = AddNode<UMaterialExpressionTextureObjectParameter>(Defaulted);
	Map->ParameterName = TEXT("Normal Map");
	Map->Texture = Normal;
	ConnectCallInput(Bare, TEXT("Normal Map"), Map);
	ConnectCallInput(Bare, TEXT("UV"), AddNode<UMaterialExpressionTextureCoordinate>(Defaulted));
	Defaulted->GetEditorOnlyData()->Normal.Connect(0, Bare);
	const FMantleMaterialAnalysis Default = FMantlePlaceAssets::AnalyzeMaterial(Defaulted);
	if (TestTrue(TEXT("a call with the defaults is a panned normal"), Default.Fidelity2.HasSource(1) && Default.Fidelity2.bPanning[1]))
	{
		TestEqual(TEXT("its pan is the input's default"), Default.Fidelity2.Panning[1][1], 0.1, 1.0e-6);
		TestEqual(TEXT("its tiling is the first UV set's own"), Default.Fidelity2.SourceTiling[1][0], 1.0, 1.0e-6);
		TestEqual(TEXT("and it is at full strength"), Default.Fidelity2.NormalScale, 1.0, 1.0e-6);
	}

	// A function input with nothing connected and no default to use stops the read (the UV input here has none to give).
	UMaterial* NoUv = NewObject<UMaterial>(GetTransientPackage());
	UMaterialExpressionMaterialFunctionCall* Orphan = AddCall(NoUv, Flow);
	UMaterialExpressionTextureObjectParameter* OrphanMap = AddNode<UMaterialExpressionTextureObjectParameter>(NoUv);
	OrphanMap->ParameterName = TEXT("Normal Map");
	OrphanMap->Texture = Normal;
	ConnectCallInput(Orphan, TEXT("Normal Map"), OrphanMap);
	NoUv->GetEditorOnlyData()->Normal.Connect(0, Orphan);
	const FMantleMaterialAnalysis Lost = FMantlePlaceAssets::AnalyzeMaterial(NoUv);
	TestFalse(TEXT("a UV input left at its default (0, 0) is not the first UV set"), Lost.Fidelity2.bPanning[1]);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePannedByTimeTest,
	"Mantle.Editor.PannedMaterial.TimeTimesSpeed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePannedByTimeTest::RunTest(const FString& Parameters)
{
	using namespace MantlePannedFunctionTestPrivate;

	UTexture2D* Color = UTexture2D::CreateTransient(4, 4);

	// UV x (2, 3) + Time x (0.1, -0.2) straight into the base color.
	UMaterial* Moving = NewObject<UMaterial>(GetTransientPackage());
	UMaterialExpressionMultiply* Tiled = AddNode<UMaterialExpressionMultiply>(Moving);
	Tiled->A.Connect(0, AddNode<UMaterialExpressionTextureCoordinate>(Moving));
	Tiled->B.Connect(0, AddPair(Moving, 2.0f, 3.0f));
	UMaterialExpressionMultiply* Drift = AddNode<UMaterialExpressionMultiply>(Moving);
	Drift->A.Connect(0, AddNode<UMaterialExpressionTime>(Moving));
	Drift->B.Connect(0, AddPair(Moving, 0.1f, -0.2f));
	UMaterialExpressionAdd* Sum = AddNode<UMaterialExpressionAdd>(Moving);
	Sum->A.Connect(0, Tiled);
	Sum->B.Connect(0, Drift);
	Moving->GetEditorOnlyData()->BaseColor.Connect(0, AddSample(Moving, Color, Sum));
	const FMantleMaterialAnalysis Seen = FMantlePlaceAssets::AnalyzeMaterial(Moving);
	if (TestTrue(TEXT("UV x tile + Time x speed is a panned source"), Seen.Fidelity2.HasSource(0) && Seen.Fidelity2.bPanning[0]))
	{
		TestEqual(TEXT("tiling u"), Seen.Fidelity2.SourceTiling[0][0], 2.0, 1.0e-6);
		TestEqual(TEXT("tiling v"), Seen.Fidelity2.SourceTiling[0][1], 3.0, 1.0e-6);
		TestEqual(TEXT("speed u"), Seen.Fidelity2.Panning[0][0], 0.1, 1.0e-6);
		TestEqual(TEXT("speed v"), Seen.Fidelity2.Panning[0][1], -0.2, 1.0e-6);
	}
	TestFalse(TEXT("so it is not frozen"), Seen.Has(EMantleMaterialLoss::Animation));

	// A Panner whose Time input is a Time node is the Panner's own time.
	UMaterial* Timed = NewObject<UMaterial>(GetTransientPackage());
	UMaterialExpressionPanner* Panner = AddPanner(Timed, 0.05f, 0.0f);
	Panner->Time.Connect(0, AddNode<UMaterialExpressionTime>(Timed));
	Timed->GetEditorOnlyData()->BaseColor.Connect(0, AddSample(Timed, Color, Panner));
	const FMantleMaterialAnalysis Clocked = FMantlePlaceAssets::AnalyzeMaterial(Timed);
	if (TestTrue(TEXT("a Panner on a Time node is panned"), Clocked.Fidelity2.bPanning[0]))
	{
		TestEqual(TEXT("at its own speed"), Clocked.Fidelity2.Panning[0][0], 0.05, 1.0e-6);
	}

	// The same sample on UVs that do not move is not a panned source: the bake has it right.
	UMaterial* Still = NewObject<UMaterial>(GetTransientPackage());
	UMaterialExpressionMultiply* Fixed = AddNode<UMaterialExpressionMultiply>(Still);
	Fixed->A.Connect(0, AddNode<UMaterialExpressionTextureCoordinate>(Still));
	Fixed->B.Connect(0, AddPair(Still, 2.0f, 3.0f));
	Still->GetEditorOnlyData()->BaseColor.Connect(0, AddSample(Still, Color, Fixed));
	const FMantleMaterialAnalysis Plain = FMantlePlaceAssets::AnalyzeMaterial(Still);
	TestFalse(TEXT("tiled UVs with no pan stay baked"), Plain.Fidelity2.bPanning[0]);

	// Time times a speed on a scalar that varies with the UVs is not linear: refused (and a frozen animation).
	UMaterial* Warped = NewObject<UMaterial>(GetTransientPackage());
	UMaterialExpressionMultiply* Product = AddNode<UMaterialExpressionMultiply>(Warped);
	Product->A.Connect(0, AddNode<UMaterialExpressionTime>(Warped));
	Product->B.Connect(0, AddNode<UMaterialExpressionTextureCoordinate>(Warped));
	Warped->GetEditorOnlyData()->BaseColor.Connect(0, AddSample(Warped, Color, Product));
	const FMantleMaterialAnalysis Refused = FMantlePlaceAssets::AnalyzeMaterial(Warped);
	TestFalse(TEXT("Time times the UVs is not a pan"), Refused.Fidelity2.bPanning[0]);
	TestTrue(TEXT("and it is reported frozen"), Refused.Has(EMantleMaterialLoss::Animation));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePannedNormalPassThroughTest,
	"Mantle.Editor.PannedMaterial.NormalizeAndFlatLerp",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePannedNormalPassThroughTest::RunTest(const FString& Parameters)
{
	using namespace MantlePannedFunctionTestPrivate;

	UTexture2D* Normal = UTexture2D::CreateTransient(4, 4);

	// Normalize(Lerp(flat, panned sample, 1)): the sample at full strength.
	UMaterial* Full = NewObject<UMaterial>(GetTransientPackage());
	UMaterialExpressionLinearInterpolate* Mixed = AddNode<UMaterialExpressionLinearInterpolate>(Full);
	Mixed->A.Connect(0, AddFlatNormal(Full));
	Mixed->B.Connect(0, AddSample(Full, Normal, AddPanner(Full, 0.0f, 0.2f)));
	Mixed->Alpha.Connect(0, AddNumber(Full, 1.0f));
	UMaterialExpressionNormalize* Unit = AddNode<UMaterialExpressionNormalize>(Full);
	Unit->VectorInput.Connect(0, Mixed);
	Full->GetEditorOnlyData()->Normal.Connect(0, Unit);
	const FMantleMaterialAnalysis One = FMantlePlaceAssets::AnalyzeMaterial(Full);
	if (TestTrue(TEXT("a Normalize and a Lerp against a flat normal are passed through"), One.Fidelity2.HasSource(1) && One.Fidelity2.bPanning[1]))
	{
		TestEqual(TEXT("pan v"), One.Fidelity2.Panning[1][1], 0.2, 1.0e-6);
		TestEqual(TEXT("at full strength"), One.Fidelity2.NormalScale, 1.0, 1.0e-6);
		TestFalse(TEXT("with no foam dropped"), One.Fidelity2.bNormalFoamDropped);
	}

	// Lerp(sample, flat, 0.25) (the flat on the B side): the sample keeps 0.75 of its tilt.
	UMaterial* Flattened = NewObject<UMaterial>(GetTransientPackage());
	UMaterialExpressionLinearInterpolate* Flatter = AddNode<UMaterialExpressionLinearInterpolate>(Flattened);
	Flatter->A.Connect(0, AddSample(Flattened, Normal, AddPanner(Flattened, 0.1f, 0.0f)));
	Flatter->B.Connect(0, AddFlatNormal(Flattened));
	Flatter->Alpha.Connect(0, AddNumber(Flattened, 0.25f));
	Flattened->GetEditorOnlyData()->Normal.Connect(0, Flatter);
	const FMantleMaterialAnalysis Two = FMantlePlaceAssets::AnalyzeMaterial(Flattened);
	if (TestTrue(TEXT("a Lerp toward flat is passed through"), Two.Fidelity2.bPanning[1]))
	{
		TestEqual(TEXT("keeping one minus its alpha"), Two.Fidelity2.NormalScale, 0.75, 1.0e-6);
		TestFalse(TEXT("a constant alpha is no foam"), Two.Fidelity2.bNormalFoamDropped);
	}

	// Two moving samples crossfaded by a clock (a flow map's two phases) is not one sample and one pan: refused, and frozen.
	UMaterial* Phased = NewObject<UMaterial>(GetTransientPackage());
	UMaterialExpressionLinearInterpolate* Crossfade = AddNode<UMaterialExpressionLinearInterpolate>(Phased);
	Crossfade->A.Connect(0, AddSample(Phased, Normal, AddPanner(Phased, 0.0f, 0.2f)));
	Crossfade->B.Connect(0, AddSample(Phased, Normal, AddPanner(Phased, 0.0f, 0.1f)));
	Crossfade->Alpha.Connect(0, AddNode<UMaterialExpressionTime>(Phased));
	Phased->GetEditorOnlyData()->Normal.Connect(0, Crossfade);
	const FMantleMaterialAnalysis Three = FMantlePlaceAssets::AnalyzeMaterial(Phased);
	TestFalse(TEXT("a crossfade of two samples is refused"), Three.Fidelity2.bPanning[1]);
	TestTrue(TEXT("and reported frozen"), Three.Has(EMantleMaterialLoss::Animation));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePannedFunctionDepthTest,
	"Mantle.Editor.PannedMaterial.FunctionDepthAndRecursion",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePannedFunctionDepthTest::RunTest(const FString& Parameters)
{
	using namespace MantlePannedFunctionTestPrivate;

	UTexture2D* Normal = UTexture2D::CreateTransient(4, 4);

	// A function whose output is a panned sample, nested in wrappers: each wrapper is one function deeper.
	auto PlainNested = [Normal](const int32 Wrappers)
	{
		UMaterialFunction* Function = NewObject<UMaterialFunction>(GetTransientPackage());
		UMaterialExpressionTextureSample* Sample = AddNode<UMaterialExpressionTextureSample>(Function);
		Sample->Texture = Normal;
		UMaterialExpressionPanner* Panner = AddNode<UMaterialExpressionPanner>(Function);
		Panner->SpeedY = 0.2f;
		Sample->Coordinates.Connect(0, Panner);
		UMaterialExpressionFunctionOutput* Output = AddNode<UMaterialExpressionFunctionOutput>(Function);
		Output->OutputName = TEXT("Normal");
		Output->Id = FGuid::NewGuid();
		Output->A.Connect(0, Sample);
		for (int32 Index = 0; Index < Wrappers; ++Index)
		{
			Function = MakeWrapper(Function);
		}
		UMaterial* Material = NewObject<UMaterial>(GetTransientPackage());
		Material->GetEditorOnlyData()->Normal.Connect(0, AddCall(Material, Function));
		return Material;
	};

	const FMantleMaterialAnalysis Shallow = FMantlePlaceAssets::AnalyzeMaterial(PlainNested(2));
	TestTrue(TEXT("a sample two functions down is found"), Shallow.Fidelity2.bPanning[1]);
	const FMantleMaterialAnalysis Deep = FMantlePlaceAssets::AnalyzeMaterial(PlainNested(8));
	TestFalse(TEXT("a sample eight functions down is past the depth limit"), Deep.Fidelity2.bPanning[1]);
	TestTrue(TEXT("and is reported frozen, as it was"), Deep.Has(EMantleMaterialLoss::Animation));

	// A function whose output is a call of itself is refused, not followed for ever.
	UMaterialFunction* Loop = NewObject<UMaterialFunction>(GetTransientPackage());
	UMaterialExpressionFunctionOutput* LoopOutput = AddNode<UMaterialExpressionFunctionOutput>(Loop);
	LoopOutput->OutputName = TEXT("Result");
	LoopOutput->Id = FGuid::NewGuid();
	UMaterialExpressionMaterialFunctionCall* Again = AddNode<UMaterialExpressionMaterialFunctionCall>(Loop);
	Again->SetMaterialFunction(Loop);
	LoopOutput->A.Connect(0, Again);
	UMaterial* Recursive = NewObject<UMaterial>(GetTransientPackage());
	Recursive->GetEditorOnlyData()->Normal.Connect(0, AddCall(Recursive, Loop));
	const FMantleMaterialAnalysis Looped = FMantlePlaceAssets::AnalyzeMaterial(Recursive);
	TestFalse(TEXT("a function that calls itself is refused"), Looped.Fidelity2.bPanning[1]);
	return true;
}

namespace MantlePannedFlowTestPrivate
{
	using namespace MantlePannedFunctionTestPrivate;

	/** MF_NormalBlend, cut down: Append(RG of (A + B), B of A), the output of two inputs. */
	UMaterialFunction* MakeBlendFunction()
	{
		UMaterialFunction* Function = NewObject<UMaterialFunction>(GetTransientPackage());
		UMaterialExpressionFunctionInput* A = AddInput(Function, TEXT("A"), FunctionInput_Vector3, 0.0f);
		UMaterialExpressionFunctionInput* B = AddInput(Function, TEXT("B"), FunctionInput_Vector3, 0.0f);
		UMaterialExpressionAdd* Sum = AddNode<UMaterialExpressionAdd>(Function);
		Sum->A.Connect(0, A);
		Sum->B.Connect(0, B);
		UMaterialExpressionComponentMask* Xy = AddNode<UMaterialExpressionComponentMask>(Function);
		Xy->R = true;
		Xy->G = true;
		Xy->B = false;
		Xy->A = false;
		Xy->Input.Connect(0, Sum);
		UMaterialExpressionComponentMask* Z = AddNode<UMaterialExpressionComponentMask>(Function);
		Z->R = false;
		Z->G = false;
		Z->B = true;
		Z->A = false;
		Z->Input.Connect(0, A);
		UMaterialExpressionAppendVector* Joined = AddNode<UMaterialExpressionAppendVector>(Function);
		Joined->A.Connect(0, Xy);
		Joined->B.Connect(0, Z);
		UMaterialExpressionFunctionOutput* Output = AddNode<UMaterialExpressionFunctionOutput>(Function);
		Output->OutputName = TEXT("Result");
		Output->Id = FGuid::NewGuid();
		Output->A.Connect(0, Joined);
		return Function;
	}

	/** One of the flow function's samples: the Texture input, UVs panned by Speed x the clock. */
	UMaterialExpressionTextureSample* AddFlowSample(UMaterialFunction* Function, UMaterialExpression* Texture, UMaterialExpression* Clock, UMaterialExpression* Coordinates,
		const float SpeedX, const float SpeedY)
	{
		UMaterialExpressionPanner* Panner = AddNode<UMaterialExpressionPanner>(Function);
		Panner->SpeedX = SpeedX;
		Panner->SpeedY = SpeedY;
		Panner->Coordinate.Connect(0, Coordinates);
		Panner->Time.Connect(0, Clock);
		UMaterialExpressionTextureSample* Sample = AddNode<UMaterialExpressionTextureSample>(Function);
		Sample->TextureObject.Connect(0, Texture);
		Sample->Coordinates.Connect(0, Panner);
		return Sample;
	}

	UMaterialExpressionMaterialFunctionCall* AddBlend(UMaterialFunction* Function, UMaterialFunction* Blend, UMaterialExpression* A, UMaterialExpression* B)
	{
		UMaterialExpressionMaterialFunctionCall* Call = AddNode<UMaterialExpressionMaterialFunctionCall>(Function);
		Call->SetMaterialFunction(Blend);
		ConnectCallInput(Call, TEXT("A"), A);
		ConnectCallInput(Call, TEXT("B"), B);
		return Call;
	}

	/**
	 * MF_RiverMotionNormal as it was dumped: inputs Direction (static bool), Time, Texture and Input Coordinates (none used as defaults); the
	 * Panners' Time is the Time node times the Time input; four samples of the Texture input, the second of each pair on the coordinates
	 * plus (0.4, 0.35); a blend function for each pair; a static switch on Direction picks the pair (true: the Y pair).
	 */
	UMaterialFunction* MakeRiverFunction(UMaterialFunction* Blend)
	{
		UMaterialFunction* Function = NewObject<UMaterialFunction>(GetTransientPackage());
		UMaterialExpressionFunctionInput* Direction = AddInput(Function, TEXT("Direction X or Y"), FunctionInput_StaticBool, 0.0f);
		UMaterialExpressionFunctionInput* TimeInput = AddInput(Function, TEXT("Time"), FunctionInput_Scalar, 0.0f);
		UMaterialExpressionFunctionInput* Texture = AddInput(Function, TEXT("Texture"), FunctionInput_Texture2D, 0.0f);
		UMaterialExpressionFunctionInput* Coordinates = AddInput(Function, TEXT("Input Coordinates"), FunctionInput_Vector2, 0.0f);
		Direction->bUsePreviewValueAsDefault = false;
		TimeInput->bUsePreviewValueAsDefault = false;
		Texture->bUsePreviewValueAsDefault = false;
		Coordinates->bUsePreviewValueAsDefault = false;

		UMaterialExpressionMultiply* Clock = AddNode<UMaterialExpressionMultiply>(Function);
		Clock->A.Connect(0, AddNode<UMaterialExpressionTime>(Function));
		Clock->B.Connect(0, TimeInput);
		UMaterialExpressionAdd* Shifted = AddNode<UMaterialExpressionAdd>(Function);
		Shifted->A.Connect(0, Coordinates);
		Shifted->B.Connect(0, AddPair(Function, 0.4f, 0.35f));

		UMaterialExpressionTextureSample* Y1 = AddFlowSample(Function, Texture, Clock, Coordinates, -0.02f, 0.5f);
		UMaterialExpressionTextureSample* Y2 = AddFlowSample(Function, Texture, Clock, Shifted, 0.02f, 0.25f);
		UMaterialExpressionTextureSample* X1 = AddFlowSample(Function, Texture, Clock, Coordinates, 0.5f, -0.02f);
		UMaterialExpressionTextureSample* X2 = AddFlowSample(Function, Texture, Clock, Shifted, 0.25f, 0.02f);

		UMaterialExpressionStaticSwitch* Pick = AddNode<UMaterialExpressionStaticSwitch>(Function);
		Pick->Value.Connect(0, Direction);
		Pick->A.Connect(0, AddBlend(Function, Blend, Y1, Y2));
		Pick->B.Connect(0, AddBlend(Function, Blend, X1, X2));
		UMaterialExpressionFunctionOutput* Output = AddNode<UMaterialExpressionFunctionOutput>(Function);
		Output->OutputName = TEXT("Normal");
		Output->Id = FGuid::NewGuid();
		Output->A.Connect(0, Pick);
		return Function;
	}

	/** A material whose Normal is a call of the river function as MM_Waterfall makes it; Direction is the node given. */
	UMaterial* NewRiver(UMaterialFunction* River, UTexture* Texture, UMaterialExpression* (*MakeDirection)(UMaterial*))
	{
		UMaterial* Material = NewObject<UMaterial>(GetTransientPackage());
		UMaterialExpressionMaterialFunctionCall* Call = AddCall(Material, River);
		UMaterialExpressionMultiply* Reversed = AddNode<UMaterialExpressionMultiply>(Material);
		Reversed->A.Connect(0, AddScalar(Material, TEXT("Flow Speed"), 1.0f));
		Reversed->B.Connect(0, AddNumber(Material, -1.0f));
		UMaterialExpressionTextureObjectParameter* Map = AddNode<UMaterialExpressionTextureObjectParameter>(Material);
		Map->ParameterName = TEXT("Normal Map");
		Map->Texture = Texture;
		UMaterialExpressionAppendVector* Tiles = AddNode<UMaterialExpressionAppendVector>(Material);
		Tiles->A.Connect(0, AddScalar(Material, TEXT("Normal X Tile"), 2.0f));
		Tiles->B.Connect(0, AddScalar(Material, TEXT("Normal Y Tile"), 3.0f));
		UMaterialExpressionMultiply* Scaled = AddNode<UMaterialExpressionMultiply>(Material);
		Scaled->A.Connect(0, Tiles);
		Scaled->B.Connect(0, AddNode<UMaterialExpressionTextureCoordinate>(Material));
		ConnectCallInput(Call, TEXT("Direction X or Y"), MakeDirection(Material));
		ConnectCallInput(Call, TEXT("Time"), Reversed);
		ConnectCallInput(Call, TEXT("Texture"), Map);
		ConnectCallInput(Call, TEXT("Input Coordinates"), Scaled);
		Material->GetEditorOnlyData()->Normal.Connect(0, Call);
		return Material;
	}

	UMaterialExpression* MakeYDirection(UMaterial* Material)
	{
		UMaterialExpressionStaticBool* Bool = AddNode<UMaterialExpressionStaticBool>(Material);
		Bool->Value = true;
		return Bool;
	}

	UMaterialExpression* MakeXParameter(UMaterial* Material)
	{
		UMaterialExpressionStaticBoolParameter* Bool = AddNode<UMaterialExpressionStaticBoolParameter>(Material);
		Bool->ParameterName = TEXT("Direction");
		Bool->DefaultValue = false;
		return Bool;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePannedFlowLayersTest,
	"Mantle.Editor.PannedMaterial.FlowLayersKeptAsOne",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePannedFlowLayersTest::RunTest(const FString& Parameters)
{
	using namespace MantlePannedFlowTestPrivate;

	UTexture2D* Normal = UTexture2D::CreateTransient(4, 4);
	UMaterialFunction* River = MakeRiverFunction(MakeBlendFunction());

	// Direction true (a Static Bool node): the Y pair; its faster layer is (-0.02, 0.5) x the clock's -1.
	const FMantleMaterialAnalysis Y = FMantlePlaceAssets::AnalyzeMaterial(NewRiver(River, Normal, MakeYDirection));
	if (TestTrue(TEXT("the Y pair is carried as one panned source"), Y.Fidelity2.HasSource(1) && Y.Fidelity2.bPanning[1]))
	{
		TestTrue(TEXT("of the texture the function's input gives"), Y.Fidelity2.SourceTextures[1].Get() == Normal);
		TestEqual(TEXT("speed u: the faster layer, reversed"), Y.Fidelity2.Panning[1][0], 0.02, 1.0e-6);
		TestEqual(TEXT("speed v"), Y.Fidelity2.Panning[1][1], -0.5, 1.0e-6);
		TestEqual(TEXT("tiling u"), Y.Fidelity2.SourceTiling[1][0], 2.0, 1.0e-6);
		TestEqual(TEXT("tiling v"), Y.Fidelity2.SourceTiling[1][1], 3.0, 1.0e-6);
		TestEqual(TEXT("at full strength"), Y.Fidelity2.NormalScale, 1.0, 1.0e-6);
		TestTrue(TEXT("two flowing layers kept as one is said"), Y.Fidelity2.bNormalLayersMerged);
	}
	TestFalse(TEXT("the flow is not a frozen animation"), Y.Has(EMantleMaterialLoss::Animation));
	TestTrue(TEXT("the function is a dependency"), DependsOn(Y, River));

	// Direction a Static Bool Parameter left at false: the X pair, (0.5, -0.02) x -1.
	const FMantleMaterialAnalysis X = FMantlePlaceAssets::AnalyzeMaterial(NewRiver(River, Normal, MakeXParameter));
	if (TestTrue(TEXT("the X pair is carried as one panned source"), X.Fidelity2.bPanning[1]))
	{
		TestEqual(TEXT("speed u"), X.Fidelity2.Panning[1][0], -0.5, 1.0e-6);
		TestEqual(TEXT("speed v"), X.Fidelity2.Panning[1][1], 0.02, 1.0e-6);
		TestTrue(TEXT("two flowing layers kept as one is said"), X.Fidelity2.bNormalLayersMerged);
	}

	// Blends outside a function, and what is refused.
	UTexture2D* Other = UTexture2D::CreateTransient(4, 4);
	auto Blended = [](UTexture* First, UTexture* Second, const bool bSecondMoves, const bool bOffset)
	{
		UMaterial* Material = NewObject<UMaterial>(GetTransientPackage());
		UMaterialExpressionPanner* Fast = AddPanner(Material, 0.0f, 0.4f);
		UMaterialExpressionPanner* Slow = AddPanner(Material, 0.0f, 0.1f);
		if (bOffset)
		{
			UMaterialExpressionAdd* Shifted = AddNode<UMaterialExpressionAdd>(Material);
			Shifted->A.Connect(0, AddNode<UMaterialExpressionTextureCoordinate>(Material));
			Shifted->B.Connect(0, AddPair(Material, 0.4f, 0.35f));
			Slow->Coordinate.Connect(0, Shifted);
		}
		UMaterialExpressionAdd* Sum = AddNode<UMaterialExpressionAdd>(Material);
		Sum->A.Connect(0, AddSample(Material, First, Fast));
		Sum->B.Connect(0, AddSample(Material, Second, bSecondMoves ? static_cast<UMaterialExpression*>(Slow) : nullptr));
		Material->GetEditorOnlyData()->Normal.Connect(0, Sum);
		return Material;
	};

	const FMantleMaterialAnalysis Same = FMantlePlaceAssets::AnalyzeMaterial(Blended(Normal, Normal, true, true));
	if (TestTrue(TEXT("two moving samples of one texture are one"), Same.Fidelity2.bPanning[1] && Same.Fidelity2.bNormalLayersMerged))
	{
		TestEqual(TEXT("the faster stands for them"), Same.Fidelity2.Panning[1][1], 0.4, 1.0e-6);
	}
	TestFalse(TEXT("two textures are refused"), FMantlePlaceAssets::AnalyzeMaterial(Blended(Normal, Other, true, false)).Fidelity2.bPanning[1]);
	TestFalse(TEXT("a still sample among them is refused"), FMantlePlaceAssets::AnalyzeMaterial(Blended(Normal, Normal, false, false)).Fidelity2.bPanning[1]);

	// One sample whose UVs have an offset added: the offset would be dropped with nothing to say so, so it is refused.
	UMaterial* Offset = NewObject<UMaterial>(GetTransientPackage());
	UMaterialExpressionAdd* Shifted = AddNode<UMaterialExpressionAdd>(Offset);
	Shifted->A.Connect(0, AddNode<UMaterialExpressionTextureCoordinate>(Offset));
	Shifted->B.Connect(0, AddPair(Offset, 0.4f, 0.35f));
	UMaterialExpressionPanner* Moving = AddPanner(Offset, 0.0f, 0.4f);
	Moving->Coordinate.Connect(0, Shifted);
	Offset->GetEditorOnlyData()->Normal.Connect(0, AddSample(Offset, Normal, Moving));
	TestFalse(TEXT("a lone sample with a UV offset is refused"), FMantlePlaceAssets::AnalyzeMaterial(Offset).Fidelity2.bPanning[1]);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
