// Copyright Joshua Eger. All Rights Reserved.
#include "MantleThumbnailChoice.h"
#include "MantlePng.h"

#include "Engine/Texture2D.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#define LOCTEXT_NAMESPACE "MantleThumbnailChoice"

namespace MantleThumbnailPrivate
{
	/** The card transparent pixels sit on: nearly black, as the catalogue's own cards. */
	constexpr int32 BackgroundValue = 5;

	/** A pixel laid over the dark card, as float 0..255 per channel (B, G, R). */
	FVector3f Flatten(const FColor& Color)
	{
		const float Alpha = Color.A / 255.0f;
		const float Back = BackgroundValue * (1.0f - Alpha);
		return FVector3f(Color.B * Alpha + Back, Color.G * Alpha + Back, Color.R * Alpha + Back);
	}

	FColor ToColor(const FVector3f& Bgr)
	{
		return FColor(
			static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Bgr.Z), 0, 255)),
			static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Bgr.Y), 0, 255)),
			static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Bgr.X), 0, 255)),
			255);
	}

	/** Averages the source rectangle each target pixel covers (shrinking): every source pixel counts by how much of it the target pixel covers. */
	void ResampleArea(TConstArrayView<FColor> Src, int32 SrcWidth, const FIntRect& Crop, int32 TargetWidth, int32 TargetHeight, TArray<FColor>& Out)
	{
		const double StepX = static_cast<double>(Crop.Width()) / TargetWidth;
		const double StepY = static_cast<double>(Crop.Height()) / TargetHeight;
		Out.SetNumUninitialized(TargetWidth * TargetHeight);
		for (int32 Y = 0; Y < TargetHeight; ++Y)
		{
			const double Y0 = Crop.Min.Y + Y * StepY;
			const double Y1 = Y0 + StepY;
			for (int32 X = 0; X < TargetWidth; ++X)
			{
				const double X0 = Crop.Min.X + X * StepX;
				const double X1 = X0 + StepX;
				FVector3d Sum = FVector3d::ZeroVector;
				double Weight = 0.0;
				for (int32 SY = FMath::FloorToInt(Y0); SY < FMath::CeilToInt(Y1) && SY < Crop.Max.Y; ++SY)
				{
					const double WY = FMath::Min<double>(SY + 1, Y1) - FMath::Max<double>(SY, Y0);
					for (int32 SX = FMath::FloorToInt(X0); SX < FMath::CeilToInt(X1) && SX < Crop.Max.X; ++SX)
					{
						const double W = WY * (FMath::Min<double>(SX + 1, X1) - FMath::Max<double>(SX, X0));
						const FVector3f P = Flatten(Src[SY * SrcWidth + SX]);
						Sum += FVector3d(P) * W;
						Weight += W;
					}
				}
				Out[Y * TargetWidth + X] = ToColor(Weight > 0.0 ? FVector3f(Sum / Weight) : FVector3f::ZeroVector);
			}
		}
	}

	/** Smooth (bilinear) sampling of the source rectangle, for enlarging. */
	void ResampleSmooth(TConstArrayView<FColor> Src, int32 SrcWidth, const FIntRect& Crop, int32 TargetWidth, int32 TargetHeight, TArray<FColor>& Out)
	{
		const double ScaleX = static_cast<double>(Crop.Width()) / TargetWidth;
		const double ScaleY = static_cast<double>(Crop.Height()) / TargetHeight;
		Out.SetNumUninitialized(TargetWidth * TargetHeight);
		for (int32 Y = 0; Y < TargetHeight; ++Y)
		{
			const double FY = FMath::Clamp((Y + 0.5) * ScaleY - 0.5, 0.0, Crop.Height() - 1.0);
			const int32 Y0 = FMath::FloorToInt(FY);
			const int32 Y1 = FMath::Min(Y0 + 1, Crop.Height() - 1);
			const float TY = static_cast<float>(FY - Y0);
			for (int32 X = 0; X < TargetWidth; ++X)
			{
				const double FX = FMath::Clamp((X + 0.5) * ScaleX - 0.5, 0.0, Crop.Width() - 1.0);
				const int32 X0 = FMath::FloorToInt(FX);
				const int32 X1 = FMath::Min(X0 + 1, Crop.Width() - 1);
				const float TX = static_cast<float>(FX - X0);
				auto At = [&](int32 PX, int32 PY) { return Flatten(Src[(Crop.Min.Y + PY) * SrcWidth + Crop.Min.X + PX]); };
				const FVector3f Top = FMath::Lerp(At(X0, Y0), At(X1, Y0), TX);
				const FVector3f Bottom = FMath::Lerp(At(X0, Y1), At(X1, Y1), TX);
				Out[Y * TargetWidth + X] = ToColor(FMath::Lerp(Top, Bottom, TY));
			}
		}
	}

	FString ShapeWord(const FMantleThumbnailTarget& Target)
	{
		return Target.Width == Target.Height ? FString(TEXT("a square")) : FString::Printf(TEXT("%d:%d"), Target.Width / FMath::Max(1, FMath::GreatestCommonDivisor(Target.Width, Target.Height)),
			Target.Height / FMath::Max(1, FMath::GreatestCommonDivisor(Target.Width, Target.Height)));
	}

	bool CopyToColors(const FImage& Image, TArray<FColor>& OutPixels, int32& OutWidth, int32& OutHeight, FString& OutError)
	{
		FImage Bgra;
		Image.CopyTo(Bgra, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		const TArrayView64<FColor> View = Bgra.AsBGRA8();
		if (View.Num() == 0 || Bgra.SizeX <= 0 || Bgra.SizeY <= 0)
		{
			OutError = TEXT("That picture has no pixels.");
			return false;
		}
		OutWidth = Bgra.SizeX;
		OutHeight = Bgra.SizeY;
		OutPixels = TArray<FColor>(View.GetData(), static_cast<int32>(View.Num()));
		return true;
	}
}

