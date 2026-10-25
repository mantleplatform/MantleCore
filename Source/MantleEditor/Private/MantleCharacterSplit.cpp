// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCharacterSplit.h"

#include "MantleAvatarConverter.h"
#include "MantleAvatarStripFixer.h"
#include "MantleCosmeticExport.h"
#include "MantleGarmentOutfit.h"
#include "MantleSkinnedMesh.h"
#include "MantleRuntime.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "Rendering/SkeletalMeshModel.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleCharacterSplitPrivate
{
	bool SplitHasWord(const FString& Name, std::initializer_list<const TCHAR*> Words)
	{
		const TArray<FString> Found = FMantleOutfitSections::Words(Name);
		for (const TCHAR* Word : Words)
		{
			if (Found.Contains(Word))
			{
				return true;
			}
		}
		return false;
	}

	/** The garment piece a name's own words say ("SK_Clothes_Shirt" is a top); Unknown when none or several. */
	EMantleOutfitPiece SplitPieceOfName(const FString& Name)
	{
		FMantleOutfitSection ByName;
		ByName.SlotName = Name;
		return FMantleOutfitSections::Classify(ByName);
	}

	const TCHAR* SplitRoleName(const EMantleSplitRole Role)
	{
		switch (Role)
		{
		case EMantleSplitRole::Body: return TEXT("body");
		case EMantleSplitRole::Garment: return TEXT("garment");
		case EMantleSplitRole::Hair: return TEXT("hair");
		case EMantleSplitRole::Accessory: return TEXT("accessory");
		default: return TEXT("left out");
		}
	}

	/** One section placed: its role, slot and why. */
	struct FSectionPlace
	{
		EMantleSplitRole Role = EMantleSplitRole::LeftOut;
		FString Slot;
		FString Why;
	};

	FSectionPlace SplitPlaceSection(const FMantleSplitSource& Mesh, const int32 Section, const EMantleOutfitPiece MeshPiece)
	{
		FSectionPlace Out;
		const FMantleOutfitSection& Named = Mesh.Sections[Section];
		const FString Region = FMantleCharacterSplit::RegionOf(Mesh.SectionBones.IsValidIndex(Section) ? TConstArrayView<FString>(Mesh.SectionBones[Section]) : TConstArrayView<FString>());
		const FString HairSlot = FMantleAvatarAssemblyFixer::HairSlotForName(Named.SlotName + TEXT(" ") + Named.MaterialName);
		if (!HairSlot.IsEmpty())
		{
			Out.Role = EMantleSplitRole::Hair;
			Out.Slot = HairSlot;
			Out.Why = TEXT("its name says hair");
			return Out;
		}
		const EMantleOutfitPiece Piece = FMantleOutfitSections::Classify(Named);
		if (Piece == EMantleOutfitPiece::Skin)
		{
			Out.Role = EMantleSplitRole::Body;
			Out.Slot = TEXT("avatar");
			Out.Why = TEXT("it reads as the character's skin");
			return Out;
		}
		if (!FMantleOutfitSections::PieceSlot(Piece).IsEmpty())
		{
			Out.Role = EMantleSplitRole::Garment;
			Out.Slot = FMantleOutfitSections::PieceSlot(Piece);
			Out.Why = FString::Printf(TEXT("its materials read as a %s"), FMantleOutfitSections::PieceName(Piece));
			return Out;
		}
		// The face's own parts (eyes, teeth, a tongue, the mouth) go with the body.
		if (FMantleAvatarStrip::NamesFaceDetail(Named.SlotName, Named.MaterialName))
		{
			Out.Role = EMantleSplitRole::Body;
			Out.Slot = TEXT("avatar");
			Out.Why = TEXT("it is the character's eyes, teeth or mouth");
			return Out;
		}
		// Words the classifier doesn't know: underwear and the like.
		const FString Words = Named.SlotName + TEXT(" ") + Named.MaterialName + TEXT(" ") + Mesh.MeshName;
		if (SplitHasWord(Words, { TEXT("corset"), TEXT("bra"), TEXT("bustier"), TEXT("bodice"), TEXT("tank"), TEXT("blouse"), TEXT("sweater") }))
		{
			Out.Role = EMantleSplitRole::Garment;
			Out.Slot = TEXT("top");
			Out.Why = TEXT("its name says a top");
			return Out;
		}
		if (SplitHasWord(Words, { TEXT("underwear"), TEXT("panties"), TEXT("panty"), TEXT("briefs"), TEXT("boxers"), TEXT("leggings"), TEXT("tights") }))
		{
			Out.Role = EMantleSplitRole::Garment;
			Out.Slot = TEXT("bottom");
			Out.Why = TEXT("its name says a bottom");
			return Out;
		}
		// Unknown by its materials: the mesh's name, the words of an accessory, a prop, then the bones.
		if (FMantleOutfitSections::IsProp(Named))
		{
			Out.Why = TEXT("it looks like a prop or a weapon");
			return Out;
		}
		if (FMantleCharacterSplit::NamesAccessory(Named.SlotName + TEXT(" ") + Named.MaterialName) || FMantleCharacterSplit::NamesAccessory(Mesh.MeshName))
		{
			Out.Role = EMantleSplitRole::Accessory;
			Out.Slot = TEXT("accessory");
			Out.Why = TEXT("its name says an accessory");
			return Out;
		}
		if (!FMantleOutfitSections::PieceSlot(MeshPiece).IsEmpty())
		{
			Out.Role = EMantleSplitRole::Garment;
			Out.Slot = FMantleOutfitSections::PieceSlot(MeshPiece);
			Out.Why = FString::Printf(TEXT("its mesh is named for a %s"), FMantleOutfitSections::PieceName(MeshPiece));
			return Out;
		}
		if (Region == TEXT("feet"))
		{
			Out.Role = EMantleSplitRole::Garment;
			Out.Slot = TEXT("footwear");
			Out.Why = TEXT("it is skinned to the feet");
			return Out;
		}
		if (Region == TEXT("head") || Region == TEXT("hands"))
		{
			Out.Role = EMantleSplitRole::Accessory;
			Out.Slot = TEXT("accessory");
			Out.Why = FString::Printf(TEXT("it is skinned to the %s"), *Region);
			return Out;
		}
		Out.Why = TEXT("nothing in its names or bones says what it is");
		return Out;
	}
}

