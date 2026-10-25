// Copyright Joshua Eger. All Rights Reserved.
#include "MantleChassisSpec.h"
#include "MantleVehicleBuilder.h"
#include "MantleVehicleChassisBuilder.h"
#include "MantleVehiclePreset.h"
#include "MantleVehicleShape.h"
#include "MantleVehicleSource.h"
#include "MantleVehicleSpawnPoint.h"
#include "SMantleVehicleBuilder.h"

#include "AssetRegistry/AssetData.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

// The Build a vehicle window per vehicle type: a card for every publishable type, and a shape step and a summary that suit each.

// The window's own state, which a test drives the way the buttons do. Named, not anonymous: MantleEditor builds as a unity build.
struct FMantleVehicleBuilderPanelAccess
{
	static void PickType(SMantleVehicleBuilder& Panel, const TCHAR* Id, const TCHAR* Name) { Panel.PickPreset(Id, FText::FromString(Name)); }
	static void Give(SMantleVehicleBuilder& Panel, UMantleVehicleSource* Built)
	{
		Panel.BuiltSource = Built;
		Panel.bHaveLastBuild = true;
		Panel.LastBuild.bOk = true;
		Panel.SuggestedSize = 1.25f;
		Panel.SaveStatus = FText::FromString(TEXT("Saved in your Content Browser."));
	}
	static bool HasBuild(const SMantleVehicleBuilder& Panel)
	{
		return Panel.BuiltSource.IsValid() || Panel.bHaveLastBuild || Panel.SuggestedSize.IsSet() || !Panel.SaveStatus.IsEmpty();
	}
	static void SetMesh(SMantleVehicleBuilder& Panel, USkeletalMesh* Mesh) { Panel.SetMesh(FAssetData(Mesh)); }
	static void AddGrey(SMantleVehicleBuilder& Panel, AMantleVehicleSpawnPoint* Spawn) { Panel.GreySpawns.Add(Spawn); }
	static void Finish(SMantleVehicleBuilder& Panel) { Panel.FinishWindow(); }
};

namespace MantleVehicleBuilderTypesTestPrivate
{
	bool Load(FAutomationTestBase& Test, const TCHAR* Id, FMantleChassisSpec& Out)
	{
		FString Error;
		if (!FMantleVehicleChassisBuilder::LoadSpec(Id, Out, Error))
		{
			Test.AddError(FString::Printf(TEXT("The %s preset did not load: %s"), Id, *Error));
			return false;
		}
		return true;
	}

	/** The speed the plane's wing lifts it off at, km/h, worked out here from the preset's numbers and the schema's formula, apart from the shape code. */
	double ExpectedTakeOffKmh(const double MassKg, const FMantleChassisSurface& Wing, const double WingAreaM2)
	{
		const double Degrees = (FMantleVehicleShape::TakeOffPitchDegrees + Wing.Camber) * 90.0 / Wing.StallAngle;
		const double Lift = 2.0 * FMath::Sin(FMath::DegreesToRadians(Degrees));
		return FMath::Sqrt(2.0 * MassKg * 9.8 / (1.225 * WingAreaM2 * Lift)) * 3.6;
	}

