// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleAssetUploadQueue.h"
#include "MantleCosmeticUpload.h"
#include "MantleLibraryPublish.h"
#include "MantlePlaceAssetConverter.h"
#include "MantlePlaceAssets.h"
#include "MantlePlaceValidator.h"
#include "MantlePublisher.h"
#include "MantleScriptPackages.h"
#include "UObject/GCObject.h"
#include "UObject/ObjectPtr.h"
#include "UObject/WeakObjectPtrTemplates.h"

class UWorld;

/**
 * #804: keeps every object a publish gathered alive for as long as it runs. The gathered dependencies hold their meshes,
 * materials, sounds, clips and vehicles only weakly (FMantleGathered*), and several were loaded just to be gathered (a
 * vehicle's parts, its liveries and horn, a weapon's model, a Script Asset), so nothing else references them: a garbage
 * collection between two conversion steps (the editor's periodic one while the panel's publish runs, or the headless
 * publish's own, FMantlePlacePublishSession::FTiming::CollectGarbageEverySteps) would free what a later step converts.
 */
class MANTLEEDITOR_API FMantleDependencyPins : public FGCObject
{
public:
	/** Holds every valid object Dependencies names, replacing what was held. */
	void Pin(const FMantlePlaceDependencies& Dependencies);

	/** Lets them go. */
	void Release();

	int32 Num() const { return Objects.Num(); }

	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual FString GetReferencerName() const override { return TEXT("FMantleDependencyPins"); }

private:
	TArray<TObjectPtr<UObject>> Objects;
};

/**
 * One script source a publish uploads (S1.9, S1.19): an executable, or a TypeScript file's or a
 * graph's authoring source. #549: also a UI file (bUIDocument), uploaded through the same
 * POST /scripts with language rml or rcss.
 */
struct MANTLEEDITOR_API FMantlePlaceScriptUpload
{
	/** The place-local scr_ id the exporter writes; for a UI file, its place path ("UI/Shop.rml"). */
	FString ScriptId;

	/** True for the authoring source (TypeScript, graph JSON) beside the executable. */
	bool bAuthorSource = false;

	FString Source;

	/** luau, javascript, typescript, python or graph; rml or rcss for a UI file. */
	FString Language;

	/** Its path under Scripts/ (a UI file's place path), for messages. */
	FString Path;

	bool bGraph = false;

	/** #549: a UI document or stylesheet; its src_ id goes to the export's UIDocumentSourceIds. */
	bool bUIDocument = false;

	/**
	 * #838: a script package's file (go-live only): its src_ id goes to the package's SourceIds[PackageFileIndex] for the Library step,
	 * not to the place file. INDEX_NONE for a place script or UI file.
	 */
	int32 PackageIndex = INDEX_NONE;
	int32 PackageFileIndex = INDEX_NONE;
};

/** #838: one Packages/<Name>/ folder a go-live publish turns into a library item (or a new version of the one an earlier publish made). */
struct MANTLEEDITOR_API FMantlePackagePublish
{
	/** The folder's name: the storage name. */
	FString Name;

	/** What its item is remembered under for this account and API (FMantleScriptPackages::MemoryKey). */
	FString MemoryKey;

	FMantleScriptPackageManifest Manifest;

	/** Its files in path order, TypeScript compiled (FMantleScriptPackages::PrepareFiles). */
	TArray<FMantleScriptPackageFile> Files;

	/** The uploaded src_ id of each of Files, filled by the Scripts step. */
	TArray<FString> SourceIds;

	/** Files as the library takes them: {path, side, language, source}, source the uploaded id. Empty until every file is uploaded. */
	TArray<FMantleLibraryScriptFile> LibraryFiles() const;
};

/**
 * The place file's size and instance count measured before anything is converted or uploaded (GhzGarage/mantle#818), against
 * what the backend takes: IMantlePlacePublishSource::CheckPlaceSize.
 */
struct MANTLEEDITOR_API FMantlePlaceSizeCheck
{
	/** The place file's size in bytes and its instanced mesh count, as measured (after grouping and any thinning). */
	int64 Bytes = 0;
	int64 Instances = 0;

