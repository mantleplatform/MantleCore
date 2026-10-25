// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePhysicsProp.h"
#include "MantleRuntime.h"

#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Net/UnrealNetwork.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "PhysicsEngine/BodySetup.h"

const TCHAR* MantlePhysicsBodySourceToString(EMantlePhysicsBodySource Source)
{
	switch (Source)
	{
	case EMantlePhysicsBodySource::CopiedShapes: return TEXT("copied_shapes");
	case EMantlePhysicsBodySource::BoundsBox:    return TEXT("bounds_box");
	case EMantlePhysicsBodySource::Mesh:
	default:                                     return TEXT("mesh");
	}
}

FName MantlePhysicsAnchoredTag()
{
	static const FName Tag(TEXT("MantleAnchored"));
	return Tag;
}

// ---- Physics material presets ------------------------------------------------------------

namespace MantlePhysicsMaterialPrivate
{
	/**
	 * In EMantlePhysicsMaterial order. Default is the engine's own default surface; the rest are
	 * picked to read clearly in play rather than to match a handbook: rubber bounces off anything,
	 * ice slides on anything, metal is dense and a little slick, wood is light, stone is heavy,
	 * grippy and dead.
	 */
	const FMantlePhysicsMaterialPreset GPresets[] = {
		{ TEXT("PM_MantleDefault"), 0.7f,  0.3f,  1.0f,  false, false },
		{ TEXT("PM_MantleRubber"),  0.9f,  0.8f,  1.1f,  false, true  },
		{ TEXT("PM_MantleIce"),     0.02f, 0.05f, 0.92f, true,  false },
		{ TEXT("PM_MantleMetal"),   0.4f,  0.2f,  7.8f,  false, false },
		{ TEXT("PM_MantleWood"),    0.6f,  0.25f, 0.7f,  false, false },
		{ TEXT("PM_MantleStone"),   0.8f,  0.1f,  2.5f,  false, false },
	};
	static_assert(static_cast<int32>(UE_ARRAY_COUNT(GPresets)) == MantlePhysicsMaterialCount, "one preset per EMantlePhysicsMaterial");

	int32 PresetIndex(EMantlePhysicsMaterial Material)
	{
		const int32 Index = static_cast<int32>(Material);
		return (Index >= 0 && Index < MantlePhysicsMaterialCount) ? Index : 0;
	}
}

const FMantlePhysicsMaterialPreset& MantlePhysicsMaterialPreset(EMantlePhysicsMaterial Material)
{
	return MantlePhysicsMaterialPrivate::GPresets[MantlePhysicsMaterialPrivate::PresetIndex(Material)];
}

FString MantlePhysicsMaterialAssetPath(EMantlePhysicsMaterial Material)
{
	const TCHAR* Name = MantlePhysicsMaterialPreset(Material).AssetName;
	return FString::Printf(TEXT("/Game/Mantle/Physics/%s.%s"), Name, Name);
}

UPhysicalMaterial* MantleLoadPhysicsMaterial(EMantlePhysicsMaterial Material)
{
	// A missing preset stays missing for the process (a cooked build can't gain one), so it is
	// looked for, and logged, once rather than for each of up to 500 props.
	static bool bMissing[MantlePhysicsMaterialCount] = {};
	const int32 Index = MantlePhysicsMaterialPrivate::PresetIndex(Material);
	if (bMissing[Index])
	{
		return nullptr;
	}
	const FString Path = MantlePhysicsMaterialAssetPath(Material);
	UPhysicalMaterial* Loaded = LoadObject<UPhysicalMaterial>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!Loaded)
	{
		bMissing[Index] = true;
		UE_LOG(LogMantle, Warning,
			TEXT("MANTLE_PHYSICS_MATERIAL_MISSING preset=%s path=%s: this build has no such physical material, so bodies with it keep their mesh's surface. ")
			TEXT("Create it with tools/physics/Create-MantlePhysicalMaterials.py and cook /Game/Mantle/Physics."),
			MantlePhysicsMaterialToString(Material), *Path);
	}
	return Loaded;
}

// ---- UMantlePhysicsMeshComponent ----------------------------------------------------------------

