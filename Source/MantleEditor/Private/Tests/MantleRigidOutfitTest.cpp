// Copyright Joshua Eger. All Rights Reserved.
#include "MantleParametricOutfit.h"
#include "MantleAvatarContract.h"
#include "MantleCosmeticValidation.h"
#include "MantleCosmeticThumbnail.h"

#include "Engine/StaticMesh.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// An Outfit made into a rigid accessory hangs from its slot's socket bone: a hat from the head (fitted to the reference
// head, as before), a waist piece from the pelvis. Maths only, on the plugin's contract.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleRigidOutfitSocketBoneTest,
	"Mantle.Editor.ParametricOutfit.RigidHangsFromSocketBone",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleRigidOutfitSocketBoneTest::RunTest(const FString& Parameters)
{
	FString ContractError;
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get(&ContractError);
	if (!TestNotNull(FString::Printf(TEXT("the contract loads (%s)"), *ContractError), Contract)) { return false; }

	FMantleParametricOutfit::FRigidPivot Hat, Waist, Face, Top;
	FString Error;
	TestTrue(TEXT("acc_hat has a pivot"), FMantleParametricOutfit::RigidPivot(*Contract, TEXT("acc_hat"), Hat, Error));
	TestEqual(TEXT("a hat hangs from the head"), Hat.Bone, FString(TEXT("head")));
	TestTrue(TEXT("a hat is fitted to the reference head"), Hat.bHeadFit);
	TestTrue(TEXT("acc_waist has a pivot"), FMantleParametricOutfit::RigidPivot(*Contract, TEXT("acc_waist"), Waist, Error));
	TestEqual(TEXT("a waist piece hangs from the pelvis"), Waist.Bone, FString(TEXT("pelvis")));
	TestFalse(TEXT("a waist piece keeps its size"), Waist.bHeadFit);
	TestTrue(TEXT("acc_face has a pivot"), FMantleParametricOutfit::RigidPivot(*Contract, TEXT("acc_face"), Face, Error));
	TestFalse(TEXT("only the hat socket is fitted per avatar"), Face.bHeadFit);
	TestFalse(TEXT("a clothing slot is not an accessory"), FMantleParametricOutfit::RigidPivot(*Contract, TEXT("top"), Top, Error));

	// A MetaHuman-like bind pose: the pelvis 95 cm up, the head 165 cm up, each with its own rotation.
	const FTransform Pelvis(FRotator(0.0, -90.0, 90.0).Quaternion(), FVector(0.0, 1.5, 95.0));
	const FTransform Head(FRotator(-5.0, 90.0, -88.0).Quaternion(), FVector(0.0, -2.0, 165.0));

	// The hat route is unchanged: in the head's frame, then the reference-head fit undone.
	FMantleHeadFit::FFit Fit;
	Fit.Scale = FVector3f(0.9f, 1.1f, 0.95f);
	Fit.Offset = FVector3f(1.0f, -0.5f, 0.25f);
	const FVector3f Crown(2.0f, -1.0f, 180.0f), Up(0.0f, 0.0f, 1.0f);
	const FVector3f WasPosition = (FVector3f(Head.InverseTransformPositionNoScale(FVector(Crown))) - Fit.Offset) / Fit.Scale;
	const FVector3f WasNormal = (FVector3f(Head.InverseTransformVectorNoScale(FVector(Up))) * Fit.Scale).GetSafeNormal();
	TestTrue(TEXT("a hat vertex lands where it did"), FMantleParametricOutfit::RigidPosition(Crown, Head, Fit).Equals(WasPosition, 1e-4f));
	TestTrue(TEXT("a hat normal points where it did"), FMantleParametricOutfit::RigidNormal(Up, Head, Fit).Equals(WasNormal, 1e-5f));

	// A shirt tied round the hips: a ring 18 cm out, 82 to 100 cm up, with a flap hanging to 70 cm at the back.
	TArray<FVector3f> Shirt;
	for (int32 Step = 0; Step < 24; ++Step)
	{
		const float Angle = 2.0f * UE_PI * Step / 24.0f;
		for (const float Height : { 82.0f, 100.0f })
		{
			Shirt.Add(FVector3f(18.0f * FMath::Cos(Angle), 18.0f * FMath::Sin(Angle), Height));
		}
	}
	Shirt.Add(FVector3f(-10.0f, 16.0f, 70.0f));
	Shirt.Add(FVector3f(10.0f, 16.0f, 70.0f));
	auto Placed = [&Shirt](const FTransform& Pose)
	{
		FBox3f Box(ForceInit);
		for (const FVector3f& Point : Shirt) { Box += FMantleParametricOutfit::RigidPosition(Point, Pose, FMantleHeadFit::FFit()); }
		return Box;
	};
	TArray<FMantleCosmeticIssue> FromPelvis, FromHead;
	FMantleCosmeticChecks::CheckAccessoryBounds(TEXT("acc_waist"), Placed(Pelvis), *Contract, FromPelvis);
	FMantleCosmeticChecks::CheckAccessoryBounds(TEXT("acc_waist"), Placed(Head), *Contract, FromHead);
	TestEqual(FString::Printf(TEXT("from the pelvis it is within acc_waist's reach (%s)"), FromPelvis.IsEmpty() ? TEXT("") : *FromPelvis[0].ToString()),
		FromPelvis.Num(), 0);
	TestTrue(TEXT("from the head (the old route) it is out of reach"), FromHead.Num() > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleRigidOutfitBackFacesTest,
	"Mantle.Editor.ParametricOutfit.ThumbnailBackFaces",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

// Cloth made rigid shows its inside: the thumbnail draws a copy facing the other way (winding reversed, normals turned round).
bool FMantleRigidOutfitBackFacesTest::RunTest(const FString& Parameters)
{
	FMeshDescription Description;
	FStaticMeshAttributes Attributes(Description);
	Attributes.Register();
	const FPolygonGroupID Group = Description.CreatePolygonGroup();
	Attributes.GetPolygonGroupMaterialSlotNames()[Group] = TEXT("Cloth");
	TArray<FVertexInstanceID> Corners;
	for (const FVector3f& Position : { FVector3f(0, 0, 0), FVector3f(10, 0, 0), FVector3f(0, 10, 0) })
	{
		const FVertexID Vertex = Description.CreateVertex();
		Attributes.GetVertexPositions()[Vertex] = Position;
		Corners.Add(Description.CreateVertexInstance(Vertex));
		Attributes.GetVertexInstanceNormals()[Corners.Last()] = FVector3f(0, 0, 1);
	}
	Description.CreateTriangle(Group, Corners);
	UStaticMesh* Mesh = NewObject<UStaticMesh>(GetTransientPackage(), NAME_None, RF_Transient);
	Mesh->GetStaticMaterials().Add(FStaticMaterial(nullptr, TEXT("Cloth"), TEXT("Cloth")));
	Mesh->SetNumSourceModels(1);
	Mesh->GetSourceModel(0).BuildSettings.bRecomputeNormals = false;
	UStaticMesh::FBuildMeshDescriptionsParams Params;
	Params.bMarkPackageDirty = false;
	if (!TestTrue(TEXT("the triangle builds"), Mesh->BuildFromMeshDescriptions({ &Description }, Params))) { return false; }

	UStaticMesh* Back = FMantleCosmeticThumbnail::BackFaces(Mesh);
	if (!TestNotNull(TEXT("the back faces build"), Back)) { return false; }
	const FMeshDescription* Out = Back->GetMeshDescription(0);
	if (!TestNotNull(TEXT("they keep a mesh description"), Out) || !TestEqual(TEXT("one triangle"), Out->Triangles().Num(), 1)) { return false; }
	FStaticMeshConstAttributes OutAttributes(*Out);
	const FTriangleID Triangle = *Out->Triangles().GetElementIDs().CreateConstIterator();
	TArray<FVector3f> Positions;
	for (const FVertexInstanceID Instance : Out->GetTriangleVertexInstances(Triangle))
	{
		Positions.Add(OutAttributes.GetVertexPositions()[Out->GetVertexInstanceVertex(Instance)]);
		TestTrue(TEXT("each normal is turned round"), OutAttributes.GetVertexInstanceNormals()[Instance].Equals(FVector3f(0, 0, -1), 1e-5f));
	}
	const FVector3f Facing = FVector3f::CrossProduct(Positions[1] - Positions[0], Positions[2] - Positions[0]);
	const FVector3f Was = FVector3f::CrossProduct(FVector3f(10, 0, 0), FVector3f(0, 10, 0));
	TestTrue(TEXT("the winding is reversed"), FVector3f::DotProduct(Facing, Was) < 0.0f);
	TestEqual(TEXT("the material slots are kept"), Back->GetStaticMaterials().Num(), 1);
	return true;
}

#endif
