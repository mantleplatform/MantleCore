// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

namespace Nanite
{
	struct FResources;
}

/**
 * The game's reader for a place mesh's Nanite file: the full-detail Nanite data the plugin bakes at publish (layout MND1 version 2: BP\contracts\nanite-v2.md,
 * written by the plugin's FMantleMeshNaniteFile) and uploads as an asset of kind "nanite", cut into pieces in an MNZ2 container (BP\contracts\nanite-v2.md,
 * "The container: MNZ2"). A mesh's manifest entry names it (FMantleAssetRef::Nanite); FMantlePlaceAssets downloads it only where it can be used, reads its head off the
 * game thread and the mesh builder attaches it. The streamed pages stay in the downloaded file on disk and are read from it one by one when the engine asks (FMantleNanitePageSource).
 *
 * Called the "side file" here, not the plugin's class name, because the editor target holds both modules.
 */

/** The container's table, read from the file's first bytes alone (ParseTable): where each piece of the file is and what it inflates to. Piece 0 is the head, pieces 1 to N-1 the streamed pages in page order. */
struct MANTLERUNTIME_API FMantleNaniteTable
{
	struct FPiece
	{
		/** The piece's zlib stream: its length and where it starts in the file. */
		uint32 Packed = 0;
		uint32 PackedStart = 0;
		/** What it inflates to, and (for a page) where it starts in the streamed block: the sum of the Raw of the pages before it. 0 for the head. */
		uint32 Raw = 0;
		uint32 RawStart = 0;
	};

	/** The file's size in bytes; the table's pieces fill it exactly. */
	int64 FileSize = 0;

	/** At least 1 piece. */
	TArray<FPiece> Pieces;

	/** The streamed block's length: the sum of the pages' Raw. 0 for a mesh with no streamed pages. */
	int64 StreamedBytes = 0;

	/** The number of streamed pages (N - 1). */
	int32 PageCount() const { return Pieces.Num() - 1; }

	/** The index (1 to N-1) of the page piece that is exactly the streamed block's bytes [Offset, Offset + Size), or INDEX_NONE. */
	int32 FindPage(uint64 Offset, uint64 Size) const;

	/** What the table holds in memory, in bytes. */
	int64 GetAllocatedBytes() const { return Pieces.GetAllocatedSize() + sizeof(FMantleNaniteTable); }
};

/** What the streamed pages of a version 2 file add up to (the pieces 1 to N-1). Size 0: the file has none. */
struct FMantleNaniteStreamedBlock
{
	int64 Size = 0;

	bool IsPresent() const { return Size > 0; }
};

class MANTLERUNTIME_API FMantleNaniteSideFile
{
public:
	/** The only MND1 version this reads: 2 (BP\contracts\nanite-v2.md). The head is the file up to the end of field 25; the streamed pages are the container's other pieces. */
	static constexpr uint32 FormatVersion = 2;

	/** The MNZ2 container's header: "MNZ2" and the uint32 piece count N; then 8 bytes (packed, raw) for each of the N pieces. */
	static constexpr int32 ContainerHeaderBytes = 8;
	static constexpr int32 ContainerEntryBytes = 8;

	/** The most any piece, and the whole MND1 the pieces make, may inflate to (the backend refuses a larger one too): a damaged size never allocates more. */
	static constexpr uint32 MaxInflatedBytes = 512u * 1024u * 1024u;

	/** The largest Nanite file (the backend's upload limit, assets.MaxNaniteBytes): the table keeps 32-bit offsets. */
	static constexpr int64 MaxFileBytes = 64ll * 1024 * 1024;

	/** The engine version the file's layout belongs to (FEngineVersion::Current, changelist included). */
	static FString EngineTag();

	/**
	 * Whether this process draws Nanite meshes: it renders, its RHI runs Nanite (shader model 6 and the 64-bit atomics Nanite
	 * needs) and the project has Nanite on. False on a dedicated server, with no RHI (tests, commandlets) and on a machine
	 * whose graphics cannot run Nanite; such a process never downloads a Nanite file. Game thread, after the RHI started.
	 */
	static bool CanRunNanite();

