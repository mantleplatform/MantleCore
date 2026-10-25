// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCosmeticValidation.h"
#include "MantleAvatarContract.h"
#include "MantleRuntime.h"

#include "ReferenceSkeleton.h"

const TCHAR* FMantleCosmeticChecks::UnsupportedSkeletonMessage =
	TEXT("This character's skeleton isn't one we can use yet. Try the Convert route, which rebuilds it on a standard body.");

FString FMantleCosmeticChecks::UnsupportedSkeleton(const FString& Detail)
{
	UE_LOG(LogMantle, Log, TEXT("MANTLE_SKELETON not skinned to a supported skeleton: %s"), *Detail);
	return UnsupportedSkeletonMessage;
}

FString FMantleCosmeticChecks::EndWithFullStop(const FString& Message)
{
	return Message.EndsWith(TEXT(".")) ? Message : Message + TEXT(".");
}

const TCHAR* FMantleCosmeticChecks::NotGarmentCompatibleMessage =
	TEXT("not garment-compatible: export the MetaHuman with UE Optimized at Medium, High or Cinematic quality, without clothing");

const TCHAR* FMantleCosmeticChecks::NotGarmentCompatibleHint =
	TEXT("It still uploads as-is: it wears accessories and emotes, not 3D clothing. To wear clothing, pick it under Avatar and choose Convert...: it becomes a MetaHuman.");

const TCHAR* FMantleCosmeticChecks::ConvertFirstMessage =
	TEXT("Convert it first: this body isn't a MetaHuman with Mantle's reference topology");

const TCHAR* FMantleCosmeticChecks::ConvertFirstHint =
	TEXT("Pick it under Avatar and choose Convert...: it becomes a MetaHuman that wears clothing and takes the avatar options, at Mantle's default height.");

const TCHAR* FMantleCosmeticChecks::NotClothingReadyMessage =
	TEXT("not clothing-ready yet: a converted MetaHuman wears no 3D clothing for now");

const TCHAR* FMantleCosmeticChecks::NotClothingReadyHint =
	TEXT("It uploads and animates, with Epic's default underwear. Clothing for converted avatars comes later.");

const TCHAR* FMantleCosmeticChecks::HeadlessAvatarMessage =
	TEXT("no head: this body has Mantle's reference topology, which ends at the neck, and no face is chosen");

const TCHAR* FMantleCosmeticChecks::HeadlessAvatarHint =
	TEXT("Pick its face under Face: the MetaHuman's own face, which Convert picks for you.");

const TCHAR* FMantleCosmeticChecks::UE5MannequinAvatarMessage =
	TEXT("UE5 Mannequin body: it wears accessories and emotes, 3D clothing is set aside while it is worn, and it takes no MetaHuman face");

const TCHAR* FMantleCosmeticChecks::UE4MannequinAvatarMessage =
	TEXT("UE4 Mannequin body: it wears accessories and emotes, 3D clothing is set aside while it is worn, and it takes no MetaHuman face");

const TCHAR* FMantleCosmeticChecks::UEFNMannequinAvatarMessage =
	TEXT("UEFN Mannequin body: it moves exactly as Mantle's character does, wears accessories and emotes, 3D clothing is set aside while it is worn, and it takes no MetaHuman face");

const TCHAR* FMantleCosmeticChecks::PublishAsNewItemLabel = TEXT("A new avatar item");

const FName FMantleCosmeticChecks::JawSlotBone(TEXT("jaw"));
const FName FMantleCosmeticChecks::JawSlotParent(TEXT("head"));

FString FMantleCosmeticIssue::ToString() const
{
	const TCHAR* Prefix = Severity == EMantleCosmeticSeverity::Error ? TEXT("Problem")
		: Severity == EMantleCosmeticSeverity::Warning ? TEXT("Note") : TEXT("Done");
	FString Text = FString::Printf(TEXT("%s: %s"), Prefix, *Message);
	if (!Hint.IsEmpty())
	{
		Text += TEXT(" ") + Hint;
	}
	return Text;
}

FMantleCosmeticIssue FMantleCosmeticChecks::Error(const FString& Message, const FString& Hint)
{
	return FMantleCosmeticIssue{ EMantleCosmeticSeverity::Error, Message, Hint };
}

FMantleCosmeticIssue FMantleCosmeticChecks::Warning(const FString& Message, const FString& Hint)
{
	return FMantleCosmeticIssue{ EMantleCosmeticSeverity::Warning, Message, Hint };
}

FMantleCosmeticIssue FMantleCosmeticChecks::Info(const FString& Message)
{
	return FMantleCosmeticIssue{ EMantleCosmeticSeverity::Info, Message, FString() };
}

bool FMantleCosmeticChecks::HasErrors(TConstArrayView<FMantleCosmeticIssue> Issues)
{
	for (const FMantleCosmeticIssue& Issue : Issues)
	{
		if (Issue.Severity == EMantleCosmeticSeverity::Error)
		{
			return true;
		}
	}
	return false;
}

bool FMantleCosmeticChecks::MatchSkeleton(TConstArrayView<FMantleSkinJointName> Joints, const FMantleContractSkeleton& Skeleton,
	bool bRequireRoot, FString& OutError)
{
	TSet<FName> Present;
	for (const FMantleSkinJointName& Joint : Joints)
	{
		Present.Add(Joint.Name);
	}

	int32 Roots = 0;
	for (const FMantleSkinJointName& Joint : Joints)
	{
		if (Skeleton.Find(Joint.Name) == INDEX_NONE)
		{
			OutError = FString::Printf(TEXT("bone \"%s\" is not on the %s skeleton"), *Joint.Name.ToString(), *Skeleton.Id);
			return false;
		}
		const FName Want = Skeleton.ParentOf(Joint.Name);
		const FName Got = Joints.IsValidIndex(Joint.Parent) ? Joints[Joint.Parent].Name : NAME_None;
		if (!Want.IsNone() && Present.Contains(Want))
		{
			if (Got != Want)
			{
				OutError = FString::Printf(TEXT("bone \"%s\" has parent \"%s\", the %s skeleton has \"%s\""),
					*Joint.Name.ToString(), Got.IsNone() ? TEXT("") : *Got.ToString(), *Skeleton.Id, *Want.ToString());
				return false;
			}
		}
		else
		{
			if (!Got.IsNone())
			{
				OutError = FString::Printf(TEXT("bone \"%s\" has parent \"%s\", the %s skeleton has \"%s\""),
					*Joint.Name.ToString(), *Got.ToString(), *Skeleton.Id, Want.IsNone() ? TEXT("") : *Want.ToString());
				return false;
			}
			++Roots;
		}
	}
	if (Roots != 1)
	{
		OutError = FString::Printf(TEXT("the bones form %d separate hierarchies; a skinned mesh has one root"), Roots);
		return false;
	}
	if (bRequireRoot && !Present.Contains(Skeleton.Root()))
	{
		OutError = FString::Printf(TEXT("the skin has no \"%s\" bone; the %s skeleton's root must be included"),
			*Skeleton.Root().ToString(), *Skeleton.Id);
		return false;
	}
	return true;
}

TArray<FMantleSkinJointName> FMantleCosmeticChecks::JointsOf(const FReferenceSkeleton& Reference)
{
	TArray<FMantleSkinJointName> Joints;
	for (const FMeshBoneInfo& Bone : Reference.GetRawRefBoneInfo())
	{
		FMantleSkinJointName& Joint = Joints.AddDefaulted_GetRef();
		Joint.Name = Bone.Name;
		Joint.Parent = Bone.ParentIndex;
	}
	return Joints;
}

TSet<FName> FMantleCosmeticChecks::SupportedFixtureBones(const FMantleAvatarContract& Contract)
{
	TSet<FName> Bones;
	for (const FString& Id : Contract.AvatarBody.Skeletons)
	{
		if (const FMantleContractSkeleton* Skeleton = Contract.FindSkeleton(Id))
		{
			Bones.Append(Skeleton->Bones);
		}
	}
	// #594: the optional jaw slot is a bone only when picked; unpicked, a bone of its name folds like any extra.
	Bones.Remove(JawSlotBone);
	// #595: so are the secondary motion slots, until mapped.
	for (const FMantleSecondaryChainDef& Chain : MantleSecondaryMotion::Chains())
	{
		for (const FName Bone : Chain.Bones)
		{
			Bones.Remove(Bone);
		}
	}
	return Bones;
}

