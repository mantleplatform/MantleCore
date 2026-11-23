// Copyright Joshua Eger. All Rights Reserved.
#include "MantleBrushBake.h"

#include "MantlePlaceAssets.h"
#include "MantleRuntime.h"

#include "MantleBlockingVolumeExport.h"

#include "ActorEditorUtils.h"
#include "BSPOps.h"
#include "Components/BrushComponent.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/Brush.h"
#include "Engine/Level.h"
#include "Engine/Polys.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "Misc/ScopeExit.h"
#include "Misc/SecureHash.h"
#include "Model.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshResources.h"
#include "UObject/GCObject.h"
#include "UObject/Package.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleBrushBakePrivate
{
	/** The spline bake's soft caps: a baked mesh stays well under the place mesh limits (the limits themselves are applied on top). */
	constexpr int32 SoftMaxTriangles = 250000;
	constexpr int32 SoftMaxVertices = 400000;

	/** Bumped when what a bake writes changes, so a cached bake from before is not reused. */
	const TCHAR* const BakeVersion = TEXT("brushbake-2");

	/** The one name of the single material slot every baked BSP mesh has. */
	const TCHAR* const SlotName = TEXT("MantleBrushMaterial_0");

	const TCHAR* const OwnerLabelText = TEXT("BSP geometry");

	/** One solid, visible surface of the level's cut BSP, in world space. */
	struct FSurface
	{
		const FPoly* Poly = nullptr;
		UMaterialInterface* Material = nullptr;
		FVector Center = FVector::ZeroVector;
		int32 Triangles = 0;
	};

	/** The surfaces that share a material (or, for volumes, a collision and a sight rule): baked together. */
	struct FGroup
	{
		UMaterialInterface* Material = nullptr;
		/** What tells this group from another of the same material: part of its meshes' ids. */
		FString Tag;
		bool bVisible = true;
		bool bBlocksVisibility = true;
		EMantleCollision Collision = EMantleCollision::Blocking;
		FString NamePrefix = TEXT("BSP");
		TArray<FSurface> Surfaces;
	};

	struct FCachedChunk
	{
		TObjectPtr<UStaticMesh> Mesh;
		FVector Pivot = FVector::ZeroVector;
		FString Name;
		FString IdSeed;
		int32 Triangles = 0;
		int32 Vertices = 0;
		bool bVisible = true;
		bool bBlocksVisibility = true;
		EMantleCollision Collision = EMantleCollision::Blocking;
	};

	struct FCacheEntry
	{
		TWeakObjectPtr<UWorld> World;
		bool bHadWorld = false;
		/** "bsp" or "vol": a bake of one kind prunes only the entries of its own. */
		FString Kind;
		int32 Brushes = 0;
		TArray<FCachedChunk> Chunks;
	};

	/** Holds every baked mesh (they are transient, and nothing else references them) until its brushes change. */
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
			return TEXT("FMantleBrushBakeCache");
		}
	};

	FBakeCache& BrushCache()
	{
		// Never destroyed: a static destructor would run after the object system is gone.
		static FBakeCache* Instance = new FBakeCache();
		return *Instance;
	}

	FString BrushMd5Hex(const FString& Text)
	{
		return FMD5::HashAnsiString(*Text);
	}

	const FStaticMeshLODResources* BrushFirstLOD(const UStaticMesh* Mesh)
	{
		const FStaticMeshRenderData* RenderData = Mesh ? Mesh->GetRenderData() : nullptr;
		return RenderData && RenderData->LODResources.IsValidIndex(0) ? &RenderData->LODResources[0] : nullptr;
	}

	FString BrushNumber(double Value)
	{
		return FString::SanitizeFloat(Value, 3);
	}

	FString BrushVector(const FVector& Value)
	{
		return BrushNumber(Value.X) + TEXT(",") + BrushNumber(Value.Y) + TEXT(",") + BrushNumber(Value.Z);
	}

	/** Everything about a brush that changes the BSP it cuts: where it is, how it combines, and every surface's shape, material and mapping. */
	FString BrushSignature(const ABrush* Brush)
	{
		const FTransform& Transform = Brush->GetActorTransform();
		FString Text = FString::Printf(TEXT("%s|%d|%d|%s|%s|%s"), *Brush->GetPathName(), static_cast<int32>(Brush->BrushType), Brush->PolyFlags,
			*BrushVector(Transform.GetLocation()), *BrushVector(Transform.GetRotation().Rotator().Euler()), *BrushVector(Transform.GetScale3D()));
		if (const UModel* Model = Brush->Brush; Model && Model->Polys)
		{
			for (const FPoly& Poly : Model->Polys->Element)
			{
				Text += FString::Printf(TEXT("\n%u|%s|%s|%s|%s"), Poly.PolyFlags, Poly.Material ? *Poly.Material->GetPathName() : TEXT(""),
					*BrushVector(FVector(Poly.Base)), *BrushVector(FVector(Poly.TextureU)), *BrushVector(FVector(Poly.TextureV)));
				for (const FVector3f& Vertex : Poly.Vertices)
				{
					Text += TEXT("|") + BrushVector(FVector(Vertex));
				}
			}
		}
		return Text;
	}

	/** A Morton code of Point inside Box, 10 bits an axis: nearby surfaces land in the same chunk. */
	uint32 BrushMorton(const FVector& Point, const FBox& Box)
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

	/**
	 * Adds one surface to Description, in world space relative to Pivot, as the engine's own brush-to-mesh conversion (GetBrushMesh)
	 * does: a fan of triangles in the same order, every edge hard, texture coordinates from the surface's own mapping.
	 */
	void AddSurface(FMeshDescription& Description, const FSurface& Surface, const FVector& Pivot, FPolygonGroupID PolygonGroup)
	{
		FStaticMeshAttributes Attributes(Description);
		TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
		TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
		TEdgeAttributesRef<bool> EdgeHardnesses = Attributes.GetEdgeHardnesses();

		const FPoly& Poly = *Surface.Poly;
		const FVector3f Base = Poly.Base;
		const FVector3f TextureX = Poly.TextureU / UModel::GetGlobalBSPTexelScale();
		const FVector3f TextureY = Poly.TextureV / UModel::GetGlobalBSPTexelScale();
		const FVector3f PivotF(Pivot);

		auto AddTriangle = [&](const FVector3f& A, const FVector3f& B, const FVector3f& C)
		{
			TArray<FVertexInstanceID> Corners;
			for (const FVector3f& Corner : { A, B, C })
			{
				const FVertexID Vertex = Description.CreateVertex();
				Positions[Vertex] = Corner - PivotF;
				const FVertexInstanceID Instance = Description.CreateVertexInstance(Vertex);
				UVs.Set(Instance, 0, FVector2f((Corner - Base) | TextureX, (Corner - Base) | TextureY));
				Corners.Add(Instance);
			}
			TArray<FEdgeID> NewEdges;
			Description.CreatePolygon(PolygonGroup, Corners, &NewEdges);
			for (const FEdgeID Edge : NewEdges)
			{
				EdgeHardnesses[Edge] = true; // BSP is flat shaded
			}
		};
		for (int32 Index = 2; Index < Poly.Vertices.Num(); ++Index)
		{
			// The engine's order: the fan's first vertex last.
			AddTriangle(Poly.Vertices[Index], Poly.Vertices[Index - 1], Poly.Vertices[0]);
			// A two-sided surface (a sheet: the Plane brush, Geometry mode's sheets) draws both faces in game, and its triangles are its
			// collision, so the back face is a second, reversed triangle.
			if (Poly.PolyFlags & PF_TwoSided)
			{
				AddTriangle(Poly.Vertices[0], Poly.Vertices[Index - 1], Poly.Vertices[Index]);
			}
		}
	}

	/** Merges Chunk's surfaces into one static mesh, in world space relative to Pivot, with the group's one material. */
	bool BakeChunk(const FGroup& Group, TConstArrayView<const FSurface*> Chunk, const FVector& Pivot, const FString& MeshName, FCachedChunk& Out, FString& OutWhy)
	{
		FMeshDescription Combined;
		FStaticMeshAttributes Attributes(Combined);
		Attributes.Register();
		Attributes.GetVertexInstanceUVs().SetNumChannels(1);
		const FPolygonGroupID PolygonGroup = Combined.CreatePolygonGroup();
		Attributes.GetPolygonGroupMaterialSlotNames()[PolygonGroup] = FName(SlotName);
		for (const FSurface* Surface : Chunk)
		{
			AddSurface(Combined, *Surface, Pivot, PolygonGroup);
		}
		if (Combined.Triangles().Num() == 0)
		{
			OutWhy = TEXT("there was nothing to merge");
			return false;
		}

		UObject* Outer = GetTransientPackage();
		UStaticMesh* Baked = NewObject<UStaticMesh>(Outer, FMantlePlaceAssets::UniqueBakedMeshName(Outer, MeshName), RF_Transient);
		Baked->GetStaticMaterials().Add(FStaticMaterial(Group.Material, FName(SlotName), FName(SlotName)));
		Baked->bAllowCPUAccess = true;
		Baked->SetNumSourceModels(1);
		// Flat shaded, so the engine works the normals and tangents out from the hard edges and the mapping; no lightmap UVs.
		FMeshBuildSettings& Settings = Baked->GetSourceModel(0).BuildSettings;
		Settings.bRecomputeNormals = true;
		Settings.bRecomputeTangents = true;
		Settings.bGenerateLightmapUVs = false;
		Settings.bBuildReversedIndexBuffer = false;

		UStaticMesh::FBuildMeshDescriptionsParams Params;
		Params.bMarkPackageDirty = false;
		Params.bAllowCpuAccess = true;
		TArray<const FMeshDescription*> Descriptions;
		Descriptions.Add(&Combined);
		if (!Baked->BuildFromMeshDescriptions(Descriptions, Params))
		{
			OutWhy = TEXT("the engine could not build a static mesh from the merged geometry");
			return false;
		}
		const FStaticMeshLODResources* BuiltLOD = BrushFirstLOD(Baked);
		if (!BuiltLOD || BuiltLOD->GetNumTriangles() <= 0)
		{
			OutWhy = TEXT("the merged static mesh has no triangles");
			return false;
		}

		// The collision is the triangles, as a spline mesh's is.
		Baked->CreateBodySetup();
		if (UBodySetup* Body = Baked->GetBodySetup())
		{
			Body->CollisionTraceFlag = CTF_UseComplexAsSimple;
			Body->InvalidatePhysicsData();
		}
		Out.Mesh = Baked;
		Out.Triangles = BuiltLOD->GetNumTriangles();
		Out.Vertices = BuiltLOD->GetNumVertices();
		return true;
	}

	bool BakeGroups(const TArray<FGroup>& Groups, const FString& Key, int32 TriangleCap, int32 VertexCap, TArray<FCachedChunk>& OutChunks, FString& OutWhy);

	/**
	 * Cuts Brushes (one level's static brushes) into a temporary model, as the editor's Build does, and bakes its visible solid
	 * surfaces. The model is a transient object of its own: no level's model, brush or package is written.
	 */
	bool BakeLevel(const TArray<ABrush*>& Brushes, const FString& LevelKey, int32 TriangleCap, int32 VertexCap, TArray<FCachedChunk>& OutChunks, FString& OutWhy)
	{
		if (!GEditor)
		{
			OutWhy = TEXT("the editor is not running");
			return false;
		}

		// The engine's order: structural brushes first, then the detail (semi-solid) ones.
		TArray<ABrush*> Ordered;
		for (ABrush* Brush : Brushes)
		{
			if (!(Brush->PolyFlags & PF_Semisolid) || Brush->BrushType != Brush_Add || (Brush->PolyFlags & PF_Portal))
			{
				Ordered.Add(Brush);
			}
		}
		for (ABrush* Brush : Brushes)
		{
			if ((Brush->PolyFlags & PF_Semisolid) && Brush->BrushType == Brush_Add && !(Brush->PolyFlags & PF_Portal))
			{
				Ordered.Add(Brush);
			}
		}

		// bspBrushCSG records on each brush's own model the transform it was built at and its vertex count: what the editor reads to align
		// surfaces when the BSP is not rebuilt. The creator's values are saved here and put back, whatever happens.
		struct FBuiltState
		{
			UModel* Model = nullptr;
			FVector3f Location = FVector3f::ZeroVector;
			FRotator Rotation = FRotator::ZeroRotator;
			FVector3f Scale = FVector3f::OneVector;
			bool bValid = false;
			uint32 NumUniqueVertices = 0;
		};
		TArray<FBuiltState> Saved;
		for (const ABrush* Brush : Ordered)
		{
			FBuiltState& State = Saved.AddDefaulted_GetRef();
			State.Model = Brush->Brush;
			State.Location = State.Model->OwnerLocationWhenLastBuilt;
			State.Rotation = State.Model->OwnerRotationWhenLastBuilt;
			State.Scale = State.Model->OwnerScaleWhenLastBuilt;
			State.bValid = State.Model->bCachedOwnerTransformValid;
			State.NumUniqueVertices = State.Model->NumUniqueVertices;
		}
		ON_SCOPE_EXIT
		{
			for (const FBuiltState& State : Saved)
			{
				State.Model->OwnerLocationWhenLastBuilt = State.Location;
				State.Model->OwnerRotationWhenLastBuilt = State.Rotation;
				State.Model->OwnerScaleWhenLastBuilt = State.Scale;
				State.Model->bCachedOwnerTransformValid = State.bValid;
				State.Model->NumUniqueVertices = State.NumUniqueVertices;
			}
		};

		// The cut itself is the engine's (UEditorEngine::RebuildModelFromBrushes, which this follows) without its call to Brush->Modify, which
		// would tell every listener that the creator's brush changed.
		UModel* Model = NewObject<UModel>(GetTransientPackage(), NAME_None, RF_Transient);
		Model->Initialize(nullptr, /*InRootOutside*/ true);
		{
			TUniquePtr<FBspPointsGrid> BspPoints = MakeUnique<FBspPointsGrid>(50.0f, THRESH_POINTS_ARE_SAME);
			TUniquePtr<FBspPointsGrid> BspVectors = MakeUnique<FBspPointsGrid>(1.0f / 16.0f, FMath::Max(THRESH_NORMALS_ARE_SAME, THRESH_VECTORS_ARE_NEAR));
			FBspPointsGrid::GBspPoints = BspPoints.Get();
			FBspPointsGrid::GBspVectors = BspVectors.Get();
			ON_SCOPE_EXIT
			{
				FBspPointsGrid::GBspPoints = nullptr;
				FBspPointsGrid::GBspVectors = nullptr;
			};
			for (ABrush* Brush : Ordered)
			{
				GEditor->bspBrushCSG(Brush, Model, Brush->PolyFlags, static_cast<EBrushType>(Brush->BrushType.GetValue()), CSG_None,
					/*bBuildBounds*/ false, /*bMergePolys*/ true, /*bReplaceNULLMaterialRefs*/ false, /*bShowProgressBar*/ false);
			}
		}
		if (Model->Nodes.Num() == 0)
		{
			return true; // everything cut away: nothing to publish, and nothing wrong
		}
		GEditor->bspBuildFPolys(Model, /*SurfLinks*/ true, /*iNode*/ 0);

		// The visible surfaces, by material, the not-solid ones (a sheet a player walks through) apart from the solid: they draw, but the mesh
		// they are in has no collision.
		TArray<FGroup> Groups;
		TMap<FString, int32> GroupByMaterial;
		for (const FPoly& Poly : Model->Polys->Element)
		{
			if (Poly.Vertices.Num() < 3 || (Poly.PolyFlags & (PF_Invisible | PF_Portal)))
			{
				continue;
			}
			UMaterialInterface* Material = Poly.Material ? Poly.Material.Get() : UMaterial::GetDefaultMaterial(MD_Surface);
			const bool bNotSolid = (Poly.PolyFlags & PF_NotSolid) != 0;
			int32& GroupIndex = GroupByMaterial.FindOrAdd(Material->GetPathName() + (bNotSolid ? TEXT("|notsolid") : TEXT("")), INDEX_NONE);
			if (GroupIndex == INDEX_NONE)
			{
				GroupIndex = Groups.Num();
				FGroup& Added = Groups.AddDefaulted_GetRef();
				Added.Material = Material;
				if (bNotSolid)
				{
					Added.Tag = TEXT("notsolid");
					Added.Collision = EMantleCollision::None;
				}
			}
			FSurface Surface;
			Surface.Poly = &Poly;
			Surface.Material = Material;
			for (const FVector3f& Vertex : Poly.Vertices)
			{
				Surface.Center += FVector(Vertex);
			}
			Surface.Center /= Poly.Vertices.Num();
			Surface.Triangles = (Poly.Vertices.Num() - 2) * ((Poly.PolyFlags & PF_TwoSided) ? 2 : 1);
			Groups[GroupIndex].Surfaces.Add(Surface);
		}

		return BakeGroups(Groups, LevelKey, TriangleCap, VertexCap, OutChunks, OutWhy);
	}
}

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleBrushBakePrivate
{
	/** Cuts Groups' surfaces into chunks within the caps, along a Morton curve, and bakes each into a mesh. Key is part of the mesh ids. */
	bool BakeGroups(const TArray<FGroup>& Groups, const FString& Key, int32 TriangleCap, int32 VertexCap, TArray<FCachedChunk>& OutChunks, FString& OutWhy)
	{
		for (const FGroup& Group : Groups)
		{
			FBox GroupBounds(ForceInit);
			for (const FSurface& Surface : Group.Surfaces)
			{
				GroupBounds += Surface.Center;
			}
			TArray<const FSurface*> Order;
			for (const FSurface& Surface : Group.Surfaces)
			{
				Order.Add(&Surface);
			}
			// (Sorting an array of pointers hands the predicate what they point to.)
			Order.StableSort([&GroupBounds](const FSurface& A, const FSurface& B)
			{
				return BrushMorton(A.Center, GroupBounds) < BrushMorton(B.Center, GroupBounds);
			});

			// Chunks within the caps (a surface of n corners makes n - 2 triangles and, here, three vertices a triangle).
			TArray<TArray<const FSurface*>> Chunks;
			int32 Triangles = 0;
			for (const FSurface* Surface : Order)
			{
				if (Chunks.Num() == 0 || Triangles + Surface->Triangles > TriangleCap || (Triangles + Surface->Triangles) * 3 > VertexCap)
				{
					Chunks.AddDefaulted();
					Triangles = 0;
				}
				Chunks.Last().Add(Surface);
				Triangles += Surface->Triangles;
			}

			for (int32 ChunkIndex = 0; ChunkIndex < Chunks.Num(); ++ChunkIndex)
			{
				FBox ChunkBounds(ForceInit);
				for (const FSurface* Surface : Chunks[ChunkIndex])
				{
					for (const FVector3f& Vertex : Surface->Poly->Vertices)
					{
						ChunkBounds += FVector(Vertex);
					}
				}
				const FVector Center = ChunkBounds.GetCenter();
				FCachedChunk Chunk;
				Chunk.Pivot = FVector(FMath::RoundToDouble(Center.X), FMath::RoundToDouble(Center.Y), FMath::RoundToDouble(Center.Z));
				Chunk.Name = FString::Printf(TEXT("%s (%s) %d/%d"), *Group.NamePrefix, *Group.Material->GetName(), ChunkIndex + 1, Chunks.Num());
				Chunk.IdSeed = FString::Printf(TEXT("%s|%s|%s|%d"), *Key, *Group.Material->GetPathName(), *Group.Tag, ChunkIndex);
				Chunk.bVisible = Group.bVisible;
				Chunk.bBlocksVisibility = Group.bBlocksVisibility;
				Chunk.Collision = Group.Collision;
				const FString MeshName = FString::Printf(TEXT("MantleBsp_%s_%d"), *BrushMd5Hex(Chunk.IdSeed).Left(8), ChunkIndex);
				FString Why;
				if (!BakeChunk(Group, Chunks[ChunkIndex], Chunk.Pivot, MeshName, Chunk, Why))
				{
					OutWhy = Why;
					return false;
				}
				OutChunks.Add(MoveTemp(Chunk));
			}
		}
		return true;
	}
}

