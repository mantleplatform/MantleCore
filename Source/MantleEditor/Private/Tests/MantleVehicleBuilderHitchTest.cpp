// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleAsset.h"
#include "MantleVehicleBuilder.h"
#include "MantleVehicleChassisDocument.h"
#include "MantleVehicleConverter.h"
#include "MantleVehicleSource.h"
#include "MantlePlaceValidator.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

// A creator's hitch (#446): the vehicle builder's Towing choice goes into the vehicle's settings (FMantleVehicleBuilder::ApplyHitch), at the place the
// creator gave or the builder's default, and from there into the document the vehicle publishes (FMantleVehicleConverter::MakeDocument). A vehicle that
// is not wheeled or tracked gets none, with a note, and a document is refused one.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVehicleBuilderHitchTestPrivate
{
	/** A built result holding a transient source of Movement, as a build leaves it (not saved: no package to save). */
	FMantleVehicleBuildResult MakeBuilt(const EMantleVehicleMovementKind Movement)
	{
		FMantleVehicleBuildResult Built;
		Built.Source = NewObject<UMantleVehicleSource>(GetTransientPackage());
		Built.Source->MovementType = Movement;
		Built.bOk = true;
		Built.bSaved = false;
		return Built;
	}

	FMantleVehicleModelOptions WithHitch(const EMantleVehicleSourceHitch Hitch, const TOptional<FVector>& Location = TOptional<FVector>())
	{
		FMantleVehicleModelOptions Options;
		Options.Hitch = Hitch;
		Options.HitchLocation = Location;
		return Options;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleBuilderHitchTest,
	"Mantle.Vehicle.Builder.Hitch",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleBuilderHitchTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleBuilderHitchTestPrivate;

	// Unset: the type's own stays.
	FMantleVehicleBuildResult Own = MakeBuilt(EMantleVehicleMovementKind::Wheeled);
	FMantleVehicleBuilder::ApplyHitch(Own, FMantleVehicleModelOptions());
	TestTrue(TEXT("no choice keeps the type's own"), Own.Source->Tuning.Hitch == EMantleVehicleSourceHitch::Preset);

	// A tow ball with no place: the builder's default (a body it can't read gets a car's, behind a 5 m car at a utility trailer's coupler height).
	FMantleVehicleBuildResult Ball = MakeBuilt(EMantleVehicleMovementKind::Wheeled);
	FMantleVehicleBuilder::ApplyHitch(Ball, WithHitch(EMantleVehicleSourceHitch::Ball));
	TestTrue(TEXT("a tow ball is set"), Ball.Source->Tuning.Hitch == EMantleVehicleSourceHitch::Ball);
	TestTrue(*FString::Printf(TEXT("at the default place (%s)"), *Ball.Source->Tuning.HitchLocation.ToString()),
		Ball.Source->Tuning.HitchLocation.Equals(FMantleVehicleChassisDocument::DefaultHitchLocation(EMantleVehicleHitchKind::Ball, -250.0, -150.0, 0.0)));
	TestTrue(TEXT("the note says where"), Ball.Notes.ContainsByPredicate([](const FString& Note) { return Note.StartsWith(TEXT("Hitch: a tow ball at")); }));

	// A fifth wheel on a tracked vehicle, where the creator put it.
	FMantleVehicleBuildResult Tank = MakeBuilt(EMantleVehicleMovementKind::Tracked);
	FMantleVehicleBuilder::ApplyHitch(Tank, WithHitch(EMantleVehicleSourceHitch::FifthWheel, FVector(-120.0, 0.0, 125.0)));
	TestTrue(TEXT("a tracked vehicle takes a fifth wheel"), Tank.Source->Tuning.Hitch == EMantleVehicleSourceHitch::FifthWheel);
	TestTrue(TEXT("where the creator put it"), Tank.Source->Tuning.HitchLocation.Equals(FVector(-120.0, 0.0, 125.0)));

	// None.
	FMantleVehicleBuildResult None = MakeBuilt(EMantleVehicleMovementKind::Wheeled);
	FMantleVehicleBuilder::ApplyHitch(None, WithHitch(EMantleVehicleSourceHitch::None));
	TestTrue(TEXT("none is set"), None.Source->Tuning.Hitch == EMantleVehicleSourceHitch::None);

	// Not wheeled or tracked: nothing set, and a note says why.
	for (const EMantleVehicleMovementKind Movement : { EMantleVehicleMovementKind::Plane, EMantleVehicleMovementKind::Helicopter, EMantleVehicleMovementKind::Hover,
			 EMantleVehicleMovementKind::Boat, EMantleVehicleMovementKind::Trailer })
	{
		FMantleVehicleBuildResult Refused = MakeBuilt(Movement);
		FMantleVehicleBuilder::ApplyHitch(Refused, WithHitch(EMantleVehicleSourceHitch::Ball));
		const FString Name = FMantleVehicleChassisDocument::MovementName(Movement);
		TestTrue(*FString::Printf(TEXT("a %s gets no hitch"), *Name), Refused.Source->Tuning.Hitch == EMantleVehicleSourceHitch::Preset);
		TestTrue(*FString::Printf(TEXT("a %s's note says it tows nothing"), *Name),
			Refused.Notes.ContainsByPredicate([](const FString& Note) { return Note.Contains(TEXT("tows nothing")); }));
	}

	// Into the document: the creator's tow ball is the document's hitch, as set.
	{
		FString Error;
		const TSharedRef<FJsonObject> Document = FMantleVehicleConverter::MakeDocument(*Ball.Source, TMap<FString, FString>(), &Error);
		TestTrue(*FString::Printf(TEXT("the document is made (%s)"), *Error), Error.IsEmpty());
		const TSharedPtr<FJsonObject>* Hitch = nullptr;
		if (TestTrue(TEXT("the document has a hitch"), Document->TryGetObjectField(TEXT("hitch"), Hitch) && Hitch && Hitch->IsValid()))
		{
			TestEqual(TEXT("its type"), (*Hitch)->GetStringField(TEXT("type")), FString(TEXT("ball")));
			const TArray<TSharedPtr<FJsonValue>>* Location = nullptr;
			if (TestTrue(TEXT("its location"), (*Hitch)->TryGetArrayField(TEXT("location"), Location) && Location && Location->Num() == 3))
			{
				const FVector Read((*Location)[0]->AsNumber(), (*Location)[1]->AsNumber(), (*Location)[2]->AsNumber());
				TestTrue(*FString::Printf(TEXT("as set (%s)"), *Read.ToString()), Read.Equals(Ball.Source->Tuning.HitchLocation, 0.01));
			}
		}
	}
	// None writes no hitch; a plane source given a tow ball by hand is refused, in words.
	{
		FString Error;
		const TSharedRef<FJsonObject> Document = FMantleVehicleConverter::MakeDocument(*None.Source, TMap<FString, FString>(), &Error);
		TestFalse(TEXT("none writes no hitch"), Document->HasField(TEXT("hitch")));
		UMantleVehicleSource* Plane = NewObject<UMantleVehicleSource>(GetTransientPackage());
		Plane->MovementType = EMantleVehicleMovementKind::Plane;
		Plane->Tuning.Hitch = EMantleVehicleSourceHitch::Ball;
		FMantleVehicleConverter::MakeDocument(*Plane, TMap<FString, FString>(), &Error);
		TestTrue(*FString::Printf(TEXT("a plane's hitch is refused (%s)"), *Error), Error.Contains(TEXT("only for a wheeled or tracked vehicle")));
	}
	// A boat given a tow ball in its settings: the creator sees why, in the vehicle's own issue (not "movement settings that can't be read").
	{
		UMantleVehicleSource* Boat = NewObject<UMantleVehicleSource>(GetTransientPackage());
		Boat->MovementType = EMantleVehicleMovementKind::Boat;
		Boat->Tuning.Hitch = EMantleVehicleSourceHitch::Ball;
		TArray<FMantleValidationIssue> Issues;
		FMantleVehicleConverter::CheckSource(Boat, FString(), Issues);
		const bool bSaid = Issues.ContainsByPredicate([](const FMantleValidationIssue& Issue)
		{
			return Issue.Severity == EMantleIssueSeverity::Error && Issue.Problem.Contains(TEXT("has a hitch, but a boat tows nothing"));
		});
		TestTrue(TEXT("a boat's hitch is an error the creator sees"), bSaid);
		TestFalse(TEXT("and not also as unreadable movement settings"), Issues.ContainsByPredicate([](const FMantleValidationIssue& Issue)
		{
			return Issue.Problem.Contains(TEXT("movement settings that can't be read")) && Issue.Problem.Contains(TEXT("hitch"));
		}));
	}
	// The rear-centre default, in the body's space: a tow ball 10 cm behind the rear end on the centre line at a utility trailer's coupler height above
	// the ground, a fifth wheel just ahead of the rear axles at a semi trailer's.
	TestTrue(TEXT("a default tow ball is behind the rear end, centred, 50 cm up"),
		FMantleVehicleChassisDocument::DefaultHitchLocation(EMantleVehicleHitchKind::Ball, -240.0, -140.0, -30.0).Equals(FVector(-250.0, 0.0, 20.0)));
	TestTrue(TEXT("a default fifth wheel is 30 cm ahead of the rear axles, centred, 119 cm up"),
		FMantleVehicleChassisDocument::DefaultHitchLocation(EMantleVehicleHitchKind::FifthWheel, -300.0, -190.0, 0.0).Equals(FVector(-160.0, 0.0, 119.0)));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