void FMantleCosmeticChecks::FitSkinToSkeleton(FMantleGlbMesh& Glb, TConstArrayView<FTransform> ComponentPose, TConstArrayView<bool> Weighted,
	const FMantleContractSkeleton& Skeleton, FMantleSkinFit& OutFit, const bool bKeepJawSlot, const TSet<FName>* NoFoldBones,
	const TSet<FName>* KeptSlotBones)
{
	OutFit = FMantleSkinFit();
	const int32 NumBones = Glb.Joints.Num();
	// Per source bone: its index among the kept joints (INDEX_NONE when left out), the kept joint its weights go to,
	// and whether it was folded.
	TArray<int32> KeptIndex;
	KeptIndex.Init(INDEX_NONE, NumBones);
	TArray<int32> WeightTarget;
	WeightTarget.Init(INDEX_NONE, NumBones);
	TArray<bool> bFolded;
	bFolded.Init(false, NumBones);
	TArray<FMantleGlbJoint> Kept;
	for (int32 Bone = 0; Bone < NumBones; ++Bone)
	{
		const FMantleGlbJoint& Source = Glb.Joints[Bone];
		// The nearest kept ancestor. Joints are parent-first, so every ancestor has been decided; the step bound only
		// guards a malformed table.
		int32 Ancestor = Source.Parent;
		for (int32 Steps = 0; Ancestor != INDEX_NONE; ++Steps)
		{
			if (Steps > NumBones || !KeptIndex.IsValidIndex(Ancestor))
			{
				Ancestor = INDEX_NONE;
				break;
			}
			if (Ancestor < Bone && KeptIndex[Ancestor] != INDEX_NONE)
			{
				break;
			}
			Ancestor = Glb.Joints[Ancestor].Parent;
		}
		// #594: the jaw slot is on the skeleton only for a jaw the creator picked; an unpicked bone of its name is an extra.
		// #595: so are the secondary motion slots, for a chain the creator mapped.
		const bool bOptionalSlotLeftOut = (!bKeepJawSlot && Source.Name == JawSlotBone)
			|| (MantleSecondaryMotion::IsSlotBone(Source.Name) && !(KeptSlotBones && KeptSlotBones->Contains(Source.Name)));
		const int32 SkeletonBone = bOptionalSlotLeftOut ? INDEX_NONE : Skeleton.Find(Source.Name);
		const bool bWeighted = Weighted.IsValidIndex(Bone) && Weighted[Bone];
		// A weighted bone of another supported skeleton (#593: spine_05 on a MetaHuman body tried as a UE4 Mannequin one)
		// is not an extra to fold: it is kept, and MatchSkeleton refuses it as the worker would.
		const bool bOtherSkeletonsBone = bWeighted && SkeletonBone == INDEX_NONE && NoFoldBones && NoFoldBones->Contains(Source.Name);
		if (SkeletonBone == INDEX_NONE && !bOtherSkeletonsBone && (!bWeighted || Ancestor != INDEX_NONE))
		{
			// Off the skeleton: an unweighted helper is left out; a weighted bone is folded into its nearest kept
			// ancestor. A weighted bone with no kept ancestor is kept below, for MatchSkeleton to refuse.
			++OutFit.Dropped;
			WeightTarget[Bone] = Ancestor != INDEX_NONE ? KeptIndex[Ancestor] : INDEX_NONE;
			if (bWeighted)
			{
				bFolded[Bone] = true;
				OutFit.Folded.Add(FMantleFoldedBone{ Source.Name, Kept[KeptIndex[Ancestor]].Name });
			}
			continue;
		}
		FMantleGlbJoint Joint;
		Joint.Name = SkeletonBone != INDEX_NONE ? Skeleton.Bones[SkeletonBone] : Source.Name;
		Joint.Parent = Ancestor == INDEX_NONE ? INDEX_NONE : KeptIndex[Ancestor];
		if (Ancestor == Source.Parent)
		{
			Joint.Local = Source.Local; // exact, no round trip
		}
		else
		{
			const FTransform Pose = ComponentPose.IsValidIndex(Bone) ? ComponentPose[Bone] : Source.Local;
			Joint.Local = Ancestor == INDEX_NONE || !ComponentPose.IsValidIndex(Ancestor) ? Pose : Pose.GetRelativeTransform(ComponentPose[Ancestor]);
		}
		KeptIndex[Bone] = Kept.Num();
		WeightTarget[Bone] = Kept.Num();
		Kept.Add(Joint);
		OutFit.Names.Add(FMantleSkinJointName{ Joint.Name, Joint.Parent });
	}

	// The weights onto the kept joints. A vertex a folded bone weighs has its influences on one joint merged, renormalised
	// and sorted heaviest first; any other keeps its slots as they were, only renumbered.
	const int32 Slots = Glb.NumInfluences;
	const int32 NumVertices = Slots > 0 ? Glb.InfluenceJoints.Num() / Slots : 0;
	const bool bHasWeights = Glb.InfluenceWeights.Num() == Glb.InfluenceJoints.Num();
	TArray<TPair<int32, float>> Influences;
	for (int32 Vertex = 0; Vertex < NumVertices; ++Vertex)
	{
		Influences.Reset();
		bool bTouched = false;
		for (int32 Slot = 0; Slot < Slots; ++Slot)
		{
			const int32 Index = Vertex * Slots + Slot;
			const int32 SourceJoint = Glb.InfluenceJoints[Index];
			const int32 Joint = WeightTarget.IsValidIndex(SourceJoint) ? FMath::Max(0, WeightTarget[SourceJoint]) : 0;
			Glb.InfluenceJoints[Index] = static_cast<uint16>(Joint);
			const float Weight = bHasWeights ? Glb.InfluenceWeights[Index] : 0.0f;
			if (Weight <= 0.0f)
			{
				continue;
			}
			bTouched = bTouched || (bFolded.IsValidIndex(SourceJoint) && bFolded[SourceJoint]);
			TPair<int32, float>* Existing = Influences.FindByPredicate([Joint](const TPair<int32, float>& Pair) { return Pair.Key == Joint; });
			if (Existing)
			{
				Existing->Value += Weight;
			}
			else
			{
				Influences.Add(TPair<int32, float>(Joint, Weight));
			}
		}
		if (bTouched)
		{
			float Total = 0.0f;
			for (const TPair<int32, float>& Influence : Influences)
			{
				Total += Influence.Value;
			}
			if (Total > 0.0f)
			{
				for (TPair<int32, float>& Influence : Influences)
				{
					Influence.Value /= Total;
				}
			}
			// Merging only ever lowers the count, so this sorts; it would drop the lightest past the slots.
			CapInfluences(Influences, Slots);
			for (int32 Slot = 0; Slot < Slots; ++Slot)
			{
				const int32 Index = Vertex * Slots + Slot;
				Glb.InfluenceJoints[Index] = Influences.IsValidIndex(Slot) ? static_cast<uint16>(Influences[Slot].Key) : 0;
				Glb.InfluenceWeights[Index] = Influences.IsValidIndex(Slot) ? Influences[Slot].Value : 0.0f;
			}
		}
		OutFit.MostInfluences = FMath::Max(OutFit.MostInfluences, FMath::Min(Influences.Num(), Slots));
	}
	Glb.Joints = MoveTemp(Kept);
}

bool FMantleCosmeticChecks::IsBetterSkeletonFit(const FMantleSkinFit& Candidate, const FMantleSkinFit& Best)
{
	if (Candidate.Folded.Num() != Best.Folded.Num())
	{
		return Candidate.Folded.Num() < Best.Folded.Num();
	}
	return Candidate.Dropped < Best.Dropped;
}

const FMantleContractSkeleton* FMantleCosmeticChecks::FitSkinToClass(FMantleGlbMesh& Glb, TConstArrayView<FTransform> ComponentPose,
	TConstArrayView<bool> Weighted, const FMantleContractClass& Class, const FMantleAvatarContract& Contract, bool bRequireRoot,
	FMantleSkinFit& OutFit, FString& OutError, const bool bKeepJawSlot)
{
	FString FirstError;
	const FMantleContractSkeleton* Best = nullptr;
	FMantleGlbMesh BestGlb;
	FMantleSkinFit BestFit;
	// Decided for #593: a weighted bone of any supported skeleton is never folded away to fit another one.
	const TSet<FName> NoFold = SupportedFixtureBones(Contract);
	for (const FString& Id : Class.Skeletons)
	{
		const FMantleContractSkeleton* Skeleton = Contract.FindSkeleton(Id);
		if (!Skeleton)
		{
			continue;
		}
		FMantleGlbMesh Candidate = Glb;
		FMantleSkinFit Fit;
		FitSkinToSkeleton(Candidate, ComponentPose, Weighted, *Skeleton, Fit, bKeepJawSlot, &NoFold);
		FString SkeletonError;
		if (MatchSkeleton(Fit.Names, *Skeleton, bRequireRoot, SkeletonError))
		{
			if (!Best || IsBetterSkeletonFit(Fit, BestFit))
			{
				Best = Skeleton;
				BestGlb = MoveTemp(Candidate);
				BestFit = MoveTemp(Fit);
			}
			continue;
		}
		if (FirstError.IsEmpty())
		{
			FirstError = SkeletonError;
		}
	}
	if (Best)
	{
		Glb = MoveTemp(BestGlb);
		OutFit = MoveTemp(BestFit);
		return Best;
	}
	OutError = UnsupportedSkeleton(FirstError.IsEmpty() ? FString(TEXT("no supported skeleton is available for this kind of upload")) : FirstError);
	return nullptr;
}

