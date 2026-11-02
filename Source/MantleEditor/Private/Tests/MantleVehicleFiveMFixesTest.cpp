// Copyright Joshua Eger. All Rights Reserved.
#include "MantleGlowMaterial.h"
#include "MantleMeshReduce.h"
#include "MantleSkinnedMesh.h"
#include "MantleVehicleAsset.h"
#include "MantleVehicleBodyReduce.h"
#include "MantleVehicleBuilder.h"
#include "MantleVehicleConverter.h"
#include "MantleVehicleLightFinder.h"
#include "MantleVehicleScratchBody.h"
#include "MantleVehicleTypeMatch.h"

#include "Engine/SkeletalMesh.h"
#include "Engine/SkinnedAssetCommon.h"
#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// What a FiveM car (a GTA model through Sollumz and Blender, as a glb) needs of the vehicle builder: a glTF material's other textures are not its glow, a flat white
// switched off is no light, GTA's part and wheel names are read, a steering wheel is not a wheel, and a body over the triangle limit is reduced and not refused.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVehicleFiveMTestPrivate
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

	/** A cell by cell texture in Outer: every pixel Color, or when Lit is set only those cells. */
	UTexture2D* MakeTexture(UObject* Outer, const TCHAR* Name, const FColor& Color, const TArray<FIntPoint>& Lit = {})
	{
		TArray<FColor> Pixels;
		Pixels.Init(Lit.Num() > 0 ? FColor(0, 0, 0, 255) : Color, 16);
		for (const FIntPoint& Cell : Lit)
		{
			Pixels[Cell.Y * 4 + Cell.X] = Color;
		}
		UTexture2D* Texture = NewObject<UTexture2D>(Outer, FName(Name), RF_Transient);
		Texture->SRGB = false;
		Texture->Source.Init(4, 4, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Pixels.GetData()));
		return Texture;
	}

	/**
	 * A glTF material as the engine imports one, in little: one function call whose outputs are Emissive (its Factor times its Strength times its texture) and BaseColor
	 * (its own texture). The call is given the base color texture and the emissive texture, the factor and the strength, as the glTF master material gives them.
	 */
	UMaterial* MakeGltfMaterial(UTexture2D* EmissiveTexture, UTexture2D* BaseTexture, const FLinearColor& Factor, const double Strength)
	{
		UMaterialFunction* Function = NewObject<UMaterialFunction>(GetTransientPackage(), NAME_None, RF_Transient);
		const auto MakeInput = [Function](const TCHAR* Name, const int32 Index)
		{
			UMaterialExpressionFunctionInput* Input = AddFunctionNode<UMaterialExpressionFunctionInput>(Function);
			Input->InputName = Name;
			Input->InputType = FunctionInput_Vector3;
			Input->SortPriority = Index;
			Input->Id = FGuid::NewGuid();
			return Input;
		};
		UMaterialExpressionFunctionInput* BaseIn = MakeInput(TEXT("BaseColorIn"), 0);
		UMaterialExpressionFunctionInput* FactorIn = MakeInput(TEXT("FactorIn"), 1);
		UMaterialExpressionFunctionInput* StrengthIn = MakeInput(TEXT("StrengthIn"), 2);
		UMaterialExpressionFunctionInput* EmissiveIn = MakeInput(TEXT("EmissiveIn"), 3);
		UMaterialExpressionMultiply* Product = AddFunctionNode<UMaterialExpressionMultiply>(Function);
		Product->A.Connect(0, FactorIn);
		Product->B.Connect(0, EmissiveIn);
		UMaterialExpressionMultiply* Strong = AddFunctionNode<UMaterialExpressionMultiply>(Function);
		Strong->A.Connect(0, Product);
		Strong->B.Connect(0, StrengthIn);
		UMaterialExpressionFunctionOutput* BaseOut = AddFunctionNode<UMaterialExpressionFunctionOutput>(Function);
		BaseOut->OutputName = TEXT("BaseColor");
		BaseOut->A.Connect(0, BaseIn);
		BaseOut->Id = FGuid::NewGuid();
		UMaterialExpressionFunctionOutput* EmissiveOut = AddFunctionNode<UMaterialExpressionFunctionOutput>(Function);
		EmissiveOut->OutputName = TEXT("EmissiveColor");
		EmissiveOut->A.Connect(0, Strong);
		EmissiveOut->Id = FGuid::NewGuid();

		UMaterial* Material = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
		UMaterialExpressionMaterialFunctionCall* Call = AddNode<UMaterialExpressionMaterialFunctionCall>(Material);
		Call->SetMaterialFunction(Function);
		const auto Sample = [Material](const TCHAR* Parameter, UTexture2D* Texture)
		{
			UMaterialExpressionTextureSampleParameter2D* Node = AddNode<UMaterialExpressionTextureSampleParameter2D>(Material);
			Node->ParameterName = Parameter;
			Node->Texture = Texture;
			Node->SamplerType = SAMPLERTYPE_LinearColor;
			Node->UpdateParameterGuid(true, false);
			return Node;
		};
		UMaterialExpressionVectorParameter* FactorNode = AddNode<UMaterialExpressionVectorParameter>(Material);
		FactorNode->ParameterName = TEXT("EmissiveFactor");
		FactorNode->DefaultValue = Factor;
		FactorNode->UpdateParameterGuid(true, false);
		UMaterialExpressionScalarParameter* StrengthNode = AddNode<UMaterialExpressionScalarParameter>(Material);
		StrengthNode->ParameterName = TEXT("EmissiveStrength");
		StrengthNode->DefaultValue = static_cast<float>(Strength);
		StrengthNode->UpdateParameterGuid(true, false);
		for (FFunctionExpressionInput& Input : Call->FunctionInputs)
		{
			if (Input.ExpressionInput == BaseIn)
			{
				Input.Input.Connect(0, Sample(TEXT("BaseColorTexture"), BaseTexture));
			}
			else if (Input.ExpressionInput == FactorIn)
			{
				Input.Input.Connect(0, FactorNode);
			}
			else if (Input.ExpressionInput == StrengthIn)
			{
				Input.Input.Connect(0, StrengthNode);
			}
			else if (Input.ExpressionInput == EmissiveIn)
			{
				Input.Input.Connect(0, Sample(TEXT("EmissiveTexture"), EmissiveTexture));
			}
		}
		for (int32 Output = 0; Output < Call->Outputs.Num(); ++Output)
		{
			if (Call->Outputs[Output].OutputName == FName(TEXT("EmissiveColor")))
			{
				Material->GetEditorOnlyData()->EmissiveColor.Connect(Output, Call);
			}
			else if (Call->Outputs[Output].OutputName == FName(TEXT("BaseColor")))
			{
				Material->GetEditorOnlyData()->BaseColor.Connect(Output, Call);
			}
		}
		Material->UpdateCachedExpressionData();
		return Material;
	}

	/** A body 400 long, 160 wide and 100 high with a quad at each end (front left, back left) and one on the roof, all one material: enough for the finder's ends. */
	void MakeBody(FMantleSkinnedMesh& OutBody, TArray<FSkeletalMaterial>& OutMaterials, const FName SlotName)
	{
		OutBody.Bones = { { FName(TEXT("root")), INDEX_NONE } };
		OutBody.ReferencePose = { FTransform::Identity };
		FMantleSkinnedMeshLOD& LOD = OutBody.LODs.AddDefaulted_GetRef();
		LOD.NumInfluences = 1;
		OutMaterials.Add(FSkeletalMaterial(nullptr, SlotName));
		const FVector3f Centers[] = { { 200.f, -60.f, 50.f }, { -200.f, 60.f, 50.f }, { 0.f, 0.f, 100.f } };
		for (const FVector3f& Center : Centers)
		{
			const uint32 First = static_cast<uint32>(LOD.Positions.Num());
			for (const FVector3f& Corner : { FVector3f(0.f, -10.f, -10.f), FVector3f(0.f, 10.f, -10.f), FVector3f(0.f, 10.f, 10.f), FVector3f(0.f, -10.f, 10.f) })
			{
				LOD.Positions.Add(Center + Corner);
				LOD.Normals.Add(FVector3f(1.f, 0.f, 0.f));
				LOD.UVs.Add(FVector2f(0.5f, 0.5f));
				LOD.InfluenceBones.Add(0);
				LOD.InfluenceWeights.Add(255);
			}
			LOD.Indices.Append({ First, First + 1, First + 2, First, First + 2, First + 3 });
		}
		LOD.Positions.Add(FVector3f(-200.f, -80.f, 0.f));
		LOD.Positions.Add(FVector3f(200.f, 80.f, 100.f));
		for (int32 Extra = 0; Extra < 2; ++Extra)
		{
			LOD.Normals.Add(FVector3f(0.f, 0.f, 1.f));
			LOD.UVs.Add(FVector2f(0.f, 0.f));
			LOD.InfluenceBones.Add(0);
			LOD.InfluenceWeights.Add(255);
		}
		FMantleSkinnedMeshSection& Section = LOD.Sections.AddDefaulted_GetRef();
		Section.MaterialIndex = 0;
		Section.MaterialName = SlotName;
		Section.FirstIndex = 0;
		Section.IndexCount = LOD.Indices.Num();
	}

	int32 TotalLights(const FMantleVehicleLightGuess& Guess)
	{
		int32 Total = 0;
		for (int32 Kind = 0; Kind < FMantleVehicleLights::NumKinds; ++Kind)
		{
			Total += Guess.Triangles[Kind].Num() + Guess.WholeSlots[Kind].Num();
		}
		return Total;
	}

	/** A flat grid of Cells x Cells quads (two triangles each) riding Bone, on material Material, appended to Body's LOD 0. */
	void AddGrid(FMantleSkinnedMesh& Body, const int32 Cells, const int32 Bone, const int32 Material, const FVector3f& Origin)
	{
		FMantleSkinnedMeshLOD& LOD = Body.LODs[0];
		const uint32 First = static_cast<uint32>(LOD.Positions.Num());
		for (int32 Y = 0; Y <= Cells; ++Y)
		{
			for (int32 X = 0; X <= Cells; ++X)
			{
				// A gentle swell, so the reducer has curvature to keep.
				const float Height = 2.f * FMath::Sin(X * 0.2f) * FMath::Cos(Y * 0.2f);
				LOD.Positions.Add(Origin + FVector3f(X * 2.f, Y * 2.f, Height));
				LOD.Normals.Add(FVector3f(0.f, 0.f, 1.f));
				LOD.UVs.Add(FVector2f(static_cast<float>(X) / Cells, static_cast<float>(Y) / Cells));
				LOD.InfluenceBones.Add(static_cast<uint16>(Bone));
				LOD.InfluenceWeights.Add(255);
			}
		}
		const int32 FirstIndex = LOD.Indices.Num();
		for (int32 Y = 0; Y < Cells; ++Y)
		{
			for (int32 X = 0; X < Cells; ++X)
			{
				const uint32 A = First + Y * (Cells + 1) + X;
				const uint32 B = A + 1;
				const uint32 C = A + (Cells + 1);
				const uint32 D = C + 1;
				LOD.Indices.Append({ A, C, B, B, C, D });
			}
		}
		FMantleSkinnedMeshSection& Section = LOD.Sections.AddDefaulted_GetRef();
		Section.MaterialIndex = Material;
		Section.MaterialName = FName(*FString::Printf(TEXT("Material%d"), Material));
		Section.FirstIndex = FirstIndex;
		Section.IndexCount = LOD.Indices.Num() - FirstIndex;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleFiveMGlowTest,
	"Mantle.Vehicle.FiveM.GltfMaterialsAreNotLights",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleFiveMGlowTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleFiveMTestPrivate;

	// The glTF master material: the glow is what the Emissive output reads. The base color texture goes into the same function call and is not the glow.
	UPackage* Outer = GetTransientPackage();
	UTexture2D* WhiteDefault = MakeTexture(Outer, TEXT("T_White_srgb"), FColor::White);
	UTexture2D* Paint = MakeTexture(Outer, TEXT("Paint"), FColor(200, 30, 30, 255), { FIntPoint(0, 0), FIntPoint(1, 1) });
	UMaterial* Off = MakeGltfMaterial(WhiteDefault, Paint, FLinearColor::Black, 1.0);
	FMantleMaterialGlow Glow;
	FMantleMaterialGlow::Read(Off, Glow);
	TestEqual(TEXT("only the emissive texture is on the glow path, not the base color"), Glow.Textures.Num(), 1);
	TestTrue(TEXT("and it is the emissive one"), Glow.Textures.Num() == 1 && Glow.Textures[0].Texture == WhiteDefault);
	TestTrue(TEXT("the base color is read separately, as the lens color"), Glow.BaseTexture == Paint);
	TestTrue(TEXT("a black Emissive Factor is a glow switched off"), Glow.bColorOff);

	// The finder: nothing on the body is a light, whatever the paint looks like.
	FMantleSkinnedMesh Body;
	TArray<FSkeletalMaterial> Materials;
	MakeBody(Body, Materials, TEXT("plastic_black_01"));
	Materials[0].MaterialInterface = Off;
	TestEqual(TEXT("a material with a black emissive factor makes no light"), TotalLights(FMantleVehicleLights::Find(Body, Materials, false)), 0);

	// A white factor at strength 0 is off as well.
	Materials[0].MaterialInterface = MakeGltfMaterial(WhiteDefault, Paint, FLinearColor::White, 0.0);
	FMantleMaterialGlow::Read(Materials[0].MaterialInterface, Glow);
	TestTrue(TEXT("a strength of 0 is listed off"), Glow.OffScalars.Contains(FName(TEXT("EmissiveStrength"))));
	TestEqual(TEXT("a flat white switched off by its strength makes no light"), TotalLights(FMantleVehicleLights::Find(Body, Materials, false)), 0);

	// Switched on, the same flat white is a light: the quad at the front is a headlight and the one at the back a reverse light.
	Materials[0].MaterialInterface = MakeGltfMaterial(WhiteDefault, Paint, FLinearColor::White, 1.0);
	const FMantleVehicleLightGuess On = FMantleVehicleLights::Find(Body, Materials, false);
	TestTrue(TEXT("a white glow that is on still finds lights"), TotalLights(On) > 0);

	// A glow painted in a texture is a glow even with its strength at 0 (a pack ships it so, and the finder turns it on in the copy).
	UTexture2D* Painted = MakeTexture(Outer, TEXT("T_Emissive"), FColor::White, { FIntPoint(2, 2) });
	Materials[0].MaterialInterface = MakeGltfMaterial(Painted, Paint, FLinearColor::White, 0.0);
	for (FVector2f& UV : Body.LODs[0].UVs)
	{
		UV = FVector2f(0.6f, 0.6f);
	}
	TestTrue(TEXT("a painted glow with its strength at 0 is still found"), TotalLights(FMantleVehicleLights::Find(Body, Materials, false)) > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleFiveMNamesTest,
	"Mantle.Vehicle.FiveM.GtaNames",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleFiveMNamesTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleFiveMTestPrivate;

	// The lights, by the names a GTA car's parts and materials carry.
	FBox3f Bounds(FVector3f(-200.f, -80.f, 0.f), FVector3f(200.f, 80.f, 100.f));
	const FVector3f Front(190.f, -50.f, 50.f);
	const FVector3f Rear(-190.f, 50.f, 50.f);
	const auto Says = [this, &Bounds](const TCHAR* Name, const FVector3f& Where, const TOptional<EMantleLightKind> Expected)
	{
		TestTrue(*FString::Printf(TEXT("'%s' reads as expected"), Name), FMantleVehicleLights::FromName(Name, Where, Bounds) == Expected);
	};
	Says(TEXT("headlight_l"), Front, EMantleLightKind::Head);
	Says(TEXT("headlights_x5"), Front, EMantleLightKind::Head);
	Says(TEXT("taillight_r"), Rear, EMantleLightKind::Rear);
	Says(TEXT("brakelight_m"), Rear, EMantleLightKind::Rear);
	Says(TEXT("x7_brakelight"), Rear, EMantleLightKind::Rear);
	Says(TEXT("reversinglight_l"), Rear, EMantleLightKind::Reverse);
	Says(TEXT("ReverseLights"), Rear, EMantleLightKind::Reverse);
	Says(TEXT("indicator_lf"), Front, EMantleLightKind::Left);
	Says(TEXT("indicator_rr"), FVector3f(-190.f, 50.f, 50.f), EMantleLightKind::Right);
	Says(TEXT("window_lf"), Front, TOptional<EMantleLightKind>());
	Says(TEXT("wheel_lf"), Front, TOptional<EMantleLightKind>());
	Says(TEXT("steeringwheel"), Front, TOptional<EMantleLightKind>());

	// A body rigged part by part: the triangles on a bone named for a light are that light, whatever their material.
	FMantleSkinnedMesh Body;
	TArray<FSkeletalMaterial> Materials;
	MakeBody(Body, Materials, TEXT("plain_material"));
	Body.Bones.Add({ FName(TEXT("headlight_l")), 0 });
	Body.ReferencePose.Add(FTransform::Identity);
	Body.Bones.Add({ FName(TEXT("reversinglight_l")), 0 });
	Body.ReferencePose.Add(FTransform::Identity);
	// The first quad (front) rides the headlight bone, the second (back) the reversing light's.
	for (int32 Vertex = 0; Vertex < 4; ++Vertex)
	{
		Body.LODs[0].InfluenceBones[Vertex] = 1;
		Body.LODs[0].InfluenceBones[4 + Vertex] = 2;
	}
	const FMantleVehicleLightGuess Guess = FMantleVehicleLights::Find(Body, Materials, false);
	TestEqual(TEXT("the headlight bone's triangles are headlights"), Guess.Triangles[static_cast<int32>(EMantleLightKind::Head)], TArray<int32>({ 0, 1 }));
	TestEqual(TEXT("the reversing light bone's triangles are reverse lights"), Guess.Triangles[static_cast<int32>(EMantleLightKind::Reverse)], TArray<int32>({ 2, 3 }));

	// The wheels, by GTA's order: the side first.
	FMantleVehicleWheelBone Wheel;
	TestTrue(TEXT("wheel_lf is a wheel"), FMantleVehicleConverter::ParseWheelBoneName(TEXT("wheel_lf"), Wheel));
	TestTrue(TEXT("on the front axle, left"), Wheel.Axle == 0 && Wheel.bLeft);
	TestTrue(TEXT("wheel_rf is a wheel"), FMantleVehicleConverter::ParseWheelBoneName(TEXT("wheel_rf"), Wheel));
	TestTrue(TEXT("on the front axle, right"), Wheel.Axle == 0 && !Wheel.bLeft);
	TestTrue(TEXT("wheel_lr is a wheel"), FMantleVehicleConverter::ParseWheelBoneName(TEXT("wheel_lr"), Wheel));
	TestTrue(TEXT("on the rear axle, left"), Wheel.Axle == 2 && Wheel.bLeft);
	TestTrue(TEXT("wheel_rr is a wheel"), FMantleVehicleConverter::ParseWheelBoneName(TEXT("wheel_rr"), Wheel));
	TestTrue(TEXT("on the rear axle, right"), Wheel.Axle == 2 && !Wheel.bLeft);
	TestFalse(TEXT("hub_lf is not"), FMantleVehicleConverter::ParseWheelBoneName(TEXT("hub_lf"), Wheel));
	TestFalse(TEXT("nor is the steering wheel"), FMantleVehicleConverter::ParseWheelBoneName(TEXT("steeringwheel"), Wheel));
	TestEqual(TEXT("wheel_lf is the corner front left"), FMantleVehicleConverter::CornerOf(TEXT("wheel_lf")), FString(TEXT("_fl")));
	TestEqual(TEXT("wheel_rf is the corner front right"), FMantleVehicleConverter::CornerOf(TEXT("wheel_rf")), FString(TEXT("_fr")));
	TestEqual(TEXT("wheel_lr is the corner back left"), FMantleVehicleConverter::CornerOf(TEXT("wheel_lr")), FString(TEXT("_bl")));
	TestEqual(TEXT("wheel_rr is still the corner back right"), FMantleVehicleConverter::CornerOf(TEXT("wheel_rr")), FString(TEXT("_br")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleFiveMSteeringWheelTest,
	"Mantle.Vehicle.FiveM.SteeringWheelIsNotAWheel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleFiveMSteeringWheelTest::RunTest(const FString& Parameters)
{
	FMantleVehicleModel Model;
	Model.Name = TEXT("X7");
	for (int32 Corner = 0; Corner < 8; ++Corner)
	{
		Model.LOD.Positions.Add(FVector3f(Corner & 1 ? 225.f : -225.f, Corner & 2 ? 95.f : -95.f, Corner & 4 ? 140.f : 0.f));
	}
	const TArray<TTuple<FName, int32, FVector>> Bones = { { FName("chassis"), INDEX_NONE, FVector::ZeroVector },
		{ FName("wheel_lf"), 0, FVector(135, -80, 35) }, { FName("wheel_rf"), 0, FVector(135, 80, 35) },
		{ FName("wheel_lr"), 0, FVector(-135, -80, 35) }, { FName("wheel_rr"), 0, FVector(-135, 80, 35) },
		{ FName("steeringwheel"), 0, FVector(30, -40, 110) } };
	for (const TTuple<FName, int32, FVector>& Bone : Bones)
	{
		Model.Bones.Add({ Bone.Get<0>(), Bone.Get<1>() });
		Model.BonePositions.Add(Bone.Get<2>());
	}
	Model.bSkeletal = true;
	TArray<FString> Evidence;
	EMantleVehicleTypeConfidence Confidence;
	const EMantleVehicleModelFamily Family = FMantleVehicleTypeMatchCheck::FamilyOfModel(Model, Evidence, Confidence);
	TestTrue(TEXT("a car with a steering wheel is a road vehicle"), Family == EMantleVehicleModelFamily::Road);
	TestTrue(TEXT("with four wheels, the steering wheel not counted"), Evidence.Contains(TEXT("4 wheels")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleFiveMReduceTest,
	"Mantle.Vehicle.FiveM.HeavyBodyIsReducedToFit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleFiveMReduceTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleFiveMTestPrivate;
	if (!FMantleMeshReduce::IsAvailable())
	{
		AddInfo(TEXT("This editor has no static mesh reducer, so the reduction is not run."));
		return true;
	}
	// Two bones, two materials: a big panel (7,200 triangles) on the root and a small one (800) on a wheel bone.
	FMantleSkinnedMesh Body;
	Body.Bones = { { FName(TEXT("root")), INDEX_NONE }, { FName(TEXT("wheel")), 0 } };
	Body.ReferencePose = { FTransform::Identity, FTransform::Identity };
	Body.LODs.AddDefaulted();
	Body.LODs[0].NumInfluences = 1;
	AddGrid(Body, 60, 0, 0, FVector3f::ZeroVector);
	AddGrid(Body, 20, 1, 1, FVector3f(500.f, 0.f, 0.f));
	FMantleVehicleBodyExtras Extras;
	Extras.Colors.Init(FColor::White, Body.LODs[0].Positions.Num());
	const int32 Before = FMantleVehicleBodyReduce::CountTriangles(Body);
	TestEqual(TEXT("the body starts with 8,000 triangles"), Before, 8000);

	FMantleVehicleBodyReduceResult Result;
	TestTrue(TEXT("a body within the limit is left alone"), FMantleVehicleBodyReduce::ReduceToFit(Body, Extras, 9000, Result));
	TestFalse(TEXT("and says it was not reduced"), Result.bReduced);
	TestEqual(TEXT("with its triangles as they were"), FMantleVehicleBodyReduce::CountTriangles(Body), Before);

	const int32 Limit = 2000;
	TestTrue(*FString::Printf(TEXT("a body over the limit is reduced, not refused (%s)"), *Result.Problem), FMantleVehicleBodyReduce::ReduceToFit(Body, Extras, Limit, Result));
	TestTrue(TEXT("and says it was reduced"), Result.bReduced);
	const int32 After = FMantleVehicleBodyReduce::CountTriangles(Body);
	TestTrue(*FString::Printf(TEXT("it fits the limit (%d of %d)"), After, Limit), After > 0 && After <= Limit);
	TestEqual(TEXT("the result says how many it has"), Result.After, After);
	TestTrue(TEXT("its extras are dropped, as the vertices changed"), Extras.IsEmpty());

	const FMantleSkinnedMeshLOD& LOD = Body.LODs[0];
	TestTrue(TEXT("the sections cover every index"), LOD.Sections.Num() == 2 && LOD.Sections[0].FirstIndex == 0 && LOD.Sections[1].FirstIndex == LOD.Sections[0].IndexCount
		&& LOD.Sections[0].IndexCount + LOD.Sections[1].IndexCount == LOD.Indices.Num());
	TestTrue(TEXT("both materials are still drawn"), LOD.Sections.Num() == 2 && LOD.Sections[0].MaterialIndex == 0 && LOD.Sections[1].MaterialIndex == 1);
	TestEqual(TEXT("every vertex has its normal, uv and one whole bone"), LOD.Normals.Num() + LOD.UVs.Num() + LOD.InfluenceBones.Num() + LOD.InfluenceWeights.Num(), LOD.Positions.Num() * 4);
	bool bAllIndicesValid = true;
	for (const uint32 Index : LOD.Indices)
	{
		bAllIndicesValid = bAllIndicesValid && Index < static_cast<uint32>(LOD.Positions.Num());
	}
	TestTrue(TEXT("every index is a vertex"), bAllIndicesValid);

	// Each triangle still rides the bone it did: the panel around the origin the root's, the one at x 500 the wheel's.
	bool bBonesKept = true;
	for (int32 Triangle = 0; Triangle < LOD.NumTriangles(); ++Triangle)
	{
		const int32 Vertex = static_cast<int32>(LOD.Indices[Triangle * 3]);
		const bool bWheelPanel = LOD.Positions[Vertex].X >= 499.f;
		bBonesKept = bBonesKept && LOD.InfluenceBones[Vertex] == (bWheelPanel ? 1 : 0);
	}
	TestTrue(TEXT("each triangle rides the bone it did"), bBonesKept);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleFiveMRebuildTest,
	"Mantle.Vehicle.FiveM.BuildAgainUpdatesTheSameVehicle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleFiveMRebuildTest::RunTest(const FString& Parameters)
{
	FString Error;
	USkeletalMesh* Model = MantleVehicleScratchBody::Make(TEXT("/Temp/MantleFiveMRebuildModel"), TEXT("FiveMRebuildBody"), Error);
	if (!TestNotNull(*FString::Printf(TEXT("the scratch model is made (%s)"), *Error), Model))
	{
		return false;
	}
	// No name asked for, as a script or a new window builds: the same model and type built twice is one vehicle, updated, and not a second set of assets.
	FMantleVehicleModelOptions Options;
	Options.Folder = TEXT("/Temp/MantleFiveMRebuildOut");
	const FMantleVehicleBuildResult First = FMantleVehicleBuilder::BuildFromModel(Model, TEXT("sedan"), Options);
	for (const FString& Problem : First.Problems)
	{
		AddError(Problem);
	}
	if (!TestTrue(TEXT("the first build works"), First.bOk && First.Source != nullptr))
	{
		return false;
	}
	const FMantleVehicleBuildResult Second = FMantleVehicleBuilder::BuildFromModel(Model, TEXT("sedan"), Options);
	for (const FString& Problem : Second.Problems)
	{
		AddError(Problem);
	}
	TestTrue(TEXT("the second build works"), Second.bOk && Second.Source != nullptr);
	TestEqual(TEXT("and has the first one's name"), Second.AssetName, First.AssetName);
	TestTrue(TEXT("and is the first one's vehicle asset"), Second.Source == First.Source);
	TestTrue(TEXT("and says it updated the earlier vehicle"), Second.Notes.ContainsByPredicate([](const FString& Note) { return Note.Contains(TEXT("updated that vehicle")); }));
	MantleVehicleScratchBody::Discard(Model);
	return true;
}

#endif