	/** Over the instance limit or the file-size limit with nothing left to try: the publish stops here. */
	bool bOver = false;

	/** Why, for the Check step's rows. */
	TArray<FMantleValidationIssue> Issues;

	/** When thinning is on offer: what it would do, in plain words. Empty when it is not (or already chosen). */
	FString ThinOffer;
};

enum class EMantlePlaceExportOutcome : uint8
{
	Exported,
	Failed,
	/** The level now uses a mesh or material that was not uploaded (it changed while publishing): gather again. */
	NeedsNewAssets,
};

/**
 * What the publish flow needs from a level. FMantleLevelPublishSource is the open level; the tests
 * use a fake with a ready-made asset set, so the whole flow runs with no world and no network.
 * Every call is on the game thread.
 */
class MANTLEEDITOR_API IMantlePlacePublishSource
{
public:
	virtual ~IMantlePlacePublishSource() = default;

	/** What the creator calls the level. */
	virtual FString GetLevelName() const = 0;

	/**
	 * Check this place, then gather what needs converting. False when there is no level to publish,
	 * or it cannot tell which account it publishes as (OutMessage says why).
	 */
	virtual bool Prepare(FMantleValidationResult& OutValidation, FString& OutMessage) = 0;

	/**
	 * GhzGarage/mantle#818: before any conversion or upload, the place file's size and instance count against what the backend takes
	 * (the file is refused at the draft save, after every upload). False, with OutCheck.bOver set and the issues saying why and how
	 * to fix it, when the place cannot fit; the session stops there. A source that cannot tell passes.
	 */
	virtual bool CheckPlaceSize(FMantlePlaceSizeCheck& OutCheck) { return true; }

	/** Converting is split into steps so the editor keeps running between them. */
	virtual int32 NumConversionSteps() const = 0;
	/** How many of those steps are not project content to prepare (#786: the thumbnail), so the log counts only content. */
	virtual int32 NumNonContentSteps() const { return 0; }
	virtual FString ConversionStepName(int32 Step) const = 0;
	virtual void Convert(int32 Step, FMantlePlaceAssetSet& Set) = 0;

	/** After the last step: a short note for the panel ("41 from the local cache"). */
	virtual FString DescribeConversion() const { return FString(); }

	/**
	 * The level's scripts to upload, in order, then its UI files (#549, bUIDocument). False with a
	 * plain sentence when one cannot be published.
	 */
	virtual bool CollectScripts(TArray<FMantlePlaceScriptUpload>& OutScripts, FString& OutMessage) = 0;

	/**
	 * #549: the uploaded UI files' src_ ids, keyed by place path, for the next Export. The session
	 * calls it right before every Export; a source without UI files can ignore it.
	 */
	virtual void SetUIDocumentSourceIds(const TMap<FString, FString>& UIDocumentSourceIds) {}

	/** The place file, with the uploaded assets' and scripts' ids. */
	virtual EMantlePlaceExportOutcome Export(const FMantlePlaceAssetSet& Set, const TMap<FString, FString>& ScriptSourceIds,
		const TMap<FString, FString>& ScriptAuthorSourceIds, FString& OutPlaceJson, TArray<FMantleValidationIssue>& OutIssues) = 0;

	/** Lines for the publish log after a successful Export: each Script Asset's id ("Script asset <path> = <id>"). */
	virtual TArray<FString> GetExportNotes() const { return TArray<FString>(); }

	/** The place this level publishes to (UMantleSettings), empty when it has none yet. */
	virtual FString FindPlaceId() = 0;

	virtual void RememberPlaceId(const FString& PlaceId) = 0;

	/**
	 * The library item (itm_u...) this level's vehicle at SourcePath (its object path) was last published as, for
	 * this account and API (#614 stage 6); empty for none. A later publish adds a version to it.
	 */
	virtual FString FindLibraryItem(const FString& SourcePath) { return FString(); }
	virtual void RememberLibraryItem(const FString& SourcePath, const FString& ItemId) {}
	virtual void ForgetLibraryItem(const FString& SourcePath) {}

