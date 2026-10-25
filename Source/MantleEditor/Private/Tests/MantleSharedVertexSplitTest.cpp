// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCosmeticExport.h"
#include "MantleGlbWriter.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// A static mesh whose material sections share vertices exports with each section split off on its own copy
// of the vertices it uses, so the sections tile the vertex buffer the way the runtime reader needs, and no triangle moves.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleSharedVertexSplitTestPrivate
{
	/** A quad of two triangles, (0,1,2) and (2,1,3), that share vertices 1 and 2; one triangle in each of two sections. */
	void MakeQuad(FMantleGlbMesh& Glb, TArray<uint32>& Indices, TArray<FMantleSharedVertexSection>& Sections)
	{
		Glb.Name = TEXT("SharedQuad");
		Glb.Positions = { FVector3f(0, 0, 0), FVector3f(100, 0, 0), FVector3f(0, 100, 0), FVector3f(100, 100, 0) };
		Glb.Normals = { FVector3f(0, 0, 1), FVector3f(0, 0, 1), FVector3f(0, 0, 1), FVector3f(0, 0, 1) };
		Glb.Tangents = { FVector4f(1, 0, 0, 1), FVector4f(1, 0, 0, 1), FVector4f(1, 0, 0, 1), FVector4f(1, 0, 0, 1) };
		Glb.UVs = { FVector2f(0, 0), FVector2f(1, 0), FVector2f(0, 1), FVector2f(1, 1) };
		Glb.UV1s = { FVector2f(0.0f, 0.5f), FVector2f(0.25f, 0.5f), FVector2f(0.5f, 0.5f), FVector2f(0.75f, 0.5f) };
		Glb.Colors = { FColor(10, 0, 0, 255), FColor(20, 0, 0, 255), FColor(30, 0, 0, 255), FColor(40, 0, 0, 255) };
		Glb.MaterialNames = { TEXT("Red"), TEXT("Blue") };
		Indices = { 0, 1, 2, 2, 1, 3 };

		FMantleSharedVertexSection& First = Sections.AddDefaulted_GetRef();
		First.FirstIndex = 0;
		First.NumTriangles = 1;
		First.GlbMaterial = 0;
		FMantleSharedVertexSection& Second = Sections.AddDefaulted_GetRef();
		Second.FirstIndex = 3;
		Second.NumTriangles = 1;
		Second.GlbMaterial = 1;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleSharedVertexSplitTest,
	"Mantle.Editor.StaticMesh.SharedVertexSectionsAreSplit",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleSharedVertexSplitTest::RunTest(const FString& Parameters)
{
	using namespace MantleSharedVertexSplitTestPrivate;
	FMantleGlbMesh Glb;
	TArray<uint32> Indices;
	TArray<FMantleSharedVertexSection> Sections;
	MakeQuad(Glb, Indices, Sections);
	const FMantleGlbMesh Before = Glb;

	TestEqual(TEXT("the split counts each section's own vertices"), FMantleCosmeticExport::CountSplitVertices(Indices, Sections), 6);

	FMantleCosmeticExport::SplitSharedVertexSections(Glb, Indices, Sections);
	if (!TestEqual(TEXT("one primitive a section"), Glb.Primitives.Num(), 2) || !TestEqual(TEXT("the shared vertices are written once for each"), Glb.Positions.Num(), 6))
	{
		return false;
	}
	TestEqual(TEXT("the count matches what the split wrote"), FMantleCosmeticExport::CountSplitVertices(Indices, Sections), Glb.Positions.Num());
	TestEqual(TEXT("normals follow"), Glb.Normals.Num(), 6);
	TestEqual(TEXT("tangents follow"), Glb.Tangents.Num(), 6);
	TestEqual(TEXT("UVs follow"), Glb.UVs.Num(), 6);
	TestEqual(TEXT("second UVs follow"), Glb.UV1s.Num(), 6);
	TestEqual(TEXT("colours follow"), Glb.Colors.Num(), 6);

	// The sections tile the vertex buffer in order, each with its material and one triangle.
	int32 Covered = 0;
	for (int32 Primitive = 0; Primitive < Glb.Primitives.Num(); ++Primitive)
	{
		const FMantleGlbPrimitive& Section = Glb.Primitives[Primitive];
		TestEqual(*FString::Printf(TEXT("section %d starts where the one before ended"), Primitive), Section.FirstVertex, Covered);
		TestEqual(*FString::Printf(TEXT("section %d keeps its material"), Primitive), Section.MaterialIndex, Primitive);
		TestEqual(*FString::Printf(TEXT("section %d keeps its triangle"), Primitive), Section.Indices.Num(), 3);
		for (const uint32 Index : Section.Indices)
		{
			TestTrue(*FString::Printf(TEXT("section %d's indices stay inside its own vertices"), Primitive), Index < static_cast<uint32>(Section.NumVertices));
		}
		Covered += Section.NumVertices;
	}
	TestEqual(TEXT("the sections cover every vertex"), Covered, Glb.Positions.Num());

	// Every triangle corner has the position and attributes it had.
	for (int32 Primitive = 0; Primitive < Glb.Primitives.Num(); ++Primitive)
	{
		const FMantleGlbPrimitive& Section = Glb.Primitives[Primitive];
		for (int32 Corner = 0; Corner < Section.Indices.Num(); ++Corner)
		{
			const int32 NewVertex = Section.FirstVertex + static_cast<int32>(Section.Indices[Corner]);
			const int32 OldVertex = static_cast<int32>(Indices[Sections[Primitive].FirstIndex + Corner]);
			TestTrue(TEXT("a corner keeps its position"), Glb.Positions[NewVertex] == Before.Positions[OldVertex]);
			TestTrue(TEXT("a corner keeps its normal"), Glb.Normals[NewVertex] == Before.Normals[OldVertex]);
			TestTrue(TEXT("a corner keeps its tangent"), Glb.Tangents[NewVertex] == Before.Tangents[OldVertex]);
			TestTrue(TEXT("a corner keeps its UV"), Glb.UVs[NewVertex] == Before.UVs[OldVertex]);
			TestTrue(TEXT("a corner keeps its second UV"), Glb.UV1s[NewVertex] == Before.UV1s[OldVertex]);
			TestTrue(TEXT("a corner keeps its colour"), Glb.Colors[NewVertex] == Before.Colors[OldVertex]);
		}
	}

	// The writer's own layout rule accepts it.
	TArray<uint8> Bytes;
	FString Error;
	const bool bWritten = FMantleGlbWriter::Write(Glb, Bytes, Error);
	TestTrue(*FString::Printf(TEXT("the GLB writes (%s)"), *Error), bWritten);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
