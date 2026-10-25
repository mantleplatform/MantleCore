// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleAssetStore.h"

class FJsonObject;
struct FMantleCosmeticPackage;

/**
 * The avatar half of an offline preview bundle (#563 stage 2). The creator's chosen preview avatar is a catalog avatar
 * item id (UMantleSettings::PreviewAvatarItemId, set in Project Settings or with the console command
 * Mantle.Preview.Avatar <item id>). The export copies what the client needs to dress the preview player without the
 * network: the item as POST /avatar/items/batch serves it (without its URLs) and its cooked files, in the folder layout
 * the client's MantleBuiltinBundle reads, plus a body-only avatar descriptor naming it. The editor may use the network
 * here (the catalog lookup and the downloads); the preview itself never does.
 */
namespace MantlePreviewAvatarExport
{
	/**
	 * Writes BundleDir/Avatar/descriptor.json and BundleDir/Avatar/Builtin/<item id>/ (manifest.json, item.json,
	 * assets/<hex>) for the chosen preview avatar. On success OutDescriptorRelPath is "Avatar/descriptor.json" and
	 * OutBuiltinRelPath "Avatar/Builtin". Returns false with OutError empty when no avatar is chosen, and with OutError
	 * set when the chosen one cannot be written (not an item id, not signed in, not in the catalog, not an avatar, an
	 * asset that does not download). A failure leaves no Avatar folder behind. Blocks until the downloads are done.
	 * bUseOwnAvatarWhenUnset (a test with weapons): with no avatar chosen, the signed-in creator's own saved avatar is
	 * used, and OutError says what to do when there is none to use.
	 */
	bool Write(const FString& BundleDir, FString& OutDescriptorRelPath, FString& OutBuiltinRelPath, FString& OutError,
		bool bUseOwnAvatarWhenUnset = false);

	/** One file of a checked avatar package after the local cook (mantle-cook): its package key, cooked bytes and the cook's info. */
	struct FCookedDraftAsset
	{
		FString Key;
		TArray<uint8> Bytes;
		/** mantle-cook's result.info (cook.SkinnedInfo for a mesh); may be null. */
		TSharedPtr<FJsonObject> Info;
	};

	/**
	 * The id a local draft goes by: "itm_u" and the first 32 hex digits of its cooked body mesh's hash. It has the form of an item id
	 * so the client's checks read it, but it names no catalog item; the manifest's localDraft flag is what keeps it out of real traffic.
	 */
	FString LocalDraftItemId(const FMantleContentHash& BodyMeshHash);

	/**
	 * The item a checked, not uploaded avatar would be once published, as POST /avatar/items/batch serves it (without URLs): the
	 * package's own item (FMantleCosmeticPackage::MakeItemJson) with each asset id turned into {cookedHash, cookedBytes}, and what the
	 * server copies from the cooked headers (skeleton, garmentCompatible, bodyMetrics, the face's jaw bone). Version 0: it is not published.
	 * The thumbnail is left out (a bundle has no use for it). Null with OutError when the package is not an avatar or a file is missing.
	 */
	TSharedPtr<FJsonObject> MakeLocalDraftItem(const FMantleCosmeticPackage& Package, const TArray<FCookedDraftAsset>& Cooked, FString& OutError);

	/**
	 * Chunk 9: writes a checked avatar the creator has not uploaded as the preview player, with no network at all. The same layout as
	 * Write (BundleDir/Avatar/descriptor.json and Avatar/Builtin/<id>/{manifest.json, item.json, assets/<hex>}), with the manifest flagged
	 * "localDraft": true and version 0 (agreed with the client, which accepts that only in the offline preview). Cooked holds every
	 * package file except the thumbnail. A failure leaves no Avatar folder behind.
	 */
	bool WriteLocalDraft(const FString& BundleDir, const FMantleCosmeticPackage& Package, const TArray<FCookedDraftAsset>& Cooked,
		FString& OutDescriptorRelPath, FString& OutBuiltinRelPath, FString& OutError);
}
