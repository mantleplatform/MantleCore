// Copyright Joshua Eger. All Rights Reserved.
#include "MantleDecalBake.h"

#include "MantleRuntime.h"
#include "MantleGlbWriter.h"
#include "MantlePlaceAssetConverter.h"
#include "MantlePlaceAssets.h"

#include "Engine/Texture2D.h"
#include "RHIGlobals.h"
#include "RenderUtils.h"
#include "HAL/PlatformTime.h"
#include "Materials/MaterialInterface.h"
#include "Misc/SecureHash.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleDecalBakePrivate
{
	/** One kept result: the textures, or why there are none (an unbakeable material is not tried again until it changes). */
	struct FKept
	{
		TStrongObjectPtr<UTexture2D> BaseColor;
		TStrongObjectPtr<UTexture2D> Normal;
		FString Why;
	};

	TMap<FString, FKept>& Kept()
	{
		static TMap<FString, FKept> Results;
		return Results;
	}

	/** The session's decal bakes so far, and the time they took. */
	int32 BakeCount = 0;
	double BakeSeconds = 0.0;

	TSharedPtr<IMantleMaterialFlattener>& TestFlattener()
	{
		static TSharedPtr<IMantleMaterialFlattener> Flattener;
		return Flattener;
	}

	IMantleMaterialFlattener& Flattener()
	{
		if (TestFlattener().IsValid())
		{
			return *TestFlattener();
		}
		static FMantleGltfMaterialFlattener Gltf;
		return Gltf;
	}

	UTexture2D* NewTexture(const UMaterialInterface& Material, const TCHAR* What, const FString& Hash, const TArray<FColor>& Pixels, int32 Width, int32 Height)
	{
		UTexture2D* Texture = NewObject<UTexture2D>(GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UTexture2D::StaticClass(),
			FName(*FString::Printf(TEXT("%s_Decal%s_%s"), *Material.GetName(), What, *Hash))));
		Texture->Source.Init(Width, Height, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Pixels.GetData()));
		return Texture;
	}
}