FMantleCosmeticIssue FMantleCosmeticChecks::FoldedBonesIssue(const FString& What, TConstArrayView<FMantleFoldedBone> Folded)
{
	TArray<FString> Pairs;
	for (int32 Index = 0; Index < Folded.Num() && Index < FoldedBonesListed; ++Index)
	{
		Pairs.Add(FString::Printf(TEXT("%s -> %s"), *Folded[Index].Bone.ToString(), *Folded[Index].Into.ToString()));
	}
	FString List = FString::Join(Pairs, TEXT(", "));
	if (Folded.Num() > FoldedBonesListed)
	{
		List += FString::Printf(TEXT(", and %d more"), Folded.Num() - FoldedBonesListed);
	}
	FString Message = Folded.Num() == 1
		? FString::Printf(TEXT("1 bone was merged into its parent bone and won't move on its own: %s."), *List)
		: FString::Printf(TEXT("%d bones were merged into their parent bones and won't move on their own: %s."), Folded.Num(), *List);
	if (!What.IsEmpty())
	{
		Message = What + TEXT(": ") + Message;
	}
	return Warning(Message,
		TEXT("Mantle animates only the bones of its skeleton, so what they moved (a jaw, hair strands, cloth) now follows the parent bone. It still uploads."));
}

bool FMantleCosmeticChecks::HasJawSlot(const FMantleContractSkeleton& Skeleton)
{
	return Skeleton.Find(JawSlotBone) != INDEX_NONE && Skeleton.ParentOf(JawSlotBone) == JawSlotParent;
}

TArray<FName> FMantleCosmeticChecks::JawCandidates(TConstArrayView<FMantleSkinJointName> Joints)
{
	TArray<FName> Out;
	int32 Head = INDEX_NONE;
	for (int32 Bone = 0; Bone < Joints.Num(); ++Bone)
	{
		if (Joints[Bone].Name == JawSlotParent)
		{
			Head = Bone;
			break;
		}
	}
	if (Head == INDEX_NONE)
	{
		return Out;
	}
	// Parent-first, so a bone is below the head when its parent is the head or below it.
	TArray<bool> Below;
	Below.Init(false, Joints.Num());
	for (int32 Bone = Head + 1; Bone < Joints.Num(); ++Bone)
	{
		const int32 Parent = Joints[Bone].Parent;
		Below[Bone] = Parent == Head || (Parent > Head && Parent < Bone && Below[Parent]);
		if (Below[Bone])
		{
			Out.Add(Joints[Bone].Name);
		}
	}
	return Out;
}

FName FMantleCosmeticChecks::PreselectJawBone(TConstArrayView<FName> Candidates)
{
	FName Found = NAME_None;
	int32 Count = 0;
	for (const FName Candidate : Candidates)
	{
		if (Candidate.ToString().Contains(TEXT("jaw"), ESearchCase::IgnoreCase))
		{
			Found = Candidate;
			++Count;
		}
	}
	return Count == 1 ? Found : NAME_None;
}

bool FMantleCosmeticChecks::KeepJawBone(FMantleGlbMesh& Glb, TConstArrayView<FTransform> ComponentPose, const FName Picked, FString& OutError)
{
	const int32 NumBones = Glb.Joints.Num();
	int32 Bone = INDEX_NONE;
	for (int32 Index = 0; Index < NumBones && !Picked.IsNone(); ++Index)
	{
		if (Glb.Joints[Index].Name == Picked)
		{
			Bone = Index;
			break;
		}
	}
	if (Bone == INDEX_NONE)
	{
		OutError = FString::Printf(TEXT("the jaw bone \"%s\" is not a bone of the body"), *Picked.ToString());
		return false;
	}
	// Its nearest ancestor named like the slot's parent; the step bound only guards a malformed table.
	int32 Head = INDEX_NONE;
	int32 Steps = 0;
	for (int32 Ancestor = Glb.Joints[Bone].Parent; Glb.Joints.IsValidIndex(Ancestor) && Steps <= NumBones; Ancestor = Glb.Joints[Ancestor].Parent, ++Steps)
	{
		if (Glb.Joints[Ancestor].Name == JawSlotParent)
		{
			Head = Ancestor;
			break;
		}
	}
	if (Head == INDEX_NONE)
	{
		OutError = FString::Printf(TEXT("the jaw bone \"%s\" is not below the %s bone"), *Picked.ToString(), *JawSlotParent.ToString());
		return false;
	}
	for (int32 Index = 0; Index < NumBones; ++Index)
	{
		if (Index != Bone && Glb.Joints[Index].Name == JawSlotBone)
		{
			OutError = FString::Printf(TEXT("the body already has a bone named \"%s\" besides \"%s\""), *JawSlotBone.ToString(), *Picked.ToString());
			return false;
		}
	}
	// Its reference pose relative to the head's: the component transform it had under its own parent.
	FTransform Local = Glb.Joints[Bone].Local;
	if (ComponentPose.IsValidIndex(Bone) && ComponentPose.IsValidIndex(Head))
	{
		Local = ComponentPose[Bone].GetRelativeTransform(ComponentPose[Head]);
	}
	else
	{
		Steps = 0;
		for (int32 Ancestor = Glb.Joints[Bone].Parent; Ancestor != Head && Glb.Joints.IsValidIndex(Ancestor) && Steps <= NumBones;
			Ancestor = Glb.Joints[Ancestor].Parent, ++Steps)
		{
			Local = Local * Glb.Joints[Ancestor].Local;
		}
	}
	FMantleGlbJoint& Joint = Glb.Joints[Bone];
	Joint.Name = JawSlotBone;
	Joint.Parent = Head;
	Joint.Local = Local;
	return true;
}

const FMantleContractSkeleton* FMantleCosmeticChecks::FitAvatarBodyWithSlots(FMantleGlbMesh& Glb, TConstArrayView<FTransform> ComponentPose,
	TConstArrayView<bool> Weighted, const FName PickedJaw, TConstArrayView<FMantleSecondaryMapping> Mappings, const FMantleContractClass& Class,
	const FMantleAvatarContract& Contract, FMantleAvatarBodyFit& Out, FString& OutError)
{
	Out = FMantleAvatarBodyFit();
	// The skin as read, for the second fit: the slots are kept on the source joints, before anything is left out.
	FMantleGlbMesh Source;
	if (!PickedJaw.IsNone() || Mappings.Num() > 0)
	{
		Source = Glb;
	}
	const FMantleContractSkeleton* Skeleton = FitSkinToClass(Glb, ComponentPose, Weighted, Class, Contract, true, Out.Fit, OutError);
	if (!Skeleton)
	{
		return nullptr;
	}
	const bool bJawSlot = HasJawSlot(*Skeleton);
	Out.bSecondarySlots = HasSecondarySlots(*Skeleton);
	const bool bKeepJaw = bJawSlot && !PickedJaw.IsNone();
	const bool bKeepChains = Out.bSecondarySlots && Mappings.Num() > 0;
	if (bJawSlot && !bKeepJaw)
	{
		Out.Jaw = EMantleJawSlot::NotPicked;
	}
	if (!bKeepJaw && !bKeepChains)
	{
		return Skeleton;
	}
	if (bKeepJaw && !KeepJawBone(Source, ComponentPose, PickedJaw, OutError))
	{
		Out.Jaw = EMantleJawSlot::Refused;
		return nullptr;
	}
	TSet<FName> KeptSlots;
	if (bKeepChains)
	{
		if (!KeepSecondaryChains(Source, ComponentPose, Mappings, *Skeleton, Out.Kept, OutError))
		{
			Out.bSecondaryRefused = true;
			Out.Kept.Reset();
			return nullptr;
		}
		for (const FMantleSecondaryKept& Kept : Out.Kept)
		{
			const FMantleSecondaryChainDef* Chain = MantleSecondaryMotion::FindChain(Kept.ChainId);
			for (int32 Index = 0; Chain && Index < Kept.SourceBones.Num() && Index < Chain->Bones.Num(); ++Index)
			{
				KeptSlots.Add(Chain->Bones[Index]);
			}
		}
	}
	FMantleSkinFit Fit;
	const TSet<FName> NoFold = SupportedFixtureBones(Contract);
	FitSkinToSkeleton(Source, ComponentPose, Weighted, *Skeleton, Fit, bKeepJaw, &NoFold, &KeptSlots);
	FString MatchError;
	if (!MatchSkeleton(Fit.Names, *Skeleton, true, MatchError))
	{
		// A bone kept under its fixture parent cannot break a fit that held without it; guarded all the same.
		Out.Jaw = bKeepJaw ? EMantleJawSlot::Refused : Out.Jaw;
		Out.bSecondaryRefused = bKeepChains;
		Out.Kept.Reset();
		OutError = UnsupportedSkeleton(MatchError);
		return nullptr;
	}
	Glb = MoveTemp(Source);
	Out.Fit = MoveTemp(Fit);
	if (bKeepJaw)
	{
		Out.Jaw = EMantleJawSlot::Kept;
	}
	return Skeleton;
}

