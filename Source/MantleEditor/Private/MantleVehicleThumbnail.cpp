// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleThumbnail.h"

#include "MantleCosmeticThumbnail.h"
#include "MantleModelThumbnail.h"
#include "MantlePng.h"
#include "MantleSkinnedMesh.h"
#include "MantleVehicleSource.h"

#include "Engine/SkeletalMesh.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Math/RotationMatrix.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVehicleThumbnailPrivate
{
	/** Subpixels per side: the drawing is made at this many times the picture's size and averaged down, which softens the edges. */
	constexpr int32 Supersample = 2;

	/** Twice the signed area of the triangle (A, B, P) as the rasteriser's edge function. */
	double Edge(const FVector2D& A, const FVector2D& B, const FVector2D& P)
	{
		return (B.X - A.X) * (P.Y - A.Y) - (B.Y - A.Y) * (P.X - A.X);
	}

	/** The card behind the vehicle: dark, a little lighter at the top. */
	FColor Backdrop(const int32 Row, const int32 Rows)
	{
		const float T = Rows > 1 ? static_cast<float>(Row) / static_cast<float>(Rows - 1) : 0.0f;
		return FColor(static_cast<uint8>(FMath::Lerp(58.0f, 28.0f, T)), static_cast<uint8>(FMath::Lerp(63.0f, 31.0f, T)), static_cast<uint8>(FMath::Lerp(74.0f, 38.0f, T)), 255);
	}
}

FRotator FMantleVehicleThumbnail::View()
{
	return FRotator(-18.0, 145.0, 0.0);
}

