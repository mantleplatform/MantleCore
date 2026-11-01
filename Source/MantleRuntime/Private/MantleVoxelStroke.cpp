// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVoxelStroke.h"

#include "MantleVoxelGrid.h"

// Whole numbers only from here on: the same stroke on the same voxels gives the same voxels on every machine.
namespace MantleVoxelStrokePrivate
{
	constexpr const TCHAR* ToolNames[] = { TEXT("dig"), TEXT("fill"), TEXT("flatten"), TEXT("smooth"), TEXT("paint") };

	/** Density per 1/16 voxel of height across a flattened floor: 48 a voxel, the generated ground's slope. */
	constexpr int32 FloorSlope = 3;

	FORCEINLINE int32 FloorDiv(int32 A, int32 B) { return A >= 0 ? A / B : -((-A + B - 1) / B); }

	void PutInt(TArray<uint8>& Out, int32 V)
	{
		const uint32 U = uint32(V);
		Out.Append({ uint8(U & 0xFF), uint8((U >> 8) & 0xFF), uint8((U >> 16) & 0xFF), uint8(U >> 24) });
	}

	int32 GetInt(const uint8* P) { return int32(uint32(P[0]) | (uint32(P[1]) << 8) | (uint32(P[2]) << 16) | (uint32(P[3]) << 24)); }

	struct FNew
	{
		FIntVector P;
		uint8 Material;
		int8 Density;
	};
}

bool FMantleVoxelStroke::IsValid() const
{
	return uint8(Tool) < uint8(ETool::Count) && Radius >= MinRadius && Radius <= MaxRadius && Strength >= 1 && Strength <= 100;
}

void FMantleVoxelStroke::GetBounds(FIntVector& OutMin, FIntVector& OutMax) const
{
	using namespace MantleVoxelStrokePrivate;
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		OutMin[Axis] = FloorDiv(Centre[Axis] - Radius, Fixed);
		OutMax[Axis] = FloorDiv(Centre[Axis] + Radius, Fixed) + 1;
	}
}

void FMantleVoxelStroke::Encode(TArray<uint8>& Out) const
{
	using namespace MantleVoxelStrokePrivate;
	Out.Append({ uint8(Tool), Material, Strength, uint8(Radius & 0xFF), uint8(Radius >> 8) });
	PutInt(Out, Centre.X);
	PutInt(Out, Centre.Y);
	PutInt(Out, Centre.Z);
	PutInt(Out, PlaneZ);
}

bool FMantleVoxelStroke::Decode(TConstArrayView<uint8> Bytes, int32& Offset, FMantleVoxelStroke& Out)
{
	using namespace MantleVoxelStrokePrivate;
	if (Offset < 0 || Offset + EncodedBytes > Bytes.Num())
	{
		return false;
	}
	const uint8* P = Bytes.GetData() + Offset;
	Out.Tool = ETool(P[0]);
	Out.Material = P[1];
	Out.Strength = P[2];
	Out.Radius = P[3] | (P[4] << 8);
	Out.Centre = FIntVector(GetInt(P + 5), GetInt(P + 9), GetInt(P + 13));
	Out.PlaneZ = GetInt(P + 17);
	Offset += EncodedBytes;
	return Out.IsValid();
}

namespace MantleVoxelStrokePrivate
{
	/** A smooth grid, as ApplyStroke reads and writes it. */
	struct FGridAccess
	{
		FMantleVoxelGrid& Grid;

		bool IsInside(const FIntVector& P) const { return Grid.IsInside(P); }
		int32 GetDensity(const FIntVector& P) const { return Grid.GetDensity(P); }
		uint8 GetBlock(const FIntVector& P) const { return Grid.GetBlock(P); }
		int32 NumTypes() const { return Grid.GetTypes().Num(); }
		bool Set(const FIntVector& P, uint8 M, int8 D) { return Grid.SetVoxel(P, M, D) == FMantleVoxelGrid::ESetResult::Changed; }
	};

