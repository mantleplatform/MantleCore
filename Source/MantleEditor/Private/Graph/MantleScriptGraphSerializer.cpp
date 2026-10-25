// Copyright Joshua Eger. All Rights Reserved.
#include "Graph/MantleScriptGraphSerializer.h"

#include "Graph/MantleScriptEdGraph.h"
#include "Graph/MantleScriptGraphNode.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"

namespace
{
	FString MantleEdNodeId(const UMantleScriptGraphNode& Node)
	{
		return Node.NodeGuid.ToString(EGuidFormats::Digits);
	}

	/** Unconnected: a literal from the pin's own default value, for the three scalar categories a
	 *  pin can be edited inline for (MantleScriptGraphSchema's pin-colour categories). Connected:
	 *  an "@id[.output]" reference. Neither: nullptr, so the caller omits the JSON key entirely and
	 *  lets the graph compiler's own RequireField/RequireInput/CollectCallArgs explain a missing
	 *  value -- see MantleScriptGraphSerializer.h's header comment on why per-node validation isn't
	 *  duplicated here. */
	TSharedPtr<FJsonValue> ResolvePinValue(const UEdGraphPin& Pin, const TMap<UEdGraphNode*, FString>& IdMap)
	{
		if (Pin.LinkedTo.Num() > 0)
		{
			const UEdGraphPin* Source = Pin.LinkedTo[0];
			const FString* SourceId = IdMap.Find(Source->GetOwningNode());
			if (!SourceId)
			{
				return nullptr;
			}
			return MakeShared<FJsonValueString>(FString::Printf(TEXT("@%s.%s"), **SourceId, *Source->PinName.ToString()));
		}
		if (Pin.PinType.PinCategory == FMantleGraphPinCategory::Boolean)
		{
			return MakeShared<FJsonValueBoolean>(Pin.DefaultValue.ToBool());
		}
		if (Pin.PinType.PinCategory == FMantleGraphPinCategory::Number)
		{
			return MakeShared<FJsonValueNumber>(Pin.DefaultValue.IsEmpty() ? 0.0 : FCString::Atod(*Pin.DefaultValue));
		}
		if (Pin.PinType.PinCategory == FMantleGraphPinCategory::String)
		{
			return MakeShared<FJsonValueString>(Pin.DefaultValue);
		}
		if (Pin.PinType.PinCategory == FMantleGraphPinCategory::Vector3 && !Pin.DefaultValue.IsEmpty())
		{
			// "X,Y,Z" -- there is no inline vector pin widget (MantleScriptGraphEditorToolkit's
			// known simplification; use a Make Vector3 or Literal Vector3 node instead), but a
			// template graph (MantleScriptTemplates.cpp) still needs to express a fixed literal
			// position/offset without an extra node, so the plain default-value string is still
			// honoured here if something (a template, a future editor) sets one.
			FVector V;
			if (V.InitFromString(Pin.DefaultValue))
			{
				TSharedPtr<FJsonObject> Vec = MakeShared<FJsonObject>();
				TArray<TSharedPtr<FJsonValue>> Arr = {
					MakeShared<FJsonValueNumber>(V.X), MakeShared<FJsonValueNumber>(V.Y), MakeShared<FJsonValueNumber>(V.Z)
				};
				Vec->SetArrayField(TEXT("vector"), Arr);
				return MakeShared<FJsonValueObject>(Vec);
			}
		}
		return nullptr;
	}

	void CollectNonExecInputs(const UMantleScriptGraphNode& Node, const TMap<UEdGraphNode*, FString>& IdMap,
		TFunctionRef<void(const FString& PinName, const TSharedPtr<FJsonValue>& Value)> Visit)
	{
		for (const UEdGraphPin* Pin : Node.Pins)
		{
			if (Pin->Direction != EGPD_Input || Pin->PinType.PinCategory == FMantleGraphPinCategory::Exec || Pin->PinName == TEXT("target"))
			{
				continue;
			}
			Visit(Pin->PinName.ToString(), ResolvePinValue(*Pin, IdMap));
		}
	}

