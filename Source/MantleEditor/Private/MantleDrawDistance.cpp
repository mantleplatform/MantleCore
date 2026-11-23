// Copyright Joshua Eger. All Rights Reserved.
#include "MantleDrawDistance.h"

#include "MantleClientFeatures.h"
#include "MantlePlaceLimits.h"
#include "MantleTargetFeatures.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleDrawDistancePrivate
{
	/**
	 * Radius below (cm), draw distance: the one size band left (the big-map plan, step 3). Small clutter under 30 cm radius culls at
	 * 60 m, where it is still about 12 pixels across on a 1080p screen. It streams in the grid's 64 m cells, as before.
	 *
	 * Every bigger piece, up to NeverCullRadius, has no cut-off of its own (NoCutOff): it goes to the 256 m cells and draws as long as
	 * its cell stands, so a building's pieces come and go together with the ground they stand on (fable-review.md 4.1, 4.2). Before,
	 * four more bands (120 m, 250 m, 600 m and 2.5 km) cut each piece at its own distance and put it in a cell level by that distance,
	 * which split buildings across levels and left a ring between a piece's distance and its cell's where nothing drew it.
	 */
	const FMantleDrawDistance::FBand GBands[] = {
		{ 30.0, 6000.0 },
	};

	/** A mesh of this radius (cm) or more is never culled: ground tiles and freeway decks of a whole district, always loaded. */
	constexpr double GNeverCullRadius = 10000.0;

	/** The distance in cm; 0 at or past MaxDrawDistance (then the mesh is never culled), so that number only ever means NoCutOff. */
	double InRange(const double Distance)
	{
		return Distance >= FMantlePlaceLimits::MaxDrawDistance ? 0.0 : Distance;
	}
}

TConstArrayView<FMantleDrawDistance::FBand> FMantleDrawDistance::Bands()
{
	return MakeArrayView(MantleDrawDistancePrivate::GBands);
}

double FMantleDrawDistance::NeverCullRadius()
{
	return MantleDrawDistancePrivate::GNeverCullRadius;
}

double FMantleDrawDistance::NoCutOffRadius()
{
	return MantleDrawDistancePrivate::GBands[UE_ARRAY_COUNT(MantleDrawDistancePrivate::GBands) - 1].MaxRadius;
}

bool FMantleDrawDistance::IsNoCutOff(const double DrawDistance)
{
	return DrawDistance >= NoCutOff;
}

bool FMantleDrawDistance::Apply(const FMantleDrawDistanceSource& Source, const FMantleTargetFeatures& Target, FMantleStaticMeshProps& Props)
{
	using namespace MantleDrawDistancePrivate;
	Props.DrawDistance = 0.0;
	if (!Target.Reads(MantleClientFeatures::DrawDistance) || Source.bNeverDistanceCull)
	{
		return false;
	}

	// The component's own: the end cull distance of instances, else Desired Max Draw Distance, else the current one. Never 0 (refused).
	double Sourced = 0.0;
	if (Source.bInstanced && Source.InstanceEndCullDistance > 0)
	{
		Sourced = Source.InstanceEndCullDistance;
	}
	else if (Source.LDMaxDrawDistance > 0.0)
	{
		Sourced = Source.LDMaxDrawDistance;
	}
	else if (Source.CachedMaxDrawDistance > 0.0)
	{
		Sourced = Source.CachedMaxDrawDistance;
	}

	if (Sourced > 0.0)
	{
		Props.DrawDistance = InRange(Sourced);
	}
	else
	{
		// The size band, by the mesh's bounding sphere; a bigger mesh has no cut-off of its own (its cell decides), and a huge one none at all.
		for (const FBand& Band : Bands())
		{
			if (Source.SphereRadius < Band.MaxRadius)
			{
				Props.DrawDistance = Band.Draw;
				break;
			}
		}
		if (Props.DrawDistance <= 0.0 && Source.SphereRadius < NeverCullRadius())
		{
			Props.DrawDistance = NoCutOff;
		}
	}
	return Props.DrawDistance > 0.0;
}

FMantleDrawDistanceSource FMantleDrawDistance::ReadComponent(const UStaticMeshComponent* Component, const UStaticMesh* Mesh)
{
	FMantleDrawDistanceSource Source;
	if (!Component)
	{
		return Source;
	}
	Source.LDMaxDrawDistance = Component->LDMaxDrawDistance;
	Source.CachedMaxDrawDistance = Component->CachedMaxDrawDistance;
	Source.bNeverDistanceCull = Component->bNeverDistanceCull;

	if (!Mesh)
	{
		Mesh = Component->GetStaticMesh();
	}
	double Scale = Component->GetComponentScale().GetAbsMax();
	if (const UInstancedStaticMeshComponent* Instanced = Cast<UInstancedStaticMeshComponent>(Component))
	{
		Source.bInstanced = true;
		Source.InstanceEndCullDistance = Instanced->InstanceEndCullDistance;

		// One component, one distance: sized by its largest instance, so a big one is not culled at a small one's distance.
		double LargestInstance = 0.0;
		for (int32 Index = 0; Index < Instanced->GetInstanceCount(); ++Index)
		{
			FTransform Instance;
			if (Instanced->GetInstanceTransform(Index, Instance, /*bWorldSpace*/ false))
			{
				LargestInstance = FMath::Max(LargestInstance, Instance.GetScale3D().GetAbsMax());
			}
		}
		Scale *= FMath::Max(LargestInstance, KINDA_SMALL_NUMBER);
	}
	Source.SphereRadius = Mesh ? Mesh->GetBounds().SphereRadius * Scale : 0.0;
	return Source;
}

int32 FMantleDrawDistance::CountEntities(const FMantlePlace& Place)
{
	int32 Count = 0;
	for (const FMantleEntity& Entity : Place.Entities)
	{
		const FMantleStaticMeshProps* Mesh = Entity.Props.TryGet<FMantleStaticMeshProps>();
		if (const FMantleInstancedMeshProps* Instanced = Entity.Props.TryGet<FMantleInstancedMeshProps>())
		{
			Mesh = &Instanced->Mesh;
		}
		Count += Mesh && Mesh->DrawDistance > 0.0 ? 1 : 0;
	}
	return Count;
}

bool FMantleDrawDistance::HasMeshEntities(const FMantlePlace& Place)
{
	for (const FMantleEntity& Entity : Place.Entities)
	{
		if (Entity.Props.TryGet<FMantleStaticMeshProps>() || Entity.Props.TryGet<FMantleInstancedMeshProps>())
		{
			return true;
		}
	}
	return false;
}
