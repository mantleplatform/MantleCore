// Copyright Joshua Eger. All Rights Reserved.
#include "MantleItemSpawnPoint.h"

#include "MantlePlaceLimits.h"

#if WITH_EDITORONLY_DATA
#include "Components/ArrowComponent.h"
#include "Components/BillboardComponent.h"
#include "Engine/Texture2D.h"
#include "UObject/ConstructorHelpers.h"
#endif

AMantleItemSpawnPoint::AMantleItemSpawnPoint()
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
		EditorArrow->ArrowColor = FColor(90, 200, 120);
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
#endif
}

void AMantleItemSpawnPoint::ApplyProps(const FMantleItemSpawnProps& In)
{
	Item = In.Item;
	Count = In.Count;
	RespawnSeconds = In.RespawnSeconds;
	ModelId = In.Model;
	bEnabled = In.bEnabled;
}

bool AMantleItemSpawnPoint::ReadProps(FMantleItemSpawnProps& Out, FString& OutProblem) const
{
	Out.Item = Item;
	Out.Count = Count;
	Out.RespawnSeconds = RespawnSeconds;
	Out.Model = ModelId;
	Out.bEnabled = bEnabled;
	if (!FMantlePlaceLimits::IsWorldItemId(Item))
	{
		OutProblem = Item.IsEmpty()
			? FString(TEXT("names no item; set Item to an item id your script defines"))
			: FString::Printf(TEXT("names item '%s'; an item id is lowercase letters, digits and underscores (not first), 1 to %d characters"), *Item, FMantlePlaceLimits::MaxWorldItemIdLength);
		return false;
	}
	if (Count < 1 || Count > FMantlePlaceLimits::MaxItemSpawnCount)
	{
		OutProblem = FString::Printf(TEXT("holds %d; set Count between 1 and %d"), Count, FMantlePlaceLimits::MaxItemSpawnCount);
		return false;
	}
	if (!FMath::IsFinite(RespawnSeconds) || RespawnSeconds < FMantlePlaceLimits::MinItemSpawnRespawnSeconds || RespawnSeconds > FMantlePlaceLimits::MaxItemSpawnRespawnSeconds)
	{
		OutProblem = FString::Printf(TEXT("respawns after %g s; set Respawn Seconds between %g and %g"),
			RespawnSeconds, FMantlePlaceLimits::MinItemSpawnRespawnSeconds, FMantlePlaceLimits::MaxItemSpawnRespawnSeconds);
		return false;
	}
	return true;
}
