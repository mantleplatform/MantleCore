// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMeshActor.h"
#include "MantleCollisionGate.h"
#include "MantlePlaceTypes.h"
#include "MantlePhysicsProp.generated.h"

class UBodySetup;
class UPhysicalMaterial;
class UStaticMesh;
class UWorld;

/**
 * Simulated place objects.
 *
 * The replication model (decided up front): the server simulates every physics entity and clients
 * follow it through the engine's standard replicated physics movement, over Iris like everything
 * else. The server builds each StaticMesh entity that has a physics block as an AMantlePhysicsProp:
 * a replicated, always-relevant actor with bReplicateMovement, so FRepMovement (with bRepPhysics
 * while it simulates) carries its pose and velocity and each client's copy is steered onto it by
 * the engine's physics replication, in its predictive interpolation mode: the copy's velocities
 * are driven toward the server's pose, extrapolated over the send interval, so it follows smoothly
 * rather than simulating freely between updates and being pulled back at each one (a client has
 * none of the server's joints, motors or grab handles). There is no client prediction of props.
 * A body at rest produces an unchanged FRepMovement, so a sleeping prop sends nothing.
 *
 * A connected client builds the place from data like every other entity, so for a physics entity
 * it builds a plain placeholder (a Movable AStaticMeshActor with the entity's mesh and materials).
 * When the server's prop replicates in, it takes the placeholder's look over (AdoptPlaceholder),
 * points the retained place and the script host at itself, and destroys the placeholder. A late
 * joiner gets the prop with its current pose in the prop's initial state.
 *
 * A server script makes physics props too (script API 1.95: Assets:Spawn with Anchored = false,
 * Entity:Clone of a physics entity). They are the same actor, replicated the same way. Their
 * placeholder is the plain stand-in each client's script host builds from the script replicator's
 * spawn item, registered with the place by id (RegisterRuntimeEntityActor); the prop and the stand-in
 * may arrive in either order (AdoptWaiting). So a prop never reads its settings from the place: they
 * come with it, once (ReplicatedSettings, with the surface preset's own byte).
 */

/**
 * What a client's copy of a prop needs of its physics block, sent once with the prop (the surface
 * preset has its own byte, PhysicsMaterial, since a script may change it). Placed and script-made
 * props alike: a client never looks the settings up in the place.
 */
USTRUCT()
struct MANTLERUNTIME_API FMantlePhysicsPropSettings
{
	GENERATED_BODY()

	/** kg; 0 when the mass is computed from the collision (no mass is ever below FMantlePlaceLimits::MinPhysicsMass). */
	UPROPERTY() float MassKg = 0.0f;
	UPROPERTY() float LinearDamping = 0.01f;
	UPROPERTY() float AngularDamping = 0.0f;
	UPROPERTY() bool bGravity = true;

	static FMantlePhysicsPropSettings From(const FMantlePhysicsProps& Physics);
	/** Onto Physics: mass, damping and gravity (simulate and the surface preset are left as they are). */
	void ApplyTo(FMantlePhysicsProps& Physics) const;
};

/** Where the collision a physics entity simulates on came from. */
enum class EMantlePhysicsBodySource : uint8
{
	/** The mesh's own simple collision, used as it is. */
	Mesh,
	/** The mesh's simple shapes, copied because the mesh collides complex-as-simple. */
	CopiedShapes,
	/** The mesh has no simple shapes: a box the size of its bounds. Never complex-as-simple. */
	BoundsBox
};

/** What the builder makes for a StaticMesh entity with a physics block. */
enum class EMantlePhysicsBuild : uint8
{
	/** The server (dedicated or listen), standalone play and the editor preview: the replicated, simulating prop. */
	Prop,
	/** A connected client: a plain placeholder that the server's replicated prop takes over. */
	Placeholder,
	/** An authoring build (the editor importer): a plain actor carrying the settings, for export. */
	Authoring
};

MANTLERUNTIME_API const TCHAR* MantlePhysicsBodySourceToString(EMantlePhysicsBodySource Source);

