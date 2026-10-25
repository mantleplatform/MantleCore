// Copyright Joshua Eger. All Rights Reserved.
#include "MantleWeaponBuilderLogic.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Misc/SecureHash.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
#include "Serialization/StructuredArchiveAdapters.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "MantleWeaponBuilderLogic"

namespace MantleWeaponBuilderLogicPrivate
{
	struct FSavedFileFingerprint
	{
		int64 Size = -2;
		FDateTime Modified;
		FMD5Hash Hash;
	};

	FMD5Hash SavedFileHash(const FString& Filename)
	{
		// Button enablement polls this signature. Read saved contents only when the file changes.
		static FCriticalSection Lock;
		static TMap<FString, FSavedFileFingerprint> Cache;
		FScopeLock Guard(&Lock);
		const int64 Size = IFileManager::Get().FileSize(*Filename);
		const FDateTime Modified = IFileManager::Get().GetTimeStamp(*Filename);
		FSavedFileFingerprint& Entry = Cache.FindOrAdd(Filename);
		if (Entry.Size != Size || Entry.Modified != Modified)
		{
			Entry.Size = Size;
			Entry.Modified = Modified;
			Entry.Hash = Size >= 0 ? FMD5Hash::HashFile(*Filename) : FMD5Hash();
		}
		return Entry.Hash;
	}

	void SavedAssets(FArchive& Archive, const UMantleWeaponSource& Source)
	{
		TArray<FString> Packages;
		auto Add = [&](const FSoftObjectPath& Asset) { if (!Asset.IsNull()) Packages.AddUnique(Asset.GetLongPackageName()); };
		Add(Source.Model.ToSoftObjectPath());
		for (const auto& Section : Source.MagazineSections) Add(Section.ToSoftObjectPath());
		Add(Source.Projectile.Model.ToSoftObjectPath());
		for (const auto& Part : Source.Parts) Add(Part.Model.ToSoftObjectPath());
		for (const auto& Attachment : Source.Attachments) Add(Attachment.Model.ToSoftObjectPath());
		Add(Source.FireSound.ToSoftObjectPath());
		Add(Source.FireSuppressedSound.ToSoftObjectPath());
		Packages.Sort();
		for (FString& Package : Packages)
		{
			Archive << Package;
			FString Filename;
			bool bSaved = FPackageName::DoesPackageExist(Package, &Filename);
			Archive << bSaved;
			if (!bSaved) continue;
			// Directly selected packages only: no dependency graph or source UObject identity.
			FMD5Hash Hash = SavedFileHash(Filename);
			Archive << Hash;
			for (const TCHAR* Extension : { TEXT("uexp"), TEXT("ubulk"), TEXT("uptnl") })
			{
				Hash = SavedFileHash(FPaths::ChangeExtension(Filename, Extension));
				Archive << Hash;
			}
		}
	}
}

