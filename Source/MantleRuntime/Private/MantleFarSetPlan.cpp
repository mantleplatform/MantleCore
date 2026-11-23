// Copyright Joshua Eger. All Rights Reserved.
#include "MantleFarSetPlan.h"

bool FMantleFarSetPlan::CellHasFarSet(const FMantleCellInfo& Cell, const FMantleFarSetSettings& Settings)
{
	return Settings.bEnabled && Cell.DrawDistance >= Settings.MinDrawDistanceCm;
}

double FMantleFarSetPlan::ReachOf(const FMantleCellInfo& Cell, const FMantleFarSetSettings& Settings)
{
	return FMath::Min(Cell.DrawDistance * Settings.ReachFactor, Settings.MaxReachCm);
}

double FMantleFarSetPlan::LiveReachOf(const FMantleCellInfo& Cell, const FMantleFarSetSettings& Settings)
{
	return ReachOf(Cell, Settings) * FMath::Clamp(Settings.ReachScale, 0.0, 1.0);
}

bool FMantleFarSetPlan::IsWanted(const FMantleCellInfo& Cell, const FMantleCellStreamPlan::FPath& Path, const FMantleFarSetSettings& Settings)
{
	return CellHasFarSet(Cell, Settings) && Settings.ReachScale > 0.0 && FMantleCellStreamPlan::DistanceToCell(Cell, Path) <= LiveReachOf(Cell, Settings);
}

bool FMantleFarSetPlan::IsReleased(const FMantleCellInfo& Cell, const FMantleCellStreamPlan::FPath& Path, const FMantleFarSetSettings& Settings)
{
	const double Reach = LiveReachOf(Cell, Settings);
	const double Release = FMath::Max(Reach * Settings.ReleaseMargin, Reach + Settings.MinReleaseMarginCm);
	return FMantleCellStreamPlan::DistanceToCell(Cell, Path) > Release;
}

bool FMantleFarSetPlan::NeedsBuilding(const FMantleCellInfo& Cell, const FMantleCellStreamPlan::FPath& Path, const FMantleFarSetSettings& Settings)
{
	// Not while a budget has given the far sets up (the real cells' reach shrinks only after that, so a cell is never left with neither).
	return Settings.ReachScale > 0.0 && FMantleCellStreamPlan::DistanceToCell(Cell, Path) > Cell.DrawDistance * Settings.BuildFromShare;
}

int32 FMantleFarSetPlan::StrideFor(const double EntityDrawDistanceCm, const FMantleFarSetSettings& Settings)
{
	return EntityDrawDistanceCm >= Settings.FullDensityFromCm ? 1 : FMath::Max(Settings.ThinnedStride, 1);
}

bool FMantleFarSetPlan::MakeTwin(const FMantleEntity& Source, const double ReachCm, const FMantleFarSetSettings& Settings, FMantleEntity& OutTwin)
{
	const FMantleStaticMeshProps* SourceProps = MantleMeshPropsOf(Source);
	// A collision-only mesh is never drawn, near or far.
	if (!SourceProps || !SourceProps->bVisible || SourceProps->DrawDistance < Settings.MinDrawDistanceCm || SourceProps->DrawDistance <= 0.0)
	{
		return false;
	}
	// Cell entities have no parent, tags, interactable, physics or script name (cell contract 5.2), so a twin carries none of them either.
	FMantleEntity Twin;
	Twin.Id = Source.Id;
	Twin.Type = Source.Type;
	Twin.Name = Source.Name + TEXT(" (far)");
	Twin.Transform = Source.Transform;
	Twin.bDynamic = false;

	FMantleStaticMeshProps Mesh;
	Mesh.Mesh = SourceProps->Mesh;
	Mesh.Materials = SourceProps->Materials;
	Mesh.Collision = EMantleCollision::None;
	Mesh.bCastShadow = false;
	Mesh.Surface = EMantleSurface::Default;
	Mesh.DrawDistance = ReachCm;

	if (const FMantleInstancedMeshProps* Instanced = Source.Props.TryGet<FMantleInstancedMeshProps>())
	{
		const int32 Stride = StrideFor(SourceProps->DrawDistance, Settings);
		FMantleInstancedMeshProps Kept;
		Kept.Mesh = MoveTemp(Mesh);
		Kept.Instances.Reserve((Instanced->Instances.Num() + Stride - 1) / Stride);
		for (int32 At = 0; At < Instanced->Instances.Num(); At += Stride)
		{
			Kept.Instances.Add(Instanced->Instances[At]);
		}
		if (Kept.Instances.IsEmpty())
		{
			return false;
		}
		Twin.Props.Emplace<FMantleInstancedMeshProps>(MoveTemp(Kept));
	}
	else if (Source.Props.TryGet<FMantleStaticMeshProps>())
	{
		Twin.Props.Emplace<FMantleStaticMeshProps>(MoveTemp(Mesh));
	}
	else
	{
		return false;
	}
	OutTwin = MoveTemp(Twin);
	return true;
}

bool FMantleFarSetPlan::ShouldShow(const bool bCellWanted, const bool bRealStanding)
{
	return !(bCellWanted && bRealStanding);
}

FMantleFarSetPlan::ENaniteLeftOut FMantleFarSetPlan::ClassifyNaniteLeftOut(const bool bMeshHasNaniteData, const bool bAnyMaterialSeeThrough)
{
	if (bMeshHasNaniteData)
	{
		return ENaniteLeftOut::Kept;
	}
	return bAnyMaterialSeeThrough ? ENaniteLeftOut::SeeThrough : ENaniteLeftOut::NoNanite;
}
