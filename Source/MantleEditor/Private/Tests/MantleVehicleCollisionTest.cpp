// Copyright Joshua Eger. All Rights Reserved.
#include "MantleGlbWriter.h"
#include "MantleVehicleCollision.h"
#include "MantleVehicleConverter.h"
#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

// A split car's attached parts' collision triangles (FMantleVehicleCollision): placed on their bone's rest pose, welded, written into the vehicle's upload where the cook
// finds them (backend TestVehicleCollisionCooksIntoVersion2AndRoundTrips).

namespace MantleVehicleCollisionTestPrivate
{
	/** One triangle on the root: the smallest skinned body the writer takes. */
	FMantleGlbMesh MakeTinyBody()
	{
		FMantleGlbMesh Body;
		Body.Name = TEXT("TinyCar");
		Body.MaterialNames = { TEXT("M_Body") };
		FMantleGlbJoint Root;
		Root.Name = FName(TEXT("root"));
		Root.Parent = INDEX_NONE;
		Body.Joints = { Root };
		Body.NumInfluences = 1;
		FMantleGlbPrimitive& Primitive = Body.Primitives.AddDefaulted_GetRef();
		Primitive.NumVertices = 3;
		Primitive.Indices = { 0, 1, 2 };
		Body.Positions = { FVector3f(0, 0, 0), FVector3f(10, 0, 0), FVector3f(0, 10, 0) };
		for (int32 Vertex = 0; Vertex < 3; ++Vertex)
		{
			Body.Normals.Add(FVector3f::UpVector);
			Body.UVs.Add(FVector2f::ZeroVector);
			Body.InfluenceJoints.Add(0);
			Body.InfluenceWeights.Add(1.0f);
		}
		return Body;
	}