	/** The range of every number a Scaled chassis keeps in step with the size (centimetres, so they follow the factor, not its square). */
	bool Near(const double A, const double B, const double Relative)
	{
		return FMath::Abs(A - B) <= Relative * FMath::Max(FMath::Abs(A), FMath::Abs(B)) + 1.0e-9;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleBuilderCardsTest,
	"Mantle.Vehicle.Builder.CardForEveryPublishableType",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleBuilderCardsTest::RunTest(const FString& Parameters)
{
	const TArray<const FMantleVehiclePreset*> Cards = FMantleVehiclePresets::Publishable();

	// Every type a creator can build has a card: three kinds of ground vehicle, the bike and the three that leave the ground.
	const TCHAR* const Expected[] = { TEXT("sedan"), TEXT("sports"), TEXT("limousine"), TEXT("pickup"), TEXT("van"), TEXT("truck"), TEXT("sixwheeler"),
		TEXT("motorcycle"), TEXT("plane"), TEXT("jet"), TEXT("helicopter"), TEXT("hovercraft") };
	for (const TCHAR* Id : Expected)
	{
		TestTrue(*FString::Printf(TEXT("%s has a card"), Id), Cards.ContainsByPredicate([Id](const FMantleVehiclePreset* Card) { return Card->Id == Id; }));
	}

	int32 PublishableCount = 0;
	for (const FMantleVehiclePreset& Preset : FMantleVehiclePresets::All())
	{
		PublishableCount += Preset.IsPublishable() ? 1 : 0;
	}
	TestEqual(TEXT("a card for each publishable preset and for no other"), Cards.Num(), PublishableCount);

	// Each card is one plain sentence.
	for (const FMantleVehiclePreset* Card : Cards)
	{
		const FString Sentence = FMantleVehiclePresets::CardSentence(*Card);
		TestTrue(*FString::Printf(TEXT("%s has a sentence"), *Card->Id), !Sentence.IsEmpty() && Sentence.EndsWith(TEXT(".")));
		TestFalse(*FString::Printf(TEXT("%s's is one sentence"), *Card->Id), Sentence.Contains(TEXT(". ")));
		// Three tall road vehicles would each name vans first, so their cards say their own line (FMantleVehiclePresets::CardSentence); every other card is its file's first sentence.
		const bool bOwnLine = Card->Id == TEXT("pickup") || Card->Id == TEXT("van") || Card->Id == TEXT("truck");
		TestTrue(*FString::Printf(TEXT("%s's is the start of its description, or its own line"), *Card->Id), bOwnLine || Card->Description.StartsWith(Sentence.LeftChop(1)));
		TestTrue(*FString::Printf(TEXT("%s has a fit range to hold its size to"), *Card->Id), Card->FitMinScale > 0.0 && Card->FitMaxScale >= Card->FitMinScale);
	}

	// The sentence cutter: the first sentence, a stop added where the text has none, nothing from nothing, and a stop inside a number is no sentence end.
	FMantleVehiclePreset Said;
	const auto Card = [&Said](const TCHAR* Description)
	{
		Said.Description = Description;
		return FMantleVehiclePresets::CardSentence(Said);
	};
	TestEqual(TEXT("two sentences give the first"), Card(TEXT("A car. It drives.")), FString(TEXT("A car.")));
	TestEqual(TEXT("one sentence is itself"), Card(TEXT("A plane.")), FString(TEXT("A plane.")));
	TestEqual(TEXT("one with no stop gets one"), Card(TEXT("A jet")), FString(TEXT("A jet.")));
	TestEqual(TEXT("a stop inside a number is not the end"), Card(TEXT("Tops out at 2.5 m/s. Slowly.")), FString(TEXT("Tops out at 2.5 m/s.")));
	TestEqual(TEXT("surrounding space is trimmed"), Card(TEXT("  A van.  ")), FString(TEXT("A van.")));
	TestEqual(TEXT("no description, no sentence"), Card(TEXT("")), FString());

	// Road vehicles first, then what flies, then what floats.
	const auto Rank = [](const FMantleVehiclePreset* Card)
	{
		return Card->MovementType == TEXT("wheeled") ? 0 : Card->MovementType == TEXT("plane") ? 1 : Card->MovementType == TEXT("helicopter") ? 2 : 3;
	};
	bool bInOrder = true;
	for (int32 Index = 1; Index < Cards.Num(); ++Index)
	{
		bInOrder &= Rank(Cards[Index - 1]) <= Rank(Cards[Index]);
	}
	TestTrue(TEXT("the cards run from road vehicles to planes to helicopters to hovercraft"), bInOrder);

	// A preset whose file does not say it is ready is no card (positive control for the filter): the sedan's file with the flag turned off.
	FString Json;
	if (FFileHelper::LoadFileToString(Json, *(FMantleVehiclePresets::Directory() / TEXT("sedan.json"))))
	{
		Json.ReplaceInline(TEXT("\"publishable\": true"), TEXT("\"publishable\": false"));
		FMantleVehiclePreset Unready;
		FString Error;
		if (TestTrue(TEXT("the unmarked sedan still parses"), FMantleVehiclePresets::Parse(Json, Unready, Error)))
		{
			TestFalse(TEXT("but is not publishable"), Unready.IsPublishable());
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleBuilderSizeTest,
	"Mantle.Vehicle.Builder.AirborneAndHoverShapeIsSize",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleBuilderSizeTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleBuilderTypesTestPrivate;

	// Cars shape by eight sizes and take no Size; the others shape by one Size and take no sizes.
	FMantleChassisSpec Sedan;
	if (Load(*this, TEXT("sedan"), Sedan))
	{
		FMantleVehicleShapeSession Session;
		Session.Start(Sedan, 0.5, 2.0);
		TestFalse(TEXT("a sedan has no Size"), Session.UsesSize());
		TestFalse(TEXT("a Size set on it changes nothing"), Session.SetSize(1.5));
		TestTrue(TEXT("its wheelbase moves"), Session.SetDimension(EMantleVehicleDimension::Wheelbase, 300.0));
		TestTrue(TEXT("its summary is the ground one"), FMantleVehicleShape::Summarise(Session.GetShaped()).Family == EMantleVehicleFamily::Ground);
	}

	for (const TCHAR* Id : { TEXT("plane"), TEXT("jet"), TEXT("helicopter"), TEXT("hovercraft") })
	{
		FMantleChassisSpec Archetype;
		if (!Load(*this, Id, Archetype))
		{
			continue;
		}
		FMantleVehicleShapeSession Session;
		Session.Start(Archetype, 0.5, 2.0);
		TestTrue(*FString::Printf(TEXT("%s is shaped by Size"), Id), Session.UsesSize());
		TestFalse(*FString::Printf(TEXT("%s starts unshaped"), Id), Session.IsShaped());
		TestFalse(*FString::Printf(TEXT("%s's wheelbase cannot be moved on its own"), Id), Session.SetDimension(EMantleVehicleDimension::Wheelbase, 123.0));

		// Positive control: a Size of 1 is the preset itself, hand-tuned, and 1.5 is the preset sized by Scaled, every number alike.
		TestFalse(*FString::Printf(TEXT("%s at Size 1 has no derived handling"), Id), Session.GetShaped().Derived.IsSet());
		const FString SignatureBefore = Session.Signature();
		TestTrue(*FString::Printf(TEXT("%s takes a Size of 1.5"), Id), Session.SetSize(1.5));
		const FMantleChassisSpec Scaled = FMantleChassisSpec::Scaled(Archetype, 1.5);
		const FMantleChassisSpec& Shaped = Session.GetShaped();
		TestTrue(*FString::Printf(TEXT("%s is shaped now"), Id), Session.IsShaped() && Shaped.Derived.IsSet());
		TestTrue(*FString::Printf(TEXT("%s's body is the preset's times 1.5"), Id),
			Near(Shaped.Length, Archetype.Length * 1.5, 1.0e-9) && Near(Shaped.Width, Archetype.Width * 1.5, 1.0e-9) && Near(Shaped.Height, Archetype.Height * 1.5, 1.0e-9));
		TestTrue(*FString::Printf(TEXT("%s's mass is Scaled's"), Id), Near(Shaped.Derived->Mass, Scaled.Derived->Mass, 1.0e-9));
		// The mass follows the body's volume (1.5 cubed) unless the document's mass range held it, which the window then says in words.
		const bool bMassHeld = Shaped.Derived->Clamped.ContainsByPredicate([](const FString& Line) { return Line.StartsWith(TEXT("mass ")); });
		if (bMassHeld)
		{
			TestTrue(*FString::Printf(TEXT("%s's mass was held (%.0f kg) and says so"), Id, Shaped.Derived->Mass),
				Shaped.Derived->Mass < Archetype.Archetype.Mass * 3.375 && Session.GetWarnings().ToString().Contains(TEXT("as heavy as Mantle allows")));
		}
		else
		{
			TestTrue(*FString::Printf(TEXT("%s's mass followed the Size (about 1.5 cubed)"), Id), Near(Shaped.Derived->Mass, Archetype.Archetype.Mass * 3.375, 0.02));
		}
		TestTrue(*FString::Printf(TEXT("%s's signature changed with its Size"), Id), Session.Signature() != SignatureBefore);

		if (Archetype.Aircraft.bSet)
		{
			// The airframe grows with the fuselage: wing, tail and fin by 1.5 each way, so their areas by 2.25 (Reshape would have left them at 1).
			TestTrue(*FString::Printf(TEXT("%s's wing span and chord are 1.5 times"), Id),
				Near(Shaped.Aircraft.Wing.Span, Archetype.Aircraft.Wing.Span * 1.5, 1.0e-9) && Near(Shaped.Aircraft.Wing.Chord, Archetype.Aircraft.Wing.Chord * 1.5, 1.0e-9));
			TestTrue(*FString::Printf(TEXT("%s's tailplane and fin grew too"), Id),
				Near(Shaped.Aircraft.Tailplane.Span, Archetype.Aircraft.Tailplane.Span * 1.5, 1.0e-9) && Near(Shaped.Aircraft.Fin.Span, Archetype.Aircraft.Fin.Span * 1.5, 1.0e-9));
			TestTrue(*FString::Printf(TEXT("%s's wing area is 2.25 times (the way Scaled does it)"), Id),
				Near(Shaped.Derived->WingAreaM2, Archetype.Aircraft.Wing.AreaM2 * 2.25, 1.0e-3) && Near(Shaped.Derived->WingAreaM2, Scaled.Derived->WingAreaM2, 1.0e-9));
		}
		if (Archetype.Helicopter.bSet)
		{
			TestTrue(*FString::Printf(TEXT("%s's rotor is 1.5 times as wide"), Id), Near(Shaped.Helicopter.RotorDiameter, Archetype.Helicopter.RotorDiameter * 1.5, 1.0e-9));
			TestTrue(*FString::Printf(TEXT("%s's rotor thrust follows its mass"), Id), Near(Shaped.Derived->RotorThrustNewtons, Archetype.Archetype.RotorThrustNewtons * 3.375, 0.02));
		}
		if (Archetype.Hovercraft.bSet)
		{
			TestTrue(*FString::Printf(TEXT("%s's pads and hover height grew"), Id),
				Near(Shaped.Hovercraft.PadTravel, Archetype.Hovercraft.PadTravel * 1.5, 1.0e-9) && Near(Shaped.Clearance, Archetype.Clearance * 1.5, 1.0e-9));
		}

		// Reset puts it back.
		TestTrue(*FString::Printf(TEXT("%s resets"), Id), Session.Reset());
		TestTrue(*FString::Printf(TEXT("%s is back to its own size"), Id), !Session.IsShaped() && Session.GetSize() == 1.0 && !Session.GetShaped().Derived.IsSet());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleBuilderSizeRangeTest,
	"Mantle.Vehicle.Builder.SizeIsHeldToTheFitRange",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleBuilderSizeRangeTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleBuilderTypesTestPrivate;
	FMantleChassisSpec Heli;
	if (!Load(*this, TEXT("helicopter"), Heli))
	{
		return false;
	}
	const FMantleVehiclePreset* Preset = FMantleVehiclePresets::Find(TEXT("helicopter"));
	if (!TestNotNull(TEXT("the helicopter preset is there"), Preset))
	{
		return false;
	}

	// The preset's own range is the window's, not the wider one of a selector (0.25 to 2.5).
	FMantleVehicleShapeSession Session;
	Session.Start(Heli, Preset->FitMinScale, Preset->FitMaxScale);
	TestEqual(TEXT("the slider's range is the preset's fit range"), Session.GetSizeRange().Min, Preset->FitMinScale);
	TestEqual(TEXT("both ends"), Session.GetSizeRange().Max, Preset->FitMaxScale);
	TestTrue(TEXT("narrower than a selector's"), Session.GetSizeRange().Min > FMantleChassisSpec::MinScale && Session.GetSizeRange().Max < FMantleChassisSpec::MaxScale);

	TestTrue(TEXT("a Size far too big changes the Size"), Session.SetSize(10.0));
	TestEqual(TEXT("to the most the preset allows"), Session.GetSize(), Preset->FitMaxScale);
	if (TestEqual(TEXT("and says so"), Session.GetHeldNotes().Num(), 1))
	{
		TestTrue(TEXT("in plain words"), Session.GetHeldNotes()[0].ToString().Contains(TEXT("can't go above")));
		TestTrue(TEXT("that are in the warnings under the sliders"), Session.GetWarnings().ToString().Contains(TEXT("can't go above")));
	}
	TestTrue(TEXT("the chassis is the preset sized by that"), Near(Session.GetShaped().Length, Heli.Length * Preset->FitMaxScale, 1.0e-9));
	TestFalse(TEXT("the same too-big Size again changes nothing"), Session.SetSize(10.0));
	TestTrue(TEXT("a Size far too small is held at the least"), Session.SetSize(0.01));
	TestEqual(TEXT("the preset's"), Session.GetSize(), Preset->FitMinScale);
	TestTrue(TEXT("one in range clears the note"), Session.SetSize(1.2) && Session.GetHeldNotes().Num() == 0 && Session.GetSize() == 1.2);
	TestFalse(TEXT("a number that is not one changes nothing"), Session.SetSize(std::numeric_limits<double>::quiet_NaN()));

	// A session started without a range holds to the selector's.
	FMantleVehicleShapeSession Plain;
	Plain.Start(Heli);
	TestEqual(TEXT("the default range's least"), Plain.GetSizeRange().Min, FMantleChassisSpec::MinScale);
	TestEqual(TEXT("and most"), Plain.GetSizeRange().Max, FMantleChassisSpec::MaxScale);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleBuilderFitRangeHoldsNothingTest,
	"Mantle.Vehicle.Builder.FitRangeClampsNothing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleBuilderFitRangeHoldsNothingTest::RunTest(const FString& Parameters)
{
	// A plane, jet, helicopter or hovercraft is shaped by one Size held to its preset's fit range, and the range is the one inside which the derivation
	// holds no handling number at a limit and the mass stays inside the preset's own mass range. At each end nothing is held; one step of 0.05 past it,
	// something is (positive control: the range is not simply narrower than it needs to be).
	for (const TCHAR* Id : { TEXT("plane"), TEXT("jet"), TEXT("helicopter"), TEXT("hovercraft") })
	{
		FMantleChassisSpec Archetype;
		const FMantleVehiclePreset* Preset = FMantleVehiclePresets::Find(Id);
		FVector2D MassRange;
		if (!MantleVehicleBuilderTypesTestPrivate::Load(*this, Id, Archetype) || !TestNotNull(*FString::Printf(TEXT("%s preset"), Id), Preset)
			|| !TestTrue(*FString::Printf(TEXT("%s has a mass range"), Id), FMantleVehiclePresets::RangeFor(*Preset, TEXT("mass"), MassRange)))
		{
			continue;
		}
		const auto Problem = [&](const double Size) -> FString
		{
			const FMantleChassisSpec Sized = FMantleChassisSpec::Scaled(Archetype, Size);
			if (!Sized.Derived.IsSet())
			{
				return FString();
			}
			if (Sized.Derived->Clamped.Num() > 0)
			{
				return FString::Join(Sized.Derived->Clamped, TEXT("; "));
			}
			const double Mass = Sized.Derived->Mass;
			return Mass < MassRange.X || Mass > MassRange.Y ? FString::Printf(TEXT("mass %.0f outside %.0f to %.0f"), Mass, MassRange.X, MassRange.Y) : FString();
		};
		const FString AtLeast = Problem(Preset->FitMinScale);
		const FString AtMost = Problem(Preset->FitMaxScale);
		TestTrue(*FString::Printf(TEXT("%s at its least Size %.2f holds nothing (%s)"), Id, Preset->FitMinScale, *AtLeast), AtLeast.IsEmpty());
		TestTrue(*FString::Printf(TEXT("%s at its most Size %.2f holds nothing (%s)"), Id, Preset->FitMaxScale, *AtMost), AtMost.IsEmpty());
		TestFalse(*FString::Printf(TEXT("%s a step below its least holds something"), Id), Problem(Preset->FitMinScale - 0.05).IsEmpty());
		TestFalse(*FString::Printf(TEXT("%s a step above its most holds something"), Id), Problem(Preset->FitMaxScale + 0.05).IsEmpty());
		// Rounded to 0.05, so the window shows tidy numbers.
		TestTrue(*FString::Printf(TEXT("%s's range is in steps of 0.05"), Id),
			FMath::IsNearlyEqual(Preset->FitMinScale / 0.05, FMath::RoundToDouble(Preset->FitMinScale / 0.05), 1.0e-6)
				&& FMath::IsNearlyEqual(Preset->FitMaxScale / 0.05, FMath::RoundToDouble(Preset->FitMaxScale / 0.05), 1.0e-6));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleBuilderSummaryTest,
	"Mantle.Vehicle.Builder.SummaryPerType",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleBuilderSummaryTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleBuilderTypesTestPrivate;

	// Planes and jets: weight, wing loading, take-off speed and the air-speed limit; nothing about turning circles.
	for (const TCHAR* Id : { TEXT("plane"), TEXT("jet") })
	{
		FMantleChassisSpec Archetype;
		if (!Load(*this, Id, Archetype))
		{
			continue;
		}
		const FMantleVehicleSummary Own = FMantleVehicleShape::Summarise(Archetype);
		TestTrue(*FString::Printf(TEXT("%s is an aircraft"), Id), Own.Family == EMantleVehicleFamily::Aircraft);
		TestTrue(*FString::Printf(TEXT("%s weighs what its preset says"), Id), Near(Own.MassKg, Archetype.Archetype.Mass, 1.0e-9));
		TestTrue(*FString::Printf(TEXT("%s's wing loading is its weight over its wing"), Id), Near(Own.WingLoadingKgM2, Archetype.Archetype.Mass / Archetype.Aircraft.Wing.AreaM2, 1.0e-9));
		const double Expected = ExpectedTakeOffKmh(Archetype.Archetype.Mass, Archetype.Aircraft.Wing, Archetype.Aircraft.Wing.AreaM2);
		TestTrue(*FString::Printf(TEXT("%s takes off at %.1f km/h, worked out apart from the shape code (%.1f)"), Id, Own.TakeOffKmh, Expected), Near(Own.TakeOffKmh, Expected, 1.0e-6));
		TestTrue(*FString::Printf(TEXT("%s takes off slower than it flies and faster than a walk"), Id), Own.TakeOffKmh > 60.0 && Own.TakeOffKmh < Own.TopSpeedKmh);
		TestTrue(*FString::Printf(TEXT("%s's top speed is its air-speed limit"), Id), Near(Own.TopSpeedKmh, Archetype.Aircraft.AirSpeedLimitCmPerSec * 0.036, 1.0e-9));
		const FString Said = FMantleVehicleShape::Describe(Own).ToString();
		TestTrue(*FString::Printf(TEXT("%s says what it weighs"), Id), Said.StartsWith(TEXT("Weighs about ")));
		TestTrue(*FString::Printf(TEXT("%s says its wing loading"), Id), Said.Contains(TEXT(" kg on each square metre of wing.")));
		TestTrue(*FString::Printf(TEXT("%s says its take-off speed"), Id), Said.Contains(TEXT("Takes off at about ")) && Said.Contains(TEXT(" km/h. Top speed about ")));
		TestFalse(*FString::Printf(TEXT("%s does not turn like a car"), Id), Said.Contains(TEXT("Turns in a circle")));

		// Positive control: sized up 1.2 times it weighs 1.728 times as much on 1.44 times the wing, so its wing loading rises by 1.2 and its take-off
		// speed by the square root of that; the air-speed limit does not move (speeds do not scale). (1.2 keeps the mass inside the document's range.)
		FMantleVehicleShapeSession Session;
		Session.Start(Archetype, 0.5, 2.0);
		Session.SetSize(1.2);
		const FMantleVehicleSummary Bigger = FMantleVehicleShape::Summarise(Session.GetShaped());
		TestTrue(*FString::Printf(TEXT("%s's wing loading rises by 1.2 with Size 1.2 (%.1f to %.1f)"), Id, Own.WingLoadingKgM2, Bigger.WingLoadingKgM2),
			Near(Bigger.WingLoadingKgM2, Own.WingLoadingKgM2 * 1.2, 0.03));
		TestTrue(*FString::Printf(TEXT("%s's take-off speed rises by the square root of 1.2 (%.1f to %.1f)"), Id, Own.TakeOffKmh, Bigger.TakeOffKmh),
			Near(Bigger.TakeOffKmh, Own.TakeOffKmh * FMath::Sqrt(1.2), 0.03));
		TestTrue(*FString::Printf(TEXT("%s's top speed is the same"), Id), Near(Bigger.TopSpeedKmh, Own.TopSpeedKmh, 1.0e-9));
		TestTrue(*FString::Printf(TEXT("the session says %s's new numbers, not the old ones"), Id), Session.GetSummary().ToString() != Said);
	}

	// A wing too small to lift the plane is said so instead of a speed.
	{
		FMantleVehicleSummary Grounded;
		Grounded.Family = EMantleVehicleFamily::Aircraft;
		Grounded.MassKg = 1000.0;
		Grounded.WingLoadingKgM2 = 100.0;
		Grounded.TopSpeedKmh = 300.0;
		const FString Said = FMantleVehicleShape::Describe(Grounded).ToString();
		TestTrue(TEXT("a plane with no lift says so"), Said.Contains(TEXT("too small to lift it")) && !Said.Contains(TEXT("Takes off")));
	}

	// The helicopter: weight and the throttle that hovers it.
	FMantleChassisSpec Heli;
	if (Load(*this, TEXT("helicopter"), Heli))
	{
		const FMantleVehicleSummary Own = FMantleVehicleShape::Summarise(Heli);
		TestTrue(TEXT("it is a helicopter"), Own.Family == EMantleVehicleFamily::Helicopter);
		TestTrue(TEXT("its hover throttle is the preset's own collective"), Near(Own.HoverThrottle, Heli.Helicopter.HoverCollective, 0.02));
		const FString Said = FMantleVehicleShape::Describe(Own).ToString();
		TestTrue(TEXT("it says what it weighs and where it hovers"), Said.StartsWith(TEXT("Weighs about ")) && Said.Contains(TEXT(" Hovers at about ")) && Said.EndsWith(TEXT(" percent throttle.")));
		TestFalse(TEXT("and nothing of turning circles or top speeds"), Said.Contains(TEXT("Turns in")) || Said.Contains(TEXT("Top speed")));

		// Its rotor lifts the same share of its weight at every size.
		FMantleVehicleShapeSession Session;
		Session.Start(Heli, 0.5, 2.0);
		Session.SetSize(1.5);
		const FMantleVehicleSummary Bigger = FMantleVehicleShape::Summarise(Session.GetShaped());
		TestTrue(TEXT("a bigger helicopter is heavier"), Bigger.MassKg > Own.MassKg * 3.0);
		TestTrue(TEXT("and hovers at the same throttle"), Near(Bigger.HoverThrottle, Own.HoverThrottle, 0.02));

		FMantleVehicleSummary TooHeavy = Own;
		TooHeavy.HoverThrottle = 1.3;
		TestTrue(TEXT("a rotor too weak to lift it says so"), FMantleVehicleShape::Describe(TooHeavy).ToString().Contains(TEXT("rotor is too weak")));
	}

	// The hovercraft: weight, hover height and top speed.
	FMantleChassisSpec Hover;
	if (Load(*this, TEXT("hovercraft"), Hover))
	{
		const FMantleVehicleSummary Own = FMantleVehicleShape::Summarise(Hover);
		TestTrue(TEXT("it is a hovercraft"), Own.Family == EMantleVehicleFamily::Hovercraft);
		TestTrue(TEXT("it floats at its ground clearance"), Near(Own.HoverHeightCm, Hover.Clearance, 1.0e-9));
		// schema/validate.py holds the preset to a top speed of 15 to 25 m/s on the flat.
		TestTrue(*FString::Printf(TEXT("its top speed (%.1f km/h) is the one the validator solves for (54 to 90)"), Own.TopSpeedKmh), Own.TopSpeedKmh > 54.0 && Own.TopSpeedKmh < 90.0);
		const FString Said = FMantleVehicleShape::Describe(Own).ToString();
		TestTrue(TEXT("it says its weight, height and speed"),
			Said.StartsWith(TEXT("Weighs about ")) && Said.Contains(FString::Printf(TEXT("Floats about %d cm above the ground."), FMath::RoundToInt(Hover.Clearance))) && Said.Contains(TEXT("Top speed about ")));
		TestFalse(TEXT("and nothing of turning circles"), Said.Contains(TEXT("Turns in")));

		// The feel: positive control that the slider moves the speed, both ways, and that a bigger one floats higher.
		FMantleVehicleShapeSession Session;
		Session.Start(Hover, 0.5, 2.0);
		TestTrue(TEXT("a hovercraft has the top-speed feel"), Session.HasTopSpeedFeel());
		TestTrue(TEXT("and the passenger seat to turn off"), Session.HasPassengerOption() && Session.GetPassenger());
		TestTrue(TEXT("the feel moves"), Session.SetTopSpeedFeel(1.0));
		const double Fast = FMantleVehicleShape::Summarise(Session.GetShaped()).TopSpeedKmh;
		Session.SetTopSpeedFeel(-1.0);
		const double Slow = FMantleVehicleShape::Summarise(Session.GetShaped()).TopSpeedKmh;
		TestTrue(*FString::Printf(TEXT("faster at the top of the slider (%.1f) than the preset (%.1f) and slower at the bottom (%.1f)"), Fast, Own.TopSpeedKmh, Slow),
			Fast > Own.TopSpeedKmh * 1.1 && Slow < Own.TopSpeedKmh * 0.9);
		TestTrue(TEXT("the passenger seat comes off"), Session.SetPassenger(false) && !Session.GetShaped().bPassenger);
		Session.Reset();
		TestTrue(TEXT("Reset puts the feel and the seat back"), !Session.IsShaped() && Session.GetTopSpeedFeel() == 0.0 && Session.GetPassenger());
		Session.SetSize(2.0);
		TestTrue(TEXT("sized up it floats higher"), Near(FMantleVehicleShape::Summarise(Session.GetShaped()).HoverHeightCm, Hover.Clearance * 2.0, 1.0e-9));
	}

	// A plane has no feel or seat to turn off; a ground vehicle's summary is what it was.
	FMantleChassisSpec Plane;
	if (Load(*this, TEXT("plane"), Plane))
	{
		FMantleVehicleShapeSession Session;
		Session.Start(Plane);
		TestFalse(TEXT("a plane has no top-speed feel"), Session.HasTopSpeedFeel() || Session.SetTopSpeedFeel(0.5));
		TestFalse(TEXT("and no passenger seat to turn off"), Session.HasPassengerOption() || Session.SetPassenger(true));
	}
	FMantleChassisSpec Sedan;
	if (Load(*this, TEXT("sedan"), Sedan))
	{
		const FString Said = FMantleVehicleShape::Describe(FMantleVehicleShape::Summarise(Sedan)).ToString();
		TestTrue(TEXT("a car still says how it turns"), Said.StartsWith(TEXT("Weighs about ")) && Said.Contains(TEXT("Turns in a circle about ")) && Said.EndsWith(TEXT(" km/h.")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleBuilderWarningsTest,
	"Mantle.Vehicle.Builder.WheelsPastBodyIsForRoadVehicles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleBuilderWarningsTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleBuilderTypesTestPrivate;
	const auto SaysWheelsStickOut = [](const FMantleChassisSpec& Spec)
	{
		for (const FText& Line : FMantleVehicleShape::Warnings(Spec))
		{
			if (Line.ToString().Contains(TEXT("wheels stick out")))
			{
				return true;
			}
		}
		return false;
	};

	// Positive control: a car with a body shorter than its wheelbase says so.
	FMantleChassisSpec Sedan;
	if (Load(*this, TEXT("sedan"), Sedan))
	{
		FMantleVehicleDimensions Short = FMantleVehicleShape::DimensionsOf(Sedan);
		Short.BodyLength = 0.5 * Sedan.Wheelbase;
		TestTrue(TEXT("a car whose body is far shorter than its wheelbase warns"), SaysWheelsStickOut(FMantleVehicleShape::Reshape(Sedan, Short)));
	}

	// A hovercraft's pads stand at a share of its hull and are never drawn: it is exempt, at the preset's size and sized up.
	FMantleChassisSpec Hover;
	if (Load(*this, TEXT("hovercraft"), Hover))
	{
		TestFalse(TEXT("the hovercraft at its own size does not warn"), SaysWheelsStickOut(Hover));
		TestFalse(TEXT("nor sized up"), SaysWheelsStickOut(FMantleChassisSpec::Scaled(Hover, 1.7)));
		FMantleChassisSpec Narrow = Hover;
		Narrow.Length = 0.5 * Hover.Wheelbase;
		Narrow.Width = 0.5 * Hover.Track;
		TestFalse(TEXT("even with a body smaller than its pads (they are not wheels)"), SaysWheelsStickOut(Narrow));
	}
	for (const TCHAR* Id : { TEXT("plane"), TEXT("jet"), TEXT("helicopter") })
	{
		FMantleChassisSpec Spec;
		if (Load(*this, Id, Spec))
		{
			TestFalse(*FString::Printf(TEXT("%s does not warn either"), Id), SaysWheelsStickOut(FMantleChassisSpec::Scaled(Spec, 1.4)));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleBuilderSetMeshTest,
	"Mantle.Vehicle.Builder.ChangingTheModelForgetsTheBuild",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleBuilderSetMeshTest::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddWarning(TEXT("SKIPPED, not passed: there is no Slate application in this runner, so the window cannot be made."));
		return true;
	}
	USkeletalMesh* First = NewObject<USkeletalMesh>(GetTransientPackage(), NAME_None, RF_Transient);
	USkeletalMesh* Second = NewObject<USkeletalMesh>(GetTransientPackage(), NAME_None, RF_Transient);
	UMantleVehicleSource* Built = NewObject<UMantleVehicleSource>(GetTransientPackage(), NAME_None, RF_Transient);
	if (!TestTrue(TEXT("the test objects exist"), First && Second && Built))
	{
		return false;
	}

	TSharedRef<SMantleVehicleBuilder> Panel = SNew(SMantleVehicleBuilder);
	FMantleVehicleBuilderPanelAccess::PickType(*Panel, TEXT("sedan"), TEXT("Sedan"));
	FMantleVehicleBuilderPanelAccess::SetMesh(*Panel, First);
	TestFalse(TEXT("a model with nothing built has no build"), FMantleVehicleBuilderPanelAccess::HasBuild(*Panel));

	FMantleVehicleBuilderPanelAccess::Give(*Panel, Built);
	TestTrue(TEXT("a build is held (positive control)"), FMantleVehicleBuilderPanelAccess::HasBuild(*Panel));
	FMantleVehicleBuilderPanelAccess::SetMesh(*Panel, First);
	TestTrue(TEXT("the same model again keeps it"), FMantleVehicleBuilderPanelAccess::HasBuild(*Panel));
	FMantleVehicleBuilderPanelAccess::SetMesh(*Panel, Second);
	TestFalse(TEXT("another model forgets the build, its size and its save line"), FMantleVehicleBuilderPanelAccess::HasBuild(*Panel));

	FMantleVehicleBuilderPanelAccess::Give(*Panel, Built);
	FMantleVehicleBuilderPanelAccess::SetMesh(*Panel, nullptr);
	TestFalse(TEXT("clearing the model forgets it too"), FMantleVehicleBuilderPanelAccess::HasBuild(*Panel));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleBuilderGreySpawnTest,
	"Mantle.Vehicle.Builder.CancelTakesTheGreyVehicleOut",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleBuilderGreySpawnTest::RunTest(const FString& Parameters)
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!FSlateApplication::IsInitialized() || !World || !World->PersistentLevel)
	{
		AddWarning(TEXT("SKIPPED, not passed: there is no Slate application or editor world in this runner, so the level cannot be checked."));
		return true;
	}
	const auto Count = [World]()
	{
		int32 Total = 0;
		for (TActorIterator<AMantleVehicleSpawnPoint> It(World); It; ++It)
		{
			++Total;
		}
		return Total;
	};
	const auto PlaceOne = [this](UMantleVehicleSource& Source) -> AMantleVehicleSpawnPoint*
	{
		FString Problem;
		AMantleVehicleSpawnPoint* Spawn = nullptr;
		if (!FMantleVehicleBuilder::PlaceInLevel(Source, Problem, &Spawn) || !Spawn)
		{
			AddError(FString::Printf(TEXT("A spawn could not be placed: %s"), *Problem));
		}
		return Spawn;
	};
	UMantleVehicleSource* Grey = NewObject<UMantleVehicleSource>(GetTransientPackage(), NAME_None, RF_Transient);
	UMantleVehicleSource* Model = NewObject<UMantleVehicleSource>(GetTransientPackage(), NAME_None, RF_Transient);
	const int32 Before = Count();

	// Cancel (the window closed any way but Done): the grey vehicle goes.
	{
		TSharedRef<SMantleVehicleBuilder> Panel = SNew(SMantleVehicleBuilder);
		AMantleVehicleSpawnPoint* Spawn = PlaceOne(*Grey);
		if (!Spawn)
		{
			return false;
		}
		FMantleVehicleBuilderPanelAccess::AddGrey(*Panel, Spawn);
		TestEqual(TEXT("the grey vehicle is in the level"), Count(), Before + 1);
		Panel = SNew(SMantleVehicleBuilder);
		TestEqual(TEXT("closing the window without Done took it out"), Count(), Before);
	}

	// Done with no model: the grey vehicle is the result and stays.
	{
		TSharedPtr<SMantleVehicleBuilder> Panel = SNew(SMantleVehicleBuilder);
		AMantleVehicleSpawnPoint* Spawn = PlaceOne(*Grey);
		if (!Spawn)
		{
			return false;
		}
		FMantleVehicleBuilderPanelAccess::AddGrey(*Panel, Spawn);
		FMantleVehicleBuilderPanelAccess::Finish(*Panel);
		Panel.Reset();
		TestEqual(TEXT("Done with no model leaves the grey vehicle in the level"), Count(), Before + 1);
		World->EditorDestroyActor(Spawn, /*bShouldModifyLevel*/ true);
	}

	// Done with a model whose vehicle is in the level: the grey one it replaced goes, the model's stays.
	{
		TSharedPtr<SMantleVehicleBuilder> Panel = SNew(SMantleVehicleBuilder);
		AMantleVehicleSpawnPoint* GreySpawn = PlaceOne(*Grey);
		AMantleVehicleSpawnPoint* ModelSpawn = PlaceOne(*Model);
		if (!GreySpawn || !ModelSpawn)
		{
			return false;
		}
		FMantleVehicleBuilderPanelAccess::AddGrey(*Panel, GreySpawn);
		FMantleVehicleBuilderPanelAccess::Give(*Panel, Model);
		FMantleVehicleBuilderPanelAccess::Finish(*Panel);
		Panel.Reset();
		TestEqual(TEXT("Done with the model's vehicle in the level took out only the grey one"), Count(), Before + 1);
		TestTrue(TEXT("and the model's is the one left"), IsValid(ModelSpawn) && ModelSpawn->GetLevel() != nullptr);
		World->EditorDestroyActor(ModelSpawn, /*bShouldModifyLevel*/ true);
	}
	TestEqual(TEXT("the level is as it was"), Count(), Before);
	return true;
}

#endif
