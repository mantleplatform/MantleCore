// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantlePlaceTypes.h"

class AActor;
class UMaterialInterface;
class UWorld;

/**
 * The cooked decal masters a Decal entity draws with, kept
 * under /Game/Mantle/Materials and cooked into every client
 * with the rest of that folder. Both are deferred decals, translucent: M_MantleDecal projects
 * BaseColor (its alpha the coverage) times Opacity; M_MantleDecal_Normal projects Normal as well.
 * Two masters rather than a switch, because a decal material that writes a normal at all replaces
 * the surface's own normal under it. The exporter recognizes a creator's instance of either by
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
	 * The entity ids whose actor FMantleBuildOptions::OnActorBuilt has been called for (as each stood, spread over the build's frames): what a caller does once for every actor does not
	 * need doing again, in one frame, when the build ends (UMantleLoadedPlaceSubsystem::Retain skips these).
	 */
	TSet<FString> ActorsHandledWhileBuilding;

	/**
	 * Environment actors spawned by FMantleEnvironment (the default sky cluster, and a
	 * synthesized sun when the place has none). Not keyed by entity id — they are not place
	 * entities — but torn down alongside Actors by every FMantlePlaceBuilder::Teardown caller.
	 */
	TArray<AActor*> EnvironmentActors;

	/** Non-fatal problems — an entity that could not be built is reported, not skipped silently. */
	TArray<FMantleParseError> Errors;

	/**
	 * Where the game-thread time of a StaticMesh or InstancedMesh went, seconds, summed over the build (the cell streamer names them when one piece is slow):
	 * in asset-library loads, in spawning the actor, and in applying the mesh props (collision, materials: the physics body is made there).
	 */
	double LoadSeconds = 0.0;
	double SpawnSeconds = 0.0;
	double PropsSeconds = 0.0;

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

	/**
	 * The build adds a few entities to a place that already stands (the cell streamer builds a cell's scenery this way, one
	 * slice at a time): it does none of the place-wide setup (world gravity, environment, reflection probe notice) and logs
	 * none of the per-build lines, which would otherwise be written and repeated for every slice. The entities must have no
	 * parent and nothing that names them (the cell format guarantees both).
	 */
	bool bCellSlice = false;

	/**
	 * Called for each entity's actor as soon as it stands (right after it is added to the result), with its entity id. A caller's own per-actor work goes here, so a big place pays it a few
	 * entities a frame with the build instead of all at once in the frame the build ends (7,000 actors named for the network took 77 ms in one tick). The actor is not parented or placed yet:
	 * only work that does not depend on that (a name) belongs here. Not called for a cell slice's actors.
	 */
	TFunction<void(AActor* Actor, const FString& EntityId)> OnActorBuilt;
};

/**
 * A build of one place, in steps. The passes of FMantlePlaceBuilder::Build are the same, but each runs a
 * unit (one entity) at a time, so a step can stop between any two entities and the build can span frames:
 * a big place no longer costs one long frame. Begin one with FMantlePlaceBuilder::Begin and call Step until
 * it says it is finished (FMantlePlaceBuildRunner does that from the engine's ticker).
 *
 * Until it is finished the actors in the result are only partly set up (no parent, transform or mobility yet),
 * so the result is not to be read, retained or shown to scripts before then.
 */
class MANTLERUNTIME_API FMantlePlaceBuildSession
{
public:
	/** A step budget that means "all of it in this call": the one-frame build. */
	static constexpr double NoBudget = 1.0e10;

	~FMantlePlaceBuildSession();

	/**
	 * Does work for about MaxSeconds of game-thread time, and at least one unit of it (so a build always advances),
	 * stopping after the unit that crosses the budget. A single entity is never split, and what one costs is what
	 * the step overshoots by. Returns true once the build is finished.
	 */
	bool Step(double MaxSeconds);

	bool IsFinished() const;

	/** The result so far; final once Step has returned true. Moved out by the owner when the build is finished. */
	FMantleBuildResult& GetResult();

	/** How many entities have not been spawned yet (zero once the spawn pass is over). */
	int32 EntitiesLeftToSpawn() const;

private:
	friend class FMantlePlaceBuilder;
	FMantlePlaceBuildSession();

	struct FState;
	TUniquePtr<FState> State;
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
	/** Spawns every entity in Place into World, all in this call. */
	static FMantleBuildResult Build(
		UWorld* World,
		const FMantlePlace& Place,
		const FMantleBuildOptions& Options = FMantleBuildOptions());

	/**
	 * The same build as Build, in steps (see FMantlePlaceBuildSession). The place must outlive the session:
	 * the first overload only points at it, the second keeps it alive.
	 */
	static TSharedRef<FMantlePlaceBuildSession> Begin(
		UWorld* World,
		const FMantlePlace& Place,
		const FMantleBuildOptions& Options = FMantleBuildOptions());
	static TSharedRef<FMantlePlaceBuildSession> Begin(
		UWorld* World,
		TSharedRef<const FMantlePlace> Place,
		const FMantleBuildOptions& Options = FMantleBuildOptions());

	/** Destroys actors previously produced by Build. */
	static void Teardown(FMantleBuildResult& Result);

	/**
	 * Whether a machine of this net mode draws a Terrain as the far ground plus full chunks near the view (the terrain stream, stage 3 and 4 of large places): every machine that draws, for
	 * every place, with cells or without: a client, a standalone game and a listen server. A dedicated server draws nothing and builds no chunk. The ground's collision is a height field on
	 * every machine, whatever is drawn.
	 */
	static bool DrawsTerrainAsFarGround(ENetMode NetMode);

	/** The same for a world: false for a world that is not a game world (the editor's importer and preview build every chunk in full: they have no stream to build them near a view), else by its net mode. */
	static bool DrawsTerrainAsFarGround(const UWorld* World);
};
