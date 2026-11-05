// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlayStyle.h"

namespace MantlePlayStyleNames
{
	const TCHAR* const NumberOptions[] = { TEXT("Distance"), TEXT("MinDistance"), TEXT("MaxDistance"), TEXT("Pitch"), TEXT("Yaw"), TEXT("FieldOfView") };
	const TCHAR* const FlagOptions[] = { TEXT("ClickToMove"), TEXT("HideWalls") };
	/** Case sensitive: FString's == is not. */
	inline bool Is(const FString& Key, const TCHAR* Name) { return Key.Equals(Name, ESearchCase::CaseSensitive); }

	const TCHAR* const SideScrollNumberOptions[] = { TEXT("Distance"), TEXT("Height"), TEXT("LookAhead"), TEXT("FieldOfView") };
	const TCHAR* const AllSideScrollOptions[] = { TEXT("Path"), TEXT("Distance"), TEXT("Height"), TEXT("LookAhead"), TEXT("FieldOfView"), TEXT("Bounds") };

	const TCHAR* const AllOptions[] = { TEXT("Distance"), TEXT("MinDistance"), TEXT("MaxDistance"), TEXT("Pitch"), TEXT("Yaw"), TEXT("FieldOfView"), TEXT("ClickToMove"), TEXT("HideWalls") };

	template <SIZE_T N>
	bool Has(const TCHAR* const (&Names)[N], const FString& Key)
	{
		for (const TCHAR* Name : Names)
		{
			if (Key.Equals(Name, ESearchCase::CaseSensitive))
			{
				return true;
			}
		}
		return false;
	}
}

const TCHAR* MantlePlayStyle::StyleName(const EMantlePlayStyle Style)
{
	return Style == EMantlePlayStyle::TopDown ? TEXT("TopDown") : Style == EMantlePlayStyle::SideScroll ? TEXT("SideScroll") : TEXT("Default");
}

bool MantlePlayStyle::ParseStyleName(const FString& Name, EMantlePlayStyle& OutStyle)
{
	if (Name.Equals(TEXT("Default"), ESearchCase::CaseSensitive))
	{
		OutStyle = EMantlePlayStyle::Default;
		return true;
	}
	if (Name.Equals(TEXT("TopDown"), ESearchCase::CaseSensitive))
	{
		OutStyle = EMantlePlayStyle::TopDown;
		return true;
	}
	if (Name.Equals(TEXT("SideScroll"), ESearchCase::CaseSensitive))
	{
		OutStyle = EMantlePlayStyle::SideScroll;
		return true;
	}
	return false;
}

TConstArrayView<const TCHAR*> MantlePlayStyle::TopDownOptionNames()
{
	return MakeArrayView(MantlePlayStyleNames::AllOptions);
}

bool MantlePlayStyle::IsNumberOption(const FString& Key)
{
	return MantlePlayStyleNames::Has(MantlePlayStyleNames::NumberOptions, Key);
}

bool MantlePlayStyle::IsFlagOption(const FString& Key)
{
	return MantlePlayStyleNames::Has(MantlePlayStyleNames::FlagOptions, Key);
}