	/** How often SourcePath's remembered item was forgotten: part of the Idempotency-Key of the item made next, so a new item is never taken for a replay of a forgotten one. */
	virtual int32 LibraryGeneration(const FString& SourcePath) { return 0; }

	/**
	 * After a successful go-live that stored vehicles: bring the level in line with what was published (its Storage list
	 * and the spawns of those vehicles), as one undoable edit. Nothing else in the level changes.
	 */
	virtual void ApplyPublishedVehicles(TArray<FMantleStoredVehicle>& Stored) {}

	/** #789: ApplyPublishedVehicles for the weapons a go-live stored: the level's Storage list and its Weapon Spawns. */
	virtual void ApplyPublishedWeapons(TArray<FMantleStoredVehicle>& Stored) {}

	/**
	 * #838: the level's script packages (Packages/, beside Scripts/), each ready to publish: manifest read, files prepared. False with
	 * a plain sentence when one cannot be published (Check this place names the problem). A source with no packages returns none.
	 */
	virtual bool CollectPackages(TArray<FMantlePackagePublish>& OutPackages, FString& OutMessage) { OutPackages.Reset(); return true; }

	/** #838: after a go-live that stored packages: the level's Storage list holds each under its folder's name, as one undoable edit. */
	virtual void ApplyPublishedPackages(const TArray<FMantleLibraryPackageRef>& Stored) {}

	/** Unlinks the level from the place FindPlaceId named, for this account and API only. */
	virtual void ForgetPlaceId() = 0;

	/**
	 * The attribution players are shown for an uploaded mesh or material (#398), by its object path:
	 * normalized, empty for none. The open level reads FMantleAttribution::ForObjectPath (a package's
	 * MantleAttribution tag, else the project's Credits rules).
	 */
	virtual FString AttributionFor(const FString& ObjectPath) const { return FString(); }

	/**
	 * #804: a folder the session may write its spilled files to (FTiming::bSpillConvertedBytes), empty for none. The session
	 * makes one folder of its own under it and deletes that folder when it ends.
	 */
	virtual FString GetSpillDirectory() const { return FString(); }

	/**
	 * #804: every conversion step has run. Nothing is converted from the gathered objects again unless the level changes, and
	 * then Prepare gathers (and holds) afresh, so let go of what was held for the steps (FMantleDependencyPins) now.
	 */
	virtual void ConversionFinished() {}

	/** #804: the publish is over (done, failed or cancelled): let go of whatever was held for it. */
	virtual void EndPublish() {}

	/**
	 * #840: true for a source that publishes no place (the weapon builder's Publish): once its files are uploaded and approved, each
	 * vehicle and weapon it converted becomes a library item (or a new version of the one an earlier publish made), and that is all.
	 * No scripts are collected, no place file is written, and no place is created, saved or published.
	 */
	virtual bool IsLibraryOnly() const { return false; }
};

/** The open level: FMantlePlaceValidator, FMantlePlaceDependencies, FMantlePlaceAssetConverter (F5 #339), the script collection and FMantlePlaceExporter. */
class MANTLEEDITOR_API FMantleLevelPublishSource : public IMantlePlacePublishSource
{
public:
	UE_NONCOPYABLE(FMantleLevelPublishSource);

	/** Converter: the GLTFExporter flattener and the project's cache when null (tests pass their own). */
	FMantleLevelPublishSource(UWorld* InWorld, FString InNotes, TSharedPtr<FMantlePlaceAssetConverter> InConverter = nullptr);

