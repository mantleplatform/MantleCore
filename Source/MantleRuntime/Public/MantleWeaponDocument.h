// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

// #611: a creator weapon's document (schema/weapon.schema.json) as plain types, with its strict reader and its writer.
// The plugin's weapon builder writes it; the client builds its weapon definition from it (MantleGameplay). A creator
// supplies a weapon's mesh only: every clip it plays comes from the client's animation set for its hold, so the
// document holds no clips, only which bones play which part of the hold's clips. Nothing here names an asset path:
// meshes and sounds are local names inside the weapon's own set, which the reader of the document resolves. No UObjects:
// any thread.
//
// #840: schema v2 names the hold (sixteen: the eight firearm archetypes, then melee_1h, melee_2h, spear, staff, bow,
// unarmed, shield, throwable) and the delivery (hitscan, projectile, sweep, beam), with charge, block, a resource and each
// delivery's numbers. A v1 document (archetype) reads as v2 with hold = archetype, delivery = hitscan and resource =
// magazine; Write always writes v2. Decision 4 (2026-10-11): a shield is its own item (hold shield), which the client
// pairs at run time with a main-hand weapon by the schema's x-pairings (MantleWeaponArchetypeLibrary::FindPairing); a
// document has no off-hand mesh.
//
// #1025: the consumable hold (a potion, a medkit, food) with its one delivery, use, and the consumable block: an item used
// on its holder, no fire. It has no clips of its own: its use plays the potion clip of whatever the holder holds.

/**
 * An optional value of the document: Value holds the schema's default until the document sets it, and bSet says whether
 * it did (the writer writes only what is set, so a document round-trips as it was written).
 */
template <typename T>
struct TMantleWeaponDocValue
{
	T Value;
	bool bSet = false;

	TMantleWeaponDocValue() : Value() {}
	explicit TMantleWeaponDocValue(const T& InDefault) : Value(InDefault) {}

	void Set(const T& InValue)
	{
		Value = InValue;
		bSet = true;
	}
};

/** schema archetype: v1's hold, v2's optional alias of a firearm hold, in the schema's order (the first eight holds). */
enum class EMantleWeaponDocArchetype : uint8
{
	Pistol,
	Revolver,
	Rifle,
	Smg,
	Dmr,
	PumpShotgun,
	BoltSniper,
	Lmg,
	Count
};

/** schema hold (#840): the animation set the weapon plays, in the schema's order. The first eight are the archetypes, in
 *  their order, so a firearm hold and its archetype convert by value. EMantleWeaponDefHold (MantleGameplay) mirrors it value
 *  for value. Never stored by value: documents spell it, definitions are transient. */
enum class EMantleWeaponDocHold : uint8
{
	Pistol,
	Revolver,
	Rifle,
	Smg,
	Dmr,
	PumpShotgun,
	BoltSniper,
	Lmg,
	Melee1h,
	Melee2h,
	Spear,
	Staff,
	Bow,
	Unarmed,
	/** Decision 4: a shield, the off-hand item (x-pairings). */
	Shield,
	/** Step 4b: one hand, thrown overhand; its projectile is the weapon itself. */
	Throwable,
	/** #1025: an item used on its holder (delivery use, the consumable block). */
	Consumable,
	Count
};

/** schema delivery (#840): what an attack does on the server. */
enum class EMantleWeaponDocDelivery : uint8
{
	Hitscan,
	Projectile,
	Sweep,
	Beam,
	/** #1025: no attack; the item is used on its holder (a consumable). */
	Use,
	Count
};

/** schema resource (#840): what an attack spends. */
enum class EMantleWeaponDocResource : uint8
{
	Magazine,
	Quiver,
	Charges,
	None,
	Count
};

/** schema x-holds charge (#840): whether a hold's clips charge. */
enum class EMantleWeaponDocChargeRule : uint8
{
	/** No charge (the firearms): a document's charge is refused. */
	None,
	/** A melee heavy, a charged cast: a document may have charge. */
	Optional,
	/** A bow's draw: a document must have charge, with required true. */
	Required,
};

/** schema x-holds block (#840): whether a hold's clips block. */
enum class EMantleWeaponDocBlockRule : uint8
{
	/** No block (the firearms, staff, bow): a document's block is refused. */
	None,
	/** The melee holds and unarmed: a document may have block. */
	Optional,
	/** A shield: a document must have block, and it is never disabled. */
	Required,
};

/**
 * schema effects: the firearms' three shot styles, then the elements (#904), in EMantleWeaponSourceStyle's order. An element is valid on a
 * projectile or beam weapon (IsElement; #905: a firearm's too, a laser rifle's beam, a launcher's trail); a firearm style only on a
 * firearm hold.
 */
enum class EMantleWeaponDocEffects : uint8
{
	Rifle,
	Pistol,
	Shotgun,
	Laser,
	Fire,
	Ice,
	Lightning,
	Arcane,
	Venom,
	Water,
	Wind,
	Rock,
	Sand,
	Void,
	Dark,
	Light,
	Generic,
	Heal,
	Lava,
	Nature,
	Count
};

/** schema fire.modes items. */
enum class EMantleWeaponDocFireMode : uint8
{
	Semi,
	Burst,
	Auto,
	Count
};

