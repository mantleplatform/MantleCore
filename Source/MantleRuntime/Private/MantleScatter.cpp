// Copyright Joshua Eger. All Rights Reserved.
#include "MantleScatter.h"

#include "Async/ParallelFor.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "Misc/CoreMisc.h"

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleScatterPrivate
{
	constexpr uint64 ScatterGoldenGamma = 0x9E3779B97F4A7C15ull;

	/** The schema's alignment enum, in EMantleScatterAlignment order. */
	const TCHAR* const GScatterAlignmentNames[] = { TEXT("upright"), TEXT("tilted"), TEXT("tumbled"), TEXT("ground") };
	constexpr int32 GScatterAlignmentCount = static_cast<int32>(UE_ARRAY_COUNT(GScatterAlignmentNames));

	/** SplitMix64's output function: a bijective 64-bit mix. */
	uint64 ScatterMix64(uint64 Z)
	{
		Z = (Z ^ (Z >> 30)) * 0xBF58476D1CE4E5B9ull;
		Z = (Z ^ (Z >> 27)) * 0x94D049BB133111EBull;
		return Z ^ (Z >> 31);
	}

	/**
	 * One instance's own random stream: SplitMix64 started from the seed and the instance's index.
	 * Counter-based, so an instance never depends on how many draws another one made, and the
	 * generation could be split across threads or cells without changing a single instance.
	 */
	struct FScatterStream
	{
		uint64 State;

		FScatterStream(int32 Seed, uint32 Index)
			: State(ScatterMix64((static_cast<uint64>(static_cast<uint32>(Seed)) << 32) | Index))
		{
		}

		uint64 Next()
		{
			State += ScatterGoldenGamma;
			return ScatterMix64(State);
		}
	};

	uint32 ScatterLow(uint64 Value) { return static_cast<uint32>(Value); }
	uint32 ScatterHigh(uint64 Value) { return static_cast<uint32>(Value >> 32); }

	/** A 32-bit draw mapped onto Lo..Hi inclusive, in integers only (Hi - Lo stays under 2^32). */
	int64 ScatterPick(uint32 Draw, int64 Lo, int64 Hi)
	{
		const uint64 Span = static_cast<uint64>(Hi - Lo) + 1;
		return Lo + static_cast<int64>((static_cast<uint64>(Draw) * Span) >> 32);
	}

	bool ValidateScatterParams(const FMantleScatterParams& Params, FString& OutError)
	{
		if (Params.Seed < 0)
		{
			OutError = FString::Printf(TEXT("seed %d must be 0 or more"), Params.Seed);
			return false;
		}
		const FVector& Half = Params.HalfExtent;
		if (!FMath::IsFinite(Half.X) || !FMath::IsFinite(Half.Y) || !FMath::IsFinite(Half.Z)
			|| Half.X <= 0.0 || Half.Y <= 0.0 || Half.Z < 0.0
			|| Half.X > FMantleScatter::MaxHalfExtentCm || Half.Y > FMantleScatter::MaxHalfExtentCm
			|| Half.Z > FMantleScatter::MaxHalfExtentCm)
		{
			OutError = FString::Printf(TEXT("half extent %s must be above 0 in X and Y, at least 0 in Z, and at most %.0f cm"),
				*Half.ToCompactString(), FMantleScatter::MaxHalfExtentCm);
			return false;
		}
		if (!FMath::IsFinite(Params.Density) || Params.Density < 0.0)
		{
			OutError = FString::Printf(TEXT("density %g must be 0 or more instances per square metre"), Params.Density);
			return false;
		}
		// Written so a NaN fails too.
		if (!(Params.MinScale >= FMantleScatter::MinScaleLimit && Params.MaxScale <= FMantleScatter::MaxScaleLimit
			&& Params.MinScale <= Params.MaxScale))
		{
			OutError = FString::Printf(TEXT("scale range %g to %g must be ordered and within %g to %g"),
				Params.MinScale, Params.MaxScale, FMantleScatter::MinScaleLimit, FMantleScatter::MaxScaleLimit);
			return false;
		}
		if (!(Params.MaxTiltDegrees >= 0.0 && Params.MaxTiltDegrees <= FMantleScatter::MaxTiltLimit))
		{
			OutError = FString::Printf(TEXT("max tilt %g must be 0 to %g degrees"), Params.MaxTiltDegrees, FMantleScatter::MaxTiltLimit);
			return false;
		}
		if (!(Params.DrawDistance == 0.0 || (Params.DrawDistance > 0.0 && Params.DrawDistance <= FMantleScatter::MaxDrawDistanceCm)))
		{
			OutError = FString::Printf(TEXT("draw distance %g must be above 0 and at most %g cm (0 for none)"), Params.DrawDistance,
				FMantleScatter::MaxDrawDistanceCm);
			return false;
		}
		if (!(Params.MaxSlopeDegrees >= 0.0 && Params.MaxSlopeDegrees <= 90.0))
		{
			OutError = FString::Printf(TEXT("max slope %g must be 0 to 90 degrees"), Params.MaxSlopeDegrees);
			return false;
		}
		if (!(Params.MinSlopeDegrees >= 0.0 && Params.MinSlopeDegrees <= Params.MaxSlopeDegrees))
		{
			OutError = FString::Printf(TEXT("min slope %g must be 0 or more and at most the max slope %g degrees"), Params.MinSlopeDegrees, Params.MaxSlopeDegrees);
			return false;
		}
		if (Params.Alignment == EMantleScatterAlignment::Ground && Params.Placement != EMantleScatterPlacement::Terrain)
		{
			OutError = TEXT("alignment ground only applies where placement is terrain");
			return false;
		}
		const double MaxCoordinate = FMantlePlaceLimits::MaxScatterCoordinate;
		if ((Params.MinHeight.IsSet() && !(FMath::Abs(Params.MinHeight.GetValue()) <= MaxCoordinate))
			|| (Params.MaxHeight.IsSet() && !(FMath::Abs(Params.MaxHeight.GetValue()) <= MaxCoordinate)))
		{
			OutError = FString::Printf(TEXT("min and max height must be within %g cm either way"), MaxCoordinate);
			return false;
		}
		if (Params.MinHeight.IsSet() && Params.MaxHeight.IsSet() && Params.MinHeight.GetValue() > Params.MaxHeight.GetValue())
		{
			OutError = FString::Printf(TEXT("max height %g is below min height %g"), Params.MaxHeight.GetValue(), Params.MinHeight.GetValue());
			return false;
		}
		if (Params.PaintLayer < -1 || Params.PaintLayer >= FMantlePlaceLimits::MaxTerrainLayers)
		{
			OutError = FString::Printf(TEXT("paint layer %d must be 0 to %d"), Params.PaintLayer, FMantlePlaceLimits::MaxTerrainLayers - 1);
			return false;
		}
		if ((Params.MinLayerWeight.IsSet() && !(Params.MinLayerWeight.GetValue() >= 0.0 && Params.MinLayerWeight.GetValue() <= 1.0))
			|| (Params.MaxLayerWeight.IsSet() && !(Params.MaxLayerWeight.GetValue() >= 0.0 && Params.MaxLayerWeight.GetValue() <= 1.0)))
		{
			OutError = TEXT("min and max layer weight must be 0 to 1");
			return false;
		}
		if (Params.MinLayerWeight.IsSet() && Params.MaxLayerWeight.IsSet() && Params.MinLayerWeight.GetValue() > Params.MaxLayerWeight.GetValue())
		{
			OutError = FString::Printf(TEXT("max layer weight %g is below min layer weight %g"), Params.MaxLayerWeight.GetValue(), Params.MinLayerWeight.GetValue());
			return false;
		}
		if ((Params.PaintLayer >= 0) != (Params.MinLayerWeight.IsSet() || Params.MaxLayerWeight.IsSet()))
		{
			OutError = TEXT("paint layer and min or max layer weight go together");
			return false;
		}
		if (Params.Shape == EMantleScatterShape::Disc
			&& !(Params.Radius > 0.0 && Params.Radius <= FMantlePlaceLimits::MaxScatterRadius))
		{
			OutError = FString::Printf(TEXT("radius %g must be above 0 and at most %g cm"), Params.Radius, FMantlePlaceLimits::MaxScatterRadius);
			return false;
		}
		if (Params.Shape == EMantleScatterShape::Path)
		{
			if (Params.Path.Num() < 2 || Params.Path.Num() > FMantleScatter::MaxPathPoints)
			{
				OutError = FString::Printf(TEXT("path has %d points; a path takes 2 to %d"), Params.Path.Num(), FMantleScatter::MaxPathPoints);
				return false;
			}
			for (const FVector2D& Point : Params.Path)
			{
				if (!(FMath::Abs(Point.X) <= MaxCoordinate && FMath::Abs(Point.Y) <= MaxCoordinate))
				{
					OutError = FString::Printf(TEXT("path point %s must be within %g cm either way"), *Point.ToString(), MaxCoordinate);
					return false;
				}
			}
			if (!(Params.Width > 0.0 && Params.Width <= FMantlePlaceLimits::MaxScatterPathWidth))
			{
				OutError = FString::Printf(TEXT("width %g must be above 0 and at most %g cm"), Params.Width, FMantlePlaceLimits::MaxScatterPathWidth);
				return false;
			}
		}
		if (Params.Exclusions.Num() > FMantleScatter::MaxExclusions)
		{
			OutError = FString::Printf(TEXT("%d exclusions; a scatter takes at most %d"), Params.Exclusions.Num(), FMantleScatter::MaxExclusions);
			return false;
		}
		for (const FMantleScatterExclusion& Exclusion : Params.Exclusions)
		{
			if (!(FMath::Abs(Exclusion.Center.X) <= MaxCoordinate && FMath::Abs(Exclusion.Center.Y) <= MaxCoordinate
				&& Exclusion.HalfExtent.X > 0.0 && Exclusion.HalfExtent.Y > 0.0
				&& Exclusion.HalfExtent.X <= FMantleScatter::MaxHalfExtentCm && Exclusion.HalfExtent.Y <= FMantleScatter::MaxHalfExtentCm))
			{
				OutError = TEXT("an exclusion's centre must be within 1000000 cm either way and its half extent above 0 and at most 1000000 cm");
				return false;
			}
		}
		if (!(Params.MinSpacing == 0.0 || (Params.MinSpacing > 0.0 && Params.MinSpacing <= FMantleScatter::MaxMinSpacingCm)))
		{
			OutError = FString::Printf(TEXT("min spacing %g must be above 0 and at most %g cm (0 for none)"), Params.MinSpacing, FMantleScatter::MaxMinSpacingCm);
			return false;
		}
		if (!(Params.PathStep == 0.0 || (Params.PathStep >= FMantleScatter::MinPathStepCm && Params.PathStep <= FMantleScatter::MaxCoordinateCm)))
		{
			OutError = FString::Printf(TEXT("path step %g must be %g to %g cm (0 for random along the path)"), Params.PathStep,
				FMantleScatter::MinPathStepCm, FMantleScatter::MaxCoordinateCm);
			return false;
		}
		if (Params.PathStep > 0.0 && Params.Shape != EMantleScatterShape::Path)
		{
			OutError = TEXT("path step only applies where shape is path");
			return false;
		}
		if (Params.PathStep > 0.0 && Params.ClusterCount > 0)
		{
			OutError = TEXT("clusters cannot be combined with a path step: instances sit at a fixed step");
			return false;
		}
		if (Params.PathExclusions.Num() > FMantleScatter::MaxExclusions)
		{
			OutError = FString::Printf(TEXT("%d path exclusions; a scatter takes at most %d"), Params.PathExclusions.Num(), FMantleScatter::MaxExclusions);
			return false;
		}
		for (const FMantleScatterPathExclusion& Strip : Params.PathExclusions)
		{
			bool bPointsOk = Strip.Path.Num() >= 2 && Strip.Path.Num() <= FMantleScatter::MaxPathPoints;
			for (const FVector2D& Point : Strip.Path)
			{
				bPointsOk = bPointsOk && FMath::Abs(Point.X) <= MaxCoordinate && FMath::Abs(Point.Y) <= MaxCoordinate;
			}
			if (!bPointsOk || !(Strip.Width > 0.0 && Strip.Width <= FMantlePlaceLimits::MaxScatterPathWidth))
			{
				OutError = FString::Printf(TEXT("a path exclusion takes 2 to %d points within %g cm either way and a width above 0 and at most %g cm"),
					FMantleScatter::MaxPathPoints, MaxCoordinate, FMantlePlaceLimits::MaxScatterPathWidth);
				return false;
			}
		}
		if (Params.ClusterCount < 0 || Params.ClusterCount > FMantleScatter::MaxClusters
			|| (Params.ClusterCount > 0
				&& !(Params.ClusterRadius > 0.0 && Params.ClusterRadius <= FMantlePlaceLimits::MaxScatterClusterRadius)))
		{
			OutError = FString::Printf(TEXT("clusters must be 1 to %d of a radius above 0 and at most %g cm"), FMantleScatter::MaxClusters,
				FMantlePlaceLimits::MaxScatterClusterRadius);
			return false;
		}
		if (Params.Meshes.Num() == 0 || Params.Meshes.Num() > FMantleScatter::MaxMeshes)
		{
			OutError = FString::Printf(TEXT("%d meshes; a scatter takes 1 to %d"), Params.Meshes.Num(), FMantleScatter::MaxMeshes);
			return false;
		}
		int64 TotalWeight = 0;
		for (int32 Index = 0; Index < Params.Meshes.Num(); ++Index)
		{
			const FMantleScatterMesh& Mesh = Params.Meshes[Index];
			if (Mesh.MeshId.IsEmpty())
			{
				OutError = FString::Printf(TEXT("mesh %d has no id"), Index);
				return false;
			}
			if (Mesh.Weight < 0 || Mesh.Weight > FMantleScatter::MaxMeshWeight)
			{
				OutError = FString::Printf(TEXT("mesh %d (%s) has a weight of %d; a weight is 0 to %d"),
					Index, *Mesh.MeshId, Mesh.Weight, FMantleScatter::MaxMeshWeight);
				return false;
			}
			TotalWeight += Mesh.Weight;
		}
		if (TotalWeight <= 0)
		{
			OutError = TEXT("every mesh weighs 0; at least one must weigh more");
			return false;
		}
		return true;
	}

	/** Half a size in whole millimetres, rounded half up: the fixed point every count starts from. */
	int64 ScatterHalfMillimetres(double HalfCm)
	{
		// Two statements, not one expression, so no compiler fuses the multiply and the add into one
		// rounding: the backend and validate.py round each step, and the counts must agree exactly.
		const double Millimetres = HalfCm * 10.0;
		return static_cast<int64>(FMath::FloorToDouble(Millimetres + 0.5));
	}

	/** A 0 to 1 value in whole millionths, rounded half up, the multiply and the add as separate steps (see above). */
	int64 ScatterMillionths(double Value)
	{
		const double Millionths = Value * 1.0e6;
		return static_cast<int64>(FMath::FloorToDouble(Millionths + 0.5));
	}

	// Ground alignment (#188) is integer maths from end to end: the rounded surface normal in, hundredths of a degree out, with no
	// sin, cos, atan or sqrt from the C library, so the server, every client and the editor preview turn an instance the same bit for
	// bit. Vectors are in millionths (ScatterUnit is 1). sin, cos and atan2 are CORDIC (shifts and adds on a table of angles) and the
	// square root is an integer one.
	constexpr int64 ScatterUnit = 1000000;

	/** atan(2^-i) in millionths of a degree, the CORDIC table. */
	constexpr int64 GScatterCordicAngle[] = {
		45000000, 26565051, 14036243, 7125016, 3576334, 1789911, 895174, 447614, 223811, 111906, 55953, 27976, 13988, 6994,
		3497, 1749, 874, 437, 219, 109, 55, 27, 14, 7, 3, 2, 1 };
	constexpr int32 GScatterCordicSteps = static_cast<int32>(UE_ARRAY_COUNT(GScatterCordicAngle));
	/** The product of 1 / sqrt(1 + 2^-2i) over the steps, times 1e9: the start length that leaves a CORDIC rotation of length 1e9. */
	constexpr int64 GScatterCordicGain = 607252935;

	/** Value / Divisor rounded down (Divisor above 0), the same on every machine for a negative Value too. */
	int64 ScatterFloorDiv(const int64 Value, const int64 Divisor)
	{
		int64 Quotient = Value / Divisor;
		if (Value % Divisor < 0)
		{
			--Quotient;
		}
		return Quotient;
	}

	/** Value / Divisor rounded half up (Divisor above 0). */
	int64 ScatterDivRound(const int64 Value, const int64 Divisor)
	{
		return ScatterFloorDiv(2 * Value + Divisor, 2 * Divisor);
	}

	/** The square root of Value (0 or more) rounded down, one bit pair at a time. */
	int64 ScatterISqrt(const int64 Value)
	{
		if (Value <= 0)
		{
			return 0;
		}
		uint64 Remaining = static_cast<uint64>(Value);
		uint64 Result = 0;
		uint64 Bit = 1ull << 62;
		while (Bit > Remaining)
		{
			Bit >>= 2;
		}
		while (Bit != 0)
		{
			if (Remaining >= Result + Bit)
			{
				Remaining -= Result + Bit;
				Result = (Result >> 1) + Bit;
			}
			else
			{
				Result >>= 1;
			}
			Bit >>= 2;
		}
		return static_cast<int64>(Result);
	}

	/** The sine and cosine of an angle in hundredths of a degree (any whole number), each in millionths. */
	void ScatterSinCos(const int64 Hundredths, int64& OutSin, int64& OutCos)
	{
		// Millionths of a degree, wrapped to -180 to 180 and then to -90 to 90 (the CORDIC's reach), flipping both results past 90.
		int64 Angle = Hundredths * 10000;
		Angle = ((Angle % 360000000) + 360000000) % 360000000;
		if (Angle > 180000000)
		{
			Angle -= 360000000;
		}
		int64 Flip = 1;
		if (Angle > 90000000)
		{
			Angle -= 180000000;
			Flip = -1;
		}
		else if (Angle < -90000000)
		{
			Angle += 180000000;
			Flip = -1;
		}
		int64 X = GScatterCordicGain;
		int64 Y = 0;
		int64 Left = Angle;
		for (int32 Step = 0; Step < GScatterCordicSteps; ++Step)
		{
			const int64 ShiftedX = X >> Step;
			const int64 ShiftedY = Y >> Step;
			if (Left >= 0)
			{
				X -= ShiftedY;
				Y += ShiftedX;
				Left -= GScatterCordicAngle[Step];
			}
			else
			{
				X += ShiftedY;
				Y -= ShiftedX;
				Left += GScatterCordicAngle[Step];
			}
		}
		OutSin = Flip * ScatterDivRound(Y, 1000);
		OutCos = Flip * ScatterDivRound(X, 1000);
	}

	/** atan2(Y, X) in whole hundredths of a degree, above -18000 and at most 18000; 0 for (0, 0). Any size of Y and X. */
	int64 ScatterAtan2(int64 Y, int64 X)
	{
		if (X == 0 && Y == 0)
		{
			return 0;
		}
		// Both scaled so the larger is about 2^40: a left shift loses nothing, a right one only bits far below the angle's resolution.
		const uint64 Larger = static_cast<uint64>(FMath::Max(FMath::Abs(X), FMath::Abs(Y)));
		const int32 Shift = 40 - (64 - static_cast<int32>(FMath::CountLeadingZeros64(Larger)));
		if (Shift >= 0)
		{
			X *= (1ll << Shift);
			Y *= (1ll << Shift);
		}
		else
		{
			X >>= -Shift;
			Y >>= -Shift;
		}
		// The CORDIC turns a vector in the right half onto the X axis; a vector in the left half is flipped there first.
		int64 Total = 0;
		if (X < 0)
		{
			X = -X;
			Y = -Y;
			Total = 180000000;
		}
		for (int32 Step = 0; Step < GScatterCordicSteps; ++Step)
		{
			const int64 ShiftedX = X >> Step;
			const int64 ShiftedY = Y >> Step;
			if (Y > 0)
			{
				X += ShiftedY;
				Y -= ShiftedX;
				Total += GScatterCordicAngle[Step];
			}
			else
			{
				X -= ShiftedY;
				Y += ShiftedX;
				Total -= GScatterCordicAngle[Step];
			}
		}
		int64 Hundredths = ScatterDivRound(Total, 10000);
		if (Hundredths > 18000)
		{
			Hundredths -= 36000;
		}
		else if (Hundredths <= -18000)
		{
			Hundredths += 36000;
		}
		return Hundredths;
	}

	/** A direction, in millionths. */
	struct FScatterAxis
	{
		int64 X = 0;
		int64 Y = 0;
		int64 Z = 0;
	};

	/** The forward (local X) and up (local Z) axes of FRotator(pitch, yaw, roll), each angle in hundredths of a degree: FRotationMatrix's first and third rows. */
	void ScatterAxes(const int64 Pitch, const int64 Yaw, const int64 Roll, FScatterAxis& OutForward, FScatterAxis& OutUp)
	{
		int64 SinPitch, CosPitch, SinYaw, CosYaw, SinRoll, CosRoll;
		ScatterSinCos(Pitch, SinPitch, CosPitch);
		ScatterSinCos(Yaw, SinYaw, CosYaw);
		ScatterSinCos(Roll, SinRoll, CosRoll);
		const int64 SinPitchCosYaw = ScatterDivRound(SinPitch * CosYaw, ScatterUnit);
		const int64 SinPitchSinYaw = ScatterDivRound(SinPitch * SinYaw, ScatterUnit);
		OutForward.X = ScatterDivRound(CosPitch * CosYaw, ScatterUnit);
		OutForward.Y = ScatterDivRound(CosPitch * SinYaw, ScatterUnit);
		OutForward.Z = SinPitch;
		OutUp.X = -ScatterDivRound(CosRoll * SinPitchCosYaw, ScatterUnit) - ScatterDivRound(SinRoll * SinYaw, ScatterUnit);
		OutUp.Y = ScatterDivRound(CosYaw * SinRoll, ScatterUnit) - ScatterDivRound(CosRoll * SinPitchSinYaw, ScatterUnit);
		OutUp.Z = ScatterDivRound(CosRoll * CosPitch, ScatterUnit);
	}

	/**
	 * Vector after the turn that takes straight up onto the unit normal Normal (its up component 0 or more): the shortest turn, about
	 * the horizontal axis across the slope. Written without a division by the slope's length, so a flat normal needs no special case.
	 */
	FScatterAxis ScatterTurnToNormal(const FScatterAxis& Normal, const FScatterAxis& Vector)
	{
		const int64 Across = ScatterDivRound(Normal.X * Vector.X + Normal.Y * Vector.Y, ScatterUnit);
		const int64 OnePlusUp = ScatterUnit + Normal.Z;
		FScatterAxis Out;
		Out.X = Vector.X - ScatterDivRound(Normal.X * Across, OnePlusUp) + ScatterDivRound(Normal.X * Vector.Z, ScatterUnit);
		Out.Y = Vector.Y - ScatterDivRound(Normal.Y * Across, OnePlusUp) + ScatterDivRound(Normal.Y * Vector.Z, ScatterUnit);
		Out.Z = -Across + ScatterDivRound(Normal.Z * Vector.Z, ScatterUnit);
		return Out;
	}

	/**
	 * Ground alignment (#188). Yaw, Pitch and Roll (hundredths of a degree) are the instance's turn as if it stood on flat ground; this
	 * replaces them with that turn followed by the shortest turn that takes up onto the surface normal (NormalX/Y/Z in millionths of the
	 * scatter's space, any length). The instance's up ends on the normal, with the random yaw as its turn about it and the tilt as its
	 * lean off it. A normal that is straight up (or has no length) leaves the angles exactly as they were.
	 */
	void ScatterAlignToGround(const int64 NormalX, const int64 NormalY, const int64 NormalZ, uint16& Yaw, int16& Pitch, int16& Roll)
	{
		FScatterAxis Normal;
		Normal.X = NormalX;
		Normal.Y = NormalY;
		Normal.Z = FMath::Max<int64>(NormalZ, 0);
		const int64 Length = ScatterISqrt(Normal.X * Normal.X + Normal.Y * Normal.Y + Normal.Z * Normal.Z);
		if (Length <= 0)
		{
			return;
		}
		Normal.X = ScatterDivRound(Normal.X * ScatterUnit, Length);
		Normal.Y = ScatterDivRound(Normal.Y * ScatterUnit, Length);
		Normal.Z = ScatterDivRound(Normal.Z * ScatterUnit, Length);
		if (Normal.X == 0 && Normal.Y == 0)
		{
			return;
		}

		FScatterAxis Forward;
		FScatterAxis Up;
		ScatterAxes(Pitch, Yaw, Roll, Forward, Up);
		Forward = ScatterTurnToNormal(Normal, Forward);
		Up = ScatterTurnToNormal(Normal, Up);

		// Back to yaw, pitch and roll: yaw and pitch from the forward axis, roll from where up sits between the roll-free up and side axes.
		const int64 Flat = ScatterISqrt(Forward.X * Forward.X + Forward.Y * Forward.Y);
		const int64 Whole = ScatterISqrt(Flat * Flat + Forward.Z * Forward.Z);
		int64 NewYaw = 0;
		int64 CosYaw = ScatterUnit;
		int64 SinYaw = 0;
		if (Flat > 0)
		{
			NewYaw = ScatterAtan2(Forward.Y, Forward.X);
			CosYaw = ScatterDivRound(Forward.X * ScatterUnit, Flat);
			SinYaw = ScatterDivRound(Forward.Y * ScatterUnit, Flat);
		}
		const int64 NewPitch = ScatterAtan2(Forward.Z, Flat);
		const int64 SinPitch = Whole > 0 ? ScatterDivRound(Forward.Z * ScatterUnit, Whole) : 0;
		const int64 CosPitch = Whole > 0 ? ScatterDivRound(Flat * ScatterUnit, Whole) : ScatterUnit;
		// Roll-free side axis (-sin yaw, cos yaw, 0) and up axis (-sin pitch cos yaw, -sin pitch sin yaw, cos pitch); up = cos roll x the second + sin roll x the first.
		const int64 Side = ScatterDivRound(-SinYaw * Up.X + CosYaw * Up.Y, ScatterUnit);
		const int64 Lift = ScatterDivRound(-ScatterDivRound(SinPitch * CosYaw, ScatterUnit) * Up.X - ScatterDivRound(SinPitch * SinYaw, ScatterUnit) * Up.Y
			+ CosPitch * Up.Z, ScatterUnit);
		int64 NewRoll = ScatterAtan2(Side, Lift);
		if (NewRoll == 18000)
		{
			NewRoll = -18000;
		}
		Yaw = static_cast<uint16>(((NewYaw % 36000) + 36000) % 36000);
		Pitch = static_cast<int16>(FMath::Clamp<int64>(NewPitch, -9000, 9000));
		Roll = static_cast<int16>(NewRoll);
	}

	/** A path in whole millimetres: its points, each segment's length rounded to whole mm (a segment that rounds to 0 is skipped), and the strip's width. */
	struct FScatterPath
	{
		TArray<int64> X;
		TArray<int64> Y;
		/** Segment i runs from point i to point i + 1 and is Length[i] mm long (at least 1). */
		TArray<int32> Segment;
		TArray<int64> Length;
		/** Running total of Length: segment k covers [Cumulative[k] - Length[k], Cumulative[k]). */
		TArray<int64> Cumulative;
		int64 Total = 0;
		int64 WidthMm = 0;
	};

	FScatterPath ScatterBuildPath(const FMantleScatterParams& Params)
	{
		FScatterPath Path;
		for (const FVector2D& Point : Params.Path)
		{
			Path.X.Add(ScatterHalfMillimetres(Point.X));
			Path.Y.Add(ScatterHalfMillimetres(Point.Y));
		}
		Path.WidthMm = ScatterHalfMillimetres(Params.Width);
		for (int32 Index = 0; Index + 1 < Path.X.Num(); ++Index)
		{
			const int64 Dx = Path.X[Index + 1] - Path.X[Index];
			const int64 Dy = Path.Y[Index + 1] - Path.Y[Index];
			// The squared length is exact in a double (under 2^53) and sqrt is correctly rounded, so the length is the same everywhere.
			const double Exact = FMath::Sqrt(static_cast<double>(Dx * Dx + Dy * Dy));
			const int64 Length = static_cast<int64>(FMath::FloorToDouble(Exact + 0.5));
			if (Length > 0)
			{
				Path.Total += Length;
				Path.Segment.Add(Index);
				Path.Length.Add(Length);
				Path.Cumulative.Add(Path.Total);
			}
		}
		return Path;
	}

	/** A point in whole millimetres. */
	struct FScatterPoint
	{
		int64 X = 0;
		int64 Y = 0;
	};

	/** A box in whole millimetres. */
	struct FScatterBoxMm
	{
		int64 CenterX = 0;
		int64 CenterY = 0;
		int64 HalfX = 0;
		int64 HalfY = 0;
	};

	/** A path exclusion (#190) in whole millimetres: its points, each segment's rounded length, the strip's half width and the box around it. */
	struct FScatterStripMm
	{
		TArray<int64> X;
		TArray<int64> Y;
		TArray<int64> Length;
		int64 Radius = 0;
		int64 MinX = 0;
		int64 MaxX = 0;
		int64 MinY = 0;
		int64 MaxY = 0;
	};

	FScatterStripMm ScatterBuildStrip(const FMantleScatterPathExclusion& Strip)
	{
		FScatterStripMm Out;
		const double HalfWidth = Strip.Width * 0.5;
		Out.Radius = ScatterHalfMillimetres(HalfWidth);
		for (const FVector2D& Point : Strip.Path)
		{
			Out.X.Add(ScatterHalfMillimetres(Point.X));
			Out.Y.Add(ScatterHalfMillimetres(Point.Y));
		}
		Out.MinX = Out.MaxX = Out.X[0];
		Out.MinY = Out.MaxY = Out.Y[0];
		for (int32 Index = 0; Index < Out.X.Num(); ++Index)
		{
			Out.MinX = FMath::Min(Out.MinX, Out.X[Index]);
			Out.MaxX = FMath::Max(Out.MaxX, Out.X[Index]);
			Out.MinY = FMath::Min(Out.MinY, Out.Y[Index]);
			Out.MaxY = FMath::Max(Out.MaxY, Out.Y[Index]);
			if (Index + 1 < Out.X.Num())
			{
				const int64 Dx = Out.X[Index + 1] - Out.X[Index];
				const int64 Dy = Out.Y[Index + 1] - Out.Y[Index];
				Out.Length.Add(static_cast<int64>(FMath::FloorToDouble(FMath::Sqrt(static_cast<double>(Dx * Dx + Dy * Dy)) + 0.5)));
			}
		}
		Out.MinX -= Out.Radius;
		Out.MaxX += Out.Radius;
		Out.MinY -= Out.Radius;
		Out.MaxY += Out.Radius;
		return Out;
	}

	/**
	 * Whether (X, Y) mm lies within the strip's half width of its polyline. All whole numbers: a point past a segment's end is measured
	 * to that end point, a point beside it by its cross product over the segment's rounded length, rounded half up.
	 */
	bool ScatterInStrip(const FScatterStripMm& Strip, const int64 X, const int64 Y)
	{
		if (X < Strip.MinX || X > Strip.MaxX || Y < Strip.MinY || Y > Strip.MaxY)
		{
			return false;
		}
		const int64 RadiusSquared = Strip.Radius * Strip.Radius;
		for (int32 Index = 0; Index + 1 < Strip.X.Num(); ++Index)
		{
			const int64 Ax = Strip.X[Index];
			const int64 Ay = Strip.Y[Index];
			const int64 Dx = Strip.X[Index + 1] - Ax;
			const int64 Dy = Strip.Y[Index + 1] - Ay;
			const int64 Px = X - Ax;
			const int64 Py = Y - Ay;
			const int64 Along = Px * Dx + Py * Dy;
			if (Along <= 0 || Strip.Length[Index] == 0)
			{
				if (Px * Px + Py * Py <= RadiusSquared)
				{
					return true;
				}
			}
			else if (Along >= Dx * Dx + Dy * Dy)
			{
				const int64 Ex = X - Strip.X[Index + 1];
				const int64 Ey = Y - Strip.Y[Index + 1];
				if (Ex * Ex + Ey * Ey <= RadiusSquared)
				{
					return true;
				}
			}
			else if (ScatterDivRound(FMath::Abs(Px * Dy - Py * Dx), Strip.Length[Index]) <= Strip.Radius)
			{
				return true;
			}
		}
		return false;
	}

	/** Instances kept so far, by cell (a square of the minimum spacing), for the spacing test (#190). */
	struct FScatterSpacingGrid
	{
		int64 Spacing = 0;
		TMap<uint64, TArray<FIntPoint, TInlineAllocator<4>>> Cells;

		static uint64 Key(const int64 CellX, const int64 CellY)
		{
			return (static_cast<uint64>(static_cast<uint32>(CellX)) << 32) | static_cast<uint64>(static_cast<uint32>(CellY));
		}

		/** Whether a kept instance lies closer than Spacing to (X, Y) mm: the same cell or one of the eight around it. */
		bool IsTooClose(const int64 X, const int64 Y) const
		{
			const int64 CellX = ScatterFloorDiv(X, Spacing);
			const int64 CellY = ScatterFloorDiv(Y, Spacing);
			for (int64 StepX = -1; StepX <= 1; ++StepX)
			{
				for (int64 StepY = -1; StepY <= 1; ++StepY)
				{
					const TArray<FIntPoint, TInlineAllocator<4>>* Kept = Cells.Find(Key(CellX + StepX, CellY + StepY));
					if (!Kept)
					{
						continue;
					}
					for (const FIntPoint& Other : *Kept)
					{
						// Far apart on one axis is far apart: this also keeps the squares below within 64 bits.
						const int64 Dx = FMath::Abs(X - Other.X);
						const int64 Dy = FMath::Abs(Y - Other.Y);
						if (Dx < Spacing && Dy < Spacing && Dx * Dx + Dy * Dy < Spacing * Spacing)
						{
							return true;
						}
					}
				}
			}
			return false;
		}

		void Add(const int64 X, const int64 Y)
		{
			Cells.FindOrAdd(Key(ScatterFloorDiv(X, Spacing), ScatterFloorDiv(Y, Spacing))).Add(FIntPoint(static_cast<int32>(X), static_cast<int32>(Y)));
		}
	};

	/** A point on a disc of the given radius (mm): the stream's draw first, then more of its draws until one lands inside. Integers only. */
	void ScatterOnDisc(const int64 Radius, const uint64 Draw, FScatterStream& Stream, int64& OutX, int64& OutY)
	{
		const int64 RadiusSquared = Radius * Radius;
		uint64 Current = Draw;
		for (int32 Try = 0; Try < 32; ++Try)
		{
			const int64 X = ScatterPick(ScatterLow(Current), -Radius, Radius);
			const int64 Y = ScatterPick(ScatterHigh(Current), -Radius, Radius);
			if (X * X + Y * Y <= RadiusSquared)
			{
				OutX = X;
				OutY = Y;
				return;
			}
			Current = Stream.Next();
		}
		OutX = 0;
		OutY = 0;
	}

	/**
	 * A point in the shape (X-Y, mm about the scatter's origin) from one 64-bit draw, plus the stream's later draws where it has to retry.
	 * On a path, ForcedAlong (0 or more, mm from the start; clamped to the path's length) puts the point there instead of at the draw's
	 * place, and OutTangentYaw, when given, gets the heading of the segment it lies on in hundredths of a degree, 0 to 35999 (#190).
	 */
	void ScatterInShape(const FMantleScatterParams& Params, const FScatterPath& Path, const int64 HalfX, const int64 HalfY,
		const int64 RadiusMm, const uint64 Draw, FScatterStream& Stream, int64& OutX, int64& OutY, const int64 ForcedAlong = -1,
		int64* OutTangentYaw = nullptr)
	{
		switch (Params.Shape)
		{
		case EMantleScatterShape::Disc:
			ScatterOnDisc(RadiusMm, Draw, Stream, OutX, OutY);
			return;
		case EMantleScatterShape::Path:
		{
			if (Path.Total <= 0)
			{
				OutX = Path.X.Num() > 0 ? Path.X[0] : 0;
				OutY = Path.Y.Num() > 0 ? Path.Y[0] : 0;
				if (OutTangentYaw)
				{
					*OutTangentYaw = 0;
				}
				return;
			}
			const int64 Along = ForcedAlong >= 0 ? FMath::Min(ForcedAlong, Path.Total) : ScatterPick(ScatterLow(Draw), 0, Path.Total - 1);
			int32 Pick = 0;
			while (Pick < Path.Cumulative.Num() - 1 && Along >= Path.Cumulative[Pick])
			{
				++Pick;
			}
			const int32 From = Path.Segment[Pick];
			const int64 Length = Path.Length[Pick];
			const int64 Offset = Along - (Path.Cumulative[Pick] - Length);
			const int64 Dx = Path.X[From + 1] - Path.X[From];
			const int64 Dy = Path.Y[From + 1] - Path.Y[From];
			const int64 Across = ScatterPick(ScatterHigh(Draw), 0, Path.WidthMm) - Path.WidthMm / 2;
			// Integer division truncates toward zero on every machine.
			OutX = Path.X[From] + Dx * Offset / Length - Dy * Across / Length;
			OutY = Path.Y[From] + Dy * Offset / Length + Dx * Across / Length;
			if (OutTangentYaw)
			{
				*OutTangentYaw = ((ScatterAtan2(Dy, Dx) % 36000) + 36000) % 36000;
			}
			return;
		}
		case EMantleScatterShape::Box:
		default:
			OutX = ScatterPick(ScatterLow(Draw), -HalfX, HalfX);
			OutY = ScatterPick(ScatterHigh(Draw), -HalfY, HalfY);
			return;
		}
	}

	/**
	 * Everything an instance's index turns into, split in two so a cell can be read without generating the whole stream (#189): Place is
	 * the instance's X-Y (and where an exclusion drops it), Finish is every other filter and the rest of the instance, minimum spacing
	 * excepted (spacing needs the instances before it). Place and Finish each start the instance's own stream afresh, so an instance never
	 * depends on another. Generate runs both for every index in order, so it is the same as it was before cells.
	 */
	struct FScatterPipeline
	{
		const FMantleScatterParams& Params;
		const FMantleScatterGenerateOptions& Options;

		int64 HalfX = 0;
		int64 HalfY = 0;
		int64 HalfZ = 0;
		int64 RadiusMm = 0;
		int64 ScaleLo = 0;
		int64 ScaleHi = 0;
		int64 Tilt = 0;
		FScatterPath Path;

		bool bTerrain = false;
		bool bSlopeFilter = false;
		bool bMinSlopeFilter = false;
		bool bAlignToGround = false;
		// The filters compare whole numbers: the normal's up component and the threshold (the cosine of the limit) each rounded to
		// millionths, heights in whole millimetres, so a last bit of float maths on another machine cannot flip a decision.
		int64 SlopeLimitMicro = 0;
		// The minimum slope (#188) is the same test the other way round: kept where the up component is at most the cosine of the limit.
		int64 MinSlopeLimitMicro = 0;
		// The paint-layer filter (#187): the weight is the weightmap's byte (0 to 255), the thresholds millionths, and the test
		// is byte x 1,000,000 against threshold x 255, all whole numbers.
		bool bLayerFilter = false;
		bool bMinLayer = false;
		bool bMaxLayer = false;
		int64 MinLayerMicro = 0;
		int64 MaxLayerMicro = 0;
		bool bMinHeight = false;
		bool bMaxHeight = false;
		int64 MinHeightMm = 0;
		int64 MaxHeightMm = 0;

		TArray<FScatterBoxMm, TInlineAllocator<FMantleScatter::MaxExclusions>> Exclusions;
		TArray<FScatterStripMm> Strips;

		/** The minimum spacing (#190) in whole mm, 0 for none, and the path step (#190) in whole mm, 0 for random places along the path. */
		int64 SpacingMm = 0;
		int64 StepMm = 0;

		/**
		 * The share of candidates kept, in millionths (1,000,000 for all). A Scatter built in cells (#189) whose spacing is wide for its density
		 * would make every cell check thousands of neighbours, so it keeps about ScatterSpacingCandidates candidates per spacing square, each picked
		 * by its own hash: the same on every machine, and the same for a cell and the whole build. The spacing then fills the same as it would with all.
		 */
		int64 ThinKeepMicro = 1000000;

		/** Cluster centres: each from its own stream, numbered past any instance's so the two never share one. */
		TArray<FScatterPoint> Centres;
		int64 ClusterRadiusMm = 0;

		/** Cumulative weights: a draw below Cumulative[i] and at or above Cumulative[i - 1] picks mesh i, so a weight of 0 is never picked. */
		TArray<int64, TInlineAllocator<FMantleScatter::MaxMeshes>> Cumulative;
		int64 TotalWeight = 0;

		FScatterPipeline(const FMantleScatterParams& InParams, const FMantleScatterGenerateOptions& InOptions)
			: Params(InParams)
			, Options(InOptions)
		{
			HalfX = ScatterHalfMillimetres(Params.HalfExtent.X);
			HalfY = ScatterHalfMillimetres(Params.HalfExtent.Y);
			HalfZ = ScatterHalfMillimetres(Params.HalfExtent.Z);
			RadiusMm = ScatterHalfMillimetres(Params.Radius);
			ScaleLo = FMath::Clamp<int64>(FMath::RoundToInt64(Params.MinScale * 1000.0), 1, MAX_uint16);
			ScaleHi = FMath::Clamp<int64>(FMath::RoundToInt64(Params.MaxScale * 1000.0), ScaleLo, MAX_uint16);
			Tilt = FMath::Clamp<int64>(FMath::RoundToInt64(Params.MaxTiltDegrees * 100.0), 0, 9000);
			if (Params.Shape == EMantleScatterShape::Path)
			{
				Path = ScatterBuildPath(Params);
			}

			bTerrain = Params.Placement == EMantleScatterPlacement::Terrain;
			bSlopeFilter = bTerrain && Params.MaxSlopeDegrees < 90.0;
			bMinSlopeFilter = bTerrain && Params.MinSlopeDegrees > 0.0;
			bAlignToGround = bTerrain && Params.Alignment == EMantleScatterAlignment::Ground;
			SlopeLimitMicro = ScatterMillionths(FMath::Cos(FMath::DegreesToRadians(Params.MaxSlopeDegrees)));
			MinSlopeLimitMicro = ScatterMillionths(FMath::Cos(FMath::DegreesToRadians(Params.MinSlopeDegrees)));
			bLayerFilter = bTerrain && Params.PaintLayer >= 0;
			bMinLayer = bLayerFilter && Params.MinLayerWeight.IsSet();
			bMaxLayer = bLayerFilter && Params.MaxLayerWeight.IsSet();
			MinLayerMicro = bMinLayer ? ScatterMillionths(Params.MinLayerWeight.GetValue()) : 0;
			MaxLayerMicro = bMaxLayer ? ScatterMillionths(Params.MaxLayerWeight.GetValue()) : 0;
			bMinHeight = Params.MinHeight.IsSet();
			bMaxHeight = Params.MaxHeight.IsSet();
			MinHeightMm = bMinHeight ? ScatterHalfMillimetres(Params.MinHeight.GetValue()) : 0;
			MaxHeightMm = bMaxHeight ? ScatterHalfMillimetres(Params.MaxHeight.GetValue()) : 0;

			for (const FMantleScatterExclusion& Exclusion : Params.Exclusions)
			{
				FScatterBoxMm& Box = Exclusions.AddDefaulted_GetRef();
				Box.CenterX = ScatterHalfMillimetres(Exclusion.Center.X);
				Box.CenterY = ScatterHalfMillimetres(Exclusion.Center.Y);
				Box.HalfX = ScatterHalfMillimetres(Exclusion.HalfExtent.X);
				Box.HalfY = ScatterHalfMillimetres(Exclusion.HalfExtent.Y);
			}
			for (const FMantleScatterPathExclusion& Strip : Params.PathExclusions)
			{
				Strips.Add(ScatterBuildStrip(Strip));
			}

			SpacingMm = Params.MinSpacing > 0.0 ? FMath::Max<int64>(ScatterHalfMillimetres(Params.MinSpacing), 1) : 0;
			StepMm = Params.Shape == EMantleScatterShape::Path && Params.PathStep > 0.0 ? FMath::Max<int64>(ScatterHalfMillimetres(Params.PathStep), 1) : 0;
			if (SpacingMm > 0)
			{
				FString Ignored;
				if (FMantleScatter::CountFor(Params, Ignored) > FMantleScatter::WholeInstances)
				{
					// Each step is its own statement and IEEE operation, so every machine rounds the share the same.
					const double SpacingM = static_cast<double>(SpacingMm) / 1000.0;
					const double SquareM2 = SpacingM * SpacingM;
					const double PerSquare = Params.Density * SquareM2;
					const double Target = static_cast<double>(FMantlePlaceLimits::ScatterSpacingCandidates);
					if (PerSquare > Target)
					{
						const double Share = Target * 1000000.0;
						const double Kept = FMath::FloorToDouble(Share / PerSquare);
						ThinKeepMicro = FMath::Clamp<int64>(static_cast<int64>(Kept), 1, 999999);
					}
				}
			}

			ClusterRadiusMm = ScatterHalfMillimetres(Params.ClusterRadius);
			for (int32 Cluster = 0; Cluster < Params.ClusterCount; ++Cluster)
			{
				FScatterStream Stream(Params.Seed, 0x80000000u + static_cast<uint32>(Cluster));
				FScatterPoint& Centre = Centres.AddDefaulted_GetRef();
				ScatterInShape(Params, Path, HalfX, HalfY, RadiusMm, Stream.Next(), Stream, Centre.X, Centre.Y);
			}

			for (const FMantleScatterMesh& Mesh : Params.Meshes)
			{
				TotalWeight += Mesh.Weight;
				Cumulative.Add(TotalWeight);
			}
		}

		/** Instance Index's X-Y in whole mm and, along a stepped path, the heading there; false where an exclusion drops it. */
		bool Place(const int64 Index, int64& OutX, int64& OutY, int64& OutTangentYaw) const
		{
			// A thinned Scatter drops a candidate by its own hash first, from a stream numbered clear of the instances' and the clusters' (#189).
			if (ThinKeepMicro < 1000000)
			{
				FScatterStream Thin(Params.Seed, 0x40000000u + static_cast<uint32>(Index));
				if (Thin.Next() % 1000000u >= static_cast<uint64>(ThinKeepMicro))
				{
					return false;
				}
			}
			// Five 64-bit draws per instance, always all five, so changing the alignment, the scale
			// range, the meshes or the shape never moves the rest of an instance. A retry (a disc's) takes
			// later draws of the same stream.
			FScatterStream Stream(Params.Seed, static_cast<uint32>(Index));
			const uint64 PlaceDraw = Stream.Next();
			Stream.Next();
			Stream.Next();
			Stream.Next();
			const uint64 Group = Stream.Next();

			int64 X = 0;
			int64 Y = 0;
			int64 TangentYaw = 0;
			if (Centres.Num() > 0)
			{
				const FScatterPoint& Centre = Centres[static_cast<int32>(ScatterPick(ScatterLow(Group), 0, Centres.Num() - 1))];
				int64 OffsetX = 0;
				int64 OffsetY = 0;
				ScatterOnDisc(ClusterRadiusMm, PlaceDraw, Stream, OffsetX, OffsetY);
				X = Centre.X + OffsetX;
				Y = Centre.Y + OffsetY;
			}
			else if (StepMm > 0)
			{
				// Instance Index sits Index steps along the path, turned along the line (#190).
				ScatterInShape(Params, Path, HalfX, HalfY, RadiusMm, PlaceDraw, Stream, X, Y, Index * StepMm, &TangentYaw);
			}
			else
			{
				ScatterInShape(Params, Path, HalfX, HalfY, RadiusMm, PlaceDraw, Stream, X, Y);
			}
			OutX = X;
			OutY = Y;
			OutTangentYaw = TangentYaw;

			for (const FScatterStripMm& Strip : Strips)
			{
				if (ScatterInStrip(Strip, X, Y))
				{
					return false;
				}
			}
			for (const FScatterBoxMm& Box : Exclusions)
			{
				if (FMath::Abs(X - Box.CenterX) <= Box.HalfX && FMath::Abs(Y - Box.CenterY) <= Box.HalfY)
				{
					return false;
				}
			}
			return true;
		}

		/**
		 * The rest of instance Index, placed at X, Y by Place: the terrain, slope, height, layer and range filters, then the instance's height,
		 * turn, scale and mesh. False where a filter drops it. The minimum spacing is not applied here.
		 */
		bool Finish(const int64 Index, const int64 X, const int64 Y, const int64 TangentYaw, FMantleScatterInstance& Out) const
		{
			FScatterStream Stream(Params.Seed, static_cast<uint32>(Index));
			Stream.Next();
			const uint64 Height = Stream.Next();
			const uint64 Turn = Stream.Next();
			const uint64 Look = Stream.Next();

			int64 Z = ScatterPick(ScatterLow(Height), -HalfZ, HalfZ);
			// The surface normal Ground alignment follows, in millionths of the scatter's space (up until a terrain says otherwise).
			int64 AlignX = 0;
			int64 AlignY = 0;
			int64 AlignZ = ScatterUnit;
			if (bTerrain)
			{
				FMantleScatterGround Ground;
				if (!Options.Ground || !Options.Ground(X, Y, Ground))
				{
					return false;
				}
				if (bSlopeFilter)
				{
					if (ScatterMillionths(Ground.NormalZ) < SlopeLimitMicro)
					{
						return false;
					}
				}
				if (bMinSlopeFilter)
				{
					if (ScatterMillionths(Ground.NormalZ) > MinSlopeLimitMicro)
					{
						return false;
					}
				}
				if (bMinHeight || bMaxHeight)
				{
					const int64 HeightMm = ScatterHalfMillimetres(Ground.WorldZCm);
					if ((bMinHeight && HeightMm < MinHeightMm) || (bMaxHeight && HeightMm > MaxHeightMm))
					{
						return false;
					}
				}
				if (bLayerFilter)
				{
					const int64 Scaled = static_cast<int64>(FMath::Clamp(Ground.LayerWeight, 0, 255)) * 1000000;
					if ((bMinLayer && Scaled < MinLayerMicro * 255) || (bMaxLayer && Scaled > MaxLayerMicro * 255))
					{
						return false;
					}
				}
				Z += Ground.ZMm;
				if (bAlignToGround)
				{
					AlignX = ScatterMillionths(Ground.LocalNormalX);
					AlignY = ScatterMillionths(Ground.LocalNormalY);
					AlignZ = ScatterMillionths(Ground.LocalNormalZ);
				}
			}
			// The fixed point is 32-bit millimetres (2147 km): a position past it cannot be a place's.
			if (FMath::Abs(X) > MAX_int32 || FMath::Abs(Y) > MAX_int32 || FMath::Abs(Z) > MAX_int32)
			{
				return false;
			}

			Out = FMantleScatterInstance();
			Out.X = static_cast<int32>(X);
			Out.Y = static_cast<int32>(Y);
			Out.Z = static_cast<int32>(Z);
			Out.Yaw = static_cast<uint16>(StepMm > 0 ? TangentYaw : ScatterPick(ScatterHigh(Height), 0, 35999));
			switch (Params.Alignment)
			{
			case EMantleScatterAlignment::Tilted:
			case EMantleScatterAlignment::Ground:
				Out.Pitch = static_cast<int16>(ScatterPick(ScatterLow(Turn), -Tilt, Tilt));
				Out.Roll = static_cast<int16>(ScatterPick(ScatterHigh(Turn), -Tilt, Tilt));
				// Ground leans like Tilted, then the whole turn is carried onto the slope (#188).
				if (bAlignToGround)
				{
					ScatterAlignToGround(AlignX, AlignY, AlignZ, Out.Yaw, Out.Pitch, Out.Roll);
				}
				break;
			case EMantleScatterAlignment::Tumbled:
				Out.Pitch = static_cast<int16>(ScatterPick(ScatterLow(Turn), -9000, 9000));
				Out.Roll = static_cast<int16>(ScatterPick(ScatterHigh(Turn), -18000, 17999));
				break;
			case EMantleScatterAlignment::Upright:
			default:
				Out.Pitch = 0;
				Out.Roll = 0;
				break;
			}
			Out.Scale = static_cast<uint16>(ScatterPick(ScatterLow(Look), ScaleLo, ScaleHi));

			const int64 Pick = ScatterPick(ScatterHigh(Look), 0, TotalWeight - 1);
			int32 MeshIndex = 0;
			while (MeshIndex < Cumulative.Num() - 1 && Pick >= Cumulative[MeshIndex])
			{
				++MeshIndex;
			}
			Out.Mesh = static_cast<uint16>(MeshIndex);
			return true;
		}
	};
}

