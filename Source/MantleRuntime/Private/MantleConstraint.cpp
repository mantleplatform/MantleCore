// Copyright Joshua Eger. All Rights Reserved.
#include "MantleConstraint.h"
#include "MantlePlaceLimits.h"
#include "MantleRuntime.h"

#include "Chaos/ChaosConstraintSettings.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Math/RotationMatrix.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/ConstraintInstance.h"
#include "PhysicsEngine/PhysicsConstraintComponent.h"

// ---- FMantleConstraints ------------------------------------------------------------------------

namespace
{
	/** Components of a direction this close to zero are written as zero: an axis read back from the engine's floats. */
	constexpr double ConstraintAxisSnap = 1e-6;

	/** How close two engine angles or distances read back from floats count as the same. */
	constexpr double ConstraintReadTolerance = 1e-3;

	/** Whether Type turns about Axis (a hinge) rather than moving along it (a slider): its motor is an angular one. */
	bool IsAngularMotorType(EMantleConstraintType Type)
	{
		return Type == EMantleConstraintType::Hinge;
	}

	/**
	 * The motions, limits, collision and break thresholds Props asks of the engine: the one place both
	 * a runtime joint and an authoring build's engine constraint take them from, so they cannot differ.
	 * The motor is ApplyConstraintMotor's.
	 */
	void ApplyConstraintProfile(FConstraintInstance& Instance, const FMantleConstraintProps& Props, const FMantleConstraintFrames& Frames)
	{
		switch (Props.Type)
		{
		case EMantleConstraintType::Hinge:
			Instance.SetLinearLimits(LCM_Locked, LCM_Locked, LCM_Locked, 0.0f);
			// The hinge axis is each frame's X: the engine's twist. The two swings are locked.
			Instance.SetAngularSwing1Limit(ACM_Locked, 0.0f);
			Instance.SetAngularSwing2Limit(ACM_Locked, 0.0f);
			Instance.SetAngularTwistLimit(Props.bLimited ? ACM_Limited : ACM_Free, static_cast<float>(Frames.HalfRange));
			break;
		case EMantleConstraintType::Slider:
			// The slider axis is each frame's X. The engine limits it symmetrically, by HalfRange either
			// way of entityB's anchor, which MakeFrames moved to the middle of [min, max].
			Instance.SetLinearLimits(Props.bLimited ? LCM_Limited : LCM_Free, LCM_Locked, LCM_Locked,
				Props.bLimited ? static_cast<float>(Frames.HalfRange) : 0.0f);
			Instance.SetAngularSwing1Limit(ACM_Locked, 0.0f);
			Instance.SetAngularSwing2Limit(ACM_Locked, 0.0f);
			Instance.SetAngularTwistLimit(ACM_Locked, 0.0f);
			break;
		case EMantleConstraintType::Ball:
		{
			// The anchors held together; entityA turns about them. Both swings limited to one angle are a
			// round cone about each frame's X, the ball's axis; the twist turns about that axis.
			Instance.SetLinearLimits(LCM_Locked, LCM_Locked, LCM_Locked, 0.0f);
			const EAngularConstraintMotion Swing = Props.SwingLimit.IsSet() ? ACM_Limited : ACM_Free;
			const float SwingLimit = Props.SwingLimit.IsSet() ? static_cast<float>(Props.SwingLimit.GetValue()) : 0.0f;
			Instance.SetAngularSwing1Limit(Swing, SwingLimit);
			Instance.SetAngularSwing2Limit(Swing, SwingLimit);
			const double Twist = Props.TwistLimit.Get(0.0);
			Instance.SetAngularTwistLimit(!Props.TwistLimit.IsSet() ? ACM_Free : (Twist > 0.0 ? ACM_Limited : ACM_Locked),
				static_cast<float>(Twist));
			break;
		}
		case EMantleConstraintType::Rope:
		{
			// Every linear axis limited to one length is a sphere around the other anchor: a rope that
			// goes slack but never stretches. A rope of no length holds the anchors together.
			const bool bSlack = Frames.RopeLength >= FMantleConstraints::MinRopeLength;
			const ELinearConstraintMotion Motion = bSlack ? LCM_Limited : LCM_Locked;
			Instance.SetLinearLimits(Motion, Motion, Motion, bSlack ? static_cast<float>(Frames.RopeLength) : 0.0f);
			Instance.SetAngularSwing1Limit(ACM_Free, 0.0f);
			Instance.SetAngularSwing2Limit(ACM_Free, 0.0f);
			Instance.SetAngularTwistLimit(ACM_Free, 0.0f);
			break;
		}
		case EMantleConstraintType::Weld:
		default:
			Instance.SetLinearLimits(LCM_Locked, LCM_Locked, LCM_Locked, 0.0f);
			Instance.SetAngularSwing1Limit(ACM_Locked, 0.0f);
			Instance.SetAngularSwing2Limit(ACM_Locked, 0.0f);
			Instance.SetAngularTwistLimit(ACM_Locked, 0.0f);
			break;
		}
		// Two joined bodies touching would fight the joint.
		Instance.SetDisableCollision(true);

		const bool bBreakable = Props.BreakForce.IsSet();
		Instance.SetLinearBreakable(bBreakable,
			bBreakable ? static_cast<float>(Props.BreakForce.GetValue() * FMantleConstraints::UnrealForcePerNewton) : 0.0f);
		// A rope's and a ball's angles are (mostly) free, so nothing twists them; a weld's, a hinge's or a
		// slider's twist breaks it too.
		const bool bAngularBreakable = bBreakable && Props.Type != EMantleConstraintType::Rope && Props.Type != EMantleConstraintType::Ball;
		Instance.SetAngularBreakable(bAngularBreakable,
			bAngularBreakable ? static_cast<float>(Props.BreakForce.GetValue() * FMantleConstraints::UnrealTorquePerNewtonMeter) : 0.0f);
	}

