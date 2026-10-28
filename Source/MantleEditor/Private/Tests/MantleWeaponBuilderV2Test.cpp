// Copyright Joshua Eger. All Rights Reserved.
#include "MantleWeaponBuilderExport.h"
#include "MantleWeaponBuilderKind.h"
#include "MantleWeaponBuilderLogic.h"
#include "MantleWeaponDocument.h"
#include "MantleWeaponRigidMesh.h"
#include "MantleWeaponSource.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "MeshDescription.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Sound/SoundWave.h"
#include "UObject/UObjectIterator.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace MantleWeaponBuilderV2TestPrivate
{
	struct FIsolatedExport
	{
		FString Name = TEXT("WB840_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
		FString PackageFolder() const { return TEXT("/Game/MantleWeapons/") + Name; }
		FString Folder() const { return FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / TEXT("MantleWeapons") / Name); }
		~FIsolatedExport()
		{
			TArray<UObject*> Assets;
			for (TObjectIterator<UObject> It; It; ++It)
				if (It->IsAsset() && It->GetOutermost()->GetName().StartsWith(PackageFolder() + TEXT("/"))) Assets.Add(*It);
			for (UObject* Asset : Assets)
			{
				FAssetRegistryModule::AssetDeleted(Asset);
				Asset->ClearFlags(RF_Public | RF_Standalone);
				Asset->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
				Asset->MarkAsGarbage();
			}
			const FString Base = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / TEXT("MantleWeapons")) + TEXT("/");
			const FString Target = Folder();
			if (Target.StartsWith(Base, ESearchCase::IgnoreCase) && Name.StartsWith(TEXT("WB840_")))
				IFileManager::Get().DeleteDirectory(*Target, false, true);
		}
	};

	UMantleWeaponSource* Source(EMantleWeaponHold Hold, const FString& Name, UStaticMesh* Model)
	{
		UMantleWeaponSource* Out = NewObject<UMantleWeaponSource>();
		Out->DisplayName = Name;
		Out->Hold = Hold;
		Out->Tuning = MantleWeaponBuilderLogic::DefaultTuning(Hold);
		Out->Model = Model;
		Out->bScaleOverridden = true;
		Out->FitScale = 1.f;
		return Out;
	}

	TMap<FName, FTransform> RequiredPoints(EMantleWeaponHold Hold)
	{
		TMap<FName, FTransform> Points;
		const auto& Info = FMantleWeaponDocument::GetHoldInfo(MantleWeaponBuilderLogic::ToDocHold(Hold));
		int32 Index = 0;
		for (auto Socket : Info.Sockets)
			Points.Add(MantleWeaponBuilderLogic::SocketRole(Socket), FTransform(FQuat::Identity, FVector(++Index * 10.0, 0.0, 0.0)));
		if (Info.bTwoHanded && Info.OptionalSockets.Contains(EMantleWeaponDocSocket::GripL))
			Points.Add(MantleWeaponBuilderLogic::SocketRole(EMantleWeaponDocSocket::GripL), FTransform(FQuat::Identity, FVector(++Index * 10.0, 0.0, 0.0)));
		return Points;
	}

	bool Read(const FIsolatedExport& Fixture, FMantleWeaponDesc& Desc, FString& Error)
	{
		FString Json;
		return FFileHelper::LoadFileToString(Json, *(Fixture.Folder() / (Fixture.Name + TEXT(".weapon.json")))) && FMantleWeaponDocument::Parse(Json, Desc, Error);
	}

	USkeletalMesh* Mesh(const FIsolatedExport& Fixture)
	{
		return LoadObject<USkeletalMesh>(nullptr, *(Fixture.PackageFolder() / (TEXT("SKM_") + Fixture.Name + TEXT(".SKM_") + Fixture.Name)));
	}

	int32 Triangles(USkeletalMesh& Mesh)
	{
		int32 Count = 0;
		if (const auto* Data = Mesh.GetResourceForRendering())
			if (!Data->LODRenderData.IsEmpty()) for (const auto& Section : Data->LODRenderData[0].RenderSections) Count += Section.NumTriangles;
		return Count;
	}

	int32 InputTriangles(UStaticMesh& Mesh)
	{
		const FMeshDescription* Geometry = Mesh.GetMeshDescription(0);
		return Geometry ? Geometry->Triangles().Num() : 0;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleWeaponBuilderV2AllHoldsTest, "Mantle.Editor.WeaponBuilderV2.ExportEveryHold", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleWeaponBuilderV2AllHoldsTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponBuilderV2TestPrivate;
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("fixture cube"), Cube)) return false;
	for (uint8 Index = 0; Index < static_cast<uint8>(EMantleWeaponDocHold::Count); ++Index)
	{
		FIsolatedExport Fixture;
		const auto Hold = static_cast<EMantleWeaponHold>(Index);
		UMantleWeaponSource* Input = Source(Hold, Fixture.Name, Cube);
		TArray<FText> Messages;
		const bool bExported = FMantleWeaponBuilderExport::Export(*Input, Messages);
		if (!TestTrue(*FString::Printf(TEXT("%s actual export"), FMantleWeaponDocument::HoldName(MantleWeaponBuilderLogic::ToDocHold(Hold))), bExported))
		{
			for (const auto& Message : Messages) AddError(Message.ToString());
			continue;
		}
		FMantleWeaponDesc Desc;
		FString Error;
		if (!TestTrue(TEXT("saved v2 parses"), Read(Fixture, Desc, Error))) { AddError(Error); continue; }
		TestTrue(TEXT("hold preserved"), Desc.GetHold() == MantleWeaponBuilderLogic::ToDocHold(Hold));
		const auto& Info = FMantleWeaponDocument::GetHoldInfo(Desc.GetHold());
		TestTrue(TEXT("default delivery is the hold's"), Desc.GetDelivery() == MantleWeaponBuilderLogic::UsualDelivery(Desc.GetHold()));
		TestTrue(TEXT("default resource is the hold's"), Desc.GetResource() == Info.Resources[0]);
		TArray<FMantleWeaponDocError> ValidationErrors;
		TestTrue(TEXT("document round trips exactly"), FMantleWeaponDocument::Validate(Desc, ValidationErrors));
		USkeletalMesh* Built = Mesh(Fixture);
		if (!TestNotNull(TEXT("saved generated rigid mesh"), Built)) continue;
		const auto Sockets = FMantleWeaponRigidMesh::ReadSocketsInMeshSpace(Built);
		for (auto Socket : Info.Sockets)
		{
			TestTrue(TEXT("required document socket"), Desc.Socket(Socket).bSet);
			TestTrue(TEXT("required saved mesh socket"), Sockets.Contains(MantleWeaponBuilderLogic::SocketRole(Socket)));
		}
		TestTrue(TEXT("LOD0 contains geometry"), Triangles(*Built) > 0);
		if (Hold == EMantleWeaponHold::Bow) TestTrue(TEXT("bow draw required"), Desc.Charge.bSet && Desc.Charge.Value.bRequired.Value);
		if (Hold == EMantleWeaponHold::Shield) TestTrue(TEXT("shield block required"), Desc.Block.bSet && Desc.Block.Value.bEnabled.Value);
		if (Desc.Block.bSet) TestEqual(TEXT("the stun time exports (default 1.5 s)"), Desc.Block.Value.StunSeconds, 1.5);
		if (Hold == EMantleWeaponHold::Staff)
		{
			// A staff casts the held beam by default and carries its charges.
			TestTrue(TEXT("staff: the held beam"), Desc.GetDelivery() == EMantleWeaponDocDelivery::Beam && Desc.Beam.bSet);
			TestTrue(TEXT("staff: the charges"), Desc.Charges.bSet && Desc.Charges.Value.Count == 3 && FMath::IsNearlyEqual(Desc.Charges.Value.RefillSeconds, 4.0));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleWeaponBuilderV2CombinationsTest, "Mantle.Editor.WeaponBuilderV2.DeliveryAndModifiers", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleWeaponBuilderV2CombinationsTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponBuilderV2TestPrivate;
	for (uint8 Index = 0; Index < static_cast<uint8>(EMantleWeaponDocHold::Count); ++Index)
	{
		const auto Hold = static_cast<EMantleWeaponHold>(Index);
		const auto& Info = FMantleWeaponDocument::GetHoldInfo(MantleWeaponBuilderLogic::ToDocHold(Hold));
		for (auto Delivery : Info.Deliveries)
		{
			UMantleWeaponSource* Input = Source(Hold, TEXT("Combination"), nullptr);
			Input->Delivery = static_cast<EMantleWeaponSourceDelivery>(static_cast<uint8>(Delivery) + 1);
			if (Info.Charge != EMantleWeaponDocChargeRule::None) { Input->Charge.bEnabled = true; Input->Charge.Seconds = 1.25f; Input->Charge.bCancelable = false; }
			if (Info.bHasEffects && Delivery != EMantleWeaponDocDelivery::Sweep) { Input->bOverrideStyle = true; Input->Style = EMantleWeaponSourceStyle::Pistol; }
			FMantleWeaponDesc Desc;
			TArray<FText> Messages;
			const bool bBuilt = FMantleWeaponBuilderExport::BuildDocument(*Input, RequiredPoints(Hold), TEXT("FixtureMesh"), Desc, Messages);
			// Every delivery a hold's table lists is offered (the staff's flying spell too).
			TestTrue(TEXT("the panel offers every delivery the hold lists"), MantleWeaponBuilderLogic::IsDeliveryOffered(MantleWeaponBuilderLogic::ToDocHold(Hold), Delivery));
			if (!TestTrue(TEXT("allowed delivery/modifier combination"), bBuilt)) for (const auto& Message : Messages) AddError(Message.ToString());
			if (bBuilt && Delivery == EMantleWeaponDocDelivery::Beam) TestEqual(TEXT("beam tick rate"), Desc.Beam.Value.TickRate.Value, 10.0);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleWeaponBuilderV2EquipStylesTest, "Mantle.Editor.WeaponBuilderV2.EquipStylesExportAndValidate", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleWeaponBuilderV2EquipStylesTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponBuilderV2TestPrivate;
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("equip style fixture cube"), Cube)) return false;

	auto ExportAndRead = [this, Cube](EMantleWeaponHold Hold, FName Style, const FString& Expected, const TCHAR* Label)
	{
		FIsolatedExport Fixture;
		UMantleWeaponSource* Input = Source(Hold, Fixture.Name, Cube);
		Input->EquipStyle = Style;
		TArray<FText> Messages;
		if (!TestTrue(*FString::Printf(TEXT("%s exports"), Label), FMantleWeaponBuilderExport::Export(*Input, Messages)))
		{
			for (const FText& Message : Messages) AddError(Message.ToString());
			return;
		}
		FMantleWeaponDesc Desc;
		FString Error;
		if (!TestTrue(*FString::Printf(TEXT("%s saved document parses"), Label), Read(Fixture, Desc, Error)))
		{
			AddError(Error);
			return;
		}
		if (Expected.IsEmpty())
		{
			TestFalse(*FString::Printf(TEXT("%s leaves the first hold style as default"), Label), Desc.EquipStyle.bSet);
		}
		else
		{
			TestTrue(*FString::Printf(TEXT("%s writes an explicit style"), Label), Desc.EquipStyle.bSet);
			TestEqual(*FString::Printf(TEXT("%s keeps its style id"), Label), Desc.EquipStyle.Value, Expected);
		}
	};

	ExportAndRead(EMantleWeaponHold::Melee1h, FName(TEXT("hip")), TEXT("hip"), TEXT("SwordShield hip draw"));
	ExportAndRead(EMantleWeaponHold::Melee1h, FName(TEXT("back")), TEXT("back"), TEXT("SwordShield back draw"));
	ExportAndRead(EMantleWeaponHold::Melee1h, FName(TEXT("overhead")), TEXT("overhead"), TEXT("SwordShield overhead flourish"));
	ExportAndRead(EMantleWeaponHold::Spear, FName(TEXT("back")), TEXT("back"), TEXT("SpearShield back draw"));
	ExportAndRead(EMantleWeaponHold::Bow, FName(TEXT("back")), TEXT("back"), TEXT("Bow back draw"));
	ExportAndRead(EMantleWeaponHold::Bow, FName(TEXT("bow_equip_01")), TEXT("bow_equip_01"), TEXT("Bow Equip 01"));
	ExportAndRead(EMantleWeaponHold::Bow, FName(TEXT("bow_equip_02")), TEXT("bow_equip_02"), TEXT("Bow Equip 02"));
	ExportAndRead(EMantleWeaponHold::Bow, FName(TEXT("bow_equip_03")), TEXT("bow_equip_03"), TEXT("Bow Equip 03"));
	ExportAndRead(EMantleWeaponHold::Bow, FName(TEXT("bow_equip_04")), TEXT("bow_equip_04"), TEXT("Bow Equip 04"));
	ExportAndRead(EMantleWeaponHold::Melee1h, NAME_None, TEXT(""), TEXT("SwordShield default"));
	ExportAndRead(EMantleWeaponHold::Bow, NAME_None, TEXT(""), TEXT("Bow default"));

	UMantleWeaponSource* InvalidSpear = Source(EMantleWeaponHold::Spear, TEXT("InvalidSpearStyle"), Cube);
	InvalidSpear->EquipStyle = TEXT("hip");
	FMantleWeaponDesc Refused;
	TArray<FText> Messages;
	TestFalse(TEXT("Spear rejects a SwordShield-only hip style"), FMantleWeaponBuilderExport::BuildDocument(
		*InvalidSpear, RequiredPoints(EMantleWeaponHold::Spear), TEXT("Spear"), Refused, Messages));
	TestTrue(TEXT("the hold-specific refusal explains the invalid choice"), Messages.ContainsByPredicate([](const FText& Message)
	{
		return Message.ToString().Contains(TEXT("does not offer"));
	}));
	return true;
}

// Round 2: the dodge style a creator picks is written as dodgeStyle where the hold has more than one dodge, and refused on a hold without
// that style; none picked writes nothing (the hold's default).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleWeaponBuilderV2DodgeStyleTest, "Mantle.Editor.WeaponBuilderV2.DodgeStyle", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleWeaponBuilderV2DodgeStyleTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponBuilderV2TestPrivate;
	auto Build = [](EMantleWeaponHold Hold, FName Style, FMantleWeaponDesc& Desc, TArray<FText>& Messages)
	{
		UMantleWeaponSource* Input = Source(Hold, TEXT("DodgeStyle"), nullptr);
		Input->DodgeStyle = Style;
		Messages.Reset();
		return FMantleWeaponBuilderExport::BuildDocument(*Input, RequiredPoints(Hold), TEXT("FixtureMesh"), Desc, Messages);
	};
	FMantleWeaponDesc Desc;
	TArray<FText> Messages;
	if (TestTrue(TEXT("a shield with the crouched dodge builds"), Build(EMantleWeaponHold::Shield, TEXT("crouched"), Desc, Messages)))
	{
		TestTrue(TEXT("and writes it"), Desc.DodgeStyle.bSet && Desc.DodgeStyle.Value == TEXT("crouched"));
	}
	if (TestTrue(TEXT("knuckles with the sidestep build"), Build(EMantleWeaponHold::Unarmed, TEXT("sidestep"), Desc, Messages)))
	{
		TestTrue(TEXT("and write it"), Desc.DodgeStyle.bSet && Desc.DodgeStyle.Value == TEXT("sidestep"));
	}
	if (TestTrue(TEXT("a shield with no dodge picked builds"), Build(EMantleWeaponHold::Shield, NAME_None, Desc, Messages)))
	{
		TestFalse(TEXT("and leaves the hold's default"), Desc.DodgeStyle.bSet);
	}
	TestFalse(TEXT("knuckles refuse the shield's upright dodge"), Build(EMantleWeaponHold::Unarmed, TEXT("upright"), Desc, Messages));
	TestFalse(TEXT("a bow, with one dodge, refuses any"), Build(EMantleWeaponHold::Bow, TEXT("dash"), Desc, Messages));
	TestTrue(TEXT("the refusal explains the choice"), Messages.ContainsByPredicate([](const FText& Message)
	{
		return Message.ToString().Contains(TEXT("does not offer"));
	}));
	return true;
}