const TCHAR* FMantleScatter::AlignmentName(const EMantleScatterAlignment Alignment)
{
	const int32 Index = static_cast<int32>(Alignment);
	return Index >= 0 && Index < MantleScatterPrivate::GScatterAlignmentCount
		? MantleScatterPrivate::GScatterAlignmentNames[Index]
		: TEXT("upright");
}

bool FMantleScatter::ParseAlignment(const FString& Name, EMantleScatterAlignment& Out)
{
	for (int32 Index = 0; Index < MantleScatterPrivate::GScatterAlignmentCount; ++Index)
	{
		if (Name.Equals(MantleScatterPrivate::GScatterAlignmentNames[Index], ESearchCase::CaseSensitive))
		{
			Out = static_cast<EMantleScatterAlignment>(Index);
			return true;
		}
	}
	return false;
}

int64 FMantleScatter::CountFor(const FMantleScatterParams& Params, FString& OutError)
{
	using namespace MantleScatterPrivate;

	if (!ValidateScatterParams(Params, OutError))
	{
		return -1;
	}

	// The footprint in square metres, from whole millimetres. Each step is its own statement and its own IEEE
	// operation (no fused multiply-add), exactly as validate.py and the backend do them, so all three agree on every count.
	double AreaM2 = 0.0;
	switch (Params.Shape)
	{
	case EMantleScatterShape::Disc:
	{
		const int64 Radius = ScatterHalfMillimetres(Params.Radius);
		const double RadiusSquared = static_cast<double>(Radius * Radius);
		const double Scaled = RadiusSquared * 3141593.0;
		const double AreaMm2 = Scaled / 1000000.0;
		AreaM2 = AreaMm2 / 1000000.0;
		break;
	}
	case EMantleScatterShape::Path:
	{
		const FScatterPath Path = ScatterBuildPath(Params);
		if (Params.PathStep > 0.0)
		{
			// A fixed step (#190): the whole steps that fit, plus the one at the start; the density is not used.
			const int64 Steps = Path.Total / FMath::Max<int64>(ScatterHalfMillimetres(Params.PathStep), 1) + 1;
			// A stepped path is built whole, never in cells, so it keeps the whole-Scatter cap (#189).
			if (Steps > WholeInstances)
			{
				OutError = FString::Printf(TEXT("a path step of %g cm makes %lld instances along the path; a scatter makes at most %d"),
					Params.PathStep, static_cast<long long>(Steps), WholeInstances);
				return -1;
			}
			return Steps;
		}
		const double AreaMm2 = static_cast<double>(Path.Total * Path.WidthMm);
		AreaM2 = AreaMm2 / 1000000.0;
		break;
	}
	case EMantleScatterShape::Box:
	default:
	{
		// The footprint in whole square millimetres (exact in a double up to the extent cap), then
		// square metres by one division, so 100 m x 100 m at density 1 is 10000.
		const int64 AreaMm2 = 4 * ScatterHalfMillimetres(Params.HalfExtent.X) * ScatterHalfMillimetres(Params.HalfExtent.Y);
		AreaM2 = static_cast<double>(AreaMm2) / 1.0e6;
		break;
	}
	}
	const double Wanted = AreaM2 * Params.Density;
	const double Rounded = FMath::FloorToDouble(Wanted + 0.5);
	if (!(Rounded <= static_cast<double>(MaxInstances)))
	{
		OutError = FString::Printf(TEXT("%.0f square metres at density %g makes %.0f instances; a scatter makes at most %d"),
			AreaM2, Params.Density, Rounded, MaxInstances);
		return -1;
	}
	// More than a Scatter built whole holds is built in cells around each camera, which needs to know how far (#189).
	if (Rounded > static_cast<double>(WholeInstances) && !(Params.DrawDistance > 0.0))
	{
		OutError = FString::Printf(TEXT("%.0f instances is more than %d; a scatter that big needs a drawDistance, so it is built around each player and not all at once"),
			Rounded, WholeInstances);
		return -1;
	}
	return static_cast<int64>(Rounded);
}

