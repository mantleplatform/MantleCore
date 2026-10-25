// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleSoundReader.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundWaveProcedural.h"

#include "MantleSoundWave.generated.h"

/**
 * How a place spatialises a sound that reaches silence at Radius cm (#263): a 10% full-volume core
 * and a linear falloff over the rest, the proportions of UE's default attenuation. SoundEmitters and
 * the 3D sounds scripts play (Sounds:Play, #459) share it, so a script sound placed beside an emitter
 * of the same radius fades out the same way.
 */
MANTLERUNTIME_API FSoundAttenuationSettings MantleSoundAttenuationForRadius(double Radius);

/**
 * The component tag a script puts on a SoundEmitter's audio component while it has stopped it
 * (Entity:StopSound, #459). The place builder binds an uploaded sound that arrives late without
 * starting it while the tag is there, so an emitter a script silenced stays silent until
 * Entity:PlaySound takes the tag off again.
 */
MANTLERUNTIME_API FName MantleSoundEmitterHeldTag();

/**
 * One sound emitter's playback of a creator's uploaded sound (#324): the decoded MAU1 samples, shared
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
	 * #898: a one-shot any number of voices may play at once, such as a weapon's attack sound (every shot of every copy of the
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
