// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePreviewAvatarExport.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HttpManager.h"
#include "HttpModule.h"
#include "MantleApiClient.h"
#include "MantleAssetStore.h"
#include "MantleAvatarContract.h"
#include "MantleCosmeticItem.h"
#include "MantleCosmeticPackage.h"
#include "MantleNativeEye.h"
#include "MantleRuntime.h"
#include "MantleSettings.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantlePreviewAvatarExportPrivate
{
	/** How long the cooked files may take to download before the export gives up. */
	constexpr double DownloadTimeoutSeconds = 300.0;

	/**
	 * The neutral body-only descriptor (schema/examples/avatar/valid-selected-avatar.json, descriptor v9: closed, every
	 * field required). Only body.avatarItemId changes. The preview player wears the avatar as it is, with no clothing.
	 */
	const TCHAR* const NeutralDescriptor = TEXT(R"json({"schemaVersion":9,"body":{"avatarItemId":"","skinToneId":"tone_00","eyeColorId":"eye_00","heightPct":100,"weight":0,"muscle":0,"chest":0,"hips":0},"wearing":[],"emotes":[],"eyes":{"left":{"eyeColorId":"eye_00","pupil":{"mode":"source"},"irisPattern":{"mode":"default","id":0},"irisBlend":{"mode":"default","amount":0,"secondaryColor":"#ffffff","softness":128,"method":1},"iris":{"mode":"default","rotation":0,"shadowDetails":128,"saturation":128,"tint":"#ffffff","pupilFeather":115},"limbalRing":{"mode":"default","size":128,"softness":128,"color":"#ffffff"},"cornea":{"mode":"default","size":128,"limbusSoftness":128,"limbusColor":"#ffffff"},"sclera":{"mode":"default","color":"#ffffff","vascularity":0,"rotation":0,"transmissionSpread":128,"transmissionColor":"#ffffff","vascularityCoverage":170}},"right":{"eyeColorId":"eye_00","pupil":{"mode":"source"},"irisPattern":{"mode":"default","id":0},"irisBlend":{"mode":"default","amount":0,"secondaryColor":"#ffffff","softness":128,"method":1},"iris":{"mode":"default","rotation":0,"shadowDetails":128,"saturation":128,"tint":"#ffffff","pupilFeather":115},"limbalRing":{"mode":"default","size":128,"softness":128,"color":"#ffffff"},"cornea":{"mode":"default","size":128,"limbusSoftness":128,"limbusColor":"#ffffff"},"sclera":{"mode":"default","color":"#ffffff","vascularity":0,"rotation":0,"transmissionSpread":128,"transmissionColor":"#ffffff","vascularityCoverage":170}}},"hairColors":{"hair":{"mode":"source"},"eyebrows":{"mode":"match_hair"},"eyelashes":{"mode":"match_hair"},"facial_hair":{"mode":"match_hair"}},"appearance":{"skin":{"finish":{"enabled":false,"roughness":128,"specular":128,"scatter":128,"normalStrength":128,"poreStrength":0,"poreScale":128,"fuzz":0},"tone":{"u":128,"v":128},"faceTexture":-1,"bodyTexture":-1,"accents":{"scalp":{"redness":128,"saturation":128,"lightness":128},"forehead":{"redness":128,"saturation":128,"lightness":128},"nose":{"redness":128,"saturation":128,"lightness":128},"underEye":{"redness":128,"saturation":128,"lightness":128},"cheeks":{"redness":128,"saturation":128,"lightness":128},"lips":{"redness":128,"saturation":128,"lightness":128},"chin":{"redness":128,"saturation":128,"lightness":128},"ears":{"redness":128,"saturation":128,"lightness":128}},"body":{"detail":{"id":0,"strength":0},"scar":{"itemId":"","strength":0,"color":"#ffffff","roughness":128},"tattoo":{"itemId":"","strength":0,"color":"#ffffff"}},"face":{"detail":{"id":0,"strength":0},"scar":{"itemId":"","strength":0,"color":"#ffffff","roughness":128},"tattoo":{"itemId":"","strength":0,"color":"#ffffff"}}},"makeup":{"eye":{"id":0,"strength":0,"primary":{"color":"#ffffff","opacity":0,"roughness":128,"specular":128,"scatter":255,"normal":255,"metallic":0},"secondary":{"color":"#ffffff","opacity":0,"roughness":128,"specular":128,"scatter":255,"normal":255,"metallic":0}},"blush":{"id":0,"strength":0,"channel":{"color":"#ffffff","opacity":0,"roughness":128,"specular":128,"scatter":255,"normal":255,"metallic":0}},"lipstick":{"id":0,"strength":0,"channel":{"color":"#ffffff","opacity":0,"roughness":128,"specular":128,"scatter":255,"normal":255,"metallic":0}},"foundation":{"id":0,"strength":0,"foundation":{"color":"#ffffff","opacity":0,"roughness":128,"specular":128,"scatter":255,"normal":255,"metallic":0},"concealer":{"color":"#ffffff","opacity":0,"roughness":128,"specular":128,"scatter":255,"normal":255,"metallic":0}},"freckles":{"id":0,"strength":0,"saturation":128,"density":0,"toneShift":128}},"nails":{"fingers":{"color":"#520010","opacity":0,"roughness":128,"metallic":0},"toes":{"color":"#520010","opacity":0,"roughness":128,"metallic":0}},"palms":{"lightness":191,"tint":128,"cavityDarkness":77},"teeth":{"color":"#ffffff","gumColor":"#ffffff","plaqueColor":"#ffffff","plaqueAmount":0},"hair":{"mode":"source","melanin":0,"redness":0,"dyeColor":"#ffffff","rootDarkening":0,"rootFade":0,"seedVariation":0,"whiteness":0,"ombreColor":"#ffffff","ombreAmount":0,"ombreTransition":0,"highlightsColor":"#ffffff","highlightsAmount":0,"roughness":64,"lightness":0,"ombreContrast":0,"regionColor":"#ffffff","regionAmount":0,"highlightsBlending":0,"highlightsVariation":0}}})json");

	/**
	 * The item's asset reference fields and its materials' texture fields, as the client's MantleBuiltinBundle reads them
	 * (BundleItemRefs, BundleMaterialLists and BundleMaterialRefs there).
	 */
	const TCHAR* const ItemRefs[] = { TEXT("mesh"), TEXT("faceMesh"), TEXT("texture"), TEXT("clip"), TEXT("dna"), TEXT("coverage"),
		TEXT("sourceShape"), TEXT("irisMask") };
	const TCHAR* const MaterialLists[] = { TEXT("materials"), TEXT("faceMaterials") };
	const TCHAR* const MaterialRefs[] = { TEXT("albedo"), TEXT("normal"), TEXT("orm") };

	/** The flat URL fields the batch answer repeats beside each asset reference; the bundle carries none of them. */
	const TCHAR* const ItemUrlFields[] = { TEXT("thumbnail"), TEXT("thumbnailUrl"), TEXT("meshUrl"), TEXT("faceMeshUrl"),
		TEXT("textureUrl"), TEXT("clipUrl"), TEXT("dnaUrl"), TEXT("coverageUrl"), TEXT("sourceShapeUrl"), TEXT("irisMaskUrl") };
	const TCHAR* const MaterialUrlFields[] = { TEXT("albedoUrl"), TEXT("normalUrl"), TEXT("ormUrl") };

	/** One cooked file the item names, by the field path the bundle's manifest lists it under. */
	struct FExportRef
	{
		FString Field;
		TSharedPtr<FJsonObject> Ref;
		FMantleContentHash Hash;
		FString Url;
		int64 Bytes = 0;
	};

	/** The state the download callbacks share with the waiting loop. */
	struct FDownloadState
	{
		int32 Pending = 0;
		TArray<FString> Errors;
		TMap<FMantleContentHash, int64> Sizes;
	};

	bool WriteJsonFile(const TSharedRef<FJsonObject>& Object, const FString& Path, FString& OutError)
	{
		FString Text;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
		if (!FJsonSerializer::Serialize(Object, Writer))
		{
			OutError = FString::Printf(TEXT("%s could not be serialized"), *FPaths::GetCleanFilename(Path));
			return false;
		}
		if (!FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = FString::Printf(TEXT("%s could not be written"), *Path);
			return false;
		}
		return true;
	}

	/** Takes one asset reference of Owner (a nested {url, cookedHash, cookedBytes}) and removes its URL. */
	bool TakeRef(const TSharedPtr<FJsonObject>& Owner, const TCHAR* Name, const FString& Field, TArray<FExportRef>& Out, FString& OutError)
	{
		const TSharedPtr<FJsonValue> Value = Owner->TryGetField(Name);
		if (!Value.IsValid() || Value->Type == EJson::Null)
		{
			return true;
		}
		if (Value->Type != EJson::Object)
		{
			OutError = FString::Printf(TEXT("the item's %s is not an asset reference"), *Field);
			return false;
		}
		const TSharedPtr<FJsonObject> Ref = Value->AsObject();
		FString Url, HashText;
		Ref->TryGetStringField(TEXT("url"), Url);
		Ref->TryGetStringField(TEXT("cookedHash"), HashText);
		Ref->RemoveField(TEXT("url"));
		if (Url.IsEmpty() && HashText.IsEmpty())
		{
			// Not there (not approved yet): nothing to bundle.
			return true;
		}
		FExportRef Taken;
		FString HashError;
		if (HashText.IsEmpty() || !FMantleContentHash::Parse(HashText, Taken.Hash, &HashError))
		{
			OutError = FString::Printf(TEXT("the item's %s has no usable cooked hash (%s); it cannot be bundled"), *Field,
				HashText.IsEmpty() ? TEXT("none recorded") : *HashError);
			return false;
		}
		double Number = 0.0;
		if (Ref->TryGetNumberField(TEXT("cookedBytes"), Number) && Number > 0.0)
		{
			Taken.Bytes = static_cast<int64>(Number);
		}
		Taken.Field = Field;
		Taken.Ref = Ref;
		Taken.Url = Url;
		Out.Add(MoveTemp(Taken));
		return true;
	}

	/** Every asset reference Item names, URLs removed, in the field spelling MantleBuiltinBundle::ReadEntry expects. */
	bool CollectRefs(const TSharedPtr<FJsonObject>& Item, TArray<FExportRef>& Out, FString& OutError)
	{
		for (const TCHAR* Name : ItemRefs)
		{
			if (!TakeRef(Item, Name, Name, Out, OutError))
			{
				return false;
			}
		}
		for (const TCHAR* Url : ItemUrlFields)
		{
			Item->RemoveField(Url);
		}
		if (const TSharedPtr<FJsonValue> Variants = Item->TryGetField(TEXT("garmentVariants")); Variants.IsValid() && Variants->Type != EJson::Null)
		{
			OutError = TEXT("the item has garment variants, which an avatar body does not");
			return false;
		}
		for (const TCHAR* List : MaterialLists)
		{
			const TSharedPtr<FJsonValue> Value = Item->TryGetField(List);
			if (!Value.IsValid() || Value->Type == EJson::Null)
			{
				continue;
			}
			if (Value->Type != EJson::Array)
			{
				OutError = FString::Printf(TEXT("the item's %s is not an array"), List);
				return false;
			}
			const TArray<TSharedPtr<FJsonValue>>& Sections = Value->AsArray();
			for (int32 Index = 0; Index < Sections.Num(); ++Index)
			{
				if (!Sections[Index].IsValid() || Sections[Index]->Type != EJson::Object)
				{
					OutError = FString::Printf(TEXT("the item's %s[%d] is not an object"), List, Index);
					return false;
				}
				const TSharedPtr<FJsonObject> Section = Sections[Index]->AsObject();
				for (const TCHAR* Name : MaterialRefs)
				{
					if (!TakeRef(Section, Name, FString::Printf(TEXT("%s[%d].%s"), List, Index, Name), Out, OutError))
					{
						return false;
					}
				}
				for (const TCHAR* Url : MaterialUrlFields)
				{
					Section->RemoveField(Url);
				}
				if (const TSharedPtr<FJsonValue> Mask = Section->TryGetField(TEXT("colorCustomization")); Mask.IsValid() && Mask->Type != EJson::Null)
				{
					OutError = FString::Printf(TEXT("the item's %s[%d] carries a clothing colour mask, which an avatar body does not"), List, Index);
					return false;
				}
				const TSharedPtr<FJsonValue> NativeHair = Section->TryGetField(TEXT("nativeHair"));
				if (NativeHair.IsValid() && NativeHair->Type == EJson::Object
					&& !TakeRef(NativeHair->AsObject(), TEXT("tangent"), FString::Printf(TEXT("%s[%d].nativeHair.tangent"), List, Index), Out, OutError))
				{
					return false;
				}
				const TSharedPtr<FJsonValue> NativeEye = Section->TryGetField(TEXT("nativeEye"));
				if (NativeEye.IsValid() && NativeEye->Type == EJson::Object)
				{
					const TSharedPtr<FJsonObject>* Textures = nullptr;
					if (NativeEye->AsObject()->TryGetObjectField(TEXT("textures"), Textures) && Textures)
					{
						for (const TCHAR* Parameter : FMantleNativeEye::TextureParameters())
						{
							if (!TakeRef(*Textures, Parameter, FString::Printf(TEXT("%s[%d].nativeEye.textures.%s"), List, Index, Parameter), Out, OutError))
							{
								return false;
							}
						}
					}
				}
			}
		}
		return true;
	}

	/** The raw item object for ItemId in a batch answer, or null with OutError. */
	TSharedPtr<FJsonObject> FindItemObject(const FString& Body, const FString& ItemId, FString& OutError)
	{
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid() || !Root->TryGetArrayField(TEXT("items"), Items) || !Items)
		{
			OutError = TEXT("the catalog's answer could not be read");
			return nullptr;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Items)
		{
			const TSharedPtr<FJsonObject> Object = Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
			FString Id;
			if (Object.IsValid() && Object->TryGetStringField(TEXT("id"), Id) && Id.Equals(ItemId, ESearchCase::CaseSensitive))
			{
				return Object;
			}
		}
		OutError = FString::Printf(TEXT("the catalog does not serve %s (unknown, delisted or not approved)"), *ItemId);
		return nullptr;
	}

	/** The preview avatar: the console command's choice, kept in the editor's Mantle settings. */
	void RunPreviewAvatarCommand(const TArray<FString>& Args)
	{
		UMantleSettings* Settings = UMantleSettings::GetMutable();
		if (Args.Num() == 0)
		{
			UE_LOG(LogMantle, Display, TEXT("Mantle.Preview.Avatar is %s. Usage: Mantle.Preview.Avatar <itm_u... item id> (none clears it)."),
				Settings->PreviewAvatarItemId.IsEmpty() ? TEXT("not set") : *Settings->PreviewAvatarItemId);
			return;
		}
		const FString Id = Args[0].TrimStartAndEnd();
		if (Id.Equals(TEXT("none"), ESearchCase::IgnoreCase))
		{
			Settings->PreviewAvatarItemId.Reset();
		}
		else if (MantleCosmetic::IsItemId(Id))
		{
			Settings->PreviewAvatarItemId = Id;
		}
		else
		{
			UE_LOG(LogMantle, Warning, TEXT("Mantle.Preview.Avatar: '%s' is not an avatar item id (itm_u and 32 lowercase hex digits)."), *Id);
			return;
		}
		Settings->SaveConfig();
		UE_LOG(LogMantle, Display, TEXT("Mantle.Preview.Avatar is now %s."),
			Settings->PreviewAvatarItemId.IsEmpty() ? TEXT("not set") : *Settings->PreviewAvatarItemId);
	}

	/**
	 * The signed-in creator's own saved avatar (GET /me/avatar): its body item id and height, for a test that needs a body
	 * when no preview avatar is chosen. False with OutError saying what the creator can do.
	 */
	bool FetchOwnAvatar(FString& OutItemId, int32& OutHeightPct, FString& OutError)
	{
		FString NotConfigured;
		if (!FMantleApiClient::IsConfigured(NotConfigured))
		{
			OutError = NotConfigured;
			return false;
		}
		const FMantleApiResult Answer = FMantleApiClient::Send(TEXT("GET"), TEXT("/me/avatar"), FString());
		if (!Answer.bSuccess)
		{
			OutError = FString::Printf(TEXT("Your avatar could not be read from Mantle (%s)."),
				Answer.Message.IsEmpty() ? *FString::Printf(TEXT("HTTP %d"), Answer.StatusCode) : *Answer.Message);
			return false;
		}
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Answer.Body);
		const TSharedPtr<FJsonObject>* Descriptor = nullptr;
		const TSharedPtr<FJsonObject>* Body = nullptr;
		bool bHasSaved = false;
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			OutError = TEXT("Mantle's answer about your avatar could not be read.");
			return false;
		}
		if (!Root->TryGetBoolField(TEXT("hasSavedAvatar"), bHasSaved) || !bHasSaved
			|| !Root->TryGetObjectField(TEXT("descriptor"), Descriptor) || !Descriptor
			|| !(*Descriptor)->TryGetObjectField(TEXT("body"), Body) || !Body
			|| !(*Body)->TryGetStringField(TEXT("avatarItemId"), OutItemId) || OutItemId.IsEmpty())
		{
			OutError = TEXT("Your account has no saved avatar yet: open Mantle, choose one in Avatar and save it.");
			return false;
		}
		int32 Height = 100;
		OutHeightPct = (*Body)->TryGetNumberField(TEXT("heightPct"), Height) ? Height : 100;
		return true;
	}

	/** Avatar/descriptor.json: the neutral body-only descriptor wearing ItemId at HeightPct. */
	bool WriteDescriptor(const FString& AvatarDir, const FString& ItemId, const int32 HeightPct, FString& OutError)
	{
		TSharedPtr<FJsonObject> Descriptor;
		const TSharedRef<TJsonReader<>> DescriptorReader = TJsonReaderFactory<>::Create(FString(NeutralDescriptor));
		const TSharedPtr<FJsonObject>* Body = nullptr;
		if (!FJsonSerializer::Deserialize(DescriptorReader, Descriptor) || !Descriptor.IsValid()
			|| !Descriptor->TryGetObjectField(TEXT("body"), Body) || !Body)
		{
			OutError = TEXT("the neutral avatar descriptor template is unreadable");
			return false;
		}
		(*Body)->SetStringField(TEXT("avatarItemId"), ItemId);
		(*Body)->SetNumberField(TEXT("heightPct"), HeightPct);
		return WriteJsonFile(Descriptor.ToSharedRef(), FPaths::Combine(AvatarDir, TEXT("descriptor.json")), OutError);
	}

	FAutoConsoleCommand PreviewAvatarCommand(
		TEXT("Mantle.Preview.Avatar"),
		TEXT("Sets the catalog avatar item (itm_u...) the offline preview's player wears, or 'none'. With no argument, prints the current choice."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&RunPreviewAvatarCommand));
}

