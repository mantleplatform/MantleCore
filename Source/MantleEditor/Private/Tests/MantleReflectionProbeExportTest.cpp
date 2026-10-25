// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCoverage.h"
#include "MantlePlaceExporter.h"
#include "MantlePlaceLimits.h"
#include "MantlePlaceParser.h"
#include "MantlePlaceValidator.h"
#include "MantleReflectionProbeExport.h"
#include "MantleTargetFeatures.h"

#include "Components/BoxReflectionCaptureComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SphereReflectionCaptureComponent.h"
#include "Engine/BoxReflectionCapture.h"
#include "Engine/SphereReflectionCapture.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// A placed Box or Sphere Reflection Capture is a ReflectionProbe entity: its size and look read from the capture,
// its transform at scale 1 with the capture offset folded in, left out with a line where the target cannot read probes or
// the place is full, and carried through the place file. Built in a transient editor world; the capture itself (what the
// renderer makes of it) is the runtime tests' and the visual check's.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleReflectionProbeExportTestPrivate
{
	using EOutcome = EMantleCoverageOutcome;

	/** An empty editor world for one test, destroyed with it. */
	struct FProbeExportWorld
	{
		UWorld* World = nullptr;

		FProbeExportWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld*/ false, TEXT("MantleReflectionProbeExportTestWorld"));
		}

		~FProbeExportWorld()
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

	ABoxReflectionCapture* SpawnBox(UWorld* World, const TCHAR* Label, const FVector& Location, const FRotator& Rotation, const FVector& HalfExtent)
	{
		ABoxReflectionCapture* Capture = World->SpawnActor<ABoxReflectionCapture>(Location, Rotation);
		if (Capture)
		{
			Capture->SetActorScale3D(HalfExtent);
			Capture->SetActorLabel(Label);
		}
		return Capture;
	}

	const FMantleEntity* FindEntity(const FMantlePlace& Place, EMantleEntityType Type, int32 Index = 0)
	{
		for (const FMantleEntity& Entity : Place.Entities)
		{
			if (Entity.Type == Type && Index-- == 0)
			{
				return &Entity;
			}
		}
		return nullptr;
	}

	int32 CountEntities(const FMantlePlace& Place, EMantleEntityType Type)
	{
		int32 Count = 0;
		for (const FMantleEntity& Entity : Place.Entities)
		{
			Count += Entity.Type == Type ? 1 : 0;
		}
		return Count;
	}

	bool HasExample(const FMantleCoverage& Coverage, EOutcome Outcome, const FString& Contains)
	{
		const FMantleCoverageCategory* Category = Coverage.Find(MantleCoverageCategory::Lights);
		return Category && Category->Get(Outcome).Examples.ContainsByPredicate(
			[&Contains](const FMantleCoverageExample& Example) { return Example.Detail.Contains(Contains) || Example.Name.Contains(Contains); });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleReflectionProbeExportBoxTest,
	"Mantle.Editor.ReflectionProbe.Export.BoxCaptureBecomesABoxProbe",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleReflectionProbeExportBoxTest::RunTest(const FString& Parameters)
{
	using namespace MantleReflectionProbeExportTestPrivate;
	FProbeExportWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	ABoxReflectionCapture* Capture = SpawnBox(Fixture.World, TEXT("Hall capture"), FVector(100.0, 200.0, 300.0), FRotator(0.0, 45.0, 0.0), FVector(1500.0, 800.0, 300.0));
	if (!TestNotNull(TEXT("the capture"), Capture))
	{
		return false;
	}
	UBoxReflectionCaptureComponent* Box = CastChecked<UBoxReflectionCaptureComponent>(Capture->GetCaptureComponent());
	Box->BoxTransitionDistance = 250.0f;
	Box->Brightness = 2.0f;
	Box->RuntimeSkylightScale = FLinearColor(0.25f, 0.25f, 0.25f);
	TestTrue(TEXT("a box capture is a probe actor"), FMantleReflectionProbeExport::IsProbeActor(Capture));
	TestTrue(TEXT("which the exporter classifies as a ReflectionProbe"), [Capture]()
	{
		EMantleEntityType Type = EMantleEntityType::Group;
		return FMantlePlaceExporter::ClassifyActor(Capture, Type) && Type == EMantleEntityType::ReflectionProbe;
	}());
	TestFalse(TEXT("and not as engine infrastructure"), FMantlePlaceExporter::IsEngineInfrastructure(Capture));

	const FMantleExportResult Result = FMantlePlaceExporter::Export(Fixture.World, FMantleExportOptions());
	TestFalse(TEXT("export does not refuse it"), Result.Errors.ContainsByPredicate([](const FMantleParseError& Error) { return Error.Path.Contains(TEXT("Capture")); }));
	const FMantleEntity* Entity = FindEntity(Result.Place, EMantleEntityType::ReflectionProbe);
	if (!TestNotNull(TEXT("a ReflectionProbe entity"), Entity))
	{
		return false;
	}
	const FMantleReflectionProbeProps* Props = Entity->Props.TryGet<FMantleReflectionProbeProps>();
	if (!TestNotNull(TEXT("with probe properties"), Props))
	{
		return false;
	}
	TestTrue(TEXT("a box"), Props->Shape == EMantleReflectionProbeShape::Box);
	TestTrue(TEXT("its half extent is the capture's scale"), Props->HalfExtent.Equals(FVector(1500.0, 800.0, 300.0), 0.01));
	TestEqual(TEXT("its transition distance"), Props->TransitionDistance, 250.0);
	TestEqual(TEXT("its brightness"), Props->Brightness, 2.0);
	TestEqual(TEXT("its sky light scale"), Props->SkylightScale, 0.25);
	TestTrue(TEXT("it stands where the capture does"), Entity->Transform.GetLocation().Equals(FVector(100.0, 200.0, 300.0), 0.01));
	TestTrue(TEXT("turned as it is"), Entity->Transform.Rotator().Equals(FRotator(0.0, 45.0, 0.0), 0.01));
	TestTrue(TEXT("at scale 1, the size being in the properties"), Entity->Transform.GetScale3D().Equals(FVector::OneVector, 0.0001));
	TestEqual(TEXT("it is named by its label"), Entity->Name, FString(TEXT("Hall capture")));
	TestTrue(TEXT("and is not dynamic"), !Entity->bDynamic);

	TestEqual(TEXT("one Kept line for it"), Result.Coverage.Count(MantleCoverageCategory::Lights, EOutcome::Kept), static_cast<int64>(1));
	TestEqual(TEXT("nothing approximated"), Result.Coverage.Count(MantleCoverageCategory::Lights, EOutcome::Approximated), static_cast<int64>(0));
	TestEqual(TEXT("and nothing dropped"), Result.Coverage.Count(MantleCoverageCategory::Lights, EOutcome::Dropped), static_cast<int64>(0));

	// Check this place: no error, whatever its preview boxes are.
	const FMantleValidationResult Validation = FMantlePlaceValidator::Validate(Fixture.World);
	TestFalse(TEXT("Check raises no error for it"), Validation.Issues.ContainsByPredicate([](const FMantleValidationIssue& Issue)
	{
		return Issue.Actor == TEXT("Hall capture") && Issue.Severity == EMantleIssueSeverity::Error;
	}));
	TestFalse(TEXT("and no warning that it drops a component"), Validation.Issues.ContainsByPredicate([](const FMantleValidationIssue& Issue)
	{
		return Issue.Actor == TEXT("Hall capture") && Issue.Problem.Contains(TEXT("components Mantle drops"));
	}));

	// Through the place file and back.
	const FString Json = FMantlePlaceExporter::ToJson(Result.Place);
	TestTrue(TEXT("the file names the type"), Json.Contains(TEXT("\"ReflectionProbe\"")));
	const FMantleParseResult Parsed = FMantlePlaceParser::ParseFromString(Json);
	if (TestTrue(TEXT("the file parses as a place"), Parsed.bSuccess))
	{
		const FMantleEntity* Back = FindEntity(Parsed.Place, EMantleEntityType::ReflectionProbe);
		const FMantleReflectionProbeProps* BackProps = Back ? Back->Props.TryGet<FMantleReflectionProbeProps>() : nullptr;
		if (TestNotNull(TEXT("the probe survives it"), BackProps))
		{
			TestTrue(TEXT("with its half extent"), BackProps->HalfExtent.Equals(Props->HalfExtent, 0.001));
			TestEqual(TEXT("its transition distance"), BackProps->TransitionDistance, Props->TransitionDistance);
			TestEqual(TEXT("its brightness"), BackProps->Brightness, Props->Brightness);
			TestEqual(TEXT("its sky light scale"), BackProps->SkylightScale, Props->SkylightScale);
		}
	}
	else
	{
		AddError(Parsed.ErrorSummary());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleReflectionProbeExportSphereTest,
	"Mantle.Editor.ReflectionProbe.Export.SphereCaptureBecomesASphereProbe",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleReflectionProbeExportSphereTest::RunTest(const FString& Parameters)
{
	using namespace MantleReflectionProbeExportTestPrivate;
	FProbeExportWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	ASphereReflectionCapture* Capture = Fixture.World->SpawnActor<ASphereReflectionCapture>(FVector(0.0, 3000.0, 400.0), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("the capture"), Capture))
	{
		return false;
	}
	Capture->SetActorLabel(TEXT("Atrium capture"));
	USphereReflectionCaptureComponent* Sphere = CastChecked<USphereReflectionCaptureComponent>(Capture->GetCaptureComponent());
	Sphere->InfluenceRadius = 1200.0f;
	Sphere->Brightness = 0.75f;
	// An offset moves where the probe is captured from, so it is folded into the probe's position.
	Sphere->CaptureOffset = FVector(0.0, 0.0, 120.0);

	const FMantleExportResult Result = FMantlePlaceExporter::Export(Fixture.World, FMantleExportOptions());
	const FMantleEntity* Entity = FindEntity(Result.Place, EMantleEntityType::ReflectionProbe);
	if (!TestNotNull(TEXT("a ReflectionProbe entity"), Entity))
	{
		return false;
	}
	const FMantleReflectionProbeProps* Props = Entity->Props.TryGet<FMantleReflectionProbeProps>();
	if (!TestNotNull(TEXT("with probe properties"), Props))
	{
		return false;
	}
	TestTrue(TEXT("a sphere"), Props->Shape == EMantleReflectionProbeShape::Sphere);
	TestEqual(TEXT("its influence radius"), Props->InfluenceRadius, 1200.0);
	TestEqual(TEXT("its brightness"), Props->Brightness, 0.75);
	TestTrue(TEXT("the capture offset is folded into its position"), Entity->Transform.GetLocation().Equals(FVector(0.0, 3000.0, 520.0), 0.01));
	TestTrue(TEXT("at scale 1"), Entity->Transform.GetScale3D().Equals(FVector::OneVector, 0.0001));
	TestEqual(TEXT("one Approximated line, for the offset"), Result.Coverage.Count(MantleCoverageCategory::Lights, EOutcome::Approximated), static_cast<int64>(1));
	TestTrue(TEXT("which says so"), HasExample(Result.Coverage, EOutcome::Approximated, TEXT("capture offset")));

	// The JSON of a sphere carries its radius and none of a box's keys.
	const FString Json = FMantlePlaceExporter::ToJson(Result.Place);
	TestTrue(TEXT("the file has the radius"), Json.Contains(TEXT("\"influenceRadius\"")));
	TestFalse(TEXT("and no half extent"), Json.Contains(TEXT("\"halfExtent\"")));
	TestFalse(TEXT("and no transition distance"), Json.Contains(TEXT("\"transitionDistance\"")));
	TestTrue(TEXT("and parses as a place"), FMantlePlaceParser::ParseFromString(Json).bSuccess);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleReflectionProbeExportApproximationsTest,
	"Mantle.Editor.ReflectionProbe.Export.ApproximationsAreReported",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleReflectionProbeExportApproximationsTest::RunTest(const FString& Parameters)
{
	using namespace MantleReflectionProbeExportTestPrivate;
	FProbeExportWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}

	// A third-party level never fails export for a capture a place can approximate: a size outside the range is clamped.
	ABoxReflectionCapture* Huge = SpawnBox(Fixture.World, TEXT("Huge"), FVector::ZeroVector, FRotator::ZeroRotator, FVector(0.5, 200000.0, 100.0));
	// A capture that specified its own cubemap is captured at runtime instead.
	ABoxReflectionCapture* Cube = SpawnBox(Fixture.World, TEXT("Cubemap"), FVector(5000.0, 0.0, 0.0), FRotator::ZeroRotator, FVector(500.0, 500.0, 300.0));
	if (!TestNotNull(TEXT("the captures"), Huge) || !TestNotNull(TEXT("and the other"), Cube))
	{
		return false;
	}
	Cube->GetCaptureComponent()->ReflectionSourceType = EReflectionSourceType::SpecifiedCubemap;

	FMantleReflectionProbeProps Read;
	TArray<FString> Notes;
	FMantleReflectionProbeExport::Read(Huge, Read, Notes);
	TestTrue(TEXT("the half extent is clamped into the range"), Read.HalfExtent.Equals(FVector(1.0, 100000.0, 100.0), 0.01));
	TestEqual(TEXT("and each change is a note"), Notes.Num(), 2);

	const FMantleExportResult Result = FMantlePlaceExporter::Export(Fixture.World, FMantleExportOptions());
	TestEqual(TEXT("both publish"), CountEntities(Result.Place, EMantleEntityType::ReflectionProbe), 2);
	TestEqual(TEXT("both are approximated"), Result.Coverage.Count(MantleCoverageCategory::Lights, EOutcome::Approximated), static_cast<int64>(2));
	TestTrue(TEXT("one says it was clamped"), HasExample(Result.Coverage, EOutcome::Approximated, TEXT("outside what a place holds")));
	TestTrue(TEXT("and one that it is captured at runtime with the place's own lighting"),
		HasExample(Result.Coverage, EOutcome::Approximated, TEXT("captured at runtime with the place's own lighting")));

	// Check this place warns of the clamp, and raises no error.
	const FMantleValidationResult Validation = FMantlePlaceValidator::Validate(Fixture.World);
	TestTrue(TEXT("Check warns of the clamped size"), Validation.Issues.ContainsByPredicate([](const FMantleValidationIssue& Issue)
	{
		return Issue.Actor == TEXT("Huge") && Issue.Severity == EMantleIssueSeverity::Warning;
	}));
	TestFalse(TEXT("and no error for either capture"), Validation.Issues.ContainsByPredicate([](const FMantleValidationIssue& Issue)
	{
		return (Issue.Actor == TEXT("Huge") || Issue.Actor == TEXT("Cubemap")) && Issue.Severity == EMantleIssueSeverity::Error;
	}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleReflectionProbeExportParentTest,
	"Mantle.Editor.ReflectionProbe.Export.UnderAGroupIsRelative",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleReflectionProbeExportParentTest::RunTest(const FString& Parameters)
{
	using namespace MantleReflectionProbeExportTestPrivate;
	FProbeExportWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	AActor* Group = Fixture.World->SpawnActor<AActor>(FVector(1000.0, 0.0, 0.0), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("a group"), Group))
	{
		return false;
	}
	USceneComponent* Root = NewObject<USceneComponent>(Group, TEXT("Root"));
	Group->SetRootComponent(Root);
	// Static, like the capture: the engine will not attach a Static component to a Movable one.
	Root->SetMobility(EComponentMobility::Static);
	Root->RegisterComponent();
	Group->SetActorLocation(FVector(1000.0, 0.0, 0.0));
	Group->SetActorLabel(TEXT("Lift"));

	ABoxReflectionCapture* Capture = SpawnBox(Fixture.World, TEXT("Lift capture"), FVector(1000.0, 0.0, 150.0), FRotator::ZeroRotator, FVector(200.0, 200.0, 150.0));
	if (!TestNotNull(TEXT("the capture"), Capture))
	{
		return false;
	}
	Capture->AttachToActor(Group, FAttachmentTransformRules::KeepWorldTransform);

	const FMantleExportResult Result = FMantlePlaceExporter::Export(Fixture.World, FMantleExportOptions());
	const FMantleEntity* Entity = FindEntity(Result.Place, EMantleEntityType::ReflectionProbe);
	const FMantleEntity* GroupEntity = FindEntity(Result.Place, EMantleEntityType::Group);
	if (!TestNotNull(TEXT("the probe"), Entity) || !TestNotNull(TEXT("and its group"), GroupEntity))
	{
		return false;
	}
	TestEqual(TEXT("it is parented to the group"), Entity->Parent, GroupEntity->Id);
	TestTrue(TEXT("at a location relative to it"), Entity->Transform.GetLocation().Equals(FVector(0.0, 0.0, 150.0), 0.01));
	TestTrue(TEXT("at scale 1"), Entity->Transform.GetScale3D().Equals(FVector::OneVector, 0.0001));
	if (const FMantleReflectionProbeProps* Props = Entity->Props.TryGet<FMantleReflectionProbeProps>())
	{
		TestTrue(TEXT("its half extent is untouched by the parent"), Props->HalfExtent.Equals(FVector(200.0, 200.0, 150.0), 0.01));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleReflectionProbeExportLimitsTest,
	"Mantle.Editor.ReflectionProbe.Export.TargetAndCapAreDropsWithALine",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleReflectionProbeExportLimitsTest::RunTest(const FString& Parameters)
{
	using namespace MantleReflectionProbeExportTestPrivate;
	FProbeExportWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	const int32 Cap = FMantlePlaceLimits::MaxReflectionProbesPerPlace;
	for (int32 Number = 0; Number < Cap + 3; ++Number)
	{
		SpawnBox(Fixture.World, *FString::Printf(TEXT("Probe %d"), Number), FVector(Number * 3000.0, 0.0, 0.0), FRotator::ZeroRotator, FVector(500.0, 500.0, 300.0));
	}

	// A target client that reads probes: the first 64 publish, the rest are said to be left out.
	{
		FMantleTargetFeaturesScope Scope(FMantleTargetFeatures::All());
		const FMantleExportResult Result = FMantlePlaceExporter::Export(Fixture.World, FMantleExportOptions());
		TestEqual(TEXT("the cap's worth of probes publish"), CountEntities(Result.Place, EMantleEntityType::ReflectionProbe), Cap);
		TestEqual(TEXT("each is a Kept line"), Result.Coverage.Count(MantleCoverageCategory::Lights, EOutcome::Kept), static_cast<int64>(Cap));
		TestEqual(TEXT("and the three past it are dropped"), Result.Coverage.Count(MantleCoverageCategory::Lights, EOutcome::Dropped), static_cast<int64>(3));
		TestTrue(TEXT("with the reason"), HasExample(Result.Coverage, EOutcome::Dropped, TEXT("at most 64 reflection probes")));
		TestFalse(TEXT("and no export error"), Result.Errors.ContainsByPredicate([](const FMantleParseError& Error) { return Error.Path.Contains(TEXT("Probe")); }));
		const FMantleValidationResult Validation = FMantlePlaceValidator::Validate(Fixture.World);
		TestTrue(TEXT("Check warns of the one past the cap"), Validation.Issues.ContainsByPredicate([](const FMantleValidationIssue& Issue)
		{
			return Issue.Severity == EMantleIssueSeverity::Warning && Issue.Problem.Contains(TEXT("at most 64"));
		}));
	}

	// A target client that does not read probes: none publish, and the line says why.
	{
		FMantleTargetFeaturesScope Scope(FMantleTargetFeatures::None());
		const FMantleExportResult Result = FMantlePlaceExporter::Export(Fixture.World, FMantleExportOptions());
		TestEqual(TEXT("no probe is written"), CountEntities(Result.Place, EMantleEntityType::ReflectionProbe), 0);
		TestEqual(TEXT("each is a Dropped line"), Result.Coverage.Count(MantleCoverageCategory::Lights, EOutcome::Dropped), static_cast<int64>(Cap + 3));
		TestTrue(TEXT("saying it needs a newer client"), HasExample(Result.Coverage, EOutcome::Dropped, FMantleTargetFeatures::NeedsNewerClient));
		TestFalse(TEXT("and it is no error"), Result.Errors.ContainsByPredicate([](const FMantleParseError& Error) { return Error.Path.Contains(TEXT("Probe")); }));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
