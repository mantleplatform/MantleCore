// Copyright Joshua Eger. All Rights Reserved.
#include "MantleNaniteSideFile.h"
#include "MantleEngineTag.h"

#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/PlatformFileManager.h"
#include "Matrix3x4.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "RHI.h"
#include "RenderUtils.h"
#include "Rendering/NaniteResources.h"
#include "Serialization/MemoryReader.h"

// The engine's zlib (this module depends on it): a piece is inflated with the library itself, so the whole stream and nothing after it can be required.
#include "zlib.h"

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleNaniteSideFilePrivate
{
	void SetReason(FString* OutReason, const FString& Reason)
	{
		if (OutReason)
		{
			*OutReason = Reason;
		}
	}

	uint32 ReadU32(const uint8* Bytes)
	{
		uint32 Value = 0;
		FMemory::Memcpy(&Value, Bytes, sizeof(Value));
#if PLATFORM_LITTLE_ENDIAN == 0
		Value = BYTESWAP_ORDER32(Value);
#endif
		return Value;
	}

	/** An array as FArchive's TArray reads one (int32 count, then each item), refusing a count the bytes left cannot hold. */
	template <typename T, typename FItem>
	void ReadArray(FArchive& Ar, TArray<T>& Items, const int64 MinItemBytes, FItem&& Item)
	{
		int32 Num = 0;
		Ar << Num;
		if (Ar.IsError() || Num < 0 || static_cast<int64>(Num) * MinItemBytes > Ar.TotalSize() - Ar.Tell())
		{
			Ar.SetError();
			return;
		}
		Items.SetNum(Num);
		for (T& Each : Items)
		{
			Item(Ar, Each);
		}
	}

	template <typename T>
	void ReadPlainArray(FArchive& Ar, TArray<T>& Items)
	{
		ReadArray(Ar, Items, sizeof(T), [](FArchive& InAr, T& Each) { InAr << Each; });
	}

	/** The engine's operator<< for each (NaniteResources.cpp), which it does not export: the same fields in the same order. */
	void ReadNode(FArchive& Ar, Nanite::FPackedHierarchyNode& Node)
	{
		for (uint32 Index = 0; Index < NANITE_MAX_BVH_NODE_FANOUT; ++Index)
		{
			Ar << Node.LODBounds[Index];
			Ar << Node.Misc0[Index].BoxBoundsCenter;
			Ar << Node.Misc0[Index].MinLODError_MaxParentLODError;
			Ar << Node.Misc1[Index].BoxBoundsExtent;
			Ar << Node.Misc1[Index].ChildStartReference;
			Ar << Node.Misc2[Index].ResourcePageRangeKey;
			Ar << Node.Misc2[Index].GroupPartSize_AssemblyPartIndex;
			Ar << Node.Misc2[Index].NumBones_BoneIndicesOffset;
		}
	}

	/** The root pages' bytes are RootData's, one after another from 0 to its end (the engine's encoder writes them so; 64-bit sums, the fields are uint32). */
	bool RootPagesAreContiguous(const Nanite::FResources& Resources, FString* OutReason)
	{
		uint64 Next = 0;
		for (uint32 Index = 0; Index < Resources.NumRootPages; ++Index)
		{
			const Nanite::FPageStreamingState& Page = Resources.PageStreamingStates[Index];
			if (Page.BulkSize == 0 || static_cast<uint64>(Page.BulkOffset) != Next || Next + Page.BulkSize > static_cast<uint64>(Resources.RootData.Num()))
			{
				SetReason(OutReason, FString::Printf(TEXT("is damaged (root page %u does not follow the one before it)"), Index));
				return false;
			}
			Next += Page.BulkSize;
		}
		if (Next != static_cast<uint64>(Resources.RootData.Num()))
		{
			SetReason(OutReason, TEXT("is damaged (the root pages do not end where the root data does)"));
			return false;
		}
		return true;
	}

	/**
	 * The streamed pages (page table index NumRootPages and up) are the container's pieces 1 to N-1: page i is piece i - NumRootPages + 1, which inflates to exactly the page's BulkSize and starts
	 * in the streamed block where the page's BulkOffset says. No page is more than MaxPageBytes (the engine's staging ring could never hold it) or less than its own PageSize.
	 */
	bool StreamedPagesMatchTable(const Nanite::FResources& Resources, const FMantleNaniteTable& Table, const int64 MaxPageBytes, FString* OutReason)
	{
		const int32 StreamedPages = Resources.PageStreamingStates.Num() - static_cast<int32>(Resources.NumRootPages);
		if (StreamedPages != Table.PageCount())
		{
			SetReason(OutReason, FString::Printf(TEXT("is damaged (its page table has %d streamed pages and its container holds %d)"), StreamedPages, Table.PageCount()));
			return false;
		}
		for (int32 Index = 0; Index < StreamedPages; ++Index)
		{
			const Nanite::FPageStreamingState& Page = Resources.PageStreamingStates[static_cast<int32>(Resources.NumRootPages) + Index];
			const FMantleNaniteTable::FPiece& Piece = Table.Pieces[Index + 1];
			if (Page.BulkSize == 0 || Page.BulkSize < Page.PageSize || Page.BulkSize != Piece.Raw || Page.BulkOffset != Piece.RawStart)
			{
				SetReason(OutReason, FString::Printf(TEXT("is damaged (streamed page %d does not match its piece of the container)"), Index));
				return false;
			}
			if (static_cast<int64>(Page.BulkSize) > MaxPageBytes)
			{
				SetReason(OutReason, FString::Printf(TEXT("has a streamed page of %u bytes, more than the engine's staging ring can ever hold (%lld)"), Page.BulkSize, MaxPageBytes));
				return false;
			}
		}
		return true;
	}

	void ReadPage(FArchive& Ar, Nanite::FPageStreamingState& Page)
	{
		Ar << Page.BulkOffset;
		Ar << Page.BulkSize;
		Ar << Page.PageSize;
		Ar << Page.DependenciesStart;
		Ar << Page.DependenciesNum;
		Ar << Page.MaxHierarchyDepth;
		Ar << Page.Flags;
	}
}

