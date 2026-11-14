// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantlePlaceTypes.h"

struct FMantleInstancedMeshGroup;

/** What FMantlePlaceInstancing::GroupStaticMeshes did (GhzGarage/mantle#818). */
struct FMantleGroupReport
{
	/** Static mesh actors that became instances of a set. */
	int32 Actors = 0;

	/** Distinct sets (a mesh with one material set, collision and shadow) those actors went into. */
	int32 Sets = 0;
};

/**
 * Making a place fit (GhzGarage/mantle#818): the pure steps on a built FMantlePlace that FMantlePlaceExporter::Export runs, so
 * they are tested without a level.
 *
 * - GroupStaticMeshes folds repeated identical static mesh actors of the root into instance sets (the scenery in cells is folded by
 *   FMantleCellPlanner, per cell).
 * - EvenKeepMask picks items evenly: the code far sets (stage 3) will thin by. Nothing is thinned to fit a place any more.
 *
 * Deterministic: the same place gives the same result, whatever the order Unreal iterates actors in.
 */
class MANTLEEDITOR_API FMantlePlaceInstancing
{
public:
	/** A mesh needs this many identical actors before they are folded into a set; fewer stay separate, named objects. */
	static constexpr int32 MinGroupSize = 4;

	/** The instances across all InstancedMesh entities of Place. */
	static int64 CountInstances(const FMantlePlace& Place);

	/**
	 * Moves the plain StaticMesh entities of Place into Groups (merged by FMantlePlaceExporter::InstancedGroupKey with the groups
	 * already there), so ChunkInstancedMeshes cuts them into InstancedMesh entities with the rest. An entity is plain when nothing
	 * else could tell it from its twins: static, no parent and no children, no tags, no physics block, no interactable, and not
	 * named by a constraint, a crowd spawner's point of interest or an interactable part. A mesh with fewer than MinGroupSize plain
	 * twins stays as it is. Instances keep their world transform (written to the precision the place file keeps instances to:
	 * 0.01 cm, 0.001 degrees, 0.0001 scale). The entities leave Place.Entities; instances join Groups in entity order.
	 */
	static FMantleGroupReport GroupStaticMeshes(FMantlePlace& Place, TArray<FMantleInstancedMeshGroup>& Groups);

	/**
	 * Keeps Keep of Count items, spread evenly along the order they are in: item Index is kept when
	 * floor((Index + 1) * Keep / Count) is more than floor(Index * Keep / Count). Exactly Keep are kept, the same ones every time.
	 * Instances come out of the exporter in Morton order, so an even pick along that order is an even pick across space.
	 */
	static TArray<bool> EvenKeepMask(int64 Count, int64 Keep);

	/** "11.2 MiB". */
	static FString DescribeBytes(int64 Bytes);

	/**
	 * The refusal for a root place file over a limit, phrased for the creator: which limit, by how much, and what moves scenery out of
	 * it (a draw distance puts it in cells). Empty when the root is inside both.
	 */
	static FString DescribeOverBudget(int64 Instances, int64 InstanceLimit, int64 Bytes, int64 ByteLimit);
};
