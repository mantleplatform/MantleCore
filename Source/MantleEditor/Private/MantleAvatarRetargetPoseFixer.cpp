// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAvatarRetargetPoseFixer.h"
#include "MantleAvatarContract.h"
#include "MantleAvatarReposeFixer.h"
#include "MantleCosmeticExport.h"
#include "MantleCosmeticPreview.h"
#include "MantleRuntime.h"

#include "AssetToolsModule.h"
#include "Engine/AssetUserData.h"
#include "Engine/SkeletalMesh.h"
#include "IAssetTools.h"
#include "MeshDescription.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "ReferenceSkeleton.h"
#include "UObject/Class.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleAvatarRetargetPosePrivate
{
	const TCHAR* const RetargetPoseAvatarSlot = TEXT("avatar");
	const TCHAR* const RetargetPoseMetaHuman = TEXT("metahuman");

	int32 RetargetPoseFindBone(TConstArrayView<FName> Names, const FName Bone)
	{
		for (int32 Index = 0; Index < Names.Num(); ++Index)
		{
			if (Names[Index] == Bone)
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	/** The angle between two directions, degrees; 0 when either has no length. */
	double RetargetPoseDegrees(const FVector& A, const FVector& B)
	{
		const FVector UnitA = A.GetSafeNormal();
		const FVector UnitB = B.GetSafeNormal();
		if (UnitA.IsNearlyZero() || UnitB.IsNearlyZero())
		{
			return 0.0;
		}
		return FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(UnitA, UnitB), -1.0, 1.0)));
	}

	/** RigLogic DNA on the mesh (UDNAAsset or its user data, by class name: MantleEditor does not link RigLogic). */
	bool RetargetPoseHasDNA(const USkeletalMesh& Mesh)
	{
		const TArray<UAssetUserData*>* UserData = Mesh.GetAssetUserDataArray();
		if (!UserData)
		{
			return false;
		}
		for (const UAssetUserData* Data : *UserData)
		{
			if (Data && Data->GetClass()->GetName().Contains(TEXT("DNA")))
			{
				return true;
			}
		}
		return false;
	}

	const FProperty* RetargetPoseProperty(const UStruct* Owner, const TCHAR* Name)
	{
		return Owner ? FindFProperty<FProperty>(Owner, FName(Name)) : nullptr;
	}

	/**
	 * The current retarget pose's local rotation offsets on one side of a UIKRetargeter: TargetRetargetPoses (or
	 * SourceRetargetPoses) at CurrentTargetRetargetPose (CurrentSourceRetargetPose), its BoneRotationOffsets. A current
	 * pose the map lacks has no offsets (the reference pose itself, as the processor uses it).
	 */
	bool RetargetPoseReadOffsets(const UObject& Retargeter, const bool bTarget, TMap<FName, FQuat>& Out, FString& OutError)
	{
		Out.Reset();
		const UClass* Class = Retargeter.GetClass();
		const TCHAR* PosesName = bTarget ? TEXT("TargetRetargetPoses") : TEXT("SourceRetargetPoses");
		const TCHAR* CurrentName = bTarget ? TEXT("CurrentTargetRetargetPose") : TEXT("CurrentSourceRetargetPose");
		const FMapProperty* Poses = CastField<FMapProperty>(RetargetPoseProperty(Class, PosesName));
		const FNameProperty* Current = CastField<FNameProperty>(RetargetPoseProperty(Class, CurrentName));
		const FNameProperty* PoseKey = Poses ? CastField<FNameProperty>(Poses->KeyProp) : nullptr;
		const FStructProperty* PoseValue = Poses ? CastField<FStructProperty>(Poses->ValueProp) : nullptr;
		const FMapProperty* Offsets = PoseValue ? CastField<FMapProperty>(RetargetPoseProperty(PoseValue->Struct, TEXT("BoneRotationOffsets"))) : nullptr;
		const FNameProperty* OffsetKey = Offsets ? CastField<FNameProperty>(Offsets->KeyProp) : nullptr;
		const FStructProperty* OffsetValue = Offsets ? CastField<FStructProperty>(Offsets->ValueProp) : nullptr;
		if (!Current || !PoseKey || !PoseValue || !OffsetKey || !OffsetValue || OffsetValue->Struct != TBaseStructure<FQuat>::Get())
		{
			OutError = FString::Printf(TEXT("%s's %s cannot be read (the IK Rig plugin's retargeter changed)"), *Retargeter.GetName(), PosesName);
			return false;
		}
		const FName Wanted = Current->GetPropertyValue_InContainer(&Retargeter);
		FScriptMapHelper PoseMap(Poses, Poses->ContainerPtrToValuePtr<void>(&Retargeter));
		for (int32 Index = 0; Index < PoseMap.GetMaxIndex(); ++Index)
		{
			if (!PoseMap.IsValidIndex(Index) || FNameProperty::GetPropertyValue(PoseMap.GetKeyPtr(Index)) != Wanted)
			{
				continue;
			}
			FScriptMapHelper OffsetMap(Offsets, Offsets->ContainerPtrToValuePtr<void>(PoseMap.GetValuePtr(Index)));
			for (int32 Entry = 0; Entry < OffsetMap.GetMaxIndex(); ++Entry)
			{
				if (!OffsetMap.IsValidIndex(Entry))
				{
					continue;
				}
				const FName Bone = FNameProperty::GetPropertyValue(OffsetMap.GetKeyPtr(Entry));
				const FQuat Offset = *reinterpret_cast<const FQuat*>(OffsetMap.GetValuePtr(Entry));
				Out.Add(Bone, Offset.GetNormalized());
			}
			break;
		}
		return true;
	}

	/**
	 * One of the retargeter's IK rigs (RigProperty: SourceIKRigAsset or TargetIKRigAsset), its stored skeleton: bone
	 * names, parents and the reference pose of the mesh it was made on, in component space.
	 */
	bool RetargetPoseReadRigSkeleton(const UObject& Retargeter, const TCHAR* RigPropertyName, TArray<FName>& OutNames, TArray<int32>& OutParents,
		TArray<FTransform>& OutComponent, FString& OutRigName, FString& OutError)
	{
		OutNames.Reset();
		OutParents.Reset();
		OutComponent.Reset();
		const FObjectPropertyBase* RigProperty = CastField<FObjectPropertyBase>(RetargetPoseProperty(Retargeter.GetClass(), RigPropertyName));
		const UObject* Rig = RigProperty ? RigProperty->GetObjectPropertyValue_InContainer(&Retargeter) : nullptr;
		if (!Rig)
		{
			OutError = FString::Printf(TEXT("%s has no %s to read a skeleton from"), *Retargeter.GetName(), RigPropertyName);
			return false;
		}
		OutRigName = Rig->GetName();
		const FStructProperty* SkeletonProperty = CastField<FStructProperty>(RetargetPoseProperty(Rig->GetClass(), TEXT("Skeleton")));
		const UStruct* SkeletonStruct = SkeletonProperty ? SkeletonProperty->Struct : nullptr;
		const void* Skeleton = SkeletonProperty ? SkeletonProperty->ContainerPtrToValuePtr<void>(Rig) : nullptr;
		const FArrayProperty* NamesProperty = CastField<FArrayProperty>(RetargetPoseProperty(SkeletonStruct, TEXT("BoneNames")));
		const FArrayProperty* ParentsProperty = CastField<FArrayProperty>(RetargetPoseProperty(SkeletonStruct, TEXT("ParentIndices")));
		const FArrayProperty* PoseProperty = CastField<FArrayProperty>(RetargetPoseProperty(SkeletonStruct, TEXT("RefPoseGlobal")));
		const FStructProperty* PoseInner = PoseProperty ? CastField<FStructProperty>(PoseProperty->Inner) : nullptr;
		if (!Skeleton || !NamesProperty || !CastField<FNameProperty>(NamesProperty->Inner) || !ParentsProperty || !CastField<FIntProperty>(ParentsProperty->Inner)
			|| !PoseInner || PoseInner->Struct != TBaseStructure<FTransform>::Get())
		{
			OutError = FString::Printf(TEXT("%s's stored skeleton cannot be read (the IK Rig plugin's rig changed)"), *Rig->GetName());
			return false;
		}
		FScriptArrayHelper NameArray(NamesProperty, NamesProperty->ContainerPtrToValuePtr<void>(Skeleton));
		FScriptArrayHelper ParentArray(ParentsProperty, ParentsProperty->ContainerPtrToValuePtr<void>(Skeleton));
		FScriptArrayHelper PoseArray(PoseProperty, PoseProperty->ContainerPtrToValuePtr<void>(Skeleton));
		const int32 NumBones = NameArray.Num();
		if (NumBones == 0 || ParentArray.Num() != NumBones || PoseArray.Num() != NumBones)
		{
			OutError = FString::Printf(TEXT("%s stores no reference pose (%d bones, %d parents, %d transforms)"), *Rig->GetName(), NumBones, ParentArray.Num(),
				PoseArray.Num());
			return false;
		}
		for (int32 Bone = 0; Bone < NumBones; ++Bone)
		{
			OutNames.Add(*reinterpret_cast<const FName*>(NameArray.GetRawPtr(Bone)));
			OutParents.Add(*reinterpret_cast<const int32*>(ParentArray.GetRawPtr(Bone)));
			OutComponent.Add(*reinterpret_cast<const FTransform*>(PoseArray.GetRawPtr(Bone)));
		}
		return true;
	}
}

