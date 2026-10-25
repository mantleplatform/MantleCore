// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAnimClipWriter.h"
#include "MantleAvatarContract.h"
#include "MantleSkeletonHash.h"

#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/FrameRate.h"
#include "ReferenceSkeleton.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleAnimClipWriterPrivate
{
	/** MAN1 flag bit 0: a place clip carrying its skeleton (cook.AnimFlagSkeleton). */
	constexpr uint32 ClipFlagSkeleton = 1 << 0;

	constexpr uint8 TrackRotationAnimated = 1 << 0;
	constexpr uint8 TrackTranslation = 1 << 1;
	constexpr uint8 TrackTranslationAnimated = 1 << 2;
	constexpr double MaxTranslationCm = 10000.0;
	constexpr double QuaternionTolerance = 0.01;

	/** How long the end of a looping emote blends into its loop_start pose (CloseLoopSeam). */
	constexpr float LoopSeamBlendSeconds = 0.25f;

	void WriteU32(TArray<uint8>& Out, uint32 Value)
	{
		Out.Add(static_cast<uint8>(Value));
		Out.Add(static_cast<uint8>(Value >> 8));
		Out.Add(static_cast<uint8>(Value >> 16));
		Out.Add(static_cast<uint8>(Value >> 24));
	}

	void WriteU16(TArray<uint8>& Out, uint16 Value)
	{
		Out.Add(static_cast<uint8>(Value));
		Out.Add(static_cast<uint8>(Value >> 8));
	}

	void WriteF32(TArray<uint8>& Out, float Value)
	{
		uint32 Bits = 0;
		FMemory::Memcpy(&Bits, &Value, 4);
		WriteU32(Out, Bits);
	}

	void WriteStr16(TArray<uint8>& Out, const FString& Value)
	{
		const FTCHARToUTF8 Utf8(*Value);
		WriteU16(Out, static_cast<uint16>(Utf8.Length()));
		Out.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	}

	template <typename T>
	bool AllEqual(const TArray<T>& Keys)
	{
		for (int32 Index = 1; Index < Keys.Num(); ++Index)
		{
			if (!(Keys[Index] == Keys[0]))
			{
				return false;
			}
		}
		return Keys.Num() > 0;
	}
}

double FMantleAnimClipData::DurationSeconds() const
{
	return FrameRate > 0.0f ? static_cast<double>(FrameCount - 1) / FrameRate : 0.0;
}

int16 FMantleAnimClipWriter::QuantizeSnorm16(float Value)
{
	const double Scaled = FMath::RoundHalfFromZero(FMath::Clamp(static_cast<double>(Value), -1.0, 1.0) * 32767.0);
	return static_cast<int16>(Scaled);
}

void FMantleAnimClipWriter::Canonicalize(FMantleAnimClipData& Clip)
{
	using namespace MantleAnimClipWriterPrivate;
	for (FMantleAnimClipTrackData& Track : Clip.Tracks)
	{
		for (FQuat4f& Rotation : Track.Rotations)
		{
			Rotation.Normalize();
		}
		if (AllEqual(Track.Rotations))
		{
			Track.Rotations.SetNum(1);
		}
		if (AllEqual(Track.Translations))
		{
			Track.Translations.SetNum(1);
		}
	}
	Clip.Events.StableSort([](const FMantleAnimClipEventData& A, const FMantleAnimClipEventData& B) { return A.TimeSeconds < B.TimeSeconds; });
}

