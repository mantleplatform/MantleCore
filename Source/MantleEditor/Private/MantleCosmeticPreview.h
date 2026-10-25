// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Engine/SkinnedAssetCommon.h"
#include "MantleAvatarContract.h"
#include "MantleCosmeticItem.h"
#include "UObject/GCObject.h"
#include "UObject/ObjectPtr.h"

class FMantleAssetStore;
class FMantleAvatarContract;
class UAnimationAsset;
class USkeletalMesh;
class USkeleton;
struct FMantleAnimClip;
struct FMantleBodyShape;
struct FMantleSkinnedMesh;
struct FMantleCosmeticDraft;

/** Who the preview dresses: the body the garment was authored on (local, no download), or a catalog avatar by id. */
enum class EMantlePreviewTargetKind : uint8
{
	SourceBody,
	CatalogAvatar,
};

struct FMantlePreviewTarget
{
	EMantlePreviewTargetKind Kind = EMantlePreviewTargetKind::SourceBody;

	/** CatalogAvatar: the avatar's item id (itm_u...), trimmed. */
	FString AvatarId;

	static FMantlePreviewTarget Source();
	static FMantlePreviewTarget Catalog(const FString& ItemId);

	/** "source", or the avatar id. */
	FString Key() const;

	/** For the status line: "the body it was authored on", "catalog avatar itm_u...". */
	FString Describe() const;

	bool operator==(const FMantlePreviewTarget& Other) const { return Kind == Other.Kind && AvatarId == Other.AvatarId; }
};

/** #283: the fit preview's picker rows: the body it was authored on or an id the creator types. */
enum class EMantlePreviewPickKind : uint8
{
	SourceBody,
	TypedId,
};

/**
 * What the creator picked to preview a garment on. The panel opens on the source body and remembers the pick
 * for the editor session.
 */
struct FMantlePreviewPick
{
	EMantlePreviewPickKind Kind = EMantlePreviewPickKind::SourceBody;

	/** What the creator typed for TypedId; kept while another row is picked, so picking it again restores the text. */
	FString TypedId;

	static FMantlePreviewPick Source();
	static FMantlePreviewPick Typed(const FString& Id);

	/** The picker's rows: the source body, then a typed id. */
	static TArray<FMantlePreviewPick> Options();

	/** The source body is the only default. */
	static FMantlePreviewPick Default();

	/**
	 * What the picker opens on: Remembered (this editor session's last source/typed pick), else Default, keeping
	 * Remembered's typed text. Unknown old pick kinds fall back to the source body.
	 */
	static FMantlePreviewPick Initial(const FMantlePreviewPick* Remembered);

	/**
	 * The target it dresses: the source body or the typed id, trimmed. False with a sentence for the creator when nothing
	 * is typed; the session reports malformed or retired catalog ids clearly.
	 */
	bool ToTarget(FMantlePreviewTarget& OutTarget, FString& OutError) const;

	/** The picker's row text: "The body it was made on" or "Another avatar by id". */
	FString Label() const;

	/** The same row: source body or typed-id row (the typed text is not compared). */
	bool IsSameRow(const FMantlePreviewPick& Other) const;
};

/**
 * #283: how the avatar preview poses a creator's avatar body, the client's path for its appearance skeleton
 * (UMantleBodyAnimInstance): a hidden UEFN mannequin driver plays a standing idle and the body copies its pose through
 * the skeleton's IK retargeter (Retarget Pose From Mesh). #535: the retargeter is the MantleCore plugin's own, the one
 * copy the client loads too (UMantleBodyAnimInstance::RetargeterPathForSkeleton; MantleCore cannot reference
 * client/Source, so the path is repeated here), so it works in any project. The driver is the project's UEFN mannequin
 * when it has one, else the one the plugin builds in memory from its own IK rig (FMantleBuiltInMannequin::UEFNMannequin,
 * FMantleAvatarPreviewInputs): nothing to add to the project.
 */
struct FMantleAvatarPreviewPlan
{
	/** The appearance skeleton id the upload identifies (metahuman, ue5mannequin, ue4mannequin, uefnmannequin). */
	FString SkeletonId;