double FMantleRetargetPosePlan::MaxFingerReframeDegrees() const
{
	double Max = 0.0;
	for (const FMantleRetargetPoseTurn& Turn : FingerReframes)
	{
		Max = FMath::Max(Max, Turn.Degrees);
	}
	return Max;
}

TConstArrayView<FMantleRetargetPoseStep> FMantleAvatarRetargetPose::Steps()
{
	// Parents first on each side; the hand's segment runs through its middle metacarpal to the finger's first joint.
	static const FMantleRetargetPoseStep Table[] = {
		{ FName(TEXT("upperarm_l")), FName(TEXT("lowerarm_l")) },
		{ FName(TEXT("lowerarm_l")), FName(TEXT("hand_l")) },
		{ FName(TEXT("hand_l")), FName(TEXT("middle_01_l")) },
		{ FName(TEXT("upperarm_r")), FName(TEXT("lowerarm_r")) },
		{ FName(TEXT("lowerarm_r")), FName(TEXT("hand_r")) },
		{ FName(TEXT("hand_r")), FName(TEXT("middle_01_r")) },
		{ FName(TEXT("thigh_l")), FName(TEXT("calf_l")) },
		{ FName(TEXT("calf_l")), FName(TEXT("foot_l")) },
		{ FName(TEXT("thigh_r")), FName(TEXT("calf_r")) },
		{ FName(TEXT("calf_r")), FName(TEXT("foot_r")) },
	};
	return MakeArrayView(Table);
}

