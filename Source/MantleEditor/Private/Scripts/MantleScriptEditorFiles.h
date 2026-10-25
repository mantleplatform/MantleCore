// Copyright Joshua Eger. All Rights Reserved.
// The tooling files that make VS Code + luau-lsp/tsc understand a Scripts/ folder against
// Mantle's current API surface. The Scripts panel regenerates them.

#pragma once

#include "CoreMinimal.h"

class FMantleScriptEditorFiles
{
public:
	/**
	 * Refreshes <ProjectDir>/ScriptTypes/{mantle.d.luau, mantle.d.ts, mantle.pyi,
	 * mantle_api.json} from the plugin's copies (FMantleApiManifest::ResourceDirectory), and writes
	 * <ProjectDir>/tsconfig.json if it doesn't exist yet. Returns the ScriptTypes directory written to.
	 */
	static FString RegenerateTypes();

	/** Writes ScriptsRoot/.luaurc (strict mode, @Shared/@Server/@Client aliases) if it doesn't
	 *  already exist. A creator's own .luaurc, once they have one, is never overwritten. */
	static void EnsureLuauRc(const FString& ScriptsRoot);

	/** Writes <ProjectDir>/.vscode/settings.json (luau-lsp pointed at ScriptTypes/mantle.d.luau)
	 *  if it doesn't already exist. */
	static void EnsureVsCodeSettings();

	/** RegenerateTypes() + EnsureLuauRc(ScriptsRoot) + EnsureVsCodeSettings(), in that order.
	 *  Called whenever the Scripts panel opens or rescans, and by its "Regenerate Types" button
	 *  and Mantle.ScriptsPanel/Mantle.CheckScripts, so opening the folder in VS Code always has
	 *  something to work with. */
	static void EnsureAll(const FString& ScriptsRoot);
};
