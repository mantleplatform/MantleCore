// Copyright Joshua Eger. All Rights Reserved.
#include "MantleMinimapDrawing.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// #167: the drawn map's grid (scale, tiles, bounds), its orientation, the top-most surface per pixel, the names that say what a
// surface is, and the outlines. Pure: no level, no renderer.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleMinimapDrawingTestPrivate
{
	FBox2D Box(double MinX, double MinY, double MaxX, double MaxY)
	{
		return FBox2D(FVector2D(MinX, MinY), FVector2D(MaxX, MaxY));
	}

	/** A flat square of Surface at height Z over X0..X1, Y0..Y1 (cm). */
	void Square(FMantleMinimapCanvas& Canvas, double X0, double Y0, double X1, double Y1, double Z, EMantleMinimapSurface Surface)
	{
		Canvas.DrawTriangle(FVector(X0, Y0, Z), FVector(X1, Y0, Z), FVector(X1, Y1, Z), Surface);
		Canvas.DrawTriangle(FVector(X0, Y0, Z), FVector(X1, Y1, Z), FVector(X0, Y1, Z), Surface);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMinimapGridTest,
	"Mantle.Editor.Minimap.GridScaleTilesAndBounds",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMinimapGridTest::RunTest(const FString& Parameters)
{
	using namespace MantleMinimapDrawingTestPrivate;

	// A 60 m arena: finer than 1 m per pixel, so its longer side is 1024 pixels; one tile.
	const FMantleMinimapGrid Arena = FMantleMinimapGrid::Plan(Box(-3000.0, -2000.0, 3000.0, 2000.0));
	TestEqual(TEXT("arena width"), Arena.Width, 1024);
	TestEqual(TEXT("arena tiles"), Arena.Columns * Arena.Rows, 1);
	TestTrue(TEXT("arena is finer than 1 m per pixel"), Arena.CmPerPixel < 100.0);
	TestTrue(TEXT("arena bounds hold the area"), Arena.MinX <= -3000.0 && Arena.MaxX >= 3000.0 && Arena.MinY <= -2000.0 && Arena.MaxY >= 2000.0);
	TestEqual(TEXT("arena bounds are the pixels' edges"), Arena.MaxX - Arena.MinX, Arena.Width * Arena.CmPerPixel, 1e-6);

	// City Sample's 4.48 x 5.28 km: 1 m per pixel, 3 x 3 equal tiles, grown to whole tiles around the middle.
	const FMantleMinimapGrid City = FMantleMinimapGrid::Plan(Box(-245000.0, -415000.0, 203000.0, 113000.0));
	TestEqual(TEXT("city scale"), City.CmPerPixel, 100.0);
	TestEqual(TEXT("city columns"), City.Columns, 3);
	TestEqual(TEXT("city rows"), City.Rows, 3);
	TestEqual(TEXT("city tile width"), City.TileWidth, 1494);
	TestEqual(TEXT("city tile height"), City.TileHeight, 1760);
	TestEqual(TEXT("city width is whole tiles"), City.Width, City.TileWidth * City.Columns);
	TestEqual(TEXT("city centered"), (City.MinX + City.MaxX) * 0.5, (-245000.0 + 203000.0) * 0.5, 1e-6);
	TestTrue(TEXT("city tile 1,2 starts two tiles right and one down"), City.TileBounds(1, 2).Min.Equals(
		FVector2D(City.MinX + 2 * City.TileWidth * 100.0, City.MinY + City.TileHeight * 100.0), 1e-6));

	// 40 km: coarser, 8192 pixels on the longer side, 4 x 4 tiles of 2048.
	const FMantleMinimapGrid Huge = FMantleMinimapGrid::Plan(Box(0.0, 0.0, 4000000.0, 4000000.0));
	TestEqual(TEXT("huge width"), Huge.Width, 8192);
	TestEqual(TEXT("huge tiles"), Huge.Columns * Huge.Rows, 16);
	TestEqual(TEXT("huge tile side"), Huge.TileWidth, FMantleMinimapGrid::MaxTileSide);

	TestFalse(TEXT("no area, no grid"), FMantleMinimapGrid::Plan(FBox2D(ForceInit)).IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMinimapOrientationTest,
	"Mantle.Editor.Minimap.PlusXIsRightPlusYIsDownTopSurfaceWins",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMinimapOrientationTest::RunTest(const FString& Parameters)
{
	using namespace MantleMinimapDrawingTestPrivate;
	const FMantleMinimapGrid Grid = FMantleMinimapGrid::Plan(Box(0.0, 0.0, 102400.0, 102400.0));
	TestEqual(TEXT("1 m per pixel"), Grid.CmPerPixel, 100.0);
	FMantleMinimapCanvas Canvas(Grid);

	// A road at the +X end (middle of Y), water at the +Y end (middle of X).
	Square(Canvas, 100000.0, 50000.0, 102000.0, 52000.0, 0.0, EMantleMinimapSurface::Road);
	Square(Canvas, 50000.0, 100000.0, 52000.0, 102000.0, 0.0, EMantleMinimapSurface::Water);
	double Z = 0.0;
	EMantleMinimapSurface Surface = EMantleMinimapSurface::None;
	TestTrue(TEXT("+X is the right edge"), Canvas.Top(1010, 510, Z, Surface) && Surface == EMantleMinimapSurface::Road);
	TestTrue(TEXT("+Y is the bottom edge"), Canvas.Top(510, 1010, Z, Surface) && Surface == EMantleMinimapSurface::Water);
	TestFalse(TEXT("nothing at the top left"), Canvas.Top(5, 5, Z, Surface));

	// A building's roof over the ground wins; a road laid at the ground's own height shows over it.
	Square(Canvas, 0.0, 0.0, 10000.0, 10000.0, 0.0, EMantleMinimapSurface::Ground);
	Square(Canvas, 2000.0, 2000.0, 4000.0, 4000.0, 1200.0, EMantleMinimapSurface::Building);
	Square(Canvas, 6000.0, 0.0, 7000.0, 10000.0, 0.0, EMantleMinimapSurface::Road);
	TestTrue(TEXT("roof on top"), Canvas.Top(30, 30, Z, Surface) && Surface == EMantleMinimapSurface::Building && Z >= 1200.0);
	TestTrue(TEXT("road over ground at the same height"), Canvas.Top(65, 50, Z, Surface) && Surface == EMantleMinimapSurface::Road);
	TestTrue(TEXT("ground elsewhere"), Canvas.Top(50, 80, Z, Surface) && Surface == EMantleMinimapSurface::Ground);

	// Walls seen edge-on from above cover nothing.
	FMantleMinimapCanvas Walls(Grid);
	Walls.DrawTriangle(FVector(0.0, 500.0, 0.0), FVector(5000.0, 500.0, 0.0), FVector(5000.0, 500.0, 3000.0), EMantleMinimapSurface::Building);
	TestEqual(TEXT("an edge-on wall covers nothing"), Walls.CountCovered(), static_cast<int64>(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMinimapClassifyTest,
	"Mantle.Editor.Minimap.NamesSayTheSurface",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMinimapClassifyTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("road"), FMantleMinimapCanvas::Classify(TEXT("M_TQ3280NE_ROAD")) == EMantleMinimapSurface::Road);
	TestTrue(TEXT("bridge"), FMantleMinimapCanvas::Classify(TEXT("SM_TQ3280NE_FOOT_BRIDGE")) == EMantleMinimapSurface::Road);
	TestTrue(TEXT("rail bridge is rail"), FMantleMinimapCanvas::Classify(TEXT("SM_TQ3280NE_RAIL_BRIDGE")) == EMantleMinimapSurface::Rail);
	TestTrue(TEXT("building"), FMantleMinimapCanvas::Classify(TEXT("SM_TQ3280NE_Building0054")) == EMantleMinimapSurface::Building);
	TestTrue(TEXT("camel case"), FMantleMinimapCanvas::Classify(TEXT("SM_TQ3280NE_UnderConstruction0001")) == EMantleMinimapSurface::Building);
	TestTrue(TEXT("green space"), FMantleMinimapCanvas::Classify(TEXT("M_TQ3280NE_GREEN_SPACE")) == EMantleMinimapSurface::Green);
	TestTrue(TEXT("plural"), FMantleMinimapCanvas::Classify(TEXT("SM_Trees_Cluster")) == EMantleMinimapSurface::Green);
	TestTrue(TEXT("water"), FMantleMinimapCanvas::Classify(TEXT("T_TQ3280NE_WATER")) == EMantleMinimapSurface::Water);
	TestTrue(TEXT("terrain"), FMantleMinimapCanvas::Classify(TEXT("SM_TQ3280NE_TERRAIN")) == EMantleMinimapSurface::Ground);
	TestTrue(TEXT("a guard rail is no railway"), FMantleMinimapCanvas::Classify(TEXT("SM_GuardRail_01")) == EMantleMinimapSurface::None);
	TestTrue(TEXT("a word inside another is not it"), FMantleMinimapCanvas::Classify(TEXT("SM_Broadcast_Seat")) == EMantleMinimapSurface::None);
	TestTrue(TEXT("nothing said"), FMantleMinimapCanvas::Classify(TEXT("SM_Chair_02")) == EMantleMinimapSurface::None);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMinimapShadeTest,
	"Mantle.Editor.Minimap.ShadeOutlinesAndRaisedGround",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMinimapShadeTest::RunTest(const FString& Parameters)
{
	using namespace MantleMinimapDrawingTestPrivate;
	const FMantleMinimapGrid Grid = FMantleMinimapGrid::Plan(Box(0.0, 0.0, 102400.0, 102400.0));
	FMantleMinimapCanvas Canvas(Grid);
	Square(Canvas, 0.0, 0.0, 102400.0, 102400.0, 0.0, EMantleMinimapSurface::Ground);
	// A named building, and an unnamed block 10 m tall that is drawn as one.
	Square(Canvas, 10000.0, 10000.0, 20000.0, 20000.0, 1500.0, EMantleMinimapSurface::Building);
	Square(Canvas, 40000.0, 40000.0, 50000.0, 50000.0, 1000.0, EMantleMinimapSurface::Ground);
	const TArray<FColor> Pixels = Canvas.Shade();
	TestEqual(TEXT("one pixel each"), Pixels.Num(), Grid.Width * Grid.Height);
	auto At = [&Pixels, &Grid](int32 X, int32 Y) { return Pixels[Y * Grid.Width + X]; };

	const FColor Ground = FMantleMinimapCanvas::ColorOf(EMantleMinimapSurface::Ground);
	TestEqual(TEXT("open ground is the ground color"), At(80, 80), Ground);
	const FColor Inside = At(150, 150), Edge = At(100, 150);
	TestTrue(TEXT("a building is not ground"), Inside != Ground);
	TestTrue(TEXT("its edge is darker than its roof"), Edge.R < Inside.R && Edge.G < Inside.G && Edge.B < Inside.B);
	TestTrue(TEXT("a raised unnamed block is drawn as a building"), At(450, 450) != Ground && At(450, 450).R < Ground.R);
	bool bOpaque = true;
	for (const FColor& Pixel : Pixels)
	{
		bOpaque &= Pixel.A == 255;
	}
	TestTrue(TEXT("every pixel opaque"), bOpaque);
	return true;
}

#endif
