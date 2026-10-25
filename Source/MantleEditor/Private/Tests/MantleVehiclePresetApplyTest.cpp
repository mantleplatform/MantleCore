// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehiclePreset.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MantleVehicleConverter.h"
#include "MantleVehicleSource.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// Applying a vehicle preset's defaults to a vehicle source: ApplyDefaults is the inverse of
// FMantleVehicleConverter::MakeDocument for every value a preset carries, leaves everything else alone, and refuses a
// defaults key it does not know.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVehiclePresetApplyTestPrivate
{
	/** Checks that Actual holds what Expected holds: numbers within 1e-4, booleans equal, arrays element by element. */
	void ExpectSame(FAutomationTestBase& Test, const FString& Path, const TSharedPtr<FJsonValue>& Expected, const TSharedPtr<FJsonValue>& Actual)
	{
		if (!Actual.IsValid())
		{
			Test.AddError(FString::Printf(TEXT("%s is missing from the document"), *Path));
			return;
		}
		const TArray<TSharedPtr<FJsonValue>>* ExpectedItems = nullptr;
		if (Expected->TryGetArray(ExpectedItems) && ExpectedItems)
		{
			const TArray<TSharedPtr<FJsonValue>>* ActualItems = nullptr;
			if (!Actual->TryGetArray(ActualItems) || !ActualItems)
			{
				Test.AddError(FString::Printf(TEXT("%s is not an array in the document"), *Path));
				return;
			}
			if (ActualItems->Num() != ExpectedItems->Num())
			{
				Test.AddError(FString::Printf(TEXT("%s has %d items in the document, the preset has %d"), *Path, ActualItems->Num(), ExpectedItems->Num()));
				return;
			}
			for (int32 Index = 0; Index < ExpectedItems->Num(); ++Index)
			{
				ExpectSame(Test, FString::Printf(TEXT("%s/%d"), *Path, Index), (*ExpectedItems)[Index], (*ActualItems)[Index]);
			}
			return;
		}
		bool ExpectedBool = false;
		if (Expected->Type == EJson::Boolean && Expected->TryGetBool(ExpectedBool))
		{
			bool ActualBool = false;
			Test.TestTrue(FString::Printf(TEXT("%s is a boolean in the document"), *Path), Actual->TryGetBool(ActualBool));
			Test.TestEqual(FString::Printf(TEXT("%s"), *Path), ActualBool, ExpectedBool);
			return;
		}
		double ExpectedNumber = 0.0;
		double ActualNumber = 0.0;
		if (!Expected->TryGetNumber(ExpectedNumber) || !Actual->TryGetNumber(ActualNumber))
		{
			Test.AddError(FString::Printf(TEXT("%s is not a number in the preset or in the document"), *Path));
			return;
		}
		Test.TestNearlyEqual(FString::Printf(TEXT("%s"), *Path), ActualNumber, ExpectedNumber, 1e-4);
	}

	/** Walks the preset's defaults and compares every leaf with the same path in the document. */
	void ExpectFragment(FAutomationTestBase& Test, const FString& Prefix, const TSharedPtr<FJsonObject>& Defaults, const TSharedPtr<FJsonObject>& Document)
	{
		for (const auto& Pair : Defaults->Values) // keyed by UE::TSharedString in 5.8, not FString
		{
			const FString Key(Pair.Key);
			const FString Path = Prefix.IsEmpty() ? Key : Prefix + TEXT("/") + Key;
			const TSharedPtr<FJsonObject>* ChildDefaults = nullptr;
			if (Pair.Value->TryGetObject(ChildDefaults) && ChildDefaults)
			{
				const TSharedPtr<FJsonObject>* ChildDocument = nullptr;
				if (!Document.IsValid() || !Document->TryGetObjectField(Key, ChildDocument) || !ChildDocument)
				{
					Test.AddError(FString::Printf(TEXT("%s is missing from the document"), *Path));
					continue;
				}
				ExpectFragment(Test, Path, *ChildDefaults, *ChildDocument);
				continue;
			}
			const TSharedPtr<FJsonValue> Actual = Document.IsValid() ? Document->TryGetField(Key) : nullptr;
			ExpectSame(Test, Path, Pair.Value, Actual);
		}
	}

	/** A minimal valid preset with the given defaults object text. */
	FString PresetWithDefaults(const FString& Defaults)
	{
		return FString::Printf(TEXT("{ \"schemaVersion\": 1, \"id\": \"x\", \"name\": \"Test Car\", \"movementType\": \"wheeled\", \"fit\": { \"wheelRadius\": 30, \"trackWidth\": 160, \"minScale\": 0.5, \"maxScale\": 2 }, \"defaults\": %s }"), *Defaults);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehiclePresetApplyRoundTripTest,
	"Mantle.Vehicle.Presets.ApplyRoundTrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehiclePresetApplyRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehiclePresetApplyTestPrivate;
	TestTrue(TEXT("there are presets to check"), FMantleVehiclePresets::All().Num() >= 2);
	for (const FMantleVehiclePreset& Preset : FMantleVehiclePresets::All())
	{
		UMantleVehicleSource* Source = NewObject<UMantleVehicleSource>(GetTransientPackage());
		TArray<FString> Notes;
		TestTrue(FString::Printf(TEXT("%s applies"), *Preset.Id), FMantleVehiclePresets::ApplyDefaults(Preset, *Source, Notes));
		TestTrue(FString::Printf(TEXT("%s says what it set"), *Preset.Id), Notes.Num() >= 1 && Notes.Num() <= 2);
		if (!Preset.Defaults.IsValid())
		{
			continue;
		}
		const TSharedRef<FJsonObject> Document = FMantleVehicleConverter::MakeDocument(*Source, TMap<FString, FString>());
		ExpectFragment(*this, Preset.Id, Preset.Defaults, Document);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehiclePresetApplyLeavesOthersTest,
	"Mantle.Vehicle.Presets.ApplyLeavesOthers",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehiclePresetApplyLeavesOthersTest::RunTest(const FString& Parameters)
{
	const FMantleVehiclePreset* Sedan = FMantleVehiclePresets::Find(TEXT("sedan"));
	if (!TestNotNull(TEXT("the sedan preset loads"), Sedan))
	{
		return false;
	}
	UMantleVehicleSource* Source = NewObject<UMantleVehicleSource>(GetTransientPackage());
	FMantleVehicleSourceTuning& T = Source->Tuning;
	T.bSeatLocation = true;
	T.SeatLocation = FVector(1.0, 2.0, 3.0);
	T.bPassengerSeat = true;
	T.PassengerSeatLocation = FVector(4.0, 5.0, 6.0);
	T.bTrunk = true;
	T.TrunkYaw = 90.f;
	T.bHasDriverDoor = true;
	T.DriverDoor.Bone = TEXT("Door_L");
	T.Collision.AddDefaulted();
	T.Collision[0].Center = FVector(7.0, 8.0, 9.0);
	FMantleVehicleSourceWheel& Wheel = Source->Wheels.AddDefaulted_GetRef();
	Wheel.Bone = TEXT("Wheel_FL");
	Wheel.Radius = 33.f;
	Wheel.bSteering = true;
	Source->Paint.Metallic = 0.9f;
	Source->Paint.Sections.Add(TEXT("Body"));

	TArray<FString> Notes;
	TestTrue(TEXT("the sedan applies"), FMantleVehiclePresets::ApplyDefaults(*Sedan, *Source, Notes));

	TestTrue(TEXT("seat location is still on"), T.bSeatLocation);
	TestTrue(TEXT("seat location"), T.SeatLocation.Equals(FVector(1.0, 2.0, 3.0)));
	TestTrue(TEXT("passenger seat is still on"), T.bPassengerSeat);
	TestTrue(TEXT("passenger seat location"), T.PassengerSeatLocation.Equals(FVector(4.0, 5.0, 6.0)));
	TestTrue(TEXT("trunk is still on"), T.bTrunk);
	TestEqual(TEXT("trunk yaw"), T.TrunkYaw, 90.f);
	TestEqual(TEXT("driver door bone"), T.DriverDoor.Bone.ToString(), FString(TEXT("Door_L")));
	TestEqual(TEXT("collision box count"), T.Collision.Num(), 1);
	if (T.Collision.Num() == 1)
	{
		TestTrue(TEXT("collision box centre"), T.Collision[0].Center.Equals(FVector(7.0, 8.0, 9.0)));
	}
	TestEqual(TEXT("wheel count"), Source->Wheels.Num(), 1);
	if (Source->Wheels.Num() == 1)
	{
		TestEqual(TEXT("wheel bone"), Source->Wheels[0].Bone.ToString(), FString(TEXT("Wheel_FL")));
		TestEqual(TEXT("wheel radius"), Source->Wheels[0].Radius, 33.f);
		TestTrue(TEXT("wheel still steers"), Source->Wheels[0].bSteering);
	}
	TestEqual(TEXT("paint metallic"), Source->Paint.Metallic, 0.9f);
	TestEqual(TEXT("paint sections"), Source->Paint.Sections.Num(), 1);
	TestTrue(TEXT("the body is still unset"), Source->Body.IsNull());
	// The preset carries no centre of mass, so the creator's choice (off) stays.
	TestFalse(TEXT("centre of mass override is untouched"), T.bOverrideCenterOfMass);
	// And the values it does carry did change.
	TestEqual(TEXT("mass applied"), T.Mass, 1500.f);
	TestEqual(TEXT("max speed applied"), T.MaxSpeed, 4500.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehiclePresetApplyRejectsUnknownTest,
	"Mantle.Vehicle.Presets.ApplyRejectsUnknown",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehiclePresetApplyRejectsUnknownTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehiclePresetApplyTestPrivate;
	const TCHAR* Cases[] = {
		TEXT("{ \"mass\": 1000, \"warpDrive\": 5 }"),
		TEXT("{ \"mass\": 1000, \"engine\": { \"maxTorque\": 300, \"turbo\": 1 } }") };
	const TCHAR* Keys[] = { TEXT("warpDrive"), TEXT("engine/turbo") };
	for (int32 Index = 0; Index < 2; ++Index)
	{
		FMantleVehiclePreset Preset;
		FString Error;
		if (!TestTrue(TEXT("the inline preset parses"), FMantleVehiclePresets::Parse(PresetWithDefaults(Cases[Index]), Preset, Error)))
		{
			AddError(Error);
			continue;
		}
		UMantleVehicleSource* Source = NewObject<UMantleVehicleSource>(GetTransientPackage());
		const float MassBefore = Source->Tuning.Mass;
		TArray<FString> Notes;
		TestFalse(TEXT("an unknown key is rejected"), FMantleVehiclePresets::ApplyDefaults(Preset, *Source, Notes));
		const FString All = FString::Join(Notes, TEXT("\n"));
		TestTrue(FString::Printf(TEXT("the note names %s"), Keys[Index]), All.Contains(Keys[Index]));
		TestEqual(TEXT("nothing was applied, not even the known keys"), Source->Tuning.Mass, MassBefore);
	}

	// A known key holding the wrong kind of value is refused too.
	FMantleVehiclePreset Wrong;
	FString Error;
	if (TestTrue(TEXT("the wrong-kind preset parses"), FMantleVehiclePresets::Parse(PresetWithDefaults(TEXT("{ \"mass\": \"heavy\" }")), Wrong, Error)))
	{
		UMantleVehicleSource* Source = NewObject<UMantleVehicleSource>(GetTransientPackage());
		TArray<FString> Notes;
		TestFalse(TEXT("a wrong-kind value is rejected"), FMantleVehiclePresets::ApplyDefaults(Wrong, *Source, Notes));
		TestTrue(TEXT("the note names mass"), FString::Join(Notes, TEXT("\n")).Contains(TEXT("mass")));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
