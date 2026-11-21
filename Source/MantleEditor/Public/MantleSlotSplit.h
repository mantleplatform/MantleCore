// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class UStaticMesh;
class UStaticMeshComponent;

/**
 * A placed mesh that draws with more materials than a place mesh may (FMantlePlaceAssetLimits::MaxSections, 8) publishes as
 * several meshes at export, so it looks the same in game and nothing is refused. City Sample's freeway markings and traffic
 * lights are meshes of 9 to 17 materials, every one different, so merging materials can't fix them.
 *
 * The mesh is cut by material slot into ceil(slots / 8) transient parts of at most 8 slots each, as the mirror bake
 * (FMantleMirrorBake) makes its copies: a static mesh built from a mesh description, never written into the creator's Content, kept
 * alive by this class's own cache so that Gather, Check and Export all see the same objects (and the same mesh keys) for
 * as long as the source is unchanged. One source is cut once, however many actors and instances draw it.
 *
 * What a part keeps: every LOD the source's render data can be read for (and the LOD screen sizes), the triangles of its own
 * slots (the slots the source draws in LOD 0, in the order the upload lists them), every UV set and vertex colors, and the
 * source's lightmap setup. A part's slots are numbered from 0; SourceSlot says which slot of the source each one is, so a
 * component's material overrides still land on the right one.
 *
 * Collision stays what the source had. A source with simple shapes (boxes, spheres, capsules, hulls) gives them to part 0 only,
 * and the other parts publish with no collision (CarriesCollision). A source that collides with its triangles gives every
 * part its own triangles, which together are all of them.
 */
class MANTLEEDITOR_API FMantleSlotSplit
{
public:
	/** How many slots a part may have by default (FMantlePlaceAssetLimits::FromContract().MaxSections). */
	static int32 DefaultMaxSections();

	/**
	 * The parts Source publishes as when it draws more than MaxSections slots, in slot order; false with OutParts empty when it
	 * publishes as it is (it draws that many or fewer, it is a library mesh, or it is itself a part). False with OutWhy set when
	 * it has too many slots and the parts could not be made (no readable geometry); the caller then publishes it as it is, and
	 * the mesh check names the slot count as before. Cached by the source: a source that changed since (its geometry or its
	 * material slots) is cut again.
	 */
	static bool Split(const UStaticMesh* Source, int32 MaxSections, TArray<UStaticMesh*>& OutParts, FString& OutWhy);

	/**
	 * What a placed Mesh (a component's own mesh or a mirrored copy of it) publishes as: Split's parts, less the ones that
	 * publish nothing for this use. False when it publishes as it is: it has few enough slots, or Component simulates physics
	 * (a physics body is one piece, so its mesh is not cut and the mesh check refuses it as before). A hidden component
	 * (collision only) keeps only the parts that carry collision.
	 */
	static bool PartsFor(const UStaticMeshComponent* Component, const UStaticMesh* Mesh, TArray<UStaticMesh*>& OutParts);

	/** Whether Mesh is a part Split made. */
	static bool IsPart(const UStaticMesh* Mesh);

	/** The mesh Mesh was cut from, or Mesh itself when it is not a part. */
	static const UStaticMesh* WholeOf(const UStaticMesh* Mesh);

	/** The slot of the source that slot Slot of Mesh is; Slot itself when Mesh is not a part. */
	static int32 SourceSlot(const UStaticMesh* Mesh, int32 Slot);

	/** Whether Mesh publishes with its source's collision: every mesh but a part that is not part 0 of a source with simple collision shapes. */
	static bool CarriesCollision(const UStaticMesh* Mesh);

	/** Forgets every cached part (tests; a publish's end, FMantlePublishCaches::Prune). */
	static void ClearCache();
};
