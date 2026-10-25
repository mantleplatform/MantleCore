// Copyright Joshua Eger. All Rights Reserved.
#include "MantleNailSeating.h"

#include "MantleGarmentClearance.h"

#include "Engine/SkeletalMesh.h"
#include "MeshDescription.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshModel.h"
#include "SkeletalMeshAttributes.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleNailSeatingPrivate
{
	int32 NailFindRoot(TArray<int32>& Parent, int32 Vertex)
	{
		while (Parent[Vertex] != Vertex)
		{
			Parent[Vertex] = Parent[Parent[Vertex]];
			Vertex = Parent[Vertex];
		}
		return Vertex;
	}
}

int32 FMantleNailSeating::FindIslands(TConstArrayView<uint32> Indices, const int32 NumVertices, TArray<int32>& OutIsland)
{
	using namespace MantleNailSeatingPrivate;
	TArray<int32> Parent;
	Parent.SetNumUninitialized(NumVertices);
	TBitArray<> Used(false, NumVertices);
	for (int32 Vertex = 0; Vertex < NumVertices; ++Vertex)
	{
		Parent[Vertex] = Vertex;
	}
	for (int32 Corner = 0; Corner + 2 < Indices.Num(); Corner += 3)
	{
		const int32 A = static_cast<int32>(Indices[Corner]);
		const int32 B = static_cast<int32>(Indices[Corner + 1]);
		const int32 C = static_cast<int32>(Indices[Corner + 2]);
		if (A >= NumVertices || B >= NumVertices || C >= NumVertices)
		{
			continue;
		}
		Used[A] = true;
		Used[B] = true;
		Used[C] = true;
		const int32 RootA = NailFindRoot(Parent, A);
		Parent[NailFindRoot(Parent, B)] = RootA;
		Parent[NailFindRoot(Parent, C)] = RootA;
	}
	OutIsland.Init(INDEX_NONE, NumVertices);
	TMap<int32, int32> IslandOfRoot;
	for (int32 Vertex = 0; Vertex < NumVertices; ++Vertex)
	{
		if (!Used[Vertex])
		{
			continue;
		}
		const int32 Root = NailFindRoot(Parent, Vertex);
		int32* Found = IslandOfRoot.Find(Root);
		if (!Found)
		{
			const int32 Next = IslandOfRoot.Num();
			Found = &IslandOfRoot.Add(Root, Next);
		}
		OutIsland[Vertex] = *Found;
	}
	return IslandOfRoot.Num();
}

FVector3f FMantleNailSeating::ThinAxis(TConstArrayView<FVector3f> Points)
{
	if (Points.Num() < 3)
	{
		return FVector3f::ZeroVector;
	}
	FVector Mean = FVector::ZeroVector;
	for (const FVector3f& Point : Points)
	{
		Mean += FVector(Point);
	}
	Mean /= static_cast<double>(Points.Num());
	// The covariance (symmetric, positive semi-definite).
	double Cov[3][3] = {};
	for (const FVector3f& Point : Points)
	{
		const FVector D = FVector(Point) - Mean;
		const double V[3] = { D.X, D.Y, D.Z };
		for (int32 Row = 0; Row < 3; ++Row)
		{
			for (int32 Col = 0; Col < 3; ++Col)
			{
				Cov[Row][Col] += V[Row] * V[Col];
			}
		}
	}
	const double Trace = Cov[0][0] + Cov[1][1] + Cov[2][2];
	if (Trace <= UE_DOUBLE_SMALL_NUMBER)
	{
		return FVector3f::ZeroVector;
	}
	// trace*I - Cov has the same axes with the order of the spreads reversed, so its largest axis (power iteration) is
	// Cov's smallest. The start is arbitrary and not on any axis a nail is likely to have.
	double B[3][3];
	for (int32 Row = 0; Row < 3; ++Row)
	{
		for (int32 Col = 0; Col < 3; ++Col)
		{
			B[Row][Col] = (Row == Col ? Trace : 0.0) - Cov[Row][Col];
		}
	}
	FVector Axis(0.5773, 0.4113, 0.7071);
	Axis.Normalize();
	for (int32 Iteration = 0; Iteration < 200; ++Iteration)
	{
		const FVector Next(B[0][0] * Axis.X + B[0][1] * Axis.Y + B[0][2] * Axis.Z,
			B[1][0] * Axis.X + B[1][1] * Axis.Y + B[1][2] * Axis.Z,
			B[2][0] * Axis.X + B[2][1] * Axis.Y + B[2][2] * Axis.Z);
		const double Length = Next.Size();
		if (Length <= UE_DOUBLE_SMALL_NUMBER)
		{
			break;
		}
		Axis = Next / Length;
	}
	return FVector3f(Axis);
}

