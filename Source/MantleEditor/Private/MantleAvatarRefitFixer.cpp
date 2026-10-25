// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAvatarRefitFixer.h"
#include "MantleAvatarContract.h"
#include "MantleAvatarConverter.h"
#include "MantleAvatarReposeFixer.h"
#include "MantleAvatarRigMapFixer.h"
#include "MantleBodyTopologyData.h"
#include "MantleCosmeticExport.h"
#include "MantleCosmeticValidation.h"
#include "MantleGarmentClearance.h"
#include "MantleRBFInterpolation.h"
#include "MantleMetaHumanBuild.h"
#include "MantleReferenceBodies.h"
#include "MantleRuntime.h"

#include "Async/ParallelFor.h"
#include "AssetToolsModule.h"
#include "Engine/SkeletalMesh.h"
#include "IAssetTools.h"
#include "MeshDescription.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "ReferenceSkeleton.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshModel.h"
#include "SkeletalMeshAttributes.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleAvatarRefitPrivate
{
	/** A mesh's raw reference skeleton, bone and component space. */
	struct FRefitSkeleton
	{
		TArray<FName> Names;
		TArray<int32> Parents;
		TArray<FTransform> Local;
		TArray<FTransform> Component;
		TMap<FName, int32> IndexOf;
	};

	void RefitReadSkeleton(const USkeletalMesh& Mesh, FRefitSkeleton& Out)
	{
		const FReferenceSkeleton& Reference = Mesh.GetRefSkeleton();
		const TArray<FMeshBoneInfo>& Info = Reference.GetRawRefBoneInfo();
		const TArray<FTransform>& Pose = Reference.GetRawRefBonePose();
		Out = FRefitSkeleton();
		for (int32 Bone = 0; Bone < Info.Num(); ++Bone)
		{
			Out.Names.Add(Info[Bone].Name);
			Out.Parents.Add(Info[Bone].ParentIndex);
			Out.Local.Add(Pose.IsValidIndex(Bone) ? Pose[Bone] : FTransform::Identity);
			Out.IndexOf.Add(Info[Bone].Name, Bone);
		}
		FMantleAvatarRepose::ComposeComponent(Out.Parents, Out.Local, Out.Component);
	}

	/** IK targets and virtual bones are placed for animation, not by the body's shape: never landmarks. */
	bool RefitIsLandmarkBone(const FName Name)
	{
		const FString Text = Name.ToString();
		return !Text.StartsWith(TEXT("ik_"), ESearchCase::IgnoreCase) && !Text.StartsWith(TEXT("VB "), ESearchCase::IgnoreCase);
	}

	/**
	 * Farthest-point sampling: the first NumSeeds points always, then the point farthest from every one taken, until
	 * Count are taken. Indices into Points, in the order taken.
	 */
	TArray<int32> RefitFarthestPoints(TConstArrayView<FVector3f> Points, const int32 Count, const int32 NumSeeds)
	{
		TArray<int32> Out;
		const int32 NumPoints = Points.Num();
		if (NumPoints == 0 || Count <= 0)
		{
			return Out;
		}
		if (Count >= NumPoints)
		{
			for (int32 Index = 0; Index < NumPoints; ++Index)
			{
				Out.Add(Index);
			}
			return Out;
		}
		TArray<float> Nearest;
		Nearest.Init(MAX_FLT, NumPoints);
		TArray<bool> Taken;
		Taken.Init(false, NumPoints);
		auto Take = [&Out, &Nearest, &Taken, Points, NumPoints](const int32 Index)
		{
			Out.Add(Index);
			Taken[Index] = true;
			const FVector3f Point = Points[Index];
			for (int32 Other = 0; Other < NumPoints; ++Other)
			{
				Nearest[Other] = FMath::Min(Nearest[Other], FVector3f::DistSquared(Point, Points[Other]));
			}
		};
		for (int32 Seed = 0; Seed < FMath::Min(NumSeeds, NumPoints); ++Seed)
		{
			Take(Seed);
		}
		if (Out.Num() == 0)
		{
			Take(0);
		}
		while (Out.Num() < Count)
		{
			int32 Best = INDEX_NONE;
			float BestDistance = -1.0f;
			for (int32 Index = 0; Index < NumPoints; ++Index)
			{
				if (!Taken[Index] && Nearest[Index] > BestDistance)
				{
					Best = Index;
					BestDistance = Nearest[Index];
				}
			}
			if (Best == INDEX_NONE)
			{
				break;
			}
			Take(Best);
		}
		return Out;
	}

	/** Adds the landmark pair unless From already has a landmark within MinApart cm of A (a coincident pair makes the RBF system singular). */
	void RefitAddLandmark(TArray<FVector3f>& From, TArray<FVector3f>& To, const FVector3f& A, const FVector3f& B, const float MinApart)
	{
		for (const FVector3f& Existing : From)
		{
			if (FVector3f::DistSquared(Existing, A) < MinApart * MinApart)
			{
				return;
			}
		}
		From.Add(A);
		To.Add(B);
	}

	/** The neck and head joints: the dense path adds them, because a body mesh ends at the neck and hair sits above it. */
	const TArray<FName>& RefitHeadBones()
	{
		static const TArray<FName> Bones = { FName(TEXT("neck_01")), FName(TEXT("neck_02")), FName(TEXT("head")) };
		return Bones;
	}

	/** The linear blend of Skin (one per reference bone) for one imported-model vertex, and whether any of its bones moved. */
	bool RefitBlend(const FSoftSkinVertex& Vertex, TConstArrayView<FBoneIndexType> BoneMap, TConstArrayView<FMatrix> Skin, TConstArrayView<bool> Moved,
		FMatrix& OutBlend)
	{
		FMatrix Sum(ForceInitToZero);
		double Total = 0.0;
		bool bAnyMoved = false;
		for (int32 Influence = 0; Influence < MAX_TOTAL_INFLUENCES; ++Influence)
		{
			const double Weight = static_cast<double>(Vertex.InfluenceWeights[Influence]);
			if (Weight <= 0.0)
			{
				continue;
			}
			const int32 Local = Vertex.InfluenceBones[Influence];
			const int32 Bone = BoneMap.IsValidIndex(Local) ? static_cast<int32>(BoneMap[Local]) : INDEX_NONE;
			if (!Skin.IsValidIndex(Bone))
			{
				continue;
			}
			Sum += Skin[Bone] * Weight;
			Total += Weight;
			bAnyMoved |= Moved[Bone];
		}
		if (!bAnyMoved || Total <= 0.0)
		{
			return false;
		}
		Sum *= 1.0 / Total;
		OutBlend = Sum;
		return true;
	}
}

const TCHAR* LexToString(const EMantleRefitCorrespondence Path)
{
	switch (Path)
	{
	case EMantleRefitCorrespondence::Dense:
		return TEXT("dense");
	case EMantleRefitCorrespondence::Joints:
		return TEXT("joints");
	default:
		return TEXT("identity");
	}
}

FString FMantleRefitPushOut::ToString() const
{
	FString Line = FString::Printf(TEXT("%d of %d under the skin (deepest %.2f cm), %d moved (largest %.2f cm), %d left under (%d deeper than %.0f cm, %d under their own shell, %d under cover further than %.0f cm from an opening)"),
		InsideBefore, Tracked, DeepestCm, Moved, MaxMoveCm, InsideAfter, TooDeep, FMantleAvatarRefit::MaxPushDepthCm, UnderShell, UnderCover, FMantleAvatarRefit::OpeningReachCm);
	if (SkinThroughBefore >= 0)
	{
		Line += FString::Printf(TEXT("; skin through it between vertices %d, covered to %d"), SkinThroughBefore, SkinThroughAfter);
	}
	return Detail.IsEmpty() ? Line : FString::Printf(TEXT("%s; %s"), *Line, *Detail);
}

FVector3f FMantleRefitPlan::Warp(const FVector3f& P) const
{
	return Path == EMantleRefitCorrespondence::Identity || Field.bIdentity ? P : Field.Evaluate(P);
}

bool FMantleRefitPlan::ExceedsMoveCap() const
{
	return !bOnBones && MaxMoveCm > FMantleAvatarRefit::MaxMoveCm;
}

const TArray<FName>& FMantleAvatarRefit::RequiredBones()
{
	static const TArray<FName> Bones = {
		FName(TEXT("pelvis")), FName(TEXT("spine_01")), FName(TEXT("spine_02")), FName(TEXT("spine_03")), FName(TEXT("spine_04")),
		FName(TEXT("spine_05")), FName(TEXT("neck_01")), FName(TEXT("head")) };
	return Bones;
}

FString FMantleAvatarRefit::NoReferenceBodyText()
{
	return FMantleReferenceBodies::UnavailableText(FString());
}

bool FMantleAvatarRefit::IsOverlaySlot(const FString& Slot)
{
	return Slot.Equals(TEXT("nails"), ESearchCase::CaseSensitive);
}

