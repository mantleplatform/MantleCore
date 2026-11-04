// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MantlePlaceTypes.h"

#include "MantleItemSpawnPoint.generated.h"

class UArrowComponent;
class UBillboardComponent;

/**
 * Where an item waits to be picked up and comes back after (#235): the ItemSpawn place entity. It only holds data: which item, how many,
 * how soon it comes back, an optional model, whether it is on. MantleGameplay's world items own the item itself (a record, not an actor);
 * this actor is hidden in game, never replicated, and carries nothing a client draws.
 */
UCLASS(ClassGroup = Mantle, meta = (DisplayName = "Mantle Item Spawn"),
	hidecategories = (Advanced, Cooking, HLOD, Input, LOD, Networking, Physics, Replication, Collision))
class MANTLERUNTIME_API AMantleItemSpawnPoint : public AActor
{
	GENERATED_BODY()

public:
	AMantleItemSpawnPoint();

	/** The item that waits here: an id a script defines with WorldItems:Define (lowercase letters, digits and underscores, up to 48), or a stored weapon's library id. */
	UPROPERTY(EditAnywhere, Category = "Item")
	FString Item;

	/** How many of the item the stack holds, 1 to 9999 (held to the item's MaxStack when it is made). */
	UPROPERTY(EditAnywhere, Category = "Item", meta = (ClampMin = "1", ClampMax = "9999", UIMin = "1", UIMax = "100"))
	int32 Count = 1;

	/** How many seconds after a player takes the item before a new one appears, 5 to 86400. */
	UPROPERTY(EditAnywhere, Category = "Item", meta = (ClampMin = "5", ClampMax = "86400", UIMin = "5", UIMax = "600", Units = "s"))
	double RespawnSeconds = MANTLE_DEFAULT_ITEM_SPAWN_RESPAWN_SECONDS;

	/** The place asset id (ast_...) of a StaticMesh the item lies as instead of the model its script gave it. Leave empty for the item's own. Set by the exporter from a mesh. */
	UPROPERTY(EditAnywhere, Category = "Item")
	FString ModelId;

	/** Untick to keep the spawn in your place without an item (a script can turn it on with WorldItems:SetSpawnEnabled). */
	UPROPERTY(EditAnywhere, Category = "Item")
	bool bEnabled = true;

	/** Sets the actor from ItemSpawn properties: the place builder's. */
	void ApplyProps(const FMantleItemSpawnProps& In);

	/**
	 * Reads the actor into ItemSpawn properties. False, with OutProblem phrased to follow the actor's name, when a value is outside
	 * the schema's limits. Out is filled either way.
	 */
	bool ReadProps(FMantleItemSpawnProps& Out, FString& OutProblem) const;

private:
	UPROPERTY(VisibleAnywhere, Category = "Item")
	TObjectPtr<USceneComponent> SpawnRoot;

#if WITH_EDITORONLY_DATA
	UPROPERTY()
	TObjectPtr<UArrowComponent> EditorArrow;

	UPROPERTY()
	TObjectPtr<UBillboardComponent> EditorBillboard;
#endif
};