// The two enums list the same holds in the same order; the mapping below is a cast, so keep it checked.
static_assert(static_cast<uint8>(EMantleWeaponHold::Pistol) == static_cast<uint8>(EMantleWeaponFitHold::Pistol), "hold order");
static_assert(static_cast<uint8>(EMantleWeaponHold::Revolver) == static_cast<uint8>(EMantleWeaponFitHold::Revolver), "hold order");
static_assert(static_cast<uint8>(EMantleWeaponHold::Rifle) == static_cast<uint8>(EMantleWeaponFitHold::Rifle), "hold order");
static_assert(static_cast<uint8>(EMantleWeaponHold::Smg) == static_cast<uint8>(EMantleWeaponFitHold::Smg), "hold order");
static_assert(static_cast<uint8>(EMantleWeaponHold::Dmr) == static_cast<uint8>(EMantleWeaponFitHold::Dmr), "hold order");
static_assert(static_cast<uint8>(EMantleWeaponHold::PumpShotgun) == static_cast<uint8>(EMantleWeaponFitHold::PumpShotgun), "hold order");
static_assert(static_cast<uint8>(EMantleWeaponHold::BoltSniper) == static_cast<uint8>(EMantleWeaponFitHold::BoltSniper), "hold order");
static_assert(static_cast<uint8>(EMantleWeaponHold::Lmg) == static_cast<uint8>(EMantleWeaponFitHold::Lmg), "hold order");
static_assert(static_cast<uint8>(EMantleWeaponHold::Count) == static_cast<uint8>(EMantleWeaponFitHold::Count), "hold count");
static_assert(static_cast<uint8>(EMantleWeaponHold::Throwable) == static_cast<uint8>(EMantleWeaponDocHold::Throwable), "document hold order");
static_assert(static_cast<uint8>(EMantleWeaponHold::Consumable) == static_cast<uint8>(EMantleWeaponDocHold::Consumable), "document hold order");
static_assert(static_cast<uint8>(EMantleWeaponHold::Consumable) + 1 == static_cast<uint8>(EMantleWeaponDocHold::Count), "document hold count");
static_assert(static_cast<uint8>(EMantleWeaponHold::Shield) == static_cast<uint8>(EMantleWeaponDocHold::Shield), "document hold order");
static_assert(static_cast<uint8>(EMantleWeaponSourceAction::Release) == static_cast<uint8>(EMantleWeaponDocAction::Release), "action order");

EMantleWeaponDocHold MantleWeaponBuilderLogic::ToDocHold(EMantleWeaponHold Hold)
{
	return static_cast<EMantleWeaponDocHold>(static_cast<uint8>(Hold));
}

FName MantleWeaponBuilderLogic::SocketRole(EMantleWeaponDocSocket Socket)
{
	static const TCHAR* Names[] = { TEXT("Sight"), TEXT("Muzzle"), TEXT("Grip_R"), TEXT("Grip_L"), TEXT("Magazine"), TEXT("ShellEject"),
		TEXT("EdgeBase"), TEXT("EdgeTip"), TEXT("Tip"), TEXT("Focus"), TEXT("StringTop"), TEXT("StringBottom"), TEXT("Nock"), TEXT("Grip"), TEXT("Face") };
	static_assert(UE_ARRAY_COUNT(Names) == static_cast<int32>(EMantleWeaponDocSocket::Count), "one builder role per socket");
	const int32 Index = static_cast<int32>(Socket);
	return Index >= 0 && Index < UE_ARRAY_COUNT(Names) ? FName(Names[Index]) : NAME_None;
}

bool MantleWeaponBuilderLogic::DocSocket(FName Role, EMantleWeaponDocSocket& OutSocket)
{
	for (int32 Index = 0; Index < static_cast<int32>(EMantleWeaponDocSocket::Count); ++Index)
	{
		const EMantleWeaponDocSocket Socket = static_cast<EMantleWeaponDocSocket>(Index);
		if (Role == SocketRole(Socket) || Role == FName(FMantleWeaponDocument::SocketName(Socket)))
		{
			OutSocket = Socket;
			return true;
		}
	}
	return false;
}

EMantleWeaponFitHold MantleWeaponBuilderLogic::ToFitHold(EMantleWeaponHold Hold)
{
	return static_cast<EMantleWeaponFitHold>(static_cast<uint8>(Hold));
}

EMantleWeaponHold MantleWeaponBuilderLogic::FromFitHold(EMantleWeaponFitHold Hold)
{
	return static_cast<EMantleWeaponHold>(static_cast<uint8>(Hold));
}

bool MantleWeaponBuilderLogic::IsHoldAvailable(EMantleWeaponHold Hold)
{
	// Every hold is available.
	return static_cast<uint8>(Hold) < static_cast<uint8>(EMantleWeaponHold::Count);
}

