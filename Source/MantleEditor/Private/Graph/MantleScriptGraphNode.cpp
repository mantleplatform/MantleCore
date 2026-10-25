// Copyright Joshua Eger. All Rights Reserved.
#include "Graph/MantleScriptGraphNode.h"

#include "Graph/MantleGraphCatalogue.h"
#include "Graph/MantleScriptEdGraph.h"

const FName FMantleGraphPinCategory::Exec(TEXT("exec"));
const FName FMantleGraphPinCategory::Boolean(TEXT("boolean"));
const FName FMantleGraphPinCategory::Number(TEXT("number"));
const FName FMantleGraphPinCategory::String(TEXT("string"));
const FName FMantleGraphPinCategory::Vector3(TEXT("vector3"));
const FName FMantleGraphPinCategory::Object(TEXT("object"));
const FName FMantleGraphPinCategory::Wildcard(TEXT("wildcard"));

FName FMantleGraphPinCategory::CategoryForType(const FString& RegistryType, FName& OutSubCategory)
{
	OutSubCategory = NAME_None;
	FString T = RegistryType.TrimStartAndEnd();
	if (T.EndsWith(TEXT("?")))
	{
		T = T.LeftChop(1);
	}
	if (T.IsEmpty() || T == TEXT("any") || T.StartsWith(TEXT("{")) || T.StartsWith(TEXT("(")))
	{
		return Wildcard;
	}
	if (T == TEXT("boolean") || T == TEXT("bool"))
	{
		return Boolean;
	}
	if (T == TEXT("number"))
	{
		return Number;
	}
	if (T == TEXT("string"))
	{
		return String;
	}
	if (T == TEXT("Vector3") || T == TEXT("vector"))
	{
		return Vector3;
	}
	OutSubCategory = FName(*T);
	return Object;
}

namespace
{
	UEdGraphPin* AddExecPin(UMantleScriptGraphNode& Node, EEdGraphPinDirection Dir, const TCHAR* Name)
	{
		return Node.CreatePin(Dir, FMantleGraphPinCategory::Exec, FName(Name));
	}

	UEdGraphPin* AddDataPin(UMantleScriptGraphNode& Node, EEdGraphPinDirection Dir, const FString& Type, const FString& PinName)
	{
		FName SubCategory;
		const FName Category = FMantleGraphPinCategory::CategoryForType(Type, SubCategory);
		UEdGraphPin* Pin = Node.CreatePin(Dir, Category, FName(*PinName));
		Pin->PinType.PinSubCategory = SubCategory;
		return Pin;
	}

	void AddPinsForMember(UMantleScriptGraphNode& Node, const FMantleGraphNodeDef& Def, bool bWithExec)
	{
		if (bWithExec)
		{
			AddExecPin(Node, EGPD_Output, TEXT("next"));
		}
		if (!Def.bService)
		{
			AddDataPin(Node, EGPD_Input, TEXT("Entity"), TEXT("target"));
		}
		for (const FMantleGraphPinDef& In : Def.Inputs)
		{
			AddDataPin(Node, EGPD_Input, In.Type, In.Name);
		}
		for (const FMantleGraphPinDef& Out : Def.Outputs)
		{
			AddDataPin(Node, EGPD_Output, Out.Type, Out.Name);
		}
		if (Def.Outputs.Num() == 0 && Def.Category == EMantleGraphNodeCategory::Event)
		{
			// The registry couldn't name this handler's own parameters (a fully variadic
			// signature); fall back to the author's own names, exactly like the JSON model's
			// "outputs" field does at compile time (MantleGraphCompiler.cpp's EmitEventNode).
			for (const FString& Name : Node.ExtraOutputNames)
			{
				AddDataPin(Node, EGPD_Output, TEXT("any"), Name);
			}
		}
	}
}

bool UMantleScriptGraphNode::IsPureKind() const
{
	static const TSet<FString> PureKinds = {
		TEXT("literal"), TEXT("getvar"), TEXT("math"), TEXT("compare"), TEXT("bool"),
		TEXT("concat"), TEXT("format"), TEXT("makevector3"), TEXT("breakvector3"),
		TEXT("vectormath"), TEXT("positiongoal"), TEXT("reroute"), TEXT("get")
	};
	return PureKinds.Contains(NodeKind);
}

