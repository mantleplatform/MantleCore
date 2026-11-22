// Copyright Joshua Eger. All Rights Reserved.
#include "MantleStreamBudget.h"

#include "DynamicRHI.h"
#include "HAL/PlatformMemory.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "RHI.h"
#include "RHIGlobals.h"
#include "RHIStats.h"
#include "Scalability.h"

namespace MantleStreamBudgetPrivate
{
	/** One row of the table in BP\gates\S8-A.budgets.md: Epic is what S0-M2 and S0-M34 measured, the rest are quarter steps. */
	struct FLevelRow
	{
		int64 Instances;
		int64 SolidThings;
		int64 FarInstances;
		int64 NaniteRootPages;
		double ProcessMemoryShare;
	};

	constexpr FLevelRow Rows[FMantleStreamBudget::LevelCount] = {
		{ 275000, 30000, 50000, 8192, 0.55 },
		{ 550000, 60000, 100000, 16384, 0.60 },
		{ 825000, 90000, 200000, 24576, 0.65 },
		{ 1100000, 120000, 400000, 36864, 0.70 },
	};

	/** TextureShareOfVideo per preset level (#189; the client big-map plan, 4.3). */
	constexpr double TextureShares[FMantleStreamBudget::LevelCount] = { 0.25, 0.33, 0.42, 0.5 };

	constexpr double GiB = 1024.0 * 1024.0 * 1024.0;

	/** The machine the Epic row was measured on. */
	constexpr double MeasuredVideoBytes = 16.0 * 1024.0 * 1024.0 * 1024.0;
	constexpr double MeasuredPhysicalBytes = 32.0 * 1024.0 * 1024.0 * 1024.0;

	int64 Scaled(const int64 Count, const double Factor)
	{
		return FMath::Max<int64>(1, FMath::RoundToInt64(static_cast<double>(Count) * Factor));
	}

	bool IsLimited(const int64 Limit)
	{
		return Limit > 0 && Limit < MAX_int64;
	}

	void TakeWorst(double& Worst, const int64 Limit, const int64 Used)
	{
		if (IsLimited(Limit))
		{
			Worst = FMath::Max(Worst, static_cast<double>(Used) / static_cast<double>(Limit));
		}
	}
}

int32 FMantleStreamBudget::ClampLevel(const int32 Level)
{
	return FMath::Clamp(Level, 0, LevelCount - 1);
}

double FMantleStreamBudget::MachineFactor(const FMantleBudgetMachine& Machine)
{
	using namespace MantleStreamBudgetPrivate;
	const double Video = Machine.VideoBytes > 0 ? static_cast<double>(Machine.VideoBytes) / MeasuredVideoBytes : 1.0;
	const double Physical = Machine.PhysicalBytes > 0 ? static_cast<double>(Machine.PhysicalBytes) / MeasuredPhysicalBytes : 1.0;
	return FMath::Clamp(FMath::Min(Video, Physical), 0.5, 1.0);
}

FMantleBudgetLimits FMantleStreamBudget::LimitsFor(const int32 Level, const double Factor)
{
	using namespace MantleStreamBudgetPrivate;
	const FLevelRow& Row = Rows[ClampLevel(Level)];
	FMantleBudgetLimits Limits;
	Limits.Instances = Scaled(Row.Instances, Factor);
	Limits.SolidThings = Scaled(Row.SolidThings, Factor);
	Limits.FarInstances = Scaled(Row.FarInstances, Factor);
	Limits.NaniteRootPages = Scaled(Row.NaniteRootPages, Factor);
	Limits.ProcessMemoryShare = Row.ProcessMemoryShare;
	return Limits;
}

FMantleBudgetLimits FMantleStreamBudget::LimitsFor(const int32 Level, const FMantleBudgetMachine& Machine, const double Scale)
{
	FMantleBudgetLimits Limits = LimitsFor(Level, MachineFactor(Machine) * Scale);
	if (Machine.VideoBytes > 0)
	{
		Limits.TextureBytes = FMath::Max<int64>(1, static_cast<int64>(static_cast<double>(Machine.VideoBytes) * TextureShareOfVideo(Level) * Scale));
	}
	Limits.ProcessMemoryBytes = ProcessMemoryCapBytes(Machine.PhysicalBytes);
	return Limits;
}

double FMantleStreamBudget::TextureShareOfVideo(const int32 Level)
{
	return MantleStreamBudgetPrivate::TextureShares[ClampLevel(Level)];
}

int64 FMantleStreamBudget::ProcessMemoryCapBytes(const int64 PhysicalBytes)
{
	using namespace MantleStreamBudgetPrivate;
	if (PhysicalBytes <= 0)
	{
		return MAX_int64;
	}
	// The table in the client plan's 4.3 (Joshua's minimum PC is the first point), in GB of RAM -> GB the game may use.
	const double Ram = static_cast<double>(PhysicalBytes) / GiB;
	double Cap = 0.0;
	if (Ram <= 16.0)
	{
		Cap = Ram * 9.0 / 16.0;
	}
	else if (Ram <= 32.0)
	{
		Cap = 9.0 + (Ram - 16.0) * (17.0 - 9.0) / 16.0;
	}
	else
	{
		Cap = 17.0 + (Ram - 32.0) * (24.0 - 17.0) / 32.0;
	}
	return static_cast<int64>(Cap * GiB);
}

