// Copyright Joshua Eger. All Rights Reserved.
#include "MantleClientFeatures.h"
#include "MantleCoverage.h"
#include "MantlePlaceExporter.h"
#include "MantlePlaceLimits.h"
#include "MantlePlaceParser.h"
#include "MantlePlaceValidator.h"
#include "MantleTargetFeatures.h"
#include "MantleTextExport.h"

#include "Components/SceneComponent.h"
#include "Components/TextRenderComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/Font.h"
#include "Engine/TextRenderActor.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// World text (GhzGarage/mantle#724): a Text Render Actor and a Blueprint's text component both export as Text entities,
// written with the issue's field names; a target without worldText gets neither and a "needs a newer Mantle client" line; a
// custom font is approximated; text that is too long, or too many, refuses the export. The place file is read back as JSON.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleTextExportTestPrivate
{
	using EOutcome = EMantleCoverageOutcome;

	/** An empty editor world for one test, destroyed with it. */
	struct FTextExportWorld
	{
		UWorld* World = nullptr;

		FTextExportWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld*/ false, TEXT("MantleTextExportTestWorld"));
		}

		~FTextExportWorld()
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

	ATextRenderActor* SpawnText(UWorld* World, const TCHAR* Label, const FString& Text, const FVector& Location = FVector::ZeroVector)
	{
		ATextRenderActor* Actor = World->SpawnActor<ATextRenderActor>(Location, FRotator::ZeroRotator);
		if (Actor)
		{
			Actor->SetActorLabel(Label);
			Actor->GetTextRender()->SetText(FText::FromString(Text));
		}
		return Actor;
	}

	/** A plain actor on a scene root with one text component attached: what a Blueprint of a sign is. */
	AActor* SpawnSignBlueprint(UWorld* World, const TCHAR* Label, const FString& Text, UTextRenderComponent*& OutText)
	{
		AActor* Actor = World->SpawnActor<AActor>(FVector(500.0, 0.0, 0.0), FRotator::ZeroRotator);
		if (!Actor)
		{
			return nullptr;
		}
		Actor->SetActorLabel(Label);
		USceneComponent* Root = NewObject<USceneComponent>(Actor, TEXT("DefaultSceneRoot"));
		Actor->SetRootComponent(Root);
		Root->RegisterComponent();
		OutText = NewObject<UTextRenderComponent>(Actor, TEXT("SignText"));
		OutText->SetupAttachment(Root);
		OutText->SetRelativeLocation(FVector(0.0, 0.0, 120.0));
		OutText->SetText(FText::FromString(Text));
		OutText->RegisterComponent();
		return Actor;
	}

	/** The place file as JSON: ToJson, parsed. */
	TSharedPtr<FJsonObject> PlaceJson(const FMantlePlace& Place)
	{
		TSharedPtr<FJsonObject> Root;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FMantlePlaceExporter::ToJson(Place)), Root);
		return Root;
	}

	/** The index'th entity of the type in the place file, or null. */
	TSharedPtr<FJsonObject> JsonEntity(const TSharedPtr<FJsonObject>& Root, const TCHAR* Type, int32 Index = 0)
	{
		const TArray<TSharedPtr<FJsonValue>>* Entities = nullptr;
		if (Root.IsValid() && Root->TryGetArrayField(TEXT("entities"), Entities))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Entities)
			{
				const TSharedPtr<FJsonObject>* Entity = nullptr;
				FString EntityType;
				if (Value->TryGetObject(Entity) && (*Entity)->TryGetStringField(TEXT("type"), EntityType) && EntityType == Type && Index-- == 0)
				{
					return *Entity;
				}
			}
		}
		return nullptr;
	}

	TSharedPtr<FJsonObject> JsonProperties(const TSharedPtr<FJsonObject>& Entity)
	{
		const TSharedPtr<FJsonObject>* Properties = nullptr;
		return Entity.IsValid() && Entity->TryGetObjectField(TEXT("properties"), Properties) ? *Properties : nullptr;
	}

	bool HasExample(const FMantleCoverage& Coverage, const TCHAR* Category, EOutcome Outcome, const FString& Contains)
	{
		const FMantleCoverageCategory* Found = Coverage.Find(Category);
		return Found && Found->Get(Outcome).Examples.ContainsByPredicate(
			[&Contains](const FMantleCoverageExample& Example) { return Example.Detail.Contains(Contains) || Example.Name.Contains(Contains); });
	}

	bool HasError(const FMantleExportResult& Result, const FString& PathPart)
	{
		return Result.Errors.ContainsByPredicate([&PathPart](const FMantleParseError& Error) { return Error.Path.Contains(PathPart); });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTextExportActorTest,
	"Mantle.Editor.WorldText.Export.TextRenderActorBecomesAText",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTextExportActorTest::RunTest(const FString& Parameters)
{
	using namespace MantleTextExportTestPrivate;
	FTextExportWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	ATextRenderActor* Actor = SpawnText(Fixture.World, TEXT("Room sign"), TEXT("Room 12<br>Staff only"), FVector(100.0, 200.0, 300.0));
	if (!TestNotNull(TEXT("the text actor"), Actor))
	{
		return false;
	}
	UTextRenderComponent* Text = Actor->GetTextRender();
	Text->SetTextRenderColor(FColor(255, 0, 0, 128));
	Text->SetWorldSize(50.0f);
	Text->SetHorizontalAlignment(EHTA_Center);
	Text->SetVerticalAlignment(EVRTA_TextBottom);
	Text->SetXScale(2.0f);
	Text->SetYScale(0.5f);
	Actor->SetActorScale3D(FVector(1.0, 2.0, 3.0));

	TestTrue(TEXT("it is a text actor"), FMantleTextExport::IsTextActor(Actor));
	TestFalse(TEXT("and not engine infrastructure on a target that reads worldText"), FMantlePlaceExporter::IsEngineInfrastructure(Actor));

	const FMantleExportResult Result = FMantlePlaceExporter::Export(Fixture.World, FMantleExportOptions());
	TestFalse(TEXT("export does not refuse it"), HasError(Result, TEXT("Text")));
	TestEqual(TEXT("one Kept line"), Result.Coverage.Count(MantleCoverageCategory::Text, EOutcome::Kept), static_cast<int64>(1));
	TestEqual(TEXT("nothing approximated"), Result.Coverage.Count(MantleCoverageCategory::Text, EOutcome::Approximated), static_cast<int64>(0));
	TestEqual(TEXT("and nothing dropped"), Result.Coverage.Count(MantleCoverageCategory::OtherActors, EOutcome::Dropped), static_cast<int64>(0));

	const TSharedPtr<FJsonObject> Root = PlaceJson(Result.Place);
	const TSharedPtr<FJsonObject> Entity = JsonEntity(Root, TEXT("Text"));
	if (!TestTrue(TEXT("the file has a Text entity"), Entity.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("named by its label"), Entity->GetStringField(TEXT("name")), FString(TEXT("Room sign")));
	const TSharedPtr<FJsonObject> Properties = JsonProperties(Entity);
	if (!TestTrue(TEXT("with properties"), Properties.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("text, with the line break the file holds"), Properties->GetStringField(TEXT("text")), FString(TEXT("Room 12\nStaff only")));
	TestEqual(TEXT("worldSize"), Properties->GetNumberField(TEXT("worldSize")), 50.0);
	TestEqual(TEXT("horizontalAlignment"), Properties->GetStringField(TEXT("horizontalAlignment")), FString(TEXT("center")));
	TestEqual(TEXT("verticalAlignment"), Properties->GetStringField(TEXT("verticalAlignment")), FString(TEXT("bottom")));
	// The actor's scale is folded in: along the line is Y (2), up the glyph is Z (3).
	TestEqual(TEXT("xScale is the component's times the actor's Y scale"), Properties->GetNumberField(TEXT("xScale")), 4.0);
	TestEqual(TEXT("yScale is the component's times the actor's Z scale"), Properties->GetNumberField(TEXT("yScale")), 1.5);
	const TArray<TSharedPtr<FJsonValue>>& Color = Properties->GetArrayField(TEXT("color"));
	if (TestEqual(TEXT("color is RGBA"), Color.Num(), 4))
	{
		// 255 is 1 in linear; 128 of alpha is linear 128/255.
		TestEqual(TEXT("red"), Color[0]->AsNumber(), 1.0);
		TestEqual(TEXT("green"), Color[1]->AsNumber(), 0.0);
		TestEqual(TEXT("blue"), Color[2]->AsNumber(), 0.0);
		TestTrue(TEXT("alpha"), FMath::IsNearlyEqual(Color[3]->AsNumber(), 128.0 / 255.0, 0.001));
	}
	const TArray<TSharedPtr<FJsonValue>>& Location = Entity->GetObjectField(TEXT("transform"))->GetArrayField(TEXT("location"));
	if (TestEqual(TEXT("a location"), Location.Num(), 3))
	{
		TestEqual(TEXT("where the actor stands"), Location[2]->AsNumber(), 300.0);
	}
	const TArray<TSharedPtr<FJsonValue>>& Scale = Entity->GetObjectField(TEXT("transform"))->GetArrayField(TEXT("scale"));
	if (TestEqual(TEXT("a scale"), Scale.Num(), 3))
	{
		TestEqual(TEXT("written at scale 1"), Scale[1]->AsNumber(), 1.0);
	}

	// The file reads as a place, and Check raises no error for it.
	const FMantleParseResult Parsed = FMantlePlaceParser::ParseFromString(FMantlePlaceExporter::ToJson(Result.Place));
	TestTrue(TEXT("the file parses as a place"), Parsed.bSuccess);
	const FMantleValidationResult Validation = FMantlePlaceValidator::Validate(Fixture.World);
	TestFalse(TEXT("Check raises no error for it"), Validation.Issues.ContainsByPredicate([](const FMantleValidationIssue& Issue)
	{
		return Issue.Actor == TEXT("Room sign") && Issue.Severity == EMantleIssueSeverity::Error;
	}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTextExportBlueprintTest,
	"Mantle.Editor.WorldText.Export.BlueprintTextComponentBecomesAText",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTextExportBlueprintTest::RunTest(const FString& Parameters)
{
	using namespace MantleTextExportTestPrivate;
	FTextExportWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	UTextRenderComponent* Text = nullptr;
	AActor* Sign = SpawnSignBlueprint(Fixture.World, TEXT("Sign blueprint"), TEXT("Open"), Text);
	if (!TestNotNull(TEXT("the blueprint actor"), Sign) || !TestNotNull(TEXT("and its text"), Text))
	{
		return false;
	}
	Text->SetHorizontalAlignment(EHTA_Right);
	Text->SetVerticalAlignment(EVRTA_TextCenter);

	const FMantleExportResult Result = FMantlePlaceExporter::Export(Fixture.World, FMantleExportOptions());
	TestFalse(TEXT("export does not refuse it"), HasError(Result, TEXT("Sign")));
	const TSharedPtr<FJsonObject> Root = PlaceJson(Result.Place);
	TestTrue(TEXT("the actor is a Group"), JsonEntity(Root, TEXT("Group")).IsValid());
	const TSharedPtr<FJsonObject> Entity = JsonEntity(Root, TEXT("Text"));
	if (!TestTrue(TEXT("and its text component a Text entity"), Entity.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("a child of the group"), Entity->HasTypedField<EJson::String>(TEXT("parent")));
	const TSharedPtr<FJsonObject> Properties = JsonProperties(Entity);
	if (TestTrue(TEXT("with properties"), Properties.IsValid()))
	{
		TestEqual(TEXT("text"), Properties->GetStringField(TEXT("text")), FString(TEXT("Open")));
		TestEqual(TEXT("horizontalAlignment"), Properties->GetStringField(TEXT("horizontalAlignment")), FString(TEXT("right")));
		TestEqual(TEXT("verticalAlignment"), Properties->GetStringField(TEXT("verticalAlignment")), FString(TEXT("center")));
		TestEqual(TEXT("worldSize is the engine's default"), Properties->GetNumberField(TEXT("worldSize")), static_cast<double>(Text->WorldSize));
	}
	const TArray<TSharedPtr<FJsonValue>>& Location = Entity->GetObjectField(TEXT("transform"))->GetArrayField(TEXT("location"));
	if (TestEqual(TEXT("a location"), Location.Num(), 3))
	{
		TestEqual(TEXT("raised above the group's origin"), Location[2]->AsNumber(), 120.0);
	}
	TestEqual(TEXT("one Kept line for it"), Result.Coverage.Count(MantleCoverageCategory::Text, EOutcome::Kept), static_cast<int64>(1));
	TestTrue(TEXT("the file parses as a place"), FMantlePlaceParser::ParseFromString(FMantlePlaceExporter::ToJson(Result.Place)).bSuccess);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTextExportGatedTest,
	"Mantle.Editor.WorldText.Export.LeftOutWhenTheTargetLacksWorldText",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTextExportGatedTest::RunTest(const FString& Parameters)
{
	using namespace MantleTextExportTestPrivate;
	FTextExportWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	ATextRenderActor* Actor = SpawnText(Fixture.World, TEXT("Lobby sign"), TEXT("Lobby"));
	UTextRenderComponent* Text = nullptr;
	AActor* Sign = SpawnSignBlueprint(Fixture.World, TEXT("Sign blueprint"), TEXT("Open"), Text);
	if (!TestNotNull(TEXT("the text actor"), Actor) || !TestNotNull(TEXT("and the blueprint"), Sign))
	{
		return false;
	}

	const FMantleTargetFeaturesScope Scope(FMantleTargetFeatures::None());
	FString What;
	FString Remedy;
	TestTrue(TEXT("a text actor is named as left out"), FMantlePlaceExporter::DescribeUnpublishedContent(Actor, What, Remedy));
	TestTrue(TEXT("saying the target needs a newer client"), What.Contains(FMantleTargetFeatures::NeedsNewerClient));
	TestTrue(TEXT("it is skipped rather than refused"), FMantlePlaceExporter::IsEngineInfrastructure(Actor));

	const FMantleExportResult Result = FMantlePlaceExporter::Export(Fixture.World, FMantleExportOptions());
	TestFalse(TEXT("export refuses nothing"), HasError(Result, TEXT("Sign")) || HasError(Result, TEXT("TextRender")));
	const TSharedPtr<FJsonObject> Root = PlaceJson(Result.Place);
	TestFalse(TEXT("the file has no Text entity"), JsonEntity(Root, TEXT("Text")).IsValid());
	TestTrue(TEXT("the text actor is dropped, with the reason"),
		HasExample(Result.Coverage, MantleCoverageCategory::OtherActors, EOutcome::Dropped, FMantleTargetFeatures::NeedsNewerClient));
	TestTrue(TEXT("and the blueprint's text component too"),
		HasExample(Result.Coverage, MantleCoverageCategory::Text, EOutcome::Dropped, FMantleTargetFeatures::NeedsNewerClient));
	TestEqual(TEXT("nothing is kept"), Result.Coverage.Count(MantleCoverageCategory::Text, EOutcome::Kept), static_cast<int64>(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTextExportCustomFontTest,
	"Mantle.Editor.WorldText.Export.CustomFontIsApproximated",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTextExportCustomFontTest::RunTest(const FString& Parameters)
{
	using namespace MantleTextExportTestPrivate;
	FTextExportWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	ATextRenderActor* Actor = SpawnText(Fixture.World, TEXT("Fancy sign"), TEXT("Welcome"));
	if (!TestNotNull(TEXT("the text actor"), Actor))
	{
		return false;
	}
	Actor->GetTextRender()->SetFont(NewObject<UFont>(GetTransientPackage(), TEXT("FancyFont")));
	// Quad Top has no exact form either.
	Actor->GetTextRender()->SetVerticalAlignment(EVRTA_QuadTop);

	const FMantleExportResult Result = FMantlePlaceExporter::Export(Fixture.World, FMantleExportOptions());
	TestTrue(TEXT("it is still published"), JsonEntity(PlaceJson(Result.Place), TEXT("Text")).IsValid());
	TestEqual(TEXT("one Approximated line"), Result.Coverage.Count(MantleCoverageCategory::Text, EOutcome::Approximated), static_cast<int64>(1));
	TestEqual(TEXT("and none Kept"), Result.Coverage.Count(MantleCoverageCategory::Text, EOutcome::Kept), static_cast<int64>(0));
	TestTrue(TEXT("it says it is drawn in Mantle's font"),
		HasExample(Result.Coverage, MantleCoverageCategory::Text, EOutcome::Approximated, TEXT("drawn in Mantle's font")));
	TestTrue(TEXT("and names the font"), HasExample(Result.Coverage, MantleCoverageCategory::Text, EOutcome::Approximated, TEXT("FancyFont")));
	TestEqual(TEXT("Quad Top is published as top"),
		JsonProperties(JsonEntity(PlaceJson(Result.Place), TEXT("Text")))->GetStringField(TEXT("verticalAlignment")), FString(TEXT("top")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTextExportMirroredTest,
	"Mantle.Editor.WorldText.Export.NegativeScaleIsAWarningNotAnError",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTextExportMirroredTest::RunTest(const FString& Parameters)
{
	using namespace MantleTextExportTestPrivate;
	FTextExportWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	ATextRenderActor* Sign = SpawnText(Fixture.World, TEXT("Mirrored sign"), TEXT("Hello"));
	if (!TestNotNull(TEXT("a sign"), Sign))
	{
		return false;
	}
	Sign->SetActorScale3D(FVector(1.0, -1.0, 1.0));

	const FMantleValidationResult Validation = FMantlePlaceValidator::Validate(Fixture.World);
	TestFalse(TEXT("Check does not refuse the sign"), Validation.Issues.ContainsByPredicate([](const FMantleValidationIssue& Issue)
	{
		return Issue.Actor == TEXT("Mirrored sign") && Issue.Severity == EMantleIssueSeverity::Error;
	}));
	TestTrue(TEXT("it says the text is drawn unmirrored"), Validation.Issues.ContainsByPredicate([](const FMantleValidationIssue& Issue)
	{
		return Issue.Actor == TEXT("Mirrored sign") && Issue.Severity == EMantleIssueSeverity::Warning && Issue.Problem.Contains(TEXT("unmirrored"));
	}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTextExportLimitsTest,
	"Mantle.Editor.WorldText.Export.LimitsAreRefused",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTextExportLimitsTest::RunTest(const FString& Parameters)
{
	using namespace MantleTextExportTestPrivate;

	// Text one character over the limit.
	{
		FTextExportWorld Fixture;
		if (!TestNotNull(TEXT("a world"), Fixture.World))
		{
			return false;
		}
		const FString Long = FString::ChrN(FMantlePlaceLimits::MaxTextLength + 1, TEXT('a'));
		const FString Exact = FString::ChrN(FMantlePlaceLimits::MaxTextLength, TEXT('b'));
		SpawnText(Fixture.World, TEXT("Long sign"), Long);
		const FMantleExportResult Result = FMantlePlaceExporter::Export(Fixture.World, FMantleExportOptions());
		TestTrue(TEXT("over-long text refuses the export, at its text"), HasError(Result, TEXT("properties/text")));
		const FMantleValidationResult Validation = FMantlePlaceValidator::Validate(Fixture.World);
		TestTrue(TEXT("and Check says so as an error"), Validation.Issues.ContainsByPredicate([](const FMantleValidationIssue& Issue)
		{
			return Issue.Actor == TEXT("Long sign") && Issue.Severity == EMantleIssueSeverity::Error;
		}));

		FTextExportWorld Boundary;
		SpawnText(Boundary.World, TEXT("Exact sign"), Exact);
		TestFalse(TEXT("text of exactly the limit is accepted"),
			HasError(FMantlePlaceExporter::Export(Boundary.World, FMantleExportOptions()), TEXT("properties/text")));
	}

	// One text too many in the place.
	{
		FTextExportWorld Fixture;
		if (!TestNotNull(TEXT("a world"), Fixture.World))
		{
			return false;
		}
		for (int32 Index = 0; Index < FMantlePlaceLimits::MaxTextsPerPlace + 1; ++Index)
		{
			SpawnText(Fixture.World, *FString::Printf(TEXT("Label %d"), Index), TEXT("x"));
		}
		const FMantleExportResult Result = FMantlePlaceExporter::Export(Fixture.World, FMantleExportOptions());
		TestTrue(TEXT("too many texts refuse the export"), Result.Errors.ContainsByPredicate([](const FMantleParseError& Error)
		{
			return Error.Message.Contains(FString::Printf(TEXT("at most %d"), FMantlePlaceLimits::MaxTextsPerPlace));
		}));
		const FMantleValidationResult Validation = FMantlePlaceValidator::Validate(Fixture.World);
		TestTrue(TEXT("and Check says so as an error"), Validation.Issues.ContainsByPredicate([](const FMantleValidationIssue& Issue)
		{
			return Issue.Severity == EMantleIssueSeverity::Error && Issue.Problem.Contains(TEXT("text number"));
		}));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
