// Copyright Joshua Eger. All Rights Reserved.
#include "MantleLibraryPanel.h"
#include "MantleCreatorSignIn.h"
#include "MantlePlaceLimits.h"
#include "MantlePlaceSettingsActor.h"
#include "MantlePlaceStorage.h"
#include "MantleRuntime.h"
#include "MantleScriptPackages.h"
#include "MantleScriptPoint.h"
#include "MantleVehicleSpawnPoint.h"
#include "MantleWeaponSpawnPoint.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/Selection.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "HttpModule.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Modules/ModuleManager.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateBrush.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "MantleLibrary"

const FName SMantleLibraryPanel::TabName(TEXT("MantleLibrary"));

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleLibraryPanelPrivate
{
	constexpr float TileWidth = 176.0f;
	constexpr float ThumbnailWidth = 160.0f;
	constexpr float ThumbnailHeight = 120.0f;

	/** The images a card shows are small; anything bigger than this is not one. */
	constexpr int64 MaxThumbnailBytes = FMantleLibraryImages::MaxBytes;
	constexpr int32 MaxThumbnailSide = FMantleLibraryImages::MaxSide;
	constexpr int32 MaxThumbnailDownloads = 4;

	TSharedRef<SDockTab> SpawnLibraryTab(const FSpawnTabArgs&)
	{
		return SNew(SDockTab)
			.TabRole(ETabRole::NomadTab)
			.Label(LOCTEXT("TabLabel", "Mantle Library"))
			[
				SNew(SMantleLibraryPanel)
			];
	}

	UWorld* EditorWorld()
	{
		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	FSlateColor ErrorColour()
	{
		return FSlateColor(FLinearColor(0.95f, 0.35f, 0.30f));
	}

	FSlateColor GoodColour()
	{
		return FSlateColor(FLinearColor(0.55f, 0.80f, 0.55f));
	}

	bool IsAsciiDigit(const TCHAR Char)
	{
		return Char >= TEXT('0') && Char <= TEXT('9');
	}

	bool IsAsciiAlnum(const TCHAR Char)
	{
		return IsAsciiDigit(Char) || (Char >= TEXT('A') && Char <= TEXT('Z')) || (Char >= TEXT('a') && Char <= TEXT('z'));
	}

	/**
	 * A script package version's manifest as the JSON a storage entry keeps (FMantleStorageSetting::Manifest, read back with
	 * FMantleScriptPackages::ReadManifestForHints): {name, description, parameters: [{name, type, default, help}]}. Empty for a
	 * manifest with nothing in it.
	 */
	FString LibraryManifestJson(const FMantleLibraryScriptManifest& Manifest)
	{
		if (Manifest.Name.IsEmpty() && Manifest.Description.IsEmpty() && Manifest.Parameters.Num() == 0 && Manifest.Settings.Num() == 0)
		{
			return FString();
		}
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("name"), Manifest.Name);
		Object->SetStringField(TEXT("description"), Manifest.Description);
		// Settings beside parameters, and each option's required, min, max and options.
		auto OptionsJson = [](const TArray<FMantleLibraryScriptParameter>& Options)
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const FMantleLibraryScriptParameter& Parameter : Options)
			{
				const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("name"), Parameter.Name);
				Entry->SetStringField(TEXT("type"), Parameter.Type);
				Entry->SetStringField(TEXT("default"), Parameter.Default);
				Entry->SetStringField(TEXT("help"), Parameter.Help);
				Entry->SetBoolField(TEXT("required"), Parameter.bRequired);
				if (Parameter.bHasMin)
				{
					Entry->SetNumberField(TEXT("min"), Parameter.Min);
				}
				if (Parameter.bHasMax)
				{
					Entry->SetNumberField(TEXT("max"), Parameter.Max);
				}
				if (Parameter.Options.Num() > 0)
				{
					TArray<TSharedPtr<FJsonValue>> Choices;
					for (const FString& Choice : Parameter.Options)
					{
						Choices.Add(MakeShared<FJsonValueString>(Choice));
					}
					Entry->SetArrayField(TEXT("options"), Choices);
				}
				Values.Add(MakeShared<FJsonValueObject>(Entry));
			}
			return Values;
		};
		Object->SetArrayField(TEXT("parameters"), OptionsJson(Manifest.Parameters));
		Object->SetArrayField(TEXT("settings"), OptionsJson(Manifest.Settings));
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Object, Writer);
		return Out;
	}

	/** The card's newest version that the detail answered, or null (a list card carries none). */
	const FMantleLibraryVersion* LatestLibraryVersion(const FMantleLibraryCard& Card)
	{
		const FMantleLibraryVersion* Latest = nullptr;
		for (const FMantleLibraryVersion& Version : Card.Versions)
		{
			if (!Latest || Version.Version > Latest->Version)
			{
				Latest = &Version;
			}
		}
		return Latest;
	}

	/**
	 * One edit of a settings actor's Storage as the Details panel makes it: recorded for undo, announced to
	 * whoever shows the property (the open Details panel), once the scope ends. Inside a FScopedTransaction.
	 */
	class FStorageEdit
	{
	public:
		explicit FStorageEdit(AMantlePlaceSettingsActor* InSettings)
			: Settings(InSettings)
			, Property(FindFProperty<FProperty>(AMantlePlaceSettingsActor::StaticClass(), GET_MEMBER_NAME_CHECKED(AMantlePlaceSettingsActor, Storage)))
		{
			Settings->Modify();
			Settings->PreEditChange(Property);
		}

		~FStorageEdit()
		{
			FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
			Settings->PostEditChangeProperty(Event);
		}

		FStorageEdit(const FStorageEdit&) = delete;
		FStorageEdit& operator=(const FStorageEdit&) = delete;

	private:
		AMantlePlaceSettingsActor* Settings;
		FProperty* Property;
	};
}

// ---------------------------------------------------------------------------------------------------------------------
// The storage rules.

FString FMantleLibraryStorage::SanitizeName(const FString& ItemName)
{
	using namespace MantleLibraryPanelPrivate;
	FString Name;
	bool bSeparator = false;
	for (const TCHAR Char : ItemName)
	{
		if (!IsAsciiAlnum(Char))
		{
			bSeparator = true;
			continue;
		}
		if (bSeparator && !Name.IsEmpty())
		{
			Name.AppendChar(TEXT('_'));
		}
		bSeparator = false;
		Name.AppendChar(Char);
	}
	if (Name.IsEmpty())
	{
		return FString(TEXT("Item"));
	}
	if (IsAsciiDigit(Name[0]))
	{
		Name = FString(TEXT("Item_")) + Name;
	}
	if (Name.Len() > MaxNameLength)
	{
		Name.LeftInline(MaxNameLength);
	}
	while (Name.EndsWith(TEXT("_")))
	{
		Name.LeftChopInline(1);
	}
	return Name;
}

FString FMantleLibraryStorage::MakeUniqueName(const FString& Base, const TArray<FMantleStorageEntry>& Entries)
{
	const auto IsTaken = [&Entries](const FString& Candidate)
	{
		return Entries.ContainsByPredicate([&Candidate](const FMantleStorageEntry& Entry)
		{
			return Entry.Name.Equals(Candidate, ESearchCase::IgnoreCase);
		});
	};
	FString Candidate = Base;
	for (int32 Number = 2; IsTaken(Candidate); ++Number)
	{
		const FString Suffix = FString::Printf(TEXT("_%d"), Number);
		Candidate = Base.Left(MaxNameLength - Suffix.Len()) + Suffix;
	}
	return Candidate;
}

int32 FMantleLibraryStorage::FindItem(const TArray<FMantleStorageEntry>& Entries, const FString& ItemId)
{
	return Entries.IndexOfByPredicate([&ItemId](const FMantleStorageEntry& Entry)
	{
		return Entry.Item.Equals(ItemId, ESearchCase::CaseSensitive);
	});
}

int32 FMantleLibraryStorage::FindUpdatable(const TArray<FMantleStorageEntry>& Entries, const FString& ItemId, const int32 LatestVersion)
{
	const int32 Index = FindItem(Entries, ItemId);
	return Index != INDEX_NONE && Entries[Index].Version < LatestVersion ? Index : INDEX_NONE;
}

bool FMantleLibraryStorage::IsUsableStatus(const FString& Status)
{
	return Status.Equals(TEXT("active"), ESearchCase::CaseSensitive) || Status.Equals(TEXT("delisted"), ESearchCase::CaseSensitive);
}

bool FMantleLibraryStorage::IsAddableStatus(const FString& Status)
{
	return Status.Equals(TEXT("active"), ESearchCase::CaseSensitive);
}

bool FMantleLibraryStorage::IsTakenDownStatus(const FString& Status)
{
	return Status.Equals(TEXT("taken_down"), ESearchCase::CaseSensitive);
}

FString FMantleLibraryStorage::TakenDownNote()
{
	return TEXT("This item was taken down by a Mantle moderator after a complaint, so a place can no longer use it.");
}

