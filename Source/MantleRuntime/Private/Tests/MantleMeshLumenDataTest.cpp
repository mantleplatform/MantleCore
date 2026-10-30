// Copyright Joshua Eger. All Rights Reserved.
#include "DistanceFieldAtlas.h"
#include "MantleAssetStore.h"
#include "MantleMeshLumenData.h"
#include "MeshCardBuild.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// #149: the MLD1 side file reads back what was written, for its own mesh only, and anything else reads as "not
// present". The bake itself is editor only (MeshUtilities); this builds a small field by hand.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMeshLumenFileTest,
	"Mantle.Runtime.StaticMesh.LumenFile",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMeshLumenFileTest::RunTest(const FString& Parameters)
{
	const uint8 MeshBytes[] = { 1, 2, 3, 4 };
	const uint8 OtherBytes[] = { 5, 6, 7, 8 };
	const FMantleContentHash Mesh = FMantleContentHash::OfBytes(MeshBytes, sizeof(MeshBytes));
	const FMantleContentHash Other = FMantleContentHash::OfBytes(OtherBytes, sizeof(OtherBytes));

	FDistanceFieldVolumeData Field;
	Field.LocalSpaceMeshBounds = FBox3f(FVector3f(-50.0f), FVector3f(50.0f));
	Field.bMostlyTwoSided = true;
	Field.Mips[0].IndirectionDimensions = FIntVector(2, 2, 2);
	Field.Mips[0].NumDistanceFieldBricks = 3;
	Field.AlwaysLoadedMip = { 9, 8, 7 };
	Field.StreamableMips.Lock(LOCK_READ_WRITE);
	FMemory::Memset(Field.StreamableMips.Realloc(64, 1), 0x5A, 64);
	Field.StreamableMips.Unlock();
	FCardRepresentationData Cards;
	Cards.MeshCardsBuildData.Bounds = FBox(FVector(-50.0), FVector(50.0));
	Cards.MeshCardsBuildData.CardBuildData.AddDefaulted(2);

	const TArray<uint8> File = FMantleMeshLumenFile::Write(Mesh, Field, Cards);
	TestTrue(TEXT("a valid field writes a file"), File.Num() > 0);

	FMantleMeshLumenData Read;
	FString Reason;
	if (!TestTrue(TEXT("its own mesh reads it"), FMantleMeshLumenFile::Read(File, Mesh, Read, &Reason)))
	{
		AddInfo(Reason);
		return false;
	}
	TestEqual(TEXT("bounds"), Read.DistanceField->LocalSpaceMeshBounds, Field.LocalSpaceMeshBounds);
	TestTrue(TEXT("two-sided"), Read.DistanceField->bMostlyTwoSided);
	TestEqual(TEXT("bricks"), Read.DistanceField->Mips[0].NumDistanceFieldBricks, 3);
	TestEqual(TEXT("always-loaded mip"), Read.DistanceField->AlwaysLoadedMip, Field.AlwaysLoadedMip);
	TestEqual(TEXT("streamed mips"), Read.DistanceField->StreamableMips.GetBulkDataSize(), 64LL);
	TestEqual(TEXT("cards"), Read.Cards->MeshCardsBuildData.CardBuildData.Num(), 2);

	TestFalse(TEXT("another mesh's file is not present"), FMantleMeshLumenFile::Read(File, Other, Read, &Reason));
	TestFalse(TEXT("nothing is left behind"), Read.DistanceField.IsValid() || Read.Cards.IsValid());
	TArray<uint8> Cut = File;
	Cut.SetNum(Cut.Num() - 5);
	TestFalse(TEXT("a cut file is not present"), FMantleMeshLumenFile::Read(Cut, Mesh, Read, &Reason));
	TArray<uint8> Engine = File;
	Engine[12] ^= 0x01; // inside the engine tag's characters (after magic, version and the string's length)
	TestFalse(TEXT("another engine's file is not present"), FMantleMeshLumenFile::Read(Engine, Mesh, Read, &Reason));
	TestFalse(TEXT("an empty file is not present"), FMantleMeshLumenFile::Read(TArray<uint8>(), Mesh, Read, &Reason));

	FDistanceFieldVolumeData Empty;
	TestEqual(TEXT("a mesh with no field gets no file"), FMantleMeshLumenFile::Write(Mesh, Empty, Cards).Num(), 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