/**
 * "MantleAnchored": the actor tag that authors a physics entity which starts anchored ("simulate":
 * false) in the editor. Tick Simulate Physics on the mesh, as for any physics entity, and add this
 * tag to the actor's Tags: the exporter writes simulate false and never exports the tag as one of
 * the entity's tags, and the importer (an authoring build) adds it back. Simulate Physics stays the
 * one switch that makes a mesh a physics entity, so the tag alone does nothing.
 */
MANTLERUNTIME_API FName MantlePhysicsAnchoredTag();

/**
 * What one physics.material preset's UPhysicalMaterial holds.
 * Density is in g/cm^3 and
 * only matters for a body whose place gives no mass.
 */
struct FMantlePhysicsMaterialPreset
{
	/** The asset's name under /Game/Mantle/Physics. */
	const TCHAR* AssetName = nullptr;
	float Friction = 0.7f;
	float Restitution = 0.3f;
	float Density = 1.0f;
	/** Ice slides on anything: its friction combines by Min rather than the project's Average. */
	bool bFrictionCombineMin = false;
	/** Rubber bounces off anything: its restitution combines by Max rather than the project's Average. */
	bool bRestitutionCombineMax = false;
};

MANTLERUNTIME_API const FMantlePhysicsMaterialPreset& MantlePhysicsMaterialPreset(EMantlePhysicsMaterial Material);

/** The preset's asset as an object path, "/Game/Mantle/Physics/PM_MantleRubber.PM_MantleRubber". */
MANTLERUNTIME_API FString MantlePhysicsMaterialAssetPath(EMantlePhysicsMaterial Material);

/**
 * The preset's shipped UPhysicalMaterial, loaded by path. Null when the asset is missing (not
 * created, or not cooked): logs MANTLE_PHYSICS_MATERIAL_MISSING once per preset, and the body keeps
 * its mesh's own surface.
 */
MANTLERUNTIME_API UPhysicalMaterial* MantleLoadPhysicsMaterial(EMantlePhysicsMaterial Material);

/**
 * A static mesh component whose simulated body can differ from its mesh's collision. A physics
 * entity must simulate on simple shapes (Chaos does not simulate a dynamic triangle mesh), but an
 * uploaded mesh collides complex-as-simple, and its body setup is shared with every static use of
 * the same mesh, so the simulation body is this component's own.
 */
UCLASS()
class MANTLERUNTIME_API UMantlePhysicsMeshComponent : public UStaticMeshComponent
{
	GENERATED_BODY()

public:
	/**
	 * Picks the body this component simulates for Mesh: the mesh's own simple collision when it has
	 * some and does not collide complex-as-simple; otherwise a body of this component's own that
	 * collides simple-as-complex, holding a copy of the mesh's simple shapes, or a box the size of
	 * the mesh's bounds when it has none. Call before SetStaticMesh, or recreate the physics state
	 * after. Deterministic, so the server and every client simulate the same shape.
	 */
	EMantlePhysicsBodySource PrepareSimulationBody(UStaticMesh* Mesh);

	//~ Begin UPrimitiveComponent
	virtual UBodySetup* GetBodySetup() override;
	//~ End UPrimitiveComponent

private:
	UPROPERTY(Transient)
	TObjectPtr<UBodySetup> SimulationBodySetup;
};

class AMantlePhysicsProp;

/**
 * What a client's script reads of a physics entity whose prop is not here (stage 6 of large places: the server sends a player only the props near their views, and a prop that goes
 * out of range is replaced by a plain placeholder). On the placeholder a prop left behind: the facts of the prop as it was last seen here. A script that asks about a far prop
 * (Mass, Anchored, Grabbable, PhysicsMaterial) is answered from it; a placeholder that never had a prop (not in range yet) has none, and the place's own values answer.
 */