	virtual FString GetLevelName() const override;
	virtual bool Prepare(FMantleValidationResult& OutValidation, FString& OutMessage) override;
	virtual bool CheckPlaceSize(FMantlePlaceSizeCheck& OutCheck) override;
	virtual int32 NumConversionSteps() const override;
	virtual int32 NumNonContentSteps() const override { return bThumbnailStep ? 1 : 0; }
	virtual FString ConversionStepName(int32 Step) const override;
	virtual void Convert(int32 Step, FMantlePlaceAssetSet& Set) override;
	virtual FString DescribeConversion() const override;
	virtual bool CollectScripts(TArray<FMantlePlaceScriptUpload>& OutScripts, FString& OutMessage) override;
	virtual void SetUIDocumentSourceIds(const TMap<FString, FString>& InUIDocumentSourceIds) override { UIDocumentSourceIds = InUIDocumentSourceIds; }
	virtual EMantlePlaceExportOutcome Export(const FMantlePlaceAssetSet& Set, const TMap<FString, FString>& ScriptSourceIds,
		const TMap<FString, FString>& ScriptAuthorSourceIds, FString& OutPlaceJson, TArray<FMantleValidationIssue>& OutIssues) override;
	virtual TArray<FString> GetExportNotes() const override { return ExportNotes; }
	virtual FString FindPlaceId() override;
	virtual void RememberPlaceId(const FString& PlaceId) override;
	virtual void ForgetPlaceId() override;
	virtual FString FindLibraryItem(const FString& SourcePath) override;
	virtual void RememberLibraryItem(const FString& SourcePath, const FString& ItemId) override;
	virtual void ForgetLibraryItem(const FString& SourcePath) override;
	virtual int32 LibraryGeneration(const FString& SourcePath) override;
	virtual void ApplyPublishedVehicles(TArray<FMantleStoredVehicle>& Stored) override;
	virtual void ApplyPublishedWeapons(TArray<FMantleStoredVehicle>& Stored) override;
	virtual bool CollectPackages(TArray<FMantlePackagePublish>& OutPackages, FString& OutMessage) override;
	virtual void ApplyPublishedPackages(const TArray<FMantleLibraryPackageRef>& Stored) override;
	virtual FString AttributionFor(const FString& ObjectPath) const override;
	virtual FString GetSpillDirectory() const override;
	virtual void ConversionFinished() override;
	virtual void EndPublish() override;

	/** GhzGarage/mantle#818: remove instances evenly when the place is over a limit (the creator chose it). Set before the session starts. */
	void SetThinInstances(bool bThin) { bThinInstances = bThin; }

private:
	TWeakObjectPtr<UWorld> World;
	FString LevelName;
	FString LevelKey;
	FString Notes;
	/** The API and account the level's place is remembered under (#394), resolved in Prepare. */
	FString LinkApi;
	FString LinkAccount;
	TSharedPtr<FMantlePlaceAssetConverter> Converter;
	FMantlePlaceDependencies Dependencies;
	/** #804: keeps what Dependencies names alive from Prepare until the conversion steps are done (ConversionFinished). */
	FMantleDependencyPins Pins;
	/** #556 item 5: the images in the level's UI/ folder (place path, disk path), converted after Dependencies as texture uploads. */
	TArray<TPair<FString, FString>> UIImages;
	/** #786: whether the place's thumbnail is captured, as the last conversion step (false without a renderer). */
	bool bThumbnailStep = false;
	/** GetExportNotes: the last successful Export's Script Asset ids. */
	TArray<FString> ExportNotes;
	/** #549: SetUIDocumentSourceIds, for the next Export. */
	TMap<FString, FString> UIDocumentSourceIds;
	/** GhzGarage/mantle#818: SetThinInstances. */
	bool bThinInstances = false;
	/** GhzGarage/mantle#818: the World Partition cells Prepare loaded, let go in EndPublish. */
	TWeakObjectPtr<UObject> LoadedCells;

	/** The Level Instance actors that load also loaded (GhzGarage/mantle#818); EndPublish unloads them. */
	TArray<TWeakObjectPtr<UObject>> LoadedLevelInstances;
	/** GhzGarage/mantle#818: what loading them cost, for the Check step's notes. */
	FString WorldPartitionNote;
};

