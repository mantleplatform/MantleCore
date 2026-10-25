// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAvatarConverter.h"
#include "MantleAvatarHeight.h"
#include "MantleCosmeticExport.h"
#include "MantleMetaHumanBuild.h"

#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"

#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

// An assembled MetaHuman's face is found by its skeleton and folder when its name doesn't say Face (Fab's Dystopian Kids:
// "SKM_Kid_01", "SKM_Kid_04MetahumanMesh"); a face named for its body still wins. Package names only, no assets.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleFacePickTest,
	"Mantle.Editor.AvatarAssembly.FacePick",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleFacePickTest::RunTest(const FString& Parameters)
{
	const FString FaceSkeleton = TEXT("/MetaHumanCharacter/Face/Face_Archetype_Skeleton.Face_Archetype_Skeleton");
	const FString BodySkeleton = TEXT("/Game/MetaHumans/Common/Female/Medium/NormalWeight/Body/metahuman_base_skel.metahuman_base_skel");
	auto Mesh = [](const FString& Package, const FString& Skeleton)
	{
		FMantleFaceCandidate Candidate;
		Candidate.PackageName = Package;
		Candidate.AssetName = FPackageName::GetShortName(Package);
		Candidate.Skeleton = Skeleton;
		return Candidate;
	};

	const TArray<FMantleFaceCandidate> Named = {
		Mesh(TEXT("/Game/MetaHumans/Kid05/Body/SKM_Kid05_BodyMesh"), BodySkeleton),
		Mesh(TEXT("/Game/MetaHumans/Kid05/Face/SKM_MHC_Kid11_FaceMesh"), FaceSkeleton) };
	TestEqual(TEXT("a face named Face is picked"), FMantleCosmeticExport::PickAuthoringFace(TEXT("SKM_Kid05_BodyMesh"), Named), 1);

	const TArray<FMantleFaceCandidate> Unnamed = {
		Mesh(TEXT("/Game/MetaHumans/Kid01/Body/SKM_Kid01"), BodySkeleton),
		Mesh(TEXT("/Game/MetaHumans/Kid01/Face/SKM_Kid_01"), FaceSkeleton) };
	TestEqual(TEXT("a face under Face/ on the face skeleton is picked without Face in its name"),
		FMantleCosmeticExport::PickAuthoringFace(TEXT("SKM_Kid01"), Unnamed), 1);

	const TArray<FMantleFaceCandidate> Both = {
		Mesh(TEXT("/Game/MetaHumans/Ada/Face/SKM_Ada_Extra"), FaceSkeleton),
		Mesh(TEXT("/Game/MetaHumans/Ada/Face/SKM_Ada_FaceMesh"), FaceSkeleton) };
	TestEqual(TEXT("a face named Face wins over an unnamed one"), FMantleCosmeticExport::PickAuthoringFace(TEXT("SKM_Ada_BodyMesh"), Both), 1);

	const TArray<FMantleFaceCandidate> Elsewhere = {
		Mesh(TEXT("/Game/MetaHumans/Kid04/Grooms/SKM_Kid_04Helmet"), FaceSkeleton),
		Mesh(TEXT("/Game/MetaHumans/Kid04/Body/SKM_Kid04_Body"), BodySkeleton) };
	TestEqual(TEXT("an unnamed mesh on the face skeleton outside Face/ is not a face"),
		FMantleCosmeticExport::PickAuthoringFace(TEXT("SKM_Kid04_Body"), Elsewhere), INDEX_NONE);

	const TArray<FMantleFaceCandidate> WrongSkeleton = { Mesh(TEXT("/Game/MetaHumans/Kid01/Face/SKM_Kid_01"), BodySkeleton) };
	TestEqual(TEXT("a mesh under Face/ on another skeleton is not a face"),
		FMantleCosmeticExport::PickAuthoringFace(TEXT("SKM_Kid01"), WrongSkeleton), INDEX_NONE);
	return true;
}

// The build step's feet guard and the fixed-body hand-off decisions (pure): the lowest point is what ZHeight can't see, a custom
// body fails the build when it isn't the reference topology or isn't at the canonical height.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleFeetAndFixedBodyTest,
	"Mantle.Editor.AvatarAssembly.FeetAndFixedBody",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleFeetAndFixedBodyTest::RunTest(const FString& Parameters)
{
	// A body 165 cm tall standing 40 cm below its feet: the height is right, the lowest point is not.
	const TArray<FVector3f> Sunk = { FVector3f(0.0f, 0.0f, -40.0f), FVector3f(0.0f, 0.0f, 125.0f), FVector3f(5.0f, 5.0f, 60.0f) };
	TestEqual(TEXT("the height of a sunken body is right"), FMantleAvatarHeight::ZHeight(Sunk), 165.0);
	TestEqual(TEXT("its lowest point is -40"), FMantleAvatarHeight::LowestZ(Sunk), -40.0);
	TestEqual(TEXT("no points have lowest point 0"), FMantleAvatarHeight::LowestZ(TArray<FVector3f>()), 0.0);
	TestFalse(TEXT("feet 40 cm below the floor fail a 10 cm tolerance"), FMantleAvatarHeight::FeetOnFloor(-40.0, 10.0));
	TestFalse(TEXT("feet 40 cm above the floor fail it too"), FMantleAvatarHeight::FeetOnFloor(40.0, 10.0));
	TestTrue(TEXT("feet 3 cm below the floor pass"), FMantleAvatarHeight::FeetOnFloor(-3.0, 10.0));
	TestTrue(TEXT("feet exactly at the tolerance pass"), FMantleAvatarHeight::FeetOnFloor(-10.0, 10.0));
	TestFalse(TEXT("an unmeasured (NaN) lowest point is not on the floor"), FMantleAvatarHeight::FeetOnFloor(std::numeric_limits<double>::quiet_NaN(), 10.0));

	TestTrue(TEXT("a fixed body that fails the complete-body check fails the build"), FMantleMetaHumanBuild::FixedBodyCantBuild(true, false, true));
	TestFalse(TEXT("a fixed body that passes it at the canonical height is an ordinary build"), FMantleMetaHumanBuild::FixedBodyCantBuild(true, true, true));
	TestTrue(TEXT("a fixed body off the canonical height fails the build"), FMantleMetaHumanBuild::FixedBodyCantBuild(true, true, false));
	TestFalse(TEXT("any other character that fails it still fails"), FMantleMetaHumanBuild::FixedBodyCantBuild(false, false, false));
	TestFalse(TEXT("any other character that passes it is an ordinary build"), FMantleMetaHumanBuild::FixedBodyCantBuild(false, true, false));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
