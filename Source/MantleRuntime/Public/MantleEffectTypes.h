// Copyright Joshua Eger. All Rights Reserved.
// GhzGarage/mantle#460: the vocabulary of place particle effects, shared by the place file (the Effect
// entity), the actor a creator places and the runtime builds (AMantleEffect, MantleEffect.h) and the
// script API (Effects:Play, Entity:PlayEffect).
//
// Creators cannot publish a Niagara system of their own: a shipped client cannot compile one. Mantle
// ships a small set of parametric archetypes instead, each a Niagara system cooked into every client
// under /Game/Mantle/Effects (tools/effects/Create-MantleEffects.py makes them), each driven by the same
// five user parameters. An effect is an archetype plus those five values; nothing else reaches a player.
// The exception (#628) is the sprites archetype, one general sprite emitter (NS_MantleSprites) with a closed,
// larger parameter set (FMantleSpritesSettings) and a texture, which the editor's effect export translates a
// project's own particle systems onto.
//
// MantleCore has no plugin dependencies, Niagara included, so nothing here links Niagara: the systems are
// loaded as UFXSystemAsset and drawn through the engine's UFXSystemComponent interface, the component
// class found by name (FMantleEffects::ComponentClass). Where Niagara or an archetype's asset is missing,
// that is logged once and the effect draws nothing; the place still builds.

#pragma once

#include "CoreMinimal.h"

#include "MantleEffectTypes.generated.h"

class UClass;
class UFXSystemAsset;

/** A built-in effect. The place schema's archetype enum, in this order. */
UENUM()
enum class EMantleEffectArchetype : uint8
{
	Smoke,
	Fire,
	Sparks,
	Dust,
	Glow,
	/** #628: the general sprite emitter, tuned by FMantleSpritesSettings. Not offered to scripts. */
	Sprites
};

/** How a sprites effect's particles mix with what is behind them. The schema's blend enum, in this order. */
UENUM()
enum class EMantleSpritesBlend : uint8
{
	Translucent,
	Additive
};

/** Which frame of a flipbook sheet (subImages) a sprites effect's particle shows. The schema's subImageMode enum, in this order. */
UENUM()
enum class EMantleSpritesSubImageMode : uint8
{
	/** One random frame per particle, held for its life. */
	Random,
	/** The frames played in order across the particle's life. */
	OverLife
};

/** Where a sprites effect's particles appear. The schema's shape.type enum, in this order. */
UENUM()
enum class EMantleSpritesShape : uint8
{
	Point,
	Box,
	Sphere,
	Cylinder
};

/**
 * What a sprites effect (#628) looks like beyond the five values every archetype has (FMantleEffectLook: the
 * colour at birth, and the rate). The schema's keys of the same names, each already held to the schema's ranges
 * and pairs by the parser, with the schema's defaults: a key a place file leaves out takes the default here, and
 * a lone min or max takes the other's value (FMantleEffects::ResolveSprites). Lengths are cm, speeds cm/s,
 * accelerations cm/s^2, times seconds, in the effect's own space.
 */
USTRUCT()
struct MANTLERUNTIME_API FMantleSpritesSettings
{
	GENERATED_BODY()

	/** Translucent uses the texture's alpha; additive adds its colour to the scene. */
	UPROPERTY(EditAnywhere, Category = "Sprites")
	EMantleSpritesBlend Blend = EMantleSpritesBlend::Translucent;

	/** The colour at the end of a particle's life, blended linearly from the effect's Color. */
	UPROPERTY(EditAnywhere, Category = "Sprites", meta = (HideAlphaChannel))
	FLinearColor ColorEnd = FLinearColor::White;

	/** Opacity at its peak, 0 to 1. */
	UPROPERTY(EditAnywhere, Category = "Sprites", meta = (ClampMin = "0", ClampMax = "1", UIMin = "0", UIMax = "1"))
	double Alpha = 1.0;

