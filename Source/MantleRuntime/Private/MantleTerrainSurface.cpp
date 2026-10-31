// Copyright Joshua Eger. All Rights Reserved.
#include "MantleTerrainSurface.h"
#include "MantleHeightfieldReader.h"
#include "MantlePlaceTypes.h"
#include "MantleRuntimeTextureBuilder.h"

#include "GameFramework/Actor.h"

// ---- FMantleTerrainWeights ----------------------------------------------------------------------

namespace MantleTerrainSurfacePrivate
{
	/** The heaviest of four layer weights; the lower layer on a tie. */
	uint8 Heaviest(const uint8 (&Weights)[4])
	{
		uint8 Best = 0;
		for (uint8 Layer = 1; Layer < 4; ++Layer)
		{
			Best = Weights[Layer] > Weights[Best] ? Layer : Best;
		}
		return Best;
	}
}

bool FMantleTerrainWeights::FromPixels(const int32 InWidth, const int32 InHeight, const TConstArrayView<FColor> Pixels, FMantleTerrainWeights& Out,
	const bool bKeepPixels)
{
	Out = FMantleTerrainWeights();
	if (InWidth <= 0 || InHeight <= 0 || Pixels.Num() != static_cast<int64>(InWidth) * InHeight)
	{
		return false;
	}
	Out.Width = InWidth;
	Out.Height = InHeight;
	Out.Dominant.SetNumUninitialized(Pixels.Num());
	Out.DominantWeight.SetNumUninitialized(Pixels.Num());
	if (bKeepPixels)
	{
		Out.Rgba.SetNumUninitialized(Pixels.Num() * 4);
	}
	for (int32 Index = 0; Index < Pixels.Num(); ++Index)
	{
		const FColor& Pixel = Pixels[Index];
		const uint8 Weights[4] = { Pixel.R, Pixel.G, Pixel.B, Pixel.A };
		Out.Dominant[Index] = MantleTerrainSurfacePrivate::Heaviest(Weights);
		Out.DominantWeight[Index] = Weights[Out.Dominant[Index]];
		if (bKeepPixels)
		{
			FMemory::Memcpy(Out.Rgba.GetData() + Index * 4, Weights, 4);
		}
	}
	return true;
}

bool FMantleTerrainWeights::FromDecodedTexture(const FMantleDecodedTexture& Texture, FMantleTerrainWeights& Out, FString* OutError,
	const bool bKeepPixels)
{
	Out = FMantleTerrainWeights();
	auto Fail = [OutError](const FString& Error)
	{
		if (OutError)
		{
			*OutError = Error;
		}
		return false;
	};
	if (Texture.Mips.Num() == 0)
	{
		return Fail(TEXT("has no pixels"));
	}
	if (Texture.Format != PF_B8G8R8A8)
	{
		return Fail(FString::Printf(TEXT("is %s, not an uncompressed image; a terrain weightmap is uploaded as a PNG"),
			GetPixelFormatString(Texture.Format)));
	}
	const FMantleTextureMip& Top = Texture.Mips[0];
	const int64 Texels = static_cast<int64>(Top.Width) * Top.Height;
	if (Top.Width <= 0 || Top.Height <= 0 || Top.Bytes.Num() != Texels * 4)
	{
		return Fail(FString::Printf(TEXT("has a %d x %d top level of %lld bytes"), Top.Width, Top.Height, Top.Bytes.Num()));
	}
	Out.Width = Top.Width;
	Out.Height = Top.Height;
	Out.Dominant.SetNumUninitialized(static_cast<int32>(Texels));
	Out.DominantWeight.SetNumUninitialized(static_cast<int32>(Texels));
	if (bKeepPixels)
	{
		Out.Rgba.SetNumUninitialized(static_cast<int32>(Texels * 4));
	}
	const uint8* Bytes = Top.Bytes.GetData();
	for (int64 Index = 0; Index < Texels; ++Index)
	{
		// BGRA in memory: red (layer 0) is byte 2, green byte 1, blue byte 0, alpha byte 3.
		const uint8* Pixel = Bytes + Index * 4;
		const uint8 Weights[4] = { Pixel[2], Pixel[1], Pixel[0], Pixel[3] };
		Out.Dominant[static_cast<int32>(Index)] = MantleTerrainSurfacePrivate::Heaviest(Weights);
		Out.DominantWeight[static_cast<int32>(Index)] = Weights[Out.Dominant[static_cast<int32>(Index)]];
		if (bKeepPixels)
		{
			FMemory::Memcpy(Out.Rgba.GetData() + Index * 4, Weights, 4);
		}
	}
	return true;
}