UCLASS(NotBlueprintable, Transient)
class MANTLERUNTIME_API UMantleFarPropFacts : public UActorComponent
{
	GENERATED_BODY()

public:
	double MassKg = 0.0;
	/** The server's body was held still (not simulating) when the prop was last here. */
	bool bAnchored = true;
	bool bGrabbable = true;
	EMantlePhysicsMaterial Material = EMantlePhysicsMaterial::Default;
};

/**
 * What a connected client's prop needs from the place its world was built from, to take its placeholder
 * over: the place, the actor built for each entity, and a signal once the place is there. The game's
 * loaded-place subsystem provides it through AMantlePhysicsProp::PlaceForWorld.
 */
class IMantlePhysicsPropPlace
{
public:
	virtual ~IMantlePhysicsPropPlace() = default;

	virtual bool HasPlace() const = 0;
	virtual const FMantlePlace& GetPlace() const = 0;
	/** The place entity's actor, or a script-made entity's stand-in registered with the place (a prop adopts either). */
	virtual AActor* FindActor(const FString& EntityId) const = 0;
	/** EntityId's actor is NewActor from now on. */
	virtual void ReplaceEntityActor(const FString& EntityId, AActor* NewActor) = 0;
	/** Fires once the place is retained; HasPlace is true from then on. */
	virtual FSimpleMulticastDelegate& OnPlaceReady() = 0;
};

/** Finds the place a world was built from; see AMantlePhysicsProp::PlaceForWorld. */
using FMantlePhysicsPropPlaceResolver = IMantlePhysicsPropPlace* (*)(UWorld* World);

/** Server: a physics prop fell out of the world (the kill floor): the script world destroys its entity so every client's stand-in goes too. */
DECLARE_MULTICAST_DELEGATE_OneParam(FMantlePhysicsPropFellDelegate, AMantlePhysicsProp* /*Prop*/);

/** Server: a physics prop hit something while its hit events are on (the script host's Entity.Collided). */
DECLARE_MULTICAST_DELEGATE_TwoParams(FMantlePhysicsPropHitDelegate, AMantlePhysicsProp* /*Prop*/, AActor* /*Other*/);

/** A StaticMesh entity with a physics block, as the server and standalone play build it. See the file comment. */
UCLASS(NotPlaceable, Transient)
class MANTLERUNTIME_API AMantlePhysicsProp : public AStaticMeshActor
{
	GENERATED_BODY()

public:
	AMantlePhysicsProp(const FObjectInitializer& ObjectInitializer);

	/** The place a world was built from, or null when the world has none (yet). The game sets it at startup; without it a
	    client's prop never adopts its placeholder. */
	static FMantlePhysicsPropPlaceResolver PlaceForWorld;

	/** Which actor a physics entity becomes: bRuntimeBuild is a transient build into a game world. */
	static EMantlePhysicsBuild ChooseBuild(bool bRuntimeBuild, ENetMode NetMode);

	/** Mass, damping, gravity and the surface preset from the place onto Component's body (also an authoring actor's, for export). */
	static void ApplyBodySettings(UPrimitiveComponent* Component, const FMantlePhysicsProps& Physics);

	/**
	 * The preset's physical material as Component's override, then its mass recomputed so
	 * the preset's density counts when no mass is set. Leaves the body as it is when the asset is
	 * missing (MantleLoadPhysicsMaterial logs it).
	 */
	static void ApplyPhysicsMaterial(UPrimitiveComponent* Component, EMantlePhysicsMaterial Material);

	/**
	 * Server and standalone, from the builder once the mesh is on: records the entity id (replicated
	 * to clients once) and the place's settings, and prepares the body. Does not start simulating.
	 */
	void InitFromPlace(const FString& InEntityId, const FMantlePhysicsProps& InPhysics);

	/**
	 * Server: a script's prop whose mesh is not here yet (Assets:Spawn of an uploaded mesh still downloading): records the id and
	 * settings as InitFromPlace does but makes no body (there is no shape to make it from). BeginSimulation then holds it, with no
	 * body, until MeshArrived.
	 */
	void InitWithoutMesh(const FString& InEntityId, const FMantlePhysicsProps& InPhysics);

