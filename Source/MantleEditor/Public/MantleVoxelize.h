// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class AActor;
class AMantleVoxelWorldActor;
class UMaterialInterface;
class UTexture2D;
class UWorld;

/** How Voxelize and Import make a block world (#168 step 6). */
struct MANTLEEDITOR_API FMantleVoxelizeOptions
{
	/** The side of every tile it makes, in pixels: 16, 32, 64 or 128. */
	int32 TileSize = 32;

	/**
	 * Smooth ground instead of blocks: 50 cm voxels, each solid voxel a ground material, drawn as a smooth surface. Same shape
	 * rules as blocks (#64); at most 32 materials.
	 */
	bool bSmooth = false;

	/**
	 * The content folder the tiles are saved in, one texture per block type (e.g. /Game/MantleVoxelize/Map_1). Empty makes
	 * them in the transient package and saves nothing (the tests).
	 */
	FString TileFolder;
};

/** What a Voxelize or Import made, or why it made nothing. */
struct MANTLEEDITOR_API FMantleVoxelizeResult
{
	/** Empty on success; otherwise what stopped it, in plain words. */
	FString Error;

	/** On success, what Check will warn about the result (one mesh filled most of it, #64); empty otherwise. */
	FString Warning;

	AMantleVoxelWorldActor* World = nullptr;

	/** The world's lowest corner, in cm (a multiple of 100), and its size in blocks (multiples of 16). */
	FVector Origin = FVector::ZeroVector;
	FIntVector Size = FIntVector::ZeroValue;

	/** Blocks that are not air. */
	int64 Blocks = 0;

	/** The block type names, id 1 first. */
	TArray<FString> TypeNames;

	/** The actors turned into blocks, moved to the "Voxelized sources" folder and left out of the place. */
	int32 SourceActors = 0;

	bool IsOk() const { return Error.IsEmpty() && World != nullptr; }
};

/**
 * #168 step 6: turns a level's cube meshes into a Mantle Voxel World, and an MBW1 file into one.
 *
 * Voxelize: each source actor's static mesh components (and instanced mesh instances) fill the 100 cm cells of their shape
 * (#64): a closed mesh every cell whose centre is inside it, an open or too-thin one the cells its surface passes through, and
 * one with no source triangles (cooked only) its world bounding box. So voxel-art cubes of any multiple of 100 cm become
 * blocks. Sky domes are skipped, as the exporter skips them (FMantleSkyMesh). Each distinct (mesh, first
 * material) pair is a block type, named after its mesh; its tile is the material's base colour texture rendered down to a
 * Tile Size square (or a flat tile of its base colour), saved as a new texture in the project's own content. The world sits
 * at the lowest filled cell, its size rounded up to whole sections. The sources move to the "Voxelized sources" folder,
 * hidden, and the exporter leaves them out.
 *
 * Import: an MBW1 file's blocks with placeholder block types block_1 to block_N (N the largest id used), each a flat grey
 * tile for the creator to replace.
 */
class MANTLEEDITOR_API FMantleVoxelize
{
public:
	/** Voxelizes Sources into World. Nothing changes when it fails (Result.Error says why). The caller opens the undo transaction. */
	static FMantleVoxelizeResult Voxelize(UWorld* World, const TArray<AActor*>& Sources, const FMantleVoxelizeOptions& Options);

	/** A Mantle Voxel World at Location holding Bytes (MBW1, blocky only). The caller opens the undo transaction. */
	static FMantleVoxelizeResult ImportMbw(UWorld* World, TConstArrayView<uint8> Bytes, const FVector& Location, const FMantleVoxelizeOptions& Options);

	/** Tools > Mantle > Voxelize selected meshes: the selected actors, one undoable step, a notification with what it made. */
	static void VoxelizeSelection(bool bSmooth = false);

	/** VoxelizeSelection as smooth ground (the menu's second entry). */
	static void VoxelizeSelectionSmooth() { VoxelizeSelection(/*bSmooth*/ true); }

	/** Tools > Mantle > Import block world (.mbw): a file picked on disk, one undoable step, a notification. */
	static void ImportWithDialog();

	/**
	 * A block type name from a mesh name: lower case, a-z 0-9 and _ only, without the SM_ prefix, at most 32 characters,
	 * never "air", made unique against Used (which it is added to) with _2, _3...
	 */
	static FString MakeTypeName(const FString& MeshName, TSet<FString>& Used);

	/** The content folder Voxelize saves a level's tiles in: /Game/MantleVoxelize/<LevelName>. */
	static FString TileFolderFor(const UWorld* World);

	/**
	 * Pixels downscaled (an area average, in linear colour) or upscaled (nearest) to a Side x Side square, as sRGB colours.
	 * Pure (tested).
	 */
	static TArray<FColor> ResampleSquare(TConstArrayView64<FLinearColor> Pixels, int32 Width, int32 Height, int32 Side);

	/** The material's base colour texture, or null when it has none (a flat colour). */
	static UTexture2D* FindBaseColorTexture(UMaterialInterface* Material);

	/** The material's flat base colour (linear): a colour parameter, a constant on Base Color, else mid grey. */
	static FLinearColor FindBaseColor(UMaterialInterface* Material);
};
