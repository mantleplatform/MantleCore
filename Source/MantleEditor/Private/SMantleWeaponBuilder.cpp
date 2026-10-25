// Copyright Joshua Eger. All Rights Reserved.
#include "SMantleWeaponBuilder.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetThumbnail.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/FileManager.h"
#include "MantlePreviewExport.h"
#include "MantleTestLocally.h"
#include "MantleWeaponPublish.h"
#include "MantleWeaponBuilderExport.h"
#include "MantleWeaponBuilderLogic.h"
#include "MantleWeaponBuilderPanel.h"
#include "MantleWeaponDocument.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/MessageDialog.h"
#include "PropertyCustomizationHelpers.h"
#include "SMantleBuilderViewport.h"
#include "Styling/AppStyle.h"
#include "ThumbnailRendering/ThumbnailManager.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Sound/SoundWave.h"
#include "MantleModelThumbnail.h"
#include "MantleThumbnailStore.h"
#include "SMantleThumbnailSection.h"

#define LOCTEXT_NAMESPACE "MantleWeaponBuilder"

const FName FMantleWeaponBuilderPanel::TabName(TEXT("MantleWeaponBuilder"));

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleWeaponBuilderPanelPrivate
{
	TSharedRef<SDockTab> SpawnWeaponBuilderTab(const FSpawnTabArgs&)
	{
		return SNew(SDockTab)
			.TabRole(ETabRole::NomadTab)
			.Label(LOCTEXT("TabLabel", "Build a weapon"))
			[
				SNew(SMantleWeaponBuilder)
			];
	}

	FLinearColor GoodColour() { return FLinearColor(0.55f, 0.80f, 0.55f); }
	FLinearColor WarnColour() { return FLinearColor(0.95f, 0.65f, 0.25f); }

	const FMantleWeaponDocHoldInfo& HoldInfo(EMantleWeaponHold Hold)
	{
		return FMantleWeaponDocument::GetHoldInfo(MantleWeaponBuilderLogic::ToDocHold(Hold));
	}

	EMantleWeaponDocDelivery EffectiveDelivery(const UMantleWeaponSource& Source)
	{
		return Source.Delivery == EMantleWeaponSourceDelivery::Default
			? MantleWeaponBuilderLogic::UsualDelivery(MantleWeaponBuilderLogic::ToDocHold(Source.Hold))
			: static_cast<EMantleWeaponDocDelivery>(static_cast<uint8>(Source.Delivery) - 1);
	}

	EMantleWeaponDocResource EffectiveResource(const UMantleWeaponSource& Source)
	{
		const FMantleWeaponDocHoldInfo& Info = HoldInfo(Source.Hold);
		return Source.Resource == EMantleWeaponSourceResource::Default
			? Info.Resources[0]
			: static_cast<EMantleWeaponDocResource>(static_cast<uint8>(Source.Resource) - 1);
	}

	/** #905: a gun's launched round and beam say what they make (a grenade launcher, a laser rifle) in its own words. */
	FText DeliveryLabel(EMantleWeaponSourceDelivery Delivery, bool bGun = false)
	{
		switch (Delivery)
		{
		case EMantleWeaponSourceDelivery::Default: return LOCTEXT("DeliveryDefault", "Usual for this hold");
		case EMantleWeaponSourceDelivery::Hitscan: return LOCTEXT("DeliveryHitscan", "Instant shot");
		case EMantleWeaponSourceDelivery::Projectile: return bGun
			? LOCTEXT("DeliveryProjectileGun", "Flying round (grenade or rocket launcher)")
			: LOCTEXT("DeliveryProjectile", "Flying projectile");
		case EMantleWeaponSourceDelivery::Sweep: return LOCTEXT("DeliverySweep", "Melee swing");
		case EMantleWeaponSourceDelivery::Beam: return bGun
			? LOCTEXT("DeliveryBeamGun", "Beam (laser, flamethrower)")
			: LOCTEXT("DeliveryBeam", "Held beam");
		case EMantleWeaponSourceDelivery::Use: return LOCTEXT("DeliveryUse", "Use it up");
		}
		return FText::GetEmpty();
	}

	FText ResourceLabel(EMantleWeaponSourceResource Resource)
	{
		switch (Resource)
		{
		case EMantleWeaponSourceResource::Default: return LOCTEXT("ResourceDefault", "Usual for this hold");
		case EMantleWeaponSourceResource::Magazine: return LOCTEXT("ResourceMagazine", "Magazine");
		case EMantleWeaponSourceResource::Quiver: return LOCTEXT("ResourceQuiver", "Quiver");
		case EMantleWeaponSourceResource::Charges: return LOCTEXT("ResourceCharges", "Charges");
		case EMantleWeaponSourceResource::None: return LOCTEXT("ResourceNone", "No ammunition");
		}
		return FText::GetEmpty();
	}

	FText StyleLabel(EMantleWeaponSourceStyle Style)
	{
		switch (Style)
		{
		case EMantleWeaponSourceStyle::Rifle: return LOCTEXT("StyleRifle", "Rifle");
		case EMantleWeaponSourceStyle::Pistol: return LOCTEXT("StylePistol", "Pistol");
		case EMantleWeaponSourceStyle::Shotgun: return LOCTEXT("StyleShotgun", "Shotgun");
		case EMantleWeaponSourceStyle::Laser: return LOCTEXT("StyleLaser", "Laser");
		case EMantleWeaponSourceStyle::Fire: return LOCTEXT("StyleFire", "Fire");
		case EMantleWeaponSourceStyle::Ice: return LOCTEXT("StyleIce", "Ice");
		case EMantleWeaponSourceStyle::Lightning: return LOCTEXT("StyleLightning", "Lightning");
		case EMantleWeaponSourceStyle::Arcane: return LOCTEXT("StyleArcane", "Arcane");
		case EMantleWeaponSourceStyle::Venom: return LOCTEXT("StyleVenom", "Venom");
		case EMantleWeaponSourceStyle::Water: return LOCTEXT("StyleWater", "Water");
		case EMantleWeaponSourceStyle::Wind: return LOCTEXT("StyleWind", "Wind");
		case EMantleWeaponSourceStyle::Rock: return LOCTEXT("StyleRock", "Rock");
		case EMantleWeaponSourceStyle::Sand: return LOCTEXT("StyleSand", "Sand");
		case EMantleWeaponSourceStyle::Void: return LOCTEXT("StyleVoid", "Void");
		case EMantleWeaponSourceStyle::Dark: return LOCTEXT("StyleDark", "Dark");
		case EMantleWeaponSourceStyle::Light: return LOCTEXT("StyleLight", "Light");
		case EMantleWeaponSourceStyle::Generic: return LOCTEXT("StyleGeneric", "Generic");
		case EMantleWeaponSourceStyle::Heal: return LOCTEXT("StyleHeal", "Heal");
		case EMantleWeaponSourceStyle::Lava: return LOCTEXT("StyleLava", "Lava");
		case EMantleWeaponSourceStyle::Nature: return LOCTEXT("StyleNature", "Nature");
		}
		return FText::GetEmpty();
	}

	FText BuilderHoldBlurb(EMantleWeaponHold Hold)
	{
		switch (Hold)
		{
		case EMantleWeaponHold::Pistol: return LOCTEXT("BlurbPistol", "A small gun held in one hand.");
		case EMantleWeaponHold::Revolver: return LOCTEXT("BlurbRevolver", "A heavy one-handed gun with a few strong shots.");
		case EMantleWeaponHold::Rifle: return LOCTEXT("BlurbRifle", "A long gun held with two hands and raised to aim.");
		case EMantleWeaponHold::Smg: return LOCTEXT("BlurbSmg", "A short, fast two-handed gun.");
		case EMantleWeaponHold::Dmr: return LOCTEXT("BlurbDmr", "A long, accurate gun for single shots at range.");
		case EMantleWeaponHold::PumpShotgun: return LOCTEXT("BlurbShotgun", "A two-handed shotgun you load one shell at a time.");
		case EMantleWeaponHold::BoltSniper: return LOCTEXT("BlurbSniper", "A long gun with a bolt and powerful shots.");
		case EMantleWeaponHold::Lmg: return LOCTEXT("BlurbLmg", "A large two-handed gun that fires for a long time.");
		case EMantleWeaponHold::Melee1h: return LOCTEXT("BlurbMelee1h", "A one-handed blade for quick swings and heavy strikes.");
		case EMantleWeaponHold::Melee2h: return LOCTEXT("BlurbMelee2h", "A large blade held in both hands.");
		case EMantleWeaponHold::Spear: return LOCTEXT("BlurbSpear", "A long weapon for thrusts.");
		case EMantleWeaponHold::Staff: return LOCTEXT("BlurbStaff", "A casting weapon that fires a held beam, a few casts at a time.");
		case EMantleWeaponHold::Bow: return LOCTEXT("BlurbBow", "Hold to draw, then release an arrow from a two-handed bow.");
		case EMantleWeaponHold::Unarmed: return LOCTEXT("BlurbUnarmed", "Knuckles, claws or a gauntlet worn on the hand; it plays the fist animations. Empty hands are already built in.");
		case EMantleWeaponHold::Shield: return LOCTEXT("BlurbShield", "A left-hand shield for blocking and bashing.");
		case EMantleWeaponHold::Throwable: return LOCTEXT("BlurbThrowable", "A one-handed item thrown at a target.");
		case EMantleWeaponHold::Consumable: return LOCTEXT("BlurbConsumable", "A potion, medkit or snack the character drinks, eats or uses up to heal.");
		case EMantleWeaponHold::Count: break;
		}
		return FText::GetEmpty();
	}

	FString FriendlyFitWords(FString Text)
	{
		Text = MantleWeaponBuilderLogic::PlainWords(Text);
		Text.ReplaceInline(TEXT("gun"), TEXT("weapon"), ESearchCase::IgnoreCase);
		return Text;
	}

	FText FriendlyRoleHelp(const FMantleBuilderPointRole& Role, EMantleWeaponHold Hold)
	{
		if (Role.Role == FName(TEXT("Grip")) && Hold == EMantleWeaponHold::Shield)
		{
			return LOCTEXT("ShieldGripHelp", "Place this point in the middle of the left palm where the hand holds the shield.");
		}
		return FText::FromString(FriendlyFitWords(Role.Help.ToString()));
	}

	FText PartRoleLabel(EMantleWeaponSourcePartRole Role)
	{
		switch (Role)
		{
		case EMantleWeaponSourcePartRole::Magazine: return LOCTEXT("PartMagazine", "Magazine");
		case EMantleWeaponSourcePartRole::Round: return LOCTEXT("PartRound", "Round or shell");
		case EMantleWeaponSourcePartRole::Arrow: return LOCTEXT("PartArrow", "Arrow");
		}
		return FText::GetEmpty();
	}

	FText FriendlyRoleLabel(const FMantleBuilderPointRole& Role, EMantleWeaponHold Hold)
	{
		const FString Name = Role.Role.ToString();
		if (Name == TEXT("Grip_R")) return LOCTEXT("RoleRightHand", "Right hand");
		if (Name == TEXT("Grip_L")) return LOCTEXT("RoleLeftHand", "Left hand");
		if (Name == TEXT("Grip")) return Hold == EMantleWeaponHold::Shield ? LOCTEXT("RoleShieldGrip", "Left hand on shield") : FText::FromString(Name);
		if (Name == TEXT("EdgeBase")) return LOCTEXT("RoleEdgeBase", "Blade base");
		if (Name == TEXT("EdgeTip")) return LOCTEXT("RoleEdgeTip", "Blade tip");
		if (Name == TEXT("Tip")) return LOCTEXT("RoleTip", "Weapon point");
		if (Name == TEXT("Focus")) return LOCTEXT("RoleFocus", "Cast point");
		if (Name == TEXT("StringTop")) return LOCTEXT("RoleStringTop", "Top of string");
		if (Name == TEXT("StringBottom")) return LOCTEXT("RoleStringBottom", "Bottom of string");
		if (Name == TEXT("Nock")) return LOCTEXT("RoleNock", "Arrow position");
		if (Name == TEXT("Sight")) return LOCTEXT("RoleSight", "Aim point");
		if (Name == TEXT("Muzzle")) return LOCTEXT("RoleMuzzle", "Shot origin");
		if (Name == TEXT("Magazine")) return LOCTEXT("RoleMagazine", "Magazine position");
		if (Name == TEXT("ShellEject")) return LOCTEXT("RoleShellEject", "Case exit");
		if (Name == TEXT("Face")) return LOCTEXT("RoleFace", "Shield face");
		return FText::FromString(Name);
	}

	template<typename TValue>
	TSharedRef<SWidget> MakeChoiceControl(const TArray<TPair<TValue, FText>>& Choices, TFunction<TValue()> Get, TFunction<void(TValue)> Set)
	{
		using FChoice = TPair<TValue, FText>;
		using FItem = TSharedPtr<FChoice>;
		TSharedRef<TArray<FItem>> Items = MakeShared<TArray<FItem>>();
		for (const TPair<TValue, FText>& Choice : Choices)
		{
			Items->Add(MakeShared<FChoice>(Choice));
		}
		TSharedPtr<TArray<FItem>> KeepAlive = Items;
		return SNew(SComboBox<FItem>)
			.OptionsSource(KeepAlive.Get())
			.OnGenerateWidget_Lambda([KeepAlive](FItem Item)
			{
				return SNew(STextBlock).Text(Item.IsValid() ? Item->Value : FText::GetEmpty());
			})
			.OnSelectionChanged_Lambda([Set](FItem Item, ESelectInfo::Type)
			{
				if (Item.IsValid()) Set(Item->Key);
			})
			[
				SNew(STextBlock).Text_Lambda([KeepAlive, Get]
				{
					const TValue Current = Get();
					for (const FItem& Item : *KeepAlive)
						if (Item.IsValid() && Item->Key == Current) return Item->Value;
					return KeepAlive->IsEmpty() ? FText::GetEmpty() : KeepAlive->Last()->Value;
				})
			];
	}

	TSharedRef<SWidget> MakeVectorControl(TFunction<FVector()> Get, TFunction<void(const FVector&)> Set, float Min, float Max, float Delta)
	{
		TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			Row->AddSlot().FillWidth(1.f).Padding(Axis == 0 ? 0.f : 4.f, 0.f, 0.f, 0.f)
			[
				SNew(SSpinBox<float>)
				.MinValue(Min).MaxValue(Max).MinSliderValue(Min).MaxSliderValue(Max).Delta(Delta)
				.MinFractionalDigits(1).MaxFractionalDigits(2)
				.Value_Lambda([Get, Axis] { return static_cast<float>(Get()[Axis]); })
				.OnValueChanged_Lambda([Get, Set, Axis](float Value)
				{
					FVector Vector = Get();
					Vector[Axis] = Value;
					Set(Vector);
				})
				.OnValueCommitted_Lambda([Get, Set, Axis](float Value, ETextCommit::Type)
				{
					FVector Vector = Get();
					Vector[Axis] = Value;
					Set(Vector);
				})
			];
		}
		return Row;
	}

	TSharedRef<SWidget> MakeMeshOrSoundPicker(TFunction<FSoftObjectPath()> Get, TFunction<void(const FSoftObjectPath&)> Set, bool bSound)
	{
		return SNew(SObjectPropertyEntryBox)
			.AllowedClass(UObject::StaticClass())
			.DisplayThumbnail(false)
			.AllowClear(true)
			.ObjectPath_Lambda([Get] { return Get().ToString(); })
			.OnShouldFilterAsset_Lambda([bSound](const FAssetData& Asset)
			{
				if (bSound)
				{
					return Asset.AssetClassPath != USoundWave::StaticClass()->GetClassPathName();
				}
				return Asset.AssetClassPath != UStaticMesh::StaticClass()->GetClassPathName()
					&& Asset.AssetClassPath != USkeletalMesh::StaticClass()->GetClassPathName();
			})
			.OnObjectChanged_Lambda([Set](const FAssetData& Asset)
			{
				Set(Asset.IsValid() ? Asset.ToSoftObjectPath() : FSoftObjectPath());
			});
	}

	FText PartActionLabel(EMantleWeaponSourceAction Action)
	{
		switch (Action)
		{
		case EMantleWeaponSourceAction::Equip: return LOCTEXT("PartActionEquip", "Equip");
		case EMantleWeaponSourceAction::Holster: return LOCTEXT("PartActionHolster", "Put away");
		case EMantleWeaponSourceAction::Idle: return LOCTEXT("PartActionIdle", "Idle");
		case EMantleWeaponSourceAction::Fire: return LOCTEXT("PartActionFire", "Fire");
		case EMantleWeaponSourceAction::FireLast: return LOCTEXT("PartActionFireLast", "Last shot");
		case EMantleWeaponSourceAction::ReloadTactical: return LOCTEXT("PartActionReload", "Reload");
		case EMantleWeaponSourceAction::ReloadEmpty: return LOCTEXT("PartActionReloadEmpty", "Reload empty");
		case EMantleWeaponSourceAction::ReloadStart: return LOCTEXT("PartActionReloadStart", "Reload starts");
		case EMantleWeaponSourceAction::ReloadStartEmpty: return LOCTEXT("PartActionReloadStartEmpty", "Empty reload starts");
		case EMantleWeaponSourceAction::ReloadLoop: return LOCTEXT("PartActionReloadLoop", "Reloading each round");
		case EMantleWeaponSourceAction::ReloadEnd: return LOCTEXT("PartActionReloadEnd", "Reload ends");
		case EMantleWeaponSourceAction::ReloadPartial: return LOCTEXT("PartActionReloadPartial", "Partial reload");
		case EMantleWeaponSourceAction::Attack: return LOCTEXT("PartActionAttack", "Attack");
		case EMantleWeaponSourceAction::Heavy: return LOCTEXT("PartActionHeavy", "Heavy attack");
		case EMantleWeaponSourceAction::Block: return LOCTEXT("PartActionBlock", "Block");
		case EMantleWeaponSourceAction::Throw: return LOCTEXT("PartActionThrow", "Throw");
		case EMantleWeaponSourceAction::Cast: return LOCTEXT("PartActionCast", "Cast");
		case EMantleWeaponSourceAction::Nock: return LOCTEXT("PartActionNock", "Nock arrow");
		case EMantleWeaponSourceAction::Draw: return LOCTEXT("PartActionDraw", "Draw bow");
		case EMantleWeaponSourceAction::Release: return LOCTEXT("PartActionRelease", "Release arrow");
		}
		return FText::GetEmpty();
	}

	bool IsFirearm(EMantleWeaponHold Hold)
	{
		return static_cast<uint8>(Hold) <= static_cast<uint8>(EMantleWeaponHold::Lmg);
	}

	bool PartRoleAllowed(EMantleWeaponHold Hold, EMantleWeaponSourcePartRole Role)
	{
		const FMantleWeaponDocHoldInfo& Info = HoldInfo(Hold);
		switch (Role)
		{
		case EMantleWeaponSourcePartRole::Magazine: return Info.Roles.Contains(EMantleWeaponDocRole::Magazine);
		case EMantleWeaponSourcePartRole::Round: return Info.StowedRoles.Contains(EMantleWeaponDocRole::Round);
		case EMantleWeaponSourcePartRole::Arrow: return Info.StowedRoles.Contains(EMantleWeaponDocRole::Arrow);
		}
		return false;
	}

	TArray<TPair<EMantleWeaponSourcePartRole, FText>> PartRoleChoices(EMantleWeaponHold Hold)
	{
		TArray<TPair<EMantleWeaponSourcePartRole, FText>> Out;
		for (uint8 Index = 0; Index <= static_cast<uint8>(EMantleWeaponSourcePartRole::Arrow); ++Index)
		{
			const auto Role = static_cast<EMantleWeaponSourcePartRole>(Index);
			if (PartRoleAllowed(Hold, Role)) Out.Emplace(Role, PartRoleLabel(Role));
		}
		return Out;
	}

	TArray<EMantleWeaponSourceAction> DefaultPartActions(EMantleWeaponHold Hold, EMantleWeaponSourcePartRole Role)
	{
		TArray<EMantleWeaponSourceAction> Out;
		for (EMantleWeaponDocAction Action : HoldInfo(Hold).Actions)
		{
			const bool bShow = Role == EMantleWeaponSourcePartRole::Arrow
				? Action == EMantleWeaponDocAction::Nock || Action == EMantleWeaponDocAction::Draw || Action == EMantleWeaponDocAction::Release
				: Action >= EMantleWeaponDocAction::ReloadTactical && Action <= EMantleWeaponDocAction::ReloadPartial;
			if (bShow) Out.Add(static_cast<EMantleWeaponSourceAction>(Action));
		}
		return Out;
	}

	TArray<EMantleWeaponSourceAction> PartActionChoices(EMantleWeaponHold Hold, EMantleWeaponSourcePartRole Role)
	{
		if (!HoldInfo(Hold).StowedRoles.Contains(Role == EMantleWeaponSourcePartRole::Arrow ? EMantleWeaponDocRole::Arrow : EMantleWeaponDocRole::Round)) return {};
		return DefaultPartActions(Hold, Role);
	}

	TArray<TPair<EMantleWeaponSourceStyle, FText>> AllStyles()
	{
		TArray<TPair<EMantleWeaponSourceStyle, FText>> Out;
		for (uint8 Index = 0; Index <= static_cast<uint8>(EMantleWeaponSourceStyle::Nature); ++Index)
		{
			const auto Style = static_cast<EMantleWeaponSourceStyle>(Index);
			Out.Emplace(Style, StyleLabel(Style));
		}
		return Out;
	}

	/** #904: the elements (laser, fire, ice...) or the firearm styles (rifle, pistol, shotgun). Which a weapon picks from:
	 *  MantleWeaponBuilderLogic::PicksElement (#905: a gun's flying round or beam takes the elements too). */
	TArray<TPair<EMantleWeaponSourceStyle, FText>> StyleChoices(bool bElements)
	{
		TArray<TPair<EMantleWeaponSourceStyle, FText>> Out = AllStyles();
		Out.RemoveAll([bElements](const TPair<EMantleWeaponSourceStyle, FText>& Style)
			{ return FMantleWeaponDocument::IsElement(static_cast<EMantleWeaponDocEffects>(Style.Key)) != bElements; });
		return Out;
	}

	/** A label on the left and a control on the right, one setting per row. */
	TSharedRef<SWidget> MakeRow(const FText& Label, const FText& Tooltip, const TSharedRef<SWidget>& Control)
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 4.f, 12.f, 4.f)
			[
				SNew(SBox).WidthOverride(190.f)
				[
					SNew(STextBlock).Text(Label).ToolTipText(Tooltip)
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 4.f)
			[
				SNew(SBox).WidthOverride(220.f)
				[
					Control
				]
			];
	}

	TSharedRef<SWidget> MakeFloatSpin(float Min, float Max, float Delta, TFunction<float()> Get, TFunction<void(float)> Set)
	{
		return SNew(SSpinBox<float>)
			.MinValue(Min)
			.MaxValue(Max)
			.MinSliderValue(Min)
			.MaxSliderValue(Max)
			.Delta(Delta)
			.MinFractionalDigits(1)
			.MaxFractionalDigits(2)
			.Value_Lambda([Get] { return Get(); })
			.OnValueChanged_Lambda([Set](float Value) { Set(Value); })
			.OnValueCommitted_Lambda([Set](float Value, ETextCommit::Type) { Set(Value); });
	}

	TSharedRef<SWidget> MakeIntSpin(int32 Min, int32 Max, TFunction<int32()> Get, TFunction<void(int32)> Set)
	{
		return SNew(SSpinBox<int32>)
			.MinValue(Min)
			.MaxValue(Max)
			.MinSliderValue(Min)
			.MaxSliderValue(Max)
			.Value_Lambda([Get] { return Get(); })
			.OnValueChanged_Lambda([Set](int32 Value) { Set(Value); })
			.OnValueCommitted_Lambda([Set](int32 Value, ETextCommit::Type) { Set(Value); });
	}

	void CopySourceProperties(const UMantleWeaponSource& Source, UMantleWeaponSource& Destination)
	{
		for (TFieldIterator<FProperty> Property(UMantleWeaponSource::StaticClass(), EFieldIteratorFlags::ExcludeSuper); Property; ++Property)
		{
			Property->CopyCompleteValue_InContainer(&Destination, &Source);
		}
	}
}

