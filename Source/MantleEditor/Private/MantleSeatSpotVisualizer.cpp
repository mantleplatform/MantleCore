// Copyright Joshua Eger. All Rights Reserved.
#include "MantleSeatSpotVisualizer.h"
#include "MantleGizmoStyle.h"
#include "MantleSeatExport.h"
#include "MantleSeatSpot.h"

#include "CanvasItem.h"
#include "CanvasTypes.h"
#include "Engine/Engine.h"
#include "SceneManagement.h"
#include "SceneView.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleSeatSpotVisualizerPrivate
{
	/** The seated hips' footprint in centimetres: X is forward, Y is sideways, Z is up. */
	const FVector HipHalfExtent(20.0, 20.0, 10.0);
	constexpr double FacingLength = 60.0;
	constexpr double ArrowHeadLength = 12.0;
	constexpr double UpLength = 30.0;
	constexpr double FloorCrossHalf = 8.0;
	constexpr double DashLength = 6.0;

	void Add(TArray<FMantleSeatGizmoLine>& Out, const FVector& Start, const FVector& End, const FLinearColor& Color)
	{
		Out.Add(FMantleSeatGizmoLine{ Start, End, Color });
	}
}

void FMantleSeatSpotVisualizer::Lines(const FVector& Hip, double Yaw, const TOptional<double>& HipHeight, TArray<FMantleSeatGizmoLine>& Out)
{
	using namespace MantleSeatSpotVisualizerPrivate;
	const FQuat Heading = FRotator(0.0, Yaw, 0.0).Quaternion();
	const FVector Forward = Heading.GetForwardVector(), Right = Heading.GetRightVector(), Up = FVector::UpVector;

	// The hips: a flat box, its twelve edges.
	const FVector X = Forward * HipHalfExtent.X, Y = Right * HipHalfExtent.Y, Z = Up * HipHalfExtent.Z;
	for (const double SZ : { -1.0, 1.0 })
	{
		const FVector Corners[4] = { Hip - X - Y + Z * SZ, Hip + X - Y + Z * SZ, Hip + X + Y + Z * SZ, Hip - X + Y + Z * SZ };
		for (int32 i = 0; i < 4; ++i)
		{
			Add(Out, Corners[i], Corners[(i + 1) % 4], MantleGizmoStyle::Reach);
		}
	}
	for (const FVector& Corner : { -X - Y, X - Y, X + Y, -X + Y })
	{
		Add(Out, Hip + Corner - Z, Hip + Corner + Z, MantleGizmoStyle::Reach);
	}

	// The way the body faces: a shaft and a four-line head.
	const FVector Tip = Hip + Forward * FacingLength;
	Add(Out, Hip, Tip, MantleGizmoStyle::Facing);
	for (const FVector& Side : { Right, -Right, Up, -Up })
	{
		Add(Out, Tip, Tip - Forward * ArrowHeadLength + Side * ArrowHeadLength * 0.5, MantleGizmoStyle::Facing);
	}

	Add(Out, Hip, Hip + Up * UpLength, MantleGizmoStyle::Up);

	// The hip height: down to the floor, solid while a place can hold it and dashed when Check this place would refuse it.
	if (HipHeight.IsSet())
	{
		const FVector Floor = Hip - Up * HipHeight.GetValue();
		if (FMantleSeatExport::HeightProblem(true, HipHeight.GetValue()).IsEmpty())
		{
			Add(Out, Hip, Floor, MantleGizmoStyle::Outline);
		}
		else
		{
			for (double Along = 0.0; Along < HipHeight.GetValue(); Along += DashLength * 2.0)
			{
				Add(Out, Hip - Up * Along, Hip - Up * FMath::Min(Along + DashLength, HipHeight.GetValue()), MantleGizmoStyle::Outline);
			}
		}
		Add(Out, Floor - Forward * FloorCrossHalf, Floor + Forward * FloorCrossHalf, MantleGizmoStyle::Outline);
		Add(Out, Floor - Right * FloorCrossHalf, Floor + Right * FloorCrossHalf, MantleGizmoStyle::Outline);
	}
}

void FMantleSeatSpotVisualizer::DrawVisualization(const UActorComponent* Component, const FSceneView* View, FPrimitiveDrawInterface* PDI)
{
	const UMantleSeatSpotComponent* Seat = Cast<UMantleSeatSpotComponent>(Component);
	if (!Seat)
	{
		return;
	}
	double HipHeight = 0.0;
	TArray<FMantleSeatGizmoLine> Drawn;
	Lines(Seat->GetComponentLocation(), Seat->GetComponentRotation().Yaw,
		FMantleSeatExport::FloorBelow(Seat, HipHeight) ? TOptional<double>(HipHeight) : TOptional<double>(), Drawn);
	for (const FMantleSeatGizmoLine& Line : Drawn)
	{
		PDI->DrawLine(Line.Start, Line.End, Line.Color, SDPG_Foreground, 0.f);
	}
}

void FMantleSeatSpotVisualizer::DrawVisualizationHUD(const UActorComponent* Component, const FViewport* Viewport, const FSceneView* View, FCanvas* Canvas)
{
	const UMantleSeatSpotComponent* Seat = Cast<UMantleSeatSpotComponent>(Component);
	if (!Seat || !View || !Canvas)
	{
		return;
	}
	double HipHeight = 0.0;
	const bool bFound = FMantleSeatExport::FloorBelow(Seat, HipHeight);
	const FString Text = bFound ? FString::Printf(TEXT("Hip %.0f cm"), HipHeight) : FString(TEXT("Nothing to sit on"));
	FVector2D Pixel;
	if (View->WorldToPixel(Seat->GetComponentLocation() + FVector(0.0, 0.0, 40.0), Pixel))
	{
		FCanvasTextItem Label(Pixel, FText::FromString(Text), GEngine->GetSmallFont(), MantleGizmoStyle::Outline);
		Canvas->DrawItem(Label);
	}
}
