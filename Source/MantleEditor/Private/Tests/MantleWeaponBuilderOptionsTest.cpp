// Copyright Joshua Eger. All Rights Reserved.
#include "Engine/StaticMesh.h"
#include "MantleWeaponBuilderExport.h"
#include "MantleWeaponBuilderLogic.h"
#include "MantleWeaponDocument.h"
#include "MantleWeaponSource.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace MantleWeaponBuilderOptionsTestPrivate
{
	TMap<FName, FTransform> RiflePoints()
	{
		TMap<FName, FTransform> Points;
		const FMantleWeaponDocHoldInfo& Info = FMantleWeaponDocument::GetHoldInfo(MantleWeaponBuilderLogic::ToDocHold(EMantleWeaponHold::Rifle));
		int32 Index = 0;
		for (const auto Socket : Info.Sockets)
		{
			Points.Add(MantleWeaponBuilderLogic::SocketRole(Socket), FTransform(FQuat::Identity, FVector(++Index * 10.0, 0.0, 0.0)));
		}
		// A rifle is held in both hands: the left palm is optional in the table but required by the check.
		Points.Add(MantleWeaponBuilderLogic::SocketRole(EMantleWeaponDocSocket::GripL), FTransform(FQuat::Identity, FVector(++Index * 10.0, 0.0, 0.0)));
		return Points;
	}

	/** A rifle whose details are effects (a model-less option is an effect, which needs no asset): an iron sight, a scope, a silencer, a torch. */
	UMantleWeaponSource* MakeOptionRifle()
	{
		UMantleWeaponSource* Out = NewObject<UMantleWeaponSource>();
		Out->DisplayName = TEXT("Options rifle");
		Out->Hold = EMantleWeaponHold::Rifle;
		Out->Tuning = MantleWeaponBuilderLogic::DefaultTuning(EMantleWeaponHold::Rifle);
		const auto Add = [Out](const TCHAR* Name, const EMantleWeaponSourceSlot Slot) -> FMantleWeaponSourceAttachment&
		{
			FMantleWeaponSourceAttachment& A = Out->Attachments.AddDefaulted_GetRef();
			A.Name = Name;
			A.bEffect = true;
			A.Effect = EMantleWeaponSourceStyle::Generic;
			A.Slot = Slot;
			return A;
		};
		Add(TEXT("IronSight"), EMantleWeaponSourceSlot::Fixed).bEffect = true;
		FMantleWeaponSourceAttachment& Scope = Add(TEXT("Scope4"), EMantleWeaponSourceSlot::Sight);
		Scope.bFitted = true;
		Scope.SightZoom = 4.f;
		Scope.bHasSightDistance = true;
		Scope.SightDistance = 35.f;
		Scope.SightAim.SetLocation(FVector(0.0, 0.0, 4.0));
		Scope.Stats.Spread = 0.8f;
		Scope.HideAttachments = {TEXT("IronSight")};
		FMantleWeaponSourceAttachment& Silencer = Add(TEXT("Silencer"), EMantleWeaponSourceSlot::Muzzle);
		Silencer.bSuppressed = true;
		Silencer.Stats.Damage = 0.9f;
		Silencer.Stats.Range = 0.9f;
		FMantleWeaponSourceAttachment& Torch = Add(TEXT("Torch"), EMantleWeaponSourceSlot::Light);
		Torch.bFlashlight = true;
		Torch.FlashlightBrightness = 1.5f;
		Torch.bLaser = true;
		Torch.LaserRange = 6000.f;
		return Out;
	}
}

