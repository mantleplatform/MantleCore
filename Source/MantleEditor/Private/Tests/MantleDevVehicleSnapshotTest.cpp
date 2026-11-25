// Copyright Joshua Eger. All Rights Reserved.
#include "MantleDevVehicleSnapshot.h"
#include "MantleVehicleScratchBody.h"
#include "MantleVehicleSource.h"

#include "Engine/SkeletalMesh.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// The car check's snapshot (Mantle.Dev.VehicleSnapshot) is stable: the same vehicle always writes the same text, keys sorted, lists of names sorted but
// liveries in their order, numbers rounded to 3 decimals, the library picture left out; and it carries the body's sections and bones.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleDevVehicleSnapshotStableTest,
	"Mantle.Vehicle.Dev.SnapshotIsStable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleDevVehicleSnapshotStableTest::RunTest(const FString&)
{
	TestEqual(TEXT("1.5"), FMantleDevVehicleSnapshot::Number(1.5), FString(TEXT("1.5")));
	TestEqual(TEXT("100"), FMantleDevVehicleSnapshot::Number(100.0), FString(TEXT("100")));
	TestEqual(TEXT("12.34567 rounds"), FMantleDevVehicleSnapshot::Number(12.34567), FString(TEXT("12.346")));
	TestEqual(TEXT("-2.25"), FMantleDevVehicleSnapshot::Number(-2.25), FString(TEXT("-2.25")));
	TestEqual(TEXT("a tiny negative is 0"), FMantleDevVehicleSnapshot::Number(-0.0001), FString(TEXT("0")));

	FString Error;
	USkeletalMesh* Body = MantleVehicleScratchBody::Make(TEXT("/Temp/MantleDevVehicleSnapshotTest/Body"), TEXT("Body"), Error);
	if (!TestNotNull(*FString::Printf(TEXT("scratch body (%s)"), *Error), Body))
	{
		return false;
	}
	UMantleVehicleSource* Source = NewObject<UMantleVehicleSource>(GetTransientPackage());
	Source->Body = Body;
	Source->Paint.Sections = { TEXT("b_section"), TEXT("a_section") };
	Source->Paint.LiveryNames = { TEXT("Zebra"), TEXT("Arrow") };
	Source->Lights.Head = { TEXT("lamp_y"), TEXT("lamp_x") };
	FMantleVehicleSourceWheel& Wheel = Source->Wheels.AddDefaulted_GetRef();
	Wheel.Bone = TEXT("wheel_fl");
	Wheel.RimRadius = 12.34567f;
	Source->Tuning.SteeringWheelBone = TEXT("steering_wheel");
	Source->ThumbnailPng = { 1, 2, 3 };

	const FString Text = FMantleDevVehicleSnapshot::ToText(*Source);
	TestEqual(TEXT("the same vehicle writes the same text"), FMantleDevVehicleSnapshot::ToText(*Source), Text);
	TestFalse(TEXT("the library picture is left out"), Text.Contains(TEXT("ThumbnailPng")));
	TestTrue(TEXT("numbers rounded"), Text.Contains(TEXT("\"RimRadius\": 12.346")));
	TestTrue(TEXT("the steering wheel's bone"), Text.Contains(TEXT("\"SteeringWheelBone\": \"steering_wheel\"")));
	TestTrue(TEXT("keys sorted"), Text.Find(TEXT("\"Bones\"")) < Text.Find(TEXT("\"Wheels\"")) && Text.Find(TEXT("\"Lights\"")) < Text.Find(TEXT("\"Paint\"")));

	const TSharedRef<FJsonObject> Json = FMantleDevVehicleSnapshot::Make(*Source);
	const auto Strings = [](const TArray<TSharedPtr<FJsonValue>>& Items)
	{
		TArray<FString> Out;
		for (const TSharedPtr<FJsonValue>& Item : Items)
		{
			Out.Add(Item->AsString());
		}
		return FString::Join(Out, TEXT(","));
	};
	const TSharedPtr<FJsonObject> Paint = Json->GetObjectField(TEXT("Paint"));
	TestEqual(TEXT("paint sections sorted"), Strings(Paint->GetArrayField(TEXT("Sections"))), FString(TEXT("a_section,b_section")));
	TestEqual(TEXT("livery names keep their order"), Strings(Paint->GetArrayField(TEXT("LiveryNames"))), FString(TEXT("Zebra,Arrow")));
	TestEqual(TEXT("head lights sorted"), Strings(Json->GetObjectField(TEXT("Lights"))->GetArrayField(TEXT("Head"))), FString(TEXT("lamp_x,lamp_y")));

	TestEqual(TEXT("a section per body material"), Json->GetArrayField(TEXT("Sections")).Num(), Body->GetMaterials().Num());
	TestEqual(TEXT("every body bone"), Json->GetArrayField(TEXT("Bones")).Num(), Body->GetRefSkeleton().GetNum());
	for (const TSharedPtr<FJsonValue>& Section : Json->GetArrayField(TEXT("Sections")))
	{
		TestTrue(TEXT("a section has its name, material and built-in material"), Section->AsObject()->HasField(TEXT("Name")) && Section->AsObject()->HasField(TEXT("Material"))
			&& Section->AsObject()->HasField(TEXT("BuiltIn")));
	}

	MantleVehicleScratchBody::Discard(Body);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
