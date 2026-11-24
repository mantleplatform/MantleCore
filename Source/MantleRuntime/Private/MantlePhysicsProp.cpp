// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePhysicsProp.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

#include "MantleCellArea.h"
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
	// looked for, and logged, once rather than for each prop of a place.
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
			TEXT("No tool in the repo authors these physical materials; add the asset under /Game/Mantle/Physics and cook it."),
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
		// the mesh's local bounds. At least a centimeter on each axis, so a flat mesh still has volume.
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
	// Always relevant to the engine's own filters (its distance grid knows no scripted camera): on a place with cells the script replicator puts each prop in an Iris
	// exclusion group of its area and opens the group for the players whose views are near (MantleScriptReplicatorProps.cpp), and a client whose copy the server stops
	// sending is left a placeholder (PreDestroyFromReplication). On any other place every player is sent every prop, as before; the per-place cap bounds how many there are.
	bReplicates = true;
	// Ticks only on a client's adopted copy, to ask the collision gate (BeginPlay turns it on there).
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;
	SetReplicatingMovement(true);
	bStaticMeshReplicateMovement = true;
	bAlwaysRelevant = true;
	SetNetUpdateFrequency(30.0f);
	// A client's copy follows the server's pose by predictive interpolation: the engine drives its body's velocities
	// toward the replicated pose (extrapolated over the send interval), instead of the default mode, which lets the
	// copy simulate freely and pulls it back at each update. A client has none of the server's joints, motors or
	// grab handles, so freely the copy drifts between updates and the default mode yanks it back thirty times a
	// second, a pulse every player saw (the joints play test, 2026-11-23). AStaticMeshActor::PostInitializeComponents
	// applies this (SetPhysicsReplicationMode); FBodyInstance::GetPhysicsReplicationMode reads it from the owner, and
	// FPhysicsReplication's PredictiveInterpolation path does the following (PhysicsReplication.cpp). Other players'
	// cars already replicate this way (AMantleVehicle::PhysicsReplicationModeFor).
	StaticMeshPhysicsReplicationMode = EPhysicsReplicationMode::PredictiveInterpolation;
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

FMantlePhysicsPropSettings FMantlePhysicsPropSettings::From(const FMantlePhysicsProps& InPhysics)
{
	FMantlePhysicsPropSettings Settings;
	Settings.MassKg = InPhysics.Mass.IsSet() ? static_cast<float>(InPhysics.Mass.GetValue()) : 0.0f;
	Settings.LinearDamping = static_cast<float>(InPhysics.LinearDamping);
	Settings.AngularDamping = static_cast<float>(InPhysics.AngularDamping);
	Settings.bGravity = InPhysics.bGravity;
	return Settings;
}

void FMantlePhysicsPropSettings::ApplyTo(FMantlePhysicsProps& InPhysics) const
{
	InPhysics.Mass = MassKg > 0.0f ? TOptional<double>(MassKg) : TOptional<double>();
	InPhysics.LinearDamping = LinearDamping;
	InPhysics.AngularDamping = AngularDamping;
	InPhysics.bGravity = bGravity;
}

void AMantlePhysicsProp::InitFromPlace(const FString& InEntityId, const FMantlePhysicsProps& InPhysics)
{
	EntityId = InEntityId;
	Physics = InPhysics;
	PhysicsMaterial = static_cast<uint8>(Physics.Material);
	ReplicatedSettings = FMantlePhysicsPropSettings::From(Physics);
	bWaitingForMesh = false;
	UMantlePhysicsMeshComponent* Component = GetPhysicsMeshComponent();
	if (!Component)
	{
		return;
	}
	BodySource = Component->PrepareSimulationBody(Component->GetStaticMesh());
	Component->RecreatePhysicsState();
	ApplyBodySettings(Component, Physics);
}

void AMantlePhysicsProp::InitWithoutMesh(const FString& InEntityId, const FMantlePhysicsProps& InPhysics)
{
	EntityId = InEntityId;
	Physics = InPhysics;
	PhysicsMaterial = static_cast<uint8>(Physics.Material);
	ReplicatedSettings = FMantlePhysicsPropSettings::From(Physics);
	bWaitingForMesh = true;
}

