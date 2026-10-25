// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MantlePlaceTypes.h"

#include "MantleConstraint.generated.h"

class AMantleConstraint;
class UPhysicsConstraintComponent;
class UPrimitiveComponent;

/**
 * GhzGarage/mantle#528 P3: joints between physics entities.
 *
 * A Constraint joins a physics entity (a StaticMesh with a physics block, AMantlePhysicsProp) to
 * another, or to the world: a weld holds the two as they are, a hinge lets entityA turn about one
 * axis through its anchor, a rope keeps the two anchors within a length of each other. Only the
 * server (and standalone play) makes the joint, with Unreal's UPhysicsConstraintComponent between
 * the two props' bodies; nothing of it replicates. Every client sees the props' own replicated
 * movement, as for any physics entity, and a late joiner gets their current poses the same way.
 *
 * The place builder spawns one AMantleConstraint per Constraint entity on every machine, so the
 * entity has its actor everywhere; it joins its props only where they are the simulating props
 * (the server's). A server script's Constraints:Create spawns one too. An authoring build (the
 * importer) makes an engine Physics Constraint Actor instead, which the exporter reads back.
 */

/** The world-space joint FMantleConstraints::MakeFrames works out for a Constraint's props and its entities' poses. */
struct FMantleConstraintFrames
{
	/** Where the joint sits on entityA, and on entityB (or in the world). */
	FVector AnchorA = FVector::ZeroVector;
	FVector AnchorB = FVector::ZeroVector;
	/** entityA's joint axes: X is a hinge's axis (for a weld or a rope, entityA's own X), Y is perpendicular to it. */
	FVector PrimaryA = FVector::XAxisVector;
	FVector SecondaryA = FVector::YAxisVector;
	/** entityB's joint axes: entityA's, turned about the hinge axis by the middle of its limits so they are symmetric. */
	FVector PrimaryB = FVector::XAxisVector;
	FVector SecondaryB = FVector::YAxisVector;
	/** A limited hinge: half of max - min, degrees, which the engine's twist limit allows either side. */
	double HalfRange = 0.0;
	/** A rope: its length in cm (the props' own, else the anchors' distance); 0 for anything else. */
	double RopeLength = 0.0;
};

/** The rules a Constraint's values follow outside the place parser (a script's Constraints:Create, the exporter). */
struct MANTLERUNTIME_API FMantleConstraints
{
	/** Unreal forces are in kg*cm/s^2: a newton is 100 of them, and a newton-metre of torque 10000 kg*cm^2/s^2. */
	static constexpr double UnrealForcePerNewton = 100.0;
	static constexpr double UnrealTorquePerNewtonMetre = 10000.0;

	/** A rope shorter than this is held to its anchors, like a ball joint: a zero-length limit is not stable. */
	static constexpr double MinRopeLength = 0.1;

	/**
	 * False, with OutError naming the value, when Props breaks a range the place parser holds a place's
	 * Constraint to: a finite, non-zero axis; the hinge limits; the rope length; breakForce; finite
	 * anchors. Which keys a type takes, and what the entities are, are the caller's to check.
	 */
	static bool CheckValues(const FMantleConstraintProps& Props, FString& OutError);

	/**
	 * The joint Props describes between an entity at A and one at B (null: the world), as it would be
	 * made now. Anchors are in each transform's space, scale included; the axis in A's rotation.
	 */
	static FMantleConstraintFrames MakeFrames(const FMantleConstraintProps& Props, const FTransform& A, const FTransform* B);

	/**
	 * An authoring build (the importer): sets up an engine Physics Constraint Actor's component so it
	 * holds what Props says between ActorA and ActorB (null: the world), with the actors where they are
	 * now. The exporter reads it back through ReadEditorConstraint. An engine constraint has one
	 * location, so a rope's two anchors both become entityA's (its length is kept).
	 */
	static void ConfigureEditorConstraint(UPhysicsConstraintComponent* Component, const FMantleConstraintProps& Props, AActor* ActorA, AActor* ActorB);