enum class EMantlePublishPhase : uint8
{
	Idle,
	/** Check this place, and gather what needs uploading. */
	Checking,
	/** Meshes and materials to upload files (F5's converter and its cache). */
	Converting,
	/** The upload queue: diff, upload, then cooking and screening. */
	Uploading,
	/** The uploaded assets' attributions (#398), written on this creator's claims before the draft is saved. */
	Crediting,
	Scripts,
	Exporting,
	/**
	 * Go live only, with vehicles (#614 stage 6): each uploaded vehicle becomes a library item (or a new version of
	 * the one an earlier publish made) and the place file names it through storage. Waits for the creator's
	 * choice of who can use the vehicles (VehicleChoice) before it sends anything.
	 */
	Library,
	CreatingPlace,
	Saving,
	Publishing,
	Done,
	Failed,
	Cancelled,
};

/** The rows of the panel's checklist. */
enum class EMantlePublishStep : uint8
{
	Check,
	Prepare,
	Upload,
	Process,
	GoLive,
};

enum class EMantlePublishStepState : uint8
{
	NotStarted,
	Running,
	Done,
	/** Done, with something the creator should read. */
	DoneWithNotes,
	Failed,
	Skipped,
};

/**
 * One Publish (or Save) of a level, start to finish, with no separate upload step:
 * Check this place, convert (F5 #339, cached), upload through FMantleAssetUploadQueue (F4 #338:
 * textures, then materials naming their ids, then meshes), wait for cooking and screening, upload
 * the scripts, export the place file with every id, save the draft, publish.
 *
 * - Runs in the background, a step per Tick: the creator keeps working.
 * - Cancel leaves nothing half-published: nothing is saved or published until every upload is
 *   approved, and once the publish request itself is sent it can no longer be cancelled (it is
 *   one atomic call). Uploads stay on Mantle, so the next Publish resumes from them.
 * - A republish with nothing changed converts from the cache and uploads nothing.
 * - Every refusal is a sentence naming the Unreal asset, never an id or a hash (GetAttention).
 * - A texture refused because the same pixels are on Mantle for another use does not stop the
 *   publish: its material goes live without that map, with a warning saying so.
 * - Nothing about prices.
 */
class MANTLEEDITOR_API FMantlePlacePublishSession : public TSharedFromThis<FMantlePlacePublishSession>
{
public:
	/** #804: the converted bytes a set holds before a publish starts spilling them by itself (override: Mantle.Publish.AutoSpillMB, 0 = this, below 0 = never). */
	static constexpr int64 DefaultAutoSpillBytes = 2LL * 1024 * 1024 * 1024;

	/** #804: the commit growth that triggers a collection between conversion steps. */
	static constexpr int64 DefaultCollectGarbageGrowthBytes = 2LL * 1024 * 1024 * 1024;

	/** #804: a spill folder left by a publish that never cleaned up is deleted once its newest file is this old. */
	static constexpr int32 StaleSpillDays = 1;

	struct FTiming
	{
		/** scripts_pending at publish: a fresh script starts pending. About twenty seconds, then the creator is told. */
		TArray<float> ScriptsPendingDelays = { 2.0f, 4.0f, 6.0f, 8.0f };
		/** assets_pending at publish (a material's textures racing their approval). */
		TArray<float> AssetsPendingDelays = { 5.0f, 10.0f, 20.0f, 30.0f };
		/** A script upload or draft save that failed for a reason time can fix. */
		TArray<float> RetryDelays = { 2.0f, 5.0f, 10.0f };
		/** Real seconds of conversion per Tick before the editor gets a frame. */
		double ConvertBudgetSeconds = 0.05;
		/** How often the level may change under a running publish before it stops asking. */
		int32 MaxRegathers = 2;
		/**
		 * #804: collect garbage after every this many conversion steps; 0 never (the panel's publish: the editor collects
		 * between ticks on its own). A headless publish blocks the editor, so nothing else would (FMantlePublisher::Run).
		 */
		int32 CollectGarbageEverySteps = 0;
		/** #804: when collecting at all (CollectGarbageEverySteps above 0), also collect once the process's commit has grown by this much since the last collection; 0 never. */
		int64 CollectGarbageGrowthBytes = DefaultCollectGarbageGrowthBytes;
		/**
		 * #804 step 6: after each conversion slice, write the converted files of at least SpillMinBytes to the conversion cache
		 * folder and keep only their paths, and send them from there. On when this or the console variable
		 * Mantle.Publish.SpillConvertedBytes says so (SpillRequested), or on its own once the set holds more
		 * than AutoSpillBytes of converted files. Hashes and uploads are the same.
		 */
		bool bSpillConvertedBytes = false;
		int64 SpillMinBytes = 64 * 1024;
		int64 AutoSpillBytes = DefaultAutoSpillBytes;
	};

