// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleScriptError.h"

/** Editor-side JavaScript/TypeScript checks. The live QuickJS VM is in Private/JS/MantleQuickJS.cpp. */
class MANTLESCRIPTTOOLS_API FMantleQuickJSRuntime
{
public:
	/** Parses and bytecode-compiles one ES module without evaluating code or loading dependencies.
     * Static import/export syntax is checked; catalogue existence, path case, side and exported
     * names are checked by the real runtime module loader, not this single-file gate. */
	static bool ValidateSource(
		const FString& Source,
		FMantleScriptError& OutError,
		uint64 MaximumSourceBytes = 1024 * 1024);

#if WITH_EDITOR
	/**
	 * Transpiles one in-memory TypeScript module with the editor's vendored compiler bundle.
	 * The bundle is supplied by MantleEditor so neither file access nor the 9 MiB compiler
	 * resource becomes part of the shipped runtime API.
	 */
	static bool TranspileTypeScript(
		const FString& CompilerBundle,
		const FString& Source,
		FString& OutJavaScript,
		FMantleScriptError& OutError,
		uint64 MaximumSourceBytes = 1024 * 1024);
#endif
};
