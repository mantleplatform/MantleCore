// Copyright Joshua Eger. All Rights Reserved.
#include "MantleRuntime.h"

#include "MantleAssetStore.h"

DEFINE_LOG_CATEGORY(LogMantle);

void FMantleRuntimeModule::StartupModule()
{
	UE_LOG(LogMantle, Log, TEXT("MantleRuntime module started"));
}

void FMantleRuntimeModule::ShutdownModule()
{
	FMantleAssetStore::ShutdownDefault();
	UE_LOG(LogMantle, Log, TEXT("MantleRuntime module shut down"));
}
