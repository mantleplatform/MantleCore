// Copyright Joshua Eger. All Rights Reserved.
#include "MantleOutfitSections.h"
#include "MantleParametricOutfit.h"

#include "Engine/SkeletalMesh.h"
#include "MetaHumanCharacter.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// Which meshes beside a garment may be taken as the character's whole body (route 2 of a skeletal garment's sizes).

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleParametricOutfitWholeBodyTest,
	"Mantle.Editor.ParametricOutfit.WholeBody",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleParametricOutfitWholeBodyTest::RunTest(const FString& Parameters)
{
	const TArray<FVector3f> Whole = { FVector3f(0, 0, 0), FVector3f(5, 0, 90), FVector3f(0, 0, 168) };
	const TArray<FVector3f> Feet = { FVector3f(0, 0, 0), FVector3f(10, 0, 4), FVector3f(0, 5, 12) };
	const TArray<FVector3f> Torso = { FVector3f(0, 0, 95), FVector3f(0, 0, 130), FVector3f(0, 0, 160) };
	TestTrue(TEXT("a body from feet to head is whole"), FMantleParametricOutfit::IsWholeBody(Whole));
	TestFalse(TEXT("feet alone are not a body"), FMantleParametricOutfit::IsWholeBody(Feet));
	TestFalse(TEXT("a torso alone is not a body"), FMantleParametricOutfit::IsWholeBody(Torso));
	TestFalse(TEXT("no vertices are not a body"), FMantleParametricOutfit::IsWholeBody(TArray<FVector3f>()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleParametricOutfitBareBodyTest,
	"Mantle.Editor.ParametricOutfit.BareBody",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleParametricOutfitBareBodyTest::RunTest(const FString& Parameters)
{
	auto Section = [](const TCHAR* Slot, const TCHAR* Material)
	{
		FMantleOutfitSection Result;
		Result.SlotName = Slot;
		Result.MaterialName = Material;
		return Result;
	};
	// Only a body with no clothing section is confirmed as bare skin automatically.
	TestTrue(TEXT("a body with skin, eyes and lashes is bare"), FMantleParametricOutfit::IsBareBody(
		{ Section(TEXT("Body"), TEXT("M_Skin")), Section(TEXT("Eyes"), TEXT("M_Eyes")), Section(TEXT("Lashes"), TEXT("M_Lashes")) }, TEXT("SK_VampireGirl")));
	TestFalse(TEXT("a body with a shirt section is not"), FMantleParametricOutfit::IsBareBody(
		{ Section(TEXT("Body"), TEXT("M_Skin")), Section(TEXT("Shirt"), TEXT("M_Shirt")) }, TEXT("SK_Girl_Body")));
	TestFalse(TEXT("a body with a pants section is not"), FMantleParametricOutfit::IsBareBody(
		{ Section(TEXT("Body"), TEXT("M_Skin")), Section(TEXT("Pants"), TEXT("M_Pants")) }, TEXT("SK_Dressed")));
	TestFalse(TEXT("a head is not a body"), FMantleParametricOutfit::IsBareBody({ Section(TEXT("Head"), TEXT("M_Skin")) }, TEXT("SK_Head")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleParametricOutfitThirdPartyTest,
	"Mantle.Editor.ParametricOutfit.ThirdPartyConversion",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleParametricOutfitThirdPartyTest::RunTest(const FString& Parameters)
{
	// A body converted from a MetaHuman Creator character (Aera, saved under Aera/Converted/Aera) is the wrap route's; one
	// converted from a third-party character's mesh is not; one from no asset (an assembly folder) is not refused.
	TestFalse(TEXT("a Creator character's conversion"), FMantleParametricOutfit::IsThirdPartyConversion(UMetaHumanCharacter::StaticClass()->GetClassPathName()));
	TestTrue(TEXT("a third-party mesh's conversion"), FMantleParametricOutfit::IsThirdPartyConversion(USkeletalMesh::StaticClass()->GetClassPathName()));
	TestFalse(TEXT("no source asset"), FMantleParametricOutfit::IsThirdPartyConversion(FTopLevelAssetPath()));
	return true;
}

#endif