FString FMantleCharacterSplit::RegionOf(const TConstArrayView<FString> BoneNames)
{
	int32 Head = 0;
	int32 Hands = 0;
	int32 Feet = 0;
	for (const FString& Bone : BoneNames)
	{
		const FString Lower = Bone.ToLower();
		auto Has = [&Lower](std::initializer_list<const TCHAR*> Words)
		{
			for (const TCHAR* Word : Words)
			{
				if (Lower.Contains(Word))
				{
					return true;
				}
			}
			return false;
		};
		if (Has({ TEXT("head"), TEXT("neck"), TEXT("face"), TEXT("jaw"), TEXT("eye") }))
		{
			++Head;
		}
		else if (Has({ TEXT("hand"), TEXT("finger"), TEXT("thumb"), TEXT("pinky"), TEXT("index"), TEXT("middle"), TEXT("ring_") }))
		{
			++Hands;
		}
		else if (Has({ TEXT("foot"), TEXT("toe"), TEXT("ball_") }))
		{
			++Feet;
		}
	}
	const int32 Total = BoneNames.Num();
	if (Total == 0)
	{
		return TEXT("body");
	}
	auto Most = [Total](const int32 Count) { return Count * 10 >= Total * 6; };
	return Most(Head) ? TEXT("head") : Most(Hands) ? TEXT("hands") : Most(Feet) ? TEXT("feet") : TEXT("body");
}

bool FMantleCharacterSplit::NamesAccessory(const FString& Name)
{
	return MantleCharacterSplitPrivate::SplitHasWord(Name, { TEXT("hat"), TEXT("cap"), TEXT("helmet"), TEXT("glove"), TEXT("gloves"), TEXT("gauntlet"), TEXT("bracer"),
		TEXT("belt"), TEXT("sash"), TEXT("bag"), TEXT("backpack"), TEXT("purse"), TEXT("scarf"), TEXT("mask"), TEXT("glasses"), TEXT("goggles"), TEXT("necklace"),
		TEXT("bracelet"), TEXT("ring"), TEXT("earring"), TEXT("earrings"), TEXT("cape"), TEXT("cloak"), TEXT("quiver"), TEXT("pauldron"), TEXT("pauldrons") });
}

int32 FMantleCharacterSplit::LayerOf(const EMantleSplitRole Role, const FString& Slot, const FString& Name)
{
	using namespace MantleCharacterSplitPrivate;
	switch (Role)
	{
	case EMantleSplitRole::Body:
		return 0;
	case EMantleSplitRole::Hair:
		return 7;
	case EMantleSplitRole::Accessory:
		return 6;
	case EMantleSplitRole::LeftOut:
		return 8;
	default:
		break;
	}
	if (SplitHasWord(Name, { TEXT("underwear"), TEXT("bra"), TEXT("panty"), TEXT("panties"), TEXT("lingerie"), TEXT("bikini"), TEXT("boxer"), TEXT("briefs") }))
	{
		return 1;
	}
	if (SplitHasWord(Name, { TEXT("jacket"), TEXT("coat"), TEXT("cape"), TEXT("cloak"), TEXT("armor"), TEXT("armour"), TEXT("breastplate"), TEXT("robe"), TEXT("vest"), TEXT("poncho") }))
	{
		return 5;
	}
	return Slot == TEXT("top") ? 3 : 2;
}

FMantleSplitProposal FMantleCharacterSplit::Propose(const TConstArrayView<FMantleSplitSource> Meshes)
{
	using namespace MantleCharacterSplitPrivate;
	FMantleSplitProposal Out;
	bool bFused = false;
	FString FusedWhy;
	for (int32 MeshIndex = 0; MeshIndex < Meshes.Num(); ++MeshIndex)
	{
		const FMantleSplitSource& Mesh = Meshes[MeshIndex];
		const FString Display = FMantleAvatarAssemblyFixer::DisplayName(Mesh.MeshName);
		// A mesh named for hair is a hair item, whatever its sections are called.
		const FString MeshHair = FMantleAvatarAssemblyFixer::HairSlotForName(Mesh.MeshName);
		if (!MeshHair.IsEmpty())
		{
			FMantleSplitPiece& Piece = Out.Pieces.AddDefaulted_GetRef();
			Piece.Name = Display;
			Piece.Role = EMantleSplitRole::Hair;
			Piece.Slot = MeshHair;
			Piece.MeshIndex = MeshIndex;
			Piece.Why = TEXT("its name says hair");
			continue;
		}
		// Name words that say a body part (the whole mesh is skin) or a garment.
		const bool bBodyNamed = SplitHasWord(Mesh.MeshName, { TEXT("nude"), TEXT("naked"), TEXT("body"), TEXT("torso"), TEXT("head"), TEXT("face"), TEXT("hand"), TEXT("hands"),
			TEXT("foot"), TEXT("feet"), TEXT("legs"), TEXT("arms") });
		EMantleOutfitPiece MeshPiece = SplitPieceOfName(Mesh.MeshName);
		if (MeshPiece == EMantleOutfitPiece::Skin)
		{
			MeshPiece = EMantleOutfitPiece::Unknown;
		}
		TArray<FSectionPlace> Places;
		for (int32 Section = 0; Section < Mesh.Sections.Num(); ++Section)
		{
			Places.Add(SplitPlaceSection(Mesh, Section, MeshPiece));
		}
		// A body-named mesh whose sections the classifier could not place is body (a "SK_Girl_Body" with "Material0").
		bool bAnySkin = false;
		for (const FSectionPlace& Place : Places)
		{
			bAnySkin |= Place.Role == EMantleSplitRole::Body;
		}
		if (bBodyNamed && MeshPiece == EMantleOutfitPiece::Unknown)
		{
			for (FSectionPlace& Place : Places)
			{
				if (Place.Role == EMantleSplitRole::LeftOut || Place.Role == EMantleSplitRole::Accessory)
				{
					Place.Role = EMantleSplitRole::Body;
					Place.Slot = TEXT("avatar");
					Place.Why = TEXT("its mesh is named for the body");
					bAnySkin = true;
				}
			}
		}
		if (bAnySkin && Mesh.Fused == EMantleFusedVerdict::FusedClothing)
		{
			bFused = true;
			FusedWhy = Mesh.FusedWhy;
		}
		// One piece per role and slot in this mesh; a mesh that is one role is named for itself.
		TArray<TPair<EMantleSplitRole, FString>> Seen;
		for (const FSectionPlace& Place : Places)
		{
			const TPair<EMantleSplitRole, FString> Key(Place.Role, Place.Slot);
			if (Seen.Contains(Key))
			{
				continue;
			}
			Seen.Add(Key);
			FMantleSplitPiece& Piece = Out.Pieces.AddDefaulted_GetRef();
			Piece.Role = Place.Role;
			Piece.Slot = Place.Slot;
			Piece.MeshIndex = MeshIndex;
			Piece.Why = Place.Why;
			TArray<FString> Bones;
			for (int32 Section = 0; Section < Places.Num(); ++Section)
			{
				if (Places[Section].Role == Place.Role && Places[Section].Slot == Place.Slot)
				{
					Piece.Sections.Add(Section);
					if (Mesh.SectionBones.IsValidIndex(Section))
					{
						Bones.Append(Mesh.SectionBones[Section]);
					}
				}
			}
			Piece.Region = RegionOf(Bones);
			const bool bWholeMesh = Piece.Sections.Num() == Places.Num();
			Piece.Name = bWholeMesh ? Display : FString::Printf(TEXT("%s: %s"), *Display, Place.Role == EMantleSplitRole::Body ? TEXT("body") : Place.Slot.IsEmpty() ? TEXT("other") : *Place.Slot);
			if (bWholeMesh)
			{
				Piece.Sections.Reset();
			}
		}
	}
	for (FMantleSplitPiece& Piece : Out.Pieces)
	{
		Piece.Layer = LayerOf(Piece.Role, Piece.Slot, Piece.Name);
	}
	Out.Pieces.StableSort([](const FMantleSplitPiece& A, const FMantleSplitPiece& B) { return A.Layer < B.Layer; });

	bool bBody = false;
	bool bGarment = false;
	for (const FMantleSplitPiece& Piece : Out.Pieces)
	{
		bBody |= Piece.Role == EMantleSplitRole::Body;
		bGarment |= Piece.Role == EMantleSplitRole::Garment;
	}
	if (bFused)
	{
		Out.WhyNot = FString::Printf(TEXT("%s, so there is no bare body to fit from: it stays a fixed avatar"), *FusedWhy);
	}
	else if (!bBody)
	{
		Out.WhyNot = TEXT("no part of it reads as a bare body to fit from: it stays a fixed avatar");
	}
	else if (!bGarment)
	{
		Out.WhyNot = TEXT("no separate clothing was found: it stays a fixed avatar");
	}
	Out.bSplittable = Out.WhyNot.IsEmpty();
	return Out;
}