// Round 3: the combo a creator picks is written as comboSet where the hold has more than one, and refused on a hold without that combo;
// none picked writes nothing (the hold's default).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleWeaponBuilderV2ComboSetTest, "Mantle.Editor.WeaponBuilderV2.ComboSet", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleWeaponBuilderV2ComboSetTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponBuilderV2TestPrivate;
	auto Build = [](EMantleWeaponHold Hold, FName Set, FMantleWeaponDesc& Desc, TArray<FText>& Messages)
	{
		UMantleWeaponSource* Input = Source(Hold, TEXT("ComboSet"), nullptr);
		Input->ComboSet = Set;
		Messages.Reset();
		return FMantleWeaponBuilderExport::BuildDocument(*Input, RequiredPoints(Hold), TEXT("FixtureMesh"), Desc, Messages);
	};
	FMantleWeaponDesc Desc;
	TArray<FText> Messages;
	if (TestTrue(TEXT("a sword with the spin combo builds"), Build(EMantleWeaponHold::Melee1h, TEXT("spin"), Desc, Messages)))
	{
		TestTrue(TEXT("and writes it"), Desc.ComboSet.bSet && Desc.ComboSet.Value == TEXT("spin"));
	}
	if (TestTrue(TEXT("knuckles with the jab and cross build"), Build(EMantleWeaponHold::Unarmed, TEXT("jab_cross"), Desc, Messages)))
	{
		TestTrue(TEXT("and write it"), Desc.ComboSet.bSet && Desc.ComboSet.Value == TEXT("jab_cross"));
	}
	if (TestTrue(TEXT("a spear with the lunge builds"), Build(EMantleWeaponHold::Spear, TEXT("lunge"), Desc, Messages)))
	{
		TestTrue(TEXT("and writes it"), Desc.ComboSet.bSet && Desc.ComboSet.Value == TEXT("lunge"));
	}
	if (TestTrue(TEXT("a sword with no combo picked builds"), Build(EMantleWeaponHold::Melee1h, NAME_None, Desc, Messages)))
	{
		TestFalse(TEXT("and leaves the hold's default"), Desc.ComboSet.bSet);
	}
	TestFalse(TEXT("knuckles refuse the sword's spin"), Build(EMantleWeaponHold::Unarmed, TEXT("spin"), Desc, Messages));
	TestFalse(TEXT("a bow, with one combo, refuses any"), Build(EMantleWeaponHold::Bow, TEXT("quick"), Desc, Messages));
	TestTrue(TEXT("the refusal explains the choice"), Messages.ContainsByPredicate([](const FText& Message)
	{
		return Message.ToString().Contains(TEXT("does not offer"));
	}));
	return true;
}

