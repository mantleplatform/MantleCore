// Copyright Joshua Eger. All Rights Reserved.
#include "MantleGlbWriter.h"
#include "MantleMeshReduce.h"
#include "MantlePlaceAssets.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// A mesh far past the triangle limit is refused before any reduction starts (a 517 MB scene mesh hung a publish for 100 minutes),
// and a value that is not a number in a mesh is set to a safe one before the export (the server refuses it otherwise).

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMeshReduceCeilingTest,
	"Mantle.Editor.MeshReduce.RefusesAMeshTooBigToReduce",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMeshReduceCeilingTest::RunTest(const FString& Parameters)
{
	FMantlePlaceAssetLimits Limits;
	Limits.MaxTriangles = 500000;
	const int64 Ceiling = static_cast<int64>(Limits.MaxTriangles) * FMantleMeshReduce::MaxSourceTriangleFactor;

	FString Why;
	TestFalse(TEXT("a mesh just over the limit is reduced"), FMantleMeshReduce::IsTooBigToReduce(Limits.MaxTriangles + 1, Limits, Why));
	TestTrue(TEXT("and gives no reason"), Why.IsEmpty());
	TestFalse(TEXT("a mesh at the ceiling is still reduced"), FMantleMeshReduce::IsTooBigToReduce(Ceiling, Limits, Why));
	TestTrue(TEXT("a mesh one triangle past the ceiling is refused"), FMantleMeshReduce::IsTooBigToReduce(Ceiling + 1, Limits, Why));
	TestTrue(TEXT("with a reason that says what to do"), Why.Contains(TEXT("too big to reduce")) && Why.Contains(TEXT("Split it")));
	Why.Reset();
	TestTrue(TEXT("a mesh with more triangles than an int32 holds is refused"), FMantleMeshReduce::IsTooBigToReduce(5000000000LL, Limits, Why));

	// The ceiling follows the limit.
	FMantlePlaceAssetLimits Small;
	Small.MaxTriangles = 1000;
	Why.Reset();
	TestFalse(TEXT("20 times a small limit is reduced"), FMantleMeshReduce::IsTooBigToReduce(20000, Small, Why));
	TestTrue(TEXT("past it is refused"), FMantleMeshReduce::IsTooBigToReduce(20001, Small, Why));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleGlbSanitiseNonFiniteTest,
	"Mantle.Editor.GlbWriter.SetsValuesThatAreNotNumbers",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleGlbSanitiseNonFiniteTest::RunTest(const FString& Parameters)
{
	const float NaN = TNumericLimits<float>::QuietNaN();
	const float Infinity = TNumericLimits<float>::Infinity();

	FMantleGlbMesh Glb;
	Glb.Positions = { FVector3f(0, 0, 0), FVector3f(100, 0, 0), FVector3f(0, 100, 0) };
	Glb.Normals = { FVector3f(0, 0, 1), FVector3f(NaN, 0, 1), FVector3f(1, 0, 0) };
	Glb.Tangents = { FVector4f(1, 0, 0, 1), FVector4f(1, 0, 0, 1), FVector4f(0, Infinity, 0, 1) };
	Glb.UVs = { FVector2f(0.25f, 0.5f), FVector2f(NaN, 0.5f), FVector2f(0.0f, Infinity) };
	Glb.UV1s = { FVector2f(NaN, NaN), FVector2f(0.75f, 0.5f), FVector2f(0.0f, Infinity) };

	const FMantleGlbNonFinite Found = FMantleGlbWriter::SanitiseNonFinite(Glb);
	TestEqual(TEXT("vertices with a bad texture coordinate, counted once each"), Found.TexCoords, 3);
	TestEqual(TEXT("bad normals"), Found.Normals, 1);
	TestEqual(TEXT("bad tangents"), Found.Tangents, 1);
	TestEqual(TEXT("no bad positions"), Found.Positions, 0);
	TestTrue(TEXT("something was found"), Found.Any());

	TestTrue(TEXT("a good texture coordinate is kept"), Glb.UVs[0] == FVector2f(0.25f, 0.5f));
	TestTrue(TEXT("a bad UV0 is 0, 0"), Glb.UVs[1] == FVector2f::ZeroVector);
	TestTrue(TEXT("a bad UV0 component is 0, 0 too"), Glb.UVs[2] == FVector2f::ZeroVector);
	TestTrue(TEXT("a bad UV1 is 0, 0"), Glb.UV1s[0] == FVector2f::ZeroVector);
	TestTrue(TEXT("a good UV1 is kept"), Glb.UV1s[1] == FVector2f(0.75f, 0.5f));
	TestTrue(TEXT("a bad UV1 component is 0, 0 too"), Glb.UV1s[2] == FVector2f::ZeroVector);

	TestTrue(TEXT("a good normal is kept"), Glb.Normals[0] == FVector3f(0, 0, 1));
	TestTrue(TEXT("a bad normal is straight up"), Glb.Normals[1] == FVector3f(0, 0, 1));
	TestTrue(TEXT("a good tangent is kept"), Glb.Tangents[0] == FVector4f(1, 0, 0, 1));

	// The replacement tangent is a unit direction at right angles to its normal (1, 0, 0), with w 1.
	const FVector4f Tangent = Glb.Tangents[2];
	TestFalse(TEXT("the replacement tangent is a number"), Tangent.ContainsNaN());
	const FVector3f TangentXYZ(Tangent.X, Tangent.Y, Tangent.Z);
	TestTrue(TEXT("a unit direction"), FMath::IsNearlyEqual(TangentXYZ.Size(), 1.0f, 0.001f));
	TestTrue(TEXT("at right angles to the normal"), FMath::IsNearlyZero(FVector3f::DotProduct(TangentXYZ, Glb.Normals[2]), 0.001f));
	TestEqual(TEXT("with w 1"), Tangent.W, 1.0f);

	// A mesh with nothing wrong is not changed.
	const FMantleGlbNonFinite Clean = FMantleGlbWriter::SanitiseNonFinite(Glb);
	TestFalse(TEXT("a second pass finds nothing"), Clean.Any());

	// A position cannot be repaired: counted, and left as it is.
	Glb.Positions[1] = FVector3f(NaN, 0, 0);
	const FMantleGlbNonFinite BadPosition = FMantleGlbWriter::SanitiseNonFinite(Glb);
	TestEqual(TEXT("a bad position is counted"), BadPosition.Positions, 1);
	TestTrue(TEXT("and left as it was"), Glb.Positions[1].ContainsNaN());
	return true;
}

#endif
