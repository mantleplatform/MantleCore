// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleAsset.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// A car split into a body and attached parts has most of its surface in the parts: FMantleVehicleBodyCollision::WithParts puts each part's triangles into the body's shape at
// the part's transform on its bone. Pure arithmetic: the same on a server and a client.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehiclePartShapeTest,
	"Mantle.Vehicle.PartShape.WithPartsPlacesEachPart",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehiclePartShapeTest::RunTest(const FString&)
{
	// A body of one triangle.
	const TSharedRef<FMantleVehicleBodyCollision> Body = MakeShared<FMantleVehicleBodyCollision>();
	Body->Positions = { FVector3f(0, 0, 0), FVector3f(10, 0, 0), FVector3f(0, 10, 0) };
	Body->Indices = { 0, 1, 2 };

	// A part of a quad (two triangles) and a triangle with no area and one with a corner that is not a vertex, in its own space, turned a quarter and put at (100, 0, 50).
	FMantleVehicleBodyCollision::FPart Part;
	Part.Positions = { FVector3f(0, 0, 0), FVector3f(1, 0, 0), FVector3f(1, 1, 0), FVector3f(0, 1, 0), FVector3f(5, 5, 5) };
	Part.Indices = { 0, 1, 2, 0, 2, 3, 0, 4, 4, 0, 1, 9 };
	Part.ToComponent = FTransform(FRotator(0.0, 90.0, 0.0), FVector(100, 0, 50));

	const TArray<FMantleVehicleBodyCollision::FPart> Parts = { Part };
	const TArray<FMantleVehicleBodyCollision::FPart> None;
	const TSharedPtr<const FMantleVehicleBodyCollision> Combined = FMantleVehicleBodyCollision::WithParts(Body, Parts);
	if (!TestTrue(TEXT("a shape is made"), Combined.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("the body's triangle and the part's two (the one with no area and the one past its vertices are dropped)"), Combined->NumTriangles(), 3);
	TestEqual(TEXT("the body's vertices come first, as they were"), Combined->Positions[1], FVector3f(10, 0, 0));
	// (1, 0, 0) turned a quarter about Z is (0, 1, 0), then moved: (100, 1, 50).
	TestTrue(TEXT("the part's vertices are in the body's space, turned and moved"), Combined->Positions.ContainsByPredicate([](const FVector3f& P) { return P.Equals(FVector3f(100, 1, 50), 0.001f); }));
	TestTrue(TEXT("and every index points at a vertex"), !Combined->Indices.ContainsByPredicate([&Combined](const int32 Index) { return Index < 0 || Index >= Combined->Positions.Num(); }));

	// No body: the shape is the parts'. Nothing at all: no shape.
	const TSharedPtr<const FMantleVehicleBodyCollision> OnlyParts = FMantleVehicleBodyCollision::WithParts(nullptr, Parts);
	TestTrue(TEXT("a car with no body triangles has its parts' shape"), OnlyParts.IsValid() && OnlyParts->NumTriangles() == 2);
	TestFalse(TEXT("a car with neither has none"), FMantleVehicleBodyCollision::WithParts(nullptr, None).IsValid());
	TestTrue(TEXT("a car with only its body keeps it"), FMantleVehicleBodyCollision::WithParts(Body, None).IsValid() && FMantleVehicleBodyCollision::WithParts(Body, None)->NumTriangles() == 1);
	TestEqual(TEXT("and the body it was given is not changed"), Body->NumTriangles(), 1);
	return true;
}

#endif
