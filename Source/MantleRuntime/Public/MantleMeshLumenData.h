// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Templates/UniquePtr.h"

class FCardRepresentationData;
class FDistanceFieldVolumeData;

/**
 * #149: a place mesh's Lumen data, the mesh distance field and the Lumen card representation the
 * editor's MeshUtilities bakes (GenerateSignedDistanceFieldVolumeData, GenerateCardRepresentationData).
 * A runtime-built mesh has neither, and a cooked client cannot make them (both builders are editor only),
 * so the plugin bakes them at publish from the cooked mesh it ships and the client attaches them to LOD0.
 */
struct MANTLERUNTIME_API FMantleMeshLumenData
{
	TUniquePtr<FDistanceFieldVolumeData> DistanceField;
	TUniquePtr<FCardRepresentationData> Cards;

	FMantleMeshLumenData();
	~FMantleMeshLumenData();
	FMantleMeshLumenData(FMantleMeshLumenData&&);
	FMantleMeshLumenData& operator=(FMantleMeshLumenData&&);

	/** What the two hold in memory (the distance field's streamed mips included). */
	int64 GetAllocatedBytes() const;
};

/**
 * MLD1, the optional side file of one cooked mesh:
 *   "MLD1", uint32 format version (1),
 *   FString engine tag (FMantleMeshLumenFile::EngineTag: the engine's serialized layout is the format, so the
 *     release of the editor that baked it must be the client's own, the changelist aside),
 *   FString the mesh tag: the hash of the mesh upload it was baked from ("sha256:<hex>", the place file's assets[].hash),
 *   the distance field: local bounds, bMostlyTwoSided, its finest mip (FSparseDistanceFieldMip) and that mip's
 *     bytes, which the client makes the resident mip (the renderer streams the others only from a file on disk,
 *     which a runtime mesh has not got),
 *   the card representation (FCardRepresentationData's own operator<<).
 * Anything else, a different engine or another mesh's file, reads as "not present": the mesh builds without.
 *
 * The backend stores it as MLD2 (#189 step 5): the MLD1 in a zlib envelope (MantleCookedPacking.h). Read and
 * ReadHeader take either; ReadHeader inflates only the header's bytes. An MLD1 is what the plugin uploads.
 */
class MANTLERUNTIME_API FMantleMeshLumenFile
{
public:
	/** The engine release the file's layout belongs to ("5.8.3", see MantleEngineTag: the changelist is not part of it). */
	static FString EngineTag();

	/** The file for the mesh MeshTag names. Empty when the distance field is not valid (a mesh too thin or empty to have one). */
	static TArray<uint8> Write(const FString& MeshTag, const FDistanceFieldVolumeData& DistanceField, const FCardRepresentationData& Cards);

	/** False, with the reason, when Bytes is not an MLD1 (or MLD2) for MeshTag's mesh from this engine. Any thread. */
	static bool Read(TConstArrayView<uint8> Bytes, const FString& MeshTag, FMantleMeshLumenData& OutData, FString* OutReason = nullptr);

	/**
	 * #189: an MLD2's MLD1, inflated into Out (any thread: a place inflates it in its decode task, so the mesh build on the game thread reads
	 * the MLD1 as it is). False, with the reason and Out empty, when Bytes is not an MLD2 that inflates whole to at most MaxBytes.
	 */
	static bool Unpack(TConstArrayView<uint8> Bytes, TArray<uint8>& Out, FString* OutReason = nullptr);

	/** True when Bytes start as an MLD2. */
	static bool IsPacked(TConstArrayView<uint8> Bytes);

	/** For tests: how many Reads succeeded in this process, and how many of those were handed an MLD2 and had to inflate it first. */
	static void GetReadCounts(int64& OutReads, int64& OutInflated);

	/**
	 * The header alone: the engine tag and the mesh tag. False when Bytes does not start as an MLD1 (or MLD2) of this format
	 * version. What a publish checks before it uploads, and what the place file's lumen entry repeats.
	 */
	static bool ReadHeader(TConstArrayView<uint8> Bytes, FString& OutEngine, FString& OutMeshTag);

	/** The most an MLD1 may be: what the API accepts for an upload of kind "lumen" (backend lumen.MaxBytes). */
	static constexpr int64 MaxBytes = 8LL * 1024 * 1024;
};
