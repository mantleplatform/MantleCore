// Copyright Joshua Eger. All Rights Reserved.
#include "MantleSkinnedMesh.h"
#include "MantleVehicleAsset.h"
#include "MantleVehicleConverter.h"
#include "MantleVehicleLightFinder.h"
#include "MantleVehicleSource.h"

#include "MantleGlowMaterial.h"

#include "Dom/JsonObject.h"
#include "Engine/SkinnedAssetCommon.h"
#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// The vehicle builder's lights (vehicle-lights-plan.md): the lights block the document carries (in the shape of mantle's schema/fixtures/vehicle-lights-v1.mvh,
// which the game reads), the finder's sorting of a glowing spot by where it sits and its color, the names it reads as lights, and the cut that moves a light's
// triangles into a section of its own without changing how the body looks.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVehicleLightsTestPrivate
{
	FString Text(const TSharedPtr<FJsonValue>& Value)
	{
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Value, FString(), Writer);
		return Out;
	}

	FString Text(const TSharedRef<FJsonObject>& Doc)
	{
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Doc, Writer);
		return Out;
	}

	UMantleVehicleSource* MakeSource()
	{
		UMantleVehicleSource* Source = NewObject<UMantleVehicleSource>(GetTransientPackage());
		FMantleVehicleSourceWheel& Wheel = Source->Wheels.AddDefaulted_GetRef();
		Wheel.Bone = TEXT("Wheel_FL");
		Wheel.bSteering = true;
		return Source;
	}

	/** A box-shaped body 400 long (x -200..200), 160 wide and 100 high, of quads: one at each corner of the front and of the back, and one on the roof, all one material. */
	void MakeBody(FMantleSkinnedMesh& OutBody, TArray<FSkeletalMaterial>& OutMaterials, const FName SlotName)
	{
		OutBody.Bones = { { FName(TEXT("root")), INDEX_NONE } };
		OutBody.ReferencePose = { FTransform::Identity };
		FMantleSkinnedMeshLOD& LOD = OutBody.LODs.AddDefaulted_GetRef();
		LOD.NumInfluences = 1;
		OutMaterials.Add(FSkeletalMaterial(nullptr, SlotName));
		const FVector3f Centers[] = { { 200.f, -60.f, 50.f }, { 200.f, 60.f, 50.f }, { -200.f, -60.f, 50.f }, { -200.f, 60.f, 50.f }, { 0.f, 0.f, 100.f } };
		for (const FVector3f& Center : Centers)
		{
			const uint32 First = static_cast<uint32>(LOD.Positions.Num());
			for (const FVector3f& Corner : { FVector3f(0.f, -10.f, -10.f), FVector3f(0.f, 10.f, -10.f), FVector3f(0.f, 10.f, 10.f), FVector3f(0.f, -10.f, 10.f) })
			{
				LOD.Positions.Add(Center + Corner);
				LOD.Normals.Add(FVector3f(1.f, 0.f, 0.f));
				LOD.UVs.Add(FVector2f(0.f, 0.f));
				LOD.InfluenceBones.Add(0);
				LOD.InfluenceWeights.Add(255);
			}
			LOD.Indices.Append({ First, First + 1, First + 2, First, First + 2, First + 3 });
		}
		// The box's ends, so the bounds run the whole length.
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

	/** A 4x4 glow texture in Outer, black but for the given cells (x, y) in the given color; named as a pack names its emissive textures. */
	UTexture2D* MakeGlowTexture(UObject* Outer, const TCHAR* Name, const FColor& Color, const TArray<FIntPoint>& Cells)
	{
		TArray<FColor> Pixels;
		Pixels.Init(FColor(0, 0, 0, 255), 16);
		for (const FIntPoint& Cell : Cells)
		{
			Pixels[Cell.Y * 4 + Cell.X] = Color;
		}
		UTexture2D* Texture = NewObject<UTexture2D>(Outer, FName(Name), RF_Transient);
		Texture->SRGB = false;
		Texture->Source.Init(4, 4, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Pixels.GetData()));
		return Texture;
	}

	template <typename TExpression>
	TExpression* AddNode(UMaterial* Material)
	{
		TExpression* Expression = NewObject<TExpression>(Material);
		Expression->Material = Material;
		Material->GetExpressionCollection().AddExpression(Expression);
		return Expression;
	}

	UMaterialExpressionTextureSampleParameter2D* AddSample(UMaterial* Material, const TCHAR* Parameter, UTexture2D* Texture)
	{
		UMaterialExpressionTextureSampleParameter2D* Sample = AddNode<UMaterialExpressionTextureSampleParameter2D>(Material);
		Sample->ParameterName = Parameter;
		Sample->Texture = Texture;
		Sample->SamplerType = SAMPLERTYPE_LinearColor;
		Sample->UpdateParameterGuid(true, false);
		return Sample;
	}

	/** A sample times a Scalar Parameter that ships at 0 (the glow switched off in the model). */
	UMaterialExpressionMultiply* AddSwitched(UMaterial* Material, UMaterialExpressionTextureSampleParameter2D* Sample, const TCHAR* Scalar)
	{
		UMaterialExpressionScalarParameter* Switch = AddNode<UMaterialExpressionScalarParameter>(Material);
		Switch->ParameterName = Scalar;
		Switch->DefaultValue = 0.f;
		Switch->UpdateParameterGuid(true, false);
		UMaterialExpressionMultiply* Multiply = AddNode<UMaterialExpressionMultiply>(Material);
		Multiply->A.Connect(0, Sample);
		Multiply->B.Connect(0, Switch);
		return Multiply;
	}

	/**
	 * A car's light material as the Military Vehicles Pack makes it: a lights texture (white at the front lights and the back left cluster), a stop texture (red at
	 * that cluster) and a reverse texture (white at the back right), the last two each switched by a Scalar Parameter at 0. With bAll false, the lights texture alone.
	 */
	UMaterial* MakeLightsMaterial(const bool bAll)
	{
		UMaterial* Material = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
		UMaterialExpression* Emissive = AddSample(Material, TEXT("Lights"), MakeGlowTexture(Material, TEXT("T_Vehicle_emissive_Light"), FColor::White, { FIntPoint(0, 0), FIntPoint(2, 0) }));
		if (bAll)
		{
			UMaterialExpressionAdd* First = AddNode<UMaterialExpressionAdd>(Material);
			First->A.Connect(0, Emissive);
			First->B.Connect(0, AddSwitched(Material, AddSample(Material, TEXT("Stop"), MakeGlowTexture(Material, TEXT("T_Vehicle_emissive_Brake"), FColor(255, 0, 0, 255), { FIntPoint(2, 0) })), TEXT("StopIntensity")));
			UMaterialExpressionAdd* Second = AddNode<UMaterialExpressionAdd>(Material);
			Second->A.Connect(0, First);
			// A reverse texture with a name that says nothing: its parameter says it.
			Second->B.Connect(0, AddSwitched(Material, AddSample(Material, TEXT("Reverse_Emissive"), MakeGlowTexture(Material, TEXT("T_Mask_03"), FColor::White, { FIntPoint(2, 2) })), TEXT("ReverseIntensity")));
			Emissive = Second;
		}
		Material->GetEditorOnlyData()->EmissiveColor.Connect(0, Emissive);
		Material->UpdateCachedExpressionData();
		return Material;
	}

	/** MakeBody's quads' UVs: both front quads over (0.1, 0.1), the back left over (0.6, 0.1), the back right over (0.6, 0.6) and the roof over a dark spot. */
	void LayUVs(FMantleSkinnedMesh& Body)
	{
		const FVector2f Spots[] = { { 0.1f, 0.1f }, { 0.1f, 0.1f }, { 0.6f, 0.1f }, { 0.6f, 0.6f }, { 0.9f, 0.9f } };
		for (int32 Quad = 0; Quad < 5; ++Quad)
		{
			for (int32 Corner = 0; Corner < 4; ++Corner)
			{
				Body.LODs[0].UVs[Quad * 4 + Corner] = Spots[Quad];
			}
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleLightsDocumentTest,
	"Mantle.Vehicle.Lights.Document",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleLightsDocumentTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleLightsTestPrivate;

	// No lights: no block (the game builds a car with no lights).
	UMantleVehicleSource* Dark = MakeSource();
	TestFalse(TEXT("a vehicle with no lights writes no lights block"), FMantleVehicleConverter::MakeDocument(*Dark, TMap<FString, FString>())->HasField(TEXT("lights")));

	// The fixture's lights (mantle schema/fixtures/vehicle-lights-v1.mvh): every kind written, the empty ones as empty lists, and the beams.
	UMantleVehicleSource* Lit = MakeSource();
	Lit->Lights.Head = { TEXT("M_Body") };
	Lit->Lights.Rear = { TEXT("M_Tire") };
	FMantleVehicleSourceLightBeam Left;
	Left.Position = FVector(150.0, -40.0, 30.0);
	Left.Direction = FVector(1.0, 0.0, -0.05);
	FMantleVehicleSourceLightBeam Right = Left;
	Right.Position.Y = 40.0;
	Lit->Lights.Beams = { Left, Right };
	const TSharedRef<FJsonObject> Doc = FMantleVehicleConverter::MakeDocument(*Lit, TMap<FString, FString>());
	const TSharedPtr<FJsonValue> Block = Doc->TryGetField(TEXT("lights"));
	if (!TestTrue(TEXT("a vehicle with lights writes the block"), Block.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("the block is the fixture's, key for key"), Text(Block),
		FString(TEXT("{\"head\":[\"M_Body\"],\"rear\":[\"M_Tire\"],\"reverse\":[],\"left\":[],\"right\":[],\"beams\":[{\"position\":[150,-40,30],\"direction\":[1,0,-0.05]},{\"position\":[150,40,30],\"direction\":[1,0,-0.05]}]}")));

	// The game's reader takes it as written.
	FMantleVehicleDesc Desc;
	FString Error;
	const FString Json = Text(Doc);
	const FTCHARToUTF8 Utf8(*Json);
	if (TestTrue(*FString::Printf(TEXT("the game reads the document (%s)"), *Error),
		FMantleVehicleAssetReader::ParseDocument(TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()), Desc, Error)))
	{
		TestTrue(TEXT("it has lights"), Desc.bLights);
		TestEqual(TEXT("its headlights"), Desc.LightHead, TArray<FString>{ TEXT("M_Body") });
		TestEqual(TEXT("its rear lights"), Desc.LightRear, TArray<FString>{ TEXT("M_Tire") });
		TestEqual(TEXT("two beams"), Desc.LightBeams.Num(), 2);
		if (Desc.LightBeams.Num() == 2)
		{
			TestEqual(TEXT("the right beam's place"), Desc.LightBeams[1].Position, FVector(150.0, 40.0, 30.0));
		}
	}

	// A vehicle at Size 2 has its beams twice as far out, as its seat is.
	Lit->FitScale = 2.f;
	const TSharedRef<FJsonObject> Sized = FMantleVehicleConverter::MakeDocument(*Lit, TMap<FString, FString>());
	const TSharedPtr<FJsonObject>* SizedLights = nullptr;
	if (TestTrue(TEXT("the sized document has lights"), Sized->TryGetObjectField(TEXT("lights"), SizedLights)))
	{
		TestEqual(TEXT("the sized beams move out, their aim kept"), Text((*SizedLights)->TryGetField(TEXT("beams"))),
			FString(TEXT("[{\"position\":[300,-80,60],\"direction\":[1,0,-0.05]},{\"position\":[300,80,60],\"direction\":[1,0,-0.05]}]")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleLightsClassifyTest,
	"Mantle.Vehicle.Lights.Classify",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleLightsClassifyTest::RunTest(const FString& Parameters)
{
	const FBox3f Car(FVector3f(-200.f, -80.f, 0.f), FVector3f(200.f, 80.f, 100.f));
	const FLinearColor White(1.f, 1.f, 0.95f);
	const FLinearColor WarmWhite(1.f, 0.9f, 0.6f);
	const FLinearColor Red(0.9f, 0.05f, 0.05f);
	const FLinearColor Amber(1.f, 0.5f, 0.05f);
	const FLinearColor Blue(0.1f, 0.3f, 1.f);
	const auto Is = [this, &Car](const TCHAR* What, const FVector3f& At, const FLinearColor& Glow, const TOptional<EMantleLightKind> Expected)
	{
		const TOptional<EMantleLightKind> Got = FMantleVehicleLights::Classify(At, Glow, Car);
		TestTrue(What, Got == Expected);
	};
	Is(TEXT("white at the front is a headlight"), FVector3f(190.f, -50.f, 50.f), White, EMantleLightKind::Head);
	Is(TEXT("warm white at the front is a headlight"), FVector3f(190.f, 50.f, 50.f), WarmWhite, EMantleLightKind::Head);
	Is(TEXT("red at the back is a rear light"), FVector3f(-195.f, 50.f, 60.f), Red, EMantleLightKind::Rear);
	Is(TEXT("white at the back is a reverse light"), FVector3f(-195.f, 30.f, 60.f), White, EMantleLightKind::Reverse);
	Is(TEXT("amber on the left (-Y) is the left signal"), FVector3f(180.f, -70.f, 50.f), Amber, EMantleLightKind::Left);
	Is(TEXT("amber on the right is the right signal"), FVector3f(-180.f, 70.f, 50.f), Amber, EMantleLightKind::Right);
	Is(TEXT("amber on a mirror, mid-car, is still a signal"), FVector3f(40.f, 75.f, 70.f), Amber, EMantleLightKind::Right);
	Is(TEXT("white in the middle (a dashboard) is no light"), FVector3f(20.f, 0.f, 60.f), White, TOptional<EMantleLightKind>());
	Is(TEXT("red at the front is no light"), FVector3f(190.f, 0.f, 50.f), Red, TOptional<EMantleLightKind>());
	Is(TEXT("blue is no light"), FVector3f(190.f, 0.f, 50.f), Blue, TOptional<EMantleLightKind>());
	Is(TEXT("black is no light"), FVector3f(190.f, 0.f, 50.f), FLinearColor::Black, TOptional<EMantleLightKind>());

	const auto Named = [this, &Car](const TCHAR* Name, const FVector3f& At, const TOptional<EMantleLightKind> Expected)
	{
		TestTrue(*FString::Printf(TEXT("'%s' reads as expected"), Name), FMantleVehicleLights::FromName(Name, At, Car) == Expected);
	};
	const FVector3f Front(190.f, -50.f, 50.f);
	const FVector3f Back(-190.f, 50.f, 50.f);
	Named(TEXT("M_Headlight"), Back, EMantleLightKind::Head);
	Named(TEXT("HeadLight_Glass"), Back, EMantleLightKind::Head);
	Named(TEXT("MI_TailLights"), Front, EMantleLightKind::Rear);
	Named(TEXT("Brake_Light"), Front, EMantleLightKind::Rear);
	Named(TEXT("reverse_light"), Front, EMantleLightKind::Reverse);
	Named(TEXT("Indicator"), Front, EMantleLightKind::Left);
	Named(TEXT("Blinker"), Back, EMantleLightKind::Right);
	Named(TEXT("Lamp"), Front, EMantleLightKind::Head);
	Named(TEXT("Light"), Back, EMantleLightKind::Rear);
	Named(TEXT("P38_Lightning"), Front, TOptional<EMantleLightKind>());
	Named(TEXT("M_Body"), Front, TOptional<EMantleLightKind>());
	Named(TEXT("Brake"), Back, TOptional<EMantleLightKind>());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleLightsCutTest,
	"Mantle.Vehicle.Lights.Cut",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleLightsCutTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleLightsTestPrivate;
	FMantleSkinnedMesh Body;
	TArray<FSkeletalMaterial> Materials;
	MakeBody(Body, Materials, TEXT("M_Body"));
	const FMantleSkinnedMeshLOD Before = Body.LODs[0];

	// The two front quads are headlights (triangles 0-3), the back left the left signal (4, 5); the back right and the roof stay body.
	FMantleVehicleLightGuess Guess;
	Guess.Triangles[static_cast<int32>(EMantleLightKind::Head)] = { 0, 1, 2, 3 };
	Guess.Triangles[static_cast<int32>(EMantleLightKind::Left)] = { 4, 5 };
	FMantleVehicleSourceLightBeam Beam;
	Beam.Position = FVector(200.0, -60.0, 50.0);
	Guess.Beams = { Beam };
	FMantleVehicleSourceLights Lights;
	FMantleVehicleLights::Cut(Body, Materials, Guess, Lights);

	const FMantleSkinnedMeshLOD& After = Body.LODs[0];
	FString Why;
	TestTrue(*FString::Printf(TEXT("the cut body is valid (%s)"), *Why), Body.Validate(Why));
	TestEqual(TEXT("no vertex is added or moved"), After.Positions.Num(), Before.Positions.Num());
	TestEqual(TEXT("every triangle is kept"), After.Indices.Num(), Before.Indices.Num());
	TestEqual(TEXT("two new slots"), Materials.Num(), 3);
	TestEqual(TEXT("three sections"), After.Sections.Num(), 3);
	TestEqual(TEXT("the body keeps the other four triangles"), After.Sections.Num() > 0 ? After.Sections[0].IndexCount : 0, 12);
	TestEqual(TEXT("the headlights are named for them"), Lights.Head, TArray<FString>{ TEXT("Headlights") });
	TestEqual(TEXT("the left signal too"), Lights.Left, TArray<FString>{ TEXT("LeftSignal") });
	TestTrue(TEXT("the rear, reverse and right lights are empty"), Lights.Rear.IsEmpty() && Lights.Reverse.IsEmpty() && Lights.Right.IsEmpty());
	TestEqual(TEXT("the beams ride along"), Lights.Beams.Num(), 1);
	TestEqual(TEXT("a new slot keeps the body's material"), Materials.Num() > 1 ? Materials[1].MaterialInterface.Get() : nullptr, Materials[0].MaterialInterface.Get());

	// The same triangles, in the same places, whatever section they are in now.
	TMap<FString, int32> Was;
	TMap<FString, int32> Is;
	const auto Count = [](const FMantleSkinnedMeshLOD& LOD, TMap<FString, int32>& Out)
	{
		for (int32 At = 0; At + 2 < LOD.Indices.Num(); At += 3)
		{
			TArray<FString> Corners;
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const FVector3f& P = LOD.Positions[LOD.Indices[At + Corner]];
				Corners.Add(FString::Printf(TEXT("%d,%d,%d"), FMath::RoundToInt(P.X), FMath::RoundToInt(P.Y), FMath::RoundToInt(P.Z)));
			}
			Corners.Sort();
			Out.FindOrAdd(FString::Join(Corners, TEXT("|")))++;
		}
	};
	Count(Before, Was);
	Count(After, Is);
	TestTrue(TEXT("the body looks as it did"), Was.OrderIndependentCompareEqual(Is));

	// A slot that is all one light is not cut: it is that light as it is.
	FMantleSkinnedMesh Lamp;
	TArray<FSkeletalMaterial> LampMaterials;
	MakeBody(Lamp, LampMaterials, TEXT("TailLights"));
	FMantleVehicleLightGuess Whole;
	Whole.WholeSlots[static_cast<int32>(EMantleLightKind::Rear)] = { 0 };
	FMantleVehicleSourceLights LampLights;
	FMantleVehicleLights::Cut(Lamp, LampMaterials, Whole, LampLights);
	TestEqual(TEXT("a whole slot keeps its one section"), Lamp.LODs[0].Sections.Num(), 1);
	TestEqual(TEXT("and is the rear light by its own name"), LampLights.Rear, TArray<FString>{ TEXT("TailLights") });

	// The finder reads a slot named for a light without any glow: the named slot here is a rear light, as one.
	const FMantleVehicleLightGuess Found = FMantleVehicleLights::Find(Lamp, LampMaterials, false);
	TestEqual(TEXT("the finder takes the named slot whole"), Found.WholeSlots[static_cast<int32>(EMantleLightKind::Rear)].Num(), 1);
	TestEqual(TEXT("with no headlights, two beams at the front corners"), Found.Beams.Num(), 2);
	if (Found.Beams.Num() == 2)
	{
		TestTrue(TEXT("the first beam is on the left (-Y), at the front"), Found.Beams[0].Position.Y < 0.0 && FMath::IsNearlyEqual(Found.Beams[0].Position.X, 200.0));
	}
	TestEqual(TEXT("a bike gets one beam"), FMantleVehicleLights::Find(Lamp, LampMaterials, true).Beams.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleLightsGlowTexturesTest,
	"Mantle.Vehicle.Lights.GlowTextures",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleLightsGlowTexturesTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleLightsTestPrivate;

	// What a glow texture's name says.
	const auto Says = [this](const TCHAR* Name, const TOptional<EMantleLightKind> Expected)
	{
		TestTrue(*FString::Printf(TEXT("'%s' reads as expected"), Name), FMantleVehicleLights::FromGlowName(Name) == Expected);
	};
	Says(TEXT("T_Vehicle_emissive_Brake"), EMantleLightKind::Rear);
	Says(TEXT("vehicle_lights_emissive_stop"), EMantleLightKind::Rear);
	Says(TEXT("TailLights_Emissive"), EMantleLightKind::Rear);
	Says(TEXT("vehicle_lights_emissive_reverse"), EMantleLightKind::Reverse);
	Says(TEXT("T_lights_emissive_Reverse"), EMantleLightKind::Reverse);
	Says(TEXT("T_Turn_Signal"), EMantleLightKind::Left);
	Says(TEXT("T_lights_emissive_Lights"), TOptional<EMantleLightKind>());
	Says(TEXT("vehicle_body_emissive_lights"), TOptional<EMantleLightKind>());
	Says(TEXT("T_Mask_03"), TOptional<EMantleLightKind>());

	// Read lists every texture the emissive graph reads, with the parameter it comes from; one texture reads as before.
	UMaterial* Several = MakeLightsMaterial(true);
	FMantleMaterialGlow Glow;
	TestTrue(TEXT("the material glows"), FMantleMaterialGlow::Read(Several, Glow));
	if (!TestEqual(TEXT("three glow textures"), Glow.Textures.Num(), 3))
	{
		return false;
	}
	TestEqual(TEXT("the first is the lights texture"), Glow.Textures[0].Texture->GetName(), FString(TEXT("T_Vehicle_emissive_Light")));
	TestTrue(TEXT("and Texture is still the first"), Glow.Texture == Glow.Textures[0].Texture);
	TestEqual(TEXT("the stop texture's parameter"), Glow.Textures[1].Parameter.ToString(), FString(TEXT("Stop")));
	TestTrue(TEXT("the stop switch belongs to the stop texture"), Glow.Textures[1].OffScalars.Num() == 1 && Glow.Textures[1].OffScalars[0] == FName(TEXT("StopIntensity")));
	TestTrue(TEXT("the reverse switch belongs to the reverse texture"), Glow.Textures[2].OffScalars.Num() == 1 && Glow.Textures[2].OffScalars[0] == FName(TEXT("ReverseIntensity")));
	TestTrue(TEXT("the lights texture has no switch"), Glow.Textures[0].OffScalars.IsEmpty());
	TestEqual(TEXT("both switches are still listed off"), Glow.OffScalars.Num(), 2);
	UMaterial* Single = MakeLightsMaterial(false);
	TestTrue(TEXT("a one-texture material glows"), FMantleMaterialGlow::Read(Single, Glow));
	TestEqual(TEXT("with one glow texture"), Glow.Textures.Num(), 1);
	UTexture* SingleTexture = Glow.Texture;
	TestTrue(TEXT("which is Texture"), Glow.Textures.Num() == 1 && Glow.Textures[0].Texture == Glow.Texture);

	// The finder: the lights texture lights both front quads (head) and the back left cluster (which alone would read as a reverse light); the stop texture lights
	// that cluster too and names it rear, which wins; the reverse texture (named by its parameter) lights the back right.
	FMantleSkinnedMesh Body;
	TArray<FSkeletalMaterial> Materials;
	MakeBody(Body, Materials, TEXT("M_Lights"));
	LayUVs(Body);
	Materials[0].MaterialInterface = Several;
	const FMantleVehicleLightGuess Guess = FMantleVehicleLights::Find(Body, Materials, false);
	const auto Triangles = [&Guess](const EMantleLightKind Each) { return Guess.Triangles[static_cast<int32>(Each)]; };
	TestEqual(TEXT("the front quads are headlights"), Triangles(EMantleLightKind::Head), TArray<int32>({ 0, 1, 2, 3 }));
	TestEqual(TEXT("the back left, lit by lights and stop, is rear (the named texture wins)"), Triangles(EMantleLightKind::Rear), TArray<int32>({ 4, 5 }));
	TestEqual(TEXT("the back right is reverse"), Triangles(EMantleLightKind::Reverse), TArray<int32>({ 6, 7 }));
	TestTrue(TEXT("the roof and the signals are nothing"), Triangles(EMantleLightKind::Left).IsEmpty() && Triangles(EMantleLightKind::Right).IsEmpty());
	TestEqual(TEXT("the same material finds the same lights again"), FMantleVehicleLights::Find(Body, Materials, false).Triangles[static_cast<int32>(EMantleLightKind::Rear)],
		TArray<int32>({ 4, 5 }));

	// A one-texture material is read as before: color and position alone (the back left cluster is white there, so a reverse light).
	Materials[0].MaterialInterface = Single;
	const FMantleVehicleLightGuess Plain = FMantleVehicleLights::Find(Body, Materials, false);
	TestEqual(TEXT("one texture: headlights as before"), Plain.Triangles[static_cast<int32>(EMantleLightKind::Head)], TArray<int32>({ 0, 1, 2, 3 }));
	TestEqual(TEXT("one texture: the white back cluster is reverse as before"), Plain.Triangles[static_cast<int32>(EMantleLightKind::Reverse)], TArray<int32>({ 4, 5 }));
	TestTrue(TEXT("one texture: no rear light"), Plain.Triangles[static_cast<int32>(EMantleLightKind::Rear)].IsEmpty());

	// The copy keeps the texture that matches its kind: the reverse light drops the stop texture (and its switch) and keeps its own.
	FString Error;
	const TCHAR* Folder = TEXT("/Temp/MantleLightsTest");
	FMantleMaterialGlow Original;
	FMantleMaterialGlow::Read(Several, Original);
	const auto TextureOf = [](UMaterialInterface* Material, const TCHAR* Parameter) -> UTexture*
	{
		UTexture* Texture = nullptr;
		Material->GetTextureParameterValue(FHashedMaterialParameterInfo(FMaterialParameterInfo(Parameter)), Texture);
		return Texture;
	};
	const auto ScalarOf = [](UMaterialInterface* Material, const TCHAR* Parameter) -> float
	{
		float Value = -1.f;
		Material->GetScalarParameterValue(FHashedMaterialParameterInfo(FMaterialParameterInfo(Parameter)), Value);
		return Value;
	};
	UMaterialInterface* Reverse = FMantleVehicleLights::MakeLightMaterial(Several, EMantleLightKind::Reverse, Folder, TEXT("MI_Reverse"), Error);
	if (TestNotNull(*FString::Printf(TEXT("the reverse copy is made (%s)"), *Error), Reverse))
	{
		TestTrue(TEXT("the reverse copy keeps the reverse texture"), TextureOf(Reverse, TEXT("Reverse_Emissive")) == Original.Textures[2].Texture);
		TestTrue(TEXT("and the generic lights texture"), TextureOf(Reverse, TEXT("Lights")) == Original.Textures[0].Texture);
		TestTrue(TEXT("but not the stop texture"), TextureOf(Reverse, TEXT("Stop")) != Original.Textures[1].Texture);
		TestEqual(TEXT("its own switch is on"), ScalarOf(Reverse, TEXT("ReverseIntensity")), 1.f);
		TestEqual(TEXT("the stop switch stays off"), ScalarOf(Reverse, TEXT("StopIntensity")), 0.f);
	}
	UMaterialInterface* Rear = FMantleVehicleLights::MakeLightMaterial(Several, EMantleLightKind::Rear, Folder, TEXT("MI_Rear"), Error);
	if (TestNotNull(TEXT("the rear copy is made"), Rear))
	{
		TestTrue(TEXT("the rear copy keeps the stop texture"), TextureOf(Rear, TEXT("Stop")) == Original.Textures[1].Texture);
		TestTrue(TEXT("and drops the reverse texture"), TextureOf(Rear, TEXT("Reverse_Emissive")) != Original.Textures[2].Texture);
		TestEqual(TEXT("the rear copy turns the stop switch on"), ScalarOf(Rear, TEXT("StopIntensity")), 1.f);
		TestEqual(TEXT("and leaves the reverse switch off"), ScalarOf(Rear, TEXT("ReverseIntensity")), 0.f);
	}

	// A one-texture material's copy is what it was: no texture changed.
	UMaterialInterface* Head = FMantleVehicleLights::MakeLightMaterial(Single, EMantleLightKind::Head, Folder, TEXT("MI_Head"), Error);
	if (TestNotNull(TEXT("a one-texture copy is made"), Head))
	{
		TestTrue(TEXT("it keeps its one texture"), TextureOf(Head, TEXT("Lights")) == SingleTexture);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
