// Copyright Joshua Eger. All Rights Reserved.
#include "MantleLayerFit.h"
#include "MantleSkinnedMesh.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// The layer pass's hide and sink (#65 layers, L4) on synthetic tubes around Z, no assets.
//
// The shirt is a closed tube of radius 10 cm, 48 sides, rings 1 cm apart from z -25 to 25, in two sections (the split at z 0); its
// LOD1 has 24 sides and rings 2 cm apart. The jacket is an open arc (36 of the 48 steps, 270 degrees, the front left open) 0.35 cm
// outside it, rings 1 cm apart from z -10.2 to 9.8, turned a quarter step so no vertex sits on an edge or a diagonal of the shirt.
// Its open edges are the hems at both ends and the two sides of the front. Shirt vertices are named by ring (their z) and step.
//
// Distances from the jacket's edges (3 cm kept band, the 0.35 cm stand-off included): ring -7 is 3.2 cm from the bottom hem (outside
// the band), ring -8 is 2.2 (inside); ring 6 is 3.8 from the top hem, ring 7 is 2.8; step 4 is 4.9 cm from the front edge, step 2 is
// 2.35; the arc's back edge is at step 36.25 (it has 37 vertices a ring, steps 0.25 to 36.25), so step 31 is 6.9 cm from it, step 34 is
// 3.006 (just outside the band: the shirt's circle and the jacket's, 0.35 cm out, are 3.006 cm apart there), step 35 is 1.7 and step 36
// is 0.5 (covered, under the jacket's last column). Rings and steps in between are not asserted.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleLayerHideTestPrivate
{
	constexpr float ShirtRadius = 10.0f;
	constexpr float JacketOutCm = 0.35f;
	constexpr int32 ShirtAround = 48;

	struct FTubeSpec
	{
		int32 Around = 48;
		float Phase = 0.0f;
		float Z0 = 0.0f;
		float ZStep = 1.0f;
		int32 Rings = 2;
		float Radius = 10.0f;
		/** Steps of the arc, 0 for the whole way round (a closed tube). An arc has one more vertex per ring than steps and open sides. */
		int32 ArcSteps = 0;
		/** More than 0: the bands before this one are section 0 (material 0), the rest section 1 (material 1). */
		int32 SplitAfterBands = 0;
	};

	/** A tube (or an arc of one) around Z, normals and winding outward, one ring per ZStep. */
	FMantleSkinnedMeshLOD MakeTube(const FTubeSpec& Spec)
	{
		FMantleSkinnedMeshLOD LOD;
		const int32 Steps = Spec.ArcSteps > 0 ? Spec.ArcSteps : Spec.Around;
		const int32 PerRing = Steps == Spec.Around ? Spec.Around : Steps + 1;
		for (int32 Ring = 0; Ring < Spec.Rings; ++Ring)
		{
			const float Z = Spec.Z0 + Spec.ZStep * Ring;
			for (int32 Step = 0; Step < PerRing; ++Step)
			{
				const float Theta = 2.0f * PI * (Step + Spec.Phase) / Spec.Around;
				LOD.Positions.Add(FVector3f(Spec.Radius * FMath::Cos(Theta), Spec.Radius * FMath::Sin(Theta), Z));
				LOD.Normals.Add(FVector3f(FMath::Cos(Theta), FMath::Sin(Theta), 0.0f));
			}
		}
		for (int32 Ring = 0; Ring + 1 < Spec.Rings; ++Ring)
		{
			for (int32 Step = 0; Step < Steps; ++Step)
			{
				const uint32 A = Ring * PerRing + Step;
				const uint32 B = Ring * PerRing + (Step + 1) % PerRing;
				const uint32 C = A + PerRing;
				const uint32 D = B + PerRing;
				LOD.Indices.Append({ A, B, D, A, D, C });
			}
		}
		if (Spec.SplitAfterBands > 0)
		{
			const int32 FirstCount = Spec.SplitAfterBands * Steps * 6;
			FMantleSkinnedMeshSection First;
			First.MaterialIndex = 0;
			First.FirstIndex = 0;
			First.IndexCount = FirstCount;
			FMantleSkinnedMeshSection Second;
			Second.MaterialIndex = 1;
			Second.FirstIndex = FirstCount;
			Second.IndexCount = LOD.Indices.Num() - FirstCount;
			LOD.Sections.Add(First);
			LOD.Sections.Add(Second);
		}
		else
		{
			FMantleSkinnedMeshSection Section;
			Section.MaterialIndex = 0;
			Section.FirstIndex = 0;
			Section.IndexCount = LOD.Indices.Num();
			LOD.Sections.Add(Section);
		}
		return LOD;
	}

	TSharedPtr<const FMantleSkinnedMesh> MakeGarment(TArray<FMantleSkinnedMeshLOD>&& LODs)
	{
		TSharedRef<FMantleSkinnedMesh> Mesh = MakeShared<FMantleSkinnedMesh>();
		Mesh->LODs = MoveTemp(LODs);
		return Mesh;
	}

	/** The shirt: LOD0 48 sides, rings 1 cm apart, two sections (split at z 0); LOD1 24 sides, rings 2 cm apart, one section. */
	TSharedPtr<const FMantleSkinnedMesh> MakeShirt()
	{
		FTubeSpec Fine;
		Fine.Z0 = -25.0f;
		Fine.Rings = 51;
		Fine.Radius = ShirtRadius;
		Fine.SplitAfterBands = 25;
		FTubeSpec Coarse;
		Coarse.Around = ShirtAround / 2;
		Coarse.Z0 = -25.0f;
		Coarse.ZStep = 2.0f;
		Coarse.Rings = 26;
		Coarse.Radius = ShirtRadius;
		TArray<FMantleSkinnedMeshLOD> LODs;
		LODs.Add(MakeTube(Fine));
		LODs.Add(MakeTube(Coarse));
		return MakeGarment(MoveTemp(LODs));
	}

	/** The jacket: an arc of 36 of the 48 steps, 0.35 cm outside the shirt, z -10.2 to 9.8. */
	TSharedPtr<const FMantleSkinnedMesh> MakeOpenJacket()
	{
		FTubeSpec Spec;
		Spec.Phase = 0.25f;
		Spec.Z0 = -10.2f;
		Spec.Rings = 21;
		Spec.Radius = ShirtRadius + JacketOutCm;
		Spec.ArcSteps = 36;
		TArray<FMantleSkinnedMeshLOD> LODs;
		LODs.Add(MakeTube(Spec));
		return MakeGarment(MoveTemp(LODs));
	}

	FMantleLayerGarment MakeLayer(const TSharedPtr<const FMantleSkinnedMesh>& Mesh, const int32 Rank, const bool bOverlay = false, const bool bCoversOverlay = false)
	{
		FMantleLayerGarment Layer;
		Layer.Mesh = Mesh;
		Layer.Rank = Rank;
		Layer.bOverlay = bOverlay;
		Layer.bCoversOverlay = bCoversOverlay;
		return Layer;
	}

	/** The ring (z rounded) and the step (angle over a step, wrapped) of a shirt or nail vertex. */
	int32 RingOf(const FVector3f& P)
	{
		return FMath::RoundToInt32(P.Z);
	}

	int32 StepOf(const FVector3f& P, const int32 Around)
	{
		const int32 Step = FMath::RoundToInt32(FMath::Atan2(P.Y, P.X) / (2.0f * PI / Around));
		return ((Step % Around) + Around) % Around;
	}

	float Radial(const FVector3f& P)
	{
		return FMath::Sqrt(P.X * P.X + P.Y * P.Y);
	}

	FIntVector Triple(const FMantleSkinnedMeshLOD& LOD, const int32 Triangle)
	{
		return FIntVector(static_cast<int32>(LOD.Indices[Triangle * 3]), static_cast<int32>(LOD.Indices[Triangle * 3 + 1]), static_cast<int32>(LOD.Indices[Triangle * 3 + 2]));
	}

	/** The triangles (by their three vertex indices; a cut keeps each one's own) an LOD still has. */
	TSet<FIntVector> KeptTriangles(const FMantleSkinnedMeshLOD& LOD)
	{
		TSet<FIntVector> Kept;
		for (int32 Triangle = 0; Triangle < LOD.NumTriangles(); ++Triangle)
		{
			Kept.Add(Triple(LOD, Triangle));
		}
		return Kept;
	}

	/** The shirt under the open jacket: the look all the hide tests read. */
	struct FShirtUnderJacket
	{
		TSharedPtr<const FMantleSkinnedMesh> Shirt;
		TSharedPtr<const FMantleSkinnedMesh> Jacket;
		TArray<TSharedPtr<const FMantleSkinnedMesh>> Placed;
		TArray<TSharedPtr<const FMantleSkinnedMesh>> Shown;
		FMantleLayerFitStats Stats;

		/** The premises every test needs: both garments, the jacket needed no push and nothing lies over it, the shirt was cut. */
		bool Run(FAutomationTestBase& Test)
		{
			Shirt = MakeShirt();
			Jacket = MakeOpenJacket();
			TArray<FMantleLayerGarment> Garments;
			Garments.Add(MakeLayer(Shirt, 5));
			Garments.Add(MakeLayer(Jacket, 6));
			FMantleLayerFit::Apply(Garments, Placed, Stats, &Shown);
			if (!Test.TestEqual(TEXT("one placed and one shown per garment"), Placed.Num() + Shown.Num(), 4) || !Shown[0].IsValid() || !Shown[1].IsValid())
			{
				return false;
			}
			Test.TestTrue(TEXT("the jacket is 0.35 cm off the shirt: it was not pushed (the premise of these numbers)"), Placed[1] == Jacket);
			Test.TestTrue(TEXT("the placed shirt, the dressed body's, is the uncut one"), Placed[0] == Shirt);
			Test.TestTrue(TEXT("nothing lies over the jacket: shown as placed"), Shown[1] == Jacket);
			Test.TestTrue(TEXT("the shirt shown is an edited copy"), Shown[0] != Shirt && Shown[0]->LODs.Num() == 2);
			return Placed[1] == Jacket && Shown[0] != Shirt && Shown[0]->LODs.Num() == 2;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleLayerHideCoveredTest,
	"Mantle.Runtime.LayerFit.HideCovered",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleLayerHideCoveredTest::RunTest(const FString& Parameters)
{
	using namespace MantleLayerHideTestPrivate;
	// Shirt triangles whose corners the jacket covers and that lie 3 cm or more from its hems and front edges are cut, in every
	// LOD; the ones within 3 cm of an edge stay; the sections' ranges follow the cut.
	FShirtUnderJacket Look;
	if (!Look.Run(*this))
	{
		return false;
	}
	TestEqual(TEXT("one garment edited"), Look.Stats.Edited, 1);
	TestEqual(TEXT("none refused"), Look.Stats.HideFailed, 0);

	// LOD0.
	{
		const FMantleSkinnedMeshLOD& Before = Look.Shirt->LODs[0];
		const FMantleSkinnedMeshLOD& After = Look.Shown[0]->LODs[0];
		const TSet<FIntVector> Kept = KeptTriangles(After);
		int32 Cut = 0;
		int32 CertainCut = 0;
		int32 MissedCuts = 0;
		int32 WrongCuts = 0;
		for (int32 Triangle = 0; Triangle < Before.NumTriangles(); ++Triangle)
		{
			bool bAllRingsDeep = true;
			bool bAllStepsDeep = true;
			bool bNearAnEdge = false;
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const FVector3f& P = Before.Positions[Before.Indices[Triangle * 3 + Corner]];
				const int32 Ring = RingOf(P);
				const int32 Step = StepOf(P, ShirtAround);
				bAllRingsDeep &= Ring >= -7 && Ring <= 6;
				bAllStepsDeep &= Step >= 4 && Step <= 31;
				bNearAnEdge |= Ring <= -8 || Ring >= 7 || Step <= 2 || Step >= 35;
			}
			const bool bKept = Kept.Contains(Triple(Before, Triangle));
			Cut += bKept ? 0 : 1;
			if (bAllRingsDeep && bAllStepsDeep)
			{
				++CertainCut;
				MissedCuts += bKept ? 1 : 0;
			}
			WrongCuts += (bNearAnEdge && !bKept) ? 1 : 0;
		}
		TestTrue(TEXT("LOD0: a good many triangles are certain to go (the premise)"), CertainCut > 600);
		TestEqual(TEXT("LOD0: covered triangles 3 cm or more from every edge that stayed"), MissedCuts, 0);
		TestEqual(TEXT("LOD0: triangles within 3 cm of an edge (or past it) that were cut"), WrongCuts, 0);
		TestEqual(TEXT("LOD0: the index list lost exactly the cut triangles"), After.Indices.Num(), Before.Indices.Num() - 3 * Cut);
		TestEqual(TEXT("LOD0: the stats count them"), Look.Stats.HiddenLOD0, Cut);
		TestEqual(TEXT("LOD0: no vertex was added or removed"), After.NumVertices(), Before.NumVertices());

		// Sections: the shirt's two, contiguous, covering the whole list, each holding only its own triangles (section 0 is the part below z 0).
		if (TestEqual(TEXT("LOD0: both sections are still there"), After.Sections.Num(), 2))
		{
			const FMantleSkinnedMeshSection& First = After.Sections[0];
			const FMantleSkinnedMeshSection& Second = After.Sections[1];
			TestEqual(TEXT("LOD0: section 0 starts at 0"), First.FirstIndex, 0);
			TestEqual(TEXT("LOD0: section 1 follows section 0"), Second.FirstIndex, First.IndexCount);
			TestEqual(TEXT("LOD0: the sections cover every index"), First.IndexCount + Second.IndexCount, After.Indices.Num());
			TestTrue(TEXT("LOD0: both lost triangles (the cut crosses z 0)"), First.IndexCount < Before.Sections[0].IndexCount && Second.IndexCount < Before.Sections[1].IndexCount);
			TestTrue(TEXT("LOD0: whole triangles"), First.IndexCount % 3 == 0 && Second.IndexCount % 3 == 0);
			TestEqual(TEXT("LOD0: materials unchanged"), First.MaterialIndex + 10 * Second.MaterialIndex, 10);
			int32 WrongSection = 0;
			for (int32 Index = 0; Index < After.Indices.Num(); ++Index)
			{
				const int32 Ring = RingOf(After.Positions[After.Indices[Index]]);
				const bool bInFirst = Index >= First.FirstIndex && Index < First.FirstIndex + First.IndexCount;
				WrongSection += bInFirst ? (Ring > 0 ? 1 : 0) : (Ring < 0 ? 1 : 0);
			}
			TestEqual(TEXT("LOD0: every triangle is in the section it was in (below z 0, from z 0 up)"), WrongSection, 0);
		}
	}

	// LOD1 (24 sides, rings 2 cm apart, against the jacket's LOD0): the same rule on its own, longer triangles.
	{
		const FMantleSkinnedMeshLOD& Before = Look.Shirt->LODs[1];
		const FMantleSkinnedMeshLOD& After = Look.Shown[0]->LODs[1];
		const TSet<FIntVector> Kept = KeptTriangles(After);
		int32 Cut = 0;
		int32 CertainCut = 0;
		int32 MissedCuts = 0;
		int32 WrongCuts = 0;
		for (int32 Triangle = 0; Triangle < Before.NumTriangles(); ++Triangle)
		{
			bool bAllRingsDeep = true;
			bool bAllStepsDeep = true;
			bool bNearAnEdge = false;
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const FVector3f& P = Before.Positions[Before.Indices[Triangle * 3 + Corner]];
				const int32 Ring = RingOf(P);
				const int32 Step = StepOf(P, ShirtAround / 2);
				bAllRingsDeep &= Ring >= -7 && Ring <= 5;
				bAllStepsDeep &= Step >= 2 && Step <= 15;
				bNearAnEdge |= Ring <= -9 || Ring >= 7 || Step <= 1 || Step >= 18;
			}
			const bool bKept = Kept.Contains(Triple(Before, Triangle));
			Cut += bKept ? 0 : 1;
			if (bAllRingsDeep && bAllStepsDeep)
			{
				++CertainCut;
				MissedCuts += bKept ? 1 : 0;
			}
			WrongCuts += (bNearAnEdge && !bKept) ? 1 : 0;
		}
		TestTrue(TEXT("LOD1: triangles are certain to go (the premise)"), CertainCut > 50);
		TestEqual(TEXT("LOD1: covered triangles 3 cm or more from every edge that stayed"), MissedCuts, 0);
		TestEqual(TEXT("LOD1: triangles within 3 cm of an edge (or past it) that were cut"), WrongCuts, 0);
		TestEqual(TEXT("LOD1: the index list lost exactly the cut triangles"), After.Indices.Num(), Before.Indices.Num() - 3 * Cut);
		if (TestEqual(TEXT("LOD1: its one section is still there"), After.Sections.Num(), 1))
		{
			TestEqual(TEXT("LOD1: the section covers every index"), After.Sections[0].IndexCount, After.Indices.Num());
			TestEqual(TEXT("LOD1: the section starts at 0"), After.Sections[0].FirstIndex, 0);
		}
	}
	TestTrue(TEXT("every LOD counted in the total"), Look.Stats.Hidden > Look.Stats.HiddenLOD0 && Look.Stats.HiddenLOD0 > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleLayerHideOpenTest,
	"Mantle.Runtime.LayerFit.HideNothingWhereOpen",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleLayerHideOpenTest::RunTest(const FString& Parameters)
{
	using namespace MantleLayerHideTestPrivate;
	// Where the jacket is open (the front: steps 36 to 47 and 0, a quarter turn) or does not reach (above its top hem, below its
	// bottom hem), nothing of the shirt is cut and no point moves.
	FShirtUnderJacket Look;
	if (!Look.Run(*this))
	{
		return false;
	}
	for (int32 LODIndex = 0; LODIndex < 2; ++LODIndex)
	{
		const FMantleSkinnedMeshLOD& Before = Look.Shirt->LODs[LODIndex];
		const FMantleSkinnedMeshLOD& After = Look.Shown[0]->LODs[LODIndex];
		const int32 Around = LODIndex == 0 ? ShirtAround : ShirtAround / 2;
		const int32 Scale = ShirtAround / Around;
		const TSet<FIntVector> Kept = KeptTriangles(After);
		int32 InOpening = 0;
		int32 InOpeningCut = 0;
		int32 OutsideReach = 0;
		int32 OutsideReachCut = 0;
		for (int32 Triangle = 0; Triangle < Before.NumTriangles(); ++Triangle)
		{
			bool bOpening = false;
			bool bBeyond = false;
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const FVector3f& P = Before.Positions[Before.Indices[Triangle * 3 + Corner]];
				const int32 Step = StepOf(P, Around) * Scale;
				bOpening |= Step >= 37 || Step == 0;
				bBeyond |= RingOf(P) <= -11 || RingOf(P) >= 10;
			}
			const bool bKept = Kept.Contains(Triple(Before, Triangle));
			InOpening += bOpening ? 1 : 0;
			InOpeningCut += (bOpening && !bKept) ? 1 : 0;
			OutsideReach += bBeyond ? 1 : 0;
			OutsideReachCut += (bBeyond && !bKept) ? 1 : 0;
		}
		TestTrue(*FString::Printf(TEXT("LOD%d: there are triangles in the opening (the premise)"), LODIndex), InOpening > 0);
		TestEqual(*FString::Printf(TEXT("LOD%d: triangles at the open front that were cut"), LODIndex), InOpeningCut, 0);
		TestTrue(*FString::Printf(TEXT("LOD%d: there are triangles past the hems (the premise)"), LODIndex), OutsideReach > 0);
		TestEqual(*FString::Printf(TEXT("LOD%d: triangles past the hems that were cut"), LODIndex), OutsideReachCut, 0);
		int32 MovedAtOpening = 0;
		for (int32 Vertex = 0; Vertex < Before.NumVertices(); ++Vertex)
		{
			const int32 Step = StepOf(Before.Positions[Vertex], Around) * Scale;
			const int32 Ring = RingOf(Before.Positions[Vertex]);
			const bool bUncovered = Step >= 37 || Step == 0 || Ring <= -11 || Ring >= 10;
			MovedAtOpening += (bUncovered && After.Positions[Vertex] != Before.Positions[Vertex]) ? 1 : 0;
		}
		TestEqual(*FString::Printf(TEXT("LOD%d: points the jacket does not cover that moved"), LODIndex), MovedAtOpening, 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleLayerHideSinkTest,
	"Mantle.Runtime.LayerFit.SinkAtTheEdges",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleLayerHideSinkTest::RunTest(const FString& Parameters)
{
	using namespace MantleLayerHideTestPrivate;
	// The covered points of the shirt that stay (the band at the jacket's edges, and the ring beside the cut) end 0.5 cm under the
	// jacket: the jacket is 0.35 cm out, so about 0.17 cm in from the shirt's 10 cm, whatever the quarter-step sag of the jacket's
	// sides makes it; never 1 cm. Points the jacket does not cover, and points no triangle uses any more, do not move. Step 36 is the
	// one column the jacket's last column covers (the arc's back edge is at step 36.25): it sinks too, its depth not asserted.
	FShirtUnderJacket Look;
	if (!Look.Run(*this))
	{
		return false;
	}
	const FMantleSkinnedMeshLOD& Before = Look.Shirt->LODs[0];
	const FMantleSkinnedMeshLOD& After = Look.Shown[0]->LODs[0];
	TBitArray<> Used(false, Before.NumVertices());
	for (const uint32 Index : After.Indices)
	{
		Used[static_cast<int32>(Index)] = true;
	}
	int32 SunkChecked = 0;
	int32 WrongDepth = 0;
	int32 MovedButNotMeant = 0;
	float DeepestSink = 0.0f;
	for (int32 Vertex = 0; Vertex < Before.NumVertices(); ++Vertex)
	{
		const int32 Step = StepOf(Before.Positions[Vertex], ShirtAround);
		const int32 Ring = RingOf(Before.Positions[Vertex]);
		const bool bCovered = Step >= 1 && Step <= 35 && Ring >= -10 && Ring <= 9;
		const bool bLastColumn = Step == 36 && Ring >= -10 && Ring <= 9;
		if (bLastColumn)
		{
			DeepestSink = FMath::Max(DeepestSink, ShirtRadius - Radial(After.Positions[Vertex]));
		}
		else if (bCovered && Used[Vertex])
		{
			++SunkChecked;
			const float Radius = Radial(After.Positions[Vertex]);
			WrongDepth += (Radius < 9.80f || Radius > 9.90f) ? 1 : 0;
			DeepestSink = FMath::Max(DeepestSink, ShirtRadius - Radius);
		}
		else
		{
			if (After.Positions[Vertex] != Before.Positions[Vertex])
			{
				if (++MovedButNotMeant <= 6)
				{
					AddInfo(FString::Printf(TEXT("moved without being covered: ring %d step %d (%s -> %s)"), Ring, Step, *Before.Positions[Vertex].ToString(), *After.Positions[Vertex].ToString()));
				}
			}
		}
	}
	TestTrue(TEXT("covered points of kept triangles exist (the premise)"), SunkChecked > 100);
	TestEqual(TEXT("covered kept points not 0.5 cm under the jacket (9.80 to 9.90 cm from the axis)"), WrongDepth, 0);
	TestTrue(*FString::Printf(TEXT("the deepest sink is under 1 cm (%.3f)"), DeepestSink), DeepestSink <= FMantleLayerFit::SinkMaxCm + 0.001f);
	TestEqual(TEXT("points that moved without being covered and kept"), MovedButNotMeant, 0);
	TestTrue(TEXT("the stats count the sunk points"), Look.Stats.Sunk >= SunkChecked);
	TestTrue(*FString::Printf(TEXT("the stats' deepest sink agrees (%.3f)"), Look.Stats.MaxSunkCm), FMath::Abs(Look.Stats.MaxSunkCm - DeepestSink) < 0.05);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleLayerHideSingleTest,
	"Mantle.Runtime.LayerFit.HideSingleUnchanged",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleLayerHideSingleTest::RunTest(const FString& Parameters)
{
	using namespace MantleLayerHideTestPrivate;
	// One garment, nothing over it: the same mesh comes back, nothing cut, nothing sunk. So does a garment with nothing over it
	// in the stack (the jacket), one far from the garment over it (100 cm up), and one with only a lower rank near it.
	const TSharedPtr<const FMantleSkinnedMesh> Shirt = MakeShirt();
	const TSharedPtr<const FMantleSkinnedMesh> Jacket = MakeOpenJacket();
	TArray<TSharedPtr<const FMantleSkinnedMesh>> Placed;
	TArray<TSharedPtr<const FMantleSkinnedMesh>> Shown;
	FMantleLayerFitStats Stats;
	{
		TArray<FMantleLayerGarment> Garments;
		Garments.Add(MakeLayer(Shirt, 5));
		FMantleLayerFit::Apply(Garments, Placed, Stats, &Shown);
		TestTrue(TEXT("one: shown is the same mesh"), Shown.Num() == 1 && Shown[0] == Shirt);
		TestEqual(TEXT("one: cut"), Stats.Hidden, 0);
		TestEqual(TEXT("one: sunk"), Stats.Sunk, 0);
		TestEqual(TEXT("one: edited"), Stats.Edited, 0);
		TestEqual(TEXT("one: pairs"), Stats.HidePairs, 0);
	}
	{
		// Without OutShown the pass cuts nothing at all.
		TArray<FMantleLayerGarment> Garments;
		Garments.Add(MakeLayer(Shirt, 5));
		Garments.Add(MakeLayer(Jacket, 6));
		FMantleLayerFit::Apply(Garments, Placed, Stats);
		TestEqual(TEXT("no OutShown: pairs of the hide step"), Stats.HidePairs, 0);
		TestEqual(TEXT("no OutShown: cut"), Stats.Hidden, 0);
		TestTrue(TEXT("no OutShown: both placed as they were"), Placed.Num() == 2 && Placed[0] == Shirt && Placed[1] == Jacket);
	}
	{
		FTubeSpec Far;
		Far.Phase = 0.25f;
		Far.Z0 = 80.0f;
		Far.Rings = 21;
		Far.Radius = ShirtRadius + JacketOutCm;
		TArray<FMantleSkinnedMeshLOD> LODs;
		LODs.Add(MakeTube(Far));
		const TSharedPtr<const FMantleSkinnedMesh> FarJacket = MakeGarment(MoveTemp(LODs));
		TArray<FMantleLayerGarment> Garments;
		Garments.Add(MakeLayer(Shirt, 5));
		Garments.Add(MakeLayer(FarJacket, 6));
		FMantleLayerFit::Apply(Garments, Placed, Stats, &Shown);
		TestTrue(TEXT("far: both shown as they were"), Shown.Num() == 2 && Shown[0] == Shirt && Shown[1] == FarJacket);
		TestEqual(TEXT("far: pairs"), Stats.HidePairs, 0);
	}
	{
		// Two garments of the same rank are neither over the other.
		TArray<FMantleLayerGarment> Garments;
		Garments.Add(MakeLayer(Shirt, 5));
		Garments.Add(MakeLayer(Jacket, 5));
		FMantleLayerFit::Apply(Garments, Placed, Stats, &Shown);
		TestTrue(TEXT("same rank: both shown as they were"), Shown.Num() == 2 && Shown[0] == Shirt && Shown[1] == Jacket);
		TestEqual(TEXT("same rank: cut"), Stats.Hidden, 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleLayerHideNailsTest,
	"Mantle.Runtime.LayerFit.NailsUnderGloves",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleLayerHideNailsTest::RunTest(const FString& Parameters)
{
	using namespace MantleLayerHideTestPrivate;
	// Nails (an overlay, rank under gloves) on a finger: a tube of radius 1 cm, 12 sides, z 0 to 4. The glove is a tube 0.1 cm
	// outside it (a gap a push would widen to 0.3 if the nails were a layer). Nails are hidden entirely where the glove covers
	// them, edges and all; a top over them that does not cover overlays hides nothing.
	const auto MakeFingerTube = [](const float Phase, const float Z0, const int32 Rings, const float Radius)
	{
		FTubeSpec Spec;
		Spec.Around = 12;
		Spec.Phase = Phase;
		Spec.Z0 = Z0;
		Spec.Rings = Rings;
		Spec.Radius = Radius;
		TArray<FMantleSkinnedMeshLOD> LODs;
		LODs.Add(MakeTube(Spec));
		return MakeGarment(MoveTemp(LODs));
	};
	const TSharedPtr<const FMantleSkinnedMesh> Nails = MakeFingerTube(0.25f, 0.0f, 5, 1.0f);

	// The glove covers all of the nails: nothing of them is left to see, and what stays is one tiny triangle (a mesh needs one).
	{
		const TSharedPtr<const FMantleSkinnedMesh> Glove = MakeFingerTube(0.0f, -2.1f, 10, 1.1f);
		TArray<FMantleLayerGarment> Garments;
		Garments.Add(MakeLayer(Nails, 480, /*bOverlay*/ true));
		Garments.Add(MakeLayer(Glove, 500, false, /*bCoversOverlay*/ true));
		TArray<TSharedPtr<const FMantleSkinnedMesh>> Placed;
		TArray<TSharedPtr<const FMantleSkinnedMesh>> Shown;
		FMantleLayerFitStats Stats;
		FMantleLayerFit::Apply(Garments, Placed, Stats, &Shown);
		if (!TestEqual(TEXT("all: one shown per garment"), Shown.Num(), 2) || !Shown[0].IsValid() || !Shown[1].IsValid())
		{
			return false;
		}
		TestTrue(TEXT("all: the glove is not pushed out of the nails (overlays are not layers)"), Placed[1] == Glove && Stats.Pushed == 0 && Stats.Pairs == 0);
		TestTrue(TEXT("all: the glove shown as it was"), Shown[1] == Glove);
		TestTrue(TEXT("all: the nails shown are an edited copy"), Shown[0] != Nails && Placed[0] == Nails);
		if (Shown[0] != Nails && Shown[0]->LODs.Num() == 1)
		{
			const FMantleSkinnedMeshLOD& LOD = Shown[0]->LODs[0];
			TestEqual(TEXT("all: every nail triangle was cut, one tiny one stays"), LOD.Indices.Num(), 3);
			TestEqual(TEXT("all: the stats count the nail triangles"), Stats.HiddenLOD0, Nails->LODs[0].NumTriangles());
			if (TestEqual(TEXT("all: the section is still there"), LOD.Sections.Num(), 1))
			{
				TestEqual(TEXT("all: the section holds the one triangle"), LOD.Sections[0].IndexCount, 3);
				TestEqual(TEXT("all: the section starts at 0"), LOD.Sections[0].FirstIndex, 0);
			}
			if (LOD.Indices.Num() == 3)
			{
				const FVector3f& A = LOD.Positions[LOD.Indices[0]];
				const FVector3f& B = LOD.Positions[LOD.Indices[1]];
				const FVector3f& C = LOD.Positions[LOD.Indices[2]];
				TestTrue(TEXT("all: the triangle left is a hundredth of a centimetre across"), (B - A).Size() < 0.02f && (C - A).Size() < 0.02f);
			}
		}
	}

	// The glove covers the upper part (z 1.9 up): the nail triangles with all three corners at z 2 or more are cut, the rest stay.
	{
		const TSharedPtr<const FMantleSkinnedMesh> Glove = MakeFingerTube(0.0f, 1.9f, 7, 1.1f);
		TArray<FMantleLayerGarment> Garments;
		Garments.Add(MakeLayer(Nails, 480, true));
		Garments.Add(MakeLayer(Glove, 500, false, true));
		TArray<TSharedPtr<const FMantleSkinnedMesh>> Placed;
		TArray<TSharedPtr<const FMantleSkinnedMesh>> Shown;
		FMantleLayerFitStats Stats;
		FMantleLayerFit::Apply(Garments, Placed, Stats, &Shown);
		if (!TestEqual(TEXT("part: one shown per garment"), Shown.Num(), 2) || !Shown[0].IsValid() || Shown[0] == Nails)
		{
			return TestTrue(TEXT("part: the nails were edited"), false);
		}
		const FMantleSkinnedMeshLOD& Before = Nails->LODs[0];
		const FMantleSkinnedMeshLOD& After = Shown[0]->LODs[0];
		const TSet<FIntVector> Kept = KeptTriangles(After);
		int32 Cut = 0;
		int32 CoveredStayed = 0;
		int32 UncoveredCut = 0;
		for (int32 Triangle = 0; Triangle < Before.NumTriangles(); ++Triangle)
		{
			int32 LowestRing = 100;
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				LowestRing = FMath::Min(LowestRing, RingOf(Before.Positions[Before.Indices[Triangle * 3 + Corner]]));
			}
			const bool bKept = Kept.Contains(Triple(Before, Triangle));
			Cut += bKept ? 0 : 1;
			CoveredStayed += (LowestRing >= 2 && bKept) ? 1 : 0;
			UncoveredCut += (LowestRing < 2 && !bKept) ? 1 : 0;
		}
		TestEqual(TEXT("part: nail triangles all covered that stayed (no band at the glove's edge)"), CoveredStayed, 0);
		TestEqual(TEXT("part: nail triangles with an uncovered corner that were cut"), UncoveredCut, 0);
		TestEqual(TEXT("part: two bands of 12 quads were cut"), Cut, 48);
		TestEqual(TEXT("part: the section follows"), After.Sections.Num() == 1 ? After.Sections[0].IndexCount : -1, 48 * 3);
		TestEqual(TEXT("part: the index list follows"), After.Indices.Num(), 48 * 3);
		int32 MovedBelow = 0;
		for (int32 Vertex = 0; Vertex < Before.NumVertices(); ++Vertex)
		{
			MovedBelow += (RingOf(Before.Positions[Vertex]) <= 1 && After.Positions[Vertex] != Before.Positions[Vertex]) ? 1 : 0;
		}
		TestEqual(TEXT("part: nail points below the glove that moved"), MovedBelow, 0);
	}

	// A top over the nails is not a glove: it covers no overlay, so the nails come back as they were.
	{
		const TSharedPtr<const FMantleSkinnedMesh> Sleeve = MakeFingerTube(0.0f, -2.1f, 10, 1.1f);
		TArray<FMantleLayerGarment> Garments;
		Garments.Add(MakeLayer(Nails, 480, true));
		Garments.Add(MakeLayer(Sleeve, 600));
		TArray<TSharedPtr<const FMantleSkinnedMesh>> Placed;
		TArray<TSharedPtr<const FMantleSkinnedMesh>> Shown;
		FMantleLayerFitStats Stats;
		FMantleLayerFit::Apply(Garments, Placed, Stats, &Shown);
		TestTrue(TEXT("top: both shown as they were"), Shown.Num() == 2 && Shown[0] == Nails && Shown[1] == Sleeve);
		TestEqual(TEXT("top: cut"), Stats.Hidden, 0);
	}
	return true;
}

#endif
