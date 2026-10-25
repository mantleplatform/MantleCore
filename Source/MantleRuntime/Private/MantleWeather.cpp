// Copyright Joshua Eger. All Rights Reserved.
#include "MantleWeather.h"

#include "Misc/Parse.h"

namespace
{
	/** Hash salts: one per independent decision, so no two decisions share random bits. */
	constexpr uint32 SaltSlotLength = 1;
	constexpr uint32 SaltSlotState = 2;
	constexpr uint32 SaltAfterStorm = 3;
	constexpr uint32 SaltStrikeInterval = 4;
	constexpr uint32 SaltStrikeAccept = 5;
	constexpr uint32 SaltStrikeBrightness = 6;
	constexpr uint32 SaltStrikeCloudFrame = 7;

	/** 0 at From or below, 1 at To or above, linear between. */
	double WeatherLookRamp(double Value, double From, double To)
	{
		return FMath::Clamp((Value - From) / (To - From), 0.0, 1.0);
	}

	/** Piecewise-linear through (X, Y) sorted by X, held flat past either end. */
	template <int32 N>
	double WeatherLookTable(const double (&X)[N], const double (&Y)[N], double Value)
	{
		if (Value <= X[0])
		{
			return Y[0];
		}
		for (int32 Index = 1; Index < N; ++Index)
		{
			if (Value <= X[Index])
			{
				return FMath::Lerp(Y[Index - 1], Y[Index], (Value - X[Index - 1]) / (X[Index] - X[Index - 1]));
			}
		}
		return Y[N - 1];
	}

	/**
	 * The draw's cumulative weights, per ten thousand, over the natural states in EMantleWeatherState
	 * order (Clear, PartlyCloudy, Overcast, Rain, Storm, Cloudy, Foggy, LightRain). Not the documented
	 * shares (SharePercent): after a Storm the next slot is always Overcast, Rain or LightRain (f, in
	 * their own proportion 10 : 5 : 6), which on its own would take from the rest. Solving
	 * pi = (1 - pi_storm) q + pi_storm f for the draw q gives 32.65 / 25.51 / 9.23 / 4.62 / 2.04 /
	 * 15.31 / 5.10 / 5.54, so the long-run shares come out at the documented ones again.
	 */
	constexpr uint32 DrawCumulativePerTenThousand[] = { 3265, 5816, 6739, 7201, 7405, 8936, 9446, 10000 };
	static_assert(UE_ARRAY_COUNT(DrawCumulativePerTenThousand) == static_cast<SIZE_T>(MantleNaturalWeatherStateCount),
		"one draw weight per natural state");

	constexpr int32 NumWeatherStates = MantleWeatherStateCount;
	static_assert(static_cast<int32>(MantleLastWeatherState) + 1 == MantleWeatherStateCount, "MantleWeatherStateCount counts every state");
	static_assert(static_cast<int32>(EMantleWeatherState::LightRain) + 1 == MantleNaturalWeatherStateCount,
		"the natural states come first and end at LightRain");

	/**
	 * Ultra Dynamic Sky's fog density for a weather (its Current Fog Density) over its base density, which Mantle's
	 * default fog has: (0.0055 + max(0.1 (Fog/10)^4, 0.5 (Dust/10)^4) + 0.002 overcast) / 0.0055, its overcast being
	 * its cover 4 to 7 of 10 mapped to 0-1. Every argument is on its 0-10 scale.
	 */
	double PackageFogDensityScale(double Fog10, double Dust10, double Cover10)
	{
		const double Fog = Fog10 / 10.0;
		const double Dust = Dust10 / 10.0;
		const double Overcast = FMath::Clamp((Cover10 - 4.0) / 3.0, 0.0, 1.0);
		const double Density = 0.0055 + FMath::Max(0.1 * Fog * Fog * Fog * Fog, 0.5 * Dust * Dust * Dust * Dust) + 0.002 * Overcast;
		return Density / 0.0055;
	}

	uint64 SplitMix64(uint64 Z)
	{
		Z += 0x9E3779B97F4A7C15ull;
		Z = (Z ^ (Z >> 30)) * 0xBF58476D1CE4E5B9ull;
		Z = (Z ^ (Z >> 27)) * 0x94D049BB133111EBull;
		return Z ^ (Z >> 31);
	}

