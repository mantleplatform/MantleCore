// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCellAssetUse.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// Who uses which uploaded mesh and material (stage 2 of large places): the counts the asset set fetches and lets go by. Expected values are worked out by hand in each test.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleCellAssetUseTestPrivate
{
	FString Id(const uint32 Number)
	{
		return FString::Printf(TEXT("ast_u%032x"), Number);
	}

	FMantleEntity Mesh(const FString& EntityId, const FString& MeshId, const EMantleCollision Collision, const TArray<FString>& Materials = TArray<FString>(), const bool bInstanced = false)
	{
		FMantleEntity Entity;
		Entity.Id = EntityId;
		FMantleStaticMeshProps Props;
		Props.Mesh = MeshId;
		Props.Materials = Materials;
		Props.Collision = Collision;
		if (bInstanced)
		{
			Entity.Type = EMantleEntityType::InstancedMesh;
			FMantleInstancedMeshProps Instanced;
			Instanced.Mesh = Props;
			Entity.Props.Set<FMantleInstancedMeshProps>(Instanced);
		}
		else
		{
			Entity.Type = EMantleEntityType::StaticMesh;
			Entity.Props.Set<FMantleStaticMeshProps>(Props);
		}
		return Entity;
	}

	FMantleCellAssetNeed Need(const FString& AssetId, const bool bMaterial, const bool bCollision = false)
	{
		FMantleCellAssetNeed Result;
		Result.Id = AssetId;
		Result.bMaterial = bMaterial;
		Result.bCollision = bCollision;
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellAssetNeedsOfTest,
	"Mantle.Runtime.CellStream.AssetUse.NeedsOf",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellAssetNeedsOfTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellAssetUseTestPrivate;
	const FString A = Id(0xa), B = Id(0xb), M1 = Id(0x101), M2 = Id(0x102);

	// E1: mesh A, solid, material M1 and a built-in id. E2: an instanced list of A with no collision, materials M1 and M2. E3: mesh B with no collision. E4: a built-in mesh (not uploaded, so nothing to fetch).
	TArray<FMantleEntity> Entities;
	Entities.Add(Mesh(TEXT("ent_1"), A, EMantleCollision::Blocking, { M1, TEXT("mat_default") }));
	Entities.Add(Mesh(TEXT("ent_2"), A, EMantleCollision::None, { M1, M2 }, /*bInstanced*/ true));
	Entities.Add(Mesh(TEXT("ent_3"), B, EMantleCollision::None));
	Entities.Add(Mesh(TEXT("ent_4"), TEXT("ast_cube"), EMantleCollision::Blocking));
	FMantleEntity Group;
	Group.Id = TEXT("ent_5");
	Group.Type = EMantleEntityType::Group;
	Group.Props.Set<FMantleEmptyProps>(FMantleEmptyProps{});
	Entities.Add(Group);

	TArray<FMantleCellAssetNeed> Needs;
	FMantleCellAssetUse::NeedsOf(Entities, Needs);
	// In first-use order: A (solid, since E1 collides, though E2 does not), M1, M2, B. M1 once; the built-in ids and the group name nothing.
	if (!TestEqual(TEXT("four assets"), Needs.Num(), 4))
	{
		return false;
	}
	TestTrue(TEXT("A is a mesh and needs collision"), Needs[0] == Need(A, false, true));
	TestTrue(TEXT("M1 is a material"), Needs[1] == Need(M1, true));
	TestTrue(TEXT("M2 is a material"), Needs[2] == Need(M2, true));
	TestTrue(TEXT("B is a mesh and needs none"), Needs[3] == Need(B, false, false));

	// No entity at all names nothing.
	FMantleCellAssetUse::NeedsOf(TArray<FMantleEntity>(), Needs);
	TestTrue(TEXT("no entities, no needs"), Needs.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellAssetCountsTest,
	"Mantle.Runtime.CellStream.AssetUse.Counts",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellAssetCountsTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellAssetUseTestPrivate;
	const FString A = Id(0xa), B = Id(0xb), M1 = Id(0x101), M2 = Id(0x102);
	FMantleCellAssetUse Use;
	TArray<FMantleCellAssetNeed> Started;
	TArray<FMantleCellAssetNeed> Ended;

	// Cell 1 wants A (solid) and M1. Both are new.
	Use.Acquire(1, { Need(A, false, true), Need(M1, true) }, Started);
	TestEqual(TEXT("cell 1 starts two assets"), Started.Num(), 2);
	TestTrue(TEXT("A with collision, and M1"), Started.Num() == 2 && Started[0] == Need(A, false, true) && Started[1] == Need(M1, true));

	// Cell 2 wants A (a picture), M1 and M2: only M2 is new; A is wanted already, and without collision it adds nothing to what A was wanted as.
	Use.Acquire(2, { Need(A, false, false), Need(M1, true), Need(M2, true) }, Started);
	TestEqual(TEXT("cell 2 starts only M2"), Started.Num(), 1);
	TestTrue(TEXT("it is M2"), Started.Num() == 1 && Started[0] == Need(M2, true));
	TestEqual(TEXT("A has two users"), Use.Users(A, false), 2);
	TestEqual(TEXT("of whom one needs collision"), Use.CollisionUsers(A), 1);
	TestEqual(TEXT("M1 has two"), Use.Users(M1, true), 2);
	TestEqual(TEXT("M2 has one"), Use.Users(M2, true), 1);
	TestEqual(TEXT("a mesh and a material of one id are different things: A is no material"), Use.Users(A, true), 0);
	TestEqual(TEXT("one mesh is wanted"), Use.DistinctMeshes(), 1);
	TestEqual(TEXT("two materials are wanted"), Use.DistinctMaterials(), 2);
	TestEqual(TEXT("two cells hold"), Use.HolderCount(), 2);

	// Cell 1 goes: nothing is left without a user.
	Use.Release(1, Ended);
	TestTrue(TEXT("cell 1's release ends nothing"), Ended.IsEmpty());
	TestEqual(TEXT("A has one user"), Use.Users(A, false), 1);
	TestEqual(TEXT("and none of them needs collision any more"), Use.CollisionUsers(A), 0);
	TestEqual(TEXT("M1 has one"), Use.Users(M1, true), 1);

	// Cell 2 goes: the last user of each. In the order cell 2 took them: A, M1, M2.
	Use.Release(2, Ended);
	TestEqual(TEXT("cell 2's release ends all three"), Ended.Num(), 3);
	TestTrue(TEXT("A, M1, M2"), Ended.Num() == 3 && Ended[0].Id == A && !Ended[0].bMaterial && Ended[1].Id == M1 && Ended[1].bMaterial && Ended[2].Id == M2);
	TestEqual(TEXT("nothing is wanted"), Use.DistinctMeshes() + Use.DistinctMaterials() + Use.HolderCount(), 0);

	// A release of a holder that holds nothing, and a second release of one that went, change nothing.
	Use.Release(2, Ended);
	TestTrue(TEXT("a holder released twice ends nothing the second time"), Ended.IsEmpty());
	Use.Release(99, Ended);
	TestTrue(TEXT("a holder that never held ends nothing"), Ended.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellAssetCollisionTest,
	"Mantle.Runtime.CellStream.AssetUse.CollisionArrivesLater",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellAssetCollisionTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellAssetUseTestPrivate;
	const FString A = Id(0xa), B = Id(0xb);
	FMantleCellAssetUse Use;
	TArray<FMantleCellAssetNeed> Started;
	TArray<FMantleCellAssetNeed> Ended;

	// A picture cell wants A first; a solid cell wants it after: the mesh must now be had with collision, so it starts again, flagged.
	Use.Acquire(1, { Need(A, false, false) }, Started);
	TestTrue(TEXT("the picture cell starts A without collision"), Started.Num() == 1 && !Started[0].bCollision);
	Use.Acquire(2, { Need(A, false, true) }, Started);
	TestTrue(TEXT("the solid cell starts A again, with collision"), Started.Num() == 1 && Started[0].Id == A && Started[0].bCollision);
	// A second solid cell adds nothing: collision was wanted already.
	Use.Acquire(3, { Need(A, false, true) }, Started);
	TestTrue(TEXT("a second solid cell starts nothing"), Started.IsEmpty());
	TestEqual(TEXT("two cells need collision"), Use.CollisionUsers(A), 2);
	TestEqual(TEXT("three use A"), Use.Users(A, false), 3);

	// A holder asked twice for the same cell holds once: the second call replaces the first, giving back what it dropped.
	Use.Acquire(1, { Need(B, false, false) }, Started);
	TestTrue(TEXT("the replaced cell starts B"), Started.Num() == 1 && Started[0].Id == B);
	TestEqual(TEXT("A lost one user"), Use.Users(A, false), 2);
	TestEqual(TEXT("B has one"), Use.Users(B, false), 1);
	// An id twice in one holder's list counts once.
	Use.Acquire(4, { Need(B, false, false), Need(B, false, false) }, Started);
	TestEqual(TEXT("B has two users, not three"), Use.Users(B, false), 2);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
