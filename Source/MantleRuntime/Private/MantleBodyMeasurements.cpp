// Copyright Joshua Eger. All Rights Reserved.
#include "MantleBodyMeasurements.h"

namespace MantleBodyMeasurementsPrivate
{
	static float Ratio(float R)
	{
		return FMantleBodyMeasurements::HeightCm * R;
	}

	/** Adds the right and left capsule of a pair; Start and End are the right side's, y is mirrored for the left. */
	static void AddPair(TArray<FMantleGhostCapsule>& Out, const TCHAR* Name, const FVector& Start, const FVector& End, float Radius)
	{
		FMantleGhostCapsule R;
		R.Part = FName(*FString::Printf(TEXT("%s_R"), Name));
		R.Start = Start;
		R.End = End;
		R.RadiusCm = Radius;
		FMantleGhostCapsule L = R;
		L.Part = FName(*FString::Printf(TEXT("%s_L"), Name));
		L.Start.Y = -Start.Y;
		L.End.Y = -End.Y;
		Out.Add(R);
		Out.Add(L);
	}

	static void AddSingle(TArray<FMantleGhostCapsule>& Out, const TCHAR* Name, const FVector& Start, const FVector& End, float Radius)
	{
		FMantleGhostCapsule C;
		C.Part = FName(Name);
		C.Start = Start;
		C.End = End;
		C.RadiusCm = Radius;
		Out.Add(C);
	}

	static void AddHead(TArray<FMantleGhostCapsule>& Out, float TopZ)
	{
		const float R = Ratio(0.045f);
		AddSingle(Out, TEXT("Head"), FVector(0, 0, TopZ - Ratio(0.13f) + R), FVector(0, 0, TopZ - R), R);
	}

	static void AddFeet(TArray<FMantleGhostCapsule>& Out, float AnkleX, float AnkleZ, float HipY)
	{
		const float FootR = Ratio(0.018f);
		const float Length = FMantleBodyMeasurements::FootLengthCm();
		AddPair(Out, TEXT("Foot"), FVector(AnkleX - Length * 0.2f, HipY, AnkleZ), FVector(AnkleX + Length * 0.8f - FootR, HipY, AnkleZ), FootR);
	}
}

float FMantleBodyMeasurements::EyeHeightCm() { return MantleBodyMeasurementsPrivate::Ratio(0.936f); }
float FMantleBodyMeasurements::ShoulderHeightCm() { return MantleBodyMeasurementsPrivate::Ratio(0.818f); }
float FMantleBodyMeasurements::ShoulderWidthCm() { return MantleBodyMeasurementsPrivate::Ratio(0.259f); }
float FMantleBodyMeasurements::UpperArmCm() { return MantleBodyMeasurementsPrivate::Ratio(0.186f); }
float FMantleBodyMeasurements::ForearmCm() { return MantleBodyMeasurementsPrivate::Ratio(0.146f); }
float FMantleBodyMeasurements::HandLengthCm() { return MantleBodyMeasurementsPrivate::Ratio(0.108f); }
float FMantleBodyMeasurements::HandWidthCm() { return MantleBodyMeasurementsPrivate::Ratio(0.045f); }
float FMantleBodyMeasurements::HipHeightCm() { return MantleBodyMeasurementsPrivate::Ratio(0.530f); }
float FMantleBodyMeasurements::HipWidthCm() { return MantleBodyMeasurementsPrivate::Ratio(0.191f); }
float FMantleBodyMeasurements::ThighCm() { return MantleBodyMeasurementsPrivate::Ratio(0.245f); }
float FMantleBodyMeasurements::ShinCm() { return MantleBodyMeasurementsPrivate::Ratio(0.246f); }
float FMantleBodyMeasurements::FootLengthCm() { return MantleBodyMeasurementsPrivate::Ratio(0.152f); }
float FMantleBodyMeasurements::SeatedEyeHeightCm() { return MantleBodyMeasurementsPrivate::Ratio(0.445f); }
float FMantleBodyMeasurements::SeatedHipHeightCm() { return MantleBodyMeasurementsPrivate::Ratio(0.05f); }

