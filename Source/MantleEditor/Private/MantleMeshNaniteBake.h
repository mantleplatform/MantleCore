// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class UStaticMesh;
namespace Nanite
{
	struct FResources;
}

/**
 * A place mesh's Nanite data as a side file, so the game can draw the creator's full-detail model where it would otherwise build
 * Nanite's reduced fallback from the uploaded GLB. The converter bakes one for every mesh it uploads (FMantlePlaceAssetConverter::
 * ConvertMesh, FMantleConvertedMesh::Nanite); the upload stage compresses it into the MNZ1 container the backend takes. The layout
 * is in D:\Mantle\Untracked\local\nanite\reports\step1-format.md.
 *
 * MND1, little-endian, FArchive conventions (an FString is int32 length then characters; an array is int32 count then items):
 *   "MND1", uint32 format version (1),
 *   FString engine tag (the editor's FEngineVersion with its changelist: the engine's Nanite layout is the format),
 *   FString the mesh tag: the hash of the mesh upload it was baked from ("sha256:<hex>", the place file's assets[].hash),
 *   int32 slot count, then per material slot of the baked mesh an int32: the glTF material it publishes as. Nanite's clusters name
 *     that slot (FMeshDataSection::MaterialIndex). The bake reorders its copy's slots to the upload's glTF materials and removes the
 *     sections the upload does not draw, so the table is always 0, 1, 2, ... (one entry per glTF material),
 *   the Nanite data, as Nanite::FResources::SerializeInternal writes it but with no streamed pages (every page is a root page,
 *     so the StreamablePages bulk data is left out, not written empty), then TangentPrecision, which the engine does not store.
 */
class FMantleMeshNaniteFile
{
public:
	static constexpr uint32 FormatVersion = 1;

	/** The engine version the file's layout belongs to (FEngineVersion::Current, changelist included). */
	static FString EngineTag();

	/**
	 * The file for MeshTag's mesh. False, with the reason, when Resources is empty or still streams (fewer root pages than pages:
	 * a shipped game streams those from a file installed with it, which a place mesh has not got).
	 */
	static bool Write(const FString& MeshTag, TConstArrayView<int32> SlotToPublished, const Nanite::FResources& Resources, TArray<uint8>& OutBytes,
		FString& OutReason);

	/** False, with the reason, when Bytes is not an MND1 for MeshTag's mesh from this engine, or is damaged. */
	static bool Read(TConstArrayView<uint8> Bytes, const FString& MeshTag, Nanite::FResources& OutResources, TArray<int32>& OutSlotToPublished,
		FString* OutReason = nullptr);

	/**
	 * The most one uploaded Nanite file may be, measured on the container (the compressed bytes): the backend's assets.MaxNaniteBytes,
	 * 64 MiB (build-plan contracts\nanite-entry.md). A mesh still over it is baked coarser (FMantleMeshNaniteBake::BakeToFit).
	 */
	static constexpr int64 MaxUploadBytes = 64ll * 1024 * 1024;

	/** The most an uploaded file may inflate to: the backend's cook.NaniteMaxRawBytes, the game's FMantleNaniteSideFile::MaxInflatedBytes. */
	static int64 MaxRawBytes();

	/**
	 * The container the backend takes: "MNZ1", a little-endian uint32 with Mnd1's length, then one zlib stream (RFC 1950, Unreal's
	 * NAME_Zlib at its default window) of Mnd1. False, with the reason, when Mnd1 is empty or over MaxRawBytes, or the stream cannot be made.
	 */
	static bool Pack(TConstArrayView<uint8> Mnd1, TArray<uint8>& OutContainer, FString& OutReason);

	/** The MND1 inside an uploaded file, through the game's own unpacker (FMantleNaniteSideFile::Unpack). False, with the reason, if it does not unpack. */
	static bool Unpack(TConstArrayView<uint8> Container, TArray<uint8>& OutMnd1, FString* OutReason = nullptr);

	/** Unpack, then Read: what the game does with an uploaded file. */
	static bool ReadUploaded(TConstArrayView<uint8> Container, const FString& MeshTag, Nanite::FResources& OutResources,
		TArray<int32>& OutSlotToPublished, FString* OutReason = nullptr);
};

/** How a mesh's Nanite file came to fit the upload limits: what FMantleMeshNaniteBake::BakeToFit did. */
struct FMantleNaniteFit
{
	/** The share of the mesh's triangles the file was finally baked at (1 = the mesh's own detail). */
	float KeepPercentTriangles = 1.0f;

	/** Bakes made (1 when the mesh fit as it was). */
	int32 Bakes = 0;

	/** The container's and the MND1's size at the first bake, before anything was made coarser. */
	int64 FirstPackedBytes = 0;
	int64 FirstRawBytes = 0;

	bool IsCoarser() const { return KeepPercentTriangles < 1.0f; }
};

/**
 * Builds the file from the mesh the converter was given (the creator's own, or one the converter made: mirrored, spline-baked or
 * cable-baked), which is only read: a transient copy of it is given the upload's material order (ConformToPublished), set to keep
 * its whole Nanite mesh in memory (TargetMinimumResidencyInKB = MAX_uint32, "Entire mesh"), built, and its Nanite data written.
 * A mesh the upload had to shrink (reduced to fit, or published from a lower LOD) bakes from its full source, so Nanite restores
 * the detail the upload lost.
 */
