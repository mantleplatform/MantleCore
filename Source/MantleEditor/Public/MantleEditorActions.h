// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleOutfitSections.h"
#include "Templates/SharedPointer.h"

class FMantleAvatarContract;
class FMantlePlacePublishSession;
class UObject;
class UWorld;
class USkeletalMesh;
enum class EMantleAvatarRoute : uint8;
enum class EMantleCosmeticClass : uint8;
enum class EMantleFitSex : uint8;
struct FMantleCosmeticDraft;
struct FMantleCosmeticIssue;

/**
 * The rows of the Cosmetics panel's Sections list for a garment from a skeletal mesh (#483): the
 * material slots LOD 0 draws, each row's material slot index, and the ticks the chosen slot starts with.
 */
struct MANTLEEDITOR_API FMantleCosmeticOutfitRows
{
	TArray<FMantleOutfitSection> Sections;
	TArray<int32> Slots;
	TArray<bool> Keep;
};

/**
 * The Tools > Mantle actions whose state lives in this module's private classes (the pasted
 * sign-in, the Mantle Publish panel), for the Mantle toolset for AI agents (MantleAgentTools,
 * #434), and the Cosmetics panel's draft defaults, which the panel and the toolset share so the
 * two cannot drift. Deliberately narrow: each call is what the menu does, without its dialogs,
 * and nothing here returns or accepts a token.
 */
struct MANTLEEDITOR_API FMantleEditorActions
{
	/** FMantleCreatorSignIn::IsSignedIn: a request would carry a creator token (pasted, or MANTLE_AUTH_TOKEN). */
	static bool IsSignedIn();

	/**
	 * What Publish (bGoLive) and Save to Mantle run once signed in: the background publish under
	 * the Mantle Publish panel (SMantlePublishPanel::Start). While one runs it only shows the panel.
	 * Callers check World and FMantleApiClient::IsConfigured first: Start answers those with a dialog.
	 */
	static void StartPublish(UWorld* World, bool bGoLive, const FString& Notes);

	/** The publish running now, or the last one this editor ran; null before the first. */
	static TSharedPtr<FMantlePlacePublishSession> GetPublishSession();

	/** True while a publish or save runs. */
	static bool IsPublishRunning();

	/**
	 * The draft Tools > Mantle > Cosmetics... holds once the creator has picked Class, Slot, Source
	 * and Name, every other setting at the panel's default: a fresh draft with the Apply* defaults
	 * below applied in the panel's order. An empty Name takes the asset's, as the panel does. FitSex is the panel's
	 * "Made for" choice (Auto by default), set before the fit bodies are made.
	 */
	static FMantleCosmeticDraft MakeCosmeticDraft(EMantleCosmeticClass Class, UObject* Source, const FString& Slot,
		const FString& Name, const FMantleAvatarContract& Contract, EMantleFitSex FitSex);

	/** The FitSex an agent tool names: "" or "auto" is Auto, "female" or "male" (any case, trimmed) that sex; false for anything else. */
	static bool ParseFitSex(const FString& Text, EMantleFitSex& OutSex);

	/**
	 * A newly picked source (the panel's SetSource): the body a garment or hair was authored on,
	 * found near it when none is chosen yet (needs Contract), and the asset's name when Name is empty.
	 */
	static void ApplyCosmeticSourceDefaults(FMantleCosmeticDraft& Draft, const FMantleAvatarContract* Contract);

	/**
	 * Hair's source details: a groom's default cards entry, and for a skeletal mesh its first
	 * section named for lashes when no section is chosen yet. Nothing for other classes.
	 */
	static void ApplyCosmeticSectionDefaults(FMantleCosmeticDraft& Draft);

	/** Avatars' socket overrides at the body's own sockets (needs Contract); cleared for every other class. */
	static void ApplyCosmeticSocketDefaults(FMantleCosmeticDraft& Draft, const FMantleAvatarContract* Contract);

	/**
	 * #483, garments from a skeletal mesh: KeepSlots for the draft's slot (FMantleOutfitSections::InitialKeep,
	 * through CosmeticKeepSlots); cleared otherwise. Returns the rows it read, empty when it cleared.
	 */
	static FMantleCosmeticOutfitRows ApplyCosmeticOutfitDefaults(FMantleCosmeticDraft& Draft);

