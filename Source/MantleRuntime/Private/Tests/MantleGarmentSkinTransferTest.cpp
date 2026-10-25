// Copyright Joshua Eger. All Rights Reserved.
#include "MantleGarmentSkinTransfer.h"

#include "MantleGarmentClearance.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// The skin transfer's collar weights: an unrigged garment (one slot, all root) over a one-triangle body that is all
// spine_04, with a one-triangle face whose skin is neck_01 and a face-only neck joint under it. The clearances are
// written by hand, so the face's share of each vertex is known: dB / (dB + dF), faded over the face's last FaceFadeCm
// (three loose vertices, so the smoothing over the garment's edges leaves it as it is). The collar ring checks that
// smoothing: a strip of quads around a neck whose raw shares jump from vertex to vertex.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleGarmentSkinTransferTestPrivate
{
	enum EBone : uint16 { Root = 0, Spine04 = 1, Neck01 = 2 };

	/** One triangle of three vertices at Z, NumInfluences slots, every vertex weighted Bones / Weights. */
	FMantleSkinnedMeshLOD Triangle(const float Z, const int32 NumInfluences, const TArray<uint16>& Bones, const TArray<uint8>& Weights)
	{
		FMantleSkinnedMeshLOD LOD;
		LOD.Positions = { FVector3f(0.0f, 0.0f, Z), FVector3f(10.0f, 0.0f, Z), FVector3f(0.0f, 10.0f, Z) };
		LOD.Normals.Init(FVector3f(0.0f, 0.0f, 1.0f), 3);
		LOD.UVs.Init(FVector2f::ZeroVector, 3);
		LOD.Indices = { 0, 1, 2 };
		LOD.NumInfluences = NumInfluences;
		LOD.InfluenceBones.SetNumZeroed(3 * NumInfluences);
		LOD.InfluenceWeights.SetNumZeroed(3 * NumInfluences);
		for (int32 Vertex = 0; Vertex < 3; ++Vertex)
		{
			for (int32 Slot = 0; Slot < Bones.Num(); ++Slot)
			{
				LOD.InfluenceBones[Vertex * NumInfluences + Slot] = Bones[Slot];
				LOD.InfluenceWeights[Vertex * NumInfluences + Slot] = Weights[Slot];
			}
		}
		return LOD;
	}

	/** A clearance over a three-vertex triangle: per garment vertex tracked at its own corner (or not, ClearanceCm < 0 here meaning untracked). */
	FMantleGarmentClearance Clearance(const TArray<float>& ClearanceCm)
	{
		FMantleGarmentClearance Out;
		Out.SurfaceIndices = { 0, 1, 2 };
		Out.TriangleSign = { 1 };
		Out.NumSurfaceVertices = 3;
		for (int32 Vertex = 0; Vertex < ClearanceCm.Num(); ++Vertex)
		{
			const bool bTracked = ClearanceCm[Vertex] >= 0.0f;
			Out.Triangle.Add(bTracked ? 0 : INDEX_NONE);
			FVector3f Bary = FVector3f::ZeroVector;
			Bary[Vertex % 3] = 1.0f;
			Out.Barycentrics.Add(Bary);
			Out.ClearanceCm.Add(bTracked ? ClearanceCm[Vertex] : 0.0f);
			Out.NumTracked += bTracked ? 1 : 0;
		}
		return Out;
	}

	/** Bone and weight of each used slot of Vertex. */
	TMap<uint16, int32> WeightsOf(const FMantleSkinnedMeshLOD& LOD, const int32 Vertex)
	{
		TMap<uint16, int32> Out;
		for (int32 Slot = 0; Slot < LOD.NumInfluences; ++Slot)
		{
			const uint8 Weight = LOD.InfluenceWeights[Vertex * LOD.NumInfluences + Slot];
			if (Weight > 0)
			{
				Out.FindOrAdd(LOD.InfluenceBones[Vertex * LOD.NumInfluences + Slot]) += Weight;
			}
		}
		return Out;
	}

	int32 Sum(const TMap<uint16, int32>& Weights)
	{
		int32 Total = 0;
		for (const TPair<uint16, int32>& Pair : Weights)
		{
			Total += Pair.Value;
		}
		return Total;
	}

	TArray<FMantleSkinnedMeshBone> BodyBones()
	{
		return { { TEXT("root"), INDEX_NONE }, { TEXT("spine_04"), 0 }, { TEXT("neck_01"), 1 } };
	}

	/** Vertices around the collar ring. */
	constexpr int32 RingCount = 24;
	constexpr float RingRadiusCm = 7.0f;

	/**
	 * An unrigged collar strip (one slot, all root): the rim (vertices 0..23, Z 1) over a lower row (24..47, Z 0),
	 * both of radius RingRadiusCm around the Z axis, joined by quads.
	 */
	FMantleSkinnedMeshLOD CollarRing()
	{
		FMantleSkinnedMeshLOD LOD;
		for (int32 Row = 0; Row < 2; ++Row)
		{
			for (int32 Index = 0; Index < RingCount; ++Index)
			{
				const float Angle = 2.0f * UE_PI * static_cast<float>(Index) / static_cast<float>(RingCount);
				LOD.Positions.Add(FVector3f(RingRadiusCm * FMath::Cos(Angle), RingRadiusCm * FMath::Sin(Angle), Row == 0 ? 1.0f : 0.0f));
			}
		}
		for (int32 Index = 0; Index < RingCount; ++Index)
		{
			const uint32 A = static_cast<uint32>(Index);
			const uint32 B = static_cast<uint32>((Index + 1) % RingCount);
			LOD.Indices.Append({ A, A + RingCount, B, B, A + RingCount, B + RingCount });
		}
		LOD.Normals.Init(FVector3f(1.0f, 0.0f, 0.0f), 2 * RingCount);
		LOD.UVs.Init(FVector2f::ZeroVector, 2 * RingCount);
		LOD.NumInfluences = 1;
		LOD.InfluenceBones.Init(Root, 2 * RingCount);
		LOD.InfluenceWeights.Init(255, 2 * RingCount);
		return LOD;
	}

	/** Neck_01's weight of Vertex, 0..1. */
	float NeckShare(const FMantleSkinnedMeshLOD& LOD, const int32 Vertex)
	{
		return static_cast<float>(WeightsOf(LOD, Vertex).FindRef(Neck01)) / 255.0f;
	}

	/**
	 * The rim skinned with neck_01 tipped forward by Degrees about the X axis through (0, 0, -3) (spine_04 stays):
	 * the largest bump in its radius about the posed rim's centre, |r(i) - (r(i - 1) + r(i + 1)) / 2|, in cm.
	 */
	float PosedRimBump(const FMantleSkinnedMeshLOD& LOD, const float Degrees)
	{
		const FQuat4f Tip(FVector3f(1.0f, 0.0f, 0.0f), FMath::DegreesToRadians(Degrees));
		const FVector3f Pivot(0.0f, 0.0f, -3.0f);
		TArray<FVector3f> Posed;
		FVector3f Centre = FVector3f::ZeroVector;
		for (int32 Index = 0; Index < RingCount; ++Index)
		{
			const FVector3f P = LOD.Positions[Index];
			const float Neck = NeckShare(LOD, Index);
			Posed.Add((1.0f - Neck) * P + Neck * (Pivot + Tip.RotateVector(P - Pivot)));
			Centre += Posed.Last() / static_cast<float>(RingCount);
		}
		float Bump = 0.0f;
		for (int32 Index = 0; Index < RingCount; ++Index)
		{
			const float Here = FVector3f::Dist(Posed[Index], Centre);
			const float Before = FVector3f::Dist(Posed[(Index + RingCount - 1) % RingCount], Centre);
			const float After = FVector3f::Dist(Posed[(Index + 1) % RingCount], Centre);
			Bump = FMath::Max(Bump, FMath::Abs(Here - 0.5f * (Before + After)));
		}
		return Bump;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleGarmentSkinTransferCollarTest,
	"Mantle.Runtime.GarmentSkinTransfer.CollarTakesFaceWeights",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleGarmentSkinTransferCollarTest::RunTest(const FString& Parameters)
{
	using namespace MantleGarmentSkinTransferTestPrivate;

	FMantleSkinnedMesh Wearer;
	Wearer.Bones = BodyBones();
	Wearer.LODs.Add(Triangle(0.0f, 4, { Spine04 }, { 255 }));

	// The face's table differs from the body's, and its neck joint (face bone 3) is not on the garment: it rides on neck_01.
	const TArray<FMantleSkinnedMeshBone> FaceBones = { { TEXT("root"), INDEX_NONE }, { TEXT("spine_04"), 0 }, { TEXT("neck_01"), 1 }, { TEXT("FACIAL_C_NeckBackB"), 2 } };
	const FMantleSkinnedMeshLOD FaceLOD = Triangle(0.0f, 8, { 2, 3 }, { 128, 127 });

	// Three loose vertices: no edges, so each keeps its own share.
	FMantleSkinnedMeshLOD Garment = Triangle(1.0f, 1, { Root }, { 255 });
	Garment.Indices.Reset();
	const FMantleGarmentClearance Authored = Clearance({ 2.0f, 2.0f, 2.0f });
	// Vertex 0 lies 0.5 cm off the face (share 2 / 2.5 = 0.8), vertex 1 is past its reach, vertex 2 at 5.5 cm (faded to 1/4 of 2 / 7.5).
	const FMantleGarmentClearance FaceClearance = Clearance({ 0.5f, -1.0f, 5.5f });

	FMantleSkinTransferFace Face;
	Face.Surface = &FaceLOD;
	Face.Bones = FaceBones;
	Face.Clearance = &FaceClearance;
	FMantleSkinTransferStats Stats;
	FString Error;
	const TArray<FMantleSkinnedMeshBone> GarmentBones = BodyBones();
	if (!TestTrue(TEXT("the transfer runs"), FMantleGarmentSkinTransfer::Transfer(Garment, GarmentBones, Authored, Wearer, Face, &Stats, &Error)))
	{
		AddError(Error);
		return false;
	}

	TestEqual(TEXT("the one-slot garment grows to the two bones its blends use"), Garment.NumInfluences, 2);
	TestEqual(TEXT("the stats say so"), Stats.Influences, 2);
	TestEqual(TEXT("slots before"), Stats.SlotsBefore, 1);
	TestEqual(TEXT("every vertex from the skin"), Stats.FromBody, 3);
	TestEqual(TEXT("two took the face's weights"), Stats.FromFace, 2);
	TestEqual(TEXT("the face's neck joint mapped to neck_01, nothing dropped"), Stats.DroppedInfluences, 0);

	const TMap<uint16, int32> Collar = WeightsOf(Garment, 0);
	TestEqual(TEXT("the collar vertex sums to 255"), Sum(Collar), 255);
	TestEqual(TEXT("the collar vertex is 80% neck_01"), Collar.FindRef(Neck01), 204);
	TestEqual(TEXT("and 20% spine_04"), Collar.FindRef(Spine04), 51);
	TestFalse(TEXT("and no root left"), Collar.Contains(Root));

	const TMap<uint16, int32> Shoulder = WeightsOf(Garment, 1);
	TestEqual(TEXT("past the face: the body's weights only"), Shoulder.FindRef(Spine04), 255);
	TestEqual(TEXT("and only one bone"), Shoulder.Num(), 1);

	const TMap<uint16, int32> Edge = WeightsOf(Garment, 2);
	TestEqual(TEXT("at the face's edge the vertex sums to 255"), Sum(Edge), 255);
	TestTrue(TEXT("and mostly the body's"), Edge.FindRef(Spine04) > 230 && Edge.FindRef(Neck01) > 0);

	// Without the face the same garment stays on the chest, the old behaviour.
	FMantleSkinnedMeshLOD BodyOnly = Triangle(1.0f, 1, { Root }, { 255 });
	TestTrue(TEXT("the body-only transfer runs"), FMantleGarmentSkinTransfer::Transfer(BodyOnly, GarmentBones, Authored, Wearer));
	TestEqual(TEXT("body only: the collar vertex is all spine_04"), WeightsOf(BodyOnly, 0).FindRef(Spine04), 255);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleGarmentSkinTransferRingTest,
	"Mantle.Runtime.GarmentSkinTransfer.CollarRingStaysRound",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleGarmentSkinTransferRingTest::RunTest(const FString& Parameters)
{
	using namespace MantleGarmentSkinTransferTestPrivate;

	FMantleSkinnedMesh Wearer;
	Wearer.Bones = BodyBones();
	Wearer.LODs.Add(Triangle(0.0f, 4, { Spine04 }, { 255 }));
	const TArray<FMantleSkinnedMeshBone> GarmentBones = BodyBones();
	const FMantleSkinnedMeshLOD FaceLOD = Triangle(0.0f, 8, { Neck01 }, { 255 });

	// A snug collar at the face's lower edge: the rim over the face at uneven distances, every few vertices past
	// its open boundary (untracked, -1); the row below mostly past it. The raw shares jump between about 0.8 and 0
	// from one rim vertex to the next, so the per-vertex share of #961 left this rim wavy (largest step 0.82,
	// posed bump 1.2 cm, worked out by hand from the same numbers).
	const TArray<float> RimFaceCm = { 0.4f, -1.0f, 0.6f, 2.5f, 0.3f, -1.0f, 0.8f, 4.0f, 0.5f, 1.8f, -1.0f, 0.4f, 0.7f, -1.0f, 0.3f, 3.0f, 0.5f, -1.0f, 0.9f, 0.4f, 2.2f, -1.0f, 0.6f, 0.5f };
	const TArray<float> LowFaceCm = { -1.0f, 3.5f, -1.0f, -1.0f, 4.5f, -1.0f, -1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 5.0f, -1.0f, -1.0f, 4.0f, -1.0f, -1.0f, -1.0f, 3.8f, -1.0f, -1.0f, -1.0f, 4.2f, -1.0f };
	const TArray<float> RimBodyCm = { 1.0f, 0.6f, 1.4f, 0.8f, 1.2f, 0.5f, 1.0f, 1.5f, 0.7f, 1.1f, 0.9f, 1.3f };
	TArray<float> FaceCm = RimFaceCm;
	FaceCm.Append(LowFaceCm);
	TArray<float> BodyCm;
	for (int32 Index = 0; Index < 2 * RingCount; ++Index)
	{
		BodyCm.Add(Index < RingCount ? RimBodyCm[Index % RimBodyCm.Num()] : 0.5f);
	}
	const FMantleGarmentClearance Authored = Clearance(BodyCm);
	const FMantleGarmentClearance FaceClearance = Clearance(FaceCm);

	FMantleSkinTransferFace Face;
	Face.Surface = &FaceLOD;
	Face.Bones = GarmentBones;
	Face.Clearance = &FaceClearance;
	FMantleSkinnedMeshLOD Garment = CollarRing();
	FMantleSkinTransferStats Stats;
	FString Error;
	TArray<float> FaceShare;
	if (!TestTrue(TEXT("the transfer runs"), FMantleGarmentSkinTransfer::Transfer(Garment, GarmentBones, Authored, Wearer, Face, &Stats, &Error, &FaceShare)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("a face share per vertex"), FaceShare.Num(), 2 * RingCount);
	TestEqual(TEXT("the unrigged collar grows to its two bones"), Garment.NumInfluences, 2);
	TestEqual(TEXT("every vertex took some of the face's weights, the untracked ones through their neighbours"), Stats.FromFace, 2 * RingCount);

	float LargestStep = 0.0f;
	float Lowest = 1.0f;
	float Mean = 0.0f;
	for (int32 Index = 0; Index < RingCount; ++Index)
	{
		const float Neck = NeckShare(Garment, Index);
		LargestStep = FMath::Max(LargestStep, FMath::Abs(Neck - NeckShare(Garment, (Index + 1) % RingCount)));
		Lowest = FMath::Min(Lowest, Neck);
		Mean += Neck / static_cast<float>(RingCount);
		TestEqual(FString::Printf(TEXT("rim vertex %d sums to 255"), Index), Sum(WeightsOf(Garment, Index)), 255);
	}
	AddInfo(FString::Printf(TEXT("rim neck_01 share: mean %.3f, lowest %.3f, largest step between neighbours %.3f"), Mean, Lowest, LargestStep));
	TestTrue(TEXT("neighbouring rim vertices get about the same neck share"), LargestStep < 0.15f);
	TestTrue(TEXT("the collar still follows the neck: a substantial neck share on average"), Mean > 0.3f);
	TestTrue(TEXT("and on every rim vertex, the untracked ones too"), Lowest > 0.25f);

	const float Bump = PosedRimBump(Garment, 25.0f);
	AddInfo(FString::Printf(TEXT("posed rim (neck_01 tipped 25 degrees): largest radius bump %.3f cm"), Bump));
	TestTrue(TEXT("the posed rim stays round"), Bump < 0.15f);

	// The weight smoothing covers the collar although it lies on the skin (authored clearance at or below 1 cm,
	// which alone keeps the skin's exact weights): FaceSmoothFactor from a share of FaceSmoothFullShare up.
	TArray<float> Factor;
	const FMantleGarmentClearance Snug = Clearance({ 0.5f, 0.5f, 0.5f, 2.0f });
	const TArray<float> Shares = { 0.0f, 0.1f, 0.6f, 0.6f };
	FMantleGarmentSkinTransfer::SmoothFactor(Snug, FMantleGarmentSkinTransfer::DefaultSmoothTightCm, FMantleGarmentSkinTransfer::DefaultSmoothLooseCm, Factor, &Shares);
	TestEqual(TEXT("tight, no face share: not smoothed"), Factor[0], 0.0f);
	TestEqual(TEXT("tight, a small face share: a little"), Factor[1], 0.2f, 1.0e-4f);
	TestEqual(TEXT("tight collar: FaceSmoothFactor"), Factor[2], FMantleGarmentSkinTransfer::FaceSmoothFactor, 1.0e-4f);
	TestEqual(TEXT("looser than that: its own factor"), Factor[3], 0.5f, 1.0e-4f);
	FMantleGarmentSkinTransfer::SmoothFactor(Snug, FMantleGarmentSkinTransfer::DefaultSmoothTightCm, FMantleGarmentSkinTransfer::DefaultSmoothLooseCm, Factor);
	TestEqual(TEXT("without the shares the tight collar is not smoothed, as before"), Factor[2], 0.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleGarmentSkinTransferSlotsTest,
	"Mantle.Runtime.GarmentSkinTransfer.UnriggedGarmentKeepsTheBlend",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleGarmentSkinTransferSlotsTest::RunTest(const FString& Parameters)
{
	using namespace MantleGarmentSkinTransferTestPrivate;

	// A body blended over three bones; the unrigged garment (one slot) used to keep only the heaviest.
	FMantleSkinnedMesh Wearer;
	Wearer.Bones = BodyBones();
	Wearer.LODs.Add(Triangle(0.0f, 4, { Spine04, Neck01, Root }, { 153, 77, 25 }));
	FMantleSkinnedMeshLOD Garment = Triangle(1.0f, 1, { Root }, { 255 });
	const FMantleGarmentClearance Authored = Clearance({ 1.0f, 1.0f, 1.0f });
	FMantleSkinTransferStats Stats;
	if (!TestTrue(TEXT("the transfer runs"), FMantleGarmentSkinTransfer::Transfer(Garment, BodyBones(), Authored, Wearer, FMantleSkinTransferFace(), &Stats)))
	{
		return false;
	}
	TestTrue(TEXT("more than one influence"), Garment.NumInfluences > 1);
	TestEqual(TEXT("as many slots as the blend has bones"), Garment.NumInfluences, 3);
	const TMap<uint16, int32> Weights = WeightsOf(Garment, 0);
	TestEqual(TEXT("spine_04 kept"), Weights.FindRef(Spine04), 153);
	TestEqual(TEXT("neck_01 kept"), Weights.FindRef(Neck01), 77);
	TestEqual(TEXT("root kept"), Weights.FindRef(Root), 25);

	// Smoothing works on the wider slots and keeps every vertex at 255.
	TestTrue(TEXT("the smoothing runs on the grown slots"), FMantleGarmentSkinTransfer::Smooth(Garment, 3));
	for (int32 Vertex = 0; Vertex < 3; ++Vertex)
	{
		TestEqual(FString::Printf(TEXT("vertex %d sums to 255 after the smoothing"), Vertex), Sum(WeightsOf(Garment, Vertex)), 255);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
