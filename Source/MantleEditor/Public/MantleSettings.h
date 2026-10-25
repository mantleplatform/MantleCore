// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "MantleSettings.generated.h"

class UWorld;

/**
 * Where the plugin talks to, and which place each level belongs to.
 *
 * Per user, not in the project's Default config (#394): both the API address and the level links
 * are one creator's, and a link shared through source control would send a teammate's publish to
 * a place their account cannot see. Deliberately holds no credentials either. The token is a
 * creator token (#94) pasted into the editor and held in memory only (FMantleCreatorSignIn), or
 * the MANTLE_AUTH_TOKEN environment variable.
 */
UCLASS(config = EditorPerProjectUserSettings, meta = (DisplayName = "Mantle"))
class MANTLEEDITOR_API UMantleSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UMantleSettings();

	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

	/**
	 * Development only: a Mantle API to use instead of Mantle's hosted one, for example
	 * http://localhost:8080. Empty, the default, publishes to Mantle. The -MantleApi= command line
	 * and the MANTLE_API_URL environment variable still take precedence over this.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Connection", meta = (DisplayName = "API Base URL Override (development)"))
	FString ApiBaseUrlOverride;

	/**
	 * Which place each level publishes to, keyed by MakeLinkKey: the API address, the account id
	 * and the level's package path. An empty value means this account's place for the level was
	 * deleted, so the next publish creates a new place.
	 *
	 * Remembered so publishing a second time updates the same place instead of creating a new
	 * one every save: otherwise a creator's list fills with duplicates and the link they shared
	 * stops being the one they are updating.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Places")
	TMap<FString, FString> PlaceIdByLink;

	/**
	 * The library item each vehicle publishes to (#614 stage 6), keyed by MakeLinkKey: the API address, the
	 * account id and the vehicle source asset's object path. A later publish of the same vehicle adds a new
	 * version to this item instead of making another one. An item that no longer exists (or is not this
	 * account's) is forgotten and the next publish makes a new one.
	 */
	UPROPERTY(config, VisibleAnywhere, Category = "Library")
	TMap<FString, FString> LibraryItemByLink;

	/**
	 * How many times each LibraryItemByLink key's item was forgotten (it was deleted or delisted), same keys. It goes into the
	 * Idempotency-Key of the next item the vehicle creates, so a new item is not mistaken for a retry of the old creation.
	 */
	UPROPERTY(config, VisibleAnywhere, Category = "Library")
	TMap<FString, int32> LibraryGenerationByLink;

	/**
	 * #625: avatars upload "as-is" (what you see is what you get: a supported skeleton and Mantle's
	 * limits, no customisation, no clothing) only when this is on. Off by default, so creators see only Convert (the
	 * avatar becomes a MetaHuman); with it off an avatar body without Mantle's reference topology is refused with
	 * "Convert it first". Hidden: no EditAnywhere, so Project Settings never shows it; set it in the per-user
	 * EditorPerProjectUserSettings.ini ([/Script/MantleEditor.MantleSettings] bAllowAsIsAvatars=True). The as-is code
	 * paths stay compiled; their tests turn it on for their own scope (FMantleAsIsAvatarsScope).
	 */
	UPROPERTY(config)
	bool bAllowAsIsAvatars = false;

	/**
	 * #563: the catalog avatar item (itm_u and 32 hex digits) the offline preview's player wears. The preview export
	 * copies that item's metadata and cooked files into the bundle. Empty: the preview player has no avatar body.
	 * Set it here or with the console command Mantle.Preview.Avatar <item id>.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Preview", meta = (DisplayName = "Preview Avatar Item Id"))
	FString PreviewAvatarItemId;

	/**
	 * #563: the Mantle client Test Locally runs your level in. Leave it empty and Mantle looks for the client you
	 * installed from Steam or the Epic Games Store. Set it to Mantle-Win64-Shipping.exe (or Mantle.exe in the install
	 * folder) to use a specific install. Developers may point it at the engine's UnrealEditor.exe, which then runs
	 * this project with -game.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Preview", meta = (DisplayName = "Preview Client Executable", FilePathFilter = "exe",
		ToolTip = "The Mantle client that Test Locally runs your level in. Leave empty to use the Mantle you installed from Steam or the Epic Games Store. Pick Mantle-Win64-Shipping.exe (or Mantle.exe) in a Mantle install folder to use a specific one."))
	FFilePath PreviewClientExecutable;

	/**
	 * For developers: a Mantle client project (its .uproject, such as mantle/client/Mantle.uproject) that Test Locally runs
	 * with the engine's UnrealEditor.exe -game instead of the open project. Used when Preview Client Executable is empty or
	 * UnrealEditor.exe (empty: this editor's own UnrealEditor.exe). The command-line -MantlePreviewProject=<path> wins over this.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Preview", meta = (DisplayName = "Preview Client Project", FilePathFilter = "uproject",
		ToolTip = "Developers only. A Mantle client project (.uproject) that Test Locally runs with UnrealEditor.exe -game, instead of this project. Leave empty to use the Preview Client Executable or the Steam or Epic install. Works with an empty Preview Client Executable (this editor's UnrealEditor.exe) or with UnrealEditor.exe picked there. The command line -MantlePreviewProject=<path> overrides it."))
	FFilePath PreviewClientProject;

	/** Whether avatars may upload as-is (bAllowAsIsAvatars). */
	static bool AllowsAsIsAvatars();

	static const UMantleSettings* Get();
	static UMantleSettings* GetMutable();

	/** The key a level is remembered under: its package path, e.g. "/Game/Maps/Main". */
	static FString LevelKey(const UWorld* World);

	/** "<api base url, lower case, no trailing slash>|<account id>|<level package path>". */
	static FString MakeLinkKey(const FString& ApiBaseUrl, const FString& AccountId, const FString& LevelPath);

	/** The place the level at LevelPath publishes to for AccountId at ApiBaseUrl: empty when it has none or was unlinked. */
	FString FindPlaceForLevel(const FString& ApiBaseUrl, const FString& AccountId, const FString& LevelPath) const;

	/** Records the place a level publishes to for this account and API, and saves immediately. */
	void RememberPlaceForLevel(const FString& ApiBaseUrl, const FString& AccountId, const FString& LevelPath, const FString& PlaceId);

	/** Unlinks the level from its place for this account and API only (the place was deleted), and saves immediately. Other accounts' links are untouched. */
	void UnlinkPlaceForLevel(const FString& ApiBaseUrl, const FString& AccountId, const FString& LevelPath);

	/** The library item (itm_u...) the vehicle at SourcePath publishes to for AccountId at ApiBaseUrl, empty when it has none. */
	FString FindLibraryItemForVehicle(const FString& ApiBaseUrl, const FString& AccountId, const FString& SourcePath) const;

	/** Records the vehicle's library item for this account and API, and saves immediately. */
	void RememberLibraryItemForVehicle(const FString& ApiBaseUrl, const FString& AccountId, const FString& SourcePath, const FString& ItemId);

	/** Forgets the vehicle's library item for this account and API only (it is gone, delisted, or not this account's), bumps the vehicle's generation, and saves immediately. */
	void ForgetLibraryItemForVehicle(const FString& ApiBaseUrl, const FString& AccountId, const FString& SourcePath);

	/** How often the vehicle's library item was forgotten for this account and API: 0 until it first is. */
	int32 FindLibraryGenerationForVehicle(const FString& ApiBaseUrl, const FString& AccountId, const FString& SourcePath) const;

	/** Resolved API address: -MantleApi=, then MANTLE_API_URL, then ApiBaseUrlOverride, then Mantle's hosted API. */
	FString ResolveApiBaseUrl() const;

	/** Where an editor publishes when nothing overrides it: the hosted API the shipped client uses. */
	static FString DefaultApiBaseUrl();
};

/** Tests: UMantleSettings::bAllowAsIsAvatars set for the scope (in memory, never saved), restored after. */
struct MANTLEEDITOR_API FMantleAsIsAvatarsScope
{
	explicit FMantleAsIsAvatarsScope(bool bAllow = true);
	~FMantleAsIsAvatarsScope();
	UE_NONCOPYABLE(FMantleAsIsAvatarsScope);

private:
	bool bWas = false;
};
