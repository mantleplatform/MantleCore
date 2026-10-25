// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantlePlaceTypes.h"
#include "MantleRuntimeTextureBuilder.h"
#include "UObject/SoftObjectPath.h"

class UStaticMesh;
class UMaterialInterface;
class USoundBase;
class UTexture;

struct FMantleAssetEntry
{
	EMantleAssetKind Kind = EMantleAssetKind::StaticMesh;
	FSoftObjectPath Path;
};

/**
 * Resolves place-file asset IDs to loadable objects.
 *
 * Today every ID maps to content shipped with the build. This class is deliberately the
 * only place that knows that: when assets begin streaming from the CDN, the lookup is
 * replaced here and nothing else in the runtime changes.
 *
 * Unknown IDs are an error, never a silent fallback — a place referencing something that
 * cannot be resolved is broken, and saying so plainly is more useful than quietly
 * substituting a cube.
 */
class MANTLERUNTIME_API FMantleAssetLibrary
{
public:
	static FMantleAssetLibrary& Get();

	bool IsKnown(const FString& AssetId) const;

	const FMantleAssetEntry* Find(const FString& AssetId) const;

	/**
	 * Reverse lookup: which asset ID refers to this object path.
	 * Used by the editor plugin to turn a level's asset references back into IDs on export.
	 * Returns an empty string when the object is not part of the library, which is how the
	 * exporter detects content a creator cannot publish yet.
	 */
	FString FindIdByPath(const FSoftObjectPath& Path) const;

	/**
	 * Returns nullptr and fills OutError when the ID is unknown, the wrong kind, or fails to load.
	 * Uploaded ids (ast_u...) are answered by the newest live FMantlePlaceAssets set that prepared
	 * them (Fab F8, #342), with its reason when that asset failed; IsKnown, Find and
	 * FindIdByPath stay the built-in table's. An uploaded sound (#324) comes back as a new one-shot
	 * UMantleSoundWave over its decoded samples, or an error while it is still loading; the place
	 * builder binds its emitters through FMantlePlaceAssets itself, so a sound never delays a join.
	 */
	UStaticMesh* LoadMesh(const FString& AssetId, FString& OutError) const;
	UMaterialInterface* LoadMaterial(const FString& AssetId, FString& OutError) const;
	USoundBase* LoadSound(const FString& AssetId, FString& OutError) const;

	/**
	 * A texture an entity names directly (a Decal's, #472), read with Role: Color for a base colour,
	 * Normal for a normal map. An uploaded id is answered by the live set that prepared it in that
	 * role; the built-in table has no textures today, so any other id is an error.
	 */
	UTexture* LoadTexture(const FString& AssetId, EMantleTextureRole Role, FString& OutError) const;

private:
	FMantleAssetLibrary();

	UObject* LoadChecked(const FString& AssetId, EMantleAssetKind Expected, FString& OutError) const;

	TMap<FString, FMantleAssetEntry> Entries;
};
