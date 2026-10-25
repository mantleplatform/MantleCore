// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleCosmeticValidation.h"

/** #483: what one material section of an outfit mesh is, for splitting it into slot items. */
enum class EMantleOutfitPiece : uint8
{
	Top,
	Bottom,
	Footwear,
	/** The source character's own skin (a MetaHuman Creator outfit can carry the body under it). Never uploaded. */
	Skin,
	/** Anything else (gems, buttons, "Other1"), or a name that says two pieces: the creator decides. */
	Unknown,
};

/** One material section of an outfit mesh, by the names that describe it. */
struct MANTLEEDITOR_API FMantleOutfitSection
{
	/** The material slot name, "Shirt", "M_DG_bodyShape1_Shirt". */
	FString SlotName;
	/** The material (instance) asset name, "MI_Suit_Pants". */
	FString MaterialName;
	/** The base material it is an instance of, "M_Skin_Unified_Baked" (empty when unknown). */
	FString BaseMaterialName;
};

/**
 * #483: classifies the sections of a one-piece outfit (MetaHuman Creator exports one mesh with many materials,
 * sometimes including the character's skin) so each garment slot can take its own sections. Pure: names only, no
 * UObjects (tested).
 *
 * A name is split into lower-case words at non-letters, digits and camelCase ("M_DG_bodyShape1_Shirt" is m, dg, body,
 * shape, 1, shirt). The slot name decides first, then the material's name: a name with words of exactly one garment
 * piece is that piece, one with words of two pieces is Unknown. Garment words beat skin words in the same name, so the
 * MetaHuman default shirt ("bodyShape ... Shirt") is a top. The slot names the part, while a material is a texture sheet
 * often shared by several parts and named after them (#751), so after the slot's garment words: a prop word in the slot
 * (bow, arrow, gun, sword ...) makes it Unknown, and a skin word in the slot makes it Skin, whatever the material says
 * ("M00_Head" with "M_TwinBlast_Head_Shirt"). Then the material's garment words; then an accessory word (hat, glove,
 * belt, gem ...) in either name, or a prop word in the material's, says it is something worn or carried; otherwise skin
 * words in the material's name, or "skin" in the base material's, make it Skin.
 */
struct MANTLEEDITOR_API FMantleOutfitSections
{
	static EMantleOutfitPiece Classify(const FMantleOutfitSection& Section);

	/** "top", "bottom", "footwear", "skin", "unknown". */
	static const TCHAR* PieceName(EMantleOutfitPiece Piece);

	/** The garment slot a piece uploads into ("top", "bottom", "footwear"); empty for skin and unknown. */
	static FString PieceSlot(EMantleOutfitPiece Piece);

	/** Per section, whether an item for garment Slot keeps it by default: the sections whose piece is that slot. Never skin. */
	static TArray<bool> DefaultKeep(TConstArrayView<FMantleOutfitSection> Sections, const FString& Slot);

	/**
	 * The panel's first ticks for garment Slot: DefaultKeep when the sections name another garment piece than Slot's
	 * (a one-piece outfit), else every section but skin, so a plain garment ("Hoodie" and "Zipper", or names that say
	 * nothing, "Material0") uploads whole as before. Never skin.
	 */
	static TArray<bool> InitialKeep(TConstArrayView<FMantleOutfitSection> Sections, const FString& Slot);

	/** Whether the slot's or the material's name has a weapon or prop word (bow, arrow, gun, sword, shield ...). Pure (tested). */
	static bool IsProp(const FMantleOutfitSection& Section);

	/** #46: whether a name (a bone's, "sword_handle_l", "dagger_base_r") has a weapon or prop word. Pure (tested). */
	static bool IsPropName(const FString& Name);

	/** The words of a name (see the class comment). */
	static TArray<FString> Words(const FString& Name);

	/**
	 * The garment upload's section notes over the sections it uploads (the panel's ticks): a warning for each section
	 * that looks like the character's skin (every wearer would wear it), and a warning when those hold more than one
	 * garment piece but upload as one Slot item.
	 */
	static void CheckGarmentSections(TConstArrayView<FMantleOutfitSection> Sections, const FString& Slot, TArray<FMantleCosmeticIssue>& OutIssues);
};
