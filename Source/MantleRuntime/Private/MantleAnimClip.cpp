// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAnimClip.h"

#include "MantleCookedCursor.h"
#include "MantleRuntime.h"
#include "MantleSkeletonHash.h"
#include "Misc/FileHelper.h"
#include "ReferenceSkeleton.h"

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleAnimClipPrivate
{
	/** MAN1 flag bit 0: a place clip carrying its skeleton (#469). */
	constexpr uint32 ClipFlagSkeleton = 1 << 0;

	constexpr uint8 TrackRotationAnimated = 1 << 0;
	constexpr uint8 TrackTranslation = 1 << 1;
	constexpr uint8 TrackTranslationAnimated = 1 << 2;
	constexpr uint8 KnownTrackFlags = TrackRotationAnimated | TrackTranslation | TrackTranslationAnimated;

	/** How far a dequantised key's length may be from 1 (the worker allows 0.01 before quantising). */
	constexpr float RotationLengthTolerance = 0.011f;

	/** Slack on an event's time past the end of the clip (float32 seconds). */
	constexpr float EventTimeSlack = 1.0e-4f;

	bool RejectClip(FString& OutError, const FString& Error)
	{
		OutError = Error;
		return false;
	}

	FMantleAnimClipReadResult FailClip(const FString& Error)
	{
		FMantleAnimClipReadResult Result;
		Result.Error = Error;
		return Result;
	}

	FMantleAnimClipReadResult TruncatedClip(const TCHAR* Part)
	{
		return FailClip(FString::Printf(TEXT("That animation clip is incomplete or corrupt (%s)."), Part));
	}

	/** The two keys around Frame and the blend between them, for a track with NumKeys keys (1 or FrameCount). */
	void KeysAround(const float Frame, const int32 NumKeys, int32& OutA, int32& OutB, float& OutAlpha)
	{
		if (NumKeys <= 1)
		{
			OutA = 0;
			OutB = 0;
			OutAlpha = 0.0f;
			return;
		}
		const float Clamped = FMath::Clamp(Frame, 0.0f, static_cast<float>(NumKeys - 1));
		OutA = FMath::Min(FMath::FloorToInt32(Clamped), NumKeys - 1);
		OutB = FMath::Min(OutA + 1, NumKeys - 1);
		OutAlpha = Clamped - static_cast<float>(OutA);
	}
}

float FMantleAnimClip::GetDuration() const
{
	if (FrameCount <= 1 || !(FrameRate > 0.0f))
	{
		return 0.0f;
	}
	return static_cast<float>(FrameCount - 1) / FrameRate;
}

FMantleAnimClipBinding FMantleAnimClip::Bind(const TConstArrayView<FName> BoneNames) const
{
	FMantleAnimClipBinding Binding;
	Binding.NumBones = BoneNames.Num();
	TMap<FName, int32> BoneIndexByName;
	BoneIndexByName.Reserve(BoneNames.Num());
	for (int32 BoneIndex = 0; BoneIndex < BoneNames.Num(); ++BoneIndex)
	{
		// The first bone of a name wins, as the reference skeleton's own lookup does.
		if (!BoneIndexByName.Contains(BoneNames[BoneIndex]))
		{
			BoneIndexByName.Add(BoneNames[BoneIndex], BoneIndex);
		}
	}
	Binding.TrackToBone.Init(INDEX_NONE, Tracks.Num());
	for (int32 TrackIndex = 0; TrackIndex < Tracks.Num(); ++TrackIndex)
	{
		if (const int32* BoneIndex = BoneIndexByName.Find(Tracks[TrackIndex].Bone))
		{
			Binding.TrackToBone[TrackIndex] = *BoneIndex;
			++Binding.NumBound;
			if (*BoneIndex == 0)
			{
				Binding.RootTrack = TrackIndex;
			}
		}
		else
		{
			++Binding.NumUnbound;
		}
	}
	return Binding;
}

FMantleAnimClipBinding FMantleAnimClip::Bind(const FReferenceSkeleton& Skeleton) const
{
	const TArray<FMeshBoneInfo>& BoneInfo = Skeleton.GetRawRefBoneInfo();
	TArray<FName> Names;
	Names.Reserve(BoneInfo.Num());
	for (const FMeshBoneInfo& Bone : BoneInfo)
	{
		Names.Add(Bone.Name);
	}
	return Bind(Names);
}