	/** Builds an ordered "values" array for Concat/Format from pins named "values0".."valuesN-1".
	 *  Unlike an ordinary optional input, a missing element here becomes an explicit JSON null
	 *  (rather than being skipped) so positions never shift out from under the node's own NumOutputs
	 *  -- ResolveValue in the graph compiler already turns a null array element into a clear
	 *  "references a missing value" error naming this node. */
	TArray<TSharedPtr<FJsonValue>> CollectIndexedValues(const UMantleScriptGraphNode& Node, const TMap<UEdGraphNode*, FString>& IdMap)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (int32 i = 0; i < Node.NumOutputs; ++i)
		{
			const UEdGraphPin* Pin = Node.FindPin(*FString::Printf(TEXT("values%d"), i), EGPD_Input);
			TSharedPtr<FJsonValue> V = Pin ? ResolvePinValue(*Pin, IdMap) : nullptr;
			Values.Add(V.IsValid() ? V : MakeShared<FJsonValueNull>());
		}
		return Values;
	}

	TSharedPtr<FJsonObject> BuildFields(const UMantleScriptGraphNode& Node, const FString& NodeId,
		const TMap<UEdGraphNode*, FString>& IdMap, FString& OutError)
	{
		TSharedPtr<FJsonObject> F = MakeShared<FJsonObject>();
		F->SetStringField(TEXT("id"), NodeId);
		F->SetStringField(TEXT("kind"), Node.NodeKind);

		auto SetFromPin = [&](const TCHAR* JsonKey, const TCHAR* PinName)
		{
			if (const UEdGraphPin* Pin = Node.FindPin(PinName, EGPD_Input))
			{
				if (TSharedPtr<FJsonValue> V = ResolvePinValue(*Pin, IdMap))
				{
					F->SetField(JsonKey, V);
				}
			}
		};

		const FString& Kind = Node.NodeKind;

		if (Kind == TEXT("event") || Kind == TEXT("call") || Kind == TEXT("get") || Kind == TEXT("set"))
		{
			F->SetStringField(TEXT("member"), Node.Dotted);
			SetFromPin(TEXT("target"), TEXT("target"));
			if (Kind == TEXT("set"))
			{
				SetFromPin(TEXT("value"), TEXT("value"));
			}
			else
			{
				TSharedPtr<FJsonObject> Inputs = MakeShared<FJsonObject>();
				CollectNonExecInputs(Node, IdMap, [&Inputs](const FString& Name, const TSharedPtr<FJsonValue>& V)
				{
					if (V.IsValid()) Inputs->SetField(Name, V);
				});
				if (Inputs->Values.Num() > 0)
				{
					F->SetObjectField(TEXT("inputs"), Inputs);
				}
			}
			if (Kind == TEXT("event") && Node.ExtraOutputNames.Num() > 0)
			{
				// Only consulted by the graph compiler when the registry's own handler signature
				// couldn't name its parameters (MantleGraphCompiler.cpp's EmitEventNode); harmless
				// to always include when the node has any (AllocateDefaultPins only ever populates
				// ExtraOutputNames-derived pins in exactly that situation).
				TArray<TSharedPtr<FJsonValue>> OutputNames;
				for (const FString& Name : Node.ExtraOutputNames)
				{
					OutputNames.Add(MakeShared<FJsonValueString>(Name));
				}
				F->SetArrayField(TEXT("outputs"), OutputNames);
			}
			return F;
		}
		if (Kind == TEXT("branch") || Kind == TEXT("while"))
		{
			SetFromPin(TEXT("condition"), TEXT("condition"));
			return F;
		}
		if (Kind == TEXT("sequence") || Kind == TEXT("doonce") || Kind == TEXT("start"))
		{
			return F;
		}
		if (Kind == TEXT("forloop"))
		{
			SetFromPin(TEXT("first"), TEXT("first"));
			SetFromPin(TEXT("last"), TEXT("last"));
			SetFromPin(TEXT("step"), TEXT("step"));
			return F;
		}
		if (Kind == TEXT("foreach"))
		{
			SetFromPin(TEXT("array"), TEXT("array"));
			return F;
		}
		if (Kind == TEXT("delay"))
		{
			SetFromPin(TEXT("seconds"), TEXT("seconds"));
			return F;
		}
		if (Kind == TEXT("setvar"))
		{
			F->SetStringField(TEXT("variable"), Node.VariableName);
			SetFromPin(TEXT("value"), TEXT("value"));
			return F;
		}
		if (Kind == TEXT("getvar"))
		{
			F->SetStringField(TEXT("variable"), Node.VariableName);
			return F;
		}
		if (Kind == TEXT("print") || Kind == TEXT("warn"))
		{
			SetFromPin(TEXT("text"), TEXT("text"));
			return F;
		}
		if (Kind == TEXT("literal"))
		{
			F->SetStringField(TEXT("type"), Node.LiteralType);
			if (Node.LiteralType == TEXT("bool"))
			{
				F->SetBoolField(TEXT("value"), Node.BoolValue);
			}
			else if (Node.LiteralType == TEXT("number"))
			{
				F->SetNumberField(TEXT("value"), Node.NumberValue);
			}
			else if (Node.LiteralType == TEXT("string"))
			{
				F->SetStringField(TEXT("value"), Node.TextValue);
			}
			else if (Node.LiteralType == TEXT("vector"))
			{
				TSharedPtr<FJsonObject> Vec = MakeShared<FJsonObject>();
				TArray<TSharedPtr<FJsonValue>> Arr = {
					MakeShared<FJsonValueNumber>(Node.VectorValue.X),
					MakeShared<FJsonValueNumber>(Node.VectorValue.Y),
					MakeShared<FJsonValueNumber>(Node.VectorValue.Z)
				};
				Vec->SetArrayField(TEXT("vector"), Arr);
				F->SetObjectField(TEXT("value"), Vec);
			}
			else
			{
				OutError = FString::Printf(TEXT("unknown literal type '%s'"), *Node.LiteralType);
				return nullptr;
			}
			return F;
		}
		if (Kind == TEXT("math"))
		{
			F->SetStringField(TEXT("op"), Node.Op);
			SetFromPin(TEXT("a"), TEXT("a"));
			if (Node.Op != TEXT("neg")) SetFromPin(TEXT("b"), TEXT("b"));
			return F;
		}
		if (Kind == TEXT("compare"))
		{
			F->SetStringField(TEXT("op"), Node.Op);
			SetFromPin(TEXT("a"), TEXT("a"));
			SetFromPin(TEXT("b"), TEXT("b"));
			return F;
		}
		if (Kind == TEXT("bool"))
		{
			F->SetStringField(TEXT("op"), Node.Op);
			SetFromPin(TEXT("a"), TEXT("a"));
			if (Node.Op != TEXT("not")) SetFromPin(TEXT("b"), TEXT("b"));
			return F;
		}
		if (Kind == TEXT("concat"))
		{
			F->SetArrayField(TEXT("values"), CollectIndexedValues(Node, IdMap));
			return F;
		}
		if (Kind == TEXT("format"))
		{
			F->SetStringField(TEXT("text"), Node.TextValue);
			F->SetArrayField(TEXT("values"), CollectIndexedValues(Node, IdMap));
			return F;
		}
		if (Kind == TEXT("makevector3"))
		{
			SetFromPin(TEXT("x"), TEXT("x"));
			SetFromPin(TEXT("y"), TEXT("y"));
			SetFromPin(TEXT("z"), TEXT("z"));
			return F;
		}
		if (Kind == TEXT("positiongoal"))
		{
			SetFromPin(TEXT("position"), TEXT("position"));
			return F;
		}
		if (Kind == TEXT("breakvector3"))
		{
			SetFromPin(TEXT("value"), TEXT("value"));
			return F;
		}
		if (Kind == TEXT("vectormath"))
		{
			F->SetStringField(TEXT("op"), Node.Op);
			SetFromPin(TEXT("a"), TEXT("a"));
			SetFromPin(TEXT("b"), TEXT("b"));
			return F;
		}
		if (Kind == TEXT("reroute"))
		{
			SetFromPin(TEXT("value"), TEXT("value"));
			return F;
		}
		if (Kind == TEXT("onevent") || Kind == TEXT("function"))
		{
			F->SetStringField(Kind == TEXT("onevent") ? TEXT("event") : TEXT("function"),
				Kind == TEXT("onevent") ? Node.EventName : Node.FunctionName);
			return F;
		}
		if (Kind == TEXT("callevent") || Kind == TEXT("callfunction"))
		{
			F->SetStringField(Kind == TEXT("callevent") ? TEXT("event") : TEXT("function"),
				Kind == TEXT("callevent") ? Node.EventName : Node.FunctionName);
			TSharedPtr<FJsonObject> Inputs = MakeShared<FJsonObject>();
			CollectNonExecInputs(Node, IdMap, [&Inputs](const FString& Name, const TSharedPtr<FJsonValue>& V)
			{
				if (V.IsValid()) Inputs->SetField(Name, V);
			});
			if (Inputs->Values.Num() > 0)
			{
				F->SetObjectField(TEXT("inputs"), Inputs);
			}
			return F;
		}
		if (Kind == TEXT("return"))
		{
			TSharedPtr<FJsonObject> Values = MakeShared<FJsonObject>();
			CollectNonExecInputs(Node, IdMap, [&Values](const FString& Name, const TSharedPtr<FJsonValue>& V)
			{
				if (V.IsValid()) Values->SetField(Name, V);
			});
			F->SetObjectField(TEXT("values"), Values);
			return F;
		}

		OutError = FString::Printf(TEXT("unrecognised node kind '%s'"), *Kind);
		return nullptr;
	}
}

