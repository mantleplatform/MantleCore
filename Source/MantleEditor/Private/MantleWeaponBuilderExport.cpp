// Copyright Joshua Eger. All Rights Reserved.
#include "MantleWeaponBuilderExport.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "HAL/FileManager.h"
#include "MantleBuilderPoint.h"
#include "MantleWeaponBuilderKind.h"
#include "MantleWeaponBuilderLogic.h"
#include "MantleWeaponDocument.h"
#include "MantleWeaponRigidMesh.h"
#include "MantleWeaponSource.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "SMantleBuilderViewport.h"
#include "Sound/SoundWave.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#define LOCTEXT_NAMESPACE "MantleWeaponBuilderExport"
DEFINE_LOG_CATEGORY_STATIC(LogMantleWeaponBuilderExport, Log, All);

namespace MantleWeaponBuilderExportPrivate
{
	FString SafeName(const UMantleWeaponSource& Source)
	{
		return MantleWeaponBuilderLogic::SafeAssetName(Source.DisplayName.TrimStartAndEnd().IsEmpty()
			? MantleWeaponBuilderLogic::DefaultName(Source.Hold) : Source.DisplayName.TrimStartAndEnd());
	}

	FString AuxiliaryName(const UMantleWeaponSource& Source, const TCHAR* Kind, int32 Index = 0)
	{
		const FString Suffix = FString::Printf(TEXT("_%s_%d"), Kind, Index);
		const FString Base = SafeName(Source);
		return Base.Left(FMath::Max(0, 64 - Suffix.Len())) + Suffix;
	}

	bool IsMesh(UObject* Asset) { return Cast<UStaticMesh>(Asset) || Cast<USkeletalMesh>(Asset); }