bool FMantleAnimClip::SampleTrack(const int32 TrackIndex, float Time, FQuat4f& OutRotation, FVector3f& OutTranslation, bool& bOutHasTranslation,
	const FMantleAnimClipSampleOptions& Options) const
{
	using namespace MantleAnimClipPrivate;

	bOutHasTranslation = false;
	if (!Tracks.IsValidIndex(TrackIndex) || Tracks[TrackIndex].Rotations.Num() == 0)
	{
		return false;
	}

	// Seconds to a fractional frame, clamped to the clip or wrapped around it.
	const float Duration = GetDuration();
	float Frame = 0.0f;
	if (Duration > 0.0f && FMath::IsFinite(Time))
	{
		if (Options.bLoop)
		{
			Time = FMath::Fmod(Time, Duration);
			if (Time < 0.0f)
			{
				Time += Duration;
			}
		}
		else
		{
			Time = FMath::Clamp(Time, 0.0f, Duration);
		}
		Frame = FMath::Clamp(Time * FrameRate, 0.0f, static_cast<float>(FrameCount - 1));
	}

	const FMantleAnimClipTrack& Track = Tracks[TrackIndex];
	int32 A = 0;
	int32 B = 0;
	float Alpha = 0.0f;
	KeysAround(Frame, Track.Rotations.Num(), A, B, Alpha);
	OutRotation = A == B ? Track.Rotations[A] : FQuat4f::Slerp(Track.Rotations[A], Track.Rotations[B], Alpha);

	if (Track.HasTranslation())
	{
		KeysAround(Frame, Track.Translations.Num(), A, B, Alpha);
		OutTranslation = A == B ? Track.Translations[A] : FMath::Lerp(Track.Translations[A], Track.Translations[B], Alpha);
		bOutHasTranslation = true;
	}
	return true;
}

