// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

/**
 * A CPU trace scope named "<StaticName> <Detail>", for example
 * "MantleAssetLoad /Engine/BasicShapes/Cube.Cube", so an Insights capture shows which asset a stall
 * belongs to. DetailExpr must be an FString expression; it is evaluated and formatted only while the CPU
 * channel is recording, so a run without tracing pays one branch. Compiles out wherever CPU tracing does
 * (Shipping, or UE_TRACE_ENABLED off). Pair it with a static TRACE_CPUPROFILER_EVENT_SCOPE, which Insights
 * aggregates across calls; dynamic names do not aggregate.
 */
#if CPUPROFILERTRACE_ENABLED
#define MANTLE_TRACE_SCOPE_DETAIL(StaticName, DetailExpr) \
	const bool UE_JOIN(bMantleTraceOn, __LINE__) = UE_TRACE_CHANNELEXPR_IS_ENABLED(CpuChannel); \
	TRACE_CPUPROFILER_EVENT_SCOPE_TEXT_CONDITIONAL( \
		UE_JOIN(bMantleTraceOn, __LINE__) ? *FString::Printf(TEXT("%s %s"), TEXT(StaticName), *(DetailExpr)) : TEXT(""), \
		UE_JOIN(bMantleTraceOn, __LINE__))
#else
#define MANTLE_TRACE_SCOPE_DETAIL(StaticName, DetailExpr)
#endif
