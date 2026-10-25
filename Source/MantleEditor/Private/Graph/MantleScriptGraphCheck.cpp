// Copyright Joshua Eger. All Rights Reserved.
#include "Graph/MantleScriptGraphCheck.h"

#include "Graph/MantleGraphCompiler.h"
#include "Graph/MantleGraphTypes.h"
#include "Graph/MantleScriptEdGraph.h"
#include "Graph/MantleScriptGraphAsset.h"
#include "Graph/MantleScriptGraphSerializer.h"

#include "Misc/PackageName.h"
#include "UObject/UObjectGlobals.h"

bool FMantleScriptGraphCheck::CheckAsset(const FString& PackageName, FString& OutMessage)
{
	UMantleScriptGraph* Graph = LoadObject<UMantleScriptGraph>(nullptr, *(PackageName + TEXT(".") + FPackageName::GetShortName(PackageName)));
	if (!Graph)
	{
		OutMessage = TEXT("couldn't load the graph asset");
		return false;
	}
	Graph->EnsureEdGraph();
	const UMantleScriptEdGraph* EdGraph = Cast<UMantleScriptEdGraph>(Graph->EdGraph);
	if (!EdGraph)
	{
		OutMessage = TEXT("graph asset has no editable graph");
		return false;
	}

	EMantleSide Side;
	FString FolderWarning;
	if (!FMantleScriptGraphSerializer::ClassifySide(PackageName, Side, FolderWarning))
	{
		OutMessage = FolderWarning;
		return false;
	}

	FMantleGraph ParsedGraph;
	FString BuildError;
	if (!FMantleScriptGraphSerializer::BuildGraph(*EdGraph, FPackageName::GetShortName(PackageName), Side, ParsedGraph, BuildError))
	{
		OutMessage = BuildError;
		return false;
	}

	const FMantleGraphCompileResult Result = FMantleGraphCompiler::Compile(ParsedGraph);
	if (!Result.bSuccess)
	{
		OutMessage = Result.Error;
		return false;
	}
	OutMessage = FString::Printf(TEXT("%d line(s)"), Result.LineMap.Num());
	return true;
}
