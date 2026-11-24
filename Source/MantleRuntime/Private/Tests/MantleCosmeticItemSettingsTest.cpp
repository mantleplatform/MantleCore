// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCosmeticItem.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// #65, #136, #389: the item batch reader takes the creator's item settings (tuckable, tallBoots, hairRule, pairId),
// plain top-level fields the API leaves out when unset, and gives the contract's defaults for anything absent or unreadable.

// Named, not anonymous: unity builds.
namespace MantleCosmeticItemSettingsTestHelpers
{
	/** A one-item batch (the shape the colour-region test uses); Extra is more top-level fields, e.g. `"tuckable":false,`. */
	FString Batch(const TCHAR* Slot, const TCHAR* Kind, const FString& Extra)
	{
		const FString Hash = TEXT("blake3:") + FString::ChrN(64, TEXT('a'));
		return FString::Printf(TEXT(R"({"items":[{"id":"itm_u00000000000000000000000000000001","slot":"%s","kind":"%s","version":1,%s)"
			R"("mesh":{"url":"https://example.test/mesh","cookedHash":"%s"},)"
			R"("materials":[{"role":"other","albedo":{"url":"https://example.test/albedo","cookedHash":"%s"}}]}]})"),
			Slot, Kind, *Extra, *Hash, *Hash);
	}

	/** Reads one item; false (with the reason in Info) when the batch is refused. */
	bool Read(const FString& Json, FMantleCosmeticItem& OutItem, FString& OutInfo)
	{
		TArray<FMantleCosmeticItem> Items;
		TArray<FString> Missing;
		FString WhyNot;
		if (!MantleCosmetic::ParseItemBatch(Json, Items, Missing, WhyNot) || Items.Num() != 1)
		{
			OutInfo = WhyNot;
			return false;
		}
		OutItem = Items[0];
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCosmeticItemSettingsTest,
	"Mantle.Cosmetic.ItemSettings",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCosmeticItemSettingsTest::RunTest(const FString& Parameters)
{
	using namespace MantleCosmeticItemSettingsTestHelpers;
	FMantleCosmeticItem Item;
	FString Info;

	// A fresh item is the contract's defaults.
	TestTrue(TEXT("a new item is tuckable"), Item.bTuckable);
	TestFalse(TEXT("a new item has no tall boots"), Item.bTallBoots);
	TestEqual(TEXT("a new item's hair rule is underIt"), Item.HairRule, FString(TEXT("underIt")));
	TestTrue(TEXT("a new item has no pair"), Item.PairId.IsEmpty());

	// Unset: the defaults.
	if (!TestTrue(TEXT("a top with no settings reads"), Read(Batch(TEXT("top"), TEXT("skinned"), FString()), Item, Info)))
	{
		AddInfo(Info);
		return false;
	}
	TestTrue(TEXT("unset: tuckable"), Item.bTuckable);
	TestFalse(TEXT("unset: no tall boots"), Item.bTallBoots);
	TestEqual(TEXT("unset: underIt"), Item.HairRule, FString(TEXT("underIt")));
	TestTrue(TEXT("unset: no pair"), Item.PairId.IsEmpty());

	// Set.
	if (!TestTrue(TEXT("a top that is not tuckable reads"), Read(Batch(TEXT("top"), TEXT("skinned"), TEXT(R"("tuckable":false,)")), Item, Info)))
	{
		AddInfo(Info);
		return false;
	}
	TestFalse(TEXT("tuckable false is read"), Item.bTuckable);
	TestFalse(TEXT("and tall boots stay off"), Item.bTallBoots);

	if (!TestTrue(TEXT("boots read"), Read(Batch(TEXT("footwear"), TEXT("skinned"), TEXT(R"("tallBoots":true,)")), Item, Info)))
	{
		AddInfo(Info);
		return false;
	}
	TestTrue(TEXT("tallBoots true is read"), Item.bTallBoots);
	TestTrue(TEXT("and the item stays tuckable"), Item.bTuckable);

	if (!TestTrue(TEXT("a hat reads"), Read(Batch(TEXT("acc_hat"), TEXT("rigid"), TEXT(R"("hairRule":"hideAll",)")), Item, Info)))
	{
		AddInfo(Info);
		return false;
	}
	TestEqual(TEXT("hairRule hideAll is read"), Item.HairRule, FString(TEXT("hideAll")));
	if (TestTrue(TEXT("leaveAlone reads"), Read(Batch(TEXT("outerwear"), TEXT("skinned"), TEXT(R"("hairRule":"leaveAlone",)")), Item, Info)))
	{
		TestEqual(TEXT("hairRule leaveAlone is read"), Item.HairRule, FString(TEXT("leaveAlone")));
	}

	if (!TestTrue(TEXT("an earring reads"), Read(Batch(TEXT("acc_ear_l"), TEXT("rigid"), TEXT(R"("pairId":"studs-1",)")), Item, Info)))
	{
		AddInfo(Info);
		return false;
	}
	TestEqual(TEXT("pairId is read"), Item.PairId, FString(TEXT("studs-1")));

	// Unreadable settings leave the defaults and do not fail the batch: they only tune the layers and the hair.
	if (TestTrue(TEXT("a wrong-typed tuckable still reads"), Read(Batch(TEXT("top"), TEXT("skinned"), TEXT(R"("tuckable":"no","tallBoots":1,)")), Item, Info)))
	{
		TestTrue(TEXT("a text tuckable leaves the default"), Item.bTuckable);
		TestFalse(TEXT("a number tallBoots leaves the default"), Item.bTallBoots);
	}
	if (TestTrue(TEXT("null settings still read"), Read(Batch(TEXT("top"), TEXT("skinned"), TEXT(R"("tuckable":null,"hairRule":null,"pairId":null,)")), Item, Info)))
	{
		TestTrue(TEXT("null tuckable is the default"), Item.bTuckable);
		TestEqual(TEXT("null hairRule is the default"), Item.HairRule, FString(TEXT("underIt")));
		TestTrue(TEXT("null pairId is no pair"), Item.PairId.IsEmpty());
	}
	if (TestTrue(TEXT("an unknown hair rule still reads"), Read(Batch(TEXT("acc_hat"), TEXT("rigid"), TEXT(R"("hairRule":"hideSome",)")), Item, Info)))
	{
		TestEqual(TEXT("an unknown hairRule is the default"), Item.HairRule, FString(TEXT("underIt")));
	}
	// The rules are spelled exactly: a different case is not one of them (FString's == ignores case; the reader does not).
	if (TestTrue(TEXT("a mis-cased hair rule still reads"), Read(Batch(TEXT("acc_hat"), TEXT("rigid"), TEXT(R"("hairRule":"HIDEALL",)")), Item, Info)))
	{
		TestEqual(TEXT("a mis-cased hairRule is the default"), Item.HairRule, FString(TEXT("underIt")));
	}
	return true;
}

#endif