	/** Opacity at the end of a particle's life, 0 to 1. */
	UPROPERTY(EditAnywhere, Category = "Sprites", meta = (ClampMin = "0", ClampMax = "1", UIMin = "0", UIMax = "1"))
	double AlphaEnd = 0.0;

	/** The fraction of its life a particle takes to fade in from 0 to Alpha, 0 to 1. */
	UPROPERTY(EditAnywhere, Category = "Sprites", meta = (ClampMin = "0", ClampMax = "1", UIMin = "0", UIMax = "1"))
	double FadeIn = 0.0;

	/** Each particle starts between SizeMin and SizeMax, 0.1 to 1000 cm. */
	UPROPERTY(EditAnywhere, Category = "Sprites", meta = (ClampMin = "0.1", ClampMax = "1000", UIMin = "0.1", UIMax = "1000", Units = "cm"))
	double SizeMin = 25.0;

	UPROPERTY(EditAnywhere, Category = "Sprites", meta = (ClampMin = "0.1", ClampMax = "1000", UIMin = "0.1", UIMax = "1000", Units = "cm"))
	double SizeMax = 25.0;

	/** How large a particle is at the end of its life, as a multiple of its starting size, 0 to 10. */
	UPROPERTY(EditAnywhere, Category = "Sprites", meta = (ClampMin = "0", ClampMax = "10", UIMin = "0", UIMax = "10"))
	double SizeEndScale = 1.0;

	/** Each particle lives between LifetimeMin and LifetimeMax, 0.05 to 10 seconds. */
	UPROPERTY(EditAnywhere, Category = "Sprites", meta = (ClampMin = "0.05", ClampMax = "10", UIMin = "0.05", UIMax = "10", Units = "s"))
	double LifetimeMin = 2.0;

	UPROPERTY(EditAnywhere, Category = "Sprites", meta = (ClampMin = "0.05", ClampMax = "10", UIMin = "0.05", UIMax = "10", Units = "s"))
	double LifetimeMax = 2.0;

	/** Each particle starts between VelocityMin and VelocityMax on each axis, -5000 to 5000 cm/s, in the effect's own space. */
	UPROPERTY(EditAnywhere, Category = "Sprites", meta = (Units = "cm/s"))
	FVector VelocityMin = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, Category = "Sprites", meta = (Units = "cm/s"))
	FVector VelocityMax = FVector::ZeroVector;

	/** A constant acceleration, -5000 to 5000 cm/s^2 on each axis: gravity is negative Z, buoyancy positive. */
	UPROPERTY(EditAnywhere, Category = "Sprites")
	FVector Acceleration = FVector::ZeroVector;

	/** Where particles appear; the matching size below is read, the others kept for switching back. */
	UPROPERTY(EditAnywhere, Category = "Sprites")
	EMantleSpritesShape Shape = EMantleSpritesShape::Point;

	/** A box shape's half size on each axis, 0 to 10000 cm. */
	UPROPERTY(EditAnywhere, Category = "Sprites", meta = (Units = "cm"))
	FVector BoxHalfExtent = FVector(50.0, 50.0, 50.0);

	/** A sphere shape's radius, 0 to 10000 cm. */
	UPROPERTY(EditAnywhere, Category = "Sprites", meta = (ClampMin = "0", ClampMax = "10000", UIMin = "0", UIMax = "10000", Units = "cm"))
	double SphereRadius = 50.0;

	/** A cylinder shape's radius and full height (its axis +Z), 0 to 10000 cm. */
	UPROPERTY(EditAnywhere, Category = "Sprites", meta = (ClampMin = "0", ClampMax = "10000", UIMin = "0", UIMax = "10000", Units = "cm"))
	double CylinderRadius = 50.0;

	UPROPERTY(EditAnywhere, Category = "Sprites", meta = (ClampMin = "0", ClampMax = "10000", UIMin = "0", UIMax = "10000", Units = "cm"))
	double CylinderHeight = 100.0;

	/** The most particles alive at once, 1 to 2000; the rate is lowered where Rate times LifetimeMax would pass it. */
	UPROPERTY(EditAnywhere, Category = "Sprites", meta = (ClampMin = "1", ClampMax = "2000", UIMin = "1", UIMax = "2000"))
	int32 MaxParticles = 500;

	/**
	 * The texture is a flipbook sheet of this many columns and rows (1 to 16 each), and each particle shows one frame
	 * of it (SubImageMode) instead of the whole sheet. 1 by 1 is no sheet.
	 */
	UPROPERTY(EditAnywhere, Category = "Sprites", meta = (ClampMin = "1", ClampMax = "16", UIMin = "1", UIMax = "16"))
	int32 SubImageColumns = 1;

	UPROPERTY(EditAnywhere, Category = "Sprites", meta = (ClampMin = "1", ClampMax = "16", UIMin = "1", UIMax = "16"))
	int32 SubImageRows = 1;

	/** Which frame a particle shows: a random one for its life, or the frames in order over its life (left to right, top to bottom). */
	UPROPERTY(EditAnywhere, Category = "Sprites")
	EMantleSpritesSubImageMode SubImageMode = EMantleSpritesSubImageMode::Random;
};

