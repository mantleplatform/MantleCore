// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/**
 * A mesh in the form the backend cooks it into.
 *
 * The client never parses glTF. Every load would otherwise repeat the same JSON parsing,
 * accessor walking, bounds checking and coordinate conversion — on the game thread, against
 * bytes a stranger uploaded. The cook worker does all of that once and writes this, so
 * loading here is a validated read of a fixed layout.
 *
 * Written by `backend/internal/cook/runtimemesh.go`. The two must agree, so the format is
 * versioned and a mismatch is refused rather than guessed at.
 */
struct FMantleRuntimeMesh
{
	/** Unreal centimetres, in Unreal's coordinate system. The worker already converted them. */
	TArray<FVector3f> Positions;

	/** Empty when the upload carried none. */
	TArray<FVector3f> Normals;
	TArray<FVector2f> UVs;

	/** Triangle list, already wound for Unreal's handedness. */
	TArray<uint32> Indices;

	FBox3f Bounds = FBox3f(ForceInit);

	int32 VertexCount() const { return Positions.Num(); }
	int32 TriangleCount() const { return Indices.Num() / 3; }
};

struct FMantleRuntimeMeshResult
{
	bool bSuccess = false;

	FMantleRuntimeMesh Mesh;

	/** What went wrong, phrased for whoever has to act on it. */
	FString Error;
};

/**
 * Reads the cooked mesh format.
 *
 * Strict on purpose, in the same way the place parser is: these bytes arrive over the
 * network, and a reader that trusts its header is a reader that can be handed a length
 * field pointing at somebody else's memory.
 */
class MANTLERUNTIME_API FMantleRuntimeMeshReader
{
public:
	/** The version this build understands. Must match RuntimeMeshVersion in the worker. */
	static constexpr uint32 SupportedVersion = 1;

	static FMantleRuntimeMeshResult Read(const TArray<uint8>& Data);
};