bool MantlePreviewAvatarExport::Write(const FString& BundleDir, FString& OutDescriptorRelPath, FString& OutBuiltinRelPath, FString& OutError,
	const bool bUseOwnAvatarWhenUnset)
{
	using namespace MantlePreviewAvatarExportPrivate;
	OutDescriptorRelPath.Reset();
	OutBuiltinRelPath.Reset();
	OutError.Reset();

	FString ItemId = UMantleSettings::Get()->PreviewAvatarItemId.TrimStartAndEnd();
	int32 HeightPct = 100;
	if (ItemId.IsEmpty())
	{
		if (!bUseOwnAvatarWhenUnset)
		{
			return false;
		}
		// A weapon is held and aimed by an avatar body; the bare stand-in skeleton plays the raw clips. With no avatar
		// chosen, the test wears the creator's own.
		FString Why;
		if (!FetchOwnAvatar(ItemId, HeightPct, Why))
		{
			OutError = FString::Printf(TEXT("Testing a weapon needs an avatar to hold it, so the test wears yours. %s"), *Why);
			return false;
		}
		UE_LOG(LogMantle, Display, TEXT("MANTLE_PREVIEW_AVATAR own %s height=%d"), *ItemId, HeightPct);
	}
	if (BundleDir.IsEmpty() || !IFileManager::Get().DirectoryExists(*BundleDir))
	{
		OutError = FString::Printf(TEXT("The preview avatar %s could not be bundled: the bundle folder '%s' does not exist."), *ItemId, *BundleDir);
		return false;
	}
	const FString AvatarDir = FPaths::Combine(BundleDir, TEXT("Avatar"));
	const FString ItemDir = FPaths::Combine(AvatarDir, TEXT("Builtin"), ItemId);
	// A partly written avatar is never left for the preview to find.
	const auto Fail = [&OutError, &AvatarDir, &ItemId](const FString& Why) -> bool
	{
		OutError = FString::Printf(TEXT("The preview avatar %s could not be bundled: %s"), *ItemId, *Why);
		IFileManager::Get().DeleteDirectory(*AvatarDir, /*RequireExists*/ false, /*Tree*/ true);
		return false;
	};

	if (!MantleCosmetic::IsItemId(ItemId))
	{
		return Fail(TEXT("it is not an avatar item id (itm_u and 32 lowercase hex digits). Set it with Mantle.Preview.Avatar."));
	}
	IFileManager::Get().DeleteDirectory(*AvatarDir, /*RequireExists*/ false, /*Tree*/ true);

	// ---- The item, as the catalog serves it. ----
	FString NotConfigured;
	if (!FMantleApiClient::IsConfigured(NotConfigured))
	{
		return Fail(FString::Printf(TEXT("the catalog cannot be reached (%s)"), *NotConfigured));
	}
	const FMantleApiResult Answer = FMantleApiClient::Send(TEXT("POST"), TEXT("/avatar/items/batch"), MantleCosmetic::BuildItemBatchBody({ ItemId }));
	if (!Answer.bSuccess)
	{
		return Fail(FString::Printf(TEXT("the catalog lookup failed: %s"), Answer.Message.IsEmpty() ? *FString::Printf(TEXT("HTTP %d"), Answer.StatusCode) : *Answer.Message));
	}
	TArray<FMantleCosmeticItem> Parsed;
	TArray<FString> Missing;
	FString Why;
	if (!MantleCosmetic::ParseItemBatch(Answer.Body, Parsed, Missing, Why))
	{
		return Fail(FString::Printf(TEXT("the catalog's answer could not be read: %s"), *Why));
	}
	const FMantleCosmeticItem* Found = Parsed.FindByPredicate([&ItemId](const FMantleCosmeticItem& Candidate)
	{
		return Candidate.Id.Equals(ItemId, ESearchCase::CaseSensitive);
	});
	if (!Found)
	{
		return Fail(TEXT("the catalog does not serve it (unknown, delisted or not approved)"));
	}
	if (Found->Kind != EMantleCosmeticKind::Avatar)
	{
		return Fail(FString::Printf(TEXT("it is a %s item, not an avatar"), MantleCosmetic::KindName(Found->Kind)));
	}
	if (Found->Version < 1 || Found->UnverifiedAssets.Num() > 0)
	{
		return Fail(Found->Version < 1 ? FString(TEXT("it has no published version"))
			: FString::Printf(TEXT("its cooked files carry no hash to verify: %s"), *FString::Join(Found->UnverifiedAssets, TEXT(", "))));
	}
	const TSharedPtr<FJsonObject> Item = FindItemObject(Answer.Body, ItemId, Why);
	if (!Item.IsValid())
	{
		return Fail(Why);
	}
	TArray<FExportRef> Refs;
	if (!CollectRefs(Item, Refs, Why))
	{
		return Fail(Why);
	}
	if (Refs.IsEmpty())
	{
		return Fail(TEXT("the catalog names no cooked files for it"));
	}

	// ---- The cooked files, through the editor's asset store (the same one the cosmetic preview downloads into). ----
	const FString AssetsDir = FPaths::Combine(ItemDir, TEXT("assets"));
	if (!IFileManager::Get().MakeDirectory(*AssetsDir, /*Tree*/ true))
	{
		return Fail(FString::Printf(TEXT("%s cannot be created"), *AssetsDir));
	}
	FMantleAssetStore& Store = FMantleAssetStore::Get();
	const TSharedRef<FDownloadState> State = MakeShared<FDownloadState>();
	TSet<FMantleContentHash> Requested;
	TArray<uint64> RequestIds;
	TArray<FMantleContentHash> Pinned;
	for (const FExportRef& Ref : Refs)
	{
		bool bAlready = false;
		Requested.Add(Ref.Hash, &bAlready);
		if (bAlready)
		{
			continue;
		}
		FMantleStoredAssetRef StoreRef;
		StoreRef.Hash = Ref.Hash;
		StoreRef.Url = Ref.Url;
		StoreRef.Bytes = Ref.Bytes;
		Store.Pin(Ref.Hash);
		Pinned.Add(Ref.Hash);
		++State->Pending;
		const FString Destination = FPaths::Combine(AssetsDir, Ref.Hash.ToHex());
		const FString Field = Ref.Field;
		RequestIds.Add(Store.Request(StoreRef, EMantleAssetLoad::PathOnly,
			FMantleAssetDone::CreateLambda([State, Destination, Field](const FMantleAssetResult& Result)
			{
				--State->Pending;
				if (!Result.bSuccess)
				{
					State->Errors.Add(FString::Printf(TEXT("%s did not download: %s"), *Field, *Result.Error));
					return;
				}
				if (IFileManager::Get().Copy(*Destination, *Result.Path, /*Replace*/ true) != COPY_OK)
				{
					State->Errors.Add(FString::Printf(TEXT("%s could not be copied from the store"), *Field));
					return;
				}
				State->Sizes.Add(Result.Hash, Result.SizeBytes);
			})));
	}
	const double Deadline = FPlatformTime::Seconds() + DownloadTimeoutSeconds;
	while (State->Pending > 0 && FPlatformTime::Seconds() < Deadline)
	{
		FHttpModule::Get().GetHttpManager().Tick(0.016f);
		Store.Pump();
		FPlatformProcess::Sleep(0.005f);
	}
	const bool bTimedOut = State->Pending > 0;
	if (bTimedOut)
	{
		for (const uint64 RequestId : RequestIds)
		{
			Store.Cancel(RequestId);
		}
	}
	for (const FMantleContentHash& Hash : Pinned)
	{
		Store.Unpin(Hash);
	}
	if (bTimedOut)
	{
		return Fail(TEXT("its cooked files did not finish downloading in time"));
	}
	if (State->Errors.Num() > 0)
	{
		return Fail(FString::Join(State->Errors, TEXT("; ")));
	}

	// ---- manifest.json, item.json and the descriptor. ----
	TArray<TSharedPtr<FJsonValue>> ManifestAssets;
	for (const FExportRef& Ref : Refs)
	{
		const int64* Size = State->Sizes.Find(Ref.Hash);
		if (!Size || *Size <= 0)
		{
			return Fail(FString::Printf(TEXT("%s has no size"), *Ref.Field));
		}
		// What the manifest lists and item.json says must agree; the verified file's size is the truth.
		Ref.Ref->SetNumberField(TEXT("cookedBytes"), static_cast<double>(*Size));
		const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("field"), Ref.Field);
		Entry->SetStringField(TEXT("cookedHash"), Ref.Hash.ToString());
		Entry->SetNumberField(TEXT("cookedBytes"), static_cast<double>(*Size));
		Entry->SetStringField(TEXT("file"), TEXT("assets/") + Ref.Hash.ToHex());
		ManifestAssets.Add(MakeShared<FJsonValueObject>(Entry));
	}
	const TSharedRef<FJsonObject> Manifest = MakeShared<FJsonObject>();
	Manifest->SetNumberField(TEXT("schema"), 1);
	Manifest->SetStringField(TEXT("key"), ItemId);
	Manifest->SetStringField(TEXT("itemId"), ItemId);
	Manifest->SetNumberField(TEXT("version"), Found->Version);
	Manifest->SetArrayField(TEXT("assets"), ManifestAssets);
	if (!WriteJsonFile(Item.ToSharedRef(), FPaths::Combine(ItemDir, TEXT("item.json")), Why)
		|| !WriteJsonFile(Manifest, FPaths::Combine(ItemDir, TEXT("manifest.json")), Why))
	{
		return Fail(Why);
	}

	if (!WriteDescriptor(AvatarDir, ItemId, HeightPct, Why))
	{
		return Fail(Why);
	}

	OutDescriptorRelPath = TEXT("Avatar/descriptor.json");
	OutBuiltinRelPath = TEXT("Avatar/Builtin");
	UE_LOG(LogMantle, Log, TEXT("Preview avatar %s v%d bundled: %d file reference(s), %d distinct file(s)."), *ItemId, Found->Version, Refs.Num(), Requested.Num());
	return true;
}

