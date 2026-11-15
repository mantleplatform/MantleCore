// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCollisionGate.h"

#include "Containers/Ticker.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HAL/PlatformTime.h"
#include "MantleCellArea.h"
#include "MantleRuntime.h"

namespace MantleCollisionGatePrivate
{
	/** What every gate in the process cost and did since the last stats line (game thread only). */
	struct FGateStats
	{
		int32 Total = 0;
		int32 Closed = 0;
		int32 Asks = 0;
		double AskSeconds = 0.0;
		double WorstAskSeconds = 0.0;
		/** Per frame: the time spent asking in the frame being counted, and the frames' mean and worst. */
		uint64 FrameNumber = 0;
		double FrameSeconds = 0.0;
		double FrameSum = 0.0;
		double FrameWorst = 0.0;
		int32 Frames = 0;
		FTSTicker::FDelegateHandle Ticker;

		void FlushFrame()
		{
			if (FrameNumber != 0)
			{
				FrameSum += FrameSeconds;
				FrameWorst = FMath::Max(FrameWorst, FrameSeconds);
				++Frames;
			}
			FrameSeconds = 0.0;
		}
	};

	FGateStats Stats;

	bool LogStats(float)
	{
		Stats.FlushFrame();
		if (Stats.Total > 0)
		{
			UE_LOG(LogMantle, Log, TEXT("MANTLE_COL_GATE_STATS closed=%d total=%d asks=%d ask_mean_us=%.1f ask_worst_us=%.1f frame_mean_ms=%.4f frame_worst_ms=%.4f"),
				Stats.Closed, Stats.Total, Stats.Asks, Stats.Asks > 0 ? Stats.AskSeconds / Stats.Asks * 1.0e6 : 0.0, Stats.WorstAskSeconds * 1.0e6,
				Stats.Frames > 0 ? Stats.FrameSum / Stats.Frames * 1000.0 : 0.0, Stats.FrameWorst * 1000.0);
		}
		Stats.Asks = 0;
		Stats.AskSeconds = 0.0;
		Stats.WorstAskSeconds = 0.0;
		Stats.FrameSum = 0.0;
		Stats.FrameWorst = 0.0;
		Stats.Frames = 0;
		return true;
	}

	void CountIn()
	{
		++Stats.Total;
		if (!Stats.Ticker.IsValid())
		{
			Stats.Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&LogStats), 1.0f);
		}
	}
}

double FMantleCollisionGate::RadiusCm(const double BodyRadiusCm, const double SpeedCmPerSecond)
{
	return FMath::Max(BodyRadiusCm, 0.0) + MarginCm + FMath::Max(SpeedCmPerSecond, 0.0) * LookaheadSeconds;
}

bool FMantleCollisionGate::NextClosed(const bool bClosed, const bool bGroundStands, const double NowSeconds, double& InOutStandingSince)
{
	if (!bGroundStands)
	{
		InOutStandingSince = -1.0;
		return true;
	}
	if (!bClosed)
	{
		return false;
	}
	if (InOutStandingSince < 0.0)
	{
		InOutStandingSince = NowSeconds;
	}
	return NowSeconds - InOutStandingSince < ReopenSeconds;
}

FMantleCollisionGate::~FMantleCollisionGate()
{
	using namespace MantleCollisionGatePrivate;
	if (bCounted)
	{
		--Stats.Total;
		Stats.Closed -= bClosed ? 1 : 0;
	}
}

