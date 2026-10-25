// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAssetCleanup.h"
#include "MantleBuilderKind.h"
#include "MantleWeaponBuilderKind.h"
#include "MantleWeaponBuilderLogic.h"
#include "MantleWeaponSource.h"
#include "SMantleWeaponBuilder.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/StaticMesh.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/GarbageCollection.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MantleWeaponBuilderPanelTestPrivate
{
	/** The corners of a box, long along X: enough of a "gun" for the fit to give guesses. */
	static TArray<FVector3f> BoxCorners(float Length, float Height, float Depth)
	{
		TArray<FVector3f> Out;
		for (int32 Corner = 0; Corner < 8; ++Corner)
		{
			Out.Add(FVector3f((Corner & 1) ? Length : 0.f, (Corner & 2) ? Depth : 0.f, (Corner & 4) ? Height : 0.f));
		}
		return Out;
	}

	struct FSourceFixtureCleanup
	{
		FString PackagePath;
		FString Folder;
		FString AssetName;

		void SetDisplayName(const FString& DisplayName)
		{
			using namespace MantleWeaponBuilderLogic;
			PackagePath = SourcePackagePath(DisplayName);
			AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
			SetFolderName(SafeAssetName(DisplayName));
		}

		void SetPackagePath(const FString& InPackagePath, const FString& FolderName)
		{
			PackagePath = InPackagePath;
			AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
			SetFolderName(FolderName);
		}

		void SetFolderName(const FString& FolderName)
		{
			const FString WeaponsRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / TEXT("MantleWeapons"));
			const FString Candidate = FPaths::ConvertRelativePathToFull(WeaponsRoot / FolderName);
			if (Candidate.StartsWith(WeaponsRoot + TEXT("/"), ESearchCase::IgnoreCase)
				&& FPaths::GetCleanFilename(Candidate).Equals(FolderName, ESearchCase::IgnoreCase))
			{
				Folder = Candidate;
			}
		}

		~FSourceFixtureCleanup()
		{
			UPackage* Package = PackagePath.IsEmpty() ? nullptr : FindPackage(nullptr, *PackagePath);
			if (Package)
			{
				if (UMantleWeaponSource* Asset = FindObject<UMantleWeaponSource>(Package, *AssetName))
				{
					FAssetRegistryModule::AssetDeleted(Asset);
				}
				FText Error;
				FMantleAssetCleanup::UnloadPackagesClean({ Package }, Error);
			}
			if (!Folder.IsEmpty())
			{
				IFileManager::Get().DeleteDirectory(*Folder, false, true);
			}
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleWeaponBuilderPanelDefaultsTest, "Mantle.Editor.WeaponBuilderPanel.HoldDefaults", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleWeaponBuilderPanelDefaultsTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponBuilderLogic;
	const FMantleWeaponTuning Rifle = DefaultTuning(EMantleWeaponHold::Rifle);
	TestEqual(TEXT("rifle damage"), Rifle.Damage, 12.f);
	TestEqual(TEXT("rifle shots per second"), Rifle.ShotsPerSecond, 12.5f);
	TestEqual(TEXT("rifle magazine"), Rifle.MagazineSize, 30);
	TestEqual(TEXT("rifle spare ammo"), Rifle.SpareAmmo, 90);
	TestEqual(TEXT("rifle reload"), Rifle.ReloadSeconds, 1.7f);
	TestTrue(TEXT("rifle is automatic"), Rifle.bAutomatic);

	const FMantleWeaponTuning Pistol = DefaultTuning(EMantleWeaponHold::Pistol);
	TestEqual(TEXT("pistol damage"), Pistol.Damage, 18.f);
	TestEqual(TEXT("pistol shots per second"), Pistol.ShotsPerSecond, 6.f);
	TestEqual(TEXT("pistol magazine"), Pistol.MagazineSize, 15);
	TestEqual(TEXT("pistol spare ammo"), Pistol.SpareAmmo, 45);
	TestEqual(TEXT("pistol reload"), Pistol.ReloadSeconds, 1.4f);
	TestFalse(TEXT("pistol is semi-automatic"), Pistol.bAutomatic);

	// Every starting value sits inside its own range, for every hold.
	for (uint8 H = 0; H < static_cast<uint8>(EMantleWeaponHold::Count); ++H)
	{
		const FMantleWeaponTuning Defaults = DefaultTuning(static_cast<EMantleWeaponHold>(H));
		const FMantleWeaponTuning Clamped = ClampTuning(Defaults);
		TestEqual(*FString::Printf(TEXT("hold %d defaults need no clamping (damage)"), H), Defaults.Damage, Clamped.Damage);
		TestEqual(*FString::Printf(TEXT("hold %d defaults need no clamping (rate)"), H), Defaults.ShotsPerSecond, Clamped.ShotsPerSecond);
		TestEqual(*FString::Printf(TEXT("hold %d defaults need no clamping (magazine)"), H), Defaults.MagazineSize, Clamped.MagazineSize);
		TestEqual(*FString::Printf(TEXT("hold %d defaults need no clamping (spare)"), H), Defaults.SpareAmmo, Clamped.SpareAmmo);
		TestEqual(*FString::Printf(TEXT("hold %d defaults need no clamping (reload)"), H), Defaults.ReloadSeconds, Clamped.ReloadSeconds);
	}

	TestEqual(TEXT("default name"), DefaultName(EMantleWeaponHold::Rifle), FString(TEXT("My Rifle")));
	for (uint8 HoldIndex = 0; HoldIndex < static_cast<uint8>(EMantleWeaponHold::Count); ++HoldIndex)
	{
		const EMantleWeaponHold Hold = static_cast<EMantleWeaponHold>(HoldIndex);
		// Every hold is offered.
		TestTrue(*FString::Printf(TEXT("hold %d availability"), HoldIndex), IsHoldAvailable(Hold));
	}
	TestFalse(TEXT("the Count sentinel is not a hold"), IsHoldAvailable(EMantleWeaponHold::Count));
	TestFalse(TEXT("out-of-range values are not holds"), IsHoldAvailable(static_cast<EMantleWeaponHold>(255)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleWeaponBuilderPanelSourceCopyTest, "Mantle.Editor.WeaponBuilderPanel.CopyHelperCopiesEverySourcePropertyInMemory", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleWeaponBuilderPanelSourceCopyTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponBuilderPanelPrivate;
	UMantleWeaponSource* Source = NewObject<UMantleWeaponSource>(GetTransientPackage());
	UMantleWeaponSource* Destination = NewObject<UMantleWeaponSource>(GetTransientPackage());
	Source->DisplayName = TEXT("Typed source copy");
	Source->Hold = EMantleWeaponHold::Bow;
	Source->Model = FSoftObjectPath(TEXT("/Game/Test/SourceBow.SourceBow"));
	Source->MagazineSections.Add(TSoftObjectPtr<UObject>(FSoftObjectPath(TEXT("/Game/Test/SourceQuiver.SourceQuiver"))));
	Source->FitScale = 1.27f;
	Source->bScaleOverridden = true;
	Source->FlipRotation = FQuat(FVector::UpVector, 0.31f);
	FMantleBuilderPoint Point;
	Point.Role = TEXT("Nock");
	Point.Transform.SetLocation(FVector(3.0, 4.0, 5.0));
	Source->Points.Add(Point);
	Source->Tuning.Damage = 77.f;
	Source->Tuning.ShotsPerSecond = 0.7f;
	Source->Tuning.MagazineSize = 11;
	Source->Tuning.SpareAmmo = 32;
	Source->Tuning.ReloadSeconds = 1.25f;
	Source->Tuning.bAutomatic = false;
	Source->Tuning.MaxRange = 14000.f;
	Source->Tuning.TraceSweepRadius = 2.5f;
	Source->Tuning.Pellets = 4;
	Source->Tuning.PelletSpread = 2.25f;
	Source->Tuning.PlainRecoil = 0.6f;
	Source->Tuning.bBurstMode = true;
	Source->Delivery = EMantleWeaponSourceDelivery::Projectile;
	Source->Resource = EMantleWeaponSourceResource::Quiver;
	Source->bOverrideTwoHanded = true;
	Source->bTwoHanded = true;
	Source->bOverrideStyle = true;
	Source->Style = EMantleWeaponSourceStyle::Ice;
	Source->bOverrideColor = true;
	Source->Color = FColor(14, 160, 220);
	Source->Charge.bEnabled = true;
	Source->Charge.Seconds = 2.5f;
	Source->Charge.bRequired = true;
	Source->Charge.bReleaseOnFull = true;
	Source->Charge.bCancelable = false;
	Source->bBlock = true;
	Source->bBreakOnHeavy = false;
	Source->Projectile.Speed = 2400.f;
	Source->Projectile.Gravity = 0.45f;
	Source->Projectile.BlastRadius = 8.f;
	Source->Projectile.bThrown = true;
	Source->Projectile.bStick = true;
	Source->Projectile.bPickup = true;
	Source->Projectile.Model = FSoftObjectPath(TEXT("/Game/Test/SourceArrow.SourceArrow"));
	Source->Sweep.Reach = 180.f;
	Source->Sweep.ComboCount = 4;
	Source->Sweep.HeavyMultiplier = 2.7f;
	Source->Sweep.EdgeBase = TEXT("EdgeBase");
	Source->Sweep.EdgeTip = TEXT("EdgeTip");
	Source->Beam.TickRate = 15.f;
	Source->Beam.DamagePerSecond = 21.f;
	Source->Beam.MaxSeconds = 4.f;
	Source->Beam.Radius = 6.f;
	Source->Beam.Range = 9000.f;
	Source->BowStringRadiusCm = 2.2f;
	FMantleWeaponSourcePart Part;
	Part.Role = EMantleWeaponSourcePartRole::Arrow;
	Part.Model = FSoftObjectPath(TEXT("/Game/Test/SourcePart.SourcePart"));
	Part.InBody = FTransform(FRotator(12.f, 23.f, 34.f), FVector(1.f, 2.f, 3.f), FVector(1.1f, 1.2f, 1.3f));
	Part.ShownDuring.Add(EMantleWeaponSourceAction::Release);
	Source->Parts.Add(Part);
	FMantleWeaponSourceAttachment Attachment;
	Attachment.Name = TEXT("Bow glow");
	Attachment.Model = FSoftObjectPath(TEXT("/Game/Test/SourceEffect.SourceEffect"));
	Attachment.bEffect = true;
	Attachment.Effect = EMantleWeaponSourceStyle::Arcane;
	Attachment.Socket = TEXT("Focus");
	Attachment.RelativeTransform = FTransform(FRotator(5.f, 15.f, 25.f), FVector(8.f, 9.f, 10.f), FVector(0.7f, 0.8f, 0.9f));
	Attachment.bHidden = true;
	Source->Attachments.Add(Attachment);
	Source->FireSound = FSoftObjectPath(TEXT("/Game/Test/SourceFire.SourceFire"));
	Source->FireSuppressedSound = FSoftObjectPath(TEXT("/Game/Test/SourceQuiet.SourceQuiet"));
	Source->bSuppressed = true;
	Source->FireSoundTrigger = TEXT("PrimaryAttack");

	CopySourceProperties(*Source, *Destination);
	for (TFieldIterator<FProperty> Property(UMantleWeaponSource::StaticClass(), EFieldIteratorFlags::ExcludeSuper); Property; ++Property)
	{
		TestTrue(*FString::Printf(TEXT("in-memory copy helper preserves property %s"), *Property->GetName()),
			Property->Identical_InContainer(Source, Destination));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleWeaponBuilderPanelSaveReopenTest, "Mantle.Editor.WeaponBuilderPanel.CreateSavesAndReopensSourceProperties", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleWeaponBuilderPanelSaveReopenTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponBuilderLogic;
	using namespace MantleWeaponBuilderPanelTestPrivate;

	// A fist weapon (knuckles, claws) is worn on the hand, so changing to that hold keeps the chosen model, and the
	// source is created and marked ready for Test locally like any weapon with a model.
	TSharedRef<SMantleWeaponBuilder> FistsBuilder = SNew(SMantleWeaponBuilder);
	FistsBuilder->SetHold(EMantleWeaponHold::Melee1h);
	FistsBuilder->Working->Model = FSoftObjectPath(TEXT("/Engine/BasicShapes/Cube.Cube"));
	FistsBuilder->SetHold(EMantleWeaponHold::Unarmed);
	TestFalse(TEXT("changing to the fist weapon keeps its model"), FistsBuilder->Working->Model.IsNull());
	FistsBuilder->Working->DisplayName = FString::Printf(TEXT("UI Knuckles %s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	FistsBuilder->bNameEdited = true;
	FistsBuilder->Working->Tuning = DefaultTuning(EMantleWeaponHold::Unarmed);
	FistsBuilder->Working->Tuning.Damage = 31.5f;
	FistsBuilder->SetHold(EMantleWeaponHold::Unarmed);
	TestEqual(TEXT("reselecting the active hold preserves its edited attack settings"), FistsBuilder->Working->Tuning.Damage, 31.5f);
	FistsBuilder->Working->Sweep.Reach = 123.f;
	FistsBuilder->Working->Sweep.ComboCount = 4;
	FSourceFixtureCleanup FistsCleanup;
	FistsCleanup.SetDisplayName(FistsBuilder->Working->DisplayName);
	FistsBuilder->Create();
	TestTrue(TEXT("the fist weapon exports and is ready to test"), !FistsBuilder->CreatedSignature.IsEmpty() && FistsBuilder->CanTestLocally());

	// A separate valid, mesh-backed weapon exercises the actual source package save, unload,
	// reload and wizard reopen path for nested settings that used to be dropped by manual copying.
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("the fixed-detail fixture mesh loads"), Cube)) return false;
	TSharedRef<SMantleWeaponBuilder> Builder = SNew(SMantleWeaponBuilder);
	Builder->SetHold(EMantleWeaponHold::Melee1h);
	Builder->SetModel(Cube);
	Builder->SetFit(1.f, true);
	Builder->Working->Points.Reset();
	auto AddPoint = [&Builder](const TCHAR* Role, const FVector& Location)
	{
		FMantleBuilderPoint& Point = Builder->Working->Points.AddDefaulted_GetRef();
		Point.Role = Role;
		Point.Transform = FTransform(FQuat::Identity, Location);
		Point.Guess = Point.Transform;
		Point.Source = EMantleBuilderPointSource::Creator;
	};
	AddPoint(TEXT("Grip_R"), FVector(0.f, 0.f, 0.f));
	AddPoint(TEXT("EdgeBase"), FVector(30.f, 0.f, 0.f));
	AddPoint(TEXT("EdgeTip"), FVector(80.f, 0.f, 0.f));
	Builder->Working->DisplayName = FString::Printf(TEXT("UI Save Reload %s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	Builder->bNameEdited = true;
	Builder->Working->Tuning = DefaultTuning(EMantleWeaponHold::Melee1h);
	Builder->Working->Tuning.Damage = 31.5f;
	Builder->Working->Sweep.Reach = 123.f;
	Builder->Working->Sweep.ComboCount = 4;
	FMantleWeaponSourceAttachment& Detail = Builder->Working->Attachments.AddDefaulted_GetRef();
	Detail.Name = TEXT("RoundTripDetail");
	Detail.Model = Cube;
	Detail.Socket = TEXT("Grip_R");
	Detail.RelativeTransform = FTransform(FRotator(5.f, 15.f, 25.f), FVector(8.f, 9.f, 10.f), FVector(0.7f, 0.8f, 0.9f));
	Detail.bHidden = true;
	Builder->Working->FireSound = FSoftObjectPath(TEXT("/Engine/EditorSounds/Notifications/CompileSuccess.CompileSuccess"));
	Builder->Working->FireSoundTrigger = TEXT("PrimaryAttack");

	FSourceFixtureCleanup Cleanup;
	Cleanup.SetDisplayName(Builder->Working->DisplayName);
	Builder->Create();
	TestTrue(TEXT("the valid model-based weapon exports and is ready to test"), !Builder->CreatedSignature.IsEmpty() && Builder->CanTestLocally());
	const FString PackagePath = SourcePackagePath(Builder->Working->DisplayName);
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
	UPackage* SavedPackage = FindPackage(nullptr, *PackagePath);
	UMantleWeaponSource* SavedAsset = SavedPackage ? FindObject<UMantleWeaponSource>(SavedPackage, *AssetName) : nullptr;
	if (!TestNotNull(TEXT("Create saved the source asset"), SavedAsset))
	{
		return false;
	}

	FText UnloadError;
	const bool bUnloaded = FMantleAssetCleanup::UnloadPackagesClean({ SavedPackage }, UnloadError);
	TestTrue(*FString::Printf(TEXT("the saved source package unloads before reopen (%s)"), *UnloadError.ToString()), bUnloaded);
	if (!bUnloaded)
	{
		return false;
	}

	UPackage* ReloadedPackage = LoadPackage(nullptr, *PackagePath, LOAD_None);
	UMantleWeaponSource* ReloadedAsset = ReloadedPackage ? FindObject<UMantleWeaponSource>(ReloadedPackage, *AssetName) : nullptr;
	if (!TestNotNull(TEXT("the source asset reopens from its saved package"), ReloadedAsset))
	{
		return false;
	}

	// A fresh wizard instance uses the same route as the first-step asset picker.
	TSharedRef<SMantleWeaponBuilder> ReopenedBuilder = SNew(SMantleWeaponBuilder);
	ReopenedBuilder->LoadSourceAsset(ReloadedAsset);
	TestEqual(TEXT("opening the saved source restores its original hold"), ReopenedBuilder->Working->Hold, ReloadedAsset->Hold);
	TestEqual(TEXT("opening the saved source restores the existing asset identity"), ReopenedBuilder->CreatedSafeName,
		SafeAssetName(ReloadedAsset->DisplayName));
	TestTrue(TEXT("the reopened wizard keeps the saved source path"), ReopenedBuilder->SourceAssetPath == FSoftObjectPath(ReloadedAsset));
	TestTrue(TEXT("the loaded source name remains marked as edited in the wizard"), ReopenedBuilder->bNameEdited);
	TestTrue(TEXT("opening a saved source requires Create before Test locally is ready"), !ReopenedBuilder->CanTestLocally());
	for (TFieldIterator<FProperty> Property(UMantleWeaponSource::StaticClass(), EFieldIteratorFlags::ExcludeSuper); Property; ++Property)
	{
		TestTrue(*FString::Printf(TEXT("the wizard reopens saved property %s without resetting it"), *Property->GetName()),
			Property->Identical_InContainer(ReopenedBuilder->Working.Get(), ReloadedAsset));
	}
	TestEqual(TEXT("the reopened fixed detail keeps its model"), ReloadedAsset->Attachments.Num(), 1);
	if (ReloadedAsset->Attachments.Num() == 1)
	{
		TestEqual(TEXT("the detail mesh path survives save and reload"), ReloadedAsset->Attachments[0].Model.ToSoftObjectPath().ToString(),
			FString(TEXT("/Engine/BasicShapes/Cube.Cube")));
		TestEqual(TEXT("the attachment point survives save and reload"), ReloadedAsset->Attachments[0].Socket, FName(TEXT("Grip_R")));
		TestTrue(TEXT("the detail keeps its non-default transform"), ReloadedAsset->Attachments[0].RelativeTransform.Equals(
			FTransform(FRotator(5.f, 15.f, 25.f), FVector(8.f, 9.f, 10.f), FVector(0.7f, 0.8f, 0.9f))));
		TestTrue(TEXT("the detail keeps its hidden state"), ReloadedAsset->Attachments[0].bHidden);
	}
	TestEqual(TEXT("the selected SoundWave survives save and reload"), ReloadedAsset->FireSound.ToSoftObjectPath().ToString(),
		FString(TEXT("/Engine/EditorSounds/Notifications/CompileSuccess.CompileSuccess")));

	ReopenedBuilder->Create();
	TestTrue(TEXT("creating the reopened source updates it and makes the saved result ready to test"),
		ReopenedBuilder->CreatedSafeName == SafeAssetName(ReloadedAsset->DisplayName) && ReopenedBuilder->CanTestLocally());
	ReopenedBuilder->Working->Tuning.Damage += 1.f;
	TestFalse(TEXT("an unsaved weapon setting edit disables Test locally"), ReopenedBuilder->CanTestLocally());

	// A copied or externally renamed source can carry a display name that belongs to another
	// saved output. It must be treated as a new weapon, even though the display names match.
	FString CollisionName = FString::Printf(TEXT("UI_Identity_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	while (CollisionName.Len() < 64) CollisionName.AppendChar(TEXT('X'));
	FString ConflictingLongName = CollisionName.Left(60) + TEXT("DIFF");
	TestEqual(TEXT("both long display names share the 60-character canonical folder name"),
		SafeAssetName(CollisionName), SafeAssetName(ConflictingLongName));
	TestEqual(TEXT("the boundary fixture still has a 64-character display name"), CollisionName.Len(), 64);
	FSourceFixtureCleanup CollisionCleanup;
	CollisionCleanup.SetDisplayName(CollisionName);
	TSharedRef<SMantleWeaponBuilder> CollisionBuilder = SNew(SMantleWeaponBuilder);
	CollisionBuilder->SetHold(EMantleWeaponHold::Unarmed);
	CollisionBuilder->SetModel(Cube);
	CollisionBuilder->Working->DisplayName = CollisionName;
	CollisionBuilder->bNameEdited = true;
	CollisionBuilder->Working->Tuning.Damage = 83.f;
	CollisionBuilder->Create();
	TestTrue(TEXT("the boundary-length existing fist weapon output is a valid created weapon"), CollisionBuilder->CanTestLocally());
	const FString CollisionPackagePath = SourcePackagePath(CollisionName);
	const FString CollisionAssetName = FPackageName::GetLongPackageAssetName(CollisionPackagePath);
	UPackage* CollisionPackage = FindPackage(nullptr, *CollisionPackagePath);
	UMantleWeaponSource* CollisionAsset = CollisionPackage
		? FindObject<UMantleWeaponSource>(CollisionPackage, *CollisionAssetName)
		: nullptr;
	if (!TestNotNull(TEXT("the existing same-name output is available for the identity-collision case"), CollisionAsset))
	{
		return false;
	}

	// Reproduce a source saved by an older builder in its full 64-character folder. Its display
	// name now canonicalizes to the same first60 as the existing output above. Reopening must keep
	// the old package selected, while Create chooses a new canonical output instead of redirecting it.
	const FString LegacyFolderName = ConflictingLongName;
	const FString LegacyPackagePath = FString::Printf(TEXT("/Game/MantleWeapons/%s/%s_Source"), *LegacyFolderName, *LegacyFolderName);
	FSourceFixtureCleanup LegacyCleanup;
	LegacyCleanup.SetPackagePath(LegacyPackagePath, LegacyFolderName);
	UPackage* LegacyPackage = CreatePackage(*LegacyPackagePath);
	const FString LegacyAssetName = FPackageName::GetLongPackageAssetName(LegacyPackagePath);
	UMantleWeaponSource* LegacyAsset = LegacyPackage
		? NewObject<UMantleWeaponSource>(LegacyPackage, FName(*LegacyAssetName), RF_Public | RF_Standalone)
		: nullptr;
	if (!TestNotNull(TEXT("the old full-length source package fixture is created"), LegacyAsset)) return false;
	LegacyAsset->DisplayName = ConflictingLongName;
	LegacyAsset->Hold = EMantleWeaponHold::Unarmed;
	LegacyAsset->Model = Cube;
	LegacyAsset->Tuning = DefaultTuning(EMantleWeaponHold::Unarmed);
	LegacyAsset->Tuning.Damage = 82.f;
	LegacyPackage->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(LegacyAsset);
	FSavePackageArgs LegacySaveArgs;
	LegacySaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	LegacySaveArgs.SaveFlags = SAVE_NoError;
	const FString LegacyFilename = FPackageName::LongPackageNameToFilename(LegacyPackagePath, FPackageName::GetAssetPackageExtension());
	if (!TestTrue(TEXT("the old full-length source package saves"), UPackage::SavePackage(LegacyPackage, LegacyAsset, *LegacyFilename, LegacySaveArgs))) return false;
	FText LegacyUnloadError;
	if (!TestTrue(TEXT("the old full-length source unloads before it is reopened"), FMantleAssetCleanup::UnloadPackagesClean({ LegacyPackage }, LegacyUnloadError))) return false;
	LegacyPackage = LoadPackage(nullptr, *LegacyPackagePath, LOAD_None);
	LegacyAsset = LegacyPackage ? FindObject<UMantleWeaponSource>(LegacyPackage, *LegacyAssetName) : nullptr;
	if (!TestNotNull(TEXT("the old full-length source reloads from its original path"), LegacyAsset)) return false;
	const FString LegacySourcePackagePath = LegacyPackagePath;
	TSharedRef<SMantleWeaponBuilder> RenamedSourceBuilder = SNew(SMantleWeaponBuilder);
	RenamedSourceBuilder->LoadSourceAsset(LegacyAsset);
	TestTrue(TEXT("a display name that points at another folder is not mistaken for this source's output identity"),
		RenamedSourceBuilder->CreatedSafeName.IsEmpty());
	TestEqual(TEXT("the source's original package remains the selected source"),
		RenamedSourceBuilder->SourceAssetPath.GetLongPackageName(), LegacySourcePackagePath);
	TestFalse(TEXT("an old long-folder source needs Create before it can be tested"), RenamedSourceBuilder->CanTestLocally());
	RenamedSourceBuilder->Create();
	FSourceFixtureCleanup NewOutputCleanup;
	if (!RenamedSourceBuilder->CreatedSafeName.IsEmpty())
	{
		NewOutputCleanup.SetDisplayName(RenamedSourceBuilder->Working->DisplayName);
	}
	TestTrue(TEXT("creating the renamed source chooses a fresh name instead of the colliding output"),
		!RenamedSourceBuilder->CreatedSafeName.IsEmpty()
		&& !RenamedSourceBuilder->CreatedSafeName.Equals(SafeAssetName(CollisionName), ESearchCase::IgnoreCase)
		&& RenamedSourceBuilder->SourceAssetPath.GetLongPackageName() != CollisionPackagePath);
	TestEqual(TEXT("the new output name and its source package use one canonical safe name"),
		RenamedSourceBuilder->SourceAssetPath.GetLongPackageName(), SourcePackagePath(RenamedSourceBuilder->Working->DisplayName));
	TestTrue(TEXT("the numeric collision suffix remains in the final safe name"), RenamedSourceBuilder->CreatedSafeName.EndsWith(TEXT("_2")));
	TestTrue(TEXT("the suffixed output exports and is ready to test"), RenamedSourceBuilder->CanTestLocally());
	TestEqual(TEXT("the unrelated same-name output keeps its saved settings"), CollisionAsset->Tuning.Damage, 83.f);
	TestEqual(TEXT("the unrelated output sharing the same first60 keeps its display name"), CollisionAsset->DisplayName, CollisionName);
	TestEqual(TEXT("the legacy long-folder source remains unchanged"), LegacyAsset->Tuning.Damage, 82.f);
	TestEqual(TEXT("the legacy source keeps its full-length package identity"), LegacyAsset->GetOutermost()->GetName(), LegacyPackagePath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleWeaponBuilderPanelShieldGripPreviewTest, "Mantle.Editor.WeaponBuilderPanel.ShieldGripShowsLeftPalm", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleWeaponBuilderPanelShieldGripPreviewTest::RunTest(const FString& Parameters)
{
	FMantleWeaponBuilderKind Kind(EMantleWeaponFitHold::Shield);
	FMantleBuilderPoint Grip;
	Grip.Role = TEXT("Grip");
	Grip.Transform = FTransform(FRotator::ZeroRotator, FVector(7.f, 11.f, 13.f));
	const TArray<FMantleBuilderPoint> Points = { Grip };
	const TArray<FMantleGhostCapsule> Ghost = Kind.GhostPose(Points);
	TestTrue(TEXT("the shield grip draws a hand at its point"), Ghost.Num() >= 13);
	if (Ghost.Num() >= 1)
	{
		const FVector PalmAxis = Ghost[0].End - Ghost[0].Start;
		TestTrue(TEXT("the shield hand uses the left support palm orientation"), FMath::Abs(PalmAxis.X) > FMath::Abs(PalmAxis.Z));
	}

	TestEqual(TEXT("the overlay draws no hand: it is the body's"), Kind.Overlay(Points).Capsules.Num(), 0);
	TestTrue(TEXT("the shield grip's left hand ends in the back of the hand"), Ghost.ContainsByPredicate([](const FMantleGhostCapsule& C) { return C.Part == FName(TEXT("Hand_L")); }));
	TestTrue(TEXT("the body holding the shield is drawn"), Ghost.ContainsByPredicate([](const FMantleGhostCapsule& C) { return C.Part == FName(TEXT("Torso")); }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleWeaponBuilderPanelAssetInvalidationTest, "Mantle.Editor.WeaponBuilderPanel.UnsavedSelectedAssetChangeDisablesTest", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleWeaponBuilderPanelAssetInvalidationTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponBuilderLogic;
	TSharedRef<SMantleWeaponBuilder> Builder = SNew(SMantleWeaponBuilder);
	const FString PackageName = FString::Printf(TEXT("/Game/Tests/UnsavedWeaponModel_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	UPackage* Package = CreatePackage(*PackageName);
	UStaticMesh* SelectedModel = NewObject<UStaticMesh>(Package, TEXT("SelectedModel"), RF_Transactional);
	Builder->Working->Model = SelectedModel;
	Builder->CreatedSignature = EditSignature(*Builder->Working);
	TestTrue(TEXT("Test locally is ready before the selected unsaved model changes"), Builder->CanTestLocally());

	SelectedModel->SetLightMapResolution(128);
	Package->MarkPackageDirty();
	FProperty* ChangedProperty = FindFProperty<FProperty>(UStaticMesh::StaticClass(), UStaticMesh::GetLightMapResolutionName());
	TestNotNull(TEXT("the selected model has an editable lightmap setting"), ChangedProperty);
	FPropertyChangedEvent ChangedEvent(ChangedProperty, EPropertyChangeType::ValueSet);
	SelectedModel->PostEditChangeProperty(ChangedEvent);
	TestTrue(TEXT("the selected model remains unsaved after its edit"), Package->IsDirty());
	TestFalse(TEXT("an in-place edit to a selected unsaved model invalidates the previous test"), Builder->CanTestLocally());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleWeaponBuilderPanelHoldMappingTest, "Mantle.Editor.WeaponBuilderPanel.HoldMappingRoundTrips", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleWeaponBuilderPanelHoldMappingTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponBuilderLogic;
	for (uint8 H = 0; H <= static_cast<uint8>(EMantleWeaponFitHold::Lmg); ++H)
	{
		const EMantleWeaponFitHold Fit = static_cast<EMantleWeaponFitHold>(H);
		const EMantleWeaponHold Hold = FromFitHold(Fit);
		TestEqual(*FString::Printf(TEXT("fit hold %d keeps its number"), H), static_cast<uint8>(Hold), H);
		TestTrue(*FString::Printf(TEXT("fit hold %d round trips"), H), ToFitHold(Hold) == Fit);
	}
	TestTrue(TEXT("rifle maps to the fit's rifle"), ToFitHold(EMantleWeaponHold::Rifle) == EMantleWeaponFitHold::Rifle);
	TestTrue(TEXT("machine gun maps to the fit's LMG"), ToFitHold(EMantleWeaponHold::Lmg) == EMantleWeaponFitHold::Lmg);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleWeaponBuilderPanelClampTest, "Mantle.Editor.WeaponBuilderPanel.TuningClampsToRanges", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleWeaponBuilderPanelClampTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponBuilderLogic;
	FMantleWeaponTuning Wild;
	Wild.Damage = 5000.f;
	Wild.ShotsPerSecond = -3.f;
	Wild.MagazineSize = 0;
	Wild.SpareAmmo = 100000;
	Wild.ReloadSeconds = 0.f;
	Wild.bAutomatic = false;

	const FMantleWeaponTuning Clamped = ClampTuning(Wild);
	TestEqual(TEXT("damage tops out"), Clamped.Damage, DamageRange().Max);
	TestEqual(TEXT("rate floors"), Clamped.ShotsPerSecond, ShotsPerSecondRange().Min);
	TestEqual(TEXT("magazine floors"), Clamped.MagazineSize, static_cast<int32>(MagazineSizeRange().Min));
	TestEqual(TEXT("spare ammo tops out"), Clamped.SpareAmmo, static_cast<int32>(SpareAmmoRange().Max));
	TestEqual(TEXT("reload floors"), Clamped.ReloadSeconds, ReloadSecondsRange().Min);
	TestFalse(TEXT("the automatic flag is untouched"), Clamped.bAutomatic);

	FMantleWeaponTuning Fine = DefaultTuning(EMantleWeaponHold::Rifle);
	Fine.SpareAmmo = 0;
	TestEqual(TEXT("zero spare ammo is allowed"), ClampTuning(Fine).SpareAmmo, 0);

	TestEqual(TEXT("a name with spaces is a safe asset name"), SafeAssetName(TEXT("My Rifle")), FString(TEXT("My_Rifle")));
	TestEqual(TEXT("an empty name still has a name"), SafeAssetName(TEXT("  ")), FString(TEXT("MyWeapon")));
	TestEqual(TEXT("the source lives under the weapon's own folder"), SourcePackagePath(TEXT("My Rifle")), FString(TEXT("/Game/MantleWeapons/My_Rifle/My_Rifle_Source")));

	const TOptional<EMantleWeaponHold> Offered = AvailableHoldNamedIn(TEXT("This is short for a rifle (30 cm). Try the pistol or SMG preset."), EMantleWeaponHold::Rifle);
	TestTrue(TEXT("a rifle that is too short is offered the pistol"), Offered.IsSet() && Offered.GetValue() == EMantleWeaponHold::Pistol);
	TestFalse(TEXT("nothing to switch to when only the current hold is named"),
		AvailableHoldNamedIn(TEXT("This is long for a rifle (200 cm). Try the rifle preset."), EMantleWeaponHold::Rifle).IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleWeaponBuilderPanelMergeTest, "Mantle.Editor.WeaponBuilderPanel.NewModelKeepsCreatorPoints", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleWeaponBuilderPanelMergeTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponBuilderPanelTestPrivate;
	const FMantleWeaponBuilderKind Kind(EMantleWeaponFitHold::Rifle);

	// The first model, and a point the creator moved by hand.
	TArray<FMantleBuilderPoint> Points = Kind.GuessPointsFromPositions(BoxCorners(85.f, 20.f, 6.f));
	if (!TestTrue(TEXT("the first model gives guesses"), Points.Num() > 0))
	{
		return false;
	}
	const FName Role = Points[0].Role;
	FMantleBuilderPoint* Placed = nullptr;
	for (FMantleBuilderPoint& Point : Points)
	{
		if (Point.Role == Role)
		{
			Placed = &Point;
		}
	}
	const FVector Moved(1.0, 2.0, 3.0);
	Placed->Transform.SetLocation(Moved);
	Placed->Source = EMantleBuilderPointSource::Creator;

	// A different model: the creator's point stays where it was put, the others follow the new guesses.
	const TArray<FMantleBuilderPoint> NewGuesses = Kind.GuessPointsFromPositions(BoxCorners(100.f, 30.f, 8.f));
	const TArray<FMantleBuilderPoint> Merged = MantleBuilderPoints::MergeGuesses(Points, NewGuesses);
	const FMantleBuilderPoint* Kept = MantleBuilderPoints::Find(Merged, Role);
	if (!TestNotNull(TEXT("the placed point survives"), Kept))
	{
		return false;
	}
	TestTrue(TEXT("a creator point keeps its place"), Kept->Transform.GetLocation().Equals(Moved));
	TestTrue(TEXT("and stays the creator's"), Kept->Source == EMantleBuilderPointSource::Creator);
	const FMantleBuilderPoint* NewGuess = MantleBuilderPoints::Find(NewGuesses, Role);
	if (TestNotNull(TEXT("the new model guesses the same role"), NewGuess))
	{
		TestTrue(TEXT("a creator point still learns the new guess"), Kept->Guess.GetLocation().Equals(NewGuess->Guess.GetLocation()));
	}
	for (const FMantleBuilderPoint& Point : Merged)
	{
		if (Point.Role != Role && Point.Source == EMantleBuilderPointSource::Guess)
		{
			const FMantleBuilderPoint* Fresh = MantleBuilderPoints::Find(NewGuesses, Point.Role);
			if (Fresh)
			{
				TestTrue(*FString::Printf(TEXT("%s follows the new guess"), *Point.Role.ToString()), Point.Transform.GetLocation().Equals(Fresh->Transform.GetLocation()));
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleWeaponBuilderPanelSizeAndTurnTest, "Mantle.Editor.WeaponBuilderPanel.SizeAndTurnCorrections", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleWeaponBuilderPanelSizeAndTurnTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponBuilderPanelTestPrivate;
	FMantleWeaponBuilderKind Kind(EMantleWeaponFitHold::Rifle);
	const TArray<FVector3f> Box = BoxCorners(85.f, 20.f, 6.f);
	Kind.GuessPointsFromPositions(Box);
	const float GuessedScale = Kind.GetLastFit().Scale;

	Kind.SetScaleOverride(GuessedScale * 2.f);
	Kind.GuessPointsFromPositions(Box);
	TestTrue(TEXT("the creator's size replaces the guess"), FMath::IsNearlyEqual(Kind.GetLastFit().Scale, GuessedScale * 2.f, 1.0e-4f));
	TestTrue(TEXT("and the fit's transform carries it"), FMath::IsNearlyEqual(static_cast<float>(Kind.GetLastFit().MeshToWeapon.GetScale3D().X), GuessedScale * 2.f, 1.0e-4f));

	Kind.SetScaleOverride(0.f);
	const TArray<FMantleBuilderPoint> Straight = Kind.GuessPointsFromPositions(Box);
	Kind.SetFlipRotation(FQuat(FVector::UpVector, UE_PI));
	const TArray<FMantleBuilderPoint> Flipped = Kind.GuessPointsFromPositions(Box);
	const FMantleBuilderPoint* A = MantleBuilderPoints::Find(Straight, TEXT("Muzzle"));
	const FMantleBuilderPoint* B = MantleBuilderPoints::Find(Flipped, TEXT("Muzzle"));
	if (TestNotNull(TEXT("muzzle before"), A) && TestNotNull(TEXT("muzzle after"), B))
	{
		TestFalse(TEXT("flipping front and back moves the muzzle to the other end"), A->Transform.GetLocation().Equals(B->Transform.GetLocation(), 0.5));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleWeaponBuilderPanelFreeNameTest, "Mantle.Editor.WeaponBuilderPanel.FirstFreeDisplayName", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleWeaponBuilderPanelFreeNameTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponBuilderLogic;
	TArray<FString> Taken;
	TestEqual(TEXT("a free name is kept"), FirstFreeDisplayName(TEXT("My Rifle"), Taken), FString(TEXT("My Rifle")));

	Taken.Add(TEXT("My_Rifle"));
	TestEqual(TEXT("a taken name gets 2"), FirstFreeDisplayName(TEXT("My Rifle"), Taken), FString(TEXT("My Rifle 2")));
	TestEqual(TEXT("case does not matter"), FirstFreeDisplayName(TEXT("my rifle"), Taken), FString(TEXT("my rifle 2")));

	Taken.Add(TEXT("My_Rifle_2"));
	Taken.Add(TEXT("My_Rifle_3"));
	TestEqual(TEXT("the first free number wins"), FirstFreeDisplayName(TEXT("My Rifle"), Taken), FString(TEXT("My Rifle 4")));
	TestEqual(TEXT("other names are untouched"), FirstFreeDisplayName(TEXT("My Pistol"), Taken), FString(TEXT("My Pistol")));

	FString BoundaryName = TEXT("Boundary_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	while (BoundaryName.Len() < 64) BoundaryName.AppendChar(TEXT('X'));
	const FString BoundarySafeName = SafeAssetName(BoundaryName);
	Taken.Add(BoundarySafeName);
	const FString BoundarySuffix = FirstFreeDisplayName(BoundaryName, Taken);
	const FString BoundarySuffixSafeName = SafeAssetName(BoundarySuffix);
	TestTrue(TEXT("a taken boundary name keeps the numeric suffix inside the safe-name limit"),
		BoundarySuffixSafeName.Len() <= 60 && BoundarySuffixSafeName.EndsWith(TEXT("_2")));
	TestFalse(TEXT("the boundary suffix does not collapse back onto the existing output"),
		BoundarySuffixSafeName.Equals(BoundarySafeName, ESearchCase::IgnoreCase));
	TArray<FString> ManyBoundaryNames = { BoundarySafeName };
	for (int32 Number = 2; Number <= 10; ++Number)
	{
		const FString Suffix = FString::Printf(TEXT(" %d"), Number);
		ManyBoundaryNames.Add(SafeAssetName(BoundaryName.Left(60 - Suffix.Len()) + Suffix));
	}
	const FString DoubleDigitSuffix = SafeAssetName(FirstFreeDisplayName(BoundaryName, ManyBoundaryNames));
	TestTrue(TEXT("a double-digit collision suffix keeps both digits at the boundary"),
		DoubleDigitSuffix.Len() <= 60 && DoubleDigitSuffix.EndsWith(TEXT("_11")));
	return true;
}

#endif