TConstArrayView<FMantleRetargetPoseStep> FMantleAvatarRetargetPose::FrameSteps()
{
	static const FMantleRetargetPoseStep Table[] = {
		{ FName(TEXT("upperarm_l")), FName(TEXT("lowerarm_l")) },
		{ FName(TEXT("lowerarm_l")), FName(TEXT("hand_l")) },
		{ FName(TEXT("hand_l")), FName(TEXT("middle_01_l")) },
		{ FName(TEXT("upperarm_r")), FName(TEXT("lowerarm_r")) },
		{ FName(TEXT("lowerarm_r")), FName(TEXT("hand_r")) },
		{ FName(TEXT("hand_r")), FName(TEXT("middle_01_r")) },
		{ FName(TEXT("thigh_l")), FName(TEXT("calf_l")) },
		{ FName(TEXT("calf_l")), FName(TEXT("foot_l")) },
		{ FName(TEXT("foot_l")), FName(TEXT("ball_l")) },
		{ FName(TEXT("thigh_r")), FName(TEXT("calf_r")) },
		{ FName(TEXT("calf_r")), FName(TEXT("foot_r")) },
		{ FName(TEXT("foot_r")), FName(TEXT("ball_r")) },
	};
	return MakeArrayView(Table);
}

TConstArrayView<FMantleRetargetPoseStep> FMantleAvatarRetargetPose::OptionalFrameSteps()
{
	// In order: a bone takes the first of its entries both skeletons have (spine_03 to spine_04, else to neck_01 on the UE4
	// Mannequin, which has no spine_04).
	static const FMantleRetargetPoseStep Table[] = {
		{ FName(TEXT("clavicle_l")), FName(TEXT("upperarm_l")) },
		{ FName(TEXT("clavicle_r")), FName(TEXT("upperarm_r")) },
		{ FName(TEXT("spine_01")), FName(TEXT("spine_02")) },
		{ FName(TEXT("spine_02")), FName(TEXT("spine_03")) },
		{ FName(TEXT("spine_03")), FName(TEXT("spine_04")) },
		{ FName(TEXT("spine_03")), FName(TEXT("neck_01")) },
		{ FName(TEXT("spine_04")), FName(TEXT("spine_05")) },
		{ FName(TEXT("spine_05")), FName(TEXT("neck_01")) },
	};
	return MakeArrayView(Table);
}

TArray<TArray<FName>> FMantleAvatarRetargetPose::FingerChains()
{
	TArray<TArray<FName>> Out;
	for (const TCHAR* Side : { TEXT("l"), TEXT("r") })
	{
		for (const TCHAR* Finger : { TEXT("thumb"), TEXT("index"), TEXT("middle"), TEXT("ring"), TEXT("pinky") })
		{
			TArray<FName>& Chain = Out.AddDefaulted_GetRef();
			Chain.Add(FName(*FString::Printf(TEXT("%s_metacarpal_%s"), Finger, Side)));
			for (int32 Segment = 1; Segment <= 3; ++Segment)
			{
				Chain.Add(FName(*FString::Printf(TEXT("%s_%02d_%s"), Finger, Segment, Side)));
			}
		}
	}
	return Out;
}

TArray<FName> FMantleAvatarRetargetPose::RequiredBones()
{
	TArray<FName> Out;
	for (const FMantleRetargetPoseStep& Step : FrameSteps())
	{
		Out.AddUnique(Step.Bone);
		Out.AddUnique(Step.Child);
	}
	return Out;
}

void FMantleAvatarRetargetPose::RetargetPoseComponent(TConstArrayView<FName> Names, TConstArrayView<int32> Parents, TConstArrayView<FTransform> Local,
	const TMap<FName, FQuat>& Offsets, TArray<FTransform>& OutComponent)
{
	TArray<FTransform> Retarget(Local.GetData(), Local.Num());
	for (int32 Bone = 0; Bone < Retarget.Num(); ++Bone)
	{
		const FQuat* Offset = Names.IsValidIndex(Bone) ? Offsets.Find(Names[Bone]) : nullptr;
		if (Offset)
		{
			Retarget[Bone].SetRotation((Retarget[Bone].GetRotation() * *Offset).GetNormalized());
		}
	}
	FMantleAvatarRepose::ComposeComponent(Parents, Retarget, OutComponent);
}