bool FMantleVehicleThumbnail::Draw(const FMantleSkinnedMesh& Body, TArray<FColor>& OutPixels)
{
	using namespace MantleVehicleThumbnailPrivate;
	OutPixels.Reset();
	if (Body.LODs.Num() == 0 || Body.LODs[0].Indices.Num() < 3 || Body.LODs[0].Positions.Num() == 0)
	{
		return false;
	}
	const FMantleSkinnedMeshLOD& LOD = Body.LODs[0];
	const int32 NumVertices = LOD.Positions.Num();

	// The camera: looks along Forward, with Right and Up across the picture. The vertices are put on those axes (x right, y up, z away from the camera).
	const FRotationMatrix Camera(View());
	const FVector Forward = Camera.GetScaledAxis(EAxis::X);
	const FVector Right = Camera.GetScaledAxis(EAxis::Y);
	const FVector Up = Camera.GetScaledAxis(EAxis::Z);
	TArray<FVector> Projected;
	Projected.SetNumUninitialized(NumVertices);
	FBox Extent(ForceInit);
	for (int32 Vertex = 0; Vertex < NumVertices; ++Vertex)
	{
		const FVector Point(LOD.Positions[Vertex]);
		Projected[Vertex] = FVector(FVector::DotProduct(Point, Right), FVector::DotProduct(Point, Up), FVector::DotProduct(Point, Forward));
		Extent += Projected[Vertex];
	}
	const int32 Full = Size * Supersample;
	const double Span = Extent.IsValid != 0 ? FMath::Max(Extent.GetSize().X, Extent.GetSize().Y) : 0.0;
	if (Span < KINDA_SMALL_NUMBER)
	{
		return false;
	}
	// Orthographic: the vehicle fills about four fifths of the card, centred.
	const double Scale = 0.82 * static_cast<double>(Full) / Span;
	const FVector Middle = Extent.GetCenter();
	const auto ToScreen = [&](const FVector& Point)
	{
		return FVector2D(0.5 * Full + (Point.X - Middle.X) * Scale, 0.5 * Full - (Point.Y - Middle.Y) * Scale);
	};

	TArray<FColor> Layer;
	Layer.SetNumUninitialized(Full * Full);
	for (int32 Row = 0; Row < Full; ++Row)
	{
		const FColor Back = Backdrop(Row, Full);
		for (int32 Column = 0; Column < Full; ++Column)
		{
			Layer[Row * Full + Column] = Back;
		}
	}
	TArray<float> Depth;
	Depth.Init(TNumericLimits<float>::Max(), Full * Full);

	// Lit from above, the front and the left: flat shaded, two sided (the side facing the camera is the one lit).
	const FVector Light = (Up * 0.75 - Forward * 0.55 - Right * 0.35).GetSafeNormal();
	constexpr double BaseR = 182.0;
	constexpr double BaseG = 190.0;
	constexpr double BaseB = 204.0;
	for (int32 At = 0; At + 2 < LOD.Indices.Num(); At += 3)
	{
		const int32 I0 = static_cast<int32>(LOD.Indices[At]);
		const int32 I1 = static_cast<int32>(LOD.Indices[At + 1]);
		const int32 I2 = static_cast<int32>(LOD.Indices[At + 2]);
		if (I0 < 0 || I1 < 0 || I2 < 0 || I0 >= NumVertices || I1 >= NumVertices || I2 >= NumVertices)
		{
			continue;
		}
		FVector Normal = FVector::CrossProduct(FVector(LOD.Positions[I1]) - FVector(LOD.Positions[I0]), FVector(LOD.Positions[I2]) - FVector(LOD.Positions[I0]));
		if (Normal.SizeSquared() < 1.0e-12)
		{
			continue;
		}
		Normal.Normalize();
		if (FVector::DotProduct(Normal, Forward) > 0.0)
		{
			Normal = -Normal;
		}
		const double Shade = 0.28 + 0.72 * FMath::Max(0.0, FVector::DotProduct(Normal, Light));
		const FColor Colour(static_cast<uint8>(FMath::Clamp(BaseR * Shade, 0.0, 255.0)), static_cast<uint8>(FMath::Clamp(BaseG * Shade, 0.0, 255.0)),
			static_cast<uint8>(FMath::Clamp(BaseB * Shade, 0.0, 255.0)), 255);

		const FVector2D S0 = ToScreen(Projected[I0]);
		const FVector2D S1 = ToScreen(Projected[I1]);
		const FVector2D S2 = ToScreen(Projected[I2]);
		const double Area = Edge(S0, S1, S2);
		if (FMath::Abs(Area) < 1.0e-9)
		{
			continue;
		}
		const int32 MinX = FMath::Max(0, FMath::FloorToInt(FMath::Min3(S0.X, S1.X, S2.X)));
		const int32 MaxX = FMath::Min(Full - 1, FMath::CeilToInt(FMath::Max3(S0.X, S1.X, S2.X)));
		const int32 MinY = FMath::Max(0, FMath::FloorToInt(FMath::Min3(S0.Y, S1.Y, S2.Y)));
		const int32 MaxY = FMath::Min(Full - 1, FMath::CeilToInt(FMath::Max3(S0.Y, S1.Y, S2.Y)));
		for (int32 Y = MinY; Y <= MaxY; ++Y)
		{
			for (int32 X = MinX; X <= MaxX; ++X)
			{
				const FVector2D P(X + 0.5, Y + 0.5);
				const double W0 = Edge(S1, S2, P) / Area;
				const double W1 = Edge(S2, S0, P) / Area;
				const double W2 = 1.0 - W0 - W1;
				if (W0 < -1.0e-6 || W1 < -1.0e-6 || W2 < -1.0e-6)
				{
					continue;
				}
				const float Z = static_cast<float>(W0 * Projected[I0].Z + W1 * Projected[I1].Z + W2 * Projected[I2].Z);
				const int32 Pixel = Y * Full + X;
				if (Z < Depth[Pixel])
				{
					Depth[Pixel] = Z;
					Layer[Pixel] = Colour;
				}
			}
		}
	}

	// Average each Supersample x Supersample block down to a pixel.
	OutPixels.SetNumUninitialized(Size * Size);
	for (int32 Row = 0; Row < Size; ++Row)
	{
		for (int32 Column = 0; Column < Size; ++Column)
		{
			int32 R = 0;
			int32 G = 0;
			int32 B = 0;
			for (int32 SubY = 0; SubY < Supersample; ++SubY)
			{
				for (int32 SubX = 0; SubX < Supersample; ++SubX)
				{
					const FColor& Sub = Layer[(Row * Supersample + SubY) * Full + Column * Supersample + SubX];
					R += Sub.R;
					G += Sub.G;
					B += Sub.B;
				}
			}
			constexpr int32 Blocks = Supersample * Supersample;
			OutPixels[Row * Size + Column] = FColor(static_cast<uint8>(R / Blocks), static_cast<uint8>(G / Blocks), static_cast<uint8>(B / Blocks), 255);
		}
	}
	return true;
}