	/** Preserve existing files before any generated asset is replaced. Failed multi-asset builds remain recoverable. */
	struct FOutputBackup
	{
		FString Folder;
		FString BackupFolder;
		TArray<FString> Files;
		TArray<FText>& Messages;
		bool bReady = false;
		bool bComplete = false;
		FOutputBackup(const FString& Name, TArray<FText>& InMessages) : Messages(InMessages)
		{
			Folder = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / TEXT("MantleWeapons") / Name);
			BackupFolder = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("MantleWeaponBuilder/Backups") / Name / FGuid::NewGuid().ToString(EGuidFormats::Digits));
			IFileManager::Get().FindFiles(Files, *(Folder / TEXT("*")), true, false);
			if (!Files.IsEmpty()) IFileManager::Get().MakeDirectory(*BackupFolder, true);
			for (const FString& File : Files)
			{
				if (IFileManager::Get().Copy(*(BackupFolder / File), *(Folder / File)) != COPY_OK)
				{
					Messages.Add(LOCTEXT("BackupFailed", "The previous weapon files could not be backed up. Check that the output and Saved folders are writable."));
					return;
				}
			}
			bReady = true;
		}
		~FOutputBackup()
		{
			if (!bReady || Files.IsEmpty()) return;
			if (bComplete)
			{
				const FString Base = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("MantleWeaponBuilder/Backups")) + TEXT("/");
				FGuid Id;
				if (BackupFolder.StartsWith(Base, ESearchCase::IgnoreCase) && FGuid::ParseExact(FPaths::GetCleanFilename(BackupFolder), EGuidFormats::Digits, Id))
					IFileManager::Get().DeleteDirectory(*BackupFolder, false, true);
				return;
			}
			bool bRestored = true;
			for (const FString& File : Files)
			{
				if (IFileManager::Get().IsReadOnly(*(Folder / File))) { bRestored = false; continue; }
				bRestored = IFileManager::Get().Copy(*(Folder / File), *(BackupFolder / File)) == COPY_OK && bRestored;
			}
			Messages.Add(FText::Format(bRestored ? LOCTEXT("PriorFilesRestored", "Previous saved files were restored on disk. Reload the project before using them. Their backup is in {0}.")
				: LOCTEXT("PriorFilesRecoverable", "Previous saved files are preserved in {0}; some output files could not be restored. Recover them from the backup and reload the project before using them."), FText::FromString(BackupFolder)));
		}
	};

	/** A unit quaternion the document accepts: rounding can leave a component a hair past 1 (1.0000000000000002), which the
	 *  schema refuses. */
	FQuat DocumentRotation(FQuat Rotation)
	{
		Rotation.Normalize();
		return FQuat(FMath::Clamp(Rotation.X, -1.0, 1.0), FMath::Clamp(Rotation.Y, -1.0, 1.0), FMath::Clamp(Rotation.Z, -1.0, 1.0),
			FMath::Clamp(Rotation.W, -1.0, 1.0));
	}

	FMantleWeaponDocSocket MakeSocket(const FTransform& Transform)
	{
		FMantleWeaponDocSocket Socket;
		Socket.Bone = TEXT("root");
		Socket.Location.Set(Transform.GetLocation());
		Socket.Rotation.Set(DocumentRotation(Transform.GetRotation()));
		return Socket;
	}

	EMantleWeaponDocRole PartRole(EMantleWeaponSourcePartRole Role)
	{
		switch (Role)
		{
		case EMantleWeaponSourcePartRole::Magazine: return EMantleWeaponDocRole::Magazine;
		case EMantleWeaponSourcePartRole::Round: return EMantleWeaponDocRole::Round;
		case EMantleWeaponSourcePartRole::Arrow: return EMantleWeaponDocRole::Arrow;
		}
		return EMantleWeaponDocRole::Count;
	}

	bool AddPart(FMantleWeaponDesc& Desc, EMantleWeaponDocRole Role, const TArray<EMantleWeaponSourceAction>& ShownDuring,
		TArray<FText>& Messages)
	{
		if (Role == EMantleWeaponDocRole::Count)
		{
			Messages.Add(LOCTEXT("UnknownPart", "Pick a magazine, round or arrow role for the separate part."));
			return false;
		}
		if (!Desc.Parts.bSet) Desc.Parts.Set(FMantleWeaponDocParts());
		FMantleWeaponDocPart Part;
		Part.Bone = FMantleWeaponDocument::RoleName(Role);
		TArray<EMantleWeaponDocAction> Actions;
		for (EMantleWeaponSourceAction Action : ShownDuring) Actions.Add(static_cast<EMantleWeaponDocAction>(Action));
		const FMantleWeaponDocHoldInfo& Info = FMantleWeaponDocument::GetHoldInfo(Desc.GetHold());
		if (Actions.IsEmpty() && Info.StowedRoles.Contains(Role))
		{
			for (EMantleWeaponDocAction Action : Info.Actions)
			{
				if (Role == EMantleWeaponDocRole::Arrow
					? Action == EMantleWeaponDocAction::Nock || Action == EMantleWeaponDocAction::Draw || Action == EMantleWeaponDocAction::Release
					: Action >= EMantleWeaponDocAction::ReloadTactical && Action <= EMantleWeaponDocAction::ReloadPartial)
				{
					Actions.Add(Action);
				}
			}
		}
		if (!Actions.IsEmpty()) Part.ShownDuring.Set(Actions);
		const auto& Existing = Desc.Parts.Value[Role];
		if (Existing.bSet && (Existing.Value.ShownDuring.bSet != Part.ShownDuring.bSet || Existing.Value.ShownDuring.Value != Part.ShownDuring.Value))
		{
			Messages.Add(LOCTEXT("PartActionsDiffer", "Sections of the same separate part must use the same visible actions."));
			return false;
		}
		Desc.Parts.Value[Role].Set(Part);
		return true;
	}

	bool CopyAsset(UObject* Original, const FString& Folder, const FString& Name, TArray<FText>& Messages)
	{
		const FString PackageName = Folder / Name;
		UPackage* Package = LoadPackage(nullptr, *PackageName, LOAD_NoWarn | LOAD_Quiet);
		if (!Package) Package = CreatePackage(*PackageName);
		if (!Package) { Messages.Add(FText::FromString(FString::Printf(TEXT("Could not create the asset package for %s."), *Name))); return false; }
		UObject* Existing = FindObject<UObject>(Package, *Name);
		const EObjectFlags ExistingFlags = Existing ? Existing->GetFlags() : RF_NoFlags;
		if (Existing)
		{
			Existing->ClearFlags(RF_Public | RF_Standalone);
			if (!Existing->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional))
			{
				Existing->SetFlags(ExistingFlags);
				Messages.Add(FText::FromString(FString::Printf(TEXT("Could not prepare the existing %s asset for replacement."), *Name)));
				return false;
			}
		}
		auto RestoreExisting = [&]
		{
			if (Existing)
			{
				Existing->Rename(*Name, Package, REN_DontCreateRedirectors | REN_NonTransactional);
				Existing->SetFlags(ExistingFlags);
			}
		};
		UObject* Copy = StaticDuplicateObject(Original, Package, FName(*Name));
		if (!Copy) { RestoreExisting(); Messages.Add(FText::FromString(FString::Printf(TEXT("Could not copy the selected %s asset."), *Name))); return false; }
		Copy->SetFlags(RF_Public | RF_Standalone);
		Package->MarkPackageDirty();
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
		if (!UPackage::SavePackage(Package, Copy, *Filename, Args))
		{
			Copy->ClearFlags(RF_Public | RF_Standalone);
			Copy->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
			Copy->MarkAsGarbage();
			RestoreExisting();
			Messages.Add(FText::Format(LOCTEXT("AuxSaveFailed", "Could not save the weapon's {0} asset. Check that its folder is writable."), FText::FromString(Name)));
			return false;
		}
		if (Existing) { FAssetRegistryModule::AssetDeleted(Existing); Existing->MarkAsGarbage(); }
		FAssetRegistryModule::AssetCreated(Copy);
		return true;
	}
}