int32 FMantleNaniteTable::FindPage(const uint64 Offset, const uint64 Size) const
{
	// The pages' RawStart rise strictly (every Raw is at least 1): a binary search for the one that starts at Offset.
	int32 Low = 1;
	int32 High = Pieces.Num() - 1;
	while (Low <= High)
	{
		const int32 Mid = Low + (High - Low) / 2;
		const FPiece& Piece = Pieces[Mid];
		if (static_cast<uint64>(Piece.RawStart) == Offset)
		{
			return static_cast<uint64>(Piece.Raw) == Size ? Mid : INDEX_NONE;
		}
		if (static_cast<uint64>(Piece.RawStart) < Offset)
		{
			Low = Mid + 1;
		}
		else
		{
			High = Mid - 1;
		}
	}
	return INDEX_NONE;
}

FString FMantleNaniteSideFile::EngineTag()
{
	return MantleEngineTag::Current();
}

bool FMantleNaniteSideFile::CanRunNanite()
{
	// No window to draw in (a dedicated server, a commandlet), or the null RHI (automated tests): nothing here draws a mesh.
	if (IsRunningDedicatedServer() || !FApp::CanEverRender() || !GIsRHIInitialized || GUsingNullRHI)
	{
		return false;
	}
	// The engine's own answer for this card: shader model 6 with the atomics Nanite's rasterizer needs, and Nanite on in the project.
	return UseNanite(GMaxRHIShaderPlatform);
}

bool FMantleNaniteSideFile::ShouldFetch(const bool bCanRunNanite, const FString& EntryEngine, const FString& ThisEngine)
{
	return bCanRunNanite && MantleEngineTag::Matches(EntryEngine, ThisEngine);
}

