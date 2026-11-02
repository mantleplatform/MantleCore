// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlacingSnap.h"

namespace MantlePlacingSnapPrivate
{
	/** Half the box's size along n, turned by Rotation: how far its center sits from a surface it rests on. */
	double SupportHalfExtent(const FQuat& Rotation, const FVector& Half, const FVector& Normal)
	{
		return FMath::Abs(FVector::DotProduct(Rotation.GetAxisX(), Normal)) * Half.X
			+ FMath::Abs(FVector::DotProduct(Rotation.GetAxisY(), Normal)) * Half.Y
			+ FMath::Abs(FVector::DotProduct(Rotation.GetAxisZ(), Normal)) * Half.Z;
	}

	/** A facing is flat enough to turn the piece to meet it. */
	bool IsHorizontal(const FVector& Direction)
	{
		return FMath::Abs(Direction.Z) < 0.7;
	}

	/** Whether some other piece has a socket where this one is (so it is taken). */
	bool IsTaken(TConstArrayView<MantlePlacingSnap::FNeighbor> Neighbors, int32 Owner, const FVector& At)
	{
		for (const MantlePlacingSnap::FNeighbor& Other : Neighbors)
		{
			if (Other.Id == Owner || !Other.Sockets)
			{
				continue;
			}
			for (const MantlePlacingSnap::FSocket& Socket : *Other.Sockets)
			{
				if (FVector::DistSquared(Other.Pose.TransformPosition(Socket.Position), At) <= FMath::Square(MantlePlacingSnap::OccupiedRadiusCm))
				{
					return true;
				}
			}
		}
		return false;
	}
}

bool MantlePlacingSnap::Compatible(const FSocket& A, const FSocket& B)
{
	return A.Accepts.Contains(B.Kind) || B.Accepts.Contains(A.Kind);
}

MantlePlacingSnap::FResult MantlePlacingSnap::Compute(const FInput& Input)
{
	using namespace MantlePlacingSnapPrivate;
	FResult Result;
	const FRotator Turned(0.0, Input.RotationSteps * Input.RotationStepDegrees, 0.0);
	Result.Rotation = Turned.GetNormalized();
	if (!Input.bHit)
	{
		Result.Location = Input.HitPoint;
		return Result;
	}

	// Resting on the surface the ray hit: the box's center one support extent off it.
	const FQuat Rotation = Turned.Quaternion();
	const FVector Normal = Input.HitNormal.GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
	const double Support = SupportHalfExtent(Rotation, Input.LocalBounds.GetExtent(), Normal);
	FVector Location = Input.HitPoint + Normal * Support - Rotation.RotateVector(Input.LocalBounds.GetCenter());
	if (Input.Grid > 0.0)
	{
		Location.X = FMath::RoundToDouble(Location.X / Input.Grid) * Input.Grid;
		Location.Y = FMath::RoundToDouble(Location.Y / Input.Grid) * Input.Grid;
		Result.Mode = EMode::Grid;
	}
	Result.Location = Location;

	if (!Input.bSnap || !Input.Sockets || Input.Sockets->IsEmpty() || Input.Neighbors.IsEmpty())
	{
		return Result;
	}

	// Socket snap: the closest pair (own socket, a free socket of a piece around) that may join, within the radius, as the piece
	// is held now. Ties: the lower piece id, then the own socket, then the target socket, so both sides pick the same.
	double BestDistance = Input.SocketRadius;
	const FNeighbor* BestNeighbor = nullptr;
	int32 BestOwn = INDEX_NONE;
	int32 BestTarget = INDEX_NONE;
	for (int32 Own = 0; Own < Input.Sockets->Num(); ++Own)
	{
		const FSocket& Socket = (*Input.Sockets)[Own];
		const FVector Held = Location + Rotation.RotateVector(Socket.Position * Input.Scale);
		for (const FNeighbor& Neighbor : Input.Neighbors)
		{
			if (!Neighbor.Sockets)
			{
				continue;
			}
			for (int32 Target = 0; Target < Neighbor.Sockets->Num(); ++Target)
			{
				const FSocket& Candidate = (*Neighbor.Sockets)[Target];
				if (!Compatible(Socket, Candidate))
				{
					continue;
				}
				const FVector At = Neighbor.Pose.TransformPosition(Candidate.Position);
				const double Distance = FVector::Dist(Held, At);
				// Strictly closer wins; a tie keeps the first found, which is the lower piece id, own and target socket.
				if (Distance < BestDistance - UE_KINDA_SMALL_NUMBER || (!BestNeighbor && Distance <= BestDistance))
				{
					if (IsTaken(Input.Neighbors, Neighbor.Id, At))
					{
						continue;
					}
					BestDistance = Distance;
					BestNeighbor = &Neighbor;
					BestOwn = Own;
					BestTarget = Target;
				}
			}
		}
	}
	if (!BestNeighbor)
	{
		return Result;
	}

	const FSocket& Own = (*Input.Sockets)[BestOwn];
	const FSocket& Target = (*BestNeighbor->Sockets)[BestTarget];
	const FVector TargetPoint = BestNeighbor->Pose.TransformPosition(Target.Position);
	const FVector TargetFacing = BestNeighbor->Pose.GetRotation().RotateVector(Target.Rotation.Vector());
	const FVector OwnFacing = Own.Rotation.Vector();
	double Yaw = Turned.Yaw; // facing up or down: the player's rotation decides
	if (IsHorizontal(TargetFacing) && IsHorizontal(OwnFacing))
	{
		// Turn the piece so its socket faces the other way round from the one it meets.
		const double Wanted = FMath::RadiansToDegrees(FMath::Atan2(-TargetFacing.Y, -TargetFacing.X));
		const double Own2D = FMath::RadiansToDegrees(FMath::Atan2(OwnFacing.Y, OwnFacing.X));
		Yaw = Wanted - Own2D;
	}
	const FRotator Final = FRotator(0.0, Yaw, 0.0).GetNormalized();
	Result.Rotation = Final;
	Result.Location = TargetPoint - Final.Quaternion().RotateVector(Own.Position * Input.Scale);
	Result.Mode = EMode::Socket;
	Result.NeighborId = BestNeighbor->Id;
	Result.OwnSocket = Own.Name;
	Result.TargetSocket = Target.Name;
	return Result;
}