	/**
	 * Clothing from a MetaHuman Outfit with no fit sources yet: Mantle's standard fit bodies, made automatically in memory
	 * from Epic's standard body types (Resources/GarmentFit/garment-fit.json, by default the medium-height underweight, normal and
	 * overweight bodies of one sex; Draft.FitSex overrides the sex chosen from the Outfit's own sizes), as OutfitTargets named
	 * for the type (f_med_nrw ...), so every upload carries a size for slim and broad wearers. Nothing is converted or saved and
	 * no Epic sign-in is needed. The bodies are held by Draft.FitBodies (they live as long as any copy of the draft); when they
	 * cannot be made Draft.FitBodiesStatus says why and the item uploads without variants. Contract null reads the shipped one.
	 */
	static void ApplyCosmeticFitTargetDefaults(FMantleCosmeticDraft& Draft, const FMantleAvatarContract* Contract = nullptr);

	/**
	 * The panel's "Made for" choice changed: Draft.FitSex becomes Sex and the standard fit bodies are made again for it
	 * (ApplyCosmeticFitTargetDefaults); fit sources the creator added are kept after them.
	 */
	static void SetCosmeticFitSex(FMantleCosmeticDraft& Draft, EMantleFitSex Sex, const FMantleAvatarContract* Contract = nullptr);

	/**
	 * #594, avatars: the jaw picker. On a body whose skeleton has the jaw slot (the UE5, UE4 or UEFN Mannequin, as
	 * FMantleCosmeticExport::AvatarSkeletonId identifies it; needs Contract), OutCandidates gets the body's bones below
	 * head (FMantleCosmeticChecks::JawCandidates) and Draft.JawBone the one whose name has "jaw" in it
	 * (FMantleCosmeticChecks::PreselectJawBone), else none; returns true. On any other body (a MetaHuman's jaw is on its
	 * face) or class, no candidates, Draft.JawBone none, and false: the panel hides the picker.
	 */
	static bool ApplyCosmeticJawDefaults(FMantleCosmeticDraft& Draft, const FMantleAvatarContract* Contract, TArray<FName>& OutCandidates);

	/**
	 * #595, avatars: the Secondary motion section. On a body whose skeleton has the secondary motion slots (the UE5, UE4 or
	 * UEFN Mannequin, as FMantleCosmeticExport::AvatarSkeletonId identifies it; needs Contract), OutCandidates gets the body's
	 * extra bones a chain may start at (FMantleCosmeticChecks::SecondaryCandidates, less the jaw pick) and
	 * Draft.SecondaryMotion the suggested mappings (SuggestSecondaryMappings); returns true. Run after
	 * ApplyCosmeticJawDefaults. On any other body or class, none of either, and false: the panel hides the section.
	 */
	static bool ApplyCosmeticSecondaryDefaults(FMantleCosmeticDraft& Draft, const FMantleAvatarContract* Contract, TArray<FName>& OutCandidates);

	/** Avatars' section roles from MetaHuman's slot names at the body's upload LOD (needs Contract); unchanged otherwise. */
	static void ApplyCosmeticRoleDefaults(FMantleCosmeticDraft& Draft, const FMantleAvatarContract* Contract);

	/** KeepSlots from ticks by row: unset when every row is ticked (the whole mesh), else the ticked rows' Slots. */
	static TOptional<TArray<int32>> CosmeticKeepSlots(TConstArrayView<bool> Keep, TConstArrayView<int32> Slots);

	/**
	 * Chunk 8, agent = panel: what Check and Upload do to the draft right before FMantleCosmeticExport::Build, in the panel
	 * and the toolset alike. An avatar gets its publication mode for Route ("converted" or "as_is"; cleared for every other
	 * class), and a Convert output body with no face picked its converted face (FindConvertedFace), as Use picks it.
	 * False with OutBlockers when the draft must not be built: clothing from an Outfit whose standard fit bodies could not
	 * be made (it would upload in one size).
	 */
	static bool PrepareCosmeticBuild(FMantleCosmeticDraft& Draft, EMantleAvatarRoute Route, TArray<FMantleCosmeticIssue>& OutBlockers);

	/** The route an avatar body was converted on (FMantleAvatarConverter::RouteKey): As-is for an As-is output, else Convert. */
	static EMantleAvatarRoute CosmeticRouteOf(const UObject* Body);

	/** "converted" or "as_is" for an avatar on Route; "" for every other class. Pure (tested). */
	static FString AvatarPublicationMode(EMantleCosmeticClass Class, EMantleAvatarRoute Route);

	/**
	 * The face built with a Convert output body: the skeletal mesh with a facial root under the body's build root that shares
	 * its build (FMantleAvatarConverter::BuiltKey). Null for any other body or when none is found.
	 */
	static USkeletalMesh* FindConvertedFace(const USkeletalMesh* Body);
};
