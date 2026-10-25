// Copyright Joshua Eger. All Rights Reserved.
#include "MantleBodyOffsetsBuilder.h"
#include "MantleGlbWriter.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Math/RandomStream.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// Body-shape offsets come from two evaluations of MetaHuman Creator's body state (normal and reshaped) mapped
// onto the converter's built body, and ride the upload in the root extension MANTLE_body_offsets.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleBodyOffsetsBuilderTestPrivate
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	bool Near(const FVector3f& A, const FVector3f& B, float Tol = 1e-3f)
	{
		return FVector3f::Distance(A, B) <= Tol;
	}

	/** A skinned triangle with two joints (root, child), the smallest mesh the writer takes with a skin. */
	FMantleGlbMesh MakeSkinnedTriangle()
	{
		FMantleGlbMesh Mesh;
		Mesh.Name = TEXT("OffsetsTriangle");
		Mesh.Positions = { FVector3f(0, 0, 0), FVector3f(100, 0, 0), FVector3f(0, 100, 0) };
		Mesh.Normals = { FVector3f(0, 0, 1), FVector3f(0, 0, 1), FVector3f(0, 0, 1) };
		Mesh.UVs = { FVector2f(0, 0), FVector2f(1, 0), FVector2f(0, 1) };
		Mesh.MaterialNames = { TEXT("Skin") };
		FMantleGlbPrimitive& Primitive = Mesh.Primitives.AddDefaulted_GetRef();
		Primitive.NumVertices = 3;
		Primitive.Indices = { 0, 1, 2 };

		FMantleGlbJoint& Root = Mesh.Joints.AddDefaulted_GetRef();
		Root.Name = TEXT("root");
		FMantleGlbJoint& Child = Mesh.Joints.AddDefaulted_GetRef();
		Child.Name = TEXT("child");
		Child.Parent = 0;
		Child.Local.SetTranslation(FVector(0, 0, 50));

		Mesh.NumInfluences = 1;
		Mesh.InfluenceJoints = { 0, 1, 1 };
		Mesh.InfluenceWeights = { 1.0f, 1.0f, 1.0f };
		return Mesh;
	}

	FMantleBodyOffsetShape MakeShape(const TCHAR* Name, int32 NumLODs, int32 NumVertices, int32 NumJoints)
	{
		FMantleBodyOffsetShape Shape;
		Shape.Name = Name;
		Shape.LODs.SetNum(NumLODs);
		for (TArray<FVector3f>& Lod : Shape.LODs)
		{
			Lod.Init(FVector3f(100.0f, 200.0f, 300.0f), NumVertices);
		}
		Shape.Joints.Init(FVector3f(10.0f, 20.0f, 30.0f), NumJoints);
		return Shape;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleBodyOffsetsLearnMapTest, "Mantle.Editor.BodyOffsets.LearnVertexMap", MantleBodyOffsetsBuilderTestPrivate::Flags)

