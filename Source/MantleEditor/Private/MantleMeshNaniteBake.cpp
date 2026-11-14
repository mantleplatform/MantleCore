// Copyright Joshua Eger. All Rights Reserved.
#include "MantleMeshNaniteBake.h"

#include "MantleRuntime.h"

#include "Engine/NaniteAssemblyData.h"
#include "Engine/StaticMesh.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "Matrix3x4.h"
#include "Misc/EngineVersion.h"
#include "Rendering/NaniteResources.h"
#include "RenderUtils.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "StaticMeshCompiler.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleMeshNaniteBakePrivate
{
	void SetReason(FString* OutReason, const FString& Reason)
	{
		if (OutReason)
		{
			*OutReason = Reason;
		}
	}

	/** An array as FArchive's TArray writes one (int32 count, then each item), refusing on load a count the bytes left cannot hold. */
	template <typename T, typename FItem>
	void SerializeArray(FArchive& Ar, TArray<T>& Items, const int64 MinItemBytes, FItem&& Item)
	{
		int32 Num = Items.Num();
		Ar << Num;
		if (Ar.IsLoading())
		{
			if (Ar.IsError() || Num < 0 || static_cast<int64>(Num) * MinItemBytes > Ar.TotalSize() - Ar.Tell())
			{
				Ar.SetError();
				return;
			}
			Items.SetNum(Num);
		}
		for (T& Each : Items)
		{
			Item(Ar, Each);
		}
	}

	template <typename T>
	void SerializePlainArray(FArchive& Ar, TArray<T>& Items)
	{
		SerializeArray(Ar, Items, sizeof(T), [](FArchive& InAr, T& Each) { InAr << Each; });
	}

	/** The engine's operator<< for each (NaniteResources.cpp), which it does not export: the same fields in the same order. */
	void SerializeNode(FArchive& Ar, Nanite::FPackedHierarchyNode& Node)
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

	void SerializePage(FArchive& Ar, Nanite::FPageStreamingState& Page)
	{
		Ar << Page.BulkOffset;
		Ar << Page.BulkSize;
		Ar << Page.PageSize;
		Ar << Page.DependenciesStart;
		Ar << Page.DependenciesNum;
		Ar << Page.MaxHierarchyDepth;
		Ar << Page.Flags;
	}

	/** Nanite::FResources::SerializeInternal from RootData on (the flags are the caller's, the streamed pages are left out), then TangentPrecision. */
	void SerializeBody(FArchive& Ar, Nanite::FResources& Resources)
	{
		// Bytes in one go: the same layout as one at a time.
		int32 RootBytes = Resources.RootData.Num();
		Ar << RootBytes;
		if (Ar.IsLoading())
		{
			if (Ar.IsError() || RootBytes < 0 || RootBytes > Ar.TotalSize() - Ar.Tell())
			{
				Ar.SetError();
				return;
			}
			Resources.RootData.SetNumUninitialized(RootBytes);
		}
		Ar.Serialize(Resources.RootData.GetData(), RootBytes);
		SerializeArray(Ar, Resources.PageStreamingStates, 20, [](FArchive& InAr, Nanite::FPageStreamingState& Page) { SerializePage(InAr, Page); });
		SerializeArray(Ar, Resources.HierarchyNodes, sizeof(Nanite::FPackedHierarchyNode),
			[](FArchive& InAr, Nanite::FPackedHierarchyNode& Node) { SerializeNode(InAr, Node); });
		SerializePlainArray(Ar, Resources.HierarchyRootOffsets);
		SerializePlainArray(Ar, Resources.PageDependencies);
		SerializeArray(Ar, Resources.AssemblyTransforms, 48, [](FArchive& InAr, FMatrix3x4& Transform) { InAr << Transform; });
		SerializePlainArray(Ar, Resources.AssemblyBoneAttachmentData);
		SerializePlainArray(Ar, Resources.BoneIndices);
		SerializeArray(Ar, Resources.PageRangeLookup, 4, [](FArchive& InAr, Nanite::FPageRangeKey& Key) { InAr << Key.Value; });
		Ar << Resources.MeshBounds;
		Ar << Resources.NumRootPages;
		Ar << Resources.PositionPrecision;
		Ar << Resources.NormalPrecision;
		Ar << Resources.NumInputTriangles;
		Ar << Resources.NumInputVertices;
		Ar << Resources.NumClusters;
		Ar << Resources.VoxelMaterialsMask;
		Ar << Resources.NumInputCurves;
		Ar << Resources.TangentPrecision;
	}
}

