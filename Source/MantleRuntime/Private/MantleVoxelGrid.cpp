// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVoxelGrid.h"

FMantleVoxelBlockTypes::FMantleVoxelBlockTypes()
{
	FMantleVoxelBlockType AirType;
	AirType.Name = TEXT("air");
	AirType.bSolid = false;
	Types.Add(AirType);
	FMemory::Memzero(Flags, sizeof(Flags));
	FMemory::Memzero(Lights, sizeof(Lights));
}

bool FMantleVoxelBlockTypes::SetTypes(TArray<FMantleVoxelBlockType> InTypes, FString* OutError)
{
	if (InTypes.Num() < 1 || InTypes.Num() > MantleVoxel::MaxBlockTypes + 1)
	{
		if (OutError)
		{
			*OutError = FString::Printf(TEXT("a world has 1 to %d block types besides air, not %d"), MantleVoxel::MaxBlockTypes, InTypes.Num() - 1);
		}
		return false;
	}
	// Id 0 is air whatever the list says.
	InTypes[0] = FMantleVoxelBlockType();
	InTypes[0].Name = TEXT("air");
	InTypes[0].bSolid = false;
	Types = MoveTemp(InTypes);
	FMemory::Memzero(Flags, sizeof(Flags));
	FMemory::Memzero(Lights, sizeof(Lights));
	for (int32 Id = 1; Id < Types.Num(); ++Id)
	{
		const FMantleVoxelBlockType& Type = Types[Id];
		uint8 F = FlagVisible;
		if (Type.Render == EMantleVoxelRender::Opaque)
		{
			F |= FlagOccludes;
		}
		if (Type.bSolid)
		{
			F |= FlagSolid;
		}
		Flags[Id] = F;
		Lights[Id] = FMath::Min(Type.Light, MantleVoxel::MaxLight);
	}
	return true;
}

int32 FMantleVoxelBlockTypes::FindId(FName Name) const
{
	for (int32 Id = 0; Id < Types.Num(); ++Id)
	{
		if (Types[Id].Name == Name)
		{
			return Id;
		}
	}
	return INDEX_NONE;
}

bool FMantleVoxelGrid::Init(const FIntVector& SizeInBlocks, TArray<FMantleVoxelBlockType> InTypes, FString* OutError)
{
	auto Fail = [OutError](const FString& Error)
	{
		if (OutError)
		{
			*OutError = Error;
		}
		return false;
	};
	const FIntVector& S = SizeInBlocks;
	if (S.X <= 0 || S.Y <= 0 || S.Z <= 0 || (S.X & MantleVoxel::SectionMask) || (S.Y & MantleVoxel::SectionMask) || (S.Z & MantleVoxel::SectionMask))
	{
		return Fail(FString::Printf(TEXT("size %s: each side is a positive multiple of 16 blocks"), *S.ToString()));
	}
	if (S.X > MantleVoxel::MaxSizeXY || S.Y > MantleVoxel::MaxSizeXY || S.Z > MantleVoxel::MaxSizeZ)
	{
		return Fail(FString::Printf(TEXT("size %s: at most %d x %d x %d blocks"), *S.ToString(), MantleVoxel::MaxSizeXY, MantleVoxel::MaxSizeXY, MantleVoxel::MaxSizeZ));
	}
	TSharedRef<FMantleVoxelBlockTypes, ESPMode::ThreadSafe> NewTypes = MakeShared<FMantleVoxelBlockTypes, ESPMode::ThreadSafe>();
	if (!NewTypes->SetTypes(MoveTemp(InTypes), OutError))
	{
		return false;
	}
	Types = NewTypes;
	Size = S;
	Sections = FIntVector(S.X >> MantleVoxel::SectionShift, S.Y >> MantleVoxel::SectionShift, S.Z >> MantleVoxel::SectionShift);
	SectionData.Reset();
	SectionData.SetNum(Sections.X * Sections.Y * Sections.Z);
	ColumnTops.Init(-1, S.X * S.Y);
	NumNonUniform = 0;
	return true;
}

FIntVector FMantleVoxelGrid::SectionCoord(int32 Index) const
{
	const int32 X = Index % Sections.X;
	const int32 Y = (Index / Sections.X) % Sections.Y;
	const int32 Z = Index / (Sections.X * Sections.Y);
	return FIntVector(X, Y, Z);
}

