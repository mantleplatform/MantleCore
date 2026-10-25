// Copyright Joshua Eger. All Rights Reserved.
#include "MantleMeshReduce.h"

#include "MantlePlaceAssets.h"
#include "MantleRuntime.h"

#include "Engine/StaticMesh.h"
#include "IMeshReductionInterfaces.h"
#include "IMeshReductionManagerModule.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "MeshReductionSettings.h"
#include "Misc/ScopedSlowTask.h"
#include "Misc/SecureHash.h"
#include "Modules/ModuleManager.h"
#include "OverlappingCorners.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshOperations.h"
#include "StaticMeshResources.h"
#include "UObject/GCObject.h"
#include "UObject/Package.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleMeshReducePrivate
{
	/** Bumped when what a reduced copy holds changes, so a cached copy from before is not reused. */
	const TCHAR* const ReduceVersion = TEXT("meshreduce-1");

	/** The prefix a copy's polygon groups and material slots are named with, followed by the source slot's index. */
	const TCHAR* const SlotPrefix = TEXT("MantleReducedSlot_");

	/** The reducer tests set in place of the engine's, and whether tests hide the engine's. */
	IMeshReduction* ReducerForTests = nullptr;
	bool bUnavailableForTests = false;

	/** The editor's static mesh reducer, or null. */
	IMeshReduction* StaticReducer()
	{
		if (bUnavailableForTests)
		{
			return nullptr;
		}
		IMeshReduction* Reduction = ReducerForTests;
		if (!Reduction)
		{
			const IMeshReductionManagerModule* Module = FModuleManager::Get().LoadModulePtr<IMeshReductionManagerModule>(TEXT("MeshReductionInterface"));
			Reduction = Module ? Module->GetStaticMeshReductionInterface() : nullptr;
		}
		return Reduction && Reduction->IsSupported() ? Reduction : nullptr;
	}

	/** The share of Limit a reduction aims for. */
	int32 TargetOf(int32 Limit)
	{
		return FMath::Max(1, FMath::FloorToInt(static_cast<float>(Limit) * FMantleMeshReduce::TargetShare));
	}

	struct FCopy
	{
		TWeakObjectPtr<const UStaticMesh> Source;

		/** What the copy was made from (see SignatureOf); a different one means the source changed. */
		FString Signature;

		/** Null when the reduction failed: Why says so, and the attempt is not repeated until the source changes. */
		TObjectPtr<UStaticMesh> Mesh;
		FString Why;
	};

	/** Holds every reduced copy (they are transient, and nothing else references them) until its source changes. */
	class FReduceCache : public FGCObject
	{
	public:
		TMap<FString, FCopy> Copies;

		virtual void AddReferencedObjects(FReferenceCollector& Collector) override
		{
			for (TPair<FString, FCopy>& Entry : Copies)
			{
				Collector.AddReferencedObject(Entry.Value.Mesh);
			}
		}

		virtual FString GetReferencerName() const override
		{
			return TEXT("FMantleMeshReduceCache");
		}
	};

	FReduceCache& Cache()
	{
		// Never destroyed: a static destructor would run after the object system is gone.
		static FReduceCache* Instance = new FReduceCache();
		return *Instance;
	}

	/**
	 * Everything about the source a copy depends on: its geometry (the lighting guid changes whenever the mesh is
	 * rebuilt), its material slots (assigning a slot's material does not), and the reducer that made it.
	 */
	FString SignatureOf(const UStaticMesh& Source)
	{
		const IMeshReduction* Reducer = StaticReducer();
		FString Text = FString::Printf(TEXT("%s|%s|%s"), ReduceVersion, *Source.GetLightingGuid().ToString(), Reducer ? *Reducer->GetVersionString() : TEXT("none"));
		for (const FStaticMaterial& Slot : Source.GetStaticMaterials())
		{
			Text += FString::Printf(TEXT("|%s/%s/%s"), *Slot.MaterialSlotName.ToString(), *Slot.ImportedMaterialSlotName.ToString(),
				Slot.MaterialInterface ? *Slot.MaterialInterface->GetPathName() : TEXT(""));
		}
		return Text;
	}

	bool SameSlots(const TArray<int32>& A, const TArray<int32>& B)
	{
		if (A.Num() != B.Num())
		{
			return false;
		}
		for (const int32 Slot : A)
		{
			if (!B.Contains(Slot))
			{
				return false;
			}
		}
		return true;
	}

	/**
	 * The transient static mesh for Reduced: the source's material slots in the source's order (so every slot index means
	 * the same thing), named as Reduced's polygon groups are (the engine's build matches them by name), the source's
	 * lightmap setup and its simple collision. Null with OutWhy when the engine cannot build it.
	 */
	UStaticMesh* BuildCopy(const UStaticMesh& Source, const FMeshDescription& Reduced, const FString& Signature, FString& OutWhy)
	{
		UObject* Outer = GetTransientPackage();
		const FString MeshName = FString::Printf(TEXT("MantleReduced_%s_%s"), *Source.GetName().Left(60), *FMD5::HashAnsiString(*Signature).Left(8));
		UStaticMesh* Baked = NewObject<UStaticMesh>(Outer, MakeUniqueObjectName(Outer, UStaticMesh::StaticClass(), FName(*MeshName)), RF_Transient);

		for (const FStaticMaterial& Slot : Source.GetStaticMaterials())
		{
			FStaticMaterial& Copied = Baked->GetStaticMaterials().Add_GetRef(Slot);
			Copied.ImportedMaterialSlotName = FMantleMeshReduce::SlotName(SlotPrefix, Baked->GetStaticMaterials().Num() - 1);
		}

		Baked->bAllowCPUAccess = true;
		Baked->SetLightMapCoordinateIndex(Source.GetLightMapCoordinateIndex());
		Baked->SetLightMapResolution(Source.GetLightMapResolution());
		Baked->SetNumSourceModels(1);
		FMeshBuildSettings& Settings = Baked->GetSourceModel(0).BuildSettings;
		if (Source.GetNumSourceModels() > 0)
		{
			// The precision the source stores its tangents and UVs at: a reduced copy is as exact as the original.
			Settings.bUseHighPrecisionTangentBasis = Source.GetSourceModel(0).BuildSettings.bUseHighPrecisionTangentBasis;
			Settings.bUseFullPrecisionUVs = Source.GetSourceModel(0).BuildSettings.bUseFullPrecisionUVs;
		}
		// The geometry is final: nothing recomputed (the reducer's normals, tangents and lightmap UVs are kept as they are),
		// and no distance field or Lumen cards, which only an editor view of the mesh would use.
		Settings.bRecomputeNormals = false;
		Settings.bRecomputeTangents = false;
		Settings.bGenerateLightmapUVs = false;
		Settings.bBuildReversedIndexBuffer = false;
		Settings.DistanceFieldResolutionScale = 0.0f;
		Settings.MaxLumenMeshCards = 0;

		UStaticMesh::FBuildMeshDescriptionsParams Params;
		Params.bMarkPackageDirty = false;
		Params.bAllowCpuAccess = true;
		TArray<const FMeshDescription*> Descriptions;
		Descriptions.Add(&Reduced);
		if (!Baked->BuildFromMeshDescriptions(Descriptions, Params))
		{
			OutWhy = FString::Printf(TEXT("the engine could not build a static mesh from the reduced '%s'"), *Source.GetName());
			return nullptr;
		}
		const FStaticMeshRenderData* BakedData = Baked->GetRenderData();
		if (!BakedData || !BakedData->LODResources.IsValidIndex(0) || BakedData->LODResources[0].GetNumTriangles() <= 0)
		{
			OutWhy = FString::Printf(TEXT("the reduced '%s' has no triangles"), *Source.GetName());
			return nullptr;
		}

		// Its collision: the source's shapes as they are, or the triangles where the source has no body setup.
		Baked->CreateBodySetup();
		if (UBodySetup* Body = Baked->GetBodySetup())
		{
			if (const UBodySetup* SourceBody = Source.GetBodySetup())
			{
				Body->CollisionTraceFlag = SourceBody->CollisionTraceFlag;
				Body->AggGeom = SourceBody->AggGeom;
			}
			else
			{
				Body->CollisionTraceFlag = CTF_UseComplexAsSimple;
			}
			Body->InvalidatePhysicsData();
		}
		return Baked;
	}
}