void AMantlePhysicsProp::InitAsCopyOf(const AMantlePhysicsProp& Source, const FString& InEntityId)
{
	UMantlePhysicsMeshComponent* To = GetPhysicsMeshComponent();
	const UStaticMeshComponent* From = Source.GetStaticMeshComponent();
	if (!To || !From)
	{
		return;
	}
	// Scale first: a mass computed from the collision is computed at the scale the body is made at.
	SetActorScale3D(Source.GetActorScale3D());
	To->SetStaticMesh(From->GetStaticMesh());
	for (int32 Slot = 0; Slot < From->GetNumOverrideMaterials(); ++Slot)
	{
		if (UMaterialInterface* Material = From->OverrideMaterials[Slot])
		{
			To->SetMaterial(Slot, Material);
		}
	}
	To->SetCastShadow(From->CastShadow);
	To->SetCullDistance(From->LDMaxDrawDistance);
	To->SetCollisionProfileName(UCollisionProfile::CustomCollisionProfileName);
	To->SetCollisionEnabled(From->GetCollisionEnabled());
	To->SetCollisionResponseToChannels(From->GetCollisionResponseToChannels());
	SetActorHiddenInGame(Source.IsHidden());
	bGrabbable = Source.bGrabbable;

	// As Source is now: simulating, or anchored (a Source still waiting for its ground or its mesh is going to simulate).
	FMantlePhysicsProps Copy = Source.Physics;
	Copy.bSimulate = Source.IsSimulating() || ((Source.bWaitingForGround || Source.bWaitingForMesh) && Source.Physics.bSimulate);
	if (From->GetStaticMesh())
	{
		InitFromPlace(InEntityId, Copy);
	}
	else
	{
		InitWithoutMesh(InEntityId, Copy);
	}
}

void AMantlePhysicsProp::MeshArrived()
{
	UMantlePhysicsMeshComponent* Component = GetPhysicsMeshComponent();
	if (!Component || !Component->GetStaticMesh())
	{
		return;
	}
	BodySource = Component->PrepareSimulationBody(Component->GetStaticMesh());
	Component->RecreatePhysicsState();
	ApplyBodySettings(Component, Physics);
	if (bWaitingForMesh)
	{
		bWaitingForMesh = false;
		if (bBeginWhenMeshed)
		{
			bBeginWhenMeshed = false;
			BeginSimulation(bWaitForGroundWhenMeshed);
		}
	}
	else if (bAdopted && !HasAuthority() && !CollisionGate.IsClosed())
	{
		TakeServerPhysicsState(*Component);
	}
}

void AMantlePhysicsProp::BeginSimulation(const bool bWaitForGround)
{
	if (GetAttachParentActor())
	{
		DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
	}
	if (bWaitingForMesh)
	{
		// No shape to simulate yet: held, with no body, until the mesh is here (MeshArrived starts it then).
		bBeginWhenMeshed = true;
		bWaitForGroundWhenMeshed = bWaitForGround;
		UE_LOG(LogMantle, Log, TEXT("MANTLE_PHYSICS_PROP entity=%s simulate=0 waiting_for_mesh=1"), *EntityId);
		return;
	}
	UStaticMeshComponent* Component = GetStaticMeshComponent();
	if (Component && Physics.bSimulate && bWaitForGround)
	{
		// Held where it stands; the tick starts it once its ground does.
		bWaitingForGround = true;
		SetActorTickEnabled(true);
	}
	else if (Component && Physics.bSimulate)
	{
		Component->SetSimulatePhysics(true);
	}
	UE_LOG(LogMantle, Log, TEXT("MANTLE_PHYSICS_PROP entity=%s simulate=%d body=%s mass_kg=%.2f%s"),
		*EntityId, IsSimulating() ? 1 : 0, MantlePhysicsBodySourceToString(BodySource), GetMassKg(), bWaitingForGround ? TEXT(" waiting_for_ground=1") : TEXT(""));
}