bool FMantleAvatarRefit::CheckBones(TConstArrayView<FName> GarmentBones, TConstArrayView<FName> Weighted, TConstArrayView<FName> BodyBones, FString& OutError,
	const bool bOverlay)
{
	for (const FName& Bone : RequiredBones())
	{
		if (!bOverlay && !GarmentBones.Contains(Bone))
		{
			OutError = FString::Printf(TEXT("the piece has no bone \"%s\", so it is not on the MetaHuman body skeleton"), *Bone.ToString());
			return false;
		}
		if (!BodyBones.Contains(Bone))
		{
			OutError = FString::Printf(TEXT("the body has no bone \"%s\", so it is not on the MetaHuman body skeleton"), *Bone.ToString());
			return false;
		}
	}
	for (const FName& Bone : Weighted)
	{
		if (!BodyBones.Contains(Bone))
		{
			OutError = FString::Printf(TEXT("bone \"%s\" carries weight and the MetaHuman body skeleton has no bone of that name"), *Bone.ToString());
			return false;
		}
	}
	return true;
}

TArray<FMantleFoldedBone> FMantleAvatarRefit::FoldBones(TConstArrayView<FName> Names, TConstArrayView<int32> Parents, TConstArrayView<FName> Weighted,
	TConstArrayView<FName> BodyBones)
{
	TArray<FMantleFoldedBone> Out;
	for (const FName& Bone : Weighted)
	{
		const int32 Index = Names.Find(Bone);
		if (Index == INDEX_NONE || BodyBones.Contains(Bone))
		{
			continue;
		}
		// The nearest ancestor the body has; the step bound only guards a malformed table.
		int32 Ancestor = Parents.IsValidIndex(Index) ? Parents[Index] : INDEX_NONE;
		for (int32 Steps = 0; Ancestor != INDEX_NONE && Names.IsValidIndex(Ancestor) && Steps <= Names.Num(); ++Steps)
		{
			if (BodyBones.Contains(Names[Ancestor]))
			{
				Out.Add(FMantleFoldedBone{ Bone, Names[Ancestor] });
				break;
			}
			Ancestor = Parents.IsValidIndex(Ancestor) ? Parents[Ancestor] : INDEX_NONE;
		}
	}
	return Out;
}

TArray<FName> FMantleAvatarRefit::WeightedBones(const USkeletalMesh& Mesh)
{
	const FReferenceSkeleton& Reference = Mesh.GetRefSkeleton();
	const int32 NumBones = Reference.GetRawBoneNum();
	TBitArray<> Weighted(false, NumBones);
	bool bAny = false;
	const FSkeletalMeshModel* Imported = Mesh.GetImportedModel();
	if (Imported && Imported->LODModels.Num() > 0)
	{
		for (const FSkelMeshSection& Section : Imported->LODModels[0].Sections)
		{
			for (const FSoftSkinVertex& Vertex : Section.SoftVertices)
			{
				for (int32 Influence = 0; Influence < MAX_TOTAL_INFLUENCES; ++Influence)
				{
					if (Vertex.InfluenceWeights[Influence] == 0)
					{
						continue;
					}
					const int32 Local = Vertex.InfluenceBones[Influence];
					const int32 Bone = Section.BoneMap.IsValidIndex(Local) ? static_cast<int32>(Section.BoneMap[Local]) : INDEX_NONE;
					if (Bone >= 0 && Bone < NumBones)
					{
						Weighted[Bone] = true;
						bAny = true;
					}
				}
			}
		}
	}
	TArray<FName> Out;
	for (int32 Bone = 0; Bone < NumBones; ++Bone)
	{
		if (!bAny || Weighted[Bone])
		{
			Out.Add(Reference.GetRawRefBoneInfo()[Bone].Name);
		}
	}
	return Out;
}

bool FMantleAvatarRefit::FindBindOffset(const USkeletalMesh& Garment, const USkeletalMesh& Body, const FMantleAvatarContract& Contract,
	FName& OutBone, double& OutCm, double& OutDegrees)
{
	using namespace MantleAvatarRefitPrivate;
	OutBone = NAME_None;
	OutCm = 0.0;
	OutDegrees = 0.0;
	FRefitSkeleton GarmentSkeleton;
	FRefitSkeleton BodySkeleton;
	RefitReadSkeleton(Garment, GarmentSkeleton);
	RefitReadSkeleton(Body, BodySkeleton);
	const TSet<FName> Weighted(WeightedBones(Garment));
	for (int32 Bone = 0; Bone < GarmentSkeleton.Names.Num(); ++Bone)
	{
		const FName Name = GarmentSkeleton.Names[Bone];
		const int32* InBody = BodySkeleton.IndexOf.Find(Name);
		if (!InBody || !Weighted.Contains(Name))
		{
			continue;
		}
		double Distance = 0.0;
		double Degrees = 0.0;
		if (!FMantleCosmeticChecks::WithinBindTolerance(GarmentSkeleton.Component[Bone], BodySkeleton.Component[*InBody], Contract, &Distance, &Degrees))
		{
			OutBone = Name;
			OutCm = Distance;
			OutDegrees = Degrees;
			return true;
		}
	}
	return false;
}

bool FMantleAvatarRefit::FindDenseCorrespondence(const USkeletalMesh& Source, const USkeletalMesh& Target, int32& OutSourceLOD, int32& OutTargetLOD,
	TArray<FVector3f>* OutSourcePositions, TArray<FVector3f>* OutTargetPositions)
{
	OutSourceLOD = INDEX_NONE;
	OutTargetLOD = INDEX_NONE;
	struct FRead
	{
		bool bOk = false;
		FMantleBodyLODTopology Topology;
		TArray<FVector3f> Positions;
		TArray<FVector2f> UVs;
	};
	auto ReadAll = [](const USkeletalMesh& Mesh)
	{
		const FMantleAvatarContract* Contract = FMantleAvatarContract::Get();
		TArray<FRead> Out;
		for (int32 LOD = 0; LOD < UMantleBodyTopologyData::GetNumLODs(&Mesh); ++LOD)
		{
			FRead& Read = Out.AddDefaulted_GetRef();
			bool bDescriptionOrder = false;
			if (Contract && FMantleCosmeticExport::HasReferenceTopology(&Mesh, LOD, *Contract, &bDescriptionOrder) && bDescriptionOrder)
			{
				// #620: a catalog reference body, or a body written in its order: its reference order is its mesh description's
				// (the editor build renumbered the render vertices), so it is read there.
				Read.bOk = FMantleCosmeticExport::ReadDescriptionVertices(&Mesh, LOD, Read.Positions, Read.UVs);
				Read.Topology.VertexCount = Read.Positions.Num();
				continue;
			}
			Read.bOk = UMantleBodyTopologyData::ReadLOD(&Mesh, LOD, Read.Topology, &Read.Positions, &Read.UVs);
		}
		return Out;
	};
	const TArray<FRead> SourceLODs = ReadAll(Source);
	const TArray<FRead> TargetLODs = ReadAll(Target);
	for (int32 TargetLOD = 0; TargetLOD < TargetLODs.Num(); ++TargetLOD)
	{
		const FRead& To = TargetLODs[TargetLOD];
		for (int32 SourceLOD = 0; To.bOk && SourceLOD < SourceLODs.Num(); ++SourceLOD)
		{
			const FRead& From = SourceLODs[SourceLOD];
			if (!From.bOk || From.Topology.VertexCount != To.Topology.VertexCount || From.UVs.Num() != To.UVs.Num())
			{
				continue;
			}
			bool bSame = true;
			for (int32 Vertex = 0; bSame && Vertex < From.UVs.Num(); ++Vertex)
			{
				bSame = FMath::Abs(From.UVs[Vertex].X - To.UVs[Vertex].X) <= DenseUVTolerance
					&& FMath::Abs(From.UVs[Vertex].Y - To.UVs[Vertex].Y) <= DenseUVTolerance;
			}
			if (!bSame)
			{
				continue;
			}
			OutSourceLOD = SourceLOD;
			OutTargetLOD = TargetLOD;
			if (OutSourcePositions)
			{
				*OutSourcePositions = From.Positions;
			}
			if (OutTargetPositions)
			{
				*OutTargetPositions = To.Positions;
			}
			return true;
		}
	}
	return false;
}

bool FMantleAvatarRefit::MakeField(TConstArrayView<FVector3f> From, TConstArrayView<FVector3f> To, FMantleWarpField& OutField, FString& OutError)
{
	OutField = FMantleWarpField();
	if (From.Num() != To.Num() || From.Num() < 4)
	{
		OutError = FString::Printf(TEXT("%d landmarks on the source body and %d on the authoring body (at least 4 each, as many on both)"), From.Num(), To.Num());
		return false;
	}
	// Every landmark is a sample: the engine's sampler takes them all when asked for all, in its own order.
	FMantleRBFInterpolationData Weights;
	FMantleRBFInterpolation::GenerateWeights(From, From.Num(), Weights);
	if (Weights.SampleIndices.Num() != From.Num())
	{
		OutError = FString::Printf(TEXT("the RBF sampler kept %d of %d landmarks"), Weights.SampleIndices.Num(), From.Num());
		return false;
	}
	OutField.SampleRestPositions = Weights.SampleRestPositions;
	OutField.TargetSamples.Reserve(Weights.SampleIndices.Num());
	for (const int32 Sample : Weights.SampleIndices)
	{
		OutField.TargetSamples.Add(To[Sample]);
	}
	FMantleRBFInterpolation::GenerateMeshSamples(OutField.TargetSamples, Weights, OutField.SampleDeformations);
	if (!OutField.IsValid())
	{
		OutError = TEXT("the RBF field could not be made from the landmarks");
		return false;
	}
	return true;
}

