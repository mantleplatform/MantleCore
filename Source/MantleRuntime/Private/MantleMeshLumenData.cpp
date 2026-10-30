// Copyright Joshua Eger. All Rights Reserved.
#include "MantleMeshLumenData.h"

#include "DistanceFieldAtlas.h"
#include "MantleAssetStore.h"
#include "MeshCardBuild.h"
#include "Misc/EngineVersion.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleMeshLumenDataPrivate
{
	constexpr uint32 FormatVersion = 1;

	/** No mesh's file comes near this; a count past it is a damaged file, not a big mesh. */
	constexpr int64 MaxMipBytes = 256LL * 1024 * 1024;

	void SetReason(FString* OutReason, const FString& Reason)
	{
		if (OutReason)
		{
			*OutReason = Reason;
		}
	}
}

FMantleMeshLumenData::FMantleMeshLumenData() = default;
FMantleMeshLumenData::~FMantleMeshLumenData() = default;
FMantleMeshLumenData::FMantleMeshLumenData(FMantleMeshLumenData&&) = default;
FMantleMeshLumenData& FMantleMeshLumenData::operator=(FMantleMeshLumenData&&) = default;

int64 FMantleMeshLumenData::GetAllocatedBytes() const
{
	int64 Bytes = 0;
	if (DistanceField)
	{
		Bytes += static_cast<int64>(DistanceField->GetResourceSizeBytes()) + DistanceField->StreamableMips.GetBulkDataSize();
	}
	if (Cards)
	{
		Bytes += static_cast<int64>(Cards->GetResourceSizeBytes());
	}
	return Bytes;
}

FString FMantleMeshLumenFile::EngineTag()
{
	return FEngineVersion::Current().ToString(EVersionComponent::Changelist);
}

TArray<uint8> FMantleMeshLumenFile::Write(const FMantleContentHash& MeshHash, const FDistanceFieldVolumeData& DistanceField, const FCardRepresentationData& Cards)
{
	using namespace MantleMeshLumenDataPrivate;

	TArray<uint8> Bytes;
	if (!DistanceField.IsValid())
	{
		return Bytes;
	}
	FMemoryWriter Writer(Bytes, /*bIsPersistent*/ true);
	uint8 Magic[4] = { 'M', 'L', 'D', '1' };
	Writer.Serialize(Magic, 4);
	uint32 Version = FormatVersion;
	Writer << Version;
	FString Engine = EngineTag();
	FString Mesh = MeshHash.ToString();
	Writer << Engine << Mesh;

	// One mip, the finest: the renderer streams every mip but the resident one from a file on disk
	// (FDistanceFieldSceneData::AsyncUpdate reads StreamableMips through the IO dispatcher or an async file handle), and a
	// runtime mesh has none, so the client makes this the resident mip and never asks for another. The finest is mip 0,
	// in the streamed bytes; a field with no streamed bytes keeps its resident mip.
	FDistanceFieldVolumeData& Field = const_cast<FDistanceFieldVolumeData&>(DistanceField);
	FSparseDistanceFieldMip Mip = Field.Mips[DistanceField::NumMips - 1];
	TArray<uint8> MipBytes = Field.AlwaysLoadedMip;
	const int64 StreamedBytes = Field.StreamableMips.GetBulkDataSize();
	if (StreamedBytes > 0 && Field.Mips[0].BulkSize > 0 && static_cast<int64>(Field.Mips[0].BulkOffset) + Field.Mips[0].BulkSize <= StreamedBytes)
	{
		Mip = Field.Mips[0];
		MipBytes.Reset();
		MipBytes.Append(static_cast<const uint8*>(Field.StreamableMips.LockReadOnly()) + Mip.BulkOffset, Mip.BulkSize);
		Field.StreamableMips.Unlock();
	}
	Mip.BulkOffset = 0;
	Mip.BulkSize = MipBytes.Num();
	Writer << Field.LocalSpaceMeshBounds << Field.bMostlyTwoSided << Mip << MipBytes;
	Writer << const_cast<FCardRepresentationData&>(Cards);
	return Bytes;
}

bool FMantleMeshLumenFile::Read(const TConstArrayView<uint8> Bytes, const FMantleContentHash& MeshHash, FMantleMeshLumenData& OutData, FString* OutReason)
{
	using namespace MantleMeshLumenDataPrivate;

	OutData = FMantleMeshLumenData();
	if (Bytes.Num() < 8 || FMemory::Memcmp(Bytes.GetData(), "MLD1", 4) != 0)
	{
		SetReason(OutReason, TEXT("is not an MLD1"));
		return false;
	}
	// FMemoryReader takes a TArray; the view's bytes are only read.
	const TArray<uint8> Copy(Bytes.GetData(), Bytes.Num());
	FMemoryReader Reader(Copy, /*bIsPersistent*/ true);
	Reader.Seek(4);
	uint32 Version = 0;
	Reader << Version;
	if (Version != FormatVersion)
	{
		SetReason(OutReason, FString::Printf(TEXT("is MLD1 version %u (this client reads %u)"), Version, FormatVersion));
		return false;
	}
	FString Engine;
	FString Mesh;
	Reader << Engine << Mesh;
	if (Reader.IsError() || Engine != EngineTag())
	{
		SetReason(OutReason, FString::Printf(TEXT("was baked by engine %s (this client is %s)"), *Engine, *EngineTag()));
		return false;
	}
	if (Mesh != MeshHash.ToString())
	{
		SetReason(OutReason, FString::Printf(TEXT("belongs to mesh %s"), *Mesh));
		return false;
	}

	TUniquePtr<FDistanceFieldVolumeData> Field = MakeUnique<FDistanceFieldVolumeData>();
	FSparseDistanceFieldMip Mip;
	int32 MipBytes = 0;
	Reader << Field->LocalSpaceMeshBounds << Field->bMostlyTwoSided << Mip << MipBytes;
	if (Reader.IsError() || MipBytes <= 0 || MipBytes > MaxMipBytes || MipBytes > Reader.TotalSize() - Reader.Tell() || Mip.BulkSize != static_cast<uint32>(MipBytes))
	{
		SetReason(OutReason, TEXT("has a damaged distance field"));
		return false;
	}
	Field->AlwaysLoadedMip.SetNumUninitialized(MipBytes);
	Reader.Serialize(Field->AlwaysLoadedMip.GetData(), MipBytes);
	// The one mip is the resident one (the last). The others describe the same field with no bytes behind them: the
	// renderer only asks for a mip whose BulkSize is not 0, so it stays on this one, and IsValid reads mip 0.
	for (int32 Index = 0; Index < DistanceField::NumMips; ++Index)
	{
		Field->Mips[Index] = Mip;
		if (Index != DistanceField::NumMips - 1)
		{
			Field->Mips[Index].BulkSize = 0;
		}
	}
	TUniquePtr<FCardRepresentationData> Cards = MakeUnique<FCardRepresentationData>();
	Reader << *Cards;
	if (Reader.IsError() || Reader.Tell() != Reader.TotalSize() || !Field->IsValid())
	{
		SetReason(OutReason, TEXT("is damaged"));
		return false;
	}
	OutData.DistanceField = MoveTemp(Field);
	OutData.Cards = MoveTemp(Cards);
	return true;
}
