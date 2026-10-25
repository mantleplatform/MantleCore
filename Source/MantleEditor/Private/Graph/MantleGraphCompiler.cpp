// Copyright Joshua Eger. All Rights Reserved.
#include "Graph/MantleGraphCompiler.h"

#include "Dom/JsonValue.h"
#include "Graph/MantleGraphCatalogue.h"
#include "Misc/Base64.h"
#include "Serialization/JsonWriter.h"

namespace
{
	/** A short, stable label for diagnostics. The editor's visual title for a member node is its
	 * member name (Teleport rather than Player.Teleport); keep the generated source map equally
	 * creator-facing without making the runtime import graph palette/editor types. */
	FString MantleGraphDiagnosticNodeTitle(const FMantleGraphNode& Node)
	{
		if (Node.Kind == TEXT("call") || Node.Kind == TEXT("get") || Node.Kind == TEXT("set") || Node.Kind == TEXT("event"))
		{
			FString Dotted;
			if (Node.Fields.IsValid() && Node.Fields->TryGetStringField(TEXT("member"), Dotted))
			{
				FString Ignored;
				FString Member;
				if (Dotted.Split(TEXT("."), &Ignored, &Member, ESearchCase::CaseSensitive, ESearchDir::FromEnd) && !Member.IsEmpty())
				{
					return Member;
				}
			}
		}
		if (Node.Kind == TEXT("onevent") || Node.Kind == TEXT("callevent")) return TEXT("Event");
		if (Node.Kind == TEXT("function") || Node.Kind == TEXT("callfunction")) return TEXT("Function");
		if (Node.Kind.IsEmpty()) return TEXT("Node");
		FString Title = Node.Kind;
		Title[0] = FChar::ToUpper(Title[0]);
		return Title;
	}

	/** Kept as a single comment so it survives normal Luau loading unchanged. The schema is
	 * deliberately mundane base64 JSON (`version`, `entries[].line/id/title`) so MantleScript can
	 * read it using only Core/Json, with no MantleEditor dependency. */
	FString MantleEncodeGraphSourceMapHeader(const TArray<FMantleGraphLineMapEntry>& Entries, const FMantleGraph& Graph)
	{
		FString Json;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("version"), 1);
		Writer->WriteArrayStart(TEXT("entries"));
		for (const FMantleGraphLineMapEntry& Entry : Entries)
		{
			Writer->WriteObjectStart();
			Writer->WriteValue(TEXT("line"), Entry.Line);
			Writer->WriteValue(TEXT("id"), Entry.NodeId);
			const FMantleGraphNode* Node = Graph.FindNode(Entry.NodeId);
			Writer->WriteValue(TEXT("title"), Node ? MantleGraphDiagnosticNodeTitle(*Node) : TEXT("Node"));
			Writer->WriteObjectEnd();
		}
		Writer->WriteArrayEnd();
		Writer->WriteObjectEnd();
		Writer->Close();
		return TEXT("-- MANTLE_GRAPH_MAP v1 ") + FBase64::Encode(Json);
	}

	bool MantleGraphIsPureKind(const FString& Kind)
	{
		static const TSet<FString> PureKinds = {
			TEXT("literal"), TEXT("getvar"), TEXT("math"), TEXT("compare"), TEXT("bool"),
			TEXT("concat"), TEXT("format"), TEXT("makevector3"), TEXT("breakvector3"),
			TEXT("vectormath"), TEXT("positiongoal"), TEXT("reroute"), TEXT("get")
		};
		return PureKinds.Contains(Kind);
	}

	/** Normalises a registry/graph type annotation for equality comparison: strips a trailing
	 *  "?" (optional marker) and "bool"/"vector" spellings used by graph JSON onto the registry's
	 *  own "boolean"/"Vector3". Array ("{T}") and function ("(...) -> ...") types, and "any", are
	 *  left alone -- MantleGraphTypesCompatible treats those as unchecked. */
	FString MantleGraphNormalizeType(const FString& InType)
	{
		FString T = InType;
		if (T.EndsWith(TEXT("?")))
		{
			T = T.LeftChop(1);
		}
		if (T == TEXT("bool")) return TEXT("boolean");
		if (T == TEXT("vector")) return TEXT("Vector3");
		return T;
	}

	bool MantleGraphTypesCompatible(const FString& Expected, const FString& Actual)
	{
		if (Expected.IsEmpty() || Actual.IsEmpty() || Expected == TEXT("any") || Actual == TEXT("any"))
		{
			return true; // unknown or explicitly dynamic: nothing to check
		}
		const FString E = MantleGraphNormalizeType(Expected);
		const FString A = MantleGraphNormalizeType(Actual);
		if (E.StartsWith(TEXT("{")) || E.StartsWith(TEXT("(")) || A.StartsWith(TEXT("{")) || A.StartsWith(TEXT("(")))
		{
			return true; // arrays/maps/function types: not modelled precisely enough to check
		}
		return E == A;
	}
}

/** Holds one compile's mutable state. A fresh instance per FMantleGraphCompiler::Compile call,
 *  so the compiler itself stays a stateless, thread-safe static API. Everything below is a
 *  member (not a free function) precisely so unity builds concatenating this file with others
 *  can never see a name collision. */
class FMantleGraphCompilerRun
{
public:
	explicit FMantleGraphCompilerRun(const FMantleGraph& InGraph) : Graph(InGraph) {}

	FMantleGraphCompileResult Run();

private:
	const FMantleGraph& Graph;
	FMantleGraphCompileResult Result;

	TMap<FString, const FMantleGraphNode*> NodesById;
	/** NodeId -> Port -> target NodeId. Built once from Graph.Exec. */
	TMap<FString, TMap<FString, FString>> ExecTargets;
	/** Lines of the compiled file in order, each tagged with the node that produced it (empty
	 *  for header/declaration lines with no single owner). */
	TArray<TPair<FString, FString>> Lines;
	/** NodeId -> output pin name -> Luau expression text. Populated as each node compiles. */
	TMap<FString, TMap<FString, FString>> OutputExpr;
	/** NodeId -> output pin name -> best-known type, for the pin-type check. */
	TMap<FString, TMap<FString, FString>> OutputType;
	TSet<FString> PureNodesInProgress;
	/** Every node id referenced anywhere in the graph as "@id" or "@id.output", computed once up
	 *  front (ComputeReferencedNodeIds) so EmitCallNode can tell whether a registry call's return
	 *  value is ever actually read before deciding whether to bind it to a local at all -- an
	 *  unconsumed `local x = ...` is dead-code noise a creator may read, and trips luau-lsp's
	 *  unused-variable diagnostic under the strict mode generated graphs are type-checked in. */
	TSet<FString> ReferencedNodeIds;

    // Node IDs are source-map identities, not Luau identifiers (editor GUIDs may begin
    // with a digit). Allocate local bindings separately and preserve the original IDs.
    TMap<FString, TMap<FString, FString>> NodeLocals;
    TSet<FString> UsedLocalNames;
    FString NodeLocal(const FString& NodeId, const FString& Output)
    {
        TMap<FString, FString>& Locals = NodeLocals.FindOrAdd(NodeId);
        if (const FString* Existing = Locals.Find(Output)) return *Existing;
        const auto IdentifierPart = [](const FString& Text)
        {
            FString Safe;
            for (TCHAR Ch : Text)
            {
                Safe += ((Ch >= TEXT('a') && Ch <= TEXT('z')) || (Ch >= TEXT('A') && Ch <= TEXT('Z')) ||
                    (Ch >= TEXT('0') && Ch <= TEXT('9')) || Ch == TEXT('_')) ? Ch : TEXT('_');
            }
            return Safe;
        };
        const FString Base = TEXT("n_") + IdentifierPart(NodeId) + TEXT("_") + IdentifierPart(Output);
        FString Name = Base;
        int32 Suffix = 1;
        while (UsedLocalNames.Contains(Name)) Name = Base + TEXT("_") + FString::FromInt(Suffix++);
        UsedLocalNames.Add(Name);
        Locals.Add(Output, Name);
        return Name;
    }


	bool HasFailed() const { return !Result.Error.IsEmpty(); }
	/** Every reported failure names the node it belongs to, per issue #135's check requirement. */
	void Fail(const FString& NodeId, const FString& Message)
	{
		if (!HasFailed())
		{
			Result.Error = FString::Printf(TEXT("node '%s': %s"), *NodeId, *Message);
		}
	}
	/** For the handful of whole-graph structural problems with no single owning node. */
	void FailGraph(const FString& Message)
	{
		if (!HasFailed())
		{
			Result.Error = Message;
		}
	}

	void EmitLine(const FString& NodeId, const FString& Text) { Lines.Add(TPair<FString, FString>(Text, NodeId)); }
	static FString Indent(int32 Level)
	{
		FString Out;
		for (int32 i = 0; i < Level; ++i) Out += TEXT("\t");
		return Out;
	}

	const FString* FindExecTarget(const FString& NodeId, const FString& Port) const
	{
		const TMap<FString, FString>* Ports = ExecTargets.Find(NodeId);
		return Ports ? Ports->Find(Port) : nullptr;
	}

	// Structural passes -----------------------------------------------------------------------
	void BuildExecTargets();
	void CheckExecCycles();
	void CheckCustomEventsAndFunctionsHaveRoots();
	/** Fills ReferencedNodeIds by walking every node's Fields for "@id"/"@id.output" strings. */
	void ComputeReferencedNodeIds();

	// Value / type resolution -------------------------------------------------------------------
	FString ResolveValue(const TSharedPtr<FJsonValue>& Value, const FString& ConsumerNodeId, const TSet<FString>& Scope);
	FString InferType(const TSharedPtr<FJsonValue>& Value, const TSet<FString>& Scope);
	void EnsurePureNodeCompiled(const FString& NodeId, const TSet<FString>& Scope);
	/** Looks up a required pin value from a node's "inputs" object; fails (naming NodeId) if
	 *  absent. Returns the resolved Luau expression, or an empty string on failure. */
	FString RequireInput(const TSharedPtr<FJsonObject>& InputsObj, const FString& PinName, const FString& NodeId,
		const FString& Dotted, const TSet<FString>& Scope, const FString& ExpectedType);
	FString RequireField(const FMantleGraphNode& Node, const FString& FieldName, const TSet<FString>& Scope, const FString& ExpectedType);
	void CheckMemberSide(const FString& NodeId, const FMantleGraphNodeDef& Def);