bool FMantleAvatarRefit::Plan(const USkeletalMesh& Garment, const USkeletalMesh* SourceBody, const USkeletalMesh& Target, const bool bAllowDense,
	FMantleRefitPlan& Out, FString& OutError, const bool bOverlay)
{
	using namespace MantleAvatarRefitPrivate;
	Out = FMantleRefitPlan();
	FRefitSkeleton GarmentSkeleton;
	FRefitSkeleton TargetSkeleton;
	FRefitSkeleton SourceSkeleton;
	RefitReadSkeleton(Garment, GarmentSkeleton);
	RefitReadSkeleton(Target, TargetSkeleton);
	if (SourceBody)
	{
		RefitReadSkeleton(*SourceBody, SourceSkeleton);
	}
	// A weighted bone the body lacks (a breast or cloth helper) with an ancestor the body has is folded into it, as the
	// upload folds it: it keeps its local transform under that ancestor below, so its vertices move as the ancestor's.
	TArray<FName> Weighted = WeightedBones(Garment);
	if (!bOverlay)
	{
		Out.Folded = FoldBones(GarmentSkeleton.Names, GarmentSkeleton.Parents, Weighted, TargetSkeleton.Names);
		for (const FMantleFoldedBone& Folded : Out.Folded)
		{
			Weighted.Remove(Folded.Bone);
			UE_LOG(LogMantle, Log, TEXT("MANTLE_AVATAR_REFIT fold %s: %s -> %s"), *Garment.GetName(), *Folded.Bone.ToString(), *Folded.Into.ToString());
		}
	}
	if (!CheckBones(GarmentSkeleton.Names, Weighted, TargetSkeleton.Names, OutError, bOverlay))
	{
		return false;
	}

	// The poses: shared bones at each body's, the garment's own extra bones keeping their local transforms. An overlay
	// piece's source pose is the authoring body's (bOnBones): each nail follows its own finger bone onto the body.
	Out.bOnBones = bOverlay;
	const int32 NumBones = GarmentSkeleton.Names.Num();
	Out.Names = GarmentSkeleton.Names;
	Out.Parents = GarmentSkeleton.Parents;
	Out.GarmentComponent = GarmentSkeleton.Component;
	Out.SourceComponent.SetNum(NumBones);
	Out.TargetComponent.SetNum(NumBones);
	Out.TargetLocal.SetNum(NumBones);
	for (int32 Bone = 0; Bone < NumBones; ++Bone)
	{
		const FName Name = GarmentSkeleton.Names[Bone];
		const int32 Parent = GarmentSkeleton.Parents[Bone];
		const FTransform& Local = GarmentSkeleton.Local[Bone];
		const bool bHasParent = Parent != INDEX_NONE && Parent < Bone;
		const int32* InSource = SourceBody ? SourceSkeleton.IndexOf.Find(Name) : nullptr;
		const int32* InTarget = TargetSkeleton.IndexOf.Find(Name);
		if (Out.bOnBones)
		{
			Out.SourceComponent[Bone] = InTarget ? TargetSkeleton.Component[*InTarget] : bHasParent ? Local * Out.SourceComponent[Parent] : Local;
		}
		else if (!SourceBody)
		{
			Out.SourceComponent[Bone] = GarmentSkeleton.Component[Bone];
		}
		else if (InSource)
		{
			Out.SourceComponent[Bone] = SourceSkeleton.Component[*InSource];
		}
		else
		{
			Out.SourceComponent[Bone] = bHasParent ? Local * Out.SourceComponent[Parent] : Local;
		}
		if (InTarget)
		{
			Out.TargetComponent[Bone] = TargetSkeleton.Component[*InTarget];
			++Out.NumShared;
		}
		else
		{
			Out.TargetComponent[Bone] = bHasParent ? Local * Out.TargetComponent[Parent] : Local;
		}
		Out.TargetLocal[Bone] = bHasParent ? Out.TargetComponent[Bone].GetRelativeTransform(Out.TargetComponent[Parent]) : Out.TargetComponent[Bone];
	}

	// The field from the source body's space to the authoring body's.
	TArray<FVector3f> From;
	TArray<FVector3f> To;
	if (SourceBody == &Target || Out.bOnBones)
	{
		Out.Path = EMantleRefitCorrespondence::Identity;
	}
	else
	{
		TArray<FVector3f> SourcePositions;
		TArray<FVector3f> TargetPositions;
		if (bAllowDense && SourceBody
			&& FindDenseCorrespondence(*SourceBody, Target, Out.SourceLOD, Out.TargetLOD, &SourcePositions, &TargetPositions))
		{
			Out.Path = EMantleRefitCorrespondence::Dense;
			// The neck and head joints first (always kept), then the body's vertices, farthest-point sampled.
			TArray<FVector3f> Candidates;
			TArray<FVector3f> CandidateTargets;
			for (const FName& Bone : RefitHeadBones())
			{
				const int32* InSource = SourceSkeleton.IndexOf.Find(Bone);
				const int32* InTarget = TargetSkeleton.IndexOf.Find(Bone);
				if (InSource && InTarget)
				{
					Candidates.Add(FVector3f(SourceSkeleton.Component[*InSource].GetLocation()));
					CandidateTargets.Add(FVector3f(TargetSkeleton.Component[*InTarget].GetLocation()));
				}
			}
			const int32 NumSeeds = Candidates.Num();
			Candidates.Append(SourcePositions);
			CandidateTargets.Append(TargetPositions);
			for (const int32 Index : RefitFarthestPoints(Candidates, NumSeeds + DensePoints, NumSeeds))
			{
				RefitAddLandmark(From, To, Candidates[Index], CandidateTargets[Index], 0.05f);
			}
		}
		else
		{
			Out.Path = EMantleRefitCorrespondence::Joints;
			// The joints of the source body (the garment's own pose without one) the authoring body has too, and two
			// points on each bone segment between two of them.
			const FRefitSkeleton& Landmarks = SourceBody ? SourceSkeleton : GarmentSkeleton;
			TArray<int32> TargetOf;
			TargetOf.Init(INDEX_NONE, Landmarks.Names.Num());
			TArray<FVector3f> JointFrom;
			TArray<FVector3f> JointTo;
			for (int32 Bone = 0; Bone < Landmarks.Names.Num(); ++Bone)
			{
				const int32* InTarget = TargetSkeleton.IndexOf.Find(Landmarks.Names[Bone]);
				if (!InTarget || !RefitIsLandmarkBone(Landmarks.Names[Bone]))
				{
					continue;
				}
				TargetOf[Bone] = *InTarget;
				RefitAddLandmark(JointFrom, JointTo, FVector3f(Landmarks.Component[Bone].GetLocation()),
					FVector3f(TargetSkeleton.Component[*InTarget].GetLocation()), 0.5f);
			}
			for (int32 Bone = 0; Bone < Landmarks.Names.Num(); ++Bone)
			{
				const int32 Parent = Landmarks.Parents[Bone];
				if (TargetOf[Bone] == INDEX_NONE || Parent == INDEX_NONE || !TargetOf.IsValidIndex(Parent) || TargetOf[Parent] == INDEX_NONE)
				{
					continue;
				}
				const FVector3f A0(Landmarks.Component[Parent].GetLocation());
				const FVector3f A1(Landmarks.Component[Bone].GetLocation());
				const FVector3f B0(TargetSkeleton.Component[TargetOf[Parent]].GetLocation());
				const FVector3f B1(TargetSkeleton.Component[TargetOf[Bone]].GetLocation());
				if (FVector3f::Dist(A0, A1) < 3.0f)
				{
					continue;
				}
				for (const float Along : { 1.0f / 3.0f, 2.0f / 3.0f })
				{
					RefitAddLandmark(JointFrom, JointTo, FMath::Lerp(A0, A1, Along), FMath::Lerp(B0, B1, Along), 0.5f);
				}
			}
			for (const int32 Index : RefitFarthestPoints(JointFrom, MaxJointLandmarks, 0))
			{
				From.Add(JointFrom[Index]);
				To.Add(JointTo[Index]);
			}
		}

		// Landmarks that sit on each other: the bodies are one shape and the warp is the identity.
		bool bSame = true;
		for (int32 Index = 0; bSame && Index < From.Num(); ++Index)
		{
			bSame = From[Index].Equals(To[Index], 0.001f);
		}
		if (bSame)
		{
			Out.Path = EMantleRefitCorrespondence::Identity;
		}
	}
	if (Out.Path == EMantleRefitCorrespondence::Identity)
	{
		Out.Field.bIdentity = true;
	}
	else
	{
		if (!MakeField(From, To, Out.Field, OutError))
		{
			return false;
		}
		Out.NumLandmarks = From.Num();
	}

	// The largest move, on the imported model every LOD is built from: the re-bind to the source pose, then the warp.
	TArray<FMatrix> Skin;
	TArray<bool> Moved;
	Skin.SetNum(NumBones);
	Moved.SetNum(NumBones);
	for (int32 Bone = 0; Bone < NumBones; ++Bone)
	{
		Skin[Bone] = Out.GarmentComponent[Bone].ToMatrixWithScale().Inverse() * Out.SourceComponent[Bone].ToMatrixWithScale();
		Moved[Bone] = !Out.GarmentComponent[Bone].Equals(Out.SourceComponent[Bone], 1.e-6);
	}
	const FSkeletalMeshModel* Imported = Garment.GetImportedModel();
	for (int32 LOD = 0; Imported && LOD < Imported->LODModels.Num(); ++LOD)
	{
		for (const FSkelMeshSection& Section : Imported->LODModels[LOD].Sections)
		{
			const TArray<FSoftSkinVertex>& Vertices = Section.SoftVertices;
			TArray<float> Moves;
			Moves.SetNumZeroed(Vertices.Num());
			const FMantleRefitPlan& ConstOut = Out;
			ParallelFor(Vertices.Num(), [&Vertices, &Moves, &Section, &Skin, &Moved, &ConstOut](const int32 Index)
			{
				const FSoftSkinVertex& Vertex = Vertices[Index];
				FMatrix Blend = FMatrix::Identity;
				const FVector3f Rebound = RefitBlend(Vertex, Section.BoneMap, Skin, Moved, Blend)
					? FVector3f(FVector(Blend.TransformPosition(FVector(Vertex.Position)))) : Vertex.Position;
				Moves[Index] = FVector3f::Dist(ConstOut.Warp(Rebound), Vertex.Position);
			});
			for (const float Move : Moves)
			{
				if (Move > Out.MaxMoveCm)
				{
					Out.MaxMoveCm = Move;
					Out.MaxMoveLOD = LOD;
				}
			}
		}
	}
	UE_LOG(LogMantle, Log, TEXT("MANTLE_AVATAR_REFIT plan %s from %s onto %s: %s, %d landmarks, %d of %d bones shared, max move %.2f cm (LOD %d)"),
		*Garment.GetName(), SourceBody ? *SourceBody->GetName() : TEXT("its own pose"), *Target.GetName(), LexToString(Out.Path), Out.NumLandmarks,
		Out.NumShared, NumBones, Out.MaxMoveCm, Out.MaxMoveLOD);
	return true;
}

