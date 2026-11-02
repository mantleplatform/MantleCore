// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Templates/UniquePtr.h"

class FCardRepresentationData;
class FDistanceFieldVolumeData;

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
 *   FString engine tag (FMantleMeshLumenFile::EngineTag: the engine's serialized layout is the format, so the
 *     tag of the editor that baked it must be the client's own),
 *   FString the mesh tag: the hash of the mesh upload it was baked from ("sha256:<hex>", the place file's assets[].hash),
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

	/** The file for the mesh MeshTag names. Empty when the distance field is not valid (a mesh too thin or empty to have one). */
	static TArray<uint8> Write(const FString& MeshTag, const FDistanceFieldVolumeData& DistanceField, const FCardRepresentationData& Cards);

	/** False, with the reason, when Bytes is not an MLD1 for MeshTag's mesh from this engine. Any thread. */
	static bool Read(TConstArrayView<uint8> Bytes, const FString& MeshTag, FMantleMeshLumenData& OutData, FString* OutReason = nullptr);

	/**
	 * The header alone: the engine tag and the mesh tag. False when Bytes does not start as an MLD1 of this format
	 * version. What a publish checks before it uploads, and what the place file's lumen entry repeats.
	 */
	static bool ReadHeader(TConstArrayView<uint8> Bytes, FString& OutEngine, FString& OutMeshTag);

	/** The most an MLD1 may be: what the API accepts for an upload of kind "lumen" (backend lumen.MaxBytes). */
	static constexpr int64 MaxBytes = 8LL * 1024 * 1024;
};
