// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCells.h"
#include "MantlePlaceExporter.h"
#include "MantlePlaceAssets.h"
#include "MantleSha256.h"
#include "Misc/Compression.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleCellFilesPrivate
{
	/** 12.5 -> "12.5", 12 -> "12", never "-0": numbers in the index, to 0.01. */
	FString FormatNumber(const double Value)
	{
		FString Text = FString::Printf(TEXT("%.2f"), Value);
		while (Text.EndsWith(TEXT("0")))
		{
			Text.LeftChopInline(1);
		}
		Text.RemoveFromEnd(TEXT("."));
		return Text == TEXT("-0") ? FString(TEXT("0")) : Text;
	}

	/** An entity as the index and the report count it: a StaticMesh is 1 (solid when it collides), an InstancedMesh its instances. */
	void CountEntityForFile(const FMantleEntity& Entity, int64& Instances, int64& Solid)
	{
		if (const FMantleStaticMeshProps* Mesh = Entity.Props.TryGet<FMantleStaticMeshProps>())
		{
			Solid += Mesh->Collision != EMantleCollision::None ? 1 : 0;
		}
		else if (const FMantleInstancedMeshProps* Instanced = Entity.Props.TryGet<FMantleInstancedMeshProps>())
		{
			Instances += Instanced->Instances.Num();
			Solid += Instanced->Mesh.Collision != EMantleCollision::None ? Instanced->Instances.Num() : 0;
		}
	}
}

bool FMantleCellFiles::MakeContainer(const TCHAR* Magic, const FString& Json, TArray<uint8>& OutBytes, FString& OutError)
{
	const FTCHARToUTF8 Utf8(*Json);
	const int32 Raw = Utf8.Length();
	const int32 Bound = FCompression::CompressMemoryBound(NAME_Zlib, Raw);
	TArray<uint8> Packed;
	Packed.SetNumUninitialized(Bound);
	int32 PackedSize = Bound;
	if (!FCompression::CompressMemory(NAME_Zlib, Packed.GetData(), PackedSize, Utf8.Get(), Raw, COMPRESS_BiasMemory))
	{
		OutError = TEXT("the cell data could not be compressed");
		return false;
	}
	OutBytes.Reset();
	OutBytes.Reserve(8 + PackedSize);
	const FTCHARToUTF8 MagicText(Magic);
	OutBytes.Append(reinterpret_cast<const uint8*>(MagicText.Get()), 4);
	const uint32 Length = static_cast<uint32>(Raw);
	OutBytes.Add(static_cast<uint8>(Length & 0xff));
	OutBytes.Add(static_cast<uint8>((Length >> 8) & 0xff));
	OutBytes.Add(static_cast<uint8>((Length >> 16) & 0xff));
	OutBytes.Add(static_cast<uint8>((Length >> 24) & 0xff));
	OutBytes.Append(Packed.GetData(), PackedSize);
	return true;
}

bool FMantleCellFiles::ReadContainer(const TConstArrayView<uint8> Bytes, const TCHAR* Magic, const int64 MaxRaw, FString& OutJson, FString& OutError)
{
	if (Bytes.Num() < 8)
	{
		OutError = TEXT("too short to be a cell container");
		return false;
	}
	const FTCHARToUTF8 MagicText(Magic);
	if (FMemory::Memcmp(Bytes.GetData(), MagicText.Get(), 4) != 0)
	{
		OutError = TEXT("wrong magic");
		return false;
	}
	const uint32 Raw = static_cast<uint32>(Bytes[4]) | (static_cast<uint32>(Bytes[5]) << 8) | (static_cast<uint32>(Bytes[6]) << 16) | (static_cast<uint32>(Bytes[7]) << 24);
	if (static_cast<int64>(Raw) > MaxRaw)
	{
		OutError = FString::Printf(TEXT("the inflated length %u is over the %lld byte limit"), Raw, MaxRaw);
		return false;
	}
	TArray<uint8> Inflated;
	Inflated.SetNumUninitialized(static_cast<int32>(Raw));
	if (!FCompression::UncompressMemory(NAME_Zlib, Inflated.GetData(), Inflated.Num(), Bytes.GetData() + 8, Bytes.Num() - 8))
	{
		OutError = TEXT("the compressed data does not inflate to the length it says");
		return false;
	}
	const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Inflated.GetData()), Inflated.Num());
	OutJson = FString(Text.Length(), Text.Get());
	return true;
}