	/**
	 * Server: a script's Entity:Clone of Source, a fresh prop rather than the engine's template copy (that would copy the
	 * replicated entity id and Source's own body setup). Takes Source's mesh, materials, shadow, draw distance, collision, scale,
	 * settings, surface preset and Grabbable; it starts as Source is now (simulating or anchored) once BeginSimulation runs, at
	 * rest. Call before BeginSimulation, on a prop spawned at Source's transform.
	 */
	void InitAsCopyOf(const AMantlePhysicsProp& Source, const FString& InEntityId);

	/**
	 * The component's mesh was just set (an uploaded mesh finished downloading here): the body is made for it. A server prop
	 * waiting for its mesh starts as BeginSimulation would have; a client's adopted copy takes the server's simulate and sleep
	 * state, as adopting its placeholder does.
	 */
	void MeshArrived();
	/** Server: InitWithoutMesh's prop, until MeshArrived. */
	bool IsWaitingForMesh() const { return bWaitingForMesh; }

	/** The physics block this prop simulates with (the place's, a script's, or what a client was sent). */
	const FMantlePhysicsProps& GetPhysicsProps() const { return Physics; }

	/**
	 * Client: a script-made entity's stand-in was just registered with the place under EntityId (the script host built it from
	 * the spawn item): a copy of that entity's prop that replicated in first, and is waiting for it, takes it over now.
	 */
	static void AdoptWaiting(UWorld* World, const FString& InEntityId);

	/**
	 * Client: the script-made entity this copy stood for is gone here while the server still sends the copy (its spawn's area went away on
	 * this machine; or the server destroyed it, and the copy goes too in a moment). It stays as it is and waits, as a copy that came before
	 * its stand-in does, so a stand-in built for the entity again is taken over rather than drawn beside it.
	 */
	void ReturnToWaiting();

	/**
	 * A plain, movable mesh actor at Transform with From's mesh, override materials, shadow, draw distance and collision: what a
	 * client stands in a prop's place with (RestorePlaceholder, and the script host's stand-in for a clone of a physics entity).
	 * Not simulating, not replicated. Null when the world can't spawn it.
	 */
	static AStaticMeshActor* SpawnPlainCopy(UWorld& World, const UStaticMeshComponent& From, const FTransform& Transform);

	/**
	 * Server and standalone, from the builder once transforms are applied: detaches from any parent
	 * (a parent place actor has no network identity, and a simulating body follows none) and starts
	 * simulating when the place says so. It keeps the world transform its parents gave it (a Group's
	 * location, rotation and scale, composed), but from then on it no longer follows them: a script
	 * moving the Group leaves the prop where it is, anchored or not.
	 *
	 * bWaitForGround: the place streams its cells (a standalone game builds the root first and its solid cells after, to the join): the prop stays where it stands, not
	 * simulating, until the solid cells around it stand (or until the place is complete and nothing streams for it), and then starts. Without that it simulates over
	 * ground that is not there yet and falls out of the world before the join.
	 */
	void BeginSimulation(bool bWaitForGround = false);
	bool IsWaitingForGround() const { return bWaitingForGround; }
	/** Asks the ground wait once (the tick does, every frame, while the prop waits). Public for tests. */
	void UpdateGroundWait();

	/**
	 * Client: takes Placeholder's mesh, materials, shadow, collision, visibility, scale and attached
	 * children over, then destroys it. Physics settings are the actor's own Physics member. False, changing nothing, when Placeholder is not a mesh actor.
	 */
	bool AdoptPlaceholder(AActor* Placeholder);

	/**
	 * Client: the mirror of AdoptPlaceholder, for a prop the server stopped sending (the player's views are far from it now: the engine destroys the client's copy). A plain
	 * placeholder with this prop's mesh, materials, shadow, collision, scale, hidden state, pose and attached children takes its place as the entity's actor (ReplaceEntityActor),
	 * carrying a UMantleFarPropFacts with what this prop was last seen as. The placeholder, or null (nothing made) when this prop is not an adopted client copy.
	 */
	AStaticMeshActor* RestorePlaceholder();

