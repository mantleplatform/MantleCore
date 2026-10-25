// Copyright Joshua Eger. All Rights Reserved.
#include "MantleClothingColorPreview.h"
#include "MantleClothingMaterial.h"
#include "ImageUtils.h"
#include "ImageCore.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/Package.h"

/** PNG data/normal channels retain encoded bytes regardless of the decoder's inferred gamma. */
static bool DecodeClothingPreviewPixels(TConstArrayView<uint8> Bytes, bool bData, TArray<FColor>& Pixels, FIntPoint& Size)
{
	FImage Image;
	if (!FImageUtils::DecompressImage(Bytes.GetData(), Bytes.Num(), Image)) return false;
	if (bData) Image.GammaSpace = EGammaSpace::Linear;
	FImage Bgra; Image.CopyTo(Bgra, ERawImageFormat::BGRA8, bData ? EGammaSpace::Linear : EGammaSpace::sRGB);
	const TArrayView64<FColor> View = Bgra.AsBGRA8();
	Pixels = TArray<FColor>(View.GetData(), static_cast<int32>(View.Num())); Size = {Bgra.SizeX, Bgra.SizeY};
	return Pixels.Num() > 0;
}

bool MakeClothingColorPreviewMaterials(const FMantleCosmeticPackage& Package,
	TConstArrayView<FString> ExportMaterialNames, TConstArrayView<FMantleClothingColor> Colors,
	TArray<FSkeletalMaterial>& Materials, FString& OutError)
{
	if (Package.Materials.Num() != ExportMaterialNames.Num()) { OutError = TEXT("Preview/export material ordering differs."); return false; }
	auto Texture = [&Package, &OutError](const FString& Key, bool bData, bool bNormal) -> UTexture2D*
	{
		const FMantleCosmeticUploadAsset* Asset = Package.FindAsset(Key);
		TArray<FColor> Pixels; FIntPoint Size;
		if (!Asset || !DecodeClothingPreviewPixels(Asset->Bytes, bData, Pixels, Size))
		{ OutError = TEXT("Preview could not decode exported texture ") + Key; return nullptr; }
		FCreateTexture2DParameters Parameters; Parameters.bSRGB = !bData; Parameters.bUseAlpha = true;
		Parameters.CompressionSettings = bNormal ? TC_Normalmap : bData ? TC_Masks : TC_Default;
		Parameters.TextureGroup = bNormal ? TEXTUREGROUP_CharacterNormalMap : TEXTUREGROUP_Character;
		return FImageUtils::CreateTexture2D(Size.X, Size.Y, Pixels, GetTransientPackage(),
			MakeUniqueObjectName(GetTransientPackage(), UTexture2D::StaticClass(), TEXT("MantleClothingPreviewTexture")).ToString(), RF_Transient, Parameters);
	};
	for (int32 Index = 0; Index < Package.Materials.Num(); ++Index)
	{
		const FMantleCosmeticMaterialRef& Ref = Package.Materials[Index];
		if (Ref.ColorRegions.IsEmpty()) continue;
		int32 PreviewIndex = INDEX_NONE;
		for (int32 Slot = 0; Slot < Materials.Num(); ++Slot)
		{
			const FSkeletalMaterial& Existing = Materials[Slot];
			const FString Name = !Existing.MaterialSlotName.IsNone() ? Existing.MaterialSlotName.ToString() : Existing.MaterialInterface ? Existing.MaterialInterface->GetName() : FString::Printf(TEXT("Material%d"), Slot);
			if (Name == ExportMaterialNames[Index]) { PreviewIndex = Slot; break; }
		}
		if (PreviewIndex == INDEX_NONE) { OutError = TEXT("A declared color material is absent from the preview."); return false; }
		// The garment color material is the game's: a creator project has none, so its preview is a plain engine
		// material. Test locally shows the real colors.
		UMaterialInterface* Parent = FMantleClothingMaterial::Parent();
		if (!Parent) Parent = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
		UTexture2D* Albedo = Texture(Ref.AlbedoKey, false, false);
		UTexture2D* Mask = Texture(Ref.ColorMaskKey, true, false);
		UTexture2D* Normal = Ref.NormalKey.IsEmpty() ? nullptr : Texture(Ref.NormalKey, true, true);
		if (!Parent || !Albedo || !Mask || (!Ref.NormalKey.IsEmpty() && !Normal))
		{ if (OutError.IsEmpty()) OutError = TEXT("The fixed clothing color material or preview textures are unavailable."); return false; }
		UMaterialInstanceDynamic* Material = UMaterialInstanceDynamic::Create(Parent, GetTransientPackage());
		Material->SetTextureParameterValue(TEXT("Albedo"), Albedo);
		Material->SetTextureParameterValue(TEXT("DyeMask"), Mask);
		if (Normal) Material->SetTextureParameterValue(TEXT("Normal"), Normal);
		FMantleClothingMaterial::Apply(*Material, Ref.ColorRegions, Colors);
		Materials[PreviewIndex].MaterialInterface = Material;
	}
	return true;
}