TSharedPtr<const FMantleTerrainHoleGrid> FMantleTerrainHoleGrid::FromMask(const FMantleTerrainWeights& Mask, const int32 InWidth, const int32 InHeight)
{
	if (!Mask.HasPixels() || InWidth < 2 || InHeight < 2)
	{
		return nullptr;
	}
	TSharedRef<FMantleTerrainHoleGrid> Grid = MakeShared<FMantleTerrainHoleGrid>();
	Grid->Width = InWidth;
	Grid->Height = InHeight;

	// A sample is a hole where the nearest texel's red is below 128.
	TArray<uint8> Samples;
	Samples.SetNumUninitialized(InWidth * InHeight);
	for (int32 Y = 0; Y < InHeight; ++Y)
	{
		const int32 TexelY = FMath::RoundToInt32(static_cast<double>(Y) * (Mask.Height - 1) / (InHeight - 1));
		for (int32 X = 0; X < InWidth; ++X)
		{
			const int32 TexelX = FMath::RoundToInt32(static_cast<double>(X) * (Mask.Width - 1) / (InWidth - 1));
			Samples[Y * InWidth + X] = Mask.Rgba[(TexelY * Mask.Width + TexelX) * 4] < 128 ? 1 : 0;
		}
	}
	Grid->Quad.SetNumZeroed((InWidth - 1) * (InHeight - 1));
	bool bAny = false;
	for (int32 Y = 0; Y + 1 < InHeight; ++Y)
	{
		for (int32 X = 0; X + 1 < InWidth; ++X)
		{
			if (Samples[Y * InWidth + X] | Samples[Y * InWidth + X + 1] | Samples[(Y + 1) * InWidth + X] | Samples[(Y + 1) * InWidth + X + 1])
			{
				Grid->Quad[Y * (InWidth - 1) + X] = 1;
				bAny = true;
			}
		}
	}
	return bAny ? TSharedPtr<const FMantleTerrainHoleGrid>(Grid) : nullptr;
}

bool FMantleTerrainHoleGrid::AnyHole(const int32 X0, const int32 Y0, const int32 X1, const int32 Y1) const
{
	for (int32 Y = Y0; Y < Y1; ++Y)
	{
		for (int32 X = X0; X < X1; ++X)
		{
			if (QuadIsHole(X, Y))
			{
				return true;
			}
		}
	}
	return false;
}

int32 FMantleTerrainWeights::DominantLayerAt(const double U, const double V) const
{
	if (!IsValid())
	{
		return 0;
	}
	// The texel whose span holds (U, V), as the material's point sample would pick it.
	const int32 X = FMath::Clamp(FMath::FloorToInt32(U * Width), 0, Width - 1);
	const int32 Y = FMath::Clamp(FMath::FloorToInt32(V * Height), 0, Height - 1);
	return Dominant[Y * Width + X];
}

int32 FMantleTerrainWeights::DominantWeightAt(const double U, const double V) const
{
	if (!IsValid())
	{
		return 0;
	}
	const int32 X = FMath::Clamp(FMath::FloorToInt32(U * Width), 0, Width - 1);
	const int32 Y = FMath::Clamp(FMath::FloorToInt32(V * Height), 0, Height - 1);
	return DominantWeight[Y * Width + X];
}

int32 FMantleTerrainWeights::HeaviestLayerOf(const FMantleTerrainWeights& First, const FMantleTerrainWeights* Second, const double U, const double V)
{
	const int32 Layer = First.DominantLayerAt(U, V);
	if (Second && Second->IsValid() && Second->DominantWeightAt(U, V) > First.DominantWeightAt(U, V))
	{
		return 4 + Second->DominantLayerAt(U, V);
	}
	return Layer;
}

