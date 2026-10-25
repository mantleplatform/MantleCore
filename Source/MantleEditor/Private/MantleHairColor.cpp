// Copyright Joshua Eger. All Rights Reserved.
#include "MantleHairColor.h"

#include "Materials/MaterialInterface.h"

// Named, not anonymous: the module may build as a unity build.
namespace MantleHairColorPrivate
{
	/** HairShadingCommon.ush's D for the default B = 0.3 (A Practical and Controllable Hair and Fur Model). */
	float AbsorptionScale()
	{
		const float B = 0.3f;
		const float B2 = B * B;
		const float B3 = B * B2;
		const float B4 = B2 * B2;
		const float B5 = B * B4;
		return 5.969f - 0.215f * B + 2.532f * B2 - 10.73f * B3 + 5.574f * B4 + 0.245f * B5;
	}

	/** HairColorToAbsorption for one channel (the colour kept off 0, where the log is not finite). */
	float ChannelToAbsorption(const float Channel)
	{
		const float Scaled = FMath::Loge(FMath::Clamp(Channel, 1.0e-4f, 1.0f)) / AbsorptionScale();
		return Scaled * Scaled;
	}

	/** Color with each channel kept in 0 to 1 and the alpha opaque. */
	FLinearColor OpaqueClamped(const FLinearColor& Color)
	{
		return FLinearColor(FMath::Clamp(Color.R, 0.0f, 1.0f), FMath::Clamp(Color.G, 0.0f, 1.0f), FMath::Clamp(Color.B, 0.0f, 1.0f), 1.0f);
	}

	/** HairAbsorptionToColor for one channel. */
	float AbsorptionToChannel(const float Absorption)
	{
		return FMath::Exp(-FMath::Sqrt(FMath::Max(Absorption, 0.0f)) * AbsorptionScale());
	}

	bool HairColorHexDigit(const TCHAR Char, int32& OutValue)
	{
		if (Char >= TEXT('0') && Char <= TEXT('9'))
		{
			OutValue = Char - TEXT('0');
			return true;
		}
		if (Char >= TEXT('a') && Char <= TEXT('f'))
		{
			OutValue = 10 + (Char - TEXT('a'));
			return true;
		}
		if (Char >= TEXT('A') && Char <= TEXT('F'))
		{
			OutValue = 10 + (Char - TEXT('A'));
			return true;
		}
		return false;
	}
}

FLinearColor FMantleHairColor::FromMelanin(const float Melanin, const float Redness, const FLinearColor& DyeColor)
{
	using namespace MantleHairColorPrivate;
	const float ClampedMelanin = FMath::Clamp(Melanin, 0.0f, 1.0f);
	const float ClampedRedness = FMath::Clamp(Redness, 0.0f, 1.0f);
	const float Amount = -FMath::Loge(FMath::Max(1.0f - ClampedMelanin, 0.0001f));
	const float Eumelanin = Amount * (1.0f - ClampedRedness);
	const float Pheomelanin = Amount * ClampedRedness;
	const float EumelaninAbsorption[3] = { 0.506f, 0.841f, 1.653f };
	const float PheomelaninAbsorption[3] = { 0.343f, 0.733f, 1.924f };
	const float Dye[3] = { DyeColor.R, DyeColor.G, DyeColor.B };
	float Out[3] = { 0.0f, 0.0f, 0.0f };
	for (int32 Channel = 0; Channel < 3; ++Channel)
	{
		const float Absorption = Eumelanin * EumelaninAbsorption[Channel] + Pheomelanin * PheomelaninAbsorption[Channel]
			+ ChannelToAbsorption(Dye[Channel]);
		Out[Channel] = AbsorptionToChannel(Absorption);
	}
	return FLinearColor(Out[0], Out[1], Out[2], 1.0f);
}

const TArray<FName>& FMantleHairColor::MelaninNames()
{
	static const TArray<FName> Names = { FName(TEXT("Melanin")), FName(TEXT("HairMelanin")), FName(TEXT("Hair Melanin")) };
	return Names;
}

const TArray<FName>& FMantleHairColor::RednessNames()
{
	static const TArray<FName> Names = { FName(TEXT("Redness")), FName(TEXT("HairRedness")), FName(TEXT("Hair Redness")) };
	return Names;
}

const TArray<FName>& FMantleHairColor::DyeNames()
{
	static const TArray<FName> Names = { FName(TEXT("DyeColor")), FName(TEXT("Dye Color")), FName(TEXT("HairDye")), FName(TEXT("Hair Dye")) };
	return Names;
}

const TArray<FName>& FMantleHairColor::ColorNames()
{
	static const TArray<FName> Names = { FName(TEXT("HairColor")), FName(TEXT("Hair Color")), FName(TEXT("BaseColor")), FName(TEXT("Base Color")),
		FName(TEXT("Color")), FName(TEXT("Tint")) };
	return Names;
}

