// Copyright Joshua Eger. All Rights Reserved.
#include "MantleSlotSplit.h"

#include "MantleMeshReduce.h"
#include "MantlePlaceAssets.h"
#include "MantlePlaceExporter.h"
#include "MantleRuntime.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "Misc/SecureHash.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshResources.h"
#include "UObject/GCObject.h"
#include "UObject/Package.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleSlotSplitPrivate
{
	/** Bumped when what a part holds changes, so a cached part from before is not reused. */
	const TCHAR* const SplitVersion = TEXT("slotsplit-2");

	/**
	 * The name every polygon group of the source's read (and so every slot of a part) is given, by slot index. The source's
	 * groups carry the source's slot numbers, and each part renames its own to its own numbers before it is built.
	 */
	const TCHAR* const SlotPrefix = TEXT("MantleSplitSlot_");

	struct FPartInfo
	{
		TWeakObjectPtr<const UStaticMesh> Whole;

		/** The source's slot for each of the part's slots. */
		TArray<int32> SourceSlots;
		bool bCarriesCollision = true;
	};

	struct FSplit
	{
		TWeakObjectPtr<const UStaticMesh> Source;

		/** What the parts were made from (see SignatureOf); a different one means the source changed. */
		FString Signature;
		TArray<TObjectPtr<UStaticMesh>> Parts;

		/** Set when the parts could not be made; the attempt is not repeated until the source changes. */
		FString Why;
	};

	/** Holds every part (they are transient, and nothing else references them) until their source changes. */
	class FSplitCache : public FGCObject
	{
	public:
		TMap<FString, FSplit> Splits;

		/** Keyed by the part, which the cache keeps alive, so the pointer stays its own. */
		TMap<const UStaticMesh*, FPartInfo> Infos;

		virtual void AddReferencedObjects(FReferenceCollector& Collector) override
		{
			for (TPair<FString, FSplit>& Entry : Splits)
			{
				Collector.AddReferencedObjects(Entry.Value.Parts);
			}
		}

		virtual FString GetReferencerName() const override
		{
			return TEXT("FMantleSlotSplitCache");
		}

		void Forget(const FSplit& Split)
		{
			for (const TObjectPtr<UStaticMesh>& Part : Split.Parts)
			{
				Infos.Remove(Part.Get());
			}
		}
	};

	FSplitCache& Cache()
	{
		// Never destroyed: a static destructor would run after the object system is gone.
		static FSplitCache* Instance = new FSplitCache();
		return *Instance;
	}

	FName SlotName(int32 Index)
	{
		return FMantleMeshReduce::SlotName(SlotPrefix, Index);
	}

	/** Everything about the source a split depends on: its geometry (the lighting guid changes whenever the mesh is rebuilt), its material slots, and the cap. */
	FString SignatureOf(const UStaticMesh& Source, int32 MaxSections)
	{
		FString Text = FString::Printf(TEXT("%s|%s|%d"), SplitVersion, *Source.GetLightingGuid().ToString(), MaxSections);
		for (const FStaticMaterial& Slot : Source.GetStaticMaterials())
		{
			Text += FString::Printf(TEXT("|%s/%s/%s"), *Slot.MaterialSlotName.ToString(), *Slot.ImportedMaterialSlotName.ToString(),
				Slot.MaterialInterface ? *Slot.MaterialInterface->GetPathName() : TEXT(""));
		}
		return Text;
	}

	/** The source slot a polygon group of a read description is named for, or INDEX_NONE. */
	int32 SlotOfGroup(const FName& Name)
	{
		const FString Text = Name.ToString();
		const FString Prefix(SlotPrefix);
		return Text.StartsWith(Prefix) ? FCString::Atoi(*Text.RightChop(Prefix.Len())) : INDEX_NONE;
	}

	/**
	 * Description cut down to the triangles of the slots in PartSlots, its polygon groups renamed to each slot's place in
	 * PartSlots (so the part's slots are numbered from 0). Returns how many triangles are left.
	 */
	int32 KeepSlots(FMeshDescription& Description, const TArray<int32>& PartSlots)
	{
		FStaticMeshAttributes Attributes(Description);
		TPolygonGroupAttributesRef<FName> SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();

		TMap<FPolygonGroupID, int32> LocalSlot;
		for (const FPolygonGroupID Group : Description.PolygonGroups().GetElementIDs())
		{
			const int32 Source = SlotOfGroup(SlotNames[Group]);
			const int32 Local = PartSlots.IndexOfByKey(Source);
			if (Local != INDEX_NONE)
			{
				LocalSlot.Add(Group, Local);
			}
		}

		// The polygons of every other slot, then what only they used (edges, corners, points), then their emptied groups.
		TArray<FEdgeID> Edges;
		TArray<FVertexInstanceID> Corners;
		TArray<FPolygonGroupID> Unused;
		TArray<FPolygonID> Doomed;
		for (const FPolygonID Polygon : Description.Polygons().GetElementIDs())
		{
			if (!LocalSlot.Contains(Description.GetPolygonPolygonGroup(Polygon)))
			{
				Doomed.Add(Polygon);
			}
		}
		for (const FPolygonID Polygon : Doomed)
		{
			Description.DeletePolygon(Polygon, &Edges, &Corners, &Unused);
		}
		// Corners before edges: an edge's points may only go once no corner uses them.
		TSet<FVertexID> Points;
		for (const FVertexInstanceID Corner : Corners)
		{
			Points.Add(Description.GetVertexInstanceVertex(Corner));
			Description.DeleteVertexInstance(Corner);
		}
		for (const FEdgeID Edge : Edges)
		{
			Points.Add(Description.GetEdgeVertex(Edge, 0));
			Points.Add(Description.GetEdgeVertex(Edge, 1));
			Description.DeleteEdge(Edge);
		}
		for (const FVertexID Point : Points)
		{
			if (Description.IsVertexValid(Point) && Description.GetVertexVertexInstanceIDs(Point).Num() == 0 && Description.GetVertexConnectedEdgeIDs(Point).Num() == 0)
			{
				Description.DeleteVertex(Point);
			}
		}
		TArray<FPolygonGroupID> Groups;
		for (const FPolygonGroupID Group : Description.PolygonGroups().GetElementIDs())
		{
			Groups.Add(Group);
		}
		for (const FPolygonGroupID Group : Groups)
		{
			if (!LocalSlot.Contains(Group))
			{
				// Empty by now: every polygon of it went.
				if (Description.GetPolygonGroupPolygonIDs(Group).Num() == 0)
				{
					Description.DeletePolygonGroup(Group);
				}
			}
		}
		FElementIDRemappings Remappings;
		Description.Compact(Remappings);

		// The renumbering, after Compact, which renumbers the groups.
		FStaticMeshAttributes Compacted(Description);
		TPolygonGroupAttributesRef<FName> CompactedNames = Compacted.GetPolygonGroupMaterialSlotNames();
		TMap<int32, int32> SourceToLocal;
		for (int32 Local = 0; Local < PartSlots.Num(); ++Local)
		{
			SourceToLocal.Add(PartSlots[Local], Local);
		}
		for (const FPolygonGroupID Group : Description.PolygonGroups().GetElementIDs())
		{
			const int32* Local = SourceToLocal.Find(SlotOfGroup(CompactedNames[Group]));
			CompactedNames[Group] = SlotName(Local ? *Local : 0);
		}
		return Description.Triangles().Num();
	}

	/**
	 * Description, a clone of a source mesh's own LOD 0 (full detail), with its polygon groups named by the slot they draw with, as the
	 * read of a render LOD names them (ReadRenderLOD): the slot the mesh's section info map gives each group (the build's own rule), else
	 * the slot whose imported name the group carries. False when a group's slot cannot be told.
	 */
	bool NameGroupsBySlot(const UStaticMesh& Source, FMeshDescription& Description)
	{
		FStaticMeshAttributes Attributes(Description);
		TPolygonGroupAttributesRef<FName> SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
		const FMeshSectionInfoMap& Map = Source.GetSectionInfoMap();
		int32 Index = 0;
		for (const FPolygonGroupID Group : Description.PolygonGroups().GetElementIDs())
		{
			int32 Slot = INDEX_NONE;
			if (Map.IsValidSection(0, Index))
			{
				Slot = Map.Get(0, Index).MaterialIndex;
			}
			else
			{
				const TArray<FStaticMaterial>& Materials = Source.GetStaticMaterials();
				for (int32 Candidate = 0; Candidate < Materials.Num(); ++Candidate)
				{
					if (Materials[Candidate].ImportedMaterialSlotName == SlotNames[Group])
					{
						Slot = Candidate;
						break;
					}
				}
			}
			if (Slot == INDEX_NONE)
			{
				return false;
			}
			SlotNames[Group] = SlotName(Slot);
			++Index;
		}
		return true;
	}

	/** Whether the source's collision is simple shapes (carried by part 0 alone), as the converter reads them (ReadSimpleCollision). */
	bool HasSimpleShapes(const UStaticMesh& Source)
	{
		const UBodySetup* Body = Source.GetBodySetup();
		return Body && Body->GetCollisionTraceFlag() != CTF_UseComplexAsSimple && Body->AggGeom.GetElementCount() > 0;
	}

	/**
	 * One part: the source's slots PartSlots, built from the LOD descriptions that have triangles for them. Null with OutWhy when
	 * the engine cannot build it.
	 */
	UStaticMesh* BuildPart(const UStaticMesh& Source, const FString& Signature, const TArray<FMeshDescription>& Descriptions, const int32 PartIndex,
		const TArray<int32>& PartSlots, const bool bFullDetailNanite, FString& OutWhy)
	{
		TArray<FMeshDescription> Cut;
		for (int32 LODIndex = 0; LODIndex < Descriptions.Num(); ++LODIndex)
		{
			FMeshDescription Part = Descriptions[LODIndex];
			// A lower LOD that has dropped all of the part's slots ends the part's LODs there.
			if (KeepSlots(Part, PartSlots) == 0)
			{
				break;
			}
			Cut.Add(MoveTemp(Part));
		}
		if (Cut.Num() == 0)
		{
			OutWhy = FString::Printf(TEXT("part %d of '%s' has no triangles"), PartIndex, *Source.GetName());
			return nullptr;
		}

		UObject* Outer = GetTransientPackage();
		const FString MeshName = FString::Printf(TEXT("MantleSplit_%s_%d_%s"), *Source.GetName().Left(60), PartIndex, *FMD5::HashAnsiString(*Signature).Left(8));
		UStaticMesh* Baked = NewObject<UStaticMesh>(Outer, FMantlePlaceAssets::UniqueBakedMeshName(Outer, MeshName), RF_Transient);

		// The part's own slots, from 0, each the source's material for the slot it came from.
		for (int32 Local = 0; Local < PartSlots.Num(); ++Local)
		{
			FStaticMaterial Slot;
			if (Source.GetStaticMaterials().IsValidIndex(PartSlots[Local]))
			{
				Slot = Source.GetStaticMaterials()[PartSlots[Local]];
			}
			Slot.ImportedMaterialSlotName = SlotName(Local);
			Baked->GetStaticMaterials().Add(MoveTemp(Slot));
		}

		Baked->bAllowCPUAccess = true;
		Baked->SetLightMapCoordinateIndex(Source.GetLightMapCoordinateIndex());
		Baked->SetLightMapResolution(Source.GetLightMapResolution());
		Baked->SetNumSourceModels(Cut.Num());
		if (bFullDetailNanite)
		{
			// A Nanite source: the part is a Nanite mesh made from the source's own full-detail triangles, with the source's Nanite settings, so the
			// converter bakes its Nanite data as it does the source's, and the plain model it publishes is the engine's own fallback for the part.
			Baked->GetNaniteSettings() = Source.GetNaniteSettings();
		}
		for (int32 LODIndex = 0; LODIndex < Cut.Num(); ++LODIndex)
		{
			// SetNumSourceModels gives a new LOD that has no mesh description yet a 50% reduction of the LOD before it, and the build
			// would then replace this LOD's own description with that reduction. Every LOD here has its own: reduce none.
			Baked->GetSourceModel(LODIndex).ReductionSettings = FMeshReductionSettings();
			FMeshBuildSettings& Settings = Baked->GetSourceModel(LODIndex).BuildSettings;
			if (LODIndex < Source.GetNumSourceModels())
			{
				// The precision the source stores its tangents and UVs at: a part is as exact as the original.
				Settings.bUseHighPrecisionTangentBasis = Source.GetSourceModel(LODIndex).BuildSettings.bUseHighPrecisionTangentBasis;
				Settings.bUseFullPrecisionUVs = Source.GetSourceModel(LODIndex).BuildSettings.bUseFullPrecisionUVs;
			}
			// The geometry is final: nothing recomputed, and no distance field or Lumen cards, which only an editor view would use.
			Settings.bRecomputeNormals = false;
			Settings.bRecomputeTangents = false;
			if (bFullDetailNanite && Source.GetNumSourceModels() > 0)
			{
				// The source's own description: built as the source builds it.
				Settings.bRecomputeNormals = Source.GetSourceModel(0).BuildSettings.bRecomputeNormals;
				Settings.bRecomputeTangents = Source.GetSourceModel(0).BuildSettings.bRecomputeTangents;
				Settings.bRemoveDegenerates = Source.GetSourceModel(0).BuildSettings.bRemoveDegenerates;
			}
			Settings.bGenerateLightmapUVs = false;
			Settings.bBuildReversedIndexBuffer = false;
			Settings.DistanceFieldResolutionScale = 0.0f;
			Settings.MaxLumenMeshCards = 0;
		}

		UStaticMesh::FBuildMeshDescriptionsParams Params;
		Params.bMarkPackageDirty = false;
		Params.bAllowCpuAccess = true;
		TArray<const FMeshDescription*> Pointers;
		for (const FMeshDescription& Description : Cut)
		{
			Pointers.Add(&Description);
		}
		if (!Baked->BuildFromMeshDescriptions(Pointers, Params))
		{
			OutWhy = FString::Printf(TEXT("the engine could not build part %d of '%s'"), PartIndex, *Source.GetName());
			return nullptr;
		}
		FStaticMeshRenderData* BakedData = Baked->GetRenderData();
		if (!BakedData || !BakedData->LODResources.IsValidIndex(0) || BakedData->LODResources[0].GetNumTriangles() <= 0)
		{
			OutWhy = FString::Printf(TEXT("part %d of '%s' has no triangles"), PartIndex, *Source.GetName());
			return nullptr;
		}

		// The LODs switch where the source's do (the build gives its own, by LOD number).
		const FStaticMeshRenderData* SourceData = Source.GetRenderData();
		for (int32 LODIndex = 0; SourceData && LODIndex < BakedData->LODResources.Num() && LODIndex < MAX_STATIC_MESH_LODS; ++LODIndex)
		{
			BakedData->ScreenSize[LODIndex].Default = SourceData->ScreenSize[LODIndex].Default;
		}

		// Its collision: the source's own for part 0, and for the rest what the source gave every part (its triangles), or nothing at all
		// when part 0 carries simple shapes (the exporter then publishes those parts with no collision). A source with no body setup has
		// no collision, so a part gets none: its body is left empty and CarriesCollision says false.
		Baked->CreateBodySetup();
		if (UBodySetup* Body = Baked->GetBodySetup())
		{
			if (const UBodySetup* SourceBody = Source.GetBodySetup())
			{
				Body->CollisionTraceFlag = SourceBody->CollisionTraceFlag;
				if (PartIndex == 0)
				{
					Body->AggGeom = SourceBody->AggGeom;
				}
			}
			Body->InvalidatePhysicsData();
		}
		return Baked;
	}
}