	/** Client: this copy has taken its placeholder over (a copy that has not is not a stand-in for anything, and leaves nothing behind when it goes). */
	bool IsAdopted() const { return bAdopted; }

	/** Server and standalone: every prop of the world that is out of BeginPlay and not yet ended, for the script replicator's area bookkeeping. */
	static void ForEachServerProp(const UWorld* World, TFunctionRef<void(AMantlePhysicsProp&)> Visit);

	/** Whether the simulated body is awake (not asleep, not anchored). Server. */
	bool IsBodyAwake() const;

	const FString& GetEntityId() const { return EntityId; }
	UMantlePhysicsMeshComponent* GetPhysicsMeshComponent() const;

	// The script-facing physics members (Entity:ApplyImpulse and friends, script API 1.15). -------

	/** Whether the body simulates now: false while anchored. */
	bool IsSimulating() const;
	/** Anchored stops simulating (the body stays where it is); un-anchored simulates again. Server only. */
	void SetAnchored(bool bAnchored);
	/** Script API 1.55 (Entity.Grabbable): a tool that grabs (the staff's telekinesis, the gravity gun) leaves a prop alone while this is false. Server; on unless a script turns it off. */
	bool IsGrabbable() const { return bGrabbable; }
	void SetGrabbable(const bool bInGrabbable) { bGrabbable = bInGrabbable; }
	/** kg*cm/s, world axes. Wakes the body; does nothing while anchored. */
	void ApplyImpulse(const FVector& Impulse);
	/** kg*cm^2/s, in degrees about the world axes. Wakes the body; does nothing while anchored. */
	void ApplyAngularImpulse(const FVector& ImpulseDegrees);
	/** cm/s. The simulated velocity, or the actor's while anchored. */
	FVector GetLinearVelocity() const;
	/** cm/s. Does nothing while anchored. */
	void SetLinearVelocity(const FVector& Velocity);
	/** Degrees per second about the world axes (script API 1.25). The simulated spin; zero while anchored. */
	FVector GetAngularVelocityDegrees() const;
	/** Degrees per second about the world axes. Wakes the body; does nothing while anchored. */
	void SetAngularVelocityDegrees(const FVector& AngularVelocity);
	/** kg: the place's mass, else the one computed from the collision; 0 without a body. */
	double GetMassKg() const;
	/** The surface preset (script API 1.28): the server's, which a client has replicated. */
	EMantlePhysicsMaterial GetPhysicsMaterial() const { return Physics.Material; }
	/** Server: changes the surface preset, anchored or not, and replicates it to every client as one byte. */
	void SetPhysicsMaterial(EMantlePhysicsMaterial Material);
	/** Server: turns the rigid-body hit events OnPhysicsHit relays on (they cost a callback per contact). */
	void SetHitEventsEnabled(bool bEnabled);

	/** Server, while hit events are on. */
	FMantlePhysicsPropHitDelegate OnPhysicsHit;

	/** Server: any prop falling out of the world, before the engine destroys it (the script world of its world listens). */
	static FMantlePhysicsPropFellDelegate OnAnyFellOutOfWorld;
	virtual void FellOutOfWorld(const class UDamageType& DamageType) override;

	//~ Begin AActor
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	/** Client: the engine is destroying this copy because the server stopped sending it (or destroyed it): leaves a placeholder in its place, see RestorePlaceholder. */
	virtual void PreDestroyFromReplication() override;
	/** A client's copy asks the collision gate each tick (it ticks on a client only, once adopted). */
	virtual void Tick(float DeltaSeconds) override;
	/** A client's copy whose gate is closed stays off and at the server's pose after each movement update. */
	virtual void OnRep_ReplicatedMovement() override;
	virtual void NotifyHit(UPrimitiveComponent* MyComp, AActor* Other, UPrimitiveComponent* OtherComp, bool bSelfMoved,
		FVector HitLocation, FVector HitNormal, FVector NormalImpulse, const FHitResult& Hit) override;
	//~ End AActor

protected:
	UFUNCTION()
	void OnRep_EntityId();