FMantleLibraryAddPlan FMantleLibraryStorage::PlanAdd(const TArray<FMantleStorageEntry>& Entries, const FString& ItemId,
	const FString& ItemName, const int32 Version, const FString& Status)
{
	FMantleLibraryAddPlan Plan;
	if (!FMantlePlaceLimits::IsLibraryItemId(ItemId))
	{
		Plan.Message = FString::Printf(TEXT("'%s' is not a library item id."), *ItemId);
		return Plan;
	}
	if (Version < 1)
	{
		Plan.Message = TEXT("This item has no published version to store yet.");
		return Plan;
	}
	const int32 Existing = FindItem(Entries, ItemId);
	if (Existing != INDEX_NONE)
	{
		Plan.Result = EMantleLibraryAdd::AlreadyStored;
		Plan.ExistingIndex = Existing;
		const FMantleStorageEntry& Stored = Entries[Existing];
		if (IsTakenDownStatus(Status))
		{
			Plan.Message = FString::Printf(TEXT("Already stored as \"%s\" at v%d. %s Remove it from storage."), *Stored.Name, Stored.Version, *TakenDownNote());
		}
		else if (Stored.Version < Version && IsAddableStatus(Status))
		{
			Plan.Message = FString::Printf(TEXT("Already stored as \"%s\" at v%d. A place stores one version of an item: use Update to v%d."),
				*Stored.Name, Stored.Version, Version);
		}
		else if (Stored.Version < Version)
		{
			Plan.Message = FString::Printf(TEXT("Already stored as \"%s\" at v%d. The item is %s, so its pin cannot be updated."),
				*Stored.Name, Stored.Version, Status.IsEmpty() ? TEXT("not available") : *Status);
		}
		else
		{
			Plan.Message = FString::Printf(TEXT("Already stored as \"%s\" at v%d."), *Stored.Name, Stored.Version);
		}
		return Plan;
	}
	if (!IsAddableStatus(Status))
	{
		if (IsTakenDownStatus(Status))
		{
			Plan.Message = TakenDownNote() + TEXT(" It cannot be added to a place.");
		}
		else if (Status.Equals(TEXT("delisted"), ESearchCase::CaseSensitive))
		{
			Plan.Message = TEXT("This item is delisted, so it can no longer be added to a place. A place that already stores it keeps working.");
		}
		else
		{
			Plan.Message = FString::Printf(TEXT("This item is %s, so a place cannot store it."), Status.IsEmpty() ? TEXT("not available") : *Status);
		}
		return Plan;
	}
	if (Entries.Num() >= FMantlePlaceLimits::MaxStorageEntries)
	{
		Plan.Result = EMantleLibraryAdd::Full;
		Plan.Message = FString::Printf(TEXT("This place already stores %d items, the most it can. Remove one first."),
			FMantlePlaceLimits::MaxStorageEntries);
		return Plan;
	}

	FMantleStorageEntry Entry;
	Entry.Name = MakeUniqueName(SanitizeName(ItemName), Entries);
	Entry.Item = ItemId;
	Entry.Version = Version;

	TArray<FMantleStorageEntry> Result = Entries;
	Result.Add(Entry);
	const TArray<FMantleStorageProblem> Problems = FMantleStorage::CheckEntries(Result);
	if (Problems.Num() > 0)
	{
		Plan.Message = FString::Printf(TEXT("The place's storage would not be valid: %s %s"), *Problems[0].Path, *Problems[0].Message);
		return Plan;
	}
	Plan.Result = EMantleLibraryAdd::Added;
	Plan.Entry = Entry;
	Plan.Message = FString::Printf(TEXT("Added as \"%s\" at v%d."), *Entry.Name, Entry.Version);
	return Plan;
}

namespace MantleLibraryPanelPrivate
{
	/** The parts of an http(s) address the image host rules look at. */
	struct FWebUrl
	{
		FString Scheme;
		FString Host;
		int32 Port = 0;
	};

	/** Reads Url's scheme, lower-cased host and port (defaulted). False for anything but a plain http(s) address. */
	bool ParseWebUrl(const FString& Url, FWebUrl& Out)
	{
		for (const TCHAR Character : Url)
		{
			if (Character <= TEXT(' ') || Character >= 0x7f || Character == TEXT('\\'))
			{
				return false;
			}
		}
		const int32 SchemeEnd = Url.Find(TEXT("://"), ESearchCase::CaseSensitive);
		if (SchemeEnd == INDEX_NONE)
		{
			return false;
		}
		Out.Scheme = Url.Left(SchemeEnd).ToLower();
		int32 DefaultPort = 0;
		if (Out.Scheme == TEXT("https"))
		{
			DefaultPort = 443;
		}
		else if (Out.Scheme == TEXT("http"))
		{
			DefaultPort = 80;
		}
		else
		{
			return false;
		}
		const FString Rest = Url.Mid(SchemeEnd + 3);
		int32 AuthorityEnd = Rest.Len();
		for (int32 Index = 0; Index < Rest.Len(); ++Index)
		{
			if (Rest[Index] == TEXT('/') || Rest[Index] == TEXT('?') || Rest[Index] == TEXT('#'))
			{
				AuthorityEnd = Index;
				break;
			}
		}
		const FString Authority = Rest.Left(AuthorityEnd);
		if (Authority.IsEmpty() || Authority.Contains(TEXT("@")))
		{
			return false;
		}

		FString HostPart = Authority;
		FString PortPart;
		if (Authority.StartsWith(TEXT("[")))
		{
			const int32 Close = Authority.Find(TEXT("]"), ESearchCase::CaseSensitive);
			if (Close == INDEX_NONE)
			{
				return false;
			}
			HostPart = Authority.Left(Close + 1);
			const FString After = Authority.Mid(Close + 1);
			if (!After.IsEmpty())
			{
				if (!After.StartsWith(TEXT(":")))
				{
					return false;
				}
				PortPart = After.Mid(1);
			}
		}
		else
		{
			int32 Colon = INDEX_NONE;
			if (Authority.FindLastChar(TEXT(':'), Colon))
			{
				HostPart = Authority.Left(Colon);
				PortPart = Authority.Mid(Colon + 1);
			}
		}
		if (HostPart.IsEmpty())
		{
			return false;
		}
		Out.Host = HostPart.ToLower();
		Out.Port = DefaultPort;
		if (!PortPart.IsEmpty())
		{
			if (PortPart.Len() > 5 || !PortPart.IsNumeric())
			{
				return false;
			}
			const int32 Port = FCString::Atoi(*PortPart);
			if (Port < 1 || Port > 65535)
			{
				return false;
			}
			Out.Port = Port;
		}
		return true;
	}

	bool HostHasSuffix(const FString& Host, const FString& Suffix)
	{
		return Suffix.Len() > 1 && Suffix.StartsWith(TEXT(".")) && Host.Len() > Suffix.Len() && Host.EndsWith(Suffix);
	}
}

bool FMantleLibraryImages::IsAllowedUrl(const FString& Url, const FString& ApiBase, const TArray<FString>& ExtraHosts)
{
	using namespace MantleLibraryPanelPrivate;
	FWebUrl Parts;
	if (!ParseWebUrl(Url.TrimStartAndEnd(), Parts))
	{
		return false;
	}

	// The API's own address: the host and port it was reached on, over https, or over http when the API itself is http.
	FWebUrl Api;
	if (ParseWebUrl(ApiBase.TrimStartAndEnd(), Api) && Parts.Host == Api.Host && Parts.Port == Api.Port
		&& (Parts.Scheme == TEXT("https") || Api.Scheme == TEXT("http")))
	{
		return true;
	}

	// An asset download host: https on the default port only.
	if (Parts.Scheme != TEXT("https") || Parts.Port != 443)
	{
		return false;
	}
	if (HostHasSuffix(Parts.Host, TEXT(".mantle-edge-gateway.workers.dev")) || HostHasSuffix(Parts.Host, TEXT(".r2.dev")))
	{
		return true;
	}
	for (const FString& Extra : ExtraHosts)
	{
		FString Entry = Extra.TrimStartAndEnd().ToLower();
		if (Entry.StartsWith(TEXT("*.")))
		{
			Entry.RightChopInline(1);
		}
		if (Entry.IsEmpty())
		{
			continue;
		}
		if (Entry.StartsWith(TEXT(".")) ? HostHasSuffix(Parts.Host, Entry) : Parts.Host == Entry)
		{
			return true;
		}
	}
	return false;
}

bool FMantleLibraryImages::ResolveUrl(const FString& ThumbnailUrl, const FString& ApiBase, FString& OutUrl, const TArray<FString>& ExtraHosts)
{
	OutUrl.Reset();
	const FString Url = ThumbnailUrl.TrimStartAndEnd();
	if (Url.IsEmpty())
	{
		return false;
	}
	FString Candidate = Url;
	if (Url.StartsWith(TEXT("/")) && !Url.StartsWith(TEXT("//")))
	{
		FString Base = ApiBase.TrimStartAndEnd();
		while (Base.EndsWith(TEXT("/")))
		{
			Base.LeftChopInline(1);
		}
		if (Base.IsEmpty())
		{
			return false;
		}
		Candidate = Base + Url;
	}
	if (!IsAllowedUrl(Candidate, ApiBase, ExtraHosts))
	{
		return false;
	}
	OutUrl = Candidate;
	return true;
}