	/** Sanity bound on how far ahead a query can reach: about 31 years of environment time. */
	constexpr double MaxWeatherSeconds = 1.0e9;

	double ClampWeatherSeconds(double Seconds)
	{
		// NaN compares false both ways, so it lands on zero too.
		return (Seconds > 0.0) ? FMath::Min(Seconds, MaxWeatherSeconds) : 0.0;
	}
}

const TCHAR* MantleWeatherStateName(EMantleWeatherState State)
{
	switch (State)
	{
	case EMantleWeatherState::Clear: return TEXT("Clear");
	case EMantleWeatherState::PartlyCloudy: return TEXT("PartlyCloudy");
	case EMantleWeatherState::Overcast: return TEXT("Overcast");
	case EMantleWeatherState::Rain: return TEXT("Rain");
	case EMantleWeatherState::Storm: return TEXT("Storm");
	case EMantleWeatherState::Cloudy: return TEXT("Cloudy");
	case EMantleWeatherState::Foggy: return TEXT("Foggy");
	case EMantleWeatherState::LightRain: return TEXT("LightRain");
	case EMantleWeatherState::LightSnow: return TEXT("LightSnow");
	case EMantleWeatherState::Snow: return TEXT("Snow");
	case EMantleWeatherState::Blizzard: return TEXT("Blizzard");
	case EMantleWeatherState::DustCalm: return TEXT("DustCalm");
	case EMantleWeatherState::DustStorm: return TEXT("DustStorm");
	}
	return TEXT("Clear");
}

bool MantleParseWeatherState(const FString& Name, EMantleWeatherState& OutState)
{
	for (int32 Index = 0; Index < NumWeatherStates; ++Index)
	{
		const EMantleWeatherState State = static_cast<EMantleWeatherState>(Index);
		if (Name.Equals(MantleWeatherStateName(State), ESearchCase::CaseSensitive))
		{
			OutState = State;
			return true;
		}
	}
	return false;
}

FMantleWeatherValues FMantleWeatherValues::Lerp(const FMantleWeatherValues& A, const FMantleWeatherValues& B, double Alpha)
{
	FMantleWeatherValues Out;
	Out.CloudCover = FMath::Lerp(A.CloudCover, B.CloudCover, Alpha);
	Out.FogDensityScale = FMath::Lerp(A.FogDensityScale, B.FogDensityScale, Alpha);
	Out.RainIntensity = FMath::Lerp(A.RainIntensity, B.RainIntensity, Alpha);
	Out.LightningPerMinute = FMath::Lerp(A.LightningPerMinute, B.LightningPerMinute, Alpha);
	Out.SnowIntensity = FMath::Lerp(A.SnowIntensity, B.SnowIntensity, Alpha);
	Out.DustIntensity = FMath::Lerp(A.DustIntensity, B.DustIntensity, Alpha);
	Out.WindIntensity = FMath::Lerp(A.WindIntensity, B.WindIntensity, Alpha);
	Out.FogAmount = FMath::Lerp(A.FogAmount, B.FogAmount, Alpha);
	return Out;
}

double FMantleWeatherLook::FogSunScale(double FogAmount, double SunElevationDegrees)
{
	const double Fog10 = (FogAmount > 0.0 ? FMath::Min(FogAmount, 1.0) : 0.0) * 10.0;
	const double SunZ = FMath::Abs(FMath::Sin(FMath::DegreesToRadians(SunElevationDegrees)));
	const double Least = 0.5 * FMath::Lerp(0.25, 0.35, WeatherLookRamp(SunZ, 0.1, 0.7));
	return FMath::Lerp(1.0, Least, WeatherLookRamp(Fog10, 5.0, 10.0));
}

