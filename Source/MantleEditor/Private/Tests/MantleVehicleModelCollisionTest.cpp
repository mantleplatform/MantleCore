// Copyright Joshua Eger. All Rights Reserved.
#include "MantleSkinnedMesh.h"
#include "MantleVehicleAsset.h"
#include "MantleVehicleChassisBuilder.h"
#include "MantleVehicleChassisFit.h"
#include "MantleVehicleChassisSkin.h"

#include "Engine/SkeletalMesh.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// The collision of a vehicle built from a creator's model: shaped like the model and not the grey chassis's one box. A plane-shaped model gets a fuselage, wings
// and a tail; a car-shaped one the chassis's box fitted to its hull with boxes over the cabin; a model that came with collision shapes of its own keeps them, but for its
// wheels. Every box stays inside the document's limits and no lower than the chassis's own box. The models are boxes made in the test, as arrays.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVehicleModelCollisionTestPrivate
{
	/** A flat-shaded box (four vertices a face) on model bone Bone, appended to the model's one section. */
	void AddModelBox(FMantleVehicleModel& Model, const FVector3f& Min, const FVector3f& Max, const int32 Bone)
	{
		FMantleSkinnedMeshLOD& LOD = Model.LOD;
		const FVector3f Centre = 0.5f * (Min + Max);
		const FVector3f Half = 0.5f * (Max - Min);
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
				const FVector3f FaceCentre = Centre + Normal * Half[Axis];
				const float Du = Half[(Axis + 1) % 3];
				const float Dv = Half[(Axis + 2) % 3];
				const FVector3f Corners[4] = { FaceCentre - U * Du - V * Dv, FaceCentre + U * Du - V * Dv, FaceCentre + U * Du + V * Dv, FaceCentre - U * Du + V * Dv };
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

	FMantleVehicleModel NewModel(const TCHAR* Name)
	{
		FMantleVehicleModel Model;
		Model.Name = Name;
		Model.Materials.Add(FSkeletalMaterial(static_cast<UMaterialInterface*>(nullptr), FName(TEXT("Skin"))));
		return Model;
	}

	/** One section over every triangle added so far. */
	void FinishModel(FMantleVehicleModel& Model)
	{
		FMantleSkinnedMeshLOD& LOD = Model.LOD;
		LOD.Sections.Reset();
		FMantleSkinnedMeshSection& Section = LOD.Sections.AddDefaulted_GetRef();
		Section.MaterialIndex = 0;
		Section.FirstIndex = 0;
		Section.IndexCount = LOD.Indices.Num();
		Section.MaterialName = FName(TEXT("Skin"));
		LOD.NumInfluences = 1;
	}

	struct FSkinnedModel
	{
		FMantleChassisSpec Spec;
		TArray<FMantleVehiclePartSlot> Slots;
		FMantleVehicleModelPlacement Placement;
		FMantleVehicleSkinned Skinned;
		FMantleChassisPlacement Tub;
		double Floor = 0.0;
	};

	/** The model put on PresetId's chassis, fitted to its footprint (bFit) or as it is (an identity placement). */
	bool PutOnChassis(FAutomationTestBase& Test, const TCHAR* PresetId, const FMantleVehicleModel& Model, const FMantleVehicleModelMap& Map, const bool bFit, FSkinnedModel& Out)
	{
		FMantleVehicleAsset Grey;
		FString Error;
		if (!Test.TestTrue(*FString::Printf(TEXT("the %s chassis loads (%s)"), PresetId, *Error),
				FMantleVehicleChassisBuilder::LoadSpec(PresetId, Out.Spec, Error) && FMantleVehicleChassisBuilder::MakeAsset(Out.Spec, Grey, Error)))
		{
			return false;
		}
		Out.Slots = FMantleVehicleChassisSkin::PartSlots(Out.Spec, Grey.Body);
		Out.Placement = bFit ? FMantleVehicleChassisSkin::FitPlacement(Out.Spec, Grey.Body, Model, 0.0, Map) : FMantleVehicleModelPlacement();
		Out.Tub = FMantleVehicleChassisFit::Place(FMantleVehicleChassisBuilder::ReferenceOf(Out.Spec), FMantleChassisStretch());
		Out.Floor = Out.Tub.CollisionCenter.Z - Out.Tub.CollisionExtent.Z;
		return Test.TestTrue(*FString::Printf(TEXT("the model is put on the %s (%s)"), PresetId, *Error),
			FMantleVehicleChassisSkin::Skin(Grey.Body, Out.Slots, Model, Out.Placement, Map, Out.Skinned, Error));
	}

	bool Covers(const TArray<FMantleChassisBox>& Boxes, const FVector& Point, const double Tolerance)
	{
		for (const FMantleChassisBox& Box : Boxes)
		{
			const FVector Off = (Point - Box.Center).GetAbs();
			const FVector Reach = Box.Extent + FVector(Tolerance);
			if (Off.X <= Reach.X && Off.Y <= Reach.Y && Off.Z <= Reach.Z)
			{
				return true;
			}
		}
		return false;
	}

	double SumOfVolumes(const TArray<FMantleChassisBox>& Boxes)
	{
		double Sum = 0.0;
		for (const FMantleChassisBox& Box : Boxes)
		{
			Sum += 8.0 * Box.Extent.X * Box.Extent.Y * Box.Extent.Z;
		}
		return Sum;
	}

	/** True when every box is inside the document's limits (centre within +-1000, extent 1 to 1000 on each axis) and no lower than Floor. */
	bool InsideLimits(const TArray<FMantleChassisBox>& Boxes, const double Floor)
	{
		for (const FMantleChassisBox& Box : Boxes)
		{
			if (Box.Center.GetAbsMax() > 1000.0 + 1.0e-6 || Box.Extent.GetMin() < 1.0 - 1.0e-6 || Box.Extent.GetMax() > 1000.0 + 1.0e-6 || Box.Center.Z - Box.Extent.Z < Floor - 1.0e-6)
			{
				return false;
			}
		}
		return true;
	}

	/** A box as eight shared vertices and twelve triangles. */
	void AddBoxTriangles(TArray<FVector3f>& Positions, TArray<uint32>& Indices, const FVector3f& Min, const FVector3f& Max)
	{
		const uint32 First = static_cast<uint32>(Positions.Num());
		for (int32 Corner = 0; Corner < 8; ++Corner)
		{
			Positions.Add(FVector3f((Corner & 1) ? Max.X : Min.X, (Corner & 2) ? Max.Y : Min.Y, (Corner & 4) ? Max.Z : Min.Z));
		}
		const uint32 Faces[36] = { 0, 1, 3, 0, 3, 2, 4, 6, 7, 4, 7, 5, 0, 4, 5, 0, 5, 1, 2, 3, 7, 2, 7, 6, 0, 2, 6, 0, 6, 4, 1, 5, 7, 1, 7, 3 };
		for (const uint32 Corner : Faces)
		{
			Indices.Add(First + Corner);
		}
	}

	bool Within(const FBox& Outer, const FVector3f& Point, const double Tolerance)
	{
		return Outer.ExpandBy(Tolerance).IsInside(FVector(Point));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleModelCollisionPlaneTest,
	"Mantle.Vehicle.ChassisSkin.PlaneGetsAirframeCollision",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleModelCollisionPlaneTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleModelCollisionTestPrivate;
	// A plane-shaped static model with no collision of its own: a fuselage, a wing 14.5 m across, a tailplane, a fin, and a nose gear leg that stands on the ground below
	// the fuselage, 11.5 m long like the plane chassis.
	FMantleVehicleModel Model = NewModel(TEXT("Plane"));
	AddModelBox(Model, FVector3f(-880.f, -55.f, 120.f), FVector3f(270.f, 55.f, 280.f), 0);
	AddModelBox(Model, FVector3f(-230.f, -725.f, 245.f), FVector3f(-90.f, 725.f, 259.f), 0);
	AddModelBox(Model, FVector3f(-860.f, -200.f, 200.f), FVector3f(-785.f, 200.f, 208.f), 0);
	AddModelBox(Model, FVector3f(-860.f, -6.f, 280.f), FVector3f(-760.f, 6.f, 480.f), 0);
	AddModelBox(Model, FVector3f(180.f, -10.f, 0.f), FVector3f(220.f, 10.f, 120.f), 0);
	FinishModel(Model);

	FSkinnedModel Put;
	if (!PutOnChassis(*this, TEXT("plane"), Model, FMantleVehicleModelMap(), /*bFit*/ true, Put))
	{
		return false;
	}
	const FMantleVehicleSkinCollision Collision = FMantleVehicleChassisSkin::ProposeCollision(Put.Spec, Put.Skinned.Body, Put.Slots, Model, Put.Placement, FMantleVehicleModelMap());
	TestTrue(TEXT("a plane with no collision of its own gets an airframe"), Collision.Source == EMantleVehicleCollisionSource::Airframe);
	TestFalse(TEXT("and says so"), Collision.Note.IsEmpty());
	TestTrue(*FString::Printf(TEXT("more than one box (%d), within the document's %d"), Collision.Boxes.Num(), FMantleVehicleLimits::MaxCollisionBoxes),
		Collision.Boxes.Num() > 1 && Collision.Boxes.Num() <= FMantleVehicleLimits::MaxCollisionBoxes);
	TestTrue(TEXT("every box is inside +-1000 cm and no lower than the chassis's own box"), InsideLimits(Collision.Boxes, Put.Floor));

	// The reason: the chassis's one box is a fuselage, narrower than the wing that the model has.
	TestTrue(*FString::Printf(TEXT("the chassis's own box is %.0f cm across, far short of the wings"), 2.0 * Put.Tub.CollisionExtent.Y), Put.Tub.CollisionExtent.Y < 200.0);

	// The boxes cover the wings (all four corners of each tip), the tailplane's tips and the fin's top, once the model is placed.
	const double Tolerance = 0.6;
	int32 Uncovered = 0;
	for (const float X : { -230.f, -90.f })
	{
		for (const float Y : { -725.f, 725.f })
		{
			Uncovered += Covers(Collision.Boxes, Put.Placement.Point(FVector(X, Y, X < -100.f ? 245.f : 259.f)), Tolerance) ? 0 : 1;
		}
	}
	TestEqual(TEXT("every wing tip corner is inside a box"), Uncovered, 0);
	TestTrue(TEXT("the tailplane's tip is inside a box"), Covers(Collision.Boxes, Put.Placement.Point(FVector(-860.0, 200.0, 208.0)), Tolerance)
		&& Covers(Collision.Boxes, Put.Placement.Point(FVector(-785.0, -200.0, 200.0)), Tolerance));
	TestTrue(TEXT("the top of the fin is inside a box"), Covers(Collision.Boxes, Put.Placement.Point(FVector(-810.0, 0.0, 480.0)), Tolerance));
	// Negative control: a point well above a wing tip is not.
	TestFalse(TEXT("space above a wing tip is not"), Covers(Collision.Boxes, Put.Placement.Point(FVector(-160.0, 700.0, 420.0)), Tolerance));
	// The nose gear leg stands below the chassis's own box: it must not drag, so nothing reaches down to it.
	TestFalse(TEXT("the gear leg below the fuselage is not collision"), Covers(Collision.Boxes, Put.Placement.Point(FVector(200.0, 0.0, 30.0)), Tolerance));

	// The boxes hug the airframe rather than fill the box around it: the same volume the plane draws, not the cube round its wings and fin.
	FBox Around(ForceInit);
	for (const FVector3f& Position : Put.Skinned.Body.LODs[0].Positions)
	{
		Around += FVector(Position);
	}
	Around.Min.Z = FMath::Max(Around.Min.Z, Put.Floor);
	const double Volume = SumOfVolumes(Collision.Boxes);
	TestTrue(*FString::Printf(TEXT("the boxes fill a tenth of the box around the plane at most (%.1f of %.1f million cm3)"), Volume / 1.0e6, Around.GetVolume() / 1.0e6),
		Volume <= 0.1 * Around.GetVolume());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleModelCollisionCarTest,
	"Mantle.Vehicle.ChassisSkin.CarModelGetsHullBoxes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleModelCollisionCarTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleModelCollisionTestPrivate;
	FMantleChassisSpec Sedan;
	FMantleVehicleAsset Grey;
	FString Error;
	if (!TestTrue(*FString::Printf(TEXT("the sedan chassis loads (%s)"), *Error), FMantleVehicleChassisBuilder::LoadSpec(TEXT("sedan"), Sedan, Error) && FMantleVehicleChassisBuilder::MakeAsset(Sedan, Grey, Error)))
	{
		return false;
	}
	const float Length = static_cast<float>(Sedan.Length);
	const float Width = static_cast<float>(Sedan.Width);
	const float TubTop = static_cast<float>(Sedan.Clearance + Sedan.Height);

	// A car-shaped model, the chassis's length, with a tall cabin on its body.
	FMantleVehicleModel WithCabin = NewModel(TEXT("Car"));
	AddModelBox(WithCabin, FVector3f(-0.5f * Length, -0.5f * Width, 0.f), FVector3f(0.5f * Length, 0.5f * Width, TubTop), 0);
	AddModelBox(WithCabin, FVector3f(-0.2f * Length, -0.35f * Width, TubTop - 20.f), FVector3f(0.2f * Length, 0.35f * Width, TubTop + 80.f), 0);
	FinishModel(WithCabin);
	FSkinnedModel Put;
	if (!PutOnChassis(*this, TEXT("sedan"), WithCabin, FMantleVehicleModelMap(), /*bFit*/ true, Put))
	{
		return false;
	}
	const FMantleVehicleSkinCollision Collision = FMantleVehicleChassisSkin::ProposeCollision(Put.Spec, Put.Skinned.Body, Put.Slots, WithCabin, Put.Placement, FMantleVehicleModelMap());
	TestTrue(TEXT("a car model with no collision of its own gets its hull"), Collision.Source == EMantleVehicleCollisionSource::Hull);
	TestTrue(*FString::Printf(TEXT("the chassis's box and a box over the cabin (%d boxes)"), Collision.Boxes.Num()), Collision.Boxes.Num() >= 2 && Collision.Boxes.Num() <= FMantleVehicleLimits::MaxCollisionBoxes);
	TestTrue(TEXT("every box is inside the limits"), InsideLimits(Collision.Boxes, Put.Floor));
	double Top = -1.0e9;
	for (const FMantleChassisBox& Box : Collision.Boxes)
	{
		Top = FMath::Max(Top, Box.Center.Z + Box.Extent.Z);
	}
	const double ModelTop = Put.Placement.Point(FVector(0.0, 0.0, TubTop + 80.0)).Z;
	TestTrue(*FString::Printf(TEXT("the roof is solid: the boxes reach the cabin's top (%.0f of %.0f cm)"), Top, ModelTop), Top >= ModelTop - 0.05 * ModelTop);

	// Negative control: the same car without its cabin has nothing above the chassis's box to add.
	FMantleVehicleModel Low = NewModel(TEXT("LowCar"));
	AddModelBox(Low, FVector3f(-0.5f * Length, -0.5f * Width, 0.f), FVector3f(0.5f * Length, 0.5f * Width, TubTop - 30.f), 0);
	FinishModel(Low);
	FSkinnedModel PutLow;
	if (PutOnChassis(*this, TEXT("sedan"), Low, FMantleVehicleModelMap(), /*bFit*/ true, PutLow))
	{
		const FMantleVehicleSkinCollision Plain = FMantleVehicleChassisSkin::ProposeCollision(PutLow.Spec, PutLow.Skinned.Body, PutLow.Slots, Low, PutLow.Placement, FMantleVehicleModelMap());
		TestEqual(TEXT("without the cabin it is the chassis's box alone"), Plain.Boxes.Num(), 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleModelCollisionPhysicsTest,
	"Mantle.Vehicle.ChassisSkin.ModelPhysicsShapesTheCollision",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleModelCollisionPhysicsTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleModelCollisionTestPrivate;
	const auto Body = [](const FVector& Min, const FVector& Max, const int32 Bone)
	{
		FMantleVehicleModelBody Out;
		Out.Box = FBox(Min, Max);
		Out.Bone = Bone;
		return Out;
	};

	// A model with a simple geometry and collision shapes of its own, put on the sedan as it is (no fit, so its numbers are the chassis's).
	FMantleVehicleModel Model = NewModel(TEXT("Shaped"));
	AddModelBox(Model, FVector3f(-200.f, -80.f, 20.f), FVector3f(200.f, 80.f, 100.f), 0);
	FinishModel(Model);
	FSkinnedModel Put;
	if (!PutOnChassis(*this, TEXT("sedan"), Model, FMantleVehicleModelMap(), /*bFit*/ false, Put))
	{
		return false;
	}
	const double Floor = Put.Floor;
	const FVector Seat = Put.Tub.Seat;

	// Its own shapes are followed: three boxes stay three, and the one the seat sits in comes first though it is the smallest.
	Model.PhysicsBodies.Reset();
	Model.PhysicsBodies.Add(Body(FVector(Seat.X + 100.0, -80.0, Floor + 5.0), FVector(Seat.X + 500.0, 80.0, Floor + 120.0), INDEX_NONE));
	Model.PhysicsBodies.Add(Body(Seat - FVector(30.0), Seat + FVector(30.0), INDEX_NONE));
	Model.PhysicsBodies.Add(Body(FVector(Seat.X - 400.0, -60.0, Floor + 5.0), FVector(Seat.X - 300.0, 60.0, Floor + 60.0), INDEX_NONE));
	FMantleVehicleSkinCollision Shaped = FMantleVehicleChassisSkin::ProposeCollision(Put.Spec, Put.Skinned.Body, Put.Slots, Model, Put.Placement, FMantleVehicleModelMap());
	TestTrue(TEXT("a model with collision shapes keeps them"), Shaped.Source == EMantleVehicleCollisionSource::PhysicsAsset);
	TestEqual(TEXT("three shapes are three boxes"), Shaped.Boxes.Num(), 3);
	TestTrue(TEXT("the box the seat sits in is first"), Shaped.Boxes.Num() > 0 && Covers(TArray<FMantleChassisBox>({ Shaped.Boxes[0] }), Seat, 2.0));
	TestTrue(TEXT("they are inside the limits"), InsideLimits(Shaped.Boxes, Floor));

	// More shapes than the document holds are merged down; a shape reaching past +-1000 cm is held to it.
	Model.PhysicsBodies.Reset();
	for (int32 Index = 0; Index < 12; ++Index)
	{
		Model.PhysicsBodies.Add(Body(FVector(-600.0 + 100.0 * Index, -40.0, Floor + 5.0), FVector(-570.0 + 100.0 * Index, 40.0, Floor + 50.0), INDEX_NONE));
	}
	Model.PhysicsBodies.Add(Body(FVector(0.0, -50.0, Floor + 5.0), FVector(5000.0, 50.0, Floor + 50.0), INDEX_NONE));
	const FMantleVehicleSkinCollision Many = FMantleVehicleChassisSkin::ProposeCollision(Put.Spec, Put.Skinned.Body, Put.Slots, Model, Put.Placement, FMantleVehicleModelMap());
	TestEqual(TEXT("thirteen shapes are merged to the document's eight boxes"), Many.Boxes.Num(), static_cast<int32>(FMantleVehicleLimits::MaxCollisionBoxes));
	TestTrue(TEXT("and the one 5 m long past the limit is held inside it"), InsideLimits(Many.Boxes, Floor));

	// A shape below the chassis's own box is raised to it.
	Model.PhysicsBodies.Reset();
	Model.PhysicsBodies.Add(Body(FVector(-100.0, -60.0, Floor - 40.0), FVector(100.0, 60.0, Floor + 60.0), INDEX_NONE));
	const FMantleVehicleSkinCollision Low = FMantleVehicleChassisSkin::ProposeCollision(Put.Spec, Put.Skinned.Body, Put.Slots, Model, Put.Placement, FMantleVehicleModelMap());
	if (TestEqual(TEXT("one shape is one box"), Low.Boxes.Num(), 1))
	{
		TestTrue(TEXT("raised to the chassis's own box"), FMath::IsNearlyEqual(Low.Boxes[0].Center.Z - Low.Boxes[0].Extent.Z, Floor, 1.0e-3));
	}

	// Wheels are not the body's collision: a body on a bone named like a wheel is left out, and so is one on a bone the creator assigned to a wheel.
	FMantleVehicleModel Skeletal = NewModel(TEXT("Rigged"));
	Skeletal.bSkeletal = true;
	Skeletal.Bones.Add({ FName(TEXT("Root")), INDEX_NONE });
	Skeletal.Bones.Add({ FName(TEXT("WheelFront")), 0 });
	Skeletal.Bones.Add({ FName(TEXT("FL")), 0 });
	Skeletal.BonePositions = { FVector::ZeroVector, FVector::ZeroVector, FVector::ZeroVector };
	AddModelBox(Skeletal, FVector3f(-200.f, -80.f, 20.f), FVector3f(200.f, 80.f, 100.f), 0);
	FinishModel(Skeletal);
	FSkinnedModel PutRigged;
	if (PutOnChassis(*this, TEXT("sedan"), Skeletal, FMantleVehicleModelMap(), /*bFit*/ false, PutRigged))
	{
		const FBox Hull(FVector(-200.0, -80.0, Floor + 5.0), FVector(200.0, 80.0, Floor + 80.0));
		const FBox Tyre(FVector(120.0, -95.0, Floor + 5.0), FVector(190.0, -60.0, Floor + 70.0));
		Skeletal.PhysicsBodies = { Body(Hull.Min, Hull.Max, 0), Body(Tyre.Min, Tyre.Max, 1) };
		const FMantleVehicleSkinCollision ByName = FMantleVehicleChassisSkin::ProposeCollision(PutRigged.Spec, PutRigged.Skinned.Body, PutRigged.Slots, Skeletal, PutRigged.Placement, FMantleVehicleModelMap());
		TestEqual(TEXT("a body on a bone named like a wheel is not collision"), ByName.Boxes.Num(), 1);

		Skeletal.PhysicsBodies = { Body(Hull.Min, Hull.Max, 0), Body(Tyre.Min, Tyre.Max, 2) };
		const FMantleVehicleSkinCollision Control = FMantleVehicleChassisSkin::ProposeCollision(PutRigged.Spec, PutRigged.Skinned.Body, PutRigged.Slots, Skeletal, PutRigged.Placement, FMantleVehicleModelMap());
		TestEqual(TEXT("control: the same body on an unnamed bone is"), Control.Boxes.Num(), 2);

		FMantleVehicleModelMap Assigned;
		Assigned.Parts.Add(FName(TEXT("wheel_fl")), FMantleVehicleModelPiece::OfBone(FName(TEXT("FL"))));
		const FMantleVehicleSkinCollision ByMap = FMantleVehicleChassisSkin::ProposeCollision(PutRigged.Spec, PutRigged.Skinned.Body, PutRigged.Slots, Skeletal, PutRigged.Placement, Assigned);
		TestEqual(TEXT("a body on a bone the creator assigned to the front left wheel is not collision either"), ByMap.Boxes.Num(), 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleModelCollisionSplitTest,
	"Mantle.Vehicle.ChassisSkin.BoxesAroundSurface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleModelCollisionSplitTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleModelCollisionTestPrivate;
	// One solid box stays one box, the box it is.
	{
		TArray<FVector3f> Positions;
		TArray<uint32> Indices;
		AddBoxTriangles(Positions, Indices, FVector3f(-675.f, -540.f, -160.f), FVector3f(675.f, 540.f, 160.f));
		const TArray<FBox> Boxes = FMantleVehicleChassisSkin::BoxesAroundSurface(Positions, Indices, 8);
		if (TestEqual(TEXT("a box is one box"), Boxes.Num(), 1))
		{
			TestTrue(TEXT("and that box is the box"), Boxes[0].Min.Equals(FVector(-675.0, -540.0, -160.0), 1.0e-3) && Boxes[0].Max.Equals(FVector(675.0, 540.0, 160.0), 1.0e-3));
		}
		TestEqual(TEXT("one box asked for is one box, round everything"), FMantleVehicleChassisSkin::BoxesAroundSurface(Positions, Indices, 1).Num(), 1);
		TestEqual(TEXT("nothing to wrap is no box"), FMantleVehicleChassisSkin::BoxesAroundSurface(TConstArrayView<FVector3f>(), TConstArrayView<uint32>(), 8).Num(), 0);
	}

	// An L, a long slab with a tall column at one end, is two or more boxes that together fill much less than the box round it, and every corner is inside one.
	{
		TArray<FVector3f> Positions;
		TArray<uint32> Indices;
		AddBoxTriangles(Positions, Indices, FVector3f(0.f, 0.f, 0.f), FVector3f(1000.f, 200.f, 50.f));
		AddBoxTriangles(Positions, Indices, FVector3f(900.f, 0.f, 50.f), FVector3f(1000.f, 200.f, 500.f));
		const TArray<FBox> Boxes = FMantleVehicleChassisSkin::BoxesAroundSurface(Positions, Indices, 8);
		TestTrue(*FString::Printf(TEXT("an L is more than one box (%d)"), Boxes.Num()), Boxes.Num() >= 2 && Boxes.Num() <= 8);
		double Volume = 0.0;
		for (const FBox& Box : Boxes)
		{
			Volume += Box.GetVolume();
		}
		TestTrue(*FString::Printf(TEXT("filling under half the box round it (%.0f of %.0f)"), Volume, 1000.0 * 200.0 * 500.0), Volume < 0.5 * 1000.0 * 200.0 * 500.0);
		int32 Outside = 0;
		for (const FVector3f& Position : Positions)
		{
			bool bInside = false;
			for (const FBox& Box : Boxes)
			{
				bInside = bInside || Within(Box, Position, 0.01);
			}
			Outside += bInside ? 0 : 1;
		}
		TestEqual(TEXT("and every corner is inside one"), Outside, 0);
	}

	// Merging: twelve boxes in a row are eight, each of the twelve inside one of them; fewer than asked for are left alone.
	{
		TArray<FBox> Row;
		for (int32 Index = 0; Index < 12; ++Index)
		{
			Row.Add(FBox(FVector(100.0 * Index, 0.0, 0.0), FVector(100.0 * Index + 30.0, 40.0, 40.0)));
		}
		const TArray<FBox> Merged = FMantleVehicleChassisSkin::ReduceBoxes(Row, 8);
		TestEqual(TEXT("twelve boxes are merged to eight"), Merged.Num(), 8);
		int32 Lost = 0;
		for (const FBox& Original : Row)
		{
			bool bInside = false;
			for (const FBox& Box : Merged)
			{
				bInside = bInside || (Box.IsInsideOrOn(Original.Min) && Box.IsInsideOrOn(Original.Max));
			}
			Lost += bInside ? 0 : 1;
		}
		TestEqual(TEXT("each of the twelve is inside one"), Lost, 0);
		TestEqual(TEXT("boxes under the limit are left alone"), FMantleVehicleChassisSkin::ReduceBoxes(Row, 20).Num(), 12);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
