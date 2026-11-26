// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleBuilderKind.h"
#include "MantleChassisSpec.h"

/**
 * The vehicle side of the shared builder viewport (#793 step 4): there are no points to place, so it only tells the viewport what to draw
 * around the creator's model on its frame (the window sets the mesh itself), and a dot on each wheel (FMantleVehicleBuilder::MakeModelPreview): the Mantle character seated at the driver's seat (a rider's or a pilot's),
 * so a creator sees how big the vehicle is for a person, and a label on each seat and on the center of mass. The extra seats (#449) have a figure each too, seated or
 * standing, and in the seats and paint step they are points to drag and turn, as the headlight beams are.
 */
class MANTLEEDITOR_API FMantleVehicleShapeKind : public IMantleBuilderKind
{
public:
	/** The chassis the viewport shows. Ask the viewport to refresh (SMantleBuilderViewport::RefreshFromKind) after changing it. */
	void SetSpec(const FMantleChassisSpec& InSpec) { Spec = InSpec; }

	/** Where Mantle thinks each wheel is, in the mesh's space (FMantleVehicleModelPreview::WheelDots): a dot goes on each. Refresh the viewport after changing it. */
	void SetWheelDots(const TArray<FVector>& InDots) { WheelDots = InDots; }

	/**
	 * The lights (the seats and paint step): BeamCount headlight beams as points to drag and turn (roles Beam1, Beam2, ...; a yellow arrow each), and a colored marker on
	 * each light section. Zero beams and no markers outside that step. Refresh the viewport after changing it.
	 */
	void SetLights(int32 InBeamCount, const TArray<FMantleBuilderOverlayMarker>& InMarkers) { BeamCount = InBeamCount; LightMarkers = InMarkers; }
	/** The role of beam Index (from 0): Beam1, Beam2, ... */
	static FName BeamRole(int32 Index);

	/**
	 * Whether the spec's extra seats (#449) are points to drag and turn (the seats and paint step: roles ExtraSeat1, ExtraSeat2, ..., an orange box each). They are
	 * drawn as figures either way: the same seated character as the driver's on a sit seat, standing on a stand spot. Refresh the viewport after changing it.
	 */
	void SetSeatPoints(bool bInSeatPoints) { bSeatPoints = bInSeatPoints; }
	/** The role of extra seat Index (from 0): ExtraSeat1, ExtraSeat2, ... */
	static FName SeatRole(int32 Index);

	// IMantleBuilderKind
	virtual TArray<FMantleBuilderPointRole> GetRoles() const override;
	virtual TArray<FMantleBuilderPoint> GuessPoints(const UObject* Mesh) const override { return TArray<FMantleBuilderPoint>(); }
	virtual TArray<FMantleGhostCapsule> GhostPose(const TArray<FMantleBuilderPoint>& Points) const override;
	virtual FMantleBuilderOverlay Overlay(const TArray<FMantleBuilderPoint>& Points) const override;
	virtual FText Hint() const override;

private:
	FMantleChassisSpec Spec;
	TArray<FVector> WheelDots;
	int32 BeamCount = 0;
	TArray<FMantleBuilderOverlayMarker> LightMarkers;
	bool bSeatPoints = false;
};
