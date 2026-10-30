// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Templates/UniquePtr.h"

class FCardRepresentationData;
class FDistanceFieldVolumeData;
struct FMantleContentHash;

/**
 * #149 step 5: a place mesh's Lumen data, the mesh distance field and the Lumen card representation the
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
 *   FString engine tag (FMantleMeshLumenFile::EngineTag: the engine's serialised layout is the format, so the
 *     tag of the editor that baked it must be the client's own),
 *   FString the mesh's cooked hash ("blake3:<hex>", the MRM2 it was baked from),
 *   the distance field: local bounds, bMostlyTwoSided, its finest mip (FSparseDistanceFieldMip) and that mip's
 *     bytes, which the client makes the resident mip (the renderer streams the others only from a file on disk,
 *     which a runtime mesh has not got),
 *   the card representation (FCardRepresentationData's own operator<<).
 * Anything else, a different engine or another mesh's file, reads as "not present": the mesh builds without.
 */
class MANTLERUNTIME_API FMantleMeshLumenFile
{
public:
	/** The engine version the file's layout belongs to (FEngineVersion::Current, changelist included). */
	static FString EngineTag();

	/** The file for MeshHash's mesh. Empty when the distance field is not valid (a mesh too thin or empty to have one). */
	static TArray<uint8> Write(const FMantleContentHash& MeshHash, const FDistanceFieldVolumeData& DistanceField, const FCardRepresentationData& Cards);

	/** False, with the reason, when Bytes is not an MLD1 for MeshHash from this engine. Any thread. */
	static bool Read(TConstArrayView<uint8> Bytes, const FMantleContentHash& MeshHash, FMantleMeshLumenData& OutData, FString* OutReason = nullptr);
};
