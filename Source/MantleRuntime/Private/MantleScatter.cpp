// Copyright Joshua Eger. All Rights Reserved.
#include "MantleScatter.h"

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

	/** A point in the shape (X-Y, mm about the scatter's origin) from one 64-bit draw, plus the stream's later draws where it has to retry. */
	void ScatterInShape(const FMantleScatterParams& Params, const FScatterPath& Path, const int64 HalfX, const int64 HalfY,
		const int64 RadiusMm, const uint64 Draw, FScatterStream& Stream, int64& OutX, int64& OutY)
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
				return;
			}
			const int64 Along = ScatterPick(ScatterLow(Draw), 0, Path.Total - 1);
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
			return;
		}
		case EMantleScatterShape::Box:
		default:
			OutX = ScatterPick(ScatterLow(Draw), -HalfX, HalfX);
			OutY = ScatterPick(ScatterHigh(Draw), -HalfY, HalfY);
			return;
		}
	}
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

	const int64 HalfX = ScatterHalfMillimetres(Params.HalfExtent.X);
	const int64 HalfY = ScatterHalfMillimetres(Params.HalfExtent.Y);
	const int64 HalfZ = ScatterHalfMillimetres(Params.HalfExtent.Z);
	const int64 RadiusMm = ScatterHalfMillimetres(Params.Radius);
	const int64 ScaleLo = FMath::Clamp<int64>(FMath::RoundToInt64(Params.MinScale * 1000.0), 1, MAX_uint16);
	const int64 ScaleHi = FMath::Clamp<int64>(FMath::RoundToInt64(Params.MaxScale * 1000.0), ScaleLo, MAX_uint16);
	const int64 Tilt = FMath::Clamp<int64>(FMath::RoundToInt64(Params.MaxTiltDegrees * 100.0), 0, 9000);
	const FScatterPath Path = Params.Shape == EMantleScatterShape::Path ? ScatterBuildPath(Params) : FScatterPath();

	const bool bTerrain = Params.Placement == EMantleScatterPlacement::Terrain;
	const bool bSlopeFilter = bTerrain && Params.MaxSlopeDegrees < 90.0;
	const bool bMinSlopeFilter = bTerrain && Params.MinSlopeDegrees > 0.0;
	const bool bAlignToGround = bTerrain && Params.Alignment == EMantleScatterAlignment::Ground;
	// The filters compare whole numbers: the normal's up component and the threshold (the cosine of the limit) each rounded to
	// millionths, heights in whole millimetres, so a last bit of float maths on another machine cannot flip a decision.
	const int64 SlopeLimitMicro = ScatterMillionths(FMath::Cos(FMath::DegreesToRadians(Params.MaxSlopeDegrees)));
	// The minimum slope (#188) is the same test the other way round: kept where the up component is at most the cosine of the limit.
	const int64 MinSlopeLimitMicro = ScatterMillionths(FMath::Cos(FMath::DegreesToRadians(Params.MinSlopeDegrees)));
	// The paint-layer filter (#187): the weight is the weightmap's byte (0 to 255), the thresholds millionths, and the test
	// is byte x 1,000,000 against threshold x 255, all whole numbers.
	const bool bLayerFilter = bTerrain && Params.PaintLayer >= 0;
	const bool bMinLayer = bLayerFilter && Params.MinLayerWeight.IsSet();
	const bool bMaxLayer = bLayerFilter && Params.MaxLayerWeight.IsSet();
	const int64 MinLayerMicro = bMinLayer ? ScatterMillionths(Params.MinLayerWeight.GetValue()) : 0;
	const int64 MaxLayerMicro = bMaxLayer ? ScatterMillionths(Params.MaxLayerWeight.GetValue()) : 0;
	const bool bMinHeight = Params.MinHeight.IsSet();
	const bool bMaxHeight = Params.MaxHeight.IsSet();
	const int64 MinHeightMm = bMinHeight ? ScatterHalfMillimetres(Params.MinHeight.GetValue()) : 0;
	const int64 MaxHeightMm = bMaxHeight ? ScatterHalfMillimetres(Params.MaxHeight.GetValue()) : 0;

	TArray<FScatterBoxMm, TInlineAllocator<FMantleScatter::MaxExclusions>> Exclusions;
	for (const FMantleScatterExclusion& Exclusion : Params.Exclusions)
	{
		FScatterBoxMm& Box = Exclusions.AddDefaulted_GetRef();
		Box.CenterX = ScatterHalfMillimetres(Exclusion.Center.X);
		Box.CenterY = ScatterHalfMillimetres(Exclusion.Center.Y);
		Box.HalfX = ScatterHalfMillimetres(Exclusion.HalfExtent.X);
		Box.HalfY = ScatterHalfMillimetres(Exclusion.HalfExtent.Y);
	}

	// Cluster centres: each from its own stream, numbered past any instance's so the two never share one.
	TArray<FScatterPoint> Centres;
	const int64 ClusterRadiusMm = ScatterHalfMillimetres(Params.ClusterRadius);
	for (int32 Cluster = 0; Cluster < Params.ClusterCount; ++Cluster)
	{
		FScatterStream Stream(Params.Seed, 0x80000000u + static_cast<uint32>(Cluster));
		FScatterPoint& Centre = Centres.AddDefaulted_GetRef();
		ScatterInShape(Params, Path, HalfX, HalfY, RadiusMm, Stream.Next(), Stream, Centre.X, Centre.Y);
	}

	// Cumulative weights: a draw below Cumulative[i] and at or above Cumulative[i - 1] picks mesh i,
	// so a weight of 0 is never picked.
	TArray<int64, TInlineAllocator<MaxMeshes>> Cumulative;
	int64 TotalWeight = 0;
	for (const FMantleScatterMesh& Mesh : Params.Meshes)
	{
		TotalWeight += Mesh.Weight;
		Cumulative.Add(TotalWeight);
	}

	OutInstances.Reserve(static_cast<int32>(Candidates));
	for (int64 Index = 0; Index < Candidates; ++Index)
	{
		// Five 64-bit draws per instance, always all five, so changing the alignment, the scale
		// range, the meshes or the shape never moves the rest of an instance. A retry (a disc's) takes
		// later draws of the same stream.
		FScatterStream Stream(Params.Seed, static_cast<uint32>(Index));
		const uint64 Place = Stream.Next();
		const uint64 Height = Stream.Next();
		const uint64 Turn = Stream.Next();
		const uint64 Look = Stream.Next();
		const uint64 Group = Stream.Next();

		int64 X = 0;
		int64 Y = 0;
		if (Centres.Num() > 0)
		{
			const FScatterPoint& Centre = Centres[static_cast<int32>(ScatterPick(ScatterLow(Group), 0, Centres.Num() - 1))];
			int64 OffsetX = 0;
			int64 OffsetY = 0;
			ScatterOnDisc(ClusterRadiusMm, Place, Stream, OffsetX, OffsetY);
			X = Centre.X + OffsetX;
			Y = Centre.Y + OffsetY;
		}
		else
		{
			ScatterInShape(Params, Path, HalfX, HalfY, RadiusMm, Place, Stream, X, Y);
		}

		bool bExcluded = false;
		for (const FScatterBoxMm& Box : Exclusions)
		{
			if (FMath::Abs(X - Box.CenterX) <= Box.HalfX && FMath::Abs(Y - Box.CenterY) <= Box.HalfY)
			{
				bExcluded = true;
				break;
			}
		}
		if (bExcluded)
		{
			continue;
		}

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
				continue;
			}
			if (bSlopeFilter)
			{
				if (ScatterMillionths(Ground.NormalZ) < SlopeLimitMicro)
				{
					continue;
				}
			}
			if (bMinSlopeFilter)
			{
				if (ScatterMillionths(Ground.NormalZ) > MinSlopeLimitMicro)
				{
					continue;
				}
			}
			if (bMinHeight || bMaxHeight)
			{
				const int64 HeightMm = ScatterHalfMillimetres(Ground.WorldZCm);
				if ((bMinHeight && HeightMm < MinHeightMm) || (bMaxHeight && HeightMm > MaxHeightMm))
				{
					continue;
				}
			}
			if (bLayerFilter)
			{
				const int64 Scaled = static_cast<int64>(FMath::Clamp(Ground.LayerWeight, 0, 255)) * 1000000;
				if ((bMinLayer && Scaled < MinLayerMicro * 255) || (bMaxLayer && Scaled > MaxLayerMicro * 255))
				{
					continue;
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
			continue;
		}

		FMantleScatterInstance& Instance = OutInstances.AddDefaulted_GetRef();
		Instance.X = static_cast<int32>(X);
		Instance.Y = static_cast<int32>(Y);
		Instance.Z = static_cast<int32>(Z);
		Instance.Yaw = static_cast<uint16>(ScatterPick(ScatterHigh(Height), 0, 35999));
		switch (Params.Alignment)
		{
		case EMantleScatterAlignment::Tilted:
		case EMantleScatterAlignment::Ground:
			Instance.Pitch = static_cast<int16>(ScatterPick(ScatterLow(Turn), -Tilt, Tilt));
			Instance.Roll = static_cast<int16>(ScatterPick(ScatterHigh(Turn), -Tilt, Tilt));
			// Ground leans like Tilted, then the whole turn is carried onto the slope (#188).
			if (bAlignToGround)
			{
				ScatterAlignToGround(AlignX, AlignY, AlignZ, Instance.Yaw, Instance.Pitch, Instance.Roll);
			}
			break;
		case EMantleScatterAlignment::Tumbled:
			Instance.Pitch = static_cast<int16>(ScatterPick(ScatterLow(Turn), -9000, 9000));
			Instance.Roll = static_cast<int16>(ScatterPick(ScatterHigh(Turn), -18000, 17999));
			break;
		case EMantleScatterAlignment::Upright:
		default:
			Instance.Pitch = 0;
			Instance.Roll = 0;
			break;
		}
		Instance.Scale = static_cast<uint16>(ScatterPick(ScatterLow(Look), ScaleLo, ScaleHi));

		const int64 Pick = ScatterPick(ScatterHigh(Look), 0, TotalWeight - 1);
		int32 MeshIndex = 0;
		while (MeshIndex < Cumulative.Num() - 1 && Pick >= Cumulative[MeshIndex])
		{
			++MeshIndex;
		}
		Instance.Mesh = static_cast<uint16>(MeshIndex);
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
