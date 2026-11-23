// Copyright Joshua Eger. All Rights Reserved.
#include "MantleStreamBudget.h"

#include "MantlePlaceAssets.h"
#include "MantleRuntime.h"

#include "DynamicRHI.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMemory.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "RHI.h"
#include "RHIGlobals.h"
#include "RHIStats.h"
#include "Scalability.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <dxgi1_4.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace MantleStreamBudgetPrivate
{
#if PLATFORM_WINDOWS
	/** The card the game draws on, found once: the DXGI adapter with the RHI's vendor and device id (the largest, if there are two alike), or null when there is none. */
	IDXGIAdapter3* FindDxgiAdapter()
	{
		// dxgi.dll is loaded in every D3D process; looked up by name, so this module links no DXGI library of its own.
		HMODULE Dxgi = ::GetModuleHandleW(L"dxgi.dll");
		using FCreateFactory = HRESULT(WINAPI*)(REFIID, void**);
		const FCreateFactory CreateFactory = Dxgi ? reinterpret_cast<FCreateFactory>(reinterpret_cast<void*>(::GetProcAddress(Dxgi, "CreateDXGIFactory1"))) : nullptr;
		IDXGIFactory1* Factory = nullptr;
		if (!CreateFactory || FAILED(CreateFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&Factory))) || !Factory)
		{
			return nullptr;
		}
		IDXGIAdapter3* Best = nullptr;
		SIZE_T BestDedicated = 0;
		IDXGIAdapter1* Adapter = nullptr;
		for (UINT Index = 0; Factory->EnumAdapters1(Index, &Adapter) != DXGI_ERROR_NOT_FOUND; ++Index)
		{
			DXGI_ADAPTER_DESC1 Desc;
			IDXGIAdapter3* Adapter3 = nullptr;
			if (SUCCEEDED(Adapter->GetDesc1(&Desc)) && Desc.VendorId == GRHIVendorId && Desc.DeviceId == GRHIDeviceId && (!Best || Desc.DedicatedVideoMemory > BestDedicated)
				&& SUCCEEDED(Adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&Adapter3))))
			{
				if (Best)
				{
					Best->Release();
				}
				Best = Adapter3;
				BestDedicated = Desc.DedicatedVideoMemory;
			}
			Adapter->Release();
			Adapter = nullptr;
		}
		Factory->Release();
		UE_LOG(LogMantle, Log, TEXT("MANTLE_CELLS_BUDGET card watch: %s (%.0f MB dedicated) read through DXGI"), Best ? *GRHIAdapterName : TEXT("no matching adapter"),
			BestDedicated / (1024.0 * 1024.0));
		return Best;
	}

	/** What this process uses of the card and the budget Windows gives it (QueryVideoMemoryInfo, local segment, node 0); false off D3D or when DXGI cannot say. */
	bool QueryDxgiVideoMemory(int64& OutUsedBytes, int64& OutBudgetBytes)
	{
		const ERHIInterfaceType Type = RHIGetInterfaceType();
		if (Type != ERHIInterfaceType::D3D12 && Type != ERHIInterfaceType::D3D11)
		{
			return false;
		}
		// Kept for the process's life (one reference, never released: the RHI outlives every caller here).
		static IDXGIAdapter3* const Adapter = FindDxgiAdapter();
		DXGI_QUERY_VIDEO_MEMORY_INFO Info;
		if (!Adapter || FAILED(Adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &Info)) || Info.Budget == 0)
		{
			return false;
		}
		OutUsedBytes = static_cast<int64>(Info.CurrentUsage);
		OutBudgetBytes = static_cast<int64>(Info.Budget);
		return true;
	}
#else
	bool QueryDxgiVideoMemory(int64& OutUsedBytes, int64& OutBudgetBytes)
	{
		return false;
	}
#endif

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
	// The instance and solid counts stand in for memory (their basis is RAM and video bytes each, FMantleBudgetLimits): once the memory they stand for is read, it decides.
	// City Sample on Joshua's PC: 122,554 solid things against 117,554 took the ladder to 0.20 and cut buildings while the process used 0.60 of its memory limit.
	if (!IsMemoryRead(Limits, Use) || !IsCardRead(Use))
	{
		TakeWorst(Worst, Limits.Instances, Use.Instances);
	}
	if (!IsMemoryRead(Limits, Use))
	{
		TakeWorst(Worst, Limits.SolidThings, Use.SolidThings);
		TakeWorst(Worst, Limits.FarInstances, Use.FarInstances);
	}
	// The Nanite pages' per-setting limit stands in for the card too (32 KB each); once the card is read they press only against the asset set's own ceiling, past which a mesh
	// would draw plain. The test join on High (2026-11-22): 24,134 pages against 24,075 took the ladder to 0 with the card at 0.53.
	const int64 NaniteLimit = IsCardRead(Use) && IsLimited(Limits.NaniteRootPages) ? FMath::Max(Limits.NaniteRootPages, FMantleNaniteRootPages::BudgetPages) : Limits.NaniteRootPages;
	TakeWorst(Worst, NaniteLimit, Use.NaniteRootPages);
	// The textures' bytes act like a count: a build erased takes its share out at once, so it can lower the ladder at every look.
	TakeWorst(Worst, Limits.TextureBytes, Use.TextureBytes);
	return Worst;
}

