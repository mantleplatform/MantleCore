// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleAvatarConvertFixer.h"
#include "MantleCosmeticExport.h"
#include "MantleCosmeticPackage.h"
#include "MantleCosmeticValidation.h"

class FJsonObject;
class FMantleAvatarContract;
struct FMantlePackReview;
class UObject;
class USkeletalMesh;

/** How FMantleAvatarConverter runs (#610). */
struct MANTLEEDITOR_API FMantleConvertOptions
{
	/**
	 * The slot a single skeletal mesh converts for ("avatar", "top", "hair", ...), as the Cosmetics panel names slots.
	 * Empty: the slot the mesh has in its assembly when it belongs to one, else "avatar". An assembly's Blueprint,
	 * body or folder ignores it: every part gets its own slot.
	 */
	FString Slot;

	/** Where the converted assets go instead of <source folder>/Converted (tests). */
	FString OutputFolder;

	/**
	 * Save the converted packages to disk (the panel and the toolset); the tests keep them in memory. #625: Convert's
	 * MHBuild step always writes (as Epic's build and the proven script do): it saves the character and its build under
	 * <output>/<Name>, whatever this says.
	 */
	bool bSave = true;

	/** Run the upload checks on each part after converting (FMantleCosmeticExport::Build without LODs or a thumbnail). */
	bool bCheck = true;

	/** #625: Convert (the default) or As-is, which MakeContext refuses while UMantleSettings::bAllowAsIsAvatars is off. */
	EMantleAvatarRoute Route = EMantleAvatarRoute::Convert;

	/**
	 * Empty: every fixer runs. Otherwise only these fixers analyse and apply (FMantleGarmentOutfit runs Strip, MHPose and
	 * MHConform to make a MetaHuman of a garment's own body, without the build).
	 */
	TSet<FName> OnlyFixers;

	/**
	 * #55: the pack review's rows ("Found in this pack", FMantleCharacterSplit::ReviewFor) as the creator left them: the selection is
	 * the body they chose, and its clothing comes from the rows (FMantleGarmentOutfit::GarmentsFromReview) instead of the pack's own
	 * scan. Null: the scan decides, as before.
	 */
	TSharedPtr<const FMantlePackReview> Review;
};

/** What the upload checks say about one converted part. */
struct MANTLEEDITOR_API FMantleConvertPartCheck
{
	int32 PartIndex = INDEX_NONE;

	/** No error: the part is ready for Upload. */
	bool bCanUpload = false;

	/** The checks' errors, warnings and notes, in their order. */
	TArray<FMantleCosmeticIssue> Issues;

	/** An avatar's standing height as the cook measures it (with its face), when the check got that far. */
	TOptional<double> StandingHeightCm;
};

/** One fix as Convert met it: the fix, its key (FMantleAvatarConverter::FixKeys), and why it failed or was left alone. */
struct MANTLEEDITOR_API FMantleConvertOutcome
{
	FMantleConvertFix Fix;
	FString Key;
	FString Reason;
};

/** What FMantleAvatarConverter::Convert did. */
struct MANTLEEDITOR_API FMantleConvertResult
{
	/** The fixes applied, in the order they ran. */
	TArray<FMantleConvertOutcome> Applied;

	/** Fixes turned on that failed (Reason: why); their problems stay for the upload checks to refuse. */
	TArray<FMantleConvertOutcome> Failed;

	/** Fixes left alone (Reason: turned off, not fixable here, or on a part left out). */
	TArray<FMantleConvertOutcome> LeftAlone;

	/** One per part that is included, in part order (empty when the options skip the checks). */
	TArray<FMantleConvertPartCheck> Checks;

	/** Assets of an earlier conversion of the same source that were deleted first. */
	int32 RemovedPrevious = 0;

	/** Some included part still has an error the upload would refuse. */
	bool HasBlockers() const;
};

/** One skeletal mesh of a MetaHuman Creator assembly besides its body and face, and the slot it uploads into. */
struct MANTLEEDITOR_API FMantleAssemblyMesh
{
	TWeakObjectPtr<USkeletalMesh> Mesh;
	FString Slot;
	/** The cloth simulation it carries (a Chaos cloth asset named for it, or its own clothing asset); empty for none. */
	FString ClothAsset;
};

