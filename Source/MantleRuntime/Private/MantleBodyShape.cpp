// Copyright Joshua Eger. All Rights Reserved.
#include "MantleBodyShape.h"

#include "MantleBodyTopologyData.h"
#include "MantleRuntime.h"
#include "Engine/SkeletalMesh.h"
#include "ReferenceSkeleton.h"

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleBodyShapePrivate
{
	bool RejectShape(FString& OutError, const FString& Error)
	{
		OutError = Error;
		return false;
	}
}

FMantleReferencePose FMantleReferencePose::FromReferenceSkeleton(const FReferenceSkeleton& Skeleton)
{
	FMantleReferencePose Pose;
	const TArray<FMeshBoneInfo>& BoneInfo = Skeleton.GetRawRefBoneInfo();
	const TArray<FTransform>& BonePose = Skeleton.GetRawRefBonePose();
	Pose.Bones.SetNum(BoneInfo.Num());
	Pose.LocalTransforms.SetNum(BoneInfo.Num());
	for (int32 BoneIndex = 0; BoneIndex < BoneInfo.Num(); ++BoneIndex)
	{
		Pose.Bones[BoneIndex].Name = BoneInfo[BoneIndex].Name;
		Pose.Bones[BoneIndex].ParentIndex = BoneInfo[BoneIndex].ParentIndex;
		Pose.LocalTransforms[BoneIndex] = BonePose.IsValidIndex(BoneIndex) ? BonePose[BoneIndex] : FTransform::Identity;
	}
	return Pose;
}

FMantleReferencePose FMantleReferencePose::FromSkinnedMesh(const FMantleSkinnedMesh& Mesh)
{
	FMantleReferencePose Pose;
	Pose.Bones = Mesh.Bones;
	Pose.LocalTransforms = Mesh.ReferencePose;
	return Pose;
}

bool FMantleReferencePose::Validate(FString& OutError) const
{
	using namespace MantleBodyShapePrivate;

	const int32 NumBones = Bones.Num();
	if (NumBones == 0 || NumBones > MAX_uint16)
	{
		return RejectShape(OutError, FString::Printf(TEXT("the reference pose has %d bones (1 to %d)"), NumBones, static_cast<int32>(MAX_uint16)));
	}
	if (LocalTransforms.Num() != NumBones)
	{
		return RejectShape(OutError, FString::Printf(TEXT("the reference pose has %d transforms for %d bones"), LocalTransforms.Num(), NumBones));
	}
	TSet<FName> Names;
	Names.Reserve(NumBones);
	for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
	{
		const FMantleSkinnedMeshBone& Bone = Bones[BoneIndex];
		bool bDuplicate = false;
		Names.Add(Bone.Name, &bDuplicate);
		if (Bone.Name.IsNone() || bDuplicate)
		{
			return RejectShape(OutError, FString::Printf(TEXT("reference pose bone %d (%s) has no name or appears twice"), BoneIndex, *Bone.Name.ToString()));
		}
		const bool bParentValid = BoneIndex == 0
			? Bone.ParentIndex == INDEX_NONE
			: (Bone.ParentIndex >= 0 && Bone.ParentIndex < BoneIndex);
		if (!bParentValid)
		{
			return RejectShape(OutError, FString::Printf(TEXT("reference pose bone %d (%s) has parent %d: the table must be parent-first with one root"),
				BoneIndex, *Bone.Name.ToString(), Bone.ParentIndex));
		}
		const FTransform& Local = LocalTransforms[BoneIndex];
		if (Local.ContainsNaN() || !Local.GetRotation().IsNormalized())
		{
			return RejectShape(OutError, FString::Printf(TEXT("reference pose bone %d (%s) has a non-finite transform or a rotation that is not unit length"),
				BoneIndex, *Bone.Name.ToString()));
		}
	}
	return true;
}

int64 FMantleBodyShape::GetWeightsBytes() const
{
	return static_cast<int64>(Weights.SampleIndices.Num()) * sizeof(int32)
		+ static_cast<int64>(Weights.SampleRestPositions.Num()) * sizeof(FVector3f)
		+ static_cast<int64>(Weights.InterpolationWeights.Num()) * sizeof(float);
}

bool FMantleBodyShape::ValidateWeights(FString& OutError) const
{
	using namespace MantleBodyShapePrivate;

	const int32 S = Weights.SampleIndices.Num();
	if (S == 0)
	{
		return RejectShape(OutError, TEXT("the shape has no RBF weights"));
	}
	if (Weights.SampleRestPositions.Num() != S)
	{
		return RejectShape(OutError, FString::Printf(TEXT("the weights have %d sample indices and %d rest positions"), S, Weights.SampleRestPositions.Num()));
	}
	const int64 Entries = static_cast<int64>(S + 4) * (S + 4);
	if (Weights.InterpolationWeights.Num() != Entries)
	{
		return RejectShape(OutError, FString::Printf(TEXT("the weights have %d interpolation weights; %d samples need (%d + 4)^2 = %lld"),
			Weights.InterpolationWeights.Num(), S, S, Entries));
	}
	for (int32 Sample = 0; Sample < S; ++Sample)
	{
		const int32 Index = Weights.SampleIndices[Sample];
		if (!Positions.IsValidIndex(Index))
		{
			return RejectShape(OutError, FString::Printf(TEXT("sample %d names vertex %d, out of range for the shape's %d positions"), Sample, Index, Positions.Num()));
		}
		if (Weights.SampleRestPositions[Sample] != Positions[Index])
		{
			return RejectShape(OutError, FString::Printf(TEXT("sample %d's rest position is not the shape's vertex %d (weights made for another shape)"), Sample, Index));
		}
	}
	for (const float Weight : Weights.InterpolationWeights)
	{
		if (!FMath::IsFinite(Weight))
		{
			return RejectShape(OutError, TEXT("the interpolation weights are not all finite (a singular RBF system?)"));
		}
	}
	return true;
}

