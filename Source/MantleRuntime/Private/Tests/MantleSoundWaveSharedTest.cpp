// Copyright Joshua Eger. All Rights Reserved.
#include "MantleSoundWave.h"

#include "Misc/AutomationTest.h"
#include "Sound/SoundGenerator.h"

#if WITH_DEV_AUTOMATION_TESTS

// A weapon's uploaded attack sound is one wave that every shot of every copy of the weapon plays at once. Each voice of a
// shared wave reads the samples on its own and ends after the last one; a plain wave (an emitter's) makes no generator and
// keeps playing through its own OnGeneratePCMAudio.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleSoundWaveSharedTest,
	"Mantle.Sound.SharedOneShot.EachVoiceReadsOnItsOwnAndEnds",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleSoundWaveSharedTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FMantleSoundData> Sound = MakeShared<FMantleSoundData>();
	Sound->SampleRate = 48000;
	Sound->Channels = 1;
	for (int32 Index = 0; Index < 300; ++Index)
	{
		Sound->Samples.Add(static_cast<int16>(Index * 100));
	}

	UMantleSoundWave* Shared = UMantleSoundWave::CreateShared(GetTransientPackage(), Sound);
	UMantleSoundWave* Plain = UMantleSoundWave::Create(GetTransientPackage(), Sound, /*bInLoop*/ false);
	if (!TestNotNull(TEXT("a shared wave is made"), Shared) || !TestNotNull(TEXT("a plain wave is made"), Plain))
	{
		return false;
	}
	TestTrue(TEXT("it says it is shared"), Shared->IsShared() && !Plain->IsShared());
	TestFalse(TEXT("it is a one-shot"), Shared->LoopsSamples());
	TestFalse(TEXT("a plain wave makes no generator"), Plain->CreateSoundGenerator(FSoundGeneratorInitParams()).IsValid());

	const ISoundGeneratorPtr First = Shared->CreateSoundGenerator(FSoundGeneratorInitParams());
	const ISoundGeneratorPtr Second = Shared->CreateSoundGenerator(FSoundGeneratorInitParams());
	if (!TestTrue(TEXT("each voice gets its own generator"), First.IsValid() && Second.IsValid() && First != Second))
	{
		return false;
	}
	TestEqual(TEXT("mono"), First->GetNumChannels(), 1);
	TArray<float> Buffer;
	Buffer.SetNumZeroed(256);
	First->OnGenerateAudio(Buffer.GetData(), 256);
	TestEqual(TEXT("the first voice starts at the first sample"), Buffer[1], 100.f / 32768.f);
	TestFalse(TEXT("and is not finished part way"), First->IsFinished());
	Second->OnGenerateAudio(Buffer.GetData(), 256);
	TestEqual(TEXT("a second voice starts at the beginning too, not where the first got to"), Buffer[1], 100.f / 32768.f);
	First->OnGenerateAudio(Buffer.GetData(), 256);
	TestEqual(TEXT("the first voice carries on from its own place"), Buffer[0], 25600.f / 32768.f);
	TestEqual(TEXT("and pads its last buffer with silence"), Buffer[100], 0.f);
	TestTrue(TEXT("then it is finished, by itself"), First->IsFinished());
	TestFalse(TEXT("while the second is not"), Second->IsFinished());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
