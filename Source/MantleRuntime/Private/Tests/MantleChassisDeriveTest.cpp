// Copyright Joshua Eger. All Rights Reserved.
#include "MantleChassisDerive.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MantleChassisSpec.h"
#include "MantleVehicleChassisBuilder.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

// Handling derived from a chassis's dimensions: every number is the preset's hand-tuned one times a ratio of dimensions, so a preset at its own
// size gets back exactly its own numbers, scaled and unscaled are continuous, each dimension moves only what depends on it, and every machine gets the
// same numbers.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleChassisDeriveTestPrivate
{
	struct FNumber
	{
		const TCHAR* Name;
		double Derived;
		double HandTuned;
	};

	TArray<FNumber> Numbers(const FMantleChassisHandling& D, const FMantleChassisHandling& H)
	{
		return {
			{ TEXT("mass"), D.Mass, H.Mass },
			{ TEXT("maxSpeed"), D.MaxSpeed, H.MaxSpeed },
			{ TEXT("dragCoefficient"), D.DragCoefficient, H.DragCoefficient },
			{ TEXT("frontalArea"), D.FrontalArea, H.FrontalArea },
			{ TEXT("centerOfMass.x"), D.CenterOfMass.X, H.CenterOfMass.X },
			{ TEXT("centerOfMass.y"), D.CenterOfMass.Y, H.CenterOfMass.Y },
			{ TEXT("centerOfMass.z"), D.CenterOfMass.Z, H.CenterOfMass.Z },
			{ TEXT("maxTorque"), D.MaxTorque, H.MaxTorque },
			{ TEXT("finalDrive"), D.FinalDrive, H.FinalDrive },
			{ TEXT("maxRaise"), D.MaxRaise, H.MaxRaise },
			{ TEXT("maxDrop"), D.MaxDrop, H.MaxDrop },
			{ TEXT("springRate"), D.SpringRate, H.SpringRate },
			{ TEXT("springPreload"), D.SpringPreload, H.SpringPreload },
			{ TEXT("damping"), D.SpringDamping, H.SpringDamping },
			{ TEXT("maxAngle"), D.MaxSteerAngle, H.MaxSteerAngle },
			{ TEXT("brakeTorque"), D.BrakeTorque, H.BrakeTorque },
			{ TEXT("handbrakeTorque"), D.HandbrakeTorque, H.HandbrakeTorque },
			{ TEXT("friction"), D.TireFriction, H.TireFriction },
			{ TEXT("cameraArmLength"), D.CameraArmLength, H.CameraArmLength },
			{ TEXT("cameraHeight"), D.CameraHeight, H.CameraHeight },
		};
	}

	/** The numbers a vehicle document does not hold: a plane's airframe's (chassis.aircraft), a helicopter's rotor's and a hovercraft's fan, pads and rudder;
	 *  zero for everything without them. */
	TArray<FNumber> AircraftNumbers(const FMantleChassisHandling& D, const FMantleChassisHandling& H)
	{
		return {
			{ TEXT("wingArea"), D.WingAreaM2, H.WingAreaM2 },
			{ TEXT("tailplaneArea"), D.TailplaneAreaM2, H.TailplaneAreaM2 },
			{ TEXT("finArea"), D.FinAreaM2, H.FinAreaM2 },
			{ TEXT("engineThrust"), D.EngineThrustNewtons, H.EngineThrustNewtons },
			{ TEXT("rotorThrust"), D.RotorThrustNewtons, H.RotorThrustNewtons },
			{ TEXT("hoverThrust"), D.HoverThrustNewtons, H.HoverThrustNewtons },
			{ TEXT("padSpringRate"), D.PadSpringRate, H.PadSpringRate },
			{ TEXT("hoverFinArea"), D.HoverFinAreaM2, H.HoverFinAreaM2 },
		};
	}

	bool Same(const FMantleChassisHandling& A, const FMantleChassisHandling& B)
	{
		const TArray<FNumber> Pairs = Numbers(A, B);
		for (const FNumber& Pair : Pairs)
		{
			if (Pair.Derived != Pair.HandTuned)
			{
				return false;
			}
		}
		for (const FNumber& Pair : AircraftNumbers(A, B))
		{
			if (Pair.Derived != Pair.HandTuned)
			{
				return false;
			}
		}
		return true;
	}

	/** The first number two handlings differ in, as text; empty when they are the same. */
	FString FirstDifference(const FMantleChassisHandling& A, const FMantleChassisHandling& B)
	{
		const TArray<FNumber> Pairs = Numbers(A, B);
		for (const FNumber& Pair : Pairs)
		{
			if (Pair.Derived != Pair.HandTuned)
			{
				return FString::Printf(TEXT("%s %.17g against %.17g"), Pair.Name, Pair.Derived, Pair.HandTuned);
			}
		}
		return FString();
	}

	bool ReadPreset(const FString& Id, FMantleChassisSpec& OutSpec, FString& OutError)
	{
		FString Text;
		return FMantleVehicleChassisBuilder::LoadPresetText(Id, Text, OutError) && FMantleChassisSpec::Parse(Text, OutSpec, OutError);
	}

	/** Whether Got is Want times Factor, to a few parts in a billion: a length scaled by a factor. */
	bool Scales(const double Got, const double Want, const double Factor)
	{
		return FMath::Abs(Got - Want * Factor) <= 1.0e-9 * FMath::Max(1.0, FMath::Abs(Want * Factor));
	}

	/** Whether Got is Want times Factor to within what rounding both to six significant digits can move them: a derived number. */
	bool ScalesRounded(const double Got, const double Want, const double Factor)
	{
		return FMath::Abs(Got - Want * Factor) <= 2.0e-5 * FMath::Max(1.0e-3, FMath::Abs(Want * Factor));
	}

	/** Whether Value is at most six significant digits: its six-digit text read back is itself. */
	bool IsSixDigits(const double Value)
	{
		const FString Text = FString::Printf(TEXT("%.5e"), Value);
		return FCString::Atod(*Text) == Value;
	}

	bool Number(const TSharedPtr<FJsonObject>& Root, const TCHAR* Group, const TCHAR* Key, double& Out)
	{
		const TSharedPtr<FJsonObject>* Child = nullptr;
		if (Group)
		{
			return Root->TryGetObjectField(Group, Child) && Child && (*Child)->TryGetNumberField(Key, Out);
		}
		return Root->TryGetNumberField(Key, Out);
	}

	/** The handling a composed vehicle document holds. False when it is not JSON or a number is missing. */
	bool DocumentHandling(const FString& Document, FMantleChassisHandling& Out)
	{
		TSharedPtr<FJsonObject> Json;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Document), Json) || !Json.IsValid())
		{
			return false;
		}
		FMantleChassisHandling H;
		const TArray<TSharedPtr<FJsonValue>>* Com = nullptr;
		double Parts[3] = { 0.0, 0.0, 0.0 };
		bool bOk = Number(Json, nullptr, TEXT("mass"), H.Mass) && Number(Json, nullptr, TEXT("maxSpeed"), H.MaxSpeed)
			&& Number(Json, nullptr, TEXT("dragCoefficient"), H.DragCoefficient) && Number(Json, nullptr, TEXT("frontalArea"), H.FrontalArea)
			&& Number(Json, TEXT("engine"), TEXT("maxTorque"), H.MaxTorque) && Number(Json, TEXT("transmission"), TEXT("finalDrive"), H.FinalDrive)
			&& Number(Json, TEXT("suspension"), TEXT("maxRaise"), H.MaxRaise) && Number(Json, TEXT("suspension"), TEXT("maxDrop"), H.MaxDrop)
			&& Number(Json, TEXT("suspension"), TEXT("springRate"), H.SpringRate) && Number(Json, TEXT("suspension"), TEXT("springPreload"), H.SpringPreload)
			&& Number(Json, TEXT("suspension"), TEXT("damping"), H.SpringDamping) && Number(Json, TEXT("steering"), TEXT("maxAngle"), H.MaxSteerAngle)
			&& Number(Json, TEXT("brakes"), TEXT("brakeTorque"), H.BrakeTorque) && Number(Json, TEXT("brakes"), TEXT("handbrakeTorque"), H.HandbrakeTorque)
			&& Number(Json, TEXT("tires"), TEXT("friction"), H.TireFriction) && Number(Json, TEXT("camera"), TEXT("armLength"), H.CameraArmLength)
			&& Number(Json, TEXT("camera"), TEXT("height"), H.CameraHeight)
			&& Json->TryGetArrayField(TEXT("centerOfMass"), Com) && Com && Com->Num() == 3;
		for (int32 Index = 0; bOk && Index < 3; ++Index)
		{
			bOk = (*Com)[Index].IsValid() && (*Com)[Index]->TryGetNumber(Parts[Index]);
		}
		if (bOk)
		{
			H.CenterOfMass = FVector(Parts[0], Parts[1], Parts[2]);
			// A ground car's document holds its preset's one friction as the front's and the rear's (6/7 and 8/7 of it, so their mean is the preset's).
			const TSharedPtr<FJsonObject>* Tires = nullptr;
			double RearFriction = 0.0;
			if (Json->TryGetObjectField(TEXT("tires"), Tires) && Tires && Tires->IsValid() && (*Tires)->TryGetNumberField(TEXT("rearFriction"), RearFriction))
			{
				H.TireFriction = FMath::RoundToDouble(0.5 * (H.TireFriction + RearFriction) * 1.0e9) / 1.0e9;
			}
			Out = H;
		}
		return bOk;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleChassisDeriveExactAtArchetypeTest,
	"Mantle.Vehicle.Chassis.DeriveIsExactAtTheArchetype",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleChassisDeriveExactAtArchetypeTest::RunTest(const FString& Parameters)
{
	using namespace MantleChassisDeriveTestPrivate;
	const TArray<FString> Ids = FMantleVehicleChassisBuilder::PresetIds();
	if (!TestTrue(TEXT("the plugin has vehicle presets"), Ids.Num() >= 9))
	{
		return false;
	}
	int32 TypesSeen = 0;
	for (const FString& Id : Ids)
	{
		FMantleChassisSpec Spec;
		FString Error;
		if (!TestTrue(*FString::Printf(TEXT("%s: it reads (%s)"), *Id, *Error), ReadPreset(Id, Spec, Error)))
		{
			continue;
		}
		TypesSeen |= 1 << static_cast<int32>(Spec.Type);

		// At its own dimensions every ratio is 1: the preset's own numbers, to the last bit, with nothing held back.
		const FMantleChassisHandling Derived = FMantleChassisDerive::Derive(Spec);
		for (const FNumber& Number : Numbers(Derived, Spec.Archetype))
		{
			TestTrue(*FString::Printf(TEXT("%s: derived %s %.17g is the hand-tuned %.17g exactly"), *Id, Number.Name, Number.Derived, Number.HandTuned), Number.Derived == Number.HandTuned);
		}
		for (const FNumber& Number : AircraftNumbers(Derived, Spec.Archetype))
		{
			TestTrue(*FString::Printf(TEXT("%s: derived %s %.17g is the hand-tuned %.17g exactly"), *Id, Number.Name, Number.Derived, Number.HandTuned), Number.Derived == Number.HandTuned);
		}
		TestTrue(*FString::Printf(TEXT("%s: a plane's areas and thrust are its airframe's, and only a plane has one"), *Id), (Spec.Archetype.WingAreaM2 > 0.0) == (Spec.Type == EMantleChassisType::Plane));
		TestTrue(*FString::Printf(TEXT("%s: a helicopter's rotor thrust is its rotor's, and only a helicopter has one"), *Id),
			(Spec.Archetype.RotorThrustNewtons > 0.0) == (Spec.Type == EMantleChassisType::Helicopter));
		TestTrue(*FString::Printf(TEXT("%s: a hovercraft's fan, pads and rudder are its own, and only a hovercraft has them"), *Id),
			(Spec.Archetype.HoverThrustNewtons > 0.0 && Spec.Archetype.PadSpringRate > 0.0 && Spec.Archetype.HoverFinAreaM2 > 0.0) == (Spec.Type == EMantleChassisType::Hovercraft));
		TestTrue(*FString::Printf(TEXT("%s: nothing is held back at the preset's own size (%s)"), *Id, Derived.Clamped.IsEmpty() ? TEXT("") : *Derived.Clamped[0]), Derived.Clamped.IsEmpty());

		// And they are the numbers its document has: the defaults completed with the schema's.
		FMantleChassisHandling InDocument;
		if (TestTrue(*FString::Printf(TEXT("%s: its document holds every number"), *Id), DocumentHandling(FMantleChassisSpec::ComposeDocument(Spec), InDocument)))
		{
			TestEqual(*FString::Printf(TEXT("%s: derived is what the document holds"), *Id), FirstDifference(Derived, InDocument), FString());
		}
		// A document composed from the derived handling is the same again.
		FMantleChassisSpec WithDerived = Spec;
		WithDerived.Derived = Derived;
		FMantleChassisHandling DerivedInDocument;
		if (TestTrue(*FString::Printf(TEXT("%s: the derived document holds every number"), *Id), DocumentHandling(FMantleChassisSpec::ComposeDocument(WithDerived), DerivedInDocument)))
		{
			TestEqual(*FString::Printf(TEXT("%s: the derived document is the preset's"), *Id), FirstDifference(DerivedInDocument, InDocument), FString());
		}
	}
	// One bit per type (EMantleChassisType's order): car, truck, motorcycle, plane, helicopter and hovercraft.
	TestEqual(TEXT("the presets are cars, trucks, a motorcycle, planes, a helicopter and a hovercraft"), TypesSeen, 63);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleChassisDeriveContinuousTest,
	"Mantle.Vehicle.Chassis.DeriveIsContinuousAtTheArchetype",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleChassisDeriveContinuousTest::RunTest(const FString& Parameters)
{
	using namespace MantleChassisDeriveTestPrivate;
	// A thousandth of a size is a few thousandths of a number (a brake is size to the fourth: 0.4 percent), at every preset on both sides.
	constexpr double Reach = 0.01;
	for (const FString& Id : FMantleVehicleChassisBuilder::PresetIds())
	{
		FMantleChassisSpec Spec;
		FString Error;
		if (!TestTrue(*FString::Printf(TEXT("%s: it reads (%s)"), *Id, *Error), ReadPreset(Id, Spec, Error)))
		{
			continue;
		}
		for (const double Factor : { 0.999, 1.001 })
		{
			const FMantleChassisSpec Near = FMantleChassisSpec::Scaled(Spec, Factor);
			if (!TestTrue(*FString::Printf(TEXT("%s x%g: it is derived"), *Id, Factor), Near.Derived.IsSet()))
			{
				continue;
			}
			TArray<FNumber> AllNumbers = Numbers(Near.Derived.GetValue(), Spec.Archetype);
			AllNumbers.Append(AircraftNumbers(Near.Derived.GetValue(), Spec.Archetype));
			for (const FNumber& Number : AllNumbers)
			{
				const double Want = Number.HandTuned;
				const double Jump = FMath::Abs(Number.Derived - Want);
				TestTrue(*FString::Printf(TEXT("%s x%g: %s %.6g is within %.0f%% of the preset's %.6g"), *Id, Factor, Number.Name, Number.Derived, 100.0 * Reach, Want),
					Jump <= Reach * FMath::Abs(Want));
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleChassisScaledSpecScalesGeometryTest,
	"Mantle.Vehicle.Chassis.ScaledSpecScalesGeometry",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleChassisScaledSpecScalesGeometryTest::RunTest(const FString& Parameters)
{
	using namespace MantleChassisDeriveTestPrivate;
	const double Factors[] = { 0.5, 0.7, 1.3, 1.6 };
	for (const FString& Id : FMantleVehicleChassisBuilder::PresetIds())
	{
		FMantleChassisSpec Spec;
		FString Error;
		if (!TestTrue(*FString::Printf(TEXT("%s: it reads (%s)"), *Id, *Error), ReadPreset(Id, Spec, Error)))
		{
			continue;
		}
		const FString Before = FMantleChassisSpec::ComposeDocument(Spec);
		for (const double S : Factors)
		{
			const FString Label = FString::Printf(TEXT("%s x%g"), *Id, S);
			const FMantleChassisSpec Big = FMantleChassisSpec::Scaled(Spec, S);
			// The archetype's own size, which the derived handling is a ratio against, is the preset's and does not follow the scale.
			TestTrue(*FString::Printf(TEXT("%s: scale and the archetype's size"), *Label),
				Big.Scale == S && Big.ArchetypeSize.WheelRadius == Spec.ArchetypeSize.WheelRadius && Big.ArchetypeSize.Wheelbase == Spec.ArchetypeSize.Wheelbase
				&& Big.ArchetypeSize.Track == Spec.ArchetypeSize.Track && Big.ArchetypeSize.WheelWidth == Spec.ArchetypeSize.WheelWidth && Big.ArchetypeSize.Length == Spec.ArchetypeSize.Length
				&& Big.ArchetypeSize.Width == Spec.ArchetypeSize.Width && Big.ArchetypeSize.Height == Spec.ArchetypeSize.Height
				&& Big.ArchetypeSize.Clearance == Spec.ArchetypeSize.Clearance && Big.ArchetypeSize.WheelCount() == Spec.WheelCount() && Big.ArchetypeSize.bCentreline == Spec.bCentreline);
			TestTrue(*FString::Printf(TEXT("%s: the preset's size is its own"), *Label),
				Spec.ArchetypeSize.WheelRadius == Spec.WheelRadius && Spec.ArchetypeSize.Wheelbase == Spec.Wheelbase && Spec.ArchetypeSize.Length == Spec.Length
				&& Spec.ArchetypeSize.Axles.Num() == Spec.Axles.Num());
			TestTrue(*FString::Printf(TEXT("%s: the wheelbase"), *Label), Scales(Big.Wheelbase, Spec.Wheelbase, S));
			TestTrue(*FString::Printf(TEXT("%s: the track"), *Label), Scales(Big.Track, Spec.Track, S));
			TestTrue(*FString::Printf(TEXT("%s: the wheel radius"), *Label), Scales(Big.WheelRadius, Spec.WheelRadius, S));
			TestTrue(*FString::Printf(TEXT("%s: the wheel width"), *Label), Scales(Big.WheelWidth, Spec.WheelWidth, S));
			TestTrue(*FString::Printf(TEXT("%s: the body's length"), *Label), Scales(Big.Length, Spec.Length, S));
			TestTrue(*FString::Printf(TEXT("%s: the body's width"), *Label), Scales(Big.Width, Spec.Width, S));
			TestTrue(*FString::Printf(TEXT("%s: the body's height"), *Label), Scales(Big.Height, Spec.Height, S));
			TestTrue(*FString::Printf(TEXT("%s: the ground clearance"), *Label), Scales(Big.Clearance, Spec.Clearance, S));
			TestTrue(*FString::Printf(TEXT("%s: the seat"), *Label),
				Scales(Big.Seat.X, Spec.Seat.X, S) && Scales(Big.Seat.Y, Spec.Seat.Y, S) && Scales(Big.Seat.Z, Spec.Seat.Z, S));
			TestTrue(*FString::Printf(TEXT("%s: the body box's middle along X"), *Label), Scales(Big.BodyX, Spec.BodyX, S));
			if (Spec.Aircraft.bSet)
			{
				// A plane's airframe grows with it, its angles do not.
				const FMantleChassisAircraft& Own = Spec.Aircraft;
				const FMantleChassisAircraft& Grown = Big.Aircraft;
				TestTrue(*FString::Printf(TEXT("%s: the wing, tailplane and fin"), *Label),
					Scales(Grown.Wing.Span, Own.Wing.Span, S) && Scales(Grown.Wing.Chord, Own.Wing.Chord, S) && Scales(Grown.Wing.X, Own.Wing.X, S) && Scales(Grown.Wing.Z, Own.Wing.Z, S)
					&& Scales(Grown.Tailplane.Span, Own.Tailplane.Span, S) && Scales(Grown.Tailplane.X, Own.Tailplane.X, S) && Scales(Grown.Tailplane.Z, Own.Tailplane.Z, S)
					&& Scales(Grown.Fin.Span, Own.Fin.Span, S) && Scales(Grown.Fin.Chord, Own.Fin.Chord, S) && Scales(Grown.Fin.X, Own.Fin.X, S));
				TestTrue(*FString::Printf(TEXT("%s: the engine, the propeller and the pilot's hands"), *Label),
					Scales(Grown.EngineX, Own.EngineX, S) && Scales(Grown.PropellerRadius, Own.PropellerRadius, S) && Scales(Grown.ThrottleGrip.Z, Own.ThrottleGrip.Z, S)
					&& Scales(Grown.StickGrip.Y, Own.StickGrip.Y, S));
				// (The tailplane's camber is the plane's trim, solved again for the new size: MantleChassisTrimTest holds it.)
				TestTrue(*FString::Printf(TEXT("%s: the angles, the engine count, the spin and the speed limit are not sizes"), *Label),
					Grown.Wing.Camber == Own.Wing.Camber && Grown.Wing.MaxControlAngle == Own.Wing.MaxControlAngle
					&& Grown.Tailplane.StallAngle == Own.Tailplane.StallAngle && Grown.EngineCount == Own.EngineCount && Grown.PropellerRpm == Own.PropellerRpm
					&& Grown.AirSpeedLimitCmPerSec == Own.AirSpeedLimitCmPerSec);
			}
			if (Spec.bSteeringWheel)
			{
				TestTrue(*FString::Printf(TEXT("%s: the steering wheel's hub"), *Label),
					Scales(Big.Hub.X, Spec.Hub.X, S) && Scales(Big.Hub.Y, Spec.Hub.Y, S) && Scales(Big.Hub.Z, Spec.Hub.Z, S));
				// The rim is a hand's span, held to the document's 10 to 30 cm.
				TestTrue(*FString::Printf(TEXT("%s: the steering wheel's rim %.2f"), *Label, Big.RimRadius), Big.RimRadius == FMath::Clamp(Spec.RimRadius * S, 10.0, 30.0));
			}
			if (Spec.Rider.bSet)
			{
				TestTrue(*FString::Printf(TEXT("%s: the grips, pegs and pelvis"), *Label),
					Scales(Big.Rider.LeftGrip.Z, Spec.Rider.LeftGrip.Z, S) && Scales(Big.Rider.RightGrip.Y, Spec.Rider.RightGrip.Y, S)
					&& Scales(Big.Rider.LeftPeg.X, Spec.Rider.LeftPeg.X, S) && Scales(Big.Rider.PelvisOffset.Z, Spec.Rider.PelvisOffset.Z, S)
					&& Scales(Big.Rider.StopPelvisShift.Y, Spec.Rider.StopPelvisShift.Y, S));
				TestEqual(*FString::Printf(TEXT("%s: the rider's knee splay is an angle"), *Label), Big.Rider.KneeSplayDegrees, Spec.Rider.KneeSplayDegrees);
			}
			if (TestEqual(*FString::Printf(TEXT("%s: the same axles"), *Label), Big.Axles.Num(), Spec.Axles.Num()))
			{
				for (int32 Axle = 0; Axle < Big.Axles.Num(); ++Axle)
				{
					// Positions are shares of the wheelbase: what is on the axle does not change with the size.
					TestTrue(*FString::Printf(TEXT("%s: axle %d"), *Label, Axle),
						Big.Axles[Axle].Position == Spec.Axles[Axle].Position && Big.Axles[Axle].bSteer == Spec.Axles[Axle].bSteer
						&& Big.Axles[Axle].bDrive == Spec.Axles[Axle].bDrive && Big.Axles[Axle].bHandbrake == Spec.Axles[Axle].bHandbrake);
				}
			}
			TestTrue(*FString::Printf(TEXT("%s: the type, seats and flags"), *Label),
				Big.Type == Spec.Type && Big.bPassenger == Spec.bPassenger && Big.bCentreline == Spec.bCentreline && Big.DriverSeat == Spec.DriverSeat
				&& Big.ForkBone == Spec.ForkBone && Big.WheelCount() == Spec.WheelCount());

			// The handling is the derived one for the new size, not the archetype's; the centre of mass is in it.
			if (TestTrue(*FString::Printf(TEXT("%s: it has derived handling"), *Label), Big.Derived.IsSet()))
			{
				TestTrue(*FString::Printf(TEXT("%s: the handling derives from the scaled dimensions"), *Label), Same(Big.Derived.GetValue(), FMantleChassisDerive::Derive(Big)));
				TestTrue(*FString::Printf(TEXT("%s: the centre of mass is the derived one"), *Label), Big.CenterOfMass == Big.Derived->CenterOfMass);
				TestTrue(*FString::Printf(TEXT("%s: and the hand-tuned numbers are kept as the archetype's"), *Label), Same(Big.Archetype, Spec.Archetype));
			}

			// The preset itself is untouched.
			TestTrue(*FString::Printf(TEXT("%s: the preset keeps its size and handling"), *Label), !Spec.Derived.IsSet() && Spec.Scale == 1.0);
			TestEqual(*FString::Printf(TEXT("%s: and composes the same document"), *Label), FMantleChassisSpec::ComposeDocument(Spec), Before);

			// The document is composed from the scaled spec.
			const FString Document = FMantleChassisSpec::ComposeDocument(Big);
			TSharedPtr<FJsonObject> Json;
			if (TestTrue(*FString::Printf(TEXT("%s: the document is JSON"), *Label), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Document), Json) && Json.IsValid()) && Big.Derived.IsSet())
			{
				double Mass = 0.0;
				double Length = 0.0;
				const TArray<TSharedPtr<FJsonValue>>* Boxes = nullptr;
				Json->TryGetNumberField(TEXT("mass"), Mass);
				TestEqual(*FString::Printf(TEXT("%s: the document's mass is the derived one"), *Label), Mass, Big.Derived->Mass);
				if (Json->TryGetArrayField(TEXT("collision"), Boxes) && Boxes && Boxes->Num() >= 1)
				{
					const TArray<TSharedPtr<FJsonValue>>* Extent = nullptr;
					if ((*Boxes)[0]->AsObject()->TryGetArrayField(TEXT("extent"), Extent) && Extent && Extent->Num() == 3)
					{
						(*Extent)[0]->TryGetNumber(Length);
					}
				}
				// A plane's boxes (fuselage, wing, tailplane, fin) are held inside the document's +-1000 cm, so a large one is not the scaled body's.
				// A helicopter's are its cabin, boom and fin, the cabin first: each scales, the boom's and fin's held inside the same reach when large.
				if (Spec.Helicopter.bSet)
				{
					TestTrue(*FString::Printf(TEXT("%s: a helicopter has three boxes, the first its scaled cabin"), *Label), Boxes && Boxes->Num() == 3 && Scales(2.0 * Length, Spec.Length, S));
				}
				else if (!Spec.Aircraft.bSet)
				{
					TestTrue(*FString::Printf(TEXT("%s: the collision box is the scaled body's"), *Label), Boxes && Boxes->Num() == 1 && Scales(2.0 * Length, Spec.Length, S));
				}
				else if (S <= 0.7)
				{
					TestTrue(*FString::Printf(TEXT("%s: a small plane has four boxes, the first its scaled fuselage"), *Label), Boxes && Boxes->Num() == 4 && Scales(2.0 * Length, Spec.Length, S));
				}
			}
		}

		// Scale 1, and a scale that is not a number above 0, change nothing: the preset keeps its hand-tuned numbers.
		const double Unchanged[] = { 1.0, 0.0, -2.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity() };
		for (const double S : Unchanged)
		{
			const FMantleChassisSpec Kept = FMantleChassisSpec::Scaled(Spec, S);
			TestTrue(*FString::Printf(TEXT("%s: scaled by %g it is the preset"), *Id, S), !Kept.Derived.IsSet() && Kept.Scale == 1.0 && FMantleChassisSpec::ComposeDocument(Kept) == Before);
		}

		// Scaling twice is scaling by the product, up to rounding, and 1 of them again is the product's handling.
		const FMantleChassisSpec Twice = FMantleChassisSpec::Scaled(FMantleChassisSpec::Scaled(Spec, 0.5), 1.4);
		const FMantleChassisSpec Once = FMantleChassisSpec::Scaled(Spec, 0.7);
		TestTrue(*FString::Printf(TEXT("%s: scaling by 0.5 and then 1.4 is scaling by 0.7"), *Id),
			FMath::IsNearlyEqual(Twice.Scale, 0.7, 1.0e-12) && FMath::IsNearlyEqual(Twice.Wheelbase, Once.Wheelbase, 1.0e-9) && FMath::IsNearlyEqual(Twice.Length, Once.Length, 1.0e-9));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleChassisDeriveIsDeterministicTest,
	"Mantle.Vehicle.Chassis.DeriveIsDeterministic",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleChassisDeriveIsDeterministicTest::RunTest(const FString& Parameters)
{
	using namespace MantleChassisDeriveTestPrivate;
	// Every machine rebuilds the same chassis from the same selector: the same inputs give bit-for-bit the same handling and the same document,
	// however often, in whatever order, and from however many separate reads of the preset.
	const FString Selectors[] = { TEXT("sedan@0.7"), TEXT("sedan@1.6"), TEXT("sports@0.5"), TEXT("pickup@1.3"), TEXT("truck@0.7"), TEXT("sixwheeler@1.3"), TEXT("van@0.5"),
		TEXT("limousine@1.6"), TEXT("motorcycle@0.7"), TEXT("motorcycle@1.3"), TEXT("plane@0.7"), TEXT("plane@1.3"), TEXT("plane@2.5") };
	for (const FString& Selector : Selectors)
	{
		FMantleChassisSpec A;
		FMantleChassisSpec B;
		FString Error;
		if (!TestTrue(*FString::Printf(TEXT("%s: it loads twice (%s)"), *Selector, *Error),
				FMantleVehicleChassisBuilder::LoadSpec(Selector, A, Error) && FMantleVehicleChassisBuilder::LoadSpec(Selector, B, Error)))
		{
			continue;
		}
		if (!TestTrue(*FString::Printf(TEXT("%s: it is derived"), *Selector), A.Derived.IsSet() && B.Derived.IsSet()))
		{
			continue;
		}
		TestTrue(*FString::Printf(TEXT("%s: two reads derive the same handling"), *Selector), Same(A.Derived.GetValue(), B.Derived.GetValue()));
		const FMantleChassisHandling First = FMantleChassisDerive::Derive(A);
		TestTrue(*FString::Printf(TEXT("%s: the handling is the one the spec holds"), *Selector), Same(First, A.Derived.GetValue()));
		// Deriving other sizes in between, and with a default feel, leaves it alone: the function has no state.
		FMantleChassisDerive::Derive(FMantleChassisSpec::Scaled(A, 1.5));
		FMantleChassisDerive::Derive(FMantleChassisSpec::Scaled(A, 0.5), FMantleChassisFeel{ 1.0, -1.0, 0.5, -0.5 });
		TestTrue(*FString::Printf(TEXT("%s: deriving again gives the same handling"), *Selector), Same(FMantleChassisDerive::Derive(A), First));
		TestTrue(*FString::Printf(TEXT("%s: a default feel and a feel of zeros are the same"), *Selector),
			Same(FMantleChassisDerive::Derive(A, FMantleChassisFeel()), FMantleChassisDerive::Derive(A, FMantleChassisFeel{ 0.0, 0.0, 0.0, 0.0 })));
		const FString Document = FMantleChassisSpec::ComposeDocument(A);
		TestEqual(*FString::Printf(TEXT("%s: two reads compose the same document"), *Selector), FMantleChassisSpec::ComposeDocument(B), Document);
		TestEqual(*FString::Printf(TEXT("%s: composing again gives the same document"), *Selector), FMantleChassisSpec::ComposeDocument(A), Document);

		// Six significant digits and no more, whatever order the floating point did it in: the Linux server and a Windows client write the same text.
		for (const FMantleChassisHandling& Handling : { A.Derived.GetValue(), FMantleChassisDerive::Derive(A, FMantleChassisFeel{ 0.7, -0.3, 0.9, -0.1 }) })
		{
			TArray<FNumber> AllNumbers = Numbers(Handling, Handling);
			AllNumbers.Append(AircraftNumbers(Handling, Handling));
			for (const FNumber& Number : AllNumbers)
			{
				TestTrue(*FString::Printf(TEXT("%s: %s %.17g has at most six significant digits"), *Selector, Number.Name, Number.Derived), IsSixDigits(Number.Derived));
			}
		}

		// A selector's spelling does not matter, only the number it names.
		FString Id;
		double Scale = 0.0;
		FMantleChassisSpec::ParseSelector(Selector, Id, Scale, Error);
		FMantleChassisSpec C;
		TestTrue(*FString::Printf(TEXT("%s: another spelling of the scale"), *Selector),
			FMantleVehicleChassisBuilder::LoadSpec(Id + TEXT("@") + FString::Printf(TEXT("%.3f"), Scale), C, Error) && FMantleChassisSpec::ComposeDocument(C) == Document);

		// Scaling the preset after the fact is the same as asking for it by selector.
		FMantleChassisSpec Preset;
		if (ReadPreset(Id, Preset, Error))
		{
			TestEqual(*FString::Printf(TEXT("%s: scaling the preset composes the selector's document"), *Selector), FMantleChassisSpec::ComposeDocument(FMantleChassisSpec::Scaled(Preset, Scale)), Document);
		}
	}

	// The handling holds nothing but numbers and no random, and Derive changes nothing in its spec: a body twice as long weighs twice as much, in a copy.
	FMantleChassisSpec Sedan;
	FString Error;
	if (ReadPreset(TEXT("sedan"), Sedan, Error))
	{
		const FMantleChassisHandling Own = FMantleChassisDerive::Derive(Sedan);
		FMantleChassisSpec Longer = Sedan;
		Longer.Length *= 2.0;
		TestEqual(TEXT("a body twice as long weighs twice as much"), FMantleChassisDerive::Derive(Longer).Mass, 2.0 * Own.Mass);
		TestEqual(TEXT("and the spec's archetype is not changed by it"), Sedan.Archetype.Mass, 1500.0);
		TestEqual(TEXT("nor the sedan's own mass"), FMantleChassisDerive::Derive(Sedan).Mass, Own.Mass);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleChassisDeriveScalingLawsTest,
	"Mantle.Vehicle.Chassis.DeriveScalingLaws",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleChassisDeriveScalingLawsTest::RunTest(const FString& Parameters)
{
	using namespace MantleChassisDeriveTestPrivate;
	// How each number moves when the chassis is scaled by S (every length by S, nothing held at a limit). The sedan, the truck and the motorcycle,
	// below and above their own size.
	const TCHAR* const Ids[] = { TEXT("sedan"), TEXT("truck"), TEXT("motorcycle"), TEXT("plane") };
	const double Factors[] = { 0.7, 1.3 };
	for (const TCHAR* Id : Ids)
	{
		FMantleChassisSpec Preset;
		FString Error;
		if (!TestTrue(*FString::Printf(TEXT("%s: it reads (%s)"), Id, *Error), ReadPreset(Id, Preset, Error)))
		{
			continue;
		}
		const FMantleChassisHandling Own = FMantleChassisDerive::Derive(Preset);
		const FMantleChassisDeriveConstants& K = Preset.Constants;
		for (const double S : Factors)
		{
			const FString Label = FString::Printf(TEXT("%s x%g"), Id, S);
			const FMantleChassisSpec Big = FMantleChassisSpec::Scaled(Preset, S);
			if (!TestTrue(*FString::Printf(TEXT("%s: it is derived"), *Label), Big.Derived.IsSet()))
			{
				continue;
			}
			const FMantleChassisHandling& H = Big.Derived.GetValue();
			TestTrue(*FString::Printf(TEXT("%s: the mass grows with the volume, S^3"), *Label), ScalesRounded(H.Mass, Own.Mass, S * S * S));
			TestTrue(*FString::Printf(TEXT("%s: the frontal area grows with the face, S^2"), *Label), ScalesRounded(H.FrontalArea, Own.FrontalArea, S * S));
			TestTrue(*FString::Printf(TEXT("%s: the travel grows with the wheel, S"), *Label), ScalesRounded(H.MaxRaise, Own.MaxRaise, S) && ScalesRounded(H.MaxDrop, Own.MaxDrop, S));
			// k = (m / n) (2 pi f)^2 with f falling as 1 / sqrt(radius): S^3 x S^-1, so the sag stays the same share of the travel.
			TestTrue(*FString::Printf(TEXT("%s: the springs follow mass x frequency^2, S^2"), *Label),
				ScalesRounded(H.SpringRate, Own.SpringRate, S * S) && ScalesRounded(H.SpringPreload, Own.SpringPreload, S * S));
			TestTrue(*FString::Printf(TEXT("%s: the engine's torque follows the mass at the same power-to-weight, S^3"), *Label), ScalesRounded(H.MaxTorque, Own.MaxTorque, S * S * S));
			TestTrue(*FString::Printf(TEXT("%s: the brakes give the same deceleration, torque S^4"), *Label), ScalesRounded(H.BrakeTorque, Own.BrakeTorque, S * S * S * S));
			// The lock is the archetype's own angle, so a uniform scale leaves it alone (and the turning circle, which follows the wheelbase, grows with the size).
			TestEqual(*FString::Printf(TEXT("%s: the steering lock is an angle, the same"), *Label), H.MaxSteerAngle, Own.MaxSteerAngle);
			TestTrue(*FString::Printf(TEXT("%s: the archetype's tyre, drag and damping are kept"), *Label),
				H.TireFriction == Own.TireFriction && H.DragCoefficient == Own.DragCoefficient && H.SpringDamping == Own.SpringDamping);
			// A smaller wheel moves less road a step: the top speed falls with it, and the final drive keeps the top gear at the same share of the
			// redline. A larger one keeps the archetype's top speed and gears down for it.
			TestTrue(*FString::Printf(TEXT("%s: the top speed is the archetype's, less in proportion to a smaller wheel"), *Label), ScalesRounded(H.MaxSpeed, Own.MaxSpeed, FMath::Min(1.0, S)));
			TestTrue(*FString::Printf(TEXT("%s: the final drive follows the wheel over the top speed"), *Label), ScalesRounded(H.FinalDrive, Own.FinalDrive, S / FMath::Min(1.0, S)));
			// The camera is the archetype's, longer and higher by the type's share of the body's growth: the driver, who does not scale, is the rest.
			TestTrue(*FString::Printf(TEXT("%s: the camera arm"), *Label), ScalesRounded(H.CameraArmLength, Own.CameraArmLength + K.CameraArmPerLength * Preset.Length * (S - 1.0), 1.0));
			TestTrue(*FString::Printf(TEXT("%s: the camera height"), *Label),
				ScalesRounded(H.CameraHeight, Own.CameraHeight + K.CameraHeightPerHeight * (Preset.Clearance + Preset.Height) * (S - 1.0), 1.0));
			if (Preset.Aircraft.bSet)
			{
				// Areas follow planforms, a size squared, and the thrust follows the mass: the plane keeps its thrust-to-weight, and its weight over wing area grows with S.
				TestTrue(*FString::Printf(TEXT("%s: the wing, tailplane and fin areas grow with their planforms, S^2"), *Label),
					ScalesRounded(H.WingAreaM2, Own.WingAreaM2, S * S) && ScalesRounded(H.TailplaneAreaM2, Own.TailplaneAreaM2, S * S) && ScalesRounded(H.FinAreaM2, Own.FinAreaM2, S * S));
				TestTrue(*FString::Printf(TEXT("%s: the engines' thrust grows with the mass, S^3"), *Label), ScalesRounded(H.EngineThrustNewtons, Own.EngineThrustNewtons, S * S * S));
			}
			if (Preset.Type != EMantleChassisType::Motorcycle)
			{
				// A bike's centre of mass sits within 3 cm of its wheel centres: at the smaller size the floor above them holds it.
				TestTrue(*FString::Printf(TEXT("%s: the centre of mass keeps its place in the body, S"), *Label),
					ScalesRounded(H.CenterOfMass.Z, Own.CenterOfMass.Z, S) && ScalesRounded(H.CenterOfMass.X, Own.CenterOfMass.X, S));
			}
		}
	}

	// The feel sliders move the number they name by up to 30 percent, and no other.
	FMantleChassisSpec Sedan;
	FString Error;
	if (ReadPreset(TEXT("sedan"), Sedan, Error))
	{
		const FMantleChassisHandling Own = FMantleChassisDerive::Derive(Sedan);
		const double Reach = FMantleChassisFeel::Reach;
		const FMantleChassisHandling Grippy = FMantleChassisDerive::Derive(Sedan, FMantleChassisFeel{ 1.0, 0.0, 0.0, 0.0 });
		TestTrue(TEXT("full grip is 30% more friction"), ScalesRounded(Grippy.TireFriction, Own.TireFriction, 1.0 + Reach));
		TestTrue(TEXT("and nothing else moves"), Grippy.Mass == Own.Mass && Grippy.MaxTorque == Own.MaxTorque && Grippy.SpringRate == Own.SpringRate && Grippy.MaxSpeed == Own.MaxSpeed);
		const FMantleChassisHandling Strong = FMantleChassisDerive::Derive(Sedan, FMantleChassisFeel{ 0.0, 1.0, 0.0, 0.0 });
		TestTrue(TEXT("full power is 30% more torque"), ScalesRounded(Strong.MaxTorque, Own.MaxTorque, 1.0 + Reach) && Strong.TireFriction == Own.TireFriction);
		const FMantleChassisHandling Stiff = FMantleChassisDerive::Derive(Sedan, FMantleChassisFeel{ 0.0, 0.0, 1.0, 0.0 });
		TestTrue(TEXT("full stiffness is 30% more ride frequency, 69% more spring"), ScalesRounded(Stiff.SpringRate, Own.SpringRate, (1.0 + Reach) * (1.0 + Reach)) && Stiff.MaxTorque == Own.MaxTorque);
		const FMantleChassisHandling Quick = FMantleChassisDerive::Derive(Sedan, FMantleChassisFeel{ 0.0, 0.0, 0.0, -1.0 });
		TestTrue(TEXT("least top speed is 30% less speed, and the final drive follows"),
			ScalesRounded(Quick.MaxSpeed, Own.MaxSpeed, 1.0 - Reach) && ScalesRounded(Quick.FinalDrive, Own.FinalDrive, 1.0 / (1.0 - Reach)));
		const FMantleChassisHandling Beyond = FMantleChassisDerive::Derive(Sedan, FMantleChassisFeel{ 9.0, 0.0, 0.0, 0.0 });
		TestTrue(TEXT("a slider past its end is held at it"), Beyond.TireFriction == Grippy.TireFriction);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleChassisDeriveFollowsEachDimensionTest,
	"Mantle.Vehicle.Chassis.DeriveFollowsEachDimension",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleChassisDeriveFollowsEachDimensionTest::RunTest(const FString& Parameters)
{
	using namespace MantleChassisDeriveTestPrivate;
	// A creator edits one dimension at a time (the builder's sliders), not only a uniform scale: each moves the numbers that depend on it and no others.
	FMantleChassisSpec Sedan;
	FString Error;
	if (!TestTrue(*FString::Printf(TEXT("the sedan reads (%s)"), *Error), ReadPreset(TEXT("sedan"), Sedan, Error)))
	{
		return false;
	}
	const FMantleChassisHandling Own = FMantleChassisDerive::Derive(Sedan);
	const FMantleChassisDeriveConstants& K = Sedan.Constants;
	// The turning circle of a plain two-axle chassis, cm: the wheelbase over tan(lock). The lock is an angle, so the circle follows the wheelbase.
	const auto TurningCircle = [](const FMantleChassisSpec& Chassis, const FMantleChassisHandling& Handling)
	{
		constexpr double Pi = 3.14159265358979323846;
		return Chassis.Wheelbase / FMath::Tan(Handling.MaxSteerAngle * Pi / 180.0);
	};
	const double OwnCircle = TurningCircle(Sedan, Own);

	// A longer wheelbase alone: the same body, so the same mass, lock, springs, torque, brakes and camera, and a wider turning circle, as on a real car.
	{
		FMantleChassisSpec Spec = Sedan;
		Spec.Wheelbase *= 1.3;
		const FMantleChassisHandling H = FMantleChassisDerive::Derive(Spec);
		TestEqual(TEXT("a longer wheelbase alone: the mass"), H.Mass, Own.Mass);
		TestTrue(TEXT("a longer wheelbase alone: the springs, torque and brakes"), H.SpringRate == Own.SpringRate && H.MaxTorque == Own.MaxTorque && H.BrakeTorque == Own.BrakeTorque);
		TestTrue(TEXT("a longer wheelbase alone: the camera follows the body, not the wheels"), H.CameraArmLength == Own.CameraArmLength && H.CameraHeight == Own.CameraHeight);
		TestEqual(TEXT("a longer wheelbase alone: the lock is the archetype's"), H.MaxSteerAngle, Own.MaxSteerAngle);
		TestTrue(TEXT("a longer wheelbase alone: the turning circle is 30% wider"), TurningCircle(Spec, H) > OwnCircle && ScalesRounded(TurningCircle(Spec, H), OwnCircle, 1.3));
	}
	// ... and the centre of mass sits at the same share of it.
	{
		FMantleChassisSpec Six;
		if (TestTrue(*FString::Printf(TEXT("the six-wheeler reads (%s)"), *Error), ReadPreset(TEXT("sixwheeler"), Six, Error)))
		{
			const FMantleChassisHandling SixOwn = FMantleChassisDerive::Derive(Six);
			Six.Wheelbase *= 1.3;
			TestTrue(TEXT("a longer wheelbase alone: the centre of mass moves with it"), ScalesRounded(FMantleChassisDerive::Derive(Six).CenterOfMass.X, SixOwn.CenterOfMass.X, 1.3));
		}
	}

	// A longer body alone: more mass, and with it springs, torque and brakes; the same wheels, so the same top speed, gearing, lock and track; the camera
	// pulls back.
	{
		FMantleChassisSpec Spec = Sedan;
		Spec.Length *= 1.2;
		const FMantleChassisHandling H = FMantleChassisDerive::Derive(Spec);
		TestTrue(TEXT("a longer body alone: the mass, springs, torque and brakes follow the volume"),
			ScalesRounded(H.Mass, Own.Mass, 1.2) && ScalesRounded(H.SpringRate, Own.SpringRate, 1.2) && ScalesRounded(H.MaxTorque, Own.MaxTorque, 1.2)
			&& ScalesRounded(H.BrakeTorque, Own.BrakeTorque, 1.2));
		TestTrue(TEXT("a longer body alone: the face, speed and gearing are the same"), H.FrontalArea == Own.FrontalArea && H.MaxSpeed == Own.MaxSpeed && H.FinalDrive == Own.FinalDrive);
		TestTrue(TEXT("a longer body alone: the camera pulls back by the type's share of it"),
			ScalesRounded(H.CameraArmLength, Own.CameraArmLength + K.CameraArmPerLength * (Spec.Length - Sedan.Length), 1.0) && H.CameraHeight == Own.CameraHeight);
		TestEqual(TEXT("a longer body alone: the lock is the same"), H.MaxSteerAngle, Own.MaxSteerAngle);
	}

	// A taller body alone: more mass and a bigger face, the camera rises, the centre of mass rises with the body.
	{
		FMantleChassisSpec Spec = Sedan;
		Spec.Height *= 1.5;
		const FMantleChassisHandling H = FMantleChassisDerive::Derive(Spec);
		const double Volume = (Spec.Height + Spec.Clearance) / (Sedan.Height + Sedan.Clearance);
		TestTrue(TEXT("a taller body alone: the mass and the face follow (height + clearance)"), ScalesRounded(H.Mass, Own.Mass, Volume) && ScalesRounded(H.FrontalArea, Own.FrontalArea, Volume));
		TestTrue(TEXT("a taller body alone: the camera rises by the type's share of it"),
			ScalesRounded(H.CameraHeight, Own.CameraHeight + K.CameraHeightPerHeight * (Spec.Height - Sedan.Height), 1.0) && H.CameraArmLength == Own.CameraArmLength);
		TestTrue(TEXT("a taller body alone: the centre of mass is higher"), H.CenterOfMass.Z > Own.CenterOfMass.Z);
	}

	// A bigger wheel alone: the same mass, speed and lock; softer springs (a lower frequency), longer travel, more brake torque, the final drive geared for the
	// bigger wheel. A smaller one is slower, and its gearing is the same.
	{
		FMantleChassisSpec Big = Sedan;
		Big.WheelRadius *= 1.25;
		const FMantleChassisHandling H = FMantleChassisDerive::Derive(Big);
		TestTrue(TEXT("a bigger wheel alone: the mass, speed and lock are the same"), H.Mass == Own.Mass && H.MaxSpeed == Own.MaxSpeed && H.MaxSteerAngle == Own.MaxSteerAngle);
		TestTrue(TEXT("a bigger wheel alone: the springs soften, the travel and brakes grow, the drive gears down"),
			ScalesRounded(H.SpringRate, Own.SpringRate, 1.0 / 1.25) && ScalesRounded(H.MaxRaise, Own.MaxRaise, 1.25) && ScalesRounded(H.BrakeTorque, Own.BrakeTorque, 1.25)
			&& ScalesRounded(H.FinalDrive, Own.FinalDrive, 1.25));
		FMantleChassisSpec Small = Sedan;
		Small.WheelRadius *= 0.8;
		const FMantleChassisHandling S = FMantleChassisDerive::Derive(Small);
		TestTrue(TEXT("a smaller wheel alone: the speed falls with it and the gearing is the same"), ScalesRounded(S.MaxSpeed, Own.MaxSpeed, 0.8) && ScalesRounded(S.FinalDrive, Own.FinalDrive, 1.0));
		TestTrue(TEXT("a smaller wheel alone: stiffer springs"), ScalesRounded(S.SpringRate, Own.SpringRate, 1.0 / 0.8));
	}

	// A narrower track alone: the same numbers except the centre of mass, which is lowered to keep it upright, and the result says so.
	{
		FMantleChassisSpec Spec = Sedan;
		Spec.Track *= 0.5;
		const FMantleChassisHandling H = FMantleChassisDerive::Derive(Spec);
		TestTrue(TEXT("a narrower track alone: the mass, springs and lock are the same"), H.Mass == Own.Mass && H.SpringRate == Own.SpringRate && H.MaxSteerAngle == Own.MaxSteerAngle);
		TestTrue(TEXT("a narrower track alone: the centre of mass is lowered to the stability target"), H.CenterOfMass.Z < Own.CenterOfMass.Z && ScalesRounded(H.CenterOfMass.Z, Spec.Track / (2.0 * K.Stability), 1.0));
		TestTrue(TEXT("and the result says it was lowered"), H.Clamped.ContainsByPredicate([](const FString& Entry) { return Entry.StartsWith(TEXT("centerOfMass/z ")); }));
		FMantleChassisSpec Wider = Sedan;
		Wider.Track *= 1.5;
		TestTrue(TEXT("a wider track alone changes nothing"), Same(FMantleChassisDerive::Derive(Wider), Own));
	}

	// A plane's airframe, one dimension at a time: a wider wing has more lift area and the same weight; a deeper tailplane only a bigger tail; a longer
	// fuselage is heavier and has the thrust for it; two engines share the thrust one had.
	{
		FMantleChassisSpec Plane;
		if (TestTrue(*FString::Printf(TEXT("the plane reads (%s)"), *Error), ReadPreset(TEXT("plane"), Plane, Error)))
		{
			const FMantleChassisHandling PlaneOwn = FMantleChassisDerive::Derive(Plane);
			FMantleChassisSpec Wider = Plane;
			Wider.Aircraft.Wing.Span *= 2.0;
			const FMantleChassisHandling Lifting = FMantleChassisDerive::Derive(Wider);
			TestTrue(TEXT("a wider wing alone: twice the wing's area, and the same mass, tail, fin and thrust"),
				ScalesRounded(Lifting.WingAreaM2, PlaneOwn.WingAreaM2, 2.0) && Lifting.Mass == PlaneOwn.Mass && Lifting.TailplaneAreaM2 == PlaneOwn.TailplaneAreaM2
				&& Lifting.FinAreaM2 == PlaneOwn.FinAreaM2 && Lifting.EngineThrustNewtons == PlaneOwn.EngineThrustNewtons);
			FMantleChassisSpec Deeper = Plane;
			Deeper.Aircraft.Tailplane.Chord *= 1.5;
			const FMantleChassisHandling Tailed = FMantleChassisDerive::Derive(Deeper);
			TestTrue(TEXT("a deeper tailplane alone: 1.5 times the tailplane's area and nothing else"),
				ScalesRounded(Tailed.TailplaneAreaM2, PlaneOwn.TailplaneAreaM2, 1.5) && Tailed.WingAreaM2 == PlaneOwn.WingAreaM2 && Tailed.Mass == PlaneOwn.Mass);
			FMantleChassisSpec Longer = Plane;
			Longer.Length *= 1.2;
			const FMantleChassisHandling Heavy = FMantleChassisDerive::Derive(Longer);
			TestTrue(TEXT("a longer fuselage alone: more mass and the thrust for it, the same wing"),
				ScalesRounded(Heavy.Mass, PlaneOwn.Mass, 1.2) && ScalesRounded(Heavy.EngineThrustNewtons, PlaneOwn.EngineThrustNewtons, 1.2) && Heavy.WingAreaM2 == PlaneOwn.WingAreaM2);
			FMantleChassisSpec Twin = Plane;
			Twin.Aircraft.EngineCount = 2;
			const FMantleChassisHandling Pair = FMantleChassisDerive::Derive(Twin);
			TestTrue(TEXT("two engines alone: each has half the thrust, and the mass is the same"), ScalesRounded(Pair.EngineThrustNewtons, PlaneOwn.EngineThrustNewtons, 0.5) && Pair.Mass == PlaneOwn.Mass);
		}
	}

	// A third axle: the same mass, the load shared by six wheels so each spring and brake is two thirds. The lock is the archetype's own.
	{
		FMantleChassisSpec Spec = Sedan;
		FMantleChassisAxle Middle;
		Middle.bDrive = true;
		Middle.Position = 0.5;
		Spec.Axles.Insert(Middle, 1);
		const FMantleChassisHandling H = FMantleChassisDerive::Derive(Spec);
		TestTrue(TEXT("three axles: six wheels"), Spec.WheelCount() == 6 && Sedan.WheelCount() == 4);
		TestTrue(TEXT("three axles: the mass is the same, the springs and brakes are shared by more wheels"),
			H.Mass == Own.Mass && ScalesRounded(H.SpringRate, Own.SpringRate, 4.0 / 6.0) && ScalesRounded(H.BrakeTorque, Own.BrakeTorque, 4.0 / 6.0));
		TestEqual(TEXT("three axles: the lock is the archetype's"), H.MaxSteerAngle, Own.MaxSteerAngle);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleChassisDeriveHoldsAreVisibleTest,
	"Mantle.Vehicle.Chassis.DeriveHoldsAreVisible",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleChassisDeriveHoldsAreVisibleTest::RunTest(const FString& Parameters)
{
	using namespace MantleChassisDeriveTestPrivate;
	// Every path Derive holds a result to is one the document has a range for: a path the document did not know would hold nothing, silently.
	const TArray<FString> Paths = FMantleChassisDerive::LimitedPaths();
	TestEqual(TEXT("Derive holds fifteen numbers"), Paths.Num(), 15);
	for (const FString& Path : Paths)
	{
		double Min = 0.0;
		double Max = 0.0;
		TestTrue(*FString::Printf(TEXT("%s is in the document's ranges"), *Path), FMantleVehicleLimits::GetRange(Path, Min, Max));
	}

	// And Derive says when it used one: no entry of Clamped ever says a path has no range, at any preset and size.
	const auto Mentions = [](const FMantleChassisHandling& H, const TCHAR* Text)
	{
		return H.Clamped.ContainsByPredicate([Text](const FString& Entry) { return Entry.Contains(Text); });
	};
	for (const FString& Id : FMantleVehicleChassisBuilder::PresetIds())
	{
		FMantleChassisSpec Spec;
		FString Error;
		if (!TestTrue(*FString::Printf(TEXT("%s: it reads (%s)"), *Id, *Error), ReadPreset(Id, Spec, Error)))
		{
			continue;
		}
		for (const double S : { 0.25, 0.5, 1.6, 2.5 })
		{
			const FMantleChassisSpec Big = FMantleChassisSpec::Scaled(Spec, S);
			if (TestTrue(*FString::Printf(TEXT("%s x%g: it is derived"), *Id, S), Big.Derived.IsSet()))
			{
				TestFalse(*FString::Printf(TEXT("%s x%g: every number it holds has a document range"), *Id, S), Mentions(Big.Derived.GetValue(), TEXT("has no document range")));
			}
		}
	}

	FMantleChassisSpec Sedan;
	FString Error;
	if (ReadPreset(TEXT("sedan"), Sedan, Error))
	{
		// A hold is listed, with the raw number: the 1.6 sedan's handbrake is twice a brake the cap is not far above.
		const FMantleChassisSpec Big = FMantleChassisSpec::Scaled(Sedan, 1.6);
		if (TestTrue(TEXT("the 1.6 sedan is derived"), Big.Derived.IsSet()))
		{
			TestEqual(TEXT("the 1.6 sedan's handbrake is held at the document's 20000"), Big.Derived->HandbrakeTorque, 20000.0);
			TestTrue(TEXT("and the result says so, with the raw number"), Mentions(Big.Derived.GetValue(), TEXT("brakes/handbrakeTorque 39")));
		}
		// At its own size nothing is held.
		TestTrue(TEXT("the sedan at its own size is held by nothing"), FMantleChassisDerive::Derive(Sedan).Clamped.IsEmpty());

		// A chassis with no body at all gives numbers, held, and not a NaN.
		FMantleChassisSpec Flat = Sedan;
		Flat.Length = 0.0;
		Flat.Width = 0.0;
		Flat.Height = 0.0;
		Flat.WheelRadius = 0.0;
		const FMantleChassisHandling H = FMantleChassisDerive::Derive(Flat);
		bool bFinite = true;
		for (const FNumber& Number : Numbers(H, H))
		{
			bFinite = bFinite && FMath::IsFinite(Number.Derived);
		}
		TestTrue(TEXT("a chassis with no size derives finite numbers"), bFinite);
		TestEqual(TEXT("and its mass is held at the document's least"), H.Mass, 50.0);
		TestTrue(TEXT("and the result says so"), Mentions(H, TEXT("mass ")));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
