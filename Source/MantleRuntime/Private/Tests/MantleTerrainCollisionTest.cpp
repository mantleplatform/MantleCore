// Copyright Joshua Eger. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS

#include "MantleHeightfieldReader.h"
#include "MantleTerrainBuilder.h"
#include "MantleTerrainCollision.h"
#include "MantleTerrainSurface.h"

#include "Chaos/HeightField.h"
#include "CollisionQueryParams.h"
#include "Engine/EngineTypes.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"

namespace MantleTerrainCollisionTestPrivate
{
	/** A Width x Height heightfield at 100 cm spacing and 1 cm a unit whose origin is at the height field's own bias, so a sample's height is (value - 32768) cm in the shape and in the file. */
	TSharedRef<FMantleHeightfield> MakeField(const int32 Width, const int32 Height, TFunctionRef<int32(int32, int32)> Value)
	{
		TSharedRef<FMantleHeightfield> Field = MakeShared<FMantleHeightfield>();
		Field->Width = Width;
		Field->Height = Height;
		Field->SpacingX = 100.0f;
		Field->SpacingY = 100.0f;
		Field->HeightScale = 1.0f;
		Field->HeightOffset = -static_cast<float>(FMantleTerrainCollision::HeightBias);
		Field->Heights.SetNumUninitialized(Width * Height);
		for (int32 Y = 0; Y < Height; ++Y)
		{
			for (int32 X = 0; X < Width; ++X)
			{
				Field->Heights[Y * Width + X] = static_cast<uint16>(Value(X, Y));
			}
		}
		return Field;
	}

	/** The block of the whole field as a Chaos height field, built as the component builds it. */
	Chaos::FHeightFieldPtr MakeShape(const FMantleHeightfield& Field, const FMantleTerrainHoleGrid* Holes)
	{
		TArray<uint16> Heights;
		FMantleTerrainCollision::BlockHeights(Field, FIntPoint(0, 0), FIntPoint(Field.Width - 1, Field.Height - 1), Heights);
		TArray<uint8> Materials;
		FMantleTerrainCollision::BlockMaterialIndices(FIntPoint(0, 0), FIntPoint(Field.Width - 1, Field.Height - 1), Holes, Materials);
		Chaos::FHeightFieldPtr Shape(new Chaos::FHeightField(TArrayView<const uint16>(Heights), TArrayView<const uint8>(Materials), Field.Height, Field.Width,
			Chaos::FVec3(Field.SpacingX, Field.SpacingY, Field.HeightScale)));
		// The shape's bounds are made the first time they are asked for, and a ray walks the grid inside them: a shape that was never asked misses its first ray. The engine asks when it puts the
		// shape in a body (the component does it in OnCreatePhysicsState), so the test asks here.
		Shape->BoundingBox();
		return Shape;
	}

	/** Where a ray straight down at (X, Y) (cm) meets the shape: false when it misses. */
	bool RayDown(const Chaos::FHeightField& Shape, const double X, const double Y, double& OutZ)
	{
		Chaos::FReal Time = 0;
		Chaos::FVec3 Position;
		Chaos::FVec3 Normal;
		int32 Face = INDEX_NONE;
		if (!Shape.Raycast(Chaos::FVec3(X, Y, 10000.0), Chaos::FVec3(0, 0, -1), 20000.0, 0.0, Time, Position, Normal, Face))
		{
			return false;
		}
		OutZ = Position.Z;
		return true;
	}

