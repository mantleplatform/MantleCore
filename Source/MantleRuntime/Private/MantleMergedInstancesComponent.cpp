// Copyright Joshua Eger. All Rights Reserved.
#include "MantleMergedInstancesComponent.h"

#include "MantleRuntime.h"

#include "AI/Navigation/NavCollisionBase.h"
#include "AI/Navigation/NavigationRelevantData.h"
#include "AI/NavigationModifier.h"
#include "AI/NavigationSystemBase.h"
#include "AI/NavigationSystemHelpers.h"
#include "Chaos/ParticleHandle.h"
#include "Chaos/ShapeInstance.h"
#include "Chaos/ImplicitObjectTransformed.h"
#include "Physics/Experimental/ChaosInterfaceUtils.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/OverlapResult.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Physics/PhysicsInterfaceCore.h"
#include "Physics/PhysicsInterfaceScene.h"
#include "Physics/PhysicsInterfaceTypes.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/PhysicsSettings.h"
#include "PhysicsProxy/SingleParticlePhysicsProxy.h"
#include "PhysicsPublic.h"
#include "PrimitiveComponentHelper.h"

namespace MantleMergedInstancesPrivate
{
	/** Dirty areas handed to the navigation system in one call, so a list of a million instances is never a million boxes at once. */
	constexpr int32 NavigationDirtyBatch = 4096;
}

UMantleMergedInstancesComponent::UMantleMergedInstancesComponent()
{
	// As the engine's instanced component: nothing simulates, and an overlap is told for every body that is overlapped (here one for each run of instances), not once for the component.
	BodyInstance.bSimulatePhysics = false;
	bMultiBodyOverlap = true;
}

// ---- Shapes and bodies ----------------------------------------------------------------------------------

int32 UMantleMergedInstancesComponent::ShapesPerInstance(const UBodySetup* BodySetup)
{
	if (!BodySetup)
	{
		return 0;
	}
	// The same decisions as ChaosInterface::CreateGeometry (ChaosInterfaceUtils.cpp), so the count is the shapes the engine makes: it never makes the simple ones for a complex-as-simple mesh that has
	// triangles, nor the triangles for a simple-as-complex mesh that has simple shapes. A convex whose hull was not cooked makes no shape.
	ECollisionTraceFlag TraceFlag = BodySetup->GetCollisionTraceFlag();
	if (TraceFlag == CTF_UseDefault)
	{
		TraceFlag = UPhysicsSettings::Get()->DefaultShapeComplexity;
	}
	const FKAggregateGeom& Geometry = BodySetup->AggGeom;
	const int32 TriangleMeshes = BodySetup->TriMeshGeometries.Num();
	const int32 SimpleElements = Geometry.SphereElems.Num() + Geometry.BoxElems.Num() + Geometry.SphylElems.Num() + Geometry.TaperedCapsuleElems.Num() + Geometry.ConvexElems.Num();
	const bool bMakesSimple = TraceFlag != CTF_UseComplexAsSimple || TriangleMeshes == 0;
	const bool bMakesComplex = TraceFlag != CTF_UseSimpleAsComplex || SimpleElements == 0;

	int32 Count = 0;
	if (bMakesSimple)
	{
		Count += Geometry.SphereElems.Num() + Geometry.BoxElems.Num() + Geometry.SphylElems.Num() + Geometry.TaperedCapsuleElems.Num();
		for (const FKConvexElem& Convex : Geometry.ConvexElems)
		{
			if (Convex.GetChaosConvexMesh())
			{
				++Count;
			}
		}
	}
	if (bMakesComplex)
	{
		Count += TriangleMeshes;
	}
	return Count;
}

int32 UMantleMergedInstancesComponent::InstancesPerBody(const UBodySetup* BodySetup)
{
	const int32 Shapes = ShapesPerInstance(BodySetup);
	return Shapes <= 0 ? 1 : FMath::Max(1, MaxShapesPerBody / Shapes);
}