EMantlePhysicsBodySource UMantlePhysicsMeshComponent::PrepareSimulationBody(UStaticMesh* Mesh)
{
	SimulationBodySetup = nullptr;
	UBodySetup* MeshSetup = Mesh ? Mesh->GetBodySetup() : nullptr;
	const bool bHasSimpleShapes = MeshSetup && MeshSetup->AggGeom.GetElementCount() > 0;
	const bool bComplexAsSimple = MeshSetup && MeshSetup->CollisionTraceFlag == CTF_UseComplexAsSimple;
	if (bHasSimpleShapes && !bComplexAsSimple)
	{
		return EMantlePhysicsBodySource::Mesh;
	}

	// Simple-as-complex: queries (a player standing on the crate, a trace) hit the same shapes the
	// body simulates on, on the server and on every client alike.
	UBodySetup* Own = NewObject<UBodySetup>(this, NAME_None, RF_Transient);
	Own->BodySetupGuid = FGuid::NewGuid();
	Own->CollisionTraceFlag = CTF_UseSimpleAsComplex;
	if (MeshSetup)
	{
		Own->PhysMaterial = MeshSetup->PhysMaterial;
	}

	EMantlePhysicsBodySource Source = EMantlePhysicsBodySource::BoundsBox;
	if (bHasSimpleShapes)
	{
		Own->AddCollisionFrom(MeshSetup->AggGeom);
		Source = EMantlePhysicsBodySource::CopiedShapes;
	}
	else
	{
		// No simple collision at all (an uploaded mesh collides on its triangles): a box the size of
		// the mesh's local bounds. At least a centimetre on each axis, so a flat mesh still has volume.
		FBox MeshBounds = Mesh ? Mesh->GetBoundingBox() : FBox(ForceInit);
		if (!MeshBounds.IsValid)
		{
			MeshBounds = FBox(FVector(-50.0), FVector(50.0));
		}
		const FVector Size = MeshBounds.GetSize().ComponentMax(FVector(1.0));
		FKBoxElem Box(static_cast<float>(Size.X), static_cast<float>(Size.Y), static_cast<float>(Size.Z));
		Box.Center = MeshBounds.GetCenter();
		Own->AggGeom.BoxElems.Add(Box);
	}
	Own->CreatePhysicsMeshes();
	SimulationBodySetup = Own;
	return Source;
}

UBodySetup* UMantlePhysicsMeshComponent::GetBodySetup()
{
	return SimulationBodySetup ? SimulationBodySetup.Get() : Super::GetBodySetup();
}

// ---- AMantlePhysicsProp -------------------------------------------------------------------------

AMantlePhysicsProp::AMantlePhysicsProp(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer.SetDefaultSubobjectClass<UMantlePhysicsMeshComponent>(AStaticMeshActor::StaticMeshComponentName))
{
	// Server-authoritative, replicated through the engine's movement replication (see the header).
	// Always relevant: a client's copy is the only one it has once the placeholder is adopted, and a
	// relevancy drop would destroy it; the per-place cap bounds how many there are.
	bReplicates = true;
	SetReplicatingMovement(true);
	bStaticMeshReplicateMovement = true;
	bAlwaysRelevant = true;
	SetNetUpdateFrequency(30.0f);
	if (UStaticMeshComponent* Mesh = GetStaticMeshComponent())
	{
		Mesh->SetMobility(EComponentMobility::Movable);
	}
}

EMantlePhysicsBuild AMantlePhysicsProp::ChooseBuild(bool bRuntimeBuild, ENetMode NetMode)
{
	if (!bRuntimeBuild)
	{
		return EMantlePhysicsBuild::Authoring;
	}
	return NetMode == NM_Client ? EMantlePhysicsBuild::Placeholder : EMantlePhysicsBuild::Prop;
}

void AMantlePhysicsProp::ApplyBodySettings(UPrimitiveComponent* Component, const FMantlePhysicsProps& InPhysics)
{
	if (!Component)
	{
		return;
	}
	if (InPhysics.Mass.IsSet())
	{
		Component->SetMassOverrideInKg(NAME_None, static_cast<float>(InPhysics.Mass.GetValue()), true);
	}
	Component->SetLinearDamping(static_cast<float>(InPhysics.LinearDamping));
	Component->SetAngularDamping(static_cast<float>(InPhysics.AngularDamping));
	Component->SetEnableGravity(InPhysics.bGravity);
	ApplyPhysicsMaterial(Component, InPhysics.Material);
}