bool UMantleScriptGraphNode::IsRootKind() const
{
	// "event" is deliberately not here even though it's often used as one (World.OnTagged with no
	// incoming wire): the S1.18 compiler also allows an exec wire INTO an event node (Door's
	// "touch" (Entity.Touched) is wired from setup code that first finds the door -- see
	// EmitExecChain's dispatch to EmitEventNode, and its own comment on why only start/onevent/
	// function are disallowed as a wire target). An event node's root-ness is decided purely by
	// wire topology (unwired == root) exactly like the compiler decides it, so it gets the same
	// "In" exec pin an ordinary statement node has; the pin simply stays unconnected on a graph
	// that uses it as a root.
	return NodeKind == TEXT("start") || NodeKind == TEXT("onevent") || NodeKind == TEXT("function");
}

void UMantleScriptGraphNode::AllocateDefaultPins()
{
	Pins.Empty();

	const bool bPure = IsPureKind();
	const bool bRoot = IsRootKind();
	if (!bPure && !bRoot)
	{
		AddExecPin(*this, EGPD_Input, TEXT("In"));
	}

	if (NodeKind == TEXT("event") || NodeKind == TEXT("call") || NodeKind == TEXT("get") || NodeKind == TEXT("set"))
	{
		FString Err;
		const EMantleGraphNodeCategory Category =
			NodeKind == TEXT("event") ? EMantleGraphNodeCategory::Event :
			NodeKind == TEXT("call") ? EMantleGraphNodeCategory::Call :
			NodeKind == TEXT("get") ? EMantleGraphNodeCategory::Get : EMantleGraphNodeCategory::Set;
		if (const FMantleGraphNodeDef* Def = FMantleGraphCatalogue::FindByDotted(Dotted, Category, Err))
		{
			AddPinsForMember(*this, *Def, !bPure);
			CatalogueDoc = Def->Doc;
		}
		return;
	}

	const UMantleScriptEdGraph* Owner = Cast<UMantleScriptEdGraph>(GetGraph());

	if (NodeKind == TEXT("start"))
	{
		AddExecPin(*this, EGPD_Output, TEXT("next"));
	}
	else if (NodeKind == TEXT("onevent"))
	{
		AddExecPin(*this, EGPD_Output, TEXT("next"));
		if (Owner)
		{
			if (const FMantleEdGraphCustomEvent* Event = Owner->FindCustomEvent(EventName))
			{
				for (const FMantleEdGraphParam& Param : Event->Params)
				{
					AddDataPin(*this, EGPD_Output, Param.Type, Param.Name);
				}
			}
		}
	}
	else if (NodeKind == TEXT("function"))
	{
		AddExecPin(*this, EGPD_Output, TEXT("next"));
		if (Owner)
		{
			if (const FMantleEdGraphFunction* Fn = Owner->FindFunction(FunctionName))
			{
				for (const FMantleEdGraphParam& Param : Fn->Params)
				{
					AddDataPin(*this, EGPD_Output, Param.Type, Param.Name);
				}
			}
		}
	}
	else if (NodeKind == TEXT("callevent"))
	{
		AddExecPin(*this, EGPD_Output, TEXT("next"));
		if (Owner)
		{
			if (const FMantleEdGraphCustomEvent* Event = Owner->FindCustomEvent(EventName))
			{
				for (const FMantleEdGraphParam& Param : Event->Params)
				{
					AddDataPin(*this, EGPD_Input, Param.Type, Param.Name);
				}
			}
		}
	}
	else if (NodeKind == TEXT("callfunction"))
	{
		AddExecPin(*this, EGPD_Output, TEXT("next"));
		if (Owner)
		{
			if (const FMantleEdGraphFunction* Fn = Owner->FindFunction(FunctionName))
			{
				for (const FMantleEdGraphParam& Param : Fn->Params)
				{
					AddDataPin(*this, EGPD_Input, Param.Type, Param.Name);
				}
				for (const FMantleEdGraphParam& Ret : Fn->Returns)
				{
					AddDataPin(*this, EGPD_Output, Ret.Type, Ret.Name);
				}
			}
		}
	}
	else if (NodeKind == TEXT("return"))
	{
		// No exec out: a return node ends its function's exec chain. Its input pins are the
		// enclosing function's declared Returns -- FunctionName is set by the author for the
		// editor's benefit only (the compiler infers the enclosing function structurally and never
		// reads a "function" field off a return node; see MantleGraphCompiler.cpp's EmitReturnNode).
		if (Owner)
		{
			if (const FMantleEdGraphFunction* Fn = Owner->FindFunction(FunctionName))
			{
				for (const FMantleEdGraphParam& Ret : Fn->Returns)
				{
					AddDataPin(*this, EGPD_Input, Ret.Type, Ret.Name);
				}
			}
		}
	}
	else if (NodeKind == TEXT("branch"))
	{
		AddDataPin(*this, EGPD_Input, TEXT("boolean"), TEXT("condition"));
		AddExecPin(*this, EGPD_Output, TEXT("true"));
		AddExecPin(*this, EGPD_Output, TEXT("false"));
	}
	else if (NodeKind == TEXT("sequence"))
	{
		NumOutputs = FMath::Clamp(NumOutputs, 1, 12);
		for (int32 i = 0; i < NumOutputs; ++i)
		{
			AddExecPin(*this, EGPD_Output, *FString::FromInt(i));
		}
	}
	else if (NodeKind == TEXT("forloop"))
	{
		AddDataPin(*this, EGPD_Input, TEXT("number"), TEXT("first"));
		AddDataPin(*this, EGPD_Input, TEXT("number"), TEXT("last"));
		AddDataPin(*this, EGPD_Input, TEXT("number"), TEXT("step"));
		AddExecPin(*this, EGPD_Output, TEXT("body"));
		AddExecPin(*this, EGPD_Output, TEXT("done"));
		AddDataPin(*this, EGPD_Output, TEXT("number"), TEXT("index"));
	}
	else if (NodeKind == TEXT("foreach"))
	{
		AddDataPin(*this, EGPD_Input, TEXT("any"), TEXT("array"));
		AddExecPin(*this, EGPD_Output, TEXT("body"));
		AddExecPin(*this, EGPD_Output, TEXT("done"));
		AddDataPin(*this, EGPD_Output, TEXT("any"), TEXT("value"));
	}
	else if (NodeKind == TEXT("while"))
	{
		AddDataPin(*this, EGPD_Input, TEXT("boolean"), TEXT("condition"));
		AddExecPin(*this, EGPD_Output, TEXT("body"));
		AddExecPin(*this, EGPD_Output, TEXT("done"));
	}
	else if (NodeKind == TEXT("delay"))
	{
		AddDataPin(*this, EGPD_Input, TEXT("number"), TEXT("seconds"));
		AddExecPin(*this, EGPD_Output, TEXT("next"));
	}
	else if (NodeKind == TEXT("doonce"))
	{
		AddExecPin(*this, EGPD_Output, TEXT("next"));
	}
	else if (NodeKind == TEXT("setvar"))
	{
		FString Type = TEXT("any");
		if (Owner)
		{
			if (const FMantleEdGraphVariable* Var = Owner->FindVariable(VariableName))
			{
				Type = Var->Type;
			}
		}
		AddDataPin(*this, EGPD_Input, Type, TEXT("value"));
		AddExecPin(*this, EGPD_Output, TEXT("next"));
	}
	else if (NodeKind == TEXT("getvar"))
	{
		FString Type = TEXT("any");
		if (Owner)
		{
			if (const FMantleEdGraphVariable* Var = Owner->FindVariable(VariableName))
			{
				Type = Var->Type;
			}
		}
		AddDataPin(*this, EGPD_Output, Type, TEXT("value"));
	}
	else if (NodeKind == TEXT("print") || NodeKind == TEXT("warn"))
	{
		AddDataPin(*this, EGPD_Input, TEXT("string"), TEXT("text"));
		AddExecPin(*this, EGPD_Output, TEXT("next"));
	}
	else if (NodeKind == TEXT("literal"))
	{
		const FString Type = LiteralType == TEXT("bool") ? TEXT("boolean")
			: LiteralType == TEXT("vector") ? TEXT("Vector3") : LiteralType;
		AddDataPin(*this, EGPD_Output, Type.IsEmpty() ? TEXT("number") : Type, TEXT("value"));
	}
	else if (NodeKind == TEXT("math"))
	{
		AddDataPin(*this, EGPD_Input, TEXT("number"), TEXT("a"));
		if (Op != TEXT("neg"))
		{
			AddDataPin(*this, EGPD_Input, TEXT("number"), TEXT("b"));
		}
		AddDataPin(*this, EGPD_Output, TEXT("number"), TEXT("value"));
	}
	else if (NodeKind == TEXT("compare"))
	{
		AddDataPin(*this, EGPD_Input, TEXT("number"), TEXT("a"));
		AddDataPin(*this, EGPD_Input, TEXT("number"), TEXT("b"));
		AddDataPin(*this, EGPD_Output, TEXT("boolean"), TEXT("value"));
	}
	else if (NodeKind == TEXT("bool"))
	{
		AddDataPin(*this, EGPD_Input, TEXT("boolean"), TEXT("a"));
		if (Op != TEXT("not"))
		{
			AddDataPin(*this, EGPD_Input, TEXT("boolean"), TEXT("b"));
		}
		AddDataPin(*this, EGPD_Output, TEXT("boolean"), TEXT("value"));
	}
	else if (NodeKind == TEXT("concat") || NodeKind == TEXT("format"))
	{
		NumOutputs = FMath::Clamp(NumOutputs, 0, 12);
		for (int32 i = 0; i < NumOutputs; ++i)
		{
			AddDataPin(*this, EGPD_Input, TEXT("any"), *FString::Printf(TEXT("values%d"), i));
		}
		AddDataPin(*this, EGPD_Output, TEXT("string"), TEXT("value"));
	}
	else if (NodeKind == TEXT("makevector3"))
	{
		AddDataPin(*this, EGPD_Input, TEXT("number"), TEXT("x"));
		AddDataPin(*this, EGPD_Input, TEXT("number"), TEXT("y"));
		AddDataPin(*this, EGPD_Input, TEXT("number"), TEXT("z"));
		AddDataPin(*this, EGPD_Output, TEXT("Vector3"), TEXT("value"));
	}
	else if (NodeKind == TEXT("positiongoal"))
	{
		AddDataPin(*this, EGPD_Input, TEXT("Vector3"), TEXT("position"));
		AddDataPin(*this, EGPD_Output, TEXT("{Position: Vector3?, Rotation: Vector3?, Scale: Vector3?}"), TEXT("value"));
	}
	else if (NodeKind == TEXT("breakvector3"))
	{
		AddDataPin(*this, EGPD_Input, TEXT("Vector3"), TEXT("value"));
		AddDataPin(*this, EGPD_Output, TEXT("number"), TEXT("x"));
		AddDataPin(*this, EGPD_Output, TEXT("number"), TEXT("y"));
		AddDataPin(*this, EGPD_Output, TEXT("number"), TEXT("z"));
	}
	else if (NodeKind == TEXT("vectormath"))
	{
		AddDataPin(*this, EGPD_Input, TEXT("Vector3"), TEXT("a"));
		AddDataPin(*this, EGPD_Input, TEXT("any"), TEXT("b"));
		AddDataPin(*this, EGPD_Output, TEXT("Vector3"), TEXT("value"));
	}
	else if (NodeKind == TEXT("reroute"))
	{
		AddDataPin(*this, EGPD_Input, TEXT("any"), TEXT("value"));
		AddDataPin(*this, EGPD_Output, TEXT("any"), TEXT("value"));
	}
}

