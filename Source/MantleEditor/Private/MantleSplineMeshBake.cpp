// Copyright Joshua Eger. All Rights Reserved.
#include "MantleSplineMeshBake.h"

#include "MantleEnvironmentExport.h"
#include "MantleMeshReduce.h"
#include "MantlePlaceAssets.h"
#include "MantlePlaceExporter.h"
#include "MantleRuntime.h"

#include "Components/SplineMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "Misc/SecureHash.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshOperations.h"
#include "StaticMeshResources.h"
#include "UObject/GCObject.h"
#include "UObject/Package.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleSplineMeshBakePrivate
{
	/**
	 * A baked mesh stays well under the place mesh limits (limits.json placeAssets.staticMesh): a road network
	 * in a few meshes uploads and loads faster than one at the cap, and a mesh this size still builds in
	 * seconds. The limits themselves are applied on top (the smaller wins).
	 */
	constexpr int32 SoftMaxTriangles = 250000;
	constexpr int32 SoftMaxVertices = 400000;

	/** Bumped when what a bake writes changes, so a cached bake from before is not reused. */
	const TCHAR* const BakeVersion = TEXT("splinebake-1");

	/** One spline mesh component, read for baking. */
	struct FSource
	{
		USplineMeshComponent* Component = nullptr;
		AActor* Owner = nullptr;

		/** The effective material of every slot the mesh's LOD 0 draws, once each, in slot order. */
		TArray<UMaterialInterface*> Materials;

		int32 Triangles = 0;
		int32 Vertices = 0;
		FBox WorldBounds = FBox(ForceInit);
		EMantleCollision Collision = EMantleCollision::Blocking;
		EMantleSurface Surface = EMantleSurface::Default;
		bool bCastShadow = true;
	};

	/** Sources that share an owner, a material set, collision, shadow and surface: baked together. */
	struct FGroup
	{
		AActor* Owner = nullptr;
		FString Key;

		/** The material set, by path, and the slot each material takes in the baked meshes. */
		TArray<UMaterialInterface*> Materials;

		EMantleCollision Collision = EMantleCollision::Blocking;
		EMantleSurface Surface = EMantleSurface::Default;
		bool bCastShadow = true;
		TArray<const FSource*> Sources;
	};

	struct FCachedChunk
	{
		TObjectPtr<UStaticMesh> Mesh;
		FVector Pivot = FVector::ZeroVector;
		FString Name;
		FString IdSeed;
		int32 Components = 0;
		int32 Triangles = 0;
		int32 Vertices = 0;
	};

	struct FCacheEntry
	{
		TWeakObjectPtr<UWorld> World;
		bool bHadWorld = false;
		TArray<FCachedChunk> Chunks;
	};

	/** Holds every baked mesh (they are transient, and nothing else references them) until its sources change. */
	class FBakeCache : public FGCObject
	{
	public:
		TMap<FString, FCacheEntry> Entries;

		virtual void AddReferencedObjects(FReferenceCollector& Collector) override
		{
			for (TPair<FString, FCacheEntry>& Entry : Entries)
			{
				for (FCachedChunk& Chunk : Entry.Value.Chunks)
				{
					Collector.AddReferencedObject(Chunk.Mesh);
				}
			}
		}

		virtual FString GetReferencerName() const override
		{
			return TEXT("FMantleSplineMeshBakeCache");
		}
	};

	FBakeCache& Cache()
	{
		// Never destroyed: a static destructor would run after the object system is gone.
		static FBakeCache* Instance = new FBakeCache();
		return *Instance;
	}

	FString Md5Hex(const FString& Text)
	{
		return FMD5::HashAnsiString(*Text);
	}

	FName SlotName(int32 Index)
	{
		return FName(*FString::Printf(TEXT("MantleSplineMaterial_%d"), Index));
	}

	const FStaticMeshLODResources* FirstLOD(const UStaticMesh* Mesh)
	{
		const FStaticMeshRenderData* RenderData = Mesh ? Mesh->GetRenderData() : nullptr;
		return RenderData && RenderData->LODResources.IsValidIndex(0) ? &RenderData->LODResources[0] : nullptr;
	}

	FString Number(double Value)
	{
		return FString::SanitizeFloat(Value, 3);
	}

	/** Everything about a source that changes what its bake looks like. */
	FString SourceSignature(const FSource& Source)
	{
		const USplineMeshComponent* Component = Source.Component;
		const UStaticMesh* Mesh = Component->GetStaticMesh();
		const FSplineMeshParams& Params = Component->SplineParams;
		const FMatrix Matrix = Component->GetComponentTransform().ToMatrixWithScale();
		FString Text = FString::Printf(TEXT("%s|%s|%s|"), *Component->GetPathName(), *Mesh->GetPathName(), *Mesh->GetLightingGuid().ToString());
		for (int32 Row = 0; Row < 4; ++Row)
		{
			for (int32 Column = 0; Column < 4; ++Column)
			{
				Text += Number(Matrix.M[Row][Column]) + TEXT(",");
			}
		}
		Text += FString::Printf(TEXT("|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%d|%s|%s|%s|%d"),
			*Params.StartPos.ToString(), *Params.StartTangent.ToString(), *Params.StartScale.ToString(), *Number(Params.StartRoll),
			*Params.StartOffset.ToString(), *Params.EndPos.ToString(), *Params.EndTangent.ToString(), *Params.EndScale.ToString(),
			*Number(Params.EndRoll), *Params.EndOffset.ToString(),
			static_cast<int32>(Component->GetForwardAxis()), *Component->GetSplineUpDir().ToString(),
			*Number(Component->GetBoundaryMin()), *Number(Component->GetBoundaryMax()), Component->bSmoothInterpRollScale ? 1 : 0);
		for (const UMaterialInterface* Material : Source.Materials)
		{
			Text += TEXT("|") + Material->GetPathName();
		}
		return Text;
	}

	/** A Morton code of Point inside Box, 10 bits an axis: nearby components land in the same chunk. */
	uint32 Morton(const FVector& Point, const FBox& Box)
	{
		auto Spread = [](uint32 Value)
		{
			Value &= 0x3ff;
			Value = (Value | (Value << 16)) & 0x30000ff;
			Value = (Value | (Value << 8)) & 0x300f00f;
			Value = (Value | (Value << 4)) & 0x30c30c3;
			Value = (Value | (Value << 2)) & 0x9249249;
			return Value;
		};
		const FVector Size = Box.GetSize();
		auto Quantize = [](double Value, double Min, double Extent) -> uint32
		{
			return Extent <= UE_SMALL_NUMBER ? 0u : static_cast<uint32>(FMath::Clamp((Value - Min) / Extent, 0.0, 1.0) * 1023.0);
		};
		return Spread(Quantize(Point.X, Box.Min.X, Size.X)) | (Spread(Quantize(Point.Y, Box.Min.Y, Size.Y)) << 1)
			| (Spread(Quantize(Point.Z, Box.Min.Z, Size.Z)) << 2);
	}

	EMantleCollision CollisionOf(const UPrimitiveComponent* Component)
	{
		switch (Component->GetCollisionEnabled())
		{
		case ECollisionEnabled::NoCollision: return EMantleCollision::None;
		case ECollisionEnabled::QueryOnly:   return EMantleCollision::Overlap;
		default:                             return EMantleCollision::Blocking;
		}
	}

	/**
	 * Merges the chunk's sources into one static mesh, in world space relative to Pivot. Each source is
	 * read from its mesh's render data (LOD 0), deformed with the component's own spline
	 * (USplineMeshComponent::PropagateSplineDeformationToMesh, the call Merge Actors makes), and moved
	 * to the world by its component transform. Its polygon groups are named by material so that equal
	 * materials share a slot across sources.
	 */
	bool BakeChunk(const FGroup& Group, const TArray<const FSource*>& Chunk, const FVector& Pivot, const FString& MeshName,
		const FMantlePlaceAssetLimits& Limits, FCachedChunk& Out, FString& OutWhy)
	{
		FMeshDescription Combined;
		FStaticMeshAttributes(Combined).Register();
		FStaticMeshOperations::FAppendSettings Append;
		for (int32 Channel = 0; Channel < FStaticMeshOperations::FAppendSettings::MAX_NUM_UV_CHANNELS; ++Channel)
		{
			Append.bMergeUVChannels[Channel] = true;
		}

		for (const FSource* Source : Chunk)
		{
			const USplineMeshComponent* Component = Source->Component;
			const UStaticMesh* Mesh = Component->GetStaticMesh();
			const FStaticMeshLODResources* LOD = FirstLOD(Mesh);
			if (!LOD || !FMantleMeshReduce::HasCpuCopy(*LOD))
			{
				OutWhy = FString::Printf(TEXT("the mesh '%s' has no readable geometry"), Mesh ? *Mesh->GetName() : TEXT(""));
				return false;
			}

			FMeshDescription Part;
			Mesh->ExportStaticMeshLOD(*LOD, Part);
			if (Part.Triangles().Num() == 0)
			{
				OutWhy = FString::Printf(TEXT("the mesh '%s' has no triangles"), *Mesh->GetName());
				return false;
			}

			// ExportStaticMeshLOD makes one polygon group per section with triangles, in section order, named by the
			// slot. Rename them by effective material, so two slots (or two meshes) with one material share a slot.
			TArray<int32> Sections;
			for (int32 Section = 0; Section < LOD->Sections.Num(); ++Section)
			{
				if (LOD->Sections[Section].NumTriangles > 0)
				{
					Sections.Add(Section);
				}
			}
			FStaticMeshAttributes PartAttributes(Part);
			TPolygonGroupAttributesRef<FName> PartSlotNames = PartAttributes.GetPolygonGroupMaterialSlotNames();
			int32 Index = 0;
			for (const FPolygonGroupID PolygonGroup : Part.PolygonGroups().GetElementIDs())
			{
				const int32 Slot = Sections.IsValidIndex(Index) ? LOD->Sections[Sections[Index]].MaterialIndex : 0;
				++Index;
				const UMaterialInterface* Material = FMantlePlaceAssets::EffectiveMaterial(Component, Slot);
				PartSlotNames[PolygonGroup] = SlotName(FMath::Max(0, Group.Materials.IndexOfByKey(Material)));
			}

			Component->PropagateSplineDeformationToMesh(Part);
			FTransform ToPivot = Component->GetComponentTransform();
			ToPivot.AddToTranslation(-Pivot);
			// Not the "correct" normal transform: the renderer's own (as a placed mesh draws), which is what the component shows.
			FStaticMeshOperations::ApplyTransform(Part, ToPivot);
			FStaticMeshOperations::AppendMeshDescription(Part, Combined, Append);
		}
		if (Combined.Triangles().Num() == 0)
		{
			OutWhy = TEXT("there was nothing to merge");
			return false;
		}

		// A static mesh built from a merged description: slots in the order its polygon groups are, named as the groups are
		// (the builder matches by name). False, with OutError, when the engine cannot build it.
		auto BuildMerged = [&Group, &MeshName](const FMeshDescription& Merged, UStaticMesh*& OutBaked, const FStaticMeshLODResources*& OutBuiltLOD, FString& OutError) -> bool
		{
			UObject* Outer = GetTransientPackage();
			UStaticMesh* Baked = NewObject<UStaticMesh>(Outer, FMantlePlaceAssets::UniqueBakedMeshName(Outer, MeshName), RF_Transient);

			TMap<FName, UMaterialInterface*> MaterialOfSlot;
			for (int32 Index = 0; Index < Group.Materials.Num(); ++Index)
			{
				MaterialOfSlot.Add(SlotName(Index), Group.Materials[Index]);
			}
			FStaticMeshConstAttributes MergedAttributes(Merged);
			TPolygonGroupAttributesConstRef<FName> MergedSlotNames = MergedAttributes.GetPolygonGroupMaterialSlotNames();
			for (const FPolygonGroupID PolygonGroup : Merged.PolygonGroups().GetElementIDs())
			{
				const FName Name = MergedSlotNames[PolygonGroup];
				UMaterialInterface* const* Material = MaterialOfSlot.Find(Name);
				Baked->GetStaticMaterials().Add(FStaticMaterial(Material ? *Material : nullptr, Name, Name));
			}

			Baked->bAllowCPUAccess = true;
			Baked->SetNumSourceModels(1);
			// The geometry is final: nothing recomputed, no lightmap UVs, no distance field or Lumen cards for a mesh that spans a landscape.
			FMeshBuildSettings& Settings = Baked->GetSourceModel(0).BuildSettings;
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
			Descriptions.Add(&Merged);
			if (!Baked->BuildFromMeshDescriptions(Descriptions, Params))
			{
				OutError = TEXT("the engine could not build a static mesh from the merged geometry");
				return false;
			}
			const FStaticMeshLODResources* BuiltLOD = FirstLOD(Baked);
			if (!BuiltLOD || BuiltLOD->GetNumTriangles() <= 0)
			{
				OutError = TEXT("the merged static mesh has no triangles");
				return false;
			}

			// A road is walked on where its visible triangles are: complex collision as simple, which the place
			// carries as no simple shapes (the client then collides with the triangles).
			Baked->CreateBodySetup();
			if (UBodySetup* Body = Baked->GetBodySetup())
			{
				Body->CollisionTraceFlag = CTF_UseComplexAsSimple;
				Body->InvalidatePhysicsData();
			}
			OutBaked = Baked;
			OutBuiltLOD = BuiltLOD;
			return true;
		};

		UStaticMesh* Baked = nullptr;
		const FStaticMeshLODResources* BuiltLOD = nullptr;
		if (!BuildMerged(Combined, Baked, BuiltLOD, OutWhy))
		{
			return false;
		}

		Out.Mesh = Baked;
		Out.Triangles = BuiltLOD->GetNumTriangles();
		Out.Vertices = BuiltLOD->GetNumVertices();
		if (Out.Triangles > Limits.MaxTriangles || Out.Vertices > Limits.MaxVertices)
		{
			// Over the limit, the merged geometry is reduced (the editor's mesh reduction) rather than refused. The
			// reduction aims below the limit, and a try that comes out over it is followed by another aiming lower.
			const int32 MergedTriangles = Out.Triangles;
			const int32 MergedVertices = Out.Vertices;
			UStaticMesh* ReducedMesh = nullptr;
			const FStaticMeshLODResources* ReducedLOD = nullptr;
			FString ReduceWhy;
			auto Measure = [&BuildMerged, &ReducedMesh, &ReducedLOD, &ReduceWhy](const FMeshDescription& Reduced, int32& OutTriangles, int32& OutVertices) -> bool
			{
				if (!BuildMerged(Reduced, ReducedMesh, ReducedLOD, ReduceWhy))
				{
					return false;
				}
				OutTriangles = ReducedLOD->GetNumTriangles();
				OutVertices = ReducedLOD->GetNumVertices();
				return true;
			};
			if (FMantleMeshReduce::ReduceUntilFits(Combined, Limits, Measure, ReduceWhy))
			{
				Out.Mesh = ReducedMesh;
				Out.Triangles = ReducedLOD->GetNumTriangles();
				Out.Vertices = ReducedLOD->GetNumVertices();
				UE_LOG(LogMantle, Log, TEXT("MANTLE_804 SplineBake: '%s' merged to %d triangles and %d vertices, over the limit, and was reduced to %d and %d"),
					*MeshName, MergedTriangles, MergedVertices, Out.Triangles, Out.Vertices);
				return true;
			}
			OutWhy = FString::Printf(TEXT("the merged mesh has %d triangles and %d vertices; a place mesh may have at most %d and %d (and it could not be reduced: %s)"),
				MergedTriangles, MergedVertices, Limits.MaxTriangles, Limits.MaxVertices, *ReduceWhy);
			return false;
		}
		return true;
	}
}