	/** One section's voxels apart from a grid; a read outside it takes the nearest voxel inside. */
	struct FSectionAccess
	{
		FIntVector Origin;
		uint8* Materials;
		int8* Densities;
		int32 TypeCount;

		static constexpr int32 Side = 16;

		bool IsInside(const FIntVector& P) const
		{
			const FIntVector L = P - Origin;
			return L.X >= 0 && L.Y >= 0 && L.Z >= 0 && L.X < Side && L.Y < Side && L.Z < Side;
		}
		int32 Local(const FIntVector& P) const
		{
			const FIntVector L = P - Origin;
			return FMath::Clamp(L.X, 0, Side - 1) + Side * (FMath::Clamp(L.Y, 0, Side - 1) + Side * FMath::Clamp(L.Z, 0, Side - 1));
		}
		int32 GetDensity(const FIntVector& P) const { return Densities[Local(P)]; }
		uint8 GetBlock(const FIntVector& P) const { return Materials[Local(P)]; }
		int32 NumTypes() const { return TypeCount; }
		bool Set(const FIntVector& P, uint8 M, int8 D)
		{
			// FMantleVoxelGrid::SetVoxel's rule: no unknown material, a density at or below 0 holds no material, and air holds no
			// density above 0.
			if (M >= TypeCount)
			{
				return false;
			}
			const bool bSolid = D > 0 && M != 0;
			const uint8 NewM = bSolid ? M : uint8(0);
			const int8 NewD = bSolid ? D : int8(FMath::Min<int32>(D, 0));
			const int32 I = Local(P);
			if (Materials[I] == NewM && Densities[I] == NewD)
			{
				return false;
			}
			Materials[I] = NewM;
			Densities[I] = NewD;
			return true;
		}
	};

	template <typename TAccess>
	int32 ApplyStroke(const FMantleVoxelStroke& Stroke, TAccess& Access, FIntVector& OutMin, FIntVector& OutMax);
}

int32 FMantleVoxelStroke::Apply(FMantleVoxelGrid& Grid, FIntVector& OutMin, FIntVector& OutMax) const
{
	if (!Grid.IsSmooth() || !IsValid())
	{
		return 0;
	}
	MantleVoxelStrokePrivate::FGridAccess Access{ Grid };
	return MantleVoxelStrokePrivate::ApplyStroke(*this, Access, OutMin, OutMax);
}

int32 FMantleVoxelStroke::ApplyToSection(const FIntVector& Origin, uint8* Materials, int8* Densities, int32 TypeCount) const
{
	if (!IsValid() || !Materials || !Densities)
	{
		return 0;
	}
	MantleVoxelStrokePrivate::FSectionAccess Access{ Origin, Materials, Densities, TypeCount };
	FIntVector Min, Max;
	return MantleVoxelStrokePrivate::ApplyStroke(*this, Access, Min, Max);
}

