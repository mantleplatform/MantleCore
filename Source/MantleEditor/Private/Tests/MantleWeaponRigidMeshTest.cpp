// Copyright Joshua Eger. All Rights Reserved.
#include "MantleWeaponRigidMesh.h"

#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "BoneWeights.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Engine/SkinnedAssetCommon.h"
#include "Engine/StaticMesh.h"
#include "HAL/FileManager.h"
#include "MeshDescription.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "SkeletalMeshAttributes.h"
#include "SkeletalMeshReductionSettings.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"

using UE::AnimationCore::FBoneWeight;

#if WITH_DEV_AUTOMATION_TESTS

// The rigid weapon mesh generator: a root bone (and a magazine bone), every vertex weighted whole to one,
// the sockets the runtime reads.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleWeaponRigidMeshTestPrivate
{
	const TCHAR* const CubePath = TEXT("/Engine/BasicShapes/Cube.Cube");
	const TCHAR* const TestFolder = TEXT("/Game/Tests/WeaponRigid");

	/** Removes the assets a test made, from memory and from disk, however the test ends. */
	struct FRigidCleanup
	{
		FString Name;
		FMantleWeaponRigidMeshResult* Result = nullptr;

		FRigidCleanup(const FString& InName, FMantleWeaponRigidMeshResult* InResult)
			: Name(InName)
			, Result(InResult)
		{
		}

		~FRigidCleanup()
		{
			if (Result)
			{
				Remove(Result->Mesh);
				Remove(Result->Skeleton);
			}
			for (const FString& Prefix : { FString(TEXT("SKM_")), FString(TEXT("SK_")) })
			{
				const FString File = FPackageName::LongPackageNameToFilename(FString(TestFolder) / (Prefix + Name), FPackageName::GetAssetPackageExtension());
				IFileManager::Get().Delete(*File, false, true, true);
			}
		}

		static void Remove(UObject* Asset)
		{
			if (!Asset)
			{
				return;
			}
			FAssetRegistryModule::AssetDeleted(Asset);
			Asset->ClearFlags(RF_Public | RF_Standalone);
			Asset->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
			Asset->MarkAsGarbage();
		}
	};

	/** The vertex count the generator should reproduce: the source description's, else the render data's. */
	int32 SourceVertexCount(UStaticMesh& Mesh)
	{
		if (Mesh.IsMeshDescriptionValid(0))
		{
			if (const FMeshDescription* Description = Mesh.GetMeshDescription(0))
			{
				return Description->Vertices().Num();
			}
		}
		return Mesh.GetRenderData() && Mesh.GetRenderData()->LODResources.Num() > 0 ? static_cast<int32>(Mesh.GetRenderData()->LODResources[0].VertexBuffers.PositionVertexBuffer.GetNumVertices()) : 0;
	}

	/** The triangle count the generator should keep: the source description's, else the render data's. */
	int32 SourceTriangleCount(UStaticMesh& Mesh)
	{
		if (Mesh.IsMeshDescriptionValid(0))
		{
			if (const FMeshDescription* Description = Mesh.GetMeshDescription(0))
			{
				return Description->Triangles().Num();
			}
		}
		return Mesh.GetRenderData() && Mesh.GetRenderData()->LODResources.Num() > 0 ? static_cast<int32>(Mesh.GetRenderData()->LODResources[0].GetNumTriangles()) : 0;
	}

	/** The triangles in the skeletal mesh's LOD0 render data, which is what a weapon exports and publishes. */
	int32 RenderTriangleCount(USkeletalMesh& Mesh)
	{
		const FSkeletalMeshRenderData* RenderData = Mesh.GetResourceForRendering();
		if (!RenderData || RenderData->LODRenderData.Num() == 0)
		{
			return 0;
		}
		int32 Triangles = 0;
		for (const FSkelMeshRenderSection& Section : RenderData->LODRenderData[0].RenderSections)
		{
			Triangles += static_cast<int32>(Section.NumTriangles);
		}
		return Triangles;
	}

	bool NearlyEqual(const FTransform& A, const FTransform& B, const double Tolerance)
	{
		return A.GetTranslation().Equals(B.GetTranslation(), Tolerance) && FMath::Abs(A.GetRotation() | B.GetRotation()) > 1.0 - 1.0e-6;
	}

	/** The bone index each vertex of the mesh's LOD0 is weighted to, counted per bone. */
	TMap<int32, int32> CountWeightedVertices(USkeletalMesh& Mesh, bool& bOutOnlyWhole)
	{
		TMap<int32, int32> Counts;
		bOutOnlyWhole = true;
		FMeshDescription Description;
		if (!Mesh.CloneMeshDescription(0, Description))
		{
			bOutOnlyWhole = false;
			return Counts;
		}
		const FSkeletalMeshConstAttributes Attributes(Description);
		const FSkinWeightsVertexAttributesConstRef Weights = Attributes.GetVertexSkinWeights();
		for (const FVertexID Vertex : Description.Vertices().GetElementIDs())
		{
			int32 Influences = 0;
			int32 Bone = INDEX_NONE;
			for (const FBoneWeight Weight : Weights.Get(Vertex))
			{
				if (Weight.GetRawWeight() > 0)
				{
					++Influences;
					Bone = Weight.GetBoneIndex();
				}
			}
			bOutOnlyWhole = bOutOnlyWhole && Influences == 1;
			Counts.FindOrAdd(Bone) += 1;
		}
		return Counts;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWeaponRigidMeshBodyOnlyTest,
	"Mantle.Editor.WeaponRigidMesh.BodyOnly",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWeaponRigidMeshBodyOnlyTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponRigidMeshTestPrivate;
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, CubePath);
	if (!TestNotNull(TEXT("the engine cube loads"), Cube))
	{
		return false;
	}

	FMantleWeaponRigidMeshInput Input;
	Input.BodyMesh = Cube;
	Input.PackagePath = TestFolder;
	Input.Name = TEXT("BodyOnly");
	const FTransform Grip(FRotator(0.0, 30.0, 0.0), FVector(10.0, 5.0, -20.0));
	const FTransform Muzzle(FRotator(0.0, 0.0, 0.0), FVector(60.0, 0.0, 10.0));
	Input.Sockets.Add(TEXT("Grip_R"), Grip);
	Input.Sockets.Add(TEXT("Muzzle"), Muzzle);

	FMantleWeaponRigidMeshResult Result = FMantleWeaponRigidMesh::Build(Input);
	FRigidCleanup Cleanup(Input.Name, &Result);
	for (const FString& Error : Result.Errors)
	{
		AddError(Error);
	}
	if (!TestNotNull(TEXT("a mesh is built"), Result.Mesh) || !TestNotNull(TEXT("a skeleton is built"), Result.Skeleton))
	{
		return false;
	}

	const FReferenceSkeleton& Reference = Result.Mesh->GetRefSkeleton();
	TestEqual(TEXT("one bone"), Reference.GetRawBoneNum(), 1);
	TestEqual(TEXT("it is root"), Reference.GetBoneName(0), FName(TEXT("root")));

	FMeshDescription Description;
	if (TestTrue(TEXT("the mesh keeps its geometry"), Result.Mesh->CloneMeshDescription(0, Description)))
	{
		TestEqual(TEXT("the vertex count is the source's"), Description.Vertices().Num(), SourceVertexCount(*Cube));
	}
	bool bWhole = false;
	const TMap<int32, int32> Weighted = CountWeightedVertices(*Result.Mesh, bWhole);
	TestTrue(TEXT("every vertex is weighted whole"), bWhole);
	TestEqual(TEXT("to root alone"), Weighted.Num(), 1);

	const FBox Source = Cube->GetBounds().GetBox();
	const FBox Built = Result.Mesh->GetBounds().GetBox();
	TestTrue(TEXT("the bounds match the source's"), Built.Min.Equals(Source.Min, 0.01) && Built.Max.Equals(Source.Max, 0.01));
	TestEqual(TEXT("one material slot per source material"), Result.Mesh->GetMaterials().Num(), Cube->GetStaticMaterials().Num());

	const TMap<FName, FTransform> Sockets = FMantleWeaponRigidMesh::ReadSocketsInMeshSpace(Result.Mesh);
	const FTransform* FoundGrip = Sockets.Find(TEXT("Grip_R"));
	const FTransform* FoundMuzzle = Sockets.Find(TEXT("Muzzle"));
	TestTrue(TEXT("Grip_R is there, as given"), FoundGrip && NearlyEqual(*FoundGrip, Grip, 0.01));
	TestTrue(TEXT("Muzzle is there, as given"), FoundMuzzle && NearlyEqual(*FoundMuzzle, Muzzle, 0.01));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWeaponRigidMeshMagazineTest,
	"Mantle.Editor.WeaponRigidMesh.Magazine",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWeaponRigidMeshMagazineTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponRigidMeshTestPrivate;
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, CubePath);
	if (!TestNotNull(TEXT("the engine cube loads"), Cube))
	{
		return false;
	}

	FMantleWeaponRigidMeshInput Input;
	Input.BodyMesh = Cube;
	Input.PackagePath = TestFolder;
	Input.Name = TEXT("Magazine");
	FMantleWeaponRigidPart Magazine;
	Magazine.Mesh = Cube;
	Magazine.InBody = FTransform(FRotator(0.0, 20.0, 0.0), FVector(30.0, 0.0, -60.0), FVector(0.3, 0.3, 0.5));
	Input.Magazine = Magazine;
	const FTransform MagazineSocket(FRotator(10.0, 0.0, 0.0), FVector(30.0, 2.0, -45.0));
	const FTransform MuzzleSocket(FRotator::ZeroRotator, FVector(60.0, 0.0, 10.0));
	Input.Sockets.Add(TEXT("Magazine"), MagazineSocket);
	Input.Sockets.Add(TEXT("Muzzle"), MuzzleSocket);

	FMantleWeaponRigidMeshResult Result = FMantleWeaponRigidMesh::Build(Input);
	FRigidCleanup Cleanup(Input.Name, &Result);
	for (const FString& Error : Result.Errors)
	{
		AddError(Error);
	}
	if (!TestNotNull(TEXT("a mesh is built"), Result.Mesh))
	{
		return false;
	}

	const FReferenceSkeleton& Reference = Result.Mesh->GetRefSkeleton();
	if (!TestEqual(TEXT("two bones"), Reference.GetRawBoneNum(), 2))
	{
		return false;
	}
	TestEqual(TEXT("the second is the magazine"), Reference.GetBoneName(1), FName(TEXT("magazine")));
	TestEqual(TEXT("under root"), Reference.GetParentIndex(1), 0);
	// The bone sits at the part's pivot: its placement without the part's scale.
	TestTrue(TEXT("the magazine bone is at the part's placement"), NearlyEqual(Reference.GetRefBonePose()[1], Magazine.InBody, 0.01));

	bool bWhole = false;
	const TMap<int32, int32> Weighted = CountWeightedVertices(*Result.Mesh, bWhole);
	TestTrue(TEXT("every vertex is weighted whole"), bWhole);
	const int32 CubeVertices = SourceVertexCount(*Cube);
	TestEqual(TEXT("the magazine's vertices are on bone 1"), Weighted.FindRef(1), CubeVertices);
	TestEqual(TEXT("the body's are on root"), Weighted.FindRef(0), CubeVertices);

	const TMap<FName, FTransform> Sockets = FMantleWeaponRigidMesh::ReadSocketsInMeshSpace(Result.Mesh);
	const FTransform* FoundMagazine = Sockets.Find(TEXT("Magazine"));
	const FTransform* FoundMuzzle = Sockets.Find(TEXT("Muzzle"));
	TestTrue(TEXT("the Magazine socket resolves to the input"), FoundMagazine && NearlyEqual(*FoundMagazine, MagazineSocket, 0.01));
	TestTrue(TEXT("the Muzzle socket resolves to the input"), FoundMuzzle && NearlyEqual(*FoundMuzzle, MuzzleSocket, 0.01));
	if (const USkeletalMeshSocket* Socket = Result.Mesh->FindSocket(TEXT("Magazine")))
	{
		TestEqual(TEXT("the Magazine socket is on the magazine bone"), Socket->BoneName, FName(TEXT("magazine")));
	}
	else
	{
		AddError(TEXT("the mesh has no Magazine socket"));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWeaponRigidMeshKeepsEveryTriangleTest,
	"Mantle.Editor.WeaponRigidMesh.KeepsEveryTriangle",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWeaponRigidMeshKeepsEveryTriangleTest::RunTest(const FString& Parameters)
{
	// A new LOD's default reduction settings (half the triangles, base LOD 0) reduced LOD0 itself in the build.
	using namespace MantleWeaponRigidMeshTestPrivate;
	UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (!TestNotNull(TEXT("the engine sphere loads"), Sphere))
	{
		return false;
	}
	const int32 SourceTriangles = SourceTriangleCount(*Sphere);
	if (!TestTrue(TEXT("the source has triangles enough for a reduction to show"), SourceTriangles > 100))
	{
		return false;
	}

	FMantleWeaponRigidMeshInput Input;
	Input.BodyMesh = Sphere;
	Input.PackagePath = TestFolder;
	Input.Name = TEXT("KeepsEveryTriangle");
	Input.Sockets.Add(TEXT("Muzzle"), FTransform(FRotator::ZeroRotator, FVector(60.0, 0.0, 10.0)));

	FMantleWeaponRigidMeshResult Result = FMantleWeaponRigidMesh::Build(Input);
	FRigidCleanup Cleanup(Input.Name, &Result);
	for (const FString& Error : Result.Errors)
	{
		AddError(Error);
	}
	if (!TestNotNull(TEXT("a mesh is built"), Result.Mesh))
	{
		return false;
	}

	if (const FSkeletalMeshLODInfo* LODInfo = Result.Mesh->GetLODInfo(0))
	{
		TestEqual(TEXT("LOD0 keeps every triangle in its reduction settings"), LODInfo->ReductionSettings.NumOfTrianglesPercentage, 1.0f);
		TestEqual(TEXT("LOD0 keeps every vertex in its reduction settings"), LODInfo->ReductionSettings.NumOfVertPercentage, 1.0f);
	}
	else
	{
		AddError(TEXT("the mesh has no LOD0 info"));
	}
	TestFalse(TEXT("no reduction runs on LOD0"), Result.Mesh->IsReductionActive(0));
	TestEqual(TEXT("the saved render data has every source triangle"), RenderTriangleCount(*Result.Mesh), SourceTriangles);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWeaponRigidMeshNullBodyTest,
	"Mantle.Editor.WeaponRigidMesh.NullBody",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWeaponRigidMeshNullBodyTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponRigidMeshTestPrivate;
	FMantleWeaponRigidMeshInput Input;
	Input.PackagePath = TestFolder;
	Input.Name = TEXT("NullBody");

	FMantleWeaponRigidMeshResult Result = FMantleWeaponRigidMesh::Build(Input);
	FRigidCleanup Cleanup(Input.Name, &Result);
	TestTrue(TEXT("a null body is an error"), Result.Errors.Num() > 0);
	TestNull(TEXT("and builds no mesh"), Result.Mesh);
	TestNull(TEXT("and no skeleton"), Result.Skeleton);
	TestNull(TEXT("and nothing is left in the package"), FindObject<UObject>(nullptr, *(FString(TestFolder) / TEXT("SKM_NullBody.SKM_NullBody"))));

	// A mesh that is neither static nor skeletal is refused too.
	Input.BodyMesh = GetTransientPackage();
	const FMantleWeaponRigidMeshResult Wrong = FMantleWeaponRigidMesh::Build(Input);
	TestTrue(TEXT("an unsupported body is an error"), Wrong.Errors.Num() > 0 && !Wrong.Mesh);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWeaponRigidMeshReExportTest,
	"Mantle.Editor.WeaponRigidMesh.ReExport",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWeaponRigidMeshReExportTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponRigidMeshTestPrivate;
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, CubePath);
	if (!TestNotNull(TEXT("the engine cube loads"), Cube))
	{
		return false;
	}

	// First export: a magazine bone and a Muzzle socket.
	FMantleWeaponRigidMeshInput First;
	First.BodyMesh = Cube;
	First.PackagePath = TestFolder;
	First.Name = TEXT("ReExport");
	FMantleWeaponRigidPart Magazine;
	Magazine.Mesh = Cube;
	Magazine.InBody = FTransform(FRotator::ZeroRotator, FVector(30.0, 0.0, -60.0), FVector(0.3, 0.3, 0.5));
	First.Magazine = Magazine;
	First.Sockets.Add(TEXT("Muzzle"), FTransform(FRotator::ZeroRotator, FVector(60.0, 0.0, 10.0)));
	FMantleWeaponRigidMeshResult FirstResult = FMantleWeaponRigidMesh::Build(First);
	FRigidCleanup Cleanup(First.Name, nullptr);
	for (const FString& Error : FirstResult.Errors)
	{
		AddError(Error);
	}
	if (!TestNotNull(TEXT("the first export builds a mesh"), FirstResult.Mesh))
	{
		return false;
	}

	// Second export to the same names: no magazine, a Grip_R socket. It must save without error.
	FMantleWeaponRigidMeshInput Second;
	Second.BodyMesh = Cube;
	Second.PackagePath = TestFolder;
	Second.Name = First.Name;
	const FTransform Grip(FRotator::ZeroRotator, FVector(5.0, 1.0, -20.0));
	Second.Sockets.Add(TEXT("Grip_R"), Grip);
	FMantleWeaponRigidMeshResult SecondResult = FMantleWeaponRigidMesh::Build(Second);
	Cleanup.Result = &SecondResult;
	for (const FString& Error : SecondResult.Errors)
	{
		AddError(Error);
	}
	if (!TestNotNull(TEXT("the second export builds a mesh"), SecondResult.Mesh))
	{
		return false;
	}
	TestEqual(TEXT("the bones are the second input's: root alone"), SecondResult.Mesh->GetRefSkeleton().GetRawBoneNum(), 1);
	const TMap<FName, FTransform> Sockets = FMantleWeaponRigidMesh::ReadSocketsInMeshSpace(SecondResult.Mesh);
	TestFalse(TEXT("the first input's Muzzle socket is gone"), Sockets.Contains(TEXT("Muzzle")));
	const FTransform* FoundGrip = Sockets.Find(TEXT("Grip_R"));
	TestTrue(TEXT("the second input's Grip_R is there"), FoundGrip && NearlyEqual(*FoundGrip, Grip, 0.01));
	const FString File = FPackageName::LongPackageNameToFilename(FString(TestFolder) / TEXT("SKM_ReExport"), FPackageName::GetAssetPackageExtension());
	TestTrue(TEXT("the mesh's file is on disk"), IFileManager::Get().FileExists(*File));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
