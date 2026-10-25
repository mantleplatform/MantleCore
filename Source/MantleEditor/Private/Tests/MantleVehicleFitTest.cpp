// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleFit.h"
#include "MantleVehiclePreset.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// Sizing a creator's vehicle for the Mantle character: the scale from the wheel radius and track width, against the owner's
// Fab pack's measured wheels, and the rules around it (axles, missing signals, clamps, disagreeing signals, plain words).

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVehicleFitTestPrivate
{
	/** A wheel at (X, Y) whose radius is its centre's height Z, as the pack's measurements are given. */
	FMantleVehicleFitWheel W(const double X, const double Y, const double Radius)
	{
		FMantleVehicleFitWheel Wheel;
		Wheel.Centre = FVector(X, Y, Radius);
		Wheel.Radius = Radius;
		return Wheel;
	}

	void Report(FAutomationTestBase& Test, const TCHAR* Name, const FMantleVehicleFit& Fit)
	{
		Test.AddInfo(FString::Printf(TEXT("%s: scale %.3f (wheels %.3f, track %.3f; mean radius %.2f, mean track %.2f)%s%s. %s"),
			Name, Fit.Scale, Fit.RadiusScale, Fit.TrackScale, Fit.MeanRadius, Fit.MeanTrack,
			Fit.bClamped ? TEXT(" clamped") : TEXT(""), Fit.bSignalsDisagree ? TEXT(" disagree") : TEXT(""), *Fit.Reason));
	}

	/** Fits the wheels against a preset and prints the result; Low and High bound the scale unless High is below Low. */
	void CheckFab(FAutomationTestBase& Test, const TCHAR* Name, const TCHAR* PresetId, const TArray<FMantleVehicleFitWheel>& Wheels, const double Low, const double High)
	{
		const FMantleVehiclePreset* Preset = FMantleVehiclePresets::Find(PresetId);
		if (!Test.TestNotNull(FString::Printf(TEXT("%s: preset %s loads"), Name, PresetId), Preset))
		{
			return;
		}
		const FMantleVehicleFit Fit = FMantleVehicleFitter::Compute(*Preset, Wheels);
		Report(Test, Name, Fit);
		if (High >= Low)
		{
			Test.TestTrue(FString::Printf(TEXT("%s: scale %.3f is within [%.2f, %.2f]"), Name, Fit.Scale, Low, High), Fit.Scale >= Low && Fit.Scale <= High);
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleFitFabPackTest,
	"Mantle.Vehicle.Fit.FabPack",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleFitFabPackTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleFitTestPrivate;

	// Wheel centres measured from the owner's Fab pack; the radius is the centre's height.
	CheckFab(*this, TEXT("Vol2 Sedan"), TEXT("sedan"),
		{ W(136.48, -83.38, 31.43), W(137.41, 83.70, 31.43), W(-136.06, 87.17, 31.44), W(-135.54, -87.17, 31.44) }, 0.95, 1.05);
	CheckFab(*this, TEXT("Vol2 SUV"), TEXT("pickup"),
		{ W(144.24, -77.6, 36.2), W(144.24, 77.6, 36.2), W(-137.58, 77.6, 36.2), W(-137.58, -77.6, 36.2) }, 0.95, 1.05);
	CheckFab(*this, TEXT("Vol1 Pickup"), TEXT("pickup"),
		{ W(144.69, -66.24, 30.26), W(144.69, 66.24, 30.26), W(-110.73, 66.24, 30.26), W(-110.73, -66.24, 30.26) }, 1.15, 1.30);
	CheckFab(*this, TEXT("Vol1 Hatchback"), TEXT("sedan"),
		{ W(117.96, -70, 27.07), W(117.96, 70, 27.07), W(-120.56, 70, 27.09), W(-119.53, -66.24, 27.09) }, 1.08, 1.25);
	// The box truck's scale range is not pinned: High below Low only logs the result.
	CheckFab(*this, TEXT("Vol1 Box truck"), TEXT("pickup"),
		{ W(233.77, -89.77, 43.46), W(233.76, 91.96, 43.46), W(-162.55, 85.85, 43.36), W(-162.55, -85.96, 43.36) }, 1.0, 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleFitRulesTest,
	"Mantle.Vehicle.Fit.Rules",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleFitRulesTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleFitTestPrivate;

	const FMantleVehiclePreset* Sedan = FMantleVehiclePresets::Find(TEXT("sedan"));
	if (!TestNotNull(TEXT("the sedan preset loads"), Sedan))
	{
		return false;
	}

	const TArray<FMantleVehicleFitWheel> SedanWheels = { W(136.48, -83.38, 31.43), W(137.41, 83.70, 31.43), W(-136.06, 87.17, 31.44), W(-135.54, -87.17, 31.44) };
	const FMantleVehicleFit SedanFit = FMantleVehicleFitter::Compute(*Sedan, SedanWheels);
	Report(*this, TEXT("sedan"), SedanFit);

	// A stretched limousine keeps its length: the rear axle far back changes nothing.
	const TArray<FMantleVehicleFitWheel> Limousine = { W(136.48, -83.38, 31.43), W(137.41, 83.70, 31.43), W(-400.0, 87.17, 31.44), W(-400.0, -87.17, 31.44) };
	const FMantleVehicleFit LimousineFit = FMantleVehicleFitter::Compute(*Sedan, Limousine);
	Report(*this, TEXT("limousine"), LimousineFit);
	TestEqual(TEXT("a limousine gets its sedan's scale"), LimousineFit.Scale, SedanFit.Scale);

	// A truck with three axles: each axle's track counts.
	const TArray<FMantleVehicleFitWheel> Truck = {
		W(250, -80, 40), W(250, 80, 40), W(-50, -80, 40), W(-50, 80, 40), W(-250, -80, 40), W(-250, 80, 40) };
	const FMantleVehicleFit TruckFit = FMantleVehicleFitter::Compute(*Sedan, Truck);
	Report(*this, TEXT("three-axle truck"), TruckFit);
	TestEqual(TEXT("three axles: mean track 160"), TruckFit.MeanTrack, 160.0, 0.01);
	TestEqual(TEXT("three axles: mean radius 40"), TruckFit.MeanRadius, 40.0, 0.01);
	TestTrue(TEXT("three axles: it has a scale from both signals"), TruckFit.RadiusScale > 0.0 && TruckFit.TrackScale > 0.0);

	// A two-wheeler on the centre line has no track: the radius alone decides.
	const TArray<FMantleVehicleFitWheel> Bike = { W(60, 0, 32), W(-60, 0, 32) };
	const FMantleVehicleFit BikeFit = FMantleVehicleFitter::Compute(*Sedan, Bike);
	Report(*this, TEXT("two-wheeler"), BikeFit);
	TestEqual(TEXT("a two-wheeler has no track scale"), BikeFit.TrackScale, 0.0);
	TestTrue(TEXT("a two-wheeler is sized from its radius"), BikeFit.RadiusScale > 0.0);
	TestEqual(TEXT("a two-wheeler's size is its radius's"), BikeFit.Scale, 1.0);

	// A tiny model asks for more than the preset allows.
	const TArray<FMantleVehicleFitWheel> Tiny = { W(1.4, -0.8, 3), W(1.4, 0.8, 3), W(-1.4, -0.8, 3), W(-1.4, 0.8, 3) };
	const FMantleVehicleFit TinyFit = FMantleVehicleFitter::Compute(*Sedan, Tiny);
	Report(*this, TEXT("tiny model"), TinyFit);
	TestTrue(TEXT("a tiny model is clamped"), TinyFit.bClamped);
	TestEqual(TEXT("a tiny model gets the largest size"), TinyFit.Scale, Sedan->FitMaxScale, 0.001);

	// A model modelled a hundred times too big.
	const TArray<FMantleVehicleFitWheel> Huge = { W(13648, -8338, 3143), W(13741, 8370, 3143), W(-13606, 8717, 3144), W(-13554, -8717, 3144) };
	const FMantleVehicleFit HugeFit = FMantleVehicleFitter::Compute(*Sedan, Huge);
	Report(*this, TEXT("a hundred times too big"), HugeFit);
	TestTrue(TEXT("a model a hundred times too big is clamped"), HugeFit.bClamped);
	TestEqual(TEXT("a model a hundred times too big gets the smallest size"), HugeFit.Scale, Sedan->FitMinScale, 0.001);

	// Wheels that suggest a size and a track that suggests another.
	const TArray<FMantleVehicleFitWheel> Odd = { W(130, -165, 32), W(130, 165, 32), W(-130, -165, 32), W(-130, 165, 32) };
	const FMantleVehicleFit OddFit = FMantleVehicleFitter::Compute(*Sedan, Odd);
	Report(*this, TEXT("disagreeing signals"), OddFit);
	TestTrue(TEXT("disagreeing signals are flagged"), OddFit.bSignalsDisagree);
	TestFalse(TEXT("agreeing signals are not flagged"), SedanFit.bSignalsDisagree);

	// No wheels at all: left as modelled, and says why.
	const FMantleVehicleFit NoneFit = FMantleVehicleFitter::Compute(*Sedan, TArray<FMantleVehicleFitWheel>());
	Report(*this, TEXT("no wheels"), NoneFit);
	TestEqual(TEXT("no wheels leaves the size alone"), NoneFit.Scale, 1.0);
	TestFalse(TEXT("no wheels still explains itself"), NoneFit.Reason.IsEmpty());

	// A model centred off the origin has the same size: the sides come from its own centre line.
	TArray<FMantleVehicleFitWheel> Shifted = SedanWheels;
	for (FMantleVehicleFitWheel& Wheel : Shifted)
	{
		Wheel.Centre.Y += 40.0;
	}
	const FMantleVehicleFit ShiftedFit = FMantleVehicleFitter::Compute(*Sedan, Shifted);
	Report(*this, TEXT("sedan shifted 40 sideways"), ShiftedFit);
	TestEqual(TEXT("a shifted sedan gets the same scale"), ShiftedFit.Scale, SedanFit.Scale, 0.001);
	TestEqual(TEXT("a shifted sedan has the same track"), ShiftedFit.MeanTrack, SedanFit.MeanTrack, 0.01);

	// A clamp that lands exactly on 1 is still a clamp: it never claims the model is already the right size.
	FMantleVehiclePreset Pinned;
	Pinned.Id = TEXT("pinned");
	Pinned.Name = TEXT("Pinned car");
	Pinned.FitWheelRadius = 62.86;
	Pinned.FitMinScale = 1.0;
	Pinned.FitMaxScale = 1.0;
	const FMantleVehicleFit PinnedFit = FMantleVehicleFitter::Compute(Pinned, SedanWheels);
	Report(*this, TEXT("clamped to 1"), PinnedFit);
	TestTrue(TEXT("the pinned preset clamps"), PinnedFit.bClamped);
	TestEqual(TEXT("the pinned preset lands on 1"), PinnedFit.Scale, 1.0);
	TestFalse(TEXT("a clamped result is never \"already the right size\""), PinnedFit.Reason.Contains(TEXT("Already the right size")));
	TestTrue(TEXT("a clamped result names the direction asked for"), PinnedFit.Reason.Contains(TEXT("enlarged")));
	TestTrue(TEXT("a clamped result says its wheels are smaller than typical"), PinnedFit.Reason.Contains(TEXT("smaller than a typical Pinned car")));

	// What a creator reads has no engineering words in it.
	for (const FMantleVehicleFit* Fit : { &SedanFit, &LimousineFit, &TruckFit, &BikeFit, &TinyFit, &HugeFit, &OddFit, &NoneFit, &ShiftedFit, &PinnedFit })
	{
		TestFalse(FString::Printf(TEXT("no \"cm\" in: %s"), *Fit->Reason), Fit->Reason.Contains(TEXT("cm"), ESearchCase::IgnoreCase));
		TestFalse(FString::Printf(TEXT("no \"bone\" in: %s"), *Fit->Reason), Fit->Reason.Contains(TEXT("bone"), ESearchCase::IgnoreCase));
		TestFalse(FString::Printf(TEXT("no \"Unreal\" in: %s"), *Fit->Reason), Fit->Reason.Contains(TEXT("Unreal"), ESearchCase::IgnoreCase));
		TestFalse(TEXT("a reason is never empty"), Fit->Reason.IsEmpty());
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