class FMantleMeshNaniteBake
{
public:
	/**
	 * PublishedSlots: the asset slot each glTF material of the upload draws (FMantleConvertedMesh::Slots). MeshTag: the upload's hash.
	 * A mesh made without Nanite gets it in the copy (engine defaults, Voxelize when WantsVoxels); a Nanite mesh keeps its own settings.
	 * False, with the reason (a phrase that finishes "so it draws as its plain model: ..."), for a mesh with a see-through material,
	 * a Nanite assembly whose upload leaves out a slot, or a build that kept no Nanite data. Game thread.
	 * OutVoxelMask, when given, gets the built data's VoxelMaterialsMask: nonzero only when voxels were really made, which needs voxels
	 * on at startup (the plugin's Config/Engine.ini sets r.Nanite.AllowVoxels=1).
	 * OutRetryable, when given, is set on a false return: true when the failure belongs to this run or this editor (a copy that could
	 * not be made, a build that kept no data, a part that did not load) and so may not repeat, false when it belongs to the mesh and
	 * its upload (see-through, a slot an assembly cannot drop, a hi-res source, no slot drawn). Only the latter may be cached.
	 */
	static bool Bake(const UStaticMesh* Mesh, TConstArrayView<int32> PublishedSlots, const FString& MeshTag, TArray<uint8>& OutFile, FString& OutReason,
		uint64* OutVoxelMask = nullptr, bool* OutRetryable = nullptr, float KeepPercentTriangles = 1.0f);

	/**
	 * Bake (at 1, then coarser as needed), pack into the uploaded container (FMantleMeshNaniteFile::Pack) and keep it inside the upload
	 * limits: the container at most FMantleMeshNaniteFile::MaxUploadBytes and its MND1 at most MaxRawBytes(). A file over either is baked
	 * again with the copy's FMeshNaniteSettings::KeepPercentTriangles lowered (NextKeepPercent) until it fits. OutFit says what it took.
	 * Nothing is refused for size: a mesh that still does not fit at MinKeepPercentTriangles, or after MaxBakes bakes, returns false with a
	 * reason that belongs to the mesh (not retryable): the game draws it plain and Check says so. OutContainer is the file to upload.
	 * Game thread. The other arguments are Bake's. MaxPackedBytes and MaxRawBytes are the limits, the upload's own unless a test asks for smaller.
	 */
	static bool BakeToFit(const UStaticMesh* Mesh, TConstArrayView<int32> PublishedSlots, const FString& MeshTag, TArray<uint8>& OutContainer,
		FString& OutReason, FMantleNaniteFit& OutFit, bool* OutRetryable = nullptr, int64 MaxPackedBytes = FMantleMeshNaniteFile::MaxUploadBytes,
		int64 MaxRawBytes = FMantleMeshNaniteFile::MaxRawBytes());

	/** The share of a mesh's triangles BakeToFit never goes below. */
	static constexpr float MinKeepPercentTriangles = 0.02f;

	/** The bakes BakeToFit makes at most, the first one included. */
	static constexpr int32 MaxBakes = 8;

	/**
	 * The share of its triangles to bake again at, after a bake at Keep came out PackedBytes (container) and RawBytes (MND1): Keep
	 * scaled by how far over the limits it was (the larger of PackedBytes / MaxUploadBytes and RawBytes / MaxRawBytes) with a tenth
	 * to spare, because a file does not shrink in step with its triangles. The step is never to less than a fifth of Keep nor to more
	 * than nine tenths of it, so every step moves. Keep itself when the file fits.
	 */
	static float NextKeepPercent(float Keep, int64 PackedBytes, int64 RawBytes, int64 MaxPackedBytes = FMantleMeshNaniteFile::MaxUploadBytes,
		int64 MaxRawBytes = FMantleMeshNaniteFile::MaxRawBytes());

	/**
	 * What the converter's cache key has to cover for Mesh: the mesh and, for a Nanite assembly, each part's mesh. An assembly names
	 * its parts by soft path, which the key does not follow (it follows hard references), so without them an edited part would
	 * leave the old Nanite file standing. A part that cannot be loaded is left out (the bake then fails and is not cached).
	 */
	static TArray<const UObject*> CacheObjects(const UStaticMesh& Mesh);

	/**
	 * Edits Copy (a transient copy, never the creator's mesh) so its material slot s is glTF material s of the upload: slot s gets
	 * the material of asset slot PublishedSlots[s], each LOD 0 section is pointed at its new slot, the sections the upload does not
	 * draw lose their triangles, and the other LODs go. False, with the reason, for a hi-res source model or
	 * no section the upload draws. A Nanite assembly goes to ConformAssembly. Not built: the caller builds. OutRetryable as in Bake.
	 */
	static bool ConformToPublished(UStaticMesh* Copy, TConstArrayView<int32> PublishedSlots, FString& OutReason, bool* OutRetryable = nullptr);

	/**
	 * ConformToPublished for a Nanite assembly: renumbers the copy's material list to the upload's glTF order, and with it the base
	 * mesh's sections and every part's MaterialRemap. False, with the reason, when the upload leaves out a slot (a part's sections
	 * cannot be cut, as a plain mesh's are) or a part cannot be loaded.
	 */
	static bool ConformAssembly(UStaticMesh* Copy, TConstArrayView<int32> PublishedSlots, FString& OutReason, bool* OutRetryable = nullptr);

	/**
	 * Whether a mesh made without Nanite is baked with Voxelize shape preservation (foliage): a slot the upload draws has a masked
	 * (cut-out) material seen from both sides, as leaf and needle cards are.
	 */
	static bool WantsVoxels(const UStaticMesh* Mesh, TConstArrayView<int32> PublishedSlots);

	/**
	 * What the converter's per-mesh cache key has to carry for the Nanite file beside the GLB: the file format, the engine build, and
	 * whether this editor was started with voxels and assemblies on (they change what the bake builds). The mesh itself, its Nanite
	 * settings and its materials are in the key already: the key hashes the mesh's package and what it references.
	 */
	static FString CacheSalt();
};
