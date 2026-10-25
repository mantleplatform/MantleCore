// Copyright Joshua Eger. All Rights Reserved.
#include "MantleEnvironment.h"
#include "MantlePlaceExporter.h"
#include "MantlePlaceLimits.h"
#include "MantlePlaceParser.h"
#include "MantlePlaceValidator.h"
#include "MantleSeatExport.h"
#include "MantleSeatSpot.h"

#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/TextRenderActor.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

// Mantle Seats (GhzGarage/MantleCore#142): how a seat component is written, what "Check this place" says about it, and
// that the exported place reads back through the parser.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleSeatExportTestPrivate
{
	/** An empty editor world for one test, destroyed with it. */
	struct FSeatWorld
	{
		UWorld* World = nullptr;

		FSeatWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld*/ false, TEXT("MantleSeatExportTestWorld"));
		}

		~FSeatWorld()
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

	/** A static, published bench-like prop at Location, turned Yaw degrees. */
	AStaticMeshActor* SpawnProp(UWorld* World, const TCHAR* Label, const FVector& Location, double Yaw = 0.0)
	{
		AStaticMeshActor* Prop = World->SpawnActor<AStaticMeshActor>(Location, FRotator(0.0, Yaw, 0.0));
		if (Prop)
		{
			Prop->SetActorLabel(Label);
			Prop->GetStaticMeshComponent()->SetMobility(EComponentMobility::Static);
			Prop->GetStaticMeshComponent()->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
		}
		return Prop;
	}

	/** A seat on Actor, at Offset from the actor's origin. */
	UMantleSeatSpotComponent* AddSeat(AActor* Actor, const FVector& Offset, const TCHAR* Name = TEXT("Seat"))
	{
		UMantleSeatSpotComponent* Seat = NewObject<UMantleSeatSpotComponent>(Actor, Name);
		Seat->SetupAttachment(Actor->GetRootComponent());
		Seat->SetRelativeLocation(Offset);
		Seat->RegisterComponent();
		return Seat;
	}

	bool HasIssue(const FMantleValidationResult& Result, const FString& Contains)
	{
		return Result.Issues.ContainsByPredicate([&Contains](const FMantleValidationIssue& Issue) { return Issue.ToString().Contains(Contains); });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleSeatPlaceSeatTest,
	"Mantle.Editor.Seat.Export.WritesTheHipInTheOwnersSpace",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleSeatPlaceSeatTest::RunTest(const FString& Parameters)
{
	// The owner stands at (1000, 0, 0) turned 90 degrees; the seat is 45 cm up and 100 cm along the world's Y, turned 120.
	const FTransform Owner(FRotator(0.0, 90.0, 0.0), FVector(1000.0, 0.0, 0.0));
	const FTransform SeatWorld(FRotator(0.0, 120.0, 0.0), FVector(1000.0, 100.0, 45.0));
	const FMantleSeat Seat = FMantleSeatExport::ToPlaceSeat(TEXT("ent_bench"), Owner, SeatWorld, EMantleSeatStyle::Bench);
	TestEqual(TEXT("entity"), Seat.Entity, FString(TEXT("ent_bench")));
	// Along the owner's forward (its +X, world +Y): local X 100, Y 0, Z 45.
	TestTrue(TEXT("position is local to the owner"), Seat.Position.Equals(FVector(100.0, 0.0, 45.0), 0.11));
	TestEqual(TEXT("yaw is against the owner's"), Seat.Yaw, 30.0);
	TestTrue(TEXT("style"), Seat.Style == EMantleSeatStyle::Bench);

	// The yaw wraps the short way round.
	const FMantleSeat Wrapped = FMantleSeatExport::ToPlaceSeat(TEXT("e"), FTransform(FRotator(0.0, 170.0, 0.0)), FTransform(FRotator(0.0, -170.0, 0.0)), EMantleSeatStyle::Bench);
	TestEqual(TEXT("yaw wraps into -180 to 180"), Wrapped.Yaw, 20.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleSeatHeightTest,
	"Mantle.Editor.Seat.Check.HipHeightRange",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleSeatHeightTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("a bench's hip is fine"), FMantleSeatExport::HeightProblem(true, 45.0).IsEmpty());
	TestTrue(TEXT("the lowest allowed is fine"), FMantleSeatExport::HeightProblem(true, FMantleSeatExport::MinHipHeight).IsEmpty());
	TestTrue(TEXT("the highest allowed is fine"), FMantleSeatExport::HeightProblem(true, FMantleSeatExport::MaxHipHeight).IsEmpty());
	TestTrue(TEXT("no floor says there is nothing to sit on"), FMantleSeatExport::HeightProblem(false, 0.0).Contains(TEXT("nothing to sit on")));
	TestFalse(TEXT("a hip in the floor is refused"), FMantleSeatExport::HeightProblem(true, 2.0).IsEmpty());
	TestTrue(TEXT("a hip far above the floor says there is nothing to sit on"),
		FMantleSeatExport::HeightProblem(true, 300.0).Contains(TEXT("nothing to sit on")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleSeatCheckTest,
	"Mantle.Editor.Seat.Check.OwnerAndFloorRules",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleSeatCheckTest::RunTest(const FString& Parameters)
{
	using namespace MantleSeatExportTestPrivate;
	FSeatWorld Fixture;
	if (!TestNotNull(TEXT("world"), Fixture.World))
	{
		return false;
	}

	// An empty world has no floor under a seat.
	AStaticMeshActor* Bench = SpawnProp(Fixture.World, TEXT("Bench"), FVector::ZeroVector);
	if (!TestNotNull(TEXT("bench"), Bench))
	{
		return false;
	}
	AddSeat(Bench, FVector(0.0, 0.0, 45.0));
	FMantleValidationResult Result = FMantlePlaceValidator::Validate(Fixture.World);
	TestTrue(TEXT("a seat with no floor says nothing to sit on"), HasIssue(Result, TEXT("nothing to sit on")));
	TestFalse(TEXT("the prop publishes, so the owner is not the complaint"), HasIssue(Result, TEXT("not published")));

	// A prop that moves cannot carry a seat.
	Bench->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
	Result = FMantlePlaceValidator::Validate(Fixture.World);
	TestTrue(TEXT("a Movable prop is refused"), HasIssue(Result, TEXT("Movable")));
	Bench->GetStaticMeshComponent()->SetMobility(EComponentMobility::Static);

	// A prop that simulates physics cannot either.
	Bench->GetStaticMeshComponent()->BodyInstance.bSimulatePhysics = true;
	TestTrue(TEXT("a physics prop is refused"), FMantleSeatExport::OwnerProblem(Bench).Contains(TEXT("physics")));
	Bench->GetStaticMeshComponent()->BodyInstance.bSimulatePhysics = false;

	// A seat on an actor that does not publish has nothing to belong to.
	AActor* Plain = Fixture.World->SpawnActor<AActor>();
	if (TestNotNull(TEXT("plain actor"), Plain))
	{
		USceneComponent* Root = NewObject<USceneComponent>(Plain, TEXT("Root"));
		Plain->SetRootComponent(Root);
		Root->RegisterComponent();
		// What Mantle's environment spawns into a built place is never content, so it does not publish.
		Plain->Tags.Add(FName(FMantleEnvironment::EnvironmentActorTag));
		Plain->SetActorLabel(TEXT("Not published"));
		AddSeat(Plain, FVector(0.0, 0.0, 45.0));
		TestFalse(TEXT("an actor that does not publish is not an owner"), FMantleSeatExport::OwnerPublishes(Plain));
		TestTrue(TEXT("Check says the seat has nothing to belong to"),
			HasIssue(FMantlePlaceValidator::Validate(Fixture.World), TEXT("nothing to belong to")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleSeatCapTest,
	"Mantle.Editor.Seat.Check.AtMost256PerPlace",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleSeatCapTest::RunTest(const FString& Parameters)
{
	using namespace MantleSeatExportTestPrivate;
	FSeatWorld Fixture;
	if (!TestNotNull(TEXT("world"), Fixture.World))
	{
		return false;
	}
	AStaticMeshActor* Row = SpawnProp(Fixture.World, TEXT("Row"), FVector::ZeroVector);
	if (!TestNotNull(TEXT("row"), Row))
	{
		return false;
	}
	for (int32 i = 0; i < FMantlePlaceLimits::MaxSeatsPerPlace; ++i)
	{
		AddSeat(Row, FVector(i * 50.0, 0.0, 45.0), *FString::Printf(TEXT("Seat%d"), i));
	}
	TestFalse(TEXT("256 seats are within the cap"),
		HasIssue(FMantlePlaceValidator::Validate(Fixture.World), TEXT("at most 256")));

	AddSeat(Row, FVector(0.0, 100.0, 45.0), TEXT("OneTooMany"));
	TestTrue(TEXT("257 seats are over the cap"),
		HasIssue(FMantlePlaceValidator::Validate(Fixture.World), TEXT("at most 256")));

	const FMantleExportResult Exported = FMantlePlaceExporter::Export(Fixture.World, FMantleExportOptions());
	TestEqual(TEXT("the export keeps only the cap"), Exported.Place.Seats.Num(), FMantlePlaceLimits::MaxSeatsPerPlace);
	TestTrue(TEXT("the export says the cap was passed"),
		Exported.Errors.ContainsByPredicate([](const FMantleParseError& Error) { return Error.Path == TEXT("/seats"); }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleSeatRoundTripTest,
	"Mantle.Editor.Seat.Export.PlaceReadsBack",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleSeatRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace MantleSeatExportTestPrivate;
	FSeatWorld Fixture;
	if (!TestNotNull(TEXT("world"), Fixture.World))
	{
		return false;
	}
	AStaticMeshActor* Bench = SpawnProp(Fixture.World, TEXT("Bench"), FVector(500.0, 0.0, 0.0), 90.0);
	if (!TestNotNull(TEXT("bench"), Bench))
	{
		return false;
	}
	UMantleSeatSpotComponent* Left = AddSeat(Bench, FVector(0.0, -60.0, 45.0), TEXT("SeatA"));
	AddSeat(Bench, FVector(0.0, 60.0, 45.0), TEXT("SeatB"));
	Left->SetRelativeRotation(FRotator(0.0, 180.0, 0.0));

	const FMantleExportResult Exported = FMantlePlaceExporter::Export(Fixture.World, FMantleExportOptions());
	if (!TestEqual(TEXT("two seats are exported"), Exported.Place.Seats.Num(), 2))
	{
		return false;
	}
	TestTrue(TEXT("the first seat is in the bench's space"), Exported.Place.Seats[0].Position.Equals(FVector(0.0, -60.0, 45.0), 0.11));
	TestEqual(TEXT("the first seat's heading"), FMath::Abs(Exported.Place.Seats[0].Yaw), 180.0);
	TestTrue(TEXT("both ride on the bench's entity"),
		Exported.Place.Entities.ContainsByPredicate([&Exported](const FMantleEntity& Entity) { return Entity.Id == Exported.Place.Seats[0].Entity; })
		&& Exported.Place.Seats[1].Entity == Exported.Place.Seats[0].Entity);

	// A level with no seats writes no seats key.
	FMantlePlace NoSeats = Exported.Place;
	NoSeats.Seats.Reset();
	TestFalse(TEXT("no seats, no key"), FMantlePlaceExporter::ToJson(NoSeats).Contains(TEXT("\"seats\"")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleSeatAttachedTest,
	"Mantle.Editor.Seat.Export.AttachedPropKeepsItsOwnSpace",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleSeatAttachedTest::RunTest(const FString& Parameters)
{
	using namespace MantleSeatExportTestPrivate;
	FSeatWorld Fixture;
	if (!TestNotNull(TEXT("world"), Fixture.World))
	{
		return false;
	}
	// A bench attached to a platform: Export writes the bench relative to the platform, but its seat is still in the
	// bench's own space.
	AStaticMeshActor* Platform = SpawnProp(Fixture.World, TEXT("Platform"), FVector(1000.0, 0.0, 200.0), 30.0);
	AStaticMeshActor* Bench = SpawnProp(Fixture.World, TEXT("Bench"), FVector(1200.0, 0.0, 250.0), 75.0);
	if (!TestNotNull(TEXT("platform"), Platform) || !TestNotNull(TEXT("bench"), Bench))
	{
		return false;
	}
	Bench->AttachToActor(Platform, FAttachmentTransformRules::KeepWorldTransform);
	AddSeat(Bench, FVector(10.0, 20.0, 45.0))->SetRelativeRotation(FRotator(0.0, 15.0, 0.0));

	const FMantleExportResult Exported = FMantlePlaceExporter::Export(Fixture.World, FMantleExportOptions());
	if (!TestEqual(TEXT("one seat"), Exported.Place.Seats.Num(), 1))
	{
		return false;
	}
	const FMantleEntity* BenchEntity = Exported.Place.Entities.FindByPredicate([](const FMantleEntity& Entity) { return Entity.Name == TEXT("Bench"); });
	if (TestNotNull(TEXT("the bench is an entity"), BenchEntity))
	{
		TestFalse(TEXT("the bench has the platform as its parent"), BenchEntity->Parent.IsEmpty());
		TestEqual(TEXT("the seat rides on the bench"), Exported.Place.Seats[0].Entity, BenchEntity->Id);
	}
	TestTrue(TEXT("the hip is in the bench's own space"), Exported.Place.Seats[0].Position.Equals(FVector(10.0, 20.0, 45.0), 0.11));
	TestEqual(TEXT("the heading is against the bench's"), Exported.Place.Seats[0].Yaw, 15.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleSeatParsesBackTest,
	"Mantle.Editor.Seat.Export.FileParsesBack",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleSeatParsesBackTest::RunTest(const FString& Parameters)
{
	using namespace MantleSeatExportTestPrivate;
	FSeatWorld Fixture;
	if (!TestNotNull(TEXT("world"), Fixture.World))
	{
		return false;
	}
	// A text sign stands in for the prop: it exports with nothing to upload, so the file is complete without a publish.
	ATextRenderActor* Sign = Fixture.World->SpawnActor<ATextRenderActor>(FVector(0.0, 300.0, 0.0), FRotator(0.0, 45.0, 0.0));
	if (!TestNotNull(TEXT("sign"), Sign))
	{
		return false;
	}
	Sign->SetActorLabel(TEXT("Sign"));
	Sign->GetTextRender()->SetText(FText::FromString(TEXT("Rest here")));
	AddSeat(Sign, FVector(30.0, 0.0, 45.0));

	const FMantleExportResult Exported = FMantlePlaceExporter::Export(Fixture.World, FMantleExportOptions());
	const FString Json = FMantlePlaceExporter::ToJson(Exported.Place);
	TestTrue(TEXT("the file has a seats key"), Json.Contains(TEXT("\"seats\"")));
	const FMantleParseResult Parsed = FMantlePlaceParser::ParseFromString(Json);
	if (!TestTrue(TEXT("the file parses as a place"), Parsed.bSuccess))
	{
		AddError(Parsed.ErrorSummary());
		return false;
	}
	if (TestEqual(TEXT("one seat reads back"), Parsed.Place.Seats.Num(), 1))
	{
		const FMantleSeat& Seat = Parsed.Place.Seats[0];
		TestEqual(TEXT("on the sign's entity"), Seat.Entity, Exported.Place.Seats[0].Entity);
		TestTrue(TEXT("at the hip it was given"), Seat.Position.Equals(FVector(30.0, 0.0, 45.0), 0.11));
		TestEqual(TEXT("facing as the sign does"), Seat.Yaw, 0.0);
		TestTrue(TEXT("as a bench"), Seat.Style == EMantleSeatStyle::Bench);
	}
	return true;
}

#endif