bool FMantleAvatarRetargetPose::Plan(TConstArrayView<FName> Names, TConstArrayView<int32> Parents, TConstArrayView<FTransform> Local,
	const FMantleRetargetPoseDriver& Driver, FMantleRetargetPosePlan& OutPlan, FString& OutError)
{
	using namespace MantleAvatarRetargetPosePrivate;
	OutPlan = FMantleRetargetPosePlan();
	if (Names.Num() == 0 || Names.Num() != Parents.Num() || Names.Num() != Local.Num())
	{
		OutError = TEXT("the reference pose has no bones, or its names, parents and transforms differ in number");
		return false;
	}
	for (int32 Bone = 0; Bone < Parents.Num(); ++Bone)
	{
		if (Parents[Bone] < INDEX_NONE || Parents[Bone] >= Bone)
		{
			OutError = FString::Printf(TEXT("bone \"%s\" comes before its parent in the reference skeleton"), *Names[Bone].ToString());
			return false;
		}
	}
	const bool bFrames = Driver.TargetReference.Num() > 0;
	TSet<FName> FingerBones;
	for (const TArray<FName>& FingerChain : FingerChains())
	{
		FingerBones.Append(FingerChain);
	}
	for (const FName& Bone : RequiredBones())
	{
		if (RetargetPoseFindBone(Names, Bone) == INDEX_NONE)
		{
			OutError = FString::Printf(TEXT("the body has no bone \"%s\""), *Bone.ToString());
			return false;
		}
		if (!Driver.DriverJoints.Contains(Bone))
		{
			OutError = FString::Printf(TEXT("the driver has no bone \"%s\""), *Bone.ToString());
			return false;
		}
		// #774: a finger bone (the hand's segment runs to middle_01) is optional on the target skeleton: without it the hand keeps
		// its axes, and the fingers are skipped, quietly.
		if (bFrames && !Driver.TargetReference.Contains(Bone) && !FingerBones.Contains(Bone))
		{
			OutError = FString::Printf(TEXT("the retargeter's target skeleton has no bone \"%s\""), *Bone.ToString());
			return false;
		}
	}
	OutPlan.LocalPose = TArray<FTransform>(Local.GetData(), Local.Num());

	// The segments reported: every step's, then the feet's (anatomy, never turned).
	TArray<FMantleRetargetPoseStep> Reported(Steps().GetData(), Steps().Num());
	Reported.Add({ FName(TEXT("foot_l")), FName(TEXT("ball_l")) });
	Reported.Add({ FName(TEXT("foot_r")), FName(TEXT("ball_r")) });
	auto DriverSegment = [&Driver](const FMantleRetargetPoseStep& Step)
	{
		return Driver.DriverJoints.FindChecked(Step.Child) - Driver.DriverJoints.FindChecked(Step.Bone);
	};
	auto BodySegment = [Names](const TArray<FTransform>& Pose, const FMantleRetargetPoseStep& Step)
	{
		return Pose[RetargetPoseFindBone(Names, Step.Child)].GetLocation() - Pose[RetargetPoseFindBone(Names, Step.Bone)].GetLocation();
	};

	// Before: what the client's retargeter does with the body as it is.
	TArray<FTransform> Retarget;
	RetargetPoseComponent(Names, Parents, OutPlan.LocalPose, Driver.TargetOffsets, Retarget);
	for (const FMantleRetargetPoseStep& Step : Reported)
	{
		const FVector Mine = BodySegment(Retarget, Step);
		const FVector Theirs = DriverSegment(Step);
		if (Mine.Size() < 0.01 || Theirs.Size() < 0.01)
		{
			OutError = FString::Printf(TEXT("bone \"%s\" sits on bone \"%s\" (on the %s), so the segment has no direction"), *Step.Child.ToString(), *Step.Bone.ToString(),
				Mine.Size() < 0.01 ? TEXT("body") : TEXT("driver"));
			return false;
		}
		FMantleRetargetPoseSegment& Segment = OutPlan.Segments.AddDefaulted_GetRef();
		Segment.From = Step.Bone;
		Segment.To = Step.Child;
		Segment.BeforeDegrees = RetargetPoseDegrees(Mine, Theirs);
	}

	// Step 1, the axes: each limb bone takes the target skeleton's, swung onto its own segment; so do the clavicles and the
	// spine where both skeletons have them (the UE4 retargeter's offsets act on them). Every joint keeps its component
	// transform (the bones below a changed one get new bone-space transforms), so no vertex has to move.
	TArray<FTransform> Component;
	FMantleAvatarRepose::ComposeComponent(Parents, OutPlan.LocalPose, Component);
	if (bFrames)
	{
		TArray<FTransform> Framed = Component;
		TArray<bool> Changed;
		Changed.Init(false, Names.Num());
		TArray<FMantleRetargetPoseStep> Framing(FrameSteps().GetData(), FrameSteps().Num());
		TSet<FName> Framable;
		for (const FMantleRetargetPoseStep& Optional : OptionalFrameSteps())
		{
			const bool bPresent = RetargetPoseFindBone(Names, Optional.Bone) != INDEX_NONE && RetargetPoseFindBone(Names, Optional.Child) != INDEX_NONE
				&& Driver.TargetReference.Contains(Optional.Bone) && Driver.TargetReference.Contains(Optional.Child);
			if (bPresent && !Framable.Contains(Optional.Bone))
			{
				Framable.Add(Optional.Bone);
				Framing.Add(Optional);
			}
		}
		for (const FMantleRetargetPoseStep& Step : Framing)
		{
			if (!Driver.TargetReference.Contains(Step.Bone) || !Driver.TargetReference.Contains(Step.Child))
			{
				continue; // Only a finger bone can be missing here (checked above): the hand keeps its axes.
			}
			const int32 Bone = RetargetPoseFindBone(Names, Step.Bone);
			const FTransform& ReferenceBone = Driver.TargetReference.FindChecked(Step.Bone);
			const FVector Theirs = Driver.TargetReference.FindChecked(Step.Child).GetLocation() - ReferenceBone.GetLocation();
			const FVector Mine = BodySegment(Component, Step);
			if (Theirs.Size() < 0.01 || Mine.Size() < 0.01)
			{
				OutError = FString::Printf(TEXT("bone \"%s\" sits on bone \"%s\" (on the %s), so its axes cannot be set"), *Step.Child.ToString(), *Step.Bone.ToString(),
					Mine.Size() < 0.01 ? TEXT("body") : TEXT("retargeter's target skeleton"));
				return false;
			}
			const FQuat Axes = (FQuat::FindBetweenVectors(Theirs, Mine) * ReferenceBone.GetRotation()).GetNormalized();
			const double Degrees = FMath::RadiansToDegrees(Component[Bone].GetRotation().AngularDistance(Axes));
			if (Degrees < MinTurnDegrees)
			{
				continue;
			}
			Framed[Bone].SetRotation(Axes);
			Changed[Bone] = true;
			OutPlan.Reframes.Add({ Step.Bone, Degrees });
		}

		// #774, the fingers: after the hand, each finger bone takes the target skeleton's frame as it sits in the bone above it
		// (that bone as framed just now; UE's quaternion product applies its right side first, so the template's rotation relative to its
		// parent is TP^-1 * T and the bone's start frame is Framed parent * TP^-1 * T), swung onto the segment to the chain's next bone. The tip has no segment and keeps
		// the frame its parent hands it. Nothing here moves a joint; a chain either skeleton lacks, and a segment with no
		// length, are left as they are (fingers are optional, the limbs' check is not theirs).
		for (const TArray<FName>& FingerChain : FingerChains())
		{
			TArray<FName> Present;
			for (const FName& FingerBone : FingerChain)
			{
				if (RetargetPoseFindBone(Names, FingerBone) != INDEX_NONE && Driver.TargetReference.Contains(FingerBone))
				{
					Present.Add(FingerBone);
				}
			}
			for (int32 Link = 0; Link < Present.Num(); ++Link)
			{
				const int32 Bone = RetargetPoseFindBone(Names, Present[Link]);
				const FQuat ReferenceRotation = Driver.TargetReference.FindChecked(Present[Link]).GetRotation();
				const int32 Parent = Parents[Bone];
				const FTransform* ReferenceParent = Parent != INDEX_NONE ? Driver.TargetReference.Find(Names[Parent]) : nullptr;
				const FQuat Start = ReferenceParent
					? (Framed[Parent].GetRotation() * ReferenceParent->GetRotation().Inverse() * ReferenceRotation).GetNormalized()
					: ReferenceRotation;
				FQuat Axes = Start;
				if (Link + 1 < Present.Num())
				{
					const int32 Next = RetargetPoseFindBone(Names, Present[Link + 1]);
					const FVector Theirs = Driver.TargetReference.FindChecked(Present[Link + 1]).GetLocation() - Driver.TargetReference.FindChecked(Present[Link]).GetLocation();
					const FVector Mine = Component[Next].GetLocation() - Component[Bone].GetLocation();
					if (Theirs.Size() < 0.01 || Mine.Size() < 0.01)
					{
						continue;
					}
					// The template segment as the starting frame carries it, onto the body's.
					const FVector Carried = Start.RotateVector(ReferenceRotation.UnrotateVector(Theirs));
					Axes = (FQuat::FindBetweenVectors(Carried, Mine) * Start).GetNormalized();
				}
				const double Degrees = FMath::RadiansToDegrees(Component[Bone].GetRotation().AngularDistance(Axes));
				if (Degrees < MinTurnDegrees)
				{
					continue;
				}
				Framed[Bone].SetRotation(Axes);
				Changed[Bone] = true;
				OutPlan.FingerReframes.Add({ Present[Link], Degrees });
			}
		}
		for (int32 Bone = 0; Bone < Names.Num(); ++Bone)
		{
			const int32 Parent = Parents[Bone];
			if (Changed[Bone] || (Parent != INDEX_NONE && Changed[Parent]))
			{
				FTransform BoneSpace = Parent != INDEX_NONE ? Framed[Bone].GetRelativeTransform(Framed[Parent]) : Framed[Bone];
				BoneSpace.NormalizeRotation();
				OutPlan.LocalPose[Bone] = BoneSpace;
			}
		}
		FMantleAvatarRepose::ComposeComponent(Parents, OutPlan.LocalPose, Component);
	}
	OutPlan.FramedPose = OutPlan.LocalPose;
	// The pose after step 1: the feet get their component rotation back from it.
	const TArray<FTransform> FramedComponent = Component;

	// Step 2, the pose.
	for (const FMantleRetargetPoseStep& Step : Steps())
	{
		RetargetPoseComponent(Names, Parents, OutPlan.LocalPose, Driver.TargetOffsets, Retarget);
		const int32 Bone = RetargetPoseFindBone(Names, Step.Bone);
		const FVector Mine = BodySegment(Retarget, Step);
		const FVector Theirs = DriverSegment(Step);
		const double Turn = RetargetPoseDegrees(Mine, Theirs);
		if (Turn > MaxTurnDegrees)
		{
			OutError = FString::Printf(TEXT("lining %s to %s up with the driver would turn bone \"%s\" by %.0f degrees (at most %.0f): it is probably not a humanoid limb"),
				*Step.Bone.ToString(), *Step.Child.ToString(), *Step.Bone.ToString(), Turn, MaxTurnDegrees);
			return false;
		}
		if (Turn < MinTurnDegrees)
		{
			continue;
		}
		// The smallest rotation (a swing: the bone keeps its twist) that points the segment along the driver's, in the
		// retarget pose, about the bone's own joint; then into the parent's retarget space, and the retargeter's offset
		// taken off again (retarget local = reference local times offset), so the retargeter applying it lands here.
		const FQuat Swing = FQuat::FindBetweenVectors(Mine, Theirs);
		const FQuat Turned = (Swing * Retarget[Bone].GetRotation()).GetNormalized();
		const int32 Parent = Parents[Bone];
		FQuat LocalRotation = Parent != INDEX_NONE ? Retarget[Parent].GetRotation().Inverse() * Turned : Turned;
		if (const FQuat* Offset = Driver.TargetOffsets.Find(Step.Bone))
		{
			LocalRotation = LocalRotation * Offset->Inverse();
		}
		OutPlan.LocalPose[Bone].SetRotation(LocalRotation.GetNormalized());
		OutPlan.Turns.Add({ Step.Bone, Turn });
	}

	// Each foot keeps its component rotation: its sole stays at the angle to the floor the character was made with.
	FMantleAvatarRepose::ComposeComponent(Parents, OutPlan.LocalPose, Component);
	for (const TCHAR* Foot : { TEXT("foot_l"), TEXT("foot_r") })
	{
		const int32 Bone = RetargetPoseFindBone(Names, FName(Foot));
		const int32 Parent = Parents[Bone];
		const FQuat Kept = Parent != INDEX_NONE ? Component[Parent].GetRotation().Inverse() * FramedComponent[Bone].GetRotation() : FramedComponent[Bone].GetRotation();
		OutPlan.LocalPose[Bone].SetRotation(Kept.GetNormalized());
	}

	RetargetPoseComponent(Names, Parents, OutPlan.LocalPose, Driver.TargetOffsets, Retarget);
	for (int32 Index = 0; Index < Reported.Num(); ++Index)
	{
		OutPlan.Segments[Index].AfterDegrees = RetargetPoseDegrees(BodySegment(Retarget, Reported[Index]), DriverSegment(Reported[Index]));
	}
	return true;
}

