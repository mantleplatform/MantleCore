// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FMantleEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	static void RegisterMenus();
	/** #434: loads MantleAgentTools once the engine is up, in an editor with Toolset Registry enabled. */
	static void LoadAgentTools();
	FDelegateHandle AgentToolsHandle;
	/** GhzGarage/MantleCore#142: registers the Mantle Seat's viewport marker once the editor engine exists. */
	static void RegisterSeatVisualizer();
	FDelegateHandle SeatVisualizerHandle;
};