bool FMantleBrushBake::IsBakeSource(const ABrush* Brush)
{
	if (!IsValid(Brush) || Brush->HasAnyFlags(RF_Transient) || FActorEditorUtils::IsABuilderBrush(Brush))
	{
		return false;
	}
	const UWorld* World = Brush->GetWorld();
	if (!World || World->GetDefaultBrush() == Brush || !Brush->GetLevel())
	{
		return false;
	}
	// A volume, a brush shape and any movable brush say IsStaticBrush false (or are volumes): none of them is BSP.
	if (Brush->IsVolumeBrush() || !Brush->IsStaticBrush())
	{
		return false;
	}
	return Brush->Brush && Brush->Brush->Polys && Brush->Brush->Polys->Element.Num() > 0;
}

TArray<ABrush*> FMantleBrushBake::CollectSources(UWorld* World)
{
	TArray<ABrush*> Sources;
	if (!World)
	{
		return Sources;
	}
	// Each level's brushes in the level's own actor order: the editor combines brushes in that order (a wall, then the doorway cut in it),
	// and Order > Send to First/Last changes it.
	for (const ULevel* Level : World->GetLevels())
	{
		if (!Level)
		{
			continue;
		}
		for (AActor* Actor : Level->Actors)
		{
			if (ABrush* Brush = Cast<ABrush>(Actor); Brush && IsBakeSource(Brush))
			{
				Sources.Add(Brush);
			}
		}
	}
	return Sources;
}

