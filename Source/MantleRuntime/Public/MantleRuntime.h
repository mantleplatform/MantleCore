// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

MANTLERUNTIME_API DECLARE_LOG_CATEGORY_EXTERN(LogMantle, Log, All);

class FMantleRuntimeModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
