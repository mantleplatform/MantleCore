// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class FMantleAvatarContract;
class UObject;
class USkeletalMesh;

/** Mantle's neutral reference body, used only as editor refit and fitting provenance. */
struct MANTLEEDITOR_API FMantleReferenceBody
{
	/** The built body's asset path; provenance only, never a public catalog item id. */
	FString Id;

	/** The built body, checked against the contract topology. */
	USkeletalMesh* Body = nullptr;
};

/**
 * Supplies Mantle's neutral reference body to editor-only Refit/fitting tools. The plugin ships no body: the first time
 * one is needed, Build makes a blank MetaHuman Character (Epic's template, no particular person) in the creator's own
 * project under Folder and builds it as Convert builds every MetaHuman (FMantleMetaHumanBuild: Optimized / Medium, so
 * it has the reference topology). Later sessions find the saved build there. It never becomes a player default or
 * catalog entry. Nothing here builds on its own: GetAll and the rest only read what is already in the project.
 */
class MANTLEEDITOR_API FMantleReferenceBodies
{
public:
	/** Where the reference character and its build live in the creator's project (a save scope FMantleMetaHumanBuild accepts). */
	static const TCHAR* const Folder;

	/** The reference character's asset name. */
	static const TCHAR* const CharacterName;

	/** The reference body, with the contract's reference topology; false includes a clear reason. Never builds. */
	static bool GetAll(const FMantleAvatarContract& Contract, TArray<FMantleReferenceBody>& Out, FString& OutWhyNot);

	/** The one body measured against To. This does not choose between body variants. Never builds. */
	static bool GetClosest(const USkeletalMesh& To, const FMantleAvatarContract& Contract, FMantleReferenceBody& Out, FString& OutWhyNot,
		FString* OutWhy = nullptr);

	/** The reference body as a mesh, empty with the reason in OutWhyNot when unavailable. Never builds. */
	static TArray<USkeletalMesh*> FindBodies(const FMantleAvatarContract& Contract, FString* OutWhyNot = nullptr);

	/**
	 * Index into References of the body closest to Source by height and shoulder-to-hip ratio. The provider currently has
	 * one candidate; INDEX_NONE means it could not be measured. Reads only reference skeletons.
	 */
	static int32 ChooseClosest(const USkeletalMesh& Source, TConstArrayView<USkeletalMesh*> References, FString* OutWhy = nullptr);

	/** The reference body's face. Null, with why in OutNote, when it hasn't been built. Never builds. */
	static USkeletalMesh* GetFace(FString* OutNote = nullptr);

	/** Whether the project holds a built reference body and face under Folder (not whether they pass the contract). */
	static bool IsBuilt();

	/**
	 * Builds the reference body into the project: a blank MetaHuman Character Folder/CharacterName (reused if a failed
	 * build left one), built by FMantleMetaHumanBuild::Build, which asks Epic's MetaHuman cloud (sign-in and Optional
	 * Content required) and blocks for the build. True at once when it is already built. False with why; a failed build
	 * deletes what it made.
	 */
	static bool Build(FString& OutError);

	/** Whether Object is the reference body or face, with its provenance id (and :face for the face). */
	static bool IsReferenceMesh(const UObject* Object, FString* OutId = nullptr);

	/** For reports: the reference mesh's provenance, else the object's path; empty for null. */
	static FString Describe(const UObject* Object);

	/** A whole fix line for an unavailable reference body: "Can't: " + WhyNot. */
	static FString UnavailableText(const FString& WhyNot);

	/** Drops the cached meshes (MantleEditor's ShutdownModule, and after Build). */
	static void Shutdown();
};