/** What FMantleAvatarAssemblyFixer::Scan found in an assembly's folder. */
struct MANTLEEDITOR_API FMantleAssemblyScan
{
	/** The assembly's folder, /Game/CharacterAssemblies/Vampire. */
	FString Folder;
	/** Its display name, the folder's last segment: Vampire. */
	FString Name;

	TWeakObjectPtr<USkeletalMesh> Body;
	TWeakObjectPtr<USkeletalMesh> Face;

	/** Clothing, one entry per slot a mesh's sections name (a one-piece outfit can name several). */
	TArray<FMantleAssemblyMesh> Garments;
	TArray<FMantleAssemblyMesh> Hair;

	/** Skeletal meshes of the folder that are none of those, each with why ("SKM_X: on skeleton Y, not the body's"). */
	TArray<FString> LeftOut;

	/** Mesh's entry among Garments and Hair, or null. */
	const FMantleAssemblyMesh* Find(const USkeletalMesh* Mesh) const;
};

/**
 * The avatar converter (#610): Tools > Mantle > Cosmetics' Convert, and the toolset's ConvertCosmetic. From a selection
 * (a skeletal mesh, a MetaHuman Creator assembly's Blueprint or body, or the assembly's folder) it builds the context
 * (MakeContext), asks every registered fixer what it would fix (Analyze: the fix list the creator ticks), then runs the
 * fixers in pipeline order (Convert), each analysing the context as the fixers before it left it and applying the fixes
 * the creator left on. Every write goes to new assets under Context.OutputFolder (<source folder>/Converted): the
 * creator's assets are never modified. Converting the same source again deletes that source's earlier output first,
 * found by the package metadata the converter stamps on every asset it keeps (ConvertedFromKey). Last, it runs the
 * upload checks on each part, so the fix list's result says what still blocks an upload.
 */
class MANTLEEDITOR_API FMantleAvatarConverter
{
public:
	/** Package metadata key on every converted asset: the source it was converted from (SourceKey). */
	static const FName ConvertedFromKey;

	/** #625: package metadata key on every converted asset: the route it was converted on ("convert", "asis"). */
	static const FName RouteKey;

	/**
	 * #625: package metadata key on a MetaHuman Convert built and handed on (FMantleMetaHumanBuild::HandOff writes it on the
	 * built body and face, and nothing else does): its build root.
	 */
	static const FName BuiltKey;

	/**
	 * #625: whether Asset is a finished Convert output: stamped with ConvertedFromKey, the route "convert" and BuiltKey (a
	 * built MetaHuman handed on), in memory or on disk. The upload takes such an avatar body while As-is uploads are off,
	 * clothing-ready or not; what a Convert that stopped part-way left (a skin-only copy) is not one.
	 */
	static bool IsConvertOutput(const UObject* Asset);

	/** Registers the fixers this module implements (Assembly, Triangles) with FMantleAvatarFixers. Module startup. */
	static void RegisterBuiltInFixers();

	/**
	 * The context for Selection: a MetaHuman Creator assembly (its Blueprint, or its body skeletal mesh) yields the
	 * avatar, each garment and each hair piece as parts (MakeAssemblyContext); any other skeletal mesh one part for
	 * Options.Slot. #776: a static mesh, or a skeletal mesh with no usable weights (FMantleUnriggedBody::HasNoUsableWeights),
	 * has no skeleton: it yields one avatar part that Convert
	 * fits to a MetaHuman body (FMantleUnriggedBody), or that As-is skins to Epic's MetaHuman body skeleton, keeping its look
	 * (chunk 7, FMantleUnriggedAsIs; the part's Mesh stays null until the AsIsRig step makes it). #625: a MetaHuman Creator character asset (UMetaHumanCharacter: an Epic preset or the creator's own) one
	 * avatar part with no mesh, the context's source the character (the MetaHuman-source route, FMantleMetaHumanSourceFixer;
	 * Convert route only), output next to a character under /Game, else /Game/Converted. False with a sentence for the
	 * creator for anything else.
	 */
	static bool MakeContext(UObject* Selection, const FMantleConvertOptions& Options, const FMantleAvatarContract& Contract,
		FMantleConvertContext& OutContext, FString& OutError);