bool FMantleNaniteSideFile::ReadTableSize(const TConstArrayView<uint8> FirstBytes, const int64 FileSize, int64& OutTableBytes, FString* OutReason)
{
	using namespace MantleNaniteSideFilePrivate;

	OutTableBytes = 0;
	if (FileSize > MaxFileBytes)
	{
		SetReason(OutReason, FString::Printf(TEXT("is %lld bytes (the most a Nanite file may be is %lld)"), FileSize, MaxFileBytes));
		return false;
	}
	if (FirstBytes.Num() < ContainerHeaderBytes || FileSize < ContainerHeaderBytes || FMemory::Memcmp(FirstBytes.GetData(), "MNZ2", 4) != 0)
	{
		SetReason(OutReason, TEXT("is not an MNZ2 container"));
		return false;
	}
	const uint64 Count = ReadU32(FirstBytes.GetData() + 4);
	if (Count == 0)
	{
		SetReason(OutReason, TEXT("is damaged (its container has no pieces)"));
		return false;
	}
	// 64-bit: a count that cannot fit in the file is a refusal, never an allocation.
	const uint64 TableBytes = ContainerHeaderBytes + Count * ContainerEntryBytes;
	if (TableBytes > static_cast<uint64>(FileSize))
	{
		SetReason(OutReason, FString::Printf(TEXT("is damaged (its table of %llu pieces is longer than the file)"), Count));
		return false;
	}
	OutTableBytes = static_cast<int64>(TableBytes);
	return true;
}

bool FMantleNaniteSideFile::ParseTable(const TConstArrayView<uint8> TableBytes, const int64 FileSize, FMantleNaniteTable& OutTable, FString* OutReason)
{
	using namespace MantleNaniteSideFilePrivate;

	OutTable = FMantleNaniteTable();
	FMantleNaniteTable Parsed;
	int64 NeededBytes = 0;
	if (!ReadTableSize(TableBytes, FileSize, NeededBytes, OutReason))
	{
		return false;
	}
	if (static_cast<int64>(TableBytes.Num()) < NeededBytes)
	{
		SetReason(OutReason, TEXT("is damaged (its table was not read whole)"));
		return false;
	}
	const uint32 Count = ReadU32(TableBytes.GetData() + 4);
	uint64 PackedSum = 0;
	uint64 StreamedRaw = 0;
	for (uint32 Piece = 0; Piece < Count; ++Piece)
	{
		const uint8* Entry = TableBytes.GetData() + ContainerHeaderBytes + static_cast<uint64>(Piece) * ContainerEntryBytes;
		FMantleNaniteTable::FPiece Made;
		Made.Packed = ReadU32(Entry);
		Made.Raw = ReadU32(Entry + 4);
		if (Made.Packed < 8 || Made.Raw < 1 || (Piece == 0 && Made.Raw < 8))
		{
			SetReason(OutReason, FString::Printf(TEXT("is damaged (piece %u says %u bytes packed and %u inflated)"), Piece, Made.Packed, Made.Raw));
			return false;
		}
		if (Made.Raw > MaxInflatedBytes)
		{
			SetReason(OutReason, FString::Printf(TEXT("is damaged (piece %u inflates to %u bytes, the most allowed is %u)"), Piece, Made.Raw, MaxInflatedBytes));
			return false;
		}
		// The pieces follow the table, one after another; they must not run past the file, which is checked as they are added (a table that lies never grows the array past the file).
		if (static_cast<uint64>(NeededBytes) + PackedSum + Made.Packed > static_cast<uint64>(FileSize))
		{
			SetReason(OutReason, FString::Printf(TEXT("is damaged (piece %u runs past the end of the file)"), Piece));
			return false;
		}
		Made.PackedStart = static_cast<uint32>(static_cast<uint64>(NeededBytes) + PackedSum);
		Made.RawStart = Piece == 0 ? 0u : static_cast<uint32>(StreamedRaw);
		PackedSum += Made.Packed;
		if (Piece > 0)
		{
			StreamedRaw += Made.Raw;
		}
		Parsed.Pieces.Add(Made);
	}
	if (static_cast<uint64>(NeededBytes) + PackedSum != static_cast<uint64>(FileSize))
	{
		OutTable = FMantleNaniteTable();
		SetReason(OutReason, FString::Printf(TEXT("is damaged (its pieces make %llu bytes and the file holds %llu behind the table)"), PackedSum, static_cast<uint64>(FileSize - NeededBytes)));
		return false;
	}
	// The MND1 the pieces make: the head, and for a mesh with streamed pages the 4-byte length word and the pages.
	const uint64 Mnd1Bytes = Parsed.Pieces[0].Raw + (Count > 1 ? 4 + StreamedRaw : 0);
	if (Mnd1Bytes > MaxInflatedBytes)
	{
		OutTable = FMantleNaniteTable();
		SetReason(OutReason, FString::Printf(TEXT("says it inflates to %llu bytes (the most allowed is %u)"), Mnd1Bytes, MaxInflatedBytes));
		return false;
	}
	Parsed.FileSize = FileSize;
	Parsed.StreamedBytes = static_cast<int64>(StreamedRaw);
	OutTable = MoveTemp(Parsed);
	return true;
}

