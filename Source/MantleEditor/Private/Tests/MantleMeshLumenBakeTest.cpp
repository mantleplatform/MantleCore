// Copyright Joshua Eger. All Rights Reserved.
#include "DistanceFieldAtlas.h"
#include "MantleGlbWriter.h"
#include "MantleMeshLumenBake.h"
#include "MantleMeshLumenData.h"
#include "MeshCardBuild.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// #149: a place mesh's Lumen side file, baked by MeshUtilities from the geometry the upload carries. Each section bakes as
// its material draws it: a two-sided card (foliage, a thin sheet) is solid from both sides, a translucent mesh gets none.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleMeshLumenBakeTestPrivate
{
	const FString MeshTag = TEXT("sha256:") + FString::ChrN(64, TEXT('c'));

	/** A flat 200 cm square card in the XY plane, facing +Z: one primitive, two triangles. */
	FMantleGlbMesh MakeCard()
	{
		FMantleGlbMesh Mesh;
		Mesh.Name = TEXT("Card");
		Mesh.Positions = { FVector3f(-100, -100, 0), FVector3f(100, -100, 0), FVector3f(100, 100, 0), FVector3f(-100, 100, 0) };
		Mesh.MaterialNames = { TEXT("Leaf") };
		FMantleGlbPrimitive& Primitive = Mesh.Primitives.AddDefaulted_GetRef();
		Primitive.MaterialIndex = 0;
		Primitive.FirstVertex = 0;
		Primitive.NumVertices = 4;
		Primitive.Indices = { 0, 2, 1, 0, 3, 2 };
		return Mesh;
	}

	/** A closed 100 cm box: 8 corners, 12 triangles facing out, one primitive. */
	FMantleGlbMesh MakeBox()
	{
		FMantleGlbMesh Mesh;
		Mesh.Name = TEXT("Box");
		for (int32 Corner = 0; Corner < 8; ++Corner)
		{
			Mesh.Positions.Add(FVector3f((Corner & 1) ? 50.f : -50.f, (Corner & 2) ? 50.f : -50.f, (Corner & 4) ? 50.f : -50.f));
		}
		Mesh.MaterialNames = { TEXT("Wall") };
		FMantleGlbPrimitive& Primitive = Mesh.Primitives.AddDefaulted_GetRef();
		Primitive.NumVertices = 8;
		Primitive.Indices = { 0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6, 0, 1, 4, 1, 5, 4, 2, 6, 3, 3, 6, 7, 0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5 };
		return Mesh;
	}

	/** The baked file read back, or null with the bake's reason in OutWhy. */
	TOptional<FMantleMeshLumenData> BakeAndRead(const FMantleGlbMesh& Mesh, const FMantleMeshLumenSection& Section, FString& OutWhy)
	{
		TArray<uint8> File;
		if (!FMantleMeshLumenBake::Bake(Mesh, { Section }, MeshTag, File, OutWhy))
		{
			return {};
		}
		FMantleMeshLumenData Data;
		if (!FMantleMeshLumenFile::Read(File, MeshTag, Data, &OutWhy))
		{
			return {};
		}
		return MoveTemp(Data);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMeshLumenBakeTwoSidedTest,
	"Mantle.Editor.Lumen.Bake.TwoSidedCard",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMeshLumenBakeTwoSidedTest::RunTest(const FString& Parameters)
{
	using namespace MantleMeshLumenBakeTestPrivate;

	FMantleMeshLumenSection TwoSided;
	TwoSided.BlendMode = BLEND_Masked;
	TwoSided.bTwoSided = true;
	FString Why;
	TOptional<FMantleMeshLumenData> Card = BakeAndRead(MakeCard(), TwoSided, Why);
	if (!TestTrue(TEXT("a two-sided card bakes"), Card.IsSet()))
	{
		AddError(Why);
		return false;
	}
	TestTrue(TEXT("its field is two-sided"), Card->DistanceField->bMostlyTwoSided);
	TestTrue(TEXT("its cards are two-sided"), Card->Cards->MeshCardsBuildData.bMostlyTwoSided);
	TestTrue(TEXT("its field spans the card"), Card->DistanceField->LocalSpaceMeshBounds.GetSize().X >= 200.f);

	// The same card one-sided is a sheet seen from one side only, so whatever it bakes to is not two-sided.
	TOptional<FMantleMeshLumenData> OneSided = BakeAndRead(MakeCard(), FMantleMeshLumenSection(), Why);
	TestTrue(TEXT("one-sided, it is not two-sided"), !OneSided.IsSet() || !OneSided->DistanceField->bMostlyTwoSided);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMeshLumenBakeSolidTest,
	"Mantle.Editor.Lumen.Bake.SolidAndTranslucent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMeshLumenBakeSolidTest::RunTest(const FString& Parameters)
{
	using namespace MantleMeshLumenBakeTestPrivate;

	FString Why;
	TOptional<FMantleMeshLumenData> Box = BakeAndRead(MakeBox(), FMantleMeshLumenSection(), Why);
	if (!TestTrue(TEXT("an opaque box bakes"), Box.IsSet()))
	{
		AddError(Why);
		return false;
	}
	TestTrue(TEXT("its field is valid"), Box->DistanceField->IsValid());
	TestFalse(TEXT("its field is one-sided"), Box->DistanceField->bMostlyTwoSided);
	TestTrue(TEXT("it has cards"), Box->Cards->MeshCardsBuildData.CardBuildData.Num() > 0);
	TestEqual(TEXT("the finest mip is the resident one"), Box->DistanceField->Mips[0].BulkSize, 0u);

	FMantleMeshLumenSection Glass;
	Glass.BlendMode = BLEND_Translucent;
	TArray<uint8> File;
	TestFalse(TEXT("a translucent box gets none"), FMantleMeshLumenBake::Bake(MakeBox(), { Glass }, MeshTag, File, Why));
	TestTrue(TEXT("and says why"), Why.Contains(TEXT("translucent")));
	return true;
}

// The bake on worker threads makes what the bake in line makes, byte for byte, with the same reason where there is no file: several
// different meshes (more than there are workers, so the launches have to wait for a free one) baked both ways.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMeshLumenBakeWorkersTest,
	"Mantle.Editor.Lumen.Bake.WorkersMatchInline",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMeshLumenBakeWorkersTest::RunTest(const FString& Parameters)
{
	using namespace MantleMeshLumenBakeTestPrivate;

	struct FCase
	{
		FString Name;
		FMantleGlbMesh Mesh;
		FMantleMeshLumenSection Section;
	};
	TArray<FCase> Cases;
	const int32 Count = FMantleMeshLumenBake::MaxInFlight() * 2 + 3;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		FCase& Case = Cases.AddDefaulted_GetRef();
		Case.Name = FString::Printf(TEXT("case %d"), Index);
		switch (Index % 4)
		{
		case 0: // a box of a size of its own
			Case.Mesh = MakeBox();
			for (FVector3f& Position : Case.Mesh.Positions)
			{
				Position *= 1.0f + 0.37f * Index;
			}
			break;
		case 1: // a two-sided card
			Case.Mesh = MakeCard();
			Case.Section.BlendMode = BLEND_Masked;
			Case.Section.bTwoSided = true;
			for (FVector3f& Position : Case.Mesh.Positions)
			{
				Position *= 1.0f + 0.21f * Index;
			}
			break;
		case 2: // translucent: no file, and a reason
			Case.Mesh = MakeBox();
			Case.Section.BlendMode = BLEND_Translucent;
			break;
		default: // no triangles: no file, and another reason
			Case.Mesh.Name = TEXT("Empty");
			break;
		}
	}

	// In line first, then every case started before any is waited for.
	TArray<TArray<uint8>> InlineFiles;
	TArray<FString> InlineReasons;
	TArray<bool> InlineBaked;
	for (const FCase& Case : Cases)
	{
		TArray<uint8>& File = InlineFiles.AddDefaulted_GetRef();
		FString& Reason = InlineReasons.AddDefaulted_GetRef();
		InlineBaked.Add(FMantleMeshLumenBake::Bake(Case.Mesh, { Case.Section }, MeshTag, File, Reason));
	}
	TArray<TSharedRef<FMantleMeshLumenJob>> Jobs;
	for (const FCase& Case : Cases)
	{
		Jobs.Add(FMantleMeshLumenBake::Launch(FMantleGlbMesh(Case.Mesh), { Case.Section }, MeshTag));
	}
	int32 Baked = 0;
	for (int32 Index = 0; Index < Cases.Num(); ++Index)
	{
		Jobs[Index]->Wait();
		const FMantleMeshLumenResult& Result = Jobs[Index]->GetResult();
		TestEqual(*FString::Printf(TEXT("%s: baked on a worker or not as in line"), *Cases[Index].Name), Result.bBaked, InlineBaked[Index]);
		TestEqual(*FString::Printf(TEXT("%s: the same reason"), *Cases[Index].Name), Result.Reason, InlineReasons[Index]);
		TestTrue(*FString::Printf(TEXT("%s: the same bytes"), *Cases[Index].Name), Result.File == InlineFiles[Index]);
		Baked += Result.bBaked ? 1 : 0;
	}
	TestTrue(TEXT("some of the cases make a file, so the bytes were compared"), Baked > 0);
	TestTrue(TEXT("some of the cases make none, so the reasons were compared"), Baked < Cases.Num());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
