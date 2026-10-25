// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCosmeticPackage.h"
#include "MantleCosmeticItem.h"
#include "MantleAvatarContract.h"
#include "MantleHairColor.h"
#include "MantleOverlayChecks.h"
#include "MantleSha256.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

const TCHAR* LexToString(EMantleCosmeticClass Class)
{
	switch (Class)
	{
	case EMantleCosmeticClass::Hair: return TEXT("Hair");
	case EMantleCosmeticClass::Garment: return TEXT("Clothing");
	case EMantleCosmeticClass::Accessory: return TEXT("Accessory");
	case EMantleCosmeticClass::Avatar: return TEXT("Avatar");
	case EMantleCosmeticClass::Emote: return TEXT("Emote");
	case EMantleCosmeticClass::Overlay: return TEXT("Overlay");
	default: return TEXT("Item");
	}
}

const TCHAR* ContractItemClass(EMantleCosmeticClass Class)
{
	switch (Class)
	{
	case EMantleCosmeticClass::Hair:
	case EMantleCosmeticClass::Garment: return TEXT("skinned_garment");
	case EMantleCosmeticClass::Accessory: return TEXT("rigid_accessory");
	case EMantleCosmeticClass::Avatar: return TEXT("avatar_body");
	case EMantleCosmeticClass::Emote: return TEXT("emote");
	case EMantleCosmeticClass::Overlay: return TEXT("overlay");
	default: return TEXT("");
	}
}

TArray<FString> CosmeticSlotOptions(const FMantleAvatarContract& Contract, EMantleCosmeticClass Class)
{
	TArray<FString> Options;
	if (Class == EMantleCosmeticClass::Overlay)
	{
		return FMantleOverlayChecks::Slots();
	}
	const FString ItemClass = ContractItemClass(Class);
	for (const FString& Slot : Contract.SlotsOfClass(ItemClass))
	{
		// Hair items take the hair slots only; garments every other skinned slot.
		const bool bHairSlot = Contract.HairSlots.Contains(Slot);
		if (ItemClass == TEXT("skinned_garment") && bHairSlot != (Class == EMantleCosmeticClass::Hair))
		{
			continue;
		}
		Options.Add(Slot);
	}
	return Options;
}

const FMantleCosmeticUploadAsset* FMantleCosmeticPackage::FindAsset(const FString& Key) const
{
	return Assets.FindByPredicate([&Key](const FMantleCosmeticUploadAsset& Asset) { return Asset.Key == Key; });
}

FMantleCosmeticUploadAsset& FMantleCosmeticPackage::AddAsset(const FString& Key, const FString& Label,
	const FMantleAssetUploadOptions& Options, TArray<uint8>&& Bytes)
{
	FMantleCosmeticUploadAsset* Asset = Assets.FindByPredicate([&Key](const FMantleCosmeticUploadAsset& Existing) { return Existing.Key == Key; });
	if (!Asset)
	{
		Asset = &Assets.AddDefaulted_GetRef();
	}
	Asset->Key = Key;
	Asset->Label = Label;
	Asset->Options = Options;
	Asset->Bytes = MoveTemp(Bytes);
	Asset->Hash = FMantleSha256::AssetHash(Asset->Bytes);
	return *Asset;
}