bool FMantleAvatarRetargetPose::PlanForMesh(const USkeletalMesh& Mesh, const FMantleRetargetPoseDriver& Driver, FMantleRetargetPosePlan& OutPlan, FString& OutError)
{
	TArray<FName> Names;
	TArray<int32> Parents;
	TArray<FTransform> Local;
	FMantleAvatarRepose::ReadMesh(Mesh, Names, Parents, Local);
	return Plan(Names, Parents, Local, Driver, OutPlan, OutError);
}

bool FMantleAvatarRetargetPose::ReadRetargeter(const UObject* Retargeter, FMantleRetargetPoseDriver& OutDriver, FString& OutError)
{
	using namespace MantleAvatarRetargetPosePrivate;
	OutDriver = FMantleRetargetPoseDriver();
	const UClass* RetargeterClass = FindObject<UClass>(nullptr, TEXT("/Script/IKRig.IKRetargeter"));
	if (!Retargeter || !RetargeterClass || !Retargeter->IsA(RetargeterClass))
	{
		OutError = Retargeter ? FString::Printf(TEXT("%s is not an IK retargeter"), *Retargeter->GetName()) : FString(TEXT("there is no retargeter"));
		return false;
	}
	TMap<FName, FQuat> SourceOffsets;
	if (!RetargetPoseReadOffsets(*Retargeter, /*bTarget*/ true, OutDriver.TargetOffsets, OutError)
		|| !RetargetPoseReadOffsets(*Retargeter, /*bTarget*/ false, SourceOffsets, OutError))
	{
		return false;
	}

	// The driver's retarget pose: the source rig's stored reference pose with the source offsets, as the retargeter applies them.
	TArray<FName> Names;
	TArray<int32> Parents;
	TArray<FTransform> Reference;
	FString SourceRig;
	if (!RetargetPoseReadRigSkeleton(*Retargeter, TEXT("SourceIKRigAsset"), Names, Parents, Reference, SourceRig, OutError))
	{
		return false;
	}
	TArray<FTransform> Local;
	Local.SetNum(Names.Num());
	for (int32 Bone = 0; Bone < Names.Num(); ++Bone)
	{
		const int32 Parent = Parents[Bone];
		if (Parent >= Bone || Parent < INDEX_NONE)
		{
			OutError = FString::Printf(TEXT("%s's bone \"%s\" comes before its parent"), *SourceRig, *Names[Bone].ToString());
			return false;
		}
		Local[Bone] = Parent != INDEX_NONE ? Reference[Bone].GetRelativeTransform(Reference[Parent]) : Reference[Bone];
	}
	TArray<FTransform> Component;
	RetargetPoseComponent(Names, Parents, Local, SourceOffsets, Component);
	for (int32 Bone = 0; Bone < Names.Num(); ++Bone)
	{
		OutDriver.DriverJoints.Add(Names[Bone], Component[Bone].GetLocation());
	}

	// The body the retargeter was authored on (a MetaHuman, a Mannequin): its bone axes.
	TArray<FName> TargetNames;
	TArray<int32> TargetParents;
	TArray<FTransform> TargetComponent;
	FString TargetRig;
	if (!RetargetPoseReadRigSkeleton(*Retargeter, TEXT("TargetIKRigAsset"), TargetNames, TargetParents, TargetComponent, TargetRig, OutError))
	{
		return false;
	}
	for (int32 Bone = 0; Bone < TargetNames.Num(); ++Bone)
	{
		OutDriver.TargetReference.Add(TargetNames[Bone], TargetComponent[Bone]);
	}
	OutDriver.Describe = FString::Printf(TEXT("%s (%d target offsets), driver from %s (%d bones, %d source offsets), axes from %s (%d bones)"),
		*Retargeter->GetName(), OutDriver.TargetOffsets.Num(), *SourceRig, Names.Num(), SourceOffsets.Num(), *TargetRig, TargetNames.Num());
	return true;
}