FMantleCosmeticIssue FMantleCosmeticChecks::JawSlotIssue(const FName KeptFrom)
{
	if (KeptFrom.IsNone())
	{
		return Warning(TEXT("no jaw bone: this avatar's mouth won't move with voice chat."),
			TEXT("If the body has a jaw bone, pick it in Jaw bone (optional)."));
	}
	return Warning(FString::Printf(TEXT("the mouth moves with voice chat using %s."), *KeptFrom.ToString()));
}

FName FMantleCosmeticChecks::AvatarJawBone(const EMantleJawSlot Jaw, const FName Picked)
{
	if (Jaw == EMantleJawSlot::Kept)
	{
		return JawSlotBone;
	}
	return Jaw == EMantleJawSlot::NoSlot ? Picked : NAME_None;
}

bool FMantleCosmeticChecks::HasSecondarySlots(const FMantleContractSkeleton& Skeleton)
{
	const TConstArrayView<FMantleSecondaryChainDef> Chains = MantleSecondaryMotion::Chains();
	for (const FMantleSecondaryChainDef& Chain : Chains)
	{
		if (Chain.Bones.Num() == 0 || Skeleton.Find(Chain.Bones[0]) == INDEX_NONE)
		{
			return false;
		}
	}
	return Chains.Num() > 0;
}

bool FMantleCosmeticChecks::IsSkeletonBoneForMapping(const FMantleContractSkeleton& Skeleton, const FName Bone)
{
	return Skeleton.Find(Bone) != INDEX_NONE && !MantleSecondaryMotion::IsSlotBone(Bone);
}

TArray<FName> FMantleCosmeticChecks::SecondaryCandidates(TConstArrayView<FMantleSkinJointName> Joints, const FMantleContractSkeleton& Skeleton)
{
	TArray<FName> Out;
	for (const FMantleSkinJointName& Joint : Joints)
	{
		if (!IsSkeletonBoneForMapping(Skeleton, Joint.Name))
		{
			Out.Add(Joint.Name);
		}
	}
	return Out;
}

TArray<int32> FMantleCosmeticChecks::SecondaryChainFrom(TConstArrayView<FMantleSkinJointName> Joints, const int32 Start,
	const FMantleContractSkeleton& Skeleton)
{
	TArray<int32> Chain;
	if (!Joints.IsValidIndex(Start))
	{
		return Chain;
	}
	Chain.Add(Start);
	for (int32 Current = Start; Chain.Num() <= Joints.Num();)
	{
		// Parent-first, so a child comes after its parent.
		int32 Next = INDEX_NONE;
		for (int32 Bone = Current + 1; Bone < Joints.Num(); ++Bone)
		{
			if (Joints[Bone].Parent == Current && !IsSkeletonBoneForMapping(Skeleton, Joints[Bone].Name))
			{
				Next = Bone;
				break;
			}
		}
		if (Next == INDEX_NONE)
		{
			break;
		}
		Chain.Add(Next);
		Current = Next;
	}
	return Chain;
}

FString FMantleCosmeticChecks::SecondaryKindOf(const FName Bone)
{
	const FString Name = Bone.ToString().ToLower();
	auto Has = [&Name](std::initializer_list<const TCHAR*> Words)
	{
		for (const TCHAR* Word : Words)
		{
			if (Name.Contains(Word, ESearchCase::CaseSensitive))
			{
				return true;
			}
		}
		return false;
	};
	if (Has({ TEXT("breast"), TEXT("pector"), TEXT("chest"), TEXT("boob") }))
	{
		return TEXT("chest");
	}
	if (Has({ TEXT("hair"), TEXT("ponytail"), TEXT("braid"), TEXT("pigtail") }))
	{
		return TEXT("hair");
	}
	if (Has({ TEXT("tail") }))
	{
		return TEXT("tail");
	}
	if (Has({ TEXT("cape"), TEXT("cloak"), TEXT("back") }))
	{
		return TEXT("back");
	}
	if (Has({ TEXT("skirt"), TEXT("sleeve"), TEXT("coat"), TEXT("cloth"), TEXT("dress"), TEXT("robe"), TEXT("scarf"), TEXT("tassel") }))
	{
		return TEXT("cloth");
	}
	return FString();
}

namespace MantleSecondaryMappingPrivate
{
	/** "l" or "r" when a bone's name says its side (_l, _l_, l_ at the start, left), else null. */
	const TCHAR* SecondarySideOf(const FName Bone)
	{
		const FString Name = Bone.ToString().ToLower();
		struct FSide
		{
			const TCHAR* Letter;
			const TCHAR* Word;
		};
		const FSide Sides[] = { { TEXT("l"), TEXT("left") }, { TEXT("r"), TEXT("right") } };
		for (const FSide& Side : Sides)
		{
			const FString Suffix = FString(TEXT("_")) + Side.Letter;
			const FString Infix = Suffix + TEXT("_");
			const FString Prefix = FString(Side.Letter) + TEXT("_");
			if (Name.EndsWith(Suffix, ESearchCase::CaseSensitive) || Name.Contains(Infix, ESearchCase::CaseSensitive)
				|| Name.StartsWith(Prefix, ESearchCase::CaseSensitive) || Name.Contains(Side.Word, ESearchCase::CaseSensitive))
			{
				return Side.Letter;
			}
		}
		return nullptr;
	}

	/** Whether Bone (an index into Joints) hangs below a bone named Parent. */
	bool SecondaryIsBelow(TConstArrayView<FMantleSkinJointName> Joints, const int32 Bone, const FName Parent)
	{
		int32 Steps = 0;
		for (int32 Ancestor = Joints.IsValidIndex(Bone) ? Joints[Bone].Parent : INDEX_NONE; Joints.IsValidIndex(Ancestor) && Steps <= Joints.Num();
			Ancestor = Joints[Ancestor].Parent, ++Steps)
		{
			if (Joints[Ancestor].Name == Parent)
			{
				return true;
			}
		}
		return false;
	}
}

TArray<FMantleSecondaryMapping> FMantleCosmeticChecks::SuggestSecondaryMappings(TConstArrayView<FMantleSkinJointName> Joints,
	const FMantleContractSkeleton& Skeleton)
{
	using namespace MantleSecondaryMappingPrivate;
	TArray<FMantleSecondaryMapping> Out;
	// How many extra (off-skeleton) children each bone has: a helper with several is a root the chains hang from.
	TArray<int32> ExtraChildren;
	ExtraChildren.Init(0, Joints.Num());
	for (int32 Bone = 0; Bone < Joints.Num(); ++Bone)
	{
		if (!IsSkeletonBoneForMapping(Skeleton, Joints[Bone].Name) && ExtraChildren.IsValidIndex(Joints[Bone].Parent))
		{
			++ExtraChildren[Joints[Bone].Parent];
		}
	}
	TSet<FString> Taken;
	for (int32 Bone = 0; Bone < Joints.Num(); ++Bone)
	{
		const FName Name = Joints[Bone].Name;
		const int32 Parent = Joints[Bone].Parent;
		if (IsSkeletonBoneForMapping(Skeleton, Name) || !Joints.IsValidIndex(Parent) || ExtraChildren[Bone] >= 2)
		{
			continue;
		}
		const bool bTop = IsSkeletonBoneForMapping(Skeleton, Joints[Parent].Name) || ExtraChildren[Parent] >= 2;
		const FString Kind = bTop ? SecondaryKindOf(Name) : FString();
		if (Kind.IsEmpty())
		{
			continue;
		}
		const TCHAR* Side = Kind.Equals(TEXT("chest"), ESearchCase::CaseSensitive) ? SecondarySideOf(Name) : nullptr;
		const FMantleSecondaryChainDef* Choice = nullptr;
		for (const FMantleSecondaryChainDef& Chain : MantleSecondaryMotion::Chains())
		{
			if (!Chain.Id.StartsWith(Kind, ESearchCase::CaseSensitive) || Taken.Contains(Chain.Id) || Chain.Bones.Num() == 0)
			{
				continue;
			}
			const FName SlotParent = Skeleton.ParentOf(Chain.Bones[0]);
			if (SlotParent.IsNone() || !SecondaryIsBelow(Joints, Bone, SlotParent))
			{
				continue;
			}
			if (Side && !Chain.Id.EndsWith(FString(TEXT("_")) + Side, ESearchCase::CaseSensitive))
			{
				// The chest chain of the other side: taken only when this side's is gone.
				if (!Choice)
				{
					Choice = &Chain;
				}
				continue;
			}
			Choice = &Chain;
			break;
		}
		if (Choice)
		{
			Taken.Add(Choice->Id);
			FMantleSecondaryMapping& Mapping = Out.AddDefaulted_GetRef();
			Mapping.ChainId = Choice->Id;
			Mapping.StartBone = Name;
			Mapping.Preset = Choice->DefaultPreset;
		}
	}
	return Out;
}

