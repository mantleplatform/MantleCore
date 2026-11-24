// Copyright Joshua Eger. All Rights Reserved.
#include "MantleMinimapDrawing.h"

#include "HAL/PlatformAtomics.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleMinimapDrawingPrivate
{
	/** A float as an unsigned integer that sorts the same way, so the top-most height is an integer max. */
	uint32 OrderedFromFloat(float Value)
	{
		uint32 Bits = 0;
		FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
		return (Bits & 0x80000000u) ? ~Bits : (Bits | 0x80000000u);
	}

	float FloatFromOrdered(uint32 Ordered)
	{
		const uint32 Bits = (Ordered & 0x80000000u) ? (Ordered & 0x7FFFFFFFu) : ~Ordered;
		float Value = 0.0f;
		FMemory::Memcpy(&Value, &Bits, sizeof(Value));
		return Value;
	}

	int64 Pack(float Z, EMantleMinimapSurface Surface)
	{
		return (static_cast<int64>(OrderedFromFloat(Z)) << 8) | static_cast<int64>(Surface);
	}

	/** The name's words, lower case: split at anything not a letter or digit, at a lower-to-upper case change and between letters and digits. */
	TArray<FString> Words(const FString& Name)
	{
		TArray<FString> Out;
		FString Current;
		TCHAR Previous = 0;
		auto Flush = [&Out, &Current]()
		{
			if (!Current.IsEmpty())
			{
				Out.Add(Current.ToLower());
				Current.Reset();
			}
		};
		for (const TCHAR Char : Name)
		{
			if (!FChar::IsAlnum(Char))
			{
				Flush();
				Previous = 0;
				continue;
			}
			const bool bBreak = Previous != 0
				&& ((FChar::IsLower(Previous) && FChar::IsUpper(Char))
					|| (FChar::IsDigit(Previous) != FChar::IsDigit(Char)));
			if (bBreak)
			{
				Flush();
			}
			Current.AppendChar(Char);
			Previous = Char;
		}
		Flush();
		return Out;
	}

	bool HasWord(const TArray<FString>& Words, std::initializer_list<const TCHAR*> Wanted)
	{
		for (const FString& Word : Words)
		{
			// "roads" and "trees" name the same thing as "road" and "tree".
			const FString Single = Word.Len() > 3 && Word.EndsWith(TEXT("s")) ? Word.LeftChop(1) : Word;
			for (const TCHAR* Candidate : Wanted)
			{
				if (Word.Equals(Candidate) || Single.Equals(Candidate))
				{
					return true;
				}
			}
		}
		return false;
	}

	/** The lowest value within Radius of each of Count values Stride apart, in place (a sliding minimum, O(Count)). */
	void SlidingMin(float* Values, int32 Count, int32 Stride, int32 Radius, TArray<int32>& Queue, TArray<float>& Line)
	{
		Line.SetNumUninitialized(Count);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Line[Index] = Values[Index * Stride];
		}
		Queue.SetNumUninitialized(Count);
		int32 Head = 0, Tail = 0;
		int32 Next = 0;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			// Add every value up to Index + Radius, keeping the queue's values increasing.
			for (; Next < Count && Next <= Index + Radius; ++Next)
			{
				while (Tail > Head && Line[Queue[Tail - 1]] >= Line[Next])
				{
					--Tail;
				}
				Queue[Tail++] = Next;
			}
			while (Queue[Head] < Index - Radius)
			{
				++Head;
			}
			Values[Index * Stride] = Line[Queue[Head]];
		}
	}

	FColor Scale(FColor Color, float Factor)
	{
		return FColor(
			static_cast<uint8>(FMath::Clamp(Color.R * Factor, 0.0f, 255.0f)),
			static_cast<uint8>(FMath::Clamp(Color.G * Factor, 0.0f, 255.0f)),
			static_cast<uint8>(FMath::Clamp(Color.B * Factor, 0.0f, 255.0f)),
			255);
	}

	FColor Lerp(FColor A, FColor B, float Alpha)
	{
		return FColor(
			static_cast<uint8>(FMath::RoundToInt(FMath::Lerp(static_cast<float>(A.R), static_cast<float>(B.R), Alpha))),
			static_cast<uint8>(FMath::RoundToInt(FMath::Lerp(static_cast<float>(A.G), static_cast<float>(B.G), Alpha))),
			static_cast<uint8>(FMath::RoundToInt(FMath::Lerp(static_cast<float>(A.B), static_cast<float>(B.B), Alpha))),
			255);
	}

	/** A building's roof color at its foot and at TallCm above the ground: taller reads a little darker. */
	const FColor BuildingLow(216, 210, 200);
	const FColor BuildingHigh(184, 176, 164);
	constexpr double TallCm = 6000.0;

	/** An outline pixel is its surface's color this much darker. */
	constexpr float OutlineFactor = 0.70f;
}

