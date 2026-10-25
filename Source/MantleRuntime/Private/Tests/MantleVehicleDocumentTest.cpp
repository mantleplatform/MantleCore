// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleChassisDocument.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MantleChassisSpec.h"
#include "MantleVehicleAsset.h"
#include "MantleVehicleChassisBuilder.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#if WITH_DEV_AUTOMATION_TESTS

// The movement part of a vehicle document (#821, WP2): a chassis spec written as flat numbers (movementType, the flight, helicopter or hover block, the
// seats' kinds, the rider, the camera boom), read back by the client's own reader, equal to what the chassis demo path computed from the spec and to the
// examples the schema ships, and refused when malformed. Pure data; no UObjects.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleVehicleDocumentTestPrivate
{
	TSharedPtr<FJsonObject> ParseObject(const FString& Text)
	{
		TSharedPtr<FJsonObject> Object;
		return FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object) ? Object : nullptr;
	}

	FString Serialize(const TSharedRef<FJsonObject>& Object)
	{
		FString Text;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Object, Writer);
		return Text;
	}

	bool ParseDesc(const FString& Text, FMantleVehicleDesc& Out, FString& OutError)
	{
		const FTCHARToUTF8 Utf8(*Text);
		return FMantleVehicleAssetReader::ParseDocument(TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()), Out, OutError);
	}

	bool LoadSpec(FAutomationTestBase& Test, const FString& Selector, FMantleChassisSpec& OutSpec)
	{
		FString Error;
		const bool bOk = FMantleVehicleChassisBuilder::LoadSpec(Selector, OutSpec, Error);
		Test.TestTrue(*FString::Printf(TEXT("%s: the chassis loads (%s)"), *Selector, *Error), bOk);
		return bOk;
	}

	/** The selectors the golden covers: every shipped preset as it is, and resized to 0.7 with its handling derived. */
	TArray<FString> Selectors()
	{
		TArray<FString> Out;
		for (const FString& Id : FMantleVehicleChassisBuilder::PresetIds())
		{
			Out.Add(Id);
			Out.Add(Id + TEXT("@0.7"));
		}
		return Out;
	}

	/** Document minus the keys WP2 adds, so what it carried before is what remains. */
	void StripMovement(FJsonObject& Doc)
	{
		for (const TCHAR* Key : { TEXT("movementType"), TEXT("flight"), TEXT("helicopter"), TEXT("hover") })
		{
			Doc.RemoveField(Key);
		}
		const TPair<const TCHAR*, const TCHAR*> Extras[] = {
			{ TEXT("seat"), TEXT("kind") }, { TEXT("seat"), TEXT("exitSideDistance") }, { TEXT("seat"), TEXT("rider") },
			{ TEXT("passengerSeat"), TEXT("kind") }, { TEXT("camera"), TEXT("boomLocation") } };
		for (const TPair<const TCHAR*, const TCHAR*>& Extra : Extras)
		{
			const TSharedPtr<FJsonObject>* Parent = nullptr;
			if (Doc.TryGetObjectField(Extra.Key, Parent) && Parent && Parent->IsValid())
			{
				(*Parent)->RemoveField(Extra.Value);
			}
		}
	}

	/**
	 * A document made of Base with Example's movement keys: the examples are authoring documents (the cook measures the collision and the rest), so they do not read
	 * on their own, and what is compared is their movement part laid over a complete document.
	 */
	TSharedRef<FJsonObject> WithExampleMovement(const TSharedPtr<FJsonObject>& Base, const TSharedPtr<FJsonObject>& Example)
	{
		const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		FJsonObject::Duplicate(Base, Out);
		StripMovement(*Out);
		for (const TCHAR* Key : { TEXT("movementType"), TEXT("flight"), TEXT("helicopter"), TEXT("hover") })
		{
			if (Example->HasField(Key))
			{
				Out->SetField(Key, Example->TryGetField(Key));
			}
		}
		const TPair<const TCHAR*, const TCHAR*> Extras[] = {
			{ TEXT("seat"), TEXT("kind") }, { TEXT("seat"), TEXT("exitSideDistance") }, { TEXT("seat"), TEXT("rider") },
			{ TEXT("passengerSeat"), TEXT("kind") }, { TEXT("camera"), TEXT("boomLocation") } };
		for (const TPair<const TCHAR*, const TCHAR*>& Extra : Extras)
		{
			const TSharedPtr<FJsonObject>* From = nullptr;
			const TSharedPtr<FJsonObject>* To = nullptr;
			if (Example->TryGetObjectField(Extra.Key, From) && From && From->IsValid() && (*From)->HasField(Extra.Value)
				&& Out->TryGetObjectField(Extra.Key, To) && To && To->IsValid())
			{
				(*To)->SetField(Extra.Value, (*From)->TryGetField(Extra.Value));
			}
		}
		return Out;
	}

	/** Names the grips a parse left to the cook as the root bone's, as the cook (and Read) do, so a parsed example compares with a composed document. */
	void NameRootGrips(FMantleVehicleDesc& D)
	{
		const FName Root = FMantleVehicleChassisBuilder::RootBoneName();
		TArray<FMantleVehicleGripDesc*> Grips = { &D.Flight.PilotGrips.Left, &D.Flight.PilotGrips.Right, &D.Helicopter.PilotGrips.Left,
			&D.Helicopter.PilotGrips.Right, &D.Rider.FootPegs.Left, &D.Rider.FootPegs.Right };
		for (FMantleVehicleGripDesc* Grip : Grips)
		{
			if (Grip->Bone.IsNone())
			{
				Grip->Bone = Root;
			}
		}
	}

	/** The movement part of D as the document spells it: what Write makes of it, as text, so two Descs equal when their texts are. */
	FString MovementText(const FMantleVehicleDesc& D)
	{
		const TSharedRef<FJsonObject> Doc = MakeShared<FJsonObject>();
		// The seat and camera objects exist in a real document; Write puts its keys on them.
		Doc->SetObjectField(TEXT("seat"), MakeShared<FJsonObject>());
		Doc->SetObjectField(TEXT("passengerSeat"), MakeShared<FJsonObject>());
		Doc->SetObjectField(TEXT("camera"), MakeShared<FJsonObject>());
		FMantleVehicleChassisDocument::Write(D, *Doc);
		return Serialize(Doc);
	}

	/** Where two JSON values differ (a JSON pointer), numbers within Tolerance of one another (relative above 1); empty when they do not. */
	FString JsonDifference(const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B, const FString& Path, const double Tolerance)
	{
		if (!A.IsValid() || !B.IsValid() || A->Type != B->Type)
		{
			return Path.IsEmpty() ? FString(TEXT("/")) : Path;
		}
		if (A->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject> ObjectA = A->AsObject();
			const TSharedPtr<FJsonObject> ObjectB = B->AsObject();
			for (const auto& Pair : ObjectA->Values)
			{
				const FString Key(Pair.Key.ToView());
				const FString Difference = JsonDifference(Pair.Value, ObjectB->TryGetField(Key), Path + TEXT("/") + Key, Tolerance);
				if (!Difference.IsEmpty())
				{
					return Difference;
				}
			}
			for (const auto& Pair : ObjectB->Values)
			{
				const FString Key(Pair.Key.ToView());
				if (!ObjectA->HasField(Key))
				{
					return Path + TEXT("/") + Key;
				}
			}
			return FString();
		}
		if (A->Type == EJson::Array)
		{
			const TArray<TSharedPtr<FJsonValue>>& ArrayA = A->AsArray();
			const TArray<TSharedPtr<FJsonValue>>& ArrayB = B->AsArray();
			for (int32 Index = 0; Index < FMath::Max(ArrayA.Num(), ArrayB.Num()); ++Index)
			{
				const FString Difference = JsonDifference(ArrayA.IsValidIndex(Index) ? ArrayA[Index] : nullptr, ArrayB.IsValidIndex(Index) ? ArrayB[Index] : nullptr,
					Path + FString::Printf(TEXT("/%d"), Index), Tolerance);
				if (!Difference.IsEmpty())
				{
					return Difference;
				}
			}
			return FString();
		}
		if (A->Type == EJson::Number)
		{
			return FMath::Abs(A->AsNumber() - B->AsNumber()) <= Tolerance * FMath::Max(1.0, FMath::Abs(B->AsNumber())) ? FString() : Path;
		}
		return FJsonValue::CompareEqual(*A, *B) ? FString() : Path;
	}

	FString FirstDifferenceOf(const FString& A, const FString& B)
	{
		int32 At = 0;
		while (At < A.Len() && At < B.Len() && A[At] == B[At])
		{
			++At;
		}
		return FString::Printf(TEXT("at %d: ...%s... against ...%s..."), At, *A.Mid(FMath::Max(0, At - 40), 100), *B.Mid(FMath::Max(0, At - 40), 100));
	}

	/** The dev path's number: a definition holds it as a float. */
	float F(const double Value)
	{
		return static_cast<float>(Value);
	}

	/**
	 * What the chassis demo path (FMantleChassisDefinitions: ApplySeatKind, ApplyAircraft, ApplyHelicopter, ApplyHovercraft) sets from a spec, transcribed here as
	 * expected values and compared field by field with the Desc a composed document reads as. Every comparison is of the float the definition holds.
	 */
	void CheckAgainstDevPath(FAutomationTestBase& Test, const FString& Label, const FMantleChassisSpec& Spec, const FMantleVehicleDesc& D)
	{
		const FMantleChassisHandling& H = Spec.Derived.IsSet() ? Spec.Derived.GetValue() : Spec.Archetype;
		const FName Root = FMantleVehicleChassisBuilder::RootBoneName();
		auto Eq = [&](const FString& What, const double Got, const double Want) { Test.TestEqual(*(Label + TEXT(": ") + What), F(Got), F(Want)); };
		auto EqV = [&](const FString& What, const FVector& Got, const FVector& Want) { Test.TestEqual(*(Label + TEXT(": ") + What), Got, Want); };
		auto EqName = [&](const FString& What, const FName Got, const FName Want) { Test.TestEqual(*(Label + TEXT(": ") + What), Got.ToString(), Want.ToString()); };
		auto Expect = [&](const FString& What, const bool bGot, const bool bWant) { Test.TestEqual(*(Label + TEXT(": ") + What), bGot, bWant); };

		// The seat's kind: the preset's driver seat (a passenger seat is a car's).
		const EMantleVehicleSeatKind WantKind = Spec.DriverSeat.Equals(TEXT("motorcycle"), ESearchCase::CaseSensitive) ? EMantleVehicleSeatKind::Motorcycle
			: (Spec.DriverSeat.Equals(TEXT("pilot"), ESearchCase::CaseSensitive) ? EMantleVehicleSeatKind::Pilot : EMantleVehicleSeatKind::Car);
		Test.TestTrue(*(Label + TEXT(": the seat kind")), D.SeatKind == WantKind);
		Test.TestTrue(*(Label + TEXT(": the passenger seat is a car's")), D.PassengerSeatKind == EMantleVehicleSeatKind::Car);

		// ApplySeatKind's rider branch.
		Expect(TEXT("a rider fit exactly when the preset has one"), D.bRider, Spec.Rider.bSet);
		if (Spec.Rider.bSet && D.bRider)
		{
			const FMantleVehicleChassisRider& R = Spec.Rider;
			EqName(TEXT("the fork bone"), D.Rider.ForkBone, Spec.ForkBone);
			EqName(TEXT("the left handlebar grip is on the fork"), D.Rider.Handlebar.Left.Bone, Spec.ForkBone);
			EqName(TEXT("the right handlebar grip is on the fork"), D.Rider.Handlebar.Right.Bone, Spec.ForkBone);
			EqV(TEXT("left grip"), D.Rider.Handlebar.Left.Location, R.LeftGrip);
			EqV(TEXT("right grip"), D.Rider.Handlebar.Right.Location, R.RightGrip);
			EqName(TEXT("the left peg is on the root"), D.Rider.FootPegs.Left.Bone, Root);
			EqName(TEXT("the right peg is on the root"), D.Rider.FootPegs.Right.Bone, Root);
			EqV(TEXT("left peg"), D.Rider.FootPegs.Left.Location, R.LeftPeg);
			EqV(TEXT("right peg"), D.Rider.FootPegs.Right.Location, R.RightPeg);
			EqV(TEXT("pelvis offset"), D.Rider.PelvisOffset, R.PelvisOffset);
			EqV(TEXT("stop pelvis shift"), D.Rider.StopPelvisShift, R.StopPelvisShift);
			Eq(TEXT("knee splay"), D.Rider.KneeSplayDegrees, R.KneeSplayDegrees);
			// A rear tyre friction and brake torque of 0 keep the document's.
			Expect(TEXT("a rear tyre friction exactly when above 0"), D.Rider.bRearTireFriction, R.RearTireFriction > 0.0);
			Expect(TEXT("a rear brake torque exactly when above 0"), D.Rider.bRearBrakeTorque, R.RearBrakeTorque > 0.0);
			if (R.RearTireFriction > 0.0)
			{
				Eq(TEXT("rear tyre friction"), D.Rider.RearTireFriction, R.RearTireFriction);
			}
			if (R.RearBrakeTorque > 0.0)
			{
				Eq(TEXT("rear brake torque"), D.Rider.RearBrakeTorque, R.RearBrakeTorque);
			}
			const FMantleVehicleChassisRotationControl& C = R.RotationControl;
			const FMantleVehicleRotationControlDesc& O = D.Rider.RotationControl;
			Expect(TEXT("torque control"), O.bTorqueControl, C.bTorqueControl);
			Expect(TEXT("target rotation"), O.bTargetRotation, C.bTargetRotation);
			Expect(TEXT("roll vs speed"), O.bRollVsSpeed, C.bRollVsSpeed);
			Eq(TEXT("torque rotation damping"), O.TorqueRotationDamping, C.TorqueRotationDamping);
			Eq(TEXT("roll control scaling"), O.RollControlScaling, C.RollControlScaling);
			Eq(TEXT("roll max angle"), O.RollMaxAngle, C.RollMaxAngle);
			Eq(TEXT("pitch control scaling"), O.PitchControlScaling, C.PitchControlScaling);
			Eq(TEXT("pitch max angle"), O.PitchMaxAngle, C.PitchMaxAngle);
			Eq(TEXT("rotation stiffness"), O.RotationStiffness, C.RotationStiffness);
			Eq(TEXT("rotation damping"), O.RotationDamping, C.RotationDamping);
			Eq(TEXT("max accel"), O.MaxAccel, C.MaxAccel);
			Eq(TEXT("auto centre roll"), O.AutoCentreRollStrength, C.AutoCentreRollStrength);
			Eq(TEXT("auto centre pitch"), O.AutoCentrePitchStrength, C.AutoCentrePitchStrength);
			Eq(TEXT("auto centre yaw"), O.AutoCentreYawStrength, C.AutoCentreYawStrength);
			// A bike is narrow: the rider steps off the side a body-width out, not a car's door.
			Expect(TEXT("a bike sets its exit distance"), D.bSeatExitSideDistance, true);
			Eq(TEXT("the bike's exit distance"), D.SeatExitSideDistance, 100.0);
		}

		const bool bFlier = Spec.Aircraft.bSet || Spec.Helicopter.bSet;
		Expect(TEXT("a boom exactly when it flies"), D.bCameraBoom, bFlier);
		if (bFlier)
		{
			EqV(TEXT("the chase camera orbits the cockpit's height over the centre of mass"), D.CameraBoomLocation,
				FVector(Spec.CenterOfMass.X, 0.0, Spec.Seat.Z + 7.0 * Spec.Scale));
		}
		if (!Spec.Rider.bSet)
		{
			Expect(TEXT("an exit distance exactly when it flies (a rider's is the bike's above)"), D.bSeatExitSideDistance, bFlier);
		}

		Test.TestTrue(*(Label + TEXT(": the movement type")),
			D.MovementType == (Spec.Aircraft.bSet ? EMantleVehicleMovementKind::Plane
				: (Spec.Helicopter.bSet ? EMantleVehicleMovementKind::Helicopter : (Spec.Hovercraft.bSet ? EMantleVehicleMovementKind::Hover : EMantleVehicleMovementKind::Wheeled))));

		if (Spec.Aircraft.bSet)
		{
			const FMantleChassisAircraft& Plane = Spec.Aircraft;
			const FMantleVehicleFlightDesc& Fl = D.Flight;
			Eq(TEXT("air speed limit"), Fl.AirSpeedLimit, Plane.AirSpeedLimitCmPerSec);
			Eq(TEXT("cruise throttle"), Fl.CruiseThrottle, Plane.CruiseThrottle);
			Eq(TEXT("the player's pitch authority is half the elevator's travel"), Fl.PitchAuthority, 0.5);
			Eq(TEXT("the throttle lever rate is the definition's"), Fl.ThrottleLeverRate, 0.5);
			Eq(TEXT("no rigid-body linear damping under the aerofoils"), Fl.LinearDamping, 0.0);
			Eq(TEXT("no rigid-body angular damping"), Fl.AngularDamping, 0.0);
			if (Test.TestEqual(*(Label + TEXT(": four aerofoils")), Fl.Aerofoils.Num(), 4))
			{
				for (int32 Side = 0; Side < 2; ++Side)
				{
					const FMantleVehicleAerofoilDesc& Wing = Fl.Aerofoils[Side];
					EqName(TEXT("aileron bone"), Wing.Bone, FMantleVehicleChassisBuilder::AileronBoneName(Side == 1));
					EqV(TEXT("wing location"), Wing.Location, Spec.WingAerofoilLocation(Side == 1));
					Test.TestTrue(*(Label + TEXT(": a wing")), Wing.Type == EMantleVehicleAerofoilKind::Wing);
					Expect(TEXT("the right half is mirrored"), Wing.bMirror, Side == 1);
					Eq(TEXT("a half-wing is half the wing's area"), Wing.AreaM2, 0.5 * H.WingAreaM2);
					Eq(TEXT("wing camber"), Wing.Camber, Plane.Wing.Camber);
					Eq(TEXT("wing control angle"), Wing.MaxControlAngle, Plane.Wing.MaxControlAngle);
					Eq(TEXT("wing stall angle"), Wing.StallAngle, Plane.Wing.StallAngle);
				}
				const FMantleVehicleAerofoilDesc& Elevator = Fl.Aerofoils[2];
				EqName(TEXT("elevator bone"), Elevator.Bone, FMantleVehicleChassisBuilder::ElevatorBoneName());
				EqV(TEXT("elevator location"), Elevator.Location, Spec.ElevatorLocation());
				Test.TestTrue(*(Label + TEXT(": an elevator")), Elevator.Type == EMantleVehicleAerofoilKind::Elevator);
				Expect(TEXT("the elevator is not mirrored"), Elevator.bMirror, false);
				Eq(TEXT("tailplane area"), Elevator.AreaM2, H.TailplaneAreaM2);
				Eq(TEXT("tailplane camber (the plane's trim)"), Elevator.Camber, Plane.Tailplane.Camber);
				Eq(TEXT("tailplane control angle"), Elevator.MaxControlAngle, Plane.Tailplane.MaxControlAngle);
				Eq(TEXT("tailplane stall angle"), Elevator.StallAngle, Plane.Tailplane.StallAngle);
				const FMantleVehicleAerofoilDesc& Rudder = Fl.Aerofoils[3];
				EqName(TEXT("rudder bone"), Rudder.Bone, FMantleVehicleChassisBuilder::RudderBoneName());
				EqV(TEXT("rudder location"), Rudder.Location, Spec.RudderLocation());
				Test.TestTrue(*(Label + TEXT(": a rudder")), Rudder.Type == EMantleVehicleAerofoilKind::Rudder);
				Eq(TEXT("fin area"), Rudder.AreaM2, H.FinAreaM2);
				Eq(TEXT("fin camber"), Rudder.Camber, Plane.Fin.Camber);
				Eq(TEXT("fin control angle"), Rudder.MaxControlAngle, Plane.Fin.MaxControlAngle);
				Eq(TEXT("fin stall angle"), Rudder.StallAngle, Plane.Fin.StallAngle);
			}
			if (Test.TestEqual(*(Label + TEXT(": a thruster per engine")), Fl.Thrusters.Num(), Plane.EngineCount))
			{
				for (int32 Engine = 0; Engine < Plane.EngineCount; ++Engine)
				{
					const FName EngineBone = FMantleVehicleChassisBuilder::EngineBoneName(Plane, Engine);
					EqName(TEXT("thruster bone"), Fl.Thrusters[Engine].Bone, EngineBone);
					EqV(TEXT("thruster location"), Fl.Thrusters[Engine].Location, Spec.EngineLocation(Engine));
					EqV(TEXT("thruster pushes forward"), Fl.Thrusters[Engine].Direction, FVector::ForwardVector);
					Eq(TEXT("thruster thrust"), Fl.Thrusters[Engine].MaxThrustNewtons, H.EngineThrustNewtons);
				}
			}
			// Propellers spin about X (the second of two the other way); a jet has none.
			if (Test.TestEqual(*(Label + TEXT(": a spin bone per propeller")), Fl.SpinBones.Num(), Plane.bPropeller ? Plane.EngineCount : 0))
			{
				for (int32 Engine = 0; Engine < Fl.SpinBones.Num(); ++Engine)
				{
					EqName(TEXT("spin bone"), Fl.SpinBones[Engine].Bone, FMantleVehicleChassisBuilder::EngineBoneName(Plane, Engine));
					EqV(TEXT("spin axis"), Fl.SpinBones[Engine].Axis, FVector::ForwardVector);
					Eq(TEXT("spin rpm"), Fl.SpinBones[Engine].Rpm, Engine == 1 ? -Plane.PropellerRpm : Plane.PropellerRpm);
				}
			}
			Expect(TEXT("the pilot's hands"), Fl.bPilotGrips, true);
			EqName(TEXT("the throttle grip is on the root"), Fl.PilotGrips.Left.Bone, Root);
			EqName(TEXT("the stick grip is on the root"), Fl.PilotGrips.Right.Bone, Root);
			EqV(TEXT("throttle grip"), Fl.PilotGrips.Left.Location, Plane.ThrottleGrip);
			EqV(TEXT("stick grip"), Fl.PilotGrips.Right.Location, Plane.StickGrip);
			Eq(TEXT("a pilot steps out beside the fuselage"), D.SeatExitSideDistance, 0.5 * Spec.Width + 145.0 * Spec.Scale);
		}

		if (Spec.Helicopter.bSet)
		{
			const FMantleChassisHelicopter& Heli = Spec.Helicopter;
			const FMantleVehicleHelicopterDesc& He = D.Helicopter;
			Eq(TEXT("air speed limit"), He.AirSpeedLimit, Heli.AirSpeedLimitCmPerSec);
			Eq(TEXT("the collective that holds the weight"), He.HoverCollective, Heli.HoverCollective);
			Eq(TEXT("the collective's rate"), He.ThrottleLeverRate, 1.0);
			Eq(TEXT("the rotor's drag"), He.LinearDamping, 0.1);
			Eq(TEXT("no angular damping"), He.AngularDamping, 0.0);
			if (Test.TestEqual(*(Label + TEXT(": one main rotor")), He.Rotors.Num(), 1))
			{
				const FMantleVehicleRotorDesc& Rotor = He.Rotors[0];
				EqName(TEXT("rotor bone"), Rotor.Bone, FMantleVehicleChassisBuilder::MainRotorBoneName());
				EqV(TEXT("rotor hub"), Rotor.Location, Spec.MainRotorHub());
				EqV(TEXT("rotor lifts along up"), Rotor.Axis, FVector::UpVector);
				Eq(TEXT("rotor thrust"), Rotor.MaxThrustNewtons, H.RotorThrustNewtons);
				// The rotor's other numbers are the rotor struct's defaults.
				Eq(TEXT("pitch acceleration"), Rotor.PitchAcceleration, 1.5);
				Eq(TEXT("roll acceleration"), Rotor.RollAcceleration, 1.5);
				Eq(TEXT("yaw acceleration"), Rotor.YawAcceleration, 1.0);
				Expect(TEXT("stability assist"), Rotor.bStabilityAssist, true);
				Eq(TEXT("stability stiffness"), Rotor.StabilityStiffness, 3.0);
				Eq(TEXT("stability damping"), Rotor.StabilityDamping, 2.0);
				Eq(TEXT("stability max acceleration"), Rotor.StabilityMaxAcceleration, 4.0);
			}
			if (Test.TestEqual(*(Label + TEXT(": two spinning rotors")), He.SpinBones.Num(), 2))
			{
				EqName(TEXT("main spin bone"), He.SpinBones[0].Bone, FMantleVehicleChassisBuilder::MainRotorBoneName());
				EqV(TEXT("main spins about Z"), He.SpinBones[0].Axis, FVector::UpVector);
				Eq(TEXT("main rpm"), He.SpinBones[0].Rpm, Heli.RotorRpm);
				EqName(TEXT("tail spin bone"), He.SpinBones[1].Bone, FMantleVehicleChassisBuilder::TailRotorBoneName());
				EqV(TEXT("tail spins about Y"), He.SpinBones[1].Axis, FVector::RightVector);
				Eq(TEXT("tail rpm"), He.SpinBones[1].Rpm, Heli.TailRotorRpm);
			}
			Expect(TEXT("the pilot's hands"), He.bPilotGrips, true);
			EqName(TEXT("the collective grip is on the root"), He.PilotGrips.Left.Bone, Root);
			EqName(TEXT("the cyclic grip is on the root"), He.PilotGrips.Right.Bone, Root);
			EqV(TEXT("collective grip"), He.PilotGrips.Left.Location, Heli.CollectiveGrip);
			EqV(TEXT("cyclic grip"), He.PilotGrips.Right.Location, Heli.CyclicGrip);
			Eq(TEXT("a pilot steps out past the skids"), D.SeatExitSideDistance, FMath::Max(0.5 * Spec.Width, 0.5 * Spec.Track) + 145.0 * Spec.Scale);
		}

		if (Spec.Hovercraft.bSet)
		{
			const FMantleChassisHovercraft& Craft = Spec.Hovercraft;
			const FMantleVehicleHoverDesc& Ho = D.Hover;
			if (Test.TestEqual(*(Label + TEXT(": a pad where each of the layout's wheels is")), Ho.Pads.Num(), Spec.WheelCount()))
			{
				for (int32 Index = 0; Index < Ho.Pads.Num(); ++Index)
				{
					EqV(TEXT("pad location"), Ho.Pads[Index].Location, Spec.PadLocation(Index));
					Eq(TEXT("pad raise"), Ho.Pads[Index].MaxRaise, Craft.PadRaise);
					Eq(TEXT("pad travel"), Ho.Pads[Index].MaxDrop, Craft.PadTravel);
					Eq(TEXT("pad spring rate"), Ho.Pads[Index].SpringRate, H.PadSpringRate);
					Eq(TEXT("pad damping"), Ho.Pads[Index].SpringDamping, Craft.PadDamping);
				}
			}
			Eq(TEXT("angular damping, on the ground and off it"), Ho.AngularDamping, 0.5);
			EqV(TEXT("the fan's pod"), Ho.Thruster.Location, Spec.FanLocation());
			EqV(TEXT("the fan pushes forward"), Ho.Thruster.Direction, FVector::ForwardVector);
			Eq(TEXT("the fan's thrust"), Ho.Thruster.MaxThrustNewtons, H.HoverThrustNewtons);
			Eq(TEXT("the reverse vanes"), Ho.Thruster.ReverseShare, Craft.ReverseShare);
			Eq(TEXT("the vanes' angle"), Ho.Thruster.VectorAngleDegrees, Craft.VectorAngle);
			Eq(TEXT("the push falls off at the top speed"), Ho.Thruster.TopSpeedCmPerSec, H.MaxSpeed);
			Eq(TEXT("the brake fade is the struct's"), Ho.Thruster.BrakeFadeCmPerSec, 100.0);
			Expect(TEXT("a fin"), Ho.bRudder, true);
			EqV(TEXT("the fin's middle"), Ho.Rudder.Location, Spec.HoverRudderLocation());
			Eq(TEXT("rudder area"), Ho.Rudder.AreaM2, H.HoverFinAreaM2);
			Eq(TEXT("rudder camber"), Ho.Rudder.Camber, 0.0);
			Eq(TEXT("rudder control angle"), Ho.Rudder.MaxControlAngle, Craft.FinMaxControlAngle);
			Eq(TEXT("rudder stall angle"), Ho.Rudder.StallAngle, Craft.FinStallAngle);
			Eq(TEXT("lateral damping"), Ho.Damping.LateralPerSecond, Craft.LateralPerSecond);
			Eq(TEXT("max lateral accel"), Ho.Damping.MaxLateralAccel, Craft.MaxLateralAccel);
			Eq(TEXT("yaw damping"), Ho.Damping.YawPerSecond, Craft.YawPerSecond);
			Eq(TEXT("max yaw accel"), Ho.Damping.MaxYawAccelDegrees, Craft.MaxYawAccelDegrees);
			Eq(TEXT("skirt brake"), Ho.Damping.SkirtBrakePerSecond, Craft.SkirtBrakePerSecond);
			Eq(TEXT("max skirt brake accel"), Ho.Damping.MaxSkirtBrakeAccel, Craft.MaxSkirtBrakeAccel);
		}
	}

	/** Text of a composed document of Selector with Mutate applied to its deserialized JSON. */
	FString Mutated(FAutomationTestBase& Test, const FString& Selector, TFunctionRef<void(FJsonObject&)> Mutate)
	{
		FMantleChassisSpec Spec;
		if (!LoadSpec(Test, Selector, Spec))
		{
			return FString();
		}
		const TSharedPtr<FJsonObject> Doc = ParseObject(FMantleChassisSpec::ComposeDocument(Spec));
		if (!Doc.IsValid())
		{
			Test.AddError(TEXT("the composed document is not JSON"));
			return FString();
		}
		Mutate(*Doc);
		return Serialize(Doc.ToSharedRef());
	}

	TSharedPtr<FJsonObject> Child(FJsonObject& Object, const TCHAR* Key)
	{
		const TSharedPtr<FJsonObject>* Found = nullptr;
		return Object.TryGetObjectField(Key, Found) && Found ? *Found : nullptr;
	}

	/** The i-th object of Object's array Key. */
	TSharedPtr<FJsonObject> Item(FJsonObject& Object, const TCHAR* Key, const int32 Index)
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		return Object.TryGetArrayField(Key, Array) && Array && Array->IsValidIndex(Index) ? (*Array)[Index]->AsObject() : nullptr;
	}

	TSharedRef<FJsonValue> Triple(const double X, const double Y, const double Z)
	{
		const TArray<TSharedPtr<FJsonValue>> Parts = { MakeShared<FJsonValueNumber>(X), MakeShared<FJsonValueNumber>(Y), MakeShared<FJsonValueNumber>(Z) };
		return MakeShared<FJsonValueArray>(Parts);
	}
}