	// Node compilation --------------------------------------------------------------------------
	/** NodeId -> the CurrentFunction it belongs to, so "return" knows what it's returning from. */
	void EmitExecChain(const FString& NodeId, TSet<FString> Scope, int32 IndentLevel, const FMantleGraphFunction* CurrentFunction);
	void EmitEventNode(const FMantleGraphNode& Node, TSet<FString> Scope, int32 IndentLevel, const FMantleGraphFunction* CurrentFunction);
	void EmitCallNode(const FMantleGraphNode& Node, TSet<FString> Scope, int32 IndentLevel, const FMantleGraphFunction* CurrentFunction);
	void EmitSetNode(const FMantleGraphNode& Node, const TSet<FString>& Scope, int32 IndentLevel);
	void EmitPrintNode(const FMantleGraphNode& Node, const TSet<FString>& Scope, int32 IndentLevel, bool bWarn);
	void EmitDelayNode(const FMantleGraphNode& Node, const TSet<FString>& Scope, int32 IndentLevel);
	void EmitSetVarNode(const FMantleGraphNode& Node, const TSet<FString>& Scope, int32 IndentLevel);
	void EmitCallEventNode(const FMantleGraphNode& Node, const TSet<FString>& Scope, int32 IndentLevel);
	void EmitCallFunctionNode(const FMantleGraphNode& Node, const TSet<FString>& Scope, int32 IndentLevel);
	void EmitBranchNode(const FMantleGraphNode& Node, TSet<FString> Scope, int32 IndentLevel, const FMantleGraphFunction* CurrentFunction);
	void EmitSequenceNode(const FMantleGraphNode& Node, TSet<FString> Scope, int32 IndentLevel, const FMantleGraphFunction* CurrentFunction);
	void EmitForLoopNode(const FMantleGraphNode& Node, TSet<FString> Scope, int32 IndentLevel, const FMantleGraphFunction* CurrentFunction);
	void EmitForEachNode(const FMantleGraphNode& Node, TSet<FString> Scope, int32 IndentLevel, const FMantleGraphFunction* CurrentFunction);
	void EmitWhileNode(const FMantleGraphNode& Node, TSet<FString> Scope, int32 IndentLevel, const FMantleGraphFunction* CurrentFunction);
	void EmitDoOnceNode(const FMantleGraphNode& Node, TSet<FString> Scope, int32 IndentLevel, const FMantleGraphFunction* CurrentFunction);
	void EmitReturnNode(const FMantleGraphNode& Node, const TSet<FString>& Scope, int32 IndentLevel, const FMantleGraphFunction* CurrentFunction);

	FString CollectCallArgs(const TArray<FMantleGraphPinDef>& Inputs, bool bVariadic, const TSharedPtr<FJsonObject>& NodeFields,
		const TSharedPtr<FJsonObject>& InputsObj, const FString& NodeId, const FString& Dotted, const TSet<FString>& Scope);
};

// -------------------------------------------------------------------------------------------
// Value resolution

FString FMantleGraphCompilerRun::ResolveValue(const TSharedPtr<FJsonValue>& Value, const FString& ConsumerNodeId, const TSet<FString>& Scope)
{
	if (!Value.IsValid() || Value->IsNull())
	{
		Fail(ConsumerNodeId, TEXT("references a missing value"));
		return FString();
	}
	// Dispatch on the JSON type itself (Value->Type), not on which TryGetX() happens to return
	// true first: FJsonValueNumber and FJsonValueBoolean both *also* implement TryGetString() as
	// a lenient string-representation coercion (e.g. FJsonValueNumber::TryGetString gives "5"),
	// so checking TryGetString() before TryGetBool()/TryGetNumber() silently turned every number
	// and boolean literal into a quoted Luau string.
	if (Value->Type == EJson::Boolean)
	{
		bool AsBool = false;
		Value->TryGetBool(AsBool);
		return AsBool ? TEXT("true") : TEXT("false");
	}
	if (Value->Type == EJson::Number)
	{
		double AsNumber = 0.0;
		Value->TryGetNumber(AsNumber);
		return FMantleGraphCompiler::FormatLuauNumber(AsNumber);
	}
	if (Value->Type == EJson::String)
	{
		FString AsString;
		Value->TryGetString(AsString);
		if (AsString.StartsWith(TEXT("@")))
		{
			FString Ref = AsString.Mid(1);
			FString NodeId, OutputName;
			if (!Ref.Split(TEXT("."), &NodeId, &OutputName))
			{
				NodeId = Ref;
			}
			const FMantleGraphNode* RefNode = NodesById.FindRef(NodeId);
			if (!RefNode)
			{
				Fail(ConsumerNodeId, FString::Printf(TEXT("'%s' references node '%s', which doesn't exist"), *AsString, *NodeId));
				return FString();
			}
			if (MantleGraphIsPureKind(RefNode->Kind))
			{
				EnsurePureNodeCompiled(NodeId, Scope);
			}
			else if (!Scope.Contains(NodeId))
			{
				Fail(ConsumerNodeId, FString::Printf(TEXT("'%s' is used before node '%s' runs (check the exec wires)"), *AsString, *NodeId));
				return FString();
			}
			if (HasFailed())
			{
				return FString();
			}
			const TMap<FString, FString>* Outs = OutputExpr.Find(NodeId);
			if (!Outs || Outs->Num() == 0)
			{
				Fail(ConsumerNodeId, FString::Printf(TEXT("node '%s' has no outputs"), *NodeId));
				return FString();
			}
			if (OutputName.IsEmpty())
			{
				if (Outs->Num() != 1)
				{
					TArray<FString> Names;
					Outs->GetKeys(Names);
					Fail(ConsumerNodeId, FString::Printf(TEXT("'@%s' is ambiguous; specify one of: %s"), *NodeId, *FString::Join(Names, TEXT(", "))));
					return FString();
				}
				for (const TPair<FString, FString>& Pair : *Outs)
				{
					return Pair.Value;
				}
			}
			const FString* Expr = Outs->Find(OutputName);
			if (!Expr)
			{
				Fail(ConsumerNodeId, FString::Printf(TEXT("node '%s' has no output '%s'"), *NodeId, *OutputName));
				return FString();
			}
			return *Expr;
		}
		return TEXT("\"") + FMantleGraphCompiler::EscapeLuauString(AsString) + TEXT("\"");
	}
	const TSharedPtr<FJsonObject>* AsObject = nullptr;
	if (Value->Type == EJson::Object && Value->TryGetObject(AsObject))
	{
		const TArray<TSharedPtr<FJsonValue>>* Vec = nullptr;
		if ((*AsObject)->TryGetArrayField(TEXT("vector"), Vec) && Vec->Num() == 3)
		{
			double X, Y, Z;
			if ((*Vec)[0]->TryGetNumber(X) && (*Vec)[1]->TryGetNumber(Y) && (*Vec)[2]->TryGetNumber(Z))
			{
				return FString::Printf(TEXT("Vector3.new(%s, %s, %s)"),
					*FMantleGraphCompiler::FormatLuauNumber(X), *FMantleGraphCompiler::FormatLuauNumber(Y), *FMantleGraphCompiler::FormatLuauNumber(Z));
			}
		}
		Fail(ConsumerNodeId, TEXT("expected {\"vector\": [x, y, z]}"));
		return FString();
	}
	Fail(ConsumerNodeId, TEXT("unsupported value; expected a literal, {\"vector\": [x, y, z]}, or \"@node.output\""));
	return FString();
}

FString FMantleGraphCompilerRun::InferType(const TSharedPtr<FJsonValue>& Value, const TSet<FString>& Scope)
{
	if (!Value.IsValid() || Value->IsNull())
	{
		return FString();
	}
	// Same EJson-based dispatch as ResolveValue, and for the same reason: TryGetString() alone
	// can't tell a real JSON string from a number/boolean being coerced to one.
	if (Value->Type == EJson::Boolean)
	{
		return TEXT("boolean");
	}
	if (Value->Type == EJson::Number)
	{
		return TEXT("number");
	}
	if (Value->Type == EJson::String)
	{
		FString AsString;
		Value->TryGetString(AsString);
		if (AsString.StartsWith(TEXT("@")))
		{
			FString Ref = AsString.Mid(1);
			FString NodeId, OutputName;
			if (!Ref.Split(TEXT("."), &NodeId, &OutputName))
			{
				NodeId = Ref;
			}
			const TMap<FString, FString>* Types = OutputType.Find(NodeId);
			if (!Types)
			{
				return FString(); // not compiled yet (e.g. a forward exec reference); skip the check rather than guess
			}
			if (OutputName.IsEmpty())
			{
				if (Types->Num() == 1)
				{
					for (const TPair<FString, FString>& Pair : *Types) return Pair.Value;
				}
				return FString();
			}
			const FString* T = Types->Find(OutputName);
			return T ? *T : FString();
		}
		return TEXT("string");
	}
	const TSharedPtr<FJsonObject>* AsObject = nullptr;
	if (Value->Type == EJson::Object && Value->TryGetObject(AsObject) && (*AsObject)->HasField(TEXT("vector")))
	{
		return TEXT("Vector3");
	}
	return FString();
}

