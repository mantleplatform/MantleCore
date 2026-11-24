// Copyright Joshua Eger. All Rights Reserved.
#include "MantleBodyCoverage.h"

#include "MantleSkinnedMesh.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// The body sink with several garments (#65 layers), on synthetic tubes: an open tube body (radius 8 cm, rings 1 cm apart from
// z -30 to 30) under a tee (0.3 cm off the skin, z -20.25 to 5.25) and a jacket over it (3 cm off the skin, z -5.25 to 20.25).
// The garments' vertices are turned a quarter step and their rings lie a quarter ring off the body's, so a ray from a body
// vertex goes through the middle of a garment triangle, not along an edge or a diagonal. No assets.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleBodySinkLayersTestPrivate
{
	constexpr float LayerBodyRadius = 8.0f;
	constexpr int32 LayerBodyRings = 61;
	constexpr float LayerBodyHalfLength = 30.0f;
	constexpr int32 LayerAround = 48;

	/** The skin-mask rule's sink (0.5 cm under the cloth, at most 1 cm, 4 smoothing passes): what the dressed body uses. */
	FMantleBodySinkParams LayerSinkParams()
	{
		return FMantleDressedBodyParams::SkinMaskDefaults().Sink;
	}

	/** One surface: a body or a garment. */
	struct FLayerMesh
	{
		TArray<FVector3f> Positions;
		TArray<FVector3f> Normals;
		TArray<uint32> Indices;

		FMantleGarmentSurface Surface() const { return FMantleGarmentSurface(Positions, Normals, Indices); }
	};

	/** A tube around Z of Radius from Z0 to Z1 in Rings rings of LayerAround vertices, normals and winding outward, turned Phase steps around. */
	void AddLayerTube(FLayerMesh& Mesh, const float Radius, const float Z0, const float Z1, const int32 Rings, const float Phase)
	{
		const uint32 Base = Mesh.Positions.Num();
		for (int32 Ring = 0; Ring < Rings; ++Ring)
		{
			const float Z = Z0 + (Z1 - Z0) * Ring / (Rings - 1);
			for (int32 Step = 0; Step < LayerAround; ++Step)
			{
				const float Theta = 2.0f * PI * (Step + Phase) / LayerAround;
				Mesh.Positions.Add(FVector3f(Radius * FMath::Cos(Theta), Radius * FMath::Sin(Theta), Z));
				Mesh.Normals.Add(FVector3f(FMath::Cos(Theta), FMath::Sin(Theta), 0.0f));
			}
		}
		for (int32 Ring = 0; Ring + 1 < Rings; ++Ring)
		{
			for (int32 Step = 0; Step < LayerAround; ++Step)
			{
				const uint32 A = Base + Ring * LayerAround + Step;
				const uint32 B = Base + Ring * LayerAround + (Step + 1) % LayerAround;
				const uint32 C = A + LayerAround;
				const uint32 D = B + LayerAround;
				Mesh.Indices.Append({ A, B, D, A, D, C });
			}
		}
	}

	/** The body: vertex (Ring * LayerAround + Step) is at z = Ring - 30. */
	FLayerMesh LayerBody()
	{
		FLayerMesh Body;
		AddLayerTube(Body, LayerBodyRadius, -LayerBodyHalfLength, LayerBodyHalfLength, LayerBodyRings, 0.0f);
		return Body;
	}

	/** A garment tube GapCm off the skin from Z0 to Z1, rings 0.5 cm apart, turned a quarter step. */
	void AddLayerGarmentTube(FLayerMesh& Mesh, const float GapCm, const float Z0, const float Z1)
	{
		AddLayerTube(Mesh, LayerBodyRadius + GapCm, Z0, Z1, FMath::RoundToInt((Z1 - Z0) / 0.5f) + 1, 0.25f);
	}

	FLayerMesh LayerTee(const float GapCm = 0.3f)
	{
		FLayerMesh Tee;
		AddLayerGarmentTube(Tee, GapCm, -20.25f, 5.25f);
		return Tee;
	}

	FLayerMesh LayerJacket()
	{
		FLayerMesh Jacket;
		AddLayerGarmentTube(Jacket, 3.0f, -5.25f, 20.25f);
		return Jacket;
	}

	bool RunLayerSink(const FLayerMesh& Body, TConstArrayView<FMantleGarmentSurface> Garments, TArray<float>& OutSink, FMantleBodySink& OutStats, const bool bAcrossBody = false)
	{
		FMantleBodySinkParams Params = LayerSinkParams();
		if (bAcrossBody)
		{
			Params.NearShare = 0.5f;
		}
		return FMantleBodySink::Compute(Body.Positions, Body.Normals, Body.Indices, Garments, Params, OutSink, OutStats, nullptr,
			bAcrossBody ? TConstArrayView<uint32>(Body.Indices) : TConstArrayView<uint32>());
	}

	/** The smallest and largest sink on the body rings at whole z from ZLo to ZHi. */
	void SinkRingRange(const TArray<float>& Sink, const int32 ZLo, const int32 ZHi, float& OutMin, float& OutMax)
	{
		OutMin = TNumericLimits<float>::Max();
		OutMax = 0.0f;
		for (int32 Z = ZLo; Z <= ZHi; ++Z)
		{
			const int32 Ring = Z + 30;
			for (int32 Step = 0; Step < LayerAround; ++Step)
			{
				const float Amount = Sink[Ring * LayerAround + Step];
				OutMin = FMath::Min(OutMin, Amount);
				OutMax = FMath::Max(OutMax, Amount);
			}
		}
	}

	/** A tee 0.3 cm off the skin leaves the skin 0.5 - 0.3 = 0.2 cm to sink (less the polygon's 0.02 cm chord, and some slack for the quarter-step hit). */
	constexpr float LayerTeeSinkCm = 0.2f;
	constexpr float LayerSinkSlackCm = 0.05f;

	bool SinkNear(const float Value, const float Expected)
	{
		return FMath::Abs(Value - Expected) <= LayerSinkSlackCm;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleBodySinkLayersTest,
	"Mantle.Runtime.BodySink.Layers",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleBodySinkLayersTest::RunTest(const FString& Parameters)
{
	using namespace MantleBodySinkLayersTestPrivate;
	// A jacket over a tee: the skin under the tee still sinks under the tee, also where the jacket is over it (the old rule
	// took the jacket, 3 cm out, as the garment over the skin and sank nothing there). Skin only the jacket covers does not sink.
	const FLayerMesh Body = LayerBody();
	const FLayerMesh Tee = LayerTee();
	const FLayerMesh Jacket = LayerJacket();
	const FMantleGarmentSurface Garments[] = { Tee.Surface(), Jacket.Surface() };
	TArray<float> Sink;
	FMantleBodySink Stats;
	if (!TestTrue(TEXT("the sink runs"), RunLayerSink(Body, Garments, Sink, Stats)))
	{
		return false;
	}
	AddInfo(Stats.ToString());
	TestEqual(TEXT("one sink value per body vertex"), Sink.Num(), Body.Positions.Num());

	float Min = 0.0f;
	float Max = 0.0f;
	SinkRingRange(Sink, -20, -7, Min, Max);
	TestTrue(FString::Printf(TEXT("under the tee alone the skin sinks 0.2 cm (%.3f to %.3f)"), Min, Max), SinkNear(Min, LayerTeeSinkCm) && SinkNear(Max, LayerTeeSinkCm));

	// Under both: the jacket's lower edge (z -5.25) and the tee's upper edge (z 5.25) both lie in this stretch.
	SinkRingRange(Sink, -5, 5, Min, Max);
	TestTrue(FString::Printf(TEXT("under both the skin still sinks 0.2 cm under the tee (%.3f to %.3f)"), Min, Max), SinkNear(Min, LayerTeeSinkCm) && SinkNear(Max, LayerTeeSinkCm));
	SinkRingRange(Sink, -5, -3, Min, Max);
	TestTrue(FString::Printf(TEXT("just inside the jacket's lower edge it sinks (%.3f to %.3f)"), Min, Max), SinkNear(Min, LayerTeeSinkCm) && SinkNear(Max, LayerTeeSinkCm));
	SinkRingRange(Sink, 3, 5, Min, Max);
	TestTrue(FString::Printf(TEXT("just inside the tee's upper edge, under the jacket, it sinks (%.3f to %.3f)"), Min, Max), SinkNear(Min, LayerTeeSinkCm) && SinkNear(Max, LayerTeeSinkCm));

	// Skin under the jacket alone is 3 cm clear of it: nothing to sink (the smoothing reaches 4 rings past the sunk skin).
	SinkRingRange(Sink, 11, 20, Min, Max);
	TestTrue(FString::Printf(TEXT("under the jacket alone nothing sinks (%.3f)"), Max), Max <= 1.0e-4f);
	SinkRingRange(Sink, 21, 30, Min, Max);
	TestTrue(FString::Printf(TEXT("past both nothing sinks (%.3f)"), Max), Max <= 1.0e-4f);
	SinkRingRange(Sink, -30, -25, Min, Max);
	TestTrue(FString::Printf(TEXT("below both nothing sinks (%.3f)"), Max), Max <= 1.0e-4f);

	// The order the garments are given in does not matter.
	const FMantleGarmentSurface Reversed[] = { Jacket.Surface(), Tee.Surface() };
	TArray<float> ReversedSink;
	FMantleBodySink ReversedStats;
	TestTrue(TEXT("the sink runs with the garments swapped"), RunLayerSink(Body, Reversed, ReversedSink, ReversedStats));
	TestTrue(TEXT("the same sink with the garments swapped"), Sink == ReversedSink);
	TestEqual(TEXT("the same reach with the garments swapped"), ReversedStats.Hit, Stats.Hit);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleBodySinkLayersInsideTest,
	"Mantle.Runtime.BodySink.LayersInside",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleBodySinkLayersInsideTest::RunTest(const FString& Parameters)
{
	using namespace MantleBodySinkLayersTestPrivate;
	// A tee 0.2 cm INSIDE the skin (the skin pokes through it) under the jacket: the skin sinks 0.5 + 0.2 cm, with the
	// across-body and near-half rules on. Both rules leave this hit alone: no body surface lies between it and the vertex.
	const FLayerMesh Body = LayerBody();
	const FLayerMesh Tee = LayerTee(-0.2f);
	const FLayerMesh Jacket = LayerJacket();
	const FMantleGarmentSurface Garments[] = { Tee.Surface(), Jacket.Surface() };
	TArray<float> Sink;
	FMantleBodySink Stats;
	if (!TestTrue(TEXT("the sink runs"), RunLayerSink(Body, Garments, Sink, Stats, true)))
	{
		return false;
	}
	AddInfo(Stats.ToString());
	float Min = 0.0f;
	float Max = 0.0f;
	SinkRingRange(Sink, -4, 4, Min, Max);
	TestTrue(FString::Printf(TEXT("the skin that pokes through the tee sinks 0.7 cm under the jacket too (%.3f to %.3f)"), Min, Max), SinkNear(Min, 0.7f) && SinkNear(Max, 0.7f));
	TestTrue(TEXT("nothing is refused as another part's"), Stats.AcrossBody == 0 && Stats.FarSide == 0);
	TestTrue(TEXT("the skin inside the tee is counted as inside"), Stats.Inside > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleBodySinkOneGarmentUnchangedTest,
	"Mantle.Runtime.BodySink.OneGarmentUnchanged",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleBodySinkOneGarmentUnchangedTest::RunTest(const FString& Parameters)
{
	using namespace MantleBodySinkLayersTestPrivate;
	const FLayerMesh Body = LayerBody();
	const FLayerMesh Tee = LayerTee();

	// One garment: skin under it sinks 0.2 cm, none past it; a garment out of every ray's reach changes nothing.
	const FMantleGarmentSurface One[] = { Tee.Surface() };
	TArray<float> Sink;
	FMantleBodySink Stats;
	if (!TestTrue(TEXT("the sink runs"), RunLayerSink(Body, One, Sink, Stats)))
	{
		return false;
	}
	float Min = 0.0f;
	float Max = 0.0f;
	SinkRingRange(Sink, -20, 4, Min, Max);
	TestTrue(FString::Printf(TEXT("under the tee the skin sinks 0.2 cm (%.3f to %.3f)"), Min, Max), SinkNear(Min, LayerTeeSinkCm) && SinkNear(Max, LayerTeeSinkCm));
	SinkRingRange(Sink, 12, 30, Min, Max);
	TestTrue(FString::Printf(TEXT("past the tee nothing sinks (%.3f)"), Max), Max <= 1.0e-4f);

	FLayerMesh Far;
	AddLayerGarmentTube(Far, 3.0f, 40.0f, 50.0f);
	const FMantleGarmentSurface WithFar[] = { Tee.Surface(), Far.Surface() };
	TArray<float> FarSink;
	FMantleBodySink FarStats;
	TestTrue(TEXT("the sink runs with a garment out of reach"), RunLayerSink(Body, WithFar, FarSink, FarStats));
	TestTrue(TEXT("the same sink with a garment out of reach"), Sink == FarSink);
	TestEqual(TEXT("the same reach"), FarStats.Hit, Stats.Hit);
	TestEqual(TEXT("the same inside count"), FarStats.Inside, Stats.Inside);
	TestEqual(TEXT("the same sunk count"), FarStats.Sunk, Stats.Sunk);
	TestEqual(TEXT("the same largest required sink"), FarStats.MaxRequiredCm, Stats.MaxRequiredCm);
	TestEqual(TEXT("the same largest sink"), FarStats.MaxSinkCm, Stats.MaxSinkCm);

	// One garment of two layers (a tee with a loose outer layer): still its OUTERMOST layer counts, so nothing sinks, as before.
	FLayerMesh TwoLayers;
	AddLayerGarmentTube(TwoLayers, 0.3f, -20.25f, 5.25f);
	AddLayerGarmentTube(TwoLayers, 3.0f, -20.25f, 5.25f);
	const FMantleGarmentSurface Double[] = { TwoLayers.Surface() };
	TArray<float> DoubleSink;
	FMantleBodySink DoubleStats;
	TestTrue(TEXT("the sink runs with one two-layer garment"), RunLayerSink(Body, Double, DoubleSink, DoubleStats));
	SinkRingRange(DoubleSink, -30, 30, Min, Max);
	TestTrue(FString::Printf(TEXT("under one garment's outer layer nothing sinks (%.3f)"), Max), Max <= 1.0e-4f);
	TestEqual(TEXT("nothing sinks"), DoubleStats.Sunk, 0);
	return true;
}

#endif
