// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleMinimapDrawing.h"

class FMantlePlaceAssetConverter;
class UWorld;
struct FMantlePlaceAssetSet;
struct FMantleValidationIssue;

/** #167: the place's map picture, whole, before it is cut into tiles. */
struct MANTLEEDITOR_API FMantleMinimapPicture
{
	FMantleMinimapGrid Grid;

	/** Grid.Width x Grid.Height, row 0 (MinY, north) first, every pixel opaque. */
	TArray<FColor> Pixels;

	/** One line for the log: what made the picture, from what, in how long. */
	FString Source;

	/** Something the creator should know about the picture, in plain words (their own picture was stretched). Empty when nothing. */
	FString Warning;

	bool IsValid() const { return Grid.IsValid() && Pixels.Num() == Grid.Width * Grid.Height; }

	/** Tile Row, Column's pixels: Grid.TileWidth x Grid.TileHeight, top row first. Empty when out of range. */
	TArray<FColor> Tile(int32 Row, int32 Column) const;
};

/**
 * #167: every place's map picture (settings.minimap.image), made at every publish and local preview export: the drawn map
 * (FMantleMinimapCanvas) of the level's meshes, baked spline meshes, landscapes and Water Bodies, or the creator's own picture
 * from Mantle Place Settings > Minimap. Cut into equal tiles (FMantleMinimapGrid), each an opaque color PNG uploaded as a
 * place Texture.
 */
class MANTLEEDITOR_API FMantleMinimap
{
public:
	/** The creator's own picture may differ from the area's shape by this much (a share of it) before Check warns that it is stretched. */
	static constexpr double ShapeTolerance = 0.02;

	/**
	 * The world box (cm) the map covers: everything that collides (meshes and baked spline meshes), landscapes, and lakes, rivers
	 * and custom Water Bodies. An ocean and a sky or backdrop never set it. Everything drawn when nothing collides; invalid when
	 * the level draws nothing.
	 */
	static FBox Area(UWorld* World);

	/** The drawn map of World over Area's grid. False with a plain reason when the level draws nothing. */
	static bool Draw(UWorld* World, FMantleMinimapPicture& Out, FString& OutError);

	/**
	 * Image (Width x Height, top row first) stretched over Grid's whole picture: its top edge is north (MinY), its left edge
	 * MinX. OutWarning says so when Image's shape is more than ShapeTolerance off the grid's. Pure.
	 */
	static bool FromImage(TConstArrayView<FColor> Image, int32 Width, int32 Height, const FMantleMinimapGrid& Grid, TArray<FColor>& OutPixels,
		FString& OutWarning, FString& OutError);

	/** The plain-words warning for a picture of Width x Height over Grid, or empty when its shape is within ShapeTolerance. Pure. */
	static FString DescribeShape(int32 Width, int32 Height, const FMantleMinimapGrid& Grid);

	/** World's picture: the creator's own when Mantle Place Settings choose one, else Draw's. */
	static bool Make(UWorld* World, FMantleMinimapPicture& Out, FString& OutError);

	/**
	 * Make, then every tile through Converter.ConvertMinimapTile into Set (Set.MinimapTiles and Set.MinimapBounds). A picture
	 * that cannot be made is a warning in Set.Issues: the place goes without one. True when the tiles are in Set.
	 */
	static bool Convert(UWorld* World, FMantlePlaceAssetConverter& Converter, FMantlePlaceAssetSet& Set);

	/** Check this place: an error when the creator's own picture is missing or unreadable, a warning when it will be stretched. */
	static void Check(UWorld* World, TArray<FMantleValidationIssue>& OutIssues);

	/**
	 * A simple polygon's triangles (corners in order, either way round), as index triples into Corners. Fewer than 3 corners, or
	 * a polygon that crosses itself where no ear can be cut, gives what could be cut. Pure.
	 */
	static TArray<int32> Triangulate(TConstArrayView<FVector2D> Corners);
};
