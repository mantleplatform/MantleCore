// Copyright Joshua Eger. All Rights Reserved.
#include "SMantleScriptsPanel.h"

#include "Graph/MantleScriptGraphAsset.h"
#include "Graph/MantleScriptGraphCheck.h"
#include "Graph/MantleScriptGraphEditorToolkit.h"

#include "Editor.h"
#include "MantleScriptsLog.h"
#include "MantleScriptCheck.h"
#include "MantleScriptDiscovery.h"
#include "MantleScriptEditorFiles.h"
#include "MantleScriptTemplates.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "SMantleNewScriptDialog.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Widgets/Views/STableViewBase.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "MantleEditor"

namespace
{
	FLinearColor LanguageColor(EMantleScriptEditorLanguage Language)
	{
		switch (Language)
		{
		case EMantleScriptEditorLanguage::Luau: return FLinearColor(0.35f, 0.55f, 1.0f);
		case EMantleScriptEditorLanguage::TypeScript: return FLinearColor(0.2f, 0.45f, 0.9f);
		case EMantleScriptEditorLanguage::JavaScript: return FLinearColor(0.92f, 0.78f, 0.15f);
		case EMantleScriptEditorLanguage::Python: return FLinearColor(0.95f, 0.6f, 0.2f);
		case EMantleScriptEditorLanguage::Graph: return FLinearColor(0.85f, 0.15f, 0.2f);
		default: return FLinearColor::White;
		}
	}

	FText StatusText(const FMantleScriptRowPtr& Row)
	{
		if (!Row.IsValid())
		{
			return FText::GetEmpty();
		}
		if (!Row->bChecked)
		{
			return LOCTEXT("NotChecked", "-");
		}
		if (Row->bCheckSkipped)
		{
			return FText::FromString(TEXT("Not checked yet: ") + Row->CheckMessage.Left(120));
		}
		return Row->bCheckOk ? LOCTEXT("Ok", "OK") : FText::FromString(TEXT("Error: ") + Row->CheckMessage.Left(160));
	}

	FSlateColor StatusColor(const FMantleScriptRowPtr& Row)
	{
		if (!Row.IsValid() || !Row->bChecked)
		{
			return FSlateColor::UseForeground();
		}
		if (Row->bCheckSkipped)
		{
			return FLinearColor(0.6f, 0.6f, 0.6f);
		}
		return Row->bCheckOk ? FLinearColor(0.4f, 0.9f, 0.4f) : FLinearColor(1.0f, 0.35f, 0.3f);
	}

	// One row of a section's list. Columns: Path / Language / Side / Status.
	class SMantleScriptTableRow : public SMultiColumnTableRow<FMantleScriptRowPtr>
	{
	public:
		SLATE_BEGIN_ARGS(SMantleScriptTableRow) {}
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& OwnerTable, FMantleScriptRowPtr InRow)
		{
			Row = InRow;
			SMultiColumnTableRow<FMantleScriptRowPtr>::Construct(FSuperRowType::FArguments(), OwnerTable);
		}

		virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& ColumnName) override
		{
			if (!Row.IsValid())
			{
				return SNullWidget::NullWidget;
			}
			if (ColumnName == TEXT("Path"))
			{
				const FString Display = Row->bModule ? Row->RelativePath + TEXT("  [module]") : Row->RelativePath;
				return SNew(SBox).Padding(FMargin(4, 2))
					[
						SNew(STextBlock).Text(FText::FromString(Display))
					];
			}
			if (ColumnName == TEXT("Language"))
			{
				return SNew(SBox).Padding(FMargin(4, 2))
					[
						SNew(STextBlock)
						.Text(FText::FromString(FMantleScriptTemplates::LanguageDisplayName(Row->Language)))
						.ColorAndOpacity(LanguageColor(Row->Language))
					];
			}
			if (ColumnName == TEXT("Side"))
			{
				return SNew(SBox).Padding(FMargin(4, 2))
					[
						SNew(STextBlock).Text(FText::FromString(FMantleScriptTemplates::SideFolderName(Row->Side)))
					];
			}
			if (ColumnName == TEXT("Status"))
			{
				return SNew(SBox).Padding(FMargin(4, 2))
					[
						SNew(STextBlock).Text(StatusText(Row)).ColorAndOpacity(StatusColor(Row))
					];
			}
			return SNullWidget::NullWidget;
		}

	private:
		FMantleScriptRowPtr Row;
	};
}