TOptional<FLinearColor> FMantleHairColor::FromParameters(TFunctionRef<bool(FName Name, float& OutValue)> Scalar,
	TFunctionRef<bool(FName Name, FLinearColor& OutValue)> Vector, ESource* OutSource)
{
	auto FirstScalar = [&Scalar](const TArray<FName>& Names, float& OutValue)
	{
		for (const FName Name : Names)
		{
			if (Scalar(Name, OutValue))
			{
				return true;
			}
		}
		return false;
	};
	auto FirstVector = [&Vector](const TArray<FName>& Names, FLinearColor& OutValue)
	{
		for (const FName Name : Names)
		{
			if (Vector(Name, OutValue))
			{
				return true;
			}
		}
		return false;
	};

	float Melanin = 0.0f;
	if (FirstScalar(MelaninNames(), Melanin))
	{
		float Redness = 0.0f;
		FirstScalar(RednessNames(), Redness);
		FLinearColor Dye = FLinearColor::White;
		FirstVector(DyeNames(), Dye);
		if (OutSource)
		{
			*OutSource = ESource::Melanin;
		}
		return FromMelanin(Melanin, Redness, Dye);
	}
	FLinearColor Color = FLinearColor::White;
	if (FirstVector(ColorNames(), Color))
	{
		if (OutSource)
		{
			*OutSource = ESource::Vector;
		}
		return MantleHairColorPrivate::OpaqueClamped(Color);
	}
	return TOptional<FLinearColor>();
}

TOptional<FLinearColor> FMantleHairColor::FromMaterial(const UMaterialInterface* Material, ESource* OutSource)
{
	if (!Material)
	{
		return TOptional<FLinearColor>();
	}
	return FromParameters(
		[Material](const FName Name, float& OutValue) { return Material->GetScalarParameterValue(FHashedMaterialParameterInfo(Name), OutValue); },
		[Material](const FName Name, FLinearColor& OutValue) { return Material->GetVectorParameterValue(FHashedMaterialParameterInfo(Name), OutValue); },
		OutSource);
}

FLinearColor FMantleHairColor::Resolve(const TOptional<FLinearColor>& Override, TConstArrayView<const UMaterialInterface*> Materials,
	ESource& OutSource, const UMaterialInterface** OutMaterial)
{
	if (OutMaterial)
	{
		*OutMaterial = nullptr;
	}
	if (Override.IsSet())
	{
		OutSource = ESource::Override;
		const FLinearColor Color = Override.GetValue();
		return MantleHairColorPrivate::OpaqueClamped(Color);
	}
	for (const UMaterialInterface* Material : Materials)
	{
		if (const TOptional<FLinearColor> Read = FromMaterial(Material, &OutSource))
		{
			if (OutMaterial)
			{
				*OutMaterial = Material;
			}
			return Read.GetValue();
		}
	}
	OutSource = ESource::Default;
	return DefaultColor();
}

const TCHAR* FMantleHairColor::Describe(const ESource Source)
{
	switch (Source)
	{
	case ESource::Override: return TEXT("the colour set under Hair colour");
	case ESource::Melanin: return TEXT("its material's Melanin and Redness");
	case ESource::Vector: return TEXT("its material's colour parameter");
	case ESource::Default: return TEXT("a default brown (its material names no colour)");
	}
	return TEXT("");
}

FLinearColor FMantleHairColor::DefaultColor()
{
	return FromMelanin(0.5f, 0.3f, FLinearColor::White);
}

bool FMantleHairColor::ParseHex(const FString& Text, FLinearColor& OutLinear)
{
	using namespace MantleHairColorPrivate;
	FString Hex = Text.TrimStartAndEnd();
	if (Hex.StartsWith(TEXT("#")))
	{
		Hex.RightChopInline(1);
	}
	if (Hex.Len() != 6)
	{
		return false;
	}
	uint8 Bytes[3] = { 0, 0, 0 };
	for (int32 Byte = 0; Byte < 3; ++Byte)
	{
		int32 High = 0;
		int32 Low = 0;
		if (!HairColorHexDigit(Hex[Byte * 2], High) || !HairColorHexDigit(Hex[Byte * 2 + 1], Low))
		{
			return false;
		}
		Bytes[Byte] = static_cast<uint8>(High * 16 + Low);
	}
	OutLinear = FLinearColor::FromSRGBColor(FColor(Bytes[0], Bytes[1], Bytes[2], 255));
	OutLinear.A = 1.0f;
	return true;
}

FString FMantleHairColor::ToHex(const FLinearColor& Linear)
{
	const FColor SRGB = FLinearColor(FMath::Clamp(Linear.R, 0.0f, 1.0f), FMath::Clamp(Linear.G, 0.0f, 1.0f), FMath::Clamp(Linear.B, 0.0f, 1.0f), 1.0f)
		.ToFColorSRGB();
	return FString::Printf(TEXT("#%02X%02X%02X"), SRGB.R, SRGB.G, SRGB.B);
}
