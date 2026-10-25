// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleApiClient.h"
#include "MantleCosmeticValidation.h"
#include "MantleHairFitReport.h"
#include "MantleHairRoute.h"
#include "MantleNativeEye.h"
#include "MantleNativeHair.h"
#include "MantleClothingColor.h"
#include "MantleClothingColorDetect.h"
#include "MantleProvenance.h"

/**
 * The rights declaration a creator ticks to publish a public item (#614). The version names the
 * declaration text shown in the panel ("I own this content or its licence permits public
 * redistribution on Mantle"); the server stores it with the account and the time. Change it here,
 * once, when the wording changes.
 */
namespace MantleRights
{
	inline constexpr const TCHAR* DeclarationVersion = TEXT("2026-10-06");
}

/** What a creator is publishing. Each maps to limits.json item classes and A2.1's item kinds. */
enum class EMantleCosmeticClass : uint8
{
	/** Hair, eyebrows, eyelashes, beards: a skinned hair item (slots hair, eyebrows, eyelashes, facial_hair), from groom cards, a cards mesh or a mesh section. */
	Hair,
	/** A skinned garment (top, bottom, footwear) authored on a MetaHuman body. */
	Garment,
	/** A rigid mesh on an accessory socket. */
	Accessory,
	/** A body (plus an optional MetaHuman face). */
	Avatar,
	/** An animation on the UEFN driver skeleton. */
	Emote,
	/** #736: a scar or tattoo overlay: a mask texture (and, for scars, a normal map) painted to the converted MetaHuman body or face UVs. No mesh. */
	Overlay,
};

MANTLEEDITOR_API const TCHAR* LexToString(EMantleCosmeticClass Class);

/** limits.json's item class for Class (avatar_body, skinned_garment, rigid_accessory, emote, overlay). */
MANTLEEDITOR_API const TCHAR* ContractItemClass(EMantleCosmeticClass Class);

class FMantleAvatarContract;

/**
 * The slots the upload panel offers for Class, in limits.json's order: Hair gets the hair slots
 * (hair, eyebrows, eyelashes, facial_hair), Clothing every other skinned-garment slot, the
 * rest every slot of their item class.
 */
MANTLEEDITOR_API TArray<FString> CosmeticSlotOptions(const FMantleAvatarContract& Contract, EMantleCosmeticClass Class);

/** One file the plugin uploads for an item. */
struct MANTLEEDITOR_API FMantleCosmeticUploadAsset
{
	/** Stable inside one package: "mesh", "shape", "thumbnail", "albedo.0"... Item fields name assets by key. */
	FString Key;

	/** For the creator: "Garment mesh (MantleMale_Outfit_Outfits LOD0)". */
	FString Label;

	/** kind, part, role. Source is filled in at upload time from SourceKey. */
	FMantleAssetUploadOptions Options;

	/** A skinnedmesh's body shape: the key of the asset that must be approved before this one is sent (source_not_ready otherwise). */
	FString SourceKey;

	TArray<uint8> Bytes;

	/** FMantleSha256::AssetHash(Bytes). */
	FString Hash;
};

/** One material entry of the item: which uploaded textures a section uses, by key, and its role. */
struct MANTLEEDITOR_API FMantleCosmeticMaterialRef
{
	/** Versioned, client-owned section material id. Geometry-only converted avatars use this instead of uploaded textures. */
	FString ClientMaterial;
	FString AlbedoKey;
	FString NormalKey;
	FString OrmKey;
	FString ColorMaskKey;
	TArray<FMantleClothingColorRegion> ColorRegions;

	/**
	 * skin, eyes, other or hair_cards; empty is other. Only avatars may send skin or eyes; only hair
	 * may send hair_cards (a groom's cards: AlbedoKey is the attribute atlas, NormalKey the tangent atlas).
	 */
	FString Role;

	/** hair_cards only: the linear RGB the cards are drawn in (limits.json sectionRoles, #325, #326). */
	TOptional<FLinearColor> HairColor;
	TOptional<FMantleNativeEyeSettings> NativeEye;
	TMap<FName, FString> NativeEyeTextureKeys;
	/** Audited normalized native Groom atlas profile; empty preserves artist alpha handling. */
	FString NativeHairProfile;
	TOptional<FMantleNativeHairSettings> NativeHair;
	/** Typed linear RGB direction asset. NormalKey remains an ordinary positive-Z normal map. */
	FString NativeHairTangentKey;
};

