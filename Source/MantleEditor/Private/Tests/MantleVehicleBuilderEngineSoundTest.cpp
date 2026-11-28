// Copyright Joshua Eger. All Rights Reserved.
#include "MantleEngineSounds.h"
#include "MantleVehicleBuilder.h"
#include "MantleVehiclePreset.h"
#include "MantleVehicleSource.h"
#include "SMantleVehicleBuilder.h"

#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

/** The window's Engine Sound state, set the way its row sets it, and what Build would send. */
struct FMantleVehicleBuilderEngineSoundAccess
{
	static void PickType(SMantleVehicleBuilder& Panel, const TCHAR* Id, const TCHAR* Name) { Panel.PickPreset(Id, FText::FromString(Name)); }
	static void PickSound(SMantleVehicleBuilder& Panel, const FName Id) { Panel.PickEngineSound(Id); }
	static const TArray<FName>& Ids(const SMantleVehicleBuilder& Panel) { return Panel.EngineSoundIds; }
	static int32 Shown(const SMantleVehicleBuilder& Panel) { return Panel.EngineSoundChoices.Num(); }
	static FName Current(const SMantleVehicleBuilder& Panel) { return Panel.CurrentEngineSound(); }
	static FMantleVehicleModelOptions Options(const SMantleVehicleBuilder& Panel) { return Panel.MakeBuildOptions(); }
	static void SetBuilt(SMantleVehicleBuilder& Panel, UMantleVehicleSource* Source) { Panel.BuiltSource = Source; }
	static void Forget(SMantleVehicleBuilder& Panel) { Panel.ForgetBuild(); }
};

