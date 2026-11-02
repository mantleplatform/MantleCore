// Copyright Joshua Eger. All Rights Reserved.
#include "MantleGlbWriter.h"
#include "MantleMeshReduce.h"
#include "MantleVehicleBodyLODs.h"
#include "MantleVehicleConverter.h"
#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// A vehicle body's lower LODs (limits.json lods, vehicle_body) as publishing makes them: the reducer that fits a heavy body to the limit, at half and a
// quarter of its triangles, every LOD on LOD0's joints and materials, its tires reduced with it and its light kept.

namespace MantleVehicleBodyLODsTestPrivate
{
	/** A flat N x N grid (2 N^2 triangles) at Offset, drawn with Material and riding Joint, as its own primitive. */
	void AddGrid(FMantleGlbMesh& Mesh, const int32 N, const int32 Material, const uint16 Joint, const FVector3f& Offset)
	{
		FMantleGlbPrimitive& Primitive = Mesh.Primitives.AddDefaulted_GetRef();
		Primitive.MaterialIndex = Material;
		Primitive.FirstVertex = Mesh.Positions.Num();
		for (int32 Y = 0; Y <= N; ++Y)
		{
			for (int32 X = 0; X <= N; ++X)
			{
				// A gentle bump, so the reducer has a surface to keep and not a plane it may flatten to two triangles.
				const float Height = 4.0f * FMath::Sin(X * 0.4f) * FMath::Cos(Y * 0.3f);
				Mesh.Positions.Add(Offset + FVector3f(X * 5.0f, Y * 5.0f, Height));
				Mesh.Normals.Add(FVector3f::UpVector);
				Mesh.Tangents.Add(FVector4f(1.0f, 0.0f, 0.0f, 1.0f));
				Mesh.UVs.Add(FVector2f(static_cast<float>(X) / N, static_cast<float>(Y) / N));
				Mesh.InfluenceJoints.Add(Joint);
				Mesh.InfluenceWeights.Add(1.0f);
			}
		}
		for (int32 Y = 0; Y < N; ++Y)
		{
			for (int32 X = 0; X < N; ++X)
			{
				const uint32 A = Y * (N + 1) + X, B = A + 1, C = A + N + 1, D = C + 1;
				Primitive.Indices.Append({ A, C, B, B, C, D });
			}
		}
		Primitive.NumVertices = Mesh.Positions.Num() - Primitive.FirstVertex;
	}

