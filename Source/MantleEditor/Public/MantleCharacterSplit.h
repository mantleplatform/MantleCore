// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleFusedClothing.h"
#include "MantleOutfitSections.h"

class USkeletalMesh;

/** What a piece of a clothed character is, once split (#55). */
enum class EMantleSplitRole : uint8
{
	/** The bare body (and head, hands, feet): the avatar item. */
	Body,
	/** Clothing: a top, bottom or footwear item. */
	Garment,
	/** Hair, eyebrows, eyelashes, facial hair: its own item. */
	Hair,
	/** Something worn that is not one of the three garments: a hat, gloves, a belt, a bag. */
	Accessory,
	/** Not offered (props, anything the names and bones don't place): the creator decides. */
	LeftOut,
};

/** One skeletal mesh of the character, as its names and bones describe it (FMantleCharacterSplit::Gather reads it from a mesh). */
struct MANTLEEDITOR_API FMantleSplitSource
{
	/** The mesh's asset name, "SK_Vampire_Corset". */
	FString MeshName;

	/** Its material sections (slot, material and base material names) and the triangles each has. */
	TArray<FMantleOutfitSection> Sections;
	TArray<int32> Triangles;

	/** Per section, the mesh's material slot index it draws (what the mesh's triangles carry as their material ID); empty when not read. */
	TArray<int32> Slots;

	/** Per section, the bones that carry its skin (its render bone map), for the body region it sits on. */
	TArray<TArray<FString>> SectionBones;

	/** The fused-clothing verdict on its bare-looking skin (FMantleFusedClothing); FusedWhy says why when clothing is built in. */
	EMantleFusedVerdict Fused = EMantleFusedVerdict::Unknown;
	FString FusedWhy;

	/** Per section, how many of its vertices each bone carries most (the bone each is weighted to most); empty when not read. */
	TArray<TMap<FString, int32>> SectionWeights;

	/** Per section, its lowest and highest point (cm, as made); empty when not read. */
	TArray<FVector2f> SectionHeights;

	/** The mesh's height (cm, as made). */
	float HeightCm = 0.0f;

	/**
	 * Pack review: the share of its points lying on the mesh the creator picked (FMantleCharacterSplit::OnPickedCm), when that
	 * mesh wears clothing; -1 when not measured. A piece not on it belongs to another outfit in the pack.
	 */
	float OnPicked = -1.0f;
};

/** Where on the body a piece sits (#55 pack review), from the bones its vertices are weighted to. */
enum class EMantleBodyRegion : uint8
{
	Head,
	Neck,
	Chest,
	Arms,
	Hands,
	Waist,
	Legs,
	Feet,
	Num,
};

/** A piece's place on the body: the share of its vertices in each region (FMantleCharacterSplit::PlaceOf). */
struct MANTLEEDITOR_API FMantleBodyPlace
{
	float Share[static_cast<int32>(EMantleBodyRegion::Num)] = {};

	/** The region most of it is in; Num when nothing places it. */
	EMantleBodyRegion Main = EMantleBodyRegion::Num;

	/** It has no weights to read: its height on the body placed it. */
	bool bByHeight = false;

	bool IsPlaced() const { return Main != EMantleBodyRegion::Num; }

	/** "chest and waist": the regions holding at least FMantleCharacterSplit::RegionMinShare of it, most first; "nowhere" when unplaced. */
	FString Describe() const;
};

/** A row of the pack review: one mesh the scan found, where it sits and what Convert does with it (#55, "Found in this pack"). */
struct MANTLEEDITOR_API FMantlePackPiece
{
	/** The mesh (FMantlePackReview::MeshNames, Meshes). */
	int32 MeshIndex = INDEX_NONE;

	/** The row's label, from the mesh's name ("Lace Corset"): a label only, never what places it. */
	FString Label;

	FMantleBodyPlace Place;

	/** Its triangles (the largest piece of an item is the one the others go into). */
	int32 Triangles = 0;

