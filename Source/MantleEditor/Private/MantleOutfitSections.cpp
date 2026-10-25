// Copyright Joshua Eger. All Rights Reserved.
#include "MantleOutfitSections.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleOutfitSectionsPrivate
{
	// Lower-case whole words, plurals spelled out.
	const TCHAR* const TopWords[] = {
		TEXT("shirt"), TEXT("shirts"), TEXT("tshirt"), TEXT("top"), TEXT("tops"), TEXT("vest"), TEXT("jacket"), TEXT("coat"), TEXT("hoodie"),
		TEXT("sweater"), TEXT("jumper"), TEXT("blouse"), TEXT("lapel"), TEXT("lapels"), TEXT("shawl"), TEXT("collar"), TEXT("cardigan"),
		TEXT("tunic"), TEXT("cape"), TEXT("cloak"), TEXT("sleeve"), TEXT("sleeves"), TEXT("blazer"), TEXT("corset"), TEXT("corsets"),
		TEXT("bustier"), TEXT("bodice"), TEXT("camisole"),
	};
	const TCHAR* const BottomWords[] = {
		TEXT("pants"), TEXT("pant"), TEXT("trousers"), TEXT("trouser"), TEXT("short"), TEXT("shorts"), TEXT("skirt"), TEXT("jeans"),
		TEXT("legging"), TEXT("leggings"), TEXT("bottom"), TEXT("bottoms"), TEXT("kilt"), TEXT("cargo"), TEXT("cargos"), TEXT("slacks"),
	};
	const TCHAR* const FootwearWords[] = {
		TEXT("shoe"), TEXT("shoes"), TEXT("boot"), TEXT("boots"), TEXT("sneaker"), TEXT("sneakers"), TEXT("sandal"), TEXT("sandals"),
		TEXT("heel"), TEXT("heels"), TEXT("footwear"), TEXT("sock"), TEXT("socks"), TEXT("slipper"), TEXT("slippers"), TEXT("loafer"), TEXT("loafers"),
	};
	// Worn, but not one garment slot: never mistaken for skin ("HeadWear", "BodySuit").
	const TCHAR* const WornWords[] = {
		TEXT("wear"), TEXT("hat"), TEXT("cap"), TEXT("helmet"), TEXT("hood"), TEXT("glove"), TEXT("gloves"), TEXT("belt"), TEXT("gem"),
		TEXT("button"), TEXT("buttons"), TEXT("buckle"), TEXT("jewel"), TEXT("suit"), TEXT("bodysuit"), TEXT("jumpsuit"), TEXT("dress"),
		TEXT("gown"), TEXT("robe"), TEXT("overall"), TEXT("overalls"), TEXT("uniform"), TEXT("armor"), TEXT("armour"),
	};
	// Weapons and props a character carries (a "BowAndArrow" slot, say): never skin, whatever its material says.
	const TCHAR* const PropWords[] = {
		TEXT("weapon"), TEXT("weapons"), TEXT("gun"), TEXT("guns"), TEXT("pistol"), TEXT("pistols"), TEXT("rifle"), TEXT("rifles"),
		TEXT("shotgun"), TEXT("bow"), TEXT("bows"), TEXT("arrow"), TEXT("arrows"), TEXT("quiver"), TEXT("sword"), TEXT("swords"),
		TEXT("blade"), TEXT("blades"), TEXT("knife"), TEXT("knives"), TEXT("dagger"), TEXT("daggers"), TEXT("axe"), TEXT("axes"),
		TEXT("spear"), TEXT("spears"), TEXT("shield"), TEXT("shields"), TEXT("hammer"), TEXT("mace"), TEXT("staff"), TEXT("wand"),
		TEXT("bullet"), TEXT("bullets"), TEXT("ammo"), TEXT("holster"), TEXT("grenade"), TEXT("grenades"), TEXT("scabbard"),
		TEXT("sheath"), TEXT("prop"), TEXT("props"),
	};
	const TCHAR* const SkinWords[] = { TEXT("skin"), TEXT("body"), TEXT("head"), TEXT("face"), TEXT("flesh") };

	bool HasAny(const TArray<FString>& Words, TArrayView<const TCHAR* const> List)
	{
		for (const FString& Word : Words)
		{
			for (const TCHAR* Entry : List)
			{
				if (Word.Equals(Entry, ESearchCase::CaseSensitive))
				{
					return true;
				}
			}
		}
		return false;
	}

	/** The one garment piece Words name (Unknown for several); bNamed says whether any garment word was there. */
	EMantleOutfitPiece GarmentPiece(const TArray<FString>& Words, bool& bNamed)
	{
		const bool bTop = HasAny(Words, MakeArrayView(TopWords));
		const bool bBottom = HasAny(Words, MakeArrayView(BottomWords));
		const bool bFootwear = HasAny(Words, MakeArrayView(FootwearWords));
		const int32 Count = (bTop ? 1 : 0) + (bBottom ? 1 : 0) + (bFootwear ? 1 : 0);
		bNamed = Count > 0;
		if (Count != 1)
		{
			return EMantleOutfitPiece::Unknown;
		}
		return bTop ? EMantleOutfitPiece::Top : bBottom ? EMantleOutfitPiece::Bottom : EMantleOutfitPiece::Footwear;
	}
}

