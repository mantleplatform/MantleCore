// Copyright Joshua Eger. All Rights Reserved.
#include "MantleMetaHumanHeadOnly.h"
#include "MantleAvatarStripFixer.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// The face route's pure rules, on synthetic slots and skeletons. No assets.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleMetaHumanHeadOnlyTestPrivate
{
	FMantleStripSlot HeadOnlyTestSlot(const int32 Index, const bool bSkin, const double MinZ, const double MaxZ)
	{
		FMantleStripSlot Slot;
		Slot.Index = Index;
		Slot.SlotName = FName(*FString::Printf(TEXT("Slot%d"), Index));
		Slot.bSkin = bSkin;
		Slot.Triangles = 100;
		Slot.Bounds = FBox3d(FVector(-20.0, -10.0, MinZ), FVector(20.0, 10.0, MaxZ));
		return Slot;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMetaHumanHeadOnlyCoveredTest,
	"Mantle.Editor.MetaHumanHeadOnly.Covered",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMetaHumanHeadOnlyCoveredTest::RunTest(const FString& Parameters)
{
	using namespace MantleMetaHumanHeadOnlyTestPrivate;
	FString Why;

	FMantleStripPlan Bare;
	Bare.Slots = { HeadOnlyTestSlot(0, true, 0.0, 170.0), HeadOnlyTestSlot(1, false, 150.0, 175.0) };
	TestFalse(TEXT("skin from the feet to the head is a bare body"), FMantleMetaHumanHeadOnly::IsCovered(Bare, Why));

	FMantleStripPlan NoSkin;
	NoSkin.Slots = { HeadOnlyTestSlot(0, false, 0.0, 170.0), HeadOnlyTestSlot(1, false, 140.0, 175.0) };
	TestTrue(TEXT("no slot reads as skin: covered"), FMantleMetaHumanHeadOnly::IsCovered(NoSkin, Why));
	TestTrue(TEXT("and says so"), Why.Contains(TEXT("bare skin")));

	FMantleStripPlan HeadOnly;
	HeadOnly.Slots = { HeadOnlyTestSlot(0, true, 145.0, 172.0), HeadOnlyTestSlot(1, false, 0.0, 160.0) };
	TestTrue(TEXT("skin on the head alone (armour as the body): covered"), FMantleMetaHumanHeadOnly::IsCovered(HeadOnly, Why));

	FMantleStripPlan NoLegs;
	NoLegs.Slots = { HeadOnlyTestSlot(0, true, 60.0, 172.0), HeadOnlyTestSlot(1, false, 0.0, 90.0) };
	TestTrue(TEXT("skin that stops above the legs: covered"), FMantleMetaHumanHeadOnly::IsCovered(NoLegs, Why));
	TestTrue(TEXT("and names the legs"), Why.Contains(TEXT("legs")));

	FMantleStripPlan ShortBoots;
	ShortBoots.Slots = { HeadOnlyTestSlot(0, true, 25.0, 172.0), HeadOnlyTestSlot(1, false, 0.0, 30.0) };
	TestFalse(TEXT("boots over the feet alone leave a bare body"), FMantleMetaHumanHeadOnly::IsCovered(ShortBoots, Why));

	FMantleStripPlan Fused;
	Fused.Slots = { HeadOnlyTestSlot(0, true, 0.0, 170.0), HeadOnlyTestSlot(1, false, 150.0, 175.0) };
	Fused.Slots[0].Fused = EMantleFusedVerdict::FusedClothing;
	Fused.Slots[0].FusedWhy = TEXT("clothing looks built into the body");
	TestTrue(TEXT("skin from the feet to the head with clothing built in: covered"), FMantleMetaHumanHeadOnly::IsCovered(Fused, Why));
	TestTrue(TEXT("and says so"), Why.Contains(TEXT("built into the body")));
	Fused.Slots[0].Fused = EMantleFusedVerdict::Unknown;
	TestFalse(TEXT("an unknown verdict leaves a bare body"), FMantleMetaHumanHeadOnly::IsCovered(Fused, Why));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMetaHumanHeadOnlyNamesTest,
	"Mantle.Editor.MetaHumanHeadOnly.Names",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMetaHumanHeadOnlyNamesTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("hair is not the face"), FMantleMetaHumanHeadOnly::NamesNotFace(TEXT("MI_Terra_Hair")));
	TestTrue(TEXT("a helmet is not the face"), FMantleMetaHumanHeadOnly::NamesNotFace(TEXT("Helmet_Mat")));
	TestTrue(TEXT("glasses are not the face"), FMantleMetaHumanHeadOnly::NamesNotFace(TEXT("Glasses")));
	TestFalse(TEXT("the head's skin is the face"), FMantleMetaHumanHeadOnly::NamesNotFace(TEXT("M_Head_Skin")));
	TestFalse(TEXT("eyes stay with the face"), FMantleMetaHumanHeadOnly::NamesNotFace(TEXT("Eyes")));
	TestTrue(TEXT("a ponytail bone is hair"), FMantleMetaHumanHeadOnly::NamesHairBone(TEXT("ponytail_03")));
	TestTrue(TEXT("a braid bone is hair"), FMantleMetaHumanHeadOnly::NamesHairBone(TEXT("Braid_L_01")));
	TestFalse(TEXT("the head bone is not hair"), FMantleMetaHumanHeadOnly::NamesHairBone(TEXT("head")));
	TestTrue(TEXT("a slot ending in eyes holds the eyes"), FMantleMetaHumanHeadOnly::NamesEyes(TEXT("Terra_Eyes")));
	TestTrue(TEXT("and one ending in eye"), FMantleMetaHumanHeadOnly::NamesEyes(TEXT("M_Eye")));
	TestFalse(TEXT("eyelashes are not the eyes"), FMantleMetaHumanHeadOnly::NamesEyes(TEXT("Eyelashes")));
	TestTrue(TEXT("a VRoid iris slot named after its material instance holds the eyes"),
		FMantleMetaHumanHeadOnly::NamesEyes(TEXT("N00_000_00_EyeIris_00_EYE (Instance)")));
	TestFalse(TEXT("a VRoid face slot is not the eyes"), FMantleMetaHumanHeadOnly::NamesEyes(TEXT("N00_000_00_Face_00_SKIN (Instance)")));
	TestTrue(TEXT("a VRoid face slot is skin"), FMantleMetaHumanHeadOnly::NamesSkin(TEXT("N00_000_00_Face_00_SKIN (Instance)")));
	TestFalse(TEXT("VRoid hair is not skin"), FMantleMetaHumanHeadOnly::NamesSkin(TEXT("N00_000_Hair_00_HAIR (Instance)")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMetaHumanHeadOnlyHeadBoneTest,
	"Mantle.Editor.MetaHumanHeadOnly.HeadBone",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMetaHumanHeadOnlyHeadBoneTest::RunTest(const FString& Parameters)
{
	const TArray<FName> Epic = { TEXT("root"), TEXT("pelvis"), TEXT("neck_01"), TEXT("head"), TEXT("headtop_end") };
	const TArray<int32> EpicParents = { INDEX_NONE, 0, 1, 2, 3 };
	TestEqual(TEXT("Epic's skeleton: head"), FMantleMetaHumanHeadOnly::FindHeadBone(Epic, EpicParents), 3);

	const TArray<FName> Mixamo = { TEXT("mixamorig:Hips"), TEXT("mixamorig:Neck"), TEXT("mixamorig:Head"), TEXT("mixamorig:HeadTop_End") };
	const TArray<int32> MixamoParents = { INDEX_NONE, 0, 1, 2 };
	TestEqual(TEXT("Mixamo: the shallowest bone ending in head"), FMantleMetaHumanHeadOnly::FindHeadBone(Mixamo, MixamoParents), 2);

	const TArray<FName> Biped = { TEXT("Bip01"), TEXT("Bip01 Neck"), TEXT("Bip01 Head"), TEXT("Bip01 Forehead") };
	const TArray<int32> BipedParents = { INDEX_NONE, 0, 1, 2 };
	TestEqual(TEXT("Biped: Bip01 Head, not the forehead"), FMantleMetaHumanHeadOnly::FindHeadBone(Biped, BipedParents), 2);

	const TArray<FName> None = { TEXT("root"), TEXT("spine") };
	const TArray<int32> NoneParents = { INDEX_NONE, 0 };
	TestEqual(TEXT("no head bone"), FMantleMetaHumanHeadOnly::FindHeadBone(None, NoneParents), static_cast<int32>(INDEX_NONE));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMetaHumanHeadOnlyNeckTest,
	"Mantle.Editor.MetaHumanHeadOnly.Neck",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMetaHumanHeadOnlyNeckTest::RunTest(const FString& Parameters)
{
	// A 170 cm figure of rings: a torso 34 cm wide to 140 cm, a neck 9 cm wide to 150 cm, a head 18 cm wide above, T-pose arms at 140 cm.
	TArray<FVector3f> Vertices;
	auto Ring = [&Vertices](const double Z, const double Radius, const double MiddleX)
	{
		for (int32 Step = 0; Step < 48; ++Step)
		{
			const double Angle = Step * UE_TWO_PI / 48.0;
			Vertices.Add(FVector3f(MiddleX + Radius * FMath::Cos(Angle), 0.6 * Radius * FMath::Sin(Angle), Z));
		}
	};
	for (double Z = 0.0; Z <= 170.0; Z += 1.0)
	{
		Ring(Z, Z <= 140.0 ? 17.0 : Z <= 150.0 ? 4.5 : 9.0, 0.0);
		if (Z >= 136.0 && Z <= 140.0)
		{
			for (double X = 20.0; X <= 70.0; X += 1.0)
			{
				Vertices.Add(FVector3f(X, 0.0, Z));
				Vertices.Add(FVector3f(-X, 0.0, Z));
			}
		}
	}
	FVector Neck;
	double Top = 0.0;
	TestTrue(TEXT("a neck is found"), FMantleMetaHumanHeadOnly::FindNeck(Vertices, Neck, Top));
	TestTrue(TEXT("it is in the neck's height, not the shoulders' or the head's"), Neck.Z > 140.0 && Neck.Z < 151.0);
	TestTrue(TEXT("and the top is the head's"), FMath::IsNearlyEqual(Top, 170.0, 0.01));
	TestFalse(TEXT("no vertices, no neck"), FMantleMetaHumanHeadOnly::FindNeck(TArray<FVector3f>(), Neck, Top));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
