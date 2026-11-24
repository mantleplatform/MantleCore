// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAvatarContract.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// #65, #136, #389: the plugin's contract copy reads the new clothing layers, accessory slots, item settings, ring
// fingers and the small accessories' triangle caps. On the plugin's own contract, no editor assets.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleClothingLayersContractTest,
	"Mantle.Editor.ClothingLayers.Contract",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleClothingLayersContractTest::RunTest(const FString& Parameters)
{
	FString ContractError;
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get(&ContractError);
	if (!TestNotNull(FString::Printf(TEXT("the contract loads (%s)"), *ContractError), Contract)) { return false; }

	// Clothing slots: the layer order plus gloves; hair and nails are not clothing.
	for (const TCHAR* Slot : { TEXT("socks"), TEXT("legwear"), TEXT("footwear"), TEXT("bottom"), TEXT("top"), TEXT("outerwear"), TEXT("gloves") })
	{
		TestTrue(FString::Printf(TEXT("%s is a clothing slot"), Slot), Contract->IsClothingSlot(Slot));
	}
	for (const TCHAR* Slot : { TEXT("hair"), TEXT("eyebrows"), TEXT("nails"), TEXT("acc_hat"), TEXT("") })
	{
		TestFalse(FString::Printf(TEXT("%s is not a clothing slot"), Slot), Contract->IsClothingSlot(Slot));
	}
	TestTrue(TEXT("socks go under the top"), Contract->ClothingRank(TEXT("socks")) < Contract->ClothingRank(TEXT("top")));
	TestTrue(TEXT("outerwear goes over the top"), Contract->ClothingRank(TEXT("outerwear")) > Contract->ClothingRank(TEXT("top")));
	TestEqual(TEXT("hair has no clothing rank"), Contract->ClothingRank(TEXT("hair")), static_cast<int32>(INDEX_NONE));

	// The new accessory slots, their sockets and caps.
	for (const TCHAR* Slot : { TEXT("acc_eyes"), TEXT("acc_ear_l"), TEXT("acc_ear_r"), TEXT("acc_nose"), TEXT("acc_wrist_l"), TEXT("acc_wrist_r"), TEXT("acc_ring") })
	{
		const FMantleContractSlot* Entry = Contract->FindSlot(Slot);
		if (!TestNotNull(FString::Printf(TEXT("%s is in the slot table"), Slot), Entry)) { continue; }
		TestEqual(FString::Printf(TEXT("%s is an accessory"), Slot), Entry->ItemClass, FString(TEXT("rigid_accessory")));
		TestFalse(FString::Printf(TEXT("%s has a socket bone the editor can place it on"), Slot), Entry->SocketBone.IsEmpty());
		TestNotNull(FString::Printf(TEXT("%s has size limits"), Slot), Contract->AccessoryBounds.Find(Slot));
		TestTrue(FString::Printf(TEXT("%s has its own, smaller triangle cap"), Slot),
			Contract->AccessoryMaxTriangles(Slot) > 0 && Contract->AccessoryMaxTriangles(Slot) < Contract->RigidAccessory.MaxTriangles);
		TestEqual(FString::Printf(TEXT("%s reduces to its own cap"), Slot), Contract->AccessoryClassFor(Slot).MaxTriangles, Contract->AccessoryMaxTriangles(Slot));
	}
	TestEqual(TEXT("a hat keeps the shared accessory cap"), Contract->AccessoryMaxTriangles(TEXT("acc_hat")), Contract->RigidAccessory.MaxTriangles);

	// A ring: ten fingers; the editor uses the left ring finger.
	if (const FMantleContractSlot* Ring = Contract->FindSlot(TEXT("acc_ring")))
	{
		TestEqual(TEXT("a ring can go on ten fingers"), Ring->FingerSockets.Num(), 10);
		TestEqual(TEXT("the editor places a ring on the left ring finger"), Ring->Socket, FString(TEXT("acc_ring_ring_l")));
		TestEqual(TEXT("which hangs from ring_01_l"), Ring->SocketBone, FString(TEXT("ring_01_l")));
	}
	const TArray<FString> Sockets = Contract->AccessorySockets();
	TestTrue(TEXT("an avatar may place every finger socket"), Sockets.Contains(TEXT("acc_ring_thumb_r")) && Sockets.Contains(TEXT("acc_ring_pinky_l")));
	TestEqual(TEXT("a finger socket's bone"), Contract->AccessorySocketBone(TEXT("acc_ring_index_r")), FString(TEXT("index_01_r")));
	TestEqual(TEXT("a slot socket's bone"), Contract->AccessorySocketBone(TEXT("acc_neck")), FString(TEXT("neck_01")));
	TestTrue(TEXT("an unknown socket has no bone"), Contract->AccessorySocketBone(TEXT("acc_tail")).IsEmpty());

	// Item settings, each on its own slots.
	const FMantleContractItemSetting* Tuck = Contract->FindItemSetting(TEXT("tuckable"));
	const FMantleContractItemSetting* Boots = Contract->FindItemSetting(TEXT("tallBoots"));
	const FMantleContractItemSetting* Hair = Contract->FindItemSetting(TEXT("hairRule"));
	const FMantleContractItemSetting* Pair = Contract->FindItemSetting(TEXT("pairId"));
	if (TestNotNull(TEXT("tuckable"), Tuck)) { TestTrue(TEXT("tops tuck by default"), Tuck->Type == TEXT("bool") && Tuck->bDefault); }
	if (TestNotNull(TEXT("tallBoots"), Boots)) { TestTrue(TEXT("boots are not tall by default"), Boots->Type == TEXT("bool") && !Boots->bDefault); }
	if (TestNotNull(TEXT("hairRule"), Hair))
	{
		TestTrue(TEXT("hairRule values"), Hair->Values == TArray<FString>({ TEXT("underIt"), TEXT("hideAll"), TEXT("leaveAlone") }));
		TestEqual(TEXT("hairRule default"), Hair->DefaultValue, FString(TEXT("underIt")));
	}
	if (TestNotNull(TEXT("pairId"), Pair)) { TestEqual(TEXT("pairId is a string"), Pair->Type, FString(TEXT("string"))); }
	TestTrue(TEXT("a top may be tuckable"), Contract->SettingAllowed(TEXT("tuckable"), TEXT("top")));
	TestFalse(TEXT("outerwear may not be tuckable"), Contract->SettingAllowed(TEXT("tuckable"), TEXT("outerwear")));
	TestTrue(TEXT("a hood takes a hair rule"), Contract->SettingAllowed(TEXT("hairRule"), TEXT("outerwear")));
	TestTrue(TEXT("a hat takes a hair rule"), Contract->SettingAllowed(TEXT("hairRule"), TEXT("acc_hat")));
	TestFalse(TEXT("glasses take no hair rule"), Contract->SettingAllowed(TEXT("hairRule"), TEXT("acc_eyes")));
	TestTrue(TEXT("a right earring takes a pair id"), Contract->SettingAllowed(TEXT("pairId"), TEXT("acc_ear_r")));
	TestFalse(TEXT("an unknown setting is allowed nowhere"), Contract->SettingAllowed(TEXT("color"), TEXT("top")));
	return true;
}

#endif
