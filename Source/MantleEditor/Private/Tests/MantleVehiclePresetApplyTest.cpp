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
		// A string (#445: engine/drive) is the same string.
		if (Expected->Type == EJson::String)
		{
			Test.TestTrue(FString::Printf(TEXT("%s is a string in the document"), *Path), Actual->Type == EJson::String);
			Test.TestEqual(FString::Printf(TEXT("%s"), *Path), Actual->AsString(), Expected->AsString());
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
	T.bHood = true;
	T.HoodYaw = 170.f;
	T.bRearLeftDoor = true;
	T.RearLeftDoor.Bone = TEXT("Door_RL");
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
	TestTrue(TEXT("hood is still on"), T.bHood);
	TestEqual(TEXT("hood yaw"), T.HoodYaw, 170.f);
	TestTrue(TEXT("rear left door is still on"), T.bRearLeftDoor);
	TestEqual(TEXT("rear left door bone"), T.RearLeftDoor.Bone.ToString(), FString(TEXT("Door_RL")));
	TestEqual(TEXT("driver door bone"), T.DriverDoor.Bone.ToString(), FString(TEXT("Door_L")));
	TestEqual(TEXT("collision box count"), T.Collision.Num(), 1);
	if (T.Collision.Num() == 1)
	{
		TestTrue(TEXT("collision box center"), T.Collision[0].Center.Equals(FVector(7.0, 8.0, 9.0)));
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
	// The preset carries no center of mass, so the creator's choice (off) stays.
	TestFalse(TEXT("center of mass override is untouched"), T.bOverrideCenterOfMass);
	// And the values it does carry did change.
	TestEqual(TEXT("mass applied"), T.Mass, 1546.f);
	TestEqual(TEXT("max speed applied"), T.MaxSpeed, 5944.f);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehiclePresetDriverAidsTest,
	"Mantle.Vehicle.Presets.DriverAids",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehiclePresetDriverAidsTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehiclePresetApplyTestPrivate;
	// Cars step 7 (#376): ABS, traction control, brake balance and engine braking are part of the car. A preset's are read in and written out.
	FMantleVehiclePreset Preset;
	FString Error;
	const TCHAR* Defaults = TEXT("{ \"brakes\": { \"abs\": false, \"frontShare\": 0.55 }, \"engine\": { \"tractionControl\": false, \"engineBraking\": 0.3 }, \"transmission\": { \"shiftTime\": 0.15 } }");
	if (TestTrue(TEXT("the inline preset parses"), FMantleVehiclePresets::Parse(PresetWithDefaults(Defaults), Preset, Error)))
	{
		UMantleVehicleSource* Source = NewObject<UMantleVehicleSource>(GetTransientPackage());
		TArray<FString> Notes;
		TestTrue(TEXT("the driver aids are known keys"), FMantleVehiclePresets::ApplyDefaults(Preset, *Source, Notes));
		const FMantleVehicleSourceTuning& T = Source->Tuning;
		TestFalse(TEXT("abs read in"), T.bABS);
		TestEqual(TEXT("frontShare read in"), T.BrakeFrontShare, 0.55f);
		TestFalse(TEXT("tractionControl read in"), T.bTractionControl);
		TestEqual(TEXT("engineBraking read in"), T.EngineBraking, 0.3f);
		TestEqual(TEXT("shiftTime read in"), T.ShiftTime, 0.15f);
		const TSharedRef<FJsonObject> Document = FMantleVehicleConverter::MakeDocument(*Source, TMap<FString, FString>());
		ExpectFragment(*this, TEXT("driver aids"), Preset.Defaults, Document);
	}

	// Absent: ABS and traction control on, 0.65 on the steered wheels, no engine braking; the document says the first three and leaves engine braking out.
	UMantleVehicleSource* Fresh = NewObject<UMantleVehicleSource>(GetTransientPackage());
	const FMantleVehicleSourceTuning& F = Fresh->Tuning;
	TestTrue(TEXT("absent: ABS on"), F.bABS);
	TestTrue(TEXT("absent: traction control on"), F.bTractionControl);
	TestEqual(TEXT("absent: 0.65 on the front"), F.BrakeFrontShare, 0.65f);
	TestEqual(TEXT("absent: no engine braking"), F.EngineBraking, 0.f);
	TestEqual(TEXT("absent: a gear change takes 0.5 s"), F.ShiftTime, 0.5f);
	const TSharedRef<FJsonObject> Plain = FMantleVehicleConverter::MakeDocument(*Fresh, TMap<FString, FString>());
	const TSharedPtr<FJsonObject>* Brakes = nullptr;
	const TSharedPtr<FJsonObject>* Engine = nullptr;
	if (TestTrue(TEXT("the document has brakes and an engine"), Plain->TryGetObjectField(TEXT("brakes"), Brakes) && Brakes && Plain->TryGetObjectField(TEXT("engine"), Engine) && Engine))
	{
		bool bABS = false, bTC = false;
		double Share = 0.0;
		TestTrue(TEXT("the document says ABS on"), (*Brakes)->TryGetBoolField(TEXT("abs"), bABS) && bABS);
		TestTrue(TEXT("the document says traction control on"), (*Engine)->TryGetBoolField(TEXT("tractionControl"), bTC) && bTC);
		TestTrue(TEXT("the document says 0.65 on the front"), (*Brakes)->TryGetNumberField(TEXT("frontShare"), Share) && FMath::IsNearlyEqual(Share, 0.65, 1e-4));
		TestFalse(TEXT("no engine braking is left out"), (*Engine)->HasField(TEXT("engineBraking")));
		const TSharedPtr<FJsonObject>* Transmission = nullptr;
		double Shift = 0.0;
		TestTrue(TEXT("the document says a gear change takes 0.5 s"), Plain->TryGetObjectField(TEXT("transmission"), Transmission) && Transmission
			&& (*Transmission)->TryGetNumberField(TEXT("shiftTime"), Shift) && FMath::IsNearlyEqual(Shift, 0.5, 1e-4));
	}

	// #444: a turbo is read in and written out whole; none leaves all four out; boost settings without an induction, or out of range, are refused.
	{
		FMantleVehiclePreset Turbo;
		const TCHAR* TurboDefaults = TEXT("{ \"engine\": { \"induction\": \"turbo\", \"maxBoost\": 17.4, \"spoolRPM\": 2300, \"spoolTime\": 0.6 } }");
		if (TestTrue(TEXT("the inline turbo preset parses"), FMantleVehiclePresets::Parse(PresetWithDefaults(TurboDefaults), Turbo, Error)))
		{
			UMantleVehicleSource* Source = NewObject<UMantleVehicleSource>(GetTransientPackage());
			TArray<FString> Notes;
			TestTrue(TEXT("the induction keys are known"), FMantleVehiclePresets::ApplyDefaults(Turbo, *Source, Notes));
			TestTrue(TEXT("turbo read in"), Source->Tuning.Induction == EMantleVehicleSourceInduction::Turbo);
			TestEqual(TEXT("spool rpm read in"), Source->Tuning.SpoolRPM, 2300);
			const TSharedRef<FJsonObject> Document = FMantleVehicleConverter::MakeDocument(*Source, TMap<FString, FString>());
			ExpectFragment(*this, TEXT("turbo"), Turbo.Defaults, Document);
			// A preset without an induction takes it away again, with the boost settings.
			FMantleVehiclePreset PlainPreset;
			if (TestTrue(TEXT("the inline plain preset parses"), FMantleVehiclePresets::Parse(PresetWithDefaults(TEXT("{ \"engine\": { \"maxTorque\": 300 } }")), PlainPreset, Error)))
			{
				TestTrue(TEXT("the plain preset applies"), FMantleVehiclePresets::ApplyDefaults(PlainPreset, *Source, Notes));
				TestTrue(TEXT("no induction after a preset without one"), Source->Tuning.Induction == EMantleVehicleSourceInduction::None);
				TestEqual(TEXT("and the boost back to its default"), Source->Tuning.MaxBoost, 10.f);
			}
		}
		UMantleVehicleSource* Bare = NewObject<UMantleVehicleSource>(GetTransientPackage());
		const TSharedRef<FJsonObject> NoneDoc = FMantleVehicleConverter::MakeDocument(*Bare, TMap<FString, FString>());
		const TSharedPtr<FJsonObject>* NoneEngine = nullptr;
		if (TestTrue(TEXT("a plain car's document has an engine"), NoneDoc->TryGetObjectField(TEXT("engine"), NoneEngine) && NoneEngine))
		{
			for (const TCHAR* Key : { TEXT("induction"), TEXT("maxBoost"), TEXT("spoolRPM"), TEXT("spoolTime") })
			{
				TestFalse(FString::Printf(TEXT("no induction: %s left out"), Key), (*NoneEngine)->HasField(Key));
			}
		}
		TArray<FMantleValidationIssue> BoostIssues;
		Bare->Tuning.MaxBoost = 15.f;
		FMantleVehicleConverter::CheckSource(Bare, FString(), BoostIssues);
		TestTrue(TEXT("boost settings without an induction are an error"), BoostIssues.ContainsByPredicate([](const FMantleValidationIssue& Issue)
		{
			return Issue.Severity == EMantleIssueSeverity::Error && Issue.Problem.Contains(TEXT("no Induction"));
		}));
		UMantleVehicleSource* WildTurbo = NewObject<UMantleVehicleSource>(GetTransientPackage());
		WildTurbo->Tuning.Induction = EMantleVehicleSourceInduction::Turbo;
		WildTurbo->Tuning.MaxBoost = 45.f;
		WildTurbo->Tuning.SpoolTime = 5.f;
		WildTurbo->Tuning.SpoolRPM = WildTurbo->Tuning.IdleRPM;
		TArray<FMantleValidationIssue> TurboIssues;
		FMantleVehicleConverter::CheckSource(WildTurbo, FString(), TurboIssues);
		for (const TCHAR* What : { TEXT("Max Boost"), TEXT("Spool Time") })
		{
			TestTrue(FString::Printf(TEXT("%s out of range is an error"), What), TurboIssues.ContainsByPredicate([What](const FMantleValidationIssue& Issue)
			{
				return Issue.Severity == EMantleIssueSeverity::Error && Issue.Problem.Contains(What);
			}));
		}
		TestTrue(TEXT("full boost at idle is an error"), TurboIssues.ContainsByPredicate([](const FMantleValidationIssue& Issue)
		{
			return Issue.Severity == EMantleIssueSeverity::Error && Issue.Problem.Contains(TEXT("full boost"));
		}));
	}

	// Out of range: the check says so, by name.
	UMantleVehicleSource* Wild = NewObject<UMantleVehicleSource>(GetTransientPackage());
	Wild->Tuning.BrakeFrontShare = 1.5f;
	Wild->Tuning.EngineBraking = 6.f;
	Wild->Tuning.ShiftTime = 3.f;
	TArray<FMantleValidationIssue> Issues;
	FMantleVehicleConverter::CheckSource(Wild, FString(), Issues);
	for (const TCHAR* What : { TEXT("Brake Front Share"), TEXT("Engine Braking"), TEXT("Shift Time") })
	{
		TestTrue(FString::Printf(TEXT("%s out of range is an error"), What), Issues.ContainsByPredicate([What](const FMantleValidationIssue& Issue)
		{
			return Issue.Severity == EMantleIssueSeverity::Error && Issue.Problem.Contains(What) && Issue.Problem.Contains(TEXT("outside what a vehicle may have"));
		}));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehiclePresetPedalTest,
	"Mantle.Vehicle.Presets.Pedal",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehiclePresetPedalTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehiclePresetApplyTestPrivate;
	// #447: a pedalled preset's pedal block and rolling resistance are read into the tuning and written back out.
	FMantleVehiclePreset Preset;
	FString Error;
	const TCHAR* Defaults = TEXT(R"({ "pedal": { "riderPower": 250, "sprintPower": 900, "sprintReserve": 15, "maxCadence": 200, "cadence": 85 }, "tires": { "friction": 1.12, "rollingResistance": 0.005 } })");
	if (TestTrue(TEXT("the inline preset parses"), FMantleVehiclePresets::Parse(PresetWithDefaults(Defaults), Preset, Error)))
	{
		UMantleVehicleSource* Source = NewObject<UMantleVehicleSource>(GetTransientPackage());
		TArray<FString> Notes;
		TestTrue(TEXT("the pedal keys are known"), FMantleVehiclePresets::ApplyDefaults(Preset, *Source, Notes));
		TestTrue(TEXT("pedalled"), Source->Tuning.bPedal);
		TestEqual(TEXT("250 W"), Source->Tuning.PedalRiderPower, 250.f);
		TestEqual(TEXT("0.005 rolling"), Source->Tuning.TireRollingResistance, 0.005f);
		const TSharedRef<FJsonObject> Document = FMantleVehicleConverter::MakeDocument(*Source, TMap<FString, FString>());
		ExpectFragment(*this, TEXT("pedal"), Preset.Defaults, Document);
		TArray<FMantleValidationIssue> Issues;
		FMantleVehicleConverter::CheckSource(Source, FString(), Issues);
		TestFalse(TEXT("no pedal error on the preset's numbers"), Issues.ContainsByPredicate([](const FMantleValidationIssue& Issue)
		{
			return Issue.Severity == EMantleIssueSeverity::Error && (Issue.Problem.Contains(TEXT("Power")) || Issue.Problem.Contains(TEXT("Cadence")));
		}));
	}

	// Not pedalled: no pedal block, so every other vehicle's document is what it was.
	UMantleVehicleSource* Fresh = NewObject<UMantleVehicleSource>(GetTransientPackage());
	TestFalse(TEXT("a fresh source isn't pedalled"), Fresh->Tuning.bPedal);
	TestFalse(TEXT("no pedal block"), FMantleVehicleConverter::MakeDocument(*Fresh, TMap<FString, FString>())->HasField(TEXT("pedal")));

	// The two rules: a sprint at least the held power, and the cadence 20 under the max.
	UMantleVehicleSource* Wrong = NewObject<UMantleVehicleSource>(GetTransientPackage());
	Wrong->Tuning.bPedal = true;
	Wrong->Tuning.PedalRiderPower = 400.f;
	Wrong->Tuning.PedalSprintPower = 300.f;
	Wrong->Tuning.PedalCadence = 190.f;
	Wrong->Tuning.PedalMaxCadence = 200.f;
	TArray<FMantleValidationIssue> Issues;
	FMantleVehicleConverter::CheckSource(Wrong, FString(), Issues);
	TestTrue(TEXT("a sprint under the held power is an error"), Issues.ContainsByPredicate([](const FMantleValidationIssue& Issue)
	{
		return Issue.Severity == EMantleIssueSeverity::Error && Issue.Problem.Contains(TEXT("Sprint Power")) && Issue.Problem.Contains(TEXT("under its Rider Power"));
	}));
	TestTrue(TEXT("a cadence too near the max is an error"), Issues.ContainsByPredicate([](const FMantleValidationIssue& Issue)
	{
		return Issue.Severity == EMantleIssueSeverity::Error && Issue.Problem.Contains(TEXT("within 20 of its Max Cadence"));
	}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehiclePresetRollingResistanceTest,
	"Mantle.Vehicle.Presets.RollingResistance",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehiclePresetRollingResistanceTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehiclePresetApplyTestPrivate;
	// tires.rollingResistance (#447, #448): how much every wheel loses to rolling. A preset's is read in and written out.
	FMantleVehiclePreset Preset;
	FString Error;
	if (TestTrue(TEXT("the inline preset parses"), FMantleVehiclePresets::Parse(PresetWithDefaults(TEXT("{ \"tires\": { \"friction\": 1.6, \"rollingResistance\": 0.04 } }")), Preset, Error)))
	{
		UMantleVehicleSource* Source = NewObject<UMantleVehicleSource>(GetTransientPackage());
		TArray<FString> Notes;
		TestTrue(TEXT("rollingResistance is a known key"), FMantleVehiclePresets::ApplyDefaults(Preset, *Source, Notes));
		TestEqual(TEXT("rollingResistance read in"), Source->Tuning.TireRollingResistance, 0.04f);
		const TSharedRef<FJsonObject> Document = FMantleVehicleConverter::MakeDocument(*Source, TMap<FString, FString>());
		ExpectFragment(*this, TEXT("rolling resistance"), Preset.Defaults, Document);
	}

	// The tracked presets carry a track's 0.04, and apply.
	for (const TCHAR* Id : { TEXT("tank"), TEXT("bulldozer") })
	{
		const FMantleVehiclePreset* Tracked = FMantleVehiclePresets::Find(Id);
		if (!TestNotNull(*FString::Printf(TEXT("the %s preset loads"), Id), Tracked))
		{
			continue;
		}
		UMantleVehicleSource* Source = NewObject<UMantleVehicleSource>(GetTransientPackage());
		TArray<FString> Notes;
		const bool bApplied = FMantleVehiclePresets::ApplyDefaults(*Tracked, *Source, Notes);
		TestTrue(*FString::Printf(TEXT("the %s applies (%s)"), Id, *FString::Join(Notes, TEXT(" "))), bApplied);
		TestEqual(*FString::Printf(TEXT("the %s rolls as a track does"), Id), Source->Tuning.TireRollingResistance, 0.04f);
	}

	// Absent: a car tire's 0.012, which the document leaves out (its absence means the same), so a car's document is what it was.
	UMantleVehicleSource* Fresh = NewObject<UMantleVehicleSource>(GetTransientPackage());
	TestEqual(TEXT("absent: a car tire's 0.012"), Fresh->Tuning.TireRollingResistance, 0.012f);
	const TSharedRef<FJsonObject> Plain = FMantleVehicleConverter::MakeDocument(*Fresh, TMap<FString, FString>());
	const TSharedPtr<FJsonObject>* Tires = nullptr;
	if (TestTrue(TEXT("the document has tires"), Plain->TryGetObjectField(TEXT("tires"), Tires) && Tires))
	{
		TestFalse(TEXT("a car tire's rolling resistance is left out"), (*Tires)->HasField(TEXT("rollingResistance")));
	}

	// Out of range, either way: the check says so, by name.
	for (const float Value : { 0.2f, 0.001f })
	{
		UMantleVehicleSource* Wild = NewObject<UMantleVehicleSource>(GetTransientPackage());
		Wild->Tuning.TireRollingResistance = Value;
		TArray<FMantleValidationIssue> Issues;
		FMantleVehicleConverter::CheckSource(Wild, FString(), Issues);
		TestTrue(*FString::Printf(TEXT("a rolling resistance of %g is an error"), Value), Issues.ContainsByPredicate([](const FMantleValidationIssue& Issue)
		{
			return Issue.Severity == EMantleIssueSeverity::Error && Issue.Problem.Contains(TEXT("Tire Rolling Resistance")) && Issue.Problem.Contains(TEXT("outside what a vehicle may have"));
		}));
	}
	// Positive control: in range is no such error.
	TArray<FMantleValidationIssue> Fine;
	FMantleVehicleConverter::CheckSource(Fresh, FString(), Fine);
	TestFalse(TEXT("0.012 is no rolling-resistance error"), Fine.ContainsByPredicate([](const FMantleValidationIssue& Issue) { return Issue.Problem.Contains(TEXT("Tire Rolling Resistance")); }));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
