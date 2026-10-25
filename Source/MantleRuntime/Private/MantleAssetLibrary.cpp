// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAssetLibrary.h"
#include "MantlePlaceAssets.h"
#include "MantleRuntime.h"
#include "MantleSoundWave.h"
#include "MantleTrace.h"

#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Materials/MaterialInterface.h"
#include "Sound/SoundBase.h"
#include "UObject/Package.h"

namespace
{
	const TCHAR* AssetKindName(EMantleAssetKind Kind)
	{
		switch (Kind)
		{
		case EMantleAssetKind::StaticMesh: return TEXT("StaticMesh");
		case EMantleAssetKind::Material:   return TEXT("Material");
		case EMantleAssetKind::Texture:    return TEXT("Texture");
		case EMantleAssetKind::Sound:      return TEXT("Sound");
		case EMantleAssetKind::SkeletalMesh: return TEXT("SkeletalMesh");
		case EMantleAssetKind::AnimClip:   return TEXT("AnimClip");
		case EMantleAssetKind::Heightfield: return TEXT("Heightfield");
		case EMantleAssetKind::Vehicle:    return TEXT("Vehicle");
		case EMantleAssetKind::Weapon:     return TEXT("Weapon");
		default:                           return TEXT("Unknown");
		}
	}
}

FMantleAssetLibrary::FMantleAssetLibrary()
{
	// The starter library. Engine primitives only, enough to build and reconstruct a place without authoring or cooking anything.
	//
	// These starter IDs are permanent. A published place referencing ast_cube must still resolve
	// years from now, so entries are added here but never renamed or removed.
	const auto Add = [this](const TCHAR* Id, EMantleAssetKind Kind, const TCHAR* Path)
	{
		Entries.Add(Id, FMantleAssetEntry{ Kind, FSoftObjectPath(Path) });
	};

	Add(TEXT("ast_cube"),      EMantleAssetKind::StaticMesh, TEXT("/Engine/BasicShapes/Cube.Cube"));
	Add(TEXT("ast_sphere"),    EMantleAssetKind::StaticMesh, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	Add(TEXT("ast_cylinder"),  EMantleAssetKind::StaticMesh, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	Add(TEXT("ast_cone"),      EMantleAssetKind::StaticMesh, TEXT("/Engine/BasicShapes/Cone.Cone"));
	Add(TEXT("ast_plane"),     EMantleAssetKind::StaticMesh, TEXT("/Engine/BasicShapes/Plane.Plane"));

	Add(TEXT("ast_mat_basic"), EMantleAssetKind::Material,
		TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));

	// No sounds (#263): Mantle ships no preset audio, ever. Creators upload their own with their
	// place (#324), and LoadSound answers those uploaded ids from the live place asset set.
}

FMantleAssetLibrary& FMantleAssetLibrary::Get()
{
	static FMantleAssetLibrary Instance;
	return Instance;
}

bool FMantleAssetLibrary::IsKnown(const FString& AssetId) const
{
	return Entries.Contains(AssetId);
}

const FMantleAssetEntry* FMantleAssetLibrary::Find(const FString& AssetId) const
{
	return Entries.Find(AssetId);
}

FString FMantleAssetLibrary::FindIdByPath(const FSoftObjectPath& Path) const
{
	for (const TPair<FString, FMantleAssetEntry>& Pair : Entries)
	{
		if (Pair.Value.Path == Path)
		{
			return Pair.Key;
		}
	}
	return FString();
}

UObject* FMantleAssetLibrary::LoadChecked(const FString& AssetId, EMantleAssetKind Expected, FString& OutError) const
{
	const FMantleAssetEntry* Entry = Entries.Find(AssetId);
	if (!Entry)
	{
		OutError = FString::Printf(TEXT("'%s' is not a known asset"), *AssetId);
		return nullptr;
	}

	if (Entry->Kind != Expected)
	{
		OutError = FString::Printf(
			TEXT("'%s' is a %s, not a %s"),
			*AssetId,
			AssetKindName(Entry->Kind),
			AssetKindName(Expected));
		return nullptr;
	}

	// The synchronous game-thread load a place build pays per library asset (#126). The static scope
	// aggregates in Insights; the detail scope names the asset.
	UObject* Loaded = nullptr;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(MantleAssetLibrary_TryLoad);
		MANTLE_TRACE_SCOPE_DETAIL("MantleAssetLoad", Entry->Path.ToString());
		Loaded = Entry->Path.TryLoad();
	}
	if (!Loaded)
	{
		OutError = FString::Printf(
			TEXT("'%s' could not be loaded from '%s'"),
			*AssetId,
			*Entry->Path.ToString());
		return nullptr;
	}

	return Loaded;
}

