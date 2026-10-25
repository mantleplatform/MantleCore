// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "MantleBuilderPoint.h"
#include "MantleWeaponSource.generated.h"

/** How the character holds and uses a creator weapon (#729). The same order as the editor's EMantleWeaponFitHold. */
UENUM()
enum class EMantleWeaponHold : uint8
{
	Pistol,
	Revolver,
	Rifle,
	Smg,
	Dmr,
	PumpShotgun,
	BoltSniper,
	Lmg,
	// Append only: the first eight values are already saved in creator assets.
	Melee1h,
	Melee2h,
	Spear,
	Staff,
	Bow,
	Unarmed,
	Shield,
	Throwable,
	/** #1025: a potion, a medkit, food: used on its holder (delivery Use, the Consumable numbers). */
	Consumable,
	Count UMETA(Hidden)
};

/** Default uses the hold's first allowed delivery/resource, so old firearm sources remain hitscan/magazine. */
UENUM()
enum class EMantleWeaponSourceDelivery : uint8 { Default, Hitscan, Projectile, Sweep, Beam, Use };
UENUM()
enum class EMantleWeaponSourceResource : uint8 { Default, Magazine, Quiver, Charges, None };
UENUM()
enum class EMantleWeaponSourceStyle : uint8
{
	Rifle, Pistol, Shotgun, Laser, Fire, Ice, Lightning, Arcane, Venom, Water, Wind, Rock, Sand, Void, Dark, Light, Generic, Heal, Lava, Nature
};
UENUM()
enum class EMantleWeaponSourcePartRole : uint8 { Magazine, Round, Arrow };
UENUM()
enum class EMantleWeaponSourceAction : uint8
{
	Equip, Holster, Idle, Fire, FireLast, ReloadTactical, ReloadEmpty, ReloadStart, ReloadStartEmpty, ReloadLoop, ReloadEnd,
	ReloadPartial, Attack, Heavy, Block, Throw, Cast, Nock, Draw, Release, Use
};
/** #1025: which of each hold's two potion clips a consumable plays (the document's consumable.style). */
UENUM()
enum class EMantleWeaponSourceUseStyle : uint8 { EatDrink, Use };
/** #1025: what a consumable does by itself (the document's consumable.effect): heal, or nothing (the place's script does it). */
UENUM()
enum class EMantleWeaponSourceConsumableEffect : uint8 { Heal, None };

USTRUCT()
struct MANTLERUNTIME_API FMantleWeaponSourceCharge
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, Category = "Mantle") bool bEnabled = false;
	UPROPERTY(EditAnywhere, Category = "Mantle") float Seconds = 1.f;
	UPROPERTY(EditAnywhere, Category = "Mantle") bool bRequired = false;
	UPROPERTY(EditAnywhere, Category = "Mantle") bool bReleaseOnFull = false;
	UPROPERTY(EditAnywhere, Category = "Mantle") bool bCancelable = true;
};

USTRUCT()
struct MANTLERUNTIME_API FMantleWeaponSourceProjectile
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, Category = "Mantle") float Speed = 5000.f;
	UPROPERTY(EditAnywhere, Category = "Mantle") float Gravity = 1.f;
	UPROPERTY(EditAnywhere, Category = "Mantle") float BlastRadius = 0.f;
	/** A grenade's fuse (a throwable that explodes), seconds. */
	UPROPERTY(EditAnywhere, Category = "Mantle") float FuseSeconds = 2.f;
	/** A throwable that leaves fire where it lands (a molotov): how long it burns (0: it does not), how much it hurts a second and how far it spreads. */
	UPROPERTY(EditAnywhere, Category = "Mantle") float LingerSeconds = 0.f;
	UPROPERTY(EditAnywhere, Category = "Mantle") float LingerDamagePerSecond = 20.f;
	UPROPERTY(EditAnywhere, Category = "Mantle") float LingerRadius = 250.f;
	UPROPERTY(EditAnywhere, Category = "Mantle") bool bThrown = false;
	UPROPERTY(EditAnywhere, Category = "Mantle") bool bStick = false;
	UPROPERTY(EditAnywhere, Category = "Mantle") bool bPickup = false;
	/** Optional projectile model; a thrown item flies as itself when unset. */
	UPROPERTY(EditAnywhere, Category = "Mantle") TSoftObjectPtr<UObject> Model;
};

USTRUCT()
struct MANTLERUNTIME_API FMantleWeaponSourceSweep
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, Category = "Mantle") float Reach = 100.f;
	UPROPERTY(EditAnywhere, Category = "Mantle") int32 ComboCount = 1;
	UPROPERTY(EditAnywhere, Category = "Mantle") float HeavyMultiplier = 2.f;
	/** Optional builder role names (EdgeBase, EdgeTip, Tip, Grip, Face); leave both unset for the hold's edge. */
	UPROPERTY(EditAnywhere, Category = "Mantle") FName EdgeBase;
	UPROPERTY(EditAnywhere, Category = "Mantle") FName EdgeTip;
};

