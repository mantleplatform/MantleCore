// Copyright Joshua Eger. All Rights Reserved.
#include "MantleScatter.h"

#include "Engine/World.h"
#include "Misc/CoreMisc.h"

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleScatterPrivate
{
	constexpr uint64 ScatterGoldenGamma = 0x9E3779B97F4A7C15ull;

	/** The schema's alignment enum, in EMantleScatterAlignment order. */
	const TCHAR* const GScatterAlignmentNames[] = { TEXT("upright"), TEXT("tilted"), TEXT("tumbled") };
	constexpr int32 GScatterAlignmentCount = static_cast<int32>(UE_ARRAY_COUNT(GScatterAlignmentNames));

	/** SplitMix64's output function: a bijective 64-bit mix. */
	uint64 ScatterMix64(uint64 Z)
	{
		Z = (Z ^ (Z >> 30)) * 0xBF58476D1CE4E5B9ull;
		Z = (Z ^ (Z >> 27)) * 0x94D049BB133111EBull;
		return Z ^ (Z >> 31);
	}

	/**
	 * One instance's own random stream: SplitMix64 started from the seed and the instance's index.
	 * Counter-based, so an instance never depends on how many draws another one made, and the
	 * generation could be split across threads or cells without changing a single instance.
	 */
	struct FScatterStream
	{
		uint64 State;

		FScatterStream(int32 Seed, uint32 Index)
			: State(ScatterMix64((static_cast<uint64>(static_cast<uint32>(Seed)) << 32) | Index))
		{
		}

		uint64 Next()
		{
			State += ScatterGoldenGamma;
			return ScatterMix64(State);
		}
	};

	uint32 ScatterLow(uint64 Value) { return static_cast<uint32>(Value); }
	uint32 ScatterHigh(uint64 Value) { return static_cast<uint32>(Value >> 32); }

	/** A 32-bit draw mapped onto Lo..Hi inclusive, in integers only (Hi - Lo stays under 2^32). */
	int64 ScatterPick(uint32 Draw, int64 Lo, int64 Hi)
	{
		const uint64 Span = static_cast<uint64>(Hi - Lo) + 1;
		return Lo + static_cast<int64>((static_cast<uint64>(Draw) * Span) >> 32);
	}

	bool ValidateScatterParams(const FMantleScatterParams& Params, FString& OutError)
	{
		if (Params.Seed < 0)
		{
			OutError = FString::Printf(TEXT("seed %d must be 0 or more"), Params.Seed);
			return false;
		}
		const FVector& Half = Params.HalfExtent;
		if (!FMath::IsFinite(Half.X) || !FMath::IsFinite(Half.Y) || !FMath::IsFinite(Half.Z)
			|| Half.X <= 0.0 || Half.Y <= 0.0 || Half.Z < 0.0
			|| Half.X > FMantleScatter::MaxHalfExtentCm || Half.Y > FMantleScatter::MaxHalfExtentCm
			|| Half.Z > FMantleScatter::MaxHalfExtentCm)
		{
			OutError = FString::Printf(TEXT("half extent %s must be above 0 in X and Y, at least 0 in Z, and at most %.0f cm"),
				*Half.ToCompactString(), FMantleScatter::MaxHalfExtentCm);
			return false;
		}
		if (!FMath::IsFinite(Params.Density) || Params.Density < 0.0)
		{
			OutError = FString::Printf(TEXT("density %g must be 0 or more instances per square metre"), Params.Density);
			return false;
		}
		// Written so a NaN fails too.
		if (!(Params.MinScale >= FMantleScatter::MinScaleLimit && Params.MaxScale <= FMantleScatter::MaxScaleLimit
			&& Params.MinScale <= Params.MaxScale))
		{
			OutError = FString::Printf(TEXT("scale range %g to %g must be ordered and within %g to %g"),
				Params.MinScale, Params.MaxScale, FMantleScatter::MinScaleLimit, FMantleScatter::MaxScaleLimit);
			return false;
		}
		if (!(Params.MaxTiltDegrees >= 0.0 && Params.MaxTiltDegrees <= FMantleScatter::MaxTiltLimit))
		{
			OutError = FString::Printf(TEXT("max tilt %g must be 0 to %g degrees"), Params.MaxTiltDegrees, FMantleScatter::MaxTiltLimit);
			return false;
		}
		if (Params.Meshes.Num() == 0 || Params.Meshes.Num() > FMantleScatter::MaxMeshes)
		{
			OutError = FString::Printf(TEXT("%d meshes; a scatter takes 1 to %d"), Params.Meshes.Num(), FMantleScatter::MaxMeshes);
			return false;
		}
		int64 TotalWeight = 0;
		for (int32 Index = 0; Index < Params.Meshes.Num(); ++Index)
		{
			const FMantleScatterMesh& Mesh = Params.Meshes[Index];
			if (Mesh.MeshId.IsEmpty())
			{
				OutError = FString::Printf(TEXT("mesh %d has no id"), Index);
				return false;
			}
			if (Mesh.Weight < 0 || Mesh.Weight > FMantleScatter::MaxMeshWeight)
			{
				OutError = FString::Printf(TEXT("mesh %d (%s) has a weight of %d; a weight is 0 to %d"),
					Index, *Mesh.MeshId, Mesh.Weight, FMantleScatter::MaxMeshWeight);
				return false;
			}
			TotalWeight += Mesh.Weight;
		}
		if (TotalWeight <= 0)
		{
			OutError = TEXT("every mesh weighs 0; at least one must weigh more");
			return false;
		}
		return true;
	}

	/** Half a size in whole millimetres, rounded half up: the fixed point every count starts from. */
	int64 ScatterHalfMillimetres(double HalfCm)
	{
		// Two statements, not one expression, so no compiler fuses the multiply and the add into one
		// rounding: the backend and validate.py round each step, and the counts must agree exactly.
		const double Millimetres = HalfCm * 10.0;
		return static_cast<int64>(FMath::FloorToDouble(Millimetres + 0.5));
	}
}