TArray<FString> FMantleSplitProposal::Describe(const FString& Label) const
{
	TArray<FString> Lines;
	Lines.Add(bSplittable ? FString::Printf(TEXT("MANTLE_SPLIT %s: splittable, %d pieces"), *Label, Pieces.Num())
		: FString::Printf(TEXT("MANTLE_SPLIT %s: not splittable (%s), %d pieces seen"), *Label, *WhyNot, Pieces.Num()));
	for (const FMantleSplitPiece& Piece : Pieces)
	{
		Lines.Add(FString::Printf(TEXT("MANTLE_SPLIT %s:   layer %d %s '%s' slot=%s region=%s sections=%s (%s)"), *Label, Piece.Layer, MantleCharacterSplitPrivate::SplitRoleName(Piece.Role),
			*Piece.Name, Piece.Slot.IsEmpty() ? TEXT("-") : *Piece.Slot, *Piece.Region, Piece.Sections.Num() == 0 ? TEXT("all") : *FString::JoinBy(Piece.Sections, TEXT(","), [](const int32 Section) { return FString::FromInt(Section); }), *Piece.Why));
	}
	return Lines;
}

bool FMantleCharacterSplit::Gather(USkeletalMesh& Mesh, FMantleSplitSource& Out)
{
	Out = FMantleSplitSource();
	Out.MeshName = Mesh.GetName();
	TArray<int32> Slots;
	Out.Sections = FMantleCosmeticExport::ReadOutfitSections(&Mesh, 0, Slots);
	if (Out.Sections.Num() == 0)
	{
		return false;
	}
	Out.Slots = Slots;
	// Triangles, the bones of each slot's sections, and the bone each vertex is weighted to most, from LOD 0's imported model
	// (no renderer needed).
	Out.Triangles.SetNumZeroed(Slots.Num());
	Out.SectionBones.SetNum(Slots.Num());
	Out.SectionWeights.SetNum(Slots.Num());
	Out.SectionHeights.Init(FVector2f(TNumericLimits<float>::Max(), TNumericLimits<float>::Lowest()), Slots.Num());
	Out.HeightCm = 2.0f * static_cast<float>(Mesh.GetBounds().BoxExtent.Z);
	const FReferenceSkeleton& Reference = Mesh.GetRefSkeleton();
	if (const FSkeletalMeshModel* Model = Mesh.GetImportedModel(); Model && Model->LODModels.IsValidIndex(0))
	{
		for (const FSkelMeshSection& Section : Model->LODModels[0].Sections)
		{
			const int32 Index = Slots.IndexOfByKey(static_cast<int32>(Section.MaterialIndex));
			if (Index == INDEX_NONE)
			{
				continue;
			}
			Out.Triangles[Index] += Section.NumTriangles;
			for (const uint16 Bone : Section.BoneMap)
			{
				if (Reference.IsValidIndex(Bone))
				{
					Out.SectionBones[Index].AddUnique(Reference.GetBoneName(Bone).ToString());
				}
			}
			for (const FSoftSkinVertex& Vertex : Section.SoftVertices)
			{
				FVector2f& Height = Out.SectionHeights[Index];
				Height.X = FMath::Min(Height.X, Vertex.Position.Z);
				Height.Y = FMath::Max(Height.Y, Vertex.Position.Z);
				int32 Strongest = INDEX_NONE;
				int32 StrongestWeight = 0;
				for (int32 Influence = 0; Influence < MAX_TOTAL_INFLUENCES; ++Influence)
				{
					if (Vertex.InfluenceWeights[Influence] > StrongestWeight)
					{
						StrongestWeight = Vertex.InfluenceWeights[Influence];
						Strongest = Vertex.InfluenceBones[Influence];
					}
				}
				const int32 Bone = Section.BoneMap.IsValidIndex(Strongest) ? static_cast<int32>(Section.BoneMap[Strongest]) : INDEX_NONE;
				if (Reference.IsValidIndex(Bone))
				{
					++Out.SectionWeights[Index].FindOrAdd(Reference.GetBoneName(Bone).ToString());
				}
			}
		}
	}
	for (FVector2f& Height : Out.SectionHeights)
	{
		if (Height.X > Height.Y)
		{
			Height = FVector2f::ZeroVector;
		}
	}
	// Fused clothing in the skin it holds.
	FMantleStripPlan Plan;
	FString Error;
	if (FMantleAvatarStrip::Plan(Mesh, nullptr, Plan, Error))
	{
		for (const FMantleStripSlot& Slot : Plan.Slots)
		{
			if (Slot.bSkin && Slot.Fused == EMantleFusedVerdict::FusedClothing)
			{
				Out.Fused = EMantleFusedVerdict::FusedClothing;
				Out.FusedWhy = Slot.FusedWhy;
			}
			else if (Slot.bSkin && Out.Fused != EMantleFusedVerdict::FusedClothing)
			{
				Out.Fused = Slot.Fused;
			}
		}
	}
	return true;
}