bool FMantleMeshReduce::IsAvailable()
{
	return MantleMeshReducePrivate::StaticReducer() != nullptr;
}

FString FMantleMeshReduce::CacheTag(const FMantlePlaceAssetLimits& Limits)
{
	using namespace MantleMeshReducePrivate;
	const IMeshReduction* Reducer = StaticReducer();
	if (!Reducer)
	{
		return FString();
	}
	return FString::Printf(TEXT("%d/%d/%s/%s"), TargetOf(Limits.MaxTriangles), TargetOf(Limits.MaxVertices), *Reducer->GetVersionString(), ReduceVersion);
}

bool FMantleMeshReduce::HasCpuCopy(const FStaticMeshLODResources& LOD)
{
	const FPositionVertexBuffer& Positions = LOD.VertexBuffers.PositionVertexBuffer;
	const FStaticMeshVertexBuffer& Vertices = LOD.VertexBuffers.StaticMeshVertexBuffer;
	return Positions.GetNumVertices() > 0 && Positions.GetVertexData() && Vertices.GetTangentData() && Vertices.GetTexCoordData()
		&& LOD.IndexBuffer.GetNumIndices() >= 3;
}

FName FMantleMeshReduce::SlotName(const TCHAR* Prefix, int32 Slot)
{
	return FName(*FString::Printf(TEXT("%s%d"), Prefix, Slot));
}

