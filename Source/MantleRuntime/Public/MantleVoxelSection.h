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

/**
 * #168 smooth worlds: the densities of one section, beside its FMantleVoxelSection of materials. Uniform (one value, nothing
 * allocated: all air or all solid) or 4096 values, shared copy-on-write like the blocks.
 */
class MANTLERUNTIME_API FMantleVoxelDensity
{
public:
	FMantleVoxelDensity() = default;
	explicit FMantleVoxelDensity(int8 InUniform) : Uniform(InUniform) {}

	FORCEINLINE int8 Get(int32 Index) const { return Data.IsValid() ? (*Data)[Index] : Uniform; }
	FORCEINLINE int8 Get(int32 X, int32 Y, int32 Z) const { return Get(MantleVoxel::LocalIndex(X, Y, Z)); }

	/** True when it changed. */
	bool Set(int32 Index, int8 Value);
	/** 4096 values in LocalIndex order; uniform when they all match. */
	void SetAll(const int8* Values);
	/** Uniform again when every value matches. */
	void Compact();

	bool IsUniform() const { return !Data.IsValid(); }
	int8 GetUniform() const { return Uniform; }
	int32 GetAllocatedBytes() const { return Data.IsValid() ? int32(Data->GetAllocatedSize()) : 0; }

	/** The wire form, zlib-compressed as FMantleVoxelSection::Compress: [0][value] uniform or [1][4096 values]. */
	void Compress(TArray<uint8>& Out) const;
	static bool Decompress(TConstArrayView<uint8> Bytes, FMantleVoxelDensity& Out);

	friend MANTLERUNTIME_API bool operator==(const FMantleVoxelDensity& A, const FMantleVoxelDensity& B);

private:
	int8 Uniform = MantleVoxel::EmptyDensity;
	TSharedPtr<TArray<int8>, ESPMode::ThreadSafe> Data;
};