bool MantlePlayStyle::ApplyNumber(FMantleTopDownOptions& Options, const FString& Key, const double Value, FString& OutError)
{
	if (!FMath::IsFinite(Value))
	{
		OutError = FString::Printf(TEXT("%s must be a finite number"), *Key);
		return false;
	}
	float* Target = nullptr;
	double Min = 0.0;
	double Max = 0.0;
	if (MantlePlayStyleNames::Is(Key, TEXT("Distance")) || MantlePlayStyleNames::Is(Key, TEXT("MinDistance")) || MantlePlayStyleNames::Is(Key, TEXT("MaxDistance")))
	{
		Target = MantlePlayStyleNames::Is(Key, TEXT("Distance")) ? &Options.Distance : MantlePlayStyleNames::Is(Key, TEXT("MinDistance")) ? &Options.MinDistance : &Options.MaxDistance;
		Min = MinDistanceLimit;
		Max = MaxDistanceLimit;
	}
	else if (MantlePlayStyleNames::Is(Key, TEXT("Pitch")))
	{
		Target = &Options.Pitch;
		Min = MinPitch;
		Max = MaxPitch;
	}
	else if (MantlePlayStyleNames::Is(Key, TEXT("FieldOfView")))
	{
		Target = &Options.FieldOfView;
		Min = MinFieldOfView;
		Max = MaxFieldOfView;
	}
	else if (MantlePlayStyleNames::Is(Key, TEXT("Yaw")))
	{
		Options.Yaw = static_cast<float>(FRotator::NormalizeAxis(FMath::Fmod(Value, 360.0)));
		return true;
	}
	else
	{
		OutError = FString::Printf(TEXT("%s is not a top-down play style option"), *Key);
		return false;
	}
	if (Value < Min || Value > Max)
	{
		OutError = FString::Printf(TEXT("%s must be from %s to %s"), *Key, *FString::SanitizeFloat(Min, 0), *FString::SanitizeFloat(Max, 0));
		return false;
	}
	*Target = static_cast<float>(Value);
	return true;
}

void MantlePlayStyle::ApplyFlag(FMantleTopDownOptions& Options, const FString& Key, const bool bValue)
{
	if (MantlePlayStyleNames::Is(Key, TEXT("ClickToMove")))
	{
		Options.bClickToMove = bValue;
	}
	else if (MantlePlayStyleNames::Is(Key, TEXT("HideWalls")))
	{
		Options.bHideWalls = bValue;
	}
}

bool MantlePlayStyle::Finish(const FMantleTopDownOptions& Options, FString& OutError)
{
	if (Options.MinDistance > Options.MaxDistance)
	{
		OutError = FString::Printf(TEXT("MinDistance (%s) must not be above MaxDistance (%s)"),
			*FString::SanitizeFloat(Options.MinDistance, 0), *FString::SanitizeFloat(Options.MaxDistance, 0));
		return false;
	}
	if (Options.Distance < Options.MinDistance || Options.Distance > Options.MaxDistance)
	{
		OutError = FString::Printf(TEXT("Distance (%s) must be between MinDistance (%s) and MaxDistance (%s)"),
			*FString::SanitizeFloat(Options.Distance, 0), *FString::SanitizeFloat(Options.MinDistance, 0), *FString::SanitizeFloat(Options.MaxDistance, 0));
		return false;
	}
	return true;
}

// ---- SideScroll ------------------------------------------------------------------------------------------------------

TConstArrayView<const TCHAR*> MantlePlayStyle::SideScrollOptionNames()
{
	return MakeArrayView(MantlePlayStyleNames::AllSideScrollOptions);
}

bool MantlePlayStyle::IsSideScrollNumberOption(const FString& Key)
{
	return MantlePlayStyleNames::Has(MantlePlayStyleNames::SideScrollNumberOptions, Key);
}

double MantlePlayStyle::PathLength(const TArray<FVector2D>& Path)
{
	double Length = 0.0;
	for (int32 Index = 1; Index < Path.Num(); ++Index)
	{
		Length += FVector2D::Distance(Path[Index - 1], Path[Index]);
	}
	return Length;
}