bool FMantleCosmeticChecks::KeepSecondaryChains(FMantleGlbMesh& Glb, TConstArrayView<FTransform> ComponentPose,
	TConstArrayView<FMantleSecondaryMapping> Mappings, const FMantleContractSkeleton& Skeleton, TArray<FMantleSecondaryKept>& OutKept,
	FString& OutError)
{
	using namespace MantleSecondaryMappingPrivate;
	OutKept.Reset();
	const int32 NumBones = Glb.Joints.Num();
	TArray<bool> Used;
	Used.Init(false, NumBones);
	TSet<FString> Mapped;
	// Every start by the body's own names, before any mapping renames a bone.
	TArray<int32> Starts;
	for (const FMantleSecondaryMapping& Mapping : Mappings)
	{
		Starts.Add(Mapping.StartBone.IsNone() ? INDEX_NONE
			: Glb.Joints.IndexOfByPredicate([&Mapping](const FMantleGlbJoint& Joint) { return Joint.Name == Mapping.StartBone; }));
	}
	for (int32 MappingIndex = 0; MappingIndex < Mappings.Num(); ++MappingIndex)
	{
		const FMantleSecondaryMapping& Mapping = Mappings[MappingIndex];
		const FMantleSecondaryChainDef* Chain = MantleSecondaryMotion::FindChain(Mapping.ChainId);
		if (!Chain || Chain->Bones.Num() == 0)
		{
			OutError = FString::Printf(TEXT("\"%s\" is not a secondary motion slot"), *Mapping.ChainId);
			return false;
		}
		if (Mapped.Contains(Chain->Id))
		{
			OutError = FString::Printf(TEXT("the %s secondary motion slot is mapped twice"), *Chain->Id);
			return false;
		}
		Mapped.Add(Chain->Id);
		const FName SlotParent = Skeleton.ParentOf(Chain->Bones[0]);
		if (SlotParent.IsNone())
		{
			OutError = FString::Printf(TEXT("the %s skeleton has no %s secondary motion slot"), *Skeleton.Id, *Chain->Id);
			return false;
		}

		// The joints as the chain walk sees them, with this mapping's predecessors already renamed.
		TArray<FMantleSkinJointName> Joints;
		Joints.Reserve(NumBones);
		for (const FMantleGlbJoint& Joint : Glb.Joints)
		{
			Joints.Add(FMantleSkinJointName{ Joint.Name, Joint.Parent });
		}
		const int32 Start = Starts[MappingIndex];
		if (Start == INDEX_NONE)
		{
			OutError = FString::Printf(TEXT("the bone \"%s\" for the %s slot is not a bone of the body"), *Mapping.StartBone.ToString(), *Chain->Id);
			return false;
		}
		if (IsSkeletonBoneForMapping(Skeleton, Mapping.StartBone))
		{
			OutError = FString::Printf(TEXT("\"%s\" is a bone of the %s skeleton, not an extra bone to map to the %s slot"),
				*Mapping.StartBone.ToString(), *Skeleton.Id, *Chain->Id);
			return false;
		}
		// The slot's parent: the start's nearest ancestor of that name.
		int32 Parent = INDEX_NONE;
		int32 Steps = 0;
		for (int32 Ancestor = Joints[Start].Parent; Joints.IsValidIndex(Ancestor) && Steps <= NumBones; Ancestor = Joints[Ancestor].Parent, ++Steps)
		{
			if (Joints[Ancestor].Name == SlotParent)
			{
				Parent = Ancestor;
				break;
			}
		}
		if (Parent == INDEX_NONE)
		{
			OutError = FString::Printf(TEXT("\"%s\" is not below the %s bone, where the %s slot hangs"),
				*Mapping.StartBone.ToString(), *SlotParent.ToString(), *Chain->Id);
			return false;
		}
		const TArray<int32> Bones = SecondaryChainFrom(Joints, Start, Skeleton);
		const int32 Count = FMath::Min(Bones.Num(), Chain->Bones.Num());
		for (int32 Index = 0; Index < Count; ++Index)
		{
			if (Used[Bones[Index]])
			{
				OutError = FString::Printf(TEXT("\"%s\" is in two mapped secondary motion chains"), *Joints[Bones[Index]].Name.ToString());
				return false;
			}
			for (int32 Other = 0; Other < NumBones; ++Other)
			{
				if (Other != Bones[Index] && Joints[Other].Name == Chain->Bones[Index])
				{
					OutError = FString::Printf(TEXT("the body already has a bone named \"%s\" besides \"%s\""),
						*Chain->Bones[Index].ToString(), *Joints[Bones[Index]].Name.ToString());
					return false;
				}
			}
		}

		// The first bone's reference pose relative to the slot's parent: the component transform it had under its own.
		FTransform Local = Glb.Joints[Start].Local;
		if (ComponentPose.IsValidIndex(Start) && ComponentPose.IsValidIndex(Parent))
		{
			Local = ComponentPose[Start].GetRelativeTransform(ComponentPose[Parent]);
		}
		else
		{
			Steps = 0;
			for (int32 Ancestor = Glb.Joints[Start].Parent; Ancestor != Parent && Glb.Joints.IsValidIndex(Ancestor) && Steps <= NumBones;
				Ancestor = Glb.Joints[Ancestor].Parent, ++Steps)
			{
				Local = Local * Glb.Joints[Ancestor].Local;
			}
		}
		Glb.Joints[Start].Parent = Parent;
		Glb.Joints[Start].Local = Local;

		FMantleSecondaryKept& Kept = OutKept.AddDefaulted_GetRef();
		Kept.ChainId = Chain->Id;
		Kept.Preset = Mapping.Preset;
		Kept.FoldedPastEnd = Bones.Num() - Count;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Kept.SourceBones.Add(Glb.Joints[Bones[Index]].Name);
			Glb.Joints[Bones[Index]].Name = Chain->Bones[Index];
			Used[Bones[Index]] = true;
		}
	}
	return true;
}

FMantleCosmeticIssue FMantleCosmeticChecks::SecondaryMotionIssue(TConstArrayView<FMantleSecondaryKept> Kept)
{
	TArray<FString> Chains;
	for (const FMantleSecondaryKept& Chain : Kept)
	{
		Chains.Add(FString::Printf(TEXT("%s (%s, %d %s, %s)"), *Chain.ChainId,
			Chain.SourceBones.Num() > 0 ? *Chain.SourceBones[0].ToString() : TEXT(""), Chain.SourceBones.Num(),
			Chain.SourceBones.Num() == 1 ? TEXT("bone") : TEXT("bones"), *MantleSecondaryMotion::PresetDisplayName(Chain.Preset).ToString()));
	}
	if (Chains.Num() == 0)
	{
		return Warning(TEXT("no secondary motion: hair, chest and cloth bones move rigidly with their parents."),
			TEXT("Map the body's hair, chest, cloth or tail bones in Secondary motion to let them swing."));
	}
	return Warning(FString::Printf(TEXT("these swing as the avatar moves: %s."), *FString::Join(Chains, TEXT(", "))));
}

int32 FMantleCosmeticChecks::ReductionPercent(int32 Triangles, int32 Limit)
{
	if (Triangles <= 0 || Limit <= 0)
	{
		return 100;
	}
	return FMath::Clamp(static_cast<int32>((static_cast<int64>(Limit) * 100) / Triangles), 1, 100);
}

float FMantleCosmeticChecks::CapInfluences(TArray<TPair<int32, float>>& Influences, int32 MaxInfluences)
{
	auto Heaviest = [](const TPair<int32, float>& A, const TPair<int32, float>& B) { return A.Value > B.Value; };
	Influences.StableSort(Heaviest);
	if (MaxInfluences <= 0 || Influences.Num() <= MaxInfluences)
	{
		return 0.0f;
	}
	double Total = 0.0;
	for (const TPair<int32, float>& Influence : Influences)
	{
		Total += FMath::Max(0.0f, Influence.Value);
	}
	const float LargestDropped = Total > 0.0 ? static_cast<float>(FMath::Max(0.0f, Influences[MaxInfluences].Value) / Total) : 0.0f;
	Influences.SetNum(MaxInfluences);
	double Kept = 0.0;
	for (const TPair<int32, float>& Influence : Influences)
	{
		Kept += FMath::Max(0.0f, Influence.Value);
	}
	if (Kept <= 0.0)
	{
		return LargestDropped;
	}

	// Largest remainder to exactly 255, the worker's rounding: the file's weights are what MSK1 will store.
	constexpr int32 FixedOne = 255;
	TArray<int32, TInlineAllocator<8>> Quantised;
	TArray<double, TInlineAllocator<8>> Remainder;
	int32 Sum = 0;
	for (const TPair<int32, float>& Influence : Influences)
	{
		const double Exact = FMath::Max(0.0f, Influence.Value) / Kept * FixedOne;
		const int32 Floor = FMath::FloorToInt32(Exact);
		Quantised.Add(Floor);
		Remainder.Add(Exact - Floor);
		Sum += Floor;
	}
	TArray<int32, TInlineAllocator<8>> ByRemainder;
	for (int32 Index = 0; Index < Influences.Num(); ++Index)
	{
		ByRemainder.Add(Index);
	}
	ByRemainder.StableSort([&Remainder](int32 A, int32 B) { return Remainder[A] > Remainder[B]; });
	for (int32 Step = 0; Sum < FixedOne; ++Step)
	{
		++Quantised[ByRemainder[Step % ByRemainder.Num()]];
		++Sum;
	}
	for (int32 Index = 0; Index < Influences.Num(); ++Index)
	{
		Influences[Index].Value = static_cast<float>(Quantised[Index]) / FixedOne;
	}
	Influences.StableSort(Heaviest);
	return LargestDropped;
}

