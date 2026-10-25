// Copyright Joshua Eger. All Rights Reserved.
#include "MantleClothingColor.h"

bool MantleClothingColor::IsRegionId(const FString& Id)
{
	if (Id.IsEmpty() || Id.Len() > 32 || Id[0] < TEXT('a') || Id[0] > TEXT('z'))
	{
		return false;
	}
	for (const TCHAR Character : Id)
	{
		if (!((Character >= TEXT('a') && Character <= TEXT('z')) || (Character >= TEXT('0') && Character <= TEXT('9'))
			|| Character == TEXT('_') || Character == TEXT('-')))
		{
			return false;
		}
	}
	return true;
}

bool MantleClothingColor::ValidateRegions(const TConstArrayView<FMantleClothingColorRegion> Regions, FString& OutError)
{
	OutError.Reset();
	if (Regions.Num() > MaxRegionsPerSection)
	{
		OutError = TEXT("A clothing material declares at most three RGB colour regions.");
		return false;
	}
	TSet<FString> Ids;
	TSet<uint8> Channels;
	for (const FMantleClothingColorRegion& Region : Regions)
	{
		if (!IsRegionId(Region.Id) || Ids.Contains(Region.Id) || Region.Channel > 2 || Channels.Contains(Region.Channel))
		{
			OutError = TEXT("Colour regions need unique valid IDs and distinct RGB mask channels.");
			return false;
		}
		bool bLabelHasControl = false;
		for (const TCHAR Character : Region.Label)
		{
			bLabelHasControl |= Character < 32 || Character == 127;
		}
		if (Region.Label.TrimStartAndEnd().IsEmpty() || Region.Label.Len() > 64 || bLabelHasControl || Region.DefaultColor.A != 255
			|| !FMath::IsFinite(Region.ReferenceLuminance) || Region.ReferenceLuminance < MinReferenceLuminance || Region.ReferenceLuminance > 1.0f)
		{
			OutError = TEXT("A colour region needs a label, opaque default colour and measured nonzero reference luminance.");
			return false;
		}
		Ids.Add(Region.Id);
		Channels.Add(Region.Channel);
	}
	return true;
}

bool MantleClothingColor::ValidateColors(const TConstArrayView<FMantleClothingColor> Colors,
	const TConstArrayView<FString> Declared, FString& OutError)
{
	OutError.Reset();
	if (Colors.Num() > MaxColorsPerItem)
	{
		OutError = TEXT("One worn clothing item takes at most eight colour overrides.");
		return false;
	}
	TSet<FString> Seen;
	for (const FMantleClothingColor& Color : Colors)
	{
		if (!IsRegionId(Color.RegionId) || Seen.Contains(Color.RegionId) || Color.Color.A != 255 || !Declared.Contains(Color.RegionId))
		{
			OutError = TEXT("Clothing colours must name unique creator-declared regions and contain opaque RGB values.");
			return false;
		}
		Seen.Add(Color.RegionId);
	}
	return true;
}

FLinearColor MantleClothingColor::Apply(const FLinearColor& Albedo, const FVector3f& Mask,
	const TConstArrayView<FMantleClothingColorRegion> Regions, const TConstArrayView<FMantleClothingColor> Colors)
{
	float TotalWeight = 0.0f;
	FLinearColor Dye = FLinearColor::Black;
	const float Luminance = FMath::Max(0.0f, Albedo.R * 0.2126f + Albedo.G * 0.7152f + Albedo.B * 0.0722f);
	for (const FMantleClothingColorRegion& Region : Regions)
	{
		const FMantleClothingColor* Override = Colors.FindByPredicate([&Region](const FMantleClothingColor& Color) { return Color.RegionId == Region.Id; });
		if (!Override || Region.Channel > 2 || !FMath::IsFinite(Region.ReferenceLuminance) || Region.ReferenceLuminance < MinReferenceLuminance)
		{
			continue;
		}
		const float Channel = Mask[Region.Channel];
		const float Weight = FMath::IsFinite(Channel) ? FMath::Clamp(Channel, 0.0f, 1.0f) : 0.0f;
		Dye += FLinearColor::FromSRGBColor(Override->Color) * (Luminance / Region.ReferenceLuminance * Weight);
		TotalWeight += Weight;
	}
	if (TotalWeight <= 0.0f)
	{
		return Albedo;
	}
	const float Normalization = FMath::Max(1.0f, TotalWeight);
	FLinearColor Result = Albedo * (1.0f - FMath::Min(TotalWeight, 1.0f)) + Dye / Normalization;
	Result.R = FMath::Clamp(Result.R, 0.0f, 1.0f);
	Result.G = FMath::Clamp(Result.G, 0.0f, 1.0f);
	Result.B = FMath::Clamp(Result.B, 0.0f, 1.0f);
	Result.A = Albedo.A;
	return Result;
}
