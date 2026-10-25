// Copyright Joshua Eger. All Rights Reserved.
// The Content Browser asset a creator's graph lives in. Created via "Mantle > Script
// Graph" (right-click in the Content Browser, or the Scripts panel's New Script), it lives in the
// level's Scripts/Server or Scripts/Client folder -- the folder decides its side, the same rule a
// plain script file uses (FMantleScriptGraphSerializer::ClassifySide). It owns the one
// UMantleScriptEdGraph the graph editor (MantleScriptGraphEditorToolkit.h) edits; the JSON
// graph model itself is never stored separately -- it is derived on demand from the EdGraph by
// FMantleScriptGraphSerializer::BuildGraph, so there is exactly one source of truth for a saved
// graph's content (one source of truth, derived data never stored twice).
#pragma once

#include "CoreMinimal.h"
#include "MantleScriptGraphAsset.generated.h"

class UEdGraph;

UCLASS(BlueprintType)
class MANTLEEDITOR_API UMantleScriptGraph : public UObject
{
	GENERATED_BODY()

public:
	UMantleScriptGraph();

	UPROPERTY()
	TObjectPtr<UEdGraph> EdGraph;

	/** Creates EdGraph (a UMantleScriptEdGraph) if it doesn't already have one -- true for a
	 *  freshly-created asset; a loaded one already has it from serialization. Safe to call more
	 *  than once. */
	void EnsureEdGraph();
};
