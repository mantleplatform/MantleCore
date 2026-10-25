// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "ComponentVisualizer.h"

/** One straight piece of a Mantle Seat's marker. */
struct FMantleSeatGizmoLine
{
	FVector Start = FVector::ZeroVector;
	FVector End = FVector::ZeroVector;
	FLinearColor Color = FLinearColor::White;
};

/** The viewport marker of a Mantle Seat (UMantleSeatSpotComponent), shown while its prop is selected. */
class FMantleSeatSpotVisualizer : public FComponentVisualizer
{
public:
	/**
	 * The marker as lines, in the overlay gizmo's look (MantleGizmoStyle): a box where the hips sit, an arrow for the way
	 * the body faces, a green up line, and, when HipHeight is set, a line down to the floor ending in a small cross,
	 * dashed when Check this place would refuse that height. Only Yaw counts, so the marker is flat. The viewport and
	 * Mantle.Dev.SeatGizmoShot both draw these. Pure.
	 */
	static void Lines(const FVector& Hip, double Yaw, const TOptional<double>& HipHeight, TArray<FMantleSeatGizmoLine>& Out);

	virtual void DrawVisualization(const UActorComponent* Component, const FSceneView* View, FPrimitiveDrawInterface* PDI) override;
	virtual void DrawVisualizationHUD(const UActorComponent* Component, const FViewport* Viewport, const FSceneView* View, FCanvas* Canvas) override;
};