int32 FMantleSlotSplit::DefaultMaxSections()
{
	return FMantlePlaceAssetLimits::FromContract().MaxSections;
}

bool FMantleSlotSplit::Split(const UStaticMesh* Source, int32 MaxSections, TArray<UStaticMesh*>& OutParts, FString& OutWhy)
{
	using namespace MantleSlotSplitPrivate;
	OutParts.Reset();
	OutWhy.Reset();
	MaxSections = FMath::Max(MaxSections, 1);
	if (!IsValid(Source) || FMantlePlaceAssets::IsLibraryAsset(Source) || IsPart(Source))
	{
		return false;
	}
	const TArray<int32> DrawnSlots = FMantlePlaceAssets::UploadSlots(Source);
	if (DrawnSlots.Num() <= MaxSections)
	{
		return false;
	}

	FSplitCache& Store = Cache();

	// A split whose source is gone is not needed again.
	for (auto It = Store.Splits.CreateIterator(); It; ++It)
	{
		if (!It.Value().Source.IsValid())
		{
			Store.Forget(It.Value());
			It.RemoveCurrent();
		}
	}

	const FString Key = Source->GetPathName();
	const FString Signature = SignatureOf(*Source, MaxSections);
	if (const FSplit* Found = Store.Splits.Find(Key))
	{
		if (Found->Source.Get() == Source && Found->Signature == Signature)
		{
			if (!Found->Why.IsEmpty())
			{
				OutWhy = Found->Why;
				return false;
			}
			bool bAlive = true;
			for (const TObjectPtr<UStaticMesh>& Part : Found->Parts)
			{
				bAlive = bAlive && IsValid(Part.Get());
			}
			if (bAlive)
			{
				for (const TObjectPtr<UStaticMesh>& Part : Found->Parts)
				{
					OutParts.Add(Part.Get());
				}
				return true;
			}
		}
		Store.Forget(*Found);
		Store.Splits.Remove(Key);
	}

	FSplit Made;
	Made.Source = Source;
	Made.Signature = Signature;
	auto Fail = [&](const FString& Why)
	{
		Made.Why = Why;
		OutWhy = Why;
		UE_LOG(LogMantle, Warning, TEXT("MANTLE_SLOT_SPLIT '%s' has %d slots and could not be split: %s"), *Source->GetName(), DrawnSlots.Num(), *Why);
		Store.Splits.Add(Key, MoveTemp(Made));
		return false;
	};

	const FStaticMeshRenderData* RenderData = Source->GetRenderData();
	if (!RenderData || !RenderData->LODResources.IsValidIndex(0))
	{
		return Fail(FString::Printf(TEXT("the mesh '%s' has no render data"), *Source->GetName()));
	}

	TArray<FMeshDescription> Descriptions;

	// A Nanite source's render data is only its small fallback: its parts are cut from its own LOD 0 at full detail instead (the one LOD Nanite
	// draws), as Nanite meshes of the source's settings. Without that description (it was not kept) they are cut from the fallback.
	const bool bNaniteSource = Source->IsNaniteEnabled() || Source->GetNaniteSettings().bEnabled;
	bool bFullDetailNanite = false;
	if (bNaniteSource)
	{
		FMeshDescription Full;
		if (Source->IsMeshDescriptionValid(0) && Source->CloneMeshDescription(0, Full) && Full.Triangles().Num() > 0 && NameGroupsBySlot(*Source, Full))
		{
			Descriptions.Add(MoveTemp(Full));
			bFullDetailNanite = true;
		}
		else
		{
			UE_LOG(LogMantle, Warning, TEXT("MANTLE_SLOT_SPLIT '%s' cut from its Nanite fallback: its full-detail source is not kept, so its parts publish without Nanite detail"), *Source->GetName());
		}
	}

	// Every LOD with vertices an export can read, as the converter reads them (the first that can't is where the parts stop).
	const int32 MaxLODs = bFullDetailNanite ? 0 : FMath::Min(RenderData->LODResources.Num(), static_cast<int32>(MAX_STATIC_MESH_LODS));
	for (int32 LODIndex = 0; LODIndex < MaxLODs; ++LODIndex)
	{
		FMeshDescription Description;
		const FMantleMeshReduce::EReadResult Read = FMantleMeshReduce::ReadRenderLOD(*Source, LODIndex, SlotPrefix, Description);
		if (Read != FMantleMeshReduce::EReadResult::Read)
		{
			if (LODIndex == 0)
			{
				return Fail(Read == FMantleMeshReduce::EReadResult::Unreadable
					? FString::Printf(TEXT("the mesh '%s' has no readable geometry (turn on Allow CPU Access in its details)"), *Source->GetName())
					: FString::Printf(TEXT("the mesh '%s' has no triangles"), *Source->GetName()));
			}
			break;
		}
		Descriptions.Add(MoveTemp(Description));
	}

	const bool bSimple = HasSimpleShapes(*Source);
	const bool bHasBody = Source->GetBodySetup() != nullptr;
	const int32 PartCount = FMath::DivideAndRoundUp(DrawnSlots.Num(), MaxSections);
	TArray<TPair<UStaticMesh*, FPartInfo>> Built;
	for (int32 PartIndex = 0; PartIndex < PartCount; ++PartIndex)
	{
		// The slots in the order the upload lists them, MaxSections at a time.
		TArray<int32> PartSlots;
		for (int32 At = PartIndex * MaxSections; At < DrawnSlots.Num() && PartSlots.Num() < MaxSections; ++At)
		{
			PartSlots.Add(DrawnSlots[At]);
		}
		FString Why;
		UStaticMesh* Part = BuildPart(*Source, Signature, Descriptions, PartIndex, PartSlots, bFullDetailNanite, Why);
		if (!Part)
		{
			return Fail(Why);
		}
		FPartInfo Info;
		Info.Whole = Source;
		Info.SourceSlots = PartSlots;
		Info.bCarriesCollision = bHasBody && (PartIndex == 0 || !bSimple);
		Built.Emplace(Part, MoveTemp(Info));
		Made.Parts.Add(Part);
	}

	for (TPair<UStaticMesh*, FPartInfo>& Entry : Built)
	{
		Store.Infos.Add(Entry.Key, MoveTemp(Entry.Value));
		OutParts.Add(Entry.Key);
	}
	UE_LOG(LogMantle, Log, TEXT("MANTLE_SLOT_SPLIT '%s' %d slots into %d meshes%s"), *Source->GetName(), DrawnSlots.Num(), PartCount,
		bFullDetailNanite ? TEXT(" (Nanite, cut from its full-detail source)") : TEXT(""));
	Store.Splits.Add(Key, MoveTemp(Made));
	return true;
}

