// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/**
 * #51: finds the solid cap in a bought hair mesh and splits it from the cards, so the scalp fit (FMantleScalpFit) can wrap
 * the cap's open lower edge onto the head instead of leaving the nape open. The mesh is read as it comes: one position
 * array, one triangle list, and (optionally) the section each triangle belongs to.
 *
 * Cards are many small connected pieces; a cap is one (or a few) large connected pieces with an open edge. A piece is the cap when:
 *  - its surface area is at least CapAreaRatio times the median card's (SectionHintAreaRatio when its section is named
 *    like a cap: "cap", "scalp", "base", "skull"),
 *  - it has at least MinCapTriangles triangles, and an open boundary (a hair cap is never closed),
 *  - there are at least MinPieces pieces in the mesh (a mesh that is one piece has nothing to be split from).
 * More than MaxCapPieces such pieces is not a cap, it is a pack of big sheets: nothing is split. Ratios, not sizes,
 * so it works before the piece fit, in whatever unit the vendor used. Pure.
 */
struct MANTLEEDITOR_API FMantleHairCapSplit
{
	static constexpr double CapAreaRatio = 15.0;
	static constexpr double SectionHintAreaRatio = 4.0;
	static constexpr int32 MinCapTriangles = 100;
	static constexpr int32 MinPieces = 3;
	static constexpr int32 MaxCapPieces = 3;

	struct FResult
	{
		bool bFound = false;
		/** Triangle lists (three indices per triangle, into the input positions). Cards is everything that is not the cap. */
		TArray<uint32> CapIndices;
		TArray<uint32> CardIndices;
		int32 PieceCount = 0;
		int32 CapPieceCount = 0;
		/** The section the cap lies in, when it was given and the cap lies in one. */
		FString CapSection;
		/** One line for the preview's notes. */
		FString Describe() const;
	};

	/**
	 * Triangles is a triangle list. TriangleSections (one per triangle, may be empty) indexes SectionNames. Triangles
	 * that name a missing vertex make the result "not found" with every triangle left as cards.
	 */
	static FResult Split(TConstArrayView<FVector3f> Positions, TConstArrayView<uint32> Triangles,
		TConstArrayView<int32> TriangleSections, TConstArrayView<FString> SectionNames);

	/** True when a section name reads like a cap: "cap", "scalp", "base" or "skull" (case-insensitive). */
	static bool IsCapName(const FString& SectionName);
};
