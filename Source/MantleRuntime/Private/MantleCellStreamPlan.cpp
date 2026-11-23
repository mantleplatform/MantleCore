// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCellStreamPlan.h"

void FMantleCellStreamPlan::PathOf(const FMantleCellView& View, const FMantleCellStreamSettings& Settings, FPath& OutPath)
{
	OutPath.Reset();
	OutPath.Add(View.Location);
	const double Speed = View.Velocity.Size();
	if (Speed < Settings.MinLookAheadSpeedCmPerSec || Speed <= UE_DOUBLE_SMALL_NUMBER)
	{
		return;
	}
	const double Distance = FMath::Min(Speed * Settings.LookAheadSeconds, Settings.MaxLookAheadCm);
	const FVector Direction = View.Velocity / Speed;
	for (int32 Step = 1; Step < MaxPathPoints; ++Step)
	{
		OutPath.Add(View.Location + Direction * (Distance * static_cast<double>(Step) / static_cast<double>(MaxPathPoints - 1)));
	}
}

double FMantleCellStreamPlan::DistanceToCell(const FMantleCellInfo& Cell, const FVector& Point)
{
	return FMath::Sqrt(Cell.Bounds.ComputeSquaredDistanceToPoint(Point));
}

double FMantleCellStreamPlan::DistanceToCell(const FMantleCellInfo& Cell, const FPath& Path)
{
	double Nearest = TNumericLimits<double>::Max();
	for (const FVector& Point : Path)
	{
		Nearest = FMath::Min(Nearest, DistanceToCell(Cell, Point));
	}
	return Nearest;
}

bool FMantleCellStreamPlan::IsWanted(const FMantleCellInfo& Cell, const FPath& Path)
{
	return DistanceToCell(Cell, Path) <= Cell.DrawDistance;
}

double FMantleCellStreamPlan::ReachOf(const FMantleCellInfo& Cell, const FMantleCellStreamSettings& Settings)
{
	return FMantleStreamBudget::RingReach(Cell.DrawDistance, Settings.ReachScale, Settings.BudgetTuning.MinReachCm, Settings.RingViewScale, Settings.RaisedReachCm, Settings.RaiseShare);
}

bool FMantleCellStreamPlan::IsWanted(const FMantleCellInfo& Cell, const FPath& Path, const FMantleCellStreamSettings& Settings)
{
	return DistanceToCell(Cell, Path) <= ReachOf(Cell, Settings);
}

double FMantleCellStreamPlan::ReleaseDistance(const FMantleCellInfo& Cell, const FMantleCellStreamSettings& Settings)
{
	const double Reach = ReachOf(Cell, Settings);
	return FMath::Max(Reach * Settings.ReleaseMargin, Reach + Settings.MinReleaseMarginCm);
}

bool FMantleCellStreamPlan::IsReleased(const FMantleCellInfo& Cell, const FPath& Path, const FMantleCellStreamSettings& Settings)
{
	return DistanceToCell(Cell, Path) > ReleaseDistance(Cell, Settings);
}

FMantleCellStreamPlan::FRank FMantleCellStreamPlan::RankOf(const FMantleCellInfo& Cell, const int32 Index, const FMantleCellView& View, const FMantleCellStreamSettings& Settings)
{
	FRank Rank;
	Rank.Index = Index;
	Rank.ViewDistance = DistanceToCell(Cell, View.Location);
	const double Speed = View.Velocity.Size();
	if (Speed < Settings.MinLookAheadSpeedCmPerSec || Speed <= UE_DOUBLE_SMALL_NUMBER || Settings.OrderAheadSeconds <= 0.0 || Settings.LookAheadSeconds <= 0.0)
	{
		Rank.Key = Rank.ViewDistance;
		return Rank;
	}
	const double Shift = FMath::Min(Speed * Settings.OrderAheadSeconds, Settings.MaxOrderShiftCm);
	Rank.Key = DistanceToCell(Cell, View.Location + View.Velocity / Speed * Shift);
	return Rank;
}

bool FMantleCellStreamPlan::BuildsBefore(const FRank& A, const FRank& B)
{
	if (A.Key != B.Key)
	{
		return A.Key < B.Key;
	}
	if (A.ViewDistance != B.ViewDistance)
	{
		return A.ViewDistance < B.ViewDistance;
	}
	return A.Index < B.Index;
}

double FMantleCellStreamPlan::BudgetSeconds(const FMantleCellStreamSettings& Settings, const double SpeedCmPerSec, const bool bHolding)
{
	if (bHolding)
	{
		return Settings.HoldBudgetSeconds;
	}
	const double Span = Settings.FastBudgetFullAtCmPerSec - Settings.FastBudgetFromCmPerSec;
	const double Share = Span > 0.0 ? FMath::Clamp((SpeedCmPerSec - Settings.FastBudgetFromCmPerSec) / Span, 0.0, 1.0) : (SpeedCmPerSec >= Settings.FastBudgetFromCmPerSec ? 1.0 : 0.0);
	return FMath::Lerp(Settings.BaseBudgetSeconds, Settings.FastBudgetSeconds, Share);
}

FVector FMantleCellStreamPlan::SmoothVelocity(const FVector& Previous, const FVector& Measured, const double DeltaSeconds, const FMantleCellStreamSettings& Settings)
{
	if (Settings.VelocitySmoothingSeconds <= 0.0)
	{
		return Measured;
	}
	const double Share = FMath::Clamp(DeltaSeconds / Settings.VelocitySmoothingSeconds, 0.0, 1.0);
	return Previous + (Measured - Previous) * Share;
}