void FMantleWeaponBuilderPanel::RegisterTab()
{
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(TabName, FOnSpawnTab::CreateStatic(&MantleWeaponBuilderPanelPrivate::SpawnWeaponBuilderTab))
		.SetDisplayName(LOCTEXT("TabTitle", "Build a weapon"))
		.SetTooltipText(LOCTEXT("TabTooltip", "Turn a 3D model into a weapon players can use."))
		.SetMenuType(ETabSpawnerMenuType::Hidden);
}

void FMantleWeaponBuilderPanel::UnregisterTab()
{
	if (FSlateApplication::IsInitialized())
	{
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TabName);
	}
}

void FMantleWeaponBuilderPanel::TryInvokeTab()
{
	FGlobalTabmanager::Get()->TryInvokeTab(TabName);
}

// ---------------------------------------------------------------------------------------------------------------------
// The window.

void SMantleWeaponBuilder::Construct(const FArguments& InArgs)
{
	using namespace MantleWeaponBuilderLogic;
	Working.Reset(NewObject<UMantleWeaponSource>(GetTransientPackage()));
	Working->Hold = EMantleWeaponHold::Rifle;
	Working->DisplayName = DefaultName(Working->Hold);
	Working->Tuning = DefaultTuning(Working->Hold);
	SourceBaselineSignature = EditSignature(*Working);
	Kind = MakeShared<FMantleWeaponBuilderKind>(ToFitHold(Working->Hold));
	ObjectPropertyChangedHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddSP(this, &SMantleWeaponBuilder::OnObjectPropertyChanged);

	SAssignNew(Flow, SMantleBuilderFlow)
		.Steps(MakeSteps())
		.FinishLabel(LOCTEXT("CreateWeapon", "Create weapon"))
		.OnFinish(FSimpleDelegate::CreateSP(this, &SMantleWeaponBuilder::Create));

	ChildSlot
	[
		Flow.ToSharedRef()
	];
}

SMantleWeaponBuilder::~SMantleWeaponBuilder()
{
	if (ObjectPropertyChangedHandle.IsValid())
	{
		FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(ObjectPropertyChangedHandle);
	}
}

TArray<FMantleBuilderStep> SMantleWeaponBuilder::MakeSteps()
{
	TArray<FMantleBuilderStep> Steps;

	{
		FMantleBuilderStep& Step = Steps.AddDefaulted_GetRef();
		Step.Title = LOCTEXT("StepKind", "What kind of weapon?");
		Step.Help = LOCTEXT("StepKindHelp", "Pick the closest match. This decides how the character holds your weapon. Then give it a name.");
		Step.MakeContent = [this] { return MakeKindStep(); };
		Step.CanContinue = [this](FText& OutWhyNot)
		{
			if (Working->DisplayName.TrimStartAndEnd().IsEmpty())
			{
				OutWhyNot = LOCTEXT("NeedName", "Give your weapon a name first.");
				return false;
			}
			return true;
		};
	}
	{
		FMantleBuilderStep& Step = Steps.AddDefaulted_GetRef();
		Step.Title = LOCTEXT("StepModel", "Choose the 3D model");
		Step.Help = LOCTEXT("StepModelHelp", "Pick the model you imported. Every weapon needs a 3D model.");
		Step.MakeContent = [this] { return MakeModelStep(); };
		Step.CanContinue = [this](FText& OutWhyNot)
		{
			if (Working->Model.IsNull())
			{
				OutWhyNot = MantleWeaponBuilderLogic::MissingModelMessage(Working->Hold);
				return false;
			}
			return true;
		};
	}
	{
		FMantleBuilderStep& Step = Steps.AddDefaulted_GetRef();
		Step.Title = LOCTEXT("StepSize", "Size and direction");
		Step.Help = LOCTEXT("StepSizeHelp", "Check that this weapon model is the right size and faces the right direction for its hold. The grey figure shows how it fits beside a character.");
		Step.OnEnter = [this] { Refit(); };
		Step.MakeContent = [this] { return MakeSizeStep(); };
	}
	{
		FMantleBuilderStep& Step = Steps.AddDefaulted_GetRef();
		Step.Title = LOCTEXT("StepPoints", "Place hands and action points");
		Step.Help = LOCTEXT("StepPointsHelp", "Choose a point in the list, then drag it in the picture onto the right place on your model. Hand points mark the middle of each palm. Other points mark where an aim, shot, swing, cast, arrow or shield action starts.");
		Step.OnEnter = [this] { Refit(); };
		Step.MakeContent = [this] { return MakePointsStep(); };
		Step.CanContinue = [this](FText& OutWhyNot) { return HasAllRequiredPoints(OutWhyNot); };
	}
	{
		FMantleBuilderStep& Step = Steps.AddDefaulted_GetRef();
		Step.Title = LOCTEXT("StepTuning", "Attack and effect");
		Step.Help = LOCTEXT("StepTuningHelp", "These settings start with sensible values for this kind of weapon. Change them if you like; each number stays inside its allowed range.");
		Step.MakeContent = [this] { return MakeTuningStep(); };
	}
	{
		FMantleBuilderStep& Step = Steps.AddDefaulted_GetRef();
		Step.Title = LOCTEXT("StepPartsSound", "Parts and sound");
		Step.Help = LOCTEXT("StepPartsSoundHelp", "Add moving magazine, round or arrow pieces, fixed details and sounds. Each choice is saved with this weapon.");
		Step.MakeContent = [this] { return MakePartsAndSoundStep(); };
	}
	{
		FMantleBuilderStep& Step = Steps.AddDefaulted_GetRef();
		Step.Title = FMantleThumbnailText::Heading();
		Step.Help = LOCTEXT("StepThumbnailHelp", "The picture of this item in the library. Mantle takes one for you; you can choose the angle or use your own picture.");
		Step.MakeContent = [this] { return MakeThumbnailStep(); };
	}
	{
		FMantleBuilderStep& Step = Steps.AddDefaulted_GetRef();
		Step.Title = LOCTEXT("StepCreate", "Create");
		Step.Help = LOCTEXT("StepCreateHelp", "This is what you chose. Press Create weapon to save it. You can go back to any step first.");
		Step.MakeContent = [this] { return MakeCreateStep(); };
	}
	return Steps;
}

// ---------------------------------------------------------------------------------------------------------------------
// State.

UObject* SMantleWeaponBuilder::LoadModel() const
{
	return Working->Model.IsNull() ? nullptr : Working->Model.LoadSynchronous();
}

float SMantleWeaponBuilder::ModelLengthCm() const
{
	const UObject* Model = Working->Model.IsNull() ? nullptr : Working->Model.Get();
	double Longest = 0.0;
	if (const UStaticMesh* Static = Cast<UStaticMesh>(Model))
	{
		Longest = Static->GetBounds().BoxExtent.GetMax() * 2.0;
	}
	else if (const USkeletalMesh* Skeletal = Cast<USkeletalMesh>(Model))
	{
		Longest = Skeletal->GetBounds().BoxExtent.GetMax() * 2.0;
	}
	return static_cast<float>(Longest * Working->FitScale);
}

void SMantleWeaponBuilder::SetHold(EMantleWeaponHold Hold)
{
	using namespace MantleWeaponBuilderLogic;
	using namespace MantleWeaponBuilderPanelPrivate;
	if (!IsHoldAvailable(Hold) || Working->Hold == Hold)
	{
		return;
	}
	Working->Hold = Hold;
	Kind->SetHold(ToFitHold(Hold));
	TSet<FName> ValidPointRoles;
	for (const FMantleBuilderPointRole& Role : Kind->GetRoles())
	{
		ValidPointRoles.Add(Role.Role);
	}
	Working->Points.RemoveAll([&ValidPointRoles](const FMantleBuilderPoint& Point) { return !ValidPointRoles.Contains(Point.Role); });
	for (FMantleWeaponSourceAttachment& Attachment : Working->Attachments)
	{
		if (!Attachment.Socket.IsNone() && !ValidPointRoles.Contains(Attachment.Socket))
		{
			Attachment.Socket = NAME_None;
		}
	}
	Working->Tuning = DefaultTuning(Hold);
	Working->Delivery = EMantleWeaponSourceDelivery::Default;
	Working->Resource = EMantleWeaponSourceResource::Default;
	Working->bOverrideTwoHanded = false;
	Working->bTwoHanded = false;
	Working->bOverrideStyle = false;
	Working->Style = HoldInfo(Hold).bHasEffects
		? static_cast<EMantleWeaponSourceStyle>(HoldInfo(Hold).Effects)
		: Hold == EMantleWeaponHold::Staff ? EMantleWeaponSourceStyle::Arcane : EMantleWeaponSourceStyle::Generic;
	Working->bOverrideColor = false;
	Working->Color = FColor(40, 110, 160);
	Working->Charge = FMantleWeaponSourceCharge();
	if (HoldInfo(Hold).Charge == EMantleWeaponDocChargeRule::Required)
	{
		Working->Charge.bEnabled = true;
		Working->Charge.bRequired = true;
	}
	Working->bBlock = HoldInfo(Hold).BlockRule == EMantleWeaponDocBlockRule::Required;
	Working->bBreakOnHeavy = true;
	Working->Projectile = FMantleWeaponSourceProjectile();
	Working->Sweep = FMantleWeaponSourceSweep();
	Working->Beam = FMantleWeaponSourceBeam();
	Working->bSpellsBesideBeam = false;
	Working->Charges = FMantleWeaponSourceCharges();
	Working->BowStringRadiusCm = 1.5f;
	Working->Parts.RemoveAll([Hold](const FMantleWeaponSourcePart& Part)
	{
		return !PartRoleAllowed(Hold, Part.Role);
	});
	if (!bNameEdited)
	{
		Working->DisplayName = DefaultName(Hold);
	}
}

void SMantleWeaponBuilder::LoadSourceAsset(UMantleWeaponSource* Source)
{
	using namespace MantleWeaponBuilderLogic;
	if (!Source)
	{
		return;
	}
	if (!IsHoldAvailable(Source->Hold))
	{
		return;
	}

	const FSoftObjectPath ChosenPath(Source);
	if (ChosenPath == SourceAssetPath)
	{
		return;
	}
	if (EditSignature(*Working) != SourceBaselineSignature
		&& FMessageDialog::Open(EAppMsgType::YesNo,
			LOCTEXT("DiscardWeaponEdits", "This weapon has changes that have not been created yet. Replace them with the saved weapon you selected?")) != EAppReturnType::Yes)
	{
		return;
	}

	MantleWeaponBuilderPanelPrivate::CopySourceProperties(*Source, *Working);
	SourceAssetPath = ChosenPath;
	// Treat a saved source as an existing output only when its package and asset name match the
	// path Create would use for its display name. A renamed/copied source must take the normal
	// new-name route so a display-name collision cannot redirect it into another weapon's folder.
	const FString ExpectedPackagePath = SourcePackagePath(Source->DisplayName);
	const FString ExpectedAssetName = FPackageName::GetLongPackageAssetName(ExpectedPackagePath);
	const UPackage* SourcePackage = Cast<UPackage>(Source->GetOutermost());
	const bool bHasOutputIdentity = SourcePackage
		&& SourcePackage->GetName().Equals(ExpectedPackagePath, ESearchCase::IgnoreCase)
		&& Source->GetName().Equals(ExpectedAssetName, ESearchCase::IgnoreCase);
	CreatedSafeName = bHasOutputIdentity ? SafeAssetName(Source->DisplayName) : FString();
	LoadThumbnailChoice(SafeAssetName(Source->DisplayName));
	CreatedSignature.Reset();
	bSelectedAssetEditedSinceCreate = false;
	bNameEdited = true;
	SourceBaselineSignature = EditSignature(*Working);
	Kind->SetHold(ToFitHold(Working->Hold));
	Kind->SetFlipRotation(Working->FlipRotation);
	Kind->SetScaleOverride(Working->bScaleOverridden ? Working->FitScale : 0.f);
	if (Flow.IsValid())
	{
		Flow->Refresh();
	}
}

void SMantleWeaponBuilder::SetModel(UObject* Model)
{
	Working->Model = Model;
	// A different model starts straight again at its own guessed size; the points follow the merge in Refit.
	Working->FlipRotation = FQuat::Identity;
	Working->bScaleOverridden = false;
	Refit();
}

void SMantleWeaponBuilder::SetFit(float Scale, bool bOverride)
{
	Working->FitScale = Scale;
	Working->bScaleOverridden = bOverride;
	Refit();
}

void SMantleWeaponBuilder::TurnFit(const FQuat& Turn)
{
	Working->FlipRotation = (Turn * Working->FlipRotation).GetNormalized();
	Refit();
}

void SMantleWeaponBuilder::Refit()
{
	using namespace MantleWeaponBuilderLogic;
	UObject* Model = LoadModel();
	if (!Model)
	{
		return;
	}
	Kind->SetHold(ToFitHold(Working->Hold));
	Kind->SetFlipRotation(Working->FlipRotation);
	Kind->SetScaleOverride(Working->bScaleOverridden ? Working->FitScale : 0.f);
	const TArray<FMantleBuilderPoint> Guesses = Kind->GuessPoints(Model);
	if (Guesses.Num() > 0)
	{
		Working->Points = MantleBuilderPoints::MergeGuesses(Working->Points, Guesses);
	}
	if (!Working->bScaleOverridden)
	{
		Working->FitScale = Kind->GetLastFit().Scale;
	}
	if (TSharedPtr<SMantleBuilderViewport> Viewport = SizeViewport.Pin())
	{
		Viewport->SetPoints(Working->Points);
	}
	if (TSharedPtr<SMantleBuilderViewport> Viewport = PointViewport.Pin())
	{
		Viewport->SetPoints(Working->Points);
	}
}

