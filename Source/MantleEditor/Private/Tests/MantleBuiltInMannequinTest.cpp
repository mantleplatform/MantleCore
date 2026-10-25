// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAvatarContract.h"
#include "MantleBuiltInMannequin.h"
#include "MantleCosmeticPreview.h"
#include "MantleSkinnedMesh.h"

#include "AnimationRuntime.h"
#include "Engine/SkeletalMesh.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// The avatar preview and emote upload no longer need SKM_UEFN_Mannequin in the project: the plugin builds its own UEFN
// mannequin from the skeleton its IK rig IK_UEFN_Mannequin stores.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleBuiltInMannequinMakeMeshTest, "Mantle.Cosmetics.AvatarPreview.BuiltInMannequin.MakeMesh",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMantleBuiltInMannequinMakeMeshTest::RunTest(const FString& Parameters)
{
	// root at the origin, pelvis 100 cm up and turned 90 degrees about Z, spine 10 cm above the pelvis.
	const FQuat Turn(FVector::UpVector, UE_HALF_PI);
	const TArray<FName> Bones = { TEXT("root"), TEXT("pelvis"), TEXT("spine_01") };
	const TArray<int32> Parents = { INDEX_NONE, 0, 1 };
	const TArray<FTransform> Global = {
		FTransform::Identity,
		FTransform(Turn, FVector(0.0, 0.0, 100.0)),
		FTransform(Turn, FVector(0.0, 0.0, 110.0)) };

	FMantleSkinnedMesh Mesh;
	FString Error;
	if (!TestTrue(TEXT("a parent-first skeleton makes a mesh"), FMantleBuiltInMannequin::MakeMesh(Bones, Parents, Global, Mesh, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("every bone is kept"), Mesh.Bones.Num(), 3);
	TestEqual(TEXT("parents are kept"), Mesh.Bones[2].ParentIndex, 1);
	TestTrue(TEXT("the pelvis is 100 cm above the root"), Mesh.ReferencePose[1].GetLocation().Equals(FVector(0.0, 0.0, 100.0), 1e-3));
	TestTrue(TEXT("the spine is 10 cm above the pelvis, local to it"), Mesh.ReferencePose[2].GetLocation().Equals(FVector(0.0, 0.0, 10.0), 1e-3));
	TestTrue(TEXT("the spine carries no turn of its own"), Mesh.ReferencePose[2].GetRotation().Equals(FQuat::Identity, 1e-4));
	TestEqual(TEXT("one triangle"), Mesh.LODs.Num() == 1 ? Mesh.LODs[0].NumTriangles() : 0, 1);

	FString Refused;
	TestFalse(TEXT("arrays of different lengths are refused"),
		FMantleBuiltInMannequin::MakeMesh(Bones, { INDEX_NONE, 0 }, Global, Mesh, Refused));
	TestFalse(TEXT("a child before its parent is refused"),
		FMantleBuiltInMannequin::MakeMesh(Bones, { INDEX_NONE, 2, 0 }, Global, Mesh, Refused));
	TestFalse(TEXT("an empty skeleton is refused"), FMantleBuiltInMannequin::MakeMesh({}, {}, {}, Mesh, Refused));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleBuiltInMannequinFromRigTest, "Mantle.Cosmetics.AvatarPreview.BuiltInMannequin.FromRig",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMantleBuiltInMannequinFromRigTest::RunTest(const FString& Parameters)
{
	FString ContractError;
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get(&ContractError);
	if (!TestNotNull(TEXT("the avatar contract loads"), Contract))
	{
		AddError(ContractError);
		return false;
	}
	FString Error;
	USkeletalMesh* Mesh = FMantleBuiltInMannequin::Get(&Error);
	if (!TestNotNull(TEXT("the plugin builds its UEFN mannequin from IK_UEFN_Mannequin"), Mesh))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("it is recognised as the built-in one"), FMantleBuiltInMannequin::Is(Mesh));
	TestTrue(TEXT("it is built once"), FMantleBuiltInMannequin::Get() == Mesh);
	TestNotNull(TEXT("it has a skeleton"), Mesh->GetSkeleton());
	const FSkeletalMeshRenderData* Render = Mesh->GetResourceForRendering();
	const FReferenceSkeleton& Reference = Mesh->GetRefSkeleton();
	TestTrue(TEXT("it has render data that animates every bone"), Render && Render->LODRenderData.Num() == 1
		&& Render->LODRenderData[0].RequiredBones.Num() == Reference.GetRawBoneNum());

	// Every bone of the uefn fixture the retargeters can drive; only the prop bones (and virtual bones) may be missing.
	const FMantleContractSkeleton* Uefn = Contract->FindSkeleton(Contract->EmoteSkeleton);
	if (TestNotNull(TEXT("the contract has the uefn fixture"), Uefn))
	{
		const TSet<FName> MayLack = { TEXT("props_root"), TEXT("prop_01"), TEXT("poi") };
		for (const FName Bone : Uefn->Bones)
		{
			if (!Bone.ToString().StartsWith(TEXT("VB "), ESearchCase::CaseSensitive) && !MayLack.Contains(Bone))
			{
				TestTrue(FString::Printf(TEXT("the built mannequin has %s"), *Bone.ToString()), Reference.FindRawBoneIndex(Bone) != INDEX_NONE);
			}
		}
	}

	// Its reference pose is the UEFN mannequin's A-pose the contract describes (within the cook's own tolerance).
	const FMantleContractSkeleton* Avatar = Contract->FindSkeleton(TEXT("uefnmannequin"));
	if (TestTrue(TEXT("the contract describes the UEFN mannequin's A-pose"), Avatar && Avatar->HasReferencePose()))
	{
		for (const FMantleContractReferenceChain& Chain : Avatar->ReferenceChains)
		{
			const int32 From = Reference.FindRawBoneIndex(FName(*Chain.From));
			const int32 To = Reference.FindRawBoneIndex(FName(*Chain.To));
			if (!TestTrue(FString::Printf(TEXT("the built mannequin has %s and %s"), *Chain.From, *Chain.To), From != INDEX_NONE && To != INDEX_NONE))
			{
				continue;
			}
			const FVector Direction = (FAnimationRuntime::GetComponentSpaceTransformRefPose(Reference, To).GetLocation()
				- FAnimationRuntime::GetComponentSpaceTransformRefPose(Reference, From).GetLocation()).GetSafeNormal();
			const double Degrees = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Direction | Chain.Direction.GetSafeNormal(), -1.0, 1.0)));
			TestTrue(FString::Printf(TEXT("%s to %s points as the A-pose's (%.1f degrees off)"), *Chain.From, *Chain.To, Degrees),
				Degrees <= Contract->ChainToleranceDegrees);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleBuiltInMannequinPreviewTest, "Mantle.Cosmetics.AvatarPreview.BuiltInMannequin.Preview",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMantleBuiltInMannequinPreviewTest::RunTest(const FString& Parameters)
{
	FMantleAvatarPreviewPlan Plan;
	FString Error;
	if (!TestTrue(TEXT("a UEFN Mannequin avatar has a plan"), FMantleAvatarPreviewPlan::ForSkeleton(TEXT("uefnmannequin"), false, Plan, Error)))
	{
		AddError(Error);
		return false;
	}
	USkeletalMesh* Driver = FMantleBuiltInMannequin::Get(&Error);
	if (!TestNotNull(TEXT("the built-in mannequin is made"), Driver))
	{
		AddError(Error);
		return false;
	}

	// No idle file: the built-in driver alone poses the avatar, in the mannequin's A-pose.
	const FMantleAvatarPreviewInputs Inputs = FMantleAvatarPreviewInputs::Gather(Plan, Driver, FString());
	TestEqual(TEXT("nothing is missing"), FString::Join(Inputs.Missing, TEXT(", ")), FString());
	TestTrue(TEXT("the built-in mannequin is the driver"), Inputs.Driver == Driver);
	TestNull(TEXT("no project idle is looked for on it"), Inputs.IdleAsset);
	TestTrue(TEXT("a note says the pose is the plugin's own mannequin's"), Inputs.Notes.ContainsByPredicate([](const FString& Note)
	{
		return Note.Contains(TEXT("Mantle plugin's own UEFN mannequin"));
	}));
	TestTrue(TEXT("the status says the avatar is posed"), Inputs.Status(Plan, true, true).StartsWith(TEXT("Your uefnmannequin avatar, posed by")));

	// No driver at all: the reason the built-in one failed is passed on.
	const FMantleAvatarPreviewInputs None = FMantleAvatarPreviewInputs::Gather(Plan, nullptr, FString(), TEXT("the rig is gone"));
	TestTrue(TEXT("a missing driver names why"), None.Missing.Num() == 1 && None.Missing[0].Contains(TEXT("the rig is gone")));
	return true;
}

#endif
