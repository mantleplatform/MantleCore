// Copyright Joshua Eger. All Rights Reserved.
#include "Graph/MantleScriptGraphFactory.h"

#include "Graph/MantleScriptGraphAsset.h"
#include "Graph/MantleScriptGraphSerializer.h"
#include "MantleRuntime.h"

UMantleScriptGraphFactory::UMantleScriptGraphFactory()
{
	bCreateNew = true;
	bEditAfterNew = true;
	SupportedClass = UMantleScriptGraph::StaticClass();
}

UObject* UMantleScriptGraphFactory::FactoryCreateNew(UClass* InClass, UObject* InParent, FName InName, EObjectFlags Flags,
	UObject* Context, FFeedbackContext* Warn)
{
	UMantleScriptGraph* NewGraph = NewObject<UMantleScriptGraph>(InParent, InClass, InName, Flags | RF_Transactional);
	NewGraph->EnsureEdGraph();

	EMantleSide Side;
	FString Warning;
	if (InParent && !FMantleScriptGraphSerializer::ClassifySide(InParent->GetPathName(), Side, Warning))
	{
		// Non-blocking, matching MantlePlaceValidator.cpp's own script-folder check: the graph is
		// still created (a creator building in a fresh, empty folder shouldn't be locked out), but
		// it won't have a side to compile for until it's moved into Scripts/Server or
		// Scripts/Client. The same check runs again, loudly, the first time this graph compiles.
		UE_LOG(LogMantle, Warning, TEXT("%s: %s"), *InParent->GetPathName(), *Warning);
	}

	return NewGraph;
}