bool FMantleScatter::Generate(const FMantleScatterParams& Params, TArray<FMantleScatterInstance>& OutInstances, FString& OutError,
	const FMantleScatterGenerateOptions& Options)
{
	using namespace MantleScatterPrivate;

	OutInstances.Reset();
	const int64 Count = CountFor(Params, OutError);
	if (Count < 0)
	{
		return false;
	}
	const int64 Candidates = Options.MaxCandidates >= 0 ? FMath::Min(Count, Options.MaxCandidates) : Count;

	const FScatterPipeline Pipeline(Params, Options);
	FScatterSpacingGrid Spacing;
	Spacing.Spacing = Pipeline.SpacingMm;

	OutInstances.Reserve(static_cast<int32>(FMath::Min<int64>(Candidates, MAX_int32)));
	for (int64 Index = 0; Index < Candidates; ++Index)
	{
		int64 X = 0;
		int64 Y = 0;
		int64 TangentYaw = 0;
		if (!Pipeline.Place(Index, X, Y, TangentYaw))
		{
			continue;
		}
		FMantleScatterInstance Instance;
		if (!Pipeline.Finish(Index, X, Y, TangentYaw, Instance))
		{
			continue;
		}
		// Last of the filters, so an instance another filter dropped never keeps its neighbours away (#190).
		if (Spacing.Spacing > 0)
		{
			if (Spacing.IsTooClose(X, Y))
			{
				continue;
			}
			Spacing.Add(X, Y);
		}
		OutInstances.Add(Instance);
	}
	return true;
}

