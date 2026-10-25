// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/**
 * The per-vertex hair card data a hair item carries (A2.2's MSK1 flag bit 4, the glTF attributes
 * _MANTLE_HAIR_ROOT_UV and _MANTLE_HAIR_COORD_U): CoordU, the position
 * along each card from its root (0) to its tip (1), and the card's root UV.
 *
 * CoordU is computed the way the engine's groom builder computes it for imported cards
 * (HairStrands' HairCardsBuilder.cpp, InternalCreateCardsGuides, UE 5.8): cards are the connected
 * pieces of the cards mesh; the atlas's main axis (U or V) is the one along which a card has more
 * distinct texture coordinates, voted over all cards; each card's vertices are grouped by their
 * coordinate along that axis (1/1024 apart), the groups sorted, and CoordU is the running length
 * through the groups' centres, normalised by the card's length. Ported rather than called, so the
 * plugin needs no HairStrands module dependency (the groom asset itself is read by reflection).
 *
 * Root UV: the engine takes it from the groom's strands (the closest strand root to each card
 * root), which needs the groom's built strand data. Until the plugin reads that (hook, see
 * FMantleCosmeticExport), every root UV is (0, 0): a valid stream the material treats as "no
 * per-card variation". Pure: no UObjects.
 */
struct MANTLEEDITOR_API FMantleHairCards
{
	/** Texture coordinates closer than this along the main axis are one group (the engine's 1 pixel of a 1K atlas). */
	static constexpr float UVThreshold = 1.0f / 1024.0f;

	/**
	 * CoordU per vertex for a cards mesh: Positions and atlas UVs per vertex, a triangle list.
	 * Vertices at the same position are one vertex for connectivity (render data splits them at
	 * UV or normal seams). OutCardCount, when given, gets the number of cards found.
	 */
	static TArray<float> ComputeCoordU(TConstArrayView<FVector3f> Positions, TConstArrayView<FVector2f> UVs,
		TConstArrayView<uint32> Indices, int32* OutCardCount = nullptr, bool* OutMainAxisIsU = nullptr);

	/** Triangle clusters that share an edge (by position): the cards. Each entry lists triangle indices. */
	static TArray<TArray<int32>> FindCards(TConstArrayView<FVector3f> Positions, TConstArrayView<uint32> Indices);

	/**
	 * Triangle numbers of complete cards within Budget. Never collapses a card or splits its alpha silhouette.
	 * The kept cards are spread evenly over the style (farthest-point pick over the cards' centres, a card that no longer
	 * fits is skipped for a smaller one), so a small budget gives thin but even hair, not patches.
	 */
	static TArray<int32> SelectWholeCards(TConstArrayView<FVector3f> Positions, TConstArrayView<uint32> Indices, int32 Budget);
	/** The smallest card's triangle count in a triangle list; 0 when it has none. */
	static int32 SmallestCardTriangles(TConstArrayView<FVector3f> Positions, TConstArrayView<uint32> Indices);

	/**
	 * Splits a triangle Budget over hair sections by their triangle share, but never below a section's smallest card
	 * (the section keeps at least one card; the other sections give up their share to pay for it). When even one smallest
	 * card per section does not fit, every section gets exactly its smallest card and bOverBudget is true.
	 */
	static TArray<int32> AllocateSectionBudgets(TConstArrayView<int32> SectionTriangles, TConstArrayView<int32> SectionSmallestCard, int32 Budget, bool& bOverBudget);

	/** Normalize Epic's atlas channel layouts to Mantle's coverage/depth/seed RGB contract. */
	static FColor AtlasPixel(int32 Layout, TConstArrayView<FColor> Textures, bool bPreserveNativeCoordU = false);
};
