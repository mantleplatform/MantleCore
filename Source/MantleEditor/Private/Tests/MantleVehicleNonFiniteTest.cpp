// Copyright Joshua Eger. All Rights Reserved.
#include "MantleGlbWriter.h"
#include "MantleVehicleChassisSkin.h"
#include "MantleVehicleConverter.h"
#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

// A model whose UVs Unreal reads back as infinite (#448 / Core #292: a FiveM F150's broken UVs, 2 million to 2 billion, past the 16-bit floats a skeletal mesh keeps its
// UVs in) builds, and its export is not refused: the cook refuses any normal or texture coordinate that is not a finite number. Degenerate and zero-UV-area triangles
// ride along, as the F150 had them.

namespace MantleVehicleNonFiniteTestPrivate
{
	const float Inf = std::numeric_limits<float>::infinity();
	const float NaN = std::numeric_limits<float>::quiet_NaN();

	bool Finite2(const FVector2f& V) { return FMath::IsFinite(V.X) && FMath::IsFinite(V.Y); }
	bool Finite3(const FVector3f& V) { return FMath::IsFinite(V.X) && FMath::IsFinite(V.Y) && FMath::IsFinite(V.Z); }

	/**
	 * Two sections. 'plastic_black': a quad (0-3), a degenerate triangle (4, 5, 6 at one point) and a zero-UV-area triangle (0, 1, 7 with one UV); vertex 1's UV is
	 * infinite, vertex 2's normal is not a number, vertex 5's normal too (it has only the degenerate triangle). 'headlight_lens': one triangle (8-10), vertex 9's U
	 * is minus infinity and vertex 10's UV is infinite too.
	 */
	FMantleVehicleModel MakeModel()
	{
		FMantleVehicleModel Model;
		Model.Name = TEXT("TestTruck");
		Model.Materials.Add(FSkeletalMaterial(static_cast<UMaterialInterface*>(nullptr), FName("plastic_black")));
		Model.Materials.Add(FSkeletalMaterial(static_cast<UMaterialInterface*>(nullptr), FName("headlight_lens")));
		FMantleSkinnedMeshLOD& LOD = Model.LOD;
		LOD.Positions = { FVector3f(0, 0, 0), FVector3f(100, 0, 0), FVector3f(100, 100, 0), FVector3f(0, 100, 0),
			FVector3f(50, 50, 10), FVector3f(50, 50, 10), FVector3f(50, 50, 10), FVector3f(50, -50, 0),
			FVector3f(0, 0, 50), FVector3f(10, 0, 50), FVector3f(0, 10, 50) };
		for (int32 Vertex = 0; Vertex < LOD.Positions.Num(); ++Vertex)
		{
			LOD.Normals.Add(FVector3f::UpVector);
			LOD.Tangents.Add(FVector4f(1, 0, 0, 1));
			LOD.UVs.Add(FVector2f(LOD.Positions[Vertex].X / 100.0f, LOD.Positions[Vertex].Y / 100.0f));
		}
		LOD.UVs[7] = LOD.UVs[0];
		LOD.UVs[1] = FVector2f(Inf, 0.5f);
		LOD.Normals[2] = FVector3f(NaN, 0, 0);
		LOD.Normals[5] = FVector3f(NaN, NaN, NaN);
		LOD.Tangents[3] = FVector4f(NaN, 0, 0, 1);
		LOD.UVs[9] = FVector2f(-Inf, 0.8f);
		LOD.UVs[10] = FVector2f(Inf, Inf);
		LOD.Indices = { 0, 1, 2, 0, 2, 3, 4, 5, 6, 0, 1, 7, 8, 9, 10 };
		FMantleSkinnedMeshSection Plastic;
		Plastic.MaterialIndex = 0;
		Plastic.FirstIndex = 0;
		Plastic.IndexCount = 12;
		FMantleSkinnedMeshSection Lens;
		Lens.MaterialIndex = 1;
		Lens.FirstIndex = 12;
		Lens.IndexCount = 3;
		LOD.Sections = { Plastic, Lens };
		return Model;
	}