// Round 3: the quiver a creator picks is written as quiver on a bow, and refused on a hold without one; none picked writes nothing (back, the
// hold's default).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleWeaponBuilderV2QuiverTest, "Mantle.Editor.WeaponBuilderV2.Quiver", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleWeaponBuilderV2QuiverTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponBuilderV2TestPrivate;
	auto Build = [](EMantleWeaponHold Hold, FName Position, FMantleWeaponDesc& Desc, TArray<FText>& Messages)
	{
		UMantleWeaponSource* Input = Source(Hold, TEXT("Quiver"), nullptr);
		Input->Quiver = Position;
		Messages.Reset();
		return FMantleWeaponBuilderExport::BuildDocument(*Input, RequiredPoints(Hold), TEXT("FixtureMesh"), Desc, Messages);
	};
	FMantleWeaponDesc Desc;
	TArray<FText> Messages;
	for (const TCHAR* Position : { TEXT("back"), TEXT("hip"), TEXT("side") })
	{
		if (TestTrue(*FString::Printf(TEXT("a bow with the %s quiver builds"), Position), Build(EMantleWeaponHold::Bow, Position, Desc, Messages)))
		{
			TestTrue(*FString::Printf(TEXT("and writes %s"), Position), Desc.Quiver.bSet && Desc.Quiver.Value == Position);
			FString Error;
			FMantleWeaponDesc Again;
			TestTrue(TEXT("and the document reads again"), FMantleWeaponDocument::Parse(FMantleWeaponDocument::Write(Desc), Again, Error));
			TestTrue(TEXT("with the same quiver"), Again.Quiver.bSet && Again.Quiver.Value == Position);
		}
	}
	if (TestTrue(TEXT("a bow with no quiver picked builds"), Build(EMantleWeaponHold::Bow, NAME_None, Desc, Messages)))
	{
		TestFalse(TEXT("and leaves the hold's default"), Desc.Quiver.bSet);
	}
	TestFalse(TEXT("a bow refuses a quiver it does not offer"), Build(EMantleWeaponHold::Bow, TEXT("head"), Desc, Messages));
	TestFalse(TEXT("a sword has no quiver to place"), Build(EMantleWeaponHold::Melee1h, TEXT("hip"), Desc, Messages));
	TestTrue(TEXT("the refusal explains the choice"), Messages.ContainsByPredicate([](const FText& Message)
	{
		return Message.ToString().Contains(TEXT("no hip quiver"));
	}));
	return true;
}

// Round 3: the shooting style a creator picks is written as style on a bow, and refused on a hold with one way to hold it; none picked writes
// nothing (a, the hold's default).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleWeaponBuilderV2ShootingStyleTest, "Mantle.Editor.WeaponBuilderV2.ShootingStyle", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleWeaponBuilderV2ShootingStyleTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponBuilderV2TestPrivate;
	auto Build = [](EMantleWeaponHold Hold, FName Style, FMantleWeaponDesc& Desc, TArray<FText>& Messages)
	{
		UMantleWeaponSource* Input = Source(Hold, TEXT("ShootingStyle"), nullptr);
		Input->ShootingStyle = Style;
		Messages.Reset();
		return FMantleWeaponBuilderExport::BuildDocument(*Input, RequiredPoints(Hold), TEXT("FixtureMesh"), Desc, Messages);
	};
	FMantleWeaponDesc Desc;
	TArray<FText> Messages;
	for (const TCHAR* Style : { TEXT("a"), TEXT("c") })
	{
		if (TestTrue(*FString::Printf(TEXT("a bow in style %s builds"), Style), Build(EMantleWeaponHold::Bow, Style, Desc, Messages)))
		{
			TestTrue(*FString::Printf(TEXT("and writes %s"), Style), Desc.ShootingStyle.bSet && Desc.ShootingStyle.Value == Style);
			FString Error;
			FMantleWeaponDesc Again;
			TestTrue(TEXT("and the document reads again"), FMantleWeaponDocument::Parse(FMantleWeaponDocument::Write(Desc), Again, Error));
			TestTrue(TEXT("with the same style"), Again.ShootingStyle.bSet && Again.ShootingStyle.Value == Style);
		}
	}
	if (TestTrue(TEXT("a bow with no style picked builds"), Build(EMantleWeaponHold::Bow, NAME_None, Desc, Messages)))
	{
		TestFalse(TEXT("and leaves the hold's default"), Desc.ShootingStyle.bSet);
	}
	TestFalse(TEXT("a bow refuses style b (not built)"), Build(EMantleWeaponHold::Bow, TEXT("b"), Desc, Messages));
	TestFalse(TEXT("a sword has one way to hold it"), Build(EMantleWeaponHold::Melee1h, TEXT("c"), Desc, Messages));
	TestTrue(TEXT("the refusal explains the choice"), Messages.ContainsByPredicate([](const FText& Message)
	{
		return Message.ToString().Contains(TEXT("no shooting style c"));
	}));
	return true;
}

// Round 3 (bow step 5): a bow may block. "Can block" on writes block (with its stun), off (the default) writes none; a staff still cannot.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleWeaponBuilderV2BowBlockTest, "Mantle.Editor.WeaponBuilderV2.BowBlock", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleWeaponBuilderV2BowBlockTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponBuilderV2TestPrivate;
	TestTrue(TEXT("the hold library lets a bow block"), FMantleWeaponDocument::GetHoldInfo(EMantleWeaponDocHold::Bow).BlockRule == EMantleWeaponDocBlockRule::Optional);
	TestTrue(TEXT("and not a staff"), FMantleWeaponDocument::GetHoldInfo(EMantleWeaponDocHold::Staff).BlockRule == EMantleWeaponDocBlockRule::None);
	auto Build = [](const bool bBlock, FMantleWeaponDesc& Desc, TArray<FText>& Messages)
	{
		UMantleWeaponSource* Input = Source(EMantleWeaponHold::Bow, TEXT("BowBlock"), nullptr);
		Input->bBlock = bBlock;
		Input->bBreakOnHeavy = true;
		Input->StunSeconds = 2.0f;
		Messages.Reset();
		return FMantleWeaponBuilderExport::BuildDocument(*Input, RequiredPoints(EMantleWeaponHold::Bow), TEXT("FixtureMesh"), Desc, Messages);
	};
	FMantleWeaponDesc Desc;
	TArray<FText> Messages;
	if (TestTrue(TEXT("a bow that blocks builds"), Build(true, Desc, Messages)))
	{
		TestTrue(TEXT("and writes block, enabled"), Desc.Block.bSet && Desc.Block.Value.bEnabled.bSet && Desc.Block.Value.bEnabled.Value);
		TestEqual(TEXT("with its stun"), Desc.Block.bSet ? Desc.Block.Value.StunSeconds : -1.0, 2.0);
		FString Error;
		FMantleWeaponDesc Again;
		TestTrue(TEXT("and the document reads again"), FMantleWeaponDocument::Parse(FMantleWeaponDocument::Write(Desc), Again, Error));
		TestTrue(TEXT("with the block"), Again.Block.bSet);
	}
	if (TestTrue(TEXT("a bow left as it is builds"), Build(false, Desc, Messages)))
	{
		TestFalse(TEXT("and does not block (right mouse zooms)"), Desc.Block.bSet);
	}
	return true;
}

