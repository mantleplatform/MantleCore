// Copyright Joshua Eger. All Rights Reserved.
#include "MantleStandardBodyMatch.h"

#include "MantleFitBodies.h"

#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"

DEFINE_LOG_CATEGORY_STATIC(LogMantleStandardBodyMatch, Log, All);

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleStandardBodyMatchPrivate
{
	/**
	 * Epic's body skeleton, which every standard body uses (MetaHumanCharacterSkelMeshUtils.cpp:1606-1608). The path is
	 * FMetaHumanCommonDataUtils::GetCharacterPluginBodySkeletonPath (MetaHumanCommonDataUtils.cpp:117-119), written out here
	 * so this module needs no MetaHumanCoreTech dependency.
	 */
	const TCHAR* BodySkeletonPath = TEXT("/MetaHumanCharacter/Female/Medium/NormalWeight/Body/metahuman_base_skel.metahuman_base_skel");

	/** Each standard body's joints, measured once per editor session (a body takes seconds to make; its joints never change). */
	TMap<FString, TMap<FName, FVector3f>>& JointCache()
	{
		static TMap<FString, TMap<FName, FVector3f>> Cache;
		return Cache;
	}

	TArray<FMantleGlbJoint> JointsOf(const FReferenceSkeleton& Reference)
	{
		TArray<FMantleGlbJoint> Joints;
		const TArray<FTransform>& Pose = Reference.GetRefBonePose();
		for (int32 Bone = 0; Bone < Pose.Num(); ++Bone)
		{
			FMantleGlbJoint& Joint = Joints.AddDefaulted_GetRef();
			Joint.Name = Reference.GetBoneName(Bone);
			Joint.Parent = Reference.GetParentIndex(Bone);
			Joint.Local = Pose[Bone];
		}
		return Joints;
	}
}

TArray<FString> FMantleStandardBodyMatch::AllBodyIds()
{
	TArray<FString> Ids;
	for (const TCHAR* Sex : { TEXT("f"), TEXT("m") })
	{
		for (const TCHAR* Height : { TEXT("srt"), TEXT("med"), TEXT("tal") })
		{
			for (const TCHAR* Build : { TEXT("unw"), TEXT("nrw"), TEXT("ovw") })
			{
				Ids.Add(FString::Printf(TEXT("%s_%s_%s"), Sex, Height, Build));
			}
		}
	}
	return Ids;
}

TMap<FName, FVector3f> FMantleStandardBodyMatch::JointPositions(TConstArrayView<FMantleGlbJoint> Joints)
{
	TArray<FTransform> Component;
	Component.SetNum(Joints.Num());
	TMap<FName, FVector3f> Out;
	for (int32 Index = 0; Index < Joints.Num(); ++Index)
	{
		const int32 Parent = Joints[Index].Parent;
		// Parent-first, so a parent's component transform is ready; a parent out of order is read as a root.
		Component[Index] = Parent >= 0 && Parent < Index ? Joints[Index].Local * Component[Parent] : Joints[Index].Local;
		Out.Add(Joints[Index].Name, FVector3f(Component[Index].GetLocation()));
	}
	return Out;
}

TSet<FName> FMantleStandardBodyMatch::SkinnedBones(const FMantleGlbMesh& Mesh, const TSet<int32>* KeepMaterials)
{
	TSet<FName> Out;
	const int32 Influences = Mesh.NumInfluences;
	for (const FMantleGlbPrimitive& Primitive : Mesh.Primitives)
	{
		if (KeepMaterials && !KeepMaterials->Contains(Primitive.MaterialIndex)) { continue; }
		for (int32 Vertex = Primitive.FirstVertex; Vertex < Primitive.FirstVertex + Primitive.NumVertices; ++Vertex)
		{
			for (int32 Slot = 0; Slot < Influences; ++Slot)
			{
				const int32 Index = Vertex * Influences + Slot;
				if (!Mesh.InfluenceWeights.IsValidIndex(Index) || !Mesh.InfluenceJoints.IsValidIndex(Index) || Mesh.InfluenceWeights[Index] <= 0.0f) { continue; }
				const int32 Joint = Mesh.InfluenceJoints[Index];
				if (Mesh.Joints.IsValidIndex(Joint)) { Out.Add(Mesh.Joints[Joint].Name); }
			}
		}
	}
	return Out;
}

FMantleStandardBodyMatch::FResult FMantleStandardBodyMatch::Match(const TMap<FName, FVector3f>& Garment, const TSet<FName>& Skinned,
	TConstArrayView<FCandidate> Bodies, const float CutoffCm)
{
	FResult Result;
	// The bones the score reads: skinned ones the garment has a joint for. One no body has means it isn't a standard body's skeleton.
	TArray<FName> Bones;
	for (const FName& Bone : Skinned)
	{
		if (!Garment.Contains(Bone)) { continue; }
		if (Bodies.ContainsByPredicate([&Bone](const FCandidate& Body) { return !Body.Joints.Contains(Bone); })) { Result.Missing.Add(Bone); }
		else { Bones.Add(Bone); }
	}
	Result.Missing.Sort(FNameLexicalLess());
	for (const FCandidate& Body : Bodies)
	{
		FScore& Score = Result.All.AddDefaulted_GetRef();
		Score.Id = Body.Id;
		if (!Result.Missing.IsEmpty() || Bones.IsEmpty()) { continue; }
		double Sum = 0.0;
		for (const FName& Bone : Bones)
		{
			const float Distance = FVector3f::Distance(Garment[Bone], Body.Joints[Bone]);
			Sum += Distance;
			if (Distance > Score.LargestCm) { Score.LargestCm = Distance; Score.LargestBone = Bone; }
		}
		Score.Bones = Bones.Num();
		Score.MeanCm = static_cast<float>(Sum / Bones.Num());
	}
	for (const FScore& Score : Result.All)
	{
		if (Score.MeanCm < 0.0f) { continue; }
		if (Result.Best.MeanCm < 0.0f || Score.MeanCm < Result.Best.MeanCm) { Result.RunnerUp = Result.Best; Result.Best = Score; }
		else if (Result.RunnerUp.MeanCm < 0.0f || Score.MeanCm < Result.RunnerUp.MeanCm) { Result.RunnerUp = Score; }
	}
	Result.bMatch = Result.Best.MeanCm >= 0.0f && Result.Best.MeanCm <= CutoffCm;
	return Result;
}

