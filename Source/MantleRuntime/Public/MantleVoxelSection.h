// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleVoxelTypes.h"
#include "Templates/SharedPointer.h"

/**
 * #168: the blocks of one 16 x 16 x 16 section in one of three forms. Uniform (one id throughout, nothing
 * allocated), 4-bit (a palette of up to 16 ids and 2 KB of indices) or 8-bit (4 KB of ids).
 *
 * The allocated forms are shared, not copied: a worker meshing the section holds a reference to the same data,
 * and the next edit copies it first (copy on write), so the game thread never waits for a worker and a worker
 * never sees half an edit. Edits run on the game thread only.
 */
class MANTLERUNTIME_API FMantleVoxelSection
{
public:
	enum class EFormat : uint8
	{
		Uniform = 0,
		Palette4 = 1,
		Raw8 = 2
	};

	FMantleVoxelSection() = default;
	explicit FMantleVoxelSection(uint8 InUniformId) : UniformId(InUniformId) {}

	FORCEINLINE uint8 Get(int32 Index) const
	{
		if (!Data.IsValid())
		{
			return UniformId;
		}
		const FData& D = *Data;
		if (D.bRaw)
		{
			return D.Bytes[Index];
		}
		const uint8 Packed = D.Bytes[Index >> 1];
		return D.Palette[(Index & 1) ? (Packed >> 4) : (Packed & 0x0F)];
	}

	FORCEINLINE uint8 Get(int32 X, int32 Y, int32 Z) const { return Get(MantleVoxel::LocalIndex(X, Y, Z)); }

	/** Sets one block. True when it changed. Grows the form as needed (uniform to 4-bit to 8-bit). */
	bool Set(int32 Index, uint8 Id);

	/** Replaces every block from 4096 ids in LocalIndex order, in the smallest form that holds them. */
	void SetAll(const uint8* Ids);

	/** Reads every block into 4096 ids in LocalIndex order. */
	void GetAll(uint8* OutIds) const;

	/** Shrinks to the smallest form the blocks fit (uniform, or a 4-bit palette of the ids still used). */
	void Compact();

	EFormat GetFormat() const { return !Data.IsValid() ? EFormat::Uniform : (Data->bRaw ? EFormat::Raw8 : EFormat::Palette4); }
	bool IsUniform() const { return !Data.IsValid(); }
	uint8 GetUniformId() const { return UniformId; }

	/** Bytes this section holds beyond itself (0, about 2 KB or about 4 KB). */
	int32 GetAllocatedBytes() const;

	/** The form as bytes: [format] then [id] | [count][palette][2048 indices] | [4096 ids]. */
	void Serialize(TArray<uint8>& Out) const;
	static bool Deserialize(TConstArrayView<uint8> Bytes, FMantleVoxelSection& Out);

	/** Serialize, zlib-compressed (engine FCompression), with the raw size first (2 bytes): the disk and wire form. */
	void Compress(TArray<uint8>& Out) const;
	static bool Decompress(TConstArrayView<uint8> Bytes, FMantleVoxelSection& Out);

	friend MANTLERUNTIME_API bool operator==(const FMantleVoxelSection& A, const FMantleVoxelSection& B);

private:
	struct FData
	{
		bool bRaw = false;
		uint8 PaletteCount = 0;
		uint8 Palette[16] = {};
		/** 2048 packed indices (4-bit) or 4096 ids (8-bit). */
		TArray<uint8> Bytes;
	};

	/** Data this section alone holds, copied first when a worker still reads it. */
	FData& Mutable();

	uint8 UniformId = MantleVoxel::Air;
	TSharedPtr<FData, ESPMode::ThreadSafe> Data;
};