bool FMantleStreamBudget::IsMemoryRead(const FMantleBudgetLimits& Limits, const FMantleBudgetUse& Use)
{
	using namespace MantleStreamBudgetPrivate;
	return (Use.ProcessMemoryBytes > 0 && IsLimited(Limits.ProcessMemoryBytes)) || (Use.ProcessMemoryShare > 0.0 && Limits.ProcessMemoryShare > 0.0);
}

bool FMantleStreamBudget::IsCardRead(const FMantleBudgetUse& Use)
{
	return Use.VideoUsedBytes > 0 && Use.VideoBudgetBytes > 0;
}

double FMantleStreamBudget::ProjectedMemoryPressure(const FMantleBudgetLimits& Limits, const FMantleBudgetUse& Use, const FMantleBudgetUse& Projected)
{
	if (!IsMemoryRead(Limits, Use))
	{
		return 0.0;
	}
	// The place's share of the memory is not known apart from the rest (the engine, avatars, the allocator's pages), so the whole is grown by the counts' largest growth:
	// high, so the ladder rises less readily (the side that does not cycle).
	double Growth = 1.0;
	const auto Grow = [&Growth](const int64 Now, const int64 Then)
	{
		if (Now > 0 && Then > Now)
		{
			Growth = FMath::Max(Growth, static_cast<double>(Then) / static_cast<double>(Now));
		}
	};
	Grow(Use.Instances, Projected.Instances);
	Grow(Use.SolidThings, Projected.SolidThings);
	Grow(Use.FarInstances, Projected.FarInstances);
	return MemoryPressure(Limits, Use) * Growth;
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

double FMantleStreamBudget::VideoPressure(const FMantleBudgetLimits& Limits, const FMantleBudgetUse& Use)
{
	const double Cushioned = static_cast<double>(Use.VideoBudgetBytes) * Limits.VideoUseShare;
	return Use.VideoUsedBytes > 0 && Cushioned > 0.0 ? static_cast<double>(Use.VideoUsedBytes) / Cushioned : 0.0;
}

double FMantleStreamBudget::Pressure(const FMantleBudgetLimits& Limits, const FMantleBudgetUse& Use)
{
	return FMath::Max3(CountPressure(Limits, Use), MemoryPressure(Limits, Use), VideoPressure(Limits, Use));
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

double FMantleStreamBudget::NextTextureDetailReach(const double ReachCm, const double Ladder, const double TexturePressure, const double VideoPressure, const double TexturesOfVideo,
	const double Pressure, const FMantleBudgetTuning& Tuning)
{
	const double MinCm = FMath::Min(Tuning.TextureDetailMinCm, Tuning.TextureDetailMaxCm);
	const double Step = FMath::Clamp(Tuning.TextureDetailStep, 0.1, 0.99);
	if (Ladder <= 0.0 && (TexturePressure > Tuning.LowerAbove || VideoPressure > Tuning.LowerAbove))
	{
		return FMath::Clamp(ReachCm * Step, MinCm, Tuning.TextureDetailMaxCm);
	}
	// Rising brings full files back: it rises only if the textures would still fit with the full-detail circle's area grown by the step (1 / Step^2), counting every texture
	// byte as if it grew so (the low copies do not: the side that does not cycle), and the card too with those bytes added. Else a reach that just fits would grow, pass the
	// limit, shrink, and round again.
	const double Grown = 1.0 / (Step * Step);
	if (Pressure < Tuning.RaiseBelow && TexturePressure * Grown <= Tuning.LowerAbove && VideoPressure + TexturesOfVideo * (Grown - 1.0) <= Tuning.LowerAbove)
	{
		return FMath::Clamp(ReachCm / Step, MinCm, Tuning.TextureDetailMaxCm);
	}
	return FMath::Clamp(ReachCm, MinCm, Tuning.TextureDetailMaxCm);
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

int64 FMantleStreamBudget::TexturePoolBytes(const FMantleBudgetLimits& Limits, const int64 TextureBytesHeld)
{
	constexpr int64 Step = 64ll * 1024 * 1024;
	if (Limits.TextureBytes <= 0 || Limits.TextureBytes == MAX_int64)
	{
		return 0;
	}
	const int64 Left = Limits.TextureBytes - FMath::Max<int64>(TextureBytesHeld, 0);
	return FMath::Max(MinTexturePoolBytes, Left / Step * Step);
}

int64 FMantleStreamBudget::NextTexturePool(const int64 Current, const int64 Wanted, const int64 OverBytes)
{
	constexpr int64 Step = 64ll * 1024 * 1024;
	if (Wanted <= 0)
	{
		return 0;
	}
	int64 Next = Current > 0 ? Current : Wanted;
	if (OverBytes > 0)
	{
		Next = Next - OverBytes;
	}
	else if (OverBytes <= -TexturePoolGrowBytes)
	{
		Next = Next + TexturePoolGrowBytes;
	}
	Next = FMath::Min(Next, Wanted);
	return FMath::Max(MinTexturePoolBytes, Next / Step * Step);
}

void FMantleStreamBudget::ApplyTexturePool(const int64 Bytes)
{
	check(IsInGameThread());
	static int32 LastSetMB = -1;
	const int32 MB = static_cast<int32>(FMath::Clamp<int64>(Bytes / (1024 * 1024), 0, MAX_int32));
	if (MB <= 0 || MB == LastSetMB)
	{
		return;
	}
	// The streaming manager reads it at every update (FRenderAssetStreamingManager::CheckUserSettings) and logs "Texture pool size now". Set by code, it stays over the graphics
	// setting's own value (ECVF_SetByScalability) when the player changes the setting.
	if (IConsoleVariable* PoolSize = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Streaming.PoolSize")))
	{
		PoolSize->Set(MB, ECVF_SetByCode);
		LastSetMB = MB;
	}
}

void FMantleStreamBudget::CurrentVideoMemory(int64& OutUsedBytes, int64& OutBudgetBytes)
{
	OutUsedBytes = 0;
	OutBudgetBytes = 0;
	if (!GDynamicRHI || !FApp::CanEverRender())
	{
		return;
	}
	// D3D12 and D3D11 fill the copy RHIGetMemoryStats hands out only when stats are compiled in (FD3D12DynamicRHI::UpdateMemoryStats and FD3D11DynamicRHI's, under
	// STATS || CSV_PROFILER_STATS), and neither is in a Shipping build: Joshua's Steam client read "0 of 0 MB" all session, so the pool never gave way. DXGI is asked
	// directly instead, the same query the engine makes (UE::DXGIUtilities::GetD3DMemoryStats): what this process uses of the card and the budget Windows gives it.
	if (!MantleStreamBudgetPrivate::QueryDxgiVideoMemory(OutUsedBytes, OutBudgetBytes))
	{
		// Vulkan keeps its numbers in every build (FVulkanDynamicRHI::RHIGetMemoryStats).
		FRHIMemoryStats Stats;
		RHIGetMemoryStats(Stats);
		if (Stats.BudgetLocal == 0)
		{
			return;
		}
		OutUsedBytes = static_cast<int64>(Stats.UsedLocal);
		OutBudgetBytes = static_cast<int64>(Stats.BudgetLocal);
	}
	// -MantleBudgetVideoGB (CurrentMachine): a smaller card than this one, so its budget is no more than that.
	const int64 Pretend = CurrentMachine().VideoBytes;
	double PretendGB = 0.0;
	if (Pretend > 0 && FParse::Value(FCommandLine::Get(), TEXT("MantleBudgetVideoGB="), PretendGB) && PretendGB > 0.0)
	{
		OutBudgetBytes = FMath::Min(OutBudgetBytes, Pretend);
	}
}

double FMantleStreamBudget::CurrentProcessMemoryShare()
{
	const int64 Physical = CurrentMachine().PhysicalBytes;
	return Physical > 0 ? static_cast<double>(CurrentProcessMemoryBytes()) / static_cast<double>(Physical) : 0.0;
}

int64 FMantleStreamBudget::ProcessMemoryBytes(const int64 PrivateBytes, const int64 CardUsedBytes)
{
	return FMath::Max<int64>(0, PrivateBytes - FMath::Max<int64>(0, CardUsedBytes));
}

int64 FMantleStreamBudget::CurrentProcessMemoryBytes()
{
	// Private bytes, not the working set: Windows trims the working set while what the process has committed keeps growing (City Sample's join crashed at a 36.4 GB
	// working set with 46.7 GB private). Less the card's use, which private also holds. The card's use is what the card budget reads (DXGI, else Vulkan's numbers);
	// when neither can say it is 0 and the whole private counts.
	int64 CardUsedBytes = 0;
	int64 CardBudgetBytes = 0;
	CurrentVideoMemory(CardUsedBytes, CardBudgetBytes);
	return ProcessMemoryBytes(static_cast<int64>(FPlatformMemory::GetStats().UsedVirtual), CardUsedBytes);
}
