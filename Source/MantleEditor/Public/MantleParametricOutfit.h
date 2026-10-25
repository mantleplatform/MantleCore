// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleHeadFit.h"
#include "MantleOutfitSections.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/SoftObjectPath.h"

class UObject;
class UChaosOutfitAsset;
class USkeletalMesh;
class FMantleAvatarContract;
struct FMantleCosmeticDraft;
struct FMantleGlbMesh;
class UMaterialInterface;

struct MANTLEEDITOR_API FMantleOutfitSourceSize
{
	FString Name;
	TArray<FSoftObjectPath> BodyParts;
	TArray<FString> Pieces;
	/** Native logical Outfit identities contributing pieces to this body size. */
	TArray<FGuid> OutfitGuids;
};

/** Editor authoring input only. Evaluates authored sources, retaining the existing Mantle cook and runtime fit. */
struct MANTLEEDITOR_API FMantleParametricOutfit
{
	static bool IsSource(const UObject* Source);

	/** A skeletal mesh or a Chaos cloth asset that is not an Outfit: clothing WrapGarment can make a resizable Outfit of. */
	static bool IsGarmentSource(const UObject* Source);

	/** Garment as the skeletal mesh the export reads: itself, or a cloth asset's render geometry made into a transient one (kept alive by KeepAlive). */
	static USkeletalMesh* GarmentMesh(UObject* Garment, TArray<TStrongObjectPtr<UObject>>& KeepAlive, FString& OutError);

	/**
	 * A skeletal-mesh (or Chaos cloth asset) garment as a transient one-size Outfit, so it resizes like any MetaHuman Outfit.
	 * Only the sections drawn with KeepSlots (the garment's material slots; all when unset) go in. SourceBody is the combined
	 * body and head the garment was authored on (FindSourceBody). The source is never modified; KeepAlive must outlive the Outfit.
	 * The Outfit's material slots are the kept sections' only, so a Draft made from it takes no KeepSlots.
	 */
	static UChaosOutfitAsset* WrapGarment(UObject* Garment, const TOptional<TArray<int32>>& KeepSlots, USkeletalMesh* SourceBody,
		TArray<TStrongObjectPtr<UObject>>& KeepAlive, FString& OutError);

	/**
	 * The combined body and head the garment authored on AuthoringBody (a body Mantle converted from a MetaHuman Creator
	 * character) was made for: that character's merged mesh. The character stays open for editing while OutLease lives,
	 * which must outlive the wrapped Outfit's use. False with a sentence when AuthoringBody is not such a body.
	 */
	static bool FindSourceBody(const USkeletalMesh* AuthoringBody, TSharedPtr<void>& OutLease, USkeletalMesh*& OutBody, FString& OutError);

	/**
	 * Pure (tested): whether a converted body is a third-party character's, from the class of the asset it was converted from
	 * (its ConvertedFromKey; empty when that is no asset, as an assembly folder is): any asset but a MetaHuman Creator
	 * character. Its folder can't tell: Convert puts a Creator character's output under <character>/Converted too.
	 */
	static bool IsThirdPartyConversion(const FTopLevelAssetPath& SourceClass);

	/**
	 * Route 1: the garment wrapped as an Outfit on its MetaHuman authoring body (Draft.AuthoringBody, a Mantle-converted Creator
	 * body, which must be the body the garment sits on: checked by how close its closest vertices are to the skin). Null with OutWhy.
	 */
	static UChaosOutfitAsset* WrapOnAuthoringBody(const FMantleCosmeticDraft& Draft, TArray<TStrongObjectPtr<UObject>>& KeepAlive,
		TSharedPtr<void>& Lease, FString& OutWhy);

	/** Pure (tested): Points (component space, cm) span a whole body's height, feet to head. */
	static bool IsWholeBody(TConstArrayView<FVector3f> Points);

	/** Pure (tested): Sections (and the mesh's name) are a bare character body: no clothing section, a skin section (or a name for a body), not a head. */
	static bool IsBareBody(TConstArrayView<FMantleOutfitSection> Sections, const FString& MeshName);

	/** The character's bare body beside Garment (its folder or up to two above) whose separate clothing, by the Convert list's matching, includes Garment. */
	static USkeletalMesh* FindBodyBeside(USkeletalMesh* Garment);