/** schema reload.style. */
enum class EMantleWeaponDocReloadStyle : uint8
{
	Magazine,
	PerRound,
	ByRoundCount,
	Count
};

/** schema parts' roles: the bones the archetype's weapon clips move, in the schema's order. */
enum class EMantleWeaponDocRole : uint8
{
	Bolt,
	Trigger,
	Hammer,
	Slide,
	Cylinder,
	Pump,
	Feed,
	Magazine,
	MagazineRelease,
	Shell,
	Cartridge,
	SpeedLoader,
	/** #840: stowed roles (x-holds stowedRoles). */
	Round,
	Arrow,
	Count
};

/** schema $defs.action: an action of an archetype's animation set, in the schema's order. */
enum class EMantleWeaponDocAction : uint8
{
	Equip,
	Holster,
	Idle,
	Fire,
	FireLast,
	ReloadTactical,
	ReloadEmpty,
	ReloadStart,
	ReloadStartEmpty,
	ReloadLoop,
	ReloadEnd,
	ReloadPartial,
	/** #840: the melee, magic and bow holds' actions. */
	Attack,
	Heavy,
	Block,
	Throw,
	Cast,
	Nock,
	Draw,
	Release,
	/** #1025: a consumable's use. */
	Use,
	Count
};

/** schema sockets, in the schema's order. */
enum class EMantleWeaponDocSocket : uint8
{
	Sight,
	Muzzle,
	GripR,
	GripL,
	Magazine,
	ShellEject,
	/** #840: the melee, magic and bow holds' points. */
	EdgeBase,
	EdgeTip,
	Tip,
	Focus,
	StringTop,
	StringBottom,
	Nock,
	/** Decision 4: a shield's handle and the middle of its face. */
	Grip,
	Face,
	Count
};

/** A curve key's interpolation: [t, v] and [t, v, "linear"] are linear. */
enum class EMantleWeaponDocInterp : uint8
{
	Linear,
	Constant,
	Cubic,
	Count
};

/** schema x-holds (#840; x-archetypes before it, whose rows are the firearm holds'): what the client's animation set for a
 *  hold does. */
struct FMantleWeaponDocHoldInfo
{
	/** The way its clips reload: a document's reload.style must be this. Unset (bHasReloadStyle false) on a hold whose
	 *  resources hold no rounds. */
	bool bHasReloadStyle = true;
	EMantleWeaponDocReloadStyle ReloadStyle = EMantleWeaponDocReloadStyle::Magazine;
	/** Held in both hands when the document does not say; a two-handed hold cannot be made one-handed. */
	bool bTwoHanded = true;
	/** The effects when the document does not say; a hold without (bHasEffects false) refuses a document's effects. */
	bool bHasEffects = true;
	EMantleWeaponDocEffects Effects = EMantleWeaponDocEffects::Rifle;
	/** The parts its weapon clips move: with StowedRoles, the only roles a document of this hold may map. */
	TArray<EMantleWeaponDocRole> Roles;
	/** Roles it accepts only as stowed parts (with shownDuring): a projectile gun's round, a bow's arrow. */
	TArray<EMantleWeaponDocRole> StowedRoles;
	/** Its actions: the only ones a stowed part may be shown during. */
	TArray<EMantleWeaponDocAction> Actions;
	/** The deliveries it may use. */
	TArray<EMantleWeaponDocDelivery> Deliveries;
	/** Whether a document charges. */
	EMantleWeaponDocChargeRule Charge = EMantleWeaponDocChargeRule::None;
	/** The resources it may use; the first is a document's when it names none. */
	TArray<EMantleWeaponDocResource> Resources;
	/** Whether a document blocks: never, may, or must (a shield). */
	EMantleWeaponDocBlockRule BlockRule = EMantleWeaponDocBlockRule::None;
	/** A document needs a mesh: x-holds mesh, required on every hold (#897: unarmed's is the fist weapon on the hand). */
	bool bMeshRequired = true;
	/** The points a document must place, and those it may. */
	TArray<EMantleWeaponDocSocket> Sockets;
	TArray<EMantleWeaponDocSocket> OptionalSockets;
};

/** The pre-#840 name: an archetype's entry is its firearm hold's. */
using FMantleWeaponDocArchetypeInfo = FMantleWeaponDocHoldInfo;

/** A curve key: [time, value], [time, value, "linear" | "constant"] or [time, value, "cubic", arrive, leave]. */
struct FMantleWeaponDocCurveKey
{
	double Time = 0.0;
	double Value = 0.0;
	EMantleWeaponDocInterp Interp = EMantleWeaponDocInterp::Linear;
	/** Cubic only. */
	double ArriveTangent = 0.0;
	double LeaveTangent = 0.0;
};

/** Keys in strictly rising time. */
using FMantleWeaponDocCurve = TArray<FMantleWeaponDocCurveKey>;

/** schema $defs.curve3: one curve per channel. */
struct FMantleWeaponDocCurve3
{
	FMantleWeaponDocCurve X;
	FMantleWeaponDocCurve Y;
	FMantleWeaponDocCurve Z;
};

