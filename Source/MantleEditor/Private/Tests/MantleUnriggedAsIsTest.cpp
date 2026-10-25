// Copyright Joshua Eger. All Rights Reserved.
#include "MantleUnriggedAsIs.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// Chunk 7: As-is's auto-rig takes the fitted body's skin weights at each point's closest point, and the head band goes to the
// head. A synthetic cylinder, no assets.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleUnriggedAsIsTestPrivate
{
	constexpr int32 Pelvis = 0;
	constexpr int32 Spine = 1;
	constexpr int32 Head = 2;

	constexpr double Radius = 10.0;
	constexpr double Height = 180.0;
	constexpr double RingStep = 10.0;
	constexpr int32 Segments = 16;

	/**
	 * A 180 cm open cylinder about Z of radius 10, a ring every 10 cm, skinned to two bones: below 90 cm all Pelvis, above all
	 * Spine, the ring at 90 cm half each. The Head bone weighs nothing on it (the body's skin ends at the neck).
	 */
	FMantleAutoRigSkin Cylinder()
	{
		FMantleAutoRigSkin Skin;
		Skin.NumInfluences = 4;
		const int32 Rings = FMath::RoundToInt32(Height / RingStep) + 1;
		for (int32 Ring = 0; Ring < Rings; ++Ring)
		{
			const double Z = Ring * RingStep;
			for (int32 Segment = 0; Segment < Segments; ++Segment)
			{
				const double Angle = 2.0 * UE_DOUBLE_PI * Segment / Segments;
				const FVector3f Position(static_cast<float>(Radius * FMath::Cos(Angle)), static_cast<float>(Radius * FMath::Sin(Angle)), static_cast<float>(Z));
				TArray<TPair<int32, float>> Influences;
				if (Z < 90.0)
				{
					Influences.Emplace(Pelvis, 1.0f);
				}
				else if (Z > 90.0)
				{
					Influences.Emplace(Spine, 1.0f);
				}
				else
				{
					Influences.Emplace(Pelvis, 0.5f);
					Influences.Emplace(Spine, 0.5f);
				}
				Skin.AddVertex(Position, Influences);
			}
		}
		for (int32 Ring = 0; Ring + 1 < Rings; ++Ring)
		{
			for (int32 Segment = 0; Segment < Segments; ++Segment)
			{
				const int32 A = Ring * Segments + Segment;
				const int32 B = Ring * Segments + (Segment + 1) % Segments;
				const int32 C = A + Segments;
				const int32 D = B + Segments;
				Skin.Indices.Append({ A, B, D, A, D, C });
			}
		}
		return Skin;
	}

	/** Count points on a ring of radius R at height Z. */
	void AddRing(TArray<FVector3f>& Out, const double R, const double Z, const int32 Count)
	{
		for (int32 Index = 0; Index < Count; ++Index)
		{
			// Between the skin's vertices, so the closest point is inside a triangle, not on a corner.
			const double Angle = 2.0 * UE_DOUBLE_PI * (Index + 0.5) / Count;
			Out.Emplace(static_cast<float>(R * FMath::Cos(Angle)), static_cast<float>(R * FMath::Sin(Angle)), static_cast<float>(Z));
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleUnriggedAsIsTransferTest,
	"Mantle.Editor.UnriggedAsIs.TransferWeights",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleUnriggedAsIsTransferTest::RunTest(const FString& Parameters)
{
	using namespace MantleUnriggedAsIsTestPrivate;
	const FMantleAutoRigSkin Skin = Cylinder();
	TestEqual(TEXT("the cylinder has 19 rings of 16 vertices"), Skin.Positions.Num(), 19 * 16);
	TestEqual(TEXT("each vertex keeps its slots"), Skin.Bones.Num(), Skin.Positions.Num() * Skin.NumInfluences);

	// The model: a ring 2 cm outside the skin low down, one high up, one at the split, one inside the skin, and a head band above
	// the cut at 165 cm.
	constexpr int32 PerRing = 8;
	constexpr double HeadCutZ = 165.0;
	TArray<FVector3f> Points;
	AddRing(Points, Radius + 2.0, 35.0, PerRing);   // 0..7: pelvis
	AddRing(Points, Radius + 2.0, 145.0, PerRing);  // 8..15: spine
	AddRing(Points, Radius + 2.0, 90.0, PerRing);   // 16..23: half each
	AddRing(Points, Radius - 4.0, 45.0, PerRing);   // 24..31: inside, pelvis
	AddRing(Points, Radius + 3.0, 172.0, PerRing);  // 32..39: the head band
	Points.Emplace(0.0f, 0.0f, 200.0f);             // 40: the top of the head, above the skin's end
	Points.Emplace(40.0f, 0.0f, 120.0f);            // 41: a cape's edge, 30 cm out

	FMantleAutoRigWeights Weights;
	FString Error;
	if (!TestTrue(TEXT("the weights transfer"), FMantleUnriggedAsIs::TransferWeights(Points, Skin, Head, HeadCutZ, Weights, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("every point has its slots"), Weights.Bones.Num(), Points.Num() * FMantleUnriggedAsIs::MaxInfluences);
	TestEqual(TEXT("the head band and the top of the head go to the head"), Weights.HeadPoints, PerRing + 1);
	TestEqual(TEXT("the rest are weighted from the skin"), Weights.SkinPoints, Points.Num() - PerRing - 1);
	TestEqual(TEXT("only the cape's edge is far from the skin"), Weights.FarPoints, 1);
	TestTrue(TEXT("the cape's edge is 30 cm out"), FMath::IsNearlyEqual(Weights.MaxDistanceCm, 30.0, 0.5));

	for (int32 Point = 0; Point < Points.Num(); ++Point)
	{
		float Sum = 0.0f;
		for (int32 Slot = 0; Slot < Weights.NumInfluences; ++Slot)
		{
			Sum += Weights.Weights[Point * Weights.NumInfluences + Slot];
		}
		TestTrue(FString::Printf(TEXT("point %d's weights sum to 1 (%.4f)"), Point, Sum), FMath::IsNearlyEqual(Sum, 1.0f, 1e-4f));
	}
	for (int32 Point = 0; Point < PerRing; ++Point)
	{
		TestTrue(FString::Printf(TEXT("low point %d is all pelvis"), Point), FMath::IsNearlyEqual(Weights.WeightOf(Point, Pelvis), 1.0f, 1e-4f));
		TestTrue(FString::Printf(TEXT("high point %d is all spine"), Point), FMath::IsNearlyEqual(Weights.WeightOf(PerRing + Point, Spine), 1.0f, 1e-4f));
		const float Split = Weights.WeightOf(2 * PerRing + Point, Pelvis);
		TestTrue(FString::Printf(TEXT("split point %d is half pelvis (%.3f)"), Point, Split), FMath::IsNearlyEqual(Split, 0.5f, 1e-3f));
		TestEqual(FString::Printf(TEXT("inside point %d follows the pelvis"), Point), Weights.Dominant(3 * PerRing + Point), Pelvis);
		TestTrue(FString::Printf(TEXT("head band point %d is all head"), Point), FMath::IsNearlyEqual(Weights.WeightOf(4 * PerRing + Point, Head), 1.0f, 1e-6f));
		TestEqual(FString::Printf(TEXT("head band point %d takes nothing from the spine"), Point), Weights.WeightOf(4 * PerRing + Point, Spine), 0.0f);
	}
	TestTrue(TEXT("the top of the head is all head"), FMath::IsNearlyEqual(Weights.WeightOf(5 * PerRing, Head), 1.0f, 1e-6f));
	TestEqual(TEXT("the cape's edge follows its nearest bone, the spine"), Weights.Dominant(5 * PerRing + 1), Spine);

	// No head bone: nothing is cut, the head band takes the skin's top (the spine).
	FMantleAutoRigWeights NoCut;
	TestTrue(TEXT("the weights transfer without a head bone"), FMantleUnriggedAsIs::TransferWeights(Points, Skin, INDEX_NONE, HeadCutZ, NoCut, Error));
	TestEqual(TEXT("without a head bone nothing goes to the head"), NoCut.HeadPoints, 0);
	TestEqual(TEXT("without a head bone the head band follows the spine"), NoCut.Dominant(4 * PerRing), Spine);

	// No skin: refused with why.
	FMantleAutoRigSkin Empty;
	Empty.NumInfluences = 4;
	FMantleAutoRigWeights None;
	FString EmptyError;
	TestFalse(TEXT("no skin is refused"), FMantleUnriggedAsIs::TransferWeights(Points, Empty, Head, HeadCutZ, None, EmptyError));
	TestFalse(TEXT("and says why"), EmptyError.IsEmpty());

	// AddVertex keeps the heaviest slots.
	FMantleAutoRigSkin Two;
	Two.NumInfluences = 2;
	Two.AddVertex(FVector3f::ZeroVector, { TPair<int32, float>(5, 0.1f), TPair<int32, float>(6, 0.6f), TPair<int32, float>(7, 0.3f) });
	TestEqual(TEXT("the heaviest slot first"), Two.Bones[0], 6);
	TestEqual(TEXT("then the next"), Two.Bones[1], 7);
	return true;
}


// A finger's skin follows one finger: a point between two fingers, weighted half to each by the closest-point transfer, keeps
// only its nearest finger; a point on one finger, and one on the hand alone, keep what they have.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleUnriggedAsIsFingerTest,
	"Mantle.Editor.UnriggedAsIs.KeepOneFinger",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleUnriggedAsIsFingerTest::RunTest(const FString& Parameters)
{
	// lowerarm_l, hand_l, two fingers of two bones each along +Y (index at X 0, middle at X 3), and a twist bone under the hand
	// that is no finger.
	const TArray<FName> Names = { TEXT("lowerarm_l"), TEXT("hand_l"), TEXT("index_01_l"), TEXT("index_02_l"), TEXT("middle_01_l"), TEXT("middle_02_l"),
		TEXT("hand_twist_l") };
	const TArray<int32> Parents = { INDEX_NONE, 0, 1, 2, 1, 4, 1 };
	const TArray<FVector3f> Joints = { FVector3f(0, -20, 0), FVector3f(0, 0, 0), FVector3f(0, 8, 0), FVector3f(0, 12, 0), FVector3f(3, 8, 0),
		FVector3f(3, 12, 0), FVector3f(1, -2, 0) };
	constexpr int32 Hand = 1;
	constexpr int32 Index2 = 3;
	constexpr int32 Middle2 = 5;

	FMantleAutoRigWeights Weights;
	Weights.NumInfluences = 4;
	auto AddPoint = [&Weights](TConstArrayView<TPair<int32, float>> Influences)
	{
		for (int32 Slot = 0; Slot < Weights.NumInfluences; ++Slot)
		{
			Weights.Bones.Add(Influences.IsValidIndex(Slot) ? Influences[Slot].Key : 0);
			Weights.Weights.Add(Influences.IsValidIndex(Slot) ? Influences[Slot].Value : 0.0f);
		}
	};
	const TArray<FVector3f> Points = {
		FVector3f(1.0f, 11.0f, 0.0f),   // 0: between the fingers, nearer the index: half each
		FVector3f(2.4f, 13.0f, 0.0f),   // 1: between them, nearer the middle, past its tip: half each and some hand
		FVector3f(0.2f, 10.0f, 0.0f),   // 2: on the index: all index
		FVector3f(0.5f, -1.0f, 0.0f),   // 3: on the palm: hand and twist, no finger
		FVector3f(1.6f, 10.0f, 0.0f),   // 4: nearer the middle, but weighted all to the index
	};
	AddPoint({ TPair<int32, float>(Index2, 0.5f), TPair<int32, float>(Middle2, 0.5f) });
	AddPoint({ TPair<int32, float>(Index2, 0.4f), TPair<int32, float>(Middle2, 0.4f), TPair<int32, float>(Hand, 0.2f) });
	AddPoint({ TPair<int32, float>(Index2, 1.0f) });
	AddPoint({ TPair<int32, float>(Hand, 0.7f), TPair<int32, float>(6, 0.3f) });
	AddPoint({ TPair<int32, float>(Index2, 1.0f) });

	const FMantleFingerFix Fix = FMantleUnriggedAsIs::KeepOneFinger(Points, Names, Parents, Joints, Weights);
	TestEqual(TEXT("four points are on a finger"), Fix.FingerPoints, 4);
	TestEqual(TEXT("two of them reach both fingers"), Fix.MixedPoints, 2);
	TestEqual(TEXT("three are kept to their nearest finger"), Fix.Changed, 3);
	TestTrue(TEXT("the point nearer the index is all index"), FMath::IsNearlyEqual(Weights.WeightOf(0, Index2), 1.0f, 1e-5f));
	TestEqual(TEXT("and nothing on the middle"), Weights.WeightOf(0, Middle2), 0.0f);
	TestTrue(TEXT("the point nearer the middle keeps the middle and the hand"), FMath::IsNearlyEqual(Weights.WeightOf(1, Middle2), 2.0f / 3.0f, 1e-5f)
		&& FMath::IsNearlyEqual(Weights.WeightOf(1, Hand), 1.0f / 3.0f, 1e-5f));
	TestEqual(TEXT("and nothing on the index"), Weights.WeightOf(1, Index2), 0.0f);
	TestTrue(TEXT("the point on the index keeps it"), FMath::IsNearlyEqual(Weights.WeightOf(2, Index2), 1.0f, 1e-5f));
	TestTrue(TEXT("the palm keeps its hand and twist"), FMath::IsNearlyEqual(Weights.WeightOf(3, Hand), 0.7f, 1e-5f)
		&& FMath::IsNearlyEqual(Weights.WeightOf(3, 6), 0.3f, 1e-5f));
	TestTrue(TEXT("a point on the wrong finger alone goes to its nearest bone"), FMath::IsNearlyEqual(Weights.WeightOf(4, Middle2), 1.0f, 1e-5f));

	TestEqual(TEXT("fingers 1 cm or so from their bones keep bending"), Fix.RigidHands.Num(), 0);

	// The same fingers 10 cm off their bones (a model's hand the fitted hand doesn't line up with) follow the hand rigidly.
	{
		FMantleAutoRigWeights Far;
		Far.NumInfluences = 4;
		TArray<FVector3f> FarPoints;
		for (const FVector3f& Point : Points)
		{
			FarPoints.Add(Point + FVector3f(0.0f, 0.0f, 10.0f));
		}
		for (int32 Point = 0; Point < Points.Num(); ++Point)
		{
			const TPair<int32, float> Influences[] = { TPair<int32, float>(Index2, 0.75f), TPair<int32, float>(Hand, 0.25f) };
			for (int32 Slot = 0; Slot < Far.NumInfluences; ++Slot)
			{
				Far.Bones.Add(Slot < 2 ? Influences[Slot].Key : 0);
				Far.Weights.Add(Slot < 2 ? Influences[Slot].Value : 0.0f);
			}
		}
		const FMantleFingerFix FarFix = FMantleUnriggedAsIs::KeepOneFinger(FarPoints, Names, Parents, Joints, Far);
		TestEqual(TEXT("the far hand's fingers follow it rigidly"), FarFix.RigidHands.Num(), 1);
		TestTrue(TEXT("a far finger point is all on the hand"), FMath::IsNearlyEqual(Far.WeightOf(2, Hand), 1.0f, 1e-5f));
		TestEqual(TEXT("and nothing on its finger"), Far.WeightOf(2, Index2), 0.0f);
	}

	// No hand in the skeleton: nothing changes.
	FMantleAutoRigWeights Same = Weights;
	const TArray<FName> NoHand = { TEXT("root"), TEXT("pelvis"), TEXT("index_01_l"), TEXT("index_02_l"), TEXT("middle_01_l"), TEXT("middle_02_l"), TEXT("spine") };
	const FMantleFingerFix None = FMantleUnriggedAsIs::KeepOneFinger(Points, NoHand, Parents, Joints, Same);
	TestEqual(TEXT("without a hand there are no fingers"), None.FingerPoints, 0);
	TestTrue(TEXT("and no weight changes"), Same.Weights == Weights.Weights);
	return true;
}

// The hand fit: the conform's body holds its hand away from the model's, so the body hand (its skin, its joints) is laid onto the
// model's by a rigid fit, then its skin moved with it by its weights. A synthetic hand, no assets.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleUnriggedAsIsHandFitTest,
	"Mantle.Editor.UnriggedAsIs.HandFit",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleUnriggedAsIsHandFitTest::RunTest(const FString& Parameters)
{
	// A hand as surface samples: a palm, three fingers and a thumb; the model's hand sampled apart from the body's, as two meshes are.
	auto Boxes = [](const int32 Seed, const int32 PerPalm)
	{
		FRandomStream Random(Seed);
		struct FPart { FVector3f Centre; FVector3f Size; int32 Count; };
		const FPart Parts[] = { { FVector3f(4, 4, 1), FVector3f(8, 9, 2.5f), PerPalm }, { FVector3f(1, 13, 1), FVector3f(1.8f, 9, 1.8f), PerPalm / 3 },
			{ FVector3f(3.5f, 13, 1), FVector3f(1.8f, 10, 1.8f), PerPalm / 3 }, { FVector3f(6, 12.5f, 1), FVector3f(1.8f, 8, 1.8f), PerPalm / 3 },
			{ FVector3f(10, 2, 1), FVector3f(5, 2, 2), PerPalm / 4 } };
		TArray<FVector3f> Out;
		for (const FPart& Part : Parts)
		{
			for (int32 Index = 0; Index < Part.Count; ++Index)
			{
				FVector3f Unit(Random.FRandRange(-0.5f, 0.5f), Random.FRandRange(-0.5f, 0.5f), Random.FRandRange(-0.5f, 0.5f));
				Unit[Random.RandRange(0, 2)] = Random.FRand() < 0.5f ? -0.5f : 0.5f;
				Out.Add(Part.Centre + Unit * Part.Size);
			}
		}
		return Out;
	};
	const TArray<FVector3f> Hand = Boxes(1, 600);
	const TArray<FVector3f> ModelHand = Boxes(2, 800);
	// The model's hand: turned 30 degrees about a slanted axis and moved 12 cm, its forearm beside it in the targets.
	const FTransform Truth(FQuat(FVector(0.3, 0.2, 1.0).GetSafeNormal(), FMath::DegreesToRadians(30.0)), FVector(3.0, -11.0, 4.0));
	TArray<FVector3f> Model;
	FVector3f SourceCentre = FVector3f::ZeroVector;
	FVector3f ModelCentre = FVector3f::ZeroVector;
	for (const FVector3f& Point : Hand)
	{
		SourceCentre += Point;
	}
	for (const FVector3f& Point : ModelHand)
	{
		Model.Add(FVector3f(Truth.TransformPosition(FVector(Point))));
		ModelCentre += Model.Last();
	}
	SourceCentre /= Hand.Num();
	ModelCentre /= ModelHand.Num();
	{
		FRandomStream Random(3);
		for (int32 Index = 0; Index < 500; ++Index)
		{
			const FVector Forearm(Random.FRandRange(0.5f, 7.5f), Random.FRandRange(-20.0f, -4.0f), Random.FRand() < 0.5f ? -1.5f : 3.5f);
			Model.Add(FVector3f(Truth.TransformPosition(Forearm)));
		}
	}

	double StartCm = 0.0;
	double FitCm = 0.0;
	// From the two hands' centres, a little off (the model's points that first take the hand's weights are only its near side).
	const FTransform Start(FVector(ModelCentre - SourceCentre) + FVector(2.0, 1.0, -2.0));
	const FTransform Fit = FMantleUnriggedAsIs::FitRigid(Hand, Model, Start, FMantleUnriggedAsIs::HandFitRounds, StartCm, FitCm);
	TestTrue(FString::Printf(TEXT("the fit lays the hand on (%.3f cm from %.3f cm)"), FitCm, StartCm), FitCm < 0.4 && FitCm < StartCm);
	double Worst = 0.0;
	for (const FVector3f& Point : Hand)
	{
		Worst = FMath::Max(Worst, FVector::Dist(Fit.TransformPosition(FVector(Point)), Truth.TransformPosition(FVector(Point))));
	}
	TestTrue(FString::Printf(TEXT("every point lands where the model's is (worst %.3f cm)"), Worst), Worst < 0.3);

	// Nothing to fit: the start comes back.
	double Unused = 0.0;
	const FTransform Same = FMantleUnriggedAsIs::FitRigid(Hand, TArray<FVector3f>(), FTransform(FVector(1.0, 2.0, 3.0)), 10, Unused, Unused);
	TestTrue(TEXT("no target gives the start back"), Same.GetTranslation().Equals(FVector(1.0, 2.0, 3.0)));

	// The skin with it: a point all on the hand moves with the hand, one on the forearm stays, one at the wrist half way.
	const TArray<FTransform> Rest = { FTransform::Identity, FTransform(FVector(30.0, 0.0, 0.0)) };
	const TArray<FTransform> Laid = { FTransform::Identity, FTransform(FQuat(FVector::UpVector, UE_DOUBLE_HALF_PI), FVector(30.0, 0.0, 0.0)) };
	const TArray<int32> Bones = { 1, 0, 0, 0, 1, 0 };
	const TArray<float> Weights = { 1.0f, 0.0f, 1.0f, 0.0f, 0.5f, 0.5f };
	TArray<FVector3f> Skin = { FVector3f(40, 0, 0), FVector3f(15, 2, 0), FVector3f(30, 2, 0) };
	const TArray<FVector3f> Before = Skin;
	FMantleUnriggedAsIs::MoveByWeights(Skin, 2, Bones, Weights, Rest, Laid, /*bInverse*/ false);
	TestTrue(TEXT("the fingertip turns with the hand"), FVector3f::Dist(Skin[0], FVector3f(30, 10, 0)) < 1e-3f);
	TestTrue(TEXT("the forearm stays"), FVector3f::Dist(Skin[1], Before[1]) < 1e-3f);
	TestTrue(TEXT("the wrist moves half way"), FVector3f::Dist(Skin[2], FVector3f(29, 1, 0)) < 1e-3f);
	FMantleUnriggedAsIs::MoveByWeights(Skin, 2, Bones, Weights, Rest, Laid, /*bInverse*/ true);
	for (int32 Point = 0; Point < Skin.Num(); ++Point)
	{
		TestTrue(FString::Printf(TEXT("skin point %d moves back where it was"), Point), FVector3f::Dist(Skin[Point], Before[Point]) < 1e-3f);
	}
	return true;
}

#endif