void AMantlePhysicsProp::UpdateGroundWait()
{
	UStaticMeshComponent* Component = GetStaticMeshComponent();
	if (!bWaitingForGround || !Component)
	{
		return;
	}
	FMantleCellAreaQuery Query;
	Query.Point = GetActorLocation();
	Query.RadiusCm = FMantleCollisionGate::RadiusCm(Component->Bounds.SphereRadius, 0.0);
	Query.bSolidOnly = true;
	FMantleCellAreaState State;
	uint64 Serial = 0;
	bool bReady = false;
	if (FMantleCellArea::Ask(GetWorld(), Query, State, Serial))
	{
		bReady = State.AllStand();
	}
	else
	{
		// Nothing streams here (yet): ready once the place is complete (then nothing ever will), not before (the streamer starts when the root is built).
		const IMantlePhysicsPropPlace* Loaded = PlaceForWorld ? PlaceForWorld(GetWorld()) : nullptr;
		bReady = Loaded && Loaded->HasPlace();
	}
	if (!bReady)
	{
		return;
	}
	bWaitingForGround = false;
	if (GetNetMode() != NM_Client)
	{
		SetActorTickEnabled(false);
	}
	Component->SetSimulatePhysics(true);
	UE_LOG(LogMantle, Log, TEXT("MANTLE_PHYSICS_PROP entity=%s simulate=1 its solid cells stand: started at (%.0f, %.0f, %.0f)"), *EntityId, GetActorLocation().X, GetActorLocation().Y, GetActorLocation().Z);
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
	// A stand-in whose uploaded mesh is still downloading here has none: no body until it does (MeshArrived), rather than a box
	// of a guessed size that players would bump into.
	if (Mesh)
	{
		BodySource = To->PrepareSimulationBody(Mesh);
	}
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
	// The place's drawDistance, as the placeholder was built with it (0, never culled, when it has none).
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
	TakeServerPhysicsState(*To);
	bAdopted = true;
	return true;
}

void AMantlePhysicsProp::TakeServerPhysicsState(UStaticMeshComponent& Component)
{
	Component.SetSimulatePhysics(GetReplicatedMovement().bRepPhysics);
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
			Component.PutRigidBodyToSleep();
		}
	}
}

AStaticMeshActor* AMantlePhysicsProp::SpawnPlainCopy(UWorld& World, const UStaticMeshComponent& From, const FTransform& Transform)
{
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AStaticMeshActor* Copy = World.SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), Transform, Params);
	UStaticMeshComponent* To = Copy ? Copy->GetStaticMeshComponent() : nullptr;
	if (!To)
	{
		return nullptr;
	}
	To->SetMobility(EComponentMobility::Movable);
	To->SetStaticMesh(From.GetStaticMesh());
	for (int32 Slot = 0; Slot < From.GetNumOverrideMaterials(); ++Slot)
	{
		if (UMaterialInterface* Material = From.OverrideMaterials[Slot])
		{
			To->SetMaterial(Slot, Material);
		}
	}
	To->SetCastShadow(From.CastShadow);
	To->SetCullDistance(From.LDMaxDrawDistance);
	To->SetCollisionProfileName(UCollisionProfile::CustomCollisionProfileName);
	To->SetCollisionEnabled(From.GetCollisionEnabled());
	To->SetCollisionResponseToChannels(From.GetCollisionResponseToChannels());
	Copy->SetActorScale3D(Transform.GetScale3D());
	return Copy;
}

AStaticMeshActor* AMantlePhysicsProp::RestorePlaceholder()
{
	UWorld* World = GetWorld();
	UStaticMeshComponent* From = GetStaticMeshComponent();
	IMantlePhysicsPropPlace* Loaded = PlaceForWorld ? PlaceForWorld(World) : nullptr;
	if (!bAdopted || HasAuthority() || EntityId.IsEmpty() || !World || World->bIsTearingDown || !From || !Loaded || !Loaded->HasPlace())
	{
		return nullptr;
	}
	if (Loaded->FindActor(EntityId) != this)
	{
		return nullptr; // the entity's actor is already something else (a clone's, or a placeholder made already): nothing to stand in for
	}

	AStaticMeshActor* Placeholder = SpawnPlainCopy(*World, *From, GetActorTransform());
	if (!Placeholder)
	{
		return nullptr;
	}
#if WITH_EDITOR
	Placeholder->SetActorLabel(GetActorLabel());
#endif
	Placeholder->SetActorHiddenInGame(IsHidden());

	// What was last seen of this prop, for a script that asks about it while it is far.
	UMantleFarPropFacts* Facts = NewObject<UMantleFarPropFacts>(Placeholder);
	Facts->MassKg = GetMassKg();
	Facts->bAnchored = !GetReplicatedMovement().bRepPhysics;
	Facts->bGrabbable = bGrabbable;
	Facts->Material = Physics.Material;
	Placeholder->AddInstanceComponent(Facts);
	Facts->RegisterComponent();

	TArray<AActor*> AttachedChildren;
	GetAttachedActors(AttachedChildren, /*bResetArray*/ true, /*bRecursivelyIncludeAttachedActors*/ false);
	for (AActor* Child : AttachedChildren)
	{
		if (IsValid(Child))
		{
			Child->AttachToActor(Placeholder, FAttachmentTransformRules::KeepRelativeTransform);
		}
	}
	Loaded->ReplaceEntityActor(EntityId, Placeholder);
	UE_LOG(LogMantle, Log, TEXT("MANTLE_PHYSICS_PROP_RELEVANCE leave entity=%s at (%.0f, %.0f, %.0f)"), *EntityId, GetActorLocation().X, GetActorLocation().Y, GetActorLocation().Z);
	return Placeholder;
}

