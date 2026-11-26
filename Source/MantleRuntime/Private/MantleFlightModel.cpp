// Copyright Joshua Eger. All Rights Reserved.
#include "MantleFlightModel.h"

namespace MantleFlightModelPrivate
{
	/** A finite number, or Fallback. */
	double Finite(const double Value, const double Fallback = 0.0)
	{
		return FMath::IsFinite(Value) ? Value : Fallback;
	}

	/** The stall angle every curve uses: the surface's, held to the schema's 5-30 degrees. */
	double Stall(const FMantleAerofoilSurface& Surface)
	{
		return FMath::Clamp(Finite(Surface.StallDegrees, 16.0), 5.0, 30.0);
	}

	/** The surface's peak lift, never negative. */
	double PeakLift(const FMantleAerofoilSurface& Surface)
	{
		return FMath::Max(Finite(Surface.MaxLift, 1.5), 0.0);
	}

	/** Where the post-stall fade ends and the flat plate takes over: 30 degrees, or the stall + 4 for a late stall. */
	double FadeEnd(const double Stall)
	{
		return FMath::Max(30.0, Stall + 4.0);
	}

	/** An angle in degrees folded into -180..180. */
	double Wrap(const double Degrees)
	{
		return FMath::UnwindDegrees(Finite(Degrees));
	}

	/** The flat plate's lift, 1.1 sin(2 angle). */
	double FlatPlateLift(const double Degrees)
	{
		return 1.1 * FMath::Sin(FMath::DegreesToRadians(2.0 * Degrees));
	}

	/** How attached the flow is at an angle: 1 to the stall, falling to 0 at the fade's end, 0 past it and for air from behind. */
	double Attached(const double Degrees, const double Stall)
	{
		const double A = FMath::Abs(Degrees);
		if (A > 90.0)
		{
			return 0.0;
		}
		const double End = FadeEnd(Stall);
		return A <= Stall ? 1.0 : FMath::Clamp((End - A) / (End - Stall), 0.0, 1.0);
	}
}

double FMantleFlightModel::LiftSlopePerDegree(const FMantleAerofoilSurface& Surface)
{
	using namespace MantleFlightModelPrivate;
	return PeakLift(Surface) / (0.9 * Stall(Surface));
}

double FMantleFlightModel::LiftCoefficient(const double AngleDegrees, const double ControlDegrees, const FMantleAerofoilSurface& Surface)
{
	using namespace MantleFlightModelPrivate;
	const double Angle = Wrap(AngleDegrees);
	const double S = Stall(Surface);
	const double Peak = PeakLift(Surface);
	const double A = FMath::Abs(Angle);
	double Base = 0.0;
	if (A > 90.0)
	{
		// Air from behind: the flat plate (its sign follows the angle on its own).
		Base = FlatPlateLift(Angle);
	}
	else
	{
		const double End = FadeEnd(S);
		double Magnitude = 0.0;
		if (A <= 0.8 * S)
		{
			Magnitude = Peak * A / (0.9 * S);
		}
		else if (A <= S)
		{
			const double Rest = 1.0 - A / S;
			Magnitude = Peak * (1.0 - (1.0 / 0.36) * Rest * Rest);
		}
		else if (A <= S + 2.0)
		{
			Magnitude = Peak * (1.0 - 0.3 * (A - S) / 2.0);
		}
		else if (A <= End)
		{
			const double T = (A - (S + 2.0)) / (End - (S + 2.0));
			Magnitude = FMath::Lerp(0.7 * Peak, FlatPlateLift(End), T);
		}
		else
		{
			Magnitude = FlatPlateLift(A);
		}
		Base = Angle < 0.0 ? -Magnitude : Magnitude;
	}
	// The control surface's lift step while the flow is attached: below the stall it is what adding its angle did, and it never moves the stall.
	const double Step = LiftSlopePerDegree(Surface) * Finite(ControlDegrees) * Attached(Angle, S);
	// The step (attached flow only) is held to 1.25 x the peak; the flat plate past the stall is never cut.
	if (Step == 0.0)
	{
		return Base;
	}
	const double Cap = 1.25 * Peak;
	return FMath::Clamp(Base + Step, -Cap, Cap);
}

double FMantleFlightModel::DragCoefficient(const double AngleDegrees, const double Lift, const FMantleAerofoilSurface& Surface)
{
	using namespace MantleFlightModelPrivate;
	const double Aspect = FMath::Max(Finite(Surface.AspectRatio, 7.0), 0.1);
	const double FromLift = ProfileDrag + Lift * Lift / (UE_DOUBLE_PI * SpanEfficiency * Aspect);
	const double Sine = FMath::Sin(FMath::DegreesToRadians(Wrap(AngleDegrees)));
	return FMath::Max(FromLift, 1.2 * Sine * Sine);
}

double FMantleFlightModel::LiftPointShiftCm(const double AngleDegrees, const FMantleAerofoilSurface& Surface)
{
	using namespace MantleFlightModelPrivate;
	const double Quarter = 0.25 * FMath::Max(Finite(Surface.ChordCm), 0.0);
	return Quarter * (1.0 - Attached(Wrap(AngleDegrees), Stall(Surface)));
}