TArray<FString> FMantleLibraryImages::ExtraHostsFromEnvironment()
{
	TArray<FString> Hosts;
	const FString Configured = FPlatformMisc::GetEnvironmentVariable(TEXT("MANTLE_LIBRARY_IMAGE_HOSTS"));
	Configured.ParseIntoArray(Hosts, TEXT(","), /*InCullEmpty*/ true);
	return Hosts;
}

bool FMantleLibraryImages::AreDimensionsAcceptable(const int64 Width, const int64 Height)
{
	return Width >= 1 && Height >= 1 && Width <= MaxSide && Height <= MaxSide;
}

// ---------------------------------------------------------------------------------------------------------------------
// The tab.

void SMantleLibraryPanel::RegisterTab()
{
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(TabName, FOnSpawnTab::CreateStatic(&MantleLibraryPanelPrivate::SpawnLibraryTab))
		.SetDisplayName(LOCTEXT("TabTitle", "Mantle Library"))
		.SetTooltipText(LOCTEXT("TabTooltip", "Find vehicles, weapons and script packages in the Mantle library and add them to this place's storage."))
		.SetMenuType(ETabSpawnerMenuType::Hidden);
}

void SMantleLibraryPanel::UnregisterTab()
{
	if (FSlateApplication::IsInitialized())
	{
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TabName);
	}
}

void SMantleLibraryPanel::OpenTab()
{
	FGlobalTabmanager::Get()->TryInvokeTab(TabName);
}

SMantleLibraryPanel::~SMantleLibraryPanel()
{
	// Answers and downloads still in flight hold weak pointers to this panel and are dropped.
}

