// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleSoundReader.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundWaveProcedural.h"

#include "MantleSoundWave.generated.h"

/**
 * How a place spatialises a sound that reaches silence at Radius cm: a 10% full-volume core
 * and a linear falloff over the rest, the proportions of UE's default attenuation. SoundEmitters and
 * the 3D sounds scripts play (Sounds:Play) share it, so a script sound placed beside an emitter
 * of the same radius fades out the same way.
 */
MANTLERUNTIME_API FSoundAttenuationSettings MantleSoundAttenuationForRadius(double Radius);

/**
 * The muffle every sound the player hears goes through (#383): a sound behind a wall (MantleApplyWorldSoundSettings) and the sky's weather
 * indoors (MantleWeatherSoundDefaults, MantleWeatherMuffleFor) share these numbers, in one place. Fully blocked or enclosed, a sound is
 * filtered down to BlockedLowPassFrequency and played at BlockedVolume times its volume. The attenuation assets (DefaultAttenuation, ATT_*)
 * are set with these same numbers by the editor script that makes them.
 */
namespace MantleSoundMuffle
{
	/** The low-pass cutoff of a fully blocked or enclosed sound (Hz). */
	constexpr float BlockedLowPassFrequency = 600.0f;

	/** The volume multiplier of a fully blocked or enclosed sound. */
	constexpr float BlockedVolume = 0.2f;

	/** How long a sound takes to ease into, or out of, the muffle (s), so a step past a wall corner is not a click. */
	constexpr float InterpolationSeconds = 0.25f;

	/**
	 * How often a sound re-checks for a wall (s). Must equal both ini values in DefaultEngine.ini: DefaultOcclusionCheckInterval (the sounds without a
	 * component) and [/Script/Engine.AudioComponent] OcclusionCheckInterval (every audio component, which new components copy from the class default).
	 * MantleSoundEnclosureTest checks the component one.
	 */
	constexpr float OcclusionCheckSeconds = 0.25f;
}

/**
 * Turns on, for a sound that plays in the world, what every such sound gets (#383), the one place that does:
 *  - Behind-wall muffling: the engine's own per-sound occlusion (a trace from the sound to the listener, ignoring only the sound's owner)
 *    on the Visibility channel, which walls block and characters (capsule, mesh, ragdoll) ignore, so a person standing between you and a
 *    sound does not muffle it. Blocked, the sound takes MantleSoundMuffle's cutoff and volume, eased over MantleSoundMuffle::InterpolationSeconds.
 *  - Room echo: the sound is sent to the master reverb by distance (the reverb the room echo, UMantleSoundEnclosureSubsystem, fills in),
 *    from the sound's own full volume radius out to the end of its falloff.
 * Call it after Settings' radius (AttenuationShapeExtents) and FalloffDistance are set; it keeps both, and every other setting, as they are.
 */
MANTLERUNTIME_API void MantleApplyWorldSoundSettings(FSoundAttenuationSettings& Settings);

/**
 * The component tag a script puts on a SoundEmitter's audio component while it has stopped it
 * (Entity:StopSound). The place builder binds an uploaded sound that arrives late without
 * starting it while the tag is there, so an emitter a script silenced stays silent until
 * Entity:PlaySound takes the tag off again.
 */
MANTLERUNTIME_API FName MantleSoundEmitterHeldTag();

/**
 * One sound emitter's playback of a creator's uploaded sound: the decoded MAU1 samples, shared
 * read-only with every other emitter playing the same sound, fed to the audio mixer as they are.
 *
 * Procedural, so there is no per-asset cook and no codec: OnGeneratePCMAudio copies PCM16 straight
 * from the samples. A looping wave wraps around inside a single callback, so the loop point is
 * gapless (no restart through OnAudioFinishedNative, which leaves a gap); a one-shot plays its
 * samples once and then produces nothing. A procedural source never ends by itself, so whoever plays
 * a one-shot stops its component after GetSourceDurationSeconds (the place builder does).
 *
 * One per emitter, never shared: the read position is the wave's own, and two components playing one
 * procedural wave would take turns at it. A CreateShared wave is the exception: each voice reads on its own.
 */
UCLASS(Transient)
class MANTLERUNTIME_API UMantleSoundWave : public USoundWaveProcedural
{
	GENERATED_BODY()

public:
	/** A new wave over InSound, owned by Outer (the audio component that plays it). Null when InSound is. */
	static UMantleSoundWave* Create(UObject* Outer, const TSharedPtr<const FMantleSoundData>& InSound, bool bInLoop);

	/**
	 * A one-shot any number of voices may play at once, such as a weapon's attack sound (every shot of every copy of the
	 * weapon plays it, fire and forget). Each voice gets its own reader over the shared samples (CreateSoundGenerator) and ends
	 * by itself after the last sample, so nobody stops it. Null when InSound is.
	 */
	static UMantleSoundWave* CreateShared(UObject* Outer, const TSharedPtr<const FMantleSoundData>& InSound);

	/** True for a CreateShared wave. */
	bool IsShared() const { return bShared; }

	/** Sets the samples and the wave's channels, rate, looping and duration. Before it plays. */
	void Initialize(const TSharedPtr<const FMantleSoundData>& InSound, bool bInLoop);

	/** The samples' own length (Duration is the engine's indefinite-looping value for a looping wave). */
	double GetSourceDurationSeconds() const;

	const TSharedPtr<const FMantleSoundData>& GetSoundData() const { return SoundData; }

	bool LoopsSamples() const { return bLoopSamples; }

	//~ Begin USoundWave Interface
	virtual void OnBeginGenerate() override;
	//~ End USoundWave Interface

	//~ Begin USoundBase Interface
	/** A CreateShared wave's voice: its own reader over the samples. Null otherwise (the wave's own OnGeneratePCMAudio plays). */
	virtual ISoundGeneratorPtr CreateSoundGenerator(const FSoundGeneratorInitParams& InParams) override;
	//~ End USoundBase Interface

	//~ Begin USoundWaveProcedural Interface
	/** Audio render thread: NumSamples interleaved PCM16 samples into OutAudio, or 0 once a one-shot has played. */
	virtual int32 OnGeneratePCMAudio(TArray<uint8>& OutAudio, int32 NumSamples) override;
	//~ End USoundWaveProcedural Interface

private:
	TSharedPtr<const FMantleSoundData> SoundData;

	/** The next interleaved sample. Touched only by the audio render thread once playing. */
	int64 ReadCursor = 0;

	bool bLoopSamples = false;
	bool bShared = false;
};
