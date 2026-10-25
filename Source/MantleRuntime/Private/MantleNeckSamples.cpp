// Copyright Joshua Eger. All Rights Reserved.
#include "MantleNeckSamples.h"

#include "MantleGarmentGeometry.h"
#include "MantleRuntime.h"
#include "Async/ParallelFor.h"
#include "Engine/SkeletalMesh.h"
#include "Rendering/MultiSizeIndexContainer.h"
#include "Rendering/PositionVertexBuffer.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/StaticMeshVertexBuffer.h"

int32 FMantleFaceTopology::FindReferenceLOD(const USkeletalMesh* Face)
{
	const FSkeletalMeshRenderData* RenderData = Face ? Face->GetResourceForRendering() : nullptr;
	if (!RenderData)
	{
		return INDEX_NONE;
	}
	for (int32 LODIndex = 0; LODIndex < RenderData->LODRenderData.Num(); ++LODIndex)
	{
		if (RenderData->LODRenderData[LODIndex].GetTotalFaces() == ReferenceTriangles)
		{
			return LODIndex;
		}
	}
	return INDEX_NONE;
}

const FMantleSkinnedMeshLOD* FMantleFaceTopology::FitLOD(const FMantleSkinnedMesh& Face)
{
	for (const FMantleSkinnedMeshLOD& LOD : Face.LODs)
	{
		if (IsReference(LOD))
		{
			return &LOD;
		}
	}
	return Face.LODs.Num() > 0 ? &Face.LODs[0] : nullptr;
}

bool FMantleFaceTopology::ReadFace(const USkeletalMesh* Face, FMantleSkinnedMeshLOD& Out, int32* OutLOD, FString* OutError)
{
	Out = FMantleSkinnedMeshLOD();
	if (OutLOD)
	{
		*OutLOD = INDEX_NONE;
	}
	auto Refuse = [OutError](const FString& Error)
	{
		if (OutError)
		{
			*OutError = Error;
		}
		return false;
	};
	const FSkeletalMeshRenderData* RenderData = Face ? Face->GetResourceForRendering() : nullptr;
	if (!RenderData || RenderData->LODRenderData.Num() == 0)
	{
		return Refuse(Face ? FString::Printf(TEXT("%s has no render data"), *Face->GetName()) : FString(TEXT("no face mesh")));
	}
	const int32 ReferenceLOD = FindReferenceLOD(Face);
	const int32 LODIndex = ReferenceLOD == INDEX_NONE ? 0 : ReferenceLOD;
	const FSkeletalMeshLODRenderData& LOD = RenderData->LODRenderData[LODIndex];
	const FPositionVertexBuffer& PositionBuffer = LOD.StaticVertexBuffers.PositionVertexBuffer;
	const FStaticMeshVertexBuffer& VertexBuffer = LOD.StaticVertexBuffers.StaticMeshVertexBuffer;
	const uint32 N = PositionBuffer.GetNumVertices();
	if (N == 0 || PositionBuffer.GetVertexData() == nullptr || VertexBuffer.GetNumVertices() != N || VertexBuffer.GetTangentData() == nullptr)
	{
		return Refuse(FString::Printf(TEXT("%s LOD%d has no CPU position or tangent data"), *Face->GetName(), LODIndex));
	}
	if (!LOD.MultiSizeIndexContainer.IsIndexBufferValid())
	{
		return Refuse(FString::Printf(TEXT("%s LOD%d has no index buffer"), *Face->GetName(), LODIndex));
	}
	LOD.MultiSizeIndexContainer.GetIndexBuffer(Out.Indices);
	if (Out.Indices.Num() == 0 || !MantleGarmentGeometry::IndicesInRange(Out.Indices, static_cast<int32>(N)))
	{
		Out = FMantleSkinnedMeshLOD();
		return Refuse(FString::Printf(TEXT("%s LOD%d has no usable CPU index data"), *Face->GetName(), LODIndex));
	}
	Out.Positions.SetNumUninitialized(N);
	Out.Normals.SetNumUninitialized(N);
	for (uint32 Vertex = 0; Vertex < N; ++Vertex)
	{
		Out.Positions[Vertex] = PositionBuffer.VertexPosition(Vertex);
		const FVector4f TangentZ = VertexBuffer.VertexTangentZ(Vertex);
		Out.Normals[Vertex] = FVector3f(TangentZ.X, TangentZ.Y, TangentZ.Z);
	}
	Out.Sections.Add({ 0, 0, Out.Indices.Num() });
	if (OutLOD)
	{
		*OutLOD = LODIndex;
	}
	return true;
}

