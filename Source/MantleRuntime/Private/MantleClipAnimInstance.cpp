// Copyright Joshua Eger. All Rights Reserved.
#include "MantleClipAnimInstance.h"

#include "Animation/AnimInstanceProxy.h"
#include "Animation/AnimNodeBase.h"
#include "BoneContainer.h"
#include "BonePose.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "MantleAnimClip.h"
#include "ReferenceSkeleton.h"

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleClipAnimInstancePrivate
{
	/**
	 * The one node of UMantleClipAnimInstance: the clip and its time are copied from the instance on
	 * the game thread (PreUpdate), and the pose is sampled on the worker (Evaluate) with the tracks bound to
	 * compact bones once per (clip, bone container).
	 */
	struct FMantleClipAnimProxy : public FAnimInstanceProxy
	{
		explicit FMantleClipAnimProxy(UAnimInstance* InInstance)
			: FAnimInstanceProxy(InInstance)
		{
		}

		virtual void PreUpdate(UAnimInstance* InAnimInstance, float DeltaSeconds) override
		{
			FAnimInstanceProxy::PreUpdate(InAnimInstance, DeltaSeconds);
			const UMantleClipAnimInstance* Instance = Cast<UMantleClipAnimInstance>(InAnimInstance);
			Clip = Instance ? Instance->GetClip() : nullptr;
			Time = Instance && Clip.IsValid() ? Instance->GetClipTime() : 0.0f;
			bApplyRootTranslation = !Instance || Instance->GetApplyRootTranslation();
		}

		virtual bool Evaluate(FPoseContext& Output) override
		{
			Output.ResetToRefPose();
			if (!Clip.IsValid())
			{
				return true;
			}
			const FBoneContainer& Bones = Output.Pose.GetBoneContainer();
			Bind(Bones);
			const int32 NumPoseBones = Output.Pose.GetNumBones();
			FMantleAnimClipSampleOptions Options;
			Options.bLoop = false; // Time is already wrapped or clamped (FMantleClipPlayback::TimeAt).
			for (int32 TrackIndex = 0; TrackIndex < TrackToCompact.Num(); ++TrackIndex)
			{
				const FCompactPoseBoneIndex BoneIndex = TrackToCompact[TrackIndex];
				if (BoneIndex.GetInt() == INDEX_NONE || BoneIndex.GetInt() >= NumPoseBones)
				{
					continue;
				}
				FQuat4f Rotation = FQuat4f::Identity;
				FVector3f Translation = FVector3f::ZeroVector;
				bool bHasTranslation = false;
				if (!Clip->SampleTrack(TrackIndex, Time, Rotation, Translation, bHasTranslation, Options))
				{
					continue;
				}
				FTransform& Bone = Output.Pose[BoneIndex];
				Bone.SetRotation(FQuat(Rotation).GetNormalized());
				if (bHasTranslation && (TrackIndex != RootTrack || bApplyRootTranslation))
				{
					Bone.SetTranslation(FVector(Translation));
				}
			}
			return true;
		}

	private:
		/** Binds Clip's tracks to Bones' compact indices unless they already are (same clip, same container). */
		void Bind(const FBoneContainer& Bones)
		{
			const int32 NumTracks = Clip->Tracks.Num();
			if (BoundClip == Clip && BoundSerial == Bones.GetSerialNumber() && BoundBoneCount == Bones.GetCompactPoseNumBones()
				&& TrackToCompact.Num() == NumTracks)
			{
				return;
			}
			BoundClip = Clip;
			BoundSerial = Bones.GetSerialNumber();
			BoundBoneCount = Bones.GetCompactPoseNumBones();
			TrackToCompact.Init(FCompactPoseBoneIndex(INDEX_NONE), NumTracks);
			RootTrack = INDEX_NONE;
			if (!Bones.IsValid())
			{
				return;
			}
			// Names to the mesh's bone indices, then those to compact indices through the required-bone list.
			const FReferenceSkeleton& RefSkeleton = Bones.GetReferenceSkeleton();
			const FMantleAnimClipBinding Binding = Clip->Bind(RefSkeleton);
			RootTrack = Binding.RootTrack;
			TArray<int32> MeshToCompact;
			MeshToCompact.Init(INDEX_NONE, RefSkeleton.GetNum());
			const TArray<FBoneIndexType>& Required = Bones.GetBoneIndicesArray();
			for (int32 CompactIndex = 0; CompactIndex < Required.Num(); ++CompactIndex)
			{
				const int32 MeshIndex = static_cast<int32>(Required[CompactIndex]);
				if (MeshToCompact.IsValidIndex(MeshIndex))
				{
					MeshToCompact[MeshIndex] = CompactIndex;
				}
			}
			for (int32 TrackIndex = 0; TrackIndex < NumTracks && TrackIndex < Binding.TrackToBone.Num(); ++TrackIndex)
			{
				const int32 MeshIndex = Binding.TrackToBone[TrackIndex];
				if (MeshToCompact.IsValidIndex(MeshIndex) && MeshToCompact[MeshIndex] != INDEX_NONE)
				{
					TrackToCompact[TrackIndex] = FCompactPoseBoneIndex(MeshToCompact[MeshIndex]);
				}
			}
		}

		TSharedPtr<const FMantleAnimClip> Clip;
		float Time = 0.0f;
		/** Copied from the instance each update (an NPC's body keeps its root on the capsule). */
		bool bApplyRootTranslation = true;
		/** The track bound to the skeleton's root bone (INDEX_NONE: none), set with the binding. */
		int32 RootTrack = INDEX_NONE;

		/** Held, not only compared, so a new clip can never reuse the address of the one bound. */
		TSharedPtr<const FMantleAnimClip> BoundClip;
		uint16 BoundSerial = 0;
		int32 BoundBoneCount = INDEX_NONE;
		TArray<FCompactPoseBoneIndex> TrackToCompact;
	};
}