FMantleWeatherLook FMantleWeatherLook::FromValues(const FMantleWeatherValues& Values, double SunElevationDegrees)
{
	using namespace MantleEnvironmentDefaults;

	// NaN clamps to the low end too: FMath::Clamp compares, and NaN fails both comparisons.
	const double Cover = (Values.CloudCover > 0.0) ? FMath::Min(Values.CloudCover, 1.0) : 0.0;
	const double Heavy = WeatherLookRamp(Cover, HeavyCloudCoverStart, 1.0);

	FMantleWeatherLook Look;
	Look.CloudCoverage = static_cast<float>(FMath::Lerp(CloudCoverageAtNoCover, CloudCoverageAtFullCover, Cover));
	Look.CloudDensity = static_cast<float>(FMath::Lerp(CloudDensityBelowHeavyCover, CloudDensityAtFullCover, Heavy));
	Look.StormClouds = static_cast<float>((Values.RainIntensity > 0.0) ? FMath::Min(Values.RainIntensity, 1.0) : 0.0);
	Look.FogDensityScale = static_cast<float>((Values.FogDensityScale > 0.0) ? FMath::Min(Values.FogDensityScale, MaxFogDensityScale) : 0.0);
	Look.SunIntensityScale = static_cast<float>(WeatherLookTable(SunDimCover, SunDimScale, Cover) * FogSunScale(Values.FogAmount, SunElevationDegrees));
	Look.SkyLightIntensityScale = static_cast<float>(FMath::Lerp(1.0, SkyLightIntensityAtFullCover, Heavy));
	Look.StarVisibility = static_cast<float>(1.0 - WeatherLookRamp(Cover, StarsFullyShownBelowCover, 1.0));
	return Look;
}

float FMantleWeatherLook::CloudLightningScale(double Flash)
{
	return static_cast<float>((Flash > 0.0) ? FMath::Min(Flash, 1.0) : 0.0);
}

FMantleWeatherSequence::FMantleWeatherSequence(int32 InSeed)
	: Seed(InSeed)
{
}

int32 FMantleWeatherSequence::SharePercent(EMantleWeatherState State)
{
	switch (State)
	{
	case EMantleWeatherState::Clear: return 32;
	case EMantleWeatherState::PartlyCloudy: return 25;
	case EMantleWeatherState::Cloudy: return 15;
	case EMantleWeatherState::Overcast: return 10;
	case EMantleWeatherState::Foggy: return 5;
	case EMantleWeatherState::LightRain: return 6;
	case EMantleWeatherState::Rain: return 5;
	case EMantleWeatherState::Storm: return 2;
	default: return 0;
	}
}

const FMantleWeatherValues& FMantleWeatherSequence::ValuesFor(EMantleWeatherState State)
{
	// Cover, fog density scale, rain, lightning, snow, dust, wind, fog amount.
	// The first five: clouds low / medium / high / high / full; fog at the engine default, then
	// slightly up, then up; rain medium in Rain and heavy in a Storm; lightning only in a Storm. Their
	// wind is what the client derived from their rain and cover before it was a value (the package's
	// presets' wind against their rain and cover), and they have no fog amount of their own.
	// The rest are Ultra Dynamic Weather's presets of the same names (Cloud Coverage, Rain, Snow,
	// Dust, Wind Intensity and Fog over 10), their fog density the package's for those values.
	constexpr double MaxLightning = MantleEnvironmentDefaults::MaxLightningPerMinute;
	static const FMantleWeatherValues Table[NumWeatherStates] = {
		{ 0.15, 1.0, 0.0, 0.0, 0.0, 0.0, 0.2, 0.0 },
		{ 0.45, 1.0, 0.0, 0.0, 0.0, 0.0, 0.229167, 0.0 },
		{ 0.80, 1.3, 0.0, 0.0, 0.0, 0.0, 0.3, 0.0 },
		{ 0.85, 1.8, 0.5, 0.0, 0.0, 0.0, 0.3, 0.0 },
		{ 1.00, 2.0, 1.0, MaxLightning, 0.0, 0.0, 1.0, 0.0 },
		// Cloudy, Foggy, LightRain (the package's Rain_Light).
		{ 0.50, PackageFogDensityScale(1.0, 0.0, 5.0), 0.0, 0.0, 0.0, 0.0, 0.25, 0.10 },
		{ 0.00, PackageFogDensityScale(10.0, 0.0, 0.0), 0.0, 0.0, 0.0, 0.0, 0.10, 1.00 },
		{ 0.60, PackageFogDensityScale(1.5, 0.0, 6.0), 0.3, 0.0, 0.0, 0.0, 0.20, 0.15 },
		// LightSnow, Snow, Blizzard (Snow_Light, Snow, Snow_Blizzard).
		{ 0.60, PackageFogDensityScale(3.0, 0.0, 6.0), 0.0, 0.0, 0.3, 0.0, 0.10, 0.30 },
		{ 0.85, PackageFogDensityScale(5.0, 0.0, 8.5), 0.0, 0.0, 0.6, 0.0, 0.40, 0.50 },
		{ 1.00, PackageFogDensityScale(10.0, 0.0, 10.0), 0.0, 0.0, 1.0, 0.0, 1.00, 1.00 },
		// DustCalm, DustStorm (Sand_Dust_Calm, Sand_Dust_Storm).
		{ 0.00, PackageFogDensityScale(1.0, 10.0, 0.0), 0.0, 0.0, 0.0, 1.0, 0.10, 0.10 },
		{ 0.00, PackageFogDensityScale(1.0, 10.0, 0.0), 0.0, 0.0, 0.0, 1.0, 1.00, 0.10 },
	};
	const int32 Index = FMath::Clamp(static_cast<int32>(State), 0, NumWeatherStates - 1);
	return Table[Index];
}