FMantleCosmeticIssue FMantleCosmeticChecks::CappedInfluencesIssue(const FString& What, int32 Vertices, float LargestDropped, int32 MaxInfluences)
{
	const FString Lead = What.IsEmpty() ? FString() : What + TEXT(": ");
	const FString Message = FString::Printf(TEXT("%s%d %s more than %d influences; the lightest were dropped and the rest renormalised (largest weight dropped: %.3f)."),
		*Lead, Vertices, Vertices == 1 ? TEXT("vertex had") : TEXT("vertices had"), MaxInfluences, LargestDropped);
	if (LargestDropped > CappedInfluenceWarning)
	{
		return Warning(Message, FString::Printf(TEXT("The deformation may change visibly there. To choose the weights yourself, set the mesh's Max Bone Influences (LOD settings) to %d and reimport."),
			MaxInfluences));
	}
	return Info(Message);
}

FString FMantleCosmeticChecks::TriangleLimitHint(int32 Triangles, int32 Limit, int32 NumLODs, EMantleLODPick Pick)
{
	const int32 Percent = ReductionPercent(Triangles, Limit);
	// Percent of Triangles is a ratio in the editor (0.07 keeps 7%).
	const FString Reduce = FString::Printf(TEXT("set Termination Criterion to Max Triangles and Max Triangle Count to %d (or Percent of Triangles to %.2f)"),
		Limit, Percent / 100.0);
	switch (Pick)
	{
	case EMantleLODPick::FirstWithinLimit:
		if (NumLODs <= 1)
		{
			return FString::Printf(TEXT("It has no lower LOD. Add one in the Skeletal Mesh editor: LOD Settings > Number of LODs 2, Apply Changes, then in LOD 1's Reduction Settings %s and Apply Changes. The plugin uploads the first LOD within the limit."),
				*Reduce);
		}
		return FString::Printf(TEXT("None of its %d LODs is within the limit. Reduce its last LOD in the Skeletal Mesh editor: in that LOD's Reduction Settings %s and Apply Changes. The plugin uploads the first LOD within the limit."),
			NumLODs, *Reduce);
	case EMantleLODPick::LODZeroOnly:
		return FString::Printf(TEXT("Mantle takes LOD 0 of this mesh as it is. Upload a coarser version of it, or reduce LOD 0 in its mesh editor (LOD 0 > Reduction Settings) to about %d%% of its triangles."),
			Percent);
	case EMantleLODPick::Unknown:
	default:
		return TEXT("Use a lower LOD of the mesh, or reduce it (Skeletal Mesh editor > LOD Settings, or Modeling > Simplify).");
	}
}

void FMantleCosmeticChecks::CheckMeshLimits(int32 Triangles, int32 Vertices, int32 Sections, int32 MostInfluences,
	const FMantleContractClass& Class, TArray<FMantleCosmeticIssue>& OutIssues, int32 NumLODs, EMantleLODPick Pick)
{
	if (Class.MaxTriangles > 0 && Triangles > Class.MaxTriangles)
	{
		OutIssues.Add(Error(FString::Printf(TEXT("%d triangles, the limit is %d."), Triangles, Class.MaxTriangles),
			TriangleLimitHint(Triangles, Class.MaxTriangles, NumLODs, Pick)));
	}
	if (Class.MaxTriangles > 0 && Vertices > 3 * Class.MaxTriangles)
	{
		OutIssues.Add(Error(FString::Printf(TEXT("%d vertices, the limit is %d."), Vertices, 3 * Class.MaxTriangles)));
	}
	const int32 SectionLimit = Class.MaxSections > 0 ? Class.MaxSections : Class.MaxMaterials;
	if (SectionLimit > 0 && Sections > SectionLimit)
	{
		OutIssues.Add(Error(FString::Printf(TEXT("%d material sections, the limit is %d."), Sections, SectionLimit),
			TEXT("Merge materials so the mesh uses fewer material slots.")));
	}
	if (Class.MaxInfluences > 0 && MostInfluences > Class.MaxInfluences)
	{
		OutIssues.Add(Error(FString::Printf(TEXT("a vertex has %d influences, the limit is %d."), MostInfluences, Class.MaxInfluences),
			TEXT("Set the mesh's Max Bone Influences (LOD settings) to 8 or fewer and reimport.")));
	}
}

bool FMantleCosmeticChecks::CheckAvatarBodyShape(TConstArrayView<FMantleGlbJoint> Joints, const FMantleContractSkeleton& Skeleton,
	const FMantleAvatarContract& Contract, FString& OutError)
{
	// By name: FName compares without regard to case, as the cook's BoneKey does.
	TMap<FName, int32> JointOf;
	for (int32 Joint = 0; Joint < Joints.Num(); ++Joint)
	{
		JointOf.Add(Joints[Joint].Name, Joint);
	}

	// Required bones, every missing one named (Go's %q of an ASCII bone name is the name in double quotes).
	TArray<FString> Missing;
	if (const TArray<FString>* Required = Contract.AvatarRequiredBones.Find(Skeleton.Id))
	{
		for (const FString& Name : *Required)
		{
			if (!JointOf.Contains(FName(*Name)))
			{
				Missing.Add(FString::Printf(TEXT("\"%s\""), *Name));
			}
		}
	}
	if (Missing.Num() == 1)
	{
		OutError = FString::Printf(TEXT("the skin has no %s bone; an avatar body on the %s skeleton must include it"), *Missing[0], *Skeleton.Id);
		return false;
	}
	if (Missing.Num() > 1)
	{
		OutError = FString::Printf(TEXT("the skin has no %s bones; an avatar body on the %s skeleton must include them"),
			*FString::Join(Missing, TEXT(", ")), *Skeleton.Id);
		return false;
	}

	// Unit scale, bones in fixture order as the cook's bone table has them (a bone off the fixture last, in joint order).
	TArray<int32> FixtureOrder;
	for (int32 Joint = 0; Joint < Joints.Num(); ++Joint)
	{
		FixtureOrder.Add(Joint);
	}
	FixtureOrder.StableSort([&Joints, &Skeleton](const int32 A, const int32 B)
	{
		const int32 IndexA = Skeleton.Find(Joints[A].Name);
		const int32 IndexB = Skeleton.Find(Joints[B].Name);
		return static_cast<uint32>(IndexA) < static_cast<uint32>(IndexB);
	});
	const double Tolerance = Contract.BoneScaleTolerance;
	for (const int32 Joint : FixtureOrder)
	{
		const FVector Scale = Joints[Joint].Local.GetScale3D();
		if (FMath::Abs(Scale.X - 1.0) > Tolerance || FMath::Abs(Scale.Y - 1.0) > Tolerance || FMath::Abs(Scale.Z - 1.0) > Tolerance)
		{
			OutError = FString::Printf(TEXT("bone \"%s\" has scale %.3g x %.3g x %.3g in the reference pose; every bone's scale must be 1 (within %g): apply the scale before exporting"),
				*Joints[Joint].Name.ToString(), Scale.X, Scale.Y, Scale.Z, Tolerance);
			return false;
		}
	}

	if (!Skeleton.HasReferencePose())
	{
		return true; // A fixture without a referencePose block: the scale rule only.
	}

	// Component space as the cook composes it (parent then local, matrices with scale); a joint whose parent is not
	// an earlier joint is composed once its parent is.
	TArray<FMatrix> Component;
	Component.Init(FMatrix::Identity, Joints.Num());
	TArray<bool> Composed;
	Composed.Init(false, Joints.Num());
	for (bool bProgress = true; bProgress;)
	{
		bProgress = false;
		for (int32 Joint = 0; Joint < Joints.Num(); ++Joint)
		{
			const int32 Parent = Joints[Joint].Parent;
			if (Composed[Joint] || (Joints.IsValidIndex(Parent) && !Composed[Parent]))
			{
				continue;
			}
			const FMatrix Local = Joints[Joint].Local.ToMatrixWithScale();
			Component[Joint] = Joints.IsValidIndex(Parent) ? Local * Component[Parent] : Local;
			Composed[Joint] = true;
			bProgress = true;
		}
	}

	const FString& PoseName = Skeleton.ReferencePoseName;
	for (const FMantleContractReferenceChain& Chain : Skeleton.ReferenceChains)
	{
		const int32* From = JointOf.Find(FName(*Chain.From));
		const int32* To = JointOf.Find(FName(*Chain.To));
		if (!From || !To)
		{
			continue; // Required (the bone rule), so already refused above.
		}
		const FVector Delta = Component[*To].GetOrigin() - Component[*From].GetOrigin();
		const double Length = Delta.Size();
		const FString FromName = Joints[*From].Name.ToString();
		const FString ToName = Joints[*To].Name.ToString();
		if (Length < 0.01)
		{
			OutError = FString::Printf(TEXT("bone \"%s\" sits on bone \"%s\" in the reference pose; upload in the %s"), *ToName, *FromName, *PoseName);
			return false;
		}
		const double Dot = FVector::DotProduct(Delta, Chain.Direction) / Length;
		const double Degrees = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Dot, -1.0, 1.0)));
		if (Degrees > Contract.ChainToleranceDegrees)
		{
			OutError = FString::Printf(TEXT("bone \"%s\" points %.0f degrees away from the %s (%s to %s, at most %g); upload in the %s"),
				*FromName, Degrees, *PoseName, *FromName, *ToName, Contract.ChainToleranceDegrees, *PoseName);
			return false;
		}
	}
	return true;
}