/** A point on the weapon in a bone's space. */
struct FMantleWeaponDocSocket
{
	/** A bone of the weapon mesh, spelled as the document spells it (bone names ignore case). */
	FString Bone;
	/** cm. */
	TMantleWeaponDocValue<FVector> Location{ FVector::ZeroVector };
	/** A unit quaternion (within 0.01). */
	TMantleWeaponDocValue<FQuat> Rotation{ FQuat::Identity };
};

/** The bone that plays a role of the archetype's clips. */
struct FMantleWeaponDocPart
{
	FString Bone;
	/** Set: a stowed part, hidden except while one of these of the archetype's actions plays. */
	TMantleWeaponDocValue<TArray<EMantleWeaponDocAction>> ShownDuring;
};

/** schema parts: by role. */
struct FMantleWeaponDocParts
{
	TMantleWeaponDocValue<FMantleWeaponDocPart> Roles[static_cast<int32>(EMantleWeaponDocRole::Count)];

	TMantleWeaponDocValue<FMantleWeaponDocPart>& operator[](const EMantleWeaponDocRole Role) { return Roles[static_cast<int32>(Role)]; }
	const TMantleWeaponDocValue<FMantleWeaponDocPart>& operator[](const EMantleWeaponDocRole Role) const { return Roles[static_cast<int32>(Role)]; }
};

/** A mesh, an effect or both that the weapon carries on its mesh (mesh or effect is required). */
struct FMantleWeaponDocAttachment
{
	/** Letters, digits and _; unique in the weapon (ignoring case). */
	FString Name;
	/** A local name; empty on an effect-only attachment. */
	FString Mesh;
	/** #904: an element's held effect, looping where the attachment sits (a staff's glowing head). */
	TMantleWeaponDocValue<EMantleWeaponDocEffects> Effect;
	/** Unset: the weapon mesh's origin. Never with Socket. */
	TMantleWeaponDocValue<FString> Bone;
	/** #904: one of the document's points it rides (location, rotation and scale then relative to it); the point must be set. */
	TMantleWeaponDocValue<EMantleWeaponDocSocket> Socket;
	TMantleWeaponDocValue<FVector> Location{ FVector::ZeroVector };
	TMantleWeaponDocValue<FQuat> Rotation{ FQuat::Identity };
	TMantleWeaponDocValue<FVector> Scale{ FVector::OneVector };
	TMantleWeaponDocValue<bool> bHidden{ false };
	/**
	 * Optional (#898): the material for each slot of the attachment's mesh in slot order, local names (an uploaded material's id
	 * once published), as the weapon's own Materials. Unset: the mesh draws with the materials it was built with (a local test) or
	 * untextured (a published weapon: an uploaded mesh carries only its slots' names).
	 */
	TMantleWeaponDocValue<TArray<FString>> Materials;
};

struct FMantleWeaponDocFire
{
	/** Seconds between shots (required). */
	double Interval = 0.1;
	TMantleWeaponDocValue<bool> bAutomatic{ true };
	TMantleWeaponDocValue<TArray<EMantleWeaponDocFireMode>> Modes;
	TMantleWeaponDocValue<double> Damage{ 25.0 };
	TMantleWeaponDocValue<double> MaxRange{ 25000.0 };
	TMantleWeaponDocValue<double> TraceSweepRadius{ 0.0 };
	TMantleWeaponDocValue<int32> Pellets{ 1 };
	TMantleWeaponDocValue<double> PelletSpread{ 1.0 };
	TMantleWeaponDocValue<double> PlainRecoil{ 0.5 };
};

struct FMantleWeaponDocMagazine
{
	/** Rounds (required). */
	int32 Size = 1;
	TMantleWeaponDocValue<int32> StartingReserve{ 120 };
};

struct FMantleWeaponDocReload
{
	/** Required; the archetype's. */
	EMantleWeaponDocReloadStyle Style = EMantleWeaponDocReloadStyle::Magazine;
	TMantleWeaponDocValue<double> Seconds{ 1.6 };
};

struct FMantleWeaponDocSounds
{
	/** Local names. */
	TMantleWeaponDocValue<FString> Fire;
	TMantleWeaponDocValue<FString> FireSuppressed;
	TMantleWeaponDocValue<bool> bSuppressed{ false };
	/** A trigger input on the fire sound; unset: one-shots. */
	TMantleWeaponDocValue<FString> Trigger;
};

/** schema charge (#840): hold to charge, release to attack. */
struct FMantleWeaponDocCharge
{
	/** Seconds to a full charge (required). */
	double Seconds = 1.0;
	TMantleWeaponDocValue<bool> bRequired{ false };
	TMantleWeaponDocValue<bool> bReleaseOnFull{ false };
	TMantleWeaponDocValue<bool> bCancelable{ true };
};

/** schema block (#840). */
struct FMantleWeaponDocBlock
{
	TMantleWeaponDocValue<bool> bEnabled{ true };
	TMantleWeaponDocValue<bool> bBreakOnHeavy{ true };
};

/** schema charges (#840): the charges resource's numbers. */
struct FMantleWeaponDocCharges
{
	int32 Count = 1;
	double RefillSeconds = 1.0;
};