	/**
	 * Props' motor on the engine constraint, or every drive off without one. Works on a constraint
	 * before or after it is made: each engine setter keeps its value in the profile and passes the
	 * drive on to the physics joint when there is one (FConstraintInstance::SetLinearDriveParams and
	 * the rest, ConstraintInstance.cpp).
	 *
	 * Force mode, never the engine's default acceleration mode: in acceleration mode the solver divides
	 * a drive's MaxForce by the joint's inverse mass (FAxisConstraintDatas::SetMaxForce,
	 * PBDJointCachedSolverGaussSeidel.cpp), so it would be an acceleration, not the newtons the creator
	 * gave. Stiffness and damping are then per unit of mass, so they come from Inertia (MotorGains).
	 * A hinge's drive is the engine's twist drive in TwistAndSwing mode: SLERP does not work with any
	 * angular axis locked, which a hinge's swings are (FAngularDriveConstraint::AngularDriveMode).
	 *
	 * The engine drives entityB's frame relative to entityA's (Frame2 in Frame1's space: the solver's
	 * ConnectorXs[1] against ConnectorXs[0] + ConnectorRs[0] * the target), and entityA is Frame1. So
	 * the frames' offset is the middle of the limits minus entityA's own turn or travel from where it
	 * was made, and its speed the opposite of entityA's.
	 */
	void ApplyConstraintMotor(FConstraintInstance& Instance, const FMantleConstraintProps& Props, const FMantleConstraintFrames& Frames, double Inertia)
	{
		Instance.SetLinearPositionDrive(false, false, false);
		Instance.SetLinearVelocityDrive(false, false, false);
		Instance.SetAngularDriveMode(EAngularDriveMode::TwistAndSwing);
		Instance.SetOrientationDriveTwistAndSwing(false, false);
		Instance.SetAngularVelocityDriveTwistAndSwing(false, false);
		if (!Props.Motor.IsSet() || (Props.Type != EMantleConstraintType::Hinge && Props.Type != EMantleConstraintType::Slider))
		{
			return;
		}
		const FMantleConstraintMotor& Motor = Props.Motor.GetValue();
		const bool bPosition = Motor.Mode == EMantleMotorMode::Position;
		const FMantleMotorGains Gains = FMantleConstraints::MotorGains(Props.Type, Motor.Mode, Inertia);
		if (IsAngularMotorType(Props.Type))
		{
			Instance.SetAngularDriveAccelerationMode(false);
			Instance.SetAngularDriveParams(static_cast<float>(Gains.Stiffness), static_cast<float>(Gains.Damping),
				static_cast<float>(Motor.MaxForce * FMantleConstraints::UnrealTorquePerNewtonMeter));
			if (bPosition)
			{
				Instance.SetAngularOrientationTarget(FQuat(FVector::XAxisVector, FMath::DegreesToRadians(Frames.Middle - Motor.Target)));
				Instance.SetAngularVelocityTarget(FVector::ZeroVector);
				Instance.SetOrientationDriveTwistAndSwing(true, false);
			}
			else
			{
				// The engine's angular velocity target is in turns a second.
				Instance.SetAngularVelocityTarget(FVector(-Motor.Target / 360.0, 0.0, 0.0));
				Instance.SetAngularVelocityDriveTwistAndSwing(true, false);
			}
		}
		else
		{
			Instance.SetLinearDriveAccelerationMode(false);
			Instance.SetLinearDriveParams(static_cast<float>(Gains.Stiffness), static_cast<float>(Gains.Damping),
				static_cast<float>(Motor.MaxForce * FMantleConstraints::UnrealForcePerNewton));
			if (bPosition)
			{
				Instance.SetLinearPositionTarget(FVector(Frames.Middle - Motor.Target, 0.0, 0.0));
				Instance.SetLinearVelocityTarget(FVector::ZeroVector);
				Instance.SetLinearPositionDrive(true, false, false);
			}
			else
			{
				Instance.SetLinearVelocityTarget(FVector(-Motor.Target, 0.0, 0.0));
				Instance.SetLinearVelocityDrive(true, false, false);
			}
		}
	}

	/**
	 * What one body puts against a motor: its mass (kg) for a slider, or for a hinge its inertia about
	 * the hinge line (kg*cm^2): its own about its center of mass along Axis, plus mass times the
	 * squared distance of that center from the line through Anchor along Axis. 0 without a body.
	 */
	double MotorBodyInertia(UPrimitiveComponent* Component, bool bAngular, const FVector& Anchor, const FVector& Axis)
	{
		const FBodyInstance* Body = Component ? Component->GetBodyInstance() : nullptr;
		if (!Body)
		{
			return 0.0;
		}
		double Mass = 0.0;
		double AboutCenter = 0.0;
		FVector Center = FVector::ZeroVector;
		if (Body->IsValidBodyInstance())
		{
			Mass = Body->GetBodyMass();
			const FTransform MassSpace = Body->GetMassSpaceToWorldSpace();
			const FVector LocalAxis = MassSpace.InverseTransformVectorNoScale(Axis);
			const FVector Tensor = Body->GetBodyInertiaTensor();
			AboutCenter = Tensor.X * LocalAxis.X * LocalAxis.X + Tensor.Y * LocalAxis.Y * LocalAxis.Y + Tensor.Z * LocalAxis.Z * LocalAxis.Z;
			Center = MassSpace.GetLocation();
		}
		else
		{
			// An authoring build's body before the engine has made it: the mass its collision gives, and a
			// solid sphere of its bounds for the turn. Only the editor's preview of the motor uses it.
			Mass = Component->CalculateMass();
			const FBoxSphereBounds Bounds = Component->CalcBounds(Component->GetComponentTransform());
			AboutCenter = 0.4 * Mass * FMath::Square(Bounds.SphereRadius);
			Center = Bounds.Origin;
		}
		if (!bAngular)
		{
			return Mass;
		}
		const FVector Offset = Center - Anchor;
		return AboutCenter + Mass * (Offset - (Offset | Axis) * Axis).SizeSquared();
	}

	/** What a motor between A and B (null: the world) moves: the two bodies' inertias in series, as the joint's own effective mass. */
	double MotorInertia(EMantleConstraintType Type, const FMantleConstraintFrames& Frames, UPrimitiveComponent* A, UPrimitiveComponent* B)
	{
		const bool bAngular = IsAngularMotorType(Type);
		const double InertiaA = MotorBodyInertia(A, bAngular, Frames.AnchorA, Frames.PrimaryA);
		const double InertiaB = B ? MotorBodyInertia(B, bAngular, Frames.AnchorB, Frames.PrimaryB) : 0.0;
		if (InertiaA > 0.0 && InertiaB > 0.0)
		{
			return 1.0 / (1.0 / InertiaA + 1.0 / InertiaB);
		}
		return FMath::Max(InertiaA, InertiaB);
	}

	/** A world-space joint frame in a body's own space, unscaled, as the engine keeps a constraint's frames. */
	FTransform ConstraintLocalFrame(const FTransform& Body, const FVector& Anchor, const FVector& Primary, const FVector& Secondary)
	{
		FTransform Unscaled = Body;
		Unscaled.RemoveScaling();
		const FVector LocalPrimary = Unscaled.InverseTransformVectorNoScale(Primary);
		const FVector LocalSecondary = Unscaled.InverseTransformVectorNoScale(Secondary);
		return FTransform(LocalPrimary, LocalSecondary, LocalPrimary ^ LocalSecondary, Unscaled.InverseTransformPosition(Anchor));
	}