void FMantleNeckSamples::Ray(const int32 Index, const FVector2f& Centre, const float BaseZ, FVector3f& OutOrigin, FVector3f& OutDir)
{
	const int32 Height = Index / (NumElevations * NumAzimuths);
	const int32 Elevation = (Index / NumAzimuths) % NumElevations;
	const int32 Azimuth = Index % NumAzimuths;
	OutOrigin = FVector3f(Centre.X, Centre.Y, BaseZ + HeightCm(Height));
	const float El = FMath::DegreesToRadians(ElevationDeg(Elevation));
	const float Az = 2.0f * UE_PI * static_cast<float>(Azimuth) / static_cast<float>(NumAzimuths);
	OutDir = FVector3f(FMath::Cos(Az) * FMath::Cos(El), FMath::Sin(Az) * FMath::Cos(El), FMath::Sin(El));
}

float FMantleNeckSamples::NeckBaseZ(const TConstArrayView<FVector3f> Body)
{
	float BaseZ = -MAX_flt;
	for (const FVector3f& P : Body)
	{
		if (P.X * P.X + P.Y * P.Y <= ColumnRadiusCm * ColumnRadiusCm)
		{
			BaseZ = FMath::Max(BaseZ, P.Z);
		}
	}
	return BaseZ;
}

bool FMantleNeckSamples::Cast(const FMantleSkinnedMeshLOD& Face, const TConstArrayView<FVector3f> Body, FRaySet& Out)
{
	const double Start = MantleGarmentGeometry::NowMs();
	const int32 N = NumRays();
	Out = FRaySet();
	Out.Points.SetNumZeroed(N);
	Out.Hit.Init(0, N);
	if (Face.NumTriangles() == 0 || Body.Num() == 0 || !MantleGarmentGeometry::IndicesInRange(Face.Indices, Face.NumVertices()))
	{
		return false;
	}
	// The neck base: the body's top within the column (the body mesh ends there).
	Out.BaseZ = NeckBaseZ(Body);
	if (Out.BaseZ == -MAX_flt)
	{
		return false;
	}
	// The axis: the centre of the face's neck column just over the base.
	FVector2f Sum = FVector2f::ZeroVector;
	for (const FVector3f& P : Face.Positions)
	{
		if (P.Z >= Out.BaseZ && P.Z < Out.BaseZ + ColumnHeightCm && P.X * P.X + P.Y * P.Y <= ColumnRadiusCm * ColumnRadiusCm)
		{
			Sum += FVector2f(P.X, P.Y);
			++Out.ColumnVerts;
		}
	}
	Out.Centre = Out.ColumnVerts > 0 ? Sum / static_cast<float>(Out.ColumnVerts) : FVector2f::ZeroVector;
	const int32 NumTriangles = Face.NumTriangles();
	ParallelFor(N, [&Face, &Out, NumTriangles](const int32 Index)
	{
		FVector3f Origin;
		FVector3f Dir;
		Ray(Index, Out.Centre, Out.BaseZ, Origin, Dir);
		float Best = ReachCm;
		bool bHit = false;
		for (int32 Triangle = 0; Triangle < NumTriangles; ++Triangle)
		{
			float HitT = 0.0f;
			if (MantleGarmentGeometry::RayHitsTriangle(Origin, Dir, Face.Positions[Face.Indices[Triangle * 3]], Face.Positions[Face.Indices[Triangle * 3 + 1]],
				Face.Positions[Face.Indices[Triangle * 3 + 2]], Best, HitT) && HitT < Best)
			{
				Best = HitT;
				bHit = true;
			}
		}
		if (bHit)
		{
			Out.Points[Index] = Origin + Dir * Best;
			Out.Hit[Index] = 1;
		}
	});
	for (int32 Index = 0; Index < N; ++Index)
	{
		Out.NumHit += Out.Hit[Index] ? 1 : 0;
	}
	Out.Ms = MantleGarmentGeometry::NowMs() - Start;
	return Out.NumHit > 0;
}