float FMantleClipPlayback::TimeAt(const double ServerSeconds, const float Duration) const
{
	if (!(Duration > 0.0f) || !FMath::IsFinite(ServerSeconds))
	{
		return 0.0f;
	}
	const double Length = static_cast<double>(Duration);
	double Time = StartOffset + (ServerSeconds - StartServerSeconds) * PlayRate;
	if (bLoop)
	{
		Time = FMath::Fmod(Time, Length);
		if (Time < 0.0)
		{
			Time += Length;
		}
	}
	else
	{
		Time = FMath::Clamp(Time, 0.0, Length);
	}
	return static_cast<float>(Time);
}

void UMantleClipAnimInstance::SetClip(TSharedPtr<const FMantleAnimClip> InClip, const FMantleClipPlayback& InPlayback)
{
	Clip = MoveTemp(InClip);
	Playback = InPlayback;
}

double UMantleClipAnimInstance::GetServerSeconds() const
{
	if (TimeSource)
	{
		return TimeSource();
	}
	const UWorld* World = GetWorld();
	if (!World)
	{
		return 0.0;
	}
	if (const AGameStateBase* GameState = World->GetGameState())
	{
		return GameState->GetServerWorldTimeSeconds();
	}
	return World->GetTimeSeconds();
}

float UMantleClipAnimInstance::GetClipTime() const
{
	return Clip.IsValid() ? Playback.TimeAt(GetServerSeconds(), Clip->GetDuration()) : 0.0f;
}

TSharedPtr<const FMantleAnimClip> UMantleClipAnimInstance::ReadEmoteClipFile(const FString& FilePath, FString& OutError)
{
	FMantleAnimClipReadResult Read = FMantleAnimClipReader::ReadFromFile(FilePath);
	if (!Read.bSuccess)
	{
		OutError = Read.Error;
		return nullptr;
	}
	if (Read.Clip.HasSkeleton())
	{
		OutError = TEXT("it is a place clip, not an emote on the UEFN skeleton");
		return nullptr;
	}
	return MakeShared<FMantleAnimClip>(MoveTemp(Read.Clip));
}

FAnimInstanceProxy* UMantleClipAnimInstance::CreateAnimInstanceProxy()
{
	return new MantleClipAnimInstancePrivate::FMantleClipAnimProxy(this);
}

void UMantleClipAnimInstance::DestroyAnimInstanceProxy(FAnimInstanceProxy* InProxy)
{
	delete InProxy;
}
