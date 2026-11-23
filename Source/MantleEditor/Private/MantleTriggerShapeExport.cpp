// Copyright Joshua Eger. All Rights Reserved.
#include "MantleTriggerShapeExport.h"

#include "MantleBlockingVolumeExport.h"

#include "Components/CapsuleComponent.h"
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
