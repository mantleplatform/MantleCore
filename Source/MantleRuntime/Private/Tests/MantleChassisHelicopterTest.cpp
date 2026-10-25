// Copyright Joshua Eger. All Rights Reserved.
#include "MantleChassisSpec.h"

#include "MantleChassisDerive.h"
#include "MantleVehicleChassisBuilder.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// A helicopter's chassis: two skids of hidden wheels, a main rotor whose hub stands over the centre of mass, a tail boom whose rotor clears the
// main disc, a grey body of blocks with each rotor's blades on its own bone, and a rotor thrust that follows the mass so the hover collective never moves.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleChassisHelicopterTestPrivate
{
	/** Gravity, m/s^2 (the same value schema/validate.py uses). */
	constexpr double Gravity = 9.8;

	bool LoadHelicopter(FAutomationTestBase& Test, FMantleChassisSpec& OutSpec, FString* OutText = nullptr)
	{
		FString Text;
		FString Error;
		const bool bOk = FMantleVehicleChassisBuilder::LoadPresetText(TEXT("helicopter"), Text, Error) && FMantleChassisSpec::Parse(Text, OutSpec, Error);
		Test.TestTrue(*FString::Printf(TEXT("the helicopter preset reads (%s)"), *Error), bOk);
		if (OutText)
		{
			*OutText = Text;
		}
		return bOk;
	}

	int32 BoneIndex(const FMantleVehicleAsset& Asset, const FName Name)
	{
		return Asset.Body.Bones.IndexOfByPredicate([Name](const FMantleSkinnedMeshBone& Bone) { return Bone.Name == Name; });
	}

	/** How many vertices are skinned wholly (in their first slot, as the chassis builder skins them) to Bone, and their bounds. */
	int32 VerticesOfBone(const FMantleSkinnedMeshLOD& LOD, const int32 Bone, FVector& OutMin, FVector& OutMax)
	{
		OutMin = FVector(TNumericLimits<double>::Max());
		OutMax = FVector(-TNumericLimits<double>::Max());
		int32 Count = 0;
		for (int32 Vertex = 0; Vertex < LOD.Positions.Num(); ++Vertex)
		{
			if (LOD.InfluenceBones[Vertex * LOD.NumInfluences] != Bone)
			{
				continue;
			}
			const FVector Point(LOD.Positions[Vertex]);
			OutMin = FVector(FMath::Min(OutMin.X, Point.X), FMath::Min(OutMin.Y, Point.Y), FMath::Min(OutMin.Z, Point.Z));
			OutMax = FVector(FMath::Max(OutMax.X, Point.X), FMath::Max(OutMax.Y, Point.Y), FMath::Max(OutMax.Z, Point.Z));
			++Count;
		}
		return Count;
	}

	bool Near(const FVector& A, const FVector& B)
	{
		return A.Equals(B, 1.0e-3);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleChassisHelicopterSpecTest,
	"Mantle.Vehicle.Chassis.HelicopterSpecHasSkidsAndARotor",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleChassisHelicopterSpecTest::RunTest(const FString& Parameters)
{
	using namespace MantleChassisHelicopterTestPrivate;
	FMantleChassisSpec Heli;
	if (!LoadHelicopter(*this, Heli))
	{
		return false;
	}
	TestTrue(TEXT("its type is a helicopter's"), Heli.Type == EMantleChassisType::Helicopter);
	TestEqual(TEXT("and spells so"), FString(FMantleChassisSpec::TypeName(Heli.Type)), FString(TEXT("helicopter")));
	TestTrue(TEXT("it has a helicopter block and no airframe"), Heli.Helicopter.bSet && !Heli.Aircraft.bSet);
	TestEqual(TEXT("a pilot flies it"), Heli.DriverSeat, FString(TEXT("pilot")));
	TestFalse(TEXT("with no steering wheel"), Heli.bSteeringWheel);

	// Two skids: two axles of a pair each, the skids' ends under the cabin's corners.
	if (TestEqual(TEXT("four skid wheels"), Heli.WheelCount(), 4))
	{
		const FName Bones[] = { TEXT("wheel_fl"), TEXT("wheel_fr"), TEXT("wheel_rl"), TEXT("wheel_rr") };
		const FVector Centres[] = { FVector(120.0, -110.0, 20.0), FVector(120.0, 110.0, 20.0), FVector(-120.0, -110.0, 20.0), FVector(-120.0, 110.0, 20.0) };
		for (int32 Index = 0; Index < 4; ++Index)
		{
			TestEqual(*FString::Printf(TEXT("skid wheel %d's bone"), Index), Heli.WheelBone(Index), Bones[Index]);
			TestTrue(*FString::Printf(TEXT("skid wheel %d's centre"), Index), Near(Heli.WheelCentre(Index), Centres[Index]));
		}
	}
	TestTrue(TEXT("every skid wheel is braked by its axle"), Heli.Axles.Num() == 2 && Heli.Axles[0].bHandbrake && Heli.Axles[1].bHandbrake);

	// The main rotor's hub is over the centre of mass, so its lift turns nothing while the helicopter is level, and well above the pilot's seat.
	const FVector Hub = Heli.MainRotorHub();
	TestTrue(TEXT("the hub is over the centre of mass"), FMath::IsNearlyEqual(Hub.X, Heli.CenterOfMass.X) && Hub.Y == 0.0);
	TestTrue(TEXT("at the rotor's height"), FMath::IsNearlyEqual(Hub.Z, 350.0));
	TestTrue(TEXT("a metre above the seat"), Hub.Z >= Heli.Seat.Z + 100.0);
	TestTrue(TEXT("on a mast from the cabin's top"), Heli.MastBlock().Min.Z == Heli.BodyTop() && Heli.MastBlock().Max.Z <= Heli.MainRotorBladeBlock(0).Min.Z + 1.0e-6);

	// The tail rotor turns beside the boom's end, clear of the main disc.
	const FVector Tail = Heli.TailRotorHub();
	TestTrue(TEXT("the tail rotor's hub is beside the boom's end, on its left"), Near(Tail, FVector(-655.0, -30.0, 215.0)));
	TestTrue(TEXT("and clear of the main disc"), FMath::Abs(Tail.X - Hub.X) > 0.5 * (Heli.Helicopter.RotorDiameter + Heli.Helicopter.TailRotorDiameter));
	const FMantleChassisBlock Boom = Heli.TailBoomBlock();
	TestTrue(TEXT("the boom runs back from the cabin's rear end"), FMath::IsNearlyEqual(Boom.Max.X, Heli.BodyBlock().Min.X)
		&& FMath::IsNearlyEqual(Boom.Max.X - Boom.Min.X, Heli.Helicopter.BoomLength));
	TestTrue(TEXT("the fin stands on the boom's end"), FMath::IsNearlyEqual(Heli.TailFinBlock().Min.X, Boom.Min.X) && FMath::IsNearlyEqual(Heli.TailFinBlock().Min.Z, Boom.Max.Z));

	// Each skid runs along its side's wheels on the ground, under the cabin.
	for (const bool bRight : { false, true })
	{
		const FMantleChassisBlock Skid = Heli.SkidBlock(bRight);
		const double Y = bRight ? 110.0 : -110.0;
		TestTrue(*FString::Printf(TEXT("the %s skid runs past both its wheels"), bRight ? TEXT("right") : TEXT("left")),
			Skid.Min.X < -120.0 && Skid.Max.X > 120.0 && Skid.Min.Y < Y && Skid.Max.Y > Y);
		TestTrue(*FString::Printf(TEXT("the %s skid is just off the ground, below the wheel centres"), bRight ? TEXT("right") : TEXT("left")),
			Skid.Min.Z > 0.0 && Skid.Max.Z < Heli.WheelRadius);
	}

	// The rotor holds 1.6 times the weight, so the collective that holds the weight is its inverse.
	const double Weight = Heli.Archetype.Mass * Gravity;
	TestTrue(TEXT("the rotor's thrust is the archetype's"), FMath::IsNearlyEqual(Heli.Archetype.RotorThrustNewtons, Heli.Helicopter.RotorThrustNewtons));
	TestTrue(TEXT("1.6 times the weight"), FMath::IsNearlyEqual(Heli.Helicopter.RotorThrustNewtons / Weight, 1.6, 0.05));
	TestTrue(*FString::Printf(TEXT("the hover collective %.4f is the weight over the thrust"), Heli.Helicopter.HoverCollective),
		FMath::IsNearlyEqual(Heli.Helicopter.HoverCollective, Weight / Heli.Helicopter.RotorThrustNewtons, 0.005));

	// Nothing else has a helicopter block or a rotor thrust.
	for (const FString& Id : FMantleVehicleChassisBuilder::PresetIds())
	{
		FMantleChassisSpec Other;
		FString Text;
		FString Error;
		if (Id == TEXT("helicopter") || !FMantleVehicleChassisBuilder::LoadPresetText(Id, Text, Error) || !FMantleChassisSpec::Parse(Text, Other, Error))
		{
			continue;
		}
		TestFalse(*FString::Printf(TEXT("%s has no helicopter block"), *Id), Other.Helicopter.bSet);
		TestEqual(*FString::Printf(TEXT("%s has no rotor thrust"), *Id), Other.Archetype.RotorThrustNewtons, 0.0);
		TestEqual(*FString::Printf(TEXT("%s derives none"), *Id), FMantleChassisDerive::Derive(Other).RotorThrustNewtons, 0.0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleChassisHelicopterBodyTest,
	"Mantle.Vehicle.Chassis.HelicopterBodyIsBlocksOnRotorBones",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleChassisHelicopterBodyTest::RunTest(const FString& Parameters)
{
	using namespace MantleChassisHelicopterTestPrivate;
	FMantleChassisSpec Heli;
	FString Error;
	FMantleVehicleAsset Asset;
	if (!LoadHelicopter(*this, Heli) || !TestTrue(*FString::Printf(TEXT("the helicopter makes an asset (%s)"), *Error), FMantleVehicleChassisBuilder::MakeAsset(Heli, Asset, Error)))
	{
		return false;
	}

	// The bones: the root, the four skid wheels, then each rotor's, every one a child of the root at its hub, unturned.
	const FName Names[] = { FMantleVehicleChassisBuilder::RootBoneName(), TEXT("wheel_fl"), TEXT("wheel_fr"), TEXT("wheel_rl"), TEXT("wheel_rr"), TEXT("main_rotor"),
		TEXT("tail_rotor") };
	if (!TestEqual(TEXT("seven bones"), Asset.Body.Bones.Num(), static_cast<int32>(UE_ARRAY_COUNT(Names))))
	{
		return false;
	}
	for (int32 Index = 0; Index < Asset.Body.Bones.Num(); ++Index)
	{
		TestEqual(*FString::Printf(TEXT("bone %d"), Index), Asset.Body.Bones[Index].Name, Names[Index]);
		TestEqual(*FString::Printf(TEXT("bone %d is a child of the root"), Index), Asset.Body.Bones[Index].ParentIndex, Index == 0 ? INDEX_NONE : 0);
		TestTrue(*FString::Printf(TEXT("bone %d is not turned"), Index), Asset.Body.ReferencePose[Index].GetRotation().IsIdentity(1.0e-6));
	}
	TestEqual(TEXT("the main rotor's bone name is the builder's"), FMantleVehicleChassisBuilder::MainRotorBoneName(), FName(TEXT("main_rotor")));
	TestEqual(TEXT("and the tail rotor's"), FMantleVehicleChassisBuilder::TailRotorBoneName(), FName(TEXT("tail_rotor")));
	const int32 MainBone = BoneIndex(Asset, FMantleVehicleChassisBuilder::MainRotorBoneName());
	const int32 TailBone = BoneIndex(Asset, FMantleVehicleChassisBuilder::TailRotorBoneName());
	TestTrue(TEXT("the main rotor's bone is at its hub"), Near(Asset.Body.ReferencePose[MainBone].GetLocation(), Heli.MainRotorHub()));
	TestTrue(TEXT("the tail rotor's at its"), Near(Asset.Body.ReferencePose[TailBone].GetLocation(), Heli.TailRotorHub()));

	// The skin: each rotor's two blades are on its bone, so spinning the bone spins the blades and nothing else; no wheel is drawn.
	if (!TestEqual(TEXT("one LOD"), Asset.Body.LODs.Num(), 1))
	{
		return false;
	}
	const FMantleSkinnedMeshLOD& LOD = Asset.Body.LODs[0];
	FVector Min;
	FVector Max;
	TestEqual(TEXT("the main rotor's two blades are on its bone"), VerticesOfBone(LOD, MainBone, Min, Max), 48);
	const double Radius = 0.5 * Heli.Helicopter.RotorDiameter;
	TestTrue(TEXT("and reach the disc's edge both ways"), Near(Min, Heli.MainRotorHub() - FVector(Radius, Radius, 3.0)) && Near(Max, Heli.MainRotorHub() + FVector(Radius, Radius, 3.0)));
	TestEqual(TEXT("the tail rotor's two blades are on its bone"), VerticesOfBone(LOD, TailBone, Min, Max), 48);
	const double TailRadius = 0.5 * Heli.Helicopter.TailRotorDiameter;
	TestTrue(TEXT("and turn in the XZ plane"), FMath::IsNearlyEqual(Max.X - Min.X, 2.0 * TailRadius, 1.0e-3) && FMath::IsNearlyEqual(Max.Z - Min.Z, 2.0 * TailRadius, 1.0e-3)
		&& Max.Y - Min.Y < 10.0);
	for (int32 Wheel = 1; Wheel <= 4; ++Wheel)
	{
		TestEqual(*FString::Printf(TEXT("wheel bone %d carries no geometry: the skids stand for the wheels"), Wheel), VerticesOfBone(LOD, Wheel, Min, Max), 0);
	}
	TestEqual(TEXT("sixteen boxes in all"), LOD.Positions.Num(), 16 * 24);
	FString Why;
	TestTrue(*FString::Printf(TEXT("the body validates (%s)"), *Why), Asset.Body.Validate(Why));

	// The document's collision: the cabin, the boom and the fin, as the blocks are; the rotors and the skids collide with nothing.
	if (TestEqual(TEXT("three collision boxes"), Asset.Desc.Collision.Num(), 3))
	{
		const FMantleChassisBlock Blocks[] = { Heli.BodyBlock(), Heli.TailBoomBlock(), Heli.TailFinBlock() };
		for (int32 Index = 0; Index < 3; ++Index)
		{
			const FMantleVehicleBoxDesc& Box = Asset.Desc.Collision[Index];
			TestTrue(*FString::Printf(TEXT("collision box %d is block %d"), Index, Index),
				Near(Box.Center - Box.Extent, Blocks[Index].Min) && Near(Box.Center + Box.Extent, Blocks[Index].Max));
		}
	}
	TestEqual(TEXT("four wheels, the skids' ends"), Asset.Desc.Wheels.Num(), 4);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleChassisHelicopterPartsTest,
	"Mantle.Vehicle.Chassis.HelicopterPresetNeedsItsParts",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleChassisHelicopterPartsTest::RunTest(const FString& Parameters)
{
	using namespace MantleChassisHelicopterTestPrivate;
	FMantleChassisSpec Heli;
	FString Text;
	if (!LoadHelicopter(*this, Heli, &Text))
	{
		return false;
	}
	FMantleChassisSpec Spec;
	FString Error;
	// A helicopter with no helicopter block, a car with one, a rotor with no thrust, and a collective that cannot hold the weight are not presets.
	FString Cut = Text.Replace(TEXT("\"helicopter\": {"), TEXT("\"rotorcraft\": {"));
	TestFalse(TEXT("a helicopter with no helicopter block is refused"), FMantleChassisSpec::Parse(Cut, Spec, Error));
	TestTrue(*FString::Printf(TEXT("and says so (%s)"), *Error), Error.Contains(TEXT("helicopter")));
	Cut = Text.Replace(TEXT("\"type\": \"helicopter\""), TEXT("\"type\": \"car\""));
	TestFalse(TEXT("a car with a helicopter block is refused"), FMantleChassisSpec::Parse(Cut, Spec, Error));
	Cut = Text.Replace(TEXT("\"thrust\": 39200"), TEXT("\"thrusts\": 39200"));
	TestFalse(TEXT("a rotor with no thrust is refused"), FMantleChassisSpec::Parse(Cut, Spec, Error));
	TestTrue(*FString::Printf(TEXT("and says which (%s)"), *Error), Error.Contains(TEXT("rotor.thrust")));
	Cut = Text.Replace(TEXT("\"hoverCollective\": 0.625"), TEXT("\"hoverCollective\": 1.2"));
	TestFalse(TEXT("a rotor that cannot hold the weight is refused"), FMantleChassisSpec::Parse(Cut, Spec, Error));
	Cut = Text.Replace(TEXT("\"driver\": \"pilot\""), TEXT("\"driver\": \"car\""));
	TestFalse(TEXT("a helicopter without a pilot is refused"), FMantleChassisSpec::Parse(Cut, Spec, Error));
	EMantleChassisType Type = EMantleChassisType::Car;
	TestTrue(TEXT("the type name reads back"), FMantleChassisSpec::ParseType(TEXT("helicopter"), Type) && Type == EMantleChassisType::Helicopter);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleChassisHelicopterScaledTest,
	"Mantle.Vehicle.Chassis.HelicopterScaledKeepsHoverCollective",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleChassisHelicopterScaledTest::RunTest(const FString& Parameters)
{
	using namespace MantleChassisHelicopterTestPrivate;
	FMantleChassisSpec Heli;
	if (!LoadHelicopter(*this, Heli))
	{
		return false;
	}
	const double Ratio = Heli.Helicopter.RotorThrustNewtons / Heli.Archetype.Mass;
	// At the preset's own size the rules give back its own thrust.
	TestTrue(TEXT("the archetype derives its own thrust"), FMath::IsNearlyEqual(FMantleChassisDerive::Derive(Heli).RotorThrustNewtons, Heli.Helicopter.RotorThrustNewtons));
	// Resized, the rotor lifts the same share of whatever the helicopter weighs (its mass held at a limit included), so the hover collective stays.
	for (const double Factor : { 0.25, 0.7, 1.3, 2.5 })
	{
		const FMantleChassisSpec Sized = FMantleChassisSpec::Scaled(Heli, Factor);
		if (!TestTrue(*FString::Printf(TEXT("x%g is derived"), Factor), Sized.Derived.IsSet()))
		{
			continue;
		}
		const FMantleChassisHandling& Handling = Sized.Derived.GetValue();
		TestTrue(*FString::Printf(TEXT("x%g: thrust %g N for %g kg, the archetype's thrust per kg"), Factor, Handling.RotorThrustNewtons, Handling.Mass),
			FMath::IsNearlyEqual(Handling.RotorThrustNewtons / Handling.Mass, Ratio, 1.0e-3 * Ratio));
		TestEqual(*FString::Printf(TEXT("x%g: the hover collective is the preset's"), Factor), Sized.Helicopter.HoverCollective, Heli.Helicopter.HoverCollective);
		TestTrue(*FString::Printf(TEXT("x%g: the rotor grows with it"), Factor), FMath::IsNearlyEqual(Sized.Helicopter.RotorDiameter, Factor * Heli.Helicopter.RotorDiameter));
		TestTrue(*FString::Printf(TEXT("x%g: its hub stays over the centre of mass"), Factor), FMath::IsNearlyEqual(Sized.MainRotorHub().X, Sized.CenterOfMass.X));
		TestTrue(*FString::Printf(TEXT("x%g: the tail rotor stays clear of the main disc"), Factor),
			FMath::Abs(Sized.TailRotorHub().X - Sized.MainRotorHub().X) > 0.5 * (Sized.Helicopter.RotorDiameter + Sized.Helicopter.TailRotorDiameter));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