FString FMantleSplineBakeReport::ToString() const
{
	FString Text = FString::Printf(TEXT("%d spline mesh%s on %s baked into %d mesh%s"),
		Components, Components == 1 ? TEXT("") : TEXT("es"), *OwnerLabel, Meshes, Meshes == 1 ? TEXT("") : TEXT("es"));
	if (Skipped > 0)
	{
		Text += FString::Printf(TEXT(" (%d more left out: nothing a player could see or touch)"), Skipped);
	}
	return Text;
}

bool FMantleSplineMeshBake::IsBakeSource(const USplineMeshComponent* Component)
{
	if (!IsValid(Component) || Component->HasAnyFlags(RF_Transient) || Component->IsEditorOnly())
	{
		return false;
	}
	// Not a landscape spline editor's own visual (its control point and segment guides), and not mesh-less.
	if (!Component->IsRelevantForSplinePartitioning())
	{
		return false;
	}
	const AActor* Owner = Component->GetOwner();
	if (!IsValid(Owner) || Owner->HasAnyFlags(RF_Transient) || FMantlePlaceExporter::IsProvidedByMantle(Owner))
	{
		return false;
	}
	// Something a player would see or touch, as an instanced mesh is held to.
	const bool bVisible = Component->IsVisible() && !Component->bHiddenInGame;
	return bVisible || Component->GetCollisionEnabled() != ECollisionEnabled::NoCollision;
}

