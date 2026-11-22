// Copyright Joshua Eger. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MantlePlaceTypes.h"
#include "Templates/Function.h"

struct FMantlePlaceAssetLimits;
struct FMantlePlaceAssetSet;
class UStaticMesh;

/** The side files a static mesh not converted yet will get (FMantlePlaceAssetConverter::ConvertMesh's rules). */
struct FMantleExpectedSideFiles
{
	bool bLumen = true;
	bool bNanite = true;
};

/**
 * The root place file's asset list as it will be once every asset it names is converted and uploaded (MantleCore#189,
 * GhzGarage/mantle#818). Before conversion an export lists one entry per mesh and material with a provisional md5 hash; the place
 * file the backend gets lists, beside each, its textures and its meshes' Lumen and Nanite files, every one with a sha256 hash. City
 * Sample measured 5.4 MiB before upload and 9.1 MiB after: 15,439 entries became 32,135. This is the after shape, at its worst:
 *
 * - every entry an upload will replace (ProvisionalKeys) gets a sha256 hash and the largest size its kind may be;
 * - a static mesh not converted yet gets the Lumen and Nanite entries ExpectedSideFiles says it will have, and names them;
 * - a material not converted yet gets one texture entry for each of its document's texture slots (MantlePlaceTextureSlotCount),
 *   none shared with another material.
 *
 * Ids keep their length: a provisional id and an uploaded one are both ast_u and 32 hex digits.
 */
struct MANTLEEDITOR_API FMantlePlaceUploadShape
{
	TArray<FMantleAssetRef> Assets;

	/** The entries added for meshes and materials not converted yet. */
	int32 AddedTextures = 0;
	int32 AddedLumen = 0;
	int32 AddedNanite = 0;

	int32 NumAdded() const { return AddedTextures + AddedLumen + AddedNanite; }

	/**
	 * Assets: the export's list. ProvisionalKeys: FMantleExportResult::ProvisionalKeys (provisional id to upload key). Converted: the
	 * export's asset set, or null before conversion. ExpectedSideFiles: asked once for each static mesh not converted yet.
	 */
	static FMantlePlaceUploadShape Project(const TArray<FMantleAssetRef>& Assets, const TMap<FString, FString>& ProvisionalKeys,
		const FMantlePlaceAssetSet* Converted, const FMantlePlaceAssetLimits& Limits,
		TFunctionRef<FMantleExpectedSideFiles(const FMantleAssetRef& Mesh, const FString& Key)> ExpectedSideFiles);

	/**
	 * The side files ConvertMesh will make for Mesh, by its rules, without converting it: none for a mesh only ever used as collision; no
	 * Nanite file when a section it draws is see-through (FMantleMeshNaniteBake::BakeToFit); no Lumen file when every section it draws
	 * is (FMantleMeshLumenBake). Both otherwise, a mesh made without Nanite included: the converter builds it. A Lumen bake that finds
	 * no distance field is not foreseen, so this may count one Lumen file too many, never one too few. Null Mesh: both.
	 */
	static FMantleExpectedSideFiles ExpectedSideFiles(const UStaticMesh* Mesh, bool bCollisionOnly);

	/** The hash an upload gets: "sha256:" and 64 hex digits. */
	static FString UploadedHashShape();
};