void FMantleCosmeticPackage::CheckSocketOverrides(TConstArrayView<FMantleCosmeticSocketOverride> Overrides, const FMantleAvatarContract& Contract,
	TArray<FMantleCosmeticIssue>& OutIssues)
{
	// The accessory sockets and limits.json's toolSockets (hand_rSocket), as POST /avatar/items takes them.
	TArray<FString> Known = Contract.AccessorySockets();
	for (const FMantleContractToolSocket& Tool : Contract.ToolSockets)
	{
		Known.AddUnique(Tool.Socket);
	}
	TSet<FString> Seen;
	for (const FMantleCosmeticSocketOverride& Override : Overrides)
	{
		if (Override.IsEmpty())
		{
			continue;
		}
		if (!Known.Contains(Override.Socket))
		{
			OutIssues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("\"%s\" is not an accessory or tool socket."), *Override.Socket),
				FString::Printf(TEXT("The sockets are %s."), *FString::Join(Known, TEXT(", ")))));
			continue;
		}
		bool bAlreadySeen = false;
		Seen.Add(Override.Socket, &bAlreadySeen);
		if (bAlreadySeen)
		{
			OutIssues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("the %s socket is overridden twice."), *Override.Socket)));
			continue;
		}
		// A tool socket stays on its bone and, with unitScale, at scale 1: it carries the tool, and a scaled socket
		// scales what it carries. Same words as the API (avatar.checkSocketOverride).
		bool bCheckScale = Override.bScale;
		if (const FMantleContractToolSocket* Tool = Contract.FindToolSocket(Override.Socket))
		{
			if (!Override.Bone.IsEmpty() && Override.Bone != Tool->SocketBone)
			{
				OutIssues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("%s hangs from %s, not %s."), *Override.Socket, *Tool->SocketBone, *Override.Bone),
					FString::Printf(TEXT("Move the socket back onto %s in the skeleton."), *Tool->SocketBone)));
				continue;
			}
			if (Tool->bUnitScale && Override.bScale && Override.Scale != FVector3f::OneVector)
			{
				OutIssues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("%s carries tools, so its scale stays 1."), *Override.Socket),
					TEXT("Turn its scale off, or set it to 1, 1, 1.")));
				bCheckScale = false;
			}
		}
		FMantleCosmeticChecks::CheckTransform(FString::Printf(TEXT("the %s socket's"), *Override.Socket),
			Override.bLocation ? &Override.Location : nullptr,
			Override.bRotation ? &Override.Rotation : nullptr,
			bCheckScale ? &Override.Scale : nullptr, OutIssues);
	}
}

FVector3f FMantleCosmeticPackage::ClampAttachLocation(const FVector3f& Location)
{
	return FVector3f(FMath::Clamp(Location.X, -50.0f, 50.0f), FMath::Clamp(Location.Y, -50.0f, 50.0f), FMath::Clamp(Location.Z, -50.0f, 50.0f));
}

FVector3f FMantleCosmeticPackage::ClampAttachRotation(const FVector3f& Rotation)
{
	return FVector3f(FMath::Clamp(Rotation.X, -180.0f, 180.0f), FMath::Clamp(Rotation.Y, -180.0f, 180.0f), FMath::Clamp(Rotation.Z, -180.0f, 180.0f));
}

FVector3f FMantleCosmeticPackage::ClampAttachScale(const FVector3f& Scale)
{
	return FVector3f(FMath::Clamp(Scale.X, 0.5f, 2.0f), FMath::Clamp(Scale.Y, 0.5f, 2.0f), FMath::Clamp(Scale.Z, 0.5f, 2.0f));
}

static bool ValidateNativeHairRef(const FMantleCosmeticMaterialRef& Ref, EMantleCosmeticClass Class, bool bFaceMaterials, FString& OutError)
{
	const auto NativeProfile = MantleCosmetic::NativeHairProfileFromString(Ref.NativeHairProfile);
	const bool bTyped = MantleCosmetic::RequiresNativeHairSettings(NativeProfile);
	if (NativeProfile == MantleCosmetic::ENativeHairProfile::Unsupported || bTyped != Ref.NativeHair.IsSet()
		|| (bTyped && (Class != EMantleCosmeticClass::Hair || bFaceMaterials || Ref.Role != MantleHairCardsRole
			|| !Ref.HairColor.IsSet() || !Ref.NativeHair->IsValid() || Ref.NativeHairTangentKey.IsEmpty() || Ref.AlbedoKey.IsEmpty())))
	{
		OutError = TEXT("Native v2 hair needs the exact hair_cards family, bounded source recipe, attribute atlas and typed tangent dependency.");
		return false;
	}
	return true;
}

