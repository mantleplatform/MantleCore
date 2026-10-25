// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleCoverage.h"
#include "MantlePreviewClient.h"
#include "MantlePreviewExport.h"

class UWorld;
struct FMantleCosmeticPackage;

/** Where Test Locally is, as the Mantle Preview panel and the headless command report it. */
enum class EMantleTestLocallyState : uint8
{
	/** Nothing started in this editor session yet. */
	Idle,
	FindingClient,
	Exporting,
	/** The client is running and has not said it is ready. */
	Starting,
	Ready,
	/** Refused, cancelled-with-an-error or reported failed: ErrorText says what and how to fix it. */
	Failed,
	/** Stopped by the creator, or an export the creator cancelled. */
	Stopped,
	/** The preview process exited by itself after it was ready. */
	Closed
};

/**
 * The Test Locally flow and the one place its state lives: find the client, check it can run this
 * preview, export the open level, launch the client on the bundle with an editor-owned process, then follow the
 * client's preview-status.json while it runs. The panel and the Mantle.Preview.TestLocally console command both call
 * Run, so a creator and a harness walk the same steps. Main thread only.
 */
class FMantleTestLocally
{
public:
	/** One line the preview client wrote to preview-status.json. */
	struct FMessage
	{
		double Seconds = 0.0;
		/** "log", "warn" or "error". */
		FString Level;
		FString Source;
		FString Text;
	};

	static FMantleTestLocally& Get();

	/**
	 * The whole flow for World (the level open in the editor), blocking while it finds the client and exports (each
	 * shows a cancellable progress dialog). Returns true once the client is launched; otherwise OutError, which is
	 * also State/ErrorText, says what to fix. A cancelled export returns false with State Stopped and an empty OutError.
	 * Logs MANTLE_TEST_LOCALLY step lines as it goes.
	 */
	bool Run(UWorld* World, FString& OutError);

	/** Which level a weapon test runs in. */
	enum class EWeaponTestLevel : uint8
	{
		/** The open level is saved under /Game: test in it. */
		UseOpenLevel,
		/** Switch to the test range the plugin made earlier. */
		OpenTestRange,
		/** Make the test range once, save it and switch to it. */
		CreateTestRange,
		/** The open level has changes that are not saved anywhere: switching would lose them, so ask the creator to save first. */
		AskToSave
	};

	/** The rule behind the weapon button, pure: a saved level is used as is; an unsaved one is never replaced while it holds changes. */
	static EWeaponTestLevel ChooseWeaponTestLevel(bool bOpenLevelSaved, bool bOpenLevelHasUnsavedChanges, bool bTestRangeExists);

	/** /Game/MantleWeapons/WeaponTestRange: the small level the weapon button makes when there is no saved level to use. */
	static FString WeaponTestRangePackage();

	/** What the creator is told when the open level has unsaved changes and cannot be replaced, in plain words. */
	static FString WeaponTestRangeAskToSaveMessage();

	/**
	 * The weapon builder's Test locally: the same flow as Run with Weapon bundled and equipped. It uses the open level when
	 * that is saved under /Game, else a small test range it makes once and opens (never replacing a level with unsaved changes).
	 * OutNotice says in plain words what it did beyond the test itself (empty when nothing).
	 */
	bool RunWithWeapon(const FMantleWeaponToTry& Weapon, FString& OutError, FString& OutNotice);

	/**
	 * The Cosmetics panel's Test locally for an avatar (chunk 9): the same flow as Run with Avatar, a checked package the creator has
	 * not uploaded, as the preview player. Its files are cooked locally and bundled as a local draft; no catalog call is made. The level
	 * is chosen as for a weapon: the open level when it is saved under /Game, else the small test range.
	 */
	bool RunWithDraftAvatar(const TSharedRef<const FMantleCosmeticPackage>& Avatar, FString& OutError, FString& OutNotice);

	/** Stops the preview process this editor started (never another Mantle). */
	void Stop();

