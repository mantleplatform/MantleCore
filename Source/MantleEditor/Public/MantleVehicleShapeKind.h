// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleBuilderKind.h"
#include "MantleChassisSpec.h"

/**
 * The vehicle side of the shared builder viewport (#793 step 4): there are no points to place, so it only tells the viewport what to draw
 * around the gray chassis the window shows (the window sets the mesh itself): the Mantle character seated at the driver's seat (a rider's or a pilot's),
 * so a creator sees how big the vehicle is for a person, and a label on each seat and on the center of mass.
 */
class MANTLEEDITOR_API FMantleVehicleShapeKind : public IMantleBuilderKind
{
public:
	/** The chassis the viewport shows. Ask the viewport to refresh (SMantleBuilderViewport::RefreshFromKind) after changing it. */
	void SetSpec(const FMantleChassisSpec& InSpec) { Spec = InSpec; }

	/** Where Mantle thinks each wheel is, in the mesh's space (FMantleVehicleModelPreview::WheelDots): a dot goes on each. Refresh the viewport after changing it. */
	void SetWheelDots(const TArray<FVector>& InDots) { WheelDots = InDots; }

	// IMantleBuilderKind
	virtual TArray<FMantleBuilderPointRole> GetRoles() const override { return TArray<FMantleBuilderPointRole>(); }
	virtual TArray<FMantleBuilderPoint> GuessPoints(const UObject* Mesh) const override { return TArray<FMantleBuilderPoint>(); }
	virtual TArray<FMantleGhostCapsule> GhostPose(const TArray<FMantleBuilderPoint>& Points) const override;
	virtual FMantleBuilderOverlay Overlay(const TArray<FMantleBuilderPoint>& Points) const override;
	virtual FText Hint() const override;

private:
	FMantleChassisSpec Spec;
	TArray<FVector> WheelDots;
};
