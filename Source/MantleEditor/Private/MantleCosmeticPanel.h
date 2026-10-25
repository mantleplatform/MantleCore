// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleAvatarConverter.h"
#include "MantleCosmeticExport.h"
#include "MantleCosmeticPackage.h"
#include "MantleCosmeticPreview.h"
#include "Widgets/SCompoundWidget.h"
#include "UObject/StrongObjectPtr.h"

class FMantleCosmeticPreviewSession;
class FMantleCosmeticUploadSession;
class SMantleCosmeticPreviewViewport;
class UAnimSequence;
class SBox;
class SVerticalBox;
struct FAssetData;
struct FSlateDynamicImageBrush;
template <typename OptionType> class SComboBox;
struct FMantlePackReview;

/** #55: one "Make it" choice of a pack review row: a slot (empty: not used), or into another piece (PartOf). */
struct FMantlePackMove
{
	FString Label;
	FString Slot;
	int32 PartOf = INDEX_NONE;
};

/**
 * The one Avatar door: what the panel sees in the character picked under Avatar, for the "what I see" line and the route
 * choice (FMantleConvertSection::WhatISee, AsIsGreyedWhy). The kind and bones are read on pick (SeeKind); the rest once
 * the character is looked at (SeeContext, SeeAssembly).
 */
struct FMantleAvatarSight
{
	enum class EKind : uint8
	{
		None,
		SkeletalMesh,
		StaticMesh,
		CreatorCharacter,
		Blueprint,
	};
	EKind Kind = EKind::None;
	/** The picked asset's name, at the start of the line. */
	FString Name;
	/** A skeletal mesh's reference skeleton bones. */
	int32 Bones = 0;
	/** Whether the converter has looked at it (the fix list was made). */
	bool bLooked = false;
	/**
	 * A skeletal mesh whose skeleton doesn't carry it: one bone, or (chunk 4, #161) more bones but no usable skin weights.
	 * Convert fits it to a MetaHuman body as it does a static mesh.
	 */
	bool bUnrigged = false;
	/**
	 * Chunk 7: As is can auto-rig this model with no rig (FMantleUnriggedAsIs::CanAutoRig: skinned to Epic's MetaHuman body
	 * skeleton, keeping its look), and why not when it can't in this editor (no renderer); empty when it has a rig.
	 */
	bool bAutoRigs = false;
	FString AutoRigNote;
	/** Strip read the body as covered by clothing (its face-only line ticked by default). */
	bool bCovered = false;
	/** A MetaHuman Blueprint's clothing meshes and hair pieces. */
	int32 Clothing = 0;
	int32 Hair = 0;
};

/**
 * What the Cosmetics panel's Convert section shows, apart from Slate. Nothing here asks Epic: the
 * build checks the sign-in when Convert runs (FMantleMetaHumanBuild::Preflight), and never opens a login window.
 */
struct FMantleConvertSection
{
	/**
	 * The routes the section offers: Convert alone while UMantleSettings::bAllowAsIsAvatars is off (the default: no route
	 * picker), Convert then As-is while it is on.
	 */
	static TArray<EMantleAvatarRoute> Routes();

	/** Whether the section shows the route picker: more than one route (Routes). */
	static bool ShowsRoutePicker();

	/** The route Convert... analyses on: Picked while the section offers it, else Convert. */
	static EMantleAvatarRoute EffectiveRoute(EMantleAvatarRoute Picked);

	/**
	 * The cloud note shown once, by the Convert button: FMantleMetaHumanBuild::ConsentLine while a ticked line would build
	 * with Epic's cloud (a cloud step's, IsCloudStep, fixable and ticked); empty otherwise.
	 */
	static FString CloudNote(TConstArrayView<FMantleConvertFix> Fixes);

	/** #625: the steps that build with Epic's cloud: MHBuild, and MHSource (the MetaHuman-source route). */
	static bool IsCloudStep(FName FixerId);

