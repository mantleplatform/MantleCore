// Copyright Joshua Eger. All Rights Reserved.
#include "MantleTerrainBuilder.h"

#include "MantleTerrainSurface.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "MantlePlaceLimits.h"
#include "MantlePlaceMaterial.h"
#include "MantleRuntime.h"
#include "MantleRuntimeStaticMeshBuilder.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "MantleTerrainCollision.h"
#include "PhysicsEngine/BodySetup.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleTerrainBuilderPrivate
{
	/** The least a skirt hangs below its edge, so a seam between two LODs never shows a sliver of sky. */
	constexpr float MinSkirtCm = 10.0f;

	/** MI_MantleTerrain while it is loaded, and whether its absence was already reported. Game thread. */
	TWeakObjectPtr<UMaterialInterface> GTerrainMaterial;
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

	/**
	 * The height at sample (X, Y), where X or Y may be one sample past a border of Field that has a neighbor (the neighbor shares its border samples, so the sample just past
	 * Field's right border is the neighbor's sample 1). False when there is no such sample. Along an axis only: a normal never reads a diagonal.
	 */
	bool EdgeAwareHeight(const FMantleHeightfield& Field, const FMantleTerrainEdgeNeighbors* Edges, const int32 X, const int32 Y, float& OutHeight)
	{
		if (X >= 0 && X < Field.Width && Y >= 0 && Y < Field.Height)
		{
			OutHeight = Field.HeightAt(X, Y);
			return true;
		}
		if (!Edges)
		{
			return false;
		}
		const bool bInsideY = Y >= 0 && Y < Field.Height;
		const bool bInsideX = X >= 0 && X < Field.Width;
		if (bInsideY && X < 0 && Edges->Has(0))
		{
			const FMantleHeightfield& Other = *Edges->Field[0];
			const int32 OtherX = Other.Width - 1 + X;
			if (OtherX >= 0 && Y < Other.Height)
			{
				OutHeight = Other.HeightAt(OtherX, Y);
				return true;
			}
		}
		else if (bInsideY && X >= Field.Width && Edges->Has(1))
		{
			const FMantleHeightfield& Other = *Edges->Field[1];
			const int32 OtherX = X - (Field.Width - 1);
			if (OtherX < Other.Width && Y < Other.Height)
			{
				OutHeight = Other.HeightAt(OtherX, Y);
				return true;
			}
		}
		else if (bInsideX && Y < 0 && Edges->Has(2))
		{
			const FMantleHeightfield& Other = *Edges->Field[2];
			const int32 OtherY = Other.Height - 1 + Y;
			if (OtherY >= 0 && X < Other.Width)
			{
				OutHeight = Other.HeightAt(X, OtherY);
				return true;
			}
		}
		else if (bInsideX && Y >= Field.Height && Edges->Has(3))
		{
			const FMantleHeightfield& Other = *Edges->Field[3];
			const int32 OtherY = Y - (Field.Height - 1);
			if (OtherY < Other.Height && X < Other.Width)
			{
				OutHeight = Other.HeightAt(X, OtherY);
				return true;
			}
		}
		return false;
	}

	UMaterialInterface* LoadTerrainMaterial()
	{
		if (UMaterialInterface* Loaded = GTerrainMaterial.Get())
		{
			return Loaded;
		}
		// Loaded again whenever it is gone: nothing holds it between places, so leaving a place's terrain lets the garbage
		// collector unload it, and a one-time lookup then drew every later terrain with its first layer stretched across it.
		GTerrainMaterial = LoadObject<UMaterialInterface>(nullptr, FMantleTerrainBuilder::MaterialPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
		return GTerrainMaterial.Get();
	}

	/** Whether Material is an M_MantlePlace instance (it has the base color texture parameter the layers copy). */
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

FVector3f FMantleTerrainBuilder::SampleNormal(const FMantleHeightfield& Field, const int32 X, const int32 Y, const FMantleTerrainEdgeNeighbors* Edges)
{
	using namespace MantleTerrainBuilderPrivate;
	const float Here = Field.HeightAt(X, Y);
	// A side with no sample past the border (no neighbor there) is one-sided: the sample itself stands in.
	int32 X0 = X - 1;
	int32 X1 = X + 1;
	int32 Y0 = Y - 1;
	int32 Y1 = Y + 1;
	float HeightX0 = Here;
	float HeightX1 = Here;
	float HeightY0 = Here;
	float HeightY1 = Here;
	if (!EdgeAwareHeight(Field, Edges, X0, Y, HeightX0)) { X0 = X; HeightX0 = Here; }
	if (!EdgeAwareHeight(Field, Edges, X1, Y, HeightX1)) { X1 = X; HeightX1 = Here; }
	if (!EdgeAwareHeight(Field, Edges, X, Y0, HeightY0)) { Y0 = Y; HeightY0 = Here; }
	if (!EdgeAwareHeight(Field, Edges, X, Y1, HeightY1)) { Y1 = Y; HeightY1 = Here; }
	const float DzDx = X1 > X0 ? (HeightX1 - HeightX0) / (static_cast<float>(X1 - X0) * Field.SpacingX) : 0.0f;
	const float DzDy = Y1 > Y0 ? (HeightY1 - HeightY0) / (static_cast<float>(Y1 - Y0) * Field.SpacingY) : 0.0f;
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
	const int32 Step, const float SkirtDepth, const bool (&bSkirts)[4], FMeshDescription& Out, const FMantleTerrainHoleGrid* Holes, const FMantleTerrainEdgeNeighbors* Edges)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(MantleTerrain_BuildChunkDescription);
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
		const FVector3f Normal = SampleNormal(Field, X, Y, Edges);
		const FVector3f Tangent = (FVector3f(1.0f, 0.0f, 0.0f) - Normal * Normal.X).GetSafeNormal(UE_SMALL_NUMBER, FVector3f(1.0f, 0.0f, 0.0f));
		Normals[Instance] = Normal;
		Tangents[Instance] = Tangent;
		BinormalSigns[Instance] = 1.0f;
		UVs.Set(Instance, 0, FVector2f(static_cast<float>(X) * InvU, static_cast<float>(Y) * InvV));
		return Instance;
	};

	// A vertex is made when a triangle first needs it, so a hole leaves none behind.
	TArray<FVertexInstanceID> Grid;
	Grid.Init(FVertexInstanceID(INDEX_NONE), NX * NY);
	auto At = [&](const int32 IX, const int32 IY)
	{
		FVertexInstanceID& Instance = Grid[IY * NX + IX];
		if (Instance.GetValue() == INDEX_NONE)
		{
			Instance = AddVertex(Xs[IX], Ys[IY], 0.0f);
		}
		return Instance;
	};
	// The quad (IX, IY) of this LOD covers these quads of the heightfield.
	auto IsHole = [&](const int32 IX, const int32 IY)
	{
		return Holes && Holes->AnyHole(Xs[IX], Ys[IY], Xs[IX + 1], Ys[IY + 1]);
	};

	for (int32 IY = 0; IY + 1 < NY; ++IY)
	{
		for (int32 IX = 0; IX + 1 < NX; ++IX)
		{
			if (IsHole(IX, IY))
			{
				continue;
			}
			// The square is split from (x, y) to (x + 1, y + 1): the height field's split (the collision, FMantleTerrainCollision), the engine landscape's when it draws, and navigation's.
			const FVertexInstanceID Lower[3] = { At(IX, IY), At(IX + 1, IY + 1), At(IX + 1, IY) };
			const FVertexInstanceID Upper[3] = { At(IX, IY), At(IX, IY + 1), At(IX + 1, IY + 1) };
			Out.CreateTriangle(Group, MakeArrayView(Lower));
			Out.CreateTriangle(Group, MakeArrayView(Upper));
		}
	}

	if (!bAnySkirt)
	{
		return;
	}
	// A skirt is a strip from the edge down SkirtDepth, both windings: a gap can be seen from either side.
	// Along a hole there is no edge to hang a skirt from: Side 0 and 1 run along Y, 2 and 3 along X.
	auto AddSkirt = [&](const TArray<FIntPoint>& EdgeSamples, const int32 Side)
	{
		TArray<FVertexInstanceID> Hanging;
		Hanging.Init(FVertexInstanceID(INDEX_NONE), EdgeSamples.Num());
		auto Down = [&](const int32 Segment)
		{
			if (Hanging[Segment].GetValue() == INDEX_NONE)
			{
				Hanging[Segment] = AddVertex(Xs[EdgeSamples[Segment].X], Ys[EdgeSamples[Segment].Y], SkirtDepth);
			}
			return Hanging[Segment];
		};
		for (int32 Segment = 0; Segment + 1 < EdgeSamples.Num(); ++Segment)
		{
			const FIntPoint EdgeQuad = Side == 0 ? FIntPoint(0, Segment) : Side == 1 ? FIntPoint(NX - 2, Segment)
				: Side == 2 ? FIntPoint(Segment, 0) : FIntPoint(Segment, NY - 2);
			if (IsHole(EdgeQuad.X, EdgeQuad.Y))
			{
				continue;
			}
			const FVertexInstanceID A = At(EdgeSamples[Segment].X, EdgeSamples[Segment].Y);
			const FVertexInstanceID B = At(EdgeSamples[Segment + 1].X, EdgeSamples[Segment + 1].Y);
			const FVertexInstanceID DownA = Down(Segment);
			const FVertexInstanceID DownB = Down(Segment + 1);
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
		AddSkirt(SideSamples, Side);
	}
}

bool FMantleTerrainBuilder::IsUsableHeightfield(const FMantleHeightfield& Field)
{
	return !(Field.Width < FMantleHeightfieldReader::MinSize || Field.Height < FMantleHeightfieldReader::MinSize
		|| Field.Width > FMantleHeightfieldReader::MaxSize || Field.Height > FMantleHeightfieldReader::MaxSize
		|| static_cast<int64>(Field.Width) * Field.Height != Field.Heights.Num()
		|| !(Field.SpacingX > 0.0f) || !(Field.SpacingY > 0.0f) || !FMath::IsFinite(Field.SpacingX) || !FMath::IsFinite(Field.SpacingY)
		|| !FMath::IsFinite(Field.HeightScale) || !FMath::IsFinite(Field.HeightOffset));
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
	if (!IsUsableHeightfield(Field))
	{
		return FailTerrain(OutError, FString::Printf(TEXT("the heightfield is not usable (%d x %d, %d samples, spacing %g x %g)"),
			Field.Width, Field.Height, Field.Heights.Num(), Field.SpacingX, Field.SpacingY));
	}

	const double StartSeconds = FPlatformTime::Seconds();
	const int32 ChunkQuads = FMath::Max(Params.ChunkQuads, 1);
	const int32 LODs = Params.bRenderLODs ? LODCount(ChunkQuads, FMath::Max(Params.MaxLODs, 1)) : 1;
	const int32 ChunksX = ChunkCount(Field.Width, ChunkQuads);
	const int32 ChunksY = ChunkCount(Field.Height, ChunkQuads);

	TArray<FMantleTerrainChunk> Built;
	Built.Reserve(ChunksX * ChunksY);
	int64 Vertices = 0;
	for (int32 ChunkY = 0; ChunkY < ChunksY; ++ChunkY)
	{
		for (int32 ChunkX = 0; ChunkX < ChunksX; ++ChunkX)
		{
			FMantleTerrainChunk Chunk;
			bool bEmpty = false;
			if (!BuildOneChunk(Field, Params, ChunkX, ChunkY, Vertices, Chunk, bEmpty, OutError))
			{
				return false;
			}
			if (!bEmpty)
			{
				Built.Add(Chunk);
			}
		}
	}


	UE_LOG(LogMantle, Log, TEXT("MANTLE_TERRAIN built %d x %d samples as %d x %d chunks of %d quads, %d LODs, %lld LOD 0 vertices, collision %s, in %.1f ms"),
		Field.Width, Field.Height, ChunksX, ChunksY, ChunkQuads, LODs, Vertices, Params.bCollision ? TEXT("height field") : TEXT("none"),
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
	Out = MoveTemp(Built);
	return true;
}

void FMantleTerrainBuilder::PrepareChunk(const FMantleHeightfield& Field, const FMantleTerrainBuildParams& Params, const int32 ChunkX, const int32 ChunkY,
	const EMantleTerrainChunkDetail Detail, FMantleTerrainPreparedChunk& Out)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(MantleTerrain_PrepareChunk);
	Out = FMantleTerrainPreparedChunk();
	Out.Detail = Detail;
	Out.ChunkX = ChunkX;
	Out.ChunkY = ChunkY;
	const int32 ChunkQuads = FMath::Max(Params.ChunkQuads, 1);
	// A far chunk and the full chunk it stands in for share the LOD count of a full render build, whatever this build's own LODs are: the far chunk's step and its skirt depth
	// are the full chunk's, so the two meet.
	const int32 LODs = (Params.bRenderLODs || Detail == EMantleTerrainChunkDetail::Far) ? LODCount(ChunkQuads, FMath::Max(Params.MaxLODs, 1)) : 1;

	ChunkSpan(Field.Width, ChunkQuads, ChunkX, Out.FirstSample.X, Out.LastSample.X);
	ChunkSpan(Field.Height, ChunkQuads, ChunkY, Out.FirstSample.Y, Out.LastSample.Y);
	Out.Origin = FVector3f(static_cast<float>(Out.FirstSample.X) * Field.SpacingX, static_cast<float>(Out.FirstSample.Y) * Field.SpacingY, 0.0f);

	// Skirts only where another chunk meets this one (another chunk of this terrain, or a chunk of a neighbor terrain across a tile seam): left, right, bottom, top.
	const FMantleTerrainEdgeNeighbors* Edges = Params.Neighbors.Get();
	const bool bSkirts[4] = {
		Out.FirstSample.X > 0 || (Edges && Edges->Has(0)), Out.LastSample.X < Field.Width - 1 || (Edges && Edges->Has(1)),
		Out.FirstSample.Y > 0 || (Edges && Edges->Has(2)), Out.LastSample.Y < Field.Height - 1 || (Edges && Edges->Has(3)) };
	float SkirtDepth = 0.0f;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE_STR("MantleTerrain_SkirtDepth");
		if (bSkirts[0]) { SkirtDepth = FMath::Max(SkirtDepth, EdgeSkirtDepth(Field, false, Out.FirstSample.X, Out.FirstSample.Y, Out.LastSample.Y, LODs)); }
		if (bSkirts[1]) { SkirtDepth = FMath::Max(SkirtDepth, EdgeSkirtDepth(Field, false, Out.LastSample.X, Out.FirstSample.Y, Out.LastSample.Y, LODs)); }
		if (bSkirts[2]) { SkirtDepth = FMath::Max(SkirtDepth, EdgeSkirtDepth(Field, true, Out.FirstSample.Y, Out.FirstSample.X, Out.LastSample.X, LODs)); }
		if (bSkirts[3]) { SkirtDepth = FMath::Max(SkirtDepth, EdgeSkirtDepth(Field, true, Out.LastSample.Y, Out.FirstSample.X, Out.LastSample.X, LODs)); }
	}

	if (Detail == EMantleTerrainChunkDetail::Far)
	{
		// The coarsest LOD that still has a quad (a hole takes a coarse quad whenever it takes any of the fine ones it covers): where the chunk's own LOD list ends.
		for (int32 Lod = LODs - 1; Lod >= 0; --Lod)
		{
			FMeshDescription Description;
			BuildChunkDescription(Field, Out.FirstSample, Out.LastSample, 1 << Lod, SkirtDepth, bSkirts, Description, Params.Holes.Get(), Edges);
			if (Description.Triangles().Num() > 0)
			{
				Out.Vertices = Description.Vertices().Num();
				Out.Descriptions.Add(MoveTemp(Description));
				return;
			}
		}
		Out.bEmpty = true;
		return;
	}

	Out.Descriptions.Reserve(LODs);
	for (int32 Lod = 0; Lod < LODs; ++Lod)
	{
		FMeshDescription Description;
		BuildChunkDescription(Field, Out.FirstSample, Out.LastSample, 1 << Lod, SkirtDepth, bSkirts, Description, Params.Holes.Get(), Edges);
		// A coarser LOD that has lost all its quads to a hole ends the chunk's LODs there; a chunk with none at LOD 0 is all hole.
		if (Description.Triangles().Num() == 0)
		{
			break;
		}
		Out.Descriptions.Add(MoveTemp(Description));
	}
	if (Out.Descriptions.Num() == 0)
	{
		Out.bEmpty = true;
		return;
	}
	Out.Vertices = Out.Descriptions[0].Vertices().Num();
}

