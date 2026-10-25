// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceLimits.h"
#include "MantlePlaceParser.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// Seats (GhzGarage/MantleCore#142): what the parser reads and refuses in a place's seats list.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleSeatTestPrivate
{
	/** A place with one Group entity (ent_bench) and Seats (the text between the brackets of seats). */
	FMantleParseResult ParseSeats(const FString& Seats)
	{
		const FString Json = FString::Printf(
			TEXT("{\"schemaVersion\":1,\"placeId\":\"plc_seat0001\",\"revision\":{\"number\":1,\"state\":\"draft\"},"
				 "\"metadata\":{\"name\":\"Seats\",\"authorId\":\"usr_4b1d\"},\"settings\":{},\"assets\":[],"
				 "\"entities\":[{\"id\":\"ent_bench\",\"type\":\"Group\",\"parent\":null,"
				 "\"transform\":{\"location\":[0,0,0],\"rotation\":[0,0,0],\"scale\":[1,1,1]},\"properties\":{}}],"
				 "\"seats\":[%s]}"),
			*Seats);
		return FMantlePlaceParser::ParseFromString(Json);
	}

	FString Seat(const FString& Entity, const FString& Style, const FString& Extra = TEXT(""))
	{
		return FString::Printf(TEXT("{\"entity\":\"%s\",\"position\":[10,-20,45],\"yaw\":90,\"style\":\"%s\"%s}"), *Entity, *Style, *Extra);
	}

	bool HasErrorAt(const FMantleParseResult& Result, const FString& Path)
	{
		return Result.Errors.ContainsByPredicate([&Path](const FMantleParseError& Error) { return Error.Path == Path; });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleSeatParseReadsTest,
	"Mantle.Runtime.Seat.Parser.ReadsASeat",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleSeatParseReadsTest::RunTest(const FString& Parameters)
{
	using namespace MantleSeatTestPrivate;

	const FMantleParseResult Result = ParseSeats(Seat(TEXT("ent_bench"), TEXT("bench")) + TEXT(",") + Seat(TEXT("ent_bench"), TEXT("bench")));
	if (!TestTrue(TEXT("two seats on one entity parse"), Result.bSuccess))
	{
		AddError(Result.ErrorSummary());
		return false;
	}
	if (!TestEqual(TEXT("both seats are kept"), Result.Place.Seats.Num(), 2))
	{
		return false;
	}
	const FMantleSeat& Seat0 = Result.Place.Seats[0];
	TestEqual(TEXT("entity"), Seat0.Entity, FString(TEXT("ent_bench")));
	TestTrue(TEXT("position"), Seat0.Position.Equals(FVector(10, -20, 45)));
	TestEqual(TEXT("yaw"), Seat0.Yaw, 90.0);
	TestTrue(TEXT("style"), Seat0.Style == EMantleSeatStyle::Bench);

	TestTrue(TEXT("a place with no seats list parses with none"), FMantlePlaceParser::ParseFromString(
		TEXT("{\"schemaVersion\":1,\"placeId\":\"plc_seat0002\",\"revision\":{\"number\":1,\"state\":\"draft\"},"
			 "\"metadata\":{\"name\":\"None\",\"authorId\":\"usr_4b1d\"},\"settings\":{},\"assets\":[],\"entities\":[]}")).Place.Seats.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleSeatParseRefusesTest,
	"Mantle.Runtime.Seat.Parser.RefusesBadSeats",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleSeatParseRefusesTest::RunTest(const FString& Parameters)
{
	using namespace MantleSeatTestPrivate;

	const FMantleParseResult Unknown = ParseSeats(Seat(TEXT("ent_bench"), TEXT("sofa")));
	TestFalse(TEXT("an unknown style is a parse error"), Unknown.bSuccess);
	TestTrue(TEXT("the error names the style"), HasErrorAt(Unknown, TEXT("/seats/0/style")));
	TestFalse(TEXT("the style is case-exact"), ParseSeats(Seat(TEXT("ent_bench"), TEXT("Bench"))).bSuccess);

	const FMantleParseResult Orphan = ParseSeats(Seat(TEXT("ent_missing"), TEXT("bench")));
	TestFalse(TEXT("a seat on an entity the place lacks is refused"), Orphan.bSuccess);
	TestTrue(TEXT("the error names the entity"), HasErrorAt(Orphan, TEXT("/seats/0/entity")));

	TestFalse(TEXT("an unknown key is refused"), ParseSeats(Seat(TEXT("ent_bench"), TEXT("bench"), TEXT(",\"pitch\":5"))).bSuccess);
	TestFalse(TEXT("a missing yaw is refused"), ParseSeats(TEXT("{\"entity\":\"ent_bench\",\"position\":[0,0,0],\"style\":\"bench\"}")).bSuccess);
	TestFalse(TEXT("a two-number position is refused"),
		ParseSeats(TEXT("{\"entity\":\"ent_bench\",\"position\":[0,0],\"yaw\":0,\"style\":\"bench\"}")).bSuccess);
	TestFalse(TEXT("a seat that is not an object is refused"), ParseSeats(TEXT("7")).bSuccess);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleSeatParseLimitTest,
	"Mantle.Runtime.Seat.Parser.CapsSeatsPerPlace",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleSeatParseLimitTest::RunTest(const FString& Parameters)
{
	using namespace MantleSeatTestPrivate;

	auto Many = [](int32 Count)
	{
		TArray<FString> Seats;
		for (int32 i = 0; i < Count; ++i)
		{
			Seats.Add(Seat(TEXT("ent_bench"), TEXT("bench")));
		}
		return FString::Join(Seats, TEXT(","));
	};
	TestTrue(TEXT("the most seats parse"), ParseSeats(Many(FMantlePlaceLimits::MaxSeatsPerPlace)).bSuccess);
	const FMantleParseResult Over = ParseSeats(Many(FMantlePlaceLimits::MaxSeatsPerPlace + 1));
	TestFalse(TEXT("one more is refused"), Over.bSuccess);
	TestTrue(TEXT("the error names the list"), HasErrorAt(Over, TEXT("/seats")));
	return true;
}

#endif
