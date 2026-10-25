// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleShapeKind.h"

#include "MantleBodyMeasurements.h"

#define LOCTEXT_NAMESPACE "MantleVehicleShapeKind"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVehicleShapeKindPrivate
{
	/** Where a seated driver's hips go from the seat location: 4.3 cm ahead of it and 34 cm under it (FMantleVehicleConverter::GuessSeatContacts). */
	const FVector HipsFromSeat(4.3, 0.0, -34.0);
}

TArray<FMantleGhostCapsule> FMantleVehicleShapeKind::GhostPose(const TArray<FMantleBuilderPoint>& Points) const
{
	// The seated character is drawn about the seat surface, a hip's height under the hips.
	TArray<FMantleGhostCapsule> Ghost = FMantleBodyMeasurements::SeatedGhost();
	const FVector SeatSurface = Spec.Seat + MantleVehicleShapeKindPrivate::HipsFromSeat - FVector(0.0, 0.0, FMantleBodyMeasurements::SeatedHipHeightCm());
	for (FMantleGhostCapsule& Capsule : Ghost)
	{
		Capsule.Start += SeatSurface;
		Capsule.End += SeatSurface;
	}
	return Ghost;
}

FMantleBuilderOverlay FMantleVehicleShapeKind::Overlay(const TArray<FMantleBuilderPoint>& Points) const
{
	FMantleBuilderOverlay Out;

	FMantleBuilderOverlayMarker& Driver = Out.Markers.AddDefaulted_GetRef();
	Driver.Location = Spec.Seat;
	// A two-wheeler's seat is a rider's, an aircraft's a pilot's.
	const bool bPilot = Spec.Type == EMantleChassisType::Plane || Spec.Type == EMantleChassisType::Helicopter;
	Driver.Label = Spec.bCentreline ? LOCTEXT("Rider", "Rider") : bPilot ? LOCTEXT("Pilot", "Pilot") : LOCTEXT("Driver", "Driver");
	Driver.Color = FLinearColor(1.f, 0.5f, 0.05f);
	Driver.RadiusCm = 4.f;

	if (Spec.bPassenger)
	{
		FMantleBuilderOverlayMarker& Passenger = Out.Markers.AddDefaulted_GetRef();
		Passenger.Location = FVector(Spec.Seat.X, -Spec.Seat.Y, Spec.Seat.Z);
		Passenger.Label = LOCTEXT("Passenger", "Passenger");
		Passenger.Color = FLinearColor(1.f, 0.5f, 0.05f);
		Passenger.RadiusCm = 4.f;
	}

	// The point the vehicle's weight is centred on: what makes it tip, or not.
	FMantleBuilderOverlayMarker& Weight = Out.Markers.AddDefaulted_GetRef();
	Weight.Location = Spec.CenterOfMass;
	Weight.Label = LOCTEXT("Weight", "Weight is centred here");
	Weight.Color = FLinearColor(0.35f, 0.75f, 1.f);
	Weight.RadiusCm = 5.f;
	return Out;
}

FText FMantleVehicleShapeKind::Hint() const
{
	return LOCTEXT("Hint", "Right-drag to turn the view, scroll to zoom, press F to frame the vehicle. The grey figure is a character, so you can see how big the vehicle is.");
}

#undef LOCTEXT_NAMESPACE