const TCHAR* FMantleScatter::AlignmentName(const EMantleScatterAlignment Alignment)
{
	const int32 Index = static_cast<int32>(Alignment);
	return Index >= 0 && Index < MantleScatterPrivate::GScatterAlignmentCount
		? MantleScatterPrivate::GScatterAlignmentNames[Index]
		: TEXT("upright");
}

bool FMantleScatter::ParseAlignment(const FString& Name, EMantleScatterAlignment& Out)
{
	for (int32 Index = 0; Index < MantleScatterPrivate::GScatterAlignmentCount; ++Index)
	{
		if (Name.Equals(MantleScatterPrivate::GScatterAlignmentNames[Index], ESearchCase::CaseSensitive))
		{
			Out = static_cast<EMantleScatterAlignment>(Index);
			return true;
		}
	}
	return false;
}

int64 FMantleScatter::CountFor(const FMantleScatterParams& Params, FString& OutError)
{
	using namespace MantleScatterPrivate;

	if (!ValidateScatterParams(Params, OutError))
	{
		return -1;
	}

	// The footprint in whole square millimetres (exact in a double up to the extent cap), then
	// square metres by one division, so 100 m x 100 m at density 1 is 10000. validate.py and the
	// backend do exactly these IEEE operations, so all three agree on every count.
	const int64 AreaMm2 = 4 * ScatterHalfMillimetres(Params.HalfExtent.X) * ScatterHalfMillimetres(Params.HalfExtent.Y);
	const double AreaM2 = static_cast<double>(AreaMm2) / 1.0e6;
	const double Wanted = AreaM2 * Params.Density;
	const double Rounded = FMath::FloorToDouble(Wanted + 0.5);
	if (!(Rounded <= static_cast<double>(MaxInstances)))
	{
		OutError = FString::Printf(TEXT("%.0f square metres at density %g makes %.0f instances; a scatter makes at most %d"),
			AreaM2, Params.Density, Rounded, MaxInstances);
		return -1;
	}
	return static_cast<int64>(Rounded);
}

