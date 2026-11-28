// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehiclePreset.h"

#include "Dom/JsonObject.h"
#include "MantleEngineSounds.h"
#include "MantleVehicleConverter.h"
#include "MantleVehicleSource.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// engine.sound (#258): a preset's engine sound is read in (absent: None, the type's own) and an unknown one refuses the preset; the converter writes
// the creator's pick into the engine block only when the vehicle has one to pick, keeps it through the rebuilds, refuses one the vehicle can't have
// as a creator-visible error, and notes one that does nothing (an electric motor's, a plane's); a boat or a jet ski picks one too; the dropdown offers
// what the vehicle can have.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVehicleEngineSoundConverterTestPrivate
{
	FString PresetWithEngine(const FString& Engine)
	{
		return FString::Printf(TEXT("{ \"schemaVersion\": 1, \"id\": \"x\", \"name\": \"Test Car\", \"movementType\": \"wheeled\", \"fit\": { \"wheelRadius\": 30, \"trackWidth\": 160, \"minScale\": 0.5, \"maxScale\": 2 }, \"defaults\": { \"engine\": %s } }"), *Engine);
	}

	/** The document's engine.sound, or empty when it has none. */
	FString DocumentSound(const UMantleVehicleSource& Source)
	{
		const TSharedRef<FJsonObject> Document = FMantleVehicleConverter::MakeDocument(Source, TMap<FString, FString>());
		const TSharedPtr<FJsonObject>* Engine = nullptr;
		FString Sound;
		if (Document->TryGetObjectField(TEXT("engine"), Engine) && Engine)
		{
			(*Engine)->TryGetStringField(TEXT("sound"), Sound);
		}
		return Sound;
	}

	/** The first issue of Severity whose problem mentions Text, or null. */
	const FMantleValidationIssue* FindIssue(const UMantleVehicleSource* Source, const EMantleIssueSeverity Severity, const TCHAR* Text, TArray<FMantleValidationIssue>& Issues)
	{
		Issues.Reset();
		FMantleVehicleConverter::CheckSource(Source, FString(), Issues);
		return Issues.FindByPredicate([Severity, Text](const FMantleValidationIssue& Issue) { return Issue.Severity == Severity && Issue.Problem.Contains(Text); });
	}

	void SetWheels(UMantleVehicleSource& Source, const int32 Count)
	{
		Source.Wheels.Reset();
		for (int32 Index = 0; Index < Count; ++Index)
		{
			FMantleVehicleSourceWheel& Wheel = Source.Wheels.AddDefaulted_GetRef();
			Wheel.Bone = FName(*FString::Printf(TEXT("Wheel_%d"), Index));
			Wheel.bSteering = Index == 0;
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleEngineSoundConverterTest,
	"Mantle.Vehicle.Presets.EngineSound",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleEngineSoundConverterTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleEngineSoundConverterTestPrivate;
	TArray<FString> Notes;
	FString Error;

	// The shipped presets' sounds are read in; an electric motor and a bicycle have none. One source goes through every preset in turn, so a type
	// switch leaves no sound of the type before behind.
	const TPair<const TCHAR*, const TCHAR*> Shipped[] = { { TEXT("sedan"), TEXT("inline4") }, { TEXT("motorcycle"), TEXT("bike_inline4") },
		{ TEXT("atv"), TEXT("inline3") }, { TEXT("truck"), TEXT("diesel") }, { TEXT("boat"), TEXT("v8") }, { TEXT("jetski"), TEXT("inline3") },
		{ TEXT("electric"), TEXT("None") }, { TEXT("bicycle"), TEXT("None") } };
	UMantleVehicleSource* Switched = NewObject<UMantleVehicleSource>(GetTransientPackage());
	for (const TPair<const TCHAR*, const TCHAR*>& Pair : Shipped)
	{
		const FMantleVehiclePreset* Preset = FMantleVehiclePresets::Find(Pair.Key);
		if (TestNotNull(*FString::Printf(TEXT("the %s preset loads"), Pair.Key), Preset))
		{
			TestTrue(*FString::Printf(TEXT("the %s applies"), Pair.Key), FMantleVehiclePresets::ApplyDefaults(*Preset, *Switched, Notes));
			TestEqual(*FString::Printf(TEXT("%s: its engine sound"), Pair.Key), Switched->Tuning.EngineSound, FName(Pair.Value));
		}
	}

	// A preset with no engine sound gives None (the type's own), whatever the vehicle had; an unknown one refuses the preset and changes nothing.
	FMantleVehiclePreset Plain;
	FMantleVehiclePreset Unknown;
	if (TestTrue(TEXT("a preset without a sound parses"), FMantleVehiclePresets::Parse(PresetWithEngine(TEXT("{ \"maxTorque\": 300 }")), Plain, Error))
		&& TestTrue(TEXT("a preset with an unknown sound parses"), FMantleVehiclePresets::Parse(PresetWithEngine(TEXT("{ \"sound\": \"jet_fighter\" }")), Unknown, Error)))
	{
		UMantleVehicleSource* Source = NewObject<UMantleVehicleSource>(GetTransientPackage());
		Source->Tuning.EngineSound = TEXT("v8");
		TestTrue(TEXT("the preset without a sound applies"), FMantleVehiclePresets::ApplyDefaults(Plain, *Source, Notes));
		TestTrue(TEXT("and leaves no sound"), Source->Tuning.EngineSound.IsNone());
		Source->Tuning.EngineSound = TEXT("v8");
		Notes.Reset();
		TestFalse(TEXT("an unknown sound refuses the preset"), FMantleVehiclePresets::ApplyDefaults(Unknown, *Source, Notes));
		TestTrue(TEXT("saying which setting"), Notes.ContainsByPredicate([](const FString& Note) { return Note.Contains(TEXT("engine/sound")); }));
		TestEqual(TEXT("and the vehicle keeps its sound"), Source->Tuning.EngineSound, FName(TEXT("v8")));
	}

	// The converter: written when set, left out when None, kept through the rebuilds.
	UMantleVehicleSource* Car = NewObject<UMantleVehicleSource>(GetTransientPackage());
	SetWheels(*Car, 4);
	TestEqual(TEXT("a car with no sound picked writes none"), DocumentSound(*Car), FString());
	Car->Tuning.EngineSound = TEXT("v8");
	TestEqual(TEXT("a car's V8 is written"), DocumentSound(*Car), FString(TEXT("v8")));
	{
		const TSharedRef<FJsonObject> Rebuilt = FMantleVehicleConverter::WithMaterials(FMantleVehicleConverter::MakeDocument(*Car, TMap<FString, FString>()),
			TMap<FString, FString>());
		const TSharedPtr<FJsonObject>* Engine = nullptr;
		FString Sound;
		TestTrue(TEXT("the sound survives the materials rebuild"), Rebuilt->TryGetObjectField(TEXT("engine"), Engine) && Engine
			&& (*Engine)->TryGetStringField(TEXT("sound"), Sound) && Sound == TEXT("v8"));
	}
	TArray<FMantleValidationIssue> Issues;
	TestNull(TEXT("a car's V8 is no error"), FindIssue(Car, EMantleIssueSeverity::Error, TEXT("Engine Sound"), Issues));
	Car->Tuning.EngineSound = TEXT("bike_vtwin");
	TestNotNull(TEXT("a motorcycle's sound on a car is an error"), FindIssue(Car, EMantleIssueSeverity::Error, TEXT("Engine Sound 'bike_vtwin'"), Issues));
	Car->Tuning.EngineSound = TEXT("jet_fighter");
	TestNotNull(TEXT("an unknown sound is an error"), FindIssue(Car, EMantleIssueSeverity::Error, TEXT("Engine Sound 'jet_fighter'"), Issues));

	// A motorcycle takes a motorcycle's sounds only; an ATV (handlebars on four wheels) either kind.
	UMantleVehicleSource* Bike = NewObject<UMantleVehicleSource>(GetTransientPackage());
	SetWheels(*Bike, 2);
	Bike->DriverSeatKind = EMantleVehicleSeatKind::Motorcycle;
	Bike->Tuning.EngineSound = TEXT("v8");
	TestNotNull(TEXT("a car's sound on a motorcycle is an error"), FindIssue(Bike, EMantleIssueSeverity::Error, TEXT("Engine Sound 'v8'"), Issues));
	Bike->Tuning.EngineSound = TEXT("bike_vtwin");
	TestNull(TEXT("a V-twin on a motorcycle is no error"), FindIssue(Bike, EMantleIssueSeverity::Error, TEXT("Engine Sound"), Issues));
	SetWheels(*Bike, 4);
	TestNull(TEXT("a V-twin on an ATV is no error"), FindIssue(Bike, EMantleIssueSeverity::Error, TEXT("Engine Sound"), Issues));
	Bike->Tuning.EngineSound = TEXT("inline3");
	TestNull(TEXT("an inline-3 on an ATV is no error"), FindIssue(Bike, EMantleIssueSeverity::Error, TEXT("Engine Sound"), Issues));

	// The dropdown: None first, then what the vehicle can have.
	const TArray<FName> CarOptions = Car->GetEngineSoundOptions();
	TestEqual(TEXT("a car's list: None and a car's sounds"), CarOptions.Num(), 1 + FMantleEngineSounds::Car().Num());
	TestTrue(TEXT("None first"), CarOptions.Num() > 0 && CarOptions[0].IsNone());
	SetWheels(*Bike, 2);
	TestEqual(TEXT("a motorcycle's list: None and a motorcycle's sounds"), Bike->GetEngineSoundOptions().Num(), 1 + FMantleEngineSounds::Bike().Num());

	// A boat plays its drive's engine (car sounds); a jet ski's handlebars take a motorcycle's too.
	UMantleVehicleSource* Boat = NewObject<UMantleVehicleSource>(GetTransientPackage());
	SetWheels(*Boat, 4);
	Boat->MovementType = EMantleVehicleMovementKind::Boat;
	Boat->Tuning.EngineSound = TEXT("v8");
	TestEqual(TEXT("a boat's V8 is written"), DocumentSound(*Boat), FString(TEXT("v8")));
	TestNull(TEXT("and is no error"), FindIssue(Boat, EMantleIssueSeverity::Error, TEXT("Engine Sound"), Issues));
	TestNull(TEXT("nor a note"), FindIssue(Boat, EMantleIssueSeverity::Warning, TEXT("Engine Sound"), Issues));
	Boat->Tuning.EngineSound = TEXT("bike_vtwin");
	TestNotNull(TEXT("a motorcycle's sound on a boat is an error"), FindIssue(Boat, EMantleIssueSeverity::Error, TEXT("Engine Sound 'bike_vtwin'"), Issues));
	Boat->DriverSeatKind = EMantleVehicleSeatKind::Motorcycle;
	TestNull(TEXT("but not on a jet ski's handlebars"), FindIssue(Boat, EMantleIssueSeverity::Error, TEXT("Engine Sound"), Issues));

	// Electric: no sound written and a note (it plays its own motor), not an error.
	UMantleVehicleSource* Electric = NewObject<UMantleVehicleSource>(GetTransientPackage());
	SetWheels(*Electric, 4);
	Electric->Tuning.bElectric = true;
	Electric->Tuning.GearRatios = { 1.f };
	Electric->Tuning.EngineSound = TEXT("v8");
	TestEqual(TEXT("an electric car writes no sound"), DocumentSound(*Electric), FString());
	TestNotNull(TEXT("and is told its Engine Sound does nothing"), FindIssue(Electric, EMantleIssueSeverity::Warning, TEXT("Engine Sound"), Issues));
	TestNull(TEXT("as a note, not an error"), FindIssue(Electric, EMantleIssueSeverity::Error, TEXT("Engine Sound"), Issues));
	TestEqual(TEXT("an electric car's list is None alone"), Electric->GetEngineSoundOptions().Num(), 1);
	// Car sounds step 4: a plane, a helicopter or a hovercraft takes an aircraft's sound, and only an aircraft's; nothing else takes one.
	UMantleVehicleSource* Plane = NewObject<UMantleVehicleSource>(GetTransientPackage());
	SetWheels(*Plane, 3);
	Plane->MovementType = EMantleVehicleMovementKind::Plane;
	Plane->Tuning.EngineSound = TEXT("aircraft_fighter");
	TestEqual(TEXT("a plane's fighter jet is written"), DocumentSound(*Plane), FString(TEXT("aircraft_fighter")));
	TestNull(TEXT("and is no error"), FindIssue(Plane, EMantleIssueSeverity::Error, TEXT("Engine Sound"), Issues));
	TestNull(TEXT("nor a note"), FindIssue(Plane, EMantleIssueSeverity::Warning, TEXT("Engine Sound"), Issues));
	Plane->Tuning.EngineSound = TEXT("v8");
	TestNotNull(TEXT("a car's sound on a plane is an error"), FindIssue(Plane, EMantleIssueSeverity::Error, TEXT("Engine Sound 'v8'"), Issues));
	TestEqual(TEXT("a plane's list: None and an aircraft's sounds"), Plane->GetEngineSoundOptions().Num(), 1 + FMantleEngineSounds::Aircraft().Num());
	Plane->MovementType = EMantleVehicleMovementKind::Hover;
	Plane->Tuning.EngineSound = TEXT("hovercraft");
	TestNull(TEXT("a hovercraft's sound on a hovercraft is no error"), FindIssue(Plane, EMantleIssueSeverity::Error, TEXT("Engine Sound"), Issues));
	Car->Tuning.EngineSound = TEXT("helicopter");
	TestNotNull(TEXT("an aircraft's sound on a car is an error"), FindIssue(Car, EMantleIssueSeverity::Error, TEXT("Engine Sound 'helicopter'"), Issues));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