	/**
	 * The exporter: what an engine Physics Constraint Actor's component holds, as Constraint props
	 * (without the entity ids: ActorA and ActorB are its Constraint Actor 1 and 2, which the caller
	 * resolves). False, with OutProblem saying what Mantle cannot publish and how to fix it, when its
	 * settings are not a weld, a hinge or a rope.
	 */
	static bool ReadEditorConstraint(const UPhysicsConstraintComponent* Component, const AActor* ActorA, const AActor* ActorB,
		FMantleConstraintProps& OutProps, FString& OutProblem);
};

/** Server: a joint broke under its breakForce. Broadcast once. */
DECLARE_MULTICAST_DELEGATE_OneParam(FMantleConstraintBrokenDelegate, AMantleConstraint* /*Constraint*/);

/** One Constraint: a place's Constraint entity, or a server script's Constraints:Create. See the file comment. */
UCLASS(NotPlaceable, Transient)
class MANTLERUNTIME_API AMantleConstraint : public AActor
{
	GENERATED_BODY()

public:
	AMantleConstraint();

	/** Records the entity id (empty for a script's) and the props. Joins nothing. */
	void InitFromProps(const FString& InEntityId, const FMantleConstraintProps& InProps);

	/**
	 * Server and standalone: joins A (entityA's body) to B (entityB's; null for the world) as the props
	 * say, with both where they stand now, and moves this actor to the joint. False, with OutError,
	 * when a body cannot take part (none, or the same one twice), it is already joined, or the physics
	 * engine refuses the joint.
	 */
	bool Join(UPrimitiveComponent* A, UPrimitiveComponent* B, FString& OutError);

	/** Spawns a constraint for Props in World and joins A to B (null: the world). Null, with OutError, when the join fails. */
	static AMantleConstraint* SpawnJoined(UWorld* World, const FString& EntityId, const FMantleConstraintProps& Props,
		UPrimitiveComponent* A, UPrimitiveComponent* B, FString& OutError);

	/** Joined constraints in World that are not broken, being destroyed or gone: what MaxConstraintsPerPlace counts. */
	static int32 CountLive(const UWorld* World);

	bool IsJoined() const { return bJoined; }
	bool IsBroken() const { return bBroken; }
	const FString& GetEntityId() const { return EntityId; }
	const FMantleConstraintProps& GetProps() const { return Props; }
	/** The world-space joint as it was made; default until joined. */
	const FMantleConstraintFrames& GetFrames() const { return Frames; }
	UPhysicsConstraintComponent* GetConstraintComponent() const { return Constraint; }
	UPrimitiveComponent* GetJoinedA() const { return JoinedA.Get(); }
	UPrimitiveComponent* GetJoinedB() const { return JoinedB.Get(); }

	/** Server: the joint broke. Once. */
	FMantleConstraintBrokenDelegate OnBroken;

	//~ Begin AActor
	virtual void Destroyed() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	//~ End AActor

private:
	/** The physics engine's FOnConstraintBroken. */
	void HandleEngineBroken(int32 ConstraintIndex);

	/**
	 * The world's native actor-destroyed notification (UWorld::AddOnActorDestroyedHandler): when a
	 * joined entity's actor goes, the joint goes with it (without Broken), before that body does. Not
	 * AActor::OnDestroyed: a dynamic delegate reaches an actor through ProcessEvent, which drops the
	 * call in a world whose actors are not initialized yet.
	 */
	void HandleActorDestroyed(AActor* DestroyedActor);
	/** Stops HandleActorDestroyed (this constraint is going). */
	void StopWatchingJoinedActors();

	FDelegateHandle ActorDestroyedHandle;
	TWeakObjectPtr<AActor> JoinedActorA;
	TWeakObjectPtr<AActor> JoinedActorB;

	UPROPERTY(VisibleAnywhere, Category = "Constraint")
	TObjectPtr<UPhysicsConstraintComponent> Constraint;

	FString EntityId;
	FMantleConstraintProps Props;
	FMantleConstraintFrames Frames;
	TWeakObjectPtr<UPrimitiveComponent> JoinedA;
	TWeakObjectPtr<UPrimitiveComponent> JoinedB;
	bool bJoined = false;
	bool bBroken = false;
};
