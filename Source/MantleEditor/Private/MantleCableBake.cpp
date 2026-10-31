// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCableBake.h"

#include "MantleEnvironmentExport.h"
#include "MantlePlaceAssets.h"
#include "MantlePlaceExporter.h"
#include "MantleRuntime.h"

#include "CableComponent.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "Misc/SecureHash.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshResources.h"
#include "UObject/GCObject.h"
#include "UObject/Package.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleCableBakePrivate
{
	/** The spline bake's soft caps: a baked mesh stays well under the place mesh limits (the limits themselves are applied on top). */
	constexpr int32 SoftMaxTriangles = 250000;
	constexpr int32 SoftMaxVertices = 400000;

	/** Bumped when what a bake writes changes, so a cached bake from before is not reused. */
	const TCHAR* const BakeVersion = TEXT("cablebake-1");

	/** The one name of the single material slot every baked cable mesh has. */
	const TCHAR* const SlotName = TEXT("MantleCableMaterial_0");

	/** One cable, read for baking. */
	struct FSource
	{
		UCableComponent* Component = nullptr;
		AActor* Owner = nullptr;
		UMaterialInterface* Material = nullptr;

		/** The cable's points in world space, from its first end to its last, to a tenth of a unit. */
		TArray<FVector> Points;

		int32 NumSides = 1;
		int32 Triangles = 0;
		int32 Vertices = 0;
		FBox WorldBounds = FBox(ForceInit);
		bool bCastShadow = true;
	};

	/** Cables that share a material and shadow: baked together. */
	struct FGroup
	{
		FString Key;
		UMaterialInterface* Material = nullptr;
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
			return TEXT("FMantleCableBakeCache");
		}
	};

	FBakeCache& CableCache()
	{
		// Never destroyed: a static destructor would run after the object system is gone.
		static FBakeCache* Instance = new FBakeCache();
		return *Instance;
	}

	FString CableMd5Hex(const FString& Text)
	{
		return FMD5::HashAnsiString(*Text);
	}

	const FStaticMeshLODResources* CableFirstLOD(const UStaticMesh* Mesh)
	{
		const FStaticMeshRenderData* RenderData = Mesh ? Mesh->GetRenderData() : nullptr;
		return RenderData && RenderData->LODResources.IsValidIndex(0) ? &RenderData->LODResources[0] : nullptr;
	}

	FString CableNumber(double Value)
	{
		return FString::SanitizeFloat(Value, 3);
	}

	/** A point to a tenth of a unit: a cable that has settled keeps the same shape (and signature) from one call to the next. */
	FVector CableRounded(const FVector& Point)
	{
		return FVector(FMath::RoundToDouble(Point.X * 10.0) / 10.0, FMath::RoundToDouble(Point.Y * 10.0) / 10.0, FMath::RoundToDouble(Point.Z * 10.0) / 10.0);
	}

	/** Everything about a source that changes what its bake looks like. */
	FString SourceSignature(const FSource& Source)
	{
		const UCableComponent* Component = Source.Component;
		FString Text = FString::Printf(TEXT("%s|%s|%d|%s|%s"), *Component->GetPathName(), *Source.Material->GetPathName(), Source.NumSides,
			*CableNumber(Component->CableWidth), *CableNumber(Component->TileMaterial));
		for (const FVector& Point : Source.Points)
		{
			Text += TEXT("|") + CableNumber(Point.X) + TEXT(",") + CableNumber(Point.Y) + TEXT(",") + CableNumber(Point.Z);
		}
		return Text;
	}

	/** A Morton code of Point inside Box, 10 bits an axis: nearby cables land in the same chunk. */
	uint32 CableMorton(const FVector& Point, const FBox& Box)
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
	 * Adds one cable's tube to Description, in world space relative to Pivot, as the Cable Component builds it
	 * (FCableSceneProxy::BuildCableMesh): a ring of NumSides + 1 vertices at every point (the first and last of the
	 * ring apart, because their UVs differ), the ring's frame turned from the cable's direction there, the same
	 * triangles in the same order.
	 */
	void AddTube(FMeshDescription& Description, const FSource& Source, const FVector& Pivot, FPolygonGroupID PolygonGroup)
	{
		FStaticMeshAttributes Attributes(Description);
		TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
		TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
		TVertexInstanceAttributesRef<FVector3f> Tangents = Attributes.GetVertexInstanceTangents();
		TVertexInstanceAttributesRef<float> BinormalSigns = Attributes.GetVertexInstanceBinormalSigns();
		TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();

		const UCableComponent* Component = Source.Component;
		const int32 NumPoints = Source.Points.Num();
		const int32 NumSegments = NumPoints - 1;
		const int32 NumRingVertices = Source.NumSides + 1;

		TArray<FVertexInstanceID> Corners;
		Corners.Reserve(NumPoints * NumRingVertices);
		FVector ForwardDir = FVector::XAxisVector;
		for (int32 PointIndex = 0; PointIndex < NumPoints; ++PointIndex)
		{
			const float AlongFrac = static_cast<float>(PointIndex) / static_cast<float>(NumSegments);

			// The direction of the cable here: the points on either side of this one.
			const int32 PrevIndex = FMath::Max(0, PointIndex - 1);
			const int32 NextIndex = FMath::Min(PointIndex + 1, NumPoints - 1);
			const FVector Direction = (Source.Points[NextIndex] - Source.Points[PrevIndex]).GetSafeNormal();
			ForwardDir = Direction.IsNearlyZero() ? ForwardDir : Direction;

			const FQuat DeltaQuat = FQuat::FindBetween(FVector(0, 0, -1), ForwardDir);
			const FVector RightDir = DeltaQuat.RotateVector(FVector(0, 1, 0));
			const FVector UpDir = DeltaQuat.RotateVector(FVector(1, 0, 0));

			for (int32 RingIndex = 0; RingIndex < NumRingVertices; ++RingIndex)
			{
				const float AroundFrac = static_cast<float>(RingIndex) / static_cast<float>(Source.NumSides);
				const float RadAngle = 2.f * PI * AroundFrac;
				const FVector OutDir = (FMath::Cos(RadAngle) * UpDir) + (FMath::Sin(RadAngle) * RightDir);

				const FVertexID Vertex = Description.CreateVertex();
				Positions[Vertex] = FVector3f(Source.Points[PointIndex] + (OutDir * 0.5f * Component->CableWidth) - Pivot);
				const FVertexInstanceID Corner = Description.CreateVertexInstance(Vertex);
				Normals[Corner] = FVector3f(OutDir);
				Tangents[Corner] = FVector3f(ForwardDir);
				// The component's tangent basis is (forward, out ^ forward, out): its binormal is out ^ forward, so the sign is plus.
				BinormalSigns[Corner] = 1.0f;
				UVs.Set(Corner, 0, FVector2f(AlongFrac * Component->TileMaterial, AroundFrac));
				Corners.Add(Corner);
			}
		}

		for (int32 SegmentIndex = 0; SegmentIndex < NumSegments; ++SegmentIndex)
		{
			for (int32 SideIndex = 0; SideIndex < Source.NumSides; ++SideIndex)
			{
				const FVertexInstanceID TL = Corners[SegmentIndex * NumRingVertices + SideIndex];
				const FVertexInstanceID BL = Corners[SegmentIndex * NumRingVertices + SideIndex + 1];
				const FVertexInstanceID TR = Corners[(SegmentIndex + 1) * NumRingVertices + SideIndex];
				const FVertexInstanceID BR = Corners[(SegmentIndex + 1) * NumRingVertices + SideIndex + 1];
				Description.CreateTriangle(PolygonGroup, { TL, BL, TR });
				Description.CreateTriangle(PolygonGroup, { TR, BL, BR });
			}
		}
	}

	/** Merges the chunk's cables into one static mesh, in world space relative to Pivot, with the group's one material. */
	bool BakeChunk(const FGroup& Group, const TArray<const FSource*>& Chunk, const FVector& Pivot, const FString& MeshName, FCachedChunk& Out, FString& OutWhy)
	{
		FMeshDescription Combined;
		FStaticMeshAttributes Attributes(Combined);
		Attributes.Register();
		const FPolygonGroupID PolygonGroup = Combined.CreatePolygonGroup();
		Attributes.GetPolygonGroupMaterialSlotNames()[PolygonGroup] = FName(SlotName);
		for (const FSource* Source : Chunk)
		{
			AddTube(Combined, *Source, Pivot, PolygonGroup);
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
		// The geometry is final: nothing recomputed, no lightmap UVs, no distance field or Lumen cards for a thin wire.
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
		Descriptions.Add(&Combined);
		if (!Baked->BuildFromMeshDescriptions(Descriptions, Params))
		{
			OutWhy = TEXT("the engine could not build a static mesh from the merged geometry");
			return false;
		}
		const FStaticMeshLODResources* BuiltLOD = CableFirstLOD(Baked);
		if (!BuiltLOD || BuiltLOD->GetNumTriangles() <= 0)
		{
			OutWhy = TEXT("the merged static mesh has no triangles");
			return false;
		}

		// A cable collides with nothing (the entity's collision is None), so the body setup only has to exist for the upload.
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
}

bool FMantleCableBake::IsCableComponent(const UActorComponent* Component)
{
	return Component && Component->IsA(UCableComponent::StaticClass());
}

bool FMantleCableBake::IsBakeSource(const UCableComponent* Component)
{
	if (!IsValid(Component) || Component->HasAnyFlags(RF_Transient) || Component->IsEditorOnly())
	{
		return false;
	}
	const AActor* Owner = Component->GetOwner();
	if (!IsValid(Owner) || Owner->HasAnyFlags(RF_Transient) || FMantlePlaceExporter::IsProvidedByMantle(Owner))
	{
		return false;
	}
	// Something a player would see. (The component has no collision geometry in the game, whatever its profile says.)
	return FMantlePlaceExporter::IsShownInGame(Component);
}

TArray<UCableComponent*> FMantleCableBake::CollectSources(UWorld* World)
{
	TArray<UCableComponent*> Sources;
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
		TArray<UCableComponent*> Components;
		Actor->GetComponents(Components);
		Components.Sort([](const UCableComponent& A, const UCableComponent& B)
		{
			return A.GetName().Compare(B.GetName(), ESearchCase::CaseSensitive) < 0;
		});
		for (UCableComponent* Component : Components)
		{
			if (IsBakeSource(Component))
			{
				Sources.Add(Component);
			}
		}
	}
	return Sources;
}

