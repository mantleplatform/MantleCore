// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleVoxelSection.h"
#include "MantleVoxelTypes.h"

/**
 * #168: a VoxelWorld's blocks. Sections in X-fastest order, the block types, and a per-column height of the highest
 * block that stops light (the sky light's start, kept up to date by every edit). Game thread; workers read section
 * copies (FMantleVoxelSection shares its data), never the grid.
 */
class MANTLERUNTIME_API FMantleVoxelGrid
{
public:
	enum class ESetResult : uint8
	{
		Changed,
		/** The block already was that id. */
		Same,
		Outside,
		/** The edit would make one section too many that is not one block throughout. */
		OverCap
	};

	/** Size in blocks: each axis a multiple of 16, at most 1024 x 1024 x 256. Every block starts as air. */
	bool Init(const FIntVector& SizeInBlocks, TArray<FMantleVoxelBlockType> Types, FString* OutError = nullptr);

	const FIntVector& GetSize() const { return Size; }
	const FIntVector& GetSectionCounts() const { return Sections; }
	int32 NumSections() const { return SectionData.Num(); }

	const FMantleVoxelBlockTypes& GetTypes() const { return *Types; }
	TSharedRef<const FMantleVoxelBlockTypes, ESPMode::ThreadSafe> GetSharedTypes() const { return Types; }

	FORCEINLINE bool IsInside(const FIntVector& P) const
	{
		return P.X >= 0 && P.Y >= 0 && P.Z >= 0 && P.X < Size.X && P.Y < Size.Y && P.Z < Size.Z;
	}

	/** The block at P, air outside the world. */
	FORCEINLINE uint8 GetBlock(const FIntVector& P) const
	{
		if (!IsInside(P))
		{
			return MantleVoxel::Air;
		}
		const FIntVector S(P.X >> MantleVoxel::SectionShift, P.Y >> MantleVoxel::SectionShift, P.Z >> MantleVoxel::SectionShift);
		return SectionData[SectionIndex(S)].Get(P.X & MantleVoxel::SectionMask, P.Y & MantleVoxel::SectionMask, P.Z & MantleVoxel::SectionMask);
	}

	ESetResult SetBlock(const FIntVector& P, uint8 Id);

	FORCEINLINE bool IsSectionInside(const FIntVector& S) const
	{
		return S.X >= 0 && S.Y >= 0 && S.Z >= 0 && S.X < Sections.X && S.Y < Sections.Y && S.Z < Sections.Z;
	}
	FORCEINLINE int32 SectionIndex(const FIntVector& S) const { return S.X + Sections.X * (S.Y + Sections.Y * S.Z); }
	FIntVector SectionCoord(int32 Index) const;
	const FMantleVoxelSection& GetSection(int32 Index) const { return SectionData[Index]; }

	/** Shrinks a section to its smallest form (after a run of edits). */
	void CompactSection(int32 Index);

	/** Replaces a whole section from 4096 ids (generation). Call RebuildColumnTops after the last. False over the cap. */
	bool SetSectionBlocks(int32 Index, const uint8* Ids);

	void RebuildColumnTops();

	/** The highest Z in column (X, Y) whose block stops light, or -1 (none, or outside the world). */
	FORCEINLINE int32 GetColumnTop(int32 X, int32 Y) const
	{
		return (X >= 0 && Y >= 0 && X < Size.X && Y < Size.Y) ? ColumnTops[X + Size.X * Y] : -1;
	}

	/** The highest Z in column (X, Y) whose block collides, or -1. Walks down from the top. */
	int32 GetSolidTop(int32 X, int32 Y) const;

	int32 GetNumNonUniformSections() const { return NumNonUniform; }
	int64 GetAllocatedBytes() const;

private:
	void UpdateColumnTop(int32 X, int32 Y, int32 Z, uint8 NewId);

	FIntVector Size = FIntVector::ZeroValue;
	FIntVector Sections = FIntVector::ZeroValue;
	TArray<FMantleVoxelSection> SectionData;
	TArray<int16> ColumnTops;
	TSharedRef<FMantleVoxelBlockTypes, ESPMode::ThreadSafe> Types = MakeShared<FMantleVoxelBlockTypes, ESPMode::ThreadSafe>();
	int32 NumNonUniform = 0;
};
