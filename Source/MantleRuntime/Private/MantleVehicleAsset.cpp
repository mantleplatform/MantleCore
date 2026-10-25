// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleAsset.h"

#include "Animation/Skeleton.h"
#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/SkeletalMesh.h"
#include "MantleCookedCursor.h"
#include "MantleRuntime.h"
#include "MantleRuntimeSkeletalMeshBuilder.h"
#include "MantleSkinnedMeshReader.h"
#include "ReferenceSkeleton.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleVehicleAssetPrivate
{
	struct FVehicleRange
	{
		const TCHAR* Path;
		double Min;
		double Max;
	};

	/** limits.json placeAssets.vehicle.ranges, key for key. */
	const FVehicleRange GVehicleRanges[] = {
		{ TEXT("mass"), 50.0, 20000.0 },
		{ TEXT("maxSpeed"), 100.0, 12000.0 },
		{ TEXT("centerOfMass/*"), -1000.0, 1000.0 },
		{ TEXT("dragCoefficient"), 0.0, 2.0 },
		{ TEXT("frontalArea"), 0.0, 20.0 },
		{ TEXT("engine/maxTorque"), 10.0, 5000.0 },
		{ TEXT("engine/maxRPM"), 1000.0, 20000.0 },
		{ TEXT("engine/idleRPM"), 300.0, 5000.0 },
		{ TEXT("engine/torqueCurve/*/0"), 0.0, 20000.0 },
		{ TEXT("engine/torqueCurve/*/1"), 0.0, 1.0 },
		{ TEXT("transmission/gearRatios/*"), 0.1, 10.0 },
		{ TEXT("transmission/reverseRatio"), 0.1, 10.0 },
		{ TEXT("transmission/finalDrive"), 0.1, 10.0 },
		{ TEXT("transmission/changeUpRPM"), 500.0, 20000.0 },
		{ TEXT("transmission/changeDownRPM"), 300.0, 20000.0 },
		{ TEXT("suspension/maxRaise"), 0.0, 50.0 },
		{ TEXT("suspension/maxDrop"), 0.0, 50.0 },
		{ TEXT("suspension/springRate"), 1.0, 5000.0 },
		{ TEXT("suspension/springPreload"), 0.0, 5000.0 },
		{ TEXT("suspension/damping"), 0.0, 1.0 },
		{ TEXT("steering/maxAngle"), 5.0, 70.0 },
		{ TEXT("steering/speedCurve/*/0"), 0.0, 200.0 },
		{ TEXT("steering/speedCurve/*/1"), 0.1, 1.0 },
		{ TEXT("brakes/brakeTorque"), 0.0, 20000.0 },
		{ TEXT("brakes/handbrakeTorque"), 0.0, 20000.0 },
		{ TEXT("tires/friction"), 0.1, 5.0 },
		{ TEXT("tires/rearFriction"), 0.1, 10.0 },
		{ TEXT("wheels/*/radius"), 5.0, 150.0 },
		{ TEXT("wheels/*/width"), 2.0, 100.0 },
		{ TEXT("seat/location/*"), -1000.0, 1000.0 },
		{ TEXT("seat/weaponArc/yawMin"), -180.0, 180.0 },
		{ TEXT("seat/weaponArc/yawMax"), -180.0, 180.0 },
		{ TEXT("seat/weaponArc/pitchMin"), -90.0, 90.0 },
		{ TEXT("seat/weaponArc/pitchMax"), -90.0, 90.0 },
		// #546 the seat's steering wheel.
		{ TEXT("seat/steeringWheel/location/*"), -1000.0, 1000.0 },
		{ TEXT("seat/steeringWheel/rotation/*"), -180.0, 180.0 },
		{ TEXT("seat/steeringWheel/radius"), 10.0, 30.0 },
		// #546 the doors, the passenger seat and the trunk.
		{ TEXT("seat/door/pivot/*"), -1000.0, 1000.0 },
		{ TEXT("seat/door/axis/*"), -1.0, 1.0 },
		{ TEXT("seat/door/scale"), -2.0, 2.0 },
		{ TEXT("passengerSeat/location/*"), -1000.0, 1000.0 },
		{ TEXT("passengerSeat/weaponArc/yawMin"), -180.0, 180.0 },
		{ TEXT("passengerSeat/weaponArc/yawMax"), -180.0, 180.0 },
		{ TEXT("passengerSeat/weaponArc/pitchMin"), -90.0, 90.0 },
		{ TEXT("passengerSeat/weaponArc/pitchMax"), -90.0, 90.0 },
		{ TEXT("passengerSeat/door/pivot/*"), -1000.0, 1000.0 },
		{ TEXT("passengerSeat/door/axis/*"), -1.0, 1.0 },
		{ TEXT("passengerSeat/door/scale"), -2.0, 2.0 },
		{ TEXT("trunk/lid/pivot/*"), -1000.0, 1000.0 },
		{ TEXT("trunk/lid/axis/*"), -1.0, 1.0 },
		{ TEXT("trunk/lid/scale"), -2.0, 2.0 },
		{ TEXT("trunk/stand/*"), -1000.0, 1000.0 },
		{ TEXT("trunk/yaw"), -180.0, 180.0 },
		// #612 the seats' contact points.
		{ TEXT("seat/pelvis/location/*"), -1000.0, 1000.0 },
		{ TEXT("seat/pelvis/rotation/*"), -180.0, 180.0 },
		{ TEXT("seat/hands/left/location/*"), -1000.0, 1000.0 },
		{ TEXT("seat/hands/left/rotation/*"), -180.0, 180.0 },
		{ TEXT("seat/hands/right/location/*"), -1000.0, 1000.0 },
		{ TEXT("seat/hands/right/rotation/*"), -180.0, 180.0 },
		{ TEXT("seat/feet/left/location/*"), -1000.0, 1000.0 },
		{ TEXT("seat/feet/left/rotation/*"), -180.0, 180.0 },
		{ TEXT("seat/feet/right/location/*"), -1000.0, 1000.0 },
		{ TEXT("seat/feet/right/rotation/*"), -180.0, 180.0 },
		{ TEXT("seat/exits/*/location/*"), -1000.0, 1000.0 },
		{ TEXT("seat/exits/*/yaw"), -180.0, 180.0 },
		{ TEXT("passengerSeat/pelvis/location/*"), -1000.0, 1000.0 },
		{ TEXT("passengerSeat/pelvis/rotation/*"), -180.0, 180.0 },
		{ TEXT("passengerSeat/hands/left/location/*"), -1000.0, 1000.0 },
		{ TEXT("passengerSeat/hands/left/rotation/*"), -180.0, 180.0 },
		{ TEXT("passengerSeat/hands/right/location/*"), -1000.0, 1000.0 },
		{ TEXT("passengerSeat/hands/right/rotation/*"), -180.0, 180.0 },
		{ TEXT("passengerSeat/feet/left/location/*"), -1000.0, 1000.0 },
		{ TEXT("passengerSeat/feet/left/rotation/*"), -180.0, 180.0 },
		{ TEXT("passengerSeat/feet/right/location/*"), -1000.0, 1000.0 },
		{ TEXT("passengerSeat/feet/right/rotation/*"), -180.0, 180.0 },
		{ TEXT("passengerSeat/exits/*/location/*"), -1000.0, 1000.0 },
		{ TEXT("passengerSeat/exits/*/yaw"), -180.0, 180.0 },
		{ TEXT("camera/armLength"), 100.0, 3000.0 },
		{ TEXT("camera/height"), 0.0, 1000.0 },
		{ TEXT("collision/*/center/*"), -1000.0, 1000.0 },
		{ TEXT("collision/*/extent/*"), 1.0, 1000.0 },
		// #435 (f) paint and #488 liveries.
		{ TEXT("paint/color/*"), 0.0, 1.0 },
		{ TEXT("paint/metallic"), 0.0, 1.0 },
		{ TEXT("paint/roughness"), 0.0, 1.0 },
		{ TEXT("paint/flakeDensity"), 0.0, 1.0 },
		{ TEXT("paint/flakeSize"), 0.01, 1.0 },
		{ TEXT("paint/flakeTint/*"), 0.0, 1.0 },
		{ TEXT("paint/clearCoat"), 0.0, 1.0 },
		{ TEXT("paint/clearCoatRoughness"), 0.0, 1.0 },
		{ TEXT("liveryUV"), 0.0, 0.0 },
		{ TEXT("defaultLivery"), 0.0, 7.0 },
		// #435 (b) suspension linkage.
		{ TEXT("linkage/*/weight"), 0.0, 1.0 },
		// #435 item 2 attached parts.
		{ TEXT("parts/*/location/*"), -1000.0, 1000.0 },
		{ TEXT("parts/*/rotation/*"), -1.0, 1.0 },
		{ TEXT("parts/*/scale/*"), -100.0, 100.0 },
		{ TEXT("parts/*/paintSlots/*"), 0.0, 7.0 },
		// #821 seat kinds, the rider's fit and the chase camera's boom.
		{ TEXT("seat/exitSideDistance"), 0.0, 1000.0 },
		{ TEXT("seat/rider/handlebar/left/location/*"), -1000.0, 1000.0 },
		{ TEXT("seat/rider/handlebar/right/location/*"), -1000.0, 1000.0 },
		{ TEXT("seat/rider/footPegs/left/location/*"), -1000.0, 1000.0 },
		{ TEXT("seat/rider/footPegs/right/location/*"), -1000.0, 1000.0 },
		{ TEXT("seat/rider/pelvisOffset/*"), -100.0, 100.0 },
		{ TEXT("seat/rider/stopPelvisShift/*"), -100.0, 100.0 },
		{ TEXT("seat/rider/kneeSplayDegrees"), -30.0, 45.0 },
		{ TEXT("seat/rider/rearTireFriction"), 0.1, 20.0 },
		{ TEXT("seat/rider/rearBrakeTorque"), 0.0, 20000.0 },
		{ TEXT("seat/rider/rotationControl/torqueRotationDamping"), 0.0, 100.0 },
		{ TEXT("seat/rider/rotationControl/rollControlScaling"), 0.0, 10.0 },
		{ TEXT("seat/rider/rotationControl/rollMaxAngle"), 0.0, 90.0 },
		{ TEXT("seat/rider/rotationControl/pitchControlScaling"), 0.0, 10.0 },
		{ TEXT("seat/rider/rotationControl/pitchMaxAngle"), 0.0, 90.0 },
		{ TEXT("seat/rider/rotationControl/rotationStiffness"), 0.0, 1000.0 },
		{ TEXT("seat/rider/rotationControl/rotationDamping"), 0.0, 1000.0 },
		{ TEXT("seat/rider/rotationControl/maxAccel"), 0.0, 1000.0 },
		{ TEXT("seat/rider/rotationControl/autoCentreRollStrength"), 0.0, 10.0 },
		{ TEXT("seat/rider/rotationControl/autoCentrePitchStrength"), 0.0, 10.0 },
		{ TEXT("seat/rider/rotationControl/autoCentreYawStrength"), 0.0, 10.0 },
		{ TEXT("camera/boomLocation/*"), -1000.0, 1000.0 },
		// #821 a plane's flight block.
		{ TEXT("flight/airSpeedLimit"), 500.0, 60000.0 },
		{ TEXT("flight/cruiseThrottle"), 0.0, 1.0 },
		{ TEXT("flight/pitchAuthority"), 0.0, 1.0 },
		{ TEXT("flight/throttleLeverRate"), 0.05, 20.0 },
		{ TEXT("flight/linearDamping"), 0.0, 5.0 },
		{ TEXT("flight/angularDamping"), 0.0, 5.0 },
		{ TEXT("flight/aerofoils/*/location/*"), -1000.0, 1000.0 },
		{ TEXT("flight/aerofoils/*/area"), 0.01, 500.0 },
		{ TEXT("flight/aerofoils/*/camber"), -30.0, 30.0 },
		{ TEXT("flight/aerofoils/*/maxControlAngle"), 0.0, 60.0 },
		{ TEXT("flight/aerofoils/*/stallAngle"), 1.0, 90.0 },
		{ TEXT("flight/thrusters/*/location/*"), -1000.0, 1000.0 },
		{ TEXT("flight/thrusters/*/direction/*"), -1.0, 1.0 },
		{ TEXT("flight/thrusters/*/maxThrust"), 0.0, 2000000.0 },
		{ TEXT("flight/spinBones/*/axis/*"), -1.0, 1.0 },
		{ TEXT("flight/spinBones/*/rpm"), -10000.0, 10000.0 },
		{ TEXT("flight/pilotGrips/left/location/*"), -1000.0, 1000.0 },
		{ TEXT("flight/pilotGrips/right/location/*"), -1000.0, 1000.0 },
		// #821 a helicopter's block.
		{ TEXT("helicopter/airSpeedLimit"), 500.0, 60000.0 },
		{ TEXT("helicopter/hoverCollective"), 0.05, 1.0 },
		{ TEXT("helicopter/throttleLeverRate"), 0.05, 20.0 },
		{ TEXT("helicopter/linearDamping"), 0.0, 5.0 },
		{ TEXT("helicopter/angularDamping"), 0.0, 5.0 },
		{ TEXT("helicopter/rotors/*/location/*"), -1000.0, 1000.0 },
		{ TEXT("helicopter/rotors/*/axis/*"), -1.0, 1.0 },
		{ TEXT("helicopter/rotors/*/maxThrust"), 0.0, 2000000.0 },
		{ TEXT("helicopter/rotors/*/pitchAcceleration"), 0.0, 20.0 },
		{ TEXT("helicopter/rotors/*/rollAcceleration"), 0.0, 20.0 },
		{ TEXT("helicopter/rotors/*/yawAcceleration"), 0.0, 20.0 },
		{ TEXT("helicopter/rotors/*/stabilityStiffness"), 0.0, 50.0 },
		{ TEXT("helicopter/rotors/*/stabilityDamping"), 0.0, 50.0 },
		{ TEXT("helicopter/rotors/*/stabilityMaxAcceleration"), 0.0, 50.0 },
		{ TEXT("helicopter/spinBones/*/axis/*"), -1.0, 1.0 },
		{ TEXT("helicopter/spinBones/*/rpm"), -10000.0, 10000.0 },
		{ TEXT("helicopter/pilotGrips/left/location/*"), -1000.0, 1000.0 },
		{ TEXT("helicopter/pilotGrips/right/location/*"), -1000.0, 1000.0 },
		// #821 a hovercraft's block.
		{ TEXT("hover/pads/*/location/*"), -1000.0, 1000.0 },
		{ TEXT("hover/pads/*/maxRaise"), 0.0, 100.0 },
		{ TEXT("hover/pads/*/maxDrop"), 0.0, 200.0 },
		{ TEXT("hover/pads/*/springRate"), 0.0, 1000.0 },
		{ TEXT("hover/pads/*/springDamping"), 0.0, 1.0 },
		{ TEXT("hover/thruster/location/*"), -1000.0, 1000.0 },
		{ TEXT("hover/thruster/direction/*"), -1.0, 1.0 },
		{ TEXT("hover/thruster/maxThrust"), 0.0, 500000.0 },
		{ TEXT("hover/thruster/reverseShare"), 0.0, 1.0 },
		{ TEXT("hover/thruster/vectorAngle"), 0.0, 45.0 },
		{ TEXT("hover/thruster/topSpeed"), 0.0, 12000.0 },
		{ TEXT("hover/thruster/brakeFade"), 1.0, 1000.0 },
		{ TEXT("hover/rudder/location/*"), -1000.0, 1000.0 },
		{ TEXT("hover/rudder/area"), 0.01, 50.0 },
		{ TEXT("hover/rudder/camber"), -30.0, 30.0 },
		{ TEXT("hover/rudder/maxControlAngle"), 0.0, 60.0 },
		{ TEXT("hover/rudder/stallAngle"), 1.0, 90.0 },
		{ TEXT("hover/damping/lateralPerSecond"), 0.0, 50.0 },
		{ TEXT("hover/damping/maxLateralAccel"), 0.0, 5000.0 },
		{ TEXT("hover/damping/yawPerSecond"), 0.0, 50.0 },
		{ TEXT("hover/damping/maxYawAccel"), 0.0, 5000.0 },
		{ TEXT("hover/damping/skirtBrakePerSecond"), 0.0, 50.0 },
		{ TEXT("hover/damping/maxSkirtBrakeAccel"), 0.0, 5000.0 },
		{ TEXT("hover/angularDamping"), 0.0, 5.0 },
	};

	/** The longest wheel bone name the schema allows, in characters. */
	constexpr int32 MaxBoneNameLength = 64;

	/** The longest section name a materials key may be (the schema's propertyNames maxLength). */
	constexpr int32 MaxSectionNameLength = 256;

	bool IsUploadedAssetId(const FString& Id)
	{
		if (Id.Len() != 37 || !Id.StartsWith(TEXT("ast_u"), ESearchCase::CaseSensitive))
		{
			return false;
		}
		for (int32 Index = 5; Index < Id.Len(); ++Index)
		{
			const TCHAR Character = Id[Index];
			if (!((Character >= TEXT('0') && Character <= TEXT('9')) || (Character >= TEXT('a') && Character <= TEXT('f'))))
			{
				return false;
			}
		}
		return true;
	}

	/** Walks the resolved document; the first problem wins and names its JSON pointer. */
	class FVehicleDocParser
	{
	public:
		FString Error;

		bool Fail(const FString& Pointer, const FString& Why)
		{
			if (Error.IsEmpty())
			{
				Error = FString::Printf(TEXT("%s %s"), Pointer.IsEmpty() ? TEXT("the vehicle document") : *Pointer, *Why);
			}
			return false;
		}

		bool Ok() const { return Error.IsEmpty(); }

		/** Only the known keys, and every required one. */
		bool Keys(const TSharedPtr<FJsonObject>& Obj, const FString& Pointer, std::initializer_list<const TCHAR*> Known,
			std::initializer_list<const TCHAR*> Required)
		{
			for (const auto& Pair : Obj->Values)
			{
				const FString Key(Pair.Key.ToView());
				bool bKnown = false;
				for (const TCHAR* Name : Known)
				{
					bKnown = bKnown || Key.Equals(Name, ESearchCase::CaseSensitive);
				}
				if (!bKnown)
				{
					return Fail(Pointer, FString::Printf(TEXT("has %s, which a vehicle document does not have"), *Key));
				}
			}
			for (const TCHAR* Name : Required)
			{
				if (!Obj->HasField(Name))
				{
					return Fail(Pointer, FString::Printf(TEXT("is missing %s (a cooked vehicle carries every value)"), Name));
				}
			}
			return true;
		}

		/** A finite number (whole when bInteger), within RangePath's range when one is given. */
		bool NumberValue(const TSharedPtr<FJsonValue>& Value, const FString& Pointer, const TCHAR* RangePath, const bool bInteger, double& Out)
		{
			if (!Value.IsValid() || Value->Type != EJson::Number)
			{
				return Fail(Pointer, TEXT("must be a number"));
			}
			const double Number = Value->AsNumber();
			if (!FMath::IsFinite(Number))
			{
				return Fail(Pointer, TEXT("must be a finite number"));
			}
			if (bInteger && Number != FMath::RoundToDouble(Number))
			{
				return Fail(Pointer, TEXT("must be a whole number"));
			}
			if (RangePath)
			{
				double Min = 0.0;
				double Max = 0.0;
				if (!FMantleVehicleLimits::GetRange(RangePath, Min, Max))
				{
					return Fail(Pointer, FString::Printf(TEXT("has no range in this build (%s)"), RangePath));
				}
				if (Number < Min || Number > Max)
				{
					return Fail(Pointer, FString::Printf(TEXT("is %g, outside [%g, %g]"), Number, Min, Max));
				}
			}
			Out = Number;
			return true;
		}

		bool Number(const TSharedPtr<FJsonObject>& Obj, const FString& Pointer, const TCHAR* Key, const TCHAR* RangePath, double& Out,
			const bool bInteger = false)
		{
			return NumberValue(Obj->TryGetField(Key), Pointer + TEXT("/") + Key, RangePath, bInteger, Out);
		}

		/** A number the object may leave out: Out keeps its value (the contract's default) when the key is absent. */
		bool OptNumber(const TSharedPtr<FJsonObject>& Obj, const FString& Pointer, const TCHAR* Key, const TCHAR* RangePath, double& Out)
		{
			return !Obj->HasField(Key) || Number(Obj, Pointer, Key, RangePath, Out);
		}

		bool Int(const TSharedPtr<FJsonObject>& Obj, const FString& Pointer, const TCHAR* Key, const TCHAR* RangePath, int32& Out)
		{
			double Number = 0.0;
			if (!NumberValue(Obj->TryGetField(Key), Pointer + TEXT("/") + Key, RangePath, true, Number))
			{
				return false;
			}
			Out = static_cast<int32>(Number);
			return true;
		}

		bool Bool(const TSharedPtr<FJsonObject>& Obj, const FString& Pointer, const TCHAR* Key, bool& Out)
		{
			const TSharedPtr<FJsonValue> Value = Obj->TryGetField(Key);
			if (!Value.IsValid() || Value->Type != EJson::Boolean)
			{
				return Fail(Pointer + TEXT("/") + Key, TEXT("must be true or false"));
			}
			Out = Value->AsBool();
			return true;
		}

		bool Object(const TSharedPtr<FJsonObject>& Obj, const FString& Pointer, const TCHAR* Key, TSharedPtr<FJsonObject>& Out)
		{
			const TSharedPtr<FJsonValue> Value = Obj->TryGetField(Key);
			if (!Value.IsValid() || Value->Type != EJson::Object || !Value->AsObject().IsValid())
			{
				return Fail(Pointer + TEXT("/") + Key, TEXT("must be an object"));
			}
			Out = Value->AsObject();
			return true;
		}

		bool Array(const TSharedPtr<FJsonValue>& Value, const FString& Pointer, const int32 Min, const int32 Max,
			const TArray<TSharedPtr<FJsonValue>>*& Out)
		{
			if (!Value.IsValid() || Value->Type != EJson::Array)
			{
				return Fail(Pointer, TEXT("must be an array"));
			}
			Out = &Value->AsArray();
			if (Out->Num() < Min || Out->Num() > Max)
			{
				return Fail(Pointer, FString::Printf(TEXT("has %d entries, not %d to %d"), Out->Num(), Min, Max));
			}
			return true;
		}

		/** Three numbers, each within RangePath's range. */
		bool Vector(const TSharedPtr<FJsonValue>& Value, const FString& Pointer, const TCHAR* RangePath, FVector& Out)
		{
			const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
			if (!Array(Value, Pointer, 3, 3, Items))
			{
				return false;
			}
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				double Component = 0.0;
				if (!NumberValue((*Items)[Axis], FString::Printf(TEXT("%s/%d"), *Pointer, Axis), RangePath, false, Component))
				{
					return false;
				}
				Out[Axis] = Component;
			}
			return true;
		}
	};

	/** How far Point is from a steering wheel's rim (#612): the circle of Radius round Hub, square to the hub's X (its column). */
	double VehicleOffRim(const FVector& Point, const FVector& Hub, const FRotator& Turn, const double Radius)
	{
		const FVector Axis = FRotator(Turn.Pitch, Turn.Yaw, 0.0).Vector();
		const FVector Offset = Point - Hub;
		const double Along = FVector::DotProduct(Offset, Axis);
		const double Across = FMath::Sqrt(FMath::Max(0.0, Offset.SizeSquared() - Along * Along));
		return FMath::Sqrt(FMath::Square(Across - Radius) + FMath::Square(Along));
	}

	/**
	 * A seat's contact points (#612, the cook's contactRules): hands of kind Wheel are the driver's, on a seat with a steering
	 * wheel, in the body's space and within WheelHandTolerance of its rim; a pelvis, hand or foot in the body's space is within
	 * ContactReach of the seat's location; none rides a wheel's bone. Seat is the document's key ("seat").
	 */
	bool CheckVehicleSeatContacts(FVehicleDocParser& P, const FMantleVehicleDesc& D, const TCHAR* Seat, const FVector& SeatLocation,
		const FMantleVehicleSeatContactsDesc& C, const bool bDriver)
	{
		const FString Pointer = FString(TEXT("/")) + Seat;
		if (C.Hands == EMantleVehicleHandsKind::Wheel)
		{
			if (!bDriver)
			{
				return P.Fail(Pointer + TEXT("/hands/kind"), TEXT("is wheel, but a passenger has no steering wheel to hold"));
			}
			if (!D.bSteeringWheel)
			{
				return P.Fail(Pointer + TEXT("/hands/kind"), TEXT("is wheel, but the seat has no steering wheel to hold"));
			}
			const TPair<const TCHAR*, const FMantleVehicleContactDesc*> WheelHands[] = {
				{ TEXT("/hands/left"), &C.LeftHand }, { TEXT("/hands/right"), &C.RightHand } };
			for (const TPair<const TCHAR*, const FMantleVehicleContactDesc*>& Hand : WheelHands)
			{
				if (!Hand.Value->Bone.IsNone())
				{
					return P.Fail(Pointer + Hand.Key + TEXT("/bone"), TEXT("is on a bone, but a hand on the steering wheel is in the body's space"));
				}
				const double Off = VehicleOffRim(Hand.Value->Location, D.SteeringWheelLocation, D.SteeringWheelRotation, D.SteeringWheelRadius);
				if (Off > FMantleVehicleLimits::WheelHandTolerance)
				{
					return P.Fail(Pointer + Hand.Key + TEXT("/location"), FString::Printf(TEXT("is %.1f cm off the steering wheel's rim (at most %g)"),
						Off, FMantleVehicleLimits::WheelHandTolerance));
				}
			}
		}
		TArray<TPair<const TCHAR*, const FMantleVehicleContactDesc*>, TInlineAllocator<5>> Points;
		if (C.bPelvis)
		{
			Points.Add({ TEXT("/pelvis"), &C.Pelvis });
		}
		if (C.Hands != EMantleVehicleHandsKind::None)
		{
			Points.Add({ TEXT("/hands/left"), &C.LeftHand });
			Points.Add({ TEXT("/hands/right"), &C.RightHand });
		}
		if (C.bFeet)
		{
			Points.Add({ TEXT("/feet/left"), &C.LeftFoot });
			Points.Add({ TEXT("/feet/right"), &C.RightFoot });
		}
		for (const TPair<const TCHAR*, const FMantleVehicleContactDesc*>& Entry : Points)
		{
			const FMantleVehicleContactDesc& Point = *Entry.Value;
			if (!Point.Bone.IsNone())
			{
				// FName compares without regard to case, as the body's bone names do.
				for (const FMantleVehicleWheelDesc& Wheel : D.Wheels)
				{
					if (Wheel.Bone == Point.Bone)
					{
						return P.Fail(Pointer + Entry.Key + TEXT("/bone"), FString::Printf(TEXT("is %s, a wheel's bone, which spins"), *Point.Bone.ToString()));
					}
				}
				continue;
			}
			const double Reach = FVector::Dist(Point.Location, SeatLocation);
			if (Reach > FMantleVehicleLimits::ContactReach)
			{
				return P.Fail(Pointer + Entry.Key + TEXT("/location"), FString::Printf(TEXT("is %.0f cm from the seat's location (at most %g)"),
					Reach, FMantleVehicleLimits::ContactReach));
			}
		}
		return true;
	}

	/** The resolved document's rules across fields: the cook's (vehicle.go rules) and validate.py's. */
	bool CheckVehicleRules(FVehicleDocParser& P, const FMantleVehicleDesc& D)
	{
		if (D.IdleRPM >= D.MaxRPM)
		{
			return P.Fail(TEXT("/engine/idleRPM"), FString::Printf(TEXT("is %d, not below maxRPM %d"), D.IdleRPM, D.MaxRPM));
		}
		for (int32 Index = 1; Index < D.TorqueCurve.Num(); ++Index)
		{
			if (D.TorqueCurve[Index].X <= D.TorqueCurve[Index - 1].X || D.TorqueCurve[Index].X > D.MaxRPM)
			{
				return P.Fail(FString::Printf(TEXT("/engine/torqueCurve/%d/0"), Index), TEXT("does not rise, or passes maxRPM"));
			}
		}
		if (D.ChangeUpRPM > D.MaxRPM || D.ChangeDownRPM >= D.ChangeUpRPM)
		{
			return P.Fail(TEXT("/transmission"), FString::Printf(TEXT("shifts up at %d and down at %d with maxRPM %d"),
				D.ChangeUpRPM, D.ChangeDownRPM, D.MaxRPM));
		}
		int32 Driven = 0;
		int32 Steered = 0;
		for (int32 Index = 0; Index < D.Wheels.Num(); ++Index)
		{
			Driven += D.Wheels[Index].bDriven ? 1 : 0;
			Steered += D.Wheels[Index].bSteering ? 1 : 0;
			for (int32 Other = 0; Other < Index; ++Other)
			{
				// FName compares without regard to case, as the body's bone names do.
				if (D.Wheels[Other].Bone == D.Wheels[Index].Bone)
				{
					return P.Fail(FString::Printf(TEXT("/wheels/%d/bone"), Index), TEXT("is another wheel's bone"));
				}
			}
		}
		if (Driven == 0 || Steered == 0)
		{
			return P.Fail(TEXT("/wheels"), TEXT("needs at least one driven and one steering wheel"));
		}
		// #488: a livery lies on the paint, and the default is one of the list.
		if (D.Liveries.Num() > 0 && !D.HasPaint())
		{
			return P.Fail(TEXT("/liveries"), TEXT("has no paint sections to lie on"));
		}
		if (D.DefaultLivery != INDEX_NONE && !D.Liveries.IsValidIndex(D.DefaultLivery))
		{
			return P.Fail(TEXT("/defaultLivery"), FString::Printf(TEXT("is %d, not one of the %d liveries"), D.DefaultLivery, D.Liveries.Num()));
		}
		// #435 item 2: a part's painted slots wear the vehicle's paint, and the paint paints something (a section or a part's slot).
		for (int32 Index = 0; Index < D.Parts.Num(); ++Index)
		{
			if (D.Parts[Index].PaintSlots.Num() > 0 && !D.bPaint)
			{
				return P.Fail(FString::Printf(TEXT("/parts/%d/paintSlots"), Index), TEXT("paints with no paint to wear"));
			}
		}
		if (D.bPaint && D.PaintSections.IsEmpty() && !D.HasPartPaint())
		{
			return P.Fail(TEXT("/paint"), TEXT("paints nothing (no body section and no part's slot)"));
		}
		// #546: no two of the parts the car clips turn share a bone (FName compares without regard to case, as the body's do).
		struct FTurned
		{
			const TCHAR* Pointer;
			FName Bone;
		};
		const FTurned Turned[] = {
			{ TEXT("/seat/steeringWheel/bone"), D.SteeringWheelBone },
			{ TEXT("/seat/door/bone"), D.SeatDoor.Bone },
			{ TEXT("/passengerSeat/door/bone"), D.PassengerSeatDoor.Bone },
			{ TEXT("/trunk/lid/bone"), D.TrunkLid.Bone },
		};
		for (int32 Index = 0; Index < static_cast<int32>(UE_ARRAY_COUNT(Turned)); ++Index)
		{
			for (int32 Before = 0; Before < Index && !Turned[Index].Bone.IsNone(); ++Before)
			{
				if (Turned[Before].Bone == Turned[Index].Bone)
				{
					return P.Fail(Turned[Index].Pointer, FString::Printf(TEXT("is %s, the bone at %s too"), *Turned[Index].Bone.ToString(), Turned[Before].Pointer));
				}
			}
		}
		// #612: each seat's contact points.
		if (!CheckVehicleSeatContacts(P, D, TEXT("seat"), D.SeatLocation, D.SeatContacts, true))
		{
			return false;
		}
		return !D.bPassengerSeat || CheckVehicleSeatContacts(P, D, TEXT("passengerSeat"), D.PassengerSeatLocation, D.PassengerSeatContacts, false);
	}

	/** Three numbers within RangePath's range, as a linear colour. */
	bool ReadVehicleColor(FVehicleDocParser& P, const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, const TCHAR* RangePath, FLinearColor& Out)
	{
		FVector Value;
		if (!P.Vector(Obj->TryGetField(Key), FString(TEXT("/paint/")) + Key, RangePath, Value))
		{
			return false;
		}
		Out = FLinearColor(static_cast<float>(Value.X), static_cast<float>(Value.Y), static_cast<float>(Value.Z), 1.f);
		return true;
	}

	/** The resolved document's paint (#435 f): every key the cook fills, the sections each once. */
	void ReadVehiclePaint(FVehicleDocParser& P, const TSharedPtr<FJsonObject>& Root, FMantleVehicleDesc& D)
	{
		TSharedPtr<FJsonObject> Paint;
		if (!P.Object(Root, TEXT(""), TEXT("paint"), Paint)
			|| !P.Keys(Paint, TEXT("/paint"),
				{ TEXT("sections"), TEXT("color"), TEXT("metallic"), TEXT("roughness"), TEXT("flakeDensity"), TEXT("flakeSize"),
				  TEXT("flakeTint"), TEXT("clearCoat"), TEXT("clearCoatRoughness") },
				{ TEXT("sections"), TEXT("color"), TEXT("metallic"), TEXT("roughness"), TEXT("flakeDensity"), TEXT("flakeSize"),
				  TEXT("flakeTint"), TEXT("clearCoat"), TEXT("clearCoatRoughness") }))
		{
			return;
		}
		D.bPaint = true;
		// Empty when only parts' slots are painted (#435 item 2: the OffroadCar's shell is a part).
		const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
		if (P.Array(Paint->TryGetField(TEXT("sections")), TEXT("/paint/sections"), 0, FMantleVehicleLimits::MaxSections, Sections))
		{
			for (int32 Index = 0; Index < Sections->Num() && P.Ok(); ++Index)
			{
				FString Section;
				const TSharedPtr<FJsonValue>& Value = (*Sections)[Index];
				if (!Value.IsValid() || !Value->TryGetString(Section) || Section.IsEmpty() || Section.Len() > MaxSectionNameLength
					|| D.PaintSections.ContainsByPredicate([&Section](const FString& Seen) { return Seen.Equals(Section, ESearchCase::CaseSensitive); }))
				{
					P.Fail(FString::Printf(TEXT("/paint/sections/%d"), Index), TEXT("must name a body section, once"));
					break;
				}
				D.PaintSections.Add(Section);
			}
		}
		ReadVehicleColor(P, Paint, TEXT("color"), TEXT("paint/color/*"), D.PaintColor);
		ReadVehicleColor(P, Paint, TEXT("flakeTint"), TEXT("paint/flakeTint/*"), D.FlakeTint);
		P.Number(Paint, TEXT("/paint"), TEXT("metallic"), TEXT("paint/metallic"), D.PaintMetallic);
		P.Number(Paint, TEXT("/paint"), TEXT("roughness"), TEXT("paint/roughness"), D.PaintRoughness);
		P.Number(Paint, TEXT("/paint"), TEXT("flakeDensity"), TEXT("paint/flakeDensity"), D.FlakeDensity);
		P.Number(Paint, TEXT("/paint"), TEXT("flakeSize"), TEXT("paint/flakeSize"), D.FlakeSize);
		P.Number(Paint, TEXT("/paint"), TEXT("clearCoat"), TEXT("paint/clearCoat"), D.ClearCoat);
		P.Number(Paint, TEXT("/paint"), TEXT("clearCoatRoughness"), TEXT("paint/clearCoatRoughness"), D.ClearCoatRoughness);
	}

	/** The resolved document's liveries (#488): uploaded texture ids, each once, with liveryUV (the cook fills it). */
	void ReadVehicleLiveries(FVehicleDocParser& P, const TSharedPtr<FJsonObject>& Root, FMantleVehicleDesc& D)
	{
		const TArray<TSharedPtr<FJsonValue>>* Liveries = nullptr;
		if (!P.Array(Root->TryGetField(TEXT("liveries")), TEXT("/liveries"), 1, FMantleVehicleLimits::MaxLiveries, Liveries))
		{
			return;
		}
		for (int32 Index = 0; Index < Liveries->Num() && P.Ok(); ++Index)
		{
			FString Id;
			const TSharedPtr<FJsonValue>& Value = (*Liveries)[Index];
			if (!Value.IsValid() || !Value->TryGetString(Id) || !IsUploadedAssetId(Id)
				|| D.Liveries.ContainsByPredicate([&Id](const FString& Seen) { return Seen.Equals(Id, ESearchCase::CaseSensitive); }))
			{
				P.Fail(FString::Printf(TEXT("/liveries/%d"), Index), TEXT("must be an uploaded texture's id, once"));
				break;
			}
			D.Liveries.Add(Id);
		}
		if (P.Ok() && !Root->HasField(TEXT("liveryUV")))
		{
			P.Fail(TEXT(""), TEXT("is missing liveryUV (a cooked vehicle with liveries carries it)"));
		}
	}

	/** A linkage rule's bone name: 1 to MaxBoneNameLength characters. */
	bool ReadLinkageBone(FVehicleDocParser& P, const TSharedPtr<FJsonObject>& Rule, const FString& Pointer, const TCHAR* Key, FName& Out)
	{
		FString Bone;
		if (!Rule->TryGetStringField(Key, Bone) || Bone.IsEmpty() || Bone.Len() > MaxBoneNameLength)
		{
			return P.Fail(Pointer + TEXT("/") + Key, FString::Printf(TEXT("must be a bone name of 1 to %d characters"), MaxBoneNameLength));
		}
		Out = FName(*Bone);
		return true;
	}

	/**
	 * The resolved document's linkage (#435 b): every rule with exactly its type's keys, the cook's resolved weight, a
	 * copy's rotation and translation; no rule moving a wheel bone or reading a bone it moves (the cook's linkageRules).
	 */
	void ReadVehicleLinkage(FVehicleDocParser& P, const TSharedPtr<FJsonObject>& Root, FMantleVehicleDesc& D)
	{
		const TArray<TSharedPtr<FJsonValue>>* Rules = nullptr;
		if (!P.Array(Root->TryGetField(TEXT("linkage")), TEXT("/linkage"), 1, FMantleVehicleLimits::MaxLinkageRules, Rules))
		{
			return;
		}
		for (int32 Index = 0; Index < Rules->Num() && P.Ok(); ++Index)
		{
			const FString Pointer = FString::Printf(TEXT("/linkage/%d"), Index);
			const TSharedPtr<FJsonValue>& Value = (*Rules)[Index];
			const TSharedPtr<FJsonObject> Rule = Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
			FString Type;
			if (!Rule.IsValid() || !Rule->TryGetStringField(TEXT("type"), Type))
			{
				P.Fail(Pointer, TEXT("must be a rule with a type"));
				break;
			}
			FMantleVehicleLinkageRule R;
			bool bKeys = false;
			if (Type.Equals(TEXT("aim"), ESearchCase::CaseSensitive))
			{
				R.Type = EMantleVehicleLinkageType::Aim;
				bKeys = P.Keys(Rule, Pointer, { TEXT("type"), TEXT("bone"), TEXT("target"), TEXT("up"), TEXT("weight") },
					{ TEXT("type"), TEXT("bone"), TEXT("target"), TEXT("weight") })
					&& ReadLinkageBone(P, Rule, Pointer, TEXT("target"), R.Target)
					&& (!Rule->HasField(TEXT("up")) || ReadLinkageBone(P, Rule, Pointer, TEXT("up"), R.Up));
			}
			else if (Type.Equals(TEXT("stretch"), ESearchCase::CaseSensitive))
			{
				R.Type = EMantleVehicleLinkageType::Stretch;
				bKeys = P.Keys(Rule, Pointer, { TEXT("type"), TEXT("bone"), TEXT("end"), TEXT("target"), TEXT("weight") },
					{ TEXT("type"), TEXT("bone"), TEXT("end"), TEXT("weight") })
					&& ReadLinkageBone(P, Rule, Pointer, TEXT("end"), R.End)
					&& (!Rule->HasField(TEXT("target")) || ReadLinkageBone(P, Rule, Pointer, TEXT("target"), R.Target));
			}
			else if (Type.Equals(TEXT("copy"), ESearchCase::CaseSensitive))
			{
				R.Type = EMantleVehicleLinkageType::Copy;
				bKeys = P.Keys(Rule, Pointer, { TEXT("type"), TEXT("bone"), TEXT("source"), TEXT("rotation"), TEXT("translation"), TEXT("axis"), TEXT("weight") },
					{ TEXT("type"), TEXT("bone"), TEXT("source"), TEXT("rotation"), TEXT("translation"), TEXT("weight") })
					&& ReadLinkageBone(P, Rule, Pointer, TEXT("source"), R.Source)
					&& P.Bool(Rule, Pointer, TEXT("rotation"), R.bRotation)
					&& P.Bool(Rule, Pointer, TEXT("translation"), R.bTranslation);
				FString Axis;
				if (bKeys && Rule->HasField(TEXT("axis")))
				{
					R.Axis = Rule->TryGetStringField(TEXT("axis"), Axis) && Axis.Len() == 1 ? FString(TEXT("xyz")).Find(Axis, ESearchCase::CaseSensitive) : INDEX_NONE;
					bKeys = R.Axis != INDEX_NONE || P.Fail(Pointer + TEXT("/axis"), TEXT("must be \"x\", \"y\" or \"z\""));
				}
				if (bKeys && !R.bRotation && !R.bTranslation)
				{
					bKeys = P.Fail(Pointer, TEXT("copies neither rotation nor translation"));
				}
			}
			else
			{
				P.Fail(Pointer + TEXT("/type"), FString::Printf(TEXT("is '%s'; this build reads aim, stretch and copy rules"), *Type));
				break;
			}
			if (!bKeys || !ReadLinkageBone(P, Rule, Pointer, TEXT("bone"), R.Bone)
				|| !P.Number(Rule, Pointer, TEXT("weight"), TEXT("linkage/*/weight"), R.Weight))
			{
				break;
			}
			// FName compares without regard to case, as the body's bone names do.
			for (const FName Read : { R.Target, R.Up, R.Source })
			{
				if (!Read.IsNone() && (Read == R.Bone || Read == R.End))
				{
					P.Fail(Pointer, FString::Printf(TEXT("reads %s, a bone it moves"), *Read.ToString()));
				}
			}
			for (const FMantleVehicleWheelDesc& Wheel : D.Wheels)
			{
				if (Wheel.Bone == R.Bone || Wheel.Bone == R.End)
				{
					P.Fail(Pointer, FString::Printf(TEXT("moves %s, a wheel's bone (the simulation moves the wheels)"), *Wheel.Bone.ToString()));
				}
			}
			D.Linkage.Add(R);
		}
	}

	/**
	 * The resolved seat's steering wheel (#546): exactly the wheel's keys, its hub's location and the rotation the cook filled
	 * ([pitch, yaw, roll] degrees), and its rim's radius, each within its range, and the bone that turns with it when it names
	 * one.
	 */
	void ReadVehicleSteeringWheel(FVehicleDocParser& P, const TSharedPtr<FJsonObject>& Seat, FMantleVehicleDesc& D)
	{
		const FString Pointer = TEXT("/seat/steeringWheel");
		TSharedPtr<FJsonObject> Wheel;
		FVector Rotation = FVector::ZeroVector;
		if (!P.Object(Seat, TEXT("/seat"), TEXT("steeringWheel"), Wheel)
			|| !P.Keys(Wheel, Pointer, { TEXT("location"), TEXT("rotation"), TEXT("radius"), TEXT("bone") }, { TEXT("location"), TEXT("rotation"), TEXT("radius") })
			|| !P.Vector(Wheel->TryGetField(TEXT("location")), Pointer + TEXT("/location"), TEXT("seat/steeringWheel/location/*"), D.SteeringWheelLocation)
			|| !P.Vector(Wheel->TryGetField(TEXT("rotation")), Pointer + TEXT("/rotation"), TEXT("seat/steeringWheel/rotation/*"), Rotation)
			|| !P.Number(Wheel, Pointer, TEXT("radius"), TEXT("seat/steeringWheel/radius"), D.SteeringWheelRadius))
		{
			return;
		}
		// The bone that turns with it: optional.
		if (Wheel->HasField(TEXT("bone")) && !ReadLinkageBone(P, Wheel, Pointer, TEXT("bone"), D.SteeringWheelBone))
		{
			return;
		}
		D.SteeringWheelRotation = FRotator(Rotation.X, Rotation.Y, Rotation.Z);
		D.bSteeringWheel = true;
	}

	/**
	 * A resolved hinge (#546: a door, the trunk lid) at Parent's Key: exactly the hinge's keys, its bone's name, a point on it
	 * and its direction (not near zero length) within RangePath's ranges ("seat/door"), and the scale the cook filled. Whether
	 * the bone is the body's is Read's to check, with the body.
	 */
	bool ReadVehicleHinge(FVehicleDocParser& P, const TSharedPtr<FJsonObject>& Parent, const FString& ParentPointer, const TCHAR* Key,
		const FString& RangePath, FMantleVehicleHingeDesc& Out)
	{
		const FString Pointer = ParentPointer + TEXT("/") + Key;
		const FString PivotRange = RangePath + TEXT("/pivot/*");
		const FString AxisRange = RangePath + TEXT("/axis/*");
		const FString ScaleRange = RangePath + TEXT("/scale");
		TSharedPtr<FJsonObject> Hinge;
		FMantleVehicleHingeDesc H;
		if (!P.Object(Parent, ParentPointer, Key, Hinge)
			|| !P.Keys(Hinge, Pointer, { TEXT("bone"), TEXT("pivot"), TEXT("axis"), TEXT("scale") }, { TEXT("bone"), TEXT("pivot"), TEXT("axis"), TEXT("scale") })
			|| !ReadLinkageBone(P, Hinge, Pointer, TEXT("bone"), H.Bone)
			|| !P.Vector(Hinge->TryGetField(TEXT("pivot")), Pointer + TEXT("/pivot"), *PivotRange, H.Pivot)
			|| !P.Vector(Hinge->TryGetField(TEXT("axis")), Pointer + TEXT("/axis"), *AxisRange, H.Axis)
			|| !P.Number(Hinge, Pointer, TEXT("scale"), *ScaleRange, H.Scale))
		{
			return false;
		}
		if (H.Axis.Size() < FMantleVehicleLimits::HingeMinAxis)
		{
			return P.Fail(Pointer + TEXT("/axis"), TEXT("has no direction to turn about (near zero length)"));
		}
		Out = H;
		return true;
	}

	/**
	 * A resolved contact point (#612) at Parent's Key: exactly a contact point's keys, its location and the rotation the cook
	 * filled ([pitch, yaw, roll] degrees) within RangePath's ranges ("seat/pelvis"), and its bone when it names one. Whether the
	 * bone is the body's is Read's to check, with the body.
	 */
	bool ReadVehicleContact(FVehicleDocParser& P, const TSharedPtr<FJsonObject>& Parent, const FString& ParentPointer, const TCHAR* Key,
		const FString& RangePath, FMantleVehicleContactDesc& Out)
	{
		const FString Pointer = ParentPointer + TEXT("/") + Key;
		const FString LocationRange = RangePath + TEXT("/location/*");
		const FString RotationRange = RangePath + TEXT("/rotation/*");
		TSharedPtr<FJsonObject> Point;
		FMantleVehicleContactDesc C;
		FVector Rotation = FVector::ZeroVector;
		if (!P.Object(Parent, ParentPointer, Key, Point)
			|| !P.Keys(Point, Pointer, { TEXT("location"), TEXT("rotation"), TEXT("bone") }, { TEXT("location"), TEXT("rotation") })
			|| !P.Vector(Point->TryGetField(TEXT("location")), Pointer + TEXT("/location"), *LocationRange, C.Location)
			|| !P.Vector(Point->TryGetField(TEXT("rotation")), Pointer + TEXT("/rotation"), *RotationRange, Rotation)
			|| (Point->HasField(TEXT("bone")) && !ReadLinkageBone(P, Point, Pointer, TEXT("bone"), C.Bone)))
		{
			return false;
		}
		C.Rotation = FRotator(Rotation.X, Rotation.Y, Rotation.Z);
		Out = C;
		return true;
	}

	/**
	 * A resolved seat's contact points (#612), each group where the seat has it: the pelvis, the hands (their kind, wheel or
	 * fixed, and both), the feet (both) and the exits (1 to MaxSeatExits, each with the yaw the cook filled). Seat is the seat's
	 * object, Pointer its JSON pointer ("/seat") and RangePath its ranges' prefix ("seat").
	 */
	bool ReadVehicleSeatContacts(FVehicleDocParser& P, const TSharedPtr<FJsonObject>& Seat, const FString& Pointer, const FString& RangePath,
		FMantleVehicleSeatContactsDesc& Out)
	{
		FMantleVehicleSeatContactsDesc C;
		if (Seat->HasField(TEXT("pelvis")))
		{
			if (!ReadVehicleContact(P, Seat, Pointer, TEXT("pelvis"), RangePath + TEXT("/pelvis"), C.Pelvis))
			{
				return false;
			}
			C.bPelvis = true;
		}
		if (Seat->HasField(TEXT("hands")))
		{
			const FString HandsPointer = Pointer + TEXT("/hands");
			TSharedPtr<FJsonObject> Hands;
			FString Kind;
			if (!P.Object(Seat, Pointer, TEXT("hands"), Hands)
				|| !P.Keys(Hands, HandsPointer, { TEXT("kind"), TEXT("left"), TEXT("right") }, { TEXT("kind"), TEXT("left"), TEXT("right") }))
			{
				return false;
			}
			const bool bKind = Hands->TryGetStringField(TEXT("kind"), Kind);
			if (bKind && Kind.Equals(TEXT("wheel"), ESearchCase::CaseSensitive))
			{
				C.Hands = EMantleVehicleHandsKind::Wheel;
			}
			else if (bKind && Kind.Equals(TEXT("fixed"), ESearchCase::CaseSensitive))
			{
				C.Hands = EMantleVehicleHandsKind::Fixed;
			}
			else
			{
				return P.Fail(HandsPointer + TEXT("/kind"), TEXT("must be \"wheel\" or \"fixed\" in this build"));
			}
			if (!ReadVehicleContact(P, Hands, HandsPointer, TEXT("left"), RangePath + TEXT("/hands/left"), C.LeftHand)
				|| !ReadVehicleContact(P, Hands, HandsPointer, TEXT("right"), RangePath + TEXT("/hands/right"), C.RightHand))
			{
				return false;
			}
		}
		if (Seat->HasField(TEXT("feet")))
		{
			const FString FeetPointer = Pointer + TEXT("/feet");
			TSharedPtr<FJsonObject> Feet;
			if (!P.Object(Seat, Pointer, TEXT("feet"), Feet)
				|| !P.Keys(Feet, FeetPointer, { TEXT("left"), TEXT("right") }, { TEXT("left"), TEXT("right") })
				|| !ReadVehicleContact(P, Feet, FeetPointer, TEXT("left"), RangePath + TEXT("/feet/left"), C.LeftFoot)
				|| !ReadVehicleContact(P, Feet, FeetPointer, TEXT("right"), RangePath + TEXT("/feet/right"), C.RightFoot))
			{
				return false;
			}
			C.bFeet = true;
		}
		if (Seat->HasField(TEXT("exits")))
		{
			const FString LocationRange = RangePath + TEXT("/exits/*/location/*");
			const FString YawRange = RangePath + TEXT("/exits/*/yaw");
			const TArray<TSharedPtr<FJsonValue>>* Exits = nullptr;
			if (!P.Array(Seat->TryGetField(TEXT("exits")), Pointer + TEXT("/exits"), 1, FMantleVehicleLimits::MaxSeatExits, Exits))
			{
				return false;
			}
			for (int32 Index = 0; Index < Exits->Num(); ++Index)
			{
				const FString ExitPointer = FString::Printf(TEXT("%s/exits/%d"), *Pointer, Index);
				const TSharedPtr<FJsonValue>& Value = (*Exits)[Index];
				const TSharedPtr<FJsonObject> Exit = Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
				if (!Exit.IsValid())
				{
					return P.Fail(ExitPointer, TEXT("must be an object"));
				}
				FMantleVehicleExitDesc E;
				if (!P.Keys(Exit, ExitPointer, { TEXT("location"), TEXT("yaw") }, { TEXT("location"), TEXT("yaw") })
					|| !P.Vector(Exit->TryGetField(TEXT("location")), ExitPointer + TEXT("/location"), *LocationRange, E.Location)
					|| !P.Number(Exit, ExitPointer, TEXT("yaw"), *YawRange, E.Yaw))
				{
					return false;
				}
				C.Exits.Add(E);
			}
		}
		Out = MoveTemp(C);
		return true;
	}

	/** One seat's optional gun aim arc (#615). Without one the seat's arc is worked out from where it sits; Out.bSet says which. */
	bool ReadVehicleWeaponArc(FVehicleDocParser& P, const TSharedPtr<FJsonObject>& Seat, const FString& Pointer, const FString& RangePath,
		FMantleVehicleWeaponArcDesc& Out)
	{
		if (!Seat->HasField(TEXT("weaponArc")))
		{
			return true;
		}
		TSharedPtr<FJsonObject> Arc;
		if (!P.Object(Seat, Pointer, TEXT("weaponArc"), Arc)
			|| !P.Keys(Arc, Pointer + TEXT("/weaponArc"),
				{ TEXT("yawMin"), TEXT("yawMax"), TEXT("pitchMin"), TEXT("pitchMax") },
				{ TEXT("yawMin"), TEXT("yawMax"), TEXT("pitchMin"), TEXT("pitchMax") })
			|| !P.Number(Arc, Pointer + TEXT("/weaponArc"), TEXT("yawMin"), *(RangePath + TEXT("/weaponArc/yawMin")), Out.YawMin)
			|| !P.Number(Arc, Pointer + TEXT("/weaponArc"), TEXT("yawMax"), *(RangePath + TEXT("/weaponArc/yawMax")), Out.YawMax)
			|| !P.Number(Arc, Pointer + TEXT("/weaponArc"), TEXT("pitchMin"), *(RangePath + TEXT("/weaponArc/pitchMin")), Out.PitchMin)
			|| !P.Number(Arc, Pointer + TEXT("/weaponArc"), TEXT("pitchMax"), *(RangePath + TEXT("/weaponArc/pitchMax")), Out.PitchMax))
		{
			return false;
		}
		if (Out.YawMin >= Out.YawMax)
		{
			return P.Fail(Pointer + TEXT("/weaponArc/yawMin"), TEXT("must be below yawMax"));
		}
		if (Out.PitchMin >= Out.PitchMax)
		{
			return P.Fail(Pointer + TEXT("/weaponArc/pitchMin"), TEXT("must be below pitchMax"));
		}
		Out.bSet = true;
		return true;
	}

	// Defined with the rest of #821's readers below.
	bool ReadVehicleSeatKind(FVehicleDocParser& P, const TSharedPtr<FJsonObject>& Seat, const FString& Pointer, EMantleVehicleSeatKind& Out);

	/** The resolved passenger seat (#546): exactly its keys, where the passenger sits, optionally the door beside it, and its kind (#821). */
	void ReadVehiclePassengerSeat(FVehicleDocParser& P, const TSharedPtr<FJsonObject>& Root, FMantleVehicleDesc& D)
	{
		const FString Pointer = TEXT("/passengerSeat");
		TSharedPtr<FJsonObject> Seat;
		if (!P.Object(Root, TEXT(""), TEXT("passengerSeat"), Seat)
			|| !P.Keys(Seat, Pointer, { TEXT("location"), TEXT("door"), TEXT("pelvis"), TEXT("hands"), TEXT("feet"), TEXT("exits"), TEXT("hidden"), TEXT("kind"), TEXT("weaponArc") },
				{ TEXT("location") })
			|| !P.Vector(Seat->TryGetField(TEXT("location")), Pointer + TEXT("/location"), TEXT("passengerSeat/location/*"), D.PassengerSeatLocation)
			|| !ReadVehicleWeaponArc(P, Seat, Pointer, TEXT("passengerSeat"), D.PassengerSeatWeaponArc)
			|| !ReadVehicleSeatKind(P, Seat, Pointer, D.PassengerSeatKind)
			|| (Seat->HasField(TEXT("hidden")) && !P.Bool(Seat, Pointer, TEXT("hidden"), D.bPassengerSeatHidden))
			|| (Seat->HasField(TEXT("door")) && !ReadVehicleHinge(P, Seat, Pointer, TEXT("door"), TEXT("passengerSeat/door"), D.PassengerSeatDoor))
			|| !ReadVehicleSeatContacts(P, Seat, Pointer, TEXT("passengerSeat"), D.PassengerSeatContacts))
		{
			return;
		}
		D.bPassengerSeat = true;
	}

	/** The resolved trunk (#546): exactly its keys, its lid, where to stand to open it and the yaw the cook filled. */
	void ReadVehicleTrunk(FVehicleDocParser& P, const TSharedPtr<FJsonObject>& Root, FMantleVehicleDesc& D)
	{
		const FString Pointer = TEXT("/trunk");
		TSharedPtr<FJsonObject> Trunk;
		if (!P.Object(Root, TEXT(""), TEXT("trunk"), Trunk)
			|| !P.Keys(Trunk, Pointer, { TEXT("lid"), TEXT("stand"), TEXT("yaw") }, { TEXT("lid"), TEXT("stand"), TEXT("yaw") })
			|| !ReadVehicleHinge(P, Trunk, Pointer, TEXT("lid"), TEXT("trunk/lid"), D.TrunkLid)
			|| !P.Vector(Trunk->TryGetField(TEXT("stand")), Pointer + TEXT("/stand"), TEXT("trunk/stand/*"), D.TrunkStand)
			|| !P.Number(Trunk, Pointer, TEXT("yaw"), TEXT("trunk/yaw"), D.TrunkYaw))
		{
			return;
		}
		D.bTrunk = true;
	}

	/** The resolved horn (#546): exactly its key, its sound an uploaded id (never Mantle's built-in horn, which is what a car without one plays). */
	void ReadVehicleHorn(FVehicleDocParser& P, const TSharedPtr<FJsonObject>& Root, FMantleVehicleDesc& D)
	{
		const FString Pointer = TEXT("/horn");
		TSharedPtr<FJsonObject> Horn;
		FString Sound;
		if (!P.Object(Root, TEXT(""), TEXT("horn"), Horn) || !P.Keys(Horn, Pointer, { TEXT("sound") }, { TEXT("sound") }))
		{
			return;
		}
		if (!Horn->TryGetStringField(TEXT("sound"), Sound) || !IsUploadedAssetId(Sound))
		{
			P.Fail(Pointer + TEXT("/sound"), TEXT("must be an uploaded sound's id"));
			return;
		}
		D.HornSound = Sound;
	}

	/**
	 * The resolved document's parts (#435 item 2): each with exactly the part's keys, its mesh an uploaded id, its transform
	 * the cook filled (a unit quaternion, no axis of its scale near 0), and its materials, when it lists any, uploaded ids or
	 * null. Whether its bone is the body's is Read's to check, with the body.
	 */
	void ReadVehicleParts(FVehicleDocParser& P, const TSharedPtr<FJsonObject>& Root, FMantleVehicleDesc& D)
	{
		const TArray<TSharedPtr<FJsonValue>>* Parts = nullptr;
		if (!P.Array(Root->TryGetField(TEXT("parts")), TEXT("/parts"), 1, FMantleVehicleLimits::MaxParts, Parts))
		{
			return;
		}
		for (int32 Index = 0; Index < Parts->Num() && P.Ok(); ++Index)
		{
			const FString Pointer = FString::Printf(TEXT("/parts/%d"), Index);
			const TSharedPtr<FJsonValue>& Value = (*Parts)[Index];
			const TSharedPtr<FJsonObject> Part = Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
			if (!Part.IsValid())
			{
				P.Fail(Pointer, TEXT("must be an object"));
				break;
			}
			if (!P.Keys(Part, Pointer, { TEXT("mesh"), TEXT("bone"), TEXT("location"), TEXT("rotation"), TEXT("scale"), TEXT("materials"), TEXT("paintSlots") },
				{ TEXT("mesh"), TEXT("bone"), TEXT("location"), TEXT("rotation"), TEXT("scale") }))
			{
				break;
			}
			FMantleVehiclePartDesc Desc;
			if (!Part->TryGetStringField(TEXT("mesh"), Desc.Mesh) || !IsUploadedAssetId(Desc.Mesh))
			{
				P.Fail(Pointer + TEXT("/mesh"), TEXT("must be an uploaded mesh's id"));
				break;
			}
			if (!ReadLinkageBone(P, Part, Pointer, TEXT("bone"), Desc.Bone))
			{
				break;
			}
			FVector Location = FVector::ZeroVector;
			FVector Scale = FVector::OneVector;
			const TArray<TSharedPtr<FJsonValue>>* Rotation = nullptr;
			if (!P.Vector(Part->TryGetField(TEXT("location")), Pointer + TEXT("/location"), TEXT("parts/*/location/*"), Location)
				|| !P.Vector(Part->TryGetField(TEXT("scale")), Pointer + TEXT("/scale"), TEXT("parts/*/scale/*"), Scale)
				|| !P.Array(Part->TryGetField(TEXT("rotation")), Pointer + TEXT("/rotation"), 4, 4, Rotation))
			{
				break;
			}
			double Quat[4] = { 0.0, 0.0, 0.0, 1.0 };
			for (int32 Axis = 0; Axis < 4 && P.Ok(); ++Axis)
			{
				P.NumberValue((*Rotation)[Axis], FString::Printf(TEXT("%s/rotation/%d"), *Pointer, Axis), TEXT("parts/*/rotation/*"), false, Quat[Axis]);
			}
			if (!P.Ok())
			{
				break;
			}
			const FQuat Turn(Quat[0], Quat[1], Quat[2], Quat[3]);
			if (FMath::Abs(Turn.Size() - 1.0) > FMantleVehicleLimits::PartRotationTolerance)
			{
				P.Fail(Pointer + TEXT("/rotation"), TEXT("is not a unit quaternion"));
				break;
			}
			if (Scale.GetAbsMin() < FMantleVehicleLimits::PartMinScale)
			{
				P.Fail(Pointer + TEXT("/scale"), TEXT("flattens the part (an axis near 0)"));
				break;
			}
			Desc.Transform = FTransform(Turn.GetNormalized(), Location, Scale);
			if (Part->HasField(TEXT("materials")))
			{
				const TArray<TSharedPtr<FJsonValue>>* Materials = nullptr;
				if (!P.Array(Part->TryGetField(TEXT("materials")), Pointer + TEXT("/materials"), 0, FMantleVehicleLimits::MaxPartMaterials, Materials))
				{
					break;
				}
				for (int32 Slot = 0; Slot < Materials->Num() && P.Ok(); ++Slot)
				{
					const TSharedPtr<FJsonValue>& Entry = (*Materials)[Slot];
					FString Id;
					if (!Entry.IsValid() || Entry->IsNull())
					{
						Desc.Materials.Add(FString());
					}
					else if (Entry->TryGetString(Id) && IsUploadedAssetId(Id))
					{
						Desc.Materials.Add(Id);
					}
					else
					{
						P.Fail(FString::Printf(TEXT("%s/materials/%d"), *Pointer, Slot), TEXT("must be an uploaded material's id or null"));
					}
				}
			}
			// The slots drawn with the vehicle's paint: each a slot index, once.
			if (P.Ok() && Part->HasField(TEXT("paintSlots")))
			{
				const TArray<TSharedPtr<FJsonValue>>* Painted = nullptr;
				if (!P.Array(Part->TryGetField(TEXT("paintSlots")), Pointer + TEXT("/paintSlots"), 1, FMantleVehicleLimits::MaxPartMaterials, Painted))
				{
					break;
				}
				for (int32 Entry = 0; Entry < Painted->Num() && P.Ok(); ++Entry)
				{
					double Slot = 0.0;
					const FString SlotPointer = FString::Printf(TEXT("%s/paintSlots/%d"), *Pointer, Entry);
					if (P.NumberValue((*Painted)[Entry], SlotPointer, TEXT("parts/*/paintSlots/*"), true, Slot))
					{
						if (Desc.PaintSlots.Contains(static_cast<int32>(Slot)))
						{
							P.Fail(SlotPointer, TEXT("names a slot twice"));
						}
						Desc.PaintSlots.Add(static_cast<int32>(Slot));
					}
				}
			}
			if (!P.Ok())
			{
				break;
			}
			D.Parts.Add(MoveTemp(Desc));
		}
	}

	// ---- the movement type, its blocks, the seat kinds and the rider -------------------------------------------------------------

	/**
	 * A grip (#821 $defs.grip) at Parent's Key: exactly its keys, its location within RangePath's ranges ("flight/pilotGrips/left") and the bone it
	 * moves with when it names one. Whether the bone is the body's is ResolveMovementBones'.
	 */
	bool ReadVehicleGrip(FVehicleDocParser& P, const TSharedPtr<FJsonObject>& Parent, const FString& ParentPointer, const TCHAR* Key,
		const FString& RangePath, FMantleVehicleGripDesc& Out)
	{
		const FString Pointer = ParentPointer + TEXT("/") + Key;
		const FString LocationRange = RangePath + TEXT("/location/*");
		TSharedPtr<FJsonObject> Grip;
		FMantleVehicleGripDesc G;
		if (!P.Object(Parent, ParentPointer, Key, Grip)
			|| !P.Keys(Grip, Pointer, { TEXT("bone"), TEXT("location") }, { TEXT("location") })
			|| !P.Vector(Grip->TryGetField(TEXT("location")), Pointer + TEXT("/location"), *LocationRange, G.Location)
			|| (Grip->HasField(TEXT("bone")) && !ReadLinkageBone(P, Grip, Pointer, TEXT("bone"), G.Bone)))
		{
			return false;
		}
		Out = G;
		return true;
	}

	/** A left grip and a right one (#821 $defs.gripPair), both required, within RangePath's ranges ("flight/pilotGrips"). */
	bool ReadVehicleGripPair(FVehicleDocParser& P, const TSharedPtr<FJsonObject>& Parent, const FString& ParentPointer, const TCHAR* Key,
		const FString& RangePath, FMantleVehicleGripPairDesc& Out)
	{
		const FString Pointer = ParentPointer + TEXT("/") + Key;
		TSharedPtr<FJsonObject> Pair;
		return P.Object(Parent, ParentPointer, Key, Pair)
			&& P.Keys(Pair, Pointer, { TEXT("left"), TEXT("right") }, { TEXT("left"), TEXT("right") })
			&& ReadVehicleGrip(P, Pair, Pointer, TEXT("left"), RangePath + TEXT("/left"), Out.Left)
			&& ReadVehicleGrip(P, Pair, Pointer, TEXT("right"), RangePath + TEXT("/right"), Out.Right);
	}

	/** A direction or an axis at Obj's Key: three numbers within RangePath's range (a slash and a star are added), pointing somewhere. A missing key keeps Out. */
	bool ReadVehicleDirection(FVehicleDocParser& P, const TSharedPtr<FJsonObject>& Obj, const FString& Pointer, const TCHAR* Key, const FString& RangePath,
		FVector& Out)
	{
		if (!Obj->HasField(Key))
		{
			return true;
		}
		FVector Direction = FVector::ZeroVector;
		if (!P.Vector(Obj->TryGetField(Key), Pointer + TEXT("/") + Key, *(RangePath + TEXT("/*")), Direction))
		{
			return false;
		}
		if (Direction.Size() < FMantleVehicleLimits::DirectionMinLength)
		{
			return P.Fail(Pointer + TEXT("/") + Key, FString::Printf(TEXT("has length %.3f, so it points nowhere (at least %g)"), Direction.Size(),
				FMantleVehicleLimits::DirectionMinLength));
		}
		Out = Direction;
		return true;
	}

	/** A bone name the object may leave out (a reference or a posed bone: the block's rules say which). */
	bool ReadVehicleOptionalBone(FVehicleDocParser& P, const TSharedPtr<FJsonObject>& Obj, const FString& Pointer, FName& Out)
	{
		return !Obj->HasField(TEXT("bone")) || ReadLinkageBone(P, Obj, Pointer, TEXT("bone"), Out);
	}

	/** A block's spin bones (#821 $defs.spinBone), 0 to MaxSpinBones, each a bone, an axis and a speed, no bone twice. Block is "flight" or "helicopter". */
	bool ReadVehicleSpinBones(FVehicleDocParser& P, const TSharedPtr<FJsonObject>& BlockObject, const TCHAR* Block, TArray<FMantleVehicleSpinBoneDesc>& Out)
	{
		if (!BlockObject->HasField(TEXT("spinBones")))
		{
			return true;
		}
		const FString Pointer = FString(TEXT("/")) + Block + TEXT("/spinBones");
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (!P.Array(BlockObject->TryGetField(TEXT("spinBones")), Pointer, 0, FMantleVehicleLimits::MaxSpinBones, Items))
		{
			return false;
		}
		for (int32 Index = 0; Index < Items->Num(); ++Index)
		{
			const FString ItemPointer = FString::Printf(TEXT("%s/%d"), *Pointer, Index);
			const TSharedPtr<FJsonValue>& Value = (*Items)[Index];
			const TSharedPtr<FJsonObject> Item = Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
			if (!Item.IsValid())
			{
				return P.Fail(ItemPointer, TEXT("must be an object"));
			}
			FMantleVehicleSpinBoneDesc Spin;
			if (!P.Keys(Item, ItemPointer, { TEXT("bone"), TEXT("axis"), TEXT("rpm") }, { TEXT("bone"), TEXT("axis"), TEXT("rpm") })
				|| !ReadLinkageBone(P, Item, ItemPointer, TEXT("bone"), Spin.Bone)
				|| !ReadVehicleDirection(P, Item, ItemPointer, TEXT("axis"), FString::Printf(TEXT("%s/spinBones/*/axis"), Block), Spin.Axis)
				|| !P.Number(Item, ItemPointer, TEXT("rpm"), *FString::Printf(TEXT("%s/spinBones/*/rpm"), Block), Spin.Rpm))
			{
				return false;
			}
			for (int32 Before = 0; Before < Out.Num(); ++Before)
			{
				// FName compares without regard to case, as the body's bone names do.
				if (Out[Before].Bone == Spin.Bone)
				{
					return P.Fail(ItemPointer + TEXT("/bone"), FString::Printf(TEXT("is %s, spinBones/%d's bone too; each spinning bone has its own"),
						*Spin.Bone.ToString(), Before));
				}
			}
			Out.Add(Spin);
		}
		return true;
	}

	/** The enum a string key names: Names[i] is the string of the i-th value. The key must be there, a string, and one of Names. */
	bool ReadVehicleEnumName(FVehicleDocParser& P, const TSharedPtr<FJsonObject>& Obj, const FString& Pointer, const TCHAR* Key,
		TConstArrayView<const TCHAR*> Names, int32& OutIndex)
	{
		FString Text;
		if (Obj->TryGetStringField(Key, Text))
		{
			for (int32 Index = 0; Index < Names.Num(); ++Index)
			{
				if (Text.Equals(Names[Index], ESearchCase::CaseSensitive))
				{
					OutIndex = Index;
					return true;
				}
			}
		}
		FString Choices;
		for (const TCHAR* Name : Names)
		{
			Choices += (Choices.IsEmpty() ? TEXT("") : TEXT(", ")) + FString(Name);
		}
		return P.Fail(Pointer + TEXT("/") + Key, FString::Printf(TEXT("must be one of %s"), *Choices));
	}

	/** A plane's flight block (#821): exactly its keys, aerofoils, thrusters and spin bones within their caps and ranges, the optional numbers the contract's defaults. */
	void ReadVehicleFlight(FVehicleDocParser& P, const TSharedPtr<FJsonObject>& Root, FMantleVehicleDesc& D)
	{
		const FString Pointer = TEXT("/flight");
		TSharedPtr<FJsonObject> Block;
		FMantleVehicleFlightDesc F;
		if (!P.Object(Root, TEXT(""), TEXT("flight"), Block)
			|| !P.Keys(Block, Pointer,
				{ TEXT("airSpeedLimit"), TEXT("cruiseThrottle"), TEXT("pitchAuthority"), TEXT("throttleLeverRate"), TEXT("linearDamping"), TEXT("angularDamping"),
				  TEXT("aerofoils"), TEXT("thrusters"), TEXT("spinBones"), TEXT("pilotGrips") },
				{ TEXT("airSpeedLimit"), TEXT("aerofoils"), TEXT("thrusters") })
			|| !P.Number(Block, Pointer, TEXT("airSpeedLimit"), TEXT("flight/airSpeedLimit"), F.AirSpeedLimit)
			|| !P.OptNumber(Block, Pointer, TEXT("cruiseThrottle"), TEXT("flight/cruiseThrottle"), F.CruiseThrottle)
			|| !P.OptNumber(Block, Pointer, TEXT("pitchAuthority"), TEXT("flight/pitchAuthority"), F.PitchAuthority)
			|| !P.OptNumber(Block, Pointer, TEXT("throttleLeverRate"), TEXT("flight/throttleLeverRate"), F.ThrottleLeverRate)
			|| !P.OptNumber(Block, Pointer, TEXT("linearDamping"), TEXT("flight/linearDamping"), F.LinearDamping)
			|| !P.OptNumber(Block, Pointer, TEXT("angularDamping"), TEXT("flight/angularDamping"), F.AngularDamping))
		{
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* Aerofoils = nullptr;
		if (!P.Array(Block->TryGetField(TEXT("aerofoils")), Pointer + TEXT("/aerofoils"), 1, FMantleVehicleLimits::MaxAerofoils, Aerofoils))
		{
			return;
		}
		static const TCHAR* const Kinds[] = { TEXT("fixed"), TEXT("wing"), TEXT("elevator"), TEXT("rudder") };
		for (int32 Index = 0; Index < Aerofoils->Num(); ++Index)
		{
			const FString ItemPointer = FString::Printf(TEXT("%s/aerofoils/%d"), *Pointer, Index);
			const TSharedPtr<FJsonValue>& Value = (*Aerofoils)[Index];
			const TSharedPtr<FJsonObject> Item = Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
			if (!Item.IsValid())
			{
				P.Fail(ItemPointer, TEXT("must be an object"));
				return;
			}
			FMantleVehicleAerofoilDesc A;
			int32 Kind = 0;
			if (!P.Keys(Item, ItemPointer,
					{ TEXT("bone"), TEXT("location"), TEXT("type"), TEXT("mirror"), TEXT("area"), TEXT("camber"), TEXT("maxControlAngle"), TEXT("stallAngle") },
					{ TEXT("location"), TEXT("type"), TEXT("area") })
				|| !ReadVehicleOptionalBone(P, Item, ItemPointer, A.Bone)
				|| !P.Vector(Item->TryGetField(TEXT("location")), ItemPointer + TEXT("/location"), TEXT("flight/aerofoils/*/location/*"), A.Location)
				|| !ReadVehicleEnumName(P, Item, ItemPointer, TEXT("type"), Kinds, Kind)
				|| (Item->HasField(TEXT("mirror")) && !P.Bool(Item, ItemPointer, TEXT("mirror"), A.bMirror))
				|| !P.Number(Item, ItemPointer, TEXT("area"), TEXT("flight/aerofoils/*/area"), A.AreaM2)
				|| !P.OptNumber(Item, ItemPointer, TEXT("camber"), TEXT("flight/aerofoils/*/camber"), A.Camber)
				|| !P.OptNumber(Item, ItemPointer, TEXT("maxControlAngle"), TEXT("flight/aerofoils/*/maxControlAngle"), A.MaxControlAngle)
				|| !P.OptNumber(Item, ItemPointer, TEXT("stallAngle"), TEXT("flight/aerofoils/*/stallAngle"), A.StallAngle))
			{
				return;
			}
			A.Type = static_cast<EMantleVehicleAerofoilKind>(Kind);
			F.Aerofoils.Add(A);
		}

		const TArray<TSharedPtr<FJsonValue>>* Thrusters = nullptr;
		if (!P.Array(Block->TryGetField(TEXT("thrusters")), Pointer + TEXT("/thrusters"), 1, FMantleVehicleLimits::MaxThrusters, Thrusters))
		{
			return;
		}
		for (int32 Index = 0; Index < Thrusters->Num(); ++Index)
		{
			const FString ItemPointer = FString::Printf(TEXT("%s/thrusters/%d"), *Pointer, Index);
			const TSharedPtr<FJsonValue>& Value = (*Thrusters)[Index];
			const TSharedPtr<FJsonObject> Item = Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
			if (!Item.IsValid())
			{
				P.Fail(ItemPointer, TEXT("must be an object"));
				return;
			}
			FMantleVehicleThrusterDesc T;
			if (!P.Keys(Item, ItemPointer, { TEXT("bone"), TEXT("location"), TEXT("direction"), TEXT("maxThrust") }, { TEXT("location"), TEXT("maxThrust") })
				|| !ReadVehicleOptionalBone(P, Item, ItemPointer, T.Bone)
				|| !P.Vector(Item->TryGetField(TEXT("location")), ItemPointer + TEXT("/location"), TEXT("flight/thrusters/*/location/*"), T.Location)
				|| !ReadVehicleDirection(P, Item, ItemPointer, TEXT("direction"), TEXT("flight/thrusters/*/direction"), T.Direction)
				|| !P.Number(Item, ItemPointer, TEXT("maxThrust"), TEXT("flight/thrusters/*/maxThrust"), T.MaxThrustNewtons))
			{
				return;
			}
			F.Thrusters.Add(T);
		}
		if (!ReadVehicleSpinBones(P, Block, TEXT("flight"), F.SpinBones))
		{
			return;
		}
		if (Block->HasField(TEXT("pilotGrips")))
		{
			if (!ReadVehicleGripPair(P, Block, Pointer, TEXT("pilotGrips"), TEXT("flight/pilotGrips"), F.PilotGrips))
			{
				return;
			}
			F.bPilotGrips = true;
		}
		D.Flight = MoveTemp(F);
	}

	/** A helicopter's block (#821): exactly its keys, its rotors and spin bones within their caps and ranges, the optional numbers the contract's defaults. */
	void ReadVehicleHelicopter(FVehicleDocParser& P, const TSharedPtr<FJsonObject>& Root, FMantleVehicleDesc& D)
	{
		const FString Pointer = TEXT("/helicopter");
		TSharedPtr<FJsonObject> Block;
		FMantleVehicleHelicopterDesc H;
		const TArray<TSharedPtr<FJsonValue>>* Rotors = nullptr;
		if (!P.Object(Root, TEXT(""), TEXT("helicopter"), Block)
			|| !P.Keys(Block, Pointer,
				{ TEXT("airSpeedLimit"), TEXT("hoverCollective"), TEXT("throttleLeverRate"), TEXT("linearDamping"), TEXT("angularDamping"), TEXT("rotors"),
				  TEXT("spinBones"), TEXT("pilotGrips") },
				{ TEXT("airSpeedLimit"), TEXT("hoverCollective"), TEXT("rotors") })
			|| !P.Number(Block, Pointer, TEXT("airSpeedLimit"), TEXT("helicopter/airSpeedLimit"), H.AirSpeedLimit)
			|| !P.Number(Block, Pointer, TEXT("hoverCollective"), TEXT("helicopter/hoverCollective"), H.HoverCollective)
			|| !P.OptNumber(Block, Pointer, TEXT("throttleLeverRate"), TEXT("helicopter/throttleLeverRate"), H.ThrottleLeverRate)
			|| !P.OptNumber(Block, Pointer, TEXT("linearDamping"), TEXT("helicopter/linearDamping"), H.LinearDamping)
			|| !P.OptNumber(Block, Pointer, TEXT("angularDamping"), TEXT("helicopter/angularDamping"), H.AngularDamping)
			|| !P.Array(Block->TryGetField(TEXT("rotors")), Pointer + TEXT("/rotors"), 1, FMantleVehicleLimits::MaxRotors, Rotors))
		{
			return;
		}
		for (int32 Index = 0; Index < Rotors->Num(); ++Index)
		{
			const FString ItemPointer = FString::Printf(TEXT("%s/rotors/%d"), *Pointer, Index);
			const TSharedPtr<FJsonValue>& Value = (*Rotors)[Index];
			const TSharedPtr<FJsonObject> Item = Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
			if (!Item.IsValid())
			{
				P.Fail(ItemPointer, TEXT("must be an object"));
				return;
			}
			FMantleVehicleRotorDesc R;
			if (!P.Keys(Item, ItemPointer,
					{ TEXT("bone"), TEXT("location"), TEXT("axis"), TEXT("maxThrust"), TEXT("pitchAcceleration"), TEXT("rollAcceleration"), TEXT("yawAcceleration"),
					  TEXT("stabilityAssist"), TEXT("stabilityStiffness"), TEXT("stabilityDamping"), TEXT("stabilityMaxAcceleration") },
					{ TEXT("location"), TEXT("maxThrust") })
				|| !ReadVehicleOptionalBone(P, Item, ItemPointer, R.Bone)
				|| !P.Vector(Item->TryGetField(TEXT("location")), ItemPointer + TEXT("/location"), TEXT("helicopter/rotors/*/location/*"), R.Location)
				|| !ReadVehicleDirection(P, Item, ItemPointer, TEXT("axis"), TEXT("helicopter/rotors/*/axis"), R.Axis)
				|| !P.Number(Item, ItemPointer, TEXT("maxThrust"), TEXT("helicopter/rotors/*/maxThrust"), R.MaxThrustNewtons)
				|| !P.OptNumber(Item, ItemPointer, TEXT("pitchAcceleration"), TEXT("helicopter/rotors/*/pitchAcceleration"), R.PitchAcceleration)
				|| !P.OptNumber(Item, ItemPointer, TEXT("rollAcceleration"), TEXT("helicopter/rotors/*/rollAcceleration"), R.RollAcceleration)
				|| !P.OptNumber(Item, ItemPointer, TEXT("yawAcceleration"), TEXT("helicopter/rotors/*/yawAcceleration"), R.YawAcceleration)
				|| (Item->HasField(TEXT("stabilityAssist")) && !P.Bool(Item, ItemPointer, TEXT("stabilityAssist"), R.bStabilityAssist))
				|| !P.OptNumber(Item, ItemPointer, TEXT("stabilityStiffness"), TEXT("helicopter/rotors/*/stabilityStiffness"), R.StabilityStiffness)
				|| !P.OptNumber(Item, ItemPointer, TEXT("stabilityDamping"), TEXT("helicopter/rotors/*/stabilityDamping"), R.StabilityDamping)
				|| !P.OptNumber(Item, ItemPointer, TEXT("stabilityMaxAcceleration"), TEXT("helicopter/rotors/*/stabilityMaxAcceleration"), R.StabilityMaxAcceleration))
			{
				return;
			}
			H.Rotors.Add(R);
		}
		if (!ReadVehicleSpinBones(P, Block, TEXT("helicopter"), H.SpinBones))
		{
			return;
		}
		if (Block->HasField(TEXT("pilotGrips")))
		{
			if (!ReadVehicleGripPair(P, Block, Pointer, TEXT("pilotGrips"), TEXT("helicopter/pilotGrips"), H.PilotGrips))
			{
				return;
			}
			H.bPilotGrips = true;
		}
		D.Helicopter = MoveTemp(H);
	}

	/** A hovercraft's block (#821): exactly its keys, its pads within their caps, the fan, the optional fin and damping, and the optional numbers the contract's defaults. */
	void ReadVehicleHover(FVehicleDocParser& P, const TSharedPtr<FJsonObject>& Root, FMantleVehicleDesc& D)
	{
		const FString Pointer = TEXT("/hover");
		TSharedPtr<FJsonObject> Block;
		FMantleVehicleHoverDesc H;
		const TArray<TSharedPtr<FJsonValue>>* Pads = nullptr;
		if (!P.Object(Root, TEXT(""), TEXT("hover"), Block)
			|| !P.Keys(Block, Pointer, { TEXT("pads"), TEXT("thruster"), TEXT("rudder"), TEXT("damping"), TEXT("angularDamping") }, { TEXT("pads"), TEXT("thruster") })
			|| !P.Array(Block->TryGetField(TEXT("pads")), Pointer + TEXT("/pads"), FMantleVehicleLimits::MinHoverPads, FMantleVehicleLimits::MaxHoverPads, Pads))
		{
			return;
		}
		for (int32 Index = 0; Index < Pads->Num(); ++Index)
		{
			const FString ItemPointer = FString::Printf(TEXT("%s/pads/%d"), *Pointer, Index);
			const TSharedPtr<FJsonValue>& Value = (*Pads)[Index];
			const TSharedPtr<FJsonObject> Item = Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
			if (!Item.IsValid())
			{
				P.Fail(ItemPointer, TEXT("must be an object"));
				return;
			}
			FMantleVehicleHoverPadDesc Pad;
			if (!P.Keys(Item, ItemPointer, { TEXT("location"), TEXT("maxRaise"), TEXT("maxDrop"), TEXT("springRate"), TEXT("springDamping") }, { TEXT("location") })
				|| !P.Vector(Item->TryGetField(TEXT("location")), ItemPointer + TEXT("/location"), TEXT("hover/pads/*/location/*"), Pad.Location)
				|| !P.OptNumber(Item, ItemPointer, TEXT("maxRaise"), TEXT("hover/pads/*/maxRaise"), Pad.MaxRaise)
				|| !P.OptNumber(Item, ItemPointer, TEXT("maxDrop"), TEXT("hover/pads/*/maxDrop"), Pad.MaxDrop)
				|| !P.OptNumber(Item, ItemPointer, TEXT("springRate"), TEXT("hover/pads/*/springRate"), Pad.SpringRate)
				|| !P.OptNumber(Item, ItemPointer, TEXT("springDamping"), TEXT("hover/pads/*/springDamping"), Pad.SpringDamping))
			{
				return;
			}
			H.Pads.Add(Pad);
		}

		TSharedPtr<FJsonObject> Fan;
		FMantleVehicleHoverThrusterDesc& T = H.Thruster;
		const FString FanPointer = Pointer + TEXT("/thruster");
		if (!P.Object(Block, Pointer, TEXT("thruster"), Fan)
			|| !P.Keys(Fan, FanPointer,
				{ TEXT("location"), TEXT("direction"), TEXT("maxThrust"), TEXT("reverseShare"), TEXT("vectorAngle"), TEXT("topSpeed"), TEXT("brakeFade") },
				{ TEXT("location"), TEXT("maxThrust") })
			|| !P.Vector(Fan->TryGetField(TEXT("location")), FanPointer + TEXT("/location"), TEXT("hover/thruster/location/*"), T.Location)
			|| !ReadVehicleDirection(P, Fan, FanPointer, TEXT("direction"), TEXT("hover/thruster/direction"), T.Direction)
			|| !P.Number(Fan, FanPointer, TEXT("maxThrust"), TEXT("hover/thruster/maxThrust"), T.MaxThrustNewtons)
			|| !P.OptNumber(Fan, FanPointer, TEXT("reverseShare"), TEXT("hover/thruster/reverseShare"), T.ReverseShare)
			|| !P.OptNumber(Fan, FanPointer, TEXT("vectorAngle"), TEXT("hover/thruster/vectorAngle"), T.VectorAngleDegrees)
			|| !P.OptNumber(Fan, FanPointer, TEXT("topSpeed"), TEXT("hover/thruster/topSpeed"), T.TopSpeedCmPerSec)
			|| !P.OptNumber(Fan, FanPointer, TEXT("brakeFade"), TEXT("hover/thruster/brakeFade"), T.BrakeFadeCmPerSec))
		{
			return;
		}

		if (Block->HasField(TEXT("rudder")))
		{
			TSharedPtr<FJsonObject> Fin;
			FMantleVehicleHoverRudderDesc& R = H.Rudder;
			const FString FinPointer = Pointer + TEXT("/rudder");
			if (!P.Object(Block, Pointer, TEXT("rudder"), Fin)
				|| !P.Keys(Fin, FinPointer, { TEXT("location"), TEXT("area"), TEXT("camber"), TEXT("maxControlAngle"), TEXT("stallAngle") },
					{ TEXT("location"), TEXT("area"), TEXT("maxControlAngle"), TEXT("stallAngle") })
				|| !P.Vector(Fin->TryGetField(TEXT("location")), FinPointer + TEXT("/location"), TEXT("hover/rudder/location/*"), R.Location)
				|| !P.Number(Fin, FinPointer, TEXT("area"), TEXT("hover/rudder/area"), R.AreaM2)
				|| !P.OptNumber(Fin, FinPointer, TEXT("camber"), TEXT("hover/rudder/camber"), R.Camber)
				|| !P.Number(Fin, FinPointer, TEXT("maxControlAngle"), TEXT("hover/rudder/maxControlAngle"), R.MaxControlAngle)
				|| !P.Number(Fin, FinPointer, TEXT("stallAngle"), TEXT("hover/rudder/stallAngle"), R.StallAngle))
			{
				return;
			}
			H.bRudder = true;
		}
		if (Block->HasField(TEXT("damping")))
		{
			TSharedPtr<FJsonObject> Damping;
			FMantleVehicleHoverDampingDesc& M = H.Damping;
			const FString DampingPointer = Pointer + TEXT("/damping");
			if (!P.Object(Block, Pointer, TEXT("damping"), Damping)
				|| !P.Keys(Damping, DampingPointer,
					{ TEXT("lateralPerSecond"), TEXT("maxLateralAccel"), TEXT("yawPerSecond"), TEXT("maxYawAccel"), TEXT("skirtBrakePerSecond"), TEXT("maxSkirtBrakeAccel") }, {})
				|| !P.OptNumber(Damping, DampingPointer, TEXT("lateralPerSecond"), TEXT("hover/damping/lateralPerSecond"), M.LateralPerSecond)
				|| !P.OptNumber(Damping, DampingPointer, TEXT("maxLateralAccel"), TEXT("hover/damping/maxLateralAccel"), M.MaxLateralAccel)
				|| !P.OptNumber(Damping, DampingPointer, TEXT("yawPerSecond"), TEXT("hover/damping/yawPerSecond"), M.YawPerSecond)
				|| !P.OptNumber(Damping, DampingPointer, TEXT("maxYawAccel"), TEXT("hover/damping/maxYawAccel"), M.MaxYawAccelDegrees)
				|| !P.OptNumber(Damping, DampingPointer, TEXT("skirtBrakePerSecond"), TEXT("hover/damping/skirtBrakePerSecond"), M.SkirtBrakePerSecond)
				|| !P.OptNumber(Damping, DampingPointer, TEXT("maxSkirtBrakeAccel"), TEXT("hover/damping/maxSkirtBrakeAccel"), M.MaxSkirtBrakeAccel))
			{
				return;
			}
		}
		if (!P.OptNumber(Block, Pointer, TEXT("angularDamping"), TEXT("hover/angularDamping"), H.AngularDamping))
		{
			return;
		}
		D.Hover = MoveTemp(H);
	}

	/** A seat's kind (#821, seat.kind or passengerSeat.kind) when it names one: car, motorcycle or pilot. */
	bool ReadVehicleSeatKind(FVehicleDocParser& P, const TSharedPtr<FJsonObject>& Seat, const FString& Pointer, EMantleVehicleSeatKind& Out)
	{
		if (!Seat->HasField(TEXT("kind")))
		{
			return true;
		}
		static const TCHAR* const Kinds[] = { TEXT("car"), TEXT("motorcycle"), TEXT("pilot") };
		int32 Kind = 0;
		if (!ReadVehicleEnumName(P, Seat, Pointer, TEXT("kind"), Kinds, Kind))
		{
			return false;
		}
		Out = static_cast<EMantleVehicleSeatKind>(Kind);
		return true;
	}

	/** The rider's rotation control (#821): exactly its keys, an absent one 0 or false (not the definition's default), each number within its range. */
	bool ReadVehicleRotationControl(FVehicleDocParser& P, const TSharedPtr<FJsonObject>& Rider, const FString& RiderPointer, FMantleVehicleRotationControlDesc& Out)
	{
		TSharedPtr<FJsonObject> Control;
		const FString Pointer = RiderPointer + TEXT("/rotationControl");
		FMantleVehicleRotationControlDesc C;
		if (!P.Object(Rider, RiderPointer, TEXT("rotationControl"), Control)
			|| !P.Keys(Control, Pointer,
				{ TEXT("torqueControl"), TEXT("torqueRotationDamping"), TEXT("targetRotation"), TEXT("rollVsSpeed"), TEXT("rollControlScaling"), TEXT("rollMaxAngle"),
				  TEXT("pitchControlScaling"), TEXT("pitchMaxAngle"), TEXT("rotationStiffness"), TEXT("rotationDamping"), TEXT("maxAccel"),
				  TEXT("autoCentreRollStrength"), TEXT("autoCentrePitchStrength"), TEXT("autoCentreYawStrength") }, {})
			|| (Control->HasField(TEXT("torqueControl")) && !P.Bool(Control, Pointer, TEXT("torqueControl"), C.bTorqueControl))
			|| (Control->HasField(TEXT("targetRotation")) && !P.Bool(Control, Pointer, TEXT("targetRotation"), C.bTargetRotation))
			|| (Control->HasField(TEXT("rollVsSpeed")) && !P.Bool(Control, Pointer, TEXT("rollVsSpeed"), C.bRollVsSpeed))
			|| !P.OptNumber(Control, Pointer, TEXT("torqueRotationDamping"), TEXT("seat/rider/rotationControl/torqueRotationDamping"), C.TorqueRotationDamping)
			|| !P.OptNumber(Control, Pointer, TEXT("rollControlScaling"), TEXT("seat/rider/rotationControl/rollControlScaling"), C.RollControlScaling)
			|| !P.OptNumber(Control, Pointer, TEXT("rollMaxAngle"), TEXT("seat/rider/rotationControl/rollMaxAngle"), C.RollMaxAngle)
			|| !P.OptNumber(Control, Pointer, TEXT("pitchControlScaling"), TEXT("seat/rider/rotationControl/pitchControlScaling"), C.PitchControlScaling)
			|| !P.OptNumber(Control, Pointer, TEXT("pitchMaxAngle"), TEXT("seat/rider/rotationControl/pitchMaxAngle"), C.PitchMaxAngle)
			|| !P.OptNumber(Control, Pointer, TEXT("rotationStiffness"), TEXT("seat/rider/rotationControl/rotationStiffness"), C.RotationStiffness)
			|| !P.OptNumber(Control, Pointer, TEXT("rotationDamping"), TEXT("seat/rider/rotationControl/rotationDamping"), C.RotationDamping)
			|| !P.OptNumber(Control, Pointer, TEXT("maxAccel"), TEXT("seat/rider/rotationControl/maxAccel"), C.MaxAccel)
			|| !P.OptNumber(Control, Pointer, TEXT("autoCentreRollStrength"), TEXT("seat/rider/rotationControl/autoCentreRollStrength"), C.AutoCentreRollStrength)
			|| !P.OptNumber(Control, Pointer, TEXT("autoCentrePitchStrength"), TEXT("seat/rider/rotationControl/autoCentrePitchStrength"), C.AutoCentrePitchStrength)
			|| !P.OptNumber(Control, Pointer, TEXT("autoCentreYawStrength"), TEXT("seat/rider/rotationControl/autoCentreYawStrength"), C.AutoCentreYawStrength))
		{
			return false;
		}
		Out = C;
		return true;
	}

	/** The driver's motorcycle rider fit (#821, seat.rider): exactly its keys, the fork bone, both grip pairs, the offsets and the rotation control. That the seat is a motorcycle's is CheckVehicleMovementRules'. */
	void ReadVehicleRider(FVehicleDocParser& P, const TSharedPtr<FJsonObject>& Seat, FMantleVehicleDesc& D)
	{
		const FString Pointer = TEXT("/seat/rider");
		TSharedPtr<FJsonObject> Rider;
		FMantleVehicleRiderDesc R;
		if (!P.Object(Seat, TEXT("/seat"), TEXT("rider"), Rider)
			|| !P.Keys(Rider, Pointer,
				{ TEXT("forkBone"), TEXT("handlebar"), TEXT("footPegs"), TEXT("pelvisOffset"), TEXT("stopPelvisShift"), TEXT("kneeSplayDegrees"), TEXT("rearTireFriction"),
				  TEXT("rearBrakeTorque"), TEXT("rotationControl") },
				{ TEXT("forkBone"), TEXT("handlebar"), TEXT("footPegs") })
			|| !ReadLinkageBone(P, Rider, Pointer, TEXT("forkBone"), R.ForkBone)
			|| !ReadVehicleGripPair(P, Rider, Pointer, TEXT("handlebar"), TEXT("seat/rider/handlebar"), R.Handlebar)
			|| !ReadVehicleGripPair(P, Rider, Pointer, TEXT("footPegs"), TEXT("seat/rider/footPegs"), R.FootPegs)
			|| (Rider->HasField(TEXT("pelvisOffset")) && !P.Vector(Rider->TryGetField(TEXT("pelvisOffset")), Pointer + TEXT("/pelvisOffset"), TEXT("seat/rider/pelvisOffset/*"), R.PelvisOffset))
			|| (Rider->HasField(TEXT("stopPelvisShift"))
				&& !P.Vector(Rider->TryGetField(TEXT("stopPelvisShift")), Pointer + TEXT("/stopPelvisShift"), TEXT("seat/rider/stopPelvisShift/*"), R.StopPelvisShift))
			|| !P.OptNumber(Rider, Pointer, TEXT("kneeSplayDegrees"), TEXT("seat/rider/kneeSplayDegrees"), R.KneeSplayDegrees)
			|| (Rider->HasField(TEXT("rotationControl")) && !ReadVehicleRotationControl(P, Rider, Pointer, R.RotationControl)))
		{
			return;
		}
		if (Rider->HasField(TEXT("rearTireFriction")))
		{
			if (!P.Number(Rider, Pointer, TEXT("rearTireFriction"), TEXT("seat/rider/rearTireFriction"), R.RearTireFriction))
			{
				return;
			}
			R.bRearTireFriction = true;
		}
		if (Rider->HasField(TEXT("rearBrakeTorque")))
		{
			if (!P.Number(Rider, Pointer, TEXT("rearBrakeTorque"), TEXT("seat/rider/rearBrakeTorque"), R.RearBrakeTorque))
			{
				return;
			}
			R.bRearBrakeTorque = true;
		}
		// A handlebar grip that names no bone is on the fork.
		for (FMantleVehicleGripDesc* Grip : { &R.Handlebar.Left, &R.Handlebar.Right })
		{
			if (Grip->Bone.IsNone())
			{
				Grip->Bone = R.ForkBone;
			}
		}
		D.Rider = MoveTemp(R);
		D.bRider = true;
	}

	/**
	 * The movement blocks' rules across fields (the cook's movementRules, #821): each aerofoil bone once and not also a spin bone; no grip, and not the
	 * fork, on a wheel's bone; a rider only on a motorcycle seat.
	 */
	bool CheckVehicleMovementRules(FVehicleDocParser& P, const FMantleVehicleDesc& D)
	{
		auto OnWheel = [&D](const FName Bone)
		{
			// FName compares without regard to case, as the body's bone names do.
			return !Bone.IsNone() && D.Wheels.ContainsByPredicate([Bone](const FMantleVehicleWheelDesc& Wheel) { return Wheel.Bone == Bone; });
		};
		auto CheckGrips = [&](const FString& Pointer, const FMantleVehicleGripPairDesc& Pair)
		{
			const TPair<const TCHAR*, const FMantleVehicleGripDesc*> Sides[] = { { TEXT("/left/bone"), &Pair.Left }, { TEXT("/right/bone"), &Pair.Right } };
			for (const TPair<const TCHAR*, const FMantleVehicleGripDesc*>& Side : Sides)
			{
				if (OnWheel(Side.Value->Bone))
				{
					return P.Fail(Pointer + Side.Key, FString::Printf(TEXT("is %s, a wheel's bone, which spins; hold on to another bone"), *Side.Value->Bone.ToString()));
				}
			}
			return true;
		};
		if (D.MovementType == EMantleVehicleMovementKind::Plane)
		{
			const FMantleVehicleFlightDesc& F = D.Flight;
			for (int32 Index = 0; Index < F.Aerofoils.Num(); ++Index)
			{
				const FName Bone = F.Aerofoils[Index].Bone;
				if (Bone.IsNone())
				{
					continue;
				}
				for (int32 Before = 0; Before < Index; ++Before)
				{
					if (F.Aerofoils[Before].Bone == Bone)
					{
						return P.Fail(FString::Printf(TEXT("/flight/aerofoils/%d/bone"), Index),
							FString::Printf(TEXT("is %s, aerofoils/%d's bone too; each control surface has its own"), *Bone.ToString(), Before));
					}
				}
				if (F.SpinBones.ContainsByPredicate([Bone](const FMantleVehicleSpinBoneDesc& Spin) { return Spin.Bone == Bone; }))
				{
					return P.Fail(FString::Printf(TEXT("/flight/aerofoils/%d/bone"), Index),
						FString::Printf(TEXT("is %s, also a spinning bone; a bone is posed by one thing"), *Bone.ToString()));
				}
			}
			if (F.bPilotGrips && !CheckGrips(TEXT("/flight/pilotGrips"), F.PilotGrips))
			{
				return false;
			}
		}
		if (D.MovementType == EMantleVehicleMovementKind::Helicopter && D.Helicopter.bPilotGrips && !CheckGrips(TEXT("/helicopter/pilotGrips"), D.Helicopter.PilotGrips))
		{
			return false;
		}
		if (D.bRider)
		{
			if (D.SeatKind != EMantleVehicleSeatKind::Motorcycle)
			{
				return P.Fail(TEXT("/seat/rider"), TEXT("is a motorcycle rider's fit, but the seat's kind is not motorcycle; set seat.kind to motorcycle"));
			}
			if (OnWheel(D.Rider.ForkBone))
			{
				return P.Fail(TEXT("/seat/rider/forkBone"), FString::Printf(TEXT("is %s, a wheel's bone; the fork is the bone the steered wheel hangs under"),
					*D.Rider.ForkBone.ToString()));
			}
			return CheckGrips(TEXT("/seat/rider/handlebar"), D.Rider.Handlebar) && CheckGrips(TEXT("/seat/rider/footPegs"), D.Rider.FootPegs);
		}
		return true;
	}

	/** The movement type the document names (#821): wheeled when it names none, else wheeled, plane, helicopter or hover. */
	bool ReadVehicleMovementType(FVehicleDocParser& P, const TSharedPtr<FJsonObject>& Root, FMantleVehicleDesc& D)
	{
		if (!Root->HasField(TEXT("movementType")))
		{
			return true;
		}
		static const TCHAR* const Kinds[] = { TEXT("wheeled"), TEXT("plane"), TEXT("helicopter"), TEXT("hover") };
		int32 Kind = 0;
		if (!ReadVehicleEnumName(P, Root, TEXT(""), TEXT("movementType"), Kinds, Kind))
		{
			return false;
		}
		D.MovementType = static_cast<EMantleVehicleMovementKind>(Kind);
		return true;
	}

	/**
	 * Whether the movement blocks agree with the movement type (the cook's movementRules): a plane has a flight block, a helicopter a helicopter block and a
	 * hovercraft a hover block, and no other type has any block.
	 */
	bool CheckVehicleMovementBlocks(FVehicleDocParser& P, const TSharedPtr<FJsonObject>& Root, const EMantleVehicleMovementKind Movement)
	{
		struct FBlock
		{
			EMantleVehicleMovementKind Movement;
			const TCHAR* Key;
			const TCHAR* Name;
		};
		static const FBlock Blocks[] = {
			{ EMantleVehicleMovementKind::Plane, TEXT("flight"), TEXT("plane") },
			{ EMantleVehicleMovementKind::Helicopter, TEXT("helicopter"), TEXT("helicopter") },
			{ EMantleVehicleMovementKind::Hover, TEXT("hover"), TEXT("hover") },
		};
		static const TCHAR* const Names[] = { TEXT("wheeled"), TEXT("plane"), TEXT("helicopter"), TEXT("hover") };
		for (const FBlock& Block : Blocks)
		{
			const bool bHas = Root->HasField(Block.Key);
			if (Movement == Block.Movement && !bHas)
			{
				return P.Fail(TEXT("/movementType"), FString::Printf(TEXT("is \"%s\", which needs a %s block"), Block.Name, Block.Key));
			}
			if (Movement != Block.Movement && bHas)
			{
				return P.Fail(FString(TEXT("/")) + Block.Key, FString::Printf(TEXT("belongs to movementType \"%s\"; this vehicle is \"%s\""), Block.Name,
					Names[static_cast<int32>(Movement)]));
			}
		}
		return true;
	}

	bool FailVehicle(FString& OutError, const FString& Why)
	{
		OutError = Why;
		return false;
	}
}