FString FMantleMeshNaniteFile::EngineTag()
{
	return FEngineVersion::Current().ToString(EVersionComponent::Changelist);
}

bool FMantleMeshNaniteFile::Write(const FString& MeshTag, const TConstArrayView<int32> SlotToPublished, const Nanite::FResources& Resources,
	TArray<uint8>& OutBytes, FString& OutReason)
{
	using namespace MantleMeshNaniteBakePrivate;

	OutBytes.Reset();
	if (Resources.RootData.Num() == 0 || Resources.NumRootPages == 0)
	{
		OutReason = TEXT("it has no Nanite data");
		return false;
	}
	if (static_cast<uint32>(Resources.PageStreamingStates.Num()) != Resources.NumRootPages)
	{
		OutReason = FString::Printf(TEXT("only %u of its %d Nanite pages are kept in memory; the rest would stream from a file the game has not got"),
			Resources.NumRootPages, Resources.PageStreamingStates.Num());
		return false;
	}
	FMemoryWriter Writer(OutBytes, /*bIsPersistent*/ true);
	uint8 Magic[4] = { 'M', 'N', 'D', '1' };
	Writer.Serialize(Magic, 4);
	uint32 Version = FormatVersion;
	FString Engine = EngineTag();
	FString Mesh = MeshTag;
	Writer << Version << Engine << Mesh;
	TArray<int32> Slots(SlotToPublished.GetData(), SlotToPublished.Num());
	SerializePlainArray(Writer, Slots);
	// The flags as a cooked build stores them: nothing of this mesh is in the editor's cache.
	uint32 Flags = Resources.ResourceFlags & ~NANITE_RESOURCE_FLAG_STREAMING_DATA_IN_DDC;
	Writer << Flags;
	SerializeBody(Writer, const_cast<Nanite::FResources&>(Resources));
	return true;
}

bool FMantleMeshNaniteFile::Read(const TConstArrayView<uint8> Bytes, const FString& MeshTag, Nanite::FResources& OutResources, TArray<int32>& OutSlotToPublished,
	FString* OutReason)
{
	using namespace MantleMeshNaniteBakePrivate;

	OutSlotToPublished.Reset();
	if (Bytes.Num() < 8 || FMemory::Memcmp(Bytes.GetData(), "MND1", 4) != 0)
	{
		SetReason(OutReason, TEXT("is not an MND1"));
		return false;
	}
	const TArray<uint8> Copy(Bytes.GetData(), Bytes.Num());
	FMemoryReader Reader(Copy, /*bIsPersistent*/ true);
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
	SerializePlainArray(Reader, OutSlotToPublished);
	Reader << OutResources.ResourceFlags;
	SerializeBody(Reader, OutResources);
	if (Reader.IsError() || Reader.Tell() != Reader.TotalSize() || OutResources.RootData.Num() == 0
		|| static_cast<uint32>(OutResources.PageStreamingStates.Num()) != OutResources.NumRootPages)
	{
		SetReason(OutReason, TEXT("is damaged"));
		return false;
	}
	return true;
}

