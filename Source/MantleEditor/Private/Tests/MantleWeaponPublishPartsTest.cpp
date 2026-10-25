// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceAssetConverter.h"
#include "MantlePlaceAssets.h"
#include "MantlePng.h"
#include "MantleThumbnailChoice.h"
#include "MantleThumbnailStore.h"
#include "MantleWeaponBuilderExport.h"
#include "MantleWeaponBuilderLogic.h"
#include "MantleWeaponDocument.h"
#include "MantleWeaponSource.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Materials/Material.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Sound/SoundWave.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

#if WITH_DEV_AUTOMATION_TESTS

// A weapon with extra parts and its own attack sounds publishes. The builder copies each part's model and each
// sound into the weapon's folder; the publish gathers them (a static part as a mesh, a skeletal part as a skeletal mesh, the sounds
// as a SoundEmitter's) with every slot's material, and the settings file it uploads names each by uploaded id, the parts' materials
// in a materials list of their own. A real builder export in Content/MantleWeapons, deleted after the test; no network.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleWeaponPublishPartsTestPrivate
{
	struct FExportedWeapon
	{
		FString Name = TEXT("WB898_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12);
		FString PackageFolder() const { return TEXT("/Game/MantleWeapons/") + Name; }
		FString Folder() const { return FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / TEXT("MantleWeapons") / Name); }
		~FExportedWeapon()
		{
			TArray<UObject*> Assets;
			for (TObjectIterator<UObject> It; It; ++It)
			{
				if (It->IsAsset() && It->GetOutermost()->GetName().StartsWith(PackageFolder() + TEXT("/")))
				{
					Assets.Add(*It);
				}
			}
			for (UObject* Asset : Assets)
			{
				FAssetRegistryModule::AssetDeleted(Asset);
				Asset->ClearFlags(RF_Public | RF_Standalone);
				Asset->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
				Asset->MarkAsGarbage();
			}
			IFileManager::Get().DeleteDirectory(*Folder(), /*RequireExists*/ false, /*Tree*/ true);
		}
	};

	template <typename EntryType>
	bool HasKey(const TArray<EntryType>& List, const FString& Key)
	{
		return List.ContainsByPredicate([&Key](const EntryType& Entry) { return Entry.Key == Key; });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWeaponPublishPartsTest,
	"Mantle.WeaponPublish.Parts.NamesExtraPartsAndAttackSoundsByUploadedId",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWeaponPublishPartsTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponPublishPartsTestPrivate;
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	USkeletalMesh* SkeletalCube = LoadObject<USkeletalMesh>(nullptr, TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
	USoundWave* Sound = LoadObject<USoundWave>(nullptr, TEXT("/Engine/EditorSounds/Notifications/CompileSuccess.CompileSuccess"));
	if (!TestNotNull(TEXT("fixture cube"), Cube) || !TestNotNull(TEXT("fixture skeletal cube"), SkeletalCube) || !TestNotNull(TEXT("fixture sound"), Sound))
	{
		return false;
	}

	// A rifle with a static sight, a skeletal grip and its own attack sound, as the builder makes it.
	FExportedWeapon Weapon;
	UMantleWeaponSource* Input = NewObject<UMantleWeaponSource>();
	Input->DisplayName = Weapon.Name;
	Input->Hold = EMantleWeaponHold::Rifle;
	Input->Tuning = MantleWeaponBuilderLogic::DefaultTuning(Input->Hold);
	Input->Model = Cube;
	Input->bScaleOverridden = true;
	Input->FitScale = 1.f;
	FMantleWeaponSourceAttachment& Sight = Input->Attachments.AddDefaulted_GetRef();
	Sight.Name = TEXT("Sight");
	Sight.Model = Cube;
	FMantleWeaponSourceAttachment& Grip = Input->Attachments.AddDefaulted_GetRef();
	Grip.Name = TEXT("Grip");
	Grip.Model = SkeletalCube;
	Input->FireSound = Sound;
	TArray<FText> Messages;
	if (!TestTrue(TEXT("the builder exports the weapon"), FMantleWeaponBuilderExport::Export(*Input, Messages)))
	{
		for (const FText& Message : Messages)
		{
			AddError(Message.ToString());
		}
		return false;
	}
	FString Text;
	if (!TestTrue(TEXT("its settings file is written"), FFileHelper::LoadFileToString(Text, *(Weapon.Folder() / (Weapon.Name + TEXT(".weapon.json"))))))
	{
		return false;
	}

	// Nothing about it is refused any more, and it names what is in its folder.
	FMantleBuiltWeaponParts Parts;
	TestEqual(TEXT("an extra part and an attack sound no longer stop a publish"), FMantlePlaceAssets::WeaponDocumentProblem(Weapon.Name, Text, &Parts), FString());
	if (!TestEqual(TEXT("both extra parts are found"), Parts.Attachments.Num(), 2))
	{
		return false;
	}
	UStaticMesh* SightModel = Cast<UStaticMesh>(Parts.Attachments[0]);
	USkeletalMesh* GripModel = Cast<USkeletalMesh>(Parts.Attachments[1]);
	if (!TestNotNull(TEXT("the sight is a mesh in the weapon's folder"), SightModel) || !TestNotNull(TEXT("the grip is a skeletal mesh there"), GripModel)
		|| !TestNotNull(TEXT("the attack sound is a wave there"), Parts.Fire))
	{
		return false;
	}
	TestTrue(TEXT("the parts are the weapon's own copies"), SightModel->GetPathName().StartsWith(Weapon.PackageFolder() + TEXT("/"))
		&& GripModel->GetPathName().StartsWith(Weapon.PackageFolder() + TEXT("/")) && Parts.Fire->GetPathName().StartsWith(Weapon.PackageFolder() + TEXT("/")));
	TestNull(TEXT("no quieter sound is named"), Parts.FireSuppressed);

	// Gathered: the parts as a mesh and a skeletal mesh, the sound as a sound, every slot's material.
	FMantlePlaceDependencies Dependencies;
	if (!TestTrue(TEXT("the weapon is gathered"), Dependencies.AddBuiltWeapon(Weapon.Name, TEXT("Spawn"))))
	{
		return false;
	}
	const FString SightKey = FMantlePlaceAssets::MeshKey(SightModel);
	const FString GripKey = FMantlePlaceAssets::SkeletalMeshKey(GripModel);
	const FString SoundKey = FMantlePlaceAssets::SoundKey(Parts.Fire);
	TestTrue(TEXT("the static part is gathered as a mesh"), HasKey(Dependencies.Meshes, SightKey));
	TestTrue(TEXT("the skeletal part is gathered as a skeletal mesh"), HasKey(Dependencies.SkeletalMeshes, GripKey));
	TestTrue(TEXT("the attack sound is gathered as a sound"), HasKey(Dependencies.Sounds, SoundKey));
	TArray<FString> PartMaterialKeys;
	for (const int32 Slot : FMantlePlaceAssets::UploadSlots(SightModel))
	{
		UMaterialInterface* Material = FMantlePlaceAssets::MeshSlotMaterial(SightModel, Slot);
		PartMaterialKeys.Add(FMantlePlaceAssets::MaterialKey(Material ? Material : UMaterial::GetDefaultMaterial(MD_Surface)));
	}
	for (const int32 Slot : FMantlePlaceAssets::SkeletalUploadSlots(GripModel))
	{
		UMaterialInterface* Material = GripModel->GetMaterials().IsValidIndex(Slot) ? GripModel->GetMaterials()[Slot].MaterialInterface.Get() : nullptr;
		PartMaterialKeys.Add(FMantlePlaceAssets::MaterialKey(Material ? Material : UMaterial::GetDefaultMaterial(MD_Surface)));
	}
	TestTrue(TEXT("the parts have slots to dress"), PartMaterialKeys.Num() >= 2);
	for (const FString& Key : PartMaterialKeys)
	{
		TestTrue(FString::Printf(TEXT("the part material %s is gathered, even an engine one"), *Key), HasKey(Dependencies.Materials, Key));
	}

	// Converted: the parts' and the sound's keys, in the settings file's order.
	FMantlePlaceAssetConverter Converter;
	FMantlePlaceAssetSet Set;
	if (!TestEqual(TEXT("one weapon gathered"), Dependencies.Weapons.Num(), 1) || !TestTrue(TEXT("it converts"), Converter.ConvertWeapon(Dependencies.Weapons[0], Set)))
	{
		for (const FMantleValidationIssue& Issue : Set.Issues)
		{
			AddError(Issue.Problem);
		}
		return false;
	}
	const FMantleConvertedWeapon* Converted = Set.FindWeapon(Dependencies.Weapons[0].Key);
	if (!TestNotNull(TEXT("the converted weapon"), Converted) || !TestEqual(TEXT("two extra parts"), Converted->Attachments.Num(), 2))
	{
		return false;
	}
	TestEqual(TEXT("the sight's model key"), Converted->Attachments[0].MeshKey, SightKey);
	TestEqual(TEXT("the grip's model key"), Converted->Attachments[1].MeshKey, GripKey);
	TestEqual(TEXT("the attack sound's key"), Converted->FireSoundKey, SoundKey);
	TestTrue(TEXT("no quieter sound key"), Converted->FireSuppressedSoundKey.IsEmpty());

	// #124: with a custom thumbnail chosen for it (kept under the weapon's name), the weapon uploads a 512 x 512 library picture beside it.
	{
		TArray<FColor> Pixels;
		Pixels.Init(FColor(200, 80, 40, 255), 640 * 640);
		const FString PictureFile = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("MantleWeaponThumbnail.png"));
		FFileHelper::SaveArrayToFile(FMantlePng::EncodeRGBA8(640, 640, Pixels), *PictureFile);
		FMantleThumbnailChoice Choice;
		Choice.Mode = EMantleThumbnailMode::Custom;
		Choice.ImageFile.FilePath = PictureFile;
		// The choice lives in the project's Config ini: put it back as it was (no file at all when there was none).
		const bool bHadStore = IFileManager::Get().FileExists(*FMantleThumbnailStore::DefaultConfigFile());
		FMantleThumbnailStore::Save(Weapon.Name, TEXT("weapon"), Choice, FMantleThumbnailStore::DefaultConfigFile());
		ON_SCOPE_EXIT
		{
			FMantleThumbnailStore::Save(Weapon.Name, TEXT("weapon"), FMantleThumbnailChoice(), FMantleThumbnailStore::DefaultConfigFile());
			if (!bHadStore)
			{
				IFileManager::Get().Delete(*FMantleThumbnailStore::DefaultConfigFile());
			}
		};
		FMantlePlaceAssetConverter PictureConverter;
		FMantlePlaceAssetSet PictureSet;
		if (TestTrue(TEXT("it converts with a custom thumbnail"), PictureConverter.ConvertWeapon(Dependencies.Weapons[0], PictureSet)))
		{
			const FMantleConvertedWeapon* WithPicture = PictureSet.FindWeapon(Dependencies.Weapons[0].Key);
			TestTrue(TEXT("it names a library picture"), WithPicture && !WithPicture->ThumbnailKey.IsEmpty());
		}
	}

	// Written once everything has an id: the settings file names the uploads, the parts' materials included.
	int32 NextId = 1;
	auto Give = [&Set, &NextId](const FString& Key)
	{
		FMantlePlaceUploadedRef& Ref = Set.Uploaded.FindOrAdd(Key);
		if (Ref.Id.IsEmpty())
		{
			Ref.Id = FString::Printf(TEXT("ast_u%032x"), NextId++);
		}
		return Ref.Id;
	};
	Give(Converted->MeshKey);
	for (const FMantleConvertedWeapon::FSlot& Slot : Converted->Slots)
	{
		Give(Slot.MaterialKey);
	}
	const FString SightId = Give(SightKey);
	const FString GripId = Give(GripKey);
	const FString SoundId = Give(SoundKey);
	for (const FMantleConvertedWeapon::FAttachment& Part : Converted->Attachments)
	{
		for (const FMantleConvertedWeapon::FSlot& Slot : Part.Slots)
		{
			Give(Slot.MaterialKey);
		}
	}
	TArray<uint8> Bytes;
	FString Error;
	if (!TestTrue(TEXT("with every id the upload is written"), Set.WeaponBytes(*Converted, Bytes, Error)))
	{
		AddError(Error);
		return false;
	}
	const FUTF8ToTCHAR Utf8(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
	const FString Json(Utf8.Length(), Utf8.Get());
	FMantleWeaponDesc Desc;
	if (!TestTrue(TEXT("the upload is a valid settings file"), FMantleWeaponDocument::Parse(Json, Desc, Error)))
	{
		AddError(Error);
		return false;
	}
	if (TestTrue(TEXT("it keeps both extra parts"), Desc.Attachments.bSet && Desc.Attachments.Value.Num() == 2))
	{
		const FMantleWeaponDocAttachment& OutSight = Desc.Attachments.Value[0];
		const FMantleWeaponDocAttachment& OutGrip = Desc.Attachments.Value[1];
		TestEqual(TEXT("the sight names its uploaded mesh"), OutSight.Mesh, SightId);
		TestEqual(TEXT("the grip names its uploaded skeletal mesh"), OutGrip.Mesh, GripId);
		TestEqual(TEXT("the parts keep their names"), OutSight.Name + OutGrip.Name, FString(TEXT("SightGrip")));
		for (const TPair<const FMantleWeaponDocAttachment*, const FMantleConvertedWeapon::FAttachment*> Part :
			{ TPair<const FMantleWeaponDocAttachment*, const FMantleConvertedWeapon::FAttachment*>(&OutSight, &Converted->Attachments[0]),
			  TPair<const FMantleWeaponDocAttachment*, const FMantleConvertedWeapon::FAttachment*>(&OutGrip, &Converted->Attachments[1]) })
		{
			if (TestTrue(TEXT("a part names one material per slot"), Part.Key->Materials.bSet && Part.Key->Materials.Value.Num() == Part.Value->Slots.Num()))
			{
				for (int32 Slot = 0; Slot < Part.Value->Slots.Num(); ++Slot)
				{
					TestEqual(TEXT("by uploaded id"), Part.Key->Materials.Value[Slot], Set.Uploaded.FindChecked(Part.Value->Slots[Slot].MaterialKey).Id);
				}
			}
		}
	}
	if (TestTrue(TEXT("it keeps its sounds"), Desc.Sounds.bSet))
	{
		TestEqual(TEXT("the attack sound by uploaded id"), Desc.Sounds.Value.Fire.bSet ? Desc.Sounds.Value.Fire.Value : FString(), SoundId);
		TestFalse(TEXT("and no quieter sound"), Desc.Sounds.Value.FireSuppressed.bSet);
	}
	TestFalse(TEXT("no local name is left"), Json.Contains(Weapon.Name + TEXT("_")));

	// A part missing from the folder is named, in plain words.
	const FString Missing = Text.Replace(*(Weapon.Name + TEXT("_Attachment_1")), TEXT("NoSuchPart"));
	TestTrue(TEXT("a missing part is named"), FMantlePlaceAssets::WeaponDocumentProblem(Weapon.Name, Missing).Contains(TEXT("NoSuchPart")));
	return true;
}

// A launcher with its own round. The builder copies the round's model beside the weapon and names it in projectile.mesh; the
// publish gathers it as a mesh with every slot's material, and the settings file it uploads names the round and its materials
// (projectile.materials) by uploaded id. A real builder export in Content/MantleWeapons, deleted after the test; no network.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWeaponPublishRoundTest,
	"Mantle.WeaponPublish.Parts.NamesTheRoundByUploadedId",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWeaponPublishRoundTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponPublishPartsTestPrivate;
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (!TestNotNull(TEXT("fixture cube"), Cube) || !TestNotNull(TEXT("fixture sphere"), Sphere))
	{
		return false;
	}

	// A rifle that launches rounds, with a sphere for its round, as the builder makes it.
	FExportedWeapon Weapon;
	UMantleWeaponSource* Input = NewObject<UMantleWeaponSource>();
	Input->DisplayName = Weapon.Name;
	Input->Hold = EMantleWeaponHold::Rifle;
	Input->Tuning = MantleWeaponBuilderLogic::DefaultTuning(Input->Hold);
	Input->Model = Cube;
	Input->bScaleOverridden = true;
	Input->FitScale = 1.f;
	MantleWeaponBuilderLogic::SetDelivery(*Input, EMantleWeaponSourceDelivery::Projectile);
	Input->Projectile.Model = Sphere;
	TArray<FText> Messages;
	if (!TestTrue(TEXT("the builder exports the launcher"), FMantleWeaponBuilderExport::Export(*Input, Messages)))
	{
		for (const FText& Message : Messages)
		{
			AddError(Message.ToString());
		}
		return false;
	}
	FString Text;
	if (!TestTrue(TEXT("its settings file is written"), FFileHelper::LoadFileToString(Text, *(Weapon.Folder() / (Weapon.Name + TEXT(".weapon.json"))))))
	{
		return false;
	}
	FMantleWeaponDesc Written;
	FString Error;
	if (!TestTrue(TEXT("the settings file reads"), FMantleWeaponDocument::Parse(Text, Written, Error)))
	{
		AddError(Error);
		return false;
	}
	const FString RoundName = Weapon.Name + TEXT("_Projectile_0");
	TestTrue(TEXT("the file names the round by its local name"), Written.Projectile.bSet && Written.Projectile.Value.Mesh.bSet
		&& Written.Projectile.Value.Mesh.Value == RoundName);
	TestFalse(TEXT("the builder leaves the round's materials to the publish"), Written.Projectile.bSet && Written.Projectile.Value.Materials.bSet);

	// Nothing about it is refused, and the round is the weapon's own copy.
	FMantleBuiltWeaponParts Parts;
	TestEqual(TEXT("a round does not stop a publish"), FMantlePlaceAssets::WeaponDocumentProblem(Weapon.Name, Text, &Parts), FString());
	UStaticMesh* RoundModel = Cast<UStaticMesh>(Parts.Projectile);
	if (!TestNotNull(TEXT("the round is a mesh in the weapon's folder"), RoundModel))
	{
		return false;
	}
	TestTrue(TEXT("the round is the weapon's own copy"), RoundModel->GetPathName().StartsWith(Weapon.PackageFolder() + TEXT("/")));
	TestEqual(TEXT("no extra parts"), Parts.Attachments.Num(), 0);

	// Gathered: the round as a mesh, with every slot's material.
	FMantlePlaceDependencies Dependencies;
	if (!TestTrue(TEXT("the launcher is gathered"), Dependencies.AddBuiltWeapon(Weapon.Name, TEXT("Spawn"))))
	{
		return false;
	}
	const FString RoundKey = FMantlePlaceAssets::MeshKey(RoundModel);
	TestTrue(TEXT("the round is gathered as a mesh"), HasKey(Dependencies.Meshes, RoundKey));
	TArray<FString> RoundMaterialKeys;
	for (const int32 Slot : FMantlePlaceAssets::UploadSlots(RoundModel))
	{
		UMaterialInterface* Material = FMantlePlaceAssets::MeshSlotMaterial(RoundModel, Slot);
		RoundMaterialKeys.Add(FMantlePlaceAssets::MaterialKey(Material ? Material : UMaterial::GetDefaultMaterial(MD_Surface)));
	}
	TestTrue(TEXT("the round has slots to dress"), RoundMaterialKeys.Num() >= 1);
	for (const FString& Key : RoundMaterialKeys)
	{
		TestTrue(FString::Printf(TEXT("the round material %s is gathered"), *Key), HasKey(Dependencies.Materials, Key));
	}

	// Converted: the round's key and its slots.
	FMantlePlaceAssetConverter Converter;
	FMantlePlaceAssetSet Set;
	if (!TestEqual(TEXT("one weapon gathered"), Dependencies.Weapons.Num(), 1) || !TestTrue(TEXT("it converts"), Converter.ConvertWeapon(Dependencies.Weapons[0], Set)))
	{
		for (const FMantleValidationIssue& Issue : Set.Issues)
		{
			AddError(Issue.Problem);
		}
		return false;
	}
	const FMantleConvertedWeapon* Converted = Set.FindWeapon(Dependencies.Weapons[0].Key);
	if (!TestNotNull(TEXT("the converted weapon"), Converted))
	{
		return false;
	}
	TestEqual(TEXT("the round's model key"), Converted->Projectile.MeshKey, RoundKey);
	TestEqual(TEXT("one slot per round material"), Converted->Projectile.Slots.Num(), RoundMaterialKeys.Num());

	// Written once everything has an id: projectile.mesh and projectile.materials name the uploads.
	int32 NextId = 1;
	auto Give = [&Set, &NextId](const FString& Key)
	{
		FMantlePlaceUploadedRef& Ref = Set.Uploaded.FindOrAdd(Key);
		if (Ref.Id.IsEmpty())
		{
			Ref.Id = FString::Printf(TEXT("ast_u%032x"), NextId++);
		}
		return Ref.Id;
	};
	Give(Converted->MeshKey);
	for (const FMantleConvertedWeapon::FSlot& Slot : Converted->Slots)
	{
		Give(Slot.MaterialKey);
	}
	TArray<uint8> Bytes;
	FString BytesError;
	TestFalse(TEXT("without the round's id nothing is written"), Set.WeaponBytes(*Converted, Bytes, BytesError));
	TestTrue(TEXT("and the creator reads why"), BytesError.Contains(TEXT("round or arrow")));
	const FString RoundId = Give(RoundKey);
	for (const FMantleConvertedWeapon::FSlot& Slot : Converted->Projectile.Slots)
	{
		Give(Slot.MaterialKey);
	}
	Bytes.Reset();
	if (!TestTrue(TEXT("with every id the upload is written"), Set.WeaponBytes(*Converted, Bytes, BytesError)))
	{
		AddError(BytesError);
		return false;
	}
	const FUTF8ToTCHAR Utf8(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
	const FString Json(Utf8.Length(), Utf8.Get());
	FMantleWeaponDesc Desc;
	if (!TestTrue(TEXT("the upload is a valid settings file"), FMantleWeaponDocument::Parse(Json, Desc, Error)))
	{
		AddError(Error);
		return false;
	}
	if (!TestTrue(TEXT("it keeps its projectile"), Desc.Projectile.bSet))
	{
		return false;
	}
	const FMantleWeaponDocProjectile& Round = Desc.Projectile.Value;
	TestEqual(TEXT("the round names its uploaded mesh"), Round.Mesh.bSet ? Round.Mesh.Value : FString(), RoundId);
	if (TestTrue(TEXT("the round names one material per slot"), Round.Materials.bSet && Round.Materials.Value.Num() == Converted->Projectile.Slots.Num()))
	{
		for (int32 Slot = 0; Slot < Converted->Projectile.Slots.Num(); ++Slot)
		{
			TestEqual(TEXT("by uploaded id"), Round.Materials.Value[Slot], Set.Uploaded.FindChecked(Converted->Projectile.Slots[Slot].MaterialKey).Id);
		}
	}
	TestTrue(TEXT("the launcher's numbers are kept"), FMath::IsNearlyEqual(Round.Speed, 4000.0) && FMath::IsNearlyEqual(Round.BlastRadius.Value, 250.0));
	TestFalse(TEXT("no local name is left"), Json.Contains(Weapon.Name + TEXT("_")));

	// A round missing from the folder is named, in plain words.
	const FString Missing = Text.Replace(*RoundName, TEXT("NoSuchRound"));
	TestTrue(TEXT("a missing round is named"), FMantlePlaceAssets::WeaponDocumentProblem(Weapon.Name, Missing).Contains(TEXT("NoSuchRound")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