// ---- Spec to document to Desc ------------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleDocumentGoldenTest,
	"Mantle.Vehicle.Document.SpecToDescIsTheDevPath",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleDocumentGoldenTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleDocumentTestPrivate;
	const TArray<FString> Selectors = MantleVehicleDocumentTestPrivate::Selectors();
	// The twelve presets at scale 1 and at 0.7.
	TestEqual(TEXT("twelve presets, two sizes each"), Selectors.Num(), 24);
	for (const FString& Selector : Selectors)
	{
		FMantleChassisSpec Spec;
		if (!LoadSpec(*this, Selector, Spec))
		{
			continue;
		}
		FMantleVehicleDesc Desc;
		FString Error;
		const FString Text = FMantleChassisSpec::ComposeDocument(Spec);
		if (!TestTrue(*FString::Printf(TEXT("%s: the composed document reads (%s)"), *Selector, *Error), ParseDesc(Text, Desc, Error)))
		{
			continue;
		}
		CheckAgainstDevPath(*this, Selector, Spec, Desc);

		// Deriving straight from the spec gives what the document read as, and holds nothing back at these sizes.
		FMantleVehicleDesc Derived;
		TArray<FString> Held;
		FMantleVehicleChassisDocument::Derive(Spec, Derived, &Held);
		TestEqual(*FString::Printf(TEXT("%s: no number is held to a range (%s)"), *Selector, *FString::Join(Held, TEXT("; "))), Held.Num(), 0);
		NameRootGrips(Desc);
		const FString FromDocument = MovementText(Desc);
		const FString FromSpec = MovementText(Derived);
		TestTrue(*FString::Printf(TEXT("%s: Derive and the document's reading agree (%s)"), *Selector, *FirstDifferenceOf(FromDocument, FromSpec)),
			FromDocument.Equals(FromSpec, ESearchCase::CaseSensitive));

		// The pure wheeled types write nothing new: a car composes to the document it did before the movement keys existed.
		if (!Spec.Aircraft.bSet && !Spec.Helicopter.bSet && !Spec.Hovercraft.bSet && !Spec.Rider.bSet)
		{
			const TSharedPtr<FJsonObject> Doc = ParseObject(Text);
			const TSharedPtr<FJsonObject> Copy = ParseObject(Text);
			StripMovement(*Copy);
			TestTrue(*FString::Printf(TEXT("%s: a car writes no movement key"), *Selector), Doc.IsValid() && Copy.IsValid() && FJsonValue::CompareEqual(FJsonValueObject(Doc), FJsonValueObject(Copy)));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleDocumentExamplesTest,
	"Mantle.Vehicle.Document.ReadsTheSchemaExamples",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleDocumentExamplesTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleDocumentTestPrivate;
	// schema/examples/vehicle/valid-<preset>.json are composed from the presets at scale 1 by the cook's side: the numbers they hold are not C++'s.
	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("../schema/examples/vehicle")));
	if (!TestTrue(*FString::Printf(TEXT("the schema examples are beside this project (%s): a test that compares nothing proves nothing"), *Directory), FPaths::DirectoryExists(Directory)))
	{
		return false;
	}
	for (const TCHAR* Id : { TEXT("plane"), TEXT("jet"), TEXT("helicopter"), TEXT("hovercraft"), TEXT("motorcycle") })
	{
		FString Example;
		if (!TestTrue(*FString::Printf(TEXT("%s: the example exists"), Id), FFileHelper::LoadFileToString(Example, *FPaths::Combine(Directory, FString::Printf(TEXT("valid-%s.json"), Id)))))
		{
			continue;
		}
		FMantleChassisSpec Spec;
		FMantleVehicleDesc Wrote;
		FMantleVehicleDesc Read;
		FString Error;
		if (!LoadSpec(*this, Id, Spec))
		{
			continue;
		}
		const FString Composed = FMantleChassisSpec::ComposeDocument(Spec);
		const TSharedPtr<FJsonObject> ExampleJson = ParseObject(Example);
		const TSharedPtr<FJsonObject> ComposedJson = ParseObject(Composed);
		if (!TestTrue(*FString::Printf(TEXT("%s: the example is JSON"), Id), ExampleJson.IsValid() && ComposedJson.IsValid())
			|| !TestTrue(*FString::Printf(TEXT("%s: the example's movement part reads (%s)"), Id, *Error),
				ParseDesc(Serialize(WithExampleMovement(ComposedJson, ExampleJson)), Read, Error))
			|| !TestTrue(*FString::Printf(TEXT("%s: the composed document reads (%s)"), Id, *Error), ParseDesc(Composed, Wrote, Error)))
		{
			continue;
		}
		NameRootGrips(Read);
		NameRootGrips(Wrote);
		// The examples spell a number to a few decimals where the editor's arithmetic has its last binary digits: equal to a thousandth of a cm.
		const FString A = MovementText(Wrote);
		const FString B = MovementText(Read);
		TSharedPtr<FJsonValue> JsonA;
		TSharedPtr<FJsonValue> JsonB;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(A), JsonA);
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(B), JsonB);
		const FString Difference = JsonDifference(JsonA, JsonB, FString(), 1.0e-3);
		TestTrue(*FString::Printf(TEXT("%s: the document the editor composes is the example (differs at %s: %s)"), Id, *Difference, *FirstDifferenceOf(A, B)), Difference.IsEmpty());
	}
	return true;
}