double FMantleTerrainWeights::SampleChannel(const int32 Channel, const double U, const double V) const
{
	if (!HasPixels() || Channel < 0 || Channel > 3)
	{
		return 0.0;
	}
	// The GPU's bilinear filter: texel i's centre is at (i + 0.5) / size, and the edge clamps.
	const double X = FMath::Clamp(U * Width - 0.5, 0.0, static_cast<double>(Width - 1));
	const double Y = FMath::Clamp(V * Height - 0.5, 0.0, static_cast<double>(Height - 1));
	const int32 X0 = FMath::Min(FMath::FloorToInt32(X), Width - 1);
	const int32 Y0 = FMath::Min(FMath::FloorToInt32(Y), Height - 1);
	const int32 X1 = FMath::Min(X0 + 1, Width - 1);
	const int32 Y1 = FMath::Min(Y0 + 1, Height - 1);
	const double FX = X - X0;
	const double FY = Y - Y0;
	const auto Texel = [this, Channel](const int32 TX, const int32 TY)
	{
		return static_cast<double>(Rgba[(TY * Width + TX) * 4 + Channel]);
	};
	const double Top = FMath::Lerp(Texel(X0, Y0), Texel(X1, Y0), FX);
	const double Bottom = FMath::Lerp(Texel(X0, Y1), Texel(X1, Y1), FX);
	return FMath::Lerp(Top, Bottom, FY) / 255.0;
}

// ---- UMantleTerrainSurfaceComponent -------------------------------------------------------------

UMantleTerrainSurfaceComponent::UMantleTerrainSurfaceComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

bool UMantleTerrainSurfaceComponent::HasSurfaces(const FMantleTerrainProps& Props)
{
	return Props.Layers.ContainsByPredicate([](const FMantleTerrainLayer& Layer) { return Layer.Surface != EMantleSurface::Default; });
}

bool UMantleTerrainSurfaceComponent::NeedsWeights(const FMantleTerrainProps& Props)
{
	return Props.Layers.Num() > 1 && !Props.Weightmap.IsEmpty() && HasSurfaces(Props);
}

void UMantleTerrainSurfaceComponent::Init(const FMantleHeightfield& Field, TArray<EMantleSurface> InLayerSurfaces,
	TSharedPtr<const FMantleTerrainWeights> InWeights, TSharedPtr<const FMantleTerrainWeights> InWeights2)
{
	LayerSurfaces = MoveTemp(InLayerSurfaces);
	Weights.Reset();
	Weights2.Reset();
	if (InWeights.IsValid() && InWeights->IsValid())
	{
		Weights = MoveTemp(InWeights);
	}
	if (InWeights2.IsValid() && InWeights2->IsValid())
	{
		Weights2 = MoveTemp(InWeights2);
	}
	ExtentCm = FVector2D(Field.ExtentX(), Field.ExtentY());
}

EMantleSurface UMantleTerrainSurfaceComponent::SurfaceAtLocal(const double X, const double Y) const
{
	if (LayerSurfaces.Num() == 0)
	{
		return EMantleSurface::Default;
	}
	if (LayerSurfaces.Num() == 1 || !HasWeights())
	{
		return LayerSurfaces[0];
	}
	const double U = ExtentCm.X > 0.0 ? X / ExtentCm.X : 0.0;
	const double V = ExtentCm.Y > 0.0 ? Y / ExtentCm.Y : 0.0;
	const int32 Layer = FMantleTerrainWeights::HeaviestLayerOf(*Weights, Weights2.Get(), U, V);
	// A channel with no layer behind it carries no weight in an exported weightmap; layer 0 if one ever does.
	return LayerSurfaces.IsValidIndex(Layer) ? LayerSurfaces[Layer] : LayerSurfaces[0];
}

EMantleSurface UMantleTerrainSurfaceComponent::SurfaceAt(const FVector& WorldLocation) const
{
	const AActor* Owner = GetOwner();
	const FVector Local = Owner ? Owner->GetActorTransform().InverseTransformPosition(WorldLocation) : WorldLocation;
	return SurfaceAtLocal(Local.X, Local.Y);
}
