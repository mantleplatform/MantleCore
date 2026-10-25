// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleSpawnPoint.h"

#include "MantlePlaceLimits.h"

#if WITH_EDITORONLY_DATA
#include "Components/ArrowComponent.h"
#include "Components/BillboardComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/Texture2D.h"
#include "UObject/ConstructorHelpers.h"
#endif

AMantleVehicleSpawnPoint::AMantleVehicleSpawnPoint()
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
		EditorArrow->ArrowColor = FColor(80, 200, 120);
		EditorArrow->ArrowSize = 2.0f;
		EditorArrow->bIsScreenSizeScaled = true;
		EditorArrow->SetIsVisualizationComponent(true);
		EditorArrow->SetHiddenInGame(true);
	}
	EditorBillboard = CreateEditorOnlyDefaultSubobject<UBillboardComponent>(TEXT("EditorBillboard"));
	if (EditorBillboard)
	{
		EditorBillboard->SetupAttachment(SpawnRoot);
		// An engine-shipped placement icon, as AMantleVehicleSpawner uses; cosmetic only.
		static ConstructorHelpers::FObjectFinderOptional<UTexture2D> SpawnIcon(TEXT("/Engine/EditorResources/S_TargetPoint"));
		EditorBillboard->Sprite = SpawnIcon.Get();
		EditorBillboard->bIsScreenSizeScaled = true;
		EditorBillboard->SetIsVisualizationComponent(true);
		EditorBillboard->SetHiddenInGame(true);
	}

	// #614 stage 6: a stored vehicle is another creator's item that is never loaded into the project, so the spawn
	// shows only its name. Two labels back to back: a text render faces one way, +X for the front one.
	for (const bool bFront : { true, false })
	{
		UTextRenderComponent* Label = CreateEditorOnlyDefaultSubobject<UTextRenderComponent>(bFront ? TEXT("EditorLabelFront") : TEXT("EditorLabelBack"));
		(bFront ? EditorLabelFront : EditorLabelBack) = Label;
		if (Label)
		{
			Label->SetupAttachment(SpawnRoot);
			Label->SetRelativeLocation(FVector(0.0, 0.0, 160.0));
			Label->SetRelativeRotation(FRotator(0.0, bFront ? 0.0 : 180.0, 0.0));
			Label->SetHorizontalAlignment(EHTA_Center);
			Label->SetVerticalAlignment(EVRTA_TextCenter);
			Label->SetWorldSize(36.0f);
			Label->SetTextRenderColor(FColor(255, 215, 110));
			Label->SetText(FText::GetEmpty());
			Label->SetVisibility(false);
			Label->SetIsVisualizationComponent(true);
			Label->SetHiddenInGame(true);
		}
	}
#endif
}

FString AMantleVehicleSpawnPoint::MakeEditorLabelText() const
{
	if (StorageItem.IsEmpty())
	{
		return FString();
	}
	return FString::Printf(TEXT("\"%s\"\n(stored vehicle)"), *StorageItem);
}

void AMantleVehicleSpawnPoint::UpdateEditorLabel()
{
#if WITH_EDITORONLY_DATA
	const FString Text = MakeEditorLabelText();
	for (UTextRenderComponent* Label : { EditorLabelFront.Get(), EditorLabelBack.Get() })
	{
		if (Label)
		{
			Label->SetText(FText::FromString(Text));
			Label->SetVisibility(!Text.IsEmpty());
		}
	}
#endif
}

#if WITH_EDITOR
void AMantleVehicleSpawnPoint::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	UpdateEditorLabel();
}

void AMantleVehicleSpawnPoint::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();
	UpdateEditorLabel();
}

void AMantleVehicleSpawnPoint::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	UpdateEditorLabel();
}
#endif

void AMantleVehicleSpawnPoint::ApplyProps(const FMantleVehicleSpawnProps& In)
{
	VehicleId = In.Vehicle;
	StorageItem = In.StorageItem;
	RespawnSeconds = In.RespawnSeconds;
	bEnabled = In.bEnabled;
	Livery = In.Livery;
	UpdateEditorLabel();
}

bool AMantleVehicleSpawnPoint::ReadProps(FMantleVehicleSpawnProps& Out, FString& OutProblem) const
{
	Out.Vehicle = VehicleId;
	Out.StorageItem = StorageItem;
	Out.RespawnSeconds = RespawnSeconds;
	Out.bEnabled = bEnabled;
	Out.Livery = Livery;
	if (!FMath::IsFinite(RespawnSeconds) || RespawnSeconds < 0.0 || RespawnSeconds > FMantlePlaceLimits::MaxVehicleRespawnSeconds)
	{
		OutProblem = FString::Printf(TEXT("respawns after %g s; set Respawn Seconds between 0 and %g"),
			RespawnSeconds, FMantlePlaceLimits::MaxVehicleRespawnSeconds);
		return false;
	}
	if (Livery < -1 || Livery > FMantlePlaceLimits::MaxVehicleLiveryIndex)
	{
		OutProblem = FString::Printf(TEXT("wears livery %d; set Livery between -1 (the vehicle's default) and %d"),
			Livery, FMantlePlaceLimits::MaxVehicleLiveryIndex);
		return false;
	}
	return true;
}