// Round 3: a bow's bash is on by default and writes nothing; a creator turning it off or changing its damage is written, and no other hold has one.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleWeaponBuilderV2BowBashTest, "Mantle.Editor.WeaponBuilderV2.BowBash", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleWeaponBuilderV2BowBashTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponBuilderV2TestPrivate;
	TestTrue(TEXT("the hold library gives a bow a bash"), FMantleWeaponDocument::GetHoldInfo(EMantleWeaponDocHold::Bow).bHasBash);
	TestFalse(TEXT("and a sword none"), FMantleWeaponDocument::GetHoldInfo(EMantleWeaponDocHold::Melee1h).bHasBash);
	auto Build = [](const EMantleWeaponHold Hold, const bool bBash, const float Damage, FMantleWeaponDesc& Desc, TArray<FText>& Messages)
	{
		UMantleWeaponSource* Input = Source(Hold, TEXT("BowBash"), nullptr);
		Input->bBash = bBash;
		Input->BashDamage = Damage;
		Messages.Reset();
		return FMantleWeaponBuilderExport::BuildDocument(*Input, RequiredPoints(Hold), TEXT("FixtureMesh"), Desc, Messages);
	};
	FMantleWeaponDesc Desc;
	TArray<FText> Messages;
	if (TestTrue(TEXT("a bow left as it is builds"), Build(EMantleWeaponHold::Bow, true, 20.0f, Desc, Messages)))
	{
		TestFalse(TEXT("and writes no bash (on, 20 damage)"), Desc.Bash.bSet);
	}
	if (TestTrue(TEXT("a bow with the bash off builds"), Build(EMantleWeaponHold::Bow, false, 20.0f, Desc, Messages)))
	{
		TestTrue(TEXT("and writes the bash, off"), Desc.Bash.bSet && Desc.Bash.Value.bEnabled.bSet && !Desc.Bash.Value.bEnabled.Value);
		FString Error;
		FMantleWeaponDesc Again;
		TestTrue(TEXT("and the document reads again"), FMantleWeaponDocument::Parse(FMantleWeaponDocument::Write(Desc), Again, Error));
		TestTrue(TEXT("with the bash off"), Again.Bash.bSet && !Again.Bash.Value.bEnabled.Value);
	}
	if (TestTrue(TEXT("a bow with a bash damage builds"), Build(EMantleWeaponHold::Bow, true, 35.0f, Desc, Messages)))
	{
		TestTrue(TEXT("and writes it, on"), Desc.Bash.bSet && Desc.Bash.Value.bEnabled.Value && Desc.Bash.Value.Damage == 35.0);
	}
	if (TestTrue(TEXT("a sword with the same fields builds"), Build(EMantleWeaponHold::Melee1h, false, 35.0f, Desc, Messages)))
	{
		TestFalse(TEXT("and writes no bash (it has none)"), Desc.Bash.bSet);
	}
	return true;
}

// Round 3 step 12: a shield's throw is off by default and writes nothing; turned on it writes its range and damage and reads again; no other hold
// has one.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleWeaponBuilderV2ShieldThrowTest, "Mantle.Editor.WeaponBuilderV2.ShieldThrow", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleWeaponBuilderV2ShieldThrowTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponBuilderV2TestPrivate;
	TestTrue(TEXT("the hold library lets a shield throw"), FMantleWeaponDocument::GetHoldInfo(EMantleWeaponDocHold::Shield).bHasShieldThrow);
	TestFalse(TEXT("and a sword not"), FMantleWeaponDocument::GetHoldInfo(EMantleWeaponDocHold::Melee1h).bHasShieldThrow);
	auto Build = [](const EMantleWeaponHold Hold, const bool bThrow, const float Range, const float Damage, FMantleWeaponDesc& Desc, TArray<FText>& Messages)
	{
		UMantleWeaponSource* Input = Source(Hold, TEXT("ShieldThrow"), nullptr);
		Input->bShieldThrow = bThrow;
		Input->ShieldThrowRange = Range;
		Input->ShieldThrowDamage = Damage;
		Messages.Reset();
		return FMantleWeaponBuilderExport::BuildDocument(*Input, RequiredPoints(Hold), TEXT("FixtureMesh"), Desc, Messages);
	};
	FMantleWeaponDesc Desc;
	TArray<FText> Messages;
	if (TestTrue(TEXT("a shield left as it is builds"), Build(EMantleWeaponHold::Shield, false, 1000.0f, 25.0f, Desc, Messages)))
	{
		TestFalse(TEXT("and writes no throw (off)"), Desc.ShieldThrow.bSet);
	}
	if (TestTrue(TEXT("a shield that throws builds"), Build(EMantleWeaponHold::Shield, true, 1500.0f, 40.0f, Desc, Messages)))
	{
		TestTrue(TEXT("and writes the throw, on"), Desc.ShieldThrow.bSet && Desc.ShieldThrow.Value.bEnabled.Value);
		TestEqual(TEXT("with its range"), Desc.ShieldThrow.Value.Range, 1500.0);
		TestEqual(TEXT("and damage"), Desc.ShieldThrow.Value.Damage, 40.0);
		FString Error;
		FMantleWeaponDesc Again;
		TestTrue(TEXT("and the document reads again"), FMantleWeaponDocument::Parse(FMantleWeaponDocument::Write(Desc), Again, Error));
		TestTrue(TEXT("with the throw on"), Again.ShieldThrow.bSet && Again.ShieldThrow.Value.bEnabled.Value && Again.ShieldThrow.Value.Range == 1500.0);
	}
	if (TestTrue(TEXT("a sword with the same fields builds"), Build(EMantleWeaponHold::Melee1h, true, 1500.0f, 40.0f, Desc, Messages)))
	{
		TestFalse(TEXT("and writes no throw (it has none)"), Desc.ShieldThrow.bSet);
	}
	return true;
}

// A staff's held beam that also throws spells writes both blocks, with or without a charge (aim and hold), and the document
// reader takes it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleWeaponBuilderV2StaffSpellsBesideBeamTest, "Mantle.Editor.WeaponBuilderV2.StaffSpellsBesideBeam", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleWeaponBuilderV2StaffSpellsBesideBeamTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponBuilderV2TestPrivate;
	UMantleWeaponSource* Input = Source(EMantleWeaponHold::Staff, TEXT("BeamAndSpells"), nullptr);
	Input->Delivery = EMantleWeaponSourceDelivery::Beam;
	Input->bSpellsBesideBeam = true;
	FMantleWeaponDesc Desc;
	TArray<FText> Messages;
	const bool bBuilt = FMantleWeaponBuilderExport::BuildDocument(*Input, RequiredPoints(EMantleWeaponHold::Staff), TEXT("FixtureMesh"), Desc, Messages);
	if (!TestTrue(TEXT("a staff with a beam and spells builds"), bBuilt)) for (const auto& Message : Messages) AddError(Message.ToString());
	TestTrue(TEXT("its delivery is the beam"), Desc.GetDelivery() == EMantleWeaponDocDelivery::Beam);
	TestTrue(TEXT("with the beam's numbers"), Desc.Beam.bSet);
	TestTrue(TEXT("and the spells' (a projectile beside the beam)"), Desc.Projectile.bSet && !Desc.Projectile.Value.bThrown.Value);
	TestFalse(TEXT("and no charge"), Desc.Charge.bSet);
	TArray<FMantleWeaponDocError> Errors;
	TestTrue(TEXT("the document reader takes it"), FMantleWeaponDocument::Validate(Desc, Errors));

	Input->Charge.bEnabled = true;
	Messages.Reset();
	TestTrue(TEXT("with a charge too (aim and hold)"), FMantleWeaponBuilderExport::BuildDocument(*Input, RequiredPoints(EMantleWeaponHold::Staff), TEXT("FixtureMesh"), Desc, Messages));
	TestTrue(TEXT("which it writes"), Desc.Charge.bSet && Desc.Beam.bSet && Desc.Projectile.bSet);
	Errors.Reset();
	TestTrue(TEXT("and the document reader takes"), FMantleWeaponDocument::Validate(Desc, Errors));

	Input->Charge.bEnabled = false;
	Input->bSpellsBesideBeam = false;
	TestTrue(TEXT("a beam staff alone still builds"), FMantleWeaponBuilderExport::BuildDocument(*Input, RequiredPoints(EMantleWeaponHold::Staff), TEXT("FixtureMesh"), Desc, Messages));
	TestFalse(TEXT("with no spells"), Desc.Projectile.bSet);
	return true;
}