	/** MakeContext for an asset path (/Game/A/SKM_B or /Game/A/SKM_B.SKM_B) or a folder holding an assembly. */
	static bool MakeContextForPath(const FString& Path, const FMantleConvertOptions& Options, const FMantleAvatarContract& Contract,
		FMantleConvertContext& OutContext, FString& OutError);

	/**
	 * An assembly's parts: the avatar (its body, the face left for the Assembly fixer to pick, skeleton metahuman), one
	 * garment per slot its clothing names, and its hair, each cloth-simulated piece left out (bIncluded false) until
	 * the creator includes it. Source is what the creator picked (may be null for a folder).
	 */
	/**
	 * An assembly's clothing (one part per slot its meshes name, skeleton metahuman, a cloth-simulated piece not included)
	 * and hair, as parts added to OutParts: the As-is route's parts, and on Convert the pieces kept as they are
	 * (FMantleConvertContext::Kept, chunk 6). Pure on the scan's meshes (tested).
	 */
	static void AddKeptPieces(const FMantleAssemblyScan& Scan, TArray<FMantleConvertPart>& OutParts);

	static bool MakeAssemblyContext(const FMantleAssemblyScan& Scan, UObject* Source, const FMantleConvertOptions& Options,
		FMantleConvertContext& OutContext, FString& OutError);

	/** Every registered fixer's fixes for Context as it stands, in pipeline order: the fix list. Modifies nothing. */
	static TArray<FMantleConvertFix> Analyze(const FMantleConvertContext& Context);

	/**
	 * Each fix's key, "<fixer>.<part>.<kind>" ("Assembly.0.Face"), or with the fix's ordinal among its fixer's fixes for
	 * the part when it has no kind ("Triangles.2.0"): what Convert matches the creator's choices by, and the toolset's
	 * TurnOff names. Pure (tested).
	 */
	static TArray<FString> FixKeys(TConstArrayView<FMantleConvertFix> Fixes);

	/**
	 * Converts: deletes the source's earlier output under Context.OutputFolder, then for each fixer in pipeline order
	 * analyses Context again and applies each of its fixes that Choices (the fix list as the creator left it, matched by
	 * FixKeys) keeps on, that can be fixed, and whose part is included (the Assembly fixer's own fixes apply to left-out
	 * parts too: one of them includes a part). A fix Choices doesn't list keeps the fixer's default. #625: a failed Convert-route
	 * step (Strip, MHUnrigged, MHPose, MHConform, MHSource or MHBuild with its hand-off) stops the run there; no fixer after it runs. Stamps and (with
	 * Options.bSave) saves every asset the fixers created, then (Options.bCheck) runs the upload checks on each included
	 * part. #625: Context.Route is checked first: an As-is context while As-is is no longer allowed runs nothing (one Failed
	 * line). One progress frame per fixer and one for the checks (FScopedSlowTask; no Cancel).
	 */
	static FMantleConvertResult Convert(FMantleConvertContext& Context, TConstArrayView<FMantleConvertFix> Choices,
		const FMantleConvertOptions& Options, const FMantleAvatarContract& Contract);

	/** The Cosmetics panel's class for a slot: avatar, a hair slot, a garment slot. False for any other (accessories, emotes). */
	static bool ClassOfSlot(const FString& Slot, const FMantleAvatarContract& Contract, EMantleCosmeticClass& OutClass);

	/** The panel's draft for Part, every other setting at the panel's default (FMantleEditorActions' Apply* defaults). */
	static FMantleCosmeticDraft MakeDraft(const FMantleConvertPart& Part, const FMantleAvatarContract& Contract);

	/** "Avatar: Vampire", "Clothing (top): Vampire Clothing", "Hair (hair): Vampire Hair". */
	static FString DescribePart(const FMantleConvertPart& Part, const FMantleAvatarContract& Contract);

	/**
	 * For fixers: a mesh the context may change. Mesh itself when a fixer of this conversion created it, else a copy
	 * of it under Context.OutputFolder (DuplicateIntoOutput). Null with the reason when it cannot be copied.
	 */
	static USkeletalMesh* MakeWritable(FMantleConvertContext& Context, USkeletalMesh* Mesh, FString& OutError);