	/**
	 * Reads preview-status.json while the owned process runs and notices it exiting. True when anything the panel
	 * shows changed. Cheap and safe to call every frame; does nothing unless a preview is Starting or Ready.
	 */
	bool Poll();

	/** The client Find located, once per editor session; asks it (with a progress dialog) on first use or after ForgetClient. */
	bool EnsureClient(FMantlePreviewClientInfo& OutClient, FString& OutError);

	/** Drops the cached client, so the next EnsureClient looks again (after Choose client). */
	void ForgetClient();

	/** Opens a file picker for the client executable, saves it as UMantleSettings::PreviewClientExecutable and looks again. Message says what was found or why not. */
	bool ChooseClient(FString& OutMessage);

	/** Deletes the saved local data of LevelName's preview (Test Locally keeps it between runs). */
	bool ResetLocalData(const FString& LevelName, FString& OutMessage);

	/** The level a Test Locally on World would preview (its short package name), or empty when it is not a saved /Game level. */
	static FString LevelNameOf(UWorld* World);

	bool IsBusy() const { return bBusy; }

	EMantleTestLocallyState GetState() const { return State; }
	const FString& GetErrorText() const { return ErrorText; }
	/** A note from a panel action (Choose client, Reset local data); not an error of the run. */
	const FString& GetNotice() const { return Notice; }
	bool IsNoticeError() const { return bNoticeError; }
	void SetNotice(const FString& Text, bool bError);

	bool HasClient() const { return bHasClient; }
	const FMantlePreviewClientInfo& GetClient() const { return Client; }

	bool IsGeometryReady() const { return bGeometry; }
	bool IsScriptsReady() const { return bScripts; }
	bool IsSpawnReady() const { return bSpawn; }
	const TArray<FMessage>& GetMessages() const { return Messages; }
	int32 GetBlockedRequests() const { return BlockedRequests; }

	/** What the last export kept, approximated and dropped, which the panel shows as a collapsible section. Empty before the first export. */
	const FMantleCoverage& GetCoverage() const { return Coverage; }

	const FString& GetLevelName() const { return LevelName; }
	const FString& GetManifestFile() const { return ManifestFile; }
	const FString& GetBundleDirectory() const { return BundleDirectory; }

	/** Raised on every change the panel should redraw for. */
	int32 GetRevision() const { return Revision; }

private:
	FMantleTestLocally() = default;

	/** Run's flow, with the weapons (if any) to bundle and the draft avatar (if any) the player wears. */
	bool RunInternal(UWorld* World, const TArray<FMantleWeaponToTry>& Weapons, FString& OutError,
		const TSharedPtr<const FMantleCosmeticPackage>& DraftAvatar = nullptr);

	/** The level a weapon or draft avatar test runs in (ChooseWeaponTestLevel), opening or making the test range; null with OutError. */
	UWorld* PrepareTestLevel(const FString& RangeNotice, FString& OutError, FString& OutNotice);

	/** Back to a clean slate for a new run (the cached client stays). */
	void ResetRun();
	bool Failed(const FString& Why, FString& OutError);
	void LogStep(const FString& Step, const FString& Detail) const;

	/** Applies preview-status.json's text to the state shown; false when it is missing, partial, or unchanged. */
	bool ReadStatusFile();

	EMantleTestLocallyState State = EMantleTestLocallyState::Idle;
	FString ErrorText;
	FString Notice;
	bool bNoticeError = false;
	bool bBusy = false;
	int32 Revision = 0;

	bool bHasClient = false;
	FMantlePreviewClientInfo Client;
	/** The Preview Client Executable setting Client was found with; a different value finds the client again. */
	FString ClientSetting;

	FString LevelName;
	FString ManifestFile;
	FString BundleDirectory;

	bool bGeometry = false;
	bool bScripts = false;
	bool bSpawn = false;
	/** What the editor itself has to say (the export's warnings); shown before the client's own messages. */
	TArray<FMessage> EditorMessages;
	TArray<FMessage> Messages;
	FMantleCoverage Coverage;
	int32 BlockedRequests = 0;
	FString LastStatusText;
};