// ---- Document to Desc to document -------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleDocumentRoundTripTest,
	"Mantle.Vehicle.Document.RoundTrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleDocumentRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleDocumentTestPrivate;
	for (const FString& Selector : Selectors())
	{
		FMantleChassisSpec Spec;
		if (!LoadSpec(*this, Selector, Spec))
		{
			continue;
		}
		// The document MVH1 would hold: resolved, every default written. The reader reads it and Write writes what it read: the same document, key for key.
		const TSharedPtr<FJsonObject> Original = ParseObject(FMantleChassisSpec::ComposeDocument(Spec));
		FMantleVehicleDesc Read;
		FString Error;
		if (!TestTrue(*FString::Printf(TEXT("%s: the document is JSON"), *Selector), Original.IsValid())
			|| !TestTrue(*FString::Printf(TEXT("%s: it reads (%s)"), *Selector, *Error), ParseDesc(Serialize(Original.ToSharedRef()), Read, Error)))
		{
			continue;
		}
		const TSharedRef<FJsonObject> Rebuilt = MakeShared<FJsonObject>();
		FJsonObject::Duplicate(Original, Rebuilt);
		StripMovement(*Rebuilt);
		FMantleVehicleChassisDocument::Write(Read, *Rebuilt);
		TestTrue(*FString::Printf(TEXT("%s: writing what was read gives the document back"), *Selector),
			FJsonValue::CompareEqual(FJsonValueObject(Original), FJsonValueObject(Rebuilt)));

		// And reading that again gives the same Desc.
		FMantleVehicleDesc Again;
		if (TestTrue(*FString::Printf(TEXT("%s: the written document reads (%s)"), *Selector, *Error), ParseDesc(Serialize(Rebuilt), Again, Error)))
		{
			const FString A = MovementText(Read);
			const FString B = MovementText(Again);
			TestTrue(*FString::Printf(TEXT("%s: and reads as the same vehicle (%s)"), *Selector, *FirstDifferenceOf(A, B)), A.Equals(B, ESearchCase::CaseSensitive));
		}

		// The chassis builder's body holds every bone the blocks name, and the ones it poses are free of the wheels (the check Read makes of a published body).
		FMantleVehicleAsset Asset;
		TestTrue(*FString::Printf(TEXT("%s: its body holds the bones its blocks name (%s)"), *Selector, *Error), FMantleVehicleChassisBuilder::MakeAsset(Spec, Asset, Error));
	}
	return true;
}

// ---- A chassis past what a document can say --------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleDocumentHeldTest,
	"Mantle.Vehicle.Document.ResizedChassisIsHeldToTheRanges",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleDocumentHeldTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleDocumentTestPrivate;
	// A plane 1.6 times the preset's size has its tail surfaces 13 m back: past the +-1000 cm a document can say. Positive control: it is held, said so, and the
	// document still reads, with the held position at the range's end.
	FMantleChassisSpec Plane;
	if (!LoadSpec(*this, TEXT("plane@1.6"), Plane))
	{
		return false;
	}
	FMantleVehicleDesc Desc;
	TArray<FString> Held;
	FMantleVehicleChassisDocument::Derive(Plane, Desc, &Held);
	TestTrue(TEXT("the elevator's X is reported held"), Held.ContainsByPredicate([](const FString& Line) { return Line.StartsWith(TEXT("flight/aerofoils/*/location/*")); }));
	if (TestEqual(TEXT("four aerofoils"), Desc.Flight.Aerofoils.Num(), 4))
	{
		TestEqual(TEXT("the elevator is held at -1000"), Desc.Flight.Aerofoils[2].Location.X, -1000.0);
		TestTrue(TEXT("the unheld ones are the spec's"), Desc.Flight.Aerofoils[2].Location.Z == Plane.ElevatorLocation().Z);
	}

	// Every preset at the selectors' least and largest sizes, and the ones between, composes a document that reads and whose blocks are within their ranges.
	for (const FString& Id : FMantleVehicleChassisBuilder::PresetIds())
	{
		for (const double Scale : { 0.25, 1.3, 1.6, 2.5 })
		{
			const FString Selector = FMantleChassisSpec::MakeSelector(Id, Scale);
			FMantleChassisSpec Spec;
			FMantleVehicleDesc Read;
			FString Error;
			if (LoadSpec(*this, Selector, Spec))
			{
				// A body past the collision box's range (the six-wheeler at 2.5) is refused for that, as it was before the movement keys existed
				// (Mantle.Vehicle.Chassis.ScaledChassisMakesValidAssets): the movement part is not what refuses it.
				const bool bRead = ParseDesc(FMantleChassisSpec::ComposeDocument(Spec), Read, Error);
				TestTrue(*FString::Printf(TEXT("%s: the document reads (%s)"), *Selector, *Error), bRead || Error.StartsWith(TEXT("/collision/")));
			}
		}
	}
	return true;
}

// ---- A ground car's handling -----------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleDocumentGroundHandlingTest,
	"Mantle.Vehicle.Document.GroundHandlingIsBakedIn",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleDocumentGroundHandlingTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleDocumentTestPrivate;
	const TArray<FVector2D> WantCurve = { FVector2D(0.0, 1.0), FVector2D(20.0, 0.8), FVector2D(60.0, 0.4), FVector2D(120.0, 0.3) };
	int32 Grounded = 0;
	for (const FString& Selector : Selectors())
	{
		FMantleChassisSpec Spec;
		FMantleVehicleDesc D;
		FString Error;
		if (!LoadSpec(*this, Selector, Spec) || !TestTrue(*FString::Printf(TEXT("%s: the document reads (%s)"), *Selector, *Error), ParseDesc(FMantleChassisSpec::ComposeDocument(Spec), D, Error)))
		{
			continue;
		}
		const bool bGround = !Spec.Aircraft.bSet && !Spec.Helicopter.bSet && !Spec.Hovercraft.bSet && Spec.WheelCount() > 2;
		if (!bGround)
		{
			// A bike keeps its tyres' own, a plane's, a helicopter's and a hovercraft's are not a car's: neither key is written.
			TestTrue(*FString::Printf(TEXT("%s: no curve and no rear grip"), *Selector), D.SteeringSpeedCurve.IsEmpty() && !D.bTireRearFriction);
			continue;
		}
		++Grounded;
		// The preset's one friction is the middle: the front gets 6/7 of it, the rear 8/7 (3.0 and 4.0 of a 3.5), and nothing else is touched.
		const double Friction = (Spec.Derived.IsSet() ? Spec.Derived.GetValue() : Spec.Archetype).TireFriction;
		TestTrue(*FString::Printf(TEXT("%s: the front grips 6/7 of the preset's friction (%g of %g)"), *Selector, D.TireFriction, Friction), FMath::IsNearlyEqual(D.TireFriction, Friction * 6.0 / 7.0, 1.0e-9));
		TestTrue(*FString::Printf(TEXT("%s: the rear grips 8/7 of it (%g)"), *Selector, D.TireRearFriction), D.bTireRearFriction && FMath::IsNearlyEqual(D.TireRearFriction, Friction * 8.0 / 7.0, 1.0e-9));
		if (TestEqual(*FString::Printf(TEXT("%s: the steering curve has four points"), *Selector), D.SteeringSpeedCurve.Num(), WantCurve.Num()))
		{
			for (int32 Index = 0; Index < WantCurve.Num(); ++Index)
			{
				TestEqual(*FString::Printf(TEXT("%s: curve point %d"), *Selector, Index), D.SteeringSpeedCurve[Index], WantCurve[Index]);
			}
		}
	}
	TestTrue(TEXT("the cars, the trucks and the six-wheeler were checked"), Grounded >= 12);

	// The reader refuses a malformed curve or a rear grip out of range.
	auto Refuses = [this](const TCHAR* Name, const TCHAR* Expect, TFunctionRef<void(FJsonObject&)> Mutate)
	{
		const FString Text = Mutated(*this, TEXT("sedan"), Mutate);
		FMantleVehicleDesc D;
		FString Error;
		if (TestFalse(*FString::Printf(TEXT("%s is refused"), Name), ParseDesc(Text, D, Error)))
		{
			TestTrue(*FString::Printf(TEXT("%s: the refusal says '%s' (it says: %s)"), Name, Expect, *Error), Error.Contains(Expect));
		}
	};
	auto SetCurve = [](FJsonObject& Doc, const TArray<TArray<double>>& Points)
	{
		TArray<TSharedPtr<FJsonValue>> Curve;
		for (const TArray<double>& Point : Points)
		{
			const TArray<TSharedPtr<FJsonValue>> Pair = { MakeShared<FJsonValueNumber>(Point[0]), MakeShared<FJsonValueNumber>(Point[1]) };
			Curve.Add(MakeShared<FJsonValueArray>(Pair));
		}
		Child(Doc, TEXT("steering"))->SetArrayField(TEXT("speedCurve"), Curve);
	};
	Refuses(TEXT("a curve going back in speed"), TEXT("does not rise"), [&SetCurve](FJsonObject& Doc) { SetCurve(Doc, { { 0, 1 }, { 60, 0.4 }, { 40, 0.3 } }); });
	Refuses(TEXT("a curve with a repeated speed"), TEXT("does not rise"), [&SetCurve](FJsonObject& Doc) { SetCurve(Doc, { { 0, 1 }, { 0, 0.5 } }); });
	Refuses(TEXT("a steering share of 0.05"), TEXT("outside"), [&SetCurve](FJsonObject& Doc) { SetCurve(Doc, { { 0, 1 }, { 20, 0.05 } }); });
	Refuses(TEXT("a speed of 250 mph"), TEXT("outside"), [&SetCurve](FJsonObject& Doc) { SetCurve(Doc, { { 0, 1 }, { 250, 0.5 } }); });
	Refuses(TEXT("an empty curve"), TEXT("0 entries"), [&SetCurve](FJsonObject& Doc) { SetCurve(Doc, {}); });
	Refuses(TEXT("nine points"), TEXT("9 entries"), [&SetCurve](FJsonObject& Doc) { SetCurve(Doc, { { 0, 1 }, { 1, 1 }, { 2, 1 }, { 3, 1 }, { 4, 1 }, { 5, 1 }, { 6, 1 }, { 7, 1 }, { 8, 1 } }); });
	Refuses(TEXT("a point of one number"), TEXT("1 entries"), [](FJsonObject& Doc)
	{
		const TArray<TSharedPtr<FJsonValue>> One = { MakeShared<FJsonValueNumber>(0) };
		const TArray<TSharedPtr<FJsonValue>> Curve = { MakeShared<FJsonValueArray>(One) };
		Child(Doc, TEXT("steering"))->SetArrayField(TEXT("speedCurve"), Curve);
	});
	Refuses(TEXT("a rear grip of 12"), TEXT("outside"), [](FJsonObject& Doc) { Child(Doc, TEXT("tires"))->SetNumberField(TEXT("rearFriction"), 12); });
	Refuses(TEXT("an unknown tires key"), TEXT("tires has bogus"), [](FJsonObject& Doc) { Child(Doc, TEXT("tires"))->SetNumberField(TEXT("bogus"), 1); });
	return true;
}

