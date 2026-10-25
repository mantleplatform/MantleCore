// Copyright Joshua Eger. All Rights Reserved.
// The JSON graph model for visual scripting: a typed C++ model wide enough for the
// Blueprint-basics wave: branches, loops, script variables, custom events/functions, and
// reroute/comment nodes, on top of the event/call graph.
//
// A node's kind-specific fields are read directly off its raw JSON object (Node.Fields) by the
// compiler rather than duplicated into typed C++ members per kind -- there are ~25 node kinds
// and most only need one or two fields, so a second copy of the schema here would just be a
// second place to keep in sync. What *is* structurally typed -- id, kind, exec wires, variables,
// custom events/functions -- is exactly what graph-shape validation (cycles, unknown wires,
// unconnected requireds) needs to walk without re-parsing JSON at every step.
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "MantleScriptSide.h"

/** A script-local variable declared on the graph (Blueprint's "My Blueprint > Variables"). */
struct MANTLEEDITOR_API FMantleGraphVariable
{
	FString Name;
	/** "bool" | "number" | "string" | "Vector3" | "Entity" | "Player" | "list" | "map". */
	FString Type;
	/** Optional; absent means the language's zero value (false/0/""/nil/{}). */
	TSharedPtr<FJsonValue> Default;
};

/** One named parameter of a custom event or function (declaration, not a call-site argument). */
struct MANTLEEDITOR_API FMantleGraphParam
{
	FString Name;
	FString Type;
};

/** A graph-local custom event: `graph.customEvents[]`. Fired by a "callevent" node, handled by
 *  an "onevent" node naming it. */
struct MANTLEEDITOR_API FMantleGraphCustomEvent
{
	FString Name;
	TArray<FMantleGraphParam> Params;
};

/** A graph-local function: `graph.functions[]`. Has its own exec root ("function" node) and
 *  must terminate every path with a "return" node. */
struct MANTLEEDITOR_API FMantleGraphFunction
{
	FString Name;
	TArray<FMantleGraphParam> Params;
	TArray<FMantleGraphParam> Returns;
};

/** One node instance. Id is author-assigned and stable (the editor never reassigns an
 *  id it didn't just create); it is what the line map reports runtime errors against. */
struct MANTLEEDITOR_API FMantleGraphNode
{
	FString Id;
	/** "start" | "event" | "onevent" | "function" | "branch" | "sequence" | "forloop" |
	 *  "foreach" | "while" | "delay" | "doonce" | "call" | "get" | "set" | "callevent" |
	 *  "callfunction" | "return" | "print" | "warn" | "literal" | "getvar" | "setvar" | "math" |
	 *  "compare" | "bool" | "concat" | "format" | "makevector3" | "breakvector3" | "vectormath" | "positiongoal" |
	 *  "comment" | "reroute". */
	FString Kind;
	/** The node's own JSON object verbatim (including "id"/"kind"), so kind-specific fields can
	 *  be read without a second schema. Never null after a successful parse. */
	TSharedPtr<FJsonObject> Fields;
};

/** One exec (control-flow) wire. Port distinguishes a node's several exec outputs: plain flow
 *  uses "next"; Branch uses "true"/"false"; loops use "body" (the repeated chain) and "done"
 *  (after the loop finishes); Sequence uses "0", "1", "2", .... */
struct MANTLEEDITOR_API FMantleGraphExecWire
{
	FString From;
	FString Port = TEXT("next");
	FString To;
};

/** A parsed graph, exactly what the editor saves and publish uploads as `authorSource`.
 *  Everything the compiler needs to validate and emit Luau lives here; nothing is
 *  re-read from disk mid-compile. */
struct MANTLEEDITOR_API FMantleGraph
{
	FString Name;
	EMantleSide Side = EMantleSide::Server;
	TArray<FMantleGraphVariable> Variables;
	TArray<FMantleGraphCustomEvent> CustomEvents;
	TArray<FMantleGraphFunction> Functions;
	TArray<FMantleGraphNode> Nodes;
	TArray<FMantleGraphExecWire> Exec;

	const FMantleGraphNode* FindNode(const FString& Id) const;
	const FMantleGraphVariable* FindVariable(const FString& Name) const;
	const FMantleGraphCustomEvent* FindCustomEvent(const FString& Name) const;
	const FMantleGraphFunction* FindFunction(const FString& Name) const;

	/** Parse a graph from JSON text. On failure, returns false and fills OutError with a message
	 *  naming the offending path (not necessarily a node -- structural errors, e.g. a missing
	 *  "nodes" array, have no node yet). Node-level semantic errors are the compiler's job, not
	 *  the parser's: parsing only enforces the shape (fields present and the right JSON type). */
	static bool ParseFromJson(const FString& JsonText, FMantleGraph& OutGraph, FString& OutError);
	static bool ParseFromFile(const FString& FilePath, FMantleGraph& OutGraph, FString& OutError);

	/** Writes the graph in canonical JSON form. Keys and object members are sorted so exporting an
	 *  unchanged graph produces the same authorSource content id on every publish. */
	static FString ToJson(const FMantleGraph& Graph);
};