/**
 * What an effect looks like: the five values every archetype's system reads as user parameters. The
 * ranges are FMantlePlaceLimits' (the place schema's); FMantleEffects::DefaultLook is each archetype's own.
 */
struct FMantleEffectLook
{
	/** Linear RGB, 0 to 1: User.Color. */
	FLinearColor Color = FLinearColor::White;
	/** Particle size, cm: User.Size2D, as (Size, Size). Fire takes it as the whole fire's size, 40 being its own. */
	double Size = 50.0;
	/** Particles spawned a second: User.Rate. */
	double Rate = 10.0;
	/** How fast particles leave the emitter, cm/s: User.Speed. */
	double Speed = 50.0;
	/** How long each particle lives, seconds: User.Lifetime. */
	double Lifetime = 2.0;
};

struct MANTLERUNTIME_API FMantleEffects
{
	/** Every archetype, the sprites one (#628) last. */
	static constexpr int32 NumArchetypes = 6;

	/** The archetypes with the five fixed looks above: the ones a script's Effects:Play can show (sprites needs its own keys). */
	static constexpr int32 NumLookArchetypes = 5;

	/** The folder the archetype systems are cooked from (DirectoriesToAlwaysCook). */
	static constexpr const TCHAR* Folder = TEXT("/Game/Mantle/Effects");

	/**
	 * The user parameters every archetype system exposes, each of the type its lightweight (stateless)
	 * emitter's module binds: Color a LinearColor (Initialize Particle: Color); Size2D a Vector2D
	 * (Initialize Particle: Sprite Size, a 2-value distribution), written (Size, Size); Rate, Speed and
	 * Lifetime floats (Spawn Rate, Add Velocity: Cone Velocity, Initialize Particle: Lifetime). The size's
	 * name carries its type so a system still holding an old float User.Size cannot pass for it.
	 */
	static constexpr const TCHAR* ColorParameter = TEXT("User.Color");
	static constexpr const TCHAR* SizeParameter = TEXT("User.Size2D");
	static constexpr const TCHAR* RateParameter = TEXT("User.Rate");
	static constexpr const TCHAR* SpeedParameter = TEXT("User.Speed");
	static constexpr const TCHAR* LifetimeParameter = TEXT("User.Lifetime");

