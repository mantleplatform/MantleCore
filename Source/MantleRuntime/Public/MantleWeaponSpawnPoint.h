// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MantlePlaceTypes.h"

#include "MantleWeaponSpawnPoint.generated.h"

class UArrowComponent;
class UBillboardComponent;
class USkeletalMeshComponent;

/**
 * Where a creator weapon waits to be picked up (#789): the WeaponSpawn place entity. It only holds data:
 * which uploaded weapon, how soon it comes back, whether it is on. MantleGameplay owns the pickup (a later
 * stage); this actor is hidden in game, never replicated, and carries nothing a client draws.
 *
 * In the editor a creator picks one of the weapons they built with Tools > Mantle > Build a weapon (Weapon), and the
 * gun shows on the spawn so the level reads at a glance. Publishing uploads that weapon with the place.
 */
UCLASS(ClassGroup = Mantle, meta = (DisplayName = "Mantle Weapon Spawn"),
	hidecategories = (Advanced, Cooking, HLOD, Input, LOD, Networking, Physics, Replication, Collision))
class MANTLERUNTIME_API AMantleWeaponSpawnPoint : public AActor
{
	GENERATED_BODY()

public:
	AMantleWeaponSpawnPoint();

	/**
	 * The weapon that waits here: one you made with Tools > Mantle > Build a weapon. The list shows every weapon in your
	 * project's MantleWeapons folder. Publishing sends it with your place.
	 */
	UPROPERTY(EditAnywhere, Category = "Weapon", meta = (GetOptions = "GetWeaponOptions"))
	FString Weapon;

	/** The uploaded Weapon asset waiting here: ast_u and 32 lowercase hex digits. Set from the place file. */
	UPROPERTY(VisibleAnywhere, Category = "Weapon")
	FString WeaponId;

	/**
	 * A weapon held in the place's storage (Tools > Mantle > Library), by the Name of its entry in Mantle Place Settings >
	 * Storage, instead of a Weapon. Leave it empty to use a weapon you built.
	 */
	UPROPERTY(EditAnywhere, Category = "Weapon")
	FString StorageItem;

	/** How many seconds after a player picks the weapon up before a new one appears, 0 to 600. */
	UPROPERTY(EditAnywhere, Category = "Weapon", meta = (ClampMin = "0", ClampMax = "600", UIMin = "0", UIMax = "600", Units = "s"))
	double RespawnSeconds = MANTLE_DEFAULT_WEAPON_RESPAWN_SECONDS;

	/** Untick to keep the spawn in your place without a weapon (a script can still use it). */
	UPROPERTY(EditAnywhere, Category = "Weapon")
	bool bEnabled = true;

	/** Sets the actor from WeaponSpawn properties: the place builder's. */
	void ApplyProps(const FMantleWeaponSpawnProps& In);

	/** The names of the weapons the weapon builder made in this project (the Weapon list): each folder of Content/MantleWeapons with its settings file and 3D model. Empty in a game build. */
	UFUNCTION()
	TArray<FString> GetWeaponOptions() const;

	/** True for a name the weapon builder could have made: letters, digits and underscores, so it can never leave the MantleWeapons folder. */
	static bool IsBuilderName(const FString& Name);

	/** The object path of a built weapon's 3D model: /Game/MantleWeapons/<Name>/SKM_<Name>.SKM_<Name>. */
	static FString BuiltMeshObjectPath(const FString& Name);

	/** The built weapon's settings file on disk: Content/MantleWeapons/<Name>/<Name>.weapon.json. */
	static FString BuiltDocumentFile(const FString& Name);

	/**
	 * Reads the actor into WeaponSpawn properties. False, with OutProblem phrased to follow the actor's
	 * name, when a value is outside the schema's limits. Out is filled either way.
	 */
	bool ReadProps(FMantleWeaponSpawnProps& Out, FString& OutProblem) const;

protected:
#if WITH_EDITOR
	virtual void OnConstruction(const FTransform& Transform) override;
	/** A level opened in the editor registers its actors without running their construction: the gun is shown then too. */
	virtual void PostRegisterAllComponents() override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	/** Shows the chosen weapon's 3D model on the spawn (editor only; nothing in a game build). */
	void UpdateEditorPreview();

	UPROPERTY(VisibleAnywhere, Category = "Weapon")
	TObjectPtr<USceneComponent> SpawnRoot;

#if WITH_EDITORONLY_DATA
	UPROPERTY()
	TObjectPtr<UArrowComponent> EditorArrow;

	UPROPERTY()
	TObjectPtr<UBillboardComponent> EditorBillboard;

	/** The chosen weapon's 3D model, so the spawn shows the gun in the editor. Never in a game. */
	UPROPERTY()
	TObjectPtr<USkeletalMeshComponent> EditorGun;
#endif
};
