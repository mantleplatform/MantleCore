// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceValidator.h"
#include "MantlePlaceWorldPartition.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// Data Layers (publish support list row 68). The publish test level L4 showed, on main: an actor in a layer that is switched off in
// the editor is not loaded, so Check refused the place, but named "cells the editor has not loaded" and gave cell advice that
// didn't apply. And Mantle has no Data Layers at all: an actor in a runtime layer publishes as an ordinary actor whatever the
// layer's Initial Runtime State, which nothing said.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleDataLayerCheckTestPrivate
{
	using FEntry = FMantlePlaceWorldPartition::FActorEntry;
	using FLayer = FMantlePlaceWorldPartition::FLayerEntry;

	FEntry Unloaded(const TCHAR* Label, std::initializer_list<const TCHAR*> OffLayers = {})
	{
		FEntry Entry;
		Entry.Label = Label;
		Entry.bLoaded = false;
		for (const TCHAR* Layer : OffLayers)
		{
			Entry.OffLayers.Add(Layer);
		}
		return Entry;
	}

	FLayer Layer(const TCHAR* Name, bool bRuntime, const TCHAR* State = TEXT(""))
	{
		FLayer Out;
		Out.Name = Name;
		Out.bRuntime = bRuntime;
		Out.InitialState = State;
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleDataLayerOffIsNamedTest,
	"Mantle.Editor.Check.AnActorInAnOffDataLayerIsNamedWithItsLayer",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleDataLayerOffIsNamedTest::RunTest(const FString& Parameters)
{
	using namespace MantleDataLayerCheckTestPrivate;

	TArray<FEntry> Entries;
	Entries.Add(Unloaded(TEXT("L4_Cube_EditorUnloaded"), { TEXT("DL_PT_EditorUnloaded") }));
	Entries.Add(Unloaded(TEXT("L4_FarCell_Rock")));
	FEntry Loaded;
	Loaded.Label = TEXT("L4_Cube_EditorLoaded");
	Loaded.bLoaded = true;
	Entries.Add(Loaded);
	FEntry Hlod = Unloaded(TEXT("HLOD_Proxy"), { TEXT("DL_PT_EditorUnloaded") });
	Hlod.bHLOD = true;
	Entries.Add(Hlod);

	const FMantlePlaceWorldPartition::FUnloadedSplit Split = FMantlePlaceWorldPartition::SplitUnloaded(Entries);
	TestEqual(TEXT("one actor is out because its layer is off"), Split.LayerCount, 1);
	TestEqual(TEXT("one is out because its cell is not loaded"), Split.CellCount, 1);
	TestTrue(TEXT("the cell one is the rock"), Split.CellExamples.Num() == 1 && Split.CellExamples[0] == TEXT("L4_FarCell_Rock"));
	TestTrue(TEXT("the layer one is the cube"), Split.LayerExamples.Num() == 1 && Split.LayerExamples[0] == TEXT("L4_Cube_EditorUnloaded"));
	TestTrue(TEXT("and the layer is named once"), Split.LayerNames.Num() == 1 && Split.LayerNames[0] == TEXT("DL_PT_EditorUnloaded"));

	const FString Problem = FMantlePlaceWorldPartition::DescribeLayerProblem(Split);
	TestTrue(TEXT("the problem names the actor"), Problem.Contains(TEXT("L4_Cube_EditorUnloaded")));
	TestTrue(TEXT("and the layer"), Problem.Contains(TEXT("DL_PT_EditorUnloaded")));
	TestTrue(TEXT("and says it is switched off in the editor"), Problem.Contains(TEXT("switched off in the editor")));
	TestFalse(TEXT("and says nothing about cells"), Problem.Contains(TEXT("cell")));
	const FString Remedy = FMantlePlaceWorldPartition::DescribeLayerRemedy();
	TestTrue(TEXT("the remedy is the Data Layers window"), Remedy.Contains(TEXT("Data Layers window")));
	TestFalse(TEXT("not the cell advice"), Remedy.Contains(TEXT("Load Region")));

	// Several layers and actors read in the plural.
	TArray<FEntry> Many;
	Many.Add(Unloaded(TEXT("A"), { TEXT("Layer One") }));
	Many.Add(Unloaded(TEXT("B"), { TEXT("Layer Two") }));
	const FString Plural = FMantlePlaceWorldPartition::DescribeLayerProblem(FMantlePlaceWorldPartition::SplitUnloaded(Many));
	TestTrue(TEXT("two actors in two layers"), Plural.StartsWith(TEXT("2 actors")) && Plural.Contains(TEXT("Data Layers that are switched off")) && Plural.Contains(TEXT("Layer One, Layer Two")));

	// Nothing unloaded says nothing.
	TestEqual(TEXT("a loaded level has no layer problem"), FMantlePlaceWorldPartition::SplitUnloaded({ Loaded }).LayerCount, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleRuntimeDataLayerWarningTest,
	"Mantle.Editor.Check.RuntimeDataLayersAreSaidToBeIgnored",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleRuntimeDataLayerWarningTest::RunTest(const FString& Parameters)
{
	using namespace MantleDataLayerCheckTestPrivate;

	FMantleValidationIssue Issue;
	TestFalse(TEXT("no layers, nothing to say"), FMantlePlaceWorldPartition::DescribeRuntimeLayers({}, Issue));
	TestFalse(TEXT("layers that only organize the editor, nothing to say"),
		FMantlePlaceWorldPartition::DescribeRuntimeLayers({ Layer(TEXT("Lighting"), false), Layer(TEXT("Props"), false) }, Issue));

	const TArray<FLayer> Layers = { Layer(TEXT("Lighting"), false), Layer(TEXT("Night"), true, TEXT("Unloaded")), Layer(TEXT("Crowd"), true, TEXT("Activated")) };
	if (TestTrue(TEXT("runtime layers are said"), FMantlePlaceWorldPartition::DescribeRuntimeLayers(Layers, Issue)))
	{
		TestTrue(TEXT("as a warning"), Issue.Severity == EMantleIssueSeverity::Warning);
		TestTrue(TEXT("for the whole place"), Issue.Actor.IsEmpty());
		TestTrue(TEXT("naming each runtime layer with its starting state"), Issue.Problem.Contains(TEXT("Night (starts Unloaded)")) && Issue.Problem.Contains(TEXT("Crowd (starts Activated)")));
		TestFalse(TEXT("but not the editor layer"), Issue.Problem.Contains(TEXT("Lighting")));
		TestTrue(TEXT("saying the state is ignored"), Issue.Problem.Contains(TEXT("Initial Runtime State is ignored")));
		TestTrue(TEXT("and what the creator can do"), Issue.Remedy.Contains(TEXT("come out of the level")));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
