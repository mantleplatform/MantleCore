// Copyright Joshua Eger. All Rights Reserved.
// Headless "compile one graph to Luau" entry point for the orchestrator's test harness
// (Mantle can't be tested in PIE, so fixtures are compiled outside the
// editor UI, exactly like any other headless UnrealEditor-Cmd.exe run). Editor-only, like the
// rest of MantleEditor; never staged into a player or server build.
//
// Usage: UnrealEditor-Cmd.exe client\Mantle.uproject -run=MantleGraphCompile
//            -Graph="C:\path\In.graph.json" -Out="C:\path\Out.luau"
// Exits 0 and writes Out on success; exits 1 and logs the node-naming compiler error (no file
// written) on failure. Always exits on its own -- a commandlet's Main() returning is the whole
// process's exit, so nothing here can hang a headless run.
#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "MantleGraphCompileCommandlet.generated.h"

#if WITH_EDITOR

UCLASS()
class MANTLEEDITOR_API UMantleGraphCompileCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UMantleGraphCompileCommandlet();

	virtual int32 Main(const FString& Params) override;
};

#endif // WITH_EDITOR