bool FMantleScriptGraphSerializer::BuildGraph(const UMantleScriptEdGraph& EdGraph, const FString& GraphName, EMantleSide Side, FMantleGraph& OutGraph, FString& OutError)
{
	OutGraph = FMantleGraph();
	OutGraph.Name = GraphName;
	OutGraph.Side = Side;

	for (const FMantleEdGraphVariable& V : EdGraph.Variables)
	{
		OutGraph.Variables.Add(FMantleGraphVariable{V.Name, V.Type, nullptr});
	}
	for (const FMantleEdGraphCustomEvent& E : EdGraph.CustomEvents)
	{
		FMantleGraphCustomEvent CE;
		CE.Name = E.Name;
		for (const FMantleEdGraphParam& P : E.Params) CE.Params.Add(FMantleGraphParam{P.Name, P.Type});
		OutGraph.CustomEvents.Add(MoveTemp(CE));
	}
	for (const FMantleEdGraphFunction& Fn : EdGraph.Functions)
	{
		FMantleGraphFunction GF;
		GF.Name = Fn.Name;
		for (const FMantleEdGraphParam& P : Fn.Params) GF.Params.Add(FMantleGraphParam{P.Name, P.Type});
		for (const FMantleEdGraphParam& R : Fn.Returns) GF.Returns.Add(FMantleGraphParam{R.Name, R.Type});
		OutGraph.Functions.Add(MoveTemp(GF));
	}

	TMap<UEdGraphNode*, FString> IdMap;
	for (UEdGraphNode* N : EdGraph.Nodes)
	{
		if (const UMantleScriptGraphNode* MN = Cast<UMantleScriptGraphNode>(N))
		{
			IdMap.Add(N, MantleEdNodeId(*MN));
		}
	}

	for (UEdGraphNode* N : EdGraph.Nodes)
	{
		UMantleScriptGraphNode* MN = Cast<UMantleScriptGraphNode>(N);
		if (!MN)
		{
			continue; // e.g. a comment box: freeform annotation, never compiled
		}

		const FString NodeId = IdMap.FindChecked(N);
		FMantleGraphNode GraphNode;
		GraphNode.Id = NodeId;
		GraphNode.Kind = MN->NodeKind;

		FString FieldError;
		GraphNode.Fields = BuildFields(*MN, NodeId, IdMap, FieldError);
		if (!GraphNode.Fields.IsValid())
		{
			OutError = FString::Printf(TEXT("node '%s': %s"), *NodeId, *FieldError);
			return false;
		}
		OutGraph.Nodes.Add(MoveTemp(GraphNode));

		for (const UEdGraphPin* Pin : MN->Pins)
		{
			if (Pin->Direction != EGPD_Output || Pin->PinType.PinCategory != FMantleGraphPinCategory::Exec)
			{
				continue;
			}
			for (const UEdGraphPin* Linked : Pin->LinkedTo)
			{
				const UMantleScriptGraphNode* TargetNode = Cast<UMantleScriptGraphNode>(Linked->GetOwningNode());
				if (!TargetNode)
				{
					OutError = FString::Printf(TEXT("node '%s': an exec wire targets something that isn't a script node"), *NodeId);
					return false;
				}
				FMantleGraphExecWire Wire;
				Wire.From = NodeId;
				Wire.Port = Pin->PinName.ToString();
				Wire.To = IdMap.FindChecked(Linked->GetOwningNode());
				OutGraph.Exec.Add(MoveTemp(Wire));
			}
		}
	}
	return true;
}