void FMantleGraphCompilerRun::EnsurePureNodeCompiled(const FString& NodeId, const TSet<FString>& Scope)
{
	if (OutputExpr.Contains(NodeId) || HasFailed())
	{
		return;
	}
	if (PureNodesInProgress.Contains(NodeId))
	{
		Fail(NodeId, TEXT("its data pins form a cycle (a node's value depends, directly or indirectly, on itself)"));
		return;
	}
	PureNodesInProgress.Add(NodeId);

	const FMantleGraphNode* Node = NodesById.FindRef(NodeId);
	check(Node);
	const TSharedPtr<FJsonObject>& F = Node->Fields;
	const FString& Kind = Node->Kind;

	if (Kind == TEXT("literal"))
	{
		FString Type;
		if (!F->TryGetStringField(TEXT("type"), Type))
		{
			Fail(NodeId, TEXT("literal needs a \"type\""));
		}
		else if (Type == TEXT("bool"))
		{
			bool V = false;
			F->TryGetBoolField(TEXT("value"), V);
			OutputExpr.FindOrAdd(NodeId).Add(TEXT("value"), V ? TEXT("true") : TEXT("false"));
			OutputType.FindOrAdd(NodeId).Add(TEXT("value"), TEXT("boolean"));
		}
		else if (Type == TEXT("number"))
		{
			double V = 0.0;
			F->TryGetNumberField(TEXT("value"), V);
			OutputExpr.FindOrAdd(NodeId).Add(TEXT("value"), FMantleGraphCompiler::FormatLuauNumber(V));
			OutputType.FindOrAdd(NodeId).Add(TEXT("value"), TEXT("number"));
		}
		else if (Type == TEXT("string"))
		{
			FString V;
			F->TryGetStringField(TEXT("value"), V);
			OutputExpr.FindOrAdd(NodeId).Add(TEXT("value"), TEXT("\"") + FMantleGraphCompiler::EscapeLuauString(V) + TEXT("\""));
			OutputType.FindOrAdd(NodeId).Add(TEXT("value"), TEXT("string"));
		}
		else if (Type == TEXT("vector"))
		{
			const FString Expr = ResolveValue(F->TryGetField(TEXT("value")), NodeId, Scope);
			OutputExpr.FindOrAdd(NodeId).Add(TEXT("value"), Expr);
			OutputType.FindOrAdd(NodeId).Add(TEXT("value"), TEXT("Vector3"));
		}
		else
		{
			Fail(NodeId, FString::Printf(TEXT("unknown literal type '%s'"), *Type));
		}
	}
	else if (Kind == TEXT("getvar"))
	{
		FString VarName;
		if (!F->TryGetStringField(TEXT("variable"), VarName))
		{
			Fail(NodeId, TEXT("getvar needs a \"variable\""));
		}
		else
		{
			const FMantleGraphVariable* Var = Graph.FindVariable(VarName);
			if (!Var)
			{
				Fail(NodeId, FString::Printf(TEXT("unknown variable '%s'"), *VarName));
			}
			else
			{
				OutputExpr.FindOrAdd(NodeId).Add(TEXT("value"), VarName);
				OutputType.FindOrAdd(NodeId).Add(TEXT("value"), Var->Type);
			}
		}
	}
	else if (Kind == TEXT("math") || Kind == TEXT("bool"))
	{
		FString Op;
		F->TryGetStringField(TEXT("op"), Op);
		static const TMap<FString, FString> MathOps = {{TEXT("add"), TEXT("+")}, {TEXT("sub"), TEXT("-")}, {TEXT("mul"), TEXT("*")}, {TEXT("div"), TEXT("/")}, {TEXT("mod"), TEXT("%")}};
		static const TMap<FString, FString> BoolOps = {{TEXT("and"), TEXT("and")}, {TEXT("or"), TEXT("or")}};
		const bool bUnary = (Kind == TEXT("math") && Op == TEXT("neg")) || (Kind == TEXT("bool") && Op == TEXT("not"));
		const FString A = ResolveValue(F->TryGetField(TEXT("a")), NodeId, Scope);
		if (HasFailed()) { PureNodesInProgress.Remove(NodeId); return; }
		if (bUnary)
		{
			const FString Sym = Kind == TEXT("math") ? TEXT("-") : TEXT("not ");
			OutputExpr.FindOrAdd(NodeId).Add(TEXT("value"), FString::Printf(TEXT("(%s%s)"), *Sym, *A));
		}
		else
		{
			const FString* Sym = Kind == TEXT("math") ? MathOps.Find(Op) : BoolOps.Find(Op);
			if (!Sym)
			{
				Fail(NodeId, FString::Printf(TEXT("unknown %s op '%s'"), *Kind, *Op));
			}
			else
			{
				const FString B = ResolveValue(F->TryGetField(TEXT("b")), NodeId, Scope);
				if (HasFailed()) { PureNodesInProgress.Remove(NodeId); return; }
				OutputExpr.FindOrAdd(NodeId).Add(TEXT("value"), FString::Printf(TEXT("(%s %s %s)"), *A, **Sym, *B));
			}
		}
		OutputType.FindOrAdd(NodeId).Add(TEXT("value"), Kind == TEXT("math") ? TEXT("number") : TEXT("boolean"));
	}
	else if (Kind == TEXT("compare"))
	{
		FString Op;
		F->TryGetStringField(TEXT("op"), Op);
		static const TMap<FString, FString> Ops = {
			{TEXT("eq"), TEXT("==")}, {TEXT("ne"), TEXT("~=")}, {TEXT("lt"), TEXT("<")},
			{TEXT("le"), TEXT("<=")}, {TEXT("gt"), TEXT(">")}, {TEXT("ge"), TEXT(">=")}};
		const FString* Sym = Ops.Find(Op);
		if (!Sym)
		{
			Fail(NodeId, FString::Printf(TEXT("unknown compare op '%s'"), *Op));
		}
		else
		{
			const FString A = ResolveValue(F->TryGetField(TEXT("a")), NodeId, Scope);
			const FString B = HasFailed() ? FString() : ResolveValue(F->TryGetField(TEXT("b")), NodeId, Scope);
			if (HasFailed()) { PureNodesInProgress.Remove(NodeId); return; }
			OutputExpr.FindOrAdd(NodeId).Add(TEXT("value"), FString::Printf(TEXT("(%s %s %s)"), *A, **Sym, *B));
		}
		OutputType.FindOrAdd(NodeId).Add(TEXT("value"), TEXT("boolean"));
	}
	else if (Kind == TEXT("concat"))
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!F->TryGetArrayField(TEXT("values"), Values) || Values->Num() == 0)
		{
			Fail(NodeId, TEXT("concat needs a non-empty \"values\" array"));
		}
		else
		{
			TArray<FString> Parts;
			for (const TSharedPtr<FJsonValue>& V : *Values)
			{
				const FString Expr = ResolveValue(V, NodeId, Scope);
				if (HasFailed()) { PureNodesInProgress.Remove(NodeId); return; }
				Parts.Add(FString::Printf(TEXT("tostring(%s)"), *Expr));
			}
			OutputExpr.FindOrAdd(NodeId).Add(TEXT("value"), TEXT("(") + FString::Join(Parts, TEXT(" .. ")) + TEXT(")"));
		}
		OutputType.FindOrAdd(NodeId).Add(TEXT("value"), TEXT("string"));
	}
	else if (Kind == TEXT("format"))
	{
		FString Text;
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		F->TryGetArrayField(TEXT("values"), Values);
		const int32 ValueCount = Values ? Values->Num() : 0;
		if (!F->TryGetStringField(TEXT("text"), Text))
		{
			Fail(NodeId, TEXT("format needs a \"text\""));
		}
		else
		{
			TArray<FString> Parts;
			int32 ValueIndex = 0;
			int32 SegmentStart = 0;
			for (int32 i = 0; i + 1 < Text.Len(); ++i)
			{
				if (Text[i] == TEXT('{') && Text[i + 1] == TEXT('}'))
				{
					Parts.Add(TEXT("\"") + FMantleGraphCompiler::EscapeLuauString(Text.Mid(SegmentStart, i - SegmentStart)) + TEXT("\""));
					if (!Values || !Values->IsValidIndex(ValueIndex))
					{
						Fail(NodeId, FString::Printf(TEXT("format has more \"{}\" placeholders than values (%d given)"), ValueCount));
						PureNodesInProgress.Remove(NodeId);
						return;
					}
					const FString Expr = ResolveValue((*Values)[ValueIndex], NodeId, Scope);
					if (HasFailed()) { PureNodesInProgress.Remove(NodeId); return; }
					Parts.Add(FString::Printf(TEXT("tostring(%s)"), *Expr));
					++ValueIndex;
					++i;
					SegmentStart = i + 1;
				}
			}
			Parts.Add(TEXT("\"") + FMantleGraphCompiler::EscapeLuauString(Text.Mid(SegmentStart)) + TEXT("\""));
			if (ValueIndex != ValueCount)
			{
				Fail(NodeId, FString::Printf(TEXT("format has %d \"{}\" placeholder(s) but %d value(s)"), ValueIndex, ValueCount));
			}
			else
			{
				OutputExpr.FindOrAdd(NodeId).Add(TEXT("value"), TEXT("(") + FString::Join(Parts, TEXT(" .. ")) + TEXT(")"));
			}
		}
		OutputType.FindOrAdd(NodeId).Add(TEXT("value"), TEXT("string"));
	}
	else if (Kind == TEXT("makevector3"))
	{
		auto Component = [&](const TCHAR* Field) -> FString
		{
			TSharedPtr<FJsonValue> V = F->TryGetField(Field);
			return V.IsValid() ? ResolveValue(V, NodeId, Scope) : TEXT("0");
		};
		const FString X = Component(TEXT("x"));
		const FString Y = HasFailed() ? FString() : Component(TEXT("y"));
		const FString Z = HasFailed() ? FString() : Component(TEXT("z"));
		if (!HasFailed())
		{
			OutputExpr.FindOrAdd(NodeId).Add(TEXT("value"), FString::Printf(TEXT("Vector3.new(%s, %s, %s)"), *X, *Y, *Z));
		}
		OutputType.FindOrAdd(NodeId).Add(TEXT("value"), TEXT("Vector3"));
	}
	else if (Kind == TEXT("positiongoal"))
	{
		// A deliberately narrow typed object constructor, not an arbitrary Luau expression.
		// The fixed key and normal value resolver keep authored text inside the value grammar.
		const FString Position = RequireField(*Node, TEXT("position"), Scope, TEXT("Vector3"));
		if (!HasFailed())
		{
			OutputExpr.FindOrAdd(NodeId).Add(TEXT("value"), FString::Printf(TEXT("{ Position = %s }"), *Position));
			OutputType.FindOrAdd(NodeId).Add(TEXT("value"), TEXT("{Position: Vector3?, Rotation: Vector3?, Scale: Vector3?}"));
		}
	}
	else if (Kind == TEXT("breakvector3"))
	{
		const FString V = RequireField(*Node, TEXT("value"), Scope, TEXT("Vector3"));
		if (!HasFailed())
		{
			const FString Wrapped = FString::Printf(TEXT("(%s)"), *V);
			OutputExpr.FindOrAdd(NodeId).Add(TEXT("x"), Wrapped + TEXT(".X"));
			OutputExpr.FindOrAdd(NodeId).Add(TEXT("y"), Wrapped + TEXT(".Y"));
			OutputExpr.FindOrAdd(NodeId).Add(TEXT("z"), Wrapped + TEXT(".Z"));
			OutputType.FindOrAdd(NodeId).Add(TEXT("x"), TEXT("number"));
			OutputType.FindOrAdd(NodeId).Add(TEXT("y"), TEXT("number"));
			OutputType.FindOrAdd(NodeId).Add(TEXT("z"), TEXT("number"));
		}
	}
	else if (Kind == TEXT("vectormath"))
	{
		FString Op;
		F->TryGetStringField(TEXT("op"), Op);
		static const TMap<FString, FString> Ops = {{TEXT("add"), TEXT("+")}, {TEXT("sub"), TEXT("-")}, {TEXT("scale"), TEXT("*")}};
		const FString* Sym = Ops.Find(Op);
		if (!Sym)
		{
			Fail(NodeId, FString::Printf(TEXT("unknown vectormath op '%s'"), *Op));
		}
		else
		{
			const FString A = RequireField(*Node, TEXT("a"), Scope, TEXT("Vector3"));
			const FString B = HasFailed() ? FString() : ResolveValue(F->TryGetField(TEXT("b")), NodeId, Scope);
			if (!HasFailed())
			{
				OutputExpr.FindOrAdd(NodeId).Add(TEXT("value"), FString::Printf(TEXT("(%s %s %s)"), *A, **Sym, *B));
			}
		}
		OutputType.FindOrAdd(NodeId).Add(TEXT("value"), TEXT("Vector3"));
	}
	else if (Kind == TEXT("reroute"))
	{
		const TSharedPtr<FJsonValue> V = F->TryGetField(TEXT("value"));
		const FString Expr = ResolveValue(V, NodeId, Scope);
		const FString Type = InferType(V, Scope);
		if (!HasFailed())
		{
			OutputExpr.FindOrAdd(NodeId).Add(TEXT("value"), Expr);
			OutputType.FindOrAdd(NodeId).Add(TEXT("value"), Type);
		}
	}
	else if (Kind == TEXT("get"))
	{
		FString Dotted;
		F->TryGetStringField(TEXT("member"), Dotted);
		FString Err;
		const FMantleGraphNodeDef* Def = FMantleGraphCatalogue::FindByDotted(Dotted, EMantleGraphNodeCategory::Get, Err);
		if (!Def)
		{
			Fail(NodeId, Err);
		}
		else
		{
			CheckMemberSide(NodeId, *Def);
			FString Target;
			if (!HasFailed())
			{
				Target = Def->bService ? Def->ClassName : RequireField(*Node, TEXT("target"), Scope, FString());
			}
			if (!HasFailed())
			{
				OutputExpr.FindOrAdd(NodeId).Add(TEXT("value"), FString::Printf(TEXT("%s.%s"), *Target, *Def->MemberName));
				OutputType.FindOrAdd(NodeId).Add(TEXT("value"), Def->Outputs.Num() > 0 ? Def->Outputs[0].Type : FString());
			}
		}
	}
	else
	{
		Fail(NodeId, FString::Printf(TEXT("'%s' is not a pin-producing node kind"), *Kind));
	}

	PureNodesInProgress.Remove(NodeId);
}

