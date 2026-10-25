// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "MantleSeatSpot.generated.h"

/** A seat's style (GhzGarage/MantleCore#142). It only picks the sit clips; v1 has one. */
UENUM()
enum class EMantleSeatSpotStyle : uint8
{
	/** Sitting on a bench, a chair-height seat or a ledge. */
	Bench
};

/**
 * A place a player or an NPC can sit (GhzGarage/MantleCore#142). Add one to any prop, one per sit spot: the component's
 * location is where the hip rests and its forward direction is the way the seated body faces (only its yaw counts).
 * The component is a marker for the editor; Export writes each one into the place's seats.
 */
UCLASS(ClassGroup = Mantle, meta = (BlueprintSpawnableComponent, DisplayName = "Mantle Seat"))
class MANTLEEDITOR_API UMantleSeatSpotComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	UMantleSeatSpotComponent();

	/** Which sit clips the seat uses. */
	UPROPERTY(EditAnywhere, Category = "Mantle")
	EMantleSeatSpotStyle Style = EMantleSeatSpotStyle::Bench;
};
