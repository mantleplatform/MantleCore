// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAvatarRefitFixer.h"
#include "MantleCatalogGarmentFit.h"
#include "MantleNailSeating.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// Shaped nails: the seating step on a synthetic body (a flat sheet) and nails (small flat patches), and the
// slot rules that keep nails on the warp-only fit. No paid assets.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleNailSeatingTestPrivate
{
	/** A flat sheet in z = 0 from -10 to 10 cm, one cm cells, normals +Z, triangles counter-clockwise seen from +Z. */
	FMantleSkinnedMeshLOD NailTestSheet()
	{
		FMantleSkinnedMeshLOD Sheet;
		constexpr int32 Side = 21;
		for (int32 Y = 0; Y < Side; ++Y)
		{
			for (int32 X = 0; X < Side; ++X)
			{
				Sheet.Positions.Add(FVector3f(static_cast<float>(X - 10), static_cast<float>(Y - 10), 0.0f));
				Sheet.Normals.Add(FVector3f(0.0f, 0.0f, 1.0f));
				Sheet.UVs.Add(FVector2f(0.0f, 0.0f));
			}
		}
		for (int32 Y = 0; Y + 1 < Side; ++Y)
		{
			for (int32 X = 0; X + 1 < Side; ++X)
			{
				const uint32 A = static_cast<uint32>(Y * Side + X);
				const uint32 B = A + 1;
				const uint32 C = A + Side;
				const uint32 D = C + 1;
				Sheet.Indices.Append({ A, B, D, A, D, C });
			}
		}
		return Sheet;
	}

	/** A 3 x 3 vertex patch, 1 cm by 2 cm, tilted TiltDegrees about X, centred at Centre. */
	void NailTestPatch(const FVector3f& Centre, const float TiltDegrees, TArray<FVector3f>& Positions, TArray<uint32>& Indices)
	{
		const uint32 First = static_cast<uint32>(Positions.Num());
		const FQuat Tilt(FVector(1.0, 0.0, 0.0), FMath::DegreesToRadians(static_cast<double>(TiltDegrees)));
		for (int32 Y = 0; Y < 3; ++Y)
		{
			for (int32 X = 0; X < 3; ++X)
			{
				const FVector Local(0.5 * (X - 1), 1.0 * (Y - 1), 0.0);
				Positions.Add(Centre + FVector3f(Tilt.RotateVector(Local)));
			}
		}
		for (uint32 Y = 0; Y < 2; ++Y)
		{
			for (uint32 X = 0; X < 2; ++X)
			{
				const uint32 A = First + Y * 3 + X;
				Indices.Append({ A, A + 1, A + 4, A, A + 4, A + 3 });
			}
		}
	}

	/** The closest vertex of Positions[First, First + 9) over the sheet (z of the sheet is 0). */
	float NailTestLowest(const TArray<FVector3f>& Positions, const int32 First)
	{
		float Lowest = TNumericLimits<float>::Max();
		for (int32 Index = First; Index < First + 9; ++Index)
		{
			Lowest = FMath::Min(Lowest, Positions[Index].Z);
		}
		return Lowest;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleNailSeatingTest,
	"Mantle.Editor.NailSeating.SeatsSunkAndFloatingNails",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleNailSeatingTest::RunTest(const FString& Parameters)
{
	using namespace MantleNailSeatingTestPrivate;
	const FMantleSkinnedMeshLOD Sheet = NailTestSheet();

	TArray<FVector3f> Positions;
	TArray<uint32> Indices;
	NailTestPatch(FVector3f(-4.0f, 0.0f, -0.30f), 10.0f, Positions, Indices); // sunk and tilted
	NailTestPatch(FVector3f(4.0f, 0.0f, 1.0f), 0.0f, Positions, Indices);     // floating
	const TArray<FVector3f> Authored = Positions;

	TArray<int32> Island;
	TestEqual(TEXT("two separate patches are two islands"), FMantleNailSeating::FindIslands(Indices, Positions.Num(), Island), 2);
	TestEqual(TEXT("the first patch's vertices are island 0"), Island[0], 0);
	TestEqual(TEXT("the second patch's vertices are island 1"), Island[9], 1);

	TArray<FVector3f> FirstPatch;
	for (int32 Index = 0; Index < 9; ++Index)
	{
		FirstPatch.Add(Positions[Index]);
	}
	const FVector3f Thin = FMantleNailSeating::ThinAxis(FirstPatch);
	TestTrue(TEXT("a flat patch's thin axis is its normal, tilted 10 degrees from Z"), FMath::Abs(Thin.Z) > FMath::Cos(FMath::DegreesToRadians(10.5f)));

	FMantleNailSeatingResult Result;
	FString Error;
	if (!TestTrue(TEXT("the nails seat on the sheet"), FMantleNailSeating::Seat(Positions, Indices, Sheet, Result, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("both nails are seated"), Result.NumSeated, 2);
	TestTrue(TEXT("the sunk nail's closest vertex is 0.05 cm above the sheet"), FMath::IsNearlyEqual(NailTestLowest(Positions, 0), FMantleNailSeating::SeatCm, 0.01f));
	TestTrue(TEXT("the floating nail comes down to 0.05 cm"), FMath::IsNearlyEqual(NailTestLowest(Positions, 9), FMantleNailSeating::SeatCm, 0.01f));
	TestTrue(TEXT("the reported clearance is the seat"), FMath::IsNearlyEqual(Result.LowestClearanceCm, FMantleNailSeating::SeatCm, 0.01f)
		&& FMath::IsNearlyEqual(Result.HighestClearanceCm, FMantleNailSeating::SeatCm, 0.01f));

	// Rigid: every edge keeps its length, and the whole island moved by one vector.
	for (int32 Corner = 0; Corner < Indices.Num(); Corner += 3)
	{
		for (int32 Edge = 0; Edge < 3; ++Edge)
		{
			const int32 A = static_cast<int32>(Indices[Corner + Edge]);
			const int32 B = static_cast<int32>(Indices[Corner + (Edge + 1) % 3]);
			if (!FMath::IsNearlyEqual(FVector3f::Dist(Positions[A], Positions[B]), FVector3f::Dist(Authored[A], Authored[B]), 1.e-4f))
			{
				AddError(TEXT("a nail edge changed length: the seat is not rigid"));
				return false;
			}
		}
	}
	const FVector3f FirstMove = Positions[0] - Authored[0];
	for (int32 Index = 1; Index < 9; ++Index)
	{
		TestTrue(TEXT("a nail moves as one piece"), (Positions[Index] - Authored[Index]).Equals(FirstMove, 1.e-4f));
	}

	// Seated again, nothing moves.
	TArray<FVector3f> Again = Positions;
	FMantleNailSeatingResult Second;
	TestTrue(TEXT("seating again works"), FMantleNailSeating::Seat(Again, Indices, Sheet, Second, Error));
	TestTrue(TEXT("seating again moves nothing"), Second.MaxMoveCm < 0.005f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleNailSlotRulesTest,
	"Mantle.Editor.NailSeating.NailsStayOnTheWarpOnlyFitAndNeedNoSpine",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleNailSlotRulesTest::RunTest(const FString& Parameters)
{
	TestFalse(TEXT("nails do not track the body (they take the warp-only fit)"), FMantleCatalogGarmentFit::TracksBody(TEXT("nails")));
	TestTrue(TEXT("tops still do"), FMantleCatalogGarmentFit::TracksBody(TEXT("top")));
	TestTrue(TEXT("nails are an overlay slot"), FMantleAvatarRefit::IsOverlaySlot(TEXT("nails")));
	TestFalse(TEXT("tops are not"), FMantleAvatarRefit::IsOverlaySlot(TEXT("top")));
	TestFalse(TEXT("slot ids are case-sensitive"), FMantleAvatarRefit::IsOverlaySlot(TEXT("Nails")));

	// A hand-only skeleton: no spine, no neck, no head.
	const TArray<FName> BodyBones = { FName(TEXT("pelvis")), FName(TEXT("spine_01")), FName(TEXT("spine_02")), FName(TEXT("spine_03")),
		FName(TEXT("spine_04")), FName(TEXT("spine_05")), FName(TEXT("neck_01")), FName(TEXT("head")), FName(TEXT("hand_l")), FName(TEXT("index_03_l")) };
	const TArray<FName> HandBones = { FName(TEXT("hand_l")), FName(TEXT("index_03_l")) };
	const TArray<FName> Weighted = { FName(TEXT("index_03_l")) };
	FString Error;
	TestFalse(TEXT("a body piece without the spine is refused"), FMantleAvatarRefit::CheckBones(HandBones, Weighted, BodyBones, Error));
	TestTrue(TEXT("the refusal names the first missing bone"), Error.Contains(TEXT("pelvis")));
	TestTrue(TEXT("a nail piece without the spine is accepted"), FMantleAvatarRefit::CheckBones(HandBones, Weighted, BodyBones, Error, /*bOverlay*/ true));

	// Every weighted bone must still be on the MetaHuman skeleton.
	const TArray<FName> Stray = { FName(TEXT("index_03_l")), FName(TEXT("nail_helper_01")) };
	TestFalse(TEXT("a weighted bone the body lacks is refused for nails too"),
		FMantleAvatarRefit::CheckBones(Stray, Stray, BodyBones, Error, /*bOverlay*/ true));
	TestTrue(TEXT("the refusal names it"), Error.Contains(TEXT("nail_helper_01")));
	// The body still needs its own spine.
	const TArray<FName> ShortBody = { FName(TEXT("hand_l")), FName(TEXT("index_03_l")) };
	TestFalse(TEXT("the overlay rule skips the garment's spine, not the body's"),
		FMantleAvatarRefit::CheckBones(HandBones, Weighted, ShortBody, Error, /*bOverlay*/ true));

	// A garment's weighted bone the body lacks folds into its nearest ancestor the body has; one with none does not.
	const TArray<FName> ShirtBones = { FName(TEXT("pelvis")), FName(TEXT("spine_04")), FName(TEXT("breast_l")), FName(TEXT("breast_tip_l")),
		FName(TEXT("loose_helper")) };
	const TArray<int32> ShirtParents = { INDEX_NONE, 0, 1, 2, INDEX_NONE };
	const TArray<FName> ShirtWeighted = { FName(TEXT("spine_04")), FName(TEXT("breast_tip_l")), FName(TEXT("loose_helper")) };
	const TArray<FMantleFoldedBone> Folded = FMantleAvatarRefit::FoldBones(ShirtBones, ShirtParents, ShirtWeighted, BodyBones);
	TestEqual(TEXT("one bone folds"), Folded.Num(), 1);
	TestTrue(TEXT("past an unweighted extra, into the body's bone"),
		Folded.Num() == 1 && Folded[0].Bone == FName(TEXT("breast_tip_l")) && Folded[0].Into == FName(TEXT("spine_04")));

	// Nails follow their finger bones, however far the source arm was posed: the whole-piece move cap is not theirs.
	FMantleRefitPlan Plan;
	Plan.MaxMoveCm = FMantleAvatarRefit::MaxMoveCm + 20.0;
	TestTrue(TEXT("a body piece that moves too far is capped"), Plan.ExceedsMoveCap());
	Plan.bOnBones = true;
	TestFalse(TEXT("a nail carried on its finger bone is not"), Plan.ExceedsMoveCap());
	return true;
}

#endif
