// Copyright Joshua Eger. All Rights Reserved.
#include "MantleChassisDerive.h"
#include "MantleChassisSpec.h"
#include "MantlePlaceValidator.h"
#include "MantleVehicleBuilder.h"
#include "MantleVehicleChassisBuilder.h"
#include "MantleVehicleChassisMesh.h"
#include "MantleVehicleConverter.h"
#include "MantleVehiclePreview.h"
#include "MantleVehicleShape.h"
#include "MantleVehicleSource.h"
#include "MantleVehicleSpawnPoint.h"

#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVehicleShapeTestPrivate
{
	/** The preset's chassis at its own size, or an error on Test. */
	bool LoadPreset(FAutomationTestBase& Test, const TCHAR* Id, FMantleChassisSpec& Out)
	{
		FString Error;
		if (!FMantleVehicleChassisBuilder::LoadSpec(Id, Out, Error))
		{
			Test.AddError(FString::Printf(TEXT("The %s preset did not load: %s"), Id, *Error));
			return false;
		}
		return true;
	}

	/**
	 * A throwaway project folder (under /Game, named for a test) and the assets in it: gone from memory, the asset registry and the disk when this goes
	 * out of scope, whatever the test did, and before it begins in case an earlier run died.
	 */
	struct FTestFolder
	{
		FString Folder;
		TArray<FString> AssetNames;

		FTestFolder(const FString& InFolder, TArray<FString> InAssetNames)
			: Folder(InFolder)
			, AssetNames(MoveTemp(InAssetNames))
		{
			Clean();
		}

		~FTestFolder()
		{
			Clean();
		}

		void Clean() const
		{
			for (const FString& AssetName : AssetNames)
			{
				if (UPackage* Package = FindPackage(nullptr, *(Folder / AssetName)))
				{
					if (UObject* Asset = FindObject<UObject>(Package, *AssetName))
					{
						FAssetRegistryModule::AssetDeleted(Asset);
						Asset->ClearFlags(RF_Public | RF_Standalone);
						Asset->MarkAsGarbage();
					}
					ResetLoaders(Package);
					Package->ClearDirtyFlag();
					Package->MarkAsGarbage();
				}
			}
			IFileManager::Get().DeleteDirectory(*FPackageName::LongPackageNameToFilename(Folder), false, true);
		}
	};

	/** Drops an asset the test made in a /Temp package, so nothing is left in the editor or on disk. */
	void Discard(UObject* Asset)
	{
		if (!Asset)
		{
			return;
		}
		UPackage* Package = Asset->GetOutermost();
		Asset->ClearFlags(RF_Public | RF_Standalone);
		Asset->MarkAsGarbage();
		if (Package && Package != GetTransientPackage())
		{
			Package->ClearDirtyFlag();
			Package->MarkAsGarbage();
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleShapeChangesHandlingTest,
	"Mantle.Vehicle.Builder.ShapeChangesDerivedHandling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleShapeChangesHandlingTest::RunTest(const FString& Parameters)
{
	FMantleChassisSpec Sedan;
	if (!MantleVehicleShapeTestPrivate::LoadPreset(*this, TEXT("sedan"), Sedan))
	{
		return false;
	}

	// The preset's own sizes are the preset, hand-tuned: nothing is derived until something is shaped.
	const FMantleVehicleDimensions Own = FMantleVehicleShape::DimensionsOf(Sedan);
	TestFalse(TEXT("the preset's own sizes are not shaped"), FMantleVehicleShape::IsShaped(Sedan, Own));
	const FMantleChassisSpec Unshaped = FMantleVehicleShape::Reshape(Sedan, Own);
	TestFalse(TEXT("an unshaped sedan keeps its hand-tuned handling"), Unshaped.Derived.IsSet());
	TestEqual(TEXT("an unshaped sedan is the preset's wheelbase"), Unshaped.Wheelbase, Sedan.Wheelbase);

	// A sedan with a 3.4 m wheelbase.
	TArray<FText> Notes;
	const FMantleVehicleDimensions Long = FMantleVehicleShape::Edit(Sedan, Own, EMantleVehicleDimension::Wheelbase, 340.0, Notes);
	TestEqual(TEXT("the wheelbase is 3.4 m"), Long.Wheelbase, 340.0);
	TestEqual(TEXT("340 cm needs no holding"), Notes.Num(), 0);
	TestTrue(TEXT("only the wheelbase changed"), Long.Track == Own.Track && Long.WheelRadius == Own.WheelRadius && Long.WheelWidth == Own.WheelWidth
		&& Long.BodyLength == Own.BodyLength && Long.BodyWidth == Own.BodyWidth && Long.BodyHeight == Own.BodyHeight && Long.GroundClearance == Own.GroundClearance);
	TestTrue(TEXT("the sedan with a longer wheelbase is shaped"), FMantleVehicleShape::IsShaped(Sedan, Long));
	const FMantleChassisSpec Spec = FMantleVehicleShape::Reshape(Sedan, Long);
	if (!TestTrue(TEXT("a shaped sedan has derived handling"), Spec.Derived.IsSet()))
	{
		return false;
	}

	// Mass is the body's volume, so a longer wheelbase leaves it where the derivation puts the preset's own sizes; the steering lock stays the
	// archetype's, so the turning circle (wheelbase over the tangent of the lock) grows with the wheelbase.
	const FMantleChassisHandling AtOwnSizes = FMantleChassisDerive::Derive(Sedan);
	TestTrue(TEXT("a longer wheelbase leaves the mass unchanged"), FMath::IsNearlyEqual(Spec.Derived->Mass, AtOwnSizes.Mass, AtOwnSizes.Mass * 0.005));
	const double Circle = FMantleVehicleShape::TurningCircleCm(Spec, Spec.Derived.GetValue());
	TestTrue(TEXT("a longer wheelbase turns in a bigger circle"), Circle > FMantleVehicleShape::TurningCircleCm(Sedan, AtOwnSizes));
	TestTrue(TEXT("and a bigger circle than the hand-tuned preset's"), Circle > FMantleVehicleShape::Summarize(Sedan).TurningCircleCm);
	const FMantleVehicleSummary Summary = FMantleVehicleShape::Summarize(Spec);
	TestEqual(TEXT("the summary's weight is the derived mass"), Summary.MassKg, Spec.Derived->Mass);
	TestEqual(TEXT("the summary's circle is the one measured here"), Summary.TurningCircleCm, Circle);

	// The geometry follows: the axles are half the wheelbase either side of the middle, the track is kept, the seat and the steering wheel
	// keep their places on the body, and the center of mass is the derived one.
	TestEqual(TEXT("the front axle is half the wheelbase ahead"), Spec.WheelCenter(0).X, 170.0);
	TestEqual(TEXT("the rear axle is half the wheelbase behind"), Spec.WheelCenter(2).X, -170.0);
	TestEqual(TEXT("the track is kept"), Spec.WheelCenter(1).Y - Spec.WheelCenter(0).Y, Sedan.Track);
	TestTrue(TEXT("the seat moves along with the wheelbase, nowhere else"), Spec.Seat.Equals(FVector(Sedan.Seat.X * 340.0 / Sedan.Wheelbase, Sedan.Seat.Y, Sedan.Seat.Z), 1.0e-6));
	TestTrue(TEXT("the steering wheel keeps its reach from the seat"), (Spec.Hub - Spec.Seat).Equals(Sedan.Hub - Sedan.Seat, 1.0e-6));
	TestTrue(TEXT("the center of mass is the derived one"), Spec.CenterOfMass.Equals(Spec.Derived->CenterOfMass, 1.0e-6));

	// The summary says so in a creator's words (the numbers only: a locale may spell a decimal its own way).
	const FString Said = FMantleVehicleShape::Describe(FMantleVehicleShape::Summarize(Sedan)).ToString();
	TestTrue(TEXT("the sentence starts with the weight"), Said.StartsWith(TEXT("Weighs about ")));
	TestTrue(TEXT("the sentence gives the turning circle"), Said.Contains(TEXT("Turns in a circle about 7 m across.")));
	TestTrue(TEXT("the weight is in metric tons"), Said.Contains(TEXT("metric tons. Turns in")));
	TestTrue(TEXT("the sentence gives the top speed"), Said.Contains(TEXT("Top speed about 160 km/h.")));

	// Ground clearance lifts the seat with the body; body height does not (a driver is the same size in any vehicle).
	FMantleVehicleDimensions Lifted = Own;
	Lifted.GroundClearance = Own.GroundClearance + 10.0;
	Lifted.BodyHeight = Own.BodyHeight + 30.0;
	const FMantleChassisSpec LiftedSpec = FMantleVehicleShape::Reshape(Sedan, Lifted);
	TestEqual(TEXT("ground clearance lifts the seat"), LiftedSpec.Seat.Z, Sedan.Seat.Z + 10.0);
	TestTrue(TEXT("a taller body is heavier"), LiftedSpec.Derived.IsSet() && LiftedSpec.Derived->Mass > AtOwnSizes.Mass);

	// Resetting to the preset's sizes gives the preset back, hand-tuned.
	TestFalse(TEXT("back at the preset's sizes it is the preset again"), FMantleVehicleShape::Reshape(Sedan, FMantleVehicleShape::DimensionsOf(Sedan)).Derived.IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleShapeHoldsTest,
	"Mantle.Vehicle.Builder.ShapeHoldsSizesTogether",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleShapeHoldsTest::RunTest(const FString& Parameters)
{
	FMantleChassisSpec Sedan;
	FMantleChassisSpec Bike;
	FMantleChassisSpec Truck;
	if (!MantleVehicleShapeTestPrivate::LoadPreset(*this, TEXT("sedan"), Sedan) || !MantleVehicleShapeTestPrivate::LoadPreset(*this, TEXT("motorcycle"), Bike)
		|| !MantleVehicleShapeTestPrivate::LoadPreset(*this, TEXT("sixwheeler"), Truck))
	{
		return false;
	}
	const FMantleVehicleDimensions Own = FMantleVehicleShape::DimensionsOf(Sedan);

	// Every slider covers 0.25 to 2.5 times the preset's size, inside the document's limits.
	for (const EMantleVehicleDimension Dimension : MantleVehicleDimensions)
	{
		const FMantleDimensionRange Range = FMantleVehicleShape::SliderRange(Sedan, Dimension);
		const double Base = Own.Get(Dimension);
		TestTrue(TEXT("a slider covers a range"), Range.Min <= Range.Max);
		TestTrue(TEXT("a slider never goes below a quarter of the preset's size"), Range.Min >= Base * FMantleChassisSpec::MinScale - 1.0e-9);
		TestTrue(TEXT("a slider never goes above two and a half times the preset's size"), Range.Max <= Base * FMantleChassisSpec::MaxScale + 1.0e-9);
		TestTrue(TEXT("the preset's own size is on its slider"), Range.Min <= Base + 1.0e-9 && Range.Max >= Base - 1.0e-9);
	}
	const FMantleDimensionRange Radius = FMantleVehicleShape::SliderRange(Sedan, EMantleVehicleDimension::WheelRadius);
	TestTrue(TEXT("a wheel is at least the document's 5 cm and at most its 150 cm"), Radius.Min >= 5.0 && Radius.Max <= 150.0);

	// A size typed past its range is held, with a plain sentence.
	TArray<FText> Notes;
	FMantleVehicleDimensions Held = FMantleVehicleShape::Edit(Sedan, Own, EMantleVehicleDimension::BodyLength, 99999.0, Notes);
	TestEqual(TEXT("a body past its slider is held at its end"), Held.BodyLength, FMantleVehicleShape::SliderRange(Sedan, EMantleVehicleDimension::BodyLength).Max);
	TestEqual(TEXT("and says so"), Notes.Num(), 1);
	TestTrue(TEXT("a number that is not one changes nothing"), FMantleVehicleShape::Edit(Sedan, Own, EMantleVehicleDimension::BodyLength, std::numeric_limits<double>::quiet_NaN(), Notes).BodyLength == Own.BodyLength);

	// Wheels never touch: the wheelbase stops at 2.2 wheel radii, and the wheels stop at the wheelbase over 2.2.
	Notes.Reset();
	Held = FMantleVehicleShape::Edit(Sedan, Own, EMantleVehicleDimension::Wheelbase, 5.0, Notes);
	TestTrue(TEXT("a wheelbase too short for its wheels is held longer"), Held.Wheelbase >= FMantleVehicleShape::MinAxleGapPerRadius * Own.WheelRadius - 1.0e-6);
	TestEqual(TEXT("and says so"), Notes.Num(), 1);
	Notes.Reset();
	const FMantleVehicleDimensions ShortBase = FMantleVehicleShape::Edit(Sedan, Own, EMantleVehicleDimension::Wheelbase, 100.0, Notes);
	TestEqual(TEXT("a 1 m wheelbase is allowed for these wheels"), ShortBase.Wheelbase, 100.0);
	Held = FMantleVehicleShape::Edit(Sedan, ShortBase, EMantleVehicleDimension::WheelRadius, 1000.0, Notes);
	TestTrue(TEXT("wheels too big for the wheelbase are held smaller"), Held.WheelRadius <= 100.0 / FMantleVehicleShape::MinAxleGapPerRadius + 1.0e-6);
	TestTrue(TEXT("and no smaller than the room they have"), Held.WheelRadius >= 100.0 / FMantleVehicleShape::MinAxleGapPerRadius - 1.0e-6);
	TestEqual(TEXT("and say so"), Notes.Num(), 1);
	Notes.Reset();
	Held = FMantleVehicleShape::Edit(Truck, FMantleVehicleShape::DimensionsOf(Truck), EMantleVehicleDimension::WheelRadius, 1000.0, Notes);
	TestTrue(TEXT("a three-axle truck's wheels are held by its closest axles"),
		Held.WheelRadius <= Truck.Wheelbase * (Truck.Axles[2].Position - Truck.Axles[1].Position) / FMantleVehicleShape::MinAxleGapPerRadius + 1.0e-6);
	Notes.Reset();
	Held = FMantleVehicleShape::Edit(Sedan, Own, EMantleVehicleDimension::Track, 1.0, Notes);
	TestTrue(TEXT("a track too narrow for its wheels is held wider"), Held.Track >= FMantleVehicleShape::MinTrackPerWidth * Own.WheelWidth - 1.0e-6);

	// A motorcycle's wheels are in a line: no track to change.
	TestFalse(TEXT("a motorcycle has no track slider"), FMantleVehicleShape::IsEditable(Bike, EMantleVehicleDimension::Track));
	TestTrue(TEXT("a car does"), FMantleVehicleShape::IsEditable(Sedan, EMantleVehicleDimension::Track));
	const FMantleVehicleDimensions BikeOwn = FMantleVehicleShape::DimensionsOf(Bike);
	TestEqual(TEXT("a motorcycle's track is not changed"), FMantleVehicleShape::Edit(Bike, BikeOwn, EMantleVehicleDimension::Track, 100.0, Notes).Track, BikeOwn.Track);

	// A motorcycle's rider moves with its seat: a higher ground clearance lifts the grips and pegs as far as the seat.
	FMantleVehicleDimensions BikeLifted = BikeOwn;
	BikeLifted.GroundClearance += 12.0;
	const FMantleChassisSpec BikeSpec = FMantleVehicleShape::Reshape(Bike, BikeLifted);
	TestEqual(TEXT("the rider's seat is lifted"), BikeSpec.Seat.Z, Bike.Seat.Z + 12.0);
	TestEqual(TEXT("and the grips with it"), BikeSpec.Rider.LeftGrip.Z, Bike.Rider.LeftGrip.Z + 12.0);
	TestEqual(TEXT("and the pegs"), BikeSpec.Rider.RightPeg.Z, Bike.Rider.RightPeg.Z + 12.0);
	TestEqual(TEXT("the grips stay as far apart"), BikeSpec.Rider.RightGrip.Y - BikeSpec.Rider.LeftGrip.Y, Bike.Rider.RightGrip.Y - Bike.Rider.LeftGrip.Y);

	// A truck with three axles keeps them where they are as shares of the wheelbase.
	FMantleVehicleDimensions TruckLonger = FMantleVehicleShape::DimensionsOf(Truck);
	TruckLonger.Wheelbase *= 1.2;
	const FMantleChassisSpec TruckSpec = FMantleVehicleShape::Reshape(Truck, TruckLonger);
	TestEqual(TEXT("the middle axle keeps its share of the wheelbase"), TruckSpec.Axles[1].Position, Truck.Axles[1].Position);
	TestEqual(TEXT("the middle axle moved with the wheelbase"), TruckSpec.WheelCenter(2).X, TruckLonger.Wheelbase * (0.5 - Truck.Axles[1].Position));

	// Warnings: none for any preset as it is; wheels outside the body say so.
	for (const FString& Id : FMantleVehicleChassisBuilder::PresetIds())
	{
		FMantleChassisSpec Preset;
		if (MantleVehicleShapeTestPrivate::LoadPreset(*this, *Id, Preset))
		{
			TestEqual(*FString::Printf(TEXT("%s as it is has nothing to warn about"), *Id), FMantleVehicleShape::Warnings(Preset).Num(), 0);
		}
	}
	// A handling number the derivation had to hold at a limit is said in plain words: a body 2.5 times the sedan's in every direction weighs more than
	// a vehicle may.
	FMantleVehicleDimensions Huge = Own;
	for (const EMantleVehicleDimension Dimension : { EMantleVehicleDimension::BodyLength, EMantleVehicleDimension::BodyWidth, EMantleVehicleDimension::BodyHeight,
			 EMantleVehicleDimension::GroundClearance })
	{
		Huge = FMantleVehicleShape::Edit(Sedan, Huge, Dimension, 1.0e6, Notes);
	}
	const FMantleChassisSpec HugeSpec = FMantleVehicleShape::Reshape(Sedan, Huge);
	if (TestTrue(TEXT("a huge sedan is shaped"), HugeSpec.Derived.IsSet()))
	{
		TestTrue(TEXT("the derivation listed what it held"), HugeSpec.Derived->Clamped.Num() > 0);
		TestTrue(TEXT("and the window says the weight is held"), FMantleVehicleShape::Warnings(HugeSpec).ContainsByPredicate([](const FText& Warning)
		{
			return Warning.ToString().Contains(TEXT("as heavy as Mantle allows"));
		}));
	}

	FMantleVehicleDimensions ShortBody = Own;
	ShortBody.BodyLength = 150.0;
	TestTrue(TEXT("wheels past the body's ends are warned about"), FMantleVehicleShape::Warnings(FMantleVehicleShape::Reshape(Sedan, ShortBody)).Num() > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleShapeSessionTest,
	"Mantle.Vehicle.Builder.ShapeTypedSizesAreHeldAndSaid",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleShapeSessionTest::RunTest(const FString& Parameters)
{
	FMantleChassisSpec Sedan;
	if (!MantleVehicleShapeTestPrivate::LoadPreset(*this, TEXT("sedan"), Sedan))
	{
		return false;
	}

	// The slider callbacks (a drag, a typed number, Reset) all come to the session, and the spin box's own limits are unset so it snaps nothing
	// before the session sees the number: a size past its range is held and said, not snapped without a word.
	FMantleVehicleShapeSession Session;
	Session.Start(Sedan);
	TestTrue(TEXT("a started session shows the preset's own sizes"), Session.IsStarted() && !Session.IsShaped() && Session.GetHeldNotes().Num() == 0);
	TestTrue(TEXT("it says what the sedan weighs"), Session.GetSummary().ToString().StartsWith(TEXT("Weighs about ")));

	const double Longest = FMantleVehicleShape::SliderRange(Sedan, EMantleVehicleDimension::Wheelbase).Max;
	TestTrue(TEXT("a wheelbase typed far past the end changes the size"), Session.SetDimension(EMantleVehicleDimension::Wheelbase, 99999.0));
	TestEqual(TEXT("to the longest there is"), Session.GetDimensions().Wheelbase, Longest);
	if (TestEqual(TEXT("and says so"), Session.GetHeldNotes().Num(), 1))
	{
		TestTrue(TEXT("in plain words, naming the size"), Session.GetHeldNotes()[0].ToString().Contains(TEXT("Wheelbase can't go above")));
		TestTrue(TEXT("and it is in the warnings under the sliders"), Session.GetWarnings().ToString().Contains(TEXT("Wheelbase can't go above")));
	}
	TestTrue(TEXT("the summary and the spec follow at once"), Session.IsShaped() && Session.GetShaped().Derived.IsSet() && Session.GetShaped().Wheelbase == Longest);

	// The same number again, as a drag's last step and its commit are: nothing changes, and the note stays.
	TestFalse(TEXT("the same typed size again changes nothing"), Session.SetDimension(EMantleVehicleDimension::Wheelbase, 99999.0));
	TestEqual(TEXT("and the note is still there"), Session.GetHeldNotes().Num(), 1);

	// A number in range takes the note away.
	TestTrue(TEXT("a wheelbase in range changes the size"), Session.SetDimension(EMantleVehicleDimension::Wheelbase, 340.0));
	TestEqual(TEXT("to what was typed"), Session.GetDimensions().Wheelbase, 340.0);
	TestEqual(TEXT("with nothing to say"), Session.GetHeldNotes().Num(), 0);
	TestFalse(TEXT("340 again changes nothing"), Session.SetDimension(EMantleVehicleDimension::Wheelbase, 340.0));

	// A number held where it already is says so without calling for a new picture.
	TestTrue(TEXT("a wheelbase typed too short changes the size"), Session.SetDimension(EMantleVehicleDimension::Wheelbase, 5.0));
	TestEqual(TEXT("and says why"), Session.GetHeldNotes().Num(), 1);
	TestFalse(TEXT("1 cm typed on top of it holds it where it is"), Session.SetDimension(EMantleVehicleDimension::Wheelbase, 1.0));
	TestEqual(TEXT("and the note is the same one"), Session.GetHeldNotes().Num(), 1);

	TestTrue(TEXT("Reset changes the sizes back"), Session.Reset());
	TestTrue(TEXT("to the preset's own, hand-tuned, with nothing to say"), !Session.IsShaped() && !Session.GetShaped().Derived.IsSet() && Session.GetHeldNotes().Num() == 0);
	TestFalse(TEXT("Reset again changes nothing"), Session.Reset());

	// A motorcycle has no track slider, and a track typed on it changes nothing.
	FMantleChassisSpec Bike;
	if (MantleVehicleShapeTestPrivate::LoadPreset(*this, TEXT("motorcycle"), Bike))
	{
		FMantleVehicleShapeSession BikeSession;
		BikeSession.Start(Bike);
		TestFalse(TEXT("a motorcycle's track cannot be typed"), BikeSession.SetDimension(EMantleVehicleDimension::Track, 100.0));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleShapeWordsTest,
	"Mantle.Vehicle.Builder.ShapeWordsAreRight",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleShapeWordsTest::RunTest(const FString& Parameters)
{
	// The spring and travel warnings say which way the limit held the number: the springs a body this heavy would want stiffer are held at the
	// stiffest there is, so they cannot be stiff enough.
	const auto Said = [](const TCHAR* Path, const bool bHeldDown) { return FMantleVehicleShape::ClampedSentence(Path, bHeldDown).ToString(); };
	for (const TCHAR* Path : { TEXT("suspension/springRate"), TEXT("suspension/springPreload") })
	{
		TestEqual(TEXT("springs the rule wanted stiffer"), Said(Path, true), FString(TEXT("Its springs can't be made stiff enough for this weight, so it may sag and bounce.")));
		TestEqual(TEXT("springs the rule wanted softer"), Said(Path, false), FString(TEXT("Its springs can't be made soft enough for this light a body, so it may ride hard.")));
	}
	for (const TCHAR* Path : { TEXT("suspension/maxRaise"), TEXT("suspension/maxDrop") })
	{
		TestEqual(TEXT("travel the rule wanted longer"), Said(Path, true), FString(TEXT("Its suspension can't have as much give as these wheels want, so it may bottom out on bumps.")));
		TestEqual(TEXT("travel the rule wanted shorter"), Said(Path, false), FString(TEXT("Its suspension has more give than these small wheels want, so it may feel floaty.")));
	}
	TestEqual(TEXT("a weight held at the most"), Said(TEXT("mass"), true), FString(TEXT("It is as heavy as Mantle allows, so it may feel lighter than its size suggests.")));
	TestTrue(TEXT("a camera held at a limit is not something to worry a creator with"), FMantleVehicleShape::ClampedSentence(TEXT("camera/armLength"), true).IsEmpty());
	TestTrue(TEXT("nor is the center of mass"), FMantleVehicleShape::ClampedSentence(TEXT("centerOfMass/z"), false).IsEmpty());

	// The summary: metric tons over a metric ton, kilograms under it.
	FMantleVehicleSummary Summary;
	Summary.TurningCircleCm = 650.0;
	Summary.TopSpeedKmh = 162.0;
	const auto Describe = [&Summary](const double Kilograms)
	{
		Summary.MassKg = Kilograms;
		return FMantleVehicleShape::Describe(Summary).ToString();
	};
	TestTrue(TEXT("750 kg is said in kilograms"), Describe(750.0).StartsWith(TEXT("Weighs about 750 kg.")));
	TestTrue(TEXT("a weight under 100 kg to the kilogram"), Describe(97.4).StartsWith(TEXT("Weighs about 97 kg.")));
	TestTrue(TEXT("a weight over 100 kg to the nearest ten"), Describe(754.0).StartsWith(TEXT("Weighs about 750 kg.")));
	TestTrue(TEXT("1500 kg is in metric tons"), Describe(1500.0).Contains(TEXT("metric tons. Turns in")));
	TestTrue(TEXT("one metric ton is one metric ton"), Describe(1000.0).StartsWith(TEXT("Weighs about 1 metric ton.")));
	TestTrue(TEXT("the circle is how far across it turns"), Describe(750.0).Contains(TEXT("Turns in a circle about 7 m across.")));
	TestTrue(TEXT("and the speed to the nearest five"), Describe(750.0).EndsWith(TEXT("Top speed about 160 km/h.")));

	// A reason from deeper down loses its own full stops, so a sentence of ours has the only one.
	TestEqual(TEXT("a trailing full stop is cut"), FMantleVehicleShape::PlainReason(TEXT("the body is not valid. ")), FString(TEXT("the body is not valid")));
	TestEqual(TEXT("a reason without one is left alone"), FMantleVehicleShape::PlainReason(TEXT("no wheels")), FString(TEXT("no wheels")));

	// Names for a new vehicle: safe to save under, free of the ones taken.
	const auto Free = [](const TCHAR* Wanted, const TSet<FString>& Taken)
	{
		return FMantleVehicleBuilder::FirstFreeName(Wanted, [&Taken](const FString& Candidate) { return Taken.Contains(Candidate); });
	};
	const TSet<FString> None;
	TestEqual(TEXT("a plain name stays"), Free(TEXT("Sedan"), None), FString(TEXT("Sedan")));
	TestEqual(TEXT("spaces become underscores"), Free(TEXT("My Sedan"), None), FString(TEXT("My_Sedan")));
	TestEqual(TEXT("hyphens too"), Free(TEXT("four-door coupe"), None), FString(TEXT("four_door_coupe")));
	TestEqual(TEXT("a name cannot start with a digit"), Free(TEXT("3 door"), None), FString(TEXT("Vehicle_3_door")));
	TestEqual(TEXT("an empty name is Vehicle"), Free(TEXT(""), None), FString(TEXT("Vehicle")));
	TestEqual(TEXT("so is one of nothing but marks"), Free(TEXT("!!!"), None), FString(TEXT("Vehicle")));
	TestEqual(TEXT("a name that is taken gets _2"), Free(TEXT("Sedan"), TSet<FString>({ TEXT("Sedan") })), FString(TEXT("Sedan_2")));
	TestEqual(TEXT("and _3 when that is taken too"), Free(TEXT("Sedan"), TSet<FString>({ TEXT("Sedan"), TEXT("Sedan_2") })), FString(TEXT("Sedan_3")));
	TestEqual(TEXT("a free name beside a taken one stays"), Free(TEXT("Pickup"), TSet<FString>({ TEXT("Sedan") })), FString(TEXT("Pickup")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleShapeTopHeavyTest,
	"Mantle.Vehicle.Builder.ShapeTopHeavyIsNotFalse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleShapeTopHeavyTest::RunTest(const FString& Parameters)
{
	// A body as tall as it goes, on tracks from 1 m to 3 m: the derivation lowers the center of mass to the track's target and rounds it, so the
	// track just meets the target and may read a hair under it. That is not top-heavy; only a center of mass the floor above the wheels holds up is.
	int32 Checked = 0;
	for (const FString& Id : FMantleVehicleChassisBuilder::PresetIds())
	{
		FMantleChassisSpec Preset;
		if (!MantleVehicleShapeTestPrivate::LoadPreset(*this, *Id, Preset) || Preset.bCenterline || Preset.Constants.Stability <= 0.0)
		{
			continue;
		}
		FMantleVehicleShapeSession Session;
		Session.Start(Preset);
		Session.SetDimension(EMantleVehicleDimension::BodyHeight, 1.0e6);
		Session.SetDimension(EMantleVehicleDimension::GroundClearance, 1.0e6);
		for (double Track = 100.0; Track <= 300.0; Track += 20.0)
		{
			Session.SetDimension(EMantleVehicleDimension::Track, Track);
			const FMantleChassisSpec& Spec = Session.GetShaped();
			const bool bFloorHoldsItUp = Spec.CenterOfMass.Z <= Spec.WheelRadius + 2.0 + 1.0e-3;
			if (!bFloorHoldsItUp)
			{
				++Checked;
				TestFalse(*FString::Printf(TEXT("%s on a %.0f cm track is not called top-heavy"), *Id, Spec.Track), Session.GetWarnings().ToString().Contains(TEXT("top-heavy")));
			}
		}
	}
	TestTrue(TEXT("some tall bodies were checked"), Checked > 0);
	return true;
}

#endif