bool FMantleTerrainBuilder::FinishChunk(const FMantleTerrainBuildParams& Params, FMantleTerrainPreparedChunk&& Prepared,
	int64& InOutVertices, FMantleTerrainChunk& OutChunk, bool& bOutEmpty, FString* OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(MantleTerrain_FinishChunk);
	using namespace MantleTerrainBuilderPrivate;

	bOutEmpty = false;
	if (Prepared.bEmpty || Prepared.Descriptions.Num() == 0)
	{
		bOutEmpty = true;
		return true;
	}
	const bool bFar = Prepared.Detail == EMantleTerrainChunkDetail::Far;
	const int32 ChunkX = Prepared.ChunkX;
	const int32 ChunkY = Prepared.ChunkY;
	const int32 ChunkLODs = Prepared.Descriptions.Num();
	// A far chunk is its one LOD at every distance: the screen size of the last LOD a full chunk has.
	const int32 FarLod = FMath::Max(LODCount(FMath::Max(Params.ChunkQuads, 1), FMath::Max(Params.MaxLODs, 1)) - 1, 0);
	TArray<const FMeshDescription*> DescriptionPointers;
	DescriptionPointers.Reserve(ChunkLODs);
	for (const FMeshDescription& Description : Prepared.Descriptions)
	{
		DescriptionPointers.Add(&Description);
	}
	InOutVertices += Prepared.Vertices;
	UObject* MeshOuter = Params.Outer ? Params.Outer : GetTransientPackage();

	FMantleTerrainChunk Chunk;
	Chunk.FirstSample = Prepared.FirstSample;
	Chunk.LastSample = Prepared.LastSample;
	Chunk.Origin = Prepared.Origin;

	const FName MeshName = MakeUniqueObjectName(MeshOuter, UStaticMesh::StaticClass(),
		FName(*FString::Printf(TEXT("%s_%d_%d"), bFar ? TEXT("MantleTerrainFar") : TEXT("MantleTerrainChunk"), ChunkX, ChunkY)));
	UStaticMesh* Mesh = NewObject<UStaticMesh>(MeshOuter, MeshName, RF_Transient);
	// UVChannelData must be initialized (UStaticMesh::GetUVChannelData ensures it); see the static mesh builder.
	FStaticMaterial& Slot = Mesh->GetStaticMaterials().Add_GetRef(FStaticMaterial(nullptr, FName(MaterialSlotName)));
	Slot.UVChannelData = FMeshUVChannelInfo(1.0f);
	// A picture only: the ground's collision is the height field (FMantleTerrainCollision), so no CPU copy of the triangles is kept and no body is made.
	Mesh->bAllowCPUAccess = false;

	UStaticMesh::FBuildMeshDescriptionsParams BuildParams;
	BuildParams.bMarkPackageDirty = false;
	BuildParams.bBuildSimpleCollision = false;
	BuildParams.bCommitMeshDescription = false;
	BuildParams.bFastBuild = true;
	BuildParams.bAllowCpuAccess = false;
	// UV0 spans the whole terrain and each layer repeats hundreds of times over it: half floats
	// would step visibly, so every LOD keeps full-precision UVs.
	BuildParams.PerLODOverrides.SetNum(ChunkLODs);
	for (UStaticMesh::FBuildMeshDescriptionsLODParams& LODParams : BuildParams.PerLODOverrides)
	{
		LODParams.bUseFullPrecisionUVs = true;
	}
	// The engine's build always ends in InitResources, which asserts on a dedicated server: there
	// the same mesh is built without render resources.
	bool bBuilt = false;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(MantleTerrain_BuildFromMeshDescriptions);
		bBuilt = FMantleRuntimeStaticMeshBuilder::ShouldInitRenderResources(Params.bInitRenderResources)
			? Mesh->BuildFromMeshDescriptions(DescriptionPointers, BuildParams)
			: BuildTerrainMeshCpuOnly(*Mesh, DescriptionPointers, BuildParams);
	}
	if (!bBuilt)
	{
		return FailTerrain(OutError, FString::Printf(TEXT("the engine could not build terrain chunk (%d, %d)"), ChunkX, ChunkY));
	}
	FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
	if (!RenderData || RenderData->LODResources.Num() != ChunkLODs)
	{
		return FailTerrain(OutError, FString::Printf(TEXT("the engine built %d LODs of terrain chunk (%d, %d), not %d"),
			RenderData ? RenderData->LODResources.Num() : 0, ChunkX, ChunkY, ChunkLODs));
	}
	for (int32 Lod = 0; Lod < ChunkLODs; ++Lod)
	{
		// Chosen by screen size, which for chunks of one size is by distance (LODScreenSize).
		RenderData->ScreenSize[Lod].Default = LODScreenSize(bFar ? FarLod : Lod);
		// One slot: bound explicitly, as FMantleRuntimeStaticMeshBuilder does, rather than trusting
		// the fast build's lookup of the slot name.
		for (FStaticMeshSection& Section : RenderData->LODResources[Lod].Sections)
		{
			Section.MaterialIndex = 0;
		}
	}

	Chunk.Mesh = Mesh;
	OutChunk = Chunk;
	return true;
}

