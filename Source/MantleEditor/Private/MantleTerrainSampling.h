// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/** Grid sampling shared by the terrain and grass exports. */
namespace MantleTerrainSampling
{
	/** A Width x Height grid of values sampled bilinearly at (X, Y), clamped to the grid. */
	template <typename TValue>
	double Bilinear(const TArray<TValue>& Values, const int32 Width, const int32 Height, double X, double Y)
	{
		X = FMath::Clamp(X, 0.0, static_cast<double>(Width - 1));
		Y = FMath::Clamp(Y, 0.0, static_cast<double>(Height - 1));
		const int32 X0 = FMath::Min(FMath::FloorToInt32(X), Width - 1);
		const int32 Y0 = FMath::Min(FMath::FloorToInt32(Y), Height - 1);
		const int32 X1 = FMath::Min(X0 + 1, Width - 1);
		const int32 Y1 = FMath::Min(Y0 + 1, Height - 1);
		const double FracX = X - X0;
		const double FracY = Y - Y0;
		const double Bottom = FMath::Lerp(static_cast<double>(Values[Y0 * Width + X0]), static_cast<double>(Values[Y0 * Width + X1]), FracX);
		const double Top = FMath::Lerp(static_cast<double>(Values[Y1 * Width + X0]), static_cast<double>(Values[Y1 * Width + X1]), FracX);
		return FMath::Lerp(Bottom, Top, FracY);
	}
}
