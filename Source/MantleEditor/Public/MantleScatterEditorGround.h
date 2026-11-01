// Copyright Joshua Eger. All Rights Reserved.
// mantleplatform/mantle#169: the ground a Mantle Scatter Volume placed on terrain drops onto in the editor. A
// player's client drops it onto the Terrains it built from the place; in the level being edited the ground
// is the level's landscapes, so this reads each one with the same code that publishes it
// (FMantleTerrainExport) and hands the volumes the very heightfields and holes players get. The editor
// preview then lands every instance where it lands for players, and its hash is theirs.
#pragma once

#include "CoreMinimal.h"
#include "MantleScatterVolume.h"

struct FMantleTerrainSource;
class UWorld;

class MANTLEEDITOR_API FMantleScatterEditorGround
{
public:
	/** Watches the editor's level: terrain volumes get the ground when they appear, and again when a landscape changes. */
	static void Startup();
	static void Shutdown();

	/**
	 * A transform as a place file carries it and a client reads it back: its rotation through the Rotator the
	 * exporter writes (WriteTransform). What the client's TerrainToWorld is for the same landscape.
	 */
	static FTransform AsPublished(const FTransform& Transform);

	/**
	 * One landscape's surface as a client builds it from the published Terrain: the heightfield MakeHeightfield
	 * uploads (through the MHF1 file the client reads), the holes from the hole mask the converter uploads, and
	 * the Terrain entity's transform. Field is empty when the source cannot make a heightfield.
	 */
	static FMantleScatterTerrainSurface SurfaceFor(const FMantleTerrainSource& Source);

	/** Every landscape of World that publishes as a Terrain (FMantleTerrainExport::TerrainLandscapes), as SurfaceFor makes it. */
	static TArray<FMantleScatterTerrainSurface> SurfacesFor(UWorld* World);

	/** Gives every terrain-placed scatter volume in World the ground (SurfacesFor), which rebuilds each one's preview. */
	static void FeedVolumes(UWorld* World);
};
