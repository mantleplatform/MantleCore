// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/**
 * #52: which route a hair upload takes, decided once from what the source is and how its mesh is built, and the plain words
 * the panel shows for it. Pure: the export fills the input, the panel shows the result. The routes already exist in the export
 * (grooms pick a cards LOD, skeletal hair picks an authored LOD, bought cards are sized to the head); this names them so the creator
 * is told which one their hair gets before they confirm it.
 */
struct MANTLEEDITOR_API FMantleHairRoute
{
	enum class EKind : uint8
	{
		/** A MetaHuman groom: its own cards, its atlas. */
		Groom,
		/** A skeletal mesh (MetaHuman hair, or one section lifted from a face). */
		SkeletalHair,
		/** A static cards mesh with a MetaHuman origin: already fitted to the head. */
		MetaHumanCards,
		/** Bought hair cards: sized to the standard head. */
		BoughtCards,
		/** Bought hair cards with more triangles than the budget: sized to the head, then thinned evenly. */
		DenseCards,
		/** One solid sculpted mesh with no cards. Not supported yet. */
		SculptedSolid,
	};

	/** A mesh whose largest connected piece holds at least this share of the triangles ... */
	static constexpr double SculptedLargestShare = 0.8;
	/** ... in at most this many pieces ... */
	static constexpr int32 SculptedMaxPieces = 3;
	/** ... and that has at least this many triangles, is solid hair, not a few cards. */
	static constexpr int32 SculptedMinTriangles = 1500;

	struct FInput
	{
		bool bGroom = false;
		bool bSkeletalMesh = false;
		/** A static mesh that FMantleHairSourceDiscovery ties to a MetaHuman groom, head or binding. */
		bool bMetaHumanOrigin = false;
		/** The creator chose to place the hair by hand: it is not sized to the head. */
		bool bPlacedByHand = false;
		/** The mesh as read, before the budget: its triangles, its connected pieces (cards) and the largest piece's triangles. */
		int32 Triangles = 0;
		int32 Pieces = 0;
		int32 LargestPieceTriangles = 0;
		/** The slot's triangle limit; 0 = none. */
		int32 Budget = 0;
	};

	struct FResult
	{
		EKind Kind = EKind::BoughtCards;
		/** False for a kind the plugin cannot make well yet (SculptedSolid). */
		bool bSupported = true;
		/** The hair is sized and seated on the standard head (the fit and its Looks good / Adjust). */
		bool bFitsToHead = false;
		/** Whole cards are dropped (evenly) to reach the budget. */
		bool bThinned = false;
		/** Percent of the triangles kept when thinned; 100 otherwise. */
		int32 KeptPercent = 100;
		/** Short name of the kind, then one or two plain sentences. */
		FString Title;
		FString Words;
	};

	static FResult Decide(const FInput& Input);
};
