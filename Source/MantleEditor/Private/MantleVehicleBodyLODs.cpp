// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleBodyLODs.h"

#include "MantleAvatarContract.h"
#include "MantleSkinnedMesh.h"
#include "MantleVehicleBodyReduce.h"
#include "MantleVehicleChassisMesh.h"
#include "MantleVehicleLightFinder.h"
#include "MantleVehicleSource.h"

#define LOCTEXT_NAMESPACE "MantleVehicleBodyLODs"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVehicleBodyLODsPrivate
{
	/** The joint that moves a vertex most (the first of equal weights). */
	int32 HeaviestJoint(const FMantleGlbMesh& Mesh, const int32 Vertex)
	{
		const int32 Slots = FMath::Max(1, Mesh.NumInfluences);
		int32 Best = 0;
		float BestWeight = -1.0f;
		for (int32 Slot = 0; Slot < Slots; ++Slot)
		{
			const int32 At = Vertex * Slots + Slot;
			if (Mesh.InfluenceWeights.IsValidIndex(At) && Mesh.InfluenceWeights[At] > BestWeight)
			{
				BestWeight = Mesh.InfluenceWeights[At];
				Best = Mesh.InfluenceJoints.IsValidIndex(At) ? Mesh.InfluenceJoints[At] : 0;
			}
		}
		return Best;
	}

	/** Body as the reducer takes it: one LOD, its triangles grouped by material, each vertex whole on its heaviest joint. */
	FMantleSkinnedMesh ToSkinned(const FMantleGlbMesh& Body)
	{
		FMantleSkinnedMesh Out;
		FMantleSkinnedMeshLOD& LOD = Out.LODs.AddDefaulted_GetRef();
		LOD.Positions = Body.Positions;
		LOD.Normals = Body.Normals;
		LOD.UVs = Body.UVs;
		LOD.NumInfluences = 1;
		LOD.InfluenceBones.SetNumUninitialized(Body.Positions.Num());
		for (int32 Vertex = 0; Vertex < Body.Positions.Num(); ++Vertex)
		{
			LOD.InfluenceBones[Vertex] = static_cast<uint16>(HeaviestJoint(Body, Vertex));
		}
		LOD.InfluenceWeights.Init(255, Body.Positions.Num());
		for (int32 Material = 0; Material < Body.MaterialNames.Num(); ++Material)
		{
			FMantleSkinnedMeshSection Section;
			Section.MaterialIndex = Material;
			Section.MaterialName = FName(*Body.MaterialNames[Material]);
			Section.FirstIndex = LOD.Indices.Num();
			for (const FMantleGlbPrimitive& Primitive : Body.Primitives)
			{
				if (Primitive.MaterialIndex == Material)
				{
					for (const uint32 Index : Primitive.Indices)
					{
						LOD.Indices.Add(static_cast<uint32>(Primitive.FirstVertex) + Index);
					}
				}
			}
			Section.IndexCount = LOD.Indices.Num() - Section.FirstIndex;
			if (Section.IndexCount > 0)
			{
				LOD.Sections.Add(Section);
			}
		}
		return Out;
	}

	/** The tangents of a mesh from its UVs, kept square to its normals (w the binormal's sign). */
	TArray<FVector4f> MakeTangents(const TArray<FVector3f>& Positions, const TArray<FVector3f>& Normals, const TArray<FVector2f>& UVs, const TArray<uint32>& Indices)
	{
		TArray<FVector3f> Tan, Bin;
		Tan.Init(FVector3f::ZeroVector, Positions.Num());
		Bin.Init(FVector3f::ZeroVector, Positions.Num());
		for (int32 First = 0; First + 2 < Indices.Num(); First += 3)
		{
			const uint32 A = Indices[First], B = Indices[First + 1], C = Indices[First + 2];
			const FVector3f E1 = Positions[B] - Positions[A];
			const FVector3f E2 = Positions[C] - Positions[A];
			const FVector2f D1 = UVs[B] - UVs[A];
			const FVector2f D2 = UVs[C] - UVs[A];
			const float Det = D1.X * D2.Y - D2.X * D1.Y;
			if (FMath::Abs(Det) < UE_SMALL_NUMBER)
			{
				continue;
			}
			const float R = 1.0f / Det;
			const FVector3f T = (E1 * D2.Y - E2 * D1.Y) * R;
			const FVector3f S = (E2 * D1.X - E1 * D2.X) * R;
			for (const uint32 V : { A, B, C })
			{
				Tan[V] += T;
				Bin[V] += S;
			}
		}
		TArray<FVector4f> Out;
		Out.SetNumUninitialized(Positions.Num());
		for (int32 V = 0; V < Positions.Num(); ++V)
		{
			const FVector3f N = Normals.IsValidIndex(V) ? Normals[V].GetSafeNormal() : FVector3f::UpVector;
			FVector3f T = (Tan[V] - N * FVector3f::DotProduct(N, Tan[V])).GetSafeNormal();
			if (T.IsNearlyZero())
			{
				// No UV gradient here: any direction square to the normal.
				T = FVector3f::CrossProduct(N, FMath::Abs(N.Z) < 0.9f ? FVector3f::UpVector : FVector3f::ForwardVector).GetSafeNormal();
			}
			const float Sign = FVector3f::DotProduct(FVector3f::CrossProduct(N, T), Bin[V]) < 0.0f ? -1.0f : 1.0f;
			Out[V] = FVector4f(T.X, T.Y, T.Z, Sign);
		}
		return Out;
	}

	/** One lower LOD as it uploads: a primitive per material over its own run of vertices, LOD0's joints, one whole joint per vertex. */
	struct FLowerLOD
	{
		FMantleGlbMesh Glb;
		/** Material names of LOD0 this LOD draws. */
		TSet<FString> Drawn;
	};

	/** Appends triangles (absolute indices into the From arrays) as a primitive drawn with Material, its vertices copied in. */
	void AppendPrimitive(FMantleGlbMesh& To, const FString& Material, const TArray<uint32>& Triangles, const TArray<FVector3f>& Positions,
		const TArray<FVector3f>& Normals, const TArray<FVector2f>& UVs, const TFunctionRef<int32(uint32)> JointOf)
	{
		int32 MaterialIndex = To.MaterialNames.IndexOfByKey(Material);
		if (MaterialIndex == INDEX_NONE)
		{
			MaterialIndex = To.MaterialNames.Add(Material);
		}
		FMantleGlbPrimitive& Primitive = To.Primitives.AddDefaulted_GetRef();
		Primitive.MaterialIndex = MaterialIndex;
		Primitive.FirstVertex = To.Positions.Num();
		TMap<uint32, uint32> Local;
		for (const uint32 Old : Triangles)
		{
			uint32* Found = Local.Find(Old);
			if (!Found)
			{
				Found = &Local.Add(Old, static_cast<uint32>(To.Positions.Num() - Primitive.FirstVertex));
				To.Positions.Add(Positions[Old]);
				To.Normals.Add(Normals.IsValidIndex(Old) ? Normals[Old] : FVector3f::UpVector);
				To.UVs.Add(UVs.IsValidIndex(Old) ? UVs[Old] : FVector2f::ZeroVector);
				To.InfluenceJoints.Add(static_cast<uint16>(JointOf(Old)));
				To.InfluenceWeights.Add(1.0f);
			}
			Primitive.Indices.Add(*Found);
		}
		Primitive.NumVertices = To.Positions.Num() - Primitive.FirstVertex;
	}

	/** A vertex's bytes in every stream, hashed once. */
	struct FVertexKey
	{
		TArray<uint8> Bytes;
		uint32 Hash = 0;

		bool operator==(const FVertexKey& Other) const { return Hash == Other.Hash && Bytes == Other.Bytes; }
		friend uint32 GetTypeHash(const FVertexKey& Key) { return Key.Hash; }
	};

	/** The triangles (absolute indices) Body draws with material Material. */
	TArray<uint32> TrianglesOf(const FMantleGlbMesh& Body, const int32 Material)
	{
		TArray<uint32> Out;
		for (const FMantleGlbPrimitive& Primitive : Body.Primitives)
		{
			if (Primitive.MaterialIndex == Material)
			{
				for (const uint32 Index : Primitive.Indices)
				{
					Out.Add(static_cast<uint32>(Primitive.FirstVertex) + Index);
				}
			}
		}
		return Out;
	}
}