FMantleMeshReduce::EReadResult FMantleMeshReduce::ReadRenderLOD(const UStaticMesh& Source, int32 LODIndex, const TCHAR* SlotPrefix, FMeshDescription& Out)
{
	const FStaticMeshRenderData* RenderData = Source.GetRenderData();
	if (!RenderData || !RenderData->LODResources.IsValidIndex(LODIndex))
	{
		return EReadResult::Unreadable;
	}
	const FStaticMeshLODResources& LOD = RenderData->LODResources[LODIndex];
	if (!HasCpuCopy(LOD))
	{
		return EReadResult::Unreadable;
	}

	Source.ExportStaticMeshLOD(LOD, Out);
	if (Out.Triangles().Num() == 0)
	{
		return EReadResult::Empty;
	}

	// ExportStaticMeshLOD makes one polygon group per section with triangles, in section order, named by the slot's
	// imported name (which a mesh may repeat or leave empty). Name them by slot index, so each section keeps its slot.
	TArray<int32> Sections;
	for (int32 Section = 0; Section < LOD.Sections.Num(); ++Section)
	{
		if (LOD.Sections[Section].NumTriangles > 0)
		{
			Sections.Add(Section);
		}
	}
	FStaticMeshAttributes OutAttributes(Out);
	TPolygonGroupAttributesRef<FName> OutSlotNames = OutAttributes.GetPolygonGroupMaterialSlotNames();
	int32 Index = 0;
	for (const FPolygonGroupID PolygonGroup : Out.PolygonGroups().GetElementIDs())
	{
		const int32 Slot = Sections.IsValidIndex(Index) ? LOD.Sections[Sections[Index]].MaterialIndex : 0;
		++Index;
		OutSlotNames[PolygonGroup] = SlotName(SlotPrefix, Slot);
	}
	return EReadResult::Read;
}