bool FMantleAvatarRefit::RefitMeshDescription(FMeshDescription& Mesh, const FMantleRefitPlan& InPlan, double& OutMaxMoveCm, FString& OutError)
{
	const FMantleRefitPlan& Refit = InPlan;
	OutMaxMoveCm = 0.0;
	FSkeletalMeshAttributes Attributes(Mesh);
	TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
	TArray<FVertexID> Vertices;
	TArray<int32> IndexOf;
	IndexOf.Init(INDEX_NONE, Mesh.Vertices().GetArraySize());
	TArray<FVector3f> Before;
	for (const FVertexID Vertex : Mesh.Vertices().GetElementIDs())
	{
		IndexOf[Vertex.GetValue()] = Vertices.Num();
		Vertices.Add(Vertex);
		Before.Add(Positions.Get(Vertex));
	}

	// 1. The re-bind from the garment's own reference pose to the pose on its source body (positions, normals,
	// tangents and morph deltas by linear blend skinning); the description's bone poses become the authoring body's.
	if (!FMantleAvatarRepose::RebindMeshDescription(Mesh, Refit.Names, Refit.GarmentComponent, Refit.SourceComponent, Refit.TargetLocal, OutError))
	{
		return false;
	}

	// 2. The warp from the source body's space to the authoring body's.
	if (Refit.Path != EMantleRefitCorrespondence::Identity)
	{
		TArray<FVector3f> Rebound;
		Rebound.Reserve(Vertices.Num());
		for (const FVertexID Vertex : Vertices)
		{
			Rebound.Add(Positions.Get(Vertex));
		}
		TArray<FVector3f> Warped;
		Warped.SetNumUninitialized(Rebound.Num());
		ParallelFor(Rebound.Num(), [&Rebound, &Warped, &Refit](const int32 Index)
		{
			Warped[Index] = Refit.Warp(Rebound[Index]);
		});

		// Morph position deltas: the field's change over the delta.
		for (const FName& Morph : Attributes.GetMorphTargetNames())
		{
			TVertexAttributesRef<FVector3f> Deltas = Attributes.GetVertexMorphPositionDelta(Morph);
			if (!Deltas.IsValid())
			{
				continue;
			}
			for (int32 Index = 0; Index < Vertices.Num(); ++Index)
			{
				const FVector3f Delta = Deltas.Get(Vertices[Index]);
				if (!Delta.IsNearlyZero())
				{
					Deltas.Set(Vertices[Index], Refit.Warp(Rebound[Index] + Delta) - Warped[Index]);
				}
			}
		}

		// Normals and tangents: the field applied to a 1 cm offset along them (the runtime fitter's rule), the tangent
		// re-orthogonalised to the new normal, the binormal sign kept; a degenerate result keeps the direction it had.
		TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
		TVertexInstanceAttributesRef<FVector3f> Tangents = Attributes.GetVertexInstanceTangents();
		TArray<TVertexInstanceAttributesRef<FVector3f>> MorphNormals;
		for (const FName& Morph : Attributes.GetMorphTargetNames())
		{
			if (Attributes.HasMorphTargetNormalsAttribute(Morph))
			{
				TVertexInstanceAttributesRef<FVector3f> Deltas = Attributes.GetVertexInstanceMorphNormalDelta(Morph);
				if (Deltas.IsValid())
				{
					MorphNormals.Add(Deltas);
				}
			}
		}
		TArray<FVertexInstanceID> Instances;
		TArray<int32> InstanceVertex;
		TArray<FVector3f> OldNormals;
		TArray<FVector3f> OldTangents;
		for (const FVertexInstanceID Instance : Mesh.VertexInstances().GetElementIDs())
		{
			const int32 Vertex = Mesh.GetVertexInstanceVertex(Instance).GetValue();
			if (!IndexOf.IsValidIndex(Vertex) || IndexOf[Vertex] == INDEX_NONE)
			{
				continue;
			}
			Instances.Add(Instance);
			InstanceVertex.Add(IndexOf[Vertex]);
			OldNormals.Add(Normals.IsValid() ? Normals.Get(Instance) : FVector3f::ZeroVector);
			OldTangents.Add(Tangents.IsValid() ? Tangents.Get(Instance) : FVector3f::ZeroVector);
		}
		TArray<FVector3f> NewNormals;
		TArray<FVector3f> NewTangents;
		NewNormals.SetNumUninitialized(Instances.Num());
		NewTangents.SetNumUninitialized(Instances.Num());
		ParallelFor(Instances.Num(), [&](const int32 Index)
		{
			const FVector3f& Point = Rebound[InstanceVertex[Index]];
			const FVector3f& Moved = Warped[InstanceVertex[Index]];
			FVector3f Normal = OldNormals[Index];
			if (!Normal.IsNearlyZero())
			{
				const FVector3f Turned = (Refit.Warp(Point + Normal.GetSafeNormal()) - Moved).GetSafeNormal();
				Normal = Turned.IsNearlyZero() ? Normal : Turned;
			}
			FVector3f Tangent = OldTangents[Index];
			if (!Tangent.IsNearlyZero())
			{
				FVector3f Turned = Refit.Warp(Point + Tangent.GetSafeNormal()) - Moved;
				Turned = (Turned - FVector3f::DotProduct(Turned, Normal) * Normal).GetSafeNormal();
				Tangent = Turned.IsNearlyZero() ? Tangent : Turned;
			}
			NewNormals[Index] = Normal;
			NewTangents[Index] = Tangent;
		});
		for (int32 Index = 0; Index < Instances.Num(); ++Index)
		{
			if (Normals.IsValid())
			{
				Normals.Set(Instances[Index], NewNormals[Index]);
			}
			if (Tangents.IsValid())
			{
				Tangents.Set(Instances[Index], NewTangents[Index]);
			}
			for (TVertexInstanceAttributesRef<FVector3f>& Deltas : MorphNormals)
			{
				const FVector3f Delta = Deltas.Get(Instances[Index]);
				if (!Delta.IsNearlyZero())
				{
					const FVector3f& Point = Rebound[InstanceVertex[Index]];
					Deltas.Set(Instances[Index], Refit.Warp(Point + Delta) - Warped[InstanceVertex[Index]]);
				}
			}
		}
		for (int32 Index = 0; Index < Vertices.Num(); ++Index)
		{
			Positions.Set(Vertices[Index], Warped[Index]);
		}
	}

	for (int32 Index = 0; Index < Vertices.Num(); ++Index)
	{
		OutMaxMoveCm = FMath::Max(OutMaxMoveCm, static_cast<double>(FVector3f::Dist(Positions.Get(Vertices[Index]), Before[Index])));
	}
	return true;
}