FString MantlePreviewAvatarExport::LocalDraftItemId(const FMantleContentHash& BodyMeshHash)
{
	return TEXT("itm_u") + BodyMeshHash.ToHex().Left(32);
}

TSharedPtr<FJsonObject> MantlePreviewAvatarExport::MakeLocalDraftItem(const FMantleCosmeticPackage& Package, const TArray<FCookedDraftAsset>& Cooked, FString& OutError)
{
	OutError.Reset();
	if (Package.Class != EMantleCosmeticClass::Avatar)
	{
		OutError = FString::Printf(TEXT("it is a %s item, not an avatar"), LexToString(Package.Class));
		return nullptr;
	}

	// The package names its files by key; MakeItemJson is given each key as its own id, so its asset id fields carry keys.
	TMap<FString, const FCookedDraftAsset*> ByKey;
	TMap<FString, FString> IdsByKey;
	for (const FCookedDraftAsset& Asset : Cooked)
	{
		if (Asset.Bytes.IsEmpty())
		{
			OutError = FString::Printf(TEXT("%s was cooked to nothing"), *Asset.Key);
			return nullptr;
		}
		ByKey.Add(Asset.Key, &Asset);
		IdsByKey.Add(Asset.Key, Asset.Key);
	}
	// The thumbnail is named by the item but never bundled.
	if (!Package.ThumbnailKey.IsEmpty())
	{
		IdsByKey.Add(Package.ThumbnailKey, Package.ThumbnailKey);
	}
	// A draft is never listed, so a public item's rights declaration does not apply to it.
	TOptional<FMantleCosmeticPackage> AsPrivate;
	if (Package.Visibility != TEXT("private"))
	{
		AsPrivate.Emplace(Package);
		AsPrivate->Visibility = TEXT("private");
	}
	FString CreateJson;
	if (!(AsPrivate.IsSet() ? AsPrivate.GetValue() : Package).MakeItemJson(IdsByKey, CreateJson, OutError))
	{
		return nullptr;
	}
	TSharedPtr<FJsonObject> Item;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(CreateJson);
	if (!FJsonSerializer::Deserialize(Reader, Item) || !Item.IsValid())
	{
		OutError = TEXT("the item could not be read back");
		return nullptr;
	}
	// What only the create request carries.
	for (const TCHAR* Field : { TEXT("visibility"), TEXT("rightsDeclarationVersion"), TEXT("provenance"), TEXT("thumbnailAssetId") })
	{
		Item->RemoveField(Field);
	}

	// Each asset id field becomes the reference the catalog serves in its place (backend avatar.AssetRef, without the url).
	const auto ToRef = [&ByKey, &OutError](const TSharedPtr<FJsonObject>& Owner, const FString& Key, const FString& RefField) -> bool
	{
		if (Key.IsEmpty())
		{
			return true;
		}
		const FCookedDraftAsset* const* Asset = ByKey.Find(Key);
		if (!Asset)
		{
			OutError = FString::Printf(TEXT("%s was not cooked"), *Key);
			return false;
		}
		const TSharedRef<FJsonObject> Ref = MakeShared<FJsonObject>();
		Ref->SetStringField(TEXT("cookedHash"), FMantleContentHash::OfBytes((*Asset)->Bytes).ToString());
		Ref->SetNumberField(TEXT("cookedBytes"), static_cast<double>((*Asset)->Bytes.Num()));
		Owner->SetObjectField(RefField, Ref);
		return true;
	};
	const auto MoveRef = [&ToRef](const TSharedPtr<FJsonObject>& Owner, const TCHAR* IdField, const TCHAR* RefField) -> bool
	{
		FString Key;
		Owner->TryGetStringField(IdField, Key);
		Owner->RemoveField(IdField);
		return ToRef(Owner, Key, RefField);
	};
	if (!MoveRef(Item, TEXT("meshAssetId"), TEXT("mesh")) || !MoveRef(Item, TEXT("faceMeshAssetId"), TEXT("faceMesh")))
	{
		return nullptr;
	}
	for (const TCHAR* List : { TEXT("materials"), TEXT("faceMaterials") })
	{
		const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
		if (!Item->TryGetArrayField(List, Sections) || !Sections)
		{
			continue;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Sections)
		{
			const TSharedPtr<FJsonObject> Section = Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
			if (!Section.IsValid())
			{
				continue;
			}
			if (!MoveRef(Section, TEXT("albedoAssetId"), TEXT("albedo")) || !MoveRef(Section, TEXT("normalAssetId"), TEXT("normal"))
				|| !MoveRef(Section, TEXT("ormAssetId"), TEXT("orm")))
			{
				return nullptr;
			}
			const TSharedPtr<FJsonObject>* NativeEye = nullptr;
			const TSharedPtr<FJsonObject>* TextureIds = nullptr;
			if (Section->TryGetObjectField(TEXT("nativeEye"), NativeEye) && NativeEye
				&& (*NativeEye)->TryGetObjectField(TEXT("textureAssetIds"), TextureIds) && TextureIds)
			{
				const TSharedPtr<FJsonObject> Textures = MakeShared<FJsonObject>();
				for (const auto& Texture : (*TextureIds)->Values)
				{
					if (!ToRef(Textures, Texture.Value.IsValid() ? Texture.Value->AsString() : FString(), FString(Texture.Key.ToView())))
					{
						return nullptr;
					}
				}
				const TSharedPtr<FJsonObject> Eye = *NativeEye;
				Eye->RemoveField(TEXT("textureAssetIds"));
				Eye->SetObjectField(TEXT("textures"), Textures);
			}
		}
	}

	// What the server copies from the cooked headers when it creates the item (backend avatar/items.go).
	const FCookedDraftAsset* const* Body = ByKey.Find(Package.MeshKey);
	const TSharedPtr<FJsonObject> BodyInfo = Body ? (*Body)->Info : nullptr;
	FString Skeleton;
	bool bGarmentCompatible = false;
	if (!BodyInfo.IsValid() || !BodyInfo->TryGetStringField(TEXT("skeleton"), Skeleton))
	{
		OutError = TEXT("the cook did not say which skeleton the body has");
		return nullptr;
	}
	BodyInfo->TryGetBoolField(TEXT("garmentCompatible"), bGarmentCompatible);
	Item->SetStringField(TEXT("kind"), TEXT("avatar"));
	Item->SetNumberField(TEXT("version"), 0);
	Item->SetStringField(TEXT("skeleton"), Skeleton);
	Item->SetBoolField(TEXT("garmentCompatible"), bGarmentCompatible);

	const FCookedDraftAsset* const* Face = Package.FaceMeshKey.IsEmpty() ? nullptr : ByKey.Find(Package.FaceMeshKey);
	const TSharedPtr<FJsonObject> FaceInfo = Face ? (*Face)->Info : nullptr;
	const TSharedPtr<FJsonObject>* BodyMetrics = nullptr;
	if (BodyInfo->TryGetObjectField(TEXT("metrics"), BodyMetrics) && BodyMetrics)
	{
		// avatarMetrics: as tall as the higher of the body and its face, with the face's eye height when the body has none.
		const TSharedRef<FJsonObject> Metrics = MakeShared<FJsonObject>();
		Metrics->Values = (*BodyMetrics)->Values;
		const TArray<TSharedPtr<FJsonValue>>* FaceMax = nullptr;
		if (FaceInfo.IsValid() && FaceInfo->TryGetArrayField(TEXT("max"), FaceMax) && FaceMax && FaceMax->Num() == 3)
		{
			double Standing = 0.0;
			Metrics->TryGetNumberField(TEXT("standingHeightCm"), Standing);
			const double FaceTop = FMath::RoundToDouble((*FaceMax)[2]->AsNumber() * 10.0) / 10.0;
			Metrics->SetNumberField(TEXT("standingHeightCm"), FMath::Max(Standing, FaceTop));
		}
		double FaceEyes = 0.0;
		if (!Metrics->HasField(TEXT("eyeHeightCm")) && FaceInfo.IsValid() && FaceInfo->TryGetNumberField(TEXT("eyeHeightCm"), FaceEyes))
		{
			Metrics->SetNumberField(TEXT("eyeHeightCm"), FaceEyes);
		}
		Item->SetObjectField(TEXT("bodyMetrics"), Metrics);
	}

	// The face's jaw when the creator named none: the contract's default, spelled as the face spells it.
	const TArray<TSharedPtr<FJsonValue>>* FaceBones = nullptr;
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get();
	if (!Item->HasField(TEXT("jawBone")) && Contract && FaceInfo.IsValid() && FaceInfo->TryGetArrayField(TEXT("bones"), FaceBones) && FaceBones)
	{
		const FString Default = Contract->FaceJawBoneDefault.ToString();
		for (const TSharedPtr<FJsonValue>& Bone : *FaceBones)
		{
			if (Bone.IsValid() && Bone->Type == EJson::String && Bone->AsString().Equals(Default, ESearchCase::IgnoreCase))
			{
				Item->SetStringField(TEXT("jawBone"), Bone->AsString());
				break;
			}
		}
	}

	// The catalog's generated column (migration 0084).
	Item->SetBoolField(TEXT("customizationEligible"), Package.AvatarPublicationMode == TEXT("converted") && Skeleton == TEXT("metahuman")
		&& bGarmentCompatible && !Package.FaceMeshKey.IsEmpty());
	return Item;
}