FString FMantleScriptGraphSerializer::ApplyCompileResult(UMantleScriptEdGraph& EdGraph, const FMantleGraphCompileResult& Result)
{
	TMap<FString, UMantleScriptGraphNode*> ById;
	for (UEdGraphNode* N : EdGraph.Nodes)
	{
		if (UMantleScriptGraphNode* MN = Cast<UMantleScriptGraphNode>(N))
		{
			MN->bHasCompilerMessage = false;
			MN->ErrorMsg.Empty();
			ById.Add(MantleEdNodeId(*MN), MN);
		}
	}

	if (Result.bSuccess)
	{
		return FString();
	}

	// The graph compiler always writes a node-owned failure as `node '<id>': <reason>` (see
	// MantleGraphCompiler.h); a handful of whole-graph structural failures have no single node and
	// read as plain text instead, and are left unapplied here (only the compiler results panel
	// shows those).
	static const FString Prefix(TEXT("node '"));
	if (!Result.Error.StartsWith(Prefix))
	{
		return FString();
	}
	FString Id, Reason;
	if (!Result.Error.Mid(Prefix.Len()).Split(TEXT("': "), &Id, &Reason))
	{
		return FString();
	}
	UMantleScriptGraphNode** Found = ById.Find(Id);
	if (!Found)
	{
		return FString();
	}
	(*Found)->bHasCompilerMessage = true;
	(*Found)->ErrorType = int32(EMessageSeverity::Error);
	(*Found)->ErrorMsg = Reason;
	return Id;
}

