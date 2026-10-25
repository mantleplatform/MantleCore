// Copyright Joshua Eger. All Rights Reserved.
// Compiles a parsed FMantleGraph to Luau against the script API manifest via
// FMantleGraphCatalogue. Covers the Blueprint-basics wave: branches, loops, variables, custom
// events/functions, on top of the event/call graph.
#pragma once

#include "CoreMinimal.h"
#include "Graph/MantleGraphTypes.h"

/** One compiled Luau line's origin, for runtime-error-on-node reporting. Line is
 *  1-based, matching how Luau itself reports line numbers in tracebacks. */
struct MANTLEEDITOR_API FMantleGraphLineMapEntry
{
	int32 Line = 0;
	FString NodeId;
};

struct MANTLEEDITOR_API FMantleGraphCompileResult
{
	bool bSuccess = false;
	/** Valid only when bSuccess. Deterministic: compiling the same graph twice gives byte-identical text. */
	FString Luau;
	/** Valid only when !bSuccess. Always names the offending node as `node '<id>': <reason>` when
	 *  the problem is tied to one node; a handful of whole-graph structural errors (a dangling
	 *  exec wire, a declared custom event with no matching node) have no single node and read
	 *  as plain text instead. */
	FString Error;
	/** Valid only when bSuccess. Excludes lines with no owning node (the header comment, blank
	 *  separators, variable/forward declarations). */
	TArray<FMantleGraphLineMapEntry> LineMap;
};

class MANTLEEDITOR_API FMantleGraphCompiler
{
public:
	static FMantleGraphCompileResult Compile(const FMantleGraph& Graph);

	/** Luau double-quoted string-literal escaping (backslash, quote, control characters).
	 *  Exposed directly so the escaping rules have their own focused test independent of a full
	 *  graph compile. */
	static FString EscapeLuauString(const FString& In);

	/** Deterministic Luau number literal: integral values print without a decimal point. */
	static FString FormatLuauNumber(double Value);
};
