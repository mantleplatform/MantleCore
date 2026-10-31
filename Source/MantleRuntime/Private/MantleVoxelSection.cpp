// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVoxelSection.h"

#include "Misc/Compression.h"

namespace MantleVoxelSectionPrivate
{
	constexpr int32 PackedBytes = MantleVoxel::SectionVolume / 2;

	FORCEINLINE void SetNibble(TArray<uint8>& Bytes, int32 Index, uint8 Value)
	{
		uint8& Packed = Bytes[Index >> 1];
		Packed = (Index & 1) ? uint8((Packed & 0x0F) | (Value << 4)) : uint8((Packed & 0xF0) | Value);
	}
}

FMantleVoxelSection::FData& FMantleVoxelSection::Mutable()
{
	check(Data.IsValid());
	if (!Data.IsUnique())
	{
		Data = MakeShared<FData, ESPMode::ThreadSafe>(*Data);
	}
	return *Data;
}

bool FMantleVoxelSection::Set(int32 Index, uint8 Id)
{
	using namespace MantleVoxelSectionPrivate;
	check(Index >= 0 && Index < MantleVoxel::SectionVolume);
	if (!Data.IsValid())
	{
		if (Id == UniformId)
		{
			return false;
		}
		TSharedPtr<FData, ESPMode::ThreadSafe> New = MakeShared<FData, ESPMode::ThreadSafe>();
		New->PaletteCount = 2;
		New->Palette[0] = UniformId;
		New->Palette[1] = Id;
		New->Bytes.SetNumZeroed(PackedBytes);
		SetNibble(New->Bytes, Index, 1);
		Data = MoveTemp(New);
		return true;
	}
	if (Get(Index) == Id)
	{
		return false;
	}
	FData& D = Mutable();
	if (D.bRaw)
	{
		D.Bytes[Index] = Id;
		return true;
	}
	int32 Slot = INDEX_NONE;
	for (int32 I = 0; I < D.PaletteCount; ++I)
	{
		if (D.Palette[I] == Id)
		{
			Slot = I;
			break;
		}
	}
	if (Slot == INDEX_NONE && D.PaletteCount < 16)
	{
		Slot = D.PaletteCount++;
		D.Palette[Slot] = Id;
	}
	if (Slot != INDEX_NONE)
	{
		SetNibble(D.Bytes, Index, uint8(Slot));
		return true;
	}
	// A 17th id: the palette may still hold ids nothing uses any more, so shrink first, else go to 8-bit.
	Compact();
	if (!Data.IsValid() || (!Data->bRaw && Data->PaletteCount < 16))
	{
		return Set(Index, Id);
	}
	uint8 Ids[MantleVoxel::SectionVolume];
	GetAll(Ids);
	Ids[Index] = Id;
	TSharedPtr<FData, ESPMode::ThreadSafe> New = MakeShared<FData, ESPMode::ThreadSafe>();
	New->bRaw = true;
	New->Bytes.SetNumUninitialized(MantleVoxel::SectionVolume);
	FMemory::Memcpy(New->Bytes.GetData(), Ids, MantleVoxel::SectionVolume);
	Data = MoveTemp(New);
	return true;
}

void FMantleVoxelSection::GetAll(uint8* OutIds) const
{
	if (!Data.IsValid())
	{
		FMemory::Memset(OutIds, UniformId, MantleVoxel::SectionVolume);
		return;
	}
	if (Data->bRaw)
	{
		FMemory::Memcpy(OutIds, Data->Bytes.GetData(), MantleVoxel::SectionVolume);
		return;
	}
	const uint8* Packed = Data->Bytes.GetData();
	for (int32 I = 0; I < MantleVoxel::SectionVolume; I += 2)
	{
		const uint8 B = Packed[I >> 1];
		OutIds[I] = Data->Palette[B & 0x0F];
		OutIds[I + 1] = Data->Palette[B >> 4];
	}
}

void FMantleVoxelSection::SetAll(const uint8* Ids)
{
	using namespace MantleVoxelSectionPrivate;
	// Which ids are used, in first-seen order, so the palette is the same for the same blocks.
	int16 SlotOf[256];
	FMemory::Memset(SlotOf, 0xFF, sizeof(SlotOf));
	uint8 Palette[16];
	int32 Count = 0;
	for (int32 I = 0; I < MantleVoxel::SectionVolume; ++I)
	{
		const uint8 Id = Ids[I];
		if (SlotOf[Id] < 0)
		{
			if (Count < 16)
			{
				Palette[Count] = Id;
			}
			SlotOf[Id] = int16(Count++);
		}
	}
	if (Count == 1)
	{
		UniformId = Ids[0];
		Data.Reset();
		return;
	}
	TSharedPtr<FData, ESPMode::ThreadSafe> New = MakeShared<FData, ESPMode::ThreadSafe>();
	if (Count > 16)
	{
		New->bRaw = true;
		New->Bytes.SetNumUninitialized(MantleVoxel::SectionVolume);
		FMemory::Memcpy(New->Bytes.GetData(), Ids, MantleVoxel::SectionVolume);
	}
	else
	{
		New->PaletteCount = uint8(Count);
		FMemory::Memcpy(New->Palette, Palette, Count);
		New->Bytes.SetNumUninitialized(PackedBytes);
		for (int32 I = 0; I < MantleVoxel::SectionVolume; I += 2)
		{
			New->Bytes[I >> 1] = uint8(SlotOf[Ids[I]] | (SlotOf[Ids[I + 1]] << 4));
		}
	}
	UniformId = MantleVoxel::Air;
	Data = MoveTemp(New);
}

