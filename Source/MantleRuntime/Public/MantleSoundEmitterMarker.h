// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "MantleSoundEmitterMarker.generated.h"

/**
 * What stands for a SoundEmitter place entity on a dedicated server. Nothing is heard on a server and the engine will not spawn an AmbientSound there (BaseEngine.ini
 * ClassesExcludedOnDedicatedServer), so without this the entity was missing for server scripts and its build was logged as a failure. A hidden, unreplicated actor at the
 * entity's transform with no audio: it exists for transforms, parenting, tags and scripts (Entity:Play and its siblings route to the clients, which have the real emitter).
 */
UCLASS(ClassGroup = Mantle, NotPlaceable, hidecategories = (Advanced, Cooking, HLOD, Input, LOD, Networking, Physics, Replication, Collision))
class MANTLERUNTIME_API AMantleSoundEmitterMarker : public AActor
{
	GENERATED_BODY()

public:
	AMantleSoundEmitterMarker();

	/** The actor class the place builder spawns for a SoundEmitter entity: this marker on a dedicated server, the engine's AmbientSound everywhere else. Pure. */
	static UClass* ClassFor(bool bDedicatedServer);

private:
	UPROPERTY()
	TObjectPtr<USceneComponent> MarkerRoot;
};