// ---- Defaults -----------------------------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleDocumentDefaultsTest,
	"Mantle.Vehicle.Document.OptionalKeysReadAsTheContractsDefaults",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleDocumentDefaultsTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleDocumentTestPrivate;
	FString Error;
	FMantleVehicleDesc D;

	// A plane's block with only what it must have: every other number is the contract's default.
	const FString Plane = Mutated(*this, TEXT("plane"), [](FJsonObject& Doc)
	{
		const TSharedPtr<FJsonObject> Flight = Child(Doc, TEXT("flight"));
		for (const TCHAR* Key : { TEXT("cruiseThrottle"), TEXT("pitchAuthority"), TEXT("throttleLeverRate"), TEXT("linearDamping"), TEXT("angularDamping"), TEXT("spinBones"), TEXT("pilotGrips") })
		{
			Flight->RemoveField(Key);
		}
		for (int32 Index = 0; Index < 4; ++Index)
		{
			const TSharedPtr<FJsonObject> Surface = Item(*Flight, TEXT("aerofoils"), Index);
			for (const TCHAR* Key : { TEXT("bone"), TEXT("mirror"), TEXT("camber"), TEXT("maxControlAngle"), TEXT("stallAngle") })
			{
				Surface->RemoveField(Key);
			}
		}
		Item(*Flight, TEXT("thrusters"), 0)->RemoveField(TEXT("direction"));
		Item(*Flight, TEXT("thrusters"), 0)->RemoveField(TEXT("bone"));
		Child(Doc, TEXT("camera"))->RemoveField(TEXT("boomLocation"));
		Child(Doc, TEXT("seat"))->RemoveField(TEXT("exitSideDistance"));
		Child(Doc, TEXT("seat"))->RemoveField(TEXT("kind"));
	});
	if (TestTrue(*FString::Printf(TEXT("a plane with only its required keys reads (%s)"), *Error), ParseDesc(Plane, D, Error)))
	{
		TestEqual(TEXT("cruise throttle 0.4"), D.Flight.CruiseThrottle, 0.4);
		TestEqual(TEXT("pitch authority 0.5"), D.Flight.PitchAuthority, 0.5);
		TestEqual(TEXT("throttle lever rate 0.5"), D.Flight.ThrottleLeverRate, 0.5);
		TestEqual(TEXT("no linear damping"), D.Flight.LinearDamping, 0.0);
		TestEqual(TEXT("no angular damping"), D.Flight.AngularDamping, 0.0);
		TestEqual(TEXT("no spin bones"), D.Flight.SpinBones.Num(), 0);
		TestFalse(TEXT("no pilot grips"), D.Flight.bPilotGrips);
		TestEqual(TEXT("the car seat kind"), static_cast<int32>(D.SeatKind), static_cast<int32>(EMantleVehicleSeatKind::Car));
		TestFalse(TEXT("no exit distance"), D.bSeatExitSideDistance);
		TestFalse(TEXT("no boom"), D.bCameraBoom);
		TestTrue(TEXT("a surface with no bone poses nothing"), D.Flight.Aerofoils.Num() == 4 && D.Flight.Aerofoils[0].Bone.IsNone());
		for (const FMantleVehicleAerofoilDesc& Surface : D.Flight.Aerofoils)
		{
			TestFalse(TEXT("not mirrored"), Surface.bMirror);
			TestEqual(TEXT("camber 5"), Surface.Camber, 5.0);
			TestEqual(TEXT("control angle 12"), Surface.MaxControlAngle, 12.0);
			TestEqual(TEXT("stall angle 20"), Surface.StallAngle, 20.0);
		}
		TestTrue(TEXT("a thruster pushes forward"), D.Flight.Thrusters[0].Direction.Equals(FVector::ForwardVector));
	}

	// A helicopter's rotor and a hovercraft's pad and fan with only theirs.
	const FString Heli = Mutated(*this, TEXT("helicopter"), [](FJsonObject& Doc)
	{
		const TSharedPtr<FJsonObject> Block = Child(Doc, TEXT("helicopter"));
		for (const TCHAR* Key : { TEXT("throttleLeverRate"), TEXT("linearDamping"), TEXT("angularDamping") })
		{
			Block->RemoveField(Key);
		}
		const TSharedPtr<FJsonObject> Rotor = Item(*Block, TEXT("rotors"), 0);
		for (const TCHAR* Key : { TEXT("axis"), TEXT("pitchAcceleration"), TEXT("rollAcceleration"), TEXT("yawAcceleration"), TEXT("stabilityAssist"), TEXT("stabilityStiffness"),
			TEXT("stabilityDamping"), TEXT("stabilityMaxAcceleration") })
		{
			Rotor->RemoveField(Key);
		}
	});
	if (TestTrue(*FString::Printf(TEXT("a helicopter with only its required keys reads (%s)"), *Error), ParseDesc(Heli, D, Error)))
	{
		TestEqual(TEXT("collective rate 1"), D.Helicopter.ThrottleLeverRate, 1.0);
		TestEqual(TEXT("linear damping 0.1"), D.Helicopter.LinearDamping, 0.1);
		TestEqual(TEXT("angular damping 0"), D.Helicopter.AngularDamping, 0.0);
		const FMantleVehicleRotorDesc& Rotor = D.Helicopter.Rotors[0];
		TestTrue(TEXT("the rotor lifts along up"), Rotor.Axis.Equals(FVector::UpVector));
		TestEqual(TEXT("pitch 1.5"), Rotor.PitchAcceleration, 1.5);
		TestEqual(TEXT("roll 1.5"), Rotor.RollAcceleration, 1.5);
		TestEqual(TEXT("yaw 1"), Rotor.YawAcceleration, 1.0);
		TestTrue(TEXT("stability assist on"), Rotor.bStabilityAssist);
		TestEqual(TEXT("stiffness 3"), Rotor.StabilityStiffness, 3.0);
		TestEqual(TEXT("damping 2"), Rotor.StabilityDamping, 2.0);
		TestEqual(TEXT("max acceleration 4"), Rotor.StabilityMaxAcceleration, 4.0);
	}
	const FString Hover = Mutated(*this, TEXT("hovercraft"), [](FJsonObject& Doc)
	{
		const TSharedPtr<FJsonObject> Block = Child(Doc, TEXT("hover"));
		Block->RemoveField(TEXT("damping"));
		Block->RemoveField(TEXT("angularDamping"));
		Block->RemoveField(TEXT("rudder"));
		for (const TCHAR* Key : { TEXT("direction"), TEXT("reverseShare"), TEXT("vectorAngle"), TEXT("topSpeed"), TEXT("brakeFade") })
		{
			Child(*Block, TEXT("thruster"))->RemoveField(Key);
		}
		for (int32 Index = 0; Index < 4; ++Index)
		{
			for (const TCHAR* Key : { TEXT("maxRaise"), TEXT("maxDrop"), TEXT("springRate"), TEXT("springDamping") })
			{
				Item(*Block, TEXT("pads"), Index)->RemoveField(Key);
			}
		}
	});
	if (TestTrue(*FString::Printf(TEXT("a hovercraft with only its required keys reads (%s)"), *Error), ParseDesc(Hover, D, Error)))
	{
		TestFalse(TEXT("no fin"), D.Hover.bRudder);
		TestEqual(TEXT("angular damping 0.5"), D.Hover.AngularDamping, 0.5);
		TestEqual(TEXT("lateral 1.5"), D.Hover.Damping.LateralPerSecond, 1.5);
		TestEqual(TEXT("max lateral accel 400"), D.Hover.Damping.MaxLateralAccel, 400.0);
		TestEqual(TEXT("yaw 2"), D.Hover.Damping.YawPerSecond, 2.0);
		TestEqual(TEXT("max yaw accel 180"), D.Hover.Damping.MaxYawAccelDegrees, 180.0);
		TestEqual(TEXT("skirt brake 2"), D.Hover.Damping.SkirtBrakePerSecond, 2.0);
		TestEqual(TEXT("max skirt brake accel 500"), D.Hover.Damping.MaxSkirtBrakeAccel, 500.0);
		TestEqual(TEXT("reverse share 0.5"), D.Hover.Thruster.ReverseShare, 0.5);
		TestEqual(TEXT("vane angle 25"), D.Hover.Thruster.VectorAngleDegrees, 25.0);
		TestEqual(TEXT("top speed 2500"), D.Hover.Thruster.TopSpeedCmPerSec, 2500.0);
		TestEqual(TEXT("brake fade 100"), D.Hover.Thruster.BrakeFadeCmPerSec, 100.0);
		TestEqual(TEXT("pad raise 10"), D.Hover.Pads[0].MaxRaise, 10.0);
		TestEqual(TEXT("pad drop 50"), D.Hover.Pads[0].MaxDrop, 50.0);
		TestEqual(TEXT("pad spring 37"), D.Hover.Pads[0].SpringRate, 37.0);
		TestEqual(TEXT("pad damping 0.6"), D.Hover.Pads[0].SpringDamping, 0.6);
	}

	// A rider's grips with no bone: the handlebar's on the fork, the pegs left for the body's root.
	const FString Bike = Mutated(*this, TEXT("motorcycle"), [](FJsonObject& Doc)
	{
		const TSharedPtr<FJsonObject> Rider = Child(*Child(Doc, TEXT("seat")), TEXT("rider"));
		for (const TCHAR* Pair : { TEXT("handlebar"), TEXT("footPegs") })
		{
			for (const TCHAR* Side : { TEXT("left"), TEXT("right") })
			{
				Child(*Child(*Rider, Pair), Side)->RemoveField(TEXT("bone"));
			}
		}
		for (const TCHAR* Key : { TEXT("pelvisOffset"), TEXT("stopPelvisShift"), TEXT("kneeSplayDegrees"), TEXT("rearTireFriction"), TEXT("rearBrakeTorque"), TEXT("rotationControl") })
		{
			Rider->RemoveField(Key);
		}
	});
	if (TestTrue(*FString::Printf(TEXT("a bike with only its required keys reads (%s)"), *Error), ParseDesc(Bike, D, Error)))
	{
		TestEqual(TEXT("the left handlebar grip is on the fork"), D.Rider.Handlebar.Left.Bone, D.Rider.ForkBone);
		TestEqual(TEXT("the right handlebar grip is on the fork"), D.Rider.Handlebar.Right.Bone, D.Rider.ForkBone);
		TestTrue(TEXT("the pegs name no bone yet"), D.Rider.FootPegs.Left.Bone.IsNone() && D.Rider.FootPegs.Right.Bone.IsNone());
		TestEqual(TEXT("knee splay 8"), D.Rider.KneeSplayDegrees, 8.0);
		TestFalse(TEXT("no rear friction"), D.Rider.bRearTireFriction);
		TestFalse(TEXT("no rear brake"), D.Rider.bRearBrakeTorque);
		TestTrue(TEXT("a missing rotation control key is 0 or false, not the definition's default"),
			!D.Rider.RotationControl.bTorqueControl && !D.Rider.RotationControl.bTargetRotation && D.Rider.RotationControl.RotationStiffness == 0.0 && D.Rider.PelvisOffset.IsZero());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleWeaponArcTest,
	"Mantle.Vehicle.Document.ReadsSeatWeaponArcsAndLeavesUnsetSeatsDerived",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleWeaponArcTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleDocumentTestPrivate;
	FString Error;
	FMantleVehicleDesc D;
	const FString Legacy = Mutated(*this, TEXT("sedan"), [](FJsonObject& Doc)
	{
		const TSharedPtr<FJsonObject> Driver = Child(Doc, TEXT("seat"));
		Driver->RemoveField(TEXT("weaponArc"));
		const TSharedRef<FJsonObject> Passenger = MakeShared<FJsonObject>();
		Passenger->SetField(TEXT("location"), Driver->TryGetField(TEXT("location")));
		Doc.SetObjectField(TEXT("passengerSeat"), Passenger);
	});
	if (!TestTrue(*FString::Printf(TEXT("a legacy document without arcs reads (%s)"), *Error), ParseDesc(Legacy, D, Error)))
	{
		return false;
	}
	TestFalse(TEXT("a driver seat without an arc has none set (its arc is worked out from the seat)"), D.SeatWeaponArc.bSet);
	TestFalse(TEXT("a passenger seat without an arc has none set"), D.PassengerSeatWeaponArc.bSet);

	const FString Explicit = Mutated(*this, TEXT("sedan"), [](FJsonObject& Doc)
	{
		auto AddArc = [](FJsonObject& Seat, const double YawMin, const double YawMax, const double PitchMin, const double PitchMax)
		{
			const TSharedRef<FJsonObject> Arc = MakeShared<FJsonObject>();
			Arc->SetNumberField(TEXT("yawMin"), YawMin);
			Arc->SetNumberField(TEXT("yawMax"), YawMax);
			Arc->SetNumberField(TEXT("pitchMin"), PitchMin);
			Arc->SetNumberField(TEXT("pitchMax"), PitchMax);
			Seat.SetObjectField(TEXT("weaponArc"), Arc);
		};
		AddArc(*Child(Doc, TEXT("seat")), -55.0, 120.0, -5.0, 45.0);
		const TSharedRef<FJsonObject> Passenger = MakeShared<FJsonObject>();
		Passenger->SetField(TEXT("location"), Child(Doc, TEXT("seat"))->TryGetField(TEXT("location")));
		AddArc(*Passenger, -70.0, 95.0, -10.0, 50.0);
		Doc.SetObjectField(TEXT("passengerSeat"), Passenger);
	});
	if (!TestTrue(*FString::Printf(TEXT("explicit seat arcs read (%s)"), *Error), ParseDesc(Explicit, D, Error)))
	{
		return false;
	}
	TestTrue(TEXT("the driver's arc is set"), D.SeatWeaponArc.bSet);
	TestTrue(TEXT("the passenger's arc is set"), D.PassengerSeatWeaponArc.bSet);
	TestEqual(TEXT("driver yaw minimum"), D.SeatWeaponArc.YawMin, -55.0);
	TestEqual(TEXT("passenger yaw maximum"), D.PassengerSeatWeaponArc.YawMax, 95.0);
	TestEqual(TEXT("passenger pitch maximum"), D.PassengerSeatWeaponArc.PitchMax, 50.0);

	const FString Inverted = Mutated(*this, TEXT("sedan"), [](FJsonObject& Doc)
	{
		const TSharedRef<FJsonObject> Arc = MakeShared<FJsonObject>();
		Arc->SetNumberField(TEXT("yawMin"), 20.0);
		Arc->SetNumberField(TEXT("yawMax"), -20.0);
		Arc->SetNumberField(TEXT("pitchMin"), -90.0);
		Arc->SetNumberField(TEXT("pitchMax"), 90.0);
		Child(Doc, TEXT("seat"))->SetObjectField(TEXT("weaponArc"), Arc);
	});
	TestFalse(TEXT("an inverted yaw arc is refused"), ParseDesc(Inverted, D, Error));
	TestTrue(TEXT("the refusal names the yaw minimum"), Error.Contains(TEXT("/seat/weaponArc/yawMin")));
	return true;
}