	bool IsFiniteConstraintVector(const FVector& Value)
	{
		return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y) && FMath::IsFinite(Value.Z);
	}

	FVector SnapConstraintAxis(const FVector& Axis)
	{
		FVector Snapped = Axis.GetSafeNormal();
		for (int32 Component = 0; Component < 3; ++Component)
		{
			if (FMath::Abs(Snapped[Component]) < ConstraintAxisSnap)
			{
				Snapped[Component] = 0.0;
			}
		}
		return Snapped;
	}

	/** Whether Type takes an axis: a hinge turns about it, a slider moves along it, a ball twists about it. */
	bool TakesConstraintAxis(EMantleConstraintType Type)
	{
		return Type == EMantleConstraintType::Hinge || Type == EMantleConstraintType::Slider || Type == EMantleConstraintType::Ball;
	}
}

double FMantleConstraints::MotorTargetMost(EMantleConstraintType Type, EMantleMotorMode Mode)
{
	if (IsAngularMotorType(Type))
	{
		return Mode == EMantleMotorMode::Position ? FMantlePlaceLimits::MaxHingeAngle : FMantlePlaceLimits::MaxMotorAngularSpeed;
	}
	return Mode == EMantleMotorMode::Position ? FMantlePlaceLimits::MaxSliderTravel : FMantlePlaceLimits::MaxMotorLinearSpeed;
}

FMantleMotorGains FMantleConstraints::MotorGains(EMantleConstraintType Type, EMantleMotorMode Mode, double Inertia)
{
	// A body the engine reports no mass for still gets a drive that does something.
	const double Moved = Inertia > 0.0 ? Inertia : 1.0;
	FMantleMotorGains Gains;
	if (Mode == EMantleMotorMode::Position)
	{
		// A critically damped spring at MotorPositionResponseHz: it reaches its target without swinging past.
		const double Omega = 2.0 * UE_DOUBLE_PI * MotorPositionResponseHz;
		Gains.Stiffness = Moved * Omega * Omega;
		Gains.Damping = 2.0 * Moved * Omega;
	}
	else
	{
		Gains.Damping = Moved * MotorSpeedResponse;
	}
	// Chaos scales what it is given (ConstraintSettings, PhysicsCore): undone so the gains are the ones above.
	const double StiffnessScale = IsAngularMotorType(Type) ? Chaos::ConstraintSettings::AngularDriveStiffnessScale()
		: Chaos::ConstraintSettings::LinearDriveStiffnessScale();
	const double DampingScale = IsAngularMotorType(Type) ? Chaos::ConstraintSettings::AngularDriveDampingScale()
		: Chaos::ConstraintSettings::LinearDriveDampingScale();
	Gains.Stiffness /= StiffnessScale > 0.0 ? StiffnessScale : 1.0;
	Gains.Damping /= DampingScale > 0.0 ? DampingScale : 1.0;
	return Gains;
}

bool FMantleConstraints::CheckMotor(EMantleConstraintType Type, bool bLimited, double LimitMin, double LimitMax, const FMantleConstraintMotor& Motor,
	FString& OutError)
{
	const bool bHinge = IsAngularMotorType(Type);
	if (!(Motor.MaxForce > 0.0 && Motor.MaxForce <= FMantlePlaceLimits::MaxMotorForce))
	{
		OutError = FString::Printf(TEXT("MaxForce must be above 0 and at most %g %s, got %g"), FMantlePlaceLimits::MaxMotorForce,
			bHinge ? TEXT("newton-meters") : TEXT("newtons"), Motor.MaxForce);
		return false;
	}
	const double Most = MotorTargetMost(Type, Motor.Mode);
	const bool bPosition = Motor.Mode == EMantleMotorMode::Position;
	if (!(Motor.Target >= -Most && Motor.Target <= Most))
	{
		OutError = FString::Printf(TEXT("Target must be from -%g to %g %s, got %g"), Most, Most,
			bPosition ? (bHinge ? TEXT("degrees") : TEXT("cm")) : (bHinge ? TEXT("degrees a second") : TEXT("cm a second")), Motor.Target);
		return false;
	}
	if (bPosition && bLimited && (Motor.Target < LimitMin || Motor.Target > LimitMax))
	{
		OutError = FString::Printf(TEXT("Target %g is outside the joint's limits {%g, %g}"), Motor.Target, LimitMin, LimitMax);
		return false;
	}
	return true;
}

bool FMantleConstraints::CheckValues(const FMantleConstraintProps& Props, FString& OutError)
{
	if (!IsFiniteConstraintVector(Props.AnchorA) || (Props.AnchorB.IsSet() && !IsFiniteConstraintVector(Props.AnchorB.GetValue())))
	{
		OutError = TEXT("an anchor must be three finite numbers");
		return false;
	}
	if (TakesConstraintAxis(Props.Type) && (!IsFiniteConstraintVector(Props.Axis) || Props.Axis.IsZero()))
	{
		OutError = TEXT("Axis must be a finite direction, not (0, 0, 0)");
		return false;
	}
	if (Props.bLimited && (Props.Type == EMantleConstraintType::Hinge || Props.Type == EMantleConstraintType::Slider))
	{
		const bool bHinge = Props.Type == EMantleConstraintType::Hinge;
		const double Most = bHinge ? FMantlePlaceLimits::MaxHingeAngle : FMantlePlaceLimits::MaxSliderTravel;
		if (!(Props.LimitMin >= -Most && Props.LimitMin <= 0.0 && Props.LimitMax >= 0.0 && Props.LimitMax <= Most))
		{
			OutError = FString::Printf(TEXT("Limits must be {min, max} with -%g <= min <= 0 <= max <= %g %s, got {%g, %g}"),
				Most, Most, bHinge ? TEXT("degrees") : TEXT("cm"), Props.LimitMin, Props.LimitMax);
			return false;
		}
	}
	if (Props.SwingLimit.IsSet() && !(Props.SwingLimit.GetValue() > 0.0 && Props.SwingLimit.GetValue() <= FMantlePlaceLimits::MaxBallAngle))
	{
		OutError = FString::Printf(TEXT("SwingLimit must be above 0 and at most %g degrees, got %g"), FMantlePlaceLimits::MaxBallAngle,
			Props.SwingLimit.GetValue());
		return false;
	}
	if (Props.TwistLimit.IsSet() && !(Props.TwistLimit.GetValue() >= 0.0 && Props.TwistLimit.GetValue() <= FMantlePlaceLimits::MaxBallAngle))
	{
		OutError = FString::Printf(TEXT("TwistLimit must be from 0 to %g degrees, got %g"), FMantlePlaceLimits::MaxBallAngle, Props.TwistLimit.GetValue());
		return false;
	}
	if (Props.Motor.IsSet())
	{
		FString MotorError;
		if (!CheckMotor(Props.Type, Props.bLimited, Props.LimitMin, Props.LimitMax, Props.Motor.GetValue(), MotorError))
		{
			OutError = TEXT("Motor.") + MotorError;
			return false;
		}
	}
	if (Props.Length.IsSet() && !(Props.Length.GetValue() > 0.0 && Props.Length.GetValue() <= FMantlePlaceLimits::MaxRopeLength))
	{
		OutError = FString::Printf(TEXT("Length must be above 0 and at most %g cm, got %g"), FMantlePlaceLimits::MaxRopeLength, Props.Length.GetValue());
		return false;
	}
	if (Props.BreakForce.IsSet()
		&& !(Props.BreakForce.GetValue() > 0.0 && Props.BreakForce.GetValue() <= FMantlePlaceLimits::MaxConstraintBreakForce))
	{
		OutError = FString::Printf(TEXT("BreakForce must be above 0 and at most %g newtons, got %g"),
			FMantlePlaceLimits::MaxConstraintBreakForce, Props.BreakForce.GetValue());
		return false;
	}
	return true;
}