EMantleWeaponDocDelivery MantleWeaponBuilderLogic::UsualDelivery(EMantleWeaponDocHold Hold)
{
	const FMantleWeaponDocHoldInfo& Info = FMantleWeaponDocument::GetHoldInfo(Hold);
	// A staff's table lists the projectile first; its usual stays the held beam (AMantlePackStaff's first attack),, and the flying spell is a choice.
	return Hold == EMantleWeaponDocHold::Staff ? EMantleWeaponDocDelivery::Beam : Info.Deliveries[0];
}

bool MantleWeaponBuilderLogic::IsDeliveryOffered(EMantleWeaponDocHold Hold, EMantleWeaponDocDelivery Delivery)
{
	// Every delivery the hold's table lists.
	return FMantleWeaponDocument::GetHoldInfo(Hold).Deliveries.Contains(Delivery);
}

EMantleWeaponDocDelivery MantleWeaponBuilderLogic::DeliveryOf(const UMantleWeaponSource& Source)
{
	return Source.Delivery == EMantleWeaponSourceDelivery::Default
		? UsualDelivery(ToDocHold(Source.Hold))
		: static_cast<EMantleWeaponDocDelivery>(static_cast<uint8>(Source.Delivery) - 1);
}

bool MantleWeaponBuilderLogic::PicksElement(EMantleWeaponHold Hold, EMantleWeaponDocDelivery Delivery)
{
	return !FMantleWeaponDocument::GetHoldInfo(ToDocHold(Hold)).bHasEffects
		|| Delivery == EMantleWeaponDocDelivery::Projectile || Delivery == EMantleWeaponDocDelivery::Beam;
}

void MantleWeaponBuilderLogic::SetDelivery(UMantleWeaponSource& Source, EMantleWeaponSourceDelivery Delivery)
{
	const EMantleWeaponDocDelivery Before = DeliveryOf(Source);
	Source.Delivery = Delivery;
	const EMantleWeaponDocDelivery After = DeliveryOf(Source);
	if (After == EMantleWeaponDocDelivery::Sweep)
	{
		Source.bOverrideStyle = false;
		Source.bOverrideColor = false;
		return;
	}
	const FMantleWeaponDocHoldInfo& Info = FMantleWeaponDocument::GetHoldInfo(ToDocHold(Source.Hold));
	if (!Info.bHasEffects || After == Before)
	{
		return;
	}
	// The style in hand must be one the new choice list offers: an element on a gun that launches or beams, a firearm style on an
	// instant shot (the reader refuses an element there).
	const bool bElement = PicksElement(Source.Hold, After);
	if (FMantleWeaponDocument::IsElement(static_cast<EMantleWeaponDocEffects>(Source.Style)) != bElement)
	{
		Source.bOverrideStyle = false;
		Source.Style = bElement ? EMantleWeaponSourceStyle::Fire : static_cast<EMantleWeaponSourceStyle>(Info.Effects);
	}
	if (After == EMantleWeaponDocDelivery::Beam)
	{
		// A laser rifle: a thin line that reaches 30 m, ten hit checks a second, five seconds a burst.
		Source.Beam = FMantleWeaponSourceBeam();
		Source.Beam.TickRate = 10.f;
		Source.Beam.DamagePerSecond = 40.f;
		Source.Beam.MaxSeconds = 5.f;
		Source.Beam.Radius = 0.f;
		Source.Beam.Range = 3000.f;
		Source.bOverrideStyle = true;
		Source.Style = EMantleWeaponSourceStyle::Laser;
	}
	else if (After == EMantleWeaponDocDelivery::Projectile)
	{
		// A launcher: a fast round on a shallow arc that bursts where it lands.
		const TSoftObjectPtr<UObject> Model = Source.Projectile.Model;
		Source.Projectile = FMantleWeaponSourceProjectile();
		Source.Projectile.Model = Model;
		Source.Projectile.Speed = 4000.f;
		Source.Projectile.Gravity = 0.5f;
		Source.Projectile.BlastRadius = 250.f;
	}
}

