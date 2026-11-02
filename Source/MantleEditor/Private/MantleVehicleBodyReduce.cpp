// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleBodyReduce.h"

#include "MantleMeshReduce.h"

#include "MeshDescription.h"
#include "Misc/ScopedSlowTask.h"
#include "StaticMeshAttributes.h"

#define LOCTEXT_NAMESPACE "MantleVehicleBodyReduce"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVehicleBodyReducePrivate
{
	/** The prefix a piece's polygon groups are named with, followed by the material slot's index. */
	const TCHAR* const SlotPrefix = TEXT("MantleVehicleBodySlot_");

	/** A piece below this many triangles is left as it is: the reducer has nothing to gain from it. */
	constexpr int32 SmallestWorthReducing = 24;

	/** The bone a vertex rides: its first influence (a body on a chassis has one, whole). */
	int32 BoneOfVertex(const FMantleSkinnedMeshLOD& LOD, const int32 Vertex)
	{
		const int32 Stride = FMath::Max(1, LOD.NumInfluences);
		const int32 At = Vertex * Stride;
		return LOD.InfluenceBones.IsValidIndex(At) ? static_cast<int32>(LOD.InfluenceBones[At]) : 0;
	}

	/** One bone's triangles, by material. */
	struct FBonePiece
	{
		int32 Bone = 0;
		/** Material slot index to the triangles (index into LOD.Indices / 3). */
		TMap<int32, TArray<int32>> ByMaterial;
		int32 Triangles = 0;
		FMeshDescription Description;
		TMap<FName, int32> MaterialOfSlotName;
	};

	/** Each triangle's material, from the sections (all material 0 for a body with none). */
	TArray<int32> MaterialOfTriangles(const FMantleSkinnedMeshLOD& LOD, TMap<int32, FName>& OutNames)
	{
		TArray<int32> Material;
		Material.Init(0, LOD.NumTriangles());
		for (const FMantleSkinnedMeshSection& Section : LOD.Sections)
		{
			OutNames.FindOrAdd(Section.MaterialIndex) = Section.MaterialName;
			for (int32 Index = Section.FirstIndex; Index + 2 < Section.FirstIndex + Section.IndexCount && Index / 3 < Material.Num(); Index += 3)
			{
				Material[Index / 3] = Section.MaterialIndex;
			}
		}
		return Material;
	}

	FVector3f FaceNormal(const FMantleSkinnedMeshLOD& LOD, const int32 Triangle)
	{
		const FVector3f& A = LOD.Positions[LOD.Indices[Triangle * 3]];
		const FVector3f& B = LOD.Positions[LOD.Indices[Triangle * 3 + 1]];
		const FVector3f& C = LOD.Positions[LOD.Indices[Triangle * 3 + 2]];
		return FVector3f::CrossProduct(B - A, C - A).GetSafeNormal();
	}

	/** The piece as a mesh description: one vertex and instance per model vertex, one polygon group per material. */
	void MakeDescription(const FMantleSkinnedMeshLOD& LOD, FBonePiece& Piece)
	{
		FStaticMeshAttributes Attributes(Piece.Description);
		Attributes.Register();
		TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
		TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
		TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
		TPolygonGroupAttributesRef<FName> SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
		UVs.SetNumChannels(1);
		const bool bNormals = LOD.Normals.Num() == LOD.Positions.Num();
		const bool bUVs = LOD.UVs.Num() == LOD.Positions.Num();

		TMap<int32, FVertexInstanceID> InstanceOf;
		const auto Instance = [&](const int32 Vertex, const int32 Triangle) -> FVertexInstanceID
		{
			if (const FVertexInstanceID* Found = InstanceOf.Find(Vertex))
			{
				return *Found;
			}
			const FVertexID Id = Piece.Description.CreateVertex();
			Positions[Id] = LOD.Positions[Vertex];
			const FVertexInstanceID Made = Piece.Description.CreateVertexInstance(Id);
			Normals[Made] = bNormals ? LOD.Normals[Vertex] : FaceNormal(LOD, Triangle);
			UVs.Set(Made, 0, bUVs ? LOD.UVs[Vertex] : FVector2f::ZeroVector);
			InstanceOf.Add(Vertex, Made);
			return Made;
		};
		TArray<int32> Materials;
		Piece.ByMaterial.GetKeys(Materials);
		Materials.Sort();
		for (const int32 Material : Materials)
		{
			const FPolygonGroupID Group = Piece.Description.CreatePolygonGroup();
			const FName Name = FMantleMeshReduce::SlotName(SlotPrefix, Material);
			SlotNames[Group] = Name;
			Piece.MaterialOfSlotName.Add(Name, Material);
			for (const int32 Triangle : Piece.ByMaterial[Material])
			{
				const FVertexInstanceID Corners[3] = { Instance(LOD.Indices[Triangle * 3], Triangle), Instance(LOD.Indices[Triangle * 3 + 1], Triangle),
					Instance(LOD.Indices[Triangle * 3 + 2], Triangle) };
				Piece.Description.CreateTriangle(Group, TArrayView<const FVertexInstanceID>(Corners, 3));
			}
		}
	}
}

