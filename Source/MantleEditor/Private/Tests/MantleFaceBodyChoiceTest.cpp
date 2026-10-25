// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAvatarStripFixer.h"
#include "MantleCosmeticPanel.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// With the face kept, the creator picks the body it goes on; Convert never guesses it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleFaceBodyChoiceTest,
	"Mantle.Editor.MetaHumanHeadOnly.BodyChoice",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleFaceBodyChoiceTest::RunTest(const FString& Parameters)
{
	auto Line = [](const FName Kind, const bool bEnabled)
	{
		FMantleConvertFix Fix;
		Fix.FixerId = MantleAvatarFixerIds::Strip;
		Fix.PartIndex = 0;
		Fix.Kind = Kind;
		Fix.bEnabled = bEnabled;
		return Fix;
	};
	const FName Face = FMantleAvatarStripFixer::KindFaceOnly;
	const FName Feminine = FMantleAvatarStripFixer::KindFeminineBody;
	const FName Masculine = FMantleAvatarStripFixer::KindMasculineBody;

	TestTrue(TEXT("face kept, no body ticked: Convert waits"),
		!FMantleAvatarStripFixer::MissingBodyChoice({ Line(Face, true), Line(Masculine, false), Line(Feminine, false) }, 0).IsEmpty());
	TestTrue(TEXT("face kept, both ticked: Convert waits"),
		!FMantleAvatarStripFixer::MissingBodyChoice({ Line(Face, true), Line(Masculine, true), Line(Feminine, true) }, 0).IsEmpty());
	TestTrue(TEXT("face kept, feminine ticked: Convert runs"),
		FMantleAvatarStripFixer::MissingBodyChoice({ Line(Face, true), Line(Masculine, false), Line(Feminine, true) }, 0).IsEmpty());
	TestTrue(TEXT("face kept, masculine ticked: Convert runs"),
		FMantleAvatarStripFixer::MissingBodyChoice({ Line(Face, true), Line(Masculine, true), Line(Feminine, false) }, 0).IsEmpty());
	TestTrue(TEXT("whole body converted: no body to pick"),
		FMantleAvatarStripFixer::MissingBodyChoice({ Line(Face, false), Line(Masculine, false), Line(Feminine, false) }, 0).IsEmpty());
	TestTrue(TEXT("another part's lines don't count"),
		FMantleAvatarStripFixer::MissingBodyChoice({ Line(Face, true), Line(Feminine, false) }, 1).IsEmpty());
	return true;
}