FMantleMinimapGrid FMantleMinimapGrid::Plan(const FBox2D& Area)
{
	FMantleMinimapGrid Grid;
	if (!Area.bIsValid)
	{
		return Grid;
	}
	const FVector2D Size = Area.GetSize();
	const double Longer = FMath::Max3(Size.X, Size.Y, 1.0);
	double CmPerPixel = DefaultCmPerPixel;
	if (Longer / CmPerPixel > MaxPixels)
	{
		CmPerPixel = Longer / MaxPixels;
	}
	if (Longer / CmPerPixel < MinPixels)
	{
		CmPerPixel = Longer / MinPixels;
	}
	// A hair under whole pixels is one pixel, not two: Longer / CmPerPixel is exactly MaxPixels or MinPixels at the clamps.
	const int32 Wide = FMath::Max(1, FMath::CeilToInt32(Size.X / CmPerPixel - 1e-6));
	const int32 High = FMath::Max(1, FMath::CeilToInt32(Size.Y / CmPerPixel - 1e-6));
	Grid.CmPerPixel = CmPerPixel;
	Grid.Columns = FMath::Clamp(FMath::DivideAndRoundUp(Wide, MaxTileSide), 1, MaxTilesPerSide);
	Grid.Rows = FMath::Clamp(FMath::DivideAndRoundUp(High, MaxTileSide), 1, MaxTilesPerSide);
	Grid.TileWidth = FMath::DivideAndRoundUp(Wide, Grid.Columns);
	Grid.TileHeight = FMath::DivideAndRoundUp(High, Grid.Rows);
	Grid.Width = Grid.TileWidth * Grid.Columns;
	Grid.Height = Grid.TileHeight * Grid.Rows;
	const FVector2D Center = Area.GetCenter();
	Grid.MinX = Center.X - Grid.Width * CmPerPixel * 0.5;
	Grid.MaxX = Grid.MinX + Grid.Width * CmPerPixel;
	Grid.MinY = Center.Y - Grid.Height * CmPerPixel * 0.5;
	Grid.MaxY = Grid.MinY + Grid.Height * CmPerPixel;
	return Grid;
}

FBox2D FMantleMinimapGrid::TileBounds(int32 Row, int32 Column) const
{
	const double X0 = MinX + Column * TileWidth * CmPerPixel;
	const double Y0 = MinY + Row * TileHeight * CmPerPixel;
	return FBox2D(FVector2D(X0, Y0), FVector2D(X0 + TileWidth * CmPerPixel, Y0 + TileHeight * CmPerPixel));
}

FMantleMinimapCanvas::FMantleMinimapCanvas(const FMantleMinimapGrid& InGrid)
	: Grid(InGrid)
{
	Cells.SetNumZeroed(Grid.IsValid() ? static_cast<int64>(Grid.Width) * Grid.Height : 0);
}

