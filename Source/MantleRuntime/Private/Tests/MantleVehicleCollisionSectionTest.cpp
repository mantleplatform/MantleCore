// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleAsset.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

// A car split into a body and attached parts carries the parts' collision triangles in its file: MVH1 version 2's last section. The cook's own fixture (backend
// TestVehicleCollisionFixtureMatchesWhatWeWriteToday) is the tiny car with a door's two triangles.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleCollisionSectionTest,
	"Mantle.Vehicle.Asset.CollisionSection",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleCollisionSectionTest::RunTest(const FString&)
{
	const FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("../schema/fixtures/vehicle-collision-v2.mvh")));
	TArray<uint8> Bytes;
	if (!TestTrue(*FString::Printf(TEXT("the collision fixture exists (%s)"), *Path), FFileHelper::LoadFileToArray(Bytes, *Path)))
	{
		return false;
	}
	FMantleVehicleAsset Asset;
	FString Error;
	if (!TestTrue(*FString::Printf(TEXT("the reader takes a version 2 vehicle (%s)"), *Error), FMantleVehicleAssetReader::Read(Bytes, Asset, Error)))
	{
		return false;
	}
	TestEqual(TEXT("with the door's four corners"), Asset.CollisionPositions.Num(), 4);
	TestEqual(TEXT("and its two triangles"), Asset.CollisionIndices.Num(), 6);
	// glTF (3, 1, 0) m is Unreal (300, 0, 100) cm.
	TestTrue(TEXT("in the body's space, in centimeters, Unreal's axes"), Asset.CollisionPositions.ContainsByPredicate([](const FVector3f& P) { return P.Equals(FVector3f(300, 0, 100), 0.001f); }));

	// The built car's shape is its body's triangles and the door's two.
	FMantleVehicleAsset WithoutDoor = Asset;
	WithoutDoor.CollisionPositions.Reset();
	WithoutDoor.CollisionIndices.Reset();
	const UMantleBuiltVehicle* Split = UMantleBuiltVehicle::Build(Asset, GetTransientPackage(), &Error);
	const UMantleBuiltVehicle* Plain = UMantleBuiltVehicle::Build(WithoutDoor, GetTransientPackage(), &Error);
	if (!TestNotNull(*FString::Printf(TEXT("the car builds (%s)"), *Error), Split) || !TestNotNull(TEXT("and without the door"), Plain)
		|| !TestTrue(TEXT("both have a shape"), Split->BodyCollision.IsValid() && Plain->BodyCollision.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("the shape is the body's triangles and the door's two"), Split->BodyCollision->NumTriangles(), Plain->BodyCollision->NumTriangles() + 2);
	TestTrue(TEXT("and every index is a vertex"), !Split->BodyCollision->Indices.ContainsByPredicate([&Split](const int32 Index) { return Index < 0 || Index >= Split->BodyCollision->Positions.Num(); }));

	// A version 1 vehicle (a car with no split parts) has no section and the shape of its body.
	const FString LightsPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("../schema/fixtures/vehicle-lights-v1.mvh")));
	TArray<uint8> Version1;
	FMantleVehicleAsset OldCar;
	if (TestTrue(TEXT("the version 1 fixture exists"), FFileHelper::LoadFileToArray(Version1, *LightsPath)) && TestTrue(TEXT("and reads"), FMantleVehicleAssetReader::Read(Version1, OldCar, Error)))
	{
		TestEqual(TEXT("with no collision section"), OldCar.CollisionIndices.Num(), 0);
	}

	// Damaged files are refused, as every cooked reader refuses them.
	const auto Refused = [&](const TCHAR* What, const TArray<uint8>& Damaged)
	{
		FMantleVehicleAsset Out;
		FString Why;
		TestFalse(*FString::Printf(TEXT("a vehicle with %s is refused"), What), FMantleVehicleAssetReader::Read(Damaged, Out, Why));
	};
	TArray<uint8> Damaged = Bytes;
	Damaged.SetNum(Damaged.Num() - 4);
	Refused(TEXT("its collision section cut short"), Damaged);
	Damaged = Bytes;
	Damaged.Add(0);
	Refused(TEXT("a byte after the section"), Damaged);
	Damaged = Bytes;
	Damaged[Damaged.Num() - 4] = 99; // the last index now names a vertex that is not there
	Refused(TEXT("an index past its vertices"), Damaged);
	Damaged = Bytes;
	Damaged[4] = 1; // version 1 has nothing after the body
	Refused(TEXT("a section in a version 1 file"), Damaged);
	Damaged = Bytes;
	Damaged[4] = 3;
	Refused(TEXT("a version this build does not read"), Damaged);
	return true;
}

#endif
