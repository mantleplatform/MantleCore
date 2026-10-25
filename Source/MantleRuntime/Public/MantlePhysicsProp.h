// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMeshActor.h"
#include "MantlePlaceTypes.h"
#include "MantlePhysicsProp.generated.h"

class UBodySetup;
class UPhysicalMaterial;
class UStaticMesh;
class UWorld;

/**
 * GhzGarage/mantle#456: simulated place objects.
 *
 * The replication model (decided for #456): the server simulates every physics entity and clients
 * follow it through the engine's standard replicated physics movement, over Iris like everything
 * else. The server builds each StaticMesh entity that has a physics block as an AMantlePhysicsProp:
 * a replicated, always-relevant actor with bReplicateMovement, so FRepMovement (with bRepPhysics
 * while it simulates) carries its pose and velocity and each client's copy is steered onto it by
 * the engine's physics replication. There is no client prediction of props: a client's copy
 * simulates locally between updates and is corrected toward the server's state. A body at rest
 * produces an unchanged FRepMovement, so a sleeping prop sends nothing.
 *
 * A connected client builds the place from data like every other entity, so for a physics entity
 * it builds a plain placeholder (a Movable AStaticMeshActor with the entity's mesh and materials).
 * When the server's prop replicates in, it takes the placeholder's look over (AdoptPlaceholder),
 * points the retained place and the script host at itself, and destroys the placeholder. A late
 * joiner gets the prop with its current pose in the prop's initial state.
 */

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
 * What one physics.material preset's UPhysicalMaterial holds. tools/physics/
 * Create-MantlePhysicalMaterials.py writes the assets from the same numbers. Density is in g/cm^3 and
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
	virtual AActor* FindActor(const FString& EntityId) const = 0;
	/** EntityId's actor is NewActor from now on. */
	virtual void ReplaceEntityActor(const FString& EntityId, AActor* NewActor) = 0;
	/** Fires once the place is retained; HasPlace is true from then on. */
	virtual FSimpleMulticastDelegate& OnPlaceReady() = 0;
};

/** Finds the place a world was built from; see AMantlePhysicsProp::PlaceForWorld. */
using FMantlePhysicsPropPlaceResolver = IMantlePhysicsPropPlace* (*)(UWorld* World);

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
	 * The preset's physical material as Component's override (#528), then its mass recomputed so
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
	 * Server and standalone, from the builder once transforms are applied: detaches from any parent
	 * (a parent place actor has no network identity, and a simulating body follows none) and starts
	 * simulating when the place says so. It keeps the world transform its parents gave it (a Group's
	 * location, rotation and scale, composed), but from then on it no longer follows them: a script
	 * moving the Group leaves the prop where it is, anchored or not.
	 */
	void BeginSimulation();

	/**
	 * Client: takes Placeholder's mesh, materials, shadow, collision, visibility, scale and attached
	 * children over, then destroys it. Physics settings are the actor's own Physics member. False, changing nothing, when Placeholder is not a mesh actor.
	 */
	bool AdoptPlaceholder(AActor* Placeholder);

	const FString& GetEntityId() const { return EntityId; }
	UMantlePhysicsMeshComponent* GetPhysicsMeshComponent() const;

	// The script-facing physics members (Entity:ApplyImpulse and friends, script API 1.15). -------

	/** Whether the body simulates now: false while anchored. */
	bool IsSimulating() const;
	/** Anchored stops simulating (the body stays where it is); un-anchored simulates again. Server only. */
	void SetAnchored(bool bAnchored);
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

	//~ Begin AActor
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
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
	/** The preset MaterialByte names, Default for a byte past the last preset. */
	static EMantlePhysicsMaterial PhysicsMaterialFromByte(uint8 MaterialByte);

	/** Client: adopt this entity's placeholder once the place is retained (now, or when it is). */
	void TryAdopt();
	void OnPlaceRetained();

	/** The place entity this prop is. Sent once: it never changes. */
	UPROPERTY(ReplicatedUsing = OnRep_EntityId)
	FString EntityId;

	/**
	 * The surface preset (#528), an EMantlePhysicsMaterial as one byte. Replicated for the whole
	 * life of the prop, since a server script may change it; a late joiner gets the current one in
	 * the prop's initial state, and it takes precedence over the place's on a client.
	 */
	UPROPERTY(ReplicatedUsing = OnRep_PhysicsMaterial)
	uint8 PhysicsMaterial = 0;

	FMantlePhysicsProps Physics;
	EMantlePhysicsBodySource BodySource = EMantlePhysicsBodySource::Mesh;
	bool bAdopted = false;
	bool bHitEventsEnabled = false;
	FDelegateHandle PlaceRetainedHandle;
};
