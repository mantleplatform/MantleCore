// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleHairFitReport.h"
#include "MantleHairPieceFit.h"
#include "MantleHairScalpFit.h"

/**
 * #51: the whole upload-time head fit of bought hair cards, in one call: split the cap from the cards (FMantleHairCapSplit),
 * size and seat the piece on the head (FMantleHairPieceFit, stage 1), settle it on the scalp with the cap's lower edge
 * wrapped (FMantleScalpFit, stage 2), and word the result for the preview (FMantleHairFitReport). Positions only; topology,
 * UVs and normals are untouched. A failure leaves the positions as they were and says so in Warnings. Pure: no UObjects.
 */
struct MANTLEEDITOR_API FMantleHairCardsFit
{
	/** A cap's median distance to the skin, after the piece fit, must be at most this to count as a cap, cm. */
	static constexpr double CapMaxMedianDistanceCm = 3.0;
	/** ... and, after the piece fit, span at least this share of the scalp's width (X) and depth (Y). */
	static constexpr float CapMinScalpSpan = 0.5f;

	/**
	 * Bought hair imported from an OBJ keeps the file's Y-up axes, so it lies on its side. Before the piece fit, the fit
	 * tries the hair as imported and turned a quarter either way about X, and keeps a turn only when the roots seat on the
	 * scalp clearly better (OrientMinGain), so hair that was upright is never touched.
	 */
	static constexpr double OrientMinGain = 0.10;
	/** The turn's other effect: normals and tangents must turn with the positions (RotateUpright). */
	static FVector3f RotateUpright(const FVector3f& P, int32 QuarterTurns);

	/**
	 * Hair is left covering the face when it was made that way (a fringe, a lock over one eye), so only coverage the fit ADDED is
	 * undone (#106). The face is the head's skin facing forward (normal Y above 0.3) more than FaceClearTopCm below the top (the
	 * eyebrows and down); a vertex covers it when it is inside that skin or within FaceClearMarginCm in front of it. The hair
	 * as the whole-piece fit left it (the rigid size and seat, which keeps the hair's own design) is the baseline: a vertex that
	 * covers the face after the scalp fit but did not at the baseline goes back to its baseline position.
	 */
	static constexpr double FaceClearTopCm = 10.0;
	static constexpr double FaceClearMarginCm = 1.5;

	/** True when P covers the face of Head (see above). */
	static bool CoversFace(const FMantleHairHeadSamples& Head, const FVector& P);

	/** Puts the vertices (ids into the arrays) that Fitted newly pushed onto the face back to their Baseline position; returns how many went back. */
	static int32 ClearFaceAddedByTheFit(const FMantleHairHeadSamples& Head, TConstArrayView<int32> Vertices, TConstArrayView<FVector3f> Baseline, TArray<FVector3f>& Fitted);

	struct FInput
	{
		/** The head's skin triangles, in cm. */
		TConstArrayView<FVector3f> HeadPositions;
		TConstArrayView<uint32> HeadIndices;

		/** The hair mesh: one position array, a triangle list, each triangle's section and the section names. */
		TConstArrayView<FVector3f> HairPositions;
		TConstArrayView<uint32> HairIndices;
		TConstArrayView<int32> TriangleSections;
		TConstArrayView<FString> SectionNames;
	};

	struct FResult
	{
		/** The fitted positions (same count as the input); the input's when the fit did not run. */
		TArray<FVector3f> Positions;
		bool bFitted = false;
		bool bCapFound = false;
		/** 0 when the hair was upright; +1 or -1 when it was turned a quarter about X (RotateUpright): normals and tangents must turn the same way. */
		int32 QuarterTurns = 0;
		/** Hair vertices moved aside because they covered the face. */
		int32 FaceVerticesMoved = 0;
		FMantleHairFitReport::FResult Report;
		/** Lines for the package's notes and warnings the creator should read (a failure sentence each). */
		TArray<FString> Notes;
		TArray<FString> Warnings;
	};

	static FResult Run(const FInput& Input);

	/** The median distance, cm, of the vertices the triangles use from the head's skin; the cap guard's measure. */
	static double MedianDistanceToSkin(const FMantleScalpHeadSurface& Head, TConstArrayView<FVector3f> Positions, TConstArrayView<uint32> Triangles);

	/** True when the triangles' bounds span at least CapMinScalpSpan of the scalp's (the head within ScalpTopCm of its top) width and depth: the cap's shape guard. */
	static bool CoversScalp(TConstArrayView<FVector3f> HeadPositions, TConstArrayView<FVector3f> Positions, TConstArrayView<uint32> Triangles);
};
