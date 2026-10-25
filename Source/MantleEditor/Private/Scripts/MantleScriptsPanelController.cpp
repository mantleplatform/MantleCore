// Copyright Joshua Eger. All Rights Reserved.
#include "MantleScriptsPanelController.h"

#include "SMantleScriptsPanel.h"

#include "Framework/Docking/TabManager.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/Docking/SDockTab.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

#define LOCTEXT_NAMESPACE "MantleEditor"

const FName FMantleScriptsPanelController::TabName(TEXT("MantleScriptsPanel"));
TWeakPtr<SMantleScriptsPanel> FMantleScriptsPanelController::PanelWidget;
FDelegateHandle FMantleScriptsPanelController::ToolMenusStartupHandle;

void FMantleScriptsPanelController::Startup()
{
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(TabName, FOnSpawnTab::CreateStatic(&FMantleScriptsPanelController::SpawnTab))
		.SetDisplayName(LOCTEXT("MantleScriptsTabTitle", "Mantle Scripts"))
		.SetTooltipText(LOCTEXT("MantleScriptsTabTooltip", "Browse, create and check this level's Scripts/ folder."))
		// Registering with a WorkspaceMenu group (rather than ETabSpawnerMenuType::Hidden) is
		// what lists this tab in the editor's menus for free: the Tools category, plus the
		// toolbar button registered below.
		.SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory());

	ToolMenusStartupHandle = UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateStatic(&FMantleScriptsPanelController::RegisterToolbar));
}

void FMantleScriptsPanelController::Shutdown()
{
	UToolMenus::UnRegisterStartupCallback(ToolMenusStartupHandle);
	if (UObjectInitialized())
	{
		UToolMenus::UnregisterOwner(TabName);
	}
	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TabName);
}

void FMantleScriptsPanelController::OpenPanel()
{
	FGlobalTabmanager::Get()->TryInvokeTab(TabName);
}

TSharedPtr<SMantleScriptsPanel> FMantleScriptsPanelController::GetPanel()
{
	return PanelWidget.Pin();
}

TSharedRef<SDockTab> FMantleScriptsPanelController::SpawnTab(const FSpawnTabArgs& Args)
{
	TSharedRef<SMantleScriptsPanel> Panel = SNew(SMantleScriptsPanel);
	PanelWidget = Panel;

	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		[
			Panel
		];
}

void FMantleScriptsPanelController::RegisterToolbar()
{
	FToolMenuOwnerScoped OwnerScoped(TabName);

	UToolMenu* ToolbarMenu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.LevelEditorToolBar.User"));
	if (!ToolbarMenu)
	{
		return;
	}
	FToolMenuSection& Section = ToolbarMenu->FindOrAddSection("MantleScripts");
	Section.AddEntry(FToolMenuEntry::InitToolBarButton(
		"OpenMantleScripts",
		FUIAction(FExecuteAction::CreateStatic(&FMantleScriptsPanelController::OpenPanel)),
		LOCTEXT("MantleScriptsToolbar", "Mantle Scripts"),
		LOCTEXT("MantleScriptsToolbarTooltip", "Open the Mantle Scripts panel"),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.OpenLevelBlueprint")));

	// This callback runs at PostEngineInit, after LevelEditorToolBar.cpp already built and
	// cached the toolbar widget once -- without this the extension above sits registered but
	// unseen until something else happens to rebuild it.
	UToolMenus::Get()->RefreshAllWidgets();
}

#undef LOCTEXT_NAMESPACE