bool FMantleNailSeating::Seat(TArray<FVector3f>& Positions, TConstArrayView<uint32> Indices, const FMantleSkinnedMeshLOD& Body,
	FMantleNailSeatingResult& Out, FString& OutError)
{
	Out = FMantleNailSeatingResult();
	if (Positions.Num() == 0 || Indices.Num() < 3 || Indices.Num() % 3 != 0 || Body.NumVertices() == 0 || Body.NumTriangles() == 0)
	{
		OutError = FString::Printf(TEXT("nail seating needs a nail mesh and a body surface (%d vertices, %d indices, a body of %d vertices and %d triangles)"),
			Positions.Num(), Indices.Num(), Body.NumVertices(), Body.NumTriangles());
		return false;
	}
	for (const uint32 Index : Indices)
	{
		if (static_cast<int32>(Index) >= Positions.Num())
		{
			OutError = TEXT("a nail triangle names a vertex the mesh does not have");
			return false;
		}
	}
	TArray<int32> Island;
	Out.NumIslands = FindIslands(Indices, Positions.Num(), Island);
	TArray<TArray<int32>> Members;
	Members.SetNum(Out.NumIslands);
	for (int32 Vertex = 0; Vertex < Island.Num(); ++Vertex)
	{
		if (Island[Vertex] != INDEX_NONE)
		{
			Members[Island[Vertex]].Add(Vertex);
		}
	}

	const TArray<FVector3f> Start = Positions;
	TArray<bool> bSeated;
	bSeated.Init(false, Out.NumIslands);
	FMantleGarmentClearance Clearance;
	for (int32 Pass = 0; Pass < Passes; ++Pass)
	{
		if (!FMantleGarmentClearance::Compute(Positions, Body, Clearance, SearchCm, &OutError))
		{
			return false;
		}
		for (int32 Shell = 0; Shell < Out.NumIslands; ++Shell)
		{
			const TArray<int32>& Vertices = Members[Shell];
			float Closest = TNumericLimits<float>::Max();
			FVector3f SkinNormal = FVector3f::ZeroVector;
			TArray<FVector3f> Points;
			Points.Reserve(Vertices.Num());
			for (const int32 Vertex : Vertices)
			{
				Points.Add(Positions[Vertex]);
				float Distance = 0.0f;
				FVector3f Normal = FVector3f::ZeroVector;
				if (Clearance.IsTracked(Vertex) && Clearance.Measure(Vertex, Positions[Vertex], Body.Positions, Distance, Normal))
				{
					Closest = FMath::Min(Closest, Distance);
					SkinNormal += Normal;
				}
			}
			if (Closest == TNumericLimits<float>::Max())
			{
				continue;
			}
			FVector3f Axis = ThinAxis(Points);
			if (Axis.IsNearlyZero())
			{
				continue;
			}
			// Away from the skin under it.
			if (FVector3f::DotProduct(Axis, SkinNormal) < 0.0f)
			{
				Axis = -Axis;
			}
			const FVector3f Move = Axis * (SeatCm - Closest);
			for (const int32 Vertex : Vertices)
			{
				Positions[Vertex] += Move;
			}
			bSeated[Shell] = true;
		}
	}

	// What it came to.
	if (!FMantleGarmentClearance::Compute(Positions, Body, Clearance, SearchCm, &OutError))
	{
		return false;
	}
	Out.LowestClearanceCm = TNumericLimits<float>::Max();
	Out.HighestClearanceCm = -TNumericLimits<float>::Max();
	for (int32 Shell = 0; Shell < Out.NumIslands; ++Shell)
	{
		if (!bSeated[Shell])
		{
			continue;
		}
		++Out.NumSeated;
		float Closest = TNumericLimits<float>::Max();
		for (const int32 Vertex : Members[Shell])
		{
			Out.MaxMoveCm = FMath::Max(Out.MaxMoveCm, FVector3f::Dist(Positions[Vertex], Start[Vertex]));
			float Distance = 0.0f;
			FVector3f Normal = FVector3f::ZeroVector;
			if (Clearance.IsTracked(Vertex) && Clearance.Measure(Vertex, Positions[Vertex], Body.Positions, Distance, Normal))
			{
				Closest = FMath::Min(Closest, Distance);
			}
		}
		if (Closest != TNumericLimits<float>::Max())
		{
			Out.LowestClearanceCm = FMath::Min(Out.LowestClearanceCm, Closest);
			Out.HighestClearanceCm = FMath::Max(Out.HighestClearanceCm, Closest);
		}
	}
	if (Out.NumSeated == 0 || Out.LowestClearanceCm > Out.HighestClearanceCm)
	{
		Out.LowestClearanceCm = 0.0f;
		Out.HighestClearanceCm = 0.0f;
	}
	return true;
}

