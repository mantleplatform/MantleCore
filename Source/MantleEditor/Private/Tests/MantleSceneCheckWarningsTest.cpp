// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceValidator.h"

#include "Components/AudioComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Misc/App.h"
#include "Materials/Material.h"
#include "Misc/AutomationTest.h"
#include "RHIGlobals.h"
#include "Sound/AmbientSound.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundConcurrency.h"
#include "Sound/SoundWave.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// Scene-level Check warnings for what the place file can't carry (publish support list rows W4 and 126): a level checked or
// published without a renderer, and sounds that set a sound class volume or pitch, a concurrency limit or a reverb send.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleSceneCheckWarningsTestPrivate
{
	/** A transient editor world for one test, destroyed with it. */
	struct FSceneWorld
	{
		UWorld* World = nullptr;

		FSceneWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld*/ false, TEXT("MantleSceneCheckWarningsTestWorld"));
		}

		~FSceneWorld()
		{
			if (World)
			{
				World->DestroyWorld(/*bInformEngineOfWorld*/ false);
				if (World->IsRooted())
				{
					World->RemoveFromRoot();
				}
			}
		}

		AAmbientSound* AddSound(const TCHAR* Label, USoundBase* Sound, const FVector& Location) const
		{
			AAmbientSound* Ambient = World->SpawnActor<AAmbientSound>(Location, FRotator::ZeroRotator);
			if (Ambient)
			{
				Ambient->SetActorLabel(Label);
				Ambient->GetAudioComponent()->SetSound(Sound);
			}
			return Ambient;
		}
	};

	int32 CountWithText(const FMantleValidationResult& Result, const TCHAR* Text)
	{
		int32 Count = 0;
		for (const FMantleValidationIssue& Issue : Result.Issues)
		{
			Count += Issue.Severity == EMantleIssueSeverity::Warning && Issue.Problem.Contains(Text) ? 1 : 0;
		}
		return Count;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleNoRendererWarningTest,
	"Mantle.Editor.Check.NoRendererIsSaidOnceAtTheTop",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleNoRendererWarningTest::RunTest(const FString& Parameters)
{
	using namespace MantleSceneCheckWarningsTestPrivate;

	// The wording and the rule, whatever this process can render.
	FMantleValidationIssue Issue;
	TestFalse(TEXT("a level that uploads no material has nothing to say"), FMantlePlaceValidator::DescribeNoRenderer(0, Issue));
	if (TestTrue(TEXT("one material is said"), FMantlePlaceValidator::DescribeNoRenderer(1, Issue)))
	{
		TestTrue(TEXT("as a warning"), Issue.Severity == EMantleIssueSeverity::Warning);
		TestTrue(TEXT("for the whole place"), Issue.Actor.IsEmpty());
		TestTrue(TEXT("names the count in the singular"), Issue.Problem.Contains(TEXT("the 1 material this level uploads")));
		TestTrue(TEXT("and what the creator can do"), Issue.Remedy.Contains(TEXT("window open")));
	}
	if (TestTrue(TEXT("many materials are said"), FMantlePlaceValidator::DescribeNoRenderer(40, Issue)))
	{
		TestTrue(TEXT("names the count in the plural"), Issue.Problem.Contains(TEXT("the 40 materials this level uploads")));
	}

	// Through Validate, in a process that really has none (this run is headless): once, and first in the list.
	if (FApp::CanEverRender() && !GUsingNullRHI)
	{
		AddInfo(TEXT("This process has a renderer, so Validate says nothing about one; only the wording was checked."));
		return true;
	}
	FSceneWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("the engine cube"), Cube))
	{
		return false;
	}
	// The engine's cube is a library mesh, which brings its own material; a material of the project's own on it is what uploads.
	UMaterial* Material = NewObject<UMaterial>(GetTransientPackage(), TEXT("M_TestNoRenderer"));
	for (int32 Index = 0; Index < 3; ++Index)
	{
		AStaticMeshActor* Actor = Fixture.World->SpawnActor<AStaticMeshActor>(FVector(300.0 * Index, 0.0, 100.0), FRotator::ZeroRotator);
		if (!TestNotNull(TEXT("a cube actor"), Actor))
		{
			return false;
		}
		Actor->GetStaticMeshComponent()->SetStaticMesh(Cube);
		Actor->GetStaticMeshComponent()->SetMaterial(0, Material);
	}
	const FMantleValidationResult Result = FMantlePlaceValidator::Validate(Fixture.World);
	TestEqual(TEXT("the no-renderer warning appears once, however many materials"), CountWithText(Result, TEXT("has no renderer")), 1);
	if (TestTrue(TEXT("there are issues"), Result.Issues.Num() > 0))
	{
		TestTrue(TEXT("and it is the first"), Result.Issues[0].Problem.Contains(TEXT("has no renderer")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleSoundMixingWarningTest,
	"Mantle.Editor.Check.SoundClassConcurrencyAndReverbAreSaidToBeDropped",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleSoundMixingWarningTest::RunTest(const FString& Parameters)
{
	using namespace MantleSceneCheckWarningsTestPrivate;
	FSceneWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}

	USoundClass* Quiet = NewObject<USoundClass>(GetTransientPackage(), TEXT("SC_TestQuiet"));
	Quiet->Properties.Volume = 0.4f;
	USoundClass* Plain = NewObject<USoundClass>(GetTransientPackage(), TEXT("SC_TestPlain"));
	Plain->Properties.Volume = 1.0f;
	Plain->Properties.Pitch = 1.0f;
	// A class that is plain itself but sits under a quiet parent: its volume is the parent's.
	USoundClass* Child = NewObject<USoundClass>(GetTransientPackage(), TEXT("SC_TestChild"));
	Child->ParentClass = Quiet;

	USoundWave* InQuietClass = NewObject<USoundWave>(GetTransientPackage(), TEXT("W_TestQuiet"));
	InQuietClass->SoundClassObject = Quiet;
	USoundWave* InChildClass = NewObject<USoundWave>(GetTransientPackage(), TEXT("W_TestChild"));
	InChildClass->SoundClassObject = Child;
	USoundWave* Limited = NewObject<USoundWave>(GetTransientPackage(), TEXT("W_TestLimited"));
	Limited->ConcurrencySet.Add(NewObject<USoundConcurrency>(GetTransientPackage(), TEXT("SCON_Test")));
	USoundWave* InPlainClass = NewObject<USoundWave>(GetTransientPackage(), TEXT("W_TestPlain"));
	InPlainClass->SoundClassObject = Plain;
	USoundWave* NoClass = NewObject<USoundWave>(GetTransientPackage(), TEXT("W_TestNone"));

	Fixture.AddSound(TEXT("Quiet class sound"), InQuietClass, FVector(0.0, 0.0, 100.0));
	Fixture.AddSound(TEXT("Child of quiet class"), InChildClass, FVector(300.0, 0.0, 100.0));
	Fixture.AddSound(TEXT("Limited sound"), Limited, FVector(600.0, 0.0, 100.0));
	Fixture.AddSound(TEXT("Plain class sound"), InPlainClass, FVector(900.0, 0.0, 100.0));
	Fixture.AddSound(TEXT("Unclassed sound"), NoClass, FVector(1200.0, 0.0, 100.0));

	const FMantleValidationResult Result = FMantlePlaceValidator::Validate(Fixture.World);
	const FMantleValidationIssue* Found = Result.Issues.FindByPredicate([](const FMantleValidationIssue& Issue)
	{
		return Issue.Severity == EMantleIssueSeverity::Warning && Issue.Problem.Contains(TEXT("sound class volume or pitch"));
	});
	if (!TestNotNull(TEXT("Check says some sounds' mixing isn't published"), Found))
	{
		return false;
	}
	TestEqual(TEXT("as one place-wide warning"), CountWithText(Result, TEXT("sound class volume or pitch")), 1);
	TestTrue(TEXT("for the place, not one actor"), Found->Actor.IsEmpty());
	TestTrue(TEXT("counting the three sounds"), Found->Problem.StartsWith(TEXT("3 sounds")));
	TestTrue(TEXT("names the quiet class sound and its class"), Found->Problem.Contains(TEXT("'Quiet class sound'")) && Found->Problem.Contains(TEXT("SC_TestQuiet")));
	TestTrue(TEXT("names the one under a quiet parent"), Found->Problem.Contains(TEXT("'Child of quiet class'")));
	TestTrue(TEXT("names the concurrency limit"), Found->Problem.Contains(TEXT("'Limited sound' (a concurrency limit)")));
	TestFalse(TEXT("a class that changes nothing is left out"), Found->Problem.Contains(TEXT("Plain class sound")));
	TestFalse(TEXT("so is a sound with no class"), Found->Problem.Contains(TEXT("Unclassed sound")));
	TestTrue(TEXT("the remedy names the Volume Multiplier"), Found->Remedy.Contains(TEXT("Volume Multiplier")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