// ---- Refusals -----------------------------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleDocumentRefusalsTest,
	"Mantle.Vehicle.Document.RefusesMalformedBlocks",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleDocumentRefusalsTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleDocumentTestPrivate;

	// A document with Mutate applied must not read, and the refusal says Expect.
	auto Refuses = [this](const TCHAR* Name, const TCHAR* Selector, const TCHAR* Expect, TFunctionRef<void(FJsonObject&)> Mutate)
	{
		const FString Text = Mutated(*this, Selector, Mutate);
		FMantleVehicleDesc D;
		FString Error;
		if (TestFalse(*FString::Printf(TEXT("%s is refused"), Name), ParseDesc(Text, D, Error)))
		{
			TestTrue(*FString::Printf(TEXT("%s: the refusal says '%s' (it says: %s)"), Name, Expect, *Error), Error.Contains(Expect));
		}
	};
	// The baseline reads: the mutations below are the only reason a document does not.
	for (const TCHAR* Id : { TEXT("plane"), TEXT("jet"), TEXT("helicopter"), TEXT("hovercraft"), TEXT("motorcycle"), TEXT("sedan") })
	{
		FMantleVehicleDesc D;
		FString Error;
		TestTrue(*FString::Printf(TEXT("%s reads unchanged (%s)"), Id, *Error), ParseDesc(Mutated(*this, Id, [](FJsonObject&) {}), D, Error));
	}

	// The type and its block.
	Refuses(TEXT("a plane with no flight block"), TEXT("plane"), TEXT("needs a flight block"), [](FJsonObject& Doc) { Doc.RemoveField(TEXT("flight")); });
	Refuses(TEXT("a helicopter with no helicopter block"), TEXT("helicopter"), TEXT("needs a helicopter block"), [](FJsonObject& Doc) { Doc.RemoveField(TEXT("helicopter")); });
	Refuses(TEXT("a hovercraft with no hover block"), TEXT("hovercraft"), TEXT("needs a hover block"), [](FJsonObject& Doc) { Doc.RemoveField(TEXT("hover")); });
	Refuses(TEXT("an unknown movement type"), TEXT("plane"), TEXT("/movementType"), [](FJsonObject& Doc) { Doc.SetStringField(TEXT("movementType"), TEXT("boat")); });
	Refuses(TEXT("a car with a flight block"), TEXT("sedan"), TEXT("belongs to movementType"), [](FJsonObject& Doc)
	{
		Doc.SetObjectField(TEXT("flight"), MakeShared<FJsonObject>());
	});
	Refuses(TEXT("a plane with a hover block"), TEXT("plane"), TEXT("/hover"), [](FJsonObject& Doc) { Doc.SetObjectField(TEXT("hover"), MakeShared<FJsonObject>()); });
	Refuses(TEXT("a helicopter that says it is a plane"), TEXT("helicopter"), TEXT("needs a flight block"), [](FJsonObject& Doc) { Doc.SetStringField(TEXT("movementType"), TEXT("plane")); });

	// A plane's block.
	Refuses(TEXT("an unknown key in flight"), TEXT("plane"), TEXT("flight has bogus"), [](FJsonObject& Doc) { Child(Doc, TEXT("flight"))->SetNumberField(TEXT("bogus"), 1); });
	Refuses(TEXT("no air speed limit"), TEXT("plane"), TEXT("airSpeedLimit"), [](FJsonObject& Doc) { Child(Doc, TEXT("flight"))->RemoveField(TEXT("airSpeedLimit")); });
	Refuses(TEXT("an air speed limit out of range"), TEXT("plane"), TEXT("outside"), [](FJsonObject& Doc) { Child(Doc, TEXT("flight"))->SetNumberField(TEXT("airSpeedLimit"), 100); });
	Refuses(TEXT("a cruise throttle above 1"), TEXT("plane"), TEXT("outside"), [](FJsonObject& Doc) { Child(Doc, TEXT("flight"))->SetNumberField(TEXT("cruiseThrottle"), 1.5); });
	Refuses(TEXT("no aerofoils"), TEXT("plane"), TEXT("0 entries"), [](FJsonObject& Doc) { Child(Doc, TEXT("flight"))->SetArrayField(TEXT("aerofoils"), {}); });
	Refuses(TEXT("nine aerofoils"), TEXT("jet"), TEXT("9 entries"), [](FJsonObject& Doc)
	{
		TArray<TSharedPtr<FJsonValue>> Nine;
		for (int32 Index = 0; Index < 9; ++Index)
		{
			Nine.Add(MakeShared<FJsonValueObject>(Item(*Child(Doc, TEXT("flight")), TEXT("aerofoils"), 0)));
		}
		Child(Doc, TEXT("flight"))->SetArrayField(TEXT("aerofoils"), Nine);
	});
	Refuses(TEXT("an aerofoil of an unknown type"), TEXT("plane"), TEXT("must be one of fixed, wing, elevator, rudder"), [](FJsonObject& Doc)
	{
		Item(*Child(Doc, TEXT("flight")), TEXT("aerofoils"), 0)->SetStringField(TEXT("type"), TEXT("flap"));
	});
	Refuses(TEXT("an aerofoil of no area"), TEXT("plane"), TEXT("outside"), [](FJsonObject& Doc) { Item(*Child(Doc, TEXT("flight")), TEXT("aerofoils"), 0)->SetNumberField(TEXT("area"), 0); });
	Refuses(TEXT("an aerofoil without a location"), TEXT("plane"), TEXT("missing location"), [](FJsonObject& Doc)
	{
		Item(*Child(Doc, TEXT("flight")), TEXT("aerofoils"), 0)->RemoveField(TEXT("location"));
	});
	Refuses(TEXT("two aerofoils on one bone (ignoring case)"), TEXT("plane"), TEXT("each control surface has its own"), [](FJsonObject& Doc)
	{
		Item(*Child(Doc, TEXT("flight")), TEXT("aerofoils"), 1)->SetStringField(TEXT("bone"), TEXT("AILERON_L"));
	});
	Refuses(TEXT("an aerofoil on a spinning bone"), TEXT("plane"), TEXT("also a spinning bone"), [](FJsonObject& Doc)
	{
		Item(*Child(Doc, TEXT("flight")), TEXT("aerofoils"), 0)->SetStringField(TEXT("bone"), TEXT("propeller"));
	});
	Refuses(TEXT("a thruster that points nowhere"), TEXT("plane"), TEXT("points nowhere"), [](FJsonObject& Doc)
	{
		Item(*Child(Doc, TEXT("flight")), TEXT("thrusters"), 0)->SetField(TEXT("direction"), Triple(0, 0, 0));
	});
	Refuses(TEXT("a thruster direction out of range"), TEXT("plane"), TEXT("outside"), [](FJsonObject& Doc)
	{
		Item(*Child(Doc, TEXT("flight")), TEXT("thrusters"), 0)->SetField(TEXT("direction"), Triple(2, 0, 0));
	});
	Refuses(TEXT("five thrusters"), TEXT("plane"), TEXT("5 entries"), [](FJsonObject& Doc)
	{
		TArray<TSharedPtr<FJsonValue>> Five;
		for (int32 Index = 0; Index < 5; ++Index)
		{
			Five.Add(MakeShared<FJsonValueObject>(Item(*Child(Doc, TEXT("flight")), TEXT("thrusters"), 0)));
		}
		Child(Doc, TEXT("flight"))->SetArrayField(TEXT("thrusters"), Five);
	});
	Refuses(TEXT("a spin axis that points nowhere"), TEXT("plane"), TEXT("points nowhere"), [](FJsonObject& Doc)
	{
		Item(*Child(Doc, TEXT("flight")), TEXT("spinBones"), 0)->SetField(TEXT("axis"), Triple(0, 0, 0));
	});
	Refuses(TEXT("a spin bone twice"), TEXT("plane"), TEXT("each spinning bone has its own"), [](FJsonObject& Doc)
	{
		TArray<TSharedPtr<FJsonValue>> Twice = { MakeShared<FJsonValueObject>(Item(*Child(Doc, TEXT("flight")), TEXT("spinBones"), 0)),
			MakeShared<FJsonValueObject>(Item(*Child(Doc, TEXT("flight")), TEXT("spinBones"), 0)) };
		Child(Doc, TEXT("flight"))->SetArrayField(TEXT("spinBones"), Twice);
	});
	Refuses(TEXT("a spin speed out of range"), TEXT("plane"), TEXT("outside"), [](FJsonObject& Doc) { Item(*Child(Doc, TEXT("flight")), TEXT("spinBones"), 0)->SetNumberField(TEXT("rpm"), 50000); });
	Refuses(TEXT("a pilot's grip on a wheel's bone"), TEXT("plane"), TEXT("a wheel's bone"), [](FJsonObject& Doc)
	{
		Child(*Child(*Child(Doc, TEXT("flight")), TEXT("pilotGrips")), TEXT("left"))->SetStringField(TEXT("bone"), TEXT("wheel_f"));
	});
	Refuses(TEXT("one pilot's grip"), TEXT("plane"), TEXT("missing right"), [](FJsonObject& Doc) { Child(*Child(Doc, TEXT("flight")), TEXT("pilotGrips"))->RemoveField(TEXT("right")); });
	Refuses(TEXT("a boom out of range"), TEXT("plane"), TEXT("outside"), [](FJsonObject& Doc) { Child(Doc, TEXT("camera"))->SetField(TEXT("boomLocation"), Triple(2000, 0, 0)); });
	Refuses(TEXT("a boom of two numbers"), TEXT("plane"), TEXT("2 entries"), [](FJsonObject& Doc)
	{
		const TArray<TSharedPtr<FJsonValue>> Two = { MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueNumber>(0) };
		Child(Doc, TEXT("camera"))->SetArrayField(TEXT("boomLocation"), Two);
	});

	// A helicopter's.
	Refuses(TEXT("no rotors"), TEXT("helicopter"), TEXT("0 entries"), [](FJsonObject& Doc) { Child(Doc, TEXT("helicopter"))->SetArrayField(TEXT("rotors"), {}); });
	Refuses(TEXT("a rotor axis that points nowhere"), TEXT("helicopter"), TEXT("points nowhere"), [](FJsonObject& Doc)
	{
		Item(*Child(Doc, TEXT("helicopter")), TEXT("rotors"), 0)->SetField(TEXT("axis"), Triple(0, 0, 0));
	});
	Refuses(TEXT("a hover collective of 0"), TEXT("helicopter"), TEXT("outside"), [](FJsonObject& Doc) { Child(Doc, TEXT("helicopter"))->SetNumberField(TEXT("hoverCollective"), 0); });
	Refuses(TEXT("a rotor thrust out of range"), TEXT("helicopter"), TEXT("outside"), [](FJsonObject& Doc)
	{
		Item(*Child(Doc, TEXT("helicopter")), TEXT("rotors"), 0)->SetNumberField(TEXT("maxThrust"), 5000000);
	});
	Refuses(TEXT("a stability assist that is not a boolean"), TEXT("helicopter"), TEXT("must be true or false"), [](FJsonObject& Doc)
	{
		Item(*Child(Doc, TEXT("helicopter")), TEXT("rotors"), 0)->SetNumberField(TEXT("stabilityAssist"), 1);
	});
	Refuses(TEXT("a pilot's grip with no location"), TEXT("helicopter"), TEXT("missing location"), [](FJsonObject& Doc)
	{
		Child(*Child(*Child(Doc, TEXT("helicopter")), TEXT("pilotGrips")), TEXT("left"))->RemoveField(TEXT("location"));
	});

	// A hovercraft's.
	Refuses(TEXT("two pads"), TEXT("hovercraft"), TEXT("2 entries, not 3 to 8"), [](FJsonObject& Doc)
	{
		const TArray<TSharedPtr<FJsonValue>> Two = { MakeShared<FJsonValueObject>(Item(*Child(Doc, TEXT("hover")), TEXT("pads"), 0)),
			MakeShared<FJsonValueObject>(Item(*Child(Doc, TEXT("hover")), TEXT("pads"), 1)) };
		Child(Doc, TEXT("hover"))->SetArrayField(TEXT("pads"), Two);
	});
	Refuses(TEXT("a fan with no thrust"), TEXT("hovercraft"), TEXT("maxThrust"), [](FJsonObject& Doc) { Child(*Child(Doc, TEXT("hover")), TEXT("thruster"))->RemoveField(TEXT("maxThrust")); });
	Refuses(TEXT("a fan thrust out of range"), TEXT("hovercraft"), TEXT("outside"), [](FJsonObject& Doc)
	{
		Child(*Child(Doc, TEXT("hover")), TEXT("thruster"))->SetNumberField(TEXT("maxThrust"), 900000);
	});
	Refuses(TEXT("a fan that points nowhere"), TEXT("hovercraft"), TEXT("points nowhere"), [](FJsonObject& Doc)
	{
		Child(*Child(Doc, TEXT("hover")), TEXT("thruster"))->SetField(TEXT("direction"), Triple(0, 0, 0));
	});
	Refuses(TEXT("a vane angle of 60"), TEXT("hovercraft"), TEXT("outside"), [](FJsonObject& Doc)
	{
		Child(*Child(Doc, TEXT("hover")), TEXT("thruster"))->SetNumberField(TEXT("vectorAngle"), 60);
	});
	Refuses(TEXT("a rudder with no area"), TEXT("hovercraft"), TEXT("area"), [](FJsonObject& Doc) { Child(*Child(Doc, TEXT("hover")), TEXT("rudder"))->RemoveField(TEXT("area")); });
	Refuses(TEXT("an unknown damping key"), TEXT("hovercraft"), TEXT("damping has bogus"), [](FJsonObject& Doc)
	{
		Child(*Child(Doc, TEXT("hover")), TEXT("damping"))->SetNumberField(TEXT("bogus"), 1);
	});
	Refuses(TEXT("a pad spring damping above 1"), TEXT("hovercraft"), TEXT("outside"), [](FJsonObject& Doc)
	{
		Item(*Child(Doc, TEXT("hover")), TEXT("pads"), 0)->SetNumberField(TEXT("springDamping"), 3);
	});

	// The seats.
	Refuses(TEXT("an unknown seat kind"), TEXT("plane"), TEXT("must be one of car, motorcycle, pilot"), [](FJsonObject& Doc) { Child(Doc, TEXT("seat"))->SetStringField(TEXT("kind"), TEXT("boat")); });
	Refuses(TEXT("an unknown passenger seat kind"), TEXT("sedan"), TEXT("must be one of car, motorcycle, pilot"), [](FJsonObject& Doc)
	{
		Child(Doc, TEXT("passengerSeat"))->SetStringField(TEXT("kind"), TEXT("boat"));
	});
	Refuses(TEXT("an exit distance out of range"), TEXT("plane"), TEXT("outside"), [](FJsonObject& Doc) { Child(Doc, TEXT("seat"))->SetNumberField(TEXT("exitSideDistance"), 5000); });
	Refuses(TEXT("a rider on a car seat"), TEXT("motorcycle"), TEXT("set seat.kind to motorcycle"), [](FJsonObject& Doc) { Child(Doc, TEXT("seat"))->SetStringField(TEXT("kind"), TEXT("car")); });
	Refuses(TEXT("a rider on a pilot seat"), TEXT("motorcycle"), TEXT("set seat.kind to motorcycle"), [](FJsonObject& Doc) { Child(Doc, TEXT("seat"))->SetStringField(TEXT("kind"), TEXT("pilot")); });
	Refuses(TEXT("a rider with no fork"), TEXT("motorcycle"), TEXT("missing forkBone"), [](FJsonObject& Doc) { Child(*Child(Doc, TEXT("seat")), TEXT("rider"))->RemoveField(TEXT("forkBone")); });
	Refuses(TEXT("a fork that is a wheel"), TEXT("motorcycle"), TEXT("a wheel's bone"), [](FJsonObject& Doc)
	{
		Child(*Child(Doc, TEXT("seat")), TEXT("rider"))->SetStringField(TEXT("forkBone"), TEXT("wheel_f"));
	});
	Refuses(TEXT("a foot peg on a wheel"), TEXT("motorcycle"), TEXT("a wheel's bone"), [](FJsonObject& Doc)
	{
		Child(*Child(*Child(*Child(Doc, TEXT("seat")), TEXT("rider")), TEXT("footPegs")), TEXT("left"))->SetStringField(TEXT("bone"), TEXT("wheel_r"));
	});
	Refuses(TEXT("one handlebar grip"), TEXT("motorcycle"), TEXT("missing right"), [](FJsonObject& Doc)
	{
		Child(*Child(*Child(Doc, TEXT("seat")), TEXT("rider")), TEXT("handlebar"))->RemoveField(TEXT("right"));
	});
	Refuses(TEXT("a pelvis offset out of range"), TEXT("motorcycle"), TEXT("outside"), [](FJsonObject& Doc)
	{
		Child(*Child(Doc, TEXT("seat")), TEXT("rider"))->SetField(TEXT("pelvisOffset"), Triple(0, 0, 500));
	});
	Refuses(TEXT("a rear tyre friction of 0"), TEXT("motorcycle"), TEXT("outside"), [](FJsonObject& Doc)
	{
		Child(*Child(Doc, TEXT("seat")), TEXT("rider"))->SetNumberField(TEXT("rearTireFriction"), 0);
	});
	Refuses(TEXT("a roll angle of 120"), TEXT("motorcycle"), TEXT("outside"), [](FJsonObject& Doc)
	{
		Child(*Child(*Child(Doc, TEXT("seat")), TEXT("rider")), TEXT("rotationControl"))->SetNumberField(TEXT("rollMaxAngle"), 120);
	});
	Refuses(TEXT("an unknown rotation control key"), TEXT("motorcycle"), TEXT("rotationControl has bogus"), [](FJsonObject& Doc)
	{
		Child(*Child(*Child(Doc, TEXT("seat")), TEXT("rider")), TEXT("rotationControl"))->SetBoolField(TEXT("bogus"), true);
	});
	Refuses(TEXT("a rider on the passenger seat"), TEXT("sedan"), TEXT("does not have"), [](FJsonObject& Doc)
	{
		Child(Doc, TEXT("passengerSeat"))->SetObjectField(TEXT("rider"), MakeShared<FJsonObject>());
	});
	return true;
}