bool MantlePreviewAvatarExport::WriteLocalDraft(const FString& BundleDir, const FMantleCosmeticPackage& Package, const TArray<FCookedDraftAsset>& Cooked,
	FString& OutDescriptorRelPath, FString& OutBuiltinRelPath, FString& OutError)
{
	using namespace MantlePreviewAvatarExportPrivate;
	OutDescriptorRelPath.Reset();
	OutBuiltinRelPath.Reset();
	OutError.Reset();

	const FString Name = Package.Name.TrimStartAndEnd().IsEmpty() ? FString(TEXT("Your avatar")) : Package.Name.TrimStartAndEnd();
	if (BundleDir.IsEmpty() || !IFileManager::Get().DirectoryExists(*BundleDir))
	{
		OutError = FString::Printf(TEXT("%s could not be bundled for the test: the bundle folder '%s' does not exist."), *Name, *BundleDir);
		return false;
	}
	const FString AvatarDir = FPaths::Combine(BundleDir, TEXT("Avatar"));
	// A partly written avatar is never left for the preview to find.
	const auto Fail = [&OutError, &AvatarDir, &Name](const FString& Why) -> bool
	{
		OutError = FString::Printf(TEXT("%s could not be bundled for the test: %s"), *Name, *Why);
		IFileManager::Get().DeleteDirectory(*AvatarDir, /*RequireExists*/ false, /*Tree*/ true);
		return false;
	};
	IFileManager::Get().DeleteDirectory(*AvatarDir, /*RequireExists*/ false, /*Tree*/ true);

	FString Why;
	const TSharedPtr<FJsonObject> Item = MakeLocalDraftItem(Package, Cooked, Why);
	if (!Item.IsValid())
	{
		return Fail(Why);
	}
	const FCookedDraftAsset* BodyMesh = Cooked.FindByPredicate([&Package](const FCookedDraftAsset& Asset) { return Asset.Key == Package.MeshKey; });
	if (!BodyMesh)
	{
		return Fail(TEXT("its body mesh was not cooked"));
	}
	const FString ItemId = LocalDraftItemId(FMantleContentHash::OfBytes(BodyMesh->Bytes));
	Item->SetStringField(TEXT("id"), ItemId);

	// The manifest lists exactly the references item.json names: the client matches them one to one.
	TArray<FExportRef> Refs;
	if (!CollectRefs(Item, Refs, Why))
	{
		return Fail(Why);
	}
	TMap<FMantleContentHash, const FCookedDraftAsset*> ByHash;
	for (const FCookedDraftAsset& Asset : Cooked)
	{
		ByHash.Add(FMantleContentHash::OfBytes(Asset.Bytes), &Asset);
	}
	const FString ItemDir = FPaths::Combine(AvatarDir, TEXT("Builtin"), ItemId);
	if (!IFileManager::Get().MakeDirectory(*FPaths::Combine(ItemDir, TEXT("assets")), /*Tree*/ true))
	{
		return Fail(FString::Printf(TEXT("%s cannot be created"), *ItemDir));
	}
	TArray<TSharedPtr<FJsonValue>> ManifestAssets;
	TSet<FMantleContentHash> Written;
	for (const FExportRef& Ref : Refs)
	{
		const FCookedDraftAsset* const* Asset = ByHash.Find(Ref.Hash);
		if (!Asset)
		{
			return Fail(FString::Printf(TEXT("%s names a file that was not cooked"), *Ref.Field));
		}
		bool bAlready = false;
		Written.Add(Ref.Hash, &bAlready);
		if (!bAlready && !FFileHelper::SaveArrayToFile((*Asset)->Bytes, *FPaths::Combine(ItemDir, TEXT("assets"), Ref.Hash.ToHex())))
		{
			return Fail(FString::Printf(TEXT("%s could not be written"), *Ref.Field));
		}
		const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("field"), Ref.Field);
		Entry->SetStringField(TEXT("cookedHash"), Ref.Hash.ToString());
		Entry->SetNumberField(TEXT("cookedBytes"), static_cast<double>((*Asset)->Bytes.Num()));
		Entry->SetStringField(TEXT("file"), TEXT("assets/") + Ref.Hash.ToHex());
		ManifestAssets.Add(MakeShared<FJsonValueObject>(Entry));
	}
	if (ManifestAssets.IsEmpty())
	{
		return Fail(TEXT("it names no cooked files"));
	}
	// localDraft and version 0: never published, and never a made-up version. The client takes this only in the offline preview.
	const TSharedRef<FJsonObject> Manifest = MakeShared<FJsonObject>();
	Manifest->SetNumberField(TEXT("schema"), 1);
	Manifest->SetStringField(TEXT("key"), ItemId);
	Manifest->SetStringField(TEXT("itemId"), ItemId);
	Manifest->SetNumberField(TEXT("version"), 0);
	Manifest->SetBoolField(TEXT("localDraft"), true);
	Manifest->SetArrayField(TEXT("assets"), ManifestAssets);
	if (!WriteJsonFile(Item.ToSharedRef(), FPaths::Combine(ItemDir, TEXT("item.json")), Why)
		|| !WriteJsonFile(Manifest, FPaths::Combine(ItemDir, TEXT("manifest.json")), Why)
		|| !WriteDescriptor(AvatarDir, ItemId, 100, Why))
	{
		return Fail(Why);
	}

	OutDescriptorRelPath = TEXT("Avatar/descriptor.json");
	OutBuiltinRelPath = TEXT("Avatar/Builtin");
	UE_LOG(LogMantle, Display, TEXT("MANTLE_PREVIEW_AVATAR local-draft %s: %d file reference(s), %d distinct file(s)."), *ItemId, Refs.Num(), Written.Num());
	return true;
}