uint64 FMantleWeatherSequence::Hash(int32 InSeed, int64 A, int64 B, uint32 Salt)
{
	uint64 X = SplitMix64(static_cast<uint64>(static_cast<uint32>(InSeed)) ^ (static_cast<uint64>(Salt) << 32));
	X = SplitMix64(X ^ static_cast<uint64>(A));
	return SplitMix64(X ^ static_cast<uint64>(B));
}

double FMantleWeatherSequence::HashUnit(int32 InSeed, int64 A, int64 B, uint32 Salt)
{
	return static_cast<double>(Hash(InSeed, A, B, Salt) >> 11) * (1.0 / 9007199254740992.0);
}

void FMantleWeatherSequence::AppendNextSlot()
{
	FMantleWeatherSlot Slot;
	Slot.Index = Slots.Num();
	Slot.StartSeconds = Slots.Num() > 0 ? Slots.Last().EndSeconds() : 0;

	const uint64 LengthSpan = static_cast<uint64>(
		MantleEnvironmentDefaults::WeatherSlotMaxSeconds - MantleEnvironmentDefaults::WeatherSlotMinSeconds + 1);
	Slot.LengthSeconds = MantleEnvironmentDefaults::WeatherSlotMinSeconds
		+ static_cast<int64>(Hash(Seed, Slot.Index, 0, SaltSlotLength) % LengthSpan);

	const bool bHasPrevious = Slots.Num() > 0;
	if (bHasPrevious && Slots.Last().State == EMantleWeatherState::Storm)
	{
		// A storm always clears through Overcast, Rain or LightRain, in their own proportion.
		const uint64 Overcast = static_cast<uint64>(SharePercent(EMantleWeatherState::Overcast));
		const uint64 Rain = static_cast<uint64>(SharePercent(EMantleWeatherState::Rain));
		const uint64 LightRain = static_cast<uint64>(SharePercent(EMantleWeatherState::LightRain));
		const uint64 Roll = Hash(Seed, Slot.Index, 0, SaltAfterStorm) % (Overcast + Rain + LightRain);
		Slot.State = Roll < Overcast ? EMantleWeatherState::Overcast
			: Roll < Overcast + Rain ? EMantleWeatherState::Rain
			: EMantleWeatherState::LightRain;
	}
	else
	{
		const uint64 Roll = Hash(Seed, Slot.Index, 0, SaltSlotState) % 10000;
		Slot.State = EMantleWeatherState::LightRain;
		for (int32 StateIndex = 0; StateIndex < MantleNaturalWeatherStateCount; ++StateIndex)
		{
			if (Roll < DrawCumulativePerTenThousand[StateIndex])
			{
				Slot.State = static_cast<EMantleWeatherState>(StateIndex);
				break;
			}
		}
	}

	// Slot 0 has nothing to blend from: the server starts in its first state outright.
	Slot.PreviousState = bHasPrevious ? Slots.Last().State : Slot.State;
	Slots.Add(Slot);
}

void FMantleWeatherSequence::ExtendPastSeconds(int64 EnvironmentSeconds)
{
	while (Slots.Num() == 0 || Slots.Last().EndSeconds() <= EnvironmentSeconds)
	{
		AppendNextSlot();
	}
}

void FMantleWeatherSequence::ExtendToIndex(int64 Index)
{
	while (Slots.Num() <= Index)
	{
		AppendNextSlot();
	}
}

