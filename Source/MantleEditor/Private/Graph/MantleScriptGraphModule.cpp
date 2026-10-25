// Copyright Joshua Eger. All Rights Reserved.
#include "Graph/MantleScriptGraphModule.h"

#include "Graph/MantleScriptGraphActions.h"

#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Modules/ModuleManager.h"

#define LOCTEXT_NAMESPACE "MantleScriptGraph"

namespace
{
	TSharedPtr<FMantleScriptGraphActions> GMantleScriptGraphActions;
}

void FMantleScriptGraphModule::Startup()
{
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	const EAssetTypeCategories::Type MantleCategory = AssetTools.RegisterAdvancedAssetCategory(
		FName(TEXT("Mantle")), LOCTEXT("MantleAssetCategory", "Mantle"));

	GMantleScriptGraphActions = MakeShared<FMantleScriptGraphActions>(MantleCategory);
	AssetTools.RegisterAssetTypeActions(GMantleScriptGraphActions.ToSharedRef());
}

void FMantleScriptGraphModule::Shutdown()
{
	if (FModuleManager::Get().IsModuleLoaded("AssetTools"))
	{
		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
		if (GMantleScriptGraphActions.IsValid())
		{
			AssetTools.UnregisterAssetTypeActions(GMantleScriptGraphActions.ToSharedRef());
		}
	}
	GMantleScriptGraphActions.Reset();
}

#undef LOCTEXT_NAMESPACE
