// Copyright Joshua Eger. All Rights Reserved.
#include "MantleNaniteSideFile.h"

#include "Matrix3x4.h"
#include "Misc/App.h"
#include "Misc/Compression.h"
#include "Misc/EngineVersion.h"
#include "RHI.h"
#include "RenderUtils.h"
#include "Rendering/NaniteResources.h"
#include "Serialization/MemoryReader.h"

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

FString FMantleNaniteSideFile::EngineTag()
{
	return FEngineVersion::Current().ToString(EVersionComponent::Changelist);
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
	return bCanRunNanite && !EntryEngine.IsEmpty() && EntryEngine == ThisEngine;
}

bool FMantleNaniteSideFile::Unpack(const TConstArrayView<uint8> Container, TArray<uint8>& OutMnd1, FString* OutReason)
{
	using namespace MantleNaniteSideFilePrivate;

	OutMnd1.Reset();
	if (Container.Num() < ContainerHeaderBytes || FMemory::Memcmp(Container.GetData(), "MNZ1", 4) != 0)
	{
		SetReason(OutReason, TEXT("is not an MNZ1 container"));
		return false;
	}
	uint32 RawBytes = 0;
	FMemory::Memcpy(&RawBytes, Container.GetData() + 4, sizeof(RawBytes));
#if PLATFORM_LITTLE_ENDIAN == 0
	RawBytes = BYTESWAP_ORDER32(RawBytes);
#endif
	// The size word is checked before anything is allocated: a damaged one must not take memory (the backend refuses these too).
	if (RawBytes == 0 || RawBytes > MaxInflatedBytes)
	{
		SetReason(OutReason, FString::Printf(TEXT("says it inflates to %u bytes (1 to %u are allowed)"), RawBytes, MaxInflatedBytes));
		return false;
	}
	OutMnd1.SetNumUninitialized(static_cast<int32>(RawBytes));
	const int32 PackedBytes = Container.Num() - ContainerHeaderBytes;
	if (!FCompression::UncompressMemory(NAME_Zlib, OutMnd1.GetData(), OutMnd1.Num(), Container.GetData() + ContainerHeaderBytes, PackedBytes))
	{
		OutMnd1.Reset();
		SetReason(OutReason, TEXT("is damaged (its zlib stream does not inflate to the size it names)"));
		return false;
	}
	return true;
}

bool FMantleNaniteSideFile::Read(const TConstArrayView<uint8> Bytes, const FString& MeshTag, Nanite::FResources& OutResources,
	TArray<int32>& OutSlotToPublished, FString* OutReason)
{
	using namespace MantleNaniteSideFilePrivate;

	OutSlotToPublished.Reset();
	if (Bytes.Num() < 8 || FMemory::Memcmp(Bytes.GetData(), "MND1", 4) != 0)
	{
		SetReason(OutReason, TEXT("is not an MND1"));
		return false;
	}
	FMemoryReaderView Reader(MakeMemoryView(Bytes.GetData(), Bytes.Num()), /*bIsPersistent*/ true);
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
	if (Reader.IsError() || Engine != EngineTag())
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

	// Nanite::FResources::SerializeInternal from RootData on (the streamed pages are not in the file), then TangentPrecision.
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
	Reader << OutResources.TangentPrecision;

	if (Reader.IsError() || Reader.Tell() != Reader.TotalSize() || OutResources.RootData.Num() == 0
		|| OutResources.PageStreamingStates.Num() == 0
		|| static_cast<uint32>(OutResources.PageStreamingStates.Num()) != OutResources.NumRootPages)
	{
		SetReason(OutReason, TEXT("is damaged"));
		return false;
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
