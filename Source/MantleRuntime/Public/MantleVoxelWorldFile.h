// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleVoxelSection.h"

class FMantleVoxelGrid;

/**
 * #168: a block world's blocks as a file, MBW1: the uploaded VoxelWorldData asset (backend/internal/cook/voxelworld.go checks it),
 * and a world save (step 5: only the sections that differ from the place's own world). Little-endian:
 *
 *   0   "MBW1"
 *   4   u16 version (1)
 *   6   u8  look (0 blocky, 1 smooth)
 *   7   u8  reserved (0)
 *   8   u16 size x, y, z in blocks (multiples of 16)
 *   14  u16 reserved (0)
 *   16  u32 section count
 *   20  per section, ascending index: u32 index (x + nx * (y + ny * z), in sections), u32 byte length, the section's compressed
 *       form (FMantleVoxelSection::Compress); smooth: then u32 byte length and the section's compressed densities
 *       (FMantleVoxelDensity::Compress)
 *
 * In a world's own file a section not listed is all air; in a save it is as the world's own file has it.
 */
struct MANTLERUNTIME_API FMantleVoxelWorldFile
{
	static constexpr uint16 Version = 1;
	static constexpr int32 HeaderBytes = 20;
	static constexpr int64 MaxBytes = 64ll * 1024 * 1024;

	struct FSection
	{
		int32 Index = 0;
		FMantleVoxelSection Blocks;
		/** Smooth files only. */
		FMantleVoxelDensity Densities;
	};

	FIntVector Size = FIntVector::ZeroValue;
	bool bSmooth = false;
	TArray<FSection> Sections;

	/** Reads and checks a whole file: header, sizes, ascending sections in range, every section's bytes. Any thread. */
	static bool Read(TConstArrayView<uint8> Bytes, FMantleVoxelWorldFile& Out, FString* OutError = nullptr);

	/** Writes Size and Sections (ascending by index). */
	void Write(TArray<uint8>& Out) const;

	/** Every section of Grid that is not all air. */
	static FMantleVoxelWorldFile FromGrid(const FMantleVoxelGrid& Grid);

	/** The given sections of Grid, whatever they hold (a world save: the edited sections; Indices in any order). */
	static FMantleVoxelWorldFile FromGridSections(const FMantleVoxelGrid& Grid, TConstArrayView<int32> Indices);

	/**
	 * Fills an initialized grid of the same size and look; a block id past the grid's types reads as air and is counted in
	 * OutUnknownIds. Only the listed sections change. False when the sizes or looks differ (grid untouched) or a section is over
	 * the grid's cap (grid partly filled).
	 */
	bool ApplyTo(FMantleVoxelGrid& Grid, int32* OutUnknownIds = nullptr, FString* OutError = nullptr) const;
};