	/** A car body: a 40 x 40 panel (3,200 triangles) on the root, a 20 x 20 tire (800) on a wheel bone, a two-triangle head light on the root. */
	FMantleGlbMesh MakeBody()
	{
		FMantleGlbMesh Body;
		Body.Name = TEXT("TestCar");
		Body.MaterialNames = { TEXT("M_Body"), TEXT("M_Tire"), TEXT("M_Head") };
		FMantleGlbJoint Root;
		Root.Name = FName(TEXT("root"));
		Root.Parent = INDEX_NONE;
		FMantleGlbJoint Wheel;
		Wheel.Name = FName(TEXT("wheel_fl"));
		Wheel.Parent = 0;
		Wheel.Local = FTransform(FVector(130.0, -80.0, 35.0));
		Body.Joints = { Root, Wheel };
		Body.NumInfluences = 1;
		AddGrid(Body, 40, 0, 0, FVector3f::ZeroVector);
		AddGrid(Body, 20, 1, 1, FVector3f(300.0f, 0.0f, 0.0f));
		AddGrid(Body, 1, 2, 0, FVector3f(0.0f, 0.0f, 50.0f));
		return Body;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleBodyLODsTest,
	"Mantle.Vehicle.LODs.BodyGetsLowerLODs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleBodyLODsTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleBodyLODsTestPrivate;
	if (!FMantleMeshReduce::IsAvailable())
	{
		AddInfo(TEXT("This editor has no static mesh reducer, so the LODs are not made."));
		return true;
	}
	const FMantleGlbMesh Body = MakeBody();
	const int32 Triangles = FMantleVehicleBodyLODs::CountTriangles(Body);
	TestEqual(TEXT("the body starts with 4,002 triangles"), Triangles, 4002);

	TArray<FString> Notes;
	FString Warning;
	const TArray<float> Fractions = { 0.5f, 0.25f };
	const TArray<FString> Keep = { TEXT("M_Head") };
	const TArray<FMantleGlbMesh> Lower = FMantleVehicleBodyLODs::Make(Body, Fractions, Keep, Notes, Warning);
	if (!TestEqual(*FString::Printf(TEXT("LOD1 and LOD2 are made (%s)"), *Warning), Lower.Num(), 2))
	{
		return false;
	}
	TestTrue(TEXT("no warning when every LOD was made"), Warning.IsEmpty());
	TestTrue(TEXT("a note gives the triangle counts"), Notes.ContainsByPredicate([](const FString& Note) { return Note.Contains(TEXT("4002 / ")); }));

	int32 Previous = Triangles;
	for (int32 Index = 0; Index < Lower.Num(); ++Index)
	{
		const FMantleGlbMesh& LOD = Lower[Index];
		const int32 Count = FMantleVehicleBodyLODs::CountTriangles(LOD);
		const int32 Target = FMath::FloorToInt(Triangles * Fractions[Index]);
		TestTrue(*FString::Printf(TEXT("LOD%d has at most the previous LOD's triangles (%d of %d)"), Index + 1, Count, Previous), Count > 0 && Count <= Previous);
		TestTrue(*FString::Printf(TEXT("LOD%d is about its share of LOD0 (%d, aiming at %d)"), Index + 1, Count, Target), Count <= Target + Target / 4);
		Previous = Count;

		bool bSameJoints = LOD.Joints.Num() == Body.Joints.Num();
		for (int32 Joint = 0; bSameJoints && Joint < Body.Joints.Num(); ++Joint)
		{
			bSameJoints = LOD.Joints[Joint].Name == Body.Joints[Joint].Name && LOD.Joints[Joint].Parent == Body.Joints[Joint].Parent;
		}
		TestTrue(*FString::Printf(TEXT("LOD%d keeps LOD0's joints"), Index + 1), bSameJoints);
		TestTrue(*FString::Printf(TEXT("LOD%d draws only LOD0's materials"), Index + 1), !LOD.MaterialNames.ContainsByPredicate([&Body](const FString& Name)
		{
			return !Body.MaterialNames.Contains(Name);
		}));
		TestTrue(*FString::Printf(TEXT("LOD%d still draws the head light"), Index + 1), LOD.MaterialNames.Contains(TEXT("M_Head")));
		TestEqual(*FString::Printf(TEXT("LOD%d carries tangents, as LOD0 does"), Index + 1), LOD.Tangents.Num(), LOD.Positions.Num());

		// The tire is reduced too, and stays on its wheel.
		int32 TireTriangles = 0;
		bool bTireOnWheel = true;
		const int32 Tire = LOD.MaterialNames.IndexOfByKey(TEXT("M_Tire"));
		for (const FMantleGlbPrimitive& Primitive : LOD.Primitives)
		{
			if (Primitive.MaterialIndex == Tire)
			{
				TireTriangles += Primitive.Indices.Num() / 3;
				for (const uint32 Local : Primitive.Indices)
				{
					bTireOnWheel = bTireOnWheel && LOD.InfluenceJoints[Primitive.FirstVertex + Local] == 1;
				}
			}
		}
		TestTrue(*FString::Printf(TEXT("LOD%d reduces the tire (%d of 800 triangles)"), Index + 1, TireTriangles), TireTriangles > 0 && TireTriangles < 800);
		TestTrue(*FString::Printf(TEXT("LOD%d's tire rides its wheel"), Index + 1), bTireOnWheel);
	}

	// The upload carries all three, as LOD nodes, and a body with fractions asking for nothing uploads as it did.
	TArray<uint8> Bytes;
	FString Error;
	TestTrue(*FString::Printf(TEXT("the upload writes with its LODs (%s)"), *Error),
		FMantleVehicleConverter::WriteUpload(Body, MakeShared<FJsonObject>(), Bytes, Error, Lower));
	// The JSON chunk: its length at byte 12, its text from byte 20.
	const uint32 JsonBytes = Bytes.Num() >= 20 ? *reinterpret_cast<const uint32*>(Bytes.GetData() + 12) : 0;
	const FString Json = JsonBytes > 0 && Bytes.Num() >= 20 + static_cast<int32>(JsonBytes)
		? FString(static_cast<int32>(JsonBytes), reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + 20)) : FString();
	TestTrue(TEXT("its nodes are LOD0, LOD1 and LOD2"), Json.Contains(TEXT("\"LOD0\"")) && Json.Contains(TEXT("\"LOD1\"")) && Json.Contains(TEXT("\"LOD2\"")));
	TestEqual(TEXT("no fractions, no lower LODs"), FMantleVehicleBodyLODs::Make(Body, TArray<float>(), Keep, Notes, Warning).Num(), 0);
	TestTrue(TEXT("the skin is written as bytes"), Json.Contains(TEXT("\"normalized\":true")) && Json.Contains(TEXT("\"componentType\":5121")));

	// Slim: a body whose every triangle has its own three corners (as many imports do) is written with its shared corners once, the same triangles.
	FMantleGlbMesh Split = MakeBody();
	{
		FMantleGlbMesh Unwelded = Split;
		Unwelded.Positions.Reset(); Unwelded.Normals.Reset(); Unwelded.Tangents.Reset(); Unwelded.UVs.Reset();
		Unwelded.InfluenceJoints.Reset(); Unwelded.InfluenceWeights.Reset();
		for (FMantleGlbPrimitive& Primitive : Unwelded.Primitives)
		{
			const int32 First = Unwelded.Positions.Num();
			TArray<uint32> Indices;
			for (const uint32 Local : Primitive.Indices)
			{
				const int32 Old = Primitive.FirstVertex + static_cast<int32>(Local);
				Indices.Add(static_cast<uint32>(Unwelded.Positions.Num() - First));
				Unwelded.Positions.Add(Split.Positions[Old]);
				Unwelded.Normals.Add(Split.Normals[Old]);
				Unwelded.Tangents.Add(Split.Tangents[Old]);
				Unwelded.UVs.Add(Split.UVs[Old]);
				Unwelded.InfluenceJoints.Add(Split.InfluenceJoints[Old]);
				Unwelded.InfluenceWeights.Add(Split.InfluenceWeights[Old]);
			}
			Primitive.FirstVertex = First;
			Primitive.NumVertices = Unwelded.Positions.Num() - First;
			Primitive.Indices = MoveTemp(Indices);
		}
		Split = MoveTemp(Unwelded);
	}
	TestEqual(TEXT("the split body has three corners a triangle"), Split.Positions.Num(), Triangles * 3);
	FMantleVehicleBodyLODs::Slim(Split);
	TestEqual(TEXT("slimmed, it has the original's vertices"), Split.Positions.Num(), Body.Positions.Num());
	TestEqual(TEXT("and the same triangles"), FMantleVehicleBodyLODs::CountTriangles(Split), Triangles);
	TestTrue(TEXT("and compact streams"), Split.bCompactStreams);
	return true;
}

#endif