FMantleSplitProposal FMantleCharacterSplit::ProposeFor(USkeletalMesh& Main, TArray<FMantleSplitSource>& OutSources)
{
	OutSources.Reset();
	FMantleSplitSource MainSource;
	if (Gather(Main, MainSource))
	{
		OutSources.Add(MoveTemp(MainSource));
	}
	// The meshes beside it: its folder, never Convert's output.
	const FString Folder = FPackageName::GetLongPackagePath(Main.GetPackage()->GetName());
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FARFilter Filter;
	Filter.PackagePaths.Add(*Folder);
	Filter.bRecursivePaths = true;
	Filter.ClassPaths.Add(USkeletalMesh::StaticClass()->GetClassPathName());
	TArray<FAssetData> Assets;
	Registry.GetAssets(Filter, Assets);
	Assets.Sort([](const FAssetData& A, const FAssetData& B) { return A.PackageName.LexicalLess(B.PackageName); });
	for (const FAssetData& Asset : Assets)
	{
		if (Asset.PackageName == Main.GetPackage()->GetFName() || (Asset.PackagePath.ToString() + TEXT("/")).Contains(TEXT("/Converted/")))
		{
			continue;
		}
		USkeletalMesh* Other = Cast<USkeletalMesh>(Asset.GetAsset());
		FMantleSplitSource Source;
		if (Other && Gather(*Other, Source))
		{
			OutSources.Add(MoveTemp(Source));
		}
	}
	const FMantleSplitProposal Proposal = Propose(OutSources);
	for (const FString& Line : Proposal.Describe(Main.GetName()))
	{
		UE_LOG(LogMantle, Log, TEXT("%s"), *Line);
	}
	return Proposal;
}

// ---- #55, the pack review ("Found in this pack") --------------------------------------------------------------

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleCharacterSplitPrivate
{
	constexpr int32 RegionCount = static_cast<int32>(EMantleBodyRegion::Num);

	const TCHAR* RegionName(const EMantleBodyRegion Region)
	{
		switch (Region)
		{
		case EMantleBodyRegion::Head: return TEXT("head");
		case EMantleBodyRegion::Neck: return TEXT("neck");
		case EMantleBodyRegion::Chest: return TEXT("chest");
		case EMantleBodyRegion::Arms: return TEXT("arms");
		case EMantleBodyRegion::Hands: return TEXT("hands");
		case EMantleBodyRegion::Waist: return TEXT("waist");
		case EMantleBodyRegion::Legs: return TEXT("legs");
		case EMantleBodyRegion::Feet: return TEXT("feet");
		default: return TEXT("nowhere");
		}
	}

	float RegionShare(const FMantleBodyPlace& Place, const EMantleBodyRegion Region)
	{
		return Place.Share[static_cast<int32>(Region)];
	}

	/** Both hold one region (two tops on the chest): alternatives, not parts of one item. */
	bool PlacesOverlap(const FMantleBodyPlace& A, const FMantleBodyPlace& B)
	{
		for (int32 Region = 0; Region < RegionCount; ++Region)
		{
			if (A.Share[Region] >= FMantleCharacterSplit::OverlapShare && B.Share[Region] >= FMantleCharacterSplit::OverlapShare)
			{
				return true;
			}
		}
		return false;
	}

	/** "chest 55% waist 45%", for the log. */
	FString PlaceShares(const FMantleBodyPlace& Place)
	{
		TArray<FString> Parts;
		for (int32 Region = 0; Region < RegionCount; ++Region)
		{
			if (Place.Share[Region] >= 0.005f)
			{
				Parts.Add(FString::Printf(TEXT("%s %.0f%%"), RegionName(static_cast<EMantleBodyRegion>(Region)), 100.0f * Place.Share[Region]));
			}
		}
		return Parts.IsEmpty() ? FString(TEXT("none")) : FString::Join(Parts, TEXT(" ")) + (Place.bByHeight ? TEXT(" (by height)") : TEXT(""));
	}

	bool IsGarmentSlot(const FString& Slot)
	{
		return Slot == TEXT("top") || Slot == TEXT("bottom") || Slot == TEXT("footwear");
	}

	/** Row order: top, bottom, footwear, accessory, hair, not used. */
	int32 SlotOrder(const FString& Slot)
	{
		return Slot == TEXT("top") ? 0 : Slot == TEXT("bottom") ? 1 : Slot == TEXT("footwear") ? 2 : Slot == TEXT("accessory") ? 3 : Slot.IsEmpty() ? 5 : 4;
	}

	/** "a, b and c". */
	FString JoinAnd(const TArray<FString>& Words)
	{
		if (Words.Num() <= 1)
		{
			return Words.IsEmpty() ? FString() : Words[0];
		}
		FString Text = Words[0];
		for (int32 Word = 1; Word + 1 < Words.Num(); ++Word)
		{
			Text += TEXT(", ") + Words[Word];
		}
		return Text + TEXT(" and ") + Words.Last();
	}
}

FString FMantleBodyPlace::Describe() const
{
	using namespace MantleCharacterSplitPrivate;
	if (!IsPlaced())
	{
		return TEXT("nowhere");
	}
	TArray<int32> Held;
	for (int32 Region = 0; Region < RegionCount; ++Region)
	{
		if (Share[Region] >= FMantleCharacterSplit::RegionMinShare || Region == static_cast<int32>(Main))
		{
			Held.Add(Region);
		}
	}
	Held.StableSort([this](const int32 A, const int32 B) { return Share[A] > Share[B]; });
	TArray<FString> Names;
	for (const int32 Region : Held)
	{
		Names.Add(RegionName(static_cast<EMantleBodyRegion>(Region)));
	}
	return JoinAnd(Names) + (bByHeight ? TEXT(" (by its height: it has no skin weights)") : TEXT(""));
}

EMantleBodyRegion FMantleCharacterSplit::RegionOfBone(const FString& Bone)
{
	const FString Lower = Bone.ToLower();
	auto Has = [&Lower](std::initializer_list<const TCHAR*> Words)
	{
		for (const TCHAR* Word : Words)
		{
			if (Lower.Contains(Word))
			{
				return true;
			}
		}
		return false;
	};
	// The face's bones first ("FACIAL_C_LipUpperMiddle" is the head, not a middle finger).
	if (Has({ TEXT("facial"), TEXT("head"), TEXT("jaw"), TEXT("eye"), TEXT("brow"), TEXT("lip"), TEXT("tongue"), TEXT("teeth"), TEXT("nose"), TEXT("cheek"), TEXT("chin") }))
	{
		return EMantleBodyRegion::Head;
	}
	if (Has({ TEXT("neck") }))
	{
		return EMantleBodyRegion::Neck;
	}
	if (Has({ TEXT("thumb"), TEXT("index"), TEXT("middle"), TEXT("ring"), TEXT("pinky"), TEXT("finger"), TEXT("hand"), TEXT("wrist"), TEXT("palm"), TEXT("metacarpal") }))
	{
		return EMantleBodyRegion::Hands;
	}
	if (Has({ TEXT("foot"), TEXT("toe"), TEXT("ball"), TEXT("ankle"), TEXT("heel") }))
	{
		return EMantleBodyRegion::Feet;
	}
	if (Has({ TEXT("thigh"), TEXT("calf"), TEXT("knee"), TEXT("shin"), TEXT("leg") }))
	{
		return EMantleBodyRegion::Legs;
	}
	if (Has({ TEXT("clavicle"), TEXT("shoulder"), TEXT("chest"), TEXT("breast"), TEXT("bust"), TEXT("boob"), TEXT("pec") }))
	{
		return EMantleBodyRegion::Chest;
	}
	if (Has({ TEXT("upperarm"), TEXT("lowerarm"), TEXT("forearm"), TEXT("elbow"), TEXT("arm") }))
	{
		return EMantleBodyRegion::Arms;
	}
	if (Has({ TEXT("pelvis"), TEXT("hip"), TEXT("butt"), TEXT("glute") }))
	{
		return EMantleBodyRegion::Waist;
	}
	const int32 Spine = Lower.Find(TEXT("spine"));
	if (Spine != INDEX_NONE)
	{
		// UE and MetaHuman count spine_01 to spine_05 up from the pelvis (01 and 02 the lower back); Mixamo has Spine, Spine1, Spine2.
		const bool bUnderscore = Lower.Mid(Spine + 5, 1) == TEXT("_");
		int32 Number = 0;
		for (int32 Index = Spine + 5; Index < Lower.Len(); ++Index)
		{
			if (FChar::IsDigit(Lower[Index]))
			{
				Number = Number * 10 + (Lower[Index] - TEXT('0'));
			}
			else if (Lower[Index] != TEXT('_'))
			{
				break;
			}
		}
		return Number <= (bUnderscore ? 2 : 1) ? EMantleBodyRegion::Waist : EMantleBodyRegion::Chest;
	}
	return EMantleBodyRegion::Num;
}