const FMantleWeatherSlot& FMantleWeatherSequence::SlotAt(double WeatherSeconds)
{
	const double Seconds = ClampWeatherSeconds(WeatherSeconds);
	// Slot ends are whole seconds, so a slot ends after Seconds exactly when it ends after
	// floor(Seconds).
	ExtendPastSeconds(FMath::FloorToInt64(Seconds));

	// Almost every query is for the newest slot; the rest binary-search the history.
	if (static_cast<double>(Slots.Last().StartSeconds) <= Seconds)
	{
		return Slots.Last();
	}
	int32 Low = 0;
	int32 High = Slots.Num() - 1;
	while (Low < High)
	{
		const int32 Mid = Low + (High - Low + 1) / 2;
		if (static_cast<double>(Slots[Mid].StartSeconds) <= Seconds)
		{
			Low = Mid;
		}
		else
		{
			High = Mid - 1;
		}
	}
	return Slots[Low];
}

const FMantleWeatherSlot& FMantleWeatherSequence::SlotByIndex(int64 Index)
{
	const int64 Clamped = FMath::Max<int64>(Index, 0);
	ExtendToIndex(Clamped);
	return Slots[static_cast<int32>(Clamped)];
}

FMantleWeatherSample FMantleWeatherSequence::Sample(double WeatherSeconds)
{
	const double Seconds = ClampWeatherSeconds(WeatherSeconds);
	const FMantleWeatherSlot Slot = SlotAt(Seconds);

	FMantleWeatherSample Out;
	Out.SlotIndex = Slot.Index;
	Out.State = Slot.State;
	Out.PreviousState = Slot.PreviousState;
	Out.Blend = FMath::SmoothStep(0.0, MantleEnvironmentDefaults::WeatherBlendSeconds, Seconds - static_cast<double>(Slot.StartSeconds));
	Out.Values = FMantleWeatherValues::Lerp(ValuesFor(Slot.PreviousState), ValuesFor(Slot.State), Out.Blend);
	return Out;
}

TArray<FMantleLightningStrike> FMantleWeatherSequence::StrikesInSlot(int64 Index)
{
	TArray<FMantleLightningStrike> Out;
	if (Index < 0)
	{
		return Out;
	}

	// A copy: extending the slot cache below could move the array.
	const FMantleWeatherSlot Slot = SlotByIndex(Index);
	if (Slot.State != EMantleWeatherState::Storm && Slot.PreviousState != EMantleWeatherState::Storm)
	{
		return Out;
	}

	const double FromRate = ValuesFor(Slot.PreviousState).LightningPerMinute;
	const double ToRate = ValuesFor(Slot.State).LightningPerMinute;
	const uint64 IntervalSpan = static_cast<uint64>(
		MantleEnvironmentDefaults::LightningIntervalMaxMs - MantleEnvironmentDefaults::LightningIntervalMinMs + 1);

	// Candidate times are exact integer milliseconds, so every machine agrees on them; only a
	// candidate inside the opening blend is thinned by a floating-point rate.
	const int64 EndMs = Slot.EndSeconds() * 1000;
	int64 Ms = Slot.StartSeconds * 1000;
	for (int32 Strike = 0; ; ++Strike)
	{
		Ms += MantleEnvironmentDefaults::LightningIntervalMinMs
			+ static_cast<int64>(Hash(Seed, Index, Strike, SaltStrikeInterval) % IntervalSpan);
		if (Ms >= EndMs)
		{
			break;
		}

		const double Seconds = static_cast<double>(Ms) / 1000.0;
		const double Blend = FMath::SmoothStep(0.0, MantleEnvironmentDefaults::WeatherBlendSeconds,
			Seconds - static_cast<double>(Slot.StartSeconds));
		const double Rate = FMath::Lerp(FromRate, ToRate, Blend);
		if (HashUnit(Seed, Index, Strike, SaltStrikeAccept) * MantleEnvironmentDefaults::MaxLightningPerMinute < Rate)
		{
			FMantleLightningStrike& Hit = Out.AddDefaulted_GetRef();
			Hit.SlotIndex = Index;
			Hit.StrikeIndex = Strike;
			Hit.Seconds = Seconds;
			Hit.Brightness = 0.6 + 0.4 * HashUnit(Seed, Index, Strike, SaltStrikeBrightness);
			Hit.CloudFrame = HashUnit(Seed, Index, Strike, SaltStrikeCloudFrame);
		}
	}
	return Out;
}

