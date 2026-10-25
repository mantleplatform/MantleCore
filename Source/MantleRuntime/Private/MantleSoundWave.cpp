// Copyright Joshua Eger. All Rights Reserved.
#include "MantleSoundWave.h"

#include "Sound/SoundGenerator.h"
#include "Sound/SoundGroups.h"

FSoundAttenuationSettings MantleSoundAttenuationForRadius(const double Radius)
{
	FSoundAttenuationSettings Attenuation;
	Attenuation.bAttenuate = true;
	Attenuation.bSpatialize = true;
	Attenuation.AttenuationShape = EAttenuationShape::Sphere;
	Attenuation.AttenuationShapeExtents = FVector(Radius * 0.1, 0.0, 0.0);
	Attenuation.FalloffDistance = static_cast<float>(Radius * 0.9);
	return Attenuation;
}

FName MantleSoundEmitterHeldTag()
{
	static const FName Tag(TEXT("MantleScriptSoundHeld"));
	return Tag;
}

UMantleSoundWave* UMantleSoundWave::Create(UObject* Outer, const TSharedPtr<const FMantleSoundData>& InSound, const bool bInLoop)
{
	if (!InSound.IsValid() || InSound->Samples.Num() == 0)
	{
		return nullptr;
	}
	// Transient: an editor level a place is imported into never saves a wave whose samples it cannot keep.
	UMantleSoundWave* Wave = NewObject<UMantleSoundWave>(Outer ? Outer : GetTransientPackage(), NAME_None, RF_Transient);
	Wave->Initialize(InSound, bInLoop);
	return Wave;
}

UMantleSoundWave* UMantleSoundWave::CreateShared(UObject* Outer, const TSharedPtr<const FMantleSoundData>& InSound)
{
	UMantleSoundWave* Wave = Create(Outer, InSound, /*bInLoop*/ false);
	if (Wave)
	{
		Wave->bShared = true;
	}
	return Wave;
}

namespace MantleSoundWavePrivate
{
	/** One voice of a shared one-shot: its own read position over the shared samples, finished after the last one. */
	class FOneShotReader : public ISoundGenerator
	{
	public:
		explicit FOneShotReader(const TSharedPtr<const FMantleSoundData>& InSound)
			: Sound(InSound)
		{
		}

		virtual int32 OnGenerateAudio(float* OutAudio, const int32 NumSamples) override
		{
			const int64 Total = Sound.IsValid() ? Sound->Samples.Num() : 0;
			const int32 Run = static_cast<int32>(FMath::Clamp<int64>(Total - Cursor, 0, NumSamples));
			const int16* Samples = Run > 0 ? Sound->Samples.GetData() + Cursor : nullptr;
			for (int32 Index = 0; Index < Run; ++Index)
			{
				OutAudio[Index] = static_cast<float>(Samples[Index]) / 32768.f;
			}
			// The last buffer is padded with silence; the voice ends once IsFinished says so.
			for (int32 Index = Run; Index < NumSamples; ++Index)
			{
				OutAudio[Index] = 0.f;
			}
			Cursor += Run;
			return NumSamples;
		}

		virtual bool IsFinished() const override
		{
			return !Sound.IsValid() || Cursor >= Sound->Samples.Num();
		}

		virtual int32 GetNumChannels() const override
		{
			return Sound.IsValid() ? FMath::Clamp(Sound->Channels, 1, 2) : 1;
		}

	private:
		TSharedPtr<const FMantleSoundData> Sound;
		int64 Cursor = 0;
	};
}

ISoundGeneratorPtr UMantleSoundWave::CreateSoundGenerator(const FSoundGeneratorInitParams& InParams)
{
	if (!bShared || !SoundData.IsValid())
	{
		return nullptr;
	}
	return MakeShared<MantleSoundWavePrivate::FOneShotReader, ESPMode::ThreadSafe>(SoundData);
}

void UMantleSoundWave::Initialize(const TSharedPtr<const FMantleSoundData>& InSound, const bool bInLoop)
{
	SoundData = InSound;
	bLoopSamples = bInLoop;
	ReadCursor = 0;

	const int32 Channels = SoundData.IsValid() ? SoundData->Channels : 1;
	const int32 Rate = SoundData.IsValid() ? SoundData->SampleRate : 0;
	NumChannels = FMath::Clamp(Channels, 1, 2);
	SetSampleRate(static_cast<uint32>(FMath::Max(Rate, 0)));
	SoundGroup = SOUNDGROUP_Default;
	// What the engine's own procedural sounds set: generation may run off the audio render thread.
	bCanProcessAsync = true;
	// The engine reads looping and length from the wave: a looping wave is indefinite, a one-shot its samples' length.
	bLooping = bInLoop;
	Duration = bInLoop ? INDEFINITELY_LOOPING_DURATION : static_cast<float>(GetSourceDurationSeconds());
}

double UMantleSoundWave::GetSourceDurationSeconds() const
{
	return SoundData.IsValid() ? SoundData->DurationSeconds() : 0.0;
}

void UMantleSoundWave::OnBeginGenerate()
{
	// A new voice (a Play, or a virtualised sound coming back) starts at the beginning.
	ReadCursor = 0;
}

int32 UMantleSoundWave::OnGeneratePCMAudio(TArray<uint8>& OutAudio, const int32 NumSamples)
{
	// The base class reads back whatever is left in OutAudio, so it holds exactly what this call made.
	OutAudio.Reset();
	const FMantleSoundData* Data = SoundData.Get();
	const int64 Total = Data ? Data->Samples.Num() : 0;
	if (Total <= 0 || NumSamples <= 0 || (!bLoopSamples && ReadCursor >= Total))
	{
		return 0;
	}

	OutAudio.SetNumUninitialized(NumSamples * static_cast<int32>(sizeof(int16)));
	int16* Out = reinterpret_cast<int16*>(OutAudio.GetData());
	const int16* Samples = Data->Samples.GetData();
	int32 Written = 0;
	while (Written < NumSamples)
	{
		if (ReadCursor >= Total)
		{
			if (!bLoopSamples)
			{
				break;
			}
			// Gapless: the next sample after the last is the first, in the same buffer. The sample
			// count is a whole number of frames, so the channels stay interleaved in step.
			ReadCursor = 0;
		}
		const int32 Run = static_cast<int32>(FMath::Min<int64>(NumSamples - Written, Total - ReadCursor));
		FMemory::Memcpy(Out + Written, Samples + ReadCursor, Run * sizeof(int16));
		Written += Run;
		ReadCursor += Run;
	}
	if (Written < NumSamples)
	{
		// A one-shot's last buffer: the rest is silence, then nothing more.
		FMemory::Memzero(Out + Written, (NumSamples - Written) * sizeof(int16));
	}
	return NumSamples;
}
