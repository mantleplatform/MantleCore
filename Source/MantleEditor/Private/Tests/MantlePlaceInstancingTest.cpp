// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCheck.h"
#include "MantleHeadlessPublish.h"
#include "MantlePlaceExporter.h"
#include "MantlePlaceInstancing.h"
#include "MantlePlaceParser.h"
#include "MantlePlacePublishSession.h"
#include "MantlePlaceWorldPartition.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// GhzGarage/mantle#818, size and density: repeated static meshes become instance sets with their transforms kept, a place over
// the instance or file limit is thinned evenly (exact count, same result every time, only when asked), the place file is
// condensed and still parses, a too-big place is refused before anything is uploaded, and Mantle.Check ends in one greppable line.
// All on built places, with no level and no network.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantlePlaceInstancingTestPrivate
{
	const TCHAR* const MeshA = TEXT("ast_u0123456789abcdef0123456789abcdef");
	const TCHAR* const MeshB = TEXT("ast_u11111111111111111111111111111111");
	const TCHAR* const MaterialA = TEXT("ast_u22222222222222222222222222222222");

	FMantleEntity MakeMesh(const FString& Id, const TCHAR* Mesh, const FVector& Location, const TCHAR* Material = nullptr)
	{
		FMantleStaticMeshProps Props;
		Props.Mesh = Mesh;
		if (Material)
		{
			Props.Materials.Add(Material);
		}
		FMantleEntity Entity;
		Entity.Id = Id;
		Entity.Name = Id;
		Entity.Type = EMantleEntityType::StaticMesh;
		Entity.Transform = FTransform(FRotator(0.0, Location.X * 0.5, 0.0), Location, FVector(1.0 + Location.Y * 0.001));
		Entity.Props.Set<FMantleStaticMeshProps>(MoveTemp(Props));
		return Entity;
	}

	/** An InstancedMesh entity of Count instances 100 cm apart along X from StartX. */
	FMantleEntity MakeInstanced(const FString& Id, const TCHAR* Mesh, const int32 Count, const double StartX)
	{
		FMantleInstancedMeshProps Props;
		Props.Mesh.Mesh = Mesh;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Props.Instances.Add(FTransform(FQuat::Identity, FVector(StartX + Index * 100.0, 0.0, 0.0), FVector::OneVector));
		}
		FMantleEntity Entity;
		Entity.Id = Id;
		Entity.Name = Id;
		Entity.Type = EMantleEntityType::InstancedMesh;
		Entity.Props.Set<FMantleInstancedMeshProps>(MoveTemp(Props));
		return Entity;
	}

	FMantlePlace MakePlace()
	{
		FMantlePlace Place;
		Place.SchemaVersion = MANTLE_PLACE_SCHEMA_VERSION;
		Place.PlaceId = TEXT("plc_instancing_test");
		Place.Revision.Number = 1;
		Place.Revision.State = EMantleRevisionState::Draft;
		Place.Metadata.Name = TEXT("Instancing test");
		Place.Metadata.AuthorId = TEXT("usr_test");
		return Place;
	}

	int32 CountType(const FMantlePlace& Place, const EMantleEntityType Type)
	{
		int32 Count = 0;
		for (const FMantleEntity& Entity : Place.Entities)
		{
			Count += Entity.Type == Type ? 1 : 0;
		}
		return Count;
	}

	/** The X of each instance of the InstancedMesh entity with Id, in order. */
	TArray<double> InstanceXs(const FMantlePlace& Place, const FString& Id)
	{
		TArray<double> Xs;
		for (const FMantleEntity& Entity : Place.Entities)
		{
			if (const FMantleInstancedMeshProps* Props = Entity.Props.TryGet<FMantleInstancedMeshProps>(); Props && Entity.Id == Id)
			{
				for (const FTransform& Instance : Props->Instances)
				{
					Xs.Add(Instance.GetLocation().X);
				}
			}
		}
		return Xs;
	}

	int64 InstancesOf(const FMantlePlace& Place, const FString& Id)
	{
		return InstanceXs(Place, Id).Num();
	}

	/** A place with Foliage painted instances and Props prop instances, as the exporter leaves them before thinning. */
	FMantlePlace MakeBigPlace(const int32 Foliage, const int32 Props, TSet<FString>& OutFoliageIds)
	{
		FMantlePlace Place = MakePlace();
		Place.Entities.Add(MakeInstanced(TEXT("ent_ism_foliage_0"), MeshA, Foliage, 0.0));
		Place.Entities.Add(MakeInstanced(TEXT("ent_ism_props_0"), MeshB, Props, 0.0));
		OutFoliageIds.Add(TEXT("ent_ism_foliage_0"));
		return Place;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceInstancingGroupTest,
	"Mantle.Editor.PlaceInstancing.IdenticalStaticMeshesBecomeOneInstanceSet",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceInstancingGroupTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceInstancingTestPrivate;
	FMantlePlace Place = MakePlace();
	// Five of the same mesh and material, a different material of the same mesh, and a mesh with only two twins.
	for (int32 Index = 0; Index < 5; ++Index)
	{
		Place.Entities.Add(MakeMesh(FString::Printf(TEXT("ent_a%d"), Index), MeshA, FVector(Index * 250.0, Index * 40.0, 10.0), MaterialA));
	}
	Place.Entities.Add(MakeMesh(TEXT("ent_other_material"), MeshA, FVector(5.0, 5.0, 5.0)));
	Place.Entities.Add(MakeMesh(TEXT("ent_b0"), MeshB, FVector(1.0, 2.0, 3.0)));
	Place.Entities.Add(MakeMesh(TEXT("ent_b1"), MeshB, FVector(4.0, 5.0, 6.0)));

	TArray<FMantleInstancedMeshGroup> Groups;
	const FMantleGroupReport Report = FMantlePlaceInstancing::GroupStaticMeshes(Place, Groups);

	TestEqual(TEXT("five actors are folded in"), Report.Actors, 5);
	TestEqual(TEXT("into one set"), Report.Sets, 1);
	TestEqual(TEXT("the group holds five instances"), Groups.Num() == 1 ? Groups[0].WorldInstances.Num() : -1, 5);
	TestEqual(TEXT("the other material and the pair stay as entities"), CountType(Place, EMantleEntityType::StaticMesh), 3);

	if (Groups.Num() == 1)
	{
		TestFalse(TEXT("props are not what thinning takes first"), Groups[0].bThinFirst);
		// Every transform is the actor's, in entity order.
		for (int32 Index = 0; Index < 5; ++Index)
		{
			const FTransform Expected = MakeMesh(TEXT("x"), MeshA, FVector(Index * 250.0, Index * 40.0, 10.0)).Transform;
			TestTrue(*FString::Printf(TEXT("instance %d keeps its location"), Index), Groups[0].WorldInstances[Index].GetLocation().Equals(Expected.GetLocation(), 1.0e-9));
			TestTrue(*FString::Printf(TEXT("instance %d keeps its rotation"), Index), Groups[0].WorldInstances[Index].GetRotation().Equals(Expected.GetRotation(), 1.0e-9));
			TestTrue(*FString::Printf(TEXT("instance %d keeps its scale"), Index), Groups[0].WorldInstances[Index].GetScale3D().Equals(Expected.GetScale3D(), 1.0e-9));
		}

		// Cut into entities, each instance is at its place again: the anchor plus where the entity puts it.
		const TArray<FMantleEntity> Chunked = FMantlePlaceExporter::ChunkInstancedMeshes(Groups, FMantlePlaceLimits::MaxInstancesPerEntity);
		TestEqual(TEXT("one entity"), Chunked.Num(), 1);
		if (Chunked.Num() == 1)
		{
			const FMantleInstancedMeshProps* Props = Chunked[0].Props.TryGet<FMantleInstancedMeshProps>();
			if (TestNotNull(TEXT("an InstancedMesh"), Props))
			{
				int32 Matched = 0;
				for (int32 Index = 0; Index < 5; ++Index)
				{
					const FVector Want = MakeMesh(TEXT("x"), MeshA, FVector(Index * 250.0, Index * 40.0, 10.0)).Transform.GetLocation();
					for (const FTransform& Instance : Props->Instances)
					{
						Matched += (Instance.GetLocation() + Chunked[0].Transform.GetLocation()).Equals(Want, 1.0e-6) ? 1 : 0;
					}
				}
				TestEqual(TEXT("all five are where the actors were"), Matched, 5);
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceInstancingGroupLeavesTest,
	"Mantle.Editor.PlaceInstancing.NamedTaggedAndPhysicsMeshesStayEntities",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceInstancingGroupLeavesTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceInstancingTestPrivate;
	FMantlePlace Place = MakePlace();
	for (int32 Index = 0; Index < 8; ++Index)
	{
		Place.Entities.Add(MakeMesh(FString::Printf(TEXT("ent_m%d"), Index), MeshA, FVector(Index * 100.0, 0.0, 0.0)));
	}
	Place.Entities[0].Tags.Add(TEXT("door"));
	Place.Entities[1].bDynamic = true;
	Place.Entities[2].Parent = TEXT("ent_m3");
	// ent_m3 is a parent now, and ent_m4 is a joint's end.
	FMantleConstraintProps Joint;
	Joint.EntityA = TEXT("ent_m4");
	FMantleEntity JointEntity;
	JointEntity.Id = TEXT("ent_joint");
	JointEntity.Type = EMantleEntityType::Constraint;
	JointEntity.Props.Set<FMantleConstraintProps>(MoveTemp(Joint));
	Place.Entities.Add(MoveTemp(JointEntity));
	FMantlePhysicsProps Physics;
	Place.Entities[5].Props.Get<FMantleStaticMeshProps>().Physics = Physics;

	TArray<FMantleInstancedMeshGroup> Groups;
	const FMantleGroupReport Report = FMantlePlaceInstancing::GroupStaticMeshes(Place, Groups);

	// Tagged (0), dynamic (1), child (2), parent (3), joint end (4) and physics (5) stay; only m6 and m7 are plain, below the minimum.
	TestEqual(TEXT("nothing is folded in: two plain twins are below the minimum"), Report.Actors, 0);
	TestEqual(TEXT("so every mesh is still an entity"), CountType(Place, EMantleEntityType::StaticMesh), 8);
	TestEqual(TEXT("and no group was made"), Groups.Num(), 0);

	// With enough plain twins they do group, and the special ones still stay.
	for (int32 Index = 8; Index < 14; ++Index)
	{
		Place.Entities.Add(MakeMesh(FString::Printf(TEXT("ent_m%d"), Index), MeshA, FVector(Index * 100.0, 0.0, 0.0)));
	}
	const FMantleGroupReport Second = FMantlePlaceInstancing::GroupStaticMeshes(Place, Groups);
	TestEqual(TEXT("the eight plain ones group"), Second.Actors, 8);
	TestEqual(TEXT("the six special ones stay"), CountType(Place, EMantleEntityType::StaticMesh), 6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceInstancingGroupMergesTest,
	"Mantle.Editor.PlaceInstancing.GroupedMeshesJoinTheInstancesOfTheSameMesh",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceInstancingGroupMergesTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceInstancingTestPrivate;
	FMantlePlace Place = MakePlace();
	for (int32 Index = 0; Index < 4; ++Index)
	{
		Place.Entities.Add(MakeMesh(FString::Printf(TEXT("ent_m%d"), Index), MeshA, FVector(Index * 100.0, 0.0, 0.0)));
	}
	// Painted foliage of the same mesh already made a group, which thinning would take from first.
	TArray<FMantleInstancedMeshGroup> Groups;
	FMantleInstancedMeshGroup& Existing = Groups.AddDefaulted_GetRef();
	Existing.Mesh.Mesh = MeshA;
	Existing.MeshName = TEXT("Tree");
	Existing.bThinFirst = true;
	Existing.WorldInstances.Add(FTransform(FVector(9000.0, 0.0, 0.0)));

	const FMantleGroupReport Report = FMantlePlaceInstancing::GroupStaticMeshes(Place, Groups);
	TestEqual(TEXT("four actors fold in"), Report.Actors, 4);
	TestEqual(TEXT("into the existing group, not a second one"), Groups.Num(), 1);
	TestEqual(TEXT("which now holds all five"), Groups[0].WorldInstances.Num(), 5);
	TestFalse(TEXT("and is no longer foliage alone"), Groups[0].bThinFirst);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceInstancingEvenKeepTest,
	"Mantle.Editor.PlaceInstancing.EvenKeepMaskKeepsExactlyTheCountSpreadEvenly",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceInstancingEvenKeepTest::RunTest(const FString& Parameters)
{
	const int64 Counts[] = { 1, 7, 100, 1000, 4096, 99991 };
	for (const int64 Count : Counts)
	{
		for (const int64 Keep : { int64(0), int64(1), Count / 3, Count / 2, Count - 1, Count })
		{
			const TArray<bool> Mask = FMantlePlaceInstancing::EvenKeepMask(Count, Keep);
			int64 Kept = 0;
			int64 LongestGap = 0;
			int64 Gap = 0;
			for (const bool bKept : Mask)
			{
				if (bKept)
				{
					++Kept;
					Gap = 0;
				}
				else
				{
					LongestGap = FMath::Max(LongestGap, ++Gap);
				}
			}
			TestEqual(*FString::Printf(TEXT("keeps exactly %lld of %lld"), Keep, Count), Kept, Keep);
			if (Keep > 0)
			{
				// Even: no run of removed items is longer than the spacing between kept ones, plus one.
				TestTrue(*FString::Printf(TEXT("keeping %lld of %lld leaves no big hole"), Keep, Count), LongestGap <= Count / Keep + 1);
			}
			TestTrue(TEXT("and the same ones every time"), Mask == FMantlePlaceInstancing::EvenKeepMask(Count, Keep));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceInstancingThinCountTest,
	"Mantle.Editor.PlaceInstancing.ThinningRemovesFoliageFirstToTheExactCount",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceInstancingThinCountTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceInstancingTestPrivate;
	TSet<FString> Foliage;
	FMantlePlace Place = MakeBigPlace(1000, 200, Foliage);
	const int64 NoByteLimit = 1ll << 40;

	// 1,200 instances against a limit of 700: 500 go, all from the foliage.
	const FMantleThinReport Preview = FMantlePlaceInstancing::ThinToFit(Place, Foliage, 700, NoByteLimit, /*bApply*/ false);
	TestFalse(TEXT("a preview applies nothing"), Preview.bApplied);
	TestEqual(TEXT("and the place is as it was"), FMantlePlaceInstancing::CountInstances(Place), static_cast<int64>(1200));
	TestEqual(TEXT("it says 500 would go"), Preview.Removed(), static_cast<int64>(500));
	TestTrue(TEXT("and that it fits"), Preview.bFits);

	FMantlePlace Copy = Place;
	const FMantleThinReport Report = FMantlePlaceInstancing::ThinToFit(Place, Foliage, 700, NoByteLimit, /*bApply*/ true);
	TestTrue(TEXT("applied"), Report.bApplied);
	TestEqual(TEXT("exactly 700 are left"), FMantlePlaceInstancing::CountInstances(Place), static_cast<int64>(700));
	TestEqual(TEXT("the report counts them"), Report.InstancesAfter, static_cast<int64>(700));
	TestEqual(TEXT("all from the foliage"), Report.FoliageRemoved(), static_cast<int64>(500));
	TestEqual(TEXT("the props are untouched"), InstancesOf(Place, TEXT("ent_ism_props_0")), static_cast<int64>(200));
	TestEqual(TEXT("the foliage keeps 500"), InstancesOf(Place, TEXT("ent_ism_foliage_0")), static_cast<int64>(500));
	TestTrue(TEXT("the description says how many"), Report.Describe().Contains(TEXT("500")));

	// Even across the level: the kept foliage reaches both ends and has no big hole.
	const TArray<double> Xs = InstanceXs(Place, TEXT("ent_ism_foliage_0"));
	if (Xs.Num() == 500)
	{
		TestTrue(TEXT("the first stretch is still planted"), Xs[0] < 300.0);
		TestTrue(TEXT("and so is the last"), Xs.Last() > 999.0 * 100.0 - 300.0);
		double Widest = 0.0;
		for (int32 Index = 1; Index < Xs.Num(); ++Index)
		{
			Widest = FMath::Max(Widest, Xs[Index] - Xs[Index - 1]);
		}
		TestTrue(TEXT("with no gap wider than every second one"), Widest <= 300.0);
	}

	// The same input gives the same output.
	const FMantleThinReport Again = FMantlePlaceInstancing::ThinToFit(Copy, Foliage, 700, NoByteLimit, /*bApply*/ true);
	TestEqual(TEXT("thinning twice removes the same number"), Again.Removed(), Report.Removed());
	TestTrue(TEXT("and keeps the same instances"), InstanceXs(Copy, TEXT("ent_ism_foliage_0")) == Xs);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceInstancingThinOthersTest,
	"Mantle.Editor.PlaceInstancing.ThinningTakesPropsOnlyWhenFoliageIsNotEnough",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceInstancingThinOthersTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceInstancingTestPrivate;
	TSet<FString> Foliage;
	FMantlePlace Place = MakeBigPlace(100, 1000, Foliage);
	const int64 NoByteLimit = 1ll << 40;

	// 1,100 against 500: 600 must go. Foliage gives up at most 90 of its 100; the props give the other 510.
	const FMantleThinReport Report = FMantlePlaceInstancing::ThinToFit(Place, Foliage, 500, NoByteLimit, /*bApply*/ true);
	TestEqual(TEXT("exactly 500 are left"), FMantlePlaceInstancing::CountInstances(Place), static_cast<int64>(500));
	TestEqual(TEXT("the foliage lost nine tenths"), Report.FoliageRemoved(), static_cast<int64>(90));
	TestEqual(TEXT("the props lost the rest"), Report.OthersRemoved(), static_cast<int64>(510));
	TestTrue(TEXT("the description names both"), Report.Describe().Contains(TEXT("foliage")) && Report.Describe().Contains(TEXT("other")));

	// More than thinning is allowed to take: it reports that it cannot fit.
	FMantlePlace Tiny = MakeBigPlace(10, 10, Foliage);
	const FMantleThinReport Cannot = FMantlePlaceInstancing::ThinToFit(Tiny, Foliage, 1, NoByteLimit, /*bApply*/ false);
	TestFalse(TEXT("one instance cannot be reached"), Cannot.bFits);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceInstancingThinBytesTest,
	"Mantle.Editor.PlaceInstancing.ThinningCanBeDrivenByTheFileSize",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceInstancingThinBytesTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceInstancingTestPrivate;
	TSet<FString> Foliage;
	FMantlePlace Place = MakeBigPlace(3000, 100, Foliage);
	const int64 Before = FMantlePlaceInstancing::PlaceBytes(Place);
	const int64 Limit = Before / 2;

	const FMantleThinReport Report = FMantlePlaceInstancing::ThinToFit(Place, Foliage, 100000, Limit, /*bApply*/ true);
	TestTrue(TEXT("it fits the file limit"), Report.bFits);
	TestTrue(TEXT("the file is within it"), FMantlePlaceInstancing::PlaceBytes(Place) <= Limit);
	TestEqual(TEXT("the report holds the size it measured"), Report.BytesAfter, FMantlePlaceInstancing::PlaceBytes(Place));
	TestTrue(TEXT("instances went, though the count was far under its limit"), Report.RemovesAnything());

	// Nothing over, nothing done.
	FMantlePlace Small = MakeBigPlace(10, 10, Foliage);
	const FMantleThinReport Untouched = FMantlePlaceInstancing::ThinToFit(Small, Foliage, 100000, 1ll << 40, /*bApply*/ true);
	TestFalse(TEXT("a place inside both limits is left alone"), Untouched.RemovesAnything());
	TestEqual(TEXT("with all its instances"), FMantlePlaceInstancing::CountInstances(Small), static_cast<int64>(20));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceInstancingOverBudgetTextTest,
	"Mantle.Editor.PlaceInstancing.TheRefusalSaysWhatThinningWouldDo",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceInstancingOverBudgetTextTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("inside both limits there is nothing to say"), FMantlePlaceInstancing::DescribeOverBudget(10, 100, 10, 100, FMantleThinReport()).IsEmpty());

	FMantleThinReport Thin;
	Thin.InstancesBefore = 106964;
	Thin.InstancesAfter = 50000;
	Thin.FoliageBefore = 106964;
	Thin.FoliageAfter = 50000;
	Thin.BytesBefore = 11 * 1024 * 1024;
	Thin.BytesAfter = 6 * 1024 * 1024;
	Thin.bFits = true;
	const FString OverFile = FMantlePlaceInstancing::DescribeOverBudget(106964, 100000, 11 * 1024 * 1024, 8 * 1024 * 1024, Thin);
	TestTrue(TEXT("it names the file size and the limit"), OverFile.Contains(TEXT("11.0 MiB")) && OverFile.Contains(TEXT("8.0 MiB")));
	TestTrue(TEXT("it offers even thinning with the count that goes"), OverFile.Contains(TEXT("thinning")) && OverFile.Contains(TEXT("56,964")));
	TestTrue(TEXT("and says nothing is removed unless chosen"), OverFile.Contains(TEXT("Nothing is removed unless you choose it")));

	const FString NoInstances = FMantlePlaceInstancing::DescribeOverBudget(0, 100000, 9 * 1024 * 1024, 8 * 1024 * 1024, FMantleThinReport());
	TestTrue(TEXT("with no instances to thin, it points at merging objects or splitting the world"), NoInstances.Contains(TEXT("no instances to thin")) && NoInstances.Contains(TEXT("split")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceInstancingCondensedTest,
	"Mantle.Editor.PlaceInstancing.ThePlaceFileIsCondensedAndStillParses",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceInstancingCondensedTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceInstancingTestPrivate;
	FMantlePlace Place = MakePlace();
	FMantleAssetRef Asset;
	Asset.Id = MeshA;
	Asset.Kind = EMantleAssetKind::StaticMesh;
	Asset.Hash = TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000001");
	Asset.Bytes = 1234;
	Place.Assets.Add(Asset);
	Place.Entities.Add(MakeInstanced(TEXT("ent_ism_a_0"), MeshA, 50, 0.0));

	const FString Json = FMantlePlaceExporter::ToJson(Place);
	TestFalse(TEXT("no line breaks"), Json.Contains(TEXT("\n")) || Json.Contains(TEXT("\r")));
	TestFalse(TEXT("no tabs"), Json.Contains(TEXT("\t")));
	TestFalse(TEXT("no space after a colon"), Json.Contains(TEXT("\": ")));

	const FMantleParseResult Parsed = FMantlePlaceParser::ParseFromString(Json);
	if (!TestTrue(TEXT("the condensed file parses"), Parsed.bSuccess))
	{
		AddError(Parsed.ErrorSummary());
		return false;
	}
	TestEqual(TEXT("with its entity"), Parsed.Place.Entities.Num(), 1);
	const FMantleInstancedMeshProps* Props = Parsed.Place.Entities.Num() == 1 ? Parsed.Place.Entities[0].Props.TryGet<FMantleInstancedMeshProps>() : nullptr;
	if (TestNotNull(TEXT("an InstancedMesh"), Props))
	{
		TestEqual(TEXT("and all its instances"), Props->Instances.Num(), 50);
		TestTrue(TEXT("where they were"), Props->Instances.Num() == 50 && Props->Instances[7].GetLocation().Equals(FVector(700.0, 0.0, 0.0), 0.01));
	}

	// Condensed is smaller than the 8 MiB-hungry indented form would be: well under two thirds of the instances' share, say.
	TestTrue(TEXT("and it is small"), Json.Len() < 50 * 60 + 700);
	return true;
}

namespace MantlePlaceInstancingTestPrivate
{
	/** Counts every call the session sends. */
	class FCountingTransport : public IMantleUploadTransport
	{
	public:
		int32 Calls = 0;

		virtual void Send(const FMantleUploadCallRequest& Request, FMantleApiCallback OnDone) override
		{
			++Calls;
		}
	};

	/** A level source whose place is too big (or not), with nothing to convert. */
	class FSizedSource : public IMantlePlacePublishSource
	{
	public:
		bool bOver = true;
		/** Findings Prepare hands the session, as the level's check would. */
		TArray<FMantleValidationIssue> PrepareIssues;
		int32 SizeChecks = 0;
		int32 ConvertCalls = 0;
		int32 ExportCalls = 0;

		virtual FString GetLevelName() const override { return TEXT("BigLevel"); }
		virtual bool Prepare(FMantleValidationResult& OutValidation, FString& OutMessage) override
		{
			OutValidation.Issues.Append(PrepareIssues);
			return true;
		}

		virtual bool CheckPlaceSize(FMantlePlaceSizeCheck& OutCheck) override
		{
			++SizeChecks;
			OutCheck.Bytes = 11 * 1024 * 1024;
			OutCheck.Instances = 106964;
			if (!bOver)
			{
				return true;
			}
			OutCheck.bOver = true;
			OutCheck.Issues.Add({ EMantleIssueSeverity::Error, FString(), TEXT("The place file would be 11.0 MiB; Mantle accepts at most 8.0 MiB."), FString() });
			OutCheck.ThinOffer = TEXT("Thinning would remove 56,964 foliage instances.");
			return false;
		}

		virtual int32 NumConversionSteps() const override { return 1; }
		virtual FString ConversionStepName(int32 Step) const override { return TEXT("Step"); }
		virtual void Convert(int32 Step, FMantlePlaceAssetSet& Set) override { ++ConvertCalls; }
		virtual bool CollectScripts(TArray<FMantlePlaceScriptUpload>& OutScripts, FString& OutMessage) override
		{
			OutScripts.Reset();
			return true;
		}
		virtual EMantlePlaceExportOutcome Export(const FMantlePlaceAssetSet& Set, const TMap<FString, FString>& ScriptSourceIds,
			const TMap<FString, FString>& ScriptAuthorSourceIds, FString& OutPlaceJson, TArray<FMantleValidationIssue>& OutIssues) override
		{
			++ExportCalls;
			OutPlaceJson = TEXT("{}");
			return EMantlePlaceExportOutcome::Exported;
		}
		virtual FString FindPlaceId() override { return TEXT("plc_00000000000000000000000000000001"); }
		virtual void RememberPlaceId(const FString& PlaceId) override {}
		virtual void ForgetPlaceId() override {}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceInstancingRefusedBeforeUploadTest,
	"Mantle.Editor.PlaceInstancing.ATooBigPlaceIsRefusedBeforeAnythingIsConvertedOrSent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceInstancingRefusedBeforeUploadTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceInstancingTestPrivate;
	double Now = 1000.0;
	const TSharedRef<FSizedSource> Source = MakeShared<FSizedSource>();
	const TSharedRef<FCountingTransport> Transport = MakeShared<FCountingTransport>();
	const TSharedRef<FMantlePlacePublishSession> Session = MakeShared<FMantlePlacePublishSession>(
		Source, Transport, /*bGoLive*/ true, FString(), [&Now] { return Now; });
	Session->Start();

	TestEqual(TEXT("the publish stopped at the check"), static_cast<int32>(Session->GetPhase()), static_cast<int32>(EMantlePublishPhase::Failed));
	TestEqual(TEXT("the size was checked once"), Source->SizeChecks, 1);
	TestEqual(TEXT("nothing was converted"), Source->ConvertCalls, 0);
	TestEqual(TEXT("nothing was exported"), Source->ExportCalls, 0);
	TestEqual(TEXT("and not one call went to Mantle"), Transport->Calls, 0);

	const FMantlePublishResult& Result = Session->GetResult();
	TestTrue(TEXT("the message says it is too big and that nothing was uploaded"), Result.Message.Contains(TEXT("too big")) && Result.Message.Contains(TEXT("Nothing was prepared or uploaded")));
	TestTrue(TEXT("with the sizes"), Result.Message.Contains(TEXT("11.0 MiB")));
	TestTrue(TEXT("the check lists the reason as an error"), Result.Validation.HasErrors());
	TestEqual(TEXT("thinning is on offer, with its numbers"), Result.ThinOffer, FString(TEXT("Thinning would remove 56,964 foliage instances.")));

	// A place that fits goes on past the check.
	const TSharedRef<FSizedSource> Fitting = MakeShared<FSizedSource>();
	Fitting->bOver = false;
	const TSharedRef<FMantlePlacePublishSession> Next = MakeShared<FMantlePlacePublishSession>(
		Fitting, MakeShared<FCountingTransport>(), /*bGoLive*/ true, FString(), [&Now] { return Now; });
	Next->Start();
	TestEqual(TEXT("it is checked as well"), Fitting->SizeChecks, 1);
	TestFalse(TEXT("and is not refused for its size"), Next->GetResult().Message.Contains(TEXT("too big")));
	TestTrue(TEXT("with no thinning offered"), Next->GetResult().ThinOffer.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceInstancingOfferWithBudgetErrorTest,
	"Mantle.Editor.PlaceInstancing.ThinOfferIsMadeWhenInstancesAreTheOnlyError",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceInstancingOfferWithBudgetErrorTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceInstancingTestPrivate;
	double Now = 1000.0;
	FMantleValidationIssue Budget{ EMantleIssueSeverity::Error, FString(), TEXT("This level has 106,964 instanced meshes."), FString(), true };

	// The check's own instance-count error is the only one: the size check runs and the offer is made.
	const TSharedRef<FSizedSource> Only = MakeShared<FSizedSource>();
	Only->PrepareIssues.Add(Budget);
	const TSharedRef<FMantlePlacePublishSession> OnlySession = MakeShared<FMantlePlacePublishSession>(
		Only, MakeShared<FCountingTransport>(), /*bGoLive*/ true, FString(), [&Now] { return Now; });
	OnlySession->Start();
	TestEqual(TEXT("the size was checked"), Only->SizeChecks, 1);
	TestEqual(TEXT("the publish stopped"), static_cast<int32>(OnlySession->GetPhase()), static_cast<int32>(EMantlePublishPhase::Failed));
	TestFalse(TEXT("thinning is on offer"), OnlySession->GetResult().ThinOffer.IsEmpty());
	TestEqual(TEXT("and the instance error is listed once, not twice"), OnlySession->GetResult().Validation.NumErrors(), 1);

	// Another error beside it: no offer, since thinning would not make the place publishable.
	const TSharedRef<FSizedSource> Both = MakeShared<FSizedSource>();
	Both->PrepareIssues.Add(Budget);
	Both->PrepareIssues.Add({ EMantleIssueSeverity::Error, TEXT("Lamp"), TEXT("has a scale of 0."), FString() });
	const TSharedRef<FMantlePlacePublishSession> BothSession = MakeShared<FMantlePlacePublishSession>(
		Both, MakeShared<FCountingTransport>(), /*bGoLive*/ true, FString(), [&Now] { return Now; });
	BothSession->Start();
	TestEqual(TEXT("the size was not checked"), Both->SizeChecks, 0);
	TestEqual(TEXT("the publish stopped"), static_cast<int32>(BothSession->GetPhase()), static_cast<int32>(EMantlePublishPhase::Failed));
	TestTrue(TEXT("with no thinning offered"), BothSession->GetResult().ThinOffer.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCheckCommandTest,
	"Mantle.Editor.PlaceInstancing.CheckEndsInOneGreppableLine",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCheckCommandTest::RunTest(const FString& Parameters)
{
	// The arguments.
	const FMantleCheck::FArgs None = FMantleCheck::ParseArgs({});
	TestTrue(TEXT("no arguments: nothing exported, nothing thinned, no error"), None.ExportDir.IsEmpty() && !None.bThin && None.Error.IsEmpty());
	const FMantleCheck::FArgs Both = FMantleCheck::ParseArgs({ TEXT("-Export=D:/Checks/Level"), TEXT("-Thin=auto") });
	TestEqual(TEXT("-Export names the folder"), Both.ExportDir, FString(TEXT("D:/Checks/Level")));
	TestTrue(TEXT("-Thin=auto asks for thinning"), Both.bThin);
	TestFalse(TEXT("-Thin=off does not"), FMantleCheck::ParseArgs({ TEXT("-Thin=off") }).bThin);
	TestFalse(TEXT("-Thin=lots is refused"), FMantleCheck::ParseArgs({ TEXT("-Thin=lots") }).Error.IsEmpty());
	TestFalse(TEXT("an empty -Export is refused"), FMantleCheck::ParseArgs({ TEXT("-Export=") }).Error.IsEmpty());
	TestFalse(TEXT("so is a word it does not know"), FMantleCheck::ParseArgs({ TEXT("publish") }).Error.IsEmpty());

	// A report with one error, two warnings and a place of 7,340,032 bytes and 61,000 instances.
	FMantleCheckReport Report;
	Report.Issues.Add({ EMantleIssueSeverity::Warning, TEXT("Lamp"), TEXT("has a dynamic light."), TEXT("Fine.") });
	Report.Issues.Add({ EMantleIssueSeverity::Error, FString(), TEXT("This level\nhas   no player start."), TEXT("Add one.") });
	Report.Issues.Add({ EMantleIssueSeverity::Warning, FString(), TEXT("Loaded 12 World Partition actors."), FString() });
	Report.PlaceBytes = 7340032;
	Report.Instances = 61000;
	Report.GroupedActors = 5;
	Report.ThinnedInstances = 0;
	Report.DrawDistances = 12;

	TestFalse(TEXT("an error means not ready"), Report.IsReady());
	TestEqual(TEXT("the line"), FMantleCheck::DoneLine(Report),
		FString(TEXT("MANTLE_CHECK_DONE ready=0 errors=1 warnings=2 placeBytes=7340032 instances=61000 grouped=5 thinned=0 drawDistances=12")));
	const TArray<FString> Lines = FMantleCheck::IssueLines(Report);
	if (TestEqual(TEXT("one line for each finding"), Lines.Num(), 3))
	{
		TestTrue(TEXT("a warning"), Lines[0].StartsWith(TEXT("MANTLE_CHECK_WARNING ")) && Lines[0].Contains(TEXT("Lamp")));
		TestEqual(TEXT("an error, on one line"), Lines[1], FString(TEXT("MANTLE_CHECK_ERROR Error: This level has no player start. Add one.")));
	}

	Report.Issues.RemoveAt(1);
	TestTrue(TEXT("with no error it is ready"), Report.IsReady());
	TestTrue(TEXT("and says so"), FMantleCheck::DoneLine(Report).StartsWith(TEXT("MANTLE_CHECK_DONE ready=1 errors=0 warnings=2 ")));

	// Without a level there is nothing to check, and it says so on that line too.
	const FMantleCheckReport NoLevel = FMantleCheck::Evaluate(nullptr, false);
	TestFalse(TEXT("no level is not ready"), NoLevel.IsReady());
	TestTrue(TEXT("with its line"), FMantleCheck::DoneLine(NoLevel).StartsWith(TEXT("MANTLE_CHECK_DONE ready=0 errors=1 ")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleHeadlessThinArgsTest,
	"Mantle.Editor.PlaceInstancing.HeadlessPublishTakesThinAuto",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleHeadlessThinArgsTest::RunTest(const FString& Parameters)
{
	const FMantleHeadlessPublish::FArgs Plain = FMantleHeadlessPublish::ParseArgs({ TEXT("A"), TEXT("note") });
	TestFalse(TEXT("thinning is off unless asked for"), Plain.bThin);
	TestEqual(TEXT("the notes are as before"), Plain.Notes, FString(TEXT("A note")));

	const FMantleHeadlessPublish::FArgs Thin = FMantleHeadlessPublish::ParseArgs({ TEXT("-Thin=auto"), TEXT("-TimeoutMinutes=5"), TEXT("Second"), TEXT("pass") });
	TestTrue(TEXT("-Thin=auto asks for it"), Thin.bThin);
	TestEqual(TEXT("the options come in either order"), Thin.TimeoutSeconds, 300.0);
	TestEqual(TEXT("and the notes follow them"), Thin.Notes, FString(TEXT("Second pass")));
	TestTrue(TEXT("the other order too"), FMantleHeadlessPublish::ParseArgs({ TEXT("-TimeoutMinutes=5"), TEXT("-Thin=auto") }).bThin);

	TestFalse(TEXT("an unknown thin mode is refused"), FMantleHeadlessPublish::ParseArgs({ TEXT("-Thin=some") }).Error.IsEmpty());
	TestFalse(TEXT("a bad timeout still is"), FMantleHeadlessPublish::ParseArgs({ TEXT("-TimeoutMinutes=0") }).Error.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWorldPartitionLoadNoteTest,
	"Mantle.Editor.PlaceInstancing.WorldPartitionLoadIsDescribedWithTheMemory",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWorldPartitionLoadNoteTest::RunTest(const FString& Parameters)
{
	FMantlePlaceWorldPartition::FLoadResult Loaded;
	Loaded.UnloadedBefore = 1200;
	Loaded.UnloadedAfter = 0;
	Loaded.CommitBeforeMB = 7100;
	Loaded.CommitAfterMB = 11400;
	const FString Note = FMantlePlaceWorldPartition::DescribeLoad(Loaded);
	TestEqual(TEXT("it counts what was loaded"), Loaded.Loaded(), 1200);
	TestTrue(TEXT("and the note names the count and the memory before and after"), Note.Contains(TEXT("1200")) && Note.Contains(TEXT("11400")) && Note.Contains(TEXT("7100")));
	TestFalse(TEXT("with nothing left out there is no warning of it"), Note.Contains(TEXT("still not loaded")));

	Loaded.UnloadedAfter = 40;
	TestTrue(TEXT("a cell that would not load is said"), FMantlePlaceWorldPartition::DescribeLoad(Loaded).Contains(TEXT("40 actors are still not loaded")));

	// A level with no World Partition (here, none at all) loads nothing and holds nothing.
	const FMantlePlaceWorldPartition::FLoadResult None = FMantlePlaceWorldPartition::LoadAll(nullptr);
	TestFalse(TEXT("no level: nothing held"), None.bLoaded());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