	/**
	 * What the sprites system (/Game/Mantle/Effects/NS_MantleSprites, #628) reads, on top of User.Rate. Each is a
	 * user parameter the runtime writes and the system binds where it can; one the system does not bind is simply ignored. Floats unless noted.
	 * User.Material is the sprite material instance (a MaterialInterface) the system's renderer binds, which
	 * carries the texture, blend, colours, opacity and end scale; User.Size* and User.Lifetime* are the range a
	 * particle's size (cm) and life (s) is drawn from; User.Velocity* and User.Acceleration are Vector3s;
	 * User.BoxSize a Vector3 (the box's full size, twice its half extent), User.SphereRadius, User.CylinderRadius and User.CylinderHeight (full) floats
	 * for the three volume emitters. The emitters "Box" (which a point uses with a zero extent), "Sphere" and
	 * "Cylinder" are enabled one at a time through the component's SetEmitterEnable.
	 */
	static constexpr const TCHAR* MaterialParameter = TEXT("User.Material");
	static constexpr const TCHAR* SizeMinParameter = TEXT("User.SizeMin");
	static constexpr const TCHAR* SizeMaxParameter = TEXT("User.SizeMax");
	static constexpr const TCHAR* LifetimeMinParameter = TEXT("User.LifetimeMin");
	static constexpr const TCHAR* LifetimeMaxParameter = TEXT("User.LifetimeMax");
	static constexpr const TCHAR* VelocityMinParameter = TEXT("User.VelocityMin");
	static constexpr const TCHAR* VelocityMaxParameter = TEXT("User.VelocityMax");
	static constexpr const TCHAR* AccelerationParameter = TEXT("User.Acceleration");
	static constexpr const TCHAR* BoxSizeParameter = TEXT("User.BoxSize");
	static constexpr const TCHAR* SphereRadiusParameter = TEXT("User.SphereRadius");
	static constexpr const TCHAR* CylinderRadiusParameter = TEXT("User.CylinderRadius");
	static constexpr const TCHAR* CylinderHeightParameter = TEXT("User.CylinderHeight");
	static constexpr const TCHAR* BoxEmitterName = TEXT("Box");
	static constexpr const TCHAR* SphereEmitterName = TEXT("Sphere");
	static constexpr const TCHAR* CylinderEmitterName = TEXT("Cylinder");

	/**
	 * The sprite master materials in Folder: M_MantleSprite_Translucent and M_MantleSprite_Additive. Their
	 * parameters: texture SpriteTexture; scalars UseTexture (1 with a texture, else a soft round dot), Alpha,
	 * AlphaEnd, FadeIn and SizeEndScale; vectors Color and ColorEnd (RGB). The material reads each particle's
	 * normalized age, so colour, opacity and size follow the particle's life without Niagara curves. A flipbook sheet
	 * (#628): the vector SubImages (columns, rows) and the scalar SubImageOverLife (1 plays the frames across a
	 * particle's life by its normalized age, 0 shows one frame picked by its Particle Random, which the system's
	 * Initialize Particle writes); the material picks the frame, so no Niagara SubUV setup is involved.
	 */
	static constexpr const TCHAR* TextureParameter = TEXT("SpriteTexture");
	static constexpr const TCHAR* UseTextureParameter = TEXT("UseTexture");
	static constexpr const TCHAR* AlphaParameter = TEXT("Alpha");
	static constexpr const TCHAR* AlphaEndParameter = TEXT("AlphaEnd");
	static constexpr const TCHAR* FadeInParameter = TEXT("FadeIn");
	static constexpr const TCHAR* SizeEndScaleParameter = TEXT("SizeEndScale");
	static constexpr const TCHAR* ColorStartMaterialParameter = TEXT("Color");
	static constexpr const TCHAR* ColorEndMaterialParameter = TEXT("ColorEnd");
	static constexpr const TCHAR* SubImagesParameter = TEXT("SubImages");
	static constexpr const TCHAR* SubImageOverLifeParameter = TEXT("SubImageOverLife");

	/** How long a non-looping effect emits each time it starts, seconds; its particles then die out. */
	static constexpr double OneShotEmitSeconds = 1.0;

	/** "smoke", "fire", "sparks", "dust", "glow" or "sprites": the place schema's spelling. */
	static const TCHAR* ArchetypeName(EMantleEffectArchetype Archetype);