bool FMantleThumbnailResolver::IsInsideProject(const FString& File)
{
	return !File.IsEmpty() && FPaths::IsUnderDirectory(FPaths::ConvertRelativePathToFull(File), FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
}

FIntRect FMantleThumbnailResolver::CropRect(int32 Width, int32 Height, const FMantleThumbnailTarget& Target)
{
	if (Width <= 0 || Height <= 0 || Target.Width <= 0 || Target.Height <= 0)
	{
		return FIntRect();
	}
	int32 CropWidth = Width;
	int32 CropHeight = Height;
	if (static_cast<int64>(Width) * Target.Height >= static_cast<int64>(Height) * Target.Width)
	{
		// Wider than the target: keep the full height.
		CropWidth = FMath::Clamp<int32>(static_cast<int64>(Height) * Target.Width / Target.Height, 1, Width);
	}
	else
	{
		CropHeight = FMath::Clamp<int32>(static_cast<int64>(Width) * Target.Height / Target.Width, 1, Height);
	}
	const int32 X = (Width - CropWidth) / 2;
	const int32 Y = (Height - CropHeight) / 2;
	return FIntRect(X, Y, X + CropWidth, Y + CropHeight);
}

EMantleThumbnailSize FMantleThumbnailResolver::JudgeSize(int32 Width, int32 Height, const FMantleThumbnailTarget& Target)
{
	const FIntRect Crop = CropRect(Width, Height, Target);
	if (Crop.Width() * 2 < Target.Width || Crop.Height() * 2 < Target.Height)
	{
		return EMantleThumbnailSize::TooSmall;
	}
	return Crop.Width() < Target.Width || Crop.Height() < Target.Height ? EMantleThumbnailSize::Enlarged : EMantleThumbnailSize::Fine;
}

bool FMantleThumbnailResolver::FitToTarget(TConstArrayView<FColor> Pixels, int32 Width, int32 Height, const FMantleThumbnailTarget& Target,
	TArray<FColor>& OutPixels, FString& OutWarning, FString& OutError)
{
	using namespace MantleThumbnailPrivate;
	OutPixels.Reset();
	OutWarning.Reset();
	OutError.Reset();
	if (Width <= 0 || Height <= 0 || Pixels.Num() != Width * Height)
	{
		OutError = TEXT("That picture has no pixels.");
		return false;
	}
	const FIntRect Crop = CropRect(Width, Height, Target);
	const EMantleThumbnailSize Verdict = JudgeSize(Width, Height, Target);
	if (Verdict == EMantleThumbnailSize::TooSmall)
	{
		OutError = FString::Printf(TEXT("That picture is too small: cut to %s shape it is %d x %d, and the thumbnail needs at least %d x %d. Use a bigger picture."),
			*ShapeWord(Target), Crop.Width(), Crop.Height(), Target.Width / 2, Target.Height / 2);
		return false;
	}
	if (Crop.Width() >= Target.Width && Crop.Height() >= Target.Height)
	{
		ResampleArea(Pixels, Width, Crop, Target.Width, Target.Height, OutPixels);
	}
	else
	{
		ResampleSmooth(Pixels, Width, Crop, Target.Width, Target.Height, OutPixels);
	}
	if (Verdict == EMantleThumbnailSize::Enlarged)
	{
		OutWarning = FString::Printf(TEXT("That picture is %d x %d after cutting to %s shape, smaller than the %d x %d thumbnail, so it was enlarged and may look soft."),
			Crop.Width(), Crop.Height(), *ShapeWord(Target), Target.Width, Target.Height);
	}
	return true;
}

bool FMantleThumbnailResolver::LoadCustom(const FMantleThumbnailChoice& Choice, TArray<FColor>& OutPixels, int32& OutWidth, int32& OutHeight, FString& OutError)
{
	using namespace MantleThumbnailPrivate;
	OutPixels.Reset();
	OutError.Reset();
	if (!Choice.Texture.IsNull())
	{
		UTexture2D* Texture = Choice.Texture.LoadSynchronous();
		if (!Texture)
		{
			OutError = FString::Printf(TEXT("The thumbnail texture %s could not be loaded."), *Choice.Texture.ToString());
			return false;
		}
#if WITH_EDITORONLY_DATA
		FImage Image;
		if (!Texture->Source.IsValid() || Texture->Source.GetNumBlocks() > 1 || Texture->Source.GetNumSlices() > 1 || !Texture->Source.GetMipImage(Image, 0, 0, 0))
		{
			OutError = FString::Printf(TEXT("%s is not a plain 2D picture Mantle can read. Use a PNG file or an ordinary texture."), *Texture->GetName());
			return false;
		}
		return CopyToColors(Image, OutPixels, OutWidth, OutHeight, OutError);
#else
		OutError = TEXT("Textures can only be read in the editor.");
		return false;
#endif
	}
	if (Choice.ImageFile.FilePath.IsEmpty())
	{
		OutError = TEXT("Choose a PNG file or a texture for the custom image.");
		return false;
	}
	if (!FPaths::FileExists(Choice.ImageFile.FilePath))
	{
		OutError = FString::Printf(TEXT("The picture %s was not found."), *Choice.ImageFile.FilePath);
		return false;
	}
	if (!Choice.ImageFile.FilePath.EndsWith(TEXT(".png"), ESearchCase::IgnoreCase))
	{
		OutError = TEXT("The custom image must be a PNG file or a texture.");
		return false;
	}
	FImage Image;
	if (!FImageUtils::LoadImage(*Choice.ImageFile.FilePath, Image))
	{
		OutError = FString::Printf(TEXT("%s is not a PNG Mantle can read."), *FPaths::GetCleanFilename(Choice.ImageFile.FilePath));
		return false;
	}
	return CopyToColors(Image, OutPixels, OutWidth, OutHeight, OutError);
}

FMantleThumbnailResult FMantleThumbnailResolver::Resolve(const FMantleThumbnailChoice& Choice, const FMantleThumbnailTarget& Target, const FRender& Render)
{
	FMantleThumbnailResult Result;
	TArray<FColor> Source;
	int32 Width = 0;
	int32 Height = 0;
	if (Choice.Mode == EMantleThumbnailMode::Custom)
	{
		if (!LoadCustom(Choice, Source, Width, Height, Result.Error))
		{
			return Result;
		}
		Result.Source = !Choice.Texture.IsNull() ? FString::Printf(TEXT("Thumbnail: custom image from texture %s"), *Choice.Texture.GetAssetName())
			: FString::Printf(TEXT("Thumbnail: custom image from %s"), *FPaths::GetCleanFilename(Choice.ImageFile.FilePath));
	}
	else
	{
		if (!Render)
		{
			Result.Error = TEXT("This kind cannot draw a thumbnail here. Choose Custom image.");
			return Result;
		}
		if (!Render(Choice, Source, Width, Height, Result.Error))
		{
			if (Result.Error.IsEmpty())
			{
				Result.Error = TEXT("The thumbnail could not be drawn.");
			}
			return Result;
		}
		Result.Source = Choice.Mode == EMantleThumbnailMode::Angle ? TEXT("Thumbnail: render from the chosen angle") : TEXT("Thumbnail: automatic render");
	}
	if (Choice.Mode != EMantleThumbnailMode::Custom && Width == Target.Width && Height == Target.Height && Source.Num() == Width * Height)
	{
		// The kind's render is already the target's size: opaque, as a thumbnail always is.
		Result.Pixels = MoveTemp(Source);
		for (FColor& Pixel : Result.Pixels)
		{
			Pixel.A = 255;
		}
		return Result;
	}
	TArray<FColor> Fitted;
	if (!FitToTarget(Source, Width, Height, Target, Fitted, Result.Warning, Result.Error))
	{
		return Result;
	}
	Result.Pixels = MoveTemp(Fitted);
	// A PNG outside the project is only on this machine: say so, so the choice does not quietly break when the project moves.
	if (Choice.Mode == EMantleThumbnailMode::Custom && Choice.Texture.IsNull() && !IsInsideProject(Choice.ImageFile.FilePath))
	{
		const FString Travel = TEXT("This picture is outside your project, so it will not move with the project. Put the PNG inside the project folder, or use a texture.");
		Result.Warning = Result.Warning.IsEmpty() ? Travel : Result.Warning + TEXT(" ") + Travel;
	}
	return Result;
}

TArray<uint8> FMantleThumbnailResolver::ToPng(const FMantleThumbnailResult& Result, const FMantleThumbnailTarget& Target)
{
	if (!Result.Succeeded() || Result.Pixels.Num() != Target.Width * Target.Height)
	{
		return TArray<uint8>();
	}
	return FMantlePng::EncodeRGBA8(Target.Width, Target.Height, Result.Pixels);
}

FText FMantleThumbnailText::Heading()
{
	return LOCTEXT("Heading", "Thumbnail");
}

FText FMantleThumbnailText::ModeName(EMantleThumbnailMode Mode)
{
	switch (Mode)
	{
	case EMantleThumbnailMode::Angle: return LOCTEXT("ModeAngle", "Choose the angle");
	case EMantleThumbnailMode::Custom: return LOCTEXT("ModeCustom", "Custom image");
	default: return LOCTEXT("ModeAutomatic", "Automatic");
	}
}

FText FMantleThumbnailText::ModeHelp(EMantleThumbnailMode Mode)
{
	switch (Mode)
	{
	case EMantleThumbnailMode::Angle: return LOCTEXT("HelpAngle", "The same picture, taken from the angle you choose.");
	case EMantleThumbnailMode::Custom: return LOCTEXT("HelpCustom", "Use your own picture: a PNG file or a texture. It is cut to the shape below and resized for you.");
	default: return LOCTEXT("HelpAutomatic", "Mantle takes the picture for you.");
	}
}

FText FMantleThumbnailText::UseThisView()
{
	return LOCTEXT("UseThisView", "Use this view");
}

FText FMantleThumbnailText::PreviewCaption(const FMantleThumbnailTarget& Target)
{
	return FText::Format(LOCTEXT("PreviewCaption", "This is the picture that will be uploaded ({0} x {1})."), Target.Width, Target.Height);
}

#undef LOCTEXT_NAMESPACE