	/** Whether Mantle.Publish.SpillConvertedBytes is non-zero. */
	static bool SpillRequested();

	/**
	 * Deletes the folders under Root (the spill directory) whose newest file is older than MaxAge: what a publish that was killed
	 * left behind. A folder with no files goes by its own time. Returns how many were deleted.
	 */
	static int32 SweepStaleSpillFolders(const FString& Root, const FTimespan& MaxAge);

	/** Clock: seconds, FPlatformTime::Seconds when unset (the tests' fake clock also drives the queue). */
	FMantlePlacePublishSession(TSharedRef<IMantlePlacePublishSource> InSource, TSharedRef<IMantleUploadTransport> InTransport,
		bool bInGoLive, FString InNotes, TFunction<double()> InClock = nullptr);
	~FMantlePlacePublishSession();

	FTiming Timing;

	/** The upload queue's settings; bWaitForApproval follows GoLive (a Save does not wait for screening). */
	FMantleAssetUploadQueue::FSettings QueueSettings;

	/**
	 * Who can use this place's vehicles once they are library items (#614 stage 6). The panel keeps it current while the
	 * session runs. Public needs the declaration: the Library step waits, sending nothing, until IsReady.
	 */
	FMantleVehicleLibraryChoice VehicleChoice;

	/**
	 * True for a go-live that uploads vehicles or weapons (known once they are converted) or, #838, publishes script packages (known
	 * once the scripts are collected): the panel shows the visibility choice then. One choice covers all of them.
	 */
	bool NeedsVehicleChoice() const { return bGoLive && (Set.Vehicles.Num() > 0 || Set.Weapons.Num() > 0 || Packages.Num() > 0) && !IsFinished(); }

	/** True while the Library step has nothing to do but wait for a valid VehicleChoice. */
	bool IsWaitingForVehicleChoice() const { return Phase == EMantlePublishPhase::Library && !VehicleChoice.IsReady(); }

	void Start();
	void Tick();

	/** Stops everything that can still be stopped. False once the publish request has been sent. */
	bool Cancel();
	bool CanCancel() const;

	EMantlePublishPhase GetPhase() const { return Phase; }
	bool IsFinished() const { return Phase == EMantlePublishPhase::Done || Phase == EMantlePublishPhase::Failed || Phase == EMantlePublishPhase::Cancelled; }
	bool Succeeded() const { return Phase == EMantlePublishPhase::Done; }
	bool IsGoLive() const { return bGoLive; }
	FString GetLevelName() const;

	const FMantlePublishResult& GetResult() const { return Result; }

	/** 0 to 1, never backwards. */
	float GetProgress() const;

	/** One plain sentence: what is happening, or how it ended. */
	FString GetStatusText() const;

	EMantlePublishStepState GetStepState(EMantlePublishStep Step) const;
	FString GetStepDetail(EMantlePublishStep Step) const;

	/** Rows that need the creator (FMantlePublishResult::Attention), available while it runs. */
	const TArray<FMantleValidationIssue>& GetAttention() const { return Result.Attention; }

	/** The running (or last) upload queue, null before uploading starts. */
	const FMantleAssetUploadQueue* GetQueue() const { return Queue.Get(); }

	/** The converted assets and their recorded ids. */
	const FMantlePlaceAssetSet& GetAssetSet() const { return Set; }

	/** The library item versions this publish made or pinned for its vehicles and weapons, in the order it made them. */
	const TArray<FMantleLibraryVehicleRef>& GetLibraryRefs() const { return LibraryRefs; }

	/** What happened, oldest first: the panel's details. */
	const TArray<FString>& GetLog() const { return Log; }