	/** One primitive on the root with the same troubles as MakeModel's first section: the body an export writes. */
	FMantleGlbMesh MakeBody()
	{
		FMantleGlbMesh Body;
		Body.Name = TEXT("TestTruck");
		Body.MaterialNames = { TEXT("plastic_black") };
		FMantleGlbJoint Root;
		Root.Name = FName(TEXT("root"));
		Root.Parent = INDEX_NONE;
		Body.Joints = { Root };
		Body.NumInfluences = 1;
		const FMantleVehicleModel Model = MakeModel();
		FMantleGlbPrimitive& Primitive = Body.Primitives.AddDefaulted_GetRef();
		Primitive.NumVertices = 8;
		Primitive.Indices = { 0, 1, 2, 0, 2, 3, 4, 5, 6, 0, 1, 7 };
		for (int32 Vertex = 0; Vertex < 8; ++Vertex)
		{
			Body.Positions.Add(Model.LOD.Positions[Vertex]);
			Body.Normals.Add(Model.LOD.Normals[Vertex]);
			Body.Tangents.Add(Model.LOD.Tangents[Vertex]);
			Body.UVs.Add(Model.LOD.UVs[Vertex]);
			Body.InfluenceJoints.Add(0);
			Body.InfluenceWeights.Add(1.0f);
		}
		return Body;
	}