TSharedRef<SWidget> SMantleLibraryPanel::MakeToggle(const FText& Label, TFunction<bool()> IsOn, TFunction<void()> OnPicked, const FText& Tooltip)
{
	return SNew(SCheckBox)
		.Style(&FAppStyle::Get().GetWidgetStyle<FCheckBoxStyle>("ToggleButton"))
		.ToolTipText(Tooltip)
		.IsChecked_Lambda([IsOn] { return IsOn() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
		.OnCheckStateChanged_Lambda([OnPicked](ECheckBoxState State)
		{
			if (State == ECheckBoxState::Checked)
			{
				OnPicked();
			}
		})
		[
			SNew(STextBlock)
			.Margin(FMargin(8.0f, 2.0f))
			.Text(Label)
		];
}

void SMantleLibraryPanel::Construct(const FArguments& InArgs)
{
	using namespace MantleLibraryPanelPrivate;

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(12.0f)
		[
			SNew(SSplitter)
			.Orientation(Orient_Vertical)

			// Browsing: search, filters, then the tiles.
			+ SSplitter::Slot().Value(0.68f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
				[
					SNew(STextBlock)
					.Font(FAppStyle::GetFontStyle("HeadingExtraSmall"))
					.Text(LOCTEXT("Heading", "Mantle library"))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0, 0, 6, 0)
					[
						SNew(SEditableTextBox)
						.HintText(LOCTEXT("SearchHint", "Search vehicles, weapons and script packages"))
						.OnTextChanged_Lambda([this](const FText& Text) { SearchText = Text.ToString(); })
						.OnTextCommitted_Lambda([this](const FText&, ETextCommit::Type CommitType)
						{
							if (CommitType == ETextCommit::OnEnter)
							{
								Refresh();
							}
						})
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
					[
						SNew(SButton)
						.Text(LOCTEXT("Search", "Search"))
						.OnClicked_Lambda([this] { Refresh(); return FReply::Handled(); })
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SButton)
						.Text(LOCTEXT("Refresh", "Refresh"))
						.ToolTipText(LOCTEXT("RefreshTip", "Ask Mantle again, and sign in if you are not."))
						.OnClicked_Lambda([this] { Refresh(); return FReply::Handled(); })
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 2, 0)
					[
						MakeToggle(LOCTEXT("KindAll", "All"), [this] { return Query.Kind.IsEmpty(); }, [this] { SetKind(FString()); },
							LOCTEXT("KindAllTip", "Vehicles, weapons and script packages."))
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 2, 0)
					[
						MakeToggle(LOCTEXT("KindVehicles", "Vehicles"), [this] { return Query.Kind.Equals(TEXT("vehicle"), ESearchCase::CaseSensitive); },
							[this] { SetKind(TEXT("vehicle")); }, LOCTEXT("KindVehiclesTip", "Only vehicles."))
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 2, 0)
					[
						MakeToggle(LOCTEXT("KindWeapons", "Weapons"), [this] { return Query.Kind.Equals(TEXT("weapon"), ESearchCase::CaseSensitive); },
							[this] { SetKind(TEXT("weapon")); }, LOCTEXT("KindWeaponsTip", "Only weapons."))
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 16, 0)
					[
						// Script packages, stored like any item; a Script Point anchors one in the level.
						MakeToggle(LOCTEXT("KindScripts", "Scripts"), [this] { return Query.Kind.Equals(FMantleStorage::KindScript, ESearchCase::CaseSensitive); },
							[this] { SetKind(FMantleStorage::KindScript); }, LOCTEXT("KindScriptsTip", "Only script packages: scripts another creator published, which run in your place once stored."))
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 2, 0)
					[
						MakeToggle(LOCTEXT("ScopeMine", "Mine"), [this] { return bMine; }, [this] { SetMine(true); },
							LOCTEXT("ScopeMineTip", "What you published and what you got."))
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						MakeToggle(LOCTEXT("ScopeAll", "All"), [this] { return !bMine; }, [this] { SetMine(false); },
							LOCTEXT("ScopeAllTip", "Yours, and other creators' public items while Mantle lists them."))
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.Visibility_Lambda([this] { return bLoading || !ListError.IsEmpty() || (bLoaded && Cards.IsEmpty()) ? EVisibility::Visible : EVisibility::Collapsed; })
					.ColorAndOpacity_Lambda([this] { return ListError.IsEmpty() ? FSlateColor::UseSubduedForeground() : ErrorColour(); })
					.Text_Lambda([this]
					{
						if (!ListError.IsEmpty())
						{
							return FText::FromString(ListError);
						}
						return bLoading ? LOCTEXT("Loading", "Loading...") : LOCTEXT("NoItems", "No items found.");
					})
				]
				+ SVerticalBox::Slot().FillHeight(1.0f)
				[
					SNew(SScrollBox)
					+ SScrollBox::Slot()
					[
						SAssignNew(TilesBox, SWrapBox)
						.UseAllottedSize(true)
						.InnerSlotPadding(FVector2D(8.0f, 8.0f))
					]
					+ SScrollBox::Slot().Padding(0, 8, 0, 0)
					[
						SNew(SButton)
						.Visibility_Lambda([this] { return !NextCursor.IsEmpty() && !bLoading ? EVisibility::Visible : EVisibility::Collapsed; })
						.HAlign(HAlign_Center)
						.Text(LOCTEXT("LoadMore", "Load more"))
						.OnClicked_Lambda([this] { LoadMore(); return FReply::Handled(); })
					]
				]
			]

			// This place's storage.
			+ SSplitter::Slot().Value(0.32f)
			[
				MakeStorageSection()
			]
		]
	];

	RefreshStorageSnapshot();
	RebuildStorage();
	RegisterActiveTimer(0.5f, FWidgetActiveTimerDelegate::CreateSP(this, &SMantleLibraryPanel::TickStorage));

	// Open on what the creator can use, without asking to sign in just for opening the tab.
	if (FMantleCreatorSignIn::IsSignedIn())
	{
		RequestPage(FString(), false);
	}
	else
	{
		bLoaded = true;
		ListError = TEXT("Sign in to browse the library: press Refresh.");
		RebuildTiles();
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// Browsing.

void SMantleLibraryPanel::Refresh()
{
	if (!FMantleCreatorSignIn::EnsureSignedIn(LOCTEXT("SignInReason", "Sign in to browse the Mantle library.")))
	{
		bLoaded = true;
		ListError = TEXT("Sign in to browse the library: press Refresh.");
		RebuildTiles();
		return;
	}
	LookupFailed.Reset();
	RequestPage(FString(), false);
	RebuildStorage();
}

void SMantleLibraryPanel::LoadMore()
{
	if (!bLoading && !NextCursor.IsEmpty())
	{
		RequestPage(NextCursor, true);
	}
}

void SMantleLibraryPanel::SetKind(const FString& Kind)
{
	if (!Query.Kind.Equals(Kind, ESearchCase::CaseSensitive))
	{
		Query.Kind = Kind;
		Refresh();
	}
}

void SMantleLibraryPanel::SetMine(const bool bInMine)
{
	if (bMine != bInMine)
	{
		bMine = bInMine;
		Refresh();
	}
}

void SMantleLibraryPanel::RequestPage(const FString& Cursor, const bool bAppend)
{
	++RequestSerial;
	const int32 Serial = RequestSerial;
	bLoading = true;
	ListError.Reset();
	if (!bAppend)
	{
		Cards.Reset();
		NextCursor.Reset();
	}
	RebuildTiles();

	FMantleLibraryQuery Asked = Query;
	Asked.Search = SearchText;
	const TWeakPtr<SMantleLibraryPanel> WeakThis = SharedThis(this);
	FMantleApiClient::ListLibraryItemsAsync(Asked, bMine, Cursor,
		FMantleApiCallback::CreateLambda([WeakThis, Serial, bAppend](const FMantleApiResult& Result)
		{
			if (const TSharedPtr<SMantleLibraryPanel> Panel = WeakThis.Pin())
			{
				Panel->OnPageAnswer(Result, Serial, bAppend);
			}
		}));
}

void SMantleLibraryPanel::OnPageAnswer(const FMantleApiResult& Result, const int32 Serial, const bool bAppend)
{
	if (Serial != RequestSerial)
	{
		return;
	}
	bLoading = false;
	bLoaded = true;
	if (!Result.bSuccess)
	{
		ListError = Result.Message.IsEmpty() ? FString(TEXT("Mantle could not list the library.")) : Result.Message;
		RebuildTiles();
		return;
	}
	TArray<FMantleLibraryCard> Page;
	FString Next;
	FString Error;
	if (!FMantleApiClient::ParseLibraryCards(Result.Body, Page, Next, Error))
	{
		ListError = Error;
		RebuildTiles();
		return;
	}
	if (!bAppend)
	{
		Cards.Reset();
	}
	TArray<FString> PackagesToLookUp;
	for (FMantleLibraryCard& Card : Page)
	{
		// A list card carries no versions, so a script package's files (its tile's count, its manifest) come from its detail.
		const FMantleLibraryCard* Seen = Known.Find(Card.Id);
		if (Seen && Seen->Versions.Num() > 0 && Card.Versions.Num() == 0)
		{
			Card.Versions = Seen->Versions;
		}
		if (Card.Kind.Equals(FMantleStorage::KindScript, ESearchCase::CaseSensitive) && Card.Versions.Num() == 0)
		{
			PackagesToLookUp.Add(Card.Id);
		}
		Remember(Card);
		const bool bSeen = Cards.ContainsByPredicate([&Card](const FMantleLibraryCard& Other)
		{
			return Other.Id.Equals(Card.Id, ESearchCase::CaseSensitive);
		});
		if (!bSeen)
		{
			Cards.Add(MoveTemp(Card));
		}
	}
	for (const FString& ItemId : PackagesToLookUp)
	{
		LookUpItem(ItemId);
	}
	NextCursor = Next;
	RebuildStorage();
	RebuildTiles();
}

void SMantleLibraryPanel::Remember(const FMantleLibraryCard& Card)
{
	Known.Add(Card.Id, Card);
	// A stored entry learns what its item is, and a script package's entry its pinned manifest.
	StoreManifestFor(Card.Id);
}

void SMantleLibraryPanel::RebuildTiles()
{
	if (!TilesBox.IsValid())
	{
		return;
	}
	TilesBox->ClearChildren();
	for (const FMantleLibraryCard& Card : Cards)
	{
		TilesBox->AddSlot()
		[
			BuildTile(Card)
		];
	}
}

TSharedRef<SWidget> SMantleLibraryPanel::BuildTile(const FMantleLibraryCard& Card)
{
	using namespace MantleLibraryPanelPrivate;

	const FString ThumbnailUrl = Card.ThumbnailUrl;
	if (!ThumbnailUrl.IsEmpty())
	{
		RequestThumbnail(ThumbnailUrl);
	}

	const bool bIsWeapon = Card.Kind.Equals(TEXT("weapon"), ESearchCase::CaseSensitive);
	const bool bIsVehicle = Card.Kind.Equals(TEXT("vehicle"), ESearchCase::CaseSensitive);
	const bool bIsScript = Card.Kind.Equals(FMantleStorage::KindScript, ESearchCase::CaseSensitive);
	FString KindWord(TEXT("Item"));
	if (bIsWeapon)
	{
		KindWord = TEXT("Weapon");
	}
	else if (bIsVehicle)
	{
		KindWord = TEXT("Vehicle");
	}
	else if (bIsScript)
	{
		// The letter S, and what the package holds once its detail has answered.
		KindWord = TEXT("Script package");
		if (const FMantleLibraryVersion* Latest = LatestLibraryVersion(Card))
		{
			KindWord = FString::Printf(TEXT("Script package, %d file%s"), Latest->Scripts.Num(), Latest->Scripts.Num() == 1 ? TEXT("") : TEXT("s"));
		}
	}
	else if (!Card.Kind.IsEmpty())
	{
		KindWord = Card.Kind.Left(1).ToUpper() + Card.Kind.Mid(1);
	}
	const FLinearColor PlaceholderColour = bIsWeapon ? FLinearColor(0.26f, 0.13f, 0.11f)
		: bIsVehicle ? FLinearColor(0.10f, 0.17f, 0.27f)
		: bIsScript ? FLinearColor(0.12f, 0.22f, 0.14f) : FLinearColor(0.15f, 0.15f, 0.15f);
	// A package's tooltip lists its files (another creator's code: what is about to run in your place).
	FString Tooltip = Card.Description.IsEmpty() ? Card.Name : Card.Description;
	if (bIsScript)
	{
		if (const FMantleLibraryVersion* Latest = LatestLibraryVersion(Card))
		{
			TArray<FString> Paths;
			for (const FMantleLibraryScriptFile& File : Latest->Scripts)
			{
				Paths.Add(FString::Printf(TEXT("  %s (%s)"), *File.Path, *File.Language));
			}
			Tooltip += FString::Printf(TEXT("\n\nFiles in v%d:\n%s"), Latest->Version, *FString::Join(Paths, TEXT("\n")));
			if (Latest->Manifest.Parameters.Num() > 0)
			{
				TArray<FString> Parameters;
				for (const FMantleLibraryScriptParameter& Parameter : Latest->Manifest.Parameters)
				{
					Parameters.Add(FString::Printf(TEXT("  %s (%s)%s%s"), *Parameter.Name, *Parameter.Type,
						Parameter.Help.IsEmpty() ? TEXT("") : TEXT(": "), *Parameter.Help));
				}
				Tooltip += FString::Printf(TEXT("\n\nScript Point parameters:\n%s"), *FString::Join(Parameters, TEXT("\n")));
			}
		}
	}

	// The artwork: a neutral placeholder with the kind's letter, and the thumbnail over it once it has downloaded.
	const TSharedRef<SOverlay> Art = SNew(SOverlay)
		+ SOverlay::Slot()
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor(PlaceholderColour)
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
				[
					SNew(STextBlock)
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 40))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.Text(FText::FromString(KindWord.Left(1)))
				]
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
				[
					SNew(STextBlock)
					.Font(FAppStyle::GetFontStyle("SmallFont"))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.Text(FText::FromString(KindWord))
				]
			]
		]
		+ SOverlay::Slot()
		[
			SNew(SScaleBox)
			.Stretch(EStretch::ScaleToFit)
			.Visibility_Lambda([this, ThumbnailUrl] { return FindThumbnail(ThumbnailUrl) ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
			[
				SNew(SImage)
				.Image_Lambda([this, ThumbnailUrl] { return FindThumbnail(ThumbnailUrl); })
			]
		];
	if (Card.IsPrivate())
	{
		Art->AddSlot().HAlign(HAlign_Right).VAlign(VAlign_Top).Padding(4.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor(FLinearColor(0.85f, 0.55f, 0.10f, 0.95f))
			.Padding(FMargin(5.0f, 1.0f))
			.ToolTipText(LOCTEXT("PrivateTip", "Private: only you can use it."))
			[
				SNew(STextBlock)
				.Font(FAppStyle::GetFontStyle("SmallFont"))
				.ColorAndOpacity(FLinearColor::Black)
				.Text(LOCTEXT("Private", "Private"))
			]
		];
	}

	if (FMantleLibraryStorage::IsTakenDownStatus(Card.Status))
	{
		Art->AddSlot().HAlign(HAlign_Left).VAlign(VAlign_Top).Padding(4.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor(FLinearColor(0.80f, 0.15f, 0.15f, 0.95f))
			.Padding(FMargin(5.0f, 1.0f))
			.ToolTipText(FText::FromString(FMantleLibraryStorage::TakenDownNote()))
			[
				SNew(STextBlock)
				.Font(FAppStyle::GetFontStyle("SmallFont"))
				.ColorAndOpacity(FLinearColor::White)
				.Text(LOCTEXT("TakenDown", "Taken down"))
			]
		];
	}

	const FString CreatorName = Card.CreatorName.IsEmpty() ? FString(TEXT("unknown")) : Card.CreatorName;
	const FString Byline = FString::Printf(TEXT("by %s%s  -  v%d%s"), *CreatorName, Card.bCreatorOfficial ? TEXT(" (official)") : TEXT(""), Card.LatestVersion,
		Card.Status.Equals(TEXT("delisted"), ESearchCase::CaseSensitive) ? TEXT("  -  delisted")
			: (FMantleLibraryStorage::IsTakenDownStatus(Card.Status) ? TEXT("  -  taken down") : TEXT("")));

	const TSharedRef<SVerticalBox> Column = SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SBox).WidthOverride(ThumbnailWidth).HeightOverride(ThumbnailHeight)
			[
				Art
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
		[
			SNew(STextBlock)
			.Font(FAppStyle::GetFontStyle("NormalFontBold"))
			.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			.ToolTipText(FText::FromString(Tooltip))
			.Text(FText::FromString(Card.Name.IsEmpty() ? FString(TEXT("(unnamed)")) : Card.Name))
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(STextBlock)
			.Font(FAppStyle::GetFontStyle("SmallFont"))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			.Text(FText::FromString(Byline))
		];

	if (const FNote* Note = TileNotes.Find(Card.Id))
	{
		Column->AddSlot().AutoHeight().Padding(0, 4, 0, 0)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.Font(FAppStyle::GetFontStyle("SmallFont"))
			.ColorAndOpacity(Note->bError ? ErrorColour() : GoodColour())
			.Text(FText::FromString(Note->Text))
		];
	}

	const bool bBusy = TileBusy.Contains(Card.Id);
	const bool bCanUse = Card.CanUse();
	const int32 StoredIndex = FMantleLibraryStorage::FindItem(StorageSnapshot, Card.Id);
	const bool bUpdatable = FMantleLibraryStorage::FindUpdatable(StorageSnapshot, Card.Id, Card.LatestVersion) != INDEX_NONE;

	FText AddLabel = LOCTEXT("AddToStorage", "Add to storage");
	FText AddTip = LOCTEXT("AddToStorageTip", "Add this item to this place's storage, pinned at its latest version.");
	bool bAddEnabled = bCanUse && !bBusy;
	if (!bCanUse)
	{
		AddTip = LOCTEXT("AddNeedsGet", "Get this item first.");
	}
	else if (Card.LatestVersion < 1)
	{
		bAddEnabled = false;
		AddTip = LOCTEXT("AddNoVersion", "This item has no published version yet.");
	}
	else if (FMantleLibraryStorage::IsTakenDownStatus(Card.Status))
	{
		// Neither Add nor Update: a takedown overrides every pin. A stored copy stays listed under Storage, with the note.
		bAddEnabled = false;
		AddTip = FText::FromString(FMantleLibraryStorage::TakenDownNote());
	}
	else if (!FMantleLibraryStorage::IsUsableStatus(Card.Status))
	{
		bAddEnabled = false;
		AddTip = FText::Format(LOCTEXT("AddNotActive", "This item is {0}, so a place cannot store it."), FText::FromString(Card.Status));
	}
	else if (!FMantleLibraryStorage::IsAddableStatus(Card.Status) && (bUpdatable || StoredIndex == INDEX_NONE))
	{
		// Delisted: a place that already stores it keeps working, but a new add or a re-pin cannot be published.
		bAddEnabled = false;
		AddTip = bUpdatable
			? LOCTEXT("UpdateDelisted", "This item is delisted, so the pin of a place that stores it cannot be updated.")
			: LOCTEXT("AddDelisted", "This item is delisted, so it can no longer be added to a place.");
	}
	else if (bUpdatable)
	{
		AddLabel = FText::Format(LOCTEXT("UpdateTo", "Update to v{0}"), FText::AsNumber(Card.LatestVersion));
		AddTip = LOCTEXT("UpdateToTip", "This place stores an older version of the item. Pin the latest instead.");
	}
	else if (StoredIndex != INDEX_NONE)
	{
		bAddEnabled = false;
		AddLabel = LOCTEXT("InStorage", "In storage");
		AddTip = LOCTEXT("InStorageTip", "This place already stores the item's latest version.");
	}

	const TSharedRef<SHorizontalBox> Buttons = SNew(SHorizontalBox);
	if (!bCanUse)
	{
		Buttons->AddSlot().AutoWidth().Padding(0, 0, 4, 0)
		[
			SNew(SButton)
			.Text(LOCTEXT("Get", "Get"))
			.ToolTipText(LOCTEXT("GetTip", "Add this public item to your library so you can store it in a place."))
			.IsEnabled(!bBusy)
			.OnClicked_Lambda([this, Card] { return OnGetClicked(Card); })
		];
	}
	Buttons->AddSlot().AutoWidth()
	[
		SNew(SButton)
		.Text(AddLabel)
		.ToolTipText(AddTip)
		.IsEnabled(bAddEnabled)
		.OnClicked_Lambda([this, Card] { return OnAddClicked(Card); })
	];
	Column->AddSlot().AutoHeight().Padding(0, 6, 0, 0)
	[
		Buttons
	];

	return SNew(SBox).WidthOverride(TileWidth)
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
		.Padding(8.0f)
		[
			Column
		]
	];
}

