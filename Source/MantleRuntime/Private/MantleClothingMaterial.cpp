// Copyright Joshua Eger. All Rights Reserved.
#include "MantleClothingMaterial.h"

#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

UMaterialInterface* FMantleClothingMaterial::Parent()
{
	// The game's material; null where it isn't (a creator project), and callers fall back.
	return LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Mantle/Materials/M_MantleGarmentColor.M_MantleGarmentColor"), nullptr, LOAD_NoWarn | LOAD_Quiet);
}

UMaterialInstanceDynamic* FMantleClothingMaterial::MakeLocal(UMaterialInterface& Shared, UObject& Outer,
	TConstArrayView<FMantleClothingColorRegion> Regions, TConstArrayView<FMantleClothingColor> Colors)
{
	UMaterialInstanceDynamic* Source = Cast<UMaterialInstanceDynamic>(&Shared);
	UMaterialInstanceDynamic* Local = UMaterialInstanceDynamic::Create(Source ? Source->Parent.Get() : &Shared, &Outer);
	if (Local)
	{
		if (Source) Local->CopyInterpParameters(Source);
		Apply(*Local, Regions, Colors);
	}
	return Local;
}

void FMantleClothingMaterial::Apply(UMaterialInstanceDynamic& Instance, TConstArrayView<FMantleClothingColorRegion> Regions,
	TConstArrayView<FMantleClothingColor> Colors)
{
	static const TCHAR* Channels[] = { TEXT("R"), TEXT("G"), TEXT("B") };
	for (const TCHAR* Channel : Channels) Instance.SetScalarParameterValue(FName(*(FString(TEXT("DyeEnabled")) + Channel)), 0.0f);
	for (const FMantleClothingColorRegion& Region : Regions)
	{
		if (Region.Channel > 2 || !FMath::IsFinite(Region.ReferenceLuminance) || Region.ReferenceLuminance < MantleClothingColor::MinReferenceLuminance) continue;
		const FMantleClothingColor* Override = Colors.FindByPredicate([&Region](const FMantleClothingColor& Color) { return Color.RegionId == Region.Id && Color.Color.A == 255; });
		if (!Override) continue;
		const FString Channel(Channels[Region.Channel]);
		Instance.SetScalarParameterValue(FName(*(TEXT("DyeEnabled") + Channel)), 1.0f);
		Instance.SetScalarParameterValue(FName(*(TEXT("DyeReference") + Channel)), Region.ReferenceLuminance);
		Instance.SetVectorParameterValue(FName(*(TEXT("DyeColor") + Channel)), FLinearColor::FromSRGBColor(Override->Color));
	}
}