TArray<uint8> FMantleAnimClipWriter::Encode(const FMantleAnimClipData& InClip, const FMantleContractSkeleton* Skeleton)
{
	using namespace MantleAnimClipWriterPrivate;

	FMantleAnimClipData Clip = InClip;
	Canonicalize(Clip);
	const bool bPlaceClip = !Clip.SkeletonHash.IsEmpty();
	if (bPlaceClip)
	{
		// A place clip's tracks in its own bone table's order, as the cook writes them.
		const TArray<FName>& Bones = Clip.SkeletonBones;
		Clip.Tracks.StableSort([&Bones](const FMantleAnimClipTrackData& A, const FMantleAnimClipTrackData& B)
		{
			const int32 IndexA = Bones.IndexOfByKey(A.Bone);
			const int32 IndexB = Bones.IndexOfByKey(B.Bone);
			return (IndexA == INDEX_NONE ? MAX_int32 : IndexA) < (IndexB == INDEX_NONE ? MAX_int32 : IndexB);
		});
	}
	else if (Skeleton)
	{
		Clip.Tracks.StableSort([Skeleton](const FMantleAnimClipTrackData& A, const FMantleAnimClipTrackData& B)
		{
			const int32 IndexA = Skeleton->Find(A.Bone);
			const int32 IndexB = Skeleton->Find(B.Bone);
			return (IndexA == INDEX_NONE ? MAX_int32 : IndexA) < (IndexB == INDEX_NONE ? MAX_int32 : IndexB);
		});
	}

	TArray<uint8> Out;
	Out.Append(reinterpret_cast<const uint8*>("MAN1"), 4);
	WriteU32(Out, 1);
	WriteU32(Out, bPlaceClip ? ClipFlagSkeleton : 0);
	WriteF32(Out, Clip.FrameRate);
	WriteU32(Out, static_cast<uint32>(Clip.FrameCount));
	WriteU32(Out, static_cast<uint32>(Clip.Tracks.Num()));
	WriteU32(Out, static_cast<uint32>(Clip.Events.Num()));
	if (bPlaceClip)
	{
		WriteStr16(Out, Clip.SkeletonHash);
		WriteU32(Out, static_cast<uint32>(Clip.SkeletonBones.Num()));
		for (int32 Bone = 0; Bone < Clip.SkeletonBones.Num(); ++Bone)
		{
			WriteStr16(Out, Clip.SkeletonBones[Bone].ToString());
			WriteU32(Out, static_cast<uint32>(Clip.SkeletonParents.IsValidIndex(Bone) ? Clip.SkeletonParents[Bone] : INDEX_NONE));
		}
	}
	for (const FMantleAnimClipTrackData& Track : Clip.Tracks)
	{
		WriteStr16(Out, Track.Bone.ToString());
		uint8 Flags = 0;
		Flags |= Track.Rotations.Num() > 1 ? TrackRotationAnimated : 0;
		Flags |= Track.Translations.Num() > 0 ? TrackTranslation : 0;
		Flags |= Track.Translations.Num() > 1 ? TrackTranslationAnimated : 0;
		Out.Add(Flags);
		for (const FQuat4f& Rotation : Track.Rotations)
		{
			WriteU16(Out, static_cast<uint16>(QuantizeSnorm16(Rotation.X)));
			WriteU16(Out, static_cast<uint16>(QuantizeSnorm16(Rotation.Y)));
			WriteU16(Out, static_cast<uint16>(QuantizeSnorm16(Rotation.Z)));
			WriteU16(Out, static_cast<uint16>(QuantizeSnorm16(Rotation.W)));
		}
		if (Track.Translations.Num() > 0)
		{
			FVector3f Lo(TNumericLimits<float>::Max());
			FVector3f Hi(TNumericLimits<float>::Lowest());
			for (const FVector3f& T : Track.Translations)
			{
				Lo = Lo.ComponentMin(T);
				Hi = Hi.ComponentMax(T);
			}
			WriteF32(Out, Lo.X); WriteF32(Out, Lo.Y); WriteF32(Out, Lo.Z);
			WriteF32(Out, Hi.X); WriteF32(Out, Hi.Y); WriteF32(Out, Hi.Z);
			for (const FVector3f& T : Track.Translations)
			{
				for (int32 Axis = 0; Axis < 3; ++Axis)
				{
					const double Span = static_cast<double>(Hi[Axis]) - static_cast<double>(Lo[Axis]);
					uint16 Quantized = 0;
					if (Span > 0.0)
					{
						Quantized = static_cast<uint16>(FMath::Clamp(
							FMath::RoundHalfFromZero((static_cast<double>(T[Axis]) - Lo[Axis]) / Span * 65535.0), 0.0, 65535.0));
					}
					WriteU16(Out, Quantized);
				}
			}
		}
	}
	for (const FMantleAnimClipEventData& Event : Clip.Events)
	{
		WriteF32(Out, Event.TimeSeconds);
		WriteStr16(Out, Event.Name);
	}
	WriteU32(Out, 0); // no extensions
	return Out;
}