	/** A fix line's action as the section shows it: a cloud step's without the cloud note (shown once, by Convert), any other as it is. */
	static FString ActionText(const FMantleConvertFix& Fix);

	/** Whether Convert is on for Fixes: no fixable line that needs confirming (Strip's bare-skin line) is left unticked. */
	static bool CanConvert(TConstArrayView<FMantleConvertFix> Fixes);

	/**
	 * Chunk 5, the body row: keep PartIndex's body (bFaceOnBody false) or put its face on a MetaHuman body, and which one
	 * (Feminine: Women's; unset leaves the pick as it is). Ticks Strip's face line and at most one body line.
	 */
	static void PickBody(TArray<FMantleConvertFix>& Fixes, int32 PartIndex, bool bFaceOnBody, TOptional<bool> Feminine = {});

	/** The creator changed which of PartIndex's parts are kept (a Strip slot or piece line): the bare-skin tick is theirs to give again. */
	static void KeptPartsChanged(TArray<FMantleConvertFix>& Fixes, int32 PartIndex);

	/** The Avatar door's first look at Selection: its kind, name and bones (null or another kind: None). */
	static FMantleAvatarSight SeeKind(const UObject* Selection);

	/** What the converter found (MakeContext, then the fix list as Analyze made it, before any tick changes): unrigged, covered. */
	static void SeeContext(const FMantleConvertContext& Context, TConstArrayView<FMantleConvertFix> Fixes, FMantleAvatarSight& Sight);

	/** A MetaHuman Blueprint's pieces, from its assembly's scan: clothing meshes (one per mesh) and hair. */
	static void SeeAssembly(const FMantleAssemblyScan& Scan, FMantleAvatarSight& Sight);

	/**
	 * The "what I see" line under the Avatar picker: "SK_Vampire: Skeletal mesh, 68 bones, rigged. The body is covered by
	 * clothing." / "Static mesh, no skeleton." / "MetaHuman Creator character." / "MetaHuman Blueprint: body, 3 clothing
	 * pieces, hair." Empty for nothing picked. Pure (tested).
	 */
	static FString WhatISee(const FMantleAvatarSight& Sight);

	/**
	 * Why As is is greyed for Sight, empty when it isn't: a MetaHuman Creator character ("it's already a MetaHuman"); a
	 * static mesh or a skeleton with no usable weights only when it can't be auto-rigged here (its AutoRigNote, else "no rig
	 * to keep"). Pure (tested).
	 */
	static FString AsIsGreyedWhy(const FMantleAvatarSight& Sight);

	/** The route the Avatar door uses: Picked while it is offered (EffectiveRoute) and not greyed for Sight, else Convert. Pure (tested). */
	static EMantleAvatarRoute DoorRoute(EMantleAvatarRoute Picked, const FMantleAvatarSight& Sight);
};

/**
 * Chunk 6, "What to publish": one row of the one list after Convert, with a tick and a name the creator can change. Only
 * ticked rows are published ("Publish the ticked items" opens them one after another; an unticked row is never opened).
 */
struct FMantlePublishRow
{
	enum class EKind : uint8
	{
		/** A converted part (FMantleConvertContext::Parts): the body, or on As-is a Blueprint's clothing and hair. */
		Part,
		/** A MetaHuman Blueprint's clothing or hair kept as it is on Convert (FMantleConvertContext::Kept). */
		Kept,
		/** Hair ticked in the pack's rows (FMantlePackReview), uploaded under Hair as it is. */
		PackHair,
		/** A piece of clothing (FMantleConvertContext::Garments) that becomes a MetaHuman Outfit on the MetaHuman Convert made. */
		Garment,
	};
	EKind Kind = EKind::Part;
	/** Index into Parts, Kept or Garments; for PackHair, the pack review's piece. */
	int32 Index = INDEX_NONE;
	FString Slot;
	/** The item's name as it uploads; the creator can change it. */
	FString Name;
	bool bTicked = true;
	/** Why it can't be published from here, in plain words; empty when it can. A row with a why is greyed and never ticked. */
	FString Why;
	/** Uploaded from this list in this editor session. */
	bool bUploaded = false;
};