void FMantleBodyMeasurements::SolveLimb(const FVector& Root, const FVector& Target, float UpperLen, float LowerLen, const FVector& PoleDir, FVector& OutMid, FVector& OutEnd, bool& bOutReached)
{
	const FVector ToTarget = Target - Root;
	const double Dist = ToTarget.Size();
	const double MaxReach = double(UpperLen) + LowerLen;
	const double MinReach = FMath::Abs(double(UpperLen) - LowerLen);
	const FVector Dir = Dist > UE_KINDA_SMALL_NUMBER ? ToTarget / Dist : FVector(FVector::ForwardVector);

	if (Dist > MaxReach || Dist < MinReach || Dist <= UE_KINDA_SMALL_NUMBER)
	{
		const double Clamped = Dist > MaxReach ? MaxReach : MinReach;
		OutMid = Root + Dir * UpperLen;
		OutEnd = Root + Dir * Clamped;
		bOutReached = false;
		return;
	}

	const double A = (double(UpperLen) * UpperLen - double(LowerLen) * LowerLen + Dist * Dist) / (2.0 * Dist);
	const double H = FMath::Sqrt(FMath::Max(0.0, double(UpperLen) * UpperLen - A * A));
	FVector Pole = PoleDir - Dir * FVector::DotProduct(PoleDir, Dir);
	if (!Pole.Normalize())
	{
		// The pole lies along the limb: any perpendicular will do.
		Pole = FVector::CrossProduct(Dir, FMath::Abs(Dir.Z) < 0.9 ? FVector(FVector::UpVector) : FVector(FVector::RightVector)).GetSafeNormal();
	}
	OutMid = Root + Dir * A + Pole * H;
	OutEnd = Target;
	bOutReached = true;
}

TArray<FMantleGhostCapsule> FMantleBodyMeasurements::StandingGhost()
{
	using namespace MantleBodyMeasurementsPrivate;
	TArray<FMantleGhostCapsule> Out;
	const float ShoulderY = ShoulderWidthCm() * 0.5f;
	const float HipY = HipWidthCm() * 0.5f;
	const float HipZ = HipHeightCm();
	const float ShoulderZ = ShoulderHeightCm();

	AddHead(Out, HeightCm);
	AddSingle(Out, TEXT("Torso"), FVector(0, 0, ShoulderZ - Ratio(0.03f)), FVector(0, 0, HipZ + Ratio(0.07f)), Ratio(0.065f));
	AddSingle(Out, TEXT("Pelvis"), FVector(0, -HipY, HipZ), FVector(0, HipY, HipZ), Ratio(0.055f));

	const FVector Shoulder(0, ShoulderY, ShoulderZ);
	const FVector Elbow = Shoulder - FVector(0, 0, UpperArmCm());
	const FVector Wrist = Elbow - FVector(0, 0, ForearmCm());
	AddPair(Out, TEXT("UpperArm"), Shoulder, Elbow, Ratio(0.025f));
	AddPair(Out, TEXT("LowerArm"), Elbow, Wrist, Ratio(0.02f));
	AddPair(Out, TEXT("Hand"), Wrist, Wrist - FVector(0, 0, HandLengthCm()), HandWidthCm() * 0.5f);

	const FVector Hip(0, HipY, HipZ);
	const FVector Knee = Hip - FVector(0, 0, ThighCm());
	const FVector Ankle = Knee - FVector(0, 0, ShinCm());
	AddPair(Out, TEXT("Thigh"), Hip, Knee, Ratio(0.04f));
	AddPair(Out, TEXT("Shin"), Knee, Ankle, Ratio(0.028f));
	AddFeet(Out, 0.f, Ankle.Z, HipY);
	return Out;
}

TArray<FMantleGhostCapsule> FMantleBodyMeasurements::SeatedGhost()
{
	using namespace MantleBodyMeasurementsPrivate;
	TArray<FMantleGhostCapsule> Out;
	const float ShoulderY = ShoulderWidthCm() * 0.5f;
	const float HipY = HipWidthCm() * 0.5f;
	const float HipZ = SeatedHipHeightCm();
	const float ShoulderZ = HipZ + (ShoulderHeightCm() - HipHeightCm());
	const float TopZ = SeatedEyeHeightCm() + (HeightCm - EyeHeightCm());

	AddHead(Out, TopZ);
	AddSingle(Out, TEXT("Torso"), FVector(0, 0, ShoulderZ - Ratio(0.03f)), FVector(0, 0, HipZ + Ratio(0.07f)), Ratio(0.065f));
	AddSingle(Out, TEXT("Pelvis"), FVector(0, -HipY, HipZ), FVector(0, HipY, HipZ), Ratio(0.055f));

	const FVector Shoulder(0, ShoulderY, ShoulderZ);
	const FVector Elbow = Shoulder - FVector(0, 0, UpperArmCm());
	const FVector Wrist = Elbow + FVector(ForearmCm(), 0, 0);
	AddPair(Out, TEXT("UpperArm"), Shoulder, Elbow, Ratio(0.025f));
	AddPair(Out, TEXT("LowerArm"), Elbow, Wrist, Ratio(0.02f));
	AddPair(Out, TEXT("Hand"), Wrist, Wrist + FVector(HandLengthCm(), 0, 0), HandWidthCm() * 0.5f);

	const FVector Hip(0, HipY, HipZ);
	const FVector Knee = Hip + FVector(ThighCm(), 0, 0);
	const FVector Ankle = Knee - FVector(0, 0, ShinCm());
	AddPair(Out, TEXT("Thigh"), Hip, Knee, Ratio(0.04f));
	AddPair(Out, TEXT("Shin"), Knee, Ankle, Ratio(0.028f));
	AddFeet(Out, Ankle.X, Ankle.Z, HipY);
	return Out;
}
