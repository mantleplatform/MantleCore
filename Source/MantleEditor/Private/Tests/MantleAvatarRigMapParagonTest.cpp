// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAvatarContract.h"
#include "MantleAvatarRigMapFixer.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// A Paragon hero (a UE4-style rig: the UE4 Mannequin's bone names, plus bones of its own) is mapped by As-is's RigMap fixer
// like any known rig. Synthetic bone lists with Paragon's names, no assets.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleRigMapParagonTestPrivate
{
	/** A Paragon-style humanoid, Z up and facing +X (the character's left is -Y), 165 cm. */
	struct FParagonRig
	{
		TArray<FMantleRigBone> Bones;
		TArray<FVector> Positions;

		int32 Add(const TCHAR* Name, const int32 Parent, const FVector& Position, const bool bWeighted)
		{
			FMantleRigBone& Bone = Bones.AddDefaulted_GetRef();
			Bone.Name = FName(Name);
			Bone.Parent = Parent;
			Bone.Local = FTransform(FQuat::Identity, Parent == INDEX_NONE ? Position : Position - Positions[Parent]);
			Bone.bWeighted = bWeighted;
			Positions.Add(Position);
			return Bones.Num() - 1;
		}

		int32 Find(const TCHAR* Name) const
		{
			return Bones.IndexOfByPredicate([Name](const FMantleRigBone& Bone) { return Bone.Name == FName(Name); });
		}
	};

	/**
	 * Terra's layout: root with the ik_ bones beside the pelvis, three spine bones, the clavicles and neck on spine_03, the _01
	 * twist bones the UE4 Mannequin has and the _02 ones it does not (bones of the UE5 Mannequin and MetaHuman), a weapon bone and
	 * an armour bone. bWeightTwist02: the heroes weight the _02 twist bones, which is what the upload refuses.
	 */
	FParagonRig MakeParagonRig(const bool bWeightTwist02)
	{
		FParagonRig Rig;
		const int32 Root = Rig.Add(TEXT("root"), INDEX_NONE, FVector(0, 0, 0), false);
		const int32 IkFootRoot = Rig.Add(TEXT("ik_foot_root"), Root, FVector(0, 0, 0), false);
		Rig.Add(TEXT("ik_foot_l"), IkFootRoot, FVector(0, -10, 8), false);
		Rig.Add(TEXT("ik_foot_r"), IkFootRoot, FVector(0, 10, 8), false);
		const int32 IkHandRoot = Rig.Add(TEXT("ik_hand_root"), Root, FVector(0, 0, 0), false);
		const int32 IkHandGun = Rig.Add(TEXT("ik_hand_gun"), IkHandRoot, FVector(0, 70, 150), false);
		Rig.Add(TEXT("ik_hand_l"), IkHandGun, FVector(0, -70, 150), false);
		Rig.Add(TEXT("ik_hand_r"), IkHandGun, FVector(0, 70, 150), false);
		const int32 Pelvis = Rig.Add(TEXT("pelvis"), Root, FVector(0, 0, 100), true);
		Rig.Add(TEXT("buttarmor"), Pelvis, FVector(0, 0, 98), true);
		const int32 Spine1 = Rig.Add(TEXT("spine_01"), Pelvis, FVector(0, 0, 110), true);
		const int32 Spine2 = Rig.Add(TEXT("spine_02"), Spine1, FVector(0, 0, 125), true);
		const int32 Spine3 = Rig.Add(TEXT("spine_03"), Spine2, FVector(0, 0, 140), true);
		const int32 Neck = Rig.Add(TEXT("neck_01"), Spine3, FVector(0, 0, 155), true);
		Rig.Add(TEXT("head"), Neck, FVector(0, 0, 165), true);
		for (int32 Side = 0; Side < 2; ++Side)
		{
			const double Y = Side == 0 ? -1.0 : 1.0;
			const TCHAR* Suffix = Side == 0 ? TEXT("l") : TEXT("r");
			const auto Named = [Suffix](const TCHAR* Bone) { return FString::Printf(TEXT("%s_%s"), Bone, Suffix); };
			const auto At = [Y](const double Out, const double Z) { return FVector(0, Y * Out, Z); };
			const int32 Clavicle = Rig.Add(*Named(TEXT("clavicle")), Spine3, At(5, 150), true);
			const int32 Upper = Rig.Add(*Named(TEXT("upperarm")), Clavicle, At(18, 150), true);
			const int32 Lower = Rig.Add(*Named(TEXT("lowerarm")), Upper, At(45, 150), true);
			const int32 Hand = Rig.Add(*Named(TEXT("hand")), Lower, At(70, 150), true);
			Rig.Add(*Named(TEXT("upperarm_twist_01")), Upper, At(28, 150), true);
			Rig.Add(*Named(TEXT("upperarm_twist_02")), Upper, At(33, 150), bWeightTwist02);
			Rig.Add(*Named(TEXT("lowerarm_twist_01")), Lower, At(57, 150), true);
			Rig.Add(*Named(TEXT("weapon")), Hand, At(72, 150), false);
			const int32 Thigh = Rig.Add(*Named(TEXT("thigh")), Pelvis, At(10, 95), true);
			Rig.Add(*Named(TEXT("thigh_twist_01")), Thigh, At(10, 85), true);
			Rig.Add(*Named(TEXT("thigh_twist_02")), Thigh, At(10, 75), bWeightTwist02);
			const int32 Calf = Rig.Add(*Named(TEXT("calf")), Thigh, At(10, 50), true);
			Rig.Add(*Named(TEXT("calf_twist_01")), Calf, At(10, 35), true);
			const int32 Foot = Rig.Add(*Named(TEXT("foot")), Calf, At(10, 8), true);
			Rig.Add(*Named(TEXT("ball")), Foot, FVector(12, Y * 10, 0), true);
		}
		return Rig;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleRigMapParagonGateTest,
	"Mantle.Editor.AvatarRigMap.ParagonGate",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleRigMapParagonGateTest::RunTest(const FString& Parameters)
{
	using namespace MantleRigMapParagonTestPrivate;
	FString ContractError;
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get(&ContractError);
	if (!TestNotNull(*FString::Printf(TEXT("the plugin's contract loads (%s)"), *ContractError), Contract))
	{
		return false;
	}

	// The upload would refuse a hero that weights its _02 twist bones: they are bones of other supported skeletons, never folded.
	const FParagonRig Weighted = MakeParagonRig(true);
	TestFalse(TEXT("a UE4-style rig that weights upperarm_twist_02 is not on a supported skeleton"), FMantleAvatarRigMap::IsOnSupportedSkeleton(Weighted.Bones, *Contract));

	// The same rig with those bones unweighted is left out silently, so it is the UE4 Mannequin as far as the upload goes.
	const FParagonRig Clean = MakeParagonRig(false);
	TestTrue(TEXT("with the _02 twist bones unweighted it is"), FMantleAvatarRigMap::IsOnSupportedSkeleton(Clean.Bones, *Contract));

	// Names alone still decide a rig without the required bones.
	FParagonRig NoNeck = MakeParagonRig(false);
	NoNeck.Bones[NoNeck.Find(TEXT("neck_01"))].Name = FName(TEXT("neck_renamed"));
	TestFalse(TEXT("a rig missing a required bone is not"), FMantleAvatarRigMap::IsOnSupportedSkeleton(NoNeck.Bones, *Contract));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleRigMapParagonMapTest,
	"Mantle.Editor.AvatarRigMap.ParagonMap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleRigMapParagonMapTest::RunTest(const FString& Parameters)
{
	using namespace MantleRigMapParagonTestPrivate;
	FString ContractError;
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get(&ContractError);
	if (!TestNotNull(*FString::Printf(TEXT("the plugin's contract loads (%s)"), *ContractError), Contract))
	{
		return false;
	}
	const FParagonRig Rig = MakeParagonRig(true);

	// The built-in profile table knows the rig.
	bool bKnown = false;
	const FMantleRigMapping Mapping = FMantleAvatarRigMap::Detect(Rig.Bones, *Contract, FMantleRigProfiles::BuiltIn(), bKnown);
	TestTrue(TEXT("the rig is a known one"), bKnown);
	TestTrue(TEXT("the profile is the UE4-style rig"), Mapping.ProfileId.Equals(TEXT("ue4rig"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("it maps onto the UE4 Mannequin"), Mapping.TargetSkeletonId.Equals(TEXT("ue4mannequin"), ESearchCase::CaseSensitive));
	TestEqual(TEXT("no bone the converter cannot insert is missing"), Mapping.MissingBones().Num(), 0);
	TestTrue(TEXT("a bone maps to itself"), Mapping.SourceFor(FName(TEXT("hand_l"))) == FName(TEXT("hand_l")));
	TestTrue(TEXT("so does a _01 twist bone"), Mapping.SourceFor(FName(TEXT("upperarm_twist_01_r"))) == FName(TEXT("upperarm_twist_01_r")));
	TestTrue(TEXT("the _02 twist bone is not mapped"), Mapping.TargetFor(FName(TEXT("upperarm_twist_02_l"))).IsNone());

	// The plan is ready and the upload identifies the converted rig as the UE4 Mannequin.
	FMantleRigPlan Plan;
	FString Error;
	const bool bPlanned = FMantleAvatarRigMap::MakePlan(Rig.Bones, Mapping, *Contract, FBox(ForceInit), Plan, Error);
	if (!TestTrue(*FString::Printf(TEXT("the plan is made (%s)"), *Error), bPlanned))
	{
		return false;
	}
	const bool bVerified = FMantleAvatarRigMap::VerifyPlan(Rig.Bones, Plan, *Contract, Error);
	TestTrue(*FString::Printf(TEXT("the converted rig is the UE4 Mannequin to the upload (%s)"), *Error), bVerified);
	TestEqual(TEXT("the plan's root is first"), Plan.Bones.Num() > 0 ? Plan.Bones[0].Name : FName(), FName(TEXT("root")));

	const auto PlanBone = [&Plan](const TCHAR* Name)
	{
		return Plan.Bones.IndexOfByPredicate([Name](const FMantleRigPlanBone& Bone) { return Bone.Name == FName(Name); });
	};
	TestTrue(TEXT("the _01 twist bone stays under its own name"), PlanBone(TEXT("upperarm_twist_01_l")) != INDEX_NONE);
	TestTrue(TEXT("the ik_ bones are dropped"), PlanBone(TEXT("ik_foot_root")) == INDEX_NONE && PlanBone(TEXT("ik_hand_gun")) == INDEX_NONE);

	// The _02 twist bone keeps its weights as an extra under a name no supported skeleton has, so the upload folds it.
	const int32 Renamed = PlanBone(TEXT("src_upperarm_twist_02_l"));
	TestTrue(TEXT("the _02 twist bone is renamed"), Renamed != INDEX_NONE);
	TestTrue(TEXT("its plan bone is not a mapped one"), Renamed != INDEX_NONE && !Plan.Bones[Renamed].bMapped);
	TestTrue(TEXT("the plan lists the rename"), Plan.RenamedExtras.ContainsByPredicate([](const TPair<FName, FName>& Pair)
	{
		return Pair.Key == FName(TEXT("upperarm_twist_02_l")) && Pair.Value == FName(TEXT("src_upperarm_twist_02_l"));
	}));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