FString FMantleCellFiles::CellDocument(const TConstArrayView<FMantleEntity> Entities)
{
	FString Json = FString::Printf(TEXT("{\"version\":%d,\"entities\":["), FormatVersion);
	for (int32 Index = 0; Index < Entities.Num(); ++Index)
	{
		Json += Index > 0 ? TEXT(",") : TEXT("");
		Json += FMantlePlaceExporter::EntityToJson(Entities[Index]);
	}
	Json += TEXT("]}");
	return Json;
}

bool FMantleCellFiles::WriteCell(const FMantlePlannedCell& Cell, TArray<FMantleCellFileBlob>& OutFiles, FString& OutError, const int64 MaxRaw)
{
	// Parts (3.2): whole entities in id order, a new file when the next one would pass the limit. Texts are measured as UTF-8.
	constexpr int64 Frame = 64;
	TArray<TArray<int32>> Parts;
	TArray<int64> PartBytes;
	for (int32 Index = 0; Index < Cell.Entities.Num(); ++Index)
	{
		const FTCHARToUTF8 Text(*FMantlePlaceExporter::EntityToJson(Cell.Entities[Index]));
		const int64 Size = Text.Length() + 1;
		if (Parts.Num() == 0 || PartBytes.Last() + Size + Frame > MaxRaw)
		{
			Parts.AddDefaulted();
			PartBytes.Add(0);
		}
		Parts.Last().Add(Index);
		PartBytes.Last() += Size;
	}
	if (Parts.Num() > MaxFilesPerCell)
	{
		OutError = FString::Printf(TEXT("cell %s needs %d files; a cell may have at most %d"), *Cell.Key.ToString(), Parts.Num(), MaxFilesPerCell);
		return false;
	}
	for (int32 PartIndex = 0; PartIndex < Parts.Num(); ++PartIndex)
	{
		TArray<FMantleEntity> Entities;
		for (const int32 Index : Parts[PartIndex])
		{
			Entities.Add(Cell.Entities[Index]);
		}
		FMantleCellFileBlob& File = OutFiles.AddDefaulted_GetRef();
		File.Key = Cell.Key;
		File.Part = PartIndex;
		if (!MakeContainer(CellMagic(), CellDocument(Entities), File.Bytes, OutError))
		{
			return false;
		}
		if (File.Bytes.Num() > MaxCellFileBytes)
		{
			OutError = FString::Printf(TEXT("cell %s file %d is %lld bytes compressed; a cell file may be at most %lld"), *Cell.Key.ToString(), PartIndex,
				static_cast<int64>(File.Bytes.Num()), MaxCellFileBytes);
			return false;
		}
		File.Hash = FMantleSha256::AssetHash(File.Bytes);
		File.Entities = Entities.Num();
		for (const FMantleEntity& Entity : Entities)
		{
			MantleCellFilesPrivate::CountEntityForFile(Entity, File.Instances, File.Solid);
		}
	}
	return true;
}

FString FMantleCellFiles::IndexDocument(const FMantleCellGrid& Grid, const TConstArrayView<FMantleCellIndexEntry> Cells)
{
	using namespace MantleCellFilesPrivate;
	FString Json = FString::Printf(TEXT("{\"version\":%d,\"grid\":{\"levels\":["), FormatVersion);
	for (int32 Index = 0; Index < Grid.Levels.Num(); ++Index)
	{
		Json += FString::Printf(TEXT("%s{\"cellSize\":%lld,\"maxDrawDistance\":%lld}"), Index > 0 ? TEXT(",") : TEXT(""), Grid.Levels[Index].CellSize, Grid.Levels[Index].MaxDrawDistance);
	}
	Json += TEXT("]},\"cells\":[");
	auto Down = [](const double Value) { return FormatNumber(FMath::FloorToDouble(Value * 100.0) / 100.0); };
	auto Up = [](const double Value) { return FormatNumber(FMath::CeilToDouble(Value * 100.0) / 100.0); };
	for (int32 Index = 0; Index < Cells.Num(); ++Index)
	{
		const FMantleCellIndexEntry& Cell = Cells[Index];
		Json += FString::Printf(TEXT("%s{\"level\":%d,\"x\":%d,\"y\":%d,\"bounds\":{\"min\":[%s,%s,%s],\"max\":[%s,%s,%s]},\"drawDistance\":%s,\"files\":["),
			Index > 0 ? TEXT(",") : TEXT(""), Cell.Key.Level, Cell.Key.X, Cell.Key.Y,
			*Down(Cell.Bounds.Min.X), *Down(Cell.Bounds.Min.Y), *Down(Cell.Bounds.Min.Z),
			*Up(Cell.Bounds.Max.X), *Up(Cell.Bounds.Max.Y), *Up(Cell.Bounds.Max.Z),
			*FormatNumber(Cell.DrawDistance));
		for (int32 FileIndex = 0; FileIndex < Cell.Files.Num(); ++FileIndex)
		{
			const FMantleCellFileRef& File = Cell.Files[FileIndex];
			Json += FString::Printf(TEXT("%s{\"id\":\"%s\",\"hash\":\"%s\",\"bytes\":%lld}"), FileIndex > 0 ? TEXT(",") : TEXT(""), *File.Id, *File.Hash, File.Bytes);
		}
		Json += FString::Printf(TEXT("],\"counts\":{\"entities\":%d,\"instances\":%lld,\"solid\":%lld}}"), Cell.Entities, Cell.Instances, Cell.Solid);
	}
	Json += TEXT("]}");
	return Json;
}

