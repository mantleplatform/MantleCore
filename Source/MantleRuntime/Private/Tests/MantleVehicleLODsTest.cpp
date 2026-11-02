// Copyright Joshua Eger. All Rights Reserved.
#include "MantleRuntimeSkeletalMeshBuilder.h"
#include "MantleVehicleAsset.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

// A vehicle body's levels of detail as the client builds them: the cook's own fixture (backend
// TestVehicleLODsFixtureMatchesWhatWeWriteToday) with three LODs of 76, 76 and 44 triangles on one material list.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleLODsTest,
	"Mantle.Vehicle.LODs.BuildsEveryLOD",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleLODsTest::RunTest(const FString&)
{
	const FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("../schema/fixtures/vehicle-lods-v1.mvh")));
	TArray<uint8> Bytes;
	if (!TestTrue(*FString::Printf(TEXT("the LODs fixture exists (%s)"), *Path), FFileHelper::LoadFileToArray(Bytes, *Path)))
	{
		return false;
	}
	FMantleVehicleAsset Asset;
	FString Error;
	if (!TestTrue(*FString::Printf(TEXT("the reader takes a vehicle with three LODs (%s)"), *Error), FMantleVehicleAssetReader::Read(Bytes, Asset, Error)))
	{
		return false;
	}
	if (!TestEqual(TEXT("the body carries three LODs"), Asset.Body.LODs.Num(), 3))
	{
		return false;
	}
	TestEqual(TEXT("LOD2 is the front wheels' car"), Asset.Body.LODs[2].Indices.Num() / 3, 44);

	const UMantleBuiltVehicle* Built = UMantleBuiltVehicle::Build(Asset, GetTransientPackage(), &Error);
	if (!TestNotNull(*FString::Printf(TEXT("the vehicle builds (%s)"), *Error), Built) || !TestNotNull(TEXT("with a mesh"), Built->Mesh.Get()))
	{
		return false;
	}
	const USkeletalMesh* Mesh = Built->Mesh;
	if (!TestEqual(TEXT("the built mesh has every LOD"), Mesh->GetLODNum(), 3))
	{
		return false;
	}
	for (int32 LOD = 0; LOD < 3; ++LOD)
	{
		TestEqual(*FString::Printf(TEXT("LOD%d switches at lods.screenSizes"), LOD), Mesh->GetLODInfo(LOD)->ScreenSize.Default,
			FMantleRuntimeSkeletalMeshBuilder::DefaultLODScreenSize(LOD));
	}
	TestEqual(TEXT("one material slot per LOD0 section, shared by every LOD"), Mesh->GetMaterials().Num(), Asset.Body.LODs[0].Sections.Num());
	TestTrue(TEXT("the character collision is LOD0's"), Built->BodyCollision.IsValid());
	return true;
}

#endif
