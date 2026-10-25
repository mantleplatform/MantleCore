// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "MantleWeaponAsset.generated.h"

/**
 * An uploaded Weapon asset (#789 stage 4), read: the MWP1 the cook stored. MWP1 is the weapon document
 * (schema/weapon.schema.json) as compact UTF-8 JSON, the backend's canonical copy (cook.WeaponFormat); there
 * is no header. The document names its skeletal mesh, attachment meshes, shot sounds and slot materials by
 * uploaded asset id, which is what the lists below hold. Reading is a pure function, safe off the game thread.
 */
struct MANTLERUNTIME_API FMantleWeaponAsset
{
	/** The largest MWP1 read (cook.MaxWeaponDocumentBytes). */
	static constexpr int32 MaxBytes = 64 * 1024;

	/** The document, as the text the strict reader (FMantleWeaponDocument) and the document loader parse. */
	FString Text;

	/** The weapon's display name. */
	FString Name;

	/** The weapon's skeletal mesh id; every weapon has one (#897: a fist weapon's too). A shield is its own weapon, so its mesh
	 *  is this one (#840 decision 4). */
	FString Mesh;

	/** Attachment mesh ids (a mesh or a skeletal mesh), each once; #905: the projectile's mesh (projectile.mesh) is one too. */
	TArray<FString> Attachments;

	/** Shot sound ids, each once. */
	TArray<FString> Sounds;

	/** The hotbar icon's texture id, "" when the document names none. */
	FString Icon;

	/** The mesh slots' material ids in slot order, "" for a slot the document leaves; Num() is 0 with none. */
	TArray<FString> Materials;

	/**
	 * #898: an attachment mesh id's slot material ids in slot order (attachments[].materials), for each attachment that names
	 * them (#905: and projectile.materials for projectile.mesh). The materials go on the mesh itself, as the weapon's own do, so a mesh
	 * two attachments share takes the first one's.
	 */
	TMap<FString, TArray<FString>> AttachmentMaterials;

	/** Every distinct asset id the document names (mesh, attachments, sounds, materials, attachments' materials, icon). */
	TArray<FString> NamedIds() const;

	/** Reads MWP1 bytes with the strict document reader. False with every problem in OutError. */
	static bool Read(TConstArrayView<uint8> Bytes, FMantleWeaponAsset& Out, FString& OutError);
};

/**
 * What FMantlePlaceAssets keeps for an uploaded weapon id a place uses: the read MWP1. MantleGameplay builds the
 * weapon definition from it (the asset layer holds no gameplay types).
 */
UCLASS(Transient)
class MANTLERUNTIME_API UMantleBuiltWeapon : public UObject
{
	GENERATED_BODY()

public:
	FMantleWeaponAsset Asset;
};
