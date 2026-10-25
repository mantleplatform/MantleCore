// Copyright Joshua Eger. All Rights Reserved.
#include "MantleWeaponBuilderExport.h"
#include "MantleWeaponBuilderLogic.h"
#include "MantleWeaponDocument.h"
#include "MantleWeaponSource.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace MantleWeaponBuilderConsumableTestPrivate
{
	UMantleWeaponSource* MakeConsumable(EMantleWeaponSourceUseStyle Style)
	{
		UMantleWeaponSource* Out = NewObject<UMantleWeaponSource>();
		Out->DisplayName = TEXT("Health potion");
		Out->Hold = EMantleWeaponHold::Consumable;
		Out->Consumable.Style = Style;
		return Out;
	}

	TMap<FName, FTransform> GripPoint()
	{
		TMap<FName, FTransform> Points;
		Points.Add(FName(TEXT("Grip")), FTransform(FQuat::Identity, FVector(0.0, 0.0, -6.0)));
		return Points;
	}
}

// #1025: a consumable builds a use document (no fire), in both styles, and the document survives a write and a read.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleWeaponBuilderConsumableTest, "Mantle.Editor.WeaponBuilderConsumable.ExportsBothStyles", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleWeaponBuilderConsumableTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponBuilderConsumableTestPrivate;
	for (const EMantleWeaponSourceUseStyle Style : { EMantleWeaponSourceUseStyle::EatDrink, EMantleWeaponSourceUseStyle::Use })
	{
		UMantleWeaponSource* Input = MakeConsumable(Style);
		FMantleWeaponDesc Desc;
		TArray<FText> Messages;
		const bool bBuilt = FMantleWeaponBuilderExport::BuildDocument(*Input, GripPoint(), TEXT("FixtureMesh"), Desc, Messages);
		if (!TestTrue(TEXT("a consumable builds"), bBuilt)) { for (const FText& Message : Messages) AddError(Message.ToString()); continue; }
		TestTrue(TEXT("hold is consumable"), Desc.GetHold() == EMantleWeaponDocHold::Consumable);
		TestTrue(TEXT("delivery is use"), Desc.GetDelivery() == EMantleWeaponDocDelivery::Use);
		TestTrue(TEXT("it has the consumable block"), Desc.Consumable.bSet);
		TestEqual(TEXT("the heal default"), Desc.Consumable.Value.Heal.Value, 50.0);
		TestEqual(TEXT("one use"), Desc.Consumable.Value.Uses.Value, 1);
		TestFalse(TEXT("the use time is the clip's own"), Desc.Consumable.Value.UseSeconds.bSet);
		TestTrue(TEXT("it writes the grip"), Desc.Socket(EMantleWeaponDocSocket::Grip).bSet);

		const FString Json = FMantleWeaponDocument::Write(Desc);
		TestFalse(TEXT("a use document writes no fire"), Json.Contains(TEXT("\"fire\"")));
		TestTrue(TEXT("it names the style"), Json.Contains(Style == EMantleWeaponSourceUseStyle::EatDrink ? TEXT("\"eatDrink\"") : TEXT("\"use\"")));
		FMantleWeaponDesc Back;
		FString Error;
		if (TestTrue(TEXT("the document reads back"), FMantleWeaponDocument::Parse(Json, Back, Error)))
		{
			TestEqual(TEXT("and writes the same text"), FMantleWeaponDocument::Write(Back), Json);
		}
		else
		{
			AddError(Error);
		}
	}
	return true;
}

// A use time the creator sets is written; numbers outside the allowed ranges are refused with a message, not clamped.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleWeaponBuilderConsumableRangesTest, "Mantle.Editor.WeaponBuilderConsumable.RefusesBadNumbers", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleWeaponBuilderConsumableRangesTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponBuilderConsumableTestPrivate;
	UMantleWeaponSource* Input = MakeConsumable(EMantleWeaponSourceUseStyle::Use);
	Input->Consumable.bOverrideUseSeconds = true;
	Input->Consumable.UseSeconds = 2.0f;
	FMantleWeaponDesc Desc;
	TArray<FText> Messages;
	TestTrue(TEXT("a chosen use time builds"), FMantleWeaponBuilderExport::BuildDocument(*Input, GripPoint(), TEXT("FixtureMesh"), Desc, Messages));
	TestTrue(TEXT("and is written"), Desc.Consumable.Value.UseSeconds.bSet && FMath::IsNearlyEqual(Desc.Consumable.Value.UseSeconds.Value, 2.0));

	const auto Refused = [&](TFunction<void(FMantleWeaponSourceConsumable&)> Break, const TCHAR* What)
	{
		UMantleWeaponSource* Bad = MakeConsumable(EMantleWeaponSourceUseStyle::EatDrink);
		Break(Bad->Consumable);
		FMantleWeaponDesc Out;
		TArray<FText> Why;
		TestFalse(What, FMantleWeaponBuilderExport::BuildDocument(*Bad, GripPoint(), TEXT("FixtureMesh"), Out, Why));
		TestTrue(TEXT("with a message"), Why.Num() > 0);
	};
	Refused([](FMantleWeaponSourceConsumable& C) { C.Heal = 1001.f; }, TEXT("heal above 1000"));
	Refused([](FMantleWeaponSourceConsumable& C) { C.Heal = 0.f; }, TEXT("heal of 0 (use effect none instead)"));
	Refused([](FMantleWeaponSourceConsumable& C) { C.Uses = 0; }, TEXT("no uses"));
	Refused([](FMantleWeaponSourceConsumable& C) { C.bOverrideUseSeconds = true; C.UseSeconds = 0.1f; }, TEXT("use time below 0.5"));
	Refused([](FMantleWeaponSourceConsumable& C) { C.CooldownSeconds = 601.f; }, TEXT("cooldown above 600"));
	return true;
}

// "No effect" is explicit: effect none writes no heal, and the document reads back.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleWeaponBuilderConsumableNoEffectTest, "Mantle.Editor.WeaponBuilderConsumable.NoEffectIsExplicit", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleWeaponBuilderConsumableNoEffectTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponBuilderConsumableTestPrivate;
	UMantleWeaponSource* Input = MakeConsumable(EMantleWeaponSourceUseStyle::Use);
	Input->Consumable.Effect = EMantleWeaponSourceConsumableEffect::None;
	Input->Consumable.Uses = 5;
	FMantleWeaponDesc Desc;
	TArray<FText> Messages;
	if (!TestTrue(TEXT("effect none builds"), FMantleWeaponBuilderExport::BuildDocument(*Input, GripPoint(), TEXT("FixtureMesh"), Desc, Messages))) { for (const FText& Message : Messages) AddError(Message.ToString()); return true; }
	const FString Json = FMantleWeaponDocument::Write(Desc);
	TestTrue(TEXT("it says effect none"), Json.Contains(TEXT("\"effect\"")) && Json.Contains(TEXT("\"none\"")));
	TestFalse(TEXT("and writes no heal"), Json.Contains(TEXT("\"heal\"")));
	FMantleWeaponDesc Back;
	FString Error;
	TestTrue(TEXT("the document reads back"), FMantleWeaponDocument::Parse(Json, Back, Error));
	return true;
}
#endif
