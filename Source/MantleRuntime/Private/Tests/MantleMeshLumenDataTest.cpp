// Copyright Joshua Eger. All Rights Reserved.
#include "DistanceFieldAtlas.h"
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
	const FString Mesh = TEXT("sha256:") + FString::ChrN(64, TEXT('a'));
	const FString Other = TEXT("sha256:") + FString::ChrN(64, TEXT('b'));

	FDistanceFieldVolumeData Field;
	Field.LocalSpaceMeshBounds = FBox3f(FVector3f(-50.0f), FVector3f(50.0f));
	Field.bMostlyTwoSided = true;
	// Mip 0 (the finest) streamed at bytes 16..63, the coarsest resident.
	Field.Mips[0].IndirectionDimensions = FIntVector(2, 2, 2);
	Field.Mips[0].NumDistanceFieldBricks = 3;
	Field.Mips[0].BulkOffset = 16;
	Field.Mips[0].BulkSize = 48;
	Field.Mips[DistanceField::NumMips - 1].IndirectionDimensions = FIntVector(1, 1, 1);
	Field.Mips[DistanceField::NumMips - 1].BulkSize = 3;
	Field.AlwaysLoadedMip = { 9, 8, 7 };
	Field.StreamableMips.Lock(LOCK_READ_WRITE);
	uint8* Streamed = Field.StreamableMips.Realloc(64);
	FMemory::Memset(Streamed, 0x11, 16);
	FMemory::Memset(Streamed + 16, 0x5A, 48);
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
	// The finest mip comes back as the resident one, and no mip is left to stream (a runtime mesh has no file to stream it from).
	const FSparseDistanceFieldMip& Resident = Read.DistanceField->Mips[DistanceField::NumMips - 1];
	TestEqual(TEXT("resident mip is the finest"), Resident.IndirectionDimensions, FIntVector(2, 2, 2));
	TestEqual(TEXT("bricks"), Resident.NumDistanceFieldBricks, 3);
	TArray<uint8> Finest;
	Finest.Init(0x5A, 48);
	TestEqual(TEXT("resident bytes"), Read.DistanceField->AlwaysLoadedMip, Finest);
	TestEqual(TEXT("resident size"), Resident.BulkSize, 48u);
	TestEqual(TEXT("nothing to stream"), Read.DistanceField->Mips[0].BulkSize, 0u);
	TestTrue(TEXT("still a valid field"), Read.DistanceField->IsValid());
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