void AMantlePhysicsProp::ApplyPhysicsMaterial(UPrimitiveComponent* Component, EMantlePhysicsMaterial Material)
{
	UPhysicalMaterial* Surface = Component ? MantleLoadPhysicsMaterial(Material) : nullptr;
	if (!Surface)
	{
		return;
	}
	Component->SetPhysMaterialOverride(Surface);
	// Swapping the material changes the shapes' friction and restitution, not the body's mass: redo
	// that so the preset's density counts. A place mass is an override, which this keeps.
	if (FBodyInstance* Body = Component->GetBodyInstance(); Body && Body->IsValidBodyInstance())
	{
		Body->UpdateMassProperties();
	}
}

UMantlePhysicsMeshComponent* AMantlePhysicsProp::GetPhysicsMeshComponent() const
{
	return Cast<UMantlePhysicsMeshComponent>(GetStaticMeshComponent());
}

void AMantlePhysicsProp::InitFromPlace(const FString& InEntityId, const FMantlePhysicsProps& InPhysics)
{
	EntityId = InEntityId;
	Physics = InPhysics;
	PhysicsMaterial = static_cast<uint8>(Physics.Material);
	UMantlePhysicsMeshComponent* Component = GetPhysicsMeshComponent();
	if (!Component)
	{
		return;
	}
	BodySource = Component->PrepareSimulationBody(Component->GetStaticMesh());
	Component->RecreatePhysicsState();
	ApplyBodySettings(Component, Physics);
}

void AMantlePhysicsProp::BeginSimulation()
{
	if (GetAttachParentActor())
	{
		DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
	}
	UStaticMeshComponent* Component = GetStaticMeshComponent();
	if (Component && Physics.bSimulate)
	{
		Component->SetSimulatePhysics(true);
	}
	UE_LOG(LogMantle, Log, TEXT("MANTLE_PHYSICS_PROP entity=%s simulate=%d body=%s mass_kg=%.2f"),
		*EntityId, IsSimulating() ? 1 : 0, MantlePhysicsBodySourceToString(BodySource), GetMassKg());
}

bool AMantlePhysicsProp::AdoptPlaceholder(AActor* Placeholder)
{
	const AStaticMeshActor* Source = Cast<AStaticMeshActor>(Placeholder);
	UStaticMeshComponent* From = Source ? Source->GetStaticMeshComponent() : nullptr;
	UMantlePhysicsMeshComponent* To = GetPhysicsMeshComponent();
	if (!From || !To || Placeholder == this)
	{
		return false;
	}

	// Location and rotation are the server's (replicated movement); scale is not replicated, and the
	// placeholder has the place's.
	SetActorScale3D(Placeholder->GetActorScale3D());

	UStaticMesh* Mesh = From->GetStaticMesh();
	BodySource = To->PrepareSimulationBody(Mesh);
	if (!To->SetStaticMesh(Mesh))
	{
		To->RecreatePhysicsState();
	}
	for (int32 Slot = 0; Slot < From->GetNumOverrideMaterials(); ++Slot)
	{
		if (UMaterialInterface* Material = From->OverrideMaterials[Slot])
		{
			To->SetMaterial(Slot, Material);
		}
	}
	To->SetCastShadow(From->CastShadow);
	// #630: the place's drawDistance, as the placeholder was built with it (0, never culled, when it has none).
	To->SetCullDistance(From->LDMaxDrawDistance);
	To->SetCollisionProfileName(UCollisionProfile::CustomCollisionProfileName);
	To->SetCollisionEnabled(From->GetCollisionEnabled());
	To->SetCollisionResponseToChannels(From->GetCollisionResponseToChannels());
	SetActorHiddenInGame(Placeholder->IsHidden());
	ApplyBodySettings(To, Physics);

	// Whatever the place parented to this entity follows the prop from now on.
	TArray<AActor*> AttachedChildren;
	Placeholder->GetAttachedActors(AttachedChildren, /*bResetArray*/ true, /*bRecursivelyIncludeAttachedActors*/ false);
	for (AActor* Child : AttachedChildren)
	{
		if (IsValid(Child))
		{
			Child->AttachToActor(this, FAttachmentTransformRules::KeepRelativeTransform);
		}
	}
	Placeholder->Destroy();

	// The engine keeps this in step with the server from here on (AActor::SyncReplicatedPhysicsSimulation).
	To->SetSimulatePhysics(GetReplicatedMovement().bRepPhysics);
	if (GetReplicatedMovement().bRepPhysics)
	{
		// The pose that arrived before this client built the place reached a body that was not
		// simulating yet, so physics replication has no target for it. Hand it over again, and sleep
		// if the server's body sleeps: a prop held still by a server-only joint (a lamp on a rope)
		// would otherwise fall under this client's gravity, and a sleeping server body sends
		// nothing that would ever correct it.
		OnRep_ReplicatedMovement();
		if (GetReplicatedMovement().bSimulatedPhysicSleep)
		{
			To->PutRigidBodyToSleep();
		}
	}
	bAdopted = true;
	return true;
}