void FMantleAnimClipWriter::Validate(const FMantleAnimClipData& Clip, const FMantleAvatarContract& Contract, TArray<FMantleCosmeticIssue>& OutIssues)
{
	using namespace MantleAnimClipWriterPrivate;

	const FMantleContractSkeleton* Skeleton = Contract.FindSkeleton(Contract.EmoteSkeleton);
	if (!Skeleton)
	{
		OutIssues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("the %s skeleton fixture is missing."), *Contract.EmoteSkeleton),
			TEXT("Reinstall the Mantle plugin.")));
		return;
	}
	if (FMath::Abs(Clip.FrameRate - Contract.EmoteFrameRate) > 1e-3f)
	{
		OutIssues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("frame rate %g, emotes are %g fps."), Clip.FrameRate, Contract.EmoteFrameRate)));
	}
	if (Clip.FrameCount < 1)
	{
		OutIssues.Add(FMantleCosmeticChecks::Error(TEXT("the clip has no frames.")));
		return;
	}
	if (Clip.DurationSeconds() > Contract.EmoteMaxSeconds + 1e-6)
	{
		OutIssues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("the clip is %.2f s long, the limit is %g s."), Clip.DurationSeconds(), Contract.EmoteMaxSeconds),
			TEXT("Trim the animation (Animation editor > Remove frames) and try again.")));
	}
	if (Clip.Tracks.Num() == 0)
	{
		OutIssues.Add(FMantleCosmeticChecks::Error(TEXT("the clip has no tracks.")));
		return;
	}

	TSet<FName> Seen;
	bool bForeign = false;
	for (const FMantleAnimClipTrackData& Track : Clip.Tracks)
	{
		if (Skeleton->Find(Track.Bone) == INDEX_NONE)
		{
			if (!bForeign)
			{
				OutIssues.Add(FMantleCosmeticChecks::Error(
					FString::Printf(TEXT("track \"%s\" is not a bone of the %s skeleton."), *Track.Bone.ToString(), *Skeleton->Id),
					TEXT("Emotes play on the UEFN mannequin. Retarget the animation onto SK_UEFN_Mannequin (right-click it > Retarget Animations) and upload the retargeted copy.")));
				bForeign = true;
			}
			continue;
		}
		bool bDuplicate = false;
		Seen.Add(Track.Bone, &bDuplicate);
		if (bDuplicate)
		{
			OutIssues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("bone \"%s\" has two tracks."), *Track.Bone.ToString())));
		}
		if (Track.Rotations.Num() != 1 && Track.Rotations.Num() != Clip.FrameCount)
		{
			OutIssues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("bone \"%s\" has %d rotation keys for %d frames."),
				*Track.Bone.ToString(), Track.Rotations.Num(), Clip.FrameCount)));
		}
		if (Track.Translations.Num() > 1 && Track.Translations.Num() != Clip.FrameCount)
		{
			OutIssues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("bone \"%s\" has %d translation keys for %d frames."),
				*Track.Bone.ToString(), Track.Translations.Num(), Clip.FrameCount)));
		}
		for (const FQuat4f& Rotation : Track.Rotations)
		{
			const double Length = FMath::Sqrt(static_cast<double>(Rotation.SizeSquared()));
			if (!Rotation.ContainsNaN() && FMath::Abs(Length - 1.0) > QuaternionTolerance)
			{
				OutIssues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("bone \"%s\" has a rotation that is not a unit quaternion (length %.3f)."),
					*Track.Bone.ToString(), Length)));
				break;
			}
			if (Rotation.ContainsNaN())
			{
				OutIssues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("bone \"%s\" has a rotation that is not a finite number."), *Track.Bone.ToString())));
				break;
			}
		}
		for (const FVector3f& Translation : Track.Translations)
		{
			if (Translation.ContainsNaN() || Translation.GetAbsMax() > MaxTranslationCm)
			{
				OutIssues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("bone \"%s\" has a translation of %.0f cm."),
					*Track.Bone.ToString(), Translation.GetAbsMax())));
				break;
			}
		}
		if (Track.Bone == Skeleton->Root() && Track.Translations.Num() > 1)
		{
			double Reach = 0.0;
			for (const FVector3f& Translation : Track.Translations)
			{
				Reach = FMath::Max(Reach, static_cast<double>(FVector3f::Distance(Translation, Track.Translations[0])));
			}
			if (Reach > Contract.EmoteMaxRootCm)
			{
				OutIssues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("the root moves %.1f cm, the limit is %g cm."), Reach, Contract.EmoteMaxRootCm),
					TEXT("Emotes play in place: remove the root motion (or keep it within the limit).")));
			}
		}
	}
	for (const FName Required : Contract.EmoteRequiredBones)
	{
		if (!Seen.Contains(Required))
		{
			OutIssues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("the clip has no \"%s\" track."), *Required.ToString())));
		}
	}
	if (Clip.Events.Num() > Contract.EmoteMaxEvents)
	{
		OutIssues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("%d events, the limit is %d."), Clip.Events.Num(), Contract.EmoteMaxEvents)));
	}
	for (const FMantleAnimClipEventData& Event : Clip.Events)
	{
		if (!Contract.EmoteEvents.Contains(Event.Name))
		{
			OutIssues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("event \"%s\" is not one of the allowed events."), *Event.Name),
				FString::Printf(TEXT("Allowed: %s."), *FString::Join(Contract.EmoteEvents, TEXT(", ")))));
		}
		else if (Event.TimeSeconds < 0.0f || Event.TimeSeconds > Clip.DurationSeconds() + 1e-6)
		{
			OutIssues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("event \"%s\" at %g s is outside the clip."), *Event.Name, Event.TimeSeconds)));
		}
	}
}