void UMantleMergedInstancesComponent::OnCreatePhysicsState()
{
	// What UInstancedStaticMeshComponent::OnCreatePhysicsState does (InstancedStaticMesh.cpp): the primitive component's own single body is skipped, because this component's bodies are
	// the ones of its instances; the engine's navigation relevance step is made by hand, because UStaticMeshComponent's was skipped with it.
	if (UWorld* World = GetWorld())
	{
		if (World->GetPhysicsScene())
		{
			CreateChunkBodies(0);
		}
	}
	USceneComponent::OnCreatePhysicsState();
	bNavigationRelevant = IsNavigationRelevant();
}

void UMantleMergedInstancesComponent::OnDestroyPhysicsState()
{
	USceneComponent::OnDestroyPhysicsState();
	ClearChunkBodies();
	bNavigationRelevant = IsNavigationRelevant();
}

void UMantleMergedInstancesComponent::CreateChunkBodies(const int32 FirstInstance)
{
	UWorld* World = GetWorld();
	FPhysScene* PhysScene = World ? World->GetPhysicsScene() : nullptr;
	UBodySetup* BodySetup = GetBodySetup();
	if (!PhysScene || !BodySetup || FirstInstance >= Instances.Num())
	{
		return;
	}

	// The engine makes the cooked meshes when it makes the first body (CreateShapesAndActors); made first here because the count of shapes below reads them.
	BodySetup->CreatePhysicsMeshes();
	if (ShapesPerInstance(BodySetup) <= 0)
	{
		UE_LOG(LogMantle, Warning, TEXT("MANTLE_MERGED_BODIES %s: the mesh makes no collision shapes, so its %d instances have no bodies"), *GetPathName(), Instances.Num() - FirstInstance);
		return;
	}

	// The engine's instanced component sets this on the body it copies its instances' bodies from (FInstancedMeshComponentBodies::CreateAll), and a character's floor test reads it there.
	if (!BodyInstance.GetOverrideWalkableSlopeOnInstance())
	{
		BodyInstance.SetWalkableSlopeOverride(BodySetup->WalkableSlopeOverride, false);
	}

	const int32 PerBody = InstancesPerBody(BodySetup);
	const FTransform ComponentTransform = GetComponentTransform();
	BodiesTransform = ComponentTransform;
	TArray<FTransform> Run;
	Run.Reserve(PerBody);
	for (int32 Index = FirstInstance; Index < Instances.Num(); ++Index)
	{
		// An instance's world transform is the engine's own product (UInstancedStaticMeshComponent::CreateAllInstanceBodies): its matrix, then the component's transform.
		const FTransform InstanceTransform = Instances[Index] * ComponentTransform;
		if (InstanceTransform.GetScale3D().IsNearlyZero())
		{
			continue; // no body for it in the engine's component either
		}
		Run.Add(InstanceTransform);
		if (Run.Num() == PerBody)
		{
			CreateChunkBody(BodySetup, PhysScene, Run);
			Run.Reset();
		}
	}
	if (!Run.IsEmpty())
	{
		CreateChunkBody(BodySetup, PhysScene, Run);
	}
}

void UMantleMergedInstancesComponent::CreateChunkBody(UBodySetup* BodySetup, FPhysScene_Chaos* PhysScene, TConstArrayView<FTransform> InstanceWorldTransforms)
{
	const int32 ChunkIndex = ChunkBodies.Num();
	for (int32 First = 0; First < InstanceWorldTransforms.Num(); ++First)
	{
		// The first instance of the run is made as the engine makes the body of any instance (FInstancedMeshComponentBodies::InitInstanceBody): static, in place.
		FBodyInstance* Body = new FBodyInstance;
		Body->CopyBodyInstancePropertiesFrom(&BodyInstance);
		Body->InstanceBodyIndex = ChunkIndex;
		Body->bSimulatePhysics = false;
		Body->bAutoWeld = false;
		Body->InitBody(BodySetup, InstanceWorldTransforms[First], this, PhysScene);
		if (!Body->IsValidBodyInstance())
		{
			// The engine refused this instance (and said why); the next of the run takes its place.
			Body->TermBody();
			delete Body;
			continue;
		}
		ChunkBodies.Add(Body);
		AddInstanceShapes(Body, BodySetup, PhysScene, InstanceWorldTransforms.Slice(First + 1, InstanceWorldTransforms.Num() - First - 1));
		return;
	}
}

