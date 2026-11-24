// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/**
 * #167: what one pixel of the drawn map shows. The order is a rank: where two surfaces are at about the same height the
 * higher rank wins (a road painted over the ground), and an outline is drawn on the higher rank's side of a border.
 */
enum class EMantleMinimapSurface : uint8
{
	None = 0,
	Ground,
	Green,
	Water,
	Rail,
	Road,
	Building,
	Count
};

/**
 * #167: the map picture's grid over an area (settings.minimap.image). 1 m per pixel, coarser when the longer side would pass
 * MaxPixels, finer when it would be under MinPixels. Cut into Columns x Rows tiles of the same size, each side at most
 * MaxTileSide, at most MaxTilesPerSide a side. The picture is the area grown to whole tiles around its center, and MinX..MaxY
 * are the drawn pixels' outer edges in cm: +X is the picture's right, +Y its down (north, -Y, up), row 0 at MinY. Pure.
 */
struct MANTLEEDITOR_API FMantleMinimapGrid
{
	static constexpr double DefaultCmPerPixel = 100.0;
	static constexpr int32 MaxPixels = 8192;
	static constexpr int32 MinPixels = 1024;
	static constexpr int32 MaxTileSide = 2048;
	static constexpr int32 MaxTilesPerSide = 4;

	double CmPerPixel = DefaultCmPerPixel;

	/** The whole picture, in pixels: Columns x TileWidth by Rows x TileHeight. */
	int32 Width = 0;
	int32 Height = 0;
	int32 Columns = 0;
	int32 Rows = 0;
	int32 TileWidth = 0;
	int32 TileHeight = 0;

	double MinX = 0.0;
	double MaxX = 0.0;
	double MinY = 0.0;
	double MaxY = 0.0;

	/** The grid for Area (cm, world X and Y). An empty or invalid area gives an invalid grid. */
	static FMantleMinimapGrid Plan(const FBox2D& Area);

	bool IsValid() const { return Width > 0 && Height > 0; }

	/** The world rectangle (cm) tile Row, Column covers. */
	FBox2D TileBounds(int32 Row, int32 Column) const;

	/** World X, Y (cm) in pixels from the picture's top left corner (pixel centers are at .5). */
	FVector2D ToPixel(double X, double Y) const { return FVector2D((X - MinX) / CmPerPixel, (Y - MinY) / CmPerPixel); }
};

/**
 * #167: the drawn map, a top-down picture made on the CPU from the level's triangles: no renderer, no camera, the same
 * picture every time. Each pixel keeps the top-most surface drawn over it (DrawTriangle may be called from many threads at
 * once); Shade turns that into flat colors with outlines.
 */
class MANTLEEDITOR_API FMantleMinimapCanvas
{
public:
	/** Within this height (cm) per rank step, the higher-ranked surface is on top: a road mesh laid on the ground shows. */
	static constexpr double RankLiftCm = 8.0;

	/** A surface higher than this (cm) above the lowest one around it, that its name did not place, is drawn as a building. */
	static constexpr double BuildingRiseCm = 300.0;

	/** How far (cm) around a pixel the lowest surface is looked for, as the ground it stands on. */
	static constexpr double GroundRadiusCm = 2500.0;

	/** A step in height (cm) between two pixels of the same surface that gets an outline (a lower roof next to a taller one). */
	static constexpr double StepOutlineCm = 200.0;

	explicit FMantleMinimapCanvas(const FMantleMinimapGrid& InGrid);

	const FMantleMinimapGrid& GetGrid() const { return Grid; }

	/** One triangle, corners in world cm. Thread-safe. Triangles seen edge-on from above (walls) cover no pixel. */
	void DrawTriangle(const FVector& A, const FVector& B, const FVector& C, EMantleMinimapSurface Surface);

	/** The top-most surface at pixel X, Y and its height (cm, with the rank lift). False where nothing was drawn. */
	bool Top(int32 X, int32 Y, double& OutZ, EMantleMinimapSurface& OutSurface) const;

	/** The picture, Width x Height, row 0 first, every pixel opaque. */
	TArray<FColor> Shade() const;

	/** Pixels with something drawn over them. */
	int64 CountCovered() const;

	/** A surface's flat color. */
	static FColor ColorOf(EMantleMinimapSurface Surface);

	/**
	 * The surface a name says (a material's, a mesh's or an actor's): "M_Road_Asphalt" a road, "SM_Building_07" a building.
	 * None when no word in it says one.
	 */
	static EMantleMinimapSurface Classify(const FString& Name);

private:
	FMantleMinimapGrid Grid;

	/** Per pixel: 0 for nothing, else the height (as an ordered integer) above 8 bits and the surface below. */
	TArray<int64> Cells;
};
