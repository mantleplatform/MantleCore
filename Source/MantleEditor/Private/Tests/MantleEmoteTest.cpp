// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAnimClip.h"
#include "MantleAnimClipWriter.h"
#include "MantleAvatarContract.h"
#include "MantleBuiltInMannequin.h"
#include "MantleCosmeticValidation.h"
#include "MantleEmoteCheck.h"
#include "MantleEmoteRetarget.h"

#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimSequence.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"
#include "ReferenceSkeleton.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

#if WITH_DEV_AUTOMATION_TESTS

// Emotes end to end in the plugin: the MAN1 rules the cook applies, the bytes players' clients read back, which contract
// skeleton a clip is on, and IK Rig's batch retarget the upload runs.
namespace MantleEmoteTestPrivate
{
	const FMantleAvatarContract* Contract(FAutomationTestBase& Test)
	{
		FString Error;
		const FMantleAvatarContract* Out = FMantleAvatarContract::Get(&Error);
		if (!Out)
		{
			Test.AddError(FString::Printf(TEXT("the avatar contract does not load: %s"), *Error));
		}
		return Out;
	}

	/**
	 * A one-second clip (31 frames) on the uefn skeleton: the root still, the pelvis bobbing 4 cm and turning 30 degrees, the
	 * left upper arm raising 60 degrees, the head constant, and a footstep event.
	 */
	FMantleAnimClipData MakeClip(const FMantleAvatarContract& Contract)
	{
		const FMantleContractSkeleton* Skeleton = Contract.FindSkeleton(Contract.EmoteSkeleton);
		FMantleAnimClipData Clip;
		Clip.FrameRate = Contract.EmoteFrameRate;
		Clip.FrameCount = 31;
		// Out of skeleton order on purpose: the writer sorts them.
		FMantleAnimClipTrackData& Arm = Clip.Tracks.AddDefaulted_GetRef();
		Arm.Bone = TEXT("upperarm_l");
		FMantleAnimClipTrackData& Pelvis = Clip.Tracks.AddDefaulted_GetRef();
		Pelvis.Bone = TEXT("pelvis");
		FMantleAnimClipTrackData& Root = Clip.Tracks.AddDefaulted_GetRef();
		Root.Bone = Skeleton ? Skeleton->Root() : FName(TEXT("root"));
		Root.Rotations.Add(FQuat4f::Identity);
		Root.Translations.Add(FVector3f::ZeroVector);
		FMantleAnimClipTrackData& Head = Clip.Tracks.AddDefaulted_GetRef();
		Head.Bone = TEXT("head");
		for (int32 Frame = 0; Frame < Clip.FrameCount; ++Frame)
		{
			const float Alpha = static_cast<float>(Frame) / (Clip.FrameCount - 1);
			Pelvis.Rotations.Add(FQuat4f(FVector3f::UpVector, FMath::DegreesToRadians(30.0f * Alpha)));
			Pelvis.Translations.Add(FVector3f(0.0f, 0.0f, 95.0f + 4.0f * FMath::Sin(Alpha * UE_TWO_PI)));
			Arm.Rotations.Add(FQuat4f(FVector3f::ForwardVector, FMath::DegreesToRadians(60.0f * Alpha)));
			Head.Rotations.Add(FQuat4f(FVector3f::RightVector, 0.1f));
		}
		FMantleAnimClipEventData& Step = Clip.Events.AddDefaulted_GetRef();
		Step.Name = TEXT("footstep_l");
		Step.TimeSeconds = 0.5f;
		return Clip;
	}

	bool HasError(const TArray<FMantleCosmeticIssue>& Issues, const TCHAR* Text)
	{
		return Issues.ContainsByPredicate([Text](const FMantleCosmeticIssue& Issue)
		{
			return Issue.Severity == EMantleCosmeticSeverity::Error && Issue.Message.Contains(Text);
		});
	}

	TArray<FMantleCosmeticIssue> ValidateWith(const FMantleAvatarContract& Contract, TFunctionRef<void(FMantleAnimClipData&)> Change)
	{
		FMantleAnimClipData Clip = MakeClip(Contract);
		Change(Clip);
		TArray<FMantleCosmeticIssue> Issues;
		FMantleAnimClipWriter::Validate(Clip, Contract, Issues);
		return Issues;
	}

