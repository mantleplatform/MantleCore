// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/**
 * #168 block worlds: the numbers every voxel piece shares. A VoxelWorld is a grid of 100 cm blocks stored in
 * 16 x 16 x 16 sections; a block is one byte, 0 is air, so a world has up to 255 block types.
 */
namespace MantleVoxel
{
	constexpr int32 SectionShift = 4;
	constexpr int32 SectionSize = 1 << SectionShift;
	constexpr int32 SectionMask = SectionSize - 1;
	constexpr int32 SectionVolume = SectionSize * SectionSize * SectionSize;

	constexpr double BlockSize = 100.0;

	/** The world cap: 1024 x 1024 blocks wide, 256 high. */
	constexpr int32 MaxSizeXY = 1024;
	constexpr int32 MaxSizeZ = 256;

	/** Sections that are not one block throughout, held in memory at most (worst case 64 MB). */
	constexpr int32 MaxNonUniformSections = 16384;

	constexpr uint8 Air = 0;
	constexpr int32 MaxBlockTypes = 255;
	constexpr uint8 MaxLight = 15;

	/** A block's index inside its section: X fastest, then Y, then Z. */
	FORCEINLINE int32 LocalIndex(int32 X, int32 Y, int32 Z)
	{
		return X | (Y << SectionShift) | (Z << (2 * SectionShift));
	}
}

/** How a block type draws: its own pass in the mesh, so each section has at most one draw per class. */
enum class EMantleVoxelRender : uint8
{
	Opaque,
	Cutout,
	Translucent
};

/** One block type. Index 0 of a world's list is always air. */
struct FMantleVoxelBlockType
{
	FName Name;

	/** Until the tile textures come (#168 step 2), the colour the block is drawn with, linear. */
	FLinearColor Color = FLinearColor::White;

	EMantleVoxelRender Render = EMantleVoxelRender::Opaque;

	/** Collides. */
	bool bSolid = true;

	/** 0 to 15: the block glows and lights its neighbours. */
	uint8 Light = 0;

	/** Seconds to break by hand; -1 unbreakable, 0 at once. */
	float Hardness = 1.0f;

	/** What breaking and placing it sound like: an EMantleSurface value. */
	uint8 Surface = 0;

	/** The block's tiles in the world's texture array: top face, side faces, bottom face. */
	uint8 TileTop = 0;
	uint8 TileSide = 0;
	uint8 TileBottom = 0;
};

/** A world's block types with the per-id flags the mesher and the light read, laid out for a lookup per block. */
class MANTLERUNTIME_API FMantleVoxelBlockTypes
{
public:
	enum EFlags : uint8
	{
		/** Drawn (anything but air). */
		FlagVisible = 1 << 0,
		/** Hides the faces next to it and stops light: a drawn opaque block. */
		FlagOccludes = 1 << 1,
		/** Collides. */
		FlagSolid = 1 << 2,
	};

	FMantleVoxelBlockTypes();

	/** Replaces the list. Types[0] must be air; the list holds 1 to 256 entries. */
	bool SetTypes(TArray<FMantleVoxelBlockType> InTypes, FString* OutError = nullptr);

	int32 Num() const { return Types.Num(); }
	const FMantleVoxelBlockType& Get(uint8 Id) const { return Types.IsValidIndex(Id) ? Types[Id] : Types[0]; }
	int32 FindId(FName Name) const;

	FORCEINLINE uint8 GetFlags(uint8 Id) const { return Flags[Id]; }
	FORCEINLINE bool Occludes(uint8 Id) const { return (Flags[Id] & FlagOccludes) != 0; }
	FORCEINLINE bool IsSolid(uint8 Id) const { return (Flags[Id] & FlagSolid) != 0; }
	FORCEINLINE bool IsVisible(uint8 Id) const { return (Flags[Id] & FlagVisible) != 0; }
	FORCEINLINE uint8 GetLight(uint8 Id) const { return Lights[Id]; }

private:
	TArray<FMantleVoxelBlockType> Types;
	uint8 Flags[256];
	uint8 Lights[256];
};