bool FMantleAvatarRefit::PushOutOfBody(FMeshDescription& Mesh, const FMantleSkinnedMeshLOD& Body, FMantleRefitPushOut& Out, FString& OutError, const bool bCoverSkin)
{
	Out = FMantleRefitPushOut();
	FSkeletalMeshAttributes Attributes(Mesh);
	TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
	TVertexInstanceAttributesConstRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
	if (!Positions.IsValid())
	{
		OutError = TEXT("the refitted mesh has no vertex positions");
		return false;
	}

	// The garment as the clearance code reads it: one point per vertex, its instances' normals summed, the triangles.
	FMantleSkinnedMeshLOD Garment;
	TArray<FVertexID> Vertices;
	TArray<int32> IndexOf;
	IndexOf.Init(INDEX_NONE, Mesh.Vertices().GetArraySize());
	for (const FVertexID Vertex : Mesh.Vertices().GetElementIDs())
	{
		IndexOf[Vertex.GetValue()] = Vertices.Num();
		Vertices.Add(Vertex);
		Garment.Positions.Add(Positions.Get(Vertex));
	}
	Garment.Normals.Init(FVector3f::ZeroVector, Vertices.Num());
	for (const FTriangleID Triangle : Mesh.Triangles().GetElementIDs())
	{
		for (const FVertexInstanceID Instance : Mesh.GetTriangleVertexInstances(Triangle))
		{
			const int32 Vertex = IndexOf[Mesh.GetVertexInstanceVertex(Instance).GetValue()];
			Garment.Indices.Add(static_cast<uint32>(Vertex));
			if (Normals.IsValid())
			{
				Garment.Normals[Vertex] += Normals.Get(Instance);
			}
		}
	}
	for (FVector3f& Normal : Garment.Normals)
	{
		Normal = Normal.GetSafeNormal();
	}
	if (Garment.NumTriangles() == 0)
	{
		return true;
	}

	const FMantleSkinnedMeshLOD Before = Garment;
	if (!PushOutOfBody(Garment, Body, Out, OutError, bCoverSkin))
	{
		return false;
	}
	for (int32 Vertex = 0; Vertex < Vertices.Num(); ++Vertex)
	{
		if (FVector3f::Dist(Garment.Positions[Vertex], Before.Positions[Vertex]) > 1.e-4)
		{
			Positions.Set(Vertices[Vertex], Garment.Positions[Vertex]);
		}
	}
	return true;
}

TArray<float> FMantleAvatarRefit::DistanceToOpenings(const FMantleSkinnedMeshLOD& Garment)
{
	const int32 N = Garment.NumVertices();
	TArray<float> Distance;
	Distance.Init(TNumericLimits<float>::Max(), N);
	if (N == 0 || Garment.NumTriangles() == 0)
	{
		return Distance;
	}
	// Edges with their triangle counts, and each vertex's neighbours.
	TMap<uint64, int32> EdgeTriangles;
	TArray<TArray<int32>> Neighbours;
	Neighbours.SetNum(N);
	auto Key = [](const int32 A, const int32 B) { return (static_cast<uint64>(FMath::Min(A, B)) << 32) | static_cast<uint32>(FMath::Max(A, B)); };
	for (int32 Triangle = 0; Triangle < Garment.NumTriangles(); ++Triangle)
	{
		const int32 V[3] = { static_cast<int32>(Garment.Indices[Triangle * 3]), static_cast<int32>(Garment.Indices[Triangle * 3 + 1]), static_cast<int32>(Garment.Indices[Triangle * 3 + 2]) };
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			const int32 A = V[Corner];
			const int32 B = V[(Corner + 1) % 3];
			if (A == B || !Garment.Positions.IsValidIndex(A) || !Garment.Positions.IsValidIndex(B))
			{
				continue;
			}
			if (++EdgeTriangles.FindOrAdd(Key(A, B)) == 1)
			{
				Neighbours[A].Add(B);
				Neighbours[B].Add(A);
			}
		}
	}
	// Dijkstra from every vertex of an open edge (an edge with one triangle), along the edges.
	TArray<TPair<float, int32>> Heap;
	for (const TPair<uint64, int32>& Edge : EdgeTriangles)
	{
		if (Edge.Value == 1)
		{
			for (const int32 Vertex : { static_cast<int32>(Edge.Key >> 32), static_cast<int32>(Edge.Key & 0xffffffffu) })
			{
				if (Distance[Vertex] > 0.0f)
				{
					Distance[Vertex] = 0.0f;
					Heap.HeapPush(TPair<float, int32>(0.0f, Vertex));
				}
			}
		}
	}
	while (Heap.Num() > 0)
	{
		TPair<float, int32> Top;
		Heap.HeapPop(Top, EAllowShrinking::No);
		if (Top.Key > Distance[Top.Value])
		{
			continue;
		}
		for (const int32 Next : Neighbours[Top.Value])
		{
			const float Through = Top.Key + FVector3f::Dist(Garment.Positions[Top.Value], Garment.Positions[Next]);
			if (Through < Distance[Next])
			{
				Distance[Next] = Through;
				Heap.HeapPush(TPair<float, int32>(Through, Next));
			}
		}
	}
	return Distance;
}

bool FMantleAvatarRefit::PushOutOfBody(FMantleSkinnedMeshLOD& Garment, const FMantleSkinnedMeshLOD& Body, FMantleRefitPushOut& Out, FString& OutError, const bool bCoverSkin)
{
	Out = FMantleRefitPushOut();
	if (Garment.NumTriangles() == 0)
	{
		return true;
	}

	// Each vertex's clearance over the body; the target is at least PushOutCm, where it is not too deep to be meant.
	FMantleGarmentClearance Target;
	if (!FMantleGarmentClearance::Compute(Garment.Positions, Body, Target, FMantleGarmentClearance::DefaultSearchCm, &OutError))
	{
		return false;
	}
	Target.UntrackBoundary(Body);
	// #153: an inner layer under its own shell (its layer twin outside the skin) is left where it is. The restore couples a
	// vertex and its twin to the larger push, so pushing a padded cup's lining out of the bust pushed the whole cup out and
	// folded it (Vampire's corset: 124 vertices under, 1465 moved, up to 5 cm). The client's coverage hides the skin under
	// the shell. Read on the clearances as measured, before any target is raised.
	TArray<int32> Twin;
	FMantleLayerCrossing::FindTwins(Garment, Twin);
	// #153, try 2: how far each vertex is from the garment's nearest opening (an edge with one triangle), along the
	// surface. Skin is only visible at the openings: under the rest of the garment the client's coverage hides it, so a
	// vertex under the skin there is left where it is instead of being pushed out (a corset cinches tighter than the
	// body; pushing its waist out stretched the front panel and folded the cups).
	const TArray<float> OpeningCm = DistanceToOpenings(Garment);
	const TArray<float> Measured = Target.ClearanceCm;
	// The under-skin vertices by depth and by distance to an opening, for the log.
	int32 DepthBuckets[5] = { 0, 0, 0, 0, 0 };
	int32 OpeningBuckets[5] = { 0, 0, 0, 0, 0 };
	for (int32 Vertex = 0; Vertex < Garment.NumVertices(); ++Vertex)
	{
		if (!Target.IsTracked(Vertex))
		{
			continue;
		}
		++Out.Tracked;
		const float Clearance = Measured[Vertex];
		if (Clearance < 0.0f)
		{
			++Out.InsideBefore;
			Out.DeepestCm = FMath::Max(Out.DeepestCm, -Clearance);
			const float Depth = -Clearance;
			++DepthBuckets[Depth < 0.5f ? 0 : Depth < 1.0f ? 1 : Depth < 2.0f ? 2 : Depth < 4.0f ? 3 : 4];
			const float Opening = OpeningCm.IsValidIndex(Vertex) ? OpeningCm[Vertex] : TNumericLimits<float>::Max();
			++OpeningBuckets[Opening < 1.0f ? 0 : Opening < 2.0f ? 1 : Opening < 4.0f ? 2 : Opening < 8.0f ? 3 : 4];
		}
		if (Clearance < -MaxPushDepthCm)
		{
			++Out.TooDeep;
			Target.Untrack(Vertex);
			continue;
		}
		const int32 Other = Twin.IsValidIndex(Vertex) ? Twin[Vertex] : INDEX_NONE;
		if (Clearance < 0.0f && Other != INDEX_NONE && Target.IsTracked(Other) && Measured[Other] >= 0.0f)
		{
			++Out.UnderShell;
			Target.Untrack(Vertex);
			continue;
		}
		if (Clearance < 0.0f && OpeningCm.IsValidIndex(Vertex) && OpeningCm[Vertex] > OpeningReachCm)
		{
			++Out.UnderCover;
			Target.Untrack(Vertex);
			continue;
		}
		Target.ClearanceCm[Vertex] = FMath::Max(Clearance, PushOutCm);
	}
	if (Out.InsideBefore > 0)
	{
		Out.Detail = FString::Printf(TEXT("under the skin by <0.5 cm %d, <1 %d, <2 %d, <4 %d, more %d; from an opening <1 cm %d, <2 %d, <4 %d, <8 %d, further %d"),
			DepthBuckets[0], DepthBuckets[1], DepthBuckets[2], DepthBuckets[3], DepthBuckets[4],
			OpeningBuckets[0], OpeningBuckets[1], OpeningBuckets[2], OpeningBuckets[3], OpeningBuckets[4]);
	}

	const FMantleSkinnedMeshLOD Before = Garment;
	FMantleClearanceRestoreStats Stats;
	FString Error;
	if (!FMantleClearanceRestore::Restore(Garment, Target, Body.Positions, &Before, /*SlackCm*/ 0.02f, /*MinKeptCm*/ 0.0f,
		FMantleGarmentClearance::DefaultSmoothIters, &Stats, &Error))
	{
		OutError = FString::Printf(TEXT("the push out of the body failed: %s"), *Error);
		return false;
	}
	if (bCoverSkin)
	{
		Out.SkinThroughBefore = CoverSkin(Garment, Body, Out.SkinThroughAfter);
	}
	for (int32 Vertex = 0; Vertex < Garment.NumVertices(); ++Vertex)
	{
		const double Move = FVector3f::Dist(Garment.Positions[Vertex], Before.Positions[Vertex]);
		if (Move > 1.e-4)
		{
			++Out.Moved;
			Out.MaxMoveCm = FMath::Max(Out.MaxMoveCm, Move);
		}
	}

	// What it came to.
	FMantleGarmentClearance After;
	if (FMantleGarmentClearance::Compute(Garment.Positions, Body, After, FMantleGarmentClearance::DefaultSearchCm))
	{
		After.UntrackBoundary(Body);
		for (int32 Vertex = 0; Vertex < Garment.NumVertices(); ++Vertex)
		{
			Out.InsideAfter += After.IsTracked(Vertex) && After.ClearanceCm[Vertex] < -0.05f ? 1 : 0;
		}
	}
	return true;
}

