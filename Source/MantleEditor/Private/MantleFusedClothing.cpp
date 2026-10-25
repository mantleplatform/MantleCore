// Copyright Joshua Eger. All Rights Reserved.
#include "MantleFusedClothing.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleFusedClothingPrivate
{
	/** Moller-Trumbore, two-sided: the distance along Direction to the triangle, or a negative number for a miss. */
	double FusedRayTriangle(const FVector3d& Origin, const FVector3d& Direction, const FVector3d& A, const FVector3d& B, const FVector3d& C)
	{
		const FVector3d Edge1 = B - A;
		const FVector3d Edge2 = C - A;
		const FVector3d P = FVector3d::CrossProduct(Direction, Edge2);
		const double Determinant = FVector3d::DotProduct(Edge1, P);
		if (FMath::Abs(Determinant) < 1e-12)
		{
			return -1.0;
		}
		const double Inverse = 1.0 / Determinant;
		const FVector3d T = Origin - A;
		const double U = FVector3d::DotProduct(T, P) * Inverse;
		if (U < 0.0 || U > 1.0)
		{
			return -1.0;
		}
		const FVector3d Q = FVector3d::CrossProduct(T, Edge1);
		const double V = FVector3d::DotProduct(Direction, Q) * Inverse;
		if (V < 0.0 || U + V > 1.0)
		{
			return -1.0;
		}
		return FVector3d::DotProduct(Edge2, Q) * Inverse;
	}
}

bool FMantleFusedClothing::NamesClothing(const FString& SlotName)
{
	const FString Lower = SlotName.ToLower();
	for (const TCHAR* Word : { TEXT("boot"), TEXT("shoe"), TEXT("jacket"), TEXT("coat"), TEXT("shirt"), TEXT("pant"), TEXT("trouser"), TEXT("dress"), TEXT("skirt"),
		TEXT("armor"), TEXT("armour"), TEXT("belt"), TEXT("glove"), TEXT("vest"), TEXT("cloth"), TEXT("robe"), TEXT("cape"), TEXT("helmet") })
	{
		if (Lower.Contains(Word))
		{
			return true;
		}
	}
	return false;
}

FMantleFusedResult FMantleFusedClothing::CheckSlot(const FMantleFusedSlot& Slot, const double HeightCm)
{
	using namespace MantleFusedClothingPrivate;
	FMantleFusedResult Out;
	if (NamesClothing(Slot.SlotName))
	{
		Out.Verdict = EMantleFusedVerdict::FusedClothing;
		Out.Reason = FString::Printf(TEXT("its material '%s' is named for clothing"), *Slot.SlotName);
		return Out;
	}
	const int32 Triangles = Slot.Indices.Num() / 3;
	if (Triangles < MinTriangles || HeightCm <= 0.0)
	{
		Out.Verdict = EMantleFusedVerdict::Unknown;
		Out.Reason = TEXT("there is too little of it to tell");
		return Out;
	}
	auto Position = [&Slot](const int32 Index) { return FVector3d(Slot.Vertices[Slot.Indices[Index]]); };
	// Outward: the sign that makes the slot's volume positive (a closed body's winding), so every face normal points out of it.
	// Measured about the slot's own middle, so an open surface (a body without caps) still reads outward.
	FVector3d Middle = FVector3d::ZeroVector;
	for (const FVector3f& Vertex : Slot.Vertices)
	{
		Middle += FVector3d(Vertex);
	}
	Middle /= FMath::Max(1, Slot.Vertices.Num());
	double Volume = 0.0;
	TArray<FVector3d> Normals;
	Normals.SetNumUninitialized(Triangles);
	for (int32 Triangle = 0; Triangle < Triangles; ++Triangle)
	{
		const FVector3d A = Position(Triangle * 3);
		const FVector3d B = Position(Triangle * 3 + 1);
		const FVector3d C = Position(Triangle * 3 + 2);
		Volume += FVector3d::DotProduct(A - Middle, FVector3d::CrossProduct(B - Middle, C - Middle)) / 6.0;
		Normals[Triangle] = FVector3d::CrossProduct(B - A, C - A).GetSafeNormal();
	}
	const double Sign = Volume < 0.0 ? -1.0 : 1.0;
	const double MaxLayerCm = MaxLayerShare * HeightCm;
	const int32 Stride = FMath::Max(1, Triangles / MaxSamples);
	int32 Sampled = 0;
	int32 Layered = 0;
	for (int32 Triangle = 0; Triangle < Triangles; Triangle += Stride)
	{
		const FVector3d Outward = Normals[Triangle] * Sign;
		if (Outward.IsNearlyZero())
		{
			continue;
		}
		const FVector3d Centre = (Position(Triangle * 3) + Position(Triangle * 3 + 1) + Position(Triangle * 3 + 2)) / 3.0;
		const FVector3d Direction = -Outward;
		const FVector3d Origin = Centre + Direction * 0.05;
		double Nearest = TNumericLimits<double>::Max();
		int32 NearestTriangle = INDEX_NONE;
		for (int32 Other = 0; Other < Triangles; ++Other)
		{
			const double T = FusedRayTriangle(Origin, Direction, Position(Other * 3), Position(Other * 3 + 1), Position(Other * 3 + 2));
			if (T > MinLayerCm && T < Nearest)
			{
				Nearest = T;
				NearestTriangle = Other;
			}
		}
		++Sampled;
		// A second layer: the nearest surface inside is close and faces out (a body shell under cloth), not the far side facing away.
		if (NearestTriangle != INDEX_NONE && Nearest <= MaxLayerCm && FVector3d::DotProduct(Direction, Normals[NearestTriangle] * Sign) < 0.0)
		{
			++Layered;
		}
	}
	if (Sampled == 0)
	{
		Out.Verdict = EMantleFusedVerdict::Unknown;
		Out.Reason = TEXT("its surface can't be read");
		return Out;
	}
	const double Share = static_cast<double>(Layered) / Sampled;
	if (Share >= LayeredShare)
	{
		Out.Verdict = EMantleFusedVerdict::FusedClothing;
		Out.Reason = TEXT("clothing looks built into the body");
	}
	else
	{
		Out.Verdict = EMantleFusedVerdict::Bare;
		Out.Reason.Reset();
	}
	Out.Layered = Share;
	return Out;
}

TArray<FMantleFusedResult> FMantleFusedClothing::Check(const TConstArrayView<FMantleFusedSlot> Slots, const double HeightCm)
{
	TArray<FMantleFusedResult> Out;
	for (const FMantleFusedSlot& Slot : Slots)
	{
		Out.Add(CheckSlot(Slot, HeightCm));
	}
	return Out;
}
