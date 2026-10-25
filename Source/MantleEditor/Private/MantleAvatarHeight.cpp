// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAvatarHeight.h"
#include "MantleAvatarContract.h"
#include "MantleCharacterScaleRule.h"

#include "UObject/MetaData.h"
#include "UObject/Package.h"

const FName FMantleAvatarHeight::InputScaleTag(TEXT("PipelineInputScale"));

double FMantleAvatarHeight::TargetCm()
{
	// The contract keeps the rule's defaults when limits.json has no usable characterScale (FMantleAvatarContract::CharacterScale).
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get();
	return Contract ? Contract->CharacterScale.DriverStandingHeightCm : FMantleCharacterScaleRule::DefaultDriverStandingHeightCm;
}

double FMantleAvatarHeight::ZHeight(TConstArrayView<FVector3f> Points)
{
	if (Points.Num() == 0)
	{
		return 0.0;
	}
	double Low = TNumericLimits<double>::Max();
	double High = -TNumericLimits<double>::Max();
	for (const FVector3f& Point : Points)
	{
		Low = FMath::Min(Low, static_cast<double>(Point.Z));
		High = FMath::Max(High, static_cast<double>(Point.Z));
	}
	return High - Low;
}

double FMantleAvatarHeight::LowestZ(TConstArrayView<FVector3f> Points)
{
	if (Points.Num() == 0)
	{
		return 0.0;
	}
	double Low = TNumericLimits<double>::Max();
	for (const FVector3f& Point : Points)
	{
		Low = FMath::Min(Low, static_cast<double>(Point.Z));
	}
	return Low;
}

bool FMantleAvatarHeight::FeetOnFloor(const double LowestCm, const double InToleranceCm)
{
	return FMath::IsFinite(LowestCm) && FMath::Abs(LowestCm) <= InToleranceCm;
}

double FMantleAvatarHeight::ConformInputScale(const double PosedHeightCm, const double TargetCm)
{
	// Defensive, unlike the script (which divides whatever the height): no usable height or target leaves the size as it is.
	if (!FMath::IsFinite(PosedHeightCm) || !FMath::IsFinite(TargetCm) || PosedHeightCm <= 0.0 || TargetCm <= 0.0)
	{
		return 1.0;
	}
	return TargetCm / (PosedHeightCm + ConformAddsCm);
}

bool FMantleAvatarHeight::IsWithin(const double BuiltCm, const double TargetCm, const double InToleranceCm)
{
	return BuiltCm >= 0.0 && FMath::Abs(BuiltCm - TargetCm) <= InToleranceCm;
}

bool FMantleAvatarHeight::NeedsRetarget(const double BuiltCm, const double TargetCm)
{
	return FMath::IsFinite(BuiltCm) && FMath::IsFinite(TargetCm) && BuiltCm > 0.0 && FMath::Abs(BuiltCm - TargetCm) > AimCm;
}

bool FMantleAvatarHeight::IsPlausiblePrediction(const double PredictedCm, const double TargetCm)
{
	return FMath::IsFinite(PredictedCm) && FMath::IsFinite(TargetCm) && PredictedCm > 0.0 && FMath::Abs(PredictedCm - TargetCm) <= PredictionPlausibleCm;
}

double FMantleAvatarHeight::RetargetCm(const double RequestedCm, const double BuiltCm, const double TargetCm)
{
	if (!FMath::IsFinite(RequestedCm) || !FMath::IsFinite(BuiltCm) || !FMath::IsFinite(TargetCm) || RequestedCm <= 0.0 || BuiltCm <= 0.0 || TargetCm <= 0.0)
	{
		return RequestedCm;
	}
	return RequestedCm * TargetCm / BuiltCm;
}

FString FMantleAvatarHeight::BuiltHeightLine(const double BuiltCm, const double TargetCm, const double InToleranceCm)
{
	if (BuiltCm < 0.0)
	{
		return FString::Printf(TEXT("HEIGHT WARNING: the built height was not measured, target %.1f +- %.1f"), TargetCm, InToleranceCm);
	}
	return IsWithin(BuiltCm, TargetCm, InToleranceCm)
		? FString::Printf(TEXT("BUILT HEIGHT %.2f cm (target %.1f)"), BuiltCm, TargetCm)
		: FString::Printf(TEXT("HEIGHT WARNING: built %.2f cm, target %.1f +- %.1f"), BuiltCm, TargetCm, InToleranceCm);
}

void FMantleAvatarHeight::WriteInputScale(const UObject& Asset, const double K)
{
	if (UPackage* Package = Asset.GetPackage())
	{
		Package->GetMetaData().SetValue(&Asset, InputScaleTag, *FString::Printf(TEXT("%.17g"), K));
		Package->MarkPackageDirty();
	}
}