double FMantleFlightModel::AirflowAngleDegrees(const FVector& Velocity)
{
	if (Velocity.ContainsNaN() || (FMath::IsNearlyZero(Velocity.X) && FMath::IsNearlyZero(Velocity.Z)))
	{
		return 0.0;
	}
	// The air meets the surface against its motion: moving forward and down, it comes from below (a positive angle).
	return FMath::RadiansToDegrees(FMath::Atan2(-Velocity.Z, Velocity.X));
}

FMantleAerofoilForce FMantleFlightModel::AerofoilForce(const FVector& Velocity, const double ControlDegrees, const double Density, const FMantleAerofoilSurface& Surface)
{
	using namespace MantleFlightModelPrivate;
	FMantleAerofoilForce Out;
	if (Velocity.ContainsNaN() || !FMath::IsFinite(Density) || Density <= 0.0 || !FMath::IsFinite(Surface.AreaM2) || Surface.AreaM2 <= 0.0)
	{
		return Out;
	}
	// The span's flow does nothing to a surface: drop it before the angle and the dynamic pressure.
	const FVector Chordwise(Velocity.X, 0.0, Velocity.Z);
	const double SpeedSq = Chordwise.SizeSquared();
	if (SpeedSq < UE_SMALL_NUMBER)
	{
		return Out;
	}
	const FVector Along = Chordwise / FMath::Sqrt(SpeedSq);
	Out.AngleDegrees = Wrap(AirflowAngleDegrees(Chordwise) + Finite(Surface.CamberDegrees));
	Out.LiftCoefficient = LiftCoefficient(Out.AngleDegrees, ControlDegrees, Surface) * Finite(Surface.LiftMultiplier, 1.0);
	Out.DragCoefficient = DragCoefficient(Out.AngleDegrees, Out.LiftCoefficient, Surface) * Finite(Surface.DragMultiplier, 1.0);
	// Lift at right angles to the airflow in the chord-lift plane (motion x span: +Z flying forward, -Z flying backward), drag against the motion.
	const FVector LiftAxis = FVector::CrossProduct(Along, FVector(0.0, 1.0, 0.0)).GetSafeNormal();
	const double Q = 0.5 * Density * SpeedSq * Surface.AreaM2;
	Out.Force = LiftAxis * (Q * Out.LiftCoefficient) - Along * (Q * Out.DragCoefficient);
	Out.PointX = -LiftPointShiftCm(Out.AngleDegrees, Surface);
	return Out;
}

double FMantleFlightModel::Thrust(const FMantlePlaneEngine& Engine, const double Throttle, const double AirspeedMs, const double DensityRatio)
{
	using namespace MantleFlightModelPrivate;
	const double Lever = FMath::Clamp(Finite(Throttle), 0.0, 1.0);
	const double Ratio = FMath::Clamp(Finite(DensityRatio, 1.0), 0.0, 1.0);
	const double Dry = FMath::Max(Finite(Engine.MaxThrustNewtons), 0.0);
	const double Power = FMath::Max(Finite(Engine.PowerWatts), 0.0);
	if (Power > 0.0)
	{
		// A propeller: its static push, until the power it turns into push (0.8 P / V) is less.
		const double Speed = FMath::Max(Finite(AirspeedMs), PropellerLeastSpeed);
		return Lever * FMath::Min(Dry, PropellerEfficiency * Power / Speed) * Ratio;
	}
	const double Burner = Finite(Engine.AfterburnerNewtons);
	if (Burner <= Dry)
	{
		return Lever * Dry * Ratio;
	}
	const double Push = Lever <= AfterburnerLever ? Dry * Lever / AfterburnerLever
		: Dry + (Burner - Dry) * (Lever - AfterburnerLever) / (1.0 - AfterburnerLever);
	return Push * Ratio;
}

double FMantleFlightModel::AdvanceSpool(const double Spool, const double Lever, const double SpoolSeconds, const double Dt)
{
	using namespace MantleFlightModelPrivate;
	const double Target = FMath::Clamp(Finite(Lever), 0.0, 1.0);
	const double Now = FMath::Clamp(Finite(Spool), 0.0, 1.0);
	const double Seconds = Finite(SpoolSeconds);
	const double Step = FMath::Max(Finite(Dt), 0.0);
	if (Seconds <= 0.0)
	{
		return Target;
	}
	if (Target > Now)
	{
		// d(spool)/dt = (0.25 + 0.75 spool) ln 4 / (0.75 SpoolSeconds), solved exactly so any frame rate spools alike: 0.25 x 4^(t / SpoolSeconds) from idle.
		const double Grown = ((0.25 + 0.75 * Now) * FMath::Pow(4.0, Step / Seconds) - 0.25) / 0.75;
		return FMath::Min(Grown, Target);
	}
	return FMath::Max(Now - 2.0 * Step / Seconds, Target);
}

double FMantleFlightModel::StallProtectionShare(const double StallShare)
{
	using namespace MantleFlightModelPrivate;
	return FMath::Clamp((0.95 - Finite(StallShare)) / 0.10, 0.0, 1.0);
}

double FMantleFlightModel::CoordinatedYaw(const double SideslipDegrees)
{
	using namespace MantleFlightModelPrivate;
	return FMath::Clamp(Finite(SideslipDegrees) / 10.0, -0.5, 0.5);
}
