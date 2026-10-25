// Copyright Joshua Eger. All Rights Reserved.
#include "MantleGarmentOutfit.h"
#include "MantleAvatarStripFixer.h"
#include "MantleAvatarRefitFixer.h"
#include "MantleOutfitSections.h"
#include "MantleSkinnedMesh.h"

#include "Math/RandomStream.h"
#include "ReferenceSkeleton.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// "Make Outfit from garment": posing a garment the way its body was, moving it onto the MetaHuman, and taking the
// MetaHuman's skin weights, on synthetic meshes. No assets.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleGarmentOutfitTestPrivate
{
	/** Two joints: a root at the origin and an arm 10 cm up the Z axis; vertex 0 on the root, vertex 1 on the arm. */
	FMantleGlbMesh GarmentTestMesh()
	{
		FMantleGlbMesh Mesh;
		Mesh.Joints.Add({ FName(TEXT("root")), INDEX_NONE, FTransform::Identity });
		Mesh.Joints.Add({ FName(TEXT("arm")), 0, FTransform(FVector(0, 0, 10)) });
		Mesh.Positions = { FVector3f(1, 0, 0), FVector3f(1, 0, 15) };
		Mesh.Normals = { FVector3f(1, 0, 0), FVector3f(1, 0, 0) };
		Mesh.NumInfluences = 1;
		Mesh.InfluenceJoints = { 0, 1 };
		Mesh.InfluenceWeights = { 1.0f, 1.0f };
		return Mesh;
	}

	/** A Size x Size cm sheet of skin at height Z (1 cm grid, two triangles per square, normals up); Raise lifts points by (x, y). */
	FMantleSkinnedMeshLOD SkinSheet(const int32 Size, const float Z, TFunction<float(float, float)> Raise = nullptr)
	{
		FMantleSkinnedMeshLOD Sheet;
		for (int32 Y = 0; Y <= Size; ++Y)
		{
			for (int32 X = 0; X <= Size; ++X)
			{
				Sheet.Positions.Add(FVector3f(X, Y, Z + (Raise ? Raise(X, Y) : 0.0f)));
				Sheet.Normals.Add(FVector3f(0, 0, 1));
			}
		}
		for (int32 Y = 0; Y < Size; ++Y)
		{
			for (int32 X = 0; X < Size; ++X)
			{
				const uint32 A = Y * (Size + 1) + X;
				const uint32 B = A + 1;
				const uint32 C = A + Size + 1;
				const uint32 D = C + 1;
				Sheet.Indices.Append({ A, B, D, A, D, C });
			}
		}
		return Sheet;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleGarmentOutfitPoseTest,
	"Mantle.Editor.GarmentOutfit.PoseMesh",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleGarmentOutfitPoseTest::RunTest(const FString& Parameters)
{
	using namespace MantleGarmentOutfitTestPrivate;
	FString Error;

	// The bind pose itself, scaled by 2: every vertex twice as far from the origin.
	FMantleGlbMesh Bind = GarmentTestMesh();
	const TArray<FTransform> BindPose = { FTransform::Identity, FTransform(FVector(0, 0, 10)) };
	TestTrue(TEXT("the bind pose poses"), FMantleGarmentOutfit::PoseMesh(Bind, BindPose, 2.0, Error));
	TestTrue(TEXT("root vertex scaled"), Bind.Positions[0].Equals(FVector3f(2, 0, 0), 1e-4f));
	TestTrue(TEXT("arm vertex scaled"), Bind.Positions[1].Equals(FVector3f(2, 0, 30), 1e-4f));

	// The arm turned 90 degrees about Y at its joint: its vertex (1, 0, 5 above the joint) swings to (5, 0, -1) from it.
	FMantleGlbMesh Turned = GarmentTestMesh();
	const TArray<FTransform> TurnedPose = { FTransform::Identity, FTransform(FQuat(FVector::YAxisVector, UE_HALF_PI), FVector(0, 0, 10)) };
	TestTrue(TEXT("a turned arm poses"), FMantleGarmentOutfit::PoseMesh(Turned, TurnedPose, 1.0, Error));
	TestTrue(TEXT("root vertex stays"), Turned.Positions[0].Equals(FVector3f(1, 0, 0), 1e-4f));
	TestTrue(TEXT("arm vertex swings with its bone"), Turned.Positions[1].Equals(FVector3f(5, 0, 9), 1e-3f));
	TestTrue(TEXT("its normal turns with it"), Turned.Normals[1].Equals(FVector3f(0, 0, -1), 1e-3f));

	// A pose for the wrong number of joints is refused.
	FMantleGlbMesh Wrong = GarmentTestMesh();
	TestFalse(TEXT("a pose that does not match is refused"), FMantleGarmentOutfit::PoseMesh(Wrong, { FTransform::Identity }, 1.0, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleGarmentOutfitAlignTest,
	"Mantle.Editor.GarmentOutfit.AlignTranslation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleGarmentOutfitAlignTest::RunTest(const FString& Parameters)
{
	// 3000 random points on a 20 cm sphere (a regular grid would match itself one step over), and the same points moved.
	FRandomStream Random(7);
	TArray<FVector3f> To;
	for (int32 Index = 0; Index < 3000; ++Index)
	{
		To.Add(FVector3f(Random.GetUnitVector()) * 20.0f);
	}
	const FVector3f Shift(1.5f, -1.0f, 2.0f);
	TArray<FVector3f> From;
	for (const FVector3f& Point : To)
	{
		From.Add(Point - Shift);
	}
	double Mean = -1.0;
	const FVector3f Found = FMantleGarmentOutfit::AlignTranslation(From, To, Mean);
	TestTrue(FString::Printf(TEXT("the shift is found (%s)"), *Found.ToString()), Found.Equals(Shift, 0.2f));
	TestTrue(TEXT("the points then lie on each other"), Mean < 0.5);

	double Empty = -1.0;
	TestTrue(TEXT("nothing to align moves nothing"), FMantleGarmentOutfit::AlignTranslation({}, To, Empty).IsNearlyZero());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleGarmentOutfitWeightsTest,
	"Mantle.Editor.GarmentOutfit.TransferWeights",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleGarmentOutfitWeightsTest::RunTest(const FString& Parameters)
{
	// A body: a row of skin vertices at x = 0..40 cm, the first half on "thigh", the rest on "calf".
	FMantleGlbMesh Body;
	Body.Joints.Add({ FName(TEXT("pelvis")), INDEX_NONE, FTransform::Identity });
	Body.Joints.Add({ FName(TEXT("thigh")), 0, FTransform::Identity });
	Body.Joints.Add({ FName(TEXT("calf")), 1, FTransform(FVector(20, 0, 0)) });
	Body.NumInfluences = 1;
	for (int32 X = 0; X <= 40; ++X)
	{
		Body.Positions.Add(FVector3f(X, 0, 0));
		Body.InfluenceJoints.Add(X < 20 ? 1 : 2);
		Body.InfluenceWeights.Add(1.0f);
	}

	// A garment on its own bones: one vertex over the thigh, one over the calf, one 50 cm away.
	FMantleGlbMesh Garment;
	Garment.Joints.Add({ FName(TEXT("own_root")), INDEX_NONE, FTransform::Identity });
	Garment.Positions = { FVector3f(5, 0, 1), FVector3f(35, 0, 1), FVector3f(40, 0, 50) };
	Garment.NumInfluences = 1;
	Garment.InfluenceJoints = { 0, 0, 0 };
	Garment.InfluenceWeights = { 1.0f, 1.0f, 1.0f };

	int32 Far = -1;
	double Mean = -1.0;
	FString Error;
	TestTrue(TEXT("the weights transfer"), FMantleGarmentOutfit::TransferWeights(Garment, Body, Far, Mean, Error));
	TestEqual(TEXT("the garment takes the body's joints"), Garment.Joints.Num(), 3);
	TestEqual(TEXT("one slot is all any vertex uses"), Garment.NumInfluences, 1);
	TestEqual(TEXT("the vertex over the thigh follows the thigh"), static_cast<int32>(Garment.InfluenceJoints[0]), 1);
	TestEqual(TEXT("the vertex over the calf follows the calf"), static_cast<int32>(Garment.InfluenceJoints[1]), 2);
	TestEqual(TEXT("the far vertex still follows its nearest skin"), static_cast<int32>(Garment.InfluenceJoints[2]), 2);
	TestEqual(TEXT("one vertex is far from the skin"), Far, 1);
	TestTrue(TEXT("weights are whole"), FMath::IsNearlyEqual(Garment.InfluenceWeights[0], 1.0f, 1e-4f));

	// A body with no skin gives nothing to take.
	FMantleGlbMesh Bare;
	Bare.Positions = { FVector3f::ZeroVector };
	TestFalse(TEXT("a body without skin weights is refused"), FMantleGarmentOutfit::TransferWeights(Garment, Bare, Far, Mean, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleGarmentOutfitRootOnlyTest,
	"Mantle.Editor.GarmentOutfit.FollowsRootOnly",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleGarmentOutfitRootOnlyTest::RunTest(const FString& Parameters)
{
	// A cloth asset's export: every vertex on root, as Short Sleeve Tee and Sleeveless Shirt v4 uploaded.
	FMantleGlbMesh Garment;
	Garment.Joints.Add({ FName(TEXT("root")), INDEX_NONE, FTransform::Identity });
	Garment.Positions = { FVector3f(0, 0, 100), FVector3f(10, 0, 120) };
	Garment.NumInfluences = 1;
	Garment.InfluenceJoints = { 0, 0 };
	Garment.InfluenceWeights = { 1.0f, 1.0f };
	TestTrue(TEXT("weights on root alone do not follow the body"), FMantleGarmentOutfit::FollowsRootOnly(Garment));

	// The body's bones in the table do not help while no weight is on them.
	Garment.Joints.Add({ FName(TEXT("spine_05")), 0, FTransform::Identity });
	TestTrue(TEXT("unweighted body bones do not count"), FMantleGarmentOutfit::FollowsRootOnly(Garment));

	// One vertex on the spine is skinned to the body.
	Garment.NumInfluences = 2;
	Garment.InfluenceJoints = { 0, 1, 0, 0 };
	Garment.InfluenceWeights = { 0.5f, 0.5f, 1.0f, 0.0f };
	TestFalse(TEXT("a weight on a body bone follows the body"), FMantleGarmentOutfit::FollowsRootOnly(Garment));

	// A mesh with no skin at all follows nothing.
	TestTrue(TEXT("no skin follows nothing"), FMantleGarmentOutfit::FollowsRootOnly(FMantleGlbMesh()));
	return true;
}

#endif

#if WITH_DEV_AUTOMATION_TESTS

namespace MantleGarmentOutfitTestPrivate
{
	/** root -> upperarm_l -> lowerarm_l -> hand_l, the upper arm and forearm each 30 cm along Arm. */
	FReferenceSkeleton GarmentTestArm(const FVector& Arm)
	{
		FReferenceSkeleton Reference;
		FReferenceSkeletonModifier Modifier(Reference, nullptr);
		Modifier.Add(FMeshBoneInfo(FName(TEXT("root")), TEXT("root"), INDEX_NONE), FTransform::Identity);
		Modifier.Add(FMeshBoneInfo(FName(TEXT("upperarm_l")), TEXT("upperarm_l"), 0), FTransform(FVector(0, 0, 140)));
		Modifier.Add(FMeshBoneInfo(FName(TEXT("lowerarm_l")), TEXT("lowerarm_l"), 1), FTransform(Arm * 30.0));
		Modifier.Add(FMeshBoneInfo(FName(TEXT("hand_l")), TEXT("hand_l"), 2), FTransform(Arm * 30.0));
		return Reference;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleGarmentOutfitTurnTest,
	"Mantle.Editor.GarmentOutfit.TurnToTemplate",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleGarmentOutfitTurnTest::RunTest(const FString& Parameters)
{
	using namespace MantleGarmentOutfitTestPrivate;
	// An arm held out level, turned onto a template whose arm hangs 45 degrees down.
	const FReferenceSkeleton Level = GarmentTestArm(FVector(1, 0, 0));
	const FReferenceSkeleton Template = GarmentTestArm(FVector(1, 0, -1).GetSafeNormal());
	TArray<FString> Lines;
	const TArray<FTransform> Turned = FMantleGarmentOutfit::TurnToTemplate(Level, FMantleGarmentOutfit::ComponentPose(Level), Template,
		FMantleGarmentOutfit::ComponentPose(Template), Lines);
	const TArray<FTransform> Wanted = FMantleGarmentOutfit::ComponentPose(Template);
	TestTrue(TEXT("the shoulder stays"), Turned[1].GetLocation().Equals(FVector(0, 0, 140), 1e-3));
	TestTrue(FString::Printf(TEXT("the elbow lands on the template's (%s)"), *Turned[2].GetLocation().ToString()), Turned[2].GetLocation().Equals(Wanted[2].GetLocation(), 1e-3));
	TestTrue(TEXT("the hand follows"), Turned[3].GetLocation().Equals(Wanted[3].GetLocation(), 1e-3));
	TestTrue(TEXT("the turn is reported"), Lines.Num() == 2 && Lines[0].StartsWith(TEXT("upperarm_l 45.0")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleGarmentOutfitCarryTest,
	"Mantle.Editor.GarmentOutfit.CarryWithSkin",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleGarmentOutfitCarryTest::RunTest(const FString& Parameters)
{
	using namespace MantleGarmentOutfitTestPrivate;
	FString Error;
	double MeanCm = 0.0;

	// The MetaHuman's skin 2 cm below the original's: fabric 0.5 cm over the original skin ends 0.5 cm over the MetaHuman's,
	// and fabric beyond the search (20 cm off) moves with its nearest skin too.
	const FMantleSkinnedMeshLOD Original = SkinSheet(20, 0.0f);
	const FMantleSkinnedMeshLOD Smaller = SkinSheet(20, -2.0f);
	TArray<FVector3f> Garment = { FVector3f(10, 10, 0.5f), FVector3f(5.5f, 14.2f, 0.5f), FVector3f(10, 10, 20.0f) };
	TestTrue(TEXT("carries onto a smaller body"), FMantleGarmentOutfit::CarryWithSkin(Garment, Original, Smaller, MeanCm, Error));
	TestTrue(TEXT("fabric keeps its clearance"), Garment[0].Equals(FVector3f(10, 10, -1.5f), 1e-3f) && Garment[1].Equals(FVector3f(5.5f, 14.2f, -1.5f), 1e-3f));
	TestTrue(TEXT("far fabric moves with its nearest skin"), Garment[2].Equals(FVector3f(10, 10, 18.0f), 1e-3f));
	TestEqual(TEXT("the average move"), MeanCm, 2.0, 1e-3);

	// A bump in the MetaHuman (a fuller calf): fabric over it goes out with it, fabric away from it stays.
	const FMantleSkinnedMeshLOD Bump = SkinSheet(20, 0.0f, [](const float X, const float Y)
	{
		return 2.0f * FMath::Max(0.0f, 1.0f - FVector2f::DistSquared(FVector2f(X, Y), FVector2f(10, 10)) / 36.0f);
	});
	TArray<FVector3f> OverBump = { FVector3f(10, 10, 0.3f), FVector3f(2, 2, 0.3f) };
	TestTrue(TEXT("carries onto a bump"), FMantleGarmentOutfit::CarryWithSkin(OverBump, Original, Bump, MeanCm, Error));
	TestTrue(TEXT("over the bump it goes out"), OverBump[0].Z > 1.8f);
	TestTrue(TEXT("away from it it stays"), OverBump[1].Equals(FVector3f(2, 2, 0.3f), 1e-3f));

	// A surface with no triangles is refused.
	AddExpectedError(TEXT("MANTLE_GARMENTFIT FAIL clearance"), EAutomationExpectedErrorFlags::Contains, 1);
	TArray<FVector3f> Any = { FVector3f::ZeroVector };
	TestFalse(TEXT("no skin is refused"), FMantleGarmentOutfit::CarryWithSkin(Any, FMantleSkinnedMeshLOD(), Smaller, MeanCm, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleGarmentOutfitClearanceTest,
	"Mantle.Editor.GarmentOutfit.WeldedSurfaceAndClearance",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleGarmentOutfitClearanceTest::RunTest(const FString& Parameters)
{
	using namespace MantleGarmentOutfitTestPrivate;
	// Two triangles sharing an edge, its two corners copied (a UV seam): four points, the copies welded.
	FMantleGlbMesh Mesh;
	Mesh.Positions = { FVector3f(0, 0, 0), FVector3f(1, 0, 0), FVector3f(0, 1, 0), FVector3f(1, 0, 0), FVector3f(0, 1, 0), FVector3f(1, 1, 0) };
	Mesh.Normals.Init(FVector3f(0, 0, 1), 6);
	FMantleGlbPrimitive& First = Mesh.Primitives.AddDefaulted_GetRef();
	First.FirstVertex = 0;
	First.NumVertices = 3;
	First.Indices = { 0, 1, 2 };
	FMantleGlbPrimitive& Second = Mesh.Primitives.AddDefaulted_GetRef();
	Second.FirstVertex = 3;
	Second.NumVertices = 3;
	Second.Indices = { 0, 2, 1 };
	FMantleSkinnedMeshLOD Surface;
	TArray<int32> PointOf;
	FMantleGarmentOutfit::WeldedSurface(Mesh, Surface, PointOf);
	TestEqual(TEXT("seam copies are welded"), Surface.Positions.Num(), 4);
	TestEqual(TEXT("a copy maps to its point"), PointOf[3], PointOf[1]);
	TestEqual(TEXT("both triangles are kept"), Surface.Indices.Num(), 6);

	// One point on the skin, one 2 cm out, one 1 cm under: one under, two thirds within 1 cm.
	const FString Line = FMantleGarmentOutfit::DescribeClearance({ FVector3f(0.5f, 0.2f, 0.0f), FVector3f(0.2f, 0.2f, 2.0f), FVector3f(0.7f, 0.7f, -1.0f) }, SkinSheet(4, 0.0f));
	TestTrue(TEXT("all three are tracked"), Line.StartsWith(TEXT("tracked 3/3, under 1,")));
	TestTrue(TEXT("the share within 1 cm"), Line.Contains(TEXT("within 1 cm 67%")));

	// The shape change: unmoved, nothing changed; the far corner pulled through the shared edge, its triangle turns over.
	const TArray<FVector3f> Before = Mesh.Positions;
	TestTrue(TEXT("an unmoved mesh did not change"), FMantleGarmentOutfit::DescribeShapeChange(Before, Mesh).StartsWith(TEXT("edge length change p50 0% p90 0% largest 0%, 0 of 2")));
	Mesh.Positions[5] = FVector3f(0.25f, 0.25f, 0.0f);
	const FString Shape = FMantleGarmentOutfit::DescribeShapeChange(Before, Mesh);
	TestTrue(TEXT("the folded triangle is counted"), Shape.Contains(TEXT("1 of 2 triangles turned over")));
	TestTrue(TEXT("a changed vertex count is not measured"), FMantleGarmentOutfit::DescribeShapeChange({ FVector3f::ZeroVector }, Mesh).StartsWith(TEXT("not measured")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleGarmentOutfitCoverSkinTest,
	"Mantle.Editor.GarmentOutfit.CoverSkin",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleGarmentOutfitCoverSkinTest::RunTest(const FString& Parameters)
{
	using namespace MantleGarmentOutfitTestPrivate;
	// Skin with a 1.2 cm kneecap at (6, 3); a coarse garment square 1 cm over the flat skin, its vertices all well outside
	// it, but the kneecap comes through the triangle (1, 1), (9, 1), (9, 9) between them.
	const FMantleSkinnedMeshLOD Knee = SkinSheet(10, 0.0f, [](const float X, const float Y) { return X == 6.0f && Y == 3.0f ? 1.2f : 0.0f; });
	auto Square = [](const float Z, const bool bFacingSkin)
	{
		FMantleSkinnedMeshLOD Garment;
		Garment.Positions = { FVector3f(1, 1, Z), FVector3f(9, 1, Z), FVector3f(1, 9, Z), FVector3f(9, 9, Z) };
		Garment.Normals.Init(FVector3f(0, 0, bFacingSkin ? -1.0f : 1.0f), 4);
		Garment.Indices = { 0, 1, 3, 0, 3, 2 };
		return Garment;
	};
	FMantleSkinnedMeshLOD Garment = Square(1.0f, false);
	int32 After = 0;
	const int32 Before = FMantleAvatarRefit::CoverSkin(Garment, Knee, After);
	TestEqual(TEXT("the kneecap came through"), Before, 1);
	TestEqual(TEXT("it is covered"), After, 0);
	for (const int32 Corner : { 0, 1, 3 })
	{
		TestTrue(TEXT("the triangle over it went out by the cover"), FMath::IsNearlyEqual(Garment.Positions[Corner].Z, 1.2f + FMantleAvatarRefit::SkinCoverCm, 0.02f));
	}
	TestTrue(TEXT("the other corner stays"), FMath::IsNearlyEqual(Garment.Positions[2].Z, 1.0f, 1e-4f));

	// The same square as an inner lining facing the skin: left alone.
	FMantleSkinnedMeshLOD Lining = Square(1.0f, true);
	FMantleAvatarRefit::CoverSkin(Lining, Knee, After);
	TestTrue(TEXT("a lining facing the skin does not move"), Lining.Positions[0].Equals(FVector3f(1, 1, 1), 1e-4f) && Lining.Positions[3].Equals(FVector3f(9, 9, 1), 1e-4f));

	// Skin far through it is beside the garment's edge, not under it (the chest above a neckline): left alone.
	const FMantleSkinnedMeshLOD Beside = SkinSheet(10, 0.0f, [](const float X, const float Y) { return X == 6.0f && Y == 3.0f ? 2.5f : 0.0f; });
	FMantleSkinnedMeshLOD Edge = Square(1.0f, false);
	FMantleAvatarRefit::CoverSkin(Edge, Beside, After);
	TestTrue(TEXT("skin far through does not move it"), Edge.Positions[1].Equals(FVector3f(9, 1, 1), 1e-4f));

	// Skin well under the garment asks nothing of it.
	FMantleSkinnedMeshLOD High = Square(1.6f, false);
	TestEqual(TEXT("nothing through a garment over it"), FMantleAvatarRefit::CoverSkin(High, Knee, After), 0);
	TestTrue(TEXT("and it does not move"), High.Positions[0].Equals(FVector3f(1, 1, 1.6f), 1e-4f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleGarmentOutfitSlotTest,
	"Mantle.Editor.GarmentOutfit.GarmentSlot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleGarmentOutfitSlotTest::RunTest(const FString& Parameters)
{
	auto Section = [](const TCHAR* Slot, const TCHAR* Material)
	{
		FMantleOutfitSection Out;
		Out.SlotName = Slot;
		Out.MaterialName = Material;
		return Out;
	};
	FString Why;
	// The name says the piece; sections that say nothing don't matter.
	TestEqual(TEXT("a shirt is a top"), FMantleGarmentOutfit::GarmentSlot(TEXT("SK_Clothes_Shirt"), { Section(TEXT("M_Clothes"), TEXT("M_Clothes")) }, Why), FString(TEXT("top")));
	TestEqual(TEXT("upper-case pants are a bottom"), FMantleGarmentOutfit::GarmentSlot(TEXT("SK_MECHANICGIRL_PANTS"), {}, Why), FString(TEXT("bottom")));
	TestEqual(TEXT("shoes are footwear"), FMantleGarmentOutfit::GarmentSlot(TEXT("SK_Underwear_Shoes"), {}, Why), FString(TEXT("footwear")));
	// Not clothing: no slot and no why.
	TestEqual(TEXT("short hair is not a bottom"), FMantleGarmentOutfit::GarmentSlot(TEXT("SK_MECHANICGIRL_HAIR_SHORT"), {}, Why), FString());
	TestTrue(TEXT("hair has no why"), Why.IsEmpty());
	TestEqual(TEXT("a body is not clothing"), FMantleGarmentOutfit::GarmentSlot(TEXT("SK_Girl_Body"), { Section(TEXT("Body"), TEXT("M_Skin")) }, Why), FString());
	TestTrue(TEXT("a body has no why"), Why.IsEmpty());
	TestEqual(TEXT("gloves are not offered"), FMantleGarmentOutfit::GarmentSlot(TEXT("SK_MECHANICGIRL_GLOVES"), {}, Why), FString());
	// Clothing that can't be one Outfit says why.
	TestEqual(TEXT("a dressed body is merged"), FMantleGarmentOutfit::GarmentSlot(TEXT("SK_MechanicGirl_01"),
		{ Section(TEXT("Body"), TEXT("M_Skin")), Section(TEXT("Shirt"), TEXT("M_Shirt")), Section(TEXT("Pants"), TEXT("M_Pants")) }, Why), FString());
	TestTrue(TEXT("merged with the skin says so"), Why.Contains(TEXT("skin")));
	TestEqual(TEXT("two pieces in one mesh"), FMantleGarmentOutfit::GarmentSlot(TEXT("SK_Outfit"),
		{ Section(TEXT("Shirt"), TEXT("M_Shirt")), Section(TEXT("Pants"), TEXT("M_Pants")) }, Why), FString());
	TestTrue(TEXT("several pieces says which"), Why.Contains(TEXT("top and bottom")));
	TestEqual(TEXT("a shirt with skin in it"), FMantleGarmentOutfit::GarmentSlot(TEXT("SK_Shirt"), { Section(TEXT("Shirt"), TEXT("M_Shirt")), Section(TEXT("Skin"), TEXT("M_Skin")) }, Why), FString());
	TestFalse(TEXT("a shirt with skin says why"), Why.IsEmpty());
	// No name to go by: one piece of sections is that piece.
	TestEqual(TEXT("sections say a bottom"), FMantleGarmentOutfit::GarmentSlot(TEXT("SK_Piece_02"), { Section(TEXT("Jeans"), TEXT("MI_Denim")) }, Why), FString(TEXT("bottom")));
	return true;
}

#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleGarmentOutfitBareBodyRankTest,
	"Mantle.Editor.GarmentOutfit.BareBodyRank",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleGarmentOutfitBareBodyRankTest::RunTest(const FString& Parameters)
{
	// A whole body wearing nothing beats one in underwear; a part, or a dressed body not named bare, is no bare body.
	const int32 Bare = FMantleGarmentOutfit::BareBodyRank(TEXT("SK_Girl_Body"), 0, 1.0);
	const int32 Underwear = FMantleGarmentOutfit::BareBodyRank(TEXT("SK_VampireGirl_Underwear"), 1, 0.99);
	TestTrue(TEXT("all skin is best"), Bare > Underwear);
	TestTrue(TEXT("a body in underwear is a bare body"), Underwear >= 0);
	TestTrue(TEXT("a nude name is too"), FMantleGarmentOutfit::BareBodyRank(TEXT("Nude"), 1, 1.0) >= 0);
	TestEqual(TEXT("a torso alone is a part"), FMantleGarmentOutfit::BareBodyRank(TEXT("SK_Torso"), 0, 0.45), -1);
	TestEqual(TEXT("a dressed body is not bare"), FMantleGarmentOutfit::BareBodyRank(TEXT("SK_VampireGirl_Nightwear"), 3, 1.0), -1);
	// Vampire's pack: her all-skin body (SK_VampireGirl: body, eyes, mouth, lashes, head) beats the one in underwear.
	TestTrue(TEXT("all skin beats underwear"), FMantleGarmentOutfit::BareBodyRank(TEXT("SK_VampireGirl"), 0, 0.96)
		> FMantleGarmentOutfit::BareBodyRank(TEXT("SK_VampireGirl_Underwear"), 1, 1.0));
	// Strip's check, which FindBareBody counts by: her sections are skin or a face detail, her underwear is neither.
	for (const TCHAR* Part : { TEXT("M_VG_Body"), TEXT("M_VG_Head") })
	{
		TestTrue(FString::Printf(TEXT("%s is skin"), Part), FMantleAvatarStrip::LooksLikeSkin(Part, Part, FString()));
	}
	for (const TCHAR* Part : { TEXT("M_VG_Eyes"), TEXT("M_VG_Mouth"), TEXT("M_VG_Lashes") })
	{
		TestTrue(FString::Printf(TEXT("%s is a face detail"), Part), FMantleAvatarStrip::NamesFaceDetail(Part, Part));
	}
	TestFalse(TEXT("underwear is not skin"), FMantleAvatarStrip::LooksLikeSkin(TEXT("M_VG_Underwear"), TEXT("M_VG_Underwear"), FString()));
	TestFalse(TEXT("nor a face detail"), FMantleAvatarStrip::NamesFaceDetail(TEXT("M_VG_Underwear"), TEXT("M_VG_Underwear")));
	TestTrue(TEXT("an underwear material is underwear"), FMantleGarmentOutfit::IsUnderwearName(TEXT("M_VG_Underwear")));
	TestFalse(TEXT("a corset is not underwear"), FMantleGarmentOutfit::IsUnderwearName(TEXT("M_VG_Corset")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleGarmentOutfitSplitsCleanlyTest,
	"Mantle.Editor.GarmentOutfit.SplitsCleanly",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleGarmentOutfitSplitsCleanlyTest::RunTest(const FString& Parameters)
{
	// Vampire's numbers: her merged mesh's skin folded 1706 of 47664 MetaHuman triangles (not splittable); her separate bare
	// body 522 (at the toes, in her heels) and Ida Faber's none (both splittable).
	TestFalse(TEXT("the merged Vampire doesn't split cleanly"), FMantleGarmentOutfit::SplitsCleanly(1706, 47664));
	TestTrue(TEXT("her separate bare body does"), FMantleGarmentOutfit::SplitsCleanly(522, 47664));
	TestTrue(TEXT("no folds splits"), FMantleGarmentOutfit::SplitsCleanly(0, 47664));
	TestFalse(TEXT("no body is not clean"), FMantleGarmentOutfit::SplitsCleanly(0, 0));

	// Turned triangles: one of two folded through the shared edge.
	FMantleGlbMesh Mesh;
	Mesh.Positions = { FVector3f(0, 0, 0), FVector3f(1, 0, 0), FVector3f(0, 1, 0), FVector3f(1, 1, 0) };
	FMantleGlbPrimitive& Primitive = Mesh.Primitives.AddDefaulted_GetRef();
	Primitive.NumVertices = 4;
	Primitive.Indices = { 0, 1, 2, 1, 3, 2 };
	const TArray<FVector3f> Before = Mesh.Positions;
	TestEqual(TEXT("unmoved, none turned"), FMantleGarmentOutfit::TurnedTriangles(Before, Mesh), 0);
	Mesh.Positions[3] = FVector3f(0.25f, 0.25f, 0.0f);
	TestEqual(TEXT("the folded one turned"), FMantleGarmentOutfit::TurnedTriangles(Before, Mesh), 1);
	return true;
}

namespace MantleGarmentOutfitTrimTestPrivate
{
	/** A flat square of Cells x Cells cells, Size cm across, at height Z, one primitive of material Material. */
	FMantleGlbMesh Sheet(const int32 Cells, const float Size, const float Z, const int32 Material = 0)
	{
		FMantleGlbMesh Mesh;
		for (int32 Y = 0; Y <= Cells; ++Y)
		{
			for (int32 X = 0; X <= Cells; ++X)
			{
				Mesh.Positions.Add(FVector3f(Size * X / Cells, Size * Y / Cells, Z));
				Mesh.Normals.Add(FVector3f(0, 0, 1));
				Mesh.UVs.Add(FVector2f(static_cast<float>(X) / Cells, static_cast<float>(Y) / Cells));
			}
		}
		FMantleGlbPrimitive& Primitive = Mesh.Primitives.AddDefaulted_GetRef();
		Primitive.MaterialIndex = Material;
		Primitive.NumVertices = Mesh.Positions.Num();
		for (int32 Y = 0; Y < Cells; ++Y)
		{
			for (int32 X = 0; X < Cells; ++X)
			{
				const uint32 A = Y * (Cells + 1) + X;
				Primitive.Indices.Append({ A, A + 1, A + Cells + 1, A + 1, A + Cells + 2, A + Cells + 1 });
			}
		}
		Mesh.MaterialNames.SetNum(Material + 1);
		return Mesh;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleGarmentOutfitTrimTest,
	"Mantle.Editor.GarmentOutfit.Trim",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleGarmentOutfitTrimTest::RunTest(const FString& Parameters)
{
	using namespace MantleGarmentOutfitTrimTestPrivate;
	// A corset (20 x 20 cells) and a small lace strip on it: trim. The same strip 3 cm off it, or a piece as big as the
	// corset lying on it (a jacket over a shirt), is not.
	const FMantleGlbMesh Corset = Sheet(20, 20.0f, 0.0f);
	TestTrue(TEXT("a small piece 0.4 cm off it is its trim"), FMantleGarmentOutfit::IsTrimOn(Sheet(4, 5.0f, 0.4f), Corset));
	TestFalse(TEXT("3 cm off it is not"), FMantleGarmentOutfit::IsTrimOn(Sheet(4, 5.0f, 3.0f), Corset));
	// An edging standing up from the rim: half of it on the corset, half up to 1.5 cm past its edge.
	FMantleGlbMesh Edging = Sheet(4, 4.0f, 0.3f);
	for (FVector3f& Position : Edging.Positions)
	{
		Position.X += 17.5f;
	}
	TestTrue(TEXT("an edging over the rim is its trim"), FMantleGarmentOutfit::IsTrimOn(Edging, Corset));
	TestFalse(TEXT("a whole garment on it is not trim"), FMantleGarmentOutfit::IsTrimOn(Sheet(20, 20.0f, 0.4f), Corset));

	// Appended: its vertices after the garment's, its primitive drawn with its own material after the garment's.
	FMantleGlbMesh Garment = Sheet(2, 1.0f, 0.0f);
	const FMantleGlbMesh Trim = Sheet(1, 1.0f, 0.1f);
	FMantleGarmentOutfit::AppendTrim(Garment, Trim);
	TestEqual(TEXT("vertices added"), Garment.Positions.Num(), 9 + 4);
	TestEqual(TEXT("normals kept in step"), Garment.Normals.Num(), Garment.Positions.Num());
	TestEqual(TEXT("UVs kept in step"), Garment.UVs.Num(), Garment.Positions.Num());
	TestEqual(TEXT("a primitive added"), Garment.Primitives.Num(), 2);
	TestEqual(TEXT("its first vertex after the garment's"), Garment.Primitives[1].FirstVertex, 9);
	TestEqual(TEXT("its material after the garment's"), Garment.Primitives[1].MaterialIndex, 1);
	TestEqual(TEXT("material names appended"), Garment.MaterialNames.Num(), 2);
	return true;
}
