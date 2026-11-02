// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAssetCleanup.h"
#include "MantleSkinnedMesh.h"
#include "MantleVehicleAsset.h"
#include "MantleVehicleBlueprint.h"
#include "MantleVehicleBodyJoin.h"
#include "MantleVehicleBodySplit.h"
#include "MantleVehicleBuilder.h"
#include "MantleVehicleChassisBuilder.h"
#include "MantleVehicleConverter.h"
#include "MantleVehicleLightFinder.h"
#include "MantleVehicleScratchBody.h"
#include "MantleVehicleSource.h"

#include "Engine/Blueprint.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Materials/Material.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "ReferenceSkeleton.h"

#if WITH_DEV_AUTOMATION_TESTS

// The static pieces a vehicle Blueprint attaches to a bare skeletal body are joined into the body (FMantleVehicleBodyJoin), so the builder's picture, paint and lights see the
// whole car. The arithmetic and the split of pieces are tested with meshes made here; the pack's cars are tested where the pack is (the MantleVehicles scratch project), and skip
// with a warning anywhere else.

namespace MantleVehicleBodyJoinTestPrivate
{
	const TCHAR* const LightBlueprint = TEXT("/Game/Military_Vehicles_Pack/Light_Military_Vehicle/Light_Military_Vehicle.Light_Military_Vehicle");
	const TCHAR* const OffRoadBlueprint = TEXT("/Game/Military_Vehicles_Pack/OffRoad_Military_Vehicle/OffRoad_Military_Vehicle.OffRoad_Military_Vehicle");
	const TCHAR* const ArmoredBlueprint = TEXT("/Game/Military_Vehicles_Pack/Armored_Vehicle/BP_Armored_Military_Vehicle.BP_Armored_Military_Vehicle");
	const TCHAR* const TruckBlueprint = TEXT("/Game/Military_Vehicles_Pack/Military_Truck/Blueprints/Military_Truck.Military_Truck");
	const TCHAR* const LightBodyMesh = TEXT("/Game/Military_Vehicles_Pack/Light_Military_Vehicle/Mesh/SK_Base_Bones.SK_Base_Bones");

	/** A flat square of Size cm in the XY plane (normal +Z) with one material slot, as a static piece reads. */
	FMantleVehicleBodyJoin::FPiece Square(UMaterialInterface* Material, const float Size, const bool bTangents)
	{
		FMantleVehicleBodyJoin::FPiece Piece;
		FMantleSkinnedMeshLOD& LOD = Piece.LOD;
		const float Half = 0.5f * Size;
		const FVector3f Corners[4] = { FVector3f(-Half, -Half, 0), FVector3f(Half, -Half, 0), FVector3f(Half, Half, 0), FVector3f(-Half, Half, 0) };
		for (const FVector3f& Corner : Corners)
		{
			LOD.Positions.Add(Corner);
			LOD.Normals.Add(FVector3f(0, 0, 1));
			LOD.UVs.Add(FVector2f(Corner.X > 0 ? 1.f : 0.f, Corner.Y > 0 ? 1.f : 0.f));
			if (bTangents)
			{
				LOD.Tangents.Add(FVector4f(1, 0, 0, 1));
			}
		}
		for (const uint32 Index : { 0u, 1u, 2u, 0u, 2u, 3u })
		{
			LOD.Indices.Add(Index);
		}
		FMantleSkinnedMeshSection& Section = LOD.Sections.AddDefaulted_GetRef();
		Section.MaterialIndex = 0;
		Section.FirstIndex = 0;
		Section.IndexCount = 6;
		Section.MaterialName = FName(TEXT("Square"));
		LOD.NumInfluences = 1;
		Piece.Materials.Add(FSkeletalMaterial(Material, FName(TEXT("Square")), FName(TEXT("Square"))));
		return Piece;
	}

	/** Which way a piece's first triangle faces against its first vertex normal: positive when the winding and the normal agree. */
	double Facing(const FMantleSkinnedMeshLOD& LOD, const int32 FirstIndex)
	{
		const FVector3f A = LOD.Positions[LOD.Indices[FirstIndex]];
		const FVector3f B = LOD.Positions[LOD.Indices[FirstIndex + 1]];
		const FVector3f C = LOD.Positions[LOD.Indices[FirstIndex + 2]];
		return FVector3f::DotProduct(FVector3f::CrossProduct(B - A, C - A), LOD.Normals[LOD.Indices[FirstIndex]]);
	}