void FMantleVoxelSection::Compact()
{
	if (!Data.IsValid())
	{
		return;
	}
	uint8 Ids[MantleVoxel::SectionVolume];
	GetAll(Ids);
	SetAll(Ids);
}

int32 FMantleVoxelSection::GetAllocatedBytes() const
{
	return Data.IsValid() ? int32(sizeof(FData)) + Data->Bytes.GetAllocatedSize() : 0;
}

void FMantleVoxelSection::Serialize(TArray<uint8>& Out) const
{
	Out.Reset();
	const EFormat Format = GetFormat();
	Out.Add(uint8(Format));
	if (Format == EFormat::Uniform)
	{
		Out.Add(UniformId);
		return;
	}
	if (Format == EFormat::Palette4)
	{
		Out.Add(Data->PaletteCount);
		Out.Append(Data->Palette, Data->PaletteCount);
	}
	Out.Append(Data->Bytes);
}

bool FMantleVoxelSection::Deserialize(TConstArrayView<uint8> Bytes, FMantleVoxelSection& Out)
{
	using namespace MantleVoxelSectionPrivate;
	if (Bytes.Num() < 2)
	{
		return false;
	}
	const EFormat Format = EFormat(Bytes[0]);
	if (Format == EFormat::Uniform)
	{
		if (Bytes.Num() != 2)
		{
			return false;
		}
		Out = FMantleVoxelSection(Bytes[1]);
		return true;
	}
	TSharedPtr<FData, ESPMode::ThreadSafe> New = MakeShared<FData, ESPMode::ThreadSafe>();
	if (Format == EFormat::Palette4)
	{
		const int32 Count = Bytes[1];
		if (Count < 2 || Count > 16 || Bytes.Num() != 2 + Count + PackedBytes)
		{
			return false;
		}
		New->PaletteCount = uint8(Count);
		FMemory::Memcpy(New->Palette, Bytes.GetData() + 2, Count);
		New->Bytes.Append(Bytes.GetData() + 2 + Count, PackedBytes);
		// Every index must name a palette entry.
		for (const uint8 Packed : New->Bytes)
		{
			if ((Packed & 0x0F) >= Count || (Packed >> 4) >= Count)
			{
				return false;
			}
		}
	}
	else if (Format == EFormat::Raw8)
	{
		if (Bytes.Num() != 1 + MantleVoxel::SectionVolume)
		{
			return false;
		}
		New->bRaw = true;
		New->Bytes.Append(Bytes.GetData() + 1, MantleVoxel::SectionVolume);
	}
	else
	{
		return false;
	}
	Out = FMantleVoxelSection();
	Out.Data = MoveTemp(New);
	return true;
}

void FMantleVoxelSection::Compress(TArray<uint8>& Out) const
{
	TArray<uint8> Raw;
	Serialize(Raw);
	int32 Bound = FCompression::CompressMemoryBound(NAME_Zlib, Raw.Num());
	Out.SetNumUninitialized(2 + Bound);
	Out[0] = uint8(Raw.Num() & 0xFF);
	Out[1] = uint8(Raw.Num() >> 8);
	int32 Compressed = Bound;
	if (!FCompression::CompressMemory(NAME_Zlib, Out.GetData() + 2, Compressed, Raw.GetData(), Raw.Num()))
	{
		// Zlib does not fail on a buffer of its own bound; keep a valid answer anyway: stored as is.
		Out.SetNum(2);
		Out[1] |= 0x80;
		Out.Append(Raw);
		return;
	}
	Out.SetNum(2 + Compressed);
}

bool FMantleVoxelSection::Decompress(TConstArrayView<uint8> Bytes, FMantleVoxelSection& Out)
{
	if (Bytes.Num() < 3)
	{
		return false;
	}
	const bool bStored = (Bytes[1] & 0x80) != 0;
	const int32 RawSize = Bytes[0] | ((Bytes[1] & 0x7F) << 8);
	if (RawSize < 2 || RawSize > 1 + MantleVoxel::SectionVolume)
	{
		return false;
	}
	if (bStored)
	{
		return Bytes.Num() == 2 + RawSize && Deserialize(Bytes.RightChop(2), Out);
	}
	TArray<uint8> Raw;
	Raw.SetNumUninitialized(RawSize);
	if (!FCompression::UncompressMemory(NAME_Zlib, Raw.GetData(), RawSize, Bytes.GetData() + 2, Bytes.Num() - 2))
	{
		return false;
	}
	return Deserialize(Raw, Out);
}

