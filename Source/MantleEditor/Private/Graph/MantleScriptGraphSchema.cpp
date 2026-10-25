// Copyright Joshua Eger. All Rights Reserved.
#include "Graph/MantleScriptGraphSchema.h"

#include "Graph/MantleGraphCatalogue.h"
#include "Graph/MantleScriptEdGraph.h"
#include "Graph/MantleScriptGraphNode.h"
#include "Graph/MantleScriptGraphSerializer.h"

#include "EdGraph/EdGraphNode.h"
#include "EdGraphNode_Comment.h"
#include "ScopedTransaction.h"
#include "ToolMenus.h"

#define LOCTEXT_NAMESPACE "MantleScriptGraph"

namespace
{
	UMantleScriptGraphNode* MakeTemplate(const FText& Category, const FText& MenuDesc, const FText& Tooltip,
		TArray<TSharedPtr<FEdGraphSchemaAction>>& Out, TFunctionRef<void(UMantleScriptGraphNode&)> Configure)
	{
		UMantleScriptGraphNode* Template = NewObject<UMantleScriptGraphNode>(GetTransientPackage());
		Configure(*Template);

		TSharedPtr<FEdGraphSchemaAction_NewNode> Action = MakeShared<FEdGraphSchemaAction_NewNode>(Category, MenuDesc, Tooltip, 0);
		Action->NodeTemplate = Template;
		Out.Add(Action);
		return Template;
	}

	const TCHAR* MathOpSymbol(const FString& Op)
	{
		if (Op == TEXT("add")) return TEXT("+");
		if (Op == TEXT("sub")) return TEXT("-");
		if (Op == TEXT("mul")) return TEXT("*");
		if (Op == TEXT("div")) return TEXT("/");
		if (Op == TEXT("mod")) return TEXT("%");
		return TEXT("Negate");
	}

	const TCHAR* CompareOpSymbol(const FString& Op)
	{
		if (Op == TEXT("eq")) return TEXT("Equal (==)");
		if (Op == TEXT("ne")) return TEXT("Not Equal (~=)");
		if (Op == TEXT("lt")) return TEXT("Less Than (<)");
		if (Op == TEXT("le")) return TEXT("Less Or Equal (<=)");
		if (Op == TEXT("gt")) return TEXT("Greater Than (>)");
		return TEXT("Greater Or Equal (>=)");
	}
}