void UMantleMergedInstancesComponent::AddInstanceShapes(FBodyInstance* Body, UBodySetup* BodySetup, FPhysScene_Chaos* PhysScene, TConstArrayView<FTransform> InstanceWorldTransforms)
{
	if (InstanceWorldTransforms.IsEmpty())
	{
		return;
	}

	// Each further instance's shapes are made exactly as the engine makes the shapes of a body of its own at that instance (FInstancedMeshComponentBodies: InitBody at the
	// instance's world transform): ChaosInterface::CreateGeometry with the instance's scale and no local transform, so every sphere, box, capsule, hull and triangle mesh is the
	// same geometry, built about the instance's own origin. Each is then placed in this body by a rigid transform wrapper (rotation and translation only, the instance's frame
	// relative to the body's), which is what a separate body's own transform does for it. Not UBodySetup::AddShapesToRigidActor, as a welded body's shapes are made: that bakes
	// the offset into a sphere's, box's or capsule's own coordinates, kilometres from the body's origin, and a sweep against such a primitive does not answer as against the
	// engine's body (a mirrored box also came out a centimetre off).
	const FTransform BodyFrame(Body->GetUnrealWorldTransform(/*bWithProjection*/ false).GetRotation(), Body->GetUnrealWorldTransform(/*bWithProjection*/ false).GetTranslation());

	UPhysicalMaterial* SimpleMaterial = Body->GetSimplePhysicalMaterial();
	TArray<FPhysicalMaterialMaskParams> ComplexMaterialMasks;
	TArray<UPhysicalMaterial*> ComplexMaterials = Body->GetComplexPhysicalMaterials(ComplexMaterialMasks);
	FBodyCollisionData CollisionData;
	Body->BuildBodyFilterData(CollisionData.CollisionFilterData);
	FBodyInstance::BuildBodyCollisionFlags(CollisionData.CollisionFlags, Body->GetCollisionEnabled(), BodySetup->GetCollisionTraceFlag() == CTF_UseComplexAsSimple);

	FPhysicsCommand::ExecuteWrite(Body->GetPhysicsActor(), [&](const FPhysicsActorHandle& Actor)
	{
		// What the first instance's shapes were given after the body went into the scene (TInitBodiesHelperBase::InitBodies): the raw collision trace flag. The shapes made now take it
		// from the first, so a CTF_UseDefault mesh is treated alike by all of them.
		const auto TraceType = Actor->GetGameThreadAPI().ShapesArray()[0]->GetCollisionTraceType();
		const TArrayView<UPhysicalMaterial*> MaterialsView(ComplexMaterials);
		const TArrayView<FPhysicalMaterialMaskParams> MasksView(ComplexMaterialMasks);

		TArray<Chaos::FImplicitObjectPtr> Placed;
		Chaos::FShapesArray PlacedShapes;
		for (const FTransform& InstanceTransform : InstanceWorldTransforms)
		{
			FVector Scale = InstanceTransform.GetScale3D();
			if (Scale.IsNearlyZero())
			{
				Scale = FVector(0.1f); // as UBodySetup::AddShapesToRigidActor_AssumesLocked does for a body's own scale
			}
			const FTransform InstanceFrame(InstanceTransform.GetRotation(), InstanceTransform.GetTranslation());
			const FTransform Relative = InstanceFrame.GetRelativeTransform(BodyFrame);

			FGeometryAddParams AddParams;
			AddParams.bDoubleSided = BodySetup->bDoubleSidedGeometry;
			AddParams.CollisionData = CollisionData;
			AddParams.CollisionTraceType = BodySetup->GetCollisionTraceFlag();
			AddParams.Scale = Scale;
			AddParams.SimpleMaterial = SimpleMaterial;
			AddParams.ComplexMaterials = TArrayView<UPhysicalMaterial*>(ComplexMaterials);
			AddParams.ComplexMaterialMasks = TArrayView<FPhysicalMaterialMaskParams>(ComplexMaterialMasks);
			AddParams.LocalTransform = FTransform::Identity;
			AddParams.WorldTransform = InstanceFrame;
			AddParams.Geometry = &BodySetup->AggGeom;
			AddParams.TriMeshGeometries = MakeArrayView(BodySetup->TriMeshGeometries);

			TArray<Chaos::FImplicitObjectPtr> Geoms;
			Chaos::FShapesArray Shapes;
			ChaosInterface::CreateGeometry(AddParams, Geoms, Shapes);
			check(Geoms.Num() == Shapes.Num());
			const bool bPlaced = !Relative.GetTranslation().IsNearlyZero() || !Relative.GetRotation().IsIdentity();
			for (int32 Index = 0; Index < Geoms.Num(); ++Index)
			{
				Chaos::FImplicitObjectPtr Geometry = bPlaced
					? MakeImplicitObjectPtr<Chaos::TImplicitObjectTransformed<Chaos::FReal, 3>>(Geoms[Index], Chaos::FRigidTransform3(Relative))
					: Geoms[Index];
				// A new shape around the placed geometry, with everything CreateGeometry gave the instance's own shape: its filters, its collision switches and its user data.
				const TUniquePtr<Chaos::FPerShapeData>& Made = Shapes[Index];
				TUniquePtr<Chaos::FPerShapeData> Shape = Chaos::FShapeInstanceProxy::Make(PlacedShapes.Num(), Geometry);
				Shape->SetShapeFilterData(Made->GetShapeFilterData());
				Shape->SetFilterInstanceData(Made->GetFilterInstanceData());
				Shape->SetQueryEnabled(Made->GetQueryEnabled());
				Shape->SetSimEnabled(Made->GetSimEnabled());
				Shape->SetIsProbe(Made->GetIsProbe());
				Shape->SetUserData(Made->GetUserData());
				Shape->SetCollisionTraceType(TraceType);
				Shape->UpdateShapeBounds(Chaos::FRigidTransform3(BodyFrame));
				FBodyInstance::ApplyMaterialToShape_AssumesLocked(FPhysicsShapeHandle(Shape.Get(), Actor), SimpleMaterial, MaterialsView, &MasksView);
				Placed.Add(MoveTemp(Geometry));
				PlacedShapes.Add(MoveTemp(Shape));
			}
		}

		// As FPhysInterface_Chaos::AddGeometry adds a body setup's shapes: the geometry into the body's union (once for the whole run), then the shapes, which fill in its new entries.
		const int32 NewShapes = PlacedShapes.Num();
		Actor->GetGameThreadAPI().MergeGeometry(MoveTemp(Placed));
		Actor->GetGameThreadAPI().MergeShapesArray(MoveTemp(PlacedShapes));
		const Chaos::FShapesArray& All = Actor->GetGameThreadAPI().ShapesArray();
		for (int32 Index = All.Num() - NewShapes; Index < All.Num(); ++Index)
		{
			All[Index]->SetProxy(Actor);
		}

		// Weld ends with FBodyInstance::PostShapeChange, whose UpdatePhysicsFilterData is what puts the grown body into the scene's query structure again (and sets filter data, which the
		// shapes already carry here, and mass, which a static body has none of). Called alone: UpdatePhysicsFilterData would read a shape's place in the body's list as a place in the mesh's.
		PhysScene->UpdateActorInAccelerationStructure(Actor);
	});
}

