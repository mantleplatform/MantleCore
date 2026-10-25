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
 * What FMantlePlaceInstancing::ThinToFit did, or, for a preview, would do (GhzGarage/mantle#818). Counts are instances of
 * InstancedMesh entities; Bytes are the place file's UTF-8 size.
 */
struct FMantleThinReport
{
	/** True when the instances were removed from the place; false for a preview of what thinning would do. */
	bool bApplied = false;

	/** Whether the place fits both limits (the instance count and the file size) after thinning; always true when nothing was over. */
	bool bFits = true;

	int64 InstancesBefore = 0;
	int64 InstancesAfter = 0;

	/** The instances thinned first (painted foliage), before and after. */
	int64 FoliageBefore = 0;
	int64 FoliageAfter = 0;

	int64 BytesBefore = 0;
	int64 BytesAfter = 0;

	int64 Removed() const { return InstancesBefore - InstancesAfter; }
	bool RemovesAnything() const { return InstancesAfter < InstancesBefore; }

	/** Foliage instances removed, and the others (props and PCG output) removed after the foliage ran out. */
	int64 FoliageRemoved() const { return FoliageBefore - FoliageAfter; }
	int64 OthersRemoved() const { return Removed() - FoliageRemoved(); }

	/** A sentence a creator can read: how many of how many are kept, what goes first, and the file's size before and after. */
	FString Describe() const;
};

/**
 * Making a big place fit (GhzGarage/mantle#818): the pure steps on a built FMantlePlace that FMantlePlaceExporter::Export runs, so
 * they are tested without a level.
 *
 * - GroupStaticMeshes folds repeated identical static mesh actors into instance sets.
 * - ThinToFit removes instances evenly until the place is inside the instance limit and the file inside the backend's size limit.
 *
 * Both are deterministic: the same place gives the same result, whatever the order Unreal iterates actors in.
 */
class MANTLEEDITOR_API FMantlePlaceInstancing
{
public:
	/** A mesh needs this many identical actors before they are folded into a set; fewer stay separate, named objects. */
	static constexpr int32 MinGroupSize = 4;

	/** Thinning aims below the file limit by this fraction, so a late id of a different length cannot tip it over. */
	static constexpr double SizeHeadroom = 0.97;

	/** The most of the thin-first instances (foliage) thinning removes; the rest is taken from the other instances. */
	static constexpr double MaxFoliageThinFraction = 0.9;

	/** The place file's size in bytes: the UTF-8 length of FMantlePlaceExporter::ToJson. */
	static int64 PlaceBytes(const FMantlePlace& Place);

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

	/**
	 * Removes instances from Place's InstancedMesh entities, evenly, until there are at most MaxInstances and the place file is at
	 * most MaxBytes (aiming SizeHeadroom below that). The entities named by ThinFirstIds (painted foliage) lose instances first, up
	 * to MaxFoliageThinFraction of theirs; if that is not enough, the other instances give the rest. Each tier is thinned along one
	 * order across all its entities, so exactly the right number go and every area keeps a share.
	 *
	 * bApply false leaves Place as it was and only reports. An entity left with no instance is removed when applying.
	 * The report's bFits is false when even the most thinning allowed leaves it over a limit.
	 */
	static FMantleThinReport ThinToFit(FMantlePlace& Place, const TSet<FString>& ThinFirstIds, int64 MaxInstances, int64 MaxBytes, bool bApply);

	/** "11.2 MiB". */
	static FString DescribeBytes(int64 Bytes);

	/**
	 * The refusal for a place over a limit, phrased for the creator: which limit, by how much, the fix Publish offers (Thin, with
	 * its numbers) when Thin removes anything, and what else to do. Empty when the place is inside both.
	 */
	static FString DescribeOverBudget(int64 Instances, int64 InstanceLimit, int64 Bytes, int64 ByteLimit, const FMantleThinReport& Thin);
};
