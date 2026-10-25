// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleAssetStore.h"
#include "MantleNativeEye.h"
#include "MantleNativeHair.h"
#include "MantleClothingColor.h"

class FJsonValue;
class FJsonObject;
struct FMantleSkinnedMesh;

// ---- What the catalog says about an item (POST /avatar/items/batch) ---------------------------------
//
// The catalog's description of an item, shared by the plugin's cosmetic preview and the client's cosmetic cache.

/** What an avatar item is (backend avatar_items.kind). */
enum class EMantleCosmeticKind : uint8
{
	Avatar,
	Skinned,
	Rigid,
	Emote,
	/** A skin overlay (scar or tattoo, #736): a mask texture and an optional normal map, never a worn mesh. */
	Overlay,
	/** A kind this build does not know (a newer catalog, or the dropped 2D clothing): never loaded. */
	Unknown,
};

/**
 * One cooked asset an item names: where to download it and what the downloaded bytes must hash to.
 *
 * The hash is the cooked object's BLAKE3 ("cookedHash", F2 #336), which FMantleAssetStore verifies
 * every download against. An asset with a URL and no cooked hash cannot be verified, so the cache
 * does not load it (the item is reported NotLoadable), rather than trusting unverified bytes.
 */
struct MANTLERUNTIME_API FMantleCosmeticAssetRef
{
	FString Url;
	FMantleContentHash Hash;
	/** Size of the cooked object ("cookedBytes"); 0 when the API does not say. */
	int64 Bytes = 0;

	/** The API names this asset (a URL; null when the item has none of it or it is not approved yet). */
	bool IsPresent() const { return !Url.IsEmpty(); }
	/** Present and verifiable: what the store can fetch. */
	bool IsLoadable() const { return IsPresent() && Hash.IsValid(); }

	FMantleStoredAssetRef ToStoreRef() const;
};

/**
 * An avatar's socket override or a rigid accessory's attach offset as the catalog serves it (backend
 * avatar.Transform): location in cm and rotation in degrees (pitch, yaw, roll), both in Unreal axes, and a
 * per-axis scale. An absent part is the socket's own (an override) or identity (an attach offset). The
 * server refuses values past 50 cm, 180 degrees and scale 0.5 to 2; the client clamps them again
 * (FMantleAvatarAccessories).
 */
struct MANTLERUNTIME_API FMantleCosmeticTransform
{
	TOptional<FVector> Location;
	TOptional<FRotator> Rotation;
	TOptional<FVector> Scale;

	bool IsEmpty() const { return !Location.IsSet() && !Rotation.IsSet() && !Scale.IsSet(); }
};

/**
 * One mesh section's material: its role (skin, eyes, other, hair_cards) and cooked textures. Entry k is section k.
 * A hair_cards section (a groom's cards, #325, #326) has the cards attribute atlas as its albedo (coverage in R, seed
 * in B: data, not colour). Generic artist cards use HairColor. Native v2 cards additionally require
 * nativeHair with signed direction data and explicit source colour settings; normal/ORM keep their ordinary meanings.
 */
struct MANTLERUNTIME_API FMantleCosmeticNativeEye
{
	FMantleNativeEyeSettings Settings;
	TMap<FName, FMantleCosmeticAssetRef> Textures;
};

/** Catalog form: immutable cooked direction texture plus the explicit source colour recipe. */
struct MANTLERUNTIME_API FMantleCosmeticNativeHair
{
	FMantleNativeHairSettings Settings;
	FMantleCosmeticAssetRef Tangent;
};

struct MANTLERUNTIME_API FMantleCosmeticMaterial
{
	FString Role;
	/** Fixed client resource identifier; never a catalog-selected object path. */
	FString ClientMaterial;
	FMantleCosmeticAssetRef Albedo;
	FMantleCosmeticAssetRef Normal;
	FMantleCosmeticAssetRef ORM;
	/** hair_cards only ("hairColor": [r, g, b], linear); unset elsewhere. */
	TOptional<FLinearColor> HairColor;
	/** Avatar face eyes only: fixed native shader settings and approved verified textures. */
	TOptional<FMantleCosmeticNativeEye> NativeEye;
	/** Audited normalized native Groom atlas profile; empty preserves artist alpha handling. */
	FString NativeHairProfile;
	TOptional<FMantleCosmeticNativeHair> NativeHair;
	/** Optional fixed mask-driven clothing colour customization. */
	FMantleCosmeticAssetRef ColorMask;
	TArray<FMantleClothingColorRegion> ColorRegions;

	/** Role is hair_cards (limits.json sectionRoles; compared case-sensitively, as the API spells it). */
	bool IsHairCards() const;
};

