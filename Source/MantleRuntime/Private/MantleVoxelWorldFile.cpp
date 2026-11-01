// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVoxelWorldFile.h"

#include "MantleVoxelGrid.h"

namespace MantleVoxelWorldFilePrivate
{
	struct FReader
	{
		TConstArrayView<uint8> Bytes;
		int64 Pos = 0;

		bool Has(int64 Count) const { return Pos + Count <= Bytes.Num(); }
		uint8 U8() { return Bytes[Pos++]; }
		uint16 U16()
		{
			const uint16 V = uint16(Bytes[Pos] | (Bytes[Pos + 1] << 8));
			Pos += 2;
			return V;
		}
		uint32 U32()
		{
			const uint32 V = uint32(Bytes[Pos]) | (uint32(Bytes[Pos + 1]) << 8) | (uint32(Bytes[Pos + 2]) << 16) | (uint32(Bytes[Pos + 3]) << 24);
			Pos += 4;
			return V;
		}
	};

	void Put16(TArray<uint8>& Out, uint16 V)
	{
		Out.Add(uint8(V & 0xFF));
		Out.Add(uint8(V >> 8));
	}

	void Put32(TArray<uint8>& Out, uint32 V)
	{
		for (int32 Shift = 0; Shift < 32; Shift += 8)
		{
			Out.Add(uint8((V >> Shift) & 0xFF));
		}
	}

	bool Fail(FString* OutError, const FString& Error)
	{
		if (OutError)
		{
			*OutError = Error;
		}
		return false;
	}
}

bool FMantleVoxelWorldFile::Read(TConstArrayView<uint8> Bytes, FMantleVoxelWorldFile& Out, FString* OutError)
{
	using namespace MantleVoxelWorldFilePrivate;
	if (Bytes.Num() > MaxBytes)
	{
		return Fail(OutError, FString::Printf(TEXT("is %lld bytes; a block world file is at most %lld"), int64(Bytes.Num()), MaxBytes));
	}
	FReader R{ Bytes };
	if (!R.Has(HeaderBytes) || FMemory::Memcmp(Bytes.GetData(), "MBW1", 4) != 0)
	{
		return Fail(OutError, TEXT("is not a block world file (MBW1)"));
	}
	R.Pos = 4;
	const uint16 FileVersion = R.U16();
	const uint8 Look = R.U8();
	R.U8();
	if (FileVersion != Version || Look > 1)
	{
		return Fail(OutError, FString::Printf(TEXT("is MBW1 version %d look %d; this client reads version %d, blocky or smooth"), FileVersion, Look, Version));
	}
	FIntVector Size;
	Size.X = R.U16();
	Size.Y = R.U16();
	Size.Z = R.U16();
	R.U16();
	const int32 Mask = MantleVoxel::SectionMask;
	if (Size.X <= 0 || Size.Y <= 0 || Size.Z <= 0 || (Size.X & Mask) || (Size.Y & Mask) || (Size.Z & Mask)
		|| Size.X > MantleVoxel::MaxSizeXY || Size.Y > MantleVoxel::MaxSizeXY || Size.Z > MantleVoxel::MaxSizeZ)
	{
		return Fail(OutError, FString::Printf(TEXT("has size %s; each side a multiple of 16 up to 1024 x 1024 x 256"), *Size.ToString()));
	}
	const int32 SectionTotal = (Size.X >> MantleVoxel::SectionShift) * (Size.Y >> MantleVoxel::SectionShift) * (Size.Z >> MantleVoxel::SectionShift);
	const uint32 Count = R.U32();
	if (Count > uint32(SectionTotal))
	{
		return Fail(OutError, FString::Printf(TEXT("lists %u sections; the world has %d"), Count, SectionTotal));
	}
	Out = FMantleVoxelWorldFile();
	Out.Size = Size;
	Out.bSmooth = Look == 1;
	Out.Sections.Reserve(Count);
	int64 Previous = -1;
	int32 NonUniform = 0;
	for (uint32 I = 0; I < Count; ++I)
	{
		if (!R.Has(8))
		{
			return Fail(OutError, FString::Printf(TEXT("ends inside section entry %u"), I));
		}
		const uint32 Index = R.U32();
		const uint32 Length = R.U32();
		if (int64(Index) <= Previous || Index >= uint32(SectionTotal))
		{
			return Fail(OutError, FString::Printf(TEXT("section entry %u has index %u; indices ascend below %d"), I, Index, SectionTotal));
		}
		if (!R.Has(Length))
		{
			return Fail(OutError, FString::Printf(TEXT("ends inside section %u"), Index));
		}
		FSection& Section = Out.Sections.AddDefaulted_GetRef();
		Section.Index = int32(Index);
		if (!FMantleVoxelSection::Decompress(Bytes.Slice(R.Pos, Length), Section.Blocks))
		{
			return Fail(OutError, FString::Printf(TEXT("section %u's blocks cannot be read"), Index));
		}
		NonUniform += Section.Blocks.IsUniform() ? 0 : 1;
		R.Pos += Length;
		if (Out.bSmooth)
		{
			if (!R.Has(4))
			{
				return Fail(OutError, FString::Printf(TEXT("ends before section %u's densities"), Index));
			}
			const uint32 DensityLength = R.U32();
			if (!R.Has(DensityLength) || !FMantleVoxelDensity::Decompress(Bytes.Slice(R.Pos, DensityLength), Section.Densities))
			{
				return Fail(OutError, FString::Printf(TEXT("section %u's densities cannot be read"), Index));
			}
			R.Pos += DensityLength;
		}
		Previous = Index;
	}
	if (NonUniform > MantleVoxel::MaxNonUniformSections)
	{
		return Fail(OutError, FString::Printf(TEXT("has %d mixed sections; at most %d"), NonUniform, MantleVoxel::MaxNonUniformSections));
	}
	if (R.Pos != Bytes.Num())
	{
		return Fail(OutError, FString::Printf(TEXT("has %lld bytes after its last section"), int64(Bytes.Num()) - R.Pos));
	}
	return true;
}