	/** Object path of the plugin's retargeter for SkeletonId. */
	FString RetargeterPath;

	/** The chosen face follows the body: a MetaHuman body with a face. A UE5, UE4 or UEFN Mannequin body is whole and has no separate face. */
	bool bShowFace = false;

	/** Not fatal: what the preview leaves out ("no face chosen", "the face is not shown"). */
	TArray<FString> Notes;

	/**
	 * #535: the idle the driver plays, a MAN1 clip on the UEFN skeleton in the plugin's Resources/AvatarContract, played
	 * through UMantleClipAnimInstance. Absent until the owner decides the idle's licence; the preview then plays an idle
	 * named IdleAssetName from the project, when it has one on the driver's skeleton, else the driver's reference pose.
	 */
	static const TCHAR* const IdleClipFileName;
	static const TCHAR* const IdleAssetName;

	/** The plan for SkeletonId (case-sensitive), or false with a sentence for a skeleton the client poses no body on. */
	static bool ForSkeleton(const FString& SkeletonId, bool bHasFace, FMantleAvatarPreviewPlan& Out, FString& OutError);
};

/**
 * #535: what the avatar preview found to pose a body with, and what it tells the creator. The plugin's retargeter, the
 * driver (given: FMantleBuiltInMannequin::UEFNMannequin, the project's UEFN mannequin else the plugin's built-in one), and
 * an idle: the plugin's MAN1 clip when present, else the project's IdleAssetName on the driver's skeleton (never for the
 * built-in driver, whose skeleton no project asset is on). Anything that keeps the body
 * in its reference pose is named in Missing; nothing here crashes on a missing asset. Game thread (loads assets).
 */
struct FMantleAvatarPreviewInputs
{
	/** A UIKRetargeter, as a UObject (MantleEditor does not link IKRig). */
	UObject* Retargeter = nullptr;
	USkeletalMesh* Driver = nullptr;

	/** The plugin's idle clip (null without the file, or when it cannot be read: a note says why). */
	TSharedPtr<const FMantleAnimClip> IdleClip;

	/** Else the project's idle on the driver's skeleton (null without one). */
	UAnimationAsset* IdleAsset = nullptr;

	/** What keeps the body in its reference pose, each a phrase naming the asset and what to do. */
	TArray<FString> Missing;

	/** Not fatal ("no idle, so the pose is the driver's reference pose"). */
	TArray<FString> Notes;

	/**
	 * Loads Plan's retargeter, takes InDriver as found (null: none; DriverError then says why the built-in one could not
	 * be made), and reads the idle from IdleClipPath (a file; empty: none).
	 */
	static FMantleAvatarPreviewInputs Gather(const FMantleAvatarPreviewPlan& Plan, USkeletalMesh* InDriver, const FString& IdleClipPath,
		const FString& DriverError = FString());

	/** The plugin's idle clip file: <MantleCore>/Resources/AvatarContract/<IdleClipFileName>; empty without the plugin. */
	static FString DefaultIdleClipPath();

	/**
	 * The status line: posed ("Your metahuman avatar, posed by ..."), or the reference pose with the reason, IK Rig's node
	 * missing (bRetargetNode false) or the missing assets. Notes (the plan's and these) follow.
	 */
	FString Status(const FMantleAvatarPreviewPlan& Plan, bool bPosed, bool bRetargetNode) const;
};

/**
 * What the preview dresses the target in: the creator's garment and the body it was authored on, as CPU data read
 * from the assets on the game thread. The same inputs the upload sends (the garment's mesh, its source body's
 * reference-topology positions as the body shape; the plugin computes the shape's RBF weights), so the fit the
 * preview runs is the one the client runs on the cooked item.
 */
struct FMantlePreviewGarment
{
	/** Hair preview uses the finalized exported cards and approved source head, and the runtime head fitter. */
	bool bHair = false;
	bool bFacialHair = false;
	/** The garment's LOD0 (FMantleSkinnedMesh::ExtractFromSkeletalMesh). */
	TSharedPtr<const FMantleSkinnedMesh> Garment;