void UMantleMergedInstancesComponent::ClearChunkBodies()
{
	for (FBodyInstance* Body : ChunkBodies)
	{
		Body->TermBody();
		delete Body;
	}
	ChunkBodies.Empty();
}

void UMantleMergedInstancesComponent::BeginDestroy()
{
	ClearChunkBodies();
	Super::BeginDestroy();
}

// ---- Instances ------------------------------------------------------------------------------------------

void UMantleMergedInstancesComponent::AppendInstances(TConstArrayView<FTransform> ComponentSpaceTransforms)
{
	if (Instances.IsEmpty())
	{
		// Exactly the first list's size; later pieces grow the array the usual way (an exact Reserve for each of thousands of pieces would copy the whole list every time).
		Instances.Reserve(ComponentSpaceTransforms.Num());
	}
	for (const FTransform& Transform : ComponentSpaceTransforms)
	{
		// The engine's instanced component keeps an instance as a matrix and reads it back (SetupNewInstanceData); a mirrored instance comes back with the same matrix but its own split into rotation and
		// scale. Going through the matrix here too makes every instance exactly the transform the engine's component would have made its body from.
		Instances.Add(FTransform(Transform.ToMatrixWithScale()));
	}
}

void UMantleMergedInstancesComponent::GrowBounds(const int32 FirstNew)
{
	// A long list arrives a hundred at a time (the cell streamer's pieces): working the bounds out again over all of the instances each time would be quadratic.
	const UStaticMesh* Mesh = GetStaticMesh();
	if (!Mesh || FirstNew >= Instances.Num())
	{
		return;
	}
	const FBox MeshBox = Mesh->GetBounds().GetBox();
	if (MeshBox.IsValid)
	{
		FBoxSphereBounds Added = CalcInstanceBounds(MeshBox, GetComponentTransform(), FirstNew);
		Added.BoxExtent *= BoundsScale;
		Added.SphereRadius *= BoundsScale;
		Bounds = FirstNew == 0 ? Added : Bounds + Added;
	}
	if (CachedNavigationBounds.IsValid)
	{
		const FBox NavigationBox = GetInstanceNavigationBounds();
		if (NavigationBox.IsValid)
		{
			CachedNavigationBounds += CalcInstanceBounds(NavigationBox, CachedNavigationBoundsTransform, FirstNew).GetBox();
		}
		else
		{
			CachedNavigationBounds = FBox(ForceInit);
		}
	}
}

