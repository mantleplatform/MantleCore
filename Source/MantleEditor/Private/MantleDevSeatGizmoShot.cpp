// Copyright Joshua Eger. All Rights Reserved.
//
// Maintainer command: Mantle.Dev.SeatGizmoShot <Directory> [size]
// Draws a Mantle Seat's marker (GhzGarage/MantleCore#142) on a bench-sized block as the catalog thumbnail does (FMantleCosmeticThumbnail:
// the engine's thumbnail scene, its lights and floor), into <Directory>/seat_gizmo.png. The marker is the viewport's own lines
// (FMantleSeatSpotVisualizer::Lines), the hip 48 cm above the floor and facing the camera's left. Nothing is saved or uploaded.
// Logs MANTLE_SEAT_GIZMO_SHOT lines.
#include "MantleCosmeticThumbnail.h"
#include "MantlePng.h"
#include "MantleSeatSpotVisualizer.h"

#include "Components/LineBatchComponent.h"
#include "Engine/StaticMesh.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PreviewScene.h"

DEFINE_LOG_CATEGORY_STATIC(LogMantleSeatGizmoShot, Log, All);

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleDevSeatGizmoShotPrivate
{
	/** A bench: the engine's 100 cm cube, 160 long, 45 deep and 42 high, standing on the floor. */
	const FVector BenchScale(1.6, 0.45, 0.42);
	constexpr double HipHeightCm = 48.0;
	constexpr float LineThickness = 1.2f;

	void Run(const TArray<FString>& Args)
	{
		if (Args.Num() < 1)
		{
			UE_LOG(LogMantleSeatGizmoShot, Error, TEXT("MANTLE_SEAT_GIZMO_SHOT usage: Mantle.Dev.SeatGizmoShot <Directory> [size]"));
			return;
		}
		const int32 Size = Args.Num() > 1 ? FMath::Clamp(FCString::Atoi(*Args[1]), 128, 2048) : 1024;
		FString WhyNot;
		UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		if (!Cube || !FMantleCosmeticThumbnail::CanCapture(&WhyNot))
		{
			UE_LOG(LogMantleSeatGizmoShot, Error, TEXT("MANTLE_SEAT_GIZMO_SHOT failed: no cube, or nothing can render (%s)"), *WhyNot);
			return;
		}

		FMantleThumbnailSubject Shot;
		Shot.StaticMesh = Cube;
		Shot.StaticTransform = FTransform(FQuat::Identity, FVector(0.0, 0.0, 50.0 * BenchScale.Z), BenchScale);
		Shot.bFloor = true;
		Shot.Frame = FBox(FVector(-90.0, -60.0, 0.0), FVector(90.0, 60.0, 100.0));
		Shot.View = FRotator(-20.0f, -125.0f, 0.0f);
		Shot.Describe = TEXT("a Mantle Seat on a bench");
		// The seat faces -Y, off the bench's long side; its lines go in as a line batch so the capture draws them like the bench.
		Shot.Pose = [](FPreviewScene& Scene, TConstArrayView<USkeletalMeshComponent*>, FBox&)
		{
			ULineBatchComponent* Batch = NewObject<ULineBatchComponent>(GetTransientPackage());
			TArray<FMantleSeatGizmoLine> Lines;
			FMantleSeatSpotVisualizer::Lines(FVector(0.0, 0.0, HipHeightCm), -90.0, TOptional<double>(HipHeightCm), Lines);
			for (const FMantleSeatGizmoLine& Line : Lines)
			{
				Batch->DrawLine(Line.Start, Line.End, Line.Color, SDPG_Foreground, LineThickness, /*LifeTime*/ 3600.f);
			}
			Scene.AddComponent(Batch, FTransform::Identity);
		};

		TArray<FColor> Pixels;
		FString Error;
		const FString File = FPaths::Combine(Args[0], TEXT("seat_gizmo.png"));
		const bool bWritten = FMantleCosmeticThumbnail::Capture(Shot, Size, Pixels, Error)
			&& FFileHelper::SaveArrayToFile(FMantlePng::EncodeRGBA8(Size, Size, Pixels), *File);
		UE_LOG(LogMantleSeatGizmoShot, Display, TEXT("MANTLE_SEAT_GIZMO_SHOT %s %s"), bWritten ? *File : TEXT("FAILED"), bWritten ? TEXT("") : *Error);
		UE_LOG(LogMantleSeatGizmoShot, Display, TEXT("MANTLE_SEAT_GIZMO_SHOT done"));
	}

	FAutoConsoleCommand GMantleDevSeatGizmoShotCommand(
		TEXT("Mantle.Dev.SeatGizmoShot"),
		TEXT("Mantle.Dev.SeatGizmoShot <Directory> [size] -- draws a Mantle Seat's marker on a bench as the catalog thumbnail does"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&Run));
}