FMantleBodyPlace FMantleCharacterSplit::PlaceOf(const TMap<FString, int32>& BoneVertices, const FVector2f HeightCm, const float BodyHeightCm)
{
	using namespace MantleCharacterSplitPrivate;
	FMantleBodyPlace Place;
	int64 Counts[RegionCount] = {};
	int64 Total = 0;
	for (const TPair<FString, int32>& Bone : BoneVertices)
	{
		const EMantleBodyRegion Region = RegionOfBone(Bone.Key);
		if (Region != EMantleBodyRegion::Num && Bone.Value > 0)
		{
			Counts[static_cast<int32>(Region)] += Bone.Value;
			Total += Bone.Value;
		}
	}
	if (Total > 0)
	{
		for (int32 Region = 0; Region < RegionCount; ++Region)
		{
			Place.Share[Region] = static_cast<float>(static_cast<double>(Counts[Region]) / Total);
		}
	}
	else if (BodyHeightCm > 0.0f && HeightCm != FVector2f::ZeroVector)
	{
		// No weights (a root alone, or none): the middle of its height on a standing body.
		const float Middle = 0.5f * (HeightCm.X + HeightCm.Y) / BodyHeightCm;
		const EMantleBodyRegion Region = Middle >= 0.88f ? EMantleBodyRegion::Head : Middle >= 0.82f ? EMantleBodyRegion::Neck : Middle >= 0.6f ? EMantleBodyRegion::Chest
			: Middle >= 0.47f ? EMantleBodyRegion::Waist : Middle >= 0.06f ? EMantleBodyRegion::Legs : EMantleBodyRegion::Feet;
		Place.Share[static_cast<int32>(Region)] = 1.0f;
		Place.bByHeight = true;
	}
	else
	{
		return Place;
	}
	int32 Best = 0;
	for (int32 Region = 1; Region < RegionCount; ++Region)
	{
		Best = Place.Share[Region] > Place.Share[Best] ? Region : Best;
	}
	Place.Main = static_cast<EMantleBodyRegion>(Best);
	return Place;
}

FString FMantleCharacterSplit::SlotOfPlace(const FMantleBodyPlace& Place)
{
	using namespace MantleCharacterSplitPrivate;
	if (!Place.IsPlaced())
	{
		return FString();
	}
	// Mostly on the feet, or on the feet and the lower legs only (a boot): footwear.
	const float Upper = RegionShare(Place, EMantleBodyRegion::Chest) + RegionShare(Place, EMantleBodyRegion::Arms) + RegionShare(Place, EMantleBodyRegion::Waist);
	if (Place.Main == EMantleBodyRegion::Feet || (RegionShare(Place, EMantleBodyRegion::Feet) >= 0.35f && Upper < RegionMinShare))
	{
		return TEXT("footwear");
	}
	if (Place.Main == EMantleBodyRegion::Head || Place.Main == EMantleBodyRegion::Neck || Place.Main == EMantleBodyRegion::Hands)
	{
		return TEXT("accessory");
	}
	return RegionShare(Place, EMantleBodyRegion::Chest) + RegionShare(Place, EMantleBodyRegion::Arms) >= TopMinShare ? TEXT("top") : TEXT("bottom");
}