	/**
	 * For fixers: a copy of Source as a new asset under Context.OutputFolder (the source's name, made unique), recorded in
	 * Context.CreatedAssets. Null with the reason when it cannot be made. Source is not modified.
	 */
	static UObject* DuplicateIntoOutput(FMantleConvertContext& Context, UObject* Source, FString& OutError);

	/** What a conversion's assets are stamped with: the selection's path name, else the assembly folder. */
	static FString SourceKey(const FMantleConvertContext& Context);

	/** <Folder>/Converted for a package in Folder; a package already under a Converted folder keeps that folder. Pure (tested). */
	static FString OutputFolderFor(const FString& SourcePackagePath);

	/** Deletes the assets under OutputFolder stamped with Key (an earlier conversion of the same source). Returns how many. */
	static int32 RemovePreviousOutput(const FString& OutputFolder, const FString& Key);

	/**
	 * The fix list and, when Result is set, what Convert did, as the toolset's ConvertCosmetic returns it: {"ok",
	 * "source", "assembly", "outputFolder", "parts": [...], "fixes": [{"key", "fixer", "kind", "part", "problem",
	 * "action", "canFix", "enabled"}], "applied", "result": {...}}.
	 */
	static TSharedRef<FJsonObject> ToJson(const FMantleConvertContext& Context, TConstArrayView<FMantleConvertFix> Fixes,
		const FMantleConvertResult* Result, const FMantleAvatarContract& Contract);

	/**
	 * The toolset's ConvertCosmetic: MakeContextForPath, Analyze, and with bApply Convert (saving) with the fixes TurnOff
	 * names (comma-separated keys) switched off, the fixes Confirm names (#625: lines that need confirming) ticked, and
	 * every other at its default. One JSON object; a refusal when the path is neither a skeletal mesh, an assembly's
	 * Blueprint nor an assembly's folder.
	 */
	static FString RunForAgent(const FString& Path, const FString& Slot, bool bApply, const FString& TurnOff, const FString& Confirm = FString(),
		const FString& Route = FString(), const FMantleConvertOptions* OptionsOverride = nullptr);

	/**
	 * The body an assembly's garments are made on once this conversion has replaced it: the avatar part converted from
	 * CharacterBody when its mesh is an asset this conversion created and has Mantle's reference topology, else
	 * CharacterBody itself. The Assembly and Refit fixers use it.
	 */
	static USkeletalMesh* ConvertedBodyFor(const FMantleConvertContext& Context, USkeletalMesh* CharacterBody, const FMantleAvatarContract& Contract);
};

/**
 * #610 fixer 3: a MetaHuman Creator assembly. The converter splits an assembly into parts (MakeAssemblyContext); this
 * fixer picks what the Cosmetics panel made the creator pick by hand: the avatar's face (so its height is measured with
 * the head), the section a skeletal-mesh hair piece lifts (the hair cards), and the body garments and hair were made on
 * (the assembly's own body when it has Mantle's reference topology, the rule every garment body keeps). A cloth-simulated
 * piece is offered as a plain garment, off until the creator ticks it. It writes no assets: it points the parts.
 */
class MANTLEEDITOR_API FMantleAvatarAssemblyFixer final : public IMantleAvatarFixer
{
public:
	static const FName KindFace;
	static const FName KindSection;
	static const FName KindBody;
	static const FName KindInclude;

	virtual FName GetId() const override { return MantleAvatarFixerIds::Assembly; }
	virtual void Analyze(const FMantleConvertContext& Context, TArray<FMantleConvertFix>& OutFixes) const override;
	virtual bool Apply(const FMantleConvertFix& Fix, FMantleConvertContext& Context, FString& OutError) const override;

	/**
	 * The assembly in Folder: its body (a mesh on the MetaHuman body skeleton that is not clothing, FMantleCosmeticExport::
	 * CouldBeAuthoringBody, one under Body/ or named Body first), its face (FMantleCosmeticExport::PickAuthoringFace),
	 * its hair (a mesh under Hair/ or named for hair) and its clothing (the rest on the body skeleton), leaving out
	 * everything in a Converted folder. #625: bAllowConvertedRoot reads a Folder that is itself inside a Converted folder (a
	 * Convert build handed on, FMantleMetaHumanBuild::HandOff; FMantleConvertContext::IsHandedOffBuild), still leaving out
	 * the Converted folders below it. False with the reason when there is no body and face.
	 */
	static bool Scan(const FString& Folder, FMantleAssemblyScan& Out, FString& OutError, bool bAllowConvertedRoot = false);