void AMantlePhysicsProp::PreDestroyFromReplication()
{
	// The engine runs this for every replicated destroy of this copy: the server stopped sending it (its players' views are far from it), it destroyed the prop, or the connection
	// went. Whichever it is, the entity keeps an actor here until the script world says otherwise: a destroyed entity's placeholder is destroyed with it (OnEntityActorReplaced,
	// DestroyEntity), and a world coming down is left alone.
	Super::PreDestroyFromReplication();
	RestorePlaceholder();
}

void AMantlePhysicsProp::ForEachServerProp(const UWorld* World, TFunctionRef<void(AMantlePhysicsProp&)> Visit)
{
	// By index, no copy (this runs every frame over up to thousands of props); a visitor must not end a prop.
	const TArray<AMantlePhysicsProp*>& Props = ServerProps();
	for (int32 Index = 0; Index < Props.Num(); ++Index)
	{
		AMantlePhysicsProp* Prop = Props[Index];
		if (IsValid(Prop) && Prop->GetWorld() == World)
		{
			Visit(*Prop);
		}
	}
}

bool AMantlePhysicsProp::IsBodyAwake() const
{
	const UStaticMeshComponent* Component = GetStaticMeshComponent();
	return Component && Component->IsSimulatingPhysics() && Component->RigidBodyIsAwake();
}

TArray<AMantlePhysicsProp*>& AMantlePhysicsProp::ServerProps()
{
	static TArray<AMantlePhysicsProp*> Props;
	return Props;
}

TArray<AMantlePhysicsProp*>& AMantlePhysicsProp::WaitingProps()
{
	static TArray<AMantlePhysicsProp*> Props;
	return Props;
}

void AMantlePhysicsProp::AdoptWaiting(UWorld* World, const FString& InEntityId)
{
	// Copied out first: adopting takes a copy off the list.
	TArray<AMantlePhysicsProp*, TInlineAllocator<2>> Matches;
	for (AMantlePhysicsProp* Prop : WaitingProps())
	{
		if (IsValid(Prop) && Prop->GetWorld() == World && Prop->EntityId.Equals(InEntityId, ESearchCase::CaseSensitive))
		{
			Matches.Add(Prop);
		}
	}
	for (AMantlePhysicsProp* Prop : Matches)
	{
		Prop->TryAdopt();
	}
}

void AMantlePhysicsProp::ReturnToWaiting()
{
	if (HasAuthority() || !bAdopted)
	{
		return;
	}
	bAdopted = false;
	WaitingProps().AddUnique(this);
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
	// Sent with the entity id, in the same first state, so a copy has its settings when it adopts (OnRep_EntityId runs once the whole state is in).
	DOREPLIFETIME_CONDITION(AMantlePhysicsProp, ReplicatedSettings, COND_InitialOnly);
}

FMantlePhysicsPropPlaceResolver AMantlePhysicsProp::PlaceForWorld = nullptr;
FMantlePhysicsPropFellDelegate AMantlePhysicsProp::OnAnyFellOutOfWorld;