TArray<FString> FMantleOutfitSections::Words(const FString& Name)
{
	TArray<FString> Out;
	FString Current;
	auto Flush = [&Out, &Current]()
	{
		if (!Current.IsEmpty())
		{
			Out.Add(Current.ToLower());
			Current.Reset();
		}
	};
	for (int32 Index = 0; Index < Name.Len(); ++Index)
	{
		const TCHAR Character = Name[Index];
		if (!FChar::IsAlnum(Character))
		{
			Flush();
			continue;
		}
		if (!Current.IsEmpty())
		{
			const TCHAR Previous = Name[Index - 1];
			const bool bDigitEdge = FChar::IsDigit(Character) != FChar::IsDigit(Previous);
			const bool bCamel = FChar::IsUpper(Character) && FChar::IsLower(Previous);
			// "TShirt": the capital before a lower-case letter starts a word after a run of capitals.
			const bool bAcronymEnd = FChar::IsUpper(Character) && FChar::IsUpper(Previous) && Index + 1 < Name.Len() && FChar::IsLower(Name[Index + 1]);
			if (bDigitEdge || bCamel || bAcronymEnd)
			{
				Flush();
			}
		}
		Current.AppendChar(Character);
	}
	Flush();
	return Out;
}

EMantleOutfitPiece FMantleOutfitSections::Classify(const FMantleOutfitSection& Section)
{
	using namespace MantleOutfitSectionsPrivate;
	const TArray<FString> SlotWords = Words(Section.SlotName);
	const TArray<FString> MaterialWords = Words(Section.MaterialName);
	bool bNamed = false;
	const EMantleOutfitPiece BySlot = GarmentPiece(SlotWords, bNamed);
	if (bNamed)
	{
		return BySlot;
	}
	// The slot names the part; a material is a texture sheet, often shared by several parts and named after them
	// (a shirt material on the head slot, an arrow material on the bow's). So a prop word in the slot is never
	// skin, and a skin word in the slot beats a garment word in its material.
	if (HasAny(SlotWords, MakeArrayView(PropWords)))
	{
		return EMantleOutfitPiece::Unknown;
	}
	const bool bSlotWorn = HasAny(SlotWords, MakeArrayView(WornWords));
	if (!bSlotWorn && HasAny(SlotWords, MakeArrayView(SkinWords)))
	{
		return EMantleOutfitPiece::Skin;
	}
	const EMantleOutfitPiece ByMaterial = GarmentPiece(MaterialWords, bNamed);
	if (bNamed)
	{
		return ByMaterial;
	}
	if (bSlotWorn || HasAny(MaterialWords, MakeArrayView(WornWords)) || HasAny(MaterialWords, MakeArrayView(PropWords)))
	{
		return EMantleOutfitPiece::Unknown;
	}
	const TCHAR* const SkinOnly[] = { TEXT("skin") };
	if (HasAny(MaterialWords, MakeArrayView(SkinWords)) || HasAny(Words(Section.BaseMaterialName), MakeArrayView(SkinOnly)))
	{
		return EMantleOutfitPiece::Skin;
	}
	return EMantleOutfitPiece::Unknown;
}

bool FMantleOutfitSections::IsProp(const FMantleOutfitSection& Section)
{
	using namespace MantleOutfitSectionsPrivate;
	return HasAny(Words(Section.SlotName), MakeArrayView(PropWords)) || HasAny(Words(Section.MaterialName), MakeArrayView(PropWords));
}

bool FMantleOutfitSections::IsPropName(const FString& Name)
{
	using namespace MantleOutfitSectionsPrivate;
	return HasAny(Words(Name), MakeArrayView(PropWords));
}