FString FMantleGraphCompilerRun::RequireField(const FMantleGraphNode& Node, const FString& FieldName, const TSet<FString>& Scope, const FString& ExpectedType)
{
	TSharedPtr<FJsonValue> V = Node.Fields->TryGetField(FieldName);
	if (!V.IsValid())
	{
		Fail(Node.Id, FString::Printf(TEXT("needs a \"%s\""), *FieldName));
		return FString();
	}
	const FString Expr = ResolveValue(V, Node.Id, Scope);
	if (HasFailed())
	{
		return FString();
	}
	if (!ExpectedType.IsEmpty())
	{
		const FString Actual = InferType(V, Scope);
		if (!MantleGraphTypesCompatible(ExpectedType, Actual))
		{
			Fail(Node.Id, FString::Printf(TEXT("\"%s\" expects %s, got %s (%s)"), *FieldName, *ExpectedType, *Actual, *Expr));
			return FString();
		}
	}
	return Expr;
}

FString FMantleGraphCompilerRun::RequireInput(const TSharedPtr<FJsonObject>& InputsObj, const FString& PinName, const FString& NodeId,
	const FString& Dotted, const TSet<FString>& Scope, const FString& ExpectedType)
{
	TSharedPtr<FJsonValue> V = InputsObj.IsValid() ? InputsObj->TryGetField(PinName) : nullptr;
	if (!V.IsValid())
	{
		Fail(NodeId, FString::Printf(TEXT("%s is missing input '%s'"), *Dotted, *PinName));
		return FString();
	}
	const FString Expr = ResolveValue(V, NodeId, Scope);
	if (HasFailed())
	{
		return FString();
	}
	if (!ExpectedType.IsEmpty())
	{
		const FString Actual = InferType(V, Scope);
		if (!MantleGraphTypesCompatible(ExpectedType, Actual))
		{
			Fail(NodeId, FString::Printf(TEXT("%s input '%s' expects %s, got %s (%s)"), *Dotted, *PinName, *ExpectedType, *Actual, *Expr));
			return FString();
		}
	}
	return Expr;
}

void FMantleGraphCompilerRun::CheckMemberSide(const FString& NodeId, const FMantleGraphNodeDef& Def)
{
	if (Def.Side != EMantleSide::Both && Def.Side != Graph.Side)
	{
		Fail(NodeId, FString::Printf(TEXT("%s only works on the %s; this graph runs on the %s"),
			*Def.Dotted, MantleSideName(Def.Side), MantleSideName(Graph.Side)));
	}
}

FString FMantleGraphCompilerRun::CollectCallArgs(const TArray<FMantleGraphPinDef>& Inputs, bool bVariadic, const TSharedPtr<FJsonObject>& NodeFields,
	const TSharedPtr<FJsonObject>& InputsObj, const FString& NodeId, const FString& Dotted, const TSet<FString>& Scope)
{
	TArray<FString> Args;
	for (const FMantleGraphPinDef& Pin : Inputs)
	{
		TSharedPtr<FJsonValue> V = InputsObj.IsValid() ? InputsObj->TryGetField(Pin.Name) : nullptr;
		if (!V.IsValid())
		{
			if (Pin.bOptional)
			{
				continue; // trailing optional args may simply be omitted, like the Luau call itself
			}
			Fail(NodeId, FString::Printf(TEXT("%s is missing input '%s'"), *Dotted, *Pin.Name));
			return FString();
		}
		const FString Expr = ResolveValue(V, NodeId, Scope);
		if (HasFailed())
		{
			return FString();
		}
		if (!Pin.Type.IsEmpty())
		{
			const FString Actual = InferType(V, Scope);
			if (!MantleGraphTypesCompatible(Pin.Type, Actual))
			{
				Fail(NodeId, FString::Printf(TEXT("%s input '%s' expects %s, got %s (%s)"), *Dotted, *Pin.Name, *Pin.Type, *Actual, *Expr));
				return FString();
			}
		}
		Args.Add(Expr);
	}
	if (bVariadic && NodeFields.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Extra = nullptr;
		if (NodeFields->TryGetArrayField(TEXT("extra"), Extra))
		{
			for (const TSharedPtr<FJsonValue>& V : *Extra)
			{
				const FString Expr = ResolveValue(V, NodeId, Scope);
				if (HasFailed())
				{
					return FString();
				}
				Args.Add(Expr);
			}
		}
	}
	return FString::Join(Args, TEXT(", "));
}

// -------------------------------------------------------------------------------------------
// Structural passes

void FMantleGraphCompilerRun::BuildExecTargets()
{
	for (const FMantleGraphExecWire& Wire : Graph.Exec)
	{
		if (!NodesById.Contains(Wire.From))
		{
			FailGraph(FString::Printf(TEXT("exec wire from '%s' references a missing node"), *Wire.From));
			return;
		}
		if (!NodesById.Contains(Wire.To))
		{
			Fail(Wire.From, FString::Printf(TEXT("exec wire to '%s' references a missing node"), *Wire.To));
			return;
		}
		TMap<FString, FString>& Ports = ExecTargets.FindOrAdd(Wire.From);
		if (Ports.Contains(Wire.Port))
		{
			Fail(Wire.From, FString::Printf(TEXT("has two exec wires from its '%s' pin"), *Wire.Port));
			return;
		}
		Ports.Add(Wire.Port, Wire.To);
	}
}

void FMantleGraphCompilerRun::CheckExecCycles()
{
	enum class EState : uint8 { White, Gray, Black };
	TMap<FString, EState> State;
	for (const FMantleGraphNode& Node : Graph.Nodes)
	{
		State.Add(Node.Id, EState::White);
	}

	TFunction<bool(const FString&)> Visit = [&](const FString& NodeId) -> bool
	{
		State.FindOrAdd(NodeId) = EState::Gray;
		const TMap<FString, FString>* Ports = ExecTargets.Find(NodeId);
		if (Ports)
		{
			TArray<FString> SortedPorts;
			Ports->GetKeys(SortedPorts);
			SortedPorts.Sort();
			for (const FString& Port : SortedPorts)
			{
				const FString Target = Ports->FindRef(Port);
				const EState TargetState = State.FindRef(Target);
				if (TargetState == EState::Gray)
				{
					Fail(Target, TEXT("closes an exec cycle; loops must use a For Loop/For Each/While node's body pin, not a wire back to an earlier node"));
					return false;
				}
				if (TargetState == EState::White && !Visit(Target))
				{
					return false;
				}
			}
		}
		State.FindOrAdd(NodeId) = EState::Black;
		return true;
	};

	TArray<FString> SortedIds;
	State.GetKeys(SortedIds);
	SortedIds.Sort();
	for (const FString& Id : SortedIds)
	{
		if (State.FindRef(Id) == EState::White && !Visit(Id))
		{
			return;
		}
	}
}