TSharedRef<SMantleBuilderViewport> SMantleWeaponBuilder::MakeViewport()
{
	TSharedRef<TWeakPtr<SMantleBuilderViewport>> Self = MakeShared<TWeakPtr<SMantleBuilderViewport>>();
	TSharedRef<SMantleBuilderViewport> Viewport = SNew(SMantleBuilderViewport)
		.Kind(Kind)
		.OnPointsChanged(FSimpleDelegate::CreateLambda([this, Self]
		{
			if (TSharedPtr<SMantleBuilderViewport> Pinned = Self->Pin())
			{
				Working->Points = Pinned->GetPoints();
			}
		}));
	*Self = Viewport;
	Viewport->SetMesh(LoadModel());
	Viewport->SetPoints(Working->Points);
	return Viewport;
}

bool SMantleWeaponBuilder::HasAllRequiredPoints(FText& OutWhyNot) const
{
	for (const FMantleBuilderPointRole& Role : Kind->GetRoles())
	{
		if (Role.bRequired && !MantleBuilderPoints::Find(Working->Points, Role.Role))
		{
			OutWhyNot = FText::Format(LOCTEXT("NeedPoint", "Place the {0} to continue."), Role.Label.ToLower());
			return false;
		}
	}
	return true;
}

bool SMantleWeaponBuilder::IsRoleFlagged(const FMantleBuilderPointRole& Role, const TArray<FText>& Warnings) const
{
	// The kind's warnings are plain sentences that name the point ("The right hand can't reach..."), so match on the label.
	const FString Label = Role.Label.ToString();
	for (const FText& Warning : Warnings)
	{
		if (Warning.ToString().Contains(Label, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return false;
}

FText SMantleWeaponBuilder::WarningsText() const
{
	const TArray<FText> Warnings = Kind->Check(Working->Points);
	if (Warnings.Num() == 0)
	{
		return FText::GetEmpty();
	}
	FString Out;
	for (const FText& Warning : Warnings)
	{
		Out += (Out.IsEmpty() ? TEXT("- ") : TEXT("\n- ")) + MantleWeaponBuilderLogic::PlainWords(Warning.ToString());
	}
	return FText::FromString(Out);
}

FText SMantleWeaponBuilder::SummaryText() const
{
	using namespace MantleWeaponBuilderLogic;
	using namespace MantleWeaponBuilderPanelPrivate;
	const FMantleWeaponTuning& T = Working->Tuning;
	TArray<FString> Lines;
	Lines.Add(FString::Printf(TEXT("Name: %s"), *Working->DisplayName.TrimStartAndEnd()));
	Lines.Add(FString::Printf(TEXT("Weapon type: %s"), *HoldLabel(Working->Hold).ToString()));
	if (Working->Model.IsNull())
	{
		Lines.Add(TEXT("3D model: none selected"));
	}
	else
	{
		Lines.Add(FString::Printf(TEXT("3D model: %s"), *Working->Model.GetAssetName()));
		Lines.Add(FString::Printf(TEXT("Size: %.2f times its imported size; longest side about %.0f cm"), Working->FitScale, ModelLengthCm()));
	}
	Lines.Add(FString::Printf(TEXT("Points placed: %d"), Working->Points.Num()));

	const EMantleWeaponDocDelivery Delivery = EffectiveDelivery(*Working);
	const auto SourceDelivery = static_cast<EMantleWeaponSourceDelivery>(static_cast<uint8>(Delivery) + 1);
	const EMantleWeaponDocResource Resource = EffectiveResource(*Working);
	const auto SourceResource = static_cast<EMantleWeaponSourceResource>(static_cast<uint8>(Resource) + 1);
	if (Working->Hold == EMantleWeaponHold::Consumable)
	{
		const FMantleWeaponSourceConsumable& C = Working->Consumable;
		Lines.Add(FString::Printf(TEXT("Used by: %s"), C.Style == EMantleWeaponSourceUseStyle::EatDrink ? TEXT("eating or drinking") : TEXT("using it (lift and crush)")));
		if (C.Effect == EMantleWeaponSourceConsumableEffect::Heal)
		{
			Lines.Add(FString::Printf(TEXT("Heals: %.0f%s; uses: %d"), C.Heal, C.HealSeconds > 0.f ? *FString::Printf(TEXT(" over %.1f seconds"), C.HealSeconds) : TEXT(""), C.Uses));
		}
		else
		{
			Lines.Add(FString::Printf(TEXT("Effect: none of its own (a script does it); uses: %d"), C.Uses));
		}
		Lines.Add(FString::Printf(TEXT("Use time: %s; wait between uses: %.1f seconds; %s"),
			C.bOverrideUseSeconds ? *FString::Printf(TEXT("%.1f seconds"), C.UseSeconds) : TEXT("the animation's own length"), C.CooldownSeconds,
			C.bMoveWhileUsing ? TEXT("can walk while using") : TEXT("stands still while using")));
		return FText::FromString(FString::Join(Lines, TEXT("\n")));
	}
	Lines.Add(FString::Printf(TEXT("Attack: %s; resource: %s"), *DeliveryLabel(SourceDelivery, IsFirearm(Working->Hold)).ToString(), *ResourceLabel(SourceResource).ToString()));
	Lines.Add(FString::Printf(TEXT("Damage: %.0f; attack rate: %.1f per second"), T.Damage, T.ShotsPerSecond));
	if (Resource == EMantleWeaponDocResource::Magazine || Resource == EMantleWeaponDocResource::Quiver)
	{
		Lines.Add(FString::Printf(TEXT("Ready: %d; extra: %d; loading time: %.1f seconds"), T.MagazineSize, T.SpareAmmo, T.ReloadSeconds));
	}
	if (Resource == EMantleWeaponDocResource::Charges)
	{
		Lines.Add(FString::Printf(TEXT("Charges: %d; one comes back every %.1f seconds"), Working->Charges.Count, Working->Charges.RefillSeconds));
	}
	if (Delivery == EMantleWeaponDocDelivery::Beam)
	{
		Lines.Add(FString::Printf(TEXT("Beam: %.0f damage per second; %.1f seconds maximum; %.0f cm wide"), Working->Beam.DamagePerSecond, Working->Beam.MaxSeconds, Working->Beam.Radius * 2.f));
		if (Working->Beam.Range > 0.f)
		{
			Lines.Add(FString::Printf(TEXT("Beam reach: %.0f cm"), Working->Beam.Range));
		}
	}
	const bool bSpellsBesideBeam = Working->Hold == EMantleWeaponHold::Staff && Delivery == EMantleWeaponDocDelivery::Beam && Working->bSpellsBesideBeam;
	if (bSpellsBesideBeam)
	{
		Lines.Add(TEXT("Also throws spells: a tap casts, holding the button is the beam; aim to cast aimed, and aim and hold to charge"));
	}
	if (Delivery == EMantleWeaponDocDelivery::Projectile || bSpellsBesideBeam)
	{
		Lines.Add(FString::Printf(TEXT("Projectile speed: %.0f cm/s; gravity: %.1f"), Working->Projectile.Speed, Working->Projectile.Gravity));
		if (Working->Hold == EMantleWeaponHold::Throwable && Working->Projectile.BlastRadius > 0.f)
		{
			Lines.Add(FString::Printf(TEXT("Explodes after %.1f seconds, %.0f cm wide"), Working->Projectile.FuseSeconds, Working->Projectile.BlastRadius * 2.f));
		}
		if (Working->Hold == EMantleWeaponHold::Throwable && Working->Projectile.LingerSeconds > 0.f && Working->Projectile.BlastRadius <= 0.f)
		{
			Lines.Add(FString::Printf(TEXT("Leaves fire for %.1f seconds, %.0f cm wide, %.0f damage per second"), Working->Projectile.LingerSeconds,
				Working->Projectile.LingerRadius * 2.f, Working->Projectile.LingerDamagePerSecond));
		}
		if (!Working->Projectile.Model.IsNull() && !HoldInfo(Working->Hold).Actions.Contains(EMantleWeaponDocAction::Throw))
		{
			Lines.Add(FString::Printf(TEXT("Round or arrow model: %s"), *Working->Projectile.Model.GetAssetName()));
		}
	}
	if (Working->Charge.bEnabled || HoldInfo(Working->Hold).Charge == EMantleWeaponDocChargeRule::Required)
	{
		Lines.Add(FString::Printf(TEXT("Charge: %.1f seconds%s"), Working->Charge.Seconds, Working->Charge.bRequired ? TEXT(" (required)") : TEXT("")));
	}
	if (Working->bBlock || HoldInfo(Working->Hold).BlockRule == EMantleWeaponDocBlockRule::Required)
	{
		Lines.Add(FString::Printf(TEXT("Blocking: on%s"), Working->bBreakOnHeavy ? TEXT("; heavy attacks break a guard") : TEXT("")));
	}
	if (Working->bOverrideStyle) Lines.Add(FString::Printf(TEXT("Effect style: %s"), *StyleLabel(Working->Style).ToString()));
	if (Working->Parts.Num() > 0) Lines.Add(FString::Printf(TEXT("Moving parts: %d"), Working->Parts.Num()));
	if (Working->Attachments.Num() > 0) Lines.Add(FString::Printf(TEXT("Fixed details: %d"), Working->Attachments.Num()));
	if (!Working->FireSound.IsNull() || !Working->FireSuppressedSound.IsNull()) Lines.Add(TEXT("Custom sound: set"));
	return FText::FromString(FString::Join(Lines, TEXT("\n")));
}

// ---------------------------------------------------------------------------------------------------------------------
// Steps.

TSharedRef<SWidget> SMantleWeaponBuilder::MakeHoldCard(EMantleWeaponHold Hold)
{
	using namespace MantleWeaponBuilderLogic;
	using namespace MantleWeaponBuilderPanelPrivate;
	const bool bAvailable = IsHoldAvailable(Hold);
	return SNew(SBox)
		.WidthOverride(190.f)
		.HeightOverride(110.f)
		.Padding(FMargin(0.f, 0.f, 8.f, 8.f))
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor_Lambda([this, Hold]
			{
				return FSlateColor(Working->Hold == Hold ? FLinearColor(0.10f, 0.40f, 0.80f) : FLinearColor(0.10f, 0.10f, 0.10f));
			})
			.Padding(2.f)
			[
				SNew(SButton)
				.IsEnabled(bAvailable)
				.HAlign(HAlign_Fill)
				.VAlign(VAlign_Fill)
				.OnClicked_Lambda([this, Hold]
				{
					SetHold(Hold);
					return FReply::Handled();
				})
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock)
						.Font(FAppStyle::GetFontStyle("DetailsView.CategoryFontStyle"))
						.Text(HoldLabel(Hold))
					]
					+ SVerticalBox::Slot().FillHeight(1.f).Padding(0.f, 4.f, 0.f, 0.f)
					[
						SNew(STextBlock)
						.AutoWrapText(true)
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.Text(bAvailable ? BuilderHoldBlurb(Hold) : LOCTEXT("ComingSoon", "Coming soon"))
					]
				]
			]
		];
}

TSharedRef<SWidget> SMantleWeaponBuilder::MakeKindStep()
{
	using namespace MantleWeaponBuilderPanelPrivate;
	TSharedRef<SWrapBox> Cards = SNew(SWrapBox).UseAllottedSize(true);
	// The guns first, then the rest.
	const EMantleWeaponHold Order[] = {
		EMantleWeaponHold::Pistol, EMantleWeaponHold::Rifle,
		EMantleWeaponHold::Revolver, EMantleWeaponHold::Smg, EMantleWeaponHold::Dmr,
		EMantleWeaponHold::PumpShotgun, EMantleWeaponHold::BoltSniper, EMantleWeaponHold::Lmg,
		EMantleWeaponHold::Melee1h, EMantleWeaponHold::Melee2h, EMantleWeaponHold::Spear, EMantleWeaponHold::Staff,
		EMantleWeaponHold::Bow, EMantleWeaponHold::Unarmed, EMantleWeaponHold::Shield, EMantleWeaponHold::Throwable, EMantleWeaponHold::Consumable, };
	for (const EMantleWeaponHold Hold : Order)
	{
		Cards->AddSlot()[MakeHoldCard(Hold)];
	}

	return SNew(SScrollBox)
		+ SScrollBox::Slot().Padding(0.f, 0.f, 0.f, 12.f)
		[
			MakeRow(LOCTEXT("OpenSavedWeapon", "Open a saved weapon to edit"),
				LOCTEXT("OpenSavedWeaponTip", "Choose a weapon you made earlier to reopen its saved points, size, attack settings, details and sounds."),
				SNew(SObjectPropertyEntryBox)
					.AllowedClass(UMantleWeaponSource::StaticClass())
					.DisplayThumbnail(false)
					.AllowClear(false)
					.ObjectPath_Lambda([this] { return SourceAssetPath.ToString(); })
					.OnShouldFilterAsset_Lambda([](const FAssetData& Asset)
					{
						const FString PackagePath = Asset.PackagePath.ToString();
						return !PackagePath.StartsWith(TEXT("/Game/MantleWeapons/"), ESearchCase::IgnoreCase);
					})
					.OnObjectChanged_Lambda([this](const FAssetData& Asset)
					{
						if (UMantleWeaponSource* Source = Cast<UMantleWeaponSource>(Asset.GetAsset()))
						{
							LoadSourceAsset(Source);
						}
					}))
		]
		+ SScrollBox::Slot()
		[
			Cards
		]
		+ SScrollBox::Slot().Padding(0.f, 12.f, 0.f, 0.f)
		[
			MakeRow(LOCTEXT("NameLabel", "Name your weapon"), LOCTEXT("NameTip", "Players see this name."),
				SNew(SEditableTextBox)
				.HintText(LOCTEXT("NameHint", "Name your weapon"))
				.Text_Lambda([this] { return FText::FromString(Working->DisplayName); })
				.OnTextChanged_Lambda([this](const FText& NewText)
				{
					Working->DisplayName = NewText.ToString();
					bNameEdited = true;
				}))
		];
}

TSharedRef<SWidget> SMantleWeaponBuilder::MakeModelStep()
{
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 8.f)
		[
			SNew(SObjectPropertyEntryBox)
			.AllowedClass(UObject::StaticClass())
			.DisplayThumbnail(true)
			.ThumbnailPool(UThumbnailManager::Get().GetSharedThumbnailPool())
			.AllowClear(true)
			.ObjectPath_Lambda([this] { return Working->Model.ToSoftObjectPath().ToString(); })
			.OnShouldFilterAsset_Lambda([](const FAssetData& Asset)
			{
				// True hides the asset: only 3D models show.
				return Asset.AssetClassPath != UStaticMesh::StaticClass()->GetClassPathName()
					&& Asset.AssetClassPath != USkeletalMesh::StaticClass()->GetClassPathName();
			})
			.OnObjectChanged_Lambda([this](const FAssetData& Asset)
			{
				SetModel(Asset.GetAsset());
			})
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 8.f)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.Text_Lambda([this]
			{
				return LOCTEXT("ModelHint", "Click the box and pick your weapon model from the list, or drag it in from your project's files.");
			})
		];
}

TSharedRef<SWidget> SMantleWeaponBuilder::MakeSizeStep()
{
	using namespace MantleWeaponBuilderLogic;
	using namespace MantleWeaponBuilderPanelPrivate;
	Kind->SetShowAiming(false);
	if (!LoadModel())
	{
		return SNew(SBorder)
			.Padding(18.f)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Text(LOCTEXT("SizeNeedsModel", "Choose your weapon's 3D model on the previous page first; its size is set from the model."))
			];
	}
	TSharedRef<SMantleBuilderViewport> Viewport = MakeViewport();
	Viewport->SetShowOverlay(false);
	SizeViewport = Viewport;

	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().FillHeight(1.f)
		[
			SNew(SBox).MinDesiredHeight(240.f)
			[
				Viewport
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)
		[
			SNew(SScrollBox)
			+ SScrollBox::Slot()
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.Text_Lambda([this] { return FText::FromString(FriendlyFitWords(Kind->GetLastFit().ScaleReason)); })
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f)
				[
					SNew(STextBlock)
					.Text_Lambda([this]
					{
						return FText::Format(LOCTEXT("LengthNow", "The longest side of this weapon model is about {0} cm."), FText::AsNumber(FMath::RoundToInt(ModelLengthCm())));
					})
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					MakeRow(LOCTEXT("SizeLabel", "Model size (1 = as imported)"), LOCTEXT("SizeTip", "Makes the weapon model bigger or smaller. The grey figure shows how it fits beside a character."),
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(1.f)
						[
							SNew(SSpinBox<float>)
							.MinValue(0.01f)
							.MaxValue(100.f)
							.MinSliderValue(0.01f)
							.MaxSliderValue(100.f)
							.SliderExponent(6.f)
							.Delta(0.01f)
							.MinFractionalDigits(2)
							.MaxFractionalDigits(3)
							.Value_Lambda([this] { return Working->FitScale; })
							.OnValueChanged_Lambda([this](float Value)
							{
								// While dragging only the number moves; the fit is redone when the drag ends.
								Working->FitScale = Value;
								Working->bScaleOverridden = true;
							})
							.OnEndSliderMovement_Lambda([this](float Value) { SetFit(Value, true); })
							.OnValueCommitted_Lambda([this](float Value, ETextCommit::Type) { SetFit(Value, true); })
						])
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 8.f, 0.f)
					[
						SNew(SButton)
						.Text(LOCTEXT("ResetSize", "Reset size"))
						.ToolTipText(LOCTEXT("ResetSizeTip", "Go back to the size we guessed."))
						.IsEnabled_Lambda([this] { return Working->bScaleOverridden; })
						.OnClicked_Lambda([this]
						{
							Working->bScaleOverridden = false;
							Refit();
							return FReply::Handled();
						})
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 8.f, 0.f)
					[
						SNew(SButton)
						.Text(LOCTEXT("FlipFrontBack", "Flip front and back"))
						.ToolTipText(LOCTEXT("FlipFrontBackTip", "Turn the model around if its front and back face the wrong way for this hold."))
						.OnClicked_Lambda([this]
						{
							// A half turn about the weapon's up direction.
							TurnFit(FQuat(FVector::UpVector, UE_PI));
							return FReply::Handled();
						})
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SButton)
						.Text(LOCTEXT("TurnUpsideDown", "Turn upside down"))
						.ToolTipText(LOCTEXT("TurnUpsideDownTip", "Turn the model over if its grip points away from the hand."))
						.OnClicked_Lambda([this]
						{
							// A half turn about the barrel.
							TurnFit(FQuat(FVector::ForwardVector, UE_PI));
							return FReply::Handled();
						})
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f)
				[
					SNew(SHorizontalBox)
					.Visibility_Lambda([this] { return Kind->GetLastFit().bOutsideTypicalRange && !Kind->GetLastFit().Suggestion.IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed; })
					+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.AutoWrapText(true)
						.ColorAndOpacity(WarnColour())
						.Text_Lambda([this] { return FText::FromString(FriendlyFitWords(Kind->GetLastFit().Suggestion)); })
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(8.f, 0.f, 0.f, 0.f)
					[
						SNew(SButton)
						.Visibility_Lambda([this]
						{
							return AvailableHoldNamedIn(Kind->GetLastFit().Suggestion, Working->Hold).IsSet() ? EVisibility::Visible : EVisibility::Collapsed;
						})
						.Text_Lambda([this]
						{
							const TOptional<EMantleWeaponHold> Named = AvailableHoldNamedIn(Kind->GetLastFit().Suggestion, Working->Hold);
							return Named.IsSet() ? FText::Format(LOCTEXT("SwitchTo", "Switch to {0}"), HoldLabel(Named.GetValue())) : FText::GetEmpty();
						})
						.OnClicked_Lambda([this]
						{
							const TOptional<EMantleWeaponHold> Named = AvailableHoldNamedIn(Kind->GetLastFit().Suggestion, Working->Hold);
							if (Named.IsSet())
							{
								SetHold(Named.GetValue());
								Refit();
							}
							return FReply::Handled();
						})
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f)
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.Text_Lambda([this]
					{
						FString Out;
						for (const FString& Note : Kind->GetLastFit().Notes)
						{
							Out += (Out.IsEmpty() ? TEXT("- ") : TEXT("\n- ")) + FriendlyFitWords(Note);
						}
						return FText::FromString(Out);
					})
				]
			]
		];
}

