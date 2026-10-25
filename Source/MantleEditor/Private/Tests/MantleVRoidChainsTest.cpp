// Copyright Joshua Eger. All Rights Reserved.
#include "MantleMetaHumanPose.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// VRoid/VRM skeletons (J_Bip_* bones), which Epic's auto characterisation finds no chains on: the converter names them.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVRoidChainsTestPrivate
{
	/** VRoid Studio's export, as Yura's: body, both arms with fingers, both legs, plus hair and secondary bones. */
	TArray<FName> VRoidTestBones(const bool bUpperChest)
	{
		TArray<FName> Out;
		for (const TCHAR* Core : { TEXT("Root"), TEXT("J_Bip_C_Hips"), TEXT("J_Bip_C_Spine"), TEXT("J_Bip_C_Chest"), TEXT("J_Bip_C_Neck"),
			TEXT("J_Bip_C_Head"), TEXT("J_Sec_Hair1_01"), TEXT("J_Adj_L_FaceEye") })
		{
			Out.Add(Core);
		}
		if (bUpperChest)
		{
			Out.Add(TEXT("J_Bip_C_UpperChest"));
		}
		for (const TCHAR* Side : { TEXT("L"), TEXT("R") })
		{
			for (const TCHAR* Bone : { TEXT("Shoulder"), TEXT("UpperArm"), TEXT("LowerArm"), TEXT("Hand"), TEXT("UpperLeg"), TEXT("LowerLeg"), TEXT("Foot"),
				TEXT("ToeBase") })
			{
				Out.Add(FName(FString::Printf(TEXT("J_Bip_%s_%s"), Side, Bone)));
			}
			for (const TCHAR* Finger : { TEXT("Thumb"), TEXT("Index"), TEXT("Middle"), TEXT("Ring"), TEXT("Little") })
			{
				for (int32 Joint = 1; Joint <= 3; ++Joint)
				{
					Out.Add(FName(FString::Printf(TEXT("J_Bip_%s_%s%d"), Side, Finger, Joint)));
				}
			}
		}
		return Out;
	}

	const FMantleNamedChain* VRoidTestChain(const TArray<FMantleNamedChain>& Chains, const TCHAR* Name)
	{
		return Chains.FindByPredicate([Name](const FMantleNamedChain& Chain) { return Chain.Chain == FName(Name); });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVRoidChainsTest,
	"Mantle.Editor.MetaHumanPose.VRoidChains",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVRoidChainsTest::RunTest(const FString& Parameters)
{
	using namespace MantleVRoidChainsTestPrivate;
	FName Pelvis;
	const TArray<FMantleNamedChain> Chains = FMantleMetaHumanPose::VRoidChains(VRoidTestBones(true), Pelvis);
	TestEqual(TEXT("the hips are the pelvis"), Pelvis, FName(TEXT("J_Bip_C_Hips")));
	// Spine, Neck, Head; per side Leg, Foot, Clavicle, Arm and five fingers.
	TestEqual(TEXT("every chain is named"), Chains.Num(), 3 + 2 * 9);
	for (const FName& Required : FMantleMetaHumanPose::RequiredChains())
	{
		TestNotNull(*FString::Printf(TEXT("the %s chain the pose check needs"), *Required.ToString()), VRoidTestChain(Chains, *Required.ToString()));
	}
	auto Expect = [&](const TCHAR* Chain, const TCHAR* Start, const TCHAR* End)
	{
		const FMantleNamedChain* Found = VRoidTestChain(Chains, Chain);
		if (TestNotNull(*FString::Printf(TEXT("%s is named"), Chain), Found))
		{
			TestEqual(*FString::Printf(TEXT("%s starts"), Chain), Found->Start, FName(Start));
			TestEqual(*FString::Printf(TEXT("%s ends"), Chain), Found->End, FName(End));
		}
	};
	Expect(TEXT("Spine"), TEXT("J_Bip_C_Spine"), TEXT("J_Bip_C_UpperChest"));
	Expect(TEXT("Neck"), TEXT("J_Bip_C_Neck"), TEXT("J_Bip_C_Neck"));
	Expect(TEXT("Head"), TEXT("J_Bip_C_Head"), TEXT("J_Bip_C_Head"));
	Expect(TEXT("LeftArm"), TEXT("J_Bip_L_UpperArm"), TEXT("J_Bip_L_Hand"));
	Expect(TEXT("RightArm"), TEXT("J_Bip_R_UpperArm"), TEXT("J_Bip_R_Hand"));
	Expect(TEXT("LeftClavicle"), TEXT("J_Bip_L_Shoulder"), TEXT("J_Bip_L_Shoulder"));
	Expect(TEXT("LeftLeg"), TEXT("J_Bip_L_UpperLeg"), TEXT("J_Bip_L_Foot"));
	Expect(TEXT("RightLeg"), TEXT("J_Bip_R_UpperLeg"), TEXT("J_Bip_R_Foot"));
	Expect(TEXT("RightFoot"), TEXT("J_Bip_R_ToeBase"), TEXT("J_Bip_R_ToeBase"));
	Expect(TEXT("LeftThumb"), TEXT("J_Bip_L_Thumb1"), TEXT("J_Bip_L_Thumb3"));
	Expect(TEXT("RightPinky"), TEXT("J_Bip_R_Little1"), TEXT("J_Bip_R_Little3"));

	// No UpperChest (optional in VRM): the spine ends at the chest.
	const TArray<FMantleNamedChain> NoUpper = FMantleMetaHumanPose::VRoidChains(VRoidTestBones(false), Pelvis);
	const FMantleNamedChain* Spine = VRoidTestChain(NoUpper, TEXT("Spine"));
	if (TestNotNull(TEXT("a spine without an upper chest"), Spine))
	{
		TestEqual(TEXT("ends at the chest"), Spine->End, FName(TEXT("J_Bip_C_Chest")));
	}

	// Prefixed and lower-case exports match by name ending.
	TArray<FName> Prefixed;
	for (const FName& Bone : VRoidTestBones(true))
	{
		Prefixed.Add(FName(TEXT("Armature_") + Bone.ToString().ToLower()));
	}
	TestEqual(TEXT("prefixed, lower-case bones still name every chain"), FMantleMetaHumanPose::VRoidChains(Prefixed, Pelvis).Num(), 3 + 2 * 9);

	// Not VRoid, or a VRoid skeleton missing a hand: nothing named, Epic's auto characterisation decides.
	const TArray<FName> Epic = { TEXT("root"), TEXT("pelvis"), TEXT("spine_01"), TEXT("head"), TEXT("upperarm_l"), TEXT("hand_l") };
	TestEqual(TEXT("Epic's skeleton is not VRoid"), FMantleMetaHumanPose::VRoidChains(Epic, Pelvis).Num(), 0);
	TestTrue(TEXT("and has no VRoid pelvis"), Pelvis.IsNone());
	TArray<FName> NoHand = VRoidTestBones(true);
	NoHand.Remove(FName(TEXT("J_Bip_R_Hand")));
	TestEqual(TEXT("a VRoid skeleton without its right hand"), FMantleMetaHumanPose::VRoidChains(NoHand, Pelvis).Num(), 0);
	return true;
}

#endif