void AMantlePhysicsProp::FellOutOfWorld(const UDamageType& DamageType)
{
	// The kill floor destroys the actor on the server (and only there). The entity goes with it: told first, so clients are sent the destroy and take their stand-in down.
	if (HasAuthority())
	{
		OnAnyFellOutOfWorld.Broadcast(this);
	}
	Super::FellOutOfWorld(DamageType);
}

void AMantlePhysicsProp::BeginPlay()
{
	Super::BeginPlay();
	if (GetNetMode() == NM_Client)
	{
		SetActorTickEnabled(true);
	}
	else if (HasAuthority())
	{
		ServerProps().AddUnique(this);
	}
	TryAdopt();
}

void AMantlePhysicsProp::Tick(const float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	UpdateGroundWait();
	UpdateCollisionGate();
}

void AMantlePhysicsProp::FollowServerPose()
{
	const FRepMovement& Replicated = GetReplicatedMovement();
	SetActorLocationAndRotation(Replicated.Location, Replicated.Rotation, false, nullptr, ETeleportType::TeleportPhysics);
}

void AMantlePhysicsProp::UpdateCollisionGate()
{
	UStaticMeshComponent* Component = GetStaticMeshComponent();
	if (!bAdopted || HasAuthority() || !Component)
	{
		return;
	}
	if (!CollisionGate.Update(GetWorld(), GetActorLocation(), Component->Bounds.SphereRadius, GetReplicatedMovement().LinearVelocity.Size(), TEXT("prop"), EntityId))
	{
		return;
	}
	if (CollisionGate.IsClosed())
	{
		// No ground here: not simulated, and where the server says it is (an asleep prop sends nothing more, so the last pose is the truth).
		Component->SetSimulatePhysics(false);
		FollowServerPose();
		return;
	}
	// Ground again: from the server's pose, in the server's simulate and sleep state (as adopting the placeholder did).
	FollowServerPose();
	TakeServerPhysicsState(*Component);
}

void AMantlePhysicsProp::OnRep_ReplicatedMovement()
{
	Super::OnRep_ReplicatedMovement();
	if (CollisionGate.IsClosed())
	{
		// The engine has just put the body back to the server's simulate state: it stays off while its ground is not built here.
		if (UStaticMeshComponent* Component = GetStaticMeshComponent())
		{
			if (Component->IsSimulatingPhysics())
			{
				Component->SetSimulatePhysics(false);
			}
		}
		FollowServerPose();
	}
}

void AMantlePhysicsProp::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ServerProps().RemoveSingleSwap(this);
	WaitingProps().RemoveSingleSwap(this);
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
	TRACE_CPUPROFILER_EVENT_SCOPE_STR("Retained_PhysicsProp");
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

	// The settings came with this copy, so a place entity and a script's alike adopt without the place file. The server's preset,
	// not the place's: a server script may have changed it before this client joined (it arrives with the entity id).
	ReplicatedSettings.ApplyTo(Physics);
	Physics.Material = PhysicsMaterialFromByte(PhysicsMaterial);

	AActor* Placeholder = Loaded->FindActor(EntityId);
	if (Placeholder == this)
	{
		WaitingProps().RemoveSingleSwap(this);
		bAdopted = true;
		return;
	}
	if (!Placeholder)
	{
		// A script-made entity whose stand-in is not built here yet (its spawn item comes on the script replicator's own channel, and
		// may come after this copy): taken over when it is (AdoptWaiting).
		if (!WaitingProps().Contains(this))
		{
			WaitingProps().Add(this);
			UE_LOG(LogMantle, Log, TEXT("MANTLE_PHYSICS_ADOPT entity=%s waiting for its stand-in"), *EntityId);
		}
		return;
	}
	WaitingProps().RemoveSingleSwap(this);
	if (!AdoptPlaceholder(Placeholder))
	{
		UE_LOG(LogMantle, Warning, TEXT("MANTLE_PHYSICS_ADOPT entity=%s: no placeholder mesh to take over; the prop stays invisible"), *EntityId);
		return;
	}
	Loaded->ReplaceEntityActor(EntityId, this);
	UE_LOG(LogMantle, Log, TEXT("MANTLE_PHYSICS_ADOPT entity=%s simulate=%d body=%s"),
		*EntityId, IsSimulating() ? 1 : 0, MantlePhysicsBodySourceToString(BodySource));
}
