// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleApiClient.h"
#include "MantlePlaceSettingsActor.h"
#include "MantlePlaceTypes.h"
#include "Widgets/SCompoundWidget.h"
#include "UObject/StrongObjectPtr.h"

class SBox;
class SVerticalBox;
class SWrapBox;
class UTexture2D;
struct FSlateBrush;

/** What FMantleLibraryStorage::PlanAdd decided. */
enum class EMantleLibraryAdd : uint8
{
	/** Entry may be appended. */
	Added,
	/** The item is already stored (a place stores one version of an item): ExistingIndex says where. */
	AlreadyStored,
	/** The place already holds the most entries a place may. */
	Full,
	/** The result would break a storage rule, or the item is not usable: Message says which. */
	Invalid,
};

/** The outcome of FMantleLibraryStorage::PlanAdd. */
struct FMantleLibraryAddPlan
{
	EMantleLibraryAdd Result = EMantleLibraryAdd::Invalid;

	/** The entry to append, set when Result is Added. */
	FMantleStorageEntry Entry;

	/** The entry that already stores the item, set when Result is AlreadyStored; else INDEX_NONE. */
	int32 ExistingIndex = INDEX_NONE;

	/** Worded for a creator: what went wrong, or what was decided. */
	FString Message;
};

/**
 * The Library panel's rules for a place's storage, apart from Slate and the level so they are tested
 * headless: how an item becomes a storage entry (its name, its version), and when it may not. The rules themselves are
 * FMantleStorage::CheckEntries'; this only picks a legal name and says why a request is refused before the level is touched.
 */
struct FMantleLibraryStorage
{
	/** The longest storage name (the schema's pattern: a letter then up to 63 more). */
	static constexpr int32 MaxNameLength = 64;

	/**
	 * An item's name as a storage name: runs of anything but ASCII letters and digits become one '_', the ends are trimmed,
	 * a name that starts with a digit gets "Item_" in front, an empty one is "Item", and the result is at most
	 * MaxNameLength characters. Always matches FMantlePlaceLimits::IsStorageName.
	 */
	static FString SanitizeName(const FString& ItemName);

	/**
	 * Base, or Base_2, Base_3... (the base shortened so the whole stays within MaxNameLength), the first no entry already
	 * uses. Compared ignoring case, so 'Truck' and 'truck' are never both offered although the schema allows both.
	 */
	static FString MakeUniqueName(const FString& Base, const TArray<FMantleStorageEntry>& Entries);

	/**
	 * Whether {sanitized unique name of ItemName, ItemId, Version} may be appended to Entries. Refuses (and says why):
	 * a bad item id or a version below 1; an item that is already stored, at any version; an item whose Status a place may
	 * not newly add (IsAddableStatus: a delisted item is refused, because the backend refuses to publish one); a full place
	 * (the schema's cap); and anything FMantleStorage::CheckEntries would report for the entries with it added.
	 */
	static FMantleLibraryAddPlan PlanAdd(const TArray<FMantleStorageEntry>& Entries, const FString& ItemId, const FString& ItemName, int32 Version,
		const FString& Status);

	/** The index of the entry that stores ItemId (compared case-sensitively), or INDEX_NONE. */
	static int32 FindItem(const TArray<FMantleStorageEntry>& Entries, const FString& ItemId);

	/** The index of the entry that stores ItemId at a version below LatestVersion, or INDEX_NONE when it is absent or already current. */
	static int32 FindUpdatable(const TArray<FMantleStorageEntry>& Entries, const FString& ItemId, int32 LatestVersion);

	/** True for an item status a place may store: what the storage resolve loads, "active" and "delisted". */
	static bool IsUsableStatus(const FString& Status);

	/**
	 * True for an item status a creator may newly add to a place, or update a pin of: only "active". A delisted item still
	 * loads (IsUsableStatus), so an entry already stored keeps working, but publishing a place that adds or re-pins it is refused.
	 */
	static bool IsAddableStatus(const FString& Status);

	/**
	 * True for status "taken_down": a moderator removed the item after a copyright complaint. It never loads
	 * (not IsUsableStatus), so a place that stores it cannot spawn it and no place may add or re-pin it; the Library
	 * panel keeps showing it, with TakenDownNote, so the creator can remove it from storage.
	 */
	static bool IsTakenDownStatus(const FString& Status);

	/** The sentence the panel shows for a taken-down item, in a card tooltip, a stored row or a refused add. */
	static FString TakenDownNote();
};

/** Where the Library panel downloads card images from. */
struct FMantleLibraryImages
{
	/** The most bytes of one image downloaded (the download is cancelled past this) and the longest side decoded. */
	static constexpr int64 MaxBytes = 4 * 1024 * 1024;
	static constexpr int32 MaxSide = 4096;

	/**
	 * The address to fetch a card's ThumbnailUrl from: a path starting with '/' is joined to ApiBase, and the result must
	 * pass IsAllowedUrl. False when empty or refused.
	 */
	static bool ResolveUrl(const FString& ThumbnailUrl, const FString& ApiBase, FString& OutUrl, const TArray<FString>& ExtraHosts = TArray<FString>());

	/**
	 * Whether a card image may be fetched from Url, or may be what a redirect ended at. The host must be one of:
	 * the API's own host and port (https, or http when ApiBase is http, a development API); an https host under
	 * .mantle-edge-gateway.workers.dev (the Cloudflare account that serves the asset downloads, R2_PUBLIC_BASE_URL) or under
	 * .r2.dev; or an https host named in ExtraHosts (exact, or a "*.suffix" entry). https hosts other than the API's must use
	 * port 443. A user-info section ('@'), a backslash or whitespace is refused.
	 */
	static bool IsAllowedUrl(const FString& Url, const FString& ApiBase, const TArray<FString>& ExtraHosts = TArray<FString>());

