// Copyright Joshua Eger. All Rights Reserved.
#include "MantleSkinnedMesh.h"

#include "MantleSkinnedMeshReader.h"
#include "Misc/AutomationTest.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

// Body-shape offsets (#849): the BOFS block of an avatar body's MSK1, and applying the four sliders to a mesh. The MSK1 is built by hand: two
// LODs of two triangles that share their diagonal's two positions but not its vertices (a UV seam, so 6 vertices for 4 positions), two bones
// and a tangent per vertex. The offsets are small dyadic numbers, so sums and halves are exact in float.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleBodyOffsetsTestPrivate
{
	constexpr int32 NumLODs = 2;
	constexpr int32 NumBones = 2;
	constexpr int32 NumVertices = 6;

	/** Which position each vertex is: vertices 1 and 3, and 2 and 5, are the seam's copies. */
	constexpr int32 PositionGroup[NumVertices] = { 0, 1, 2, 1, 3, 2 };

	struct FBytes
	{
		TArray<uint8> Data;

		void Raw(const void* Source, const int32 Count)
		{
			Data.Append(static_cast<const uint8*>(Source), Count);
		}
		void U8(const uint8 Value) { Raw(&Value, 1); }
		void U16(const uint16 Value) { Raw(&Value, 2); }
		void U32(const uint32 Value) { Raw(&Value, 4); }
		void F32(const float Value) { Raw(&Value, 4); }
		void Str16(const char* Text)
		{
			U16(static_cast<uint16>(FCStringAnsi::Strlen(Text)));
			Raw(Text, FCStringAnsi::Strlen(Text));
		}
	};

	FVector3f VertexDelta(const int32 Id, const int32 LOD, const int32 Vertex)
	{
		const float Group = static_cast<float>(PositionGroup[Vertex]);
		return FVector3f((Id + 1) * 1.0f, Group * 0.5f, (LOD + 1) * (Id + 1) * Group);
	}

	FVector3f BoneDelta(const int32 Id, const int32 Bone)
	{
		return FVector3f((Id + 1) * 1.0f, static_cast<float>(Bone), 0.5f);
	}

	/** A BOFS payload for the given shape ids, as the Go encoder writes it. */
	TArray<uint8> MakeBlock(const TArray<int32>& Ids)
	{
		FBytes Out;
		Out.U32(1);
		Out.U32(NumLODs);
		Out.U32(NumBones);
		Out.U32(Ids.Num());
		for (const int32 Id : Ids)
		{
			Out.U8(static_cast<uint8>(Id));
			for (int32 LOD = 0; LOD < NumLODs; ++LOD)
			{
				for (int32 Vertex = 0; Vertex < NumVertices; ++Vertex)
				{
					const FVector3f Delta = VertexDelta(Id, LOD, Vertex);
					Out.F32(Delta.X);
					Out.F32(Delta.Y);
					Out.F32(Delta.Z);
				}
			}
			for (int32 Bone = 0; Bone < NumBones; ++Bone)
			{
				const FVector3f Delta = BoneDelta(Id, Bone);
				Out.F32(Delta.X);
				Out.F32(Delta.Y);
				Out.F32(Delta.Z);
			}
		}
		return Out.Data;
	}

	/** The seam's positions, tangents and so on, written for one LOD. */
	void WriteLOD(FBytes& Out)
	{
		const FVector3f Positions[NumVertices] = {
			FVector3f(0, 0, 0), FVector3f(10, 0, 0), FVector3f(0, 10, 0),
			FVector3f(10, 0, 0), FVector3f(10, 10, 0), FVector3f(0, 10, 0) };
		Out.U32(NumVertices);
		Out.U32(6);
		// One section over both triangles.
		Out.U32(1);
		Out.U32(0);
		Out.U32(0);
		Out.U32(6);
		Out.Str16("skin");
		for (const FVector3f& Position : Positions)
		{
			Out.F32(Position.X);
			Out.F32(Position.Y);
			Out.F32(Position.Z);
		}
		for (int32 Vertex = 0; Vertex < NumVertices; ++Vertex)
		{
			Out.F32(0);
			Out.F32(0);
			Out.F32(1);
		}
		for (int32 Vertex = 0; Vertex < NumVertices; ++Vertex)
		{
			// Tangent along +X; the second triangle's frame is mirrored.
			Out.F32(1);
			Out.F32(0);
			Out.F32(0);
			Out.F32(Vertex >= 3 ? -1.0f : 1.0f);
		}
		for (int32 Vertex = 0; Vertex < NumVertices; ++Vertex)
		{
			Out.F32(Vertex * 0.1f);
			Out.F32(Vertex * 0.2f);
		}
		const uint32 Indices[6] = { 0, 1, 2, 3, 4, 5 };
		for (const uint32 Index : Indices)
		{
			Out.U32(Index);
		}
		for (int32 Vertex = 0; Vertex < NumVertices; ++Vertex)
		{
			Out.U16(0);
		}
		for (int32 Vertex = 0; Vertex < NumVertices; ++Vertex)
		{
			Out.U8(255);
		}
	}

	struct FExtension
	{
		const char* Tag;
		TArray<uint8> Bytes;
	};

	/** An avatar body MSK1 (flags: reference pose, tangents; one influence per vertex) with the given extension blocks. */
	TArray<uint8> MakeMsk1(const TArray<FExtension>& Extensions)
	{
		FBytes Out;
		Out.Raw("MSK1", 4);
		Out.U32(1);
		Out.U32(1 | 4);
		Out.U32(0);
		Out.U32(1);
		Out.U32(NumBones);
		Out.U32(NumLODs);
		Out.U32(0);
		Out.U32(0);
		for (const float Bound : { -1.0f, -1.0f, -1.0f, 11.0f, 11.0f, 11.0f })
		{
			Out.F32(Bound);
		}
		Out.Str16("metahuman");
		Out.Str16("");
		Out.U16(4);
		Out.Raw("root", 4);
		Out.U32(static_cast<uint32>(INDEX_NONE));
		Out.U16(5);
		Out.Raw("child", 5);
		Out.U32(0);
		for (int32 Bone = 0; Bone < NumBones; ++Bone)
		{
			// Translation, rotation (x y z w), scale.
			for (const float Value : { Bone * 5.0f, 0.0f, 100.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f })
			{
				Out.F32(Value);
			}
		}
		for (int32 LOD = 0; LOD < NumLODs; ++LOD)
		{
			WriteLOD(Out);
		}
		Out.U32(Extensions.Num());
		for (const FExtension& Extension : Extensions)
		{
			Out.Raw(Extension.Tag, 4);
			Out.U32(Extension.Bytes.Num());
			Out.Raw(Extension.Bytes.GetData(), Extension.Bytes.Num());
		}
		return Out.Data;
	}

	TArray<uint8> MakeMsk1WithBlock(const TArray<uint8>& Block)
	{
		return MakeMsk1({ { "BOFS", Block } });
	}

	/** A body with the weight and muscle shapes (ids 0, 1, 2), read back; fails the test when it does not read. */
	bool ReadBody(FAutomationTestBase& Test, FMantleSkinnedMesh& OutMesh)
	{
		const FMantleSkinnedMeshReadResult Result = FMantleSkinnedMeshReader::Read(MakeMsk1WithBlock(MakeBlock({ 0, 1, 2 })));
		Test.TestTrue(*FString::Printf(TEXT("the body reads (%s)"), *Result.Error), Result.bSuccess);
		OutMesh = Result.Mesh;
		return Result.bSuccess;
	}

	template <typename T>
	bool SameBits(const TArray<T>& A, const TArray<T>& B)
	{
		return A.Num() == B.Num() && FMemory::Memcmp(A.GetData(), B.GetData(), A.Num() * sizeof(T)) == 0;
	}

	bool SameMesh(const FMantleSkinnedMesh& A, const FMantleSkinnedMesh& B)
	{
		if (A.LODs.Num() != B.LODs.Num() || A.ReferencePose.Num() != B.ReferencePose.Num())
		{
			return false;
		}
		for (int32 LOD = 0; LOD < A.LODs.Num(); ++LOD)
		{
			if (!SameBits(A.LODs[LOD].Positions, B.LODs[LOD].Positions) || !SameBits(A.LODs[LOD].Normals, B.LODs[LOD].Normals)
				|| !SameBits(A.LODs[LOD].Tangents, B.LODs[LOD].Tangents))
			{
				return false;
			}
		}
		for (int32 Bone = 0; Bone < A.ReferencePose.Num(); ++Bone)
		{
			if (!A.ReferencePose[Bone].Equals(B.ReferencePose[Bone], 0.0f))
			{
				return false;
			}
		}
		return true;
	}

	bool NearlyEqual(const FVector3f& A, const FVector3f& B) { return (A - B).GetAbsMax() < 1e-4f; }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleBodyOffsetsDecodeTest,
	"Mantle.Runtime.BodyOffsets.Decode",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleBodyOffsetsDecodeTest::RunTest(const FString& Parameters)
{
	using namespace MantleBodyOffsetsTestPrivate;

	// The positive control: a body with no block carries no offsets, so the block is what fills them.
	const FMantleSkinnedMeshReadResult Plain = FMantleSkinnedMeshReader::Read(MakeMsk1({}));
	TestTrue(TEXT("a body without a block reads"), Plain.bSuccess);
	TestTrue(TEXT("and carries no offsets"), Plain.Mesh.BodyOffsets.IsEmpty());

	// An unknown tag beside BOFS is still skipped, and BOFS still read.
	const FMantleSkinnedMeshReadResult Both = FMantleSkinnedMeshReader::Read(MakeMsk1({ { "ZZZZ", { 1, 2, 3 } }, { "BOFS", MakeBlock({ 0, 1, 2 }) } }));
	TestTrue(*FString::Printf(TEXT("a body with an unknown block and BOFS reads (%s)"), *Both.Error), Both.bSuccess);
	TestEqual(TEXT("the unknown block is reported skipped"), Both.Header.SkippedExtensions.Num(), 1);

	FMantleSkinnedMesh Mesh;
	if (!ReadBody(*this, Mesh))
	{
		return false;
	}
	const FMantleBodyOffsets& Offsets = Mesh.BodyOffsets;
	for (int32 Id = 0; Id < FMantleBodyOffsets::NumShapes; ++Id)
	{
		TestEqual(*FString::Printf(TEXT("shape %d presence"), Id), Offsets.Has(Id), Id <= 2);
	}
	TestFalse(TEXT("Has is false past the last id"), Offsets.Has(8));
	for (const int32 Id : { 0, 1, 2 })
	{
		for (int32 LOD = 0; LOD < NumLODs; ++LOD)
		{
			TestEqual(TEXT("shape LOD vertex count"), Offsets.Shapes[Id].LODs[LOD].Num(), NumVertices);
			for (int32 Vertex = 0; Vertex < NumVertices; ++Vertex)
			{
				TestTrue(*FString::Printf(TEXT("shape %d LOD%d vertex %d"), Id, LOD, Vertex),
					Offsets.Shapes[Id].LODs[LOD][Vertex] == VertexDelta(Id, LOD, Vertex));
			}
		}
		for (int32 Bone = 0; Bone < NumBones; ++Bone)
		{
			TestTrue(*FString::Printf(TEXT("shape %d bone %d"), Id, Bone), Offsets.Shapes[Id].Bones[Bone] == BoneDelta(Id, Bone));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleBodyOffsetsRefusedTest,
	"Mantle.Runtime.BodyOffsets.MalformedRefused",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleBodyOffsetsRefusedTest::RunTest(const FString& Parameters)
{
	using namespace MantleBodyOffsetsTestPrivate;

	auto Refused = [this](const TCHAR* What, const TArray<uint8>& Block)
	{
		const FMantleSkinnedMeshReadResult Result = FMantleSkinnedMeshReader::Read(MakeMsk1WithBlock(Block));
		TestFalse(*FString::Printf(TEXT("%s is refused"), What), Result.bSuccess);
		TestFalse(*FString::Printf(TEXT("%s says why"), What), Result.Error.IsEmpty());
	};
	auto PatchU32 = [](TArray<uint8> Block, const int32 Offset, const uint32 Value)
	{
		FMemory::Memcpy(Block.GetData() + Offset, &Value, 4);
		return Block;
	};

	// The positive control: the unpatched block reads.
	const TArray<uint8> Good = MakeBlock({ 0, 3 });
	TestTrue(TEXT("the unpatched block reads"), FMantleSkinnedMeshReader::Read(MakeMsk1WithBlock(Good)).bSuccess);

	Refused(TEXT("version 2"), PatchU32(Good, 0, 2));
	Refused(TEXT("a wrong LOD count"), PatchU32(Good, 4, 1));
	Refused(TEXT("a wrong bone count"), PatchU32(Good, 8, 3));
	Refused(TEXT("nine shapes"), PatchU32(Good, 12, 9));
	Refused(TEXT("one shape short of its count"), PatchU32(Good, 12, 3));

	TArray<uint8> Short = Good;
	Short.SetNum(Short.Num() - 1);
	Refused(TEXT("a block a byte short"), Short);
	TArray<uint8> Trailing = Good;
	Trailing.Add(0);
	Refused(TEXT("a block with a trailing byte"), Trailing);
	Refused(TEXT("an empty block"), {});

	const int32 SecondShapeId = 16 + 1 + (NumLODs * NumVertices + NumBones) * 12;
	TArray<uint8> Descending = Good;
	Descending[16] = 3;
	Descending[SecondShapeId] = 0;
	Refused(TEXT("descending ids"), Descending);
	TArray<uint8> Repeated = Good;
	Repeated[SecondShapeId] = 0;
	Refused(TEXT("a repeated id"), Repeated);
	TArray<uint8> OutOfRange = Good;
	OutOfRange[SecondShapeId] = 8;
	Refused(TEXT("shape id 8"), OutOfRange);

	TArray<uint8> NotFinite = Good;
	const float NaN = std::numeric_limits<float>::quiet_NaN();
	FMemory::Memcpy(NotFinite.GetData() + 17 + 8, &NaN, 4);
	Refused(TEXT("a NaN delta"), NotFinite);
	TArray<uint8> Infinite = Good;
	const float Inf = std::numeric_limits<float>::infinity();
	FMemory::Memcpy(Infinite.GetData() + SecondShapeId + 1 + (NumLODs * NumVertices * 12), &Inf, 4);
	Refused(TEXT("an infinite bone delta"), Infinite);

	TestFalse(TEXT("two BOFS blocks are refused"), FMantleSkinnedMeshReader::Read(MakeMsk1({ { "BOFS", Good }, { "BOFS", Good } })).bSuccess);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleBodyOffsetsApplyTest,
	"Mantle.Runtime.BodyOffsets.Apply",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleBodyOffsetsApplyTest::RunTest(const FString& Parameters)
{
	using namespace MantleBodyOffsetsTestPrivate;

	FMantleSkinnedMesh Base;
	if (!ReadBody(*this, Base))
	{
		return false;
	}

	// Nothing asked, nothing moved: bit-identical, normals and tangents included.
	{
		FMantleSkinnedMesh Same = Base;
		FMantleBodyOffsets::Apply(Same, 0, 0, 0, 0);
		TestTrue(TEXT("all zeros leaves the mesh bit-identical"), SameMesh(Same, Base));
		FMantleBodyOffsets::Apply(Same, 0, 0, 0, 100);
		TestTrue(TEXT("a slider whose shapes the body lacks (hips) leaves it bit-identical"), SameMesh(Same, Base));
		FMantleSkinnedMesh NoOffsets = Base;
		NoOffsets.BodyOffsets = FMantleBodyOffsets();
		FMantleSkinnedMesh Reference = NoOffsets;
		FMantleBodyOffsets::Apply(NoOffsets, 100, 100, 100, 100);
		TestTrue(TEXT("a body without offsets stays bit-identical"), SameMesh(NoOffsets, Reference));
	}

	// Weight 100 adds exactly the + shape; -50 adds half the - shape. Vertex 0 sits at the origin and bone 1 at x = 5.
	FMantleSkinnedMesh Plus = Base;
	FMantleBodyOffsets::Apply(Plus, 100, 0, 0, 0);
	FMantleSkinnedMesh Minus = Base;
	FMantleBodyOffsets::Apply(Minus, -50, 0, 0, 0);
	for (int32 LOD = 0; LOD < NumLODs; ++LOD)
	{
		for (int32 Vertex = 0; Vertex < NumVertices; ++Vertex)
		{
			const FVector3f Rest = Base.LODs[LOD].Positions[Vertex];
			TestTrue(*FString::Printf(TEXT("weight +100, LOD%d vertex %d"), LOD, Vertex),
				Plus.LODs[LOD].Positions[Vertex] == Rest + VertexDelta(0, LOD, Vertex));
			TestTrue(*FString::Printf(TEXT("weight -50, LOD%d vertex %d"), LOD, Vertex),
				Minus.LODs[LOD].Positions[Vertex] == Rest + VertexDelta(1, LOD, Vertex) * 0.5f);
		}
	}
	for (int32 Bone = 0; Bone < NumBones; ++Bone)
	{
		const FVector Rest = Base.ReferencePose[Bone].GetTranslation();
		TestTrue(*FString::Printf(TEXT("weight +100, bone %d"), Bone),
			NearlyEqual(FVector3f(Plus.ReferencePose[Bone].GetTranslation() - Rest), BoneDelta(0, Bone)));
		TestTrue(*FString::Printf(TEXT("weight -50, bone %d"), Bone),
			NearlyEqual(FVector3f(Minus.ReferencePose[Bone].GetTranslation() - Rest), BoneDelta(1, Bone) * 0.5f));
		TestTrue(*FString::Printf(TEXT("bone %d keeps its rotation"), Bone),
			Plus.ReferencePose[Bone].GetRotation().Equals(Base.ReferencePose[Bone].GetRotation(), 0.0f));
	}
	TestFalse(TEXT("moving changes the normals"), SameBits(Plus.LODs[0].Normals, Base.LODs[0].Normals));

	// Out-of-range sliders clamp, and two shapes add: weight -100 and muscle +50 (shapes 1 and 2).
	FMantleSkinnedMesh Both = Base;
	FMantleBodyOffsets::Apply(Both, -250, 50, 0, 0);
	for (int32 Vertex = 0; Vertex < NumVertices; ++Vertex)
	{
		TestTrue(*FString::Printf(TEXT("two shapes add, vertex %d"), Vertex),
			Both.LODs[1].Positions[Vertex] == Base.LODs[1].Positions[Vertex] + VertexDelta(1, 1, Vertex) + VertexDelta(2, 1, Vertex) * 0.5f);
	}
	TestTrue(TEXT("two shapes add, bone 1"),
		NearlyEqual(FVector3f(Both.ReferencePose[1].GetTranslation() - Base.ReferencePose[1].GetTranslation()), BoneDelta(1, 1) + BoneDelta(2, 1) * 0.5f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleBodyOffsetsNormalsTest,
	"Mantle.Runtime.BodyOffsets.Normals",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleBodyOffsetsNormalsTest::RunTest(const FString& Parameters)
{
	using namespace MantleBodyOffsetsTestPrivate;

	FMantleSkinnedMesh Mesh;
	if (!ReadBody(*this, Mesh))
	{
		return false;
	}
	FMantleBodyOffsets::Apply(Mesh, 100, 0, 0, 0);

	for (int32 LOD = 0; LOD < NumLODs; ++LOD)
	{
		const FMantleSkinnedMeshLOD& Moved = Mesh.LODs[LOD];
		for (int32 Vertex = 0; Vertex < NumVertices; ++Vertex)
		{
			TestTrue(*FString::Printf(TEXT("LOD%d vertex %d normal is unit length"), LOD, Vertex), FMath::Abs(Moved.Normals[Vertex].Size() - 1.0f) < 1e-4f);
			const FVector3f Tangent(Moved.Tangents[Vertex].X, Moved.Tangents[Vertex].Y, Moved.Tangents[Vertex].Z);
			TestTrue(*FString::Printf(TEXT("LOD%d vertex %d tangent is unit length"), LOD, Vertex), FMath::Abs(Tangent.Size() - 1.0f) < 1e-4f);
			TestTrue(*FString::Printf(TEXT("LOD%d vertex %d tangent is square to the normal"), LOD, Vertex),
				FMath::Abs(FVector3f::DotProduct(Tangent, Moved.Normals[Vertex])) < 1e-4f);
			TestEqual(*FString::Printf(TEXT("LOD%d vertex %d tangent keeps its sign"), LOD, Vertex), Moved.Tangents[Vertex].W, Vertex >= 3 ? -1.0f : 1.0f);
		}
		// The seam's copies (same position, different UV) share one normal, exactly.
		TestTrue(*FString::Printf(TEXT("LOD%d seam copies 1 and 3 agree"), LOD), Moved.Normals[1] == Moved.Normals[3]);
		TestTrue(*FString::Printf(TEXT("LOD%d seam copies 2 and 5 agree"), LOD), Moved.Normals[2] == Moved.Normals[5]);
		// The positive control: the offsets tilted the flat quad, so the normals are no longer +Z.
		TestTrue(*FString::Printf(TEXT("LOD%d normals really changed"), LOD), Moved.Normals[0].Z < 0.9999f);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleBodyOffsetsKeyTest,
	"Mantle.Runtime.BodyOffsets.Key",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleBodyOffsetsKeyTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("all zeros gives the empty key"), FMantleBodyOffsets::Key(0, 0, 0, 0).IsEmpty());
	TestFalse(TEXT("one slider gives a key"), FMantleBodyOffsets::Key(0, 0, 0, 1).IsEmpty());
	TestFalse(TEXT("different values give different keys"), FMantleBodyOffsets::Key(10, 0, 0, 0) == FMantleBodyOffsets::Key(0, 10, 0, 0));
	TestTrue(TEXT("values past the range key like the range's end"), FMantleBodyOffsets::Key(500, -500, 0, 0) == FMantleBodyOffsets::Key(100, -100, 0, 0));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
