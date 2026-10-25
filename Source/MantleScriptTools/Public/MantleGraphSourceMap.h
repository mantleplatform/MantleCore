// Copyright Joshua Eger. All Rights Reserved.
// S1.20: the runtime-side half of graph diagnostics.  This deliberately knows only the small,
// versioned comment format emitted by MantleGraphCompiler; MantleScript must not depend on
// MantleEditor just to turn a Luau traceback into a graph node.
#pragma once

#include "CoreMinimal.h"

struct MANTLESCRIPTTOOLS_API FMantleGraphSourceMapEntry
{
	int32 Line = 0;
	FString NodeId;
	FString NodeTitle;
};

/** Decodes an optional `-- MANTLE_GRAPH_MAP v1 <base64-json>` header from generated Luau.
 *  False means the source is not graph-authored or has a malformed/unknown map; it never makes
 *  an ordinary script fail to start. */
MANTLESCRIPTTOOLS_API bool MantleParseGraphSourceMap(
	const FString& Source,
	TArray<FMantleGraphSourceMapEntry>& OutEntries);

/** Finds the first graph-owned Luau line mentioned by an error or traceback.  Luau reports
 *  locations as `chunk:line`, so accepting all colon-number occurrences lets this work for both
 *  load failures and coroutine traces without binding to a particular Luau trace formatter. */
MANTLESCRIPTTOOLS_API const FMantleGraphSourceMapEntry* MantleFindGraphSourceMapEntry(
	TArrayView<const FMantleGraphSourceMapEntry> Entries,
	const FString& Message,
	const FString& Traceback);