void FMantleMinimapCanvas::DrawTriangle(const FVector& A, const FVector& B, const FVector& C, EMantleMinimapSurface Surface)
{
	using namespace MantleMinimapDrawingPrivate;
	if (!Grid.IsValid() || Surface == EMantleMinimapSurface::None || Surface >= EMantleMinimapSurface::Count)
	{
		return;
	}
	const FVector2D PA = Grid.ToPixel(A.X, A.Y), PB = Grid.ToPixel(B.X, B.Y), PC = Grid.ToPixel(C.X, C.Y);
	const double Area2 = (PB.X - PA.X) * (PC.Y - PA.Y) - (PB.Y - PA.Y) * (PC.X - PA.X);
	if (FMath::Abs(Area2) < 1e-9 || !FMath::IsFinite(Area2))
	{
		return;
	}
	const int32 X0 = FMath::Max(0, FMath::FloorToInt32(FMath::Min3(PA.X, PB.X, PC.X) - 0.5));
	const int32 X1 = FMath::Min(Grid.Width - 1, FMath::CeilToInt32(FMath::Max3(PA.X, PB.X, PC.X) - 0.5));
	const int32 Y0 = FMath::Max(0, FMath::FloorToInt32(FMath::Min3(PA.Y, PB.Y, PC.Y) - 0.5));
	const int32 Y1 = FMath::Min(Grid.Height - 1, FMath::CeilToInt32(FMath::Max3(PA.Y, PB.Y, PC.Y) - 0.5));
	if (X0 > X1 || Y0 > Y1)
	{
		return;
	}
	const double Lift = static_cast<double>(Surface) * RankLiftCm;
	const double Inverse = 1.0 / Area2;
	for (int32 Y = Y0; Y <= Y1; ++Y)
	{
		const double SY = Y + 0.5;
		for (int32 X = X0; X <= X1; ++X)
		{
			const double SX = X + 0.5;
			// Each corner's weight: the signed area of the triangle the sample makes with the opposite edge.
			const double WA = ((PC.X - PB.X) * (SY - PB.Y) - (PC.Y - PB.Y) * (SX - PB.X)) * Inverse;
			const double WB = ((PA.X - PC.X) * (SY - PC.Y) - (PA.Y - PC.Y) * (SX - PC.X)) * Inverse;
			const double WC = 1.0 - WA - WB;
			if (WA < 0.0 || WB < 0.0 || WC < 0.0)
			{
				continue;
			}
			const double Z = WA * A.Z + WB * B.Z + WC * C.Z + Lift;
			const int64 Key = Pack(static_cast<float>(Z), Surface);
			int64* Slot = &Cells[static_cast<int64>(Y) * Grid.Width + X];
			int64 Seen = FPlatformAtomics::AtomicRead(Slot);
			while (Key > Seen)
			{
				const int64 Before = FPlatformAtomics::InterlockedCompareExchange(Slot, Key, Seen);
				if (Before == Seen)
				{
					break;
				}
				Seen = Before;
			}
		}
	}
}

bool FMantleMinimapCanvas::Top(int32 X, int32 Y, double& OutZ, EMantleMinimapSurface& OutSurface) const
{
	using namespace MantleMinimapDrawingPrivate;
	if (X < 0 || Y < 0 || X >= Grid.Width || Y >= Grid.Height)
	{
		return false;
	}
	const int64 Cell = Cells[static_cast<int64>(Y) * Grid.Width + X];
	if (Cell == 0)
	{
		return false;
	}
	OutSurface = static_cast<EMantleMinimapSurface>(Cell & 0xFF);
	OutZ = FloatFromOrdered(static_cast<uint32>(Cell >> 8));
	return true;
}

int64 FMantleMinimapCanvas::CountCovered() const
{
	int64 Count = 0;
	for (const int64 Cell : Cells)
	{
		Count += Cell != 0 ? 1 : 0;
	}
	return Count;
}