UStaticMesh* FMantleAssetLibrary::LoadMesh(const FString& AssetId, FString& OutError) const
{
	// Uploaded ids (F8, #342) were resolved, downloaded and built before the place was: the live
	// place asset set answers them, never the built-in table.
	if (FMantlePlaceAssets::IsUploadedId(AssetId))
	{
		return FMantlePlaceAssets::FindLiveMesh(AssetId, OutError);
	}

	UObject* Loaded = LoadChecked(AssetId, EMantleAssetKind::StaticMesh, OutError);
	if (!Loaded)
	{
		return nullptr;
	}

	UStaticMesh* Mesh = Cast<UStaticMesh>(Loaded);
	if (!Mesh)
	{
		OutError = FString::Printf(TEXT("'%s' did not load as a static mesh"), *AssetId);
	}
	return Mesh;
}

UMaterialInterface* FMantleAssetLibrary::LoadMaterial(const FString& AssetId, FString& OutError) const
{
	if (FMantlePlaceAssets::IsUploadedId(AssetId))
	{
		return FMantlePlaceAssets::FindLiveMaterial(AssetId, OutError);
	}

	UObject* Loaded = LoadChecked(AssetId, EMantleAssetKind::Material, OutError);
	if (!Loaded)
	{
		return nullptr;
	}

	UMaterialInterface* Material = Cast<UMaterialInterface>(Loaded);
	if (!Material)
	{
		OutError = FString::Printf(TEXT("'%s' did not load as a material"), *AssetId);
	}
	return Material;
}

UTexture* FMantleAssetLibrary::LoadTexture(const FString& AssetId, const EMantleTextureRole Role, FString& OutError) const
{
	if (FMantlePlaceAssets::IsUploadedId(AssetId))
	{
		return FMantlePlaceAssets::FindLiveTexture(AssetId, Role, OutError);
	}

	UObject* Loaded = LoadChecked(AssetId, EMantleAssetKind::Texture, OutError);
	if (!Loaded)
	{
		return nullptr;
	}

	UTexture* Texture = Cast<UTexture>(Loaded);
	if (!Texture)
	{
		OutError = FString::Printf(TEXT("'%s' did not load as a texture"), *AssetId);
	}
	return Texture;
}

USoundBase* FMantleAssetLibrary::LoadSound(const FString& AssetId, FString& OutError) const
{
	// A creator's uploaded sound (#324), decoded by the live place asset set: a new one-shot wave
	// over its samples on each call, since a procedural wave's read position is its player's own.
	// An id this table knows is loaded from it, as before #324, whatever its shape.
	if (FMantlePlaceAssets::IsUploadedSoundId(AssetId))
	{
		const TSharedPtr<const FMantleSoundData> Sound = FMantlePlaceAssets::FindLiveSound(AssetId, OutError);
		return Sound.IsValid() ? UMantleSoundWave::Create(GetTransientPackage(), Sound, /*bLoop*/ false) : nullptr;
	}

	UObject* Loaded = LoadChecked(AssetId, EMantleAssetKind::Sound, OutError);
	if (!Loaded)
	{
		return nullptr;
	}

	USoundBase* Sound = Cast<USoundBase>(Loaded);
	if (!Sound)
	{
		OutError = FString::Printf(TEXT("'%s' did not load as a sound"), *AssetId);
	}
	return Sound;
}