	/** The authoring body's reference-topology positions and reference pose; weights computed on the fit task when absent. */
	TSharedPtr<const FMantleBodyShape> SourceShape;

	/** The authoring body's LOD0 as CPU data: the wearer when the target is the source body. */
	TSharedPtr<const FMantleSkinnedMesh> SourceBody;

	/**
	 * #430: the face the garment was authored with (its surface at the face's reference LOD, one LOD; null without a
	 * face): the shape's neck and the collar's authored face clearance are measured on it, as for the upload, and it is
	 * the wearer's face when the target is the source body.
	 */
	TSharedPtr<const FMantleSkinnedMesh> SourceFace;

	/** The garment asset's material slots: the fitted garment is built with them (it looks as authored). */
	TArray<FSkeletalMaterial> Materials;

	/** The item's API slot ("top", "bottom", "footwear", ...): top, bottom and footwear take the wearer path. */
	FString Slot;

	/** Content identity of the inputs (the fit cache's garment key): a changed asset gets another key. */
	FString Key;

	/** RBF interpolation points and the reference vertex count (limits.json), for the shape's weights. */
	int32 WarpPointCount = 750;
	int32 TopologyVertexCount = 0;
	uint32 TopologyUVHash = 0;

	bool IsValid() const;
	static bool FromHairAssets(const FMantleCosmeticDraft& Draft, const FMantleAvatarContract& Contract,
		FMantlePreviewGarment& Out, FString& OutError);

	/**
	 * Reads Garment (LOD0) and Body (its reference-topology LOD as the shape, its LOD0 as the local wearer) for
	 * Slot, and Face when given (#430: the face it was authored with; a face that cannot be read is left out, as the
	 * upload leaves it out). False with a sentence for the creator when either has no CPU data, the body has no
	 * reference-topology LOD, or the body's LOD0 is not that LOD. Game thread (reads render data). #483: KeepSlots,
	 * when given, keeps only the garment's sections of those material slots (KeepSections), as the upload does.
	 */
	static bool FromAssets(const USkeletalMesh* Garment, const USkeletalMesh* Body, const FString& Slot, const FMantleAvatarContract& Contract,
		FMantlePreviewGarment& Out, FString& OutError, const USkeletalMesh* Face = nullptr, const TArray<int32>* KeepSlots = nullptr);

	/**
	 * Keeps only the sections of Mesh drawn with one of KeepSlots' material slots, on every LOD: their triangles,
	 * and the vertices those use, renumbered in the order the kept triangles first use them. Section material slots and
	 * names are unchanged.
	 */
	static void KeepSections(FMantleSkinnedMesh& Mesh, TConstArrayView<int32> KeepSlots);

	/** The key for inputs already read: the garment's and the shape's positions and indices, and the slot. */
	static FString MakeKey(const FMantleSkinnedMesh& Garment, const FMantleBodyShape& Shape, const FString& Slot);
};

/** Where the preview's current request is. */
enum class EMantlePreviewState : uint8
{
	/** Nothing asked yet. */
	Idle,
	/** Asking the catalog for the avatar (POST /avatar/items/batch). */
	Resolving,
	/** Downloading and reading the avatar's cooked meshes (the asset store; a cached file is not downloaded again). */
	Downloading,
	/** The shape's weights, the warp field, the authored clearance, the fit and the dressed body, off the game thread. */
	Fitting,
	/** Building the meshes the viewport shows (game thread). */
	Building,
	Ready,
	Failed,
};

/** What the viewport shows right now. Replaced whole on every change. */
struct FMantlePreviewView
{
	/** The request this view answers (0: none). */
	uint64 Request = 0;
	FMantlePreviewTarget Target;
	EMantlePreviewState State = EMantlePreviewState::Idle;

	/** One line for the creator: what is happening or what went wrong. */
	FString Status;

	/** Ready: the wearer's body (the dressed body when the garment covers it), its face (may be null) and the fitted garment. */
	USkeletalMesh* Body = nullptr;
	USkeletalMesh* Face = nullptr;
	USkeletalMesh* Garment = nullptr;
	bool bGarmentFollowsFace = false;
	/** Catalog avatar socket corrections, also used by accessory preview. */
	TMap<FString, FMantleCosmeticTransform> Sockets;

};