FMantleSplineBakeResult FMantleBrushBake::Bake(UWorld* World, const FMantlePlaceAssetLimits& Limits)
{
	return BakeBrushes(CollectSources(World), Limits, World);
}

FMantleSplineBakeResult FMantleBrushBake::BakeBrushes(TConstArrayView<ABrush*> Brushes, const FMantlePlaceAssetLimits& Limits, UWorld* PruneWorld)
{
	using namespace MantleBrushBakePrivate;
	FMantleSplineBakeResult Result;

	const int32 TriangleCap = FMath::Min(Limits.MaxTriangles, SoftMaxTriangles);
	const int32 VertexCap = FMath::Min(Limits.MaxVertices, SoftMaxVertices);

	// One model a level: brushes in different levels do not cut each other.
	TMap<FString, TArray<ABrush*>> ByLevel;
	for (ABrush* Brush : Brushes)
	{
		if (IsBakeSource(Brush))
		{
			ByLevel.FindOrAdd(Brush->GetLevel()->GetPathName()).Add(Brush);
		}
	}
	TArray<FString> LevelKeys;
	ByLevel.GetKeys(LevelKeys);
	LevelKeys.Sort();

	FMantleSplineBakeReport Report;
	Report.OwnerLabel = OwnerLabelText;
	TSet<FString> Used;
	for (const FString& LevelKey : LevelKeys)
	{
		TArray<ABrush*>& LevelBrushes = ByLevel[LevelKey];
		// In the level's actor order, whatever order the caller gave them in (and so in the cache key below): the cut depends on it.
		const ULevel* BrushLevel = LevelBrushes[0]->GetLevel();
		TMap<const ABrush*, int32> ActorIndex;
		for (const ABrush* Brush : LevelBrushes)
		{
			ActorIndex.Add(Brush, BrushLevel->Actors.IndexOfByPredicate([Brush](const AActor* Actor) { return Actor == Brush; }));
		}
		LevelBrushes.StableSort([&ActorIndex](const ABrush& A, const ABrush& B)
		{
			return ActorIndex[&A] < ActorIndex[&B];
		});

		// Everything that changes the bake is in the signature, so a cached bake is reused only when it would come out the same.
		FString Signature = FString::Printf(TEXT("%s|%d|%d|%s"), BakeVersion, TriangleCap, VertexCap, *LevelKey);
		for (const ABrush* Brush : LevelBrushes)
		{
			Signature += TEXT("\n") + BrushMd5Hex(BrushSignature(Brush));
		}
		const FString CacheKey = LevelKey + TEXT("#") + BrushMd5Hex(Signature);
		Used.Add(CacheKey);

		FBakeCache& Store = BrushCache();
		FCacheEntry* Entry = Store.Entries.Find(CacheKey);
		if (!Entry)
		{
			FCacheEntry Fresh;
			Fresh.bHadWorld = PruneWorld != nullptr;
			Fresh.World = PruneWorld;
			Fresh.Kind = TEXT("bsp");
			Fresh.Brushes = LevelBrushes.Num();
			FString Why;
			if (!BakeLevel(LevelBrushes, LevelKey, TriangleCap, VertexCap, Fresh.Chunks, Why))
			{
				const ABrush* Owner = LevelBrushes[0];
				Result.Problems.Add(FMantleSplineBakeProblem{ Owner->GetActorLabel(), Owner->GetName(),
					FString::Printf(TEXT("is BSP geometry that couldn't be baked into a static mesh: %s."), *Why),
					TEXT("Convert the level's BSP to a static mesh (Actor > Convert), or rebuild it from library meshes.") });
				continue; // not cached: a fixed source is tried again
			}
			Entry = &Store.Entries.Add(CacheKey, MoveTemp(Fresh));
		}

		Report.Components += Entry->Brushes;
		Report.Meshes += Entry->Chunks.Num();
		for (const FCachedChunk& Chunk : Entry->Chunks)
		{
			FMantleBakedSplineMesh& Baked = Result.Meshes.AddDefaulted_GetRef();
			Baked.Mesh = Chunk.Mesh;
			Baked.Name = Chunk.Name;
			Baked.IdSeed = Chunk.IdSeed;
			Baked.OwnerLabel = Report.OwnerLabel;
			Baked.OwnerName = TEXT("BSP");
			Baked.Transform = FTransform(Chunk.Pivot);
			Baked.Collision = Chunk.Collision;
			Baked.Surface = EMantleSurface::Default;
			Baked.bCastShadow = true;
			Baked.SourceComponents = Entry->Brushes;
			Baked.Triangles = Chunk.Triangles;
			Baked.Vertices = Chunk.Vertices;
		}
	}

	// What this world baked before and no longer does (a brush moved, was removed, or changed) goes.
	FBakeCache& Store = BrushCache();
	for (auto It = Store.Entries.CreateIterator(); It; ++It)
	{
		const FCacheEntry& Entry = It.Value();
		const bool bGone = Entry.bHadWorld && !Entry.World.IsValid();
		const bool bReplaced = PruneWorld && Entry.Kind == TEXT("bsp") && Entry.World.Get() == PruneWorld && !Used.Contains(It.Key());
		if (bGone || bReplaced)
		{
			It.RemoveCurrent();
		}
	}

	if (Report.Components > 0)
	{
		Result.Reports.Add(Report);
		UE_LOG(LogMantle, Log, TEXT("MantleBrushBake: %d brush%s baked into %d mesh%s"),
			Report.Components, Report.Components == 1 ? TEXT("") : TEXT("es"), Report.Meshes, Report.Meshes == 1 ? TEXT("") : TEXT("es"));
	}
	return Result;
}

