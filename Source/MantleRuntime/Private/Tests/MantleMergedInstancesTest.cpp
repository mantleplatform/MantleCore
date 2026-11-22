// Copyright Joshua Eger. All Rights Reserved.
#include "MantleMergedInstancesComponent.h"
#include "MantlePlaceBuilder.h"
#include "MantlePlaceParser.h"
#include "MantleRuntimeStaticMeshBuilder.h"
#include "MantleStaticMeshReader.h"

#include "CollisionQueryParams.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/DemoNetDriver.h"
#include "Engine/Engine.h"
#include "Engine/HitResult.h"
#include "Engine/OverlapResult.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "Chaos/ImplicitObject.h"
#include "Chaos/ImplicitObjectType.h"
#include "Chaos/ShapeInstance.h"
#include "Physics/PhysicsInterfaceCore.h"
#include "PhysicsProxy/SingleParticlePhysicsProxy.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/BodySetup.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS

// A dedicated server holds a static, solid InstancedMesh as a few bodies with many shapes each (UMantleMergedInstancesComponent) instead of a body for every instance.
// The same instances are made twice in one world, far apart: once as the engine's instanced component (what a server held before) and once as the merged component. Every
// trace, sweep and overlap fired at one copy is fired at the other the same way, and the answers must agree.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleMergedInstancesTestPrivate
{
	/** How far the merged copy stands from the engine's (cm): the world's queries then never see one copy from the other. */
	const FVector CopyOffset(0.0, 100000.0, 0.0);

	/** How many instances a copy has: more than two runs' worth for a mesh of one shape (128 to a body). */
	constexpr int32 InstanceCount = 300;

	constexpr double DistanceToleranceCm = 0.01;
	constexpr double NormalTolerance = 0.001;
	constexpr double PointToleranceCm = 0.05;

	struct FMergedTestWorld
	{
		UWorld* World = nullptr;
		FMergedTestWorld() { World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, FName(*FString::Printf(TEXT("MantleMergedInstancesTestWorld_%s"), *FGuid::NewGuid().ToString()))); }
		~FMergedTestWorld()
		{
			if (World)
			{
				World->DestroyWorld(/*bInformEngineOfWorld*/ false);
				if (World->IsRooted())
				{
					World->RemoveFromRoot();
				}
			}
		}
	};

	/** A 100 cm box as a one-section MRM2 mesh, with the given simple collision (empty: the triangles are the collision, complex-as-simple). */
	FMantleStaticMeshData MakeMergedTestBox(const FMantleStaticMeshCollision& Collision)
	{
		FMantleStaticMeshData Mesh;
		for (int32 Corner = 0; Corner < 8; ++Corner)
		{
			const float X = (Corner & 1) ? 50.0f : -50.0f;
			const float Y = (Corner & 2) ? 50.0f : -50.0f;
			const float Z = (Corner & 4) ? 50.0f : -50.0f;
			Mesh.Positions.Add(FVector3f(X, Y, Z));
			Mesh.Normals.Add(FVector3f(0.0f, 0.0f, 1.0f));
			Mesh.Tangents.Add(FVector4f(1.0f, 0.0f, 0.0f, 1.0f));
			Mesh.UVs.Add(FVector2f((X + 50.0f) / 100.0f, (Y + 50.0f) / 100.0f));
		}
		const uint32 Triangles[12][3] = {
			{ 0, 2, 1 }, { 1, 2, 3 }, { 4, 5, 6 }, { 5, 7, 6 },
			{ 0, 1, 4 }, { 1, 5, 4 }, { 2, 6, 3 }, { 3, 6, 7 },
			{ 0, 4, 2 }, { 2, 4, 6 }, { 1, 3, 5 }, { 3, 7, 5 } };
		for (const uint32 (&Triangle)[3] : Triangles)
		{
			Mesh.Indices.Add(Triangle[0]);
			Mesh.Indices.Add(Triangle[1]);
			Mesh.Indices.Add(Triangle[2]);
		}
		Mesh.Bounds = FBox3f(FVector3f(-50.0f), FVector3f(50.0f));
		FMantleSkinnedMeshSection Section;
		Section.MaterialIndex = 0;
		Section.FirstIndex = 0;
		Section.IndexCount = 36;
		Section.MaterialName = TEXT("A");
		Mesh.Sections = { Section };
		Mesh.Collision = Collision;
		return Mesh;
	}

	/** A turned box, a sphere, a turned capsule and a wedge-shaped hull, with the triangles kept for complex traces: five shapes for every instance. */
	FMantleStaticMeshCollision MixedTestCollision()
	{
		FMantleStaticMeshCollision Collision;
		Collision.bSimpleAsComplex = false;
		FMantleStaticMeshBox Box;
		Box.Center = FVector3f(10.0f, 5.0f, 0.0f);
		Box.Rotation = FRotator3f(0.0f, 30.0f, 0.0f).Quaternion();
		Box.Size = FVector3f(60.0f, 40.0f, 30.0f);
		Collision.Boxes.Add(Box);
		FMantleStaticMeshSphere Sphere;
		Sphere.Center = FVector3f(0.0f, 0.0f, 40.0f);
		Sphere.Radius = 20.0f;
		Collision.Spheres.Add(Sphere);
		FMantleStaticMeshCapsule Capsule;
		Capsule.Center = FVector3f(-20.0f, 0.0f, 0.0f);
		Capsule.Rotation = FRotator3f(90.0f, 0.0f, 0.0f).Quaternion();
		Capsule.Radius = 12.0f;
		Capsule.Length = 30.0f;
		Collision.Capsules.Add(Capsule);
		FMantleStaticMeshHull Hull;
		Hull.Vertices = {
			FVector3f(-30.0f, -30.0f, -50.0f), FVector3f(30.0f, -30.0f, -50.0f), FVector3f(30.0f, 30.0f, -50.0f),
			FVector3f(-30.0f, 30.0f, -50.0f), FVector3f(0.0f, -30.0f, -20.0f), FVector3f(0.0f, 30.0f, -20.0f) };
		Collision.Hulls.Add(Hull);
		return Collision;
	}

	/** The meshes the copies are made from and the surface they are given, held for a test's whole run. */
	struct FMergedTestAssets
	{
		/** The engine's own cube: simple collision. */
		UStaticMesh* Cube = nullptr;
		/** Complex-as-simple: only the triangles collide. */
		TStrongObjectPtr<UStaticMesh> Triangles;
		/** Simple shapes and triangles. */
		TStrongObjectPtr<UStaticMesh> Mixed;
		TStrongObjectPtr<UPhysicalMaterial> Surface;

		bool Make(FAutomationTestBase& Test)
		{
			Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
			FMantleStaticMeshBuildParams Params;
			Params.bComplexCollision = true;
			Params.bCookCollisionNow = true;
			// Render resources as the engine allows them (a process that cannot render makes none): the engine's instanced copy is drawn where this runs with a GPU.
			Params.bInitRenderResources = true;
			FString Error;
			Triangles.Reset(FMantleRuntimeStaticMeshBuilder::Build(MakeMergedTestBox(FMantleStaticMeshCollision()), Params, &Error));
			if (!Test.TestNotNull(TEXT("the triangle-collision mesh builds"), Triangles.Get()))
			{
				Test.AddError(Error);
				return false;
			}
			Mixed.Reset(FMantleRuntimeStaticMeshBuilder::Build(MakeMergedTestBox(MixedTestCollision()), Params, &Error));
			if (!Test.TestNotNull(TEXT("the mixed-collision mesh builds"), Mixed.Get()))
			{
				Test.AddError(Error);
				return false;
			}
			Surface.Reset(NewObject<UPhysicalMaterial>(GetTransientPackage()));
			Surface->Friction = 0.37f;
			return Test.TestNotNull(TEXT("the engine's cube"), Cube) && Test.TestNotNull(TEXT("a surface"), Surface.Get());
		}
	};

	/**
	 * Instances in the component's space, of every kind a place has: identity, uniform scale, non-uniform scale with a turn, mirrored ones (one negative axis, turned), a tall one, one
	 * turned far from the others, and two instances far (more than 10 m) from the first, so the first body of a run holds a shape far from its first instance.
	 */
	TArray<FTransform> MakeMergedTestInstances(const int32 Count)
	{
		TArray<FTransform> Out;
		Out.Add(FTransform::Identity);
		Out.Add(FTransform(FRotator(5.0, 40.0, 0.0), FVector(15000.0, 9000.0, 250.0), FVector(1.2)));
		Out.Add(FTransform(FRotator(0.0, -70.0, 10.0), FVector(-12000.0, 16000.0, -300.0), FVector(1.0, 2.0, 0.8)));
		for (int32 Index = 3; Index < Count; ++Index)
		{
			const FVector Location((Index % 20) * 300.0 + 600.0, (Index / 20) * 300.0 + 600.0, (Index % 5) * 15.0);
			FRotator Rotation = FRotator::ZeroRotator;
			FVector Scale = FVector::OneVector;
			switch (Index % 8)
			{
			case 1: Scale = FVector(1.4); break;
			case 2: Rotation = FRotator(10.0, 35.0, 0.0); Scale = FVector(2.0, 0.6, 1.3); break;
			case 3: Rotation = FRotator(0.0, 0.0, 25.0); Scale = FVector(-1.0, 1.0, 1.0); break;
			case 4: Rotation = FRotator(0.0, -50.0, 0.0); Scale = FVector(1.0, -1.5, 1.0); break;
			case 5: Rotation = FRotator(45.0, 90.0, 0.0); break;
			case 6: Rotation = FRotator(0.0, 0.0, 70.0); Scale = FVector(0.5, 0.5, 3.0); break;
			default: break;
			}
			Out.Add(FTransform(Rotation, Location + FVector(0.0, 0.0, Index % 8 == 7 ? 100.0 : 0.0), Scale));
		}
		return Out;
	}

	/** The component of both copies of one set of instances: turned, and scaled (not uniformly). */
	FTransform MergedTestComponentTransform()
	{
		return FTransform(FRotator(0.0, 20.0, 0.0), FVector(500.0, -300.0, 100.0), FVector(1.3, 0.9, 1.1));
	}

	/** What a place's builder does to an instanced mesh's component before it registers (ApplyMeshProps), for the parts that decide collision. */
	void ApplyMergedTestCollision(UStaticMeshComponent* Component, UStaticMesh* Mesh, UPhysicalMaterial* Surface, const bool bInvisibleWall)
	{
		Component->SetStaticMesh(Mesh);
		Component->SetCollisionProfileName(UCollisionProfile::CustomCollisionProfileName);
		Component->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Component->SetCollisionResponseToAllChannels(ECR_Block);
		if (bInvisibleWall)
		{
			Component->SetCollisionResponseToChannel(ECC_Visibility, ECR_Ignore);
		}
		Component->SetPhysMaterialOverride(Surface);
	}

	/** One copy of the instances in the world: its actor, its component (the engine's or the merged one), where the component stands. */
	struct FMergedCopy
	{
		AActor* Actor = nullptr;
		UStaticMeshComponent* Component = nullptr;
		UInstancedStaticMeshComponent* Engine = nullptr;
		UMantleMergedInstancesComponent* Merged = nullptr;
		FTransform Transform;
	};

	/** What a server held before: the engine's instanced component, Static, with its bodies made in one batch at registration. */
	FMergedCopy MakeEngineCopy(UWorld* World, UStaticMesh* Mesh, UPhysicalMaterial* Surface, const FTransform& ComponentTransform, const TArray<FTransform>& Instances, const bool bInvisibleWall)
	{
		FMergedCopy Copy;
		Copy.Transform = ComponentTransform;
		Copy.Actor = World->SpawnActor<AActor>();
		Copy.Engine = NewObject<UInstancedStaticMeshComponent>(Copy.Actor, TEXT("Instances"));
		Copy.Actor->SetRootComponent(Copy.Engine);
		Copy.Actor->AddInstanceComponent(Copy.Engine);
		Copy.Engine->SetMobility(EComponentMobility::Static);
		Copy.Engine->SetRelativeTransform(ComponentTransform);
		ApplyMergedTestCollision(Copy.Engine, Mesh, Surface, bInvisibleWall);
		Copy.Engine->PreAllocateInstancesMemory(Instances.Num());
		Copy.Engine->AddInstances(Instances, /*bShouldReturnIndices*/ false, /*bWorldSpace*/ false);
		Copy.Engine->RegisterComponent();
		Copy.Component = Copy.Engine;
		return Copy;
	}

	FMergedCopy MakeMergedCopy(UWorld* World, UStaticMesh* Mesh, UPhysicalMaterial* Surface, const FTransform& ComponentTransform, const TArray<FTransform>& Instances, const bool bInvisibleWall)
	{
		FMergedCopy Copy;
		Copy.Transform = ComponentTransform;
		Copy.Actor = World->SpawnActor<AActor>();
		Copy.Merged = NewObject<UMantleMergedInstancesComponent>(Copy.Actor, TEXT("Instances"));
		Copy.Actor->SetRootComponent(Copy.Merged);
		Copy.Actor->AddInstanceComponent(Copy.Merged);
		Copy.Merged->SetMobility(EComponentMobility::Static);
		Copy.Merged->SetRelativeTransform(ComponentTransform);
		ApplyMergedTestCollision(Copy.Merged, Mesh, Surface, bInvisibleWall);
		Copy.Merged->SetInstances(Instances);
		Copy.Merged->RegisterComponent();
		Copy.Component = Copy.Merged;
		return Copy;
	}

	/** One pair of copies of the same instances, 1 km apart. */
	struct FMergedPair
	{
		FMergedCopy Engine;
		FMergedCopy Merged;
	};

	FMergedPair MakePair(UWorld* World, UStaticMesh* Mesh, UPhysicalMaterial* Surface, const TArray<FTransform>& Instances, const bool bInvisibleWall)
	{
		FMergedPair Pair;
		FTransform Far = MergedTestComponentTransform();
		Far.AddToTranslation(CopyOffset);
		Pair.Engine = MakeEngineCopy(World, Mesh, Surface, MergedTestComponentTransform(), Instances, bInvisibleWall);
		Pair.Merged = MakeMergedCopy(World, Mesh, Surface, Far, Instances, bInvisibleWall);
		return Pair;
	}

	// ---- The probes ---------------------------------------------------------------------------------------

	enum EMergedProbeKind : int32
	{
		ProbeLineDownSimple,
		ProbeLineDownComplex,
		ProbeLineAlongX,
		ProbeSphereDownSimple,
		ProbeSphereDownComplex,
		ProbeBoxDown,
		ProbeCapsuleAlongY,
		ProbeOverlapSphere,
		ProbeOverlapBox,
		ProbeComponentLine,
		ProbeComponentSweep,
		ProbeKindCount
	};

	const ECollisionChannel ProbeChannels[] = { ECC_Visibility, ECC_WorldStatic, ECC_Pawn, ECC_Camera };

	/** Every kind runs on the visibility channel; a few also on the others (a place's responses are one setting for all channels, so these check the channel is read, not the geometry). */
	bool RunsOnEveryChannel(const int32 Kind)
	{
		return Kind == ProbeLineDownSimple || Kind == ProbeSphereDownSimple || Kind == ProbeOverlapSphere;
	}

	struct FMergedProbe
	{
		int32 Kind = 0;
		ECollisionChannel Channel = ECC_Visibility;
		FVector Point = FVector::ZeroVector;
		bool bHit = false;
		/** An overlap result that is not the copy's own actor. */
		bool bForeign = false;
		FHitResult Hit;
	};

	/** Component-space places to probe: around every instance (on it, and on each side of it). */
	TArray<FVector> MakeMergedProbePoints(const TArray<FTransform>& Instances)
	{
		const FVector2D Offsets[] = { FVector2D(0.0, 0.0), FVector2D(60.0, 40.0), FVector2D(-60.0, -40.0) };
		TArray<FVector> Points;
		for (const FTransform& Instance : Instances)
		{
			const FVector Center = Instance.GetLocation();
			for (const FVector2D& Offset : Offsets)
			{
				Points.Add(FVector(Center.X + Offset.X, Center.Y + Offset.Y, 0.0));
			}
		}
		return Points;
	}

	void RunMergedProbe(UWorld* World, const FMergedCopy& Copy, const int32 Kind, const ECollisionChannel Channel, const FVector& Point, FMergedProbe& Out)
	{
		const auto At = [&Copy](const double X, const double Y, const double Z) { return Copy.Transform.TransformPosition(FVector(X, Y, Z)); };
		const FQuat CopyRotation = Copy.Transform.GetRotation();
		FCollisionQueryParams Simple(SCENE_QUERY_STAT(MantleMergedInstancesTest), /*bTraceComplex*/ false);
		Simple.bReturnPhysicalMaterial = true;
		FCollisionQueryParams Complex(SCENE_QUERY_STAT(MantleMergedInstancesTest), /*bTraceComplex*/ true);
		Complex.bReturnPhysicalMaterial = true;

		Out.Kind = Kind;
		Out.Channel = Channel;
		Out.Point = Point;
		const FVector DownFrom = At(Point.X, Point.Y, 350.0);
		const FVector DownTo = At(Point.X, Point.Y, -350.0);
		switch (Kind)
		{
		case ProbeLineDownSimple:
			Out.bHit = World->LineTraceSingleByChannel(Out.Hit, DownFrom, DownTo, Channel, Simple);
			break;
		case ProbeLineDownComplex:
			Out.bHit = World->LineTraceSingleByChannel(Out.Hit, DownFrom, DownTo, Channel, Complex);
			break;
		case ProbeLineAlongX:
			Out.bHit = World->LineTraceSingleByChannel(Out.Hit, At(Point.X - 350.0, Point.Y, 5.0), At(Point.X + 350.0, Point.Y, 5.0), Channel, Simple);
			break;
		case ProbeSphereDownSimple:
			Out.bHit = World->SweepSingleByChannel(Out.Hit, DownFrom, DownTo, FQuat::Identity, Channel, FCollisionShape::MakeSphere(30.0f), Simple);
			break;
		case ProbeSphereDownComplex:
			Out.bHit = World->SweepSingleByChannel(Out.Hit, DownFrom, DownTo, FQuat::Identity, Channel, FCollisionShape::MakeSphere(30.0f), Complex);
			break;
		case ProbeBoxDown:
			Out.bHit = World->SweepSingleByChannel(Out.Hit, DownFrom, DownTo, CopyRotation * FRotator(0.0, 20.0, 0.0).Quaternion(), Channel, FCollisionShape::MakeBox(FVector(25.0, 15.0, 10.0)), Simple);
			break;
		case ProbeCapsuleAlongY:
			Out.bHit = World->SweepSingleByChannel(Out.Hit, At(Point.X, Point.Y - 350.0, 0.0), At(Point.X, Point.Y + 350.0, 0.0), CopyRotation, Channel, FCollisionShape::MakeCapsule(15.0f, 30.0f), Simple);
			break;
		case ProbeOverlapSphere:
		{
			TArray<FOverlapResult> Overlaps;
			World->OverlapMultiByChannel(Overlaps, At(Point.X, Point.Y, 0.0), FQuat::Identity, Channel, FCollisionShape::MakeSphere(40.0f), Simple);
			Out.bHit = !Overlaps.IsEmpty();
			for (const FOverlapResult& Overlap : Overlaps)
			{
				Out.bForeign = Out.bForeign || Overlap.GetActor() != Copy.Actor;
			}
			break;
		}
		case ProbeOverlapBox:
			Out.bHit = World->OverlapAnyTestByChannel(At(Point.X, Point.Y, 50.0), CopyRotation, Channel, FCollisionShape::MakeBox(FVector(30.0, 20.0, 25.0)), Simple);
			break;
		case ProbeComponentLine:
			Out.bHit = Copy.Component->LineTraceComponent(Out.Hit, DownFrom, DownTo, Simple);
			break;
		case ProbeComponentSweep:
			Out.bHit = Copy.Component->SweepComponent(Out.Hit, DownFrom, DownTo, FQuat::Identity, FCollisionShape::MakeSphere(30.0f), false);
			break;
		default:
			break;
		}
	}

	/** Every probe against one copy, in an order that does not depend on the copy. */
	TArray<FMergedProbe> RunAllMergedProbes(UWorld* World, const FMergedCopy& Copy, const TArray<FVector>& Points)
	{
		TArray<FMergedProbe> Results;
		for (const FVector& Point : Points)
		{
			for (int32 Kind = 0; Kind < ProbeKindCount; ++Kind)
			{
				const bool bChannels = RunsOnEveryChannel(Kind);
				const bool bPerComponent = Kind == ProbeComponentLine || Kind == ProbeComponentSweep;
				for (int32 ChannelIndex = 0; ChannelIndex < (bChannels ? static_cast<int32>(UE_ARRAY_COUNT(ProbeChannels)) : 1); ++ChannelIndex)
				{
					FMergedProbe& Probe = Results.AddDefaulted_GetRef();
					RunMergedProbe(World, Copy, Kind, bPerComponent ? ECC_Visibility : ProbeChannels[ChannelIndex], Point, Probe);
				}
			}
		}
		return Results;
	}

	bool IsMergedTraceKind(const int32 Kind)
	{
		return Kind != ProbeOverlapSphere && Kind != ProbeOverlapBox;
	}

	/** What one probe's pair of answers disagree about; empty when they agree. */
	FString MergedProbeDisagreement(const FMergedProbe& Engine, const FMergedProbe& Merged, const FMergedPair& Pair)
	{
		if (Engine.bHit != Merged.bHit)
		{
			return FString::Printf(TEXT("hit %d against %d"), Engine.bHit, Merged.bHit);
		}
		if (Engine.bForeign || Merged.bForeign)
		{
			return TEXT("an overlap result that is not the copy's own actor");
		}
		if (!Engine.bHit || !IsMergedTraceKind(Engine.Kind))
		{
			return FString();
		}
		const FHitResult& A = Engine.Hit;
		const FHitResult& B = Merged.Hit;
		const bool bPerComponent = Engine.Kind == ProbeComponentLine || Engine.Kind == ProbeComponentSweep;
		if (bPerComponent)
		{
			if (A.GetComponent() != Pair.Engine.Component || B.GetComponent() != Pair.Merged.Component)
			{
				return TEXT("a per-component hit that names another component");
			}
		}
		else if (A.GetActor() != Pair.Engine.Actor || B.GetActor() != Pair.Merged.Actor)
		{
			return TEXT("a hit on an actor that is not the copy's own");
		}
		if (A.bBlockingHit != B.bBlockingHit || A.bStartPenetrating != B.bStartPenetrating)
		{
			return FString::Printf(TEXT("blocking %d/%d penetrating %d/%d"), A.bBlockingHit, B.bBlockingHit, A.bStartPenetrating, B.bStartPenetrating);
		}
		if (FMath::Abs(A.Distance - B.Distance) > DistanceToleranceCm)
		{
			return FString::Printf(TEXT("distance %.4f against %.4f"), A.Distance, B.Distance);
		}
		// A swept shape that meets a face or an edge along a line or a patch touches it at many points at once, all at the same distance: which one (and so which normal, at a
		// box's edge) the query reports is a tie the engine breaks by how the shapes are wrapped, not by where they are. Where the shapes stand is checked shape by shape
		// instead (CompareMergedShapes); a sweep agrees when it hits the same thing at the same distance.
		const bool bSweep = Engine.Kind == ProbeSphereDownSimple || Engine.Kind == ProbeSphereDownComplex || Engine.Kind == ProbeBoxDown
			|| Engine.Kind == ProbeCapsuleAlongY || Engine.Kind == ProbeComponentSweep;
		if (bSweep)
		{
			return A.PhysMaterial.Get() != B.PhysMaterial.Get() ? FString(TEXT("another physical material")) : FString();
		}
		if ((A.ImpactNormal - B.ImpactNormal).GetAbsMax() > NormalTolerance || (A.Normal - B.Normal).GetAbsMax() > NormalTolerance)
		{
			return FString::Printf(TEXT("normal %s against %s"), *A.ImpactNormal.ToString(), *B.ImpactNormal.ToString());
		}
		if ((A.ImpactPoint - (B.ImpactPoint - CopyOffset)).GetAbsMax() > PointToleranceCm)
		{
			return FString::Printf(TEXT("impact point %s against %s"), *A.ImpactPoint.ToString(), *(B.ImpactPoint - CopyOffset).ToString());
		}
		if (A.PhysMaterial.Get() != B.PhysMaterial.Get())
		{
			return TEXT("another physical material");
		}
		return FString();
	}

	/** Compares two runs of the same probes; reports the first few disagreements and returns how many there were. */
	int32 CompareMergedProbes(FAutomationTestBase& Test, const FString& Where, const TArray<FMergedProbe>& Engine, const TArray<FMergedProbe>& Merged, const FMergedPair& Pair)
	{
		if (!Test.TestEqual(Where + TEXT(": the same number of probes"), Merged.Num(), Engine.Num()))
		{
			return 1;
		}
		int32 Disagreements = 0;
		for (int32 Index = 0; Index < Engine.Num(); ++Index)
		{
			const FString Why = MergedProbeDisagreement(Engine[Index], Merged[Index], Pair);
			if (!Why.IsEmpty() && ++Disagreements <= 6)
			{
				Test.AddError(FString::Printf(TEXT("%s: probe kind %d on channel %d at %s: %s"), *Where, Engine[Index].Kind, static_cast<int32>(Engine[Index].Channel), *Engine[Index].Point.ToString(), *Why));
			}
		}
		if (Disagreements > 0)
		{
			Test.AddError(FString::Printf(TEXT("%s: %d of %d probes disagree"), *Where, Disagreements, Engine.Num()));
		}
		return Disagreements;
	}

	int32 CountMergedHits(const TArray<FMergedProbe>& Probes, const int32 Kind, const ECollisionChannel Channel)
	{
		int32 Hits = 0;
		for (const FMergedProbe& Probe : Probes)
		{
			Hits += Probe.Kind == Kind && Probe.Channel == Channel && Probe.bHit ? 1 : 0;
		}
		return Hits;
	}

	/** One shape of a body: its inner geometry's kind and where it stands in the world. */
	struct FMergedShapeInfo
	{
		int32 Kind = 0;
		FBox Bounds = FBox(ForceInit);
	};

	TArray<FMergedShapeInfo> MergedShapesOf(const FBodyInstance* Body)
	{
		TArray<FMergedShapeInfo> Out;
		if (!Body)
		{
			return Out;
		}
		FPhysicsCommand::ExecuteRead(Body->GetPhysicsActor(), [&Out](const FPhysicsActorHandle& Actor)
		{
			for (const auto& Shape : Actor->GetGameThreadAPI().ShapesArray())
			{
				FMergedShapeInfo& Info = Out.AddDefaulted_GetRef();
				Info.Kind = Shape->GetGeometry() ? static_cast<int32>(Chaos::GetInnerType(Shape->GetGeometry()->GetType())) : -1;
				const Chaos::FAABB3& Box = Shape->GetWorldSpaceShapeBounds();
				Info.Bounds = FBox(FVector(Box.Min()), FVector(Box.Max()));
			}
		});
		return Out;
	}

	/**
	 * Every instance's shapes, the engine's (its own body) against the merged copy's (its run in a body): the same kinds, in the same order, standing in the same place (their world
	 * bounds, the merged copy shifted back by the offset). What a probe could miss between its grid points, this does not. Reports the first few and returns how many differ.
	 */
	int32 CompareMergedShapes(FAutomationTestBase& Test, const FString& Where, const FMergedPair& Pair, const int32 ShapesEach, const int32 PerBody)
	{
		const TArray<FBodyInstance*>& EngineBodies = Pair.Engine.Engine->GetInstanceBodies();
		const TArray<FBodyInstance*>& MergedBodies = Pair.Merged.Merged->GetChunkBodies();
		TArray<TArray<FMergedShapeInfo>> MergedShapes;
		for (const FBodyInstance* Body : MergedBodies)
		{
			MergedShapes.Add(MergedShapesOf(Body));
		}
		int32 Differences = 0;
		const auto Report = [&Test, &Where, &Differences](const FString& Why)
		{
			if (++Differences <= 8)
			{
				Test.AddError(Where + TEXT(": ") + Why);
			}
		};
		for (int32 Instance = 0; Instance < EngineBodies.Num(); ++Instance)
		{
			const TArray<FMergedShapeInfo> Engine = MergedShapesOf(EngineBodies[Instance]);
			const int32 Body = Instance / PerBody;
			const int32 First = (Instance % PerBody) * ShapesEach;
			if (!MergedShapes.IsValidIndex(Body) || Engine.Num() != ShapesEach || MergedShapes[Body].Num() < First + ShapesEach)
			{
				Report(FString::Printf(TEXT("instance %d: %d engine shapes, merged body %d has %d"), Instance, Engine.Num(), Body, MergedShapes.IsValidIndex(Body) ? MergedShapes[Body].Num() : -1));
				continue;
			}
			for (int32 Shape = 0; Shape < ShapesEach; ++Shape)
			{
				const FMergedShapeInfo& A = Engine[Shape];
				const FMergedShapeInfo& B = MergedShapes[Body][First + Shape];
				const FBox Shifted = B.Bounds.ShiftBy(-CopyOffset);
				const double Off = FMath::Max((A.Bounds.Min - Shifted.Min).GetAbsMax(), (A.Bounds.Max - Shifted.Max).GetAbsMax());
				if (A.Kind != B.Kind || Off > PointToleranceCm)
				{
					Report(FString::Printf(TEXT("instance %d shape %d: kind %d at %s against kind %d at %s (%.3f cm off)"),
						Instance, Shape, A.Kind, *A.Bounds.ToString(), B.Kind, *Shifted.ToString(), Off));
				}
			}
		}
		if (Differences > 0)
		{
			Test.AddError(FString::Printf(TEXT("%s: %d shapes differ from the engine's"), *Where, Differences));
		}
		return Differences;
	}

	/** The number of bodies the merged copy should have: its instances (none has zero scale) in runs of InstancesPerBody. */
	int32 ExpectedMergedBodies(UMantleMergedInstancesComponent* Merged)
	{
		const int32 PerBody = UMantleMergedInstancesComponent::InstancesPerBody(Merged->GetBodySetup());
		return (Merged->GetInstanceCount() + PerBody - 1) / PerBody;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMergedInstancesParityTest,
	"Mantle.Runtime.Place.MergedInstances.Parity",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMergedInstancesParityTest::RunTest(const FString& Parameters)
{
	using namespace MantleMergedInstancesTestPrivate;

	FMergedTestAssets Assets;
	if (!Assets.Make(*this))
	{
		return false;
	}
	FMergedTestWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}

	const TArray<FTransform> Instances = MakeMergedTestInstances(InstanceCount);
	const TArray<FVector> Points = MakeMergedProbePoints(Instances);
	struct FMeshCase
	{
		const TCHAR* Name;
		UStaticMesh* Mesh;
	};
	const FMeshCase Cases[] = {
		{ TEXT("the engine's cube (simple collision)"), Assets.Cube },
		{ TEXT("a mesh of triangles (complex as simple)"), Assets.Triangles.Get() },
		{ TEXT("a mesh of simple shapes and triangles"), Assets.Mixed.Get() } };

	for (const FMeshCase& Case : Cases)
	{
		const FString Where = Case.Name;
		const FMergedPair Pair = MakePair(Fixture.World, Case.Mesh, Assets.Surface.Get(), Instances, /*bInvisibleWall*/ false);
		if (!TestTrue(Where + TEXT(": both copies stand"), Pair.Engine.Engine && Pair.Merged.Merged && Pair.Engine.Engine->IsRegistered() && Pair.Merged.Merged->IsRegistered()))
		{
			return false;
		}

		// The bodies: a few, each with several instances' shapes, and none for single instances.
		UMantleMergedInstancesComponent* Merged = Pair.Merged.Merged;
		UBodySetup* BodySetup = Merged->GetBodySetup();
		if (!TestNotNull(*(Where + TEXT(": a body setup")), BodySetup))
		{
			return false;
		}
		const int32 ShapesEach = UMantleMergedInstancesComponent::ShapesPerInstance(BodySetup);
		const int32 PerBody = UMantleMergedInstancesComponent::InstancesPerBody(BodySetup);
		TestTrue(Where + TEXT(": a body holds at most the cap of shapes"), ShapesEach * PerBody <= UMantleMergedInstancesComponent::MaxShapesPerBody);
		TestTrue(*FString::Printf(TEXT("%s: several instances to a body (%d shapes each, %d to a body)"), *Where, ShapesEach, PerBody), PerBody > 1);
		TestEqual(Where + TEXT(": every instance has its shapes in a body"), Merged->GetChunkBodies().Num(), ExpectedMergedBodies(Merged));
		TestTrue(Where + TEXT(": at least three bodies"), Merged->GetChunkBodies().Num() >= 3);
		TestTrue(Where + TEXT(": far fewer bodies than instances"), Merged->GetChunkBodies().Num() * 2 < Merged->GetInstanceCount());
		const TArray<FBodyInstance*>& EngineBodies = Pair.Engine.Engine->GetInstanceBodies();
		if (TestEqual(Where + TEXT(": the engine's copy has a body for every instance"), EngineBodies.Num(), Instances.Num()) && EngineBodies[0])
		{
			TestEqual(Where + TEXT(": the engine makes the shapes the count says"), FPhysicsInterface::GetNumShapes(EngineBodies[0]->GetPhysicsActor()), ShapesEach);
		}
		int32 TotalShapes = 0;
		for (const FBodyInstance* Body : Merged->GetChunkBodies())
		{
			const int32 Shapes = FPhysicsInterface::GetNumShapes(Body->GetPhysicsActor());
			TestTrue(Where + TEXT(": a body stays under the cap"), Shapes <= UMantleMergedInstancesComponent::MaxShapesPerBody);
			TotalShapes += Shapes;
		}
		TestEqual(Where + TEXT(": the bodies hold every instance's shapes"), TotalShapes, Instances.Num() * ShapesEach);
		if (Merged->GetChunkBodies().Num() >= 2)
		{
			TestTrue(Where + TEXT(": a hit's body index finds its body"), Merged->GetBodyInstance(NAME_None, true, 1) == Merged->GetChunkBodies()[1]);
		}

		TestEqual(Where + TEXT(": every shape stands where the engine's does"), CompareMergedShapes(*this, Where, Pair, ShapesEach, PerBody), 0);

		const TArray<FMergedProbe> EngineProbes = RunAllMergedProbes(Fixture.World, Pair.Engine, Points);
		const TArray<FMergedProbe> MergedProbes = RunAllMergedProbes(Fixture.World, Pair.Merged, Points);
		const int32 Disagreements = CompareMergedProbes(*this, Where, EngineProbes, MergedProbes, Pair);
		TestEqual(Where + TEXT(": no probe disagrees"), Disagreements, 0);

		// The comparison must have had something to compare: traces that hit, on every channel, and some that did not.
		for (const ECollisionChannel Channel : ProbeChannels)
		{
			const int32 Hits = CountMergedHits(EngineProbes, ProbeLineDownSimple, Channel);
			TestTrue(*FString::Printf(TEXT("%s: line traces down hit on channel %d (%d of %d)"), *Where, static_cast<int32>(Channel), Hits, Points.Num()), Hits > Points.Num() / 4 && Hits < Points.Num());
		}
		if (Case.Mesh != Assets.Cube)
		{
			// (The engine's cube may carry no triangles in a build that cooks none; the two meshes made here always do.)
			TestTrue(Where + TEXT(": complex sweeps hit"), CountMergedHits(EngineProbes, ProbeSphereDownComplex, ECC_Visibility) > Points.Num() / 4);
		}
		TestTrue(Where + TEXT(": the per-component line traces hit"), CountMergedHits(EngineProbes, ProbeComponentLine, ECC_Visibility) > Points.Num() / 4);
		TestTrue(Where + TEXT(": overlaps found something"), CountMergedHits(EngineProbes, ProbeOverlapSphere, ECC_Visibility) > Points.Num() / 4);

		Pair.Engine.Actor->Destroy();
		Pair.Merged.Actor->Destroy();
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMergedInstancesWholeEntityTest,
	"Mantle.Runtime.Place.MergedInstances.WholeEntityChanges",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMergedInstancesWholeEntityTest::RunTest(const FString& Parameters)
{
	using namespace MantleMergedInstancesTestPrivate;

	FMergedTestAssets Assets;
	if (!Assets.Make(*this))
	{
		return false;
	}
	FMergedTestWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	UWorld* World = Fixture.World;

	const TArray<FTransform> Instances = MakeMergedTestInstances(InstanceCount);
	// Fewer places to look at than the parity test: what is checked here is a whole set's behavior, not its geometry.
	TArray<FVector> Points = MakeMergedProbePoints(Instances);
	Points.SetNum(FMath::Min(Points.Num(), 120));
	const auto Lines = [](const TArray<FMergedProbe>& Probes, const ECollisionChannel Channel) { return CountMergedHits(Probes, ProbeLineDownSimple, Channel); };

	// What a script does to a whole entity: turn its collision off and on again.
	{
		FMergedPair Pair = MakePair(World, Assets.Mixed.Get(), Assets.Surface.Get(), Instances, /*bInvisibleWall*/ false);
		{
			UBodySetup* BodySetup = Pair.Merged.Merged->GetBodySetup();
			TestEqual(TEXT("before: every shape stands where the engine's does"), CompareMergedShapes(*this, TEXT("before"), Pair,
				UMantleMergedInstancesComponent::ShapesPerInstance(BodySetup), UMantleMergedInstancesComponent::InstancesPerBody(BodySetup)), 0);
		}
		const TArray<FMergedProbe> EngineBefore = RunAllMergedProbes(World, Pair.Engine, Points);
		const TArray<FMergedProbe> MergedBefore = RunAllMergedProbes(World, Pair.Merged, Points);
		TestEqual(TEXT("before: no probe disagrees"), CompareMergedProbes(*this, TEXT("before"), EngineBefore, MergedBefore, Pair), 0);
		TestTrue(TEXT("before: the copies are hit"), Lines(EngineBefore, ECC_Pawn) > 20 && Lines(MergedBefore, ECC_Pawn) > 20);

		Pair.Engine.Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Pair.Merged.Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		TestEqual(TEXT("with no collision the merged copy has no bodies"), Pair.Merged.Merged->GetChunkBodies().Num(), 0);
		const TArray<FMergedProbe> EngineOff = RunAllMergedProbes(World, Pair.Engine, Points);
		const TArray<FMergedProbe> MergedOff = RunAllMergedProbes(World, Pair.Merged, Points);
		for (const ECollisionChannel Channel : ProbeChannels)
		{
			TestEqual(*FString::Printf(TEXT("no collision: the engine's copy is not hit on channel %d"), static_cast<int32>(Channel)), Lines(EngineOff, Channel), 0);
			TestEqual(*FString::Printf(TEXT("no collision: the merged copy is not hit on channel %d"), static_cast<int32>(Channel)), Lines(MergedOff, Channel), 0);
		}
		TestEqual(TEXT("no collision: no per-component trace hits either"), CountMergedHits(MergedOff, ProbeComponentLine, ECC_Visibility) + CountMergedHits(EngineOff, ProbeComponentLine, ECC_Visibility), 0);

		Pair.Engine.Component->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Pair.Merged.Component->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		TestEqual(TEXT("with collision again the merged copy has its bodies"), Pair.Merged.Merged->GetChunkBodies().Num(), ExpectedMergedBodies(Pair.Merged.Merged));
		const TArray<FMergedProbe> EngineOn = RunAllMergedProbes(World, Pair.Engine, Points);
		const TArray<FMergedProbe> MergedOn = RunAllMergedProbes(World, Pair.Merged, Points);
		TestEqual(TEXT("collision back: no probe disagrees"), CompareMergedProbes(*this, TEXT("collision back"), EngineOn, MergedOn, Pair), 0);
		TestEqual(TEXT("collision back: the engine's copy is hit as before"), Lines(EngineOn, ECC_Pawn), Lines(EngineBefore, ECC_Pawn));
		TestEqual(TEXT("collision back: the merged copy is hit as before"), Lines(MergedOn, ECC_Pawn), Lines(MergedBefore, ECC_Pawn));

		// Destroying the entity's actor ends its collision.
		Pair.Engine.Actor->Destroy();
		Pair.Merged.Actor->Destroy();
		FMergedCopy GoneEngine = Pair.Engine;
		FMergedCopy GoneMerged = Pair.Merged;
		for (const FVector& Point : Points)
		{
			FMergedProbe Probe;
			RunMergedProbe(World, GoneEngine, ProbeLineDownSimple, ECC_Pawn, Point, Probe);
			TestFalse(TEXT("destroyed: the engine's copy is not hit"), Probe.bHit);
			RunMergedProbe(World, GoneMerged, ProbeLineDownSimple, ECC_Pawn, Point, Probe);
			TestFalse(TEXT("destroyed: the merged copy is not hit"), Probe.bHit);
		}
	}

	// A script's Entity:Clone spawns a new actor with the entity's as its template and makes its root Movable: the copy keeps every instance and its collision, and moving it
	// moves its collision (as the engine's component's copy does).
	{
		FMergedPair Pair = MakePair(World, Assets.Cube, Assets.Surface.Get(), Instances, /*bInvisibleWall*/ false);
		const auto CloneOf = [World](const FMergedCopy& Source)
		{
			FActorSpawnParameters Params;
			Params.Template = Source.Actor;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			FMergedCopy Copy = Source;
			Copy.Actor = World->SpawnActor<AActor>(Source.Actor->GetClass(), Source.Actor->GetActorTransform(), Params);
			Copy.Component = Copy.Actor ? Cast<UStaticMeshComponent>(Copy.Actor->GetRootComponent()) : nullptr;
			Copy.Engine = Cast<UInstancedStaticMeshComponent>(Copy.Component);
			Copy.Merged = Cast<UMantleMergedInstancesComponent>(Copy.Component);
			if (Copy.Component)
			{
				Copy.Component->SetMobility(EComponentMobility::Movable);
				Copy.Actor->SetActorTransform(Source.Actor->GetActorTransform(), false, nullptr, ETeleportType::TeleportPhysics);
			}
			return Copy;
		};
		// The originals go first, so the clones are all a probe can hit.
		FMergedPair Clones;
		Clones.Engine = CloneOf(Pair.Engine);
		Clones.Merged = CloneOf(Pair.Merged);
		Pair.Engine.Actor->Destroy();
		Pair.Merged.Actor->Destroy();
		if (TestTrue(TEXT("clone: both copies were cloned"), Clones.Engine.Engine && Clones.Merged.Merged))
		{
			TestEqual(TEXT("clone: the merged clone keeps every instance"), Clones.Merged.Merged->GetInstanceCount(), Instances.Num());
			TestTrue(TEXT("clone: and has bodies"), Clones.Merged.Merged->GetChunkBodies().Num() > 0);
			const TArray<FMergedProbe> EngineClone = RunAllMergedProbes(World, Clones.Engine, Points);
			const TArray<FMergedProbe> MergedClone = RunAllMergedProbes(World, Clones.Merged, Points);
			TestEqual(TEXT("clone: no probe disagrees"), CompareMergedProbes(*this, TEXT("clone"), EngineClone, MergedClone, Clones), 0);
			TestTrue(TEXT("clone: the copies are hit"), Lines(MergedClone, ECC_Pawn) > 20);

			// Moved 50 m up: nothing is left where they stood, and the probes moved with them agree again.
			const FVector Lift(0.0, 0.0, 5000.0);
			for (FMergedCopy* Copy : { &Clones.Engine, &Clones.Merged })
			{
				Copy->Actor->SetActorLocation(Copy->Actor->GetActorLocation() + Lift, false, nullptr, ETeleportType::TeleportPhysics);
			}
			const TArray<FMergedProbe> MergedLeft = RunAllMergedProbes(World, Clones.Merged, Points);
			TestEqual(TEXT("moved: nothing of the merged clone is left where it stood"), Lines(MergedLeft, ECC_Pawn), 0);
			for (FMergedCopy* Copy : { &Clones.Engine, &Clones.Merged })
			{
				Copy->Transform.AddToTranslation(Lift);
			}
			const TArray<FMergedProbe> EngineMoved = RunAllMergedProbes(World, Clones.Engine, Points);
			const TArray<FMergedProbe> MergedMoved = RunAllMergedProbes(World, Clones.Merged, Points);
			TestEqual(TEXT("moved: no probe disagrees"), CompareMergedProbes(*this, TEXT("moved"), EngineMoved, MergedMoved, Clones), 0);
			TestEqual(TEXT("moved: the merged clone is hit where it now stands, as often as the engine's"), Lines(MergedMoved, ECC_Pawn), Lines(EngineMoved, ECC_Pawn));
			Clones.Engine.Actor->Destroy();
			Clones.Merged.Actor->Destroy();
		}
	}

	// An invisible wall (the collision-only mesh that lets sight through): sight traces pass both copies, the pawn's still stops at both.
	{
		FMergedPair Pair = MakePair(World, Assets.Cube, Assets.Surface.Get(), Instances, /*bInvisibleWall*/ true);
		const TArray<FMergedProbe> EngineWall = RunAllMergedProbes(World, Pair.Engine, Points);
		const TArray<FMergedProbe> MergedWall = RunAllMergedProbes(World, Pair.Merged, Points);
		TestEqual(TEXT("wall: no probe disagrees"), CompareMergedProbes(*this, TEXT("wall"), EngineWall, MergedWall, Pair), 0);
		TestEqual(TEXT("wall: a sight trace passes the engine's copy"), Lines(EngineWall, ECC_Visibility), 0);
		TestEqual(TEXT("wall: a sight trace passes the merged copy"), Lines(MergedWall, ECC_Visibility), 0);
		TestTrue(TEXT("wall: a pawn trace stops at the engine's copy"), Lines(EngineWall, ECC_Pawn) > 20);
		TestEqual(TEXT("wall: and at the merged copy as often"), Lines(MergedWall, ECC_Pawn), Lines(EngineWall, ECC_Pawn));
		TestTrue(TEXT("wall: the camera's trace stops at it too"), Lines(MergedWall, ECC_Camera) > 20);
		Pair.Engine.Actor->Destroy();
		Pair.Merged.Actor->Destroy();
	}
	return true;
}

namespace MantleMergedInstancesTestPrivate
{
	/** A place of one static blocking InstancedMesh of Count cubes, 300 cm apart in a grid, at x = 1000. */
	FMantleParseResult ParseMergedPlace(const int32 Count)
	{
		TArray<FString> Instances;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Instances.Add(FString::Printf(TEXT("[%d,%d,0,0,0,0,1,1,1]"), (Index % 20) * 300, (Index / 20) * 300));
		}
		const FString Json = FString::Printf(
			TEXT("{\"schemaVersion\":1,\"placeId\":\"plc_merged01\",\"revision\":{\"number\":1,\"state\":\"draft\"},"
				 "\"metadata\":{\"name\":\"Merged\",\"authorId\":\"usr_4b1d\"},\"settings\":{},"
				 "\"assets\":[{\"id\":\"ast_cube\",\"kind\":\"StaticMesh\",\"hash\":\"md5:0123456789abcdef\",\"bytes\":1024}],"
				 "\"entities\":[{\"id\":\"ent_blockers\",\"type\":\"InstancedMesh\",\"parent\":null,"
				 "\"transform\":{\"location\":[1000,0,0],\"rotation\":[0,0,0],\"scale\":[1,1,1]},"
				 "\"properties\":{\"mesh\":\"ast_cube\",\"collision\":\"blocking\",\"instances\":[%s]}}]}"),
			*FString::Join(Instances, TEXT(",")));
		return FMantlePlaceParser::ParseFromString(Json);
	}

	/** Builds the place in World and checks the entity's root: the merged component on a server, the engine's instanced one anywhere else. */
	void BuildAndCheckMergedPlace(FAutomationTestBase& Test, UWorld* World, const FString& Where, const bool bExpectMerged)
	{
		const FMantleParseResult Parsed = ParseMergedPlace(InstanceCount);
		if (!Test.TestTrue(Where + TEXT(": the place parses"), Parsed.bSuccess))
		{
			Test.AddError(Parsed.ErrorSummary());
			return;
		}
		FMantleBuildOptions Options;
		Options.bRuntimeEnvironment = false;
		FMantleBuildResult Built = FMantlePlaceBuilder::Build(World, Parsed.Place, Options);
		Test.TestTrue(Where + TEXT(": the build succeeds"), Built.bSuccess);

		AActor* Actor = Built.Actors.FindRef(TEXT("ent_blockers"));
		if (!Test.TestNotNull(*(Where + TEXT(": the entity's actor")), Actor))
		{
			FMantlePlaceBuilder::Teardown(Built);
			return;
		}
		USceneComponent* Root = Actor->GetRootComponent();
		const UInstancedStaticMeshComponent* EngineInstances = Cast<UInstancedStaticMeshComponent>(Root);
		UMantleMergedInstancesComponent* Merged = Cast<UMantleMergedInstancesComponent>(Root);
		if (bExpectMerged)
		{
			if (Test.TestNotNull(*(Where + TEXT(": the root is a merged component")), Merged))
			{
				Test.TestNull(*(Where + TEXT(": and no engine instanced component stands")), Actor->FindComponentByClass<UInstancedStaticMeshComponent>());
				Test.TestEqual(Where + TEXT(": every instance is in it"), Merged->GetInstanceCount(), InstanceCount);
				Test.TestEqual(Where + TEXT(": it is Static"), static_cast<int32>(Merged->Mobility), static_cast<int32>(EComponentMobility::Static));
				const int32 Expected = ExpectedMergedBodies(Merged);
				Test.TestEqual(Where + TEXT(": its bodies are the expected number of runs"), Merged->GetChunkBodies().Num(), Expected);
				Test.TestTrue(Where + TEXT(": at least two runs"), Expected >= 2);
				Test.TestTrue(Where + TEXT(": and fewer bodies than instances"), Merged->GetChunkBodies().Num() < InstanceCount);
				// Later pieces of a long list go in after the component stands (the cell streamer): they get bodies of their own, and are hit.
				const int32 BodiesBefore = Merged->GetChunkBodies().Num();
				Merged->AddInstances({ FTransform(FVector(0.0, -3000.0, 0.0)), FTransform(FVector(300.0, -3000.0, 0.0)) });
				Test.TestEqual(Where + TEXT(": two more instances are in it"), Merged->GetInstanceCount(), InstanceCount + 2);
				Test.TestEqual(Where + TEXT(": they have a body of their own"), Merged->GetChunkBodies().Num(), BodiesBefore + 1);
				FHitResult Added;
				const FCollisionQueryParams AddedQuery(SCENE_QUERY_STAT(MantleMergedInstancesTest), false);
				Test.TestTrue(Where + TEXT(": a trace hits an added instance"), World->LineTraceSingleByChannel(Added, FVector(1300.0, -3000.0, 300.0), FVector(1300.0, -3000.0, -300.0), ECC_Pawn, AddedQuery) && Added.GetActor() == Actor);
			}
		}
		else
		{
			Test.TestNull(*(Where + TEXT(": the root is not a merged component")), Merged);
			if (Test.TestNotNull(*(Where + TEXT(": the root is the engine's instanced component")), EngineInstances))
			{
				Test.TestEqual(Where + TEXT(": every instance is in it"), EngineInstances->GetInstanceCount(), InstanceCount);
			}
		}

		// A trace at the first instance (a cube at x = 1000, 100 cm tall) hits the actor the build mapped to the entity.
		FHitResult Hit;
		const FCollisionQueryParams Query(SCENE_QUERY_STAT(MantleMergedInstancesTest), false);
		const bool bHit = World->LineTraceSingleByChannel(Hit, FVector(1000.0, 0.0, 400.0), FVector(1000.0, 0.0, -400.0), ECC_Visibility, Query);
		Test.TestTrue(Where + TEXT(": a trace down at the first instance hits"), bHit);
		Test.TestTrue(Where + TEXT(": the actor it hits is the entity's"), bHit && Hit.GetActor() == Actor);
		Test.TestTrue(Where + TEXT(": at the cube's top, z = 50"), bHit && FMath::Abs(Hit.ImpactPoint.Z - 50.0) < 0.1);

		FMantlePlaceBuilder::Teardown(Built);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMergedInstancesGameBuildTest,
	"Mantle.Runtime.Place.MergedInstances.ServerBuild",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMergedInstancesGameBuildTest::RunTest(const FString& Parameters)
{
	using namespace MantleMergedInstancesTestPrivate;

	// Anywhere but a dedicated server a set of instances is the engine's instanced component, as before.
	{
		FMergedTestWorld Fixture;
		if (!TestNotNull(TEXT("a world"), Fixture.World))
		{
			return false;
		}
		BuildAndCheckMergedPlace(*this, Fixture.World, TEXT("a game"), /*bExpectMerged*/ false);
	}

#if WITH_EDITOR
	// A dedicated server: the same place, held as a few bodies with many shapes each. The world is made a server the way MantleCollisionOnlyMeshTest's is.
	if (!TestNotNull(TEXT("an engine"), GEngine))
	{
		return false;
	}
	UWorld* ServerWorld = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("MantleMergedInstancesServerTestWorld"));
	if (!TestNotNull(TEXT("a server world"), ServerWorld))
	{
		return false;
	}
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(ServerWorld);
	Context.RunAsDedicated = true;
	UDemoNetDriver* Driver = NewObject<UDemoNetDriver>(GetTransientPackage());
	Driver->SetWorld(ServerWorld);
	ServerWorld->SetNetDriver(Driver);
	if (TestEqual(TEXT("the test world is a dedicated server"), static_cast<int32>(ServerWorld->GetNetMode()), static_cast<int32>(NM_DedicatedServer)))
	{
		BuildAndCheckMergedPlace(*this, ServerWorld, TEXT("a dedicated server"), /*bExpectMerged*/ true);
	}
	ServerWorld->SetNetDriver(nullptr);
	Driver->SetWorld(nullptr);
	GEngine->DestroyWorldContext(ServerWorld);
	ServerWorld->DestroyWorld(/*bInformEngineOfWorld*/ false);
#endif // WITH_EDITOR
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
