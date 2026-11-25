// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAvatarContract.h"
#include "MantleCosmeticExport.h"
#include "MantleEditorActions.h"
#include "MantleFitBodies.h"
#include "MantleParametricOutfit.h"
#include "MantleStandardBodyMatch.h"

#include "Engine/SkeletalMesh.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS

// Which of Epic's standard MetaHuman bodies a skeletal-mesh garment was made on, from its joints (route 1b of a garment's sizes).

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleStandardBodyMatchPureTest,
	"Mantle.Editor.StandardBodyMatch.Pure",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleStandardBodyMatchPureTest::RunTest(const FString& Parameters)
{
	// The 18 bodies, each once.
	const TArray<FString> Ids = FMantleStandardBodyMatch::AllBodyIds();
	TestEqual(TEXT("18 standard bodies"), Ids.Num(), 18);
	TestEqual(TEXT("each once"), TSet<FString>(Ids).Num(), 18);
	TestTrue(TEXT("men's tall normal is one"), Ids.Contains(TEXT("m_tal_nrw")));
	TestEqual(TEXT("body words"), FMantleStandardBodyMatch::BodyWords(TEXT("m_tal_nrw")), FString(TEXT("men's tall normal-weight")));
	TestEqual(TEXT("an unknown id stays itself"), FMantleStandardBodyMatch::BodyWords(TEXT("x_y")), FString(TEXT("x_y")));

	// Joints in component space: a child's local offset goes through its parent's.
	TArray<FMantleGlbJoint> Joints;
	Joints.Add({ TEXT("root"), INDEX_NONE, FTransform(FVector(0, 0, 10)) });
	Joints.Add({ TEXT("spine_01"), 0, FTransform(FQuat(FVector::ZAxisVector, UE_HALF_PI), FVector(0, 0, 90)) });
	Joints.Add({ TEXT("clavicle_l"), 1, FTransform(FVector(5, 0, 0)) });
	const TMap<FName, FVector3f> Positions = FMantleStandardBodyMatch::JointPositions(Joints);
	TestTrue(TEXT("root"), Positions[TEXT("root")].Equals(FVector3f(0, 0, 10), 0.001f));
	TestTrue(TEXT("a child under its parent"), Positions[TEXT("spine_01")].Equals(FVector3f(0, 0, 100), 0.001f));
	TestTrue(TEXT("a child turned with its parent"), Positions[TEXT("clavicle_l")].Equals(FVector3f(0, 5, 100), 0.001f));

	// Skinned bones: any weight on a kept primitive's vertex; a zero weight and a dropped primitive don't count.
	FMantleGlbMesh Mesh;
	Mesh.Joints = Joints;
	Mesh.NumInfluences = 2;
	Mesh.Positions.SetNum(3);
	Mesh.InfluenceJoints = { 1, 2,  1, 2,  0, 1 };
	Mesh.InfluenceWeights = { 1.0f, 0.0f,  0.5f, 0.5f,  1.0f, 0.0f };
	Mesh.Primitives.Add({ 0, 0, 2, { 0, 1, 0 } });
	Mesh.Primitives.Add({ 1, 2, 1, { 0, 0, 0 } });
	const TSet<FName> All = FMantleStandardBodyMatch::SkinnedBones(Mesh);
	TestEqual(TEXT("three skinned bones"), All.Num(), 3);
	const TSet<int32> FirstOnly = { 0 };
	const TSet<FName> Kept = FMantleStandardBodyMatch::SkinnedBones(Mesh, &FirstOnly);
	TestTrue(TEXT("kept: spine"), Kept.Contains(TEXT("spine_01")));
	TestTrue(TEXT("kept: clavicle (its one real weight)"), Kept.Contains(TEXT("clavicle_l")));
	TestFalse(TEXT("root's weight is on the dropped primitive"), Kept.Contains(TEXT("root")));

	// Match: the garment's joints sit on body A's on its skinned bones; its feet (unskinned) are far off and don't count.
	const TMap<FName, FVector3f> Garment = { { TEXT("spine_01"), FVector3f(0, 0, 100) }, { TEXT("clavicle_l"), FVector3f(0, 5, 140) }, { TEXT("foot_l"), FVector3f(10, 0, 30) } };
	const TSet<FName> Shirt = { TEXT("spine_01"), TEXT("clavicle_l") };
	FMantleStandardBodyMatch::FCandidate BodyA{ TEXT("m_tal_nrw"), { { TEXT("spine_01"), FVector3f(0, 0, 100.5f) }, { TEXT("clavicle_l"), FVector3f(0, 5, 140.5f) }, { TEXT("foot_l"), FVector3f(10, 0, 8) } } };
	FMantleStandardBodyMatch::FCandidate BodyB{ TEXT("m_med_nrw"), { { TEXT("spine_01"), FVector3f(0, 0, 96) }, { TEXT("clavicle_l"), FVector3f(0, 5, 136) }, { TEXT("foot_l"), FVector3f(10, 0, 8) } } };
	const FMantleStandardBodyMatch::FCandidate Both[] = { BodyB, BodyA };
	const FMantleStandardBodyMatch::FResult Found = FMantleStandardBodyMatch::Match(Garment, Shirt, Both);
	TestTrue(TEXT("made on A"), Found.bMatch);
	TestEqual(TEXT("A is best"), Found.Best.Id, FString(TEXT("m_tal_nrw")));
	TestEqual(TEXT("scored on the two skinned bones only (feet ignored)"), Found.Best.Bones, 2);
	TestTrue(TEXT("A is 0.5 cm off"), FMath::IsNearlyEqual(Found.Best.MeanCm, 0.5f, 0.001f));
	TestEqual(TEXT("B is the runner-up"), Found.RunnerUp.Id, FString(TEXT("m_med_nrw")));
	TestTrue(TEXT("B is 4 cm off"), FMath::IsNearlyEqual(Found.RunnerUp.MeanCm, 4.0f, 0.001f));
	const FString Said = FMantleStandardBodyMatch::Describe(Found);
	TestTrue(TEXT("the note names the body"), Said.Contains(TEXT("men's tall normal-weight body (m_tal_nrw)")));
	TestTrue(TEXT("the note names the runner-up and its distance"), Said.Contains(TEXT("next closest m_med_nrw, 4.00 cm")));

	// A garment 4 cm off every body is not made on one; the note names the nearest and how far.
	const FMantleStandardBodyMatch::FCandidate OnlyB[] = { BodyB };
	const FMantleStandardBodyMatch::FResult Off = FMantleStandardBodyMatch::Match(Garment, Shirt, OnlyB);
	TestFalse(TEXT("4 cm off is not a match"), Off.bMatch);
	TestTrue(TEXT("the near miss names the nearest body"), FMantleStandardBodyMatch::Describe(Off).Contains(TEXT("the nearest is the men's medium normal-weight body (m_med_nrw), its joints 4.00 cm off")));
	TestEqual(TEXT("the check note's short reason"), FMantleStandardBodyMatch::ShortWhy(Off), FString(TEXT("not made on one of Epic's standard bodies: nearest m_med_nrw, joints 4.00 cm off")));
	TestTrue(TEXT("a match has no reason"), FMantleStandardBodyMatch::ShortWhy(Found).IsEmpty());

	// The skin confirm's search: exact near the body; with bCapped a vertex far from it records 12 cm instead of a full search.
	const TArray<FVector3f> Skin = { FVector3f(0, 0, 0), FVector3f(10, 0, 0) };
	const TArray<FVector3f> Cloth = { FVector3f(0, 0, 0.5f), FVector3f(10, 3, 0), FVector3f(100, 0, 0) };
	TArray<float> Near;
	TestTrue(TEXT("distances read"), FMantleFitBodies::NearestDistances(Cloth, Skin, Near, /*bCapped*/ true) && Near.Num() == 3);
	if (Near.Num() == 3)
	{
		TestTrue(TEXT("0.5 cm"), FMath::IsNearlyEqual(Near[0], 0.5f, 0.001f));
		TestTrue(TEXT("3 cm"), FMath::IsNearlyEqual(Near[1], 3.0f, 0.001f));
		TestTrue(TEXT("far records 12 cm"), FMath::IsNearlyEqual(Near[2], 12.0f, 0.001f));
	}
	TArray<float> Full;
	TestTrue(TEXT("uncapped reads the true far distance"), FMantleFitBodies::NearestDistances(Cloth, Skin, Full) && Full.Num() == 3 && FMath::IsNearlyEqual(Full[2], 90.0f, 0.001f));

	// The cut-off: Montana's shirt measured 1.07 cm on m_tal_nrw and 1.73 on m_tal_ovw (the 2026-11-25 proof, every shared bone).
	TestTrue(TEXT("1.07 cm is within the cut-off"), 1.07f <= FMantleStandardBodyMatch::JointMatchCm);
	TestTrue(TEXT("the cut-off is the one Joshua chose"), FMath::IsNearlyEqual(FMantleStandardBodyMatch::JointMatchCm, 1.5f));

	// A skinned bone the bodies don't have: not a standard body's skeleton, so nothing is scored.
	const TSet<FName> Odd = { TEXT("spine_01"), TEXT("cape_01") };
	const TMap<FName, FVector3f> Caped = { { TEXT("spine_01"), FVector3f(0, 0, 100) }, { TEXT("cape_01"), FVector3f(0, -10, 120) } };
	const FMantleStandardBodyMatch::FResult Foreign = FMantleStandardBodyMatch::Match(Caped, Odd, Both);
	TestFalse(TEXT("a bone no body has: no match"), Foreign.bMatch);
	TestEqual(TEXT("the missing bone is named"), Foreign.Missing.Num(), 1);
	TestTrue(TEXT("its note says so"), FMantleStandardBodyMatch::Describe(Foreign).Contains(TEXT("cape_01")));

	// Nothing skinned: nothing scored.
	const FMantleStandardBodyMatch::FResult Rigid = FMantleStandardBodyMatch::Match(Garment, TSet<FName>(), Both);
	TestFalse(TEXT("unskinned: no match"), Rigid.bMatch);
	TestTrue(TEXT("unskinned: no best"), Rigid.Best.MeanCm < 0.0f);
	return true;
}

