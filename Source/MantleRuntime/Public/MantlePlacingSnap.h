// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/**
 * #184 placing mode: the one snap helper. Pure maths with no actor or world access: it takes the piece, its sockets, where the camera
 * ray hit, the sockets of the pieces around, the grid and the rotation, and returns where the piece goes. The player's ghost and the
 * server's check both call it, so they agree; the server's answer is the one that counts.
 *
 * Three modes: socket snap (the nearest compatible free socket within SocketRadiusCm), grid snap (the piece's x and y on the grid),
 * free (still rotation-stepped). The piece rests on the surface the ray hit.
 */
namespace MantlePlacingSnap
{
	/** A socket joins one within this distance of it (cm) when the piece is held at the camera ray. */
	constexpr double SocketRadiusCm = 60.0;
	/** A socket with another piece's socket this close to it (cm) is taken. */
	constexpr double OccupiedRadiusCm = 5.0;

	enum class EMode : uint8
	{
		Free,
		Grid,
		Socket
	};

	/** A socket in its piece's own space. */
	struct FSocket
	{
		FName Name;
		FName Kind;
		TArray<FName> Accepts;
		FVector Position = FVector::ZeroVector;
		/** The facing is the forward (+X) axis. */
		FRotator Rotation = FRotator::ZeroRotator;
	};

	/** A piece already in the world. */
	struct FNeighbor
	{
		/** Stable and the same on both sides (the caller sorts by entity id); breaks ties. */
		int32 Id = 0;
		/** Its place, with the scale it was placed at. */
		FTransform Pose = FTransform::Identity;
		const TArray<FSocket>* Sockets = nullptr;
	};

	struct FInput
	{
		/** The camera ray's hit; with bHit false there is none and the piece is held at HitPoint. */
		bool bHit = false;
		FVector HitPoint = FVector::ZeroVector;
		FVector HitNormal = FVector::UpVector;
		/** The piece's bounds in its own space, scale applied; and the scale its sockets are placed at. */
		FBox LocalBounds = FBox(FVector(-50.0), FVector(50.0));
		FVector Scale = FVector::OneVector;
		const TArray<FSocket>* Sockets = nullptr;
		/** cm; 0 is no grid. */
		double Grid = 0.0;
		double RotationStepDegrees = 90.0;
		int32 RotationSteps = 0;
		bool bSnap = true;
		/** How close a socket must be held to another to join it (cm). The editor's preview widens it to join from anywhere. */
		double SocketRadius = SocketRadiusCm;
		/** Sorted by Id. */
		TConstArrayView<FNeighbor> Neighbors;
	};

	struct FResult
	{
		FVector Location = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
		EMode Mode = EMode::Free;
		/** Socket mode: the neighbor's Id and the two socket names. */
		int32 NeighborId = INDEX_NONE;
		FName OwnSocket;
		FName TargetSocket;
	};

	/** Whether the two sockets may join: either accepts the other's kind. */
	MANTLERUNTIME_API bool Compatible(const FSocket& A, const FSocket& B);

	MANTLERUNTIME_API FResult Compute(const FInput& Input);
}