FString FMantleStandardBodyMatch::BodyWords(const FString& Id)
{
	TArray<FString> Parts;
	Id.ParseIntoArray(Parts, TEXT("_"));
	if (Parts.Num() != 3) { return Id; }
	const TMap<FString, FString> Words = {
		{ TEXT("f"), TEXT("women's") }, { TEXT("m"), TEXT("men's") },
		{ TEXT("srt"), TEXT("short") }, { TEXT("med"), TEXT("medium") }, { TEXT("tal"), TEXT("tall") },
		{ TEXT("unw"), TEXT("underweight") }, { TEXT("nrw"), TEXT("normal-weight") }, { TEXT("ovw"), TEXT("overweight") } };
	const FString* Sex = Words.Find(Parts[0]);
	const FString* Height = Words.Find(Parts[1]);
	const FString* Build = Words.Find(Parts[2]);
	return Sex && Height && Build ? FString::Printf(TEXT("%s %s %s"), **Sex, **Height, **Build) : Id;
}

FString FMantleStandardBodyMatch::Describe(const FResult& Result)
{
	if (!Result.Missing.IsEmpty())
	{
		TArray<FString> Names;
		for (int32 Index = 0; Index < Result.Missing.Num() && Index < 5; ++Index) { Names.Add(Result.Missing[Index].ToString()); }
		return FString::Printf(TEXT("Not made on one of Epic's standard MetaHuman bodies: it is skinned to bones they don't have (%s%s)."),
			*FString::Join(Names, TEXT(", ")), Result.Missing.Num() > 5 ? TEXT(", ...") : TEXT(""));
	}
	if (Result.Best.MeanCm < 0.0f)
	{
		return TEXT("Not made on one of Epic's standard MetaHuman bodies: it is not skinned to any of their bones.");
	}
	const FString Next = Result.RunnerUp.MeanCm >= 0.0f
		? FString::Printf(TEXT("; next closest %s, %.2f cm"), *Result.RunnerUp.Id, Result.RunnerUp.MeanCm) : FString();
	if (Result.bMatch)
	{
		return FString::Printf(TEXT("Made on Epic's standard %s body (%s): its joints are %.2f cm from that body's on the %d bones it is skinned to%s. Its sizes are made directly from that body."),
			*BodyWords(Result.Best.Id), *Result.Best.Id, Result.Best.MeanCm, Result.Best.Bones, *Next);
	}
	return FString::Printf(TEXT("Not made on one of Epic's standard MetaHuman bodies: the nearest is the %s body (%s), its joints %.2f cm off on the %d bones it is skinned to (%.1f cm or less counts%s)."),
		*BodyWords(Result.Best.Id), *Result.Best.Id, Result.Best.MeanCm, Result.Best.Bones, JointMatchCm, *Next);
}

bool FMantleStandardBodyMatch::BodyJoints(const FString& Id, FMantleFitBodyPool& Pool, TFunctionRef<bool(const FString&)> Keep,
	TMap<FName, FVector3f>& OutJoints, FString& OutError)
{
	using namespace MantleStandardBodyMatchPrivate;
	if (const TMap<FName, FVector3f>* Cached = JointCache().Find(Id))
	{
		OutJoints = *Cached;
		return true;
	}
	EMetaHumanBodyType Type{};
	FMantleFitBody Body;
	if (!FMantleFitBodies::ParseBodyType(Id, Type) || !FMantleFitBodies::Make(Id, Type, Body, OutError) || !Body.MergedHeadAndBodyMesh.IsValid())
	{
		FMantleFitBodies::Release(Body);
		if (OutError.IsEmpty()) { OutError = FString::Printf(TEXT("the standard body %s could not be made"), *Id); }
		return false;
	}
	OutJoints = JointPositions(JointsOf(Body.MergedHeadAndBodyMesh->GetRefSkeleton()));
	JointCache().Add(Id, OutJoints);
	UE_LOG(LogMantleStandardBodyMatch, Display, TEXT("MANTLE_STANDARD_BODY %s made to read its %d joints (kept for this editor session)"), *Id, OutJoints.Num());
	if (Keep(Id)) { Pool.Bodies.Add(MoveTemp(Body)); }
	else { FMantleFitBodies::Release(Body); }
	return true;
}

TSet<FName> FMantleStandardBodyMatch::StandardBoneNames()
{
	TSet<FName> Names;
	const USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, MantleStandardBodyMatchPrivate::BodySkeletonPath);
	if (!Skeleton) { return Names; }
	const FReferenceSkeleton& Reference = Skeleton->GetReferenceSkeleton();
	for (int32 Bone = 0; Bone < Reference.GetRawBoneNum(); ++Bone) { Names.Add(Reference.GetBoneName(Bone)); }
	return Names;
}
