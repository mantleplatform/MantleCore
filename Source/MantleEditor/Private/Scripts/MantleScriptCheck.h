// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleScriptRow.h"
#include "MantleScriptError.h"

/**
 * Editor-side "check this source without running it" -- the Scripts panel's Check All and the
 * Mantle.CheckScripts console command. Never touches a game world, never starts a script, never
 * calls a creator's OnInit.
 */
class FMantleScriptCheck
{
public:
	/**
	 * Compiles/parses Source as Language; true and an empty OutError on success. Luau and
	 * JavaScript go through their runtime's ValidateSource (bytecode-compile / parse-only). TypeScript transpiles with the editor's
	 * bundled compiler -- the compiler itself runs, but it only ever
	 * evaluates the creator's source as a string argument to `ts.transpileModule`, never as
	 * JavaScript, so creator code still never executes.
	 *
	 * Python uses the runtime PocketPy compile-only helper; creator code never executes.
	 */
	static bool Check(EMantleScriptEditorLanguage Language, const FString& Source, FMantleScriptError& OutError);

	/** True for an unknown language Check() cannot check -- Check All shows these as skipped, not as errors. */
	static bool IsUnsupported(EMantleScriptEditorLanguage Language);
};
