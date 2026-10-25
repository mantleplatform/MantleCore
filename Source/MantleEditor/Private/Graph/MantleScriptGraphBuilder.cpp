// Copyright Joshua Eger. All Rights Reserved.
#include "Graph/MantleScriptGraphBuilder.h"

#include "Graph/MantleScriptEdGraph.h"
#include "Graph/MantleScriptGraphNode.h"

UMantleScriptGraphNode& FMantleScriptGraphBuilder::AddNode(UMantleScriptEdGraph& Graph, const FString& Kind, const FVector2D& Position)
{
	UMantleScriptGraphNode* Node = NewObject<UMantleScriptGraphNode>(&Graph, NAME_None, RF_Transactional);
	Node->NodeKind = Kind;

	Graph.AddNode(Node, /*bFromUI*/ false, /*bSelectNewNode*/ false);
	Node->CreateNewGuid();
	Node->NodePosX = Position.X;
	Node->NodePosY = Position.Y;
	Node->AllocateDefaultPins();
	return *Node;
}

UMantleScriptGraphNode& FMantleScriptGraphBuilder::AddNode(UMantleScriptEdGraph& Graph, const FString& Kind,
	const FVector2D& Position, TFunctionRef<void(UMantleScriptGraphNode&)> Configure)
{
	UMantleScriptGraphNode* Node = NewObject<UMantleScriptGraphNode>(&Graph, NAME_None, RF_Transactional);
	Node->NodeKind = Kind;
	Configure(*Node);

	Graph.AddNode(Node, /*bFromUI*/ false, /*bSelectNewNode*/ false);
	Node->CreateNewGuid();
	Node->NodePosX = Position.X;
	Node->NodePosY = Position.Y;
	Node->AllocateDefaultPins();
	return *Node;
}

UMantleScriptGraphNode& FMantleScriptGraphBuilder::AddMemberNode(UMantleScriptEdGraph& Graph, const FString& Kind, const FString& Dotted,
	const FVector2D& Position)
{
	return AddNode(Graph, Kind, Position, [&Dotted](UMantleScriptGraphNode& Node)
	{
		Node.Dotted = Dotted;
	});
}

UMantleScriptGraphNode& FMantleScriptGraphBuilder::AddMemberNode(UMantleScriptEdGraph& Graph, const FString& Kind, const FString& Dotted,
	const FVector2D& Position, TFunctionRef<void(UMantleScriptGraphNode&)> Configure)
{
	return AddNode(Graph, Kind, Position, [&](UMantleScriptGraphNode& Node)
	{
		Node.Dotted = Dotted;
		Configure(Node);
	});
}

void FMantleScriptGraphBuilder::Wire(UMantleScriptGraphNode& From, const FString& FromPinName, UMantleScriptGraphNode& To, const FString& ToPinName)
{
	UEdGraphPin* FromPin = From.FindPin(*FromPinName, EGPD_Output);
	UEdGraphPin* ToPin = To.FindPin(*ToPinName, EGPD_Input);
	if (FromPin && ToPin)
	{
		FromPin->MakeLinkTo(ToPin);
	}
}

void FMantleScriptGraphBuilder::SetDefault(UMantleScriptGraphNode& Node, const FString& PinName, const FString& Value)
{
	if (UEdGraphPin* Pin = Node.FindPin(*PinName, EGPD_Input))
	{
		Pin->DefaultValue = Value;
	}
}

void FMantleScriptGraphBuilder::SetVectorDefault(UMantleScriptGraphNode& Node, const FString& PinName, const FVector& Value)
{
	SetDefault(Node, PinName, Value.ToString());
}