bool FMantleBrushBake::IsVolumeSource(const ABrush* Brush)
{
	if (!IsValid(Brush) || Brush->HasAnyFlags(RF_Transient) || !Brush->GetLevel() || !FMantleBlockingVolumeExport::IsBlockingVolume(Brush)
		|| !Brush->Brush || !Brush->Brush->Polys)
	{
		return false;
	}
	EMantleCollision Collision;
	bool bBlocksVisibility;
	FVector Center;
	FVector Size;
	FMantleBlockingWall Wall;
	// Collides with something, has a volume, and is not a box (a box is a wall: one hidden cube).
	return FMantleBlockingVolumeExport::ReadCollision(Brush, Collision, bBlocksVisibility)
		&& FMantleBlockingVolumeExport::ReadLocalBounds(Brush, Center, Size)
		&& !FMantleBlockingVolumeExport::ReadWall(Brush, Wall);
}

TArray<ABrush*> FMantleBrushBake::CollectVolumes(UWorld* World)
{
	TArray<ABrush*> Volumes;
	if (!World)
	{
		return Volumes;
	}
	for (const ULevel* Level : World->GetLevels())
	{
		if (!Level)
		{
			continue;
		}
		for (AActor* Actor : Level->Actors)
		{
			if (ABrush* Brush = Cast<ABrush>(Actor); Brush && IsVolumeSource(Brush))
			{
				Volumes.Add(Brush);
			}
		}
	}
	return Volumes;
}