// ---------------------------------------------------------------------------------------------------------------------
// Actions on a card.

void SMantleLibraryPanel::SetTileNote(const FString& ItemId, const FString& Text, const bool bError)
{
	FNote Note;
	Note.Text = Text;
	Note.bError = bError;
	TileNotes.Add(ItemId, MoveTemp(Note));
	RebuildTiles();
}

FReply SMantleLibraryPanel::OnGetClicked(FMantleLibraryCard Card)
{
	if (TileBusy.Contains(Card.Id))
	{
		return FReply::Handled();
	}
	if (!FMantleCreatorSignIn::EnsureSignedIn(LOCTEXT("GetSignInReason", "Sign in to get items from the Mantle library.")))
	{
		SetTileNote(Card.Id, TEXT("Sign in to get this item."), true);
		return FReply::Handled();
	}
	TileBusy.Add(Card.Id);
	SetTileNote(Card.Id, TEXT("Getting..."), false);

	const FString ItemId = Card.Id;
	const TWeakPtr<SMantleLibraryPanel> WeakThis = SharedThis(this);
	FMantleApiClient::AcquireLibraryItemAsync(ItemId, FMantleApiCallback::CreateLambda([WeakThis, ItemId](const FMantleApiResult& Acquired)
	{
		const TSharedPtr<SMantleLibraryPanel> Panel = WeakThis.Pin();
		if (!Panel.IsValid())
		{
			return;
		}
		if (!Acquired.bSuccess)
		{
			Panel->TileBusy.Remove(ItemId);
			Panel->SetTileNote(ItemId, Acquired.Message.IsEmpty() ? FString(TEXT("Mantle could not add the item to your library.")) : Acquired.Message, true);
			return;
		}
		// Got. Read the item again so the tile shows what the API now says (acquired, usable).
		FMantleApiClient::GetLibraryItemAsync(ItemId, FMantleApiCallback::CreateLambda([WeakThis, ItemId](const FMantleApiResult& Detail)
		{
			if (const TSharedPtr<SMantleLibraryPanel> Inner = WeakThis.Pin())
			{
				Inner->FinishGet(ItemId, Detail);
			}
		}));
	}));
	return FReply::Handled();
}

void SMantleLibraryPanel::FinishGet(const FString& ItemId, const FMantleApiResult& Detail)
{
	TileBusy.Remove(ItemId);
	FMantleLibraryCard Fresh;
	FString Error;
	const bool bRead = Detail.bSuccess && FMantleApiClient::ParseLibraryItem(Detail.Body, Fresh, Error);
	for (FMantleLibraryCard& Card : Cards)
	{
		if (!Card.Id.Equals(ItemId, ESearchCase::CaseSensitive))
		{
			continue;
		}
		if (bRead)
		{
			if (Fresh.ThumbnailUrl.IsEmpty())
			{
				Fresh.ThumbnailUrl = Card.ThumbnailUrl;
			}
			Card = Fresh;
		}
		// The item is in the caller's library now whatever the second read said.
		Card.bAcquired = true;
		Remember(Card);
	}
	SetTileNote(ItemId, TEXT("Got it. You can add it to storage now."), false);
}

FReply SMantleLibraryPanel::OnAddClicked(FMantleLibraryCard Card)
{
	FString Message;
	const bool bUpdate = FMantleLibraryStorage::FindUpdatable(StorageSnapshot, Card.Id, Card.LatestVersion) != INDEX_NONE;
	Remember(Card);
	const bool bDone = bUpdate ? ApplyUpdate(Card.Id, Card.LatestVersion, Message) : ApplyAdd(Card, Message);
	RefreshStorageSnapshot();
	StorageNote.Text = Message;
	StorageNote.bError = !bDone;
	RebuildStorage();
	SetTileNote(Card.Id, Message, !bDone);
	return FReply::Handled();
}

// ---------------------------------------------------------------------------------------------------------------------
// Edits to the level's storage. Each is one undoable step on the level's Mantle Place Settings.