	/**
	 * The sentence for a save or publish the API refused, with the place gate's codes
	 * (assets_rejected, assets_not_owned, assets_missing, assets_pending) and the file checks
	 * (wrong_asset_kind, material_slots) turned into the Unreal assets they are about. Adds one
	 * Attention row per asset.
	 */
	FString DescribePlaceRefusal(const FMantleApiResult& Refused);

	/**
	 * Every uploaded asset of AssetSet with an id, and its attribution (#398): a mesh's and a material's
	 * from ForPath (their object paths), a texture's from the first material that uses it and has
	 * one. Each id once, meshes first, then materials and their textures, in AssetSet's order. Empty
	 * attributions are included: sending them clears what an earlier publish wrote.
	 */
	static TArray<TPair<FString, FString>> CollectAttributions(const FMantlePlaceAssetSet& AssetSet, const TFunction<FString(const FString& ObjectPath)>& ForPath);

	/** Attributions per PUT /assets/attributions (the API takes up to 1000). */
	static constexpr int32 AttributionBatchSize = 500;

private:
	void TickChecking();
	void TickConverting();
	void StartUploads();
	void TickUploading();
	void TickCrediting();
	void HandleCredited(const FMantleApiResult& Response);
	void TickScripts();
	void TickExporting();
	void EnterSaving();
	/** After the uploads (and their credits): the scripts, or #840 for a library-only source, straight to the Library step. */
	void EnterScriptsOrLibrary();
	void TickLibrary();
	void StartLibraryJob();
	void HandleLibraryResponse(const FMantleApiResult& Response);
	void FinishLibraryJob(const FString& ItemId, int32 Version, const TCHAR* What);
	void ContinueLibraryJobAfterLooking();
	void RestartLibraryJobAsNew(const TCHAR* Why);
	void FinishLibrary();
	void SendPlaceCall(const FMantleUploadCallRequest& Request);
	void OnPlaceResponse(uint32 ForGeneration, const FMantleApiResult& Response);
	void HandleScriptUploaded(const FMantleApiResult& Response);
	void HandleCreated(const FMantleApiResult& Response);
	void HandleSaved(const FMantleApiResult& Response);
	void HandlePublished(const FMantleApiResult& Response);
	bool PrepareMaterial(const FString& Key, FMantleQueuedUpload& Item, FString& OutWhyNot);
	bool PrepareVehicle(const FString& Key, FMantleQueuedUpload& Item, FString& OutWhyNot);
	bool PrepareWeapon(const FString& Key, FMantleQueuedUpload& Item, FString& OutWhyNot);
	void ForgetGonePlace();
	void AddAttention(EMantleIssueSeverity Severity, const FString& Problem, const FString& Remedy);
	void Fail(const FString& Why);
	void Finish(const FString& Message);
	/**
	 * #804: the session has ended (done, failed or cancelled). Frees every converted file's and queued file's bytes (a finished
	 * session is kept by the panel until the next Publish and only shows statuses, ids and sizes), deletes the spilled files, lets
	 * go of the gathered objects and, when no other publish or preview is running, drops the bake caches. Once.
	 */
	void ReleaseHeldMemory();
	/** #804: after a conversion step, spills the converted files that are now in the set (when asked to). */
	void SpillConverted();
	void AddLog(const FString& Line);
	FString NameForId(const FString& Id) const;

	TSharedRef<IMantlePlacePublishSource> Source;
	TSharedRef<IMantleUploadTransport> Transport;
	bool bGoLive = true;
	FString Notes;
	TFunction<double()> Clock;

	EMantlePublishPhase Phase = EMantlePublishPhase::Idle;
	/** Where it was when it failed or was cancelled (the panel marks that step). */
	EMantlePublishPhase FailedPhase = EMantlePublishPhase::Idle;
	FMantlePublishResult Result;
	FMantlePlaceAssetSet Set;
	TSharedPtr<FMantleAssetUploadQueue> Queue;
	TArray<FString> Log;

	/** #804: Start entered FMantlePublishCaches, and ReleaseHeldMemory has not left it yet. */
	bool bCachesEntered = false;
	bool bMemoryReleased = false;
	/** #804: this session's own folder under the source's spill directory; empty until the first file is spilled. */
	FString SpillFolder;
	bool bSpillFailureLogged = false;
	/** #804: the commit when garbage was last collected (or conversion began); -1 until read. */
	int64 CommitAtLastCollection = -1;

