// Copyright Joshua Eger. All Rights Reserved.
#include "Graph/MantleScriptGraphActions.h"

#include "Graph/MantleScriptGraphAsset.h"
#include "Graph/MantleScriptGraphEditorToolkit.h"

#define LOCTEXT_NAMESPACE "MantleScriptGraph"

FMantleScriptGraphActions::FMantleScriptGraphActions(EAssetTypeCategories::Type InAssetCategory)
	: AssetCategory(InAssetCategory)
{
}

FText FMantleScriptGraphActions::GetName() const { return LOCTEXT("AssetTypeName", "Mantle Script Graph"); }
FColor FMantleScriptGraphActions::GetTypeColor() const { return FColor(178, 15, 25); } // matches the graph's event-node red
UClass* FMantleScriptGraphActions::GetSupportedClass() const { return UMantleScriptGraph::StaticClass(); }
uint32 FMantleScriptGraphActions::GetCategories() { return AssetCategory; }

void FMantleScriptGraphActions::OpenAssetEditor(const TArray<UObject*>& InObjects, TSharedPtr<IToolkitHost> EditWithinLevelEditor)
{
	for (UObject* Obj : InObjects)
	{
		if (UMantleScriptGraph* Graph = Cast<UMantleScriptGraph>(Obj))
		{
			FMantleScriptGraphEditorToolkit::OpenGraphAsset(*Graph);
		}
	}
}

#undef LOCTEXT_NAMESPACE