bool FMantleScatter::Generate(const FMantleScatterParams& Params, TArray<FMantleScatterInstance>& OutInstances, FString& OutError)
{
	using namespace MantleScatterPrivate;

	OutInstances.Reset();
	const int64 Count = CountFor(Params, OutError);
	if (Count < 0)
	{
		return false;
	}

	const int64 HalfX = ScatterHalfMillimetres(Params.HalfExtent.X);
	const int64 HalfY = ScatterHalfMillimetres(Params.HalfExtent.Y);
	const int64 HalfZ = ScatterHalfMillimetres(Params.HalfExtent.Z);
	const int64 ScaleLo = FMath::Clamp<int64>(FMath::RoundToInt64(Params.MinScale * 1000.0), 1, MAX_uint16);
	const int64 ScaleHi = FMath::Clamp<int64>(FMath::RoundToInt64(Params.MaxScale * 1000.0), ScaleLo, MAX_uint16);
	const int64 Tilt = FMath::Clamp<int64>(FMath::RoundToInt64(Params.MaxTiltDegrees * 100.0), 0, 9000);

	// Cumulative weights: a draw below Cumulative[i] and at or above Cumulative[i - 1] picks mesh i,
	// so a weight of 0 is never picked.
	TArray<int64, TInlineAllocator<MaxMeshes>> Cumulative;
	int64 TotalWeight = 0;
	for (const FMantleScatterMesh& Mesh : Params.Meshes)
	{
		TotalWeight += Mesh.Weight;
		Cumulative.Add(TotalWeight);
	}

	OutInstances.SetNumUninitialized(static_cast<int32>(Count));
	for (int32 Index = 0; Index < OutInstances.Num(); ++Index)
	{
		// Four 64-bit draws per instance, always all four, so changing the alignment, the scale
		// range or the meshes never moves an instance.
		FScatterStream Stream(Params.Seed, static_cast<uint32>(Index));
		const uint64 Place = Stream.Next();
		const uint64 Height = Stream.Next();
		const uint64 Turn = Stream.Next();
		const uint64 Look = Stream.Next();

		FMantleScatterInstance& Instance = OutInstances[Index];
		Instance.X = static_cast<int32>(ScatterPick(ScatterLow(Place), -HalfX, HalfX));
		Instance.Y = static_cast<int32>(ScatterPick(ScatterHigh(Place), -HalfY, HalfY));
		Instance.Z = static_cast<int32>(ScatterPick(ScatterLow(Height), -HalfZ, HalfZ));
		Instance.Yaw = static_cast<uint16>(ScatterPick(ScatterHigh(Height), 0, 35999));
		switch (Params.Alignment)
		{
		case EMantleScatterAlignment::Tilted:
			Instance.Pitch = static_cast<int16>(ScatterPick(ScatterLow(Turn), -Tilt, Tilt));
			Instance.Roll = static_cast<int16>(ScatterPick(ScatterHigh(Turn), -Tilt, Tilt));
			break;
		case EMantleScatterAlignment::Tumbled:
			Instance.Pitch = static_cast<int16>(ScatterPick(ScatterLow(Turn), -9000, 9000));
			Instance.Roll = static_cast<int16>(ScatterPick(ScatterHigh(Turn), -18000, 17999));
			break;
		case EMantleScatterAlignment::Upright:
		default:
			Instance.Pitch = 0;
			Instance.Roll = 0;
			break;
		}
		Instance.Scale = static_cast<uint16>(ScatterPick(ScatterLow(Look), ScaleLo, ScaleHi));

		const int64 Pick = ScatterPick(ScatterHigh(Look), 0, TotalWeight - 1);
		int32 MeshIndex = 0;
		while (MeshIndex < Cumulative.Num() - 1 && Pick >= Cumulative[MeshIndex])
		{
			++MeshIndex;
		}
		Instance.Mesh = static_cast<uint16>(MeshIndex);
	}
	return true;
}

FTransform FMantleScatter::ToTransform(const FMantleScatterInstance& Instance)
{
	const FRotator Rotation(Instance.Pitch * 0.01, Instance.Yaw * 0.01, Instance.Roll * 0.01);
	const FVector Location(Instance.X * 0.1, Instance.Y * 0.1, Instance.Z * 0.1);
	const double Scale = Instance.Scale * 0.001;
	return FTransform(Rotation, Location, FVector(Scale));
}

uint64 FMantleScatter::Hash(TConstArrayView<FMantleScatterInstance> Instances)
{
	uint64 Result = 0xCBF29CE484222325ull;
	auto Mix = [&Result](uint32 Value)
	{
		for (int32 Byte = 0; Byte < 4; ++Byte)
		{
			Result ^= (Value >> (Byte * 8)) & 0xFFu;
			Result *= 0x100000001B3ull;
		}
	};
	Mix(static_cast<uint32>(Instances.Num()));
	for (const FMantleScatterInstance& Instance : Instances)
	{
		Mix(static_cast<uint32>(Instance.X));
		Mix(static_cast<uint32>(Instance.Y));
		Mix(static_cast<uint32>(Instance.Z));
		Mix(static_cast<uint32>(Instance.Yaw) | (static_cast<uint32>(static_cast<uint16>(Instance.Pitch)) << 16));
		Mix(static_cast<uint32>(static_cast<uint16>(Instance.Roll)) | (static_cast<uint32>(Instance.Scale) << 16));
		Mix(static_cast<uint32>(Instance.Mesh));
	}
	return Result;
}

FString FMantleScatter::HashText(const uint64 InHash)
{
	return FString::Printf(TEXT("%08x%08x"), static_cast<uint32>(InHash >> 32), static_cast<uint32>(InHash));
}

bool FMantleScatter::DrawsScatter(const UWorld* World)
{
#if UE_SERVER
	(void)World;
	return false;
#else
	return World && World->GetNetMode() != NM_DedicatedServer && !IsRunningDedicatedServer();
#endif
}