bool FMantleMeshNaniteBake::Bake(const UStaticMesh* Mesh, const TConstArrayView<int32> PublishedSlots, const FString& MeshTag, TArray<uint8>& OutFile,
	FString& OutReason, uint64* OutVoxelMask)
{
	OutFile.Reset();
	if (!Mesh)
	{
		OutReason = TEXT("there is no mesh");
		return false;
	}
	// A mesh with a see-through section stays plain: Nanite draws opaque and cut-out surfaces only.
	for (int32 Slot = 0; Slot < Mesh->GetStaticMaterials().Num(); ++Slot)
	{
		const UMaterialInterface* Material = Mesh->GetStaticMaterials()[Slot].MaterialInterface;
		if (PublishedSlots.Contains(Slot) && Material && IsTranslucentBlendMode(*Material))
		{
			OutReason = FString::Printf(TEXT("its material %s is see-through, which Nanite does not draw"), *Material->GetName());
			return false;
		}
	}

	UPackage* Transient = GetTransientPackage();
	UStaticMesh* Copy = DuplicateObject<UStaticMesh>(const_cast<UStaticMesh*>(Mesh), Transient,
		MakeUniqueObjectName(Transient, UStaticMesh::StaticClass(), FName(*(Mesh->GetName() + TEXT("_NaniteBake")))));
	if (!Copy)
	{
		OutReason = TEXT("it could not be copied");
		return false;
	}
	Copy->SetFlags(RF_Transient);
	FStaticMeshCompilingManager::Get().FinishCompilation({ Copy });

	bool bWritten = false;
	if (ConformToPublished(Copy, PublishedSlots, OutReason))
	{
		FMeshNaniteSettings Settings = Copy->GetNaniteSettings();
		// A mesh made without Nanite gets it in the copy, with the engine's defaults; its foliage keeps its shape far away the way
		// the Medieval pack's pines do (Voxelize, which the game draws with r.Nanite.AllowVoxels=1). A Nanite mesh keeps its own settings.
		if (!Mesh->IsNaniteEnabled())
		{
			Settings.bEnabled = true;
			if (WantsVoxels(Mesh, PublishedSlots))
			{
				Settings.ShapePreservation = ENaniteShapePreservation::Voxelize;
			}
		}
		// Voxelize needs voxels on at startup (r.Nanite.AllowVoxels, which the plugin's Config/Engine.ini sets, or the project's
		// Nanite Foliage); without them the engine quietly builds plain Nanite (StaticMeshBuilder.cpp:251-258).
		if (Settings.ShapePreservation == ENaniteShapePreservation::Voxelize && !NaniteVoxelsSupported())
		{
			UE_LOG(LogMantle, Warning, TEXT("MANTLE_NANITE %s asks for voxels, but voxels are off in this editor (r.Nanite.AllowVoxels 0): it bakes as plain Nanite"),
				*Mesh->GetName());
		}
		Settings.TargetMinimumResidencyInKB = MAX_uint32;
		Copy->SetNaniteSettings(Settings);
		Copy->Build(/*bInSilent*/ true);
		FStaticMeshCompilingManager::Get().FinishCompilation({ Copy });

		const FStaticMeshRenderData* RenderData = Copy->GetRenderData();
		if (!RenderData || !RenderData->HasValidNaniteData())
		{
			OutReason = TEXT("its copy built with no Nanite data");
		}
		else
		{
			if (OutVoxelMask)
			{
				*OutVoxelMask = RenderData->NaniteResourcesPtr->VoxelMaterialsMask;
			}
			// The copy's slots are the upload's glTF materials in order, so slot s is glTF material s.
			TArray<int32> Identity;
			for (int32 Slot = 0; Slot < Copy->GetStaticMaterials().Num(); ++Slot)
			{
				Identity.Add(Slot);
			}
			bWritten = FMantleMeshNaniteFile::Write(MeshTag, Identity, *RenderData->NaniteResourcesPtr, OutFile, OutReason);
		}
	}
	Copy->ClearFlags(RF_Standalone | RF_Public);
	Copy->MarkAsGarbage();
	return bWritten;
}

