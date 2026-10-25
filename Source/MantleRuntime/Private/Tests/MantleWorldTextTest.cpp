// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceBuilder.h"
#include "MantlePlaceLimits.h"
#include "MantlePlaceParser.h"

#include "Components/TextRenderComponent.h"
#include "Engine/Font.h"
#include "Engine/TextRenderActor.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// World text (#724): what the parser reads and refuses, and that the builder makes a Text Render component in the
// built-in font with every key applied. How the text looks beside the editor's is the visual check in a packaged client.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleWorldTextTestPrivate
{
	/** A place whose entities are Entities (the text between the brackets). */
	FMantleParseResult ParseEntities(const FString& Entities)
	{
		const FString Json = FString::Printf(
			TEXT("{\"schemaVersion\":1,\"placeId\":\"plc_signs001\",\"revision\":{\"number\":1,\"state\":\"draft\"},"
				 "\"metadata\":{\"name\":\"Signposts\",\"authorId\":\"usr_4b1d\"},\"settings\":{},\"assets\":[],\"entities\":[%s]}"),
			*Entities);
		return FMantlePlaceParser::ParseFromString(Json);
	}

	/** One Text entity (id ent_text_<Number>) with Properties (the text between the braces). */
	FString TextEntity(int32 Number, const FString& Properties)
	{
		return FString::Printf(
			TEXT("{\"id\":\"ent_text_%d\",\"type\":\"Text\",\"parent\":null,"
				 "\"transform\":{\"location\":[%d,0,0],\"rotation\":[0,0,0],\"scale\":[1,1,1]},\"properties\":{%s}}"),
			Number, Number * 100, *Properties);
	}

	FMantleParseResult ParseTextEntity(const FString& Properties)
	{
		return ParseEntities(TextEntity(0, Properties));
	}

	const FMantleTextProps* TextOf(const FMantleParseResult& Result, int32 Index = 0)
	{
		return Result.Place.Entities.IsValidIndex(Index) ? Result.Place.Entities[Index].Props.TryGet<FMantleTextProps>() : nullptr;
	}

	bool HasErrorAt(const FMantleParseResult& Result, const FString& Path)
	{
		return Result.Errors.ContainsByPredicate([&Path](const FMantleParseError& Error) { return Error.Path == Path; });
	}

	/** An empty game world for one test, destroyed with it. */
	struct FTextWorld
	{
		UWorld* World = nullptr;

		FTextWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("MantleWorldTextTestWorld"));
		}

		~FTextWorld()
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
	FMantleWorldTextParseTest,
	"Mantle.Runtime.WorldText.Parser.ReadsEveryKey",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWorldTextParseTest::RunTest(const FString& Parameters)
{
	using namespace MantleWorldTextTestPrivate;

	TestEqual(TEXT("the type's name is Text"), FString(MantleEntityTypeToString(EMantleEntityType::Text)), FString(TEXT("Text")));
	EMantleEntityType Parsed = EMantleEntityType::StaticMesh;
	TestTrue(TEXT("and it parses back"), MantleParseEntityType(TEXT("Text"), Parsed) && Parsed == EMantleEntityType::Text);

	// Only the text: every other key takes the schema's default.
	const FMantleParseResult Plain = ParseTextEntity(TEXT("\"text\":\"Welcome\""));
	if (!TestTrue(TEXT("a text with only its text parses"), Plain.bSuccess))
	{
		AddError(Plain.ErrorSummary());
		return false;
	}
	const FMantleTextProps* PlainProps = TextOf(Plain);
	if (!TestNotNull(TEXT("a text"), PlainProps))
	{
		return false;
	}
	TestEqual(TEXT("its text"), PlainProps->Text, FString(TEXT("Welcome")));
	TestTrue(TEXT("white"), PlainProps->Color.Equals(FLinearColor::White));
	TestEqual(TEXT("30 cm"), PlainProps->WorldSize, 30.0);
	TestTrue(TEXT("left"), PlainProps->HorizontalAlignment == EMantleTextHorizontalAlignment::Left);
	TestTrue(TEXT("top"), PlainProps->VerticalAlignment == EMantleTextVerticalAlignment::Top);
	TestEqual(TEXT("x scale 1"), PlainProps->XScale, 1.0);
	TestEqual(TEXT("y scale 1"), PlainProps->YScale, 1.0);

	const FMantleParseResult Full = ParseTextEntity(TEXT(
		"\"text\":\"Room 101\\nLab\",\"color\":[1,0.2,0.05,0.5],\"worldSize\":40,\"horizontalAlignment\":\"center\","
		"\"verticalAlignment\":\"baseline\",\"xScale\":1.5,\"yScale\":0.75"));
	if (!TestTrue(TEXT("a text with every key parses"), Full.bSuccess))
	{
		AddError(Full.ErrorSummary());
		return false;
	}
	if (const FMantleTextProps* Props = TextOf(Full))
	{
		TestEqual(TEXT("two lines"), Props->Text, FString(TEXT("Room 101\nLab")));
		TestTrue(TEXT("its linear colour and alpha"), Props->Color.Equals(FLinearColor(1.0f, 0.2f, 0.05f, 0.5f)));
		TestEqual(TEXT("its world size"), Props->WorldSize, 40.0);
		TestTrue(TEXT("centred across"), Props->HorizontalAlignment == EMantleTextHorizontalAlignment::Center);
		TestTrue(TEXT("on the baseline"), Props->VerticalAlignment == EMantleTextVerticalAlignment::Baseline);
		TestEqual(TEXT("x scale"), Props->XScale, 1.5);
		TestEqual(TEXT("y scale"), Props->YScale, 0.75);
	}

	// The text is counted in characters: the cap of a two-byte letter parses, one more does not.
	const FString Longest = FString::ChrN(FMantlePlaceLimits::MaxTextLength, TCHAR(0x00E9));
	TestTrue(TEXT("the longest text parses"), ParseTextEntity(FString::Printf(TEXT("\"text\":\"%s\""), *Longest)).bSuccess);
	const FMantleParseResult TooLong = ParseTextEntity(FString::Printf(TEXT("\"text\":\"%se\""), *Longest));
	TestTrue(TEXT("one character more is refused at the text"), HasErrorAt(TooLong, TEXT("/entities/0/properties/text")));
	// A character outside the BMP is one character, not two.
	FString Trophies;
	for (int32 Count = 0; Count < FMantlePlaceLimits::MaxTextLength / 2; ++Count)
	{
		// U+1F3C6, as its UTF-16 surrogate pair.
		Trophies.AppendChar(static_cast<TCHAR>(0xD83C));
		Trophies.AppendChar(static_cast<TCHAR>(0xDFC6));
	}
	TestTrue(TEXT("surrogate pairs count once"),
		ParseTextEntity(FString::Printf(TEXT("\"text\":\"%s%s\""), *Trophies, *FString::ChrN(FMantlePlaceLimits::MaxTextLength / 2, TEXT('a')))).bSuccess);

	const FMantleParseResult Edges = ParseTextEntity(TEXT(
		"\"text\":\"a\",\"color\":[0,0,0,0],\"worldSize\":1,\"horizontalAlignment\":\"right\",\"verticalAlignment\":\"bottom\","
		"\"xScale\":0.1,\"yScale\":10"));
	TestTrue(TEXT("a text at the edges of every range parses"), Edges.bSuccess);
	TestTrue(TEXT("the largest world size parses"), ParseTextEntity(TEXT("\"text\":\"a\",\"worldSize\":2000")).bSuccess);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWorldTextRefusalTest,
	"Mantle.Runtime.WorldText.Parser.RefusesAtTheKey",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWorldTextRefusalTest::RunTest(const FString& Parameters)
{
	using namespace MantleWorldTextTestPrivate;

	struct FCase
	{
		const TCHAR* Name;
		const TCHAR* Properties;
		const TCHAR* Path;
	};
	const FCase Cases[] = {
		{ TEXT("no text"), TEXT("\"color\":[1,1,1,1]"), TEXT("/entities/0/properties/text") },
		{ TEXT("empty text"), TEXT("\"text\":\"\""), TEXT("/entities/0/properties/text") },
		{ TEXT("a number for the text"), TEXT("\"text\":42"), TEXT("/entities/0/properties/text") },
		{ TEXT("a colour channel past 1"), TEXT("\"text\":\"x\",\"color\":[1.5,1,1,1]"), TEXT("/entities/0/properties/color/0") },
		{ TEXT("a negative alpha"), TEXT("\"text\":\"x\",\"color\":[1,1,1,-0.1]"), TEXT("/entities/0/properties/color/3") },
		{ TEXT("an RGB colour"), TEXT("\"text\":\"x\",\"color\":[1,1,1]"), TEXT("/entities/0/properties/color") },
		{ TEXT("a zero world size"), TEXT("\"text\":\"x\",\"worldSize\":0"), TEXT("/entities/0/properties/worldSize") },
		{ TEXT("a world size past 20 m"), TEXT("\"text\":\"x\",\"worldSize\":2001"), TEXT("/entities/0/properties/worldSize") },
		{ TEXT("a string for the world size"), TEXT("\"text\":\"x\",\"worldSize\":\"40\""), TEXT("/entities/0/properties/worldSize") },
		{ TEXT("an unknown horizontal alignment"), TEXT("\"text\":\"x\",\"horizontalAlignment\":\"middle\""), TEXT("/entities/0/properties/horizontalAlignment") },
		{ TEXT("a horizontal alignment in the wrong case"), TEXT("\"text\":\"x\",\"horizontalAlignment\":\"Center\""), TEXT("/entities/0/properties/horizontalAlignment") },
		{ TEXT("an unknown vertical alignment"), TEXT("\"text\":\"x\",\"verticalAlignment\":\"middle\""), TEXT("/entities/0/properties/verticalAlignment") },
		{ TEXT("a zero x scale"), TEXT("\"text\":\"x\",\"xScale\":0"), TEXT("/entities/0/properties/xScale") },
		{ TEXT("a y scale past 10"), TEXT("\"text\":\"x\",\"yScale\":11"), TEXT("/entities/0/properties/yScale") },
		{ TEXT("a font"), TEXT("\"text\":\"x\",\"font\":\"/Game/Fonts/Fancy\""), TEXT("/entities/0/properties/font") },
	};
	for (const FCase& Case : Cases)
	{
		const FMantleParseResult Result = ParseTextEntity(Case.Properties);
		TestFalse(FString::Printf(TEXT("%s is refused"), Case.Name), Result.bSuccess);
		TestTrue(FString::Printf(TEXT("%s is refused at %s"), Case.Name, Case.Path), HasErrorAt(Result, Case.Path));
	}

	// The place holds at most MaxTextsPerPlace, reported at the type of the one past it.
	FString Entities;
	for (int32 Number = 0; Number <= FMantlePlaceLimits::MaxTextsPerPlace; ++Number)
	{
		Entities += (Number ? TEXT(",") : TEXT("")) + TextEntity(Number, TEXT("\"text\":\"x\""));
	}
	const FMantleParseResult TooMany = ParseEntities(Entities);
	TestTrue(TEXT("one text too many is refused at its type"),
		HasErrorAt(TooMany, FString::Printf(TEXT("/entities/%d/type"), FMantlePlaceLimits::MaxTextsPerPlace)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWorldTextBuilderTest,
	"Mantle.Runtime.WorldText.Builder.AppliesEveryKey",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWorldTextBuilderTest::RunTest(const FString& Parameters)
{
	using namespace MantleWorldTextTestPrivate;
	FTextWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	const FMantleParseResult Parsed = ParseEntities(
		TextEntity(0, TEXT("\"text\":\"Room 101\\nLab\",\"color\":[1,0,0,1],\"worldSize\":40,\"horizontalAlignment\":\"right\","
						   "\"verticalAlignment\":\"baseline\",\"xScale\":2,\"yScale\":0.5")) + TEXT(",")
		+ TextEntity(1, TEXT("\"text\":\"Exit\"")));
	if (!TestTrue(TEXT("the place parses"), Parsed.bSuccess))
	{
		AddError(Parsed.ErrorSummary());
		return false;
	}

	FMantleBuildOptions Options;
	Options.bRuntimeEnvironment = false;
	FMantleBuildResult Built = FMantlePlaceBuilder::Build(Fixture.World, Parsed.Place, Options);
	TestTrue(TEXT("it builds"), Built.bSuccess);
	TestEqual(TEXT("an actor for each text"), Built.Actors.Num(), 2);

	const UTextRenderComponent* DefaultText = GetDefault<UTextRenderComponent>();
	AActor* const* SignActor = Built.Actors.Find(TEXT("ent_text_0"));
	const ATextRenderActor* Sign = SignActor ? Cast<ATextRenderActor>(*SignActor) : nullptr;
	const UTextRenderComponent* Text = Sign ? Sign->GetTextRender() : nullptr;
	if (TestNotNull(TEXT("the sign is a Text Render actor with its component"), Text))
	{
		TestEqual(TEXT("its text"), Text->Text.ToString(), FString(TEXT("Room 101\nLab")));
		TestEqual(TEXT("its colour, as the editor's sRGB"), Text->TextRenderColor, FColor(255, 0, 0, 255));
		TestEqual(TEXT("its world size"), Text->WorldSize, 40.0f);
		TestTrue(TEXT("right"), Text->HorizontalAlignment == EHTA_Right);
		TestTrue(TEXT("baseline is the engine's Quad Top"), Text->VerticalAlignment == EVRTA_QuadTop);
		TestEqual(TEXT("x scale"), Text->XScale, 2.0f);
		TestEqual(TEXT("y scale"), Text->YScale, 0.5f);
		TestTrue(TEXT("the built-in font"), Text->Font == DefaultText->Font);
		TestTrue(TEXT("the built-in text material"), Text->TextMaterial == DefaultText->TextMaterial);
		TestTrue(TEXT("movable, so the transform applies"), Text->GetMobility() == EComponentMobility::Movable);
	}

	AActor* const* ExitActor = Built.Actors.Find(TEXT("ent_text_1"));
	const ATextRenderActor* Exit = ExitActor ? Cast<ATextRenderActor>(*ExitActor) : nullptr;
	if (TestNotNull(TEXT("the exit sign is a Text Render actor"), Exit) && TestNotNull(TEXT("with its component"), Exit->GetTextRender()))
	{
		const UTextRenderComponent* ExitText = Exit->GetTextRender();
		TestEqual(TEXT("white"), ExitText->TextRenderColor, FColor::White);
		TestEqual(TEXT("30 cm"), ExitText->WorldSize, 30.0f);
		TestTrue(TEXT("left"), ExitText->HorizontalAlignment == EHTA_Left);
		TestTrue(TEXT("top"), ExitText->VerticalAlignment == EVRTA_TextTop);
		TestTrue(TEXT("standing at its place"), Exit->GetActorLocation().Equals(FVector(100.0, 0.0, 0.0)));
	}

	FMantlePlaceBuilder::Teardown(Built);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
