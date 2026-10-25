// Copyright Joshua Eger. All Rights Reserved.
#include "MantleOverlayChecks.h"
#include "MantlePng.h"

#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleOverlayChecksPrivate
{
	/** Fraction of pixels allowed to break a rule before it counts as broken (dithering, a stray brush edge): 0.5 percent. */
	constexpr double StrayFraction = 0.005;
	/** Channel difference a greyscale mask may carry (rounding from a colour-space conversion). */
	constexpr int32 GreyTolerance = 4;
	/** A red-only mask's green and blue stay at or below this. */
	constexpr int32 EmptyChannelMax = 4;

	bool CheckOverlayDimensions(const FString& What, int32 Width, int32 Height, TArray<FMantleCosmeticIssue>& OutIssues)
	{
		bool bOk = true;
		if (Width != Height)
		{
			OutIssues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("%s is %d x %d; overlay images must be square."), *What, Width, Height),
				TEXT("Paint it on a square canvas such as 1024 x 1024, 512 x 512 or 256 x 256.")));
			bOk = false;
		}
		else if (!FMantleOverlayChecks::IsPowerOfTwoSquare(Width, Height))
		{
			OutIssues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("%s is %d x %d; the size must be a power of two."), *What, Width, Height),
				TEXT("Use 1024, 512, 256, 128 or 64 pixels on each side.")));
			bOk = false;
		}
		if (Width > FMantleOverlayChecks::MaxSize || Height > FMantleOverlayChecks::MaxSize)
		{
			OutIssues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("%s is %d x %d; overlay images are at most %d x %d."),
				*What, Width, Height, FMantleOverlayChecks::MaxSize, FMantleOverlayChecks::MaxSize),
				FString::Printf(TEXT("Scale it down to %d x %d."), FMantleOverlayChecks::MaxSize, FMantleOverlayChecks::MaxSize)));
			bOk = false;
		}
		return bOk;
	}
}

TArray<FString> FMantleOverlayChecks::Slots()
{
	return { TEXT("scar_body"), TEXT("scar_face"), TEXT("tattoo_body"), TEXT("tattoo_face") };
}

bool FMantleOverlayChecks::IsOverlaySlot(const FString& Slot)
{
	return Slots().Contains(Slot);
}

bool FMantleOverlayChecks::IsScarSlot(const FString& Slot)
{
	return Slot == TEXT("scar_body") || Slot == TEXT("scar_face");
}

bool FMantleOverlayChecks::IsFaceSlot(const FString& Slot)
{
	return Slot == TEXT("scar_face") || Slot == TEXT("tattoo_face");
}

bool FMantleOverlayChecks::IsPowerOfTwoSquare(int32 Width, int32 Height)
{
	return Width > 0 && Width == Height && FMath::IsPowerOfTwo(Width);
}

bool FMantleOverlayChecks::ReadPngFile(const FString& File, TArray<FColor>& OutPixels, FIntPoint& OutSize, FString& OutError)
{
	OutPixels.Reset();
	OutSize = FIntPoint::ZeroValue;
	TArray<uint8> Bytes;
	if (File.IsEmpty() || !FFileHelper::LoadFileToArray(Bytes, *File))
	{
		OutError = FString::Printf(TEXT("the file \"%s\" could not be read."), *File);
		return false;
	}
	int32 Width = 0;
	int32 Height = 0;
	bool bAlpha = false;
	if (!FMantlePng::ReadHeader(Bytes, Width, Height, bAlpha))
	{
		OutError = FString::Printf(TEXT("\"%s\" is not a PNG file."), *FPaths::GetCleanFilename(File));
		return false;
	}
	OutSize = FIntPoint(Width, Height);
	// Refuse a huge image from its header, before decoding it.
	if (Width > 8192 || Height > 8192)
	{
		OutError = FString::Printf(TEXT("\"%s\" is %d x %d, far over the %d x %d limit."), *FPaths::GetCleanFilename(File), Width, Height, MaxSize, MaxSize);
		return false;
	}
	FImage Image;
	if (!FImageUtils::DecompressImage(Bytes.GetData(), Bytes.Num(), Image))
	{
		OutError = FString::Printf(TEXT("\"%s\" could not be decoded as a PNG."), *FPaths::GetCleanFilename(File));
		return false;
	}
	// The stored bytes are the data: a PNG's gamma metadata never changes a mask's values.
	Image.GammaSpace = EGammaSpace::Linear;
	FImage Bgra;
	Image.CopyTo(Bgra, ERawImageFormat::BGRA8, EGammaSpace::Linear);
	OutSize = FIntPoint(Bgra.SizeX, Bgra.SizeY);
	const TArrayView64<FColor> View = Bgra.AsBGRA8();
	OutPixels = TArray<FColor>(View.GetData(), static_cast<int32>(View.Num()));
	if (OutPixels.IsEmpty())
	{
		OutError = FString::Printf(TEXT("\"%s\" has no pixels."), *FPaths::GetCleanFilename(File));
		return false;
	}
	return true;
}

