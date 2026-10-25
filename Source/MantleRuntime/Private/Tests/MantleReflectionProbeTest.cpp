// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceBuilder.h"
#include "MantlePlaceLimits.h"
#include "MantlePlaceParser.h"
#include "MantleReflectionProbe.h"

#include "Components/BoxReflectionCaptureComponent.h"
#include "Components/ReflectionCaptureComponent.h"
#include "Components/SphereReflectionCaptureComponent.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// Reflection probes: what the parser reads and refuses, the pump's length, the transform and settings a capture is
// made with, and that spawning one registers a runtime capture component. The renderer cannot be run here: whether the
// cubemap is rendered, and how it looks, is the visual check in a packaged client (the MANTLE_PLACE_REFLECTION lines tell
// the captures were made and queued; only the picture says they rendered).

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleReflectionProbeTestPrivate
{
	/** A place whose entities are Entities (the text between the brackets). */
	FMantleParseResult ParseEntities(const FString& Entities)
	{
		const FString Json = FString::Printf(
			TEXT("{\"schemaVersion\":1,\"placeId\":\"plc_probes01\",\"revision\":{\"number\":1,\"state\":\"draft\"},"
				 "\"metadata\":{\"name\":\"Probes\",\"authorId\":\"usr_4b1d\"},\"settings\":{},\"assets\":[],\"entities\":[%s]}"),
			*Entities);
		return FMantlePlaceParser::ParseFromString(Json);
	}

	/** One probe entity (id ent_probe_<Number>) with Properties (the text between the braces). */
	FString ProbeEntity(int32 Number, const FString& Properties)
	{
		return FString::Printf(
			TEXT("{\"id\":\"ent_probe_%d\",\"type\":\"ReflectionProbe\",\"parent\":null,"
				 "\"transform\":{\"location\":[%d,0,0],\"rotation\":[0,0,0],\"scale\":[1,1,1]},\"properties\":{%s}}"),
			Number, Number * 100, *Properties);
	}

	FMantleParseResult ParseProbe(const FString& Properties)
	{
		return ParseEntities(ProbeEntity(0, Properties));
	}

	const FMantleReflectionProbeProps* ProbeOf(const FMantleParseResult& Result, int32 Index = 0)
	{
		return Result.Place.Entities.IsValidIndex(Index) ? Result.Place.Entities[Index].Props.TryGet<FMantleReflectionProbeProps>() : nullptr;
	}

	bool HasErrorAt(const FMantleParseResult& Result, const FString& Path)
	{
		return Result.Errors.ContainsByPredicate([&Path](const FMantleParseError& Error) { return Error.Path == Path; });
	}

	/** An empty game world for one test, destroyed with it. */
	struct FProbeWorld
	{
		UWorld* World = nullptr;

		FProbeWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("MantleReflectionProbeTestWorld"));
		}

		~FProbeWorld()
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
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleReflectionProbeParseTest,
	"Mantle.Runtime.ReflectionProbe.Parser.ReadsBoxAndSphere",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleReflectionProbeParseTest::RunTest(const FString& Parameters)
{
	using namespace MantleReflectionProbeTestPrivate;

	// Only the shape's own keys: every other takes the schema's default.
	const FMantleParseResult Box = ParseProbe(TEXT("\"shape\":\"box\",\"halfExtent\":[1000,600,300]"));
	if (!TestTrue(TEXT("a box with its half extent parses"), Box.bSuccess))
	{
		AddError(Box.ErrorSummary());
		return false;
	}
	const FMantleReflectionProbeProps* BoxProps = ProbeOf(Box);
	if (!TestNotNull(TEXT("a probe"), BoxProps))
	{
		return false;
	}
	TestTrue(TEXT("a box"), BoxProps->Shape == EMantleReflectionProbeShape::Box);
	TestTrue(TEXT("with its half extent"), BoxProps->HalfExtent.Equals(FVector(1000.0, 600.0, 300.0)));
	TestEqual(TEXT("transition 100"), BoxProps->TransitionDistance, 100.0);
	TestEqual(TEXT("brightness 1"), BoxProps->Brightness, 1.0);
	TestEqual(TEXT("sky light scale 0.1"), BoxProps->SkylightScale, 0.1);

	const FMantleParseResult Tuned = ParseProbe(TEXT(
		"\"shape\":\"box\",\"halfExtent\":[1,100000,50],\"transitionDistance\":1000,\"brightness\":8,\"skylightScale\":1"));
	TestTrue(TEXT("a box at the edges of every range parses"), Tuned.bSuccess);
	if (const FMantleReflectionProbeProps* Props = ProbeOf(Tuned))
	{
		TestEqual(TEXT("transition"), Props->TransitionDistance, 1000.0);
		TestEqual(TEXT("brightness"), Props->Brightness, 8.0);
		TestEqual(TEXT("sky light scale"), Props->SkylightScale, 1.0);
		TestEqual(TEXT("half extent Y"), Props->HalfExtent.Y, 100000.0);
	}

	const FMantleParseResult Sphere = ParseProbe(TEXT("\"shape\":\"sphere\",\"influenceRadius\":800,\"brightness\":0.5,\"skylightScale\":0"));
	if (!TestTrue(TEXT("a sphere parses"), Sphere.bSuccess))
	{
		AddError(Sphere.ErrorSummary());
		return false;
	}
	const FMantleReflectionProbeProps* SphereProps = ProbeOf(Sphere);
	if (!TestNotNull(TEXT("a sphere probe"), SphereProps))
	{
		return false;
	}
	TestTrue(TEXT("a sphere"), SphereProps->Shape == EMantleReflectionProbeShape::Sphere);
	TestEqual(TEXT("its radius"), SphereProps->InfluenceRadius, 800.0);
	TestEqual(TEXT("its brightness"), SphereProps->Brightness, 0.5);
	TestEqual(TEXT("its sky light scale"), SphereProps->SkylightScale, 0.0);

	const FMantleParseResult SmallestSphere = ParseProbe(TEXT("\"shape\":\"sphere\",\"influenceRadius\":8"));
	TestTrue(TEXT("the smallest sphere parses"), SmallestSphere.bSuccess);
	const FMantleParseResult LargestSphere = ParseProbe(TEXT("\"shape\":\"sphere\",\"influenceRadius\":16384"));
	TestTrue(TEXT("the largest sphere parses"), LargestSphere.bSuccess);

	TestEqual(TEXT("the type's name is ReflectionProbe"), FString(MantleEntityTypeToString(EMantleEntityType::ReflectionProbe)), FString(TEXT("ReflectionProbe")));
	EMantleEntityType Parsed = EMantleEntityType::Group;
	TestTrue(TEXT("and it parses back"), MantleParseEntityType(TEXT("ReflectionProbe"), Parsed) && Parsed == EMantleEntityType::ReflectionProbe);
	EMantleReflectionProbeShape Shape = EMantleReflectionProbeShape::Box;
	TestTrue(TEXT("a shape name parses, case-exact"), MantleParseReflectionProbeShape(TEXT("sphere"), Shape) && Shape == EMantleReflectionProbeShape::Sphere);
	TestFalse(TEXT("a shape in the wrong case does not"), MantleParseReflectionProbeShape(TEXT("Sphere"), Shape));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleReflectionProbeRefusalsTest,
	"Mantle.Runtime.ReflectionProbe.Parser.Refusals",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleReflectionProbeRefusalsTest::RunTest(const FString& Parameters)
{
	using namespace MantleReflectionProbeTestPrivate;

	// What is refused, and at which key: the paths the backend and validate.py report.
	struct FRefusal
	{
		const TCHAR* What;
		const TCHAR* Properties;
		const TCHAR* Path;
	};
	const FRefusal Refusals[] = {
		{ TEXT("an unknown shape"), TEXT("\"shape\":\"plane\",\"halfExtent\":[1000,600,300]"), TEXT("/entities/0/properties/shape") },
		{ TEXT("a shape in the wrong case"), TEXT("\"shape\":\"Box\",\"halfExtent\":[1000,600,300]"), TEXT("/entities/0/properties/shape") },
		{ TEXT("no shape"), TEXT("\"halfExtent\":[1000,600,300]"), TEXT("/entities/0/properties/shape") },
		{ TEXT("a box with no half extent"), TEXT("\"shape\":\"box\""), TEXT("/entities/0/properties/halfExtent") },
		{ TEXT("a sphere with no radius"), TEXT("\"shape\":\"sphere\""), TEXT("/entities/0/properties/influenceRadius") },
		{ TEXT("a box with a radius"), TEXT("\"shape\":\"box\",\"halfExtent\":[1000,600,300],\"influenceRadius\":500"), TEXT("/entities/0/properties/influenceRadius") },
		{ TEXT("a sphere with a half extent"), TEXT("\"shape\":\"sphere\",\"influenceRadius\":800,\"halfExtent\":[1000,600,300]"), TEXT("/entities/0/properties/halfExtent") },
		{ TEXT("a sphere with a transition distance"), TEXT("\"shape\":\"sphere\",\"influenceRadius\":800,\"transitionDistance\":100"), TEXT("/entities/0/properties/transitionDistance") },
		{ TEXT("a flat box"), TEXT("\"shape\":\"box\",\"halfExtent\":[1000,0,300]"), TEXT("/entities/0/properties/halfExtent/1") },
		{ TEXT("a box past 1 km"), TEXT("\"shape\":\"box\",\"halfExtent\":[100001,600,300]"), TEXT("/entities/0/properties/halfExtent/0") },
		{ TEXT("a half extent of two numbers"), TEXT("\"shape\":\"box\",\"halfExtent\":[1000,600]"), TEXT("/entities/0/properties/halfExtent") },
		{ TEXT("a radius under 8 cm"), TEXT("\"shape\":\"sphere\",\"influenceRadius\":7"), TEXT("/entities/0/properties/influenceRadius") },
		{ TEXT("a radius past 16384 cm"), TEXT("\"shape\":\"sphere\",\"influenceRadius\":16385"), TEXT("/entities/0/properties/influenceRadius") },
		{ TEXT("no transition"), TEXT("\"shape\":\"box\",\"halfExtent\":[1000,600,300],\"transitionDistance\":0"), TEXT("/entities/0/properties/transitionDistance") },
		{ TEXT("a transition past 1000 cm"), TEXT("\"shape\":\"box\",\"halfExtent\":[1000,600,300],\"transitionDistance\":1001"), TEXT("/entities/0/properties/transitionDistance") },
		{ TEXT("a negative brightness"), TEXT("\"shape\":\"sphere\",\"influenceRadius\":800,\"brightness\":-0.5"), TEXT("/entities/0/properties/brightness") },
		{ TEXT("a brightness past 8"), TEXT("\"shape\":\"sphere\",\"influenceRadius\":800,\"brightness\":9"), TEXT("/entities/0/properties/brightness") },
		{ TEXT("a string for the brightness"), TEXT("\"shape\":\"sphere\",\"influenceRadius\":800,\"brightness\":\"2\""), TEXT("/entities/0/properties/brightness") },
		{ TEXT("a sky light scale past 1"), TEXT("\"shape\":\"sphere\",\"influenceRadius\":800,\"skylightScale\":2"), TEXT("/entities/0/properties/skylightScale") },
		{ TEXT("an unknown property"), TEXT("\"shape\":\"sphere\",\"influenceRadius\":800,\"cubemap\":\"ast_cube\""), TEXT("/entities/0/properties/cubemap") },
	};
	for (const FRefusal& Refusal : Refusals)
	{
		const FMantleParseResult Result = ParseProbe(Refusal.Properties);
		TestFalse(FString::Printf(TEXT("%s is refused"), Refusal.What), Result.bSuccess);
		TestTrue(FString::Printf(TEXT("%s is refused at %s (got: %s)"), Refusal.What, Refusal.Path, *Result.ErrorSummary()), HasErrorAt(Result, Refusal.Path));
	}

	// A refused value leaves the default standing rather than a number outside the range.
	const FMantleParseResult Over = ParseProbe(TEXT("\"shape\":\"sphere\",\"influenceRadius\":16385,\"brightness\":9"));
	if (const FMantleReflectionProbeProps* Props = ProbeOf(Over))
	{
		TestTrue(TEXT("the radius stays in range"), Props->InfluenceRadius <= FMantlePlaceLimits::MaxReflectionProbeRadius);
		TestTrue(TEXT("and so does the brightness"), Props->Brightness <= FMantlePlaceLimits::MaxReflectionProbeBrightness);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleReflectionProbeBudgetTest,
	"Mantle.Runtime.ReflectionProbe.Parser.PerPlaceCap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleReflectionProbeBudgetTest::RunTest(const FString& Parameters)
{
	using namespace MantleReflectionProbeTestPrivate;
	const int32 Cap = FMantlePlaceLimits::MaxReflectionProbesPerPlace;
	TestEqual(TEXT("the cap is 64"), Cap, 64);

	auto PlaceOf = [](int32 Count)
	{
		TArray<FString> Entities;
		for (int32 Number = 0; Number < Count; ++Number)
		{
			Entities.Add(ProbeEntity(Number, TEXT("\"shape\":\"sphere\",\"influenceRadius\":800")));
		}
		return ParseEntities(FString::Join(Entities, TEXT(",")));
	};

	const FMantleParseResult AtCap = PlaceOf(Cap);
	TestTrue(TEXT("exactly the cap parses"), AtCap.bSuccess);
	TestEqual(TEXT("every probe is read"), AtCap.Place.Entities.Num(), Cap);

	const FMantleParseResult OverCap = PlaceOf(Cap + 1);
	TestFalse(TEXT("one more is refused"), OverCap.bSuccess);
	TestTrue(TEXT("at the type of the one that crosses it"),
		HasErrorAt(OverCap, FString::Printf(TEXT("/entities/%d/type"), Cap)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleReflectionProbePumpTest,
	"Mantle.Runtime.ReflectionProbe.PumpFrames",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleReflectionProbePumpTest::RunTest(const FString& Parameters)
{
	// Each probe takes ceil(6 / faces) updates and one more, two times that when the one-face timeslice runs every other frame,
	// and 4 frames of slack at the end.
	TestEqual(TEXT("no probes, no frames"), FMantleReflectionProbes::PumpFrames(0, 1, false), 0);
	TestEqual(TEXT("one probe, one face a frame"), FMantleReflectionProbes::PumpFrames(1, 1, false), 1 * 7 + 4);
	TestEqual(TEXT("three probes, two faces a frame"), FMantleReflectionProbes::PumpFrames(3, 2, false), 3 * 4 + 4);
	TestEqual(TEXT("two probes, six faces a frame"), FMantleReflectionProbes::PumpFrames(2, 6, false), 2 * 2 + 4);
	TestEqual(TEXT("slow mode doubles a one-face timeslice"), FMantleReflectionProbes::PumpFrames(1, 1, true), 1 * 7 * 2 + 4);
	TestEqual(TEXT("and does nothing for more faces"), FMantleReflectionProbes::PumpFrames(2, 2, true), 2 * 4 + 4);
	TestEqual(TEXT("a timeslice of 0 is read as 1"), FMantleReflectionProbes::PumpFrames(1, 0, false), 1 * 7 + 4);
	TestEqual(TEXT("and one past 6 as 6"), FMantleReflectionProbes::PumpFrames(1, 99, false), 1 * 2 + 4);
	TestEqual(TEXT("64 probes at the default timeslice"), FMantleReflectionProbes::PumpFrames(64, 1, false), 64 * 7 + 4);
	TestTrue(TEXT("the engine's own cvars give a positive length"), FMantleReflectionProbes::PumpFramesNow(5) > 0);
	TestTrue(TEXT("the sky light gets frames to settle first"), FMantleReflectionProbes::WarmupFrames >= 10);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleReflectionProbeCaptureTransformTest,
	"Mantle.Runtime.ReflectionProbe.CaptureTransformAndSettings",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleReflectionProbeCaptureTransformTest::RunTest(const FString& Parameters)
{
	FMantleReflectionProbeProps Box;
	Box.Shape = EMantleReflectionProbeShape::Box;
	Box.HalfExtent = FVector(1000.0, 600.0, 300.0);
	Box.TransitionDistance = 250.0;
	Box.Brightness = 1.5;
	Box.SkylightScale = 0.25;

	// The actor's scale plays no part: the box's size is its half extent.
	const FTransform Stand(FRotator(0.0, 30.0, 0.0), FVector(100.0, 200.0, 300.0), FVector(5.0, 5.0, 5.0));
	const FTransform BoxTransform = AMantleReflectionProbe::CaptureTransform(Box, Stand);
	TestTrue(TEXT("a box stands where the actor does"), BoxTransform.GetLocation().Equals(FVector(100.0, 200.0, 300.0)));
	TestTrue(TEXT("turned as it is"), BoxTransform.GetRotation().Equals(Stand.GetRotation()));
	TestTrue(TEXT("scaled by its half extent, not the actor's scale"), BoxTransform.GetScale3D().Equals(FVector(1000.0, 600.0, 300.0)));

	FMantleReflectionProbeProps Sphere;
	Sphere.Shape = EMantleReflectionProbeShape::Sphere;
	Sphere.InfluenceRadius = 800.0;
	Sphere.Brightness = 0.75;
	Sphere.SkylightScale = 0.0;
	TestTrue(TEXT("a sphere is not scaled: its radius is its own setting"),
		AMantleReflectionProbe::CaptureTransform(Sphere, Stand).GetScale3D().Equals(FVector::OneVector));

	UBoxReflectionCaptureComponent* BoxComponent = NewObject<UBoxReflectionCaptureComponent>(GetTransientPackage());
	AMantleReflectionProbe::ConfigureCapture(BoxComponent, Box);
	TestTrue(TEXT("a box capture is a runtime capture"), BoxComponent->IsRuntimeCapture());
	TestEqual(TEXT("with its brightness"), BoxComponent->Brightness, 1.5f);
	TestEqual(TEXT("its transition distance"), BoxComponent->BoxTransitionDistance, 250.0f);
	TestTrue(TEXT("and its sky light leak on every channel"),
		FMath::IsNearlyEqual(BoxComponent->RuntimeSkylightScale.R, 0.25f) && FMath::IsNearlyEqual(BoxComponent->RuntimeSkylightScale.B, 0.25f));

	USphereReflectionCaptureComponent* SphereComponent = NewObject<USphereReflectionCaptureComponent>(GetTransientPackage());
	AMantleReflectionProbe::ConfigureCapture(SphereComponent, Sphere);
	TestTrue(TEXT("a sphere capture is a runtime capture"), SphereComponent->IsRuntimeCapture());
	TestEqual(TEXT("with its influence radius"), SphereComponent->InfluenceRadius, 800.0f);
	TestEqual(TEXT("and its brightness"), SphereComponent->Brightness, 0.75f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleReflectionProbeSpawnTest,
	"Mantle.Runtime.ReflectionProbe.SpawnRegistersARuntimeCapture",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleReflectionProbeSpawnTest::RunTest(const FString& Parameters)
{
	using namespace MantleReflectionProbeTestPrivate;
	FProbeWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}

	AMantleReflectionProbe* Probe = Fixture.World->SpawnActor<AMantleReflectionProbe>(FVector(100.0, 200.0, 300.0), FRotator(0.0, 30.0, 0.0));
	if (!TestNotNull(TEXT("a probe actor"), Probe))
	{
		return false;
	}
	FMantleReflectionProbeProps Props;
	Props.Shape = EMantleReflectionProbeShape::Box;
	Props.HalfExtent = FVector(1000.0, 600.0, 300.0);
	Props.TransitionDistance = 250.0;
	Props.Brightness = 1.5;
	Probe->SetProbeProps(Props);
	TestNull(TEXT("no capture before it is asked for"), Probe->GetCapture());

	TestTrue(TEXT("a capture is made"), Probe->SpawnCapture());
	UReflectionCaptureComponent* Capture = Probe->GetCapture();
	if (!TestNotNull(TEXT("the capture component"), Capture))
	{
		return false;
	}
	TestTrue(TEXT("it is a box capture"), Capture->IsA(UBoxReflectionCaptureComponent::StaticClass()));
	TestTrue(TEXT("flagged as a runtime capture, set before it registered"), Capture->IsRuntimeCapture());
	TestTrue(TEXT("registered"), Capture->IsRegistered());
	TestTrue(TEXT("owned by the probe, so the renderer takes its capture position from the probe's location"), Capture->GetOwner() == Probe);
	TestTrue(TEXT("at the probe's location"), Capture->GetComponentLocation().Equals(FVector(100.0, 200.0, 300.0), 0.01));
	TestTrue(TEXT("turned as the probe is"), Capture->GetComponentRotation().Equals(FRotator(0.0, 30.0, 0.0), 0.01));
	TestTrue(TEXT("its scale is the half extent"), Capture->GetComponentScale().Equals(FVector(1000.0, 600.0, 300.0), 0.01));
	TestEqual(TEXT("its brightness"), Capture->Brightness, 1.5f);
	TestEqual(TEXT("its transition distance"), Cast<UBoxReflectionCaptureComponent>(Capture)->BoxTransitionDistance, 250.0f);
	TestFalse(TEXT("a second capture is not made"), Probe->SpawnCapture());
	TestTrue(TEXT("and the first stays"), Probe->GetCapture() == Capture);

	AMantleReflectionProbe* SphereProbe = Fixture.World->SpawnActor<AMantleReflectionProbe>(FVector(0.0, 0.0, 500.0), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("a sphere probe actor"), SphereProbe))
	{
		return false;
	}
	FMantleReflectionProbeProps SphereProps;
	SphereProps.Shape = EMantleReflectionProbeShape::Sphere;
	SphereProps.InfluenceRadius = 1200.0;
	SphereProbe->SetProbeProps(SphereProps);
	TestTrue(TEXT("a sphere capture is made"), SphereProbe->SpawnCapture());
	USphereReflectionCaptureComponent* SphereCapture = Cast<USphereReflectionCaptureComponent>(SphereProbe->GetCapture());
	if (TestNotNull(TEXT("it is a sphere capture"), SphereCapture))
	{
		TestTrue(TEXT("a runtime one"), SphereCapture->IsRuntimeCapture());
		TestEqual(TEXT("with its influence radius"), SphereCapture->InfluenceRadius, 1200.0f);
		TestTrue(TEXT("registered"), SphereCapture->IsRegistered());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleReflectionProbeBuilderTest,
	"Mantle.Runtime.ReflectionProbe.Builder.KeepsTheEntityAsAnActor",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleReflectionProbeBuilderTest::RunTest(const FString& Parameters)
{
	using namespace MantleReflectionProbeTestPrivate;
	FProbeWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	const FMantleParseResult Parsed = ParseEntities(
		ProbeEntity(0, TEXT("\"shape\":\"box\",\"halfExtent\":[1000,600,300],\"brightness\":2")) + TEXT(",")
		+ ProbeEntity(1, TEXT("\"shape\":\"sphere\",\"influenceRadius\":900")));
	if (!TestTrue(TEXT("the place parses"), Parsed.bSuccess))
	{
		AddError(Parsed.ErrorSummary());
		return false;
	}

	FMantleBuildOptions Options;
	Options.bRuntimeEnvironment = false;
	FMantleBuildResult Built = FMantlePlaceBuilder::Build(Fixture.World, Parsed.Place, Options);
	TestTrue(TEXT("it builds"), Built.bSuccess);
	TestEqual(TEXT("an actor for each probe, whatever this process draws"), Built.Actors.Num(), 2);

	AActor* const* BoxActor = Built.Actors.Find(TEXT("ent_probe_0"));
	const AMantleReflectionProbe* BoxProbe = BoxActor ? Cast<AMantleReflectionProbe>(*BoxActor) : nullptr;
	if (TestNotNull(TEXT("the box probe is an AMantleReflectionProbe"), BoxProbe))
	{
		TestTrue(TEXT("a box"), BoxProbe->GetProbeProps().Shape == EMantleReflectionProbeShape::Box);
		TestTrue(TEXT("with its half extent"), BoxProbe->GetProbeProps().HalfExtent.Equals(FVector(1000.0, 600.0, 300.0)));
		TestEqual(TEXT("and its brightness"), BoxProbe->GetProbeProps().Brightness, 2.0);
		TestTrue(TEXT("standing at its place's origin"), BoxProbe->GetActorLocation().Equals(FVector(0.0, 0.0, 0.0)));
		TestNull(TEXT("with no capture yet: the subsystem makes them once the place has had its warm-up"), BoxProbe->GetCapture());
	}
	AActor* const* SphereActor = Built.Actors.Find(TEXT("ent_probe_1"));
	if (TestTrue(TEXT("the sphere probe has an actor"), SphereActor != nullptr))
	{
		TestTrue(TEXT("standing at its place"), (*SphereActor)->GetActorLocation().Equals(FVector(100.0, 0.0, 0.0)));
	}

	FMantlePlaceBuilder::Teardown(Built);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