FText MantleWeaponBuilderLogic::HoldLabel(EMantleWeaponHold Hold)
{
	switch (Hold)
	{
	case EMantleWeaponHold::Pistol: return LOCTEXT("HoldPistol", "Pistol");
	case EMantleWeaponHold::Revolver: return LOCTEXT("HoldRevolver", "Revolver");
	case EMantleWeaponHold::Rifle: return LOCTEXT("HoldRifle", "Rifle");
	case EMantleWeaponHold::Smg: return LOCTEXT("HoldSmg", "SMG");
	case EMantleWeaponHold::Dmr: return LOCTEXT("HoldDmr", "Marksman rifle");
	case EMantleWeaponHold::PumpShotgun: return LOCTEXT("HoldShotgun", "Shotgun");
	case EMantleWeaponHold::BoltSniper: return LOCTEXT("HoldSniper", "Sniper rifle");
	case EMantleWeaponHold::Lmg: return LOCTEXT("HoldLmg", "Machine gun");
	case EMantleWeaponHold::Melee1h: return LOCTEXT("HoldMelee1h", "One-handed blade");
	case EMantleWeaponHold::Melee2h: return LOCTEXT("HoldMelee2h", "Two-handed blade");
	case EMantleWeaponHold::Spear: return LOCTEXT("HoldSpear", "Spear");
	case EMantleWeaponHold::Staff: return LOCTEXT("HoldStaff", "Staff");
	case EMantleWeaponHold::Bow: return LOCTEXT("HoldBow", "Bow");
	// A weapon worn on the hand (knuckles, claws, a gauntlet); empty hands are built into the game, never a creator weapon.
	case EMantleWeaponHold::Unarmed: return LOCTEXT("HoldUnarmed", "Fist weapon (knuckles, claws)");
	case EMantleWeaponHold::Shield: return LOCTEXT("HoldShield", "Shield");
	case EMantleWeaponHold::Throwable: return LOCTEXT("HoldThrowable", "Throwable");
	case EMantleWeaponHold::Consumable: return LOCTEXT("HoldConsumable", "Consumable");
	}
	return FText::GetEmpty();
}

FString MantleWeaponBuilderLogic::DefaultName(EMantleWeaponHold Hold)
{
	// The fist weapon's label explains the hold; its name is the thing itself.
	return Hold == EMantleWeaponHold::Unarmed ? FString(TEXT("My Knuckles")) : FString::Printf(TEXT("My %s"), *HoldLabel(Hold).ToString());
}

FText MantleWeaponBuilderLogic::MissingModelMessage(EMantleWeaponHold Hold)
{
	return Hold == EMantleWeaponHold::Unarmed
		? LOCTEXT("FistWeaponNeedsModel", "Fist weapons need a 3D model: pick the model of the knuckles, claws or gauntlet the hand wears.")
		: LOCTEXT("WeaponNeedsModel", "Pick the 3D model of your weapon.");
}