	/** Client: the server's preset arrived (at spawn, or a script changed it). */
	UFUNCTION()
	void OnRep_PhysicsMaterial();

private:
	/** Every prop with authority that has begun play and not ended, for ForEachServerProp. */
	static TArray<AMantlePhysicsProp*>& ServerProps();

	/** Client: every copy that has its entity id but no stand-in to take over yet (a script-made entity whose spawn item has not been applied here), for AdoptWaiting. */
	static TArray<AMantlePhysicsProp*>& WaitingProps();

	/** Client: the server's simulate state on Component, and its pose and sleep as a target, as when this copy took its placeholder over. */
	void TakeServerPhysicsState(UStaticMeshComponent& Component);

	/** The preset MaterialByte names, Default for a byte past the last preset. */
	static EMantlePhysicsMaterial PhysicsMaterialFromByte(uint8 MaterialByte);

	/** Client: adopt this entity's placeholder once the place is retained (now, or when it is). */
	void TryAdopt();
	void OnPlaceRetained();

	/** The place entity this prop is. Sent once: it never changes. */
	UPROPERTY(ReplicatedUsing = OnRep_EntityId)
	FString EntityId;

	/**
	 * The surface preset, an EMantlePhysicsMaterial as one byte. Replicated for the whole
	 * life of the prop, since a server script may change it; a late joiner gets the current one in
	 * the prop's initial state, and it takes precedence over the place's on a client.
	 */
	UPROPERTY(ReplicatedUsing = OnRep_PhysicsMaterial)
	uint8 PhysicsMaterial = 0;

	/** Mass, damping and gravity, sent once (they never change after the prop is made). A client's copy simulates with them. */
	UPROPERTY(Replicated)
	FMantlePhysicsPropSettings ReplicatedSettings;

	FMantlePhysicsProps Physics;
	EMantlePhysicsBodySource BodySource = EMantlePhysicsBodySource::Mesh;
	bool bAdopted = false;
	/** BeginSimulation(true): holding still until the solid cells around the prop stand (UpdateGroundWait). */
	bool bWaitingForGround = false;
	/** InitWithoutMesh: no body until MeshArrived; BeginSimulation's ground wait is kept for then. */
	bool bWaitingForMesh = false;
	bool bBeginWhenMeshed = false;
	bool bWaitForGroundWhenMeshed = false;

	/**
	 * The collision gate (FMantleCollisionGate): a client's copy simulates only while the solid cells under it are built here. A prop is relevant across the whole
	 * map, so its ground is the one most often not there. While closed the copy is not simulated and stands where the server's replicated movement puts it; when the
	 * ground stands again it takes the server's simulate and sleep state, as adopting its placeholder did.
	 */
	FMantleCollisionGate CollisionGate;
public:
	/** Asks the gate once and applies what it says (the tick does, every frame). Public for tests. */
	void UpdateCollisionGate();
	bool IsCollisionGateClosed() const { return CollisionGate.IsClosed(); }
	/** For tests: the prop is a client's adopted copy (what AdoptPlaceholder sets). */
	void MarkAdoptedForTest() { bAdopted = true; }
	/** For tests: what a client's copy arrives with (the entity id, the settings, the surface preset), and nothing else. */
	void SetReplicatedStateForTest(const FString& InEntityId, const FMantlePhysicsProps& InPhysics)
	{
		EntityId = InEntityId;
		ReplicatedSettings = FMantlePhysicsPropSettings::From(InPhysics);
		PhysicsMaterial = static_cast<uint8>(InPhysics.Material);
	}
private:
	void FollowServerPose();
	bool bHitEventsEnabled = false;
	bool bGrabbable = true;
	FDelegateHandle PlaceRetainedHandle;
};
