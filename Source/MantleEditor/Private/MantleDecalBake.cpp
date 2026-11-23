// Copyright Joshua Eger. All Rights Reserved.
#include "MantleDecalBake.h"

#include "MantleRuntime.h"
#include "MantleGlbWriter.h"
#include "MantlePlaceAssetConverter.h"
#include "MantlePlaceAssets.h"

#include "Engine/Texture2D.h"
#include "Materials/MaterialInterface.h"
#include "Misc/SecureHash.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleDecalBakePrivate
{
	/** One kept result: the texture, or why there is none (an unbakeable material is not tried again until it changes). */
	struct FKept
	{
		TStrongObjectPtr<UTexture2D> Texture;
		FString Why;
	};

	TMap<FString, FKept>& Kept()
	{
		static TMap<FString, FKept> Results;
		return Results;
	}

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
}

UTexture2D* FMantleDecalBake::BakeShape(const UMaterialInterface* Material, FString& OutWhy)
{
	using namespace MantleDecalBakePrivate;
	OutWhy.Reset();
	if (!Material)
	{
		OutWhy = TEXT("there is no material");
		return nullptr;
	}
	IMantleMaterialFlattener& Bake = Flattener();
	if (!Bake.CanBakeTextures())
	{
		OutWhy = TEXT("baking it needs a renderer, and this editor has none");
		return nullptr;
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
	const FString Salt = FString::Printf(TEXT("decal shape 1 %s max=%d %s"), *Bake.Describe(), Limits.MaxTextureSize, *Material->GetPathName());
	FString Key, WhyNot;
	if (!FMantlePlaceAssetConverter::ComputeCacheKey(Salt, Objects, Key, WhyNot))
	{
		Key.Reset(); // not keyable (an unsaved material): baked every time it is asked for
	}
	if (const FKept* Found = Key.IsEmpty() ? nullptr : Kept().Find(Key))
	{
		OutWhy = Found->Why;
		return Found->Texture.Get();
	}
	auto Keep = [&Key](UTexture2D* Texture, const FString& Why)
	{
		if (!Key.IsEmpty())
		{
			FKept& Entry = Kept().Add(Key);
			Entry.Texture.Reset(Texture);
			Entry.Why = Why;
		}
		return Texture;
	};

	const TStrongObjectPtr<UMaterialInterface> Surface = FMantlePlaceAssets::DecalSurfaceCopy(Material);
	if (!Surface.IsValid())
	{
		OutWhy = TEXT("it is not a decal material Mantle can copy (a dynamic material instance, or not Deferred Decal)");
		return Keep(nullptr, OutWhy);
	}
	FMantleFlattenSettings Settings;
	Settings.MaxBakeSize = Limits.MaxTextureSize;
	FMantleGlbDocument Document;
	TArray<FString> Messages;
	FString Error;
	if (!Bake.Flatten(Surface.Get(), Settings, Document, Messages, Error))
	{
		OutWhy = FString::Printf(TEXT("the engine could not bake it (%s)"), *Error);
		return Keep(nullptr, OutWhy);
	}
	TArray<FColor> Pixels;
	int32 Width = 0;
	int32 Height = 0;
	FString ImageWhy;
	if (!FMantlePlaceAssetConverter::BakedBaseColorImage(Document, Pixels, Width, Height, ImageWhy))
	{
		OutWhy = ImageWhy;
		return Keep(nullptr, OutWhy);
	}

	FMD5 Md5;
	const FTCHARToUTF8 Utf8(*Key);
	Md5.Update(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	uint8 Digest[16];
	Md5.Final(Digest);
	const FString Hash = FString::Printf(TEXT("%02x%02x%02x%02x"), Digest[0], Digest[1], Digest[2], Digest[3]);
	UTexture2D* Baked = NewObject<UTexture2D>(GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UTexture2D::StaticClass(),
		FName(*FString::Printf(TEXT("%s_DecalBake_%s"), *Material->GetName(), *Hash))));
	Baked->SRGB = true;
	Baked->Source.Init(Width, Height, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Pixels.GetData()));
	UE_LOG(LogMantle, Log, TEXT("MANTLE_DECAL_BAKE %s: baked its surface copy into a %dx%d decal texture"), *Material->GetName(), Width, Height);
	return Keep(Baked, FString());
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