int32 FMantleAvatarRefit::CoverSkin(FMantleSkinnedMeshLOD& Garment, const FMantleSkinnedMeshLOD& Body, int32& OutAfter)
{
	OutAfter = 0;
	int32 ThroughBefore = -1;
	TArray<float> Moved;
	Moved.Init(0.0f, Garment.NumVertices());
	if (Garment.NumTriangles() == 0 || Body.Positions.IsEmpty() || Body.Normals.Num() != Body.Positions.Num())
	{
		return 0;
	}
	for (int32 Pass = 0; Pass <= SkinCoverPasses; ++Pass)
	{
		// The skin over the garment: each body point near it, at its closest garment triangle.
		FMantleGarmentClearance SkinOver;
		if (!FMantleGarmentClearance::Compute(Body.Positions, Garment, SkinOver, SkinCoverSearchCm))
		{
			break;
		}
		SkinOver.UntrackBoundary(Garment);
		TArray<float> Lack;
		TArray<FVector3f> Along;
		Lack.Init(0.0f, Garment.NumVertices());
		Along.Init(FVector3f::ZeroVector, Garment.NumVertices());
		int32 Through = 0;
		for (int32 Point = 0; Point < Body.Positions.Num(); ++Point)
		{
			float Over = 0.0f;
			FVector3f Normal;
			if (!SkinOver.IsTracked(Point) || !SkinOver.Measure(Point, Body.Positions[Point], Garment.Positions, Over, Normal)
				|| FVector3f::DotProduct(Normal, Body.Normals[Point]) < SkinCoverMinDot || Over > SkinCoverMaxThroughCm)
			{
				continue;
			}
			// Over: how far the skin point is above the triangle (positive: through it).
			Through += Over > 0.0f ? 1 : 0;
			const float Needed = Over + SkinCoverCm;
			if (Needed <= 0.01f)
			{
				continue;
			}
			const int32 Triangle = SkinOver.Triangle[Point];
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const int32 Vertex = static_cast<int32>(Garment.Indices[Triangle * 3 + Corner]);
				if (Needed > Lack[Vertex])
				{
					Lack[Vertex] = Needed;
					Along[Vertex] = Normal;
				}
			}
		}
		if (Pass == 0)
		{
			ThroughBefore = Through;
		}
		OutAfter = Through;
		if (Pass == SkinCoverPasses)
		{
			break;
		}
		bool bMoved = false;
		for (int32 Vertex = 0; Vertex < Garment.NumVertices(); ++Vertex)
		{
			const float Move = FMath::Min(Lack[Vertex], SkinCoverMaxMoveCm - Moved[Vertex]);
			if (Move > 0.0f)
			{
				Garment.Positions[Vertex] += Along[Vertex] * Move;
				Moved[Vertex] += Move;
				bMoved = true;
			}
		}
		if (!bMoved)
		{
			break;
		}
	}
	return FMath::Max(0, ThroughBefore);
}

USkeletalMesh* FMantleAvatarRefit::WriteRefit(USkeletalMesh* Garment, const FMantleRefitPlan& InPlan, const FString& PackagePath, const FString& AssetName,
	double& OutMaxMoveCm, FString& OutError, const FMantleSkinnedMeshLOD* SeatBody, FMantleNailSeatingResult* OutSeating,
	const FMantleSkinnedMeshLOD* PushOutBody, FMantleRefitPushOut* OutPushOut)
{
	const FMantleRefitPlan& Refit = InPlan;
	OutMaxMoveCm = 0.0;
	if (OutSeating)
	{
		*OutSeating = FMantleNailSeatingResult();
	}
	if (!Garment)
	{
		OutError = TEXT("there is no mesh to refit");
		return nullptr;
	}
	if (Refit.Names.Num() != Garment->GetRefSkeleton().GetRawBoneNum())
	{
		OutError = FString::Printf(TEXT("the refit was planned for %d bones, %s has %d"), Refit.Names.Num(), *Garment->GetName(), Garment->GetRefSkeleton().GetRawBoneNum());
		return nullptr;
	}
	double MaxMove = 0.0;
	USkeletalMesh* Mesh = FMantleAvatarRepose::WriteWithPose(Garment, Refit.TargetLocal, PackagePath, AssetName,
		[&Refit, &MaxMove, SeatBody, OutSeating, PushOutBody, OutPushOut](const int32 LOD, FMeshDescription& Description, FString& OutLODError)
		{
			double Move = 0.0;
			if (!RefitMeshDescription(Description, Refit, Move, OutLODError))
			{
				return false;
			}
			if (Move > MaxMoveCm && !Refit.bOnBones)
			{
				OutLODError = FString::Printf(TEXT("a vertex of LOD %d would move %.1f cm (at most %.0f cm), so the piece is probably not made for a body like this one"),
					LOD, Move, MaxMoveCm);
				return false;
			}
			MaxMove = FMath::Max(MaxMove, Move);
			if (PushOutBody && !Refit.bOnBones)
			{
				FMantleRefitPushOut Pushed;
				if (!PushOutOfBody(Description, *PushOutBody, Pushed, OutLODError))
				{
					return false;
				}
				UE_LOG(LogMantle, Log, TEXT("MANTLE_AVATAR_REFIT push out LOD %d: %s"), LOD, *Pushed.ToString());
				if (OutPushOut && LOD == 0)
				{
					*OutPushOut = Pushed;
				}
			}
			if (SeatBody)
			{
				FMantleNailSeatingResult Seated;
				if (!FMantleNailSeating::SeatMeshDescription(Description, *SeatBody, Seated, OutLODError))
				{
					return false;
				}
				UE_LOG(LogMantle, Log, TEXT("MANTLE_AVATAR_REFIT nails LOD %d: %d of %d nails seated, clearance %.3f to %.3f cm, largest move %.2f cm"), LOD,
					Seated.NumSeated, Seated.NumIslands, Seated.LowestClearanceCm, Seated.HighestClearanceCm, Seated.MaxMoveCm);
				if (OutSeating)
				{
					*OutSeating = Seated;
				}
				// Carried by its bones, a nail is already on its fingertip: seating only sets it on the skin.
				if (Refit.bOnBones && Seated.NumSeated < Seated.NumIslands)
				{
					OutLODError = FString::Printf(TEXT("%d of %d nails of LOD %d are not within %.0f cm of the body once on their finger bones, so the piece is probably not bound to the fingers it covers"),
						Seated.NumIslands - Seated.NumSeated, Seated.NumIslands, LOD, FMantleNailSeating::SearchCm);
					return false;
				}
				if (Refit.bOnBones && Seated.MaxMoveCm > FMantleNailSeating::MaxSeatMoveCm)
				{
					OutLODError = FString::Printf(TEXT("a nail of LOD %d is %.1f cm off its fingertip once on its finger bone (at most %.0f cm), so the piece is probably not bound to the fingers it covers"),
						LOD, Seated.MaxMoveCm, FMantleNailSeating::MaxSeatMoveCm);
					return false;
				}
			}
			else if (Refit.bOnBones)
			{
				OutLODError = TEXT("a piece carried on its bones needs the body to seat it on");
				return false;
			}
			return true;
		}, OutError);
	if (!Mesh)
	{
		return nullptr;
	}
	OutMaxMoveCm = MaxMove;
	UE_LOG(LogMantle, Log, TEXT("MANTLE_AVATAR_REFIT %s -> %s (%s, %d landmarks, largest move %.2f cm)"), *Garment->GetPathName(), *Mesh->GetPathName(),
		LexToString(Refit.Path), Refit.NumLandmarks, MaxMove);
	return Mesh;
}

