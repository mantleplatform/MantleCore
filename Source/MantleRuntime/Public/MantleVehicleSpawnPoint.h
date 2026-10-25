// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MantlePlaceTypes.h"

#include "MantleVehicleSpawnPoint.generated.h"

class UArrowComponent;
class UBillboardComponent;
class UMantleVehicleSource;
class UTextRenderComponent;

/**
 * Where a creator vehicle is parked (#435): the VehicleSpawn place entity. It only holds and shows data:
 * which uploaded vehicle, how soon it comes back, whether it is on. On the server (and in standalone play)
 * MantleGameplay's place vehicle bridge reads it from the retained place and keeps one vehicle alive at its
 * transform; clients build nothing from it (the vehicle replicates). Never replicated.
 *
 * The vehicle's +X faces the spawn's arrow.
 */
UCLASS(ClassGroup = Mantle, meta = (DisplayName = "Mantle Vehicle Spawn"),
	hidecategories = (Advanced, Cooking, HLOD, Input, LOD, Networking, Physics, Replication, Collision))
class MANTLERUNTIME_API AMantleVehicleSpawnPoint : public AActor
{
	GENERATED_BODY()

public:
	AMantleVehicleSpawnPoint();

	/**
	 * The vehicle you built or converted, to park here while you work on the level (Tools > Mantle > Build a vehicle > "Put it in
	 * my level" sets it). Publishing adds the vehicle to your library and the place's Storage and points this spawn at that item,
	 * so a published place always parks a library item. To park a vehicle already in your library, set Storage Item instead.
	 */
	UPROPERTY(EditAnywhere, Category = "Vehicle")
	TSoftObjectPtr<UMantleVehicleSource> Vehicle;

	/**
	 * The uploaded Vehicle asset behind a draft spawn: ast_u and 32 lowercase hex digits. Set by a place built from a draft
	 * file or an editor import; publishing never writes it (a published place names a Storage Item, #679).
	 */
	UPROPERTY(VisibleAnywhere, Category = "Vehicle")
	FString VehicleId;

	/**
	 * A vehicle held in the place's storage (#614 stage 4), by the Name of its entry in Mantle Place Settings >
	 * Storage, parked here instead of a Vehicle. Leave it empty while a Vehicle is set; publishing fills it in.
	 * Places that use it need a Mantle client from after #614 stage 4 to load. Add items to the storage with
	 * Tools > Mantle > Library. The editor shows the name above the spawn; the vehicle itself is never loaded here.
	 */
	UPROPERTY(EditAnywhere, Category = "Vehicle")
	FString StorageItem;

	/** Seconds after the vehicle is destroyed before a new one appears, 0 to 600. */
	UPROPERTY(EditAnywhere, Category = "Vehicle", meta = (ClampMin = "0", ClampMax = "600", UIMin = "0", UIMax = "600", Units = "s"))
	double RespawnSeconds = MANTLE_DEFAULT_VEHICLE_RESPAWN_SECONDS;

	/** Off keeps the spawn in the place without a vehicle (a script can still spawn one). */
	UPROPERTY(EditAnywhere, Category = "Vehicle")
	bool bEnabled = true;

	/**
	 * Which of the vehicle's liveries the parked car wears (#488): its index in the vehicle's Liveries list, the
	 * first being 0. -1 wears the vehicle's Default Livery (none when it has none).
	 */
	UPROPERTY(EditAnywhere, Category = "Vehicle", meta = (ClampMin = "-1", ClampMax = "7", UIMin = "-1", UIMax = "7"))
	int32 Livery = -1;

	/** Sets the actor from VehicleSpawn properties: the place builder's and the editor importer's. */
	void ApplyProps(const FMantleVehicleSpawnProps& In);

	/**
	 * Reads the actor into VehicleSpawn properties, VehicleId as the vehicle (the exporter replaces it with
	 * the converted vehicle's upload). False, with OutProblem phrased to follow the actor's name and saying
	 * what to fix, when a value is outside the schema's limits. Out is filled either way.
	 */
	bool ReadProps(FMantleVehicleSpawnProps& Out, FString& OutProblem) const;

	/**
	 * What the editor label under the arrow says: the Storage Item name between quotes, and a second line saying a stored
	 * vehicle stands here; empty when StorageItem is empty (no label shows). Pure, so a test can pin it.
	 */
	FString MakeEditorLabelText() const;

protected:
#if WITH_EDITOR
	virtual void OnConstruction(const FTransform& Transform) override;
	/** A level opened in the editor registers its actors without running their construction: the label is set here then. */
	virtual void PostRegisterAllComponents() override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	/**
	 * Sets the editor-only label from StorageItem. A vehicle held in the place's storage is another creator's item that
	 * is never loaded into the project, so the spawn shows its name, not a mesh. Does nothing in a game build.
	 */
	void UpdateEditorLabel();

	UPROPERTY(VisibleAnywhere, Category = "Vehicle")
	TObjectPtr<USceneComponent> SpawnRoot;

#if WITH_EDITORONLY_DATA
	UPROPERTY()
	TObjectPtr<UArrowComponent> EditorArrow;

	UPROPERTY()
	TObjectPtr<UBillboardComponent> EditorBillboard;

	/** The Storage Item label, readable from the vehicle's front and from behind (a text render faces one way). */
	UPROPERTY()
	TObjectPtr<UTextRenderComponent> EditorLabelFront;

	UPROPERTY()
	TObjectPtr<UTextRenderComponent> EditorLabelBack;
#endif
};
