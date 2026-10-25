// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "MantleClipAnimInstance.generated.h"

struct FMantleAnimClip;

/**
 * How a SkeletalMesh entity plays its clip (#469), the place file's loop, playRate and startOffset.
 * Deterministic from the server's clock: every client samples the clip at the same time, so nothing
 * per frame replicates.
 */
struct MANTLERUNTIME_API FMantleClipPlayback
{
	bool bLoop = true;

	/** Clip seconds per server second; 0 holds the pose at StartOffset. */
	double PlayRate = 1.0;

	/** Seconds into the clip at server time StartServerSeconds. */
	double StartOffset = 0.0;

	/**
	 * The server time playback started: 0 for a place's own clip (it plays from the server's start), the
	 * server time of the call for a script's Entity:PlayClip (script API 1.18), so every machine starts it
	 * together however late it hears of it.
	 */
	double StartServerSeconds = 0.0;

	/**
	 * Seconds into a clip of Duration at ServerSeconds: StartOffset + (ServerSeconds - StartServerSeconds) x
	 * PlayRate, wrapped into [0, Duration) when looping, else clamped to [0, Duration] (a one-shot plays from
	 * its start and holds its last frame). 0 for a clip of no duration. Computed in double, so a server that
	 * has run for days still samples smoothly.
	 */
	float TimeAt(double ServerSeconds, float Duration) const;
};

/**
 * The single-node anim instance of a place skeletal mesh (#469): one native node that samples a runtime
 * MAN1 place clip (FMantleAnimClip) into the pose, the way UAnimSingleNodeInstance plays a UAnimSequence
 * (which a cooked client cannot build at runtime). No anim Blueprint. Bones the clip has no track for keep
 * the reference pose; the root's track is applied like any other (a prop has no capsule to keep it).
 *
 * Tracks are bound to the mesh's bones by name once per (clip, bone container), so a LOD change rebinds and
 * sampling does no name lookups. Time comes from the server's clock (AGameStateBase::GetServerWorldTimeSeconds,
 * the world's own time without a game state) through FMantleClipPlayback, read on the game thread before each
 * update. A dedicated server never builds one (FMantlePlaceBuilder spawns no mesh there).
 */
UCLASS(Transient, NotBlueprintable)
class MANTLERUNTIME_API UMantleClipAnimInstance : public UAnimInstance
{
	GENERATED_BODY()

public:
	/** The clip to play (null: the reference pose) and how. Game thread. */
	void SetClip(TSharedPtr<const FMantleAnimClip> InClip, const FMantleClipPlayback& InPlayback);

	TSharedPtr<const FMantleAnimClip> GetClip() const { return Clip; }
	const FMantleClipPlayback& GetPlayback() const { return Playback; }

	/** Seconds into the clip now (0 without one). Game thread. */
	float GetClipTime() const;

	/** The server seconds playback follows: the time source when set, else the world's server time. */
	double GetServerSeconds() const;

	/** Replaces the clock the player reads, so a script clip and tween share the server clock. An unset function restores the default. */
	void SetTimeSource(TFunction<double()> InTimeSource) { TimeSource = MoveTemp(InTimeSource); }

	/**
	 * #475: whether the root bone's translation track moves the root (true, the default: a prop has no
	 * capsule to keep it). A scripted NPC's body turns it off: its capsule carries it, so a clip with
	 * root motion must not walk the body away from it. Game thread.
	 */
	void SetApplyRootTranslation(bool bApply) { bApplyRootTranslation = bApply; }
	bool GetApplyRootTranslation() const { return bApplyRootTranslation; }

	/**
	 * #535: a MAN1 emote clip (no skeleton block) read from a file (FMantleAnimClipReader), for code that cannot include
	 * MantleAnimClip.h: MantleEditor has writer types of the same names (MantleAnimClipWriter.h). The avatar preview's
	 * idle is read this way. Null, with OutError, when the file cannot be read or holds a place clip.
	 */
	static TSharedPtr<const FMantleAnimClip> ReadEmoteClipFile(const FString& FilePath, FString& OutError);

protected:
	virtual FAnimInstanceProxy* CreateAnimInstanceProxy() override;
	virtual void DestroyAnimInstanceProxy(FAnimInstanceProxy* InProxy) override;

private:
	TSharedPtr<const FMantleAnimClip> Clip;
	FMantleClipPlayback Playback;
	TFunction<double()> TimeSource;
	bool bApplyRootTranslation = true;
};
