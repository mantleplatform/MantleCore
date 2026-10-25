// Copyright Joshua Eger. All Rights Reserved.
#include "MantleClothingColorMeasure.h"

bool FMantleClothingColorMeasure::MarkUsedTexels(const FMantleGlbMesh& Mesh, int32 MaterialIndex, FIntPoint Size,
	TArray<uint8>& OutUsed, FString& OutError)
{
	OutError.Reset();
	const int64 Count = static_cast<int64>(Size.X) * Size.Y;
	if (Size.X <= 0 || Size.Y <= 0 || Count > MAX_int32) { OutError = TEXT("Color measurement requires a 2D texture size."); return false; }
	auto Cross = [](FVector2f A, FVector2f B) { return A.X * B.Y - A.Y * B.X; };
	// A triangle's UV0, moved by whole tiles so its lowest corner sits in the unit tile. Textures repeat (the runtime samples
	// the mask and albedo with wrapping), so a tiled or offset UV layout reads the same texels; rasterizing wraps around.
	auto TriangleUVs = [&Mesh, &OutError](const FMantleGlbPrimitive& Primitive, int32 Triangle, FVector2f (&UV)[3])
	{
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			const uint32 Local = Primitive.Indices[Triangle + Corner];
			const int64 Vertex = static_cast<int64>(Primitive.FirstVertex) + Local;
			if (Local >= static_cast<uint32>(Primitive.NumVertices) || Vertex < 0 || Vertex >= Mesh.UVs.Num())
			{ OutError = TEXT("Color measurement found an out-of-range UV index."); return false; }
			UV[Corner] = Mesh.UVs[static_cast<int32>(Vertex)];
			if (!FMath::IsFinite(UV[Corner].X) || !FMath::IsFinite(UV[Corner].Y) || FMath::Abs(UV[Corner].X) > 1.e4f || FMath::Abs(UV[Corner].Y) > 1.e4f)
			{ OutError = TEXT("Clothing dye masks require finite UV0."); return false; }
		}
		const FVector2f Tile(FMath::FloorToFloat(FMath::Min3(UV[0].X, UV[1].X, UV[2].X) + 1.e-6f), FMath::FloorToFloat(FMath::Min3(UV[0].Y, UV[1].Y, UV[2].Y) + 1.e-6f));
		for (FVector2f& Corner : UV) Corner -= Tile;
		return true;
	};
	auto Bounds = [Size](const FVector2f (&UV)[3], int32& MinX, int32& MaxX, int32& MinY, int32& MaxY)
	{
		MinX = FMath::Max(FMath::FloorToInt(FMath::Min3(UV[0].X, UV[1].X, UV[2].X) * Size.X), 0);
		MaxX = FMath::FloorToInt(FMath::Max3(UV[0].X, UV[1].X, UV[2].X) * Size.X);
		MinY = FMath::Max(FMath::FloorToInt(FMath::Min3(UV[0].Y, UV[1].Y, UV[2].Y) * Size.Y), 0);
		MaxY = FMath::FloorToInt(FMath::Max3(UV[0].Y, UV[1].Y, UV[2].Y) * Size.Y);
		// Inside the unit tile the last texel row/column holds UV 1.0 exactly.
		if (MaxX == Size.X && FMath::Max3(UV[0].X, UV[1].X, UV[2].X) <= 1.000001f) MaxX = Size.X - 1;
		if (MaxY == Size.Y && FMath::Max3(UV[0].Y, UV[1].Y, UV[2].Y) <= 1.000001f) MaxY = Size.Y - 1;
	};
	auto Wrap = [](int32 Value, int32 Period) { return ((Value % Period) + Period) % Period; };
	int64 RasterVisits = 0;
	// Preflight the accumulated work before visiting any texels, including overlapping UV islands.
	for (const FMantleGlbPrimitive& Primitive : Mesh.Primitives)
	{
		if (Primitive.MaterialIndex != MaterialIndex) continue;
		if (Primitive.Indices.Num() % 3 != 0) { OutError = TEXT("Color measurement needs complete triangles."); return false; }
		for (int32 Triangle = 0; Triangle < Primitive.Indices.Num(); Triangle += 3)
		{
			FVector2f UV[3];
			if (!TriangleUVs(Primitive, Triangle, UV)) return false;
			if (FMath::Abs(Cross(UV[1] - UV[0], UV[2] - UV[0])) < 1.e-12f) continue;
			int32 MinX, MaxX, MinY, MaxY;
			Bounds(UV, MinX, MaxX, MinY, MaxY);
			RasterVisits += static_cast<int64>(MaxX - MinX + 1) * (MaxY - MinY + 1);
			if (RasterVisits > MaxRasterTexelVisits)
			{ OutError = TEXT("Clothing color measurement exceeds 64 million UV texel visits. Use a lower-resolution mask/albedo or reduce repeated overlapping UV triangles."); return false; }
		}
	}
	TArray<uint8> Used; Used.Init(0, static_cast<int32>(Count));
	bool bTriangles = false;
	for (const FMantleGlbPrimitive& Primitive : Mesh.Primitives)
	{
		if (Primitive.MaterialIndex != MaterialIndex) continue;
		for (int32 Triangle = 0; Triangle < Primitive.Indices.Num(); Triangle += 3)
		{
			FVector2f UV[3];
			if (!TriangleUVs(Primitive, Triangle, UV)) return false;
			const float Area = Cross(UV[1] - UV[0], UV[2] - UV[0]);
			if (FMath::Abs(Area) < 1.e-12f) continue;
			bTriangles = true;
			int32 MinX, MaxX, MinY, MaxY;
			Bounds(UV, MinX, MaxX, MinY, MaxY);
			for (int32 Y = MinY; Y <= MaxY; ++Y) for (int32 X = MinX; X <= MaxX; ++X)
			{
				const FVector2f Point((X + 0.5f) / Size.X, (Y + 0.5f) / Size.Y);
				const float A = Cross(UV[1] - Point, UV[2] - Point) / Area;
				const float B = Cross(UV[2] - Point, UV[0] - Point) / Area;
				if (A >= -1.e-6f && B >= -1.e-6f && A + B <= 1.000001f) Used[Wrap(Y, Size.Y) * Size.X + Wrap(X, Size.X)] = 1;
			}
			// Subpixel UV islands still contribute their centroid texel.
			const FVector2f Center = (UV[0] + UV[1] + UV[2]) / 3.0f;
			Used[Wrap(FMath::FloorToInt(Center.Y * Size.Y), Size.Y) * Size.X + Wrap(FMath::FloorToInt(Center.X * Size.X), Size.X)] = 1;
		}
	}
	if (!bTriangles) { OutError = TEXT("Color measurement found no usable UV0 triangles in the declared material."); return false; }
	OutUsed = MoveTemp(Used);
	return true;
}

