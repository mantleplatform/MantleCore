// Copyright Joshua Eger. All Rights Reserved.
#include "MantleLayerFit.h"

#include "MantleGarmentClearance.h"
#include "MantleSkinnedMesh.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// The layer pass (#65 layers) on synthetic tubes around Z, no assets. The shirt is an open tube of radius 10 cm, 48 sides,
// from z -25 to 25. The tight jacket wall is cut too small in two places: 1.5 cm inside the shirt from z 0.5 to 9.5 (a
// push away from being legal) and 4 cm inside it below z -11.5 (deeper than the push reaches); elsewhere it is 0.5 cm
// outside. It has two LODs (48 and 24 sides). The tubes' vertices are turned a fraction of a step and their rings lie
// off the shirt's, so no vertex sits on an edge or a diagonal of the shirt.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleLayerFitTestPrivate
{
	constexpr float ShirtRadius = 10.0f;
	constexpr int32 ShirtAround = 48;

	/** The 0.3 cm gap is met within this: the shirt's 48 sides are 0.02 cm shallower than the circle, and the push is smoothed. */
	constexpr float GapSlackCm = 0.03f;

	/** A tube around Z of RadiusAt(z), normals and winding outward, Rings rings ZStep apart from Z0, turned Phase steps around, one section. */
	FMantleSkinnedMeshLOD MakeTube(const int32 Around, const float Phase, const float Z0, const float ZStep, const int32 Rings, const TFunctionRef<float(float)> RadiusAt)
	{
		FMantleSkinnedMeshLOD LOD;
		for (int32 Ring = 0; Ring < Rings; ++Ring)
		{
			const float Z = Z0 + ZStep * Ring;
			const float Radius = RadiusAt(Z);
			for (int32 Step = 0; Step < Around; ++Step)
			{
				const float Theta = 2.0f * PI * (Step + Phase) / Around;
				LOD.Positions.Add(FVector3f(Radius * FMath::Cos(Theta), Radius * FMath::Sin(Theta), Z));
				LOD.Normals.Add(FVector3f(FMath::Cos(Theta), FMath::Sin(Theta), 0.0f));
			}
		}
		for (int32 Ring = 0; Ring + 1 < Rings; ++Ring)
		{
			for (int32 Step = 0; Step < Around; ++Step)
			{
				const uint32 A = Ring * Around + Step;
				const uint32 B = Ring * Around + (Step + 1) % Around;
				const uint32 C = A + Around;
				const uint32 D = B + Around;
				LOD.Indices.Append({ A, B, D, A, D, C });
			}
		}
		FMantleSkinnedMeshSection Section;
		Section.MaterialIndex = 0;
		Section.FirstIndex = 0;
		Section.IndexCount = LOD.Indices.Num();
		LOD.Sections.Add(Section);
		return LOD;
	}

	TSharedPtr<const FMantleSkinnedMesh> MakeGarment(TArray<FMantleSkinnedMeshLOD>&& LODs)
	{
		TSharedRef<FMantleSkinnedMesh> Mesh = MakeShared<FMantleSkinnedMesh>();
		Mesh->LODs = MoveTemp(LODs);
		return Mesh;
	}

	/** One LOD: a tube of one radius. */
	TSharedPtr<const FMantleSkinnedMesh> MakeUniform(const float Radius, const float Phase, const float Z0, const int32 Rings)
	{
		TArray<FMantleSkinnedMeshLOD> LODs;
		LODs.Add(MakeTube(ShirtAround, Phase, Z0, 1.0f, Rings, [Radius](const float) { return Radius; }));
		return MakeGarment(MoveTemp(LODs));
	}

	TSharedPtr<const FMantleSkinnedMesh> MakeShirt()
	{
		return MakeUniform(ShirtRadius, 0.0f, -25.0f, 51);
	}

	/** How far out of the shirt (negative: into it) the tight jacket's wall is at Z. */
	float JacketDepthAt(const float Z)
	{
		if (Z < -11.5f)
		{
			return -4.0f;
		}
		if (Z < 0.5f)
		{
			return 0.5f;
		}
		return Z <= 9.5f ? -1.5f : 0.5f;
	}

	bool InDeepZone(const float Z)
	{
		return Z < -11.5f;
	}

	bool InShallowZone(const float Z)
	{
		return Z >= 0.5f && Z <= 9.5f;
	}

	/** The jacket cut too tight: LOD0 48 sides, rings 1 cm apart; LOD1 24 sides, rings 2 cm apart. */
	TSharedPtr<const FMantleSkinnedMesh> MakeTightJacket()
	{
		const auto RadiusAt = [](const float Z) { return ShirtRadius + JacketDepthAt(Z); };
		TArray<FMantleSkinnedMeshLOD> LODs;
		LODs.Add(MakeTube(ShirtAround, 0.25f, -19.75f, 1.0f, 41, RadiusAt));
		LODs.Add(MakeTube(ShirtAround / 2, 0.25f, -19.75f, 2.0f, 21, RadiusAt));
		return MakeGarment(MoveTemp(LODs));
	}

	FMantleLayerGarment MakeLayer(const TSharedPtr<const FMantleSkinnedMesh>& Mesh, const int32 Rank)
	{
		FMantleLayerGarment Layer;
		Layer.Mesh = Mesh;
		Layer.Rank = Rank;
		return Layer;
	}

	float Radial(const FVector3f& P)
	{
		return FMath::Sqrt(P.X * P.X + P.Y * P.Y);
	}

	/** The smallest clearance over Under of Outer's vertices with |z| at most ZLimit; -1000 when one is not tracked within 10 cm. */
	float WorstClearance(const FMantleSkinnedMeshLOD& Outer, const FMantleSkinnedMeshLOD& Under, const float ZLimit)
	{
		FMantleGarmentClearance Clearance;
		if (!FMantleGarmentClearance::Compute(Outer.Positions, Under, Clearance))
		{
			return -1000.0f;
		}
		float Worst = TNumericLimits<float>::Max();
		for (int32 Vertex = 0; Vertex < Outer.NumVertices(); ++Vertex)
		{
			if (FMath::Abs(Outer.Positions[Vertex].Z) <= ZLimit)
			{
				Worst = FMath::Min(Worst, Clearance.IsTracked(Vertex) ? Clearance.ClearanceCm[Vertex] : -1000.0f);
			}
		}
		return Worst == TNumericLimits<float>::Max() ? -1000.0f : Worst;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleLayerFitTightJacketTest,
	"Mantle.Runtime.LayerFit.TightJacket",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleLayerFitTightJacketTest::RunTest(const FString& Parameters)
{
	using namespace MantleLayerFitTestPrivate;
	// A jacket (rank 6, listed first) over a shirt (rank 5): where the jacket is less than 3 cm inside the shirt it ends 0.3 cm
	// outside it, in every LOD; where it is 3 cm or more inside it stays exactly where it was; nothing moves inward; the
	// meshes handed in are not changed, and the shirt, with nothing under it, comes back as it is.
	const TSharedPtr<const FMantleSkinnedMesh> Shirt = MakeShirt();
	const TSharedPtr<const FMantleSkinnedMesh> Jacket = MakeTightJacket();
	const FMantleSkinnedMesh JacketBefore = *Jacket;

	TArray<FMantleLayerGarment> Garments;
	Garments.Add(MakeLayer(Jacket, 6));
	Garments.Add(MakeLayer(Shirt, 5));
	TArray<TSharedPtr<const FMantleSkinnedMesh>> Placed;
	FMantleLayerFitStats Stats;
	FMantleLayerFit::Apply(Garments, Placed, Stats);

	TestEqual(TEXT("both garments are layers"), Stats.Garments, 2);
	TestEqual(TEXT("one garment was pushed"), Stats.Pushed, 1);
	TestEqual(TEXT("none refused"), Stats.Failed, 0);
	TestEqual(TEXT("one pair measured"), Stats.Pairs, 1);
	TestTrue(TEXT("vertices moved"), Stats.Moved > 0);
	TestTrue(TEXT("the largest push is about 1.8 cm (1.5 cm in, 0.3 cm gap)"), Stats.MaxPushCm > 1.5 && Stats.MaxPushCm < 2.1);
	if (!TestEqual(TEXT("one placed per garment, in the order given"), Placed.Num(), 2) || !Placed[0].IsValid() || !Placed[1].IsValid())
	{
		return false;
	}
	TestTrue(TEXT("the shirt is the same mesh"), Placed[1] == Shirt);
	TestTrue(TEXT("the jacket is a copy"), Placed[0] != Jacket);
	if (Placed[0] == Jacket || Placed[0]->LODs.Num() != 2)
	{
		return false;
	}
	for (int32 LODIndex = 0; LODIndex < 2; ++LODIndex)
	{
		TestTrue(*FString::Printf(TEXT("LOD%d of the jacket handed in is untouched"), LODIndex), Jacket->LODs[LODIndex].Positions == JacketBefore.LODs[LODIndex].Positions);
	}

	for (int32 LODIndex = 0; LODIndex < 2; ++LODIndex)
	{
		const FMantleSkinnedMeshLOD& Before = Jacket->LODs[LODIndex];
		const FMantleSkinnedMeshLOD& Now = Placed[0]->LODs[LODIndex];
		if (!TestEqual(*FString::Printf(TEXT("LOD%d keeps its vertices"), LODIndex), Now.NumVertices(), Before.NumVertices()))
		{
			continue;
		}
		FMantleGarmentClearance After;
		if (!TestTrue(*FString::Printf(TEXT("LOD%d measures over the shirt"), LODIndex), FMantleGarmentClearance::Compute(Now.Positions, Shirt->LODs[0], After)))
		{
			continue;
		}
		int32 Shallow = 0;
		int32 ShallowShort = 0;
		float WorstGap = TNumericLimits<float>::Max();
		int32 Deep = 0;
		int32 DeepMoved = 0;
		int32 Inward = 0;
		for (int32 Vertex = 0; Vertex < Before.NumVertices(); ++Vertex)
		{
			const float Z = Before.Positions[Vertex].Z;
			if (InShallowZone(Z))
			{
				++Shallow;
				const float Gap = After.IsTracked(Vertex) ? After.ClearanceCm[Vertex] : -1000.0f;
				WorstGap = FMath::Min(WorstGap, Gap);
				ShallowShort += Gap >= FMantleLayerFit::GapCm - GapSlackCm ? 0 : 1;
			}
			else if (InDeepZone(Z))
			{
				++Deep;
				DeepMoved += Now.Positions[Vertex] == Before.Positions[Vertex] ? 0 : 1;
			}
			Inward += Radial(Now.Positions[Vertex]) < Radial(Before.Positions[Vertex]) - 0.001f ? 1 : 0;
		}
		TestTrue(*FString::Printf(TEXT("LOD%d has points inside the push's reach"), LODIndex), Shallow > 0);
		TestTrue(*FString::Printf(TEXT("LOD%d has points beyond it"), LODIndex), Deep > 0);
		TestEqual(*FString::Printf(TEXT("LOD%d: points less than 3 cm deep short of 0.3 cm off the shirt (worst %.3f cm)"), LODIndex, WorstGap), ShallowShort, 0);
		TestEqual(*FString::Printf(TEXT("LOD%d: points 3 cm or more deep that moved"), LODIndex), DeepMoved, 0);
		TestEqual(*FString::Printf(TEXT("LOD%d: points that moved inward"), LODIndex), Inward, 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleLayerFitUnchangedTest,
	"Mantle.Runtime.LayerFit.Unchanged",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleLayerFitUnchangedTest::RunTest(const FString& Parameters)
{
	using namespace MantleLayerFitTestPrivate;
	const TSharedPtr<const FMantleSkinnedMesh> Shirt = MakeShirt();
	TArray<TSharedPtr<const FMantleSkinnedMesh>> Placed;
	FMantleLayerFitStats Stats;

	// A look of one garment: nothing under it, nothing to do.
	{
		TArray<FMantleLayerGarment> Garments;
		Garments.Add(MakeLayer(Shirt, 5));
		FMantleLayerFit::Apply(Garments, Placed, Stats);
		TestEqual(TEXT("one: placed"), Placed.Num(), 1);
		TestTrue(TEXT("one: the same mesh"), Placed.Num() == 1 && Placed[0] == Shirt);
		TestEqual(TEXT("one: pushed"), Stats.Pushed, 0);
		TestEqual(TEXT("one: moved"), Stats.Moved, 0);
		TestEqual(TEXT("one: pairs"), Stats.Pairs, 0);
	}

	// A jacket already 1 cm clear of the shirt is measured and left as it is.
	{
		const TSharedPtr<const FMantleSkinnedMesh> Clear = MakeUniform(ShirtRadius + 1.0f, 0.25f, -19.75f, 41);
		TArray<FMantleLayerGarment> Garments;
		Garments.Add(MakeLayer(Shirt, 5));
		Garments.Add(MakeLayer(Clear, 6));
		FMantleLayerFit::Apply(Garments, Placed, Stats);
		TestTrue(TEXT("clear: both the same meshes"), Placed.Num() == 2 && Placed[0] == Shirt && Placed[1] == Clear);
		TestEqual(TEXT("clear: pairs measured"), Stats.Pairs, 1);
		TestEqual(TEXT("clear: points needing a push"), Stats.Below, 0);
		TestEqual(TEXT("clear: pushed"), Stats.Pushed, 0);
		TestEqual(TEXT("clear: moved"), Stats.Moved, 0);
	}

	// A garment on another part of the body (100 cm up the axis, inside the shirt's radius) is not measured at all.
	{
		const TSharedPtr<const FMantleSkinnedMesh> Far = MakeUniform(ShirtRadius - 1.5f, 0.25f, 80.0f, 41);
		TArray<FMantleLayerGarment> Garments;
		Garments.Add(MakeLayer(Shirt, 5));
		Garments.Add(MakeLayer(Far, 6));
		FMantleLayerFit::Apply(Garments, Placed, Stats);
		TestTrue(TEXT("far: both the same meshes"), Placed.Num() == 2 && Placed[0] == Shirt && Placed[1] == Far);
		TestEqual(TEXT("far: pairs measured"), Stats.Pairs, 0);
		TestEqual(TEXT("far: pushed"), Stats.Pushed, 0);
	}

	// An entry without a mesh is passed through and is not a layer.
	{
		TArray<FMantleLayerGarment> Garments;
		Garments.Add(MakeLayer(nullptr, 0));
		Garments.Add(MakeLayer(Shirt, 5));
		FMantleLayerFit::Apply(Garments, Placed, Stats);
		TestTrue(TEXT("empty: the entry stays empty and the shirt as it is"), Placed.Num() == 2 && !Placed[0].IsValid() && Placed[1] == Shirt);
		TestEqual(TEXT("empty: layers"), Stats.Garments, 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleLayerFitOrderTest,
	"Mantle.Runtime.LayerFit.OrderFollowsRank",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleLayerFitOrderTest::RunTest(const FString& Parameters)
{
	using namespace MantleLayerFitTestPrivate;
	// Two tubes 0.1 cm apart, listed the same way both times (A, then B). Only the ranks change: the tube with the higher rank
	// is the outer one and is the one that moves, as a tucked top swaps places with the bottom.
	const TSharedPtr<const FMantleSkinnedMesh> A = MakeUniform(ShirtRadius, 0.0f, -25.0f, 51);
	const TSharedPtr<const FMantleSkinnedMesh> B = MakeUniform(ShirtRadius + 0.1f, 0.25f, -19.75f, 41);

	{
		TArray<FMantleLayerGarment> Garments;
		Garments.Add(MakeLayer(A, 1));
		Garments.Add(MakeLayer(B, 2));
		TArray<TSharedPtr<const FMantleSkinnedMesh>> Placed;
		FMantleLayerFitStats Stats;
		FMantleLayerFit::Apply(Garments, Placed, Stats);
		if (!TestEqual(TEXT("A under B: placed"), Placed.Num(), 2) || !Placed[0].IsValid() || !Placed[1].IsValid())
		{
			return false;
		}
		TestTrue(TEXT("A under B: A is the same mesh"), Placed[0] == A);
		TestTrue(TEXT("A under B: B moved"), Placed[1] != B);
		TestEqual(TEXT("A under B: garments pushed"), Stats.Pushed, 1);
		if (Placed[1] != B)
		{
			const float Worst = WorstClearance(Placed[1]->LODs[0], A->LODs[0], 1000.0f);
			TestTrue(*FString::Printf(TEXT("A under B: every point of B is 0.3 cm off A (worst %.3f cm)"), Worst), Worst >= FMantleLayerFit::GapCm - GapSlackCm);
		}
	}
	{
		TArray<FMantleLayerGarment> Garments;
		Garments.Add(MakeLayer(A, 2));
		Garments.Add(MakeLayer(B, 1));
		TArray<TSharedPtr<const FMantleSkinnedMesh>> Placed;
		FMantleLayerFitStats Stats;
		FMantleLayerFit::Apply(Garments, Placed, Stats);
		if (!TestEqual(TEXT("B under A: placed"), Placed.Num(), 2) || !Placed[0].IsValid() || !Placed[1].IsValid())
		{
			return false;
		}
		TestTrue(TEXT("B under A: B is the same mesh"), Placed[1] == B);
		TestTrue(TEXT("B under A: A moved"), Placed[0] != A);
		TestEqual(TEXT("B under A: garments pushed"), Stats.Pushed, 1);
		if (Placed[0] != A)
		{
			// A runs 5 cm past B at both ends, where nothing is over B; the middle is checked.
			const float Worst = WorstClearance(Placed[0]->LODs[0], B->LODs[0], 15.0f);
			TestTrue(*FString::Printf(TEXT("B under A: A is 0.3 cm off B (worst %.3f cm)"), Worst), Worst >= FMantleLayerFit::GapCm - GapSlackCm);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleLayerFitStackTest,
	"Mantle.Runtime.LayerFit.Stack",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleLayerFitStackTest::RunTest(const FString& Parameters)
{
	using namespace MantleLayerFitTestPrivate;
	// Three tubes cut at the same radius, listed out of order (coat, shirt, jacket): the jacket clears the shirt, and the coat
	// clears the jacket where the jacket now is (0.3 cm further out), not where it started.
	const TSharedPtr<const FMantleSkinnedMesh> Shirt = MakeShirt();
	const TSharedPtr<const FMantleSkinnedMesh> Jacket = MakeUniform(ShirtRadius, 0.25f, -19.7f, 41);
	const TSharedPtr<const FMantleSkinnedMesh> Coat = MakeUniform(ShirtRadius, 0.6f, -19.4f, 41);

	TArray<FMantleLayerGarment> Garments;
	Garments.Add(MakeLayer(Coat, 3));
	Garments.Add(MakeLayer(Shirt, 1));
	Garments.Add(MakeLayer(Jacket, 2));
	TArray<TSharedPtr<const FMantleSkinnedMesh>> Placed;
	FMantleLayerFitStats Stats;
	FMantleLayerFit::Apply(Garments, Placed, Stats);
	if (!TestEqual(TEXT("placed"), Placed.Num(), 3) || !Placed[0].IsValid() || !Placed[1].IsValid() || !Placed[2].IsValid())
	{
		return false;
	}
	TestTrue(TEXT("the shirt is the same mesh"), Placed[1] == Shirt);
	TestTrue(TEXT("the jacket moved"), Placed[2] != Jacket);
	TestTrue(TEXT("the coat moved"), Placed[0] != Coat);
	TestEqual(TEXT("garments pushed"), Stats.Pushed, 2);
	if (Placed[0] == Coat || Placed[2] == Jacket)
	{
		return false;
	}
	const float JacketOverShirt = WorstClearance(Placed[2]->LODs[0], Shirt->LODs[0], 15.0f);
	TestTrue(*FString::Printf(TEXT("jacket 0.3 cm off the shirt (worst %.3f cm)"), JacketOverShirt), JacketOverShirt >= FMantleLayerFit::GapCm - GapSlackCm);
	const float CoatOverJacket = WorstClearance(Placed[0]->LODs[0], Placed[2]->LODs[0], 15.0f);
	TestTrue(*FString::Printf(TEXT("coat 0.3 cm off the pushed jacket (worst %.3f cm)"), CoatOverJacket), CoatOverJacket >= FMantleLayerFit::GapCm - GapSlackCm);
	// Off the shirt by the two gaps, far more than the 0.3 cm it would have if it had been cleared of the jacket as it started.
	const float CoatOverShirt = WorstClearance(Placed[0]->LODs[0], Shirt->LODs[0], 15.0f);
	TestTrue(*FString::Printf(TEXT("coat over the shirt, both gaps (worst %.3f cm)"), CoatOverShirt), CoatOverShirt >= 0.45f);
	return true;
}

#endif