FMantleSplineBakeResult FMantleBrushBake::BakeVolumes(UWorld* World, const FMantlePlaceAssetLimits& Limits)
{
	return BakeVolumeBrushes(CollectVolumes(World), Limits, World);
}

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleBrushBakePrivate
{
	/** A volume's brush polygons as world-space, two-sided surfaces (a collision mesh stops a player from either side) in groups by collision and sight rule. */
	bool BakeVolumeLevel(const TArray<ABrush*>& Volumes, const FString& LevelKey, int32 TriangleCap, int32 VertexCap, TArray<FCachedChunk>& OutChunks, FString& OutWhy)
	{
		int32 PolyCount = 0;
		for (const ABrush* Volume : Volumes)
		{
			PolyCount += Volume->Brush->Polys->Element.Num();
		}
		// The surfaces point into this array: it never grows past what it is given here.
		TArray<FPoly> WorldPolys;
		WorldPolys.Reserve(PolyCount);

		UMaterialInterface* Material = UMaterial::GetDefaultMaterial(MD_Surface);
		TArray<FGroup> Groups;
		TMap<FString, int32> GroupByTag;
		for (const ABrush* Volume : Volumes)
		{
			EMantleCollision Collision = EMantleCollision::Blocking;
			bool bBlocksVisibility = true;
			if (!FMantleBlockingVolumeExport::ReadCollision(Volume, Collision, bBlocksVisibility))
			{
				continue;
			}
			const FString Tag = FString::Printf(TEXT("%d|%d"), static_cast<int32>(Collision), bBlocksVisibility ? 1 : 0);
			int32& GroupIndex = GroupByTag.FindOrAdd(Tag, INDEX_NONE);
			if (GroupIndex == INDEX_NONE)
			{
				GroupIndex = Groups.Num();
				FGroup& Added = Groups.AddDefaulted_GetRef();
				Added.Material = Material;
				Added.Tag = Tag;
				Added.bVisible = false;
				Added.bBlocksVisibility = bBlocksVisibility;
				Added.Collision = Collision;
				Added.NamePrefix = TEXT("Blocking volumes");
			}
			const FTransform& World = Volume->GetBrushComponent()->GetComponentTransform();
			for (const FPoly& Source : Volume->Brush->Polys->Element)
			{
				if (Source.Vertices.Num() < 3)
				{
					continue;
				}
				FPoly& Poly = WorldPolys.AddDefaulted_GetRef();
				Poly = Source;
				// Nothing is drawn, so no mapping; two-sided, so the triangles stop a player from either side whatever the winding.
				Poly.PolyFlags = PF_TwoSided;
				Poly.Base = FVector3f::ZeroVector;
				Poly.TextureU = FVector3f::ZeroVector;
				Poly.TextureV = FVector3f::ZeroVector;
				FSurface Surface;
				Surface.Poly = &Poly;
				Surface.Material = Material;
				for (FVector3f& Vertex : Poly.Vertices)
				{
					Vertex = FVector3f(World.TransformPosition(FVector(Vertex)));
					Surface.Center += FVector(Vertex);
				}
				Surface.Center /= Poly.Vertices.Num();
				Surface.Triangles = (Poly.Vertices.Num() - 2) * 2;
				Groups[GroupIndex].Surfaces.Add(Surface);
			}
		}
		return BakeGroups(Groups, LevelKey, TriangleCap, VertexCap, OutChunks, OutWhy);
	}
}

