// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAvatarContract.h"
#include "MantleCosmeticExport.h"
#include "MantleCosmeticPackage.h"
#include "MantleItemSettings.h"
#include "MantleMirrorBake.h"

#include "Dom/JsonObject.h"
#include "Engine/StaticMesh.h"
#include "Math/ScaleMatrix.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshOperations.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS

// #65, #136, #389: creator-set item settings are plain top-level item fields, written only on the slots the contract allows
// them on, and an earring Pair is a left item plus a mirrored right one sharing a pair id. Pure but for the mirror test, which
// builds one tiny transient mesh.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleItemSettingsTestPrivate
{
	/** The smallest package MakeItemJson accepts for Slot: a garment for the clothing slots, an accessory for the rest. */
	FMantleCosmeticPackage MakePackage(const FString& Slot, const bool bGarment)
	{
		FMantleCosmeticPackage Package;
		Package.Class = bGarment ? EMantleCosmeticClass::Garment : EMantleCosmeticClass::Accessory;
		Package.Slot = Slot;
		Package.Name = TEXT("Thing");
		Package.Description = TEXT("A thing.");
		Package.Visibility = TEXT("private");
		Package.ThumbnailKey = TEXT("thumbnail");
		Package.MeshKey = TEXT("mesh");
		if (bGarment)
		{
			Package.SourceShapeKey = TEXT("shape");
		}
		FMantleCosmeticMaterialRef& Ref = Package.Materials.AddDefaulted_GetRef();
		Ref.AlbedoKey = TEXT("albedo");
		return Package;
	}

	TMap<FString, FString> MakeIds()
	{
		TMap<FString, FString> Ids;
		int32 Next = 1;
		for (const TCHAR* Key : { TEXT("thumbnail"), TEXT("mesh"), TEXT("shape"), TEXT("albedo") })
		{
			Ids.Add(Key, FString::Printf(TEXT("ast_u%032d"), Next++));
		}
		return Ids;
	}

	TSharedPtr<FJsonObject> ReadJson(const FString& Json)
	{
		TSharedPtr<FJsonObject> Root;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root);
		return Root;
	}

	/** The item JSON of Package (or the patch), null with the error added to Test when it is refused. */
	TSharedPtr<FJsonObject> ItemOf(FAutomationTestBase& Test, const FMantleCosmeticPackage& Package, const bool bPatch, FString* OutError = nullptr)
	{
		FString Json, Error;
		const bool bMade = bPatch ? Package.MakeItemPatchJson(MakeIds(), Json, Error) : Package.MakeItemJson(MakeIds(), Json, Error);
		if (OutError)
		{
			*OutError = Error;
		}
		if (!bMade)
		{
			return nullptr;
		}
		return ReadJson(Json);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleItemSettingsJsonTest,
	"Mantle.Cosmetic.ItemSettings.JsonWritesOnlyAllowedSettings",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleItemSettingsJsonTest::RunTest(const FString& Parameters)
{
	using namespace MantleItemSettingsTestPrivate;
	if (!TestNotNull(TEXT("the contract is readable"), FMantleAvatarContract::Get()))
	{
		return false;
	}

	FMantleCosmeticPackage Top = MakePackage(TEXT("top"), true);
	Top.bTuckable = false;
	// The values a draft carries for other slots must never reach this one.
	Top.bTallBoots = true;
	Top.HairRule = TEXT("hideAll");
	Top.PairId = TEXT("some-pair");
	TSharedPtr<FJsonObject> Item = ItemOf(*this, Top, false);
	if (TestNotNull(TEXT("a top makes its item"), Item.Get()))
	{
		bool bValue = true;
		TestTrue(TEXT("a top writes tuckable"), Item->TryGetBoolField(TEXT("tuckable"), bValue));
		TestFalse(TEXT("explicitly, at its value"), bValue);
		TestFalse(TEXT("a top writes no tallBoots"), Item->HasField(TEXT("tallBoots")));
		TestFalse(TEXT("a top writes no hairRule"), Item->HasField(TEXT("hairRule")));
		TestFalse(TEXT("a top writes no pairId"), Item->HasField(TEXT("pairId")));
	}

	Top.bTuckable = true;
	Item = ItemOf(*this, Top, false);
	if (TestNotNull(TEXT("a top at the default makes its item"), Item.Get()))
	{
		bool bValue = false;
		TestTrue(TEXT("the default is written too"), Item->TryGetBoolField(TEXT("tuckable"), bValue) && bValue);
	}

	FMantleCosmeticPackage Hat = MakePackage(TEXT("acc_hat"), false);
	Hat.HairRule = TEXT("hideAll");
	Item = ItemOf(*this, Hat, false);
	if (TestNotNull(TEXT("a hat makes its item"), Item.Get()))
	{
		TestEqual(TEXT("a hat writes hairRule"), Item->GetStringField(TEXT("hairRule")), FString(TEXT("hideAll")));
		TestFalse(TEXT("a hat writes no tuckable"), Item->HasField(TEXT("tuckable")));
		TestFalse(TEXT("a hat writes no pairId"), Item->HasField(TEXT("pairId")));
	}

	FMantleCosmeticPackage Boots = MakePackage(TEXT("footwear"), true);
	Boots.bTallBoots = true;
	Item = ItemOf(*this, Boots, false);
	if (TestNotNull(TEXT("boots make their item"), Item.Get()))
	{
		bool bValue = false;
		TestTrue(TEXT("footwear writes tallBoots"), Item->TryGetBoolField(TEXT("tallBoots"), bValue) && bValue);
		TestFalse(TEXT("footwear writes no tuckable"), Item->HasField(TEXT("tuckable")));
		TestFalse(TEXT("footwear writes no hairRule"), Item->HasField(TEXT("hairRule")));
	}

	FMantleCosmeticPackage Earring = MakePackage(TEXT("acc_ear_r"), false);
	Earring.PairId = TEXT("hoops-1a2b3c4d");
	Item = ItemOf(*this, Earring, false);
	if (TestNotNull(TEXT("an earring makes its item"), Item.Get()))
	{
		TestEqual(TEXT("acc_ear_r writes pairId"), Item->GetStringField(TEXT("pairId")), FString(TEXT("hoops-1a2b3c4d")));
		TestFalse(TEXT("an earring writes no hairRule"), Item->HasField(TEXT("hairRule")));
	}
	Earring.PairId.Reset();
	Item = ItemOf(*this, Earring, false);
	if (TestNotNull(TEXT("a single earring makes its item"), Item.Get()))
	{
		TestFalse(TEXT("a single earring has no pair, so no pairId"), Item->HasField(TEXT("pairId")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleItemSettingsInvalidTest,
	"Mantle.Cosmetic.ItemSettings.InvalidValuesFailWithAMessage",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleItemSettingsInvalidTest::RunTest(const FString& Parameters)
{
	using namespace MantleItemSettingsTestPrivate;
	if (!TestNotNull(TEXT("the contract is readable"), FMantleAvatarContract::Get()))
	{
		return false;
	}

	FMantleCosmeticPackage Earring = MakePackage(TEXT("acc_ear_l"), false);
	Earring.PairId = TEXT("a b");
	FString Error;
	TestNull(TEXT("a pair id with a space is refused"), ItemOf(*this, Earring, false, &Error).Get());
	TestTrue(TEXT("and says why"), Error.Contains(TEXT("pair id")));

	FMantleCosmeticPackage Hat = MakePackage(TEXT("acc_hat"), false);
	Hat.HairRule = TEXT("sometimes");
	TestNull(TEXT("a hair rule outside the contract's values is refused"), ItemOf(*this, Hat, false, &Error).Get());
	TestTrue(TEXT("and says why"), Error.Contains(TEXT("sometimes")));

	// A bad value on a slot that does not take the setting is not sent, so it is not checked either.
	FMantleCosmeticPackage Top = MakePackage(TEXT("top"), true);
	Top.HairRule = TEXT("sometimes");
	Top.PairId = TEXT("a b");
	TestNotNull(TEXT("a top ignores settings that are not its own"), ItemOf(*this, Top, false).Get());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleItemSettingsPatchTest,
	"Mantle.Cosmetic.ItemSettings.PatchCarriesAllowedSettings",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleItemSettingsPatchTest::RunTest(const FString& Parameters)
{
	using namespace MantleItemSettingsTestPrivate;
	if (!TestNotNull(TEXT("the contract is readable"), FMantleAvatarContract::Get()))
	{
		return false;
	}

	FMantleCosmeticPackage Top = MakePackage(TEXT("top"), true);
	Top.TargetItemId = TEXT("itm_1234567890abcdef");
	Top.bTuckable = false;
	TSharedPtr<FJsonObject> Patch = ItemOf(*this, Top, true);
	if (TestNotNull(TEXT("a top's patch is made"), Patch.Get()))
	{
		bool bValue = true;
		TestTrue(TEXT("it carries tuckable"), Patch->TryGetBoolField(TEXT("tuckable"), bValue));
		TestFalse(TEXT("at the new value"), bValue);
		TestFalse(TEXT("and no other slot's setting"), Patch->HasField(TEXT("hairRule")) || Patch->HasField(TEXT("pairId")) || Patch->HasField(TEXT("tallBoots")));
	}

	FMantleCosmeticPackage Earring = MakePackage(TEXT("acc_ear_l"), false);
	Earring.TargetItemId = TEXT("itm_1234567890abcdef");
	Earring.PairId = TEXT("hoops-1a2b3c4d");
	Patch = ItemOf(*this, Earring, true);
	if (TestNotNull(TEXT("an earring's patch is made"), Patch.Get()))
	{
		TestEqual(TEXT("it carries pairId"), Patch->GetStringField(TEXT("pairId")), FString(TEXT("hoops-1a2b3c4d")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleItemSettingsPairHelpersTest,
	"Mantle.Cosmetic.ItemSettings.PairHelpers",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleItemSettingsPairHelpersTest::RunTest(const FString& Parameters)
{
	const FGuid Guid(0x1a2b3c4d, 0x11111111, 0x22222222, 0x33333333);
	const FString Id = FMantleItemSettings::MakePairId(TEXT("Silver Hoops & Studs!"), Guid);
	TestTrue(TEXT("the pair id matches the pattern"), FMantleItemSettings::IsValidPairId(Id));
	TestEqual(TEXT("it is the name made safe, then the guid's start"), Id, FString(TEXT("silver-hoops---studs--1a2b3c4d")));
	TestEqual(TEXT("the same name and guid give the same id"), Id, FMantleItemSettings::MakePairId(TEXT("Silver Hoops & Studs!"), Guid));
	TestNotEqual(TEXT("another guid gives another id"), Id, FMantleItemSettings::MakePairId(TEXT("Silver Hoops & Studs!"), FGuid::NewGuid()));

	const FString Long = FMantleItemSettings::MakePairId(FString::ChrN(200, TEXT('x')), Guid);
	TestTrue(TEXT("a long name still makes a valid id"), FMantleItemSettings::IsValidPairId(Long));
	TestTrue(TEXT("within 64 characters"), Long.Len() <= 64);
	TestTrue(TEXT("an empty name still makes a valid id"), FMantleItemSettings::IsValidPairId(FMantleItemSettings::MakePairId(FString(), Guid)));

	TestTrue(TEXT("letters, digits, dash and underscore are valid"), FMantleItemSettings::IsValidPairId(TEXT("Ab_9-z")));
	TestFalse(TEXT("a space is not"), FMantleItemSettings::IsValidPairId(TEXT("a b")));
	TestFalse(TEXT("empty is not"), FMantleItemSettings::IsValidPairId(FString()));
	TestFalse(TEXT("65 characters are not"), FMantleItemSettings::IsValidPairId(FString::ChrN(65, TEXT('a'))));
	TestTrue(TEXT("64 are"), FMantleItemSettings::IsValidPairId(FString::ChrN(64, TEXT('a'))));

	TestEqual(TEXT("the left item's name"), FMantleItemSettings::WithSideSuffix(TEXT("Hoops"), false), FString(TEXT("Hoops (left)")));
	TestEqual(TEXT("the right item's name"), FMantleItemSettings::WithSideSuffix(TEXT("Hoops"), true), FString(TEXT("Hoops (right)")));
	TestEqual(TEXT("a suffix is not added twice"), FMantleItemSettings::WithSideSuffix(TEXT("Hoops (left)"), false), FString(TEXT("Hoops (left)")));
	TestEqual(TEXT("the other side's suffix is replaced"), FMantleItemSettings::WithSideSuffix(TEXT("Hoops (left)"), true), FString(TEXT("Hoops (right)")));
	TestEqual(TEXT("no name stays empty"), FMantleItemSettings::WithSideSuffix(FString(), true), FString());

	// The attach offset mirrored: the left-right component of the location flips, the rest stays.
	FVector3f Location, Rotation, Scale;
	FMantleItemSettings::MirrorAttach(FVector3f(2.0f, 3.0f, 4.0f), FVector3f::ZeroVector, FVector3f(1.0f, 1.5f, 2.0f), Location, Rotation, Scale);
	TestTrue(TEXT("the location's left-right part flips"), Location.Equals(FVector3f(-2.0f, 3.0f, 4.0f), 0.001f));
	TestTrue(TEXT("no rotation stays none"), Rotation.Equals(FVector3f::ZeroVector, 0.001f));
	TestTrue(TEXT("the scale is kept"), Scale.Equals(FVector3f(1.0f, 1.5f, 2.0f), 0.001f));

	// A yaw turns the other way when mirrored on X; pitch and roll flip only through the mirror of their axes.
	FMantleItemSettings::MirrorAttach(FVector3f::ZeroVector, FVector3f(0.0f, 30.0f, 0.0f), FVector3f::OneVector, Location, Rotation, Scale);
	TestTrue(TEXT("a yaw about the up axis reverses"), FMath::IsNearlyEqual(Rotation.Y, -30.0f, 0.01f));

	// Mirroring twice gives the original back.
	FVector3f BackLocation, BackRotation, BackScale;
	const FVector3f StartLocation(5.0f, -1.0f, 2.0f), StartRotation(10.0f, 20.0f, 30.0f);
	FMantleItemSettings::MirrorAttach(StartLocation, StartRotation, FVector3f::OneVector, Location, Rotation, Scale);
	FMantleItemSettings::MirrorAttach(Location, Rotation, Scale, BackLocation, BackRotation, BackScale);
	TestTrue(TEXT("the mirror of the mirror is the original location"), BackLocation.Equals(StartLocation, 0.01f));
	TestTrue(TEXT("and the original rotation"), BackRotation.Equals(StartRotation, 0.01f));

	// Worn: with the right ear's socket the left one mirrored across the body (the socket step's convention: acc_ear_r =
	// X flip * acc_ear_l * X flip, so its frame is a proper rotation), the right earring (the mirrored mesh on the mirrored
	// offset) is the mirror image of the left one wherever the socket points, not only for an unturned socket.
	const FMatrix Flip = FScaleMatrix(FVector(-1.0, 1.0, 1.0));
	const FMatrix SocketLeft = FTransform(FRotator(15.0, -40.0, 70.0), FVector(8.0, 3.0, 160.0)).ToMatrixWithScale();
	const FMatrix SocketRight = Flip * SocketLeft * Flip;
	const FMatrix AttachLeft = FTransform(FRotator(StartRotation.X, StartRotation.Y, StartRotation.Z), FVector(StartLocation)).ToMatrixWithScale();
	FMantleItemSettings::MirrorAttach(StartLocation, StartRotation, FVector3f::OneVector, Location, Rotation, Scale);
	const FMatrix AttachRight = FTransform(FRotator(Rotation.X, Rotation.Y, Rotation.Z), FVector(Location), FVector(Scale)).ToMatrixWithScale();
	for (const FVector& Point : { FVector(1.0, 0.0, 0.0), FVector(0.0, 2.0, -1.0), FVector(0.5, -0.25, 3.0) })
	{
		const FVector WornLeft = AttachLeft.TransformPosition(Point);
		const FVector Left = SocketLeft.TransformPosition(WornLeft);
		const FVector MirroredPoint = Point * FVector(-1.0, 1.0, 1.0);
		const FVector Right = SocketRight.TransformPosition(AttachRight.TransformPosition(MirroredPoint));
		TestTrue(FString::Printf(TEXT("the right earring's %s is the left one's mirrored across the body"), *Point.ToString()),
			Right.Equals(Left * FVector(-1.0, 1.0, 1.0), 0.01));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleItemSettingsMirrorMeshTest,
	"Mantle.Cosmetic.ItemSettings.RightEarringMeshIsOnTheOtherSide",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleItemSettingsMirrorMeshTest::RunTest(const FString& Parameters)
{
	FMantleMirrorBake::ClearCache();
	TStrongObjectPtr<UStaticMesh> Cube(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
	if (!TestTrue(TEXT("the engine cube loads"), Cube.IsValid() && Cube->GetRenderData() && Cube->GetRenderData()->LODResources.Num() > 0))
	{
		return false;
	}

	// The cube moved to the left (X +100, the mesh's left-right axis): an earring that is not symmetric about its origin.
	FMeshDescription Part;
	Cube->ExportStaticMeshLOD(Cube->GetRenderData()->LODResources[0], Part);
	FStaticMeshOperations::ApplyTransform(Part, FTransform(FVector(100.0, 20.0, 30.0)));
	UObject* Outer = GetTransientPackage();
	TStrongObjectPtr<UStaticMesh> Left(NewObject<UStaticMesh>(Outer, MakeUniqueObjectName(Outer, UStaticMesh::StaticClass(), FName(TEXT("ItemSettingsTestEarring"))), RF_Transient));
	Left->GetStaticMaterials() = Cube->GetStaticMaterials();
	Left->bAllowCPUAccess = true;
	Left->SetNumSourceModels(1);
	FMeshBuildSettings& Settings = Left->GetSourceModel(0).BuildSettings;
	Settings.bRecomputeNormals = false;
	Settings.bRecomputeTangents = false;
	Settings.bGenerateLightmapUVs = false;
	Settings.bBuildReversedIndexBuffer = false;
	Settings.DistanceFieldResolutionScale = 0.0f;
	Settings.MaxLumenMeshCards = 0;
	UStaticMesh::FBuildMeshDescriptionsParams Params;
	Params.bMarkPackageDirty = false;
	Params.bAllowCpuAccess = true;
	TArray<const FMeshDescription*> Descriptions;
	Descriptions.Add(&Part);
	Left->BuildFromMeshDescriptions(Descriptions, Params);
	const FBox LeftBounds = Left->GetBoundingBox();

	FMantleCosmeticDraft Draft;
	Draft.Class = EMantleCosmeticClass::Accessory;
	Draft.Slot = TEXT("acc_ear_l");
	Draft.Name = TEXT("Hoops");
	Draft.Source = Left.Get();
	Draft.bAttach = true;
	Draft.AttachLocation = FVector3f(1.0f, 2.0f, 3.0f);
	Draft.PairId = TEXT("hoops-1a2b3c4d");
	Draft.EarringSide = FMantleCosmeticDraft::EEarringSide::Pair;

	FMantleCosmeticDraft Right;
	FString Why;
	if (TestTrue(TEXT("the right earring's draft is made"), FMantleItemSettings::MakeRightDraft(Draft, Right, Why)))
	{
		TestEqual(TEXT("in the right ear's slot"), Right.Slot, FString(TEXT("acc_ear_r")));
		TestEqual(TEXT("named for its side"), Right.Name, FString(TEXT("Hoops (right)")));
		TestEqual(TEXT("with the pair's id"), Right.PairId, Draft.PairId);
		TestTrue(TEXT("its offset is mirrored"), FMath::IsNearlyEqual(Right.AttachLocation.X, -1.0f, 0.001f) && FMath::IsNearlyEqual(Right.AttachLocation.Y, 2.0f, 0.001f));
		UStaticMesh* Mirrored = Cast<UStaticMesh>(Right.Source.Get());
		if (TestNotNull(TEXT("its mesh is a copy"), Mirrored) && TestTrue(TEXT("that is not the left mesh"), Mirrored != Left.Get()))
		{
			const FBox RightBounds = Mirrored->GetBoundingBox();
			TestTrue(TEXT("its bounds are on the other side of the left-right axis"),
				FMath::IsNearlyEqual(RightBounds.Min.X, -LeftBounds.Max.X, 0.1) && FMath::IsNearlyEqual(RightBounds.Max.X, -LeftBounds.Min.X, 0.1));
			TestTrue(TEXT("and unchanged forward and up"),
				FMath::IsNearlyEqual(RightBounds.Min.Y, LeftBounds.Min.Y, 0.1) && FMath::IsNearlyEqual(RightBounds.Max.Z, LeftBounds.Max.Z, 0.1));
		}
	}
	else
	{
		AddError(Why);
	}

	// Anything but a static mesh cannot be mirrored.
	FMantleCosmeticDraft NoMesh = Draft;
	NoMesh.Source.Reset();
	TestFalse(TEXT("a draft with no static mesh makes no right earring"), FMantleItemSettings::MakeRightDraft(NoMesh, Right, Why));
	FMantleMirrorBake::ClearCache();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