bool FMantleNaniteSideFile::InflatePiece(const TConstArrayView<uint8> Packed, uint8* Out, const uint32 Raw, FString* OutReason)
{
	using namespace MantleNaniteSideFilePrivate;

	z_stream Stream;
	FMemory::Memzero(Stream);
	Stream.next_in = const_cast<Bytef*>(Packed.GetData());
	Stream.avail_in = static_cast<uInt>(Packed.Num());
	Stream.next_out = reinterpret_cast<Bytef*>(Out);
	Stream.avail_out = static_cast<uInt>(Raw);
	if (inflateInit(&Stream) != Z_OK)
	{
		SetReason(OutReason, TEXT("cannot be inflated (zlib did not start)"));
		return false;
	}
	// One call with Z_FINISH: Z_STREAM_END only when the whole stream was read and its Adler-32 matched; output past Raw or input that stops early is Z_BUF_ERROR.
	const int Result = inflate(&Stream, Z_FINISH);
	const uint64 TotalIn = Stream.total_in;
	const uint64 TotalOut = Stream.total_out;
	inflateEnd(&Stream);
	if (Result != Z_STREAM_END)
	{
		SetReason(OutReason, TEXT("is damaged (its zlib stream is cut short, damaged, or inflates to more than its table says)"));
		return false;
	}
	if (TotalIn != static_cast<uint64>(Packed.Num()))
	{
		SetReason(OutReason, FString::Printf(TEXT("is damaged (%llu bytes follow the end of its zlib stream)"), static_cast<uint64>(Packed.Num()) - TotalIn));
		return false;
	}
	if (TotalOut != Raw)
	{
		SetReason(OutReason, FString::Printf(TEXT("is damaged (its zlib stream inflates to %llu bytes, its table says %u)"), TotalOut, Raw));
		return false;
	}
	return true;
}

bool FMantleNaniteSideFile::ReadHead(const FString& Path, FMantleNaniteTable& OutTable, TArray<uint8>& OutHead, FString* OutReason)
{
	using namespace MantleNaniteSideFilePrivate;

	OutTable = FMantleNaniteTable();
	OutHead.Reset();
	const TUniquePtr<IFileHandle> File(FPlatformFileManager::Get().GetPlatformFile().OpenRead(*Path));
	if (!File)
	{
		SetReason(OutReason, TEXT("could not be opened"));
		return false;
	}
	const int64 FileSize = File->Size();
	uint8 First[ContainerHeaderBytes] = {};
	int64 TableBytes = 0;
	if (FileSize < ContainerHeaderBytes || !File->ReadAt(First, ContainerHeaderBytes, 0) || !ReadTableSize(TConstArrayView<uint8>(First, ContainerHeaderBytes), FileSize, TableBytes, OutReason))
	{
		if (OutReason && OutReason->IsEmpty())
		{
			*OutReason = TEXT("is not an MNZ2 container");
		}
		return false;
	}
	TArray<uint8> TableData;
	TableData.SetNumUninitialized(static_cast<int32>(TableBytes));
	if (!File->ReadAt(TableData.GetData(), TableBytes, 0))
	{
		SetReason(OutReason, TEXT("could not be read (its table)"));
		return false;
	}
	if (!ParseTable(TableData, FileSize, OutTable, OutReason))
	{
		return false;
	}
	const FMantleNaniteTable::FPiece& Head = OutTable.Pieces[0];
	TArray<uint8> Packed;
	Packed.SetNumUninitialized(static_cast<int32>(Head.Packed));
	if (!File->ReadAt(Packed.GetData(), Head.Packed, Head.PackedStart))
	{
		OutTable = FMantleNaniteTable();
		SetReason(OutReason, TEXT("could not be read (its head)"));
		return false;
	}
	OutHead.SetNumUninitialized(static_cast<int32>(Head.Raw));
	FString Reason;
	if (!InflatePiece(Packed, OutHead.GetData(), Head.Raw, &Reason))
	{
		OutHead.Reset();
		OutTable = FMantleNaniteTable();
		SetReason(OutReason, TEXT("head ") + Reason);
		return false;
	}
	return true;
}

