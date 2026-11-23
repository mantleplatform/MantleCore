// Copyright Joshua Eger. All Rights Reserved.
#include "MantleTriggerShapeExport.h"

#include "MantleAssetLibrary.h"
#include "MantleBlockingVolumeExport.h"
#include "MantlePlaceExporter.h"

#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SphereComponent.h"
#include "Engine/TriggerVolume.h"

bool FMantleTriggerShapeExport::IsTriggerBrush(const AActor* Actor)
{
	return Actor && Actor->IsA(ATriggerVolume::StaticClass());
}

bool FMantleTriggerShapeExport::ReadBoxBrush(const AActor* Actor, FVector& OutCenter, FVector& OutExtent)
{
	FVector Size = FVector::ZeroVector;
	if (!IsTriggerBrush(Actor) || !FMantleBlockingVolumeExport::ReadLocalBox(Actor, OutCenter, Size))
	{
		return false;
	}
	OutExtent = Size * 0.5;
	return true;
}

bool FMantleTriggerShapeExport::IsBoxTriggerBrush(const AActor* Actor)
{
	FVector Center;
	FVector Extent;
	return ReadBoxBrush(Actor, Center, Extent);
}

bool FMantleTriggerShapeExport::IsCapsule(const UActorComponent* Component)
{
	return Component && Component->IsA(UCapsuleComponent::StaticClass());
}

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleTriggerShapePrivate
{
	/** Whether Shape answers any channel with Block. */
	bool BlocksAnyChannel(const UPrimitiveComponent* Shape)
	{
		for (int32 Channel = 0; Channel < ECC_MAX; ++Channel)
		{
			if (Shape->GetCollisionResponseToChannel(static_cast<ECollisionChannel>(Channel)) == ECR_Block)
			{
				return true;
			}
		}
		return false;
	}

	/** Generates overlap events, can be queried, and blocks no channel. (A query-only shape that blocks still stops a character's sweep: a wall.) */
	bool ActsAsTrigger(const UPrimitiveComponent* Shape)
	{
		if (!Shape || !Shape->GetGenerateOverlapEvents())
		{
			return false;
		}
		if (!CollisionEnabledHasQuery(Shape->GetCollisionEnabled()))
		{
			return false; // no query: nothing can overlap it
		}
		return !BlocksAnyChannel(Shape);
	}
}

bool FMantleTriggerShapeExport::IsTriggerCapsule(const UActorComponent* Component)
{
	return IsCapsule(Component) && MantleTriggerShapePrivate::ActsAsTrigger(Cast<UPrimitiveComponent>(Component));
}

bool FMantleTriggerShapeExport::IsTriggerShape(const UActorComponent* Component)
{
	return Component && (Component->IsA(UBoxComponent::StaticClass()) || Component->IsA(USphereComponent::StaticClass()) || IsCapsule(Component))
		&& MantleTriggerShapePrivate::ActsAsTrigger(Cast<UPrimitiveComponent>(Component));
}

bool FMantleTriggerShapeExport::IsCollisionShape(const UActorComponent* Component)
{
	const UPrimitiveComponent* Shape = Cast<UPrimitiveComponent>(Component);
	return Shape && (Shape->IsA(UBoxComponent::StaticClass()) || Shape->IsA(USphereComponent::StaticClass()))
		&& Shape->GetCollisionEnabled() != ECollisionEnabled::NoCollision && !MantleTriggerShapePrivate::ActsAsTrigger(Shape)
		&& MantleTriggerShapePrivate::BlocksAnyChannel(Shape);
}

bool FMantleTriggerShapeExport::ReadCollisionShape(const UActorComponent* Component, FMantleStaticMeshProps& OutProps, FVector& OutSizeScale)
{
	const UPrimitiveComponent* Shape = Cast<UPrimitiveComponent>(Component);
	if (!Shape || !IsCollisionShape(Shape))
	{
		return false;
	}
	const UBoxComponent* Box = Cast<UBoxComponent>(Shape);
	const USphereComponent* Sphere = Cast<USphereComponent>(Shape);
	const FString MeshId = FMantleAssetLibrary::Get().FindIdByPath(FSoftObjectPath(
		Box ? TEXT("/Engine/BasicShapes/Cube.Cube") : TEXT("/Engine/BasicShapes/Sphere.Sphere")));
	if (MeshId.IsEmpty())
	{
		return false;
	}
	OutProps = FMantleStaticMeshProps();
	OutProps.Mesh = MeshId;
	OutProps.Collision = EMantleCollision::Blocking;
	OutProps.bVisible = false;
	OutProps.bCastShadow = false;
	OutProps.bBlocksVisibility = FMantlePlaceExporter::BlocksVisibility(Shape);
	// The library cube and sphere are 100 cm across, centered.
	OutSizeScale = Box ? Box->GetUnscaledBoxExtent() * 2.0 / 100.0 : FVector(Sphere->GetUnscaledSphereRadius() * 2.0 / 100.0);
	return true;
}

void FMantleTriggerShapeExport::ReadCapsule(const UCapsuleComponent* Capsule, EMantleTriggerShape& OutShape, FVector& OutExtent)
{
	const double Radius = Capsule->GetUnscaledCapsuleRadius();
	const double HalfHeight = Capsule->GetUnscaledCapsuleHalfHeight();
	// A capsule's half-height is measured to the tip of its cap, so one no taller than it is wide is a sphere.
	if (HalfHeight <= Radius * 1.001)
	{
		OutShape = EMantleTriggerShape::Sphere;
		OutExtent = FVector(Radius, Radius, Radius);
		return;
	}
	OutShape = EMantleTriggerShape::Box;
	OutExtent = FVector(Radius, Radius, HalfHeight);
}