int32 FMantleVehicleBodyReduce::CountTriangles(const FMantleSkinnedMesh& Body)
{
	return Body.LODs.Num() > 0 ? Body.LODs[0].NumTriangles() : 0;
}

bool FMantleVehicleBodyReduce::ReduceToFit(FMantleSkinnedMesh& Body, FMantleVehicleBodyExtras& Extras, const int32 MaxTriangles, FMantleVehicleBodyReduceResult& Out,
	const FText* Progress)
{
	using namespace MantleVehicleBodyReducePrivate;
	Out = FMantleVehicleBodyReduceResult();
	Out.Before = CountTriangles(Body);
	Out.After = Out.Before;
	if (Body.LODs.Num() == 0 || Out.Before <= MaxTriangles || MaxTriangles < 1)
	{
		return true;
	}
	if (!FMantleMeshReduce::IsAvailable())
	{
		Out.Problem = TEXT("this editor has no mesh reducer");
		return false;
	}
	const FMantleSkinnedMeshLOD& LOD = Body.LODs[0];
	if (LOD.Indices.Num() % 3 != 0 || LOD.Positions.Num() == 0)
	{
		Out.Problem = TEXT("its triangles could not be read");
		return false;
	}

	// The triangles of each bone, by material. A triangle rides the bone of its first corner (the body is rigid: every corner has the same one).
	TMap<int32, FName> MaterialNames;
	const TArray<int32> MaterialOf = MaterialOfTriangles(LOD, MaterialNames);
	TMap<int32, FBonePiece> PieceOfBone;
	for (int32 Triangle = 0; Triangle < LOD.NumTriangles(); ++Triangle)
	{
		const int32 Bone = BoneOfVertex(LOD, static_cast<int32>(LOD.Indices[Triangle * 3]));
		FBonePiece& Piece = PieceOfBone.FindOrAdd(Bone);
		Piece.Bone = Bone;
		Piece.ByMaterial.FindOrAdd(MaterialOf[Triangle]).Add(Triangle);
		++Piece.Triangles;
	}
	TArray<int32> Bones;
	PieceOfBone.GetKeys(Bones);
	Bones.Sort();
	for (const int32 Bone : Bones)
	{
		MakeDescription(LOD, PieceOfBone[Bone]);
	}

	FScopedSlowTask Slow(static_cast<float>(Bones.Num() * MaxAttempts), Progress ? *Progress
		: FText::Format(LOCTEXT("Reducing", "Reducing your vehicle's body from {0} to about {1} triangles so it fits (this can take a few minutes)..."),
			FText::AsNumber(Out.Before), FText::AsNumber(MaxTriangles)));
	Slow.MakeDialogDelayed(0.5f, /*bShowCancelButton*/ true);

	// What a try makes, kept until one fits.
	struct FReduced
	{
		TArray<FVector3f> Positions;
		TArray<FVector3f> Normals;
		TArray<FVector2f> UVs;
		TArray<uint16> Bone;
		/** Material slot index to the new indices. */
		TMap<int32, TArray<uint32>> Indices;
		int32 Triangles = 0;
	};
	FReduced Best;
	float Aim = static_cast<float>(MaxTriangles) * TargetShare;
	float Ratio = Aim / static_cast<float>(Out.Before);
	for (int32 Attempt = 0; Attempt < MaxAttempts; ++Attempt)
	{
		FReduced Made;
		for (const int32 Bone : Bones)
		{
			Slow.EnterProgressFrame(1.f);
			if (Slow.ShouldCancel())
			{
				Out.bCanceled = true;
				Out.Problem = TEXT("it was canceled");
				return false;
			}
			FBonePiece& Piece = PieceOfBone[Bone];
			const FMeshDescription* Source = &Piece.Description;
			FMeshDescription Reduced;
			if (Piece.Triangles >= SmallestWorthReducing)
			{
				const int32 Floor = FMath::Min(Piece.Triangles, 16 * FMath::Max(1, Piece.ByMaterial.Num()));
				const int32 Target = FMath::Clamp(FMath::CeilToInt(static_cast<float>(Piece.Triangles) * Ratio), Floor, Piece.Triangles);
				FString Why;
				if (Target < Piece.Triangles && FMantleMeshReduce::ReduceDescription(Piece.Description, Target, Target * 2, Reduced, Why))
				{
					Source = &Reduced;
				}
			}
			FStaticMeshConstAttributes Attributes(*Source);
			TVertexAttributesConstRef<FVector3f> Positions = Attributes.GetVertexPositions();
			TVertexInstanceAttributesConstRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
			TVertexInstanceAttributesConstRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
			TPolygonGroupAttributesConstRef<FName> SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
			TMap<FVertexInstanceID, uint32> NewIndex;
			for (const FTriangleID Triangle : Source->Triangles().GetElementIDs())
			{
				const int32* Material = Piece.MaterialOfSlotName.Find(SlotNames[Source->GetTrianglePolygonGroup(Triangle)]);
				if (!Material)
				{
					continue;
				}
				TArray<uint32>& Indices = Made.Indices.FindOrAdd(*Material);
				for (const FVertexInstanceID Corner : Source->GetTriangleVertexInstances(Triangle))
				{
					uint32* Found = NewIndex.Find(Corner);
					if (!Found)
					{
						Found = &NewIndex.Add(Corner, static_cast<uint32>(Made.Positions.Num()));
						Made.Positions.Add(Positions[Source->GetVertexInstanceVertex(Corner)]);
						Made.Normals.Add(Normals[Corner]);
						Made.UVs.Add(UVs.Get(Corner, 0));
						Made.Bone.Add(static_cast<uint16>(Bone));
					}
					Indices.Add(*Found);
				}
				++Made.Triangles;
			}
		}
		UE_LOG(LogTemp, Log, TEXT("MANTLE_VEHICLE_REDUCE attempt %d of %d: %d triangles to %d (limit %d)"), Attempt + 1, MaxAttempts, Out.Before, Made.Triangles, MaxTriangles);
		const bool bFits = Made.Triangles > 0 && Made.Triangles <= MaxTriangles;
		if (Best.Triangles == 0 || Made.Triangles < Best.Triangles)
		{
			Best = MoveTemp(Made);
		}
		if (bFits)
		{
			break;
		}
		// Over: aim lower by how far over it came out.
		Aim *= (static_cast<float>(MaxTriangles) * TargetShare) / static_cast<float>(FMath::Max(Best.Triangles, 1));
		Ratio = Aim / static_cast<float>(Out.Before);
	}
	if (Best.Triangles == 0 || Best.Triangles > MaxTriangles)
	{
		Out.Problem = FString::Printf(TEXT("the reducer left %d triangles and the most a vehicle may have is %d"), Best.Triangles, MaxTriangles);
		return false;
	}

	// The body, rebuilt: sections in material order, every vertex on its bone whole.
	FMantleSkinnedMeshLOD& Target = Body.LODs[0];
	TArray<FMantleSkinnedMeshSection> Sections;
	TArray<uint32> Indices;
	TArray<int32> Materials;
	Best.Indices.GetKeys(Materials);
	Materials.Sort();
	for (const int32 Material : Materials)
	{
		FMantleSkinnedMeshSection& Section = Sections.AddDefaulted_GetRef();
		Section.MaterialIndex = Material;
		Section.FirstIndex = Indices.Num();
		Section.IndexCount = Best.Indices[Material].Num();
		if (const FName* Name = MaterialNames.Find(Material))
		{
			Section.MaterialName = *Name;
		}
		Indices.Append(Best.Indices[Material]);
	}
	Target.Positions = MoveTemp(Best.Positions);
	Target.Normals = MoveTemp(Best.Normals);
	Target.UVs = MoveTemp(Best.UVs);
	Target.Tangents.Reset();
	Target.Indices = MoveTemp(Indices);
	Target.Sections = MoveTemp(Sections);
	Target.NumInfluences = 1;
	Target.InfluenceBones = MoveTemp(Best.Bone);
	Target.InfluenceWeights.Init(255, Target.Positions.Num());
	Extras = FMantleVehicleBodyExtras();
	Out.After = Target.NumTriangles();
	Out.bReduced = true;
	return true;
}

#undef LOCTEXT_NAMESPACE