	/** Where it goes: "top", "bottom", "footwear", "accessory", a hair slot ("hair", ...); empty: not used. */
	FString Slot;

	/** What the scan proposed (Slot and PartOf as found), so a changed row can say so. */
	FString ProposedSlot;
	int32 ProposedPartOf = INDEX_NONE;

	/** The piece (by index in FMantlePackReview::Pieces) it goes into as part of one item; INDEX_NONE: its own row. */
	int32 PartOf = INDEX_NONE;

	/** PartOf was found by the trim rule (FMantleGarmentOutfit::IsTrimOn): it is that piece's edging ("with lace edging"). */
	bool bTrim = false;

	/** Ticked: Convert takes it. A not-used row is never ticked. */
	bool bTicked = false;

	/** Why the scan put it here, in plain words. */
	FString Why;
};

/** A whole body in the pack that Convert could make the MetaHuman of. */
struct MANTLEEDITOR_API FMantlePackBody
{
	int32 MeshIndex = INDEX_NONE;
	FString Label;

	/** FMantleGarmentOutfit::BareBodyRank: higher is better; below 0 it wears clothing and isn't offered. */
	int32 Rank = -1;

	/** "every part skin", "2 pieces on it that Convert takes off (Bra, Briefs)", "wears clothing (Corset)". */
	FString Why;
};

/**
 * #55, the pack review ("Found in this pack"): what the scan of a character's pack proposes before Convert runs, as rows the
 * creator changes. The body it converts, each piece of clothing in its slot with what is folded into it, hair, and the rest
 * (not used, each with why). Convert takes the rows as they stand (FMantleGarmentOutfit::GarmentsFromReview).
 */
struct MANTLEEDITOR_API FMantlePackReview
{
	/** The mesh the creator picked (MeshIndex), and every mesh by MeshIndex: names always, objects when scanned (ReviewFor). */
	int32 Picked = INDEX_NONE;
	TArray<FString> MeshNames;
	TArray<TWeakObjectPtr<USkeletalMesh>> Meshes;

	/** The whole bodies: offered ones (Rank >= 0) first, best first; Body is the chosen one (index into Bodies). */
	TArray<FMantlePackBody> Bodies;
	int32 Body = INDEX_NONE;

	/** Every other mesh, in the order Rows lists them. */
	TArray<FMantlePackPiece> Pieces;

	/** The slots whose ticked pieces are one item each, not one item together (the default: we sell no arm pieces alone). */
	TSet<FString> Separate;

	/** One item Convert makes: its slot, the piece it is made from, and the pieces that go into it. */
	struct FItem
	{
		FString Slot;
		int32 Main = INDEX_NONE;
		TArray<int32> With;
	};

	/** The clothing items (top, bottom, footwear) the ticked rows make. Pure (tested). */
	TArray<FItem> Items() const;

	/** The chosen body's mesh index; INDEX_NONE when there is none to offer. */
	int32 BodyMesh() const;

	/** Whether Slot's ticked pieces are one item. */
	bool IsOneItem(const FString& Slot) const { return !Separate.Contains(Slot); }

	/** Whether any row has something to show beside the body (a piece of clothing, hair, anything not used). */
	bool HasRows() const { return Pieces.Num() > 0; }

	/** Puts Piece in Slot ("" for not used), or into another piece (PartOf): ticked unless not used. What goes into it follows. */
	void Move(int32 Piece, const FString& Slot, int32 PartOf = INDEX_NONE);

	/** The row heading a slot has ("Top", "Accessory (neck)", "Hair", "Not used"). */
	FString SlotHeading(const FMantlePackPiece& Piece) const;

	/** One piece's row text: "Corset, with lace edging (Lace Corset): chest and waist". */
	FString RowText(int32 Piece) const;

	/** The rows as log lines: "MANTLE_PACK <Label>: body ..." then one per row, every mesh of the pack in one. */
	TArray<FString> Describe(const FString& Label) const;
};