// ---- Bones --------------------------------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleDocumentBonesTest,
	"Mantle.Vehicle.Document.BlocksNameTheBodysBones",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleDocumentBonesTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleDocumentTestPrivate;
	FString Error;
	FMantleChassisSpec Plane;
	FMantleChassisSpec Bike;
	if (!LoadSpec(*this, TEXT("plane"), Plane) || !LoadSpec(*this, TEXT("motorcycle"), Bike))
	{
		return false;
	}
	FMantleVehicleAsset PlaneAsset;
	FMantleVehicleAsset BikeAsset;
	if (!TestTrue(*FString::Printf(TEXT("the plane makes an asset (%s)"), *Error), FMantleVehicleChassisBuilder::MakeAsset(Plane, PlaneAsset, Error))
		|| !TestTrue(*FString::Printf(TEXT("the bike makes an asset (%s)"), *Error), FMantleVehicleChassisBuilder::MakeAsset(Bike, BikeAsset, Error)))
	{
		return false;
	}
	// The builder's own body resolves: the pilot's grips (document: on the root) name the body's root bone.
	TestEqual(TEXT("the pilot's throttle grip is on the root"), PlaneAsset.Desc.Flight.PilotGrips.Left.Bone, FMantleVehicleChassisBuilder::RootBoneName());
	TestEqual(TEXT("the handlebar is on the fork"), BikeAsset.Desc.Rider.Handlebar.Left.Bone, FName(TEXT("fork_f")));

	// Resolving a changed Desc against the body: a bone is named as the body spells it, and the posed ones are free of the wheels and the root.
	auto Resolve = [&](const FMantleVehicleAsset& Asset, TFunctionRef<void(FMantleVehicleDesc&)> Change, FString& OutError)
	{
		FMantleVehicleDesc D = Asset.Desc;
		Change(D);
		return FMantleVehicleAssetReader::ResolveMovementBones(D, Asset.Body.Bones, OutError);
	};
	FString Why;
	TestTrue(*FString::Printf(TEXT("an aerofoil's bone in the wrong case is respelled (%s)"), *Why), Resolve(PlaneAsset, [](FMantleVehicleDesc& D)
	{
		D.Flight.Aerofoils[0].Bone = FName(TEXT("AILERON_L"));
	}, Why));
	{
		FMantleVehicleDesc D = PlaneAsset.Desc;
		D.Flight.Aerofoils[0].Bone = FName(TEXT("Elevator"));
		D.Flight.Aerofoils[2].Bone = FName(TEXT("ELEVATOR"));
		TestTrue(TEXT("respelled"), FMantleVehicleAssetReader::ResolveMovementBones(D, PlaneAsset.Body.Bones, Why));
		TestEqual(TEXT("as the body spells it"), D.Flight.Aerofoils[0].Bone.ToString(), FString(TEXT("elevator")));
	}
	TestFalse(TEXT("an aerofoil on a bone the body lacks"), Resolve(PlaneAsset, [](FMantleVehicleDesc& D) { D.Flight.Aerofoils[0].Bone = FName(TEXT("flap")); }, Why));
	TestTrue(TEXT("naming the bone"), Why.Contains(TEXT("flap")) && Why.Contains(TEXT("not in its body")));
	TestFalse(TEXT("an aerofoil on the root"), Resolve(PlaneAsset, [](FMantleVehicleDesc& D) { D.Flight.Aerofoils[0].Bone = FMantleVehicleChassisBuilder::RootBoneName(); }, Why));
	TestTrue(TEXT("the root carries the chassis"), Why.Contains(TEXT("root")));
	TestFalse(TEXT("an aerofoil on a wheel's bone"), Resolve(PlaneAsset, [](FMantleVehicleDesc& D) { D.Flight.Aerofoils[0].Bone = FName(TEXT("wheel_f")); }, Why));
	TestFalse(TEXT("a spin bone on a wheel's bone"), Resolve(PlaneAsset, [](FMantleVehicleDesc& D) { D.Flight.SpinBones[0].Bone = FName(TEXT("wheel_rl")); }, Why));
	TestTrue(TEXT("a thruster's bone is only a reference: the root will do"), Resolve(PlaneAsset, [](FMantleVehicleDesc& D)
	{
		D.Flight.Thrusters[0].Bone = FMantleVehicleChassisBuilder::RootBoneName();
	}, Why));
	TestFalse(TEXT("a thruster on a bone the body lacks"), Resolve(PlaneAsset, [](FMantleVehicleDesc& D) { D.Flight.Thrusters[0].Bone = FName(TEXT("nose")); }, Why));
	TestFalse(TEXT("a grip on a wheel's bone"), Resolve(PlaneAsset, [](FMantleVehicleDesc& D) { D.Flight.PilotGrips.Right.Bone = FName(TEXT("wheel_rr")); }, Why));
	TestTrue(TEXT("a grip left to the cook is the root's"), [&]
	{
		FMantleVehicleDesc D = PlaneAsset.Desc;
		D.Flight.PilotGrips.Left.Bone = NAME_None;
		return FMantleVehicleAssetReader::ResolveMovementBones(D, PlaneAsset.Body.Bones, Why) && D.Flight.PilotGrips.Left.Bone == FMantleVehicleChassisBuilder::RootBoneName();
	}());
	TestFalse(TEXT("a fork that is the root"), Resolve(BikeAsset, [](FMantleVehicleDesc& D) { D.Rider.ForkBone = FMantleVehicleChassisBuilder::RootBoneName(); }, Why));
	TestFalse(TEXT("a fork that is a wheel"), Resolve(BikeAsset, [](FMantleVehicleDesc& D) { D.Rider.ForkBone = FName(TEXT("wheel_f")); }, Why));
	TestTrue(TEXT("a fork bone is the wheel's parent, which is fine"), Resolve(BikeAsset, [](FMantleVehicleDesc& D) { D.Rider.ForkBone = FName(TEXT("FORK_F")); }, Why));
	return true;
}

// ---- The cook's own MVH1 -----------------------------------------------------------------------------------------------------------------------------