bool SMantleLibraryPanel::ApplyAdd(const FMantleLibraryCard& Card, FString& OutMessage)
{
	using namespace MantleLibraryPanelPrivate;
	UWorld* World = EditorWorld();
	if (!World)
	{
		OutMessage = TEXT("Open a level first: storage belongs to the level.");
		return false;
	}
	if (!Card.CanUse())
	{
		OutMessage = TEXT("Get this item first.");
		return false;
	}
	if (FMantleLibraryStorage::IsTakenDownStatus(Card.Status))
	{
		OutMessage = FMantleLibraryStorage::TakenDownNote() + TEXT(" It cannot be added to a place.");
		return false;
	}
	if (!FMantleLibraryStorage::IsUsableStatus(Card.Status))
	{
		OutMessage = FString::Printf(TEXT("This item is %s, so a place cannot store it."), *Card.Status);
		return false;
	}
	TArray<FMantleStorageEntry> Entries;
	AMantlePlaceSettingsActor::ApplyStorageTo(World, Entries);
	const FMantleLibraryAddPlan Plan = FMantleLibraryStorage::PlanAdd(Entries, Card.Id, Card.Name, Card.LatestVersion, Card.Status);
	if (Plan.Result != EMantleLibraryAdd::Added)
	{
		OutMessage = Plan.Message;
		return false;
	}

	FScopedTransaction Transaction(LOCTEXT("AddToStorageUndo", "Add library item to place storage"));
	AMantlePlaceSettingsActor* Settings = AMantlePlaceSettingsActor::FindOrAdd(World);
	if (!Settings)
	{
		Transaction.Cancel();
		OutMessage = TEXT("Could not add Mantle Place Settings to this level.");
		return false;
	}
	{
		FStorageEdit Edit(Settings);
		FMantleStorageSetting Setting;
		Setting.Name = Plan.Entry.Name;
		Setting.Item = Plan.Entry.Item;
		Setting.Version = Plan.Entry.Version;
		// What the item is, so a Script Point offers only script packages; a package's manifest once its detail is known.
		Setting.Kind = Card.Kind;
		if (Card.Kind.Equals(FMantleStorage::KindScript, ESearchCase::CaseSensitive))
		{
			const FMantleLibraryCard* Detail = Known.Find(Card.Id);
			const TArray<FMantleLibraryVersion>& Versions = Detail && Detail->Versions.Num() > 0 ? Detail->Versions : Card.Versions;
			if (const FMantleLibraryVersion* Pinned = Versions.FindByPredicate([&Plan](const FMantleLibraryVersion& Version) { return Version.Version == Plan.Entry.Version; }))
			{
				Setting.Manifest = LibraryManifestJson(Pinned->Manifest);
			}
		}
		Settings->Storage.Add(MoveTemp(Setting));
	}
	if (Card.Kind.Equals(FMantleStorage::KindScript, ESearchCase::CaseSensitive))
	{
		// The manifest arrives with the item's detail when the list did not carry it (FinishLookup stores it).
		const FMantleLibraryCard* Detail = Known.Find(Card.Id);
		if (!Detail || Detail->Versions.Num() == 0)
		{
			LookupFailed.Remove(Card.Id);
			LookUpItem(Card.Id);
		}
		OutMessage = Plan.Message + FString::Printf(TEXT(" It runs in your place now; put a Script Point (Tools > Mantle > Add Script Point) where it should act, or require(\"@%s/<module>\") from a script."), *Plan.Entry.Name);
		return true;
	}
	OutMessage = Plan.Message + TEXT(" Use the name in a Vehicle Spawn's Storage Item, or Storage:Get in a script.");
	return true;
}

bool SMantleLibraryPanel::ApplyUpdate(const FString& ItemId, const int32 Version, FString& OutMessage)
{
	using namespace MantleLibraryPanelPrivate;
	UWorld* World = EditorWorld();
	AMantlePlaceSettingsActor* Settings = AMantlePlaceSettingsActor::Find(World);
	if (!Settings)
	{
		OutMessage = TEXT("This level has no stored items.");
		return false;
	}
	const int32 Index = Settings->Storage.IndexOfByPredicate([&ItemId](const FMantleStorageSetting& Setting)
	{
		return Setting.Item.Equals(ItemId, ESearchCase::CaseSensitive);
	});
	if (Index == INDEX_NONE)
	{
		OutMessage = TEXT("This place does not store that item.");
		return false;
	}
	if (Version < 1 || Settings->Storage[Index].Version >= Version)
	{
		OutMessage = FString::Printf(TEXT("\"%s\" is already at v%d."), *Settings->Storage[Index].Name, Settings->Storage[Index].Version);
		return false;
	}
	// A delisted item stays stored at its pin, but the backend refuses to publish a place that re-pins it.
	if (const FMantleLibraryCard* Item = Known.Find(ItemId))
	{
		if (FMantleLibraryStorage::IsTakenDownStatus(Item->Status))
		{
			OutMessage = FString::Printf(TEXT("\"%s\" cannot be updated. %s"), *Settings->Storage[Index].Name, *FMantleLibraryStorage::TakenDownNote());
			return false;
		}
		if (!FMantleLibraryStorage::IsAddableStatus(Item->Status))
		{
			OutMessage = FString::Printf(TEXT("\"%s\" is %s, so its pin cannot be updated."), *Settings->Storage[Index].Name, *Item->Status);
			return false;
		}
	}
	const FScopedTransaction Transaction(LOCTEXT("UpdateStorageUndo", "Update stored library item"));
	FStorageEdit Edit(Settings);
	Settings->Storage[Index].Version = Version;
	// A script package's entry takes the new version's manifest, now or once its detail answers.
	StoreManifestFor(ItemId);
	if (const FMantleLibraryCard* Item = Known.Find(ItemId); Item && Item->Kind.Equals(FMantleStorage::KindScript, ESearchCase::CaseSensitive) && Item->Versions.Num() == 0)
	{
		LookupFailed.Remove(ItemId);
		LookUpItem(ItemId);
	}
	OutMessage = FString::Printf(TEXT("\"%s\" now pinned at v%d."), *Settings->Storage[Index].Name, Version);
	return true;
}

bool SMantleLibraryPanel::ApplyRemove(const FString& EntryName, FString& OutMessage)
{
	using namespace MantleLibraryPanelPrivate;
	UWorld* World = EditorWorld();
	AMantlePlaceSettingsActor* Settings = AMantlePlaceSettingsActor::Find(World);
	if (!Settings)
	{
		OutMessage = TEXT("This level has no stored items.");
		return false;
	}
	const int32 Index = Settings->Storage.IndexOfByPredicate([&EntryName](const FMantleStorageSetting& Setting)
	{
		return Setting.Name.Equals(EntryName, ESearchCase::CaseSensitive);
	});
	if (Index == INDEX_NONE)
	{
		OutMessage = TEXT("That entry is already gone.");
		return false;
	}
	const int32 Using = CountSpawnsUsing(EntryName);
	const FScopedTransaction Transaction(LOCTEXT("RemoveStorageUndo", "Remove item from place storage"));
	FStorageEdit Edit(Settings);
	Settings->Storage.RemoveAt(Index);
	OutMessage = FString::Printf(TEXT("Removed \"%s\"."), *EntryName);
	if (Using > 0)
	{
		OutMessage += FString::Printf(TEXT(" %d spawn(s) or Script Point(s) still name it: change them before you publish."), Using);
	}
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// This place's storage.

TSharedRef<SWidget> SMantleLibraryPanel::MakeStorageSection()
{
	using namespace MantleLibraryPanelPrivate;
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 4)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Font(FAppStyle::GetFontStyle("NormalFontBold"))
				.Text_Lambda([this]
				{
					return FText::Format(LOCTEXT("StorageHeading", "This place's storage ({0} of {1})"),
						FText::AsNumber(StorageSnapshot.Num()), FText::AsNumber(FMantlePlaceLimits::MaxStorageEntries));
				})
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton)
				.Text(LOCTEXT("SelectSettings", "Select Place Settings"))
				.ToolTipText(LOCTEXT("SelectSettingsTip", "Select the level's Mantle Place Settings: its Details panel lists Storage next to Scripts."))
				.OnClicked(this, &SMantleLibraryPanel::OnSelectPlaceSettings)
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.Visibility_Lambda([this] { return StorageNote.Text.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
			.ColorAndOpacity_Lambda([this] { return StorageNote.bError ? ErrorColour() : GoodColour(); })
			.Text_Lambda([this] { return FText::FromString(StorageNote.Text); })
		]
		+ SVerticalBox::Slot().FillHeight(1.0f)
		[
			SNew(SScrollBox)
			+ SScrollBox::Slot()
			[
				SAssignNew(StorageBox, SVerticalBox)
			]
		];
}

FString SMantleLibraryPanel::Signature(const TArray<FMantleStorageEntry>& Entries)
{
	FString Result;
	for (const FMantleStorageEntry& Entry : Entries)
	{
		Result += FString::Printf(TEXT("%s|%s|%d;"), *Entry.Name, *Entry.Item, Entry.Version);
	}
	return Result;
}

void SMantleLibraryPanel::RefreshStorageSnapshot()
{
	UWorld* World = MantleLibraryPanelPrivate::EditorWorld();
	// Finding the actor walks the level's actors: keep the last one while it is still in this world.
	AMantlePlaceSettingsActor* Settings = SettingsActor.Get();
	if (!Settings || !IsValid(Settings) || Settings->GetWorld() != World)
	{
		Settings = AMantlePlaceSettingsActor::Find(World);
		SettingsActor = Settings;
	}
	StorageSnapshot.Reset();
	if (Settings)
	{
		for (const FMantleStorageSetting& Setting : Settings->Storage)
		{
			FMantleStorageEntry Stored;
			Stored.Name = Setting.Name;
			Stored.Item = Setting.Item;
			Stored.Version = Setting.Version;
			StorageSnapshot.Add(MoveTemp(Stored));
		}
	}
	bStorageHasSettings = Settings != nullptr;
	StorageSignature = Signature(StorageSnapshot) + (bStorageHasSettings ? TEXT("+") : TEXT("-"));
}