// The Engine Sound pick in the Build a vehicle window (#258): beside the Engine pick, for every type whose vehicles can pick one, with the options and
// rule of the vehicle's Details dropdown (FMantleEngineSounds). It starts on the type's own, Build sends it (FMantleVehicleBuilder::ApplyEngineSound
// puts it in Tuning.EngineSound), and while the window holds a built vehicle the row and the vehicle's Details are one setting.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVehicleBuilderEngineSoundTestPrivate
{
	/** A built result holding a transient source of Movement on Wheels wheels, as a build leaves it (not saved: no package to save). */
	FMantleVehicleBuildResult MakeBuilt(const EMantleVehicleMovementKind Movement, const int32 Wheels, const EMantleVehicleSeatKind Seat = EMantleVehicleSeatKind::Car)
	{
		FMantleVehicleBuildResult Built;
		Built.Source = NewObject<UMantleVehicleSource>(GetTransientPackage());
		Built.Source->MovementType = Movement;
		Built.Source->DriverSeatKind = Seat;
		for (int32 Index = 0; Index < Wheels; ++Index)
		{
			Built.Source->Wheels.AddDefaulted_GetRef().Bone = FName(*FString::Printf(TEXT("Wheel_%d"), Index));
		}
		Built.bOk = true;
		Built.bSaved = false;
		return Built;
	}

	FMantleVehicleModelOptions WithSound(const FName Id)
	{
		FMantleVehicleModelOptions Options;
		Options.EngineSound = Id;
		return Options;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleBuilderEngineSoundTest,
	"Mantle.Vehicle.Builder.EngineSound",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleBuilderEngineSoundTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleBuilderEngineSoundTestPrivate;
	using FAccess = FMantleVehicleBuilderEngineSoundAccess;
	const int32 CarCount = FMantleEngineSounds::Car().Num();
	const int32 BikeCount = FMantleEngineSounds::Bike().Num();
	const int32 AircraftCount = FMantleEngineSounds::Aircraft().Num();

	// Each type's list: a car's for cars, trucks, tanks and boats; a motorcycle's for the motorcycle; both on handlebars over more wheels; an
	// aircraft's for planes, helicopters and hovercraft; none for the types with nothing to pick.
	const TPair<const TCHAR*, int32> Lists[] = { { TEXT("sedan"), CarCount }, { TEXT("truck"), CarCount }, { TEXT("tank"), CarCount }, { TEXT("boat"), CarCount },
		{ TEXT("motorcycle"), BikeCount }, { TEXT("atv"), CarCount + BikeCount }, { TEXT("jetski"), CarCount + BikeCount }, { TEXT("electric"), 0 },
		{ TEXT("bicycle"), 0 }, { TEXT("plane"), AircraftCount }, { TEXT("jet"), AircraftCount }, { TEXT("helicopter"), AircraftCount },
		{ TEXT("hovercraft"), AircraftCount }, { TEXT("semitrailer"), 0 } };
	for (const TPair<const TCHAR*, int32>& List : Lists)
	{
		const FMantleVehiclePreset* Type = FMantleVehiclePresets::Find(List.Key);
		if (!TestNotNull(*FString::Printf(TEXT("the %s preset"), List.Key), Type))
		{
			continue;
		}
		TArray<FName> Ids;
		FMantleVehicleBuilder::EngineSoundOptions(*Type, Ids);
		TestEqual(*FString::Printf(TEXT("%s: its engine sounds"), List.Key), Ids.Num(), List.Value);
		const FName Own = FMantleVehicleBuilder::PresetEngineSound(*Type);
		TestTrue(*FString::Printf(TEXT("%s: its own sound (%s) is one of them, or it has none"), List.Key, *Own.ToString()), Own.IsNone() || Ids.Contains(Own));
	}
	TestEqual(TEXT("A sedan's own sound"), FMantleVehicleBuilder::PresetEngineSound(*FMantleVehiclePresets::Find(TEXT("sedan"))), FName(TEXT("racing")));
	TestEqual(TEXT("A jet's own sound"), FMantleVehicleBuilder::PresetEngineSound(*FMantleVehiclePresets::Find(TEXT("jet"))), FName(TEXT("aircraft_fighter")));
	TestEqual(TEXT("A hovercraft's own sound"), FMantleVehicleBuilder::PresetEngineSound(*FMantleVehiclePresets::Find(TEXT("hovercraft"))), FName(TEXT("hovercraft")));

	// Names in plain words.
	TestEqual(TEXT("v8"), FMantleVehicleBuilder::EngineSoundName(TEXT("v8")), FString(TEXT("V8")));
	TestEqual(TEXT("inline4"), FMantleVehicleBuilder::EngineSoundName(TEXT("inline4")), FString(TEXT("Inline-4")));
	TestEqual(TEXT("racing"), FMantleVehicleBuilder::EngineSoundName(TEXT("racing")), FString(TEXT("Racing")));
	TestEqual(TEXT("flat6"), FMantleVehicleBuilder::EngineSoundName(TEXT("flat6")), FString(TEXT("Flat-6")));
	TestEqual(TEXT("diesel"), FMantleVehicleBuilder::EngineSoundName(TEXT("diesel")), FString(TEXT("Diesel")));
	TestEqual(TEXT("bike_vtwin"), FMantleVehicleBuilder::EngineSoundName(TEXT("bike_vtwin")), FString(TEXT("Motorcycle V-twin")));
	TestEqual(TEXT("bike_inline4"), FMantleVehicleBuilder::EngineSoundName(TEXT("bike_inline4")), FString(TEXT("Motorcycle inline-4")));
	TestEqual(TEXT("bike_highrev"), FMantleVehicleBuilder::EngineSoundName(TEXT("bike_highrev")), FString(TEXT("Motorcycle high-rev")));
	TestEqual(TEXT("none"), FMantleVehicleBuilder::EngineSoundName(NAME_None), FString(TEXT("Mantle's default")));
	TestEqual(TEXT("aircraft_fighter"), FMantleVehicleBuilder::EngineSoundName(TEXT("aircraft_fighter")), FString(TEXT("Fighter jet")));
	TestEqual(TEXT("helicopter"), FMantleVehicleBuilder::EngineSoundName(TEXT("helicopter")), FString(TEXT("Helicopter")));

	// ApplyEngineSound: the pick goes into the built vehicle's settings; one it can't have, or on a vehicle with none, is a note and nothing.
	{
		FMantleVehicleBuildResult Car = MakeBuilt(EMantleVehicleMovementKind::Wheeled, 4);
		Car.Source->Tuning.EngineSound = TEXT("inline4");
		FMantleVehicleBuilder::ApplyEngineSound(Car, FMantleVehicleModelOptions());
		TestEqual(TEXT("unset: the type's own stays"), Car.Source->Tuning.EngineSound, FName(TEXT("inline4")));
		FMantleVehicleBuilder::ApplyEngineSound(Car, WithSound(TEXT("v8")));
		TestEqual(TEXT("a car's V8 is set"), Car.Source->Tuning.EngineSound, FName(TEXT("v8")));
		FMantleVehicleBuilder::ApplyEngineSound(Car, WithSound(TEXT("bike_vtwin")));
		TestEqual(TEXT("a motorcycle's on a car is not"), Car.Source->Tuning.EngineSound, FName(TEXT("v8")));
		TestTrue(TEXT("with a note"), Car.Notes.ContainsByPredicate([](const FString& Note) { return Note.Contains(TEXT("doesn't fit")); }));
		FMantleVehicleBuildResult Atv = MakeBuilt(EMantleVehicleMovementKind::Wheeled, 4, EMantleVehicleSeatKind::Motorcycle);
		FMantleVehicleBuilder::ApplyEngineSound(Atv, WithSound(TEXT("bike_vtwin")));
		TestEqual(TEXT("an ATV takes a motorcycle's"), Atv.Source->Tuning.EngineSound, FName(TEXT("bike_vtwin")));
		FMantleVehicleBuildResult Boat = MakeBuilt(EMantleVehicleMovementKind::Boat, 4);
		FMantleVehicleBuilder::ApplyEngineSound(Boat, WithSound(TEXT("v8")));
		TestEqual(TEXT("a boat takes a V8"), Boat.Source->Tuning.EngineSound, FName(TEXT("v8")));
		FMantleVehicleBuildResult Plane = MakeBuilt(EMantleVehicleMovementKind::Plane, 3);
		FMantleVehicleBuilder::ApplyEngineSound(Plane, WithSound(TEXT("v8")));
		TestTrue(TEXT("a plane takes no car's"), Plane.Source->Tuning.EngineSound.IsNone());
		TestTrue(TEXT("with a note"), Plane.Notes.ContainsByPredicate([](const FString& Note) { return Note.Contains(TEXT("doesn't fit")); }));
		FMantleVehicleBuilder::ApplyEngineSound(Plane, WithSound(TEXT("aircraft_fighter")));
		TestEqual(TEXT("a plane takes a fighter jet's"), Plane.Source->Tuning.EngineSound, FName(TEXT("aircraft_fighter")));
		FMantleVehicleBuildResult Hover = MakeBuilt(EMantleVehicleMovementKind::Hover, 0);
		FMantleVehicleBuilder::ApplyEngineSound(Hover, WithSound(TEXT("hovercraft")));
		TestEqual(TEXT("a hovercraft takes a hovercraft's"), Hover.Source->Tuning.EngineSound, FName(TEXT("hovercraft")));
		FMantleVehicleBuilder::ApplyEngineSound(Car, WithSound(TEXT("aircraft_airliner")));
		TestEqual(TEXT("an airliner's on a car is not"), Car.Source->Tuning.EngineSound, FName(TEXT("v8")));
	}

	// The window: the row lists the type's sounds and starts on its own one; Build sends the pick; a type with none hides the row and sends nothing.
	TSharedRef<SMantleVehicleBuilder> Panel = SNew(SMantleVehicleBuilder);
	FAccess::PickType(*Panel, TEXT("sedan"), TEXT("Sedan"));
	TestEqual(TEXT("Sedan: a car's sounds"), FAccess::Ids(*Panel).Num(), CarCount);
	TestEqual(TEXT("... each with a name"), FAccess::Shown(*Panel), CarCount);
	TestEqual(TEXT("... starting on its own (Racing)"), FAccess::Current(*Panel), FName(TEXT("racing")));
	TestTrue(TEXT("... which Build sends"), FAccess::Options(*Panel).EngineSound == TOptional<FName>(FName(TEXT("racing"))));
	FAccess::PickSound(*Panel, TEXT("v8"));
	TestTrue(TEXT("Picked V8: Build sends V8"), FAccess::Options(*Panel).EngineSound == TOptional<FName>(FName(TEXT("v8"))));

	// While the window holds the vehicle it built, the row and the vehicle's Details are one setting, both ways.
	FMantleVehicleBuildResult Built = MakeBuilt(EMantleVehicleMovementKind::Wheeled, 4);
	Built.Source->Tuning.EngineSound = TEXT("v8");
	FAccess::SetBuilt(*Panel, Built.Source);
	FAccess::PickSound(*Panel, TEXT("v12"));
	TestEqual(TEXT("A pick in the window changes the built vehicle's setting"), Built.Source->Tuning.EngineSound, FName(TEXT("v12")));
	Built.Source->Tuning.EngineSound = TEXT("diesel");
	TestEqual(TEXT("A change in its Details shows in the window"), FAccess::Current(*Panel), FName(TEXT("diesel")));
	TestTrue(TEXT("... and Build sends it"), FAccess::Options(*Panel).EngineSound == TOptional<FName>(FName(TEXT("diesel"))));
	FAccess::Forget(*Panel);
	TestEqual(TEXT("Done with that vehicle, the window keeps its sound for the next build"), FAccess::Current(*Panel), FName(TEXT("diesel")));

	// Another type starts on its own; a type with none to pick sends nothing.
	FAccess::PickType(*Panel, TEXT("motorcycle"), TEXT("Motorcycle"));
	TestEqual(TEXT("Motorcycle: a motorcycle's sounds"), FAccess::Ids(*Panel).Num(), BikeCount);
	TestEqual(TEXT("... starting on its own"), FAccess::Current(*Panel), FName(TEXT("bike_inline4")));
	FAccess::PickType(*Panel, TEXT("electric"), TEXT("Electric"));
	TestEqual(TEXT("Electric: none to pick"), FAccess::Ids(*Panel).Num(), 0);
	TestFalse(TEXT("... and Build sends none"), FAccess::Options(*Panel).EngineSound.IsSet());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