TSharedRef<SWidget> SMantleWeaponBuilder::MakePointsStep()
{
	using namespace MantleWeaponBuilderPanelPrivate;
	Kind->SetShowAiming(false);
	TSharedRef<SMantleBuilderViewport> Viewport = MakeViewport();
	PointViewport = Viewport;

	// The aiming controls show only for a kind that has a Sight point.
	auto SightVisibility = [this]
	{
		for (const FMantleBuilderPointRole& Role : Kind->GetRoles())
			if (Role.Role == FName(TEXT("Sight"))) return EVisibility::Visible;
		return EVisibility::Collapsed;
	};

	TSharedRef<SVerticalBox> RoleList = SNew(SVerticalBox);
	const TArray<FMantleBuilderPointRole> Roles = Kind->GetRoles();
	for (const FMantleBuilderPointRole& Role : Roles)
	{
		const FName RoleName = Role.Role;
		FText Label = FriendlyRoleLabel(Role, Working->Hold);
		if (!Role.bRequired)
		{
			Label = FText::Format(LOCTEXT("OptionalRole", "{0} (optional)"), Label);
		}
		RoleList->AddSlot().AutoHeight().Padding(0.f, 0.f, 0.f, 4.f)
		[
			SNew(SButton)
			.ToolTipText(FriendlyRoleHelp(Role, Working->Hold))
			.HAlign(HAlign_Fill)
			.OnClicked_Lambda([Viewport, RoleName]
			{
				Viewport->SetSelectedRole(RoleName);
				return FReply::Handled();
			})
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 6.f, 0.f)
				[
					SNew(SBox).WidthOverride(16.f).HeightOverride(16.f)
					[
						SNew(SImage)
						.Image(FAppStyle::GetBrush("Icons.Check"))
						.ColorAndOpacity(GoodColour())
						.Visibility_Lambda([this, RoleName] { return MantleBuilderPoints::Find(Working->Points, RoleName) ? EVisibility::Visible : EVisibility::Hidden; })
					]
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
				[
					SNew(STextBlock).Text(Label)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SBox).WidthOverride(16.f).HeightOverride(16.f)
					[
						SNew(SImage)
						.Image(FAppStyle::GetBrush("Icons.Warning"))
						.ToolTipText(LOCTEXT("RoleFlagged", "Something about this point needs a look. See the notes under the picture."))
						.Visibility_Lambda([this, Role]
						{
							return IsRoleFlagged(Role, Kind->Check(Working->Points)) ? EVisibility::Visible : EVisibility::Hidden;
						})
					]
				]
			]
		];
	}
	if (Roles.Num() > 0)
	{
		Viewport->SetSelectedRole(Roles[0].Role);
	}

	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 12.f, 0.f)
		[
			SNew(SBox).WidthOverride(250.f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					RoleList
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 8.f, 0.f, 4.f)
				[
					SNew(SButton)
					.Text(LOCTEXT("ResetThisPoint", "Reset this point"))
					.ToolTipText(LOCTEXT("ResetThisPointTip", "Put the selected point back where we guessed it."))
					.OnClicked_Lambda([this, Viewport]
					{
						Viewport->ResetSelectedToGuess();
						Working->Points = Viewport->GetPoints();
						return FReply::Handled();
					})
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SButton)
					.Text(LOCTEXT("ResetAllPoints", "Reset all points"))
					.ToolTipText(LOCTEXT("ResetAllPointsTip", "Put every point back where we guessed it."))
					.OnClicked_Lambda([this, Viewport]
					{
						Viewport->ResetAllToGuess();
						Working->Points = Viewport->GetPoints();
						return FReply::Handled();
					})
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 12.f, 0.f, 4.f)
				[
					SNew(SCheckBox)
					.Visibility_Lambda(SightVisibility)
					.ToolTipText(LOCTEXT("ShowAimingTip", "Pose the figure aiming, and show where the player's eye will be and the line of sight through your sight point."))
					.IsChecked_Lambda([this] { return Kind->GetShowAiming() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged_Lambda([this, Viewport](ECheckBoxState State)
					{
						Kind->SetShowAiming(State == ECheckBoxState::Checked);
						Viewport->RefreshFromKind();
					})
					[
						SNew(STextBlock).Text(LOCTEXT("ShowAiming", "Show aiming"))
					]
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Visibility_Lambda(SightVisibility)
					.AutoWrapText(true)
					.Text(LOCTEXT("ShowAimingHelp", "Shows the player's eye and the line of sight through your aim point. Check the line and shoulder clearance."))
				]
			]
		]
		+ SHorizontalBox::Slot().FillWidth(1.f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().FillHeight(1.f)
			[
				SNew(SBox).MinDesiredHeight(260.f)
				[
					Viewport
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.ColorAndOpacity(WarnColour())
				.Text_Lambda([this] { return WarningsText(); })
			]
		];
}