TArray<FColor> FMantleMinimapCanvas::Shade() const
{
	using namespace MantleMinimapDrawingPrivate;
	TArray<FColor> Pixels;
	if (!Grid.IsValid())
	{
		return Pixels;
	}
	const int32 W = Grid.Width, H = Grid.Height;
	const int64 N = static_cast<int64>(W) * H;
	TArray<float> Heights;
	TArray<EMantleMinimapSurface> Surfaces;
	Heights.SetNumUninitialized(N);
	Surfaces.SetNumUninitialized(N);
	for (int64 Index = 0; Index < N; ++Index)
	{
		const int64 Cell = Cells[Index];
		Surfaces[Index] = static_cast<EMantleMinimapSurface>(Cell & 0xFF);
		Heights[Index] = Cell == 0 ? TNumericLimits<float>::Max() : FloatFromOrdered(static_cast<uint32>(Cell >> 8));
	}

	// The ground each pixel stands on: the lowest surface within GroundRadiusCm (rows, then columns).
	TArray<float> Ground = Heights;
	{
		const int32 Radius = FMath::Max(1, FMath::CeilToInt32(GroundRadiusCm / Grid.CmPerPixel));
		TArray<int32> Queue;
		TArray<float> Line;
		for (int32 Y = 0; Y < H; ++Y)
		{
			SlidingMin(&Ground[static_cast<int64>(Y) * W], W, 1, Radius, Queue, Line);
		}
		for (int32 X = 0; X < W; ++X)
		{
			SlidingMin(&Ground[X], H, W, Radius, Queue, Line);
		}
	}

	// Roofs: each connected stretch of building, or of a surface no name placed, without a step of JoinStepCm, as one. Its rise is the
	// most any of it stands above the ground just around it (its edges do, even when it is far wider than GroundRadiusCm). An unnamed
	// stretch that rises more than BuildingRiseCm is a building, else ground; a building's color darkens with its rise.
	TArray<float> Rise;
	Rise.SetNumZeroed(N);
	{
		TArray<int32> Region;
		Region.Init(INDEX_NONE, N);
		TArray<int64> Members;
		TArray<int64> Stack;
		for (int64 Start = 0; Start < N; ++Start)
		{
			const EMantleMinimapSurface Kind = Surfaces[Start];
			if (Region[Start] != INDEX_NONE || (Kind != EMantleMinimapSurface::Unknown && Kind != EMantleMinimapSurface::Building))
			{
				continue;
			}
			Members.Reset();
			Stack.Reset();
			Stack.Add(Start);
			Region[Start] = 1;
			float Most = 0.0f;
			while (Stack.Num() > 0)
			{
				const int64 Index = Stack.Pop(EAllowShrinking::No);
				Members.Add(Index);
				Most = FMath::Max(Most, Heights[Index] - Ground[Index]);
				const int32 X = static_cast<int32>(Index % W), Y = static_cast<int32>(Index / W);
				const int64 Neighbors[] = { X > 0 ? Index - 1 : -1, X + 1 < W ? Index + 1 : -1, Y > 0 ? Index - W : -1, Y + 1 < H ? Index + W : -1 };
				for (const int64 Other : Neighbors)
				{
					if (Other >= 0 && Region[Other] == INDEX_NONE && Surfaces[Other] == Kind && FMath::Abs(Heights[Other] - Heights[Index]) < JoinStepCm)
					{
						Region[Other] = 1;
						Stack.Add(Other);
					}
				}
			}
			const bool bBuilding = Kind == EMantleMinimapSurface::Building || Most > BuildingRiseCm;
			for (const int64 Index : Members)
			{
				Surfaces[Index] = bBuilding ? EMantleMinimapSurface::Building : EMantleMinimapSurface::Ground;
				Rise[Index] = Most;
			}
		}
	}

	Pixels.SetNumUninitialized(N);
	for (int32 Y = 0; Y < H; ++Y)
	{
		for (int32 X = 0; X < W; ++X)
		{
			const int64 Index = static_cast<int64>(Y) * W + X;
			const EMantleMinimapSurface Surface = Surfaces[Index];
			FColor Color = ColorOf(Surface);
			if (Surface == EMantleMinimapSurface::Building)
			{
				Color = Lerp(BuildingLow, BuildingHigh, FMath::Clamp(static_cast<float>(Rise[Index] / TallCm), 0.0f, 1.0f));
			}
			// The outline: on this side of a border with a lower-ranked surface, or of a drop to a lower part of the same one.
			bool bOutline = false;
			if (Surface != EMantleMinimapSurface::None)
			{
				static const FIntPoint Steps[] = { FIntPoint(1, 0), FIntPoint(-1, 0), FIntPoint(0, 1), FIntPoint(0, -1) };
				for (const FIntPoint& Step : Steps)
				{
					const int32 NX = X + Step.X, NY = Y + Step.Y;
					if (NX < 0 || NY < 0 || NX >= W || NY >= H)
					{
						continue;
					}
					const int64 Other = static_cast<int64>(NY) * W + NX;
					if (Surfaces[Other] < Surface
						|| (Surfaces[Other] == Surface && Surface == EMantleMinimapSurface::Building && Heights[Index] - Heights[Other] > StepOutlineCm))
					{
						bOutline = true;
						break;
					}
				}
			}
			Pixels[Index] = bOutline ? Scale(Color, OutlineFactor) : Color;
		}
	}
	return Pixels;
}