bool FMantleTerrainBuilder::BuildOneChunk(const FMantleHeightfield& Field, const FMantleTerrainBuildParams& Params, const int32 ChunkX, const int32 ChunkY,
	int64& InOutVertices, FMantleTerrainChunk& OutChunk, bool& bOutEmpty, FString* OutError)
{
	FMantleTerrainPreparedChunk Prepared;
	PrepareChunk(Field, Params, ChunkX, ChunkY, EMantleTerrainChunkDetail::Full, Prepared);
	return FinishChunk(Params, MoveTemp(Prepared), InOutVertices, OutChunk, bOutEmpty, OutError);
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

bool FMantleTerrainAdjacency::BorderMeets(const FMantleTerrainPlacement& A, const FMantleTerrainPlacement& B, const bool bAlongX)
{
	if (!A.Field.IsValid() || !B.Field.IsValid())
	{
		return false;
	}
	const FMantleHeightfield& FieldA = *A.Field;
	const FMantleHeightfield& FieldB = *B.Field;
	// bAlongX: A's last column (x = Width - 1) against B's first (x = 0), samples counted along Y; otherwise A's last row against B's first, along X.
	const int32 Count = bAlongX ? FieldA.Height : FieldA.Width;
	if (Count != (bAlongX ? FieldB.Height : FieldB.Width) || Count < 2)
	{
		return false;
	}
	// The two must also agree in their own frames, not only in the world: the builder reads the neighbor's heights as if they were this terrain's (local cm) and divides by this terrain's spacing.
	// So: the same turn and scale, no step in height between their origins (the same local height is then the same world height), and the same spacing across the border.
	const FQuat RotationA = A.World.GetRotation();
	const FQuat RotationB = B.World.GetRotation();
	if (!(RotationA.Equals(RotationB, 1.0e-4) || RotationA.Equals(-RotationB, 1.0e-4)) || !A.World.GetScale3D().Equals(B.World.GetScale3D(), 1.0e-4))
	{
		return false;
	}
	if (FMath::Abs(A.World.InverseTransformPosition(B.World.GetLocation()).Z) > ToleranceCm)
	{
		return false;
	}
	if (!FMath::IsNearlyEqual(bAlongX ? FieldA.SpacingX : FieldA.SpacingY, bAlongX ? FieldB.SpacingX : FieldB.SpacingY, 0.01f))
	{
		return false;
	}
	const auto BorderPoint = [bAlongX](const FMantleTerrainPlacement& Terrain, const bool bLastBorder, const int32 Along)
	{
		const FMantleHeightfield& Field = *Terrain.Field;
		const int32 X = bAlongX ? (bLastBorder ? Field.Width - 1 : 0) : Along;
		const int32 Y = bAlongX ? Along : (bLastBorder ? Field.Height - 1 : 0);
		return Terrain.World.TransformPosition(FVector(Field.PositionAt(X, Y)));
	};
	const double ToleranceSquared = ToleranceCm * ToleranceCm;
	// The ends first (almost every pair that does not touch fails there), then every sample.
	const int32 Probes[2] = { 0, Count - 1 };
	for (const int32 Along : Probes)
	{
		if (FVector::DistSquared(BorderPoint(A, true, Along), BorderPoint(B, false, Along)) > ToleranceSquared)
		{
			return false;
		}
	}
	for (int32 Along = 1; Along + 1 < Count; ++Along)
	{
		if (FVector::DistSquared(BorderPoint(A, true, Along), BorderPoint(B, false, Along)) > ToleranceSquared)
		{
			return false;
		}
	}
	return true;
}

void FMantleTerrainAdjacency::Find(TConstArrayView<FMantleTerrainPlacement> Terrains, TArray<FMantleTerrainEdgeNeighbors>& Out)
{
	Out.Reset();
	Out.SetNum(Terrains.Num());
	TArray<bool> Usable;
	Usable.Init(false, Terrains.Num());
	for (int32 Index = 0; Index < Terrains.Num(); ++Index)
	{
		Usable[Index] = Terrains[Index].Field.IsValid() && FMantleTerrainBuilder::IsUsableHeightfield(*Terrains[Index].Field);
	}
	for (int32 A = 0; A < Terrains.Num(); ++A)
	{
		for (int32 B = 0; B < Terrains.Num(); ++B)
		{
			if (A == B || !Usable[A] || !Usable[B])
			{
				continue;
			}
			// B beyond A's right border, or beyond its top border.
			if (!Out[A].Has(1) && !Out[B].Has(0) && BorderMeets(Terrains[A], Terrains[B], true))
			{
				Out[A].Field[1] = Terrains[B].Field;
				Out[B].Field[0] = Terrains[A].Field;
			}
			if (!Out[A].Has(3) && !Out[B].Has(2) && BorderMeets(Terrains[A], Terrains[B], false))
			{
				Out[A].Field[3] = Terrains[B].Field;
				Out[B].Field[2] = Terrains[A].Field;
			}
		}
	}
}

TSharedPtr<FMantleTerrainSpawn> FMantleTerrainSpawn::Begin(UWorld* World, const FString& Name, const FMantleHeightfield& Field,
	UMaterialInterface* Material, const bool bCastShadow, const FMantleTerrainBuildParams& Params, FString* OutError, const FTransform* StaticWorld, const FTransform* MovableWorld)
{
	using namespace MantleTerrainBuilderPrivate;

	if (!World)
	{
		FailTerrain(OutError, TEXT("no world to build the terrain into"));
		return nullptr;
	}
	if (!IsInGameThread())
	{
		FailTerrain(OutError, TEXT("terrain meshes are built on the game thread"));
		return nullptr;
	}
	if (!FMantleTerrainBuilder::IsUsableHeightfield(Field))
	{
		FailTerrain(OutError, FString::Printf(TEXT("the heightfield is not usable (%d x %d, %d samples, spacing %g x %g)"),
			Field.Width, Field.Height, Field.Heights.Num(), Field.SpacingX, Field.SpacingY));
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
	Actor->Tags.Add(FName(FMantleTerrainBuilder::TerrainActorTag));

	// Where it ends, Static, when the caller knows (the place builder's freeze plan): the root stands in place before it registers, and each chunk is made
	// Static under it, so no collision is ever made for a Movable terrain and moved. Otherwise Movable, at MovableWorld when the caller knows where it ends (so its chunks'
	// collision is not made at the origin and moved), else at the origin for the caller to place.
	USceneComponent* Root = NewObject<USceneComponent>(Actor, TEXT("Terrain"));
	Actor->SetRootComponent(Root);
	Actor->AddInstanceComponent(Root);
	if (StaticWorld)
	{
		Root->SetMobility(EComponentMobility::Static);
		Root->SetRelativeTransform(*StaticWorld);
	}
	else
	{
		Root->SetMobility(EComponentMobility::Movable);
		if (MovableWorld)
		{
			Root->SetRelativeTransform(*MovableWorld);
		}
	}
	Root->RegisterComponent();

	TSharedPtr<FMantleTerrainSpawn> Spawn = MakeShareable(new FMantleTerrainSpawn());
	Spawn->Actor = Actor;
	Spawn->Root = Root;
	Spawn->Material.Reset(Material);
	Spawn->Field = &Field;
	Spawn->FieldShared = Params.FieldOwner.IsValid() ? Params.FieldOwner : TSharedPtr<const FMantleHeightfield>(MakeShared<FMantleHeightfield>(Field));
	Spawn->Params = Params;
	Spawn->Params.Outer = Params.Outer ? Params.Outer : Actor;
	Spawn->bCastShadow = bCastShadow;
	Spawn->bStatic = StaticWorld != nullptr;
	const int32 ChunkQuads = FMath::Max(Params.ChunkQuads, 1);
	Spawn->ChunksX = FMantleTerrainBuilder::ChunkCount(Field.Width, ChunkQuads);
	Spawn->ChunksY = FMantleTerrainBuilder::ChunkCount(Field.Height, ChunkQuads);
	if (Params.bFarOnly)
	{
		Spawn->FarComponents.SetNum(Spawn->ChunksX * Spawn->ChunksY);
	}
	return Spawn;
}

FMantleTerrainSpawn::~FMantleTerrainSpawn()
{
	// Dropped before it was finished (or after a chunk failed): the half-built terrain goes with it.
	if (AActor* Held = Actor.Get())
	{
		Held->Destroy();
	}
}

bool FMantleTerrainSpawn::Step()
{
	AActor* Built = Actor.Get();
	USceneComponent* RootComponent = Root.Get();
	if (HasFailed() || !Built || !RootComponent)
	{
		return true;
	}
	const int32 Total = ChunksX * ChunksY;
	if (Next >= Total)
	{
		return true;
	}

	const double StartSeconds = FPlatformTime::Seconds();
	const int32 ChunkQuads = FMath::Max(Params.ChunkQuads, 1);
	// A far-only spawn and a spawn with no chunk meshes (a server's) are cheap per chunk: as many as fit in the budget (at least one). A full spawn builds one chunk a step.
	const bool bBudgeted = Params.bFarOnly || !Params.bChunkMeshes;
	while (Next < Total)
	{
		const int32 ChunkX = Next % ChunksX;
		const int32 ChunkY = Next / ChunksX;
		if (Params.bChunkMeshes)
		{
			FMantleTerrainChunk Chunk;
			bool bEmpty = false;
			FString ChunkError;
			bool bBuilt = false;
			if (Params.bFarOnly)
			{
				// The far ground: each chunk its lowest level of detail alone.
				FMantleTerrainPreparedChunk Prepared;
				FMantleTerrainBuilder::PrepareChunk(*Field, Params, ChunkX, ChunkY, EMantleTerrainChunkDetail::Far, Prepared);
				bBuilt = FMantleTerrainBuilder::FinishChunk(Params, MoveTemp(Prepared), Vertices, Chunk, bEmpty, &ChunkError);
			}
			else
			{
				bBuilt = FMantleTerrainBuilder::BuildOneChunk(*Field, Params, ChunkX, ChunkY, Vertices, Chunk, bEmpty, &ChunkError);
			}
			if (!bBuilt)
			{
				Error = ChunkError.IsEmpty() ? FString(Params.bFarOnly ? TEXT("a far terrain chunk failed") : TEXT("a terrain chunk failed")) : ChunkError;
				Built->Destroy();
				Actor.Reset();
				return true;
			}
			if (!bEmpty)
			{
				UStaticMeshComponent* Component = FMantleTerrainBuilder::AddChunkComponent(Built, RootComponent, Chunk, Material.Get(), bStatic, bCastShadow, /*bFar*/ Params.bFarOnly);
				if (Params.bFarOnly)
				{
					FarComponents[Next] = Component;
				}
			}
		}
		if (Params.bCollision)
		{
			if (const UMantleTerrainCollisionComponent* Block = FMantleTerrainBuilder::AddCollisionBlock(Built, RootComponent, FieldShared, Params.Holes, ChunkQuads, ChunkX, ChunkY, bStatic))
			{
				++Blocks;
				ShapeBytes += Block->GetShapeBytes();
#if !UE_BUILD_SHIPPING
				static const bool bMeasureRounding = FParse::Param(FCommandLine::Get(), TEXT("MantleTestTerrainRounding"));
				if (bMeasureRounding)
				{
					double Bound = 0.0;
					bool bAbove = false;
					const double Largest = Block->MeasureRoundingCm(Bound, bAbove);
					RoundingLargestCm = FMath::Max(RoundingLargestCm, Largest);
					RoundingBoundCm = FMath::Max(RoundingBoundCm, Bound);
					RoundingBroken += (bAbove || Largest > Bound + 1.0e-4) ? 1 : 0;
				}
#endif
			}
		}
		++Next;
		if (!bBudgeted || FPlatformTime::Seconds() - StartSeconds >= FarBudgetSeconds)
		{
			break;
		}
	}
	BuildSeconds += FPlatformTime::Seconds() - StartSeconds;
	if (Next < Total)
	{
		return false;
	}
	UE_LOG(LogMantle, Log, TEXT("MANTLE_TERRAIN spawned %d x %d samples as %d x %d %s of %d quads, %lld vertices, collision %s (%d blocks), in %.1f ms"),
		Field->Width, Field->Height, ChunksX, ChunksY, !Params.bChunkMeshes ? TEXT("collision blocks and no chunks") : Params.bFarOnly ? TEXT("far chunks") : TEXT("chunks"), ChunkQuads, Vertices,
		Params.bCollision ? TEXT("height field") : TEXT("none"), Blocks, BuildSeconds * 1000.0);
	// What this terrain holds on this machine, part by part (the cost of the ground: the heights, the shapes made from them, the hole grid).
	UE_LOG(LogMantle, Log, TEXT("MANTLE_TERRAIN_BYTES terrain=%s samples=%d heights=%lld shapes=%lld hole_grid=%lld blocks=%d"), *Built->GetName(), Field->Width * Field->Height,
		Field->GetAllocatedBytes(), ShapeBytes, Params.Holes.IsValid() ? static_cast<int64>(Params.Holes->Quad.GetAllocatedSize()) : static_cast<int64>(0), Blocks);
#if !UE_BUILD_SHIPPING
	if (FParse::Param(FCommandLine::Get(), TEXT("MantleTestTerrainRounding")))
	{
		UE_LOG(LogMantle, Log, TEXT("MANTLE_TERRAIN_ROUNDING terrain=%s blocks=%d largest_cm=%.5f largest_block_bound_cm=%.5f blocks_over_bound_or_above_file=%d"), *Built->GetName(), Blocks, RoundingLargestCm,
			RoundingBoundCm, RoundingBroken);
	}
#endif
	return true;
}

UStaticMeshComponent* FMantleTerrainBuilder::AddChunkComponent(AActor* Owner, USceneComponent* Root, const FMantleTerrainChunk& Chunk, UMaterialInterface* Material,
	const bool bStatic, const bool bCastShadow, const bool bFar, const bool bRegister)
{
	// A chunk that stood before and was taken down leaves its component's name in use until the garbage collector has it.
	FName ComponentName(*FString::Printf(TEXT("%s_%d_%d"), bFar ? TEXT("Far") : TEXT("Chunk"), Chunk.FirstSample.X, Chunk.FirstSample.Y));
	if (StaticFindObjectFast(UStaticMeshComponent::StaticClass(), Owner, ComponentName))
	{
		ComponentName = MakeUniqueObjectName(Owner, UStaticMeshComponent::StaticClass(), ComponentName);
	}
	UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(Owner, ComponentName);
	Component->SetMobility(bStatic ? EComponentMobility::Static : EComponentMobility::Movable);
	Component->SetupAttachment(Root);
	Component->SetRelativeLocation(FVector(Chunk.Origin));
	Component->SetStaticMesh(Chunk.Mesh);
	if (Material)
	{
		Component->SetMaterial(0, Material);
	}
	Component->SetCastShadow(bCastShadow && !bFar);
	// A picture only: the ground's collision is the height-field blocks (AddCollisionBlock).
	Component->SetCollisionProfileName(UCollisionProfile::CustomCollisionProfileName);
	Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	if (bFar)
	{
		// The far ground has no overlap events and no part in distance-field lighting.
		Component->SetGenerateOverlapEvents(false);
		Component->bAffectDistanceFieldLighting = false;
	}
	Owner->AddInstanceComponent(Component);
	if (bRegister)
	{
		Component->RegisterComponent();
	}
	return Component;
}

UMantleTerrainCollisionComponent* FMantleTerrainBuilder::AddCollisionBlock(AActor* Owner, USceneComponent* Root, const TSharedPtr<const FMantleHeightfield>& Field,
	const TSharedPtr<const FMantleTerrainHoleGrid>& Holes, const int32 ChunkQuads, const int32 ChunkX, const int32 ChunkY, const bool bStatic)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(MantleTerrain_AddCollisionBlock);
	if (!Owner || !Root || !Field.IsValid())
	{
		return nullptr;
	}
	FIntPoint First;
	FIntPoint Last;
	ChunkSpan(Field->Width, ChunkQuads, ChunkX, First.X, Last.X);
	ChunkSpan(Field->Height, ChunkQuads, ChunkY, First.Y, Last.Y);
	if (Last.X <= First.X || Last.Y <= First.Y || FMantleTerrainCollision::BlockIsAllHole(First, Last, Holes.Get()))
	{
		return nullptr;
	}
	UMantleTerrainCollisionComponent* Block = NewObject<UMantleTerrainCollisionComponent>(Owner, FName(*FString::Printf(TEXT("Collision_%d_%d"), First.X, First.Y)));
	Block->Init(Field, First, Last, Holes);
	if (!Block->HasShape())
	{
		Block->DestroyComponent();
		return nullptr;
	}
	Block->SetMobility(bStatic ? EComponentMobility::Static : EComponentMobility::Movable);
	Block->SetupAttachment(Root);
	Block->SetRelativeLocation(Block->GetRelativeOrigin());
	Owner->AddInstanceComponent(Block);
	Block->RegisterComponent();
	return Block;
}

AActor* FMantleTerrainSpawn::TakeActor()
{
	AActor* Built = Actor.Get();
	Actor.Reset();
	return Built;
}

AActor* FMantleTerrainBuilder::Spawn(UWorld* World, const FString& Name, const FMantleHeightfield& Field, UMaterialInterface* Material,
	const bool bCastShadow, const FMantleTerrainBuildParams& Params, FString* OutError)
{
	// The whole terrain in this call: the staged spawn, stepped to its end.
	const TSharedPtr<FMantleTerrainSpawn> Staged = FMantleTerrainSpawn::Begin(World, Name, Field, Material, bCastShadow, Params, OutError);
	if (!Staged)
	{
		return nullptr;
	}
	while (!Staged->Step())
	{
	}
	if (Staged->HasFailed())
	{
		if (OutError)
		{
			*OutError = Staged->GetError();
		}
		return nullptr;
	}
	return Staged->TakeActor();
}