// ---- Cells (#189) ----------------------------------------------------------------------------------------------------------------

namespace MantleScatterPrivate
{
	/** About this many candidates in a cell: the cell side follows from the density, clamped between these sides (whole mm). */
	constexpr int64 CellTargetCandidates = 8192;
	constexpr int64 CellMinSideMm = 4000;
	constexpr int64 CellMaxSideMm = 128000;
	/** A cell holding more candidates than this (clusters pile them up) is halved while it can be, and the grid stays under CellMaxCount cells. */
	constexpr int64 CellHotCandidates = 32768;
	constexpr int64 CellMaxCount = 4000000;

	struct FScatterPlaced
	{
		int32 X = 0;
		int32 Y = 0;
		uint32 Candidate = 0;
	};

	/** The cell side the density asks for, in whole metres' worth of mm, integers only so every machine files a candidate in the same cell. */
	int64 ScatterCellSideForDensity(const double Density)
	{
		const int64 DensityMicro = ScatterMillionths(Density);
		if (DensityMicro <= 0)
		{
			return CellMaxSideMm;
		}
		// The cell area for the target is target / density square metres; its side in mm is the square root of target x 1e12 / (density in millionths).
		const int64 SquareMm = CellTargetCandidates * 1000000000000ll / DensityMicro;
		const int64 Side = (ScatterISqrt(SquareMm) / 1000) * 1000;
		return FMath::Clamp<int64>(Side, CellMinSideMm, CellMaxSideMm);
	}