FMantleWeaponTuning MantleWeaponBuilderLogic::DefaultTuning(EMantleWeaponHold Hold)
{
	// Damage, shots per second, magazine, spare ammo, reload seconds, automatic. The rifle's numbers are the starting point; the
	// pistol and each other hold override them with their own.
	FMantleWeaponTuning Tuning;
	Tuning.Damage = 12.f;
	Tuning.ShotsPerSecond = 12.5f;
	Tuning.MagazineSize = 30;
	Tuning.SpareAmmo = 90;
	Tuning.ReloadSeconds = 1.7f;
	Tuning.bAutomatic = true;
	if (Hold == EMantleWeaponHold::Pistol)
	{
		Tuning.Damage = 18.f;
		Tuning.ShotsPerSecond = 6.f;
		Tuning.MagazineSize = 15;
		Tuning.SpareAmmo = 45;
		Tuning.ReloadSeconds = 1.4f;
		Tuning.bAutomatic = false;
	}
	// Existing rifle/pistol sources keep their saved numbers. New holds start from their own attack/resource defaults. A gun's
	// reload seconds are its clips': the clip plays and sets the time, this only stands in if one is missing (the
	// tactical clip; the revolver's empty speedloader clip; the shotgun's start, a shell per round and its end). The revolver's
	// and the machine gun's magazines are their clips' own: 8 chambers, a 100-round belt.
	else if (Hold != EMantleWeaponHold::Rifle)
	{
		Tuning.bAutomatic = Hold == EMantleWeaponHold::Smg || Hold == EMantleWeaponHold::Lmg;
		switch (Hold)
		{
		case EMantleWeaponHold::Revolver: Tuning.Damage = 36.f; Tuning.ShotsPerSecond = 2.f; Tuning.MagazineSize = 8; Tuning.SpareAmmo = 32; Tuning.ReloadSeconds = 4.62f; break;
		case EMantleWeaponHold::Smg: Tuning.Damage = 10.f; Tuning.ShotsPerSecond = 15.f; Tuning.ReloadSeconds = 3.58f; break;
		case EMantleWeaponHold::Dmr: Tuning.Damage = 30.f; Tuning.ShotsPerSecond = 3.f; Tuning.MagazineSize = 20; Tuning.SpareAmmo = 60; Tuning.ReloadSeconds = 4.13f; break;
		case EMantleWeaponHold::PumpShotgun: Tuning.Damage = 8.f; Tuning.ShotsPerSecond = 1.2f; Tuning.MagazineSize = 8; Tuning.SpareAmmo = 32; Tuning.ReloadSeconds = 8.85f; Tuning.Pellets = 8; break;
		case EMantleWeaponHold::BoltSniper: Tuning.Damage = 60.f; Tuning.ShotsPerSecond = 0.8f; Tuning.MagazineSize = 5; Tuning.SpareAmmo = 20; Tuning.ReloadSeconds = 3.08f; break;
		case EMantleWeaponHold::Lmg: Tuning.Damage = 10.f; Tuning.ShotsPerSecond = 12.f; Tuning.MagazineSize = 100; Tuning.SpareAmmo = 200; Tuning.ReloadSeconds = 7.97f; break;
		case EMantleWeaponHold::Melee1h: Tuning.Damage = 20.f; Tuning.ShotsPerSecond = 1.4f; break;
		case EMantleWeaponHold::Melee2h: Tuning.Damage = 35.f; Tuning.ShotsPerSecond = 0.8f; break;
		case EMantleWeaponHold::Spear: Tuning.Damage = 24.f; Tuning.ShotsPerSecond = 1.f; break;
		case EMantleWeaponHold::Staff: Tuning.Damage = 25.f; Tuning.ShotsPerSecond = 1.f; break;
		case EMantleWeaponHold::Bow: Tuning.Damage = 30.f; Tuning.ShotsPerSecond = 1.f; Tuning.MagazineSize = 12; Tuning.SpareAmmo = 36; Tuning.ReloadSeconds = 1.f; break;
		// A fist weapon hits harder than the built-in bare fists (10) at their pace.
		case EMantleWeaponHold::Unarmed: Tuning.Damage = 15.f; Tuning.ShotsPerSecond = 2.f; break;
		case EMantleWeaponHold::Shield: Tuning.Damage = 12.f; Tuning.ShotsPerSecond = 1.f; break;
		case EMantleWeaponHold::Throwable: Tuning.Damage = 40.f; Tuning.ShotsPerSecond = 0.5f; break;
		default: break;
		}
	}
	return Tuning;
}