	/**
	 * Whether a mesh's Nanite entry (its baked-for engine tag) is worth downloading, decided from the manifest alone: this process
	 * can run Nanite (bCanRunNanite) and the file was baked by exactly this engine build (EngineTag). A file for another build is never
	 * fetched: the mesh draws plain until the place is republished.
	 */
	static bool ShouldFetch(bool bCanRunNanite, const FString& EntryEngine, const FString& ThisEngine);

	/**
	 * Reads the container's table from the first bytes of a file of FileSize bytes (TableBytes holds at least the 8 header bytes, and 8 + 8N when the count is known: see
	 * ReadTableSize) and makes the checks that need nothing inflated (the contract's 1 to 4): the magic is MNZ2, N >= 1, the table fits the file, the pieces fill the file exactly,
	 * every Packed >= 8 and Raw >= 1 (the head's >= 8), every Raw and the MND1 they make (head, 4, pages) at most MaxInflatedBytes, the file at most MaxFileBytes. False, with the
	 * reason, when any fails; nothing is allocated for a count or size that cannot fit. Any thread.
	 */
	static bool ParseTable(TConstArrayView<uint8> TableBytes, int64 FileSize, FMantleNaniteTable& OutTable, FString* OutReason = nullptr);

	/** The bytes the table needs (8 + 8N) from the first 8 bytes of a file (its magic and count); false when they are not an MNZ2's or the table cannot fit the file. */
	static bool ReadTableSize(TConstArrayView<uint8> FirstBytes, int64 FileSize, int64& OutTableBytes, FString* OutReason = nullptr);

	/**
	 * Opens the downloaded file at Path, reads and checks its table (ParseTable) and reads and inflates the head (piece 0) into OutHead: the only part of the file read at load.
	 * Nothing of the streamed pages is read. False, with the reason, when the file is missing, not an MNZ2, or its head does not inflate exactly. Any thread (reads the file).
	 */
	static bool ReadHead(const FString& Path, FMantleNaniteTable& OutTable, TArray<uint8>& OutHead, FString* OutReason = nullptr);

	/**
	 * Inflates one piece: Packed is exactly the piece's zlib stream and Out has room for Raw bytes. Uses zlib itself and requires the stream to end (Z_STREAM_END), to have
	 * used all of Packed (no byte after its Adler-32) and to have made exactly Raw bytes (FCompression::UncompressMemory does not look at what follows the stream). False, with the
	 * reason, otherwise; what was written to Out is then not to be used. Any thread.
	 */
	static bool InflatePiece(TConstArrayView<uint8> Packed, uint8* Out, uint32 Raw, FString* OutReason = nullptr);

	/**
	 * Fills OutResources (fields 6 to 25 of the layout, runtime state left at its defaults, the DDC flag cleared; StreamablePages untouched) and
	 * OutSlotToPublished (per asset material slot, the glTF material it publishes as, or -1) from the file's head (Head, inflated piece 0: it ends exactly after field 25). False, with the reason,
	 * when Head is not an MND1 version 2, from this engine, for MeshTag's mesh (a "sha256:<hex>" string), or is damaged; or when its page table does not agree with Table: the number of
	 * streamed pages is N - 1, each page i past the root ones is piece i - NumRootPages + 1 (Raw == BulkSize, RawStart == BulkOffset), and none is bigger than MaxPageBytes (a page
	 * the engine could never stage). Any thread.
	 */
	static bool Read(TConstArrayView<uint8> Head, const FString& MeshTag, const FMantleNaniteTable& Table, int64 MaxPageBytes, Nanite::FResources& OutResources,
		TArray<int32>& OutSlotToPublished, FString* OutReason = nullptr, FMantleNaniteStreamedBlock* OutStreamed = nullptr);

	/**
	 * The byte length of the file's root data (field 7), from the head alone: the Nanite data the engine registers in one frame when the mesh is attached. False when the header is not an MND1's. Any thread.
	 */
	static bool PeekRootBytes(TConstArrayView<uint8> Head, int64& OutRootBytes);

	/**
	 * Whether the Nanite data's material slots are the published mesh's own: the table is 0, 1, 2, ... for exactly PublishedSlotCount
	 * slots. Nanite's clusters name the asset slot, so any other table would draw a cluster with another section's material. A mesh
	 * whose file has another table builds without Nanite data. False, with the reason, otherwise.
	 */
	static bool SlotsMatchPublished(TConstArrayView<int32> SlotToPublished, int32 PublishedSlotCount, FString* OutReason = nullptr);
};
