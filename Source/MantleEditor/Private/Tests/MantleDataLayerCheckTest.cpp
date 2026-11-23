// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceValidator.h"
#include "MantlePlaceWorldPartition.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// Data Layers (publish support list row 68). The publish test level L4 showed, on main: an actor in a layer that is switched off in
// the editor is not loaded, and Check refused the whole place for it with cell advice that didn't apply. A Fab map ships optional
// content in layers that are off, so that refusal meant such a map could not publish at all. Now an actor in an off layer is left
// out of the place, as the editor shows it: nothing loads it, the unloaded-cells check doesn't count it, and Check says once per
// layer how many actors are left out. And Mantle has no Data Layers: an actor in a runtime layer publishes as an ordinary actor
// whatever the layer's Initial Runtime State, which one warning now says.

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
		for (const TCHAR* Name : OffLayers)
		{
			Entry.OffLayers.Add(Name);
		}
		return Entry;
	}

	FEntry Loaded(const TCHAR* Label)
	{
		FEntry Entry;
		Entry.Label = Label;
		Entry.bLoaded = true;
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
	FMantleOffDataLayerIsLeftOutTest,
	"Mantle.Editor.Check.AnActorInAnOffDataLayerIsLeftOutWithAWarning",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleOffDataLayerIsLeftOutTest::RunTest(const FString& Parameters)
{
	using namespace MantleDataLayerCheckTestPrivate;

	TArray<FEntry> Entries;
	Entries.Add(Unloaded(TEXT("Cube_Off_1"), { TEXT("Optional Props") }));
	Entries.Add(Unloaded(TEXT("Cube_Off_2"), { TEXT("Optional Props") }));
	Entries.Add(Unloaded(TEXT("Lamp_Off"), { TEXT("Night Lamps") }));
	Entries.Add(Unloaded(TEXT("Far_Rock")));
	Entries.Add(Loaded(TEXT("Cube_On")));
	FEntry Hlod = Unloaded(TEXT("HLOD_Proxy"), { TEXT("Optional Props") });
	Hlod.bHLOD = true;
	Entries.Add(Hlod);

	// Not counted as unloaded: nothing refuses the place for them. The actor in an unloaded cell still is.
	TArray<FString> Examples;
	TestEqual(TEXT("only the actor in an unloaded cell is counted"), FMantlePlaceWorldPartition::CountUnloaded(Entries, Examples), 1);
	TestTrue(TEXT("and it is the rock"), Examples.Num() == 1 && Examples[0] == TEXT("Far_Rock"));

	// One warning per layer, each with its count; the HLOD proxy isn't content.
	const TArray<FMantlePlaceWorldPartition::FOffLayer> Layers = FMantlePlaceWorldPartition::OffLayers(Entries);
	if (!TestEqual(TEXT("two layers are off"), Layers.Num(), 2))
	{
		return false;
	}
	TestTrue(TEXT("sorted by name"), Layers[0].Name == TEXT("Night Lamps") && Layers[1].Name == TEXT("Optional Props"));
	TestEqual(TEXT("one lamp"), Layers[0].ActorCount, 1);
	TestEqual(TEXT("two props"), Layers[1].ActorCount, 2);

	FMantleValidationIssue Props;
	FMantlePlaceWorldPartition::DescribeOffLayer(Layers[1], Props);
	TestTrue(TEXT("a warning, not an error"), Props.Severity == EMantleIssueSeverity::Warning);
	TestTrue(TEXT("for the place"), Props.Actor.IsEmpty());
	TestEqual(TEXT("the words"), Props.Problem, FString(TEXT("Data layer 'Optional Props' is off in the editor, so its 2 actors are left out of the place.")));
	TestTrue(TEXT("with the way to publish them"), Props.Remedy.Contains(TEXT("Turn the layer on")));
	FMantleValidationIssue Lamp;
	FMantlePlaceWorldPartition::DescribeOffLayer(Layers[0], Lamp);
	TestEqual(TEXT("one actor reads in the singular"), Lamp.Problem, FString(TEXT("Data layer 'Night Lamps' is off in the editor, so its 1 actor is left out of the place.")));

	// A level whose only unloaded actors are in off layers has nothing unloaded to refuse or load.
	TArray<FEntry> OnlyOff;
	OnlyOff.Add(Unloaded(TEXT("Cube_Off"), { TEXT("Optional Props") }));
	TestEqual(TEXT("nothing is unloaded"), FMantlePlaceWorldPartition::CountUnloaded(OnlyOff, Examples), 0);
	TestEqual(TEXT("an on layer says nothing"), FMantlePlaceWorldPartition::OffLayers({ Loaded(TEXT("Cube_On")) }).Num(), 0);
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