void UMantleScriptGraphSchema::CollectActions(const UEdGraph* Graph, const UEdGraphPin* FromPin, TArray<TSharedPtr<FEdGraphSchemaAction>>& Out) const
{
	const UMantleScriptEdGraph* EdGraph = Cast<UMantleScriptEdGraph>(Graph);

	EMantleSide Side = EMantleSide::Server;
	FString Warning;
	if (Graph)
	{
		FMantleScriptGraphSerializer::ClassifySide(Graph->GetOutermost()->GetName(), Side, Warning);
	}

	// Right-clicking empty canvas (FromPin == nullptr) offers everything. Dragging from an exec
	// pin offers exec-capable kinds (events/calls/sets/control-flow/variable-set/etc; Get is pure
	// and excluded). Dragging from a data pin offers whatever can appear on the opposite side of a
	// data wire: an output pin wants a consumer (a Call/Set's input, or exec kinds in general,
	// since exec-kind palette entries still expose their own data input pins to wire into); an
	// input pin wants a producer (Get/Call/pure kinds). Precise per-pin *type* filtering for the
	// ~25 static control/expression kinds happens in the "pure" block further down; catalogue and
	// dynamic (variable/event/function) actions only get this coarse exec-vs-data filter -- see
	// the header comment on this method's declaration.
	const bool bFromExec = FromPin == nullptr || FromPin->PinType.PinCategory == FMantleGraphPinCategory::Exec;
	const bool bFromDataInput = FromPin && FromPin->PinType.PinCategory != FMantleGraphPinCategory::Exec && FromPin->Direction == EGPD_Input;

	// ---- Catalogue: event / call / get / set -------------------------------------------------
	for (const FMantleGraphNodeDef& Def : FMantleGraphCatalogue::All())
	{
		if (Def.Side != EMantleSide::Both && Def.Side != Side)
		{
			continue;
		}
		if (bFromDataInput && Def.Category != EMantleGraphNodeCategory::Get && Def.Category != EMantleGraphNodeCategory::Call && Def.Category != EMantleGraphNodeCategory::Event)
		{
			continue; // dragging off a data-input pin wants something that can feed it a value
		}

		const TCHAR* KindStr = Def.Category == EMantleGraphNodeCategory::Event ? TEXT("event")
			: Def.Category == EMantleGraphNodeCategory::Call ? TEXT("call")
			: Def.Category == EMantleGraphNodeCategory::Get ? TEXT("get") : TEXT("set");
		const FText Category = FText::FromString(Def.ClassName);
		const FText Desc = FText::FromString(Def.Dotted);
		const FText Tooltip = Def.Doc.IsEmpty() ? Desc : FText::FromString(Def.Doc);
		MakeTemplate(Category, Desc, Tooltip, Out, [&](UMantleScriptGraphNode& Node)
		{
			Node.NodeKind = KindStr;
			Node.Dotted = Def.Dotted;
		});
	}

	if (bFromExec)
	{
		// ---- Control flow ----------------------------------------------------------------------
		auto AddControl = [&](const TCHAR* Kind, const TCHAR* Name, const TCHAR* Tooltip)
		{
			MakeTemplate(LOCTEXT("CatFlow", "Flow Control"), FText::FromString(Name), FText::FromString(Tooltip), Out,
				[&](UMantleScriptGraphNode& Node) { Node.NodeKind = Kind; });
		};
		AddControl(TEXT("branch"), TEXT("Branch"), TEXT("If/else: routes exec to True or False depending on a boolean."));
		AddControl(TEXT("sequence"), TEXT("Sequence"), TEXT("Runs each output in order, once each."));
		AddControl(TEXT("forloop"), TEXT("For Loop"), TEXT("Runs Body once per step from First to Last, then Done."));
		AddControl(TEXT("foreach"), TEXT("For Each"), TEXT("Runs Body once per element of an array, then Done."));
		AddControl(TEXT("while"), TEXT("While"), TEXT("Runs Body while Condition is true, then Done."));
		AddControl(TEXT("delay"), TEXT("Delay"), TEXT("Waits Seconds before continuing."));
		AddControl(TEXT("doonce"), TEXT("Do Once"), TEXT("Only lets exec through the first time it runs."));
		AddControl(TEXT("print"), TEXT("Print"), TEXT("Logs Text."));
		AddControl(TEXT("warn"), TEXT("Warn"), TEXT("Logs Text as a warning."));

		if (EdGraph)
		{
			for (const FMantleEdGraphVariable& Var : EdGraph->Variables)
			{
				MakeTemplate(LOCTEXT("CatVars", "Variables"), FText::FromString(TEXT("Set ") + Var.Name), FText::GetEmpty(), Out,
					[&](UMantleScriptGraphNode& Node) { Node.NodeKind = TEXT("setvar"); Node.VariableName = Var.Name; });
			}
			for (const FMantleEdGraphCustomEvent& Event : EdGraph->CustomEvents)
			{
				MakeTemplate(LOCTEXT("CatEvents", "Custom Events"), FText::FromString(TEXT("Fire ") + Event.Name), FText::GetEmpty(), Out,
					[&](UMantleScriptGraphNode& Node) { Node.NodeKind = TEXT("callevent"); Node.EventName = Event.Name; });
				MakeTemplate(LOCTEXT("CatEvents", "Custom Events"), FText::FromString(TEXT("On ") + Event.Name), FText::GetEmpty(), Out,
					[&](UMantleScriptGraphNode& Node) { Node.NodeKind = TEXT("onevent"); Node.EventName = Event.Name; });
			}
			for (const FMantleEdGraphFunction& Fn : EdGraph->Functions)
			{
				MakeTemplate(LOCTEXT("CatFuncs", "Functions"), FText::FromString(Fn.Name), FText::GetEmpty(), Out,
					[&](UMantleScriptGraphNode& Node) { Node.NodeKind = TEXT("callfunction"); Node.FunctionName = Fn.Name; });
				MakeTemplate(LOCTEXT("CatFuncs", "Functions"), FText::FromString(TEXT("Function: ") + Fn.Name), FText::GetEmpty(), Out,
					[&](UMantleScriptGraphNode& Node) { Node.NodeKind = TEXT("function"); Node.FunctionName = Fn.Name; });
				MakeTemplate(LOCTEXT("CatFuncs", "Functions"), FText::FromString(TEXT("Return (") + Fn.Name + TEXT(")")), FText::GetEmpty(), Out,
					[&](UMantleScriptGraphNode& Node) { Node.NodeKind = TEXT("return"); Node.FunctionName = Fn.Name; });
			}
		}

		// A comment box is Unreal's own UEdGraphNode_Comment -- a freeform, resizable note that is
		// never part of FMantleGraph.Nodes (MantleScriptGraphSerializer::BuildGraph skips any node
		// that isn't a UMantleScriptGraphNode), exactly like the S1.18 compiler's own "comment"
		// kind never reaches Luau.
		TSharedPtr<FEdGraphSchemaAction_NewNode> CommentAction = MakeShared<FEdGraphSchemaAction_NewNode>(
			FText::GetEmpty(), LOCTEXT("AddComment", "Add Comment..."), LOCTEXT("AddCommentTip", "A freeform note; never compiled."), 0);
		CommentAction->NodeTemplate = NewObject<UEdGraphNode_Comment>(GetTransientPackage());
		Out.Add(CommentAction);
	}

	// ---- Pure / data-producing --------------------------------------------------------------
	if (!bFromDataInput || bFromExec)
	{
		auto AddLiteral = [&](const TCHAR* Type, const TCHAR* Name)
		{
			MakeTemplate(LOCTEXT("CatLit", "Literals"), FText::FromString(Name), FText::GetEmpty(), Out,
				[&](UMantleScriptGraphNode& Node) { Node.NodeKind = TEXT("literal"); Node.LiteralType = Type; });
		};
		AddLiteral(TEXT("bool"), TEXT("Literal Bool"));
		AddLiteral(TEXT("number"), TEXT("Literal Number"));
		AddLiteral(TEXT("string"), TEXT("Literal String"));
		AddLiteral(TEXT("vector"), TEXT("Literal Vector3"));

		for (const FString& Op : {TEXT("add"), TEXT("sub"), TEXT("mul"), TEXT("div"), TEXT("mod"), TEXT("neg")})
		{
			MakeTemplate(LOCTEXT("CatMath", "Math"), FText::FromString(MathOpSymbol(Op)), FText::GetEmpty(), Out,
				[&](UMantleScriptGraphNode& Node) { Node.NodeKind = TEXT("math"); Node.Op = Op; });
		}
		for (const FString& Op : {TEXT("eq"), TEXT("ne"), TEXT("lt"), TEXT("le"), TEXT("gt"), TEXT("ge")})
		{
			MakeTemplate(LOCTEXT("CatCompare", "Compare"), FText::FromString(CompareOpSymbol(Op)), FText::GetEmpty(), Out,
				[&](UMantleScriptGraphNode& Node) { Node.NodeKind = TEXT("compare"); Node.Op = Op; });
		}
		for (const FString& Op : {TEXT("and"), TEXT("or"), TEXT("not")})
		{
			MakeTemplate(LOCTEXT("CatBool", "Boolean"), FText::FromString(Op.ToUpper()), FText::GetEmpty(), Out,
				[&](UMantleScriptGraphNode& Node) { Node.NodeKind = TEXT("bool"); Node.Op = Op; });
		}
		MakeTemplate(LOCTEXT("CatText", "Text"), LOCTEXT("Concat", "Concat"), FText::GetEmpty(), Out,
			[&](UMantleScriptGraphNode& Node) { Node.NodeKind = TEXT("concat"); Node.NumOutputs = 2; });
		MakeTemplate(LOCTEXT("CatText", "Text"), LOCTEXT("Format", "Format"), LOCTEXT("FormatTip", "Text with \"{}\" placeholders, one per value pin."), Out,
			[&](UMantleScriptGraphNode& Node) { Node.NodeKind = TEXT("format"); Node.NumOutputs = 1; });
		MakeTemplate(LOCTEXT("CatVec", "Vector3"), LOCTEXT("MakeVector3", "Make Vector3"), FText::GetEmpty(), Out,
			[&](UMantleScriptGraphNode& Node) { Node.NodeKind = TEXT("makevector3"); });
		MakeTemplate(LOCTEXT("CatVec", "Vector3"), LOCTEXT("BreakVector3", "Break Vector3"), FText::GetEmpty(), Out,
			[&](UMantleScriptGraphNode& Node) { Node.NodeKind = TEXT("breakvector3"); });
		MakeTemplate(LOCTEXT("CatVec", "Vector3"), LOCTEXT("PositionGoal", "Position Tween Goal"),
			LOCTEXT("PositionGoalTip", "Build a typed Position goal for Entity.Tween. Other transform properties are unchanged."), Out,
			[&](UMantleScriptGraphNode& Node) { Node.NodeKind = TEXT("positiongoal"); });
		for (const FString& Op : {TEXT("add"), TEXT("sub"), TEXT("scale")})
		{
			MakeTemplate(LOCTEXT("CatVec", "Vector3"), FText::FromString(FString(TEXT("Vector3 ")) + MathOpSymbol(Op)), FText::GetEmpty(), Out,
				[&](UMantleScriptGraphNode& Node) { Node.NodeKind = TEXT("vectormath"); Node.Op = Op; });
		}
		MakeTemplate(LOCTEXT("CatUtil", "Utilities"), LOCTEXT("Reroute", "Reroute"), FText::GetEmpty(), Out,
			[&](UMantleScriptGraphNode& Node) { Node.NodeKind = TEXT("reroute"); });

		if (EdGraph)
		{
			for (const FMantleEdGraphVariable& Var : EdGraph->Variables)
			{
				MakeTemplate(LOCTEXT("CatVars", "Variables"), FText::FromString(TEXT("Get ") + Var.Name), FText::GetEmpty(), Out,
					[&](UMantleScriptGraphNode& Node) { Node.NodeKind = TEXT("getvar"); Node.VariableName = Var.Name; });
			}
		}
	}
}

