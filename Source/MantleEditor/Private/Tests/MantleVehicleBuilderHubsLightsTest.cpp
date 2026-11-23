// Copyright Joshua Eger. All Rights Reserved.
#include "MantleSkinnedMesh.h"
#include "MantleVehicleAsset.h"
#include "MantleVehicleBuilder.h"
#include "MantleVehicleBuiltInMaterials.h"
#include "MantleVehicleChassisBuilder.h"
#include "MantleVehicleChassisSkin.h"
#include "MantleVehicleConverter.h"
#include "MantleVehicleLightFinder.h"
#include "MantleVehicleSource.h"

#include "Engine/SkeletalMesh.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// What the vehicle builder does for a car built on a chassis from a model (Mantle.Vehicle.BuildModel, the route the FiveM cars take): the hub and brake bones of the model follow
// their wheel's steering, the lamps named headlight_lens, taillight_lens and reverselight_lens are found as head, rear and reverse lights, a red or amber lens keeps its own
// material instead of the shared clear one, and a paint layer can start with a finish (secondaryfinish=chrome). The models here are boxes made in the test.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVehicleBuilderHubsLightsTestPrivate
{
	/** A box of six faces on model bone Bone, in section Section. */
	void AddBox(FMantleVehicleModel& Model, const FVector3f& Center, const FVector3f& Half, const int32 Bone, const int32 Section)
	{
		FMantleSkinnedMeshLOD& LOD = Model.LOD;
		if (LOD.Sections.Num() <= Section)
		{
			LOD.Sections.SetNum(Section + 1);
		}
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			for (const float Sign : { -1.0f, 1.0f })
			{
				FVector3f Normal(0.0f);
				Normal[Axis] = Sign;
				FVector3f U(0.0f);
				U[(Axis + 1) % 3] = 1.0f;
				FVector3f V(0.0f);
				V[(Axis + 2) % 3] = 1.0f;
				const uint32 First = static_cast<uint32>(LOD.Positions.Num());
				const FVector3f FaceCenter = Center + Normal * Half[Axis];
				const float Du = Half[(Axis + 1) % 3];
				const float Dv = Half[(Axis + 2) % 3];
				const FVector3f Corners[4] = { FaceCenter - U * Du - V * Dv, FaceCenter + U * Du - V * Dv, FaceCenter + U * Du + V * Dv, FaceCenter - U * Du + V * Dv };
				for (int32 Corner = 0; Corner < 4; ++Corner)
				{
					LOD.Positions.Add(Corners[Corner]);
					LOD.Normals.Add(Normal);
					LOD.UVs.Add(FVector2f(0, 0));
					LOD.InfluenceBones.Add(static_cast<uint16>(Bone));
					LOD.InfluenceWeights.Add(255);
				}
				for (const uint32 Index : { 0u, 1u, 2u, 0u, 2u, 3u })
				{
					LOD.Indices.Add(First + Index);
				}
			}
		}
	}

	FVector ComponentLocation(const FMantleSkinnedMesh& Body, const int32 Bone)
	{
		FTransform Pose = Body.ReferencePose[Bone];
		for (int32 Parent = Body.Bones[Bone].ParentIndex; Parent != INDEX_NONE; Parent = Body.Bones[Parent].ParentIndex)
		{
			Pose = Pose * Body.ReferencePose[Parent];
		}
		return Pose.GetLocation();
	}

	int32 BoneIndex(const FMantleSkinnedMesh& Body, const TCHAR* Name)
	{
		return Body.Bones.IndexOfByPredicate([Name](const FMantleSkinnedMeshBone& Bone) { return Bone.Name == FName(Name); });
	}

	int32 VerticesOn(const FMantleSkinnedMesh& Body, const int32 Bone)
	{
		int32 Count = 0;
		for (const uint16 Influence : Body.LODs[0].InfluenceBones)
		{
			Count += Influence == Bone ? 1 : 0;
		}
		return Count;
	}

	/**
	 * A car as a model with its own skeleton: a body box on the root, a wheel bone and box at each corner of the sedan chassis's wheels, and at each a hub bone (named as
	 * HubNames, in the corner order front left, front right, rear left, rear right) with a caliper box skinned to it, Offset cm from the wheel's center.
	 */
	FMantleVehicleModel CarWithHubs(const FMantleSkinnedMesh& Chassis, const TCHAR* const (&HubNames)[4], const FVector& Offset)
	{
		static const TCHAR* const ChassisWheels[4] = { TEXT("wheel_fl"), TEXT("wheel_fr"), TEXT("wheel_rl"), TEXT("wheel_rr") };
		static const TCHAR* const WheelNames[4] = { TEXT("wheel_lf"), TEXT("wheel_rf"), TEXT("wheel_lr"), TEXT("wheel_rr_model") };
		FMantleVehicleModel Model;
		Model.Name = TEXT("HubCar");
		Model.bSkeletal = true;
		Model.Bones.Add({ FName(TEXT("body")), INDEX_NONE });
		Model.BonePositions.Add(FVector::ZeroVector);
		FVector Centers[4];
		for (int32 Corner = 0; Corner < 4; ++Corner)
		{
			Centers[Corner] = ComponentLocation(Chassis, BoneIndex(Chassis, ChassisWheels[Corner]));
			Model.Bones.Add({ FName(WheelNames[Corner]), 0 });
			Model.BonePositions.Add(Centers[Corner]);
		}
		for (int32 Corner = 0; Corner < 4; ++Corner)
		{
			Model.Bones.Add({ FName(HubNames[Corner]), 0 });
			Model.BonePositions.Add(Centers[Corner] + Offset);
		}
		Model.Materials.Add(FSkeletalMaterial(static_cast<UMaterialInterface*>(nullptr), FName(TEXT("Skin"))));
		// The body, then each wheel on its own bone, then each caliper on its hub's.
		AddBox(Model, FVector3f(0.f, 0.f, 60.f), FVector3f(230.f, 85.f, 25.f), 0, 0);
		for (int32 Corner = 0; Corner < 4; ++Corner)
		{
			AddBox(Model, FVector3f(Centers[Corner]), FVector3f(12.f, 10.f, 32.f), 1 + Corner, 0);
		}
		for (int32 Corner = 0; Corner < 4; ++Corner)
		{
			AddBox(Model, FVector3f(Centers[Corner] + Offset), FVector3f(6.f, 6.f, 14.f), 5 + Corner, 0);
		}
		Model.LOD.Sections.Reset();
		FMantleSkinnedMeshSection& Whole = Model.LOD.Sections.AddDefaulted_GetRef();
		Whole.MaterialIndex = 0;
		Whole.FirstIndex = 0;
		Whole.IndexCount = Model.LOD.Indices.Num();
		Whole.MaterialName = FName(TEXT("Skin"));
		Model.LOD.NumInfluences = 1;
		return Model;
	}

	FMantleVehicleModelMap WheelMap()
	{
		FMantleVehicleModelMap Map;
		Map.Parts.Add(FName(TEXT("wheel_fl")), FMantleVehicleModelPiece::OfBone(FName(TEXT("wheel_lf"))));
		Map.Parts.Add(FName(TEXT("wheel_fr")), FMantleVehicleModelPiece::OfBone(FName(TEXT("wheel_rf"))));
		Map.Parts.Add(FName(TEXT("wheel_rl")), FMantleVehicleModelPiece::OfBone(FName(TEXT("wheel_lr"))));
		Map.Parts.Add(FName(TEXT("wheel_rr")), FMantleVehicleModelPiece::OfBone(FName(TEXT("wheel_rr_model"))));
		return Map;
	}

	UMaterial* MaterialColored(const FLinearColor& Color)
	{
		UMaterial* Material = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
		UMaterialExpressionVectorParameter* Factor = NewObject<UMaterialExpressionVectorParameter>(Material);
		Factor->Material = Material;
		Factor->ParameterName = TEXT("BaseColorFactor");
		Factor->DefaultValue = Color;
		Factor->UpdateParameterGuid(true, false);
		Material->GetExpressionCollection().AddExpression(Factor);
		Material->UpdateCachedExpressionData();
		return Material;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleBuilderHubsTest,
	"Mantle.Vehicle.BuildModel.HubBonesFollowTheirWheel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleBuilderHubsTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleBuilderHubsLightsTestPrivate;
	FMantleChassisSpec Spec;
	FMantleVehicleAsset Gray;
	FString Error;
	if (!TestTrue(*FString::Printf(TEXT("the sedan chassis loads (%s)"), *Error), FMantleVehicleChassisBuilder::LoadSpec(TEXT("sedan"), Spec, Error) && FMantleVehicleChassisBuilder::MakeAsset(Spec, Gray, Error)))
	{
		return false;
	}
	const TArray<FMantleVehiclePartSlot> Slots = FMantleVehicleChassisSkin::PartSlots(Spec, Gray.Body);

	// Four hub bones, as the FiveM rebuild names them (hub_lf, hub_rf, hub_lr, hub_rr), each at its wheel's center with a caliper skinned to it.
	static const TCHAR* const Hubs[4] = { TEXT("hub_lf"), TEXT("hub_rf"), TEXT("hub_lr"), TEXT("hub_rr") };
	const FMantleVehicleModel Model = CarWithHubs(Gray.Body, Hubs, FVector::ZeroVector);
	const FMantleVehicleModelMap Map = WheelMap();
	const FMantleVehicleModelPlacement Placement = FMantleVehicleChassisSkin::FitPlacement(Spec, Gray.Body, Model, 0.0, Map);
	FMantleVehicleSkinned Skinned;
	if (!TestTrue(*FString::Printf(TEXT("the model skins (%s)"), *Error), FMantleVehicleChassisSkin::Skin(Gray.Body, Slots, Model, Placement, Map, Skinned, Error, 100000.0)))
	{
		return false;
	}
	TestEqual(TEXT("each of the four hubs is found"), Skinned.Hubs.Num(), 4);
	static const TCHAR* const ChassisWheels[4] = { TEXT("wheel_fl"), TEXT("wheel_fr"), TEXT("wheel_rl"), TEXT("wheel_rr") };
	for (int32 Corner = 0; Corner < 4; ++Corner)
	{
		const FMantleVehicleSkinnedHub* Hub = Skinned.Hubs.FindByPredicate([Corner](const FMantleVehicleSkinnedHub& Each) { return Each.Bone == FName(Hubs[Corner]); });
		if (!TestNotNull(*FString::Printf(TEXT("%s is a hub"), Hubs[Corner]), Hub))
		{
			continue;
		}
		TestTrue(*FString::Printf(TEXT("%s follows the chassis's %s"), Hubs[Corner], ChassisWheels[Corner]), Hub->Wheel == FName(ChassisWheels[Corner]));
		const int32 HubBone = BoneIndex(Skinned.Body, Hubs[Corner]);
		const int32 WheelBone = BoneIndex(Skinned.Body, ChassisWheels[Corner]);
		if (TestTrue(*FString::Printf(TEXT("%s has a bone of its own in the body"), Hubs[Corner]), HubBone > 0 && WheelBone > 0))
		{
			TestEqual(TEXT("under the root"), Skinned.Body.Bones[HubBone].ParentIndex, 0);
			TestTrue(*FString::Printf(TEXT("at the wheel's center"), Hubs[Corner]), ComponentLocation(Skinned.Body, HubBone).Equals(ComponentLocation(Skinned.Body, WheelBone), 0.5));
			TestEqual(TEXT("with the caliper's vertices on it (a box of six faces)"), VerticesOn(Skinned.Body, HubBone), 24);
			TestEqual(TEXT("and the wheel's own box still on the wheel"), VerticesOn(Skinned.Body, WheelBone), 24);
		}
	}

	// The builder turns them into linkage rules: each copies its wheel's steering and travel, not its spin; a second build replaces them.
	UMantleVehicleSource* Source = NewObject<UMantleVehicleSource>(GetTransientPackage(), NAME_None, RF_Transient);
	FMantleVehicleSourceLinkage Own;
	Own.Type = EMantleVehicleLinkageType::Aim;
	Own.Bone = FName(TEXT("creator_arm"));
	Own.Target = FName(TEXT("wheel_fl"));
	Source->Linkage.Add(Own);
	TArray<FString> Notes;
	FMantleVehicleBuilder::ApplyHubs(*Source, Skinned.Hubs, Notes);
	FMantleVehicleBuilder::ApplyHubs(*Source, Skinned.Hubs, Notes);
	TestEqual(TEXT("one rule for each hub, and the creator's own rule kept"), Source->Linkage.Num(), 5);
	for (int32 Corner = 0; Corner < 4; ++Corner)
	{
		const FMantleVehicleSourceLinkage* Rule = Source->Linkage.FindByPredicate([Corner](const FMantleVehicleSourceLinkage& Each) { return Each.Bone == FName(Hubs[Corner]); });
		TestTrue(*FString::Printf(TEXT("%s copies its wheel about Z (steering, not spin)"), Hubs[Corner]),
			Rule && Rule->Type == EMantleVehicleLinkageType::Copy && Rule->Source == FName(ChassisWheels[Corner]) && Rule->Axis == EMantleVehicleLinkageAxis::Z);
	}
	TestTrue(TEXT("and the notes say which follow which"), Notes.ContainsByPredicate([](const FString& Note) { return Note.StartsWith(TEXT("Hubs and brakes: ")) && Note.Contains(TEXT("hub_lf with wheel_fl")); }));

	// A brake bone is a hub too (the SUV's B_FLBrake, the caliper skinned to it), where it sits at the wheel.
	static const TCHAR* const Brakes[4] = { TEXT("B_Car_FLBrake"), TEXT("B_Car_FRBrake"), TEXT("B_Car_RLBrake"), TEXT("B_Car_RRCaliper") };
	const FMantleVehicleModel BrakeCar = CarWithHubs(Gray.Body, Brakes, FVector(0.0, 0.0, 3.0));
	FMantleVehicleSkinned BrakeSkinned;
	if (TestTrue(*FString::Printf(TEXT("the brake car skins (%s)"), *Error), FMantleVehicleChassisSkin::Skin(Gray.Body, Slots, BrakeCar, FMantleVehicleChassisSkin::FitPlacement(Spec, Gray.Body, BrakeCar, 0.0, Map), Map, BrakeSkinned, Error, 100000.0)))
	{
		TestEqual(TEXT("each brake or caliper bone is a hub"), BrakeSkinned.Hubs.Num(), 4);
	}

	// Negative controls: a model with no hub bones has none; one whose "hub" sits far from every wheel (a hub cap lying across the car) is left on the body, not made a hub.
	static const TCHAR* const NotHubs[4] = { TEXT("door_l"), TEXT("door_r"), TEXT("hood"), TEXT("trunk") };
	FMantleVehicleSkinned Plain;
	TestTrue(TEXT("a model with no hub bones skins"), FMantleVehicleChassisSkin::Skin(Gray.Body, Slots, CarWithHubs(Gray.Body, NotHubs, FVector::ZeroVector), Placement, Map, Plain, Error, 100000.0));
	TestEqual(TEXT("and has no hubs"), Plain.Hubs.Num(), 0);
	FMantleVehicleSkinned Far;
	TestTrue(TEXT("a model whose hub bones sit far from the wheels skins"), FMantleVehicleChassisSkin::Skin(Gray.Body, Slots, CarWithHubs(Gray.Body, Hubs, FVector(0.0, 0.0, 300.0)), Placement, Map, Far, Error, 100000.0));
	TestEqual(TEXT("and those are not made hubs"), Far.Hubs.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleBuilderLampNamesTest,
	"Mantle.Vehicle.BuildModel.LampLensesAreFoundAsLights",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleBuilderLampNamesTest::RunTest(const FString& Parameters)
{
	// The FiveM rebuild names its lamp materials headlight_lens, taillight_lens and reverselight_lens: by their names alone they are the head, rear and reverse lights.
	const FBox3f Bounds(FVector3f(-230.f, -90.f, 0.f), FVector3f(230.f, 90.f, 150.f));
	const FVector3f Middle(0.f, 0.f, 60.f);
	const auto Is = [&](const TCHAR* Name, const EMantleLightKind Kind) { const TOptional<EMantleLightKind> Found = FMantleVehicleLights::FromName(Name, Middle, Bounds); return Found.IsSet() && Found.GetValue() == Kind; };
	TestTrue(TEXT("headlight_lens is the headlights"), Is(TEXT("headlight_lens"), EMantleLightKind::Head));
	TestTrue(TEXT("taillight_lens is the rear lights"), Is(TEXT("taillight_lens"), EMantleLightKind::Rear));
	TestTrue(TEXT("reverselight_lens is the reverse lights"), Is(TEXT("reverselight_lens"), EMantleLightKind::Reverse));

	// And the finder takes the whole section of a model for each, wherever it sits.
	FMantleSkinnedMesh Body;
	Body.Bones.Add({ FName(TEXT("root")), INDEX_NONE });
	Body.ReferencePose.Add(FTransform::Identity);
	FMantleSkinnedMeshLOD& LOD = Body.LODs.AddDefaulted_GetRef();
	LOD.NumInfluences = 1;
	TArray<FSkeletalMaterial> Materials;
	const TCHAR* const Names[4] = { TEXT("body_paint"), TEXT("headlight_lens"), TEXT("taillight_lens"), TEXT("reverselight_lens") };
	const float Xs[4] = { 0.f, 220.f, -220.f, -220.f };
	for (int32 Slot = 0; Slot < 4; ++Slot)
	{
		Materials.Add(FSkeletalMaterial(static_cast<UMaterialInterface*>(nullptr), FName(Names[Slot])));
		FMantleSkinnedMeshSection& Section = LOD.Sections.AddDefaulted_GetRef();
		Section.MaterialIndex = Slot;
		Section.MaterialName = FName(Names[Slot]);
		Section.FirstIndex = LOD.Indices.Num();
		const uint32 First = static_cast<uint32>(LOD.Positions.Num());
		const float Spread = Slot == 0 ? 230.f : 5.f;
		for (const FVector2f Corner : { FVector2f(-1, -1), FVector2f(1, -1), FVector2f(1, 1), FVector2f(-1, 1) })
		{
			LOD.Positions.Add(FVector3f(Xs[Slot] + Corner.X * Spread * (Slot == 0 ? 1.f : 0.f) + Corner.X * 5.f, Corner.Y * 80.f, 60.f + 20.f * Slot));
			LOD.Normals.Add(FVector3f(0.f, 0.f, 1.f));
			LOD.UVs.Add(FVector2f(0.f, 0.f));
			LOD.InfluenceBones.Add(0);
			LOD.InfluenceWeights.Add(255);
		}
		LOD.Indices.Append({ First, First + 1, First + 2, First, First + 2, First + 3 });
		Section.IndexCount = 6;
	}
	const FMantleVehicleLightGuess Guess = FMantleVehicleLights::Find(Body, Materials, false);
	TestTrue(TEXT("the finder finds lights"), Guess.FoundAny());
	TestTrue(TEXT("the headlight lens is a whole headlight section"), Guess.WholeSlots[static_cast<int32>(EMantleLightKind::Head)].Contains(1));
	TestTrue(TEXT("the taillight lens is a whole rear light section"), Guess.WholeSlots[static_cast<int32>(EMantleLightKind::Rear)].Contains(2));
	TestTrue(TEXT("the reverse lens is a whole reverse light section"), Guess.WholeSlots[static_cast<int32>(EMantleLightKind::Reverse)].Contains(3));
	TestFalse(TEXT("and the paint is none of them"), Guess.WholeSlots[static_cast<int32>(EMantleLightKind::Head)].Contains(0) || Guess.WholeSlots[static_cast<int32>(EMantleLightKind::Rear)].Contains(0)
		|| Guess.WholeSlots[static_cast<int32>(EMantleLightKind::Reverse)].Contains(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleBuilderLensColorTest,
	"Mantle.Vehicle.BuiltInMaterials.RedAndAmberLensesKeepTheirColor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleBuilderLensColorTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleBuilderHubsLightsTestPrivate;
	const FString Lens = FMantleVehicleBuiltInMaterials::LightLens();
	UMaterial* Red = MaterialColored(FLinearColor(0.8f, 0.04f, 0.03f));
	UMaterial* Amber = MaterialColored(FLinearColor(0.9f, 0.45f, 0.02f));
	UMaterial* Clear = MaterialColored(FLinearColor(0.85f, 0.85f, 0.88f));
	UMaterial* Blue = MaterialColored(FLinearColor(0.05f, 0.15f, 0.8f));

	// A light section: the shared (clear) lens, unless its own color is red or amber.
	TestEqual(TEXT("a clear light is the shared lens"), FMantleVehicleBuiltInMaterials::Detect(Clear, TEXT("headlight_lens"), true), Lens);
	TestEqual(TEXT("a red light keeps its own material"), FMantleVehicleBuiltInMaterials::Detect(Red, TEXT("taillight_lens"), true), FString());
	TestEqual(TEXT("an amber one keeps its own"), FMantleVehicleBuiltInMaterials::Detect(Amber, TEXT("indicator_lens"), true), FString());
	TestEqual(TEXT("a blue one is not red or amber: the shared lens"), FMantleVehicleBuiltInMaterials::Detect(Blue, TEXT("blue_lens"), true), Lens);

	// A section named for a lens that is not found as a light: the same.
	TestEqual(TEXT("a clear lens by its name is the shared lens"), FMantleVehicleBuiltInMaterials::Detect(Clear, TEXT("headlight_lens"), false), Lens);
	TestEqual(TEXT("a red lens by its name keeps its own"), FMantleVehicleBuiltInMaterials::Detect(Red, TEXT("taillight_lens"), false), FString());
	TestEqual(TEXT("so does an amber one"), FMantleVehicleBuiltInMaterials::Detect(Amber, TEXT("signal_lens"), false), FString());

	// Without a color to read, the name says it.
	TestEqual(TEXT("a lens named red keeps its material"), FMantleVehicleBuiltInMaterials::Detect(nullptr, TEXT("lens_red"), false), FString());
	TestEqual(TEXT("a lens with no color and no word is the shared lens"), FMantleVehicleBuiltInMaterials::Detect(nullptr, TEXT("headlight_lens"), false), Lens);
	TestEqual(TEXT("a word that only has red in it is not a color"), FMantleVehicleBuiltInMaterials::Detect(nullptr, TEXT("covered_lens"), false), Lens);

	// What is not a lens is as it was: glass is glass, a red body panel is not a lens.
	TestEqual(TEXT("glass is still glass"), FMantleVehicleBuiltInMaterials::Detect(Clear, TEXT("windshield"), false), FString(FMantleVehicleBuiltInMaterials::Glass()));
	TestEqual(TEXT("a red body panel keeps its own"), FMantleVehicleBuiltInMaterials::Detect(Red, TEXT("body_panel"), false), FString());
	TestTrue(TEXT("IsColoredLens says so for red and amber and not for clear"), FMantleVehicleBuiltInMaterials::IsColoredLens(Red, TEXT("x")) && FMantleVehicleBuiltInMaterials::IsColoredLens(Amber, TEXT("x"))
		&& !FMantleVehicleBuiltInMaterials::IsColoredLens(Clear, TEXT("x")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleBuilderLayerFinishTest,
	"Mantle.Vehicle.BuildModel.PaintLayerStartsWithAFinish",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleBuilderLayerFinishTest::RunTest(const FString& Parameters)
{
	// BuildModel's secondaryfinish=chrome: the second paint layer starts as chrome (the Camaro's bumpers). The finish names are the document's, any case.
	EMantleVehicleSourcePaintFinish Finish = EMantleVehicleSourcePaintFinish::Gloss;
	TestTrue(TEXT("chrome is a finish"), FMantleVehicleConverter::ParsePaintFinish(TEXT("chrome"), Finish) && Finish == EMantleVehicleSourcePaintFinish::Chrome);
	TestTrue(TEXT("in any case"), FMantleVehicleConverter::ParsePaintFinish(TEXT("Soft_Rubber"), Finish) && Finish == EMantleVehicleSourcePaintFinish::SoftRubber);
	TestFalse(TEXT("a word that is no finish is not"), FMantleVehicleConverter::ParsePaintFinish(TEXT("shiny"), Finish));
	TestEqual(TEXT("the layers are named as the document names them"), FString(FMantleVehicleConverter::PaintLayerDocumentName(EMantleVehicleSourcePaintLayer::Secondary)), FString(TEXT("secondary")));

	UMantleVehicleSource* Source = NewObject<UMantleVehicleSource>(GetTransientPackage(), NAME_None, RF_Transient);
	FMantleVehicleSourcePaintLayer& Second = Source->Paint.Layers.AddDefaulted_GetRef();
	Second.Layer = EMantleVehicleSourcePaintLayer::Secondary;
	Second.Sections = { TEXT("front_bumper"), TEXT("rear_bumper") };
	FMantleVehicleSourcePaintLayer& Interior = Source->Paint.Layers.AddDefaulted_GetRef();
	Interior.Layer = EMantleVehicleSourcePaintLayer::Interior;
	Interior.Finish = EMantleVehicleSourcePaintFinish::Trim;
	TestTrue(TEXT("the second paint starts as gloss"), Source->Paint.Layers[0].Finish == EMantleVehicleSourcePaintFinish::Gloss);

	TMap<EMantleVehicleSourcePaintLayer, EMantleVehicleSourcePaintFinish> Finishes;
	Finishes.Add(EMantleVehicleSourcePaintLayer::Secondary, EMantleVehicleSourcePaintFinish::Chrome);
	TArray<FString> Notes;
	FMantleVehicleBuilder::ApplyLayerFinishes(*Source, Finishes, Notes);
	TestTrue(TEXT("the second paint starts as chrome"), Source->Paint.Layers[0].Finish == EMantleVehicleSourcePaintFinish::Chrome);
	TestTrue(TEXT("a layer that was not asked for is as it was"), Source->Paint.Layers[1].Finish == EMantleVehicleSourcePaintFinish::Trim);
	TestEqual(TEXT("and no layer was added"), Source->Paint.Layers.Num(), 2);
	TestTrue(TEXT("the notes say which sections and what finish"), Notes.ContainsByPredicate([](const FString& Note)
	{
		return Note.Contains(TEXT("secondary")) && Note.Contains(TEXT("front_bumper")) && Note.Contains(TEXT("chrome"));
	}));

	// A layer the build has no section for is not made up: it is said.
	Finishes.Add(EMantleVehicleSourcePaintLayer::Wheels, EMantleVehicleSourcePaintFinish::Chrome);
	Notes.Reset();
	FMantleVehicleBuilder::ApplyLayerFinishes(*Source, Finishes, Notes);
	TestEqual(TEXT("a wheels layer was not added"), Source->Paint.Layers.Num(), 2);
	TestTrue(TEXT("and it is said"), Notes.ContainsByPredicate([](const FString& Note) { return Note.StartsWith(TEXT("No section of the model is in the wheels paint layer")); }));
	return true;
}

#endif