bool FMantleNaniteSideFile::PeekRootBytes(const TConstArrayView<uint8> Head, int64& OutRootBytes)
{
	using namespace MantleNaniteSideFilePrivate;

	OutRootBytes = 0;
	if (Head.Num() < 8 || FMemory::Memcmp(Head.GetData(), "MND1", 4) != 0)
	{
		return false;
	}
	FMemoryReaderView Reader(MakeMemoryView(Head.GetData(), Head.Num()), /*bIsPersistent*/ true);
	Reader.Seek(4);
	uint32 Version = 0;
	Reader << Version;
	FString Engine;
	FString Mesh;
	Reader << Engine << Mesh;
	TArray<int32> Slots;
	ReadPlainArray(Reader, Slots);
	uint32 ResourceFlags = 0;
	Reader << ResourceFlags;
	int32 RootBytes = 0;
	Reader << RootBytes;
	if (Reader.IsError() || Version != FormatVersion || RootBytes < 0 || RootBytes > Reader.TotalSize() - Reader.Tell())
	{
		return false;
	}
	OutRootBytes = RootBytes;
	return true;
}

bool FMantleNaniteSideFile::Read(const TConstArrayView<uint8> Head, const FString& MeshTag, const FMantleNaniteTable& Table, const int64 MaxPageBytes, Nanite::FResources& OutResources,
	TArray<int32>& OutSlotToPublished, FString* OutReason, FMantleNaniteStreamedBlock* OutStreamed)
{
	using namespace MantleNaniteSideFilePrivate;

	OutSlotToPublished.Reset();
	if (OutStreamed)
	{
		*OutStreamed = FMantleNaniteStreamedBlock();
	}
	if (Head.Num() < 8 || FMemory::Memcmp(Head.GetData(), "MND1", 4) != 0)
	{
		SetReason(OutReason, TEXT("is not an MND1"));
		return false;
	}
	FMemoryReaderView Reader(MakeMemoryView(Head.GetData(), Head.Num()), /*bIsPersistent*/ true);
	Reader.Seek(4);
	uint32 Version = 0;
	Reader << Version;
	if (Version != FormatVersion)
	{
		SetReason(OutReason, FString::Printf(TEXT("is MND1 version %u (this reads %u)"), Version, FormatVersion));
		return false;
	}
	FString Engine;
	FString Mesh;
	Reader << Engine << Mesh;
	if (Reader.IsError() || !MantleEngineTag::Matches(Engine, EngineTag()))
	{
		SetReason(OutReason, FString::Printf(TEXT("was baked by engine %s (this is %s)"), *Engine, *EngineTag()));
		return false;
	}
	if (Mesh != MeshTag)
	{
		SetReason(OutReason, FString::Printf(TEXT("belongs to mesh %s"), *Mesh));
		return false;
	}

	ReadPlainArray(Reader, OutSlotToPublished);
	Reader << OutResources.ResourceFlags;
	// The pages are never in the editor's derived-data cache here: with the flag set the engine would ask the cache for them.
	OutResources.ResourceFlags &= ~static_cast<uint32>(NANITE_RESOURCE_FLAG_STREAMING_DATA_IN_DDC);

	// Nanite::FResources::SerializeInternal from RootData on, then TangentPrecision: field 25 is the head's last.
	int32 RootBytes = 0;
	Reader << RootBytes;
	if (Reader.IsError() || RootBytes < 0 || RootBytes > Reader.TotalSize() - Reader.Tell())
	{
		SetReason(OutReason, TEXT("is damaged"));
		return false;
	}
	OutResources.RootData.SetNumUninitialized(RootBytes);
	Reader.Serialize(OutResources.RootData.GetData(), RootBytes);
	ReadArray(Reader, OutResources.PageStreamingStates, 20, [](FArchive& InAr, Nanite::FPageStreamingState& Page) { ReadPage(InAr, Page); });
	ReadArray(Reader, OutResources.HierarchyNodes, sizeof(Nanite::FPackedHierarchyNode),
		[](FArchive& InAr, Nanite::FPackedHierarchyNode& Node) { ReadNode(InAr, Node); });
	ReadPlainArray(Reader, OutResources.HierarchyRootOffsets);
	ReadPlainArray(Reader, OutResources.PageDependencies);
	ReadArray(Reader, OutResources.AssemblyTransforms, 48, [](FArchive& InAr, FMatrix3x4& Transform) { InAr << Transform; });
	ReadPlainArray(Reader, OutResources.AssemblyBoneAttachmentData);
	ReadPlainArray(Reader, OutResources.BoneIndices);
	ReadArray(Reader, OutResources.PageRangeLookup, 4, [](FArchive& InAr, Nanite::FPageRangeKey& Key) { InAr << Key.Value; });
	Reader << OutResources.MeshBounds;
	Reader << OutResources.NumRootPages;
	Reader << OutResources.PositionPrecision;
	Reader << OutResources.NormalPrecision;
	Reader << OutResources.NumInputTriangles;
	Reader << OutResources.NumInputVertices;
	Reader << OutResources.NumClusters;
	Reader << OutResources.VoxelMaterialsMask;
	Reader << OutResources.NumInputCurves;
	// Field 25 is not relied on: the plugin writes 0 whatever the build had, and no runtime code reads FResources::TangentPrecision (only the builder does; each cluster
	// carries its own tangent precision).
	Reader << OutResources.TangentPrecision;

	const uint32 PageCount = static_cast<uint32>(OutResources.PageStreamingStates.Num());
	if (Reader.IsError() || OutResources.RootData.Num() == 0 || PageCount == 0 || OutResources.NumRootPages == 0 || PageCount < OutResources.NumRootPages)
	{
		SetReason(OutReason, TEXT("is damaged"));
		return false;
	}
	// The head ends exactly here: the streamed pages are the container's other pieces, never bytes of the head.
	if (Reader.Tell() != Reader.TotalSize())
	{
		SetReason(OutReason, TEXT("is damaged (bytes follow field 25 of its head)"));
		return false;
	}
	if (!RootPagesAreContiguous(OutResources, OutReason) || !StreamedPagesMatchTable(OutResources, Table, MaxPageBytes, OutReason))
	{
		return false;
	}
	if (Table.PageCount() > 0)
	{
		if (!OutStreamed)
		{
			SetReason(OutReason, TEXT("has streamed pages, which this reader was not asked to serve"));
			return false;
		}
		OutStreamed->Size = Table.StreamedBytes;
	}
	return true;
}

bool FMantleNaniteSideFile::SlotsMatchPublished(const TConstArrayView<int32> SlotToPublished, const int32 PublishedSlotCount, FString* OutReason)
{
	if (SlotToPublished.Num() != PublishedSlotCount)
	{
		MantleNaniteSideFilePrivate::SetReason(OutReason, FString::Printf(TEXT("has %d material slots where the published mesh has %d"), SlotToPublished.Num(), PublishedSlotCount));
		return false;
	}
	for (int32 Slot = 0; Slot < SlotToPublished.Num(); ++Slot)
	{
		if (SlotToPublished[Slot] != Slot)
		{
			MantleNaniteSideFilePrivate::SetReason(OutReason, FString::Printf(TEXT("draws slot %d as published material %d (the game attaches only the published order)"), Slot, SlotToPublished[Slot]));
			return false;
		}
	}
	return true;
}