bool FMantleMeshNaniteBake::ConformAssembly(UStaticMesh* Copy, const TConstArrayView<int32> PublishedSlots, FString& OutReason)
{
	// An assembly's clusters name its final material list: its own sections through the SectionInfoMap, and each part's through the
	// part's MaterialRemap (NaniteAssemblyBuild.cpp, AddAssemblyPart). The parts are other meshes, so their sections cannot be cut:
	// every final slot has to be one the upload draws. Then slot s becomes glTF material s by renumbering the list and both remaps.
	const TArray<FStaticMaterial> OldMaterials = Copy->GetStaticMaterials();
	TArray<int32> ToPublished;
	ToPublished.Init(INDEX_NONE, OldMaterials.Num());
	for (int32 Published = 0; Published < PublishedSlots.Num(); ++Published)
	{
		if (!ToPublished.IsValidIndex(PublishedSlots[Published]))
		{
			OutReason = FString::Printf(TEXT("its upload names material slot %d, which the assembly does not have"), PublishedSlots[Published]);
			return false;
		}
		ToPublished[PublishedSlots[Published]] = Published;
	}
	for (int32 Slot = 0; Slot < ToPublished.Num(); ++Slot)
	{
		if (ToPublished[Slot] == INDEX_NONE)
		{
			OutReason = FString::Printf(TEXT("it is a Nanite assembly whose upload leaves out material slot %d (%s), which an assembly's parts cannot drop"),
				Slot, *OldMaterials[Slot].MaterialSlotName.ToString());
			return false;
		}
	}

	FNaniteAssemblyData& Assembly = Copy->GetNaniteSettings().NaniteAssemblyData;
	for (FNaniteAssemblyPart& Part : Assembly.Parts)
	{
		TArray<int32> Remap = Part.MaterialRemap;
		if (Remap.Num() == 0)
		{
			// Empty means the part's slots map one to one.
			const UStaticMesh* PartMesh = Cast<UStaticMesh>(Part.MeshObjectPath.TryLoad());
			if (!PartMesh)
			{
				OutReason = FString::Printf(TEXT("its assembly part %s could not be loaded"), *Part.MeshObjectPath.ToString());
				return false;
			}
			for (int32 Slot = 0; Slot < PartMesh->GetStaticMaterials().Num(); ++Slot)
			{
				Remap.Add(Slot);
			}
		}
		for (int32& Final : Remap)
		{
			if (!ToPublished.IsValidIndex(Final))
			{
				OutReason = FString::Printf(TEXT("its assembly part %s names material slot %d, which the assembly does not have"), *Part.MeshObjectPath.ToString(), Final);
				return false;
			}
			Final = ToPublished[Final];
		}
		Part.MaterialRemap = MoveTemp(Remap);
	}

	FMeshSectionInfoMap& Sections = Copy->GetSectionInfoMap();
	for (TPair<uint32, FMeshSectionInfo>& Entry : Sections.Map)
	{
		if (ToPublished.IsValidIndex(Entry.Value.MaterialIndex))
		{
			Entry.Value.MaterialIndex = ToPublished[Entry.Value.MaterialIndex];
		}
	}
	Copy->GetOriginalSectionInfoMap().CopyFrom(Sections);

	TArray<FStaticMaterial> Materials;
	for (const int32 Slot : PublishedSlots)
	{
		Materials.Add(OldMaterials[Slot]);
	}
	Copy->SetStaticMaterials(Materials);
	return true;
}

bool FMantleMeshNaniteBake::WantsVoxels(const UStaticMesh* Mesh, const TConstArrayView<int32> PublishedSlots)
{
	if (!Mesh)
	{
		return false;
	}
	// Leaf and needle cards: a cut-out (masked) material seen from both sides, on a slot the upload draws.
	for (const int32 Slot : PublishedSlots)
	{
		const UMaterialInterface* Material = Mesh->GetStaticMaterials().IsValidIndex(Slot) ? Mesh->GetStaticMaterials()[Slot].MaterialInterface.Get() : nullptr;
		if (Material && Material->GetBlendMode() == BLEND_Masked && Material->IsTwoSided())
		{
			return true;
		}
	}
	return false;
}

