// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceUploadShape.h"
#include "MantleMeshLumenData.h"
#include "MantleNaniteSideFile.h"
#include "MantlePlaceAssetConverter.h"
#include "MantlePlaceAssets.h"
#include "MantlePlaceMaterial.h"
#include "Engine/StaticMesh.h"
#include "MaterialShared.h"
#include "Materials/MaterialInterface.h"
#include "StaticMeshResources.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantlePlaceUploadShapePrivate
{
	/** The most an upload of this kind may be: its size field at its longest. */
	int64 LargestBytes(const EMantleAssetKind Kind, const FMantlePlaceAssetLimits& Limits)
	{
		switch (Kind)
		{
		case EMantleAssetKind::Texture: return FMath::Max(Limits.MaxTextureBytes, Limits.MaxUploadBytes);
		case EMantleAssetKind::Lumen: return FMantleMeshLumenFile::MaxBytes;
		case EMantleAssetKind::Nanite: return Limits.MaxNaniteBytes;
		default: return Limits.MaxUploadBytes;
		}
	}

	FMantleAssetRef Uploaded(const FString& Id, const EMantleAssetKind Kind, const FMantlePlaceAssetLimits& Limits)
	{
		FMantleAssetRef Ref;
		Ref.Id = Id;
		Ref.Kind = Kind;
		Ref.Hash = FMantlePlaceUploadShape::UploadedHashShape();
		Ref.Bytes = LargestBytes(Kind, Limits);
		return Ref;
	}
}

FMantleExpectedSideFiles FMantlePlaceUploadShape::ExpectedSideFiles(const UStaticMesh* Mesh, const bool bCollisionOnly)
{
	FMantleExpectedSideFiles Expected;
	if (bCollisionOnly)
	{
		Expected.bLumen = false;
		Expected.bNanite = false;
		return Expected;
	}
	if (!Mesh)
	{
		return Expected;
	}
	// The slots it draws: its first LOD's sections' (every slot when it has no render data).
	const TArray<FStaticMaterial>& Materials = Mesh->GetStaticMaterials();
	TArray<int32> Drawn;
	const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
	if (RenderData && RenderData->LODResources.Num() > 0)
	{
		for (const FStaticMeshSection& Section : RenderData->LODResources[0].Sections)
		{
			Drawn.AddUnique(Section.MaterialIndex);
		}
	}
	else
	{
		for (int32 Slot = 0; Slot < Materials.Num(); ++Slot)
		{
			Drawn.Add(Slot);
		}
	}
	int32 SeeThrough = 0;
	for (const int32 Slot : Drawn)
	{
		const UMaterialInterface* Material = Materials.IsValidIndex(Slot) ? Materials[Slot].MaterialInterface.Get() : nullptr;
		SeeThrough += Material && IsTranslucentBlendMode(Material->GetBlendMode()) ? 1 : 0;
	}
	Expected.bNanite = SeeThrough == 0;
	Expected.bLumen = Drawn.Num() == 0 || SeeThrough < Drawn.Num();
	return Expected;
}

FString FMantlePlaceUploadShape::UploadedHashShape()
{
	return TEXT("sha256:") + FString::ChrN(64, TEXT('f'));
}

FMantlePlaceUploadShape FMantlePlaceUploadShape::Project(const TArray<FMantleAssetRef>& Assets, const TMap<FString, FString>& ProvisionalKeys,
	const FMantlePlaceAssetSet* Converted, const FMantlePlaceAssetLimits& Limits,
	const TFunctionRef<FMantleExpectedSideFiles(const FMantleAssetRef& Mesh, const FString& Key)> ExpectedSideFiles)
{
	using namespace MantlePlaceUploadShapePrivate;

	// The keys converted already, looked up once each (FindMesh and FindMaterial walk their lists; City Sample has 11,739 meshes).
	TSet<FString> ConvertedMeshes;
	TSet<FString> ConvertedMaterials;
	if (Converted)
	{
		for (const FMantleConvertedMesh& Mesh : Converted->Meshes)
		{
			ConvertedMeshes.Add(Mesh.Key);
		}
		for (const FMantleConvertedMaterial& Material : Converted->Materials)
		{
			ConvertedMaterials.Add(Material.Key);
		}
	}

	FMantlePlaceUploadShape Shape;
	Shape.Assets.Reserve(Assets.Num() * 2);
	for (const FMantleAssetRef& Asset : Assets)
	{
		const FString* Key = ProvisionalKeys.Find(Asset.Id);
		if (!Key)
		{
			// A library asset, an id the level names as it is, or one already uploaded: listed as it is now.
			Shape.Assets.Add(Asset);
			continue;
		}
		const int32 Index = Shape.Assets.Add(Asset);
		Shape.Assets[Index].Hash = UploadedHashShape();
		Shape.Assets[Index].Bytes = LargestBytes(Asset.Kind, Limits);

		if (Asset.Kind == EMantleAssetKind::StaticMesh && !ConvertedMeshes.Contains(*Key))
		{
			// Its Lumen and Nanite files, listed after it and named by it as AddLumen and AddNanite list a converted mesh's.
			// (By index: adding to Shape.Assets may move it.)
			const FMantleExpectedSideFiles Expected = ExpectedSideFiles(Asset, *Key);
			FMantleConvertedMesh Keys;
			Keys.Key = *Key;
			if (Expected.bLumen && Asset.Lumen.IsEmpty())
			{
				FMantleAssetRef Lumen = Uploaded(FMantlePlaceAssets::ProvisionalId(Keys.LumenKey()), EMantleAssetKind::Lumen, Limits);
				Lumen.Engine = FMantleMeshLumenFile::EngineTag();
				Shape.Assets[Index].Lumen = Lumen.Id;
				Shape.Assets.Add(MoveTemp(Lumen));
				++Shape.AddedLumen;
			}
			if (Expected.bNanite && Asset.Nanite.IsEmpty())
			{
				FMantleAssetRef Nanite = Uploaded(FMantlePlaceAssets::ProvisionalId(Keys.NaniteKey()), EMantleAssetKind::Nanite, Limits);
				Nanite.Engine = FMantleNaniteSideFile::EngineTag();
				Shape.Assets[Index].Nanite = Nanite.Id;
				Shape.Assets.Add(MoveTemp(Nanite));
				++Shape.AddedNanite;
			}
		}
		else if (Asset.Kind == EMantleAssetKind::Material && !ConvertedMaterials.Contains(*Key))
		{
			// Its textures: at most one for each of the document's slots (base color, normal, occlusion-roughness-metallic, emissive).
			for (int32 Slot = 0; Slot < MantlePlaceTextureSlotCount; ++Slot)
			{
				Shape.Assets.Add(Uploaded(FMantlePlaceAssets::ProvisionalId(FString::Printf(TEXT("%s#texture%d"), **Key, Slot)), EMantleAssetKind::Texture, Limits));
				++Shape.AddedTextures;
			}
		}
	}
	return Shape;
}