	/** "Smoke", "Fire"...: how scripts and the Details panel name it. */
	static const TCHAR* ArchetypeDisplayName(EMantleEffectArchetype Archetype);

	/** The place file's archetype: case-exact, as every enum in the place schema. */
	static bool ParseArchetype(const FString& Name, EMantleEffectArchetype& Out);

	/** A script's archetype: any case ("Fire", "fire", "FIRE"). Only the first NumLookArchetypes: a script cannot play sprites. */
	static bool ParseArchetypeAnyCase(const FString& Name, EMantleEffectArchetype& Out);

	/** Every archetype's schema name, joined for an error message: "smoke, fire, sparks, dust, glow or sprites". */
	static FString ArchetypeList();

	/** The schema's blend and shape spellings: "translucent" and "additive"; "point", "box", "sphere" and "cylinder". */
	static const TCHAR* BlendName(EMantleSpritesBlend Blend);
	static bool ParseBlend(const FString& Name, EMantleSpritesBlend& Out);
	static const TCHAR* ShapeName(EMantleSpritesShape Shape);
	static bool ParseShape(const FString& Name, EMantleSpritesShape& Out);

	/** The schema's subImageMode spellings: "random" and "overLife". */
	static const TCHAR* SubImageModeName(EMantleSpritesSubImageMode Mode);
	static bool ParseSubImageMode(const FString& Name, EMantleSpritesSubImageMode& Out);

	/**
	 * Settings held to the schema's ranges and pairs, as a system is given them: every number clamped (one that is
	 * not finite takes the default's), the max of each pair raised to its min (per axis for velocity), the colour's
	 * channels held to 0..1.
	 */
	static FMantleSpritesSettings ClampSprites(const FMantleSpritesSettings& In);

	/**
	 * Resolves a min and max pair a place file may have named only one of: a lone min gives its value to the max
	 * and a lone max to the min; neither keeps both (the defaults).
	 */
	static void ResolveSpritesPair(bool bHasMin, bool bHasMax, double& Min, double& Max);
	static void ResolveSpritesVectorPair(bool bHasMin, bool bHasMax, FVector& Min, FVector& Max);

	/** The rate a sprites system is given: Rate, lowered so that Rate times LifetimeMax stays within MaxParticles. */
	static double SpritesEffectiveRate(double Rate, const FMantleSpritesSettings& Settings);

	/** The sprite master material's object path for Blend, "/Game/Mantle/Effects/M_MantleSprite_Additive.M_MantleSprite_Additive". */
	static FString SpritesMaterialPath(EMantleSpritesBlend Blend);

	/** The archetype's own look: what an Effect whose place file leaves a key out shows. */
	static FMantleEffectLook DefaultLook(EMantleEffectArchetype Archetype);

	/** In's values held to the schema's ranges; a value that is not a finite number takes the archetype's own. */
	static FMantleEffectLook ClampLook(const FMantleEffectLook& In, EMantleEffectArchetype Archetype);

	/** The archetype system's object path, "/Game/Mantle/Effects/NS_MantleFire.NS_MantleFire". */
	static FString SystemPath(EMantleEffectArchetype Archetype);

	/**
	 * The archetype's cooked system, or null when this build lacks it (logged once per archetype per
	 * process: every effect of that archetype then draws nothing). Loads synchronously the first time.
	 */
	static UFXSystemAsset* LoadSystem(EMantleEffectArchetype Archetype);

	/**
	 * Starts loading every archetype's system in the background, so the first effect of each does not pay a synchronous load
	 * (and, in the editor, a compile) inside whatever called it: a script's call is charged that time against its CPU budget.
	 * Safe to call more than once; systems already loaded or missing are skipped.
	 */
	static void WarmSystems();

	/**
	 * Niagara's component class (/Script/Niagara.NiagaraComponent), found by name so MantleCore links no
	 * plugin. Null, logged once, where the Niagara plugin is not loaded.
	 */
	static UClass* ComponentClass();
};
