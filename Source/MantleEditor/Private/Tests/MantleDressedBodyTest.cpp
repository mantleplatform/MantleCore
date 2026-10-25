// Copyright Joshua Eger. All Rights Reserved.
#include "MantleBodyCoverage.h"
#include "MantleSkinnedMesh.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// The near-half rule. Garment found under the skin belongs to that skin only when it lies in the near half of the
// body part along the ray: a sandal's footbed under a toe is the far side's, a shirt inside a thick shoulder is not.

namespace MantleDressedBodyTestPrivate
{
	constexpr int32 GridPoints = 6;
	constexpr float GridCm = 10.0f;

	/** A flat grid of GridPoints x GridPoints points at height Z over [0, GridCm]^2 facing +z (bUp) or -z. */
	void AddGrid(TArray<FVector3f>& Positions, TArray<FVector3f>& Normals, TArray<uint32>& Indices, const float Z, const bool bUp, const float Lo = 0.0f, const float Hi = GridCm)
	{
		const uint32 Base = Positions.Num();
		for (int32 Y = 0; Y < GridPoints; ++Y)
		{
			for (int32 X = 0; X < GridPoints; ++X)
			{
				const float Step = (Hi - Lo) / (GridPoints - 1);
				Positions.Add(FVector3f(Lo + X * Step, Lo + Y * Step, Z));
				Normals.Add(FVector3f(0.0f, 0.0f, bUp ? 1.0f : -1.0f));
			}
		}
		for (int32 Y = 0; Y + 1 < GridPoints; ++Y)
		{
			for (int32 X = 0; X + 1 < GridPoints; ++X)
			{
				const uint32 A = Base + Y * GridPoints + X;
				const uint32 B = A + 1;
				const uint32 C = A + GridPoints;
				const uint32 D = C + 1;
				if (bUp)
				{
					Indices.Append({ A, B, D, A, D, C });
				}
				else
				{
					Indices.Append({ A, D, B, A, C, D });
				}
			}
		}
	}

	/** A slab ThicknessCm thick (its top facing up at z = ThicknessCm, its bottom facing down at 0) as a one-section body LOD. */
	FMantleSkinnedMeshLOD Slab(const float ThicknessCm)
	{
		FMantleSkinnedMeshLOD Body;
		AddGrid(Body.Positions, Body.Normals, Body.Indices, ThicknessCm, true);
		AddGrid(Body.Positions, Body.Normals, Body.Indices, 0.0f, false);
		FMantleSkinnedMeshSection& Section = Body.Sections.AddDefaulted_GetRef();
		Section.IndexCount = Body.Indices.Num();
		return Body;
	}

	/** An upward-facing sheet (a footbed) at height Z, wider than the slab. */
	struct FSheet
	{
		TArray<FVector3f> Positions;
		TArray<FVector3f> Normals;
		TArray<uint32> Indices;
		explicit FSheet(const float Z) { AddGrid(Positions, Normals, Indices, Z, true, -5.0f, GridCm + 5.0f); }
		FMantleGarmentSurface Surface() const { return FMantleGarmentSurface(Positions, Normals, Indices); }
	};

	constexpr int32 TopTriangles = (GridPoints - 1) * (GridPoints - 1) * 2;
	constexpr int32 TopVertices = GridPoints * GridPoints;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleDressedBodyNearHalfTest,
	"Mantle.Editor.DressedBody.NearHalf",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleDressedBodyNearHalfTest::RunTest(const FString& Parameters)
{
	using namespace MantleDressedBodyTestPrivate;

	// A toe 1.5 cm thick over a footbed 1 cm under its top (past the middle).
	const FMantleSkinnedMeshLOD Toe = Slab(1.5f);
	const FSheet Footbed(0.5f);
	const FMantleGarmentSurface Garments[] = { Footbed.Surface() };

	FMantleDressedBody Before;
	FMantleDressedBodyParams Old;
	Old.NearShare = 0.0f;
	TestTrue(TEXT("the toe dresses with the old rule"), FMantleDressedBody::Compute(Toe, Garments, Old, Before));
	TestEqual(TEXT("the old rule removes the toe's top"), Before.DroppedTriangles, TopTriangles);

	FMantleDressedBody After;
	TestTrue(TEXT("the toe dresses with the near-half rule"), FMantleDressedBody::Compute(Toe, Garments, FMantleDressedBodyParams(), After));
	TestEqual(TEXT("the near-half rule removes none of the toe"), After.DroppedTriangles, 0);
	TestEqual(TEXT("its top is refused as the far side's"), After.Coverage[0].UnderFarSide, TopVertices);
	TestEqual(TEXT("and nothing of the toe sinks"), After.Sink.Sunk, 0);
	TestEqual(TEXT("the sink refuses the top as the far side's"), After.Sink.FarSide, TopVertices);

	// The sink alone, every triangle kept: the old rule pushes the top down 2 cm (1 cm under a sheet 1 cm deep).
	FMantleBodySinkParams SinkOld;
	TArray<float> SinkCm;
	FMantleBodySink Sink;
	TestTrue(TEXT("the old sink runs"), FMantleBodySink::Compute(Toe.Positions, Toe.Normals, Toe.Indices, Garments, SinkOld, SinkCm, Sink));
	TestTrue(TEXT("the old sink pushes the toe's top 2 cm"), FMath::IsNearlyEqual(Sink.MaxRequiredCm, 2.0f, 0.01f));
	FMantleBodySinkParams SinkNew;
	SinkNew.NearShare = 0.5f;
	TestTrue(TEXT("the near-half sink runs"), FMantleBodySink::Compute(Toe.Positions, Toe.Normals, Toe.Indices, Garments, SinkNew, SinkCm, Sink, nullptr, Toe.Indices));
	TestEqual(TEXT("the near-half sink pushes nothing"), Sink.Sunk, 0);

	// A shirt 3 cm inside a part 10 cm thick (the near half) still covers and sinks it.
	const FMantleSkinnedMeshLOD Shoulder = Slab(10.0f);
	const FSheet Shirt(7.0f);
	const FMantleGarmentSurface ShirtSurface[] = { Shirt.Surface() };
	FMantleDressedBody Thick;
	TestTrue(TEXT("the shoulder dresses"), FMantleDressedBody::Compute(Shoulder, ShirtSurface, FMantleDressedBodyParams(), Thick));
	TestEqual(TEXT("the shirt inside a thick part still removes its top"), Thick.DroppedTriangles, TopTriangles);
	TestEqual(TEXT("and nothing is refused"), Thick.Coverage[0].UnderFarSide, 0);
	TestTrue(TEXT("the near-half sink still pushes a thick part 4 cm"),
		FMantleBodySink::Compute(Shoulder.Positions, Shoulder.Normals, Shoulder.Indices, ShirtSurface, SinkNew, SinkCm, Sink, nullptr, Shoulder.Indices)
		&& FMath::IsNearlyEqual(Sink.MaxRequiredCm, 4.0f, 0.01f) && Sink.FarSide == 0);
	return true;
}

#endif
