// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleTypeMatch.h"

#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// The mismatch check (#821 flow, step 3): a model against the type the creator picked. The first test uses models made in the test, as arrays. The second loads the
// Fab pack's car, bike and plane when the project has them (the MantleVehicles project does); in any other project it says so and passes.

namespace MantleVehicleTypeMatchTestPrivate
{
	/** A model with a box of Size for its geometry and the given bones (name, parent, position); a skeletal one when any are given. */
	FMantleVehicleModel MakeModel(const FVector& Size, const TArray<TTuple<FName, int32, FVector>>& Bones = {}, const TArray<FName>& MaterialSlots = {})
	{
		FMantleVehicleModel Model;
		Model.Name = TEXT("Test");
		const FVector Half = 0.5 * Size;
		for (int32 Corner = 0; Corner < 8; ++Corner)
		{
			Model.LOD.Positions.Add(FVector3f(Corner & 1 ? Half.X : -Half.X, Corner & 2 ? Half.Y : -Half.Y, Corner & 4 ? Size.Z : 0.0));
		}
		for (const TTuple<FName, int32, FVector>& Bone : Bones)
		{
			Model.Bones.Add({ Bone.Get<0>(), Bone.Get<1>() });
			Model.BonePositions.Add(Bone.Get<2>());
		}
		Model.bSkeletal = Bones.Num() > 0;
		for (const FName Slot : MaterialSlots)
		{
			Model.Materials.Add(FSkeletalMaterial(static_cast<UMaterialInterface*>(nullptr), Slot));
		}
		return Model;
	}

	/** A car: four wheel bones at the corners of a 270 cm by 160 cm rectangle. */
	FMantleVehicleModel Car()
	{
		return MakeModel(FVector(450, 190, 140), { { FName("root"), INDEX_NONE, FVector::ZeroVector },
			{ FName("Wheel_FL"), 0, FVector(135, -80, 35) }, { FName("Wheel_FR"), 0, FVector(135, 80, 35) },
			{ FName("Wheel_BL"), 0, FVector(-135, -80, 35) }, { FName("Wheel_BR"), 0, FVector(-135, 80, 35) } });
	}

	/** A motorcycle: two wheel bones in a line, a fork above the front one. */
	FMantleVehicleModel Bike()
	{
		return MakeModel(FVector(210, 80, 120), { { FName("root"), INDEX_NONE, FVector::ZeroVector },
			{ FName("wheel_f"), 0, FVector(70, 0, 35) }, { FName("wheel_r"), 0, FVector(-70, 0, 35) }, { FName("fork"), 0, FVector(70, 0, 80) } });
	}

	/** A twin-engined plane: propellers and tail surfaces, and landing gear wheels (three of them) on top. */
	FMantleVehicleModel Plane()
	{
		return MakeModel(FVector(1150, 1580, 300), { { FName("root"), INDEX_NONE, FVector::ZeroVector },
			{ FName("prop_L"), 0, FVector(300, -300, 100) }, { FName("prop_R"), 0, FVector(300, 300, 100) }, { FName("aileron_L"), 0, FVector(0, -500, 100) },
			{ FName("Wheel_nose"), 0, FVector(250, 0, 30) }, { FName("Wheel_L"), 0, FVector(-50, -200, 30) }, { FName("Wheel_R"), 0, FVector(-50, 200, 30) } });
	}