// Chunk 5: one body row for every case, and one bare-skin rule (ticked only when a bare body is clearly found).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleBodyRowTest,
	"Mantle.Editor.MetaHumanHeadOnly.BodyRow",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleBodyRowTest::RunTest(const FString& Parameters)
{
	using FBodyRow = FMantleAvatarStripFixer::FBodyRow;
	auto Find = [](const TArray<FMantleConvertFix>& Fixes, const FName Kind) -> const FMantleConvertFix*
	{
		return Fixes.FindByPredicate([Kind](const FMantleConvertFix& Fix) { return Fix.Kind == Kind; });
	};
	const FName Face = FMantleAvatarStripFixer::KindFaceOnly;
	const FName Feminine = FMantleAvatarStripFixer::KindFeminineBody;
	const FName Masculine = FMantleAvatarStripFixer::KindMasculineBody;
	const FName Bare = FMantleAvatarStripFixer::KindBareSkin;

	// A covered body: the face goes on a MetaHuman body; Women's or Men's is the creator's pick; no bare skin to confirm.
	{
		TArray<FMantleConvertFix> Fixes;
		FBodyRow Row;
		Row.bFaceOnBody = true;
		Row.Why = TEXT("the body is covered by clothing");
		FMantleAvatarStripFixer::AddBodyRow(Fixes, 0, Row);
		TestEqual(TEXT("four lines: face, Men's, Women's, bare skin"), Fixes.Num(), 4);
		TestTrue(TEXT("every line is the body row's"), Fixes.ContainsByPredicate([](const FMantleConvertFix& Fix) { return !FMantleAvatarStripFixer::IsBodyRowLine(Fix); }) == false);
		TestTrue(TEXT("face pre-picked"), Find(Fixes, Face) && Find(Fixes, Face)->bEnabled);
		TestEqual(TEXT("the reason rides on the face line"), Find(Fixes, Face) ? Find(Fixes, Face)->Problem : FString(), Row.Why);
		TestTrue(TEXT("no body guessed"), Find(Fixes, Feminine) && !Find(Fixes, Feminine)->bEnabled && Find(Fixes, Masculine) && !Find(Fixes, Masculine)->bEnabled);
		TestTrue(TEXT("bare skin: not found, so unticked"), Find(Fixes, Bare) && !Find(Fixes, Bare)->bEnabled && Find(Fixes, Bare)->bNeedsConfirm);
		TestFalse(TEXT("with the face on a MetaHuman body the bare-skin line waits for nothing"), FMantleAvatarStripFixer::IsUnconfirmed(Fixes, *Find(Fixes, Bare)));
		TestFalse(TEXT("Convert waits for Women's or Men's"), FMantleConvertSection::CanConvert(Fixes));
		FMantleConvertSection::PickBody(Fixes, 0, true, true);
		TestTrue(TEXT("Women's picked"), Find(Fixes, Feminine)->bEnabled && !Find(Fixes, Masculine)->bEnabled);
		TestTrue(TEXT("Convert runs"), FMantleConvertSection::CanConvert(Fixes));
		FMantleConvertSection::PickBody(Fixes, 0, true, false);
		TestTrue(TEXT("Men's picked instead"), !Find(Fixes, Feminine)->bEnabled && Find(Fixes, Masculine)->bEnabled);
		FMantleConvertSection::PickBody(Fixes, 0, false);
		TestFalse(TEXT("body kept: face line off"), Find(Fixes, Face)->bEnabled);
		TestTrue(TEXT("body kept, bare skin unticked: it must be confirmed"), FMantleAvatarStripFixer::IsUnconfirmed(Fixes, *Find(Fixes, Bare)));
		TestFalse(TEXT("so Convert waits"), FMantleConvertSection::CanConvert(Fixes));
	}
	// A bare body: kept, and the bare-skin line starts ticked.
	{
		TArray<FMantleConvertFix> Fixes;
		FBodyRow Row;
		Row.bBareFound = true;
		Row.Why = TEXT("a bare body was found");
		FMantleAvatarStripFixer::AddBodyRow(Fixes, 0, Row);
		TestFalse(TEXT("body kept"), Find(Fixes, Face)->bEnabled);
		TestTrue(TEXT("bare skin ticked"), Find(Fixes, Bare)->bEnabled);
		TestTrue(TEXT("Convert runs"), FMantleConvertSection::CanConvert(Fixes));
		FMantleConvertSection::KeptPartsChanged(Fixes, 1);
		TestTrue(TEXT("another part's change leaves it"), Find(Fixes, Bare)->bEnabled);
		FMantleConvertSection::KeptPartsChanged(Fixes, 0);
		TestFalse(TEXT("a changed kept part hands the tick back to the creator"), Find(Fixes, Bare)->bEnabled);
		TestFalse(TEXT("so Convert waits"), FMantleConvertSection::CanConvert(Fixes));
	}
	// A body that can't be kept (MetaHuman Creator can't fit it): the bare-skin line can't be ticked.
	{
		TArray<FMantleConvertFix> Fixes;
		FBodyRow Row;
		Row.bFaceOnBody = true;
		Row.bCanKeep = false;
		FMantleAvatarStripFixer::AddBodyRow(Fixes, 2, Row);
		TestTrue(TEXT("keeping is greyed"), Find(Fixes, Bare) && !Find(Fixes, Bare)->bCanFix && Find(Fixes, Bare)->PartIndex == 2);
	}

	// The one bare-skin rule on plans.
	auto PlanSlot = [](const int32 Index, const bool bSkin, const double MinZ, const double MaxZ)
	{
		FMantleStripSlot Slot;
		Slot.Index = Index;
		Slot.SlotName = FName(*FString::Printf(TEXT("Slot%d"), Index));
		Slot.bSkin = bSkin;
		Slot.Triangles = 100;
		Slot.Bounds = FBox3d(FVector(-20.0, -10.0, MinZ), FVector(20.0, 10.0, MaxZ));
		return Slot;
	};
	FString Why;
	FMantleStripPlan Skin;
	Skin.Slots = { PlanSlot(0, true, 0.0, 170.0), PlanSlot(1, false, 150.0, 175.0) };
	TestTrue(TEXT("skin head to feet, the hair stripped: bare body found"), FMantleAvatarStripFixer::BareBodyFound(Skin, false, Why));
	TestFalse(TEXT("nothing stripped and a slot that isn't skin: not clearly bare"), FMantleAvatarStripFixer::BareBodyFound(Skin, true, Why));
	TestTrue(TEXT("it says which slot"), Why.Contains(TEXT("Slot1")));
	FMantleStripPlan Covered;
	Covered.Slots = { PlanSlot(0, true, 150.0, 170.0), PlanSlot(1, false, 0.0, 150.0) };
	TestFalse(TEXT("skin on the head alone: covered, not bare"), FMantleAvatarStripFixer::BareBodyFound(Covered, false, Why));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