void UMantleMergedInstancesComponent::SetInstances(TConstArrayView<FTransform> ComponentSpaceTransforms)
{
	Instances.Reset();
	CachedNavigationBounds = FBox(ForceInit);
	AppendInstances(ComponentSpaceTransforms);
	if (IsRegistered())
	{
		UpdateBounds();
		RecreatePhysicsState();
	}
}

void UMantleMergedInstancesComponent::AddInstances(TConstArrayView<FTransform> ComponentSpaceTransforms)
{
	const int32 FirstNew = Instances.Num();
	AppendInstances(ComponentSpaceTransforms);
	if (!IsRegistered() || ComponentSpaceTransforms.IsEmpty())
	{
		// Not standing yet: the bounds and the navigation bounds are made when something first asks.
		CachedNavigationBounds = FBox(ForceInit);
		return;
	}
	GrowBounds(FirstNew);
	if (IsPhysicsStateCreated())
	{
		// Bodies of their own, in runs of their own: the run the earlier instances ended in is not grown (a piece of 100 solid instances nearly fills a run anyway).
		CreateChunkBodies(FirstNew);
	}
	if (IsNavigationRelevant())
	{
		if (FirstNew == 0)
		{
			// Relevance needs an instance, so the component was never registered with navigation (the engine's component does this for its first instance too).
			bNavigationRelevant = true;
			FNavigationSystem::RegisterComponent(*this);
		}
		DirtyNavigationAreas(FirstNew);
	}
}

// ---- Not drawn ------------------------------------------------------------------------------------------

FPrimitiveSceneProxy* UMantleMergedInstancesComponent::CreateSceneProxy()
{
	return nullptr;
}

FBoxSphereBounds UMantleMergedInstancesComponent::CalcInstanceBounds(const FBox& InstanceBox, const FTransform& BoundTransform, const int32 FirstInstance) const
{
	FBoxSphereBounds::Builder Builder;
	for (int32 Index = FirstInstance; Index < Instances.Num(); ++Index)
	{
		Builder += InstanceBox.TransformBy(Instances[Index] * BoundTransform);
	}
	return Builder;
}

FBoxSphereBounds UMantleMergedInstancesComponent::CalcBounds(const FTransform& BoundTransform) const
{
	FBoxSphereBounds Result(BoundTransform.GetLocation(), FVector::ZeroVector, 0.0);
	if (const UStaticMesh* Mesh = GetStaticMesh())
	{
		const FBox MeshBox = Mesh->GetBounds().GetBox();
		if (!Instances.IsEmpty() && MeshBox.IsValid)
		{
			Result = CalcInstanceBounds(MeshBox, BoundTransform);
		}
	}
	Result.BoxExtent *= BoundsScale;
	Result.SphereRadius *= BoundsScale;
	return Result;
}

// ---- Queries --------------------------------------------------------------------------------------------