int64 FMantleBodyShapeNeck::GetBytes() const
{
	return static_cast<int64>(Ids.Num()) * sizeof(int32) + static_cast<int64>(Positions.Num()) * sizeof(FVector3f)
		+ static_cast<int64>(Weights.SampleIndices.Num()) * sizeof(int32) + static_cast<int64>(Weights.SampleRestPositions.Num()) * sizeof(FVector3f)
		+ static_cast<int64>(Weights.InterpolationWeights.Num()) * sizeof(float);
}

FMantleBodyShape FMantleBodyShape::MakeNeckShape() const
{
	FMantleBodyShape Out;
	if (!Neck.IsSet())
	{
		return Out;
	}
	Out.Positions.Reserve(Positions.Num() + Neck.Positions.Num());
	Out.Positions.Append(Positions);
	Out.Positions.Append(Neck.Positions);
	Out.ReferencePose = ReferencePose;
	Out.Weights = Neck.Weights;
	Out.PositionsHash = UMantleBodyTopologyData::ComputePositionsHash(Out.Positions);
	return Out;
}

bool FMantleBodyShape::ValidateNeck(FString& OutError) const
{
	using namespace MantleBodyShapePrivate;

	if (Neck.Method == EMantleNeckMethod::None && Neck.Ids.Num() == 0 && Neck.Positions.Num() == 0 && Neck.Weights.SampleIndices.Num() == 0)
	{
		return true;
	}
	if (!Neck.IsSet())
	{
		return RejectShape(OutError, TEXT("the neck is incomplete (a method, samples and weights, or none of them)"));
	}
	if (Neck.Ids.Num() != Neck.Positions.Num())
	{
		return RejectShape(OutError, FString::Printf(TEXT("the neck has %d ids for %d samples"), Neck.Ids.Num(), Neck.Positions.Num()));
	}
	for (int32 Index = 0; Index < Neck.Ids.Num(); ++Index)
	{
		const int32 Id = Neck.Ids[Index];
		if (Id < 0 || (Index > 0 && Id <= Neck.Ids[Index - 1]))
		{
			return RejectShape(OutError, TEXT("the neck's ids are not ascending"));
		}
		if (Neck.Method == EMantleNeckMethod::Rays && Id >= FMantleNeckSamples::NumRays())
		{
			return RejectShape(OutError, FString::Printf(TEXT("neck ray %d is not in the family of %d"), Id, FMantleNeckSamples::NumRays()));
		}
	}
	for (const FVector3f& Position : Neck.Positions)
	{
		if (Position.ContainsNaN())
		{
			return RejectShape(OutError, TEXT("a neck position is not a finite number"));
		}
	}
	FString Invalid;
	if (!MakeNeckShape().ValidateWeights(Invalid))
	{
		return RejectShape(OutError, FString::Printf(TEXT("the neck's weights: %s"), *Invalid));
	}
	return true;
}

bool FMantleBodyShape::FromSkeletalMesh(const USkeletalMesh* Source, const int32 ReferenceVertexCount, const uint32 ReferenceUVHash,
	FMantleBodyShape& Out, int32* OutLODIndex, FString* OutError)
{
	Out = FMantleBodyShape();
	if (OutLODIndex)
	{
		*OutLODIndex = INDEX_NONE;
	}
	auto Refuse = [OutError](const FString& Error)
	{
		UE_LOG(LogMantle, Error, TEXT("MANTLE_GARMENTFIT FAIL shape: %s"), *Error);
		if (OutError)
		{
			*OutError = Error;
		}
		return false;
	};
	if (!Source)
	{
		return Refuse(TEXT("no source mesh"));
	}

	FString Seen;
	const int32 NumLODs = UMantleBodyTopologyData::GetNumLODs(Source);
	for (int32 LODIndex = 0; LODIndex < NumLODs; ++LODIndex)
	{
		FMantleBodyLODTopology Topology;
		TArray<FVector3f> LODPositions;
		FString Error;
		if (!UMantleBodyTopologyData::ReadLOD(Source, LODIndex, Topology, &LODPositions, nullptr, &Error))
		{
			Seen += FString::Printf(TEXT(" LOD%d unreadable (%s);"), LODIndex, *Error);
			continue;
		}
		Seen += FString::Printf(TEXT(" LOD%d %d v %s;"), LODIndex, Topology.VertexCount, *UMantleBodyTopologyData::FormatHash(Topology.UVHash));
		if (Topology.VertexCount != ReferenceVertexCount || Topology.UVHash != ReferenceUVHash)
		{
			continue;
		}
		Out.Positions = MoveTemp(LODPositions);
		Out.PositionsHash = Topology.PositionsHash;
		Out.ReferencePose = FMantleReferencePose::FromReferenceSkeleton(Source->GetRefSkeleton());
		if (OutLODIndex)
		{
			*OutLODIndex = LODIndex;
		}
		return true;
	}
	return Refuse(FString::Printf(TEXT("%s has no LOD with the reference topology (%d vertices, uvhash %s):%s"),
		*Source->GetName(), ReferenceVertexCount, *UMantleBodyTopologyData::FormatHash(ReferenceUVHash), *Seen));
}