FMantlePackReview FMantleCharacterSplit::Review(const TConstArrayView<FMantleSplitSource> Meshes, const int32 Picked,
	const TFunctionRef<double(int32 TrimMesh, int32 OnMesh)> TrimShare)
{
	using namespace MantleCharacterSplitPrivate;
	FMantlePackReview Out;
	Out.Picked = Picked;
	float BodyHeight = Meshes.IsValidIndex(Picked) ? Meshes[Picked].HeightCm : 0.0f;
	for (const FMantleSplitSource& Mesh : Meshes)
	{
		Out.MeshNames.Add(Mesh.MeshName);
	}
	if (BodyHeight <= 0.0f)
	{
		for (const FMantleSplitSource& Mesh : Meshes)
		{
			BodyHeight = FMath::Max(BodyHeight, Mesh.HeightCm);
		}
	}
	const FString PickedLabel = Meshes.IsValidIndex(Picked) ? FMantleAvatarAssemblyFixer::DisplayName(Meshes[Picked].MeshName) : FString();
	for (int32 MeshIndex = 0; MeshIndex < Meshes.Num(); ++MeshIndex)
	{
		const FMantleSplitSource& Mesh = Meshes[MeshIndex];
		const FString Label = FMantleAvatarAssemblyFixer::DisplayName(Mesh.MeshName);
		// Strip's own skin check, section by section (as FMantleGarmentOutfit::FindBareBody): skin, a face detail, or covering.
		int32 Skin = 0;
		TArray<FString> Covering;
		for (const FMantleOutfitSection& Section : Mesh.Sections)
		{
			if (FMantleAvatarStrip::LooksLikeSkin(Section.SlotName, Section.MaterialName, Section.BaseMaterialName))
			{
				++Skin;
			}
			else if (!FMantleAvatarStrip::NamesFaceDetail(Section.SlotName, Section.MaterialName))
			{
				Covering.Add(Section.SlotName);
			}
		}
		const double HeightShare = BodyHeight > 0.0f ? Mesh.HeightCm / BodyHeight : 0.0;
		if (Skin > 0 && HeightShare >= FMantleGarmentOutfit::BareHeightShare)
		{
			FMantlePackBody& Body = Out.Bodies.AddDefaulted_GetRef();
			Body.MeshIndex = MeshIndex;
			Body.Label = Label;
			Body.Rank = FMantleGarmentOutfit::BareBodyRank(Mesh.MeshName, Covering.Num(), HeightShare);
			Body.Why = Covering.IsEmpty() ? FString(TEXT("every part skin"))
				: Body.Rank >= 0 ? FString::Printf(TEXT("%d piece%s on it that Convert takes off (%s)"), Covering.Num(), Covering.Num() == 1 ? TEXT("") : TEXT("s"), *FString::Join(Covering, TEXT(", ")))
				: FString::Printf(TEXT("wears clothing (%s)"), *FString::Join(Covering, TEXT(", ")));
			continue;
		}
		FMantlePackPiece& Piece = Out.Pieces.AddDefaulted_GetRef();
		Piece.MeshIndex = MeshIndex;
		Piece.Label = Label;
		// Where it sits: the bone each of its vertices is weighted to most, every section together.
		TMap<FString, int32> Weights;
		FVector2f Height(TNumericLimits<float>::Max(), TNumericLimits<float>::Lowest());
		for (int32 Section = 0; Section < Mesh.Sections.Num(); ++Section)
		{
			Piece.Triangles += Mesh.Triangles.IsValidIndex(Section) ? Mesh.Triangles[Section] : 0;
			if (Mesh.SectionWeights.IsValidIndex(Section))
			{
				for (const TPair<FString, int32>& Bone : Mesh.SectionWeights[Section])
				{
					Weights.FindOrAdd(Bone.Key) += Bone.Value;
				}
			}
			if (Mesh.SectionHeights.IsValidIndex(Section) && Mesh.SectionHeights[Section] != FVector2f::ZeroVector)
			{
				Height.X = FMath::Min(Height.X, Mesh.SectionHeights[Section].X);
				Height.Y = FMath::Max(Height.Y, Mesh.SectionHeights[Section].Y);
			}
		}
		Piece.Place = PlaceOf(Weights, Height.X <= Height.Y ? Height : FVector2f::ZeroVector, BodyHeight);
		const FString Where = Piece.Place.Describe();
		// Hair: mostly on the head (by its weights, or its height band with none); long hair hangs below it, so its length doesn't
		// count. Mostly on the neck is an accessory (a choker).
		const bool bOnHead = Piece.Place.IsPlaced() && Piece.Place.Main == EMantleBodyRegion::Head;
		if (Skin > 0 && Covering.IsEmpty())
		{
			Piece.Why = FString::Printf(TEXT("all skin, on the %s: part of a body, not clothing"), *Where);
		}
		else if (bOnHead)
		{
			Piece.Slot = TEXT("hair");
			Piece.bTicked = true;
			Piece.Why = FString::Printf(TEXT("sits on the %s: hair"), *Where);
		}
		else if (Skin > 0)
		{
			Piece.ProposedSlot = SlotOfPlace(Piece.Place);
			Piece.Why = FString::Printf(TEXT("clothing and skin in one mesh, on the %s: it can't be an Outfit"), *Where);
		}
		else if (!Piece.Place.IsPlaced())
		{
			Piece.Why = TEXT("nothing places it: it has no skin weights and no height to read");
		}
		else
		{
			const FString Slot = SlotOfPlace(Piece.Place);
			Piece.ProposedSlot = Slot;
			if (Mesh.OnPicked >= 0.0f && Mesh.OnPicked < OnPickedShare)
			{
				Piece.Why = FString::Printf(TEXT("sits on the %s, but isn't part of %s (%.0f%% of it lies on it): another outfit in the pack"), *Where, *PickedLabel, 100.0f * Mesh.OnPicked);
			}
			else
			{
				Piece.Slot = Slot;
				Piece.bTicked = true;
				Piece.Why = FString::Printf(TEXT("sits on the %s"), *Where) + (Mesh.OnPicked >= 0.0f ? FString::Printf(TEXT(", part of %s"), *PickedLabel) : FString());
			}
		}
		if (Piece.ProposedSlot.IsEmpty())
		{
			Piece.ProposedSlot = Piece.Slot;
		}
	}
	Out.Bodies.StableSort([](const FMantlePackBody& A, const FMantlePackBody& B) { return A.Rank > B.Rank; });
	Out.Body = Out.Bodies.Num() > 0 && Out.Bodies[0].Rank >= 0 ? 0 : INDEX_NONE;
	Out.Pieces.StableSort([](const FMantlePackPiece& A, const FMantlePackPiece& B)
	{
		const int32 OrderA = SlotOrder(A.Slot);
		const int32 OrderB = SlotOrder(B.Slot);
		return OrderA != OrderB ? OrderA < OrderB : A.Label < B.Label;
	});

	// A thin piece lying on another of its slot is its edging (the trim rule, FMantleGarmentOutfit::TrimNear): into it, one item.
	// The smallest first, so a trim never takes a piece in.
	TArray<int32> BySize;
	for (int32 Index = 0; Index < Out.Pieces.Num(); ++Index)
	{
		BySize.Add(Index);
	}
	BySize.StableSort([&Out](const int32 A, const int32 B) { return Out.Pieces[A].Triangles < Out.Pieces[B].Triangles; });
	TSet<int32> Hosts;
	for (const int32 Trim : BySize)
	{
		FMantlePackPiece& Piece = Out.Pieces[Trim];
		if (!Piece.bTicked || !IsGarmentSlot(Piece.Slot) || Hosts.Contains(Trim))
		{
			continue;
		}
		int32 Best = INDEX_NONE;
		double BestShare = FMantleGarmentOutfit::TrimNearShare;
		for (int32 On = 0; On < Out.Pieces.Num(); ++On)
		{
			const FMantlePackPiece& Other = Out.Pieces[On];
			if (On == Trim || !Other.bTicked || Other.Slot != Piece.Slot || Other.PartOf != INDEX_NONE)
			{
				continue;
			}
			const double Share = TrimShare(Piece.MeshIndex, Other.MeshIndex);
			if (Share >= BestShare)
			{
				Best = On;
				BestShare = Share;
			}
		}
		if (Best != INDEX_NONE)
		{
			Piece.PartOf = Best;
			Piece.ProposedPartOf = Best;
			Piece.bTrim = true;
			Piece.Why += FString::Printf(TEXT("; %.0f%% of it lies within %.0f cm of %s: its edging"), 100.0 * BestShare, FMantleGarmentOutfit::TrimNearCm, *Out.Pieces[Best].Label);
			Hosts.Add(Best);
		}
	}
	// Several pieces in a slot are one item, unless two of them sit on one region (two tops): then one item each.
	for (int32 A = 0; A < Out.Pieces.Num(); ++A)
	{
		for (int32 B = A + 1; B < Out.Pieces.Num(); ++B)
		{
			const FMantlePackPiece& First = Out.Pieces[A];
			const FMantlePackPiece& Second = Out.Pieces[B];
			if (First.bTicked && Second.bTicked && IsGarmentSlot(First.Slot) && First.Slot == Second.Slot && First.PartOf == INDEX_NONE && Second.PartOf == INDEX_NONE
				&& PlacesOverlap(First.Place, Second.Place))
			{
				Out.Separate.Add(First.Slot);
			}
		}
	}
	return Out;
}

