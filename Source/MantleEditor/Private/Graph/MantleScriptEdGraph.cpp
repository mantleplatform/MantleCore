// Copyright Joshua Eger. All Rights Reserved.
#include "Graph/MantleScriptEdGraph.h"

const FMantleEdGraphVariable* UMantleScriptEdGraph::FindVariable(const FString& Name) const
{
	return Variables.FindByPredicate([&Name](const FMantleEdGraphVariable& V) { return V.Name == Name; });
}

const FMantleEdGraphCustomEvent* UMantleScriptEdGraph::FindCustomEvent(const FString& Name) const
{
	return CustomEvents.FindByPredicate([&Name](const FMantleEdGraphCustomEvent& E) { return E.Name == Name; });
}

const FMantleEdGraphFunction* UMantleScriptEdGraph::FindFunction(const FString& Name) const
{
	return Functions.FindByPredicate([&Name](const FMantleEdGraphFunction& F) { return F.Name == Name; });
}