/** schema projectile (#840). */
struct FMantleWeaponDocProjectile
{
	/** cm/s (required). */
	double Speed = 5000.0;
	TMantleWeaponDocValue<double> Gravity{ 1.0 };
	TMantleWeaponDocValue<double> BlastRadius{ 0.0 };
	/** A grenade's fuse, seconds (needs a blast). */
	TMantleWeaponDocValue<double> FuseSeconds{ 2.0 };
	TMantleWeaponDocValue<bool> bThrown{ false };
	TMantleWeaponDocValue<bool> bStick{ false };
	TMantleWeaponDocValue<bool> bPickup{ false };
	/** The burning area it leaves where it lands, seconds (a molotov); with its damage a second and its radius (cm). */
	TMantleWeaponDocValue<double> LingerSeconds{ 0.0 };
	TMantleWeaponDocValue<double> LingerDamagePerSecond{ 0.0 };
	TMantleWeaponDocValue<double> LingerRadius{ 200.0 };
	/** #905: a local name, the static or skeletal mesh that flies (a launcher's rocket, a creator's arrow); never on a thrown one, which
	 *  flies as the weapon itself. Unset: a bow's arrow is a thin shaft, a launcher's round its element's trail or a small dark ball. */
	TMantleWeaponDocValue<FString> Mesh;
	/** #905: that mesh's material per slot, as an attachment's (FMantleWeaponDocAttachment::Materials); without Mesh they go on nothing. */
	TMantleWeaponDocValue<TArray<FString>> Materials;
};

/** schema sweep.edge (#840): the two sockets the edge sweeps between. */
struct FMantleWeaponDocSweepEdge
{
	EMantleWeaponDocSocket Base = EMantleWeaponDocSocket::EdgeBase;
	EMantleWeaponDocSocket Tip = EMantleWeaponDocSocket::EdgeTip;
};

/** schema sweep (#840). */
struct FMantleWeaponDocSweep
{
	/** cm (required). */
	double Reach = 100.0;
	/** Unset: edgeBase to edgeTip (edgeBase to tip on a spear hold, grip to face on a shield; unarmed sweeps from the hands). */
	TMantleWeaponDocValue<FMantleWeaponDocSweepEdge> Edge;
	TMantleWeaponDocValue<int32> ComboCount{ 1 };
	TMantleWeaponDocValue<double> HeavyMultiplier{ 2.0 };
};

/** schema beam (#840). */
struct FMantleWeaponDocBeam
{
	TMantleWeaponDocValue<double> TickRate{ 10.0 };
	/** Required. */
	double DamagePerSecond = 0.0;
	TMantleWeaponDocValue<double> MaxSeconds{ 5.0 };
	/** #905: cm, the beam's thickness (a sphere swept along it, a flamethrower's stream); 0 traces a line (a laser). */
	TMantleWeaponDocValue<double> Radius{ 0.0 };
	/** #905: cm the beam reaches; unset: fire.maxRange. */
	TMantleWeaponDocValue<double> Range{ 25000.0 };
};

/** schema consumable.style (#1025): which of each hold's two potion clips a use plays. */
enum class EMantleWeaponDocUseStyle : uint8
{
	/** Raised to the mouth: each hold's _Potion_01 clip (a potion, a drink, food). */
	EatDrink,
	/** Lifted beside the head and crushed: each hold's _Potion_02 clip (a medkit, a charm). */
	Use,
	Count
};

/** schema consumable.effect (#1025): what the item does by itself at the effect moment. */
enum class EMantleWeaponDocConsumableEffect : uint8
{
	/** Heals the holder (Heal, HealSeconds). */
	Heal,
	/** Nothing built in: the place's server script does it all (Backpack.ItemUsed fires either way). */
	None,
	Count
};

/** schema consumable (#1025): the use delivery's numbers. */
struct FMantleWeaponDocConsumable
{
	/** Required. */
	EMantleWeaponDocUseStyle Style = EMantleWeaponDocUseStyle::EatDrink;
	TMantleWeaponDocValue<EMantleWeaponDocConsumableEffect> Effect{ EMantleWeaponDocConsumableEffect::Heal };
	/** With effect heal: health given, up to the maximum. */
	TMantleWeaponDocValue<double> Heal{ 50.0 };
	/** With effect heal. 0: the whole heal at the effect moment; above 0: spread evenly over this many seconds from it. */
	TMantleWeaponDocValue<double> HealSeconds{ 0.0 };
	TMantleWeaponDocValue<int32> Uses{ 1 };
	/** Seconds one use takes; unset: the clip's own length (the value here is only the clips' length, 2.5). */
	TMantleWeaponDocValue<double> UseSeconds{ 2.5 };
	TMantleWeaponDocValue<double> CooldownSeconds{ 0.0 };
	TMantleWeaponDocValue<bool> bMoveWhileUsing{ true };
};

/** The walk and sprint clips' play rates are the archetype library's, not the weapon's. */
struct FMantleWeaponDocFirstPerson
{
	TMantleWeaponDocValue<double> SightDistance{ 0.0 };
	TMantleWeaponDocValue<double> WalkFullWeightSpeed{ 200.0 };
};