int32 FMantleNeckSamples::AppendRays(const FRaySet& Source, const FRaySet& Wearer, TArray<FVector3f>& InOutSource, TArray<FVector3f>& InOutWearer, int32& OutFilled)
{
	OutFilled = 0;
	const int32 N = NumRays();
	if (Source.Points.Num() != N || Wearer.Points.Num() != N || Source.Hit.Num() != N || Wearer.Hit.Num() != N)
	{
		return 0;
	}
	// Mean wearer-minus-source offset per height ring, and over every ray, from the rays both faces hit.
	TArray<FVector3f> RingSum;
	RingSum.Init(FVector3f::ZeroVector, NumHeights);
	TArray<int32> RingCount;
	RingCount.Init(0, NumHeights);
	FVector3f AllSum = FVector3f::ZeroVector;
	int32 AllCount = 0;
	for (int32 Index = 0; Index < N; ++Index)
	{
		if (Source.Hit[Index] && Wearer.Hit[Index])
		{
			const FVector3f Offset = Wearer.Points[Index] - Source.Points[Index];
			const int32 Height = Index / (NumElevations * NumAzimuths);
			RingSum[Height] += Offset;
			++RingCount[Height];
			AllSum += Offset;
			++AllCount;
		}
	}
	if (AllCount == 0)
	{
		return 0;
	}
	int32 Added = 0;
	for (int32 Index = 0; Index < N; ++Index)
	{
		if (!Source.Hit[Index])
		{
			continue;
		}
		const FVector3f& S = Source.Points[Index];
		FVector3f W;
		if (Wearer.Hit[Index])
		{
			W = Wearer.Points[Index];
		}
		else
		{
			const int32 Height = Index / (NumElevations * NumAzimuths);
			W = S + (RingCount[Height] > 0 ? RingSum[Height] / static_cast<float>(RingCount[Height]) : AllSum / static_cast<float>(AllCount));
			++OutFilled;
		}
		InOutSource.Add(S);
		InOutWearer.Add(W);
		++Added;
	}
	return Added;
}