	TSharedPtr<FJsonObject> GltfJsonOf(const TArray<uint8>& Bytes)
	{
		const uint32 JsonBytes = Bytes.Num() >= 20 ? *reinterpret_cast<const uint32*>(Bytes.GetData() + 12) : 0;
		TSharedPtr<FJsonObject> Gltf;
		if (JsonBytes > 0 && Bytes.Num() >= 20 + static_cast<int32>(JsonBytes))
		{
			FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FString(static_cast<int32>(JsonBytes), reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + 20))), Gltf);
		}
		return Gltf;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleCollisionPlacesAndWeldsTest,
	"Mantle.Vehicle.Collision.PartsArePlacedAndWelded",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleCollisionPlacesAndWeldsTest::RunTest(const FString&)
{
	// A bone's rest pose: the root's, and a child's on it (a door hung 100 cm out and 50 up from a root that sits 10 cm up), the name in any case.
	FMantleGlbJoint Root;
	Root.Name = FName(TEXT("root"));
	Root.Parent = INDEX_NONE;
	Root.Local = FTransform(FVector(0, 0, 10));
	FMantleGlbJoint Door;
	Door.Name = FName(TEXT("Door_R"));
	Door.Parent = 0;
	Door.Local = FTransform(FVector(100, 0, 50));
	const TArray<FMantleGlbJoint> Joints = { Root, Door };
	FTransform DoorPose;
	if (!TestTrue(TEXT("a bone is found by its name in any case"), FMantleVehicleCollision::BoneRestPose(Joints, FName(TEXT("door_r")), DoorPose)))
	{
		return false;
	}
	TestTrue(TEXT("at its parent's pose then its own"), DoorPose.GetLocation().Equals(FVector(100, 0, 60), 0.001));
	FTransform Unused;
	TestFalse(TEXT("a bone the body lacks is not found"), FMantleVehicleCollision::BoneRestPose(Joints, FName(TEXT("hood")), Unused));

	// A quad (two triangles) as a part's mesh, its own space, with a triangle of no area, one with an index past its vertices and a vertex nothing uses.
	const TArray<FVector3f> Quad = { FVector3f(0, 0, 0), FVector3f(10, 0, 0), FVector3f(10, 10, 0), FVector3f(0, 10, 0), FVector3f(99, 99, 99) };
	const TArray<uint32> Triangles = { 0, 1, 2, 0, 2, 3, 0, 1, 1, 0, 1, 9 };
	FMantleVehicleCollision Shape;
	TestEqual(TEXT("the quad's two triangles are added, not the empty one or the one past the vertices"), Shape.AddTriangles(Quad, Triangles, DoorPose), 2);
	TestEqual(TEXT("on four vertices, the loose one left out"), Shape.Positions.Num(), 4);
	TestTrue(TEXT("placed on the door's pose"), Shape.Positions.ContainsByPredicate([](const FVector3f& P) { return P.Equals(FVector3f(110, 10, 60), 0.001f); }));
	TestFalse(TEXT("and the vertex nothing used is not in the shape"), Shape.Positions.ContainsByPredicate([](const FVector3f& P) { return P.X > 150.0f; }));

	// A second part sharing an edge with the first is welded to it: the same corners are the same vertices.
	const TArray<FVector3f> Next = { FVector3f(10, 0, 0), FVector3f(20, 0, 0), FVector3f(10, 10, 0) };
	TestEqual(TEXT("a neighbor's triangle is added"), Shape.AddTriangles(Next, { 0, 1, 2 }, DoorPose), 1);
	TestEqual(TEXT("with only its one new corner, the edge it shares welded"), Shape.Positions.Num(), 5);
	TestEqual(TEXT("three triangles in all"), Shape.NumTriangles(), 3);
	for (const uint32 Index : Shape.Indices)
	{
		if (!TestTrue(TEXT("every index is a vertex"), static_cast<int32>(Index) < Shape.Positions.Num()))
		{
			break;
		}
	}

	// A part mirrored by a negative scale places as well.
	FMantleVehicleCollision Mirrored;
	TestEqual(TEXT("a mirrored part is placed"), Mirrored.AddTriangles(Quad, { 0, 1, 2 }, FTransform(FQuat::Identity, FVector(0, 100, 0), FVector(1, -1, 1))), 1);
	TestTrue(TEXT("on the other side"), Mirrored.Positions.ContainsByPredicate([](const FVector3f& P) { return P.Equals(FVector3f(10, 90, 0), 0.001f); }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleCollisionIsInTheUploadTest,
	"Mantle.Vehicle.Collision.UploadCarriesTheTriangles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleCollisionIsInTheUploadTest::RunTest(const FString&)
{
	using namespace MantleVehicleCollisionTestPrivate;
	const FMantleGlbMesh Body = MakeTinyBody();
	TArray<uint8> Plain;
	FString Error;
	if (!TestTrue(*FString::Printf(TEXT("a car without parts writes (%s)"), *Error), FMantleVehicleConverter::WriteUpload(Body, MakeShared<FJsonObject>(), Plain, Error)))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> PlainGltf = GltfJsonOf(Plain);
	TestTrue(TEXT("and has no collision key"), PlainGltf.IsValid() && !PlainGltf->GetObjectField(TEXT("extras"))->HasField(TEXT("mantleVehicleCollision")));

	// A door: a quad 200 cm out on the x axis (Unreal), 100 cm up, 100 wide.
	FMantleVehicleCollision Shape;
	Shape.AddTriangles({ FVector3f(0, 0, 0), FVector3f(100, 0, 0), FVector3f(100, 0, 100), FVector3f(0, 0, 100) }, { 0, 1, 2, 0, 2, 3 }, FTransform(FVector(200, 0, 100)));
	TArray<uint8> Bytes;
	if (!TestTrue(*FString::Printf(TEXT("a car with a door writes (%s)"), *Error), FMantleVehicleConverter::WriteUpload(Body, MakeShared<FJsonObject>(), Bytes, Error, {}, &Shape)))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Gltf = GltfJsonOf(Bytes);
	if (!TestTrue(TEXT("its JSON reads"), Gltf.IsValid()) || !TestTrue(TEXT("with extras"), Gltf->HasField(TEXT("extras"))))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Extras = Gltf->GetObjectField(TEXT("extras"));
	TestTrue(TEXT("the document key is still there beside it"), Extras->HasField(TEXT("mantleVehicle")));
	if (!TestTrue(TEXT("the collision key names the two accessors"), Extras->HasField(TEXT("mantleVehicleCollision"))))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Named = Extras->GetObjectField(TEXT("mantleVehicleCollision"));
	const TArray<TSharedPtr<FJsonValue>>& Accessors = Gltf->GetArrayField(TEXT("accessors"));
	const TSharedPtr<FJsonObject> Positions = Accessors[static_cast<int32>(Named->GetNumberField(TEXT("positions")))]->AsObject();
	const TSharedPtr<FJsonObject> Indices = Accessors[static_cast<int32>(Named->GetNumberField(TEXT("indices")))]->AsObject();
	TestEqual(TEXT("four welded vertices"), static_cast<int32>(Positions->GetNumberField(TEXT("count"))), 4);
	TestEqual(TEXT("as float VEC3"), Positions->GetStringField(TEXT("type")), FString(TEXT("VEC3")));
	TestEqual(TEXT("of six indices"), static_cast<int32>(Indices->GetNumberField(TEXT("count"))), 6);
	TestEqual(TEXT("unsigned ints (the streams are not compact)"), static_cast<int32>(Indices->GetNumberField(TEXT("componentType"))), 5125);

	// Compact streams (a slimmed upload) write the indices as unsigned shorts when the vertices fit.
	FMantleGlbMesh Compact = Body;
	Compact.bCompactStreams = true;
	TArray<uint8> CompactBytes;
	if (TestTrue(TEXT("a compact upload writes"), FMantleVehicleConverter::WriteUpload(Compact, MakeShared<FJsonObject>(), CompactBytes, Error, {}, &Shape)))
	{
		const TSharedPtr<FJsonObject> CompactGltf = GltfJsonOf(CompactBytes);
		const TSharedPtr<FJsonObject> CompactNamed = CompactGltf->GetObjectField(TEXT("extras"))->GetObjectField(TEXT("mantleVehicleCollision"));
		const TSharedPtr<FJsonObject> CompactIndices = CompactGltf->GetArrayField(TEXT("accessors"))[static_cast<int32>(CompactNamed->GetNumberField(TEXT("indices")))]->AsObject();
		TestEqual(TEXT("unsigned shorts"), static_cast<int32>(CompactIndices->GetNumberField(TEXT("componentType"))), 5123);
	}
	return true;
}

#endif