/** limits.json's section role for a groom's cards (FMantleCosmeticMaterialRef::Role). */
inline constexpr const TCHAR* MantleHairCardsRole = TEXT("hair_cards");

/** Internal source representation of one logical clothing piece, never a separately published item. */
struct MANTLEEDITOR_API FMantleCosmeticGarmentVariantExport
{
	FString Id;
	FString NativeSourceSize;
	FString MeshKey;
	FString SourceShapeKey;
	FString FaceMeshKey;
	FString CoverageKey;
	TArray<FMantleCosmeticMaterialRef> Materials;
};

/**
 * An avatar's override of one accessory or tool socket (the item's "sockets" entry, backend avatar.Transform):
 * the parts that are on replace the socket's own, the rest stay the skeleton's. Location in cm from
 * the socket's bone, rotation in degrees (pitch, yaw, roll), scale per axis. For bodies whose
 * proportions put Mantle's default sockets in the wrong place (a hat floating over a small head, a
 * tool off a long-fingered hand, #585).
 */
struct MANTLEEDITOR_API FMantleCosmeticSocketOverride
{
	/** An accessory socket from limits.json (acc_hat, acc_face, ... acc_waist) or one of its toolSockets (hand_rSocket). */
	FString Socket;

	/** The socket's bone, for the panel's label. */
	FString Bone;

	bool bLocation = false;
	bool bRotation = false;
	bool bScale = false;

	FVector3f Location = FVector3f::ZeroVector;
	FVector3f Rotation = FVector3f::ZeroVector;
	FVector3f Scale = FVector3f::OneVector;

	/** No part overridden: nothing is sent for this socket. */
	bool IsEmpty() const { return !bLocation && !bRotation && !bScale; }
};

/**
 * Everything one upload sends: the files, and the item that names them (by key until they have ids).
 * Built by FMantleCosmeticExport from a draft, sent by FMantleCosmeticUploadSession.
 */
struct MANTLEEDITOR_API FMantleCosmeticPackage
{
	FString AvatarPublicationMode;

	/**
	 * "public" or "private" (#614), sent as the item's visibility. A public item also sends
	 * RightsDeclarationVersion, the rights declaration the creator ticked; MakeItemJson refuses a
	 * public package without it.
	 */
	FString Visibility = TEXT("public");
	FString RightsDeclarationVersion;

	/**
	 * Where the item came from (#614 stage 7): the source asset's path and its import file's name. MakeItemJson sends it as
	 * the item's "provenance" for a public item only, and leaves it out when empty.
	 */
	FMantleProvenance Provenance;
	EMantleCosmeticClass Class = EMantleCosmeticClass::Garment;
	FString Slot;
	FString Name;
	FString Description;

	/** Native groom hair: the groom cards LOD the export used, INDEX_NONE otherwise. */
	int32 GroomCardsLOD = INDEX_NONE;

	/**
	 * Set when this upload is a new version of an explicitly selected existing item: the session
	 * PATCHes /avatar/items/<TargetItemId> with MakeItemPatchJson instead of POSTing a new item.
	 * Empty creates a new item.
	 */
	FString TargetItemId;

	FString ThumbnailKey;
	FString MeshKey;
	FString FaceMeshKey;
	FString ClipKey;
	/** #736, overlays: the mask texture (sent as textureAssetId, role data) and the optional scar normal map (role normal). */
	FString MaskKey;
	FString OverlayNormalKey;
	FString CoverageKey;
	FString SourceShapeKey;

	TArray<FMantleCosmeticMaterialRef> Materials;
	TArray<FMantleCosmeticMaterialRef> FaceMaterials;
	/** Local authoring/preview mapping only; names are already stored in the uploaded mesh. */
	TArray<FString> MaterialNames;
	TArray<FMantleCosmeticGarmentVariantExport> GarmentVariants;
	FString DefaultGarmentVariant;

	/**
	 * #756, local only (not sent): every clothing material's colour area detection, found or not, including areas the
	 * creator unticked. The panel lists and names areas from it; Materials carries only what uploads.
	 */
	TArray<FMantleClothingColorDetection> DetectedClothingColors;