FMantleConstraintFrames FMantleConstraints::MakeFrames(const FMantleConstraintProps& Props, const FTransform& A, const FTransform* B)
{
	FMantleConstraintFrames Frames;
	Frames.AnchorA = A.TransformPosition(Props.AnchorA);
	if (Props.AnchorB.IsSet())
	{
		Frames.AnchorB = B ? B->TransformPosition(Props.AnchorB.GetValue()) : Props.AnchorB.GetValue();
	}
	else
	{
		Frames.AnchorB = Frames.AnchorA;
	}

	// A hinge turns about its axis, a slider moves along it and a ball twists about it: each frame's
	// X (the engine's twist axis, and its first linear axis). A weld and a rope take entityA's own X:
	// a weld locks every motion, a rope frees the angles, so any frame will do.
	FVector Primary = (TakesConstraintAxis(Props.Type) ? A.GetRotation().RotateVector(Props.Axis) : A.GetRotation().GetAxisX()).GetSafeNormal();
	if (Primary.IsZero())
	{
		Primary = FVector::ZAxisVector;
	}
	FVector Secondary;
	FVector Third;
	Primary.FindBestAxisVectors(Secondary, Third);
	Frames.PrimaryA = Primary;
	Frames.SecondaryA = Secondary;
	Frames.PrimaryB = Primary;
	Frames.SecondaryB = Secondary;

	// The engine limits a twist and a linear axis symmetrically, so entityB's frame is put at the
	// middle of [min, max]: turned about the hinge axis, or moved along the slider axis. The frames'
	// offset is then the middle minus entityA's own turn or travel from where it was made, and staying
	// within half the range either way of 0 is exactly min..max.
	if ((Props.Type == EMantleConstraintType::Hinge || Props.Type == EMantleConstraintType::Slider) && Props.bLimited)
	{
		Frames.Middle = 0.5 * (Props.LimitMin + Props.LimitMax);
		Frames.HalfRange = 0.5 * (Props.LimitMax - Props.LimitMin);
		if (Props.Type == EMantleConstraintType::Hinge)
		{
			Frames.SecondaryB = FQuat(Primary, FMath::DegreesToRadians(Frames.Middle)).RotateVector(Secondary);
		}
		else
		{
			Frames.AnchorB += Primary * Frames.Middle;
		}
	}
	if (Props.Type == EMantleConstraintType::Rope)
	{
		Frames.RopeLength = Props.Length.IsSet() ? Props.Length.GetValue() : FVector::Dist(Frames.AnchorA, Frames.AnchorB);
	}
	return Frames;
}

void FMantleConstraints::ConfigureEditorConstraint(UPhysicsConstraintComponent* Component, const FMantleConstraintProps& Props, AActor* ActorA,
	AActor* ActorB)
{
	if (!Component || !ActorA)
	{
		return;
	}
	const FTransform TransformA = ActorA->GetActorTransform();
	const FTransform TransformB = ActorB ? ActorB->GetActorTransform() : FTransform::Identity;
	FMantleConstraintFrames Frames = MakeFrames(Props, TransformA, ActorB ? &TransformB : nullptr);

	// The engine's constraint sits where its joint is, its X along a hinge's or a slider's axis (a
	// ball's twist axis); it makes both bodies' frames from that when the level plays
	// (UPhysicsConstraintComponent::UpdateConstraintFrames).
	Component->SetWorldLocationAndRotation(Frames.AnchorA, FRotationMatrix::MakeFromXY(Frames.PrimaryA, Frames.SecondaryA).ToQuat());
	Component->ConstraintActor1 = ActorA;
	Component->ConstraintActor2 = ActorB;
	FConstraintInstance& Instance = Component->ConstraintInstance;
	if (Props.Type == EMantleConstraintType::Slider && Props.bLimited && !FMath::IsNearlyZero(Frames.Middle))
	{
		// UpdateConstraintFrames makes both frames at the component's one location, and the engine has no
		// linear offset between them, so an engine constraint holds a slider only within the same distance
		// either way of where it starts. The range keeps its length, centered on the start.
		UE_LOG(LogMantle, Warning, TEXT("MANTLE_CONSTRAINT_EDITOR %s: slider limits [%g, %g] are not centered on where it starts; the editor's Physics Constraint holds +-%g cm instead."),
			*ActorA->GetActorNameOrLabel(), Props.LimitMin, Props.LimitMax, Frames.HalfRange);
		Frames.Middle = 0.0;
	}
	ApplyConstraintProfile(Instance, Props, Frames);
	// The middle of a hinge's limits, as the engine's own offset between the two frames: it turns
	// entityB's frame about the twist axis, as MakeFrames turns SecondaryB.
	Instance.AngularRotationOffset = (Props.Type == EMantleConstraintType::Hinge && Props.bLimited)
		? FQuat(FVector::XAxisVector, FMath::DegreesToRadians(Frames.Middle)).Rotator()
		: FRotator::ZeroRotator;
	ApplyConstraintMotor(Instance, Props, Frames,
		MotorInertia(Props.Type, Frames, Cast<UPrimitiveComponent>(ActorA->GetRootComponent()),
			ActorB ? Cast<UPrimitiveComponent>(ActorB->GetRootComponent()) : nullptr));
}