TArray<FString> FMantleVehicleDesc::MaterialIds() const
{
	TArray<FString> Ids;
	for (const TPair<FString, FString>& Section : Materials)
	{
		Ids.AddUnique(Section.Value);
	}
	for (const FMantleVehiclePartDesc& Part : Parts)
	{
		for (const FString& Id : Part.Materials)
		{
			if (!Id.IsEmpty())
			{
				Ids.AddUnique(Id);
			}
		}
	}
	return Ids;
}

TArray<FString> FMantleVehicleDesc::PartMeshIds() const
{
	TArray<FString> Ids;
	for (const FMantleVehiclePartDesc& Part : Parts)
	{
		Ids.AddUnique(Part.Mesh);
	}
	return Ids;
}

TArray<FString> FMantleVehicleDesc::SoundIds() const
{
	TArray<FString> Ids;
	if (HasHorn())
	{
		Ids.Add(HornSound);
	}
	return Ids;
}

bool FMantleVehicleLimits::GetRange(const FString& Path, double& OutMin, double& OutMax)
{
	for (const MantleVehicleAssetPrivate::FVehicleRange& Range : MantleVehicleAssetPrivate::GVehicleRanges)
	{
		if (Path.Equals(Range.Path, ESearchCase::CaseSensitive))
		{
			OutMin = Range.Min;
			OutMax = Range.Max;
			return true;
		}
	}
	return false;
}