FBodyInstance* UMantleMergedInstancesComponent::GetBodyInstance(FName BoneName, bool bGetWelded, int32 Index) const
{
	// A hit's Item is the index of the body the shape belongs to.
	if (ChunkBodies.IsValidIndex(Index))
	{
		return ChunkBodies[Index];
	}
	return const_cast<FBodyInstance*>(&BodyInstance);
}

void UMantleMergedInstancesComponent::OnActorEnableCollisionChanged()
{
	Super::OnActorEnableCollisionChanged();
	// As UInstancedStaticMeshComponent does for its bodies. (Only an actor-wide collision switch gets here; a component's own SetCollisionEnabled recreates the bodies instead.)
	for (FBodyInstance* Body : ChunkBodies)
	{
		Body->UpdatePhysicsFilterData();
	}
}

void UMantleMergedInstancesComponent::OnUpdateTransform(EUpdateTransformFlags UpdateTransformFlags, ETeleportType Teleport)
{
	Super::OnUpdateTransform(UpdateTransformFlags, Teleport);
	// A set a server holds stands Static and never moves. A copy of one a script made (Entity:Clone, Movable) can be moved: its bodies are made again where it now stands,
	// as the engine's component moves its instances' bodies. One entity's few bodies; a copy moved every frame pays that every frame.
	if (IsPhysicsStateCreated() && !ChunkBodies.IsEmpty() && !GetComponentTransform().Equals(BodiesTransform, UE_KINDA_SMALL_NUMBER))
	{
		ClearChunkBodies();
		CreateChunkBodies(0);
	}
}

void UMantleMergedInstancesComponent::OnComponentCollisionSettingsChanged(bool bUpdateOverlaps)
{
	// The bodies were made from the component's collision settings (CopyBodyInstancePropertiesFrom), and their shapes carry the filter data, so a change made after they stand (a
	// script turning the entity's collision off or on, a response to a channel) is applied by making them again. Not left to the physics state: an editor world that keeps trace
	// collision keeps the physics state of a component with no collision (UPrimitiveComponent::ShouldCreatePhysicsState), and query-only against query-and-physics changes no
	// physics state at all. Rare (a script's change to a whole entity), and one entity's bodies are a few.
	if (IsRegistered() && IsPhysicsStateCreated())
	{
		ClearChunkBodies();
		if (BodyInstance.GetCollisionEnabled() != ECollisionEnabled::NoCollision)
		{
			CreateChunkBodies(0);
		}
	}
	Super::OnComponentCollisionSettingsChanged(bUpdateOverlaps);
}

bool UMantleMergedInstancesComponent::LineTraceComponent(FHitResult& OutHit, const FVector Start, const FVector End, const FCollisionQueryParams& Params)
{
	// The nearest hit of any body, as FInstancedMeshComponentBodies::LineTrace takes it.
	bool bHaveHit = false;
	float MinTime = UE_MAX_FLT;
	FHitResult Hit;
	for (FBodyInstance* Body : ChunkBodies)
	{
		if (Body->LineTrace(Hit, Start, End, Params.bTraceComplex, Params.bReturnPhysicalMaterial))
		{
			bHaveHit = true;
			if (MinTime > Hit.Time)
			{
				MinTime = Hit.Time;
				OutHit = Hit;
			}
		}
	}
	return bHaveHit;
}

bool UMantleMergedInstancesComponent::SweepComponent(FHitResult& OutHit, const FVector Start, const FVector End, const FQuat& ShapeWorldRotation, const FCollisionShape& CollisionShape, bool bTraceComplex)
{
	bool bHaveHit = false;
	FHitResult Hit;
	for (FBodyInstance* Body : ChunkBodies)
	{
		if (Body->Sweep(Hit, Start, End, ShapeWorldRotation, CollisionShape, bTraceComplex))
		{
			if (!bHaveHit || Hit.Time < OutHit.Time)
			{
				OutHit = Hit;
			}
			bHaveHit = true;
		}
	}
	return bHaveHit;
}

bool UMantleMergedInstancesComponent::OverlapComponent(const FVector& Pos, const FQuat& Rot, const FCollisionShape& CollisionShape) const
{
	for (const FBodyInstance* Body : ChunkBodies)
	{
		if (Body->OverlapTest(Pos, Rot, CollisionShape))
		{
			return true;
		}
	}
	return false;
}