TSharedRef<SWidget> SMantleWeaponBuilder::MakeTuningStep()
{
	using namespace MantleWeaponBuilderLogic;
	using namespace MantleWeaponBuilderPanelPrivate;
	if (Working->Hold == EMantleWeaponHold::Consumable)
	{
		// A consumable is used up, not fired: its own few settings replace the attack ones.
		const TArray<TPair<EMantleWeaponSourceUseStyle, FText>> Styles = {
			{ EMantleWeaponSourceUseStyle::EatDrink, LOCTEXT("StyleEatDrink", "Eat or drink (bottle to the lips)") },
			{ EMantleWeaponSourceUseStyle::Use, LOCTEXT("StyleUseIt", "Use (lift the hand and crush; medkits)") },
		};
		const TArray<TPair<EMantleWeaponSourceConsumableEffect, FText>> Effects = {
			{ EMantleWeaponSourceConsumableEffect::Heal, LOCTEXT("EffectHeal", "Heal") },
			{ EMantleWeaponSourceConsumableEffect::None, LOCTEXT("EffectNone", "Nothing of its own (a script does it)") },
		};
		const TFunction<EVisibility()> ShowHeal = [this]
		{
			return Working->Consumable.Effect == EMantleWeaponSourceConsumableEffect::Heal ? EVisibility::Visible : EVisibility::Collapsed;
		};
		return SNew(SScrollBox)
			+ SScrollBox::Slot().Padding(0.f, 6.f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()[MakeRow(LOCTEXT("UseStyle", "How it is used"),
					LOCTEXT("UseStyleTip", "Eat or drink plays the drinking animation of whatever the character holds. Use plays the lift-and-crush animation, a good fit for a medkit."),
					MakeChoiceControl<EMantleWeaponSourceUseStyle>(Styles, [this] { return Working->Consumable.Style; }, [this](EMantleWeaponSourceUseStyle V) { Working->Consumable.Style = V; }))]
				+ SVerticalBox::Slot().AutoHeight()[MakeRow(LOCTEXT("UseEffect", "What it does"),
					LOCTEXT("UseEffectTip", "Heal gives health. Nothing gives none of its own: a script in your place does the rest."),
					MakeChoiceControl<EMantleWeaponSourceConsumableEffect>(Effects, [this] { return Working->Consumable.Effect; }, [this](EMantleWeaponSourceConsumableEffect V) { Working->Consumable.Effect = V; }))]
				+ SVerticalBox::Slot().AutoHeight()[SNew(SBox).Visibility_Lambda([ShowHeal] { return ShowHeal(); })[MakeRow(LOCTEXT("Heal", "Health given"),
					LOCTEXT("HealTip", "How much health one use gives, from 1 to 1000, up to the character's maximum."),
					MakeFloatSpin(1.f, 1000.f, 5.f, [this] { return Working->Consumable.Heal; }, [this](float V) { Working->Consumable.Heal = V; }))]]
				+ SVerticalBox::Slot().AutoHeight()[SNew(SBox).Visibility_Lambda([ShowHeal] { return ShowHeal(); })[MakeRow(LOCTEXT("HealSeconds", "Spread over (seconds)"),
					LOCTEXT("HealSecondsTip", "0 gives all of it at once. Above 0 spreads it evenly over this many seconds."),
					MakeFloatSpin(0.f, 60.f, 0.5f, [this] { return Working->Consumable.HealSeconds; }, [this](float V) { Working->Consumable.HealSeconds = V; }))]]
				+ SVerticalBox::Slot().AutoHeight()[MakeRow(LOCTEXT("Uses", "Uses"),
					LOCTEXT("UsesTip", "How many times it can be used before it is gone."),
					MakeIntSpin(1, 100, [this] { return Working->Consumable.Uses; }, [this](int32 V) { Working->Consumable.Uses = V; }))]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 3.f)
				[
					SNew(SCheckBox)
					.IsChecked_Lambda([this] { return Working->Consumable.bOverrideUseSeconds ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged_Lambda([this](ECheckBoxState State) { Working->Consumable.bOverrideUseSeconds = State == ECheckBoxState::Checked; })
					.ToolTipText(LOCTEXT("OverrideUseTimeTip", "Off: one use takes as long as the animation (2.5 seconds). On: the animation plays faster or slower to fit the time below."))
					[
						SNew(STextBlock).Text(LOCTEXT("OverrideUseTime", "Choose how long one use takes"))
					]
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SBox).Visibility_Lambda([this] { return Working->Consumable.bOverrideUseSeconds ? EVisibility::Visible : EVisibility::Collapsed; })
					[
						MakeRow(LOCTEXT("UseSeconds", "Use time (seconds)"), LOCTEXT("UseSecondsTip", "From 0.5 to 10 seconds. The health arrives at the same point of the animation, scaled to this time."),
							MakeFloatSpin(0.5f, 10.f, 0.1f, [this] { return Working->Consumable.UseSeconds; }, [this](float V) { Working->Consumable.UseSeconds = V; }))
					]
				]
				+ SVerticalBox::Slot().AutoHeight()[MakeRow(LOCTEXT("Cooldown", "Wait between uses (seconds)"),
					LOCTEXT("CooldownTip", "How long after a use before it can be used again. 0 means no wait."),
					MakeFloatSpin(0.f, 600.f, 0.5f, [this] { return Working->Consumable.CooldownSeconds; }, [this](float V) { Working->Consumable.CooldownSeconds = V; }))]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 3.f)
				[
					SNew(SCheckBox)
					.IsChecked_Lambda([this] { return Working->Consumable.bMoveWhileUsing ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged_Lambda([this](ECheckBoxState State) { Working->Consumable.bMoveWhileUsing = State == ECheckBoxState::Checked; })
					.ToolTipText(LOCTEXT("MoveWhileUsingTip", "On: the character can walk while using it. Off: the character stands still until the use ends. Sprinting, jumping or dodging cancels a use before it takes effect and nothing is spent."))
					[
						SNew(STextBlock).Text(LOCTEXT("MoveWhileUsing", "Can walk while using"))
					]
				]
			];
	}
	TSharedRef<SVerticalBox> Rows = SNew(SVerticalBox);
	auto AddHeading = [&Rows](const FText& Text)
	{
		Rows->AddSlot().AutoHeight().Padding(0.f, 12.f, 0.f, 3.f)
		[
			SNew(STextBlock).Font(FAppStyle::GetFontStyle("DetailsView.CategoryFontStyle")).Text(Text)
		];
	};
	auto AddRowWhen = [&Rows](const FText& Label, const FText& Tip, const TSharedRef<SWidget>& Control, TFunction<EVisibility()> Visibility)
	{
		const TSharedRef<SWidget> Row = MakeRow(Label, Tip, Control);
		Rows->AddSlot().AutoHeight()
		[
			SNew(SBox).Visibility_Lambda([Visibility] { return Visibility(); })[Row]
		];
	};
	auto AddRow = [&AddRowWhen](const FText& Label, const FText& Tip, const TSharedRef<SWidget>& Control)
	{
		AddRowWhen(Label, Tip, Control, [] { return EVisibility::Visible; });
	};
	auto AddToggleWhen = [&Rows](const FText& Label, const FText& Tip, TFunction<bool()> Get, TFunction<void(bool)> Set,
		TFunction<EVisibility()> Visibility, TFunction<bool()> IsEnabled)
	{
		Rows->AddSlot().AutoHeight().Padding(0.f, 3.f)
		[
			SNew(SBox).Visibility_Lambda([Visibility] { return Visibility(); })
			[
				SNew(SCheckBox)
				.IsEnabled_Lambda([IsEnabled] { return IsEnabled(); })
				.IsChecked_Lambda([Get] { return Get() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([Set](ECheckBoxState State) { Set(State == ECheckBoxState::Checked); })
				.ToolTipText(Tip)
				[
					SNew(STextBlock).Text(Label)
				]
			]
		];
	};
	auto AddToggle = [&AddToggleWhen](const FText& Label, const FText& Tip, TFunction<bool()> Get, TFunction<void(bool)> Set)
	{
		AddToggleWhen(Label, Tip, Get, Set, [] { return EVisibility::Visible; }, [] { return true; });
	};
	auto AddFloatWhen = [&AddRowWhen](const FText& Label, const FText& Tip, float Min, float Max, float Delta,
		TFunction<float()> Get, TFunction<void(float)> Set, TFunction<EVisibility()> Visibility)
	{
		AddRowWhen(Label, Tip, MakeFloatSpin(Min, Max, Delta, Get, Set), Visibility);
	};
	auto AddIntWhen = [&AddRowWhen](const FText& Label, const FText& Tip, int32 Min, int32 Max,
		TFunction<int32()> Get, TFunction<void(int32)> Set, TFunction<EVisibility()> Visibility)
	{
		AddRowWhen(Label, Tip, MakeIntSpin(Min, Max, Get, Set), Visibility);
	};

	const FMantleWeaponDocHoldInfo& Info = HoldInfo(Working->Hold);
	TArray<TPair<EMantleWeaponSourceDelivery, FText>> Deliveries;
	const bool bGun = IsFirearm(Working->Hold);
	Deliveries.Emplace(EMantleWeaponSourceDelivery::Default, DeliveryLabel(EMantleWeaponSourceDelivery::Default));
	for (const EMantleWeaponDocDelivery Delivery : Info.Deliveries)
	{
		if (!MantleWeaponBuilderLogic::IsDeliveryOffered(MantleWeaponBuilderLogic::ToDocHold(Working->Hold), Delivery))
		{
			continue;
		}
		const auto SourceDelivery = static_cast<EMantleWeaponSourceDelivery>(static_cast<uint8>(Delivery) + 1);
		Deliveries.Emplace(SourceDelivery, DeliveryLabel(SourceDelivery, bGun));
	}
	TArray<TPair<EMantleWeaponSourceResource, FText>> Resources;
	Resources.Emplace(EMantleWeaponSourceResource::Default, ResourceLabel(EMantleWeaponSourceResource::Default));
	for (const EMantleWeaponDocResource Resource : Info.Resources)
	{
		const auto SourceResource = static_cast<EMantleWeaponSourceResource>(static_cast<uint8>(Resource) + 1);
		Resources.Emplace(SourceResource, ResourceLabel(SourceResource));
	}
	TArray<TPair<FName, FText>> SocketChoices;
	SocketChoices.Emplace(NAME_None, LOCTEXT("UseDefaultSweep", "Use this hold's usual points"));
	for (const FMantleBuilderPointRole& Role : Kind->GetRoles())
	{
		SocketChoices.Emplace(Role.Role, FriendlyRoleLabel(Role, Working->Hold));
	}

	AddHeading(LOCTEXT("DeliveryHeading", "How it works"));
	if (Deliveries.Num() > 2)
	{
		// #905: SetDelivery starts a gun's beam or launched round from usable numbers, and keeps the effect style one the attack can use.
		AddRow(LOCTEXT("Delivery", "Attack style"), bGun
				? LOCTEXT("DeliveryTipGun", "An instant shot hits at once. A flying round travels and can burst where it lands, like a grenade or rocket launcher. A beam keeps hurting while the trigger is held, like a laser or a flamethrower.")
				: LOCTEXT("DeliveryTip", "Choose one of the actions this hold supports."),
			MakeChoiceControl<EMantleWeaponSourceDelivery>(Deliveries, [this] { return Working->Delivery; }, [this](EMantleWeaponSourceDelivery V)
			{
				MantleWeaponBuilderLogic::SetDelivery(*Working, V);
			}));
	}
	// #909: a staff's held beam can also throw spells (a tap casts, holding is the beam).
	AddToggleWhen(LOCTEXT("SpellsBesideBeam", "Also throws spells"), LOCTEXT("SpellsBesideBeamTip", "A tap throws a spell and holding the button channels the beam. While aiming, a tap throws an aimed spell and holding charges one (with a charged attack on)."),
		[this] { return Working->bSpellsBesideBeam; },
		[this](bool V) { Working->bSpellsBesideBeam = V; },
		[this] { return Working->Hold == EMantleWeaponHold::Staff && EffectiveDelivery(*Working) == EMantleWeaponDocDelivery::Beam ? EVisibility::Visible : EVisibility::Collapsed; },
		[] { return true; });
	if (Resources.Num() > 2)
	{
		AddRow(LOCTEXT("Resource", "What it uses"), LOCTEXT("ResourceTip", "Choose only a resource supported by this hold."),
			MakeChoiceControl<EMantleWeaponSourceResource>(Resources, [this] { return Working->Resource; }, [this](EMantleWeaponSourceResource V) { Working->Resource = V; }));
	}

	const TArray<TPair<EMantleWeaponSourceStyle, FText>> ElementStyles = StyleChoices(true);
	const TArray<TPair<EMantleWeaponSourceStyle, FText>> FirearmStyles = StyleChoices(false);
	const TFunction<bool()> StyleIsElement = [this] { return MantleWeaponBuilderLogic::PicksElement(Working->Hold, EffectiveDelivery(*Working)); };
	const TFunction<EVisibility()> bHasStyle = [this]
	{
		return EffectiveDelivery(*Working) == EMantleWeaponDocDelivery::Sweep
			&& !Working->bOverrideStyle && !Working->bOverrideColor
			? EVisibility::Collapsed : EVisibility::Visible;
	};
	AddToggleWhen(LOCTEXT("OverrideStyle", "Choose an effect style"), LOCTEXT("OverrideStyleTip", "Use a built-in style such as fire, ice, laser or arcane instead of this hold's usual effect."),
		[this] { return Working->bOverrideStyle; }, [this](bool V) { Working->bOverrideStyle = V; }, bHasStyle, [] { return true; });
	// #905: one row of elements (a staff, a bow, a gun's flying round or beam) and one of firearm styles (a gun's instant shot); the
	// attack style shows the one it can use.
	AddRowWhen(LOCTEXT("Style", "Effect style"), LOCTEXT("StyleTip", "The built-in effect and sound style used by this attack."),
		MakeChoiceControl<EMantleWeaponSourceStyle>(ElementStyles, [this] { return Working->Style; }, [this](EMantleWeaponSourceStyle V) { Working->Style = V; }),
		[this, bHasStyle, StyleIsElement] { return bHasStyle() == EVisibility::Visible && EffectiveDelivery(*Working) != EMantleWeaponDocDelivery::Sweep && Working->bOverrideStyle && StyleIsElement() ? EVisibility::Visible : EVisibility::Collapsed; });
	AddRowWhen(LOCTEXT("Style", "Effect style"), LOCTEXT("StyleTipGun", "The built-in shot effect and sound style. For an element such as laser or fire, pick a flying round or a beam above."),
		MakeChoiceControl<EMantleWeaponSourceStyle>(FirearmStyles, [this] { return Working->Style; }, [this](EMantleWeaponSourceStyle V) { Working->Style = V; }),
		[this, bHasStyle, StyleIsElement] { return bHasStyle() == EVisibility::Visible && EffectiveDelivery(*Working) != EMantleWeaponDocDelivery::Sweep && Working->bOverrideStyle && !StyleIsElement() ? EVisibility::Visible : EVisibility::Collapsed; });
	AddToggleWhen(LOCTEXT("OverrideColor", "Choose a custom effect color"), LOCTEXT("OverrideColorTip", "Tint the built-in effect with your own color."),
		[this] { return Working->bOverrideColor; }, [this](bool V) { Working->bOverrideColor = V; }, bHasStyle, [] { return true; });
	TSharedRef<SHorizontalBox> Color = SNew(SHorizontalBox);
	for (int32 Channel = 0; Channel < 3; ++Channel)
	{
		const TCHAR* ChannelName = Channel == 0 ? TEXT("R") : Channel == 1 ? TEXT("G") : TEXT("B");
		Color->AddSlot().FillWidth(1.f).Padding(Channel == 0 ? 0.f : 4.f, 0.f, 0.f, 0.f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(FText::FromString(ChannelName))]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SSpinBox<int32>).MinValue(0).MaxValue(255).Value_Lambda([this, Channel]
				{
					const FColor C = Working->Color;
					return Channel == 0 ? C.R : Channel == 1 ? C.G : C.B;
				})
				.OnValueChanged_Lambda([this, Channel](int32 V)
				{
					if (Channel == 0) Working->Color.R = static_cast<uint8>(V);
					else if (Channel == 1) Working->Color.G = static_cast<uint8>(V);
					else Working->Color.B = static_cast<uint8>(V);
				})
				.OnValueCommitted_Lambda([this, Channel](int32 V, ETextCommit::Type)
				{
					if (Channel == 0) Working->Color.R = static_cast<uint8>(V);
					else if (Channel == 1) Working->Color.G = static_cast<uint8>(V);
					else Working->Color.B = static_cast<uint8>(V);
				})
			]
		];
	}
	AddRowWhen(LOCTEXT("Color", "Effect color (RGB)"), LOCTEXT("ColorTip", "Red, green and blue values from 0 to 255."), Color,
		[this, bHasStyle] { return bHasStyle() == EVisibility::Visible && EffectiveDelivery(*Working) != EMantleWeaponDocDelivery::Sweep && Working->bOverrideColor ? EVisibility::Visible : EVisibility::Collapsed; });

	AddHeading(LOCTEXT("DamageHeading", "Damage and timing"));
	AddRow(LOCTEXT("Damage", "Damage"), LOCTEXT("DamageTip", "How much health one attack removes."),
		MakeFloatSpin(DamageRange().Min, DamageRange().Max, 1.f,
			[this] { return Working->Tuning.Damage; },
			[this](float V) { Working->Tuning.Damage = V; Working->Tuning = ClampTuning(Working->Tuning); }));
	AddRow(LOCTEXT("AttackRate", "Attacks per second"), LOCTEXT("AttackRateTip", "How often the weapon can attack, cast or throw."),
		MakeFloatSpin(ShotsPerSecondRange().Min, ShotsPerSecondRange().Max, 0.1f,
			[this] { return Working->Tuning.ShotsPerSecond; },
			[this](float V) { Working->Tuning.ShotsPerSecond = V; Working->Tuning = ClampTuning(Working->Tuning); }));
	const TFunction<bool()> UsesRounds = [this]
	{
		const auto Resource = EffectiveResource(*Working);
		return Resource == EMantleWeaponDocResource::Magazine || Resource == EMantleWeaponDocResource::Quiver;
	};
	AddRowWhen(LOCTEXT("MagazineSize", "Rounds or arrows carried"), LOCTEXT("MagazineSizeTip", "How many rounds or arrows are ready before loading again."),
		MakeIntSpin(static_cast<int32>(MagazineSizeRange().Min), static_cast<int32>(MagazineSizeRange().Max),
			[this] { return Working->Tuning.MagazineSize; },
			[this](int32 V) { Working->Tuning.MagazineSize = V; Working->Tuning = ClampTuning(Working->Tuning); }),
		[this, UsesRounds] { return UsesRounds() ? EVisibility::Visible : EVisibility::Collapsed; });
	AddRowWhen(LOCTEXT("SpareAmmo", "Extra rounds or arrows"), LOCTEXT("SpareAmmoTip", "How many more rounds or arrows the player carries."),
		MakeIntSpin(static_cast<int32>(SpareAmmoRange().Min), static_cast<int32>(SpareAmmoRange().Max),
			[this] { return Working->Tuning.SpareAmmo; },
			[this](int32 V) { Working->Tuning.SpareAmmo = V; Working->Tuning = ClampTuning(Working->Tuning); }),
		[this, UsesRounds] { return UsesRounds() ? EVisibility::Visible : EVisibility::Collapsed; });
	AddRowWhen(LOCTEXT("ReloadTime", "Reload or nock time (seconds)"), LOCTEXT("ReloadTimeTip", "How long loading takes when the hold's animation does not set its own timing."),
		MakeFloatSpin(ReloadSecondsRange().Min, ReloadSecondsRange().Max, 0.1f,
			[this] { return Working->Tuning.ReloadSeconds; },
			[this](float V) { Working->Tuning.ReloadSeconds = V; Working->Tuning = ClampTuning(Working->Tuning); }),
		[this, UsesRounds] { return HoldInfo(Working->Hold).bHasReloadStyle && UsesRounds() ? EVisibility::Visible : EVisibility::Collapsed; });
	AddToggleWhen(LOCTEXT("Automatic", "Keep firing while the trigger is held"), LOCTEXT("AutomaticTip", "Turn this off for one shot per click."),
		[this] { return Working->Tuning.bAutomatic; }, [this](bool V) { Working->Tuning.bAutomatic = V; },
		[this, UsesRounds] { return IsFirearm(Working->Hold) && UsesRounds() ? EVisibility::Visible : EVisibility::Collapsed; }, [] { return true; });
	AddRowWhen(LOCTEXT("MaxRange", "Maximum range (0 = usual; otherwise 100–100000 cm)"), LOCTEXT("MaxRangeTip", "Set how far a shot, projectile or beam can reach. Choose zero for the usual range, or a value from 100 to 100000 cm."),
		MakeFloatSpin(0.f, 100000.f, 100.f,
			[this] { return Working->Tuning.MaxRange; },
			[this](float V)
			{
				Working->Tuning.MaxRange = V == 0.f ? 0.f : FMath::Clamp(V, 100.f, 100000.f);
				Working->Tuning = ClampTuning(Working->Tuning);
			}),
		[this] { return EffectiveDelivery(*Working) == EMantleWeaponDocDelivery::Sweep ? EVisibility::Collapsed : EVisibility::Visible; });

	const TFunction<bool()> IsHitscan = [this] { return EffectiveDelivery(*Working) == EMantleWeaponDocDelivery::Hitscan; };
	AddRowWhen(LOCTEXT("TraceRadius", "Shot width"), LOCTEXT("TraceRadiusTip", "The radius in centimeters of the instant shot's hit check."),
		MakeFloatSpin(0.f, 50.f, 0.5f,
			[this] { return Working->Tuning.TraceSweepRadius; },
			[this](float V) { Working->Tuning.TraceSweepRadius = V; Working->Tuning = ClampTuning(Working->Tuning); }),
		[this, IsHitscan] { return IsHitscan() ? EVisibility::Visible : EVisibility::Collapsed; });
	const TFunction<bool()> HasPellets = [this]
	{
		return IsFirearm(Working->Hold) && (Working->Hold == EMantleWeaponHold::PumpShotgun || Working->Tuning.Pellets > 1)
			&& EffectiveDelivery(*Working) == EMantleWeaponDocDelivery::Hitscan;
	};
	AddIntWhen(LOCTEXT("Pellets", "Shots in one attack"), LOCTEXT("PelletsTip", "How many separate hit checks one attack makes."), 1, 32,
		[this] { return Working->Tuning.Pellets; },
		[this](int32 V) { Working->Tuning.Pellets = V; Working->Tuning = ClampTuning(Working->Tuning); },
		[this, HasPellets] { return HasPellets() ? EVisibility::Visible : EVisibility::Collapsed; });
	AddFloatWhen(LOCTEXT("Spread", "Shot spread"), LOCTEXT("SpreadTip", "How widely the shots fan out, in degrees."), 0.f, 10.f, 0.1f,
		[this] { return Working->Tuning.PelletSpread; },
		[this](float V) { Working->Tuning.PelletSpread = V; Working->Tuning = ClampTuning(Working->Tuning); },
		[this, HasPellets] { return HasPellets() ? EVisibility::Visible : EVisibility::Collapsed; });
	AddFloatWhen(LOCTEXT("PlainRecoil", "Recoil (−1 = usual)"), LOCTEXT("PlainRecoilTip", "Use −1 for this hold's normal recoil, or set a value from 0 to 30."), -1.f, 30.f, 0.1f,
		[this] { return Working->Tuning.PlainRecoil; },
		[this](float V) { Working->Tuning.PlainRecoil = V < 0.f ? -1.f : V; Working->Tuning = ClampTuning(Working->Tuning); },
		[this] { return IsFirearm(Working->Hold) ? EVisibility::Visible : EVisibility::Collapsed; });
	AddToggleWhen(LOCTEXT("BurstMode", "Fire a short burst per click"), LOCTEXT("BurstModeTip", "Use the hold's burst mode instead of its usual firing mode."),
		[this] { return Working->Tuning.bBurstMode; }, [this](bool V) { Working->Tuning.bBurstMode = V; },
		[this] { return IsFirearm(Working->Hold) ? EVisibility::Visible : EVisibility::Collapsed; }, [] { return true; });

	const TFunction<EVisibility()> HasCharge = [this]
	{
		return HoldInfo(Working->Hold).Charge != EMantleWeaponDocChargeRule::None ? EVisibility::Visible : EVisibility::Collapsed;
	};
	const TFunction<EVisibility()> ActiveCharge = [this, HasCharge]
	{
		return HasCharge() == EVisibility::Visible
			&& (Working->Charge.bEnabled || HoldInfo(Working->Hold).Charge == EMantleWeaponDocChargeRule::Required)
			? EVisibility::Visible : EVisibility::Collapsed;
	};
	AddHeading(LOCTEXT("ChargeHeading", "Charge and guard"));
	AddToggleWhen(LOCTEXT("EnableCharge", "Allow a charged attack"), LOCTEXT("EnableChargeTip", "Let players hold the attack button to build a stronger attack."),
		[this] { return Working->Charge.bEnabled || HoldInfo(Working->Hold).Charge == EMantleWeaponDocChargeRule::Required; },
		[this](bool V) { Working->Charge.bEnabled = V; }, HasCharge,
		[this] { return HoldInfo(Working->Hold).Charge != EMantleWeaponDocChargeRule::Required; });
	AddFloatWhen(LOCTEXT("ChargeSeconds", "Time to charge (seconds)"), LOCTEXT("ChargeSecondsTip", "How long a full charge takes."), 0.1f, 10.f, 0.1f,
		[this] { return Working->Charge.Seconds; }, [this](float V) { Working->Charge.Seconds = FMath::Clamp(V, 0.1f, 10.f); }, ActiveCharge);
	AddToggleWhen(LOCTEXT("ChargeRequired", "Require a full charge before attacking"), LOCTEXT("ChargeRequiredTip", "When on, a tap does not attack. Bows always need a draw before release."),
		[this] { return Working->Charge.bRequired || HoldInfo(Working->Hold).Charge == EMantleWeaponDocChargeRule::Required; },
		[this](bool V)
		{
			Working->Charge.bRequired = V;
			if (V) Working->Charge.bEnabled = true;
		}, ActiveCharge,
		[this] { return HoldInfo(Working->Hold).Charge != EMantleWeaponDocChargeRule::Required; });
	AddToggleWhen(LOCTEXT("ReleaseOnFull", "Release automatically when full"), LOCTEXT("ReleaseOnFullTip", "Fire the charged attack as soon as the charge is full."),
		[this] { return Working->Charge.bReleaseOnFull; }, [this](bool V) { Working->Charge.bReleaseOnFull = V; }, ActiveCharge, [] { return true; });
	AddToggleWhen(LOCTEXT("CancelableCharge", "Let players cancel a charge"), LOCTEXT("CancelableChargeTip", "Allow the player to let go without making an attack."),
		[this] { return Working->Charge.bCancelable; }, [this](bool V) { Working->Charge.bCancelable = V; }, ActiveCharge, [] { return true; });

	const TFunction<bool()> CanBlock = [this]
	{
		return HoldInfo(Working->Hold).BlockRule != EMantleWeaponDocBlockRule::None;
	};
	AddToggleWhen(LOCTEXT("Block", "Can block"), LOCTEXT("BlockTip", "This hold can use its block action."),
		[this] { return Working->bBlock; }, [this](bool V) { Working->bBlock = V; },
		[this, CanBlock] { return CanBlock() ? EVisibility::Visible : EVisibility::Collapsed; },
		[this] { return HoldInfo(Working->Hold).BlockRule != EMantleWeaponDocBlockRule::Required; });
	AddToggleWhen(LOCTEXT("BreakOnHeavy", "A heavy attack can break the guard"), LOCTEXT("BreakOnHeavyTip", "A heavy attack can interrupt a block."),
		[this] { return Working->bBreakOnHeavy; }, [this](bool V) { Working->bBreakOnHeavy = V; },
		[this, CanBlock] { return CanBlock() && Working->bBlock ? EVisibility::Visible : EVisibility::Collapsed; }, [] { return true; });
	const bool bCanOverrideHandedness = IsFirearm(Working->Hold);
	AddToggleWhen(LOCTEXT("OverrideTwoHanded", "Change how many hands hold it"), LOCTEXT("OverrideTwoHandedTip", "Use this hold with one hand or both. The usual setting follows the hold."),
		[this] { return Working->bOverrideTwoHanded; }, [this](bool V) { Working->bOverrideTwoHanded = V; },
		[bCanOverrideHandedness] { return bCanOverrideHandedness ? EVisibility::Visible : EVisibility::Collapsed; }, [] { return true; });
	AddToggleWhen(LOCTEXT("TwoHanded", "Hold with both hands"), LOCTEXT("TwoHandedTip", "Choose a one-handed or two-handed grip for this firearm."),
		[this] { return Working->bTwoHanded; }, [this](bool V) { Working->bTwoHanded = V; },
		[this, bCanOverrideHandedness] { return bCanOverrideHandedness && Working->bOverrideTwoHanded ? EVisibility::Visible : EVisibility::Collapsed; }, [] { return true; });

	const TFunction<bool()> IsSweep = [this]
	{
		return EffectiveDelivery(*Working) == EMantleWeaponDocDelivery::Sweep;
	};
	const TFunction<bool()> IsProjectile = [this]
	{
		// #909: a staff's spells beside its beam take the projectile's numbers too.
		return EffectiveDelivery(*Working) == EMantleWeaponDocDelivery::Projectile
			|| (Working->Hold == EMantleWeaponHold::Staff && Working->bSpellsBesideBeam && EffectiveDelivery(*Working) == EMantleWeaponDocDelivery::Beam);
	};
	const TFunction<bool()> HoldThrowsItsModel = [this]
	{
		return HoldInfo(Working->Hold).Actions.Contains(EMantleWeaponDocAction::Throw);
	};
	const TFunction<EVisibility()> ShowThrownCleanup = [this, IsProjectile, HoldThrowsItsModel]
	{
		// Throwing holds derive this from their action. Let creators clear the legacy-invalid true value on other holds,
		// but do not offer a choice that the document validator rejects.
		return IsProjectile() && !HoldThrowsItsModel() && Working->Projectile.bThrown
			? EVisibility::Visible : EVisibility::Collapsed;
	};
	// A hold that throws its own model (the throwable) sticks and can be picked up with no choice to make, flies as itself and
	// carries no blast: none of those rows are offered.
	const TFunction<EVisibility()> ShowThrownProjectileChoice = [IsProjectile, HoldThrowsItsModel]
	{
		return IsProjectile() && !HoldThrowsItsModel() ? EVisibility::Visible : EVisibility::Collapsed;
	};
	const TFunction<EVisibility()> ShowPickup = [this, IsProjectile, HoldThrowsItsModel]
	{
		const bool bAllowed = EffectiveResource(*Working) == EMantleWeaponDocResource::Quiver;
		// Keep an existing invalid true value visible so it can be cleared before Create.
		return IsProjectile() && !HoldThrowsItsModel() && (bAllowed || Working->Projectile.bPickup)
			? EVisibility::Visible : EVisibility::Collapsed;
	};
	AddHeading(LOCTEXT("AttackDetailHeading", "Attack details"));
	AddFloatWhen(LOCTEXT("SweepReach", "Swing reach (30–400 cm)"), LOCTEXT("SweepReachTip", "How far the melee attack reaches. Choose a value from 30 to 400 cm."), 30.f, 400.f, 1.f,
		[this] { return Working->Sweep.Reach; }, [this](float V) { Working->Sweep.Reach = FMath::Clamp(V, 30.f, 400.f); },
		[this, IsSweep] { return IsSweep() ? EVisibility::Visible : EVisibility::Collapsed; });
	AddIntWhen(LOCTEXT("ComboCount", "Combo steps"), LOCTEXT("ComboCountTip", "How many attacks chain together (1 to 5)."), 1, 5,
		[this] { return Working->Sweep.ComboCount; }, [this](int32 V) { Working->Sweep.ComboCount = FMath::Clamp(V, 1, 5); },
		[this, IsSweep] { return IsSweep() ? EVisibility::Visible : EVisibility::Collapsed; });
	AddFloatWhen(LOCTEXT("HeavyMultiplier", "Heavy attack multiplier"), LOCTEXT("HeavyMultiplierTip", "How much stronger the heavy attack is than a regular hit (1 to 5)."), 1.f, 5.f, 0.1f,
		[this] { return Working->Sweep.HeavyMultiplier; }, [this](float V) { Working->Sweep.HeavyMultiplier = FMath::Clamp(V, 1.f, 5.f); },
		[this, IsSweep] { return IsSweep() ? EVisibility::Visible : EVisibility::Collapsed; });
	AddRowWhen(LOCTEXT("SweepStart", "Swing starts at"), LOCTEXT("SweepStartTip", "Choose the point where the sweep begins, or use the hold's usual points."),
		MakeChoiceControl<FName>(SocketChoices, [this] { return Working->Sweep.EdgeBase; }, [this](FName V) { Working->Sweep.EdgeBase = V; }),
		[this, IsSweep] { return IsSweep() ? EVisibility::Visible : EVisibility::Collapsed; });
	AddRowWhen(LOCTEXT("SweepEnd", "Swing ends at"), LOCTEXT("SweepEndTip", "Choose the point where the sweep ends, or use the hold's usual points."),
		MakeChoiceControl<FName>(SocketChoices, [this] { return Working->Sweep.EdgeTip; }, [this](FName V) { Working->Sweep.EdgeTip = V; }),
		[this, IsSweep] { return IsSweep() ? EVisibility::Visible : EVisibility::Collapsed; });

	AddFloatWhen(LOCTEXT("ProjectileSpeed", "Projectile speed (cm/s)"), LOCTEXT("ProjectileSpeedTip", "How quickly the projectile travels (100 to 30000 cm/s)."), 100.f, 30000.f, 100.f,
		[this] { return Working->Projectile.Speed; }, [this](float V) { Working->Projectile.Speed = FMath::Clamp(V, 100.f, 30000.f); },
		[this, IsProjectile] { return IsProjectile() ? EVisibility::Visible : EVisibility::Collapsed; });
	AddFloatWhen(LOCTEXT("ProjectileGravity", "Projectile gravity"), LOCTEXT("ProjectileGravityTip", "Zero flies straight. One uses usual gravity; larger values fall faster (0 to 3)."), 0.f, 3.f, 0.1f,
		[this] { return Working->Projectile.Gravity; }, [this](float V) { Working->Projectile.Gravity = FMath::Clamp(V, 0.f, 3.f); },
		[this, IsProjectile] { return IsProjectile() ? EVisibility::Visible : EVisibility::Collapsed; });
	AddFloatWhen(LOCTEXT("BlastRadius", "Blast radius (0–2000 cm)"), LOCTEXT("BlastRadiusTip", "Zero means the projectile only hits what it touches."), 0.f, 2000.f, 10.f,
		[this] { return Working->Projectile.BlastRadius; }, [this](float V) { Working->Projectile.BlastRadius = FMath::Clamp(V, 0.f, 2000.f); },
		ShowThrownProjectileChoice);
	// A throwable may explode (a grenade): a choice with its blast radius and fuse. It goes off when its fuse runs out or the moment
	// it hits someone, everyone sees and hears it, and it is used up (it neither sticks nor is picked up).
	const TFunction<bool()> IsThrowable = [this, IsProjectile] { return IsProjectile() && Working->Hold == EMantleWeaponHold::Throwable; };
	const TFunction<EVisibility()> ShowExplodes = [IsThrowable] { return IsThrowable() ? EVisibility::Visible : EVisibility::Collapsed; };
	const TFunction<EVisibility()> ShowExplodeNumbers = [this, IsThrowable]
	{
		return IsThrowable() && Working->Projectile.BlastRadius > 0.f ? EVisibility::Visible : EVisibility::Collapsed;
	};
	AddToggleWhen(LOCTEXT("Explodes", "Explodes (a grenade)"), LOCTEXT("ExplodesTip", "It bounces, then explodes when its fuse runs out or when it hits someone. Everyone nearby is hurt, and the weapon is used up."),
		[this] { return Working->Projectile.BlastRadius > 0.f; },
		[this](bool V)
		{
			if (!V)
			{
				Working->Projectile.BlastRadius = 0.f;
				return;
			}
			// A thrown weapon explodes or leaves fire, not both.
			Working->Projectile.LingerSeconds = 0.f;
			if (Working->Projectile.BlastRadius <= 0.f)
			{
				Working->Projectile.BlastRadius = 450.f;
			}
			// A grenade is lobbed: the knife's flat, fast flight is not what a creator wants for one.
			if (Working->Projectile.Speed > 2500.f)
			{
				Working->Projectile.Speed = 1800.f;
				Working->Projectile.Gravity = 1.f;
			}
		},
		ShowExplodes, [] { return true; });
	AddFloatWhen(LOCTEXT("ExplosionRadius", "Explosion radius (10–2000 cm)"), LOCTEXT("ExplosionRadiusTip", "How far the explosion reaches. Damage is full close in and fades toward the edge."), 10.f, 2000.f, 10.f,
		[this] { return Working->Projectile.BlastRadius; }, [this](float V) { Working->Projectile.BlastRadius = FMath::Clamp(V, 10.f, 2000.f); },
		ShowExplodeNumbers);
	AddFloatWhen(LOCTEXT("Fuse", "Fuse (0.1–30 seconds)"), LOCTEXT("FuseTip", "How long after it leaves the hand it explodes. Hitting someone sets it off at once."), 0.1f, 30.f, 0.1f,
		[this] { return Working->Projectile.FuseSeconds; }, [this](float V) { Working->Projectile.FuseSeconds = FMath::Clamp(V, 0.1f, 30.f); },
		ShowExplodeNumbers);
	// A throwable may leave fire where it lands (a molotov): a choice with the fire's radius, how long it burns and how much it hurts.
	const TFunction<EVisibility()> ShowFireNumbers = [this, IsThrowable]
	{
		return IsThrowable() && Working->Projectile.LingerSeconds > 0.f ? EVisibility::Visible : EVisibility::Collapsed;
	};
	AddToggleWhen(LOCTEXT("LeavesFire", "Leaves fire where it lands (a molotov)"), LOCTEXT("LeavesFireTip", "It breaks where it lands and leaves a burning area that hurts everyone standing in it, then burns out. The weapon is used up."),
		[this] { return Working->Projectile.LingerSeconds > 0.f; },
		[this](bool V)
		{
			if (!V)
			{
				Working->Projectile.LingerSeconds = 0.f;
				return;
			}
			// A thrown weapon explodes or leaves fire, not both.
			Working->Projectile.BlastRadius = 0.f;
			Working->Projectile.LingerSeconds = 6.f;
			if (Working->Projectile.Speed > 2500.f)
			{
				Working->Projectile.Speed = 1400.f;
				Working->Projectile.Gravity = 1.f;
			}
		},
		ShowExplodes, [] { return true; });
	AddFloatWhen(LOCTEXT("FireRadius", "Fire radius (10–2000 cm)"), LOCTEXT("FireRadiusTip", "How far the fire spreads from where it lands."), 10.f, 2000.f, 10.f,
		[this] { return Working->Projectile.LingerRadius; }, [this](float V) { Working->Projectile.LingerRadius = FMath::Clamp(V, 10.f, 2000.f); },
		ShowFireNumbers);
	AddFloatWhen(LOCTEXT("FireSeconds", "Fire lasts (0.1–30 seconds)"), LOCTEXT("FireSecondsTip", "How long the fire burns before it goes out."), 0.1f, 30.f, 0.1f,
		[this] { return Working->Projectile.LingerSeconds; }, [this](float V) { Working->Projectile.LingerSeconds = FMath::Clamp(V, 0.1f, 30.f); },
		ShowFireNumbers);
	AddFloatWhen(LOCTEXT("FireDamage", "Fire damage per second (0–1000)"), LOCTEXT("FireDamageTip", "Damage each second to everyone standing in the fire."), 0.f, 1000.f, 1.f,
		[this] { return Working->Projectile.LingerDamagePerSecond; }, [this](float V) { Working->Projectile.LingerDamagePerSecond = FMath::Clamp(V, 0.f, 1000.f); },
		ShowFireNumbers);
	AddToggleWhen(LOCTEXT("Thrown", "Throw the weapon model itself"), LOCTEXT("ThrownTip", "This is only valid for spear and throwable holds; their throw action already uses the weapon model."),
		[this] { return Working->Projectile.bThrown; }, [this](bool V) { Working->Projectile.bThrown = V; }, ShowThrownCleanup, [] { return true; });
	AddToggleWhen(LOCTEXT("Stick", "Stick to what it hits"), LOCTEXT("StickTip", "Keep the projectile attached to the surface it hits."),
		[this] { return Working->Projectile.bStick; }, [this](bool V) { Working->Projectile.bStick = V; },
		ShowThrownProjectileChoice, [] { return true; });
	AddToggleWhen(LOCTEXT("Pickup", "Let players pick it up"), LOCTEXT("PickupTip", "Allow the projectile to be picked up after it lands."),
		[this] { return Working->Projectile.bPickup; }, [this](bool V) { Working->Projectile.bPickup = V; }, ShowPickup, [] { return true; });
	// #905: the round or arrow that flies (a launcher's rocket, a creator's arrow); Create copies it beside the weapon as it does an extra
	// part's model. A hold that throws its own model flies as itself, so it is not offered there.
	AddRowWhen(LOCTEXT("ProjectileModel", "Round or arrow model"), LOCTEXT("ProjectileModelTip", "Optional. The 3D model that flies when the weapon fires: a launcher's rocket or grenade, a bow's arrow. Leave empty for the usual look."),
		MakeMeshOrSoundPicker([this] { return Working->Projectile.Model.ToSoftObjectPath(); },
			[this](const FSoftObjectPath& Path) { Working->Projectile.Model = Path; }, false),
		ShowThrownProjectileChoice);

	const TFunction<bool()> IsBeam = [this] { return EffectiveDelivery(*Working) == EMantleWeaponDocDelivery::Beam; };
	AddFloatWhen(LOCTEXT("BeamTickRate", "Beam damage checks per second"), LOCTEXT("BeamTickRateTip", "How often the held beam checks for a hit (1 to 30 times per second)."), 1.f, 30.f, 1.f,
		[this] { return Working->Beam.TickRate; }, [this](float V) { Working->Beam.TickRate = FMath::Clamp(V, 1.f, 30.f); },
		[this, IsBeam] { return IsBeam() ? EVisibility::Visible : EVisibility::Collapsed; });
	AddFloatWhen(LOCTEXT("BeamDamage", "Beam damage per second"), LOCTEXT("BeamDamageTip", "Damage dealt each second while the beam hits a target."), 0.f, 1000.f, 1.f,
		[this] { return Working->Beam.DamagePerSecond; }, [this](float V) { Working->Beam.DamagePerSecond = FMath::Clamp(V, 0.f, 1000.f); },
		[this, IsBeam] { return IsBeam() ? EVisibility::Visible : EVisibility::Collapsed; });
	AddFloatWhen(LOCTEXT("BeamMaxSeconds", "Maximum beam time (0.1–30 seconds)"), LOCTEXT("BeamMaxSecondsTip", "How long one beam can stay active."), 0.1f, 30.f, 0.1f,
		[this] { return Working->Beam.MaxSeconds; }, [this](float V) { Working->Beam.MaxSeconds = FMath::Clamp(V, 0.1f, 30.f); },
		[this, IsBeam] { return IsBeam() ? EVisibility::Visible : EVisibility::Collapsed; });
	AddFloatWhen(LOCTEXT("BeamRadius", "Beam thickness (0–200 cm)"), LOCTEXT("BeamRadiusTip", "How far from its centre line the beam hits, in cm. Zero is a thin line, like a laser; a larger value is a wide stream, like a flamethrower's."), 0.f, 200.f, 1.f,
		[this] { return Working->Beam.Radius; }, [this](float V) { Working->Beam.Radius = FMath::Clamp(V, 0.f, 200.f); },
		[this, IsBeam] { return IsBeam() ? EVisibility::Visible : EVisibility::Collapsed; });
	AddFloatWhen(LOCTEXT("BeamRange", "Beam reach (0 = maximum range; otherwise 100–100000 cm)"), LOCTEXT("BeamRangeTip", "How far the beam reaches. A flamethrower reaches a few metres, a laser much further. Choose zero to use the maximum range above, or a value from 100 to 100000 cm."), 0.f, 100000.f, 100.f,
		[this] { return Working->Beam.Range; }, [this](float V) { Working->Beam.Range = V == 0.f ? 0.f : FMath::Clamp(V, 100.f, 100000.f); },
		[this, IsBeam] { return IsBeam() ? EVisibility::Visible : EVisibility::Collapsed; });

	const TFunction<bool()> UsesCharges = [this] { return EffectiveResource(*Working) == EMantleWeaponDocResource::Charges; };
	AddIntWhen(LOCTEXT("StaffCharges", "Casts it holds (1–100)"), LOCTEXT("StaffChargesTip", "How many casts the staff can make before it runs dry. Each cast uses one."), 1, 100,
		[this] { return Working->Charges.Count; }, [this](int32 V) { Working->Charges.Count = FMath::Clamp(V, 1, 100); },
		[this, UsesCharges] { return UsesCharges() ? EVisibility::Visible : EVisibility::Collapsed; });
	AddFloatWhen(LOCTEXT("StaffRefill", "Seconds to get one cast back (0.1–600)"), LOCTEXT("StaffRefillTip", "How long the staff takes to give back one used cast, while you are not casting."), 0.1f, 600.f, 0.1f,
		[this] { return Working->Charges.RefillSeconds; }, [this](float V) { Working->Charges.RefillSeconds = FMath::Clamp(V, 0.1f, 600.f); },
		[this, UsesCharges] { return UsesCharges() ? EVisibility::Visible : EVisibility::Collapsed; });
	AddFloatWhen(LOCTEXT("BowStringRadius", "Bow string thickness (cm)"), LOCTEXT("BowStringRadiusTip", "The visible width of the bow string."), 0.1f, 10.f, 0.1f,
		[this] { return Working->BowStringRadiusCm; }, [this](float V) { Working->BowStringRadiusCm = FMath::Clamp(V, 0.1f, 10.f); },
		[this] { return Working->Hold == EMantleWeaponHold::Bow ? EVisibility::Visible : EVisibility::Collapsed; });

	Rows->AddSlot().AutoHeight().Padding(0.f, 14.f, 0.f, 4.f)
	[
		SNew(SButton)
		.Text(LOCTEXT("UsualNumbers", "Go back to the usual attack numbers"))
		.OnClicked_Lambda([this]
		{
			Working->Tuning = DefaultTuning(Working->Hold);
			return FReply::Handled();
		})
	];
	return SNew(SScrollBox)+SScrollBox::Slot()[Rows];
}

