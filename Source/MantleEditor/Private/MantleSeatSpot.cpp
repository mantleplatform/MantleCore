// Copyright Joshua Eger. All Rights Reserved.
#include "MantleSeatSpot.h"

UMantleSeatSpotComponent::UMantleSeatSpotComponent()
{
	// A marker only: the seat is data in the place, so nothing of the component ships.
	bIsEditorOnly = true;
	PrimaryComponentTick.bCanEverTick = false;
	// About a bench's hip height until the creator moves it.
	SetRelativeLocation(FVector(0.0, 0.0, 45.0));
}
