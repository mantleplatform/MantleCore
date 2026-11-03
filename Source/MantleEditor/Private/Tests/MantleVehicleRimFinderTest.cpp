// Copyright Joshua Eger. All Rights Reserved.
#include "MantleGlbWriter.h"
#include "MantleVehicleRimFinder.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// The rim finder (vehicle-customization-plan.md section 3): the sections a wheel swap hides.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVehicleRimsTestPrivate
{
	/** Adds a primitive of Count vertices drawn with Material, each weighted fully to Joint, ringed Radius cm round Center (a rim; 0: at Center). */
	void AddSection(FMantleGlbMesh& Body, const TCHAR* Material, int32 Joint, const FVector& Center, double Radius, int32 Count)
	{
		FMantleGlbPrimitive& Primitive = Body.Primitives.AddDefaulted_GetRef();
		Primitive.MaterialIndex = Body.MaterialNames.Add(Material);
		Primitive.FirstVertex = Body.Positions.Num();
		Primitive.NumVertices = Count;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const double Angle = 2.0 * UE_PI * Index / Count;
			Body.Positions.Add(FVector3f(Center + FVector(FMath::Cos(Angle) * Radius, 0.0, FMath::Sin(Angle) * Radius)));
			Body.InfluenceJoints.Add(static_cast<uint16>(Joint));
			Body.InfluenceWeights.Add(1.f);
		}
	}

	/** A body with a root and two wheels (at x = -150 and 150), and the sections the tests name. */
	FMantleGlbMesh MakeBody()
	{
		FMantleGlbMesh Body;
		Body.NumInfluences = 1;
		Body.Joints.Add({ FName(TEXT("root")), INDEX_NONE, FTransform::Identity });
		Body.Joints.Add({ FName(TEXT("wheel_l")), 0, FTransform(FVector(-150.0, 0.0, 40.0)) });
		Body.Joints.Add({ FName(TEXT("wheel_r")), 0, FTransform(FVector(150.0, 0.0, 40.0)) });
		// One rim section for both wheels, as a GTA car's [WHEEL] material is.
		AddSection(Body, TEXT("alloy [WHEEL]"), 1, FVector(-150.0, 0.0, 40.0), 30.0, 20);
		FMantleGlbPrimitive& Both = Body.Primitives.Last();
		for (int32 Index = 0; Index < 10; ++Index)
		{
			Body.InfluenceJoints[Both.FirstVertex + Index] = 2;
			Body.Positions[Both.FirstVertex + Index] += FVector3f(300.f, 0.f, 0.f);
		}
		AddSection(Body, TEXT("rs7_tire"), 1, FVector(-150.0, 0.0, 40.0), 40.0, 20);
		return Body;
	}

	const TArray<FName> Bones = { FName(TEXT("wheel_l")), FName(TEXT("wheel_r")) };
	const TArray<double> Radii = { 40.0, 40.0 };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleRimsFindTest,
	"Mantle.Vehicle.Rims.Find",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleRimsFindTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleRimsTestPrivate;

	TestTrue(TEXT("rim, wheel, alloy, hub and spoke read as rims"), FMantleVehicleRims::NameIsRim(TEXT("Chrome_Rim")) && FMantleVehicleRims::NameIsRim(TEXT("vehicle [WHEEL]"))
		&& FMantleVehicleRims::NameIsRim(TEXT("alloy_01")) && FMantleVehicleRims::NameIsRim(TEXT("hubcap")) && FMantleVehicleRims::NameIsRim(TEXT("spokes")));
	TestFalse(TEXT("trim and primary are not rim: only whole words count"), FMantleVehicleRims::NameIsRim(TEXT("trim_chrome")) || FMantleVehicleRims::NameIsRim(TEXT("Primary_Paint"))
		|| FMantleVehicleRims::NameIsRim(TEXT("PRIMARY")) || FMantleVehicleRims::NameIsRim(TEXT("whee_l")));
	TestTrue(TEXT("a word inside a camel-case or numbered name still counts"), FMantleVehicleRims::NameIsRim(TEXT("ChromeRim01")) && FMantleVehicleRims::NameIsRim(TEXT("front-alloy.001")));
	TestFalse(TEXT("tires, brakes and the steering wheel do not"), FMantleVehicleRims::NameIsRim(TEXT("rs7_tire")) || FMantleVehicleRims::NameIsRim(TEXT("wheel_rubber"))
		|| FMantleVehicleRims::NameIsRim(TEXT("brake_disc")) || FMantleVehicleRims::NameIsRim(TEXT("steeringwheel")) || FMantleVehicleRims::NameIsRim(TEXT("body")));

	// A rim section for both wheels: each wheel has it; the tire (named as one) is never a rim.
	FMantleGlbMesh Body = MakeBody();
	TArray<FString> Notes;
	const TArray<FString> Sections = { TEXT("alloy [WHEEL]"), TEXT("rs7_tire") };
	TArray<TArray<FString>> Rims = FMantleVehicleRims::Find(Body, Bones, Radii, Sections, Notes);
	TestEqual(TEXT("a rim for each wheel"), Rims.Num(), 2);
	if (Rims.Num() == 2)
	{
		TestEqual(TEXT("the left wheel's rim"), Rims[0], TArray<FString>{ TEXT("alloy [WHEEL]") });
		TestEqual(TEXT("the right wheel's rim"), Rims[1], TArray<FString>{ TEXT("alloy [WHEEL]") });
	}
	TestEqual(TEXT("nothing to say"), Notes.Num(), 0);

	// How far each wheel's rim reaches from its bone: the ring is 30 cm out.
	TArray<double> Reach;
	FMantleVehicleRims::Find(Body, Bones, Radii, Sections, Notes, &Reach);
	TestEqual(TEXT("a reach for each wheel"), Reach.Num(), 2);
	if (Reach.Num() == 2)
	{
		TestEqual(TEXT("the left rim reaches 30 cm"), Reach[0], 30.0, 0.5);
		TestEqual(TEXT("the right rim reaches 30 cm"), Reach[1], 30.0, 0.5);
	}

	// A section named like a rim that is mixed with the body (half its vertices are far from any wheel) is left alone, and says so.
	FMantleGlbMesh Mixed = MakeBody();
	AddSection(Mixed, TEXT("body_wheel_arch"), 0, FVector(0.0, 0.0, 100.0), 50.0, 20);
	const FMantleGlbPrimitive& Arch = Mixed.Primitives.Last();
	for (int32 Index = 0; Index < 10; ++Index)
	{
		Mixed.InfluenceJoints[Arch.FirstVertex + Index] = 1;
	}
	Notes.Reset();
	Rims = FMantleVehicleRims::Find(Mixed, Bones, Radii, { TEXT("alloy [WHEEL]"), TEXT("rs7_tire"), TEXT("body_wheel_arch") }, Notes);
	TestEqual(TEXT("the mixed section is not a rim"), Rims[0], TArray<FString>{ TEXT("alloy [WHEEL]") });
	TestEqual(TEXT("and the creator is told"), Notes.Num(), 1);

	// Geometry decides too: a rim-named section far outside the wheel's radius is not the rim.
	FMantleGlbMesh Far = MakeBody();
	AddSection(Far, TEXT("rim_far"), 1, FVector(-150.0, 0.0, 40.0), 120.0, 20);
	Notes.Reset();
	Rims = FMantleVehicleRims::Find(Far, Bones, Radii, { TEXT("alloy [WHEEL]"), TEXT("rs7_tire"), TEXT("rim_far") }, Notes);
	TestFalse(TEXT("a rim-named section outside the wheel is not a rim"), Rims[0].Contains(TEXT("rim_far")));

	// A car whose wheels are welded in (no section named like a rim): no rims, no notes.
	FMantleGlbMesh Welded;
	Welded.NumInfluences = 1;
	Welded.Joints = Body.Joints;
	AddSection(Welded, TEXT("vehicle_body"), 1, FVector(-150.0, 0.0, 40.0), 30.0, 20);
	Notes.Reset();
	Rims = FMantleVehicleRims::Find(Welded, Bones, Radii, { TEXT("vehicle_body") }, Notes);
	TestTrue(TEXT("a welded car has no rims"), Rims[0].IsEmpty() && Rims[1].IsEmpty() && Notes.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleRimsGeometryTest,
	"Mantle.Vehicle.Rims.Geometry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleRimsGeometryTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleRimsTestPrivate;
	TArray<FString> Notes;

	// A wide rim (radius 18, 25 wide) is not far from its bone: it is wide. The distance that counts is round the axle.
	{
		FMantleGlbMesh Wide = MakeBody();
		AddSection(Wide, TEXT("wide_rim"), 1, FVector(-150.0, 0.0, 40.0), 18.0, 24);
		const FMantleGlbPrimitive& Section = Wide.Primitives.Last();
		for (int32 Index = 0; Index < Section.NumVertices; ++Index)
		{
			Wide.Positions[Section.FirstVertex + Index].Y += (Index % 2 == 0) ? 12.5f : -12.5f;
		}
		const TArray<double> TightRadii = { 18.0, 18.0 };
		const TArray<TArray<FString>> Rims = FMantleVehicleRims::Find(Wide, Bones, TightRadii, { TEXT("alloy [WHEEL]"), TEXT("rs7_tire"), TEXT("wide_rim") }, Notes);
		TestTrue(TEXT("a wide rim is found"), Rims[0].Contains(TEXT("wide_rim")));
	}

	// A section the body draws in two chunks is judged as one: half of it on the root is a section mixed with the body, whichever chunk is looked at.
	{
		FMantleGlbMesh Chunked = MakeBody();
		AddSection(Chunked, TEXT("chunk_rim"), 1, FVector(-150.0, 0.0, 40.0), 30.0, 20);
		const int32 Material = Chunked.Primitives.Last().MaterialIndex;
		AddSection(Chunked, TEXT("unused"), 0, FVector(0.0, 0.0, 100.0), 50.0, 20);
		Chunked.Primitives.Last().MaterialIndex = Material;
		Notes.Reset();
		const TArray<TArray<FString>> Rims = FMantleVehicleRims::Find(Chunked, Bones, Radii, { TEXT("alloy [WHEEL]"), TEXT("rs7_tire"), TEXT("chunk_rim"), TEXT("unused") }, Notes);
		TestFalse(TEXT("half of a section on the body, in another chunk, is not a rim"), Rims[0].Contains(TEXT("chunk_rim")));
		TestEqual(TEXT("and the creator is told once"), Notes.Num(), 1);
	}

	// One stray vertex of another wheel does not give that wheel the rim.
	{
		FMantleGlbMesh Stray = MakeBody();
		AddSection(Stray, TEXT("stray_rim"), 1, FVector(-150.0, 0.0, 40.0), 30.0, 40);
		const FMantleGlbPrimitive& Section = Stray.Primitives.Last();
		Stray.InfluenceJoints[Section.FirstVertex] = 2;
		Stray.Positions[Section.FirstVertex] = FVector3f(150.f, 0.f, 70.f);
		Notes.Reset();
		const TArray<TArray<FString>> Rims = FMantleVehicleRims::Find(Stray, Bones, Radii, { TEXT("alloy [WHEEL]"), TEXT("rs7_tire"), TEXT("stray_rim") }, Notes);
		TestTrue(TEXT("the wheel that owns the section has it"), Rims[0].Contains(TEXT("stray_rim")));
		TestFalse(TEXT("the wheel with one stray vertex does not"), Rims[1].Contains(TEXT("stray_rim")));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
