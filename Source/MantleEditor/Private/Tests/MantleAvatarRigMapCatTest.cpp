// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAvatarContract.h"
#include "MantleAvatarRigMapFixer.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// A 3ds Max CAT rig (Vexa, Vefects' stylized character: Base-HumanPelvis, Base-HumanLUpperarm1, ...) is mapped by As-is's RigMap
// fixer onto the UE4 Mannequin like any known rig, and so is a VRoid one (Yura: J_Bip_C_Hips, ...). Vexa's own 82 bones (names, parents, bind positions in cm, Z up, facing +Y),
// no assets.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleRigMapCatTestPrivate
{
	struct FCatTestBone
	{
		const TCHAR* Part;
		int32 Parent;
		FVector Position;
	};

	/** Vexa's skeleton, parent-first; Part follows the rig's name ("Base-Human" on Vexa). */
	const FCatTestBone CatTestVexa[] =
	{
		{ TEXT("Pelvis"), -1, FVector(-0.00, -3.11, 97.08) },
		{ TEXT("LThigh"), 0, FVector(11.03, 1.48, 98.78) },
		{ TEXT("LCalf"), 1, FVector(10.28, 0.79, 62.65) },
		{ TEXT("LFoot"), 2, FVector(9.05, -5.00, 5.70) },
		{ TEXT("LToes"), 3, FVector(9.06, 4.52, 3.53) },
		{ TEXT("RThigh"), 0, FVector(-11.03, 1.48, 98.78) },
		{ TEXT("RCalf"), 5, FVector(-10.28, 0.79, 62.65) },
		{ TEXT("RFoot"), 6, FVector(-9.05, -4.98, 5.70) },
		{ TEXT("RToes"), 7, FVector(-9.06, 4.54, 3.53) },
		{ TEXT("Spine1"), 0, FVector(-0.00, -3.47, 101.55) },
		{ TEXT("Spine2"), 9, FVector(-0.00, 0.31, 117.23) },
		{ TEXT("Spine3"), 10, FVector(-0.00, -1.44, 130.86) },
		{ TEXT("Ribcage"), 11, FVector(-0.00, -4.96, 145.20) },
		{ TEXT("LCollarbone"), 12, FVector(2.95, 4.98, 143.50) },
		{ TEXT("LUpperarm1"), 13, FVector(13.25, -4.28, 143.89) },
		{ TEXT("LUpperarm2"), 14, FVector(16.26, -4.72, 136.71) },
		{ TEXT("LUpperarm3"), 15, FVector(19.26, -5.17, 129.53) },
		{ TEXT("LForearm1"), 16, FVector(22.26, -5.61, 122.35) },
		{ TEXT("LForearm2"), 17, FVector(25.83, -5.40, 116.24) },
		{ TEXT("LForearm3"), 18, FVector(29.39, -5.18, 110.13) },
		{ TEXT("LPalm"), 19, FVector(32.96, -4.97, 104.01) },
		{ TEXT("LDigit11"), 20, FVector(33.69, -1.85, 100.22) },
		{ TEXT("LDigit12"), 21, FVector(33.33, 0.07, 95.85) },
		{ TEXT("LDigit13"), 22, FVector(32.85, 1.41, 93.20) },
		{ TEXT("LDigit21"), 20, FVector(34.39, -2.96, 100.21) },
		{ TEXT("LDigit22"), 24, FVector(36.65, -2.26, 93.16) },
		{ TEXT("LDigit23"), 25, FVector(37.14, -1.73, 89.02) },
		{ TEXT("LDigit24"), 26, FVector(36.69, -1.38, 86.45) },
		{ TEXT("LDigit31"), 20, FVector(34.72, -4.57, 100.14) },
		{ TEXT("LDigit32"), 28, FVector(37.06, -4.22, 92.65) },
		{ TEXT("LDigit33"), 29, FVector(37.67, -4.11, 88.33) },
		{ TEXT("LDigit34"), 30, FVector(37.42, -4.01, 85.46) },
		{ TEXT("LDigit41"), 20, FVector(34.65, -6.24, 100.07) },
		{ TEXT("LDigit42"), 32, FVector(37.05, -6.34, 92.58) },
		{ TEXT("LDigit43"), 33, FVector(37.20, -6.56, 88.63) },
		{ TEXT("LDigit44"), 34, FVector(36.82, -6.78, 86.09) },
		{ TEXT("LDigit51"), 20, FVector(34.43, -7.85, 99.91) },
		{ TEXT("LDigit52"), 36, FVector(36.60, -8.08, 93.15) },
		{ TEXT("LDigit53"), 37, FVector(36.60, -8.52, 90.25) },
		{ TEXT("LDigit54"), 38, FVector(36.13, -8.83, 88.82) },
		{ TEXT("Neck"), 12, FVector(-0.00, -5.31, 145.28) },
		{ TEXT("Head"), 40, FVector(-0.00, -5.31, 157.79) },
		{ TEXT("Tail1"), 41, FVector(-0.00, -8.57, 165.03) },
		{ TEXT("Tail2"), 42, FVector(-0.00, -18.07, 167.94) },
		{ TEXT("REyeBrow"), 41, FVector(-2.27, 0.37, 162.45) },
		{ TEXT("LEyeBrow"), 41, FVector(2.27, 0.37, 162.45) },
		{ TEXT("REyeBall"), 41, FVector(-2.87, 1.57, 160.81) },
		{ TEXT("LEyeBall"), 41, FVector(2.87, 1.57, 160.81) },
		{ TEXT("RUpperLash"), 41, FVector(-2.95, 0.33, 161.09) },
		{ TEXT("RLowLash"), 41, FVector(-2.95, 0.33, 160.58) },
		{ TEXT("LUpperLash"), 41, FVector(2.95, 0.33, 161.09) },
		{ TEXT("LLowLash"), 41, FVector(2.95, 0.33, 160.58) },
		{ TEXT("RCollarbone"), 12, FVector(-2.95, 4.98, 143.50) },
		{ TEXT("RUpperarm1"), 52, FVector(-13.25, -4.28, 143.89) },
		{ TEXT("RUpperarm2"), 53, FVector(-16.26, -4.72, 136.71) },
		{ TEXT("RUpperarm3"), 54, FVector(-19.26, -5.17, 129.53) },
		{ TEXT("RForearm1"), 55, FVector(-22.26, -5.61, 122.35) },
		{ TEXT("RForearm2"), 56, FVector(-25.83, -5.39, 116.24) },
		{ TEXT("RForearm3"), 57, FVector(-29.40, -5.16, 110.13) },
		{ TEXT("RPalm"), 58, FVector(-32.97, -4.93, 104.02) },
		{ TEXT("RDigit11"), 59, FVector(-33.69, -1.85, 100.20) },
		{ TEXT("RDigit12"), 60, FVector(-33.33, 0.03, 95.81) },
		{ TEXT("RDigit13"), 61, FVector(-32.84, 1.34, 93.14) },
		{ TEXT("RDigit21"), 59, FVector(-34.39, -2.96, 100.20) },
		{ TEXT("RDigit22"), 63, FVector(-36.63, -2.33, 93.13) },
		{ TEXT("RDigit23"), 64, FVector(-37.11, -1.85, 88.98) },
		{ TEXT("RDigit24"), 65, FVector(-36.66, -1.52, 86.41) },
		{ TEXT("RDigit31"), 59, FVector(-34.72, -4.58, 100.14) },
		{ TEXT("RDigit32"), 67, FVector(-37.04, -4.30, 92.64) },
		{ TEXT("RDigit33"), 68, FVector(-37.64, -4.24, 88.32) },
		{ TEXT("RDigit34"), 69, FVector(-37.38, -4.17, 85.45) },
		{ TEXT("RDigit41"), 59, FVector(-34.65, -6.25, 100.08) },
		{ TEXT("RDigit42"), 71, FVector(-37.02, -6.43, 92.59) },
		{ TEXT("RDigit43"), 72, FVector(-37.17, -6.68, 88.64) },
		{ TEXT("RDigit44"), 73, FVector(-36.78, -6.92, 86.10) },
		{ TEXT("RDigit51"), 59, FVector(-34.43, -7.85, 99.94) },
		{ TEXT("RDigit52"), 75, FVector(-36.57, -8.16, 93.18) },
		{ TEXT("RDigit53"), 76, FVector(-36.57, -8.62, 90.28) },
		{ TEXT("RDigit54"), 77, FVector(-36.09, -8.95, 88.86) },
		{ TEXT("MainBelt"), 0, FVector(6.00, 12.42, 107.20) },
		{ TEXT("SecondBelt"), 79, FVector(6.52, 11.47, 107.08) },
		{ TEXT("BagBelt"), 79, FVector(-21.73, -0.11, 100.99) },
	};

	TArray<FMantleRigBone> CatTestRig(const TCHAR* RigName)
	{
		TArray<FMantleRigBone> Out;
		for (const FCatTestBone& Bone : CatTestVexa)
		{
			FMantleRigBone& Rig = Out.AddDefaulted_GetRef();
			Rig.Name = FName(FString(RigName) + Bone.Part);
			Rig.Parent = Bone.Parent;
			Rig.Local = FTransform(FQuat::Identity, Bone.Parent == INDEX_NONE ? Bone.Position : Bone.Position - CatTestVexa[Bone.Parent].Position);
			Rig.bWeighted = true;
		}
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleRigMapCatTest,
	"Mantle.Editor.AvatarRigMap.CatMap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleRigMapCatTest::RunTest(const FString& Parameters)
{
	using namespace MantleRigMapCatTestPrivate;
	FString ContractError;
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get(&ContractError);
	if (!TestNotNull(*FString::Printf(TEXT("the plugin's contract loads (%s)"), *ContractError), Contract))
	{
		return false;
	}
	const TArray<FMantleRigBone> Vexa = CatTestRig(TEXT("Base-Human"));
	TestFalse(TEXT("Vexa's rig is not on a supported skeleton as it is"), FMantleAvatarRigMap::IsOnSupportedSkeleton(Vexa, *Contract));

	bool bKnown = false;
	const FMantleRigMapping Mapping = FMantleAvatarRigMap::Detect(Vexa, *Contract, FMantleRigProfiles::BuiltIn(), bKnown);
	TestTrue(TEXT("the rig is a known one"), bKnown);
	TestTrue(TEXT("the profile is 3ds Max CAT"), Mapping.ProfileId.Equals(TEXT("3dsmaxcat"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("it maps onto the UE4 Mannequin"), Mapping.TargetSkeletonId.Equals(TEXT("ue4mannequin"), ESearchCase::CaseSensitive));
	TestEqual(TEXT("no bone the converter cannot insert is missing"), Mapping.MissingBones().Num(), 0);
	auto Expect = [&](const TCHAR* Target, const TCHAR* Source)
	{
		TestEqual(*FString::Printf(TEXT("%s is the rig's %s"), Target, Source), Mapping.SourceFor(FName(Target)), FName(FString(TEXT("Base-Human")) + Source));
	};
	Expect(TEXT("pelvis"), TEXT("Pelvis"));
	Expect(TEXT("spine_03"), TEXT("Ribcage"));
	Expect(TEXT("clavicle_l"), TEXT("LCollarbone"));
	Expect(TEXT("upperarm_l"), TEXT("LUpperarm1"));
	Expect(TEXT("lowerarm_r"), TEXT("RForearm1"));
	Expect(TEXT("hand_r"), TEXT("RPalm"));
	Expect(TEXT("thumb_01_l"), TEXT("LDigit11"));
	Expect(TEXT("index_01_l"), TEXT("LDigit22"));
	Expect(TEXT("pinky_03_r"), TEXT("RDigit54"));
	Expect(TEXT("ball_l"), TEXT("LToes"));
	TestTrue(TEXT("a limb's second segment is not mapped"), Mapping.TargetFor(FName(TEXT("Base-HumanLUpperarm2"))).IsNone());
	TestTrue(TEXT("nor a metacarpal"), Mapping.TargetFor(FName(TEXT("Base-HumanLDigit21"))).IsNone());

	// The plan is ready and the upload identifies the converted rig as the UE4 Mannequin.
	FMantleRigPlan Plan;
	FString Error;
	const bool bPlanned = FMantleAvatarRigMap::MakePlan(Vexa, Mapping, *Contract, FBox(ForceInit), Plan, Error);
	if (!TestTrue(*FString::Printf(TEXT("the plan is made (%s)"), *Error), bPlanned))
	{
		return false;
	}
	TestTrue(*FString::Printf(TEXT("the converted rig is the UE4 Mannequin to the upload (%s)"), *Error), FMantleAvatarRigMap::VerifyPlan(Vexa, Plan, *Contract, Error));

	// The rig's own name doesn't matter: the default "Base Human" and a creator's "Bip01" map the same.
	for (const TCHAR* RigName : { TEXT("Base Human"), TEXT("Bip01 "), TEXT("Hero_") })
	{
		bool bOtherKnown = false;
		const FMantleRigMapping Other = FMantleAvatarRigMap::Detect(CatTestRig(RigName), *Contract, FMantleRigProfiles::BuiltIn(), bOtherKnown);
		TestTrue(*FString::Printf(TEXT("a CAT rig named '%s' is known"), RigName), bOtherKnown && Other.ProfileId == TEXT("3dsmaxcat"));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleRigMapVRoidTest,
	"Mantle.Editor.AvatarRigMap.VRoidMap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleRigMapVRoidTest::RunTest(const FString& Parameters)
{
	FString ContractError;
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get(&ContractError);
	if (!TestNotNull(*FString::Printf(TEXT("the plugin's contract loads (%s)"), *ContractError), Contract))
	{
		return false;
	}
	// Yura's (VRoid Studio) humanoid bones, parent-first, with some of her hair and face bones.
	TArray<FMantleRigBone> Bones;
	auto Add = [&Bones](const FString& Name, const int32 Parent)
	{
		FMantleRigBone& Bone = Bones.AddDefaulted_GetRef();
		Bone.Name = FName(*Name);
		Bone.Parent = Parent;
		Bone.bWeighted = true;
		return Bones.Num() - 1;
	};
	const int32 Hips = Add(TEXT("J_Bip_C_Hips"), INDEX_NONE);
	const int32 Spine = Add(TEXT("J_Bip_C_Spine"), Hips);
	const int32 Chest = Add(TEXT("J_Bip_C_Chest"), Spine);
	const int32 Upper = Add(TEXT("J_Bip_C_UpperChest"), Chest);
	const int32 Neck = Add(TEXT("J_Bip_C_Neck"), Upper);
	const int32 Head = Add(TEXT("J_Bip_C_Head"), Neck);
	Add(TEXT("J_Adj_L_FaceEye"), Head);
	Add(TEXT("J_Sec_Hair1_01"), Head);
	for (const TCHAR* Side : { TEXT("L"), TEXT("R") })
	{
		const auto Bip = [Side](const TCHAR* Part) { return FString::Printf(TEXT("J_Bip_%s_%s"), Side, Part); };
		const int32 Shoulder = Add(Bip(TEXT("Shoulder")), Upper);
		const int32 Hand = Add(Bip(TEXT("Hand")), Add(Bip(TEXT("LowerArm")), Add(Bip(TEXT("UpperArm")), Shoulder)));
		for (const TCHAR* Finger : { TEXT("Thumb"), TEXT("Index"), TEXT("Middle"), TEXT("Ring"), TEXT("Little") })
		{
			int32 Parent = Hand;
			for (int32 Joint = 1; Joint <= 3; ++Joint)
			{
				Parent = Add(Bip(*FString::Printf(TEXT("%s%d"), Finger, Joint)), Parent);
			}
		}
		Add(Bip(TEXT("ToeBase")), Add(Bip(TEXT("Foot")), Add(Bip(TEXT("LowerLeg")), Add(Bip(TEXT("UpperLeg")), Hips))));
	}

	bool bKnown = false;
	const FMantleRigMapping Mapping = FMantleAvatarRigMap::Detect(Bones, *Contract, FMantleRigProfiles::BuiltIn(), bKnown);
	TestTrue(TEXT("a VRoid rig is a known one"), bKnown);
	TestTrue(TEXT("the profile is VRoid"), Mapping.ProfileId.Equals(TEXT("vroid"), ESearchCase::CaseSensitive));
	TestEqual(TEXT("no bone the converter cannot insert is missing"), Mapping.MissingBones().Num(), 0);
	TestEqual(TEXT("the upper chest is spine_03"), Mapping.SourceFor(FName(TEXT("spine_03"))), FName(TEXT("J_Bip_C_UpperChest")));
	TestEqual(TEXT("the little finger is the pinky"), Mapping.SourceFor(FName(TEXT("pinky_02_r"))), FName(TEXT("J_Bip_R_Little2")));
	TestTrue(TEXT("hair is not mapped"), Mapping.TargetFor(FName(TEXT("J_Sec_Hair1_01"))).IsNone());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