bool FMantleOverlayChecks::CheckMask(const FString& What, int32 Width, int32 Height, TConstArrayView<FColor> Pixels,
	TArray<FMantleCosmeticIssue>& OutIssues)
{
	using namespace MantleOverlayChecksPrivate;
	const int32 Before = OutIssues.Num();
	if (!CheckOverlayDimensions(What, Width, Height, OutIssues) || Pixels.Num() != Width * Height)
	{
		return OutIssues.Num() == Before;
	}

	int64 NotGreyNotRed = 0;
	int32 Highest = 0;
	int64 Bright = 0;
	for (const FColor& Pixel : Pixels)
	{
		const bool bGrey = FMath::Abs(static_cast<int32>(Pixel.R) - static_cast<int32>(Pixel.G)) <= GreyTolerance
			&& FMath::Abs(static_cast<int32>(Pixel.R) - static_cast<int32>(Pixel.B)) <= GreyTolerance;
		const bool bRedOnly = Pixel.G <= EmptyChannelMax && Pixel.B <= EmptyChannelMax;
		if (!bGrey && !bRedOnly)
		{
			++NotGreyNotRed;
		}
		Highest = FMath::Max(Highest, static_cast<int32>(Pixel.R));
		if (Pixel.R >= 250)
		{
			++Bright;
		}
	}
	const int64 Total = Pixels.Num();
	if (NotGreyNotRed > static_cast<int64>(Total * StrayFraction))
	{
		OutIssues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("%s is a colour image; an overlay mask must be one channel."), *What),
			TEXT("Save it as a greyscale image (white where the overlay shows, black where it does not), or paint it only in the red channel.")));
		return false;
	}
	if (Highest <= 2)
	{
		OutIssues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("%s is blank; there is nothing painted in it."), *What),
			TEXT("Paint the overlay in white (or red) on black. A mask that is all black shows nothing.")));
		return false;
	}
	if (Bright >= static_cast<int64>(Total * (1.0 - StrayFraction)))
	{
		OutIssues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("%s is solid white; it would cover all of the skin."), *What),
			TEXT("Paint only the shape of the scar or tattoo in white, and leave the rest black.")));
		return false;
	}
	return OutIssues.Num() == Before;
}

bool FMantleOverlayChecks::CheckNormal(const FString& What, int32 Width, int32 Height, TConstArrayView<FColor> Pixels,
	TArray<FMantleCosmeticIssue>& OutIssues)
{
	using namespace MantleOverlayChecksPrivate;
	const int32 Before = OutIssues.Num();
	if (!CheckOverlayDimensions(What, Width, Height, OutIssues) || Pixels.Num() != Width * Height)
	{
		return OutIssues.Num() == Before;
	}
	// A tangent-space normal map is bluish: red and green centre on 128 and blue sits high (z points out of the surface).
	double SumR = 0.0;
	double SumG = 0.0;
	int64 BlueHigh = 0;
	for (const FColor& Pixel : Pixels)
	{
		SumR += Pixel.R;
		SumG += Pixel.G;
		if (Pixel.B >= 128)
		{
			++BlueHigh;
		}
	}
	const double Total = static_cast<double>(Pixels.Num());
	const double MeanR = SumR / Total;
	const double MeanG = SumG / Total;
	const double BlueFraction = static_cast<double>(BlueHigh) / Total;
	if (FMath::Abs(MeanR - 128.0) > 24.0 || FMath::Abs(MeanG - 128.0) > 24.0 || BlueFraction < 0.95)
	{
		OutIssues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("%s does not look like a normal map."), *What),
			TEXT("A normal map is a blue-purple image (flat areas are 128, 128, 255). Use the scar's tangent-space normal map, not a height or colour image.")));
	}
	return OutIssues.Num() == Before;
}

bool FMantleOverlayChecks::CheckNormalAllowed(const FString& Slot, bool bHasNormal, TArray<FMantleCosmeticIssue>& OutIssues)
{
	if (bHasNormal && !IsScarSlot(Slot))
	{
		OutIssues.Add(FMantleCosmeticChecks::Error(TEXT("Tattoos do not take a normal map; only scars do."),
			TEXT("Remove the normal map, or choose a scar slot.")));
		return false;
	}
	return true;
}

TArray<FColor> FMantleOverlayChecks::CanonicalMask(TConstArrayView<FColor> Pixels)
{
	TArray<FColor> Out;
	Out.SetNumUninitialized(Pixels.Num());
	for (int32 Index = 0; Index < Pixels.Num(); ++Index)
	{
		Out[Index] = FColor(Pixels[Index].R, Pixels[Index].R, Pixels[Index].R, 255);
	}
	return Out;
}