	FMantleAnimClipTrackData* Find(FMantleAnimClipData& Clip, const TCHAR* Bone)
	{
		return Clip.Tracks.FindByPredicate([Bone](const FMantleAnimClipTrackData& Track) { return Track.Bone == FName(Bone); });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleEmoteValidateTest, "Mantle.Editor.Emote.Validate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMantleEmoteValidateTest::RunTest(const FString& Parameters)
{
	using namespace MantleEmoteTestPrivate;
	const FMantleAvatarContract* C = Contract(*this);
	if (!C)
	{
		return false;
	}
	const FMantleAvatarContract& K = *C;
	{
		TArray<FMantleCosmeticIssue> Issues = ValidateWith(K, [](FMantleAnimClipData&) {});
		TestFalse(FString::Printf(TEXT("a good clip passes (%s)"), Issues.Num() > 0 ? *Issues[0].ToString() : TEXT("")), FMantleCosmeticChecks::HasErrors(Issues));
	}
	TestTrue(TEXT("another frame rate is refused"), HasError(ValidateWith(K, [](FMantleAnimClipData& Clip) { Clip.FrameRate = 24.0f; }), TEXT("frame rate")));
	TestTrue(TEXT("no frames is refused"), HasError(ValidateWith(K, [](FMantleAnimClipData& Clip) { Clip.FrameCount = 0; }), TEXT("no frames")));
	TestTrue(TEXT("a clip over the length limit is refused"), HasError(ValidateWith(K, [&K](FMantleAnimClipData& Clip)
	{
		Clip.FrameCount = FMath::RoundToInt(K.EmoteMaxSeconds * K.EmoteFrameRate) + 2;
		Clip.Tracks.SetNum(1);
		Clip.Tracks[0].Rotations.SetNum(1);
	}), TEXT("the limit is")));
	{
		// Exactly the limit is allowed.
		TArray<FMantleCosmeticIssue> Issues = ValidateWith(K, [&K](FMantleAnimClipData& Clip)
		{
			Clip.FrameCount = FMath::RoundToInt(K.EmoteMaxSeconds * K.EmoteFrameRate) + 1;
			for (FMantleAnimClipTrackData& Track : Clip.Tracks)
			{
				Track.Rotations.SetNum(1);
				Track.Translations.SetNum(FMath::Min(1, Track.Translations.Num()));
			}
		});
		TestFalse(TEXT("a clip of exactly the length limit passes"), HasError(Issues, TEXT("long")));
	}
	TestTrue(TEXT("a root that moves past the limit is refused"), HasError(ValidateWith(K, [&K](FMantleAnimClipData& Clip)
	{
		FMantleAnimClipTrackData* Root = Find(Clip, TEXT("root"));
		Root->Translations.SetNum(Clip.FrameCount);
		for (int32 Frame = 0; Frame < Clip.FrameCount; ++Frame)
		{
			Root->Translations[Frame] = FVector3f((K.EmoteMaxRootCm + 10.0f) * Frame / (Clip.FrameCount - 1), 0.0f, 0.0f);
		}
	}), TEXT("the root moves")));
	TestFalse(TEXT("a root that moves within the limit passes"), HasError(ValidateWith(K, [&K](FMantleAnimClipData& Clip)
	{
		FMantleAnimClipTrackData* Root = Find(Clip, TEXT("root"));
		Root->Translations.SetNum(Clip.FrameCount);
		for (int32 Frame = 0; Frame < Clip.FrameCount; ++Frame)
		{
			Root->Translations[Frame] = FVector3f((K.EmoteMaxRootCm - 10.0f) * Frame / (Clip.FrameCount - 1), 0.0f, 0.0f);
		}
	}), TEXT("the root moves")));
	TestTrue(TEXT("a bone the uefn skeleton lacks is refused"), HasError(ValidateWith(K, [](FMantleAnimClipData& Clip)
	{
		Clip.Tracks.Last().Bone = TEXT("FACIAL_C_Jaw");
	}), TEXT("is not a bone of the")));
	TestTrue(TEXT("two tracks on one bone are refused"), HasError(ValidateWith(K, [](FMantleAnimClipData& Clip)
	{
		const FMantleAnimClipTrackData Again = Clip.Tracks[0];
		Clip.Tracks.Add(Again);
	}), TEXT("has two tracks")));
	TestTrue(TEXT("a clip without a pelvis track is refused"), HasError(ValidateWith(K, [](FMantleAnimClipData& Clip)
	{
		Clip.Tracks.RemoveAll([](const FMantleAnimClipTrackData& Track) { return Track.Bone == FName(TEXT("pelvis")); });
	}), TEXT("no \"pelvis\" track")));
	TestTrue(TEXT("a key count that is neither 1 nor the frame count is refused"), HasError(ValidateWith(K, [](FMantleAnimClipData& Clip)
	{
		Find(Clip, TEXT("pelvis"))->Rotations.SetNum(5);
	}), TEXT("rotation keys")));
	TestTrue(TEXT("a rotation that is not a unit quaternion is refused"), HasError(ValidateWith(K, [](FMantleAnimClipData& Clip)
	{
		Find(Clip, TEXT("pelvis"))->Rotations[3] = FQuat4f(0.5f, 0.0f, 0.0f, 0.5f);
	}), TEXT("unit quaternion")));
	TestTrue(TEXT("a translation out of range is refused"), HasError(ValidateWith(K, [](FMantleAnimClipData& Clip)
	{
		Find(Clip, TEXT("pelvis"))->Translations[3] = FVector3f(0.0f, 0.0f, 20000.0f);
	}), TEXT("translation of")));
	TestTrue(TEXT("an event outside the allowlist is refused"), HasError(ValidateWith(K, [](FMantleAnimClipData& Clip)
	{
		Clip.Events[0].Name = TEXT("PlaySound");
	}), TEXT("not one of the allowed events")));
	TestTrue(TEXT("an event after the clip's end is refused"), HasError(ValidateWith(K, [](FMantleAnimClipData& Clip)
	{
		Clip.Events[0].TimeSeconds = 5.0f;
	}), TEXT("outside the clip")));
	TestTrue(TEXT("more events than the limit are refused"), HasError(ValidateWith(K, [&K](FMantleAnimClipData& Clip)
	{
		const FMantleAnimClipEventData Again = Clip.Events[0];
		for (int32 Index = 0; Index < K.EmoteMaxEvents; ++Index)
		{
			Clip.Events.Add(Again);
		}
	}), TEXT("events, the limit")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleEmoteRoundTripTest, "Mantle.Editor.Emote.RoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMantleEmoteRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace MantleEmoteTestPrivate;
	const FMantleAvatarContract* C = Contract(*this);
	const FMantleContractSkeleton* Skeleton = C ? C->FindSkeleton(C->EmoteSkeleton) : nullptr;
	if (!TestNotNull(TEXT("the contract has the emote skeleton"), Skeleton))
	{
		return false;
	}
	const FMantleAnimClipData Clip = MakeClip(*C);
	const TArray<uint8> Bytes = FMantleAnimClipWriter::Encode(Clip, Skeleton);
	TestEqual(TEXT("encoding is deterministic"), FMantleAnimClipWriter::Encode(Clip, Skeleton), Bytes);
	TestTrue(TEXT("the clip is within the size limit"), Bytes.Num() > 0 && Bytes.Num() <= C->EmoteMaxBytes);

	// Read back as players' clients read it.
	const FMantleAnimClipReadResult Read = FMantleAnimClipReader::Read(Bytes);
	if (!TestTrue(FString::Printf(TEXT("the client reads it (%s)"), *Read.Error), Read.bSuccess))
	{
		return false;
	}
	const FMantleAnimClip& Back = Read.Clip;
	TestEqual(TEXT("frame rate"), Back.FrameRate, Clip.FrameRate);
	TestEqual(TEXT("frame count"), Back.FrameCount, Clip.FrameCount);
	TestFalse(TEXT("an emote carries no skeleton block"), Back.HasSkeleton());
	if (!TestEqual(TEXT("every track"), Back.Tracks.Num(), Clip.Tracks.Num()))
	{
		return false;
	}
	for (int32 Index = 1; Index < Back.Tracks.Num(); ++Index)
	{
		TestTrue(FString::Printf(TEXT("tracks in skeleton order (%s before %s)"), *Back.Tracks[Index - 1].Bone.ToString(), *Back.Tracks[Index].Bone.ToString()),
			Skeleton->Find(Back.Tracks[Index - 1].Bone) < Skeleton->Find(Back.Tracks[Index].Bone));
	}
	for (const FMantleAnimClipTrack& Track : Back.Tracks)
	{
		const FMantleAnimClipTrackData* Source = Clip.Tracks.FindByPredicate([&Track](const FMantleAnimClipTrackData& Each) { return Each.Bone == Track.Bone; });
		if (!TestNotNull(FString::Printf(TEXT("%s came from the clip"), *Track.Bone.ToString()), Source))
		{
			continue;
		}
		bool bConstant = true;
		for (const FQuat4f& Q : Source->Rotations)
		{
			bConstant = bConstant && Q == Source->Rotations[0];
		}
		TestEqual(FString::Printf(TEXT("%s: a constant track is one key, an animated one every frame"), *Track.Bone.ToString()), Track.Rotations.Num(),
			bConstant ? 1 : Clip.FrameCount);
		TestEqual(FString::Printf(TEXT("%s: translations kept as given"), *Track.Bone.ToString()), Track.HasTranslation(), Source->Translations.Num() > 0);
		for (int32 Frame = 0; Frame < Clip.FrameCount; Frame += 5)
		{
			FQuat4f Rotation;
			FVector3f Translation;
			bool bHasTranslation = false;
			Back.SampleTrack(Back.Tracks.IndexOfByPredicate([&Track](const FMantleAnimClipTrack& Each) { return Each.Bone == Track.Bone; }),
				Frame / Clip.FrameRate, Rotation, Translation, bHasTranslation);
			const FQuat4f Want = Source->Rotations[FMath::Min(Frame, Source->Rotations.Num() - 1)];
			TestTrue(FString::Printf(TEXT("%s frame %d: rotation within the 16-bit step (%.5f)"), *Track.Bone.ToString(), Frame, FMath::Abs(Rotation | Want)),
				FMath::Abs(Rotation | Want) > 1.0f - 1e-6f);
			if (bHasTranslation)
			{
				const FVector3f WantT = Source->Translations[FMath::Min(Frame, Source->Translations.Num() - 1)];
				TestTrue(FString::Printf(TEXT("%s frame %d: translation within a hundredth of a cm"), *Track.Bone.ToString(), Frame),
					FVector3f::Distance(Translation, WantT) < 0.01f);
			}
		}
	}
	if (TestEqual(TEXT("the event"), Back.Events.Num(), 1))
	{
		TestEqual(TEXT("event name"), Back.Events[0].Name, FName(TEXT("footstep_l")));
		TestEqual(TEXT("event time"), Back.Events[0].Time, 0.5f);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleEmoteIdentifySkeletonTest, "Mantle.Editor.Emote.IdentifySkeleton",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMantleEmoteIdentifySkeletonTest::RunTest(const FString& Parameters)
{
	using namespace MantleEmoteTestPrivate;
	const FMantleAvatarContract* C = Contract(*this);
	if (!C)
	{
		return false;
	}
	auto JointsOf = [](const FMantleContractSkeleton& Fixture)
	{
		TArray<FMantleSkinJointName> Joints;
		for (int32 Bone = 0; Bone < Fixture.Bones.Num(); ++Bone)
		{
			Joints.Add({ Fixture.Bones[Bone], Fixture.Parents[Bone] });
		}
		return Joints;
	};
	// Each skeleton an emote is accepted on is told apart from the others, by its whole bone table.
	for (const FString& Id : { C->EmoteSkeleton, FString(TEXT("metahuman")), FString(TEXT("ue5mannequin")), FString(TEXT("ue4mannequin")) })
	{
		const FMantleContractSkeleton* Fixture = C->FindSkeleton(Id);
		if (TestNotNull(FString::Printf(TEXT("the contract has %s"), *Id), Fixture))
		{
			TestEqual(FString::Printf(TEXT("the %s skeleton is identified as itself"), *Id), FMantleEmoteRetarget::IdentifyJoints(JointsOf(*Fixture), *C), Id);
		}
	}
	// A clip only uses the bones it animates' skeleton: a UE5 Mannequin skeleton without its corrective and IK bones is still UE5.
	if (const FMantleContractSkeleton* UE5 = C->FindSkeleton(TEXT("ue5mannequin")))
	{
		TArray<FMantleSkinJointName> Joints;
		TArray<int32> NewIndex;
		for (int32 Bone = 0; Bone < UE5->Bones.Num(); ++Bone)
		{
			const FString Name = UE5->Bones[Bone].ToString();
			const int32 Parent = UE5->Parents[Bone];
			if (Name.StartsWith(TEXT("ik_")) || Name.Contains(TEXT("_corrective")) || (Parent != INDEX_NONE && NewIndex[Parent] == INDEX_NONE))
			{
				NewIndex.Add(INDEX_NONE);
				continue;
			}
			NewIndex.Add(Joints.Num());
			Joints.Add({ UE5->Bones[Bone], Parent == INDEX_NONE ? INDEX_NONE : NewIndex[Parent] });
		}
		TestEqual(TEXT("the UE5 Mannequin's deforming bones are identified as ue5mannequin"), FMantleEmoteRetarget::IdentifyJoints(Joints, *C), FString(TEXT("ue5mannequin")));
	}
	// A skeleton with bones of its own on top of one of them (a tail, a weapon bone) is read as that one; its own bones stay behind.
	if (const FMantleContractSkeleton* UE4 = C->FindSkeleton(TEXT("ue4mannequin")))
	{
		TArray<FMantleSkinJointName> Joints = JointsOf(*UE4);
		Joints.Add({ FName(TEXT("tail_01")), 0 });
		FString Why;
		TestEqual(TEXT("the UE4 Mannequin with a tail is the UE4 Mannequin"), FMantleEmoteRetarget::IdentifyJoints(Joints, *C, &Why), FString(TEXT("ue4mannequin")));
		TestTrue(FString::Printf(TEXT("the note names the extra bone (%s)"), *Why), Why.Contains(TEXT("tail_01")));
	}
	if (const FMantleContractSkeleton* UE5 = C->FindSkeleton(TEXT("ue5mannequin")))
	{
		TArray<FMantleSkinJointName> Joints = JointsOf(*UE5);
		const int32 Hand = UE5->Find(TEXT("hand_r"));
		Joints.Add({ FName(TEXT("weapon_r")), Hand });
		TestEqual(TEXT("the UE5 Mannequin with a weapon bone is the UE5 Mannequin, not the UEFN mannequin it shares its body bones with"),
			FMantleEmoteRetarget::IdentifyJoints(Joints, *C), FString(TEXT("ue5mannequin")));
	}
	// A skeleton of its own (another naming) is none of them, and the reason names the nearest and the first difference.
	{
		const TArray<FMantleSkinJointName> Joints = { { FName(TEXT("Hips")), INDEX_NONE }, { FName(TEXT("Spine")), 0 }, { FName(TEXT("Head")), 1 } };
		FString Why;
		TestEqual(TEXT("another rig is no contract skeleton"), FMantleEmoteRetarget::IdentifyJoints(Joints, *C, &Why), FString());
		TestTrue(FString::Printf(TEXT("the reason names the nearest skeleton (%s)"), *Why), Why.Contains(TEXT("nearest")));
	}
	TestFalse(TEXT("an IK bone is no body bone"), FMantleEmoteRetarget::IsBodyBone(TEXT("ik_hand_gun")));
	TestTrue(TEXT("the pelvis is a body bone"), FMantleEmoteRetarget::IsBodyBone(TEXT("pelvis")));
	// The plugin's own UEFN mannequin is the emote skeleton: its clips need no retarget.
	FString Error;
	if (USkeletalMesh* Mannequin = FMantleBuiltInMannequin::Get(&Error))
	{
		TestEqual(TEXT("the built-in UEFN mannequin is the emote skeleton"), FMantleEmoteRetarget::IdentifySkeleton(Mannequin->GetSkeleton(), *C), C->EmoteSkeleton);
	}
	else
	{
		AddError(Error);
	}
	TestEqual(TEXT("no skeleton is none"), FMantleEmoteRetarget::IdentifySkeleton(nullptr, *C), FString());
	TestNull(TEXT("no retargeter for the emote skeleton itself"), FMantleEmoteRetarget::RetargeterPathFor(C->EmoteSkeleton));
	for (const TCHAR* Id : { TEXT("metahuman"), TEXT("ue5mannequin"), TEXT("ue4mannequin") })
	{
		const TCHAR* Path = FMantleEmoteRetarget::RetargeterPathFor(Id);
		TestTrue(FString::Printf(TEXT("the plugin ships a retargeter from %s"), Id), Path && FSoftObjectPath(Path).TryLoad() != nullptr);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleEmoteRetargetSmokeTest, "Mantle.Editor.Emote.Retarget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMantleEmoteRetargetSmokeTest::RunTest(const FString& Parameters)
{
	using namespace MantleEmoteTestPrivate;
	const FMantleAvatarContract* C = Contract(*this);
	FString Error;
	USkeletalMesh* Mannequin = FMantleBuiltInMannequin::Get(&Error);
	if (!C || !TestNotNull(FString::Printf(TEXT("the built-in UEFN mannequin is made (%s)"), *Error), Mannequin))
	{
		return false;
	}
	// The UEFN-to-UEFN Mannequin retargeter maps every chain of one IK rig onto itself: what goes in comes out.
	UObject* Retargeter = FSoftObjectPath(TEXT("/MantleCore/Avatars/Retarget/RTG_UEFN_to_Mantle_UEFNMannequin.RTG_UEFN_to_Mantle_UEFNMannequin")).TryLoad();
	if (!TestNotNull(TEXT("the plugin's UEFN Mannequin retargeter loads"), Retargeter))
	{
		return false;
	}

	// A one-second clip made in memory: the pelvis turns 40 degrees, the left upper arm raises 50.
	UPackage* Package = CreatePackage(TEXT("/Game/__MantleEmoteTest/AS_EmoteRetargetSmoke"));
	UAnimSequence* Sequence = NewObject<UAnimSequence>(Package, TEXT("AS_EmoteRetargetSmoke"), RF_Public | RF_Standalone | RF_Transient);
	Sequence->SetSkeleton(Mannequin->GetSkeleton());
	Sequence->SetPreviewMesh(Mannequin);
	const FReferenceSkeleton& Reference = Mannequin->GetRefSkeleton();
	const int32 Frames = 30;
	{
		IAnimationDataController& Controller = Sequence->GetController();
		Controller.InitializeModel();
		Controller.OpenBracket(FText::FromString(TEXT("Emote retarget test")), false);
		Controller.SetFrameRate(FFrameRate(30, 1), false);
		Controller.SetNumberOfFrames(FFrameNumber(Frames), false);
		for (const TCHAR* Bone : { TEXT("pelvis"), TEXT("upperarm_l") })
		{
			const int32 Index = Reference.FindRawBoneIndex(FName(Bone));
			if (!TestTrue(FString::Printf(TEXT("the mannequin has %s"), Bone), Index != INDEX_NONE))
			{
				return false;
			}
			const FTransform Ref = Reference.GetRawRefBonePose()[Index];
			const FVector Axis = FCString::Strcmp(Bone, TEXT("pelvis")) == 0 ? FVector::UpVector : FVector::ForwardVector;
			const double Degrees = FCString::Strcmp(Bone, TEXT("pelvis")) == 0 ? 40.0 : 50.0;
			TArray<FVector> Positions;
			TArray<FQuat> Rotations;
			TArray<FVector> Scales;
			for (int32 Key = 0; Key <= Frames; ++Key)
			{
				Positions.Add(Ref.GetTranslation());
				Rotations.Add(FQuat(Axis, FMath::DegreesToRadians(Degrees * Key / Frames)) * Ref.GetRotation());
				Scales.Add(FVector::OneVector);
			}
			Controller.AddBoneCurve(FName(Bone), false);
			Controller.SetBoneTrackKeys(FName(Bone), Positions, Rotations, Scales, false);
		}
		Controller.NotifyPopulated();
		Controller.CloseBracket(false);
	}

	FMantleAnimClipData Clip;
	FMantleAnimClipData Original;
	FString SampleError;
	TestTrue(TEXT("the clip itself samples"), FMantleAnimClipWriter::Sample(Sequence, *C, Original, SampleError));
	const bool bRetargeted = FMantleEmoteRetarget::RetargetThrough(Sequence, Mannequin, Mannequin, Retargeter, 1.0 / 30.0,
		[&](UAnimSequence& Retargeted, FString& OutUseError)
		{
			TestTrue(TEXT("the copy is made in the transient folder"), Retargeted.GetPackage()->GetName().StartsWith(FString(FMantleEmoteRetarget::TransientFolder) + TEXT("/")));
			return FMantleAnimClipWriter::Sample(&Retargeted, *C, Clip, OutUseError);
		}, Error);
	Sequence->ClearFlags(RF_Public | RF_Standalone);
	Sequence->MarkAsGarbage();
	if (!TestTrue(FString::Printf(TEXT("IK Rig's batch retarget runs (%s)"), *Error), bRetargeted))
	{
		return false;
	}
	TestEqual(TEXT("the retargeted clip is as long"), Clip.FrameCount, Original.FrameCount);
	TArray<FMantleCosmeticIssue> Issues;
	FMantleAnimClipWriter::Validate(Clip, *C, Issues);
	TestFalse(FString::Printf(TEXT("the retargeted clip is a valid emote (%s)"), Issues.Num() > 0 ? *Issues[0].ToString() : TEXT("")), FMantleCosmeticChecks::HasErrors(Issues));
	for (const TCHAR* Bone : { TEXT("pelvis"), TEXT("upperarm_l") })
	{
		const FMantleAnimClipTrackData* Got = Clip.Tracks.FindByPredicate([Bone](const FMantleAnimClipTrackData& Track) { return Track.Bone == FName(Bone); });
		const FMantleAnimClipTrackData* Want = Original.Tracks.FindByPredicate([Bone](const FMantleAnimClipTrackData& Track) { return Track.Bone == FName(Bone); });
		if (!TestTrue(FString::Printf(TEXT("%s has a track before and after"), Bone), Got && Want && Got->Rotations.Num() > 1 && Want->Rotations.Num() > 1))
		{
			continue;
		}
		const FQuat4f& A = Got->Rotations.Last();
		const FQuat4f& B = Want->Rotations.Last();
		const float Degrees = FMath::RadiansToDegrees(FQuat4f(A).AngularDistance(B));
		TestTrue(FString::Printf(TEXT("%s ends where the source does (%.2f degrees apart)"), Bone, Degrees), Degrees < 2.0f);
	}
	// Nothing the retarget made is left behind.
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	TArray<FAssetData> Left;
	Registry.GetAssetsByPath(FName(FMantleEmoteRetarget::TransientFolder), Left, true);
	TestEqual(TEXT("the transient folder is empty again"), Left.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleEmoteCheckSheetTest, "Mantle.Editor.Emote.CheckSheet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMantleEmoteCheckSheetTest::RunTest(const FString& Parameters)
{
	// The sheet's moments run from the start to the end.
	const TArray<float> Times = FMantleEmoteCheck::Moments(2.5f, 6);
	TestEqual(TEXT("six moments"), Times.Num(), 6);
	TestEqual(TEXT("the first is the start"), Times[0], 0.0f);
	TestEqual(TEXT("the last is the end"), Times.Last(), 2.5f);
	TestEqual(TEXT("a one-frame clip shows one moment"), FMantleEmoteCheck::Moments(0.0f, 6).Num(), 1);

	// Tiles are laid out left to right, then down.
	TArray<TArray<FColor>> Tiles;
	for (const FColor Color : { FColor::Red, FColor::Green, FColor::Blue, FColor::White })
	{
		TArray<FColor>& Tile = Tiles.AddDefaulted_GetRef();
		Tile.Init(Color, 4 * 4);
	}
	int32 Width = 0;
	int32 Height = 0;
	const TArray<FColor> Sheet = FMantleEmoteCheck::Tile(Tiles, 4, 3, Width, Height);
	TestEqual(TEXT("three tiles across"), Width, 12);
	TestEqual(TEXT("two rows"), Height, 8);
	TestEqual(TEXT("the second tile is beside the first"), Sheet[5], FColor::Green);
	TestEqual(TEXT("the fourth starts the second row"), Sheet[4 * Width + 1], FColor::White);
	TestEqual(TEXT("an empty place stays black"), Sheet[5 * Width + 9], FColor::Black);

	// How far an emote takes the body: a pelvis that walks 80 cm forward drifts 80 cm, whatever the root does.
	FMantleAnimClip Clip;
	Clip.FrameRate = 30.0f;
	Clip.FrameCount = 31;
	FMantleAnimClipTrack& Pelvis = Clip.Tracks.AddDefaulted_GetRef();
	Pelvis.Bone = TEXT("pelvis");
	FMantleAnimClipTrack& Root = Clip.Tracks.AddDefaulted_GetRef();
	Root.Bone = TEXT("root");
	for (int32 Frame = 0; Frame < Clip.FrameCount; ++Frame)
	{
		Pelvis.Rotations.Add(FQuat4f::Identity);
		Pelvis.Translations.Add(FVector3f(0.0f, 80.0f * Frame / 30.0f, 95.0f));
		Root.Rotations.Add(FQuat4f(FVector3f::UpVector, FMath::DegreesToRadians(90.0f * Frame / 30.0f)));
		Root.Translations.Add(FVector3f(20.0f * Frame / 30.0f, 0.0f, 0.0f));
	}
	FReferenceSkeleton Skeleton;
	{
		FReferenceSkeletonModifier Modifier(Skeleton, nullptr);
		Modifier.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity);
		Modifier.Add(FMeshBoneInfo(TEXT("pelvis"), TEXT("pelvis"), 0), FTransform(FVector(0.0, 0.0, 95.0)));
	}
	const FMantleEmoteMotion Motion = FMantleEmoteCheck::Measure(Clip, Skeleton);
	TestTrue(FString::Printf(TEXT("the pelvis drifts 80 cm (%.1f)"), Motion.PelvisDriftCm), FMath::IsNearlyEqual(Motion.PelvisDriftCm, 80.0, 0.5));
	TestTrue(FString::Printf(TEXT("the root travels 20 cm (%.1f)"), Motion.RootTravelCm), FMath::IsNearlyEqual(Motion.RootTravelCm, 20.0, 0.5));
	TestTrue(FString::Printf(TEXT("the root turns 90 degrees (%.1f)"), Motion.RootTurnDegrees), FMath::IsNearlyEqual(Motion.RootTurnDegrees, 90.0, 0.5));
	return true;
}

namespace MantleEmoteTestPrivate
{
	/** 31 frames: the root walks Travel cm along x, bobs 10 cm up and turns 90 degrees; the pelvis stands 95 cm up, still. */
	FMantleAnimClipData RootMotionClip(const float Travel)
	{
		FMantleAnimClipData Clip;
		Clip.FrameCount = 31;
		FMantleAnimClipTrackData& Root = Clip.Tracks.AddDefaulted_GetRef();
		Root.Bone = TEXT("root");
		FMantleAnimClipTrackData& Pelvis = Clip.Tracks.AddDefaulted_GetRef();
		Pelvis.Bone = TEXT("pelvis");
		for (int32 Frame = 0; Frame < Clip.FrameCount; ++Frame)
		{
			const float Alpha = Frame / 30.0f;
			Root.Rotations.Add(FQuat4f(FVector3f::UpVector, FMath::DegreesToRadians(90.0f * Alpha)));
			Root.Translations.Add(FVector3f(Travel * Alpha, 0.0f, 10.0f * FMath::Sin(Alpha * UE_PI)));
			Pelvis.Rotations.Add(FQuat4f::Identity);
			Pelvis.Translations.Add(FVector3f(0.0f, 0.0f, 95.0f));
		}
		return Clip;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleEmoteRootMotionTest, "Mantle.Editor.Emote.RootMotion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMantleEmoteRootMotionTest::RunTest(const FString& Parameters)
{
	using namespace MantleEmoteTestPrivate;
	const TArray<FName> Children = { TEXT("pelvis") };
	{
		// Within the limit: the pelvis does what the root did; the root holds still, as players' clients hold it.
		FMantleAnimClipData Clip = RootMotionClip(30.0f);
		FMantleAnimClipWriter::MoveRootOntoChildren(Clip, TEXT("root"), Children, FTransform::Identity, 50.0);
		TestTrue(TEXT("the root's motion is found"), Clip.RootMotion.bMoved);
		TestFalse(TEXT("30 cm stays a sway"), Clip.RootMotion.bInPlace);
		TestTrue(FString::Printf(TEXT("it travels 30 cm (%.1f)"), Clip.RootMotion.TravelCm), FMath::IsNearlyEqual(Clip.RootMotion.TravelCm, 30.0, 0.01));
		TestTrue(FString::Printf(TEXT("it rises 10 cm (%.1f)"), Clip.RootMotion.RiseCm), FMath::IsNearlyEqual(Clip.RootMotion.RiseCm, 10.0, 0.05));
		TestTrue(FString::Printf(TEXT("it turns 90 degrees (%.1f)"), Clip.RootMotion.TurnDegrees), FMath::IsNearlyEqual(Clip.RootMotion.TurnDegrees, 90.0, 0.01));
		const FMantleAnimClipTrackData& Root = Clip.Tracks[0];
		TestTrue(TEXT("the root holds its reference"), Root.Rotations.Num() == 1 && Root.Translations.Num() == 1 && Root.Translations[0].IsNearlyZero());
		const FMantleAnimClipTrackData& Pelvis = Clip.Tracks[1];
		TestTrue(FString::Printf(TEXT("the pelvis ends 30 cm on, 95 cm up (%s)"), *Pelvis.Translations.Last().ToString()),
			Pelvis.Translations.Last().Equals(FVector3f(30.0f, 0.0f, 95.0f), 0.01f));
		TestTrue(TEXT("the pelvis is 10 cm higher mid-way"), FMath::IsNearlyEqual(Pelvis.Translations[15].Z, 105.0f, 0.05f));
		TestTrue(TEXT("the pelvis ends turned 90 degrees"), FMath::IsNearlyEqual(FMath::RadiansToDegrees(Pelvis.Rotations.Last().GetAngle()), 90.0f, 0.01f));
	}
	{
		// Past the limit: in place. The travel goes; the bob and the turn stay.
		FMantleAnimClipData Clip = RootMotionClip(80.0f);
		FMantleAnimClipWriter::MoveRootOntoChildren(Clip, TEXT("root"), Children, FTransform::Identity, 50.0);
		TestTrue(TEXT("80 cm plays in place"), Clip.RootMotion.bInPlace);
		const FMantleAnimClipTrackData& Pelvis = Clip.Tracks[1];
		TestTrue(FString::Printf(TEXT("the pelvis stays over the spot (%s)"), *Pelvis.Translations.Last().ToString()),
			Pelvis.Translations.Last().Equals(FVector3f(0.0f, 0.0f, 95.0f), 0.01f));
		TestTrue(TEXT("the bob stays"), FMath::IsNearlyEqual(Pelvis.Translations[15].Z, 105.0f, 0.05f));
		TestTrue(TEXT("the turn stays"), FMath::IsNearlyEqual(FMath::RadiansToDegrees(Pelvis.Rotations.Last().GetAngle()), 90.0f, 0.01f));
		TArray<FMantleCosmeticIssue> Issues;
		if (const FMantleAvatarContract* C = Contract(*this))
		{
			FMantleAnimClipWriter::Validate(Clip, *C, Issues);
			TestFalse(TEXT("the root no longer moves past the cook's limit"), HasError(Issues, TEXT("the root moves")));
		}
	}
	{
		// A still root changes nothing.
		FMantleAnimClipData Clip = RootMotionClip(0.0f);
		for (FQuat4f& Rotation : Clip.Tracks[0].Rotations)
		{
			Rotation = FQuat4f::Identity;
		}
		for (FVector3f& Translation : Clip.Tracks[0].Translations)
		{
			Translation = FVector3f::ZeroVector;
		}
		const FMantleAnimClipData Before = Clip;
		FMantleAnimClipWriter::MoveRootOntoChildren(Clip, TEXT("root"), Children, FTransform::Identity, 50.0);
		TestFalse(TEXT("a still root is no motion"), Clip.RootMotion.bMoved);
		TestEqual(TEXT("the pelvis is untouched"), Clip.Tracks[1].Translations, Before.Tracks[1].Translations);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleEmoteLoopSeamTest, "Mantle.Editor.Emote.LoopSeam",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMantleEmoteLoopSeamTest::RunTest(const FString& Parameters)
{
	// Two seconds; the pelvis turns 2 degrees and rises 1 cm a frame. loop_start at 1 s: the end (120 degrees) jumps back to 60.
	auto Make = []
	{
		FMantleAnimClipData Clip;
		Clip.FrameCount = 61;
		FMantleAnimClipTrackData& Pelvis = Clip.Tracks.AddDefaulted_GetRef();
		Pelvis.Bone = TEXT("pelvis");
		FMantleAnimClipTrackData& Head = Clip.Tracks.AddDefaulted_GetRef();
		Head.Bone = TEXT("head");
		Head.Rotations.Add(FQuat4f::Identity);
		for (int32 Frame = 0; Frame < Clip.FrameCount; ++Frame)
		{
			Pelvis.Rotations.Add(FQuat4f(FVector3f::UpVector, FMath::DegreesToRadians(2.0f * Frame)));
			Pelvis.Translations.Add(FVector3f(0.0f, 0.0f, 90.0f + Frame));
		}
		return Clip;
	};
	FMantleAnimClipData Clip = Make();
	const FMantleAnimClipData Before = Clip;
	FMantleAnimClipWriter::CloseLoopSeam(Clip, 1.0f, 0.25f);
	const FMantleAnimClipTrackData& Pelvis = Clip.Tracks[0];
	TestTrue(FString::Printf(TEXT("the seam was 60 degrees (%.2f)"), Clip.LoopSeamDegrees), FMath::IsNearlyEqual(Clip.LoopSeamDegrees, 60.0, 0.01));
	TestTrue(FString::Printf(TEXT("and 30 cm (%.2f)"), Clip.LoopSeamCm), FMath::IsNearlyEqual(Clip.LoopSeamCm, 30.0, 0.01));
	TestTrue(TEXT("the last frame is the loop_start pose"), FMath::Abs(Pelvis.Rotations.Last() | Pelvis.Rotations[30]) > 1.0f - 1e-6f
		&& Pelvis.Translations.Last().Equals(Pelvis.Translations[30], 1e-3f));
	TestEqual(TEXT("frames before the blend are untouched"), Pelvis.Rotations[52], Before.Tracks[0].Rotations[52]);
	TestFalse(TEXT("frames in the blend move toward it"), Pelvis.Rotations[57] == Before.Tracks[0].Rotations[57]);
	TestEqual(TEXT("a constant track is untouched"), Clip.Tracks[1].Rotations.Num(), 1);

	FMantleAnimClipData AtEnd = Make();
	FMantleAnimClipWriter::CloseLoopSeam(AtEnd, 2.0f, 0.25f);
	TestEqual(TEXT("a loop_start at the end changes nothing"), AtEnd.Tracks[0].Rotations, Before.Tracks[0].Rotations);
	TestEqual(TEXT("and has no seam"), AtEnd.LoopSeamDegrees, 0.0);

	// The Loop choice: on, a clip with no loop_start loops whole (an event at 0, its end joined to its first frame).
	FMantleAnimClipData Whole = Make();
	TestTrue(TEXT("Loop on adds a loop_start"), FMantleAnimClipWriter::ApplyLoopChoice(Whole, true));
	TestTrue(TEXT("at 0"), Whole.Events.Num() == 1 && Whole.Events[0].Name == TEXT("loop_start") && Whole.Events[0].TimeSeconds == 0.0f);
	TestTrue(TEXT("the last frame is the first frame's pose"), FMath::Abs(Whole.Tracks[0].Rotations.Last() | Whole.Tracks[0].Rotations[0]) > 1.0f - 1e-6f);
	TestFalse(TEXT("a second time changes nothing"), FMantleAnimClipWriter::ApplyLoopChoice(Whole, true));
	// Off, the loop_start goes and the clip plays once.
	TestTrue(TEXT("Loop off removes it"), FMantleAnimClipWriter::ApplyLoopChoice(Whole, false));
	TestEqual(TEXT("no events left"), Whole.Events.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleEmoteSampleTest, "Mantle.Editor.Emote.Sample",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMantleEmoteSampleTest::RunTest(const FString& Parameters)
{
	using namespace MantleEmoteTestPrivate;
	const FMantleAvatarContract* C = Contract(*this);
	FString Error;
	USkeletalMesh* Mannequin = FMantleBuiltInMannequin::Get(&Error);
	if (!C || !TestNotNull(FString::Printf(TEXT("the built-in UEFN mannequin is made (%s)"), *Error), Mannequin))
	{
		return false;
	}
	// One second on the UEFN mannequin: the root walks 20 cm, the pelvis turns; a sound notify and loop_start.
	UPackage* Package = CreatePackage(TEXT("/Game/__MantleEmoteTest/AS_EmoteSample"));
	UAnimSequence* Sequence = NewObject<UAnimSequence>(Package, TEXT("AS_EmoteSample"), RF_Public | RF_Standalone | RF_Transient);
	Sequence->SetSkeleton(Mannequin->GetSkeleton());
	const FReferenceSkeleton& Reference = Mannequin->GetRefSkeleton();
	const int32 Frames = 30;
	{
		IAnimationDataController& Controller = Sequence->GetController();
		Controller.InitializeModel();
		Controller.OpenBracket(FText::FromString(TEXT("Emote sample test")), false);
		Controller.SetFrameRate(FFrameRate(30, 1), false);
		Controller.SetNumberOfFrames(FFrameNumber(Frames), false);
		for (const TCHAR* Bone : { TEXT("root"), TEXT("pelvis") })
		{
			const FTransform Ref = Reference.GetRawRefBonePose()[Reference.FindRawBoneIndex(FName(Bone))];
			TArray<FVector> Positions;
			TArray<FQuat> Rotations;
			TArray<FVector> Scales;
			for (int32 Key = 0; Key <= Frames; ++Key)
			{
				const bool bRoot = FCString::Strcmp(Bone, TEXT("root")) == 0;
				Positions.Add(bRoot ? Ref.GetTranslation() + FVector(20.0 * Key / Frames, 0.0, 0.0) : Ref.GetTranslation());
				Rotations.Add(bRoot ? Ref.GetRotation() : FQuat(FVector::UpVector, FMath::DegreesToRadians(30.0 * Key / Frames)) * Ref.GetRotation());
				Scales.Add(FVector::OneVector);
			}
			Controller.AddBoneCurve(FName(Bone), false);
			Controller.SetBoneTrackKeys(FName(Bone), Positions, Rotations, Scales, false);
		}
		Controller.NotifyPopulated();
		Controller.CloseBracket(false);
	}
	for (const TPair<const TCHAR*, float>& Notify : { TPair<const TCHAR*, float>(TEXT("PlaySound"), 0.2f), TPair<const TCHAR*, float>(TEXT("loop_start"), 0.5f) })
	{
		FAnimNotifyEvent& Event = Sequence->Notifies.AddDefaulted_GetRef();
		Event.NotifyName = FName(Notify.Key);
		Event.Link(Sequence, Notify.Value);
	}

	FMantleAnimClipData Clip;
	const bool bSampled = FMantleAnimClipWriter::Sample(Sequence, *C, Clip, Error);
	Sequence->ClearFlags(RF_Public | RF_Standalone);
	Sequence->MarkAsGarbage();
	if (!TestTrue(FString::Printf(TEXT("the clip samples (%s)"), *Error), bSampled))
	{
		return false;
	}
	TestEqual(TEXT("the sound notify is reported as left out"), Clip.DroppedNotifies, TArray<FString>({ TEXT("PlaySound") }));
	TestTrue(TEXT("loop_start is an event"), Clip.Events.Num() == 1 && Clip.Events[0].Name == TEXT("loop_start"));
	TestTrue(FString::Printf(TEXT("the root's 20 cm walk is found (%.1f)"), Clip.RootMotion.TravelCm), FMath::IsNearlyEqual(Clip.RootMotion.TravelCm, 20.0, 0.1));
	const FMantleAnimClipTrackData* Root = Find(Clip, TEXT("root"));
	TestTrue(TEXT("the root is constant"), Root && Root->Rotations.Num() == 1 && Root->Translations.Num() <= 1);
	const FMantleAnimClipTrackData* Pelvis = Find(Clip, TEXT("pelvis"));
	TestTrue(TEXT("the pelvis carries the walk"), Pelvis && Pelvis->Translations.Num() == Clip.FrameCount);
	TArray<FMantleCosmeticIssue> Issues;
	FMantleAnimClipWriter::Validate(Clip, *C, Issues);
	TestFalse(FString::Printf(TEXT("it is a valid emote (%s)"), Issues.Num() > 0 ? *Issues[0].ToString() : TEXT("")), FMantleCosmeticChecks::HasErrors(Issues));
	return true;
}

namespace MantleEmoteTestPrivate
{
	/** root, hand_r, index_01_r (5 cm on), index_02_r (3 cm on), index_03_r (2 cm on), each turned by its Turn about z. */
	FReferenceSkeleton MakeHand(const TArray<float>& TurnDegrees)
	{
		FReferenceSkeleton Skeleton;
		FReferenceSkeletonModifier Modifier(Skeleton, nullptr);
		const TCHAR* Names[] = { TEXT("root"), TEXT("hand_r"), TEXT("index_01_r"), TEXT("index_02_r"), TEXT("index_03_r") };
		const double Lengths[] = { 0.0, 100.0, 5.0, 3.0, 2.0 };
		for (int32 Bone = 0; Bone < 5; ++Bone)
		{
			const FQuat Turn(FVector::UpVector, FMath::DegreesToRadians(TurnDegrees.IsValidIndex(Bone) ? TurnDegrees[Bone] : 0.0f));
			Modifier.Add(FMeshBoneInfo(Names[Bone], Names[Bone], Bone - 1), FTransform(Turn, FVector(Lengths[Bone], 0.0, 0.0)));
		}
		return Skeleton;
	}

	TArray<FTransform> RefComponentPose(const FReferenceSkeleton& Skeleton, const TMap<FName, FQuat>& Offsets)
	{
		return FMantleEmoteCheck::ComponentPose(Skeleton, [&](const int32 Bone)
		{
			FTransform Local = Skeleton.GetRawRefBonePose()[Bone];
			if (const FQuat* Offset = Offsets.Find(Skeleton.GetBoneName(Bone)))
			{
				Local.SetRotation(Local.GetRotation() * *Offset);
			}
			return Local;
		});
	}

	/** A sampled clip's component pose at Frame on Skeleton (its keys, else the reference). */
	TArray<FTransform> ClipPose(const FMantleAnimClipData& Clip, const FReferenceSkeleton& Skeleton, const int32 Frame)
	{
		return FMantleEmoteCheck::ComponentPose(Skeleton, [&](const int32 Bone)
		{
			FTransform Local = Skeleton.GetRawRefBonePose()[Bone];
			const FMantleAnimClipTrackData* Track = Clip.Tracks.FindByPredicate([&](const FMantleAnimClipTrackData& Each) { return Each.Bone == Skeleton.GetBoneName(Bone); });
			if (Track && Bone > 0 && Track->Rotations.Num() > 0)
			{
				Local.SetRotation(FQuat(Track->Rotations[FMath::Min(Frame, Track->Rotations.Num() - 1)]));
				if (Track->Translations.Num() > 0)
				{
					Local.SetTranslation(FVector(Track->Translations[FMath::Min(Frame, Track->Translations.Num() - 1)]));
				}
			}
			return Local;
		});
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleEmoteRestHandsTest, "Mantle.Editor.Emote.RestHands",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMantleEmoteRestHandsTest::RunTest(const FString& Parameters)
{
	using namespace MantleEmoteTestPrivate;
	// The source's rest index finger curls 20 degrees at each joint; the target's is straight, its hand turned 10 degrees.
	const FReferenceSkeleton Source = MakeHand({ 0.0f, 0.0f, 20.0f, 20.0f, 20.0f });
	const FReferenceSkeleton Target = MakeHand({ 0.0f, 10.0f, 0.0f, 0.0f, 0.0f });
	double Largest = 0.0;
	const TArray<FName> Bones = FMantleEmoteRetarget::HandBones();
	TestTrue(TEXT("the hand bones hold the hands and fingers"), Bones.Contains(FName(TEXT("hand_r"))) && Bones.Contains(FName(TEXT("pinky_metacarpal_l"))));
	TestFalse(TEXT("the arms are left to the retargeter"), Bones.Contains(FName(TEXT("upperarm_l"))));
	const TMap<FName, FQuat> Offsets = FMantleEmoteRetarget::MatchRestRotations(Source, {}, Target, {}, Bones, &Largest);
	TestEqual(TEXT("the hand and the two finger bones with a child get offsets (the tip has nothing to point at)"), Offsets.Num(), 3);
	const TArray<FTransform> SourcePose = RefComponentPose(Source, {});
	const TArray<FTransform> TargetPose = RefComponentPose(Target, Offsets);
	for (int32 Bone = 1; Bone < 4; ++Bone)
	{
		TestTrue(FString::Printf(TEXT("%s turns as the source's"), *Source.GetBoneName(Bone).ToString()),
			TargetPose[Bone].GetRotation().Equals(SourcePose[Bone].GetRotation(), 1e-5));
	}
	TestTrue(TEXT("the fingertip sits where the source's does"), TargetPose[4].GetLocation().Equals(SourcePose[4].GetLocation(), 1e-3));
	TestTrue(FString::Printf(TEXT("the largest change is the 20 degree curl (%.2f)"), Largest), FMath::IsNearlyEqual(Largest, 20.0, 0.01));

	// The hand comparison: the same pose bends the same; a finger 15 degrees straighter at each joint is an under-curl.
	const FMantleHandMatch Same = FMantleEmoteCheck::CompareHands({ SourcePose }, Source, { SourcePose }, Source);
	TestEqual(TEXT("two joints of the index finger are compared"), Same.Joints, 2);
	TestTrue(TEXT("a pose against itself has no error"), FMath::Abs(Same.BendBiasDegrees) < 1e-6 && Same.WristErrorCm < 1e-6);
	const FReferenceSkeleton Straighter = MakeHand({ 0.0f, 0.0f, 5.0f, 5.0f, 5.0f });
	const FMantleHandMatch Under = FMantleEmoteCheck::CompareHands({ SourcePose }, Source, { RefComponentPose(Straighter, {}) }, Straighter);
	TestTrue(FString::Printf(TEXT("15 degrees straighter is a -15 degree bend (%.2f)"), Under.BendBiasDegrees), FMath::IsNearlyEqual(Under.BendBiasDegrees, -15.0, 0.01));
	return true;
}

namespace MantleEmoteTestPrivate
{
	/**
	 * A fist made in memory on MeshPath's skeleton (every finger joint closing 70 degrees over a second) retargeted onto the
	 * UEFN mannequin as an emote upload retargets it, its hands compared with the source's every third frame: finger bends
	 * (FMantleEmoteCheck::CompareHands) and the palm (CompareBodies' "hands": hand to middle finger, index to pinky).
	 */
	bool RunFistTest(FAutomationTestBase& Test, const TCHAR* MeshPath, const TCHAR* SkeletonId)
	{
		const FMantleAvatarContract* C = Contract(Test);
		USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, MeshPath);
		USkeletalMesh* Target = C ? FMantleBuiltInMannequin::UEFNMannequin(*C) : nullptr;
		if (!C || !Test.TestNotNull(FString::Printf(TEXT("the project has %s"), MeshPath), Mesh) || !Test.TestNotNull(TEXT("a UEFN mannequin"), Target))
		{
			return false;
		}
		const FReferenceSkeleton& Reference = Mesh->GetRefSkeleton();
		UPackage* Package = CreatePackage(TEXT("/Game/__MantleEmoteTest/AS_EmoteFist"));
		UAnimSequence* Sequence = NewObject<UAnimSequence>(Package, TEXT("AS_EmoteFist"), RF_Public | RF_Standalone | RF_Transient);
		Sequence->SetSkeleton(Mesh->GetSkeleton());
		Sequence->SetPreviewMesh(Mesh);
		const int32 Frames = 30;
		{
			IAnimationDataController& Controller = Sequence->GetController();
			Controller.InitializeModel();
			Controller.OpenBracket(FText::FromString(TEXT("Emote fist test")), false);
			Controller.SetFrameRate(FFrameRate(30, 1), false);
			Controller.SetNumberOfFrames(FFrameNumber(Frames), false);
			for (const FName Bone : FMantleEmoteRetarget::HandBones())
			{
				const int32 Index = Reference.FindRawBoneIndex(Bone);
				const FString Name = Bone.ToString();
				if (Index == INDEX_NONE || Name.StartsWith(TEXT("hand_")) || Name.Contains(TEXT("metacarpal")))
				{
					continue;
				}
				// Each finger joint closes 70 degrees over the second, about the bone's own curl axis (z on Epic's hands).
				const FTransform Ref = Reference.GetRawRefBonePose()[Index];
				TArray<FVector> Positions;
				TArray<FQuat> Rotations;
				TArray<FVector> Scales;
				for (int32 Key = 0; Key <= Frames; ++Key)
				{
					Positions.Add(Ref.GetTranslation());
					Rotations.Add(Ref.GetRotation() * FQuat(FVector::UpVector, FMath::DegreesToRadians(70.0 * Key / Frames)));
					Scales.Add(FVector::OneVector);
				}
				Controller.AddBoneCurve(Bone, false);
				Controller.SetBoneTrackKeys(Bone, Positions, Rotations, Scales, false);
			}
			Controller.NotifyPopulated();
			Controller.CloseBracket(false);
		}
		FMantleAnimClipData Clip;
		FString Error;
		FString HandsNote;
		const bool bRetargeted = FMantleEmoteRetarget::RetargetAndSample(Sequence, SkeletonId, Mesh, Target, *C, Clip, Error, &HandsNote);
		FMantleAnimClipData Source;
		FString SourceError;
		FMantleAnimClipWriter::Sample(Sequence, *C, Source, SourceError);
		Sequence->ClearFlags(RF_Public | RF_Standalone);
		Sequence->MarkAsGarbage();
		if (!Test.TestTrue(FString::Printf(TEXT("the fist retargets (%s)"), *Error), bRetargeted))
		{
			return false;
		}
		Test.AddInfo(HandsNote);
		TArray<TArray<FTransform>> SourcePoses;
		TArray<TArray<FTransform>> ConvertedPoses;
		for (int32 Frame = 0; Frame < Clip.FrameCount; Frame += 3)
		{
			SourcePoses.Add(ClipPose(Source, Reference, Frame));
			ConvertedPoses.Add(ClipPose(Clip, Target->GetRefSkeleton(), Frame));
		}
		const FMantleHandMatch Hands = FMantleEmoteCheck::CompareHands(SourcePoses, Reference, ConvertedPoses, Target->GetRefSkeleton());
		const FMantleBodyMatch Body = FMantleEmoteCheck::CompareBodies(SourcePoses, Reference, ConvertedPoses, Target->GetRefSkeleton());
		const FMantleBodyPart* Palm = Body.Parts.FindByPredicate([](const FMantleBodyPart& Part) { return Part.Name == TEXT("hands"); });
		Test.AddInfo(FString::Printf(TEXT("bend %+.2f degrees, error %.2f, worst %s %.2f, joints %d; palm %.2f degrees"), Hands.BendBiasDegrees,
			Hands.BendErrorDegrees, *Hands.BendWorstJoint.ToString(), Hands.BendWorstDegrees, Hands.Joints, Palm ? Palm->Mean : -1.0));
		Test.TestTrue(TEXT("every finger joint of both hands is compared"), Hands.Joints == 20);
		Test.TestTrue(FString::Printf(TEXT("the fingers curl as much as the source's (%+.1f degrees on average)"), Hands.BendBiasDegrees),
			FMath::Abs(Hands.BendBiasDegrees) < 5.0);
		// UE5 Mannequin without the rest match: the joints 16.7 degrees off on average, the worst 24; with it 6 and 14.5
		// (thumb_01: the two hands put the thumb's root at different places, so the hand-to-thumb segment never lines up).
		Test.TestTrue(FString::Printf(TEXT("the finger joints bend as the source's (%.1f degrees off on average)"), Hands.BendErrorDegrees), Hands.BendErrorDegrees < 9.0);
		// The thumb's first joint is measured along the hand-to-thumb-root line, which follows where each skeleton puts the
		// thumb's root (15 degrees off on the UE5 Mannequin, 24 on the UE4 one, whatever the retarget does).
		const double WorstLimit = Hands.BendWorstJoint.ToString().StartsWith(TEXT("thumb_01")) ? 30.0 : 18.0;
		Test.TestTrue(FString::Printf(TEXT("no finger joint is off by much (worst %s %.1f degrees)"), *Hands.BendWorstJoint.ToString(), Hands.BendWorstDegrees),
			Hands.BendWorstDegrees < WorstLimit);
		// UE4 Mannequin with the shipped chain map (metacarpals following the whole finger): the palm 29 to 44 degrees off
		// on the trial dances; held at the rest pose, 1.3.
		Test.TestTrue(FString::Printf(TEXT("the palm stays as the source's (%.1f degrees off)"), Palm ? Palm->Mean : -1.0), Palm && Palm->Mean < 10.0);
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleEmoteFingerCurlTest, "Mantle.Editor.Emote.FingerCurl",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMantleEmoteFingerCurlTest::RunTest(const FString& Parameters)
{
	// mantle #840: a UE5 Mannequin fist retargeted onto the UEFN mannequin keeps its curl. Without the rest-hand match the
	// FK finger chains carry the two rest hands' difference (a 17 to 35 degree under-curl).
	return MantleEmoteTestPrivate::RunFistTest(*this, TEXT("/Game/Characters/UE5_Mannequins/Meshes/SKM_Manny_Simple.SKM_Manny_Simple"), TEXT("ue5mannequin"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleEmoteFingerCurlUE4Test, "Mantle.Editor.Emote.FingerCurlUE4",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMantleEmoteFingerCurlUE4Test::RunTest(const FString& Parameters)
{
	// A UE4 Mannequin fist (no metacarpals) keeps its curl and its palm: the shipped retargeter maps the UEFN mannequin's
	// metacarpal chains to the whole UE4 fingers, which folded the palm with every curl.
	return MantleEmoteTestPrivate::RunFistTest(*this, TEXT("/Game/Characters/UE4_Mannequin/Meshes/SKM_UE4_Mannequin.SKM_UE4_Mannequin"), TEXT("ue4mannequin"));
}

#endif