/**
 * An avatar body's measurements as the catalog serves them ("bodyMetrics", #582: backend cook.AvatarMetrics,
 * limits.json contentLimits.avatar_body.metrics), in cm, from its reference pose. standingHeightCm is the top of the
 * mesh (the face's on a MetaHuman avatar); eyeHeightCm the mean height of its eye bones, absent without any. Null on
 * an item cooked before #582, and on anything that is not an avatar. The character's gameplay size (#581) is the
 * dedicated server's own read of these (POST /server/avatar/metrics), never a client's.
 */
struct MANTLERUNTIME_API FMantleAvatarBodyMetrics
{
	double StandingHeightCm = 0.0;
	TOptional<double> EyeHeightCm;
	double PelvisHeightCm = 0.0;
	double ArmLengthCm = 0.0;
	double LegLengthCm = 0.0;
	double ShoulderWidthCm = 0.0;
	double HipWidthCm = 0.0;
};

/** Approved internal source of one logical clothing item. */
struct MANTLERUNTIME_API FMantleCosmeticGarmentVariant
{
	FString Id;
	FString NativeSourceSize;
	FMantleCosmeticAssetRef Mesh;
	FMantleCosmeticAssetRef SourceShape;
	FMantleCosmeticAssetRef FaceMesh;
	FMantleCosmeticAssetRef Coverage;
	TArray<FMantleCosmeticMaterial> Materials;
};

/** An item with everything needed to load it (backend avatar.Item). */
struct MANTLERUNTIME_API FMantleCosmeticItem
{
	FString Id;
	/** The API's slot id ("avatar", "top", "acc_hat", ...). */
	FString Slot;
	EMantleCosmeticKind Kind = EMantleCosmeticKind::Unknown;
	/** Bumps whenever the item's assets change; the cache keys fits and built avatars by it. */
	int32 Version = 0;
	/** Seeded from a localDraft bundle entry (offline preview only): its id is registered with MantleCosmetic::RegisterLocalDraftId and never leaves the process. */
	bool bLocalDraft = false;
	/** Avatars: their appearance skeleton ("metahuman", "ue5mannequin", "ue4mannequin", "uefnmannequin") and whether they take clothing. */
	FString Skeleton;
	bool bGarmentCompatible = false;
	FString AvatarPublicationMode;
	/** Read-only server fact; absence/unknown defaults false. */
	bool bCustomizationEligible = false;

	FMantleCosmeticAssetRef Mesh;
	FMantleCosmeticAssetRef FaceMesh;
	FMantleCosmeticAssetRef Texture;
	FMantleCosmeticAssetRef Clip;
	FMantleCosmeticAssetRef SourceShape;
	FMantleCosmeticAssetRef Coverage;
	FMantleCosmeticAssetRef IrisMask;
	FMantleCosmeticAssetRef Dna;
	/** Overlays (#736): the optional scar normal map ("normal"). The mask is the item's "texture". */
	FMantleCosmeticAssetRef Normal;
	TArray<FMantleCosmeticMaterial> Materials;
	TArray<FMantleCosmeticMaterial> FaceMaterials;
	TArray<FMantleCosmeticGarmentVariant> GarmentVariants;
	FString DefaultGarmentVariant;
	FString GarmentVariantPolicy;

	/** Avatars: per-socket overrides ("acc_hat" ...) for bodies whose proportions differ from the reference body's. */
	TMap<FString, FMantleCosmeticTransform> Sockets;
	/** Avatars: the bone voice lip-sync opens ("jawBone"; the API defaults it to FACIAL_C_Jaw on an avatar
	 *  with a MetaHuman face, in the cooked bone table's spelling). Empty: the avatar has no jaw (A3.6). */
	FString JawBone;
	/** Rigid accessories: the offset from the slot's socket. */
	FMantleCosmeticTransform Attach;
	/** Avatars: the body's measurements (#582); unset when the catalog has none or they were unreadable. */
	TOptional<FMantleAvatarBodyMetrics> BodyMetrics;
	/**
	 * Avatars: "secondaryMotion" (#595), each mapped secondary motion chain's preset by chain id ("hair_a": "hair"), as the
	 * API spells them. A chain the body carries and this leaves out moves with its default preset. Empty when the item has
	 * none or it was unreadable.
	 */
	TMap<FString, FString> SecondaryMotion;

	/** Fields that came with a URL but no usable cooked hash, for the log ("mesh", "materials[0].albedo (sha256 ...)"). */
	TArray<FString> UnverifiedAssets;

	/** "<id>@<version>": what fits and built avatars are keyed by. */
	FString VersionKey() const;
};

/** What one resolve call got back. */
struct MANTLERUNTIME_API FMantleCosmeticResolveResult
{
	/** The API answered and the body was read. False is a transient failure: the ids are retried. */
	bool bSuccess = false;
	int32 StatusCode = 0;
	FString Error;
	TArray<FMantleCosmeticItem> Items;
	/** Ids the API does not serve (unknown, delisted, not approved): taken down, removed locally. */
	TArray<FString> Missing;
};

