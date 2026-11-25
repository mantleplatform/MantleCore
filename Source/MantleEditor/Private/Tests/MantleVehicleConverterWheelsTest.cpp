// Copyright Joshua Eger. All Rights Reserved.
#include "MantleSkinnedMesh.h"
#include "MantleVehicleAsset.h"
#include "MantleVehicleBodySplit.h"
#include "MantleVehicleChassisMesh.h"
#include "MantleVehicleConverter.h"
#include "MantleVehicleLightFinder.h"
#include "MantlePlaceValidator.h"
#include "MantleVehicleSource.h"
#include "MantleVehicleScratchBody.h"

#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Materials/Material.h"
#include "MantleVehicleBuilder.h"
#include "Algo/AllOf.h"
#include "Materials/MaterialExpressionVectorParameter.h"
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

	/** A car with more materials than a published body may draw, so the convert splits it. */
	constexpr int32 PastTheLimit = FMantleVehicleLimits::MaxSections + 4;

	/** Slots a car is padded with so its body has to split: one each, the biggest of the car, so they outrank glass, seats and the like for room on the body. */
	constexpr int32 FillerSlots = FMantleVehicleLimits::MaxSections;

	/** A car's skeleton: a root, then each wheel at its place; material m is m + 1 quads, riding bone m modulo the bones. */
	USkeletalMesh* MakeCar(const TArray<FCarWheel>& Wheels, const int32 NumMaterials, FString& OutError, UMaterialInterface* WithMaterial = nullptr,
		const TArray<FString>& SlotNames = TArray<FString>(), const TArray<UMaterialInterface*>& SlotMaterials = TArray<UMaterialInterface*>())
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
			Materials.Add(FSkeletalMaterial(SlotMaterials.IsValidIndex(Material) ? SlotMaterials[Material] : WithMaterial, MaterialName));
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

	/** A material that reads as car paint (it has a Paint Tint parameter): a slot drawn with it is suggested the vehicle's paint. */
	UMaterial* NewPaintTintMaterial()
	{
		UMaterial* Material = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
		UMaterialExpressionVectorParameter* Tint = NewObject<UMaterialExpressionVectorParameter>(Material);
		Tint->Material = Material;
		Tint->ParameterName = TEXT("Paint Tint");
		Tint->DefaultValue = FLinearColor(0.1f, 0.2f, 0.7f);
		Tint->UpdateParameterGuid(true, false);
		Material->GetExpressionCollection().AddExpression(Tint);
		Material->UpdateCachedExpressionData();
		return Material;
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

	// A car that draws four materials more than a published body may: it converts, the body is a copy at the limit and the other four ride along as parts.
	FString Error;
	USkeletalMesh* Car = MakeCar(FourWheels(TEXT("B_FLWheel"), TEXT("B_FRWheel"), TEXT("B_RLWheel"), TEXT("B_RRWheel")), PastTheLimit, Error);
	if (!TestNotNull(*FString::Printf(TEXT("the car is made (%s)"), *Error), Car))
	{
		return false;
	}
	TestEqual(TEXT("the car has the materials it was made with"), Car->GetMaterials().Num(), PastTheLimit);
	UMantleVehicleSource* Source = NewObject<UMantleVehicleSource>(GetTransientPackage(), NAME_None, RF_Transient);
	const FMantleVehicleConversion Conversion = FMantleVehicleConverter::Convert(Car, *Source);
	TestTrue(*FString::Printf(TEXT("it converts (%s)"), *FString::Join(Conversion.Problems, TEXT(" "))), Conversion.bSuccess);
	USkeletalMesh* Body = Source->Body.LoadSynchronous();
	if (TestTrue(TEXT("its body is a copy of the car, not the car"), Body && Body != Car))
	{
		TestTrue(*FString::Printf(TEXT("the body has at most %d materials (%d)"), FMantleVehicleLimits::MaxSections, Body->GetMaterials().Num()),
			Body->GetMaterials().Num() <= FMantleVehicleLimits::MaxSections);
	}
	TestEqual(TEXT("the car itself was not changed"), Car->GetMaterials().Num(), PastTheLimit);
	const int32 Split = Source->Parts.FilterByPredicate([](const FMantleVehicleSourcePart& Part) { return Part.FoundOn == FMantleVehicleBodySplit::FoundOn(); }).Num();
	TestTrue(*FString::Printf(TEXT("the extra materials are parts (%d)"), Split), Split >= 1 && Split == Source->Parts.Num());
	TestTrue(TEXT("each part is a mesh on a bone of the body"), Source->Parts.Num() > 0 && !Source->Parts[0].Mesh.IsNull() && !Source->Parts[0].Bone.IsNone());
	TestTrue(TEXT("the notes say what was done, in one place"), Conversion.Notes.ContainsByPredicate([](const FString& Note)
	{
		return Note.Contains(*FString::Printf(TEXT("%d different materials"), PastTheLimit)) && Note.Contains(TEXT("split it for you")) && Note.Contains(TEXT("nothing for you to do"));
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
	// A car past the limit whose two smallest materials, the ones a split sends to parts first, are a headlight and a taillight.
	TArray<FString> Names;
	Names.SetNum(PastTheLimit);
	Names[0] = TEXT("SUV_Headlight");
	Names[1] = TEXT("SUV_Taillight");
	FString Error;
	USkeletalMesh* Car = MakeCar(FourWheels(TEXT("B_FLWheel"), TEXT("B_FRWheel"), TEXT("B_RLWheel"), TEXT("B_RRWheel")), PastTheLimit, Error, nullptr, Names);
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
	TestEqual(TEXT("the car itself was not changed"), Car->GetMaterials().Num(), PastTheLimit);

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
	FMantleVehicleConverterFreshAssetLightsTest,
	"Mantle.Vehicle.Convert.FreshAssetKeepsItsLights",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleConverterFreshAssetLightsTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleConverterWheelsTestPrivate;

	// A car's first Convert (the Convert vehicle command, Build): the vehicle asset is made new, and it must carry the lights the conversion found. It was saved with
	// every light list empty (SUV01 on 2026-11-25: Headlights, ReverseLights and the signals cut, none listed), while a Convert again onto the asset kept them.
	TArray<FString> Names = { TEXT("SUV_Headlight"), TEXT("SUV_Taillight"), TEXT("SUV_Body"), TEXT("SUV_Glass") };
	FString Error;
	USkeletalMesh* Car = MakeCar(FourWheels(TEXT("B_FLWheel"), TEXT("B_FRWheel"), TEXT("B_RLWheel"), TEXT("B_RRWheel")), Names.Num(), Error, nullptr, Names);
	if (!TestNotNull(*FString::Printf(TEXT("the car is made (%s)"), *Error), Car))
	{
		return false;
	}
	FMantleVehicleConversion Conversion;
	UMantleVehicleSource* Source = FMantleVehicleConverter::ConvertAsset(Car, Conversion);
	TestTrue(*FString::Printf(TEXT("it converts to a new vehicle asset (%s)"), *FString::Join(Conversion.Problems, TEXT(" "))), Source != nullptr && Conversion.bSuccess);
	if (Source)
	{
		TestTrue(*FString::Printf(TEXT("the new asset lists the headlight (Head: %s)"), *FString::Join(Source->Lights.Head, TEXT(", "))), Source->Lights.Head.Contains(TEXT("SUV_Headlight")));
		TestTrue(*FString::Printf(TEXT("and the taillight (Rear: %s)"), *FString::Join(Source->Lights.Rear, TEXT(", "))), Source->Lights.Rear.Contains(TEXT("SUV_Taillight")));

		// Converting again onto that asset (the other path) keeps the same lists.
		FMantleVehicleConversion Again;
		UMantleVehicleSource* Same = FMantleVehicleConverter::ConvertAsset(Car, Again);
		TestTrue(TEXT("a Convert again updates the same asset"), Same == Source);
		TestTrue(TEXT("with the same lights"), Same && Same->Lights.Head.Contains(TEXT("SUV_Headlight")) && Same->Lights.Rear.Contains(TEXT("SUV_Taillight")));

		if (USkeletalMesh* Body = Source->Body.LoadSynchronous(); Body && Body != Car)
		{
			MantleVehicleScratchBody::Discard(Body);
		}
		UPackage* Package = Source->GetOutermost();
		Source->ClearFlags(RF_Public | RF_Standalone);
		Source->MarkAsGarbage();
		if (Package && Package != GetTransientPackage())
		{
			Package->ClearDirtyFlag();
			Package->MarkAsGarbage();
		}
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
	// Slots past the limit, each its own material that reads as paint (slots of one material are merged, so each is a material of its own).
	TArray<UMaterialInterface*> PaintMaterials;
	for (int32 Slot = 0; Slot < PastTheLimit; ++Slot)
	{
		PaintMaterials.Add(NewPaintTintMaterial());
	}
	FString Error;
	USkeletalMesh* Car = MakeCar(FourWheels(TEXT("B_FLWheel"), TEXT("B_FRWheel"), TEXT("B_RLWheel"), TEXT("B_RRWheel")), PastTheLimit, Error, nullptr, TArray<FString>(), PaintMaterials);
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(*FString::Printf(TEXT("the car is made (%s)"), *Error), Car) || !TestNotNull(TEXT("a mesh for the model's own part"), Cube))
	{
		return false;
	}
	UMantleVehicleSource* Source = NewObject<UMantleVehicleSource>(GetTransientPackage(), NAME_None, RF_Transient);
	// The body's paint is its biggest section's (the last mat stays on the body); the model already has a part of its own, which is not painted.
	Source->Paint.Sections = { FString::Printf(TEXT("mat%d"), PastTheLimit - 1) };
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleConverterPublishableSplitTest,
	"Mantle.Vehicle.Convert.SplitCarNamesOnlySectionsItHas",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleConverterPublishableSplitTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleConverterWheelsTestPrivate;

	// SUV04's shape: 25 material slots, padded with enough more to pass the body's limit. The body paint is three of them with the fewest triangles, and the lights, rims, glass and interior have more. A convert
	// that sent the paint to parts, or named a light or a paint section the body does not have, was refused by the publish check (20 errors, nothing uploaded).
	TArray<FString> Names;
	TArray<UMaterialInterface*> Materials;
	// Three paint materials, each its own (three slots of one material would be merged: that is the next test).
	const auto NewPaint = []()
	{
		UMaterial* Paint = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
		UMaterialExpressionVectorParameter* Tint = NewObject<UMaterialExpressionVectorParameter>(Paint);
		Tint->Material = Paint;
		Tint->ParameterName = TEXT("Paint Tint");
		Tint->DefaultValue = FLinearColor(0.1f, 0.2f, 0.7f);
		Tint->UpdateParameterGuid(true, false);
		Paint->GetExpressionCollection().AddExpression(Tint);
		Paint->UpdateCachedExpressionData();
		return Paint;
	};
	const auto Slot = [&Names, &Materials](const TCHAR* Name, UMaterialInterface* Material = nullptr)
	{
		Names.Add(Name);
		Materials.Add(Material);
	};
	Slot(TEXT("SUV04_Body"), NewPaint());
	Slot(TEXT("SUV04_Body2"), NewPaint());
	Slot(TEXT("SUV04_Body3"), NewPaint());
	for (const TCHAR* Glass : { TEXT("SUV04_Glass"), TEXT("SUV04_Glass2"), TEXT("SUV04_Glass3"), TEXT("SUV04_Glass4"), TEXT("SUV04_Glass5"), TEXT("SUV04_Glass6") })
	{
		Slot(Glass);
	}
	for (const TCHAR* Interior : { TEXT("SUV04_Interior"), TEXT("SUV04_Interior2"), TEXT("SUV04_Interior3"), TEXT("SUV04_Interior4"), TEXT("SUV04_Interior5"), TEXT("SUV04_Interior6") })
	{
		Slot(Interior);
	}
	Slot(TEXT("SUV04_Seats"));
	for (const TCHAR* Lamp : { TEXT("headlight_lens"), TEXT("headlight_lens2"), TEXT("taillight_lens"), TEXT("taillight_lens2"), TEXT("reverselight_lens") })
	{
		Slot(Lamp);
	}
	for (const TCHAR* Rim : { TEXT("WheelSet_05"), TEXT("WheelSet_06"), TEXT("WheelSet_07") })
	{
		Slot(Rim);
	}
	Slot(TEXT("Fbx_Default_Material_17"));
	if (!TestEqual(TEXT("25 slots"), Names.Num(), 25))
	{
		return false;
	}
	// Filler slots after them, each with more triangles than any of the 25: they take the room on the body that glass and the interior would.
	for (int32 Filler = 0; Filler < FillerSlots; ++Filler)
	{
		Slot(*FString::Printf(TEXT("pad%d"), Filler));
	}
	FString Error;
	USkeletalMesh* Car = MakeCar(FourWheels(TEXT("B_FLWheel"), TEXT("B_FRWheel"), TEXT("B_RLWheel"), TEXT("B_RRWheel")), Names.Num(), Error, nullptr, Names, Materials);
	if (!TestNotNull(*FString::Printf(TEXT("the car is made (%s)"), *Error), Car))
	{
		return false;
	}

	UMantleVehicleSource* Source = NewObject<UMantleVehicleSource>(GetTransientPackage(), NAME_None, RF_Transient);
	// What an earlier convert left (SUV04's v1): paint sections, an interior layer, and lights naming sections the model does not have.
	Source->Paint.Sections = { TEXT("SUV04_Body"), TEXT("SUV04_Body2"), TEXT("SUV04_Body3") };
	FMantleVehicleSourcePaintLayer& InteriorLayer = Source->Paint.Layers.AddDefaulted_GetRef();
	InteriorLayer.Layer = EMantleVehicleSourcePaintLayer::Interior;
	InteriorLayer.Sections = { TEXT("SUV04_Interior"), TEXT("SUV04_Seats") };
	Source->Lights.Head = { TEXT("Headlights"), TEXT("Headlights2") };
	Source->Lights.Rear = { TEXT("RearLights5") };

	for (int32 Run = 1; Run <= 2; ++Run)
	{
		const FMantleVehicleConversion Conversion = FMantleVehicleConverter::Convert(Car, *Source);
		TestTrue(*FString::Printf(TEXT("run %d converts (%s)"), Run, *FString::Join(Conversion.Problems, TEXT(" "))), Conversion.bSuccess);
		USkeletalMesh* Body = Source->Body.LoadSynchronous();
		if (!TestTrue(TEXT("its body is a copy of the car"), Body && Body != Car))
		{
			break;
		}
		const TArray<FString> BodySections = FMantleVehicleLights::SectionNames(Body->GetMaterials());
		const auto OnBody = [&BodySections](const FString& Section) { return BodySections.Contains(Section); };
		TestTrue(*FString::Printf(TEXT("run %d: the body draws at most %d materials (%d)"), Run, FMantleVehicleLimits::MaxSections, BodySections.Num()), BodySections.Num() <= FMantleVehicleLimits::MaxSections);
		TestTrue(TEXT("the three car paint sections are on the body"), OnBody(TEXT("SUV04_Body")) && OnBody(TEXT("SUV04_Body2")) && OnBody(TEXT("SUV04_Body3")));
		TestTrue(TEXT("and each kind of light has a section on it"), Source->Lights.Head.Num() > 0 && Source->Lights.Rear.Num() > 0 && Source->Lights.Reverse.Num() > 0);
		TestTrue(TEXT("glass and the interior went to parts first, not the paint or the lights"), !OnBody(TEXT("SUV04_Glass")) && !OnBody(TEXT("SUV04_Seats")));
		TestTrue(TEXT("Paint names the body's sections only"), Algo::AllOf(Source->Paint.Sections, OnBody) && Source->Paint.Sections.Num() >= 3);
		bool bLayersOnBody = true;
		for (const FMantleVehicleSourcePaintLayer& Layer : Source->Paint.Layers)
		{
			bLayersOnBody = bLayersOnBody && Algo::AllOf(Layer.Sections, OnBody);
		}
		TestTrue(TEXT("so do its paint layers (the interior layer's sections went to parts, and the layer with them)"), bLayersOnBody
			&& !Source->Paint.Layers.ContainsByPredicate([](const FMantleVehicleSourcePaintLayer& Layer) { return Layer.Layer == EMantleVehicleSourcePaintLayer::Interior && Layer.Sections.Contains(TEXT("SUV04_Interior")); }));
		bool bLightsOnBody = true;
		for (const EMantleLightKind Kind : { EMantleLightKind::Head, EMantleLightKind::Rear, EMantleLightKind::Reverse, EMantleLightKind::Left, EMantleLightKind::Right })
		{
			bLightsOnBody = bLightsOnBody && Algo::AllOf(FMantleVehicleLights::Sections(Source->Lights, Kind), OnBody);
		}
		TestTrue(TEXT("and so do its lights (the stale names are gone)"), bLightsOnBody && !Source->Lights.Head.Contains(TEXT("Headlights")) && !Source->Lights.Rear.Contains(TEXT("RearLights5")));

		// The converter's own pre-upload check: no section the body does not have.
		TArray<FMantleValidationIssue> Issues;
		FMantleVehicleConverter::CheckSource(Source, FString(), Issues);
		TArray<FString> SectionErrors;
		for (const FMantleValidationIssue& Issue : Issues)
		{
			if (Issue.Severity == EMantleIssueSeverity::Error && (Issue.Problem.Contains(TEXT("which its body does not have")) || Issue.Problem.Contains(TEXT("but its body has no such section"))))
			{
				SectionErrors.Add(Issue.Problem);
			}
		}
		TestTrue(*FString::Printf(TEXT("run %d: the publish check finds no section the body lacks (%s)"), Run, *FString::Join(SectionErrors, TEXT(" | "))), SectionErrors.IsEmpty());
	}
	if (USkeletalMesh* Last = Source->Body.LoadSynchronous(); Last && Last != Car)
	{
		MantleVehicleScratchBody::Discard(Last);
	}
	MantleVehicleScratchBody::Discard(Car);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleConverterMergedSlotsTest,
	"Mantle.Vehicle.Convert.SameMaterialSlotsMergeAndKeepPaintLightsAndARim",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleConverterMergedSlotsTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleConverterWheelsTestPrivate;

	// SUV04: 25 material slots that are 9 materials (the body paint, the glass and the interior are one material each under six names, and so on), padded with more slots of a
	// material each so the body is past its limit. A convert
	// that kept every slot sent the paint and the rim to parts, and rim swapping was lost. Merged first, the paint, each light and the rims fit on the body.
	const auto NewMaterial = [](const bool bPaint)
	{
		UMaterial* Material = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
		if (bPaint)
		{
			UMaterialExpressionVectorParameter* Tint = NewObject<UMaterialExpressionVectorParameter>(Material);
			Tint->Material = Material;
			Tint->ParameterName = TEXT("Paint Tint");
			Tint->DefaultValue = FLinearColor(0.1f, 0.2f, 0.7f);
			Tint->UpdateParameterGuid(true, false);
			Material->GetExpressionCollection().AddExpression(Tint);
			Material->UpdateCachedExpressionData();
		}
		return Material;
	};
	UMaterial* PaintMaterial = NewMaterial(true);
	UMaterial* GlassMaterial = NewMaterial(false);
	UMaterial* InteriorMaterial = NewMaterial(false);
	UMaterial* SeatsMaterial = NewMaterial(false);
	UMaterial* RimMaterial = NewMaterial(false);

	struct FSlot
	{
		FString Name;
		UMaterialInterface* Material;
		int32 Bone;
	};
	TArray<FSlot> Slots;
	for (const TCHAR* Body : { TEXT("SUV04_Body"), TEXT("SUV04_Body2"), TEXT("SUV04_Body3"), TEXT("SUV04_Body4"), TEXT("SUV04_Body5") })
	{
		Slots.Add({ Body, PaintMaterial, 0 });
	}
	for (const TCHAR* Glass : { TEXT("SUV04_Glass"), TEXT("SUV04_Glass2"), TEXT("SUV04_Glass3"), TEXT("SUV04_Glass4"), TEXT("SUV04_Glass5"), TEXT("SUV04_Glass6") })
	{
		Slots.Add({ Glass, GlassMaterial, 0 });
	}
	for (const TCHAR* Interior : { TEXT("SUV04_Interior"), TEXT("SUV04_Interior2"), TEXT("SUV04_Interior3"), TEXT("SUV04_Interior4"), TEXT("SUV04_Interior5"), TEXT("SUV04_Interior6") })
	{
		Slots.Add({ Interior, InteriorMaterial, 0 });
	}
	Slots.Add({ TEXT("SUV04_Seats"), SeatsMaterial, 0 });
	for (const TCHAR* Lamp : { TEXT("headlight_lens"), TEXT("taillight_lens"), TEXT("reverselight_lens") })
	{
		Slots.Add({ Lamp, nullptr, 0 });
	}
	// WheelSet_07 is the rim of each wheel (its vertices are the wheel's own, weighted to its bone); the others are one material with it and not the wheel's.
	const int32 RimSlot = Slots.Num();
	Slots.Add({ TEXT("WheelSet_07"), RimMaterial, 1 });
	Slots.Add({ TEXT("WheelSet_06"), RimMaterial, 0 });
	Slots.Add({ TEXT("WheelSet_05"), RimMaterial, 0 });
	Slots.Add({ TEXT("Fbx_Default_Material_17"), nullptr, 0 });
	if (!TestEqual(TEXT("25 slots"), Slots.Num(), 25))
	{
		return false;
	}
	for (int32 Filler = 0; Filler < FillerSlots; ++Filler)
	{
		Slots.Add({ FString::Printf(TEXT("pad%d"), Filler), nullptr, 0 });
	}

	FMantleSkinnedMesh Skin;
	Skin.Bones.Add({ FName(TEXT("B_Car_Main")), INDEX_NONE });
	Skin.ReferencePose.Add(FTransform::Identity);
	const FVector Centers[4] = { FVector(130, -80, 35), FVector(130, 80, 35), FVector(-130, -80, 35), FVector(-130, 80, 35) };
	const TCHAR* const WheelBones[4] = { TEXT("B_FLWheel"), TEXT("B_FRWheel"), TEXT("B_RLWheel"), TEXT("B_RRWheel") };
	for (int32 Wheel = 0; Wheel < 4; ++Wheel)
	{
		Skin.Bones.Add({ FName(WheelBones[Wheel]), 0 });
		Skin.ReferencePose.Add(FTransform(Centers[Wheel]));
	}
	FMantleSkinnedMeshLOD& LOD = Skin.LODs.AddDefaulted_GetRef();
	LOD.NumInfluences = 1;
	TArray<FSkeletalMaterial> Materials;
	float X = -200.f;
	for (int32 Slot = 0; Slot < Slots.Num(); ++Slot)
	{
		Materials.Add(FSkeletalMaterial(Slots[Slot].Material, FName(*Slots[Slot].Name)));
		FMantleSkinnedMeshSection& Section = LOD.Sections.AddDefaulted_GetRef();
		Section.MaterialIndex = Slot;
		Section.MaterialName = FName(*Slots[Slot].Name);
		Section.FirstIndex = LOD.Indices.Num();
		const auto Quad = [&LOD](const FVector& At, const int32 Bone)
		{
			const uint32 First = static_cast<uint32>(LOD.Positions.Num());
			// A diamond in the plane the wheel turns in (X and Z), 6 cm out from its center.
			for (const FVector& Corner : { FVector(6, 0, 0), FVector(0, 0, 6), FVector(-6, 0, 0), FVector(0, 0, -6) })
			{
				LOD.Positions.Add(FVector3f(At + Corner));
				LOD.Normals.Add(FVector3f(0.f, 1.f, 0.f));
				LOD.UVs.Add(FVector2f(0.f, 0.f));
				LOD.InfluenceBones.Add(static_cast<uint16>(Bone));
				LOD.InfluenceWeights.Add(255);
			}
			LOD.Indices.Append({ First, First + 1, First + 2, First, First + 2, First + 3 });
		};
		if (Slot == RimSlot)
		{
			for (int32 Wheel = 0; Wheel < 4; ++Wheel)
			{
				for (int32 Each = 0; Each < 3; ++Each)
				{
					Quad(Centers[Wheel] + FVector(0, 2.f * (Each - 1), 0), 1 + Wheel);
				}
			}
		}
		else
		{
			// A few quads up the car, on the root, each slot at its own place.
			for (int32 Each = 0; Each < (Slot >= 25 ? 40 : 2 + Slot % 3); ++Each)
			{
				Quad(FVector(X, 0, 90), 0);
				X += 15.f;
			}
		}
		Section.IndexCount = LOD.Indices.Num() - Section.FirstIndex;
	}
	FString Error;
	USkeletalMesh* Car = FMantleVehicleChassisMesh::Create(Skin, TEXT("/Temp/MantleConvertWheelsTest"), FString::Printf(TEXT("SuvShape%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8)), Error, &Materials);
	if (!TestNotNull(*FString::Printf(TEXT("the car is made (%s)"), *Error), Car))
	{
		return false;
	}

	UMantleVehicleSource* Source = NewObject<UMantleVehicleSource>(GetTransientPackage(), NAME_None, RF_Transient);
	// What an earlier convert left: three paint sections (now one), and lights naming sections that are not there.
	Source->Paint.Sections = { TEXT("SUV04_Body"), TEXT("SUV04_Body2"), TEXT("SUV04_Body3") };
	Source->Lights.Head = { TEXT("Headlights2") };
	for (int32 Run = 1; Run <= 2; ++Run)
	{
		const FMantleVehicleConversion Conversion = FMantleVehicleConverter::Convert(Car, *Source);
		TestTrue(*FString::Printf(TEXT("run %d converts (%s)"), Run, *FString::Join(Conversion.Problems, TEXT(" "))), Conversion.bSuccess);
		TestTrue(TEXT("the notes say which slots use how many materials, and what was merged"), Conversion.Notes.ContainsByPredicate([](const FString& Note)
		{
			return Note.Contains(*FString::Printf(TEXT("%d material slots use %d different materials"), 25 + FillerSlots, 9 + FillerSlots)) && Note.Contains(TEXT("SUV04_Body: 5 slots")) && Note.Contains(TEXT("your own model was not changed"));
		}));
		USkeletalMesh* Body = Source->Body.LoadSynchronous();
		if (!TestTrue(TEXT("its body is a copy of the car"), Body && Body != Car))
		{
			break;
		}
		const TArray<FString> BodySections = FMantleVehicleLights::SectionNames(Body->GetMaterials());
		const auto OnBody = [&BodySections](const FString& Section) { return BodySections.Contains(Section); };
		TestTrue(*FString::Printf(TEXT("run %d: the body draws at most %d materials (%d)"), Run, FMantleVehicleLimits::MaxSections, BodySections.Num()), BodySections.Num() <= FMantleVehicleLimits::MaxSections);
		TestTrue(TEXT("the car paint is one section on the body"), OnBody(TEXT("SUV04_Body")) && !OnBody(TEXT("SUV04_Body2")) && Source->Paint.Sections.Num() == 1 && Source->Paint.Sections[0] == TEXT("SUV04_Body"));
		TestTrue(TEXT("each light is on the body"), OnBody(TEXT("headlight_lens")) && OnBody(TEXT("taillight_lens")) && OnBody(TEXT("reverselight_lens")));
		TestTrue(TEXT("and the rim is on the body"), OnBody(TEXT("WheelSet_07")));
		TestTrue(TEXT("the interior and the glass are not (one section each, on a part)"), !OnBody(TEXT("SUV04_Glass")) || !OnBody(TEXT("SUV04_Interior")) || !OnBody(TEXT("SUV04_Seats")));
		bool bRimSwap = Source->Wheels.Num() == 4;
		for (const FMantleVehicleSourceWheel& Wheel : Source->Wheels)
		{
			bRimSwap = bRimSwap && Wheel.RimSections.Contains(TEXT("WheelSet_07"));
		}
		TestTrue(TEXT("every wheel's rim is WheelSet_07, so wheel swapping is on"), bRimSwap);
		TestTrue(TEXT("Paint names the body's sections only"), Algo::AllOf(Source->Paint.Sections, OnBody));
		bool bLightsOnBody = Source->Lights.Head.Num() > 0 && Source->Lights.Rear.Num() > 0 && Source->Lights.Reverse.Num() > 0;
		for (const EMantleLightKind Kind : { EMantleLightKind::Head, EMantleLightKind::Rear, EMantleLightKind::Reverse, EMantleLightKind::Left, EMantleLightKind::Right })
		{
			bLightsOnBody = bLightsOnBody && Algo::AllOf(FMantleVehicleLights::Sections(Source->Lights, Kind), OnBody);
		}
		TestTrue(TEXT("each kind of light is set, and on the body"), bLightsOnBody);

		TArray<FMantleValidationIssue> Issues;
		FMantleVehicleConverter::CheckSource(Source, FString(), Issues);
		TArray<FString> SectionErrors;
		for (const FMantleValidationIssue& Issue : Issues)
		{
			if (Issue.Severity == EMantleIssueSeverity::Error && (Issue.Problem.Contains(TEXT("which its body does not have")) || Issue.Problem.Contains(TEXT("but its body has no such section"))))
			{
				SectionErrors.Add(Issue.Problem);
			}
		}
		TestTrue(*FString::Printf(TEXT("run %d: the publish check finds no section the body lacks (%s)"), Run, *FString::Join(SectionErrors, TEXT(" | "))), SectionErrors.IsEmpty());
	}
	if (USkeletalMesh* Last = Source->Body.LoadSynchronous(); Last && Last != Car)
	{
		MantleVehicleScratchBody::Discard(Last);
	}
	MantleVehicleScratchBody::Discard(Car);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleConverterRenumberedPartsTest,
	"Mantle.Vehicle.Convert.SplitPartPaintFollowsTheMaterial",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleConverterRenumberedPartsTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleConverterWheelsTestPrivate;

	// SUV04's front-left door got a brake's paint: the split numbers its parts afresh each Convert, and what an earlier Convert set on "Part8" (a brake, on the wheels layer) was
	// put on the new Part8 (a door), so its glass wore the wheels' white. What was set follows the material each slot draws, whatever number its part has now.
	const auto NewPart = [](const TCHAR* MeshName, const TArray<UMaterialInterface*>& Materials)
	{
		UStaticMesh* Mesh = NewObject<UStaticMesh>(GetTransientPackage(), FName(MeshName), RF_Transient);
		for (int32 Slot = 0; Slot < Materials.Num(); ++Slot)
		{
			Mesh->GetStaticMaterials().Add(FStaticMaterial(Materials[Slot], FName(*FString::Printf(TEXT("slot%d"), Slot))));
		}
		FMantleVehicleSourcePart Part;
		Part.Mesh = Mesh;
		Part.Bone = FName(TEXT("B_Car_Main"));
		Part.FoundOn = FMantleVehicleBodySplit::FoundOn();
		return Part;
	};
	UMaterial* Brake = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
	UMaterial* Glass = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
	UMaterial* DoorPaint = NewPaintTintMaterial();
	UMaterial* Interior = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);

	// The last Convert: Part8 was a brake on the wheels layer; Part1 held the door paint (painted) and an interior (on the interior layer); Part3 was glass, with nothing set.
	TArray<FMantleVehicleSourcePart> Earlier;
	FMantleVehicleSourcePart OldBrake = NewPart(TEXT("SM_Old_Part8"), { Brake });
	FMantleVehicleSourcePartLayerSlots& OnWheels = OldBrake.LayerSlots.AddDefaulted_GetRef();
	OnWheels.Layer = EMantleVehicleSourcePaintLayer::Wheels;
	OnWheels.Slots = { 0 };
	Earlier.Add(OldBrake);
	FMantleVehicleSourcePart OldDoor = NewPart(TEXT("SM_Old_Part1"), { DoorPaint, Interior });
	OldDoor.PaintSlots = { 0 };
	FMantleVehicleSourcePartLayerSlots& OnInterior = OldDoor.LayerSlots.AddDefaulted_GetRef();
	OnInterior.Layer = EMantleVehicleSourcePaintLayer::Interior;
	OnInterior.Slots = { 1 };
	Earlier.Add(OldDoor);
	Earlier.Add(NewPart(TEXT("SM_Old_Part3"), { Glass }));
	const TArray<FMantleVehicleConverter::FEarlierPartSlot> Before = FMantleVehicleConverter::CaptureSplitPartPaint(Earlier);
	TestEqual(TEXT("the slots of the parts that had something set are read (1 + 2), not the glass part's"), Before.Num(), 3);

	// This Convert: Part8 is a door (glass, paint), Part1 an interior and a brake.
	TArray<FMantleVehicleSourcePart> Now;
	Now.Add(NewPart(TEXT("SM_New_Part8"), { Glass, DoorPaint }));
	Now.Add(NewPart(TEXT("SM_New_Part1"), { Interior, Brake }));
	TMap<int32, TSet<int32>> Known;
	FMantleVehicleConverter::RestoreSplitPartPaint(Before, Now, Known);

	TestTrue(TEXT("the door's paint slot is painted, though a brake had its number"), Now[0].PaintSlots == TArray<int32>({ 1 }));
	TestTrue(TEXT("the door's glass wears no layer"), Now[0].LayerSlots.IsEmpty());
	TestTrue(TEXT("the door's paint is known (kept), its glass is not (the paint step suggests it)"), Known.Contains(0) && Known[0].Contains(1) && !Known[0].Contains(0));
	const FMantleVehicleSourcePartLayerSlots* InteriorLayer = Now[1].LayerSlots.FindByPredicate([](const FMantleVehicleSourcePartLayerSlots& Has) { return Has.Layer == EMantleVehicleSourcePaintLayer::Interior; });
	const FMantleVehicleSourcePartLayerSlots* WheelsLayer = Now[1].LayerSlots.FindByPredicate([](const FMantleVehicleSourcePartLayerSlots& Has) { return Has.Layer == EMantleVehicleSourcePaintLayer::Wheels; });
	TestTrue(TEXT("the interior is on the interior layer where it moved to"), InteriorLayer && InteriorLayer->Slots == TArray<int32>({ 0 }));
	TestTrue(TEXT("and the brake on the wheels layer"), WheelsLayer && WheelsLayer->Slots == TArray<int32>({ 1 }));
	TestTrue(TEXT("nothing is painted on a part that had no painted material"), Now[1].PaintSlots.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleConverterLayersOnlyWhatTheModelMarksTest,
	"Mantle.Vehicle.Convert.LayersOnlyForWhatTheModelMarks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleConverterLayersOnlyWhatTheModelMarksTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleConverterWheelsTestPrivate;

	// SUV04's cargo area came out flat white: its interior was suggested the interior layer, which wears Mantle's paint (white, trim finish) instead of the model's own material.
	// A converted car looks as the model does: a seat, an interior or a dashboard named like one keeps its material, and only what the model marks as a tint layer (a GTA
	// model's PRIMARY, SECONDARY and WHEEL) is given a layer, in a body section and in a part's slot alike.
	FString Error;
	{
		TArray<FString> Names = { TEXT("SUV_Body"), TEXT("SUV_Interior"), TEXT("SUV_Seats"), TEXT("SUV_Dashboard"), TEXT("leather_1__SECONDARY_") };
		TArray<UMaterialInterface*> Materials = { NewPaintTintMaterial(), NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient), NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient),
			NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient), NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient) };
		USkeletalMesh* Car = MakeCar(FourWheels(TEXT("B_FLWheel"), TEXT("B_FRWheel"), TEXT("B_RLWheel"), TEXT("B_RRWheel")), 5, Error, nullptr, Names, Materials);
		if (!TestNotNull(*FString::Printf(TEXT("the car is made (%s)"), *Error), Car))
		{
			return false;
		}
		UMantleVehicleSource* Source = NewObject<UMantleVehicleSource>(GetTransientPackage(), NAME_None, RF_Transient);
		const FMantleVehicleConversion Result = FMantleVehicleConverter::Convert(Car, *Source);
		TestTrue(*FString::Printf(TEXT("it converts (%s)"), *FString::Join(Result.Problems, TEXT(" "))), Result.bSuccess);
		TestTrue(TEXT("the body's paint is the car paint"), Source->Paint.Sections.Contains(TEXT("SUV_Body")));
		const auto LayerSections = [&Source](const EMantleVehicleSourcePaintLayer Layer)
		{
			const FMantleVehicleSourcePaintLayer* Found = Source->Paint.Layers.FindByPredicate([Layer](const FMantleVehicleSourcePaintLayer& Has) { return Has.Layer == Layer; });
			return Found ? Found->Sections : TArray<FString>();
		};
		TestTrue(TEXT("the interior, the seats and the dashboard are on no layer"), LayerSections(EMantleVehicleSourcePaintLayer::Interior).IsEmpty() && LayerSections(EMantleVehicleSourcePaintLayer::Dashboard).IsEmpty());
		TestTrue(TEXT("the model's SECONDARY is the second paint"), LayerSections(EMantleVehicleSourcePaintLayer::Secondary).Contains(TEXT("leather_1__SECONDARY_")));
		const FString Notes = FString::Join(Result.Notes, TEXT("\n"));
		TestTrue(TEXT("and the notes say they keep the model's material"), Notes.Contains(TEXT("SUV_Interior (it keeps the model's own material")));
		MantleVehicleScratchBody::Discard(Car);
	}

	// A split car: the interior materials ride on parts, and no slot of a part is given the interior layer either.
	{
		TArray<FString> Names;
		TArray<UMaterialInterface*> Materials;
		for (int32 Slot = 0; Slot < PastTheLimit; ++Slot)
		{
			Names.Add(Slot < 4 ? FString::Printf(TEXT("Interior_%d"), Slot) : FString::Printf(TEXT("mat%d"), Slot));
			Materials.Add(Slot < 4 ? NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient) : NewPaintTintMaterial());
		}
		USkeletalMesh* Car = MakeCar(FourWheels(TEXT("B_FLWheel"), TEXT("B_FRWheel"), TEXT("B_RLWheel"), TEXT("B_RRWheel")), PastTheLimit, Error, nullptr, Names, Materials);
		if (!TestNotNull(*FString::Printf(TEXT("the split car is made (%s)"), *Error), Car))
		{
			return false;
		}
		UMantleVehicleSource* Source = NewObject<UMantleVehicleSource>(GetTransientPackage(), NAME_None, RF_Transient);
		const FMantleVehicleConversion Result = FMantleVehicleConverter::Convert(Car, *Source);
		TestTrue(*FString::Printf(TEXT("it converts (%s)"), *FString::Join(Result.Problems, TEXT(" "))), Result.bSuccess);
		int32 SplitParts = 0;
		bool bAnyLayerSlot = false;
		for (const FMantleVehicleSourcePart& Part : Source->Parts)
		{
			if (Part.FoundOn == FMantleVehicleBodySplit::FoundOn())
			{
				++SplitParts;
				bAnyLayerSlot = bAnyLayerSlot || !Part.LayerSlots.IsEmpty();
			}
		}
		TestTrue(TEXT("the interiors are split-off parts"), SplitParts > 0);
		TestFalse(TEXT("none of their slots is on a paint layer"), bAnyLayerSlot);
		TestFalse(TEXT("and the vehicle has no interior layer"), Source->Paint.Layers.ContainsByPredicate([](const FMantleVehicleSourcePaintLayer& Has) { return Has.Layer == EMantleVehicleSourcePaintLayer::Interior; }));
		MantleVehicleScratchBody::Discard(Car);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleStartingPaintRulesTest,
	"Mantle.Vehicle.Convert.StartingPaintRules",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleStartingPaintRulesTest::RunTest(const FString& Parameters)
{
	using FConverter = FMantleVehicleConverter;
	using ERule = FMantleVehicleConverter::EPaintGuessRule;
	// One guess per rule, which the routes and the builder window share: the window showed the names' layers on a car Build sends through the converter, where an
	// interior merely named so keeps its own material.
	UMaterialInterface* Plain = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
	const FConverter::FPaintLayerGuess NamedNarrow = FConverter::GuessStartingPaint(TEXT("SUV_Interior"), Plain, ERule::TintLayersOnly);
	const FConverter::FPaintLayerGuess NamedWide = FConverter::GuessStartingPaint(TEXT("SUV_Interior"), Plain, ERule::ByNames);
	TestTrue(TEXT("by the names, an interior is on the interior layer"), NamedWide.bPainted && !NamedWide.bPrimary && NamedWide.Layer == EMantleVehicleSourcePaintLayer::Interior);
	TestTrue(TEXT("under the tint-layers rule it keeps its own material, and says so"), !NamedNarrow.bPainted && NamedNarrow.bKeepsOwnMaterial && NamedNarrow.LeftOut.Contains(TEXT("own material")));
	const FConverter::FPaintLayerGuess Gta = FConverter::GuessStartingPaint(TEXT("leather_1__SECONDARY_"), Plain, ERule::TintLayersOnly);
	TestTrue(TEXT("a GTA SECONDARY is the second paint under the tint-layers rule"), Gta.bPainted && !Gta.bPrimary && Gta.Layer == EMantleVehicleSourcePaintLayer::Secondary && !Gta.bKeepsOwnMaterial);
	for (const ERule Rule : { ERule::TintLayersOnly, ERule::ByNames })
	{
		const FConverter::FPaintLayerGuess Body = FConverter::GuessStartingPaint(TEXT("vehicle_generic__PRIMARY_"), Plain, Rule);
		TestTrue(TEXT("a GTA PRIMARY is the body paint under either rule"), Body.bPainted && Body.bPrimary);
		const FConverter::FPaintLayerGuess Glass = FConverter::GuessStartingPaint(TEXT("SUV_Glass"), Plain, Rule);
		TestTrue(TEXT("glass is never painted, under either rule"), !Glass.bPainted && !Glass.bKeepsOwnMaterial);
	}
	// The narrow rule is GuessPaintLayer with only that one change; the wide one is GuessPaintLayer as it reads.
	const FConverter::FPaintLayerGuess Plainly = FConverter::GuessPaintLayer(TEXT("SUV_Interior"), Plain);
	TestTrue(TEXT("by the names is GuessPaintLayer itself"), Plainly.bPainted == NamedWide.bPainted && Plainly.bPrimary == NamedWide.bPrimary && Plainly.Layer == NamedWide.Layer);
	// No model chosen in the window yet: the skin route's rule (nothing is fitted to its own wheels).
	TestTrue(TEXT("with no model, the window uses the names' rule"), FMantleVehicleBuilder::PaintGuessRuleFor(nullptr, TEXT("sedan")) == ERule::ByNames);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleConverterBothLightHalvesTest,
	"Mantle.Vehicle.Convert.BothHalvesOfALightStayOnTheBody",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleConverterBothLightHalvesTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleConverterWheelsTestPrivate;

	// SUV04's lights come in two sections each (a second glow, Headlights and Headlights2). While the body has room, both are on it and both are listed for their kind; a body
	// past the limit keeps them too when it has room (the limit is 64, and a light outranks the glass and the interior for it).
	for (const int32 Slots : { 6, PastTheLimit })
	{
		TArray<FString> Names;
		TArray<UMaterialInterface*> Materials;
		Names.SetNum(Slots);
		Materials.SetNumZeroed(Slots);
		const TCHAR* const Lamps[4] = { TEXT("headlight_lens"), TEXT("headlight_lens2"), TEXT("taillight_lens"), TEXT("taillight_lens2") };
		for (int32 Lamp = 0; Lamp < 4; ++Lamp)
		{
			Names[Lamp] = Lamps[Lamp];
			Materials[Lamp] = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
		}
		FString Error;
		USkeletalMesh* Car = MakeCar(FourWheels(TEXT("B_FLWheel"), TEXT("B_FRWheel"), TEXT("B_RLWheel"), TEXT("B_RRWheel")), Slots, Error, nullptr, Names, Materials);
		if (!TestNotNull(*FString::Printf(TEXT("the car of %d slots is made (%s)"), Slots, *Error), Car))
		{
			return false;
		}
		UMantleVehicleSource* Source = NewObject<UMantleVehicleSource>(GetTransientPackage(), NAME_None, RF_Transient);
		const FMantleVehicleConversion Conversion = FMantleVehicleConverter::Convert(Car, *Source);
		TestTrue(*FString::Printf(TEXT("%d slots: it converts (%s)"), Slots, *FString::Join(Conversion.Problems, TEXT(" "))), Conversion.bSuccess);
		TestTrue(*FString::Printf(TEXT("%d slots: both headlight sections are listed"), Slots), Source->Lights.Head.Contains(TEXT("headlight_lens")) && Source->Lights.Head.Contains(TEXT("headlight_lens2")));
		TestTrue(*FString::Printf(TEXT("%d slots: both rear light sections are listed"), Slots), Source->Lights.Rear.Contains(TEXT("taillight_lens")) && Source->Lights.Rear.Contains(TEXT("taillight_lens2")));
		USkeletalMesh* Body = Source->Body.LoadSynchronous();
		if (TestNotNull(TEXT("it has a body"), Body))
		{
			const TArray<FString> OnBody = FMantleVehicleLights::SectionNames(Body->GetMaterials());
			bool bAll = true;
			for (const TCHAR* Lamp : Lamps)
			{
				bAll = bAll && OnBody.Contains(Lamp);
			}
			TestTrue(*FString::Printf(TEXT("%d slots: all four light sections are on the body"), Slots), bAll);
		}
		if (Body && Body != Car)
		{
			MantleVehicleScratchBody::Discard(Body);
		}
		MantleVehicleScratchBody::Discard(Car);
	}
	return true;
}

#endif