	/** How many cells of Side cover the box MinX..MaxX by MinY..MaxY, and (when that is few enough to count) the most candidates one holds. */
	void ScatterCellGrid(const TArray<FScatterPlaced>& Placed, const int64 MinX, const int64 MinY, const int64 MaxX, const int64 MaxY, const int64 Side,
		int64& OutCellsX, int64& OutCellsY, int64& OutMostInCell)
	{
		OutCellsX = (MaxX - MinX) / Side + 1;
		OutCellsY = (MaxY - MinY) / Side + 1;
		OutMostInCell = 0;
		if (OutCellsX * OutCellsY > CellMaxCount)
		{
			return;
		}
		TArray<uint32> Counts;
		Counts.SetNumZeroed(static_cast<int32>(OutCellsX * OutCellsY));
		for (const FScatterPlaced& Point : Placed)
		{
			const int64 Cell = ((static_cast<int64>(Point.Y) - MinY) / Side) * OutCellsX + (static_cast<int64>(Point.X) - MinX) / Side;
			OutMostInCell = FMath::Max<int64>(OutMostInCell, ++Counts[static_cast<int32>(Cell)]);
		}
	}

	/**
	 * Reads instances out of an index, a slot at a time, with the minimum spacing exact whatever cell a neighbour is in (#189).
	 *
	 * Without spacing an instance is Finish and nothing else. With it, a candidate is kept when it passes every other filter and no
	 * lower-numbered KEPT candidate lies closer than the spacing: the same rule Generate walks in order, here asked per candidate. Whether
	 * a neighbour is kept is itself worked out the same way (from its own lower-numbered neighbours), and remembered, so the answer for a
	 * candidate is a pure function of the stream and never of which cells were read. The chain from neighbour to neighbour runs on an
	 * explicit stack, not the call stack: it can be long, and a worker thread's stack is small.
	 */
	struct FScatterCellReader
	{
		const FScatterPipeline& Pipeline;
		const FMantleScatterCellIndex& Index;