const TCHAR* FMantleOutfitSections::PieceName(EMantleOutfitPiece Piece)
{
	switch (Piece)
	{
	case EMantleOutfitPiece::Top: return TEXT("top");
	case EMantleOutfitPiece::Bottom: return TEXT("bottom");
	case EMantleOutfitPiece::Footwear: return TEXT("footwear");
	case EMantleOutfitPiece::Skin: return TEXT("skin");
	default: return TEXT("unknown");
	}
}

FString FMantleOutfitSections::PieceSlot(EMantleOutfitPiece Piece)
{
	return Piece == EMantleOutfitPiece::Top || Piece == EMantleOutfitPiece::Bottom || Piece == EMantleOutfitPiece::Footwear
		? FString(PieceName(Piece)) : FString();
}

TArray<bool> FMantleOutfitSections::DefaultKeep(TConstArrayView<FMantleOutfitSection> Sections, const FString& Slot)
{
	TArray<bool> Keep;
	for (const FMantleOutfitSection& Section : Sections)
	{
		const FString PieceSlotName = PieceSlot(Classify(Section));
		Keep.Add(!PieceSlotName.IsEmpty() && PieceSlotName.Equals(Slot, ESearchCase::IgnoreCase));
	}
	return Keep;
}

TArray<bool> FMantleOutfitSections::InitialKeep(TConstArrayView<FMantleOutfitSection> Sections, const FString& Slot)
{
	TArray<EMantleOutfitPiece> Pieces;
	bool bOtherPiece = false;
	for (const FMantleOutfitSection& Section : Sections)
	{
		const EMantleOutfitPiece Piece = Classify(Section);
		Pieces.Add(Piece);
		const FString PieceSlotName = PieceSlot(Piece);
		bOtherPiece = bOtherPiece || (!PieceSlotName.IsEmpty() && !PieceSlotName.Equals(Slot, ESearchCase::IgnoreCase));
	}
	if (bOtherPiece)
	{
		return DefaultKeep(Sections, Slot);
	}
	TArray<bool> Keep;
	for (const EMantleOutfitPiece Piece : Pieces)
	{
		Keep.Add(Piece != EMantleOutfitPiece::Skin);
	}
	return Keep;
}

void FMantleOutfitSections::CheckGarmentSections(TConstArrayView<FMantleOutfitSection> Sections, const FString& Slot, TArray<FMantleCosmeticIssue>& OutIssues)
{
	// Section names per garment piece, in Top, Bottom, Footwear order.
	constexpr int32 NumGarments = 3;
	const EMantleOutfitPiece Garments[NumGarments] = { EMantleOutfitPiece::Top, EMantleOutfitPiece::Bottom, EMantleOutfitPiece::Footwear };
	TArray<FString> NamesOf[NumGarments];
	for (const FMantleOutfitSection& Section : Sections)
	{
		const EMantleOutfitPiece Piece = Classify(Section);
		if (Piece == EMantleOutfitPiece::Skin)
		{
			const FString Material = !Section.MaterialName.IsEmpty() ? Section.MaterialName : Section.BaseMaterialName;
			const FString MaterialNote = Material.IsEmpty() ? FString() : FString::Printf(TEXT(" (%s)"), *Material);
			OutIssues.Add(FMantleCosmeticChecks::Warning(
				FString::Printf(TEXT("section \"%s\"%s looks like the character's own skin, so every wearer would wear it."), *Section.SlotName, *MaterialNote),
				TEXT("Garments carry clothing only: the wearer's own body shows through. Untick that section under Sections.")));
			continue;
		}
		for (int32 Index = 0; Index < NumGarments; ++Index)
		{
			if (Piece == Garments[Index])
			{
				NamesOf[Index].Add(Section.SlotName);
			}
		}
	}
	TArray<FString> Parts;
	for (int32 Index = 0; Index < NumGarments; ++Index)
	{
		if (NamesOf[Index].Num() > 0)
		{
			Parts.Add(FString::Printf(TEXT("%s: %s"), PieceName(Garments[Index]), *FString::Join(NamesOf[Index], TEXT(", "))));
		}
	}
	if (Parts.Num() > 1)
	{
		OutIssues.Add(FMantleCosmeticChecks::Warning(
			FString::Printf(TEXT("this mesh holds more than one garment (%s) and uploads as one \"%s\" item."), *FString::Join(Parts, TEXT("; ")), *Slot),
			TEXT("It can still be published as one item. To give each piece its own slot, tick only that piece's sections under Sections and upload the mesh once per slot.")));
	}
}
