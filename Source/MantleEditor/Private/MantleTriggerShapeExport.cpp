// Copyright Joshua Eger. All Rights Reserved.
#include "MantleTriggerShapeExport.h"

#include "MantleBlockingVolumeExport.h"

#include "Components/CapsuleComponent.h"
#include "Engine/TriggerVolume.h"

bool FMantleTriggerShapeExport::IsTriggerBrush(const AActor* Actor)
{
	return Actor && Actor->IsA(ATriggerVolume::StaticClass());
}

bool FMantleTriggerShapeExport::ReadBoxBrush(const AActor* Actor, FVector& OutCenter, FVector& OutExtent, bool* bOutApproximated)
{
	FVector Size = FVector::ZeroVector;
	if (!IsTriggerBrush(Actor))
	{
		return false;
	}
	const bool bBox = FMantleBlockingVolumeExport::ReadLocalBox(Actor, OutCenter, Size);
	if (!bBox && !FMantleBlockingVolumeExport::ReadLocalBounds(Actor, OutCenter, Size))
	{
		return false;
	}
	if (bOutApproximated)
	{
		*bOutApproximated = !bBox;
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

bool FMantleTriggerShapeExport::IsTriggerCapsule(const UActorComponent* Component)
{
	const UCapsuleComponent* Capsule = Cast<UCapsuleComponent>(Component);
	if (!Capsule || !Capsule->GetGenerateOverlapEvents())
	{
		return false;
	}
	const ECollisionEnabled::Type Enabled = Capsule->GetCollisionEnabled();
	if (!CollisionEnabledHasQuery(Enabled))
	{
		return false; // no query: nothing can overlap it
	}
	// A trigger only when it blocks nothing a player or an object moves on: a query-only capsule that blocks still stops a character's
	// sweep, so it is a wall. Only these channels count: the engine's trigger profiles leave every other channel at its default response
	// (often Block), and that is no wall.
	static const ECollisionChannel Solid[] = { ECC_WorldStatic, ECC_WorldDynamic, ECC_Pawn, ECC_PhysicsBody, ECC_Vehicle, ECC_Destructible };
	for (const ECollisionChannel Channel : Solid)
	{
		if (Capsule->GetCollisionResponseToChannel(Channel) == ECR_Block)
		{
			return false;
		}
	}
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