bool FMantleCosmeticPackage::MakeItemJson(const TMap<FString, FString>& IdsByKey, FString& OutJson, FString& OutError) const
{
	OutJson.Reset();
	bool bHasClientMaterial = false;
	for (const TArray<FMantleCosmeticMaterialRef>* Refs : { &Materials, &FaceMaterials })
	{
		for (const FMantleCosmeticMaterialRef& Ref : *Refs) bHasClientMaterial |= !Ref.ClientMaterial.IsEmpty();
	}
	const bool bClientAppearance = AvatarPublicationMode == TEXT("converted");
	if (bHasClientMaterial != bClientAppearance || (bClientAppearance && Class != EMantleCosmeticClass::Avatar))
	{
		OutError = TEXT("Converted avatars need the geometry-only client appearance profile; other upload routes keep their established materials.");
		return false;
	}
	if (bClientAppearance)
	{
		if (MeshKey.IsEmpty() || FaceMeshKey.IsEmpty())
		{
			OutError = TEXT("A converted avatar package needs both supported body and face geometry.");
			return false;
		}
		TSet<FString> Seen;
		int32 BodySkin = 0, FaceSkin = 0, LeftEye = 0, RightEye = 0;
		for (const TArray<FMantleCosmeticMaterialRef>* Refs : { &Materials, &FaceMaterials })
		{
			for (const FMantleCosmeticMaterialRef& Ref : *Refs)
			{
				const bool bFaceAuxiliaryMaterial = Ref.ClientMaterial == TEXT("teeth_v1")
					|| Ref.ClientMaterial == TEXT("eye_occlusion_v1") || Ref.ClientMaterial == TEXT("eye_shell_v1")
					|| Ref.ClientMaterial == TEXT("eyelashes_v1");
				const bool bMaterialBelongsToField = Refs == &Materials
					? Ref.ClientMaterial == TEXT("skin_body_v1")
					: Ref.ClientMaterial == TEXT("skin_face_v1") || Ref.ClientMaterial == TEXT("eye_left_v1")
						|| Ref.ClientMaterial == TEXT("eye_right_v1") || bFaceAuxiliaryMaterial;
				const bool bAppearanceDependency = !Ref.AlbedoKey.IsEmpty() || !Ref.NormalKey.IsEmpty() || !Ref.OrmKey.IsEmpty()
					|| !Ref.ColorMaskKey.IsEmpty() || !Ref.ColorRegions.IsEmpty() || Ref.HairColor.IsSet() || Ref.NativeEye.IsSet()
					|| !Ref.NativeEyeTextureKeys.IsEmpty() || !Ref.NativeHairProfile.IsEmpty() || Ref.NativeHair.IsSet()
					|| !Ref.NativeHairTangentKey.IsEmpty();
				const bool bRoleMatches = (Ref.ClientMaterial == TEXT("skin_body_v1") || Ref.ClientMaterial == TEXT("skin_face_v1"))
					? Ref.Role == TEXT("skin")
					: (Ref.ClientMaterial == TEXT("eye_left_v1") || Ref.ClientMaterial == TEXT("eye_right_v1"))
						? Ref.Role == TEXT("eyes") : bFaceAuxiliaryMaterial && Ref.Role == TEXT("other");
				if (Ref.ClientMaterial.IsEmpty() || !bMaterialBelongsToField || bAppearanceDependency || !bRoleMatches
					|| Seen.Contains(Ref.ClientMaterial))
				{
					OutError = TEXT("Converted material rows contain only one approved clientMaterial and its required role; appearance dependencies are client-owned.");
					return false;
				}
				Seen.Add(Ref.ClientMaterial);
				if (Ref.ClientMaterial == TEXT("skin_body_v1") && Ref.Role == TEXT("skin") && Refs == &Materials) ++BodySkin;
				else if (Ref.ClientMaterial == TEXT("skin_face_v1") && Ref.Role == TEXT("skin") && Refs == &FaceMaterials) ++FaceSkin;
				else if (Ref.ClientMaterial == TEXT("eye_left_v1") && Ref.Role == TEXT("eyes") && Refs == &FaceMaterials) ++LeftEye;
				else if (Ref.ClientMaterial == TEXT("eye_right_v1") && Ref.Role == TEXT("eyes") && Refs == &FaceMaterials) ++RightEye;
				else if (Ref.ClientMaterial != TEXT("skin_body_v1") && Ref.ClientMaterial != TEXT("skin_face_v1")
					&& Ref.ClientMaterial != TEXT("eye_left_v1") && Ref.ClientMaterial != TEXT("eye_right_v1")
					&& Ref.ClientMaterial != TEXT("teeth_v1") && Ref.ClientMaterial != TEXT("eye_occlusion_v1")
					&& Ref.ClientMaterial != TEXT("eye_shell_v1") && Ref.ClientMaterial != TEXT("eyelashes_v1"))
				{
					OutError = TEXT("Converted upload names an unapproved client material.");
					return false;
				}
			}
		}
		if (BodySkin != 1 || FaceSkin != 1 || LeftEye != 1 || RightEye != 1)
		{
			OutError = TEXT("A converted avatar needs one body skin, one face skin, and independent left and right client eye sections.");
			return false;
		}
		for (const FMantleCosmeticUploadAsset& Asset : Assets)
		{
			if (Asset.Options.Kind == TEXT("texture") && Asset.Key != ThumbnailKey)
			{
				OutError = TEXT("Converted uploads cannot include appearance texture assets.");
				return false;
			}
		}
	}
	auto CheckColors = [this, &OutError](const TArray<FMantleCosmeticMaterialRef>& Refs, TMap<FString, TPair<FString, FColor>>& Logical) -> bool
	{
		for (const FMantleCosmeticMaterialRef& Ref : Refs)
		{
			if (!ValidateNativeHairRef(Ref, Class, &Refs == &FaceMaterials, OutError)) return false;
			if (Ref.ColorRegions.IsEmpty() && Ref.ColorMaskKey.IsEmpty()) continue;
			// Clothing (top, bottom, footwear) and rigid accessories (a hat) carry colour areas.
			const bool bColorClass = (Class == EMantleCosmeticClass::Garment && (Slot == TEXT("top") || Slot == TEXT("bottom") || Slot == TEXT("footwear")))
				|| Class == EMantleCosmeticClass::Accessory;
			if (!bColorClass || Ref.ColorMaskKey.IsEmpty() || Ref.ColorRegions.IsEmpty() || !MantleClothingColor::ValidateRegions(Ref.ColorRegions, OutError))
			{ OutError = TEXT("Color declarations need a clothing or accessory material, mask and measured regions."); return false; }
			for (const FMantleClothingColorRegion& Region : Ref.ColorRegions)
			{
				const TPair<FString, FColor> Definition(Region.Label, Region.DefaultColor);
				if (const auto* Existing = Logical.Find(Region.Id); Existing && *Existing != Definition)
				{ OutError = TEXT("A logical color region must keep one label and default across materials."); return false; }
				Logical.Add(Region.Id, Definition);
			}
		}
		if (Logical.Num() > MantleClothingColor::MaxColorsPerItem) { OutError = TEXT("One clothing item declares at most eight logical color regions."); return false; }
		return true;
	};
	TMap<FString, TPair<FString, FColor>> DefaultColors;
	if (!CheckColors(Materials, DefaultColors)) return false;
	for (const FMantleCosmeticGarmentVariantExport& Variant : GarmentVariants)
	{
		TMap<FString, TPair<FString, FColor>> VariantColors;
		if (!CheckColors(Variant.Materials, VariantColors)) return false;
		bool bSame = VariantColors.Num() == DefaultColors.Num();
		for (const auto& Definition : DefaultColors)
		{
			const auto* Other = VariantColors.Find(Definition.Key);
			bSame &= Other && *Other == Definition.Value;
		}
		if (!bSame) { OutError = TEXT("All internal fit variants must declare the same logical clothing controls."); return false; }
	}
	for (const FMantleCosmeticMaterialRef& Ref : FaceMaterials)
	{
		if (!ValidateNativeHairRef(Ref, Class, true, OutError)) return false;
		if (!Ref.ColorRegions.IsEmpty() || !Ref.ColorMaskKey.IsEmpty())
		{ OutError = TEXT("Clothing color declarations need a clothing material, mask and measured regions."); return false; }
	}
	FString Missing;
	auto IdOf = [&IdsByKey, &Missing](const FString& Key) -> FString
	{
		if (Key.IsEmpty())
		{
			return FString();
		}
		if (const FString* Id = IdsByKey.Find(Key))
		{
			return *Id;
		}
		if (Missing.IsEmpty())
		{
			Missing = Key;
		}
		return FString();
	};

	const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
    if ((!AvatarPublicationMode.IsEmpty() && AvatarPublicationMode!=TEXT("converted") && AvatarPublicationMode!=TEXT("as_is"))
        || (!AvatarPublicationMode.IsEmpty() && Class!=EMantleCosmeticClass::Avatar))
    { OutError=TEXT("avatar publication mode must be converted/as_is and belongs only to avatar items"); return false; }
    if (Class==EMantleCosmeticClass::Avatar && !AvatarPublicationMode.IsEmpty()) Item->SetStringField(TEXT("avatarPublicationMode"),AvatarPublicationMode);
	// Public items carry the creator's rights declaration; the server refuses one without it.
	if (Visibility != TEXT("public") && Visibility != TEXT("private"))
	{
		OutError = TEXT("the visibility must be public or private");
		return false;
	}
	if (Visibility == TEXT("public") && RightsDeclarationVersion.IsEmpty())
	{
		OutError = TEXT("declare rights or choose Private: a public item needs the rights declaration");
		return false;
	}
	Item->SetStringField(TEXT("visibility"), Visibility);
	if (Visibility == TEXT("public"))
	{
		Item->SetStringField(TEXT("rightsDeclarationVersion"), RightsDeclarationVersion);
	}
	// Moderators see where a public item came from. Private items send nothing.
	Provenance.AddTo(*Item, Visibility == TEXT("public"));
	Item->SetStringField(TEXT("slot"), Slot);
	Item->SetStringField(TEXT("name"), Name.TrimStartAndEnd());
	Item->SetStringField(TEXT("description"), Description.TrimStartAndEnd());
	Item->SetStringField(TEXT("thumbnailAssetId"), IdOf(ThumbnailKey));

	auto SetOptional = [&Item, &IdOf](const TCHAR* Field, const FString& Key)
	{
		if (!Key.IsEmpty())
		{
			Item->SetStringField(Field, IdOf(Key));
		}
	};
	auto MaterialsJson = [this, &IdOf](const TArray<FMantleCosmeticMaterialRef>& Refs)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FMantleCosmeticMaterialRef& Ref : Refs)
		{
			const TSharedRef<FJsonObject> Material = MakeShared<FJsonObject>();
			if (!Ref.ClientMaterial.IsEmpty())
			{
				Material->SetStringField(TEXT("role"), Ref.Role);
				Material->SetStringField(TEXT("clientMaterial"), Ref.ClientMaterial);
				Out.Add(MakeShared<FJsonValueObject>(Material));
				continue;
			}
			Material->SetStringField(TEXT("albedoAssetId"), IdOf(Ref.AlbedoKey));
			// limits.json: avatars take albedo, normal and ORM; skinned items albedo and normal;
			// accessories albedo only (the API refuses the rest as item_field_not_allowed).
			if (!Ref.NormalKey.IsEmpty() && Class != EMantleCosmeticClass::Accessory)
			{
				Material->SetStringField(TEXT("normalAssetId"), IdOf(Ref.NormalKey));
			}
			if (!Ref.OrmKey.IsEmpty() && Class == EMantleCosmeticClass::Avatar)
			{
				Material->SetStringField(TEXT("ormAssetId"), IdOf(Ref.OrmKey));
			}
			// Only avatars may say skin or eyes; other is the default and never needs saying.
			if (Class == EMantleCosmeticClass::Avatar && (Ref.Role == TEXT("skin") || Ref.Role == TEXT("eyes")))
			{
				Material->SetStringField(TEXT("role"), Ref.Role);
			}
			if (Class == EMantleCosmeticClass::Avatar && Ref.Role == TEXT("eyes") && Ref.NativeEye.IsSet())
			{
				TSharedRef<FJsonObject> Eye = FMantleNativeEye::WriteSettings(Ref.NativeEye.GetValue());
				TSharedRef<FJsonObject> Textures = MakeShared<FJsonObject>();
				for (const TCHAR* Parameter : FMantleNativeEye::TextureParameters())
					if (const FString* Key = Ref.NativeEyeTextureKeys.Find(FName(Parameter))) Textures->SetStringField(Parameter, IdOf(*Key));
				Eye->SetObjectField(TEXT("textureAssetIds"), Textures);
				Material->SetObjectField(TEXT("nativeEye"), Eye);
			}
			if (!Ref.ColorRegions.IsEmpty())
			{
				const TSharedRef<FJsonObject> Declaration = MakeShared<FJsonObject>();
				Declaration->SetNumberField(TEXT("version"), 1);
				Declaration->SetStringField(TEXT("mode"), MantleClothingColor::Mode);
				Declaration->SetStringField(TEXT("maskAssetId"), IdOf(Ref.ColorMaskKey));
				TArray<TSharedPtr<FJsonValue>> Regions;
				for (const FMantleClothingColorRegion& Region : Ref.ColorRegions)
				{
					const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
					Row->SetStringField(TEXT("id"), Region.Id); Row->SetStringField(TEXT("label"), Region.Label);
					Row->SetStringField(TEXT("channel"), Region.Channel == 0 ? TEXT("r") : Region.Channel == 1 ? TEXT("g") : TEXT("b"));
					Row->SetStringField(TEXT("defaultColor"), TEXT("#") + Region.DefaultColor.ToHex().Left(6).ToLower());
					Row->SetNumberField(TEXT("referenceLuminance"), Region.ReferenceLuminance);
					Regions.Add(MakeShared<FJsonValueObject>(Row));
				}
				Declaration->SetArrayField(TEXT("regions"), Regions);
				Material->SetObjectField(TEXT("colorCustomization"), Declaration);
			}
			// Only hair may say hair_cards (a groom's cards), always with the colour they are drawn in: the API requires
			// it, linear RGB in [0, 1].
			if (Class == EMantleCosmeticClass::Hair && Ref.Role.Equals(MantleHairCardsRole, ESearchCase::CaseSensitive))
			{
				Material->SetStringField(TEXT("role"), MantleHairCardsRole);
				const FLinearColor Color = Ref.HairColor.Get(FMantleHairColor::DefaultColor());
				TArray<TSharedPtr<FJsonValue>> Channels;
				for (const float Channel : { Color.R, Color.G, Color.B })
				{
					Channels.Add(MakeShared<FJsonValueNumber>(FMath::Clamp(static_cast<double>(Channel), 0.0, 1.0)));
				}
				Material->SetArrayField(TEXT("hairColor"), Channels);
				if (!Ref.NativeHairProfile.IsEmpty()) Material->SetStringField(TEXT("nativeHairProfile"), Ref.NativeHairProfile);
                if (Ref.NativeHair.IsSet())
                {
                    const FMantleNativeHairSettings& Settings = Ref.NativeHair.GetValue();
                    const TSharedRef<FJsonObject> Hair = MakeShared<FJsonObject>();
                    Hair->SetNumberField(TEXT("version"), Settings.Version);
                    Hair->SetStringField(TEXT("tangentAssetId"), IdOf(Ref.NativeHairTangentKey));
                    Hair->SetNumberField(TEXT("melanin"), Settings.Melanin); Hair->SetNumberField(TEXT("redness"), Settings.Redness);
                    TArray<TSharedPtr<FJsonValue>> Dye;
                    for (float Value : {Settings.DyeColor.R, Settings.DyeColor.G, Settings.DyeColor.B}) Dye.Add(MakeShared<FJsonValueNumber>(Value));
                    Hair->SetArrayField(TEXT("dyeColor"), Dye);
                    Material->SetObjectField(TEXT("nativeHair"), Hair);
                }
			}
			Out.Add(MakeShared<FJsonValueObject>(Material));
		}
		return Out;
	};
	auto Vector = [](const FVector3f& Value)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Add(MakeShared<FJsonValueNumber>(Value.X));
		Out.Add(MakeShared<FJsonValueNumber>(Value.Y));
		Out.Add(MakeShared<FJsonValueNumber>(Value.Z));
		return Out;
	};

	switch (Class)
	{
	case EMantleCosmeticClass::Avatar:
		Item->SetStringField(TEXT("meshAssetId"), IdOf(MeshKey));
		Item->SetArrayField(TEXT("materials"), MaterialsJson(Materials));
		if (!FaceMeshKey.IsEmpty())
		{
			Item->SetStringField(TEXT("faceMeshAssetId"), IdOf(FaceMeshKey));
			Item->SetArrayField(TEXT("faceMaterials"), MaterialsJson(FaceMaterials));
		}
		if (!JawBone.IsNone())
		{
			Item->SetStringField(TEXT("jawBone"), JawBone.ToString());
		}
		if (SecondaryMotion.Num() > 0)
		{
			// Chain id to preset id (backend avatar.ItemInput.SecondaryMotion).
			const TSharedRef<FJsonObject> Motion = MakeShared<FJsonObject>();
			for (const TPair<FString, FString>& Chain : SecondaryMotion)
			{
				Motion->SetStringField(Chain.Key, Chain.Value);
			}
			Item->SetObjectField(TEXT("secondaryMotion"), Motion);
		}
		{
			// Only the parts the creator turned on: an absent part is the socket's own (backend avatar.Transform).
			const TSharedRef<FJsonObject> SocketsJson = MakeShared<FJsonObject>();
			for (const FMantleCosmeticSocketOverride& Override : Sockets)
			{
				if (Override.IsEmpty())
				{
					continue;
				}
				const TSharedRef<FJsonObject> Transform = MakeShared<FJsonObject>();
				if (Override.bLocation)
				{
					Transform->SetArrayField(TEXT("location"), Vector(Override.Location));
				}
				if (Override.bRotation)
				{
					Transform->SetArrayField(TEXT("rotation"), Vector(Override.Rotation));
				}
				if (Override.bScale)
				{
					Transform->SetArrayField(TEXT("scale"), Vector(Override.Scale));
				}
				SocketsJson->SetObjectField(Override.Socket, Transform);
			}
			if (SocketsJson->Values.Num() > 0)
			{
				Item->SetObjectField(TEXT("sockets"), SocketsJson);
			}
		}
		break;

	case EMantleCosmeticClass::Hair:
	case EMantleCosmeticClass::Garment:
		Item->SetStringField(TEXT("meshAssetId"), IdOf(MeshKey));
		if (Class != EMantleCosmeticClass::Hair || !SourceShapeKey.IsEmpty() || FaceMeshKey.IsEmpty())
			Item->SetStringField(TEXT("sourceShapeAssetId"), IdOf(SourceShapeKey));
		Item->SetArrayField(TEXT("materials"), MaterialsJson(Materials));
		SetOptional(TEXT("coverageAssetId"), CoverageKey);
		// The face it was authored with, a fit input only (no faceMaterials: it is never drawn).
		SetOptional(TEXT("faceMeshAssetId"), FaceMeshKey);
		break;

	case EMantleCosmeticClass::Accessory:
		Item->SetStringField(TEXT("meshAssetId"), IdOf(MeshKey));
		Item->SetArrayField(TEXT("materials"), MaterialsJson(Materials));
		if (bAttach)
		{
			const TSharedRef<FJsonObject> Attach = MakeShared<FJsonObject>();
			Attach->SetArrayField(TEXT("location"), Vector(ClampAttachLocation(AttachLocation)));
			Attach->SetArrayField(TEXT("rotation"), Vector(ClampAttachRotation(AttachRotation)));
			Attach->SetArrayField(TEXT("scale"), Vector(ClampAttachScale(AttachScale)));
			Item->SetObjectField(TEXT("attach"), Attach);
		}
		break;

	case EMantleCosmeticClass::Emote:
		Item->SetStringField(TEXT("clipAssetId"), IdOf(ClipKey));
		break;

	case EMantleCosmeticClass::Overlay:
		// The mask is required (the API's textureAssetId); the normal map is for the scar slots only (Build refuses it on a tattoo).
		Item->SetStringField(TEXT("textureAssetId"), IdOf(MaskKey));
		SetOptional(TEXT("normalAssetId"), OverlayNormalKey);
		break;
	}

	if (Class == EMantleCosmeticClass::Garment && !GarmentVariants.IsEmpty())
	{
		TSharedRef<FJsonObject> Manifest = MakeShared<FJsonObject>();
		Manifest->SetNumberField(TEXT("version"), 1);
		Manifest->SetStringField(TEXT("policy"), TEXT("body-rms-v1"));
		Manifest->SetStringField(TEXT("defaultVariant"), DefaultGarmentVariant);
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const FMantleCosmeticGarmentVariantExport& Variant : GarmentVariants)
		{
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("id"), Variant.Id);
			Row->SetStringField(TEXT("nativeSourceSize"), Variant.NativeSourceSize);
			Row->SetStringField(TEXT("meshAssetId"), IdOf(Variant.MeshKey));
			Row->SetStringField(TEXT("sourceShapeAssetId"), IdOf(Variant.SourceShapeKey));
			if (!Variant.FaceMeshKey.IsEmpty()) { Row->SetStringField(TEXT("faceMeshAssetId"), IdOf(Variant.FaceMeshKey)); }
			if (!Variant.CoverageKey.IsEmpty()) { Row->SetStringField(TEXT("coverageAssetId"), IdOf(Variant.CoverageKey)); }
			Row->SetArrayField(TEXT("materials"), MaterialsJson(Variant.Materials));
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
		Manifest->SetArrayField(TEXT("variants"), Rows);
		Item->SetObjectField(TEXT("garmentVariants"), Manifest);
	}

	if (!Missing.IsEmpty())
	{
		OutError = FString::Printf(TEXT("\"%s\" has not been uploaded yet"), *Missing);
		return false;
	}

	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutJson);
	if (!FJsonSerializer::Serialize(Item, Writer))
	{
		OutError = TEXT("the item could not be written as JSON");
		return false;
	}
	return true;
}