bool FMantleConstraints::ReadEditorConstraint(const UPhysicsConstraintComponent* Component, const AActor* ActorA, const AActor* ActorB,
	FMantleConstraintProps& OutProps, FString& OutProblem)
{
	OutProps = FMantleConstraintProps();
	OutProblem.Reset();
	if (!Component || !ActorA)
	{
		OutProblem = TEXT("joins nothing: set its Constraint Actor 1 to the physics object it holds.");
		return false;
	}
	const FConstraintInstance& Instance = Component->ConstraintInstance;
	const FConstraintProfileProperties& Profile = Instance.ProfileInstance;
	const FTransform Joint = Component->GetComponentTransform();

	// The frame's axes: linear X, Y, Z; twist turns about X, Swing 1 about Z, Swing 2 about Y.
	const FVector LocalAxes[3] = { FVector::XAxisVector, FVector::YAxisVector, FVector::ZAxisVector };
	const ELinearConstraintMotion Linear[3] = { Instance.GetLinearXMotion(), Instance.GetLinearYMotion(), Instance.GetLinearZMotion() };
	const bool bLinearLimited = Linear[0] == LCM_Limited && Linear[1] == LCM_Limited && Linear[2] == LCM_Limited;
	int32 LinearMoving = 0;
	int32 SliderAxis = INDEX_NONE;
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		if (Linear[Axis] != LCM_Locked)
		{
			++LinearMoving;
			SliderAxis = Axis;
		}
	}

	struct FAngularAxis
	{
		EAngularConstraintMotion Motion;
		double Limit;
		int32 Index;
	};
	const FAngularAxis Twist = { Instance.GetAngularTwistMotion(), Instance.GetAngularTwistLimit(), 0 };
	const FAngularAxis Swing1 = { Instance.GetAngularSwing1Motion(), Instance.GetAngularSwing1Limit(), 2 };
	const FAngularAxis Swing2 = { Instance.GetAngularSwing2Motion(), Instance.GetAngularSwing2Limit(), 1 };
	const FAngularAxis* const Angular[3] = { &Twist, &Swing1, &Swing2 };
	int32 AngularMoving = 0;
	const FAngularAxis* Turning = nullptr;
	for (const FAngularAxis* Axis : Angular)
	{
		if (Axis->Motion != ACM_Locked)
		{
			++AngularMoving;
			Turning = Axis;
		}
	}

	/** A direction in the constraint's frame, in entityA's own space (its rotation). */
	auto AxisInA = [&Joint, ActorA](const FVector& LocalAxis)
	{
		return SnapConstraintAxis(ActorA->GetActorQuat().Inverse().RotateVector(Joint.GetRotation().RotateVector(LocalAxis)));
	};
	/** The engine turns entityB's frame by its Angular Rotation Offset: the middle of a symmetric twist or swing, degrees about LocalAxis. */
	auto OffsetAbout = [&Instance](const FVector& LocalAxis)
	{
		return FMath::RadiansToDegrees(Instance.AngularRotationOffset.Quaternion().GetTwistAngle(LocalAxis));
	};

	if (bLinearLimited)
	{
		for (const FAngularAxis* Axis : Angular)
		{
			if (Axis->Motion != ACM_Free)
			{
				OutProblem = TEXT("is a rope (its linear motions all Limited) that also holds its angles. A Mantle rope lets both objects turn freely: set Swing 1, Swing 2 and Twist Motion to Free.");
				return false;
			}
		}
		const double Length = Instance.GetLinearLimit();
		if (!(Length > 0.0 && Length <= FMantlePlaceLimits::MaxRopeLength))
		{
			OutProblem = FString::Printf(TEXT("is a rope %g cm long; Mantle publishes ropes above 0 and at most %g cm. Set its Linear Limit Size in that range."),
				Length, FMantlePlaceLimits::MaxRopeLength);
			return false;
		}
		OutProps.Type = EMantleConstraintType::Rope;
		OutProps.Length = Length;
	}
	else if (LinearMoving == 1)
	{
		if (AngularMoving > 0)
		{
			OutProblem = TEXT("is a slider (one linear motion Free or Limited) that also turns. A Mantle slider moves along its axis only: set Swing 1, Swing 2 and Twist Motion to Locked.");
			return false;
		}
		OutProps.Type = EMantleConstraintType::Slider;
		OutProps.Axis = AxisInA(LocalAxes[SliderAxis]);
		if (Linear[SliderAxis] == LCM_Limited)
		{
			// The engine limits it the same distance either way of where it starts.
			const double Limit = Instance.GetLinearLimit();
			if (!(Limit > 0.0 && Limit <= FMantlePlaceLimits::MaxSliderTravel + ConstraintReadTolerance))
			{
				OutProblem = FString::Printf(TEXT("is a slider limited to %g cm either way; Mantle publishes limits above 0 and at most %g cm. Set its Linear Limit Size in that range, or its motion to Free."),
					Limit, FMantlePlaceLimits::MaxSliderTravel);
				return false;
			}
			OutProps.bLimited = true;
			OutProps.LimitMin = -FMath::Min(Limit, FMantlePlaceLimits::MaxSliderTravel);
			OutProps.LimitMax = FMath::Min(Limit, FMantlePlaceLimits::MaxSliderTravel);
		}
	}
	else if (LinearMoving == 0)
	{
		if (AngularMoving == 0)
		{
			OutProps.Type = EMantleConstraintType::Weld;
		}
		else if (AngularMoving == 1)
		{
			OutProps.Type = EMantleConstraintType::Hinge;
			const FVector& TurnAxis = LocalAxes[Turning->Index];
			OutProps.Axis = AxisInA(TurnAxis);
			if (Turning->Motion == ACM_Limited)
			{
				// The engine limits the turn symmetrically about its Angular Rotation Offset.
				const double Middle = OffsetAbout(TurnAxis);
				double LimitMin = Middle - Turning->Limit;
				double LimitMax = Middle + Turning->Limit;
				LimitMin = FMath::Abs(LimitMin) < ConstraintReadTolerance ? 0.0 : LimitMin;
				LimitMax = FMath::Abs(LimitMax) < ConstraintReadTolerance ? 0.0 : LimitMax;
				if (LimitMin > 0.0 || LimitMax < 0.0 || LimitMin < -FMantlePlaceLimits::MaxHingeAngle - ConstraintReadTolerance
					|| LimitMax > FMantlePlaceLimits::MaxHingeAngle + ConstraintReadTolerance)
				{
					OutProblem = FString::Printf(
						TEXT("is a hinge limited to %g..%g degrees from where it starts. Mantle publishes limits that hold the starting angle (0), within -%g..%g: change its limit or its Angular Rotation Offset."),
						LimitMin, LimitMax, FMantlePlaceLimits::MaxHingeAngle, FMantlePlaceLimits::MaxHingeAngle);
					return false;
				}
				OutProps.bLimited = true;
				OutProps.LimitMin = FMath::Max(LimitMin, -FMantlePlaceLimits::MaxHingeAngle);
				OutProps.LimitMax = FMath::Min(LimitMax, FMantlePlaceLimits::MaxHingeAngle);
			}
		}
		else if (Swing1.Motion != ACM_Locked && Swing2.Motion != ACM_Locked)
		{
			OutProps.Type = EMantleConstraintType::Ball;
			OutProps.Axis = AxisInA(FVector::XAxisVector);
			if (!Instance.AngularRotationOffset.Quaternion().Equals(FQuat::Identity, 1e-4))
			{
				OutProblem = TEXT("is a ball-and-socket joint with an Angular Rotation Offset. A Mantle ball joint's cone and twist are centered on where it starts: set its Angular Rotation Offset to 0, 0, 0.");
				return false;
			}
			if (Swing1.Motion != Swing2.Motion
				|| (Swing1.Motion == ACM_Limited && !FMath::IsNearlyEqual(Swing1.Limit, Swing2.Limit, ConstraintReadTolerance)))
			{
				OutProblem = TEXT("is a ball-and-socket joint whose two swings differ (an oval cone). Mantle publishes a round cone: give Swing 1 and Swing 2 the same Motion and the same limit.");
				return false;
			}
			if (Swing1.Motion == ACM_Limited)
			{
				if (!(Swing1.Limit > 0.0 && Swing1.Limit <= FMantlePlaceLimits::MaxBallAngle + ConstraintReadTolerance))
				{
					OutProblem = FString::Printf(TEXT("is a ball-and-socket joint whose swing limit is %g degrees; Mantle publishes above 0 and at most %g. Change its Swing 1 and Swing 2 limits, or set their Motion to Free."),
						Swing1.Limit, FMantlePlaceLimits::MaxBallAngle);
					return false;
				}
				OutProps.SwingLimit = FMath::Min(Swing1.Limit, FMantlePlaceLimits::MaxBallAngle);
			}
			if (Twist.Motion == ACM_Locked)
			{
				OutProps.TwistLimit = 0.0;
			}
			else if (Twist.Motion == ACM_Limited)
			{
				OutProps.TwistLimit = FMath::Clamp(Twist.Limit, 0.0, FMantlePlaceLimits::MaxBallAngle);
			}
		}
		else
		{
			OutProblem = TEXT("turns about two axes, one of them its twist. Mantle publishes a hinge (one of Twist, Swing 1 or Swing 2 Free or Limited, the rest Locked) or a ball-and-socket joint (Swing 1 and Swing 2 both Free or both Limited).");
			return false;
		}
	}
	else
	{
		OutProblem = TEXT("moves along more than one axis without being a rope. Mantle publishes a weld, a hinge or a ball-and-socket joint (Linear X, Y and Z Motion all Locked), a slider (one Free or Limited, the other two Locked) or a rope (all three Limited).");
		return false;
	}

	// A motor: a hinge's or a slider's drive on its own axis, and no other drive.
	const FLinearDriveConstraint& LinearDrive = Profile.LinearDrive;
	const FAngularDriveConstraint& AngularDrive = Profile.AngularDrive;
	const bool bLinearDrive = LinearDrive.IsPositionDriveEnabled() || LinearDrive.IsVelocityDriveEnabled();
	const bool bAngularDrive = AngularDrive.IsOrientationDriveEnabled() || AngularDrive.IsVelocityDriveEnabled();
	if (bLinearDrive || bAngularDrive)
	{
		const bool bSlider = OutProps.Type == EMantleConstraintType::Slider;
		const bool bHinge = OutProps.Type == EMantleConstraintType::Hinge;
		if ((!bSlider && !bHinge) || (bSlider && bAngularDrive) || (bHinge && bLinearDrive))
		{
			OutProblem = TEXT("has a drive (a motor) Mantle does not publish: Mantle motors drive a hinge about its axis or a slider along its axis. Turn the other drives off.");
			return false;
		}
		const FConstraintDrive* Drive = nullptr;
		double MaxForceUnits = UnrealForcePerNewton;
		bool bAccelerationMode = false;
		if (bSlider)
		{
			const FConstraintDrive* const Drives[3] = { &LinearDrive.XDrive, &LinearDrive.YDrive, &LinearDrive.ZDrive };
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				if (Axis != SliderAxis && (Drives[Axis]->bEnablePositionDrive || Drives[Axis]->bEnableVelocityDrive))
				{
					OutProblem = TEXT("has a linear drive on an axis it does not move along. A Mantle slider motor drives the slider's own axis: turn the other axes' drives off.");
					return false;
				}
			}
			Drive = Drives[SliderAxis];
			bAccelerationMode = LinearDrive.bAccelerationMode;
		}
		else
		{
			if (AngularDrive.AngularDriveMode != EAngularDriveMode::TwistAndSwing)
			{
				OutProblem = TEXT("has a SLERP angular drive, which does not turn a hinge (its other angles are Locked). Set its Angular Drive Mode to Twist and Swing.");
				return false;
			}
			const bool bTwistTurns = Turning->Index == 0;
			const FConstraintDrive& Other = bTwistTurns ? AngularDrive.SwingDrive : AngularDrive.TwistDrive;
			if (Other.bEnablePositionDrive || Other.bEnableVelocityDrive)
			{
				OutProblem = TEXT("has an angular drive on an axis it does not turn about. A Mantle hinge motor drives the hinge's own axis: turn the other drive off.");
				return false;
			}
			Drive = bTwistTurns ? &AngularDrive.TwistDrive : &AngularDrive.SwingDrive;
			MaxForceUnits = UnrealTorquePerNewtonMeter;
			bAccelerationMode = AngularDrive.bAccelerationMode;
		}
		if (Drive->bEnablePositionDrive && Drive->bEnableVelocityDrive)
		{
			OutProblem = TEXT("drives both to a position and at a speed. A Mantle motor does one: turn off its position drive or its velocity drive.");
			return false;
		}
		if (bAccelerationMode && Drive->MaxForce > 0.0f)
		{
			OutProblem = TEXT("has a drive in Acceleration mode with a Max Force, which Unreal reads as an acceleration. A Mantle motor's max force is a force: turn off the drive's Acceleration mode.");
			return false;
		}
		FMantleConstraintMotor Motor;
		Motor.Mode = Drive->bEnablePositionDrive ? EMantleMotorMode::Position : EMantleMotorMode::Speed;
		// No Max Force is no cap in Unreal: the most a Mantle motor may have.
		Motor.MaxForce = Drive->MaxForce > 0.0f ? FMath::Min(Drive->MaxForce / MaxForceUnits, FMantlePlaceLimits::MaxMotorForce)
			: FMantlePlaceLimits::MaxMotorForce;
		// The engine drives entityB's frame relative to entityA's, so entityA's own position or speed is
		// the opposite, measured from the middle of the limits (see ApplyConstraintMotor).
		if (bSlider)
		{
			Motor.Target = Motor.Mode == EMantleMotorMode::Position ? -LinearDrive.PositionTarget[SliderAxis] : -LinearDrive.VelocityTarget[SliderAxis];
		}
		else
		{
			const FVector& TurnAxis = LocalAxes[Turning->Index];
			Motor.Target = Motor.Mode == EMantleMotorMode::Position
				? OffsetAbout(TurnAxis) - FMath::RadiansToDegrees(AngularDrive.OrientationTarget.Quaternion().GetTwistAngle(TurnAxis))
				: -AngularDrive.AngularVelocityTarget[Turning->Index] * 360.0;
		}
		Motor.Target = FMath::Abs(Motor.Target) < ConstraintReadTolerance ? 0.0 : Motor.Target;
		OutProps.Motor = Motor;
	}

	// One location for both ends: the joint as the engine would make it when the level plays.
	OutProps.AnchorA = ActorA->GetActorTransform().InverseTransformPosition(Joint.GetLocation());

	double BreakForce = 0.0;
	if (Profile.bLinearBreakable)
	{
		BreakForce = Profile.LinearBreakThreshold / UnrealForcePerNewton;
	}
	else if (Profile.bAngularBreakable && OutProps.Type != EMantleConstraintType::Rope && OutProps.Type != EMantleConstraintType::Ball)
	{
		BreakForce = Profile.AngularBreakThreshold / UnrealTorquePerNewtonMeter;
	}
	if (BreakForce > 0.0)
	{
		OutProps.BreakForce = FMath::Min(BreakForce, FMantlePlaceLimits::MaxConstraintBreakForce);
	}

	// Whatever is left that the place parser would refuse (a motor's target outside the limits).
	FString ValueProblem;
	if (!CheckValues(OutProps, ValueProblem))
	{
		OutProblem = FString::Printf(TEXT("has a setting Mantle cannot publish: %s."), *ValueProblem);
		return false;
	}
	return true;
}