bool FMantleNeckSamples::Correspond(const FMantleSkinnedMeshLOD& SourceFace, const FMantleSkinnedMeshLOD& WearerFace, const float SourceBaseZ,
	TArray<FVector3f>& OutSource, TArray<FVector3f>& OutWearer, float& OutMaxJumpCm, TArray<int32>* OutTriangles)
{
	OutSource.Reset();
	OutWearer.Reset();
	OutMaxJumpCm = 0.0f;
	if (OutTriangles)
	{
		OutTriangles->Reset();
	}
	const int32 NumTriangles = SourceFace.NumTriangles();
	if (NumTriangles == 0 || WearerFace.NumTriangles() != NumTriangles
		|| !MantleGarmentGeometry::IndicesInRange(SourceFace.Indices, SourceFace.NumVertices())
		|| !MantleGarmentGeometry::IndicesInRange(WearerFace.Indices, WearerFace.NumVertices()))
	{
		return false;
	}
	TArray<FVector3f> Displacement;
	Displacement.SetNum(NumTriangles);
	TArray<uint8> InRegion;
	InRegion.Init(0, NumTriangles);
	for (int32 Triangle = 0; Triangle < NumTriangles; ++Triangle)
	{
		FVector3f CentroidSource = FVector3f::ZeroVector;
		FVector3f CentroidWearer = FVector3f::ZeroVector;
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			CentroidSource += SourceFace.Positions[SourceFace.Indices[Triangle * 3 + Corner]];
			CentroidWearer += WearerFace.Positions[WearerFace.Indices[Triangle * 3 + Corner]];
		}
		CentroidSource /= 3.0f;
		CentroidWearer /= 3.0f;
		Displacement[Triangle] = CentroidWearer - CentroidSource;
		if (CentroidSource.Z >= SourceBaseZ - CorrespondBelowBaseCm && CentroidSource.Z < SourceBaseZ + CorrespondAboveBaseCm
			&& FMath::Sqrt(CentroidSource.X * CentroidSource.X + CentroidSource.Y * CentroidSource.Y) <= CorrespondRadiusCm)
		{
			InRegion[Triangle] = 1;
			OutSource.Add(CentroidSource);
			OutWearer.Add(CentroidWearer);
			if (OutTriangles)
			{
				OutTriangles->Add(Triangle);
			}
		}
	}
	// Sanity: neighbouring triangles (a shared edge, welded by position on the source) move alike.
	TMap<FIntVector, int32> Cells;
	TArray<int32> Weld;
	Weld.SetNum(SourceFace.NumVertices());
	int32 NumWelded = 0;
	for (int32 Vertex = 0; Vertex < SourceFace.NumVertices(); ++Vertex)
	{
		const FVector3f& P = SourceFace.Positions[Vertex];
		const FIntVector Key(FMath::RoundToInt(P.X * 100.0f), FMath::RoundToInt(P.Y * 100.0f), FMath::RoundToInt(P.Z * 100.0f));
		if (const int32* Found = Cells.Find(Key))
		{
			Weld[Vertex] = *Found;
		}
		else
		{
			Weld[Vertex] = NumWelded;
			Cells.Add(Key, NumWelded++);
		}
	}
	TMap<uint64, int32> EdgeOwner;
	for (int32 Triangle = 0; Triangle < NumTriangles; ++Triangle)
	{
		for (int32 Edge = 0; Edge < 3; ++Edge)
		{
			const int32 A = Weld[SourceFace.Indices[Triangle * 3 + Edge]];
			const int32 B = Weld[SourceFace.Indices[Triangle * 3 + (Edge + 1) % 3]];
			const uint64 Key = (static_cast<uint64>(FMath::Min(A, B)) << 32) | static_cast<uint64>(FMath::Max(A, B));
			if (const int32* Owner = EdgeOwner.Find(Key))
			{
				if (InRegion[Triangle] || InRegion[*Owner])
				{
					OutMaxJumpCm = FMath::Max(OutMaxJumpCm, FVector3f::Dist(Displacement[Triangle], Displacement[*Owner]));
				}
			}
			else
			{
				EdgeOwner.Add(Key, Triangle);
			}
		}
	}
	return OutSource.Num() > 0;
}

bool FMantleNeckSamples::BuildSource(const FMantleSkinnedMeshLOD& SourceFace, const TConstArrayView<FVector3f> SourceBody,
	EMantleNeckMethod& OutMethod, TArray<int32>& OutIds, TArray<FVector3f>& OutSource)
{
	OutMethod = EMantleNeckMethod::None;
	OutIds.Reset();
	OutSource.Reset();
	const float BaseZ = NeckBaseZ(SourceBody);
	if (BaseZ == -MAX_flt)
	{
		UE_LOG(LogMantle, Warning, TEXT("MANTLE_GARMENTFIT neck: the source body has no vertex within %.0f cm of the z axis, so it has no neck base"), ColumnRadiusCm);
		return false;
	}
	// The correspondence, of the source face with itself: the source centroids in the region and their triangles.
	if (FMantleFaceTopology::IsReference(SourceFace))
	{
		TArray<FVector3f> Same;
		float Jump = 0.0f;
		if (Correspond(SourceFace, SourceFace, BaseZ, OutSource, Same, Jump, &OutIds))
		{
			OutMethod = EMantleNeckMethod::Correspondence;
			return true;
		}
	}
	// The rays: AppendRays keeps every ray the source face hit, in ray order, whatever the wearer's face hits.
	FRaySet Rays;
	if (Cast(SourceFace, SourceBody, Rays))
	{
		for (int32 Index = 0; Index < NumRays(); ++Index)
		{
			if (Rays.Hit[Index])
			{
				OutIds.Add(Index);
				OutSource.Add(Rays.Points[Index]);
			}
		}
		OutMethod = EMantleNeckMethod::Rays;
		return true;
	}
	OutIds.Reset();
	OutSource.Reset();
	UE_LOG(LogMantle, Warning, TEXT("MANTLE_GARMENTFIT neck: no ray hits the source face"));
	return false;
}