bool FMantleVehicleAssetReader::ParseDocument(const TConstArrayView<uint8> Json, FMantleVehicleDesc& Out, FString& OutError)
{
	using namespace MantleVehicleAssetPrivate;

	Out = FMantleVehicleDesc();
	if (Json.Num() == 0 || Json.Num() > MaxDocumentBytes)
	{
		return FailVehicle(OutError, FString::Printf(TEXT("the vehicle document is %d bytes (1 to %d)"), Json.Num(), MaxDocumentBytes));
	}
	const auto Converted = StringCast<TCHAR>(reinterpret_cast<const UTF8CHAR*>(Json.GetData()), Json.Num());
	const FString Text = FString::ConstructFromPtrSize(Converted.Get(), Converted.Length());
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return FailVehicle(OutError, TEXT("the vehicle document is not a JSON object"));
	}

	FVehicleDocParser P;
	FMantleVehicleDesc D;
	// The preset the vehicle was built from (#728) is the cook's to enforce and it strips the key; it is accepted and ignored here,
	// checked first so a wrong preset is named even when other keys are also wrong.
	if (Root->HasField(TEXT("preset")) && !Root->HasTypedField<EJson::String>(TEXT("preset")))
	{
		P.Fail(TEXT("/preset"), TEXT("is not a string"));
	}

	P.Keys(Root, TEXT(""),
		{ TEXT("schemaVersion"), TEXT("mass"), TEXT("maxSpeed"), TEXT("centerOfMass"), TEXT("dragCoefficient"), TEXT("frontalArea"),
		  TEXT("engine"), TEXT("transmission"), TEXT("suspension"), TEXT("steering"), TEXT("brakes"), TEXT("tires"), TEXT("wheels"),
		  TEXT("seat"), TEXT("camera"), TEXT("collision"), TEXT("materials"), TEXT("paint"), TEXT("liveries"), TEXT("liveryUV"),
		  TEXT("defaultLivery"), TEXT("linkage"), TEXT("parts"), TEXT("passengerSeat"), TEXT("trunk"), TEXT("horn"), TEXT("preset"), TEXT("movementType"),
		  TEXT("flight"), TEXT("helicopter"), TEXT("hover") },
		{ TEXT("schemaVersion"), TEXT("mass"), TEXT("maxSpeed"), TEXT("dragCoefficient"), TEXT("frontalArea"), TEXT("engine"),
		  TEXT("transmission"), TEXT("suspension"), TEXT("steering"), TEXT("brakes"), TEXT("tires"), TEXT("wheels"), TEXT("seat"),
		  TEXT("camera"), TEXT("collision") });

	double Version = 0.0;
	if (P.Ok() && P.Number(Root, TEXT(""), TEXT("schemaVersion"), nullptr, Version, true) && Version != 1.0)
	{
		P.Fail(TEXT("/schemaVersion"), FString::Printf(TEXT("is %g; this build reads vehicle documents of version 1 (update Mantle)"), Version));
	}
	if (P.Ok())
	{
		P.Number(Root, TEXT(""), TEXT("mass"), TEXT("mass"), D.Mass);
		P.Number(Root, TEXT(""), TEXT("maxSpeed"), TEXT("maxSpeed"), D.MaxSpeed);
		P.Number(Root, TEXT(""), TEXT("dragCoefficient"), TEXT("dragCoefficient"), D.DragCoefficient);
		P.Number(Root, TEXT(""), TEXT("frontalArea"), TEXT("frontalArea"), D.FrontalArea);
		if (Root->HasField(TEXT("centerOfMass")))
		{
			D.bCenterOfMass = P.Vector(Root->TryGetField(TEXT("centerOfMass")), TEXT("/centerOfMass"), TEXT("centerOfMass/*"), D.CenterOfMass);
		}
		// #821: wheeled when it names none.
		if (P.Ok())
		{
			ReadVehicleMovementType(P, Root, D);
		}
	}

	TSharedPtr<FJsonObject> Engine;
	if (P.Ok() && P.Object(Root, TEXT(""), TEXT("engine"), Engine)
		&& P.Keys(Engine, TEXT("/engine"), { TEXT("maxTorque"), TEXT("maxRPM"), TEXT("idleRPM"), TEXT("torqueCurve") },
			{ TEXT("maxTorque"), TEXT("maxRPM"), TEXT("idleRPM"), TEXT("torqueCurve") }))
	{
		P.Number(Engine, TEXT("/engine"), TEXT("maxTorque"), TEXT("engine/maxTorque"), D.MaxTorque);
		P.Int(Engine, TEXT("/engine"), TEXT("maxRPM"), TEXT("engine/maxRPM"), D.MaxRPM);
		P.Int(Engine, TEXT("/engine"), TEXT("idleRPM"), TEXT("engine/idleRPM"), D.IdleRPM);
		const TArray<TSharedPtr<FJsonValue>>* Curve = nullptr;
		if (P.Ok() && P.Array(Engine->TryGetField(TEXT("torqueCurve")), TEXT("/engine/torqueCurve"), 2, FMantleVehicleLimits::MaxTorqueCurvePoints, Curve))
		{
			for (int32 Index = 0; Index < Curve->Num() && P.Ok(); ++Index)
			{
				const FString Pointer = FString::Printf(TEXT("/engine/torqueCurve/%d"), Index);
				const TArray<TSharedPtr<FJsonValue>>* Pair = nullptr;
				double Rpm = 0.0;
				double Torque = 0.0;
				if (P.Array((*Curve)[Index], Pointer, 2, 2, Pair)
					&& P.NumberValue((*Pair)[0], Pointer + TEXT("/0"), TEXT("engine/torqueCurve/*/0"), false, Rpm)
					&& P.NumberValue((*Pair)[1], Pointer + TEXT("/1"), TEXT("engine/torqueCurve/*/1"), false, Torque))
				{
					D.TorqueCurve.Add(FVector2D(Rpm, Torque));
				}
			}
		}
	}

	TSharedPtr<FJsonObject> Transmission;
	if (P.Ok() && P.Object(Root, TEXT(""), TEXT("transmission"), Transmission)
		&& P.Keys(Transmission, TEXT("/transmission"),
			{ TEXT("gearRatios"), TEXT("reverseRatio"), TEXT("finalDrive"), TEXT("changeUpRPM"), TEXT("changeDownRPM") },
			{ TEXT("gearRatios"), TEXT("reverseRatio"), TEXT("finalDrive"), TEXT("changeUpRPM"), TEXT("changeDownRPM") }))
	{
		const TArray<TSharedPtr<FJsonValue>>* Gears = nullptr;
		if (P.Array(Transmission->TryGetField(TEXT("gearRatios")), TEXT("/transmission/gearRatios"), 1, FMantleVehicleLimits::MaxGears, Gears))
		{
			for (int32 Index = 0; Index < Gears->Num() && P.Ok(); ++Index)
			{
				double Ratio = 0.0;
				if (P.NumberValue((*Gears)[Index], FString::Printf(TEXT("/transmission/gearRatios/%d"), Index),
					TEXT("transmission/gearRatios/*"), false, Ratio))
				{
					D.GearRatios.Add(static_cast<float>(Ratio));
				}
			}
		}
		P.Number(Transmission, TEXT("/transmission"), TEXT("reverseRatio"), TEXT("transmission/reverseRatio"), D.ReverseRatio);
		P.Number(Transmission, TEXT("/transmission"), TEXT("finalDrive"), TEXT("transmission/finalDrive"), D.FinalDrive);
		P.Int(Transmission, TEXT("/transmission"), TEXT("changeUpRPM"), TEXT("transmission/changeUpRPM"), D.ChangeUpRPM);
		P.Int(Transmission, TEXT("/transmission"), TEXT("changeDownRPM"), TEXT("transmission/changeDownRPM"), D.ChangeDownRPM);
	}

	TSharedPtr<FJsonObject> Suspension;
	if (P.Ok() && P.Object(Root, TEXT(""), TEXT("suspension"), Suspension)
		&& P.Keys(Suspension, TEXT("/suspension"),
			{ TEXT("maxRaise"), TEXT("maxDrop"), TEXT("springRate"), TEXT("springPreload"), TEXT("damping") },
			{ TEXT("maxRaise"), TEXT("maxDrop"), TEXT("springRate"), TEXT("springPreload"), TEXT("damping") }))
	{
		P.Number(Suspension, TEXT("/suspension"), TEXT("maxRaise"), TEXT("suspension/maxRaise"), D.SuspensionMaxRaise);
		P.Number(Suspension, TEXT("/suspension"), TEXT("maxDrop"), TEXT("suspension/maxDrop"), D.SuspensionMaxDrop);
		P.Number(Suspension, TEXT("/suspension"), TEXT("springRate"), TEXT("suspension/springRate"), D.SpringRate);
		P.Number(Suspension, TEXT("/suspension"), TEXT("springPreload"), TEXT("suspension/springPreload"), D.SpringPreload);
		P.Number(Suspension, TEXT("/suspension"), TEXT("damping"), TEXT("suspension/damping"), D.SpringDamping);
	}

	TSharedPtr<FJsonObject> Steering;
	if (P.Ok() && P.Object(Root, TEXT(""), TEXT("steering"), Steering)
		&& P.Keys(Steering, TEXT("/steering"), { TEXT("maxAngle"), TEXT("ackermann"), TEXT("speedCurve") }, { TEXT("maxAngle"), TEXT("ackermann") }))
	{
		P.Number(Steering, TEXT("/steering"), TEXT("maxAngle"), TEXT("steering/maxAngle"), D.MaxSteeringAngle);
		P.Bool(Steering, TEXT("/steering"), TEXT("ackermann"), D.bAckermann);
		// #821: optional, and absent from a car that steers its full lock at every speed.
		const TArray<TSharedPtr<FJsonValue>>* Curve = nullptr;
		if (P.Ok() && Steering->HasField(TEXT("speedCurve"))
			&& P.Array(Steering->TryGetField(TEXT("speedCurve")), TEXT("/steering/speedCurve"), 1, FMantleVehicleLimits::MaxSteeringCurvePoints, Curve))
		{
			for (int32 Index = 0; Index < Curve->Num() && P.Ok(); ++Index)
			{
				const FString Pointer = FString::Printf(TEXT("/steering/speedCurve/%d"), Index);
				const TArray<TSharedPtr<FJsonValue>>* Pair = nullptr;
				double Mph = 0.0;
				double Share = 0.0;
				if (P.Array((*Curve)[Index], Pointer, 2, 2, Pair)
					&& P.NumberValue((*Pair)[0], Pointer + TEXT("/0"), TEXT("steering/speedCurve/*/0"), false, Mph)
					&& P.NumberValue((*Pair)[1], Pointer + TEXT("/1"), TEXT("steering/speedCurve/*/1"), false, Share))
				{
					if (D.SteeringSpeedCurve.Num() > 0 && Mph <= D.SteeringSpeedCurve.Last().X)
					{
						P.Fail(Pointer + TEXT("/0"), FString::Printf(TEXT("mph %g does not rise past the point before (%g)"), Mph, D.SteeringSpeedCurve.Last().X));
					}
					else
					{
						D.SteeringSpeedCurve.Add(FVector2D(Mph, Share));
					}
				}
			}
		}
	}

	TSharedPtr<FJsonObject> Brakes;
	if (P.Ok() && P.Object(Root, TEXT(""), TEXT("brakes"), Brakes)
		&& P.Keys(Brakes, TEXT("/brakes"), { TEXT("brakeTorque"), TEXT("handbrakeTorque") }, { TEXT("brakeTorque"), TEXT("handbrakeTorque") }))
	{
		P.Number(Brakes, TEXT("/brakes"), TEXT("brakeTorque"), TEXT("brakes/brakeTorque"), D.BrakeTorque);
		P.Number(Brakes, TEXT("/brakes"), TEXT("handbrakeTorque"), TEXT("brakes/handbrakeTorque"), D.HandbrakeTorque);
	}

	TSharedPtr<FJsonObject> Tires;
	if (P.Ok() && P.Object(Root, TEXT(""), TEXT("tires"), Tires) && P.Keys(Tires, TEXT("/tires"), { TEXT("friction"), TEXT("rearFriction") }, { TEXT("friction") }))
	{
		P.Number(Tires, TEXT("/tires"), TEXT("friction"), TEXT("tires/friction"), D.TireFriction);
		// #821: optional, and absent from a vehicle whose every wheel grips alike.
		if (P.Ok() && Tires->HasField(TEXT("rearFriction")))
		{
			D.bTireRearFriction = P.Number(Tires, TEXT("/tires"), TEXT("rearFriction"), TEXT("tires/rearFriction"), D.TireRearFriction);
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* Wheels = nullptr;
	if (P.Ok() && P.Array(Root->TryGetField(TEXT("wheels")), TEXT("/wheels"), FMantleVehicleLimits::MinWheels, FMantleVehicleLimits::MaxWheels, Wheels))
	{
		for (int32 Index = 0; Index < Wheels->Num() && P.Ok(); ++Index)
		{
			const FString Pointer = FString::Printf(TEXT("/wheels/%d"), Index);
			const TSharedPtr<FJsonValue>& Value = (*Wheels)[Index];
			const TSharedPtr<FJsonObject> Wheel = Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
			if (!Wheel.IsValid())
			{
				P.Fail(Pointer, TEXT("must be an object"));
				break;
			}
			if (!P.Keys(Wheel, Pointer,
				{ TEXT("bone"), TEXT("steering"), TEXT("driven"), TEXT("handbrake"), TEXT("radius"), TEXT("width") },
				{ TEXT("bone"), TEXT("steering"), TEXT("driven"), TEXT("handbrake"), TEXT("radius"), TEXT("width") }))
			{
				break;
			}
			FMantleVehicleWheelDesc W;
			FString Bone;
			if (!Wheel->TryGetStringField(TEXT("bone"), Bone) || Bone.IsEmpty() || Bone.Len() > MaxBoneNameLength)
			{
				P.Fail(Pointer + TEXT("/bone"), FString::Printf(TEXT("must be a bone name of 1 to %d characters"), MaxBoneNameLength));
				break;
			}
			W.Bone = FName(*Bone);
			P.Bool(Wheel, Pointer, TEXT("steering"), W.bSteering);
			P.Bool(Wheel, Pointer, TEXT("driven"), W.bDriven);
			P.Bool(Wheel, Pointer, TEXT("handbrake"), W.bHandbrake);
			P.Number(Wheel, Pointer, TEXT("radius"), TEXT("wheels/*/radius"), W.Radius);
			P.Number(Wheel, Pointer, TEXT("width"), TEXT("wheels/*/width"), W.Width);
			D.Wheels.Add(W);
		}
	}

	TSharedPtr<FJsonObject> Seat;
	if (P.Ok() && P.Object(Root, TEXT(""), TEXT("seat"), Seat)
		&& P.Keys(Seat, TEXT("/seat"),
			{ TEXT("location"), TEXT("steeringWheel"), TEXT("door"), TEXT("pelvis"), TEXT("hands"), TEXT("feet"), TEXT("exits"), TEXT("hidden"), TEXT("kind"), TEXT("weaponArc"),
			  TEXT("exitSideDistance"), TEXT("rider") },
			{ TEXT("location") }))
	{
		P.Vector(Seat->TryGetField(TEXT("location")), TEXT("/seat/location"), TEXT("seat/location/*"), D.SeatLocation);
		if (P.Ok())
		{
			ReadVehicleWeaponArc(P, Seat, TEXT("/seat"), TEXT("seat"), D.SeatWeaponArc);
		}
		// Optional: a model with no inside hides the people sitting in it. A value that isn't true or false is refused.
		if (P.Ok() && Seat->HasField(TEXT("hidden")))
		{
			P.Bool(Seat, TEXT("/seat"), TEXT("hidden"), D.bSeatHidden);
		}
		// #546: optional, and absent from a seat without one.
		if (P.Ok() && Seat->HasField(TEXT("steeringWheel")))
		{
			ReadVehicleSteeringWheel(P, Seat, D);
		}
		if (P.Ok() && Seat->HasField(TEXT("door")))
		{
			ReadVehicleHinge(P, Seat, TEXT("/seat"), TEXT("door"), TEXT("seat/door"), D.SeatDoor);
		}
		// #612: optional, each group absent from a seat without it.
		if (P.Ok())
		{
			ReadVehicleSeatContacts(P, Seat, TEXT("/seat"), TEXT("seat"), D.SeatContacts);
		}
		// #821: the seat's kind, how far the driver steps out to the side, and a motorcycle rider's fit; each optional.
		if (P.Ok())
		{
			ReadVehicleSeatKind(P, Seat, TEXT("/seat"), D.SeatKind);
		}
		if (P.Ok() && Seat->HasField(TEXT("exitSideDistance")))
		{
			D.bSeatExitSideDistance = P.Number(Seat, TEXT("/seat"), TEXT("exitSideDistance"), TEXT("seat/exitSideDistance"), D.SeatExitSideDistance);
		}
		if (P.Ok() && Seat->HasField(TEXT("rider")))
		{
			ReadVehicleRider(P, Seat, D);
		}
	}
	// #546: optional, and absent from a car without them.
	if (P.Ok() && Root->HasField(TEXT("passengerSeat")))
	{
		ReadVehiclePassengerSeat(P, Root, D);
	}
	if (P.Ok() && Root->HasField(TEXT("trunk")))
	{
		ReadVehicleTrunk(P, Root, D);
	}
	if (P.Ok() && Root->HasField(TEXT("horn")))
	{
		ReadVehicleHorn(P, Root, D);
	}

	TSharedPtr<FJsonObject> Camera;
	if (P.Ok() && P.Object(Root, TEXT(""), TEXT("camera"), Camera)
		&& P.Keys(Camera, TEXT("/camera"), { TEXT("armLength"), TEXT("height"), TEXT("boomLocation") }, { TEXT("armLength"), TEXT("height") }))
	{
		P.Number(Camera, TEXT("/camera"), TEXT("armLength"), TEXT("camera/armLength"), D.CameraArmLength);
		P.Number(Camera, TEXT("/camera"), TEXT("height"), TEXT("camera/height"), D.CameraHeight);
		// #821: where the chase camera orbits; optional.
		if (P.Ok() && Camera->HasField(TEXT("boomLocation")))
		{
			D.bCameraBoom = P.Vector(Camera->TryGetField(TEXT("boomLocation")), TEXT("/camera/boomLocation"), TEXT("camera/boomLocation/*"), D.CameraBoomLocation);
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* Boxes = nullptr;
	if (P.Ok() && P.Array(Root->TryGetField(TEXT("collision")), TEXT("/collision"), 1, FMantleVehicleLimits::MaxCollisionBoxes, Boxes))
	{
		for (int32 Index = 0; Index < Boxes->Num() && P.Ok(); ++Index)
		{
			const FString Pointer = FString::Printf(TEXT("/collision/%d"), Index);
			const TSharedPtr<FJsonValue>& Value = (*Boxes)[Index];
			const TSharedPtr<FJsonObject> Box = Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
			if (!Box.IsValid())
			{
				P.Fail(Pointer, TEXT("must be an object"));
				break;
			}
			FMantleVehicleBoxDesc B;
			if (P.Keys(Box, Pointer, { TEXT("center"), TEXT("extent") }, { TEXT("center"), TEXT("extent") })
				&& P.Vector(Box->TryGetField(TEXT("center")), Pointer + TEXT("/center"), TEXT("collision/*/center/*"), B.Center)
				&& P.Vector(Box->TryGetField(TEXT("extent")), Pointer + TEXT("/extent"), TEXT("collision/*/extent/*"), B.Extent))
			{
				D.Collision.Add(B);
			}
		}
	}

	if (P.Ok() && Root->HasField(TEXT("materials")))
	{
		TSharedPtr<FJsonObject> Materials;
		if (P.Object(Root, TEXT(""), TEXT("materials"), Materials))
		{
			if (Materials->Values.Num() > FMantleVehicleLimits::MaxSections)
			{
				P.Fail(TEXT("/materials"), FString::Printf(TEXT("names %d sections; a body has at most %d"),
					Materials->Values.Num(), FMantleVehicleLimits::MaxSections));
			}
			for (const auto& Pair : Materials->Values)
			{
				const FString Section(Pair.Key.ToView());
				FString Id;
				if (Section.IsEmpty() || Section.Len() > MaxSectionNameLength || !Pair.Value.IsValid()
					|| !Pair.Value->TryGetString(Id) || !IsUploadedAssetId(Id))
				{
					P.Fail(TEXT("/materials/") + Section, TEXT("must name a body section and an uploaded material's id"));
					break;
				}
				D.Materials.Add(Section, Id);
			}
		}
	}

	// #435 (f) and #488: optional, and absent from a vehicle without them.
	if (P.Ok() && Root->HasField(TEXT("paint")))
	{
		ReadVehiclePaint(P, Root, D);
	}
	if (P.Ok() && Root->HasField(TEXT("liveries")))
	{
		ReadVehicleLiveries(P, Root, D);
	}
	if (P.Ok() && Root->HasField(TEXT("liveryUV")))
	{
		P.Int(Root, TEXT(""), TEXT("liveryUV"), TEXT("liveryUV"), D.LiveryUV);
	}
	if (P.Ok() && Root->HasField(TEXT("defaultLivery")))
	{
		P.Int(Root, TEXT(""), TEXT("defaultLivery"), TEXT("defaultLivery"), D.DefaultLivery);
	}
	// #435 (b): after the wheels, which the linkage may not move.
	if (P.Ok() && Root->HasField(TEXT("linkage")))
	{
		ReadVehicleLinkage(P, Root, D);
	}
	// #435 item 2: optional, and absent from a vehicle without parts.
	if (P.Ok() && Root->HasField(TEXT("parts")))
	{
		ReadVehicleParts(P, Root, D);
	}

	// #821: the movement type's block, after the wheels and the seats its rules read.
	if (P.Ok())
	{
		CheckVehicleMovementBlocks(P, Root, D.MovementType);
	}
	if (P.Ok() && D.MovementType == EMantleVehicleMovementKind::Plane)
	{
		ReadVehicleFlight(P, Root, D);
	}
	if (P.Ok() && D.MovementType == EMantleVehicleMovementKind::Helicopter)
	{
		ReadVehicleHelicopter(P, Root, D);
	}
	if (P.Ok() && D.MovementType == EMantleVehicleMovementKind::Hover)
	{
		ReadVehicleHover(P, Root, D);
	}

	if (P.Ok())
	{
		CheckVehicleRules(P, D);
	}
	if (P.Ok())
	{
		CheckVehicleMovementRules(P, D);
	}
	if (!P.Ok())
	{
		OutError = P.Error;
		return false;
	}
	Out = MoveTemp(D);
	return true;
}

bool FMantleVehicleAssetReader::ResolveMovementBones(FMantleVehicleDesc& D, const TConstArrayView<FMantleSkinnedMeshBone> Bones, FString& OutError)
{
	using namespace MantleVehicleAssetPrivate;

	if (Bones.Num() == 0)
	{
		return true;
	}
	auto FindBone = [&Bones](const FName Name)
	{
		return Bones.IndexOfByPredicate([Name](const FMantleSkinnedMeshBone& Bone) { return Bone.Name == Name; });
	};
	TSet<int32> WheelBones;
	TSet<int32> CarriesWheel;
	for (const FMantleVehicleWheelDesc& Wheel : D.Wheels)
	{
		const int32 Found = FindBone(Wheel.Bone);
		if (Found != INDEX_NONE)
		{
			WheelBones.Add(Found);
		}
		for (int32 Bone = Found; Bone != INDEX_NONE; Bone = Bones[Bone].ParentIndex)
		{
			CarriesWheel.Add(Bone);
		}
	}

	// How the movement blocks use a bone: a posed one (an aerofoil's, a spin bone) is not the root, a wheel's or one a wheel hangs from; the fork is neither the
	// root nor a wheel's; a grip's bone is any (an unnamed one is the root); a thruster's or a rotor's is a reference only.
	enum class EUse : uint8
	{
		Reference,
		Posed,
		Fork,
		Grip,
	};
	struct FRef
	{
		FName* Name;
		FString Pointer;
		EUse Use;
	};
	TArray<FRef> Refs;
	auto AddSpins = [&Refs](const TCHAR* Block, TArray<FMantleVehicleSpinBoneDesc>& Spins)
	{
		for (int32 Index = 0; Index < Spins.Num(); ++Index)
		{
			Refs.Add({ &Spins[Index].Bone, FString::Printf(TEXT("%s spin bone %d"), Block, Index), EUse::Posed });
		}
	};
	auto AddGrips = [&Refs](const TCHAR* What, FMantleVehicleGripPairDesc& Pair)
	{
		Refs.Add({ &Pair.Left.Bone, FString::Printf(TEXT("%s left grip"), What), EUse::Grip });
		Refs.Add({ &Pair.Right.Bone, FString::Printf(TEXT("%s right grip"), What), EUse::Grip });
	};
	if (D.MovementType == EMantleVehicleMovementKind::Plane)
	{
		for (int32 Index = 0; Index < D.Flight.Aerofoils.Num(); ++Index)
		{
			if (!D.Flight.Aerofoils[Index].Bone.IsNone())
			{
				Refs.Add({ &D.Flight.Aerofoils[Index].Bone, FString::Printf(TEXT("aerofoil %d"), Index), EUse::Posed });
			}
		}
		for (int32 Index = 0; Index < D.Flight.Thrusters.Num(); ++Index)
		{
			if (!D.Flight.Thrusters[Index].Bone.IsNone())
			{
				Refs.Add({ &D.Flight.Thrusters[Index].Bone, FString::Printf(TEXT("thruster %d"), Index), EUse::Reference });
			}
		}
		AddSpins(TEXT("flight"), D.Flight.SpinBones);
		if (D.Flight.bPilotGrips)
		{
			AddGrips(TEXT("pilot's"), D.Flight.PilotGrips);
		}
	}
	if (D.MovementType == EMantleVehicleMovementKind::Helicopter)
	{
		for (int32 Index = 0; Index < D.Helicopter.Rotors.Num(); ++Index)
		{
			if (!D.Helicopter.Rotors[Index].Bone.IsNone())
			{
				Refs.Add({ &D.Helicopter.Rotors[Index].Bone, FString::Printf(TEXT("rotor %d"), Index), EUse::Reference });
			}
		}
		AddSpins(TEXT("helicopter"), D.Helicopter.SpinBones);
		if (D.Helicopter.bPilotGrips)
		{
			AddGrips(TEXT("pilot's"), D.Helicopter.PilotGrips);
		}
	}
	if (D.bRider)
	{
		Refs.Add({ &D.Rider.ForkBone, TEXT("rider's fork"), EUse::Fork });
		AddGrips(TEXT("handlebar"), D.Rider.Handlebar);
		AddGrips(TEXT("foot peg"), D.Rider.FootPegs);
	}

	for (const FRef& Ref : Refs)
	{
		if (Ref.Name->IsNone())
		{
			// A grip left to the cook: the body's root bone.
			if (Ref.Use == EUse::Grip)
			{
				*Ref.Name = Bones[0].Name;
				continue;
			}
		}
		const int32 Bone = FindBone(*Ref.Name);
		if (Bone == INDEX_NONE)
		{
			return FailVehicle(OutError, FString::Printf(TEXT("that vehicle's %s names bone %s, which is not in its body"), *Ref.Pointer, *Ref.Name->ToString()));
		}
		if ((Ref.Use == EUse::Posed || Ref.Use == EUse::Fork) && Bone == 0)
		{
			return FailVehicle(OutError, FString::Printf(TEXT("that vehicle's %s names the root bone %s, which carries the chassis; give it a bone of its own under the root"),
				*Ref.Pointer, *Ref.Name->ToString()));
		}
		if (Ref.Use == EUse::Posed && CarriesWheel.Contains(Bone))
		{
			return FailVehicle(OutError, FString::Printf(TEXT("that vehicle's %s names bone %s, which is a wheel's or carries one, so the wheel would turn with it; give it a bone of its own"),
				*Ref.Pointer, *Ref.Name->ToString()));
		}
		if ((Ref.Use == EUse::Fork || Ref.Use == EUse::Grip) && WheelBones.Contains(Bone))
		{
			return FailVehicle(OutError, FString::Printf(TEXT("that vehicle's %s names bone %s, a wheel's bone, which spins"), *Ref.Pointer, *Ref.Name->ToString()));
		}
		// The body's own spelling, which the skeleton and the simulation modules use.
		*Ref.Name = Bones[Bone].Name;
	}
	return true;
}

bool FMantleVehicleAssetReader::Read(const TConstArrayView<uint8> Data, FMantleVehicleAsset& Out, FString& OutError)
{
	using namespace MantleVehicleAssetPrivate;
	using MantleCookedFormat::FCursor;

	Out = FMantleVehicleAsset();
	FCursor Cursor(Data);
	if (!Cursor.Magic("MVH1"))
	{
		return FailVehicle(OutError, TEXT("that asset is not a Mantle vehicle"));
	}
	const uint32 Version = Cursor.U32();
	if (Cursor.Failed())
	{
		return FailVehicle(OutError, TEXT("that vehicle is incomplete (header)"));
	}
	if (Version != SupportedVersion)
	{
		return FailVehicle(OutError, FString::Printf(
			TEXT("that vehicle was cooked for a newer version of Mantle (format %u, this build reads %u)"), Version, SupportedVersion));
	}

	const uint32 DocumentBytes = Cursor.U32();
	if (Cursor.Failed() || DocumentBytes < 1 || DocumentBytes > static_cast<uint32>(MaxDocumentBytes) || !Cursor.Has(DocumentBytes))
	{
		return FailVehicle(OutError, TEXT("that vehicle is incomplete or corrupt (document)"));
	}
	const TConstArrayView<uint8> Document(Data.GetData() + Cursor.Offset(), static_cast<int32>(DocumentBytes));
	Cursor.Skip(DocumentBytes);

	const uint32 BodyBytes = Cursor.U32();
	if (Cursor.Failed() || BodyBytes < 1 || !Cursor.Has(BodyBytes))
	{
		return FailVehicle(OutError, TEXT("that vehicle is incomplete or corrupt (body)"));
	}
	const TConstArrayView<uint8> BodyData(Data.GetData() + Cursor.Offset(), static_cast<int32>(BodyBytes));
	Cursor.Skip(BodyBytes);
	if (Cursor.Failed() || Cursor.Remaining() != 0)
	{
		return FailVehicle(OutError, TEXT("that vehicle has bytes after its body"));
	}

	FString Why;
	if (!ParseDocument(Document, Out.Desc, Why))
	{
		return FailVehicle(OutError, FString::Printf(TEXT("that vehicle's document is not valid: %s"), *Why));
	}

	FMantleSkinnedMeshReadResult Body = FMantleSkinnedMeshReader::Read(BodyData);
	if (!Body.bSuccess)
	{
		return FailVehicle(OutError, FString::Printf(TEXT("that vehicle's body is not valid: %s"), *Body.Error));
	}
	if (Body.Header.Role != EMantleSkinnedMeshRole::VehicleBody)
	{
		return FailVehicle(OutError, TEXT("that vehicle's body is not a vehicle body"));
	}
	if (Body.Mesh.Bones.Num() > FMantleVehicleLimits::MaxBones || !Body.Mesh.HasReferencePose())
	{
		return FailVehicle(OutError, FString::Printf(TEXT("that vehicle's body has %d bones (at most %d, with a reference pose)"),
			Body.Mesh.Bones.Num(), FMantleVehicleLimits::MaxBones));
	}
	Out.Body = MoveTemp(Body.Mesh);

	// The reference pose in component space: each bone's local transform on its parent's.
	const TArray<FMantleSkinnedMeshBone>& Bones = Out.Body.Bones;
	TArray<FTransform> Component;
	Component.SetNum(Bones.Num());
	for (int32 Bone = 0; Bone < Bones.Num(); ++Bone)
	{
		const FTransform& Local = Out.Body.ReferencePose[Bone];
		Component[Bone] = Bones[Bone].ParentIndex >= 0 ? Local * Component[Bones[Bone].ParentIndex] : Local;
	}
	Out.RootBoneTransform = Component[0];

	for (int32 Wheel = 0; Wheel < Out.Desc.Wheels.Num(); ++Wheel)
	{
		FMantleVehicleWheelDesc& Desc = Out.Desc.Wheels[Wheel];
		int32 Found = INDEX_NONE;
		for (int32 Bone = 0; Bone < Bones.Num() && Found == INDEX_NONE; ++Bone)
		{
			if (Bones[Bone].Name == Desc.Bone)
			{
				Found = Bone;
			}
		}
		if (Found <= 0)
		{
			return FailVehicle(OutError, FString::Printf(TEXT("that vehicle's wheel %d names bone %s, which is %s"), Wheel, *Desc.Bone.ToString(),
				Found == 0 ? TEXT("the root") : TEXT("not in its body")));
		}
		// The body's own spelling, which the skeleton and the simulation modules use.
		Desc.Bone = Bones[Found].Name;
		Out.WheelPositions.Add(Component[Found].GetLocation());
	}

	// #435 (b): every bone the linkage names is the body's (in its spelling), and none it moves is the root, a wheel
	// bone or a bone a wheel hangs from: the cook refuses those (resolveLinkage), and the wheels are the simulation's.
	auto FindBone = [&Bones](const FName Name)
	{
		return Bones.IndexOfByPredicate([Name](const FMantleSkinnedMeshBone& Bone) { return Bone.Name == Name; });
	};
	TSet<int32> CarriesWheel;
	for (const FMantleVehicleWheelDesc& Wheel : Out.Desc.Wheels)
	{
		for (int32 Bone = FindBone(Wheel.Bone); Bone != INDEX_NONE; Bone = Bones[Bone].ParentIndex)
		{
			CarriesWheel.Add(Bone);
		}
	}
	for (int32 Index = 0; Index < Out.Desc.Linkage.Num(); ++Index)
	{
		FMantleVehicleLinkageRule& Rule = Out.Desc.Linkage[Index];
		for (FName* Name : { &Rule.Bone, &Rule.End, &Rule.Target, &Rule.Up, &Rule.Source })
		{
			if (Name->IsNone())
			{
				continue;
			}
			const int32 Bone = FindBone(*Name);
			if (Bone == INDEX_NONE)
			{
				return FailVehicle(OutError, FString::Printf(TEXT("that vehicle's linkage rule %d names bone %s, which is not in its body"), Index,
					*Name->ToString()));
			}
			*Name = Bones[Bone].Name;
			if ((Name == &Rule.Bone || Name == &Rule.End) && CarriesWheel.Contains(Bone))
			{
				return FailVehicle(OutError, FString::Printf(TEXT("that vehicle's linkage rule %d moves bone %s, which is the root or carries a wheel"),
					Index, *Name->ToString()));
			}
		}
	}

	// #546: every bone the car clips turn (the steering wheel's, the doors', the trunk lid's) is the body's (in its spelling), and
	// none is the root, a wheel bone or a bone a wheel hangs from: the cook refuses those (resolveCarParts).
	for (FName* Name : { &Out.Desc.SteeringWheelBone, &Out.Desc.SeatDoor.Bone, &Out.Desc.PassengerSeatDoor.Bone, &Out.Desc.TrunkLid.Bone })
	{
		if (Name->IsNone())
		{
			continue;
		}
		const int32 Bone = FindBone(*Name);
		if (Bone == INDEX_NONE)
		{
			return FailVehicle(OutError, FString::Printf(TEXT("that vehicle turns bone %s with the car clips, which is not in its body"), *Name->ToString()));
		}
		if (Bone == 0 || CarriesWheel.Contains(Bone))
		{
			return FailVehicle(OutError, FString::Printf(TEXT("that vehicle turns bone %s with the car clips, which is the root or carries a wheel"),
				*Name->ToString()));
		}
		*Name = Bones[Bone].Name;
	}

	// #612: every bone a contact point is on is the body's (in its spelling): the cook refuses others (resolveContacts), and a
	// wheel's the rules already have.
	for (FMantleVehicleSeatContactsDesc* Contacts : { &Out.Desc.SeatContacts, &Out.Desc.PassengerSeatContacts })
	{
		for (FMantleVehicleContactDesc* Point : { &Contacts->Pelvis, &Contacts->LeftHand, &Contacts->RightHand, &Contacts->LeftFoot, &Contacts->RightFoot })
		{
			if (Point->Bone.IsNone())
			{
				continue;
			}
			const int32 Bone = FindBone(Point->Bone);
			if (Bone == INDEX_NONE)
			{
				return FailVehicle(OutError, FString::Printf(TEXT("that vehicle puts a contact point on bone %s, which is not in its body"),
					*Point->Bone.ToString()));
			}
			Point->Bone = Bones[Bone].Name;
		}
	}

	// #435 item 2: every part rides a bone of the body (any one), named in the body's spelling for the socket it attaches to.
	for (int32 Index = 0; Index < Out.Desc.Parts.Num(); ++Index)
	{
		FMantleVehiclePartDesc& Part = Out.Desc.Parts[Index];
		const int32 Bone = FindBone(Part.Bone);
		if (Bone == INDEX_NONE)
		{
			return FailVehicle(OutError, FString::Printf(TEXT("that vehicle's part %d rides bone %s, which is not in its body"), Index,
				*Part.Bone.ToString()));
		}
		Part.Bone = Bones[Bone].Name;
	}

	// #821: every bone the movement blocks and the rider name is the body's (in its spelling); the cook refuses the rest (resolveMovement).
	if (!ResolveMovementBones(Out.Desc, Out.Body.Bones, Why))
	{
		return FailVehicle(OutError, Why);
	}
	return true;
}

TSharedPtr<const FMantleVehicleBodyCollision> FMantleVehicleBodyCollision::FromBody(const FMantleSkinnedMesh& Body, const TConstArrayView<FName> WheelBones)
{
	if (Body.LODs.Num() == 0)
	{
		return nullptr;
	}
	const FMantleSkinnedMeshLOD& Lod = Body.LODs[0];
	const int32 NumVertices = Lod.Positions.Num();
	const int32 Slots = Lod.NumInfluences;
	if (NumVertices == 0 || Lod.Indices.Num() < 3 || Slots <= 0
		|| Lod.InfluenceBones.Num() < NumVertices * Slots || Lod.InfluenceWeights.Num() < NumVertices * Slots)
	{
		return nullptr;
	}

	TArray<bool> BoneIsWheel;
	BoneIsWheel.Init(false, Body.Bones.Num());
	for (int32 Bone = 0; Bone < Body.Bones.Num(); ++Bone)
	{
		BoneIsWheel[Bone] = WheelBones.Contains(Body.Bones[Bone].Name);
	}

	// A vertex is a wheel's when the bone that moves it most is a wheel bone (the first of equal weights wins, as the cook reads them).
	TArray<bool> WheelVertex;
	WheelVertex.Init(false, NumVertices);
	for (int32 Vertex = 0; Vertex < NumVertices; ++Vertex)
	{
		int32 Heaviest = 0;
		for (int32 Slot = 1; Slot < Slots; ++Slot)
		{
			if (Lod.InfluenceWeights[Vertex * Slots + Slot] > Lod.InfluenceWeights[Vertex * Slots + Heaviest])
			{
				Heaviest = Slot;
			}
		}
		const int32 Bone = Lod.InfluenceBones[Vertex * Slots + Heaviest];
		WheelVertex[Vertex] = BoneIsWheel.IsValidIndex(Bone) && BoneIsWheel[Bone];
	}

	TSharedRef<FMantleVehicleBodyCollision> Out = MakeShared<FMantleVehicleBodyCollision>();
	TArray<int32> Remap;
	Remap.Init(INDEX_NONE, NumVertices);
	for (int32 First = 0; First + 2 < Lod.Indices.Num(); First += 3)
	{
		const uint32 Raw[3] = { Lod.Indices[First], Lod.Indices[First + 1], Lod.Indices[First + 2] };
		if (Raw[0] >= static_cast<uint32>(NumVertices) || Raw[1] >= static_cast<uint32>(NumVertices) || Raw[2] >= static_cast<uint32>(NumVertices))
		{
			continue;
		}
		const int32 Corners[3] = { static_cast<int32>(Raw[0]), static_cast<int32>(Raw[1]), static_cast<int32>(Raw[2]) };
		if (WheelVertex[Corners[0]] || WheelVertex[Corners[1]] || WheelVertex[Corners[2]])
		{
			continue;
		}
		const FVector3f& A = Lod.Positions[Corners[0]];
		const FVector3f& B = Lod.Positions[Corners[1]];
		const FVector3f& C = Lod.Positions[Corners[2]];
		if (FVector3f::CrossProduct(B - A, C - A).SizeSquared() <= UE_SMALL_NUMBER)
		{
			continue;
		}
		for (const int32 Vertex : Corners)
		{
			if (Remap[Vertex] == INDEX_NONE)
			{
				Remap[Vertex] = Out->Positions.Add(Lod.Positions[Vertex]);
			}
			Out->Indices.Add(Remap[Vertex]);
		}
	}
	if (Out->Indices.Num() == 0)
	{
		return nullptr;
	}
	return Out;
}

UMantleBuiltVehicle* UMantleBuiltVehicle::Build(const FMantleVehicleAsset& Asset, UObject* Outer, FString* OutError)
{
	check(IsInGameThread());
	auto Fail = [OutError](const FString& Why) -> UMantleBuiltVehicle*
	{
		if (OutError)
		{
			*OutError = Why;
		}
		return nullptr;
	};
	const FMantleSkinnedMesh& Body = Asset.Body;
	if (Body.Bones.Num() == 0 || Body.ReferencePose.Num() != Body.Bones.Num())
	{
		return Fail(TEXT("the body has no bones or no reference pose"));
	}
	UObject* Owner = Outer ? Outer : GetTransientPackage();

	// A skeleton of the body's own: its bone table and reference pose, made the way the engine
	// makes a skeleton from its first mesh.
	FReferenceSkeleton Reference;
	{
		FReferenceSkeletonModifier Modifier(Reference, nullptr);
		for (int32 Bone = 0; Bone < Body.Bones.Num(); ++Bone)
		{
			const FName Name = Body.Bones[Bone].Name;
			Modifier.Add(FMeshBoneInfo(Name, Name.ToString(), Body.Bones[Bone].ParentIndex), Body.ReferencePose[Bone]);
		}
	}
	USkeletalMesh* Template = NewObject<USkeletalMesh>(GetTransientPackage(), NAME_None, RF_Transient);
	Template->SetRefSkeleton(Reference);
	USkeleton* Skeleton = NewObject<USkeleton>(Owner, NAME_None, RF_Transient);
	Skeleton->MergeAllBonesToBoneTree(Template, /*bShowProgress*/ false);

	FMantleSkinnedMeshBuildParams Params;
	Params.Skeleton = Skeleton;
	Params.MaxLODs = 1;
	Params.Outer = Owner;
	// Nothing reads a vehicle body back: a cooked client drops the CPU copies once the GPU has them.
	Params.bKeepCPUData = false;
	FString Error;
	USkeletalMesh* Mesh = FMantleRuntimeSkeletalMeshBuilder::Build(Body, Params, &Error);
	if (!Mesh)
	{
		return Fail(FString::Printf(TEXT("the body could not be built: %s"), *Error));
	}

	UMantleBuiltVehicle* Vehicle = NewObject<UMantleBuiltVehicle>(Owner, NAME_None, RF_Transient);
	Vehicle->Mesh = Mesh;
	Vehicle->Skeleton = Skeleton;
	Vehicle->Desc = Asset.Desc;
	Vehicle->WheelPositions = Asset.WheelPositions;
	Vehicle->RootBoneTransform = Asset.RootBoneTransform;
	TArray<FName> WheelBones;
	for (const FMantleVehicleWheelDesc& Wheel : Asset.Desc.Wheels)
	{
		WheelBones.Add(Wheel.Bone);
	}
	Vehicle->BodyCollision = FMantleVehicleBodyCollision::FromBody(Body, WheelBones);
	return Vehicle;
}