FColor FMantleMinimapCanvas::ColorOf(EMantleMinimapSurface Surface)
{
	switch (Surface)
	{
	case EMantleMinimapSurface::Unknown:
	case EMantleMinimapSurface::Ground: return FColor(232, 229, 222, 255);
	case EMantleMinimapSurface::Green: return FColor(186, 220, 162, 255);
	case EMantleMinimapSurface::Water: return FColor(156, 200, 240, 255);
	case EMantleMinimapSurface::Rail: return FColor(176, 168, 186, 255);
	case EMantleMinimapSurface::Road: return FColor(252, 252, 252, 255);
	case EMantleMinimapSurface::Building: return MantleMinimapDrawingPrivate::BuildingLow;
	default: return FColor(38, 42, 50, 255);
	}
}

EMantleMinimapSurface FMantleMinimapCanvas::Classify(const FString& Name)
{
	using namespace MantleMinimapDrawingPrivate;
	const TArray<FString> All = Words(Name);
	// A guard rail or a hand rail is a fence, not a railway.
	const bool bFence = HasWord(All, { TEXT("guard"), TEXT("hand"), TEXT("railing") });
	auto SurfaceOfWord = [bFence](const FString& Word)
	{
		const TArray<FString> One = { Word };
		if (HasWord(One, { TEXT("water"), TEXT("river"), TEXT("ocean"), TEXT("lake"), TEXT("sea"), TEXT("pond"), TEXT("canal") }))
		{
			return EMantleMinimapSurface::Water;
		}
		if (!bFence && HasWord(One, { TEXT("rail"), TEXT("railway"), TEXT("railroad"), TEXT("tram"), TEXT("train") }))
		{
			return EMantleMinimapSurface::Rail;
		}
		if (HasWord(One, { TEXT("road"), TEXT("roadway"), TEXT("street"), TEXT("asphalt"), TEXT("tarmac"), TEXT("highway"), TEXT("freeway"),
			TEXT("motorway"), TEXT("crosswalk"), TEXT("avenue") }))
		{
			return EMantleMinimapSurface::Road;
		}
		if (HasWord(One, { TEXT("building"), TEXT("bldg"), TEXT("house"), TEXT("wall"), TEXT("roof"), TEXT("tower"), TEXT("skyscraper"),
			TEXT("apartment"), TEXT("shop"), TEXT("store"), TEXT("facade"), TEXT("office"), TEXT("warehouse"), TEXT("garage"), TEXT("church"),
			TEXT("hut"), TEXT("cabin"), TEXT("scaffold"), TEXT("construction") }))
		{
			return EMantleMinimapSurface::Building;
		}
		if (HasWord(One, { TEXT("green"), TEXT("greenspace"), TEXT("grass"), TEXT("park"), TEXT("lawn"), TEXT("foliage"), TEXT("tree"),
			TEXT("leaf"), TEXT("leaves"), TEXT("bush"), TEXT("hedge"), TEXT("shrub"), TEXT("plant"), TEXT("moss"), TEXT("garden"), TEXT("forest") }))
		{
			return EMantleMinimapSurface::Green;
		}
		if (HasWord(One, { TEXT("terrain"), TEXT("ground"), TEXT("landscape"), TEXT("sidewalk"), TEXT("pavement"), TEXT("plaza"), TEXT("surface"),
			TEXT("floor") }))
		{
			return EMantleMinimapSurface::Ground;
		}
		return EMantleMinimapSurface::None;
	};
	// The last word that says something decides, as in English: "WaterGround" is ground by the water, "RoadMain" a road. A bridge is
	// what it carries ("RAIL_BRIDGE" rail), so it is a road only when no other word says.
	bool bBridge = false;
	for (int32 Index = All.Num() - 1; Index >= 0; --Index)
	{
		const EMantleMinimapSurface Surface = SurfaceOfWord(All[Index]);
		if (Surface != EMantleMinimapSurface::None)
		{
			return Surface;
		}
		bBridge |= HasWord({ All[Index] }, { TEXT("bridge") });
	}
	return bBridge ? EMantleMinimapSurface::Road : EMantleMinimapSurface::None;
}