template <typename TAccess>
int32 MantleVoxelStrokePrivate::ApplyStroke(const FMantleVoxelStroke& Stroke, TAccess& Grid, FIntVector& OutMin, FIntVector& OutMax)
{
	using ETool = FMantleVoxelStroke::ETool;
	constexpr int32 Fixed = FMantleVoxelStroke::Fixed;
	const ETool Tool = Stroke.Tool;
	const uint8 Material = Stroke.Material;
	const int32 Strength = Stroke.Strength;
	const FIntVector Centre = Stroke.Centre;
	const int32 PlaneZ = Stroke.PlaneZ;
	FIntVector Min, Max;
	Stroke.GetBounds(Min, Max);
	const int64 R2 = int64(Stroke.Radius) * Stroke.Radius;
	const bool bKnownMaterial = Material > 0 && Material < Grid.NumTypes();
	// Worked out from the voxels as they were, then written: no voxel sees another's new value (Smooth reads neighbours).
	TArray<FNew> Writes;
	for (int32 Z = Min.Z; Z <= Max.Z; ++Z)
	{
		for (int32 Y = Min.Y; Y <= Max.Y; ++Y)
		{
			for (int32 X = Min.X; X <= Max.X; ++X)
			{
				const FIntVector P(X, Y, Z);
				if (!Grid.IsInside(P))
				{
					continue;
				}
				const int64 DX = int64(X) * Fixed - Centre.X;
				const int64 DY = int64(Y) * Fixed - Centre.Y;
				const int64 DZ = int64(Z) * Fixed - Centre.Z;
				const int64 D2 = DX * DX + DY * DY + DZ * DZ;
				if (D2 >= R2)
				{
					continue;
				}
				// 256 at the centre, 0 at the edge; times strength (1 to 100): 0 to 25,600.
				const int32 Amount = int32(((R2 - D2) * 256) / R2) * Strength;
				const int32 D = Grid.GetDensity(P);
				const uint8 M = Grid.GetBlock(P);
				int32 NewD = D;
				uint8 NewM = M;
				switch (Tool)
				{
				case ETool::Dig:
					NewD = D - Amount * 254 / 25600;
					break;
				case ETool::Fill:
					NewD = D + Amount * 254 / 25600;
					break;
				case ETool::Flatten:
				{
					const int32 Target = FMath::Clamp((PlaneZ - Z * Fixed) * FloorSlope, -127, 127);
					NewD = D + (Target - D) * Amount / 25600;
					break;
				}
				case ETool::Smooth:
				{
					const int32 Sum = Grid.GetDensity(P + FIntVector(-1, 0, 0)) + Grid.GetDensity(P + FIntVector(1, 0, 0)) + Grid.GetDensity(P + FIntVector(0, -1, 0))
						+ Grid.GetDensity(P + FIntVector(0, 1, 0)) + Grid.GetDensity(P + FIntVector(0, 0, -1)) + Grid.GetDensity(P + FIntVector(0, 0, 1));
					NewD = D + (Sum / 6 - D) * Amount / 25600;
					break;
				}
				case ETool::Paint:
					// The inner part, wider the stronger the stroke.
					if (D > 0 && bKnownMaterial && Amount >= 64 * 100)
					{
						NewM = Material;
					}
					break;
				default:
					break;
				}
				NewD = FMath::Clamp(NewD, -127, 127);
				if (NewD > 0 && M == 0)
				{
					// Ground that appears takes the stroke's material, else its first solid neighbour's, else stays empty.
					NewM = bKnownMaterial ? Material : uint8(0);
					static const FIntVector Around[6] = { { -1, 0, 0 }, { 1, 0, 0 }, { 0, -1, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 } };
					for (int32 I = 0; I < 6 && NewM == 0; ++I)
					{
						NewM = Grid.GetBlock(P + Around[I]);
					}
				}
				if (NewD != D || NewM != M)
				{
					Writes.Add({ P, NewM, int8(NewD) });
				}
			}
		}
	}
	int32 Changed = 0;
	for (const FNew& W : Writes)
	{
		if (Grid.Set(W.P, W.Material, W.Density))
		{
			OutMin = Changed == 0 ? W.P : OutMin.ComponentMin(W.P);
			OutMax = Changed == 0 ? W.P : OutMax.ComponentMax(W.P);
			++Changed;
		}
	}
	return Changed;
}

const TCHAR* FMantleVoxelStroke::ToolName(ETool InTool)
{
	return uint8(InTool) < uint8(ETool::Count) ? MantleVoxelStrokePrivate::ToolNames[uint8(InTool)] : TEXT("");
}

bool FMantleVoxelStroke::ParseTool(const FString& Name, ETool& OutTool)
{
	for (uint8 I = 0; I < uint8(ETool::Count); ++I)
	{
		if (Name.Equals(MantleVoxelStrokePrivate::ToolNames[I], ESearchCase::IgnoreCase))
		{
			OutTool = ETool(I);
			return true;
		}
	}
	return false;
}