bool FMantleAnimClipWriter::Sample(const UAnimSequence* Sequence, const FMantleAvatarContract& Contract, FMantleAnimClipData& OutClip, FString& OutError)
{
	OutClip = FMantleAnimClipData();
	OutClip.FrameRate = Contract.EmoteFrameRate;
	if (!Sequence)
	{
		OutError = TEXT("no animation was chosen.");
		return false;
	}
	const IAnimationDataModel* Model = Sequence->GetDataModel();
	if (!Model)
	{
		OutError = FString::Printf(TEXT("%s has no animation data to read."), *Sequence->GetName());
		return false;
	}
	const int32 NumKeys = Model->GetNumberOfKeys();
	const FFrameRate SourceRate = Model->GetFrameRate();
	const double Length = Model->GetPlayLength();
	if (NumKeys < 1 || Length < 0.0)
	{
		OutError = FString::Printf(TEXT("%s has no frames."), *Sequence->GetName());
		return false;
	}
	OutClip.FrameCount = FMath::Max(1, FMath::FloorToInt32(Length * OutClip.FrameRate + 0.5) + 1);

	const USkeleton* Skeleton = Sequence->GetSkeleton();
	const FReferenceSkeleton* Reference = Skeleton ? &Skeleton->GetReferenceSkeleton() : nullptr;

	TArray<FName> TrackNames;
	Model->GetBoneTrackNames(TrackNames);
	for (const FName TrackName : TrackNames)
	{
		TArray<FTransform> Keys;
		Model->GetBoneTrackTransforms(TrackName, Keys);
		if (Keys.Num() == 0)
		{
			continue;
		}
		FMantleAnimClipTrackData& Track = OutClip.Tracks.AddDefaulted_GetRef();
		Track.Bone = TrackName;
		Track.Rotations.Reserve(OutClip.FrameCount);
		Track.Translations.Reserve(OutClip.FrameCount);
		for (int32 Frame = 0; Frame < OutClip.FrameCount; ++Frame)
		{
			const double Time = FMath::Min(static_cast<double>(Frame) / OutClip.FrameRate, Length);
			const FFrameTime SourceTime = SourceRate.AsFrameTime(Time);
			const int32 Key0 = FMath::Clamp(SourceTime.GetFrame().Value, 0, Keys.Num() - 1);
			const int32 Key1 = FMath::Min(Key0 + 1, Keys.Num() - 1);
			const float Alpha = Key1 == Key0 ? 0.0f : FMath::Clamp(SourceTime.GetSubFrame(), 0.0f, 1.0f);
			const FQuat Rotation = FQuat::Slerp(Keys[Key0].GetRotation(), Keys[Key1].GetRotation(), Alpha).GetNormalized();
			const FVector Translation = FMath::Lerp(Keys[Key0].GetTranslation(), Keys[Key1].GetTranslation(), static_cast<double>(Alpha));
			Track.Rotations.Add(FQuat4f(Rotation));
			Track.Translations.Add(FVector3f(Translation));
		}
	}

	// Players' clients hold the root still: what it does moves onto its children (the pelvis), so it still shows.
	if (Reference && Reference->GetNum() > 0)
	{
		TArray<FName> Children;
		for (int32 Bone = 1; Bone < Reference->GetNum(); ++Bone)
		{
			if (Reference->GetParentIndex(Bone) == 0)
			{
				Children.Add(Reference->GetBoneName(Bone));
			}
		}
		MoveRootOntoChildren(OutClip, Reference->GetBoneName(0), Children, Reference->GetRefBonePose()[0], Contract.EmoteMaxRootCm);
	}

	// A translation that stays at the bone's reference translation is what the node does with
	// no translation track: leave it out (MAN1's size cap is 512 KiB).
	for (FMantleAnimClipTrackData& Track : OutClip.Tracks)
	{
		const int32 BoneIndex = Reference ? Reference->FindBoneIndex(Track.Bone) : INDEX_NONE;
		if (BoneIndex == INDEX_NONE)
		{
			continue;
		}
		const FVector3f RefTranslation(Reference->GetRefBonePose()[BoneIndex].GetTranslation());
		bool bAtReference = true;
		for (const FVector3f& Translation : Track.Translations)
		{
			if (FVector3f::Distance(Translation, RefTranslation) > 0.01f)
			{
				bAtReference = false;
				break;
			}
		}
		if (bAtReference)
		{
			Track.Translations.Reset();
		}
	}

	float LoopStart = -1.0f;
	for (const FAnimNotifyEvent& Notify : Sequence->Notifies)
	{
		const FString Name = Notify.NotifyName.ToString();
		if (Contract.EmoteEvents.Contains(Name))
		{
			FMantleAnimClipEventData& Event = OutClip.Events.AddDefaulted_GetRef();
			Event.Name = Name;
			Event.TimeSeconds = FMath::Clamp(Notify.GetTime(), 0.0f, static_cast<float>(OutClip.DurationSeconds()));
			if (Name == TEXT("loop_start") && LoopStart < 0.0f)
			{
				LoopStart = Event.TimeSeconds;
			}
		}
		else
		{
			// Sounds, effects and the creator's own notifies: no event carries them (the allowlist is the cook's).
			OutClip.DroppedNotifies.AddUnique(Name.IsEmpty() || Name == TEXT("None") ? FString(TEXT("(unnamed)")) : Name);
		}
	}
	OutClip.DroppedCurves = Model->GetNumberOfFloatCurves();
	if (LoopStart >= 0.0f)
	{
		CloseLoopSeam(OutClip, LoopStart, MantleAnimClipWriterPrivate::LoopSeamBlendSeconds);
	}

	Canonicalize(OutClip);
	return true;
}