TArray<FMantleGlbMesh> FMantleVehicleBodyLODs::MakeFor(const UMantleVehicleSource& Source, const FMantleGlbMesh& Body, TArray<FString>& OutNotes, FString& OutWarning)
{
	TArray<float> Fractions;
	if (const FMantleAvatarContract* Contract = FMantleAvatarContract::Get())
	{
		for (int32 Index = 1; Index < Contract->LODTargetFractions.Num(); ++Index)
		{
			Fractions.Add(Contract->LODTargetFractions[Index]);
		}
	}
	TArray<FString> Keep = Source.Paint.Sections;
	for (const FMantleVehicleSourcePaintLayer& Layer : Source.Paint.Layers)
	{
		Keep.Append(Layer.Sections);
	}
	for (const EMantleLightKind Kind : { EMantleLightKind::Head, EMantleLightKind::Rear, EMantleLightKind::Reverse, EMantleLightKind::Left, EMantleLightKind::Right })
	{
		Keep.Append(FMantleVehicleLights::Sections(Source.Lights, Kind));
	}
	return Make(Body, Fractions, Keep, OutNotes, OutWarning);
}

void FMantleVehicleBodyLODs::Slim(FMantleGlbMesh& Mesh)
{
	Mesh.bCompactStreams = Mesh.IsSkinned() && Mesh.NumInfluences <= 4 && Mesh.Joints.Num() <= 256;
	const int32 Slots = FMath::Max(1, Mesh.NumInfluences);
	if (Mesh.Positions.Num() == 0 || (Mesh.IsSkinned() && (Mesh.InfluenceJoints.Num() != Mesh.Positions.Num() * Slots
		|| Mesh.InfluenceWeights.Num() != Mesh.Positions.Num() * Slots)) || !Mesh.HairRootUV.IsEmpty() || !Mesh.BodyOffsets.IsEmpty())
	{
		return;
	}
	// A vertex's bytes in every stream it has: two vertices with the same bytes are one.
	const auto Key = [&Mesh, Slots](const int32 Vertex)
	{
		MantleVehicleBodyLODsPrivate::FVertexKey Made;
		TArray<uint8>& Bytes = Made.Bytes;
		const auto Add = [&Bytes](const void* Data, const int32 Size) { Bytes.Append(static_cast<const uint8*>(Data), Size); };
		Add(&Mesh.Positions[Vertex], sizeof(FVector3f));
		Add(&Mesh.Normals[Vertex], sizeof(FVector3f));
		Add(&Mesh.UVs[Vertex], sizeof(FVector2f));
		if (Mesh.Tangents.Num() > 0) { Add(&Mesh.Tangents[Vertex], sizeof(FVector4f)); }
		if (Mesh.UV1s.Num() > 0) { Add(&Mesh.UV1s[Vertex], sizeof(FVector2f)); }
		if (Mesh.Colors.Num() > 0) { Add(&Mesh.Colors[Vertex], sizeof(FColor)); }
		if (Mesh.IsSkinned())
		{
			Add(&Mesh.InfluenceJoints[Vertex * Slots], Slots * sizeof(uint16));
			Add(&Mesh.InfluenceWeights[Vertex * Slots], Slots * sizeof(float));
		}
		Made.Hash = FCrc::MemCrc32(Bytes.GetData(), Bytes.Num());
		return Made;
	};
	FMantleGlbMesh Out = Mesh;
	Out.Positions.Reset(); Out.Normals.Reset(); Out.UVs.Reset(); Out.Tangents.Reset(); Out.UV1s.Reset(); Out.Colors.Reset();
	Out.InfluenceJoints.Reset(); Out.InfluenceWeights.Reset();
	for (FMantleGlbPrimitive& Primitive : Out.Primitives)
	{
		TMap<MantleVehicleBodyLODsPrivate::FVertexKey, uint32> Seen;
		TArray<uint32> Indices;
		Indices.Reserve(Primitive.Indices.Num());
		const int32 First = Out.Positions.Num();
		for (const uint32 Local : Primitive.Indices)
		{
			const int32 Old = Primitive.FirstVertex + static_cast<int32>(Local);
			MantleVehicleBodyLODsPrivate::FVertexKey Bytes = Key(Old);
			if (const uint32* Found = Seen.Find(Bytes))
			{
				Indices.Add(*Found);
				continue;
			}
			const uint32 New = static_cast<uint32>(Out.Positions.Num() - First);
			Seen.Add(MoveTemp(Bytes), New);
			Out.Positions.Add(Mesh.Positions[Old]);
			Out.Normals.Add(Mesh.Normals[Old]);
			Out.UVs.Add(Mesh.UVs[Old]);
			if (Mesh.Tangents.Num() > 0) { Out.Tangents.Add(Mesh.Tangents[Old]); }
			if (Mesh.UV1s.Num() > 0) { Out.UV1s.Add(Mesh.UV1s[Old]); }
			if (Mesh.Colors.Num() > 0) { Out.Colors.Add(Mesh.Colors[Old]); }
			if (Mesh.IsSkinned())
			{
				Out.InfluenceJoints.Append(&Mesh.InfluenceJoints[Old * Slots], Slots);
				Out.InfluenceWeights.Append(&Mesh.InfluenceWeights[Old * Slots], Slots);
			}
			Indices.Add(New);
		}
		Primitive.FirstVertex = First;
		Primitive.NumVertices = Out.Positions.Num() - First;
		Primitive.Indices = MoveTemp(Indices);
	}
	Mesh = MoveTemp(Out);
}

