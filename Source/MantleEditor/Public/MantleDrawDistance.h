// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
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

	/** The StaticMesh and InstancedMesh entities of Place with a drawDistance. */
	static int32 CountEntities(const FMantlePlace& Place);

	/** True when Place has a StaticMesh or InstancedMesh entity, whatever its distances. */
	static bool HasMeshEntities(const FMantlePlace& Place);
};