bool UMantleMergedInstancesComponent::ComponentOverlapComponentImpl(UPrimitiveComponent* PrimComp, const FVector Pos, const FQuat& Quat, const FCollisionQueryParams& Params)
{
	// The engine does not test one set of instances against another.
	if (PrimComp->IsA<UInstancedStaticMeshComponent>() || PrimComp->IsA<UMantleMergedInstancesComponent>())
	{
		return false;
	}
	FBodyInstance* OtherBody = PrimComp->GetBodyInstance();
	return OtherBody && OtherBody->OverlapTestForBodies(Pos, Quat, ChunkBodies);
}

bool UMantleMergedInstancesComponent::ComponentOverlapMultiImpl(TArray<FOverlapResult>& OutOverlaps, const UWorld* InWorld, const FVector& Pos, const FQuat& Rot, ECollisionChannel TestChannel, const FComponentQueryParams& Params, const FCollisionObjectQueryParams& ObjectQueryParams) const
{
	OutOverlaps.Reset();

	const FTransform WorldToComponent(GetComponentTransform().Inverse());
	const FCollisionResponseParams ResponseParams(GetCollisionResponseToChannels());
	FComponentQueryParams ParamsWithSelf = Params;
	ParamsWithSelf.AddIgnoredComponent(this);

	bool bHaveBlockingHit = false;
	for (const FBodyInstance* Body : ChunkBodies)
	{
		if (Body->OverlapMulti(OutOverlaps, InWorld ? InWorld : GetWorld(), &WorldToComponent, Pos, Rot, TestChannel, ParamsWithSelf, ResponseParams, ObjectQueryParams))
		{
			bHaveBlockingHit = true;
		}
	}
	return bHaveBlockingHit;
}

// ---- Navigation -----------------------------------------------------------------------------------------

FBox UMantleMergedInstancesComponent::GetInstanceNavigationBounds() const
{
	if (const UStaticMesh* Mesh = GetStaticMesh())
	{
		const UNavCollisionBase* NavCollision = Mesh->GetNavCollision();
		const FBox NavBounds = NavCollision ? NavCollision->GetBounds() : FBox(ForceInit);
		return NavBounds.IsValid ? NavBounds : Mesh->GetBounds().GetBox();
	}
	return FBox(ForceInit);
}

bool UMantleMergedInstancesComponent::IsNavigationRelevant() const
{
	return !Instances.IsEmpty() && Super::IsNavigationRelevant();
}

FBox UMantleMergedInstancesComponent::GetNavigationBounds() const
{
	const FTransform& ComponentTransform = GetComponentTransform();
	if (CachedNavigationBounds.IsValid && CachedNavigationBoundsTransform.Equals(ComponentTransform))
	{
		return CachedNavigationBounds;
	}
	const FBox InstanceBox = GetInstanceNavigationBounds();
	FBox Result = FBoxSphereBounds(ComponentTransform.GetLocation(), FVector::ZeroVector, 0.0).GetBox();
	if (GetStaticMesh() && !Instances.IsEmpty() && InstanceBox.IsValid)
	{
		Result = CalcInstanceBounds(InstanceBox, ComponentTransform).GetBox();
	}
	CachedNavigationBounds = Result;
	CachedNavigationBoundsTransform = ComponentTransform;
	return Result;
}

void UMantleMergedInstancesComponent::GetNavigationData(FNavigationRelevantData& Data) const
{
	// FInstancedStaticMeshComponentHelper::GetNavigationData, for this component's instances.
	// The engine's own helper (UPrimitiveComponent::AddNavigationModifier, which it calls, is not exported to modules).
	FPrimitiveComponentHelper::AddNavigationModifier(*this, Data);

	UStaticMesh* MeshAsset = GetStaticMesh();
	if (MeshAsset && !MeshAsset->IsCompiling() && MeshAsset->GetNavCollision())
	{
		UNavCollisionBase* NavCollision = MeshAsset->GetNavCollision();
		if (ShouldExportAsObstacle(*NavCollision))
		{
			Data.Modifiers.MarkAsPerInstanceModifier();
			NavCollision->GetNavigationModifier(Data.Modifiers, FTransform::Identity);
			Data.NavDataPerInstanceTransformDelegate = FNavDataPerInstanceTransformDelegate::CreateUObject(this, &UMantleMergedInstancesComponent::GetNavigationPerInstanceTransforms);
		}
	}
}

