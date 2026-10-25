// Copyright Joshua Eger. All Rights Reserved.
#include "MantleWeaponSpawnPoint.h"

#include "MantlePlaceLimits.h"

#include "HAL/FileManager.h"
#include "Misc/Paths.h"

#if WITH_EDITORONLY_DATA
#include "Components/ArrowComponent.h"
#include "Components/BillboardComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/Texture2D.h"
#include "UObject/ConstructorHelpers.h"
#include "UObject/UObjectGlobals.h"
#endif

AMantleWeaponSpawnPoint::AMantleWeaponSpawnPoint()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = false;
	SetCanBeDamaged(false);

	SpawnRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SpawnRoot"));
	SetRootComponent(SpawnRoot);

#if WITH_EDITORONLY_DATA
	EditorArrow = CreateEditorOnlyDefaultSubobject<UArrowComponent>(TEXT("EditorArrow"));
	if (EditorArrow)
	{
		EditorArrow->SetupAttachment(SpawnRoot);
		EditorArrow->ArrowColor = FColor(230, 150, 60);
		EditorArrow->ArrowSize = 1.0f;
		EditorArrow->bIsScreenSizeScaled = true;
		EditorArrow->SetIsVisualizationComponent(true);
		EditorArrow->SetHiddenInGame(true);
	}
	EditorBillboard = CreateEditorOnlyDefaultSubobject<UBillboardComponent>(TEXT("EditorBillboard"));
	if (EditorBillboard)
	{
		EditorBillboard->SetupAttachment(SpawnRoot);
		static ConstructorHelpers::FObjectFinderOptional<UTexture2D> SpawnIcon(TEXT("/Engine/EditorResources/S_TargetPoint"));
		EditorBillboard->Sprite = SpawnIcon.Get();
		EditorBillboard->bIsScreenSizeScaled = true;
		EditorBillboard->SetIsVisualizationComponent(true);
		EditorBillboard->SetHiddenInGame(true);
	}
	EditorGun = CreateEditorOnlyDefaultSubobject<USkeletalMeshComponent>(TEXT("EditorGun"));
	if (EditorGun)
	{
		EditorGun->SetupAttachment(SpawnRoot);
		EditorGun->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		EditorGun->SetGenerateOverlapEvents(false);
		EditorGun->SetCastShadow(false);
		EditorGun->SetIsVisualizationComponent(true);
		EditorGun->SetHiddenInGame(true);
	}
#endif
}

bool AMantleWeaponSpawnPoint::IsBuilderName(const FString& Name)
{
	if (Name.IsEmpty() || Name.Len() > 64)
	{
		return false;
	}
	for (const TCHAR Char : Name)
	{
		if (!((Char >= 'A' && Char <= 'Z') || (Char >= 'a' && Char <= 'z') || (Char >= '0' && Char <= '9') || Char == '_'))
		{
			return false;
		}
	}
	return true;
}

FString AMantleWeaponSpawnPoint::BuiltMeshObjectPath(const FString& Name)
{
	return FString::Printf(TEXT("/Game/MantleWeapons/%s/SKM_%s.SKM_%s"), *Name, *Name, *Name);
}

FString AMantleWeaponSpawnPoint::BuiltDocumentFile(const FString& Name)
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / TEXT("MantleWeapons") / Name / (Name + TEXT(".weapon.json")));
}

TArray<FString> AMantleWeaponSpawnPoint::GetWeaponOptions() const
{
	TArray<FString> Names;
#if WITH_EDITOR
	const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / TEXT("MantleWeapons"));
	TArray<FString> Folders;
	IFileManager::Get().FindFiles(Folders, *(Root / TEXT("*")), /*Files*/ false, /*Directories*/ true);
	Folders.Sort();
	for (const FString& Folder : Folders)
	{
		if (IsBuilderName(Folder) && IFileManager::Get().FileExists(*BuiltDocumentFile(Folder))
			&& IFileManager::Get().FileExists(*(Root / Folder / (TEXT("SKM_") + Folder + TEXT(".uasset")))))
		{
			Names.Add(Folder);
		}
	}
#endif
	return Names;
}

#if WITH_EDITOR
void AMantleWeaponSpawnPoint::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	UpdateEditorPreview();
}

void AMantleWeaponSpawnPoint::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();
	UpdateEditorPreview();
}

void AMantleWeaponSpawnPoint::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	UpdateEditorPreview();
}
#endif

void AMantleWeaponSpawnPoint::UpdateEditorPreview()
{
#if WITH_EDITORONLY_DATA
	if (!EditorGun)
	{
		return;
	}
	USkeletalMesh* Mesh = nullptr;
	if (IsBuilderName(Weapon) && StorageItem.IsEmpty())
	{
		Mesh = LoadObject<USkeletalMesh>(nullptr, *BuiltMeshObjectPath(Weapon), nullptr, LOAD_NoWarn | LOAD_Quiet);
	}
	if (EditorGun->GetSkeletalMeshAsset() != Mesh)
	{
		EditorGun->SetSkeletalMesh(Mesh);
	}
	EditorGun->SetVisibility(Mesh != nullptr);
#endif
}

void AMantleWeaponSpawnPoint::ApplyProps(const FMantleWeaponSpawnProps& In)
{
	WeaponId = In.Weapon;
	StorageItem = In.StorageItem;
	RespawnSeconds = In.RespawnSeconds;
	bEnabled = In.bEnabled;
	UpdateEditorPreview();
}

bool AMantleWeaponSpawnPoint::ReadProps(FMantleWeaponSpawnProps& Out, FString& OutProblem) const
{
	Out.Weapon = WeaponId;
	Out.StorageItem = StorageItem;
	Out.RespawnSeconds = RespawnSeconds;
	Out.bEnabled = bEnabled;
	if (!FMath::IsFinite(RespawnSeconds) || RespawnSeconds < 0.0 || RespawnSeconds > FMantlePlaceLimits::MaxWeaponRespawnSeconds)
	{
		OutProblem = FString::Printf(TEXT("respawns after %g s; set Respawn Seconds between 0 and %g"),
			RespawnSeconds, FMantlePlaceLimits::MaxWeaponRespawnSeconds);
		return false;
	}
	return true;
}
