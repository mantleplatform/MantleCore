// Copyright Joshua Eger. All Rights Reserved.
#include "MantleSoundEmitterMarker.h"

#include "Components/SceneComponent.h"
#include "Sound/AmbientSound.h"

AMantleSoundEmitterMarker::AMantleSoundEmitterMarker()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = false;
	SetCanBeDamaged(false);
	SetActorHiddenInGame(true);

	MarkerRoot = CreateDefaultSubobject<USceneComponent>(TEXT("MarkerRoot"));
	SetRootComponent(MarkerRoot);
}

UClass* AMantleSoundEmitterMarker::ClassFor(const bool bDedicatedServer)
{
	return bDedicatedServer ? static_cast<UClass*>(AMantleSoundEmitterMarker::StaticClass()) : static_cast<UClass*>(AAmbientSound::StaticClass());
}