bool AMantlePhysicsProp::IsSimulating() const
{
	const UStaticMeshComponent* Component = GetStaticMeshComponent();
	return Component && Component->IsSimulatingPhysics();
}

void AMantlePhysicsProp::SetAnchored(bool bAnchored)
{
	UStaticMeshComponent* Component = GetStaticMeshComponent();
	if (!Component || bAnchored == !IsSimulating())
	{
		return;
	}
	Component->SetSimulatePhysics(!bAnchored);
	if (!bAnchored)
	{
		Component->WakeRigidBody();
	}
}

void AMantlePhysicsProp::ApplyImpulse(const FVector& Impulse)
{
	UStaticMeshComponent* Component = GetStaticMeshComponent();
	if (Component && IsSimulating())
	{
		Component->AddImpulse(Impulse);
	}
}

void AMantlePhysicsProp::ApplyAngularImpulse(const FVector& ImpulseDegrees)
{
	UStaticMeshComponent* Component = GetStaticMeshComponent();
	if (Component && IsSimulating())
	{
		Component->AddAngularImpulseInDegrees(ImpulseDegrees);
	}
}

FVector AMantlePhysicsProp::GetLinearVelocity() const
{
	const UStaticMeshComponent* Component = GetStaticMeshComponent();
	if (Component && IsSimulating())
	{
		return Component->GetPhysicsLinearVelocity();
	}
	return GetVelocity();
}

void AMantlePhysicsProp::SetLinearVelocity(const FVector& Velocity)
{
	UStaticMeshComponent* Component = GetStaticMeshComponent();
	if (Component && IsSimulating())
	{
		Component->SetPhysicsLinearVelocity(Velocity);
	}
}

FVector AMantlePhysicsProp::GetAngularVelocityDegrees() const
{
	const UStaticMeshComponent* Component = GetStaticMeshComponent();
	if (Component && IsSimulating())
	{
		return Component->GetPhysicsAngularVelocityInDegrees();
	}
	return FVector::ZeroVector;
}

void AMantlePhysicsProp::SetAngularVelocityDegrees(const FVector& AngularVelocity)
{
	UStaticMeshComponent* Component = GetStaticMeshComponent();
	if (Component && IsSimulating())
	{
		// A resting prop sleeps: wake it so the spin starts now, as an impulse would.
		Component->WakeRigidBody();
		Component->SetPhysicsAngularVelocityInDegrees(AngularVelocity);
	}
}

double AMantlePhysicsProp::GetMassKg() const
{
	const UStaticMeshComponent* Component = GetStaticMeshComponent();
	if (!Component)
	{
		return 0.0;
	}
	if (const FBodyInstance* Body = Component->GetBodyInstance(); Body && Body->IsValidBodyInstance())
	{
		return Component->GetMass();
	}
	return Physics.Mass.IsSet() ? Physics.Mass.GetValue() : 0.0;
}

void AMantlePhysicsProp::SetPhysicsMaterial(EMantlePhysicsMaterial Material)
{
	Physics.Material = Material;
	PhysicsMaterial = static_cast<uint8>(Material);
	ApplyPhysicsMaterial(GetStaticMeshComponent(), Material);
}

EMantlePhysicsMaterial AMantlePhysicsProp::PhysicsMaterialFromByte(uint8 MaterialByte)
{
	return MaterialByte < MantlePhysicsMaterialCount ? static_cast<EMantlePhysicsMaterial>(MaterialByte) : EMantlePhysicsMaterial::Default;
}

void AMantlePhysicsProp::OnRep_PhysicsMaterial()
{
	Physics.Material = PhysicsMaterialFromByte(PhysicsMaterial);
	// Before this client adopts its placeholder the component has no mesh; the override is kept on
	// the body instance, and AdoptPlaceholder applies the settings again anyway.
	ApplyPhysicsMaterial(GetStaticMeshComponent(), Physics.Material);
}