/** Chunk 6: the "What to publish" list, apart from Slate. */
struct FMantlePublishList
{
	/**
	 * The rows after Convert: each converted part (the body first; a part left out unticked), the Blueprint's pieces kept as
	 * they are, the hair ticked in the pack's rows (HairSlots: the contract's hair slots), then the clothing (one row per
	 * item, its trims in it; a piece that can't be an Outfit greyed with why). Pure (tested).
	 */
	static TArray<FMantlePublishRow> Make(const FMantleConvertContext& Context, const FMantlePackReview* Review, TConstArrayView<FString> HairSlots);

	/** The next row after After (INDEX_NONE: from the top) that is ticked, not greyed and not uploaded yet; INDEX_NONE when none. Pure (tested). */
	static int32 Next(TConstArrayView<FMantlePublishRow> Rows, int32 After);

	/** The clothing rows after the Outfits were made: a piece whose Outfit failed (a high heel) is greyed with why and unticked. Pure (tested). */
	static void Sync(const FMantleConvertContext& Context, TArray<FMantlePublishRow>& Rows);

	/** The row's heading word: "Body", "Top", "Bottom", "Footwear", "Hair", "Eyebrows" (the slot, capitalised; the avatar slot is "Body"). Pure. */
	static FString SlotWord(const FString& Slot);
};

/**
 * Tools > Mantle > Cosmetics... (A2.4 #283): publish hair, clothing, accessories, avatars and
 * emotes from the editor with one button. The creator picks what it is, the asset,
 * and a name; the plugin finds the body it was authored on, checks everything against Mantle's
 * rules with plain messages, exports, uploads with progress and retries, and follows the item
 * through screening (FMantleCosmeticExport, FMantleCosmeticUploadSession). Native Slate.
 */
class SMantleCosmeticPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SMantleCosmeticPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SMantleCosmeticPanel() override;

	/** The nomad tab: registered at module startup, opened from the Tools menu. */
	static const FName TabName;
	static void RegisterTab();
	static void UnregisterTab();
	static void OpenTab();

	/**
	 * Maintainer command Mantle.Dev.ConvertPanelShot <Character> <Directory> [Confirm keys]: opens the panel, picks the
	 * character under Avatar (the one door), lists its fixes (ticking the Confirm keys, joined by +: -ExecCmds splits on commas), converts it, then
	 * chooses Use on its first clothing piece that can be an Outfit. After each step the Convert section (and, after Use,
	 * the whole panel) is rendered offscreen into <Directory>/<Character>_{list,converted,used,panel}.png. Nothing uploads.
	 */
	static void DevConvertShot(const TArray<FString>& Args);

	/**
	 * #124: maintainer command Mantle.Dev.CosmeticPanelShot <File.png> [Source=<asset>] [Class=<number>] [SaveCustom=<png>] [Angle] [Exit]: opens the panel
	 * (with Source as the item, as that class) and renders it offscreen, to show the shared Thumbnail section as the cosmetic uploader has it; SaveCustom first
	 * chooses that custom image for the item, saved as the creator's choice would be.
	 */
	static void DevPanelShot(const TArray<FString>& Args);

	/**
	 * Chunk 8, agent = panel (tests): a new panel, Class chosen, Source picked and Slot chosen as a creator does, then its
	 * Check (BuildPackage). OutIssues are the issues the panel shows; returns whether the check passed.
	 */
	static bool DevCheck(EMantleCosmeticClass Class, UObject* Source, const FString& Slot, TArray<FMantleCosmeticIssue>& OutIssues);