MantleWeaponBuilderLogic::FRange MantleWeaponBuilderLogic::DamageRange() { return { 0.f, 1000.f }; }
MantleWeaponBuilderLogic::FRange MantleWeaponBuilderLogic::ShotsPerSecondRange() { return { 0.1f, 100.f }; }
MantleWeaponBuilderLogic::FRange MantleWeaponBuilderLogic::MagazineSizeRange() { return { 1.f, 1000.f }; }
MantleWeaponBuilderLogic::FRange MantleWeaponBuilderLogic::SpareAmmoRange() { return { 0.f, 10000.f }; }
MantleWeaponBuilderLogic::FRange MantleWeaponBuilderLogic::ReloadSecondsRange() { return { 0.01f, 30.f }; }

FMantleWeaponTuning MantleWeaponBuilderLogic::ClampTuning(const FMantleWeaponTuning& Tuning)
{
	FMantleWeaponTuning Out = Tuning;
	Out.Damage = FMath::Clamp(Tuning.Damage, DamageRange().Min, DamageRange().Max);
	Out.ShotsPerSecond = FMath::Clamp(Tuning.ShotsPerSecond, ShotsPerSecondRange().Min, ShotsPerSecondRange().Max);
	Out.MagazineSize = FMath::Clamp(Tuning.MagazineSize, static_cast<int32>(MagazineSizeRange().Min), static_cast<int32>(MagazineSizeRange().Max));
	Out.SpareAmmo = FMath::Clamp(Tuning.SpareAmmo, static_cast<int32>(SpareAmmoRange().Min), static_cast<int32>(SpareAmmoRange().Max));
	Out.ReloadSeconds = FMath::Clamp(Tuning.ReloadSeconds, ReloadSecondsRange().Min, ReloadSecondsRange().Max);
	if (Out.MaxRange != 0.f) Out.MaxRange = FMath::Clamp(Out.MaxRange, 100.f, 100000.f);
	Out.TraceSweepRadius = FMath::Clamp(Out.TraceSweepRadius, 0.f, 50.f);
	Out.Pellets = FMath::Clamp(Out.Pellets, 1, 32);
	Out.PelletSpread = FMath::Clamp(Out.PelletSpread, 0.f, 10.f);
	if (Out.PlainRecoil != -1.f) Out.PlainRecoil = FMath::Clamp(Out.PlainRecoil, 0.f, 30.f);
	return Out;
}

FString MantleWeaponBuilderLogic::SafeAssetName(const FString& Name)
{
	FString Out;
	for (const TCHAR Char : Name.TrimStartAndEnd())
	{
		if (FChar::IsAlnum(Char) && Char < 128)
		{
			Out.AppendChar(Char);
		}
		else if (Char == TEXT(' ') || Char == TEXT('_') || Char == TEXT('-'))
		{
			Out.AppendChar(TEXT('_'));
		}
	}
	// The generated body mesh is named "SKM_" + this safe folder name and the local
	// document reference is capped at64 characters. Keep four characters available.
	return Out.IsEmpty() ? FString(TEXT("MyWeapon")) : Out.Left(60);
}