	/** The folder an assembly would be in for Selection: a Blueprint's own folder, a skeletal mesh's MetaHuman folder. Empty for anything else. */
	static FString AssemblyFolderOf(const UObject* Selection);

	/** The hair slot a mesh's name says: eyebrows, eyelashes, facial_hair (beard, moustache), hair; empty when it says none. Pure (tested). */
	static FString HairSlotForName(const FString& MeshName);

	/** Whether a mesh's name says it is hair of any kind (HairSlotForName's words). Pure (tested). */
	static bool IsHairName(const FString& MeshName);

	/** The garment slot a mesh's name says (FMantleOutfitSections' words): footwear, bottom, else top. Pure (tested). */
	static FString GarmentSlotForName(const FString& MeshName);

	/**
	 * The render section a hair piece for Slot lifts: the one whose material slot or material names hair cards ("card"
	 * words first, then Slot's own words, never a scalp, helmet or fuzz), the largest by triangles on a tie, and with no
	 * name to go by the largest. INDEX_NONE for no sections. OutWhy says why, for the fix list. Pure (tested).
	 */
	static int32 PickHairSection(TConstArrayView<FString> SlotNames, TConstArrayView<FString> MaterialNames, TConstArrayView<int32> Triangles,
		const FString& Slot, FString& OutWhy);

	/** An asset name without its type prefix (SKM_, SK_, SM_, CA_, CO_, BP_): "SKM_Vampire_Cape" is "Vampire_Cape". Pure (tested). */
	static FString NameStem(const FString& AssetName);

	/** NameStem with underscores as spaces: "Vampire Cape". Pure (tested). */
	static FString DisplayName(const FString& AssetName);
};

/**
 * #610 fixer 4: a part over its slot's triangle limit. When the mesh has a lower LOD within the limit that is its own
 * geometry, a converted copy with that LOD as LOD 0 (cheaper, and better than a fresh reduction); otherwise a converted
 * copy whose LOD 0 is the editor's skeletal mesh reduction of it (skin weights kept) to TargetShare of the limit. Either
 * way the copy keeps one LOD: the upload generates its own. Hair is never reduced (its cards fall apart); a hair piece
 * with no LOD within the limit is reported, not fixed. An avatar's face is judged against the face limit on its own.
 */
class MANTLEEDITOR_API FMantleAvatarTrianglesFixer final : public IMantleAvatarFixer
{
public:
	static const FName KindMesh;
	static const FName KindFace;

	/** The share of the limit a reduction aims for. */
	static constexpr float TargetShare = 0.95f;

	virtual FName GetId() const override { return MantleAvatarFixerIds::Triangles; }
	virtual void Analyze(const FMantleConvertContext& Context, TArray<FMantleConvertFix>& OutFixes) const override;
	virtual bool Apply(const FMantleConvertFix& Fix, FMantleConvertContext& Context, FString& OutError) const override;

	/** floor(Limit x Share), at least 1. Pure (tested). */
	static int32 TargetTriangles(int32 Limit, float Share);

	/**
	 * Makes LOD LOD of Mesh (its own geometry) LOD 0 and drops every other LOD. Mesh must be a converted copy. False
	 * with the reason when that LOD is generated from another (no geometry of its own) or the rebuild fails.
	 */
	static bool PromoteLOD(USkeletalMesh* Mesh, int32 LOD, FString& OutError);

	/**
	 * Reduces LOD 0 of Mesh (a converted copy) in place to at most Target triangles with the editor's skeletal mesh
	 * reduction, skin weights (up to 8 influences) and normals kept, and drops every other LOD. False with the reason
	 * when the editor has no reduction or the result is still over Target by more than the reducer's slack (2%).
	 */
	static bool ReduceLODZero(USkeletalMesh* Mesh, int32 Target, FString& OutError);
};
