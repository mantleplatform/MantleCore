// Copyright Joshua Eger. All Rights Reserved.
// Owns the "Mantle Scripts" nomad tab's lifecycle (registration, the toolbar button, and
// the single live panel instance other pieces need to reach -- New Script and Check All's
// console commands reuse an already-open panel's rows/UI instead of duplicating its logic).
// Kept separate from MantleEditor.cpp: that file's module startup/shutdown only calls the two
// functions below.

#pragma once

#include "CoreMinimal.h"

class SMantleScriptsPanel;
class SDockTab;
class FSpawnTabArgs;
struct FToolMenuOwner;

class FMantleScriptsPanelController
{
public:
	static const FName TabName;

	/** Registers the nomad tab spawner (Window menu, via WorkspaceMenu's Tools category) and the
	 *  level editor toolbar button. Call once from FMantleEditorModule::StartupModule. */
	static void Startup();
	/** Call once from FMantleEditorModule::ShutdownModule. */
	static void Shutdown();

	/** Opens (creating if needed) the "Mantle Scripts" tab. Used by the toolbar button and
	 *  Mantle.ScriptsPanel. */
	static void OpenPanel();

	/** The live panel widget, if the tab is currently open; nullptr otherwise. Mantle.NewScript
	 *  and Mantle.CheckScripts prefer this over recomputing things directly so the console and
	 *  the panel never show different results for the same script. */
	static TSharedPtr<SMantleScriptsPanel> GetPanel();

private:
	static TSharedRef<SDockTab> SpawnTab(const FSpawnTabArgs& Args);
	static void RegisterToolbar();

	static TWeakPtr<SMantleScriptsPanel> PanelWidget;
	static FDelegateHandle ToolMenusStartupHandle;
};