bool FMantleScriptGraphSerializer::ClassifySide(const FString& PackagePath, EMantleSide& OutSide, FString& OutWarning)
{
	OutSide = EMantleSide::Server;
	OutWarning.Reset();

	TArray<FString> Segments;
	PackagePath.ParseIntoArray(Segments, TEXT("/"));
	int32 ScriptsIndex = INDEX_NONE;
	for (int32 i = 0; i < Segments.Num(); ++i)
	{
		if (Segments[i] == TEXT("Scripts"))
		{
			ScriptsIndex = i;
			break;
		}
	}
	if (ScriptsIndex == INDEX_NONE)
	{
		// Packages/<name>/ stands where Scripts/ does for a script package's graph.
		for (int32 i = 0; i + 1 < Segments.Num(); ++i)
		{
			if (Segments[i].Equals(TEXT("Packages"), ESearchCase::CaseSensitive))
			{
				ScriptsIndex = i + 1;
				break;
			}
		}
	}
	if (ScriptsIndex == INDEX_NONE || ScriptsIndex + 1 >= Segments.Num())
	{
		OutWarning = TEXT("This graph isn't inside a level's Scripts/Server or Scripts/Client folder.");
		return false;
	}

	const FString& Top = Segments[ScriptsIndex + 1];
	if (Top.Equals(TEXT("Server"), ESearchCase::CaseSensitive))
	{
		OutSide = EMantleSide::Server;
		return true;
	}
	if (Top.Equals(TEXT("Client"), ESearchCase::CaseSensitive))
	{
		OutSide = EMantleSide::Client;
		return true;
	}
	// A shorter form of MantlePlaceValidator.cpp's 'not inside Server/, Client/ or Shared/' message.
	OutWarning = FString::Printf(TEXT("Scripts/%s is not inside Server/ or Client/ (case-exact). Move this graph to fix which side it runs on."), *Top);
	return false;
}

FString FMantleScriptGraphSerializer::CompiledLuauPathForAsset(const FString& PackageName)
{
	FString Filename;
	if (!FPackageName::TryConvertLongPackageNameToFilename(PackageName, Filename))
	{
		return FString();
	}
	return FPaths::SetExtension(Filename, TEXT("luau"));
}
