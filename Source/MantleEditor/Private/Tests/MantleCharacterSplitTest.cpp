// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCharacterSplit.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// #55 step 1: the split's rules on names and bones. No assets.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleCharacterSplitTestPrivate
{
	FMantleSplitSource SplitTestMesh(const TCHAR* MeshName, std::initializer_list<const TCHAR*> SectionNames)
	{
		FMantleSplitSource Source;
		Source.MeshName = MeshName;
		for (const TCHAR* Name : SectionNames)
		{
			FMantleOutfitSection Section;
			Section.SlotName = Name;
			Section.MaterialName = Name;
			Source.Sections.Add(Section);
			Source.Triangles.Add(1000);
			Source.SectionBones.AddDefaulted();
		}
		return Source;
	}

	const FMantleSplitPiece* SplitFind(const FMantleSplitProposal& Proposal, const EMantleSplitRole Role, const TCHAR* Slot)
	{
		return Proposal.Pieces.FindByPredicate([Role, Slot](const FMantleSplitPiece& Piece) { return Piece.Role == Role && Piece.Slot == Slot; });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCharacterSplitSeparateMeshesTest,
	"Mantle.Editor.CharacterSplit.SeparateMeshes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCharacterSplitSeparateMeshesTest::RunTest(const FString& Parameters)
{
	using namespace MantleCharacterSplitTestPrivate;
	// A Vampire-style character: a bare body, clothing meshes, hair.
	const TArray<FMantleSplitSource> Meshes = {
		SplitTestMesh(TEXT("SK_Girl_Nude"), { TEXT("M_Body_Skin") }),
		SplitTestMesh(TEXT("SK_Girl_Shirt"), { TEXT("M_Shirt") }),
		SplitTestMesh(TEXT("SK_Girl_Pants"), { TEXT("M_Pants") }),
		SplitTestMesh(TEXT("SK_Girl_Shoes"), { TEXT("M_Shoes") }),
		SplitTestMesh(TEXT("SK_Girl_Jacket"), { TEXT("M_Jacket_Shirt") }),
		SplitTestMesh(TEXT("SK_Girl_Hair"), { TEXT("M_Hair_Cards") }),
	};
	const FMantleSplitProposal Proposal = FMantleCharacterSplit::Propose(Meshes);
	TestTrue(TEXT("a bare body and clothing split"), Proposal.bSplittable);
	TestEqual(TEXT("six pieces"), Proposal.Pieces.Num(), 6);
	TestTrue(TEXT("the body is first"), Proposal.Pieces[0].Role == EMantleSplitRole::Body);
	TestNotNull(TEXT("a top"), SplitFind(Proposal, EMantleSplitRole::Garment, TEXT("top")));
	TestNotNull(TEXT("a bottom"), SplitFind(Proposal, EMantleSplitRole::Garment, TEXT("bottom")));
	TestNotNull(TEXT("footwear"), SplitFind(Proposal, EMantleSplitRole::Garment, TEXT("footwear")));
	TestNotNull(TEXT("hair is its own item"), SplitFind(Proposal, EMantleSplitRole::Hair, TEXT("hair")));
	const FMantleSplitPiece* Jacket = Proposal.Pieces.FindByPredicate([](const FMantleSplitPiece& Piece) { return Piece.Name.Contains(TEXT("Jacket")); });
	const FMantleSplitPiece* Shirt = Proposal.Pieces.FindByPredicate([](const FMantleSplitPiece& Piece) { return Piece.Name == TEXT("Girl Shirt"); });
	TestTrue(TEXT("a jacket is drawn over a shirt"), Jacket && Shirt && Jacket->Layer > Shirt->Layer);
	TestEqual(TEXT("hair is last"), Proposal.Pieces.Last().Role, EMantleSplitRole::Hair);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCharacterSplitSectionsTest,
	"Mantle.Editor.CharacterSplit.Sections",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCharacterSplitSectionsTest::RunTest(const FString& Parameters)
{
	using namespace MantleCharacterSplitTestPrivate;
	// One mesh, several material sections: skin, a shirt, trousers and a belt.
	const FMantleSplitProposal Sections = FMantleCharacterSplit::Propose({ SplitTestMesh(TEXT("SK_Hero"), { TEXT("M_Skin"), TEXT("M_Shirt"), TEXT("M_Trousers"), TEXT("M_Belt") }) });
	TestTrue(TEXT("sections split a one-mesh character"), Sections.bSplittable);
	TestNotNull(TEXT("its skin is the body"), SplitFind(Sections, EMantleSplitRole::Body, TEXT("avatar")));
	TestNotNull(TEXT("its shirt is a top"), SplitFind(Sections, EMantleSplitRole::Garment, TEXT("top")));
	TestNotNull(TEXT("its trousers are a bottom"), SplitFind(Sections, EMantleSplitRole::Garment, TEXT("bottom")));
	TestNotNull(TEXT("its belt is an accessory"), SplitFind(Sections, EMantleSplitRole::Accessory, TEXT("accessory")));

	// One mesh, one section: nothing to split.
	const FMantleSplitProposal One = FMantleCharacterSplit::Propose({ SplitTestMesh(TEXT("SK_Hero"), { TEXT("M_Character") }) });
	TestFalse(TEXT("a one-section character stays a fixed avatar"), One.bSplittable);
	TestTrue(TEXT("and says so"), One.WhyNot.Contains(TEXT("fixed avatar")));

	// A body with clothing built into it (the fused-clothing verdict): not splittable.
	FMantleSplitSource Fused = SplitTestMesh(TEXT("SK_Hero"), { TEXT("M_Skin"), TEXT("M_Shirt") });
	Fused.Fused = EMantleFusedVerdict::FusedClothing;
	Fused.FusedWhy = TEXT("clothing looks built into the body");
	const FMantleSplitProposal Welded = FMantleCharacterSplit::Propose({ Fused });
	TestFalse(TEXT("clothing built into the skin is not splittable"), Welded.bSplittable);
	TestTrue(TEXT("and names the clothing"), Welded.WhyNot.Contains(TEXT("built into the body")));

	// Clothes and no body: nothing to fit from.
	const FMantleSplitProposal NoBody = FMantleCharacterSplit::Propose({ SplitTestMesh(TEXT("SK_Hero_Shirt"), { TEXT("M_Shirt") }), SplitTestMesh(TEXT("SK_Hero_Pants"), { TEXT("M_Pants") }) });
	TestFalse(TEXT("no bare body, no split"), NoBody.bSplittable);

	// A bare body alone: no clothing to offer.
	const FMantleSplitProposal NoClothes = FMantleCharacterSplit::Propose({ SplitTestMesh(TEXT("SK_Hero_Body"), { TEXT("M_Skin") }) });
	TestFalse(TEXT("a body with no clothing does not split"), NoClothes.bSplittable);

	// A bow is not clothing.
	const FMantleSplitProposal Prop = FMantleCharacterSplit::Propose({ SplitTestMesh(TEXT("SK_Hero_Body"), { TEXT("M_Skin") }), SplitTestMesh(TEXT("SK_Hero_Shirt"), { TEXT("M_Shirt") }),
		SplitTestMesh(TEXT("SK_Hero_Bow"), { TEXT("M_Bow") }) });
	TestTrue(TEXT("a prop is left out, the rest splits"), Prop.bSplittable && SplitFind(Prop, EMantleSplitRole::LeftOut, TEXT("")) != nullptr);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCharacterSplitBonesTest,
	"Mantle.Editor.CharacterSplit.Bones",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCharacterSplitBonesTest::RunTest(const FString& Parameters)
{
	using namespace MantleCharacterSplitTestPrivate;
	TestEqual(TEXT("head bones are the head"), FMantleCharacterSplit::RegionOf({ TEXT("head"), TEXT("neck_01"), TEXT("neck_02") }), FString(TEXT("head")));
	TestEqual(TEXT("finger bones are the hands"), FMantleCharacterSplit::RegionOf({ TEXT("hand_l"), TEXT("index_01_l"), TEXT("thumb_01_l"), TEXT("middle_01_l") }), FString(TEXT("hands")));
	TestEqual(TEXT("foot bones are the feet"), FMantleCharacterSplit::RegionOf({ TEXT("foot_l"), TEXT("ball_l"), TEXT("calf_l") }), FString(TEXT("feet")));
	TestEqual(TEXT("a mix is the body"), FMantleCharacterSplit::RegionOf({ TEXT("spine_01"), TEXT("spine_02"), TEXT("head") }), FString(TEXT("body")));

	// A mesh the names don't place is placed by its bones: skinned to the feet it is footwear, to the head an accessory.
	FMantleSplitSource Feet = SplitTestMesh(TEXT("SK_Hero_Thing"), { TEXT("M_Thing") });
	Feet.SectionBones[0] = { TEXT("foot_l"), TEXT("ball_l"), TEXT("foot_r") };
	FMantleSplitSource Head = SplitTestMesh(TEXT("SK_Hero_Thing2"), { TEXT("M_Thing2") });
	Head.SectionBones[0] = { TEXT("head"), TEXT("neck_01") };
	const FMantleSplitProposal Proposal = FMantleCharacterSplit::Propose({ SplitTestMesh(TEXT("SK_Hero_Body"), { TEXT("M_Skin") }), Feet, Head });
	TestNotNull(TEXT("skinned to the feet: footwear"), SplitFind(Proposal, EMantleSplitRole::Garment, TEXT("footwear")));
	TestNotNull(TEXT("skinned to the head: an accessory"), SplitFind(Proposal, EMantleSplitRole::Accessory, TEXT("accessory")));
	TestTrue(TEXT("a hat is an accessory by name"), FMantleCharacterSplit::NamesAccessory(TEXT("SK_Hero_Hat")));
	TestFalse(TEXT("a shirt is not"), FMantleCharacterSplit::NamesAccessory(TEXT("SK_Hero_Shirt")));
	TestTrue(TEXT("underwear is drawn under trousers"), FMantleCharacterSplit::LayerOf(EMantleSplitRole::Garment, TEXT("bottom"), TEXT("Underwear")) < FMantleCharacterSplit::LayerOf(EMantleSplitRole::Garment, TEXT("bottom"), TEXT("Pants")));
	return true;
}

// #55, the pack review: pieces placed by the bones they are weighted to, names only for labels. No assets.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleCharacterSplitTestPrivate
{
	/** A bone and how many vertices it carries most. */
	struct FPackTestBone
	{
		const TCHAR* Bone;
		int32 Vertices;
	};

	/** A mesh of the pack: one section named Material, its vertices weighted to Bones, HeightCm tall from MinZ. */
	FMantleSplitSource PackTestMesh(const TCHAR* MeshName, const TCHAR* Material, std::initializer_list<FPackTestBone> Bones, const float HeightCm,
		const float MinZ = 0.0f, const int32 Triangles = 1000)
	{
		FMantleSplitSource Source;
		Source.MeshName = MeshName;
		FMantleOutfitSection Section;
		Section.SlotName = Material;
		Section.MaterialName = Material;
		Source.Sections.Add(Section);
		Source.Triangles.Add(Triangles);
		Source.SectionBones.AddDefaulted();
		TMap<FString, int32>& Weights = Source.SectionWeights.AddDefaulted_GetRef();
		for (const FPackTestBone& Bone : Bones)
		{
			Weights.Add(Bone.Bone, Bone.Vertices);
			Source.SectionBones[0].Add(Bone.Bone);
		}
		Source.SectionHeights.Add(FVector2f(MinZ, MinZ + HeightCm));
		Source.HeightCm = HeightCm;
		return Source;
	}

	FMantleOutfitSection PackTestSection(const TCHAR* Material)
	{
		FMantleOutfitSection Section;
		Section.SlotName = Material;
		Section.MaterialName = Material;
		return Section;
	}

	int32 PackFind(const FMantlePackReview& Review, const TCHAR* MeshName)
	{
		return Review.Pieces.IndexOfByPredicate([&Review, MeshName](const FMantlePackPiece& Piece) { return Review.MeshNames[Piece.MeshIndex] == MeshName; });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCharacterSplitRegionsTest,
	"Mantle.Editor.CharacterSplit.Regions",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCharacterSplitRegionsTest::RunTest(const FString& Parameters)
{
	// Bones to regions: UE and MetaHuman names, Mixamo's, the face's.
	TestTrue(TEXT("upperarm is the arms"), FMantleCharacterSplit::RegionOfBone(TEXT("upperarm_twist_01_l")) == EMantleBodyRegion::Arms);
	TestTrue(TEXT("Mixamo's forearm is the arms"), FMantleCharacterSplit::RegionOfBone(TEXT("mixamorig:LeftForeArm")) == EMantleBodyRegion::Arms);
	TestTrue(TEXT("a finger is the hands"), FMantleCharacterSplit::RegionOfBone(TEXT("index_02_r")) == EMantleBodyRegion::Hands);
	TestTrue(TEXT("a face bone is the head, not a finger"), FMantleCharacterSplit::RegionOfBone(TEXT("FACIAL_C_LipUpperMiddle")) == EMantleBodyRegion::Head);
	TestTrue(TEXT("neck_01 is the neck"), FMantleCharacterSplit::RegionOfBone(TEXT("neck_01")) == EMantleBodyRegion::Neck);
	TestTrue(TEXT("spine_02 is the waist"), FMantleCharacterSplit::RegionOfBone(TEXT("spine_02")) == EMantleBodyRegion::Waist);
	TestTrue(TEXT("spine_03 is the chest"), FMantleCharacterSplit::RegionOfBone(TEXT("spine_03")) == EMantleBodyRegion::Chest);
	TestTrue(TEXT("Mixamo's Spine2 is the chest"), FMantleCharacterSplit::RegionOfBone(TEXT("mixamorig:Spine2")) == EMantleBodyRegion::Chest);
	TestTrue(TEXT("Mixamo's Spine is the waist"), FMantleCharacterSplit::RegionOfBone(TEXT("mixamorig:Spine")) == EMantleBodyRegion::Waist);
	TestTrue(TEXT("clavicle is the chest"), FMantleCharacterSplit::RegionOfBone(TEXT("clavicle_l")) == EMantleBodyRegion::Chest);
	TestTrue(TEXT("thigh is the legs"), FMantleCharacterSplit::RegionOfBone(TEXT("thigh_r")) == EMantleBodyRegion::Legs);
	TestTrue(TEXT("Mixamo's UpLeg is the legs"), FMantleCharacterSplit::RegionOfBone(TEXT("mixamorig:RightUpLeg")) == EMantleBodyRegion::Legs);
	TestTrue(TEXT("ball is the feet"), FMantleCharacterSplit::RegionOfBone(TEXT("ball_l")) == EMantleBodyRegion::Feet);
	TestTrue(TEXT("root is nowhere"), FMantleCharacterSplit::RegionOfBone(TEXT("root")) == EMantleBodyRegion::Num);

	// Places to slots.
	auto Slot = [](std::initializer_list<MantleCharacterSplitTestPrivate::FPackTestBone> Bones)
	{
		TMap<FString, int32> Weights;
		for (const MantleCharacterSplitTestPrivate::FPackTestBone& Bone : Bones)
		{
			Weights.Add(Bone.Bone, Bone.Vertices);
		}
		return FMantleCharacterSplit::SlotOfPlace(FMantleCharacterSplit::PlaceOf(Weights, FVector2f::ZeroVector, 170.0f));
	};
	TestEqual(TEXT("a corset (chest and waist) is a top"), Slot({ { TEXT("spine_03"), 500 }, { TEXT("spine_02"), 300 }, { TEXT("pelvis"), 200 } }), FString(TEXT("top")));
	TestEqual(TEXT("a nameless piece on the arms is a top"), Slot({ { TEXT("upperarm_l"), 400 }, { TEXT("lowerarm_l"), 400 }, { TEXT("upperarm_r"), 400 }, { TEXT("lowerarm_r"), 400 }, { TEXT("hand_l"), 50 } }), FString(TEXT("top")));
	TestEqual(TEXT("trousers (waist and legs) are a bottom"), Slot({ { TEXT("pelvis"), 200 }, { TEXT("thigh_l"), 400 }, { TEXT("calf_l"), 300 }, { TEXT("foot_l"), 20 } }), FString(TEXT("bottom")));
	TestEqual(TEXT("shoes are footwear"), Slot({ { TEXT("foot_l"), 300 }, { TEXT("ball_l"), 100 }, { TEXT("calf_l"), 30 } }), FString(TEXT("footwear")));
	TestEqual(TEXT("a boot up the calf is footwear"), Slot({ { TEXT("foot_l"), 300 }, { TEXT("calf_l"), 400 } }), FString(TEXT("footwear")));
	TestEqual(TEXT("a choker is an accessory"), Slot({ { TEXT("neck_01"), 300 }, { TEXT("spine_05"), 40 } }), FString(TEXT("accessory")));
	TestEqual(TEXT("gloves are an accessory"), Slot({ { TEXT("hand_r"), 200 }, { TEXT("index_01_r"), 100 } }), FString(TEXT("accessory")));
	TestEqual(TEXT("nothing places nothing"), Slot({ { TEXT("root"), 300 } }), FString());
	// With no weights, its height places it.
	const FMantleBodyPlace High = FMantleCharacterSplit::PlaceOf({}, FVector2f(110.0f, 140.0f), 170.0f);
	TestTrue(TEXT("by its height, chest high"), High.bByHeight && High.Main == EMantleBodyRegion::Chest);
	TestEqual(TEXT("so a top"), FMantleCharacterSplit::SlotOfPlace(High), FString(TEXT("top")));
	TestFalse(TEXT("no weights and no height: unplaced"), FMantleCharacterSplit::PlaceOf({}, FVector2f::ZeroVector, 170.0f).IsPlaced());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCharacterSplitPackReviewTest,
	"Mantle.Editor.CharacterSplit.PackReview",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCharacterSplitPackReviewTest::RunTest(const FString& Parameters)
{
	using namespace MantleCharacterSplitTestPrivate;
	// A Vampire-style pack, the dressed character picked. The sleeves' name says nothing about where they go ("SK_Piece_A" on the arms).
	TArray<FMantleSplitSource> Meshes = {
		PackTestMesh(TEXT("SK_Girl_Dressed"), TEXT("M_VG_Corset"), { { TEXT("spine_03"), 900 } }, 170.0f),
		PackTestMesh(TEXT("SK_Girl"), TEXT("M_VG_Body"), { { TEXT("spine_03"), 900 } }, 166.0f),
		PackTestMesh(TEXT("SK_Girl_Underwear"), TEXT("M_VG_Body"), { { TEXT("spine_03"), 900 } }, 170.0f),
		PackTestMesh(TEXT("SK_Girl_Nightwear"), TEXT("M_VG_Body"), { { TEXT("spine_03"), 900 } }, 170.0f),
		PackTestMesh(TEXT("SK_Corset"), TEXT("M_VG_Corset"), { { TEXT("spine_03"), 600 }, { TEXT("spine_02"), 400 } }, 40.0f, 95.0f, 2650),
		PackTestMesh(TEXT("SK_LaceCorset"), TEXT("M_VG_Lace"), { { TEXT("spine_03"), 300 } }, 30.0f, 105.0f, 152),
		PackTestMesh(TEXT("SK_Piece_A"), TEXT("M_VG_Lace"), { { TEXT("upperarm_l"), 300 }, { TEXT("lowerarm_l"), 300 }, { TEXT("upperarm_r"), 300 }, { TEXT("lowerarm_r"), 300 } }, 30.0f, 100.0f, 900),
		PackTestMesh(TEXT("SK_LaceNeck"), TEXT("M_VG_Lace"), { { TEXT("neck_01"), 200 } }, 4.0f, 145.0f, 100),
		PackTestMesh(TEXT("SK_Pants"), TEXT("M_VG_Pants"), { { TEXT("pelvis"), 200 }, { TEXT("thigh_l"), 400 }, { TEXT("calf_r"), 400 } }, 90.0f, 5.0f, 3000),
		PackTestMesh(TEXT("SK_Shoes"), TEXT("M_VG_Shoes"), { { TEXT("foot_l"), 300 }, { TEXT("ball_r"), 100 } }, 15.0f, 0.0f, 1500),
		PackTestMesh(TEXT("SK_Piece_B"), TEXT("M_VG_Strands"), { { TEXT("head"), 800 }, { TEXT("neck_01"), 100 } }, 30.0f, 140.0f, 8000),
		PackTestMesh(TEXT("SK_Piece_C"), TEXT("M_VG_Strands"), { { TEXT("head"), 500 }, { TEXT("neck_01"), 200 }, { TEXT("spine_05"), 150 } }, 70.0f, 100.0f, 6000),
		PackTestMesh(TEXT("SK_Torso"), TEXT("M_VG_Body"), { { TEXT("spine_03"), 900 } }, 60.0f, 90.0f),
		PackTestMesh(TEXT("SK_Nightwear_Top"), TEXT("M_VG_Nightwear"), { { TEXT("spine_03"), 900 } }, 40.0f, 95.0f, 1800),
		PackTestMesh(TEXT("SK_Mystery"), TEXT("M_VG_Thing"), { { TEXT("root"), 300 } }, 0.0f),
	};
	Meshes[0].Sections.Insert(PackTestSection(TEXT("M_VG_Body")), 0);
	Meshes[2].Sections.Add(PackTestSection(TEXT("M_VG_Underwear")));
	Meshes[3].Sections.Add(PackTestSection(TEXT("M_VG_Nightwear")));
	Meshes[14].SectionHeights[0] = FVector2f::ZeroVector;
	// Everything is part of the dressed character but the nightwear top (as measured on its surface).
	for (int32 Index = 4; Index < Meshes.Num(); ++Index)
	{
		Meshes[Index].OnPicked = Index == 13 ? 0.1f : 0.95f;
	}
	// The trim rule (FMantleGarmentOutfit::TrimNear) as measured: the lace lies on the corset.
	const FMantlePackReview Review = FMantleCharacterSplit::Review(Meshes, 0, [](const int32 Trim, const int32 On) { return Trim == 5 && On == 4 ? 1.0 : 0.0; });

	TestEqual(TEXT("nothing dropped: every mesh is a body or a row"), Review.Bodies.Num() + Review.Pieces.Num(), Meshes.Num());
	TestEqual(TEXT("the all-skin body is the one chosen"), Review.BodyMesh(), 1);
	TestTrue(TEXT("the body in underwear can be picked"), Review.Bodies.ContainsByPredicate([](const FMantlePackBody& Body) { return Body.MeshIndex == 2 && Body.Rank >= 0; }));
	TestTrue(TEXT("the dressed one is not offered"), Review.Bodies.ContainsByPredicate([](const FMantlePackBody& Body) { return Body.MeshIndex == 0 && Body.Rank < 0; }));

	const int32 Corset = PackFind(Review, TEXT("SK_Corset"));
	const int32 Lace = PackFind(Review, TEXT("SK_LaceCorset"));
	const int32 Arms = PackFind(Review, TEXT("SK_Piece_A"));
	const int32 Neck = PackFind(Review, TEXT("SK_LaceNeck"));
	const int32 Night = PackFind(Review, TEXT("SK_Nightwear_Top"));
	if (!TestTrue(TEXT("the pieces are rows"), Corset != INDEX_NONE && Lace != INDEX_NONE && Arms != INDEX_NONE && Neck != INDEX_NONE && Night != INDEX_NONE))
	{
		return false;
	}
	TestEqual(TEXT("the corset is a top"), Review.Pieces[Corset].Slot, FString(TEXT("top")));
	TestEqual(TEXT("a nameless piece on the arms lands under top"), Review.Pieces[Arms].Slot, FString(TEXT("top")));
	TestTrue(TEXT("the lace is the corset's edging"), Review.Pieces[Lace].PartOf == Corset && Review.Pieces[Lace].bTrim);
	TestTrue(TEXT("the choker is an accessory on the neck"), Review.Pieces[Neck].Slot == TEXT("accessory") && Review.Pieces[Neck].Place.Main == EMantleBodyRegion::Neck);
	TestEqual(TEXT("the trousers are a bottom"), Review.Pieces[PackFind(Review, TEXT("SK_Pants"))].Slot, FString(TEXT("bottom")));
	TestEqual(TEXT("the shoes are footwear"), Review.Pieces[PackFind(Review, TEXT("SK_Shoes"))].Slot, FString(TEXT("footwear")));
	TestEqual(TEXT("a nameless piece on the head is hair"), Review.Pieces[PackFind(Review, TEXT("SK_Piece_B"))].Slot, FString(TEXT("hair")));
	TestEqual(TEXT("long hair hanging below the head is still hair"), Review.Pieces[PackFind(Review, TEXT("SK_Piece_C"))].Slot, FString(TEXT("hair")));
	TestEqual(TEXT("the choker, mostly on the neck, is not hair"), Review.Pieces[Neck].Slot, FString(TEXT("accessory")));
	TestTrue(TEXT("a torso of skin is not used"), Review.Pieces[PackFind(Review, TEXT("SK_Torso"))].Slot.IsEmpty());
	TestTrue(TEXT("another outfit is not used, and says so"), Review.Pieces[Night].Slot.IsEmpty() && Review.Pieces[Night].Why.Contains(TEXT("another outfit")));
	TestEqual(TEXT("but would be a top"), Review.Pieces[Night].ProposedSlot, FString(TEXT("top")));
	TestTrue(TEXT("what nothing places is not used"), Review.Pieces[PackFind(Review, TEXT("SK_Mystery"))].Slot.IsEmpty());

	// The corset and the sleeves don't sit on one region: one item, the corset with its lace and the sleeves.
	TestTrue(TEXT("one item in the top slot"), Review.IsOneItem(TEXT("top")));
	TArray<FMantlePackReview::FItem> Items = Review.Items();
	TestEqual(TEXT("three items: top, bottom, footwear"), Items.Num(), 3);
	TestTrue(TEXT("the top is the corset with its lace and the sleeves"), Items.Num() > 0 && Items[0].Slot == TEXT("top") && Items[0].Main == Corset
		&& Items[0].With.Contains(Lace) && Items[0].With.Contains(Arms));
	TestTrue(TEXT("the row says so"), Review.RowText(Corset).Contains(TEXT("with edging (LaceCorset)")));

	// The creator changes rows: the sleeves unticked, the corset alone with its lace.
	FMantlePackReview Changed = Review;
	Changed.Pieces[Arms].bTicked = false;
	Items = Changed.Items();
	TestTrue(TEXT("unticked sleeves are left out"), Items.Num() > 0 && Items[0].With.Num() == 1 && Items[0].With[0] == Lace);
	// The nightwear top brought in as a top of its own; one item each.
	Changed.Move(Night, TEXT("top"));
	Changed.Separate.Add(TEXT("top"));
	Items = Changed.Items();
	TestEqual(TEXT("two tops, one item each"), Items.FilterByPredicate([](const FMantlePackReview::FItem& Item) { return Item.Slot == TEXT("top"); }).Num(), 2);
	// The choker made part of the corset goes into its item.
	Changed.Move(Neck, FString(), Corset);
	Items = Changed.Items();
	TestTrue(TEXT("the choker goes into the corset"), Items.ContainsByPredicate([Corset, Neck](const FMantlePackReview::FItem& Item) { return Item.Main == Corset && Item.With.Contains(Neck); }));
	// The lace moved into the shoes' item goes there, no longer as edging.
	const int32 Shoes = PackFind(Changed, TEXT("SK_Shoes"));
	Changed.Move(Lace, FString(), Shoes);
	TestTrue(TEXT("the lace now goes with the shoes"), Changed.Pieces[Lace].PartOf == Shoes && !Changed.Pieces[Lace].bTrim && Changed.Pieces[Lace].Slot == TEXT("footwear"));
	// The underwear body picked instead.
	Changed.Body = Changed.Bodies.IndexOfByPredicate([](const FMantlePackBody& Body) { return Body.MeshIndex == 2; });
	TestEqual(TEXT("the underwear body is chosen"), Changed.BodyMesh(), 2);

	// Two tops on the chest are alternatives: one item each by default.
	TArray<FMantleSplitSource> TwoTops = {
		PackTestMesh(TEXT("SK_Girl"), TEXT("M_VG_Body"), { { TEXT("spine_03"), 900 } }, 170.0f),
		PackTestMesh(TEXT("SK_Shirt_A"), TEXT("M_A"), { { TEXT("spine_03"), 900 } }, 40.0f, 95.0f),
		PackTestMesh(TEXT("SK_Shirt_B"), TEXT("M_B"), { { TEXT("spine_03"), 800 }, { TEXT("upperarm_l"), 100 } }, 40.0f, 95.0f),
	};
	const FMantlePackReview Alternatives = FMantleCharacterSplit::Review(TwoTops, 0);
	TestFalse(TEXT("two tops on the chest are one item each"), Alternatives.IsOneItem(TEXT("top")));
	TestEqual(TEXT("so two items"), Alternatives.Items().Num(), 2);
	TestTrue(TEXT("the log lists every mesh"), Alternatives.Describe(TEXT("Girl")).Last().Contains(TEXT("0 dropped")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