// A fist weapon (unarmed) is worn on the hand, so the builder refuses one with no model in a creator's words, and writes nothing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleWeaponBuilderV2FistsNeedAModelTest, "Mantle.Editor.WeaponBuilderV2.FistWeaponNeedsAModel", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleWeaponBuilderV2FistsNeedAModelTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponBuilderV2TestPrivate;
	FIsolatedExport Fixture;
	UMantleWeaponSource* Input = Source(EMantleWeaponHold::Unarmed, Fixture.Name, nullptr);
	TArray<FText> Messages;
	TestFalse(TEXT("a fist weapon with no model does not export"), FMantleWeaponBuilderExport::Export(*Input, Messages));
	const FString Expected = MantleWeaponBuilderLogic::MissingModelMessage(EMantleWeaponHold::Unarmed).ToString();
	TestTrue(TEXT("the plain message names the knuckles, claws or gauntlet"), Expected.Contains(TEXT("knuckles, claws or gauntlet")));
	TestTrue(TEXT("and is what the creator reads"), Messages.ContainsByPredicate([&Expected](const FText& Message) { return Message.ToString() == Expected; }));
	TestFalse(TEXT("no settings file is written"), IFileManager::Get().FileExists(*(Fixture.Folder() / (Fixture.Name + TEXT(".weapon.json")))));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleWeaponBuilderV2SocketAliasTest, "Mantle.Editor.WeaponBuilderV2.SocketAliasUsesCreatorPalm", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleWeaponBuilderV2SocketAliasTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponBuilderV2TestPrivate;
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("alias fixture cube"), Cube)) return false;
	FMantleWeaponBuilderKind Kind(EMantleWeaponFitHold::Rifle);
	Kind.SetScaleOverride(1.f);
	const auto Guesses = Kind.GuessPoints(Cube);
	const FMantleBuilderPoint* Guess = Guesses.FindByPredicate([](const auto& Point) { return Point.Role == FName(TEXT("Grip_R")); });
	if (!TestNotNull(TEXT("canonical guessed palm"), Guess)) return false;
	FIsolatedExport Fixture;
	UMantleWeaponSource* Input = Source(EMantleWeaponHold::Rifle, Fixture.Name, Cube);
	FMantleBuilderPoint Creator = *Guess;
	Creator.Role = TEXT("gripR");
	Creator.Source = EMantleBuilderPointSource::Creator;
	Creator.Transform.AddToTranslation(FVector(17.0, 23.0, 31.0));
	Input->Points.Add(Creator);
	TArray<FText> Messages;
	if (!TestTrue(TEXT("creator socket alias actually exports"), FMantleWeaponBuilderExport::Export(*Input, Messages)))
	{
		for (const auto& Message : Messages) AddError(Message.ToString());
		return false;
	}
	FMantleWeaponDesc Desc;
	FString Error;
	if (!TestTrue(TEXT("alias document saved"), Read(Fixture, Desc, Error))) { AddError(Error); return false; }
	USkeletalMesh* Built = Mesh(Fixture);
	if (!TestNotNull(TEXT("alias mesh saved"), Built)) return false;
	const auto Sockets = FMantleWeaponRigidMesh::ReadSocketsInMeshSpace(Built);
	const FTransform* Palm = Sockets.Find(TEXT("Grip_R"));
	if (!TestNotNull(TEXT("canonical generated palm"), Palm)) return false;
	const FVector Expected = Kind.GetLastFit().MeshToWeapon.TransformPosition(Creator.Transform.GetLocation());
	const FVector Guessed = Kind.GetLastFit().MeshToWeapon.TransformPosition(Guess->Transform.GetLocation());
	const FVector DocumentPalm = Desc.Socket(EMantleWeaponDocSocket::GripR).Value.Location.Value;
	TestTrue(TEXT("creator alias overrides canonical guess in settings"), DocumentPalm.Equals(Expected, 0.001) && !DocumentPalm.Equals(Guessed, 0.001));
	TestTrue(TEXT("saved mesh palm agrees with settings"), Palm->GetLocation().Equals(DocumentPalm, 0.001));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleWeaponBuilderV2PartsTest, "Mantle.Editor.WeaponBuilderV2.ExportAuxiliaryAssets", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleWeaponBuilderV2PartsTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponBuilderV2TestPrivate;
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("fixture cube"), Cube)) return false;
	FIsolatedExport Fixture;
	UMantleWeaponSource* Input = Source(EMantleWeaponHold::Rifle, Fixture.Name, Cube);
	Input->Delivery = EMantleWeaponSourceDelivery::Projectile;
	Input->Projectile.Model = Cube;
	FMantleWeaponSourceAttachment Attachment;
	Attachment.Name = TEXT("FocusGlow");
	Attachment.Model = Cube;
	Attachment.bEffect = true;
	Attachment.Effect = EMantleWeaponSourceStyle::Arcane;
	Attachment.Socket = TEXT("Muzzle");
	Attachment.RelativeTransform = FTransform(FQuat::Identity, FVector(5.0, 0.0, 0.0), FVector(0.1));
	Input->Attachments.Add(Attachment);
	USoundWave* Sound = LoadObject<USoundWave>(nullptr, TEXT("/Engine/EditorSounds/Notifications/CompileSuccess.CompileSuccess"));
	if (!TestNotNull(TEXT("fixture shot sound"), Sound)) return false;
	Input->FireSound = Sound;
	Input->FireSuppressedSound = Sound;
	Input->bSuppressed = true;
	Input->FireSoundTrigger = TEXT("Shot");
	TArray<FText> Messages;
	if (!TestTrue(TEXT("actual parts/aux export"), FMantleWeaponBuilderExport::Export(*Input, Messages)))
	{
		for (const auto& Message : Messages) AddError(Message.ToString());
		return false;
	}
	FMantleWeaponDesc Desc;
	FString Error;
	if (!TestTrue(TEXT("parts document parses"), Read(Fixture, Desc, Error))) { AddError(Error); return false; }
	USkeletalMesh* Built = Mesh(Fixture);
	if (!TestNotNull(TEXT("parts mesh"), Built)) return false;
	// Every input triangle remains in the saved render data, regardless of the engine fixture's tessellation.
	TestTrue(TEXT("source cube geometry available"), InputTriangles(*Cube) > 0);
	TestEqual(TEXT("the complete mesh's render triangles"), Triangles(*Built), InputTriangles(*Cube));
	for (const FString& LocalName : { Fixture.Name + TEXT("_Projectile_0"), Desc.Attachments.Value[0].Mesh, Desc.Sounds.Value.Fire.Value, Desc.Sounds.Value.FireSuppressed.Value })
		TestNotNull(TEXT("local auxiliary asset resolves"), LoadObject<UObject>(nullptr, *(Fixture.PackageFolder() / (LocalName + TEXT(".") + LocalName))));
	TestEqual(TEXT("short attachment names keep their existing spelling"), Desc.Attachments.Value[0].Mesh, Fixture.Name + TEXT("_Attachment_0"));
	TestEqual(TEXT("short fire sound names keep their existing spelling"), Desc.Sounds.Value.Fire.Value, Fixture.Name + TEXT("_Fire_0"));
	TestEqual(TEXT("short quiet sound names keep their existing spelling"), Desc.Sounds.Value.FireSuppressed.Value, Fixture.Name + TEXT("_FireSuppressed_0"));
	Messages.Reset();
	TestTrue(TEXT("auxiliary assets rebuild"), FMantleWeaponBuilderExport::Export(*Input, Messages));

	// Export a real weapon at the safe-name boundary. Generated local names retain their
	// identifying suffixes within the document's 64-character limit and still resolve on disk.
	FIsolatedExport LongFixture;
	while (LongFixture.Name.Len() < 60) LongFixture.Name.AppendChar(TEXT('X'));
	const FString LongDisplayName = LongFixture.Name + TEXT("TAIL");
	TestEqual(TEXT("the fixture display name reaches 64 characters"), LongDisplayName.Len(), 64);
	TestEqual(TEXT("its canonical folder leaves room for the SKM_ prefix"), MantleWeaponBuilderLogic::SafeAssetName(LongDisplayName).Len(), 60);
	UMantleWeaponSource* LongInput = Source(EMantleWeaponHold::Rifle, LongDisplayName, Cube);
	LongInput->Delivery = EMantleWeaponSourceDelivery::Projectile;
	LongInput->Projectile.Model = Cube;
	LongInput->Attachments.AddDefaulted_GetRef().Model = Cube;
	LongInput->Attachments[0].Name = TEXT("BoundaryDetail");
	LongInput->FireSound = Sound;
	LongInput->FireSuppressedSound = Sound;
	LongInput->bSuppressed = true;
	Messages.Reset();
	if (!TestTrue(TEXT("a boundary-length source exports without colliding with its own generated names"), FMantleWeaponBuilderExport::Export(*LongInput, Messages)))
	{
		for (const FText& Message : Messages) AddError(Message.ToString());
		return false;
	}
	FMantleWeaponDesc LongDesc;
	if (!TestTrue(TEXT("the boundary-length export document parses"), Read(LongFixture, LongDesc, Error))) { AddError(Error); return false; }
	const FString LongStem = MantleWeaponBuilderLogic::SafeAssetName(LongInput->DisplayName).Left(60);
	const FString ExpectedMeshName = TEXT("SKM_") + LongStem;
	TestEqual(TEXT("the generated mesh local reference fits and matches its saved object name"), LongDesc.Mesh, ExpectedMeshName);
	TestTrue(TEXT("the generated mesh reference is at most 64 characters"), LongDesc.Mesh.Len() <= 64);
	TestTrue(TEXT("the long mesh asset resolves from its package folder"), LoadObject<USkeletalMesh>(nullptr,
		*(LongFixture.PackageFolder() / (ExpectedMeshName + TEXT(".") + ExpectedMeshName))) != nullptr);
	const FString LongAttachment = LongDesc.Attachments.Value[0].Mesh;
	const FString LongFire = LongDesc.Sounds.Value.Fire.Value;
	const FString LongQuiet = LongDesc.Sounds.Value.FireSuppressed.Value;
	for (const FString& LocalName : { LongAttachment, LongFire, LongQuiet })
	{
		TestTrue(TEXT("each generated auxiliary local reference fits the document limit"), LocalName.Len() <= 64);
		TestTrue(*FString::Printf(TEXT("generated asset %s resolves from its local name"), *LocalName),
			LoadObject<UObject>(nullptr, *(LongFixture.PackageFolder() / (LocalName + TEXT(".") + LocalName))) != nullptr);
	}
	TestTrue(TEXT("the attachment reference keeps its kind and index"), LongAttachment.EndsWith(TEXT("_Attachment_0")));
	TestTrue(TEXT("the fire sound reference keeps its kind and index"), LongFire.EndsWith(TEXT("_Fire_0")));
	TestTrue(TEXT("the quiet sound reference keeps its kind and index"), LongQuiet.EndsWith(TEXT("_FireSuppressed_0")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleWeaponBuilderV2RecoveryTest, "Mantle.Editor.WeaponBuilderV2.FailedAuxiliaryRebuildIsRecoverable", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleWeaponBuilderV2RecoveryTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponBuilderV2TestPrivate;
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (!TestNotNull(TEXT("fixture cube"), Cube) || !TestNotNull(TEXT("fixture sphere"), Sphere)) return false;
	FIsolatedExport Fixture;
	UMantleWeaponSource* Input = Source(EMantleWeaponHold::Rifle, Fixture.Name, Cube);
	Input->Delivery = EMantleWeaponSourceDelivery::Projectile;
	Input->Projectile.Model = Cube;
	FMantleWeaponSourceAttachment Attachment;
	Attachment.Name = TEXT("Tip"); Attachment.Model = Cube; Attachment.Socket = TEXT("Muzzle");
	Input->Attachments.Add(Attachment);
	TArray<FText> Messages;
	if (!TestTrue(TEXT("initial complete export"), FMantleWeaponBuilderExport::Export(*Input, Messages))) return false;
	FMantleWeaponDesc Desc;
	FString Error;
	if (!TestTrue(TEXT("initial document"), Read(Fixture, Desc, Error))) return false;
	const FString ProjectileFile = Fixture.Folder() / (Fixture.Name + TEXT("_Projectile_0.uasset"));
	const FString AttachmentFile = Fixture.Folder() / (Desc.Attachments.Value[0].Mesh + TEXT(".uasset"));
	TArray<uint8> PreviousProjectile, RestoredProjectile;
	if (!TestTrue(TEXT("initial projectile bytes"), FFileHelper::LoadFileToArray(PreviousProjectile, *ProjectileFile))) return false;
	struct FRestoreWritable { FString File; ~FRestoreWritable() { FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*File, false); } } Restore{ AttachmentFile };
	if (!TestTrue(TEXT("fixture output locked against writing"), FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*AttachmentFile, true))) return false;
	// The projectile copy succeeds first. Only the subsequent attachment save is deliberately made to fail.
	Input->Projectile.Model = Sphere;
	Input->Tuning.Damage = 50.f;
	Messages.Reset();
	AddExpectedError(TEXT("as it is read only"), EAutomationExpectedErrorFlags::Contains, 1);
	TestFalse(TEXT("failed multi-asset rebuild refused"), FMantleWeaponBuilderExport::Export(*Input, Messages));
	FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*AttachmentFile, false);
	TestTrue(TEXT("previous projectile bytes restored after earlier successful copy"), FFileHelper::LoadFileToArray(RestoredProjectile, *ProjectileFile) && RestoredProjectile == PreviousProjectile);
	bool bRecoveryNotice = false, bSpecificFailure = false;
	for (const auto& Message : Messages)
	{
		bRecoveryNotice |= Message.ToString().Contains(TEXT("reload the project"), ESearchCase::IgnoreCase);
		bSpecificFailure |= Message.ToString().Contains(TEXT("Could not save"));
	}
	TestTrue(TEXT("creator gets asset save failure and truthful reload requirement"), bRecoveryNotice && bSpecificFailure);
	FMantleWeaponDesc Restored;
	TestTrue(TEXT("prior document still usable on disk"), Read(Fixture, Restored, Error));
	TestEqual(TEXT("prior tuning retained on disk"), Restored.Fire.Damage.Value, Desc.Fire.Damage.Value);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleWeaponBuilderV2ValidationTest, "Mantle.Editor.WeaponBuilderV2.InvalidChoicesAndLegacyDefaults", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleWeaponBuilderV2ValidationTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponBuilderV2TestPrivate;
	UMantleWeaponSource* Input = NewObject<UMantleWeaponSource>();
	TestEqual(TEXT("saved firearm enum value"), static_cast<uint8>(EMantleWeaponHold::Rifle), uint8(2));
	FMantleWeaponDesc Desc;
	TArray<FText> Messages;
	if (!TestTrue(TEXT("legacy rifle source defaults export as v2"), FMantleWeaponBuilderExport::BuildDocument(*Input, RequiredPoints(Input->Hold), TEXT("Legacy"), Desc, Messages)))
	{
		for (const auto& Message : Messages) AddError(Message.ToString());
		return false;
	}
	TestTrue(TEXT("legacy remains hitscan magazine"), Desc.GetDelivery() == EMantleWeaponDocDelivery::Hitscan && Desc.GetResource() == EMantleWeaponDocResource::Magazine);
	TestEqual(TEXT("legacy rifle range"), Desc.Fire.MaxRange.Value, 25000.0);
	TestEqual(TEXT("legacy rifle recoil"), Desc.Fire.PlainRecoil.Value, 0.35);
	Input->Hold = EMantleWeaponHold::Pistol;
	Messages.Reset();
	TestTrue(TEXT("legacy pistol defaults valid"), FMantleWeaponBuilderExport::BuildDocument(*Input, RequiredPoints(Input->Hold), TEXT("Legacy"), Desc, Messages));
	TestEqual(TEXT("legacy pistol range"), Desc.Fire.MaxRange.Value, 15000.0);
	TestEqual(TEXT("legacy pistol recoil"), Desc.Fire.PlainRecoil.Value, 0.5);
	Input->Hold = EMantleWeaponHold::Melee1h;
	Input->Delivery = EMantleWeaponSourceDelivery::Hitscan;
	Messages.Reset();
	TestFalse(TEXT("blade cannot silently become hitscan"), FMantleWeaponBuilderExport::BuildDocument(*Input, RequiredPoints(Input->Hold), TEXT("Blade"), Desc, Messages));
	TestTrue(TEXT("creator sees why"), !Messages.IsEmpty() && Messages[0].ToString().Contains(TEXT("delivery")));
	Input->Hold = EMantleWeaponHold::Rifle;
	Input->Delivery = EMantleWeaponSourceDelivery::Default;
	Input->Tuning.Pellets = 33;
	Messages.Reset();
	TestFalse(TEXT("invalid saved tuning refused without clamping"), FMantleWeaponBuilderExport::BuildDocument(*Input, RequiredPoints(Input->Hold), TEXT("Rifle"), Desc, Messages));
	Input->Tuning.Pellets = 1;
	Input->Charge.bEnabled = true;
	Messages.Reset();
	TestFalse(TEXT("firearm charge refused"), FMantleWeaponBuilderExport::BuildDocument(*Input, RequiredPoints(Input->Hold), TEXT("Rifle"), Desc, Messages));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleWeaponBuilderV2SignatureTest, "Mantle.Editor.WeaponBuilderV2.CompleteEditSignature", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleWeaponBuilderV2SignatureTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponBuilderLogic;
	using namespace MantleWeaponBuilderV2TestPrivate;
	UMantleWeaponSource* Input = NewObject<UMantleWeaponSource>();
	const FString Baseline = EditSignature(*Input);
	UMantleWeaponSource* Copy = DuplicateObject<UMantleWeaponSource>(Input, GetTransientPackage());
	TestEqual(TEXT("source identity is irrelevant"), EditSignature(*Copy), Baseline);
	auto Changed = [&](const TCHAR* Label) { TestFalse(Label, EditSignature(*Input) == Baseline); };
	Input->Delivery = EMantleWeaponSourceDelivery::Beam; Changed(TEXT("delivery changes signature")); Input->Delivery = EMantleWeaponSourceDelivery::Default;
	Input->Resource = EMantleWeaponSourceResource::None; Changed(TEXT("resource changes signature")); Input->Resource = EMantleWeaponSourceResource::Default;
	Input->EquipStyle = TEXT("back"); Changed(TEXT("equip style changes signature")); Input->EquipStyle = NAME_None;
	Input->DodgeStyle = TEXT("dash"); Changed(TEXT("dodge style changes signature")); Input->DodgeStyle = NAME_None;
	Input->ComboSet = TEXT("spin"); Changed(TEXT("combo set changes signature")); Input->ComboSet = NAME_None;
	Input->Quiver = TEXT("hip"); Changed(TEXT("quiver changes signature")); Input->Quiver = NAME_None;
	Input->bBash = false; Changed(TEXT("bash changes signature")); Input->bBash = true;
	Input->BashDamage = 35.0f; Changed(TEXT("bash damage changes signature")); Input->BashDamage = 20.0f;
	Input->bShieldThrow = true; Changed(TEXT("shield throw changes signature")); Input->bShieldThrow = false;
	Input->ShieldThrowRange = 1500.0f; Changed(TEXT("shield throw range changes signature")); Input->ShieldThrowRange = 1000.0f;
	Input->ShieldThrowDamage = 40.0f; Changed(TEXT("shield throw damage changes signature")); Input->ShieldThrowDamage = 25.0f;
	Input->ShootingStyle = TEXT("c"); Changed(TEXT("shooting style changes signature")); Input->ShootingStyle = NAME_None;
	Input->bOverrideStyle = true; Changed(TEXT("style choice changes signature")); Input->bOverrideStyle = false;
	Input->bOverrideColor = true; Changed(TEXT("color choice changes signature")); Input->bOverrideColor = false;
	Input->Model = FSoftObjectPath(TEXT("/Engine/BasicShapes/Cube.Cube")); Changed(TEXT("selected asset changes signature")); Input->Model.Reset();
	Input->Charge.Seconds += 0.00001f; Changed(TEXT("charge precision changes signature")); Input->Charge = {};
	Input->Beam.Radius = 2.f; Changed(TEXT("beam changes signature")); Input->Beam = {};
	Input->BowStringRadiusCm = 2.f; Changed(TEXT("bow build changes signature")); Input->BowStringRadiusCm = 1.5f;
	Input->Parts.AddDefaulted(); Changed(TEXT("parts change signature")); Input->Parts.Reset();
	Input->Attachments.AddDefaulted(); Changed(TEXT("attachments change signature")); Input->Attachments.Reset();
	Input->FireSoundTrigger = TEXT("Shot"); Changed(TEXT("sounds change signature")); Input->FireSoundTrigger.Reset();
	FMantleBuilderPoint Point;
	Point.Role = TEXT("Grip_R"); Point.Transform = FTransform::Identity; Point.Guess = FTransform::Identity;
	Input->Points.Add(Point);
	const FString PointSignature = EditSignature(*Input);
	Input->Points[0].Guess.SetLocation(FVector(20.0)); Input->Points[0].Source = EMantleBuilderPointSource::Creator;
	TestEqual(TEXT("point editor metadata is irrelevant"), EditSignature(*Input), PointSignature);
	Input->Points[0].Transform.SetLocation(FVector(0.000001, 0.0, 0.0));
	TestFalse(TEXT("small point edit invalidates local test"), CanTestLocally(PointSignature, EditSignature(*Input)));
	// A selected asset can change its exported contents without changing its path or this source's properties.
	FIsolatedExport Fixture;
	USoundWave* Original = LoadObject<USoundWave>(nullptr, TEXT("/Engine/EditorSounds/Notifications/CompileSuccess.CompileSuccess"));
	if (!TestNotNull(TEXT("saved signature sound fixture"), Original)) return false;
	const FString PackageName = Fixture.PackageFolder() / TEXT("SelectedSound");
	UPackage* Package = CreatePackage(*PackageName);
	USoundWave* Selected = DuplicateObject<USoundWave>(Original, Package, TEXT("SelectedSound"));
	if (!TestNotNull(TEXT("isolated selected sound"), Selected)) return false;
	Selected->SetFlags(RF_Public | RF_Standalone);
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	IFileManager::Get().MakeDirectory(*Fixture.Folder(), true);
	FSavePackageArgs Save;
	Save.TopLevelFlags = RF_Public | RF_Standalone;
	Save.SaveFlags = SAVE_NoError;
	if (!TestTrue(TEXT("selected sound initially saved"), UPackage::SavePackage(Package, Selected, *Filename, Save))) return false;
	Input->FireSound = Selected;
	const FString BeforeAssetEdit = EditSignature(*Input);
	Selected->Volume = Selected->Volume == 0.125f ? 0.25f : 0.125f;
	Package->MarkPackageDirty();
	if (!TestTrue(TEXT("selected sound edit saved at the same path"), UPackage::SavePackage(Package, Selected, *Filename, Save))) return false;
	TestFalse(TEXT("saved asset content edit invalidates local test"), CanTestLocally(BeforeAssetEdit, EditSignature(*Input)));
	return true;
}