struct FMantleCosmeticPreviewSettings
{
	/** The mesh the source body target shows (the authoring body asset); garments are built against it. */
	TWeakObjectPtr<USkeletalMesh> SourceBodyMesh;

	/** The source body's face, shown with it (may be null). */
	TWeakObjectPtr<USkeletalMesh> SourceFaceMesh;
};

/**
 * The cosmetic panel's fit preview, without the viewport.
 *
 * Request(target) dresses a target in the garment: the source body at once, a catalog avatar by id after resolving
 * it (MantleCosmetic's resolver: POST /avatar/items/batch) and downloading its cooked body and face through the
 * asset store (content-addressed, verified, cached on disk). The fit is the client's: the shape's RBF weights, the
 * warp field (FMantleCatalogGarmentFit::ComputeField), the authored clearance and FMantleCatalogGarmentFit::Fit
 * (the wearer path for top, bottom and footwear), then the dressed body (FMantleDressedBody) that hides the body
 * under the garment. Everything but the mesh builds runs on background tasks; the game thread only pumps.
 *
 * Cached for the session: resolved and decoded avatars (built once) by id, and fits by (garment inputs, wearer).
 * A newer request replaces the current one: a download nobody wants any more is cancelled, and a fit that lands for
 * an older request is kept in the cache but not shown. Game thread only.
 */
class FMantleCosmeticPreviewSession : public FGCObject, public TSharedFromThis<FMantleCosmeticPreviewSession>
{
public:
	static TSharedRef<FMantleCosmeticPreviewSession> Create(const FMantleCosmeticPreviewSettings& InSettings,
		TSharedRef<IMantleCosmeticResolver> InResolver, TSharedRef<FMantleAssetStore> InStore);

	virtual ~FMantleCosmeticPreviewSession() override;

	/** The garment inputs. A different Key drops the fits of the old inputs; the avatars stay cached. */
	void SetGarment(const FMantlePreviewGarment& InGarment);

	/** The source body's meshes (the panel's authoring body and its face). */
	void SetSourceMeshes(USkeletalMesh* Body, USkeletalMesh* Face);

	/** Dresses Target in the garment; replaces the current request. Returns the request id. */
	uint64 Request(const FMantlePreviewTarget& Target);

	/** Resolve and show the avatar without fitting a garment (rigid accessory preview). */
	uint64 RequestAvatar(const FMantlePreviewTarget& Target);

	/** Drops the current request (the panel's garment changed or it closed): its downloads are cancelled. */
	void Cancel();

	const FMantlePreviewView& GetView() const { return View; }

	/** Fires from Pump whenever GetView() changed. */
	TFunction<void(const FMantlePreviewView&)> OnChanged;

	/** Delivers finished work, advances the request and builds. The core ticker calls it every frame. */
	void Pump();

	// FGCObject
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual FString GetReferencerName() const override { return TEXT("FMantleCosmeticPreviewSession"); }

private:
	FMantleCosmeticPreviewSession(const FMantleCosmeticPreviewSettings& InSettings, TSharedRef<IMantleCosmeticResolver> InResolver,
		TSharedRef<FMantleAssetStore> InStore);
	UE_NONCOPYABLE(FMantleCosmeticPreviewSession);

	enum class EWearerState : uint8 { Resolving, Downloading, Decoding, Decoded, Ready, Failed };

	/** A catalog avatar: resolved, downloaded, decoded and built once per session. */
	struct FWearer
	{
		FString Id;
		EWearerState State = EWearerState::Resolving;
		FMantleCosmeticItem Item;
		uint64 BodyFetch = 0;
		uint64 FaceFetch = 0;
		int32 Pending = 0;
		TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe> BodyBytes;
		TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe> FaceBytes;
		TSharedPtr<const FMantleSkinnedMesh> Body;
		TSharedPtr<const FMantleSkinnedMesh> Face;
		/** Decoded once, validated against the current garment on every request. */
		uint32 BodyUVHash = 0;
		FString BodySkeleton;
		FString FaceSkeleton;
		TObjectPtr<USkeletalMesh> BuiltBody;
		TObjectPtr<USkeletalMesh> BuiltFace;
		FString Error;
		TArray<FString> Notes;
		double BuildMs = 0.0;
	};

