// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCoverage.h"
#include "MantleCoverageSurvey.h"
#include "MantleEnvironmentExport.h"
#include "MantlePlaceExporter.h"
#include "MantlePlaceValidator.h"
#include "MantleTargetFeatures.h"

#include "Components/ExponentialHeightFogComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/PointLight.h"
#include "Engine/PostProcessVolume.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/TextRenderActor.h"
#include "Engine/World.h"
#include "Engine/BlockingVolume.h"
#include "Misc/AutomationTest.h"
#include "ProceduralFoliageBlockingVolume.h"
#include "ProceduralFoliageVolume.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

// The export coverage report: the FMantleCoverage type, and what Export reports for the classifications this
// change touched (a Post Process Volume is a warning and a dropped line, a procedural foliage volume is a note).

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleCoverageTestPrivate
{
	using EOutcome = EMantleCoverageOutcome;

	/** An empty editor world for one test, destroyed with it. */
	struct FTestWorld
	{
		UWorld* World = nullptr;

		FTestWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld*/ false, TEXT("MantleCoverageTestWorld"));
		}

		~FTestWorld()
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

		template <typename T>
		T* Spawn(const TCHAR* Label)
		{
			T* Actor = World ? World->SpawnActor<T>() : nullptr;
			if (Actor)
			{
				Actor->SetActorLabel(Label);
			}
			return Actor;
		}
	};

	TSharedPtr<FJsonObject> ReadObject(const FString& Json)
	{
		TSharedPtr<FJsonObject> Root;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root);
		return Root;
	}

	/** Whether any validation issue of the given severity is about the actor labelled Label. */
	bool HasIssue(const FMantleValidationResult& Validation, const FString& Label, EMantleIssueSeverity Severity)
	{
		return Validation.Issues.ContainsByPredicate([&](const FMantleValidationIssue& Issue)
		{
			return Issue.Actor == Label && Issue.Severity == Severity;
		});
	}

	/** Whether any export error mentions Text. */
	bool ErrorMentions(const FMantleExportResult& Result, const FString& Text)
	{
		return Result.Errors.ContainsByPredicate([&Text](const FMantleParseError& Error)
		{
			return Error.Message.Contains(Text) || Error.Path.Contains(Text);
		});
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCoverageCountsTest,
	"Mantle.Editor.Coverage.CountsAndExamples",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCoverageCountsTest::RunTest(const FString& Parameters)
{
	using namespace MantleCoverageTestPrivate;
	FMantleCoverage Coverage;
	TestTrue(TEXT("a new report is empty"), Coverage.IsEmpty());
	TestEqual(TEXT("and counts nothing"), Coverage.Count(MantleCoverageCategory::StaticMeshes, EOutcome::Kept), static_cast<int64>(0));

	Coverage.Add(MantleCoverageCategory::StaticMeshes, EOutcome::Kept, TEXT("Rock A"));
	Coverage.Add(MantleCoverageCategory::StaticMeshes, EOutcome::Kept, TEXT("Rock A"));
	Coverage.Add(MantleCoverageCategory::StaticMeshes, EOutcome::Approximated, TEXT("Rock B"), TEXT("baked"));
	TestEqual(TEXT("kept counts both"), Coverage.Count(MantleCoverageCategory::StaticMeshes, EOutcome::Kept), static_cast<int64>(2));
	TestEqual(TEXT("approximated counts one"), Coverage.Count(MantleCoverageCategory::StaticMeshes, EOutcome::Approximated), static_cast<int64>(1));
	TestEqual(TEXT("dropped counts none"), Coverage.Count(MantleCoverageCategory::StaticMeshes, EOutcome::Dropped), static_cast<int64>(0));
	const FMantleCoverageCategory* Meshes = Coverage.Find(MantleCoverageCategory::StaticMeshes);
	if (TestNotNull(TEXT("the category exists"), Meshes))
	{
		const FMantleCoverageBucket& Kept = Meshes->Get(EOutcome::Kept);
		TestEqual(TEXT("the same name merges into one example"), Kept.Examples.Num(), 1);
		if (Kept.Examples.Num() == 1)
		{
			TestEqual(TEXT("whose count adds up"), Kept.Examples[0].Count, static_cast<int64>(2));
		}
	}

	// A bulk count with no name, and a count of zero.
	Coverage.Add(MantleCoverageCategory::PaintedFoliage, EOutcome::Kept, FString(), FString(), 41642);
	Coverage.Add(MantleCoverageCategory::Particles, EOutcome::Dropped, TEXT("Fire"), FString(), 0);
	TestEqual(TEXT("a bulk add counts every one"), Coverage.Count(MantleCoverageCategory::PaintedFoliage, EOutcome::Kept), static_cast<int64>(41642));
	TestFalse(TEXT("a count of zero adds nothing"), Coverage.HasCategory(MantleCoverageCategory::Particles));

	// Past MaxExamples names, the count still covers every one.
	for (int32 Index = 0; Index < 25; ++Index)
	{
		Coverage.Add(MantleCoverageCategory::Decals, EOutcome::Dropped, FString::Printf(TEXT("Decal %d"), Index), TEXT("why"));
	}
	TestEqual(TEXT("every drop is counted"), Coverage.Count(MantleCoverageCategory::Decals, EOutcome::Dropped), static_cast<int64>(25));
	if (const FMantleCoverageCategory* Decals = Coverage.Find(MantleCoverageCategory::Decals))
	{
		TestEqual(TEXT("but only the first few are named"), Decals->Get(EOutcome::Dropped).Examples.Num(), FMantleCoverage::MaxExamples);
	}
	TestEqual(TEXT("totals add the categories up"), Coverage.Total(EOutcome::Dropped), static_cast<int64>(25));
	TestEqual(TEXT("and the kept ones"), Coverage.Total(EOutcome::Kept), static_cast<int64>(41644));
	TestEqual(TEXT("the summary names what was dropped and approximated"), Coverage.ToSummary(), FString(TEXT("25 dropped, 1 approximated")));

	FMantleCoverage Nothing;
	Nothing.Add(MantleCoverageCategory::StaticMeshes, EOutcome::Kept, TEXT("A"));
	Nothing.Add(MantleCoverageCategory::Volumes, EOutcome::Info, TEXT("Generator"));
	TestEqual(TEXT("kept and info only is nothing dropped"), Nothing.ToSummary(), FString(TEXT("nothing dropped")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCoverageLogLinesTest,
	"Mantle.Editor.Coverage.LogLines",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCoverageLogLinesTest::RunTest(const FString& Parameters)
{
	using namespace MantleCoverageTestPrivate;
	TestEqual(TEXT("a category is one token"), FMantleCoverage::Slug(TEXT("Landscape grass")), FString(TEXT("landscape-grass")));
	TestEqual(TEXT("its hyphen stays one"), FMantleCoverage::Slug(TEXT("Post-process")), FString(TEXT("post-process")));
	TestEqual(TEXT("and punctuation runs collapse"), FMantleCoverage::Slug(TEXT("Groups and gameplay  actors!")), FString(TEXT("groups-and-gameplay-actors")));

	FMantleCoverage Coverage;
	Coverage.Add(MantleCoverageCategory::StaticMeshes, EOutcome::Kept, TEXT("Rock A"));
	Coverage.Add(MantleCoverageCategory::StaticMeshes, EOutcome::Kept, TEXT("Rock B"));
	Coverage.Add(MantleCoverageCategory::StaticMeshes, EOutcome::Approximated, TEXT("Rock C"), TEXT("baked flat"));
	Coverage.Add(MantleCoverageCategory::StaticMeshes, EOutcome::Dropped, TEXT("Rock\nD"), TEXT("hidden"));
	Coverage.Add(MantleCoverageCategory::Volumes, EOutcome::Info, TEXT("Foliage Volume"), TEXT("generator"));

	const TArray<FString> Lines = Coverage.ToLogLines();
	TestTrue(TEXT("the category line follows the contract"),
		Lines.Contains(TEXT("MANTLE_COVERAGE static-meshes kept=2 approx=1 dropped=1")));
	TestTrue(TEXT("info is appended only when there is some"),
		Lines.Contains(TEXT("MANTLE_COVERAGE volumes kept=0 approx=0 dropped=0 info=1")));
	TestTrue(TEXT("an approximation is named with its detail"),
		Lines.Contains(TEXT("MANTLE_COVERAGE_ITEM static-meshes approx Rock C: baked flat")));
	TestTrue(TEXT("a drop is named, on one line"),
		Lines.Contains(TEXT("MANTLE_COVERAGE_ITEM static-meshes dropped Rock D: hidden")));
	TestFalse(TEXT("what was kept is counted, not listed"),
		Lines.ContainsByPredicate([](const FString& Line) { return Line.StartsWith(TEXT("MANTLE_COVERAGE_ITEM")) && Line.Contains(TEXT(" kept ")); }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCoverageJsonTest,
	"Mantle.Editor.Coverage.Json",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCoverageJsonTest::RunTest(const FString& Parameters)
{
	using namespace MantleCoverageTestPrivate;
	FMantleCoverage Coverage;
	Coverage.Add(MantleCoverageCategory::Terrain, EOutcome::Kept, TEXT("Landscape1"));
	Coverage.Add(MantleCoverageCategory::LandscapeGrass, EOutcome::Dropped, TEXT("LGT_Grass"), TEXT("10 varieties"));
	Coverage.Add(MantleCoverageCategory::LandscapeGrass, EOutcome::Dropped, TEXT("LGT_Flowers"), TEXT("2 varieties"));

	const TSharedPtr<FJsonObject> Root = ReadObject(Coverage.ToJson());
	if (!TestTrue(TEXT("coverage.json is valid JSON"), Root.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("it has a schema version"), static_cast<int32>(Root->GetNumberField(TEXT("schemaVersion"))), 1);
	const TSharedPtr<FJsonObject> Totals = Root->GetObjectField(TEXT("totals"));
	TestEqual(TEXT("totals: kept"), static_cast<int32>(Totals->GetNumberField(TEXT("kept"))), 1);
	TestEqual(TEXT("totals: dropped"), static_cast<int32>(Totals->GetNumberField(TEXT("dropped"))), 2);
	TestTrue(TEXT("totals: every outcome is present"), Totals->HasField(TEXT("approx")) && Totals->HasField(TEXT("info")));

	const TArray<TSharedPtr<FJsonValue>>* Categories = nullptr;
	if (!TestTrue(TEXT("categories is an array"), Root->TryGetArrayField(TEXT("categories"), Categories)) || !TestEqual(TEXT("one entry per category"), Categories->Num(), 2))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Grass = (*Categories)[1]->AsObject();
	TestEqual(TEXT("the category is named"), Grass->GetStringField(TEXT("category")), FString(MantleCoverageCategory::LandscapeGrass));
	const TSharedPtr<FJsonObject> Dropped = Grass->GetObjectField(TEXT("dropped"));
	TestEqual(TEXT("an outcome has its count"), static_cast<int32>(Dropped->GetNumberField(TEXT("count"))), 2);
	const TArray<TSharedPtr<FJsonValue>>& Items = Dropped->GetArrayField(TEXT("items"));
	if (TestEqual(TEXT("and one item per name"), Items.Num(), 2))
	{
		const TSharedPtr<FJsonObject> First = Items[0]->AsObject();
		TestEqual(TEXT("an item has its name"), First->GetStringField(TEXT("name")), FString(TEXT("LGT_Grass")));
		TestEqual(TEXT("its detail"), First->GetStringField(TEXT("detail")), FString(TEXT("10 varieties")));
		TestEqual(TEXT("and its count"), static_cast<int32>(First->GetNumberField(TEXT("count"))), 1);
	}
	TestTrue(TEXT("an outcome with nothing is still written"), Grass->GetObjectField(TEXT("kept"))->GetArrayField(TEXT("items")).Num() == 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCoverageSortAppendTest,
	"Mantle.Editor.Coverage.SortAndAppend",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCoverageSortAppendTest::RunTest(const FString& Parameters)
{
	using namespace MantleCoverageTestPrivate;
	FMantleCoverage Coverage;
	Coverage.Add(TEXT("Zebras"), EOutcome::Kept, TEXT("Z"));
	Coverage.Add(MantleCoverageCategory::Sounds, EOutcome::Kept, TEXT("S"));
	Coverage.Add(MantleCoverageCategory::StaticMeshes, EOutcome::Kept, TEXT("M"));
	Coverage.Add(TEXT("Aardvarks"), EOutcome::Kept, TEXT("A"));
	Coverage.Sort();
	const TArray<FMantleCoverageCategory>& Sorted = Coverage.GetCategories();
	if (TestEqual(TEXT("four categories"), Sorted.Num(), 4))
	{
		TestEqual(TEXT("the named ones come in the report's order"), Sorted[0].Name, FString(MantleCoverageCategory::StaticMeshes));
		TestEqual(TEXT("sounds after meshes"), Sorted[1].Name, FString(MantleCoverageCategory::Sounds));
		TestEqual(TEXT("others keep the order they were added in, after"), Sorted[2].Name, FString(TEXT("Zebras")));
		TestEqual(TEXT("(last)"), Sorted[3].Name, FString(TEXT("Aardvarks")));
	}

	FMantleCoverage Other;
	Other.Add(MantleCoverageCategory::StaticMeshes, EOutcome::Kept, TEXT("M"), FString(), 4);
	Other.Add(MantleCoverageCategory::Particles, EOutcome::Dropped, TEXT("Fire"), TEXT("no place form"));
	Coverage.Append(Other);
	TestEqual(TEXT("appending adds counts"), Coverage.Count(MantleCoverageCategory::StaticMeshes, EOutcome::Kept), static_cast<int64>(5));
	TestEqual(TEXT("and categories"), Coverage.Count(MantleCoverageCategory::Particles, EOutcome::Dropped), static_cast<int64>(1));

	const TArray<FString> Display = Coverage.ToDisplayLines();
	TestTrue(TEXT("the panel text has a line per category"), Display.Contains(TEXT("Particles: 1 dropped")));
	TestTrue(TEXT("and names what was dropped under it"), Display.Contains(TEXT("    dropped: Fire: no place form")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCoverageGeneratorVolumeTest,
	"Mantle.Editor.Coverage.ProceduralFoliageVolumeIsAnEditorGenerator",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCoverageGeneratorVolumeTest::RunTest(const FString& Parameters)
{
	using namespace MantleCoverageTestPrivate;
	FTestWorld Test;
	if (!TestNotNull(TEXT("a test world"), Test.World))
	{
		return false;
	}
	AProceduralFoliageVolume* Generator = Test.Spawn<AProceduralFoliageVolume>(TEXT("ForestGenerator"));
	AProceduralFoliageBlockingVolume* Blocker = Test.Spawn<AProceduralFoliageBlockingVolume>(TEXT("ForestBlocker"));
	ABlockingVolume* Plain = Test.Spawn<ABlockingVolume>(TEXT("PlainVolume"));
	if (!TestNotNull(TEXT("the generator"), Generator) || !TestNotNull(TEXT("the blocker"), Blocker) || !TestNotNull(TEXT("a plain volume"), Plain))
	{
		return false;
	}

	TestTrue(TEXT("a procedural foliage volume is an editor generator"), FMantleCoverageSurvey::IsEditorGeneratorVolume(Generator));
	TestTrue(TEXT("and so is its blocking volume"), FMantleCoverageSurvey::IsEditorGeneratorVolume(Blocker));
	TestFalse(TEXT("a plain volume is not"), FMantleCoverageSurvey::IsEditorGeneratorVolume(Plain));

	// They are not "a volume Mantle does not publish": no warning telling the creator to use a Trigger Box.
	FString What;
	FString Remedy;
	TestFalse(TEXT("the generator has nothing to warn about"), FMantlePlaceExporter::DescribeUnpublishedContent(Generator, What, Remedy));
	TestFalse(TEXT("nor has the blocking volume"), FMantlePlaceExporter::DescribeUnpublishedContent(Blocker, What, Remedy));
	TestTrue(TEXT("a plain volume still does"), FMantlePlaceExporter::DescribeUnpublishedContent(Plain, What, Remedy));

	const FMantleValidationResult Validation = FMantlePlaceValidator::Validate(Test.World);
	TestFalse(TEXT("Check says nothing about the generator"), HasIssue(Validation, TEXT("ForestGenerator"), EMantleIssueSeverity::Warning) || HasIssue(Validation, TEXT("ForestGenerator"), EMantleIssueSeverity::Error));
	TestFalse(TEXT("nor the blocking volume"), HasIssue(Validation, TEXT("ForestBlocker"), EMantleIssueSeverity::Warning) || HasIssue(Validation, TEXT("ForestBlocker"), EMantleIssueSeverity::Error));
	TestTrue(TEXT("the plain volume is a warning"), HasIssue(Validation, TEXT("PlainVolume"), EMantleIssueSeverity::Warning));

	// The report lists them as info: nothing was dropped that a player would miss.
	const FMantleExportResult Exported = FMantlePlaceExporter::Export(Test.World, FMantleExportOptions());
	TestFalse(TEXT("Export refuses neither"), ErrorMentions(Exported, TEXT("ProceduralFoliage")));
	TestEqual(TEXT("the report counts both as nothing to export"), Exported.Coverage.Count(MantleCoverageCategory::Volumes, EOutcome::Info), static_cast<int64>(2));
	TestEqual(TEXT("and only the plain volume as dropped"), Exported.Coverage.Count(MantleCoverageCategory::Volumes, EOutcome::Dropped), static_cast<int64>(1));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCoveragePostProcessVolumeTest,
	"Mantle.Editor.Coverage.PostProcessVolumeIsAWarning",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCoveragePostProcessVolumeTest::RunTest(const FString& Parameters)
{
	using namespace MantleCoverageTestPrivate;
	FTestWorld Test;
	if (!TestNotNull(TEXT("a test world"), Test.World))
	{
		return false;
	}
	APostProcessVolume* Volume = Test.Spawn<APostProcessVolume>(TEXT("GradeVolume"));
	if (!TestNotNull(TEXT("the post process volume"), Volume))
	{
		return false;
	}
	// A lens flare the place has no setting for (the exposure is carried): nothing of the volume can be carried.
	Volume->Settings.bOverride_LensFlareIntensity = true;
	Volume->Settings.LensFlareIntensity = 1.0f;

	FString What;
	FString Remedy;
	TestTrue(TEXT("a post process volume is named as left out"), FMantlePlaceExporter::DescribeUnpublishedContent(Volume, What, Remedy));
	TestTrue(TEXT("saying the place uses Mantle's own post-process"), What.Contains(TEXT("Mantle's own post-process")));
	TestFalse(TEXT("and never advising a Trigger Box"), What.Contains(TEXT("Trigger Box")) || Remedy.Contains(TEXT("Trigger Box")));
	TestTrue(TEXT("it is infrastructure, so Export skips it rather than refusing it"), FMantlePlaceExporter::IsEngineInfrastructure(Volume));

	const FMantleValidationResult Validation = FMantlePlaceValidator::Validate(Test.World);
	TestTrue(TEXT("Check warns about it"), HasIssue(Validation, TEXT("GradeVolume"), EMantleIssueSeverity::Warning));
	TestFalse(TEXT("and does not refuse the level for it"), HasIssue(Validation, TEXT("GradeVolume"), EMantleIssueSeverity::Error));

	const FMantleExportResult Exported = FMantlePlaceExporter::Export(Test.World, FMantleExportOptions());
	TestFalse(TEXT("Export has no error about it"), ErrorMentions(Exported, TEXT("PostProcessVolume")));
	// An auto-exposure project's default exposure is carried as the place's, so the volume is then approximated, not dropped.
	const bool bExposureCarried = FMantleExposureProject::Current().bAuto;
	TestEqual(TEXT("the report lists it as dropped, when no exposure of it is carried"),
		Exported.Coverage.Count(MantleCoverageCategory::PostProcess, EOutcome::Dropped), static_cast<int64>(bExposureCarried ? 0 : 1));
	TestEqual(TEXT("and as approximated when one is"),
		Exported.Coverage.Count(MantleCoverageCategory::PostProcess, EOutcome::Approximated), static_cast<int64>(bExposureCarried ? 1 : 0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCoverageTextRenderActorTest,
	"Mantle.Editor.Coverage.TextRenderActorIsAWarningWithoutWorldText",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCoverageTextRenderActorTest::RunTest(const FString& Parameters)
{
	using namespace MantleCoverageTestPrivate;
	FTestWorld Test;
	if (!TestNotNull(TEXT("a test world"), Test.World))
	{
		return false;
	}
	// GhzGarage/mantle#724: the actor publishes as a Text entity; it is only a warning for a target that cannot read world text.
	const FMantleTargetFeaturesScope Scope(FMantleTargetFeatures::None());
	ATextRenderActor* Text = Test.Spawn<ATextRenderActor>(TEXT("SignLabel"));
	if (!TestNotNull(TEXT("the text render actor"), Text))
	{
		return false;
	}

	FString What;
	FString Remedy;
	TestTrue(TEXT("a text render actor is named as left out"), FMantlePlaceExporter::DescribeUnpublishedContent(Text, What, Remedy));
	TestTrue(TEXT("it is infrastructure, so Export skips it rather than refusing it"), FMantlePlaceExporter::IsEngineInfrastructure(Text));

	const FMantleValidationResult Validation = FMantlePlaceValidator::Validate(Test.World);
	TestTrue(TEXT("Check warns about it"), HasIssue(Validation, TEXT("SignLabel"), EMantleIssueSeverity::Warning));
	TestFalse(TEXT("and does not refuse the level for it"), HasIssue(Validation, TEXT("SignLabel"), EMantleIssueSeverity::Error));

	const FMantleExportResult Exported = FMantlePlaceExporter::Export(Test.World, FMantleExportOptions());
	TestFalse(TEXT("Export has no error about it"), ErrorMentions(Exported, TEXT("TextRenderActor")));
	TestEqual(TEXT("the report lists it as dropped"), Exported.Coverage.Count(MantleCoverageCategory::OtherActors, EOutcome::Dropped), static_cast<int64>(1));
	TestTrue(TEXT("saying the target needs a newer client"), What.Contains(FMantleTargetFeatures::NeedsNewerClient));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCoverageExportTest,
	"Mantle.Editor.Coverage.ExportReportsWhatItKeepsAndDrops",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCoverageExportTest::RunTest(const FString& Parameters)
{
	using namespace MantleCoverageTestPrivate;
	FTestWorld Test;
	if (!TestNotNull(TEXT("a test world"), Test.World))
	{
		return false;
	}
	// A mesh actor with no mesh is skipped (#818), so the crate gets the engine cube.
	if (AStaticMeshActor* Crate = Test.Spawn<AStaticMeshActor>(TEXT("Crate")))
	{
		Crate->GetStaticMeshComponent()->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
	}
	Test.Spawn<APointLight>(TEXT("Lamp"));
	// A fog the creator tuned in a way the place has no setting for (its density, falloff, colours and start are carried): it is dropped (an untouched one is kept, see the Mantle.Editor.EnvironmentExport tests).
	if (AExponentialHeightFog* Mist = Test.Spawn<AExponentialHeightFog>(TEXT("Mist")))
	{
		Mist->GetComponent()->FogMaxOpacity = 0.5f;
	}

	const FMantleExportResult Exported = FMantlePlaceExporter::Export(Test.World, FMantleExportOptions());
	const FMantleCoverage& Coverage = Exported.Coverage;
	TestEqual(TEXT("a static mesh actor is kept"), Coverage.Count(MantleCoverageCategory::StaticMeshes, EOutcome::Kept), static_cast<int64>(1));
	TestEqual(TEXT("a point light is kept"), Coverage.Count(MantleCoverageCategory::Lights, EOutcome::Kept), static_cast<int64>(1));
	TestEqual(TEXT("fog is replaced by Mantle's, so it is dropped"), Coverage.Count(MantleCoverageCategory::Fog, EOutcome::Dropped), static_cast<int64>(1));
	const FMantleCoverageCategory* Fog = Coverage.Find(MantleCoverageCategory::Fog);
	if (TestNotNull(TEXT("the fog line"), Fog) && TestEqual(TEXT("with its actor named"), Fog->Get(EOutcome::Dropped).Examples.Num(), 1))
	{
		TestEqual(TEXT("by label"), Fog->Get(EOutcome::Dropped).Examples[0].Name, FString(TEXT("Mist")));
	}

	// The categories come out in the report's order, whatever order the survey met them in.
	const TArray<FMantleCoverageCategory>& Categories = Coverage.GetCategories();
	int32 MeshesAt = INDEX_NONE;
	int32 FogAt = INDEX_NONE;
	for (int32 Index = 0; Index < Categories.Num(); ++Index)
	{
		MeshesAt = Categories[Index].Name == MantleCoverageCategory::StaticMeshes ? Index : MeshesAt;
		FogAt = Categories[Index].Name == MantleCoverageCategory::Fog ? Index : FogAt;
	}
	TestTrue(TEXT("meshes are listed before fog"), MeshesAt != INDEX_NONE && FogAt != INDEX_NONE && MeshesAt < FogAt);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