// #234: attachment options in the builder's source become the weapon file's slot, fitted, stats, hides, sight, suppressed and light fields, only what
// differs from the default is written, the file reads back and writes the same text, and the validator's refusals reach the creator.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleWeaponBuilderOptionsTest, "Mantle.Editor.WeaponBuilderOptions.ExportsOptions", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleWeaponBuilderOptionsTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponBuilderOptionsTestPrivate;
	UMantleWeaponSource* Input = MakeOptionRifle();
	// A suppressed muzzle option needs the suppressed shot.
	FMantleWeaponDesc Desc;
	TArray<FText> Messages;
	TestFalse(TEXT("a suppressed muzzle option with no suppressed shot is refused"), FMantleWeaponBuilderExport::BuildDocument(*Input, RiflePoints(), TEXT("FixtureMesh"), Desc, Messages));
	bool bNamed = false;
	for (const FText& Message : Messages) bNamed |= Message.ToString().Contains(TEXT("fireSuppressed"));
	TestTrue(TEXT("and the refusal names sounds.fireSuppressed"), bNamed);

	Input->Attachments[2].bSuppressed = false;
	Messages.Reset();
	if (!TestTrue(TEXT("the options build"), FMantleWeaponBuilderExport::BuildDocument(*Input, RiflePoints(), TEXT("FixtureMesh"), Desc, Messages)))
	{
		for (const FText& Message : Messages) AddError(Message.ToString());
		return true;
	}
	if (!TestEqual(TEXT("four attachments"), Desc.Attachments.Value.Num(), 4)) return true;
	const TArray<FMantleWeaponDocAttachment>& Out = Desc.Attachments.Value;
	TestFalse(TEXT("the fixed detail has no slot"), Out[0].Slot.bSet);
	TestTrue(TEXT("the scope is a sight option"), Out[1].Slot.bSet && Out[1].Slot.Value == EMantleWeaponDocSlot::Sight);
	TestTrue(TEXT("fitted at the start"), Out[1].bFitted.bSet && Out[1].bFitted.Value);
	TestTrue(TEXT("with only its spread written"), Out[1].Stats.bSet && Out[1].Stats.Value.Spread.bSet && !Out[1].Stats.Value.Damage.bSet);
	TestEqual(TEXT("its spread"), Out[1].Stats.Value.Spread.Value, 0.8, 0.0001);
	TestEqual(TEXT("it hides the iron sight"), Out[1].HideAttachments.Value.Num(), 1);
	TestTrue(TEXT("zoom and sight distance"), Out[1].Sight.bSet && FMath::IsNearlyEqual(Out[1].Sight.Value.Zoom.Value, 4.0) && Out[1].Sight.Value.SightDistance.bSet);
	TestTrue(TEXT("the silencer is a muzzle option"), Out[2].Slot.Value == EMantleWeaponDocSlot::Muzzle && !Out[2].bFitted.bSet);
	TestTrue(TEXT("the torch has both lights"), Out[3].Light.bSet && Out[3].Light.Value.Flashlight.bSet && Out[3].Light.Value.Laser.bSet);

	// #234 (recolor): the builder lists a model's material slots, a static (rigid) model's as well as a skeletal one's.
	{
		UStaticMesh* RigidModel = NewObject<UStaticMesh>(GetTransientPackage());
		RigidModel->GetStaticMaterials().Add(FStaticMaterial(nullptr, TEXT("Body")));
		RigidModel->GetStaticMaterials().Add(FStaticMaterial(nullptr, TEXT("Grip")));
		const TArray<FName> RigidSlots = MantleWeaponBuilderLogic::MaterialSlotNames(RigidModel);
		TestEqual(TEXT("a static model lists its two material slots"), RigidSlots.Num(), 2);
		TestTrue(TEXT("by name, in slot order"), RigidSlots.Num() == 2 && RigidSlots[0] == FName(TEXT("Body")) && RigidSlots[1] == FName(TEXT("Grip")));
		TestEqual(TEXT("anything else lists none"), MantleWeaponBuilderLogic::MaterialSlotNames(GetTransientPackage()).Num(), 0);
	}

	// #234 (recolor): the paintable material slots, each once, in range and in slot order; none unless ticked.
	TestFalse(TEXT("no paintable slots unless ticked"), Desc.PaintSlots.bSet);
	Input->PaintSlots = {2, 0, 2, 99, -1};
	FMantleWeaponDesc Painted;
	Messages.Reset();
	if (TestTrue(TEXT("a weapon with paintable slots builds"), FMantleWeaponBuilderExport::BuildDocument(*Input, RiflePoints(), TEXT("FixtureMesh"), Painted, Messages)))
	{
		TestTrue(TEXT("and lists them once, in range and in order"), Painted.PaintSlots.bSet && Painted.PaintSlots.Value == TArray<int32>({0, 2}));
	}
	Input->PaintSlots.Reset();

	const FString Json = FMantleWeaponDocument::Write(Desc);
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

	// The checks the schema has reach the creator.
	Input->Attachments[2].bFitted = false;
	FMantleWeaponSourceAttachment& SecondScope = Input->Attachments.AddDefaulted_GetRef();
	SecondScope.Name = TEXT("Scope8");
	SecondScope.bEffect = true;
	SecondScope.Slot = EMantleWeaponSourceSlot::Sight;
	SecondScope.bFitted = true;
	Messages.Reset();
	FMantleWeaponDesc Refused;
	TestFalse(TEXT("two fitted options in one slot are refused"), FMantleWeaponBuilderExport::BuildDocument(*Input, RiflePoints(), TEXT("FixtureMesh"), Refused, Messages));
	bool bFitted = false;
	for (const FText& Message : Messages) bFitted |= Message.ToString().Contains(TEXT("fitted"));
	TestTrue(TEXT("and the refusal names the field"), bFitted);

	// A number left on an option that no longer shows a row for it (switched to a look-only extra) is dropped, not refused.
	Input->Attachments.Pop();
	Input->Attachments[2].Slot = EMantleWeaponSourceSlot::Extra;
	Messages.Reset();
	FMantleWeaponDesc Extra;
	if (TestTrue(TEXT("a muzzle option switched to extra still builds"), FMantleWeaponBuilderExport::BuildDocument(*Input, RiflePoints(), TEXT("FixtureMesh"), Extra, Messages)))
	{
		TestFalse(TEXT("with its damage and range dropped"), Extra.Attachments.Value[2].Stats.bSet);
	}
	else
	{
		for (const FText& Message : Messages) AddError(Message.ToString());
	}
	return true;
}
#endif