int32 FMantlePackReview::BodyMesh() const
{
	return Bodies.IsValidIndex(Body) && Bodies[Body].Rank >= 0 ? Bodies[Body].MeshIndex : INDEX_NONE;
}

TArray<FMantlePackReview::FItem> FMantlePackReview::Items() const
{
	TArray<FItem> Out;
	auto Into = [this](const int32 Host, TArray<int32>& With)
	{
		for (int32 Index = 0; Index < Pieces.Num(); ++Index)
		{
			if (Pieces[Index].bTicked && Pieces[Index].PartOf == Host)
			{
				With.Add(Index);
			}
		}
	};
	for (const TCHAR* Slot : { TEXT("top"), TEXT("bottom"), TEXT("footwear") })
	{
		TArray<int32> Heads;
		for (int32 Index = 0; Index < Pieces.Num(); ++Index)
		{
			if (Pieces[Index].bTicked && Pieces[Index].Slot == Slot && Pieces[Index].PartOf == INDEX_NONE)
			{
				Heads.Add(Index);
			}
		}
		if (Heads.IsEmpty())
		{
			continue;
		}
		if (IsOneItem(Slot))
		{
			// The largest piece is the item; the others, and what goes into each, go into it.
			Heads.StableSort([this](const int32 A, const int32 B) { return Pieces[A].Triangles > Pieces[B].Triangles; });
			FItem& Item = Out.AddDefaulted_GetRef();
			Item.Slot = Slot;
			Item.Main = Heads[0];
			Into(Heads[0], Item.With);
			for (int32 Head = 1; Head < Heads.Num(); ++Head)
			{
				Item.With.Add(Heads[Head]);
				Into(Heads[Head], Item.With);
			}
			continue;
		}
		for (const int32 Head : Heads)
		{
			FItem& Item = Out.AddDefaulted_GetRef();
			Item.Slot = Slot;
			Item.Main = Head;
			Into(Head, Item.With);
		}
	}
	return Out;
}

void FMantlePackReview::Move(const int32 Index, const FString& Slot, int32 PartOf)
{
	if (!Pieces.IsValidIndex(Index))
	{
		return;
	}
	if (Pieces.IsValidIndex(PartOf))
	{
		// Into the item the other piece is in: never a piece in a piece in a piece.
		while (Pieces[PartOf].PartOf != INDEX_NONE && Pieces[PartOf].PartOf != Index)
		{
			PartOf = Pieces[PartOf].PartOf;
		}
		if (PartOf == Index || Pieces[PartOf].PartOf == Index)
		{
			return;
		}
	}
	else
	{
		PartOf = INDEX_NONE;
	}
	FMantlePackPiece& Piece = Pieces[Index];
	Piece.PartOf = PartOf;
	Piece.bTrim = PartOf != INDEX_NONE && PartOf == Piece.ProposedPartOf;
	Piece.Slot = PartOf != INDEX_NONE ? Pieces[PartOf].Slot : Slot;
	Piece.bTicked = PartOf != INDEX_NONE || !Slot.IsEmpty();
	// What went into it follows it.
	for (int32 Other = 0; Other < Pieces.Num(); ++Other)
	{
		if (Other != Index && Pieces[Other].PartOf == Index)
		{
			Pieces[Other].PartOf = PartOf != INDEX_NONE ? PartOf : Index;
			Pieces[Other].bTrim = Pieces[Other].PartOf != INDEX_NONE && Pieces[Other].PartOf == Pieces[Other].ProposedPartOf;
			Pieces[Other].Slot = Piece.Slot;
		}
	}
}

FString FMantlePackReview::SlotHeading(const FMantlePackPiece& Piece) const
{
	using namespace MantleCharacterSplitPrivate;
	if (Piece.Slot.IsEmpty())
	{
		return TEXT("Not used");
	}
	FString Heading = Piece.Slot.Replace(TEXT("_"), TEXT(" "));
	Heading[0] = FChar::ToUpper(Heading[0]);
	if (Piece.Slot == TEXT("accessory") && Piece.Place.IsPlaced())
	{
		Heading += FString::Printf(TEXT(" (%s)"), RegionName(Piece.Place.Main));
	}
	return Heading;
}

FString FMantlePackReview::RowText(const int32 Index) const
{
	if (!Pieces.IsValidIndex(Index))
	{
		return FString();
	}
	const FMantlePackPiece& Piece = Pieces[Index];
	if (Piece.PartOf != INDEX_NONE)
	{
		return FString::Printf(TEXT("%s: %s %s"), *Piece.Label, Piece.bTrim ? TEXT("the edging of") : TEXT("goes into"), Pieces.IsValidIndex(Piece.PartOf) ? *Pieces[Piece.PartOf].Label : TEXT("?"));
	}
	TArray<FString> Edging;
	TArray<FString> With;
	for (const FMantlePackPiece& Other : Pieces)
	{
		if (&Other != &Piece && Other.PartOf == Index && Other.bTicked)
		{
			(Other.bTrim ? Edging : With).Add(Other.Label);
		}
	}
	FString Text = Piece.Label;
	if (Edging.Num() > 0)
	{
		Text += FString::Printf(TEXT(", with edging (%s)"), *MantleCharacterSplitPrivate::JoinAnd(Edging));
	}
	if (With.Num() > 0)
	{
		Text += FString::Printf(TEXT(", with %s"), *MantleCharacterSplitPrivate::JoinAnd(With));
	}
	return Text + TEXT(": ") + Piece.Place.Describe();
}

