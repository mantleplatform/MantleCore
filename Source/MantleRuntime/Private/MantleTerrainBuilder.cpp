// Copyright Joshua Eger. All Rights Reserved.
#include "MantleTerrainBuilder.h"

#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformTime.h"
#include "MantlePlaceLimits.h"
#include "MantlePlaceMaterial.h"
#include "MantleRuntime.h"
#include "MantleRuntimeStaticMeshBuilder.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleTerrainBuilderPrivate
{
	/** The least a skirt hangs below its edge, so a seam between two LODs never shows a sliver of sky. */
	constexpr float MinSkirtCm = 10.0f;

	/** MI_MantleTerrain once loaded, and whether its absence was already reported. Game thread. */
	TWeakObjectPtr<UMaterialInterface> GTerrainMaterial;
	bool bTerrainMaterialLookedUp = false;
	bool bWarnedTerrainMaterialMissing = false;

	bool FailTerrain(FString* OutError, const FString& Error)
	{
		UE_LOG(LogMantle, Warning, TEXT("MANTLE_TERRAIN FAIL %s"), *Error);
		if (OutError)
		{
			*OutError = Error;
		}
		return false;
	}

	/** The height along an edge: a row (bAlongX, Y = Fixed) or a column (X = Fixed). */
	float TerrainEdgeHeight(const FMantleHeightfield& Field, const bool bAlongX, const int32 Fixed, const int32 Sample)
	{
		return bAlongX ? Field.HeightAt(Sample, Fixed) : Field.HeightAt(Fixed, Sample);
	}

	UMaterialInterface* LoadTerrainMaterial()
	{
		if (UMaterialInterface* Loaded = GTerrainMaterial.Get())
		{
			return Loaded;
		}
		if (!bTerrainMaterialLookedUp)
		{
			bTerrainMaterialLookedUp = true;
			GTerrainMaterial = LoadObject<UMaterialInterface>(nullptr, FMantleTerrainBuilder::MaterialPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
		}
		return GTerrainMaterial.Get();
	}

	/** Whether Material is an M_MantlePlace instance (it has the base colour texture parameter the layers copy). */
	bool IsPlaceMaterial(const UMaterialInterface* Material)
	{
		UTexture* Unused = nullptr;
		return Material && Material->GetTextureParameterValue(
			FHashedMaterialParameterInfo(FName(MantlePlaceMaterialParameters::BaseColor)), Unused);
	}

	/**
	 * UStaticMesh::BuildFromMeshDescriptions' fast path (UE 5.8 StaticMesh.cpp, a mesh with no render resources
	 * yet, no simple collision), step for step, minus the ray tracing representation and InitResources: the
	 * engine's fast build ends in InitResources, which asserts in a cooked run that can never render. What
	 * is left is what collision reads, the CPU buffers, the bounds and the body setup. Every LOD in Descriptions
	 * is built, so a build with render LODs would also work; the dedicated server builds one. Mesh must be new, with
	 * bAllowCPUAccess already set as the caller wants it.
	 */
	bool BuildTerrainMeshCpuOnly(UStaticMesh& Mesh, const TArray<const FMeshDescription*>& Descriptions, const UStaticMesh::FBuildMeshDescriptionsParams& BuildParams)
	{
		// The CPU-only path supports only what the terrain passes.
		check(!BuildParams.bBuildSimpleCollision && BuildParams.bFastBuild);
		const int32 NumLODs = FMath::Min(Descriptions.Num(), MAX_STATIC_MESH_LODS);
		if (NumLODs < 1 || Mesh.AreRenderingResourcesInitialized())
		{
			return false;
		}
		Mesh.NeverStream = true;
		Mesh.bDoFastBuild = true;
		Mesh.SetRenderData(MakeUnique<FStaticMeshRenderData>());
		FStaticMeshRenderData* RenderData = Mesh.GetRenderData();
		RenderData->AllocateLODResources(NumLODs);
		for (int32 LODIndex = 0; LODIndex < NumLODs; ++LODIndex)
		{
			FStaticMeshLODResources& LODResources = RenderData->LODResources[LODIndex];
			LODResources.IndexBuffer.TrySetAllowCPUAccess(Mesh.bAllowCPUAccess || BuildParams.bAllowCpuAccess);
			if (BuildParams.PerLODOverrides.IsValidIndex(LODIndex))
			{
				const UStaticMesh::FBuildMeshDescriptionsLODParams& LODParams = BuildParams.PerLODOverrides[LODIndex];
				LODResources.VertexBuffers.StaticMeshVertexBuffer.SetUseHighPrecisionTangentBasis(LODParams.bUseHighPrecisionTangentBasis);
				LODResources.VertexBuffers.StaticMeshVertexBuffer.SetUseFullPrecisionUVs(LODParams.bUseFullPrecisionUVs);
			}
		}
		for (int32 LODIndex = 0; LODIndex < NumLODs; ++LODIndex)
		{
			check(Descriptions[LODIndex] != nullptr);
			Mesh.BuildFromMeshDescription(*Descriptions[LODIndex], RenderData->LODResources[LODIndex]);
		}
		RenderData->Bounds = Descriptions[0]->GetBounds();
		Mesh.CalculateExtendedBounds();
		for (int32 LODIndex = 0; LODIndex < NumLODs; ++LODIndex)
		{
			// The engine's own default; the caller replaces it with the terrain's.
			RenderData->ScreenSize[LODIndex].Default = FMath::Pow(0.75f, static_cast<float>(LODIndex));
		}
		Mesh.CreateBodySetup();
		if (!Mesh.GetBodySetup())
		{
			return false;
		}
		Mesh.GetBodySetup()->InvalidatePhysicsData();
		return true;
	}
}

int32 FMantleTerrainBuilder::ChunkCount(const int32 Samples, const int32 ChunkQuads)
{
	const int32 Quads = FMath::Max(Samples - 1, 1);
	const int32 PerChunk = FMath::Max(ChunkQuads, 1);
	return (Quads + PerChunk - 1) / PerChunk;
}

void FMantleTerrainBuilder::ChunkSpan(const int32 Samples, const int32 ChunkQuads, const int32 Chunk, int32& OutFirst, int32& OutLast)
{
	const int32 PerChunk = FMath::Max(ChunkQuads, 1);
	OutFirst = Chunk * PerChunk;
	OutLast = FMath::Min(OutFirst + PerChunk, FMath::Max(Samples - 1, 0));
}

void FMantleTerrainBuilder::AxisSamples(const int32 First, const int32 Last, const int32 Step, TArray<int32>& Out)
{
	Out.Reset();
	const int32 SafeStep = FMath::Max(Step, 1);
	for (int32 Sample = First; Sample < Last; Sample += SafeStep)
	{
		Out.Add(Sample);
	}
	Out.Add(Last);
}

int32 FMantleTerrainBuilder::LODCount(const int32 ChunkQuads, const int32 MaxLODs)
{
	int32 Count = 1;
	for (int32 Step = 2; Count < MaxLODs && Step <= ChunkQuads; Step *= 2)
	{
		++Count;
	}
	return Count;
}

float FMantleTerrainBuilder::LODScreenSize(const int32 Lod)
{
	return Lod <= 0 ? 1.0f : 0.5f / static_cast<float>(1 << FMath::Min(Lod, 30));
}

FVector3f FMantleTerrainBuilder::SampleNormal(const FMantleHeightfield& Field, const int32 X, const int32 Y)
{
	const int32 X0 = FMath::Max(X - 1, 0);
	const int32 X1 = FMath::Min(X + 1, Field.Width - 1);
	const int32 Y0 = FMath::Max(Y - 1, 0);
	const int32 Y1 = FMath::Min(Y + 1, Field.Height - 1);
	const float DzDx = X1 > X0 ? (Field.HeightAt(X1, Y) - Field.HeightAt(X0, Y)) / (static_cast<float>(X1 - X0) * Field.SpacingX) : 0.0f;
	const float DzDy = Y1 > Y0 ? (Field.HeightAt(X, Y1) - Field.HeightAt(X, Y0)) / (static_cast<float>(Y1 - Y0) * Field.SpacingY) : 0.0f;
	return FVector3f(-DzDx, -DzDy, 1.0f).GetSafeNormal(UE_SMALL_NUMBER, FVector3f::UpVector);
}

float FMantleTerrainBuilder::EdgeSkirtDepth(const FMantleHeightfield& Field, const bool bAlongX, const int32 Fixed, const int32 First,
	const int32 Last, const int32 LODs)
{
	using namespace MantleTerrainBuilderPrivate;
	if (LODs <= 1)
	{
		return 0.0f;
	}
	float Worst = 0.0f;
	TArray<int32> Kept;
	for (int32 Lod = 1; Lod < LODs; ++Lod)
	{
		AxisSamples(First, Last, 1 << Lod, Kept);
		for (int32 Segment = 0; Segment + 1 < Kept.Num(); ++Segment)
		{
			const int32 A = Kept[Segment];
			const int32 B = Kept[Segment + 1];
			const float HeightA = TerrainEdgeHeight(Field, bAlongX, Fixed, A);
			const float HeightB = TerrainEdgeHeight(Field, bAlongX, Fixed, B);
			for (int32 Sample = A + 1; Sample < B; ++Sample)
			{
				const float Alpha = static_cast<float>(Sample - A) / static_cast<float>(B - A);
				Worst = FMath::Max(Worst, FMath::Abs(TerrainEdgeHeight(Field, bAlongX, Fixed, Sample) - FMath::Lerp(HeightA, HeightB, Alpha)));
			}
		}
	}
	return Worst + MinSkirtCm;
}

void FMantleTerrainBuilder::BuildChunkDescription(const FMantleHeightfield& Field, const FIntPoint& First, const FIntPoint& Last,
	const int32 Step, const float SkirtDepth, const bool (&bSkirts)[4], FMeshDescription& Out)
{
	FStaticMeshAttributes Attributes(Out);
	Attributes.Register();

	TArray<int32> Xs;
	TArray<int32> Ys;
	AxisSamples(First.X, Last.X, Step, Xs);
	AxisSamples(First.Y, Last.Y, Step, Ys);
	const int32 NX = Xs.Num();
	const int32 NY = Ys.Num();
	const bool bAnySkirt = SkirtDepth > 0.0f && (bSkirts[0] || bSkirts[1] || bSkirts[2] || bSkirts[3]);

	// Skirt vertices and quads, for the reservations: left and right edges run along Y, bottom and top along X.
	int32 SkirtVertices = 0;
	int32 SkirtSegments = 0;
	if (bAnySkirt)
	{
		for (int32 EdgeIndex = 0; EdgeIndex < 4; ++EdgeIndex)
		{
			if (bSkirts[EdgeIndex])
			{
				const int32 EdgeLength = EdgeIndex < 2 ? NY : NX;
				SkirtVertices += EdgeLength;
				SkirtSegments += EdgeLength - 1;
			}
		}
	}
	const int32 Triangles = 2 * (NX - 1) * (NY - 1) + 4 * SkirtSegments;
	Out.ReserveNewVertices(NX * NY + SkirtVertices);
	Out.ReserveNewVertexInstances(NX * NY + SkirtVertices);
	Out.ReserveNewTriangles(Triangles);
	Out.ReserveNewPolygons(Triangles);
	Out.ReserveNewPolygonGroups(1);

	TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
	TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
	TVertexInstanceAttributesRef<FVector3f> Tangents = Attributes.GetVertexInstanceTangents();
	TVertexInstanceAttributesRef<float> BinormalSigns = Attributes.GetVertexInstanceBinormalSigns();
	TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
	TPolygonGroupAttributesRef<FName> GroupSlotNames = Attributes.GetPolygonGroupMaterialSlotNames();

	const FPolygonGroupID Group = Out.CreatePolygonGroup();
	GroupSlotNames[Group] = FName(MaterialSlotName);

	const FVector3f Origin(static_cast<float>(First.X) * Field.SpacingX, static_cast<float>(First.Y) * Field.SpacingY, 0.0f);
	const float InvU = 1.0f / static_cast<float>(FMath::Max(Field.Width - 1, 1));
	const float InvV = 1.0f / static_cast<float>(FMath::Max(Field.Height - 1, 1));

	// One vertex instance per vertex. The tangent is +X (the direction U grows) made perpendicular to
	// the normal; V grows along +Y, which with Unreal's Binormal = Normal x Tangent is a sign of +1.
	auto AddVertex = [&](const int32 X, const int32 Y, const float Drop)
	{
		const FVertexID Vertex = Out.CreateVertex();
		FVector3f Position = Field.PositionAt(X, Y) - Origin;
		Position.Z -= Drop;
		Positions[Vertex] = Position;
		const FVertexInstanceID Instance = Out.CreateVertexInstance(Vertex);
		const FVector3f Normal = SampleNormal(Field, X, Y);
		const FVector3f Tangent = (FVector3f(1.0f, 0.0f, 0.0f) - Normal * Normal.X).GetSafeNormal(UE_SMALL_NUMBER, FVector3f(1.0f, 0.0f, 0.0f));
		Normals[Instance] = Normal;
		Tangents[Instance] = Tangent;
		BinormalSigns[Instance] = 1.0f;
		UVs.Set(Instance, 0, FVector2f(static_cast<float>(X) * InvU, static_cast<float>(Y) * InvV));
		return Instance;
	};

	TArray<FVertexInstanceID> Grid;
	Grid.Reserve(NX * NY);
	for (int32 IY = 0; IY < NY; ++IY)
	{
		for (int32 IX = 0; IX < NX; ++IX)
		{
			Grid.Add(AddVertex(Xs[IX], Ys[IY], 0.0f));
		}
	}
	auto At = [&Grid, NX](const int32 IX, const int32 IY) { return Grid[IY * NX + IX]; };

	for (int32 IY = 0; IY + 1 < NY; ++IY)
	{
		for (int32 IX = 0; IX + 1 < NX; ++IX)
		{
			const FVertexInstanceID Lower[3] = { At(IX, IY), At(IX, IY + 1), At(IX + 1, IY) };
			const FVertexInstanceID Upper[3] = { At(IX + 1, IY), At(IX, IY + 1), At(IX + 1, IY + 1) };
			Out.CreateTriangle(Group, MakeArrayView(Lower));
			Out.CreateTriangle(Group, MakeArrayView(Upper));
		}
	}

	if (!bAnySkirt)
	{
		return;
	}
	// A skirt is a strip from the edge down SkirtDepth, both windings: a gap can be seen from either side.
	auto AddSkirt = [&](const TArray<FIntPoint>& EdgeSamples)
	{
		TArray<FVertexInstanceID> Hanging;
		Hanging.Reserve(EdgeSamples.Num());
		for (const FIntPoint& GridIndex : EdgeSamples)
		{
			Hanging.Add(AddVertex(Xs[GridIndex.X], Ys[GridIndex.Y], SkirtDepth));
		}
		for (int32 Segment = 0; Segment + 1 < EdgeSamples.Num(); ++Segment)
		{
			const FVertexInstanceID A = At(EdgeSamples[Segment].X, EdgeSamples[Segment].Y);
			const FVertexInstanceID B = At(EdgeSamples[Segment + 1].X, EdgeSamples[Segment + 1].Y);
			const FVertexInstanceID DownA = Hanging[Segment];
			const FVertexInstanceID DownB = Hanging[Segment + 1];
			const FVertexInstanceID Faces[4][3] = {
				{ A, B, DownA }, { B, DownB, DownA }, { A, DownA, B }, { B, DownA, DownB } };
			for (const FVertexInstanceID (&Face)[3] : Faces)
			{
				Out.CreateTriangle(Group, MakeArrayView(Face));
			}
		}
	};
	TArray<FIntPoint> SideSamples;
	for (int32 Side = 0; Side < 4; ++Side)
	{
		if (!bSkirts[Side])
		{
			continue;
		}
		SideSamples.Reset();
		const int32 Count = Side < 2 ? NY : NX;
		for (int32 Along = 0; Along < Count; ++Along)
		{
			switch (Side)
			{
			case 0: SideSamples.Add(FIntPoint(0, Along)); break;
			case 1: SideSamples.Add(FIntPoint(NX - 1, Along)); break;
			case 2: SideSamples.Add(FIntPoint(Along, 0)); break;
			default: SideSamples.Add(FIntPoint(Along, NY - 1)); break;
			}
		}
		AddSkirt(SideSamples);
	}
}

bool FMantleTerrainBuilder::BuildChunks(const FMantleHeightfield& Field, const FMantleTerrainBuildParams& Params,
	TArray<FMantleTerrainChunk>& Out, FString* OutError)
{
	using namespace MantleTerrainBuilderPrivate;

	Out.Reset();
	if (!IsInGameThread())
	{
		return FailTerrain(OutError, TEXT("terrain meshes are built on the game thread"));
	}
	if (Field.Width < FMantleHeightfieldReader::MinSize || Field.Height < FMantleHeightfieldReader::MinSize
		|| Field.Width > FMantleHeightfieldReader::MaxSize || Field.Height > FMantleHeightfieldReader::MaxSize
		|| static_cast<int64>(Field.Width) * Field.Height != Field.Heights.Num()
		|| !(Field.SpacingX > 0.0f) || !(Field.SpacingY > 0.0f) || !FMath::IsFinite(Field.SpacingX) || !FMath::IsFinite(Field.SpacingY)
		|| !FMath::IsFinite(Field.HeightScale) || !FMath::IsFinite(Field.HeightOffset))
	{
		return FailTerrain(OutError, FString::Printf(TEXT("the heightfield is not usable (%d x %d, %d samples, spacing %g x %g)"),
			Field.Width, Field.Height, Field.Heights.Num(), Field.SpacingX, Field.SpacingY));
	}

	const double StartSeconds = FPlatformTime::Seconds();
	const int32 ChunkQuads = FMath::Max(Params.ChunkQuads, 1);
	const int32 LODs = Params.bRenderLODs ? LODCount(ChunkQuads, FMath::Max(Params.MaxLODs, 1)) : 1;
	const int32 ChunksX = ChunkCount(Field.Width, ChunkQuads);
	const int32 ChunksY = ChunkCount(Field.Height, ChunkQuads);
	UObject* MeshOuter = Params.Outer ? Params.Outer : GetTransientPackage();

	TArray<FMantleTerrainChunk> Built;
	Built.Reserve(ChunksX * ChunksY);
	int64 Vertices = 0;
	for (int32 ChunkY = 0; ChunkY < ChunksY; ++ChunkY)
	{
		for (int32 ChunkX = 0; ChunkX < ChunksX; ++ChunkX)
		{
			FMantleTerrainChunk Chunk;
			ChunkSpan(Field.Width, ChunkQuads, ChunkX, Chunk.FirstSample.X, Chunk.LastSample.X);
			ChunkSpan(Field.Height, ChunkQuads, ChunkY, Chunk.FirstSample.Y, Chunk.LastSample.Y);
			Chunk.Origin = FVector3f(static_cast<float>(Chunk.FirstSample.X) * Field.SpacingX, static_cast<float>(Chunk.FirstSample.Y) * Field.SpacingY, 0.0f);

			// Skirts only where another chunk meets this one: left, right, bottom, top.
			const bool bSkirts[4] = {
				Chunk.FirstSample.X > 0, Chunk.LastSample.X < Field.Width - 1,
				Chunk.FirstSample.Y > 0, Chunk.LastSample.Y < Field.Height - 1 };
			float SkirtDepth = 0.0f;
			if (bSkirts[0]) { SkirtDepth = FMath::Max(SkirtDepth, EdgeSkirtDepth(Field, false, Chunk.FirstSample.X, Chunk.FirstSample.Y, Chunk.LastSample.Y, LODs)); }
			if (bSkirts[1]) { SkirtDepth = FMath::Max(SkirtDepth, EdgeSkirtDepth(Field, false, Chunk.LastSample.X, Chunk.FirstSample.Y, Chunk.LastSample.Y, LODs)); }
			if (bSkirts[2]) { SkirtDepth = FMath::Max(SkirtDepth, EdgeSkirtDepth(Field, true, Chunk.FirstSample.Y, Chunk.FirstSample.X, Chunk.LastSample.X, LODs)); }
			if (bSkirts[3]) { SkirtDepth = FMath::Max(SkirtDepth, EdgeSkirtDepth(Field, true, Chunk.LastSample.Y, Chunk.FirstSample.X, Chunk.LastSample.X, LODs)); }

			TArray<FMeshDescription> Descriptions;
			Descriptions.SetNum(LODs);
			TArray<const FMeshDescription*> DescriptionPointers;
			for (int32 Lod = 0; Lod < LODs; ++Lod)
			{
				BuildChunkDescription(Field, Chunk.FirstSample, Chunk.LastSample, 1 << Lod, SkirtDepth, bSkirts, Descriptions[Lod]);
				DescriptionPointers.Add(&Descriptions[Lod]);
			}
			Vertices += Descriptions[0].Vertices().Num();

			const FName MeshName = MakeUniqueObjectName(MeshOuter, UStaticMesh::StaticClass(),
				FName(*FString::Printf(TEXT("MantleTerrainChunk_%d_%d"), ChunkX, ChunkY)));
			UStaticMesh* Mesh = NewObject<UStaticMesh>(MeshOuter, MeshName, RF_Transient);
			Mesh->GetStaticMaterials().Add(FStaticMaterial(nullptr, FName(MaterialSlotName)));
			// A non-editor build reads collision triangles back from the render data, and only from a mesh
			// flagged for CPU access (UStaticMesh::GetPhysicsTriMeshData).
			Mesh->bAllowCPUAccess = Params.bCollision;

			UStaticMesh::FBuildMeshDescriptionsParams BuildParams;
			BuildParams.bMarkPackageDirty = false;
			BuildParams.bBuildSimpleCollision = false;
			BuildParams.bCommitMeshDescription = false;
			BuildParams.bFastBuild = true;
			BuildParams.bAllowCpuAccess = Params.bCollision;
			// UV0 spans the whole terrain and each layer repeats hundreds of times over it: half floats
			// would step visibly, so every LOD keeps full-precision UVs.
			BuildParams.PerLODOverrides.SetNum(LODs);
			for (UStaticMesh::FBuildMeshDescriptionsLODParams& LODParams : BuildParams.PerLODOverrides)
			{
				LODParams.bUseFullPrecisionUVs = true;
			}
			// The engine's build always ends in InitResources, which asserts on a dedicated server: there
			// the same mesh is built without render resources.
			const bool bBuilt = FMantleRuntimeStaticMeshBuilder::ShouldInitRenderResources(Params.bInitRenderResources)
				? Mesh->BuildFromMeshDescriptions(DescriptionPointers, BuildParams)
				: BuildTerrainMeshCpuOnly(*Mesh, DescriptionPointers, BuildParams);
			if (!bBuilt)
			{
				return FailTerrain(OutError, FString::Printf(TEXT("the engine could not build terrain chunk (%d, %d)"), ChunkX, ChunkY));
			}
			FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
			if (!RenderData || RenderData->LODResources.Num() != LODs)
			{
				return FailTerrain(OutError, FString::Printf(TEXT("the engine built %d LODs of terrain chunk (%d, %d), not %d"),
					RenderData ? RenderData->LODResources.Num() : 0, ChunkX, ChunkY, LODs));
			}
			for (int32 Lod = 0; Lod < LODs; ++Lod)
			{
				// Chosen by screen size, which for chunks of one size is by distance (LODScreenSize).
				RenderData->ScreenSize[Lod].Default = LODScreenSize(Lod);
				// One slot: bound explicitly, as FMantleRuntimeStaticMeshBuilder does, rather than trusting
				// the fast build's lookup of the slot name.
				for (FStaticMeshSection& Section : RenderData->LODResources[Lod].Sections)
				{
					Section.MaterialIndex = 0;
				}
			}

			if (Params.bCollision)
			{
				UBodySetup* Body = Mesh->GetBodySetup();
				if (!Body)
				{
					return FailTerrain(OutError, FString::Printf(TEXT("the engine made no body setup for terrain chunk (%d, %d)"), ChunkX, ChunkY));
				}
				// LOD 0's triangles are the collision, from either side, cooked now: a player must be able
				// to stand on the terrain the moment the place is built.
				Body->CollisionTraceFlag = CTF_UseComplexAsSimple;
				Body->bDoubleSidedGeometry = true;
				Body->InvalidatePhysicsData();
				Body->CreatePhysicsMeshes();
			}
			Chunk.Mesh = Mesh;
			Built.Add(Chunk);
		}
	}

	UE_LOG(LogMantle, Log, TEXT("MANTLE_TERRAIN built %d x %d samples as %d x %d chunks of %d quads, %d LODs, %lld LOD 0 vertices, collision %s, in %.1f ms"),
		Field.Width, Field.Height, ChunksX, ChunksY, ChunkQuads, LODs, Vertices, Params.bCollision ? TEXT("complex") : TEXT("none"),
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
	Out = MoveTemp(Built);
	return true;
}

FName FMantleTerrainBuilder::LayerParameter(const int32 Layer, const TCHAR* Suffix)
{
	return FName(*FString::Printf(TEXT("Layer%d%s"), Layer, Suffix));
}

UMaterialInterface* FMantleTerrainBuilder::BuildMaterial(const FMantleHeightfield& Field, TConstArrayView<FMantleTerrainLayerLook> Layers,
	UTexture* Weightmap, UTexture* Weightmap2, UObject* Outer, FString* OutWarning)
{
	using namespace MantleTerrainBuilderPrivate;
	namespace Names = MantleTerrainMaterialParameters;

	if (Layers.Num() == 0)
	{
		return nullptr;
	}
	// One layer from something other than an uploaded material (a built-in library material) has
	// nothing to copy into the master: it covers the terrain as it is.
	if (Layers.Num() == 1 && !Weightmap && !IsPlaceMaterial(Layers[0].Material))
	{
		return Layers[0].Material;
	}
	UMaterialInterface* Parent = LoadTerrainMaterial();
	if (!Parent)
	{
		if (!bWarnedTerrainMaterialMissing)
		{
			bWarnedTerrainMaterialMissing = true;
			UE_LOG(LogMantle, Warning, TEXT("MANTLE_TERRAIN M_MantleTerrain is missing (%s); terrain is drawn with its first layer's material only"),
				FMantleTerrainBuilder::MaterialPath);
		}
		if (OutWarning)
		{
			*OutWarning = TEXT("the terrain master material is missing, so only the first layer is drawn");
		}
		return Layers[0].Material;
	}

	UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(Parent, Outer ? Outer : GetTransientPackage());
	if (!Instance)
	{
		return Layers[0].Material;
	}
	const int32 LayerCount = FMath::Min(Layers.Num(), FMantlePlaceLimits::MaxTerrainLayers);
	Instance->SetVectorParameterValue(FName(Names::LayerMask), FLinearColor(
		LayerCount > 0 ? 1.0f : 0.0f, LayerCount > 1 ? 1.0f : 0.0f, LayerCount > 2 ? 1.0f : 0.0f, LayerCount > 3 ? 1.0f : 0.0f));
	// Layers 4 to 7: their own mask, so a layer that does not exist weighs nothing in the one shader.
	Instance->SetVectorParameterValue(FName(Names::LayerMask2), FLinearColor(
		LayerCount > 4 ? 1.0f : 0.0f, LayerCount > 5 ? 1.0f : 0.0f, LayerCount > 6 ? 1.0f : 0.0f, LayerCount > 7 ? 1.0f : 0.0f));
	if (Weightmap)
	{
		Instance->SetTextureParameterValue(FName(Names::Weightmap), Weightmap);
	}
	if (Weightmap2)
	{
		Instance->SetTextureParameterValue(FName(Names::Weightmap2), Weightmap2);
	}

	TArray<FString> Plain;
	for (int32 Layer = 0; Layer < LayerCount; ++Layer)
	{
		const FMantleTerrainLayerLook& Look = Layers[Layer];
		const UMaterialInterface* Source = Look.Material;
		if (!IsPlaceMaterial(Source))
		{
			// Its textures are not parameters the master can take: the layer keeps the master's defaults.
			Plain.Add(FString::FromInt(Layer));
		}
		else
		{
			// The place material's parameter, and the layer's suffix for it on the master.
			struct FCopiedTexture
			{
				const TCHAR* From;
				const TCHAR* To;
			};
			const FCopiedTexture TextureParameters[] = {
				{ MantlePlaceMaterialParameters::BaseColor, Names::BaseColor },
				{ MantlePlaceMaterialParameters::Normal, Names::Normal },
				{ MantlePlaceMaterialParameters::ORM, Names::ORM } };
			for (const FCopiedTexture& Parameter : TextureParameters)
			{
				UTexture* Texture = nullptr;
				if (Source->GetTextureParameterValue(FHashedMaterialParameterInfo(FName(Parameter.From)), Texture) && Texture)
				{
					Instance->SetTextureParameterValue(LayerParameter(Layer, Parameter.To), Texture);
				}
			}
			FLinearColor Factor;
			if (Source->GetVectorParameterValue(FHashedMaterialParameterInfo(FName(MantlePlaceMaterialParameters::BaseColorFactor)), Factor))
			{
				Instance->SetVectorParameterValue(LayerParameter(Layer, Names::BaseColorFactor), Factor);
			}
			float Scalar = 0.0f;
			if (Source->GetScalarParameterValue(FHashedMaterialParameterInfo(FName(MantlePlaceMaterialParameters::MetallicFactor)), Scalar))
			{
				Instance->SetScalarParameterValue(LayerParameter(Layer, Names::MetallicFactor), Scalar);
			}
			if (Source->GetScalarParameterValue(FHashedMaterialParameterInfo(FName(MantlePlaceMaterialParameters::RoughnessFactor)), Scalar))
			{
				Instance->SetScalarParameterValue(LayerParameter(Layer, Names::RoughnessFactor), Scalar);
			}
		}
		// Repeats across the whole terrain: UV0 is 0 to 1 over its extent.
		const double Tiling = Look.TilingCm > 0.0 ? Look.TilingCm : 400.0;
		Instance->SetVectorParameterValue(LayerParameter(Layer, Names::UVScale), FLinearColor(
			static_cast<float>(Field.ExtentX() / Tiling), static_cast<float>(Field.ExtentY() / Tiling), 0.0f, 0.0f));
	}
	if (Plain.Num() > 0 && OutWarning)
	{
		*OutWarning = FString::Printf(TEXT("layer(s) %s use a material that is not an uploaded one, so they are drawn plain"),
			*FString::Join(Plain, TEXT(", ")));
	}
	return Instance;
}

AActor* FMantleTerrainBuilder::Spawn(UWorld* World, const FString& Name, const FMantleHeightfield& Field, UMaterialInterface* Material,
	const bool bCastShadow, const FMantleTerrainBuildParams& Params, FString* OutError)
{
	using namespace MantleTerrainBuilderPrivate;

	if (!World)
	{
		FailTerrain(OutError, TEXT("no world to build the terrain into"));
		return nullptr;
	}
	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* Actor = World->SpawnActor<AActor>(FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
	if (!Actor)
	{
		FailTerrain(OutError, TEXT("could not spawn the terrain actor"));
		return nullptr;
	}
#if WITH_EDITOR
	if (!Name.IsEmpty())
	{
		Actor->SetActorLabel(Name);
	}
#endif
	Actor->Tags.Add(FName(TerrainActorTag));

	FMantleTerrainBuildParams ChunkParams = Params;
	ChunkParams.Outer = Params.Outer ? Params.Outer : Actor;
	TArray<FMantleTerrainChunk> Chunks;
	if (!BuildChunks(Field, ChunkParams, Chunks, OutError))
	{
		Actor->Destroy();
		return nullptr;
	}

	// Movable while the place builder places it, like every other entity; it freezes the terrain to
	// Static afterwards.
	USceneComponent* Root = NewObject<USceneComponent>(Actor, TEXT("Terrain"));
	Actor->SetRootComponent(Root);
	Actor->AddInstanceComponent(Root);
	Root->SetMobility(EComponentMobility::Movable);
	Root->RegisterComponent();
	for (const FMantleTerrainChunk& Chunk : Chunks)
	{
		UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(Actor,
			FName(*FString::Printf(TEXT("Chunk_%d_%d"), Chunk.FirstSample.X, Chunk.FirstSample.Y)));
		Component->SetMobility(EComponentMobility::Movable);
		Component->SetupAttachment(Root);
		Component->SetRelativeLocation(FVector(Chunk.Origin));
		Component->SetStaticMesh(Chunk.Mesh);
		if (Material)
		{
			Component->SetMaterial(0, Material);
		}
		Component->SetCastShadow(bCastShadow);
		Component->SetCollisionProfileName(UCollisionProfile::CustomCollisionProfileName);
		if (Params.bCollision)
		{
			Component->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
			Component->SetCollisionResponseToAllChannels(ECR_Block);
		}
		else
		{
			Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		}
		Actor->AddInstanceComponent(Component);
		Component->RegisterComponent();
	}
	return Actor;
}