// ---- AMantleConstraint -------------------------------------------------------------------------

AMantleConstraint::AMantleConstraint()
{
	PrimaryActorTick.bCanEverTick = false;
	// Server only: players see the props move, which is their own replicated movement.
	bReplicates = false;
	Constraint = CreateDefaultSubobject<UPhysicsConstraintComponent>(TEXT("Constraint"));
	Constraint->SetMobility(EComponentMobility::Movable);
	SetRootComponent(Constraint);
}

void AMantleConstraint::InitFromProps(const FString& InEntityId, const FMantleConstraintProps& InProps)
{
	EntityId = InEntityId;
	Props = InProps;
}

bool AMantleConstraint::Join(UPrimitiveComponent* A, UPrimitiveComponent* B, FString& OutError)
{
	if (bJoined)
	{
		OutError = TEXT("it is already joined");
		return false;
	}
	FBodyInstance* BodyA = A ? A->GetBodyInstance() : nullptr;
	if (!BodyA || !BodyA->IsValidBodyInstance())
	{
		OutError = TEXT("entityA has no physics body");
		return false;
	}
	FBodyInstance* BodyB = nullptr;
	if (B)
	{
		if (B == A)
		{
			OutError = TEXT("entityA and entityB are the same entity");
			return false;
		}
		BodyB = B->GetBodyInstance();
		if (!BodyB || !BodyB->IsValidBodyInstance())
		{
			OutError = TEXT("entityB has no physics body");
			return false;
		}
	}
	if (!FMantleConstraints::CheckValues(Props, OutError))
	{
		return false;
	}

	const FTransform TransformA = A->GetComponentTransform();
	const FTransform TransformB = B ? B->GetComponentTransform() : FTransform::Identity;
	Frames = FMantleConstraints::MakeFrames(Props, TransformA, B ? &TransformB : nullptr);

	// The actor stands on the joint, where a debugger (and the log) finds it; it follows nothing.
	if (GetAttachParentActor())
	{
		DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
	}
	SetActorLocationAndRotation(Frames.AnchorA, FRotationMatrix::MakeFromXY(Frames.PrimaryA, Frames.SecondaryA).ToQuat());

	// The frames are set here, in each body's own space, and the joint made from them directly:
	// the component's own InitComponentConstraint would remake both at its one location, which
	// cannot hold a rope's two anchors or a hinge's or a slider's moved middle.
	FConstraintInstance& Instance = Constraint->ConstraintInstance;
	ApplyConstraintProfile(Instance, Props, Frames);
	Instance.SetRefFrame(EConstraintFrame::Frame1, ConstraintLocalFrame(TransformA, Frames.AnchorA, Frames.PrimaryA, Frames.SecondaryA));
	Instance.SetRefFrame(EConstraintFrame::Frame2, B
		? ConstraintLocalFrame(TransformB, Frames.AnchorB, Frames.PrimaryB, Frames.SecondaryB)
		: FTransform(Frames.PrimaryB, Frames.SecondaryB, Frames.PrimaryB ^ Frames.SecondaryB, Frames.AnchorB));
	Instance.InitConstraint(BodyA, BodyB, 1.0f, Constraint.Get(), FOnConstraintBroken::CreateUObject(this, &AMantleConstraint::HandleEngineBroken));
	if (!Instance.IsValidConstraintInstance())
	{
		OutError = TEXT("the physics engine refused the joint");
		return false;
	}
	ApplyConstraintMotor(Instance, Props, Frames, MotorInertia(Props.Type, Frames, A, B));

	JoinedA = A;
	JoinedB = B;
	bJoinedToEntityB = B != nullptr;
	bJoined = true;
	// A joined entity's actor going (Entity:Destroy) takes the joint with it, through the world's
	// native notification (see HandleActorDestroyed), which comes before that actor's body goes.
	JoinedActorA = A->GetOwner();
	JoinedActorB = B ? B->GetOwner() : nullptr;
	if (const UWorld* World = GetWorld(); World && !ActorDestroyedHandle.IsValid())
	{
		ActorDestroyedHandle = World->AddOnActorDestroyedHandler(
			FOnActorDestroyed::FDelegate::CreateUObject(this, &AMantleConstraint::HandleActorDestroyed));
	}
	UE_LOG(LogMantle, Log, TEXT("MANTLE_CONSTRAINT entity=%s type=%s a=%s b=%s anchor_a=%s anchor_b=%s middle=%.2f half_range=%.2f swing=%.2f twist=%.2f rope_cm=%.2f break_n=%.2f motor=%s"),
		EntityId.IsEmpty() ? TEXT("-") : *EntityId, MantleConstraintTypeToString(Props.Type), *Props.EntityA,
		Props.EntityB.IsEmpty() ? TEXT("world") : *Props.EntityB, *Frames.AnchorA.ToString(), *Frames.AnchorB.ToString(), Frames.Middle,
		Frames.HalfRange, Props.SwingLimit.Get(-1.0), Props.TwistLimit.Get(-1.0), Frames.RopeLength,
		Props.BreakForce.IsSet() ? Props.BreakForce.GetValue() : 0.0, *DescribeMotor());
	return true;
}