	/** Every float of every primitive's Attribute accessor in a written GLB, or false when it cannot be read. */
	bool ReadAttribute(const TArray<uint8>& Bytes, const TCHAR* Attribute, TArray<float>& Out)
	{
		if (Bytes.Num() < 20)
		{
			return false;
		}
		const uint32 JsonBytes = *reinterpret_cast<const uint32*>(Bytes.GetData() + 12);
		const int32 BinHeader = 20 + static_cast<int32>(JsonBytes);
		if (Bytes.Num() < BinHeader + 8)
		{
			return false;
		}
		TSharedPtr<FJsonObject> Gltf;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FString(static_cast<int32>(JsonBytes), reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + 20))), Gltf) || !Gltf)
		{
			return false;
		}
		const uint8* Bin = Bytes.GetData() + BinHeader + 8;
		const uint32 BinBytes = *reinterpret_cast<const uint32*>(Bytes.GetData() + BinHeader);
		const TArray<TSharedPtr<FJsonValue>>& Accessors = Gltf->GetArrayField(TEXT("accessors"));
		const TArray<TSharedPtr<FJsonValue>>& Views = Gltf->GetArrayField(TEXT("bufferViews"));
		for (const TSharedPtr<FJsonValue>& Mesh : Gltf->GetArrayField(TEXT("meshes")))
		{
			for (const TSharedPtr<FJsonValue>& Primitive : Mesh->AsObject()->GetArrayField(TEXT("primitives")))
			{
				int32 Index = INDEX_NONE;
				if (!Primitive->AsObject()->GetObjectField(TEXT("attributes"))->TryGetNumberField(Attribute, Index) || !Accessors.IsValidIndex(Index))
				{
					continue;
				}
				const TSharedPtr<FJsonObject> Accessor = Accessors[Index]->AsObject();
				const TSharedPtr<FJsonObject> View = Views[Accessor->GetIntegerField(TEXT("bufferView"))]->AsObject();
				const FString Type = Accessor->GetStringField(TEXT("type"));
				const int32 Width = Type == TEXT("VEC2") ? 2 : Type == TEXT("VEC3") ? 3 : 4;
				int32 ViewOffset = 0, AccessorOffset = 0;
				View->TryGetNumberField(TEXT("byteOffset"), ViewOffset);
				Accessor->TryGetNumberField(TEXT("byteOffset"), AccessorOffset);
				const int32 Count = Accessor->GetIntegerField(TEXT("count")) * Width;
				const int64 Start = static_cast<int64>(ViewOffset) + AccessorOffset;
				if (Accessor->GetIntegerField(TEXT("componentType")) != 5126 || Start + Count * 4 > BinBytes)
				{
					return false;
				}
				for (int32 Value = 0; Value < Count; ++Value)
				{
					Out.Add(*reinterpret_cast<const float*>(Bin + Start + Value * 4));
				}
			}
		}
		return Out.Num() > 0;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleNonFiniteModelTest,
	"Mantle.Vehicle.Builder.NonFiniteModelBuildsAndExports",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleNonFiniteModelTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleNonFiniteTestPrivate;

	// Reading a model makes its values finite, and says where.
	FMantleVehicleModel Model = MakeModel();
	TArray<FString> Notes;
	const int32 Changed = FMantleVehicleModel::CleanNonFinite(Model, Notes);
	const FMantleSkinnedMeshLOD& LOD = Model.LOD;
	TestEqual(TEXT("six vertices are changed (UVs 1, 9, 10; normals 2, 5; tangent 3)"), Changed, 6);
	bool bFinite = true;
	for (int32 Vertex = 0; Vertex < LOD.Positions.Num(); ++Vertex)
	{
		bFinite = bFinite && Finite2(LOD.UVs[Vertex]) && Finite3(LOD.Normals[Vertex])
			&& FMath::IsFinite(LOD.Tangents[Vertex].X) && FMath::IsFinite(LOD.Tangents[Vertex].Y) && FMath::IsFinite(LOD.Tangents[Vertex].Z);
	}
	TestTrue(TEXT("every UV, normal and tangent is finite"), bFinite);
	TestTrue(*FString::Printf(TEXT("vertex 1's UV is a corner beside it's (%s)"), *LOD.UVs[1].ToString()), LOD.UVs[1] == LOD.UVs[0] || LOD.UVs[1] == LOD.UVs[2] || LOD.UVs[1] == LOD.UVs[7]);
	TestTrue(TEXT("the lens's two bad UVs take its good one"), LOD.UVs[9] == LOD.UVs[8] && LOD.UVs[10] == LOD.UVs[8]);
	TestTrue(*FString::Printf(TEXT("vertex 2's normal is its triangles' (%s)"), *LOD.Normals[2].ToString()), LOD.Normals[2].Equals(FVector3f::UpVector, 1.0e-4f));
	TestTrue(TEXT("vertex 5 (only a degenerate triangle) points up"), LOD.Normals[5] == FVector3f::UpVector);
	TestTrue(TEXT("the good vertices are untouched"), LOD.UVs[0] == FVector2f(0, 0) && LOD.UVs[3] == FVector2f(0, 1) && LOD.Normals[0] == FVector3f::UpVector);
	TestTrue(*FString::Printf(TEXT("a note names the plastic and its 1 vertex (%s)"), *FString::Join(Notes, TEXT(" | "))),
		Notes.ContainsByPredicate([](const FString& Note) { return Note.StartsWith(TEXT("1 vertices of TestTruck's 'plastic_black' had texture coordinates")); }));
	TestTrue(TEXT("and one names the lens and its 2"), Notes.ContainsByPredicate([](const FString& Note) { return Note.StartsWith(TEXT("2 vertices of TestTruck's 'headlight_lens'")); }));
	TestTrue(TEXT("and one the plastic's 2 normals"), Notes.ContainsByPredicate([](const FString& Note) { return Note.Contains(TEXT("'plastic_black' had a normal")); }));
	TArray<FString> Again;
	TestEqual(TEXT("a clean model is left alone"), FMantleVehicleModel::CleanNonFinite(Model, Again), 0);
	TestEqual(TEXT("with no notes"), Again.Num(), 0);

	// The export: a body that still carries them (built before the read cleaned them) is written finite, with its degenerate and zero-UV-area triangles.
	const FMantleGlbMesh Body = MakeBody();
	TArray<uint8> Bytes;
	FString Error;
	if (!TestTrue(*FString::Printf(TEXT("the body writes (%s)"), *Error), FMantleVehicleConverter::WriteUpload(Body, MakeShared<FJsonObject>(), Bytes, Error)))
	{
		return false;
	}
	for (const TCHAR* Attribute : { TEXT("NORMAL"), TEXT("TEXCOORD_0"), TEXT("TANGENT") })
	{
		TArray<float> Values;
		TestTrue(*FString::Printf(TEXT("its %s reads back"), Attribute), ReadAttribute(Bytes, Attribute, Values));
		TestTrue(*FString::Printf(TEXT("and every %s value is finite, as the cook requires"), Attribute), !Values.ContainsByPredicate([](const float Value) { return !FMath::IsFinite(Value); }));
	}
	// A position that is not a number cannot be made anything sensible: the export says so at once instead of the cook later.
	FMantleGlbMesh BadPosition = MakeBody();
	BadPosition.Positions[1] = FVector3f(NaN, 0, 0);
	Bytes.Reset();
	Error.Reset();
	TestFalse(TEXT("a position that is not a number is refused at export"), FMantleVehicleConverter::WriteUpload(BadPosition, MakeShared<FJsonObject>(), Bytes, Error));
	TestTrue(*FString::Printf(TEXT("saying what to fix (%s)"), *Error), Error.Contains(TEXT("not numbers")));
	return true;
}

#endif