	/** The body of the sedan chassis as plain data, with a material slot for each of its sections' slots. */
	bool ChassisBody(FAutomationTestBase& Test, FMantleSkinnedMesh& OutBody, TArray<FSkeletalMaterial>& OutMaterials)
	{
		FMantleChassisSpec Spec;
		FMantleVehicleAsset Asset;
		FString Error;
		if (!FMantleVehicleChassisBuilder::LoadSpec(TEXT("sedan"), Spec, Error) || !FMantleVehicleChassisBuilder::MakeAsset(Spec, Asset, Error))
		{
			Test.AddError(FString::Printf(TEXT("the sedan chassis could not be made: %s"), *Error));
			return false;
		}
		OutBody = Asset.Body;
		int32 Slots = 1;
		for (const FMantleSkinnedMeshSection& Section : OutBody.LODs[0].Sections)
		{
			Slots = FMath::Max(Slots, Section.MaterialIndex + 1);
		}
		OutMaterials.Reset();
		for (int32 Slot = 0; Slot < Slots; ++Slot)
		{
			OutMaterials.Add(FSkeletalMaterial(UMaterial::GetDefaultMaterial(MD_Surface), FName(*FString::Printf(TEXT("Gray_%d"), Slot)), FName(*FString::Printf(TEXT("Gray_%d"), Slot))));
		}
		return true;
	}