/** The item lookup under the cache and the plugin's preview: POST /avatar/items/batch for real, a stub in tests. */
class MANTLERUNTIME_API IMantleCosmeticResolver
{
public:
	virtual ~IMantleCosmeticResolver() = default;

	/** Called on the game thread with at most 100 ids (FMantleCosmeticCacheSettings::MaxIdsPerResolve in the client).
	 *  OnDone is called exactly once, from any thread. */
	virtual void Resolve(const TArray<FString>& ItemIds, TFunction<void(const FMantleCosmeticResolveResult&)> OnDone) = 0;
};

namespace MantleCosmetic
{
	/** The logical controls shared across an item's sections, for detail/creator UI. */
	MANTLERUNTIME_API bool ReadClothingColorRegions(const TSharedPtr<FJsonObject>& Object,
		TArray<FMantleClothingColorRegion>& Out, FString& OutWhyNot);
	/** limits.json's section role for a groom's cards (FMantleCosmeticMaterial::IsHairCards). */
	inline constexpr const TCHAR* HairCardsRole = TEXT("hair_cards");
	inline constexpr const TCHAR* NativeHairProfileV1 = TEXT("metahuman_cards_v1");
	inline constexpr const TCHAR* NativeHairMaskedCardsV1 = TEXT("metahuman_masked_cards_v1");
	inline constexpr const TCHAR* NativeHairTranslucentCardsV1 = TEXT("metahuman_translucent_cards_v1");

	/** Shader family is independent of the body/face component that animates cards. */
	inline constexpr const TCHAR* NativeHairMaskedCardsV2 = TEXT("metahuman_masked_cards_v2");
	inline constexpr const TCHAR* NativeHairTranslucentCardsV2 = TEXT("metahuman_translucent_cards_v2");
	enum class ENativeHairProfile : uint8 { Artist, Legacy, MaskedCards, TranslucentCards, MaskedCardsV2, TranslucentCardsV2, Unsupported };
	inline bool RequiresNativeHairSettings(ENativeHairProfile Profile)
	{ return Profile == ENativeHairProfile::MaskedCardsV2 || Profile == ENativeHairProfile::TranslucentCardsV2; }
	inline ENativeHairProfile NativeHairProfileFromString(const FString& Value)
	{
		if (Value.IsEmpty()) return ENativeHairProfile::Artist;
		if (Value == NativeHairProfileV1) return ENativeHairProfile::Legacy;
		if (Value == NativeHairMaskedCardsV2) return ENativeHairProfile::MaskedCardsV2;
		if (Value == NativeHairTranslucentCardsV2) return ENativeHairProfile::TranslucentCardsV2;
		if (Value == NativeHairMaskedCardsV1) return ENativeHairProfile::MaskedCards;
		if (Value == NativeHairTranslucentCardsV1) return ENativeHairProfile::TranslucentCards;
		return ENativeHairProfile::Unsupported;
	}
	inline const TCHAR* NativeHairParentPath(ENativeHairProfile Profile, bool bLegacyFacial)
	{
		if (Profile == ENativeHairProfile::MaskedCardsV2) return TEXT("/Game/Mantle/Materials/Hair/MI_MantleHair_NativeMaskedV2.MI_MantleHair_NativeMaskedV2");
		if (Profile == ENativeHairProfile::TranslucentCardsV2) return TEXT("/Game/Mantle/Materials/Hair/MI_MantleHair_NativeTranslucentV2.MI_MantleHair_NativeTranslucentV2");
		if (Profile == ENativeHairProfile::Legacy) Profile = bLegacyFacial ? ENativeHairProfile::TranslucentCards : ENativeHairProfile::MaskedCards;
		if (Profile == ENativeHairProfile::MaskedCards) return TEXT("/Game/Mantle/Materials/Hair/MI_MantleHair_NativeCards.MI_MantleHair_NativeCards");
		if (Profile == ENativeHairProfile::TranslucentCards) return TEXT("/Game/Mantle/Materials/Hair/MI_MantleHair_NativeFacialCards.MI_MantleHair_NativeFacialCards");
		return nullptr;
	}

	/** "avatar", "skinned", "rigid", "emote"; Unknown for anything else. */
	MANTLERUNTIME_API EMantleCosmeticKind KindFromApi(const FString& Value);
	MANTLERUNTIME_API const TCHAR* KindName(EMantleCosmeticKind Kind);
	/** Any section binding opts the whole avatar into the geometry-only client profile. */
	MANTLERUNTIME_API bool UsesClientMaterials(const FMantleCosmeticItem& Item);
	MANTLERUNTIME_API bool ValidateClientMaterials(const FMantleCosmeticItem& Item, FString& OutWhyNot);
	/** Fixed body/face resource paths; eyes use the native procedural family. */
	MANTLERUNTIME_API FString ClientMaterialPath(const FString& Binding);
	MANTLERUNTIME_API bool IsClientMaterialBinding(const FString& Binding, const FString& Role, bool bFace);
	/** Verify every decoded LOD's semantic bindings before assigning client-owned resources. */
	MANTLERUNTIME_API bool ValidateClientMaterialGeometry(const FMantleSkinnedMesh& Mesh,
		const TArray<FMantleCosmeticMaterial>& Materials, bool bFace, FString& OutWhyNot);