FMantleAvatarRefitFixer::FBodies FMantleAvatarRefitFixer::ResolveBodies(const FMantleConvertPart& Part, USkeletalMesh* CharacterBody,
	const FMantleAvatarContract& Contract)
{
	FBodies Out;
	Out.Source = CharacterBody;
	const bool bCharacterReference = CharacterBody && FMantleCosmeticExport::FindReferenceLOD(CharacterBody, Contract) != INDEX_NONE;
	USkeletalMesh* Chosen = Part.AuthoringBody.Get();
	if (Chosen && FMantleCosmeticExport::FindReferenceLOD(Chosen, Contract) != INDEX_NONE)
	{
		Out.Target = Chosen;
		Out.TargetFace = Part.AuthoringFace.Get();
		if (!Out.TargetFace && FMantleReferenceBodies::IsReferenceMesh(Chosen))
		{
			FString FaceNote;
			Out.TargetFace = FMantleReferenceBodies::GetFace(&FaceNote);
			if (!Out.TargetFace)
			{
				UE_LOG(LogMantle, Log, TEXT("MANTLE_AVATAR_REFIT %s without an authoring face: %s"), *Chosen->GetPathName(), *FaceNote);
			}
		}
	}
	else if (bCharacterReference)
	{
		// What the Assembly fixer picks: the character's own body.
		Out.Target = CharacterBody;
	}
	else
	{
		// Mantle's neutral reference body, matched to the character (or the piece outside an assembly): the project's own
		// build of it, checked against the topology; this is editor fitting provenance only.
		Out.bSourceLacksTopology = CharacterBody != nullptr;
		const USkeletalMesh* Measured = CharacterBody ? CharacterBody : Part.Mesh.Get();
		FMantleReferenceBody Reference;
		if (Measured && FMantleReferenceBodies::GetClosest(*Measured, Contract, Reference, Out.NoTargetWhy))
		{
			Out.Target = Reference.Body;
			FString FaceNote;
			Out.TargetFace = FMantleReferenceBodies::GetFace(&FaceNote);
			if (!Out.TargetFace)
			{
				UE_LOG(LogMantle, Log, TEXT("MANTLE_AVATAR_REFIT %s without a face: %s"), *Reference.Id, *FaceNote);
			}
			return Out;
		}
		Out.bReferenceNotBuilt = Measured && !FMantleReferenceBodies::IsBuilt();
	}
	if (Out.Target && !Out.TargetFace && !FMantleReferenceBodies::IsReferenceMesh(Out.Target))
	{
		FMantleCosmeticDraft Draft;
		Out.TargetFace = FMantleCosmeticExport::FindAuthoringFace(Draft, Out.Target);
	}
	return Out;
}

void FMantleAvatarRefitFixer::AnalyzePart(const FMantleConvertContext& Context, const int32 PartIndex, const FBodies& Bodies,
	const FMantleAvatarContract& Contract, TArray<FMantleConvertFix>& OutFixes)
{
	if (!Context.Parts.IsValidIndex(PartIndex))
	{
		return;
	}
	const FMantleConvertPart& Part = Context.Parts[PartIndex];
	// Hair exports rigid head weights; fitting is driven by the immutable authoring face, not body joints.
	if (Contract.HairSlots.Contains(Part.Slot)) return;
	USkeletalMesh* Mesh = Part.Mesh.Get();
	if (!Mesh)
	{
		return;
	}
	const FString What = Mesh->GetName();
	FMantleConvertFix Fix;
	Fix.FixerId = MantleAvatarFixerIds::Refit;
	Fix.PartIndex = PartIndex;

	if (!Bodies.Target)
	{
		Fix.Problem = Bodies.Source
			? FString::Printf(TEXT("%s was made on %s, which has no LOD with Mantle's reference topology, and there is no body with it to refit onto."),
				*What, *Bodies.Source->GetName())
			: FString::Printf(TEXT("%s uploads with the body it was made on, and there is no body with Mantle's reference topology to refit onto."), *What);
		const FString Preflight = Bodies.bReferenceNotBuilt ? FMantleMetaHumanBuild::Preflight(/*bAskEpic*/ false).Problem() : FString();
		if (Bodies.bReferenceNotBuilt && Preflight.IsEmpty())
		{
			// The project hasn't built Mantle's reference body yet: this line builds it once, then refits onto it.
			Fix.Action = FString::Printf(TEXT("builds Mantle's neutral reference body into %s first (once per project: a blank MetaHuman, nobody in particular; takes a few minutes), then refits %s onto it. %s"),
				FMantleReferenceBodies::Folder, *What, FMantleMetaHumanBuild::ConsentLine);
			OutFixes.Add(Fix);
			return;
		}
		Fix.Action = Bodies.bReferenceNotBuilt
			? FMantleReferenceBodies::UnavailableText(FString::Printf(TEXT("Mantle's neutral reference body isn't built in this project yet, and building it needs Epic. %s."), *Preflight))
			: Bodies.NoTargetWhy.IsEmpty() ? FMantleAvatarRefit::NoReferenceBodyText() : FMantleReferenceBodies::UnavailableText(Bodies.NoTargetWhy);
		Fix.bCanFix = false;
		Fix.bEnabled = false;
		OutFixes.Add(Fix);
		return;
	}

	FName Bone;
	double Cm = 0.0;
	double Degrees = 0.0;
	const bool bOff = FMantleAvatarRefit::FindBindOffset(*Mesh, *Bodies.Target, Contract, Bone, Cm, Degrees);
	// #740: nails always go through the refit, whatever their bind: it is where they are seated on the body.
	const bool bSeatNails = FMantleAvatarRefit::IsOverlaySlot(Part.Slot);
	if (!bOff && !Bodies.bSourceLacksTopology && !bSeatNails)
	{
		return;
	}
	TArray<FString> Why;
	if (bSeatNails)
	{
		Why.Add(TEXT("shaped nails are seated on the body they are authored on, so each sits just above the fingertip it covers"));
	}
	if (bOff)
	{
		Why.Add(FString::Printf(TEXT("bone \"%s\" is bound %.2f cm and %.2f degrees from %s's reference pose (the upload allows %.1f cm and %.0f degree%s)"),
			*Bone.ToString(), Cm, Degrees, *Bodies.Target->GetName(), Contract.BindToleranceCm, Contract.BindToleranceDegrees,
			FMath::IsNearlyEqual(Contract.BindToleranceDegrees, 1.0f) ? TEXT("") : TEXT("s")));
	}
	if (Bodies.bSourceLacksTopology && Bodies.Source)
	{
		Why.Add(FString::Printf(TEXT("it was shaped on %s, which has no LOD with Mantle's reference topology, so no client can fit it from that body"),
			*Bodies.Source->GetName()));
	}
	Fix.Problem = FString::Printf(TEXT("%s: %s."), *What, *FString::Join(Why, TEXT(", and ")));

	FMantleRefitPlan Plan;
	FString Error;
	const FString SourceName = Bodies.Source ? Bodies.Source->GetName() : FString(TEXT("its own reference pose"));
	if (FMantleAvatarRigMap::IsUE4MannequinGarment(FMantleAvatarRigMap::ReadBones(Mesh), Contract))
	{
		// The RigMap fixer, before this one, maps it onto the MetaHuman body skeleton; it is planned again on that mesh.
		Fix.Action = FString::Printf(TEXT("refits %s onto %s once its UE4 Mannequin rig is mapped onto the MetaHuman body skeleton: re-binds to its pose and warps the shape"),
			*What, *Bodies.Target->GetName());
	}
	else if (!FMantleAvatarRefit::Plan(*Mesh, Bodies.Source, *Bodies.Target, /*bAllowDense*/ true, Plan, Error,
		FMantleAvatarRefit::IsOverlaySlot(Part.Slot)))
	{
		Fix.Action = FString::Printf(TEXT("Can't refit it onto %s: %s."), *Bodies.Target->GetName(), *Error);
		Fix.bCanFix = false;
		Fix.bEnabled = false;
	}
	else if (Plan.ExceedsMoveCap())
	{
		Fix.Action = FString::Printf(TEXT("Can't refit it onto %s: a vertex would move %.1f cm (at most %.0f cm), so it is probably not made for a body like it."),
			*Bodies.Target->GetName(), Plan.MaxMoveCm, FMantleAvatarRefit::MaxMoveCm);
		Fix.bCanFix = false;
		Fix.bEnabled = false;
	}
	else if (Plan.bOnBones)
	{
		Fix.Action = FString::Printf(TEXT("refits %s onto %s: each nail follows its finger bone onto its pose (max move %.1f cm), then sits on the fingertip"),
			*What, *Bodies.Target->GetName(), Plan.MaxMoveCm);
	}
	else
	{
		Fix.Action = FString::Printf(TEXT("refits %s from %s onto %s: re-binds to its pose and warps the shape (max move %.1f cm)"),
			*What, *SourceName, *Bodies.Target->GetName(), Plan.MaxMoveCm);
	}
	OutFixes.Add(Fix);
}

