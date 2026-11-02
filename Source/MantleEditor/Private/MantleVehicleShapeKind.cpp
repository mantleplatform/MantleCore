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

FName FMantleVehicleShapeKind::BeamRole(const int32 Index)
{
	return FName(*FString::Printf(TEXT("Beam%d"), Index + 1));
}

TArray<FMantleBuilderPointRole> FMantleVehicleShapeKind::GetRoles() const
{
	TArray<FMantleBuilderPointRole> Roles;
	for (int32 Index = 0; Index < BeamCount; ++Index)
	{
		FMantleBuilderPointRole& Role = Roles.AddDefaulted_GetRef();
		Role.Role = BeamRole(Index);
		Role.Label = FText::Format(LOCTEXT("BeamLabel", "Headlight beam {0}"), FText::AsNumber(Index + 1));
		Role.Help = LOCTEXT("BeamHelp", "A real light that lights the road while the headlights are on. Put it at a headlight and point the arrow where the light should shine.");
		Role.Shape = EMantleBuilderPointShape::Arrow;
		Role.bRequired = false;
		Role.bRotates = true;
	}
	return Roles;
}

FMantleBuilderOverlay FMantleVehicleShapeKind::Overlay(const TArray<FMantleBuilderPoint>& Points) const
{
	FMantleBuilderOverlay Out;
	Out.Markers = LightMarkers;

	for (const FVector& Dot : WheelDots)
	{
		FMantleBuilderOverlayMarker& Wheel = Out.Markers.AddDefaulted_GetRef();
		Wheel.Location = Dot;
		Wheel.Color = FLinearColor(0.2f, 0.9f, 0.3f);
		Wheel.RadiusCm = 3.f;
	}

	FMantleBuilderOverlayMarker& Driver = Out.Markers.AddDefaulted_GetRef();
	Driver.Location = Spec.Seat;
	// A two-wheeler's seat is a rider's, an aircraft's a pilot's.
	const bool bPilot = Spec.Type == EMantleChassisType::Plane || Spec.Type == EMantleChassisType::Helicopter;
	Driver.Label = Spec.bCenterline ? LOCTEXT("Rider", "Rider") : bPilot ? LOCTEXT("Pilot", "Pilot") : LOCTEXT("Driver", "Driver");
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

	// The point the vehicle's weight is centered on: what makes it tip, or not.
	FMantleBuilderOverlayMarker& Weight = Out.Markers.AddDefaulted_GetRef();
	Weight.Location = Spec.CenterOfMass;
	Weight.Label = LOCTEXT("Weight", "Weight is centered here");
	Weight.Color = FLinearColor(0.35f, 0.75f, 1.f);
	Weight.RadiusCm = 5.f;
	return Out;
}

FText FMantleVehicleShapeKind::Hint() const
{
	if (BeamCount > 0)
	{
		return LOCTEXT("HintBeams", "Click a yellow arrow (a headlight beam), then drag the handles to move it or turn it to aim. Colored dots are the lights Mantle found: white headlights, red rear lights, gray reverse lights, amber signals. Right-drag to turn the view, scroll to zoom.");
	}
	return LOCTEXT("Hint", "Right-drag to turn the view, scroll to zoom, press F to frame the vehicle. The gray figure is a character, so you can see how big the vehicle is. Green dots are where Mantle thinks the wheels are.");
}

#undef LOCTEXT_NAMESPACE
