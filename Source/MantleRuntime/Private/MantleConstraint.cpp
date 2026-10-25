// Copyright Joshua Eger. All Rights Reserved.
#include "MantleConstraint.h"
#include "MantlePlaceLimits.h"
#include "MantleRuntime.h"

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

	/**
	 * The motions, limits, collision and break thresholds Props asks of the engine: the one place both
	 * a runtime joint and an authoring build's engine constraint take them from, so they cannot differ.
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
		// A rope's angles are free, so nothing twists it; a weld's or a hinge's twist breaks it too.
		const bool bAngularBreakable = bBreakable && Props.Type != EMantleConstraintType::Rope;
		Instance.SetAngularBreakable(bAngularBreakable,
			bAngularBreakable ? static_cast<float>(Props.BreakForce.GetValue() * FMantleConstraints::UnrealTorquePerNewtonMetre) : 0.0f);
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
}

bool FMantleConstraints::CheckValues(const FMantleConstraintProps& Props, FString& OutError)
{
	if (!IsFiniteConstraintVector(Props.AnchorA) || (Props.AnchorB.IsSet() && !IsFiniteConstraintVector(Props.AnchorB.GetValue())))
	{
		OutError = TEXT("an anchor must be three finite numbers");
		return false;
	}
	if (Props.Type == EMantleConstraintType::Hinge)
	{
		if (!IsFiniteConstraintVector(Props.Axis) || Props.Axis.IsZero())
		{
			OutError = TEXT("Axis must be a finite direction, not (0, 0, 0)");
			return false;
		}
		if (Props.bLimited && !(Props.MinAngle >= -FMantlePlaceLimits::MaxHingeAngle && Props.MinAngle <= 0.0
			&& Props.MaxAngle >= 0.0 && Props.MaxAngle <= FMantlePlaceLimits::MaxHingeAngle))
		{
			OutError = FString::Printf(TEXT("Limits must be {min, max} with -%g <= min <= 0 <= max <= %g degrees, got {%g, %g}"),
				FMantlePlaceLimits::MaxHingeAngle, FMantlePlaceLimits::MaxHingeAngle, Props.MinAngle, Props.MaxAngle);
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

	// A hinge turns about its axis, which is the engine's twist axis (each frame's X). A weld and a
	// rope take entityA's own X: a weld locks every angle, a rope frees them, so any frame will do.
	FVector Primary = (Props.Type == EMantleConstraintType::Hinge ? A.GetRotation().RotateVector(Props.Axis) : A.GetRotation().GetAxisX())
		.GetSafeNormal();
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

	// The engine limits a twist symmetrically, so entityB's frame is turned to the middle of
	// [min, max]: entityA's turn from where it was made, minus the middle, then stays within half the
	// range either way, which is exactly min..max.
	if (Props.Type == EMantleConstraintType::Hinge && Props.bLimited)
	{
		const double Middle = 0.5 * (Props.MinAngle + Props.MaxAngle);
		Frames.HalfRange = 0.5 * (Props.MaxAngle - Props.MinAngle);
		Frames.SecondaryB = FQuat(Primary, FMath::DegreesToRadians(Middle)).RotateVector(Secondary);
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
	const FMantleConstraintFrames Frames = MakeFrames(Props, TransformA, ActorB ? &TransformB : nullptr);

	// The engine's constraint sits where its joint is, its X along a hinge's axis; it makes both
	// bodies' frames from that when the level plays (UPhysicsConstraintComponent::UpdateConstraintFrames).
	Component->SetWorldLocationAndRotation(Frames.AnchorA, FRotationMatrix::MakeFromXY(Frames.PrimaryA, Frames.SecondaryA).ToQuat());
	Component->ConstraintActor1 = ActorA;
	Component->ConstraintActor2 = ActorB;
	FConstraintInstance& Instance = Component->ConstraintInstance;
	ApplyConstraintProfile(Instance, Props, Frames);
	// The middle of a hinge's limits, as the engine's own offset between the two frames: it turns
	// entityB's frame about the twist axis, as MakeFrames turns SecondaryB.
	Instance.AngularRotationOffset = (Props.Type == EMantleConstraintType::Hinge && Props.bLimited)
		? FQuat(FVector::XAxisVector, FMath::DegreesToRadians(0.5 * (Props.MinAngle + Props.MaxAngle))).Rotator()
		: FRotator::ZeroRotator;
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
	if (Profile.LinearDrive.IsPositionDriveEnabled() || Profile.LinearDrive.IsVelocityDriveEnabled()
		|| Profile.AngularDrive.IsOrientationDriveEnabled() || Profile.AngularDrive.IsVelocityDriveEnabled())
	{
		OutProblem = TEXT("has a motor (a linear or angular drive), which Mantle does not publish. Turn its drives off, and move the object from a place script.");
		return false;
	}

	const FTransform Joint = Component->GetComponentTransform();
	const ELinearConstraintMotion Linear[3] = { Instance.GetLinearXMotion(), Instance.GetLinearYMotion(), Instance.GetLinearZMotion() };
	const bool bLinearLocked = Linear[0] == LCM_Locked && Linear[1] == LCM_Locked && Linear[2] == LCM_Locked;
	const bool bLinearLimited = Linear[0] == LCM_Limited && Linear[1] == LCM_Limited && Linear[2] == LCM_Limited;

	// Twist turns about the frame's X, Swing 1 about its Z, Swing 2 about its Y.
	struct FAngularAxis
	{
		EAngularConstraintMotion Motion;
		double Limit;
		FVector LocalAxis;
	};
	const FAngularAxis Angular[3] = {
		{ Instance.GetAngularTwistMotion(), Instance.GetAngularTwistLimit(), FVector::XAxisVector },
		{ Instance.GetAngularSwing1Motion(), Instance.GetAngularSwing1Limit(), FVector::ZAxisVector },
		{ Instance.GetAngularSwing2Motion(), Instance.GetAngularSwing2Limit(), FVector::YAxisVector },
	};

	if (bLinearLimited)
	{
		for (const FAngularAxis& Axis : Angular)
		{
			if (Axis.Motion != ACM_Free)
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
	else if (bLinearLocked)
	{
		const FAngularAxis* Turning = nullptr;
		for (const FAngularAxis& Axis : Angular)
		{
			if (Axis.Motion == ACM_Locked)
			{
				continue;
			}
			if (Turning)
			{
				OutProblem = TEXT("turns about more than one axis. Mantle publishes a weld (every motion Locked) or a hinge (one of Twist, Swing 1 or Swing 2 Free or Limited, the rest Locked).");
				return false;
			}
			Turning = &Axis;
		}
		if (!Turning)
		{
			OutProps.Type = EMantleConstraintType::Weld;
		}
		else
		{
			OutProps.Type = EMantleConstraintType::Hinge;
			// The axis in entityA's own space (its rotation), from the constraint's frame.
			const FVector AxisWorld = Joint.GetRotation().RotateVector(Turning->LocalAxis);
			OutProps.Axis = SnapConstraintAxis(ActorA->GetActorQuat().Inverse().RotateVector(AxisWorld));
			if (Turning->Motion == ACM_Limited)
			{
				// The engine limits the turn symmetrically about its Angular Rotation Offset.
				const double Middle = FMath::RadiansToDegrees(Instance.AngularRotationOffset.Quaternion().GetTwistAngle(Turning->LocalAxis));
				const double Tolerance = 1e-3;
				double MinAngle = Middle - Turning->Limit;
				double MaxAngle = Middle + Turning->Limit;
				MinAngle = FMath::Abs(MinAngle) < Tolerance ? 0.0 : MinAngle;
				MaxAngle = FMath::Abs(MaxAngle) < Tolerance ? 0.0 : MaxAngle;
				if (MinAngle > 0.0 || MaxAngle < 0.0 || MinAngle < -FMantlePlaceLimits::MaxHingeAngle - Tolerance
					|| MaxAngle > FMantlePlaceLimits::MaxHingeAngle + Tolerance)
				{
					OutProblem = FString::Printf(
						TEXT("is a hinge limited to %g..%g degrees from where it starts. Mantle publishes limits that hold the starting angle (0), within -%g..%g: change its limit or its Angular Rotation Offset."),
						MinAngle, MaxAngle, FMantlePlaceLimits::MaxHingeAngle, FMantlePlaceLimits::MaxHingeAngle);
					return false;
				}
				OutProps.bLimited = true;
				OutProps.MinAngle = FMath::Max(MinAngle, -FMantlePlaceLimits::MaxHingeAngle);
				OutProps.MaxAngle = FMath::Min(MaxAngle, FMantlePlaceLimits::MaxHingeAngle);
			}
		}
	}
	else
	{
		OutProblem = TEXT("moves along some axes and not others. Mantle publishes a weld or a hinge (Linear X, Y and Z Motion all Locked) or a rope (all Limited).");
		return false;
	}

	// One location for both ends: the joint as the engine would make it when the level plays.
	OutProps.AnchorA = ActorA->GetActorTransform().InverseTransformPosition(Joint.GetLocation());

	double BreakForce = 0.0;
	if (Profile.bLinearBreakable)
	{
		BreakForce = Profile.LinearBreakThreshold / UnrealForcePerNewton;
	}
	else if (Profile.bAngularBreakable && OutProps.Type != EMantleConstraintType::Rope)
	{
		BreakForce = Profile.AngularBreakThreshold / UnrealTorquePerNewtonMetre;
	}
	if (BreakForce > 0.0)
	{
		OutProps.BreakForce = FMath::Min(BreakForce, FMantlePlaceLimits::MaxConstraintBreakForce);
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
	// cannot hold a rope's two anchors or a hinge's turned middle.
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

	JoinedA = A;
	JoinedB = B;
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
	UE_LOG(LogMantle, Log, TEXT("MANTLE_CONSTRAINT entity=%s type=%s a=%s b=%s anchor_a=%s anchor_b=%s half_range=%.2f rope_cm=%.2f break_n=%.2f"),
		EntityId.IsEmpty() ? TEXT("-") : *EntityId, MantleConstraintTypeToString(Props.Type), *Props.EntityA,
		Props.EntityB.IsEmpty() ? TEXT("world") : *Props.EntityB, *Frames.AnchorA.ToString(), *Frames.AnchorB.ToString(), Frames.HalfRange,
		Frames.RopeLength, Props.BreakForce.IsSet() ? Props.BreakForce.GetValue() : 0.0);
	return true;
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
