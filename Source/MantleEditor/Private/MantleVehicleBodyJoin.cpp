// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleBodyJoin.h"

#include "MantleVehicleChassisMesh.h"
#include "MantleVehicleChassisSkin.h"

#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"

namespace MantleVehicleBodyJoinPrivate
{
	/** A slot name not used by Materials: Wanted, else Wanted_2, Wanted_3 and so on. */
	FName UniqueSlotName(const TArray<FSkeletalMaterial>& Materials, const FName Wanted)
	{
		const FString Base = Wanted.IsNone() ? FString(TEXT("Piece")) : Wanted.ToString();
		FName Name(*Base);
		for (int32 Counter = 2; Materials.ContainsByPredicate([Name](const FSkeletalMaterial& Slot) { return Slot.MaterialSlotName == Name; }); ++Counter)
		{
			Name = FName(*FString::Printf(TEXT("%s_%d"), *Base, Counter));
		}
		return Name;
	}

	/** The triangles drawn with one material slot, in the order they were met. */
	struct FBucket
	{
		int32 Slot = 0;
		TArray<uint32> Indices;
	};

	FBucket& BucketOf(TArray<FBucket>& Buckets, const int32 Slot)
	{
		for (FBucket& Bucket : Buckets)
		{
			if (Bucket.Slot == Slot)
			{
				return Bucket;
			}
		}
		FBucket& Added = Buckets.AddDefaulted_GetRef();
		Added.Slot = Slot;
		return Added;
	}
}

bool FMantleVehicleBodyJoin::Join(FMantleSkinnedMesh& Body, TArray<FSkeletalMaterial>& Materials, const TConstArrayView<FPiece> Pieces, FString& OutError)
{
	using namespace MantleVehicleBodyJoinPrivate;
	if (Body.LODs.Num() == 0 || Body.LODs[0].NumVertices() == 0)
	{
		OutError = TEXT("the body has no geometry to join the pieces to");
		return false;
	}
	for (const FPiece& Piece : Pieces)
	{
		if (!Body.Bones.IsValidIndex(Piece.Bone))
		{
			OutError = TEXT("a piece rides a bone the body does not have");
			return false;
		}
		const FMantleSkinnedMeshLOD& Source = Piece.LOD;
		if (Source.NumVertices() == 0 || Source.NumTriangles() == 0 || Source.Normals.Num() != Source.NumVertices() || Source.UVs.Num() != Source.NumVertices())
		{
			OutError = TEXT("a piece has no geometry that can be read");
			return false;
		}
	}

	// Worked on copies, so a failure leaves Body as it was.
	FMantleSkinnedMesh Joined = Body;
	TArray<FSkeletalMaterial> JoinedMaterials = Materials;
	FMantleSkinnedMeshLOD& LOD = Joined.LODs[0];
	const int32 Influences = FMath::Max(LOD.NumInfluences, 1);

	TArray<FBucket> Buckets;
	for (const FMantleSkinnedMeshSection& Section : LOD.Sections)
	{
		FBucket& Bucket = BucketOf(Buckets, Section.MaterialIndex);
		for (int32 Index = Section.FirstIndex; Index < Section.FirstIndex + Section.IndexCount; ++Index)
		{
			Bucket.Indices.Add(LOD.Indices[Index]);
		}
	}

	bool bTangents = LOD.Tangents.Num() == LOD.NumVertices();
	for (const FPiece& Piece : Pieces)
	{
		const FMantleSkinnedMeshLOD& Source = Piece.LOD;

		// Each of the piece's slots: the slot of the same material, else a new one.
		TArray<int32> SlotOf;
		for (const FSkeletalMaterial& Slot : Piece.Materials)
		{
			int32 Found = Slot.MaterialInterface ? JoinedMaterials.IndexOfByPredicate([&Slot](const FSkeletalMaterial& Have) { return Have.MaterialInterface == Slot.MaterialInterface; }) : INDEX_NONE;
			if (Found == INDEX_NONE)
			{
				Found = JoinedMaterials.Num();
				const FName SlotName = UniqueSlotName(JoinedMaterials, Slot.MaterialSlotName);
				JoinedMaterials.Add(FSkeletalMaterial(Slot.MaterialInterface, SlotName, SlotName));
			}
			SlotOf.Add(Found);
		}

		const uint32 First = static_cast<uint32>(LOD.NumVertices());
		const bool bMirrored = Piece.Placement.GetDeterminant() < 0.0;
		const FMatrix NormalMatrix = Piece.Placement.ToInverseMatrixWithScale().GetTransposed();
		bTangents = bTangents && Source.Tangents.Num() == Source.NumVertices();
		for (int32 Vertex = 0; Vertex < Source.NumVertices(); ++Vertex)
		{
			LOD.Positions.Add(FVector3f(Piece.Placement.TransformPosition(FVector(Source.Positions[Vertex]))));
			LOD.Normals.Add(FVector3f(FVector(NormalMatrix.TransformVector(FVector(Source.Normals[Vertex]))).GetSafeNormal()));
			LOD.UVs.Add(Source.UVs[Vertex]);
			if (bTangents)
			{
				const FVector4f& Tangent = Source.Tangents[Vertex];
				const FVector3f Turned(Piece.Placement.TransformVector(FVector(Tangent.X, Tangent.Y, Tangent.Z)).GetSafeNormal());
				LOD.Tangents.Add(FVector4f(Turned.X, Turned.Y, Turned.Z, bMirrored ? -Tangent.W : Tangent.W));
			}
			// Whole to the piece's bone; the other slots name a real bone with no weight.
			for (int32 Slot = 0; Slot < Influences; ++Slot)
			{
				LOD.InfluenceBones.Add(static_cast<uint16>(Slot == 0 ? Piece.Bone : 0));
				LOD.InfluenceWeights.Add(Slot == 0 ? 255 : 0);
			}
		}
		for (const FMantleSkinnedMeshSection& Section : Source.Sections)
		{
			if (!SlotOf.IsValidIndex(Section.MaterialIndex))
			{
				OutError = TEXT("a piece has a section with a material it does not list");
				return false;
			}
			FBucket& Bucket = BucketOf(Buckets, SlotOf[Section.MaterialIndex]);
			const int32 End = FMath::Min(Section.FirstIndex + Section.IndexCount, Source.Indices.Num());
			for (int32 Index = Section.FirstIndex; Index + 3 <= End; Index += 3)
			{
				// A mirrored placement turns the faces inside out; swapping two corners turns them back.
				Bucket.Indices.Add(First + Source.Indices[Index]);
				Bucket.Indices.Add(First + Source.Indices[bMirrored ? Index + 2 : Index + 1]);
				Bucket.Indices.Add(First + Source.Indices[bMirrored ? Index + 1 : Index + 2]);
			}
		}
	}
	if (!bTangents)
	{
		LOD.Tangents.Reset();
	}

	// One section for each slot.
	LOD.Indices.Reset();
	LOD.Sections.Reset();
	for (const FBucket& Bucket : Buckets)
	{
		if (Bucket.Indices.Num() == 0)
		{
			continue;
		}
		FMantleSkinnedMeshSection& Section = LOD.Sections.AddDefaulted_GetRef();
		Section.MaterialIndex = Bucket.Slot;
		Section.FirstIndex = LOD.Indices.Num();
		Section.IndexCount = Bucket.Indices.Num();
		Section.MaterialName = JoinedMaterials.IsValidIndex(Bucket.Slot) ? JoinedMaterials[Bucket.Slot].MaterialSlotName : FName();
		LOD.Indices.Append(Bucket.Indices);
	}
	if (!Joined.Validate(OutError))
	{
		return false;
	}
	Body = MoveTemp(Joined);
	Materials = MoveTemp(JoinedMaterials);
	return true;
}