private:
	void SetClass(EMantleCosmeticClass Class);
	void SetSource(UObject* Source);
	void SetSlot(const FString& Slot);
	void SetAuthoringBody(USkeletalMesh* Body);
	void RebuildSourcePicker();
	void RefreshSlots();
	void RefreshDetails();
	void RefreshOutfitSizes();
	TSharedRef<SWidget> MakeParametricOutfitSection();
	FReply OnCreateParametricOutfit();
	void RefreshOutfitTargets();
	/** Native Outfit authoring inputs must come from the chosen Creator character's same converted build. */
	void RefreshOutfitAuthoringDefaults();
	bool IsOutfitAuthoringCandidate(const FAssetData& Asset, bool bFace) const;

	/** Avatars: one row group per accessory socket and tool socket (hand_rSocket, no scale row), its values the body's own socket (FMantleCosmeticExport::DefaultSocketOverrides). */
	void RefreshSockets();

	/**
	 * #483, garments from a skeletal mesh: one checkbox per material section of the mesh, ticked for the chosen slot
	 * (FMantleOutfitSections::InitialKeep; skin rows disabled). Re-read with the source, re-ticked with the slot.
	 */
	void RefreshOutfitSections();

	/** Draft.KeepSlots from the ticks: unset when every section is ticked (the whole mesh), else the ticked slots. */
	void ApplyOutfitKeep();
	void RefreshClothingColors();
	/** #756: the found colour areas (tick box, name, highlight swatch) above the advanced hand-made section. */
	void AddFoundClothingAreas();
	/** #756: the creator's choice for one found area, made on first change. */
	FMantleClothingColorChoice& ClothingChoice(const FString& MaterialName, const FString& RegionId);
	/** #756: the preview colours: the highlight per found, ticked area (when on), then the hand-picked preview colours. */
	TArray<FMantleClothingColor> ClothingPreviewColorsWithHighlight() const;
	void PickClothingColor(int32 DeclarationIndex, int32 RegionIndex, bool bPreview);

	/**
	 * #594, avatars: the Jaw bone (optional) picker's rows for the chosen body (FMantleEditorActions::ApplyCosmeticJawDefaults):
	 * None, then its bones below head, the default selected. Shown only on a body whose skeleton has the jaw slot.
	 */
	void RefreshJaw();

	/**
	 * #595, avatars: the Secondary motion section's rows for the chosen body (FMantleEditorActions::ApplyCosmeticSecondaryDefaults):
	 * one per slot chain, its start bone (None, then the body's extra bones) and its preset, the suggestion selected. Shown
	 * only on a body whose skeleton has the slots.
	 */
	void RefreshSecondary();

	/** #595: Draft.SecondaryMotion's mapping for a slot chain, or null when the chain is not mapped. */
	FMantleSecondaryMapping* FindSecondaryMapping(const FString& ChainId);

	TSharedRef<SWidget> MakeSourcePicker();
	TSharedRef<SWidget> MakeSection(const FText& Title, const TSharedRef<SWidget>& Content, TAttribute<EVisibility> SectionVisibility = EVisibility::Visible);
	TSharedRef<SWidget> MakeAttachRow(const FText& Label, FVector3f& Value, float Min, float Max);

	/**
	 * A label and three numeric boxes (X, Y, Z) whose sliders span Min..Max; Target is read each time
	 * (null shows empty boxes). bClamp holds typed values inside Min..Max too; without it a value
	 * past them stays as typed, for the check to refuse with the number.
	 */
	TSharedRef<SWidget> MakeVectorRow(const TSharedRef<SWidget>& LabelWidget, TFunction<FVector3f*()> Target, float Min, float Max,
		TAttribute<bool> Enabled, bool bClamp);

	bool IsAllowedSource(const FAssetData& Asset) const;
	/** #584: a skeletal mesh on the emote's skeleton (every mesh while no animation is chosen). */
	bool IsEmoteBodyCandidate(const FAssetData& Asset) const;

	FReply OnCheck();
	FReply OnUpload();
	/** Avatars (chunk 9): checks, then walks the creator around as this avatar in the Mantle app, offline, with nothing uploaded. */
	FReply OnTestLocally();
	FReply OnCancel();
	EActiveTimerReturnType TickUpload(double InCurrentTime, float InDeltaTime);

	/** Exports and checks the draft (with a progress dialog); fills Package and the issue list. */
	bool BuildPackage();
	void ShowIssues(const TArray<FMantleCosmeticIssue>& Issues, const TArray<FString>& Notes);
	/** #51: under the checks, the hair head-fit words with one-click Looks good / Adjust (nothing for hair that was not fitted). */
	void ShowHairHeadFit();
	/** #52: the before and after pictures the hair fit card shows (null when nothing could render them). */
	TSharedPtr<FSlateDynamicImageBrush> HairFitBeforeBrush;
	TSharedPtr<FSlateDynamicImageBrush> HairFitAfterBrush;

	FText GetFilesText() const;
	FText GetStatusText() const;
	FText GetSignInText() const;
	bool IsRunning() const;

	/**
	 * The preview, in a viewport (SMantleCosmeticPreviewViewport). 3D clothing: the garment fitted the way players'
	 * clients fit it (FMantleCosmeticPreviewSession) on the picked body, Mantle's neutral reference body, the
	 * body it was made on, or a catalog avatar by id (#283). Avatars: the body posed by its retargeter (OnPreviewAvatar).
	 */
	TSharedRef<SWidget> MakePreviewSection();

	/** #124: the Thumbnail section every uploader shows, on this draft's choice. */
	TSharedRef<SWidget> MakeThumbnailSection();
	/** Reads the saved thumbnail choice for the draft's source and class (Automatic when none) and redraws the section. */
	void LoadThumbnailChoice();
	/** The creator changed the choice: kept on the draft and saved for this source; the package is built again. */
	void ThumbnailChoiceChanged(const FMantleThumbnailChoice& Choice);
	/** The thumbnail section, to redraw it after a check. */
	TSharedPtr<class SMantleThumbnailSection> ThumbnailSection;
	FReply OnPreview();
	FText GetPreviewStatusText() const;
	void ApplyPreviewAnimation();

	/**
	 * #283: the avatar preview: the creator's avatar body (and its face) posed by its skeleton's IK retargeter from the
	 * UEFN mannequin's idle, as players' clients pose avatars (FMantleAvatarPreviewPlan); the reference pose, with the
	 * reason, when this project lacks Mantle's retarget content.
	 */
	FReply OnPreviewAvatar();
	FReply OnPreviewAccessory();

	/**
	 * #736, overlays (scars, tattoos): the images section (a PNG file in place of a texture asset, the scar normal map, and the
	 * body and face the preview shows), the file picker, and the preview: the mask draped on the converted MetaHuman body or
	 * face with private dynamic instances of the shared skin materials, exactly the bytes that would upload.
	 */
	TSharedRef<SWidget> MakeOverlaySection();
	/** #54: place a flat picture on the body or face with a gizmo instead of painting the mask; baked at Preview and Upload. */
	TSharedRef<SWidget> MakeOverlayPlaceSection();
	void BrowseOverlayPicture();
	/** After the picture, the slot or the preview body/face changed: the viewport's mesh and the picture's shape follow. */
	void RefreshOverlayPlacement();
	TSharedPtr<class SMantleOverlayPlaceViewport> OverlayPlaceViewport;
	FString OverlayPlaceStatus;
	bool bOverlayPlaceStarted = false;
	void BrowseOverlayPng(bool bNormal);
	/** Asks where to save the overlay painting guide for the selected slot, then writes it. */
	void SaveOverlayGuide();
	FString OverlayGuideStatus;
	bool bOverlayGuideFailed = false;
	FReply OnPreviewOverlay();
	void RefreshAccessoryPreview();
	void SaveAccessoryPlacement();
	FString AccessorySettingsFile() const;
	FReply OnAlignAccessoryAxes();
	/** Saved (including disabled), explicitly edited, or initialized from a resolved neutral avatar anchor. */
	bool bAccessoryPlacementInitialized = false;

	/** #283: keeps PreviewPick for the rest of the editor session: the panel reopens on it. */
	void RememberPreviewPick() const;

	/**
	 * #610, the Convert section, under Avatar only (the one door): the character picked in the Avatar picker (a skeletal
	 * mesh, a static mesh, a MetaHuman Creator character or a MetaHuman Blueprint: PickAvatar), the "what I see" line, then
	 * "How should it go up?" (Convert pre-selected; As is beside it only while As-is uploads are allowed,
	 * FMantleConvertSection::ShowsRoutePicker, greyed with why for a character it can't take). It is analysed into its fix
	 * list (FMantleAvatarConverter::Analyze) with a checkbox per fix, converted into new assets under its Converted folder
	 * (Convert), and each converted part offered as a ready draft (LoadConvertedPart). The cloud note by the Convert
	 * button; Convert runs behind a progress dialog (it blocks, as Epic's cloud calls do).
	 */
	TSharedRef<SWidget> MakeConvertSection();

	/**
	 * The Avatar picker's choice: the character Convert works on (ConvertSource), with Convert pre-selected again. A skeletal
	 * mesh is also the draft's asset, as before (a body converted earlier checks and uploads straight away); any other kind
	 * leaves the draft empty until Use.
	 */
	void PickAvatar(UObject* Character);

	/** The route the door uses now (FMantleConvertSection::DoorRoute of the pick and AvatarSight). */
	EMantleAvatarRoute DoorRoute() const;
	FReply OnAnalyzeConvert();
	FReply OnRunConvert();
	void RefreshConvertRows();

	/** Forgets the fix list and what Convert did (another character or route was picked). */
	void ResetConvert();

	/** #610: the draft for converted part PartIndex: its class, slot, mesh, face, section, authoring body and name, then its check's issues. */
	void LoadConvertedPart(int32 PartIndex);

	/** The draft for Part (a converted part, a kept piece, the pack's hair) under Name; PartIndex: the part whose check's issues to show, or INDEX_NONE. */
	void LoadPart(const FMantleConvertPart& Part, const FString& Name, int32 PartIndex);

	/**
	 * Chunk 6, "What to publish": the one list after Convert (FMantlePublishList::Make), each row with its tick, its name, a
	 * thumbnail from the catalog thumbnail capture (FMantleCosmeticThumbnail; none without a renderer) and Open. Made once
	 * per Convert; ticks and names stay while the rows are redrawn.
	 */
	void MakePublishRows();
	void AddPublishRows();

	/** Opens row Row as the panel's draft (Open, or the walk), under its name. */
	void OpenPublishRow(int32 Row);

	/** The row's thumbnail (FMantleCosmeticThumbnail::Capture), or null when it can't be drawn here. */
	TSharedPtr<FSlateDynamicImageBrush> RenderPublishThumb(const FMantlePublishRow& Row) const;

	TArray<FMantlePublishRow> PublishRows;
	TArray<TSharedPtr<FSlateDynamicImageBrush>> PublishThumbs;
	/** The row opened last, and the asset it put in the draft (an upload of that asset marks the row uploaded). */
	int32 PublishOpen = INDEX_NONE;
	TWeakObjectPtr<UObject> PublishOpenSource;
	/** "Publish the ticked items" is walking the list: after each upload of the open row, the next ticked row opens. */
	bool bPublishWalk = false;

	/**
	 * A third-party avatar's separate clothing piece (FMantleConvertContext::Garments) as the panel's draft: a MetaHuman
	 * Outfit made on the MetaHuman Convert made of the avatar's body (FMantleGarmentOutfit::MakeFromWork). The first Use
	 * makes every piece that can be one, on one copy of that MetaHuman; later ones only load theirs.
	 */
	void UseConvertGarment(int32 GarmentIndex);

	/** What Convert... analyses: picked at the Avatar door, else the draft's asset. */
	UObject* GetConvertSelection() const;

	TWeakObjectPtr<UObject> ConvertSource;
	/** #625: the route picked while the section offers more than Convert (DoorRoute). */
	EMantleAvatarRoute ConvertRoute = EMantleAvatarRoute::Convert;
	/** What the door sees in ConvertSource, for the "what I see" line and As is greyed. */
	FMantleAvatarSight AvatarSight;
	/** The whole section (the line, the question, the rows), for Mantle.Dev.ConvertPanelShot's pictures. */
	TSharedPtr<SVerticalBox> ConvertSectionBox;
	TSharedPtr<FMantleConvertContext> ConvertContext;
	TArray<FMantleConvertFix> ConvertFixes;
	TOptional<FMantleConvertResult> ConvertResult;
	FString ConvertStatus;
	bool bConvertFailed = false;
	TSharedPtr<SVerticalBox> ConvertBox;

	/**
	 * #55, "Found in this pack": the pack review of the picked character (FMantleCharacterSplit::ReviewFor), when its pack has
	 * more than the one mesh. The body row picks the mesh Convert makes the MetaHuman of; the clothing rows are what it takes
	 * (FMantleConvertOptions::Review), read again when Convert runs, so every change the creator makes counts.
	 */
	TSharedPtr<FMantlePackReview> ConvertReview;
	/** Each row's "Make it" choices and the body choices, kept while their combo boxes show them. */
	TArray<TSharedRef<TArray<TSharedPtr<FMantlePackMove>>>> PackMoveOptions;
	TArray<TSharedPtr<int32>> PackBodyOptions;

	/** #55: lists the fixes for the chosen body (the review's, else the selection), keeping the review's rows. */
	void AnalyzeConvertBody();

	/** #55: the "Found in this pack" rows, above the fix list. */
	void AddPackReviewRows();

	/** Chunk 5: the body row per part (Strip's face, Women's / Men's and bare-skin lines), above the fix list. */
	void AddBodyRows();

	/** #55: rebuilds the rows on the next tick (bAnalyze: lists the fixes again first): a row's combo box outlives its own callback. */
	void RefreshConvertRowsLater(bool bAnalyze);

	FMantleCosmeticDraft Draft;
	TOptional<FMantleCosmeticPackage> Package;
	TSharedPtr<FMantleCosmeticUploadSession> Session;
	FString ContractError;

	/** What the last Test locally said (launched, or why not), shown under the buttons; empty before the first. */
	FText TestLocallyResult;
	bool bTestLocallyError = false;

	TArray<TSharedPtr<EMantleCosmeticClass>> ClassOptions;
	TArray<TSharedPtr<FString>> SlotOptions;
	TArray<TSharedPtr<FString>> CardsOptions;
	TArray<TSharedPtr<FString>> SectionOptions;
	TArray<TSharedPtr<FString>> OutfitSizeOptions;
	FString OutfitSizeError;
	TSharedPtr<SComboBox<TSharedPtr<FString>>> OutfitSizeCombo;
	/** Native source adaptation creates transient body/head/garment assets; hold them while the viewport uses them. */
	TArray<TStrongObjectPtr<UObject>> OutfitPreviewKeepAlive;
	/** The original combined source body/head, before conversion, used to author an ordinary garment. */
	TWeakObjectPtr<USkeletalMesh> ParametricSourceBodyHead;
	TArray<TStrongObjectPtr<UObject>> AuthoredOutfitKeepAlive;
	FString OutfitAuthoringStatus;
	TSharedPtr<SVerticalBox> OutfitTargetsBox;
	/** Hair from a groom's cards: the Hair colour field as typed (#RRGGBB); Draft.HairColor is set when it parses. */
	FString HairColorText;
	bool bCanGenerateHairCards = false;
	FString HairGenerationStatus;
	TArray<TSharedPtr<FString>> RoleOptions;
	/** Avatars' Publish as: "" (a new item). */
	TArray<TSharedPtr<FString>> PublishAsOptions;
	/** #594, avatars' Jaw bone (optional): "" (None) then the body's bones below head; shown while bJawSlotOffered. */
	TArray<TSharedPtr<FString>> JawOptions;
	bool bJawSlotOffered = false;
	/** #595, avatars' Secondary motion: "" (None) then the body's extra bones, and the preset ids; shown while bSecondaryOffered. */
	TArray<TSharedPtr<FString>> SecondaryBoneOptions;
	TArray<TSharedPtr<FString>> SecondaryPresetOptions;
	bool bSecondaryOffered = false;
	TSharedPtr<SVerticalBox> SecondaryBox;

	TSharedPtr<SComboBox<TSharedPtr<FString>>> SlotCombo;
	TSharedPtr<SComboBox<TSharedPtr<FString>>> JawCombo;
	TSharedPtr<SComboBox<TSharedPtr<FString>>> CardsCombo;
	TSharedPtr<SComboBox<TSharedPtr<FString>>> SectionCombo;
	TSharedPtr<SComboBox<TSharedPtr<FString>>> PublishAsCombo;
	TSharedPtr<SBox> SourcePickerBox;
	TSharedPtr<SVerticalBox> RolesBox;
	TSharedPtr<SVerticalBox> SocketsBox;
	TSharedPtr<SVerticalBox> IssuesBox;
	/** The issues ShowIssues showed last (DevCheck). */
	TArray<FMantleCosmeticIssue> ShownIssues;

	/** #483: the garment mesh's sections (material slots at LOD 0), their slot indices, pieces and ticks, by row. */
	TArray<FMantleOutfitSection> OutfitSections;
	TArray<int32> OutfitSlots;
	TArray<EMantleOutfitPiece> OutfitPieces;
	TArray<bool> OutfitKeep;
	TSharedPtr<SVerticalBox> OutfitBox;
	TSharedPtr<SVerticalBox> ClothingColorsBox;
	TArray<FMantleClothingColor> ClothingPreviewColors;
	TArray<FString> ClothingMaterialNames;
	/** #756: the last preview build's colour area detection; bClothingAreasKnown once a preview has looked. */
	TArray<FMantleClothingColorDetection> DetectedClothingColors;
	bool bClothingAreasKnown = false;
	bool bHighlightClothingAreas = true;
	bool bClothingColorsAdvancedOpen = false;

	TSharedPtr<FMantleCosmeticPreviewSession> PreviewSession;
	TSharedPtr<SMantleCosmeticPreviewViewport> PreviewViewport;
	/** #283: who the garment is previewed on, and the picker's rows (FMantlePreviewPick::Options). */
	FMantlePreviewPick PreviewPick;
	TArray<TSharedPtr<FMantlePreviewPick>> PreviewPickOptions;
	bool bPreviewAnimate = false;
	TWeakObjectPtr<UAnimSequence> PreviewAnimation;
	/** Why the preview could not read the draft (no garment, no body, no CPU data); empty when it could. */
	FString PreviewInputError;
	/** #736: the converted body and face an overlay previews on (unset: Mantle's reference body and face), and the preview's line. */
	TWeakObjectPtr<USkeletalMesh> OverlayPreviewBody;
	TWeakObjectPtr<USkeletalMesh> OverlayPreviewFace;
	FString OverlayPreviewStatus;
	bool bOverlayPreviewFailed = false;
	/** #283: the avatar preview's line for the creator, and whether it says what went wrong. */
	FString AvatarPreviewStatus;
	bool bAvatarPreviewFailed = false;
};