void UMantleScriptGraphSchema::GetGraphContextActions(FGraphContextMenuBuilder& ContextMenuBuilder) const
{
	TArray<TSharedPtr<FEdGraphSchemaAction>> Actions;
	CollectActions(ContextMenuBuilder.CurrentGraph, ContextMenuBuilder.FromPin, Actions);
	for (const TSharedPtr<FEdGraphSchemaAction>& Action : Actions)
	{
		ContextMenuBuilder.AddAction(Action);
	}
}

const FPinConnectionResponse UMantleScriptGraphSchema::CanCreateConnection(const UEdGraphPin* A, const UEdGraphPin* B) const
{
	if (!A || !B || A == B)
	{
		return FPinConnectionResponse(CONNECT_RESPONSE_DISALLOW, TEXT("invalid pins"));
	}
	if (A->GetOwningNode() == B->GetOwningNode())
	{
		return FPinConnectionResponse(CONNECT_RESPONSE_DISALLOW, TEXT("can't connect a node to itself"));
	}
	if (A->Direction == B->Direction)
	{
		return FPinConnectionResponse(CONNECT_RESPONSE_DISALLOW, TEXT("both pins are on the same side"));
	}

	const UEdGraphPin* OutPin = A->Direction == EGPD_Output ? A : B;
	const UEdGraphPin* InPin = A->Direction == EGPD_Output ? B : A;
	const bool bAIsIn = (InPin == A);

	const bool bOutIsExec = OutPin->PinType.PinCategory == FMantleGraphPinCategory::Exec;
	const bool bInIsExec = InPin->PinType.PinCategory == FMantleGraphPinCategory::Exec;
	if (bOutIsExec != bInIsExec)
	{
		return FPinConnectionResponse(CONNECT_RESPONSE_DISALLOW, TEXT("can't wire exec to data"));
	}

	if (!bOutIsExec)
	{
		const bool bWildcard = OutPin->PinType.PinCategory == FMantleGraphPinCategory::Wildcard || InPin->PinType.PinCategory == FMantleGraphPinCategory::Wildcard;
		if (!bWildcard)
		{
			if (OutPin->PinType.PinCategory != InPin->PinType.PinCategory)
			{
				return FPinConnectionResponse(CONNECT_RESPONSE_DISALLOW, TEXT("pin types don't match"));
			}
			if (OutPin->PinType.PinCategory == FMantleGraphPinCategory::Object &&
				OutPin->PinType.PinSubCategory != InPin->PinType.PinSubCategory)
			{
				return FPinConnectionResponse(CONNECT_RESPONSE_DISALLOW,
					FString::Printf(TEXT("%s isn't a %s"), *OutPin->PinType.PinSubCategory.ToString(), *InPin->PinType.PinSubCategory.ToString()));
			}
		}
	}

	// A data-input pin (and, for exec, an exec-OUTPUT pin -- "then" only ever goes one place)
	// takes at most one link; wiring a new one there quietly replaces the old one, matching
	// Blueprint's own convention.
	if (!bOutIsExec && InPin->LinkedTo.Num() > 0)
	{
		return FPinConnectionResponse(bAIsIn ? CONNECT_RESPONSE_BREAK_OTHERS_A : CONNECT_RESPONSE_BREAK_OTHERS_B, TEXT("replace connection"));
	}
	if (bOutIsExec && OutPin->LinkedTo.Num() > 0)
	{
		return FPinConnectionResponse(bAIsIn ? CONNECT_RESPONSE_BREAK_OTHERS_B : CONNECT_RESPONSE_BREAK_OTHERS_A, TEXT("replace connection"));
	}
	return FPinConnectionResponse(CONNECT_RESPONSE_MAKE, FString());
}