int32 FMantleVehicleBodyLODs::CountTriangles(const FMantleGlbMesh& Mesh)
{
	int32 Triangles = 0;
	for (const FMantleGlbPrimitive& Primitive : Mesh.Primitives)
	{
		Triangles += Primitive.Indices.Num() / 3;
	}
	return Triangles;
}

TArray<FMantleGlbMesh> FMantleVehicleBodyLODs::Make(const FMantleGlbMesh& Body, const TConstArrayView<float> Fractions, const TConstArrayView<FString> KeepSections,
	TArray<FString>& OutNotes, FString& OutWarning)
{
	using namespace MantleVehicleBodyLODsPrivate;
	OutWarning.Reset();
	TArray<FMantleGlbMesh> Lower;
	const int32 Triangles = CountTriangles(Body);
	if (Fractions.Num() == 0 || Triangles == 0 || !Body.IsSkinned())
	{
		return Lower;
	}

	const double Start = FPlatformTime::Seconds();
	const FMantleSkinnedMesh Source = ToSkinned(Body);
	int32 Previous = Triangles;
	TArray<FString> Counts = { FString::FromInt(Triangles) };
	for (int32 Index = 0; Index < Fractions.Num() && Index + 1 < FMantleGlbWriter::MaxLODs; ++Index)
	{
		const int32 LODIndex = Index + 1;
		const int32 Target = FMath::Max(1, FMath::FloorToInt(static_cast<float>(Triangles) * Fractions[Index]));
		FMantleSkinnedMesh Reduced = Source;
		FMantleVehicleBodyExtras Extras;
		FMantleVehicleBodyReduceResult Result;
		const FText Progress = FText::Format(LOCTEXT("MakingLOD", "Making your vehicle's far-away version {0} ({1} triangles)..."),
			FText::AsNumber(LODIndex), FText::AsNumber(Target));
		if (!FMantleVehicleBodyReduce::ReduceToFit(Reduced, Extras, Target, Result, &Progress) || !Result.bReduced)
		{
			// The reducer's own reasons speak of the body's limit; for a far-away version, a body it cannot cut down that far is simply already simple.
			const bool bTooSimple = Result.Problem.IsEmpty() || Result.Problem.StartsWith(TEXT("the reducer left"));
			OutWarning = Result.bCanceled
				? FString::Printf(TEXT("You canceled making its far-away versions, so it uploads with %d of %d. It still works; it only costs more to draw far away."),
					LODIndex, Fractions.Num() + 1)
				: bTooSimple
				? FString::Printf(TEXT("Its far-away version %d was not made because its body is already too simple to cut down that far, so it uploads with %d of %d. It still works."),
					LODIndex, LODIndex, Fractions.Num() + 1)
				: FString::Printf(TEXT("Its far-away version %d could not be made (%s), so it uploads with %d of %d. It still works; it only costs more to draw far away."),
					LODIndex, *Result.Problem, LODIndex, Fractions.Num() + 1);
			break;
		}

		// Back to an upload mesh: a primitive per material, on LOD0's joints.
		const FMantleSkinnedMeshLOD& Made = Reduced.LODs[0];
		FLowerLOD Out;
		Out.Glb.Name = FString::Printf(TEXT("%s_LOD%d"), *Body.Name, LODIndex);
		Out.Glb.AuthoringSourceHash = Body.AuthoringSourceHash;
		Out.Glb.Joints = Body.Joints;
		Out.Glb.NumInfluences = 1;
		for (const FMantleSkinnedMeshSection& Section : Made.Sections)
		{
			if (!Body.MaterialNames.IsValidIndex(Section.MaterialIndex) || Section.IndexCount < 3)
			{
				continue;
			}
			TArray<uint32> SectionIndices(Made.Indices.GetData() + Section.FirstIndex, Section.IndexCount);
			const FString& Material = Body.MaterialNames[Section.MaterialIndex];
			AppendPrimitive(Out.Glb, Material, SectionIndices, Made.Positions, Made.Normals, Made.UVs,
				[&Made](const uint32 Vertex) { return Made.InfluenceBones.IsValidIndex(Vertex) ? static_cast<int32>(Made.InfluenceBones[Vertex]) : 0; });
			Out.Drawn.Add(Material);
		}
		// A light or paint section the reduction lost comes back whole from LOD0.
		TArray<FString> PutBack;
		for (const FString& Keep : KeepSections)
		{
			const int32 Material = Body.MaterialNames.IndexOfByKey(Keep);
			if (Material == INDEX_NONE || Out.Drawn.Contains(Keep))
			{
				continue;
			}
			const TArray<uint32> Original = TrianglesOf(Body, Material);
			if (Original.Num() >= 3)
			{
				AppendPrimitive(Out.Glb, Keep, Original, Body.Positions, Body.Normals, Body.UVs,
					[&Body](const uint32 Vertex) { return HeaviestJoint(Body, static_cast<int32>(Vertex)); });
				Out.Drawn.Add(Keep);
				PutBack.Add(Keep);
			}
		}
		if (Body.Tangents.Num() > 0)
		{
			TArray<uint32> All;
			for (const FMantleGlbPrimitive& Primitive : Out.Glb.Primitives)
			{
				for (const uint32 Local : Primitive.Indices)
				{
					All.Add(static_cast<uint32>(Primitive.FirstVertex) + Local);
				}
			}
			Out.Glb.Tangents = MakeTangents(Out.Glb.Positions, Out.Glb.Normals, Out.Glb.UVs, All);
		}

		const int32 MadeTriangles = CountTriangles(Out.Glb);
		if (MadeTriangles <= 0 || MadeTriangles > Previous)
		{
			OutWarning = FString::Printf(TEXT("Its far-away version %d came out no smaller than the one before (%d triangles), so it uploads with %d of %d. It still works; it only costs more to draw far away."),
				LODIndex, MadeTriangles, LODIndex, Fractions.Num() + 1);
			break;
		}
		if (PutBack.Num() > 0)
		{
			OutNotes.Add(FString::Printf(TEXT("Far-away version %d keeps %s whole, so it still shows far away."), LODIndex, *FString::Join(PutBack, TEXT(", "))));
		}
		Previous = MadeTriangles;
		Counts.Add(FString::FromInt(MadeTriangles));
		Lower.Add(MoveTemp(Out.Glb));
	}
	OutNotes.Add(FString::Printf(TEXT("Its LODs (the versions drawn farther away): %s triangles (made in %.1f s)."), *FString::Join(Counts, TEXT(" / ")),
		FPlatformTime::Seconds() - Start));
	UE_LOG(LogTemp, Log, TEXT("MANTLE_VEHICLE_LODS %s triangles=%s"), *Body.Name, *FString::Join(Counts, TEXT("/")));
	return Lower;
}

#undef LOCTEXT_NAMESPACE