bool FMantleDecalBake::Bake(const UMaterialInterface* Material, UTexture2D*& OutBaseColor, UTexture2D*& OutNormal, FString& OutWhy)
{
	using namespace MantleDecalBakePrivate;
	OutBaseColor = nullptr;
	OutNormal = nullptr;
	OutWhy.Reset();
	if (!Material)
	{
		OutWhy = TEXT("there is no material");
		return false;
	}
	IMantleMaterialFlattener& Engine = Flattener();
	if (!Engine.CanBakeTextures())
	{
		OutWhy = TEXT("baking it needs a renderer, and this editor has none");
		return false;
	}

	// Kept under the material's content: the material, everything its graph reads (instances, functions, textures), and the bake itself.
	const FMantlePlaceAssetLimits Limits = FMantlePlaceAssetLimits::FromContract();
	const FMantleMaterialAnalysis Analysis = FMantlePlaceAssets::AnalyzeMaterial(Material);
	TArray<const UObject*> Objects;
	Objects.Add(Material);
	for (const TWeakObjectPtr<UObject>& Dependency : Analysis.Dependencies)
	{
		Objects.Add(Dependency.Get());
	}
	// A1: under Substrate's adaptive GBuffer (the engine's default) the bake exports a translucent opacity flat: it is read through a Masked copy.
	const bool bAdaptiveGBuffer = Substrate::IsSubstrateEnabled() && !Substrate::IsSubstrateBlendableGBufferEnabled(GMaxRHIShaderPlatform);
	const bool bOpacityAsMask = FMantlePlaceAssets::BakesOpacityAsMask(Material, Analysis, bAdaptiveGBuffer);
	const FString Salt = FString::Printf(TEXT("decal shape 2 %s max=%d %s%s"), *Engine.Describe(), Limits.MaxTextureSize, *Material->GetPathName(),
		bOpacityAsMask ? TEXT(" opacityasmask=2") : TEXT(""));
	FString Key, WhyNot;
	if (!FMantlePlaceAssetConverter::ComputeCacheKey(Salt, Objects, Key, WhyNot))
	{
		Key.Reset(); // not keyable (an unsaved material): baked every time it is asked for
	}
	if (const FKept* Found = Key.IsEmpty() ? nullptr : Kept().Find(Key))
	{
		OutBaseColor = Found->BaseColor.Get();
		OutNormal = Found->Normal.Get();
		OutWhy = Found->Why;
		return OutBaseColor != nullptr;
	}
	auto Keep = [&Key, &OutBaseColor, &OutNormal, &OutWhy]()
	{
		if (!Key.IsEmpty())
		{
			FKept& Entry = Kept().Add(Key);
			Entry.BaseColor.Reset(OutBaseColor);
			Entry.Normal.Reset(OutNormal);
			Entry.Why = OutWhy;
		}
		return OutBaseColor != nullptr;
	};

	const double Started = FPlatformTime::Seconds();
	auto Count = [&Material, Started](const TCHAR* Outcome)
	{
		const double Took = FPlatformTime::Seconds() - Started;
		++BakeCount;
		BakeSeconds += Took;
		UE_LOG(LogMantle, Log, TEXT("MANTLE_DECAL_BAKE %s: %s in %.2f s (%d decal bake%s this session, %.1f s in all)"), *Material->GetName(), Outcome, Took,
			BakeCount, BakeCount == 1 ? TEXT("") : TEXT("s"), BakeSeconds);
	};

	const TStrongObjectPtr<UMaterialInterface> Surface = FMantlePlaceAssets::DecalSurfaceCopy(Material);
	if (!Surface.IsValid())
	{
		OutWhy = TEXT("it is not a decal material Mantle can copy (a dynamic material instance, or not Deferred Decal)");
		return Keep();
	}
	// Its base color's alpha is then the opacity, whatever the bake's alphaMode (BakedBaseColorImage reads the image).
	const TStrongObjectPtr<UMaterialInterface> OpacityMask = bOpacityAsMask ? FMantlePlaceAssets::OpacityAsMaskCopy(Surface.Get()) : TStrongObjectPtr<UMaterialInterface>();
	FMantleFlattenSettings Settings;
	Settings.MaxBakeSize = Limits.MaxTextureSize;
	FMantleGlbDocument Document;
	TArray<FString> Messages;
	FString Error;
	const bool bFlattened = Engine.Flatten(OpacityMask.IsValid() ? OpacityMask.Get() : Surface.Get(), Settings, Document, Messages, Error);
	TArray<FColor> Pixels;
	int32 Width = 0;
	int32 Height = 0;
	if (!bFlattened)
	{
		OutWhy = FString::Printf(TEXT("the engine could not bake it (%s)"), *Error);
		Count(TEXT("not baked"));
		return Keep();
	}
	if (!FMantlePlaceAssetConverter::BakedBaseColorImage(Document, Pixels, Width, Height, OutWhy))
	{
		Count(TEXT("no usable base color"));
		return Keep();
	}

	FMD5 Md5;
	const FString Seed = Key.IsEmpty() ? Material->GetPathName() : Key;
	const FTCHARToUTF8 Utf8(*Seed);
	Md5.Update(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	uint8 Digest[16];
	Md5.Final(Digest);
	const FString Hash = FString::Printf(TEXT("%02x%02x%02x%02x"), Digest[0], Digest[1], Digest[2], Digest[3]);
	OutBaseColor = NewTexture(*Material, TEXT("BaseColor"), Hash, Pixels, Width, Height);
	OutBaseColor->SRGB = true;

	// The normal as the material draws it. The bake's image is green-up (glTF), so the texture says so: the upload keeps it as it is
	// (SourceTexturePng flips only a texture not marked Flip Green Channel). A flat normal publishes none.
	bool bFlat = false;
	FString NormalWhy;
	TArray<FColor> NormalPixels;
	int32 NormalWidth = 0;
	int32 NormalHeight = 0;
	if (FMantlePlaceAssetConverter::BakedNormalImage(Document, NormalPixels, NormalWidth, NormalHeight, bFlat, NormalWhy))
	{
		OutNormal = NewTexture(*Material, TEXT("Normal"), Hash, NormalPixels, NormalWidth, NormalHeight);
		OutNormal->SRGB = false;
		OutNormal->CompressionSettings = TC_Normalmap;
		OutNormal->bFlipGreenChannel = true;
	}
	else if (!bFlat)
	{
		UE_LOG(LogMantle, Log, TEXT("MANTLE_DECAL_BAKE %s: its baked normal map is left out (%s)"), *Material->GetName(), *NormalWhy);
	}
	Count(*FString::Printf(TEXT("baked into a %dx%d decal texture%s"), Width, Height,
		OutNormal ? *FString::Printf(TEXT(" and a %dx%d normal map"), NormalWidth, NormalHeight) : TEXT("")));
	return Keep();
}

void FMantleDecalBake::SetFlattenerForTests(TSharedPtr<IMantleMaterialFlattener> Flattener)
{
	MantleDecalBakePrivate::TestFlattener() = Flattener;
	ClearCache();
}

void FMantleDecalBake::ClearCache()
{
	MantleDecalBakePrivate::Kept().Empty();
}