void AMantlePhysicsProp::SetHitEventsEnabled(bool bEnabled)
{
	bHitEventsEnabled = bEnabled;
	if (UStaticMeshComponent* Component = GetStaticMeshComponent())
	{
		Component->SetNotifyRigidBodyCollision(bEnabled);
	}
}

void AMantlePhysicsProp::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME_CONDITION(AMantlePhysicsProp, EntityId, COND_InitialOnly);
	DOREPLIFETIME(AMantlePhysicsProp, PhysicsMaterial);
}

FMantlePhysicsPropPlaceResolver AMantlePhysicsProp::PlaceForWorld = nullptr;

void AMantlePhysicsProp::BeginPlay()
{
	Super::BeginPlay();
	TryAdopt();
}

void AMantlePhysicsProp::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (PlaceRetainedHandle.IsValid())
	{
		if (IMantlePhysicsPropPlace* Loaded = PlaceForWorld ? PlaceForWorld(GetWorld()) : nullptr)
		{
			Loaded->OnPlaceReady().Remove(PlaceRetainedHandle);
		}
		PlaceRetainedHandle.Reset();
	}
	Super::EndPlay(EndPlayReason);
}

void AMantlePhysicsProp::NotifyHit(UPrimitiveComponent* MyComp, AActor* Other, UPrimitiveComponent* OtherComp, bool bSelfMoved,
	FVector HitLocation, FVector HitNormal, FVector NormalImpulse, const FHitResult& Hit)
{
	Super::NotifyHit(MyComp, Other, OtherComp, bSelfMoved, HitLocation, HitNormal, NormalImpulse, Hit);
	if (bHitEventsEnabled && HasAuthority())
	{
		OnPhysicsHit.Broadcast(this, Other);
	}
}

void AMantlePhysicsProp::OnRep_EntityId()
{
	TryAdopt();
}

void AMantlePhysicsProp::OnPlaceRetained()
{
	if (IMantlePhysicsPropPlace* Loaded = PlaceForWorld ? PlaceForWorld(GetWorld()) : nullptr)
	{
		Loaded->OnPlaceReady().Remove(PlaceRetainedHandle);
	}
	PlaceRetainedHandle.Reset();
	TryAdopt();
}

void AMantlePhysicsProp::TryAdopt()
{
	if (bAdopted || HasAuthority() || EntityId.IsEmpty())
	{
		return;
	}
	IMantlePhysicsPropPlace* Loaded = PlaceForWorld ? PlaceForWorld(GetWorld()) : nullptr;
	if (!Loaded)
	{
		return;
	}
	if (!Loaded->HasPlace())
	{
		// This client has not built the place yet (it is still downloading): adopt once it has.
		if (!PlaceRetainedHandle.IsValid())
		{
			PlaceRetainedHandle = Loaded->OnPlaceReady().AddUObject(this, &AMantlePhysicsProp::OnPlaceRetained);
		}
		return;
	}

	const FMantleEntity* Entity = Loaded->GetPlace().Entities.FindByPredicate([this](const FMantleEntity& Candidate)
	{
		return Candidate.Id.Equals(EntityId, ESearchCase::CaseSensitive);
	});
	const FMantlePhysicsProps* PlacePhysics = Entity ? MantlePhysicsOf(*Entity) : nullptr;
	if (!PlacePhysics)
	{
		UE_LOG(LogMantle, Warning, TEXT("MANTLE_PHYSICS_ADOPT entity=%s: this client's place has no such physics entity"), *EntityId);
		return;
	}
	Physics = *PlacePhysics;
	// The server's preset, not the place's: a server script may have changed it before this client
	// joined. It arrives with the entity id, in the prop's initial state.
	Physics.Material = PhysicsMaterialFromByte(PhysicsMaterial);

	AActor* Placeholder = Loaded->FindActor(EntityId);
	if (Placeholder == this)
	{
		bAdopted = true;
		return;
	}
	if (!AdoptPlaceholder(Placeholder))
	{
		UE_LOG(LogMantle, Warning, TEXT("MANTLE_PHYSICS_ADOPT entity=%s: no placeholder mesh to take over; the prop stays invisible"), *EntityId);
		return;
	}
	Loaded->ReplaceEntityActor(EntityId, this);
	UE_LOG(LogMantle, Log, TEXT("MANTLE_PHYSICS_ADOPT entity=%s simulate=%d body=%s"),
		*EntityId, IsSimulating() ? 1 : 0, MantlePhysicsBodySourceToString(BodySource));
}