bool FMantleAvatarRetargetPose::ReadRetargeterForSkeleton(const FString& SkeletonId, FMantleRetargetPoseDriver& OutDriver, FString& OutError)
{
	FMantleAvatarPreviewPlan Plan;
	if (!FMantleAvatarPreviewPlan::ForSkeleton(SkeletonId, /*bHasFace*/ false, Plan, OutError))
	{
		return false;
	}
	const FSoftObjectPath Path(Plan.RetargeterPath);
	UObject* Retargeter = !Path.IsNull() && FPackageName::DoesPackageExist(Path.GetLongPackageName()) ? Path.TryLoad() : nullptr;
	if (!Retargeter)
	{
		OutError = FString::Printf(TEXT("the Mantle plugin's retargeter %s is missing (reinstall the Mantle plugin)"), *Plan.RetargeterPath);
		return false;
	}
	return ReadRetargeter(Retargeter, OutDriver, OutError);
}

FString FMantleAvatarRetargetPose::DescribeTurns(TConstArrayView<FMantleRetargetPoseTurn> Turns)
{
	TArray<FMantleRetargetPoseTurn> Sorted(Turns.GetData(), Turns.Num());
	Sorted.StableSort([](const FMantleRetargetPoseTurn& A, const FMantleRetargetPoseTurn& B) { return A.Degrees > B.Degrees; });
	TArray<FString> Parts;
	for (const FMantleRetargetPoseTurn& Turn : Sorted)
	{
		Parts.Add(FString::Printf(TEXT("%s %.1f%s"), *Turn.Bone.ToString(), Turn.Degrees, *FString::Chr(TCHAR(0xB0))));
	}
	return Parts.Num() > 0 ? FString::Join(Parts, TEXT(", ")) : FString(TEXT("none"));
}