bool FMantleVehicleThumbnail::Make(USkeletalMesh* Mesh, const FMantleSkinnedMesh& Body, TArray<uint8>& OutPng, FString& OutHow, FString& OutError)
{
	OutPng.Reset();
	OutHow.Reset();
	OutError.Reset();
	TArray<FColor> Pixels;
	FString WhyNot;
	if (Mesh && FMantleCosmeticThumbnail::CanCapture(&WhyNot))
	{
		// The engine's own thumbnail scene, as the avatar and garment pictures are taken.
		const FBox3f Box = Body.ComputeBounds(0);
		FMantleThumbnailSubject Subject;
		Subject.SkeletalMeshes.Add(Mesh);
		Subject.Frame = FBox(FVector(Box.Min), FVector(Box.Max));
		Subject.View = View();
		Subject.Describe = FString::Printf(TEXT("%s, library card"), *Mesh->GetName());
		if (FMantleCosmeticThumbnail::Capture(Subject, Size, Pixels, WhyNot) && Pixels.Num() == Size * Size)
		{
			OutHow = TEXT("rendered");
		}
		else
		{
			Pixels.Reset();
		}
	}
	if (Pixels.Num() != Size * Size)
	{
		if (!Draw(Body, Pixels))
		{
			OutError = TEXT("the body has no triangles to draw");
			return false;
		}
		OutHow = WhyNot.IsEmpty() ? FString(TEXT("drawn")) : FString::Printf(TEXT("drawn without a renderer (%s)"), *WhyNot);
	}
	OutPng = FMantlePng::EncodeRGBA8(Size, Size, Pixels);
	if (OutPng.Num() == 0)
	{
		OutError = TEXT("the picture could not be encoded");
		return false;
	}
	return true;
}

bool FMantleVehicleThumbnail::Take(UMantleVehicleSource& Source, USkeletalMesh* Mesh, const FMantleSkinnedMesh& Body, FString& OutHow)
{
	TArray<uint8> Png;
	FString Error;
	if (!Make(Mesh, Body, Png, OutHow, Error))
	{
		OutHow = Error;
		return false;
	}
	Source.Modify();
	Source.ThumbnailPng = MoveTemp(Png);
	return true;
}

FString FMantleVehicleThumbnail::TakenWhenBuiltNote()
{
	return TEXT("The vehicle's picture is taken when you build it. Press Build to see it here.");
}

FMantleThumbnailResolver::FRender FMantleVehicleThumbnail::PreviewFor(TFunction<const UMantleVehicleSource*()> GetBuilt, TFunction<UObject*()> GetModel)
{
	const FMantleThumbnailResolver::FRender ModelRender = FMantleModelThumbnail::RenderFor(GetModel);
	return [GetBuilt = MoveTemp(GetBuilt), ModelRender](const FMantleThumbnailChoice& Choice, TArray<FColor>& OutPixels, int32& OutWidth, int32& OutHeight, FString& OutError)
	{
		if (Choice.Mode != EMantleThumbnailMode::Automatic)
		{
			return ModelRender(Choice, OutPixels, OutWidth, OutHeight, OutError);
		}
		const UMantleVehicleSource* Built = GetBuilt();
		if (!Built || Built->ThumbnailPng.IsEmpty())
		{
			OutError = TakenWhenBuiltNote();
			return false;
		}
		FImage Image;
		if (!FImageUtils::DecompressImage(Built->ThumbnailPng.GetData(), Built->ThumbnailPng.Num(), Image))
		{
			OutError = TEXT("The vehicle's saved picture could not be read. Build the vehicle again.");
			return false;
		}
		Image.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		OutWidth = Image.SizeX;
		OutHeight = Image.SizeY;
		OutPixels = TArray<FColor>(reinterpret_cast<const FColor*>(Image.RawData.GetData()), OutWidth * OutHeight);
		return true;
	};
}