bool UMantleMergedInstancesComponent::DoCustomNavigableGeometryExport(FNavigableGeometryExport& GeomExport) const
{
	// FInstancedStaticMeshComponentHelper::DoCustomNavigableGeometryExport: the mesh's geometry once, with the instances' places given by the delegate.
	UStaticMesh* MeshAsset = GetStaticMesh();
	if (MeshAsset && MeshAsset->GetNavCollision())
	{
		UNavCollisionBase* NavCollision = MeshAsset->GetNavCollision();
		if (ShouldExportAsObstacle(*NavCollision))
		{
			return false;
		}
		if (NavCollision->HasConvexGeometry())
		{
			NavCollision->ExportGeometry(FTransform::Identity, GeomExport);
		}
		else if (UBodySetup* NavBodySetup = MeshAsset->GetBodySetup())
		{
			GeomExport.ExportRigidBodySetup(*NavBodySetup, FTransform::Identity);
		}
		GeomExport.SetNavDataPerInstanceTransformDelegate(FNavDataPerInstanceTransformDelegate::CreateUObject(this, &UMantleMergedInstancesComponent::GetNavigationPerInstanceTransforms));
	}
	// The component's own collision is never exported whole.
	return false;
}

void UMantleMergedInstancesComponent::GetNavigationPerInstanceTransforms(const FBox& AreaBox, TArray<FTransform>& InstanceData) const
{
	// FInstancedStaticMeshComponentHelper::GetNavigationPerInstanceTransforms.
	const FBox InstanceBounds = GetInstanceNavigationBounds();
	if (!InstanceBounds.IsValid)
	{
		return;
	}
	const FTransform& ComponentTransform = GetComponentTransform();
	const FBox LocalAreaBox = AreaBox.InverseTransformBy(ComponentTransform);
	for (const FTransform& InstanceToComponent : Instances)
	{
		if (InstanceToComponent.GetScale3D().IsZero())
		{
			continue;
		}
		if (LocalAreaBox.Intersect(InstanceBounds.TransformBy(InstanceToComponent)))
		{
			InstanceData.Add(InstanceToComponent * ComponentTransform);
		}
	}
}

void UMantleMergedInstancesComponent::DirtyNavigationAreas(const int32 FirstInstance)
{
	const FBox InstanceBox = GetInstanceNavigationBounds();
	if (!InstanceBox.IsValid || FirstInstance >= Instances.Num())
	{
		return;
	}
	const FTransform& ComponentTransform = GetComponentTransform();
	const FBox NewBounds = GetNavigationBounds();
	TArray<FBox> DirtyAreas;
	DirtyAreas.Reserve(FMath::Min(Instances.Num() - FirstInstance, MantleMergedInstancesPrivate::NavigationDirtyBatch));
	for (int32 Index = FirstInstance; Index < Instances.Num(); ++Index)
	{
		DirtyAreas.Add(InstanceBox.TransformBy(Instances[Index] * ComponentTransform));
		if (DirtyAreas.Num() == MantleMergedInstancesPrivate::NavigationDirtyBatch)
		{
			FNavigationSystem::OnObjectBoundsChanged(*this, NewBounds, DirtyAreas);
			DirtyAreas.Reset();
		}
	}
	if (!DirtyAreas.IsEmpty())
	{
		FNavigationSystem::OnObjectBoundsChanged(*this, NewBounds, DirtyAreas);
	}
}

void UMantleMergedInstancesComponent::OnRegister()
{
	Super::OnRegister();
	// ShouldSkipDirtyAreaOnAddOrRemove is true, as the engine's instanced component says: the navigation system does not dirty the whole area the bounds cover when this registers or leaves,
	// so the areas around the instances are given here (UInstancedStaticMeshComponent::OnRegister and OnUnregister).
	if (IsNavigationRelevant())
	{
		DirtyNavigationAreas(0);
	}
}

void UMantleMergedInstancesComponent::OnUnregister()
{
	if (bNavigationRelevant)
	{
		DirtyNavigationAreas(0);
	}
	Super::OnUnregister();
}
