// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAvatarStripFixer.h"
#include "MantleOutfitSections.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// Which material sections read as skin, by the slot and material names the Paragon heroes really carry. Names only.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleOutfitSectionsTestPrivate
{
	EMantleOutfitPiece OutfitTestClassify(const TCHAR* Slot, const TCHAR* Material, const TCHAR* BaseMaterial = TEXT(""))
	{
		FMantleOutfitSection Section;
		Section.SlotName = Slot;
		Section.MaterialName = Material;
		Section.BaseMaterialName = BaseMaterial;
		return FMantleOutfitSections::Classify(Section);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleOutfitSectionsSlotBeatsMaterialTest,
	"Mantle.Editor.OutfitSections.SlotNameBeatsMaterialName",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleOutfitSectionsSlotBeatsMaterialTest::RunTest(const FString& Parameters)
{
	using namespace MantleOutfitSectionsTestPrivate;
	auto Expect = [this](const TCHAR* Slot, const TCHAR* Material, const EMantleOutfitPiece Want, const bool bWantSkin)
	{
		const EMantleOutfitPiece Got = OutfitTestClassify(Slot, Material);
		TestEqual(FString::Printf(TEXT("%s / %s classifies as %s"), Slot, Material, FMantleOutfitSections::PieceName(Want)),
			FString(FMantleOutfitSections::PieceName(Got)), FString(FMantleOutfitSections::PieceName(Want)));
		TestEqual(FString::Printf(TEXT("%s / %s reads as bare skin"), Slot, Material), FMantleAvatarStrip::LooksLikeSkin(Slot, Material, FString()), bWantSkin);
	};

	// Twinblast: the head slot's material shares its sheet with the shirt. The slot's "head" wins.
	Expect(TEXT("M00_Head"), TEXT("M_TwinBlast_Head_Shirt"), EMantleOutfitPiece::Skin, true);
	// Sparrow: the bow's material shares the head's sheet. A prop slot is never skin.
	Expect(TEXT("M04_BowAndArrow"), TEXT("M_Sparrow_Head_Arrow"), EMantleOutfitPiece::Unknown, false);
	// Props by slot or by material.
	Expect(TEXT("M02_Pistol"), TEXT("M_TwinBlast_Pistol"), EMantleOutfitPiece::Unknown, false);
	Expect(TEXT("M05_Weapon"), TEXT("M_Kwang_Weapon"), EMantleOutfitPiece::Unknown, false);
	Expect(TEXT("Material3"), TEXT("M_Head_Sword"), EMantleOutfitPiece::Unknown, false);

	// Ordinary skin slots, by the slot's name or (when the slot says nothing) the material's.
	Expect(TEXT("M00_Body"), TEXT("M_Murdock_Body"), EMantleOutfitPiece::Skin, true);
	Expect(TEXT("M01_Face"), TEXT("M_Murdock_Head"), EMantleOutfitPiece::Skin, true);
	Expect(TEXT("M03_head"), TEXT("M_Belica_New_Head"), EMantleOutfitPiece::Skin, true);
	Expect(TEXT("M00_upperbody"), TEXT("M_Belica_UpperBody"), EMantleOutfitPiece::Skin, true);
	Expect(TEXT("M04_Lower"), TEXT("M_Kwang_LowerBody"), EMantleOutfitPiece::Skin, true);
	Expect(TEXT("M14_arm_finger_skin"), TEXT("M_Kwang_WaistArmsFlesh"), EMantleOutfitPiece::Skin, true);
	TestEqual(TEXT("a slot that says nothing reads as skin by its base material"),
		FString(FMantleOutfitSections::PieceName(OutfitTestClassify(TEXT("Material0"), TEXT("MI_Character"), TEXT("M_Skin_Unified_Baked")))),
		FString(TEXT("skin")));

	// Not skin, as before: clothing, eyes, accessories, and slots whose names say nothing about skin.
	Expect(TEXT("M01_cloth"), TEXT("M_TwinBlast_Coat"), EMantleOutfitPiece::Top, false);
	Expect(TEXT("M_DG_bodyShape1_Shirt"), TEXT("MI_Shirt"), EMantleOutfitPiece::Top, false);
	Expect(TEXT("Material0"), TEXT("MI_Suit_Pants"), EMantleOutfitPiece::Bottom, false);
	Expect(TEXT("HeadWear"), TEXT("MI_Head"), EMantleOutfitPiece::Unknown, false);
	Expect(TEXT("M02_Arms"), TEXT("M_Murdock_Arms"), EMantleOutfitPiece::Unknown, false);
	Expect(TEXT("M12_LegsandHands"), TEXT("M_Legs_Hands"), EMantleOutfitPiece::Unknown, false);
	Expect(TEXT("M06_Eyes"), TEXT("MI_Murdock_EyeRefractive"), EMantleOutfitPiece::Unknown, false);
	// Props, for pointing a creator at the slots to keep: by either name, never by a body word alone.
	auto IsProp = [](const TCHAR* Slot, const TCHAR* Material)
	{
		FMantleOutfitSection Section;
		Section.SlotName = Slot;
		Section.MaterialName = Material;
		return FMantleOutfitSections::IsProp(Section);
	};
	TestTrue(TEXT("Sparrow's bow is a prop"), IsProp(TEXT("M04_BowAndArrow"), TEXT("M_Sparrow_Head_Arrow")));
	TestTrue(TEXT("Sparrow's bow string is a prop by its material"), IsProp(TEXT("M10_String"), TEXT("M_ArrowString3")));
	TestTrue(TEXT("Terra's shield is a prop"), IsProp(TEXT("M08_Justice_Shield"), TEXT("MI_Terra_Shield")));
	TestFalse(TEXT("legs and hands are not a prop"), IsProp(TEXT("M12_LegsandHands"), TEXT("M_Legs_Hands")));
	TestFalse(TEXT("an elbow is not a bow"), IsProp(TEXT("Elbow_Pad"), TEXT("MI_Elbow")));

	// The slot reads as skin, but eye and teeth words still keep it off the bare body.
	TestFalse(TEXT("a face slot whose material is the eyes' is not bare skin"), FMantleAvatarStrip::LooksLikeSkin(TEXT("Face_Eyes"), TEXT("MI_Eye"), FString()));
	return true;
}

#endif
