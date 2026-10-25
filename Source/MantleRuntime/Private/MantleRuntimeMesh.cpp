// Copyright Joshua Eger. All Rights Reserved.
#include "MantleRuntimeMesh.h"

#include "MantleRuntime.h"

namespace
{
	constexpr uint32 FlagHasNormals = 1 << 0;
	constexpr uint32 FlagHasUVs = 1 << 1;

	constexpr int32 HeaderBytes = 4 + 4 + 4 + 4 + 4 + 6 * 4;

	/**
	 * Sanity bounds on the header's counts.
	 *
	 * These mirror the worker's budgets, but they are not here to enforce a policy — the
	 * worker already did that. They are here because the counts decide how much memory this
	 * function allocates, and a header claiming four billion vertices must be refused before
	 * anything is reserved rather than after.
	 */
	constexpr int64 MaxVertices = 1'500'000;
	constexpr int64 MaxIndices = 500'000 * 3;

	/** Little-endian reads, matching the writer. Every platform the client targets is. */
	struct FCursor
	{
		const uint8* Data = nullptr;
		int64 Size = 0;
		int64 Offset = 0;

		uint32 ReadUint32()
		{
			const uint32 Value =
				 static_cast<uint32>(Data[Offset])
				| static_cast<uint32>(Data[Offset + 1]) << 8
				| static_cast<uint32>(Data[Offset + 2]) << 16
				| static_cast<uint32>(Data[Offset + 3]) << 24;
			Offset += 4;
			return Value;
		}

		float ReadFloat()
		{
			const uint32 Bits = ReadUint32();
			float Value;
			FMemory::Memcpy(&Value, &Bits, sizeof(Value));
			return Value;
		}
	};

	FMantleRuntimeMeshResult Fail(const FString& Error)
	{
		FMantleRuntimeMeshResult Result;
		Result.Error = Error;
		return Result;
	}
}

FMantleRuntimeMeshResult FMantleRuntimeMeshReader::Read(const TArray<uint8>& Data)
{
	if (Data.Num() < HeaderBytes)
	{
		return Fail(TEXT("That asset is too small to be a Mantle mesh."));
	}

	FCursor Cursor{Data.GetData(), Data.Num(), 0};

	if (FMemory::Memcmp(Data.GetData(), "MRM1", 4) != 0)
	{
		return Fail(TEXT("That asset is not a Mantle mesh."));
	}
	Cursor.Offset = 4;

	const uint32 Version = Cursor.ReadUint32();
	if (Version != SupportedVersion)
	{
		// Deliberately specific. This is the message a player sees when their client is
		// older than the asset, and "download the latest Mantle" is actionable where
		// "could not load asset" is not.
		return Fail(FString::Printf(
			TEXT("That asset was cooked for a newer version of Mantle (format %u, this build reads %u)."),
			Version, SupportedVersion));
	}

	const uint32 Flags = Cursor.ReadUint32();
	const int64 VertexCount = static_cast<int64>(Cursor.ReadUint32());
	const int64 IndexCount = static_cast<int64>(Cursor.ReadUint32());

	if (VertexCount <= 0 || VertexCount > MaxVertices)
	{
		return Fail(FString::Printf(TEXT("That asset claims %lld vertices, which is not a usable mesh."), VertexCount));
	}
	if (IndexCount <= 0 || IndexCount > MaxIndices || IndexCount % 3 != 0)
	{
		return Fail(FString::Printf(TEXT("That asset claims %lld indices, which cannot form triangles."), IndexCount));
	}

	const bool bHasNormals = (Flags & FlagHasNormals) != 0;
	const bool bHasUVs = (Flags & FlagHasUVs) != 0;

	// Everything the header promises is checked against the file's real length before a
	// single element is allocated or read.
	int64 Promised = HeaderBytes + VertexCount * 3 * 4 + IndexCount * 4;
	if (bHasNormals)
	{
		Promised += VertexCount * 3 * 4;
	}
	if (bHasUVs)
	{
		Promised += VertexCount * 2 * 4;
	}

	if (Data.Num() < Promised)
	{
		return Fail(FString::Printf(
			TEXT("That asset is incomplete: its header promises %lld bytes and the file holds %d."),
			Promised, Data.Num()));
	}

	FMantleRuntimeMeshResult Result;
	FMantleRuntimeMesh& Mesh = Result.Mesh;

	FVector3f Min, Max;
	Min.X = Cursor.ReadFloat();
	Min.Y = Cursor.ReadFloat();
	Min.Z = Cursor.ReadFloat();
	Max.X = Cursor.ReadFloat();
	Max.Y = Cursor.ReadFloat();
	Max.Z = Cursor.ReadFloat();
	Mesh.Bounds = FBox3f(Min, Max);

	Mesh.Positions.SetNumUninitialized(VertexCount);
	for (int64 Index = 0; Index < VertexCount; ++Index)
	{
		Mesh.Positions[Index].X = Cursor.ReadFloat();
		Mesh.Positions[Index].Y = Cursor.ReadFloat();
		Mesh.Positions[Index].Z = Cursor.ReadFloat();
	}

	if (bHasNormals)
	{
		Mesh.Normals.SetNumUninitialized(VertexCount);
		for (int64 Index = 0; Index < VertexCount; ++Index)
		{
			Mesh.Normals[Index].X = Cursor.ReadFloat();
			Mesh.Normals[Index].Y = Cursor.ReadFloat();
			Mesh.Normals[Index].Z = Cursor.ReadFloat();
		}
	}

	if (bHasUVs)
	{
		Mesh.UVs.SetNumUninitialized(VertexCount);
		for (int64 Index = 0; Index < VertexCount; ++Index)
		{
			Mesh.UVs[Index].X = Cursor.ReadFloat();
			Mesh.UVs[Index].Y = Cursor.ReadFloat();
		}
	}

	Mesh.Indices.SetNumUninitialized(IndexCount);
	for (int64 Index = 0; Index < IndexCount; ++Index)
	{
		const uint32 Vertex = Cursor.ReadUint32();

		// The worker checks this too. It is checked again because the check that matters is
		// the one on the machine that would crash, and these bytes arrived over a network.
		if (static_cast<int64>(Vertex) >= VertexCount)
		{
			return Fail(TEXT("That asset is corrupt: a triangle refers to a vertex that is not there."));
		}

		Mesh.Indices[Index] = Vertex;
	}

	Result.bSuccess = true;

	UE_LOG(LogMantle, Verbose, TEXT("Read a runtime mesh: %d vertices, %d triangles"),
		Mesh.VertexCount(), Mesh.TriangleCount());

	return Result;
}