	/** Extra image hosts a deployment serves downloads from, from the MANTLE_LIBRARY_IMAGE_HOSTS environment variable (comma separated). */
	static TArray<FString> ExtraHostsFromEnvironment();

	/** Whether an image of these dimensions (read from its header, before any decode) may be decoded: both sides 1..MaxSide. */
	static bool AreDimensionsAcceptable(int64 Width, int64 Height);
};

/**
 * Tools > Mantle > Library: find a vehicle or weapon in the Mantle library, Get another creator's public
 * one, and add it to this place's storage (Mantle Place Settings > Storage), which publishing then pins. Nothing of
 * another creator's item is downloaded into the project: a card shows only its thumbnail, a spawn point only its name.
 * Native Slate; every call is asynchronous.
 */
class SMantleLibraryPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SMantleLibraryPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SMantleLibraryPanel() override;

	/** The nomad tab: registered at module startup, opened from the Tools menu. */
	static const FName TabName;
	static void RegisterTab();
	static void UnregisterTab();
	static void OpenTab();

private:
	/** A line shown under a tile or storage row, red when bError. */
	struct FNote
	{
		FString Text;
		bool bError = false;
	};

	/** A card image: its texture (kept alive here) and the brush a widget draws it with. */
	struct FThumbnail
	{
		TStrongObjectPtr<UTexture2D> Texture;
		TSharedPtr<FSlateBrush> Brush;
	};

	// Browsing.
	void Refresh();
	void LoadMore();
	void RequestPage(const FString& Cursor, bool bAppend);
	void OnPageAnswer(const FMantleApiResult& Result, int32 Serial, bool bAppend);
	void SetKind(const FString& Kind);
	void SetMine(bool bInMine);
	void RebuildTiles();
	TSharedRef<SWidget> BuildTile(const FMantleLibraryCard& Card);
	TSharedRef<SWidget> MakeToggle(const FText& Label, TFunction<bool()> IsOn, TFunction<void()> OnPicked, const FText& Tooltip);

	// Actions on a card.
	FReply OnGetClicked(FMantleLibraryCard Card);
	FReply OnAddClicked(FMantleLibraryCard Card);
	void FinishGet(const FString& ItemId, const FMantleApiResult& Detail);
	void SetTileNote(const FString& ItemId, const FString& Text, bool bError);
	void Remember(const FMantleLibraryCard& Card);
	/** The level's storage entries for ItemId learn its kind and, for a script package, its pinned version's manifest. */
	void StoreManifestFor(const FString& ItemId);

	/** The level edits, each one undoable step. True on success; OutMessage says what happened or why not. */
	bool ApplyAdd(const FMantleLibraryCard& Card, FString& OutMessage);
	bool ApplyUpdate(const FString& ItemId, int32 Version, FString& OutMessage);
	bool ApplyRemove(const FString& EntryName, FString& OutMessage);

	// This place's storage.
	TSharedRef<SWidget> MakeStorageSection();
	void RebuildStorage();
	void RefreshStorageSnapshot();
	EActiveTimerReturnType TickStorage(double InCurrentTime, float InDeltaTime);
	void LookUpItem(const FString& ItemId);
	void FinishLookup(const FString& ItemId, const FMantleApiResult& Result);
	FReply OnRemoveEntry(FString EntryName);
	FReply OnUpdateEntry(FString ItemId, int32 LatestVersion);
	FReply OnSelectPlaceSettings();
	int32 CountSpawnsUsing(const FString& EntryName) const;
	static FString Signature(const TArray<FMantleStorageEntry>& Entries);

	// Card images.
	void RequestThumbnail(const FString& ThumbnailUrl);
	void StartNextThumbnail();
	void FinishThumbnail(const FString& ThumbnailUrl, bool bConnected, int32 StatusCode, const TArray<uint8>& Bytes);
	const FSlateBrush* FindThumbnail(const FString& ThumbnailUrl) const;

	// What the creator asked for.
	FMantleLibraryQuery Query;
	FString SearchText;
	bool bMine = true;

	// What the API answered.
	TArray<FMantleLibraryCard> Cards;
	FString NextCursor;
	FString ListError;
	bool bLoading = false;
	bool bLoaded = false;
	/** Bumped by every new search, so an answer to an older one is dropped. */
	int32 RequestSerial = 0;

	/** Every card and detail seen, by item id: the storage list names its items from here. */
	TMap<FString, FMantleLibraryCard> Known;
	TSet<FString> LookupPending;
	TMap<FString, FString> LookupFailed;

	TMap<FString, FNote> TileNotes;
	TSet<FString> TileBusy;

	/** The line above the storage rows: what the last add, update or remove did. */
	FNote StorageNote;

	/** The level's storage as last read, and its signature: the timer rebuilds what shows it when it changes. */
	TArray<FMantleStorageEntry> StorageSnapshot;
	FString StorageSignature;
	bool bStorageHasSettings = false;
	TWeakObjectPtr<AMantlePlaceSettingsActor> SettingsActor;

	TMap<FString, FThumbnail> Thumbnails;
	TSet<FString> ThumbnailPending;
	TSet<FString> ThumbnailFailed;
	TArray<FString> ThumbnailQueue;
	int32 ThumbnailsActive = 0;

	TSharedPtr<SWrapBox> TilesBox;
	TSharedPtr<SVerticalBox> StorageBox;
};