void FMantleCosmeticChecks::CheckAvatarBodyBounds(const FBox3f& Bounds, const FString& SkeletonId, const FMantleAvatarContract& Contract,
	TArray<FMantleCosmeticIssue>& OutIssues)
{
	if (!Bounds.IsValid)
	{
		OutIssues.Add(Error(TEXT("the body has no vertices.")));
		return;
	}
	const FVector3f Lo = Bounds.Min;
	const FVector3f Hi = Bounds.Max;
	if (FMath::Abs(Lo.Z) > Contract.FeetToleranceCm)
	{
		OutIssues.Add(Error(FString::Printf(TEXT("the body's lowest point is at z = %.1f cm; the feet must stand on the origin (within %.0f cm)."),
			Lo.Z, Contract.FeetToleranceCm), TEXT("Move the mesh so its feet are at the origin and reimport.")));
	}
	if (Hi.Z > Contract.HeightMaxCm)
	{
		OutIssues.Add(Error(FString::Printf(TEXT("the avatar is %.1f cm tall, the limit is %.1f cm."), Hi.Z, Contract.HeightMaxCm)));
	}
	if (Hi.X - Lo.X > Contract.WidthMaxCm)
	{
		OutIssues.Add(Error(FString::Printf(TEXT("the avatar is %.1f cm wide (X), the limit is %.1f cm."), Hi.X - Lo.X, Contract.WidthMaxCm)));
	}
	if (Hi.Y - Lo.Y > Contract.DepthMaxCm)
	{
		OutIssues.Add(Error(FString::Printf(TEXT("the avatar is %.1f cm deep (Y), the limit is %.1f cm."), Hi.Y - Lo.Y, Contract.DepthMaxCm)));
	}
	// A MetaHuman body ends at the neck: its face completes the height (CheckAvatarHeight).
	if (SkeletonId != TEXT("metahuman") && Hi.Z < Contract.HeightMinCm)
	{
		OutIssues.Add(Error(FString::Printf(TEXT("the avatar is %.1f cm tall, the minimum is %.1f cm."), Hi.Z, Contract.HeightMinCm)));
	}
}

void FMantleCosmeticChecks::CheckAvatarHeight(float BodyMaxZ, TOptional<float> FaceMaxZ, const FMantleAvatarContract& Contract,
	TArray<FMantleCosmeticIssue>& OutIssues)
{
	const float Height = FaceMaxZ.IsSet() ? FMath::Max(BodyMaxZ, FaceMaxZ.GetValue()) : BodyMaxZ;
	if (Height < Contract.HeightMinCm)
	{
		OutIssues.Add(Error(FString::Printf(TEXT("the avatar stands %.1f cm tall, the minimum is %.1f cm."), Height, Contract.HeightMinCm),
			FaceMaxZ.IsSet() ? FString() : FString(TEXT("A MetaHuman body ends at the neck: add its face mesh."))));
	}
	if (Height > Contract.HeightMaxCm)
	{
		OutIssues.Add(Error(FString::Printf(TEXT("the avatar stands %.1f cm tall, the limit is %.1f cm."), Height, Contract.HeightMaxCm)));
	}
}

double FMantleCosmeticChecks::AvatarStandingHeightCm(const float BodyMaxZ, const TOptional<float> FaceMaxZ)
{
	const double Top = FaceMaxZ.IsSet() ? FMath::Max(static_cast<double>(BodyMaxZ), static_cast<double>(FaceMaxZ.GetValue()))
		: static_cast<double>(BodyMaxZ);
	// cook.RoundMetric: math.Round(cm * 10) / 10, halves away from zero.
	return FMath::RoundHalfFromZero(Top * 10.0) / 10.0;
}

double FMantleCosmeticChecks::AvatarPlayingHeightCm(const double StandingHeightCm, const FMantleAvatarContract& Contract)
{
	return Contract.CharacterScale.PlayingHeightCm(StandingHeightCm, 1.0);
}

FString FMantleCosmeticChecks::DescribeAvatarPlayingHeight(const double StandingHeightCm, const FMantleAvatarContract& Contract)
{
	const FMantleCharacterScaleRule& Rule = Contract.CharacterScale;
	const double Playing = AvatarPlayingHeightCm(StandingHeightCm, Contract);
	if (!Rule.IsClamped(StandingHeightCm, 1.0))
	{
		return FString::Printf(TEXT("Plays at %.1f cm."), Playing);
	}
	const bool bTall = StandingHeightCm > Rule.MaxStandingHeightCm;
	return FString::Printf(TEXT("Plays at %.1f cm: it stands %.1f cm tall, %s Mantle's %.0f cm limit, so its character (hitboxes, camera, collision) is clamped to %.0f cm. The avatar still shows at its own height."),
		Playing, StandingHeightCm, bTall ? TEXT("above") : TEXT("below"),
		bTall ? Rule.MaxStandingHeightCm : Rule.MinStandingHeightCm,
		bTall ? Rule.MaxStandingHeightCm : Rule.MinStandingHeightCm);
}

void FMantleCosmeticChecks::CheckTextureSize(const FString& What, int32 Width, int32 Height, int32 MaxSize, const FMantleAvatarContract& Contract,
	TArray<FMantleCosmeticIssue>& OutIssues)
{
	const int32 Limit = MaxSize > 0 ? FMath::Min(MaxSize, Contract.MaxTextureSize) : Contract.MaxTextureSize;
	if (Width <= 0 || Height <= 0)
	{
		OutIssues.Add(Error(FString::Printf(TEXT("%s has no pixels."), *What)));
		return;
	}
	if (Width > Limit || Height > Limit)
	{
		// Not an error: the plugin scales it down on export (FMantleCosmeticExport), as the
		// texture's own Maximum Texture Size would.
		OutIssues.Add(Info(FString::Printf(TEXT("%s is %dx%d; it is uploaded at most %dx%d."), *What, Width, Height, Limit, Limit)));
	}
}

void FMantleCosmeticChecks::CheckAccessoryBounds(const FString& Slot, const FBox3f& Bounds, const FMantleAvatarContract& Contract,
	TArray<FMantleCosmeticIssue>& OutIssues)
{
	const FMantleContractAccessoryBounds* Limits = Contract.AccessoryBounds.Find(Slot);
	if (!Limits)
	{
		OutIssues.Add(Error(FString::Printf(TEXT("\"%s\" is not an accessory slot."), *Slot)));
		return;
	}
	if (!Bounds.IsValid)
	{
		OutIssues.Add(Error(TEXT("the accessory has no vertices.")));
		return;
	}
	double Longest = 0.0;
	double Reach = 0.0;
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		Longest = FMath::Max(Longest, static_cast<double>(Bounds.Max[Axis] - Bounds.Min[Axis]));
		const double Far = FMath::Max(FMath::Abs(static_cast<double>(Bounds.Min[Axis])), FMath::Abs(static_cast<double>(Bounds.Max[Axis])));
		Reach += Far * Far;
	}
	Reach = FMath::Sqrt(Reach);
	if (Longest > Limits->MaxDimensionCm)
	{
		OutIssues.Add(Error(FString::Printf(TEXT("the accessory is %.1f cm across, %s allows %.0f cm."), Longest, *Slot, Limits->MaxDimensionCm),
			TEXT("Scale it down in the mesh (its pivot is the socket).")));
	}
	if (Reach > Limits->MaxReachCm)
	{
		OutIssues.Add(Error(FString::Printf(TEXT("the accessory reaches %.1f cm from its pivot, %s allows %.0f cm."), Reach, *Slot, Limits->MaxReachCm),
			TEXT("Its pivot is where it attaches: move the mesh so the pivot sits on it, near the socket.")));
	}
}

void FMantleCosmeticChecks::PoseDifference(const FTransform& A, const FTransform& B, double& OutDistanceCm, double& OutDegrees, double& OutScale)
{
	OutDistanceCm = FVector::Distance(A.GetTranslation(), B.GetTranslation());
	OutDegrees = 0.0;
	OutScale = 0.0;
	const FMatrix MA = A.ToMatrixWithScale();
	const FMatrix MB = B.ToMatrixWithScale();
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		const FVector AxisA = MA.GetScaledAxis(static_cast<EAxis::Type>(EAxis::X + Axis));
		const FVector AxisB = MB.GetScaledAxis(static_cast<EAxis::Type>(EAxis::X + Axis));
		const double LengthA = AxisA.Size();
		const double LengthB = AxisB.Size();
		if (LengthA == 0.0 || LengthB == 0.0)
		{
			OutDegrees = 180.0;
			OutScale = TNumericLimits<double>::Max();
			return;
		}
		const double Dot = FMath::Clamp(FVector::DotProduct(AxisA, AxisB) / (LengthA * LengthB), -1.0, 1.0);
		OutDegrees = FMath::Max(OutDegrees, FMath::RadiansToDegrees(FMath::Acos(Dot)));
		OutScale = FMath::Max(OutScale, FMath::Abs(LengthA / LengthB - 1.0));
	}
}