void FMantleGraphCompilerRun::CheckCustomEventsAndFunctionsHaveRoots()
{
	for (const FMantleGraphCustomEvent& Event : Graph.CustomEvents)
	{
		int32 Count = 0;
		for (const FMantleGraphNode& Node : Graph.Nodes)
		{
			FString EventName;
			if (Node.Kind == TEXT("onevent") && Node.Fields->TryGetStringField(TEXT("event"), EventName) && EventName == Event.Name)
			{
				++Count;
			}
		}
		if (Count == 0)
		{
			FailGraph(FString::Printf(TEXT("customEvent '%s' has no \"onevent\" node handling it"), *Event.Name));
			return;
		}
		if (Count > 1)
		{
			FailGraph(FString::Printf(TEXT("customEvent '%s' has more than one \"onevent\" node handling it"), *Event.Name));
			return;
		}
	}
	for (const FMantleGraphFunction& Fn : Graph.Functions)
	{
		int32 Count = 0;
		for (const FMantleGraphNode& Node : Graph.Nodes)
		{
			FString FnName;
			if (Node.Kind == TEXT("function") && Node.Fields->TryGetStringField(TEXT("function"), FnName) && FnName == Fn.Name)
			{
				++Count;
			}
		}
		if (Count == 0)
		{
			FailGraph(FString::Printf(TEXT("function '%s' has no \"function\" root node"), *Fn.Name));
			return;
		}
		if (Count > 1)
		{
			FailGraph(FString::Printf(TEXT("function '%s' has more than one \"function\" root node"), *Fn.Name));
			return;
		}
	}
}

void FMantleGraphCompilerRun::ComputeReferencedNodeIds()
{
	TFunction<void(const TSharedPtr<FJsonValue>&)> Walk = [&](const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return;
		}
		if (Value->Type == EJson::String)
		{
			FString AsString;
			Value->TryGetString(AsString);
			if (AsString.StartsWith(TEXT("@")))
			{
				FString Ref = AsString.Mid(1);
				FString NodeId, Discard;
				if (!Ref.Split(TEXT("."), &NodeId, &Discard))
				{
					NodeId = Ref;
				}
				ReferencedNodeIds.Add(NodeId);
			}
		}
		else if (Value->Type == EJson::Array)
		{
			for (const TSharedPtr<FJsonValue>& Element : Value->AsArray())
			{
				Walk(Element);
			}
		}
		else if (Value->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject>* AsObject = nullptr;
			if (Value->TryGetObject(AsObject) && AsObject->IsValid())
			{
				for (const auto& Field : (*AsObject)->Values)
				{
					Walk(Field.Value);
				}
			}
		}
	};

	for (const FMantleGraphNode& Node : Graph.Nodes)
	{
		if (!Node.Fields.IsValid())
		{
			continue;
		}
		for (const auto& Field : Node.Fields->Values)
		{
			Walk(Field.Value);
		}
	}
}

// -------------------------------------------------------------------------------------------
// Node compilation

void FMantleGraphCompilerRun::EmitExecChain(const FString& NodeId, TSet<FString> Scope, int32 IndentLevel, const FMantleGraphFunction* CurrentFunction)
{
	if (HasFailed())
	{
		return;
	}
	const FMantleGraphNode* NodePtr = NodesById.FindRef(NodeId);
	if (!NodePtr)
	{
		FailGraph(FString::Printf(TEXT("exec wire references missing node '%s'"), *NodeId));
		return;
	}
	const FMantleGraphNode& Node = *NodePtr;
	Scope.Add(NodeId);

	if (MantleGraphIsPureKind(Node.Kind) || Node.Kind == TEXT("comment"))
	{
		Fail(NodeId, FString::Printf(TEXT("'%s' is a pure node and can't be wired into exec flow"), *Node.Kind));
		return;
	}

	if (Node.Kind == TEXT("event"))
	{
		EmitEventNode(Node, Scope, IndentLevel, CurrentFunction);
		return;
	}
	if (Node.Kind == TEXT("branch"))
	{
		EmitBranchNode(Node, Scope, IndentLevel, CurrentFunction);
		return;
	}
	if (Node.Kind == TEXT("sequence"))
	{
		EmitSequenceNode(Node, Scope, IndentLevel, CurrentFunction);
		return;
	}
	if (Node.Kind == TEXT("forloop"))
	{
		EmitForLoopNode(Node, Scope, IndentLevel, CurrentFunction);
		return;
	}
	if (Node.Kind == TEXT("foreach"))
	{
		EmitForEachNode(Node, Scope, IndentLevel, CurrentFunction);
		return;
	}
	if (Node.Kind == TEXT("while"))
	{
		EmitWhileNode(Node, Scope, IndentLevel, CurrentFunction);
		return;
	}
	if (Node.Kind == TEXT("doonce"))
	{
		EmitDoOnceNode(Node, Scope, IndentLevel, CurrentFunction);
		return;
	}
	if (Node.Kind == TEXT("return"))
	{
		EmitReturnNode(Node, Scope, IndentLevel, CurrentFunction);
		return;
	}
	if (Node.Kind == TEXT("call"))
	{
		// EmitCallNode manages its own "next" continuation: an optional-typed return (e.g.
		// World.FindFirst's Entity?) wraps the rest of the chain in a nil guard instead of
		// falling through unconditionally, so it can't use the shared tail below.
		EmitCallNode(Node, Scope, IndentLevel, CurrentFunction);
		return;
	}

	// Simple, single-successor kinds: emit, then fall through to the "next" port.
	if (Node.Kind == TEXT("set")) EmitSetNode(Node, Scope, IndentLevel);
	else if (Node.Kind == TEXT("print")) EmitPrintNode(Node, Scope, IndentLevel, false);
	else if (Node.Kind == TEXT("warn")) EmitPrintNode(Node, Scope, IndentLevel, true);
	else if (Node.Kind == TEXT("delay")) EmitDelayNode(Node, Scope, IndentLevel);
	else if (Node.Kind == TEXT("setvar")) EmitSetVarNode(Node, Scope, IndentLevel);
	else if (Node.Kind == TEXT("callevent")) EmitCallEventNode(Node, Scope, IndentLevel);
	else if (Node.Kind == TEXT("callfunction")) EmitCallFunctionNode(Node, Scope, IndentLevel);
	else if (Node.Kind == TEXT("start") || Node.Kind == TEXT("onevent") || Node.Kind == TEXT("function"))
	{
		// These are only ever roots, compiled by Run(); reaching one here means something else's
		// exec wire targets it, which is not a supported shape.
		Fail(NodeId, FString::Printf(TEXT("'%s' must be a top-level root, not the target of another node's exec wire"), *Node.Kind));
		return;
	}
	else
	{
		Fail(NodeId, FString::Printf(TEXT("unknown node kind '%s'"), *Node.Kind));
		return;
	}

	if (HasFailed())
	{
		return;
	}
	if (const FString* Next = FindExecTarget(NodeId, TEXT("next")))
	{
		EmitExecChain(*Next, Scope, IndentLevel, CurrentFunction);
	}
}

void FMantleGraphCompilerRun::EmitEventNode(const FMantleGraphNode& Node, TSet<FString> Scope, int32 IndentLevel, const FMantleGraphFunction* CurrentFunction)
{
	const FString& NodeId = Node.Id;
	FString Dotted;
	if (!Node.Fields->TryGetStringField(TEXT("member"), Dotted))
	{
		Fail(NodeId, TEXT("event node needs a \"member\""));
		return;
	}
	FString Err;
	const FMantleGraphNodeDef* Def = FMantleGraphCatalogue::FindByDotted(Dotted, EMantleGraphNodeCategory::Event, Err);
	if (!Def)
	{
		Fail(NodeId, Err);
		return;
	}
	CheckMemberSide(NodeId, *Def);
	if (HasFailed()) return;

	FString Target = Def->bService ? Def->ClassName : RequireField(Node, TEXT("target"), Scope, FString());
	if (HasFailed()) return;

	const TSharedPtr<FJsonObject>* InputsObj = nullptr;
	Node.Fields->TryGetObjectField(TEXT("inputs"), InputsObj);
	const FString Args = CollectCallArgs(Def->Inputs, Def->bVariadic, Node.Fields, InputsObj ? *InputsObj : nullptr, NodeId, Dotted, Scope);
	if (HasFailed()) return;

	// Output pin names: prefer the registry's own handler signature; fall back to the graph's
	// own "outputs" list when the registry couldn't name them (a fully variadic handler, e.g.
	// Signal:Connect's "(...any) -> ()").
	TArray<FString> OutputNames;
	for (const FMantleGraphPinDef& Pin : Def->Outputs)
	{
		OutputNames.Add(Pin.Name);
	}
	if (OutputNames.Num() == 0)
	{
		const TArray<TSharedPtr<FJsonValue>>* Declared = nullptr;
		if (Node.Fields->TryGetArrayField(TEXT("outputs"), Declared))
		{
			for (const TSharedPtr<FJsonValue>& V : *Declared)
			{
				FString Name;
				if (V->TryGetString(Name)) OutputNames.Add(Name);
			}
		}
	}

	TArray<FString> Params;
	TMap<FString, FString>& Exprs = OutputExpr.FindOrAdd(NodeId);
	TMap<FString, FString>& Types = OutputType.FindOrAdd(NodeId);
	for (int32 i = 0; i < OutputNames.Num(); ++i)
	{
		const FString Local = NodeLocal(NodeId, OutputNames[i]);
		Params.Add(Local);
		Exprs.Add(OutputNames[i], Local);
		Types.Add(OutputNames[i], Def->Outputs.IsValidIndex(i) ? Def->Outputs[i].Type : FString());
	}
	const FString ParamsText = FString::Join(Params, TEXT(", "));

	if (Def->bIsSignal)
	{
		EmitLine(NodeId, FString::Printf(TEXT("%s%s.%s:Connect(function(%s)"), *Indent(IndentLevel), *Target, *Def->MemberName, *ParamsText));
	}
	else
	{
		const FString ArgsWithComma = Args.IsEmpty() ? FString() : Args + TEXT(", ");
		EmitLine(NodeId, FString::Printf(TEXT("%s%s:%s(%sfunction(%s)"), *Indent(IndentLevel), *Target, *Def->MemberName, *ArgsWithComma, *ParamsText));
	}

	if (const FString* Body = FindExecTarget(NodeId, TEXT("next")))
	{
		EmitExecChain(*Body, Scope, IndentLevel + 1, CurrentFunction);
	}
	else
	{
		EmitLine(NodeId, Indent(IndentLevel + 1) + TEXT("-- (nothing wired)"));
	}
	if (HasFailed()) return;
	EmitLine(NodeId, Indent(IndentLevel) + TEXT("end)"));
}