bool FMantleWeaponBuilderExport::BuildDocument(const UMantleWeaponSource& Source, const TMap<FName, FTransform>& PointsInWeapon,
	const FString& MeshName, FMantleWeaponDesc& Out, TArray<FText>& OutMessages)
{
	using namespace MantleWeaponBuilderExportPrivate;
	using namespace MantleWeaponBuilderLogic;
	Out = FMantleWeaponDesc();
	if (Source.Hold >= EMantleWeaponHold::Count || Source.Delivery > EMantleWeaponSourceDelivery::Use || Source.Resource > EMantleWeaponSourceResource::None)
	{
		OutMessages.Add(LOCTEXT("InvalidChoice", "Pick a supported hold, delivery and resource for your weapon."));
		return false;
	}
	FMantleWeaponDesc Desc;
	Desc.Hold.Set(ToDocHold(Source.Hold));
	const FMantleWeaponDocHoldInfo& Info = FMantleWeaponDocument::GetHoldInfo(Desc.GetHold());
	Desc.Name = Source.DisplayName.TrimStartAndEnd().IsEmpty() ? DefaultName(Source.Hold) : Source.DisplayName.TrimStartAndEnd().Left(64);
	Desc.Mesh = MeshName;
	// The hotbar icon: the weapon's library picture. It is made at publish (the Thumbnail step's choice); publish swaps this name for the
	// uploaded texture's id, or drops it when no picture was taken.
	if (!MeshName.IsEmpty()) Desc.Icon = AuxiliaryName(Source, TEXT("Icon"));
	Desc.Delivery.Set(Source.Delivery == EMantleWeaponSourceDelivery::Default ? UsualDelivery(Desc.GetHold())
		: static_cast<EMantleWeaponDocDelivery>(static_cast<uint8>(Source.Delivery) - 1));
	if (!IsDeliveryOffered(Desc.GetHold(), Desc.GetDelivery()))
	{
		OutMessages.Add(LOCTEXT("DeliveryNotOffered", "This hold cannot use that delivery: pick one of its attack styles."));
		return false;
	}
	// #909: a staff's held beam may also throw spells (a tap casts, aim and a tap casts aimed, holding is the beam, aim and holding charges).
	const bool bSpellsBesideBeam = Source.bSpellsBesideBeam && Desc.GetHold() == EMantleWeaponDocHold::Staff
		&& Desc.GetDelivery() == EMantleWeaponDocDelivery::Beam;
	Desc.Resource.Set(Source.Resource == EMantleWeaponSourceResource::Default ? Info.Resources[0]
		: static_cast<EMantleWeaponDocResource>(static_cast<uint8>(Source.Resource) - 1));
	FMantleWeaponDocument::ArchetypeOfHold(Desc.GetHold(), Desc.Archetype);
	Desc.bTwoHanded.Set(Source.bOverrideTwoHanded ? Source.bTwoHanded : Info.bTwoHanded);
	// #904: a gun takes the rifle, pistol or shotgun style; the elements are for the staff, the bow and throws. #905: a gun that launches
	// rounds or fires a beam may take an element too (a laser rifle, a fire launcher); its instant shot may not.
	const bool bStyleIsElement = FMantleWeaponDocument::IsElement(static_cast<EMantleWeaponDocEffects>(Source.Style));
	if (Source.bOverrideStyle && (Info.bHasEffects ? bStyleIsElement && !PicksElement(Source.Hold, Desc.GetDelivery()) : !bStyleIsElement))
	{
		OutMessages.Add(Info.bHasEffects
			? LOCTEXT("StyleGunOnly", "A gun's instant shot uses the rifle, pistol or shotgun effect style. Pick a flying round or a beam for an element such as laser or fire.")
			: LOCTEXT("StyleElementOnly", "Pick an element (fire, ice, lightning, arcane...) for this weapon's effect style."));
		return false;
	}
	if (Source.bOverrideStyle) Desc.Effects.Set(static_cast<EMantleWeaponDocEffects>(Source.Style));
	else if (Info.bHasEffects && Desc.GetDelivery() != EMantleWeaponDocDelivery::Sweep) Desc.Effects.Set(Info.Effects);
	// The custom colour tints an element's effects (effectsColor); on a gun it is the accent colour.
	EMantleWeaponDocEffects Element = EMantleWeaponDocEffects::Arcane;
	if (Source.bOverrideColor && Desc.GetElement(Element)) Desc.EffectsColor.Set(Source.Color);
	else if (Source.bOverrideColor) Desc.Color.Set(Source.Color);
	if (!MeshName.IsEmpty())
	{
		for (const auto& Point : PointsInWeapon)
		{
			EMantleWeaponDocSocket Socket;
			if (DocSocket(Point.Key, Socket) && (Info.Sockets.Contains(Socket) || Info.OptionalSockets.Contains(Socket)))
			{
				Desc.Socket(Socket).Set(MakeSocket(Point.Value));
			}
		}
	}
	if (Desc.GetHold() == EMantleWeaponDocHold::Consumable)
	{
		// #1025: a consumable is used up, never fired: no attack, spread, recoil, parts, attachments or sounds, just its own block.
		const FMantleWeaponSourceConsumable& C = Source.Consumable;
		const bool bHeals = C.Effect == EMantleWeaponSourceConsumableEffect::Heal;
		if (bHeals && (!FMath::IsFinite(C.Heal) || C.Heal < 1.f || C.Heal > 1000.f || !FMath::IsFinite(C.HealSeconds) || C.HealSeconds < 0.f || C.HealSeconds > 60.f))
		{
			OutMessages.Add(LOCTEXT("BadHeal", "Set the health between 1 and 1000, and the time to spread it over between 0 and 60 seconds."));
			return false;
		}
		if (C.Uses < 1 || C.Uses > 100)
		{
			OutMessages.Add(LOCTEXT("BadUses", "Set the uses between 1 and 100."));
			return false;
		}
		if (C.bOverrideUseSeconds && (!FMath::IsFinite(C.UseSeconds) || C.UseSeconds < 0.5f || C.UseSeconds > 10.f))
		{
			OutMessages.Add(LOCTEXT("BadUseSeconds", "Set the use time between 0.5 and 10 seconds."));
			return false;
		}
		if (!FMath::IsFinite(C.CooldownSeconds) || C.CooldownSeconds < 0.f || C.CooldownSeconds > 600.f)
		{
			OutMessages.Add(LOCTEXT("BadCooldown", "Set the wait between uses between 0 and 600 seconds."));
			return false;
		}
		FMantleWeaponDocConsumable Consumable;
		Consumable.Style = C.Style == EMantleWeaponSourceUseStyle::EatDrink ? EMantleWeaponDocUseStyle::EatDrink : EMantleWeaponDocUseStyle::Use;
		if (bHeals)
		{
			Consumable.Heal.Set(C.Heal);
			Consumable.HealSeconds.Set(C.HealSeconds);
		}
		else
		{
			// The document refuses heal and healSeconds beside effect none.
			Consumable.Effect.Set(EMantleWeaponDocConsumableEffect::None);
		}
		Consumable.Uses.Set(C.Uses);
		if (C.bOverrideUseSeconds) Consumable.UseSeconds.Set(C.UseSeconds);
		Consumable.CooldownSeconds.Set(C.CooldownSeconds);
		Consumable.bMoveWhileUsing.Set(C.bMoveWhileUsing);
		Desc.Consumable.Set(Consumable);
		TArray<FMantleWeaponDocError> ConsumableErrors;
		if (!FMantleWeaponDocument::Validate(Desc, ConsumableErrors))
		{
			for (const auto& Error : ConsumableErrors)
			{
				OutMessages.Add(FText::FromString(Error.ToString()));
				UE_LOG(LogMantleWeaponBuilderExport, Warning, TEXT("Weapon '%s' settings refused: %s"), *Desc.Name, *Error.ToString());
			}
			return false;
		}
		Out = MoveTemp(Desc);
		return true;
	}
	const FMantleWeaponTuning& T = Source.Tuning;
	// Export validates the creator's numbers. Clamping belongs to interactive controls, never to saved output.
	if (!FMath::IsFinite(T.ShotsPerSecond) || T.ShotsPerSecond < 0.1f || T.ShotsPerSecond > 100.f)
	{
		OutMessages.Add(LOCTEXT("BadAttackRate", "Set the attack rate between 0.1 and 100 attacks per second."));
		return false;
	}
	Desc.Fire.Interval = 1.0 / static_cast<double>(T.ShotsPerSecond);
	Desc.Fire.bAutomatic.Set(T.bAutomatic);
	TArray<EMantleWeaponDocFireMode> Modes = { EMantleWeaponDocFireMode::Semi };
	if (T.bBurstMode) Modes.Add(EMantleWeaponDocFireMode::Burst);
	if (T.bAutomatic) Modes.Add(EMantleWeaponDocFireMode::Auto);
	Desc.Fire.Modes.Set(Modes);
	Desc.Fire.Damage.Set(T.Damage);
	const bool bPistol = Info.bHasEffects && Info.Effects == EMantleWeaponDocEffects::Pistol;
	Desc.Fire.MaxRange.Set(T.MaxRange == 0.f ? (bPistol ? 15000.0 : 25000.0) : static_cast<double>(T.MaxRange));
	Desc.Fire.TraceSweepRadius.Set(T.TraceSweepRadius);
	Desc.Fire.Pellets.Set(T.Pellets);
	Desc.Fire.PelletSpread.Set(T.PelletSpread);
	Desc.Fire.PlainRecoil.Set(T.PlainRecoil == -1.f ? (bPistol ? 0.5 : 0.35) : static_cast<double>(T.PlainRecoil));
	if (Desc.UsesRounds())
	{
		Desc.Magazine.Size = T.MagazineSize;
		Desc.Magazine.StartingReserve.Set(T.SpareAmmo);
		Desc.Reload.Style = Info.ReloadStyle;
		Desc.Reload.Seconds.Set(T.ReloadSeconds);
	}
	if (Source.Charge.bEnabled || Info.Charge == EMantleWeaponDocChargeRule::Required)
	{
		FMantleWeaponDocCharge Charge;
		Charge.Seconds = Source.Charge.Seconds;
		Charge.bRequired.Set(Source.Charge.bRequired || Info.Charge == EMantleWeaponDocChargeRule::Required);
		Charge.bReleaseOnFull.Set(Source.Charge.bReleaseOnFull);
		Charge.bCancelable.Set(Source.Charge.bCancelable);
		Desc.Charge.Set(Charge);
	}
	if (Source.bBlock || Info.BlockRule == EMantleWeaponDocBlockRule::Required)
	{
		FMantleWeaponDocBlock Block;
		Block.bEnabled.Set(true);
		Block.bBreakOnHeavy.Set(Source.bBreakOnHeavy);
		Desc.Block.Set(Block);
	}
	// A hold whose action is a throw (the spear beside its sweep, the throwable alone) throws its own model: the projectile block
	// rides along, and the thrown weapon sticks where it lands and can be picked up again, with no choice to make.
	const bool bThrowsItsModel = Info.Actions.Contains(EMantleWeaponDocAction::Throw);
	const bool bThrowsBesideSweep = Desc.GetDelivery() == EMantleWeaponDocDelivery::Sweep && bThrowsItsModel;
	if (Desc.GetDelivery() == EMantleWeaponDocDelivery::Projectile || bThrowsBesideSweep || bSpellsBesideBeam)
	{
		const auto& P = Source.Projectile;
		FMantleWeaponDocProjectile Projectile;
		Projectile.Speed = P.Speed;
		Projectile.Gravity.Set(P.Gravity);
		// Only a throwable explodes (a spear's blast row is never offered, so a leftover value is not written). An exploding
		// throwable is a grenade: it is used up where it goes off, so it neither sticks nor is picked up, and it carries a fuse.
		const bool bExplodes = Desc.GetHold() == EMantleWeaponDocHold::Throwable && P.BlastRadius > 0.f;
		// A throwable that leaves fire (a molotov) breaks where it lands and is used up too; it never explodes as well.
		const bool bBurns = !bExplodes && Desc.GetHold() == EMantleWeaponDocHold::Throwable && P.LingerSeconds > 0.f;
		const bool bKeepsBlast = bExplodes || !bThrowsItsModel;
		Projectile.BlastRadius.Set(bKeepsBlast ? P.BlastRadius : 0.f);
		if (bExplodes)
		{
			Projectile.FuseSeconds.Set(FMath::Clamp(P.FuseSeconds, 0.1f, 30.f));
		}
		Projectile.bThrown.Set(P.bThrown || bThrowsItsModel);
		Projectile.bStick.Set(bExplodes || bBurns ? false : (P.bStick || bThrowsItsModel));
		Projectile.bPickup.Set(bExplodes || bBurns ? false : (P.bPickup || bThrowsItsModel));
		if (bBurns)
		{
			Projectile.LingerSeconds.Set(FMath::Clamp(P.LingerSeconds, 0.1f, 30.f));
			Projectile.LingerDamagePerSecond.Set(FMath::Clamp(P.LingerDamagePerSecond, 0.f, 1000.f));
			Projectile.LingerRadius.Set(FMath::Clamp(P.LingerRadius, 10.f, 2000.f));
		}
		// #905: the round or arrow the creator picked flies as its own model (a launcher's rocket), copied beside the weapon under
		// this local name as an extra part's model is (Export); the publish names its slots' materials (projectile.materials). A thrown
		// weapon flies as itself.
		if (!P.Model.IsNull() && !Projectile.bThrown.Value)
		{
			Projectile.Mesh.Set(AuxiliaryName(Source, TEXT("Projectile")));
		}
		Desc.Projectile.Set(Projectile);
	}
	if (Desc.GetDelivery() == EMantleWeaponDocDelivery::Sweep)
	{
		FMantleWeaponDocSweep Sweep;
		Sweep.Reach = Source.Sweep.Reach;
		Sweep.ComboCount.Set(Source.Sweep.ComboCount);
		Sweep.HeavyMultiplier.Set(Source.Sweep.HeavyMultiplier);
		if (!Source.Sweep.EdgeBase.IsNone() || !Source.Sweep.EdgeTip.IsNone())
		{
			FMantleWeaponDocSweepEdge Edge;
			if (!DocSocket(Source.Sweep.EdgeBase, Edge.Base) || !DocSocket(Source.Sweep.EdgeTip, Edge.Tip))
			{
				OutMessages.Add(LOCTEXT("BadEdge", "Pick two weapon points for the swept edge, or leave both choices empty for the hold's edge."));
				return false;
			}
			Sweep.Edge.Set(Edge);
		}
		Desc.Sweep.Set(Sweep);
	}
	if (Desc.GetDelivery() == EMantleWeaponDocDelivery::Beam)
	{
		FMantleWeaponDocBeam Beam;
		Beam.TickRate.Set(Source.Beam.TickRate);
		Beam.DamagePerSecond = Source.Beam.DamagePerSecond;
		Beam.MaxSeconds.Set(Source.Beam.MaxSeconds);
		// #905: the thickness and reach only when the creator set them: 0 is a thin line (a laser), and no range reaches fire.maxRange.
		if (Source.Beam.Radius != 0.f)
		{
			Beam.Radius.Set(Source.Beam.Radius);
		}
		if (Source.Beam.Range != 0.f)
		{
			Beam.Range.Set(Source.Beam.Range);
		}
		Desc.Beam.Set(Beam);
	}
	// A staff's charges: how many casts it holds and how long one takes to come back (the validator needs them whenever the resource is charges).
	if (Desc.GetResource() == EMantleWeaponDocResource::Charges)
	{
		if (Source.Charges.Count < 1 || Source.Charges.Count > 100 || !FMath::IsFinite(Source.Charges.RefillSeconds) || Source.Charges.RefillSeconds < 0.1f || Source.Charges.RefillSeconds > 600.f)
		{
			OutMessages.Add(LOCTEXT("BadCharges", "Set the charges between 1 and 100, and the time to get one back between 0.1 and 600 seconds."));
			return false;
		}
		FMantleWeaponDocCharges Charges;
		Charges.Count = Source.Charges.Count;
		Charges.RefillSeconds = Source.Charges.RefillSeconds;
		Desc.Charges.Set(Charges);
	}
	if (!Source.MagazineSections.IsEmpty() && !AddPart(Desc, EMantleWeaponDocRole::Magazine, {}, OutMessages)) return false;
	for (const auto& Part : Source.Parts)
	{
		if (Part.Model.IsNull())
		{
			OutMessages.Add(LOCTEXT("NoPartModel", "Pick a model for each separate part, or remove the empty part."));
			return false;
		}
		if (!AddPart(Desc, PartRole(Part.Role), Part.ShownDuring, OutMessages)) return false;
	}
	if (!Source.Attachments.IsEmpty())
	{
		TArray<FMantleWeaponDocAttachment> Attachments;
		for (int32 Index = 0; Index < Source.Attachments.Num(); ++Index)
		{
			const auto& In = Source.Attachments[Index];
			FMantleWeaponDocAttachment Attachment;
			Attachment.Name = In.Name;
			if (!In.Model.IsNull()) Attachment.Mesh = AuxiliaryName(Source, TEXT("Attachment"), Index);
			if (In.bEffect) Attachment.Effect.Set(static_cast<EMantleWeaponDocEffects>(In.Effect));
			EMantleWeaponDocSocket Socket;
			if (!In.Socket.IsNone() && DocSocket(In.Socket, Socket)) Attachment.Socket.Set(Socket);
			Attachment.Location.Set(In.RelativeTransform.GetLocation());
			Attachment.Rotation.Set(DocumentRotation(In.RelativeTransform.GetRotation()));
			Attachment.Scale.Set(In.RelativeTransform.GetScale3D());
			Attachment.bHidden.Set(In.bHidden);
			Attachments.Add(Attachment);
		}
		Desc.Attachments.Set(Attachments);
	}
	if (!Source.FireSound.IsNull() || !Source.FireSuppressedSound.IsNull() || Source.bSuppressed || !Source.FireSoundTrigger.IsEmpty())
	{
		FMantleWeaponDocSounds Sounds;
		if (!Source.FireSound.IsNull()) Sounds.Fire.Set(AuxiliaryName(Source, TEXT("Fire")));
		if (!Source.FireSuppressedSound.IsNull()) Sounds.FireSuppressed.Set(AuxiliaryName(Source, TEXT("FireSuppressed")));
		Sounds.bSuppressed.Set(Source.bSuppressed);
		if (!Source.FireSoundTrigger.IsEmpty()) Sounds.Trigger.Set(Source.FireSoundTrigger);
		Desc.Sounds.Set(Sounds);
	}
	TArray<FMantleWeaponDocError> Errors;
	if (!FMantleWeaponDocument::Validate(Desc, Errors))
	{
		for (const auto& Error : Errors)
		{
			OutMessages.Add(FText::FromString(Error.ToString()));
			UE_LOG(LogMantleWeaponBuilderExport, Warning, TEXT("Weapon '%s' settings refused: %s"), *Desc.Name, *Error.ToString());
		}
		return false;
	}
	Out = MoveTemp(Desc);
	return true;
}