EActiveTimerReturnType SMantleLibraryPanel::TickStorage(double InCurrentTime, float InDeltaTime)
{
	const FString Before = StorageSignature;
	RefreshStorageSnapshot();
	if (!Before.Equals(StorageSignature, ESearchCase::CaseSensitive))
	{
		// Changed in the Details panel, by undo, or by opening another level.
		RebuildStorage();
		RebuildTiles();
	}
	return EActiveTimerReturnType::Continue;
}

int32 SMantleLibraryPanel::CountSpawnsUsing(const FString& EntryName) const
{
	UWorld* World = MantleLibraryPanelPrivate::EditorWorld();
	int32 Count = 0;
	if (World)
	{
		for (TActorIterator<AMantleVehicleSpawnPoint> It(World); It; ++It)
		{
			if (It->StorageItem.Equals(EntryName, ESearchCase::CaseSensitive))
			{
				++Count;
			}
		}
		// A Weapon Spawn or a Script Point uses an entry the same way.
		for (TActorIterator<AMantleWeaponSpawnPoint> It(World); It; ++It)
		{
			if (It->StorageItem.Equals(EntryName, ESearchCase::CaseSensitive))
			{
				++Count;
			}
		}
		for (TActorIterator<AMantleScriptPoint> It(World); It; ++It)
		{
			if (It->StorageItem.Equals(EntryName, ESearchCase::CaseSensitive))
			{
				++Count;
			}
		}
	}
	return Count;
}

void SMantleLibraryPanel::LookUpItem(const FString& ItemId)
{
	if (!FMantleCreatorSignIn::IsSignedIn() || LookupPending.Contains(ItemId) || LookupFailed.Contains(ItemId))
	{
		return;
	}
	LookupPending.Add(ItemId);
	const TWeakPtr<SMantleLibraryPanel> WeakThis = SharedThis(this);
	FMantleApiClient::GetLibraryItemAsync(ItemId, FMantleApiCallback::CreateLambda([WeakThis, ItemId](const FMantleApiResult& Result)
	{
		if (const TSharedPtr<SMantleLibraryPanel> Panel = WeakThis.Pin())
		{
			Panel->FinishLookup(ItemId, Result);
		}
	}));
}

void SMantleLibraryPanel::FinishLookup(const FString& ItemId, const FMantleApiResult& Result)
{
	LookupPending.Remove(ItemId);
	FMantleLibraryCard Card;
	FString Error;
	if (Result.bSuccess && FMantleApiClient::ParseLibraryItem(Result.Body, Card, Error))
	{
		Remember(Card);
		// A listed script package's tile counts its files from here.
		bool bShown = false;
		for (FMantleLibraryCard& Listed : Cards)
		{
			if (Listed.Id.Equals(ItemId, ESearchCase::CaseSensitive))
			{
				Listed.Versions = Card.Versions;
				bShown = true;
			}
		}
		if (bShown)
		{
			RebuildTiles();
		}
	}
	else
	{
		LookupFailed.Add(ItemId, Result.bSuccess ? Error : Result.Message);
	}
	RebuildStorage();
}

void SMantleLibraryPanel::StoreManifestFor(const FString& ItemId)
{
	using namespace MantleLibraryPanelPrivate;
	const FMantleLibraryCard* Item = Known.Find(ItemId);
	AMantlePlaceSettingsActor* Settings = SettingsActor.Get();
	if (!Settings || !IsValid(Settings) || Settings->GetWorld() != EditorWorld())
	{
		Settings = AMantlePlaceSettingsActor::Find(EditorWorld());
	}
	if (!Item || !Settings || Item->Kind.IsEmpty())
	{
		return;
	}
	const bool bScript = Item->Kind.Equals(FMantleStorage::KindScript, ESearchCase::CaseSensitive);
	for (FMantleStorageSetting& Setting : Settings->Storage)
	{
		if (!Setting.Item.Equals(ItemId, ESearchCase::CaseSensitive))
		{
			continue;
		}
		const FMantleLibraryVersion* Pinned = Item->Versions.FindByPredicate([&Setting](const FMantleLibraryVersion& Version) { return Version.Version == Setting.Version; });
		const FString Manifest = bScript && Pinned ? LibraryManifestJson(Pinned->Manifest) : Setting.Manifest;
		if (!Setting.Kind.Equals(Item->Kind, ESearchCase::CaseSensitive) || !Setting.Manifest.Equals(Manifest, ESearchCase::CaseSensitive))
		{
			// What the item is (an entry typed in by hand learns it here) and what its points may set: kept with the level as the
			// Library panel learns it, not a change a creator made, so no undo step of its own.
			Settings->Modify();
			Setting.Kind = Item->Kind;
			Setting.Manifest = Manifest;
			// The settings the creator set stay, less any the pinned version no longer declares.
			if (bScript && !Manifest.IsEmpty())
			{
				const TArray<FString> Dropped = FMantleScriptPackages::PruneSettings(Setting.Settings, FMantleScriptPackages::ReadManifestForHints(Manifest));
				if (Dropped.Num() > 0)
				{
					UE_LOG(LogMantle, Log, TEXT("MANTLE_STORAGE package '%s' version %d no longer declares the setting(s) %s; dropped from its storage entry"),
						*Setting.Name, Setting.Version, *FString::Join(Dropped, TEXT(", ")));
				}
			}
		}
	}
}

void SMantleLibraryPanel::RebuildStorage()
{
	using namespace MantleLibraryPanelPrivate;
	if (!StorageBox.IsValid())
	{
		return;
	}
	StorageBox->ClearChildren();
	if (StorageSnapshot.IsEmpty())
	{
		StorageBox->AddSlot().AutoHeight()
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.Text(LOCTEXT("StorageEmpty", "Nothing stored yet. Add an item above; a Vehicle Spawn's Storage Item then names it."))
		];
		return;
	}
	// Items not seen yet are looked up once the rows are in: a lookup that fails at once rebuilds this list.
	TArray<FString> ToLookUp;
	for (const FMantleStorageEntry& Entry : StorageSnapshot)
	{
		const FMantleLibraryCard* Item = Known.Find(Entry.Item);
		if (!Item)
		{
			ToLookUp.AddUnique(Entry.Item);
		}

		FString Detail;
		bool bError = false;
		if (Item)
		{
			Detail = FString::Printf(TEXT("%s%s by %s  -  pinned v%d"),
				Item->Kind.Equals(FMantleStorage::KindScript, ESearchCase::CaseSensitive) ? TEXT("Script package ") : TEXT(""),
				*Item->Name, Item->CreatorName.IsEmpty() ? TEXT("unknown") : *Item->CreatorName, Entry.Version);
			if (Item->LatestVersion > Entry.Version)
			{
				Detail += FString::Printf(TEXT(" (latest v%d)"), Item->LatestVersion);
			}
			if (Item->Status.Equals(TEXT("delisted"), ESearchCase::CaseSensitive))
			{
				Detail += TEXT("  -  delisted: it stays here at this version but cannot be updated");
			}
			else if (FMantleLibraryStorage::IsTakenDownStatus(Item->Status))
			{
				Detail += TEXT("  -  taken down: ") + FMantleLibraryStorage::TakenDownNote() + TEXT(" Remove it from this list.");
				bError = true;
			}
		}
		else if (const FString* Failure = LookupFailed.Find(Entry.Item))
		{
			Detail = FString::Printf(TEXT("Could not look the item up: %s  -  pinned v%d"), **Failure, Entry.Version);
			bError = true;
		}
		else if (LookupPending.Contains(Entry.Item) || FMantleCreatorSignIn::IsSignedIn())
		{
			Detail = FString::Printf(TEXT("Looking up the item...  -  pinned v%d"), Entry.Version);
		}
		else
		{
			Detail = FString::Printf(TEXT("Sign in and press Refresh to see its name  -  pinned v%d"), Entry.Version);
		}
		const int32 Using = CountSpawnsUsing(Entry.Name);
		if (Using > 0)
		{
			Detail += FString::Printf(TEXT("  -  used by %d spawn(s) or Script Point(s)"), Using);
		}

		const FString EntryName = Entry.Name;
		const FString ItemId = Entry.Item;
		const int32 LatestVersion = Item ? Item->LatestVersion : 0;
		const bool bCanUpdate = Item && Item->LatestVersion > Entry.Version && FMantleLibraryStorage::IsAddableStatus(Item->Status);

		StorageBox->AddSlot().AutoHeight().Padding(0, 0, 0, 4)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
			.Padding(6.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock)
						.Font(FAppStyle::GetFontStyle("NormalFontBold"))
						.ToolTipText(FText::FromString(ItemId))
						.Text(FText::FromString(EntryName))
					]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock)
						.AutoWrapText(true)
						.Font(FAppStyle::GetFontStyle("SmallFont"))
						.ColorAndOpacity(bError ? ErrorColour() : FSlateColor::UseSubduedForeground())
						.Text(FText::FromString(Detail))
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(6, 0, 0, 0).VAlign(VAlign_Center)
				[
					SNew(SButton)
					.Visibility(bCanUpdate ? EVisibility::Visible : EVisibility::Collapsed)
					.Text(LOCTEXT("UpdateLatest", "Update to latest"))
					.ToolTipText(LOCTEXT("UpdateLatestTip", "Pin the item's newest version. Publishing never changes a pin by itself."))
					.OnClicked_Lambda([this, ItemId, LatestVersion] { return OnUpdateEntry(ItemId, LatestVersion); })
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(6, 0, 0, 0).VAlign(VAlign_Center)
				[
					SNew(SButton)
					.Text(LOCTEXT("Remove", "Remove"))
					.ToolTipText(LOCTEXT("RemoveTip", "Take the item out of this place's storage (Undo brings it back)."))
					.OnClicked_Lambda([this, EntryName] { return OnRemoveEntry(EntryName); })
				]
			]
		];
	}
	for (const FString& ItemId : ToLookUp)
	{
		LookUpItem(ItemId);
	}
}

