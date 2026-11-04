// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlayStyle.h"

namespace MantlePlayStyleNames
{
	const TCHAR* const NumberOptions[] = { TEXT("Distance"), TEXT("MinDistance"), TEXT("MaxDistance"), TEXT("Pitch"), TEXT("Yaw"), TEXT("FieldOfView") };
	const TCHAR* const FlagOptions[] = { TEXT("ClickToMove"), TEXT("HideWalls") };
	/** Case sensitive: FString's == is not. */
	inline bool Is(const FString& Key, const TCHAR* Name) { return Key.Equals(Name, ESearchCase::CaseSensitive); }

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
	return Style == EMantlePlayStyle::TopDown ? TEXT("TopDown") : TEXT("Default");
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