void FMantleAnimClipWriter::MoveRootOntoChildren(FMantleAnimClipData& Clip, const FName Root, TConstArrayView<FName> Children, const FTransform& RootReference,
	const double MaxTravelCm)
{
	Clip.RootMotion = FMantleAnimClipData::FRootMotion();
	FMantleAnimClipTrackData* RootTrack = Clip.Tracks.FindByPredicate([Root](const FMantleAnimClipTrackData& Track) { return Track.Bone == Root; });
	if (!RootTrack || Clip.FrameCount < 1 || RootTrack->Rotations.Num() == 0)
	{
		return;
	}
	const int32 Frames = Clip.FrameCount;
	auto RootAt = [RootTrack, &RootReference](const int32 Frame)
	{
		const FQuat4f Rotation = RootTrack->Rotations[FMath::Min(Frame, RootTrack->Rotations.Num() - 1)];
		const FVector3f Translation = RootTrack->Translations.Num() > 0 ? RootTrack->Translations[FMath::Min(Frame, RootTrack->Translations.Num() - 1)]
			: FVector3f(RootReference.GetTranslation());
		return FTransform(FQuat(Rotation).GetNormalized(), FVector(Translation));
	};

	// What the root does, from its first key: across the floor, up and down, and its turn.
	const FTransform First = RootAt(0);
	double Lowest = First.GetTranslation().Z;
	double Highest = Lowest;
	Clip.RootMotion.OffsetCm = FVector::Dist(First.GetTranslation(), RootReference.GetTranslation());
	Clip.RootMotion.OffsetDegrees = FMath::RadiansToDegrees(First.GetRotation().AngularDistance(RootReference.GetRotation()));
	const bool bOffReference = Clip.RootMotion.OffsetCm > 0.5 || Clip.RootMotion.OffsetDegrees > 0.5;
	for (int32 Frame = 0; Frame < Frames; ++Frame)
	{
		const FTransform At = RootAt(Frame);
		Clip.RootMotion.TravelCm = FMath::Max(Clip.RootMotion.TravelCm, FVector::Dist2D(At.GetTranslation(), First.GetTranslation()));
		Lowest = FMath::Min(Lowest, At.GetTranslation().Z);
		Highest = FMath::Max(Highest, At.GetTranslation().Z);
		Clip.RootMotion.TurnDegrees = FMath::Max(Clip.RootMotion.TurnDegrees, FMath::RadiansToDegrees(At.GetRotation().AngularDistance(First.GetRotation())));
	}
	Clip.RootMotion.RiseCm = Highest - Lowest;
	Clip.RootMotion.bInPlace = Clip.RootMotion.TravelCm > MaxTravelCm;
	// A retarget leaves the root a hair off its reference: under half a centimetre or half a degree is no motion.
	Clip.RootMotion.bMoved = bOffReference || Clip.RootMotion.TravelCm > 0.5 || Clip.RootMotion.RiseCm > 0.5 || Clip.RootMotion.TurnDegrees > 0.5;
	if (!Clip.RootMotion.bMoved)
	{
		return;
	}

	const FTransform ReferenceInverse = RootReference.Inverse();
	TArray<FTransform> RootKeys;
	for (int32 Frame = 0; Frame < Frames; ++Frame)
	{
		FTransform At = RootAt(Frame);
		if (Clip.RootMotion.bInPlace)
		{
			// On the spot: the root's travel across the floor is taken out, its height and turn kept.
			At.SetTranslation(FVector(RootReference.GetTranslation().X, RootReference.GetTranslation().Y, At.GetTranslation().Z));
		}
		RootKeys.Add(At * ReferenceInverse);
	}
	for (FMantleAnimClipTrackData& Track : Clip.Tracks)
	{
		if (!Children.Contains(Track.Bone) || Track.Rotations.Num() == 0 || Track.Translations.Num() == 0)
		{
			continue;
		}
		TArray<FQuat4f> Rotations;
		TArray<FVector3f> Translations;
		for (int32 Frame = 0; Frame < Frames; ++Frame)
		{
			const FTransform Child(FQuat(Track.Rotations[FMath::Min(Frame, Track.Rotations.Num() - 1)]),
				FVector(Track.Translations[FMath::Min(Frame, Track.Translations.Num() - 1)]));
			const FTransform Moved = Child * RootKeys[Frame];
			Rotations.Add(FQuat4f(Moved.GetRotation().GetNormalized()));
			Translations.Add(FVector3f(Moved.GetTranslation()));
		}
		Track.Rotations = MoveTemp(Rotations);
		Track.Translations = MoveTemp(Translations);
	}
	RootTrack->Rotations = { FQuat4f(RootReference.GetRotation().GetNormalized()) };
	RootTrack->Translations = { FVector3f(RootReference.GetTranslation()) };
}