struct FMantleWeaponDocSpreadState
{
	TMantleWeaponDocValue<double> Multiplier{ 1.0 };
	TMantleWeaponDocValue<double> Rate{ 5.0 };
};

struct FMantleWeaponDocSpread
{
	TMantleWeaponDocValue<double> Exponent{ 1.0 };
	TMantleWeaponDocValue<FMantleWeaponDocCurve> HeatToSpread;
	TMantleWeaponDocValue<FMantleWeaponDocCurve> HeatToHeatPerShot;
	TMantleWeaponDocValue<FMantleWeaponDocCurve> HeatToCooldownPerSecond;
	TMantleWeaponDocValue<double> RecoveryDelay{ 0.0 };
	TMantleWeaponDocValue<bool> bFirstShotAccuracy{ false };
	TMantleWeaponDocValue<FMantleWeaponDocSpreadState> Aiming;
	TMantleWeaponDocValue<FMantleWeaponDocSpreadState> StandingStill;
	TMantleWeaponDocValue<FMantleWeaponDocSpreadState> Crouching;
	TMantleWeaponDocValue<FMantleWeaponDocSpreadState> JumpingOrFalling;
	TMantleWeaponDocValue<double> StandingStillSpeed{ 80.0 };
	TMantleWeaponDocValue<double> StandingStillToMovingRange{ 20.0 };
	TMantleWeaponDocValue<FMantleWeaponDocCurve> DamageFalloff;
	TMantleWeaponDocValue<double> WeakSpotMultiplier{ 1.5 };
};

/** schema $defs.swayVector. The vectors have no schema default; unset they read as zero. */
struct FMantleWeaponDocSwayVector
{
	TMantleWeaponDocValue<FVector> Stiffness{ FVector::ZeroVector };
	TMantleWeaponDocValue<FVector> Damping{ FVector::ZeroVector };
	TMantleWeaponDocValue<FVector> InterpSpeed{ FVector::ZeroVector };
	TMantleWeaponDocValue<FVector> SmoothingSpeed{ FVector::ZeroVector };
	TMantleWeaponDocValue<FVector> Clamp{ FVector::ZeroVector };
	TMantleWeaponDocValue<FVector> Scale{ FVector::ZeroVector };
	TMantleWeaponDocValue<double> AimScale{ 1.0 };
};

struct FMantleWeaponDocSwayBlock
{
	TMantleWeaponDocValue<FMantleWeaponDocSwayVector> Position;
	TMantleWeaponDocValue<FMantleWeaponDocSwayVector> Rotation;
};

struct FMantleWeaponDocSway
{
	TMantleWeaponDocValue<FMantleWeaponDocSwayBlock> Movement;
	TMantleWeaponDocValue<FMantleWeaponDocSwayBlock> Look;
};

/** schema $defs.recoilKick: [min, max] ranges; Yaw and Roll hold two ranges, [X..Y] and [Z..W]. */
struct FMantleWeaponDocRecoilKick
{
	TMantleWeaponDocValue<FVector2D> Pitch{ FVector2D::ZeroVector };
	TMantleWeaponDocValue<FVector4> Yaw{ FVector4(0.0, 0.0, 0.0, 0.0) };
	TMantleWeaponDocValue<FVector4> Roll{ FVector4(0.0, 0.0, 0.0, 0.0) };
	TMantleWeaponDocValue<FVector2D> Back{ FVector2D::ZeroVector };
	TMantleWeaponDocValue<FVector2D> Right{ FVector2D::ZeroVector };
	TMantleWeaponDocValue<FVector2D> Up{ FVector2D::ZeroVector };
	TMantleWeaponDocValue<FVector> Pivot{ FVector::ZeroVector };
};

struct FMantleWeaponDocSpringAmount
{
	TMantleWeaponDocValue<double> Amount{ 0.0 };
	TMantleWeaponDocValue<double> Damping{ 0.0 };
	TMantleWeaponDocValue<double> InterpSpeed{ 0.0 };
};

struct FMantleWeaponDocRecoilCurves
{
	TMantleWeaponDocValue<FMantleWeaponDocCurve3> SemiRotation;
	TMantleWeaponDocValue<FMantleWeaponDocCurve3> SemiLocation;
	TMantleWeaponDocValue<FMantleWeaponDocCurve3> AutoRotation;
	TMantleWeaponDocValue<FMantleWeaponDocCurve3> AutoLocation;
};

struct FMantleWeaponDocRecoilNoise
{
	TMantleWeaponDocValue<FVector2D> Target{ FVector2D::ZeroVector };
	TMantleWeaponDocValue<FVector2D> InterpSpeed{ FVector2D::ZeroVector };
	TMantleWeaponDocValue<FVector2D> Damping{ FVector2D::ZeroVector };
	TMantleWeaponDocValue<double> AimScale{ 1.0 };
};

struct FMantleWeaponDocRecoilProgress
{
	TMantleWeaponDocValue<FMantleWeaponDocSpringAmount> Pitch;
	TMantleWeaponDocValue<FMantleWeaponDocSpringAmount> Up;
	TMantleWeaponDocValue<double> AimScale{ 1.0 };
};