bool FMantleMeshReduce::ReduceDescription(const FMeshDescription& In, int32 TargetTriangles, int32 TargetVertices, FMeshDescription& Out, FString& OutWhy)
{
	using namespace MantleMeshReducePrivate;
	IMeshReduction* Reducer = StaticReducer();
	if (!Reducer)
	{
		OutWhy = TEXT("the editor has no static mesh reducer");
		return false;
	}
	if (In.Triangles().Num() == 0)
	{
		OutWhy = TEXT("there are no triangles to reduce");
		return false;
	}

	// Any: the reducer keeps going until both the triangle and the vertex target are met.
	FMeshReductionSettings Settings;
	Settings.TerminationCriterion = EStaticMeshReductionTerimationCriterion::Any;
	Settings.PercentTriangles = 1.0f;
	Settings.MaxNumOfTriangles = static_cast<uint32>(FMath::Max(TargetTriangles, 2));
	Settings.PercentVertices = 1.0f;
	Settings.MaxNumOfVerts = static_cast<uint32>(FMath::Max(TargetVertices, 4));

	FOverlappingCorners OverlappingCorners;
	FStaticMeshOperations::FindOverlappingCorners(OverlappingCorners, In, UE_THRESH_POINTS_ARE_SAME);

	// The reducer writes its polygon groups' names into the static mesh attributes: they have to be there.
	FMeshDescription Reduced;
	FStaticMeshAttributes(Reduced).Register();
	float MaxDeviation = 0.0f;
	Reducer->ReduceMeshDescription(Reduced, MaxDeviation, In, OverlappingCorners, Settings);
	if (Reduced.Triangles().Num() == 0)
	{
		OutWhy = TEXT("the reducer left no triangles");
		return false;
	}
	Out = MoveTemp(Reduced);
	return true;
}

bool FMantleMeshReduce::ReduceUntilFits(const FMeshDescription& In, const FMantlePlaceAssetLimits& Limits,
	TFunctionRef<bool(const FMeshDescription& Reduced, int32& OutTriangles, int32& OutVertices)> Measure, FString& OutWhy)
{
	using namespace MantleMeshReducePrivate;
	OutWhy.Reset();
	// A mesh over on vertices alone is not made coarser than it has to be: its triangles are only capped.
	int32 TargetTriangles = FMath::Min(In.Triangles().Num(), TargetOf(Limits.MaxTriangles));
	int32 TargetVertices = TargetOf(Limits.MaxVertices);
	int32 Triangles = 0;
	int32 Vertices = 0;
	for (int32 Attempt = 0; Attempt < MaxAttempts; ++Attempt)
	{
		FMeshDescription Reduced;
		if (!ReduceDescription(In, TargetTriangles, TargetVertices, Reduced, OutWhy))
		{
			return false;
		}
		Triangles = 0;
		Vertices = 0;
		if (!Measure(Reduced, Triangles, Vertices))
		{
			if (OutWhy.IsEmpty())
			{
				OutWhy = TEXT("the reduced mesh could not be built");
			}
			return false;
		}
		if (Triangles <= Limits.MaxTriangles && Vertices <= Limits.MaxVertices)
		{
			return true;
		}

		// It stopped short of its target: aim lower by as much as it came out over, so the next try lands near the share again.
		const double Over = FMath::Max(static_cast<double>(Triangles) / FMath::Max(Limits.MaxTriangles, 1), static_cast<double>(Vertices) / FMath::Max(Limits.MaxVertices, 1));
		const double Next = Over > 1.0 ? TargetShare / Over : TargetShare;
		TargetTriangles = FMath::FloorToInt(static_cast<double>(TargetTriangles) * Next);
		TargetVertices = FMath::FloorToInt(static_cast<double>(TargetVertices) * Next);
		if (TargetTriangles < 2 || TargetVertices < 4)
		{
			break;
		}
	}
	OutWhy = FString::Printf(TEXT("the reducer could not bring it within %d triangles and %d vertices (its best was %d and %d)"),
		Limits.MaxTriangles, Limits.MaxVertices, Triangles, Vertices);
	return false;
}

