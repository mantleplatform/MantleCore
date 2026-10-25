// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCosmeticExport.h"
#include "MantleSkinnedMesh.h"
#include "MantleVehicleAsset.h"
#include "MantleVehicleBodySplit.h"
#include "MantleVehicleBuilder.h"
#include "MantleVehicleConverter.h"
#include "MantleVehicleSource.h"
#include "MantlePlaceValidator.h"

#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Misc/AutomationTest.h"
#include "StaticMeshAttributes.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS

// A body that draws more materials than a published one may (8) is split into the body and parts of at most 8 materials each, and keeps its look. A
// synthetic model of 14 materials, each its own quad strip, is split on its own (every triangle keeps its material, its place and its bone) and built as a vehicle (it
// publishes: no check refuses it, the body reads back with at most 8 materials and the part with the rest).

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVehicleBodySplitTestPrivate
{
	constexpr int32 Materials = 14;

	/** The key a triangle is known by: its material's name and its corners, rounded, in order of position (the same triangle wherever it sits in a mesh). */
	FString TriangleKey(const FName Material, const FVector& A, const FVector& B, const FVector& C)
	{
		TArray<FString> Corners;
		for (const FVector& Point : { A, B, C })
		{
			Corners.Add(FString::Printf(TEXT("%d,%d,%d"), FMath::RoundToInt(Point.X), FMath::RoundToInt(Point.Y), FMath::RoundToInt(Point.Z)));
		}
		Corners.Sort();
		return Material.ToString() + TEXT("|") + FString::Join(Corners, TEXT("|"));
	}

	/** Material k has k + 1 quads (so 14 is the biggest and the body's keepers are known), each quad its own four vertices; odd materials ride bone 1. */
	void MakeBody(FMantleSkinnedMesh& OutBody, TArray<FSkeletalMaterial>& OutMaterials)
	{
		OutBody.Bones = { { FName(TEXT("root")), INDEX_NONE }, { FName(TEXT("hub")), 0 } };
		OutBody.ReferencePose = { FTransform::Identity, FTransform(FVector(100.0, 0.0, 0.0)) };
		FMantleSkinnedMeshLOD& LOD = OutBody.LODs.AddDefaulted_GetRef();
		LOD.NumInfluences = 1;
		int32 Quad = 0;
		for (int32 Material = 0; Material < Materials; ++Material)
		{
			OutMaterials.Add(FSkeletalMaterial(nullptr, FName(*FString::Printf(TEXT("mat%d"), Material))));
			FMantleSkinnedMeshSection& Section = LOD.Sections.AddDefaulted_GetRef();
			Section.MaterialIndex = Material;
			Section.MaterialName = FName(*FString::Printf(TEXT("mat%d"), Material));
			Section.FirstIndex = LOD.Indices.Num();
			for (int32 Each = 0; Each <= Material; ++Each, ++Quad)
			{
				const uint32 First = static_cast<uint32>(LOD.Positions.Num());
				const float X = 20.0f * Quad;
				LOD.Positions.Add(FVector3f(X, 0.0f, 0.0f));
				LOD.Positions.Add(FVector3f(X + 10.0f, 0.0f, 0.0f));
				LOD.Positions.Add(FVector3f(X + 10.0f, 10.0f, 0.0f));
				LOD.Positions.Add(FVector3f(X, 10.0f, 0.0f));
				for (int32 Corner = 0; Corner < 4; ++Corner)
				{
					LOD.Normals.Add(FVector3f(0.0f, 0.0f, 1.0f));
					LOD.UVs.Add(FVector2f(0.25f * Corner, 0.5f));
					LOD.InfluenceBones.Add(static_cast<uint16>(Material % 2));
					LOD.InfluenceWeights.Add(255);
				}
				LOD.Indices.Append({ First, First + 1, First + 2, First, First + 2, First + 3 });
			}
			Section.IndexCount = LOD.Indices.Num() - Section.FirstIndex;
		}
	}

	/** Every triangle of Body by key, with the bone its first vertex rides. */
	void Triangles(const FMantleSkinnedMesh& Body, const TArray<FSkeletalMaterial>& Slots, TMap<FString, int32>& OutKeys, TMap<FString, int32>& OutBones)
	{
		const FMantleSkinnedMeshLOD& LOD = Body.LODs[0];
		for (const FMantleSkinnedMeshSection& Section : LOD.Sections)
		{
			for (int32 At = Section.FirstIndex; At + 2 < Section.FirstIndex + Section.IndexCount; At += 3)
			{
				const FString Key = TriangleKey(Slots[Section.MaterialIndex].MaterialSlotName, FVector(LOD.Positions[LOD.Indices[At]]), FVector(LOD.Positions[LOD.Indices[At + 1]]),
					FVector(LOD.Positions[LOD.Indices[At + 2]]));
				OutKeys.FindOrAdd(Key) += 1;
				OutBones.Add(Key, LOD.InfluenceBones[LOD.Indices[At]]);
			}
		}
	}

	/** A static mesh of 14 quads in a row, each its own material, neighbours sharing their edge's vertex instances (so the render sections share vertices). */
	UStaticMesh* MakeStrip()
	{
		UObject* Outer = GetTransientPackage();
		UStaticMesh* Mesh = NewObject<UStaticMesh>(Outer, MakeUniqueObjectName(Outer, UStaticMesh::StaticClass(), FName(TEXT("MantleSplitTestStrip"))), RF_Transient);
		Mesh->bAllowCPUAccess = true;
		Mesh->SetNumSourceModels(1);
		FMeshDescription Description;
		FStaticMeshAttributes Attributes(Description);
		Attributes.Register();
		TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
		TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
		TVertexInstanceAttributesRef<FVector3f> Tangents = Attributes.GetVertexInstanceTangents();
		TVertexInstanceAttributesRef<float> BinormalSigns = Attributes.GetVertexInstanceBinormalSigns();
		TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
		TPolygonGroupAttributesRef<FName> GroupNames = Attributes.GetPolygonGroupMaterialSlotNames();
		UVs.SetNumChannels(1);
		TArray<FVertexInstanceID> Lower;
		TArray<FVertexInstanceID> Upper;
		for (int32 Column = 0; Column <= Materials; ++Column)
		{
			for (int32 Row = 0; Row < 2; ++Row)
			{
				const FVertexID Vertex = Description.CreateVertex();
				Positions[Vertex] = FVector3f(100.0f * Column, 100.0f * Row, 0.0f);
				const FVertexInstanceID Instance = Description.CreateVertexInstance(Vertex);
				Normals[Instance] = FVector3f(0.0f, 0.0f, 1.0f);
				Tangents[Instance] = FVector3f(1.0f, 0.0f, 0.0f);
				BinormalSigns[Instance] = 1.0f;
				UVs.Set(Instance, 0, FVector2f(static_cast<float>(Column) / Materials, static_cast<float>(Row)));
				(Row == 0 ? Lower : Upper).Add(Instance);
			}
		}
		for (int32 Column = 0; Column < Materials; ++Column)
		{
			const FName Slot(*FString::Printf(TEXT("strip%d"), Column));
			Mesh->GetStaticMaterials().Add(FStaticMaterial(nullptr, Slot, Slot));
			const FPolygonGroupID Group = Description.CreatePolygonGroup();
			GroupNames[Group] = Slot;
			const FVertexInstanceID First[3] = { Lower[Column], Lower[Column + 1], Upper[Column + 1] };
			const FVertexInstanceID Second[3] = { Lower[Column], Upper[Column + 1], Upper[Column] };
			Description.CreateTriangle(Group, TArrayView<const FVertexInstanceID>(First, 3));
			Description.CreateTriangle(Group, TArrayView<const FVertexInstanceID>(Second, 3));
		}
		FMeshBuildSettings& Settings = Mesh->GetSourceModel(0).BuildSettings;
		Settings.bRecomputeNormals = false;
		Settings.bRecomputeTangents = false;
		Settings.bGenerateLightmapUVs = false;
		Settings.bBuildReversedIndexBuffer = false;
		Settings.DistanceFieldResolutionScale = 0.0f;
		Settings.MaxLumenMeshCards = 0;
		UStaticMesh::FBuildMeshDescriptionsParams Params;
		Params.bMarkPackageDirty = false;
		Params.bAllowCpuAccess = true;
		TArray<const FMeshDescription*> Pointers = { &Description };
		Mesh->BuildFromMeshDescriptions(Pointers, Params);
		return Mesh;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleBodySplitTest,
	"Mantle.Vehicle.BodySplit.FourteenMaterialsSplitAndKeepTheirLook",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleBodySplitTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleBodySplitTestPrivate;
	FMantleSkinnedMesh Body;
	TArray<FSkeletalMaterial> Slots;
	MakeBody(Body, Slots);
	TestEqual(TEXT("the model draws 14 materials"), FMantleVehicleBodySplit::CountMaterials(Body), 14);
	TMap<FString, int32> Before;
	TMap<FString, int32> BoneBefore;
	Triangles(Body, Slots, Before, BoneBefore);
	const int32 TrianglesBefore = Body.LODs[0].Indices.Num() / 3;

	FMantleVehicleBodyExtras Extras;
	Extras.UVs.Add(Body.LODs[0].UVs);
	FMantleVehicleBodySplitResult Result;
	const bool bSplit = FMantleVehicleBodySplit::Split(Body, Slots, Extras, FMantleVehicleLimits::MaxSections, FMantleVehicleLimits::MaxPartMaterials,
		FMantleVehicleLimits::MaxParts, Result);
	if (!TestTrue(*FString::Printf(TEXT("it splits (%s)"), *Result.Problem), bSplit))
	{
		return false;
	}
	TestEqual(TEXT("it drew 14 before"), Result.MaterialsBefore, 14);
	TestEqual(TEXT("and the body draws 8 after"), FMantleVehicleBodySplit::CountMaterials(Body), 8);
	TestEqual(TEXT("with 8 material slots left"), Slots.Num(), 8);
	TestEqual(TEXT("the six smallest materials went to pieces, one piece for each of the two bones they ride (three materials each)"), Result.Pieces.Num(), 2);
	TestTrue(TEXT("the body keeps the biggest material"), Slots.ContainsByPredicate([](const FSkeletalMaterial& Slot) { return Slot.MaterialSlotName == FName(TEXT("mat13")); }));
	TestFalse(TEXT("and not the smallest"), Slots.ContainsByPredicate([](const FSkeletalMaterial& Slot) { return Slot.MaterialSlotName == FName(TEXT("mat0")); }));
	TestEqual(TEXT("the body's vertices are kept in step with its triangles (extra UV channel)"), Extras.UVs[0].Num(), Body.LODs[0].Positions.Num());
	TestEqual(TEXT("and its skin"), Body.LODs[0].InfluenceBones.Num(), Body.LODs[0].Positions.Num());
	TestEqual(TEXT("and its normals"), Body.LODs[0].Normals.Num(), Body.LODs[0].Positions.Num());

	// Every triangle is still somewhere, with the same material, the same place and the same bone.
	TMap<FString, int32> After;
	TMap<FString, int32> BoneAfter;
	Triangles(Body, Slots, After, BoneAfter);
	int32 InPieces = 0;
	for (const FMantleVehicleBodyPiece& Piece : Result.Pieces)
	{
		TestTrue(TEXT("a piece's bone is a bone of the body"), Piece.Bone == FName(TEXT("root")) || Piece.Bone == FName(TEXT("hub")));
		TestTrue(TEXT("a piece has at most 8 materials"), Piece.Materials.Num() <= FMantleVehicleLimits::MaxPartMaterials);
		TestTrue(TEXT("a hub piece is placed relative to the hub"), Piece.Bone != FName(TEXT("hub")) || Piece.ToBone.GetLocation().Equals(FVector(-100.0, 0.0, 0.0), 0.01));
		for (const FMantleVehicleBodyPiece::FSection& Section : Piece.Sections)
		{
			for (int32 At = Section.FirstIndex; At + 2 < Section.FirstIndex + Section.IndexCount; At += 3)
			{
				const FString Key = TriangleKey(Piece.SlotNames[Section.Material], FVector(Piece.Positions[Piece.Indices[At]]), FVector(Piece.Positions[Piece.Indices[At + 1]]),
					FVector(Piece.Positions[Piece.Indices[At + 2]]));
				After.FindOrAdd(Key) += 1;
				BoneAfter.Add(Key, Piece.BoneIndex);
				++InPieces;
			}
		}
	}
	TestEqual(TEXT("no triangle was lost or added"), InPieces + Body.LODs[0].Indices.Num() / 3, TrianglesBefore);
	TestEqual(TEXT("the same triangles by material and place"), After.Num(), Before.Num());
	int32 Missing = 0;
	int32 WrongBone = 0;
	for (const TPair<FString, int32>& Triangle : Before)
	{
		const int32* Now = After.Find(Triangle.Key);
		Missing += Now && *Now == Triangle.Value ? 0 : 1;
		const int32* Bone = BoneAfter.Find(Triangle.Key);
		WrongBone += Bone && *Bone == BoneBefore[Triangle.Key] ? 0 : 1;
	}
	TestEqual(TEXT("each triangle has its material and place"), Missing, 0);
	TestEqual(TEXT("and rides its bone"), WrongBone, 0);

	// A body within the cap is left alone, and one whose pieces would be more than a vehicle's parts says why.
	FMantleSkinnedMesh Small;
	TArray<FSkeletalMaterial> SmallSlots;
	MakeBody(Small, SmallSlots);
	FMantleVehicleBodyExtras NoExtras;
	FMantleVehicleBodySplitResult Refused;
	TestFalse(TEXT("pieces past the parts a vehicle may have are refused"), FMantleVehicleBodySplit::Split(Small, SmallSlots, NoExtras, FMantleVehicleLimits::MaxSections, FMantleVehicleLimits::MaxPartMaterials, 1, Refused));
	TestFalse(TEXT("with a reason"), Refused.Problem.IsEmpty());
	TestEqual(TEXT("and the body untouched"), FMantleVehicleBodySplit::CountMaterials(Small), 14);
	FMantleVehicleBodySplitResult Within;
	TestTrue(TEXT("a body already within the cap is not split"), FMantleVehicleBodySplit::Split(Body, Slots, Extras, FMantleVehicleLimits::MaxSections, FMantleVehicleLimits::MaxPartMaterials, FMantleVehicleLimits::MaxParts, Within));
	TestEqual(TEXT("and has no pieces"), Within.Pieces.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleBodySplitBuildTest,
	"Mantle.Vehicle.BodySplit.FourteenMaterialModelBuildsAndPublishes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleBodySplitBuildTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleBodySplitTestPrivate;
	const TStrongObjectPtr<UStaticMesh> Strip(MakeStrip());
	if (!TestTrue(TEXT("the 14-material strip is built"), Strip.IsValid() && Strip->GetRenderData() && Strip->GetRenderData()->LODResources.Num() > 0))
	{
		return false;
	}
	FMantleVehicleModelOptions Options;
	Options.Folder = TEXT("/Temp/MantleBodySplitTest");
	Options.Name = TEXT("SplitCar");
	Options.bOverwrite = true;
	const FMantleVehicleBuildResult Built = FMantleVehicleBuilder::BuildFromModel(Strip.Get(), TEXT("sedan"), Options);
	for (const FString& Problem : Built.Problems)
	{
		AddError(Problem);
	}
	if (!TestTrue(TEXT("the vehicle is built"), Built.bOk && Built.Source != nullptr))
	{
		return false;
	}
	UMantleVehicleSource* Source = Built.Source;

	// It publishes: the check that refuses an over-cap body finds nothing to refuse.
	TArray<FMantleValidationIssue> Issues;
	FMantleVehicleConverter::CheckSource(Source, FString(), Issues);
	for (const FMantleValidationIssue& Issue : Issues)
	{
		if (Issue.Severity == EMantleIssueSeverity::Error)
		{
			AddError(FString::Printf(TEXT("publishing would refuse it: %s %s"), *Issue.Problem, *Issue.Remedy));
		}
	}
	bool bSaid = false;
	for (const FString& Note : Built.Notes)
	{
		TestFalse(TEXT("no note tells the creator to fix something before publishing"), Note.StartsWith(TEXT("Before you publish")));
		bSaid = bSaid || (Note.Contains(TEXT("split it for you")) && Note.Contains(TEXT("14")));
	}
	TestTrue(TEXT("and one says in plain words what was done"), bSaid);

	// Its sections: the body within the cap, the rest in the part, all 14 and all 28 triangles.
	USkeletalMesh* BodyMesh = Source->Body.LoadSynchronous();
	if (!TestNotNull(TEXT("it has a body"), BodyMesh) || !TestEqual(TEXT("and one part"), Source->Parts.Num(), 1))
	{
		return false;
	}
	FMantleGlbMesh BodyGlb;
	TArray<UMaterialInterface*> BodyMaterials;
	int32 BodyTriangles = 0;
	FString Error;
	if (!TestTrue(*FString::Printf(TEXT("the body reads for upload (%s)"), *Error), FMantleCosmeticExport::ReadSkeletalMesh(BodyMesh, 0, BodyGlb, BodyMaterials, BodyTriangles, Error)))
	{
		return false;
	}
	TestTrue(*FString::Printf(TEXT("the body has at most 8 sections (%d)"), BodyGlb.MaterialNames.Num()), BodyGlb.MaterialNames.Num() <= FMantleVehicleLimits::MaxSections);
	const UStaticMesh* PartMesh = Source->Parts[0].Mesh.LoadSynchronous();
	if (!TestNotNull(TEXT("the part has a mesh"), PartMesh))
	{
		return false;
	}
	FMantleGlbMesh PartGlb;
	TArray<int32> PartSlots;
	int32 PartTriangles = 0;
	if (!TestTrue(*FString::Printf(TEXT("the part reads for upload (%s)"), *Error), FMantleCosmeticExport::ReadStaticMesh(PartMesh, 0, PartGlb, PartSlots, PartTriangles, Error)))
	{
		return false;
	}
	TestTrue(TEXT("the part has at most 8 sections"), PartGlb.MaterialNames.Num() <= FMantleVehicleLimits::MaxPartMaterials);
	TestEqual(TEXT("14 materials in all"), BodyGlb.MaterialNames.Num() + PartGlb.MaterialNames.Num(), 14);
	TestEqual(TEXT("and all 28 triangles"), BodyTriangles + PartTriangles, 28);
	TSet<FString> Names;
	for (const FString& Name : BodyGlb.MaterialNames)
	{
		Names.Add(Name);
	}
	for (const FString& Name : PartGlb.MaterialNames)
	{
		Names.Add(Name);
	}
	TestEqual(TEXT("each material is in one of them"), Names.Num(), 14);

	// Cleanup: the throwaway assets.
	if (Source->Body.IsValid())
	{
		if (USkeleton* Skeleton = BodyMesh->GetSkeleton())
		{
			Skeleton->ClearFlags(RF_Public | RF_Standalone);
			Skeleton->MarkAsGarbage();
		}
		BodyMesh->ClearFlags(RF_Public | RF_Standalone);
		BodyMesh->MarkAsGarbage();
	}
	const_cast<UStaticMesh*>(PartMesh)->ClearFlags(RF_Public | RF_Standalone);
	const_cast<UStaticMesh*>(PartMesh)->MarkAsGarbage();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