struct FMantleWeaponDocRecoilSway
{
	TMantleWeaponDocValue<FVector2D> Pitch{ FVector2D::ZeroVector };
	TMantleWeaponDocValue<FVector2D> Yaw{ FVector2D::ZeroVector };
	TMantleWeaponDocValue<double> RollScale{ 0.0 };
	TMantleWeaponDocValue<double> Damping{ 0.0 };
	TMantleWeaponDocValue<double> InterpSpeed{ 0.0 };
	TMantleWeaponDocValue<double> AimScale{ 1.0 };
	TMantleWeaponDocValue<FVector> Pivot{ FVector::ZeroVector };
};

struct FMantleWeaponDocControllerRecoil
{
	TMantleWeaponDocValue<FVector2D> YawStep{ FVector2D::ZeroVector };
	TMantleWeaponDocValue<FVector2D> PitchStep{ FVector2D::ZeroVector };
	TMantleWeaponDocValue<FVector2D> InterpSpeed{ FVector2D::ZeroVector };
	TMantleWeaponDocValue<double> Damping{ 0.0 };
	TMantleWeaponDocValue<double> Compensation{ 0.0 };
};

struct FMantleWeaponDocCameraShake
{
	TMantleWeaponDocValue<FMantleWeaponDocCurve3> Curve;
	TMantleWeaponDocValue<FVector2D> Pitch{ FVector2D::ZeroVector };
	TMantleWeaponDocValue<FVector2D> Yaw{ FVector2D::ZeroVector };
	TMantleWeaponDocValue<FVector2D> Roll{ FVector2D::ZeroVector };
	TMantleWeaponDocValue<double> Smoothing{ 0.0 };
	TMantleWeaponDocValue<double> PlayRate{ 1.0 };
};

struct FMantleWeaponDocRecoil
{
	TMantleWeaponDocValue<FMantleWeaponDocRecoilKick> Hip;
	TMantleWeaponDocValue<FMantleWeaponDocRecoilKick> Aim;
	TMantleWeaponDocValue<FMantleWeaponDocRecoilCurves> Curves;
	TMantleWeaponDocValue<FVector> SmoothRotation{ FVector::ZeroVector };
	TMantleWeaponDocValue<FVector> SmoothLocation{ FVector::ZeroVector };
	TMantleWeaponDocValue<FVector> AutoRotationScale{ FVector::OneVector };
	TMantleWeaponDocValue<FVector> AutoLocationScale{ FVector::OneVector };
	TMantleWeaponDocValue<bool> bAlternateRoll{ false };
	/** [pitch, yaw, roll] degrees from the weapon's frame. */
	TMantleWeaponDocValue<FVector> Space{ FVector::ZeroVector };
	TMantleWeaponDocValue<double> PlayRate{ 1.0 };
	TMantleWeaponDocValue<double> PlaybackOffset{ 0.0 };
	TMantleWeaponDocValue<double> BurstEndSeconds{ 0.1 };
	TMantleWeaponDocValue<FMantleWeaponDocRecoilNoise> Noise;
	TMantleWeaponDocValue<FMantleWeaponDocSpringAmount> Pushback;
	TMantleWeaponDocValue<FMantleWeaponDocRecoilProgress> Progress;
	TMantleWeaponDocValue<FMantleWeaponDocRecoilSway> Sway;
	TMantleWeaponDocValue<FMantleWeaponDocControllerRecoil> Controller;
	TMantleWeaponDocValue<FMantleWeaponDocCameraShake> CameraShake;
};

/**
 * A creator weapon's document, schema/weapon.schema.json v2 (#840), field for field; a v1 document reads into it as v2.
 * Units are Unreal's (cm, degrees, seconds, cm/s); the weapon's axes are +X down the bore, +Y right, +Z up. An unset block
 * (Spread, Sway, Recoil) means the hold's; IsTwoHanded and GetEffects apply the hold's when the document leaves them out.
 */
