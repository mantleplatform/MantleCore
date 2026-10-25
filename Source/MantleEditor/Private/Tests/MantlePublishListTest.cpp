// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCosmeticPanel.h"
#include "MantleAvatarConverter.h"
#include "MantleCharacterSplit.h"

#include "Engine/SkeletalMesh.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// Chunk 6, "What to publish": the one list after Convert (body, clothing, hair), ticks, and the order the ticked items open in;
// a MetaHuman Blueprint's clothing and hair kept on Convert.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePublishListTest,
	"Mantle.Editor.CosmeticPanel.PublishList",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePublishListTest::RunTest(const FString& Parameters)
{
	auto Mesh = [](const TCHAR* Name) { return NewObject<USkeletalMesh>(GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), USkeletalMesh::StaticClass(), Name)); };
	using EKind = FMantlePublishRow::EKind;
	const TArray<FString> HairSlots = { TEXT("hair"), TEXT("eyebrows") };

	// A third-party character converted: the body, two clothing items (one can't be an Outfit), the pack's hair.
	FMantleConvertContext Context;
	FMantleConvertPart& Body = Context.Parts.AddDefaulted_GetRef();
	Body.Slot = TEXT("avatar");
	Body.Name = TEXT("Vampire Girl");
	Body.Mesh = Mesh(TEXT("SK_Body"));
	FMantleConvertGarment& Corset = Context.Garments.AddDefaulted_GetRef();
	Corset.Slot = TEXT("top");
	Corset.Name = TEXT("Corset with Lace Corset");
	Corset.Mesh = Mesh(TEXT("SK_Corset"));
	FMantleConvertGarment& Shoes = Context.Garments.AddDefaulted_GetRef();
	Shoes.Slot = TEXT("footwear");
	Shoes.Name = TEXT("Shoes");
	Shoes.Mesh = Mesh(TEXT("SK_Shoes"));
	Shoes.Why = TEXT("high heels: no Outfit yet (mantle#1051)");

	FMantlePackReview Review;
	for (const TCHAR* Name : { TEXT("SK_Hair"), TEXT("SK_HairBangs"), TEXT("SK_LaceNeck"), TEXT("SK_Brows") })
	{
		Review.MeshNames.Add(Name);
		Review.Meshes.Add(Mesh(Name));
	}
	auto Piece = [&Review](const int32 MeshIndex, const TCHAR* Label, const TCHAR* Slot, const bool bTicked, const int32 PartOf = INDEX_NONE)
	{
		FMantlePackPiece& Each = Review.Pieces.AddDefaulted_GetRef();
		Each.MeshIndex = MeshIndex;
		Each.Label = Label;
		Each.Slot = Slot;
		Each.bTicked = bTicked;
		Each.PartOf = PartOf;
	};
	Piece(0, TEXT("Hair"), TEXT("hair"), true);
	Piece(1, TEXT("Hair Bangs"), TEXT("hair"), true, 0);
	Piece(2, TEXT("Lace Neck"), TEXT("accessory"), true);
	Piece(3, TEXT("Brows"), TEXT("eyebrows"), false);

	TArray<FMantlePublishRow> Rows = FMantlePublishList::Make(Context, &Review, HairSlots);
	if (!TestEqual(TEXT("body, the ticked hair, two clothing rows"), Rows.Num(), 4))
	{
		return false;
	}
	TestTrue(TEXT("body first"), Rows[0].Kind == EKind::Part && Rows[0].Index == 0 && Rows[0].bTicked && Rows[0].Why.IsEmpty());
	TestEqual(TEXT("body's word"), FMantlePublishList::SlotWord(Rows[0].Slot), FString(TEXT("Body")));
	TestTrue(TEXT("hair from the pack's rows, its own row only (bangs go with it, unticked brows and the accessory aren't listed)"),
		Rows[1].Kind == EKind::PackHair && Rows[1].Index == 0 && Rows[1].Name == TEXT("Hair") && Rows[1].bTicked);
	TestTrue(TEXT("corset with its edging, ticked"), Rows[2].Kind == EKind::Garment && Rows[2].Index == 0 && Rows[2].Name == TEXT("Corset with Lace Corset") && Rows[2].bTicked);
	TestTrue(TEXT("high heels greyed with why, never ticked"), Rows[3].Kind == EKind::Garment && !Rows[3].bTicked && Rows[3].Why == Shoes.Why);
	TestEqual(TEXT("slot word"), FMantlePublishList::SlotWord(TEXT("facial_hair")), FString(TEXT("Facial hair")));

	// The walk: ticked, not greyed, not uploaded, in list order; untick means never opened.
	TestEqual(TEXT("first: the body"), FMantlePublishList::Next(Rows, INDEX_NONE), 0);
	TestEqual(TEXT("then the hair"), FMantlePublishList::Next(Rows, 0), 1);
	TestEqual(TEXT("then the corset"), FMantlePublishList::Next(Rows, 1), 2);
	TestEqual(TEXT("the greyed shoes never"), FMantlePublishList::Next(Rows, 2), INDEX_NONE);
	Rows[1].bTicked = false;
	TestEqual(TEXT("unticked hair skipped"), FMantlePublishList::Next(Rows, 0), 2);
	Rows[0].bUploaded = true;
	TestEqual(TEXT("an uploaded row isn't opened again"), FMantlePublishList::Next(Rows, INDEX_NONE), 2);
	Rows[2].bTicked = false;
	TestEqual(TEXT("nothing ticked left"), FMantlePublishList::Next(Rows, INDEX_NONE), INDEX_NONE);

	// The Outfits made: a piece whose Outfit failed (high heels are found then) is greyed and unticked; a made one stays.
	{
		Context.Garments[0].Outfit = TEXT("/Game/OA_Corset");
		FMantleConvertGarment& Boots = Context.Garments.AddDefaulted_GetRef();
		Boots.Slot = TEXT("footwear");
		Boots.Name = TEXT("Boots");
		Boots.Mesh = Mesh(TEXT("SK_Boots"));
		TArray<FMantlePublishRow> Synced = FMantlePublishList::Make(Context, &Review, HairSlots);
		TestTrue(TEXT("boots ticked before the Outfits are made"), Synced.Last().bTicked && Synced.Last().Why.IsEmpty());
		Context.Garments.Last().Error = TEXT("SK_Boots is a high heel");
		FMantlePublishList::Sync(Context, Synced);
		TestTrue(TEXT("failed Outfit: greyed and unticked"), !Synced.Last().bTicked && Synced.Last().Why.Contains(TEXT("high heel")));
		TestTrue(TEXT("made Outfit: still ticked"), Synced[2].bTicked && Synced[2].Why.IsEmpty());
		TestEqual(TEXT("the walk never opens the failed piece"), FMantlePublishList::Next(Synced, 2), INDEX_NONE);
	}

	// A part Convert didn't make is greyed; a part left out starts unticked.
	{
		FMantleConvertContext Empty;
		Empty.Parts.AddDefaulted_GetRef().Slot = TEXT("avatar");
		FMantleConvertPart& Cloth = Empty.Parts.AddDefaulted_GetRef();
		Cloth.Slot = TEXT("top");
		Cloth.Mesh = Mesh(TEXT("SK_Cape"));
		Cloth.bIncluded = false;
		const TArray<FMantlePublishRow> Made = FMantlePublishList::Make(Empty, nullptr, HairSlots);
		TestTrue(TEXT("no mesh: greyed"), Made.Num() == 2 && !Made[0].Why.IsEmpty() && !Made[0].bTicked);
		TestTrue(TEXT("left out: unticked, not greyed"), Made.Num() == 2 && Made[1].Why.IsEmpty() && !Made[1].bTicked);
	}

	// A MetaHuman Blueprint on Convert: its clothing and hair kept as they are, listed after the body.
	{
		FMantleAssemblyScan Scan;
		Scan.Name = TEXT("Ada");
		USkeletalMesh* Suit = Mesh(TEXT("SKM_Suit"));
		Scan.Garments.Add({ Suit, TEXT("top"), FString() });
		Scan.Garments.Add({ Suit, TEXT("bottom"), FString() });
		Scan.Garments.Add({ Mesh(TEXT("SKM_Cape")), TEXT("top"), TEXT("CA_Cape") });
		Scan.Hair.Add({ Mesh(TEXT("SKM_Hair")), TEXT("hair"), FString() });
		FMantleConvertContext Assembled;
		Assembled.Parts.AddDefaulted_GetRef().Slot = TEXT("avatar");
		Assembled.Parts[0].Mesh = Mesh(TEXT("SKM_Built"));
		FMantleAvatarConverter::AddKeptPieces(Scan, Assembled.Kept);
		if (TestEqual(TEXT("suit twice (top, bottom), cape, hair"), Assembled.Kept.Num(), 4))
		{
			TestTrue(TEXT("a one-piece outfit names its slot"), Assembled.Kept[0].Name.EndsWith(TEXT(" top")) && Assembled.Kept[1].Name.EndsWith(TEXT(" bottom")));
			TestEqual(TEXT("clothing on Epic's MetaHuman skeleton"), Assembled.Kept[0].SkeletonId, FString(TEXT("metahuman")));
			TestFalse(TEXT("cloth-simulated cape left out"), Assembled.Kept[2].bIncluded);
			TestTrue(TEXT("hair kept"), Assembled.Kept[3].Slot == TEXT("hair") && Assembled.Kept[3].bIncluded);
		}
		const TArray<FMantlePublishRow> Made = FMantlePublishList::Make(Assembled, nullptr, HairSlots);
		if (TestEqual(TEXT("body and four kept rows"), Made.Num(), 5))
		{
			TestTrue(TEXT("body first"), Made[0].Kind == EKind::Part);
			TestTrue(TEXT("kept clothing ticked"), Made[1].Kind == EKind::Kept && Made[1].bTicked);
			TestTrue(TEXT("kept cape unticked"), Made[3].Kind == EKind::Kept && !Made[3].bTicked && Made[3].Why.IsEmpty());
			TestTrue(TEXT("kept hair ticked"), Made[4].Kind == EKind::Kept && Made[4].Slot == TEXT("hair") && Made[4].bTicked);
		}
	}
	return true;
}

#endif