bool FMantleAnimClip::Validate(FString& OutError) const
{
	using namespace MantleAnimClipPrivate;

	if (!FMath::IsFinite(FrameRate) || FrameRate <= 0.0f)
	{
		return RejectClip(OutError, FString::Printf(TEXT("the frame rate is %f"), FrameRate));
	}
	if (FrameCount < 1)
	{
		return RejectClip(OutError, TEXT("the clip has no frames"));
	}
	if (Tracks.Num() == 0)
	{
		return RejectClip(OutError, TEXT("the clip has no tracks"));
	}
	TSet<FName> Bones;
	for (const FMantleAnimClipTrack& Track : Tracks)
	{
		bool bDuplicate = false;
		Bones.Add(Track.Bone, &bDuplicate);
		if (Track.Bone.IsNone() || bDuplicate)
		{
			return RejectClip(OutError, FString::Printf(TEXT("track '%s' has no bone or repeats one"), *Track.Bone.ToString()));
		}
		const int32 NumRotations = Track.Rotations.Num();
		if (NumRotations != 1 && NumRotations != FrameCount)
		{
			return RejectClip(OutError, FString::Printf(TEXT("bone %s has %d rotation keys for %d frames"), *Track.Bone.ToString(), NumRotations, FrameCount));
		}
		const int32 NumTranslations = Track.Translations.Num();
		if (NumTranslations != 0 && NumTranslations != 1 && NumTranslations != FrameCount)
		{
			return RejectClip(OutError, FString::Printf(TEXT("bone %s has %d translation keys for %d frames"), *Track.Bone.ToString(), NumTranslations, FrameCount));
		}
		for (const FQuat4f& Rotation : Track.Rotations)
		{
			if (Rotation.ContainsNaN() || !Rotation.IsNormalized())
			{
				return RejectClip(OutError, FString::Printf(TEXT("bone %s has a rotation that is not a finite unit quaternion"), *Track.Bone.ToString()));
			}
		}
		for (const FVector3f& Translation : Track.Translations)
		{
			if (Translation.ContainsNaN())
			{
				return RejectClip(OutError, FString::Printf(TEXT("bone %s has a translation that is not finite"), *Track.Bone.ToString()));
			}
		}
	}
	const float Duration = GetDuration();
	for (const FMantleAnimClipEvent& Event : Events)
	{
		if (Event.Name.IsNone() || !FMath::IsFinite(Event.Time) || Event.Time < 0.0f || Event.Time > Duration + EventTimeSlack)
		{
			return RejectClip(OutError, FString::Printf(TEXT("event '%s' at %f s is unnamed or outside the %f s clip"), *Event.Name.ToString(), Event.Time, Duration));
		}
	}

	// #469: a place clip's skeleton, the worker's own rules (cook.ValidatePlaceAnimClip).
	if (!HasSkeleton())
	{
		if (SkeletonBoneNames.Num() > 0 || SkeletonBoneParents.Num() > 0)
		{
			return RejectClip(OutError, TEXT("the clip has a bone table but names no skeleton"));
		}
		return true;
	}
	if (SkeletonBoneNames.Num() == 0 || SkeletonBoneNames.Num() != SkeletonBoneParents.Num())
	{
		return RejectClip(OutError, TEXT("the clip's skeleton has no bones, or bones without parents"));
	}
	TSet<FName> TableBones;
	for (int32 BoneIndex = 0; BoneIndex < SkeletonBoneNames.Num(); ++BoneIndex)
	{
		const FName Name = SkeletonBoneNames[BoneIndex];
		const int32 Parent = SkeletonBoneParents[BoneIndex];
		bool bDuplicate = false;
		TableBones.Add(Name, &bDuplicate);
		if (Name.IsNone() || bDuplicate)
		{
			return RejectClip(OutError, FString::Printf(TEXT("the clip's skeleton bone %d has no name or repeats one"), BoneIndex));
		}
		if ((BoneIndex == 0) != (Parent == INDEX_NONE) || Parent >= BoneIndex || Parent < INDEX_NONE)
		{
			return RejectClip(OutError, FString::Printf(TEXT("the clip's skeleton is not parent-first with one root at bone %s"), *Name.ToString()));
		}
	}
	const FString Computed = FMantleSkeletonHash::Compute(SkeletonBoneNames, SkeletonBoneParents);
	if (!Computed.Equals(SkeletonHash, ESearchCase::CaseSensitive))
	{
		return RejectClip(OutError, FString::Printf(TEXT("the clip names skeleton %s, but its bone table is %s"), *SkeletonHash, *Computed));
	}
	for (const FMantleAnimClipTrack& Track : Tracks)
	{
		if (!TableBones.Contains(Track.Bone))
		{
			return RejectClip(OutError, FString::Printf(TEXT("track %s is not a bone of the clip's skeleton"), *Track.Bone.ToString()));
		}
	}
	if (Events.Num() > 0)
	{
		return RejectClip(OutError, TEXT("a place clip has events"));
	}
	return true;
}