struct MANTLERUNTIME_API FMantleWeaponDesc
{
	/** The schemaVersion the document was read from (Parse sets it; Write always writes FMantleWeaponDocument::SchemaVersion). */
	int32 ReadSchemaVersion = 2;
	/** The display name. */
	FString Name;
	/**
	 * The firearm alias of the hold: Parse keeps it equal to Hold when Hold is a firearm (v1's archetype, v2's hold), and
	 * leaves it at its default otherwise, so read it only through GetHold / IsFirearmHold. Write never writes it (v2
	 * writers set hold). Code from before #840 that sets only Archetype gets that firearm as its hold (GetHold).
	 */
	EMantleWeaponDocArchetype Archetype = EMantleWeaponDocArchetype::Rifle;
	/** Unset: Archetype's firearm hold (GetHold). Parse always sets it. */
	TMantleWeaponDocValue<EMantleWeaponDocHold> Hold;
	/** Unset: hitscan (GetDelivery). Parse always sets it. */
	TMantleWeaponDocValue<EMantleWeaponDocDelivery> Delivery;
	/** Unset: the hold's (IsTwoHanded). */
	TMantleWeaponDocValue<bool> bTwoHanded;
	/** Unset: the hold's (GetEffects). */
	TMantleWeaponDocValue<EMantleWeaponDocEffects> Effects;
	/** sRGB. */
	TMantleWeaponDocValue<FColor> Color{ FColor(40, 110, 160) };
	/** #904: sRGB, multiplied into the element's effects; only where the weapon has an element (GetElement). Unset: the element's own colours. */
	TMantleWeaponDocValue<FColor> EffectsColor;
	/** The weapon's skeletal mesh, a local name; every version-2 hold needs one (#897), so empty only on a refused document. */
	FString Mesh;
	/**
	 * Optional (#789): the material for each slot of the mesh in slot order, local names (an uploaded material's id once
	 * published). Unset: the slots draw untextured, or with whatever the caller passes beside the document.
	 */
	TMantleWeaponDocValue<TArray<FString>> Materials;
	/**
	 * Optional: the texture the hotbar shows for the weapon, a local name (an uploaded texture's id once published), the
	 * library thumbnail. Empty: the hotbar shows no picture.
	 */
	FString Icon;
	/** The hold's sockets are required, its optional sockets allowed (x-holds); the sockets block is written when any is set. */
	TMantleWeaponDocValue<FMantleWeaponDocSocket> Sockets[static_cast<int32>(EMantleWeaponDocSocket::Count)];
	TMantleWeaponDocValue<FMantleWeaponDocParts> Parts;
	TMantleWeaponDocValue<TArray<FMantleWeaponDocAttachment>> Attachments;
	FMantleWeaponDocFire Fire;
	/** #840: the charge modifier (required on a bow, refused on a firearm). */
	TMantleWeaponDocValue<FMantleWeaponDocCharge> Charge;
	/** In the document (read and written) exactly when the resource holds rounds (UsesRounds). */
	FMantleWeaponDocMagazine Magazine;
	FMantleWeaponDocReload Reload;
	/** #840: unset: the hold's first resource (GetResource). */
	TMantleWeaponDocValue<EMantleWeaponDocResource> Resource;
	/** #840: the charges resource's numbers, exactly when the resource is charges. */
	TMantleWeaponDocValue<FMantleWeaponDocCharges> Charges;
	/** #840: each delivery's numbers: the delivery's own is required; no other but a spear's throw beside its sweep. */
	TMantleWeaponDocValue<FMantleWeaponDocProjectile> Projectile;
	TMantleWeaponDocValue<FMantleWeaponDocSweep> Sweep;
	TMantleWeaponDocValue<FMantleWeaponDocBeam> Beam;
	/** #1025: the use delivery's numbers, exactly when the delivery is use. */
	TMantleWeaponDocValue<FMantleWeaponDocConsumable> Consumable;
	/** #840: blocking, on a hold that blocks. */
	TMantleWeaponDocValue<FMantleWeaponDocBlock> Block;
	TMantleWeaponDocValue<FMantleWeaponDocSounds> Sounds;
	TMantleWeaponDocValue<FMantleWeaponDocFirstPerson> FirstPerson;
	TMantleWeaponDocValue<FMantleWeaponDocSpread> Spread;
	TMantleWeaponDocValue<FMantleWeaponDocSway> Sway;
	TMantleWeaponDocValue<FMantleWeaponDocRecoil> Recoil;

	TMantleWeaponDocValue<FMantleWeaponDocSocket>& Socket(const EMantleWeaponDocSocket Which) { return Sockets[static_cast<int32>(Which)]; }
	const TMantleWeaponDocValue<FMantleWeaponDocSocket>& Socket(const EMantleWeaponDocSocket Which) const { return Sockets[static_cast<int32>(Which)]; }

	/** The hold: Hold when set, else Archetype's firearm hold. */
	EMantleWeaponDocHold GetHold() const;

	/** The delivery: Delivery when set, else hitscan. */
	EMantleWeaponDocDelivery GetDelivery() const;

	/** The resource: Resource when set, else the hold's first. */
	EMantleWeaponDocResource GetResource() const;

	/** The resource holds rounds (magazine or quiver): the document has a magazine and a reload. */
	bool UsesRounds() const;

	/** The document's twoHanded, else the hold's. */
	bool IsTwoHanded() const;

	/** The document's effects, else the hold's (rifle on a hold without effects; HasEffects says whether it has any). */
	EMantleWeaponDocEffects GetEffects() const;

	/** The weapon has a firearm's shot effects: the document names a firearm style, or its hold has them. */
	bool HasEffects() const;

	/** #904: the weapon's element: the document's effects when it is one, else Arcane on a staff (its hold's look); false for none. */
	bool GetElement(EMantleWeaponDocEffects& OutElement) const;

	/** The part playing Role, or null when the weapon maps none. */
	const FMantleWeaponDocPart* FindPart(EMantleWeaponDocRole Role) const;
};

/** Why a document was refused: where (a JSON pointer without its leading slash, "" for the document), which rule, and what to do. */
struct MANTLERUNTIME_API FMantleWeaponDocError
{
	/** As validate.py reports it, such as "sockets/gripR/rotation". */
	FString Path;
	/** The JSON Schema keyword that refuses it (required, type, minimum, enum, pattern...), or "mantle" for a rule across fields (validate.py's check_weapon_rules), or "json" when the text is not JSON. */
	FString Keyword;
	/** What is wrong, then what to do about it. */
	FString Message;

