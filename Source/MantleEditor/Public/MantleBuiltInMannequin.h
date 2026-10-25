// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class FMantleAvatarContract;
class USkeletalMesh;
struct FMantleSkinnedMesh;

/**
 * The UEFN mannequin the plugin makes itself, so the avatar preview and emote upload work in a project without
 * SKM_UEFN_Mannequin. Built in memory (transient, never saved, nothing added to the plugin's Content) from the skeleton
 * the plugin's own IK rig IK_UEFN_Mannequin stores: its bone names, parents and reference pose (FIKRigSkeleton,
 * saved with the rig when it was authored on the mannequin). Every plugin retargeter, the preview's
 * RTG_UEFN_to_Mantle_* and the emotes' RTG_Mantle_*_to_UEFN, uses that rig for its UEFN side, so the built mesh has
 * exactly the bones and pose they were set up against.
 *
 * Its render mesh is one tiny triangle on the root, never drawn: the engine only animates the bones of a mesh with
 * render data. The rig has no props_root, prop_01 or poi (the uefn fixture's prop bones); no retargeter uses them, and
 * a clip retargeted onto the built mesh has no track for them, so the client keeps them in their reference pose, as it
 * does after a retarget onto the project's mannequin.
 *
 * The project's own mannequin still wins (UEFNMannequin): it can play the project's idle, which is on its skeleton.
 */
struct MANTLEEDITOR_API FMantleBuiltInMannequin
{
	/** The plugin's IK rig the skeleton is read from. */
	static const TCHAR* const RigPath;

	/**
	 * The built mesh, made on the first call and kept (rooted) for the editor's life. Null with OutError when the
	 * plugin's rig cannot be loaded or stores no usable skeleton. Game thread.
	 */
	static USkeletalMesh* Get(FString* OutError = nullptr);

	/** Mesh is the one Get makes. */
	static bool Is(const USkeletalMesh* Mesh);

	/**
	 * The project's UEFN mannequin (FMantleCosmeticExport::FindUEFNMannequin), else the built one. Null only when
	 * neither exists (OutError says why the built one could not be made). Game thread.
	 */
	static USkeletalMesh* UEFNMannequin(const FMantleAvatarContract& Contract, FString* OutError = nullptr);

	/**
	 * The mesh Get builds, from a skeleton: Bones and Parents parent-first with one root, RefPoseGlobal each bone's
	 * component-space reference transform (FIKRigSkeleton::RefPoseGlobal). The reference pose is made local to each
	 * parent with unit rotations; LOD0 is one tiny triangle weighted to the root. False with OutError when the arrays
	 * disagree in length, are empty, or are not parent-first. Pure (tested).
	 */
	static bool MakeMesh(TConstArrayView<FName> Bones, TConstArrayView<int32> Parents, TConstArrayView<FTransform> RefPoseGlobal,
		FMantleSkinnedMesh& Out, FString& OutError);
};