	/** The height of the mesh description's triangle that holds (X, Y) in plan, or false when none does: Mantle's own drawn ground. */
	bool MeshHeightAt(const FMeshDescription& Mesh, const double X, const double Y, double& OutZ)
	{
		const FStaticMeshConstAttributes Attributes(Mesh);
		const TVertexAttributesConstRef<FVector3f> Positions = Attributes.GetVertexPositions();
		for (const FTriangleID Triangle : Mesh.Triangles().GetElementIDs())
		{
			const auto Vertices = Mesh.GetTriangleVertices(Triangle);
			const FVector A(Positions[Vertices[0]]);
			const FVector B(Positions[Vertices[1]]);
			const FVector C(Positions[Vertices[2]]);
			const double Denominator = (B.Y - C.Y) * (A.X - C.X) + (C.X - B.X) * (A.Y - C.Y);
			if (FMath::IsNearlyZero(Denominator))
			{
				continue;
			}
			const double U = ((B.Y - C.Y) * (X - C.X) + (C.X - B.X) * (Y - C.Y)) / Denominator;
			const double V = ((C.Y - A.Y) * (X - C.X) + (A.X - C.X) * (Y - C.Y)) / Denominator;
			const double W = 1.0 - U - V;
			if (U >= -1.0e-6 && V >= -1.0e-6 && W >= -1.0e-6)
			{
				OutZ = U * A.Z + V * B.Z + W * C.Z;
				return true;
			}
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainCollisionSplitTest,
	"Mantle.Runtime.TerrainCollision.SplitMatchesTheDrawnGround",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainCollisionSplitTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainCollisionTestPrivate;
	// A block of 129 x 129 samples (the size a terrain's blocks are; the engine's grid walk does not find the ground of a field of a few samples), flat at 32,868 (100 cm above the shape's
	// zero) with sample (4, 4) raised to 33,368 (600 cm): the first four quads of each row are checked, and in quad (3, 3) the two diagonals disagree.
	const TSharedRef<FMantleHeightfield> Field = MakeField(129, 129, [](const int32 X, const int32 Y) { return (X == 4 && Y == 4) ? 33368 : 32868; });
	const Chaos::FHeightFieldPtr Shape = MakeShape(*Field, nullptr);

	FMeshDescription Mesh;
	const bool bNoSkirts[4] = { false, false, false, false };
	FMantleTerrainBuilder::BuildChunkDescription(*Field, FIntPoint(0, 0), FIntPoint(4, 4), 1, 0.0f, bNoSkirts, Mesh);

	double Worst = 0.0;
	for (int32 Y = 0; Y < 4; ++Y)
	{
		for (int32 X = 0; X < 4; ++X)
		{
			// Points inside each of the quad's two triangles, never on the diagonal (a ray exactly on the shared edge can fall between the two triangles' tests, in the drawn ground as in the shape).
			const double Plan[3][2] = { { (X + 0.713) * 100.0, (Y + 0.337) * 100.0 }, { (X + 0.337) * 100.0, (Y + 0.713) * 100.0 }, { (X + 0.871) * 100.0, (Y + 0.229) * 100.0 } };
			for (const double (&At)[2] : Plan)
			{
				double Drawn = 0.0;
				double Collided = 0.0;
				if (!TestTrue(*FString::Printf(TEXT("the drawn ground has a triangle at quad (%d, %d)"), X, Y), MeshHeightAt(Mesh, At[0], At[1], Drawn))
					|| !TestTrue(*FString::Printf(TEXT("the ray meets the shape at quad (%d, %d)"), X, Y), RayDown(*Shape, At[0], At[1], Collided)))
				{
					return false;
				}
				Worst = FMath::Max(Worst, FMath::Abs(Drawn - Collided));
			}
		}
	}
	TestTrue(*FString::Printf(TEXT("the shape and the drawn ground agree everywhere (worst %.4f cm)"), Worst), Worst < 0.05);
	// In the raised corner's quad, a point 45% along X and 65% along Y is in the triangle (x, y), (x, y + 1), (x + 1, y + 1): 100 + 0.45 * 500 = 325 cm. The other split would put it in the
	// triangle that holds the raised corner's neighbors on the far side of its diagonal: 600 - 0.55 * 500 - 0.35 * 500 = 150 cm.
	double Point = 0.0;
	TestTrue(TEXT("the raised corner's quad"), RayDown(*Shape, 345.0, 365.0, Point));
	TestTrue(*FString::Printf(TEXT("a point off the diagonal is on the (x, y) to (x + 1, y + 1) split: 325 cm, not 150 (%.2f)"), Point), FMath::Abs(Point - 325.0) < 0.05);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainCollisionHolesTest,
	"Mantle.Runtime.TerrainCollision.HolesLetRaysThrough",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainCollisionHolesTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainCollisionTestPrivate;
	const TSharedRef<FMantleHeightfield> Field = MakeField(129, 129, [](const int32, const int32) { return 32868; });
	FMantleTerrainHoleGrid Holes;
	Holes.Width = 129;
	Holes.Height = 129;
	Holes.Quad.Init(0, 128 * 128);
	Holes.Quad[1 * 128 + 2] = 1;   // the quad whose lowest corner is sample (2, 1)

	TArray<uint8> Materials;
	FMantleTerrainCollision::BlockMaterialIndices(FIntPoint(0, 0), FIntPoint(4, 4), &Holes, Materials);
	if (!TestEqual(TEXT("one entry a quad"), Materials.Num(), 16))
	{
		return false;
	}
	TestEqual(TEXT("the hole quad is 255"), static_cast<int32>(Materials[1 * 4 + 2]), 255);
	int32 Holey = 0;
	for (const uint8 Index : Materials)
	{
		Holey += Index == FMantleTerrainCollision::HoleMaterialIndex ? 1 : 0;
	}
	TestEqual(TEXT("and only it"), Holey, 1);
	FMantleTerrainCollision::BlockMaterialIndices(FIntPoint(0, 0), FIntPoint(4, 4), nullptr, Materials);
	TestTrue(TEXT("a block with no hole has one entry, 0, never none"), Materials.Num() == 1 && Materials[0] == 0);
	// A block that is a part of the grid: its quads are counted from its first sample.
	FMantleTerrainCollision::BlockMaterialIndices(FIntPoint(2, 1), FIntPoint(4, 3), &Holes, Materials);
	TestTrue(TEXT("a block of 2 x 2 quads whose first quad is the hole"), Materials.Num() == 4 && Materials[0] == 255 && Materials[1] == 0 && Materials[2] == 0 && Materials[3] == 0);
	TestTrue(TEXT("a block with no hole in it has one entry"), [&]() { TArray<uint8> Other; FMantleTerrainCollision::BlockMaterialIndices(FIntPoint(0, 2), FIntPoint(2, 4), &Holes, Other); return Other.Num() == 1; }());
	TestTrue(TEXT("a block that is all hole says so"), FMantleTerrainCollision::BlockIsAllHole(FIntPoint(2, 1), FIntPoint(3, 2), &Holes));
	TestFalse(TEXT("a block with ground does not"), FMantleTerrainCollision::BlockIsAllHole(FIntPoint(2, 1), FIntPoint(4, 3), &Holes));

	const Chaos::FHeightFieldPtr Shape = MakeShape(*Field, &Holes);
	double Z = 0.0;
	// Points off the diagonals, in both of the hole quad's triangles: nothing; and in the quads beside it: the ground.
	TestFalse(TEXT("a ray through the hole's lower triangle meets nothing"), RayDown(*Shape, 270.0, 140.0, Z));
	TestFalse(TEXT("nor through its upper triangle"), RayDown(*Shape, 230.0, 170.0, Z));
	TestTrue(TEXT("a ray through the quad beside it meets the ground"), RayDown(*Shape, 160.0, 140.0, Z));
	TestTrue(TEXT("at 100 cm"), FMath::Abs(Z - 100.0) < 0.05);
	TestTrue(TEXT("and the quad on its other side"), RayDown(*Shape, 360.0, 140.0, Z));
	TestTrue(TEXT("and the quad above it"), RayDown(*Shape, 270.0, 240.0, Z));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainCollisionRoundingTest,
	"Mantle.Runtime.TerrainCollision.RoundingStaysUnderTheBound",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainCollisionRoundingTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainCollisionTestPrivate;
	// A block of 129 x 129 samples with a relief of 9,000 units at 0.25 cm: the shape stores each height again in 16 bits over the block's range, rounding down.
	FRandomStream Random(11);
	const TSharedRef<FMantleHeightfield> Field = MakeField(129, 129, [&Random](const int32 X, const int32 Y) { return 20000 + Random.RandRange(0, 9000) + (X + Y) * 3; });
	Field->HeightScale = 0.25f;
	Field->HeightOffset = -static_cast<float>(FMantleTerrainCollision::HeightBias) * Field->HeightScale;
	const FIntPoint First(0, 0);
	const FIntPoint Last(128, 128);
	const double Bound = FMantleTerrainCollision::RoundingBoundCm(*Field, First, Last);
	uint16 Lowest = 65535;
	uint16 Highest = 0;
	for (const uint16 Value : Field->Heights)
	{
		Lowest = FMath::Min(Lowest, Value);
		Highest = FMath::Max(Highest, Value);
	}
	TestTrue(*FString::Printf(TEXT("the bound is the range over 65,535 steps (%.5f cm)"), Bound), FMath::Abs(Bound - static_cast<double>(Highest - Lowest) * 0.25 / 65535.0) < 1.0e-9);

	const Chaos::FHeightFieldPtr Shape = MakeShape(*Field, nullptr);
	double Largest = 0.0;
	bool bNeverAbove = true;
	for (int32 Y = 0; Y < 129; ++Y)
	{
		for (int32 X = 0; X < 129; ++X)
		{
			const double InFile = (static_cast<double>(Field->Heights[Y * 129 + X]) - FMantleTerrainCollision::HeightBias) * 0.25;
			const double InShape = Shape->GetPointScaled(Y * 129 + X).Z;
			bNeverAbove = bNeverAbove && InShape <= InFile + 1.0e-4;
			Largest = FMath::Max(Largest, InFile - InShape);
		}
	}
	TestTrue(TEXT("no sample of the shape is above the file's"), bNeverAbove);
	TestTrue(*FString::Printf(TEXT("the largest the shape lies below is %.5f cm, within the bound %.5f"), Largest, Bound), Largest <= Bound + 1.0e-4);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainCollisionBlocksTest,
	"Mantle.Runtime.TerrainCollision.BlocksOnTheActor",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainCollisionBlocksTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainCollisionTestPrivate;
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("MantleTerrainCollisionTestWorld"));
	if (!TestNotNull(TEXT("a world"), World))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		World->DestroyWorld(/*bInformEngineOfWorld*/ false);
		if (World->IsRooted())
		{
			World->RemoveFromRoot();
		}
	};
	// 257 x 129 samples: two blocks along X of 128 squares, one along Y. Flat at 100 cm, except that the second block is at 300 cm.
	const TSharedRef<FMantleHeightfield> Field = MakeField(257, 129, [](const int32 X, const int32) { return X < 128 ? 32868 : 33068; });
	FMantleTerrainBuildParams Params;
	Params.bInitRenderResources = false;
	Params.FieldOwner = Field;
	FString Error;

