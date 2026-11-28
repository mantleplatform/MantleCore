// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleAsset.h"
#include "MantleVehicleBuilder.h"
#include "MantleVehicleChassisBuilder.h"
#include "MantleVehicleChassisDocument.h"
#include "MantleVehicleChassisMesh.h"
#include "MantleVehicleScratchBody.h"
#include "MantleVehicleConverter.h"
#include "MantleVehicleSource.h"
#include "MantlePlaceValidator.h"
#include "SMantleVehicleBuilder.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/SkeletalMesh.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ScopeExit.h"
#include "UObject/UObjectIterator.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

/** The window's Towing state, set the way its picker and checkbox set it, and what Build would send. */
struct FMantleVehicleBuilderHitchAccess
{
	static void PickType(SMantleVehicleBuilder& Panel, const TCHAR* Id, const TCHAR* Name) { Panel.PickPreset(Id, FText::FromString(Name)); }
	static void Pick(SMantleVehicleBuilder& Panel, const EMantleVehicleSourceHitch Choice, const bool bPlaced = false, const FVector& At = FVector::ZeroVector)
	{
		Panel.HitchChoice = Choice;
		Panel.bHitchPlaced = bPlaced;
		Panel.HitchAt = At;
	}
	static EMantleVehicleSourceHitch Choice(const SMantleVehicleBuilder& Panel) { return Panel.HitchChoice; }
	static bool Placed(const SMantleVehicleBuilder& Panel) { return Panel.bHitchPlaced; }
	static FMantleVehicleModelOptions Options(const SMantleVehicleBuilder& Panel) { return Panel.MakeBuildOptions(); }
};

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleBuilderHitchUndoTest,
	"Mantle.Vehicle.Builder.HitchUndoAndTypeChange",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleBuilderHitchUndoTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleBuilderHitchTestPrivate;
	using Access = FMantleVehicleBuilderHitchAccess;

	// The type's own undoes an earlier pick (Plugin Manager's review of Core #292): a pickup built with a fifth wheel, then built again on the type's
	// own, has the type's own again, not the fifth wheel.
	FMantleVehicleBuildResult Pickup = MakeBuilt(EMantleVehicleMovementKind::Wheeled);
	FMantleVehicleBuilder::ApplyHitch(Pickup, WithHitch(EMantleVehicleSourceHitch::FifthWheel));
	TestTrue(TEXT("built with a fifth wheel"), Pickup.Source->Tuning.Hitch == EMantleVehicleSourceHitch::FifthWheel);
	FMantleVehicleModelOptions Own;
	FMantleVehicleBuilder::HitchOptions(Own, true, EMantleVehicleSourceHitch::Preset, false, FVector::ZeroVector);
	TestTrue(TEXT("the type's own is sent, not left out"), Own.Hitch.IsSet() && Own.Hitch.GetValue() == EMantleVehicleSourceHitch::Preset && !Own.HitchLocation.IsSet());
	FMantleVehicleBuilder::ApplyHitch(Pickup, Own);
	TestTrue(TEXT("built again on the type's own, it has the type's own"), Pickup.Source->Tuning.Hitch == EMantleVehicleSourceHitch::Preset);
	TestTrue(TEXT("and the note says so"), Pickup.Notes.Contains(TEXT("Hitch: back to the type's own.")));
	// Its document then carries the type's hitch from the movement blocks, not the fifth wheel: with no blocks here, no hitch at all.
	{
		FString Error;
		const TSharedRef<FJsonObject> Document = FMantleVehicleConverter::MakeDocument(*Pickup.Source, TMap<FString, FString>(), &Error);
		TestFalse(TEXT("the fifth wheel is gone from the document"), Document->HasField(TEXT("hitch")));
	}

	// A type that tows nothing is built with its own, whatever was picked for another type: no "tows nothing" it can't fix from the window.
	FMantleVehicleModelOptions Stale;
	FMantleVehicleBuilder::HitchOptions(Stale, false, EMantleVehicleSourceHitch::Ball, true, FVector(-200.0, 0.0, 50.0));
	TestTrue(TEXT("a plane is sent the type's own, with no place"), Stale.Hitch.IsSet() && Stale.Hitch.GetValue() == EMantleVehicleSourceHitch::Preset && !Stale.HitchLocation.IsSet());
	FMantleVehicleBuildResult Plane = MakeBuilt(EMantleVehicleMovementKind::Plane);
	FMantleVehicleBuilder::ApplyHitch(Plane, Stale);
	TestTrue(TEXT("the plane has the type's own"), Plane.Source->Tuning.Hitch == EMantleVehicleSourceHitch::Preset);
	TestFalse(TEXT("and no tows-nothing note"), Plane.Notes.ContainsByPredicate([](const FString& Note) { return Note.Contains(TEXT("tows nothing")); }));

	// A placed pick sends its place; an unplaced one doesn't.
	FMantleVehicleModelOptions Placed;
	FMantleVehicleBuilder::HitchOptions(Placed, true, EMantleVehicleSourceHitch::Ball, true, FVector(-260.0, 0.0, 48.0));
	TestTrue(TEXT("a placed tow ball sends its place"), Placed.HitchLocation.IsSet() && Placed.HitchLocation.GetValue().Equals(FVector(-260.0, 0.0, 48.0)));

	// The window: a new type starts on its own hitch, and Build sends what the window shows.
	if (!FSlateApplication::IsInitialized())
	{
		AddWarning(TEXT("SKIPPED, not passed: there is no Slate application in this runner, so the window's type change is not checked."));
		return true;
	}
	TSharedRef<SMantleVehicleBuilder> Panel = SNew(SMantleVehicleBuilder);
	Access::PickType(*Panel, TEXT("pickup"), TEXT("Pickup / SUV"));
	Access::Pick(*Panel, EMantleVehicleSourceHitch::FifthWheel, true, FVector(-150.0, 0.0, 119.0));
	FMantleVehicleModelOptions Picked = Access::Options(*Panel);
	TestTrue(TEXT("the pickup's fifth wheel is sent"), Picked.Hitch.IsSet() && Picked.Hitch.GetValue() == EMantleVehicleSourceHitch::FifthWheel && Picked.HitchLocation.IsSet());
	Access::Pick(*Panel, EMantleVehicleSourceHitch::Preset);
	FMantleVehicleModelOptions Back = Access::Options(*Panel);
	TestTrue(TEXT("back to the type's own, the type's own is sent"), Back.Hitch.IsSet() && Back.Hitch.GetValue() == EMantleVehicleSourceHitch::Preset);
	Access::Pick(*Panel, EMantleVehicleSourceHitch::Ball, true, FVector(-280.0, 0.0, 50.0));
	Access::PickType(*Panel, TEXT("plane"), TEXT("Plane"));
	TestTrue(TEXT("a new type starts on its own hitch"), Access::Choice(*Panel) == EMantleVehicleSourceHitch::Preset && !Access::Placed(*Panel));
	FMantleVehicleModelOptions PlaneOptions = Access::Options(*Panel);
	TestTrue(TEXT("and the plane is built with its own"), PlaneOptions.Hitch.IsSet() && PlaneOptions.Hitch.GetValue() == EMantleVehicleSourceHitch::Preset
		&& !PlaneOptions.HitchLocation.IsSet());
	Access::PickType(*Panel, TEXT("sedan"), TEXT("Sedan"));
	TestTrue(TEXT("a wheeled type after it starts on its own too"), Access::Choice(*Panel) == EMantleVehicleSourceHitch::Preset);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleBuilderHitchConsoleTest,
	"Mantle.Vehicle.BuildModel.Hitch",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleBuilderHitchConsoleTest::RunTest(const FString& Parameters)
{
	// The console's Mantle.Vehicle.BuildModel (the route the Car Uploader builds every FiveM car through, Plugin Manager's review): hitch=ball puts a
	// tow ball at the default place on the built vehicle, and a build again with hitch=own puts the type's own back.
	IConsoleCommand* Command = IConsoleManager::Get().FindConsoleObject(TEXT("Mantle.Vehicle.BuildModel")) ? IConsoleManager::Get().FindConsoleObject(TEXT("Mantle.Vehicle.BuildModel"))->AsCommand() : nullptr;
	if (!TestNotNull(TEXT("the console command is there"), Command))
	{
		return false;
	}
	// A model: the sedan's own gray body as a skeletal mesh, worn on the sedan's frame.
	FString Error;
	FMantleChassisSpec Spec;
	FMantleVehicleAsset Asset;
	USkeletalMesh* Model = FMantleVehicleChassisBuilder::LoadSpec(TEXT("sedan"), Spec, Error) && FMantleVehicleChassisBuilder::MakeAsset(Spec, Asset, Error)
		? FMantleVehicleChassisMesh::Create(Asset.Body, TEXT("/Temp/MantleHitchConsoleTest"), TEXT("HitchConsoleCar"), Error) : nullptr;
	if (!TestNotNull(*FString::Printf(TEXT("the sample car is made (%s)"), *Error), Model))
	{
		return false;
	}
	ON_SCOPE_EXIT { MantleVehicleScratchBody::Discard(Model); };

	const FString Folder = TEXT("/Temp/MantleHitchConsoleOut");
	auto BuildWith = [&](const TCHAR* Hitch) -> UMantleVehicleSource*
	{
		const TArray<FString> Args = { Model->GetPathName(), TEXT("sedan"), FString(TEXT("folder=")) + Folder, TEXT("name=HitchConsole"), TEXT("overwrite=1"),
			FString(TEXT("hitch=")) + Hitch };
		Command->Execute(Args, nullptr, *GLog);
		// The build saves the vehicle as <name>_Vehicle in a folder of its name (build 8's log: /Temp/MantleHitchConsoleOut/HitchConsole/HitchConsole_Vehicle).
		const FString Path = Folder + TEXT("/HitchConsole/HitchConsole_Vehicle.HitchConsole_Vehicle");
		if (UMantleVehicleSource* Found = FindObject<UMantleVehicleSource>(nullptr, *Path); IsValid(Found))
		{
			return Found;
		}
		// Else any live vehicle the build made in that folder.
		for (TObjectIterator<UMantleVehicleSource> It; It; ++It)
		{
			if (IsValid(*It) && It->GetPathName().StartsWith(Folder + TEXT("/HitchConsole/")))
			{
				return *It;
			}
		}
		return nullptr;
	};

	UMantleVehicleSource* Ball = BuildWith(TEXT("ball"));
	if (!TestNotNull(TEXT("hitch=ball builds the vehicle"), Ball))
	{
		return false;
	}
	TestTrue(TEXT("hitch=ball gives it a tow ball"), Ball->Tuning.Hitch == EMantleVehicleSourceHitch::Ball);
	const FVector Default = FMantleVehicleBuilder::DefaultHitchLocation(*Ball, EMantleVehicleHitchKind::Ball);
	TestTrue(*FString::Printf(TEXT("at the default place (%s, want %s)"), *Ball->Tuning.HitchLocation.ToString(), *Default.ToString()),
		Ball->Tuning.HitchLocation.Equals(Default, 0.01));
	TestTrue(TEXT("behind the body, on its centre line"), Default.X < 0.0 && FMath::IsNearlyZero(Default.Y));

	UMantleVehicleSource* Own = BuildWith(TEXT("own"));
	if (TestNotNull(TEXT("hitch=own builds it again"), Own))
	{
		TestTrue(TEXT("hitch=own puts the type's own back"), Own->Tuning.Hitch == EMantleVehicleSourceHitch::Preset);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