void FMantleGraphCompilerRun::EmitCallNode(const FMantleGraphNode& Node, TSet<FString> Scope, int32 IndentLevel, const FMantleGraphFunction* CurrentFunction)
{
	const FString& NodeId = Node.Id;
	FString Dotted;
	if (!Node.Fields->TryGetStringField(TEXT("member"), Dotted))
	{
		Fail(NodeId, TEXT("call node needs a \"member\""));
		return;
	}
	FString Err;
	const FMantleGraphNodeDef* Def = FMantleGraphCatalogue::FindByDotted(Dotted, EMantleGraphNodeCategory::Call, Err);
	if (!Def)
	{
		Fail(NodeId, Err);
		return;
	}
	CheckMemberSide(NodeId, *Def);
	if (HasFailed()) return;

	const FString Target = Def->bService ? Def->ClassName : RequireField(Node, TEXT("target"), Scope, FString());
	if (HasFailed()) return;

	const TSharedPtr<FJsonObject>* InputsObj = nullptr;
	Node.Fields->TryGetObjectField(TEXT("inputs"), InputsObj);
	const FString Args = CollectCallArgs(Def->Inputs, Def->bVariadic, Node.Fields, InputsObj ? *InputsObj : nullptr, NodeId, Dotted, Scope);
	if (HasFailed()) return;

	// An optional registry return (Entity?, Player?, ...) can't be handed to anything downstream
	// as-is -- Luau strict mode rejects using a possibly-nil value where the non-optional type is
	// expected (World:FindFirst("door").Position, e.g.). Guard it here rather than at every use
	// site: wrap the rest of this node's own exec chain in `if <local> then ... end`, so nothing
	// after this node ever runs with a nil value in hand. A creator whose lookup finds nothing
	// gets nothing happening, matching the ticket's read on this ("no crash").
	const bool bOptionalOutput = Def->Outputs.Num() > 0 && Def->Outputs[0].Type.EndsWith(TEXT("?"));
	// A non-optional return is only worth a local when something downstream actually reads it
	// (ReferencedNodeIds, from ComputeReferencedNodeIds's pass over every "@id"/"@id.output" in
	// the graph): an unconsumed `local x = ...` is dead-code noise a creator may well read, and
	// trips luau-lsp's unused-variable diagnostic under the strict mode generated graphs are
	// type-checked in. An *optional* return is different -- its local is the nil-guard condition
	// below, needed for correctness regardless of whether the value itself is ever read, so it
	// always gets one.
	const bool bBindLocal = Def->Outputs.Num() > 0 && (bOptionalOutput || ReferencedNodeIds.Contains(NodeId));

	if (bBindLocal)
	{
		const FString Local = NodeLocal(NodeId, TEXT("value"));
		OutputExpr.FindOrAdd(NodeId).Add(TEXT("value"), Local);
		OutputType.FindOrAdd(NodeId).Add(TEXT("value"), Def->Outputs[0].Type);
		EmitLine(NodeId, FString::Printf(TEXT("%slocal %s = %s:%s(%s)"), *Indent(IndentLevel), *Local, *Target, *Def->MemberName, *Args));

		if (bOptionalOutput)
		{
			EmitLine(NodeId, FString::Printf(TEXT("%sif %s then"), *Indent(IndentLevel), *Local));
			if (const FString* Next = FindExecTarget(NodeId, TEXT("next")))
			{
				EmitExecChain(*Next, Scope, IndentLevel + 1, CurrentFunction);
			}
			if (HasFailed()) return;
			EmitLine(NodeId, Indent(IndentLevel) + TEXT("end"));
			return;
		}
	}
	else
	{
		EmitLine(NodeId, FString::Printf(TEXT("%s%s:%s(%s)"), *Indent(IndentLevel), *Target, *Def->MemberName, *Args));
	}

	if (const FString* Next = FindExecTarget(NodeId, TEXT("next")))
	{
		EmitExecChain(*Next, Scope, IndentLevel, CurrentFunction);
	}
}

void FMantleGraphCompilerRun::EmitSetNode(const FMantleGraphNode& Node, const TSet<FString>& Scope, int32 IndentLevel)
{
	const FString& NodeId = Node.Id;
	FString Dotted;
	if (!Node.Fields->TryGetStringField(TEXT("member"), Dotted))
	{
		Fail(NodeId, TEXT("set node needs a \"member\""));
		return;
	}
	FString Err;
	const FMantleGraphNodeDef* Def = FMantleGraphCatalogue::FindByDotted(Dotted, EMantleGraphNodeCategory::Set, Err);
	if (!Def)
	{
		Fail(NodeId, Err);
		return;
	}
	CheckMemberSide(NodeId, *Def);
	if (HasFailed()) return;

	const FString Target = Def->bService ? Def->ClassName : RequireField(Node, TEXT("target"), Scope, FString());
	if (HasFailed()) return;
	const FString Value = RequireField(Node, TEXT("value"), Scope, Def->Inputs.Num() > 0 ? Def->Inputs[0].Type : FString());
	if (HasFailed()) return;

	EmitLine(NodeId, FString::Printf(TEXT("%s%s.%s = %s"), *Indent(IndentLevel), *Target, *Def->MemberName, *Value));
}

void FMantleGraphCompilerRun::EmitPrintNode(const FMantleGraphNode& Node, const TSet<FString>& Scope, int32 IndentLevel, bool bWarn)
{
	const FString Text = RequireField(Node, TEXT("text"), Scope, FString());
	if (HasFailed()) return;
	EmitLine(Node.Id, FString::Printf(TEXT("%s%s(tostring(%s))"), *Indent(IndentLevel), bWarn ? TEXT("warn") : TEXT("print"), *Text));
}

void FMantleGraphCompilerRun::EmitDelayNode(const FMantleGraphNode& Node, const TSet<FString>& Scope, int32 IndentLevel)
{
	const FString Seconds = RequireField(Node, TEXT("seconds"), Scope, TEXT("number"));
	if (HasFailed()) return;
	EmitLine(Node.Id, FString::Printf(TEXT("%stask.wait(%s)"), *Indent(IndentLevel), *Seconds));
}

void FMantleGraphCompilerRun::EmitSetVarNode(const FMantleGraphNode& Node, const TSet<FString>& Scope, int32 IndentLevel)
{
	const FString& NodeId = Node.Id;
	FString VarName;
	if (!Node.Fields->TryGetStringField(TEXT("variable"), VarName))
	{
		Fail(NodeId, TEXT("setvar needs a \"variable\""));
		return;
	}
	const FMantleGraphVariable* Var = Graph.FindVariable(VarName);
	if (!Var)
	{
		Fail(NodeId, FString::Printf(TEXT("unknown variable '%s'"), *VarName));
		return;
	}
	const FString Value = RequireField(Node, TEXT("value"), Scope, Var->Type);
	if (HasFailed()) return;
	EmitLine(NodeId, FString::Printf(TEXT("%s%s = %s"), *Indent(IndentLevel), *VarName, *Value));
}

void FMantleGraphCompilerRun::EmitCallEventNode(const FMantleGraphNode& Node, const TSet<FString>& Scope, int32 IndentLevel)
{
	const FString& NodeId = Node.Id;
	FString EventName;
	if (!Node.Fields->TryGetStringField(TEXT("event"), EventName))
	{
		Fail(NodeId, TEXT("callevent needs an \"event\""));
		return;
	}
	const FMantleGraphCustomEvent* Event = Graph.FindCustomEvent(EventName);
	if (!Event)
	{
		Fail(NodeId, FString::Printf(TEXT("unknown custom event '%s'"), *EventName));
		return;
	}
	const TSharedPtr<FJsonObject>* InputsObj = nullptr;
	Node.Fields->TryGetObjectField(TEXT("inputs"), InputsObj);
	TArray<FString> Args;
	for (const FMantleGraphParam& Param : Event->Params)
	{
		const FString Expr = RequireInput(InputsObj ? *InputsObj : nullptr, Param.Name, NodeId,
			FString::Printf(TEXT("event '%s'"), *EventName), Scope, Param.Type);
		if (HasFailed()) return;
		Args.Add(Expr);
	}
	EmitLine(NodeId, FString::Printf(TEXT("%sOn%s(%s)"), *Indent(IndentLevel), *EventName, *FString::Join(Args, TEXT(", "))));
}