FLinearColor UMantleScriptGraphSchema::GetPinTypeColor(const FEdGraphPinType& PinType) const
{
	if (PinType.PinCategory == FMantleGraphPinCategory::Exec) return FLinearColor::White;
	if (PinType.PinCategory == FMantleGraphPinCategory::Boolean) return FLinearColor(0.6f, 0.0f, 0.0f);
	if (PinType.PinCategory == FMantleGraphPinCategory::Number) return FLinearColor(0.0f, 0.6f, 0.4f);
	if (PinType.PinCategory == FMantleGraphPinCategory::String) return FLinearColor(0.9f, 0.15f, 0.85f);
	if (PinType.PinCategory == FMantleGraphPinCategory::Vector3) return FLinearColor(0.9f, 0.75f, 0.05f);
	if (PinType.PinCategory == FMantleGraphPinCategory::Object) return FLinearColor(0.05f, 0.35f, 0.9f);
	return FLinearColor(0.7f, 0.7f, 0.7f); // wildcard
}

void UMantleScriptGraphSchema::GetContextMenuActions(UToolMenu* Menu, UGraphNodeContextMenuContext* Context) const
{
	Super::GetContextMenuActions(Menu, Context);

	if (!Context || !Context->Node)
	{
		return;
	}
	UMantleScriptGraphNode* Node = const_cast<UMantleScriptGraphNode*>(Cast<const UMantleScriptGraphNode>(Context->Node.Get()));
	if (!Node || (Node->NodeKind != TEXT("sequence") && Node->NodeKind != TEXT("concat") && Node->NodeKind != TEXT("format")))
	{
		return;
	}

	FToolMenuSection& Section = Menu->AddSection(TEXT("MantlePins"), LOCTEXT("MantlePinsHeading", "Pins"));
	Section.AddMenuEntry(TEXT("AddPin"), LOCTEXT("AddPin", "Add pin"), FText::GetEmpty(), FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([Node]()
		{
			const FScopedTransaction Transaction(LOCTEXT("AddPinTx", "Add Pin"));
			Node->Modify();
			Node->AddExtraPin();
		})));
	Section.AddMenuEntry(TEXT("RemovePin"), LOCTEXT("RemovePin", "Remove pin"), FText::GetEmpty(), FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([Node]()
		{
			const FScopedTransaction Transaction(LOCTEXT("RemovePinTx", "Remove Pin"));
			Node->Modify();
			Node->RemoveExtraPin();
		})));
}

#undef LOCTEXT_NAMESPACE