	FName JawBone;

	/**
	 * #595, avatars: each secondary motion chain the body carries (the ones the upload kept) and its preset id, in the
	 * contract's spelling ("hair_a", "hair"), sent as the item's "secondaryMotion" when there are any.
	 */
	TArray<TPair<FString, FString>> SecondaryMotion;

	/** Accessories: the offset from the socket (cm, degrees pitch/yaw/roll, scale), sent when bAttach. */
	bool bAttach = false;
	FVector3f AttachLocation = FVector3f::ZeroVector;
	FVector3f AttachRotation = FVector3f::ZeroVector;
	FVector3f AttachScale = FVector3f::OneVector;

	/**
	 * Avatars: the body's standing height as the cook will measure it (FMantleCosmeticChecks::AvatarStandingHeightCm:
	 * its top, or its face's), cm; unset for anything else. The panel's "Plays at X cm" (#581). Not sent: the cook
	 * measures it itself.
	 */
	TOptional<double> AvatarStandingHeightCm;

	/** Avatars: the socket overrides with at least one part on, checked (CheckSocketOverrides), sent as "sockets". */
	TArray<FMantleCosmeticSocketOverride> Sockets;

	TArray<FMantleCosmeticUploadAsset> Assets;
	TArray<FMantleCosmeticIssue> Issues;

	/** What the export did and measured, one line each, for the panel's details and the log. */
	TArray<FString> Notes;

	/** #51, local only: bought hair was sized to the standard head, and the words the preview shows for it. */
	bool bHairHeadFitted = false;
	FMantleHairFitReport::FResult HairHeadFit;
	FMantleHairFitMeshes HairHeadFitMeshes;
	/** #52, local only: which route this hair took and the words for it; set for hair items. */
	bool bHasHairRoute = false;
	FMantleHairRoute::FResult HairRoute;

	bool HasErrors() const { return FMantleCosmeticChecks::HasErrors(Issues); }

	const FMantleCosmeticUploadAsset* FindAsset(const FString& Key) const;

	/** Adds an asset (hashing it). An asset with the same key is replaced. */
	FMantleCosmeticUploadAsset& AddAsset(const FString& Key, const FString& Label, const FMantleAssetUploadOptions& Options, TArray<uint8>&& Bytes);

	/**
	 * The POST /avatar/items body (A2.1's strict JSON) with every key replaced by its uploaded id:
	 * only the fields the item's kind takes, nothing about prices (paid items are not offered yet and
	 * stay hidden: there is no price field here, and the API refuses unknown fields). False with
	 * OutError naming a key that has no id.
	 */
	bool MakeItemJson(const TMap<FString, FString>& IdsByKey, FString& OutJson, FString& OutError) const;

	/**
	 * The PATCH /avatar/items/:id body that makes the item exactly this package: MakeItemJson without
	 * the slot (a PATCH cannot change it), plus every optional field this kind takes and the package
	 * leaves out, cleared ("" or empty), so nothing of the previous version survives. The API merges
	 * a PATCH, so a field left out would otherwise keep the old value.
	 */
	bool MakeItemPatchJson(const TMap<FString, FString>& IdsByKey, FString& OutJson, FString& OutError) const;

	/**
	 * What POST /avatar/items would refuse in an avatar's socket overrides, said before anything is
	 * uploaded: a socket that is not one of limits.json's accessory sockets or toolSockets, the same
	 * socket twice, a tool socket moved off its bone or (unitScale) scaled, and any part outside the
	 * API's bounds (FMantleCosmeticChecks::CheckTransform). Empty overrides are skipped. The
	 * messages are the API's (backend avatar.checkSocketOverride).
	 */
	static void CheckSocketOverrides(TConstArrayView<FMantleCosmeticSocketOverride> Overrides, const FMantleAvatarContract& Contract,
		TArray<FMantleCosmeticIssue>& OutIssues);

	/** Attach bounds the API enforces (A2.1): |location| <= 50 cm, |rotation| <= 180 degrees, scale 0.5 to 2. */
	static FVector3f ClampAttachLocation(const FVector3f& Location);
	static FVector3f ClampAttachRotation(const FVector3f& Rotation);
	static FVector3f ClampAttachScale(const FVector3f& Scale);
};
