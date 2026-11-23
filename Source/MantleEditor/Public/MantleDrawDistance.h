// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantlePlaceLimits.h"
#include "MantlePlaceTypes.h"

class UStaticMesh;
class UStaticMeshComponent;
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

	/**
	 * How far the mesh's bounding box reaches sideways, in cm, at the same scale: the half-diagonal of its footprint (x and y), whatever its height. 0 when it was not
	 * measured; then the sphere radius is the footprint's too. A tower's roof is 60 m across and 400 m tall: its sphere is 218 m, its footprint 64 m.
	 */
	double FootprintRadius = 0.0;

	/** The component's Desired Max Draw Distance and Current Max Draw Distance (cull distance volumes), 0 for none. */
	double LDMaxDrawDistance = 0.0;
	double CachedMaxDrawDistance = 0.0;

	/** The component's Never Distance Cull. */
	bool bNeverDistanceCull = false;

	/** An instanced or foliage component: its Instance End Cull Distance (painted foliage gets it from its foliage type), 0 for none. */
	bool bInstanced = false;
	int32 InstanceEndCullDistance = 0;
};

/**
 * The draw distance a mesh entity or instance set publishes (drawDistance, cm; the drawDistance client feature). The client fades
 * an instance set over the last fifth of it and ends shadows by its own rule, so the editor writes nothing else.
 *
 * The component's own cull distance wins: LDMaxDrawDistance (else CachedMaxDrawDistance) for a mesh, InstanceEndCullDistance
 * for instances and painted foliage. Without one, by the mesh's bounding sphere radius: small clutter (Bands, under NoCutOffRadius)
 * culls near; a bigger mesh gets NoCutOff, so it draws as long as its cell stands (the cell decides, never the piece); and a mesh whose
 * footprint reaches NeverCullRadius or more is never culled (ground tiles and freeway decks: wide). Height does not count: a tower's
 * roof or a mast is tall and narrow, one building's piece, and goes to its cell with the rest of the building. A component with Never
 * Distance Cull publishes no draw distance.
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

	/** The size-based defaults, smallest first: small clutter only. A mesh past the last band's MaxRadius gets NoCutOff. */
	static TConstArrayView<FBand> Bands();

	/**
	 * The draw distance of a mesh with no cut-off of its own: the schema's largest (10 km), which a creator's own distance never
	 * publishes as (one that far is no cull at all). The cell planner reads it as "the cell decides": such a mesh goes to the grid's
	 * NoCutOffLevel and its cell loads out to that level's MaxDrawDistance (FMantleCellGrid). The game cuts nothing in reach of it,
	 * whatever the view distance setting (0.4 on Low still leaves 4 km).
	 */
	static constexpr double NoCutOff = FMantlePlaceLimits::MaxDrawDistance;

	/** The radius (cm) at and above which a mesh with no distance of its own gets NoCutOff: the end of the last band, 30 cm. */
	static double NoCutOffRadius();

	/**
	 * The footprint radius (FMantleDrawDistanceSource::FootprintRadius) at and above which a mesh gets no default draw distance at all and is always loaded:
	 * ground and decks of 100 m radius. A narrower piece, however tall, is a building's and goes to its cell.
	 */
	static double NeverCullRadius();

	/** Whether a published draw distance is NoCutOff. */
	static bool IsNoCutOff(double DrawDistance);

	/** Sets Source's draw distance on Props (0 for none), or 0 when the target client does not read drawDistance. True when one was set. */
	static bool Apply(const FMantleDrawDistanceSource& Source, const FMantleTargetFeatures& Target, FMantleStaticMeshProps& Props);

	/** The source numbers of a component (an instanced one included); Mesh is the mesh to measure (the component's own when null). */
	static FMantleDrawDistanceSource ReadComponent(const UStaticMeshComponent* Component, const UStaticMesh* Mesh = nullptr);

	/** The StaticMesh and InstancedMesh entities of Place with a drawDistance. */
	static int32 CountEntities(const FMantlePlace& Place);

	/** True when Place has a StaticMesh or InstancedMesh entity, whatever its distances. */
	static bool HasMeshEntities(const FMantlePlace& Place);
};
