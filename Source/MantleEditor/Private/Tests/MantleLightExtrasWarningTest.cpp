// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceValidator.h"

#include "Components/SpotLightComponent.h"
#include "Engine/SpotLight.h"
#include "Engine/World.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// A spot light with a light function material (the publish test level's A19_SpotWithLightFunction) published plain with no
// word: the exporter reads a light's color, brightness, reach, cones, shadow and source shape, never its light function or IES
// profile. Check now says so for the light that has one, and stays quiet for a plain light.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleLightExtrasWarningTestPrivate
{
	/** A transient editor world for one test, destroyed with it. */
	struct FLightWorld
	{
		UWorld* World = nullptr;

		FLightWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld*/ false, TEXT("MantleLightExtrasTestWorld"));
		}

		~FLightWorld()
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

		ASpotLight* AddSpot(const TCHAR* Label, const FVector& Location) const
		{
			ASpotLight* Spot = World->SpawnActor<ASpotLight>(ASpotLight::StaticClass(), FTransform(Location));
			if (Spot)
			{
				Spot->SetActorLabel(Label);
			}
			return Spot;
		}
	};

	bool SaysLightFunction(const FMantleValidationResult& Result, const TCHAR* Label)
	{
		return Result.Issues.ContainsByPredicate([Label](const FMantleValidationIssue& Issue)
		{
			return Issue.Actor == Label && Issue.Severity == EMantleIssueSeverity::Warning && Issue.Problem.Contains(TEXT("light function material"))
				&& Issue.Problem.Contains(TEXT("doesn't publish"));
		});
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleLightFunctionWarningTest,
	"Mantle.Editor.Check.LightFunctionIsSaidToBeDropped",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleLightFunctionWarningTest::RunTest(const FString& Parameters)
{
	using namespace MantleLightExtrasWarningTestPrivate;
	FLightWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}

	UMaterial* Function = NewObject<UMaterial>(GetTransientPackage(), TEXT("M_TestLightFunction"));
	Function->MaterialDomain = MD_LightFunction;

	ASpotLight* Patterned = Fixture.AddSpot(TEXT("Spot with light function"), FVector(-200.0, 0.0, 400.0));
	ASpotLight* Plain = Fixture.AddSpot(TEXT("Plain spot"), FVector(200.0, 0.0, 400.0));
	if (!TestNotNull(TEXT("the patterned spot"), Patterned) || !TestNotNull(TEXT("the plain spot"), Plain))
	{
		return false;
	}
	Patterned->SpotLightComponent->LightFunctionMaterial = Function;

	const FMantleValidationResult Result = FMantlePlaceValidator::Validate(Fixture.World);
	TestTrue(TEXT("Check says the light function is not published"), SaysLightFunction(Result, TEXT("Spot with light function")));
	TestTrue(TEXT("and names the material"), Result.Issues.ContainsByPredicate([](const FMantleValidationIssue& Issue)
	{
		return Issue.Actor == TEXT("Spot with light function") && Issue.Problem.Contains(TEXT("'M_TestLightFunction'"));
	}));
	TestFalse(TEXT("a plain spot gets no such warning"), SaysLightFunction(Result, TEXT("Plain spot")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