TSharedRef<SWidget> SMantleWeaponBuilder::MakePartsAndSoundStep()
{
	using namespace MantleWeaponBuilderPanelPrivate;
	if (Working->Hold == EMantleWeaponHold::Consumable)
	{
		return SNew(STextBlock).AutoWrapText(true)
			.Text(LOCTEXT("ConsumableNoParts", "A consumable has no moving parts, extra details or custom sounds. Go on to Create."));
	}
	const TArray<TPair<EMantleWeaponSourcePartRole, FText>> PartChoices = PartRoleChoices(Working->Hold);
	// An attachment's effect is an element's held effect (#904).
	TArray<TPair<EMantleWeaponSourceStyle, FText>> Styles = AllStyles();
	Styles.RemoveAll([](const TPair<EMantleWeaponSourceStyle, FText>& Style) { return !FMantleWeaponDocument::IsElement(static_cast<EMantleWeaponDocEffects>(Style.Key)); });
	TArray<TPair<FName, FText>> Sockets;
	Sockets.Emplace(NAME_None, LOCTEXT("AttachmentRoot", "Weapon root"));
	for (const FMantleBuilderPointRole& Role : Kind->GetRoles())
	{
		Sockets.Emplace(Role.Role, FriendlyRoleLabel(Role, Working->Hold));
	}

	TSharedRef<SVerticalBox> Rows = SNew(SVerticalBox);
	auto AddHeading = [&Rows](const FText& Text)
	{
		Rows->AddSlot().AutoHeight().Padding(0.f, 12.f, 0.f, 4.f)
		[
			SNew(STextBlock).Font(FAppStyle::GetFontStyle("DetailsView.CategoryFontStyle")).Text(Text)
		];
	};
	auto AddText = [&Rows](const FText& Text)
	{
		Rows->AddSlot().AutoHeight().Padding(0.f, 2.f, 0.f, 6.f)
		[
			SNew(STextBlock).AutoWrapText(true).ColorAndOpacity(FSlateColor::UseSubduedForeground()).Text(Text)
		];
	};
	auto AddRow = [&Rows](const FText& Label, const FText& Tip, const TSharedRef<SWidget>& Control)
	{
		Rows->AddSlot().AutoHeight()[MakeRow(Label, Tip, Control)];
	};

	AddHeading(LOCTEXT("MovingPartsHeading", "Moving parts"));
	AddText(LOCTEXT("MovingPartsHelp", "Add the model for a magazine, a round or shell, or an arrow. Leave the timing on its usual setting unless the part should appear during different actions."));
	if (PartChoices.IsEmpty())
	{
		AddText(LOCTEXT("NoMovingParts", "This weapon type has no separate magazine, round or arrow parts."));
	}
	else
	{
		for (int32 PartIndex = 0; PartIndex < Working->Parts.Num(); ++PartIndex)
		{
			const int32 Index = PartIndex;
			TArray<EMantleWeaponSourceAction> ActionsForPart;
			for (const auto& Choice : PartChoices)
			{
				for (const EMantleWeaponSourceAction Action : PartActionChoices(Working->Hold, Choice.Key))
				{
					ActionsForPart.AddUnique(Action);
				}
			}
			TSharedRef<SWrapBox> ActionWidgets = SNew(SWrapBox).UseAllottedSize(true);
			for (const EMantleWeaponSourceAction Action : ActionsForPart)
			{
				ActionWidgets->AddSlot().Padding(8.f, 2.f)
				[
					SNew(SBox)
					.Visibility_Lambda([this, Index, Action]
					{
						return Working->Parts.IsValidIndex(Index)
							&& PartActionChoices(Working->Hold, Working->Parts[Index].Role).Contains(Action)
							? EVisibility::Visible : EVisibility::Collapsed;
					})
					[
						SNew(SCheckBox)
						.IsChecked_Lambda([this, Index, Action]
						{
							return Working->Parts.IsValidIndex(Index) && Working->Parts[Index].ShownDuring.Contains(Action)
								? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
						})
						.OnCheckStateChanged_Lambda([this, Index, Action](ECheckBoxState State)
						{
							if (!Working->Parts.IsValidIndex(Index)) return;
							FMantleWeaponSourcePart& Part = Working->Parts[Index];
							if (State == ECheckBoxState::Checked) Part.ShownDuring.AddUnique(Action);
							else Part.ShownDuring.Remove(Action);
							if (Part.ShownDuring.IsEmpty()) Part.ShownDuring = DefaultPartActions(Working->Hold, Part.Role);
						})
						[
							SNew(STextBlock).Text(PartActionLabel(Action))
						]
					]
				];
			}
			Rows->AddSlot().AutoHeight().Padding(0.f, 6.f, 0.f, 8.f)
			[
				SNew(SBorder).Padding(10.f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						MakeRow(LOCTEXT("PartRole", "Part type"), LOCTEXT("PartRoleTip", "Choose where this separate model is used."),
							MakeChoiceControl<EMantleWeaponSourcePartRole>(PartChoices,
								[this, Index, PartChoices] { return Working->Parts.IsValidIndex(Index) ? Working->Parts[Index].Role : PartChoices[0].Key; },
								[this, Index](EMantleWeaponSourcePartRole Role)
								{
									if (!Working->Parts.IsValidIndex(Index)) return;
									FMantleWeaponSourcePart& Part = Working->Parts[Index];
									Part.Role = Role;
									Part.ShownDuring.Reset();
								}))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(1.f)
						[
							MakeMeshOrSoundPicker(
								[this, Index] { return Working->Parts.IsValidIndex(Index) ? Working->Parts[Index].Model.ToSoftObjectPath() : FSoftObjectPath(); },
								[this, Index](const FSoftObjectPath& Path) { if (Working->Parts.IsValidIndex(Index)) Working->Parts[Index].Model = Path; },
								false)
						]
						+ SHorizontalBox::Slot().AutoWidth().Padding(8.f, 0.f, 0.f, 0.f)
						[
							SNew(SButton)
							.Text(LOCTEXT("RemovePart", "Remove"))
							.OnClicked_Lambda([this, Index]
							{
								if (Working->Parts.IsValidIndex(Index))
								{
									Working->Parts.RemoveAt(Index);
									if (Flow.IsValid()) Flow->Refresh();
								}
								return FReply::Handled();
							})
						]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f)
					[
						SNew(STextBlock)
						.AutoWrapText(true)
						.ColorAndOpacity_Lambda([this, Index]
						{
							return Working->Parts.IsValidIndex(Index) && Working->Parts[Index].Model.IsNull()
								? FSlateColor(WarnColour()) : FSlateColor::UseSubduedForeground();
						})
						.Text_Lambda([this, Index]
						{
							return Working->Parts.IsValidIndex(Index) && Working->Parts[Index].Model.IsNull()
								? LOCTEXT("PartNeedsModel", "Choose a model for this part, or remove it before creating the weapon.")
								: LOCTEXT("PartModelHelp", "The part is placed in the same model space as the weapon.");
						})
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f)
					[
						MakeRow(LOCTEXT("PartPosition", "Position (cm)"), LOCTEXT("PartPositionTip", "Where the stowed part sits relative to the weapon model."),
							MakeVectorControl(
								[this, Index] { return Working->Parts.IsValidIndex(Index) ? Working->Parts[Index].InBody.GetLocation() : FVector::ZeroVector; },
								[this, Index](const FVector& Value) { if (Working->Parts.IsValidIndex(Index)) Working->Parts[Index].InBody.SetLocation(Value); },
								-1000.f, 1000.f, 1.f))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f)
					[
						MakeRow(LOCTEXT("PartRotation", "Rotation (°)"), LOCTEXT("PartRotationTip", "Turn the stowed part around its own position."),
							MakeVectorControl(
								[this, Index]
								{
									if (!Working->Parts.IsValidIndex(Index)) return FVector::ZeroVector;
									const FRotator Rotation = Working->Parts[Index].InBody.GetRotation().Rotator();
									return FVector(Rotation.Pitch, Rotation.Yaw, Rotation.Roll);
								},
								[this, Index](const FVector& Value)
								{
									if (Working->Parts.IsValidIndex(Index)) Working->Parts[Index].InBody.SetRotation(FRotator(Value.X, Value.Y, Value.Z).Quaternion());
								},
								-180.f, 180.f, 1.f))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f)
					[
						MakeRow(LOCTEXT("PartScale", "Size"), LOCTEXT("PartScaleTip", "Resize this part without changing the main weapon model."),
							MakeVectorControl(
								[this, Index] { return Working->Parts.IsValidIndex(Index) ? Working->Parts[Index].InBody.GetScale3D() : FVector::OneVector; },
								[this, Index](const FVector& Value)
								{
									if (Working->Parts.IsValidIndex(Index)) Working->Parts[Index].InBody.SetScale3D(Value);
								},
								0.01f, 100.f, 0.01f))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f)
					[
						SNew(SBox)
						.Visibility_Lambda([this, Index]
						{
							return Working->Parts.IsValidIndex(Index) && !PartActionChoices(Working->Hold, Working->Parts[Index].Role).IsEmpty()
								? EVisibility::Visible : EVisibility::Collapsed;
						})
						[
							SNew(SVerticalBox)
							+ SVerticalBox::Slot().AutoHeight()
							[
								SNew(SCheckBox)
								.IsChecked_Lambda([this, Index]
								{
									return Working->Parts.IsValidIndex(Index) && Working->Parts[Index].ShownDuring.IsEmpty()
										? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
								})
								.OnCheckStateChanged_Lambda([this, Index](ECheckBoxState State)
								{
									if (!Working->Parts.IsValidIndex(Index)) return;
									FMantleWeaponSourcePart& Part = Working->Parts[Index];
									if (State == ECheckBoxState::Checked) Part.ShownDuring.Reset();
									else if (Part.ShownDuring.IsEmpty()) Part.ShownDuring = DefaultPartActions(Working->Hold, Part.Role);
								})
								[
									SNew(STextBlock).Text(LOCTEXT("UseUsualPartTiming", "Use this hold's usual loading or drawing timing"))
								]
							]
							+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f)
							[
								SNew(SBox)
								.Visibility_Lambda([this, Index]
								{
									return Working->Parts.IsValidIndex(Index) && !Working->Parts[Index].ShownDuring.IsEmpty()
										? EVisibility::Visible : EVisibility::Collapsed;
								})
								[
									SNew(SVerticalBox)
									+ SVerticalBox::Slot().AutoHeight()
									[
										SNew(STextBlock).Text(LOCTEXT("PartCustomTiming", "Show during:"))
									]
									+ SVerticalBox::Slot().AutoHeight()
									[
										ActionWidgets
									]
								]
							]
						]
					]
				]
			];
		}

		Rows->AddSlot().AutoHeight().Padding(0.f, 4.f)
		[
			SNew(SButton)
			.Text(LOCTEXT("AddMovingPart", "Add moving part"))
			.IsEnabled_Lambda([this] { return Working->Parts.Num() < 16; })
			.OnClicked_Lambda([this, PartChoices]
			{
				if (Working->Parts.Num() < 16 && !PartChoices.IsEmpty())
				{
					Working->Parts.AddDefaulted_GetRef().Role = PartChoices[0].Key;
					if (Flow.IsValid()) Flow->Refresh();
				}
				return FReply::Handled();
			})
		];
	}

	if (!Working->Model.IsNull())
	{
		AddHeading(LOCTEXT("FixedDetailsHeading", "Fixed details and effects"));
		AddText(LOCTEXT("FixedDetailsHelp", "Add a model detail, a built-in effect, or both. Place it on the weapon root or at one of the points you placed earlier."));
		AddText(LOCTEXT("AttachmentNameRules", "Each detail name must be different. Use letters, numbers and underscores only; spaces and punctuation are not allowed."));
		for (int32 AttachmentIndex = 0; AttachmentIndex < Working->Attachments.Num(); ++AttachmentIndex)
		{
			const int32 Index = AttachmentIndex;
			Rows->AddSlot().AutoHeight().Padding(0.f, 6.f, 0.f, 8.f)
			[
			SNew(SBorder).Padding(10.f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					MakeRow(LOCTEXT("AttachmentName", "Detail name"), LOCTEXT("AttachmentNameTip", "Use a unique name made only of letters, numbers and underscores, such as Scope_Ring. Spaces and punctuation are not allowed."),
						SNew(SEditableTextBox)
						.HintText(LOCTEXT("AttachmentNameHint", "e.g. Scope_Ring"))
						.Text_Lambda([this, Index] { return Working->Attachments.IsValidIndex(Index) ? FText::FromString(Working->Attachments[Index].Name) : FText::GetEmpty(); })
						.OnTextChanged_Lambda([this, Index](const FText& Value)
						{
							if (Working->Attachments.IsValidIndex(Index)) Working->Attachments[Index].Name = Value.ToString().Left(64);
						}))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f)
				[
					MakeRow(LOCTEXT("AttachmentPoint", "Attach at"), LOCTEXT("AttachmentPointTip", "Choose one of this weapon's placed points, or leave it on the weapon root."),
						MakeChoiceControl<FName>(Sockets,
							[this, Index] { return Working->Attachments.IsValidIndex(Index) ? Working->Attachments[Index].Socket : NAME_None; },
							[this, Index](FName Socket) { if (Working->Attachments.IsValidIndex(Index)) Working->Attachments[Index].Socket = Socket; }))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 3.f)
				[
					SNew(SCheckBox)
					.IsChecked_Lambda([this, Index] { return Working->Attachments.IsValidIndex(Index) && Working->Attachments[Index].bEffect ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged_Lambda([this, Index](ECheckBoxState State) { if (Working->Attachments.IsValidIndex(Index)) Working->Attachments[Index].bEffect = State == ECheckBoxState::Checked; })
					[
						SNew(STextBlock).Text(LOCTEXT("AttachmentHasEffect", "Add a built-in effect"))
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 3.f)
				[
					SNew(SBox).Visibility_Lambda([this, Index] { return Working->Attachments.IsValidIndex(Index) && !Working->Attachments[Index].Model.IsNull() ? EVisibility::Visible : EVisibility::Collapsed; })
					[
						SNew(STextBlock).AutoWrapText(true)
						.Text_Lambda([this, Index]
						{
							if (!Working->Attachments.IsValidIndex(Index)) return FText::GetEmpty();
							return Working->Attachments[Index].bEffect
								? LOCTEXT("AttachmentModelAndEffectSet", "Model and effect selected.")
								: LOCTEXT("AttachmentModelSet", "Model selected. You can also add an effect above.");
						})
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 3.f)
				[
					SNew(SBox).Visibility_Lambda([this, Index]
					{
						return Working->Attachments.IsValidIndex(Index) && Working->Attachments[Index].Model.IsNull() && !Working->Attachments[Index].bEffect
							? EVisibility::Visible : EVisibility::Collapsed;
					})
					[
						SNew(STextBlock).AutoWrapText(true).ColorAndOpacity(WarnColour())
							.Text(LOCTEXT("AttachmentNeedsVisual", "Choose a model or add a built-in effect before creating the weapon."))
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f)
				[
					MakeRow(LOCTEXT("AttachmentModel", "3D model"), LOCTEXT("AttachmentModelTip", "Optional when you add a built-in effect. Choose a mesh for a fixed model detail."),
						MakeMeshOrSoundPicker(
							[this, Index] { return Working->Attachments.IsValidIndex(Index) ? Working->Attachments[Index].Model.ToSoftObjectPath() : FSoftObjectPath(); },
							[this, Index](const FSoftObjectPath& Path) { if (Working->Attachments.IsValidIndex(Index)) Working->Attachments[Index].Model = Path; },
							false))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f)
				[
					SNew(SBox).Visibility_Lambda([this, Index] { return Working->Attachments.IsValidIndex(Index) && Working->Attachments[Index].bEffect ? EVisibility::Visible : EVisibility::Collapsed; })
					[
						MakeRow(LOCTEXT("AttachmentEffect", "Effect style"), LOCTEXT("AttachmentEffectTip", "Choose the built-in effect to show at this point."),
							MakeChoiceControl<EMantleWeaponSourceStyle>(Styles,
								[this, Index] { return Working->Attachments.IsValidIndex(Index) ? Working->Attachments[Index].Effect : EMantleWeaponSourceStyle::Generic; },
								[this, Index](EMantleWeaponSourceStyle Style) { if (Working->Attachments.IsValidIndex(Index)) Working->Attachments[Index].Effect = Style; }))
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f)
				[
					MakeRow(LOCTEXT("AttachmentPosition", "Position (cm)"), LOCTEXT("AttachmentPositionTip", "Offset from the weapon root or selected point."),
						MakeVectorControl(
							[this, Index] { return Working->Attachments.IsValidIndex(Index) ? Working->Attachments[Index].RelativeTransform.GetLocation() : FVector::ZeroVector; },
							[this, Index](const FVector& Value) { if (Working->Attachments.IsValidIndex(Index)) Working->Attachments[Index].RelativeTransform.SetLocation(Value); },
							-1000.f, 1000.f, 1.f))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f)
				[
					MakeRow(LOCTEXT("AttachmentRotation", "Rotation (°)"), LOCTEXT("AttachmentRotationTip", "Pitch, yaw and roll from the weapon root or selected point."),
						MakeVectorControl(
							[this, Index]
							{
								return Working->Attachments.IsValidIndex(Index)
									? FVector(Working->Attachments[Index].RelativeTransform.GetRotation().Rotator().Pitch,
										Working->Attachments[Index].RelativeTransform.GetRotation().Rotator().Yaw,
										Working->Attachments[Index].RelativeTransform.GetRotation().Rotator().Roll)
									: FVector::ZeroVector;
							},
							[this, Index](const FVector& Value)
							{
								if (Working->Attachments.IsValidIndex(Index))
								{
									FTransform& Transform = Working->Attachments[Index].RelativeTransform;
									Transform.SetRotation(FRotator(Value.X, Value.Y, Value.Z).Quaternion());
								}
							},
							-180.f, 180.f, 1.f))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f)
				[
					MakeRow(LOCTEXT("AttachmentScale", "Size"), LOCTEXT("AttachmentScaleTip", "Scale each model axis. Values must stay above zero."),
						MakeVectorControl(
							[this, Index] { return Working->Attachments.IsValidIndex(Index) ? Working->Attachments[Index].RelativeTransform.GetScale3D() : FVector::OneVector; },
							[this, Index](const FVector& Value)
							{
								if (Working->Attachments.IsValidIndex(Index))
								{
									Working->Attachments[Index].RelativeTransform.SetScale3D(FVector(
										FMath::Clamp(Value.X, 0.01, 100.0), FMath::Clamp(Value.Y, 0.01, 100.0), FMath::Clamp(Value.Z, 0.01, 100.0)));
								}
							},
							0.01f, 100.f, 0.01f))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
					[
						SNew(SCheckBox)
						.IsChecked_Lambda([this, Index] { return Working->Attachments.IsValidIndex(Index) && Working->Attachments[Index].bHidden ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
						.OnCheckStateChanged_Lambda([this, Index](ECheckBoxState State) { if (Working->Attachments.IsValidIndex(Index)) Working->Attachments[Index].bHidden = State == ECheckBoxState::Checked; })
						[
							SNew(STextBlock).Text(LOCTEXT("AttachmentHidden", "Start hidden"))
						]
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SButton)
						.Text(LOCTEXT("RemoveAttachment", "Remove"))
						.OnClicked_Lambda([this, Index]
						{
							if (Working->Attachments.IsValidIndex(Index))
							{
								Working->Attachments.RemoveAt(Index);
								if (Flow.IsValid()) Flow->Refresh();
							}
							return FReply::Handled();
						})
					]
				]
			]
			];
		}
		Rows->AddSlot().AutoHeight().Padding(0.f, 4.f)
		[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 8.f, 0.f)
		[
			SNew(SButton)
			.Text(LOCTEXT("AddModelDetail", "Add model detail"))
			.IsEnabled_Lambda([this] { return Working->Attachments.Num() < 16; })
			.OnClicked_Lambda([this]
			{
				if (Working->Attachments.Num() < 16)
				{
					FMantleWeaponSourceAttachment& Attachment = Working->Attachments.AddDefaulted_GetRef();
					Attachment.Name = FString::Printf(TEXT("Detail_%d"), Working->Attachments.Num());
					Attachment.bEffect = false;
					if (Flow.IsValid()) Flow->Refresh();
				}
				return FReply::Handled();
			})
		]
		+ SHorizontalBox::Slot().AutoWidth()
		[
			SNew(SButton)
			.Text(LOCTEXT("AddEffect", "Add built-in effect"))
			.IsEnabled_Lambda([this] { return Working->Attachments.Num() < 16; })
			.OnClicked_Lambda([this]
			{
				if (Working->Attachments.Num() < 16)
				{
					FMantleWeaponSourceAttachment& Attachment = Working->Attachments.AddDefaulted_GetRef();
					Attachment.Name = FString::Printf(TEXT("Effect_%d"), Working->Attachments.Num());
					Attachment.bEffect = true;
					Attachment.Effect = EMantleWeaponSourceStyle::Generic;
					if (Flow.IsValid()) Flow->Refresh();
				}
				return FReply::Handled();
			})
		]
		];
	}
	else
	{
		AddHeading(LOCTEXT("FixedDetailsHeading", "Fixed details and effects"));
		AddText(LOCTEXT("NoWeaponBodyForDetails", "Choose your weapon's 3D model first: fixed models and effects attach to it."));
	}

	AddHeading(LOCTEXT("SoundHeading", "Sounds"));
	AddText(LOCTEXT("SoundHelp", "Leave a sound empty to use the usual sound for this weapon type. Sound triggers are optional; blank uses the normal attack trigger."));
	AddRow(LOCTEXT("AttackSound", "Fire or attack sound"), LOCTEXT("AttackSoundTip", "Pick a Sound Wave to use for this weapon's attack."),
		MakeMeshOrSoundPicker(
			[this] { return Working->FireSound.ToSoftObjectPath(); },
			[this](const FSoftObjectPath& Path) { Working->FireSound = Path; },
			true));
	AddRow(LOCTEXT("SuppressedSound", "Quieter attack sound"), LOCTEXT("SuppressedSoundTip", "Optional sound used while the weapon is suppressed."),
		MakeMeshOrSoundPicker(
			[this] { return Working->FireSuppressedSound.ToSoftObjectPath(); },
			[this](const FSoftObjectPath& Path) { Working->FireSuppressedSound = Path; },
			true));
	Rows->AddSlot().AutoHeight().Padding(0.f, 3.f)
	[
		SNew(SCheckBox)
		.IsChecked_Lambda([this] { return Working->bSuppressed ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
		.OnCheckStateChanged_Lambda([this](ECheckBoxState State) { Working->bSuppressed = State == ECheckBoxState::Checked; })
		[
			SNew(STextBlock).Text(LOCTEXT("Suppressed", "Use the quieter sound while suppressed"))
		]
	];
	AddRow(LOCTEXT("SoundTrigger", "Sound trigger"), LOCTEXT("SoundTriggerTip", "Optional input name from 1 to 64 characters. Blank uses this hold's normal attack input."),
		SNew(SEditableTextBox)
		.HintText(LOCTEXT("SoundTriggerHint", "Leave blank for the usual attack"))
		.Text_Lambda([this] { return FText::FromString(Working->FireSoundTrigger); })
		.OnTextChanged_Lambda([this](const FText& Value) { Working->FireSoundTrigger = Value.ToString().Left(64); }));

	return SNew(SScrollBox) + SScrollBox::Slot()[Rows];
}

TSharedRef<SWidget> SMantleWeaponBuilder::MakeThumbnailStep()
{
	return SNew(SScrollBox)
		+ SScrollBox::Slot()
		[
			SNew(SMantleThumbnailSection)
				.Target(FMantleThumbnailTarget::Square())
				.AngleSliders(true)
				.DefaultAngle(FMantleModelThumbnail::DefaultView())
				.Choice_Lambda([this] { return ThumbnailChoice; })
				.OnChoiceChanged_Lambda([this](const FMantleThumbnailChoice& Choice)
				{
					ThumbnailChoice = Choice;
					SaveThumbnailChoice(MantleWeaponBuilderLogic::SafeAssetName(Working->DisplayName));
				})
				.Render(FMantleModelThumbnail::RenderFor([this]() -> UObject* { return LoadModel(); }))
		];
}

void SMantleWeaponBuilder::LoadThumbnailChoice(const FString& SafeName)
{
	FMantleThumbnailStore::Load(SafeName, TEXT("weapon"), ThumbnailChoice, FMantleThumbnailStore::DefaultConfigFile());
}

void SMantleWeaponBuilder::SaveThumbnailChoice(const FString& SafeName)
{
	FMantleThumbnailStore::Save(SafeName, TEXT("weapon"), ThumbnailChoice, FMantleThumbnailStore::DefaultConfigFile());
}

TSharedRef<SWidget> SMantleWeaponBuilder::MakeCreateStep()
{
	using namespace MantleWeaponBuilderPanelPrivate;
	return SNew(SScrollBox)
		+ SScrollBox::Slot()
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Text_Lambda([this] { return SummaryText(); })
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.f, 16.f, 0.f, 0.f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(SButton)
					.Text(LOCTEXT("TestLocally", "Test locally"))
					.IsEnabled_Lambda([this] { return CanTestLocally(); })
					.ToolTipText_Lambda([this]
					{
						return CanTestLocally()
							? LOCTEXT("TestLocallyTip", "Opens the game with this weapon in your hands.")
							: LOCTEXT("TestLocallyTipOff", "Press Create weapon first. If you change anything after that, press it again.");
					})
					.OnClicked_Lambda([this]
					{
						TestLocally();
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.f)
				.VAlign(VAlign_Center)
				.Padding(12.f, 0.f, 0.f, 0.f)
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.Text_Lambda([this]
					{
						return CanTestLocally()
							? LOCTEXT("TestLocallyHelp", "Opens the game with this weapon in your hands.")
							: LOCTEXT("TestLocallyHelpOff", "Press Create weapon first, and again after any change. Then you can try it here.");
					})
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.f, 8.f, 0.f, 0.f)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Visibility_Lambda([this] { return TestResult.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
				.ColorAndOpacity_Lambda([this] { return FSlateColor(bTestResultIsError ? WarnColour() : GoodColour()); })
				.Text_Lambda([this] { return TestResult; })
			]
			// #840 step 6: publish to the creator's library, from where a place stores it (Place Settings > Storage).
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.f, 20.f, 0.f, 0.f)
			[
				SNew(STextBlock)
				.Font(FAppStyle::GetFontStyle("BoldFont"))
				.Text(LOCTEXT("PublishHeading", "Publish to your library"))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.f, 6.f, 0.f, 0.f)
			[
				SNew(SCheckBox)
				.Style(&FAppStyle::Get().GetWidgetStyle<FCheckBoxStyle>("RadioButton"))
				.IsChecked_Lambda([this] { return PublishChoice.bPublic ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([this](ECheckBoxState State) { if (State == ECheckBoxState::Checked) { PublishChoice.bPublic = true; } })
				[
					SNew(STextBlock).Text(LOCTEXT("PublishPublic", "Public: other creators can find it and use it in their places"))
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.f, 4.f, 0.f, 0.f)
			[
				SNew(SCheckBox)
				.Style(&FAppStyle::Get().GetWidgetStyle<FCheckBoxStyle>("RadioButton"))
				.IsChecked_Lambda([this] { return PublishChoice.bPublic ? ECheckBoxState::Unchecked : ECheckBoxState::Checked; })
				.OnCheckStateChanged_Lambda([this](ECheckBoxState State) { if (State == ECheckBoxState::Checked) { PublishChoice.bPublic = false; } })
				[
					SNew(STextBlock).Text(LOCTEXT("PublishPrivate", "Private: only you can use it"))
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.f, 8.f, 0.f, 0.f)
			[
				SNew(SCheckBox)
				.Visibility_Lambda([this] { return PublishChoice.bPublic ? EVisibility::Visible : EVisibility::Collapsed; })
				.IsChecked_Lambda([this] { return PublishChoice.bRightsDeclared ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([this](ECheckBoxState State) { PublishChoice.bRightsDeclared = State == ECheckBoxState::Checked; })
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.Text(LOCTEXT("PublishRights", "I own this content or its licence permits public redistribution on Mantle"))
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.f, 10.f, 0.f, 0.f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(SButton)
					.ButtonStyle(&FAppStyle::Get(), "PrimaryButton")
					.Text(LOCTEXT("Publish", "Publish"))
					.IsEnabled_Lambda([this] { return CanPublish(); })
					.ToolTipText(LOCTEXT("PublishTip", "Uploads this weapon and adds it to your Mantle library. Publishing again after a change adds a new version."))
					.OnClicked_Lambda([this]
					{
						Publish();
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.f)
				.VAlign(VAlign_Center)
				.Padding(12.f, 0.f, 0.f, 0.f)
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.Text_Lambda([this]
					{
						if (!CanTestLocally())
						{
							return LOCTEXT("PublishHelpOff", "Press Create weapon first, and again after any change. Then you can publish it.");
						}
						if (PublishChoice.bPublic && !PublishChoice.bRightsDeclared)
						{
							return LOCTEXT("PublishHelpRights", "Tick the declaration to publish publicly, or choose Private.");
						}
						return LOCTEXT("PublishHelp", "Adds this weapon to your library. A place then stores it in Place Settings > Storage.");
					})
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.f, 8.f, 0.f, 0.f)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Visibility_Lambda([this]
				{
					bool bError = false;
					return PublishStatusText(bError).IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible;
				})
				.ColorAndOpacity_Lambda([this]
				{
					bool bError = false;
					PublishStatusText(bError);
					return FSlateColor(bError ? WarnColour() : GoodColour());
				})
				.Text_Lambda([this]
				{
					bool bError = false;
					return PublishStatusText(bError);
				})
			]
		];
}

bool SMantleWeaponBuilder::CanPublish() const
{
	return CanTestLocally() && !CreatedSafeName.IsEmpty() && PublishChoice.IsReady() && !FMantleWeaponPublish::Get().IsBusy();
}

void SMantleWeaponBuilder::Publish()
{
	if (!CanPublish())
	{
		return;
	}
	FString Error;
	PublishRefusal = FMantleWeaponPublish::Get().Start(CreatedSafeName, PublishChoice, Error) ? FText::GetEmpty() : FText::FromString(Error);
}

FText SMantleWeaponBuilder::PublishStatusText(bool& bOutIsError) const
{
	bOutIsError = false;
	if (!PublishRefusal.IsEmpty())
	{
		bOutIsError = true;
		return PublishRefusal;
	}
	const FMantleWeaponPublish& Publisher = FMantleWeaponPublish::Get();
	const TSharedPtr<FMantlePlacePublishSession> Session = Publisher.GetSession();
	if (!Session.IsValid() || CreatedSafeName.IsEmpty() || !Publisher.GetWeaponName().Equals(CreatedSafeName, ESearchCase::IgnoreCase))
	{
		return FText::GetEmpty();
	}
	if (!Session->IsFinished())
	{
		return FText::FromString(FString::Printf(TEXT("%s (%d%%)"), *Session->GetStatusText(), FMath::RoundToInt(Session->GetProgress() * 100.f)));
	}
	bOutIsError = !Session->Succeeded();
	FString Said = Session->GetResult().Message;
	// The first thing to fix, when the publish names one (a material that could not be prepared, a refused file).
	if (bOutIsError)
	{
		for (const TArray<FMantleValidationIssue>* Issues : { &Session->GetResult().Validation.Issues, &Session->GetAttention() })
		{
			if (const FMantleValidationIssue* First = Issues->FindByPredicate([](const FMantleValidationIssue& Issue) { return Issue.Severity == EMantleIssueSeverity::Error; }))
			{
				Said += TEXT(" ") + First->Problem + (First->Remedy.IsEmpty() ? FString() : TEXT(" ") + First->Remedy);
				break;
			}
		}
	}
	return FText::FromString(Said);
}

bool SMantleWeaponBuilder::CanTestLocally() const
{
	return !bSelectedAssetEditedSinceCreate
		&& MantleWeaponBuilderLogic::CanTestLocally(CreatedSignature, MantleWeaponBuilderLogic::EditSignature(*Working));
}

bool SMantleWeaponBuilder::IsSelectedAssetPackage(const UObject* Object) const
{
	if (!Object || !Object->GetOutermost())
	{
		return false;
	}
	const FString PackageName = Object->GetOutermost()->GetName();
	auto Matches = [&PackageName](const FSoftObjectPath& Path)
	{
		return !Path.IsNull() && Path.GetLongPackageName().Equals(PackageName, ESearchCase::IgnoreCase);
	};
	if (Matches(Working->Model.ToSoftObjectPath()) || Matches(Working->Projectile.Model.ToSoftObjectPath())
		|| Matches(Working->FireSound.ToSoftObjectPath()) || Matches(Working->FireSuppressedSound.ToSoftObjectPath())
		|| Matches(SourceAssetPath))
	{
		return true;
	}
	for (const TSoftObjectPtr<UObject>& Section : Working->MagazineSections)
	{
		if (Matches(Section.ToSoftObjectPath())) return true;
	}
	for (const FMantleWeaponSourcePart& Part : Working->Parts)
	{
		if (Matches(Part.Model.ToSoftObjectPath())) return true;
	}
	for (const FMantleWeaponSourceAttachment& Attachment : Working->Attachments)
	{
		if (Matches(Attachment.Model.ToSoftObjectPath())) return true;
	}
	return false;
}

void SMantleWeaponBuilder::OnObjectPropertyChanged(UObject* Object, FPropertyChangedEvent& Event)
{
	if (!IsSelectedAssetPackage(Object))
	{
		return;
	}
	bSelectedAssetEditedSinceCreate = true;
	CreatedSignature.Reset();
	TestResult = LOCTEXT("SelectedAssetChanged", "A selected model or sound changed. Create the weapon again before testing it.");
	bTestResultIsError = false;
}

void SMantleWeaponBuilder::TestLocally()
{
	if (!CanTestLocally())
	{
		return;
	}
	TestResult = LOCTEXT("TestStarting", "Getting your weapon ready to try...");
	bTestResultIsError = false;

	FString Error;
	FString Notice;
	const bool bOk = FMantleTestLocally::Get().RunWithWeapon(FMantleWeaponToTry::FromBuilderName(CreatedSafeName), Error, Notice);
	if (bOk)
	{
		FString Said = TEXT("The game is opening with your weapon in your hands.");
		if (!Notice.IsEmpty())
		{
			Said += TEXT(" ") + Notice;
		}
		TestResult = FText::FromString(Said);
		bTestResultIsError = false;
	}
	else
	{
		TestResult = FText::FromString(Error.IsEmpty() ? FString(TEXT("The test was cancelled.")) : Error);
		bTestResultIsError = !Error.IsEmpty();
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// Create.

void SMantleWeaponBuilder::Create()
{
	using namespace MantleWeaponBuilderLogic;
	CreatedSignature.Reset();
	bSelectedAssetEditedSinceCreate = false;
	TestResult = FText::GetEmpty();

	// A weapon this window has not made yet never lands on a weapon that is already saved: it takes the next free name.
	FText RenamedNote;
	const bool bUpdatingSameWeapon = !CreatedSafeName.IsEmpty() && SafeAssetName(Working->DisplayName).Equals(CreatedSafeName, ESearchCase::IgnoreCase);
	if (!bUpdatingSameWeapon)
	{
		TArray<FString> TakenSafeNames;
		TArray<FString> Folders;
		const FString WeaponsFolder = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / TEXT("MantleWeapons"));
		IFileManager::Get().FindFiles(Folders, *(WeaponsFolder / TEXT("*")), false, true);
		for (const FString& Folder : Folders)
		{
			// An empty folder left by a failed try holds no weapon.
			TArray<FString> Files;
			IFileManager::Get().FindFilesRecursive(Files, *(WeaponsFolder / Folder), TEXT("*.*"), true, false);
			if (Files.Num() > 0)
			{
				TakenSafeNames.Add(Folder);
				// Pre-60-character outputs are kept intact, but reserve the canonical prefix too so
				// a new source cannot collide with an older long folder after safe-name trimming.
				const FString CanonicalFolder = SafeAssetName(Folder);
				if (!CanonicalFolder.Equals(Folder, ESearchCase::IgnoreCase))
				{
					TakenSafeNames.Add(CanonicalFolder);
				}
			}
		}
		const FString Wanted = Working->DisplayName.TrimStartAndEnd();
		const FString Free = FirstFreeDisplayName(Wanted, TakenSafeNames);
		if (Free != Wanted)
		{
			RenamedNote = FText::Format(LOCTEXT("NameTaken", "A weapon called {0} already exists, so this one is saved as {1}."), FText::FromString(Wanted), FText::FromString(Free));
			Working->DisplayName = Free;
		}
	}

	const FString PackagePath = SourcePackagePath(Working->DisplayName);
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);

	UPackage* Package = CreatePackage(*PackagePath);
	UMantleWeaponSource* Asset = Package ? FindObject<UMantleWeaponSource>(Package, *AssetName) : nullptr;
	const bool bNew = Asset == nullptr;
	if (Package && !Asset)
	{
		Asset = NewObject<UMantleWeaponSource>(Package, FName(*AssetName), RF_Public | RF_Standalone);
	}

	bool bSaved = false;
	if (Asset)
	{
		Working->DisplayName = Working->DisplayName.TrimStartAndEnd();
		Working->Tuning = ClampTuning(Working->Tuning);
		MantleWeaponBuilderPanelPrivate::CopySourceProperties(*Working, *Asset);
		Package->MarkPackageDirty();
		if (bNew)
		{
			FAssetRegistryModule::AssetCreated(Asset);
		}

		const FString Filename = FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;
		bSaved = UPackage::SavePackage(Package, Asset, *Filename, SaveArgs);
	}

	if (!bSaved)
	{
		FNotificationInfo Info(LOCTEXT("SaveFailed", "The weapon could not be saved."));
		Info.SubText = LOCTEXT("SaveFailedSub", "Check that your project's files are not read-only, then try again.");
		Info.ExpireDuration = 10.f;
		FSlateNotificationManager::Get().AddNotification(Info)->SetCompletionState(SNotificationItem::CS_Fail);
		return;
	}

	CreatedSafeName = SafeAssetName(Working->DisplayName);
	// #124: the picture's choice goes with the weapon Create wrote, under its final name, so publishing finds it.
	SaveThumbnailChoice(CreatedSafeName);
	SourceAssetPath = FSoftObjectPath(Asset);
	SourceBaselineSignature = EditSignature(*Asset);

	TArray<FText> Messages;
	if (!RenamedNote.IsEmpty())
	{
		Messages.Add(RenamedNote);
	}
	const bool bExported = FMantleWeaponBuilderExport::Export(*Asset, Messages);
	// Test locally is offered for exactly what was just written, and only if its files were.
	CreatedSignature = bExported ? EditSignature(*Asset) : FString();
	bSelectedAssetEditedSinceCreate = false;
	TestResult = FText::GetEmpty();
	FString Joined;
	for (const FText& Message : Messages)
	{
		Joined += (Joined.IsEmpty() ? TEXT("") : TEXT("\n")) + Message.ToString();
	}

	FNotificationInfo Info(bExported
		? FText::Format(LOCTEXT("Saved", "Saved {0}."), FText::FromString(Asset->DisplayName))
		: FText::Format(LOCTEXT("NotReady", "{0} isn't ready yet."), FText::FromString(Asset->DisplayName)));
	Info.SubText = FText::FromString(Joined);
	Info.ExpireDuration = 8.f;
	FSlateNotificationManager::Get().AddNotification(Info)->SetCompletionState(bExported ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
}

#undef LOCTEXT_NAMESPACE
