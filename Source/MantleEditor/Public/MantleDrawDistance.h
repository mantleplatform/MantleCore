// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantlePlaceTypes.h"

class UStaticMesh;
class UStaticMeshComponent;
class UWorld;
struct FMantlePlace;
struct FMantleTargetFeatures;

/**
 * What the exporter knows about a mesh component that decides its draw and shadow distances (GhzGarage/mantle#630 stage 2),
 * read from the component so the rule itself (FMantleDrawDistance::Apply) is plain numbers.
 */
struct MANTLEEDITOR_API FMantleDrawDistanceSource
{
	/** The mesh's bounding sphere radius in cm at the scale it is placed: for instances, at the largest instance's scale. */
	double SphereRadius = 0.0;

	/** The component's Desired Max Draw Distance and Current Max Draw Distance (cull distance volumes), 0 for none. */
	double LDMaxDrawDistance = 0.0;
	double CachedMaxDrawDistance = 0.0;

	/** The component's Never Distance Cull. */
	bool bNeverDistanceCull = false;

	/** An instanced or foliage component: its Instance End Cull Distance (painted foliage gets it from its foliage type), 0 for none. */
	bool bInstanced = false;
	int32 InstanceEndCullDistance = 0;

	/**
	 * The draw distance of the structure the component is part of (FMantleDrawDistance::PlanStructures), 0 for none: it replaces the
	 * size band, never a cull distance the component sets itself.
	 */
	double StructureDistance = 0.0;
};

/**
 * One piece the structure rule looks at (FMantleDrawDistance::PlanStructures): the world box of a mesh component, or of one of an
 * instanced component's instances, and the actor it belongs to.
 */
struct MANTLEEDITOR_API FMantleStructurePiece
{
	/** The owner: an index into the owners' bands. */
	int32 Owner = INDEX_NONE;
	FBox Bounds = FBox(ForceInit);
};

/**
 * The draw distance a mesh entity or instance set publishes (drawDistance, cm; the drawDistance client feature). The client fades
 * an instance set over the last fifth of it and ends shadows by its own rule, so the editor writes nothing else.
 *
 * The component's own cull distance wins: LDMaxDrawDistance (else CachedMaxDrawDistance) for a mesh, InstanceEndCullDistance
 * for instances and painted foliage. Without one, a size-based default by
 * the mesh's bounding sphere radius (Bands): small clutter culls near, and a mesh of NeverCullRadius or more is never culled.
 * The bands are set so nothing visibly pops at normal play distances: a mesh is still about a dozen pixels across where it culls.
 * A component with Never Distance Cull publishes no draw distance.
 *
 * 0 is never written (the schema refuses it): a mesh that is never culled has no key.
 */
struct MANTLEEDITOR_API FMantleDrawDistance
{
	/** One size band: meshes with a bounding sphere radius below MaxRadius (cm) cull at Draw (cm). */
	struct FBand
	{
		double MaxRadius;
		double Draw;
	};

	/** The size-based defaults, smallest first. A mesh past the last band's MaxRadius gets none. */
	static TConstArrayView<FBand> Bands();

	/** The radius at and above which a mesh gets no default draw distance: architecture and other large pieces. */
	static double NeverCullRadius();

	/** Sets Source's draw distance on Props (0 for none), or 0 when the target client does not read drawDistance. True when one was set. */
	static bool Apply(const FMantleDrawDistanceSource& Source, const FMantleTargetFeatures& Target, FMantleStaticMeshProps& Props);

	/** The source numbers of a component (an instanced one included); Mesh is the mesh to measure (the component's own when null). */
	static FMantleDrawDistanceSource ReadComponent(const UStaticMeshComponent* Component, const UStaticMesh* Mesh = nullptr);

	/**
	 * A structure draws to one distance, so no part of it hangs in the air where another part has culled (City Sample: walls at 250 m
	 * under 600 m walls and 2.5 km roofs, freeway decks at 2.5 km on 250 m supports). A structure is an owner (the outermost actor of
	 * its pieces) and every owner that holds it up: a piece of one reaching up to the underside of a raised piece of the other
	 * (StructureLiftCm or more above the lower piece's bottom, within StructureReachCm of its top) where their footprints overlap.
	 * Ground holds nothing up (it never reaches a raised piece's underside), so roads keep their own distance.
	 *
	 * OwnerMinBand and OwnerMaxBand are the smallest and largest size-band distances among an owner's pieces that take one (0: the
	 * owner takes no part: every piece sets its own cull distance or is never culled). Links are the pieces that may hold another up
	 * or be held: the ones of at least LinkMinRadius (cm), so small props never join two structures. Out gets each owner's distance:
	 * a structure whose pieces all share one band keeps it; one that mixes bands draws to its largest, at most StructureCapCm, past
	 * which the client's far sets draw its twin whole.
	 */
	static void PlanStructures(TConstArrayView<double> OwnerMinBand, TConstArrayView<double> OwnerMaxBand, TConstArrayView<FMantleStructurePiece> Links,
		TArray<double>& OutOwnerDistance);

	/** A mixed structure's distance cap, cm: 600 m (Joshua, 2026-11-22: "B: 600 m + far"). */
	static constexpr double StructureCapCm = 60000.0;
	/** A piece is raised when its bottom is at least this far above a piece that reaches it. */
	static constexpr double StructureLiftCm = 300.0;
	/** A piece holds another up when its top is at most this far under the other's bottom. */
	static constexpr double StructureReachCm = 50.0;
	/** Pieces smaller than this radius never link structures (the 250 m band and up). */
	static constexpr double LinkMinRadius = 100.0;

	/**
	 * The structure distance of every mesh component in World (PlanStructures on its actors), for ReadComponent while an FStructureScope
	 * is alive. Components that set their own distance, are never culled, or are not shown in game are left out.
	 */
	static void BuildStructureDistances(const UWorld* World, TMap<const UStaticMeshComponent*, double>& Out);

	/** While alive, ReadComponent gives each component of World its structure distance (FMantlePlaceExporter::Export holds one). */
	class MANTLEEDITOR_API FStructureScope
	{
	public:
		explicit FStructureScope(const UWorld* World);
		~FStructureScope();
		FStructureScope(const FStructureScope&) = delete;
		FStructureScope& operator=(const FStructureScope&) = delete;

	private:
		TMap<const UStaticMeshComponent*, double> Distances;
		const TMap<const UStaticMeshComponent*, double>* Previous = nullptr;
	};

	/** The StaticMesh and InstancedMesh entities of Place with a drawDistance. */
	static int32 CountEntities(const FMantlePlace& Place);

	/** True when Place has a StaticMesh or InstancedMesh entity, whatever its distances. */
	static bool HasMeshEntities(const FMantlePlace& Place);
};