bool FMantleClothingColorMeasure::Measure(const FMantleGlbMesh& Mesh, int32 MaterialIndex, FIntPoint Size,
	TConstArrayView<FColor> Albedo, TConstArrayView<FColor> Mask,
	TArray<FMantleClothingColorRegion>& Regions, FString& OutError)
{
	OutError.Reset();
	const int64 Count = static_cast<int64>(Size.X) * Size.Y;
	if (Size.X <= 0 || Size.Y <= 0 || Count > MAX_int32 || Albedo.Num() != Count || Mask.Num() != Count || Regions.IsEmpty())
	{ OutError = TEXT("Color measurement requires matching 2D albedo/mask pixels and declared regions."); return false; }
	TArray<FMantleClothingColorRegion> Measured = Regions;
	for (FMantleClothingColorRegion& Region : Measured) Region.ReferenceLuminance = 1;
	if (!MantleClothingColor::ValidateRegions(Measured, OutError)) return false;
	TArray<uint8> Used;
	if (!MarkUsedTexels(Mesh, MaterialIndex, Size, Used, OutError)) return false;
	for (FMantleClothingColorRegion& Region : Measured)
	{
		double Weight = 0, Luminance = 0;
		for (int32 Pixel = 0; Pixel < Used.Num(); ++Pixel)
		{
			if (!Used[Pixel]) continue;
			const FColor Data = Mask[Pixel];
			const double W = (Region.Channel == 0 ? Data.R : Region.Channel == 1 ? Data.G : Data.B) / 255.0;
			const FLinearColor Color = FLinearColor::FromSRGBColor(Albedo[Pixel]);
			Weight += W; Luminance += W * (Color.R * 0.2126 + Color.G * 0.7152 + Color.B * 0.0722);
		}
		if (Weight <= 0) { OutError = FString::Printf(TEXT("Region '%s' has no mask coverage on this material's UV0."), *Region.Id); return false; }
		const double Reference = Luminance / Weight;
		if (!FMath::IsFinite(Reference) || Reference < MantleClothingColor::MinReferenceLuminance)
		{ OutError = FString::Printf(TEXT("Region '%s' is black and has no recoverable color detail. Supply an explicit textured neutral albedo before enabling lighter colors."), *Region.Id); return false; }
		Region.ReferenceLuminance = static_cast<float>(Reference);
	}
	if (!MantleClothingColor::ValidateRegions(Measured, OutError)) return false;
	Regions = MoveTemp(Measured);
	return true;
}