	FString Describe(const FMantleVehicleTypeMatch& Match)
	{
		return FString::Printf(TEXT("verdict %d, suggests '%s', %s"), static_cast<int32>(Match.Verdict), *Match.SuggestedType, *Match.Message);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleTypeMatchModelsTest,
	"Mantle.Vehicle.TypeMatch.ModelsAgainstPickedTypes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleTypeMatchModelsTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleTypeMatchTestPrivate;
	using Check = FMantleVehicleTypeMatchCheck;

	// Each model picked as its own type fits, whichever of the group's types was picked.
	for (const TCHAR* Type : { TEXT("sedan"), TEXT("sports"), TEXT("limousine"), TEXT("pickup"), TEXT("van"), TEXT("truck"), TEXT("sixwheeler") })
	{
		const FMantleVehicleTypeMatch Match = Check::Check(Car(), Type);
		TestEqual(*FString::Printf(TEXT("a car fits %s (%s)"), Type, *Describe(Match)), Match.Verdict, EMantleVehicleTypeVerdict::Fits);
	}
	TestEqual(TEXT("a bike fits motorcycle"), Check::Check(Bike(), TEXT("motorcycle")).Verdict, EMantleVehicleTypeVerdict::Fits);
	TestEqual(TEXT("a plane fits plane"), Check::Check(Plane(), TEXT("plane")).Verdict, EMantleVehicleTypeVerdict::Fits);
	TestEqual(TEXT("a plane fits jet"), Check::Check(Plane(), TEXT("jet")).Verdict, EMantleVehicleTypeVerdict::Fits);

	// Each picked as a wrong type gets the right suggestion, with a button and a plain message.
	const auto Expect = [this](const FMantleVehicleModel& Model, const TCHAR* Picked, const TCHAR* Wanted, const TCHAR* Word)
	{
		const FMantleVehicleTypeMatch Match = Check::Check(Model, Picked);
		TestTrue(*FString::Printf(TEXT("picked %s suggests %s (%s)"), Picked, Wanted, *Describe(Match)), Match.Suggests() && Match.SuggestedType == Wanted);
		TestTrue(*FString::Printf(TEXT("and the button reads Switch to ... (%s)"), *Match.ButtonText), Match.ButtonText.StartsWith(TEXT("Switch to ")));
		TestTrue(*FString::Printf(TEXT("and the message says why (%s)"), *Match.Message), Match.Message.Contains(Word) && Match.Evidence.Num() > 0);
	};
	Expect(Car(), TEXT("motorcycle"), TEXT("sedan"), TEXT("4 wheels"));
	Expect(Car(), TEXT("plane"), TEXT("sedan"), TEXT("4 wheels"));
	Expect(Car(), TEXT("helicopter"), TEXT("sedan"), TEXT("car"));
	Expect(Bike(), TEXT("sedan"), TEXT("motorcycle"), TEXT("in a line"));
	Expect(Bike(), TEXT("truck"), TEXT("motorcycle"), TEXT("motorcycle"));
	Expect(Plane(), TEXT("sedan"), TEXT("plane"), TEXT("propeller"));
	Expect(Plane(), TEXT("motorcycle"), TEXT("plane"), TEXT("plane"));
	Expect(Plane(), TEXT("hovercraft"), TEXT("plane"), TEXT("wings"));

	// A bike whose steering bone is called a rudder and whose wheels are called FR and RR (the Fab pack's) is still a bike: it has brake levers.
	const FMantleVehicleModel OddBike = MakeModel(FVector(270, 123, 174), { { FName("root"), INDEX_NONE, FVector::ZeroVector }, { FName("RR"), 0, FVector(-74, 0, 40) },
		{ FName("rudder_bone"), 0, FVector(53, 0, 144) }, { FName("FR"), 2, FVector(108, 0, 40) }, { FName("brake-levers_L"), 2, FVector(53, -34, 163) } });
	TestEqual(TEXT("a rudder bone alone is no plane: the odd bike fits motorcycle"), Check::Check(OddBike, TEXT("motorcycle")).Verdict, EMantleVehicleTypeVerdict::Fits);
	TestEqual(TEXT("and picked as a sedan suggests motorcycle"), Check::Check(OddBike, TEXT("sedan")).SuggestedType, FString(TEXT("motorcycle")));

	// A rotor is a helicopter's; a car's brake rotor is not.
	const FMantleVehicleModel Heli = MakeModel(FVector(900, 300, 350), { { FName("root"), INDEX_NONE, FVector::ZeroVector }, { FName("MainRotor"), 0, FVector(0, 0, 300) }, { FName("TailRotor"), 0, FVector(-400, 0, 200) } });
	TestEqual(TEXT("a helicopter fits helicopter"), Check::Check(Heli, TEXT("helicopter")).Verdict, EMantleVehicleTypeVerdict::Fits);
	TestEqual(TEXT("and picked as a plane suggests helicopter"), Check::Check(Heli, TEXT("plane")).SuggestedType, FString(TEXT("helicopter")));
	FMantleVehicleModel CarWithBrakes = Car();
	CarWithBrakes.Bones.Add({ FName("BrakeRotor_FL"), 0 });
	CarWithBrakes.BonePositions.Add(FVector(135, -80, 35));
	TestEqual(TEXT("a brake rotor is no helicopter"), Check::Check(CarWithBrakes, TEXT("sedan")).Verdict, EMantleVehicleTypeVerdict::Fits);

	// A sports car's rear wing is no airplane wing, and a wheel's brake bone under the wheel is the same wheel.
	FMantleVehicleModel Sports = Car();
	Sports.Bones.Add({ FName("rear_wing"), 0 });
	Sports.BonePositions.Add(FVector(-200, 0, 120));
	Sports.Bones.Add({ FName("Wheel_FL_Brake"), 1 });
	Sports.BonePositions.Add(FVector(135, -80, 35));
	TestEqual(TEXT("a sports car with a rear wing and brakes is still a car"), Check::Check(Sports, TEXT("sports")).Verdict, EMantleVehicleTypeVerdict::Fits);

	// No names at all (a static mesh): proportions are a weak sign, and only for cars and bikes.
	const FMantleVehicleModel PlainBike = MakeModel(FVector(210, 60, 120));
	const FMantleVehicleTypeMatch Weak = Check::Check(PlainBike, TEXT("sedan"));
	TestTrue(*FString::Printf(TEXT("a narrow, tall static model picked as a sedan may be a motorcycle (%s)"), *Describe(Weak)),
		Weak.Suggests() && Weak.SuggestedType == TEXT("motorcycle") && Weak.Confidence == EMantleVehicleTypeConfidence::Weak && Weak.Message.Contains(TEXT("may be")));
	TestEqual(TEXT("a plain car-shaped box picked as a sedan fits"), Check::Check(MakeModel(FVector(450, 190, 140)), TEXT("sedan")).Verdict, EMantleVehicleTypeVerdict::Fits);
	TestEqual(TEXT("a plain car-shaped box picked as a plane is let through (proportions cannot say what a plane is)"),
		Check::Check(MakeModel(FVector(450, 190, 140)), TEXT("plane")).Verdict, EMantleVehicleTypeVerdict::Fits);
	TestTrue(TEXT("a model with wheel materials and a car's proportions picked as a motorcycle suggests a car"),
		Check::Check(MakeModel(FVector(450, 190, 140), {}, { FName("M_Body"), FName("M_Wheel") }), TEXT("motorcycle")).SuggestedType == TEXT("sedan"));

	// An unknown type or an empty model never refuses; it says so.
	TestEqual(TEXT("an unknown type is unsure"), Check::Check(Car(), TEXT("spaceship")).Verdict, EMantleVehicleTypeVerdict::Unsure);
	TestEqual(TEXT("an empty model is unsure"), Check::Check(FMantleVehicleModel(), TEXT("sedan")).Verdict, EMantleVehicleTypeVerdict::Unsure);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleTypeMatchFabPackTest,
	"Mantle.Vehicle.TypeMatch.FabPackCarBikeAndPlane",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleTypeMatchFabPackTest::RunTest(const FString& Parameters)
{
	struct FCase { const TCHAR* Path; const TCHAR* Right; const TCHAR* Wrong; const TCHAR* WrongSuggests; };
	const FCase Cases[] = {
		{ TEXT("/Game/VehicleVarietyPack/Skeletons/SK_SportsCar.SK_SportsCar"), TEXT("sports"), TEXT("motorcycle"), TEXT("sedan") },
		{ TEXT("/Game/VehicleVarietyPack/Skeletons/SK_Hatchback.SK_Hatchback"), TEXT("sedan"), TEXT("plane"), TEXT("sedan") },
		{ TEXT("/Game/Bike/Models/SK_rsg_bike_UE_04.SK_rsg_bike_UE_04"), TEXT("motorcycle"), TEXT("sedan"), TEXT("motorcycle") },
		{ TEXT("/Game/WW2Assets/Airplanes/P38_Lightning/Models/SK_RSG_Plane_P38_Lightning.SK_RSG_Plane_P38_Lightning"), TEXT("plane"), TEXT("sedan"), TEXT("plane") },
	};
	int32 Found = 0;
	for (const FCase& Case : Cases)
	{
		UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, Case.Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (!Asset)
		{
			continue;
		}
		++Found;
		FMantleVehicleTypeMatch Right;
		FMantleVehicleTypeMatch Wrong;
		FString Error;
		if (!TestTrue(*FString::Printf(TEXT("%s is read (%s)"), Case.Path, *Error), FMantleVehicleTypeMatchCheck::CheckAsset(Asset, Case.Right, Right, Error)))
		{
			continue;
		}
		FMantleVehicleTypeMatchCheck::CheckAsset(Asset, Case.Wrong, Wrong, Error);
		TestEqual(*FString::Printf(TEXT("%s fits %s (%s)"), Case.Path, Case.Right, *MantleVehicleTypeMatchTestPrivate::Describe(Right)), Right.Verdict, EMantleVehicleTypeVerdict::Fits);
		TestTrue(*FString::Printf(TEXT("%s picked as %s suggests %s (%s)"), Case.Path, Case.Wrong, Case.WrongSuggests, *MantleVehicleTypeMatchTestPrivate::Describe(Wrong)),
			Wrong.Suggests() && Wrong.SuggestedType == Case.WrongSuggests);
	}
	if (Found == 0)
	{
		AddInfo(TEXT("The Fab pack car, bike and plane are not in this project (run this in the MantleVehicles project), so nothing was checked."));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
