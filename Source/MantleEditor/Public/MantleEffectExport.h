// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FMantleCoverage;
struct FMantleEffectProps;
class UDistributionFloat;
class UDistributionVector;
class UFXSystemComponent;

/**
 * Translates a project's own particle systems onto Mantle's sprites archetype (#628, S-FX).
 *
 * Mantle ships one general sprite emitter (/Game/Mantle/Effects/NS_MantleSprites, FMantleSpritesSettings) and a
 * creator's particles are translated onto it at export: bake, don't translate at play. A Cascade UParticleSystem
 * is read emitter by emitter at LOD 0, each module that maps onto a sprites key is read, and anything else is
 * said on the coverage report. A Niagara system is reported as dropped for now.
 */
struct MANTLEEDITOR_API FMantleEffectExport
{
	/**
	 * One place Effect entity's "properties" object per emitter the component's system can carry (a Cascade
	 * ParticleSystem; a Niagara system is reported and skipped for now). Returns false when nothing is carried;
	 * every loss is reported on Coverage under the label given.
	 *
	 * Each object has archetype "sprites". Its "texture", where the emitter's material has one, is the texture's
	 * object path ("/Game/FX/T_Bubble.T_Bubble"), NOT an asset id: the exporter turns it into the uploaded
	 * texture's id the way it does a decal's (FMantlePlaceDependencies::AddEffectTexture gathers it, and
	 * TextureForPath finds it again). FMantleActorDecompose::EffectPropsFromJson keeps it in FMantleEffectProps::TextureId.
	 * When the target client does not read the sprites archetype (FMantleTargetFeatures::Current), nothing is
	 * carried and Coverage says "needs a newer Mantle client".
	 */
	static bool Translate(const UFXSystemComponent* Component, const FString& Label, FMantleCoverage& Coverage,
		TArray<TSharedPtr<FJsonObject>>& OutEffectProperties);

	/** A "texture" value Translate wrote: an object path rather than an uploaded asset id. */
	static bool IsTexturePath(const FString& Value) { return Value.StartsWith(TEXT("/")); }

	/** The texture an object path names (loaded), or null. */
	static class UTexture* TextureForPath(const FString& Path);

	/**
	 * Reads one Effect "properties" object (as Translate returns it, or a place file carries it) into Out: the base
	 * keys of every archetype and, for sprites, every sprites key, a key left out taking its default and a lone min
	 * or max the other's value, all held to the schema's limits. "texture" is kept as written (an object path from
	 * Translate, or an asset id) in Out.TextureId. False when the archetype is missing or unknown.
	 */
	static bool ReadProperties(const FJsonObject& Properties, FMantleEffectProps& Out);

	/**
	 * A Cascade float distribution as the smallest and largest value it takes: a constant, a uniform range, or
	 * (approximated) a curve read at its first and last points. False for a distribution that is not one of those
	 * (null, a parameter, a vector's component...). bOutApproximated is set when a curve stood in for a range.
	 */
	static bool ReadFloat(const UDistributionFloat* Distribution, double& OutMin, double& OutMax, bool& bOutApproximated);

	/** As ReadFloat for a vector distribution: a constant, a uniform range per axis, or (approximated) a curve's first and last points. */
	static bool ReadVector(const UDistributionVector* Distribution, FVector& OutMin, FVector& OutMax, bool& bOutApproximated);
};