bool FMantleWeaponBuilderExport::Export(const UMantleWeaponSource& Source, TArray<FText>& OutMessages)
{
	using namespace MantleWeaponBuilderExportPrivate;
	using namespace MantleWeaponBuilderLogic;
	if (Source.Hold >= EMantleWeaponHold::Count)
	{
		OutMessages.Add(LOCTEXT("UnsupportedHold", "Pick a supported weapon hold."));
		return false;
	}
	const FMantleWeaponDocHoldInfo& Info = FMantleWeaponDocument::GetHoldInfo(ToDocHold(Source.Hold));
	UObject* Model = Source.Model.LoadSynchronous();
	if (!Model || !IsMesh(Model))
	{
		OutMessages.Add(MissingModelMessage(Source.Hold));
		return false;
	}
	const FString Name = SafeName(Source);
	const FString PackageFolder = TEXT("/Game/MantleWeapons/") + Name;
	TArray<TPair<UObject*, FString>> Auxiliary;
	// Check every referenced asset before replacing any generated output.
	auto CheckAsset = [&](UObject* Asset, bool bMesh, const FString& LocalName) -> bool
	{
		if (!Asset || (bMesh && !IsMesh(Asset)) || (!bMesh && !Cast<USoundWave>(Asset)))
		{
			OutMessages.Add(LOCTEXT("BadSeparateModel", "A separate part, projectile or attachment needs a static or skeletal model; a shot sound needs a SoundWave."));
			return false;
		}
		if (Asset->GetOutermost()->GetName().StartsWith(PackageFolder + TEXT("/"), ESearchCase::IgnoreCase))
		{
			OutMessages.Add(LOCTEXT("AssetInOutput", "Keep the original models and sounds outside this weapon's output folder, then try again."));
			return false;
		}
		if (!LocalName.IsEmpty()) Auxiliary.Emplace(Asset, LocalName);
		return true;
	};
	if (!CheckAsset(Model, true, FString())) return false;
	FMantleWeaponBuilderKind Kind(ToFitHold(Source.Hold));
	Kind.SetFlipRotation(Source.FlipRotation);
	Kind.SetScaleOverride(Source.bScaleOverridden ? Source.FitScale : 0.f);
	const TArray<FMantleBuilderPoint> Guesses = Kind.GuessPoints(Model);
	if (Guesses.IsEmpty())
	{
		OutMessages.Add(LOCTEXT("NoShape", "The model has no shape to read. Pick a model that has geometry."));
		return false;
	}
	const FMantleWeaponFitResult& Fit = Kind.GetLastFit();
	const FTransform MeshToWeapon = Fit.MeshToWeapon;
	TMap<FName, FTransform> InMesh;
	auto AddPoint = [&](const FMantleBuilderPoint& Point) -> bool
	{
		FTransform Parent = FTransform::Identity;
		if (!Point.Parent.IsNone() && !MantleBuilderViewportMath::ResolveParent(Model, Point.Parent, Parent))
		{
			OutMessages.Add(FText::Format(LOCTEXT("MissingParent", "The {0} point names a model bone that no longer exists. Place that point again."), FText::FromName(Point.Role)));
			return false;
		}
		const FTransform Transform = Point.Transform * Parent;
		EMantleWeaponDocSocket Socket;
		const FName Role = DocSocket(Point.Role, Socket) ? SocketRole(Socket) : Point.Role;
		InMesh.Add(Role, FTransform(Transform.GetRotation(), Transform.GetLocation()));
		return true;
	};
	for (const auto& Point : Guesses) if (!AddPoint(Point)) return false;
	for (const auto& Point : Source.Points) if (!AddPoint(Point)) return false;
	TMap<FName, FTransform> InWeapon;
	for (const auto& Point : InMesh)
	{
		InWeapon.Add(Point.Key, FTransform(MeshToWeapon.GetRotation() * Point.Value.GetRotation(), MeshToWeapon.TransformPosition(Point.Value.GetLocation())));
	}
	FMantleWeaponDesc Desc;
	if (!BuildDocument(Source, InWeapon, TEXT("SKM_") + Name, Desc, OutMessages)) return false;
	if (Source.bScaleOverridden && (!FMath::IsFinite(Source.FitScale) || Source.FitScale <= 0.f))
	{
		OutMessages.Add(LOCTEXT("BadSize", "Set the weapon's size to a positive number."));
		return false;
	}
	FMantleWeaponRigidMeshInput MeshInput;
	MeshInput.BodyMesh = Model;
	MeshInput.BodyToOutput = MeshToWeapon;
	MeshInput.PackagePath = PackageFolder;
	MeshInput.Name = Name;
	for (int32 Index = 0; Index < static_cast<int32>(EMantleWeaponDocSocket::Count); ++Index)
	{
		const auto Socket = static_cast<EMantleWeaponDocSocket>(Index);
		if (Desc.Socket(Socket).bSet)
		{
			if (const auto* Point = InMesh.Find(SocketRole(Socket))) MeshInput.Sockets.Add(SocketRole(Socket), *Point);
		}
	}
	for (const auto& Section : Source.MagazineSections)
	{
		UObject* Asset = Section.LoadSynchronous();
		if (!CheckAsset(Asset, true, FString())) return false;
		FMantleWeaponRigidPart Part;
		Part.Mesh = Asset;
		Part.InBody = FTransform::Identity;
		if (!MeshInput.Magazine.IsSet()) MeshInput.Magazine = Part;
		// Only the first magazine section is used; further sections are validated but not exported.
	}
	for (const auto& In : Source.Parts)
	{
		if (!In.InBody.IsValid() || In.InBody.GetScale3D().GetAbsMin() <= UE_SMALL_NUMBER)
		{
			OutMessages.Add(LOCTEXT("BadPartTransform", "A separate part needs a valid rotation, position and nonzero size."));
			return false;
		}
		UObject* Asset = In.Model.LoadSynchronous();
		if (!CheckAsset(Asset, true, FString())) return false;
		FMantleWeaponRigidPart Part;
		Part.Mesh = Asset;
		Part.InBody = In.InBody;
		switch (In.Role)
		{
		case EMantleWeaponSourcePartRole::Magazine:
			if (!MeshInput.Magazine.IsSet()) MeshInput.Magazine = Part;
			break;
		// Round, arrow and the bow string need the rigid-mesh block that ships with the ranged holds; the document still lists them.
		case EMantleWeaponSourcePartRole::Round:
		case EMantleWeaponSourcePartRole::Arrow:
			break;
		}
	}
	if (Desc.Projectile.bSet && Desc.Projectile.Value.Mesh.bSet && !CheckAsset(Source.Projectile.Model.LoadSynchronous(), true, AuxiliaryName(Source, TEXT("Projectile")))) return false;
	for (int32 Index = 0; Index < Source.Attachments.Num(); ++Index)
	{
		if (!Source.Attachments[Index].Model.IsNull() && !CheckAsset(Source.Attachments[Index].Model.LoadSynchronous(), true, AuxiliaryName(Source, TEXT("Attachment"), Index))) return false;
	}
	if (!Source.FireSound.IsNull() && !CheckAsset(Source.FireSound.LoadSynchronous(), false, AuxiliaryName(Source, TEXT("Fire")))) return false;
	if (!Source.FireSuppressedSound.IsNull() && !CheckAsset(Source.FireSuppressedSound.LoadSynchronous(), false, AuxiliaryName(Source, TEXT("FireSuppressed")))) return false;
	FOutputBackup Backup(Name, OutMessages);
	if (!Backup.bReady) return false;
	// Keep the existing firearm eye placement; non-firearm framing comes from its hold library.
	if (FMantleWeaponDocument::IsFirearmHold(Desc.GetHold()))
	{
		const auto& Sight = Desc.Socket(EMantleWeaponDocSocket::Sight);
		if (Sight.bSet)
		{
			const bool bPistol = Info.Effects == EMantleWeaponDocEffects::Pistol;
			const double EyeX = -Fit.GripFromRearCm + (bPistol ? PistolEyeFromRearCm : LongGunEyeFromRearCm);
			FMantleWeaponDocFirstPerson FirstPerson;
			FirstPerson.SightDistance.Set(FMath::Clamp(Sight.Value.Location.Value.X - EyeX, 0.0, 100.0));
			Desc.FirstPerson.Set(FirstPerson);
		}
	}
	const auto Built = FMantleWeaponRigidMesh::Build(MeshInput);
	if (!Built.Mesh || !Built.Errors.IsEmpty())
	{
		for (const auto& Error : Built.Errors) OutMessages.Add(FText::FromString(Error));
		return false;
	}
	for (const auto& Asset : Auxiliary) if (!CopyAsset(Asset.Key, PackageFolder, Asset.Value, OutMessages)) return false;
	const FString Folder = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / TEXT("MantleWeapons") / Name);
	IFileManager::Get().MakeDirectory(*Folder, true);
	if (!FFileHelper::SaveStringToFile(FMantleWeaponDocument::Write(Desc), *(Folder / (Name + TEXT(".weapon.json"))), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutMessages.Add(FText::Format(LOCTEXT("DocWriteFailed", "The weapon's settings file could not be saved in {0}. Check that the folder is writable."), FText::FromString(Folder)));
		return false;
	}
	OutMessages.Add(FText::Format(LOCTEXT("Ready", "Your weapon is ready: {0}."), FText::FromString(Desc.Name)));
	OutMessages.Add(FText::Format(LOCTEXT("Files", "Your weapon's files are in MantleWeapons/{0}."), FText::FromString(Name)));
	if (!Fit.ScaleReason.IsEmpty()) OutMessages.Add(FText::FromString(PlainWords(Fit.ScaleReason)));
	Backup.bComplete = true;
	return true;
}
#undef LOCTEXT_NAMESPACE