	/** "/sockets/gripR/rotation is a quaternion of length 0.500 ...". */
	FString ToString() const;
};

/**
 * Reads and writes the weapon document. Parse is strict, like every reader of what a creator hands in: unknown keys
 * (case-sensitive), wrong types, values out of the schema's ranges or patterns, and every rule validate.py's
 * check_weapon_rules applies are refused, each with where it is and what to do. Write puts keys in the schema's order,
 * only what is set, numbers in their shortest exact form: Write(Parse(x)) is x canonicalised, and parses back to the same
 * document.
 */
class MANTLERUNTIME_API FMantleWeaponDocument
{
public:
	/** The schemaVersion this build writes (#840); it reads it and every older one (1). */
	static constexpr int32 SchemaVersion = 2;

	/** The oldest schemaVersion this build reads. */
	static constexpr int32 OldestSchemaVersion = 1;

	/** The longest document text read, in characters. */
	static constexpr int32 MaxDocumentLength = 1024 * 1024;

	/** The refusal of an unarmed document without a mesh (#897), in a creator's words and the same as validate.py's and the
	 *  cook's: a fist weapon is worn on the hand; empty hands are built into the game and are never a document. */
	static constexpr const TCHAR* FistWeaponNeedsModel =
		TEXT("needs a 3D model: a fist weapon is the knuckles, claws or gauntlet the hand wears; pick that model");

	/** Parses a document. False with every problem found; Out is then the default. */
	static bool Parse(const FString& Json, FMantleWeaponDesc& Out, TArray<FMantleWeaponDocError>& OutErrors);

	/** Parses a document; OutError holds every problem, one per line. */
	static bool Parse(const FString& Json, FMantleWeaponDesc& Out, FString& OutError);

	/**
	 * The rules across fields (validate.py's check_weapon_rules) on a document whose every value is already within the
	 * schema: the hold's hands, points, parts, actions and reload style; gripL on every two-handed weapon (optional on a
	 * one-handed one, the support hand); unit rotations; no attachment scaled to nothing and none named twice; one role per
	 * bone; curves in rising time; and the hold's allow-list (#840): its deliveries and the delivery's block, its charge
	 * and block rules, its resources and the charges' numbers, a mesh where the hold needs one. What depends on
	 * which keys the text has (version 1's fields, the archetype alias, a magazine or reload the resource does not hold,
	 * points without a mesh) is Parse's.
	 */
	static void CheckRules(const FMantleWeaponDesc& Desc, TArray<FMantleWeaponDocError>& OutErrors);

	/** Whether a document built in code (the converter's) is one Parse accepts: Write, then Parse. */
	static bool Validate(const FMantleWeaponDesc& Desc, TArray<FMantleWeaponDocError>& OutErrors);

	/** #1025: what a consumable (delivery use) refuses, the top-level keys of a weapon's numbers and attachments (its mesh shows alone). */
	static TConstArrayView<const TCHAR*> GetUseRefusedFields();

	/** The document's text, UTF-8-ready, "\n" line ends, a line end at the end. */
	static FString Write(const FMantleWeaponDesc& Desc);

	/** schema x-holds' entry for Hold (#840). */
	static const FMantleWeaponDocHoldInfo& GetHoldInfo(EMantleWeaponDocHold Hold);

	/** schema x-holds' entry for Archetype's firearm hold (x-archetypes' before #840). */
	static const FMantleWeaponDocArchetypeInfo& GetArchetypeInfo(EMantleWeaponDocArchetype Archetype);

	/** A firearm hold: one of the eight archetypes. */
	static bool IsFirearmHold(EMantleWeaponDocHold Hold);

	/** Archetype's hold. */
	static EMantleWeaponDocHold HoldOfArchetype(EMantleWeaponDocArchetype Archetype);

	/** Hold's archetype, false when Hold is no firearm. */
	static bool ArchetypeOfHold(EMantleWeaponDocHold Hold, EMantleWeaponDocArchetype& OutArchetype);

	/** The document's spellings. */
	static const TCHAR* ArchetypeName(EMantleWeaponDocArchetype Archetype);
	static const TCHAR* HoldName(EMantleWeaponDocHold Hold);
	static const TCHAR* DeliveryName(EMantleWeaponDocDelivery Delivery);
	static const TCHAR* ResourceName(EMantleWeaponDocResource Resource);
	static const TCHAR* RoleName(EMantleWeaponDocRole Role);
	static const TCHAR* ActionName(EMantleWeaponDocAction Action);
	static const TCHAR* SocketName(EMantleWeaponDocSocket Socket);
	static const TCHAR* ReloadStyleName(EMantleWeaponDocReloadStyle Style);
	static const TCHAR* EffectsName(EMantleWeaponDocEffects Effects);
	static const TCHAR* UseStyleName(EMantleWeaponDocUseStyle Style);
	static const TCHAR* ConsumableEffectName(EMantleWeaponDocConsumableEffect Effect);

	/** #904: Effects is an element (not one of the firearms' rifle, pistol and shotgun). */
	static bool IsElement(EMantleWeaponDocEffects Effects) { return Effects > EMantleWeaponDocEffects::Shotgun && Effects < EMantleWeaponDocEffects::Count; }
};