FMantleSplineBakeResult FMantleBrushBake::BakeVolumeBrushes(TConstArrayView<ABrush*> Volumes, const FMantlePlaceAssetLimits& Limits, UWorld* PruneWorld)
{
	using namespace MantleBrushBakePrivate;
	FMantleSplineBakeResult Result;

	const int32 TriangleCap = FMath::Min(Limits.MaxTriangles, SoftMaxTriangles);
	const int32 VertexCap = FMath::Min(Limits.MaxVertices, SoftMaxVertices);

	TMap<FString, TArray<ABrush*>> ByLevel;
	for (ABrush* Volume : Volumes)
	{
		if (IsVolumeSource(Volume))
		{
			ByLevel.FindOrAdd(Volume->GetLevel()->GetPathName()).Add(Volume);
		}
	}
	TArray<FString> LevelKeys;
	ByLevel.GetKeys(LevelKeys);
	LevelKeys.Sort();

	FMantleSplineBakeReport Report;
	Report.OwnerLabel = TEXT("Blocking volumes");
	TSet<FString> Used;
	for (const FString& LevelKey : LevelKeys)
	{
		TArray<ABrush*>& LevelVolumes = ByLevel[LevelKey];
		// A fixed order (the actor order), so the same volumes always bake and cache the same.
		const ULevel* VolumeLevel = LevelVolumes[0]->GetLevel();
		TMap<const ABrush*, int32> ActorIndex;
		for (const ABrush* Volume : LevelVolumes)
		{
			ActorIndex.Add(Volume, VolumeLevel->Actors.IndexOfByPredicate([Volume](const AActor* Actor) { return Actor == Volume; }));
		}
		LevelVolumes.StableSort([&ActorIndex](const ABrush& A, const ABrush& B)
		{
			return ActorIndex[&A] < ActorIndex[&B];
		});

		FString Signature = FString::Printf(TEXT("vol|%s|%d|%d|%s"), BakeVersion, TriangleCap, VertexCap, *LevelKey);
		for (const ABrush* Volume : LevelVolumes)
		{
			EMantleCollision Collision = EMantleCollision::Blocking;
			bool bBlocksVisibility = true;
			FMantleBlockingVolumeExport::ReadCollision(Volume, Collision, bBlocksVisibility);
			Signature += TEXT("\n") + BrushMd5Hex(BrushSignature(Volume) + FString::Printf(TEXT("|%d|%d|%s"), static_cast<int32>(Collision),
				bBlocksVisibility ? 1 : 0, *BrushVector(Volume->GetBrushComponent()->GetComponentTransform().GetScale3D())));
		}
		const FString CacheKey = TEXT("vol|") + LevelKey + TEXT("#") + BrushMd5Hex(Signature);
		Used.Add(CacheKey);

		FBakeCache& Store = BrushCache();
		FCacheEntry* Entry = Store.Entries.Find(CacheKey);
		if (!Entry)
		{
			FCacheEntry Fresh;
			Fresh.bHadWorld = PruneWorld != nullptr;
			Fresh.World = PruneWorld;
			Fresh.Kind = TEXT("vol");
			Fresh.Brushes = LevelVolumes.Num();
			FString Why;
			if (!BakeVolumeLevel(LevelVolumes, LevelKey, TriangleCap, VertexCap, Fresh.Chunks, Why))
			{
				const ABrush* Owner = LevelVolumes[0];
				Result.Problems.Add(FMantleSplineBakeProblem{ Owner->GetActorLabel(), Owner->GetName(),
					FString::Printf(TEXT("is a Blocking Volume that couldn't be baked into collision: %s."), *Why),
					TEXT("Reshape it as a box, or build the wall from a hidden static mesh.") });
				continue;
			}
			Entry = &Store.Entries.Add(CacheKey, MoveTemp(Fresh));
		}

		Report.Components += Entry->Brushes;
		Report.Meshes += Entry->Chunks.Num();
		for (const FCachedChunk& Chunk : Entry->Chunks)
		{
			FMantleBakedSplineMesh& Baked = Result.Meshes.AddDefaulted_GetRef();
			Baked.Mesh = Chunk.Mesh;
			Baked.Name = Chunk.Name;
			Baked.IdSeed = Chunk.IdSeed;
			Baked.OwnerLabel = Report.OwnerLabel;
			Baked.OwnerName = TEXT("BlockingVolumes");
			Baked.Transform = FTransform(Chunk.Pivot);
			Baked.Collision = Chunk.Collision;
			Baked.Surface = EMantleSurface::Default;
			Baked.bCastShadow = false;
			Baked.bVisible = Chunk.bVisible;
			Baked.bBlocksVisibility = Chunk.bBlocksVisibility;
			Baked.SourceComponents = Entry->Brushes;
			Baked.Triangles = Chunk.Triangles;
			Baked.Vertices = Chunk.Vertices;
		}
	}

	FBakeCache& Store = BrushCache();
	for (auto It = Store.Entries.CreateIterator(); It; ++It)
	{
		const FCacheEntry& Entry = It.Value();
		const bool bGone = Entry.bHadWorld && !Entry.World.IsValid();
		const bool bReplaced = PruneWorld && Entry.Kind == TEXT("vol") && Entry.World.Get() == PruneWorld && !Used.Contains(It.Key());
		if (bGone || bReplaced)
		{
			It.RemoveCurrent();
		}
	}
	if (Report.Components > 0)
	{
		Result.Reports.Add(Report);
	}
	return Result;
}

void FMantleBrushBake::ClearCache()
{
	MantleBrushBakePrivate::BrushCache().Entries.Empty();
}
