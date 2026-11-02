// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class FMantleVoxelGrid;

/**
 * #168 step 8: one brush stroke on a smooth world, the unit that goes over the wire (21 bytes). The server applies it, orders
 * it and sends it on; every machine applies it again with whole-number maths only (Apply), so all copies of the world stay the
 * same to the last voxel. Positions are in 1/16 voxel.
 */
struct MANTLERUNTIME_API FMantleVoxelStroke
{
	enum class ETool : uint8
	{
		/** Takes density away. */
		Dig,
		/** Adds density, in Material where the ground was empty. */
		Fill,
		/** Pulls density toward a level floor at PlaneZ. */
		Flatten,
		/** Averages each voxel with its six neighbors. */
		Smooth,
		/** Changes the solid voxels' material, not the shape. */
		Paint,
		Count
	};

	static constexpr int32 Fixed = 16;
	/** Brush radius: 0.5 m to 8 m, at 50 cm voxels 1 to 16 voxels. */
	static constexpr int32 MinRadius = 1 * Fixed;
	static constexpr int32 MaxRadius = 16 * Fixed;
	static constexpr int32 EncodedBytes = 21;

	ETool Tool = ETool::Dig;
	uint8 Material = 0;
	/** 1 to 100. */
	uint8 Strength = 50;
	int32 Radius = 2 * Fixed;
	FIntVector Center = FIntVector::ZeroValue;
	/** Flatten: the floor's height. */
	int32 PlaneZ = 0;

	/** Within the limits above (a tool that exists, radius, strength 1 to 100). */
	bool IsValid() const;

	/** The voxels it may touch, inclusive. */
	void GetBounds(FIntVector& OutMin, FIntVector& OutMax) const;

	void Encode(TArray<uint8>& Out) const;
	/** Reads one stroke at Offset (moving it on). False when the bytes run out or it is not valid. */
	static bool Decode(TConstArrayView<uint8> Bytes, int32& Offset, FMantleVoxelStroke& Out);

	/** Applies it to a smooth grid. Returns the voxels changed; the box they lie in goes to OutMin / OutMax (unset when none). */
	int32 Apply(FMantleVoxelGrid& Grid, FIntVector& OutMin, FIntVector& OutMax) const;

	/**
	 * Applies it to one 16 x 16 x 16 section held apart from a grid (a generator's writer, script API 1.59) with the same maths: Origin is
	 * its lowest voxel, Materials and Densities its 4096 voxels x fastest, kept to the grid's rule (a density at or below 0 holds no
	 * material). Voxels outside the section are left alone and read as the nearest one inside it. Returns the voxels changed.
	 */
	int32 ApplyToSection(const FIntVector& Origin, uint8* Materials, int8* Densities, int32 TypeCount) const;

	static const TCHAR* ToolName(ETool Tool);
	/** "dig", "fill", ...; false for anything else. */
	static bool ParseTool(const FString& Name, ETool& OutTool);
};
