// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleSkinnedMesh.h"

/** What a hat does to the wearer's hair (#389). */
enum class EMantleHairHatRule : uint8
{
	/** The hair that pokes through the hat is pulled in under it (and fully poking triangles are dropped). */
	UnderIt,
	/** The scalp-hair slot is dropped whole (facial hair never is: it gets the UnderIt test, drop-only). */
	HideAll,
	/** No test, no change. */
	LeaveAlone
};

/**
 * What the UnderIt rule aims at in scalp hair (#389). Facial hair ignores it: beards always get the poke test, drop only.
 * PokingThrough is the rule the game uses; UnderHatCards is the plan's first wording, kept so the two can be compared by eye
 * (a dev-only switch picks it, see FMantleCosmeticCache).
 */
enum class EMantleHairHatAim : uint8
{
	/** Rule a: the hair that pokes out through the hat is pulled in under it; a triangle goes only when all three of its corners are lost. */
	PokingThrough,
	/**
	 * Rule b: a point is under the hat when its ray from the head centre meets the hat (it lies inside the hat or pokes out
	 * through it). A card (welded, connected triangles) with more than CardHiddenShare of its points under the hat is hidden
	 * whole; every other card stays whole, its points under the hat pulled in to PullInCm under it (as rule a moves them).
	 * Hair the hat does not cover (below the brim, through an open top) is never moved.
	 */
	UnderHatCards
};

/**
 * Directions from one origin, sorted into a cube-map grid, so "the first triangle a ray from the origin meets" looks at a
 * few triangles instead of all of them. Every ray of a fit starts at the head centre, so the one grid serves them all.
 * Each triangle is listed in every cell its projection from the origin can touch (the bounding box of the projected,
 * clipped triangle, padded), so the first hit is always among a cell's triangles: FirstHit returns exactly what the
 * brute-force loop returns. Triangles are two-sided. Holds a copy of the triangle corners, so the source arrays can go.
 */
class MANTLERUNTIME_API FMantleHairHatGrid
{
public:
	/** Cells along one side of one of the six cube faces: 6 x 32 x 32 = 6144 cells. */
	static constexpr int32 CellsPerFaceSide = 32;

	/** Triangles with an index out of range are skipped. */
	void Build(const FVector3f& InOrigin, TConstArrayView<FVector3f> Positions, TConstArrayView<uint32> Indices);

	/** Distance along Direction (a unit vector) to the first triangle it meets, or -1 when it meets none. */
	float FirstHit(const FVector3f& Direction) const;

	/** The same answer by testing every triangle; the reference FirstHit is checked against. */
	static float FirstHitBruteForce(const FVector3f& Origin, const FVector3f& Direction,
		TConstArrayView<FVector3f> Positions, TConstArrayView<uint32> Indices);

	int32 NumTriangles() const { return Corners.Num() / 3; }

private:
	FVector3f Origin = FVector3f::ZeroVector;
	/** Three corners per triangle, as given (not relative to Origin). */
	TArray<FVector3f> Corners;
	/** CSR over the cells: the triangles of cell C are CellTriangles[CellStart[C]] up to CellStart[C + 1]. */
	TArray<int32> CellStart;
	TArray<int32> CellTriangles;
};

/** The fit's settings. */
struct FMantleHairHatFitParams
{
	EMantleHairHatRule Rule = EMantleHairHatRule::UnderIt;

	/** What UnderIt does to scalp hair; PokingThrough unless a dev comparison asks for UnderHatCards. */
	EMantleHairHatAim Aim = EMantleHairHatAim::PokingThrough;

	/** Beards: the poke test with drop only (nothing is pulled in), and HideAll never hides them whole. Eyebrows and lashes are
	 *  never fitted under a hat (the caller leaves them out). */
	bool bFacialHair = false;

	/** The point every ray starts from, in the hair's space (see FMantleHairHatFit::HeadCentreFromSkin). */
	FVector3f HeadCentre = FVector3f::ZeroVector;

	/** Drop what is left of a card that is almost all dropped (FMantleHairHatFit::SpeckAreaShare). Off unless specks show in testing. */
	bool bDropSpecks = false;

	/** Test every hat triangle for every ray instead of using the grid (for the tests). */
	bool bBruteForce = false;
};

struct FMantleHairHatFitResult
{
	/** The hair's positions after the pull-in: same count and order as the input's. */
	TArray<FVector3f> Positions;

	/** Indices (ascending) of the triangles to cut out of the hair's index list. */
	TArray<int32> DroppedTriangles;

	/** Points moved that a triangle which stays uses (the points of dropped triangles move too but are not counted). */
	int32 PointsPulled = 0;

	/** DroppedTriangles.Num(). */
	int32 TrianglesDropped = 0;

	/** How many of the dropped triangles are speck clean-up. */
	int32 TrianglesDroppedAsSpecks = 0;

	/** UnderHatCards: the cards hidden whole (their triangles are in DroppedTriangles). */
	int32 CardsHidden = 0;