bool FMantleMeshNaniteBake::ConformToPublished(UStaticMesh* Copy, const TConstArrayView<int32> PublishedSlots, FString& OutReason)
{
	if (!Copy || PublishedSlots.Num() == 0)
	{
		OutReason = TEXT("its upload draws no material");
		return false;
	}
	// A hi-res source model builds Nanite from something other than LOD 0's sections, which this reorders.
	if (Copy->IsHiResMeshDescriptionValid())
	{
		OutReason = TEXT("it has a hi-res source model, which the bake does not read");
		return false;
	}
	if (Copy->IsNaniteAssembly())
	{
		return ConformAssembly(Copy, PublishedSlots, OutReason);
	}
	// Nanite builds from LOD 0 only; the other LODs' sections would name slots that no longer exist.
	Copy->SetNumSourceModels(1);
	FMeshDescription* Description = Copy->GetMeshDescription(0);
	if (!Description)
	{
		OutReason = TEXT("its LOD 0 has no mesh description");
		return false;
	}

	// Section s is the s-th polygon group, and draws SectionInfoMap(0, s).MaterialIndex: the asset slot that the Nanite build
	// stores in its clusters (StaticMeshBuilder.cpp, "Nanite build requires the section material indices ..."). Give each
	// section the glTF material its slot publishes as, and remove the sections the upload does not draw.
	const TArray<FStaticMaterial> OldMaterials = Copy->GetStaticMaterials();
	const FMeshSectionInfoMap OldSections = Copy->GetSectionInfoMap();
	TArray<FPolygonGroupID> Groups;
	for (const FPolygonGroupID Group : Description->PolygonGroups().GetElementIDs())
	{
		Groups.Add(Group);
	}
	TMap<FPolygonGroupID, FMeshSectionInfo> Kept;
	TArray<FTriangleID> Dropped;
	TArray<FPolygonGroupID> DroppedGroups;
	for (int32 Section = 0; Section < Groups.Num(); ++Section)
	{
		FMeshSectionInfo Info = OldSections.Get(0, Section);
		const int32 Published = PublishedSlots.Find(Info.MaterialIndex);
		if (Published == INDEX_NONE)
		{
			const TArrayView<const FTriangleID> Triangles = Description->GetPolygonGroupTriangles(Groups[Section]);
			Dropped.Append(Triangles.GetData(), Triangles.Num());
			DroppedGroups.Add(Groups[Section]);
			continue;
		}
		Info.MaterialIndex = Published;
		Kept.Add(Groups[Section], Info);
	}
	if (Kept.Num() == 0)
	{
		OutReason = TEXT("none of its sections draws a material its upload has");
		return false;
	}
	Description->DeleteTriangles(Dropped); // also deletes the polygon groups it empties
	for (const FPolygonGroupID Group : DroppedGroups)
	{
		if (Description->IsPolygonGroupValid(Group))
		{
			Description->DeletePolygonGroup(Group);
		}
	}

	TArray<FStaticMaterial> Materials;
	for (const int32 Slot : PublishedSlots)
	{
		Materials.Add(OldMaterials.IsValidIndex(Slot) ? OldMaterials[Slot] : FStaticMaterial(UMaterial::GetDefaultMaterial(MD_Surface)));
	}
	Copy->SetStaticMaterials(Materials);
	FMeshSectionInfoMap& Sections = Copy->GetSectionInfoMap();
	Sections.Clear();
	int32 Section = 0;
	for (const FPolygonGroupID Group : Description->PolygonGroups().GetElementIDs())
	{
		// Every group left is a kept one; an empty kept group draws nothing whatever it names.
		const FMeshSectionInfo* Info = Kept.Find(Group);
		Sections.Set(0, Section++, Info ? *Info : FMeshSectionInfo(0));
	}
	Copy->GetOriginalSectionInfoMap().CopyFrom(Sections);
	UStaticMesh::FCommitMeshDescriptionParams Params;
	Params.bMarkPackageDirty = false;
	Params.bUseHashAsGuid = true;
	Copy->CommitMeshDescription(0, Params);
	return true;
}

FString FMantleMeshNaniteBake::CacheSalt()
{
	return FString::Printf(TEXT(" nanite=%u engine=%s voxels=%d assemblies=%d"), FMantleMeshNaniteFile::FormatVersion, *FMantleMeshNaniteFile::EngineTag(),
		NaniteVoxelsSupported() ? 1 : 0, NaniteAssembliesSupported() ? 1 : 0);
}