void FMantleCollisionGate::UpdateSimulatedCharacter(ACharacter& Character)
{
	if (Character.GetLocalRole() != ROLE_SimulatedProxy)
	{
		return;
	}
	const UCapsuleComponent* Capsule = Character.GetCapsuleComponent();
	const bool bChanged = Update(Character.GetWorld(), Character.GetActorLocation(), Capsule ? Capsule->GetScaledCapsuleHalfHeight() : 90.0,
		Character.GetReplicatedMovement().LinearVelocity.Size(), TEXT("character"), Character.GetName());
	UCharacterMovementComponent* Movement = Character.GetCharacterMovement();
	if (bClosed)
	{
		// No ground here: the copy stays where the server's updates put it. Gravity off is not enough: a simulated character that finds no floor goes into the engine's
		// falling mode all the same (CharacterMovementComponent.cpp:2331-2339), so it is kept out of every mode that simulates (MOVE_None returns before any of it).
		Character.bSimGravityDisabled = true;
		bOwnsSimGravityFlag = true;
		if (Movement && Movement->MovementMode != MOVE_None)
		{
			Movement->SetMovementMode(MOVE_None);
		}
	}
	else if (bOwnsSimGravityFlag)
	{
		Character.bSimGravityDisabled = false;
		bOwnsSimGravityFlag = false;
		if (Movement)
		{
			// The mode the server says it is in (it may have changed while this copy was held).
			Movement->ApplyNetworkMovementMode(Character.GetReplicatedMovementMode());
		}
	}
	if (bChanged)
	{
		UE_LOG(LogMantle, Log, TEXT("MANTLE_COL_GATE_MODE body=%s state=%s movement_mode=%s grav_disabled=%d"), *Character.GetName(), bClosed ? TEXT("closed") : TEXT("open"),
			Movement ? *Movement->GetMovementName() : TEXT("none"), Character.bSimGravityDisabled ? 1 : 0);
	}
}

bool FMantleCollisionGate::Update(UWorld* World, const FVector& Point, const double BodyRadiusCm, const double SpeedCmPerSecond, const TCHAR* Kind, const FString& Name)
{
	// Only a body that another machine owns asks (the callers return first for an authority body), so this is a client's: a dedicated server has none, and a standalone
	// game's authority bodies wait for their ground differently (AMantlePhysicsProp::BeginSimulation).
	return UpdateWhenClient(World, Point, BodyRadiusCm, SpeedCmPerSecond, Kind, Name);
}

bool FMantleCollisionGate::UpdateWhenClient(UWorld* World, const FVector& Point, const double BodyRadiusCm, const double SpeedCmPerSecond, const TCHAR* Kind, const FString& Name)
{
	using namespace MantleCollisionGatePrivate;
	if (!World)
	{
		return false;
	}
	const double Now = World->GetTimeSeconds();
	// Ground that goes is told by the streamer's take-down serial: ask again in this frame, not at the next interval.
	uint64 TakeDownSerial = 0;
	const bool bGroundMayHaveGone = FMantleCellArea::TakeDownSerial(World, /*bSolidOnly*/ true, TakeDownSerial) && TakeDownSerial != LastTakeDownSerial;
	LastTakeDownSerial = TakeDownSerial;
	const bool bDue = bGroundMayHaveGone || Now - LastAskSeconds >= AskIntervalSeconds || FVector::DistSquared(Point, LastAskPoint) >= ReaskDistanceCm * ReaskDistanceCm;
	if (bDue)
	{
		const double AskStart = FPlatformTime::Seconds();
		FMantleCellAreaQuery Query;
		Query.Point = Point;
		Query.RadiusCm = RadiusCm(BodyRadiusCm, SpeedCmPerSecond);
		Query.bSolidOnly = true;
		FMantleCellAreaState State;
		uint64 Serial = 0;
		// A world that builds no cells for this question (nothing streams here, or a standalone game keeps every solid thing) has ground everywhere.
		bLastGroundStands = !FMantleCellArea::Ask(World, Query, State, Serial) || State.AllStand();
		LastAskSeconds = Now;
		LastAskPoint = Point;
		const double Took = FPlatformTime::Seconds() - AskStart;
		if (Stats.FrameNumber != GFrameCounter)
		{
			Stats.FlushFrame();
			Stats.FrameNumber = GFrameCounter;
		}
		Stats.FrameSeconds += Took;
		++Stats.Asks;
		Stats.AskSeconds += Took;
		Stats.WorstAskSeconds = FMath::Max(Stats.WorstAskSeconds, Took);
	}
	if (!bCounted)
	{
		bCounted = true;
		CountIn();
	}
	const bool bWasClosed = bClosed;
	bClosed = NextClosed(bClosed, bLastGroundStands, Now, StandingSince);
	if (bClosed == bWasClosed)
	{
		return false;
	}
	Stats.Closed += bClosed ? 1 : -1;
	UE_LOG(LogMantle, Log, TEXT("MANTLE_COL_GATE body=%s kind=%s state=%s reason=%s t=%.2f at=(%.0f, %.0f, %.0f)"), *Name, Kind, bClosed ? TEXT("closed") : TEXT("open"),
		bClosed ? TEXT("the solid cells around it are not built here") : TEXT("the solid cells around it stand"), Now, Point.X, Point.Y, Point.Z);
	return true;
}