FMantleSplineBakeResult FMantleCableBake::Bake(UWorld* World, const FMantlePlaceAssetLimits& Limits)
{
	return BakeComponents(CollectSources(World), Limits, World);
}

FMantleSplineBakeResult FMantleCableBake::BakeComponents(TConstArrayView<UCableComponent*> Components, const FMantlePlaceAssetLimits& Limits, UWorld* PruneWorld)
{
	using namespace MantleCableBakePrivate;
	FMantleSplineBakeResult Result;

	const int32 TriangleCap = FMath::Min(Limits.MaxTriangles, SoftMaxTriangles);
	const int32 VertexCap = FMath::Min(Limits.MaxVertices, SoftMaxVertices);

	FMantleSplineBakeReport Report;
	Report.OwnerLabel = TEXT("Cables");
	auto AddProblem = [&Result](const AActor* Owner, const FString& What, const FString& Remedy)
	{
		Result.Problems.Add(FMantleSplineBakeProblem{ Owner->GetActorLabel(), Owner->GetName(), What, Remedy });
	};

	// Read every cable, and group them. Sources are stable in memory (reserved up front) for the groups' pointers.
	TArray<FSource> Sources;
	Sources.Reserve(Components.Num());
	TArray<FGroup> Groups;
	TMap<FString, int32> GroupByKey;
	for (UCableComponent* Component : Components)
	{
		if (!IsBakeSource(Component))
		{
			continue;
		}
		AActor* Owner = Component->GetOwner();

		FSource Source;
		Source.Component = Component;
		Source.Owner = Owner;
		TArray<FVector> Points;
		Component->GetCableParticleLocations(Points);
		if (Points.Num() < 2)
		{
			++Report.Skipped; // a cable that has not been set up has no shape to bake
			continue;
		}
		for (const FVector& Point : Points)
		{
			Source.Points.Add(CableRounded(Point));
			Source.WorldBounds += Source.Points.Last();
		}
		Source.WorldBounds = Source.WorldBounds.ExpandBy(0.5 * Component->CableWidth);
		Source.NumSides = FMath::Max(1, Component->NumSides);
		Source.Triangles = (Points.Num() - 1) * Source.NumSides * 2;
		Source.Vertices = Points.Num() * (Source.NumSides + 1);
		if (Source.Triangles > TriangleCap || Source.Vertices > VertexCap)
		{
			AddProblem(Owner, FString::Printf(TEXT("has a cable with %d triangles and %d vertices; a place mesh may have at most %d and %d."),
				Source.Triangles, Source.Vertices, TriangleCap, VertexCap),
				TEXT("Give the cable fewer segments or sides."));
			continue;
		}
		UMaterialInterface* Material = Component->GetMaterial(0);
		Source.Material = Material ? Material : UMaterial::GetDefaultMaterial(MD_Surface);
		Source.bCastShadow = FMantleEnvironmentExport::CastsShadowInPlace(Component);

		const FString Key = FString::Printf(TEXT("%s|%d"), *Source.Material->GetPathName(), Source.bCastShadow ? 1 : 0);
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
			Added.Key = Key;
			Added.Material = Source.Material;
			Added.bCastShadow = Source.bCastShadow;
		}
		const int32 SourceIndex = Sources.Add(MoveTemp(Source));
		Groups[GroupIndex].Sources.Add(&Sources[SourceIndex]);
	}

	TSet<FString> Used;
	for (FGroup& Group : Groups)
	{
		// Cut into chunks along a Morton curve, so a chunk's cables are near each other, within the caps.
		FBox GroupBounds(ForceInit);
		for (const FSource* Source : Group.Sources)
		{
			GroupBounds += Source->WorldBounds;
		}
		TArray<TPair<uint32, const FSource*>> Order;
		for (const FSource* Source : Group.Sources)
		{
			Order.Emplace(CableMorton(Source->WorldBounds.GetCenter(), GroupBounds), Source);
		}
		Order.StableSort([](const TPair<uint32, const FSource*>& A, const TPair<uint32, const FSource*>& B)
		{
			if (A.Key != B.Key)
			{
				return A.Key < B.Key;
			}
			return A.Value->Component->GetPathName().Compare(B.Value->Component->GetPathName(), ESearchCase::CaseSensitive) < 0;
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
		FString Signature = FString::Printf(TEXT("%s|%d|%d"), BakeVersion, TriangleCap, VertexCap);
		for (const TPair<uint32, const FSource*>& Entry : Order)
		{
			Signature += TEXT("\n") + SourceSignature(*Entry.Value);
		}
		const FString CacheKey = Group.Key + TEXT("#") + CableMd5Hex(Signature);
		Used.Add(CacheKey);

		FBakeCache& Store = CableCache();
		FCacheEntry* Entry = Store.Entries.Find(CacheKey);
		if (!Entry)
		{
			FCacheEntry Fresh;
			Fresh.bHadWorld = PruneWorld != nullptr;
			Fresh.World = PruneWorld;
			bool bBaked = true;
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
				Chunk.Name = FString::Printf(TEXT("Cables (%s) %d/%d"), *Group.Material->GetName(), ChunkIndex + 1, Chunks.Num());
				Chunk.IdSeed = FString::Printf(TEXT("%s|%d"), *Group.Key, ChunkIndex);
				const FString MeshName = FString::Printf(TEXT("MantleCables_%s_%d"), *CableMd5Hex(Signature).Left(8), ChunkIndex);
				FString Why;
				if (!BakeChunk(Group, Chunks[ChunkIndex], Chunk.Pivot, MeshName, Chunk, Why))
				{
					for (const FSource* Source : Chunks[ChunkIndex])
					{
						AddProblem(Source->Owner, FString::Printf(TEXT("has a cable that couldn't be baked into a static mesh: %s."), *Why),
							TEXT("Check the cable's width, sides and segments, or replace it with a static mesh."));
					}
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

		Report.Components += Group.Sources.Num();
		Report.Meshes += Entry->Chunks.Num();
		for (const FCachedChunk& Chunk : Entry->Chunks)
		{
			FMantleBakedSplineMesh& Baked = Result.Meshes.AddDefaulted_GetRef();
			Baked.Mesh = Chunk.Mesh;
			Baked.Name = Chunk.Name;
			Baked.IdSeed = Chunk.IdSeed;
			Baked.OwnerLabel = Report.OwnerLabel;
			Baked.OwnerName = TEXT("Cables");
			Baked.Transform = FTransform(Chunk.Pivot);
			Baked.Collision = EMantleCollision::None;
			Baked.Surface = EMantleSurface::Default;
			Baked.bCastShadow = Group.bCastShadow;
			Baked.SourceComponents = Chunk.Components;
			Baked.Triangles = Chunk.Triangles;
			Baked.Vertices = Chunk.Vertices;
		}
	}

	// What this world baked before and no longer does (a cable moved, was removed, or changed) goes.
	FBakeCache& Store = CableCache();
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

	if (Report.Components > 0 || Report.Skipped > 0)
	{
		Result.Reports.Add(Report);
		UE_LOG(LogMantle, Log, TEXT("MantleCableBake: %d cable%s baked into %d mesh%s"),
			Report.Components, Report.Components == 1 ? TEXT("") : TEXT("s"), Report.Meshes, Report.Meshes == 1 ? TEXT("") : TEXT("es"));
	}
	return Result;
}

void FMantleCableBake::ClearCache()
{
	MantleCableBakePrivate::CableCache().Entries.Empty();
}
