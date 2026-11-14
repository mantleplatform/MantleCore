// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

namespace Nanite
{
	struct FResources;
}

/**
 * The game's reader for a place mesh's Nanite file: the full-detail Nanite data the plugin bakes at publish (layout MND1: D:\Mantle\Untracked\local\nanite\reports\step1-format.md,
 * written by the plugin's FMantleMeshNaniteFile) and uploads as an asset of kind "nanite", compressed in an MNZ1 container
 * (D:\Mantle\Untracked\local\build-plan\contracts\nanite-entry.md). A mesh's manifest entry names it (FMantleAssetRef::Nanite);
 * FMantlePlaceAssets downloads it only where it can be used, unpacks it off the game thread and the mesh builder attaches it.
 *
 * Called the "side file" here, not the plugin's class name, because the editor target holds both modules.
 */
class MANTLERUNTIME_API FMantleNaniteSideFile
{
public:
	static constexpr uint32 FormatVersion = 1;

	/** The MNZ1 container's header: "MNZ1" and the uint32 inflated size. */
	static constexpr int32 ContainerHeaderBytes = 8;

	/** The most an MNZ1 may inflate to (the backend refuses a larger one too): a damaged size word never allocates more. */
	static constexpr uint32 MaxInflatedBytes = 512u * 1024u * 1024u;

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
	 * Unpacks the uploaded file: "MNZ1", a uint32 inflated size, then one zlib stream of the MND1. False, with the reason, when the
	 * magic is not MNZ1, the size is 0 or over MaxInflatedBytes (refused before anything is allocated), the stream is damaged or
	 * does not inflate to exactly that size. Any thread.
	 */
	static bool Unpack(TConstArrayView<uint8> Container, TArray<uint8>& OutMnd1, FString* OutReason = nullptr);

	/**
	 * Fills OutResources (fields 6 to 25 of the layout: no streamed pages, runtime state left at its defaults) and
	 * OutSlotToPublished (per asset material slot, the glTF material it publishes as, or -1). False, with the reason, when Bytes is
	 * not an MND1 of this format version, from this engine, for MeshTag's mesh (a "sha256:<hex>" string), or is damaged. Any thread.
	 */
	static bool Read(TConstArrayView<uint8> Bytes, const FString& MeshTag, Nanite::FResources& OutResources, TArray<int32>& OutSlotToPublished,
		FString* OutReason = nullptr);

	/**
	 * Whether the Nanite data's material slots are the published mesh's own: the table is 0, 1, 2, ... for exactly PublishedSlotCount
	 * slots. Nanite's clusters name the asset slot, so any other table would draw a cluster with another section's material. A mesh
	 * whose file has another table builds without Nanite data. False, with the reason, otherwise.
	 */
	static bool SlotsMatchPublished(TConstArrayView<int32> SlotToPublished, int32 PublishedSlotCount, FString* OutReason = nullptr);
};