void FMantleAvatarRefitFixer::Analyze(const FMantleConvertContext& Context, TArray<FMantleConvertFix>& OutFixes) const
{
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get();
	if (!Contract)
	{
		return;
	}
	USkeletalMesh* CharacterBody = nullptr;
	bool bScanned = false;
	for (int32 Index = 0; Index < Context.Parts.Num(); ++Index)
	{
		const FMantleConvertPart& Part = Context.Parts[Index];
		EMantleCosmeticClass Class = EMantleCosmeticClass::Garment;
		if (!Part.Mesh.IsValid() || !FMantleAvatarConverter::ClassOfSlot(Part.Slot, *Contract, Class)
			|| (Class != EMantleCosmeticClass::Garment && Class != EMantleCosmeticClass::Hair))
		{
			continue;
		}
		if (!bScanned && !Context.AssemblyFolder.IsEmpty())
		{
			FMantleAssemblyScan Found;
			FString Error;
			if (FMantleAvatarAssemblyFixer::Scan(Context.AssemblyFolder, Found, Error, Context.IsHandedOffBuild(Context.AssemblyFolder)))
			{
				// #625: the body as this conversion left it (on Mantle's reference topology once converted).
				CharacterBody = FMantleAvatarConverter::ConvertedBodyFor(Context, Found.Body.Get(), *Contract);
			}
		}
		bScanned = true;
		AnalyzePart(Context, Index, ResolveBodies(Part, CharacterBody, *Contract), *Contract, OutFixes);
	}
}

bool FMantleAvatarRefitFixer::Apply(const FMantleConvertFix& Fix, FMantleConvertContext& Context, FString& OutError) const
{
	if (Fix.FixerId != GetId() || !Context.Parts.IsValidIndex(Fix.PartIndex))
	{
		OutError = TEXT("the refit names no part");
		return false;
	}
	FString ContractError;
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get(&ContractError);
	if (!Contract)
	{
		OutError = ContractError;
		return false;
	}
	FMantleConvertPart& Part = Context.Parts[Fix.PartIndex];
	USkeletalMesh* Mesh = Part.Mesh.Get();
	if (!Mesh)
	{
		OutError = TEXT("the part has no mesh to refit");
		return false;
	}
	if (Context.OutputFolder.IsEmpty() || !FPackageName::IsValidLongPackageName(Context.OutputFolder, /*bIncludeReadOnlyRoots*/ true))
	{
		OutError = FString::Printf(TEXT("\"%s\" is not a folder the refitted mesh can go in"), *Context.OutputFolder);
		return false;
	}

	// The bodies and the plan again, on the part as it stands: an earlier fixer may have changed it since the fix list.
	USkeletalMesh* CharacterBody = nullptr;
	if (!Context.AssemblyFolder.IsEmpty())
	{
		FMantleAssemblyScan Found;
		FString Error;
		if (FMantleAvatarAssemblyFixer::Scan(Context.AssemblyFolder, Found, Error, Context.IsHandedOffBuild(Context.AssemblyFolder)))
		{
			CharacterBody = FMantleAvatarConverter::ConvertedBodyFor(Context, Found.Body.Get(), *Contract);
		}
	}
	FBodies Bodies = ResolveBodies(Part, CharacterBody, *Contract);
	if (!Bodies.Target && Bodies.bReferenceNotBuilt)
	{
		// Once per project: Mantle's reference body, built with Epic's tools, then the bodies again.
		if (!FMantleReferenceBodies::Build(OutError))
		{
			return false;
		}
		Bodies = ResolveBodies(Part, CharacterBody, *Contract);
	}
	if (!Bodies.Target)
	{
		OutError = Bodies.NoTargetWhy.IsEmpty() ? FMantleAvatarRefit::NoReferenceBodyText() : FMantleReferenceBodies::UnavailableText(Bodies.NoTargetWhy);
		return false;
	}
	FMantleRefitPlan Plan;
	if (!FMantleAvatarRefit::Plan(*Mesh, Bodies.Source, *Bodies.Target, /*bAllowDense*/ true, Plan, OutError,
		FMantleAvatarRefit::IsOverlaySlot(Part.Slot)))
	{
		return false;
	}
	if (Plan.ExceedsMoveCap())
	{
		OutError = FString::Printf(TEXT("a vertex would move %.1f cm (at most %.0f cm), so %s is probably not made for a body like %s"), Plan.MaxMoveCm,
			FMantleAvatarRefit::MaxMoveCm, *Mesh->GetName(), *Bodies.Target->GetName());
		return false;
	}

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	FString PackageName;
	FString AssetName;
	AssetTools.CreateUniqueAssetName(Context.OutputFolder / (Mesh->GetName() + TEXT("_Refit")), FString(), PackageName, AssetName);
	double Moved = 0.0;
	// #740: shaped nails are seated on the authoring body once the refit has put them there.
	FMantleSkinnedMeshLOD SeatBody;
	FMantleNailSeatingResult Seating;
	const bool bSeat = FMantleAvatarRefit::IsOverlaySlot(Part.Slot);
	// Every other piece is pushed out of the authoring body's skin where the warp left it under (the same surface).
	FMantleRefitPushOut PushOut;
	{
		const int32 BodyLOD = FMantleCosmeticExport::FindReferenceLOD(Bodies.Target, *Contract);
		if (BodyLOD == INDEX_NONE || !FMantleNailSeating::ReadBodySurface(*Bodies.Target, BodyLOD, SeatBody, OutError))
		{
			if (BodyLOD == INDEX_NONE)
			{
				OutError = FString::Printf(TEXT("%s has no LOD with Mantle's reference topology to fit the piece against"), *Bodies.Target->GetName());
			}
			return false;
		}
	}
	USkeletalMesh* Refit = FMantleAvatarRefit::WriteRefit(Mesh, Plan, FPackageName::GetLongPackagePath(PackageName), AssetName, Moved, OutError,
		bSeat ? &SeatBody : nullptr, &Seating, bSeat ? nullptr : &SeatBody, &PushOut);
	if (!Refit)
	{
		return false;
	}

	// What the upload checks, on what was written.
	FName Bone;
	double Cm = 0.0;
	double Degrees = 0.0;
	if (FMantleAvatarRefit::FindBindOffset(*Refit, *Bodies.Target, *Contract, Bone, Cm, Degrees))
	{
		OutError = FString::Printf(TEXT("the refitted %s still has bone \"%s\" bound %.2f cm and %.2f degrees from %s's reference pose"), *Refit->GetName(),
			*Bone.ToString(), Cm, Degrees, *Bodies.Target->GetName());
		FMantleAvatarRepose::DiscardMesh(Refit);
		return false;
	}

	Context.CreatedAssets.Add(Refit);
	Context.Notes.Add(FString::Printf(TEXT("%s: refitted from %s onto %s as %s (%s correspondence, %d landmarks); the largest vertex move is %.1f cm."),
		*Part.Name, Bodies.Source ? *Bodies.Source->GetName() : TEXT("its own reference pose"), *Bodies.Target->GetName(), *Refit->GetName(),
		LexToString(Plan.Path), Plan.NumLandmarks, Moved));
	if (!bSeat && PushOut.Tracked > 0)
	{
		Context.Notes.Add(FString::Printf(TEXT("%s: pushed out of %s's skin to %.1f cm where the refit left it under: %s."), *Part.Name,
			*Bodies.Target->GetName(), FMantleAvatarRefit::PushOutCm, *PushOut.ToString()));
	}
	if (Plan.Folded.Num() > 0)
	{
		TArray<FString> Folds;
		for (const FMantleFoldedBone& Folded : Plan.Folded)
		{
			Folds.Add(FString::Printf(TEXT("%s -> %s"), *Folded.Bone.ToString(), *Folded.Into.ToString()));
		}
		Context.Notes.Add(FString::Printf(TEXT("%s: %d bone%s the body lacks moved with %s parent for the refit (%s); the upload merges them into it."),
			*Part.Name, Folds.Num(), Folds.Num() == 1 ? TEXT("") : TEXT("s"), Folds.Num() == 1 ? TEXT("its") : TEXT("their"), *FString::Join(Folds, TEXT(", "))));
	}
	if (bSeat)
	{
		Context.Notes.Add(FString::Printf(TEXT("%s: %d of %d nails seated on %s, each %.2f cm above the skin (rigid moves, the largest %.2f cm)."),
			*Part.Name, Seating.NumSeated, Seating.NumIslands, *Bodies.Target->GetName(), FMantleNailSeating::SeatCm, Seating.MaxMoveCm));
	}
	Part.Mesh = Refit;
	Part.AuthoringBody = Bodies.Target;
	Part.AuthoringFace = Bodies.TargetFace;
	return true;
}

void RegisterMantleAvatarRefitFixer()
{
	FMantleAvatarFixers::Register(MakeShared<FMantleAvatarRefitFixer>());
}
