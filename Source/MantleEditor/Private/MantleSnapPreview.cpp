// Copyright Joshua Eger. All Rights Reserved.
//
// Placing mode's snap sockets, previewed in the level editor (mantleplatform/mantle#184). Select two static mesh actors whose meshes have
// Snap_<kind> sockets and run Mantle.Placing.PreviewSnap: the one selected last moves so that its nearest compatible socket meets one of
// the other's, with the same snap helper the game uses (MantlePlacingSnap), so what shows here is what players get. Undo puts it back.

#include "CoreMinimal.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "HAL/IConsoleManager.h"
#include "MantlePlacingSnap.h"
#include "MantleSnapSockets.h"
#include "ScopedTransaction.h"
#include "Selection.h"

DEFINE_LOG_CATEGORY_STATIC(LogMantleSnapPreview, Log, All);

namespace MantleSnapPreviewPrivate
{
	TArray<MantlePlacingSnap::FSocket> SnapSocketsOf(const AStaticMeshActor& Actor)
	{
		TArray<MantlePlacingSnap::FSocket> Out;
		const UStaticMesh* Mesh = Actor.GetStaticMeshComponent() ? Actor.GetStaticMeshComponent()->GetStaticMesh() : nullptr;
		if (!Mesh)
		{
			return Out;
		}
		TArray<FMantlePlaceSocket> Sockets;
		TArray<FString> Problems;
		MantleSnapSockets::Read(*Mesh, Sockets, Problems);
		for (const FString& Problem : Problems)
		{
			UE_LOG(LogMantleSnapPreview, Warning, TEXT("%s: %s"), *Mesh->GetName(), *Problem);
		}
		for (const FMantlePlaceSocket& Socket : Sockets)
		{
			MantlePlacingSnap::FSocket& Added = Out.AddDefaulted_GetRef();
			Added.Name = FName(*Socket.Name);
			Added.Kind = FName(*Socket.Kind);
			for (const FString& Accepted : Socket.Accepts)
			{
				Added.Accepts.Add(FName(*Accepted));
			}
			Added.Position = Socket.Position;
			Added.Rotation = Socket.Rotation;
		}
		return Out;
	}

	void PreviewSnap()
	{
		TArray<AStaticMeshActor*> Selected;
		if (GEditor)
		{
			for (FSelectionIterator It(GEditor->GetSelectedActorIterator()); It; ++It)
			{
				if (AStaticMeshActor* Actor = Cast<AStaticMeshActor>(*It))
				{
					Selected.Add(Actor);
				}
			}
		}
		if (Selected.Num() != 2)
		{
			UE_LOG(LogMantleSnapPreview, Warning, TEXT("Select two static mesh actors with Snap_<kind> sockets (the one selected last moves onto the other), then run this again."));
			return;
		}
		AStaticMeshActor* Anchor = Selected[0];
		AStaticMeshActor* Moved = Selected[1];
		const TArray<MantlePlacingSnap::FSocket> AnchorSockets = SnapSocketsOf(*Anchor);
		const TArray<MantlePlacingSnap::FSocket> MovedSockets = SnapSocketsOf(*Moved);
		if (AnchorSockets.IsEmpty() || MovedSockets.IsEmpty())
		{
			UE_LOG(LogMantleSnapPreview, Warning, TEXT("%s has no Snap_<kind> sockets: add sockets named Snap_<kind> to the mesh in the Static Mesh Editor, and the kinds they join in the socket's Tag."),
				AnchorSockets.IsEmpty() ? *Anchor->GetActorLabel() : *Moved->GetActorLabel());
			return;
		}

		MantlePlacingSnap::FNeighbor Neighbor;
		Neighbor.Id = 0;
		Neighbor.Pose = Anchor->GetActorTransform();
		Neighbor.Sockets = &AnchorSockets;
		MantlePlacingSnap::FInput Input;
		Input.bHit = true;
		Input.HitPoint = Moved->GetActorLocation();
		Input.HitNormal = FVector::UpVector;
		Input.LocalBounds = FBox(FVector::ZeroVector, FVector::ZeroVector); // held where it is, not lifted onto a surface
		Input.Scale = Moved->GetActorScale3D();
		Input.Sockets = &MovedSockets;
		Input.RotationStepDegrees = 1.0;
		Input.RotationSteps = FMath::RoundToInt32(Moved->GetActorRotation().Yaw);
		Input.bSnap = true;
		Input.SocketRadius = 1.0e7; // join from anywhere: the point is to see the join
		TArray<MantlePlacingSnap::FNeighbor> Neighbors{ Neighbor };
		Input.Neighbors = Neighbors;
		const MantlePlacingSnap::FResult Result = MantlePlacingSnap::Compute(Input);
		if (Result.Mode != MantlePlacingSnap::EMode::Socket)
		{
			UE_LOG(LogMantleSnapPreview, Warning, TEXT("No socket of %s joins one of %s: a join needs one side's Tag to list the other's kind."),
				*Moved->GetActorLabel(), *Anchor->GetActorLabel());
			return;
		}
		const FScopedTransaction Transaction(NSLOCTEXT("MantleSnapPreview", "Preview", "Preview snap"));
		Moved->Modify();
		Moved->SetActorLocationAndRotation(Result.Location, Result.Rotation);
		UE_LOG(LogMantleSnapPreview, Log, TEXT("%s: socket %s joined %s of %s."), *Moved->GetActorLabel(), *Result.OwnSocket.ToString(),
			*Result.TargetSocket.ToString(), *Anchor->GetActorLabel());
	}

	FAutoConsoleCommand GPreviewSnapCommand(
		TEXT("Mantle.Placing.PreviewSnap"),
		TEXT("Select two static mesh actors with Snap_<kind> sockets; the one selected last moves so a compatible socket meets the other's, as placing mode would."),
		FConsoleCommandDelegate::CreateStatic(&PreviewSnap));
}