bool FMantleAvatarRetargetPoseFixer::IsBodyToLineUp(const FMantleConvertContext& Context, const int32 PartIndex, const FMantleAvatarContract& Contract,
	FString* OutSkeletonId)
{
	using namespace MantleAvatarRetargetPosePrivate;
	if (!Context.Parts.IsValidIndex(PartIndex))
	{
		return false;
	}
	const FMantleConvertPart& Part = Context.Parts[PartIndex];
	const USkeletalMesh* Mesh = Part.Mesh.Get();
	if (!Mesh || !Part.bIncluded || !Part.Slot.Equals(RetargetPoseAvatarSlot, ESearchCase::CaseSensitive) || RetargetPoseHasDNA(*Mesh))
	{
		return false;
	}
	FString SkeletonId;
	FString Error;
	FMantleAvatarPreviewPlan Retargeting;
	if (!FMantleCosmeticExport::AvatarSkeletonId(Mesh, Contract, SkeletonId, Error) || !FMantleAvatarPreviewPlan::ForSkeleton(SkeletonId, /*bHasFace*/ false, Retargeting, Error))
	{
		return false;
	}
	if (OutSkeletonId)
	{
		*OutSkeletonId = SkeletonId;
	}
	// #625: a MetaHuman-skeleton body (a converted one is Epic's own build) already has the bone axes and A-pose the
	// client's MetaHuman retargeter was authored for; only a Mannequin body, uploaded as it is, is lined up.
	return !SkeletonId.Equals(RetargetPoseMetaHuman, ESearchCase::CaseSensitive);
}

void FMantleAvatarRetargetPoseFixer::Analyze(const FMantleConvertContext& Context, TArray<FMantleConvertFix>& OutFixes) const
{
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get();
	// #625: only an As-is body keeps its own skeleton and pose.
	if (!Contract || Context.Route != EMantleAvatarRoute::AsIs)
	{
		return;
	}
	for (int32 PartIndex = 0; PartIndex < Context.Parts.Num(); ++PartIndex)
	{
		FString SkeletonId;
		if (!IsBodyToLineUp(Context, PartIndex, *Contract, &SkeletonId))
		{
			continue;
		}
		const USkeletalMesh* Mesh = Context.Parts[PartIndex].Mesh.Get();
		FMantleConvertFix Fix;
		Fix.FixerId = GetId();
		Fix.PartIndex = PartIndex;
		Fix.Action = TEXT("gives the arms, hands, fingers and legs its skeleton's bone axes and turns the limbs in the body's reference pose so each "
			"moves as the animation's driver does (no constant angle at the wrists, elbows and knees, knees bending forward, fingers bending "
			"about their own joints without twisting); the feet keep their angle to the floor, the fingers stay where they are; re-binds the mesh into a new one");
		FMantleRetargetPoseDriver Driver;
		FMantleRetargetPosePlan Plan;
		FString Error;
		if (!FMantleAvatarRetargetPose::ReadRetargeterForSkeleton(SkeletonId, Driver, Error) || !FMantleAvatarRetargetPose::PlanForMesh(*Mesh, Driver, Plan, Error))
		{
			Fix.Problem = FString::Printf(TEXT("%s's pose cannot be lined up with the client's animation"), *Mesh->GetName());
			Fix.Action = FString::Printf(TEXT("Can't: %s"), *Error);
			Fix.bCanFix = false;
			OutFixes.Add(Fix);
			continue;
		}
		if (Plan.IsEmpty())
		{
			continue; // Already lined up.
		}
		Fix.Problem = FString::Printf(TEXT("%s's limbs sit at an angle to the client's animation (turns %s; new axes %s; %d finger bones with other axes than the animation's)"),
			*Mesh->GetName(), *FMantleAvatarRetargetPose::DescribeTurns(Plan.Turns), *FMantleAvatarRetargetPose::DescribeTurns(Plan.Reframes),
			Plan.FingerReframes.Num());
		OutFixes.Add(Fix);
	}
}

