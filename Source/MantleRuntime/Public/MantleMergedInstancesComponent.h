// Copyright Joshua Eger. All Rights Reserved.
// The instances of a static InstancedMesh as a dedicated server holds them: a few physics bodies, each carrying the shapes of many instances.
//
// The engine's UInstancedStaticMeshComponent gives every instance a physics body of its own (FInstancedMeshComponentBodies): one Chaos particle, its geometry union and its
// place in the scene's search tree. A big place (City Sample: about 1.16 million instances) then costs gigabytes of bodies that nothing ever moves. This component keeps the
// instances' transforms and makes one body for each run of instances, with every instance of the run as shapes on it. The shapes are made from the same cooked mesh collision
// with the engine's own code (UBodySetup::AddShapesToRigidActor_AssumesLocked, the call FBodyInstance::Weld makes), so a trace, sweep or overlap answers as it did with a body
// for each instance.
//
// It exists only on a dedicated server, which draws nothing: it has no scene proxy and makes no render state. A hit on any of its shapes names the component (and its actor);
// nothing in the game reads which instance was hit (FHitResult::Item is the body's index here, not an instance's).

#pragma once

#include "CoreMinimal.h"
#include "Components/StaticMeshComponent.h"

#include "MantleMergedInstancesComponent.generated.h"

class FPhysScene_Chaos;
class UBodySetup;
struct FBodyInstance;
struct FNavigationRelevantData;

UCLASS(ClassGroup = (Mantle), NotBlueprintable, Transient)
class MANTLERUNTIME_API UMantleMergedInstancesComponent : public UStaticMeshComponent
{
	GENERATED_BODY()

public:
	UMantleMergedInstancesComponent();

	/**
	 * The most shapes one body holds. Chaos looks through a body's shapes one after another when a query reaches it, and FHitResult::ElementIndex is a uint8, so a body stays
	 * well under 256. A mesh with more shapes than this gets a body for each instance (as it would anyway).
	 */
	static constexpr int32 MaxShapesPerBody = 128;

	/**
	 * How many shapes the engine makes for one instance of this body setup (UBodySetup's shapes in ChaosInterface::CreateGeometry): the simple shapes unless the mesh is
	 * complex-as-simple and has triangles, the cooked triangle meshes unless it is simple-as-complex and has simple shapes. Needs the physics meshes created
	 * (UBodySetup::CreatePhysicsMeshes), which the component does before it asks; 0 for a body setup that makes no shapes.
	 */
	static int32 ShapesPerInstance(const UBodySetup* BodySetup);

	/** How many consecutive instances share one body: MaxShapesPerBody over the shapes of one instance, at least 1. */
	static int32 InstancesPerBody(const UBodySetup* BodySetup);

	/**
	 * The instances, in the component's space (the same list UInstancedStaticMeshComponent::AddInstances takes with bWorldSpace false). Call before the component registers: its bodies
	 * are made once, when its physics state is. Later, the list is replaced and the bodies made again.
	 */
	void SetInstances(TConstArrayView<FTransform> ComponentSpaceTransforms);

	/**
	 * More instances for a component that already stands (the later pieces of a long list, which the cell streamer adds to the first piece's component): they get bodies of their own,
	 * made at once, and the bounds and navigation follow. Before the component registers it is SetInstances' append.
	 */
	void AddInstances(TConstArrayView<FTransform> ComponentSpaceTransforms);

	int32 GetInstanceCount() const { return Instances.Num(); }

	/** The bodies of the instances, in order: instance runs of InstancesPerBody. Empty until the physics state is made, and while the component collides with nothing. */
	const TArray<FBodyInstance*>& GetChunkBodies() const { return ChunkBodies; }

	//~ Begin UPrimitiveComponent
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& BoundTransform) const override;
	virtual FBodyInstance* GetBodyInstance(FName BoneName = NAME_None, bool bGetWelded = true, int32 Index = INDEX_NONE) const override;
	virtual void OnActorEnableCollisionChanged() override;
	virtual void OnComponentCollisionSettingsChanged(bool bUpdateOverlaps = true) override;
	virtual void OnUpdateTransform(EUpdateTransformFlags UpdateTransformFlags, ETeleportType Teleport = ETeleportType::None) override;
	virtual bool LineTraceComponent(FHitResult& OutHit, const FVector Start, const FVector End, const FCollisionQueryParams& Params) override;
	virtual bool SweepComponent(FHitResult& OutHit, const FVector Start, const FVector End, const FQuat& ShapeWorldRotation, const FCollisionShape& CollisionShape, bool bTraceComplex = false) override;
	virtual bool OverlapComponent(const FVector& Pos, const FQuat& Rot, const FCollisionShape& CollisionShape) const override;
	//~ End UPrimitiveComponent

	//~ Begin UStaticMeshComponent
	virtual bool AllowsAsyncPhysicsStateCreation() const override { return false; }
	virtual bool DoCustomNavigableGeometryExport(FNavigableGeometryExport& GeomExport) const override;
	//~ End UStaticMeshComponent

	//~ Begin navigation: exactly what the engine's instanced component exports for the same instances
	virtual void GetNavigationData(FNavigationRelevantData& Data) const override;
	virtual FBox GetNavigationBounds() const override;
	virtual bool IsNavigationRelevant() const override;
	virtual bool ShouldSkipDirtyAreaOnAddOrRemove() const override { return true; }
	//~ End navigation

	//~ Begin UObject
	virtual void BeginDestroy() override;
	//~ End UObject

