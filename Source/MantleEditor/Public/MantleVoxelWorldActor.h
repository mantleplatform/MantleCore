// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "MantleVoxelWorldActor.generated.h"

class UBoxComponent;
class UTexture2D;
struct FMantleVoxelWorldFile;

/** How a block type draws (#168): the place file's blockType render. */
UENUM()
enum class EMantleVoxelBlockRender : uint8
{
	/** Solid all through; hides the faces next to it. */
	Opaque,
	/** Fully see-through where the tile's alpha is low (leaves, glass panes). */
	Cutout,
	/** Partly see-through (water, colored glass). */
	Translucent,
};

/**
 * What a block sounds and looks like when it is walked on, broken or hit (#168): the place file's blockType surface. In
 * EMantleSurface's order (MantleSurface.h), so a value converts by its number.
 */
UENUM()
enum class EMantleVoxelBlockSurface : uint8
{
	Default,
	Concrete,
	Stone,
	Brick,
	Asphalt,
	Wood,
	Metal,
	Glass,
	Dirt,
	Grass,
	Sand,
	Mud,
	Snow,
	Ice,
	Water,
	Rubber,
	Plastic,
	Fabric,
	Flesh,
	Gravel,
	Leaf,
};

/** How the tiles are filtered (#168): the place file's VoxelWorld filter. */
UENUM()
enum class EMantleVoxelTileFilter : uint8
{
	/** Sharp pixels: pixel art. */
	Nearest,
	/** Smoothed pixels. */
	Linear,
};

/** Who may change the blocks (#168): the place file's VoxelWorld edit. */
UENUM()
enum class EMantleVoxelWorldEdit : uint8
{
	/** Every player. */
	All,
	/** No player; scripts still can. */
	None,
	/** Players, with the place's scripts deciding each edit. */
	Script,
};

/** Whether players' edits are kept (#168): the place file's VoxelWorld persistence. */
UENUM()
enum class EMantleVoxelWorldPersistence : uint8
{
	/** Every server starts from the published blocks. */
	None,
	/** Each server keeps its own edits. */
	Server,
	/** The place keeps one set of edits for every server. */
	Place,
};

/** How a world's ground looks (#168): the place file's VoxelWorld look. */
UENUM()
enum class EMantleVoxelWorldLook : uint8
{
	/** 100 cm cubes that players break and place. */
	Blocky,
	/** Ground of 50 cm voxels drawn as a smooth surface that players dig, fill, flatten, smooth and paint. */
	Smooth,
};

/** One block type of a Mantle Voxel World (#168). Its id in the world file is its place in the list plus one (0 is air). */
USTRUCT()
struct MANTLEEDITOR_API FMantleVoxelBlockTypeSettings
{
	GENERATED_BODY()

	/** What scripts call it: 1 to 32 of a-z, 0-9 and _, unique in the world, not "air". */
	UPROPERTY(EditAnywhere, Category = "Block")
	FString Name;

	/** The tile on the block's top face: a square texture of the world's Tile Size. */
	UPROPERTY(EditAnywhere, Category = "Block")
	TObjectPtr<UTexture2D> Top;

	/** The tile on the block's four side faces. */
	UPROPERTY(EditAnywhere, Category = "Block")
	TObjectPtr<UTexture2D> Side;

	/** The tile on the block's bottom face. */
	UPROPERTY(EditAnywhere, Category = "Block")
	TObjectPtr<UTexture2D> Bottom;

	UPROPERTY(EditAnywhere, Category = "Block")
	EMantleVoxelBlockRender Render = EMantleVoxelBlockRender::Opaque;

	/** Players and objects collide with it. */
	UPROPERTY(EditAnywhere, Category = "Block")
	bool bSolid = true;

	/** Seconds to break it by hand: 0 at once, -1 unbreakable, at most 60. */
	UPROPERTY(EditAnywhere, Category = "Block", meta = (ClampMin = "-1", ClampMax = "60", UIMin = "-1", UIMax = "60"))
	float Hardness = 1.0f;

	UPROPERTY(EditAnywhere, Category = "Block")
	EMantleVoxelBlockSurface Surface = EMantleVoxelBlockSurface::Default;
};

/** One thing a Mantle Voxel World holds that a place cannot publish, in plain words, with the fix. */
struct MANTLEEDITOR_API FMantleVoxelWorldProblem
{
	FString What;
	FString Remedy;
};

/**
 * A block world in the level (#168 step 6), blocky or smooth: its blocks as the MBW1 file the VoxelWorldData asset uploads
 * (FMantleVoxelWorldFile; none when its scripts make the ground), its size, its block types (a smooth world's ground
 * materials) with their tiles, and how players may change it. The exporter writes it
 * as a VoxelWorld entity at its location (the world's lowest corner, never rotated or scaled); Publish uploads the file and
 * each tile texture. Made by Tools > Mantle > Voxelize selected meshes, or Import block world (.mbw).
 *
 * An editor actor: it lives only in levels a creator publishes from, never in a built place. The box outlines the world in
 * the editor.
 */