	// A server's terrain: no chunk mesh at all, only the blocks.
	{
		FMantleTerrainBuildParams ServerParams = Params;
		ServerParams.bRenderLODs = false;
		ServerParams.bChunkMeshes = false;
		AActor* Server = FMantleTerrainBuilder::Spawn(World, TEXT("Server"), *Field, nullptr, true, ServerParams, &Error);
		if (!TestNotNull(TEXT("the server's terrain"), Server))
		{
			AddError(Error);
			return false;
		}
		TArray<UStaticMeshComponent*> Meshes;
		Server->GetComponents<UStaticMeshComponent>(Meshes);
		TestEqual(TEXT("no chunk mesh"), Meshes.Num(), 0);
		TArray<UMantleTerrainCollisionComponent*> Blocks;
		Server->GetComponents<UMantleTerrainCollisionComponent>(Blocks);
		TestEqual(TEXT("two blocks of ground"), Blocks.Num(), 2);
		for (const UMantleTerrainCollisionComponent* Block : Blocks)
		{
			TestTrue(TEXT("each has its shape"), Block->HasShape());
			TestTrue(TEXT("and no more than the heights, a min and a max a block of six and the material index"), Block->GetShapeBytes() > 0 && Block->GetShapeBytes() < 129 * 129 * 3);
			TestFalse(TEXT("its bounds are not empty"), Block->Bounds.BoxExtent.IsNearlyZero());
		}
		// A ray down the middle of each block meets the terrain's actor at the block's own height.
		FHitResult Hit;
		FCollisionQueryParams Query;
		Query.bTraceComplex = true;
		TestTrue(TEXT("a ray at the first block hits"), World->LineTraceSingleByChannel(Hit, FVector(6400.0, 6400.0, 5000.0), FVector(6400.0, 6400.0, -5000.0), ECC_Visibility, Query));
		TestTrue(TEXT("the terrain's actor"), Hit.GetActor() == Server);
		TestTrue(*FString::Printf(TEXT("at 100 cm (%.2f)"), Hit.ImpactPoint.Z), FMath::Abs(Hit.ImpactPoint.Z - 100.0) < 0.1);
		TestTrue(TEXT("a ray at the second block hits"), World->LineTraceSingleByChannel(Hit, FVector(19200.0, 6400.0, 5000.0), FVector(19200.0, 6400.0, -5000.0), ECC_Visibility, Query));
		TestTrue(*FString::Printf(TEXT("at 300 cm (%.2f)"), Hit.ImpactPoint.Z), FMath::Abs(Hit.ImpactPoint.Z - 300.0) < 0.1);
		TestFalse(TEXT("a ray past the end of the terrain misses"), World->LineTraceSingleByChannel(Hit, FVector(30000.0, 6400.0, 5000.0), FVector(30000.0, 6400.0, -5000.0), ECC_Visibility, Query));
		Server->Destroy();
	}