		enum EState : uint8
		{
			Unknown = 0,
			/** Passes every filter but spacing; its neighbours are not all known yet. */
			Passed = 1,
			Dropped = 2,
			Kept = 3
		};
		struct FEntry
		{
			uint8 State = Unknown;
			FMantleScatterInstance Instance;
		};
		TMap<uint32, FEntry> Entries;

		/** The candidates of every cell loaded so far, by spacing-sized square. */
		TMap<uint64, TArray<uint32, TInlineAllocator<4>>> Grid;
		TSet<int32> Loaded;

		FScatterCellReader(const FScatterPipeline& InPipeline, const FMantleScatterCellIndex& InIndex)
			: Pipeline(InPipeline)
			, Index(InIndex)
		{
		}

		bool Filter(const uint32 Slot, FMantleScatterInstance& Out) const
		{
			return Pipeline.Finish(Index.Candidate[Slot], Index.X[Slot], Index.Y[Slot], 0, Out);
		}

		void LoadCell(const int64 CellX, const int64 CellY)
		{
			if (CellX < 0 || CellY < 0 || CellX >= Index.CellsX || CellY >= Index.CellsY)
			{
				return;
			}
			bool bAlready = false;
			Loaded.Add(static_cast<int32>(CellY * Index.CellsX + CellX), &bAlready);
			if (bAlready)
			{
				return;
			}
			uint32 Begin = 0;
			uint32 End = 0;
			Index.SlotRange(FIntPoint(static_cast<int32>(CellX), static_cast<int32>(CellY)), Begin, End);
			const int64 Spacing = Pipeline.SpacingMm;
			for (uint32 Slot = Begin; Slot < End; ++Slot)
			{
				Grid.FindOrAdd(FScatterSpacingGrid::Key(ScatterFloorDiv(Index.X[Slot], Spacing), ScatterFloorDiv(Index.Y[Slot], Spacing))).Add(Slot);
			}
		}

		/** The slots of lower-numbered candidates closer than the spacing to Slot's, loading the cells they might be in. */
		void EarlierNeighbours(const uint32 Slot, TArray<uint32, TInlineAllocator<16>>& Out)
		{
			Out.Reset();
			const int64 Spacing = Pipeline.SpacingMm;
			const int64 X = Index.X[Slot];
			const int64 Y = Index.Y[Slot];
			const uint32 Number = Index.Candidate[Slot];
			// Anything within the spacing is under the box X +- (Spacing - 1): the cells that box covers hold every such candidate.
			const int64 FirstX = ScatterFloorDiv(X - Spacing + 1 - Index.OriginX, Index.CellMm);
			const int64 LastX = ScatterFloorDiv(X + Spacing - 1 - Index.OriginX, Index.CellMm);
			const int64 FirstY = ScatterFloorDiv(Y - Spacing + 1 - Index.OriginY, Index.CellMm);
			const int64 LastY = ScatterFloorDiv(Y + Spacing - 1 - Index.OriginY, Index.CellMm);
			for (int64 CellY = FirstY; CellY <= LastY; ++CellY)
			{
				for (int64 CellX = FirstX; CellX <= LastX; ++CellX)
				{
					LoadCell(CellX, CellY);
				}
			}
			const int64 SquareX = ScatterFloorDiv(X, Spacing);
			const int64 SquareY = ScatterFloorDiv(Y, Spacing);
			for (int64 StepX = -1; StepX <= 1; ++StepX)
			{
				for (int64 StepY = -1; StepY <= 1; ++StepY)
				{
					const TArray<uint32, TInlineAllocator<4>>* Found = Grid.Find(FScatterSpacingGrid::Key(SquareX + StepX, SquareY + StepY));
					if (!Found)
					{
						continue;
					}
					for (const uint32 Other : *Found)
					{
						if (Index.Candidate[Other] >= Number)
						{
							continue;
						}
						// The same test Generate's grid makes: exactly the spacing apart is kept.
						const int64 Dx = FMath::Abs(X - Index.X[Other]);
						const int64 Dy = FMath::Abs(Y - Index.Y[Other]);
						if (Dx < Spacing && Dy < Spacing && Dx * Dx + Dy * Dy < Spacing * Spacing)
						{
							Out.Add(Other);
						}
					}
				}
			}
		}

		/** Whether the candidate at Slot is a kept instance of the stream, and that instance. */
		bool Read(const uint32 Slot, FMantleScatterInstance& OutInstance)
		{
			if (Pipeline.SpacingMm <= 0)
			{
				return Filter(Slot, OutInstance);
			}
			TArray<uint32> Stack;
			Stack.Add(Slot);
			TArray<uint32, TInlineAllocator<16>> Neighbours;
			while (Stack.Num() > 0)
			{
				const uint32 Top = Stack.Last();
				FEntry* Entry = Entries.Find(Top);
				if (Entry && Entry->State >= Dropped)
				{
					Stack.Pop(EAllowShrinking::No);
					continue;
				}
				if (!Entry)
				{
					FEntry Fresh;
					Fresh.State = Filter(Top, Fresh.Instance) ? Passed : Dropped;
					Entry = &Entries.Add(Top, Fresh);
					if (Entry->State == Dropped)
					{
						Stack.Pop(EAllowShrinking::No);
						continue;
					}
				}
				EarlierNeighbours(Top, Neighbours);
				bool bBlocked = false;
				const int32 StackBefore = Stack.Num();
				for (const uint32 Other : Neighbours)
				{
					const FEntry* Known = Entries.Find(Other);
					if (Known && Known->State == Kept)
					{
						bBlocked = true;
						break;
					}
					if (!Known || Known->State == Passed)
					{
						Stack.Add(Other);
					}
				}
				// Re-find: the map may have moved while the neighbours were looked at.
				Entry = Entries.Find(Top);
				if (bBlocked)
				{
					Stack.SetNum(StackBefore, EAllowShrinking::No);
					Entry->State = Dropped;
					Stack.Pop(EAllowShrinking::No);
				}
				else if (Stack.Num() == StackBefore)
				{
					Entry->State = Kept;
					Stack.Pop(EAllowShrinking::No);
				}
			}
			const FEntry* Result = Entries.Find(Slot);
			if (Result && Result->State == Kept)
			{
				OutInstance = Result->Instance;
				return true;
			}
			return false;
		}
	};