bool FMantleNeckSamples::BuildWearer(const EMantleNeckMethod Method, const TConstArrayView<int32> Ids, const TConstArrayView<FVector3f> Source,
	const FMantleSkinnedMeshLOD& WearerFace, const TConstArrayView<FVector3f> WearerBody, FResult& Out)
{
	const double Start = MantleGarmentGeometry::NowMs();
	Out = FResult();
	auto Refuse = [&Out, Start](const FString& Why)
	{
		Out = FResult();
		Out.Ms = MantleGarmentGeometry::NowMs() - Start;
		UE_LOG(LogMantle, Log, TEXT("MANTLE_GARMENTFIT neck: %s; the fit runs from the body alone"), *Why);
		return false;
	};
	if (Ids.Num() == 0 || Ids.Num() != Source.Num())
	{
		return Refuse(FString::Printf(TEXT("the source half has %d ids for %d samples"), Ids.Num(), Source.Num()));
	}
	for (int32 Index = 1; Index < Ids.Num(); ++Index)
	{
		if (Ids[Index] <= Ids[Index - 1])
		{
			return Refuse(TEXT("the source half's ids are not ascending"));
		}
	}
	Out.WearerBaseZ = NeckBaseZ(WearerBody);
	if (Out.WearerBaseZ == -MAX_flt)
	{
		return Refuse(FString::Printf(TEXT("the wearer's body has no vertex within %.0f cm of the z axis"), ColumnRadiusCm));
	}
	if (WearerFace.NumTriangles() == 0 || !MantleGarmentGeometry::IndicesInRange(WearerFace.Indices, WearerFace.NumVertices()))
	{
		return Refuse(TEXT("the wearer has no usable face"));
	}

	if (Method == EMantleNeckMethod::Correspondence)
	{
		// The correspondence, the wearer side: the same triangles' centroids.
		if (!FMantleFaceTopology::IsReference(WearerFace))
		{
			return Refuse(FString::Printf(TEXT("the garment's neck is by face correspondence and the wearer's face has %d triangles, not the reference %d"),
				WearerFace.NumTriangles(), FMantleFaceTopology::ReferenceTriangles));
		}
		Out.Wearer.Reserve(Ids.Num());
		for (const int32 Triangle : Ids)
		{
			if (Triangle < 0 || Triangle >= WearerFace.NumTriangles())
			{
				return Refuse(FString::Printf(TEXT("neck triangle %d is not on the wearer's face"), Triangle));
			}
			FVector3f Centroid = FVector3f::ZeroVector;
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				Centroid += WearerFace.Positions[WearerFace.Indices[Triangle * 3 + Corner]];
			}
			Out.Wearer.Add(Centroid / 3.0f);
		}
		Out.Source = TArray<FVector3f>(Source.GetData(), Source.Num());
		Out.Method = EMantleNeckMethod::Correspondence;
		Out.Ms = MantleGarmentGeometry::NowMs() - Start;
		return true;
	}
	if (Method != EMantleNeckMethod::Rays)
	{
		return Refuse(TEXT("the source half names no method"));
	}

	// The rays, the source half standing in for the source face's cast: each id is a ray it hit, there.
	FRaySet SourceRays;
	SourceRays.Points.SetNumZeroed(NumRays());
	SourceRays.Hit.Init(0, NumRays());
	for (int32 Index = 0; Index < Ids.Num(); ++Index)
	{
		if (Ids[Index] < 0 || Ids[Index] >= NumRays())
		{
			return Refuse(FString::Printf(TEXT("neck ray %d is not in the family of %d"), Ids[Index], NumRays()));
		}
		SourceRays.Points[Ids[Index]] = Source[Index];
		SourceRays.Hit[Ids[Index]] = 1;
		++SourceRays.NumHit;
	}
	FRaySet WearerRays;
	Cast(WearerFace, WearerBody, WearerRays);
	Out.SourceRayHits = SourceRays.NumHit;
	Out.WearerRayHits = WearerRays.NumHit;
	if (AppendRays(SourceRays, WearerRays, Out.Source, Out.Wearer, Out.RaysFilled) != Ids.Num())
	{
		return Refuse(FString::Printf(TEXT("no ray hits both faces (source %d hits, wearer %d hits)"), SourceRays.NumHit, WearerRays.NumHit));
	}
	Out.Method = EMantleNeckMethod::Rays;
	Out.Ms = MantleGarmentGeometry::NowMs() - Start;
	return true;
}