bool FMantleCellStreamPlan::IsTeleport(const FVector& Delta, const double DeltaSeconds, const FMantleCellStreamSettings& Settings)
{
	const double Limit = FMath::Max(Settings.TeleportDistanceCm, Settings.TeleportAllowedSpeedCmPerSec * FMath::Max(DeltaSeconds, 0.0));
	return Delta.SizeSquared() > Limit * Limit;
}

bool FMantleCellStreamPlan::HasStalled(const double NowSeconds, const double BeganSeconds, const double LastProgressSeconds, const double IdleTimeoutSeconds)
{
	return NowSeconds - FMath::Max(BeganSeconds, LastProgressSeconds) >= IdleTimeoutSeconds;
}

int32 FMantleCellStreamPlan::PieceCount(const int32 InstanceCount, const bool bSolid, const FMantleCellStreamSettings& Settings)
{
	if (!bSolid || InstanceCount <= 0 || Settings.MaxSolidInstancesPerPiece <= 0)
	{
		return 1;
	}
	return FMath::Max(1, (InstanceCount + Settings.MaxSolidInstancesPerPiece - 1) / Settings.MaxSolidInstancesPerPiece);
}

void FMantleCellStreamPlan::PieceRange(const int32 InstanceCount, const int32 Pieces, const int32 PieceIndex, int32& OutFirst, int32& OutCount)
{
	const int32 Safe = FMath::Max(Pieces, 1);
	const int32 Base = InstanceCount / Safe;
	const int32 Extra = InstanceCount % Safe;
	OutFirst = PieceIndex * Base + FMath::Min(PieceIndex, Extra);
	OutCount = Base + (PieceIndex < Extra ? 1 : 0);
}

void FMantleCellStreamPlan::LevelOverhangs(const FMantleCellIndex& Index, TArray<double>& OutOverhangs)
{
	OutOverhangs.Reset();
	OutOverhangs.SetNumZeroed(Index.Levels.Num());
	for (const FMantleCellInfo& Cell : Index.Cells)
	{
		if (!Index.Levels.IsValidIndex(Cell.Level))
		{
			continue;
		}
		const double Size = static_cast<double>(Index.Levels[Cell.Level].CellSize);
		const double MinX = Size * Cell.X;
		const double MinY = Size * Cell.Y;
		const double Over = FMath::Max(
			FMath::Max(MinX - Cell.Bounds.Min.X, MinY - Cell.Bounds.Min.Y),
			FMath::Max(Cell.Bounds.Max.X - (MinX + Size), Cell.Bounds.Max.Y - (MinY + Size)));
		OutOverhangs[Cell.Level] = FMath::Max(OutOverhangs[Cell.Level], Over);
	}
}

void FMantleCellStreamPlan::CellsNear(const FMantleCellIndex& Index, const TArray<double>& Overhangs, const TArray<double>& LevelRadii, const FPath& Path, TArray<int32>& OutCells)
{
	OutCells.Reset();
	const TArray<FMantleCellInfo>& Cells = Index.Cells;
	const int32 Count = Cells.Num();
	for (int32 Level = 0; Level < Index.Levels.Num() && Level < LevelRadii.Num(); ++Level)
	{
		const double Size = static_cast<double>(Index.Levels[Level].CellSize);
		if (Size <= 0.0)
		{
			continue;
		}
		const double Reach = LevelRadii[Level] + (Overhangs.IsValidIndex(Level) ? Overhangs[Level] : 0.0);
		for (const FVector& Point : Path)
		{
			const int32 MinX = static_cast<int32>(FMath::FloorToDouble((Point.X - Reach) / Size));
			const int32 MaxX = static_cast<int32>(FMath::FloorToDouble((Point.X + Reach) / Size));
			const int32 MinY = static_cast<int32>(FMath::FloorToDouble((Point.Y - Reach) / Size));
			const int32 MaxY = static_cast<int32>(FMath::FloorToDouble((Point.Y + Reach) / Size));
			for (int32 X = MinX; X <= MaxX; ++X)
			{
				// The first cell at or after (Level, X, MinY) in the index's order.
				int32 Low = 0;
				int32 High = Count;
				while (Low < High)
				{
					const int32 Mid = Low + (High - Low) / 2;
					const FMantleCellInfo& Probe = Cells[Mid];
					const bool bBefore = Probe.Level != Level ? Probe.Level < Level : (Probe.X != X ? Probe.X < X : Probe.Y < MinY);
					if (bBefore)
					{
						Low = Mid + 1;
					}
					else
					{
						High = Mid;
					}
				}
				for (int32 At = Low; At < Count && Cells[At].Level == Level && Cells[At].X == X && Cells[At].Y <= MaxY; ++At)
				{
					OutCells.Add(At);
				}
			}
		}
	}
	OutCells.Sort();
	int32 Write = 0;
	for (int32 Read = 0; Read < OutCells.Num(); ++Read)
	{
		if (Write == 0 || OutCells[Read] != OutCells[Write - 1])
		{
			OutCells[Write++] = OutCells[Read];
		}
	}
	OutCells.SetNum(Write, EAllowShrinking::No);
}