bool FMantleCosmeticChecks::WithinBindTolerance(const FTransform& A, const FTransform& B, const FMantleAvatarContract& Contract,
	double* OutDistanceCm, double* OutDegrees)
{
	double Distance = 0.0, Degrees = 0.0, Scale = 0.0;
	PoseDifference(A, B, Distance, Degrees, Scale);
	if (OutDistanceCm)
	{
		*OutDistanceCm = Distance;
	}
	if (OutDegrees)
	{
		*OutDegrees = Degrees;
	}
	return Distance <= Contract.BindToleranceCm && Degrees <= Contract.BindToleranceDegrees && Scale <= 0.01;
}

FString FMantleUVTiles::DescribeTiles() const
{
	TArray<FString> Parts;
	for (const FIntPoint& Tile : Tiles)
	{
		const int32 Udim = FMantleCosmeticChecks::UdimNumber(Tile);
		Parts.Add(Udim != INDEX_NONE ? FString::FromInt(Udim) : FString::Printf(TEXT("(%d, %d)"), Tile.X, Tile.Y));
	}
	return FString::Join(Parts, TEXT(", "));
}

FMantleUVTiles FMantleCosmeticChecks::MeasureUVTiles(TConstArrayView<FVector2f> UVs, TConstArrayView<uint32> Indices)
{
	FMantleUVTiles Out;
	for (const FVector2f& UV : UVs)
	{
		Out.Range += UV;
	}
	// A closed unit-square atlas also owns its border. A UV point/line at U or V=1
	// has centroid 1 exactly; flooring it must not invent an adjacent UDIM tile.
	const bool bInUnitTile = Out.Range.bIsValid && Out.Range.Min.X >= 0.f && Out.Range.Min.Y >= 0.f
		&& Out.Range.Max.X <= 1.f && Out.Range.Max.Y <= 1.f;
	TSet<FIntPoint> Seen;
	for (int32 First = 0; First + 2 < Indices.Num(); First += 3)
	{
		const uint32 Corners[3] = { Indices[First], Indices[First + 1], Indices[First + 2] };
		if (Corners[0] >= static_cast<uint32>(UVs.Num()) || Corners[1] >= static_cast<uint32>(UVs.Num()) || Corners[2] >= static_cast<uint32>(UVs.Num()))
		{
			continue;
		}
		const FVector2f Centroid = (UVs[Corners[0]] + UVs[Corners[1]] + UVs[Corners[2]]) / 3.0f;
		const FIntPoint Tile = bInUnitTile ? FIntPoint::ZeroValue : FIntPoint(FMath::FloorToInt32(Centroid.X), FMath::FloorToInt32(Centroid.Y));
		Seen.Add(Tile);
		++Out.Triangles;
		bool bCrosses = false;
		for (const uint32 Corner : Corners)
		{
			const FVector2f& UV = UVs[Corner];
			bCrosses |= UV.X < Tile.X - UVTileEpsilon || UV.X > Tile.X + 1 + UVTileEpsilon
				|| UV.Y < Tile.Y - UVTileEpsilon || UV.Y > Tile.Y + 1 + UVTileEpsilon;
		}
		Out.CrossingTriangles += bCrosses ? 1 : 0;
	}
	Out.Tiles = Seen.Array();
	Out.Tiles.Sort([](const FIntPoint& A, const FIntPoint& B) { return A.Y != B.Y ? A.Y < B.Y : A.X < B.X; });
	return Out;
}

int32 FMantleCosmeticChecks::UdimNumber(const FIntPoint& Tile)
{
	return Tile.X >= 0 && Tile.X < 10 && Tile.Y >= 0 ? 1001 + Tile.X + 10 * Tile.Y : INDEX_NONE;
}

void FMantleCosmeticChecks::CheckUVTiles(const FString& What, const FMantleUVTiles& Tiles, TArray<FMantleCosmeticIssue>& OutIssues)
{
	if (!Tiles.IsUdim())
	{
		return;
	}
	const bool bAllUdim = !Tiles.Tiles.ContainsByPredicate([](const FIntPoint& Tile) { return UdimNumber(Tile) == INDEX_NONE; });
	OutIssues.Add(Error(FString::Printf(TEXT("%s spans %d UV tiles (%s%s); Mantle maps one 0..1 texture per section."),
		*What, Tiles.Tiles.Num(), bAllUdim ? TEXT("UDIM ") : TEXT("tiles "), *Tiles.DescribeTiles()),
		TEXT("Bake its tiles into one texture with its UVs laid out in 0..1, then reimport the mesh and the texture.")));
}

void FMantleCosmeticChecks::CheckSectionNames(TConstArrayView<FString> Names, TArray<FMantleCosmeticIssue>& OutIssues)
{
	TSet<FString> Seen;
	for (int32 Index = 0; Index < Names.Num(); ++Index)
	{
		if (Names[Index].IsEmpty())
		{
			OutIssues.Add(Error(FString::Printf(TEXT("material slot %d has no name; sections are identified by material name."), Index),
				TEXT("Name every material slot in the mesh editor.")));
			continue;
		}
		bool bDuplicate = false;
		Seen.Add(Names[Index], &bDuplicate);
		if (bDuplicate)
		{
			OutIssues.Add(Error(FString::Printf(TEXT("two materials are named \"%s\"; sections are identified by material name."), *Names[Index]),
				TEXT("Rename one of the material slots in the mesh editor.")));
		}
	}
}

void FMantleCosmeticChecks::CheckTransform(const FString& What, const FVector3f* Location, const FVector3f* Rotation, const FVector3f* Scale,
	TArray<FMantleCosmeticIssue>& OutIssues)
{
	auto Within = [](const FVector3f* Value, float Low, float High)
	{
		if (!Value)
		{
			return true;
		}
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const float Component = (*Value)[Axis];
			if (!FMath::IsFinite(Component) || Component < Low || Component > High)
			{
				return false;
			}
		}
		return true;
	};
	auto Describe = [](const FVector3f& Value)
	{
		return FString::Printf(TEXT("(%g, %g, %g)"), Value.X, Value.Y, Value.Z);
	};
	const TCHAR* Hint = TEXT("Mantle refuses it rather than clamping it.");
	if (!Within(Location, -MaxTransformOffsetCm, MaxTransformOffsetCm))
	{
		OutIssues.Add(Error(FString::Printf(TEXT("%s location %s: offsets are at most %g cm on each axis."), *What, *Describe(*Location), MaxTransformOffsetCm), Hint));
	}
	if (!Within(Rotation, -MaxTransformDegrees, MaxTransformDegrees))
	{
		OutIssues.Add(Error(FString::Printf(TEXT("%s rotation %s: rotations are between -%g and %g degrees."), *What, *Describe(*Rotation),
			MaxTransformDegrees, MaxTransformDegrees), Hint));
	}
	if (!Within(Scale, MinTransformScale, MaxTransformScale))
	{
		OutIssues.Add(Error(FString::Printf(TEXT("%s scale %s: scale is between %g and %g."), *What, *Describe(*Scale), MinTransformScale, MaxTransformScale), Hint));
	}
}

void FMantleCosmeticChecks::CheckNameAndDescription(const FString& Name, const FString& Description, TArray<FMantleCosmeticIssue>& OutIssues)
{
	const FString Trimmed = Name.TrimStartAndEnd();
	if (Trimmed.IsEmpty())
	{
		OutIssues.Add(Error(TEXT("the item needs a name."), TEXT("Players see it in the catalog.")));
	}
	else if (Trimmed.Len() > 64)
	{
		OutIssues.Add(Error(FString::Printf(TEXT("the name is %d characters, the limit is 64."), Trimmed.Len())));
	}
	if (Description.TrimStartAndEnd().Len() > 1000)
	{
		OutIssues.Add(Error(FString::Printf(TEXT("the description is %d characters, the limit is 1000."), Description.TrimStartAndEnd().Len())));
	}
}

void FMantleCosmeticChecks::CheckUploadSize(const FString& What, int64 Bytes, const FMantleAvatarContract& Contract, TArray<FMantleCosmeticIssue>& OutIssues)
{
	if (Bytes > Contract.MaxUploadBytes)
	{
		OutIssues.Add(Error(FString::Printf(TEXT("%s is %.1f MiB, uploads can be at most %.0f MiB."), *What,
			Bytes / (1024.0 * 1024.0), Contract.MaxUploadBytes / (1024.0 * 1024.0))));
	}
}