	enum class EFitState : uint8 { Fitting, Fitted, Ready, Failed };

	/** One garment on one wearer. */
	struct FFit
	{
		EFitState State = EFitState::Fitting;
		TSharedPtr<const FMantleSkinnedMesh> Fitted;
		/** The dressed body: the wearer's LOD0 with the shrink and sink applied, and the covered vertices hidden. */
		TSharedPtr<const FMantleSkinnedMesh> Dressed;
		TBitArray<> Hidden;
		int32 HiddenCount = 0;
		TObjectPtr<USkeletalMesh> BuiltGarment;
		TObjectPtr<USkeletalMesh> BuiltBody;
		FString Path;
		FString Error;
		TArray<FString> Notes;
		double FitMs = 0.0;
		double BuildMs = 0.0;
	};

	using FCompletion = TFunction<void(FMantleCosmeticPreviewSession&)>;
	struct FCompletionQueue
	{
		FCriticalSection Lock;
		TArray<FCompletion> Items;
	};

	void Launch(TUniqueFunction<FCompletion()> Work);
	void DrainCompletions();
	void Advance();
	void SetView(FMantlePreviewView&& NewView);
	void Fail(const FString& Status);

	// Catalog avatars.
	void StartWearer(const FString& Id);
	void OnResolved(const FString& Id, const FMantleCosmeticResolveResult& Result);
	void OnFetched(const FString& Id, bool bFace, const FMantleAssetResult& Result);
	void OnDecoded(const FString& Id, TSharedPtr<const FMantleSkinnedMesh> Body, TSharedPtr<const FMantleSkinnedMesh> Face,
		const FString& BodySkeleton, const FString& FaceSkeleton, const FString& Error, const FString& FaceNote);
	void BuildWearer(FWearer& Wearer);
	/** The source body's skeleton for "metahuman" and its face's for "metahuman_face". */
	USkeleton* SkeletonFor(const FString& SkeletonId) const;
	void DropWearerIfUnwanted(const FString& Id);

	// Fits.
	void StartFit(const FString& FitKey, const TSharedPtr<const FMantleSkinnedMesh>& WearerBody, const TSharedPtr<const FMantleSkinnedMesh>& WearerFace);
	void OnFitted(const FString& FitKey, TSharedPtr<const FMantleSkinnedMesh> Fitted, TSharedPtr<const FMantleSkinnedMesh> Dressed,
		TBitArray<> Hidden, int32 HiddenCount, const FString& Path, const FString& Error, const TArray<FString>& Notes, double Ms);
	void BuildFit(FFit& Fit, USkeletalMesh* WearerMesh, USkeletalMesh* WearerFace);

	/** "<garment key>|<wearer key>". */
	FString FitKeyFor(const FString& WearerKey) const;

	/** The fit the current request shows once its wearer is known; empty until then. */
	FString CurrentFitKey() const;

	FMantleCosmeticPreviewSettings Settings;
	TSharedRef<IMantleCosmeticResolver> Resolver;
	TSharedRef<FMantleAssetStore> Store;
	TSharedRef<FCompletionQueue, ESPMode::ThreadSafe> Completions;

	FMantlePreviewGarment Garment;
	TSharedPtr<const FMantleBodyShape> WeightedShape;
	TMap<FString, TSharedRef<FWearer>> Wearers;
	TMap<FString, TSharedRef<FFit>> Fits;

	FMantlePreviewTarget Target;
	uint64 RequestId = 0;
	bool bActive = false;
	bool bAvatarOnly = false;
	/** The current request found its wearer or fit already there (for the view). */
	bool bWearerWasCached = false;
	bool bFitWasCached = false;

	FMantlePreviewView View;
	FTSTicker::FDelegateHandle TickerHandle;
};