/** One item the split proposes. */
struct MANTLEEDITOR_API FMantleSplitPiece
{
	/** "Vampire Corset", or "Vampire Corset: Belt" for one section of a mesh. */
	FString Name;
	EMantleSplitRole Role = EMantleSplitRole::LeftOut;

	/** The upload slot: "avatar" for the body, "top", "bottom", "footwear", a hair slot ("hair", "eyebrows", "eyelashes", "facial_hair"), "accessory"; empty when left out. */
	FString Slot;

	/** Where it comes from: the mesh, and its sections (empty: every section of the mesh). */
	int32 MeshIndex = INDEX_NONE;
	TArray<int32> Sections;

	/** Draw order from the body outwards: 0 body, 1 underwear, 2 trousers/skirts/shoes, 3 tops, 5 outerwear, 6 accessories, 7 hair. */
	int32 Layer = 0;

	/** The body region its bones say: "head", "hands", "feet", "body". */
	FString Region;

	/** Why it was placed there, in plain words. */
	FString Why;
};

/** What FMantleCharacterSplit::Propose decided. */
struct MANTLEEDITOR_API FMantleSplitProposal
{
	/** The pieces, body first, then by layer. */
	TArray<FMantleSplitPiece> Pieces;

	/** True when there is a bare body to fit from and at least one garment: the character can become body + clothing items. */
	bool bSplittable = false;

	/** When it is not splittable: why, in plain words ("it stays a fixed avatar"). */
	FString WhyNot;

	/** The proposal as log lines: "MANTLE_SPLIT <Label>: splittable ..." then one per piece. */
	TArray<FString> Describe(const FString& Label) const;
};

/**
 * #55, step 1, "propose the split": from a character's meshes (separate skeletal meshes, else the material sections of one),
 * the items it could become: the bare body, each piece of clothing, the hair, the accessories, in layer order. Pure: names
 * and bone names only, no objects (tested). Nothing is made or written.
 *
 * Rules, in this order:
 *   - hair: a mesh or section whose name says hair, eyebrows, lashes or a beard is a hair item (FMantleAvatarAssemblyFixer::HairSlotForName);
 *   - skin: sections that read as the character's skin (FMantleOutfitSections) make the body; a mesh named nude, naked,
 *     body, head, face, hand or foot (all of it skin) is a body part;
 *   - clothing: a mesh (or section) the classifier reads as top, bottom or footwear is a garment; one with several pieces in
 *     it splits by section;
 *   - the rest: an accessory word (hat, glove, belt, bag ...) or the bones (hands, head) make an accessory; a prop word, or
 *     nothing that places it, leaves it out;
 *   - splittable only when there is a body to fit from (skin that is not fused with clothing: FMantleFusedClothing) and a garment.
 * A one-mesh character whose clothes are painted into its skin stays a fixed avatar.
 */
class MANTLEEDITOR_API FMantleCharacterSplit
{
public:
	/** The layer a piece of Slot is drawn at, nudged by the words of its name (underwear lower, jackets and armour higher). Pure (tested). */
	static int32 LayerOf(EMantleSplitRole Role, const FString& Slot, const FString& Name);

	/** The body region a set of bone names says: "head", "hands", "feet" when most of them are, else "body". Pure (tested). */
	static FString RegionOf(TConstArrayView<FString> BoneNames);

	/** Whether a name says an accessory: hat, cap, glove, belt, bag, scarf, mask, glasses, necklace, bracelet, ring, earring, cape, quiver. Pure (tested). */
	static bool NamesAccessory(const FString& Name);

	/** The proposal for Meshes. Pure (tested). */
	static FMantleSplitProposal Propose(TConstArrayView<FMantleSplitSource> Meshes);

	/**
	 * Mesh as a source: its LOD 0 sections' names, triangles and bone maps, and the fused-clothing verdict on its skin
	 * (FMantleAvatarStrip::Plan). False when the mesh has no sections.
	 */
	static bool Gather(USkeletalMesh& Mesh, FMantleSplitSource& Out);