void FMantleGraphCompilerRun::EmitCallFunctionNode(const FMantleGraphNode& Node, const TSet<FString>& Scope, int32 IndentLevel)
{
	const FString& NodeId = Node.Id;
	FString FnName;
	if (!Node.Fields->TryGetStringField(TEXT("function"), FnName))
	{
		Fail(NodeId, TEXT("callfunction needs a \"function\""));
		return;
	}
	const FMantleGraphFunction* Fn = Graph.FindFunction(FnName);
	if (!Fn)
	{
		Fail(NodeId, FString::Printf(TEXT("unknown function '%s'"), *FnName));
		return;
	}
	const TSharedPtr<FJsonObject>* InputsObj = nullptr;
	Node.Fields->TryGetObjectField(TEXT("inputs"), InputsObj);
	TArray<FString> Args;
	for (const FMantleGraphParam& Param : Fn->Params)
	{
		const FString Expr = RequireInput(InputsObj ? *InputsObj : nullptr, Param.Name, NodeId,
			FString::Printf(TEXT("function '%s'"), *FnName), Scope, Param.Type);
		if (HasFailed()) return;
		Args.Add(Expr);
	}
	const FString Call = FString::Printf(TEXT("%s(%s)"), *FnName, *FString::Join(Args, TEXT(", ")));

	if (Fn->Returns.Num() > 0)
	{
		TArray<FString> Locals;
		TMap<FString, FString>& Exprs = OutputExpr.FindOrAdd(NodeId);
		TMap<FString, FString>& Types = OutputType.FindOrAdd(NodeId);
		for (const FMantleGraphParam& Ret : Fn->Returns)
		{
			const FString Local = NodeLocal(NodeId, Ret.Name);
			Locals.Add(Local);
			Exprs.Add(Ret.Name, Local);
			Types.Add(Ret.Name, Ret.Type);
		}
		EmitLine(NodeId, FString::Printf(TEXT("%slocal %s = %s"), *Indent(IndentLevel), *FString::Join(Locals, TEXT(", ")), *Call));
	}
	else
	{
		EmitLine(NodeId, Indent(IndentLevel) + Call);
	}
}

void FMantleGraphCompilerRun::EmitBranchNode(const FMantleGraphNode& Node, TSet<FString> Scope, int32 IndentLevel, const FMantleGraphFunction* CurrentFunction)
{
	const FString& NodeId = Node.Id;
	const FString Cond = RequireField(Node, TEXT("condition"), Scope, TEXT("boolean"));
	if (HasFailed()) return;

	EmitLine(NodeId, FString::Printf(TEXT("%sif %s then"), *Indent(IndentLevel), *Cond));
	if (const FString* TrueTarget = FindExecTarget(NodeId, TEXT("true")))
	{
		EmitExecChain(*TrueTarget, Scope, IndentLevel + 1, CurrentFunction);
	}
	else
	{
		EmitLine(NodeId, Indent(IndentLevel + 1) + TEXT("-- (nothing wired)"));
	}
	if (HasFailed()) return;
	if (const FString* FalseTarget = FindExecTarget(NodeId, TEXT("false")))
	{
		EmitLine(NodeId, Indent(IndentLevel) + TEXT("else"));
		EmitExecChain(*FalseTarget, Scope, IndentLevel + 1, CurrentFunction);
		if (HasFailed()) return;
	}
	EmitLine(NodeId, Indent(IndentLevel) + TEXT("end"));
}

void FMantleGraphCompilerRun::EmitSequenceNode(const FMantleGraphNode& Node, TSet<FString> Scope, int32 IndentLevel, const FMantleGraphFunction* CurrentFunction)
{
	const FString& NodeId = Node.Id;
	const TMap<FString, FString>* Ports = ExecTargets.Find(NodeId);
	if (!Ports || Ports->Num() == 0)
	{
		Fail(NodeId, TEXT("sequence has no wired outputs"));
		return;
	}
	TArray<int32> Indices;
	for (const TPair<FString, FString>& Pair : *Ports)
	{
		int32 Index = INDEX_NONE;
		if (Pair.Key.IsNumeric())
		{
			Index = FCString::Atoi(*Pair.Key);
		}
		else
		{
			Fail(NodeId, FString::Printf(TEXT("sequence pin '%s' must be numeric (\"0\", \"1\", ...)"), *Pair.Key));
			return;
		}
		Indices.Add(Index);
	}
	Indices.Sort();
	for (const int32 Index : Indices)
	{
		EmitExecChain(Ports->FindRef(FString::FromInt(Index)), Scope, IndentLevel, CurrentFunction);
		if (HasFailed()) return;
	}
}

void FMantleGraphCompilerRun::EmitForLoopNode(const FMantleGraphNode& Node, TSet<FString> Scope, int32 IndentLevel, const FMantleGraphFunction* CurrentFunction)
{
	const FString& NodeId = Node.Id;
	const FString First = RequireField(Node, TEXT("first"), Scope, TEXT("number"));
	if (HasFailed()) return;
	const FString Last = RequireField(Node, TEXT("last"), Scope, TEXT("number"));
	if (HasFailed()) return;
	TSharedPtr<FJsonValue> StepValue = Node.Fields->TryGetField(TEXT("step"));
	const FString Step = StepValue.IsValid() ? ResolveValue(StepValue, NodeId, Scope) : TEXT("1");
	if (HasFailed()) return;

	const FString IndexLocal = NodeLocal(NodeId, TEXT("index"));
	OutputExpr.FindOrAdd(NodeId).Add(TEXT("index"), IndexLocal);
	OutputType.FindOrAdd(NodeId).Add(TEXT("index"), TEXT("number"));

	EmitLine(NodeId, FString::Printf(TEXT("%sfor %s = %s, %s, %s do"), *Indent(IndentLevel), *IndexLocal, *First, *Last, *Step));
	if (const FString* Body = FindExecTarget(NodeId, TEXT("body")))
	{
		EmitExecChain(*Body, Scope, IndentLevel + 1, CurrentFunction);
	}
	else
	{
		EmitLine(NodeId, Indent(IndentLevel + 1) + TEXT("-- (nothing wired)"));
	}
	if (HasFailed()) return;
	EmitLine(NodeId, Indent(IndentLevel) + TEXT("end"));

	if (const FString* Done = FindExecTarget(NodeId, TEXT("done")))
	{
		EmitExecChain(*Done, Scope, IndentLevel, CurrentFunction);
	}
}

void FMantleGraphCompilerRun::EmitForEachNode(const FMantleGraphNode& Node, TSet<FString> Scope, int32 IndentLevel, const FMantleGraphFunction* CurrentFunction)
{
	const FString& NodeId = Node.Id;
	const FString Array = RequireField(Node, TEXT("array"), Scope, FString());
	if (HasFailed()) return;

	const FString IndexLocal = NodeLocal(NodeId, TEXT("index"));
	const FString ValueLocal = NodeLocal(NodeId, TEXT("value"));
	OutputExpr.FindOrAdd(NodeId).Add(TEXT("index"), IndexLocal);
	OutputExpr.FindOrAdd(NodeId).Add(TEXT("value"), ValueLocal);
	OutputType.FindOrAdd(NodeId).Add(TEXT("index"), TEXT("number"));
	OutputType.FindOrAdd(NodeId).Add(TEXT("value"), FString());

	EmitLine(NodeId, FString::Printf(TEXT("%sfor %s, %s in ipairs(%s) do"), *Indent(IndentLevel), *IndexLocal, *ValueLocal, *Array));
	if (const FString* Body = FindExecTarget(NodeId, TEXT("body")))
	{
		EmitExecChain(*Body, Scope, IndentLevel + 1, CurrentFunction);
	}
	else
	{
		EmitLine(NodeId, Indent(IndentLevel + 1) + TEXT("-- (nothing wired)"));
	}
	if (HasFailed()) return;
	EmitLine(NodeId, Indent(IndentLevel) + TEXT("end"));

	if (const FString* Done = FindExecTarget(NodeId, TEXT("done")))
	{
		EmitExecChain(*Done, Scope, IndentLevel, CurrentFunction);
	}
}

void FMantleGraphCompilerRun::EmitWhileNode(const FMantleGraphNode& Node, TSet<FString> Scope, int32 IndentLevel, const FMantleGraphFunction* CurrentFunction)
{
	const FString& NodeId = Node.Id;
	const FString Cond = RequireField(Node, TEXT("condition"), Scope, TEXT("boolean"));
	if (HasFailed()) return;

	EmitLine(NodeId, FString::Printf(TEXT("%swhile %s do"), *Indent(IndentLevel), *Cond));
	if (const FString* Body = FindExecTarget(NodeId, TEXT("body")))
	{
		EmitExecChain(*Body, Scope, IndentLevel + 1, CurrentFunction);
	}
	else
	{
		EmitLine(NodeId, Indent(IndentLevel + 1) + TEXT("-- (nothing wired)"));
	}
	if (HasFailed()) return;
	EmitLine(NodeId, Indent(IndentLevel) + TEXT("end"));

	if (const FString* Done = FindExecTarget(NodeId, TEXT("done")))
	{
		EmitExecChain(*Done, Scope, IndentLevel, CurrentFunction);
	}
}

void FMantleGraphCompilerRun::EmitDoOnceNode(const FMantleGraphNode& Node, TSet<FString> Scope, int32 IndentLevel, const FMantleGraphFunction* CurrentFunction)
{
	const FString& NodeId = Node.Id;
	const FString Flag = NodeLocal(NodeId, TEXT("once"));
	EmitLine(NodeId, FString::Printf(TEXT("%sif not %s then"), *Indent(IndentLevel), *Flag));
	EmitLine(NodeId, FString::Printf(TEXT("%s%s = true"), *Indent(IndentLevel + 1), *Flag));
	if (const FString* Next = FindExecTarget(NodeId, TEXT("next")))
	{
		EmitExecChain(*Next, Scope, IndentLevel + 1, CurrentFunction);
	}
	if (HasFailed()) return;
	EmitLine(NodeId, Indent(IndentLevel) + TEXT("end"));
}

void FMantleGraphCompilerRun::EmitReturnNode(const FMantleGraphNode& Node, const TSet<FString>& Scope, int32 IndentLevel, const FMantleGraphFunction* CurrentFunction)
{
	const FString& NodeId = Node.Id;
	if (!CurrentFunction)
	{
		Fail(NodeId, TEXT("return is only valid inside a function"));
		return;
	}
	const TSharedPtr<FJsonObject>* ValuesObj = nullptr;
	Node.Fields->TryGetObjectField(TEXT("values"), ValuesObj);
	TArray<FString> Exprs;
	for (const FMantleGraphParam& Ret : CurrentFunction->Returns)
	{
		const FString Expr = RequireInput(ValuesObj ? *ValuesObj : nullptr, Ret.Name, NodeId, FString::Printf(TEXT("function '%s'"), *CurrentFunction->Name), Scope, Ret.Type);
		if (HasFailed()) return;
		Exprs.Add(Expr);
	}
	EmitLine(NodeId, Indent(IndentLevel) + (Exprs.Num() > 0 ? TEXT("return ") + FString::Join(Exprs, TEXT(", ")) : FString(TEXT("return"))));
}