void UMantleScriptGraphNode::AddExtraPin()
{
	Modify();
	NumOutputs = FMath::Clamp(NumOutputs + 1, 1, 12);
	AllocateDefaultPins();
	GetGraph()->NotifyGraphChanged();
}

void UMantleScriptGraphNode::RemoveExtraPin()
{
	Modify();
	NumOutputs = FMath::Clamp(NumOutputs - 1, 1, 12);
	AllocateDefaultPins();
	GetGraph()->NotifyGraphChanged();
}

FText UMantleScriptGraphNode::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	if (!Dotted.IsEmpty())
	{
		return FText::FromString(Dotted);
	}
	if (NodeKind == TEXT("literal"))
	{
		return FText::FromString(FString::Printf(TEXT("Literal %s"), *LiteralType));
	}
	if (NodeKind == TEXT("getvar"))
	{
		return FText::FromString(FString::Printf(TEXT("Get %s"), *VariableName));
	}
	if (NodeKind == TEXT("setvar"))
	{
		return FText::FromString(FString::Printf(TEXT("Set %s"), *VariableName));
	}
	if (NodeKind == TEXT("onevent"))
	{
		return FText::FromString(FString::Printf(TEXT("On %s"), *EventName));
	}
	if (NodeKind == TEXT("callevent"))
	{
		return FText::FromString(FString::Printf(TEXT("Fire %s"), *EventName));
	}
	if (NodeKind == TEXT("function"))
	{
		return FText::FromString(FString::Printf(TEXT("Function: %s"), *FunctionName));
	}
	if (NodeKind == TEXT("callfunction"))
	{
		return FText::FromString(FunctionName);
	}
	if (NodeKind == TEXT("math") || NodeKind == TEXT("compare") || NodeKind == TEXT("bool") || NodeKind == TEXT("vectormath"))
	{
		return FText::FromString(Op);
	}
	static const TMap<FString, FString> Titles = {
		{TEXT("start"), TEXT("Start")}, {TEXT("branch"), TEXT("Branch")}, {TEXT("sequence"), TEXT("Sequence")},
		{TEXT("forloop"), TEXT("For Loop")}, {TEXT("foreach"), TEXT("For Each")}, {TEXT("while"), TEXT("While")},
		{TEXT("delay"), TEXT("Delay")}, {TEXT("doonce"), TEXT("Do Once")}, {TEXT("return"), TEXT("Return")},
		{TEXT("print"), TEXT("Print")}, {TEXT("warn"), TEXT("Warn")}, {TEXT("concat"), TEXT("Concat")},
		{TEXT("format"), TEXT("Format")}, {TEXT("makevector3"), TEXT("Make Vector3")},
		{TEXT("positiongoal"), TEXT("Position Tween Goal")}, {TEXT("breakvector3"), TEXT("Break Vector3")}, {TEXT("reroute"), TEXT("Reroute")}
	};
	if (const FString* Found = Titles.Find(NodeKind))
	{
		return FText::FromString(*Found);
	}
	return FText::FromString(NodeKind);
}

FText UMantleScriptGraphNode::GetTooltipText() const
{
	if (!CatalogueDoc.IsEmpty())
	{
		return FText::FromString(CatalogueDoc);
	}
	return GetNodeTitle(ENodeTitleType::FullTitle);
}

FLinearColor UMantleScriptGraphNode::GetNodeTitleColor() const
{
	if (IsRootKind())
	{
		return FLinearColor(0.7f, 0.07f, 0.1f); // Blueprint event red
	}
	if (IsPureKind())
	{
		return FLinearColor(0.02f, 0.4f, 0.02f); // Blueprint pure-node green
	}
	if (NodeKind == TEXT("branch") || NodeKind == TEXT("sequence") || NodeKind == TEXT("forloop") ||
		NodeKind == TEXT("foreach") || NodeKind == TEXT("while") || NodeKind == TEXT("doonce"))
	{
		return FLinearColor(0.15f, 0.15f, 0.6f); // Blueprint flow-control blue-grey
	}
	return FLinearColor(0.4f, 0.4f, 0.4f); // ordinary call/statement grey
}