// The Go-to-C++ contract (#821): schema/examples/vehicle/cooked/*.mvh1 are the bytes the real cook (backend/internal/cook, ProcessAsset) writes for the
// valid-*.json examples; the Go side proves they are current (TestCookedVehicleFixturesAreCurrent). Reading them with the client's own entry point, whole,
// and finding every number the example gave, is the one test that a change on either side cannot slip past.
namespace MantleVehicleDocumentTestPrivate
{
	FString CookedFixtureDirectory()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("../schema/examples/vehicle")));
	}

	/** The example the cook was given, as JSON. */
	TSharedPtr<FJsonObject> LoadExampleJson(FAutomationTestBase& Test, const FString& Name)
	{
		FString Text;
		const FString Path = FPaths::Combine(CookedFixtureDirectory(), Name + TEXT(".json"));
		if (!Test.TestTrue(*FString::Printf(TEXT("%s: the example %s exists"), *Name, *Path), FFileHelper::LoadFileToString(Text, *Path)))
		{
			return nullptr;
		}
		TSharedPtr<FJsonObject> Json = ParseObject(Text);
		Test.TestTrue(*FString::Printf(TEXT("%s: the example is JSON"), *Name), Json.IsValid());
		return Json;
	}

	TSharedPtr<FJsonObject> ObjectAt(const TSharedPtr<FJsonObject>& Parent, const TCHAR* Key)
	{
		const TSharedPtr<FJsonObject>* Child = nullptr;
		return Parent.IsValid() && Parent->TryGetObjectField(Key, Child) && Child ? *Child : nullptr;
	}

	const TArray<TSharedPtr<FJsonValue>>& ArrayAt(const TSharedPtr<FJsonObject>& Parent, const TCHAR* Key)
	{
		static const TArray<TSharedPtr<FJsonValue>> Empty;
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		return Parent.IsValid() && Parent->TryGetArrayField(Key, Items) && Items ? *Items : Empty;
	}

	FVector VectorOf(const TSharedPtr<FJsonValue>& Value)
	{
		const TArray<TSharedPtr<FJsonValue>>* Parts = nullptr;
		if (Value.IsValid() && Value->TryGetArray(Parts) && Parts && Parts->Num() == 3)
		{
			return FVector((*Parts)[0]->AsNumber(), (*Parts)[1]->AsNumber(), (*Parts)[2]->AsNumber());
		}
		return FVector(TNumericLimits<double>::Max());
	}

	FVector VectorAt(const TSharedPtr<FJsonObject>& Parent, const TCHAR* Key)
	{
		return Parent.IsValid() ? VectorOf(Parent->TryGetField(Key)) : FVector(TNumericLimits<double>::Max());
	}

	/** Compares what the client read with what the example says, naming the key that differs. */
	struct FCookedCheck
	{
		FAutomationTestBase& Test;
		FString Label;

		void Num(const FString& Key, double Expected, double Actual) const
		{
			Test.TestTrue(*FString::Printf(TEXT("%s: %s is %g, the example says %g"), *Label, *Key, Actual, Expected), FMath::IsNearlyEqual(Actual, Expected, 1.0e-4));
		}

		void Number(const FString& Key, const TSharedPtr<FJsonObject>& Json, const TCHAR* JsonKey, double Actual, double Default) const
		{
			double Expected = Default;
			if (Json.IsValid())
			{
				Json->TryGetNumberField(JsonKey, Expected);
			}
			Num(Key, Expected, Actual);
		}

		void Vec(const FString& Key, const FVector& Expected, const FVector& Actual) const
		{
			Test.TestTrue(*FString::Printf(TEXT("%s: %s is %s, the example says %s"), *Label, *Key, *Actual.ToString(), *Expected.ToString()), Actual.Equals(Expected, 1.0e-4));
		}

		void Flag(const FString& Key, const TSharedPtr<FJsonObject>& Json, const TCHAR* JsonKey, bool bActual, bool bDefault) const
		{
			bool bExpected = bDefault;
			if (Json.IsValid())
			{
				Json->TryGetBoolField(JsonKey, bExpected);
			}
			Test.TestEqual(*FString::Printf(TEXT("%s: %s"), *Label, *Key), bActual, bExpected);
		}

		void Bone(const FString& Key, const FString& Expected, const FName& Actual) const
		{
			Test.TestTrue(*FString::Printf(TEXT("%s: %s is %s, the example says %s"), *Label, *Key, *Actual.ToString(), *Expected), Actual.ToString().Equals(Expected, ESearchCase::IgnoreCase));
		}
	};

	void CheckGripPair(const FCookedCheck& C, const FString& Key, const TSharedPtr<FJsonObject>& Pair, const FMantleVehicleGripPairDesc& Actual, const FName& DefaultBone)
	{
		const TCHAR* Sides[] = { TEXT("left"), TEXT("right") };
		const FMantleVehicleGripDesc* Grips[] = { &Actual.Left, &Actual.Right };
		for (int32 Side = 0; Side < 2; ++Side)
		{
			const TSharedPtr<FJsonObject> Grip = ObjectAt(Pair, Sides[Side]);
			const FString Path = FString::Printf(TEXT("%s/%s"), *Key, Sides[Side]);
			C.Vec(Path + TEXT("/location"), VectorAt(Grip, TEXT("location")), Grips[Side]->Location);
			FString Bone;
			if (Grip.IsValid() && Grip->TryGetStringField(TEXT("bone"), Bone))
			{
				C.Bone(Path + TEXT("/bone"), Bone, Grips[Side]->Bone);
			}
			else
			{
				// A grip the upload left without a bone: the cook names the root (the fork, for a handlebar), and the client sees it named.
				C.Bone(Path + TEXT("/bone (the cook names it)"), DefaultBone.ToString(), Grips[Side]->Bone);
			}
		}
	}

	void CheckSpinBones(const FCookedCheck& C, const FString& Key, const TArray<TSharedPtr<FJsonValue>>& Spins, const TArray<FMantleVehicleSpinBoneDesc>& Actual)
	{
		if (!C.Test.TestEqual(*FString::Printf(TEXT("%s: %s count"), *C.Label, *Key), Actual.Num(), Spins.Num()))
		{
			return;
		}
		for (int32 I = 0; I < Spins.Num(); ++I)
		{
			const TSharedPtr<FJsonObject> Spin = Spins[I]->AsObject();
			const FString At = FString::Printf(TEXT("%s/%d"), *Key, I);
			FString Bone;
			Spin->TryGetStringField(TEXT("bone"), Bone);
			C.Bone(At + TEXT("/bone"), Bone, Actual[I].Bone);
			C.Vec(At + TEXT("/axis"), VectorAt(Spin, TEXT("axis")), Actual[I].Axis);
			C.Number(At + TEXT("/rpm"), Spin, TEXT("rpm"), Actual[I].Rpm, 0.0);
		}
	}

	EMantleVehicleSeatKind SeatKindOfText(const FString& Text)
	{
		return Text == TEXT("pilot") ? EMantleVehicleSeatKind::Pilot : Text == TEXT("motorcycle") ? EMantleVehicleSeatKind::Motorcycle : EMantleVehicleSeatKind::Car;
	}

	EMantleVehicleAerofoilKind AerofoilKindOfText(const FString& Text)
	{
		return Text == TEXT("wing") ? EMantleVehicleAerofoilKind::Wing : Text == TEXT("elevator") ? EMantleVehicleAerofoilKind::Elevator
			: Text == TEXT("rudder") ? EMantleVehicleAerofoilKind::Rudder : EMantleVehicleAerofoilKind::Fixed;
	}

	/** Reads one cooked fixture whole, through the entry point the client uses for a vehicle off the wire. */
	bool ReadCookedFixture(FAutomationTestBase& Test, const FString& Name, TArray<uint8>& OutBytes, FMantleVehicleAsset& OutAsset)
	{
		const FString Path = FPaths::Combine(CookedFixtureDirectory(), TEXT("cooked"), Name + TEXT(".mvh1"));
		if (!Test.TestTrue(*FString::Printf(TEXT("%s: the cooked fixture %s exists (backend: TestCookedVehicleFixturesAreCurrent)"), *Name, *Path), FFileHelper::LoadFileToArray(OutBytes, *Path)))
		{
			return false;
		}
		FString Error;
		const bool bRead = FMantleVehicleAssetReader::Read(OutBytes, OutAsset, Error);
		return Test.TestTrue(*FString::Printf(TEXT("%s: FMantleVehicleAssetReader::Read takes the cook's bytes (%s)"), *Name, *Error), bRead);
	}

	/** What every movement type's MVH1 must carry whatever its block: the body's numbers, the wheels the reader needs, the seat, the camera. */
	void CheckCommonKeys(const FCookedCheck& C, const TSharedPtr<FJsonObject>& Json, const FMantleVehicleAsset& Asset, EMantleVehicleMovementKind Movement)
	{
		const FMantleVehicleDesc& D = Asset.Desc;
		C.Test.TestEqual(*FString::Printf(TEXT("%s: movement type"), *C.Label), static_cast<int32>(D.MovementType), static_cast<int32>(Movement));
		C.Test.TestTrue(*FString::Printf(TEXT("%s: the body came with the document (%d bones)"), *C.Label, Asset.Body.Bones.Num()), Asset.Body.Bones.Num() >= 3);
		C.Test.TestTrue(*FString::Printf(TEXT("%s: the cook measured the collision"), *C.Label), D.Collision.Num() > 0);
		C.Number(TEXT("mass"), Json, TEXT("mass"), D.Mass, 0.0);
		C.Number(TEXT("maxSpeed"), Json, TEXT("maxSpeed"), D.MaxSpeed, 0.0);
		C.Number(TEXT("dragCoefficient"), Json, TEXT("dragCoefficient"), D.DragCoefficient, 0.0);
		C.Number(TEXT("frontalArea"), Json, TEXT("frontalArea"), D.FrontalArea, 0.0);
		const TSharedPtr<FJsonObject> Camera = ObjectAt(Json, TEXT("camera"));
		C.Number(TEXT("camera/armLength"), Camera, TEXT("armLength"), D.CameraArmLength, 0.0);
		C.Number(TEXT("camera/height"), Camera, TEXT("height"), D.CameraHeight, 0.0);
		if (Camera.IsValid() && Camera->HasField(TEXT("boomLocation")))
		{
			C.Test.TestTrue(*FString::Printf(TEXT("%s: camera/boomLocation survived"), *C.Label), D.bCameraBoom);
			C.Vec(TEXT("camera/boomLocation"), VectorAt(Camera, TEXT("boomLocation")), D.CameraBoomLocation);
		}
		else
		{
			C.Test.TestFalse(*FString::Printf(TEXT("%s: the example has no camera/boomLocation, and the document made none up"), *C.Label), D.bCameraBoom);
		}

		const TSharedPtr<FJsonObject> Seat = ObjectAt(Json, TEXT("seat"));
		FString Kind;
		if (Seat.IsValid())
		{
			Seat->TryGetStringField(TEXT("kind"), Kind);
		}
		C.Test.TestEqual(*FString::Printf(TEXT("%s: seat/kind"), *C.Label), static_cast<int32>(D.SeatKind), static_cast<int32>(SeatKindOfText(Kind)));
		C.Vec(TEXT("seat/location"), VectorAt(Seat, TEXT("location")), D.SeatLocation);
		if (Seat.IsValid() && Seat->HasField(TEXT("exitSideDistance")))
		{
			C.Test.TestTrue(*FString::Printf(TEXT("%s: seat/exitSideDistance survived"), *C.Label), D.bSeatExitSideDistance);
			C.Number(TEXT("seat/exitSideDistance"), Seat, TEXT("exitSideDistance"), D.SeatExitSideDistance, 0.0);
		}

		// The wheels the reader needs, in the example's order, on the example's bones.
		const TArray<TSharedPtr<FJsonValue>>& Wheels = ArrayAt(Json, TEXT("wheels"));
		if (C.Test.TestEqual(*FString::Printf(TEXT("%s: wheel count"), *C.Label), D.Wheels.Num(), Wheels.Num()))
		{
			for (int32 I = 0; I < Wheels.Num(); ++I)
			{
				const TSharedPtr<FJsonObject> Wheel = Wheels[I]->AsObject();
				const FString At = FString::Printf(TEXT("wheels/%d"), I);
				FString Bone;
				Wheel->TryGetStringField(TEXT("bone"), Bone);
				C.Bone(At + TEXT("/bone"), Bone, D.Wheels[I].Bone);
				C.Flag(At + TEXT("/steering"), Wheel, TEXT("steering"), D.Wheels[I].bSteering, false);
				C.Flag(At + TEXT("/driven"), Wheel, TEXT("driven"), D.Wheels[I].bDriven, true);
				C.Flag(At + TEXT("/handbrake"), Wheel, TEXT("handbrake"), D.Wheels[I].bHandbrake, false);
			}
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleDocumentCookedFliersTest,
	"Mantle.Vehicle.Document.ReadsTheCooksMVH1.Fliers",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleDocumentCookedFliersTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleDocumentTestPrivate;
	if (!TestTrue(TEXT("the schema examples are beside this project"), FPaths::DirectoryExists(CookedFixtureDirectory())))
	{
		return false;
	}

	for (const TCHAR* Name : { TEXT("valid-plane"), TEXT("valid-jet") })
	{
		TArray<uint8> Bytes;
		FMantleVehicleAsset Asset;
		const TSharedPtr<FJsonObject> Json = LoadExampleJson(*this, Name);
		if (!Json.IsValid() || !ReadCookedFixture(*this, Name, Bytes, Asset))
		{
			continue;
		}
		const FCookedCheck C{ *this, Name };
		CheckCommonKeys(C, Json, Asset, EMantleVehicleMovementKind::Plane);
		const FMantleVehicleFlightDesc& F = Asset.Desc.Flight;
		const TSharedPtr<FJsonObject> Flight = ObjectAt(Json, TEXT("flight"));
		C.Number(TEXT("flight/airSpeedLimit"), Flight, TEXT("airSpeedLimit"), F.AirSpeedLimit, 0.0);
		C.Number(TEXT("flight/cruiseThrottle"), Flight, TEXT("cruiseThrottle"), F.CruiseThrottle, 0.4);
		C.Number(TEXT("flight/pitchAuthority"), Flight, TEXT("pitchAuthority"), F.PitchAuthority, 0.5);
		C.Number(TEXT("flight/throttleLeverRate"), Flight, TEXT("throttleLeverRate"), F.ThrottleLeverRate, 0.5);
		C.Number(TEXT("flight/linearDamping"), Flight, TEXT("linearDamping"), F.LinearDamping, 0.0);
		C.Number(TEXT("flight/angularDamping"), Flight, TEXT("angularDamping"), F.AngularDamping, 0.0);

		const TArray<TSharedPtr<FJsonValue>>& Aerofoils = ArrayAt(Flight, TEXT("aerofoils"));
		if (TestEqual(*FString::Printf(TEXT("%s: aerofoil count"), Name), F.Aerofoils.Num(), Aerofoils.Num()))
		{
			for (int32 I = 0; I < Aerofoils.Num(); ++I)
			{
				const TSharedPtr<FJsonObject> Surface = Aerofoils[I]->AsObject();
				const FMantleVehicleAerofoilDesc& A = F.Aerofoils[I];
				const FString At = FString::Printf(TEXT("flight/aerofoils/%d"), I);
				FString Bone;
				FString Type;
				Surface->TryGetStringField(TEXT("bone"), Bone);
				Surface->TryGetStringField(TEXT("type"), Type);
				C.Bone(At + TEXT("/bone"), Bone, A.Bone);
				C.Vec(At + TEXT("/location"), VectorAt(Surface, TEXT("location")), A.Location);
				TestEqual(*FString::Printf(TEXT("%s: %s/type"), Name, *At), static_cast<int32>(A.Type), static_cast<int32>(AerofoilKindOfText(Type)));
				C.Flag(At + TEXT("/mirror"), Surface, TEXT("mirror"), A.bMirror, false);
				C.Number(At + TEXT("/area"), Surface, TEXT("area"), A.AreaM2, 0.0);
				C.Number(At + TEXT("/camber"), Surface, TEXT("camber"), A.Camber, 5.0);
				C.Number(At + TEXT("/maxControlAngle"), Surface, TEXT("maxControlAngle"), A.MaxControlAngle, 12.0);
				C.Number(At + TEXT("/stallAngle"), Surface, TEXT("stallAngle"), A.StallAngle, 20.0);
			}
		}
		const TArray<TSharedPtr<FJsonValue>>& Thrusters = ArrayAt(Flight, TEXT("thrusters"));
		if (TestEqual(*FString::Printf(TEXT("%s: thruster count"), Name), F.Thrusters.Num(), Thrusters.Num()))
		{
			for (int32 I = 0; I < Thrusters.Num(); ++I)
			{
				const TSharedPtr<FJsonObject> Push = Thrusters[I]->AsObject();
				const FString At = FString::Printf(TEXT("flight/thrusters/%d"), I);
				FString Bone;
				if (Push->TryGetStringField(TEXT("bone"), Bone))
				{
					C.Bone(At + TEXT("/bone"), Bone, F.Thrusters[I].Bone);
				}
				C.Vec(At + TEXT("/location"), VectorAt(Push, TEXT("location")), F.Thrusters[I].Location);
				C.Vec(At + TEXT("/direction"), Push->HasField(TEXT("direction")) ? VectorAt(Push, TEXT("direction")) : FVector::ForwardVector, F.Thrusters[I].Direction);
				C.Number(At + TEXT("/maxThrust"), Push, TEXT("maxThrust"), F.Thrusters[I].MaxThrustNewtons, 0.0);
			}
		}
		CheckSpinBones(C, TEXT("flight/spinBones"), ArrayAt(Flight, TEXT("spinBones")), F.SpinBones);
		TestEqual(*FString::Printf(TEXT("%s: the pilot's grips"), Name), F.bPilotGrips, Flight->HasField(TEXT("pilotGrips")));
		if (F.bPilotGrips)
		{
			CheckGripPair(C, TEXT("flight/pilotGrips"), ObjectAt(Flight, TEXT("pilotGrips")), F.PilotGrips, Asset.Body.Bones[0].Name);
		}
		TestEqual(*FString::Printf(TEXT("%s: the document carries only its own block (no rotors, no pads)"), Name), Asset.Desc.Helicopter.Rotors.Num() + Asset.Desc.Hover.Pads.Num(), 0);
	}

	// The two examples differ where they should: a jet has no propeller, and a propeller plane spins one.
	{
		TArray<uint8> Bytes;
		FMantleVehicleAsset Plane;
		FMantleVehicleAsset Jet;
		if (ReadCookedFixture(*this, TEXT("valid-plane"), Bytes, Plane) && ReadCookedFixture(*this, TEXT("valid-jet"), Bytes, Jet))
		{
			TestEqual(TEXT("the plane spins its propeller"), Plane.Desc.Flight.SpinBones.Num(), 1);
			TestEqual(TEXT("the jet spins nothing"), Jet.Desc.Flight.SpinBones.Num(), 0);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleDocumentCookedHelicopterTest,
	"Mantle.Vehicle.Document.ReadsTheCooksMVH1.Helicopter",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleDocumentCookedHelicopterTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleDocumentTestPrivate;
	if (!TestTrue(TEXT("the schema examples are beside this project"), FPaths::DirectoryExists(CookedFixtureDirectory())))
	{
		return false;
	}
	const FString Name = TEXT("valid-helicopter");
	TArray<uint8> Bytes;
	FMantleVehicleAsset Asset;
	const TSharedPtr<FJsonObject> Json = LoadExampleJson(*this, Name);
	if (!Json.IsValid() || !ReadCookedFixture(*this, Name, Bytes, Asset))
	{
		return false;
	}
	const FCookedCheck C{ *this, Name };
	CheckCommonKeys(C, Json, Asset, EMantleVehicleMovementKind::Helicopter);
	const FMantleVehicleHelicopterDesc& H = Asset.Desc.Helicopter;
	const TSharedPtr<FJsonObject> Block = ObjectAt(Json, TEXT("helicopter"));
	C.Number(TEXT("helicopter/airSpeedLimit"), Block, TEXT("airSpeedLimit"), H.AirSpeedLimit, 0.0);
	C.Number(TEXT("helicopter/hoverCollective"), Block, TEXT("hoverCollective"), H.HoverCollective, 0.0);
	C.Number(TEXT("helicopter/throttleLeverRate"), Block, TEXT("throttleLeverRate"), H.ThrottleLeverRate, 1.0);
	C.Number(TEXT("helicopter/linearDamping"), Block, TEXT("linearDamping"), H.LinearDamping, 0.1);
	C.Number(TEXT("helicopter/angularDamping"), Block, TEXT("angularDamping"), H.AngularDamping, 0.0);
	const TArray<TSharedPtr<FJsonValue>>& Rotors = ArrayAt(Block, TEXT("rotors"));
	if (TestEqual(TEXT("rotor count"), H.Rotors.Num(), Rotors.Num()))
	{
		for (int32 I = 0; I < Rotors.Num(); ++I)
		{
			const TSharedPtr<FJsonObject> Rotor = Rotors[I]->AsObject();
			const FMantleVehicleRotorDesc& R = H.Rotors[I];
			const FString At = FString::Printf(TEXT("helicopter/rotors/%d"), I);
			FString Bone;
			if (Rotor->TryGetStringField(TEXT("bone"), Bone))
			{
				C.Bone(At + TEXT("/bone"), Bone, R.Bone);
			}
			C.Vec(At + TEXT("/location"), VectorAt(Rotor, TEXT("location")), R.Location);
			C.Vec(At + TEXT("/axis"), Rotor->HasField(TEXT("axis")) ? VectorAt(Rotor, TEXT("axis")) : FVector::UpVector, R.Axis);
			C.Number(At + TEXT("/maxThrust"), Rotor, TEXT("maxThrust"), R.MaxThrustNewtons, 0.0);
			C.Number(At + TEXT("/pitchAcceleration"), Rotor, TEXT("pitchAcceleration"), R.PitchAcceleration, 1.5);
			C.Number(At + TEXT("/rollAcceleration"), Rotor, TEXT("rollAcceleration"), R.RollAcceleration, 1.5);
			C.Number(At + TEXT("/yawAcceleration"), Rotor, TEXT("yawAcceleration"), R.YawAcceleration, 1.0);
			C.Flag(At + TEXT("/stabilityAssist"), Rotor, TEXT("stabilityAssist"), R.bStabilityAssist, true);
			C.Number(At + TEXT("/stabilityStiffness"), Rotor, TEXT("stabilityStiffness"), R.StabilityStiffness, 3.0);
			C.Number(At + TEXT("/stabilityDamping"), Rotor, TEXT("stabilityDamping"), R.StabilityDamping, 2.0);
			C.Number(At + TEXT("/stabilityMaxAcceleration"), Rotor, TEXT("stabilityMaxAcceleration"), R.StabilityMaxAcceleration, 4.0);
		}
	}
	CheckSpinBones(C, TEXT("helicopter/spinBones"), ArrayAt(Block, TEXT("spinBones")), H.SpinBones);
	TestEqual(TEXT("the pilot's grips"), H.bPilotGrips, Block->HasField(TEXT("pilotGrips")));
	if (H.bPilotGrips)
	{
		CheckGripPair(C, TEXT("helicopter/pilotGrips"), ObjectAt(Block, TEXT("pilotGrips")), H.PilotGrips, Asset.Body.Bones[0].Name);
	}
	TestEqual(TEXT("the document carries only its own block (no aerofoils, no pads)"), Asset.Desc.Flight.Aerofoils.Num() + Asset.Desc.Hover.Pads.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleDocumentCookedHoverTest,
	"Mantle.Vehicle.Document.ReadsTheCooksMVH1.Hovercraft",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleDocumentCookedHoverTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleDocumentTestPrivate;
	if (!TestTrue(TEXT("the schema examples are beside this project"), FPaths::DirectoryExists(CookedFixtureDirectory())))
	{
		return false;
	}
	const FString Name = TEXT("valid-hovercraft");
	TArray<uint8> Bytes;
	FMantleVehicleAsset Asset;
	const TSharedPtr<FJsonObject> Json = LoadExampleJson(*this, Name);
	if (!Json.IsValid() || !ReadCookedFixture(*this, Name, Bytes, Asset))
	{
		return false;
	}
	const FCookedCheck C{ *this, Name };
	CheckCommonKeys(C, Json, Asset, EMantleVehicleMovementKind::Hover);
	const FMantleVehicleHoverDesc& H = Asset.Desc.Hover;
	const TSharedPtr<FJsonObject> Block = ObjectAt(Json, TEXT("hover"));
	const TArray<TSharedPtr<FJsonValue>>& Pads = ArrayAt(Block, TEXT("pads"));
	if (TestEqual(TEXT("pad count"), H.Pads.Num(), Pads.Num()))
	{
		for (int32 I = 0; I < Pads.Num(); ++I)
		{
			const TSharedPtr<FJsonObject> Pad = Pads[I]->AsObject();
			const FString At = FString::Printf(TEXT("hover/pads/%d"), I);
			C.Vec(At + TEXT("/location"), VectorAt(Pad, TEXT("location")), H.Pads[I].Location);
			C.Number(At + TEXT("/maxRaise"), Pad, TEXT("maxRaise"), H.Pads[I].MaxRaise, 10.0);
			C.Number(At + TEXT("/maxDrop"), Pad, TEXT("maxDrop"), H.Pads[I].MaxDrop, 50.0);
			C.Number(At + TEXT("/springRate"), Pad, TEXT("springRate"), H.Pads[I].SpringRate, 37.0);
			C.Number(At + TEXT("/springDamping"), Pad, TEXT("springDamping"), H.Pads[I].SpringDamping, 0.6);
		}
	}
	const TSharedPtr<FJsonObject> Fan = ObjectAt(Block, TEXT("thruster"));
	C.Vec(TEXT("hover/thruster/location"), VectorAt(Fan, TEXT("location")), H.Thruster.Location);
	C.Vec(TEXT("hover/thruster/direction"), Fan->HasField(TEXT("direction")) ? VectorAt(Fan, TEXT("direction")) : FVector::ForwardVector, H.Thruster.Direction);
	C.Number(TEXT("hover/thruster/maxThrust"), Fan, TEXT("maxThrust"), H.Thruster.MaxThrustNewtons, 0.0);
	C.Number(TEXT("hover/thruster/reverseShare"), Fan, TEXT("reverseShare"), H.Thruster.ReverseShare, 0.5);
	C.Number(TEXT("hover/thruster/vectorAngle"), Fan, TEXT("vectorAngle"), H.Thruster.VectorAngleDegrees, 25.0);
	C.Number(TEXT("hover/thruster/topSpeed"), Fan, TEXT("topSpeed"), H.Thruster.TopSpeedCmPerSec, 2500.0);
	C.Number(TEXT("hover/thruster/brakeFade"), Fan, TEXT("brakeFade"), H.Thruster.BrakeFadeCmPerSec, 100.0);

	const TSharedPtr<FJsonObject> Rudder = ObjectAt(Block, TEXT("rudder"));
	if (TestEqual(TEXT("the rudder"), H.bRudder, Rudder.IsValid()) && Rudder.IsValid())
	{
		C.Vec(TEXT("hover/rudder/location"), VectorAt(Rudder, TEXT("location")), H.Rudder.Location);
		C.Number(TEXT("hover/rudder/area"), Rudder, TEXT("area"), H.Rudder.AreaM2, 0.0);
		C.Number(TEXT("hover/rudder/camber"), Rudder, TEXT("camber"), H.Rudder.Camber, 0.0);
		C.Number(TEXT("hover/rudder/maxControlAngle"), Rudder, TEXT("maxControlAngle"), H.Rudder.MaxControlAngle, 0.0);
		C.Number(TEXT("hover/rudder/stallAngle"), Rudder, TEXT("stallAngle"), H.Rudder.StallAngle, 0.0);
	}
	const TSharedPtr<FJsonObject> Damping = ObjectAt(Block, TEXT("damping"));
	C.Number(TEXT("hover/damping/lateralPerSecond"), Damping, TEXT("lateralPerSecond"), H.Damping.LateralPerSecond, 1.5);
	C.Number(TEXT("hover/damping/maxLateralAccel"), Damping, TEXT("maxLateralAccel"), H.Damping.MaxLateralAccel, 400.0);
	C.Number(TEXT("hover/damping/yawPerSecond"), Damping, TEXT("yawPerSecond"), H.Damping.YawPerSecond, 2.0);
	C.Number(TEXT("hover/damping/maxYawAccel"), Damping, TEXT("maxYawAccel"), H.Damping.MaxYawAccelDegrees, 180.0);
	C.Number(TEXT("hover/damping/skirtBrakePerSecond"), Damping, TEXT("skirtBrakePerSecond"), H.Damping.SkirtBrakePerSecond, 2.0);
	C.Number(TEXT("hover/damping/maxSkirtBrakeAccel"), Damping, TEXT("maxSkirtBrakeAccel"), H.Damping.MaxSkirtBrakeAccel, 500.0);
	C.Number(TEXT("hover/angularDamping"), Block, TEXT("angularDamping"), H.AngularDamping, 0.5);
	TestEqual(TEXT("the document carries only its own block (no aerofoils, no rotors)"), Asset.Desc.Flight.Aerofoils.Num() + Asset.Desc.Helicopter.Rotors.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleDocumentCookedGroundTest,
	"Mantle.Vehicle.Document.ReadsTheCooksMVH1.Ground",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleDocumentCookedGroundTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleDocumentTestPrivate;
	if (!TestTrue(TEXT("the schema examples are beside this project"), FPaths::DirectoryExists(CookedFixtureDirectory())))
	{
		return false;
	}

	// The motorcycle: the rider's whole fit, and a seat of kind motorcycle.
	{
		const FString Name = TEXT("valid-motorcycle");
		TArray<uint8> Bytes;
		FMantleVehicleAsset Asset;
		const TSharedPtr<FJsonObject> Json = LoadExampleJson(*this, Name);
		if (Json.IsValid() && ReadCookedFixture(*this, Name, Bytes, Asset))
		{
			const FCookedCheck C{ *this, Name };
			CheckCommonKeys(C, Json, Asset, EMantleVehicleMovementKind::Wheeled);
			const FMantleVehicleDesc& D = Asset.Desc;
			const TSharedPtr<FJsonObject> Rider = ObjectAt(ObjectAt(Json, TEXT("seat")), TEXT("rider"));
			if (TestTrue(TEXT("the rider survived the cook"), D.bRider) && Rider.IsValid())
			{
				const FMantleVehicleRiderDesc& R = D.Rider;
				FString Fork;
				Rider->TryGetStringField(TEXT("forkBone"), Fork);
				C.Bone(TEXT("rider/forkBone"), Fork, R.ForkBone);
				CheckGripPair(C, TEXT("rider/handlebar"), ObjectAt(Rider, TEXT("handlebar")), R.Handlebar, R.ForkBone);
				CheckGripPair(C, TEXT("rider/footPegs"), ObjectAt(Rider, TEXT("footPegs")), R.FootPegs, Asset.Body.Bones[0].Name);
				C.Vec(TEXT("rider/pelvisOffset"), VectorAt(Rider, TEXT("pelvisOffset")), R.PelvisOffset);
				C.Vec(TEXT("rider/stopPelvisShift"), VectorAt(Rider, TEXT("stopPelvisShift")), R.StopPelvisShift);
				C.Number(TEXT("rider/kneeSplayDegrees"), Rider, TEXT("kneeSplayDegrees"), R.KneeSplayDegrees, 8.0);
				TestTrue(TEXT("rider/rearTireFriction survived"), R.bRearTireFriction);
				C.Number(TEXT("rider/rearTireFriction"), Rider, TEXT("rearTireFriction"), R.RearTireFriction, 0.0);
				TestTrue(TEXT("rider/rearBrakeTorque survived"), R.bRearBrakeTorque);
				C.Number(TEXT("rider/rearBrakeTorque"), Rider, TEXT("rearBrakeTorque"), R.RearBrakeTorque, 0.0);
				const TSharedPtr<FJsonObject> Control = ObjectAt(Rider, TEXT("rotationControl"));
				const FMantleVehicleRotationControlDesc& RC = R.RotationControl;
				C.Flag(TEXT("rotationControl/torqueControl"), Control, TEXT("torqueControl"), RC.bTorqueControl, false);
				C.Number(TEXT("rotationControl/torqueRotationDamping"), Control, TEXT("torqueRotationDamping"), RC.TorqueRotationDamping, 0.0);
				C.Flag(TEXT("rotationControl/targetRotation"), Control, TEXT("targetRotation"), RC.bTargetRotation, false);
				C.Flag(TEXT("rotationControl/rollVsSpeed"), Control, TEXT("rollVsSpeed"), RC.bRollVsSpeed, false);
				C.Number(TEXT("rotationControl/rollControlScaling"), Control, TEXT("rollControlScaling"), RC.RollControlScaling, 0.0);
				C.Number(TEXT("rotationControl/rollMaxAngle"), Control, TEXT("rollMaxAngle"), RC.RollMaxAngle, 0.0);
				C.Number(TEXT("rotationControl/pitchControlScaling"), Control, TEXT("pitchControlScaling"), RC.PitchControlScaling, 0.0);
				C.Number(TEXT("rotationControl/pitchMaxAngle"), Control, TEXT("pitchMaxAngle"), RC.PitchMaxAngle, 0.0);
				C.Number(TEXT("rotationControl/rotationStiffness"), Control, TEXT("rotationStiffness"), RC.RotationStiffness, 0.0);
				C.Number(TEXT("rotationControl/rotationDamping"), Control, TEXT("rotationDamping"), RC.RotationDamping, 0.0);
				C.Number(TEXT("rotationControl/maxAccel"), Control, TEXT("maxAccel"), RC.MaxAccel, 0.0);
				C.Number(TEXT("rotationControl/autoCentreRollStrength"), Control, TEXT("autoCentreRollStrength"), RC.AutoCentreRollStrength, 0.0);
				C.Number(TEXT("rotationControl/autoCentrePitchStrength"), Control, TEXT("autoCentrePitchStrength"), RC.AutoCentrePitchStrength, 0.0);
				C.Number(TEXT("rotationControl/autoCentreYawStrength"), Control, TEXT("autoCentreYawStrength"), RC.AutoCentreYawStrength, 0.0);
			}
		}
	}

	// A car with its handling baked in: the steering's speed curve and the rear tyres' own grip, as flat numbers.
	{
		const FString Name = TEXT("valid-ground-handling");
		TArray<uint8> Bytes;
		FMantleVehicleAsset Asset;
		const TSharedPtr<FJsonObject> Json = LoadExampleJson(*this, Name);
		if (Json.IsValid() && ReadCookedFixture(*this, Name, Bytes, Asset))
		{
			const FCookedCheck C{ *this, Name };
			const FMantleVehicleDesc& D = Asset.Desc;
			TestEqual(TEXT("a plain car is wheeled"), static_cast<int32>(D.MovementType), static_cast<int32>(EMantleVehicleMovementKind::Wheeled));
			TestEqual(TEXT("a plain car's seat kind"), static_cast<int32>(D.SeatKind), static_cast<int32>(EMantleVehicleSeatKind::Car));
			TestFalse(TEXT("a plain car has no rider"), D.bRider);
			const TSharedPtr<FJsonObject> Steering = ObjectAt(Json, TEXT("steering"));
			C.Number(TEXT("steering/maxAngle"), Steering, TEXT("maxAngle"), D.MaxSteeringAngle, 0.0);
			C.Flag(TEXT("steering/ackermann"), Steering, TEXT("ackermann"), D.bAckermann, true);
			const TArray<TSharedPtr<FJsonValue>>& Curve = ArrayAt(Steering, TEXT("speedCurve"));
			if (TestTrue(TEXT("a speed curve in the example"), Curve.Num() > 0) && TestEqual(TEXT("speed curve points"), D.SteeringSpeedCurve.Num(), Curve.Num()))
			{
				for (int32 I = 0; I < Curve.Num(); ++I)
				{
					const TArray<TSharedPtr<FJsonValue>>* Point = nullptr;
					if (Curve[I]->TryGetArray(Point) && Point && Point->Num() == 2)
					{
						C.Num(FString::Printf(TEXT("steering/speedCurve/%d/mph"), I), (*Point)[0]->AsNumber(), D.SteeringSpeedCurve[I].X);
						C.Num(FString::Printf(TEXT("steering/speedCurve/%d/share"), I), (*Point)[1]->AsNumber(), D.SteeringSpeedCurve[I].Y);
					}
				}
			}
			const TSharedPtr<FJsonObject> Tires = ObjectAt(Json, TEXT("tires"));
			C.Number(TEXT("tires/friction"), Tires, TEXT("friction"), D.TireFriction, 0.0);
			TestTrue(TEXT("tires/rearFriction survived"), D.bTireRearFriction);
			C.Number(TEXT("tires/rearFriction"), Tires, TEXT("rearFriction"), D.TireRearFriction, 0.0);
			TestEqual(TEXT("wheel count"), D.Wheels.Num(), ArrayAt(Json, TEXT("wheels")).Num());
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleDocumentCookedRefusalsTest,
	"Mantle.Vehicle.Document.ReadsTheCooksMVH1.RefusesDamagedBytes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleDocumentCookedRefusalsTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleDocumentTestPrivate;
	if (!TestTrue(TEXT("the schema examples are beside this project"), FPaths::DirectoryExists(CookedFixtureDirectory())))
	{
		return false;
	}
	// The positive control for every test above: the same bytes read, and when damaged they do not, so "read" means the reader looked.
	TArray<uint8> Bytes;
	FMantleVehicleAsset Asset;
	if (!ReadCookedFixture(*this, TEXT("valid-helicopter"), Bytes, Asset))
	{
		return false;
	}
	FString Error;
	{
		TArray<uint8> Truncated(Bytes.GetData(), Bytes.Num() / 2);
		FMantleVehicleAsset Out;
		TestFalse(TEXT("half the file is refused"), FMantleVehicleAssetReader::Read(Truncated, Out, Error));
	}
	{
		TArray<uint8> WrongMagic = Bytes;
		WrongMagic[0] = 'X';
		FMantleVehicleAsset Out;
		TestFalse(TEXT("a file that is not MVH1 is refused"), FMantleVehicleAssetReader::Read(WrongMagic, Out, Error));
	}
	{
		TArray<uint8> Trailing = Bytes;
		Trailing.Add(0);
		FMantleVehicleAsset Out;
		TestFalse(TEXT("bytes after the body are refused"), FMantleVehicleAssetReader::Read(Trailing, Out, Error));
	}
	// A key the reader does not know is refused: the cook writing a key an older client cannot read is what the release gates guard.
	{
		const int32 DocumentLength = static_cast<int32>(Bytes[8]) | (static_cast<int32>(Bytes[9]) << 8) | (static_cast<int32>(Bytes[10]) << 16) | (static_cast<int32>(Bytes[11]) << 24);
		const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + 12), DocumentLength);
		FString Document(Converted.Length(), Converted.Get());
		const FString Marker = TEXT("\"helicopter\":{");
		const int32 Brace = Document.Find(Marker);
		if (TestTrue(TEXT("the cooked document has a helicopter block"), Brace != INDEX_NONE))
		{
			Document.InsertAt(Brace + Marker.Len(), TEXT("\"warpDrive\":1,"));
			const FTCHARToUTF8 Utf8(*Document);
			TArray<uint8> Forged;
			Forged.Append(Bytes.GetData(), 8);
			const int32 Length = Utf8.Length();
			for (int32 Shift = 0; Shift < 32; Shift += 8)
			{
				Forged.Add(static_cast<uint8>((Length >> Shift) & 0xFF));
			}
			Forged.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Length);
			Forged.Append(Bytes.GetData() + 12 + DocumentLength, Bytes.Num() - 12 - DocumentLength);
			FMantleVehicleAsset Out;
			TestFalse(TEXT("a document with a key the reader does not know is refused"), FMantleVehicleAssetReader::Read(Forged, Out, Error));
			TestTrue(*FString::Printf(TEXT("the refusal names the key (%s)"), *Error), Error.Contains(TEXT("warpDrive")));
		}
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
