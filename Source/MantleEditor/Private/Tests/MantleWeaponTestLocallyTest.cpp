// Copyright Joshua Eger. All Rights Reserved.
#include "MantleClientFeatures.h"
#include "MantlePreviewExport.h"
#include "MantleTargetFeatures.h"
#include "MantleTestLocally.h"
#include "MantleWeaponBuilderLogic.h"
#include "MantleWeaponSource.h"

#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// Test locally from the weapon builder: the manifest's "weapons" key, the client feature that gates it, which level the
// weapon button uses, and when the button is offered.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWeaponManifestKeyTest,
	"Mantle.Editor.WeaponTestLocally.ManifestWeaponsKey",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWeaponManifestKeyTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("no weapons is an empty array"), FMantlePreviewExport::WeaponsToJson(TArray<FMantlePreviewBundleWeapon>()), FString(TEXT("[]")));

	const FMantleWeaponToTry Weapon = FMantleWeaponToTry::FromBuilderName(TEXT("My_Rifle"));
	FMantlePreviewBundleWeapon Entry;
	Entry.File = Weapon.BundleFile();
	Entry.Assets.Add((TEXT("SKM_") + Weapon.Name), TEXT("ast_u0123456789abcdef"));
	Entry.Materials = { TEXT("ast_um1"), FString(), TEXT("ast_um2") };
	Entry.bEquip = Weapon.bEquip;
	TArray<FMantlePreviewBundleWeapon> Weapons;
	Weapons.Add(Entry);
	TestEqual(TEXT("exactly the contract's shape"), FMantlePreviewExport::WeaponsToJson(Weapons),
		FString(TEXT("[{\"file\":\"weapons/My_Rifle.weapon.json\",\"assets\":{\"SKM_My_Rifle\":\"ast_u0123456789abcdef\"},\"materials\":[\"ast_um1\",\"\",\"ast_um2\"],\"equip\":true}]")));

	// Several models in one file are written in name order, whatever order they were added in.
	FMantlePreviewBundleWeapon Two;
	Two.File = TEXT("weapons/B.weapon.json");
	Two.Assets.Add(TEXT("SKM_Z"), TEXT("ast_uz"));
	Two.Assets.Add(TEXT("SKM_A"), TEXT("ast_ua"));
	Two.bEquip = false;
	Weapons.Add(Two);
	TestEqual(TEXT("two weapons, sorted assets, equip false"), FMantlePreviewExport::WeaponsToJson(Weapons),
		FString(TEXT("[{\"file\":\"weapons/My_Rifle.weapon.json\",\"assets\":{\"SKM_My_Rifle\":\"ast_u0123456789abcdef\"},\"materials\":[\"ast_um1\",\"\",\"ast_um2\"],\"equip\":true},")
			TEXT("{\"file\":\"weapons/B.weapon.json\",\"assets\":{\"SKM_A\":\"ast_ua\",\"SKM_Z\":\"ast_uz\"},\"materials\":[],\"equip\":false}]")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWeaponBundlePathsTest,
	"Mantle.Editor.WeaponTestLocally.BuilderFolderConvention",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWeaponBundlePathsTest::RunTest(const FString& Parameters)
{
	const FMantleWeaponToTry Weapon = FMantleWeaponToTry::FromBuilderName(TEXT("My_Rifle"));
	TestEqual(TEXT("the model is the builder's SKM_<Name>"), Weapon.MeshObjectPath, FString(TEXT("/Game/MantleWeapons/My_Rifle/SKM_My_Rifle.SKM_My_Rifle")));
	TestEqual(TEXT("its local name in the settings file"), (TEXT("SKM_") + Weapon.Name), FString(TEXT("SKM_My_Rifle")));
	TestEqual(TEXT("the file's place in the bundle"), Weapon.BundleFile(), FString(TEXT("weapons/My_Rifle.weapon.json")));
	TestTrue(TEXT("the settings file is read from the weapon's folder"), Weapon.DocumentFile.EndsWith(TEXT("MantleWeapons/My_Rifle/My_Rifle.weapon.json")));
	TestTrue(TEXT("a weapon is equipped by default"), Weapon.bEquip);

	TestEqual(TEXT("the model's name comes from the settings file"),
		FMantlePreviewExport::ReadWeaponMeshName(TEXT("{\"schemaVersion\":1,\"name\":\"My Rifle\",\"mesh\":\"SKM_My_Rifle\"}")), FString(TEXT("SKM_My_Rifle")));
	TestTrue(TEXT("a file that names none gives none"), FMantlePreviewExport::ReadWeaponMeshName(TEXT("{\"name\":\"x\"}")).IsEmpty());
	TestTrue(TEXT("a file that is not JSON gives none"), FMantlePreviewExport::ReadWeaponMeshName(TEXT("not json")).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWeaponFeatureGateTest,
	"Mantle.Editor.WeaponTestLocally.NeedsTheWeaponDocumentFeature",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWeaponFeatureGateTest::RunTest(const FString& Parameters)
{
	TestFalse(TEXT("an old client that lists no features cannot"), FMantlePreviewExport::ClientReadsWeapons(FMantleTargetFeatures::FromNames(TArray<FString>())));
	TestFalse(TEXT("a client with other features only cannot"),
		FMantlePreviewExport::ClientReadsWeapons(FMantleTargetFeatures::FromNames({ FString(MantleClientFeatures::SkeletalMesh), FString(MantleClientFeatures::Vehicle) })));
	TestTrue(TEXT("a client that lists weaponDocument can"),
		FMantlePreviewExport::ClientReadsWeapons(FMantleTargetFeatures::FromNames({ FString(MantleClientFeatures::SkeletalMesh), FString(MantleClientFeatures::WeaponDocument) })));
	TestEqual(TEXT("the feature's wire name"), FString(MantleClientFeatures::WeaponDocument), FString(TEXT("weaponDocument")));
	TestTrue(TEXT("a dev export (every feature this build reads) can"), FMantlePreviewExport::ClientReadsWeapons(FMantleTargetFeatures::All()));

	const FString Message = FMantlePreviewExport::WeaponsNeedNewerClientMessage();
	TestTrue(TEXT("the refusal says what to do"), Message.Contains(TEXT("Update Mantle")));
	TestTrue(TEXT("and has no Unreal words"), !Message.Contains(TEXT("manifest")) && !Message.Contains(TEXT("feature")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWeaponTestLevelChoiceTest,
	"Mantle.Editor.WeaponTestLocally.TestRangeLevelChoice",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWeaponTestLevelChoiceTest::RunTest(const FString& Parameters)
{
	using ELevel = FMantleTestLocally::EWeaponTestLevel;
	// bSaved, bUnsavedChanges, bRangeExists.
	TestTrue(TEXT("a saved level is used"), FMantleTestLocally::ChooseWeaponTestLevel(true, false, false) == ELevel::UseOpenLevel);
	TestTrue(TEXT("a saved level with unsaved edits is still used, never switched away from"), FMantleTestLocally::ChooseWeaponTestLevel(true, true, true) == ELevel::UseOpenLevel);
	TestTrue(TEXT("a saved level wins over an existing range"), FMantleTestLocally::ChooseWeaponTestLevel(true, false, true) == ELevel::UseOpenLevel);
	TestTrue(TEXT("an unsaved level with changes asks first"), FMantleTestLocally::ChooseWeaponTestLevel(false, true, false) == ELevel::AskToSave);
	TestTrue(TEXT("and asks even when the range exists"), FMantleTestLocally::ChooseWeaponTestLevel(false, true, true) == ELevel::AskToSave);
	TestTrue(TEXT("an untouched unsaved level makes the range once"), FMantleTestLocally::ChooseWeaponTestLevel(false, false, false) == ELevel::CreateTestRange);
	TestTrue(TEXT("and opens it after that"), FMantleTestLocally::ChooseWeaponTestLevel(false, false, true) == ELevel::OpenTestRange);

	TestEqual(TEXT("the range sits beside the weapons"), FMantleTestLocally::WeaponTestRangePackage(), FString(TEXT("/Game/MantleWeapons/WeaponTestRange")));
	const FString Ask = FMantleTestLocally::WeaponTestRangeAskToSaveMessage();
	TestTrue(TEXT("the request says how to save"), Ask.Contains(TEXT("Save")));
	TestTrue(TEXT("and that nothing changed"), Ask.Contains(TEXT("Nothing was changed")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWeaponTestButtonTest,
	"Mantle.Editor.WeaponTestLocally.ButtonNeedsACurrentCreate",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWeaponTestButtonTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponBuilderLogic;
	UMantleWeaponSource* Source = NewObject<UMantleWeaponSource>(GetTransientPackage());
	Source->DisplayName = TEXT("My Rifle");
	Source->Tuning = DefaultTuning(Source->Hold);

	TestFalse(TEXT("not offered before Create"), CanTestLocally(FString(), EditSignature(*Source)));

	const FString AtCreate = EditSignature(*Source);
	TestTrue(TEXT("offered right after Create"), CanTestLocally(AtCreate, EditSignature(*Source)));

	Source->Tuning.Damage += 1.f;
	TestFalse(TEXT("not after a number changes"), CanTestLocally(AtCreate, EditSignature(*Source)));
	Source->Tuning.Damage -= 1.f;
	TestTrue(TEXT("offered again when the change is undone"), CanTestLocally(AtCreate, EditSignature(*Source)));

	Source->DisplayName = TEXT("My Rifle 2");
	TestFalse(TEXT("not after a rename"), CanTestLocally(AtCreate, EditSignature(*Source)));
	Source->DisplayName = TEXT("My Rifle");

	FMantleBuilderPoint Point;
	Point.Role = TEXT("Muzzle");
	Point.Transform = FTransform(FVector(10.0, 0.0, 0.0));
	Source->Points.Add(Point);
	TestFalse(TEXT("not after a point is placed"), CanTestLocally(AtCreate, EditSignature(*Source)));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