TArray<USplineMeshComponent*> FMantleSplineMeshBake::CollectSources(UWorld* World, TMap<FString, int32>* OutNotPublished)
{
	TArray<USplineMeshComponent*> Sources;
	if (!World)
	{
		return Sources;
	}
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		const AActor* Actor = *It;
		if (!IsValid(Actor))
		{
			continue;
		}
		TArray<USplineMeshComponent*> Components;
		Actor->GetComponents(Components);
		Components.Sort([](const USplineMeshComponent& A, const USplineMeshComponent& B)
		{
			return A.GetName().Compare(B.GetName(), ESearchCase::CaseSensitive) < 0;
		});
		for (USplineMeshComponent* Component : Components)
		{
			if (IsBakeSource(Component))
			{
				Sources.Add(Component);
			}
			else if (OutNotPublished && IsValid(Component) && !Component->HasAnyFlags(RF_Transient) && !Component->IsEditorOnly()
				&& Component->IsRelevantForSplinePartitioning() && !FMantlePlaceExporter::IsProvidedByMantle(Actor) && !Actor->HasAnyFlags(RF_Transient))
			{
				++OutNotPublished->FindOrAdd(Actor->GetActorLabel());
			}
		}
	}
	return Sources;
}

FMantleSplineBakeResult FMantleSplineMeshBake::Bake(UWorld* World, const FMantlePlaceAssetLimits& Limits)
{
	TMap<FString, int32> NotPublished;
	const TArray<USplineMeshComponent*> Sources = CollectSources(World, &NotPublished);
	FMantleSplineBakeResult Result = BakeComponents(Sources, Limits, World);
	// Spline meshes nothing can see or touch publish nothing; the report says how many, per owner.
	for (const TPair<FString, int32>& Pair : NotPublished)
	{
		FMantleSplineBakeReport* Report = Result.Reports.FindByPredicate([&Pair](const FMantleSplineBakeReport& Existing) { return Existing.OwnerLabel == Pair.Key; });
		if (!Report)
		{
			Report = &Result.Reports.AddDefaulted_GetRef();
			Report->OwnerLabel = Pair.Key;
		}
		Report->Skipped += Pair.Value;
	}
	return Result;
}