FString MantleWeaponBuilderLogic::FirstFreeDisplayName(const FString& DisplayName, const TArray<FString>& TakenSafeNames)
{
	const FString Base = DisplayName.TrimStartAndEnd().Left(64);
	auto IsTaken = [&TakenSafeNames](const FString& Candidate)
	{
		const FString Safe = SafeAssetName(Candidate);
		for (const FString& Taken : TakenSafeNames)
		{
			if (Taken.Equals(Safe, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	};
	if (!IsTaken(Base))
	{
		return Base;
	}
	for (int32 Number = 2; Number < 100000; ++Number)
	{
		const FString Suffix = FString::Printf(TEXT(" %d"), Number);
		// Keep the separator too; SafeAssetName trims leading/trailing whitespace from a
		// standalone suffix, which would otherwise drop a final digit at the 60-char boundary.
		const int32 BaseLength = FMath::Max(0, 60 - Suffix.Len());
		const FString Candidate = Base.Left(BaseLength).TrimEnd() + Suffix;
		if (!IsTaken(Candidate))
		{
			return Candidate;
		}
	}
	return Base;
}

FString MantleWeaponBuilderLogic::SourcePackagePath(const FString& Name)
{
	const FString Safe = SafeAssetName(Name);
	return FString::Printf(TEXT("/Game/MantleWeapons/%s/%s_Source"), *Safe, *Safe);
}

TOptional<EMantleWeaponHold> MantleWeaponBuilderLogic::AvailableHoldNamedIn(const FString& Suggestion, EMantleWeaponHold Current)
{
	// The fit names holds in its own lower-case words ("pistol", "rifle"); the first available one that is not the current hold.
	struct FNamed
	{
		const TCHAR* Word;
		EMantleWeaponHold Hold;
	};
	static const FNamed Named[] = {
		{ TEXT("pistol"), EMantleWeaponHold::Pistol },
		{ TEXT("rifle"), EMantleWeaponHold::Rifle },
	};
	for (const FNamed& Entry : Named)
	{
		if (Entry.Hold != Current && IsHoldAvailable(Entry.Hold) && Suggestion.Contains(Entry.Word, ESearchCase::IgnoreCase))
		{
			return Entry.Hold;
		}
	}
	return TOptional<EMantleWeaponHold>();
}

FString MantleWeaponBuilderLogic::PlainWords(const FString& Text)
{
	FString Out = Text;
	Out.ReplaceInline(TEXT("mesh"), TEXT("model"), ESearchCase::IgnoreCase);
	Out.ReplaceInline(TEXT("preset"), TEXT("weapon type"), ESearchCase::IgnoreCase);
	return Out;
}

FString MantleWeaponBuilderLogic::EditSignature(const UMantleWeaponSource& Source)
{
	// Serialize only the authoring properties, never UObject identity, outer, editor metadata or output bookkeeping.
	// The proxy writes asset references as paths; binary numbers retain edits smaller than display rounding.
	TArray<uint8> Bytes;
	FMemoryWriter Writer(Bytes);
	FObjectAndNameAsStringProxyArchive Archive(Writer, false);
	Archive.ArNoDelta = true;
	for (TFieldIterator<FProperty> Property(UMantleWeaponSource::StaticClass(), EFieldIteratorFlags::ExcludeSuper); Property; ++Property)
	{
		if (Property->HasAnyPropertyFlags(CPF_Edit) && !Property->HasAnyPropertyFlags(CPF_Transient))
		{
			FString Name = Property->GetName();
			Archive << Name;
			if (Name == TEXT("Points"))
			{
				int32 Count = Source.Points.Num();
				Archive << Count;
				for (const auto& Point : Source.Points)
				{
					FName Role = Point.Role, Parent = Point.Parent;
					FVector Location = Point.Transform.GetLocation();
					FQuat Rotation = Point.Transform.GetRotation();
					FArchive& SignatureArchive = Archive;
					SignatureArchive << Role << Parent << Location << Rotation;
				}
				continue;
			}
			Property->SerializeItem(FStructuredArchiveFromArchive(Archive).GetSlot(), Property->ContainerPtrToValuePtr<void>(const_cast<UMantleWeaponSource*>(&Source)), nullptr);
		}
	}
	MantleWeaponBuilderLogicPrivate::SavedAssets(Archive, Source);
	uint8 Hash[FSHA1::DigestSize];
	FSHA1::HashBuffer(Bytes.GetData(), Bytes.Num(), Hash);
	return BytesToHex(Hash, FSHA1::DigestSize);
}

bool MantleWeaponBuilderLogic::CanTestLocally(const FString& SignatureAtCreate, const FString& SignatureNow)
{
	return !SignatureAtCreate.IsEmpty() && SignatureAtCreate.Equals(SignatureNow, ESearchCase::CaseSensitive);
}

#undef LOCTEXT_NAMESPACE