double FMantleStreamBudget::CountPressure(const FMantleBudgetLimits& Limits, const FMantleBudgetUse& Use)
{
	using namespace MantleStreamBudgetPrivate;
	double Worst = 0.0;
	TakeWorst(Worst, Limits.Instances, Use.Instances);
	TakeWorst(Worst, Limits.SolidThings, Use.SolidThings);
	TakeWorst(Worst, Limits.FarInstances, Use.FarInstances);
	TakeWorst(Worst, Limits.NaniteRootPages, Use.NaniteRootPages);
	// The textures' bytes act like a count: a build erased takes its share out at once, so it can lower the ladder at every look.
	TakeWorst(Worst, Limits.TextureBytes, Use.TextureBytes);
	return Worst;
}

double FMantleStreamBudget::MemoryPressure(const FMantleBudgetLimits& Limits, const FMantleBudgetUse& Use)
{
	using namespace MantleStreamBudgetPrivate;
	double Worst = Limits.ProcessMemoryShare > 0.0 && Use.ProcessMemoryShare > 0.0 ? Use.ProcessMemoryShare / Limits.ProcessMemoryShare : 0.0;
	if (Use.ProcessMemoryBytes > 0)
	{
		TakeWorst(Worst, Limits.ProcessMemoryBytes, Use.ProcessMemoryBytes);
	}
	return Worst;
}

double FMantleStreamBudget::Pressure(const FMantleBudgetLimits& Limits, const FMantleBudgetUse& Use)
{
	return FMath::Max(CountPressure(Limits, Use), MemoryPressure(Limits, Use));
}

double FMantleStreamBudget::NextLadder(const double Ladder, const double Pressure, const FMantleBudgetTuning& Tuning)
{
	if (Pressure > Tuning.LowerAbove)
	{
		return FMath::Max(Ladder - Tuning.LowerPerStep, 0.0);
	}
	if (Pressure < Tuning.RaiseBelow)
	{
		return FMath::Min(Ladder + Tuning.RaisePerStep, 1.0);
	}
	return FMath::Clamp(Ladder, 0.0, 1.0);
}

double FMantleStreamBudget::FarScale(const double Ladder)
{
	return FMath::Clamp((Ladder - 0.5) * 2.0, 0.0, 1.0);
}

double FMantleStreamBudget::NearScale(const double Ladder, const FMantleBudgetTuning& Tuning)
{
	if (Ladder >= 0.5)
	{
		return 1.0;
	}
	return FMath::Lerp(Tuning.MinNearScale, 1.0, FMath::Clamp(Ladder * 2.0, 0.0, 1.0));
}

double FMantleStreamBudget::ScaledReach(const double DrawDistanceCm, const double Scale, const double FloorCm)
{
	return FMath::Min(DrawDistanceCm, FMath::Max(DrawDistanceCm * Scale, FloorCm));
}

int32 FMantleStreamBudget::CurrentLevel()
{
	return ClampLevel(Scalability::GetQualityLevels().GetMinQualityLevel());
}

FMantleBudgetMachine FMantleStreamBudget::CurrentMachine()
{
	// The card is asked once a second at most (the asset set asks for the Nanite limit at every mesh it builds).
	static FMantleBudgetMachine Cached;
	static double CachedAt = -1000.0;
	const double Now = FPlatformTime::Seconds();
	if (Now - CachedAt < 1.0)
	{
		return Cached;
	}
	CachedAt = Now;
	FMantleBudgetMachine Machine;
	Machine.PhysicalBytes = static_cast<int64>(FPlatformMemory::GetConstants().TotalPhysical);
	if (GDynamicRHI && FApp::CanEverRender())
	{
		FTextureMemoryStats Stats;
		RHIGetTextureMemoryStats(Stats);
		Machine.VideoBytes = Stats.DedicatedVideoMemory > 0 ? Stats.DedicatedVideoMemory : Stats.TotalGraphicsMemory;
	}
	// A measuring aid, like -MantleBudgetScale: -MantleBudgetRamGB=16 -MantleBudgetVideoGB=8 runs the budget as Joshua's minimum PC would, on a bigger one.
	double PretendGB = 0.0;
	if (FParse::Value(FCommandLine::Get(), TEXT("MantleBudgetRamGB="), PretendGB) && PretendGB > 0.0)
	{
		Machine.PhysicalBytes = static_cast<int64>(PretendGB * MantleStreamBudgetPrivate::GiB);
	}
	PretendGB = 0.0;
	if (FParse::Value(FCommandLine::Get(), TEXT("MantleBudgetVideoGB="), PretendGB) && PretendGB > 0.0)
	{
		Machine.VideoBytes = static_cast<int64>(PretendGB * MantleStreamBudgetPrivate::GiB);
	}
	Cached = Machine;
	return Cached;
}

FMantleBudgetLimits FMantleStreamBudget::CurrentLimits()
{
	// -MantleBudgetScale=0.1 is a measuring aid: it makes every count limit that much of its size, so a run on a big machine can show a place going over budget. Absent, it is 1.
	static const double CommandScale = []()
	{
		double Scale = 1.0;
		FParse::Value(FCommandLine::Get(), TEXT("MantleBudgetScale="), Scale);
		return FMath::Clamp(Scale, 0.001, 1.0);
	}();
	return LimitsFor(CurrentLevel(), CurrentMachine(), CommandScale);
}

double FMantleStreamBudget::CurrentProcessMemoryShare()
{
	const FPlatformMemoryStats Stats = FPlatformMemory::GetStats();
	const int64 Physical = CurrentMachine().PhysicalBytes;
	return Physical > 0 ? static_cast<double>(Stats.UsedPhysical) / static_cast<double>(Physical) : 0.0;
}

int64 FMantleStreamBudget::CurrentProcessMemoryBytes()
{
	return static_cast<int64>(FPlatformMemory::GetStats().UsedPhysical);
}