UCLASS(placeable, meta = (DisplayName = "Mantle Voxel World"),
	hidecategories = (Advanced, Cooking, DataLayers, HLOD, Input, LevelInstance, Networking, Physics, Replication, WorldPartition, Collision, Rendering))
class MANTLEEDITOR_API AMantleVoxelWorldActor : public AActor
{
	GENERATED_BODY()

public:
	AMantleVoxelWorldActor();

	/** The tag Voxelize gives the meshes it turned into blocks: the exporter leaves them out (FMantlePlaceExporter::IsEngineInfrastructure). */
	static const FName VoxelizedSourceTag;

	/** The folder of the World Outliner the voxelized meshes are moved to. */
	static const FName VoxelizedSourceFolder;

	/**
	 * Blocky: 100 cm cubes. Smooth: ground of 50 cm voxels, whose block types are its ground materials (at most 32). A place may
	 * have one world of each look.
	 */
	UPROPERTY(EditAnywhere, Category = "Mantle")
	EMantleVoxelWorldLook Look = EMantleVoxelWorldLook::Blocky;

	/**
	 * In voxels (100 cm blocks, or 50 cm on a smooth world), x, y and z: each a multiple of 16, at most 1024 x 1024 x 256. Set with
	 * the blocks when Voxelize or Import makes them; set it by hand for a world its scripts generate (OnGenerateSection).
	 */
	UPROPERTY(EditAnywhere, Category = "Mantle")
	FIntVector Size = FIntVector(16);

	/** The edge of one voxel in cm: 100 blocky, 50 smooth. */
	double VoxelSize() const;

	/** How many blocks are not air (for the Details panel). */
	UPROPERTY(VisibleAnywhere, Category = "Mantle")
	int64 BlockCount = 0;

	/** The source actor Voxelize made the most blocks from, and how many (#64): Check warns when it is most of the world. */
	UPROPERTY(VisibleAnywhere, Category = "Mantle")
	FString LargestSource;

	UPROPERTY(VisibleAnywhere, Category = "Mantle")
	int64 LargestSourceBlocks = 0;

	/**
	 * A warning when one voxelized source made more than half of a world of at least one section's worth of blocks: usually
	 * a sky dome, ground plane or backdrop filled solid rather than cubes (#64). Empty otherwise.
	 */
	FString DescribeLargestSource() const;

	/** The block types, id 1 first. A block whose id is past the list publishes as air in the game. */
	UPROPERTY(EditAnywhere, Category = "Mantle")
	TArray<FMantleVoxelBlockTypeSettings> BlockTypes;

	/** The side, in pixels, of every tile texture: 16, 32, 64 or 128. */
	UPROPERTY(EditAnywhere, Category = "Mantle")
	int32 TileSize = 32;

	UPROPERTY(EditAnywhere, Category = "Mantle")
	EMantleVoxelTileFilter Filter = EMantleVoxelTileFilter::Nearest;

	UPROPERTY(EditAnywhere, Category = "Mantle")
	EMantleVoxelWorldEdit Edit = EMantleVoxelWorldEdit::All;

	UPROPERTY(EditAnywhere, Category = "Mantle")
	EMantleVoxelWorldPersistence Persistence = EMantleVoxelWorldPersistence::None;

	/** The blocks, as MBW1 (FMantleVoxelWorldFile::Write). Empty is all air. */
	UPROPERTY()
	TArray<uint8> WorldData;

	/** Replaces the blocks with File's (its size too) and counts them. */
	void SetBlocks(const FMantleVoxelWorldFile& File);

	/** The upload key of its VoxelWorldData: the actor's own, so the exporter, the gather and the converter agree. */
	FString WorldKey() const;

	/**
	 * What of it a place cannot carry: a bad size, a world file that does not read or is another size, too many or too few
	 * block types, a bad or repeated name, a missing tile or one that is not Tile Size square, too many tiles, a hardness out
	 * of range, a block id past the list, a rotation or scale. Empty when it publishes. The place-wide count (at most two
	 * worlds, one of each look) is the caller's.
	 */
	TArray<FMantleVoxelWorldProblem> DescribeProblems() const;

	/** How many of each id the world file holds (index 0 is air); empty when it does not read. */
	TArray<int64> CountBlocks() const;

	virtual void OnConstruction(const FTransform& Transform) override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	void UpdateBounds();

	UPROPERTY()
	TObjectPtr<UBoxComponent> Bounds;
};