UStaticMesh* FMantleMeshReduce::ReducedCopy(const UStaticMesh* Source, const FMantlePlaceAssetLimits& Limits, FString& OutWhy)
{
	using namespace MantleMeshReducePrivate;
	OutWhy.Reset();
	if (!IsValid(Source))
	{
		OutWhy = TEXT("there is no mesh to reduce");
		return nullptr;
	}
	FReduceCache& Store = Cache();

	// A copy whose source is gone is not needed again.
	for (auto It = Store.Copies.CreateIterator(); It; ++It)
	{
		if (!It.Value().Source.IsValid())
		{
			It.RemoveCurrent();
		}
	}

	const FString Key = FString::Printf(TEXT("%s|%d|%d"), *Source->GetPathName(), Limits.MaxTriangles, Limits.MaxVertices);
	const FString Signature = SignatureOf(*Source);
	if (const FCopy* Found = Store.Copies.Find(Key))
	{
		if (Found->Source.Get() == Source && Found->Signature.Equals(Signature, ESearchCase::CaseSensitive))
		{
			if (IsValid(Found->Mesh))
			{
				return Found->Mesh;
			}
			if (!Found->Why.IsEmpty())
			{
				OutWhy = Found->Why;
				return nullptr;
			}
		}
	}

	// Not cached: a reduction of a big mesh takes a while on the game thread, so the editor says what it is doing (a cache hit
	// returned above, with no task). Delayed, so a quick one does not flash a dialog.
	FScopedSlowTask ReducingTask(1.0f, FText::FromString(FString::Printf(TEXT("Reducing '%s' for publishing..."), *Source->GetName())));
	ReducingTask.MakeDialogDelayed(0.5f);

	UStaticMesh* Result = nullptr;
	FString Why;
	do
	{
		if (!IsAvailable())
		{
			Why = TEXT("the editor has no static mesh reducer");
			break;
		}
		FMeshDescription Original;
		const EReadResult Read = ReadRenderLOD(*Source, 0, SlotPrefix, Original);
		if (Read != EReadResult::Read)
		{
			Why = Read == EReadResult::Unreadable
				? FString::Printf(TEXT("the mesh '%s' has no readable geometry (turn on Allow CPU Access in its details)"), *Source->GetName())
				: FString::Printf(TEXT("the mesh '%s' has no triangles"), *Source->GetName());
			break;
		}

		// Reducing must not lose a material: every slot the source draws with is still drawn in the copy.
		const TArray<int32> SourceSlots = FMantlePlaceAssets::UploadSlots(Source);
		UStaticMesh* Built = nullptr;
		auto Measure = [&](const FMeshDescription& Reduced, int32& OutTriangles, int32& OutVertices) -> bool
		{
			Built = BuildCopy(*Source, Reduced, Signature, Why);
			FMantleMeshLODCount Count;
			if (!Built || !FMantlePlaceAssets::CountMeshLOD(Built, 0, Count))
			{
				return false;
			}
			if (!SameSlots(Count.Slots, SourceSlots))
			{
				Why = FString::Printf(TEXT("reducing '%s' would remove one of its materials"), *Source->GetName());
				return false;
			}
			OutTriangles = Count.Triangles;
			OutVertices = Count.Vertices;
			return true;
		};
		FMantleMeshLODCount Before;
		FMantlePlaceAssets::CountMeshLOD(Source, 0, Before);
		if (!ReduceUntilFits(Original, Limits, Measure, Why))
		{
			break;
		}
		Result = Built;
		UE_LOG(LogMantle, Log, TEXT("MANTLE_804 MeshReduce: '%s' reduced from %d triangles and %d vertices into '%s' (%d triangles)"), *Source->GetName(),
			Before.Triangles, Before.Vertices, *Result->GetName(), Result->GetRenderData()->LODResources[0].GetNumTriangles());
	}
	while (false);

	if (!Result)
	{
		UE_LOG(LogMantle, Log, TEXT("MANTLE_804 MeshReduce: '%s' was not reduced: %s"), *Source->GetName(), *Why);
	}
	FCopy& Stored = Store.Copies.FindOrAdd(Key);
	Stored.Source = Source;
	Stored.Signature = Signature;
	Stored.Mesh = Result;
	Stored.Why = Result ? FString() : Why;
	OutWhy = Why;
	return Result;
}

void FMantleMeshReduce::ClearCache()
{
	MantleMeshReducePrivate::Cache().Copies.Empty();
}

void FMantleMeshReduce::SetReducerForTests(IMeshReduction* Reducer)
{
	MantleMeshReducePrivate::ReducerForTests = Reducer;
}

void FMantleMeshReduce::SetUnavailableForTests(bool bUnavailable)
{
	MantleMeshReducePrivate::bUnavailableForTests = bUnavailable;
}