// -------------------------------------------------------------------------------------------
// Top-level orchestration

FMantleGraphCompileResult FMantleGraphCompilerRun::Run()
{
    // Authored bindings can use the same spelling as a generated name. Reserve every
    // declared scope before allocation, including parameters that could shadow flags.
    for (const FMantleGraphVariable& Var : Graph.Variables) UsedLocalNames.Add(Var.Name);
    for (const FMantleGraphCustomEvent& Event : Graph.CustomEvents)
    {
        UsedLocalNames.Add(TEXT("On") + Event.Name);
        for (const FMantleGraphParam& Param : Event.Params) UsedLocalNames.Add(Param.Name);
    }
    for (const FMantleGraphFunction& Fn : Graph.Functions)
    {
        UsedLocalNames.Add(Fn.Name);
        for (const FMantleGraphParam& Param : Fn.Params) UsedLocalNames.Add(Param.Name);
    }

	for (const FMantleGraphNode& Node : Graph.Nodes)
	{
		if (NodesById.Contains(Node.Id))
		{
			FailGraph(FString::Printf(TEXT("duplicate node id '%s'"), *Node.Id));
			return Result;
		}
		NodesById.Add(Node.Id, &Node);
	}

	BuildExecTargets();
	if (HasFailed()) return Result;
	CheckExecCycles();
	if (HasFailed()) return Result;
	CheckCustomEventsAndFunctionsHaveRoots();
	if (HasFailed()) return Result;
	ComputeReferencedNodeIds();

	// Targets of some wire's "next"/"true"/"false"/"body"/"done"/numeric port are never
	// themselves top-level roots; "start"/"event" nodes that nobody points at are.
	TSet<FString> ExecTargetNodeIds;
	for (const FMantleGraphExecWire& Wire : Graph.Exec)
	{
		ExecTargetNodeIds.Add(Wire.To);
	}

	EmitLine(FString(), FString::Printf(TEXT("-- GENERATED from %s.graph.json by MantleGraphCompiler; edit the graph, not this file."), *Graph.Name));
	EmitLine(FString(), FString());

	// doonce flags need a persistent local, declared once, outside any handler.
	for (const FMantleGraphNode& Node : Graph.Nodes)
	{
		if (Node.Kind == TEXT("doonce"))
		{
			EmitLine(FString(), FString::Printf(TEXT("local %s = false"), *NodeLocal(Node.Id, TEXT("once"))));
		}
	}

	// Script variables, in declaration order.
	for (const FMantleGraphVariable& Var : Graph.Variables)
	{
		FString DefaultExpr;
		if (Var.Default.IsValid())
		{
			DefaultExpr = ResolveValue(Var.Default, FString::Printf(TEXT("variable '%s'"), *Var.Name), TSet<FString>());
			if (HasFailed()) return Result;
		}
		else if (Var.Type == TEXT("bool") || Var.Type == TEXT("boolean")) DefaultExpr = TEXT("false");
		else if (Var.Type == TEXT("number")) DefaultExpr = TEXT("0");
		else if (Var.Type == TEXT("string")) DefaultExpr = TEXT("\"\"");
		else if (Var.Type == TEXT("list")) DefaultExpr = TEXT("{}");
		else if (Var.Type == TEXT("map")) DefaultExpr = TEXT("{}");
		else DefaultExpr = TEXT("nil");
		EmitLine(FString(), FString::Printf(TEXT("local %s = %s"), *Var.Name, *DefaultExpr));
	}

	// Custom event handlers and functions: predeclare every local first so they can call each
	// other regardless of declaration order, then fill each in.
	if (Graph.CustomEvents.Num() > 0 || Graph.Functions.Num() > 0)
	{
		TArray<FString> Locals;
		for (const FMantleGraphCustomEvent& Event : Graph.CustomEvents) Locals.Add(TEXT("On") + Event.Name);
		for (const FMantleGraphFunction& Fn : Graph.Functions) Locals.Add(Fn.Name);
		EmitLine(FString(), TEXT("local ") + FString::Join(Locals, TEXT(", ")));
	}

	auto FindRootNode = [&](const FString& Kind, const FString& FieldName, const FString& Name) -> const FMantleGraphNode*
	{
		for (const FMantleGraphNode& Node : Graph.Nodes)
		{
			FString Value;
			if (Node.Kind == Kind && Node.Fields->TryGetStringField(FieldName, Value) && Value == Name)
			{
				return &Node;
			}
		}
		return nullptr;
	};

	for (const FMantleGraphCustomEvent& Event : Graph.CustomEvents)
	{
		const FMantleGraphNode* Root = FindRootNode(TEXT("onevent"), TEXT("event"), Event.Name);
		check(Root); // guaranteed by CheckCustomEventsAndFunctionsHaveRoots
		TSet<FString> Scope{Root->Id};
		TArray<FString> Params;
		TMap<FString, FString>& Exprs = OutputExpr.FindOrAdd(Root->Id);
		TMap<FString, FString>& Types = OutputType.FindOrAdd(Root->Id);
		for (const FMantleGraphParam& Param : Event.Params)
		{
			Params.Add(Param.Name);
			Exprs.Add(Param.Name, Param.Name);
			Types.Add(Param.Name, Param.Type);
		}
		EmitLine(Root->Id, FString::Printf(TEXT("On%s = function(%s)"), *Event.Name, *FString::Join(Params, TEXT(", "))));
		if (const FString* Next = FindExecTarget(Root->Id, TEXT("next")))
		{
			EmitExecChain(*Next, Scope, 1, nullptr);
			if (HasFailed()) return Result;
		}
		EmitLine(Root->Id, TEXT("end"));
	}

	for (const FMantleGraphFunction& Fn : Graph.Functions)
	{
		const FMantleGraphNode* Root = FindRootNode(TEXT("function"), TEXT("function"), Fn.Name);
		check(Root);
		TSet<FString> Scope{Root->Id};
		TArray<FString> Params;
		TMap<FString, FString>& Exprs = OutputExpr.FindOrAdd(Root->Id);
		TMap<FString, FString>& Types = OutputType.FindOrAdd(Root->Id);
		for (const FMantleGraphParam& Param : Fn.Params)
		{
			Params.Add(Param.Name);
			Exprs.Add(Param.Name, Param.Name);
			Types.Add(Param.Name, Param.Type);
		}
		EmitLine(Root->Id, FString::Printf(TEXT("%s = function(%s)"), *Fn.Name, *FString::Join(Params, TEXT(", "))));
		if (const FString* Next = FindExecTarget(Root->Id, TEXT("next")))
		{
			EmitExecChain(*Next, Scope, 1, &Fn);
			if (HasFailed()) return Result;
		}
		EmitLine(Root->Id, TEXT("end"));
	}

	// Top-level roots: "start"/"event" nodes nobody's exec wire points at, in declaration order.
	for (const FMantleGraphNode& Node : Graph.Nodes)
	{
		if (ExecTargetNodeIds.Contains(Node.Id))
		{
			continue;
		}
		if (Node.Kind == TEXT("start"))
		{
			if (const FString* Next = FindExecTarget(Node.Id, TEXT("next")))
			{
				EmitExecChain(*Next, TSet<FString>{Node.Id}, 0, nullptr);
				if (HasFailed()) return Result;
			}
		}
		else if (Node.Kind == TEXT("event"))
		{
			EmitEventNode(Node, TSet<FString>{Node.Id}, 0, nullptr);
			if (HasFailed()) return Result;
		}
	}

	Result.bSuccess = true;
	// Insert the machine-readable map immediately after the human-facing GENERATED header. It is
	// part of the executable's line numbering, hence every old line after index zero moves down by
	// one; Result.LineMap and the embedded map intentionally use those final Luau line numbers.
	for (int32 i = 0; i < Lines.Num(); ++i)
	{
		if (!Lines[i].Value.IsEmpty())
		{
			Result.LineMap.Add(FMantleGraphLineMapEntry{i + 2, Lines[i].Value});
		}
	}
	TArray<FString> TextLines;
	TextLines.Reserve(Lines.Num() + 1);
	if (Lines.Num() > 0)
	{
		TextLines.Add(Lines[0].Key);
	}
	TextLines.Add(MantleEncodeGraphSourceMapHeader(Result.LineMap, Graph));
	for (int32 i = 1; i < Lines.Num(); ++i)
	{
		TextLines.Add(Lines[i].Key);
	}
	Result.Luau = FString::Join(TextLines, TEXT("\n")) + TEXT("\n");
	return Result;
}

// -------------------------------------------------------------------------------------------
// Public API

FMantleGraphCompileResult FMantleGraphCompiler::Compile(const FMantleGraph& Graph)
{
	FMantleGraphCompilerRun Runner(Graph);
	return Runner.Run();
}

FString FMantleGraphCompiler::EscapeLuauString(const FString& In)
{
	FString Out;
	Out.Reserve(In.Len() + 8);
	for (const TCHAR C : In)
	{
		switch (C)
		{
		case TEXT('\\'): Out += TEXT("\\\\"); break;
		case TEXT('"'): Out += TEXT("\\\""); break;
		case TEXT('\n'): Out += TEXT("\\n"); break;
		case TEXT('\r'): Out += TEXT("\\r"); break;
		case TEXT('\t'): Out += TEXT("\\t"); break;
		default: Out.AppendChar(C); break;
		}
	}
	return Out;
}

FString FMantleGraphCompiler::FormatLuauNumber(double Value)
{
	const double Rounded = FMath::RoundToDouble(Value);
	if (FMath::IsNearlyEqual(Value, Rounded, 1e-9) && FMath::Abs(Value) < 1e15)
	{
		return FString::Printf(TEXT("%lld"), (int64)Rounded);
	}
	return FString::Printf(TEXT("%.9g"), Value);
}