FString AMantleConstraint::DescribeMotor() const
{
	if (!Props.Motor.IsSet())
	{
		return TEXT("none");
	}
	const FMantleConstraintMotor& Motor = Props.Motor.GetValue();
	return FString::Printf(TEXT("%s:%.2f:%.2f"), MantleMotorModeToString(Motor.Mode), Motor.Target, Motor.MaxForce);
}

bool AMantleConstraint::SetMotor(const TOptional<FMantleConstraintMotor>& Motor, FString& OutError)
{
	if (Props.Type != EMantleConstraintType::Hinge && Props.Type != EMantleConstraintType::Slider)
	{
		OutError = FString::Printf(TEXT("a %s has no motor; only a hinge or a slider does"), MantleConstraintTypeToString(Props.Type));
		return false;
	}
	UPrimitiveComponent* A = JoinedA.Get();
	if (!bJoined || bBroken || !A)
	{
		OutError = bBroken ? TEXT("the joint is broken") : TEXT("the joint is not joined");
		return false;
	}
	if (Motor.IsSet() && !FMantleConstraints::CheckMotor(Props.Type, Props.bLimited, Props.LimitMin, Props.LimitMax, Motor.GetValue(), OutError))
	{
		return false;
	}
	Props.Motor = Motor;
	UPrimitiveComponent* B = JoinedB.Get();
	ApplyConstraintMotor(Constraint->ConstraintInstance, Props, Frames, MotorInertia(Props.Type, Frames, A, B));
	// A new target does not wake a sleeping body by itself (FConstraintInstance's drive setters only
	// pass the drive on), and a sleeping body does not move.
	for (UPrimitiveComponent* Joined : { A, B })
	{
		if (FBodyInstance* Body = Joined ? Joined->GetBodyInstance() : nullptr; Body && Body->IsValidBodyInstance())
		{
			Body->WakeInstance();
		}
	}
	UE_LOG(LogMantle, Log, TEXT("MANTLE_CONSTRAINT_MOTOR entity=%s type=%s a=%s motor=%s"), EntityId.IsEmpty() ? TEXT("-") : *EntityId,
		MantleConstraintTypeToString(Props.Type), *Props.EntityA, *DescribeMotor());
	return true;
}