void SMantleScriptsPanel::Construct(const FArguments& InArgs)
{
	RescanDisk();

	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			BuildToolbar()
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(6, 2)
		[
			SNew(STextBlock)
			.Text(this, &SMantleScriptsPanel::GetScriptsRootText)
			.ColorAndOpacity(FLinearColor(0.6f, 0.6f, 0.6f))
		]
		+ SVerticalBox::Slot().FillHeight(1.0f)
		[
			SNew(SScrollBox)
			+ SScrollBox::Slot()
			[
				BuildSection(LOCTEXT("Server", "Server"), &ServerRows, ServerList)
			]
			+ SScrollBox::Slot()
			[
				BuildSection(LOCTEXT("Client", "Client"), &ClientRows, ClientList)
			]
			+ SScrollBox::Slot()
			[
				BuildSection(LOCTEXT("Shared", "Shared"), &SharedRows, SharedList)
			]
		]
	];

	RebuildSections();

	if (GEditor)
	{
		MapChangeHandle = FEditorDelegates::MapChange.AddSP(this, &SMantleScriptsPanel::OnMapChanged);
		MapOpenedHandle = FEditorDelegates::OnMapOpened.AddSP(this, &SMantleScriptsPanel::OnMapOpened);
	}

	RegisterActiveTimer(1.0f, FWidgetActiveTimerDelegate::CreateSP(this, &SMantleScriptsPanel::OnPollTimer));
}

SMantleScriptsPanel::~SMantleScriptsPanel()
{
	FEditorDelegates::MapChange.Remove(MapChangeHandle);
	FEditorDelegates::OnMapOpened.Remove(MapOpenedHandle);
}

