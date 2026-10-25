// Copyright Joshua Eger. All Rights Reserved.
#include "MantleMetaHumanBuild.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// An assembled MetaHuman that no build step could run on says so, and a refused fit names the mesh and Epic's code and
// never offers As-is. Messages only, no editor world.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleAssemblyBuildMessageTest,
	"Mantle.Editor.AvatarAssembly.BuildMessages",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleAssemblyBuildMessageTest::RunTest(const FString& Parameters)
{
	const FString Refusal = FMantleMetaHumanBuild::AssemblyFitRefusal(TEXT("SKM_Kid06_Body"), TEXT("body"), TEXT("InvalidInputData"));
	TestTrue(TEXT("the refusal names the mesh"), Refusal.Contains(TEXT("SKM_Kid06_Body")));
	TestTrue(TEXT("the refusal names the part that was refused"), Refusal.Contains(TEXT("the body of")));
	TestTrue(TEXT("the refusal names Epic's code"), Refusal.Contains(TEXT("InvalidInputData")));
	TestTrue(TEXT("the refusal says a MetaHuman is never uploaded as it is"), Refusal.Contains(TEXT("not uploaded as it is")));

	const FString NotBuilt = FMantleMetaHumanBuild::AssemblyNotBuiltMessage(TEXT("Kid06"), TEXT("no face mesh was found next to its body"));
	TestTrue(TEXT("the message says the source is an assembly"), NotBuilt.Contains(TEXT("assembled MetaHuman")));
	TestTrue(TEXT("the message says why"), NotBuilt.Contains(TEXT("no face mesh was found")));
	TestTrue(TEXT("the message says no build step ran"), NotBuilt.Contains(TEXT("No build step ran")));

	const FString BodySkeleton = TEXT("/Game/MetaHumans/Common/Female/Medium/NormalWeight/Body/metahuman_base_skel.metahuman_base_skel");
	const FString FaceSkeleton = TEXT("/MetaHumanCharacter/Face/Face_Archetype_Skeleton.Face_Archetype_Skeleton");
	TestTrue(TEXT("a MetaHuman body and face skeleton is a MetaHuman assembly"), FMantleMetaHumanBuild::IsMetaHumanAssembly(BodySkeleton, FaceSkeleton));
	TestFalse(TEXT("another humanoid's body skeleton is not"), FMantleMetaHumanBuild::IsMetaHumanAssembly(TEXT("/Game/Paragon/Hero/Hero_Skeleton.Hero_Skeleton"), FaceSkeleton));
	TestFalse(TEXT("a MetaHuman body with another face skeleton is not"), FMantleMetaHumanBuild::IsMetaHumanAssembly(BodySkeleton, TEXT("/Game/Hero/Hero_Skeleton.Hero_Skeleton")));
	TestFalse(TEXT("a missing skeleton is not"), FMantleMetaHumanBuild::IsMetaHumanAssembly(BodySkeleton, FString()));
	// A refused body fit names the next step: the face-only choice, never As-is.
	TestTrue(TEXT("the hint offers keeping the face on MetaHuman's own body"), FMantleMetaHumanBuild::AssemblyFaceOnlyHint().Contains(TEXT("Put the face on a MetaHuman body")));
	TestTrue(TEXT("the hint names the body choice"), FMantleMetaHumanBuild::AssemblyFaceOnlyHint().Contains(TEXT("Women's or Men's")));

	// Wasteland Kid 1: 62,669 vertices (the outfit merged into the body) is far past a MetaHuman body; a MetaHuman body is not.
	TestTrue(TEXT("62,669 vertices against an 8,568-vertex reference is a merged body"), FMantleMetaHumanBuild::BodyLooksMergedWithClothing(62669, 0, 8568));
	TestTrue(TEXT("62,669 vertices against its own 14,000-vertex DNA is a merged body"), FMantleMetaHumanBuild::BodyLooksMergedWithClothing(62669, 14000, 0));
	TestFalse(TEXT("a body a little over its DNA is a MetaHuman body"), FMantleMetaHumanBuild::BodyLooksMergedWithClothing(15000, 14000, 8568));
	TestFalse(TEXT("1.5 times its DNA exactly is not past it"), FMantleMetaHumanBuild::BodyLooksMergedWithClothing(21000, 14000, 0));
	TestTrue(TEXT("past 1.5 times its DNA is"), FMantleMetaHumanBuild::BodyLooksMergedWithClothing(21001, 14000, 0));
	TestFalse(TEXT("a higher LOD0 than the reference, with no DNA, is not read as merged"), FMantleMetaHumanBuild::BodyLooksMergedWithClothing(30000, 0, 8568));
	TestFalse(TEXT("nothing known about the body offers nothing"), FMantleMetaHumanBuild::BodyLooksMergedWithClothing(62669, 0, 0));
	TestFalse(TEXT("an empty body offers nothing"), FMantleMetaHumanBuild::BodyLooksMergedWithClothing(0, 14000, 8568));

	// The face's DNA asset: the only one in the mesh's folder (Kid09_DNA); nothing is guessed among several.
	const TArray<FString> OneDna = { TEXT("Kid09_DNA") };
	const TArray<FString> NoDna;
	const TArray<FString> TwoDna = { TEXT("Kid09_DNA"), TEXT("Kid08_DNA") };
	TestEqual(TEXT("the only DNA asset beside the face is its DNA"), FMantleMetaHumanBuild::FaceDnaAssetIndex(OneDna), 0);
	TestEqual(TEXT("no DNA asset: none"), FMantleMetaHumanBuild::FaceDnaAssetIndex(NoDna), static_cast<int32>(INDEX_NONE));
	TestEqual(TEXT("two DNA assets: none is guessed"), FMantleMetaHumanBuild::FaceDnaAssetIndex(TwoDna), static_cast<int32>(INDEX_NONE));

	// A body that Epic's fit refused is remembered for the session, by its path.
	const FString RefusedPath = TEXT("/Game/MetaHumans/KidTest/Body/SKM_KidTest_Refused.SKM_KidTest_Refused");
	TestFalse(TEXT("a body not yet refused"), FMantleMetaHumanBuild::BodyFitWasRefused(RefusedPath));
	FMantleMetaHumanBuild::NoteBodyFitRefused(RefusedPath);
	TestTrue(TEXT("a refused body is remembered"), FMantleMetaHumanBuild::BodyFitWasRefused(RefusedPath));
	TestFalse(TEXT("another body is not"), FMantleMetaHumanBuild::BodyFitWasRefused(TEXT("/Game/MetaHumans/KidTest/Body/SKM_KidTest_Other.SKM_KidTest_Other")));
	return true;
}

#endif