TArray<FString> FMantlePackReview::Describe(const FString& Label) const
{
	using namespace MantleCharacterSplitPrivate;
	TArray<FString> Lines;
	const FMantlePackBody* Chosen = Bodies.IsValidIndex(Body) ? &Bodies[Body] : nullptr;
	Lines.Add(FString::Printf(TEXT("MANTLE_PACK %s: Body: %s"), *Label, Chosen && Chosen->Rank >= 0 ? *FString::Printf(TEXT("%s (%s)"), *Chosen->Label, *Chosen->Why) : TEXT("none to offer")));
	for (int32 Index = 0; Index < Bodies.Num(); ++Index)
	{
		if (Index != Body)
		{
			Lines.Add(FString::Printf(TEXT("MANTLE_PACK %s:   other body: %s (%s)%s"), *Label, *Bodies[Index].Label, *Bodies[Index].Why, Bodies[Index].Rank >= 0 ? TEXT(", can be picked") : TEXT("")));
		}
	}
	for (int32 Index = 0; Index < Pieces.Num(); ++Index)
	{
		const FMantlePackPiece& Piece = Pieces[Index];
		Lines.Add(FString::Printf(TEXT("MANTLE_PACK %s:   %s: %s%s %s [%s] (%s)"), *Label, *SlotHeading(Piece), Piece.PartOf != INDEX_NONE ? TEXT("  + ") : TEXT(""),
			Piece.Slot.IsEmpty() ? TEXT("") : Piece.bTicked ? TEXT("[x]") : TEXT("[ ]"), *RowText(Index), *PlaceShares(Piece.Place), *Piece.Why));
	}
	for (const TCHAR* Slot : { TEXT("top"), TEXT("bottom"), TEXT("footwear") })
	{
		int32 Heads = 0;
		for (const FMantlePackPiece& Piece : Pieces)
		{
			Heads += Piece.bTicked && Piece.Slot == Slot && Piece.PartOf == INDEX_NONE ? 1 : 0;
		}
		if (Heads > 1)
		{
			Lines.Add(FString::Printf(TEXT("MANTLE_PACK %s:   %s: %d pieces, %s"), *Label, Slot, Heads, IsOneItem(Slot) ? TEXT("one item") : TEXT("one item each (two sit on the same part of the body)")));
		}
	}
	for (const FItem& Item : Items())
	{
		TArray<FString> With;
		for (const int32 Piece : Item.With)
		{
			With.Add(Pieces[Piece].Label);
		}
		Lines.Add(FString::Printf(TEXT("MANTLE_PACK %s: item %s: %s%s"), *Label, *Item.Slot, *Pieces[Item.Main].Label, With.IsEmpty() ? TEXT("") : *(TEXT(" with ") + JoinAnd(With))));
	}
	const int32 Rows = Bodies.Num() + Pieces.Num();
	Lines.Add(FString::Printf(TEXT("MANTLE_PACK %s: %d meshes, %d in the rows, %d dropped"), *Label, MeshNames.Num(), Rows, MeshNames.Num() - Rows));
	return Lines;
}

FMantlePackReview FMantleCharacterSplit::ReviewFor(USkeletalMesh& Picked)
{
	TArray<USkeletalMesh*> Meshes = { &Picked };
	TArray<FMantleSplitSource> Sources;
	{
		FMantleSplitSource Source;
		if (!Gather(Picked, Source))
		{
			return FMantlePackReview();
		}
		Sources.Add(MoveTemp(Source));
	}
	// The meshes beside it: its folder, never Convert's output.
	const FString Folder = FPackageName::GetLongPackagePath(Picked.GetPackage()->GetName());
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FARFilter Filter;
	Filter.PackagePaths.Add(*Folder);
	Filter.bRecursivePaths = true;
	Filter.ClassPaths.Add(USkeletalMesh::StaticClass()->GetClassPathName());
	TArray<FAssetData> Assets;
	Registry.GetAssets(Filter, Assets);
	Assets.Sort([](const FAssetData& A, const FAssetData& B) { return A.PackageName.LexicalLess(B.PackageName); });
	for (const FAssetData& Asset : Assets)
	{
		if (Asset.PackageName == Picked.GetPackage()->GetFName() || (Asset.PackagePath.ToString() + TEXT("/")).Contains(TEXT("/Converted/")))
		{
			continue;
		}
		USkeletalMesh* Other = Cast<USkeletalMesh>(Asset.GetAsset());
		FMantleSplitSource Source;
		if (Other && Gather(*Other, Source))
		{
			Meshes.Add(Other);
			Sources.Add(MoveTemp(Source));
		}
	}

	// Each mesh's geometry as made, read when first needed.
	TArray<FMantleGlbMesh> Read;
	Read.SetNum(Meshes.Num());
	TBitArray<> bRead(false, Meshes.Num());
	const auto Geometry = [&](const int32 Index) -> const FMantleGlbMesh*
	{
		if (!bRead[Index])
		{
			bRead[Index] = true;
			TArray<UMaterialInterface*> Materials;
			int32 Triangles = 0;
			FString Error;
			if (!FMantleCosmeticExport::ReadSkeletalMesh(Meshes[Index], 0, Read[Index], Materials, Triangles, Error))
			{
				Read[Index] = FMantleGlbMesh();
			}
		}
		return Read[Index].Positions.Num() > 0 ? &Read[Index] : nullptr;
	};

	// When the picked mesh wears clothing it is the outfit: how much of each other piece lies on it.
	bool bWears = false;
	for (const FMantleOutfitSection& Section : Sources[0].Sections)
	{
		bWears |= !FMantleAvatarStrip::LooksLikeSkin(Section.SlotName, Section.MaterialName, Section.BaseMaterialName)
			&& !FMantleAvatarStrip::NamesFaceDetail(Section.SlotName, Section.MaterialName);
	}
	if (const FMantleGlbMesh* PickedGeometry = bWears ? Geometry(0) : nullptr)
	{
		FMantleSkinnedMeshLOD Surface;
		TArray<int32> PointOf;
		FMantleGarmentOutfit::WeldedSurface(*PickedGeometry, Surface, PointOf);
		for (int32 Index = 1; Index < Sources.Num(); ++Index)
		{
			// Another whole body is never a piece of it.
			if (Sources[Index].HeightCm >= FMantleGarmentOutfit::BareHeightShare * Sources[0].HeightCm)
			{
				continue;
			}
			const FMantleGlbMesh* Piece = Geometry(Index);
			Sources[Index].OnPicked = Piece ? static_cast<float>(FMantleGarmentOutfit::ShareOn(Piece->Positions, Surface, OnPickedCm)) : 0.0f;
			UE_LOG(LogMantle, Log, TEXT("MANTLE_PACK %s on %s: %.0f%% within %.1f cm"), *Sources[Index].MeshName, *Picked.GetName(), 100.0f * Sources[Index].OnPicked, OnPickedCm);
		}
	}
	FMantlePackReview Review = FMantleCharacterSplit::Review(Sources, 0, [&](const int32 Trim, const int32 On)
	{
		const FMantleGlbMesh* TrimMesh = Geometry(Trim);
		const FMantleGlbMesh* OnMesh = TrimMesh ? Geometry(On) : nullptr;
		const double Share = OnMesh ? FMantleGarmentOutfit::TrimNear(*TrimMesh, *OnMesh) : 0.0;
		UE_LOG(LogMantle, Log, TEXT("MANTLE_PACK trim check %s on %s: %.0f%% within %.0f cm"), *Sources[Trim].MeshName, *Sources[On].MeshName, 100.0 * Share, FMantleGarmentOutfit::TrimNearCm);
		return Share;
	});
	for (USkeletalMesh* Mesh : Meshes)
	{
		Review.Meshes.Add(Mesh);
	}
	for (const FString& Line : Review.Describe(Picked.GetName()))
	{
		UE_LOG(LogMantle, Log, TEXT("%s"), *Line);
	}
	return Review;
}
