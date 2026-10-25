// Copyright Joshua Eger. All Rights Reserved.
#include "Graph/MantleGraphCompileCommandlet.h"

#if WITH_EDITOR

#include "Graph/MantleGraphCompiler.h"
#include "Graph/MantleGraphTypes.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"

DEFINE_LOG_CATEGORY_STATIC(LogMantleGraphCompile, Log, All);

UMantleGraphCompileCommandlet::UMantleGraphCompileCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
	HelpDescription = TEXT("Compiles a single visual-scripting graph (JSON) to Luau, headless, for test harnesses.");
	HelpUsage = TEXT("-run=MantleGraphCompile -Graph=<in.graph.json> -Out=<out.luau>");
}

int32 UMantleGraphCompileCommandlet::Main(const FString& Params)
{
	FString GraphPath, OutPath;
	if (!FParse::Value(*Params, TEXT("Graph="), GraphPath) || GraphPath.IsEmpty())
	{
		UE_LOG(LogMantleGraphCompile, Error, TEXT("missing -Graph=<path to .graph.json>"));
		return 1;
	}
	if (!FParse::Value(*Params, TEXT("Out="), OutPath) || OutPath.IsEmpty())
	{
		UE_LOG(LogMantleGraphCompile, Error, TEXT("missing -Out=<path to write .luau>"));
		return 1;
	}

	FMantleGraph Graph;
	FString ParseError;
	if (!FMantleGraph::ParseFromFile(GraphPath, Graph, ParseError))
	{
		UE_LOG(LogMantleGraphCompile, Error, TEXT("%s: %s"), *GraphPath, *ParseError);
		return 1;
	}

	const FMantleGraphCompileResult Result = FMantleGraphCompiler::Compile(Graph);
	if (!Result.bSuccess)
	{
		UE_LOG(LogMantleGraphCompile, Error, TEXT("%s: %s"), *GraphPath, *Result.Error);
		return 1;
	}

	if (!FFileHelper::SaveStringToFile(Result.Luau, *OutPath))
	{
		UE_LOG(LogMantleGraphCompile, Error, TEXT("could not write '%s'"), *OutPath);
		return 1;
	}

	UE_LOG(LogMantleGraphCompile, Display, TEXT("compiled %s -> %s (%d line(s))"), *GraphPath, *OutPath, Result.LineMap.Num());
	return 0;
}

#endif // WITH_EDITOR