void FMantleWeatherSequence::CollectStrikes(double FromSeconds, double ToSeconds, TArray<FMantleLightningStrike>& Out)
{
	const double From = ClampWeatherSeconds(FromSeconds);
	const double To = ClampWeatherSeconds(ToSeconds);
	if (!(To > From))
	{
		return;
	}

	const int64 FirstSlot = SlotAt(From).Index;
	const int64 LastSlot = SlotAt(To).Index;
	for (int64 Index = FirstSlot; Index <= LastSlot; ++Index)
	{
		for (const FMantleLightningStrike& Strike : StrikesInSlot(Index))
		{
			if (Strike.Seconds > From && Strike.Seconds <= To)
			{
				Out.Add(Strike);
			}
		}
	}
}

void FMantleWeatherSequence::CollectScriptStrikes(int32 InSeed, int32 Revision, double StartSeconds, double FromSeconds,
	double ToSeconds, TFunctionRef<double(double)> RateAt, TArray<FMantleLightningStrike>& Out)
{
	const double From = ClampWeatherSeconds(FromSeconds);
	const double To = ClampWeatherSeconds(ToSeconds);
	if (!(To > From))
	{
		return;
	}

	// Whole milliseconds from a whole-millisecond start, as a slot's candidates are.
	const int64 StartMs = FMath::FloorToInt64(ClampWeatherSeconds(StartSeconds) * 1000.0);
	const int64 ToMs = FMath::FloorToInt64(To * 1000.0);
	if (ToMs < StartMs)
	{
		return;
	}
	const int64 ChunkMs = MantleEnvironmentDefaults::ScriptLightningChunkSeconds * 1000;
	const int64 LastChunk = (ToMs - StartMs) / ChunkMs;
	int64 FirstChunk = FMath::Max<int64>((FMath::FloorToInt64(From * 1000.0) - StartMs) / ChunkMs, 0);
	// A query spans seconds, never years (UpdateLightning catches up at most a few seconds).
	FirstChunk = FMath::Max<int64>(FirstChunk, LastChunk - 4);

	const uint64 IntervalSpan = static_cast<uint64>(
		MantleEnvironmentDefaults::LightningIntervalMaxMs - MantleEnvironmentDefaults::LightningIntervalMinMs + 1);
	for (int64 Chunk = FirstChunk; Chunk <= LastChunk; ++Chunk)
	{
		// Negative keys: a slot index is never below zero, so no chunk shares a slot's strikes.
		// Chunks stay under 2^24 (ClampWeatherSeconds caps time at 1e9 s, 1.7M chunks).
		const int64 Key = -1 - (static_cast<int64>(static_cast<uint32>(Revision)) << 24) - Chunk;
		const int64 ChunkStartMs = StartMs + Chunk * ChunkMs;
		const int64 ChunkEndMs = ChunkStartMs + ChunkMs;
		int64 Ms = ChunkStartMs;
		for (int32 Strike = 0; ; ++Strike)
		{
			Ms += MantleEnvironmentDefaults::LightningIntervalMinMs
				+ static_cast<int64>(Hash(InSeed, Key, Strike, SaltStrikeInterval) % IntervalSpan);
			if (Ms >= ChunkEndMs)
			{
				break;
			}
			const double Seconds = static_cast<double>(Ms) / 1000.0;
			if (Seconds <= From)
			{
				continue;
			}
			if (Seconds > To)
			{
				break;
			}
			if (HashUnit(InSeed, Key, Strike, SaltStrikeAccept) * MantleEnvironmentDefaults::MaxLightningPerMinute < RateAt(Seconds))
			{
				FMantleLightningStrike& Hit = Out.AddDefaulted_GetRef();
				Hit.SlotIndex = Key;
				Hit.StrikeIndex = Strike;
				Hit.Seconds = Seconds;
				Hit.Brightness = 0.6 + 0.4 * HashUnit(InSeed, Key, Strike, SaltStrikeBrightness);
				Hit.CloudFrame = HashUnit(InSeed, Key, Strike, SaltStrikeCloudFrame);
			}
		}
	}
}

#if !UE_BUILD_SHIPPING
bool FMantleWeatherSequence::ParseTestSeed(const TCHAR* CommandLine, int32& OutSeed)
{
	int32 Parsed = 0;
	if (CommandLine && FParse::Value(CommandLine, TEXT("MantleEnvWeatherSeed="), Parsed) && Parsed != 0)
	{
		OutSeed = Parsed;
		return true;
	}
	return false;
}
#endif