bool FMantleVehicleBodyJoin::ReadPiece(const FMantleVehiclePlacedMesh& Placed, FPiece& Out, FString& OutError)
{
	Out = FPiece();
	UStaticMesh* Mesh = const_cast<UStaticMesh*>(Placed.Found.Mesh);
	FMantleVehicleModel Model;
	if (!Mesh || !FMantleVehicleModel::Read(Mesh, Model, OutError))
	{
		if (!Mesh)
		{
			OutError = TEXT("a piece has no mesh");
		}
		return false;
	}
	Out.LOD = MoveTemp(Model.LOD);
	Out.Materials = MoveTemp(Model.Materials);
	for (int32 Slot = 0; Slot < Out.Materials.Num(); ++Slot)
	{
		UMaterialInterface* Override = Placed.Found.Overrides.IsValidIndex(Slot) ? Placed.Found.Overrides[Slot] : nullptr;
		if (Override)
		{
			Out.Materials[Slot].MaterialInterface = Override;
		}
	}
	Out.Bone = Placed.Bone;
	Out.Placement = Placed.Placement;
	return true;
}

USkeletalMesh* FMantleVehicleBodyJoin::Make(const USkeletalMesh& Body, const TConstArrayView<FMantleVehiclePlacedMesh> Pieces, const FString& PackagePath, const FString& Name,
	FString& OutError, int32* OutTriangles)
{
	FMantleSkinnedMesh Skin;
	TArray<FSkeletalMaterial> Materials;
	if (!FMantleSkinnedMesh::ExtractFromSkeletalMesh(&Body, Skin, &OutError, &Materials))
	{
		return nullptr;
	}
	TArray<FPiece> Read;
	for (const FMantleVehiclePlacedMesh& Placed : Pieces)
	{
		if (!ReadPiece(Placed, Read.AddDefaulted_GetRef(), OutError))
		{
			OutError = FString::Printf(TEXT("%s: %s"), Placed.Found.Mesh ? *Placed.Found.Mesh->GetName() : TEXT("a piece"), *OutError);
			return nullptr;
		}
	}
	if (!Join(Skin, Materials, Read, OutError))
	{
		return nullptr;
	}
	if (OutTriangles)
	{
		*OutTriangles = Skin.LODs[0].NumTriangles();
	}
	return FMantleVehicleChassisMesh::Create(Skin, PackagePath, Name, OutError, &Materials);
}
