// Copyright Joshua Eger. All Rights Reserved.
// Console commands mirroring the panel's own actions, so the Scripts panel's behavior is
// testable headlessly (-ExecCmds, no GUI).

#include "Graph/MantleScriptGraphCheck.h"
#include "MantleScriptsLog.h"
#include "MantleScriptCheck.h"
#include "MantleScriptDiscovery.h"
#include "MantleScriptEditorFiles.h"
#include "MantleScriptsPanelController.h"
#include "MantleScriptTemplates.h"
#include "SMantleScriptsPanel.h"

#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"

namespace
{
	FAutoConsoleCommand GMantleScriptsPanelCommand(
		TEXT("Mantle.ScriptsPanel"),
		TEXT("Open the Mantle Scripts panel."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			FMantleScriptsPanelController::OpenPanel();
		}));

	FAutoConsoleCommand GMantleNewScriptCommand(
		TEXT("Mantle.NewScript"),
		TEXT("Mantle.NewScript <Server|Client|Shared> <Luau|TypeScript|JavaScript|Python|Graph> <Empty|KillBrick|Coins|RoundTimer|Door> <Name> -- ")
		TEXT("creates a script from a template, exactly like the New Script... dialog's Create button."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() != 4)
			{
				UE_LOG(LogMantleScripts, Error,
					TEXT("Mantle.NewScript <Server|Client|Shared> <Luau|TypeScript|JavaScript|Python|Graph> <Empty|KillBrick|Coins|RoundTimer|Door> <Name>"));
				return;
			}

			FMantleNewScriptRequest Request;
			if (!FMantleScriptTemplates::ParseSide(Args[0], Request.Side))
			{
				UE_LOG(LogMantleScripts, Error, TEXT("Mantle.NewScript: unknown side '%s' (want Server, Client or Shared)"), *Args[0]);
				return;
			}
			if (!FMantleScriptTemplates::ParseLanguage(Args[1], Request.Language))
			{
				UE_LOG(LogMantleScripts, Error, TEXT("Mantle.NewScript: unknown language '%s' (want Luau, TypeScript, JavaScript, Python or Graph)"), *Args[1]);
				return;
			}
			if (!FMantleScriptTemplates::ParseTemplate(Args[2], Request.Template))
			{
				UE_LOG(LogMantleScripts, Error, TEXT("Mantle.NewScript: unknown template '%s' (want Empty, KillBrick, Coins, RoundTimer or Door)"), *Args[2]);
				return;
			}
			Request.Name = Args[3];

			FString RelativePath;
			if (Request.Language == EMantleScriptEditorLanguage::Graph)
			{
				const FString ScriptsPackageRoot = FMantleScriptDiscovery::CurrentLevelScriptsPackageRoot();
				FString Error;
				const FString PackagePath = FMantleScriptTemplates::CreateGraphAsset(ScriptsPackageRoot, Request, Error);
				if (PackagePath.IsEmpty())
				{
					UE_LOG(LogMantleScripts, Error, TEXT("Mantle.NewScript: %s"), *Error);
					return;
				}
				UE_LOG(LogMantleScripts, Log, TEXT("Mantle.NewScript: wrote %s"), *PackagePath);
				RelativePath = FMantleScriptTemplates::SideFolderName(Request.Side) / Request.Name;
			}
			else
			{
				const FString ScriptsRoot = FMantleScriptDiscovery::CurrentLevelScriptsRoot();
				FString Error;
				const FString FullPath = FMantleScriptTemplates::CreateScriptFile(ScriptsRoot, Request, Error);
				if (FullPath.IsEmpty())
				{
					UE_LOG(LogMantleScripts, Error, TEXT("Mantle.NewScript: %s"), *Error);
					return;
				}
				UE_LOG(LogMantleScripts, Log, TEXT("Mantle.NewScript: wrote %s"), *FullPath);
				RelativePath = FMantleScriptTemplates::SideFolderName(Request.Side) / (Request.Name + TEXT(".") + FMantleScriptTemplates::FileExtension(Request.Language));
			}

			if (TSharedPtr<SMantleScriptsPanel> Panel = FMantleScriptsPanelController::GetPanel())
			{
				Panel->RefreshAndSelect(RelativePath);
			}
		}));

	FAutoConsoleCommand GMantleCheckScriptsCommand(
		TEXT("Mantle.CheckScripts"),
		TEXT("Check All: compile/parse every script under the current level's Scripts/ folder without running it, and log each result."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			// Reuse the open panel's rows/UI if it's up; otherwise check headlessly off disk so
			// the command still works without ever opening the tab (Test-Unreal.ps1 runs this way).
			if (TSharedPtr<SMantleScriptsPanel> Panel = FMantleScriptsPanelController::GetPanel())
			{
				Panel->CheckAll();
				return;
			}

			const FString ScriptsRoot = FMantleScriptDiscovery::CurrentLevelScriptsRoot();
			TArray<FMantleScriptRowPtr> Rows;
			FMantleScriptDiscovery::DiscoverAt(ScriptsRoot, Rows);

			int32 OkCount = 0, ErrorCount = 0, SkippedCount = 0;
			for (const FMantleScriptRowPtr& Row : Rows)
			{
				FString Source;
				FMantleScriptError Error;
				bool bOk = false;
				bool bSkipped = false;

				if (Row->Language == EMantleScriptEditorLanguage::Graph)
				{
					FString Message;
					bOk = FMantleScriptGraphCheck::CheckAsset(Row->AssetPackageName, Message);
					Error = FMantleScriptError{EMantleScriptErrorKind::Compile, Message};
				}
				else if (FMantleScriptCheck::IsUnsupported(Row->Language))
				{
					bSkipped = true;
					FMantleScriptCheck::Check(Row->Language, FString(), Error);
				}
				else if (!FFileHelper::LoadFileToString(Source, *Row->FullPath))
				{
					Error = FMantleScriptError{EMantleScriptErrorKind::Compile, TEXT("couldn't read file")};
				}
				else
				{
					bOk = FMantleScriptCheck::Check(Row->Language, Source, Error);
				}

				if (bSkipped)
				{
					++SkippedCount;
					UE_LOG(LogMantleScripts, Log, TEXT("Mantle.CheckScripts: SKIP  %s: %s"), *Row->RelativePath, *Error.Message);
				}
				else if (bOk)
				{
					++OkCount;
					UE_LOG(LogMantleScripts, Log, TEXT("Mantle.CheckScripts: OK    %s"), *Row->RelativePath);
				}
				else
				{
					++ErrorCount;
					UE_LOG(LogMantleScripts, Warning, TEXT("Mantle.CheckScripts: ERROR %s: %s"), *Row->RelativePath, *Error.Message);
				}
			}
			UE_LOG(LogMantleScripts, Log, TEXT("Mantle.CheckScripts: %d script(s), %d OK, %d error(s), %d skipped."),
				Rows.Num(), OkCount, ErrorCount, SkippedCount);
		}));
}