	/**
	 * The kept instances among Slots (ascending candidate number), in that order. Candidates at or past MaxCandidates (the Foliage tier's
	 * prefix) are not there; one that is never depends on a later one, so the prefix is a subset of the whole.
	 */
	void ScatterReadSlots(const FScatterPipeline& Pipeline, const FMantleScatterCellIndex& Index, TConstArrayView<uint32> Slots, const int64 MaxCandidates,
		TArray<FMantleScatterInstance>& OutInstances, TArray<uint32>* OutCandidates)
	{
		FScatterCellReader Reader(Pipeline, Index);
		for (const uint32 Slot : Slots)
		{
			if (MaxCandidates >= 0 && static_cast<int64>(Index.Candidate[Slot]) >= MaxCandidates)
			{
				break;
			}
			FMantleScatterInstance Instance;
			if (Reader.Read(Slot, Instance))
			{
				OutInstances.Add(Instance);
				if (OutCandidates)
				{
					OutCandidates->Add(Index.Candidate[Slot]);
				}
			}
		}
	}

	TArray<uint32> ScatterCellSlots(const FMantleScatterCellIndex& Index, const FIntPoint& Cell)
	{
		uint32 Begin = 0;
		uint32 End = 0;
		Index.SlotRange(Cell, Begin, End);
		TArray<uint32> Slots;
		Slots.Reserve(static_cast<int32>(End - Begin));
		for (uint32 Slot = Begin; Slot < End; ++Slot)
		{
			Slots.Add(Slot);
		}
		return Slots;
	}
}

int64 FMantleScatterCellIndex::MemoryBytes() const
{
	return static_cast<int64>(CellStart.GetAllocatedSize() + Candidate.GetAllocatedSize() + X.GetAllocatedSize() + Y.GetAllocatedSize());
}

FIntPoint FMantleScatterCellIndex::CellAt(const int64 XMm, const int64 YMm) const
{
	if (CellsX <= 0 || CellsY <= 0 || CellMm <= 0)
	{
		return FIntPoint::ZeroValue;
	}
	using namespace MantleScatterPrivate;
	return FIntPoint(
		static_cast<int32>(FMath::Clamp<int64>(ScatterFloorDiv(XMm - OriginX, CellMm), 0, CellsX - 1)),
		static_cast<int32>(FMath::Clamp<int64>(ScatterFloorDiv(YMm - OriginY, CellMm), 0, CellsY - 1)));
}

void FMantleScatterCellIndex::SlotRange(const FIntPoint& Cell, uint32& OutBegin, uint32& OutEnd) const
{
	if (Cell.X < 0 || Cell.Y < 0 || Cell.X >= CellsX || Cell.Y >= CellsY)
	{
		OutBegin = OutEnd = 0;
		return;
	}
	const int32 Number = Cell.Y * CellsX + Cell.X;
	OutBegin = CellStart[Number];
	OutEnd = CellStart[Number + 1];
}

FBox2D FMantleScatterCellIndex::CellBoundsCm(const FIntPoint& Cell) const
{
	const double MinX = static_cast<double>(OriginX + static_cast<int64>(Cell.X) * CellMm) * 0.1;
	const double MinY = static_cast<double>(OriginY + static_cast<int64>(Cell.Y) * CellMm) * 0.1;
	const double Side = static_cast<double>(CellMm) * 0.1;
	return FBox2D(FVector2D(MinX, MinY), FVector2D(MinX + Side, MinY + Side));
}

bool FMantleScatter::UsesCells(const FMantleScatterParams& Params)
{
	FString Ignored;
	return CountFor(Params, Ignored) > WholeInstances;
}

TSharedPtr<const FMantleScatterCellIndex> FMantleScatter::BuildCellIndex(const FMantleScatterParams& Params, FString& OutError)
{
	using namespace MantleScatterPrivate;

	const int64 Count = CountFor(Params, OutError);
	if (Count < 0)
	{
		return nullptr;
	}
	const FMantleScatterGenerateOptions NoOptions;
	const FScatterPipeline Pipeline(Params, NoOptions);
	if (Pipeline.StepMm > 0)
	{
		OutError = TEXT("a path step is built whole, not in cells");
		return nullptr;
	}

	TArray<FScatterPlaced> Placed;
	Placed.Reserve(static_cast<int32>(Count));
	int64 MinX = MAX_int64, MinY = MAX_int64, MaxX = MIN_int64, MaxY = MIN_int64;
	for (int64 Candidate = 0; Candidate < Count; ++Candidate)
	{
		int64 X = 0;
		int64 Y = 0;
		int64 TangentYaw = 0;
		if (!Pipeline.Place(Candidate, X, Y, TangentYaw) || FMath::Abs(X) > MAX_int32 || FMath::Abs(Y) > MAX_int32)
		{
			continue;
		}
		FScatterPlaced& Point = Placed.AddDefaulted_GetRef();
		Point.X = static_cast<int32>(X);
		Point.Y = static_cast<int32>(Y);
		Point.Candidate = static_cast<uint32>(Candidate);
		MinX = FMath::Min(MinX, X);
		MinY = FMath::Min(MinY, Y);
		MaxX = FMath::Max(MaxX, X);
		MaxY = FMath::Max(MaxY, Y);
	}

	TSharedRef<FMantleScatterCellIndex> Index = MakeShared<FMantleScatterCellIndex>();
	Index->Count = Count;
	if (Placed.Num() == 0)
	{
		Index->CellMm = FMath::Max<int64>(CellMinSideMm, Pipeline.SpacingMm);
		Index->CellStart.Add(0);
		return Index;
	}

	// The side: from the density, never under the spacing (a neighbour within the spacing is then in the same or an adjacent cell),
	// halved while a cell is hotter than CellHotCandidates and the grid stays small enough.
	int64 Side = FMath::Max(ScatterCellSideForDensity(Params.Density), Pipeline.SpacingMm);
	int64 CellsX = 0, CellsY = 0, Most = 0;
	for (;;)
	{
		ScatterCellGrid(Placed, MinX, MinY, MaxX, MaxY, Side, CellsX, CellsY, Most);
		if (CellsX * CellsY > CellMaxCount)
		{
			// Too many cells for this side (a huge empty footprint): coarser, whatever the heat.
			Side *= 2;
			continue;
		}
		const int64 Half = Side / 2;
		if (Most > CellHotCandidates && Half >= CellMinSideMm && Half >= Pipeline.SpacingMm
			&& ((MaxX - MinX) / Half + 1) * ((MaxY - MinY) / Half + 1) <= CellMaxCount)
		{
			Side = Half;
			continue;
		}
		break;
	}

	Index->CellMm = Side;
	Index->OriginX = MinX;
	Index->OriginY = MinY;
	Index->CellsX = static_cast<int32>(CellsX);
	Index->CellsY = static_cast<int32>(CellsY);
	const int32 NumCells = Index->CellsX * Index->CellsY;
	Index->CellStart.SetNumZeroed(NumCells + 1);
	const auto CellOf = [&](const FScatterPlaced& Point)
	{
		return static_cast<int32>(((static_cast<int64>(Point.Y) - MinY) / Side) * CellsX + (static_cast<int64>(Point.X) - MinX) / Side);
	};
	for (const FScatterPlaced& Point : Placed)
	{
		++Index->CellStart[CellOf(Point) + 1];
	}
	for (int32 Cell = 0; Cell < NumCells; ++Cell)
	{
		Index->CellStart[Cell + 1] += Index->CellStart[Cell];
	}
	Index->Candidate.SetNumUninitialized(Placed.Num());
	Index->X.SetNumUninitialized(Placed.Num());
	Index->Y.SetNumUninitialized(Placed.Num());
	TArray<uint32> Fill;
	Fill.Append(Index->CellStart.GetData(), NumCells);
	// Placed is in ascending candidate order, so each cell's slots are too.
	for (const FScatterPlaced& Point : Placed)
	{
		const uint32 Slot = Fill[CellOf(Point)]++;
		Index->Candidate[Slot] = Point.Candidate;
		Index->X[Slot] = Point.X;
		Index->Y[Slot] = Point.Y;
	}
	return Index;
}

bool FMantleScatter::GenerateCell(const FMantleScatterParams& Params, const FMantleScatterCellIndex& Index, const FIntPoint& Cell,
	TArray<FMantleScatterInstance>& OutInstances, FString& OutError, const FMantleScatterGenerateOptions& Options)
{
	using namespace MantleScatterPrivate;

	OutInstances.Reset();
	if (CountFor(Params, OutError) < 0)
	{
		return false;
	}
	const FScatterPipeline Pipeline(Params, Options);
	const TArray<uint32> Slots = ScatterCellSlots(Index, Cell);
	ScatterReadSlots(Pipeline, Index, Slots, Options.MaxCandidates, OutInstances, nullptr);
	return true;
}

bool FMantleScatter::GenerateInBox(const FMantleScatterParams& Params, const FMantleScatterCellIndex& Index, const int64 MinXMm, const int64 MinYMm,
	const int64 MaxXMm, const int64 MaxYMm, TArray<FMantleScatterInstance>& OutInstances, FString& OutError, const FMantleScatterGenerateOptions& Options)
{
	using namespace MantleScatterPrivate;

	OutInstances.Reset();
	if (CountFor(Params, OutError) < 0)
	{
		return false;
	}
	if (Index.NumCells() == 0 || MaxXMm < MinXMm || MaxYMm < MinYMm)
	{
		return true;
	}
	const FScatterPipeline Pipeline(Params, Options);
	const FIntPoint First = Index.CellAt(MinXMm, MinYMm);
	const FIntPoint Last = Index.CellAt(MaxXMm, MaxYMm);
	TArray<uint32> Slots;
	for (int32 CellY = First.Y; CellY <= Last.Y; ++CellY)
	{
		for (int32 CellX = First.X; CellX <= Last.X; ++CellX)
		{
			uint32 Begin = 0;
			uint32 End = 0;
			Index.SlotRange(FIntPoint(CellX, CellY), Begin, End);
			for (uint32 Slot = Begin; Slot < End; ++Slot)
			{
				if (Index.X[Slot] >= MinXMm && Index.X[Slot] <= MaxXMm && Index.Y[Slot] >= MinYMm && Index.Y[Slot] <= MaxYMm)
				{
					Slots.Add(Slot);
				}
			}
		}
	}
	Slots.Sort([&Index](const uint32 A, const uint32 B) { return Index.Candidate[A] < Index.Candidate[B]; });
	ScatterReadSlots(Pipeline, Index, Slots, Options.MaxCandidates, OutInstances, nullptr);
	return true;
}

bool FMantleScatter::GenerateAllByCells(const FMantleScatterParams& Params, const FMantleScatterCellIndex& Index, TArray<FMantleScatterInstance>& OutInstances,
	FString& OutError, const FMantleScatterGenerateOptions& Options)
{
	using namespace MantleScatterPrivate;

	OutInstances.Reset();
	const int64 Count = CountFor(Params, OutError);
	if (Count < 0)
	{
		return false;
	}
	const FScatterPipeline Pipeline(Params, Options);
	TArray<FMantleScatterInstance> ByCandidate;
	ByCandidate.SetNumUninitialized(static_cast<int32>(Count));
	TArray<uint8> Present;
	Present.SetNumZeroed(static_cast<int32>(Count));
	ParallelFor(Index.NumCells(), [&](const int32 Number)
	{
		const FIntPoint Cell(Number % Index.CellsX, Number / Index.CellsX);
		const TArray<uint32> Slots = ScatterCellSlots(Index, Cell);
		TArray<FMantleScatterInstance> Instances;
		TArray<uint32> Candidates;
		ScatterReadSlots(Pipeline, Index, Slots, Options.MaxCandidates, Instances, &Candidates);
		// Each candidate number is in exactly one cell, so no two cells write the same element.
		for (int32 At = 0; At < Instances.Num(); ++At)
		{
			ByCandidate[static_cast<int32>(Candidates[At])] = Instances[At];
			Present[static_cast<int32>(Candidates[At])] = 1;
		}
	});
	OutInstances.Reserve(static_cast<int32>(Count));
	for (int32 Candidate = 0; Candidate < Present.Num(); ++Candidate)
	{
		if (Present[Candidate])
		{
			OutInstances.Add(ByCandidate[Candidate]);
		}
	}
	return true;
}