void FMantleVoxelWorldFile::Write(TArray<uint8>& Out) const
{
	using namespace MantleVoxelWorldFilePrivate;
	Out.Reset();
	Out.Append(reinterpret_cast<const uint8*>("MBW1"), 4);
	Put16(Out, Version);
	Out.Add(bSmooth ? 1 : 0);
	Out.Add(0);
	Put16(Out, uint16(Size.X));
	Put16(Out, uint16(Size.Y));
	Put16(Out, uint16(Size.Z));
	Put16(Out, 0);
	Put32(Out, uint32(Sections.Num()));
	TArray<uint8> Packed;
	for (const FSection& Section : Sections)
	{
		Section.Blocks.Compress(Packed);
		Put32(Out, uint32(Section.Index));
		Put32(Out, uint32(Packed.Num()));
		Out.Append(Packed);
		if (bSmooth)
		{
			Section.Densities.Compress(Packed);
			Put32(Out, uint32(Packed.Num()));
			Out.Append(Packed);
		}
	}
}

FMantleVoxelWorldFile FMantleVoxelWorldFile::FromGrid(const FMantleVoxelGrid& Grid)
{
	TArray<int32> Indices;
	for (int32 Index = 0; Index < Grid.NumSections(); ++Index)
	{
		const FMantleVoxelSection& Blocks = Grid.GetSection(Index);
		const FMantleVoxelDensity& Densities = Grid.GetDensitySection(Index);
		const bool bEmptyDensity = !Grid.IsSmooth() || (Densities.IsUniform() && Densities.GetUniform() == MantleVoxel::EmptyDensity);
		if (Blocks.IsUniform() && Blocks.GetUniformId() == MantleVoxel::Air && bEmptyDensity)
		{
			continue;
		}
		Indices.Add(Index);
	}
	return FromGridSections(Grid, Indices);
}

FMantleVoxelWorldFile FMantleVoxelWorldFile::FromGridSections(const FMantleVoxelGrid& Grid, TConstArrayView<int32> Indices)
{
	FMantleVoxelWorldFile File;
	File.Size = Grid.GetSize();
	File.bSmooth = Grid.IsSmooth();
	TArray<int32> Sorted(Indices.GetData(), Indices.Num());
	Sorted.Sort();
	for (int32 I = 0; I < Sorted.Num(); ++I)
	{
		const int32 Index = Sorted[I];
		if (Index < 0 || Index >= Grid.NumSections() || (I > 0 && Sorted[I - 1] == Index))
		{
			continue;
		}
		File.Sections.Add({ Index, Grid.GetSection(Index), Grid.GetDensitySection(Index) });
	}
	return File;
}

bool FMantleVoxelWorldFile::ApplyTo(FMantleVoxelGrid& Grid, int32* OutUnknownIds, FString* OutError) const
{
	using namespace MantleVoxelWorldFilePrivate;
	if (Grid.GetSize() != Size)
	{
		return Fail(OutError, FString::Printf(TEXT("is %s blocks; the VoxelWorld is %s"), *Size.ToString(), *Grid.GetSize().ToString()));
	}
	if (Grid.IsSmooth() != bSmooth)
	{
		return Fail(OutError, bSmooth ? TEXT("is a smooth world; the VoxelWorld is blocky") : TEXT("is a blocky world; the VoxelWorld is smooth"));
	}
	const int32 TypeCount = Grid.GetTypes().Num();
	int32 Unknown = 0;
	uint8 Ids[MantleVoxel::SectionVolume];
	for (const FSection& Section : Sections)
	{
		Section.Blocks.GetAll(Ids);
		for (uint8& Id : Ids)
		{
			if (Id >= TypeCount)
			{
				Id = MantleVoxel::Air;
				++Unknown;
			}
		}
		bool bFits = true;
		if (bSmooth)
		{
			int8 Values[MantleVoxel::SectionVolume];
			for (int32 I = 0; I < MantleVoxel::SectionVolume; ++I)
			{
				Values[I] = Section.Densities.Get(I);
			}
			bFits = Grid.SetSectionVoxels(Section.Index, Ids, Values);
		}
		else
		{
			bFits = Grid.SetSectionBlocks(Section.Index, Ids);
		}
		if (!bFits)
		{
			return Fail(OutError, TEXT("has more mixed sections than a world holds"));
		}
	}
	Grid.RebuildColumnTops();
	if (OutUnknownIds)
	{
		*OutUnknownIds = Unknown;
	}
	return true;
}
