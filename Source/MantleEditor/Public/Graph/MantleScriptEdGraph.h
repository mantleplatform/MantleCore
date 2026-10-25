// Copyright Joshua Eger. All Rights Reserved.
// The UEdGraph the graph editor edits with Unreal's own graph framework (UEdGraph/
// UEdGraphSchema/SGraphEditor -- the same stack the Material and Niagara editors use). One
// instance lives inside each UMantleScriptGraph asset (MantleScriptGraphAsset.h).
//
// Variables/custom events/functions are declared here rather than on the asset itself: they are
// exactly what the My Blueprint-style panel (MantleScriptGraphEditorToolkit.h) lists and edits,
// and what "getvar"/"setvar"/"callevent"/"onevent"/"callfunction"/"function" palette entries are
// generated from (MantleScriptGraphSchema.h) -- keeping them on the same object the schema
// already has a pointer to avoids threading the owning asset through every palette build.
#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraph.h"
#include "MantleScriptSide.h"
#include "MantleScriptEdGraph.generated.h"

/** One named parameter of a custom event or function declaration. Mirrors FMantleGraphParam
 *  (MantleGraphTypes.h) as an editable UPROPERTY row. */
USTRUCT()
struct FMantleEdGraphParam
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Mantle")
	FString Name;

	/** "bool" | "number" | "string" | "Vector3" | "Entity" | "Player" | "list" | "map". */
	UPROPERTY(EditAnywhere, Category = "Mantle")
	FString Type = TEXT("number");
};

/** A script-local variable (Blueprint's "My Blueprint > Variables"). Mirrors FMantleGraphVariable. */
USTRUCT()
struct FMantleEdGraphVariable
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Mantle")
	FString Name;

	UPROPERTY(EditAnywhere, Category = "Mantle")
	FString Type = TEXT("number");
};

/** A graph-local custom event: fired by a "callevent" node, handled by one "onevent" node naming it. */
USTRUCT()
struct FMantleEdGraphCustomEvent
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Mantle")
	FString Name;

	UPROPERTY(EditAnywhere, Category = "Mantle")
	TArray<FMantleEdGraphParam> Params;
};

/** A graph-local function: has its own exec root ("function" node) and must terminate every path
 *  with a "return" node. */
USTRUCT()
struct FMantleEdGraphFunction
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Mantle")
	FString Name;

	UPROPERTY(EditAnywhere, Category = "Mantle")
	TArray<FMantleEdGraphParam> Params;

	UPROPERTY(EditAnywhere, Category = "Mantle")
	TArray<FMantleEdGraphParam> Returns;
};

/**
 * The editable graph. Side is not stored here: it is recomputed from the owning asset's package
 * path every time it matters (compile, palette filtering), the same rule a plain script file
 * uses -- see FMantleScriptGraphSerializer::ClassifySide. That way moving the asset in the
 * Content Browser is the only thing that ever changes which side it compiles for.
 */
UCLASS()
class MANTLEEDITOR_API UMantleScriptEdGraph : public UEdGraph
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "Mantle")
	TArray<FMantleEdGraphVariable> Variables;

	UPROPERTY(EditAnywhere, Category = "Mantle")
	TArray<FMantleEdGraphCustomEvent> CustomEvents;

	UPROPERTY(EditAnywhere, Category = "Mantle")
	TArray<FMantleEdGraphFunction> Functions;

	const FMantleEdGraphVariable* FindVariable(const FString& Name) const;
	const FMantleEdGraphCustomEvent* FindCustomEvent(const FString& Name) const;
	const FMantleEdGraphFunction* FindFunction(const FString& Name) const;
};
