// Copyright Joshua Eger. All Rights Reserved.
// Registration entry points for everything under Graph/ -- the "Mantle" Content Browser
// asset category, UMantleScriptGraph's asset type actions, and (the compile commandlet aside)
// nothing else needs process-wide setup. Called from FMantleEditorModule::StartupModule/
// ShutdownModule.
#pragma once

#include "CoreMinimal.h"

class FMantleScriptGraphModule
{
public:
	static void Startup();
	static void Shutdown();
};
