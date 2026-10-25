// Copyright Joshua Eger. All Rights Reserved.
#include "MantleChassisSpec.h"

#include "MantleChassisDerive.h"
#include "MantleVehicleChassisBuilder.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// A hovercraft's chassis: four pads at the hull's corners (the layout's wheels, never drawn), a spring rate that rests the weight at the hover
// height, a fan behind the hull and a fin on its top, a grey body of blocks on the root, and a push, spring and rudder that follow the size so a resized
// hovercraft floats at its own hover height.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleChassisHovercraftTestPrivate
{
	/** The world's gravity, m/s^2, and a pad's stiffness per spring rate unit, N/m (measured; schema/validate.py's). */
	constexpr double Gravity = 9.8;
	constexpr double StiffnessPerRate = 222.0;

	bool LoadHovercraft(FAutomationTestBase& Test, FMantleChassisSpec& OutSpec, FString* OutText = nullptr)
	{
		FString Text;
		FString Error;
		const bool bOk = FMantleVehicleChassisBuilder::LoadPresetText(TEXT("hovercraft"), Text, Error) && FMantleChassisSpec::Parse(Text, OutSpec, Error);
		Test.TestTrue(*FString::Printf(TEXT("the hovercraft preset reads (%s)"), *Error), bOk);
		if (OutText)
		{
			*OutText = Text;
		}
		return bOk;
	}

	/** Where a pad of Rate rests, cm above the ground, under a quarter of Mass: its travel less a quarter of the weight over its stiffness. */
	double RestHeight(const double Travel, const double Rate, const double Mass)
	{
		return Travel - 100.0 * (Mass * Gravity / 4.0) / FMath::Max(Rate * StiffnessPerRate, UE_SMALL_NUMBER);
	}

	bool NearCm(const FVector& A, const FVector& B)
	{
		return A.Equals(B, 1.0e-3);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleChassisHovercraftSpecTest,
	"Mantle.Vehicle.Chassis.HovercraftSpecHasPadsAndAFan",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleChassisHovercraftSpecTest::RunTest(const FString& Parameters)
{
	using namespace MantleChassisHovercraftTestPrivate;
	FMantleChassisSpec Hover;
	if (!LoadHovercraft(*this, Hover))
	{
		return false;
	}
	const FMantleChassisHovercraft& Craft = Hover.Hovercraft;
	TestTrue(TEXT("the hovercraft type with its block"), Hover.Type == EMantleChassisType::Hovercraft && Craft.bSet);
	TestTrue(TEXT("driven from a car's seat, with a passenger"), Hover.bSteeringWheel && Hover.DriverSeat.Equals(TEXT("car"), ESearchCase::CaseSensitive) && Hover.bPassenger);

	// Four pads at 0.8 of the hull's length and width, on its underside: the hover height.
	if (TestEqual(TEXT("four pads (two axles, a pair each)"), Hover.WheelCount(), 4))
	{
		for (int32 Index = 0; Index < 4; ++Index)
		{
			const FVector Pad = Hover.PadLocation(Index);
			TestTrue(*FString::Printf(TEXT("pad %d at a corner, on the hull's underside"), Index),
				FMath::IsNearlyEqual(FMath::Abs(Pad.X), 0.4 * Hover.Length) && FMath::IsNearlyEqual(FMath::Abs(Pad.Y), 0.4 * Hover.Width) && Pad.Z == Hover.Clearance);
		}
	}
	// The spring rests a quarter of the weight at the hover height (validate.py's rule).
	const double Rest = RestHeight(Craft.PadTravel, Craft.PadSpringRate, Hover.Archetype.Mass);
	TestTrue(*FString::Printf(TEXT("a pad of %.2f rests at %.2f cm, the %.0f cm hover height"), Craft.PadSpringRate, Rest, Hover.Clearance),
		FMath::Abs(Rest - Hover.Clearance) <= 0.01 * (Craft.PadTravel - Hover.Clearance));
	TestTrue(TEXT("the pads reach below the hover height"), Craft.PadTravel > Hover.Clearance);

	// The fan's pod is behind the hull, its push between a fifth and a half of the weight; the fin stands on the hull's top, behind the seat.
	const FMantleChassisBlock Hull = Hover.BodyBlock();
	const FMantleChassisBlock Pod = Hover.FanPodBlock();
	TestTrue(TEXT("the pod's front is the hull's rear end"), FMath::IsNearlyEqual(Pod.Max.X, Hull.Min.X));
	TestTrue(TEXT("the pod is inside the hull's height"), Pod.Min.Z >= Hull.Min.Z && Pod.Max.Z <= Hull.Max.Z);
	TestTrue(TEXT("the fan pushes from the pod's middle"), NearCm(Hover.FanLocation(), 0.5 * (Pod.Min + Pod.Max)));
	const double ThrustShare = Craft.FanThrustNewtons / (Hover.Archetype.Mass * Gravity);
	TestTrue(*FString::Printf(TEXT("the fan pushes %.3f of the weight"), ThrustShare), ThrustShare >= 0.2 && ThrustShare <= 0.5);
	const FMantleChassisBlock Fin = Hover.HoverFinBlock();
	TestTrue(TEXT("the fin stands on the hull's top, on its length"), Fin.Min.Z == Hull.Max.Z && Fin.Min.X >= Hull.Min.X && Fin.Max.X <= Hull.Max.X);
	TestTrue(TEXT("behind the seat"), Fin.Max.X < Hover.Seat.X);
	TestTrue(TEXT("the rudder acts at the fin's middle"), NearCm(Hover.HoverRudderLocation(), FVector(Craft.FinX, 0.0, 0.5 * (Fin.Min.Z + Fin.Max.Z))));
	const FMantleChassisBlock Skirt = Hover.SkirtBlock();
	TestTrue(TEXT("the skirt is under the hull, wider than it, off the ground"),
		Skirt.Max.Z == Hull.Min.Z && Skirt.Min.Z > 0.0 && Skirt.Min.X < Hull.Min.X && Skirt.Max.Y > Hull.Max.Y);

	// Its push, spring and rudder are its handling's own; nothing else has them.
	TestEqual(TEXT("the archetype's push is the fan's"), Hover.Archetype.HoverThrustNewtons, Craft.FanThrustNewtons);
	TestEqual(TEXT("the archetype's spring is the pads'"), Hover.Archetype.PadSpringRate, Craft.PadSpringRate);
	TestEqual(TEXT("the archetype's rudder is the fin's"), Hover.Archetype.HoverFinAreaM2, Craft.FinAreaM2);
	for (const FString& Id : FMantleVehicleChassisBuilder::PresetIds())
	{
		FMantleChassisSpec Other;
		FString Text;
		FString Error;
		if (Id == TEXT("hovercraft") || !FMantleVehicleChassisBuilder::LoadPresetText(Id, Text, Error) || !FMantleChassisSpec::Parse(Text, Other, Error))
		{
			continue;
		}
		TestFalse(*FString::Printf(TEXT("%s has no hovercraft block"), *Id), Other.Hovercraft.bSet);
		const FMantleChassisHandling Derived = FMantleChassisDerive::Derive(Other);
		TestTrue(*FString::Printf(TEXT("%s derives no push, pad spring or rudder"), *Id), Derived.HoverThrustNewtons == 0.0 && Derived.PadSpringRate == 0.0 && Derived.HoverFinAreaM2 == 0.0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleChassisHovercraftBodyTest,
	"Mantle.Vehicle.Chassis.HovercraftBodyIsBlocksOnTheRoot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleChassisHovercraftBodyTest::RunTest(const FString& Parameters)
{
	using namespace MantleChassisHovercraftTestPrivate;
	FMantleChassisSpec Hover;
	if (!LoadHovercraft(*this, Hover))
	{
		return false;
	}
	FMantleVehicleAsset Asset;
	FString Error;
	if (!TestTrue(*FString::Printf(TEXT("it makes an asset (%s)"), *Error), FMantleVehicleChassisBuilder::MakeAsset(Hover, Asset, Error)))
	{
		return false;
	}
	// A root and the four pads' points, which its reader calls wheels: a steered pair in front and a driven pair behind, as the reader needs.
	TestEqual(TEXT("a root and four wheel bones"), Asset.Body.Bones.Num(), 5);
	if (TestEqual(TEXT("four wheels in the document"), Asset.Desc.Wheels.Num(), 4))
	{
		TestTrue(TEXT("a steered one and a driven one"), Asset.Desc.Wheels[0].bSteering && Asset.Desc.Wheels[3].bDriven);
	}
	// None of the body is on a wheel bone: the hull, the skirt, the pod and the fin are four boxes on the root.
	if (TestEqual(TEXT("one LOD"), Asset.Body.LODs.Num(), 1))
	{
		const FMantleSkinnedMeshLOD& LOD = Asset.Body.LODs[0];
		TestEqual(TEXT("four boxes' vertices"), LOD.Positions.Num(), 4 * 24);
		bool bAllOnRoot = true;
		for (int32 Vertex = 0; Vertex < LOD.Positions.Num(); ++Vertex)
		{
			bAllOnRoot = bAllOnRoot && LOD.InfluenceBones[Vertex * LOD.NumInfluences] == 0;
		}
		TestTrue(TEXT("every vertex is on the root: no wheel is drawn"), bAllOnRoot);
	}
	// Only the hull collides.
	if (TestEqual(TEXT("one collision box"), Asset.Desc.Collision.Num(), 1))
	{
		const FMantleChassisBlock Hull = Hover.BodyBlock();
		TestTrue(TEXT("the hull's"), NearCm(Asset.Desc.Collision[0].Center, 0.5 * (Hull.Min + Hull.Max)) && NearCm(Asset.Desc.Collision[0].Extent, 0.5 * (Hull.Max - Hull.Min)));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleChassisHovercraftPartsTest,
	"Mantle.Vehicle.Chassis.HovercraftPresetNeedsItsParts",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleChassisHovercraftPartsTest::RunTest(const FString& Parameters)
{
	using namespace MantleChassisHovercraftTestPrivate;
	FMantleChassisSpec Hover;
	FString Text;
	if (!LoadHovercraft(*this, Hover, &Text))
	{
		return false;
	}
	FMantleChassisSpec Spec;
	FString Error;
	// A hovercraft with no hovercraft block, a car with one, pads with no spring, a fan with no push and reverse vanes stronger than the fan are not presets.
	FString Cut = Text.Replace(TEXT("\"hovercraft\": {"), TEXT("\"aircushion\": {"));
	TestFalse(TEXT("a hovercraft with no hovercraft block is refused"), FMantleChassisSpec::Parse(Cut, Spec, Error));
	TestTrue(*FString::Printf(TEXT("and says so (%s)"), *Error), Error.Contains(TEXT("hovercraft")));
	Cut = Text.Replace(TEXT("\"type\": \"hovercraft\""), TEXT("\"type\": \"car\""));
	TestFalse(TEXT("a car with a hovercraft block is refused"), FMantleChassisSpec::Parse(Cut, Spec, Error));
	Cut = Text.Replace(TEXT("\"springRate\": 33.1"), TEXT("\"springRates\": 33.1"));
	TestFalse(TEXT("pads with no spring are refused"), FMantleChassisSpec::Parse(Cut, Spec, Error));
	TestTrue(*FString::Printf(TEXT("and says which (%s)"), *Error), Error.Contains(TEXT("pads.springRate")));
	Cut = Text.Replace(TEXT("\"thrust\": 2000"), TEXT("\"thrusts\": 2000"));
	TestFalse(TEXT("a fan with no push is refused"), FMantleChassisSpec::Parse(Cut, Spec, Error));
	Cut = Text.Replace(TEXT("\"reverseShare\": 0.5"), TEXT("\"reverseShare\": 1.5"));
	TestFalse(TEXT("reverse vanes stronger than the fan are refused"), FMantleChassisSpec::Parse(Cut, Spec, Error));
	EMantleChassisType Type = EMantleChassisType::Car;
	TestTrue(TEXT("the type name reads back"), FMantleChassisSpec::ParseType(TEXT("hovercraft"), Type) && Type == EMantleChassisType::Hovercraft
		&& FString(FMantleChassisSpec::TypeName(EMantleChassisType::Hovercraft)).Equals(TEXT("hovercraft"), ESearchCase::CaseSensitive));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleChassisHovercraftScaledTest,
	"Mantle.Vehicle.Chassis.HovercraftScaledKeepsItsHoverHeight",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleChassisHovercraftScaledTest::RunTest(const FString& Parameters)
{
	using namespace MantleChassisHovercraftTestPrivate;
	FMantleChassisSpec Hover;
	if (!LoadHovercraft(*this, Hover))
	{
		return false;
	}
	const FMantleChassisHovercraft& Own = Hover.Hovercraft;
	const double PushPerKg = Own.FanThrustNewtons / Hover.Archetype.Mass;
	const double FinPerPlanform = Own.FinAreaM2 / (Own.FinHeight * Own.FinChord);
	// At the preset's own size the rules give back its own numbers.
	const FMantleChassisHandling Itself = FMantleChassisDerive::Derive(Hover);
	TestTrue(TEXT("the archetype derives its own push, spring and rudder"), Itself.HoverThrustNewtons == Own.FanThrustNewtons && Itself.PadSpringRate == Own.PadSpringRate
		&& Itself.HoverFinAreaM2 == Own.FinAreaM2);
	// Resized, it floats at its own hover height (the pads' travel and the clearance grow with it, the spring follows the mass and how far the pads come down),
	// its fan pushes the same share of its weight, and its rudder follows its fin.
	for (const double Factor : { 0.5, 0.7, 1.5, 2.0 })
	{
		const FMantleChassisSpec Sized = FMantleChassisSpec::Scaled(Hover, Factor);
		if (!TestTrue(*FString::Printf(TEXT("x%g is derived"), Factor), Sized.Derived.IsSet()))
		{
			continue;
		}
		const FMantleChassisHandling& Handling = Sized.Derived.GetValue();
		TestTrue(*FString::Printf(TEXT("x%g: the pads reach and the hover height grow with it"), Factor),
			FMath::IsNearlyEqual(Sized.Hovercraft.PadTravel, Factor * Own.PadTravel) && FMath::IsNearlyEqual(Sized.Clearance, Factor * Hover.Clearance));
		const double Rest = RestHeight(Sized.Hovercraft.PadTravel, Handling.PadSpringRate, Handling.Mass);
		const double OwnRest = RestHeight(Own.PadTravel, Own.PadSpringRate, Hover.Archetype.Mass);
		TestTrue(*FString::Printf(TEXT("x%g: a pad of %.3f under %.1f kg rests at %.2f cm, the preset's %.2f x %g"), Factor, Handling.PadSpringRate, Handling.Mass, Rest, OwnRest, Factor),
			FMath::IsNearlyEqual(Rest, Factor * OwnRest, 1.0e-3 * Factor * OwnRest));
		TestTrue(*FString::Printf(TEXT("x%g: the fan pushes the same share of the weight"), Factor),
			FMath::IsNearlyEqual(Handling.HoverThrustNewtons / Handling.Mass, PushPerKg, 1.0e-3 * PushPerKg));
		TestTrue(*FString::Printf(TEXT("x%g: the rudder follows the fin"), Factor),
			FMath::IsNearlyEqual(Handling.HoverFinAreaM2, FinPerPlanform * Sized.Hovercraft.FinHeight * Sized.Hovercraft.FinChord, 1.0e-3 * Handling.HoverFinAreaM2));
		TestTrue(*FString::Printf(TEXT("x%g: the fan's pod stays at the hull's rear end"), Factor), FMath::IsNearlyEqual(Sized.FanPodBlock().Max.X, Sized.BodyBlock().Min.X, 1.0e-6));
		TestEqual(*FString::Printf(TEXT("x%g: the damping is accelerations, which do not scale"), Factor), Sized.Hovercraft.MaxLateralAccel, Own.MaxLateralAccel);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
