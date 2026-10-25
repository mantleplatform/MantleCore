// Copyright Joshua Eger. All Rights Reserved.
#include "MantleActorDecompose.h"
#include "MantleCoverage.h"
#include "MantlePlaceExporter.h"
#include "MantlePlaceValidator.h"

#include "Components/BillboardComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/RectLightComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SpotLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/BoxReflectionCapture.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "GameFramework/RotatingMovementComponent.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// Decomposing a multi-component actor: a Blueprint prefab becomes a Group with one entity per component, and
// whatever is not carried is reported, never an error: lights keep their units, editor-only components are ignored,
// an unknown component is dropped with a line, a reflection capture is an Info line, a Blueprint's behaviour is one
// Dropped line per class.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleActorDecomposeTestPrivate
{
	using EOutcome = EMantleCoverageOutcome;

	const TCHAR* const CubePath = TEXT("/Engine/BasicShapes/Cube.Cube");

	/** An empty editor world for one test, destroyed with it. */
	struct FDecomposeWorld
	{
		UWorld* World = nullptr;

		FDecomposeWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld*/ false, TEXT("MantleActorDecomposeTestWorld"));
		}

		~FDecomposeWorld()
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

	/** A plain actor with a plain scene component as its root: a Group by ClassifyActor's rule. */
	AActor* SpawnGroupActor(UWorld* World, const TCHAR* Label)
	{
		AActor* Actor = World->SpawnActor<AActor>();
		if (Actor)
		{
			USceneComponent* Root = NewObject<USceneComponent>(Actor, TEXT("Root"));
			Actor->SetRootComponent(Root);
			Root->RegisterComponent();
			Actor->SetActorLabel(Label);
		}
		return Actor;
	}

	UStaticMeshComponent* AddMesh(AActor* Actor, const TCHAR* Name, UStaticMesh* Mesh, const FVector& Location)
	{
		UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(Actor, Name);
		Component->SetStaticMesh(Mesh);
		Component->SetupAttachment(Actor->GetRootComponent());
		Component->SetRelativeLocation(Location);
		Component->RegisterComponent();
		return Component;
	}

	template <typename T>
	T* AddScene(AActor* Actor, const TCHAR* Name, const FVector& Location)
	{
		T* Component = NewObject<T>(Actor, Name);
		Component->SetupAttachment(Actor->GetRootComponent());
		Component->SetRelativeLocation(Location);
		Component->RegisterComponent();
		return Component;
	}

	int32 CountParts(const TArray<FMantlePrefabPart>& Parts, EMantleEntityType Type)
	{
		int32 Count = 0;
		for (const FMantlePrefabPart& Part : Parts)
		{
			Count += Part.Type == Type ? 1 : 0;
		}
		return Count;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleDecomposeGroupTest,
	"Mantle.Editor.Decompose.MultiComponentActorBecomesGroupAndParts",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleDecomposeGroupTest::RunTest(const FString& Parameters)
{
	using namespace MantleActorDecomposeTestPrivate;
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, CubePath);
	if (!Cube)
	{
		AddWarning(TEXT("the engine cube did not load; skipped"));
		return true;
	}
	FDecomposeWorld Fixture;
	AStaticMeshActor* Actor = Fixture.World->SpawnActor<AStaticMeshActor>();
	if (!TestNotNull(TEXT("the actor spawned"), Actor))
	{
		return false;
	}
	Actor->SetActorLabel(TEXT("Fridge"));
	Actor->GetStaticMeshComponent()->SetStaticMesh(Cube);
	AddMesh(Actor, TEXT("Door"), Cube, FVector(0.0, 0.0, 0.0));
	URectLightComponent* Light = AddScene<URectLightComponent>(Actor, TEXT("RectLight"), FVector(10.0, 0.0, 50.0));
	Light->Intensity = 500.0f;

	// A mesh actor with another mesh and a light beside its own is a prefab: a Group, its own mesh one of the parts.
	EMantleEntityType Type = EMantleEntityType::StaticMesh;
	TestTrue(TEXT("it is classified"), FMantlePlaceExporter::ClassifyActor(Actor, Type));
	TestTrue(TEXT("as a Group"), Type == EMantleEntityType::Group);

	TArray<FMantlePrefabPart> Parts;
	FMantlePlaceExporter::CollectPrefabParts(Actor, Parts);
	TestEqual(TEXT("with three parts"), Parts.Num(), 3);
	TestEqual(TEXT("two of them meshes"), CountParts(Parts, EMantleEntityType::StaticMesh), 2);
	TestEqual(TEXT("one an area light"), CountParts(Parts, EMantleEntityType::RectLight), 1);
	const FMantlePrefabPart* LightPart = Parts.FindByPredicate([](const FMantlePrefabPart& Part) { return Part.Type == EMantleEntityType::RectLight; });
	if (TestNotNull(TEXT("the light part is there"), LightPart))
	{
		TestTrue(TEXT("placed relative to the group"), LightPart->Transform.GetLocation().Equals(FVector(10.0, 0.0, 50.0), 0.01));
	}

	// Exported: the Group, then each part as a child of it.
	const FMantleExportResult Result = FMantlePlaceExporter::Export(Fixture.World, FMantleExportOptions());
	const FMantleEntity* Group = Result.Place.Entities.FindByPredicate([](const FMantleEntity& Entity) { return Entity.Name == TEXT("Fridge"); });
	if (TestNotNull(TEXT("the Group is written"), Group))
	{
		TestTrue(TEXT("as a Group"), Group->Type == EMantleEntityType::Group);
		int32 Children = 0;
		bool bRect = false;
		for (const FMantleEntity& Entity : Result.Place.Entities)
		{
			if (Entity.Parent == Group->Id)
			{
				++Children;
				bRect = bRect || (Entity.Type == EMantleEntityType::RectLight && Entity.Props.TryGet<FMantleRectLightProps>() != nullptr);
			}
		}
		TestEqual(TEXT("each part is a child of it"), Children, 3);
		TestTrue(TEXT("and the area light carries area-light properties"), bRect);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleDecomposeLightUnitsTest,
	"Mantle.Editor.Decompose.LightsKeepTheirUnits",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleDecomposeLightUnitsTest::RunTest(const FString& Parameters)
{
	using namespace MantleActorDecomposeTestPrivate;
	UObject* Outer = GetTransientPackage();

	// The level's own mix: Unitless 500 inside Blueprints, Candelas 3 to 8 as standalone lights.
	UPointLightComponent* Point = NewObject<UPointLightComponent>(Outer);
	Point->IntensityUnits = ELightUnits::Unitless;
	Point->Intensity = 500.0f;
	USpotLightComponent* Spot = NewObject<USpotLightComponent>(Outer);
	Spot->IntensityUnits = ELightUnits::Candelas;
	Spot->Intensity = 3.0f;
	URectLightComponent* Rect = NewObject<URectLightComponent>(Outer);
	Rect->IntensityUnits = ELightUnits::Candelas;
	Rect->Intensity = 8.0f;
	URectLightComponent* RectUnitless = NewObject<URectLightComponent>(Outer);
	RectUnitless->IntensityUnits = ELightUnits::Unitless;
	RectUnitless->Intensity = 500.0f;

	FMantleEntity PointEntity;
	FMantleEntity SpotEntity;
	FMantleEntity RectEntity;
	FMantleEntity RectUnitlessEntity;
	TestTrue(TEXT("a point light reads"), FMantlePlaceExporter::ReadComponentValueProperties(Point, EMantleEntityType::PointLight, PointEntity));
	TestTrue(TEXT("a spot light reads"), FMantlePlaceExporter::ReadComponentValueProperties(Spot, EMantleEntityType::SpotLight, SpotEntity));
	TestTrue(TEXT("a rect light reads"), FMantlePlaceExporter::ReadComponentValueProperties(Rect, EMantleEntityType::RectLight, RectEntity));
	TestTrue(TEXT("an unitless rect light reads"), FMantlePlaceExporter::ReadComponentValueProperties(RectUnitless, EMantleEntityType::RectLight, RectUnitlessEntity));

	const FMantlePointLightProps* PointProps = PointEntity.Props.TryGet<FMantlePointLightProps>();
	const FMantleSpotLightProps* SpotProps = SpotEntity.Props.TryGet<FMantleSpotLightProps>();
	const FMantleRectLightProps* RectProps = RectEntity.Props.TryGet<FMantleRectLightProps>();
	const FMantleRectLightProps* RectUnitlessProps = RectUnitlessEntity.Props.TryGet<FMantleRectLightProps>();
	if (!TestNotNull(TEXT("point props"), PointProps) || !TestNotNull(TEXT("spot props"), SpotProps)
		|| !TestNotNull(TEXT("rect props"), RectProps) || !TestNotNull(TEXT("unitless rect props"), RectUnitlessProps))
	{
		return false;
	}
	// The number travels with its unit: the builder sets the unit first, so the light is as bright as it was in the editor.
	TestTrue(TEXT("unitless 500 stays unitless 500 (point)"), PointProps->IntensityUnits == EMantleLightUnits::Unitless && FMath::IsNearlyEqual(PointProps->Intensity, 500.0));
	TestTrue(TEXT("candelas 3 stays candelas 3 (spot)"), SpotProps->IntensityUnits == EMantleLightUnits::Candelas && FMath::IsNearlyEqual(SpotProps->Intensity, 3.0));
	TestTrue(TEXT("candelas 8 stays candelas 8 (rect)"), RectProps->IntensityUnits == EMantleLightUnits::Candelas && FMath::IsNearlyEqual(RectProps->Intensity, 8.0));
	TestTrue(TEXT("unitless 500 stays unitless 500 (rect)"), RectUnitlessProps->IntensityUnits == EMantleLightUnits::Unitless && FMath::IsNearlyEqual(RectUnitlessProps->Intensity, 500.0));

	// And in the place file: candelas is written, unitless is the default an absent key means.
	FMantlePlace Place;
	PointEntity.Id = TEXT("ent_point");
	PointEntity.Type = EMantleEntityType::PointLight;
	PointEntity.Name = TEXT("Point");
	RectEntity.Id = TEXT("ent_rect");
	RectEntity.Type = EMantleEntityType::RectLight;
	RectEntity.Name = TEXT("Rect");
	Place.Entities.Add(PointEntity);
	Place.Entities.Add(RectEntity);
	TSharedPtr<FJsonObject> Root;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FMantlePlaceExporter::ToJson(Place)), Root);
	const TArray<TSharedPtr<FJsonValue>>* Entities = nullptr;
	if (TestTrue(TEXT("the place has entities"), Root.IsValid() && Root->TryGetArrayField(TEXT("entities"), Entities)) && Entities->Num() == 2)
	{
		const TSharedPtr<FJsonObject> PointJson = (*Entities)[0]->AsObject()->GetObjectField(TEXT("properties"));
		const TSharedPtr<FJsonObject> RectJson = (*Entities)[1]->AsObject()->GetObjectField(TEXT("properties"));
		TestFalse(TEXT("unitless writes no unit"), PointJson->HasField(TEXT("intensityUnits")));
		TestEqual(TEXT("candelas writes its unit"), RectJson->GetStringField(TEXT("intensityUnits")), FString(MantleLightUnitsToString(EMantleLightUnits::Candelas)));
		TestEqual(TEXT("and its number"), RectJson->GetNumberField(TEXT("intensity")), 8.0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleDecomposeEditorOnlyTest,
	"Mantle.Editor.Decompose.EditorOnlyComponentIsIgnored",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleDecomposeEditorOnlyTest::RunTest(const FString& Parameters)
{
	using namespace MantleActorDecomposeTestPrivate;
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, CubePath);
	if (!Cube)
	{
		AddWarning(TEXT("the engine cube did not load; skipped"));
		return true;
	}
	FDecomposeWorld Fixture;
	AActor* Actor = SpawnGroupActor(Fixture.World, TEXT("Prop"));
	if (!TestNotNull(TEXT("the actor spawned"), Actor))
	{
		return false;
	}
	AddMesh(Actor, TEXT("Body"), Cube, FVector::ZeroVector);
	UBillboardComponent* Icon = AddScene<UBillboardComponent>(Actor, TEXT("Icon"), FVector::ZeroVector);
	TestTrue(TEXT("a billboard is editor-only"), FMantleActorDecompose::IsVisualisation(Icon));

	TArray<FMantlePrefabPart> Parts;
	TArray<FMantleUnpublishedPart> Unsupported;
	FMantlePlaceExporter::CollectPrefabParts(Actor, Parts, &Unsupported);
	TestEqual(TEXT("only the mesh is a part"), Parts.Num(), 1);
	TestEqual(TEXT("and the icon is not named as lost"), Unsupported.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleDecomposeUnknownComponentTest,
	"Mantle.Editor.Decompose.UnknownComponentIsDroppedWithALine",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleDecomposeUnknownComponentTest::RunTest(const FString& Parameters)
{
	using namespace MantleActorDecomposeTestPrivate;
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, CubePath);
	if (!Cube)
	{
		AddWarning(TEXT("the engine cube did not load; skipped"));
		return true;
	}
	FDecomposeWorld Fixture;
	AActor* Actor = SpawnGroupActor(Fixture.World, TEXT("Spinner"));
	if (!TestNotNull(TEXT("the actor spawned"), Actor))
	{
		return false;
	}
	AddMesh(Actor, TEXT("Body"), Cube, FVector::ZeroVector);
	AddScene<UCapsuleComponent>(Actor, TEXT("Capsule"), FVector::ZeroVector);
	URotatingMovementComponent* Spin = NewObject<URotatingMovementComponent>(Actor, TEXT("Spin"));
	Spin->RegisterComponent();

	// A capsule a player could touch has no entity: named, not an error. A movement component is behaviour: dropped by class.
	TArray<FMantlePrefabPart> Parts;
	TArray<FMantleUnpublishedPart> Unsupported;
	FMantlePlaceExporter::CollectPrefabParts(Actor, Parts, &Unsupported);
	TestEqual(TEXT("the mesh is still a part"), Parts.Num(), 1);
	const FMantleUnpublishedPart* Capsule = Unsupported.FindByPredicate([](const FMantleUnpublishedPart& Part) { return Part.What.Contains(TEXT("CapsuleComponent")); });
	if (TestNotNull(TEXT("the capsule is named"), Capsule))
	{
		TestFalse(TEXT("and does not block the publish"), Capsule->bBlocksPublish);
	}

	FMantleCoverage Coverage;
	FMantleActorDecompose::ReportActor(Actor, EMantleEntityType::Group, Coverage);
	const FMantleCoverageCategory* Other = Coverage.Find(MantleCoverageCategory::OtherComponents);
	if (TestNotNull(TEXT("the movement component has a coverage line"), Other))
	{
		const FMantleCoverageBucket& Dropped = Other->Get(EOutcome::Dropped);
		TestEqual(TEXT("dropped once"), Dropped.Count, static_cast<int64>(1));
		TestTrue(TEXT("named by its class"), Dropped.Examples.ContainsByPredicate([](const FMantleCoverageExample& Example)
		{
			return Example.Name == TEXT("RotatingMovementComponent");
		}));
	}

	const FMantleValidationResult Validation = FMantlePlaceValidator::Validate(Fixture.World);
	TestFalse(TEXT("and Check raises no extra-component error for the actor"), Validation.Issues.ContainsByPredicate([](const FMantleValidationIssue& Issue)
	{
		return Issue.Actor == TEXT("Spinner") && Issue.Severity == EMantleIssueSeverity::Error;
	}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleDecomposeReflectionCaptureTest,
	"Mantle.Editor.Decompose.ReflectionCaptureIsAProbe",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleDecomposeReflectionCaptureTest::RunTest(const FString& Parameters)
{
	using namespace MantleActorDecomposeTestPrivate;
	FDecomposeWorld Fixture;
	// APlaneReflectionCapture is abstract in 5.8, so a level holds only box and sphere captures; both have a place form.
	ABoxReflectionCapture* Capture = Fixture.World->SpawnActor<ABoxReflectionCapture>();
	if (!TestNotNull(TEXT("the capture spawned"), Capture))
	{
		return false;
	}
	Capture->SetActorLabel(TEXT("Capture"));
	TestTrue(TEXT("it is a reflection capture"), FMantleActorDecompose::IsReflectionCapture(Capture));
	TestFalse(TEXT("a box one has a place form"), FMantleActorDecompose::IsUnpublishedReflectionCapture(Capture));

	const FMantleExportResult Result = FMantlePlaceExporter::Export(Fixture.World, FMantleExportOptions());
	TestFalse(TEXT("export does not refuse it"), Result.Errors.ContainsByPredicate([](const FMantleParseError& Error)
	{
		return Error.Message.Contains(TEXT("ReflectionCapture")) || Error.Path.Contains(TEXT("Capture"));
	}));
	// The box capture is a ReflectionProbe entity (the probe tests have its properties).
	int32 Probes = 0;
	for (const FMantleEntity& Entity : Result.Place.Entities)
	{
		Probes += Entity.Type == EMantleEntityType::ReflectionProbe ? 1 : 0;
	}
	TestEqual(TEXT("one probe is written"), Probes, 1);
	TestEqual(TEXT("the probe is kept"), Result.Coverage.Count(MantleCoverageCategory::Lights, EOutcome::Kept), static_cast<int64>(1));
	TestEqual(TEXT("no other actor is reported as Info"), Result.Coverage.Count(MantleCoverageCategory::OtherActors, EOutcome::Info), static_cast<int64>(0));

	const FMantleValidationResult Validation = FMantlePlaceValidator::Validate(Fixture.World);
	TestFalse(TEXT("Check raises no error for it"), Validation.Issues.ContainsByPredicate([](const FMantleValidationIssue& Issue)
	{
		return Issue.Actor == TEXT("Capture") && Issue.Severity == EMantleIssueSeverity::Error;
	}));
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleDecomposeBlueprintLogicTest,
	"Mantle.Editor.Decompose.BlueprintEventGraphIsOneDroppedLinePerClass",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleDecomposeBlueprintLogicTest::RunTest(const FString& Parameters)
{
	using namespace MantleActorDecomposeTestPrivate;

	// The hint, pure: the events of the actor's own Blueprint, then each parent's, marked as the parent's.
	FMantleBlueprintBehaviour Gate;
	Gate.Blueprint = TEXT("BP_Gate");
	Gate.Events.Add(TEXT("BeginPlay"));
	FMantleBlueprintBehaviour Door;
	Door.Blueprint = TEXT("BP_BaseDoor");
	Door.Events.Add(TEXT("ActorBeginOverlap"));
	Door.Events.Add(TEXT("Tick"));
	FMantleBlueprintBehaviour Rotator;
	Rotator.Blueprint = TEXT("BP_BaseRotator");
	Rotator.Functions.Add(TEXT("Spin"));
	TestEqual(TEXT("the hint names events and parents"), FMantleActorDecompose::ComposeBlueprintHint({ Gate, Door }),
		FString(TEXT("BeginPlay; ActorBeginOverlap, Tick from parent BP_BaseDoor")));
	TestEqual(TEXT("a function stands in when there is no event"), FMantleActorDecompose::ComposeBlueprintHint({ Rotator }),
		FString(TEXT("function Spin")));
	FMantleBlueprintBehaviour Many;
	Many.Blueprint = TEXT("BP_Many");
	Many.Events = { TEXT("A"), TEXT("B"), TEXT("C"), TEXT("D"), TEXT("E") };
	TestEqual(TEXT("a long list is cut and counted"), FMantleActorDecompose::ComposeBlueprintHint({ Many }), FString(TEXT("A, B, C and 2 more")));

	// A real Blueprint with a wired event graph: every actor of its class is counted, on one line.
	UPackage* Package = CreatePackage(TEXT("/Temp/MantleDecomposeTestBlueprint"));
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), Package, TEXT("BP_DecomposeTest"), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (!TestNotNull(TEXT("the Blueprint was made"), Blueprint) || !TestNotNull(TEXT("and compiled"), Blueprint->GeneratedClass.Get()))
	{
		return false;
	}
	UEdGraph* EventGraph = Blueprint->UbergraphPages.Num() > 0 ? Blueprint->UbergraphPages[0] : nullptr;
	UEdGraphNode* EventNode = nullptr;
	if (EventGraph)
	{
		for (UEdGraphNode* Node : EventGraph->Nodes)
		{
			if (FMantleActorDecompose::IsEventNode(Node))
			{
				EventNode = Node;
				break;
			}
		}
	}
	if (!EventNode)
	{
		AddWarning(TEXT("a new Blueprint has no default event nodes in this engine; the wired-event half is skipped"));
		return true;
	}
	// A new Blueprint's events are greyed out and unwired: no behaviour yet.
	FDecomposeWorld Fixture;
	AActor* First = Fixture.World->SpawnActor<AActor>(Blueprint->GeneratedClass);
	AActor* Second = Fixture.World->SpawnActor<AActor>(Blueprint->GeneratedClass);
	if (!TestNotNull(TEXT("the Blueprint actors spawned"), First) || !TestNotNull(TEXT("both"), Second))
	{
		return false;
	}
	FString Name;
	FString Detail;
	TestFalse(TEXT("an untouched Blueprint has no behaviour"), FMantleActorDecompose::DescribeBlueprintBehaviour(First, Name, Detail));

	// Enable the event and give it somewhere to go.
	EventNode->SetEnabledState(ENodeEnabledState::Enabled);
	UEdGraphNode* Sink = NewObject<UEdGraphNode>(EventGraph);
	EventGraph->AddNode(Sink, /*bFromUI*/ false, /*bSelectNewNode*/ false);
	UEdGraphPin* SinkPin = Sink->CreatePin(EGPD_Input, TEXT("exec"), TEXT("execute"));
	UEdGraphPin* EventPin = EventNode->Pins.FindByPredicate([](const UEdGraphPin* Pin) { return Pin && Pin->Direction == EGPD_Output; }) ? *EventNode->Pins.FindByPredicate([](const UEdGraphPin* Pin) { return Pin && Pin->Direction == EGPD_Output; }) : nullptr;
	if (!TestNotNull(TEXT("the event has an output"), EventPin))
	{
		return false;
	}
	EventPin->MakeLinkTo(SinkPin);

	TestTrue(TEXT("a wired event is behaviour"), FMantleActorDecompose::DescribeBlueprintBehaviour(First, Name, Detail));
	TestEqual(TEXT("named after the Blueprint"), Name, FString(TEXT("BP_DecomposeTest")));
	TestTrue(TEXT("with its event"), Detail.Contains(FMantleActorDecompose::EventNameOf(EventNode)));
	TestTrue(TEXT("and the honest wording"), Detail.StartsWith(TEXT("its behaviour isn't carried (e.g. ")));

	FMantleCoverage Coverage;
	FMantleActorDecompose::ReportActor(First, EMantleEntityType::Group, Coverage);
	FMantleActorDecompose::ReportActor(Second, EMantleEntityType::Group, Coverage);
	const FMantleCoverageCategory* Logic = Coverage.Find(MantleCoverageCategory::BlueprintLogic);
	if (TestNotNull(TEXT("the Blueprint logic category has a line"), Logic))
	{
		const FMantleCoverageBucket& Dropped = Logic->Get(EOutcome::Dropped);
		TestEqual(TEXT("both actors are counted"), Dropped.Count, static_cast<int64>(2));
		TestEqual(TEXT("on one line for the class"), Dropped.Examples.Num(), 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleDecomposeEffectJsonTest,
	"Mantle.Editor.Decompose.EffectPropertiesAreRead",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleDecomposeEffectJsonTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("archetype"), TEXT("fire"));
	Json->SetArrayField(TEXT("color"), { MakeShared<FJsonValueNumber>(1.0), MakeShared<FJsonValueNumber>(0.5), MakeShared<FJsonValueNumber>(0.25) });
	Json->SetNumberField(TEXT("size"), 12.0);
	Json->SetBoolField(TEXT("loop"), false);

	FMantleEffectProps Props;
	TestTrue(TEXT("a built-in archetype reads"), FMantleActorDecompose::EffectPropsFromJson(*Json, Props));
	TestTrue(TEXT("the archetype"), Props.Archetype == EMantleEffectArchetype::Fire);
	TestTrue(TEXT("the colour"), Props.Look.Color.Equals(FLinearColor(1.0f, 0.5f, 0.25f)));
	TestEqual(TEXT("the size"), Props.Look.Size, 12.0);
	TestEqual(TEXT("a key left out keeps the archetype's own"), Props.Look.Rate, FMantleEffects::DefaultLook(EMantleEffectArchetype::Fire).Rate);
	TestFalse(TEXT("loop"), Props.bLoop);
	TestTrue(TEXT("autoplay keeps its default"), Props.bAutoplay);

	Json->SetStringField(TEXT("archetype"), TEXT("not-an-effect"));
	TestFalse(TEXT("an unknown archetype is refused"), FMantleActorDecompose::EffectPropsFromJson(*Json, Props));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleDecomposeMirrorTest,
	"Mantle.Editor.Decompose.NegativeScaleBakesAMirroredCopy",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleDecomposeMirrorTest::RunTest(const FString& Parameters)
{
	using namespace MantleActorDecomposeTestPrivate;
	TestTrue(TEXT("a negative axis mirrors"), FMantleActorDecompose::HasNegativeAxis(FVector(-1.0, 1.0, 1.0)));
	TestFalse(TEXT("a positive scale does not"), FMantleActorDecompose::HasNegativeAxis(FVector(2.0, 1.0, 1.0)));

	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, CubePath);
	if (!Cube)
	{
		AddWarning(TEXT("the engine cube did not load; skipped"));
		return true;
	}
	FDecomposeWorld Fixture;
	AStaticMeshActor* Actor = Fixture.World->SpawnActor<AStaticMeshActor>();
	if (!TestNotNull(TEXT("the actor spawned"), Actor))
	{
		return false;
	}
	Actor->SetActorLabel(TEXT("Mirrored"));
	Actor->GetStaticMeshComponent()->SetStaticMesh(Cube);
	Actor->SetActorScale3D(FVector(-1.0, 1.0, 1.0));

	// Check raises no scale error: the export bakes a mirrored copy instead.
	const FMantleValidationResult Validation = FMantlePlaceValidator::Validate(Fixture.World);
	TestFalse(TEXT("a negative scale is not an error"), Validation.Issues.ContainsByPredicate([](const FMantleValidationIssue& Issue)
	{
		return Issue.Actor == TEXT("Mirrored") && Issue.Severity == EMantleIssueSeverity::Error && Issue.Problem.Contains(TEXT("scale"));
	}));

	const FMantleExportResult Result = FMantlePlaceExporter::Export(Fixture.World, FMantleExportOptions());
	const FMantleEntity* Entity = Result.Place.Entities.FindByPredicate([](const FMantleEntity& Candidate) { return Candidate.Name == TEXT("Mirrored"); });
	if (TestNotNull(TEXT("it is exported"), Entity))
	{
		TestTrue(TEXT("at the positive scale"), Entity->Transform.GetScale3D().Equals(FVector::OneVector, 0.0001));
	}
	TestEqual(TEXT("and reported as approximated"), Result.Coverage.Count(MantleCoverageCategory::StaticMeshes, EOutcome::Approximated), static_cast<int64>(1));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
