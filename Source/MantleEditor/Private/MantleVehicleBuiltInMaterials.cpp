// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleBuiltInMaterials.h"

#define LOCTEXT_NAMESPACE "MantleVehicleBuiltInMaterials"

namespace MantleVehicleBuiltInMaterialsPrivate
{
	bool HasAny(const FString& Lower, std::initializer_list<const TCHAR*> Words)
	{
		for (const TCHAR* Word : Words)
		{
			if (Lower.Contains(Word))
			{
				return true;
			}
		}
		return false;
	}
}

TArray<FString> FMantleVehicleBuiltInMaterials::All()
{
	return { Rubber(), Glass(), Chrome(), LightLens() };
}

bool FMantleVehicleBuiltInMaterials::IsBuiltIn(const FString& Id)
{
	return All().Contains(Id);
}

FString FMantleVehicleBuiltInMaterials::Suggest(const FString& SectionName)
{
	using namespace MantleVehicleBuiltInMaterialsPrivate;
	const FString Lower = SectionName.ToLower();
	// A part that sits next to glass or rubber but is not it.
	if (HasAny(Lower, { TEXT("wiper"), TEXT("frame"), TEXT("seal"), TEXT("gasket"), TEXT("mirror"), TEXT("sticker"), TEXT("tint_film") }))
	{
		return FString();
	}
	if (HasAny(Lower, { TEXT("lens") }))
	{
		return LightLens();
	}
	if (HasAny(Lower, { TEXT("glass"), TEXT("window"), TEXT("windscreen"), TEXT("windshield") }))
	{
		return Glass();
	}
	if (HasAny(Lower, { TEXT("tire"), TEXT("tyre") }) && !HasAny(Lower, { TEXT("rim"), TEXT("hub"), TEXT("alloy"), TEXT("brake"), TEXT("disc"), TEXT("disk") }))
	{
		return Rubber();
	}
	if (HasAny(Lower, { TEXT("chrome") }))
	{
		return Chrome();
	}
	return FString();
}

FText FMantleVehicleBuiltInMaterials::Label(const FString& Id)
{
	if (Id == Rubber())
	{
		return LOCTEXT("Rubber", "Tire rubber");
	}
	if (Id == Glass())
	{
		return LOCTEXT("Glass", "Glass");
	}
	if (Id == Chrome())
	{
		return LOCTEXT("Chrome", "Chrome");
	}
	if (Id == LightLens())
	{
		return LOCTEXT("LightLens", "Light lens");
	}
	return FText::FromString(Id);
}

#undef LOCTEXT_NAMESPACE
