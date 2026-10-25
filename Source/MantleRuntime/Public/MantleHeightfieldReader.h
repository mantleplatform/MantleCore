// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/**
 * A Terrain's heights as the cook worker stores them (MHF1, backend/internal/cook/heightfield.go,
 * #466): a Width x Height grid of 16-bit samples, row-major, with the spacing and scale that place
 * them. Sample (X, Y) sits at (X x SpacingX, Y x SpacingY, HeightOffset + value x HeightScale) in the
 * Terrain entity's space, in cm.
 */
struct FMantleHeightfield
{
	int32 Width = 0;
	int32 Height = 0;

	/** cm between samples along X and Y. */
	float SpacingX = 0.0f;
	float SpacingY = 0.0f;

	/** cm per height unit, and the height of value 0. */
	float HeightScale = 0.0f;
	float HeightOffset = 0.0f;

	/** Width x Height values, row-major: sample (X, Y) is Heights[Y * Width + X]. */
	TArray<uint16> Heights;

	/** The height of sample (X, Y) in cm. X and Y must be in range. */
	float HeightAt(const int32 X, const int32 Y) const
	{
		return HeightOffset + static_cast<float>(Heights[Y * Width + X]) * HeightScale;
	}

	/** Where sample (X, Y) is in the Terrain entity's space, cm. */
	FVector3f PositionAt(const int32 X, const int32 Y) const
	{
		return FVector3f(static_cast<float>(X) * SpacingX, static_cast<float>(Y) * SpacingY, HeightAt(X, Y));
	}

	/** The terrain's size along X and Y in cm: (Width - 1) x SpacingX and (Height - 1) x SpacingY. */
	double ExtentX() const { return static_cast<double>(FMath::Max(Width - 1, 0)) * SpacingX; }
	double ExtentY() const { return static_cast<double>(FMath::Max(Height - 1, 0)) * SpacingY; }

	/** Bytes the samples hold, for budgets and logs. */
	int64 GetAllocatedBytes() const { return Heights.GetAllocatedSize(); }
};

/**
 * Reads MHF1 strictly. The bytes were uploaded by a stranger and arrive over the network, so the
 * header is checked field by field before anything is allocated: the magic, version 1, no flags
 * (a hole mask would be bit 0), a grid within limits.json placeAssets.heightfield's sizes, finite
 * spacing and scale above zero, a finite offset, and a file size exactly the header's. The cook
 * enforces the tighter ranges on every upload; this refuses anything the terrain builder could not
 * use. Mirrors cook.ReadHeightfield in the Go writer, which pins the shared fixture
 * schema/fixtures/heightfield-v1.mhf.
 *
 * Layout, little-endian: "MHF1", then uint32 version, flags, width, height, float32 spacingX,
 * spacingY, heightScale, heightOffset (a 36-byte header), then width x height uint16 samples.
 *
 * No UObjects, no asserts, no logging: any thread.
 */
class MANTLERUNTIME_API FMantleHeightfieldReader
{
public:
	/** The version this build reads. Must match HeightfieldVersion in the worker. */
	static constexpr uint32 SupportedVersion = 1;

	static constexpr int32 HeaderBytes = 36;

	/** limits.json placeAssets.heightfield minSize and maxSize, which the cook enforces on every upload. */
	static constexpr int32 MinSize = 2;
	static constexpr int32 MaxSize = 2049;

	/** False, with the reason in OutError and Out left empty, on anything it refuses. */
	static bool Read(TConstArrayView<uint8> Data, FMantleHeightfield& Out, FString* OutError = nullptr);

	/**
	 * The canonical MHF1 for a heightfield, exactly what the worker stores: the plugin's writer (the
	 * editor uploads it as kind heightfield) and the tests' source of valid files. Empty when the
	 * heightfield's arrays disagree with its size.
	 */
	static TArray<uint8> Write(const FMantleHeightfield& Field);
};