USTRUCT()
struct MANTLERUNTIME_API FMantleWeaponSourceBeam
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, Category = "Mantle") float TickRate = 10.f;
	UPROPERTY(EditAnywhere, Category = "Mantle") float DamagePerSecond = 25.f;
	UPROPERTY(EditAnywhere, Category = "Mantle") float MaxSeconds = 5.f;
	UPROPERTY(EditAnywhere, Category = "Mantle") float Radius = 0.f;
	/** Zero inherits fire.maxRange. */
	UPROPERTY(EditAnywhere, Category = "Mantle") float Range = 0.f;
};

/** A staff's charges (the charges resource): how many casts it holds, and how long one takes to come back. */
USTRUCT()
struct MANTLERUNTIME_API FMantleWeaponSourceCharges
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, Category = "Mantle") int32 Count = 3;
	UPROPERTY(EditAnywhere, Category = "Mantle") float RefillSeconds = 4.f;
};

/** #1025: a consumable's numbers (the document's consumable block). */
USTRUCT()
struct MANTLERUNTIME_API FMantleWeaponSourceConsumable
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, Category = "Mantle") EMantleWeaponSourceUseStyle Style = EMantleWeaponSourceUseStyle::EatDrink;
	UPROPERTY(EditAnywhere, Category = "Mantle") EMantleWeaponSourceConsumableEffect Effect = EMantleWeaponSourceConsumableEffect::Heal;
	/** With effect Heal: health given (1..1000), and 0 for all at once or the seconds it is spread over. */
	UPROPERTY(EditAnywhere, Category = "Mantle") float Heal = 50.f;
	UPROPERTY(EditAnywhere, Category = "Mantle") float HealSeconds = 0.f;
	UPROPERTY(EditAnywhere, Category = "Mantle") int32 Uses = 1;
	/** False: one use takes the clip's own length (2.5 s) and useSeconds is not written. */
	UPROPERTY(EditAnywhere, Category = "Mantle") bool bOverrideUseSeconds = false;
	UPROPERTY(EditAnywhere, Category = "Mantle") float UseSeconds = 2.5f;
	UPROPERTY(EditAnywhere, Category = "Mantle") float CooldownSeconds = 0.f;
	UPROPERTY(EditAnywhere, Category = "Mantle") bool bMoveWhileUsing = true;
};

/** A separately selected rigid part, positioned in the body's original model space. */
USTRUCT()
struct MANTLERUNTIME_API FMantleWeaponSourcePart
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, Category = "Mantle") EMantleWeaponSourcePartRole Role = EMantleWeaponSourcePartRole::Magazine;
	UPROPERTY(EditAnywhere, Category = "Mantle") TSoftObjectPtr<UObject> Model;
	UPROPERTY(EditAnywhere, Category = "Mantle") FTransform InBody = FTransform::Identity;
	/** For a stowed round/arrow, empty uses the hold's loading/drawing actions. */
	UPROPERTY(EditAnywhere, Category = "Mantle") TArray<EMantleWeaponSourceAction> ShownDuring;
};

/** Fixed mesh/effect attachment, positioned relative to a builder role, or the weapon root when Socket is unset. */
USTRUCT()
struct MANTLERUNTIME_API FMantleWeaponSourceAttachment
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, Category = "Mantle") FString Name;
	UPROPERTY(EditAnywhere, Category = "Mantle") TSoftObjectPtr<UObject> Model;
	UPROPERTY(EditAnywhere, Category = "Mantle") bool bEffect = false;
	UPROPERTY(EditAnywhere, Category = "Mantle") EMantleWeaponSourceStyle Effect = EMantleWeaponSourceStyle::Generic;
	UPROPERTY(EditAnywhere, Category = "Mantle") FName Socket;
	UPROPERTY(EditAnywhere, Category = "Mantle") FTransform RelativeTransform = FTransform::Identity;
	UPROPERTY(EditAnywhere, Category = "Mantle") bool bHidden = false;
};

/** The numbers a creator tunes in the weapon builder, within the ranges the builder allows. */
USTRUCT()
struct MANTLERUNTIME_API FMantleWeaponTuning
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Mantle")
	float Damage = 12.f;

	UPROPERTY(EditAnywhere, Category = "Mantle")
	float ShotsPerSecond = 12.5f;

	UPROPERTY(EditAnywhere, Category = "Mantle")
	int32 MagazineSize = 30;

	UPROPERTY(EditAnywhere, Category = "Mantle")
	int32 SpareAmmo = 90;

	UPROPERTY(EditAnywhere, Category = "Mantle")
	float ReloadSeconds = 1.7f;

	UPROPERTY(EditAnywhere, Category = "Mantle")
	bool bAutomatic = true;

	/** Zero / -1 preserve the old hold-specific range and recoil on saved firearm sources. */
	UPROPERTY(EditAnywhere, Category = "Mantle") float MaxRange = 0.f;
	UPROPERTY(EditAnywhere, Category = "Mantle") float TraceSweepRadius = 5.5f;
	UPROPERTY(EditAnywhere, Category = "Mantle") int32 Pellets = 1;
	UPROPERTY(EditAnywhere, Category = "Mantle") float PelletSpread = 1.f;
	UPROPERTY(EditAnywhere, Category = "Mantle") float PlainRecoil = -1.f;
	UPROPERTY(EditAnywhere, Category = "Mantle") bool bBurstMode = false;
};