bool FMantleBodyOffsetsLearnMapTest::RunTest(const FString& Parameters)
{
	using namespace MantleBodyOffsetsBuilderTestPrivate;
	FRandomStream Random(849);

	TArray<FVector3f> State;
	for (int32 Index = 0; Index < 300; ++Index)
	{
		State.Add(FVector3f(Random.FRandRange(-100.0f, 100.0f), Random.FRandRange(-100.0f, 100.0f), Random.FRandRange(0.0f, 180.0f)));
	}
	// A seam on the state side too: the same position again at a higher index.
	const FVector3f Seam = State[7];
	State.Add(Seam);

	// The built body: a shuffled subset of the state, axis-swapped, with some vertices doubled (UV seams).
	TArray<int32> Picks;
	for (int32 Index = 0; Index < 200; ++Index)
	{
		Picks.Add(Index);
	}
	for (int32 Index = Picks.Num() - 1; Index > 0; --Index)
	{
		Picks.Swap(Index, Random.RandRange(0, Index));
	}
	TArray<FVector3f> Built;
	for (const int32 Pick : Picks)
	{
		Built.Add(FMantleBodyOffsetsBuilder::StateToBuilt(State[Pick]));
	}
	const FVector3f Doubled = Built[3];
	Built.Add(Doubled);
	Built.Add(Doubled);
	Built.Add(FMantleBodyOffsetsBuilder::StateToBuilt(State[7]));

	TArray<int32> Map;
	FString Error;
	if (!TestTrue(*FString::Printf(TEXT("the map is learned (%s)"), *Error), FMantleBodyOffsetsBuilder::LearnVertexMap(Built, State, Map, Error)))
	{
		return false;
	}
	if (!TestEqual(TEXT("one state vertex per built vertex"), Map.Num(), Built.Num()))
	{
		return false;
	}
	for (int32 Vertex = 0; Vertex < Built.Num(); ++Vertex)
	{
		if (!Near(FMantleBodyOffsetsBuilder::StateToBuilt(State[Map[Vertex]]), Built[Vertex], 1e-6f))
		{
			AddError(FString::Printf(TEXT("built vertex %d maps to a state vertex somewhere else"), Vertex));
			return false;
		}
	}
	TestEqual(TEXT("doubled built vertices map to the same state vertex"), Map[Built.Num() - 3], Map[3]);
	TestEqual(TEXT("two state vertices at one position: the lowest index"), Map.Last(), 7);

	// Two different state positions both within tolerance of a built vertex is an error, not a guess.
	TArray<FVector3f> Ambiguous = { FVector3f(0, 0, 0), FVector3f(0.0005f, 0, 0) };
	TArray<FVector3f> AmbiguousBuilt = { FVector3f(0.00025f, 0, 0) };
	TArray<int32> AmbiguousMap;
	TestFalse(TEXT("an ambiguous match fails"), FMantleBodyOffsetsBuilder::LearnVertexMap(AmbiguousBuilt, Ambiguous, AmbiguousMap, Error));
	TestTrue(TEXT("and says why"), Error.Contains(TEXT("different positions")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleBodyOffsetsMissingVertexTest, "Mantle.Editor.BodyOffsets.MissingVertexFails", MantleBodyOffsetsBuilderTestPrivate::Flags)

bool FMantleBodyOffsetsMissingVertexTest::RunTest(const FString& Parameters)
{
	// The built vertex (1, 2, 3) wants a state vertex at (1, 3, 2); only (1, 2, 3) is there, which is the wrong axis order.
	const TArray<FVector3f> State = { FVector3f(0, 0, 0), FVector3f(1, 2, 3) };
	const TArray<FVector3f> Built = { FVector3f(0, 0, 0), FVector3f(1, 2, 3) };
	TArray<int32> Map;
	FString Error;
	TestFalse(TEXT("a built vertex with no match fails"), FMantleBodyOffsetsBuilder::LearnVertexMap(Built, State, Map, Error));
	TestTrue(TEXT("naming the vertex"), Error.Contains(TEXT("built vertex 1")));
	TestEqual(TEXT("and leaves no map"), Map.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleBodyOffsetsPositionDeltasTest, "Mantle.Editor.BodyOffsets.PositionDeltas", MantleBodyOffsetsBuilderTestPrivate::Flags)

bool FMantleBodyOffsetsPositionDeltasTest::RunTest(const FString& Parameters)
{
	using namespace MantleBodyOffsetsBuilderTestPrivate;
	const TArray<FVector3f> Normal = { FVector3f(0, 0, 0), FVector3f(10, 20, 30), FVector3f(5, 5, 5) };
	const TArray<FVector3f> Shape = { FVector3f(0, 0, 0), FVector3f(11, 22, 33), FVector3f(5, 5, 4) };
	const TArray<int32> Map = { 1, 2, 1 };
	const TArray<FVector3f> Deltas = FMantleBodyOffsetsBuilder::PositionDeltas(Normal, Shape, Map);
	if (!TestEqual(TEXT("one delta per built vertex"), Deltas.Num(), 3))
	{
		return false;
	}
	// State delta (1, 2, 3) is (1, 3, 2) in the built body's axes.
	TestTrue(TEXT("the delta is in built axes"), Near(Deltas[0], FVector3f(1, 3, 2)));
	TestTrue(TEXT("through the map"), Near(Deltas[1], FVector3f(0, -1, 0)));
	TestTrue(TEXT("a doubled vertex gets the same delta"), Near(Deltas[2], Deltas[0]));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleBodyOffsetsStripVerticalTest, "Mantle.Editor.BodyOffsets.StripVerticalOffsetKeepsFeet", MantleBodyOffsetsBuilderTestPrivate::Flags)

bool FMantleBodyOffsetsStripVerticalTest::RunTest(const FString& Parameters)
{
	using namespace MantleBodyOffsetsBuilderTestPrivate;
	// Feet at Z 2 and 3; the shape lifts them by 4.5 and 5.5, so the lowest point rises by 4.5.
	const TArray<FVector3f> Normal = { FVector3f(0, 0, 2), FVector3f(1, 0, 3), FVector3f(0, 0, 90) };
	TArray<TArray<FVector3f>> Lods;
	Lods.Add({ FVector3f(1, 0, 4.5f), FVector3f(0, 2, 5.5f), FVector3f(0, 0, 10) });
	Lods.Add({ FVector3f(7, 0, 4.5f), FVector3f(0, 0, 4.5f) });
	// Joints: 0 and 2 are roots, 1 is 0's child (its delta must stay as it is).
	const TArray<int32> Parents = { INDEX_NONE, 0, INDEX_NONE };
	TArray<FVector3f> Joints = { FVector3f(0, 0, 8), FVector3f(0, 0, 1), FVector3f(3, 0, 6) };

	const float Shift = FMantleBodyOffsetsBuilder::StripVerticalOffset(Normal, Lods, Parents, Joints);
	TestEqual(TEXT("the removed shift is the rise of the lowest point"), Shift, 4.5f);

	float MinZ = TNumericLimits<float>::Max();
	for (int32 Vertex = 0; Vertex < Normal.Num(); ++Vertex)
	{
		MinZ = FMath::Min(MinZ, Normal[Vertex].Z + Lods[0][Vertex].Z);
	}
	TestEqual(TEXT("the lowest point is where it was"), MinZ, 2.0f, 1e-4f);
	TestTrue(TEXT("LOD0's other deltas move by the same Z"), Near(Lods[0][2], FVector3f(0, 0, 5.5f)));
	TestTrue(TEXT("lower LODs lose the same Z"), Near(Lods[1][0], FVector3f(7, 0, 0)));
	TestTrue(TEXT("a root joint loses it"), Near(Joints[0], FVector3f(0, 0, 3.5f)));
	TestTrue(TEXT("another root loses it"), Near(Joints[2], FVector3f(3, 0, 1.5f)));
	TestTrue(TEXT("a child joint does not: it inherits the root's"), Near(Joints[1], FVector3f(0, 0, 1)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleBodyOffsetsLocalJointsTest, "Mantle.Editor.BodyOffsets.LocalJointDeltasRecompose", MantleBodyOffsetsBuilderTestPrivate::Flags)

bool FMantleBodyOffsetsLocalJointsTest::RunTest(const FString& Parameters)
{
	using namespace MantleBodyOffsetsBuilderTestPrivate;
	// root -> mid (rotated 90 degrees about Z) -> tip, in a reference pose with a turned middle joint.
	const TArray<int32> Parents = { INDEX_NONE, 0, 1 };
	const TArray<FQuat4f> LocalRotations = { FQuat4f::Identity, FQuat4f(FVector3f(0, 0, 1), HALF_PI), FQuat4f(FVector3f(1, 0, 0), 0.3f) };
	const TArray<FVector3f> LocalTranslations = { FVector3f(0, 0, 100), FVector3f(0, 10, 0), FVector3f(0, 20, 5) };
	const TArray<FVector3f> ComponentDeltas = { FVector3f(0, 0, 2), FVector3f(1, 2, 3), FVector3f(-4, 5, 6) };

	TArray<FQuat4f> ComponentRotations;
	FString Error;
	if (!TestTrue(*FString::Printf(TEXT("rotations compose (%s)"), *Error), FMantleBodyOffsetsBuilder::ComposeComponentRotations(Parents, LocalRotations, ComponentRotations, Error)))
	{
		return false;
	}
	TArray<FVector3f> LocalDeltas;
	if (!TestTrue(*FString::Printf(TEXT("local deltas convert (%s)"), *Error), FMantleBodyOffsetsBuilder::LocalJointDeltas(Parents, ComponentRotations, ComponentDeltas, LocalDeltas, Error)))
	{
		return false;
	}
	TestTrue(TEXT("a root's delta is its component delta"), Near(LocalDeltas[0], ComponentDeltas[0]));

	// Recompose component positions from local translations with and without the deltas.
	auto ComponentPositions = [&](const bool bWithDeltas)
	{
		TArray<FVector3f> Positions;
		for (int32 Joint = 0; Joint < 3; ++Joint)
		{
			const FVector3f Local = LocalTranslations[Joint] + (bWithDeltas ? LocalDeltas[Joint] : FVector3f::ZeroVector);
			Positions.Add(Parents[Joint] == INDEX_NONE ? Local : Positions[Parents[Joint]] + ComponentRotations[Parents[Joint]].RotateVector(Local));
		}
		return Positions;
	};
	const TArray<FVector3f> Before = ComponentPositions(false);
	const TArray<FVector3f> After = ComponentPositions(true);
	for (int32 Joint = 0; Joint < 3; ++Joint)
	{
		TestTrue(*FString::Printf(TEXT("joint %d lands where its component delta put it"), Joint), Near(After[Joint] - Before[Joint], ComponentDeltas[Joint], 1e-3f));
	}
	// The rotated parent matters: the child's local delta is not its raw component difference.
	TestFalse(TEXT("the rotated parent turned the child's delta"), Near(LocalDeltas[2], ComponentDeltas[2] - ComponentDeltas[1]));

	TArray<int32> Bad = { INDEX_NONE, 2, 0 };
	TestFalse(TEXT("a child listed before its parent is refused"), FMantleBodyOffsetsBuilder::ComposeComponentRotations(Bad, LocalRotations, ComponentRotations, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleBodyOffsetsGlbWriterTest, "Mantle.Editor.BodyOffsets.GlbWriterEmitsExtension", MantleBodyOffsetsBuilderTestPrivate::Flags)

bool FMantleBodyOffsetsGlbWriterTest::RunTest(const FString& Parameters)
{
	using namespace MantleBodyOffsetsBuilderTestPrivate;
	FMantleGlbMesh Mesh = MakeSkinnedTriangle();
	Mesh.BodyOffsets.Add(MakeShape(TEXT("weight+"), 1, 3, 2));
	Mesh.BodyOffsets.Add(MakeShape(TEXT("hips-"), 1, 3, 2));
	Mesh.BodyOffsets[1].LODs[0][2] = FVector3f(-50.0f, 0.0f, 25.0f);

	TArray<uint8> Bytes;
	FString Error;
	if (!TestTrue(*FString::Printf(TEXT("the GLB writes (%s)"), *Error), FMantleGlbWriter::Write(Mesh, Bytes, Error)))
	{
		return false;
	}
	FMantleGlbDocument Document;
	if (!TestTrue(*FString::Printf(TEXT("it parses (%s)"), *Error), FMantleGlbDocument::Parse(Bytes, Document, Error)))
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Used = nullptr;
	if (!TestTrue(TEXT("extensionsUsed lists it"), Document.Json->TryGetArrayField(TEXT("extensionsUsed"), Used)))
	{
		return false;
	}
	TestTrue(TEXT("by name"), Used->ContainsByPredicate([](const TSharedPtr<FJsonValue>& Value) { return Value->AsString() == TEXT("MANTLE_body_offsets"); }));

	const TSharedPtr<FJsonObject>* Extensions = nullptr;
	const TSharedPtr<FJsonObject>* Offsets = nullptr;
	if (!TestTrue(TEXT("the root has extensions"), Document.Json->TryGetObjectField(TEXT("extensions"), Extensions))
		|| !TestTrue(TEXT("with the body offsets"), (*Extensions)->TryGetObjectField(TEXT("MANTLE_body_offsets"), Offsets)))
	{
		return false;
	}
	TestEqual(TEXT("version 1"), static_cast<int32>((*Offsets)->GetNumberField(TEXT("version"))), 1);
	const TArray<TSharedPtr<FJsonValue>>* Shapes = nullptr;
	if (!TestTrue(TEXT("shapes"), (*Offsets)->TryGetArrayField(TEXT("shapes"), Shapes)) || !TestEqual(TEXT("both shapes"), Shapes->Num(), 2))
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Accessors = nullptr;
	Document.Json->TryGetArrayField(TEXT("accessors"), Accessors);
	auto CheckAccessor = [&](const int32 Index, const int32 ExpectedCount, const TCHAR* What, TArray<double>& OutValues)
	{
		if (!Accessors || !Accessors->IsValidIndex(Index))
		{
			AddError(FString::Printf(TEXT("%s: no accessor %d"), What, Index));
			return false;
		}
		const TSharedPtr<FJsonObject> Json = (*Accessors)[Index]->AsObject();
		TestEqual(*FString::Printf(TEXT("%s is VEC3"), What), Json->GetStringField(TEXT("type")), FString(TEXT("VEC3")));
		TestEqual(*FString::Printf(TEXT("%s is float"), What), static_cast<int32>(Json->GetNumberField(TEXT("componentType"))), 5126);
		int32 Components = 0, Count = 0;
		if (!Document.ReadAccessor(Index, OutValues, &Components, &Count))
		{
			AddError(FString::Printf(TEXT("%s cannot be read"), What));
			return false;
		}
		TestEqual(*FString::Printf(TEXT("%s has one element per %s"), What, What), Count, ExpectedCount);
		return Components == 3 && Count == ExpectedCount;
	};

	const TSharedPtr<FJsonObject> First = (*Shapes)[0]->AsObject();
	TestEqual(TEXT("the first shape's name"), First->GetStringField(TEXT("name")), FString(TEXT("weight+")));
	const TArray<TSharedPtr<FJsonValue>>& FirstLods = First->GetArrayField(TEXT("lods"));
	if (!TestEqual(TEXT("one accessor a LOD"), FirstLods.Num(), 1))
	{
		return false;
	}
	TArray<double> Values;
	if (CheckAccessor(static_cast<int32>(FirstLods[0]->AsNumber()), 3, TEXT("LOD0 vertex deltas"), Values))
	{
		// Unreal (100, 200, 300) cm is glTF (1, 3, 2) m: x kept, y and z swapped, centimetres to metres, no translation.
		TestEqual(TEXT("x in metres"), static_cast<float>(Values[0]), 1.0f, 1e-5f);
		TestEqual(TEXT("y is Unreal's z"), static_cast<float>(Values[1]), 3.0f, 1e-5f);
		TestEqual(TEXT("z is Unreal's y"), static_cast<float>(Values[2]), 2.0f, 1e-5f);
	}
	if (CheckAccessor(static_cast<int32>(First->GetNumberField(TEXT("joints"))), 2, TEXT("joint deltas"), Values))
	{
		TestEqual(TEXT("joint x"), static_cast<float>(Values[3]), 0.1f, 1e-5f);
		TestEqual(TEXT("joint y"), static_cast<float>(Values[4]), 0.3f, 1e-5f);
		TestEqual(TEXT("joint z"), static_cast<float>(Values[5]), 0.2f, 1e-5f);
	}

	const TSharedPtr<FJsonObject> Second = (*Shapes)[1]->AsObject();
	TestEqual(TEXT("the second shape's name"), Second->GetStringField(TEXT("name")), FString(TEXT("hips-")));
	if (CheckAccessor(static_cast<int32>(Second->GetArrayField(TEXT("lods"))[0]->AsNumber()), 3, TEXT("second shape's vertex deltas"), Values))
	{
		TestEqual(TEXT("its own value: x"), static_cast<float>(Values[6]), -0.5f, 1e-5f);
		TestEqual(TEXT("its own value: y"), static_cast<float>(Values[7]), 0.25f, 1e-5f);
		TestEqual(TEXT("its own value: z"), static_cast<float>(Values[8]), 0.0f, 1e-5f);
	}

	// Counts that do not match what is written are refused, not written.
	{
		FMantleGlbMesh Wrong = MakeSkinnedTriangle();
		Wrong.BodyOffsets.Add(MakeShape(TEXT("weight+"), 1, 2, 2));
		TestFalse(TEXT("a vertex count that is not the LOD's fails"), FMantleGlbWriter::Write(Wrong, Bytes, Error));
		TestTrue(TEXT("naming the vertices"), Error.Contains(TEXT("vertices")));
	}
	{
		FMantleGlbMesh Wrong = MakeSkinnedTriangle();
		Wrong.BodyOffsets.Add(MakeShape(TEXT("weight+"), 1, 3, 1));
		TestFalse(TEXT("a joint count that is not the skin's fails"), FMantleGlbWriter::Write(Wrong, Bytes, Error));
		TestTrue(TEXT("naming the joints"), Error.Contains(TEXT("joint")));
	}
	{
		FMantleGlbMesh Wrong = MakeSkinnedTriangle();
		Wrong.BodyOffsets.Add(MakeShape(TEXT("weight+"), 2, 3, 2));
		TestFalse(TEXT("a LOD count that is not the file's fails"), FMantleGlbWriter::Write(Wrong, Bytes, Error));
	}
	{
		FMantleGlbMesh Wrong = MakeSkinnedTriangle();
		Wrong.BodyOffsets.Add(MakeShape(TEXT("belly+"), 1, 3, 2));
		TestFalse(TEXT("an unknown shape name fails"), FMantleGlbWriter::Write(Wrong, Bytes, Error));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleBodyOffsetsEmptyFieldTest, "Mantle.Editor.BodyOffsets.EmptyFieldChangesNothing", MantleBodyOffsetsBuilderTestPrivate::Flags)

bool FMantleBodyOffsetsEmptyFieldTest::RunTest(const FString& Parameters)
{
	using namespace MantleBodyOffsetsBuilderTestPrivate;
	FMantleGlbMesh Plain = MakeSkinnedTriangle();
	FMantleGlbMesh Emptied = MakeSkinnedTriangle();
	Emptied.BodyOffsets.Reset();
	FMantleGlbMesh With = MakeSkinnedTriangle();
	With.BodyOffsets.Add(MakeShape(TEXT("muscle+"), 1, 3, 2));

	TArray<uint8> PlainBytes, EmptiedBytes, WithBytes;
	FString Error;
	if (!TestTrue(TEXT("plain writes"), FMantleGlbWriter::Write(Plain, PlainBytes, Error))
		|| !TestTrue(TEXT("emptied writes"), FMantleGlbWriter::Write(Emptied, EmptiedBytes, Error))
		|| !TestTrue(TEXT("with offsets writes"), FMantleGlbWriter::Write(With, WithBytes, Error)))
	{
		return false;
	}
	TestTrue(TEXT("an empty field writes the same bytes"), PlainBytes == EmptiedBytes);
	TestTrue(TEXT("offsets change the file"), PlainBytes != WithBytes);

	FMantleGlbDocument Document;
	if (TestTrue(TEXT("the plain file parses"), FMantleGlbDocument::Parse(PlainBytes, Document, Error)))
	{
		TestFalse(TEXT("and has no extensions block"), Document.Json->HasField(TEXT("extensions")));
		TestFalse(TEXT("or a list of extensions used"), Document.Json->HasField(TEXT("extensionsUsed")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleBodyOffsetsNearestMapTest, "Mantle.Editor.BodyOffsets.LearnNearestVertexMap", MantleBodyOffsetsBuilderTestPrivate::Flags)

bool FMantleBodyOffsetsNearestMapTest::RunTest(const FString& Parameters)
{
	// The face: most built vertices are exact in Creator's refitted face, the neck ones are a millimetre or so off.
	const TArray<FVector3f> State = { FVector3f(0, 0, 0), FVector3f(10, 30, 20), FVector3f(10.05f, 30.0f, 20.0f), FVector3f(50, 50, 50) };
	// Built axes are (x, z, y): state (10, 30, 20) is built (10, 20, 30).
	const TArray<FVector3f> Built = { FVector3f(50, 50, 50), FVector3f(10, 20, 30), FVector3f(10.12f, 20.0f, 30.0f) };
	TArray<int32> Map;
	FString Error;
	int32 Exact = 0;
	float Worst = 0.0f;
	if (!TestTrue(*FString::Printf(TEXT("a vertex 0.07 cm off still maps (%s)"), *Error),
		FMantleBodyOffsetsBuilder::LearnNearestVertexMap(Built, State, Map, Error, 0.2f, &Exact, &Worst)))
	{
		return false;
	}
	TestEqual(TEXT("the exact ones go to their own vertex"), Map[0], 3);
	TestEqual(TEXT("an exact hit beats a near one"), Map[1], 1);
	TestEqual(TEXT("a near vertex takes the nearest, not the first in reach"), Map[2], 2);
	TestEqual(TEXT("two of three were exact"), Exact, 2);
	TestEqual(TEXT("the worst distance used"), Worst, 0.07f, 1e-3f);

	const TArray<FVector3f> TooFar = { FVector3f(10.5f, 20.0f, 30.0f) };
	TestFalse(TEXT("a vertex out of reach fails"), FMantleBodyOffsetsBuilder::LearnNearestVertexMap(TooFar, State, Map, Error, 0.2f));
	TestTrue(TEXT("naming the vertex"), Error.Contains(TEXT("built vertex 0")));
	TestEqual(TEXT("and leaves no map"), Map.Num(), 0);

	// Two state vertices at the same distance: the lowest index wins, so the map does not depend on iteration order.
	const TArray<FVector3f> Tied = { FVector3f(1, 0, 0), FVector3f(1, 0, 0) };
	const TArray<FVector3f> OnTie = { FVector3f(1, 0, 0) };
	TestTrue(TEXT("a tie maps"), FMantleBodyOffsetsBuilder::LearnNearestVertexMap(OnTie, Tied, Map, Error, 0.2f));
	TestEqual(TEXT("to the lowest index"), Map[0], 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleBodyOffsetsJointNamesTest, "Mantle.Editor.BodyOffsets.JointDeltasByName", MantleBodyOffsetsBuilderTestPrivate::Flags)

bool FMantleBodyOffsetsJointNamesTest::RunTest(const FString& Parameters)
{
	using namespace MantleBodyOffsetsBuilderTestPrivate;
	// A face skin: root (not in the rig), neck_01 and head (shared with the body), then facial joints under the head and one
	// helper nobody named under the neck.
	const TArray<FName> Names = { FName(TEXT("root")), FName(TEXT("neck_01")), FName(TEXT("head")), FName(TEXT("FACIAL_C_Jaw")), FName(TEXT("FACIAL_L_Brow")), FName(TEXT("neck_twist")) };
	const TArray<int32> Parents = { INDEX_NONE, 0, 1, 2, 2, 1 };
	TMap<FName, FVector3f> Named;
	Named.Add(FName(TEXT("neck_01")), FVector3f(0, 0, 2));
	Named.Add(FName(TEXT("head")), FVector3f(1, 0, 3));
	Named.Add(FName(TEXT("spine_01")), FVector3f(9, 9, 9));

	TArray<FVector3f> Component;
	const int32 Taken = FMantleBodyOffsetsBuilder::ComponentDeltasByName(Names, Parents, Named, Component);
	TestEqual(TEXT("two joints took a delta from the rig; the body-only name is ignored"), Taken, 2);
	if (!TestEqual(TEXT("one delta per joint"), Component.Num(), 6))
	{
		return false;
	}
	TestTrue(TEXT("an unnamed root stays still"), Near(Component[0], FVector3f::ZeroVector));
	TestTrue(TEXT("a shared bone takes the body's delta"), Near(Component[2], FVector3f(1, 0, 3)));
	TestTrue(TEXT("a face-only bone follows its parent"), Near(Component[3], Component[2]));
	TestTrue(TEXT("and so does the other"), Near(Component[4], Component[2]));
	TestTrue(TEXT("a helper follows the neck"), Near(Component[5], Component[1]));

	// In local terms the followers move by nothing, whatever the rest pose.
	TArray<FQuat4f> Rotations;
	for (int32 Joint = 0; Joint < 6; ++Joint)
	{
		Rotations.Add(FQuat4f(FVector3f(0, 0, 1), 0.4f * Joint));
	}
	TArray<FQuat4f> ComponentRotations;
	TArray<FVector3f> Local;
	FString Error;
	if (!TestTrue(*FString::Printf(TEXT("rotations compose (%s)"), *Error), FMantleBodyOffsetsBuilder::ComposeComponentRotations(Parents, Rotations, ComponentRotations, Error))
		|| !TestTrue(*FString::Printf(TEXT("local deltas convert (%s)"), *Error), FMantleBodyOffsetsBuilder::LocalJointDeltas(Parents, ComponentRotations, Component, Local, Error)))
	{
		return false;
	}
	TestTrue(TEXT("a facial joint's local delta is zero"), Near(Local[3], FVector3f::ZeroVector));
	TestTrue(TEXT("a helper's local delta is zero"), Near(Local[5], FVector3f::ZeroVector));
	TestFalse(TEXT("the head's is not"), Near(Local[2], FVector3f::ZeroVector));

	const TArray<int32> OutOfOrder = { INDEX_NONE, 2, 0, 2, 2, 1 };
	const TArray<int32> TooShort = { INDEX_NONE };
	TestEqual(TEXT("a child before its parent is refused"), FMantleBodyOffsetsBuilder::ComponentDeltasByName(Names, OutOfOrder, Named, Component), static_cast<int32>(INDEX_NONE));
	TestEqual(TEXT("and names and parents must agree"), FMantleBodyOffsetsBuilder::ComponentDeltasByName(Names, TooShort, Named, Component), static_cast<int32>(INDEX_NONE));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleBodyOffsetsFaceSharesShiftTest, "Mantle.Editor.BodyOffsets.FaceTakesTheBodysFloorShift", MantleBodyOffsetsBuilderTestPrivate::Flags)

bool FMantleBodyOffsetsFaceSharesShiftTest::RunTest(const FString& Parameters)
{
	using namespace MantleBodyOffsetsBuilderTestPrivate;
	// The body's lowest point rises 4 cm; the face's deltas must lose the same 4 cm or its neck would sit 4 cm above the body's.
	const TArray<FVector3f> BodyNormal = { FVector3f(0, 0, 1), FVector3f(0, 0, 90) };
	TArray<TArray<FVector3f>> BodyLods;
	BodyLods.Add({ FVector3f(0, 0, 4), FVector3f(0, 0, 6) });
	TArray<FVector3f> BodyJoints = { FVector3f(0, 0, 5), FVector3f(0, 0, 1) };
	const TArray<int32> BodyParents = { INDEX_NONE, 0 };

	TArray<TArray<FVector3f>> FaceLods;
	FaceLods.Add({ FVector3f(0, 0, 6), FVector3f(0, 1, 6.5f) });
	TArray<FVector3f> FaceJoints = { FVector3f(0, 0, 0), FVector3f(0, 0, 1.5f), FVector3f(0, 0, 0) };
	const TArray<int32> FaceParents = { INDEX_NONE, 0, 1 };

	const float Shift = FMantleBodyOffsetsBuilder::StripVerticalOffset(BodyNormal, BodyLods, BodyParents, BodyJoints);
	TestEqual(TEXT("the body's shift"), Shift, 4.0f);
	TestTrue(TEXT("the body's neck vertex lost it"), Near(BodyLods[0][1], FVector3f(0, 0, 2)));
	FMantleBodyOffsetsBuilder::ShiftVertical(Shift, FaceLods, FaceParents, FaceJoints);
	TestTrue(TEXT("the face's vertices lose the same Z"), Near(FaceLods[0][0], FVector3f(0, 0, 2)));
	TestTrue(TEXT("with their other components kept"), Near(FaceLods[0][1], FVector3f(0, 1, 2.5f)));
	TestTrue(TEXT("a face root joint loses it"), Near(FaceJoints[0], FVector3f(0, 0, -4)));
	TestTrue(TEXT("its children inherit, so keep theirs"), Near(FaceJoints[1], FVector3f(0, 0, 1.5f)) && Near(FaceJoints[2], FVector3f::ZeroVector));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleBodyOffsetsShapeTargetTest, "Mantle.Editor.BodyOffsets.ShapeTargets", MantleBodyOffsetsBuilderTestPrivate::Flags)

bool FMantleBodyOffsetsShapeTargetTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("weight goes to Fat"), FMantleBodyOffsetsBuilder::ShapeConstraint(TEXT("weight+")).ToString(), FString(TEXT("Fat")));
	TestEqual(TEXT("muscle to Muscularity"), FMantleBodyOffsetsBuilder::ShapeConstraint(TEXT("muscle-")).ToString(), FString(TEXT("Muscularity")));
	TestEqual(TEXT("chest to Chest"), FMantleBodyOffsetsBuilder::ShapeConstraint(TEXT("chest+")).ToString(), FString(TEXT("Chest")));
	TestEqual(TEXT("hips to Hip"), FMantleBodyOffsetsBuilder::ShapeConstraint(TEXT("hips-")).ToString(), FString(TEXT("Hip")));
	TestEqual(TEXT("anything else to nothing"), FMantleBodyOffsetsBuilder::ShapeConstraint(TEXT("height+")).ToString(), FString(TEXT("None")));

	TestEqual(TEXT("weight+ is Fat's Max"), FMantleBodyOffsetsBuilder::ShapeTargetMeasurement(TEXT("weight+"), 0.3f, -2.8f, 2.8f), 2.8f);
	TestEqual(TEXT("weight- is its Min"), FMantleBodyOffsetsBuilder::ShapeTargetMeasurement(TEXT("weight-"), 0.3f, -2.8f, 2.8f), -2.8f);
	TestEqual(TEXT("muscle+ is Muscularity's Max"), FMantleBodyOffsetsBuilder::ShapeTargetMeasurement(TEXT("muscle+"), 0.0f, -2.3f, 2.3f), 2.3f);
	TestEqual(TEXT("chest+ is 24 cm more than now"), FMantleBodyOffsetsBuilder::ShapeTargetMeasurement(TEXT("chest+"), 90.0f, 70.0f, 140.0f), 114.0f);
	TestEqual(TEXT("chest- is 24 cm less, clamped to the range"), FMantleBodyOffsetsBuilder::ShapeTargetMeasurement(TEXT("chest-"), 90.0f, 70.0f, 140.0f), 70.0f);
	TestEqual(TEXT("hips+ is 20 cm more, clamped to the range"), FMantleBodyOffsetsBuilder::ShapeTargetMeasurement(TEXT("hips+"), 95.0f, 60.0f, 110.0f), 110.0f);
	TestEqual(TEXT("hips- is 20 cm less"), FMantleBodyOffsetsBuilder::ShapeTargetMeasurement(TEXT("hips-"), 95.0f, 60.0f, 130.0f), 75.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleBodyOffsetsComponentPositionsTest, "Mantle.Editor.BodyOffsets.ComponentPositions", MantleBodyOffsetsBuilderTestPrivate::Flags)

bool FMantleBodyOffsetsComponentPositionsTest::RunTest(const FString& Parameters)
{
	using namespace MantleBodyOffsetsBuilderTestPrivate;
	// root at Z 100, a child 10 along its Y, turned 90 degrees about Z, and a grandchild 5 along the child's Y.
	const TArray<int32> Parents = { INDEX_NONE, 0, 1 };
	TArray<FTransform> Locals;
	Locals.Add(FTransform(FQuat::Identity, FVector(0, 0, 100)));
	Locals.Add(FTransform(FQuat(FVector(0, 0, 1), HALF_PI), FVector(0, 10, 0)));
	Locals.Add(FTransform(FQuat::Identity, FVector(0, 5, 0)));
	TArray<FVector3f> Positions;
	FString Error;
	if (!TestTrue(*FString::Printf(TEXT("composes (%s)"), *Error), FMantleBodyOffsetsBuilder::ComposeComponentPositions(Parents, Locals, Positions, Error)))
	{
		return false;
	}
	TestTrue(TEXT("the root"), Near(Positions[0], FVector3f(0, 0, 100)));
	TestTrue(TEXT("the child"), Near(Positions[1], FVector3f(0, 10, 100)));
	// The grandchild's 5 along Y is turned by the child's 90 degrees about Z: it ends along -X.
	TestTrue(TEXT("the grandchild goes through its parent's turn"), Near(Positions[2], FVector3f(-5, 10, 100)));
	const TArray<int32> OutOfOrder = { INDEX_NONE, 2, 0 };
	TestFalse(TEXT("a child before its parent is refused"), FMantleBodyOffsetsBuilder::ComposeComponentPositions(OutOfOrder, Locals, Positions, Error));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