FMantleAnimClipReadResult FMantleAnimClipReader::Read(const TConstArrayView<uint8> Data)
{
	using namespace MantleAnimClipPrivate;
	using MantleCookedFormat::FCursor;

	FCursor Cursor(Data);
	if (!Cursor.Magic("MAN1"))
	{
		return FailClip(TEXT("That asset is not a Mantle animation clip."));
	}
	const uint32 Version = Cursor.U32();
	if (Cursor.Failed())
	{
		return TruncatedClip(TEXT("header"));
	}
	if (Version != SupportedVersion)
	{
		return FailClip(FString::Printf(
			TEXT("That animation clip was cooked for a newer version of Mantle (format %u, this build reads %u)."), Version, SupportedVersion));
	}

	FMantleAnimClipReadResult Result;
	FMantleAnimClip& Clip = Result.Clip;

	const uint32 Flags = Cursor.U32();
	Clip.FrameRate = Cursor.F32();
	const uint32 FrameCount = Cursor.U32();
	const uint32 TrackCount = Cursor.U32();
	const uint32 EventCount = Cursor.U32();
	if (Cursor.Failed())
	{
		return TruncatedClip(TEXT("header"));
	}
	if ((Flags & ~ClipFlagSkeleton) != 0)
	{
		return FailClip(FString::Printf(TEXT("That animation clip uses flags this build does not know (0x%x)."), Flags));
	}
	if (!FMath::IsFinite(Clip.FrameRate) || Clip.FrameRate <= 0.0f || Clip.FrameRate > MaxFrameRate)
	{
		return FailClip(FString::Printf(TEXT("That animation clip has a frame rate of %f (above 0, at most %.0f)."), Clip.FrameRate, MaxFrameRate));
	}
	if (FrameCount < 1 || FrameCount > static_cast<uint32>(MaxFrames))
	{
		return FailClip(FString::Printf(TEXT("That animation clip claims %u frames (1 to %d)."), FrameCount, MaxFrames));
	}
	// Each track takes at least a str16 length, its flags and one rotation key; each event a time and a str16 length.
	if (TrackCount < 1 || TrackCount > static_cast<uint32>(MaxTracks) || !Cursor.Has(static_cast<int64>(TrackCount) * 11))
	{
		return FailClip(FString::Printf(TEXT("That animation clip claims %u tracks (1 to %d, within the file)."), TrackCount, MaxTracks));
	}
	if (EventCount > static_cast<uint32>(MaxEvents))
	{
		return FailClip(FString::Printf(TEXT("That animation clip claims %u events (at most %d)."), EventCount, MaxEvents));
	}
	Clip.FrameCount = static_cast<int32>(FrameCount);

	// #469: a place clip's skeleton block, the bone table it was authored on.
	if ((Flags & ClipFlagSkeleton) != 0)
	{
		Cursor.String(Clip.SkeletonHash);
		const uint32 BoneCount = Cursor.U32();
		if (Cursor.Failed())
		{
			return TruncatedClip(TEXT("skeleton block"));
		}
		if (!FMantleSkeletonHash::IsHash(Clip.SkeletonHash))
		{
			return FailClip(FString::Printf(TEXT("That animation clip names skeleton '%s', which is not a skeleton hash."), *Clip.SkeletonHash));
		}
		// Each bone takes at least a str16 length and a parent.
		if (BoneCount < 1 || BoneCount > static_cast<uint32>(MaxSkeletonBones) || !Cursor.Has(static_cast<int64>(BoneCount) * 6))
		{
			return FailClip(FString::Printf(TEXT("That animation clip's skeleton claims %u bones (1 to %d, within the file)."), BoneCount, MaxSkeletonBones));
		}
		Clip.SkeletonBoneNames.Reserve(static_cast<int32>(BoneCount));
		Clip.SkeletonBoneParents.Reserve(static_cast<int32>(BoneCount));
		for (uint32 BoneIndex = 0; BoneIndex < BoneCount; ++BoneIndex)
		{
			FString Name;
			Cursor.String(Name);
			const int32 Parent = Cursor.I32();
			if (Cursor.Failed())
			{
				return TruncatedClip(TEXT("skeleton block"));
			}
			if (Name.IsEmpty())
			{
				return FailClip(TEXT("That animation clip's skeleton has a bone without a name."));
			}
			Clip.SkeletonBoneNames.Add(FName(*Name));
			Clip.SkeletonBoneParents.Add(Parent);
		}
	}

	TSet<FName> Bones;
	Clip.Tracks.SetNum(static_cast<int32>(TrackCount));
	for (FMantleAnimClipTrack& Track : Clip.Tracks)
	{
		FString Bone;
		Cursor.String(Bone);
		const uint8 TrackFlags = Cursor.U8();
		if (Cursor.Failed())
		{
			return TruncatedClip(TEXT("track header"));
		}
		if (Bone.IsEmpty())
		{
			return FailClip(TEXT("That animation clip has a track without a bone."));
		}
		Track.Bone = FName(*Bone);
		bool bDuplicate = false;
		Bones.Add(Track.Bone, &bDuplicate);
		if (bDuplicate)
		{
			return FailClip(FString::Printf(TEXT("That animation clip has two tracks for bone %s."), *Bone));
		}
		if ((TrackFlags & ~KnownTrackFlags) != 0 || ((TrackFlags & TrackTranslationAnimated) != 0 && (TrackFlags & TrackTranslation) == 0))
		{
			return FailClip(FString::Printf(TEXT("That animation clip's track %s has invalid flags (0x%x)."), *Bone, TrackFlags));
		}

		const int64 RotationKeys = (TrackFlags & TrackRotationAnimated) != 0 ? FrameCount : 1;
		if (!Cursor.Has(RotationKeys * 8))
		{
			return TruncatedClip(TEXT("rotation keys"));
		}
		Track.Rotations.SetNumUninitialized(static_cast<int32>(RotationKeys));
		for (FQuat4f& Rotation : Track.Rotations)
		{
			float Components[4];
			for (float& Component : Components)
			{
				Component = static_cast<float>(static_cast<int16>(Cursor.U16())) / 32767.0f;
			}
			Rotation = FQuat4f(Components[0], Components[1], Components[2], Components[3]);
			const float Length = FMath::Sqrt(Rotation.SizeSquared());
			if (FMath::Abs(Length - 1.0f) > RotationLengthTolerance)
			{
				return FailClip(FString::Printf(TEXT("That animation clip's track %s has a rotation that is not a unit quaternion (length %.3f)."), *Bone, Length));
			}
			Rotation /= Length;
		}

		if ((TrackFlags & TrackTranslation) != 0)
		{
			FVector3f Low, High;
			Low.X = Cursor.F32();
			Low.Y = Cursor.F32();
			Low.Z = Cursor.F32();
			High.X = Cursor.F32();
			High.Y = Cursor.F32();
			High.Z = Cursor.F32();
			if (Cursor.Failed())
			{
				return TruncatedClip(TEXT("translation range"));
			}
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				if (!FMath::IsFinite(Low[Axis]) || !FMath::IsFinite(High[Axis]) || Low[Axis] > High[Axis]
					|| FMath::Abs(Low[Axis]) > MaxTranslationCm || FMath::Abs(High[Axis]) > MaxTranslationCm)
				{
					return FailClip(FString::Printf(TEXT("That animation clip's track %s has an invalid translation range."), *Bone));
				}
			}
			const int64 TranslationKeys = (TrackFlags & TrackTranslationAnimated) != 0 ? FrameCount : 1;
			if (!Cursor.Has(TranslationKeys * 6))
			{
				return TruncatedClip(TEXT("translation keys"));
			}
			Track.Translations.SetNumUninitialized(static_cast<int32>(TranslationKeys));
			for (FVector3f& Translation : Track.Translations)
			{
				for (int32 Axis = 0; Axis < 3; ++Axis)
				{
					const double Step = static_cast<double>(Cursor.U16());
					Translation[Axis] = static_cast<float>(static_cast<double>(Low[Axis]) + Step / 65535.0 * static_cast<double>(High[Axis] - Low[Axis]));
				}
			}
		}
	}

	if (!Cursor.Has(static_cast<int64>(EventCount) * 6))
	{
		return TruncatedClip(TEXT("events"));
	}
	Clip.Events.SetNum(static_cast<int32>(EventCount));
	for (FMantleAnimClipEvent& Event : Clip.Events)
	{
		Event.Time = Cursor.F32();
		FString Name;
		Cursor.String(Name);
		if (Cursor.Failed())
		{
			return TruncatedClip(TEXT("events"));
		}
		if (!Name.IsEmpty())
		{
			Event.Name = FName(*Name);
		}
	}

	FString ExtensionError;
	if (!MantleCookedFormat::SkipExtensions(Cursor, Result.SkippedExtensions, ExtensionError))
	{
		return FailClip(FString::Printf(TEXT("That animation clip is corrupt: %s."), *ExtensionError));
	}
	if (Cursor.Remaining() != 0)
	{
		return FailClip(FString::Printf(TEXT("That animation clip is corrupt: %lld bytes follow its end."), Cursor.Remaining()));
	}

	FString Invalid;
	if (!Clip.Validate(Invalid))
	{
		return FailClip(FString::Printf(TEXT("That animation clip is corrupt: %s."), *Invalid));
	}

	UE_LOG(LogMantle, Verbose, TEXT("Read an animation clip: %d frames at %.1f fps (%.2f s), %d tracks, %d events%s%s"),
		Clip.FrameCount, Clip.FrameRate, Clip.GetDuration(), Clip.Tracks.Num(), Clip.Events.Num(),
		Clip.HasSkeleton() ? TEXT(", on skeleton ") : TEXT(""), *Clip.SkeletonHash);

	Result.bSuccess = true;
	return Result;
}

FMantleAnimClipReadResult FMantleAnimClipReader::ReadFromFile(const FString& FilePath)
{
	TArray<uint8> Data;
	if (!FFileHelper::LoadFileToArray(Data, *FilePath))
	{
		return MantleAnimClipPrivate::FailClip(FString::Printf(TEXT("Could not read '%s'."), *FilePath));
	}
	return Read(Data);
}