TSharedRef<SWidget> SMantleScriptsPanel::BuildToolbar()
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().Padding(2)
		[
			SNew(SButton).Text(LOCTEXT("NewScript", "New Script...")).OnClicked(this, &SMantleScriptsPanel::OnNewScriptClicked)
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(2)
		[
			SNew(SButton).Text(LOCTEXT("Open", "Open")).OnClicked(this, &SMantleScriptsPanel::OnOpenSelectedClicked)
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(2)
		[
			SNew(SButton).Text(LOCTEXT("OpenFolder", "Open Scripts Folder")).OnClicked(this, &SMantleScriptsPanel::OnOpenScriptsFolderClicked)
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(2)
		[
			SNew(SButton).Text(LOCTEXT("OpenVsCode", "Open in VS Code (whole project)")).OnClicked(this, &SMantleScriptsPanel::OnOpenVsCodeWholeProjectClicked)
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(2)
		[
			SNew(SButton).Text(LOCTEXT("CheckAll", "Check All")).OnClicked(this, &SMantleScriptsPanel::OnCheckAllClicked)
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(2)
		[
			SNew(SButton).Text(LOCTEXT("RegenTypes", "Regenerate Types")).OnClicked(this, &SMantleScriptsPanel::OnRegenerateTypesClicked)
		];
}

TSharedRef<SHeaderRow> SMantleScriptsPanel::MakeHeaderRow() const
{
	return SNew(SHeaderRow)
		+ SHeaderRow::Column(TEXT("Path")).DefaultLabel(LOCTEXT("ColPath", "Path")).FillWidth(0.45f)
		+ SHeaderRow::Column(TEXT("Language")).DefaultLabel(LOCTEXT("ColLanguage", "Language")).FillWidth(0.15f)
		+ SHeaderRow::Column(TEXT("Side")).DefaultLabel(LOCTEXT("ColSide", "Side")).FillWidth(0.1f)
		+ SHeaderRow::Column(TEXT("Status")).DefaultLabel(LOCTEXT("ColStatus", "Status")).FillWidth(0.3f);
}

TSharedRef<SWidget> SMantleScriptsPanel::BuildSection(const FText& Title, TArray<FMantleScriptRowPtr>* RowsArray, TSharedPtr<SListView<FMantleScriptRowPtr>>& OutListView)
{
	SAssignNew(OutListView, SListView<FMantleScriptRowPtr>)
		.ListItemsSource(RowsArray)
		.OnGenerateRow(this, &SMantleScriptsPanel::OnGenerateRow)
		.OnMouseButtonDoubleClick(this, &SMantleScriptsPanel::OnRowDoubleClicked)
		.HeaderRow(MakeHeaderRow())
		.SelectionMode(ESelectionMode::Single);

	return SNew(SExpandableArea)
		.InitiallyCollapsed(false)
		.AreaTitle(Title)
		.BodyContent()
		[
			SNew(SBox).MinDesiredHeight(60.0f)
			[
				OutListView.ToSharedRef()
			]
		];
}

TSharedRef<ITableRow> SMantleScriptsPanel::OnGenerateRow(FMantleScriptRowPtr Row, const TSharedRef<STableViewBase>& OwnerTable)
{
	return SNew(SMantleScriptTableRow, OwnerTable, Row);
}

void SMantleScriptsPanel::RescanDisk()
{
	ScriptsRoot = FMantleScriptDiscovery::CurrentLevelScriptsRoot();
	if (!ScriptsRoot.IsEmpty() && FPaths::DirectoryExists(ScriptsRoot))
	{
		FMantleScriptEditorFiles::EnsureAll(ScriptsRoot);
	}

	TArray<FMantleScriptRowPtr> NewRows;
	FMantleScriptDiscovery::DiscoverAt(ScriptsRoot, NewRows);

	// Preserve Check All status for files that are still there (by relative path), so a disk
	// poll doesn't wipe out results the user just asked for.
	TMap<FString, FMantleScriptRowPtr> Previous;
	for (const FMantleScriptRowPtr& Old : AllRows)
	{
		Previous.Add(Old->RelativePath, Old);
	}
	for (FMantleScriptRowPtr& NewRow : NewRows)
	{
		if (const FMantleScriptRowPtr* Old = Previous.Find(NewRow->RelativePath))
		{
			NewRow->bChecked = (*Old)->bChecked;
			NewRow->bCheckOk = (*Old)->bCheckOk;
			NewRow->bCheckSkipped = (*Old)->bCheckSkipped;
			NewRow->CheckMessage = (*Old)->CheckMessage;
		}
	}
	AllRows = MoveTemp(NewRows);

	LastSeenTimestamps.Reset();
	for (const FMantleScriptRowPtr& Row : AllRows)
	{
		LastSeenTimestamps.Add(Row->FullPath, Row->Timestamp);
	}
}

void SMantleScriptsPanel::RebuildSections()
{
	ServerRows.Reset();
	ClientRows.Reset();
	SharedRows.Reset();
	for (const FMantleScriptRowPtr& Row : AllRows)
	{
		switch (Row->Side)
		{
		case EMantleSide::Server: ServerRows.Add(Row); break;
		case EMantleSide::Client: ClientRows.Add(Row); break;
		default: SharedRows.Add(Row); break;
		}
	}
	if (ServerList) ServerList->RequestListRefresh();
	if (ClientList) ClientList->RequestListRefresh();
	if (SharedList) SharedList->RequestListRefresh();
}

EActiveTimerReturnType SMantleScriptsPanel::OnPollTimer(double InCurrentTime, float InDeltaTime)
{
	// Cheap change detection: compare the set of (path, mtime) pairs seen on disk right now.
	TArray<FString> Files;
	if (!ScriptsRoot.IsEmpty())
	{
		IFileManager::Get().FindFilesRecursive(Files, *ScriptsRoot, TEXT("*.*"), true, false);
	}
	bool bChanged = (Files.Num() != LastSeenTimestamps.Num());
	if (!bChanged)
	{
		for (const FString& File : Files)
		{
			const FDateTime* Known = LastSeenTimestamps.Find(File);
			if (!Known || *Known != IFileManager::Get().GetTimeStamp(*File))
			{
				bChanged = true;
				break;
			}
		}
	}
	if (bChanged)
	{
		RescanDisk();
		RebuildSections();
	}

	return EActiveTimerReturnType::Continue;
}

void SMantleScriptsPanel::OnMapChanged(uint32 MapChangeFlags)
{
	RefreshNow();
}

void SMantleScriptsPanel::OnMapOpened(const FString& Filename, bool bAsTemplate)
{
	RefreshNow();
}

FText SMantleScriptsPanel::GetScriptsRootText() const
{
	return ScriptsRoot.IsEmpty()
		? LOCTEXT("NoLevel", "No level open under /Game, or its Scripts/ folder can't be resolved.")
		: FText::FromString(TEXT("Scripts: ") + ScriptsRoot);
}

void SMantleScriptsPanel::RefreshNow()
{
	RescanDisk();
	RebuildSections();
}

void SMantleScriptsPanel::RefreshAndSelect(const FString& RelativePath)
{
	RefreshNow();
	for (const FMantleScriptRowPtr& Row : AllRows)
	{
		if (Row->RelativePath == RelativePath)
		{
			TSharedPtr<SListView<FMantleScriptRowPtr>> List =
				Row->Side == EMantleSide::Server ? ServerList : Row->Side == EMantleSide::Client ? ClientList : SharedList;
			if (List.IsValid())
			{
				List->SetSelection(Row);
				List->RequestScrollIntoView(Row);
			}
			break;
		}
	}
}

void SMantleScriptsPanel::CheckAll()
{
	int32 OkCount = 0, ErrorCount = 0, SkippedCount = 0;
	for (const FMantleScriptRowPtr& Row : AllRows)
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
			FMantleScriptCheck::Check(Row->Language, FString(), Error); // fills in the "not yet" message
		}
		else if (!FFileHelper::LoadFileToString(Source, *Row->FullPath))
		{
			Error = FMantleScriptError{EMantleScriptErrorKind::Compile, TEXT("couldn't read file")};
		}
		else
		{
			bOk = FMantleScriptCheck::Check(Row->Language, Source, Error);
		}

		Row->bChecked = true;
		Row->bCheckOk = bOk;
		Row->bCheckSkipped = bSkipped;
		Row->CheckMessage = Error.Message;

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
		AllRows.Num(), OkCount, ErrorCount, SkippedCount);

	if (ServerList) ServerList->RequestListRefresh();
	if (ClientList) ClientList->RequestListRefresh();
	if (SharedList) SharedList->RequestListRefresh();
}

// Launches VS Code with Args. Returns false if it could not be started.
static bool LaunchVsCode(const FString& Args)
{
	uint32 ProcessId = 0;
	FProcHandle Handle = FPlatformProcess::CreateProc(TEXT("code"), *Args, true, false, false, &ProcessId, 0, nullptr, nullptr);
	if (!Handle.IsValid())
	{
		// "code" might only resolve through a shell (cmd.exe's PATH search, code.cmd shims on Windows).
		const FString ShellArgs = FString::Printf(TEXT("/c code %s"), *Args);
		Handle = FPlatformProcess::CreateProc(TEXT("cmd.exe"), *ShellArgs, true, false, false, &ProcessId, 0, nullptr, nullptr);
	}
	if (!Handle.IsValid())
	{
		return false;
	}
	FPlatformProcess::CloseProc(Handle);
	return true;
}

void SMantleScriptsPanel::OpenPath(const FString& FullPath)
{
	// Try VS Code on PATH first ("code <project folder> <file>"), so the file opens inside the
	// project's own workspace (tsconfig, luau-lsp settings, etc.) rather than as a loose file.
	const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	const FString Args = FString::Printf(TEXT("\"%s\" \"%s\""), *ProjectDir, *FullPath);
	if (!LaunchVsCode(Args))
	{
		FPlatformProcess::LaunchFileInDefaultExternalApplication(*FullPath);
	}
}

void SMantleScriptsPanel::OpenRow(const FMantleScriptRowPtr& Row)
{
	if (!Row.IsValid())
	{
		return;
	}
	if (Row->Language == EMantleScriptEditorLanguage::Graph)
	{
		if (UMantleScriptGraph* Graph = LoadObject<UMantleScriptGraph>(nullptr,
			*(Row->AssetPackageName + TEXT(".") + FPackageName::GetShortName(Row->AssetPackageName))))
		{
			FMantleScriptGraphEditorToolkit::OpenGraphAsset(*Graph);
		}
		return;
	}
	OpenPath(Row->FullPath);
}

void SMantleScriptsPanel::OnRowDoubleClicked(FMantleScriptRowPtr Row)
{
	OpenRow(Row);
}

FReply SMantleScriptsPanel::OnNewScriptClicked()
{
	SMantleNewScriptDialog::OpenDialog(ScriptsRoot, SharedThis(this));
	return FReply::Handled();
}

FReply SMantleScriptsPanel::OnOpenSelectedClicked()
{
	for (TSharedPtr<SListView<FMantleScriptRowPtr>> List : { ServerList, ClientList, SharedList })
	{
		if (!List.IsValid())
		{
			continue;
		}
		TArray<FMantleScriptRowPtr> Selected = List->GetSelectedItems();
		if (Selected.Num() > 0 && Selected[0].IsValid())
		{
			OpenRow(Selected[0]);
			return FReply::Handled();
		}
	}
	return FReply::Handled();
}

FReply SMantleScriptsPanel::OnOpenScriptsFolderClicked()
{
	if (!ScriptsRoot.IsEmpty())
	{
		IFileManager::Get().MakeDirectory(*ScriptsRoot, true);
		FPlatformProcess::ExploreFolder(*ScriptsRoot);
	}
	return FReply::Handled();
}

FReply SMantleScriptsPanel::OnOpenVsCodeWholeProjectClicked()
{
	const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	LaunchVsCode(FString::Printf(TEXT("\"%s\""), *ProjectDir));
	return FReply::Handled();
}

FReply SMantleScriptsPanel::OnCheckAllClicked()
{
	CheckAll();
	return FReply::Handled();
}

FReply SMantleScriptsPanel::OnRegenerateTypesClicked()
{
	const FString Dir = FMantleScriptEditorFiles::RegenerateTypes();
	UE_LOG(LogMantleScripts, Log, TEXT("Regenerated script type definitions into %s"), *Dir);
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