	/** What the fit took, in milliseconds. */
	double Ms = 0.0;
};

/**
 * Hair under hats (#389), as a pure function: no actors, components or engine objects, so it runs on any thread.
 *
 * Hair inside an opaque hat is already unseen, so the job is the hair that pokes through. For each hair point a ray goes from
 * the head centre through the point and takes the first place it meets the hat; Judge says what to do with the answer.
 * The thresholds are named constants and Judge / CornerIsLost hold all the decisions, so a different rule is a small change.
 * FMantleHairHatFitParams::Aim picks what is dropped: triangles poking wholly through (the default) or whole cards mostly
 * under the hat (EMantleHairHatAim::UnderHatCards); the rays, Judge and the pull-in are the same for both.
 */
class MANTLERUNTIME_API FMantleHairHatFit
{
public:
	/** A point this much inside the hat's surface (or more) is tucked already and is left alone; closer, at or beyond the surface, it pokes. */
	static constexpr float PokeMarginCm = 0.5f;
	/** A poking point is pulled in to this far under the hat's surface, along its ray. */
	static constexpr float PullInCm = 0.5f;
	/** ...but not closer to the scalp than this, where there is room. */
	static constexpr float ScalpClearanceCm = 0.1f;
	/** "Room" means the hat's surface is at least this far above the scalp-plus-clearance. Tighter than that, the hat wins and the point goes under the skin. */
	static constexpr float MinUnderHatCm = 0.1f;
	/** A point beyond the hat's surface by more than this counts toward dropping its triangle (3 of 3 corners). */
	static constexpr float DropBeyondCm = 0.5f;
	/** Speck clean-up: a card with at least this share of its area dropped loses what is left... */
	static constexpr float SpeckAreaShare = 0.95f;
	/** ...unless the "card" holds more than this share of the hair's triangles (a solid hair shell is one giant card). */
	static constexpr float SpeckMaxCardShare = 0.10f;
	/** UnderHatCards: a card with more than this share of its points under the hat is hidden whole. */
	static constexpr float CardHiddenShare = 0.5f;
	/** The head centre is this far below the top of the head skin, ... */
	static constexpr float HeadCentreBelowTopCm = 7.0f;
	/** ...with X and Y the average of the skin points in this much of the top. */
	static constexpr float HeadCentreBandCm = 6.0f;
	/** Points closer to the head centre than this have no direction and are left alone. */
	static constexpr float MinRayLengthCm = 0.01f;

	/** What Judge says about one hair point. */
	struct FVertexVerdict
	{
		/** At or beyond the hat's surface, or within PokeMarginCm of it. False: no hat in the way or tucked already. */
		bool bPokes = false;
		/** The scalp leaves room for the pull-in (true too when the scalp is unknown). False: the hat wins. */
		bool bRoom = true;
		/** How far beyond the hat's surface the point is (negative: inside). Only meaningful when bPokes. */
		float BeyondCm = 0.0f;
		/** Where along the ray the point should end up (distance from the head centre); never more than where it is. Only meaningful when bPokes. */
		float TargetCm = 0.0f;
	};

	/**
	 * The one decision. PointCm is the point's distance from the head centre, HatCm the distance to the first hat hit on its ray
	 * (negative: none), ScalpCm the distance to the head skin on its ray (negative: unknown or none).
	 */
	static FVertexVerdict Judge(float PointCm, float HatCm, float ScalpCm);

	/** Whether a corner counts toward dropping its triangle (a triangle goes when all three corners are lost). */
	static bool CornerIsLost(const FVertexVerdict& Verdict, bool bFacialHair);

	/**
	 * Fits one LOD of hair (only its Positions, Indices and Sections are read) to a hat. Hat and scalp are triangle lists in
	 * the hair's space; the scalp (the wearer's head skin) is only read for the clearance, so it may be empty (then there is none).
	 * Positions come back moved, dropped triangles listed; the hair LOD itself is not changed (see ApplyDrops).
	 */
	static void Fit(const FMantleSkinnedMeshLOD& Hair, TConstArrayView<FVector3f> HatPositions, TConstArrayView<uint32> HatIndices,
		TConstArrayView<FVector3f> ScalpPositions, TConstArrayView<uint32> ScalpIndices,
		const FMantleHairHatFitParams& Params, FMantleHairHatFitResult& Out);

	/**
	 * Cuts the dropped triangles out of LOD's index list and fixes each section's FirstIndex and IndexCount. A section can end up
	 * with no triangles. Positions and every per-vertex array stay as they are.
	 */
	static void ApplyDrops(FMantleSkinnedMeshLOD& LOD, const TArray<int32>& DroppedTriangles);

	/**
	 * The head centre from the wearer's head skin: the highest skin point is the top; X and Y are the average of the points in
	 * the top HeadCentreBandCm, Z is HeadCentreBelowTopCm below the top. Only points a triangle uses count. Zero when there are none.
	 */
	static FVector3f HeadCentreFromSkin(TConstArrayView<FVector3f> Positions, TConstArrayView<uint32> Indices);
};