bool FMantleCellFiles::WriteIndex(const FMantleCellGrid& Grid, const TConstArrayView<FMantleCellIndexEntry> Cells, TArray<uint8>& OutBytes, FString& OutError)
{
	FString Why;
	if (!Grid.IsValid(&Why))
	{
		OutError = Why;
		return false;
	}
	const FString Json = IndexDocument(Grid, Cells);
	if (FTCHARToUTF8(*Json).Length() > MaxIndexRawBytes)
	{
		OutError = FString::Printf(TEXT("the cell index is over %lld bytes"), MaxIndexRawBytes);
		return false;
	}
	if (!MakeContainer(IndexMagic(), Json, OutBytes, OutError))
	{
		return false;
	}
	if (OutBytes.Num() > MaxIndexBytes)
	{
		OutError = FString::Printf(TEXT("the cell index is %lld bytes compressed; it may be at most %lld"), static_cast<int64>(OutBytes.Num()), MaxIndexBytes);
		return false;
	}
	return true;
}

FString FMantleCellFiles::ProvisionalCellId(const FString& Hash)
{
	FString Hex = Hash;
	Hex.RemoveFromStart(TEXT("sha256:"));
	return FMantlePlaceAssets::ProvisionalId(TEXT("cell:") + Hex);
}

FString FMantleCellFiles::ProvisionalIndexId(const FString& Hash)
{
	FString Hex = Hash;
	Hex.RemoveFromStart(TEXT("sha256:"));
	return FMantlePlaceAssets::ProvisionalId(TEXT("cellindex:") + Hex);
}

bool FMantleCellFiles::BuildOffline(const FMantleCellGrid& Grid, const TConstArrayView<FMantlePlannedCell> Cells, TArray<FMantleCellOfflineFile>& OutFiles, FMantleCellRoot& OutRoot, FString& OutError)
{
	OutFiles.Reset();
	TArray<FMantleCellIndexEntry> Entries;
	for (const FMantlePlannedCell& Cell : Cells)
	{
		TArray<FMantleCellFileBlob> Blobs;
		if (!WriteCell(Cell, Blobs, OutError))
		{
			return false;
		}
		FMantleCellIndexEntry& Entry = Entries.AddDefaulted_GetRef();
		Entry.Key = Cell.Key;
		Entry.Bounds = Cell.Bounds;
		Entry.DrawDistance = Cell.DrawDistance;
		for (FMantleCellFileBlob& Blob : Blobs)
		{
			const FString Id = ProvisionalCellId(Blob.Hash);
			Entry.Files.Add(FMantleCellFileRef{ Id, Blob.Hash, Blob.Bytes.Num() });
			Entry.Entities += Blob.Entities;
			Entry.Instances += Blob.Instances;
			Entry.Solid += Blob.Solid;
			FMantleCellOfflineFile& File = OutFiles.AddDefaulted_GetRef();
			File.Id = Id;
			File.Bytes = MoveTemp(Blob.Bytes);
			File.Hash = Blob.Hash;
		}
	}
	FMantleCellOfflineFile Index;
	Index.bIndex = true;
	if (!WriteIndex(Grid, Entries, Index.Bytes, OutError))
	{
		return false;
	}
	Index.Hash = FMantleSha256::AssetHash(Index.Bytes);
	Index.Id = ProvisionalIndexId(Index.Hash);
	OutRoot.IndexId = Index.Id;
	OutRoot.Hash = Index.Hash;
	OutRoot.Bytes = Index.Bytes.Num();
	OutFiles.Add(MoveTemp(Index));
	return true;
}
