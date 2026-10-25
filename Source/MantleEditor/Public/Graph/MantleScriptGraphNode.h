// Copyright Joshua Eger. All Rights Reserved.
// One node in the graph editor. A single generic class represents every one of the ~25
// node kinds ("event", "call", "branch", "math", ...) rather than a subclass per kind --
// MantleGraphTypes.h's own comment on FMantleGraphNode explains why a second, C++-typed copy of
// the per-kind schema isn't worth it ("a second place to keep in sync"); the same reasoning
// applies here. NodeKind plus a handful of typed properties (Op/TextValue/VariableName/...) cover
// every kind's non-pin fields, and pins are allocated per kind in AllocateDefaultPins. See
// MantleScriptGraphSerializer.h for how a placed node round-trips to/from FMantleGraphNode's JSON.
#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraphNode.h"
#include "MantleScriptGraphNode.generated.h"

/** Pin category names this editor uses. Distinct from Unreal's own K2 pin categories (this isn't
 *  a Blueprint graph) but chosen to mean the same things, and coloured to match
 *  (MantleScriptGraphSchema::GetPinTypeColor). */
struct MANTLEEDITOR_API FMantleGraphPinCategory
{
	static const FName Exec;
	static const FName Boolean;
	static const FName Number;
	static const FName String;
	static const FName Vector3;
	/** An entity/player/handle/service reference. PinSubCategory carries the exact registry type
	 *  name ("Entity", "Player", ...) so a wire between two different object types is rejected the
	 *  same way the compiler would reject it, while "any"/unknown types stay permissive. */
	static const FName Object;
	/** "any", an array/map ("{T}"), or a type the registry doesn't resolve precisely enough to
	 *  check on the wire -- connects to anything, exactly like the compiler's own leniency
	 *  (MantleGraphTypesCompatible). */
	static const FName Wildcard;

	/** Maps a registry/graph type annotation ("Entity?", "{Entity}", "boolean", "Vector3", ...) to
	 *  one of the categories above, and (for Object) the exact sub-type name. */
	static FName CategoryForType(const FString& RegistryType, FName& OutSubCategory);
};

UCLASS()
class MANTLEEDITOR_API UMantleScriptGraphNode : public UEdGraphNode
{
	GENERATED_BODY()

public:
	/** "start" | "event" | "onevent" | "function" | "branch" | "sequence" | "forloop" | "foreach" |
	 *  "while" | "delay" | "doonce" | "call" | "get" | "set" | "callevent" | "callfunction" |
	 *  "return" | "print" | "warn" | "literal" | "getvar" | "setvar" | "math" | "compare" | "bool" |
	 *  "concat" | "format" | "makevector3" | "breakvector3" | "vectormath" | "positiongoal" | "comment" | "reroute".
	 *  Set once by whichever palette action spawned this node; never changes afterward. */
	UPROPERTY(VisibleAnywhere, Category = "Mantle")
	FString NodeKind;

	/** "ClassName.MemberName" for event/call/get/set nodes; empty otherwise. */
	UPROPERTY(VisibleAnywhere, Category = "Mantle")
	FString Dotted;

	/** literal's own value kind: "bool" | "number" | "string" | "vector". Set at creation by which
	 *  "Literal ..." palette action was used. */
	UPROPERTY(VisibleAnywhere, Category = "Mantle")
	FString LiteralType;

	/** Event-category catalogue nodes only, and only when the registry couldn't name the
	 *  handler's own parameters (a fully variadic signature, e.g. Remote.OnClientEvent's
	 *  "(...any) -> ()"): the output pin names to use instead, mirroring the JSON model's
	 *  own "outputs" fallback field (MantleGraphCompiler.cpp's EmitEventNode). Ignored -- and the
	 *  registry's own names used instead -- whenever the catalogue already knows them. */
	UPROPERTY(EditAnywhere, Category = "Mantle")
	TArray<FString> ExtraOutputNames;

	// Kind-specific configuration. Which of these apply is entirely determined by NodeKind; the
	// details panel (MantleScriptGraphEditorToolkit) hides whichever don't through
	// FMantleScriptGraphNodeDetails.
	UPROPERTY(EditAnywhere, Category = "Mantle")
	FString Op;
	UPROPERTY(EditAnywhere, Category = "Mantle", meta = (MultiLine = true))
	FString TextValue;
	UPROPERTY(EditAnywhere, Category = "Mantle")
	double NumberValue = 0.0;
	UPROPERTY(EditAnywhere, Category = "Mantle")
	bool BoolValue = false;
	UPROPERTY(EditAnywhere, Category = "Mantle")
	FVector VectorValue = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, Category = "Mantle")
	FString VariableName;
	UPROPERTY(EditAnywhere, Category = "Mantle")
	FString EventName;
	UPROPERTY(EditAnywhere, Category = "Mantle")
	FString FunctionName;
	/** Sequence: number of "then N" exec outputs. Concat/Format: number of value input pins. */
	UPROPERTY(EditAnywhere, Category = "Mantle", meta = (ClampMin = "1", ClampMax = "12"))
	int32 NumOutputs = 2;
	UPROPERTY(EditAnywhere, Category = "Mantle")
	double ForFirst = 0.0;
	UPROPERTY(EditAnywhere, Category = "Mantle")
	double ForLast = 10.0;
	UPROPERTY(EditAnywhere, Category = "Mantle")
	double ForStep = 1.0;
	UPROPERTY(EditAnywhere, Category = "Mantle")
	double DelaySeconds = 1.0;

	//~ Begin UEdGraphNode interface
	virtual void AllocateDefaultPins() override;
	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	virtual FText GetTooltipText() const override;
	virtual FLinearColor GetNodeTitleColor() const override;
	virtual FText GetKeywords() const override { return FText::FromString(NodeKind); }
	virtual bool CanDuplicateNode() const override { return NodeKind != TEXT("start"); }
	virtual bool CanUserDeleteNode() const override { return true; }
	//~ End UEdGraphNode interface

	/** True for a kind the compiler treats as pure/data-only (no exec pins at all) --
	 *  MantleGraphCompiler.cpp's MantleGraphIsPureKind, duplicated here for the same "no second
	 *  schema" reason NodeKind itself is a plain string: the palette and pin allocation need this
	 *  exact set and there is nowhere else in an editor-only module to share it from without a new
	 *  cross-module dependency for one predicate. */
	bool IsPureKind() const;
	/** "start" | "event" | "onevent" | "function": no exec input pin, only ever a root. */
	bool IsRootKind() const;

	/** Adds one more "then N" exec pin (Sequence) or "values N" input pin (Concat/Format). Bound
	 *  to the graph editor's node context menu. */
	void AddExtraPin();
	void RemoveExtraPin();

	/** Doc string shown as the tooltip for a catalogue-driven node (event/call/get/set); empty for
	 *  everything else, which falls back to a static per-kind description. */
	UPROPERTY()
	FString CatalogueDoc;
};