const TCHAR* FMantleScatter::PlacementName(const EMantleScatterPlacement Placement)
{
	return Placement == EMantleScatterPlacement::Terrain ? TEXT("terrain") : TEXT("box");
}

bool FMantleScatter::ParsePlacement(const FString& Name, EMantleScatterPlacement& Out)
{
	if (Name.Equals(TEXT("box"), ESearchCase::CaseSensitive))
	{
		Out = EMantleScatterPlacement::Box;
		return true;
	}
	if (Name.Equals(TEXT("terrain"), ESearchCase::CaseSensitive))
	{
		Out = EMantleScatterPlacement::Terrain;
		return true;
	}
	return false;
}

const TCHAR* FMantleScatter::ShapeName(const EMantleScatterShape Shape)
{
	switch (Shape)
	{
	case EMantleScatterShape::Disc:
		return TEXT("disc");
	case EMantleScatterShape::Path:
		return TEXT("path");
	case EMantleScatterShape::Box:
	default:
		return TEXT("box");
	}
}

bool FMantleScatter::ParseShape(const FString& Name, EMantleScatterShape& Out)
{
	for (const EMantleScatterShape Candidate : { EMantleScatterShape::Box, EMantleScatterShape::Disc, EMantleScatterShape::Path })
	{
		if (Name.Equals(ShapeName(Candidate), ESearchCase::CaseSensitive))
		{
			Out = Candidate;
			return true;
		}
	}
	return false;
}

double FMantleScatter::FoliageTierDensity(const int32 FoliageQuality)
{
	static const double Density[] = {0.3, 0.6, 1.0, 1.0};
	return Density[FMath::Clamp(FoliageQuality, 0, 3)];
}

double FMantleScatter::FoliageTierDistance(const int32 FoliageQuality)
{
	static const double Distance[] = {0.5, 0.7, 0.85, 1.0};
	return Distance[FMath::Clamp(FoliageQuality, 0, 3)];
}

bool FMantleScatter::CheckPlaceTotals(TConstArrayView<const FMantleScatterParams*> All, int32& OutBadIndex, FString& OutError)
{
	OutBadIndex = INDEX_NONE;
	int64 Total = 0;
	for (int32 Index = 0; Index < All.Num(); ++Index)
	{
		FString Why;
		const int64 Count = All[Index] ? CountFor(*All[Index], Why) : -1;
		if (Count < 0)
		{
			continue;
		}
		Total += Count;
		if (Total > FMantlePlaceLimits::MaxScatterInstancesPerPlace)
		{
			OutBadIndex = Index;
			OutError = FString::Printf(TEXT("brings the place to %lld scatter instances; a place holds at most %d"),
				static_cast<long long>(Total), FMantlePlaceLimits::MaxScatterInstancesPerPlace);
			return false;
		}
	}
	int64 Colliding = 0;
	for (int32 Index = 0; Index < All.Num(); ++Index)
	{
		FString Why;
		const int64 Count = All[Index] && All[Index]->bCollision ? CountFor(*All[Index], Why) : -1;
		if (Count < 0)
		{
			continue;
		}
		Colliding += Count;
		if (Colliding > FMantlePlaceLimits::MaxScatterCollidingInstancesPerPlace)
		{
			OutBadIndex = Index;
			OutError = FString::Printf(TEXT("brings the place to %lld colliding scatter instances; a place holds at most %d"),
				static_cast<long long>(Colliding), FMantlePlaceLimits::MaxScatterCollidingInstancesPerPlace);
			return false;
		}
	}
	return true;
}

TSharedRef<FJsonObject> FMantleScatter::ToJson(const FMantleScatterParams& Params)
{
	const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	const auto Numbers = [](std::initializer_list<double> Values)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const double Value : Values)
		{
			Out.Add(MakeShared<FJsonValueNumber>(Value));
		}
		return Out;
	};
	Json->SetNumberField(TEXT("seed"), Params.Seed);
	Json->SetArrayField(TEXT("halfExtent"), Numbers({ Params.HalfExtent.X, Params.HalfExtent.Y, Params.HalfExtent.Z }));
	Json->SetNumberField(TEXT("density"), Params.Density);
	Json->SetArrayField(TEXT("scale"), Numbers({ Params.MinScale, Params.MaxScale }));
	Json->SetStringField(TEXT("alignment"), AlignmentName(Params.Alignment));
	Json->SetNumberField(TEXT("maxTilt"), Params.MaxTiltDegrees);
	TArray<TSharedPtr<FJsonValue>> Meshes;
	for (const FMantleScatterMesh& Mesh : Params.Meshes)
	{
		const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("mesh"), Mesh.MeshId);
		Entry->SetNumberField(TEXT("weight"), Mesh.Weight);
		if (Mesh.Materials.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Materials;
			for (const FString& Material : Mesh.Materials)
			{
				Materials.Add(Material.IsEmpty() ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>())
					: TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Material)));
			}
			Entry->SetArrayField(TEXT("materials"), Materials);
		}
		Meshes.Add(MakeShared<FJsonValueObject>(Entry));
	}
	Json->SetArrayField(TEXT("meshes"), Meshes);
	if (Params.DrawDistance > 0.0)
	{
		Json->SetNumberField(TEXT("drawDistance"), Params.DrawDistance);
	}
	if (Params.Placement == EMantleScatterPlacement::Terrain)
	{
		Json->SetStringField(TEXT("placement"), PlacementName(Params.Placement));
		Json->SetNumberField(TEXT("maxSlopeDegrees"), Params.MaxSlopeDegrees);
		Json->SetNumberField(TEXT("minSlopeDegrees"), Params.MinSlopeDegrees);
		if (Params.MinHeight.IsSet())
		{
			Json->SetNumberField(TEXT("minHeight"), Params.MinHeight.GetValue());
		}
		if (Params.MaxHeight.IsSet())
		{
			Json->SetNumberField(TEXT("maxHeight"), Params.MaxHeight.GetValue());
		}
		if (Params.PaintLayer >= 0)
		{
			Json->SetNumberField(TEXT("paintLayer"), Params.PaintLayer);
		}
		if (Params.MinLayerWeight.IsSet())
		{
			Json->SetNumberField(TEXT("minLayerWeight"), Params.MinLayerWeight.GetValue());
		}
		if (Params.MaxLayerWeight.IsSet())
		{
			Json->SetNumberField(TEXT("maxLayerWeight"), Params.MaxLayerWeight.GetValue());
		}
	}
	if (Params.Shape != EMantleScatterShape::Box)
	{
		Json->SetStringField(TEXT("shape"), ShapeName(Params.Shape));
	}
	if (Params.Shape == EMantleScatterShape::Disc)
	{
		Json->SetNumberField(TEXT("radius"), Params.Radius);
	}
	if (Params.Shape == EMantleScatterShape::Path)
	{
		TArray<TSharedPtr<FJsonValue>> Points;
		for (const FVector2D& Point : Params.Path)
		{
			Points.Add(MakeShared<FJsonValueArray>(Numbers({ Point.X, Point.Y })));
		}
		Json->SetArrayField(TEXT("path"), Points);
		Json->SetNumberField(TEXT("width"), Params.Width);
		if (Params.PathStep > 0.0)
		{
			Json->SetNumberField(TEXT("pathStep"), Params.PathStep);
		}
	}
	if (Params.MinSpacing > 0.0)
	{
		Json->SetNumberField(TEXT("minSpacing"), Params.MinSpacing);
	}
	if (Params.PathExclusions.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> Strips;
		for (const FMantleScatterPathExclusion& Strip : Params.PathExclusions)
		{
			const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> Points;
			for (const FVector2D& Point : Strip.Path)
			{
				Points.Add(MakeShared<FJsonValueArray>(Numbers({ Point.X, Point.Y })));
			}
			Entry->SetArrayField(TEXT("path"), Points);
			Entry->SetNumberField(TEXT("width"), Strip.Width);
			Strips.Add(MakeShared<FJsonValueObject>(Entry));
		}
		Json->SetArrayField(TEXT("pathExclusions"), Strips);
	}
	if (Params.Exclusions.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> Exclusions;
		for (const FMantleScatterExclusion& Exclusion : Params.Exclusions)
		{
			const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetArrayField(TEXT("center"), Numbers({ Exclusion.Center.X, Exclusion.Center.Y }));
			Entry->SetArrayField(TEXT("halfExtent"), Numbers({ Exclusion.HalfExtent.X, Exclusion.HalfExtent.Y }));
			Exclusions.Add(MakeShared<FJsonValueObject>(Entry));
		}
		Json->SetArrayField(TEXT("exclusions"), Exclusions);
	}
	if (Params.bCollision)
	{
		Json->SetBoolField(TEXT("collision"), true);
	}
	if (Params.ClusterCount > 0)
	{
		Json->SetNumberField(TEXT("clusterCount"), Params.ClusterCount);
		Json->SetNumberField(TEXT("clusterRadius"), Params.ClusterRadius);
	}
	return Json;
}

FTransform FMantleScatter::ToTransform(const FMantleScatterInstance& Instance)
{
	const FRotator Rotation(Instance.Pitch * 0.01, Instance.Yaw * 0.01, Instance.Roll * 0.01);
	const FVector Location(Instance.X * 0.1, Instance.Y * 0.1, Instance.Z * 0.1);
	const double Scale = Instance.Scale * 0.001;
	return FTransform(Rotation, Location, FVector(Scale));
}

uint64 FMantleScatter::Hash(TConstArrayView<FMantleScatterInstance> Instances)
{
	uint64 Result = 0xCBF29CE484222325ull;
	auto Mix = [&Result](uint32 Value)
	{
		for (int32 Byte = 0; Byte < 4; ++Byte)
		{
			Result ^= (Value >> (Byte * 8)) & 0xFFu;
			Result *= 0x100000001B3ull;
		}
	};
	Mix(static_cast<uint32>(Instances.Num()));
	for (const FMantleScatterInstance& Instance : Instances)
	{
		Mix(static_cast<uint32>(Instance.X));
		Mix(static_cast<uint32>(Instance.Y));
		Mix(static_cast<uint32>(Instance.Z));
		Mix(static_cast<uint32>(Instance.Yaw) | (static_cast<uint32>(static_cast<uint16>(Instance.Pitch)) << 16));
		Mix(static_cast<uint32>(static_cast<uint16>(Instance.Roll)) | (static_cast<uint32>(Instance.Scale) << 16));
		Mix(static_cast<uint32>(Instance.Mesh));
	}
	return Result;
}

FString FMantleScatter::HashText(const uint64 InHash)
{
	return FString::Printf(TEXT("%08x%08x"), static_cast<uint32>(InHash >> 32), static_cast<uint32>(InHash));
}

bool FMantleScatter::DrawsScatter(const UWorld* World)
{
#if UE_SERVER
	(void)World;
	return false;
#else
	return World && World->GetNetMode() != NM_DedicatedServer && !IsRunningDedicatedServer();
#endif
}
