// Copyright Joshua Eger. All Rights Reserved.
#include "MantleSkinnedMesh.h"
#include "MantleVehicleAsset.h"
#include "MantleVehicleBodySplit.h"
#include "MantleVehicleChassisMesh.h"
#include "MantleVehicleConverter.h"
#include "MantleVehicleLightFinder.h"
#include "MantleVehicleSource.h"
#include "MantleVehicleScratchBody.h"

#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Materials/Material.h"
#include "Misc/AutomationTest.h"
#include "ReferenceSkeleton.h"

#if WITH_DEV_AUTOMATION_TESTS

// A Fab Chaos vehicle converts with no hand work (#240's follow-up): its wheels are the bones the Chaos wheel setups name, each corner read from where its bone sits, and a body
// with more materials than a published body may have is split into the body and parts, as the builder splits one. The cars here are small skeletal meshes made in the test
// (bones at the wheels and a quad of each material), in throwaway /Temp packages: nothing is on disk.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVehicleConverterWheelsTestPrivate
{
	struct FCarWheel
	{
		const TCHAR* Name;
		FVector Position;
	};

	/** A car's skeleton: a root, then each wheel at its place; material m is m + 1 quads, riding bone m modulo the bones. */
	USkeletalMesh* MakeCar(const TArray<FCarWheel>& Wheels, const int32 NumMaterials, FString& OutError, UMaterialInterface* WithMaterial = nullptr,
		const TArray<FString>& SlotNames = TArray<FString>())
	{
		FMantleSkinnedMesh Body;
		Body.Bones.Add({ FName(TEXT("B_Car_Main")), INDEX_NONE });
		Body.ReferencePose.Add(FTransform::Identity);
		for (const FCarWheel& Wheel : Wheels)
		{
			Body.Bones.Add({ FName(Wheel.Name), 0 });
			Body.ReferencePose.Add(FTransform(Wheel.Position));
		}
		FMantleSkinnedMeshLOD& LOD = Body.LODs.AddDefaulted_GetRef();
		LOD.NumInfluences = 1;
		TArray<FSkeletalMaterial> Materials;
		int32 Quad = 0;
		for (int32 Material = 0; Material < NumMaterials; ++Material)
		{
			const FString NameText = SlotNames.IsValidIndex(Material) && !SlotNames[Material].IsEmpty() ? SlotNames[Material] : FString::Printf(TEXT("mat%d"), Material);
			const FName MaterialName(*NameText);
			Materials.Add(FSkeletalMaterial(WithMaterial, MaterialName));
			FMantleSkinnedMeshSection& Section = LOD.Sections.AddDefaulted_GetRef();
			Section.MaterialIndex = Material;
			Section.MaterialName = MaterialName;
			Section.FirstIndex = LOD.Indices.Num();
			for (int32 Each = 0; Each <= Material; ++Each, ++Quad)
			{
				const uint32 First = static_cast<uint32>(LOD.Positions.Num());
				const float X = 20.0f * Quad;
				LOD.Positions.Add(FVector3f(X, 0.0f, 0.0f));
				LOD.Positions.Add(FVector3f(X + 10.0f, 0.0f, 0.0f));
				LOD.Positions.Add(FVector3f(X + 10.0f, 10.0f, 0.0f));
				LOD.Positions.Add(FVector3f(X, 10.0f, 0.0f));
				for (int32 Corner = 0; Corner < 4; ++Corner)
				{
					LOD.Normals.Add(FVector3f(0.0f, 0.0f, 1.0f));
					LOD.UVs.Add(FVector2f(0.25f * Corner, 0.5f));
					LOD.InfluenceBones.Add(static_cast<uint16>(Material % Body.Bones.Num()));
					LOD.InfluenceWeights.Add(255);
				}
				LOD.Indices.Append({ First, First + 1, First + 2, First, First + 2, First + 3 });
			}
			Section.IndexCount = LOD.Indices.Num() - Section.FirstIndex;
		}
		const FString Name = FString::Printf(TEXT("WheelsCar%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
		return FMantleVehicleChassisMesh::Create(Body, TEXT("/Temp/MantleConvertWheelsTest"), Name, OutError, &Materials);
	}

	TArray<FCarWheel> FourWheels(const TCHAR* FL, const TCHAR* FR, const TCHAR* RL, const TCHAR* RR)
	{
		return { { FL, FVector(130, -80, 35) }, { FR, FVector(130, 80, 35) }, { RL, FVector(-130, -80, 35) }, { RR, FVector(-130, 80, 35) } };
	}

	FMantleVehicleWheelSetup Setup(const TCHAR* Bone)
	{
		FMantleVehicleWheelSetup Out;
		Out.Bone = FName(Bone);
		return Out;
	}

	const FMantleVehicleWheelBone* Find(const TArray<FMantleVehicleWheelBone>& Wheels, const TCHAR* Bone)
	{
		return Wheels.FindByPredicate([Bone](const FMantleVehicleWheelBone& Wheel) { return Wheel.Bone == FName(Bone); });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleConverterWheelsFromSetupsTest,
	"Mantle.Vehicle.Convert.WheelsFromChaosSetups",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleConverterWheelsFromSetupsTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleConverterWheelsTestPrivate;

	// Bones no name rule reads (Tyre_A to Tyre_D), as the Chaos wheel setups name them: each is a wheel, and its corner is where its bone sits.
	FString Error;
	USkeletalMesh* Car = MakeCar(FourWheels(TEXT("Tyre_A"), TEXT("Tyre_B"), TEXT("Tyre_C"), TEXT("Tyre_D")), 2, Error);
	if (!TestNotNull(*FString::Printf(TEXT("the car is made (%s)"), *Error), Car))
	{
		return false;
	}
	const TArray<FMantleVehicleWheelBone> ByName = FMantleVehicleConverter::FindWheelBones(Car->GetRefSkeleton());
	TestEqual(TEXT("by their names there are no wheels"), ByName.Num(), 0);

	// Listed out of order, to show the places decide.
	const TArray<FMantleVehicleWheelSetup> Setups = { Setup(TEXT("Tyre_D")), Setup(TEXT("Tyre_A")), Setup(TEXT("Tyre_C")), Setup(TEXT("Tyre_B")) };
	FString How;
	const TArray<FMantleVehicleWheelBone> Wheels = FMantleVehicleConverter::FindWheelBones(Car->GetRefSkeleton(), Setups, &How);
	if (TestEqual(TEXT("the setups name four wheels"), Wheels.Num(), 4))
	{
		const FMantleVehicleWheelBone* A = Find(Wheels, TEXT("Tyre_A"));
		const FMantleVehicleWheelBone* B = Find(Wheels, TEXT("Tyre_B"));
		const FMantleVehicleWheelBone* C = Find(Wheels, TEXT("Tyre_C"));
		const FMantleVehicleWheelBone* D = Find(Wheels, TEXT("Tyre_D"));
		TestTrue(TEXT("the bone at the front, -Y, is the front left"), A && A->Axle == 0 && A->bLeft && !A->bCenter);
		TestTrue(TEXT("the one at the front, +Y, is the front right"), B && B->Axle == 0 && !B->bLeft && !B->bCenter);
		TestTrue(TEXT("the rear -Y is the rear left"), C && C->Axle == 2 && C->bLeft);
		TestTrue(TEXT("the rear +Y is the rear right"), D && D->Axle == 2 && !D->bLeft);
		TestEqual(TEXT("front to back, left before right"), Wheels[0].Bone.ToString() + Wheels[1].Bone.ToString() + Wheels[2].Bone.ToString() + Wheels[3].Bone.ToString(),
			FString(TEXT("Tyre_ATyre_BTyre_CTyre_D")));
	}
	TestTrue(TEXT("and it says where they came from"), How.Contains(TEXT("wheel settings")));

	// A whole conversion takes them: four wheels, the front ones steering, and a note saying which bones became wheels and why.
	UMantleVehicleSource* Source = NewObject<UMantleVehicleSource>(GetTransientPackage(), NAME_None, RF_Transient);
	FMantleVehicleSurroundings Around;
	Around.WheelSetups = Setups;
	const FMantleVehicleConversion Conversion = FMantleVehicleConverter::Convert(Car, *Source, Around);
	TestTrue(*FString::Printf(TEXT("the car converts (%s)"), *FString::Join(Conversion.Problems, TEXT(" "))), Conversion.bSuccess);
	TestEqual(TEXT("with four wheels"), Source->Wheels.Num(), 4);
	TestTrue(TEXT("the front ones steer"), Source->Wheels.Num() == 4 && Source->Wheels[0].bSteering && Source->Wheels[1].bSteering && !Source->Wheels[2].bSteering);
	TestTrue(TEXT("the notes say which bones became wheels, and that the settings said so"), Conversion.Notes.ContainsByPredicate([](const FString& Note)
	{
		return Note.StartsWith(TEXT("Wheels: ")) && Note.Contains(TEXT("Tyre_A (front left)")) && Note.Contains(TEXT("Tyre_D (rear right)")) && Note.Contains(TEXT("wheel settings"));
	}));

	// A setup that names a bone the car does not have: the names decide (and there are none here), and it says so.
	FString FellBack;
	const TArray<FMantleVehicleWheelSetup> Wrong = { Setup(TEXT("Tyre_A")), Setup(TEXT("Not_A_Bone")) };
	TestEqual(TEXT("a setup for a bone the car lacks leaves the names to decide"), FMantleVehicleConverter::FindWheelBones(Car->GetRefSkeleton(), Wrong, &FellBack).Num(), 0);
	TestTrue(TEXT("and says so"), FellBack.Contains(TEXT("name a bone this model does not have")));

	MantleVehicleScratchBody::Discard(Car);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleConverterWheelsFromNamesTest,
	"Mantle.Vehicle.Convert.WheelsFromNamesStillWork",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleConverterWheelsFromNamesTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleConverterWheelsTestPrivate;

	// With no setups the names decide: the corner before "wheel" (FLWheel, B_FL_Wheel1), after it (WheelFL), and the older spellings.
	const auto Reads = [this](const TCHAR* Bone, const int32 Axle, const bool bLeft, const bool bCenter)
	{
		FMantleVehicleWheelBone Wheel;
		if (TestTrue(*FString::Printf(TEXT("%s is a wheel"), Bone), FMantleVehicleConverter::ParseWheelBoneName(Bone, Wheel)))
		{
			TestTrue(*FString::Printf(TEXT("%s: axle %d, %s%s"), Bone, Axle, bCenter ? TEXT("center") : (bLeft ? TEXT("left") : TEXT("right")), TEXT("")),
				Wheel.Axle == Axle && Wheel.bLeft == bLeft && Wheel.bCenter == bCenter && Wheel.Bone == FName(Bone));
		}
	};
	Reads(TEXT("B_SUV04_FLWheel"), 0, true, false);
	Reads(TEXT("B_SUV04_RRWheel"), 2, false, false);
	Reads(TEXT("B_SUV01_FRWheel"), 0, false, false);
	Reads(TEXT("B_NYC_Car02_WheelFL"), 0, true, false);
	Reads(TEXT("B_NYC_Car02_WheelRR"), 2, false, false);
	Reads(TEXT("B_Wheel_FL"), 0, true, false);
	Reads(TEXT("B_FL_Wheel1"), 0, true, false);
	Reads(TEXT("B_FR_Wheel"), 0, false, false);
	Reads(TEXT("B_RL_Wheel2"), 2, true, false);
	Reads(TEXT("FrontLeftWheel"), 0, true, false);
	Reads(TEXT("B_FrontWheel"), 0, false, true);
	Reads(TEXT("B_RearWheel"), 2, false, true);
	// What the recipe's naming read before still reads the same.
	Reads(TEXT("PhysWheel_FL"), 0, true, false);
	Reads(TEXT("Phys_Wheel_BR"), 2, false, false);
	Reads(TEXT("wheel_front_l"), 0, true, false);
	Reads(TEXT("wheel_lf"), 0, true, false);
	Reads(TEXT("wheel_f"), 0, false, true);

	// Not wheels: the brake beside a wheel, a hub, a door.
	FMantleVehicleWheelBone Not;
	TestFalse(TEXT("B_SUV04_FLBrake is not a wheel"), FMantleVehicleConverter::ParseWheelBoneName(TEXT("B_SUV04_FLBrake"), Not));
	TestFalse(TEXT("Wheel_FL_Brake is not a wheel"), FMantleVehicleConverter::ParseWheelBoneName(TEXT("Wheel_FL_Brake"), Not));
	TestFalse(TEXT("B_Door_FL is not a wheel"), FMantleVehicleConverter::ParseWheelBoneName(TEXT("B_Door_FL"), Not));
	TestFalse(TEXT("Hub_FL is not a wheel"), FMantleVehicleConverter::ParseWheelBoneName(TEXT("Hub_FL"), Not));
	TestEqual(TEXT("the corner of B_SUV04_FLWheel is _fl"), FMantleVehicleConverter::CornerOf(TEXT("B_SUV04_FLWheel")), FString(TEXT("_fl")));
	TestEqual(TEXT("the corner of B_FR_Wheel1 is _fr"), FMantleVehicleConverter::CornerOf(TEXT("B_FR_Wheel1")), FString(TEXT("_fr")));
	TestEqual(TEXT("the corner of wheel_rear_l is as it was"), FMantleVehicleConverter::CornerOf(TEXT("wheel_rear_l")), FString(TEXT("_bl")));

	// A car of such bones, with no setups: four wheels, in their corners.
	FString Error;
	USkeletalMesh* Car = MakeCar(FourWheels(TEXT("B_SUV04_FLWheel"), TEXT("B_SUV04_FRWheel"), TEXT("B_SUV04_RLWheel"), TEXT("B_SUV04_RRWheel")), 2, Error);
	if (!TestNotNull(*FString::Printf(TEXT("the car is made (%s)"), *Error), Car))
	{
		return false;
	}
	const TArray<FMantleVehicleWheelBone> Wheels = FMantleVehicleConverter::FindWheelBones(Car->GetRefSkeleton());
	TestEqual(TEXT("four wheels by their names"), Wheels.Num(), 4);
	TestTrue(TEXT("the front left first"), Wheels.Num() == 4 && Wheels[0].Bone == FName(TEXT("B_SUV04_FLWheel")) && Wheels[0].Axle == 0 && Wheels[0].bLeft);
	TestTrue(TEXT("the rear right last"), Wheels.Num() == 4 && Wheels[3].Bone == FName(TEXT("B_SUV04_RRWheel")) && Wheels[3].Axle == 2 && !Wheels[3].bLeft);
	UMantleVehicleSource* Source = NewObject<UMantleVehicleSource>(GetTransientPackage(), NAME_None, RF_Transient);
	const FMantleVehicleConversion Conversion = FMantleVehicleConverter::Convert(Car, *Source);
	TestTrue(*FString::Printf(TEXT("it converts (%s)"), *FString::Join(Conversion.Problems, TEXT(" "))), Conversion.bSuccess && Source->Wheels.Num() == 4);
	TestTrue(TEXT("and the notes say the wheels were found by their names"), Conversion.Notes.ContainsByPredicate([](const FString& Note)
	{
		return Note.StartsWith(TEXT("Wheels: ")) && Note.Contains(TEXT("Found by their names"));
	}));
	MantleVehicleScratchBody::Discard(Car);

	// A scooter: two wheels on the center line, named for their place, or set up by a Chaos vehicle. Either way a front and a rear wheel.
	const TArray<FCarWheel> Scooter = { { TEXT("B_FrontWheel"), FVector(40, 0, 20) }, { TEXT("B_RearWheel"), FVector(-40, 0, 20) } };
	USkeletalMesh* Bike = MakeCar(Scooter, 2, Error);
	if (!TestNotNull(*FString::Printf(TEXT("the scooter is made (%s)"), *Error), Bike))
	{
		return false;
	}
	const TArray<FMantleVehicleWheelBone> ByName = FMantleVehicleConverter::FindWheelBones(Bike->GetRefSkeleton());
	TestTrue(TEXT("by name: a front wheel and a rear one on the center line"), ByName.Num() == 2 && ByName[0].Axle == 0 && ByName[0].bCenter && ByName[1].Axle == 2 && ByName[1].bCenter);
	const TArray<FMantleVehicleWheelSetup> BikeSetups = { Setup(TEXT("B_RearWheel")), Setup(TEXT("B_FrontWheel")) };
	const TArray<FMantleVehicleWheelBone> BySetup = FMantleVehicleConverter::FindWheelBones(Bike->GetRefSkeleton(), BikeSetups);
	TestTrue(TEXT("by setup: the same two, from where they sit"), BySetup.Num() == 2 && BySetup[0].Bone == FName(TEXT("B_FrontWheel")) && BySetup[0].Axle == 0 && BySetup[0].bCenter
		&& BySetup[1].Bone == FName(TEXT("B_RearWheel")) && BySetup[1].Axle == 2 && BySetup[1].bCenter);
	MantleVehicleScratchBody::Discard(Bike);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleConverterMaterialsSplitTest,
	"Mantle.Vehicle.Convert.ExtraMaterialsBecomeParts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleConverterMaterialsSplitTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleConverterWheelsTestPrivate;

	// A car that draws 12 materials, where a published body may have 8: it converts, the body is a copy with 8 and the other 4 ride along as parts.
	FString Error;
	USkeletalMesh* Car = MakeCar(FourWheels(TEXT("B_FLWheel"), TEXT("B_FRWheel"), TEXT("B_RLWheel"), TEXT("B_RRWheel")), 12, Error);
	if (!TestNotNull(*FString::Printf(TEXT("the car is made (%s)"), *Error), Car))
	{
		return false;
	}
	TestEqual(TEXT("the car has 12 materials"), Car->GetMaterials().Num(), 12);
	UMantleVehicleSource* Source = NewObject<UMantleVehicleSource>(GetTransientPackage(), NAME_None, RF_Transient);
	const FMantleVehicleConversion Conversion = FMantleVehicleConverter::Convert(Car, *Source);
	TestTrue(*FString::Printf(TEXT("it converts (%s)"), *FString::Join(Conversion.Problems, TEXT(" "))), Conversion.bSuccess);
	USkeletalMesh* Body = Source->Body.LoadSynchronous();
	if (TestTrue(TEXT("its body is a copy of the car, not the car"), Body && Body != Car))
	{
		TestTrue(*FString::Printf(TEXT("the body has at most %d materials (%d)"), FMantleVehicleLimits::MaxSections, Body->GetMaterials().Num()),
			Body->GetMaterials().Num() <= FMantleVehicleLimits::MaxSections);
	}
	TestEqual(TEXT("the car itself was not changed"), Car->GetMaterials().Num(), 12);
	const int32 Split = Source->Parts.FilterByPredicate([](const FMantleVehicleSourcePart& Part) { return Part.FoundOn == FMantleVehicleBodySplit::FoundOn(); }).Num();
	TestTrue(*FString::Printf(TEXT("the extra materials are parts (%d)"), Split), Split >= 1 && Split == Source->Parts.Num());
	TestTrue(TEXT("each part is a mesh on a bone of the body"), Source->Parts.Num() > 0 && !Source->Parts[0].Mesh.IsNull() && !Source->Parts[0].Bone.IsNone());
	TestTrue(TEXT("the notes say what was done, in one place"), Conversion.Notes.ContainsByPredicate([](const FString& Note)
	{
		return Note.Contains(TEXT("12 different materials")) && Note.Contains(TEXT("split it for you")) && Note.Contains(TEXT("nothing for you to do"));
	}));
	TestTrue(TEXT("and which bones are the wheels"), Conversion.Notes.ContainsByPredicate([](const FString& Note) { return Note.StartsWith(TEXT("Wheels: ")); }));

	// Converting again does not pile up parts: the split's own are made afresh, the same ones.
	const int32 PartsBefore = Source->Parts.Num();
	const FMantleVehicleConversion Again = FMantleVehicleConverter::Convert(Car, *Source);
	TestTrue(*FString::Printf(TEXT("it converts again (%s)"), *FString::Join(Again.Problems, TEXT(" "))), Again.bSuccess);
	TestEqual(TEXT("with the same parts"), Source->Parts.Num(), PartsBefore);

	// A body within the limit is converted as it was: its own mesh, no parts.
	USkeletalMesh* Small = MakeCar(FourWheels(TEXT("B_FLWheel"), TEXT("B_FRWheel"), TEXT("B_RLWheel"), TEXT("B_RRWheel")), 6, Error);
	if (TestNotNull(*FString::Printf(TEXT("the small car is made (%s)"), *Error), Small))
	{
		UMantleVehicleSource* SmallSource = NewObject<UMantleVehicleSource>(GetTransientPackage(), NAME_None, RF_Transient);
		const FMantleVehicleConversion SmallConversion = FMantleVehicleConverter::Convert(Small, *SmallSource);
		TestTrue(TEXT("it converts"), SmallConversion.bSuccess);
		TestTrue(TEXT("on its own mesh, with no parts"), SmallSource->Body.LoadSynchronous() == Small && SmallSource->Parts.Num() == 0);
		MantleVehicleScratchBody::Discard(Small);
	}
	MantleVehicleScratchBody::Discard(Body);
	MantleVehicleScratchBody::Discard(Car);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleConverterBrakeHubTest,
	"Mantle.Vehicle.Convert.BrakeBonesSteerWithTheirWheel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleConverterBrakeHubTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleConverterWheelsTestPrivate;

	// SUV04: each corner has a B_SUV04_*Brake bone at the wheel's place, the caliper skinned to it. Left alone it did not steer with the wheel, and the caliper cut through
	// the rim. A corner's brake bone is its wheel's hub: it travels and steers with the wheel and does not spin.
	TArray<FCarWheel> Bones = { { TEXT("B_SUV04_FLWheel"), FVector(130, -80, 35) }, { TEXT("B_SUV04_FRWheel"), FVector(130, 80, 35) },
		{ TEXT("B_SUV04_RLWheel"), FVector(-130, -80, 35) }, { TEXT("B_SUV04_RRWheel"), FVector(-130, 80, 35) },
		{ TEXT("B_SUV04_FLBrake"), FVector(130, -80, 35) }, { TEXT("B_SUV04_FRBrake"), FVector(130, 80, 35) },
		{ TEXT("B_SUV04_RLBrake"), FVector(-130, -80, 35) }, { TEXT("B_SUV04_RRCaliper"), FVector(-130, 80, 35) },
		// A brake light is named Brake too, and sits far from every wheel: it is not a hub.
		{ TEXT("B_SUV04_BrakeLight_RL"), FVector(-210, -60, 90) } };
	FString Error;
	USkeletalMesh* Car = MakeCar(Bones, 2, Error);
	if (!TestNotNull(*FString::Printf(TEXT("the car is made (%s)"), *Error), Car))
	{
		return false;
	}
	const TArray<FName> WheelNames = { FName(TEXT("B_SUV04_FLWheel")), FName(TEXT("B_SUV04_FRWheel")), FName(TEXT("B_SUV04_RLWheel")), FName(TEXT("B_SUV04_RRWheel")) };
	TArray<FString> Notes;
	const TArray<FMantleVehicleSourceLinkage> Rules = FMantleVehicleConverter::DetectLinkage(Car->GetRefSkeleton(), WheelNames, Notes);
	const auto HubOf = [&Rules](const TCHAR* Brake) -> const FMantleVehicleSourceLinkage*
	{
		return Rules.FindByPredicate([Brake](const FMantleVehicleSourceLinkage& Rule) { return Rule.Bone == FName(Brake) && Rule.Type == EMantleVehicleLinkageType::Copy; });
	};
	const struct { const TCHAR* Brake; const TCHAR* Wheel; } Expected[] = { { TEXT("B_SUV04_FLBrake"), TEXT("B_SUV04_FLWheel") }, { TEXT("B_SUV04_FRBrake"), TEXT("B_SUV04_FRWheel") },
		{ TEXT("B_SUV04_RLBrake"), TEXT("B_SUV04_RLWheel") }, { TEXT("B_SUV04_RRCaliper"), TEXT("B_SUV04_RRWheel") } };
	for (const auto& Pair : Expected)
	{
		const FMantleVehicleSourceLinkage* Hub = HubOf(Pair.Brake);
		if (TestNotNull(*FString::Printf(TEXT("%s is a hub"), Pair.Brake), Hub))
		{
			TestTrue(*FString::Printf(TEXT("%s follows its own corner's wheel"), Pair.Brake), Hub->Source == FName(Pair.Wheel));
			TestTrue(*FString::Printf(TEXT("%s takes the wheel's steering and travel, not its spin"), Pair.Brake), Hub->Axis == EMantleVehicleLinkageAxis::Z);
		}
	}
	TestNull(TEXT("a brake light far from the wheels is not a hub"), HubOf(TEXT("B_SUV04_BrakeLight_RL")));
	MantleVehicleScratchBody::Discard(Car);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleConverterLightsTest,
	"Mantle.Vehicle.Convert.LightsFoundAndKeptOnTheBody",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleConverterLightsTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleConverterWheelsTestPrivate;

	// SUV04: the body has MI_Lights_Inst sections, and the Convert wrote no lights and split them off as parts (the split kept only the sections the vehicle already listed).
	// Now a Convert finds them as the builder does on a model of its own (here by their names), writes them into Lights, and the split keeps them on the body.
	// A 12-material car whose two smallest materials, the ones a split sends to parts first, are a headlight and a taillight.
	TArray<FString> Names;
	Names.SetNum(12);
	Names[0] = TEXT("SUV_Headlight");
	Names[1] = TEXT("SUV_Taillight");
	FString Error;
	USkeletalMesh* Car = MakeCar(FourWheels(TEXT("B_FLWheel"), TEXT("B_FRWheel"), TEXT("B_RLWheel"), TEXT("B_RRWheel")), 12, Error, nullptr, Names);
	if (!TestNotNull(*FString::Printf(TEXT("the car is made (%s)"), *Error), Car))
	{
		return false;
	}
	UMantleVehicleSource* Source = NewObject<UMantleVehicleSource>(GetTransientPackage(), NAME_None, RF_Transient);
	const FMantleVehicleConversion Conversion = FMantleVehicleConverter::Convert(Car, *Source);
	TestTrue(*FString::Printf(TEXT("it converts (%s)"), *FString::Join(Conversion.Problems, TEXT(" "))), Conversion.bSuccess);
	TestTrue(TEXT("the headlight is listed as the headlights"), Source->Lights.Head.Contains(TEXT("SUV_Headlight")));
	TestTrue(TEXT("the taillight is listed as the rear lights"), Source->Lights.Rear.Contains(TEXT("SUV_Taillight")));
	USkeletalMesh* Body = Source->Body.LoadSynchronous();
	if (TestTrue(TEXT("its body is a copy of the car, not the car"), Body && Body != Car))
	{
		const TArray<FString> Sections = FMantleVehicleLights::SectionNames(Body->GetMaterials());
		TestTrue(TEXT("both light sections are on the body, where the game switches them"), Sections.Contains(TEXT("SUV_Headlight")) && Sections.Contains(TEXT("SUV_Taillight")));
		TestTrue(TEXT("and the body is within the limit"), Body->GetMaterials().Num() <= FMantleVehicleLimits::MaxSections);
	}
	TestTrue(TEXT("none of the parts is a light"), !Source->Parts.ContainsByPredicate([](const FMantleVehicleSourcePart& Part)
	{
		const UStaticMesh* Mesh = Part.Mesh.LoadSynchronous();
		return Mesh && Mesh->GetStaticMaterials().ContainsByPredicate([](const FStaticMaterial& Slot) { return Slot.MaterialSlotName.ToString().Contains(TEXT("light"), ESearchCase::IgnoreCase); });
	}));
	TestTrue(TEXT("the notes say which sections became which lights"), Conversion.Notes.ContainsByPredicate([](const FString& Note)
	{
		return Note.StartsWith(TEXT("Lights: ")) && Note.Contains(TEXT("SUV_Headlight")) && Note.Contains(TEXT("SUV_Taillight")) && Note.Contains(TEXT("your own model was not changed"));
	}));
	TestEqual(TEXT("the car itself was not changed"), Car->GetMaterials().Num(), 12);

	// Converting again finds them again, the same; lights the creator set that the model has are kept.
	const FMantleVehicleConversion Again = FMantleVehicleConverter::Convert(Car, *Source);
	TestTrue(TEXT("it converts again"), Again.bSuccess);
	TestTrue(TEXT("with the same lights"), Source->Lights.Head.Contains(TEXT("SUV_Headlight")) && Source->Lights.Rear.Contains(TEXT("SUV_Taillight")) && Source->Lights.Head.Num() == 1 && Source->Lights.Rear.Num() == 1);
	Source->Lights.Head = { TEXT("mat5") };
	Source->Lights.Rear.Reset();
	const FMantleVehicleConversion Kept = FMantleVehicleConverter::Convert(Car, *Source);
	TestTrue(TEXT("it converts with the creator's lights"), Kept.bSuccess);
	TestTrue(TEXT("a light the creator set is kept, and nothing is found beside it"), Source->Lights.Head.Num() == 1 && Source->Lights.Head[0] == TEXT("mat5") && Source->Lights.Rear.IsEmpty());
	TestTrue(TEXT("and says so"), Kept.Notes.ContainsByPredicate([](const FString& Note) { return Note.StartsWith(TEXT("Lights: your 1 section(s) are kept")); }));

	if (USkeletalMesh* Last = Source->Body.LoadSynchronous(); Last && Last != Car)
	{
		MantleVehicleScratchBody::Discard(Last);
	}
	MantleVehicleScratchBody::Discard(Car);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleConverterSplitPaintTest,
	"Mantle.Vehicle.Convert.SplitPartsKeepPaint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleConverterSplitPaintTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleConverterWheelsTestPrivate;

	// SUV04: its door shells are split-off parts that wear the body's paint. A second Convert (after the seats were set) found the model had a part of its own already
	// and suggested no paint slot for any part, so the doors came out white. The split's own parts are made afresh each Convert and are suggested their paint every time.
	UMaterialInterface* Painted = UMaterial::GetDefaultMaterial(MD_Surface);
	if (!TestNotNull(TEXT("the engine's default material"), Painted))
	{
		return false;
	}
	FString Error;
	USkeletalMesh* Car = MakeCar(FourWheels(TEXT("B_FLWheel"), TEXT("B_FRWheel"), TEXT("B_RLWheel"), TEXT("B_RRWheel")), 12, Error, Painted);
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(*FString::Printf(TEXT("the car is made (%s)"), *Error), Car) || !TestNotNull(TEXT("a mesh for the model's own part"), Cube))
	{
		return false;
	}
	UMantleVehicleSource* Source = NewObject<UMantleVehicleSource>(GetTransientPackage(), NAME_None, RF_Transient);
	// The body's paint is its biggest section's (mat11 stays on the body); the model already has a part of its own, which is not painted.
	Source->Paint.Sections = { TEXT("mat11") };
	FMantleVehicleSourcePart Own;
	Own.Mesh = Cube;
	Own.Bone = FName(TEXT("B_Car_Main"));
	Source->Parts.Add(Own);

	const auto SplitParts = [&Source]()
	{
		return Source->Parts.FilterByPredicate([](const FMantleVehicleSourcePart& Part) { return Part.FoundOn == FMantleVehicleBodySplit::FoundOn(); });
	};
	const FMantleVehicleConversion First = FMantleVehicleConverter::Convert(Car, *Source);
	TestTrue(*FString::Printf(TEXT("it converts (%s)"), *FString::Join(First.Problems, TEXT(" "))), First.bSuccess);
	TArray<FMantleVehicleSourcePart> Split = SplitParts();
	TestTrue(TEXT("the extra materials are parts"), Split.Num() >= 1);
	bool bAllPainted = Split.Num() > 0;
	for (const FMantleVehicleSourcePart& Part : Split)
	{
		const UStaticMesh* Mesh = Part.Mesh.LoadSynchronous();
		bAllPainted = bAllPainted && Mesh && !Part.PaintSlots.IsEmpty();
	}
	TestTrue(TEXT("each split part's slots that draw with the body's paint material wear the paint"), bAllPainted);
	TestTrue(TEXT("the model's own part is left as it was: no paint slots"), Source->Parts[0].PaintSlots.IsEmpty() && Source->Parts[0].LayerSlots.IsEmpty());

	// Converting again: the split's parts are made afresh, and still painted.
	const FMantleVehicleConversion Second = FMantleVehicleConverter::Convert(Car, *Source);
	TestTrue(TEXT("it converts again"), Second.bSuccess);
	Split = SplitParts();
	bAllPainted = Split.Num() > 0;
	for (const FMantleVehicleSourcePart& Part : Split)
	{
		const UStaticMesh* Mesh = Part.Mesh.LoadSynchronous();
		bAllPainted = bAllPainted && Mesh && !Part.PaintSlots.IsEmpty();
	}
	TestTrue(TEXT("the second Convert leaves every split part painted"), bAllPainted);
	TestTrue(TEXT("and the model's own part is still not"), Source->Parts[0].PaintSlots.IsEmpty());

	// What the creator set on a split part is kept by a later Convert: a slot unticked stays unticked.
	if (Split.Num() > 0)
	{
		const auto KeyOf = [](const FMantleVehicleSourcePart& Part) { const FString Asset = Part.Mesh.GetAssetName(); int32 At = INDEX_NONE; return Asset.FindLastChar(TEXT('_'), At) ? Asset.RightChop(At) : Asset; };
		const FString Name = KeyOf(Split[0]);
		for (FMantleVehicleSourcePart& Part : Source->Parts)
		{
			if (Part.FoundOn == FMantleVehicleBodySplit::FoundOn() && KeyOf(Part) == Name)
			{
				Part.PaintSlots = { 0 };
			}
		}
		FMantleVehicleConverter::Convert(Car, *Source);
		const FMantleVehicleSourcePart* Again = Source->Parts.FindByPredicate([&Name, &KeyOf](const FMantleVehicleSourcePart& Part) { return KeyOf(Part) == Name; });
		TestTrue(TEXT("the slots the creator set on a split part are kept"), Again && Again->PaintSlots.Num() == 1 && Again->PaintSlots[0] == 0);
	}
	if (USkeletalMesh* Body = Source->Body.LoadSynchronous(); Body && Body != Car)
	{
		MantleVehicleScratchBody::Discard(Body);
	}
	MantleVehicleScratchBody::Discard(Car);
	return true;
}

#endif