// Every gun hold offers the flying round and the beam beside its instant shot. Picking a beam starts from a laser rifle's numbers
// and the laser element; its thickness and reach reach the document only when set, and the document passes the reader. Going back to
// the instant shot drops the element, which the reader refuses there.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleWeaponBuilderV2GunBeamsTest, "Mantle.Editor.WeaponBuilderV2.GunBeamsAndRounds", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleWeaponBuilderV2GunBeamsTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponBuilderV2TestPrivate;
	using namespace MantleWeaponBuilderLogic;
	// Build expects the document to build and names why when it does not; Refused expects a refusal.
	auto Refused = [](const UMantleWeaponSource& In, FMantleWeaponDesc& Out, TArray<FText>& Messages)
	{
		return !FMantleWeaponBuilderExport::BuildDocument(In, RequiredPoints(In.Hold), TEXT("FixtureMesh"), Out, Messages);
	};
	auto Build = [this](const UMantleWeaponSource& In, FMantleWeaponDesc& Out, TArray<FText>& Messages)
	{
		const bool bBuilt = FMantleWeaponBuilderExport::BuildDocument(In, RequiredPoints(In.Hold), TEXT("FixtureMesh"), Out, Messages);
		if (!bBuilt)
		{
			for (const FText& Message : Messages) AddError(Message.ToString());
		}
		return bBuilt;
	};
	for (uint8 Index = 0; Index <= static_cast<uint8>(EMantleWeaponHold::Lmg); ++Index)
	{
		const auto Hold = static_cast<EMantleWeaponHold>(Index);
		const EMantleWeaponDocHold DocHold = ToDocHold(Hold);
		const FString HoldName = FMantleWeaponDocument::HoldName(DocHold);
		const auto& Info = FMantleWeaponDocument::GetHoldInfo(DocHold);
		TestTrue(*FString::Printf(TEXT("%s: a gun hold"), *HoldName), FMantleWeaponDocument::IsFirearmHold(DocHold) && Info.bHasEffects);
		for (const EMantleWeaponDocDelivery Delivery : { EMantleWeaponDocDelivery::Hitscan, EMantleWeaponDocDelivery::Projectile, EMantleWeaponDocDelivery::Beam })
		{
			TestTrue(*FString::Printf(TEXT("%s: the reader allows %s"), *HoldName, FMantleWeaponDocument::DeliveryName(Delivery)), Info.Deliveries.Contains(Delivery));
			TestTrue(*FString::Printf(TEXT("%s: the panel offers %s"), *HoldName, FMantleWeaponDocument::DeliveryName(Delivery)), IsDeliveryOffered(DocHold, Delivery));
		}
		TestFalse(*FString::Printf(TEXT("%s: the instant shot keeps the firearm styles"), *HoldName), PicksElement(Hold, EMantleWeaponDocDelivery::Hitscan));
		TestTrue(*FString::Printf(TEXT("%s: a flying round picks an element"), *HoldName), PicksElement(Hold, EMantleWeaponDocDelivery::Projectile));
		TestTrue(*FString::Printf(TEXT("%s: a beam picks an element"), *HoldName), PicksElement(Hold, EMantleWeaponDocDelivery::Beam));

		// The beam: a laser rifle's numbers and the laser element.
		UMantleWeaponSource* Input = Source(Hold, TEXT("Laser"), nullptr);
		SetDelivery(*Input, EMantleWeaponSourceDelivery::Beam);
		TestTrue(*FString::Printf(TEXT("%s: a beam starts from a laser rifle's numbers"), *HoldName), Input->Beam.TickRate == 10.f && Input->Beam.DamagePerSecond == 40.f
			&& Input->Beam.MaxSeconds == 5.f && Input->Beam.Radius == 0.f && Input->Beam.Range == 3000.f);
		TestTrue(*FString::Printf(TEXT("%s: a beam starts as a laser"), *HoldName), Input->bOverrideStyle && Input->Style == EMantleWeaponSourceStyle::Laser);
		FMantleWeaponDesc Desc;
		TArray<FText> Messages;
		if (!TestTrue(*FString::Printf(TEXT("%s: a laser builds"), *HoldName), Build(*Input, Desc, Messages)))
		{
			continue;
		}
		EMantleWeaponDocEffects Element = EMantleWeaponDocEffects::Arcane;
		TestTrue(TEXT("its beam"), Desc.GetDelivery() == EMantleWeaponDocDelivery::Beam && Desc.Beam.bSet);
		TestTrue(TEXT("its laser element"), Desc.GetElement(Element) && Element == EMantleWeaponDocEffects::Laser);
		TestTrue(TEXT("its reach"), Desc.Beam.Value.Range.bSet && FMath::IsNearlyEqual(Desc.Beam.Value.Range.Value, 3000.0));
		TestFalse(TEXT("a thin line writes no thickness"), Desc.Beam.Value.Radius.bSet);
		TestEqual(TEXT("its damage per second"), Desc.Beam.Value.DamagePerSecond, 40.0);

		// A flamethrower: thick and short. Both reach the document, which the reader takes.
		Input->Style = EMantleWeaponSourceStyle::Fire;
		Input->Beam.Radius = 40.f;
		Input->Beam.Range = 800.f;
		Messages.Reset();
		if (TestTrue(TEXT("a flamethrower builds"), Build(*Input, Desc, Messages)))
		{
			TestTrue(TEXT("its thickness"), Desc.Beam.Value.Radius.bSet && FMath::IsNearlyEqual(Desc.Beam.Value.Radius.Value, 40.0));
			TestTrue(TEXT("its reach"), Desc.Beam.Value.Range.bSet && FMath::IsNearlyEqual(Desc.Beam.Value.Range.Value, 800.0));
			TArray<FMantleWeaponDocError> Errors;
			TestTrue(TEXT("the reader takes the flamethrower"), FMantleWeaponDocument::Validate(Desc, Errors));
			FMantleWeaponDesc Reread;
			FString Error;
			TestTrue(TEXT("its written file reads back"), FMantleWeaponDocument::Parse(FMantleWeaponDocument::Write(Desc), Reread, Error)
				&& Reread.Beam.bSet && FMath::IsNearlyEqual(Reread.Beam.Value.Radius.Value, 40.0) && FMath::IsNearlyEqual(Reread.Beam.Value.Range.Value, 800.0));
		}
		// A reach the reader refuses is refused, never clamped.
		Input->Beam.Range = 50.f;
		Messages.Reset();
		TestTrue(TEXT("a reach under 1 m is refused"), Refused(*Input, Desc, Messages));
		Input->Beam.Range = 800.f;

		// The flying round: a launcher's numbers; the element stays.
		SetDelivery(*Input, EMantleWeaponSourceDelivery::Projectile);
		TestTrue(TEXT("a round starts from a launcher's numbers"), Input->Projectile.Speed == 4000.f && Input->Projectile.Gravity == 0.5f && Input->Projectile.BlastRadius == 250.f);
		Messages.Reset();
		if (TestTrue(TEXT("a fire launcher builds"), Build(*Input, Desc, Messages)))
		{
			TestTrue(TEXT("its round"), Desc.Projectile.bSet && !Desc.Beam.bSet && FMath::IsNearlyEqual(Desc.Projectile.Value.BlastRadius.Value, 250.0));
			TestTrue(TEXT("its fire element"), Desc.GetElement(Element) && Element == EMantleWeaponDocEffects::Fire);
			TestFalse(TEXT("no round model, no projectile mesh"), Desc.Projectile.Value.Mesh.bSet);
		}

		// Back to the instant shot: the element goes, and the gun builds with its firearm style.
		SetDelivery(*Input, EMantleWeaponSourceDelivery::Hitscan);
		TestFalse(TEXT("the instant shot drops the element"), Input->bOverrideStyle);
		TestFalse(TEXT("the style in hand is a firearm style"), FMantleWeaponDocument::IsElement(static_cast<EMantleWeaponDocEffects>(Input->Style)));
		Messages.Reset();
		if (TestTrue(TEXT("the instant shot builds"), Build(*Input, Desc, Messages)))
		{
			TestFalse(TEXT("with no element"), Desc.GetElement(Element));
			TestFalse(TEXT("and no beam"), Desc.Beam.bSet);
		}
		// A saved instant-shot gun that still names an element is refused in plain words, before the reader sees it.
		Input->bOverrideStyle = true;
		Input->Style = EMantleWeaponSourceStyle::Laser;
		Messages.Reset();
		TestTrue(TEXT("an element on the instant shot is refused"), Refused(*Input, Desc, Messages));
		TestTrue(TEXT("the creator reads why"), !Messages.IsEmpty() && Messages[0].ToString().Contains(TEXT("flying round or a beam")));
	}

	// A staff's held beam carries the thickness and reach too.
	UMantleWeaponSource* Staff = Source(EMantleWeaponHold::Staff, TEXT("Staff"), nullptr);
	Staff->Beam.Radius = 12.f;
	Staff->Beam.Range = 1500.f;
	FMantleWeaponDesc StaffDesc;
	TArray<FText> StaffMessages;
	if (TestTrue(TEXT("a thick staff beam builds"), Build(*Staff, StaffDesc, StaffMessages)))
	{
		TestTrue(TEXT("the staff's thickness"), StaffDesc.Beam.bSet && StaffDesc.Beam.Value.Radius.bSet && FMath::IsNearlyEqual(StaffDesc.Beam.Value.Radius.Value, 12.0));
		TestTrue(TEXT("the staff's reach"), StaffDesc.Beam.Value.Range.bSet && FMath::IsNearlyEqual(StaffDesc.Beam.Value.Range.Value, 1500.0));
	}
	return true;
}

#endif