bool MantlePlayStyle::ApplySideScrollNumber(FMantleSideScrollOptions& Options, const FString& Key, const double Value, FString& OutError)
{
	if (!FMath::IsFinite(Value))
	{
		OutError = FString::Printf(TEXT("%s must be a finite number"), *Key);
		return false;
	}
	float* Target = nullptr;
	double Min = 0.0;
	double Max = 0.0;
	if (MantlePlayStyleNames::Is(Key, TEXT("Distance")))
	{
		Target = &Options.Distance;
		Min = MinSideDistance;
		Max = MaxSideDistance;
	}
	else if (MantlePlayStyleNames::Is(Key, TEXT("Height")))
	{
		Target = &Options.Height;
		Min = MinSideHeight;
		Max = MaxSideHeight;
	}
	else if (MantlePlayStyleNames::Is(Key, TEXT("LookAhead")))
	{
		Target = &Options.LookAhead;
		Min = MinLookAhead;
		Max = MaxLookAhead;
	}
	else if (MantlePlayStyleNames::Is(Key, TEXT("FieldOfView")))
	{
		Target = &Options.FieldOfView;
		Min = MinSideFieldOfView;
		Max = MaxSideFieldOfView;
	}
	else
	{
		OutError = FString::Printf(TEXT("%s is not a side-scroll play style number option"), *Key);
		return false;
	}
	if (Value < Min || Value > Max)
	{
		OutError = FString::Printf(TEXT("%s must be from %s to %s"), *Key, *FString::SanitizeFloat(Min, 0), *FString::SanitizeFloat(Max, 0));
		return false;
	}
	*Target = static_cast<float>(Value);
	return true;
}

bool MantlePlayStyle::ApplySideScrollPath(FMantleSideScrollOptions& Options, const TArray<FVector>& Points, FString& OutError)
{
	if (Points.Num() < MinPathPoints || Points.Num() > MaxPathPoints)
	{
		OutError = FString::Printf(TEXT("Path needs from %d to %d points, got %d"), MinPathPoints, MaxPathPoints, Points.Num());
		return false;
	}
	TArray<FVector2D> Flat;
	Flat.Reserve(Points.Num());
	for (int32 Index = 0; Index < Points.Num(); ++Index)
	{
		const FVector& Point = Points[Index];
		// Z is ignored, so only X and Y have to be numbers.
		if (!FMath::IsFinite(Point.X) || !FMath::IsFinite(Point.Y))
		{
			OutError = FString::Printf(TEXT("Path point %d is not a finite number"), Index + 1);
			return false;
		}
		Flat.Add(FVector2D(Point.X, Point.Y));
	}
	for (int32 Index = 1; Index < Flat.Num(); ++Index)
	{
		if (FVector2D::Distance(Flat[Index - 1], Flat[Index]) < MinSegmentLength)
		{
			OutError = FString::Printf(TEXT("Path segment %d (from point %d to point %d) is shorter than %s cm"), Index, Index, Index + 1,
				*FString::SanitizeFloat(MinSegmentLength, 0));
			return false;
		}
	}
	Options.Path = MoveTemp(Flat);
	return true;
}

bool MantlePlayStyle::ApplySideScrollBounds(FMantleSideScrollOptions& Options, const double Min, const double Max, FString& OutError)
{
	if (!FMath::IsFinite(Min) || !FMath::IsFinite(Max))
	{
		OutError = TEXT("Bounds Min and Max must be finite numbers");
		return false;
	}
	if (Min >= Max)
	{
		OutError = FString::Printf(TEXT("Bounds Min (%s) must be below Max (%s)"), *FString::SanitizeFloat(Min, 0), *FString::SanitizeFloat(Max, 0));
		return false;
	}
	Options.bHasBounds = true;
	Options.BoundsMin = static_cast<float>(Min);
	Options.BoundsMax = static_cast<float>(Max);
	return true;
}

bool MantlePlayStyle::FinishSideScroll(const FMantleSideScrollOptions& Options, FString& OutError)
{
	if (!Options.bHasBounds)
	{
		return true;
	}
	if (Options.Path.IsEmpty())
	{
		OutError = TEXT("Bounds needs a Path: they are distances along it");
		return false;
	}
	const double Length = PathLength(Options.Path);
	// Compared in double, with 0.01 cm of slack, as the Go and Python checkers do: a Max typed as the path's exact length passes.
	if (Options.BoundsMin < 0.0 || static_cast<double>(Options.BoundsMax) > Length + 0.01)
	{
		OutError = FString::Printf(TEXT("Bounds must sit within 0 to the path's length (%s cm), got %s to %s"), *FString::SanitizeFloat(Length, 0),
			*FString::SanitizeFloat(Options.BoundsMin, 0), *FString::SanitizeFloat(Options.BoundsMax, 0));
		return false;
	}
	return true;
}