	int32 ConversionTotal = 0;
	int32 ConversionDone = 0;
	int32 SetIssuesCopied = 0;
	FString ConversionName;
	int32 Regathers = 0;
	/** GhzGarage/mantle#818: the place file's size was checked (once; a regather does not repeat it). */
	bool bSizeChecked = false;
	bool bHadUploads = false;

	/** The PUT /assets/attributions bodies still to send, and how many assets carry an attribution. */
	TArray<FString> CreditBatches;
	bool bCreditsCollected = false;
	int32 CreditIndex = 0;
	int32 CreditedAssets = 0;

	TArray<FMantlePlaceScriptUpload> Scripts;
	bool bScriptsCollected = false;
	int32 ScriptIndex = 0;
	TMap<FString, FString> ScriptSourceIds;
	TMap<FString, FString> ScriptAuthorSourceIds;
	/** #549: uploaded UI files' src_ ids, keyed by place path. */
	TMap<FString, FString> UIDocumentSourceIds;

	/** One vehicle's trip into the library (#614 stage 6). */
	struct FLibraryJob
	{
		FString Name;
		/** The vehicle source asset's object path: what its item is looked up and keyed under (the first of Paths). */
		FString Path;
		/** Every vehicle source asset that uploaded as AssetId (identical bytes share an upload): each remembers the item. */
		TArray<FString> Paths;
		FString AssetId;
		/** The uploaded texture of the card's picture (#821): the vehicle's thumbnail, named on its item. Empty for none (a weapon, or a vehicle built without one). */
		FString ThumbnailId;
		/**
		 * Where the vehicle came from (#614 stage 7): the source asset's path and its body mesh's import file name. Sent with
		 * the item or version only when the publish is public.
		 */
		FMantleProvenance Provenance;
		/** The item an earlier publish made, empty for none. */
		FString ItemId;
		/** Looking is GET of the remembered item, Patching its PATCH to the chosen visibility; Creating and Versioning are the POSTs. */
		enum class EStep : uint8 { Start, Looking, Patching, Creating, Versioning } Step = EStep::Start;
		/** #789: a weapon, not a vehicle: its item is made with kind "weapon". */
		bool bWeapon = false;
		/** #838: a script package, Packages[PackageIndex]: its item is made with kind "script" from its files and manifest. */
		bool bPackage = false;
		int32 PackageIndex = INDEX_NONE;
		/** A remembered item that is gone or delisted is forgotten once, then a new one is made. */
		bool bForgotten = false;
		/** What Looking found: the version of the remembered item that already holds AssetId, 0 for none. */
		int32 ExistingVersion = 0;
	};
	TArray<FLibraryJob> LibraryJobs;
	int32 LibraryIndex = 0;
	bool bLibraryCollected = false;
	TArray<FMantleLibraryVehicleRef> LibraryRefs;
	/** What the place file stored, adopted by the level once the place is live. */
	TArray<FMantleStoredVehicle> StoredVehicles;
	/** #789: the same for the weapons (the type is shared). */
	TArray<FMantleStoredVehicle> StoredWeapons;
	/** #838: the level's script packages (collected with the scripts), and the library item versions a go-live made of them. */
	TArray<FMantlePackagePublish> Packages;
	TArray<FMantleLibraryPackageRef> PackageRefs;
	/** What the Library step sent and the visibility it was sent with, for the log. */
	bool bLibraryPublic = true;

	/** Uploaded id to the Unreal asset (every queue run of this session). */
	TMap<FString, FString> NameById;
	/** Texture keys a material went live without (same pixels on Mantle for another use). */
	TSet<FString> DroppedTextures;

	bool bRequestInFlight = false;
	bool bPublishSent = false;
	bool bDraftSaved = false;
	uint32 Generation = 0;
	int32 Attempts = 0;
	double NextActionAt = 0.0;
	mutable float ShownProgress = 0.0f;
};