// Montana's long-sleeve shirt (2026-11-24, made on m_tal_nrw, no body sent): it takes the standard-body route and is fitted to
// the men's bodies. Runs where the shirt is in the project (D:\UEProjects\MantleAvatars, /Game/Montana); skipped elsewhere.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleStandardBodyMatchMontanaShirtTest,
	"Mantle.Editor.StandardBodyMatch.MontanaShirt",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleStandardBodyMatchMontanaShirtTest::RunTest(const FString& Parameters)
{
	USkeletalMesh* Shirt = LoadObject<USkeletalMesh>(nullptr, TEXT("/Game/Montana/SKM_M_LSleeveShirt.SKM_M_LSleeveShirt"));
	if (!Shirt)
	{
		AddWarning(TEXT("SKM_M_LSleeveShirt is not in this project; skipped"));
		return true;
	}
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get();
	if (!TestNotNull(TEXT("the avatar contract"), Contract)) { return false; }
	const FMantleCosmeticDraft Draft = FMantleEditorActions::MakeCosmeticDraft(EMantleCosmeticClass::Garment, Shirt, TEXT("top"), FString(), *Contract, EMantleFitSex::Auto);
	FMantleCosmeticDraft Wrapped;
	TArray<TStrongObjectPtr<UObject>> KeepAlive;
	TSharedPtr<void> Lease;
	FString Why, Route, RouteId, Note;
	const bool bWrapped = FMantleParametricOutfit::PrepareWrapped(Draft, *Contract, Wrapped, KeepAlive, Lease, Why, &Route, &RouteId, &Note);
	AddInfo(FString::Printf(TEXT("route %s: %s"), *RouteId, *Note));
	if (!TestTrue(FString::Printf(TEXT("the shirt is made in sizes (%s)"), *Why), bWrapped)) { return false; }
	TestEqual(TEXT("made on Epic's standard men's tall normal-weight body"), RouteId, FString(TEXT("standard_body:m_tal_nrw")));
	TestTrue(TEXT("fitted to men's bodies"), Wrapped.FitSex == EMantleFitSex::Male);
	TestEqual(TEXT("three fit sizes"), Wrapped.OutfitTargets.Num(), 3);
	for (const FMantleOutfitAuthoringTarget& Target : Wrapped.OutfitTargets)
	{
		TestTrue(FString::Printf(TEXT("%s is a men's body"), *Target.Id), Target.Id.StartsWith(TEXT("m_")));
	}
	TestTrue(TEXT("the note names the runner-up"), Note.Contains(TEXT("next closest")));
	return true;
}

#endif