	/**
	 * Route 2: Make Outfit from garment (FMantleGarmentOutfit::Make) on the bare body beside the garment, whole mesh only; Convert's bare-skin tick is confirmed automatically for that body (OutNote says so). The
	 * Outfit is saved next to the body (<body folder>/Converted) and reused when it was made from this garment before. Null with OutWhy.
	 */
	static UChaosOutfitAsset* MakeOutfitBeside(const FMantleCosmeticDraft& Draft, TArray<TStrongObjectPtr<UObject>>& KeepAlive, FString& OutWhy, FString* OutNote = nullptr);

	/**
	 * Draft's skeletal-mesh or cloth-asset garment as a draft of an Outfit with the standard fit bodies as targets (what the
	 * Outfit route builds a variant for), by the first route that applies: WrapOnAuthoringBody, then MakeOutfitBeside. False
	 * with OutWhy when neither does (or the fit bodies failed): the caller then uploads the garment in its one size.
	 * OutRoute says which route made it. Lease and KeepAlive must outlive OutDraft.
	 */
	static bool PrepareWrapped(const FMantleCosmeticDraft& Draft, const FMantleAvatarContract& Contract, FMantleCosmeticDraft& OutDraft,
		TArray<TStrongObjectPtr<UObject>>& KeepAlive, TSharedPtr<void>& Lease, FString& OutWhy, FString* OutRoute = nullptr);
	static bool ListSizes(UObject* Source, TArray<FMantleOutfitSourceSize>& OutSizes, FString& OutError);

	/**
	 * Converts Draft's native source (auto-selected when a character is supplied) into transient skinned render geometry.
	 * With OutfitAuthoringCharacter, the native Collection pipeline evaluates its configured resizing graph
	 * onto that character; AuthoringBody/Face must be that character's final converted output, unless bOutfitAuthoringInMemory
	 * says the character is a standard body type made in memory (FMantleFitBodies), which needs neither.
	 * Without a character, the authored body/head must have the complete reference layout.
	 * Does not combine source sizes or preserve simulation.
	 * Without a character, KeepSlots (the Outfit's material slots) carries over to the exported mesh, and kept sections
	 * over the slot's triangle limit are reduced to 95% of it; OutNote then says so for the creator.
	 * KeepAlive must outlive OutDraft (including preview). Source assets are never modified.
	 */
	static bool Prepare(const FMantleCosmeticDraft& Draft, const FMantleAvatarContract& Contract,
		FMantleCosmeticDraft& OutDraft, TArray<TStrongObjectPtr<UObject>>& KeepAlive, FString& OutError, FString* OutNote = nullptr);

	/** A transient skeletal mesh of all of Mesh's primitives on its own joints, with Materials per glTF material (held by KeepAlive). */
	static USkeletalMesh* MakeMesh(const FMantleGlbMesh& Mesh, const TArray<UMaterialInterface*>& Materials, TArray<TStrongObjectPtr<UObject>>& KeepAlive,
		FString& OutError);

	/**
	 * Converts Draft's Outfit into a rigid accessory (a hat, a shirt tied round the waist): one authored size's kept sections,
	 * reduced to the accessory limit, as a transient static mesh whose pivot is the slot's socket bone (limits.json
	 * socketBone, where Mantle's socket sits at identity). A hat (acc_hat) is also fitted from the head it was authored on
	 * to the reference head (FMantleHeadFit), so each avatar's fitted hat socket puts it on that avatar's head; other slots'
	 * sockets are not fitted per avatar, so their pieces keep the authored size. OutNote says what was reduced and measured. KeepAlive must outlive OutDraft.
	 */
	static bool PrepareRigid(const FMantleCosmeticDraft& Draft, const FMantleAvatarContract& Contract,
		FMantleCosmeticDraft& OutDraft, TArray<TStrongObjectPtr<UObject>>& KeepAlive, FString& OutError, FString* OutNote = nullptr);

	/** Where PrepareRigid hangs a piece for an accessory slot: its socket bone, and whether the head-size fit applies (acc_hat only). */
	struct FRigidPivot
	{
		FString Bone;
		bool bHeadFit = false;
	};
	static bool RigidPivot(const FMantleAvatarContract& Contract, const FString& Slot, FRigidPivot& OutPivot, FString& OutError);

	/** A source vertex (component space, cm) as PrepareRigid stores it: in Pose's frame (the socket bone's), the head fit undone. */
	static FVector3f RigidPosition(const FVector3f& Position, const FTransform& Pose, const FMantleHeadFit::FFit& Fit);
	static FVector3f RigidNormal(const FVector3f& Normal, const FTransform& Pose, const FMantleHeadFit::FFit& Fit);
};