bool FMantleSlotSplit::PartsFor(const UStaticMeshComponent* Component, const UStaticMesh* Mesh, TArray<UStaticMesh*>& OutParts)
{
	OutParts.Reset();
	// A physics body is one piece: its mesh is not cut.
	if (!Mesh || (Component && Component->BodyInstance.bSimulatePhysics))
	{
		return false;
	}
	FString Why;
	if (!Split(Mesh, DefaultMaxSections(), OutParts, Why))
	{
		return false;
	}
	if (Component && !FMantlePlaceExporter::IsShownInGame(Component))
	{
		// Never drawn: only what collides is published.
		OutParts.RemoveAll([](const UStaticMesh* Part) { return !CarriesCollision(Part); });
	}
	return true;
}

bool FMantleSlotSplit::IsPart(const UStaticMesh* Mesh)
{
	return Mesh && MantleSlotSplitPrivate::Cache().Infos.Contains(Mesh);
}

const UStaticMesh* FMantleSlotSplit::WholeOf(const UStaticMesh* Mesh)
{
	if (const MantleSlotSplitPrivate::FPartInfo* Info = Mesh ? MantleSlotSplitPrivate::Cache().Infos.Find(Mesh) : nullptr)
	{
		if (const UStaticMesh* Whole = Info->Whole.Get())
		{
			return Whole;
		}
	}
	return Mesh;
}

int32 FMantleSlotSplit::SourceSlot(const UStaticMesh* Mesh, int32 Slot)
{
	if (const MantleSlotSplitPrivate::FPartInfo* Info = Mesh ? MantleSlotSplitPrivate::Cache().Infos.Find(Mesh) : nullptr)
	{
		return Info->SourceSlots.IsValidIndex(Slot) ? Info->SourceSlots[Slot] : Slot;
	}
	return Slot;
}

bool FMantleSlotSplit::CarriesCollision(const UStaticMesh* Mesh)
{
	const MantleSlotSplitPrivate::FPartInfo* Info = Mesh ? MantleSlotSplitPrivate::Cache().Infos.Find(Mesh) : nullptr;
	return !Info || Info->bCarriesCollision;
}

void FMantleSlotSplit::ClearCache()
{
	MantleSlotSplitPrivate::FSplitCache& Store = MantleSlotSplitPrivate::Cache();
	Store.Splits.Empty();
	Store.Infos.Empty();
}