bool FMantleNailSeating::SeatMeshDescription(FMeshDescription& Mesh, const FMantleSkinnedMeshLOD& Body, FMantleNailSeatingResult& Out, FString& OutError)
{
	FSkeletalMeshAttributes Attributes(Mesh);
	TVertexAttributesRef<FVector3f> MeshPositions = Attributes.GetVertexPositions();
	if (!MeshPositions.IsValid())
	{
		OutError = TEXT("the nail mesh has no vertex positions");
		return false;
	}
	TArray<FVertexID> VertexIds;
	TMap<int32, int32> IndexOf;
	TArray<FVector3f> Positions;
	for (const FVertexID Vertex : Mesh.Vertices().GetElementIDs())
	{
		IndexOf.Add(Vertex.GetValue(), Positions.Num());
		VertexIds.Add(Vertex);
		Positions.Add(MeshPositions.Get(Vertex));
	}
	TArray<uint32> Indices;
	for (const FTriangleID Triangle : Mesh.Triangles().GetElementIDs())
	{
		for (const FVertexInstanceID Instance : Mesh.GetTriangleVertexInstances(Triangle))
		{
			Indices.Add(static_cast<uint32>(IndexOf.FindChecked(Mesh.GetVertexInstanceVertex(Instance).GetValue())));
		}
	}
	if (!Seat(Positions, Indices, Body, Out, OutError))
	{
		return false;
	}
	for (int32 Index = 0; Index < VertexIds.Num(); ++Index)
	{
		MeshPositions.Set(VertexIds[Index], Positions[Index]);
	}
	return true;
}

bool FMantleNailSeating::ReadBodySurface(const USkeletalMesh& Body, const int32 LODIndex, FMantleSkinnedMeshLOD& Out, FString& OutError)
{
	Out = FMantleSkinnedMeshLOD();
	if (Body.HasMeshDescription(LODIndex))
	{
		if (const FMeshDescription* Description = Body.GetMeshDescription(LODIndex))
		{
			FSkeletalMeshConstAttributes Attributes(*Description);
			TVertexAttributesConstRef<FVector3f> Positions = Attributes.GetVertexPositions();
			TVertexInstanceAttributesConstRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
			if (Positions.IsValid() && Normals.IsValid())
			{
				TMap<int32, int32> IndexOf;
				for (const FVertexID Vertex : Description->Vertices().GetElementIDs())
				{
					IndexOf.Add(Vertex.GetValue(), Out.Positions.Num());
					Out.Positions.Add(Positions.Get(Vertex));
				}
				Out.Normals.Init(FVector3f::ZeroVector, Out.Positions.Num());
				for (const FTriangleID Triangle : Description->Triangles().GetElementIDs())
				{
					for (const FVertexInstanceID Instance : Description->GetTriangleVertexInstances(Triangle))
					{
						const int32 Vertex = IndexOf.FindChecked(Description->GetVertexInstanceVertex(Instance).GetValue());
						Out.Indices.Add(static_cast<uint32>(Vertex));
						Out.Normals[Vertex] += Normals.Get(Instance);
					}
				}
				for (FVector3f& Normal : Out.Normals)
				{
					Normal = Normal.GetSafeNormal();
				}
				if (Out.NumVertices() > 0 && Out.NumTriangles() > 0)
				{
					return true;
				}
			}
		}
	}
	// The imported model: render-order vertices (position and tangent Z) and the LOD's index buffer.
	const FSkeletalMeshModel* Imported = Body.GetImportedModel();
	if (Imported && Imported->LODModels.IsValidIndex(LODIndex))
	{
		const FSkeletalMeshLODModel& Model = Imported->LODModels[LODIndex];
		Out = FMantleSkinnedMeshLOD();
		Out.Positions.SetNumZeroed(static_cast<int32>(Model.NumVertices));
		Out.Normals.SetNumZeroed(static_cast<int32>(Model.NumVertices));
		for (const FSkelMeshSection& Section : Model.Sections)
		{
			for (int32 Local = 0; Local < Section.SoftVertices.Num(); ++Local)
			{
				const int32 Vertex = static_cast<int32>(Section.BaseVertexIndex) + Local;
				if (Out.Positions.IsValidIndex(Vertex))
				{
					Out.Positions[Vertex] = Section.SoftVertices[Local].Position;
					Out.Normals[Vertex] = Section.SoftVertices[Local].TangentZ;
				}
			}
		}
		Out.Indices = Model.IndexBuffer;
		if (Out.NumVertices() > 0 && Out.NumTriangles() > 0)
		{
			return true;
		}
	}
	OutError = FString::Printf(TEXT("the body %s has no readable surface for LOD %d"), *Body.GetName(), LODIndex);
	return false;
}
