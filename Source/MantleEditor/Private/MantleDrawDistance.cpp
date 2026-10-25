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
	 * Radius below (cm), draw distance. A prop at the top of a band (30 cm radius) is still about 12 pixels across at
	 * its 60 m on a 1080p screen, and one at 3 m radius at 250 m, so nothing pops where a player looks.
	 */
	const FMantleDrawDistance::FBand GBands[] = {
		{ 30.0, 6000.0 },
		{ 100.0, 12000.0 },
		{ 300.0, 25000.0 },
		{ 1000.0, 60000.0 },
	};

	/** The distance in cm; 0 past MaxDrawDistance (then the mesh is never culled). */
	double InRange(const double Distance)
	{
		return Distance > FMantlePlaceLimits::MaxDrawDistance ? 0.0 : Distance;
	}
}

TConstArrayView<FMantleDrawDistance::FBand> FMantleDrawDistance::Bands()
{
	return MakeArrayView(MantleDrawDistancePrivate::GBands);
}

double FMantleDrawDistance::NeverCullRadius()
{
	return MantleDrawDistancePrivate::GBands[UE_ARRAY_COUNT(MantleDrawDistancePrivate::GBands) - 1].MaxRadius;
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
		// The size band, by the mesh's bounding sphere; a mesh past the last one has none.
		for (const FBand& Band : Bands())
		{
			if (Source.SphereRadius < Band.MaxRadius)
			{
				Props.DrawDistance = Band.Draw;
				break;
			}
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
