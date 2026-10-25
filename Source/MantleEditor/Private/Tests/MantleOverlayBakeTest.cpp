// Copyright Joshua Eger. All Rights Reserved.
#include "MantleOverlayBake.h"
#include "MantleOverlayChecks.h"
#include "SMantleOverlayPlaceViewport.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// #54: the placed-picture bake, on synthetic surfaces (no assets, no renderer): two UV islands that meet in 3D at x = 0 and are
// far apart in UV, as a chest and a shoulder are. A picture placed across the join must paint both islands right up to the join.

namespace MantleOverlayBakeTestPrivate
{
	using FTriangle = FMantleOverlayBake::FTriangle;

	void AddQuad(TArray<FTriangle>& Out, const FVector3f& P0, const FVector3f& P1, const FVector3f& P2, const FVector3f& P3,
		const FVector3f& Normal, const FVector2f& UVMin, const FVector2f& UVMax)
	{
		const FVector2f UV[4] = { FVector2f(UVMin.X, UVMax.Y), FVector2f(UVMax.X, UVMax.Y), FVector2f(UVMax.X, UVMin.Y), FVector2f(UVMin.X, UVMin.Y) };
		const FVector3f P[4] = { P0, P1, P2, P3 };
		const int32 Corners[2][3] = { { 0, 1, 2 }, { 0, 2, 3 } };
		for (int32 Half = 0; Half < 2; ++Half)
		{
			FTriangle Triangle;
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				Triangle.Position[Corner] = P[Corners[Half][Corner]];
				Triangle.Normal[Corner] = Normal;
				Triangle.UV[Corner] = UV[Corners[Half][Corner]];
			}
			Out.Add(Triangle);
		}
	}

	/** Island A (a flat wall facing +Y, x -10..0) and island B (tilted 30 degrees away, x 0..8.66), meeting at x = 0. */
	TArray<FTriangle> JoinedIslands()
	{
		TArray<FTriangle> Triangles;
		AddQuad(Triangles, FVector3f(-10, 0, -5), FVector3f(0, 0, -5), FVector3f(0, 0, 5), FVector3f(-10, 0, 5),
			FVector3f(0, 1, 0), FVector2f(0.1f, 0.1f), FVector2f(0.4f, 0.4f));
		AddQuad(Triangles, FVector3f(0, 0, -5), FVector3f(8.66f, -5, -5), FVector3f(8.66f, -5, 5), FVector3f(0, 0, 5),
			FVector3f(0.5f, 0.866f, 0), FVector2f(0.5f, 0.1f), FVector2f(0.8f, 0.4f));
		return Triangles;
	}

	/** A projector at the origin looking along -Y (at the walls), the picture's right along +X and up along +Z. */
	FMantleOverlayPlacement ProjectorAtJoin(const float WidthCm)
	{
		FMantleOverlayPlacement Placement;
		Placement.Transform = FTransform(FRotationMatrix::MakeFromXZ(FVector(0, -1, 0), FVector::UpVector).ToQuat(), FVector::ZeroVector);
		Placement.WidthCm = WidthCm;
		Placement.DepthCm = 8.f;
		Placement.Falloff = 0.f;
		return Placement;
	}

	/** The mask value at a UV position. */
	uint8 At(const TArray<uint8>& Mask, const float U, const float V)
	{
		return Mask[FMath::FloorToInt(V * FMantleOverlayBake::Size) * FMantleOverlayBake::Size + FMath::FloorToInt(U * FMantleOverlayBake::Size)];
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleOverlayBakeTest,
	"Mantle.Editor.Overlay.PlacedBake",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleOverlayBakeTest::RunTest(const FString& Parameters)
{
	using namespace MantleOverlayBakeTestPrivate;
	const TArray<FTriangle> Triangles = JoinedIslands();

	// A white picture, 16 cm wide across the join (x -8..8): both islands are painted, and right up to the join.
	{
		const TArray<uint8> White = { 255, 255, 255, 255 };
		TArray<uint8> Mask;
		TArray<FColor> Normal;
		const int32 Painted = FMantleOverlayBake::Bake(Triangles, White, 2, 2, ProjectorAtJoin(16.f), Mask, &Normal);
		TestTrue(TEXT("something is painted"), Painted > 0);
		TestEqual(TEXT("the mask is Size x Size"), Mask.Num(), FMantleOverlayBake::Size * FMantleOverlayBake::Size);
		// Island A spans UV 0.1..0.4 for x -10..0: x = -1 is U 0.37. Island B spans U 0.5..0.8 for x 0..8.66: x = 1 is about U 0.535.
		TestTrue(TEXT("island A is painted next to the join"), At(Mask, 0.37f, 0.25f) > 200);
		TestTrue(TEXT("island B is painted next to the join"), At(Mask, 0.535f, 0.25f) > 100);
		// Beyond the picture's 8 cm half width, island A (x -10..-8, U 0.1..0.16) is not painted.
		TestEqual(TEXT("outside the picture nothing is painted"), static_cast<int32>(At(Mask, 0.12f, 0.25f)), 0);
		// Between the islands (padding is a few texels) nothing is written.
		TestEqual(TEXT("the gap between the islands stays empty"), static_cast<int32>(At(Mask, 0.45f, 0.25f)), 0);
		// The padding extends the island's own value a few texels out: just past island A's border it equals the border.
		const int32 BorderX = FMath::FloorToInt(0.4f * FMantleOverlayBake::Size);
		TestTrue(TEXT("padding continues the border"), Mask[256 * FMantleOverlayBake::Size + BorderX + 2] > 200);
		// The mask passes the overlay checks.
		TArray<FMantleCosmeticIssue> Issues;
		TestTrue(TEXT("the baked mask passes the mask checks"), FMantleOverlayChecks::CheckMask(TEXT("The mask"), FMantleOverlayBake::Size,
			FMantleOverlayBake::Size, FMantleOverlayBake::MaskToPixels(Mask), Issues));
		// The normal map is flat (128,128,255) where the mask is flat, and tilts at the mark's edge.
		TestTrue(TEXT("flat normal away from the mark"), Normal[900 * FMantleOverlayBake::Size + 900] == FColor(128, 128, 255, 255));
		bool bTilted = false;
		for (int32 X = 0; X < FMantleOverlayBake::Size && !bTilted; ++X)
		{
			bTilted = Normal[256 * FMantleOverlayBake::Size + X] != FColor(128, 128, 255, 255);
		}
		TestTrue(TEXT("the mark's edge tilts the normal"), bTilted);
		TArray<FMantleCosmeticIssue> NormalIssues;
		TestTrue(TEXT("the baked normal map passes the normal checks"), FMantleOverlayChecks::CheckNormal(TEXT("The normal map"),
			FMantleOverlayBake::Size, FMantleOverlayBake::Size, Normal, NormalIssues));
	}

	// A picture black on its left half and white on its right: only the right half shows.
	{
		const TArray<uint8> Half = { 0, 255, 0, 255 };
		TArray<uint8> Mask;
		FMantleOverlayBake::Bake(Triangles, Half, 2, 2, ProjectorAtJoin(16.f), Mask);
		TestEqual(TEXT("the black half stays empty (x -4)"), static_cast<int32>(At(Mask, 0.26f, 0.25f)), 0);
		TestTrue(TEXT("the white half shows (x 4, island B)"), At(Mask, 0.64f, 0.25f) > 100);
	}

	// A surface turned away from the projector is not painted, and one beyond the reach is not either.
	{
		TArray<FTriangle> Away;
		AddQuad(Away, FVector3f(-5, 0, -5), FVector3f(5, 0, -5), FVector3f(5, 0, 5), FVector3f(-5, 0, 5), FVector3f(0, -1, 0),
			FVector2f(0.1f, 0.1f), FVector2f(0.4f, 0.4f));
		const TArray<uint8> White = { 255 };
		TArray<uint8> Mask;
		TestEqual(TEXT("a back face is not painted"), FMantleOverlayBake::Bake(Away, White, 1, 1, ProjectorAtJoin(16.f), Mask), 0);
		TArray<FTriangle> Far;
		AddQuad(Far, FVector3f(-5, 20, -5), FVector3f(5, 20, -5), FVector3f(5, 20, 5), FVector3f(-5, 20, 5), FVector3f(0, 1, 0),
			FVector2f(0.1f, 0.1f), FVector2f(0.4f, 0.4f));
		TestEqual(TEXT("beyond the reach is not painted"), FMantleOverlayBake::Bake(Far, White, 1, 1, ProjectorAtJoin(16.f), Mask), 0);
	}

	// Picture strength: alpha when the picture has any transparency, else brightness.
	{
		const TArray<FColor> Opaque = { FColor(255, 255, 255, 255), FColor(0, 0, 0, 255) };
		const TArray<uint8> FromBrightness = FMantleOverlayBake::PictureStrength(Opaque, 2, 1);
		TestTrue(TEXT("white shows, black does not"), FromBrightness.Num() == 2 && FromBrightness[0] >= 250 && FromBrightness[1] == 0);
		const TArray<FColor> Clear = { FColor(0, 0, 0, 255), FColor(255, 255, 255, 0) };
		const TArray<uint8> FromAlpha = FMantleOverlayBake::PictureStrength(Clear, 2, 1);
		TestTrue(TEXT("with transparency the alpha decides"), FromAlpha.Num() == 2 && FromAlpha[0] == 255 && FromAlpha[1] == 0);
	}

	// The placement viewport's maths.
	{
		FVector Point, Normal;
		TestTrue(TEXT("a ray from the front meets the wall"), MantleOverlayPlaceMath::RayOnTriangles(Triangles, FVector(-5, 50, 0), FVector(0, -1, 0), Point, Normal));
		TestTrue(TEXT("at the wall"), FMath::IsNearlyZero(Point.Y, 0.01) && FMath::IsNearlyEqual(Point.X, -5.0, 0.01));
		TestTrue(TEXT("its normal faces the ray"), Normal.Y > 0.99);
		TestFalse(TEXT("a ray past the edge misses"), MantleOverlayPlaceMath::RayOnTriangles(Triangles, FVector(-50, 50, 0), FVector(0, -1, 0), Point, Normal));
		const FQuat Facing = MantleOverlayPlaceMath::FaceSurface(FVector(0, 1, 0));
		TestTrue(TEXT("the projector looks into the surface"), Facing.GetForwardVector().Equals(FVector(0, -1, 0), 0.01));
		TestTrue(TEXT("the picture stays upright"), Facing.GetUpVector().Equals(FVector::UpVector, 0.01));
		TestTrue(TEXT("a scale drag grows the width"), FMath::IsNearlyEqual(MantleOverlayPlaceMath::ApplyScaleDrag(10.f, FVector(0.1, 0.1, 0.1)), 11.f, 0.01f));
		TestTrue(TEXT("the width never goes below the minimum"), MantleOverlayPlaceMath::ApplyScaleDrag(1.f, FVector(-5.0)) >= 0.5f);
	}
	return true;
}

#endif