	/**
	 * The character Main belongs to: Main and the other skeletal meshes beside it (its folder, not a Converted folder), as sources, then Propose.
	 * OutSources gets them in the order the proposal's MeshIndex counts.
	 */
	static FMantleSplitProposal ProposeFor(USkeletalMesh& Main, TArray<FMantleSplitSource>& OutSources);

	// ---- #55, the pack review: pieces placed by where they sit on the body, names only for labels ----

	/** The body region a bone is in, by its name ("upperarm_l" the arms, "spine_03" the chest, "Spine" the waist); Num for root and the like. Pure (tested). */
	static EMantleBodyRegion RegionOfBone(const FString& Bone);

	/**
	 * Pure (tested): where a piece sits, from how many of its vertices each bone carries most. With no weights to read (only a
	 * root, or none), by the middle of its height (HeightCm, min and max) on a body BodyHeightCm tall; unplaced when that is unknown too.
	 */
	static FMantleBodyPlace PlaceOf(const TMap<FString, int32>& BoneVertices, FVector2f HeightCm, float BodyHeightCm);

	/**
	 * Pure (tested): the slot a place says: mostly on the feet, footwear; mostly on the head, neck or hands, an accessory; else a
	 * top when at least TopMinShare of it is on the chest and arms (a corset, sleeves), a bottom otherwise (trousers); empty when unplaced.
	 */
	static FString SlotOfPlace(const FMantleBodyPlace& Place);

	/** A region holds a piece when at least this share of its vertices is there (FMantleBodyPlace::Describe, overlap). */
	static constexpr float RegionMinShare = 0.15f;
	static constexpr float TopMinShare = 0.3f;

	/** Two pieces of one slot are alternatives (two tops), not parts of one item, when both hold this share of one region. */
	static constexpr float OverlapShare = 0.25f;

	/**
	 * A piece is on the picked mesh when OnPickedShare of its points are within OnPickedCm of its surface: the dressed mesh is its
	 * pieces merged, so they lie on it exactly. Measured on Vampire and Girl: every piece of the outfit 100% within 0.5 cm, the
	 * other outfits' 50 to 91% (they hug the skin under it), so the rule is tight.
	 */
	static constexpr float OnPickedCm = 0.1f;
	static constexpr float OnPickedShare = 0.95f;

	/**
	 * Pure (tested): the pack review of Meshes, Picked the one the creator picked. Rules, in this order:
	 *   - a whole body (skin, about the picked mesh's height) is a body: offered when FMantleGarmentOutfit::BareBodyRank says it is
	 *     bare (or in underwear Convert takes off), the best one chosen;
	 *   - a piece all skin is part of a body, not used;
	 *   - every other piece is placed by its bones (PlaceOf), its slot from where it sits (SlotOfPlace); hair is a piece mostly on
	 *     the head (its weights, or its height band with none), mostly on the neck an accessory; one that nothing places is not
	 *     used. Names never place;
	 *   - when Picked wears clothing, a piece not on it (OnPicked) is another outfit in the pack: not used;
	 *   - a thin piece lying on another of its slot (TrimShare, the trim rule) goes into it as its edging;
	 *   - several pieces in one slot are one item, unless two of them sit on the same region (two tops): then one item each.
	 * TrimShare(Trim, On) is FMantleGarmentOutfit::TrimNear of the two meshes; nothing pairs without it.
	 */
	static FMantlePackReview Review(TConstArrayView<FMantleSplitSource> Meshes, int32 Picked,
		TFunctionRef<double(int32 TrimMesh, int32 OnMesh)> TrimShare = [](int32, int32) { return 0.0; });

	/**
	 * The pack review for Picked: it and the skeletal meshes beside it (its folder and below, never a Converted folder), read
	 * (Gather), measured against Picked when it wears clothing, and the trim rule; logs the rows (MANTLE_PACK).
	 */
	static FMantlePackReview ReviewFor(USkeletalMesh& Picked);
};