	UBlueprint* Load(FAutomationTestBase& Test, const TCHAR* Path)
	{
		if (!FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(FString(Path))))
		{
			Test.AddWarning(FString::Printf(TEXT("SKIPPED, not passed: %s is not in this project."), Path));
			return nullptr;
		}
		return LoadObject<UBlueprint>(nullptr, Path);
	}

	/** A box static mesh of Size in a throwaway package. */
	UStaticMesh* Box(const FVector3f& Size, const TCHAR* Name)
	{
		FMantleVehicleBodyPiece Piece;
		const FVector3f Half = 0.5f * Size;
		const FVector3f Corners[8] = {
			FVector3f(-Half.X, -Half.Y, -Half.Z), FVector3f(Half.X, -Half.Y, -Half.Z), FVector3f(Half.X, Half.Y, -Half.Z), FVector3f(-Half.X, Half.Y, -Half.Z),
			FVector3f(-Half.X, -Half.Y, Half.Z), FVector3f(Half.X, -Half.Y, Half.Z), FVector3f(Half.X, Half.Y, Half.Z), FVector3f(-Half.X, Half.Y, Half.Z) };
		for (const FVector3f& Corner : Corners)
		{
			Piece.Positions.Add(Corner);
			Piece.Normals.Add(Corner.GetSafeNormal());
			Piece.UVs.Add(FVector2f(0, 0));
		}
		for (const uint32 Index : { 0u, 2u, 1u, 0u, 3u, 2u, 4u, 5u, 6u, 4u, 6u, 7u, 0u, 1u, 5u, 0u, 5u, 4u, 2u, 3u, 7u, 2u, 7u, 6u, 1u, 2u, 6u, 1u, 6u, 5u, 3u, 0u, 4u, 3u, 4u, 7u })
		{
			Piece.Indices.Add(Index);
		}
		FMantleVehicleBodyPiece::FSection& Section = Piece.Sections.AddDefaulted_GetRef();
		Section.FirstIndex = 0;
		Section.IndexCount = Piece.Indices.Num();
		Section.Material = 0;
		Piece.Materials.Add(nullptr);
		Piece.SlotNames.Add(FName(TEXT("Box")));
		Piece.BoneIndex = 0;
		FString Error;
		return FMantleVehicleBodySplit::MakePieceMesh(Piece, TEXT("/Temp/MantleVehicleBodyJoinTest"), Name, Error);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleBodyJoinArithmeticTest,
	"Mantle.Vehicle.BodyJoin.PiecesRideTheirBonesAndShareMaterials",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleBodyJoinArithmeticTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleBodyJoinTestPrivate;
	FMantleSkinnedMesh Body;
	TArray<FSkeletalMaterial> Materials;
	if (!ChassisBody(*this, Body, Materials))
	{
		return false;
	}
	UMaterial* Gray = UMaterial::GetDefaultMaterial(MD_Surface);
	UMaterial* Other = UMaterial::GetDefaultMaterial(MD_DeferredDecal);
	if (!TestTrue(TEXT("the test has two different materials"), Gray && Other && Gray != Other) || !TestTrue(TEXT("and a body with a wheel bone"), Body.Bones.Num() > 1))
	{
		return false;
	}
	const int32 Vertices = Body.LODs[0].NumVertices();
	const int32 Triangles = Body.LODs[0].NumTriangles();
	const int32 Slots = Materials.Num();
	const int32 Influences = Body.LODs[0].NumInfluences;
	const int32 WheelBone = Body.Bones.Num() - 1;

	// A piece on the root with the body's own material, one on a wheel's bone with another, and one placed with a mirrored scale.
	TArray<FMantleVehicleBodyJoin::FPiece> Pieces;
	Pieces.Add(Square(Gray, 100.f, true));
	Pieces.Last().Bone = 0;
	Pieces.Last().Placement = FTransform(FRotator(0, 90, 0), FVector(10, 20, 30));
	Pieces.Add(Square(Other, 40.f, true));
	Pieces.Last().Bone = WheelBone;
	Pieces.Last().Placement = FTransform(FVector(0, 0, 5));
	Pieces.Add(Square(Gray, 20.f, true));
	Pieces.Last().Bone = 0;
	Pieces.Last().Placement = FTransform(FQuat::Identity, FVector(0, 0, 50), FVector(-1, 1, 1));

	FMantleSkinnedMesh Joined = Body;
	TArray<FSkeletalMaterial> JoinedMaterials = Materials;
	FString Error;
	if (!TestTrue(*FString::Printf(TEXT("the pieces are joined (%s)"), *Error), FMantleVehicleBodyJoin::Join(Joined, JoinedMaterials, Pieces, Error)))
	{
		return false;
	}
	const FMantleSkinnedMeshLOD& LOD = Joined.LODs[0];
	TestEqual(TEXT("each piece's vertices are added"), LOD.NumVertices(), Vertices + 12);
	TestEqual(TEXT("and its triangles"), LOD.NumTriangles(), Triangles + 6);
	TestEqual(TEXT("a material the body draws is shared, another gets a slot of its own"), JoinedMaterials.Num(), Slots + 1);
	TestEqual(TEXT("the new slot is the other material"), JoinedMaterials.Last().MaterialInterface.Get(), static_cast<UMaterialInterface*>(Other));
	TestEqual(TEXT("with a name of its own"), JoinedMaterials.Last().MaterialSlotName, FName(TEXT("Square")));
	TSet<int32> SeenSlots;
	bool bUnique = true;
	for (const FMantleSkinnedMeshSection& Section : LOD.Sections)
	{
		bool bAlready = false;
		SeenSlots.Add(Section.MaterialIndex, &bAlready);
		bUnique = bUnique && !bAlready;
	}
	TestTrue(TEXT("every slot is one section"), bUnique);

	// The wheel's piece (its vertices are the 4 after the root's 4) is weighted whole to the wheel bone.
	const int32 WheelPiece = Vertices + 4;
	TestEqual(TEXT("a piece on a wheel is weighted to that bone"), static_cast<int32>(LOD.InfluenceBones[WheelPiece * Influences]), WheelBone);
	TestEqual(TEXT("with all the weight"), static_cast<int32>(LOD.InfluenceWeights[WheelPiece * Influences]), 255);
	int32 Sum = 0;
	for (int32 Slot = 0; Slot < Influences; ++Slot)
	{
		Sum += LOD.InfluenceWeights[WheelPiece * Influences + Slot];
	}
	TestEqual(TEXT("a vertex's weights sum to 255"), Sum, 255);
	TestEqual(TEXT("a piece on the root is weighted to the root"), static_cast<int32>(LOD.InfluenceBones[Vertices * Influences]), 0);

	// Placed: the root piece is turned 90 degrees about Z and moved, its first corner (-50,-50,0) lands at (10 + 50, 20 - 50, 30).
	const FVector3f Landed = LOD.Positions[Vertices];
	TestTrue(*FString::Printf(TEXT("a piece sits where its placement puts it (%s)"), *Landed.ToString()), Landed.Equals(FVector3f(60.f, -30.f, 30.f), 0.01f));
	TestTrue(TEXT("and its normal is turned with it"), LOD.Normals[Vertices].Equals(FVector3f(0, 0, 1), 0.001f));

	// The mirrored piece keeps its faces outward: winding against its normal agrees with the piece placed without a mirror.
	TArray<double> Faces;
	for (int32 Index = 0; Index + 2 < LOD.Indices.Num(); Index += 3)
	{
		if (LOD.Indices[Index] >= static_cast<uint32>(Vertices) && LOD.Indices[Index] != static_cast<uint32>(WheelPiece))
		{
			Faces.Add(Facing(LOD, Index));
		}
	}
	TestTrue(TEXT("the pieces' triangles are in the joined body"), Faces.Num() == 4);
	if (Faces.Num() == 4)
	{
		bool bAgree = true;
		for (const double Face : Faces)
		{
			bAgree = bAgree && ((Face > 0.0) == (Faces[0] > 0.0));
		}
		TestTrue(TEXT("a mirrored piece's faces still face the way its unmirrored twin's do"), bAgree);
	}

	// Pieces that all have tangents keep them; one without makes the build work them all out from the UVs.
	TestTrue(TEXT("tangents are kept when the body and every piece has them"), Body.LODs[0].Tangents.Num() == 0 || LOD.Tangents.Num() == LOD.NumVertices());
	TArray<FMantleVehicleBodyJoin::FPiece> NoTangents;
	NoTangents.Add(Square(Gray, 10.f, false));
	FMantleSkinnedMesh Again = Body;
	TArray<FSkeletalMaterial> AgainMaterials = Materials;
	TestTrue(TEXT("a piece with no tangents joins"), FMantleVehicleBodyJoin::Join(Again, AgainMaterials, NoTangents, Error));
	TestEqual(TEXT("and the body's tangents are left to the build"), Again.LODs[0].Tangents.Num(), 0);

	// A piece on a bone the body does not have is refused, and the body is as it was.
	TArray<FMantleVehicleBodyJoin::FPiece> Lost;
	Lost.Add(Square(Gray, 10.f, true));
	Lost.Last().Bone = 9999;
	FMantleSkinnedMesh Refused = Body;
	TArray<FSkeletalMaterial> RefusedMaterials = Materials;
	TestFalse(TEXT("a piece on a bone the body lacks is refused"), FMantleVehicleBodyJoin::Join(Refused, RefusedMaterials, Lost, Error));
	TestFalse(TEXT("with a reason"), Error.IsEmpty());
	TestEqual(TEXT("and the body is unchanged"), Refused.LODs[0].NumVertices(), Vertices);
	TestEqual(TEXT("with its materials"), RefusedMaterials.Num(), Slots);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleBodyJoinSplitTest,
	"Mantle.Vehicle.BodyJoin.WheelPiecesStayAttached",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleBodyJoinSplitTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleBodyJoinTestPrivate;
	FString Error;
	USkeletalMesh* Car = MantleVehicleScratchBody::Make(TEXT("/Temp/MantleVehicleBodyJoinTest"), TEXT("SplitCar"), Error);
	if (!TestNotNull(*FString::Printf(TEXT("the scratch body is made (%s)"), *Error), Car))
	{
		return false;
	}
	const TArray<FMantleVehicleWheelBone> Wheels = FMantleVehicleConverter::FindWheelBones(Car->GetRefSkeleton());
	if (!TestTrue(TEXT("it has wheels"), Wheels.Num() >= 2))
	{
		MantleVehicleScratchBody::Discard(Car);
		return false;
	}
	const FReferenceSkeleton& Skeleton = Car->GetRefSkeleton();
	const int32 WheelIndex = Skeleton.FindRawBoneIndex(Wheels[0].Bone);
	FTransform Hub = FTransform::Identity;
	for (int32 Bone = WheelIndex; Bone != INDEX_NONE; Bone = Skeleton.GetRawParentIndex(Bone))
	{
		Hub = Hub * Skeleton.GetRawRefBonePose()[Bone];
	}

	UStaticMesh* Shell = Box(FVector3f(300.f, 160.f, 90.f), TEXT("SM_Shell"));
	UStaticMesh* Door = Box(FVector3f(8.f, 90.f, 100.f), TEXT("SM_Door"));
	UStaticMesh* Disk = Box(FVector3f(70.f, 20.f, 70.f), TEXT("SM_Disk"));
	UStaticMesh* Caliper = Box(FVector3f(20.f, 10.f, 25.f), TEXT("SM_Caliper"));
	UStaticMesh* Stray = Box(FVector3f(30.f, 30.f, 30.f), TEXT("SM_Stray"));
	if (!TestTrue(TEXT("the test meshes are made"), Shell && Door && Disk && Caliper && Stray))
	{
		MantleVehicleScratchBody::Discard(Car);
		return false;
	}

	FMantleVehicleSurroundings Around;
	auto Add = [&Around](const UStaticMesh* Mesh, const FName AttachTo, const FTransform& Relative, const TCHAR* Label)
	{
		FMantleVehicleAttachedMesh& Found = Around.Attached.AddDefaulted_GetRef();
		Found.Mesh = Mesh;
		Found.AttachTo = AttachTo;
		Found.Relative = Relative;
		Found.Label = Label;
	};
	Add(Shell, NAME_None, FTransform(FVector(0, 0, 60)), TEXT("Shell"));
	Add(Door, NAME_None, FTransform(FVector(40, 85, 70)), TEXT("Door"));
	Add(Disk, NAME_None, FTransform(Hub.GetLocation()), TEXT("Disk"));
	Add(Caliper, Wheels[0].Bone, FTransform::Identity, TEXT("Caliper"));
	Add(Stray, FName(TEXT("no_such_bone")), FTransform::Identity, TEXT("Stray"));

	TArray<FMantleVehiclePlacedMesh> BodyPieces;
	TArray<FMantleVehicleAttachedMesh> Rest;
	FMantleVehicleConverter::SplitBodyPieces(*Car, Around, BodyPieces, Rest);

	TArray<FString> Joined;
	for (const FMantleVehiclePlacedMesh& Piece : BodyPieces)
	{
		Joined.Add(Piece.Found.Label);
		TestEqual(*FString::Printf(TEXT("%s rides the root"), *Piece.Found.Label), Piece.Bone, 0);
	}
	TArray<FString> Kept;
	for (const FMantleVehicleAttachedMesh& Found : Rest)
	{
		Kept.Add(Found.Label);
	}
	TestTrue(TEXT("the shell and the door are the body's"), Joined.Contains(TEXT("Shell")) && Joined.Contains(TEXT("Door")) && Joined.Num() == 2);
	TestTrue(TEXT("a tire-shaped piece at a wheel stays for the wheel"), Kept.Contains(TEXT("Disk")));
	TestTrue(TEXT("a piece on a wheel's bone stays for the wheel"), Kept.Contains(TEXT("Caliper")));
	TestTrue(TEXT("a piece on a bone the body lacks stays, for the converter to say so"), Kept.Contains(TEXT("Stray")));
	TestEqual(TEXT("every piece is one or the other"), BodyPieces.Num() + Rest.Num(), Around.Attached.Num());
	TestTrue(TEXT("the shell's placement is where it was attached"), BodyPieces.Num() > 0 && BodyPieces[0].Placement.GetLocation().Equals(FVector(0, 0, 60), 0.01));

	// Joined into the body, they are in the model: more triangles, the same bones.
	int32 Triangles = 0;
	USkeletalMesh* Model = FMantleVehicleBodyJoin::Make(*Car, BodyPieces, TEXT("/Temp/MantleVehicleBodyJoinTest"), TEXT("Joined"), Error, &Triangles);
	if (TestNotNull(*FString::Printf(TEXT("the joined model is made (%s)"), *Error), Model))
	{
		FMantleSkinnedMesh Before;
		FMantleSkinnedMesh After;
		TestTrue(TEXT("the body is read"), FMantleSkinnedMesh::ExtractFromSkeletalMesh(Car, Before, &Error) && FMantleSkinnedMesh::ExtractFromSkeletalMesh(Model, After, &Error));
		if (Before.LODs.Num() > 0 && After.LODs.Num() > 0)
		{
			TestEqual(TEXT("the joined model has the pieces' triangles added"), After.LODs[0].NumTriangles(), Before.LODs[0].NumTriangles() + 24);
			TestEqual(TEXT("and the count Make reported"), Triangles, After.LODs[0].NumTriangles());
			TestEqual(TEXT("with the body's bones"), After.Bones.Num(), Before.Bones.Num());
			TestTrue(TEXT("the pieces' material is a slot of the model"), Model->GetMaterials().Num() >= 1);
		}
		MantleVehicleScratchBody::Discard(Model);
	}
	MantleVehicleScratchBody::Discard(Car);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleBodyJoinLightMilitaryTest,
	"Mantle.Vehicle.BodyJoin.LightMilitaryVehicleHasItsWholeBodyAndLights",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleBodyJoinLightMilitaryTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleBodyJoinTestPrivate;
	UBlueprint* Blueprint = Load(*this, LightBlueprint);
	USkeletalMesh* Bones = Blueprint ? LoadObject<USkeletalMesh>(nullptr, LightBodyMesh) : nullptr;
	if (!Blueprint || !Bones)
	{
		return true;
	}

	// The skeleton alone has next to nothing to see or light.
	FMantleSkinnedMesh Bare;
	TArray<FSkeletalMaterial> BareMaterials;
	FString Error;
	TestTrue(*FString::Printf(TEXT("the bare body is read (%s)"), *Error), FMantleSkinnedMesh::ExtractFromSkeletalMesh(Bones, Bare, &Error, &BareMaterials));
	const FMantleVehicleLightGuess BareGuess = FMantleVehicleLights::Find(Bare, BareMaterials, false);

	FMantleVehicleBlueprintModel Read = FMantleVehicleBlueprint::Resolve(Blueprint, FString(), /*bSave*/ false);
	USkeletalMesh* Model = Cast<USkeletalMesh>(Read.Model);
	for (const FString& Problem : Read.Problems)
	{
		AddInfo(TEXT("Problem: ") + Problem);
	}
	for (const FString& Note : Read.Notes)
	{
		AddInfo(TEXT("Note: ") + Note);
	}
	if (!TestNotNull(TEXT("the Blueprint gives a skeletal model"), Model) || !TestTrue(TEXT("that is not the bare body"), Model != Bones))
	{
		return false;
	}
	TestTrue(*FString::Printf(TEXT("the body shell, doors, tailgate and cargo are joined (%d)"), Read.PiecesJoined), Read.PiecesJoined >= 6);
	TestTrue(TEXT("the Blueprint still carries its wheel setups"), Read.Around.IsSet() && Read.Around->WheelSetups.Num() >= 2);
	if (Read.Around.IsSet())
	{
		const bool bBodyStillAPart = Read.Around->Attached.ContainsByPredicate([](const FMantleVehicleAttachedMesh& Found) { return Found.Label == TEXT("Body") || Found.Label == TEXT("Door_L"); });
		TestFalse(TEXT("and no joined piece rides along as a part as well"), bBodyStillAPart);
		const bool bHidden = Read.Around->Attached.ContainsByPredicate([](const FMantleVehicleAttachedMesh& Found) { return Found.Label == TEXT("Tent_Frame"); });
		TestFalse(TEXT("nor the tent frame the car keeps switched off"), bHidden);
	}

	FMantleSkinnedMesh Whole;
	TArray<FSkeletalMaterial> WholeMaterials;
	if (TestTrue(*FString::Printf(TEXT("the joined body is read (%s)"), *Error), FMantleSkinnedMesh::ExtractFromSkeletalMesh(Model, Whole, &Error, &WholeMaterials)))
	{
		TestTrue(*FString::Printf(TEXT("it has far more to see than the skeleton (%d against %d triangles)"), Whole.LODs[0].NumTriangles(), Bare.LODs[0].NumTriangles()),
			Whole.LODs[0].NumTriangles() > Bare.LODs[0].NumTriangles() + 20000);
		TestTrue(TEXT("with no material of its own an instance made while the Blueprint ran"), !WholeMaterials.ContainsByPredicate([](const FSkeletalMaterial& Slot)
			{ return Slot.MaterialInterface && Slot.MaterialInterface->GetOutermost() == GetTransientPackage(); }));
		const FMantleVehicleLightGuess Guess = FMantleVehicleLights::Find(Whole, WholeMaterials, false);
		for (int32 Slot = 0; Slot < WholeMaterials.Num(); ++Slot)
		{
			AddInfo(FString::Printf(TEXT("slot %d %s: %s"), Slot, *WholeMaterials[Slot].MaterialSlotName.ToString(), WholeMaterials[Slot].MaterialInterface ? *WholeMaterials[Slot].MaterialInterface->GetName() : TEXT("none")));
		}
		TestFalse(TEXT("the bare body finds no light"), BareGuess.FoundAny());
		TestTrue(TEXT("the joined body finds its lights"), Guess.FoundAny());
		int32 Kinds = 0;
		for (int32 Kind = 0; Kind < FMantleVehicleLights::NumKinds; ++Kind)
		{
			Kinds += (Guess.Triangles[Kind].Num() > 0 || Guess.WholeSlots[Kind].Num() > 0) ? 1 : 0;
			AddInfo(FString::Printf(TEXT("light kind %s: %d triangles, %d whole slots"), FMantleVehicleLights::Key(static_cast<EMantleLightKind>(Kind)), Guess.Triangles[Kind].Num(), Guess.WholeSlots[Kind].Num()));
		}
		TestTrue(*FString::Printf(TEXT("at least the head and rear lights (%d kinds)"), Kinds), Kinds >= 2);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleBodyJoinOtherPackCarsTest,
	"Mantle.Vehicle.BodyJoin.CarsWithNothingAttachedAreUnchanged",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleBodyJoinOtherPackCarsTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleBodyJoinTestPrivate;
	struct FCase
	{
		const TCHAR* Blueprint;
		const TCHAR* Mesh;
	};
	const FCase Cases[] = {
		{ OffRoadBlueprint, TEXT("ORMV") },
		{ ArmoredBlueprint, TEXT("AMV_mesh") },
	};
	for (const FCase& Case : Cases)
	{
		UBlueprint* Blueprint = Load(*this, Case.Blueprint);
		if (!Blueprint)
		{
			continue;
		}
		const FMantleVehicleBlueprintModel Read = FMantleVehicleBlueprint::Resolve(Blueprint, FString(), /*bSave*/ false);
		const USkeletalMesh* Model = Cast<USkeletalMesh>(Read.Model);
		TestTrue(*FString::Printf(TEXT("%s gives its own skeletal mesh"), Case.Mesh), Model && Model->GetName() == Case.Mesh);
		TestEqual(*FString::Printf(TEXT("%s joins nothing"), Case.Mesh), Read.PiecesJoined, 0);
		TestEqual(*FString::Printf(TEXT("%s makes no assets"), Case.Mesh), Read.MadeAssets.Num(), 0);
		TestTrue(*FString::Printf(TEXT("%s has nothing attached"), Case.Mesh), Read.Around.IsSet() && Read.Around->Attached.Num() == 0);
	}

	// The truck's Blueprint attaches its cargo bed; its mesh alone, as the pack's truck is picked, is not touched.
	if (UBlueprint* Truck = Load(*this, TruckBlueprint))
	{
		const FMantleVehicleBlueprintModel Read = FMantleVehicleBlueprint::Resolve(Truck, FString(), /*bSave*/ false);
		TestTrue(TEXT("the truck's Blueprint gives a skeletal model"), Cast<USkeletalMesh>(Read.Model) != nullptr);
		if (Read.Around.IsSet())
		{
			TestFalse(TEXT("and does not carry the tent frame it keeps switched off"), Read.Around->Attached.ContainsByPredicate([](const FMantleVehicleAttachedMesh& Found) { return Found.Label == TEXT("Tent_Frame"); }));
		}
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