	// A player's machine: the far ground is the pictures, and the same blocks stand under it.
	{
		FMantleTerrainBuildParams FarParams = Params;
		FarParams.bFarOnly = true;
		AActor* Far = FMantleTerrainBuilder::Spawn(World, TEXT("Far"), *Field, nullptr, true, FarParams, &Error);
		if (!TestNotNull(TEXT("the far ground"), Far))
		{
			AddError(Error);
			return false;
		}
		TArray<UStaticMeshComponent*> Meshes;
		Far->GetComponents<UStaticMeshComponent>(Meshes);
		TestEqual(TEXT("two far chunks"), Meshes.Num(), 2);
		for (const UStaticMeshComponent* Mesh : Meshes)
		{
			TestEqual(TEXT("a picture collides with nothing"), Mesh->GetCollisionEnabled(), ECollisionEnabled::NoCollision);
		}
		TArray<UMantleTerrainCollisionComponent*> Blocks;
		Far->GetComponents<UMantleTerrainCollisionComponent>(Blocks);
		TestEqual(TEXT("two blocks"), Blocks.Num(), 2);
		FHitResult Hit;
		TestTrue(TEXT("a ray hits the ground far from any view"), World->LineTraceSingleByChannel(Hit, FVector(19200.0, 6400.0, 5000.0), FVector(19200.0, 6400.0, -5000.0), ECC_Visibility));
		TestTrue(*FString::Printf(TEXT("at 300 cm (%.2f)"), Hit.ImpactPoint.Z), FMath::Abs(Hit.ImpactPoint.Z - 300.0) < 0.1);
		Far->Destroy();
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainCollisionMovedTest,
	"Mantle.Runtime.TerrainCollision.MovedAndScaledGroundCollides",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainCollisionMovedTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainCollisionTestPrivate;
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("MantleTerrainCollisionMovedTestWorld"));
	if (!TestNotNull(TEXT("a world"), World))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		World->DestroyWorld(/*bInformEngineOfWorld*/ false);
		if (World->IsRooted())
		{
			World->RemoveFromRoot();
		}
	};
	// 257 x 129 samples flat at 100 cm: 25,600 x 12,800 cm.
	const TSharedRef<FMantleHeightfield> Field = MakeField(257, 129, [](const int32, const int32) { return 32868; });
	FMantleTerrainBuildParams Params;
	Params.bInitRenderResources = false;
	Params.FieldOwner = Field;
	FString Error;
	const auto Down = [World](const double X, const double Y, double& OutZ)
	{
		FHitResult Hit;
		FCollisionQueryParams Query;
		Query.bTraceComplex = true;
		const bool bHit = World->LineTraceSingleByChannel(Hit, FVector(X, Y, 8000.0), FVector(X, Y, -8000.0), ECC_Visibility, Query);
		OutZ = bHit ? Hit.ImpactPoint.Z : 0.0;
		return bHit;
	};

	// A Movable terrain (what a Terrain with no frozen place is) moves, and its ground goes with it: the kinematic body follows the component.
	{
		AActor* Moving = FMantleTerrainBuilder::Spawn(World, TEXT("Moving"), *Field, nullptr, true, Params, &Error);
		if (!TestNotNull(TEXT("a movable terrain"), Moving))
		{
			AddError(Error);
			return false;
		}
		double Z = 0.0;
		TestTrue(TEXT("its ground at x = 300 is hit"), Down(300.0, 6400.0, Z));
		TestTrue(*FString::Printf(TEXT("at 100 cm (%.2f)"), Z), FMath::Abs(Z - 100.0) < 0.1);
		Moving->SetActorLocation(FVector(1000.0, 0.0, 500.0));
		TestFalse(TEXT("moved 1,000 cm along X: x = 300 is off the ground now"), Down(300.0, 6400.0, Z));
		TestTrue(TEXT("x = 1,300 is on it"), Down(1300.0, 6400.0, Z));
		TestTrue(*FString::Printf(TEXT("at 600 cm: the ground went up with the actor (%.2f)"), Z), FMath::Abs(Z - 600.0) < 0.1);
		// Scaled while it stands: its shapes are made again at the new scale (the body instance cannot rescale a height field).
		TestFalse(TEXT("x = 40,000 is past the end of the ground"), Down(40000.0, 6400.0, Z));
		Moving->SetActorScale3D(FVector(2.0, 1.0, 1.0));
		TestTrue(TEXT("scaled to twice as wide: x = 40,000 is ground"), Down(40000.0, 6400.0, Z));
		TestTrue(*FString::Printf(TEXT("still at 600 cm (%.2f)"), Z), FMath::Abs(Z - 600.0) < 0.1);
		Moving->SetActorLocation(FVector(2000.0, 0.0, 700.0));
		TestTrue(TEXT("and it still moves after the scale"), Down(40000.0, 6400.0, Z));
		TestTrue(*FString::Printf(TEXT("to 800 cm (%.2f)"), Z), FMath::Abs(Z - 800.0) < 0.1);
		Moving->Destroy();
	}

	// A terrain that ends scaled (1, 2, 1) and turned 30 degrees where a place put it: a ray at a point of its ground, found through the same transform, meets it at its height.
	{
		const FTransform Where(FRotator(0.0, 30.0, 0.0), FVector(2000.0, -1000.0, 50.0), FVector(1.0, 2.0, 1.0));
		TSharedPtr<FMantleTerrainSpawn> Spawn = FMantleTerrainSpawn::Begin(World, TEXT("Placed"), *Field, nullptr, true, Params, &Error, nullptr, &Where);
		if (!TestTrue(TEXT("the placed terrain begins"), Spawn.IsValid()))
		{
			AddError(Error);
			return false;
		}
		for (int32 Step = 0; !Spawn->Step() && Step < 200; ++Step)
		{
		}
		AActor* Placed = Spawn->TakeActor();
		if (!TestNotNull(TEXT("its actor"), Placed))
		{
			return false;
		}
		for (const FVector2D Local : { FVector2D(6400.0, 3200.0), FVector2D(20000.0, 9000.0), FVector2D(300.0, 12000.0) })
		{
			const FVector World3 = Where.TransformPosition(FVector(Local.X, Local.Y, 0.0));
			double Z = 0.0;
			if (TestTrue(*FString::Printf(TEXT("a ray at the ground point local (%.0f, %.0f) hits"), Local.X, Local.Y), Down(World3.X, World3.Y, Z)))
			{
				TestTrue(*FString::Printf(TEXT("at 150 cm (%.2f)"), Z), FMath::Abs(Z - 150.0) < 0.1);
			}
		}
		// Beyond the ground along the scaled Y (12,800 cm of ground is 25,600 cm in the world).
		double Z = 0.0;
		const FVector Past = Where.TransformPosition(FVector(6400.0, 14000.0, 0.0));
		TestFalse(TEXT("a point past the end of the ground along Y misses"), Down(Past.X, Past.Y, Z));
		Placed->Destroy();
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