FReply SMantleLibraryPanel::OnRemoveEntry(FString EntryName)
{
	FString Message;
	const bool bDone = ApplyRemove(EntryName, Message);
	StorageNote.Text = Message;
	StorageNote.bError = !bDone;
	RefreshStorageSnapshot();
	RebuildStorage();
	RebuildTiles();
	return FReply::Handled();
}

FReply SMantleLibraryPanel::OnUpdateEntry(FString ItemId, const int32 LatestVersion)
{
	FString Message;
	const bool bDone = ApplyUpdate(ItemId, LatestVersion, Message);
	StorageNote.Text = Message;
	StorageNote.bError = !bDone;
	RefreshStorageSnapshot();
	RebuildStorage();
	RebuildTiles();
	return FReply::Handled();
}

FReply SMantleLibraryPanel::OnSelectPlaceSettings()
{
	if (AMantlePlaceSettingsActor* Settings = AMantlePlaceSettingsActor::FindOrAdd(MantleLibraryPanelPrivate::EditorWorld()))
	{
		GEditor->SelectNone(/*bNoteSelectionChange*/ false, /*bDeselectBSPSurfs*/ true);
		GEditor->SelectActor(Settings, /*bInSelected*/ true, /*bNotify*/ true);
	}
	return FReply::Handled();
}

// ---------------------------------------------------------------------------------------------------------------------
// Card images: downloaded, decoded and held in memory only. Nothing is written to the project.

const FSlateBrush* SMantleLibraryPanel::FindThumbnail(const FString& ThumbnailUrl) const
{
	const FThumbnail* Found = Thumbnails.Find(ThumbnailUrl);
	return Found && Found->Brush.IsValid() ? Found->Brush.Get() : nullptr;
}

void SMantleLibraryPanel::RequestThumbnail(const FString& ThumbnailUrl)
{
	if (ThumbnailUrl.IsEmpty() || Thumbnails.Contains(ThumbnailUrl) || ThumbnailPending.Contains(ThumbnailUrl) || ThumbnailFailed.Contains(ThumbnailUrl))
	{
		return;
	}
	ThumbnailPending.Add(ThumbnailUrl);
	ThumbnailQueue.Add(ThumbnailUrl);
	StartNextThumbnail();
}

void SMantleLibraryPanel::StartNextThumbnail()
{
	using namespace MantleLibraryPanelPrivate;
	while (ThumbnailsActive < MaxThumbnailDownloads && ThumbnailQueue.Num() > 0)
	{
		const FString ThumbnailUrl = ThumbnailQueue[0];
		ThumbnailQueue.RemoveAt(0);

		FString Address;
		if (!FMantleLibraryImages::ResolveUrl(ThumbnailUrl, FMantleApiClient::BaseUrlResolver(), Address))
		{
			ThumbnailPending.Remove(ThumbnailUrl);
			ThumbnailFailed.Add(ThumbnailUrl);
			continue;
		}

		// A plain GET of a public image: no Mantle token goes to the image host.
		const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
		Request->SetURL(Address);
		Request->SetVerb(TEXT("GET"));
		Request->SetTimeout(20.0f);

		// Stop a download as soon as it is past the size cap, rather than holding the whole body first.
		Request->OnRequestProgress64().BindLambda([](FHttpRequestPtr Progressing, uint64 /*BytesSent*/, uint64 BytesReceived)
		{
			if (Progressing.IsValid() && BytesReceived > static_cast<uint64>(MaxThumbnailBytes))
			{
				Progressing->CancelRequest();
			}
		});

		const TWeakPtr<SMantleLibraryPanel> WeakThis = SharedThis(this);
		const FString ApiBase = FMantleApiClient::BaseUrlResolver();
		const TArray<FString> ExtraHosts = FMantleLibraryImages::ExtraHostsFromEnvironment();
		Request->OnProcessRequestComplete().BindLambda([WeakThis, ThumbnailUrl, Address, ApiBase, ExtraHosts](FHttpRequestPtr Finished, FHttpResponsePtr Response, bool bConnected)
		{
			const TSharedPtr<SMantleLibraryPanel> Panel = WeakThis.Pin();
			if (!Panel.IsValid())
			{
				return;
			}
			int32 StatusCode = 0;
			TArray<uint8> Bytes;
			if (bConnected && Response.IsValid())
			{
				// The HTTP module follows redirects: the address the bytes came from is the one that must be allowed.
				FString FinalUrl = Finished.IsValid() ? Finished->GetEffectiveURL() : FString();
				if (FinalUrl.IsEmpty())
				{
					FinalUrl = Response->GetEffectiveURL();
				}
				if (FinalUrl.IsEmpty())
				{
					FinalUrl = Address;
				}
				if (FMantleLibraryImages::IsAllowedUrl(FinalUrl, ApiBase, ExtraHosts))
				{
					StatusCode = Response->GetResponseCode();
					Bytes = Response->GetContent();
				}
				else
				{
					bConnected = false;
				}
			}
			Panel->FinishThumbnail(ThumbnailUrl, bConnected, StatusCode, Bytes);
		});
		++ThumbnailsActive;
		if (!Request->ProcessRequest())
		{
			--ThumbnailsActive;
			ThumbnailPending.Remove(ThumbnailUrl);
			ThumbnailFailed.Add(ThumbnailUrl);
		}
	}
}

void SMantleLibraryPanel::FinishThumbnail(const FString& ThumbnailUrl, const bool bConnected, const int32 StatusCode, const TArray<uint8>& Bytes)
{
	using namespace MantleLibraryPanelPrivate;
	--ThumbnailsActive;
	ThumbnailPending.Remove(ThumbnailUrl);

	bool bShown = false;
	if (bConnected && StatusCode >= 200 && StatusCode < 300 && Bytes.Num() > 0 && Bytes.Num() <= MaxThumbnailBytes)
	{
		// Read the dimensions from the header first: a small file can still claim an enormous image, and decoding it
		// would allocate that much before anything could refuse it.
		IImageWrapperModule& ImageWrappers = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		const EImageFormat Format = ImageWrappers.DetectImageFormat(Bytes.GetData(), Bytes.Num());
		const TSharedPtr<IImageWrapper> Wrapper = Format != EImageFormat::Invalid ? ImageWrappers.CreateImageWrapper(Format) : nullptr;
		const bool bHeaderOk = Wrapper.IsValid() && Wrapper->SetCompressed(Bytes.GetData(), Bytes.Num())
			&& FMantleLibraryImages::AreDimensionsAcceptable(Wrapper->GetWidth(), Wrapper->GetHeight());

		FImage Image;
		if (bHeaderOk && FImageUtils::DecompressImage(Bytes.GetData(), Bytes.Num(), Image)
			&& Image.SizeX > 0 && Image.SizeY > 0 && Image.SizeX <= MaxThumbnailSide && Image.SizeY <= MaxThumbnailSide)
		{
			FImage Bgra;
			Image.CopyTo(Bgra, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
			const TArrayView64<FColor> View = Bgra.AsBGRA8();
			const TArray<FColor> Pixels(View.GetData(), static_cast<int32>(View.Num()));

			FCreateTexture2DParameters Parameters;
			Parameters.bSRGB = true;
			Parameters.bUseAlpha = true;
			Parameters.CompressionSettings = TC_EditorIcon;
			Parameters.TextureGroup = TEXTUREGROUP_UI;
			UTexture2D* Texture = FImageUtils::CreateTexture2D(Bgra.SizeX, Bgra.SizeY, Pixels, GetTransientPackage(),
				MakeUniqueObjectName(GetTransientPackage(), UTexture2D::StaticClass(), TEXT("MantleLibraryThumbnail")).ToString(), RF_Transient, Parameters);
			if (Texture)
			{
				FThumbnail Thumbnail;
				Thumbnail.Texture = TStrongObjectPtr<UTexture2D>(Texture);
				Thumbnail.Brush = MakeShared<FSlateBrush>();
				Thumbnail.Brush->SetResourceObject(Texture);
				Thumbnail.Brush->ImageSize = FVector2D(Bgra.SizeX, Bgra.SizeY);
				Thumbnail.Brush->DrawAs = ESlateBrushDrawType::Image;
				Thumbnails.Add(ThumbnailUrl, MoveTemp(Thumbnail));
				bShown = true;
			}
		}
	}
	if (!bShown)
	{
		ThumbnailFailed.Add(ThumbnailUrl);
	}
	StartNextThumbnail();
}

#undef LOCTEXT_NAMESPACE