void FMantleAnimClipWriter::CloseLoopSeam(FMantleAnimClipData& Clip, const float LoopStartSeconds, const float BlendSeconds)
{
	Clip.LoopSeamDegrees = 0.0;
	Clip.LoopSeamCm = 0.0;
	const int32 Last = Clip.FrameCount - 1;
	const int32 Loop = FMath::RoundToInt32(LoopStartSeconds * Clip.FrameRate);
	if (Loop < 0 || Loop >= Last - 1)
	{
		return;
	}
	// The blend: at most BlendSeconds and at most half the loop, and at least one frame.
	const int32 Window = FMath::Clamp(FMath::RoundToInt32(BlendSeconds * Clip.FrameRate), 1, FMath::Max(1, (Last - Loop) / 2));
	for (FMantleAnimClipTrackData& Track : Clip.Tracks)
	{
		if (Track.Rotations.Num() == Clip.FrameCount)
		{
			FQuat4f Gap = Track.Rotations[Loop] * Track.Rotations[Last].Inverse();
			if (Gap.W < 0.0f)
			{
				Gap = FQuat4f(-Gap.X, -Gap.Y, -Gap.Z, -Gap.W);
			}
			Gap.Normalize();
			Clip.LoopSeamDegrees = FMath::Max(Clip.LoopSeamDegrees, static_cast<double>(FMath::RadiansToDegrees(Gap.GetAngle())));
			for (int32 Step = 1; Step <= Window; ++Step)
			{
				const int32 Frame = Last - Window + Step;
				const float Alpha = static_cast<float>(Step) / Window;
				Track.Rotations[Frame] = (FQuat4f::Slerp(FQuat4f::Identity, Gap, Alpha) * Track.Rotations[Frame]).GetNormalized();
			}
		}
		if (Track.Translations.Num() == Clip.FrameCount)
		{
			const FVector3f Gap = Track.Translations[Loop] - Track.Translations[Last];
			Clip.LoopSeamCm = FMath::Max(Clip.LoopSeamCm, static_cast<double>(Gap.Size()));
			for (int32 Step = 1; Step <= Window; ++Step)
			{
				const int32 Frame = Last - Window + Step;
				Track.Translations[Frame] += Gap * (static_cast<float>(Step) / Window);
			}
		}
	}
}

