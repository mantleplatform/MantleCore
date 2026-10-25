// Copyright Joshua Eger. All Rights Reserved.
#include "MantleChassisDerive.h"

#include "MantleChassisSpec.h"
#include "MantleVehicleChassisBuilder.h"
#include "MantleVehicleChassisDocument.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// A resized plane or jet flies level hands-off: the tailplane camber that centres the elevator at the preset's cruise speed, and the cruise throttle,
// are solved again for the new size (FMantleChassisDerive::LevelFlight and SolveTrim, matching schema/validate.py's trim). The positive control comes
// first: at the preset's own size the C++ solve reproduces the arithmetic validate.py holds every preset to. Then the numbers a document has to hold to
// its ranges are listed in plain words (FMantleVehicleChassisDocument::HeldNumbers), so a clamp is never silent.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleChassisTrimTestPrivate
{
	bool Load(FAutomationTestBase& Test, const FString& Selector, FMantleChassisSpec& OutSpec)
	{
		FString Error;
		const bool bOk = FMantleVehicleChassisBuilder::LoadSpec(Selector, OutSpec, Error);
		Test.TestTrue(*FString::Printf(TEXT("%s reads (%s)"), *Selector, *Error), bOk);
		return bOk;
	}

	/** The handling a spec's document is made from: its derived numbers when it has them, the preset's own otherwise. */
	const FMantleChassisHandling& HandlingOf(const FMantleChassisSpec& Spec)
	{
		return Spec.Derived.IsSet() ? Spec.Derived.GetValue() : Spec.Archetype;
	}

	/** Level flight at the spec's own cruise speed, with its camber as it is. */
	bool LevelAtCruise(const FMantleChassisSpec& Spec, FMantleChassisTrim& OutTrim)
	{
		return FMantleChassisDerive::LevelFlight(Spec, HandlingOf(Spec), Spec.Aircraft.CruiseSpeedCmPerSec / 100.0, OutTrim);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleChassisTrimMatchesValidatePyTest,
	"Mantle.Vehicle.Chassis.TrimMatchesValidatePy",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleChassisTrimMatchesValidatePyTest::RunTest(const FString& Parameters)
{
	using namespace MantleChassisTrimTestPrivate;

	// What schema/validate.py's _plane_level_flight gives for each preset at its cruise speed: pitch, elevator, throttle.
	struct FExpected
	{
		const TCHAR* Id;
		double Pitch;
		double Elevator;
		double Throttle;
		double PresetCamber;
		double PresetThrottle;
	};
	const FExpected Expected[] = {
		{ TEXT("plane"), 2.9400753447442574, -0.024613605390435264, 0.37701565892948463, -4.0, 0.37 },
		{ TEXT("jet"), 0.3945735855856263, -0.0007101547969350615, 0.255270240936646, -0.9, 0.255 },
	};
	for (const FExpected& Want : Expected)
	{
		FMantleChassisSpec Spec;
		if (!Load(*this, Want.Id, Spec))
		{
			continue;
		}
		const FString Label = Want.Id;
		TestEqual(*FString::Printf(TEXT("%s: a preset's trim is its own (Scaled at 1 returns it)"), *Label), Spec.Aircraft.Tailplane.Camber, Want.PresetCamber);
		FMantleChassisTrim Trim;
		if (!TestTrue(*FString::Printf(TEXT("%s holds level flight at its cruise speed"), *Label), LevelAtCruise(Spec, Trim)))
		{
			continue;
		}
		// The same arithmetic in doubles: validate.py's numbers to far better than its own tolerances.
		TestTrue(*FString::Printf(TEXT("%s: the pitch matches validate.py (%.5f, validate.py %.5f)"), *Label, Trim.PitchDegrees, Want.Pitch), FMath::Abs(Trim.PitchDegrees - Want.Pitch) < 1.0e-5);
		TestTrue(*FString::Printf(TEXT("%s: the elevator matches validate.py (%.6f, validate.py %.6f)"), *Label, Trim.Elevator, Want.Elevator), FMath::Abs(Trim.Elevator - Want.Elevator) < 1.0e-5);
		TestTrue(*FString::Printf(TEXT("%s: the throttle matches validate.py (%.6f, validate.py %.6f)"), *Label, Trim.Throttle, Want.Throttle), FMath::Abs(Trim.Throttle - Want.Throttle) < 1.0e-5);
		// Inside validate.py's tolerances: the elevator within 0.05 of centre (PLANE_TRIM_TOLERANCE), the throttle within 0.02 of the preset's (PLANE_CRUISE_THROTTLE_TOLERANCE).
		TestTrue(*FString::Printf(TEXT("%s: the preset's own camber leaves the elevator within 0.05 of centre (%+.4f)"), *Label, Trim.Elevator), FMath::Abs(Trim.Elevator) <= 0.05);
		TestTrue(*FString::Printf(TEXT("%s: the preset's own cruise throttle is within 0.02 of level flight's (%.4f against %.4f)"), *Label, Want.PresetThrottle, Trim.Throttle),
			FMath::Abs(Trim.Throttle - Want.PresetThrottle) <= 0.02);

		// The solve: the camber that would centre the elevator, and the throttle, for the same preset.
		double Camber = 0.0;
		double Throttle = 0.0;
		if (TestTrue(*FString::Printf(TEXT("%s: the trim solves"), *Label), FMantleChassisDerive::SolveTrim(Spec, HandlingOf(Spec), Camber, Throttle)))
		{
			TestTrue(*FString::Printf(TEXT("%s: the solved camber (%.3f) is within the elevator's 0.05 of the preset's %g"), *Label, Camber, Want.PresetCamber),
				FMath::Abs(Camber - Want.PresetCamber) <= 0.05 * Spec.Aircraft.Tailplane.MaxControlAngle);
			TestTrue(*FString::Printf(TEXT("%s: the solved throttle (%.4f) is within 0.02 of the preset's %g"), *Label, Throttle, Want.PresetThrottle), FMath::Abs(Throttle - Want.PresetThrottle) <= 0.02);
			// Camber and control angle only act as a sum, so the camber the solve gives centres the elevator exactly.
			FMantleChassisSpec Trimmed = Spec;
			Trimmed.Aircraft.Tailplane.Camber = Camber;
			FMantleChassisTrim After;
			if (TestTrue(*FString::Printf(TEXT("%s: level flight holds with the solved camber"), *Label), LevelAtCruise(Trimmed, After)))
			{
				TestTrue(*FString::Printf(TEXT("%s: and the elevator is centred (%+.6f)"), *Label, After.Elevator), FMath::Abs(After.Elevator) < 1.0e-3);
				TestTrue(*FString::Printf(TEXT("%s: at the same throttle"), *Label), FMath::Abs(After.Throttle - Throttle) < 1.0e-3);
			}
		}
		AddInfo(FString::Printf(TEXT("%s: validate.py trim pitch %.4f elevator %+.5f throttle %.5f; C++ pitch %.4f elevator %+.5f throttle %.5f"), *Label, Want.Pitch, Want.Elevator, Want.Throttle,
			Trim.PitchDegrees, Trim.Elevator, Trim.Throttle));
	}

	// Not a plane: nothing to solve.
	FMantleChassisSpec Car;
	if (Load(*this, TEXT("sedan"), Car))
	{
		FMantleChassisTrim Trim;
		double Camber = 1.0;
		double Throttle = 1.0;
		TestFalse(TEXT("a car has no level flight"), FMantleChassisDerive::LevelFlight(Car, Car.Archetype, 70.0, Trim));
		TestFalse(TEXT("and no trim to solve"), FMantleChassisDerive::SolveTrim(Car, Car.Archetype, Camber, Throttle));
		TestTrue(TEXT("both left alone"), Camber == 1.0 && Throttle == 1.0);
		FMantleChassisSpec Bigger = FMantleChassisSpec::Scaled(Car, 1.2);
		TestFalse(TEXT("Trim does nothing to a car"), FMantleChassisSpec::Trim(Bigger));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleChassisTrimFollowsSizeTest,
	"Mantle.Vehicle.Chassis.TrimFollowsSize",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleChassisTrimFollowsSizeTest::RunTest(const FString& Parameters)
{
	using namespace MantleChassisTrimTestPrivate;

	// The fit extremes the builder offers (plane 0.70 to 1.15, jet 0.80 to 1.20) and a size between them.
	struct FSize
	{
		const TCHAR* Id;
		double Factor;
	};
	const FSize Sizes[] = {
		{ TEXT("plane"), 0.7 }, { TEXT("plane"), 0.9 }, { TEXT("plane"), 1.05 }, { TEXT("plane"), 1.15 },
		{ TEXT("jet"), 0.8 }, { TEXT("jet"), 0.95 }, { TEXT("jet"), 1.05 }, { TEXT("jet"), 1.2 },
	};
	for (const FSize& Size : Sizes)
	{
		FMantleChassisSpec Preset;
		if (!Load(*this, Size.Id, Preset))
		{
			continue;
		}
		const FMantleChassisSpec Scaled = FMantleChassisSpec::Scaled(Preset, Size.Factor);
		const FString Label = FMantleChassisSpec::MakeSelector(Size.Id, Size.Factor);
		TestEqual(*FString::Printf(TEXT("%s: it keeps the preset's cruise speed"), *Label), Scaled.Aircraft.CruiseSpeedCmPerSec, Preset.Aircraft.CruiseSpeedCmPerSec);

		// The positive control: the preset's own trim, on the resized airframe, is what leaves it off balance. Without the solve the elevator is held off centre,
		// by a good deal more than validate.py allows a preset (0.05).
		FMantleChassisSpec Untrimmed = Scaled;
		Untrimmed.Aircraft.Tailplane.Camber = Preset.Aircraft.Tailplane.Camber;
		Untrimmed.Aircraft.CruiseThrottle = Preset.Aircraft.CruiseThrottle;
		FMantleChassisTrim Off;
		if (!TestTrue(*FString::Printf(TEXT("%s: the preset's trim on the resized airframe still holds level flight"), *Label), LevelAtCruise(Untrimmed, Off)))
		{
			continue;
		}
		AddInfo(FString::Printf(TEXT("%s: with the preset's own camber %g the elevator for level flight is %+.3f and the throttle %.4f (the preset's cruise throttle is %g)"), *Label,
			Preset.Aircraft.Tailplane.Camber, Off.Elevator, Off.Throttle, Preset.Aircraft.CruiseThrottle));

		FMantleChassisTrim On;
		if (!TestTrue(*FString::Printf(TEXT("%s: the re-trimmed airframe holds level flight"), *Label), LevelAtCruise(Scaled, On)))
		{
			continue;
		}
		AddInfo(FString::Printf(TEXT("%s: re-trimmed camber %.3f (was %g), cruise throttle %.4f (was %g); elevator %+.5f, throttle %.4f at the cruise speed"), *Label, Scaled.Aircraft.Tailplane.Camber,
			Preset.Aircraft.Tailplane.Camber, Scaled.Aircraft.CruiseThrottle, Preset.Aircraft.CruiseThrottle, On.Elevator, On.Throttle));
		TestTrue(*FString::Printf(TEXT("%s: with the solved camber the elevator is centred (%+.5f)"), *Label, On.Elevator), FMath::Abs(On.Elevator) <= 0.005);
		TestTrue(*FString::Printf(TEXT("%s: and the cruise throttle is the throttle level flight needs (%.4f against %.4f)"), *Label, Scaled.Aircraft.CruiseThrottle, On.Throttle),
			FMath::Abs(Scaled.Aircraft.CruiseThrottle - On.Throttle) <= 0.002);

		// The direction: the camber moved by the elevator the preset's trim left off centre, so it removes it. (The throttle has no one direction for every airframe: a
		// bigger plane has more weight for its wings (S^3 for S^2) and so pays more drag per newton of thrust, but its thrust grows with the weight too, and which of
		// the two wins depends on the airframe: the plane needs more throttle at a larger size and the jet less. The positive control is that it moves at all.)
		const double Moved = Scaled.Aircraft.Tailplane.Camber - Preset.Aircraft.Tailplane.Camber;
		TestTrue(*FString::Printf(TEXT("%s: the camber moves toward the elevator the preset's trim left held (%+.3f degrees, the elevator was %+.3f)"), *Label, Moved, Off.Elevator),
			FMath::Abs(Off.Elevator) < 1.0e-3 || (Moved * Off.Elevator > 0.0));
		TestTrue(*FString::Printf(TEXT("%s: the cruise throttle moves from the preset's %g to the throttle level flight needs (%.4f)"), *Label, Preset.Aircraft.CruiseThrottle, Scaled.Aircraft.CruiseThrottle),
			FMath::Abs(Scaled.Aircraft.CruiseThrottle - Preset.Aircraft.CruiseThrottle) > 1.0e-3 && FMath::Abs(Scaled.Aircraft.CruiseThrottle - Off.Throttle) < 1.0e-3);

		// What the document carries: the camber on the elevator and the throttle in flight.cruiseThrottle (a flat number, the preset never named).
		FMantleVehicleDesc Desc;
		FMantleVehicleChassisDocument::Derive(Scaled, Desc);
		const FMantleVehicleAerofoilDesc* Elevator = Desc.Flight.Aerofoils.FindByPredicate([](const FMantleVehicleAerofoilDesc& A) { return A.Type == EMantleVehicleAerofoilKind::Elevator; });
		TestTrue(*FString::Printf(TEXT("%s: the document's elevator carries the solved camber"), *Label), Elevator && FMath::IsNearlyEqual(Elevator->Camber, Scaled.Aircraft.Tailplane.Camber, 1.0e-9));
		TestTrue(*FString::Printf(TEXT("%s: and its flight block the solved cruise throttle"), *Label), FMath::IsNearlyEqual(Desc.Flight.CruiseThrottle, Scaled.Aircraft.CruiseThrottle, 1.0e-9));

		// The solve is deterministic and rounded: asking twice gives the same numbers, to the digits the document writes.
		const FMantleChassisSpec Again = FMantleChassisSpec::Scaled(Preset, Size.Factor);
		TestTrue(*FString::Printf(TEXT("%s: the same size gives the same trim"), *Label),
			Again.Aircraft.Tailplane.Camber == Scaled.Aircraft.Tailplane.Camber && Again.Aircraft.CruiseThrottle == Scaled.Aircraft.CruiseThrottle);
	}

	// A size of 1 is the preset itself: its hand-tuned trim, untouched.
	FMantleChassisSpec Plane;
	if (Load(*this, TEXT("plane"), Plane))
	{
		const FMantleChassisSpec Same = FMantleChassisSpec::Scaled(Plane, 1.0);
		TestTrue(TEXT("Scaled at 1 keeps the preset's camber and cruise throttle"), Same.Aircraft.Tailplane.Camber == -4.0 && Same.Aircraft.CruiseThrottle == 0.37);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleChassisHeldNumbersTest,
	"Mantle.Vehicle.Chassis.HeldNumbersAreListed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleChassisHeldNumbersTest::RunTest(const FString& Parameters)
{
	using namespace MantleChassisTrimTestPrivate;

	// Inside the fit ranges nothing is held: every preset at its own size and at 0.7 says everything it asks for.
	for (const FString& Id : FMantleVehicleChassisBuilder::PresetIds())
	{
		for (const double Factor : { 1.0, 0.7 })
		{
			FMantleChassisSpec Spec;
			FString Error;
			if (!FMantleVehicleChassisBuilder::LoadSpec(FMantleChassisSpec::MakeSelector(Id, Factor), Spec, Error))
			{
				AddError(FString::Printf(TEXT("%s does not read: %s"), *Id, *Error));
				continue;
			}
			const TArray<FString> Held = FMantleVehicleChassisDocument::HeldNumbers(Spec);
			TestEqual(*FString::Printf(TEXT("%s: nothing is held (%s)"), *FMantleChassisSpec::MakeSelector(Id, Factor), Held.IsEmpty() ? TEXT("") : *Held[0]), Held.Num(), 0);
		}
	}

	// A plane 1.3 times the preset's size has its tail 11 m back: past the +-1000 cm a document can say.
	FMantleChassisSpec Plane;
	if (Load(*this, TEXT("plane@1.3"), Plane))
	{
		const TArray<FString> Held = FMantleVehicleChassisDocument::HeldNumbers(Plane);
		if (TestTrue(TEXT("an oversized plane has numbers held"), Held.Num() > 0))
		{
			const bool bPlain = Held.ContainsByPredicate([](const FString& Line) { return Line.Contains(TEXT("tail")) && Line.Contains(TEXT("held to")) && !Line.Contains(TEXT("flight/")); });
			TestTrue(*FString::Printf(TEXT("and they read in plain words, not document paths (%s)"), *Held[0]), bPlain);
			// One line for each thing held, however many axes or surfaces: no sentence twice.
			TSet<FString> Seen(Held);
			TestEqual(TEXT("no sentence twice"), Seen.Num(), Held.Num());
			for (const FString& Line : Held)
			{
				AddInfo(Line);
			}
		}
	}
	return true;
}

#endif
