// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleBuilder.h"
#include "MantleVehicleChassisBuilder.h"
#include "MantleVehicleChassisDocument.h"
#include "MantleVehicleChassisFit.h"
#include "MantleVehicleChassisMesh.h"
#include "MantleVehicleChassisSkin.h"
#include "MantleVehicleConverter.h"
#include "MantleVehicleFit.h"
#include "MantleVehiclePreset.h"
#include "MantleVehicleSource.h"
#include "SMantleVehicleBuilder.h"
#include "MantleVehicleScratchBody.h"

#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetData.h"
#include "Dom/JsonObject.h"
#include "Engine/SkeletalMesh.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// Chassis from a model, as arithmetic: the creator's mesh worn as the skin of a type's chassis. The models here are boxes made in the test, as arrays,
// so nothing depends on an asset: how they are turned, scaled and stood on the ground, which bone each vertex rides, where a spinning part's bone goes, and what the
// fit makes of two wheels on the center line. The end-to-end builds (a jet from a static mesh, a plane, a bike, a helicopter) are in MantleGameplayEditor's
// Mantle.Vehicle.ChassisSkin.* tests, which also make the definitions.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVehicleChassisSkinTestPrivate
{
	/** A box on model bone Bone (INDEX_NONE for a static model's) as four vertices a face; Section is the section the triangles are counted in. */
	void AddBox(FMantleVehicleModel& Model, const FVector3f& Min, const FVector3f& Max, const int32 Bone, const int32 Section)
	{
		FMantleSkinnedMeshLOD& LOD = Model.LOD;
		const FVector3f Center = 0.5f * (Min + Max);
		const FVector3f Half = 0.5f * (Max - Min);
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
					LOD.InfluenceBones.Add(static_cast<uint16>(FMath::Max(Bone, 0)));
					LOD.InfluenceWeights.Add(255);
				}
				for (const uint32 Index : { 0u, 1u, 2u, 0u, 2u, 3u })
				{
					LOD.Indices.Add(First + Index);
				}
			}
		}
	}

	/** Finishes the sections of a model whose boxes were added in order: section s is the triangles added with Section s. */
	void CloseSections(FMantleVehicleModel& Model, const TArray<int32>& TrianglesPerSection)
	{
		int32 Index = 0;
		Model.LOD.Sections.Reset();
		for (int32 Section = 0; Section < TrianglesPerSection.Num(); ++Section)
		{
			FMantleSkinnedMeshSection& Added = Model.LOD.Sections.AddDefaulted_GetRef();
			Added.MaterialIndex = Section < Model.Materials.Num() ? Section : 0;
			Added.FirstIndex = Index;
			Added.IndexCount = TrianglesPerSection[Section] * 3;
			Added.MaterialName = Model.Materials.IsValidIndex(Added.MaterialIndex) ? Model.Materials[Added.MaterialIndex].MaterialSlotName : FName(TEXT("Skin"));
			Index += Added.IndexCount;
		}
		Model.LOD.NumInfluences = 1;
	}

	/** A skeletal model with bones (name, parent, position): the boxes are added after. */
	FMantleVehicleModel SkeletalModel(const TArray<TPair<FName, int32>>& Bones, const TArray<FVector>& Positions)
	{
		FMantleVehicleModel Model;
		Model.Name = TEXT("Test");
		Model.bSkeletal = true;
		for (int32 Bone = 0; Bone < Bones.Num(); ++Bone)
		{
			Model.Bones.Add({ Bones[Bone].Key, Bones[Bone].Value });
		}
		Model.BonePositions = Positions;
		Model.Materials.Add(FSkeletalMaterial(static_cast<UMaterialInterface*>(nullptr), FName(TEXT("Skin"))));
		return Model;
	}

	bool Load(FAutomationTestBase& Test, const TCHAR* Id, FMantleChassisSpec& OutSpec, FMantleVehicleAsset& OutGray)
	{
		FString Error;
		const bool bOk = FMantleVehicleChassisBuilder::LoadSpec(Id, OutSpec, Error) && FMantleVehicleChassisBuilder::MakeAsset(OutSpec, OutGray, Error);
		Test.TestTrue(*FString::Printf(TEXT("the %s chassis loads (%s)"), Id, *Error), bOk);
		return bOk;
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleChassisSkinCenterlineFitTest,
	"Mantle.Vehicle.ChassisFit.CenterlineBike",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleChassisSkinCenterlineFitTest::RunTest(const FString& Parameters)
{
	// A bike's two wheels, on the model's center line: not an invalid layout with no track.
	const TArray<FMantleVehicleFitWheel> Bike = { { FVector(70, 0.4, 34), 34.0 }, { FVector(-80, -0.3, 34), 34.0 } };
	const FMantleChassisLayout BikeLayout = FMantleVehicleChassisFit::LayoutFromWheels(Bike);
	TestTrue(TEXT("two wheels on the center line are a valid layout"), BikeLayout.bValid);
	TestTrue(TEXT("a centerline one"), BikeLayout.bCenterline);
	TestEqual(TEXT("with no track"), BikeLayout.Track, 0.0);
	TestEqual(TEXT("and a 150 cm wheelbase"), BikeLayout.Wheelbase(), 150.0);

	// Negative control: a car's four wheels have a track, and are not centerline.
	const TArray<FMantleVehicleFitWheel> Car = { { FVector(140, -80, 33), 33.0 }, { FVector(140, 80, 33), 33.0 }, { FVector(-140, -80, 33), 33.0 }, { FVector(-140, 80, 33), 33.0 } };
	const FMantleChassisLayout CarLayout = FMantleVehicleChassisFit::LayoutFromWheels(Car);
	TestTrue(TEXT("a car's layout is valid"), CarLayout.bValid);
	TestFalse(TEXT("and has wheels on both sides"), CarLayout.bCenterline);
	TestEqual(TEXT("with its track"), CarLayout.Track, 160.0);

	// A motorcycle chassis stretched to that bike has finite stretches in every direction, and its seat has no steering wheel to be forced on.
	FString Text;
	FString Error;
	FMantleVehicleChassisReference Chassis;
	if (!TestTrue(*FString::Printf(TEXT("the motorcycle chassis reads (%s)"), *Error),
			FMantleVehicleChassisBuilder::LoadPresetText(TEXT("motorcycle"), Text, Error) && FMantleVehicleChassisBuilder::ReadReference(Text, Chassis, Error)))
	{
		return false;
	}
	TestTrue(TEXT("the chassis is a centerline one"), Chassis.bCenterline);
	const FMantleChassisStretch Stretch = FMantleVehicleChassisFit::Stretch(Chassis, BikeLayout, 1.0);
	TestTrue(TEXT("its stretches are finite and in range"), FMath::IsFinite(Stretch.Kx) && FMath::IsFinite(Stretch.Ky) && FMath::IsFinite(Stretch.Kz) && Stretch.Ky >= FMantleVehicleChassisFit::MinStretch);
	const FMantleChassisPlacement Placement = FMantleVehicleChassisFit::Place(Chassis, Stretch);
	TestFalse(TEXT("a rider's seat is placed without a steering wheel"), Placement.bSteeringWheel);
	UMantleVehicleSource* Source = NewObject<UMantleVehicleSource>(GetTransientPackage(), NAME_None, RF_Transient);
	Source->Tuning.bSteeringWheel = true;
	FMantleVehicleChassisFit::Apply(Placement, *Source);
	TestFalse(TEXT("so Apply does not force one on"), Source->Tuning.bSteeringWheel);

	// Positive control: a car's seat keeps its steering wheel.
	FMantleVehicleChassisReference Sedan;
	if (TestTrue(TEXT("the sedan chassis reads"), FMantleVehicleChassisBuilder::LoadPresetText(TEXT("sedan"), Text, Error) && FMantleVehicleChassisBuilder::ReadReference(Text, Sedan, Error)))
	{
		UMantleVehicleSource* Car4 = NewObject<UMantleVehicleSource>(GetTransientPackage(), NAME_None, RF_Transient);
		FMantleVehicleChassisFit::Apply(FMantleVehicleChassisFit::Place(Sedan, FMantleVehicleChassisFit::Stretch(Sedan, CarLayout, 1.0)), *Car4);
		TestTrue(TEXT("a sedan's seat keeps its steering wheel"), Car4->Tuning.bSteeringWheel);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleChassisSkinFootprintTest,
	"Mantle.Vehicle.Fit.FootprintSize",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleChassisSkinFootprintTest::RunTest(const FString& Parameters)
{
	const FMantleVehiclePreset* Plane = FMantleVehiclePresets::Find(TEXT("plane"));
	if (!TestNotNull(TEXT("the plane preset"), Plane))
	{
		return false;
	}
	// The same ground covered: the standard size.
	TestEqual(TEXT("a model covering the chassis's footprint is Size 1"), FMantleVehicleFitter::ComputeFromFootprint(*Plane, 1000.0, 800.0, 1000.0, 800.0).Scale, 1.0);
	// 10 per cent longer and wider in a lump: 1.1.
	const FMantleVehicleFit Bigger = FMantleVehicleFitter::ComputeFromFootprint(*Plane, 1100.0, 880.0, 1000.0, 800.0);
	TestEqual(TEXT("10 per cent more in each direction is Size 1.1"), Bigger.Scale, 1.1);
	TestFalse(TEXT("which is within the plane's range"), Bigger.bClamped);
	// Held to the preset's fit range, with the reason said.
	const FMantleVehicleFit Huge = FMantleVehicleFitter::ComputeFromFootprint(*Plane, 5000.0, 4000.0, 1000.0, 800.0);
	TestTrue(TEXT("a model five times as big is held to the plane's largest size"), Huge.bClamped && FMath::IsNearlyEqual(Huge.Scale, Plane->FitMaxScale));
	TestTrue(TEXT("and the reason says the model is shrunk"), Huge.Reason.Contains(TEXT("shrunk")));
	const FMantleVehicleFit Tiny = FMantleVehicleFitter::ComputeFromFootprint(*Plane, 100.0, 80.0, 1000.0, 800.0);
	TestTrue(TEXT("and a tiny one to its smallest"), Tiny.bClamped && FMath::IsNearlyEqual(Tiny.Scale, Plane->FitMinScale) && Tiny.Reason.Contains(TEXT("enlarged")));
	// Nothing to measure.
	const FMantleVehicleFit Empty = FMantleVehicleFitter::ComputeFromFootprint(*Plane, 0.0, 0.0, 1000.0, 800.0);
	TestEqual(TEXT("a model with no size is left at the standard size"), Empty.Scale, 1.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleChassisSkinPlacementTest,
	"Mantle.Vehicle.ChassisSkin.PlacementAndWeights",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleChassisSkinPlacementTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleChassisSkinTestPrivate;
	FMantleChassisSpec Spec;
	FMantleVehicleAsset Gray;
	if (!Load(*this, TEXT("jet"), Spec, Gray))
	{
		return false;
	}
	const TArray<FMantleVehiclePartSlot> Slots = FMantleVehicleChassisSkin::PartSlots(Spec, Gray.Body);

	// A static model long along Y (a nose on +Y): 12 m by 18 m, 3 m tall, off to one side of the origin and floating 10 m up.
	FMantleVehicleModel Model;
	Model.Name = TEXT("Ship");
	Model.Materials.Add(FSkeletalMaterial(static_cast<UMaterialInterface*>(nullptr), FName(TEXT("Hull"))));
	AddBox(Model, FVector3f(5000.f, 2000.f, 1000.f), FVector3f(6200.f, 3800.f, 1300.f), INDEX_NONE, 0);
	CloseSections(Model, { 12 });
	TestTrue(TEXT("the model has geometry"), Model.Bounds().IsValid != 0);

	// Turned to face +X it is 18 m long and 12 m across; the chassis's footprint is matched.
	const FVector Extent = FMantleVehicleChassisSkin::Extent(Model, 270.0);
	TestTrue(*FString::Printf(TEXT("turned 270 degrees its length is the Y it had (%.0f x %.0f)"), Extent.X, Extent.Y), FMath::IsNearlyEqual(Extent.X, 1800.0, 1.0) && FMath::IsNearlyEqual(Extent.Y, 1200.0, 1.0));
	const FMantleVehicleModelPlacement Placement = FMantleVehicleChassisSkin::FitPlacement(Spec, Gray.Body, Model, 270.0, FMantleVehicleModelMap());
	FMantleVehicleSkinned Skinned;
	FString Error;
	if (!TestTrue(*FString::Printf(TEXT("the model skins (%s)"), *Error), FMantleVehicleChassisSkin::Skin(Gray.Body, Slots, Model, Placement, FMantleVehicleModelMap(), Skinned, Error)))
	{
		return false;
	}
	const FBox3f GrayBox = Gray.Body.ComputeBounds(0);
	const FBox3f Box = Skinned.Body.ComputeBounds(0);
	const double GrayArea = (GrayBox.Max.X - GrayBox.Min.X) * (GrayBox.Max.Y - GrayBox.Min.Y);
	const double Area = (Box.Max.X - Box.Min.X) * (Box.Max.Y - Box.Min.Y);
	TestTrue(*FString::Printf(TEXT("it covers the chassis's ground (%.0f against %.0f square cm)"), Area, GrayArea), FMath::Abs(Area - GrayArea) < 0.01 * GrayArea);
	TestTrue(TEXT("it is longer than wide, nose on +X"), Box.Max.X - Box.Min.X > Box.Max.Y - Box.Min.Y);
	TestTrue(TEXT("it stands on the ground"), FMath::Abs(Box.Min.Z) < 0.01f);
	TestTrue(*FString::Printf(TEXT("and is centered on the chassis (x %.0f)"), 0.5f * (Box.Min.X + Box.Max.X)),
		FMath::Abs(0.5f * (Box.Min.X + Box.Max.X) - 0.5f * (GrayBox.Min.X + GrayBox.Max.X)) < 1.0f && FMath::Abs(0.5f * (Box.Min.Y + Box.Max.Y)) < 1.0f);

	// Whole to the root, one influence of full weight each, the model's triangles and its material slot kept.
	const FMantleSkinnedMeshLOD& LOD = Skinned.Body.LODs[0];
	bool bAllRoot = LOD.NumInfluences == 1;
	for (int32 Vertex = 0; Vertex < LOD.NumVertices(); ++Vertex)
	{
		bAllRoot &= LOD.InfluenceBones[Vertex] == 0 && LOD.InfluenceWeights[Vertex] == 255;
	}
	TestTrue(TEXT("every vertex is weighted whole to the root bone"), bAllRoot);
	TestEqual(TEXT("the triangles are the model's"), LOD.NumTriangles(), 12);
	TestEqual(TEXT("and so is its one material slot"), Skinned.Materials.Num(), 1);
	TestEqual(TEXT("the skeleton is the chassis's"), Skinned.Body.Bones.Num(), Gray.Body.Bones.Num());

	// Positive control: the placement is what moved it. Unturned, the model keeps its Y as its width.
	const FVector Unturned = FMantleVehicleChassisSkin::Extent(Model, 0.0);
	TestTrue(TEXT("unturned the model is 12 m long and 18 m across"), FMath::IsNearlyEqual(Unturned.X, 1200.0, 1.0) && FMath::IsNearlyEqual(Unturned.Y, 1800.0, 1.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleChassisSkinPartsTest,
	"Mantle.Vehicle.ChassisSkin.PartsMoveTheirBones",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleChassisSkinPartsTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleChassisSkinTestPrivate;
	FMantleChassisSpec Spec;
	FMantleVehicleAsset Gray;
	if (!Load(*this, TEXT("plane"), Spec, Gray))
	{
		return false;
	}
	const TArray<FMantleVehiclePartSlot> Slots = FMantleVehicleChassisSkin::PartSlots(Spec, Gray.Body);
	const auto HasSlot = [&Slots](const TCHAR* Name) { return Slots.ContainsByPredicate([Name](const FMantleVehiclePartSlot& Slot) { return Slot.ChassisBone == FName(Name); }); };
	TestTrue(TEXT("a plane's slots are its nose and main wheels, ailerons, elevator, rudder and propeller"),
		HasSlot(TEXT("wheel_f")) && HasSlot(TEXT("wheel_rl")) && HasSlot(TEXT("wheel_rr")) && HasSlot(TEXT("aileron_l")) && HasSlot(TEXT("aileron_r")) && HasSlot(TEXT("elevator"))
		&& HasSlot(TEXT("rudder")) && HasSlot(TEXT("propeller")));
	TestFalse(TEXT("and nothing for the root"), HasSlot(TEXT("root")));

	// A plane whose bones are called anything: a hub, a nose wheel, two main wheels, a left aileron. Its fuselage (bone 0) is 11 m long, nose on +X, 4 m of wing.
	FMantleVehicleModel Model = SkeletalModel({ { FName(TEXT("Hull")), INDEX_NONE }, { FName(TEXT("Spinner")), 0 }, { FName(TEXT("WheelNose")), 0 }, { FName(TEXT("WheelMainL")), 0 },
		{ FName(TEXT("WheelMainR")), 0 }, { FName(TEXT("Flap_L")), 0 } },
		{ FVector(0, 0, 0), FVector(602, 0, 150), FVector(400, 0, 40), FVector(-40, -250, 40), FVector(-40, 250, 40), FVector(-260, -500, 140) });
	AddBox(Model, FVector3f(-550.f, -60.f, 60.f), FVector3f(560.f, 60.f, 220.f), 0, 0);   // 24 vertices: the fuselage
	AddBox(Model, FVector3f(598.f, -110.f, 140.f), FVector3f(606.f, 110.f, 160.f), 1, 0);  // the propeller
	AddBox(Model, FVector3f(380.f, -8.f, 0.f), FVector3f(420.f, 8.f, 80.f), 2, 0);          // the nose wheel
	AddBox(Model, FVector3f(-60.f, -258.f, 0.f), FVector3f(-20.f, -242.f, 80.f), 3, 0);     // the left main wheel
	AddBox(Model, FVector3f(-60.f, 242.f, 0.f), FVector3f(-20.f, 258.f, 80.f), 4, 0);       // the right main wheel
	AddBox(Model, FVector3f(-280.f, -700.f, 130.f), FVector3f(-240.f, -300.f, 150.f), 5, 0); // the left flap
	CloseSections(Model, { 72 });

	// By name and by where they are, with the model facing +X.
	FMantleVehicleModelMap Suggested;
	const double Yaw = FMantleVehicleChassisSkin::SuggestMapAndYaw(Slots, Model, Suggested);
	TestEqual(TEXT("the nose wheel leads, so the model needs no turn"), Yaw, 0.0);
	const auto Bone = [&Suggested](const TCHAR* Slot) -> FString
	{
		const FMantleVehicleModelPiece* Piece = Suggested.Find(FName(Slot));
		return Piece && Piece->Kind == FMantleVehicleModelPiece::EKind::Bone ? Piece->Bone.ToString() : FString();
	};
	TestEqual(TEXT("the nose wheel is the front one"), Bone(TEXT("wheel_f")), FString(TEXT("WheelNose")));
	TestEqual(TEXT("the left main wheel is on the left"), Bone(TEXT("wheel_rl")), FString(TEXT("WheelMainL")));
	TestEqual(TEXT("and the right one on the right"), Bone(TEXT("wheel_rr")), FString(TEXT("WheelMainR")));
	TestEqual(TEXT("a bone with 'flap' in its name is not guessed to be an aileron"), Bone(TEXT("aileron_l")), FString());

	// The same model turned: its nose on -Y. The suggestion finds the turn that puts the nose wheel ahead of the mains.
	FMantleVehicleModel Turned = Model;
	const FRotator Quarter(0.0, -90.0, 0.0);
	for (FVector3f& Position : Turned.LOD.Positions)
	{
		Position = FVector3f(Quarter.RotateVector(FVector(Position)));
	}
	for (FVector3f& Normal : Turned.LOD.Normals)
	{
		Normal = FVector3f(Quarter.RotateVector(FVector(Normal)));
	}
	for (FVector& Position : Turned.BonePositions)
	{
		Position = Quarter.RotateVector(Position);
	}
	FMantleVehicleModelMap TurnedMap;
	const double TurnedYaw = FMantleVehicleChassisSkin::SuggestMapAndYaw(Slots, Turned, TurnedMap);
	TestEqual(TEXT("a model turned 90 degrees to the right way round is found and turned back"), FRotator::NormalizeAxis(TurnedYaw), 90.0);
	TestTrue(TEXT("with its wheels assigned"), TurnedMap.Find(FName(TEXT("wheel_f"))) && TurnedMap.Find(FName(TEXT("wheel_f")))->Bone == FName(TEXT("WheelNose")));

	// Skin it with the propeller and the left main wheel assigned.
	FMantleVehicleModelMap Map;
	Map.Parts.Add(FName(TEXT("propeller")), FMantleVehicleModelPiece::OfBone(FName(TEXT("Spinner"))));
	Map.Parts.Add(FName(TEXT("wheel_rl")), FMantleVehicleModelPiece::OfBone(FName(TEXT("WheelMainL"))));
	const FMantleVehicleModelPlacement Placement = FMantleVehicleChassisSkin::FitPlacement(Spec, Gray.Body, Model, 0.0, Map);
	FMantleVehicleSkinned Skinned;
	FString Error;
	// First with a limit that lets any wheel move (the model's wheel is a meter from the chassis's): the part is put on the wheel.
	if (!TestTrue(*FString::Printf(TEXT("the model skins (%s)"), *Error), FMantleVehicleChassisSkin::Skin(Gray.Body, Slots, Model, Placement, Map, Skinned, Error, 100000.0)))
	{
		return false;
	}
	const FMantleSkinnedMesh& Body = Skinned.Body;
	const int32 Propeller = Body.Bones.IndexOfByPredicate([](const FMantleSkinnedMeshBone& B) { return B.Name == FName(TEXT("propeller")); });
	const int32 LeftWheel = Body.Bones.IndexOfByPredicate([](const FMantleSkinnedMeshBone& B) { return B.Name == FName(TEXT("wheel_rl")); });
	if (!TestTrue(TEXT("the chassis's propeller and left main wheel bones are in the skeleton"), Propeller > 0 && LeftWheel > 0))
	{
		return false;
	}
	const FMantleSkinnedMeshLOD& LOD = Body.LODs[0];
	FBox3f PropBox(ForceInit);
	FBox3f WheelBox(ForceInit);
	int32 OnProp = 0;
	int32 OnWheel = 0;
	int32 OnRoot = 0;
	for (int32 Vertex = 0; Vertex < LOD.NumVertices(); ++Vertex)
	{
		const int32 Rides = LOD.InfluenceBones[Vertex];
		if (Rides == Propeller)
		{
			PropBox += LOD.Positions[Vertex];
		}
		else if (Rides == LeftWheel)
		{
			WheelBox += LOD.Positions[Vertex];
		}
		OnProp += Rides == Propeller ? 1 : 0;
		OnWheel += Rides == LeftWheel ? 1 : 0;
		OnRoot += Rides == 0 ? 1 : 0;
	}
	TestEqual(TEXT("the propeller's 24 vertices ride the propeller bone"), OnProp, 24);
	TestEqual(TEXT("the left main wheel's ride the wheel bone"), OnWheel, 24);
	TestEqual(TEXT("the rest, the fuselage, the other wheels and the flap, ride the root"), OnRoot, LOD.NumVertices() - 48);
	// A spinning part spins about its own hub: the bone is moved to it.
	TestTrue(*FString::Printf(TEXT("the propeller bone is at the propeller's hub (%s against %s)"), *ComponentLocation(Body, Propeller).ToString(), *FVector(PropBox.GetCenter()).ToString()),
		FVector::Dist(ComponentLocation(Body, Propeller), FVector(PropBox.GetCenter())) < 0.5);
	TestFalse(TEXT("and that is not where the chassis had it"), ComponentLocation(Body, Propeller).Equals(ComponentLocation(Gray.Body, Propeller), 0.5));
	// A wheel turns about the chassis's wheel center, so the part is moved onto it and the bone stays.
	TestTrue(*FString::Printf(TEXT("the left wheel bone stays at the chassis's wheel (%s)"), *ComponentLocation(Body, LeftWheel).ToString()), ComponentLocation(Body, LeftWheel).Equals(ComponentLocation(Gray.Body, LeftWheel), 0.01));
	TestTrue(*FString::Printf(TEXT("and the wheel part is centered on it (%s)"), *FVector(WheelBox.GetCenter()).ToString()), FVector::Dist(ComponentLocation(Body, LeftWheel), FVector(WheelBox.GetCenter())) < 0.5);

	// Positive control: with the default limit that same wheel, a meter from the chassis's, is not torn off its strut or swung about the wrong point: it stays still.
	FMantleVehicleSkinned Still;
	if (TestTrue(*FString::Printf(TEXT("the model skins again (%s)"), *Error), FMantleVehicleChassisSkin::Skin(Gray.Body, Slots, Model, Placement, Map, Still, Error)))
	{
		int32 StillOnWheel = 0;
		for (int32 Vertex = 0; Vertex < Still.Body.LODs[0].NumVertices(); ++Vertex)
		{
			StillOnWheel += Still.Body.LODs[0].InfluenceBones[Vertex] == LeftWheel ? 1 : 0;
		}
		TestEqual(TEXT("a wheel that far off rides the root"), StillOnWheel, 0);
		TestTrue(TEXT("and the notes say it stays still"), Still.Notes.ContainsByPredicate([](const FString& Note) { return Note.Contains(TEXT("stays still")); }));
		TestTrue(TEXT("while the propeller, at the chassis's own hub, still turns"), Still.Body.LODs[0].InfluenceBones.Contains(static_cast<uint16>(Propeller)));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleChassisSkinSectionsTest,
	"Mantle.Vehicle.ChassisSkin.SectionsOfAStaticMesh",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleChassisSkinSectionsTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleChassisSkinTestPrivate;
	FMantleChassisSpec Spec;
	FMantleVehicleAsset Gray;
	if (!Load(*this, TEXT("helicopter"), Spec, Gray))
	{
		return false;
	}
	const TArray<FMantleVehiclePartSlot> Slots = FMantleVehicleChassisSkin::PartSlots(Spec, Gray.Body);
	TestFalse(TEXT("a helicopter's skids are never drawn, so it has no wheel slots"), Slots.ContainsByPredicate([](const FMantleVehiclePartSlot& Slot) { return Slot.ChassisBone.ToString().StartsWith(TEXT("wheel_")); }));
	TestTrue(TEXT("but it has a main and a tail rotor"), Slots.ContainsByPredicate([](const FMantleVehiclePartSlot& Slot) { return Slot.ChassisBone == FName(TEXT("main_rotor")); })
		&& Slots.ContainsByPredicate([](const FMantleVehiclePartSlot& Slot) { return Slot.ChassisBone == FName(TEXT("tail_rotor")); }));

	// A static helicopter of two sections, a "Body" of the cabin and a "MainRotorBlades" of the blades, the blades' mesh sharing one vertex with the cabin's.
	FMantleVehicleModel Model;
	Model.Name = TEXT("Heli");
	Model.Materials.Add(FSkeletalMaterial(static_cast<UMaterialInterface*>(nullptr), FName(TEXT("Body"))));
	Model.Materials.Add(FSkeletalMaterial(static_cast<UMaterialInterface*>(nullptr), FName(TEXT("MainRotorBlades"))));
	AddBox(Model, FVector3f(-100.f, -100.f, 0.f), FVector3f(100.f, 100.f, 200.f), INDEX_NONE, 0);
	AddBox(Model, FVector3f(-500.f, -15.f, 300.f), FVector3f(500.f, 15.f, 310.f), INDEX_NONE, 1);
	CloseSections(Model, { 12, 12 });
	// The blades' first triangle also uses the cabin's first vertex (a shared vertex), so the part must take a copy.
	Model.LOD.Indices[12 * 3] = Model.LOD.Indices[0];

	FMantleVehicleModelMap Suggested;
	const double Yaw = FMantleVehicleChassisSkin::SuggestMapAndYaw(Slots, Model, Suggested);
	TestEqual(TEXT("with no wheels or rotors to lead, the model is left as modeled"), Yaw, 0.0);
	const FMantleVehicleModelPiece* Main = Suggested.Find(FName(TEXT("main_rotor")));
	TestTrue(TEXT("the section named for the main rotor is suggested for it"), Main && Main->Kind == FMantleVehicleModelPiece::EKind::Section && Main->Section == 1);

	const FMantleVehicleModelPlacement Placement = FMantleVehicleChassisSkin::FitPlacement(Spec, Gray.Body, Model, Yaw, Suggested);
	FMantleVehicleSkinned Skinned;
	FString Error;
	if (!TestTrue(*FString::Printf(TEXT("the model skins (%s)"), *Error), FMantleVehicleChassisSkin::Skin(Gray.Body, Slots, Model, Placement, Suggested, Skinned, Error)))
	{
		return false;
	}
	const FMantleSkinnedMeshLOD& LOD = Skinned.Body.LODs[0];
	const int32 Rotor = Skinned.Body.Bones.IndexOfByPredicate([](const FMantleSkinnedMeshBone& B) { return B.Name == FName(TEXT("main_rotor")); });
	TestEqual(TEXT("the shared vertex was copied, so the model has one more vertex"), LOD.NumVertices(), Model.LOD.NumVertices() + 1);
	int32 OnRotor = 0;
	for (int32 Vertex = 0; Vertex < LOD.NumVertices(); ++Vertex)
	{
		OnRotor += LOD.InfluenceBones[Vertex] == Rotor ? 1 : 0;
	}
	TestEqual(TEXT("24 vertices plus the copy ride the rotor bone"), OnRotor, 25);
	// The cabin's shared vertex stays on the root: the copy, not the original, is the rotor's.
	TestEqual(TEXT("the cabin's own first vertex rides the root"), static_cast<int32>(LOD.InfluenceBones[0]), 0);
	TestEqual(TEXT("two sections, two slots, in the model's order"), Skinned.Materials.Num(), 2);
	TestEqual(TEXT("the sections keep their materials"), LOD.Sections.Num(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleChassisSkinGroundHandlingTest,
	"Mantle.Vehicle.Builder.CarFromModelHasChassisHandling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleChassisSkinGroundHandlingTest::RunTest(const FString& Parameters)
{
	// A car built from a model fitted to its wheels drives as the sedan chassis does: the steering that eases off with speed, and the front and rear grip split.
	FString Error;
	USkeletalMesh* Body = MantleVehicleScratchBody::Make(TEXT("/Temp/MantleSkinGroundHandling"), TEXT("SkinHandlingSedan"), Error);
	if (!TestNotNull(*FString::Printf(TEXT("the scratch body is made (%s)"), *Error), Body))
	{
		return false;
	}
	TestTrue(TEXT("its own corner-named wheels fit the sedan"), FMantleVehicleBuilder::FitsOwnWheels(Body, TEXT("sedan")));
	TestFalse(TEXT("but not a motorcycle's"), FMantleVehicleBuilder::FitsOwnWheels(Body, TEXT("motorcycle")));
	TestFalse(TEXT("and not a plane's"), FMantleVehicleBuilder::FitsOwnWheels(Body, TEXT("plane")));
	const FMantleVehicleBuildResult Built = FMantleVehicleBuilder::BuildAny(Body, TEXT("sedan"));
	for (const FString& Problem : Built.Problems)
	{
		AddError(Problem);
	}
	if (TestTrue(TEXT("a model with corner wheels is fitted to them"), Built.bOk && Built.Source != nullptr))
	{
		TestFalse(TEXT("that is the wheel fit, not the skin"), Built.bOnChassis);
		const FMantleVehicleSourceTuning& Tuning = Built.Source->Tuning;
		TestEqual(TEXT("the steering eases off with speed over four points"), Tuning.SteeringSpeedCurve.Num(), 4);
		TestTrue(TEXT("with the full lock at a standstill and less at speed"), Tuning.SteeringSpeedCurve.Num() == 4 && Tuning.SteeringSpeedCurve[0].Y == 1.0 && Tuning.SteeringSpeedCurve[3].Y < 0.5);
		TestTrue(TEXT("the rear tires grip more than the front"), Tuning.bTireRearFriction && Tuning.TireRearFriction > Tuning.TireFriction);
		TestEqual(TEXT("it is a car: the wheeled movement type"), static_cast<int32>(Built.Source->MovementType), static_cast<int32>(EMantleVehicleMovementKind::Wheeled));
	}
	MantleVehicleScratchBody::Discard(Body);
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleChassisSkinWheelNamesTest,
	"Mantle.Vehicle.ChassisSkin.CenterlineWheelNames",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleChassisSkinWheelNamesTest::RunTest(const FString& Parameters)
{
	// A wheel on the center line is a wheel: a bike's two and a plane's nose wheel are not "no wheels" to the converter.
	const auto Parse = [](const TCHAR* Name, FMantleVehicleWheelBone& Out) { return FMantleVehicleConverter::ParseWheelBoneName(Name, Out); };
	FMantleVehicleWheelBone Wheel;
	TestTrue(TEXT("wheel_f is a wheel"), Parse(TEXT("wheel_f"), Wheel));
	TestTrue(TEXT("on the front axle, on the center line"), Wheel.Axle == 0 && Wheel.bCenter);
	TestTrue(TEXT("wheel_r is a wheel on the back axle"), Parse(TEXT("wheel_r"), Wheel) && Wheel.Axle == 2 && Wheel.bCenter);
	TestTrue(TEXT("Front_Wheel is the front one"), Parse(TEXT("Front_Wheel"), Wheel) && Wheel.Axle == 0 && Wheel.bCenter);
	TestTrue(TEXT("and wheel_rear the back one"), Parse(TEXT("wheel_rear"), Wheel) && Wheel.Axle == 2 && Wheel.bCenter);

	// Positive control: the corner names are what they were, and a bone with no position is no wheel.
	TestTrue(TEXT("Wheel_FL is still the front left corner"), Parse(TEXT("Wheel_FL"), Wheel) && Wheel.Axle == 0 && Wheel.bLeft && !Wheel.bCenter);
	TestTrue(TEXT("wheel_rr the back right"), Parse(TEXT("wheel_rr"), Wheel) && Wheel.Axle == 2 && !Wheel.bLeft && !Wheel.bCenter);
	TestTrue(TEXT("wheel_front_l is the front left"), Parse(TEXT("wheel_front_l"), Wheel) && Wheel.Axle == 0 && Wheel.bLeft && !Wheel.bCenter);
	TestFalse(TEXT("a spare wheel is not a wheel of the vehicle"), Parse(TEXT("spare_wheel"), Wheel));
	TestFalse(TEXT("a bone with 'wheel', front and rear is no wheel"), Parse(TEXT("wheel_front_rear"), Wheel));
	TestFalse(TEXT("a steering pivot is not a wheel"), Parse(TEXT("steer_wheel_f"), Wheel));
	TestFalse(TEXT("and a bone with no wheel in its name is not one"), Parse(TEXT("fork_f"), Wheel));
	return true;
}


// The window's model step state, which a test drives the way the pickers do. Named, not anonymous: MantleEditor builds as a unity build.
struct FMantleVehicleBuilderModelAccess
{
	static void PickType(SMantleVehicleBuilder& Panel, const TCHAR* Id, const TCHAR* Name) { Panel.PickPreset(Id, FText::FromString(Name)); }
	static void SetMesh(SMantleVehicleBuilder& Panel, UObject* Model) { Panel.SetMesh(FAssetData(Model)); }
	static bool HaveModel(const SMantleVehicleBuilder& Panel) { return Panel.bHaveModelInfo; }
	static bool UsesSkin(const SMantleVehicleBuilder& Panel) { return Panel.bUseSkin; }
	static int32 NumSlots(const SMantleVehicleBuilder& Panel) { return Panel.PartSlots.Num(); }
	static double Size(const SMantleVehicleBuilder& Panel) { return Panel.Shape.GetSize(); }
	static const FString& Proposal(const SMantleVehicleBuilder& Panel) { return Panel.ProposalText; }
	static const FMantleVehicleModelMap& Map(const SMantleVehicleBuilder& Panel) { return Panel.PartMap; }
	static void Assign(SMantleVehicleBuilder& Panel, const TCHAR* ChassisBone, const FMantleVehicleModelPiece& Piece) { Panel.SetPart(FName(ChassisBone), Piece); }
	static void Turn(SMantleVehicleBuilder& Panel, const double Degrees) { Panel.SetModelYaw(Degrees); }
	static double Yaw(const SMantleVehicleBuilder& Panel) { return Panel.ModelYaw; }
	static TSharedRef<SWidget> Step(SMantleVehicleBuilder& Panel) { return Panel.MakeModelStep(); }
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleChassisSkinPanelTest,
	"Mantle.Vehicle.ChassisSkin.WindowModelStep",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleChassisSkinPanelTest::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddWarning(TEXT("SKIPPED, not passed: there is no Slate application in this runner, so the window cannot be made."));
		return true;
	}
	using Access = FMantleVehicleBuilderModelAccess;
	FString Error;
	USkeletalMesh* Model = MantleVehicleScratchBody::Make(TEXT("/Temp/MantleSkinPanel"), TEXT("SkinPanelBody"), Error);
	if (!TestNotNull(*FString::Printf(TEXT("the scratch body is made (%s)"), *Error), Model))
	{
		return false;
	}
	TSharedRef<SMantleVehicleBuilder> Panel = SNew(SMantleVehicleBuilder);

	// A car model with corner-named wheels on a car type: fitted to its own wheels, no parts to point out.
	Access::PickType(*Panel, TEXT("sedan"), TEXT("Sedan"));
	Access::SetMesh(*Panel, Model);
	TestTrue(TEXT("the model is read"), Access::HaveModel(*Panel));
	TestFalse(TEXT("a car with corner wheels is fitted to them, not worn as a skin"), Access::UsesSkin(*Panel));
	TestEqual(TEXT("and has no parts to assign"), Access::NumSlots(*Panel), 0);
	TestTrue(TEXT("its step builds"), Access::Step(*Panel)->GetType() != NAME_None);

	// The same model on a plane: worn as a skin on the plane's chassis, with the plane's moving parts to point out, and the chassis sized to it.
	Access::PickType(*Panel, TEXT("plane"), TEXT("Plane"));
	TestTrue(TEXT("the model is read again for the plane"), Access::HaveModel(*Panel));
	TestTrue(TEXT("and worn as a skin"), Access::UsesSkin(*Panel));
	TestTrue(*FString::Printf(TEXT("the plane has parts to point out (%d)"), Access::NumSlots(*Panel)), Access::NumSlots(*Panel) >= 8);
	// A plane is tuned to fly at its standard size: the chassis stays at Size 1 and the (small) model is enlarged to it.
	TestTrue(*FString::Printf(TEXT("a small car body leaves the plane at its standard size (%.2f)"), Access::Size(*Panel)), FMath::IsNearlyEqual(Access::Size(*Panel), 1.0));
	TestTrue(TEXT("and the window says the model is enlarged to fit"), Access::Proposal(*Panel).Contains(TEXT("enlarged")));
	// The hovercraft is sized to the model, within its range.
	Access::PickType(*Panel, TEXT("hovercraft"), TEXT("Hovercraft"));
	const FMantleVehiclePreset* Hover = FMantleVehiclePresets::Find(TEXT("hovercraft"));
	TestTrue(*FString::Printf(TEXT("a car body, smaller than a hovercraft's ground, makes a smaller hovercraft (%.2f)"), Access::Size(*Panel)),
		Hover && Access::Size(*Panel) < 1.0 && Access::Size(*Panel) >= Hover->FitMinScale);
	Access::PickType(*Panel, TEXT("plane"), TEXT("Plane"));
	TSharedRef<SWidget> Step = Access::Step(*Panel);
	TestTrue(TEXT("its step builds with the parts"), Step->GetType() != NAME_None);

	// Pointing out a part: it is kept, and the step is made again.
	Access::Assign(*Panel, TEXT("propeller"), FMantleVehicleModelPiece::OfBone(FName(TEXT("wheel_fl"))));
	TestTrue(TEXT("a propeller is assigned"), Access::Map(*Panel).Find(FName(TEXT("propeller"))) != nullptr);
	Access::Assign(*Panel, TEXT("propeller"), FMantleVehicleModelPiece());
	TestTrue(TEXT("and unassigned again"), Access::Map(*Panel).Find(FName(TEXT("propeller"))) == nullptr);
	Access::Turn(*Panel, 90.0);
	TestEqual(TEXT("the model is turned"), Access::Yaw(*Panel), 90.0);

	// A motorcycle: wheels, a fork and no wheels of the model's to guess at.
	Access::PickType(*Panel, TEXT("motorcycle"), TEXT("Motorcycle"));
	TestTrue(TEXT("a car body is worn as a motorcycle's skin"), Access::UsesSkin(*Panel));
	TestEqual(TEXT("the bike has its two wheels and its fork to point out"), Access::NumSlots(*Panel), 3);
	// No model: no rows, and nothing breaks.
	Access::SetMesh(*Panel, nullptr);
	TestFalse(TEXT("clearing the model clears what was read"), Access::HaveModel(*Panel));
	MantleVehicleScratchBody::Discard(Model);
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleChassisSkinTwinTest,
	"Mantle.Vehicle.ChassisSkin.TwinPushesAsTheSingleDid",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleChassisSkinTwinTest::RunTest(const FString& Parameters)
{
	// A twin-engined plane pushes with the same total as the preset's one engine, so it flies as tuned; made twice as strong by a wrong share it took off in a
	// third of the roll and climbed out of trim.
	FMantleChassisSpec Single;
	FString Error;
	if (!TestTrue(*FString::Printf(TEXT("the plane loads (%s)"), *Error), FMantleVehicleChassisBuilder::LoadSpec(TEXT("plane"), Single, Error)))
	{
		return false;
	}
	TestTrue(TEXT("a single-engined propeller plane can be made a twin"), FMantleVehicleChassisSkin::CanBeTwin(Single));
	const FMantleChassisSpec Twin = FMantleVehicleChassisSkin::AsTwin(Single);
	TestFalse(TEXT("and a twin cannot be made one again"), FMantleVehicleChassisSkin::CanBeTwin(Twin));
	const auto TotalThrust = [](const FMantleChassisSpec& Spec, int32& OutEngines)
	{
		FMantleVehicleDesc Desc;
		FMantleVehicleChassisDocument::Derive(Spec, Desc);
		double Sum = 0.0;
		for (const FMantleVehicleThrusterDesc& Thruster : Desc.Flight.Thrusters)
		{
			Sum += Thruster.MaxThrustNewtons;
		}
		OutEngines = Desc.Flight.Thrusters.Num();
		return Sum;
	};
	int32 SingleEngines = 0;
	int32 TwinEngines = 0;
	const double SingleThrust = TotalThrust(Single, SingleEngines);
	TestEqual(TEXT("the single has one engine"), SingleEngines, 1);
	TestEqual(TEXT("the twin has two"), TotalThrust(Twin, TwinEngines) > 0.0 ? TwinEngines : 0, 2);
	TestTrue(*FString::Printf(TEXT("pushing %.0f N against the single's %.0f"), TotalThrust(Twin, TwinEngines), SingleThrust), FMath::IsNearlyEqual(TotalThrust(Twin, TwinEngines), SingleThrust, 1.0));

	// Resized, the pair still pushes as the single resized does (the archetype's share is per engine).
	const FMantleChassisSpec SingleBigger = FMantleChassisSpec::Scaled(Single, 1.1);
	const FMantleChassisSpec TwinBigger = FMantleChassisSpec::Scaled(Twin, 1.1);
	TestTrue(*FString::Printf(TEXT("and at 1.1 (%.0f against %.0f N)"), TotalThrust(TwinBigger, TwinEngines), TotalThrust(SingleBigger, SingleEngines)),
		FMath::IsNearlyEqual(TotalThrust(TwinBigger, TwinEngines), TotalThrust(SingleBigger, SingleEngines), 1.0));
	return true;
}

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVehicleHingeTestPrivate
{
	/**
	 * A car as a skeletal model, nose on +X: a body on bone 0, a door on each side front and rear, a bonnet and a boot, each on a bone named as GTA's are; a window on a bone
	 * under the driver's door, and a door handle on a bone that is not a door (it stays on the body).
	 */
	FMantleVehicleModel HingeCar()
	{
		using namespace MantleVehicleChassisSkinTestPrivate;
		FMantleVehicleModel Model = SkeletalModel({ { FName(TEXT("chassis_dummy")), INDEX_NONE }, { FName(TEXT("door_dside_f")), 0 }, { FName(TEXT("door_pside_f")), 0 },
			{ FName(TEXT("door_dside_r")), 0 }, { FName(TEXT("door_pside_r")), 0 }, { FName(TEXT("bonnet")), 0 }, { FName(TEXT("boot")), 0 }, { FName(TEXT("window_dside_f")), 1 },
			{ FName(TEXT("doorhandle")), 0 } },
			{ FVector(0, 0, 0), FVector(105, -95, 100), FVector(105, 95, 100), FVector(5, -95, 100), FVector(5, 95, 100), FVector(110, 0, 100), FVector(-120, 0, 100), FVector(100, -95, 110),
			FVector(60, -96, 100) });
		AddBox(Model, FVector3f(-220.f, -90.f, 20.f), FVector3f(220.f, 90.f, 70.f), 0, 0);       // the body
		AddBox(Model, FVector3f(30.f, -96.f, 70.f), FVector3f(110.f, -86.f, 130.f), 1, 0);       // the driver's door
		AddBox(Model, FVector3f(30.f, 86.f, 70.f), FVector3f(110.f, 96.f, 130.f), 2, 0);         // the passenger's door
		AddBox(Model, FVector3f(-70.f, -96.f, 70.f), FVector3f(10.f, -86.f, 130.f), 3, 0);       // the rear left door
		AddBox(Model, FVector3f(-70.f, 86.f, 70.f), FVector3f(10.f, 96.f, 130.f), 4, 0);         // the rear right door
		AddBox(Model, FVector3f(110.f, -70.f, 100.f), FVector3f(220.f, 70.f, 106.f), 5, 0);      // the bonnet
		AddBox(Model, FVector3f(-220.f, -70.f, 100.f), FVector3f(-120.f, 70.f, 106.f), 6, 0);    // the boot
		AddBox(Model, FVector3f(40.f, -98.f, 100.f), FVector3f(100.f, -96.f, 125.f), 7, 0);      // the driver's window, under the door's bone
		AddBox(Model, FVector3f(55.f, -98.f, 90.f), FVector3f(65.f, -96.f, 94.f), 8, 0);         // the handle
		CloseSections(Model, { 9 * 12 });
		return Model;
	}

	/** How many vertices of Body's first LOD ride Bone. */
	int32 VerticesOn(const FMantleSkinnedMesh& Body, const int32 Bone)
	{
		int32 Count = 0;
		const FMantleSkinnedMeshLOD& LOD = Body.LODs[0];
		for (int32 Vertex = 0; Vertex < LOD.NumVertices(); ++Vertex)
		{
			Count += LOD.InfluenceBones[Vertex] == Bone ? 1 : 0;
		}
		return Count;
	}

	const FMantleVehicleSkinnedHinge* FindHinge(const FMantleVehicleSkinned& Skinned, const EMantleVehicleHingePart Part)
	{
		return Skinned.Hinges.FindByPredicate([Part](const FMantleVehicleSkinnedHinge& Hinge) { return Hinge.Part == Part; });
	}

	/** A skeletal model with only bones (no triangles), for the name suggestions. */
	FMantleVehicleModel NamedBones(const TArray<FString>& Names)
	{
		using namespace MantleVehicleChassisSkinTestPrivate;
		TArray<TPair<FName, int32>> Bones;
		TArray<FVector> Positions;
		Bones.Add({ FName(TEXT("root")), INDEX_NONE });
		Positions.Add(FVector::ZeroVector);
		for (const FString& Name : Names)
		{
			Bones.Add({ FName(*Name), 0 });
			Positions.Add(FVector::ZeroVector);
		}
		return SkeletalModel(Bones, Positions);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleSuggestHingesTest,
	"Mantle.Vehicle.ChassisSkin.SuggestHingesFromNames",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleSuggestHingesTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleHingeTestPrivate;
	const auto Is = [this](const FMantleVehicleHingeMap& Map, const EMantleVehicleHingePart Part, const TCHAR* Bone, const TCHAR* What)
	{
		TestEqual(*FString::Printf(TEXT("%s is the bone %s"), What, Bone), Map.BoneOf(Part).ToString(), FString(Bone));
	};

	// GTA's names.
	const FMantleVehicleHingeMap Gta = FMantleVehicleChassisSkin::SuggestHinges(NamedBones({ TEXT("chassis"), TEXT("door_dside_f"), TEXT("door_pside_f"), TEXT("door_dside_r"),
		TEXT("door_pside_r"), TEXT("bonnet"), TEXT("boot"), TEXT("wheel_lf") }));
	TestEqual(TEXT("all six are found from GTA's names"), Gta.Bones.Num(), 6);
	Is(Gta, EMantleVehicleHingePart::DriverDoor, TEXT("door_dside_f"), TEXT("the driver door"));
	Is(Gta, EMantleVehicleHingePart::PassengerDoor, TEXT("door_pside_f"), TEXT("the passenger door"));
	Is(Gta, EMantleVehicleHingePart::RearLeftDoor, TEXT("door_dside_r"), TEXT("the rear left door"));
	Is(Gta, EMantleVehicleHingePart::RearRightDoor, TEXT("door_pside_r"), TEXT("the rear right door"));
	Is(Gta, EMantleVehicleHingePart::Hood, TEXT("bonnet"), TEXT("the hood"));
	Is(Gta, EMantleVehicleHingePart::Trunk, TEXT("boot"), TEXT("the trunk"));
	TestEqual(TEXT("nothing is flipped by a guess"), Gta.Flipped.Num(), 0);

	// The usual names, in any case, with a number or a dummy on the end.
	const FMantleVehicleHingeMap Plain = FMantleVehicleChassisSkin::SuggestHinges(NamedBones({ TEXT("Door_FL"), TEXT("door_fr.001"), TEXT("Door_RL_dummy"), TEXT("DOOR_RR"), TEXT("Hood"),
		TEXT("Tailgate") }));
	TestEqual(TEXT("all six are found from the usual names"), Plain.Bones.Num(), 6);
	Is(Plain, EMantleVehicleHingePart::DriverDoor, TEXT("Door_FL"), TEXT("the driver door"));
	Is(Plain, EMantleVehicleHingePart::PassengerDoor, TEXT("door_fr.001"), TEXT("the passenger door"));
	Is(Plain, EMantleVehicleHingePart::RearLeftDoor, TEXT("Door_RL_dummy"), TEXT("the rear left door"));
	Is(Plain, EMantleVehicleHingePart::RearRightDoor, TEXT("DOOR_RR"), TEXT("the rear right door"));
	Is(Plain, EMantleVehicleHingePart::Hood, TEXT("Hood"), TEXT("the hood"));
	Is(Plain, EMantleVehicleHingePart::Trunk, TEXT("Tailgate"), TEXT("the trunk"));
	const FMantleVehicleHingeMap Swapped = FMantleVehicleChassisSkin::SuggestHinges(NamedBones({ TEXT("door_lf"), TEXT("door_rf"), TEXT("door_lr"), TEXT("trunk") }));
	Is(Swapped, EMantleVehicleHingePart::DriverDoor, TEXT("door_lf"), TEXT("the driver door by door_lf"));
	Is(Swapped, EMantleVehicleHingePart::PassengerDoor, TEXT("door_rf"), TEXT("the passenger door by door_rf"));
	Is(Swapped, EMantleVehicleHingePart::RearLeftDoor, TEXT("door_lr"), TEXT("the rear left door by door_lr"));
	Is(Swapped, EMantleVehicleHingePart::Trunk, TEXT("trunk"), TEXT("the trunk"));

	// Negative controls: a handle or a frame is not a door, a static model has no bones, and the root is never a door.
	const FMantleVehicleHingeMap Handles = FMantleVehicleChassisSkin::SuggestHinges(NamedBones({ TEXT("doorhandle"), TEXT("door_handle_fl"), TEXT("doorframe"), TEXT("hoodornament"),
		TEXT("trunk_latch") }));
	TestTrue(TEXT("doorhandle, door_handle_fl, doorframe and the like are not parts that open"), Handles.IsEmpty());
	FMantleVehicleModel Static = NamedBones({ TEXT("door_dside_f") });
	Static.bSkeletal = false;
	TestTrue(TEXT("a model that is not skeletal has nothing to open"), FMantleVehicleChassisSkin::SuggestHinges(Static).IsEmpty());
	FMantleVehicleModel RootDoor = NamedBones({});
	RootDoor.Bones[0].Name = FName(TEXT("door_dside_f"));
	TestTrue(TEXT("the root bone is never picked"), FMantleVehicleChassisSkin::SuggestHinges(RootDoor).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleSkinKeepsHingesTest,
	"Mantle.Vehicle.ChassisSkin.SkinKeepsDoorBones",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleSkinKeepsHingesTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleChassisSkinTestPrivate;
	using namespace MantleVehicleHingeTestPrivate;
	FMantleChassisSpec Spec;
	FMantleVehicleAsset Gray;
	if (!Load(*this, TEXT("sedan"), Spec, Gray))
	{
		return false;
	}
	const TArray<FMantleVehiclePartSlot> Slots = FMantleVehicleChassisSkin::PartSlots(Spec, Gray.Body);
	const FMantleVehicleModel Model = HingeCar();
	FMantleVehicleModelMap Map;
	Map.Hinges = FMantleVehicleChassisSkin::SuggestHinges(Model);
	TestEqual(TEXT("the model's six parts are found by name"), Map.Hinges.Bones.Num(), 6);
	const FMantleVehicleModelPlacement Placement = FMantleVehicleChassisSkin::FitPlacement(Spec, Gray.Body, Model, 0.0, Map);
	FMantleVehicleSkinned Skinned;
	FString Error;
	if (!TestTrue(*FString::Printf(TEXT("the model skins (%s)"), *Error), FMantleVehicleChassisSkin::Skin(Gray.Body, Slots, Model, Placement, Map, Skinned, Error)))
	{
		return false;
	}

	// The chassis's bones are all still there, in order, and the six model bones come after them, each a child of the root.
	const FMantleSkinnedMesh& Body = Skinned.Body;
	TestEqual(TEXT("six bones were added to the chassis's"), Body.Bones.Num(), Gray.Body.Bones.Num() + 6);
	bool bChassisKept = true;
	for (int32 Bone = 0; Bone < Gray.Body.Bones.Num(); ++Bone)
	{
		bChassisKept &= Body.Bones[Bone].Name == Gray.Body.Bones[Bone].Name && Body.Bones[Bone].ParentIndex == Gray.Body.Bones[Bone].ParentIndex;
	}
	TestTrue(TEXT("the chassis's own bones keep their order and parents"), bChassisKept);
	TestEqual(TEXT("six hinges come out"), Skinned.Hinges.Num(), 6);
	const TCHAR* const Names[] = { TEXT("door_dside_f"), TEXT("door_pside_f"), TEXT("door_dside_r"), TEXT("door_pside_r"), TEXT("bonnet"), TEXT("boot") };
	const int32 Expected[] = { 2 * 24, 24, 24, 24, 24, 24 };
	for (int32 Index = 0; Index < 6; ++Index)
	{
		const int32 Bone = Body.Bones.IndexOfByPredicate([&Names, Index](const FMantleSkinnedMeshBone& Candidate) { return Candidate.Name == FName(Names[Index]); });
		if (!TestTrue(*FString::Printf(TEXT("the body has a bone %s"), Names[Index]), Bone >= Gray.Body.Bones.Num()))
		{
			continue;
		}
		TestEqual(*FString::Printf(TEXT("%s hangs from the root"), Names[Index]), Body.Bones[Bone].ParentIndex, 0);
		TestEqual(*FString::Printf(TEXT("%s's vertices ride it (a window under a door's bone rides it too)"), Names[Index]), VerticesOn(Body, Bone), Expected[Index]);
		TestTrue(*FString::Printf(TEXT("%s has no turn in its pose"), Names[Index]), Body.ReferencePose[Bone].GetRotation().Equals(FQuat::Identity, 0.0001));
	}
	TestEqual(TEXT("the body, the door handle and the chassis's own bones carry the rest"), VerticesOn(Body, 0), 2 * 24);

	// The hinges: where they turn, which way, and where someone stands to open them.
	const FBox3f Box = Body.ComputeBounds(0);
	const FMantleVehicleSkinnedHinge* Driver = FindHinge(Skinned, EMantleVehicleHingePart::DriverDoor);
	const FMantleVehicleSkinnedHinge* Passenger = FindHinge(Skinned, EMantleVehicleHingePart::PassengerDoor);
	const FMantleVehicleSkinnedHinge* Hood = FindHinge(Skinned, EMantleVehicleHingePart::Hood);
	const FMantleVehicleSkinnedHinge* Trunk = FindHinge(Skinned, EMantleVehicleHingePart::Trunk);
	if (!TestTrue(TEXT("the driver door, passenger door, hood and trunk have hinges"), Driver && Passenger && Hood && Trunk))
	{
		return false;
	}
	TestTrue(TEXT("the driver door turns about an upright axis"), Driver->Axis.Equals(FVector(0, 0, 1)));
	TestTrue(TEXT("the passenger door turns the other way about it"), Passenger->Axis.Equals(FVector(0, 0, -1)));
	TestTrue(TEXT("the hood turns about a sideways axis"), Hood->Axis.Equals(FVector(0, -1, 0)));
	TestTrue(TEXT("and the trunk the other way"), Trunk->Axis.Equals(FVector(0, 1, 0)));
	TestEqual(TEXT("a door turns at three quarters"), Driver->Scale, 0.75);
	TestEqual(TEXT("a lid at one"), Hood->Scale, 1.0);
	const FVector DriverBone = Placement.Point(Model.BonePositions[1]);
	TestTrue(*FString::Printf(TEXT("the door's hinge is its bone, which sits at its front (%s against %s)"), *Driver->Pivot.ToString(), *DriverBone.ToString()), Driver->Pivot.Equals(DriverBone, 0.5));
	TestTrue(TEXT("the pose of its bone is the hinge"), Body.ReferencePose[Body.Bones.IndexOfByPredicate([](const FMantleSkinnedMeshBone& Candidate) { return Candidate.Name == FName(TEXT("door_dside_f")); })]
		.GetLocation().Equals(Driver->Pivot, 0.01));
	TestTrue(TEXT("the hood's hinge is at its rear edge, on top"), Hood->Pivot.X < Box.Max.X && Hood->Pivot.Z > 0.0 && FMath::Abs(Hood->Pivot.Y) < 1.0);
	TestTrue(TEXT("the trunk's hinge is at its front edge, on top"), Trunk->Pivot.X < Hood->Pivot.X && Trunk->Pivot.Z > 0.0);
	TestEqual(TEXT("everyone stands on the ground"), Driver->Stand.Z + Passenger->Stand.Z + Hood->Stand.Z + Trunk->Stand.Z, 0.0);
	TestTrue(*FString::Printf(TEXT("the driver door's stand is 60 cm out on the left, facing the car (%s, yaw %.0f)"), *Driver->Stand.ToString(), Driver->Yaw),
		FMath::IsNearlyEqual(Driver->Stand.Y, static_cast<double>(Box.Min.Y) - 60.0, 0.5) && Driver->Yaw == 90.0);
	TestTrue(TEXT("the passenger door's is 60 cm out on the right, facing the car"), FMath::IsNearlyEqual(Passenger->Stand.Y, static_cast<double>(Box.Max.Y) + 60.0, 0.5) && Passenger->Yaw == -90.0);
	TestTrue(TEXT("the hood's is 60 cm in front, facing back"), FMath::IsNearlyEqual(Hood->Stand.X, static_cast<double>(Box.Max.X) + 60.0, 0.5) && Hood->Yaw == 180.0);
	TestTrue(TEXT("the trunk's is 60 cm behind, facing forward"), FMath::IsNearlyEqual(Trunk->Stand.X, static_cast<double>(Box.Min.X) - 60.0, 0.5) && Trunk->Yaw == 0.0);

	// Flipped, the hood turns the other way and nothing else changes.
	FMantleVehicleModelMap Flipped = Map;
	Flipped.Hinges.Flipped.Add(EMantleVehicleHingePart::Hood);
	FMantleVehicleSkinned Again;
	if (TestTrue(TEXT("the model skins again flipped"), FMantleVehicleChassisSkin::Skin(Gray.Body, Slots, Model, Placement, Flipped, Again, Error)))
	{
		const FMantleVehicleSkinnedHinge* FlippedHood = FindHinge(Again, EMantleVehicleHingePart::Hood);
		const FMantleVehicleSkinnedHinge* SameTrunk = FindHinge(Again, EMantleVehicleHingePart::Trunk);
		TestTrue(TEXT("the flipped hood's axis is reversed"), FlippedHood && FlippedHood->Axis.Equals(FVector(0, 1, 0)));
		TestTrue(TEXT("the trunk's is not"), SameTrunk && SameTrunk->Axis.Equals(FVector(0, 1, 0)));
	}

	// Negative controls: a bone the model does not have opens nothing and says so; a bone the chassis already has the name of is renamed, not duplicated.
	FMantleVehicleModelMap Missing;
	Missing.Hinges.Bones.Add(EMantleVehicleHingePart::Hood, FName(TEXT("no_such_bone")));
	FMantleVehicleSkinned None;
	if (TestTrue(TEXT("a model skins with a bone that is not there"), FMantleVehicleChassisSkin::Skin(Gray.Body, Slots, Model, Placement, Missing, None, Error)))
	{
		TestEqual(TEXT("it adds no bone"), None.Body.Bones.Num(), Gray.Body.Bones.Num());
		TestEqual(TEXT("it makes no hinge"), None.Hinges.Num(), 0);
		TestTrue(TEXT("and says why"), None.Notes.ContainsByPredicate([](const FString& Note) { return Note.Contains(TEXT("no_such_bone")); }));
	}
	FMantleVehicleModel Clash = Model;
	Clash.Bones[5].Name = Gray.Body.Bones.Last().Name;
	FMantleVehicleModelMap ClashMap;
	ClashMap.Hinges.Bones.Add(EMantleVehicleHingePart::Hood, Clash.Bones[5].Name);
	FMantleVehicleSkinned Renamed;
	if (TestTrue(TEXT("a model with a bone named like a chassis bone skins"), FMantleVehicleChassisSkin::Skin(Gray.Body, Slots, Clash, Placement, ClashMap, Renamed, Error)))
	{
		const FMantleVehicleSkinnedHinge* ClashHood = FindHinge(Renamed, EMantleVehicleHingePart::Hood);
		TestTrue(TEXT("its hinge bone has a name of its own"), ClashHood && ClashHood->Bone != Gray.Body.Bones.Last().Name && Renamed.Body.Bones.Num() == Gray.Body.Bones.Num() + 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleHingesPublishTest,
	"Mantle.Vehicle.ChassisSkin.AllSixHingesPublish",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleHingesPublishTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleChassisSkinTestPrivate;
	using namespace MantleVehicleHingeTestPrivate;
	FMantleChassisSpec Spec;
	FMantleVehicleAsset Gray;
	if (!Load(*this, TEXT("sedan"), Spec, Gray))
	{
		return false;
	}
	const TArray<FMantleVehiclePartSlot> Slots = FMantleVehicleChassisSkin::PartSlots(Spec, Gray.Body);
	const FMantleVehicleModel Model = HingeCar();
	FMantleVehicleModelMap Map;
	Map.Hinges = FMantleVehicleChassisSkin::SuggestHinges(Model);
	const FMantleVehicleModelPlacement Placement = FMantleVehicleChassisSkin::FitPlacement(Spec, Gray.Body, Model, 0.0, Map);
	FMantleVehicleSkinned Skinned;
	FString Error;
	if (!TestTrue(*FString::Printf(TEXT("the model skins (%s)"), *Error), FMantleVehicleChassisSkin::Skin(Gray.Body, Slots, Model, Placement, Map, Skinned, Error)))
	{
		return false;
	}
	// The body as a mesh, as a build makes it, and a vehicle that wears it with both seats.
	const FString MeshName = FString::Printf(TEXT("HingeBody%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	USkeletalMesh* Mesh = FMantleVehicleChassisMesh::Create(Skinned.Body, TEXT("/Temp/MantleHingeTest"), MeshName, Error, &Skinned.Materials, &Skinned.Extras);
	if (!TestNotNull(*FString::Printf(TEXT("the body becomes a mesh (%s)"), *Error), Mesh))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		if (USkeleton* Skeleton = Mesh->GetSkeleton())
		{
			Skeleton->ClearFlags(RF_Public | RF_Standalone);
			Skeleton->MarkAsGarbage();
		}
		Mesh->ClearFlags(RF_Public | RF_Standalone);
		Mesh->MarkAsGarbage();
	};
	UMantleVehicleSource* Source = NewObject<UMantleVehicleSource>(GetTransientPackage(), NAME_None, RF_Transient);
	Source->Body = Mesh;
	Source->Tuning.bSeatLocation = true;
	Source->Tuning.bPassengerSeat = true;
	TArray<FString> Notes;
	FMantleVehicleBuilder::ApplyHinges(*Source, Skinned.Hinges, Notes);
	const FMantleVehicleSourceTuning& T = Source->Tuning;
	TestTrue(TEXT("all six are on"), T.bHasDriverDoor && T.bHasPassengerDoor && T.bRearLeftDoor && T.bRearRightDoor && T.bHood && T.bTrunk);
	TestEqual(TEXT("the driver door is on its bone"), T.DriverDoor.Bone.ToString(), FString(TEXT("door_dside_f")));
	TestEqual(TEXT("the hood's stand faces back"), T.HoodYaw, 180.f);
	TestEqual(TEXT("the rear right door's stand faces the car"), T.RearRightDoorYaw, -90.f);
	TestTrue(TEXT("the build says what opens, in one plain sentence"), Notes.ContainsByPredicate([](const FString& Note)
	{
		return Note.Contains(TEXT("driver door")) && Note.Contains(TEXT("hood")) && Note.Contains(TEXT("trunk")) && Note.EndsWith(TEXT("open in the game."));
	}));

	// The check finds nothing wrong with any of them (the vehicle has no wheels and so on, which are not what is asked here).
	const auto AboutParts = [](const FMantleValidationIssue& Issue)
	{
		return Issue.Problem.Contains(TEXT("Door")) || Issue.Problem.Contains(TEXT("Hood")) || Issue.Problem.Contains(TEXT("Trunk")) || Issue.Problem.Contains(TEXT("hinge"));
	};
	TArray<FMantleValidationIssue> Issues;
	FMantleVehicleConverter::CheckSource(Source, FString(), Issues);
	TestFalse(TEXT("the check has nothing to say about the doors, the hood or the trunk"), Issues.ContainsByPredicate(AboutParts));

	// Negative control, so that silence above means something: a hinge on a bone the body does not have is an error.
	Source->Tuning.DriverDoor.Bone = FName(TEXT("no_such_bone"));
	Issues.Reset();
	FMantleVehicleConverter::CheckSource(Source, FString(), Issues);
	TestTrue(TEXT("a door on a bone the body does not have is refused"), Issues.ContainsByPredicate([](const FMantleValidationIssue& Issue)
	{
		return Issue.Severity == EMantleIssueSeverity::Error && Issue.Problem.Contains(TEXT("Driver Door")) && Issue.Problem.Contains(TEXT("does not have"));
	}));
	Source->Tuning.DriverDoor.Bone = FName(TEXT("door_dside_f"));

	// The document carries each, in the places the schema has them.
	FString Why;
	const TSharedRef<FJsonObject> Doc = FMantleVehicleConverter::MakeDocument(*Source, TMap<FString, FString>(), &Why);
	const TSharedPtr<FJsonObject>* Seat = nullptr;
	const TSharedPtr<FJsonObject>* Passenger = nullptr;
	TestTrue(TEXT("seat.door is written"), Doc->TryGetObjectField(TEXT("seat"), Seat) && Seat && (*Seat)->HasField(TEXT("door")));
	TestTrue(TEXT("passengerSeat.door is written"), Doc->TryGetObjectField(TEXT("passengerSeat"), Passenger) && Passenger && (*Passenger)->HasField(TEXT("door")));
	const auto HasOpening = [this, &Doc](const TCHAR* Key, const TCHAR* Hinge)
	{
		const TSharedPtr<FJsonObject>* Part = nullptr;
		const bool bThere = Doc->TryGetObjectField(Key, Part) && Part;
		TestTrue(*FString::Printf(TEXT("%s is written"), Key), bThere);
		if (bThere)
		{
			TestTrue(*FString::Printf(TEXT("%s has its hinge (%s), stand and yaw"), Key, Hinge), (*Part)->HasField(Hinge) && (*Part)->HasField(TEXT("stand")) && (*Part)->HasField(TEXT("yaw")));
		}
	};
	HasOpening(TEXT("rearLeftDoor"), TEXT("door"));
	HasOpening(TEXT("rearRightDoor"), TEXT("door"));
	HasOpening(TEXT("hood"), TEXT("lid"));
	HasOpening(TEXT("trunk"), TEXT("lid"));

	// A part left out stays out of the document.
	Source->Tuning.bHood = false;
	const TSharedRef<FJsonObject> Without = FMantleVehicleConverter::MakeDocument(*Source, TMap<FString, FString>(), &Why);
	TestFalse(TEXT("a hood that is off is not written"), Without->HasField(TEXT("hood")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
