// Copyright Joshua Eger. All Rights Reserved.
#include "MantleWeaponBuilderExport.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "HAL/FileManager.h"
#include "MantleWeaponDocument.h"
#include "MantleWeaponRigidMesh.h"
#include "MantleWeaponSource.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// The weapon builder's export: a source on the engine cube with hand-placed points becomes a rigid mesh with its
// sockets and a weapon document that parses back with the tuning numbers. Export writes under /Game/MantleWeapons/<Name>.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleWeaponBuilderExportTestPrivate
{
	const TCHAR* const ExportName = TEXT("WBExportTest");

	/** Removes what the export made, from memory and from disk, however the test ends. */
	struct FExportCleanup
	{
		~FExportCleanup()
		{
			for (const FString& Prefix : { FString(TEXT("SKM_")), FString(TEXT("SK_")) })
			{
				const FString PackageName = FString(TEXT("/Game/MantleWeapons/")) / ExportName / (Prefix + ExportName);
				if (UPackage* Package = FindPackage(nullptr, *PackageName))
				{
					if (UObject* Asset = FindObject<UObject>(Package, *(Prefix + ExportName)))
					{
						FAssetRegistryModule::AssetDeleted(Asset);
						Asset->ClearFlags(RF_Public | RF_Standalone);
						Asset->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
						Asset->MarkAsGarbage();
					}
				}
			}
			IFileManager::Get().DeleteDirectory(*FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / TEXT("MantleWeapons") / ExportName), false, true);
		}
	};

	FMantleBuilderPoint MakePoint(const TCHAR* Role, const FVector& Location)
	{
		FMantleBuilderPoint Point;
		Point.Role = FName(Role);
		Point.Transform = FTransform(FQuat::Identity, Location);
		Point.Guess = Point.Transform;
		Point.Source = EMantleBuilderPointSource::Creator;
		return Point;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWeaponBuilderExportRifleTest,
	"Mantle.Editor.WeaponBuilderExport.Rifle",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWeaponBuilderExportRifleTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponBuilderExportTestPrivate;
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("the engine cube loads"), Cube))
	{
		return false;
	}
	FExportCleanup Cleanup;

	UMantleWeaponSource* Source = NewObject<UMantleWeaponSource>(GetTransientPackage());
	Source->DisplayName = ExportName;
	Source->Hold = EMantleWeaponHold::Rifle;
	Source->Model = Cube;
	Source->bScaleOverridden = true;
	Source->FitScale = 0.5f;
	Source->Points.Add(MakePoint(TEXT("Grip_R"), FVector(0.0, 0.0, -20.0)));
	Source->Points.Add(MakePoint(TEXT("Grip_L"), FVector(30.0, 0.0, -10.0)));
	Source->Points.Add(MakePoint(TEXT("Muzzle"), FVector(50.0, 0.0, 10.0)));
	Source->Points.Add(MakePoint(TEXT("Sight"), FVector(10.0, 0.0, 20.0)));
	Source->Tuning.Damage = 21.f;
	Source->Tuning.ShotsPerSecond = 10.f;
	Source->Tuning.MagazineSize = 20;
	Source->Tuning.SpareAmmo = 80;
	Source->Tuning.ReloadSeconds = 2.f;
	Source->Tuning.bAutomatic = true;

	TArray<FText> Messages;
	const bool bExported = FMantleWeaponBuilderExport::Export(*Source, Messages);
	for (const FText& Message : Messages)
	{
		AddInfo(Message.ToString());
	}
	if (!TestTrue(TEXT("the export succeeds"), bExported))
	{
		return false;
	}
	TestTrue(TEXT("it says the weapon is ready"), Messages.Num() > 0 && Messages[0].ToString().Contains(TEXT("Your weapon is ready")));

	// The mesh, with the sockets the runtime reads.
	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *FString::Printf(TEXT("/Game/MantleWeapons/%s/SKM_%s.SKM_%s"), ExportName, ExportName, ExportName));
	if (!TestNotNull(TEXT("the skeletal mesh exists"), Mesh))
	{
		return false;
	}
	const TMap<FName, FTransform> Sockets = FMantleWeaponRigidMesh::ReadSocketsInMeshSpace(Mesh);
	TestTrue(TEXT("Grip_R is a socket"), Sockets.Contains(TEXT("Grip_R")));
	TestTrue(TEXT("Muzzle is a socket"), Sockets.Contains(TEXT("Muzzle")));
	TestTrue(TEXT("Sight is a socket"), Sockets.Contains(TEXT("Sight")));

	// The document.
	const FString File = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / TEXT("MantleWeapons") / ExportName / (FString(ExportName) + TEXT(".weapon.json")));
	FString Json;
	if (!TestTrue(TEXT("the weapon document exists"), FFileHelper::LoadFileToString(Json, *File)))
	{
		return false;
	}
	FMantleWeaponDesc Desc;
	FString ParseError;
	if (!TestTrue(TEXT("the document parses"), FMantleWeaponDocument::Parse(Json, Desc, ParseError)))
	{
		AddError(ParseError);
		return false;
	}
	TestTrue(TEXT("the archetype is rifle"), Desc.Archetype == EMantleWeaponDocArchetype::Rifle);
	TestEqual(TEXT("the name"), Desc.Name, FString(ExportName));
	TestEqual(TEXT("the mesh is named"), Desc.Mesh, FString::Printf(TEXT("SKM_%s"), ExportName));
	TestTrue(TEXT("two-handed"), Desc.IsTwoHanded());
	TestEqual(TEXT("the hotbar icon is named, a local name for the library picture"), Desc.Icon, FString::Printf(TEXT("%s_Icon_0"), ExportName));
	TestEqual(TEXT("the shot interval"), Desc.Fire.Interval, 0.1, 1.0e-9);
	TestEqual(TEXT("the damage"), Desc.Fire.Damage.Value, 21.0);
	TestTrue(TEXT("automatic"), Desc.Fire.bAutomatic.Value);
	TestEqual(TEXT("semi and auto"), Desc.Fire.Modes.Value.Num(), 2);
	TestEqual(TEXT("the magazine size"), Desc.Magazine.Size, 20);
	TestEqual(TEXT("the spare ammo"), Desc.Magazine.StartingReserve.Value, 80);
	TestEqual(TEXT("the reload time"), Desc.Reload.Seconds.Value, 2.0);
	// Aiming holds the sight where a shooter's eye would be, 16.5 cm forward of the back of the gun.
	if (TestTrue(TEXT("the first-person block is written"), Desc.FirstPerson.bSet) && Desc.Socket(EMantleWeaponDocSocket::Sight).bSet)
	{
		const double RearX = Mesh->GetBounds().GetBox().Min.X;
		const double Expected = Desc.Socket(EMantleWeaponDocSocket::Sight).Value.Location.Value.X - (RearX + FMantleWeaponBuilderExport::LongGunEyeFromRearCm);
		AddInfo(FString::Printf(TEXT("sight distance %.2f cm (rear %.2f, expected %.2f)"), Desc.FirstPerson.Value.SightDistance.Value, RearX, Expected));
		TestEqual(TEXT("the sight is held so the eye sits 16.5 cm forward of the back of the gun"), Desc.FirstPerson.Value.SightDistance.Value, Expected, 1.0);
	}
	TestTrue(TEXT("the sight socket is set"), Desc.Socket(EMantleWeaponDocSocket::Sight).bSet);
	TestTrue(TEXT("the muzzle socket is set"), Desc.Socket(EMantleWeaponDocSocket::Muzzle).bSet);
	TestTrue(TEXT("the right grip is set"), Desc.Socket(EMantleWeaponDocSocket::GripR).bSet);
	TestTrue(TEXT("the left grip is set"), Desc.Socket(EMantleWeaponDocSocket::GripL).bSet);
	TestEqual(TEXT("sockets are on root"), Desc.Socket(EMantleWeaponDocSocket::Muzzle).Value.Bone, FString(TEXT("root")));

	// Exporting again over the first files works.
	Messages.Reset();
	TestTrue(TEXT("a second export over the first succeeds"), FMantleWeaponBuilderExport::Export(*Source, Messages));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWeaponBuilderExportNoModelTest,
	"Mantle.Editor.WeaponBuilderExport.NoModel",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWeaponBuilderExportNoModelTest::RunTest(const FString& Parameters)
{
	UMantleWeaponSource* Source = NewObject<UMantleWeaponSource>(GetTransientPackage());
	TArray<FText> Messages;
	TestFalse(TEXT("a weapon with no model is refused"), FMantleWeaponBuilderExport::Export(*Source, Messages));
	TestTrue(TEXT("and says why"), Messages.Num() > 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