bool FMantleAnimClipWriter::ApplyLoopChoice(FMantleAnimClipData& Clip, const bool bLoop)
{
	const auto IsLoopStart = [](const FMantleAnimClipEventData& Event) { return Event.Name == TEXT("loop_start"); };
	if (!bLoop)
	{
		return Clip.Events.RemoveAll(IsLoopStart) > 0;
	}
	if (Clip.Events.ContainsByPredicate(IsLoopStart))
	{
		return false;
	}
	FMantleAnimClipEventData& Event = Clip.Events.AddDefaulted_GetRef();
	Event.Name = TEXT("loop_start");
	Event.TimeSeconds = 0.0f;
	Clip.Events.StableSort([](const FMantleAnimClipEventData& A, const FMantleAnimClipEventData& B) { return A.TimeSeconds < B.TimeSeconds; });
	CloseLoopSeam(Clip, 0.0f, MantleAnimClipWriterPrivate::LoopSeamBlendSeconds);
	return true;
}

bool FMantleAnimClipWriter::SampleOnMesh(const UAnimSequence* Sequence, const USkeletalMesh* Mesh, const float FrameRate, FMantleAnimClipData& OutClip,
	FString& OutError)
{
	OutClip = FMantleAnimClipData();
	OutClip.FrameRate = FrameRate;
	if (!Sequence || !Mesh)
	{
		OutError = TEXT("no animation or no mesh was given.");
		return false;
	}
	const IAnimationDataModel* Model = Sequence->GetDataModel();
	if (!Model)
	{
		OutError = FString::Printf(TEXT("%s has no animation data to read."), *Sequence->GetName());
		return false;
	}
	const int32 NumKeys = Model->GetNumberOfKeys();
	const FFrameRate SourceRate = Model->GetFrameRate();
	const double Length = Model->GetPlayLength();
	if (NumKeys < 1 || Length < 0.0 || !(FrameRate > 0.0f))
	{
		OutError = FString::Printf(TEXT("%s has no frames."), *Sequence->GetName());
		return false;
	}
	const FReferenceSkeleton& Reference = Mesh->GetRefSkeleton();
	const TArray<FMeshBoneInfo>& BoneInfo = Reference.GetRawRefBoneInfo();
	const TArray<FTransform>& BonePose = Reference.GetRawRefBonePose();
	if (BoneInfo.Num() == 0)
	{
		OutError = FString::Printf(TEXT("%s has no bones."), *Mesh->GetName());
		return false;
	}
	OutClip.FrameCount = FMath::Max(1, FMath::FloorToInt32(Length * FrameRate + 0.5) + 1);

	// The skeleton block: the mesh's own bone table, which is the skeleton its upload is cooked with.
	for (const FMeshBoneInfo& Bone : BoneInfo)
	{
		OutClip.SkeletonBones.Add(Bone.Name);
		OutClip.SkeletonParents.Add(Bone.ParentIndex);
	}
	OutClip.SkeletonHash = FMantleSkeletonHash::Compute(Reference);

	TArray<FName> TrackNames;
	Model->GetBoneTrackNames(TrackNames);
	for (int32 BoneIndex = 0; BoneIndex < BoneInfo.Num(); ++BoneIndex)
	{
		const FName BoneName = BoneInfo[BoneIndex].Name;
		if (!TrackNames.Contains(BoneName))
		{
			continue;
		}
		TArray<FTransform> Keys;
		Model->GetBoneTrackTransforms(BoneName, Keys);
		if (Keys.Num() == 0)
		{
			continue;
		}
		FMantleAnimClipTrackData Track;
		Track.Bone = BoneName;
		Track.Rotations.Reserve(OutClip.FrameCount);
		Track.Translations.Reserve(OutClip.FrameCount);
		for (int32 Frame = 0; Frame < OutClip.FrameCount; ++Frame)
		{
			const double Time = FMath::Min(static_cast<double>(Frame) / FrameRate, Length);
			const FFrameTime SourceTime = SourceRate.AsFrameTime(Time);
			const int32 Key0 = FMath::Clamp(SourceTime.GetFrame().Value, 0, Keys.Num() - 1);
			const int32 Key1 = FMath::Min(Key0 + 1, Keys.Num() - 1);
			const float Alpha = Key1 == Key0 ? 0.0f : FMath::Clamp(SourceTime.GetSubFrame(), 0.0f, 1.0f);
			const FQuat Rotation = FQuat::Slerp(Keys[Key0].GetRotation(), Keys[Key1].GetRotation(), Alpha).GetNormalized();
			const FVector Translation = FMath::Lerp(Keys[Key0].GetTranslation(), Keys[Key1].GetTranslation(), static_cast<double>(Alpha));
			Track.Rotations.Add(FQuat4f(Rotation));
			Track.Translations.Add(FVector3f(Translation));
		}

		// What the bone keeps without a track is left out: a translation that stays at its reference translation
		// (0.01 cm), and a whole track that also never turns from its reference rotation.
		const FVector3f RefTranslation(BonePose[BoneIndex].GetTranslation());
		const FQuat4f RefRotation(BonePose[BoneIndex].GetRotation().GetNormalized());
		bool bTranslationAtReference = true;
		for (const FVector3f& Translation : Track.Translations)
		{
			bTranslationAtReference = bTranslationAtReference && FVector3f::Distance(Translation, RefTranslation) <= 0.01f;
		}
		if (bTranslationAtReference)
		{
			Track.Translations.Reset();
		}
		bool bRotationAtReference = true;
		for (const FQuat4f& Rotation : Track.Rotations)
		{
			bRotationAtReference = bRotationAtReference && FMath::Abs(Rotation | RefRotation) >= 1.0f - 1.0e-6f;
		}
		if (bRotationAtReference && Track.Translations.Num() == 0)
		{
			continue;
		}
		OutClip.Tracks.Add(MoveTemp(Track));
	}

	// A clip that moves nothing still holds a pose: one constant track on the root (a clip has at least one).
	if (OutClip.Tracks.Num() == 0)
	{
		FMantleAnimClipTrackData& Root = OutClip.Tracks.AddDefaulted_GetRef();
		Root.Bone = BoneInfo[0].Name;
		Root.Rotations.Add(FQuat4f(BonePose[0].GetRotation().GetNormalized()));
	}

	Canonicalize(OutClip);
	return true;
}