/**
 * A creator weapon as the weapon builder saves it (#729): the creator's own 3D model, which kind of hold it uses, the
 * points placed on the model and the tuning. Export turns this into the weapon's published files.
 */
UCLASS()
class MANTLERUNTIME_API UMantleWeaponSource : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "Mantle")
	FString DisplayName;

	UPROPERTY(EditAnywhere, Category = "Mantle")
	EMantleWeaponHold Hold = EMantleWeaponHold::Rifle;

	/** The creator's UStaticMesh or USkeletalMesh. */
	UPROPERTY(EditAnywhere, Category = "Mantle")
	TSoftObjectPtr<UObject> Model;

	/** Legacy separately selected magazine meshes, in body model space. Kept for saved source compatibility. */
	UPROPERTY(EditAnywhere, Category = "Mantle")
	TArray<TSoftObjectPtr<UObject>> MagazineSections;

	/** Uniform, centimetres per model unit. */
	UPROPERTY(EditAnywhere, Category = "Mantle")
	float FitScale = 1.f;

	/** True when the creator set the size; false when FitScale is the builder's own guess. */
	UPROPERTY(EditAnywhere, Category = "Mantle")
	bool bScaleOverridden = false;

	/** The creator's Flip and Turn corrections, applied to the model before it is fitted. */
	UPROPERTY(EditAnywhere, Category = "Mantle")
	FQuat FlipRotation = FQuat::Identity;

	UPROPERTY(EditAnywhere, Category = "Mantle")
	TArray<FMantleBuilderPoint> Points;

	UPROPERTY(EditAnywhere, Category = "Mantle")
	FMantleWeaponTuning Tuning;

	UPROPERTY(EditAnywhere, Category = "Mantle") EMantleWeaponSourceDelivery Delivery = EMantleWeaponSourceDelivery::Default;
	UPROPERTY(EditAnywhere, Category = "Mantle") EMantleWeaponSourceResource Resource = EMantleWeaponSourceResource::Default;
	UPROPERTY(EditAnywhere, Category = "Mantle") bool bOverrideTwoHanded = false;
	UPROPERTY(EditAnywhere, Category = "Mantle") bool bTwoHanded = false;
	UPROPERTY(EditAnywhere, Category = "Mantle") bool bOverrideStyle = false;
	UPROPERTY(EditAnywhere, Category = "Mantle") EMantleWeaponSourceStyle Style = EMantleWeaponSourceStyle::Rifle;
	UPROPERTY(EditAnywhere, Category = "Mantle") bool bOverrideColor = false;
	UPROPERTY(EditAnywhere, Category = "Mantle") FColor Color = FColor(40, 110, 160);
	UPROPERTY(EditAnywhere, Category = "Mantle") FMantleWeaponSourceCharge Charge;
	UPROPERTY(EditAnywhere, Category = "Mantle") bool bBlock = false;
	UPROPERTY(EditAnywhere, Category = "Mantle") bool bBreakOnHeavy = true;
	UPROPERTY(EditAnywhere, Category = "Mantle") FMantleWeaponSourceProjectile Projectile;
	UPROPERTY(EditAnywhere, Category = "Mantle") FMantleWeaponSourceSweep Sweep;
	UPROPERTY(EditAnywhere, Category = "Mantle") FMantleWeaponSourceBeam Beam;
	/** #909: a staff with the held beam also throws spells (a tap casts, holding is the beam): the projectile's numbers go with the beam's. */
	UPROPERTY(EditAnywhere, Category = "Mantle") bool bSpellsBesideBeam = false;
	UPROPERTY(EditAnywhere, Category = "Mantle") FMantleWeaponSourceCharges Charges;
	/** #1025: hold Consumable's numbers. */
	UPROPERTY(EditAnywhere, Category = "Mantle") FMantleWeaponSourceConsumable Consumable;
	UPROPERTY(EditAnywhere, Category = "Mantle") float BowStringRadiusCm = 1.5f;
	UPROPERTY(EditAnywhere, Category = "Mantle") TArray<FMantleWeaponSourcePart> Parts;
	UPROPERTY(EditAnywhere, Category = "Mantle") TArray<FMantleWeaponSourceAttachment> Attachments;
	/** A SoundWave copied into the weapon's assets; hold/style sounds apply when unset. */
	UPROPERTY(EditAnywhere, Category = "Mantle") TSoftObjectPtr<class USoundWave> FireSound;
	UPROPERTY(EditAnywhere, Category = "Mantle") TSoftObjectPtr<class USoundWave> FireSuppressedSound;
	UPROPERTY(EditAnywhere, Category = "Mantle") bool bSuppressed = false;
	UPROPERTY(EditAnywhere, Category = "Mantle") FString FireSoundTrigger;
};
