// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantlePlaceTypes.h"

class AActor;
class UMaterialInterface;
class UWorld;

/**
 * The cooked decal masters a Decal entity draws with (#472), made by
 * tools/materials/Create-DecalMaterial.py under /Game/Mantle/Materials and cooked into every client
 * with the rest of that folder. Both are deferred decals, translucent: M_MantleDecal projects
 * BaseColor (its alpha the coverage) times Opacity; M_MantleDecal_Normal projects Normal as well.
 * Two masters rather than a switch, because a decal material that writes a normal at all replaces
 * the surface's own normal under it. The exporter recognises a creator's instance of either by
 * these paths.
 */
struct MANTLERUNTIME_API FMantleDecalMaterials
{
	static constexpr const TCHAR* ColorPath = TEXT("/Game/Mantle/Materials/M_MantleDecal.M_MantleDecal");
	static constexpr const TCHAR* NormalPath = TEXT("/Game/Mantle/Materials/M_MantleDecal_Normal.M_MantleDecal_Normal");

	/** The masters' parameters. */
	static constexpr const TCHAR* BaseColorParameter = TEXT("BaseColor");
	static constexpr const TCHAR* NormalParameter = TEXT("Normal");
	static constexpr const TCHAR* OpacityParameter = TEXT("Opacity");

	/**
	 * The master for a decal with (bNormal) or without a normal map. Null when this build lacks it,
	 * logged once per process: the decal is then built without a material and draws nothing.
	 */
	static UMaterialInterface* Load(bool bNormal);
};

struct FMantleBuildResult
{
	bool bSuccess = false;

	/** Spawned actors keyed by entity id. */
	TMap<FString, AActor*> Actors;

	/**
	 * Environment actors spawned by FMantleEnvironment (the default sky cluster, and a
	 * synthesized sun when the place has none). Not keyed by entity id — they are not place
	 * entities — but torn down alongside Actors by every FMantlePlaceBuilder::Teardown caller.
	 */
	TArray<AActor*> EnvironmentActors;

	/** Non-fatal problems — an entity that could not be built is reported, not skipped silently. */
	TArray<FMantleParseError> Errors;

	FString ErrorSummary() const
	{
		TArray<FString> Lines;
		for (const FMantleParseError& Error : Errors)
		{
			Lines.Add(Error.ToString());
		}
		return FString::Join(Lines, TEXT("\n"));
	}
};

struct FMantleBuildOptions
{
	/**
	 * Transient actors are not saved with a level.
	 *
	 * True for the client, which rebuilds its world from data on every join and has nothing
	 * to save into. False when importing into a level a creator will edit and save, where
	 * transient actors would silently disappear.
	 */
	bool bTransient = true;

	/**
	 * Gives the build the runtime default environment (FMantleEnvironment::Apply): the sky
	 * cluster, the world's clock, and the day/night cycle that takes over the place's sun.
	 *
	 * True wherever the place is played or previewed: the server, the client rebuild and the
	 * editor preview. False when importing into a level a creator will edit and save: the sky
	 * belongs to players, and the day/night cycle would re-aim the place's own DirectionalLight
	 * away from its authored rotation, so the next export would publish the hour of the import
	 * instead of what the place file says. The world type cannot tell these builds apart (the
	 * preview and the importer's tests both build into Game worlds), so the caller says so.
	 */
	bool bRuntimeEnvironment = true;
};

/**
 * Turns a parsed place into live actors.
 *
 * This is the half of the architecture that has to work: if a running client cannot
 * faithfully rebuild a place from data alone, the entire data-driven approach fails and
 * per-place cooking comes back.
 *
 * Both the client and the plugin's preview use this same code, so what a creator sees
 * before publishing is what players receive.
 */
class MANTLERUNTIME_API FMantlePlaceBuilder
{
public:
	/** Spawns every entity in Place into World. */
	static FMantleBuildResult Build(
		UWorld* World,
		const FMantlePlace& Place,
		const FMantleBuildOptions& Options = FMantleBuildOptions());

	/** Destroys actors previously produced by Build. */
	static void Teardown(FMantleBuildResult& Result);
};