bool FMantleVoxelDensity::Set(int32 Index, int8 Value)
{
	check(Index >= 0 && Index < MantleVoxel::SectionVolume);
	if (Get(Index) == Value)
	{
		return false;
	}
	if (!Data.IsValid())
	{
		Data = MakeShared<TArray<int8>, ESPMode::ThreadSafe>();
		Data->Init(Uniform, MantleVoxel::SectionVolume);
	}
	else if (!Data.IsUnique())
	{
		Data = MakeShared<TArray<int8>, ESPMode::ThreadSafe>(*Data);
	}
	(*Data)[Index] = Value;
	return true;
}

void FMantleVoxelDensity::SetAll(const int8* Values)
{
	Data.Reset();
	Uniform = Values[0];
	for (int32 I = 1; I < MantleVoxel::SectionVolume; ++I)
	{
		if (Values[I] != Uniform)
		{
			Data = MakeShared<TArray<int8>, ESPMode::ThreadSafe>(Values, MantleVoxel::SectionVolume);
			return;
		}
	}
}

void FMantleVoxelDensity::Compact()
{
	if (Data.IsValid())
	{
		// Held here: SetAll lets go of Data first.
		const TSharedPtr<TArray<int8>, ESPMode::ThreadSafe> Values = Data;
		SetAll(Values->GetData());
	}
}

void FMantleVoxelDensity::Compress(TArray<uint8>& Out) const
{
	TArray<uint8> Raw;
	if (!Data.IsValid())
	{
		Raw = { 0, uint8(Uniform) };
	}
	else
	{
		Raw.Add(1);
		Raw.Append(reinterpret_cast<const uint8*>(Data->GetData()), MantleVoxel::SectionVolume);
	}
	const int32 Bound = FCompression::CompressMemoryBound(NAME_Zlib, Raw.Num());
	Out.SetNumUninitialized(2 + Bound);
	Out[0] = uint8(Raw.Num() & 0xFF);
	Out[1] = uint8(Raw.Num() >> 8);
	int32 Compressed = Bound;
	if (!FCompression::CompressMemory(NAME_Zlib, Out.GetData() + 2, Compressed, Raw.GetData(), Raw.Num()))
	{
		Out.SetNum(2);
		Out[1] |= 0x80;
		Out.Append(Raw);
		return;
	}
	Out.SetNum(2 + Compressed);
}

bool FMantleVoxelDensity::Decompress(TConstArrayView<uint8> Bytes, FMantleVoxelDensity& Out)
{
	if (Bytes.Num() < 3)
	{
		return false;
	}
	const bool bStored = (Bytes[1] & 0x80) != 0;
	const int32 RawSize = Bytes[0] | ((Bytes[1] & 0x7F) << 8);
	if (RawSize != 2 && RawSize != 1 + MantleVoxel::SectionVolume)
	{
		return false;
	}
	TArray<uint8> Raw;
	if (bStored)
	{
		if (Bytes.Num() != 2 + RawSize)
		{
			return false;
		}
		Raw = TArray<uint8>(Bytes.RightChop(2));
	}
	else
	{
		Raw.SetNumUninitialized(RawSize);
		if (!FCompression::UncompressMemory(NAME_Zlib, Raw.GetData(), RawSize, Bytes.GetData() + 2, Bytes.Num() - 2))
		{
			return false;
		}
	}
	if (Raw[0] == 0 && RawSize == 2)
	{
		Out = FMantleVoxelDensity(int8(Raw[1]));
		return true;
	}
	if (Raw[0] != 1 || RawSize != 1 + MantleVoxel::SectionVolume)
	{
		return false;
	}
	Out = FMantleVoxelDensity();
	Out.SetAll(reinterpret_cast<const int8*>(Raw.GetData() + 1));
	return true;
}

bool operator==(const FMantleVoxelDensity& A, const FMantleVoxelDensity& B)
{
	if (A.IsUniform() && B.IsUniform())
	{
		return A.Uniform == B.Uniform;
	}
	for (int32 I = 0; I < MantleVoxel::SectionVolume; ++I)
	{
		if (A.Get(I) != B.Get(I))
		{
			return false;
		}
	}
	return true;
}

bool operator==(const FMantleVoxelSection& A, const FMantleVoxelSection& B)
{
	if (A.IsUniform() && B.IsUniform())
	{
		return A.UniformId == B.UniformId;
	}
	for (int32 I = 0; I < MantleVoxel::SectionVolume; ++I)
	{
		if (A.Get(I) != B.Get(I))
		{
			return false;
		}
	}
	return true;
}