bool FMantleAvatarRetargetPoseFixer::Apply(const FMantleConvertFix& Fix, FMantleConvertContext& Context, FString& OutError) const
{
	using namespace MantleAvatarRetargetPosePrivate;
	if (Fix.FixerId != GetId() || !Context.Parts.IsValidIndex(Fix.PartIndex))
	{
		OutError = TEXT("the retarget pose fix names no part");
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
		OutError = TEXT("the part has no mesh to line up");
		return false;
	}
	FString SkeletonId;
	if (!IsBodyToLineUp(Context, Fix.PartIndex, *Contract, &SkeletonId))
	{
		// Not a Mannequin body without DNA (an earlier fixer changed it since the fix list): nothing to line up.
		Context.Notes.Add(FString::Printf(TEXT("%s: not lined up with the client's animation (not a Mannequin body)."), *Mesh->GetName()));
		return true;
	}
	if (Context.OutputFolder.IsEmpty() || !FPackageName::IsValidLongPackageName(Context.OutputFolder, /*bIncludeReadOnlyRoots*/ true))
	{
		OutError = FString::Printf(TEXT("\"%s\" is not a folder the converted mesh can go in"), *Context.OutputFolder);
		return false;
	}
	// Planned again on the mesh as it stands: an earlier fixer (Repose, Height) may have replaced it after the fix list was made.
	FMantleRetargetPoseDriver Driver;
	FMantleRetargetPosePlan Plan;
	if (!FMantleAvatarRetargetPose::ReadRetargeterForSkeleton(SkeletonId, Driver, OutError) || !FMantleAvatarRetargetPose::PlanForMesh(*Mesh, Driver, Plan, OutError))
	{
		return false;
	}
	// #774: one line per As-is body, also when its fingers already had the skeleton's axes.
	UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_ASIS_FINGERS %s: %d finger bones realigned, largest turn %.1f degrees"), *Mesh->GetName(),
		Plan.FingerReframes.Num(), Plan.MaxFingerReframeDegrees());
	if (Plan.IsEmpty())
	{
		return true; // Already lined up.
	}

	TArray<FName> Names;
	TArray<int32> Parents;
	TArray<FTransform> Local;
	FMantleAvatarRepose::ReadMesh(*Mesh, Names, Parents, Local);

	// The upload's pose check (#582) on the result: never write a body it would refuse.
	const FMantleContractSkeleton* Fixture = Contract->FindSkeleton(SkeletonId);
	if (Fixture && Fixture->HasReferencePose())
	{
		TArray<FMantleReposeChain> Chains;
		if (!FMantleAvatarRepose::MeasureChains(Names, Parents, Plan.LocalPose, *Fixture, Chains, OutError))
		{
			return false;
		}
		const double Tolerance = Contract->ChainToleranceDegrees;
		const FMantleReposeChain* Off = Chains.FindByPredicate([Tolerance](const FMantleReposeChain& Chain) { return Chain.Degrees > Tolerance; });
		if (Off)
		{
			OutError = FString::Printf(TEXT("lined up with the client's animation, %s to %s would be %.0f degrees from the %s (at most %g), and the upload would refuse it"),
				*Off->From.ToString(), *Off->To.ToString(), Off->Degrees, *Fixture->ReferencePoseName, Tolerance);
			return false;
		}
	}

	// Re-bound from the pose with the new axes (the mesh as it is) to the new pose: only step 2's turns move vertices.
	TArray<FTransform> FramedComponent;
	TArray<FTransform> NewComponent;
	FMantleAvatarRepose::ComposeComponent(Parents, Plan.FramedPose, FramedComponent);
	FMantleAvatarRepose::ComposeComponent(Parents, Plan.LocalPose, NewComponent);
	const TArray<FTransform>& NewLocal = Plan.LocalPose;
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	FString PackageName;
	FString AssetName;
	AssetTools.CreateUniqueAssetName(Context.OutputFolder / (Mesh->GetName() + TEXT("_RetargetPose")), FString(), PackageName, AssetName);
	USkeletalMesh* Posed = FMantleAvatarRepose::WriteWithPose(Mesh, Plan.LocalPose, FPackageName::GetLongPackagePath(PackageName), AssetName,
		[&Names, &FramedComponent, &NewComponent, &NewLocal](int32 /*LOD*/, FMeshDescription& Description, FString& OutLODError)
		{
			return FMantleAvatarRepose::RebindMeshDescription(Description, Names, FramedComponent, NewComponent, NewLocal, OutLODError);
		}, OutError);
	if (!Posed)
	{
		return false;
	}
	TArray<FString> Segments;
	for (const FMantleRetargetPoseSegment& Segment : Plan.Segments)
	{
		Segments.Add(FString::Printf(TEXT("%s>%s %.1f->%.1f"), *Segment.From.ToString(), *Segment.To.ToString(), Segment.BeforeDegrees, Segment.AfterDegrees));
	}
	UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_RETARGET_POSE %s -> %s through %s: new axes %s; turned %s; retarget-pose angle to the driver per segment %s"),
		*Mesh->GetName(), *Posed->GetName(), *Driver.Describe, *FMantleAvatarRetargetPose::DescribeTurns(Plan.Reframes),
		*FMantleAvatarRetargetPose::DescribeTurns(Plan.Turns), *FString::Join(Segments, TEXT(", ")));
	Context.Notes.Add(FString::Printf(TEXT("%s: lined up with the client's animation (its skeleton's bone axes on %s and %d finger bones; turned %s; the feet keep their angle to the floor) and re-bound as %s."),
		*Mesh->GetName(), *FMantleAvatarRetargetPose::DescribeTurns(Plan.Reframes), Plan.FingerReframes.Num(), *FMantleAvatarRetargetPose::DescribeTurns(Plan.Turns), *Posed->GetName()));
	Context.CreatedAssets.Add(Posed);
	Part.Mesh = Posed;
	return true;
}

void RegisterMantleAvatarRetargetPoseFixer()
{
	FMantleAvatarFixers::Register(MakeShared<FMantleAvatarRetargetPoseFixer>());
}