	/** itm_u + 32 lowercase hex digits (the schema's item id pattern). */
	MANTLERUNTIME_API bool IsItemId(const FString& Value);

	/**
	 * Local-draft item ids (an avatar a creator is testing from an offline preview, never published). The offline
	 * preview registers each id it seeds from a localDraft bundle entry; nothing else does. A registered id is not a
	 * catalog id: no request to the backend and no replicated appearance may carry one. Thread-safe.
	 */
	MANTLERUNTIME_API void RegisterLocalDraftId(const FString& ItemId);
	MANTLERUNTIME_API bool IsLocalDraftId(const FString& ItemId);
	/** Whether Text (a request path or body, a serialized descriptor) names any registered local-draft id. */
	MANTLERUNTIME_API bool ContainsLocalDraftId(const FString& Text);
	/** Test seam: forgets every registered id. */
	MANTLERUNTIME_API void ClearLocalDraftIdsForTesting();

	/** The POST /avatar/items/batch body, {"ids":[...]}, condensed JSON. */
	MANTLERUNTIME_API FString BuildItemBatchBody(const TArray<FString>& ItemIds);

	/** Strict server publication fact. No descriptor or editor checkbox can set eligibility. */
	MANTLERUNTIME_API bool ReadAvatarPublication(const TSharedPtr<FJsonObject>& Object, const FString& Where,
		bool bAvatar, const FString& Skeleton, bool bGarmentCompatible, FString& OutMode, bool& bOutEligible, FString& OutWhyNot);

	/**
	 * POST /avatar/items/batch's {items, missing} read for loading: each item's kind, version, skeleton and every
	 * cooked asset with its URL and cooked hash. An asset reference is read in either of two shapes, "<name>":
	 * {url, cookedHash, cookedBytes} or the flat "<name>Url" / "<name>CookedHash" / "<name>CookedBytes" (materials:
	 * albedo, normal, orm); null or absent means none. A URL without a usable blake3 cooked hash is kept, named in
	 * the item's UnverifiedAssets, and never fetched. Items of an unknown kind are kept with kind Unknown; an item
	 * that is not an object, or has no valid id, fails the whole answer.
	 */
	MANTLERUNTIME_API bool ParseItemBatch(const FString& Json, TArray<FMantleCosmeticItem>& OutItems, TArray<FString>& OutMissing,
		FString& OutWhyNot);

	/**
	 * An item's "secondaryMotion" (#595): an object of chain id to preset id, both strings. Absent or null: true and Out
	 * empty. Ids are kept as sent (the runtime ignores ones it does not know). Anything else is false with a why-not and
	 * Out empty. Where prefixes the why-not.
	 */
	MANTLERUNTIME_API bool ReadSecondaryMotion(const TSharedPtr<FJsonValue>& Value, const FString& Where, TMap<FString, FString>& Out,
		FString& OutWhyNot);

	/**
	 * An item's "bodyMetrics" (#582). Absent or null: true with Out unset. An object: its six lengths are required,
	 * finite and not negative (standingHeightCm above 0), eyeHeightCm optional (absent or null: unset); keys it does not
	 * know are ignored. Anything else is false with a why-not, and Out unset. Where prefixes the why-not.
	 */
	MANTLERUNTIME_API bool ReadBodyMetrics(const TSharedPtr<FJsonValue>& Value, const FString& Where,
		TOptional<FMantleAvatarBodyMetrics>& Out, FString& OutWhyNot);

	/**
	 * POST /server/avatar/metrics's answer (#581), {items: [{id, bodyMetrics}], missing: [id]}: every item's metrics by
	 * id (unset for an item with none, or whose metrics are unreadable), and the ids the catalog does not serve. False
	 * when the answer is not that shape.
	 */
	MANTLERUNTIME_API bool ParseServerBodyMetrics(const FString& Json, TMap<FString, TOptional<FMantleAvatarBodyMetrics>>& OutMetrics,
		TArray<FString>& OutMissing, FString& OutWhyNot);

	/** POST /avatar/items/batch through FMantleApiClient (the process's API address and sign-in: a player's in the
	 *  client, the creator's in the editor). */
	MANTLERUNTIME_API TSharedRef<IMantleCosmeticResolver> CreateApiResolver();
}
