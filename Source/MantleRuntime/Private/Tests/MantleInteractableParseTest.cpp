// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceParser.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// An entity's interactable block: the parser reads it, and refuses what schema/validate.py and the backend refuse
// (a key used twice in a list, a cooldown without seconds, a part that names an entity the place does not have).

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleInteractableParseTestPrivate
{
	FString PlaceJson(const FString& Interactable)
	{
		return FString::Printf(
			TEXT("{\"schemaVersion\":1,\"placeId\":\"plc_interact_test\",\"revision\":{\"number\":1,\"state\":\"draft\"},")
				TEXT("\"metadata\":{\"name\":\"Interact test\",\"authorId\":\"usr_test\"},\"settings\":{},\"assets\":[],\"entities\":[")
				TEXT("{\"id\":\"ent_door\",\"type\":\"Group\",\"name\":\"Door\",\"parent\":null,")
				TEXT("\"transform\":{\"location\":[0,0,0],\"rotation\":[0,0,0],\"scale\":[1,1,1]},\"properties\":{},\"interactable\":%s},")
				TEXT("{\"id\":\"ent_handle\",\"type\":\"Group\",\"name\":\"Handle\",\"parent\":\"ent_door\",")
				TEXT("\"transform\":{\"location\":[0,0,0],\"rotation\":[0,0,0],\"scale\":[1,1,1]},\"properties\":{}}]}"),
			*Interactable);
	}

	bool HasErrorAt(const FMantleParseResult& Result, const FString& Path)
	{
		return Result.Errors.ContainsByPredicate([&Path](const FMantleParseError& Error) { return Error.Path == Path; });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleInteractableParseReadsTest,
	"Mantle.Interactable.Parser.ReadsTheBlock",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleInteractableParseReadsTest::RunTest(const FString& Parameters)
{
	using namespace MantleInteractableParseTestPrivate;
	const FMantleParseResult Result = FMantlePlaceParser::ParseFromString(PlaceJson(TEXT(
		"{\"range\":300,\"options\":[{\"key\":\"primary\",\"text\":\"Open\",\"afterUse\":\"cooldown\",\"cooldownSeconds\":2},")
		TEXT("{\"key\":\"secondary\",\"text\":\"Lock\",\"holdSeconds\":1.5}],")
		TEXT("\"display\":{\"style\":\"balanced\",\"title\":\"Door\"},")
		TEXT("\"parts\":[{\"id\":\"handle\",\"anchor\":\"ent_handle\",\"highlight\":[\"ent_handle\"],\"promptOffset\":[0,0,20],")
		TEXT("\"options\":[{\"pressCount\":5}],\"ignoreObstacles\":true}]}")));
	if (!TestTrue(TEXT("a place with an interactable block parses"), Result.bSuccess))
	{
		AddError(Result.ErrorSummary());
		return false;
	}
	const FMantleEntity& Door = Result.Place.Entities[0];
	if (!TestTrue(TEXT("the door is interactable"), Door.Interactable.IsSet()))
	{
		return false;
	}
	const FMantleInteractableSpec& Spec = *Door.Interactable;
	TestEqual(TEXT("range"), Spec.Range, 300.0);
	if (TestEqual(TEXT("two options"), Spec.Options.Num(), 2))
	{
		TestTrue(TEXT("the first uses the primary key"), !Spec.Options[0].bSecondaryKey);
		TestTrue(TEXT("its cooldown"), Spec.Options[0].AfterUse == EMantleInteractableAfterUse::Cooldown && Spec.Options[0].CooldownSeconds == 2.0);
		TestTrue(TEXT("the second uses the secondary key"), Spec.Options[1].bSecondaryKey);
		TestEqual(TEXT("its hold"), Spec.Options[1].HoldSeconds, 1.5);
	}
	TestTrue(TEXT("balanced style"), Spec.Display.Style == EMantleInteractablePromptStyle::Balanced);
	TestEqual(TEXT("title"), Spec.Display.Title, FString(TEXT("Door")));
	if (TestEqual(TEXT("one part"), Spec.Parts.Num(), 1))
	{
		const FMantleInteractablePartSpec& Part = Spec.Parts[0];
		TestEqual(TEXT("part id"), Part.Id, FString(TEXT("handle")));
		TestEqual(TEXT("anchor"), Part.AnchorEntity, FString(TEXT("ent_handle")));
		TestEqual(TEXT("highlight"), Part.HighlightEntities.Num(), 1);
		TestEqual(TEXT("prompt offset"), Part.PromptOffset.Z, 20.0);
		TestTrue(TEXT("ignores obstacles"), Part.bIgnoreObstacles);
		if (TestEqual(TEXT("part options"), Part.Options.Num(), 1))
		{
			TestEqual(TEXT("mash count"), Part.Options[0].PressCount, 5);
		}
	}
	TestFalse(TEXT("an entity without a block has none"), Result.Place.Entities[1].Interactable.IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleInteractableParseRefusesTest,
	"Mantle.Interactable.Parser.RefusesWhatTheSchemaRefuses",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleInteractableParseRefusesTest::RunTest(const FString& Parameters)
{
	using namespace MantleInteractableParseTestPrivate;
	const FString Base = TEXT("/entities/0/interactable");

	FMantleParseResult Result = FMantlePlaceParser::ParseFromString(PlaceJson(TEXT(
		"{\"options\":[{\"key\":\"primary\"},{\"key\":\"primary\"}]}")));
	TestFalse(TEXT("a key used twice is refused"), Result.bSuccess);
	TestTrue(TEXT("at the second option's key"), HasErrorAt(Result, Base + TEXT("/options/1/key")));

	Result = FMantlePlaceParser::ParseFromString(PlaceJson(TEXT("{\"options\":[{\"afterUse\":\"cooldown\"}]}")));
	TestFalse(TEXT("a cooldown without seconds is refused"), Result.bSuccess);
	TestTrue(TEXT("at cooldownSeconds"), HasErrorAt(Result, Base + TEXT("/options/0/cooldownSeconds")));

	Result = FMantlePlaceParser::ParseFromString(PlaceJson(TEXT("{\"parts\":[{\"id\":\"a\",\"anchor\":\"ent_missing\"}]}")));
	TestFalse(TEXT("an anchor that is not an entity is refused"), Result.bSuccess);
	TestTrue(TEXT("at the anchor"), HasErrorAt(Result, Base + TEXT("/parts/0/anchor")));

	Result = FMantlePlaceParser::ParseFromString(PlaceJson(TEXT("{\"parts\":[{\"id\":\"a\"},{\"id\":\"a\"}]}")));
	TestFalse(TEXT("a part id used twice is refused"), Result.bSuccess);
	TestTrue(TEXT("at the second id"), HasErrorAt(Result, Base + TEXT("/parts/1/id")));

	Result = FMantlePlaceParser::ParseFromString(PlaceJson(TEXT("{\"glow\":true}")));
	TestFalse(TEXT("an unknown key is refused"), Result.bSuccess);
	TestTrue(TEXT("at the key"), HasErrorAt(Result, Base + TEXT("/glow")));
	return true;
}

#endif