protected:
	//~ Begin UActorComponent
	virtual void OnRegister() override;
	virtual void OnUnregister() override;
	virtual void OnCreatePhysicsState() override;
	virtual void OnDestroyPhysicsState() override;
	virtual bool ShouldCreateRenderState() const override { return false; }
	virtual bool ComponentOverlapComponentImpl(UPrimitiveComponent* PrimComp, const FVector Pos, const FQuat& Quat, const FCollisionQueryParams& Params) override;
	virtual bool ComponentOverlapMultiImpl(TArray<FOverlapResult>& OutOverlaps, const UWorld* InWorld, const FVector& Pos, const FQuat& Rot, ECollisionChannel TestChannel, const FComponentQueryParams& Params, const FCollisionObjectQueryParams& ObjectQueryParams = FCollisionObjectQueryParams::DefaultObjectQueryParam) const override;
	//~ End UActorComponent

private:
	/** Makes the bodies of the instances from FirstInstance on, after the ones that exist. */
	void CreateChunkBodies(int32 FirstInstance);

	/** One body for a run of instances (world transforms): the first instance as the engine makes a body, the others as shapes added to it the way FBodyInstance::Weld adds a body's. */
	void CreateChunkBody(UBodySetup* BodySetup, FPhysScene_Chaos* PhysScene, TConstArrayView<FTransform> InstanceWorldTransforms);

	/** Adds the shapes of more instances (world transforms) to a body the engine made for the first of its run. */
	void AddInstanceShapes(FBodyInstance* Body, UBodySetup* BodySetup, FPhysScene_Chaos* PhysScene, TConstArrayView<FTransform> InstanceWorldTransforms);

	void ClearChunkBodies();

	/** The bounds the navigation system sees for one instance: the mesh's navigation collision bounds, or the mesh's own when it has none. */
	FBox GetInstanceNavigationBounds() const;

	/** The union of one instance bounds' box at every instance from FirstInstance on, with the component at BoundTransform. */
	FBoxSphereBounds CalcInstanceBounds(const FBox& InstanceBox, const FTransform& BoundTransform, int32 FirstInstance = 0) const;

	/** After instances were appended: the component's bounds and the cached navigation bounds grown by them, not worked out again over every instance. */
	void GrowBounds(int32 FirstNew);

	/** Where each instance of the navigation's area is, in the world (the per-instance transform delegate of the navigation data). */
	void GetNavigationPerInstanceTransforms(const FBox& AreaBox, TArray<FTransform>& InstanceData) const;

	/** Tells the navigation system which areas the instances from FirstInstance on cover, as the engine's instanced component does for the ones it adds or removes. */
	void DirtyNavigationAreas(int32 FirstInstance);

	/** Appends the instances, each as the engine's instanced component keeps one (through a matrix). The bounds are the caller's to bring up to date. */
	void AppendInstances(TConstArrayView<FTransform> ComponentSpaceTransforms);

	/**
	 * Every instance's transform in the component's space. A zero-scale one stays in the list (it counts) but has no shape, as it has no body in the engine's component.
	 * A property, as the engine's component's instances are, so an actor spawned from this one as its template (a script's Entity:Clone) has them too.
	 */
	UPROPERTY()
	TArray<FTransform> Instances;

	/** The component's transform the bodies were made at: a move to another one makes them again (OnUpdateTransform). */
	FTransform BodiesTransform = FTransform::Identity;

	/** The bodies, one for each run of InstancesPerBody consecutive instances that have a shape. Owned here, made and ended with the physics state. */
	TArray<FBodyInstance*> ChunkBodies;

	/** The navigation bounds, worked out once for the component's transform (the engine's component caches its bounds the same way): O(instances) to make. */
	mutable FBox CachedNavigationBounds = FBox(ForceInit);
	mutable FTransform CachedNavigationBoundsTransform = FTransform::Identity;
};
