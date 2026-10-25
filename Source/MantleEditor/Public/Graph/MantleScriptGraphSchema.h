// Copyright Joshua Eger. All Rights Reserved.
// The UEdGraphSchema driving pin colours/shapes, the right-click "New Node" palette
// (filtered by the graph's side and, when dragging from a pin, by pin type), and connection
// rules. Palette entries for events/calls/gets/sets come straight from FMantleGraphCatalogue --
// "nothing is listed by hand" -- so a new registered API member shows up here
// automatically; entries for variables/custom events/functions come from the owning
// UMantleScriptEdGraph's own declarations.
#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraphSchema.h"
#include "MantleScriptGraphSchema.generated.h"

UCLASS()
class MANTLEEDITOR_API UMantleScriptGraphSchema : public UEdGraphSchema
{
	GENERATED_BODY()

public:
	//~ Begin UEdGraphSchema interface
	virtual void GetGraphContextActions(FGraphContextMenuBuilder& ContextMenuBuilder) const override;
	virtual const FPinConnectionResponse CanCreateConnection(const UEdGraphPin* A, const UEdGraphPin* B) const override;
	virtual FLinearColor GetPinTypeColor(const FEdGraphPinType& PinType) const override;
	virtual void GetContextMenuActions(UToolMenu* Menu, UGraphNodeContextMenuContext* Context) const override;
	//~ End UEdGraphSchema interface

private:
	/** Adds one action per FMantleGraphCatalogue entry usable on Side (event/call/get/set), plus
	 *  the fixed control-flow/expression/literal actions and one per declared variable/custom
	 *  event/function on the owning UMantleScriptEdGraph. */
	void CollectActions(const UEdGraph* Graph, const UEdGraphPin* FromPin, TArray<TSharedPtr<struct FEdGraphSchemaAction>>& Out) const;
};