FMantleSplineBakeResult FMantleSplineMeshBake::BakeComponents(TConstArrayView<USplineMeshComponent*> Components, const FMantlePlaceAssetLimits& Limits, UWorld* PruneWorld)
{
	using namespace MantleSplineMeshBakePrivate;
	FMantleSplineBakeResult Result;

	const int32 TriangleCap = FMath::Min(Limits.MaxTriangles, SoftMaxTriangles);
	const int32 VertexCap = FMath::Min(Limits.MaxVertices, SoftMaxVertices);

	TArray<FMantleSplineBakeReport>& Reports = Result.Reports;
	auto ReportFor = [&Reports](const AActor* Owner) -> FMantleSplineBakeReport&
	{
		const FString Label = Owner->GetActorLabel();
		FMantleSplineBakeReport* Found = Reports.FindByPredicate([&Label](const FMantleSplineBakeReport& Report) { return Report.OwnerLabel == Label; });
		if (!Found)
		{
			Found = &Reports.AddDefaulted_GetRef();
			Found->OwnerLabel = Label;
		}
		return *Found;
	};
	TSet<FString> ProblemKeys;
	auto AddProblem = [&Result, &ProblemKeys](const AActor* Owner, const FString& What, const FString& Remedy)
	{
		bool bAlreadyAdded = true;
		ProblemKeys.Add(Owner->GetPathName() + TEXT("|") + What, &bAlreadyAdded);
		if (!bAlreadyAdded)
		{
			Result.Problems.Add(FMantleSplineBakeProblem{ Owner->GetActorLabel(), Owner->GetName(), What, Remedy });
		}
	};

	// Read every component, and group them. Sources are stable in memory (reserved up front) for the groups' pointers.
	TArray<FSource> Sources;
	Sources.Reserve(Components.Num());
	TArray<FGroup> Groups;
	TMap<FString, int32> GroupByKey;
	for (USplineMeshComponent* Component : Components)
	{
		if (!IsBakeSource(Component))
		{
			continue;
		}
		AActor* Owner = Component->GetOwner();
		const UStaticMesh* Mesh = Component->GetStaticMesh();
		const FStaticMeshLODResources* LOD = FirstLOD(Mesh);
		if (!LOD)
		{
			AddProblem(Owner, FString::Printf(TEXT("has spline meshes of '%s', which has no render data, so they can't be baked."), *Mesh->GetName()),
				TEXT("Open the mesh in the Static Mesh Editor and save it to rebuild it, or use another mesh."));
			continue;
		}
		if (!FMantleMeshReduce::HasCpuCopy(*LOD))
		{
			AddProblem(Owner, FString::Printf(TEXT("has spline meshes of '%s', whose geometry has no CPU copy to bake from."), *Mesh->GetName()),
				TEXT("Turn on Allow CPU Access in the mesh's details and try again."));
			continue;
		}

		FSource Source;
		Source.Component = Component;
		Source.Owner = Owner;
		TSet<const UMaterialInterface*> Seen;
		for (const FStaticMeshSection& Section : LOD->Sections)
		{
			if (Section.NumTriangles == 0)
			{
				continue;
			}
			Source.Triangles += static_cast<int32>(Section.NumTriangles);
			UMaterialInterface* Material = FMantlePlaceAssets::EffectiveMaterial(Component, Section.MaterialIndex);
			if (Material && !Seen.Contains(Material))
			{
				Seen.Add(Material);
				Source.Materials.Add(Material);
			}
		}
		if (Source.Triangles == 0 || Source.Materials.Num() == 0)
		{
			++ReportFor(Owner).Skipped; // an empty mesh draws nothing
			continue;
		}
		Source.Vertices = static_cast<int32>(LOD->VertexBuffers.PositionVertexBuffer.GetNumVertices());
		if (Source.Triangles > TriangleCap || Source.Vertices > VertexCap)
		{
			AddProblem(Owner, FString::Printf(TEXT("has a spline mesh of '%s' with %d triangles and %d vertices; a place mesh may have at most %d and %d."),
				*Mesh->GetName(), Source.Triangles, Source.Vertices, TriangleCap, VertexCap),
				TEXT("Use a lower-detail mesh for the spline mesh."));
			continue;
		}
		Source.WorldBounds = Component->CalcBounds(Component->GetComponentTransform()).GetBox();
		Source.Collision = CollisionOf(Component);
		Source.Surface = FMantlePlaceExporter::ReadSurface(Component);
		Source.bCastShadow = FMantleEnvironmentExport::CastsShadowInPlace(Component);
		Source.Materials.Sort([](const UMaterialInterface& A, const UMaterialInterface& B) { return A.GetPathName() < B.GetPathName(); });

		FString Key = FString::Printf(TEXT("%s|%d|%d|%d"), *Owner->GetPathName(), static_cast<int32>(Source.Collision), static_cast<int32>(Source.Surface), Source.bCastShadow ? 1 : 0);
		for (const UMaterialInterface* Material : Source.Materials)
		{
			Key += TEXT("|") + Material->GetPathName();
		}
		int32 GroupIndex;
		if (const int32* Found = GroupByKey.Find(Key))
		{
			GroupIndex = *Found;
		}
		else
		{
			GroupIndex = Groups.Num();
			GroupByKey.Add(Key, GroupIndex);
			FGroup& Added = Groups.AddDefaulted_GetRef();
			Added.Owner = Owner;
			Added.Key = Key;
			Added.Materials = Source.Materials;
			Added.Collision = Source.Collision;
			Added.Surface = Source.Surface;
			Added.bCastShadow = Source.bCastShadow;
		}
		const int32 SourceIndex = Sources.Add(MoveTemp(Source));
		Groups[GroupIndex].Sources.Add(&Sources[SourceIndex]);
	}

	TSet<FString> Used;
	for (FGroup& Group : Groups)
	{
		if (Group.Materials.Num() > Limits.MaxSections)
		{
			AddProblem(Group.Owner, FString::Printf(TEXT("has %d spline meshes that use %d materials together; a place mesh may have at most %d material slots."),
				Group.Sources.Num(), Group.Materials.Num(), Limits.MaxSections),
				TEXT("Give those spline meshes fewer materials (merge them into one), or split them across more than one mesh."));
			continue;
		}

		// Cut into chunks along a Morton curve, so a chunk's pieces are near each other, within the caps.
		FBox GroupBounds(ForceInit);
		for (const FSource* Source : Group.Sources)
		{
			GroupBounds += Source->WorldBounds;
		}
		TArray<TPair<uint32, const FSource*>> Order;
		for (const FSource* Source : Group.Sources)
		{
			Order.Emplace(Morton(Source->WorldBounds.GetCenter(), GroupBounds), Source);
		}
		Order.StableSort([](const TPair<uint32, const FSource*>& A, const TPair<uint32, const FSource*>& B)
		{
			if (A.Key != B.Key)
			{
				return A.Key < B.Key;
			}
			return A.Value->Component->GetName().Compare(B.Value->Component->GetName(), ESearchCase::CaseSensitive) < 0;
		});
		TArray<TArray<const FSource*>> Chunks;
		int32 Triangles = 0;
		int32 Vertices = 0;
		for (const TPair<uint32, const FSource*>& Entry : Order)
		{
			const FSource* Source = Entry.Value;
			if (Chunks.Num() == 0 || Triangles + Source->Triangles > TriangleCap || Vertices + Source->Vertices > VertexCap)
			{
				Chunks.AddDefaulted();
				Triangles = 0;
				Vertices = 0;
			}
			Chunks.Last().Add(Source);
			Triangles += Source->Triangles;
			Vertices += Source->Vertices;
		}

		// Everything that changes the bake is in the signature, so a cached bake is reused only when it would come out the same.
		FString Signature = FString::Printf(TEXT("%s|%d|%d|%d"), BakeVersion, TriangleCap, VertexCap, Limits.MaxSections);
		for (const TPair<uint32, const FSource*>& Entry : Order)
		{
			Signature += TEXT("\n") + SourceSignature(*Entry.Value);
		}
		const FString CacheKey = Group.Key + TEXT("#") + Md5Hex(Signature);
		Used.Add(CacheKey);

		FBakeCache& Store = Cache();
		FCacheEntry* Entry = Store.Entries.Find(CacheKey);
		if (!Entry)
		{
			FCacheEntry Fresh;
			Fresh.bHadWorld = PruneWorld != nullptr;
			Fresh.World = PruneWorld;
			bool bBaked = true;
			const FString OwnerName = Group.Owner->GetName();
			for (int32 ChunkIndex = 0; ChunkIndex < Chunks.Num() && bBaked; ++ChunkIndex)
			{
				FBox ChunkBounds(ForceInit);
				for (const FSource* Source : Chunks[ChunkIndex])
				{
					ChunkBounds += Source->WorldBounds;
				}
				const FVector Centre = ChunkBounds.GetCenter();
				FCachedChunk Chunk;
				Chunk.Pivot = FVector(FMath::RoundToDouble(Centre.X), FMath::RoundToDouble(Centre.Y), FMath::RoundToDouble(Centre.Z));
				Chunk.Components = Chunks[ChunkIndex].Num();
				Chunk.Name = FString::Printf(TEXT("%s splines (%s) %d/%d"), *Group.Owner->GetActorLabel(), *Group.Materials[0]->GetName(), ChunkIndex + 1, Chunks.Num());
				Chunk.IdSeed = FString::Printf(TEXT("%s|%d"), *Group.Key, ChunkIndex);
				const FString MeshName = FString::Printf(TEXT("MantleSplines_%s_%s_%d"), *OwnerName.Left(40), *Md5Hex(Signature).Left(8), ChunkIndex);
				FString Why;
				if (!BakeChunk(Group, Chunks[ChunkIndex], Chunk.Pivot, MeshName, Limits, Chunk, Why))
				{
					AddProblem(Group.Owner, FString::Printf(TEXT("has spline meshes that couldn't be baked into a static mesh: %s."), *Why),
						TEXT("Check the spline meshes' source meshes, or replace them with a static mesh (Merge Actors can bake them in the editor)."));
					bBaked = false;
					break;
				}
				Fresh.Chunks.Add(MoveTemp(Chunk));
			}
			if (!bBaked)
			{
				continue; // not cached: a fixed source is tried again
			}
			Entry = &Store.Entries.Add(CacheKey, MoveTemp(Fresh));
		}

		FMantleSplineBakeReport& Report = ReportFor(Group.Owner);
		Report.Components += Group.Sources.Num();
		Report.Meshes += Entry->Chunks.Num();
		for (const FCachedChunk& Chunk : Entry->Chunks)
		{
			FMantleBakedSplineMesh& Baked = Result.Meshes.AddDefaulted_GetRef();
			Baked.Mesh = Chunk.Mesh;
			Baked.Name = Chunk.Name;
			Baked.IdSeed = Chunk.IdSeed;
			Baked.OwnerLabel = Group.Owner->GetActorLabel();
			Baked.OwnerName = Group.Owner->GetName();
			Baked.Transform = FTransform(Chunk.Pivot);
			Baked.Collision = Group.Collision;
			Baked.Surface = Group.Surface;
			Baked.bCastShadow = Group.bCastShadow;
			Baked.SourceComponents = Chunk.Components;
			Baked.Triangles = Chunk.Triangles;
			Baked.Vertices = Chunk.Vertices;
		}
	}

	// What this world baked before and no longer does (a component moved, was removed, or its mesh changed) goes.
	FBakeCache& Store = Cache();
	for (auto It = Store.Entries.CreateIterator(); It; ++It)
	{
		const FCacheEntry& Entry = It.Value();
		const bool bGone = Entry.bHadWorld && !Entry.World.IsValid();
		const bool bReplaced = PruneWorld && Entry.World.Get() == PruneWorld && !Used.Contains(It.Key());
		if (bGone || bReplaced)
		{
			It.RemoveCurrent();
		}
	}

	for (const FMantleSplineBakeReport& Report : Result.Reports)
	{
		UE_LOG(LogMantle, Log, TEXT("MANTLE_628 SplineBake: %s"), *Report.ToString());
	}
	return Result;
}

void FMantleSplineMeshBake::ClearCache()
{
	MantleSplineMeshBakePrivate::Cache().Entries.Empty();
}