TOptional<double> AMantleConstraint::GetPosition() const
{
	UPrimitiveComponent* A = JoinedA.Get();
	UPrimitiveComponent* B = JoinedB.Get();
	if ((Props.Type != EMantleConstraintType::Hinge && Props.Type != EMantleConstraintType::Slider) || !bJoined || !A
		|| (bJoinedToEntityB && !B) || !Constraint)
	{
		return TOptional<double>();
	}
	// The two frames in the world now, as Join set them in each body's own (unscaled) space.
	const FConstraintInstance& Instance = Constraint->ConstraintInstance;
	FTransform BodyA = A->GetComponentTransform();
	BodyA.RemoveScaling();
	const FTransform FrameA = Instance.GetRefFrame(EConstraintFrame::Frame1) * BodyA;
	FTransform FrameB = Instance.GetRefFrame(EConstraintFrame::Frame2);
	if (B)
	{
		FTransform BodyB = B->GetComponentTransform();
		BodyB.RemoveScaling();
		FrameB = FrameB * BodyB;
	}
	// entityB's frame sits at the middle of the limits; entityA's moves with entityA. So the frames'
	// offset is the middle minus entityA's turn or travel from where it was made.
	if (Props.Type == EMantleConstraintType::Hinge)
	{
		const FQuat Relative = FrameA.GetRotation().Inverse() * FrameB.GetRotation();
		const double Twist = FMath::RadiansToDegrees(Relative.GetTwistAngle(FVector::XAxisVector));
		return FRotator::NormalizeAxis(Frames.Middle - Twist);
	}
	const double Offset = FrameA.InverseTransformPositionNoScale(FrameB.GetLocation()).X;
	return Frames.Middle - Offset;
}

AMantleConstraint* AMantleConstraint::SpawnJoined(UWorld* World, const FString& InEntityId, const FMantleConstraintProps& InProps,
	UPrimitiveComponent* A, UPrimitiveComponent* B, FString& OutError)
{
	if (!World)
	{
		OutError = TEXT("there is no world to join them in");
		return nullptr;
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Params.ObjectFlags |= RF_Transient;
	AMantleConstraint* Spawned = World->SpawnActor<AMantleConstraint>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
	if (!Spawned)
	{
		OutError = TEXT("the constraint could not be spawned");
		return nullptr;
	}
	Spawned->InitFromProps(InEntityId, InProps);
	if (!Spawned->Join(A, B, OutError))
	{
		Spawned->Destroy();
		return nullptr;
	}
	return Spawned;
}

int32 AMantleConstraint::CountLive(const UWorld* World)
{
	int32 Live = 0;
	if (!World)
	{
		return Live;
	}
	for (TActorIterator<AMantleConstraint> It(const_cast<UWorld*>(World)); It; ++It)
	{
		const AMantleConstraint* Candidate = *It;
		if (IsValid(Candidate) && !Candidate->IsActorBeingDestroyed() && Candidate->IsJoined() && !Candidate->IsBroken())
		{
			++Live;
		}
	}
	return Live;
}

void AMantleConstraint::Destroyed()
{
	// Called for every destroy, begun play or not (EndPlay is only routed to an actor that began play).
	StopWatchingJoinedActors();
	Super::Destroyed();
}

void AMantleConstraint::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// The component terms its constraint in its own EndPlay (or, without one, when it unregisters).
	StopWatchingJoinedActors();
	Super::EndPlay(EndPlayReason);
}

void AMantleConstraint::StopWatchingJoinedActors()
{
	if (ActorDestroyedHandle.IsValid())
	{
		if (const UWorld* World = GetWorld())
		{
			World->RemoveOnActorDestroyedHandler(ActorDestroyedHandle);
		}
		ActorDestroyedHandle.Reset();
	}
}

void AMantleConstraint::HandleEngineBroken(int32 ConstraintIndex)
{
	if (bBroken)
	{
		return;
	}
	bBroken = true;
	UE_LOG(LogMantle, Log, TEXT("MANTLE_CONSTRAINT_BROKEN entity=%s type=%s a=%s b=%s break_n=%.2f"),
		EntityId.IsEmpty() ? TEXT("-") : *EntityId, MantleConstraintTypeToString(Props.Type), *Props.EntityA,
		Props.EntityB.IsEmpty() ? TEXT("world") : *Props.EntityB, Props.BreakForce.IsSet() ? Props.BreakForce.GetValue() : 0.0);
	OnBroken.Broadcast(this);
}

void AMantleConstraint::HandleActorDestroyed(AActor* DestroyedActor)
{
	if (!DestroyedActor || IsActorBeingDestroyed())
	{
		return;
	}
	if (DestroyedActor == JoinedActorA.Get() || DestroyedActor == JoinedActorB.Get())
	{
		Destroy();
	}
}
