// Copyright Joshua Eger. All Rights Reserved.
#include "Graph/MantleScriptGraphAsset.h"

#include "Graph/MantleScriptEdGraph.h"
#include "Graph/MantleScriptGraphSchema.h"

UMantleScriptGraph::UMantleScriptGraph()
{
}

void UMantleScriptGraph::EnsureEdGraph()
{
	if (EdGraph)
	{
		return;
	}
	EdGraph = NewObject<UMantleScriptEdGraph>(this, UMantleScriptEdGraph::StaticClass(), NAME_None, RF_Transactional);
	EdGraph->Schema = UMantleScriptGraphSchema::StaticClass();
}