FMantleVoxelGrid::ESetResult FMantleVoxelGrid::SetBlock(const FIntVector& P, uint8 Id)
{
	if (!IsInside(P) || Id >= Types->Num())
	{
		return ESetResult::Outside;
	}
	const FIntVector S(P.X >> MantleVoxel::SectionShift, P.Y >> MantleVoxel::SectionShift, P.Z >> MantleVoxel::SectionShift);
	FMantleVoxelSection& Section = SectionData[SectionIndex(S)];
	const int32 Local = MantleVoxel::LocalIndex(P.X & MantleVoxel::SectionMask, P.Y & MantleVoxel::SectionMask, P.Z & MantleVoxel::SectionMask);
	if (Section.Get(Local) == Id)
	{
		return ESetResult::Same;
	}
	const bool bWasUniform = Section.IsUniform();
	if (bWasUniform && NumNonUniform >= MantleVoxel::MaxNonUniformSections)
	{
		return ESetResult::OverCap;
	}
	Section.Set(Local, Id);
	if (bWasUniform)
	{
		++NumNonUniform;
	}
	UpdateColumnTop(P.X, P.Y, P.Z, Id);
	return ESetResult::Changed;
}

void FMantleVoxelGrid::CompactSection(int32 Index)
{
	FMantleVoxelSection& Section = SectionData[Index];
	if (Section.IsUniform())
	{
		return;
	}
	Section.Compact();
	if (Section.IsUniform())
	{
		--NumNonUniform;
	}
}

bool FMantleVoxelGrid::SetSectionBlocks(int32 Index, const uint8* Ids)
{
	FMantleVoxelSection& Section = SectionData[Index];
	const bool bWasUniform = Section.IsUniform();
	FMantleVoxelSection New;
	New.SetAll(Ids);
	if (bWasUniform && !New.IsUniform() && NumNonUniform >= MantleVoxel::MaxNonUniformSections)
	{
		return false;
	}
	NumNonUniform += (New.IsUniform() ? 0 : 1) - (bWasUniform ? 0 : 1);
	Section = MoveTemp(New);
	return true;
}

void FMantleVoxelGrid::RebuildColumnTops()
{
	RebuildColumnTops(FIntPoint(0, 0), FIntPoint(Size.X, Size.Y));
}

void FMantleVoxelGrid::RebuildColumnTops(const FIntPoint& Min, const FIntPoint& Max)
{
	for (int32 Y = FMath::Max(0, Min.Y); Y < FMath::Min(Size.Y, Max.Y); ++Y)
	{
		for (int32 X = FMath::Max(0, Min.X); X < FMath::Min(Size.X, Max.X); ++X)
		{
			int16 Top = -1;
			for (int32 Z = Size.Z - 1; Z >= 0; --Z)
			{
				if (Types->Occludes(GetBlock(FIntVector(X, Y, Z))))
				{
					Top = int16(Z);
					break;
				}
			}
			ColumnTops[X + Size.X * Y] = Top;
		}
	}
}

void FMantleVoxelGrid::UpdateColumnTop(int32 X, int32 Y, int32 Z, uint8 NewId)
{
	int16& Top = ColumnTops[X + Size.X * Y];
	if (Types->Occludes(NewId))
	{
		Top = int16(FMath::Max<int32>(Top, Z));
		return;
	}
	if (Z != Top)
	{
		return;
	}
	int32 Below = Z - 1;
	while (Below >= 0 && !Types->Occludes(GetBlock(FIntVector(X, Y, Below))))
	{
		--Below;
	}
	Top = int16(Below);
}

int32 FMantleVoxelGrid::GetSolidTop(int32 X, int32 Y) const
{
	for (int32 Z = Size.Z - 1; Z >= 0; --Z)
	{
		if (Types->IsSolid(GetBlock(FIntVector(X, Y, Z))))
		{
			return Z;
		}
	}
	return -1;
}

int64 FMantleVoxelGrid::GetAllocatedBytes() const
{
	int64 Bytes = SectionData.GetAllocatedSize() + ColumnTops.GetAllocatedSize();
	for (const FMantleVoxelSection& Section : SectionData)
	{
		Bytes += Section.GetAllocatedBytes();
	}
	return Bytes;
}
