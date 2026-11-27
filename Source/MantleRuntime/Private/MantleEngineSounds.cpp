// Copyright Joshua Eger. All Rights Reserved.
#include "MantleEngineSounds.h"

const TArray<FName>& FMantleEngineSounds::Car()
{
	static const TArray<FName> Ids = { TEXT("standard"), TEXT("rotary"), TEXT("inline3"), TEXT("inline4"), TEXT("inline5"), TEXT("inline6"),
		TEXT("flat6"), TEXT("v6"), TEXT("v8"), TEXT("v10"), TEXT("v12"), TEXT("w16"), TEXT("diesel") };
	return Ids;
}

const TArray<FName>& FMantleEngineSounds::Bike()
{
	static const TArray<FName> Ids = { TEXT("bike_vtwin"), TEXT("bike_inline3"), TEXT("bike_inline4"), TEXT("bike_highrev") };
	return Ids;
}

void FMantleEngineSounds::Options(const bool bTwoWheeler, const bool bHandlebars, TArray<FName>& OutIds)
{
	OutIds.Reset();
	if (TakesCar(bTwoWheeler))
	{
		OutIds.Append(Car());
	}
	if (TakesBike(bTwoWheeler, bHandlebars))
	{
		OutIds.Append(Bike());
	}
}

bool FMantleEngineSounds::Fits(const FName Id, const bool bTwoWheeler, const bool bHandlebars)
{
	return (TakesCar(bTwoWheeler) && Car().Contains(Id)) || (TakesBike(bTwoWheeler, bHandlebars) && Bike().Contains(Id));
}