bool FMantleCosmeticPackage::MakeItemPatchJson(const TMap<FString, FString>& IdsByKey, FString& OutJson, FString& OutError) const
{
	FString CreateJson;
	if (!MakeItemJson(IdsByKey, CreateJson, OutError))
	{
		OutJson.Reset();
		return false;
	}
	TSharedPtr<FJsonObject> Item;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(CreateJson);
	if (!FJsonSerializer::Deserialize(Reader, Item) || !Item.IsValid())
	{
		OutJson.Reset();
		OutError = TEXT("the item could not be read back as JSON");
		return false;
	}
	Item->RemoveField(TEXT("slot"));
	// A private version carries no declaration; this clears the one a public version made.
	if (Visibility == TEXT("private"))
	{
		Item->SetStringField(TEXT("rightsDeclarationVersion"), FString());
	}

	// Every optional field of this kind the package leaves out is cleared, so the new version
	// carries nothing of the old one (backend avatar.ItemPatch: "" clears an asset, [] and {} a list).
	auto ClearString = [&Item](const TCHAR* Field)
	{
		if (!Item->HasField(Field))
		{
			Item->SetStringField(Field, FString());
		}
	};
	auto ClearArray = [&Item](const TCHAR* Field)
	{
		if (!Item->HasField(Field))
		{
			Item->SetArrayField(Field, TArray<TSharedPtr<FJsonValue>>());
		}
	};
	auto ClearObject = [&Item](const TCHAR* Field)
	{
		if (!Item->HasField(Field))
		{
			Item->SetObjectField(Field, MakeShared<FJsonObject>());
		}
	};
	switch (Class)
	{
	case EMantleCosmeticClass::Avatar:
		ClearString(TEXT("faceMeshAssetId"));
		ClearArray(TEXT("faceMaterials"));
		ClearString(TEXT("dnaAssetId"));
		ClearString(TEXT("irisMaskAssetId"));
		ClearString(TEXT("jawBone"));
		ClearObject(TEXT("sockets"));
		ClearObject(TEXT("secondaryMotion"));
		break;
	case EMantleCosmeticClass::Hair:
	case EMantleCosmeticClass::Garment:
		ClearString(TEXT("coverageAssetId"));
		ClearString(TEXT("faceMeshAssetId"));
		// Hair authored with a face carries no source shape; a garment without size variants carries no manifest.
		ClearString(TEXT("sourceShapeAssetId"));
		if (Class == EMantleCosmeticClass::Garment)
		{
			ClearObject(TEXT("garmentVariants"));
		}
		break;
	case EMantleCosmeticClass::Accessory:
		// An empty transform is stored as no offset.
		ClearObject(TEXT("attach"));
		break;
	case EMantleCosmeticClass::Overlay:
		ClearString(TEXT("normalAssetId"));
		break;
	default:
		break;
	}

	OutJson.Reset();
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutJson);
	if (!FJsonSerializer::Serialize(Item.ToSharedRef(), Writer))
	{
		OutError = TEXT("the item could not be written as JSON");
		return false;
	}
	return true;
}
