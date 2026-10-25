// Copyright Joshua Eger. All Rights Reserved.
// Small helpers for building a graph directly in C++ -- node-by-node, wire-by-wire -- with
// no Slate involved. Used by MantleScriptTemplates.cpp (the five graph "New Script" templates:
// kill brick, coins, round timer + HUD, door), so templates build graphs the exact same way a
// hand-placed one ends up shaped, rather than each reinventing pin lookup and wiring.
#pragma once

#include "CoreMinimal.h"

class UMantleScriptEdGraph;
class UMantleScriptGraphNode;

class MANTLEEDITOR_API FMantleScriptGraphBuilder
{
public:
	/** Adds one node of Kind to Graph at Position, with no further configuration -- the common case
	 *  for a kind with no Dotted/Op/etc to set (start, doonce, branch, sequence, math with its Op
	 *  set some other way, ...). See the Configure overload below for the rest. */
	static UMantleScriptGraphNode& AddNode(UMantleScriptEdGraph& Graph, const FString& Kind, const FVector2D& Position);

	/** Adds one node of Kind to Graph at Position. Configure runs before AllocateDefaultPins, so
	 *  it's the place to set Dotted/Op/VariableName/EventName/FunctionName/LiteralType/NumOutputs
	 *  -- whatever AllocateDefaultPins for this Kind reads (MantleScriptGraphNode.cpp) -- and, for
	 *  onevent/function/getvar/setvar/callevent/callfunction, the referenced variable/event/
	 *  function must already be declared on Graph (Graph.Variables/CustomEvents/Functions).
	 *  A separate overload from the no-argument one above rather than a default argument:
	 *  TFunctionRef has no sensible default (nothing to safely bind a temporary to at every call
	 *  site), so the two-overload split is the clean way to keep the no-configuration case free of
	 *  an empty `[](UMantleScriptGraphNode&){}` at every call site. */
	static UMantleScriptGraphNode& AddNode(UMantleScriptEdGraph& Graph, const FString& Kind,
		const FVector2D& Position, TFunctionRef<void(UMantleScriptGraphNode&)> Configure);

	/** Convenience for the common event/call/get/set case with nothing else to configure. */
	static UMantleScriptGraphNode& AddMemberNode(UMantleScriptEdGraph& Graph, const FString& Kind, const FString& Dotted,
		const FVector2D& Position);

	/** As above, plus Configure for anything beyond Dotted. */
	static UMantleScriptGraphNode& AddMemberNode(UMantleScriptEdGraph& Graph, const FString& Kind, const FString& Dotted,
		const FVector2D& Position, TFunctionRef<void(UMantleScriptGraphNode&)> Configure);

	/** Links From's FromPinName output to To's ToPinName input (exec or data -- whichever pin
	 *  category FromPinName resolves to). No-ops (silently) if either pin doesn't exist, so a
	 *  typo shows up as a compile error naming the node instead of a crash. */
	static void Wire(UMantleScriptGraphNode& From, const FString& FromPinName, UMantleScriptGraphNode& To, const FString& ToPinName);

	/** Sets an unconnected input pin's literal default (bool: "true"/"false"; number: e.g. "5";
	 *  string: raw text). No-op if PinName doesn't exist. */
	static void SetDefault(UMantleScriptGraphNode& Node, const FString& PinName, const FString& Value);

	/** Vector3 input pins have no inline editor widget in the graph editor yet (a documented
	 *  simplification -- use a Make Vector3 or Literal Vector3 node instead), but a template still
	 *  needs to express a fixed literal without an extra node; this writes the same "X=.. Y=.. Z=.."
	 *  format FVector::ToString()/InitFromString() use, which
	 *  MantleScriptGraphSerializer::BuildGraph's pin-resolution already understands. */
	static void SetVectorDefault(UMantleScriptGraphNode& Node, const FString& PinName, const FVector& Value);
};
