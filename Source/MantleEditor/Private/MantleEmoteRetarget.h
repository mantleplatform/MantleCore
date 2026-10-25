// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

class FMantleAvatarContract;
class UObject;
class UAnimSequence;
class USkeletalMesh;
class USkeleton;
struct FReferenceSkeleton;
struct FMantleAnimClipData;
struct FMantleSkinJointName;

/**
 * #535: an emote made on a MetaHuman, UE5 Mannequin or (#593) UE4 Mannequin body, retargeted onto the UEFN mannequin
 * when it is uploaded. Emotes play on the hidden UEFN driver and every body follows it through the client's retargeters,
 * so the cook takes UEFN bones only (backend/internal/cook/animclip.go). Here, at upload, a clip whose skeleton is the contract's
 * metahuman, ue5mannequin or ue4mannequin skeleton goes through the plugin's reverse retargeter
 * (RTG_Mantle_MetaHuman_to_UEFN, RTG_Mantle_UE5Mannequin_to_UEFN and RTG_Mantle_UE4Mannequin_to_UEFN, which ship in the
 * plugin content) with IK Rig's batch retarget into TransientFolder, is sampled as
 * MAN1 exactly like a UEFN clip, and the temporary copy is deleted. Nothing is saved and the creator's clip is untouched.
 * #609: a clip made on a UEFN Mannequin body needs no reverse retargeter: its skeleton is the emote skeleton itself (uefn,
 * tried first by IdentifySkeleton), so it is sampled as it is, the identity the reverse of RTG_UEFN_to_Mantle_UEFNMannequin
 * would be, and plays on every body through their forward retargeters.
 *
 * UIKRetargetBatchOperation::RunBatchRetarget is BlueprintCallable, so it is called by reflection (its UFunction, its
 * input struct's properties by name), the way the avatar preview reaches IK Rig's retarget node: MantleEditor takes no
 * module dependency on IKRigEditor. The target is the project's UEFN mannequin when it has one, else the one the plugin
 * builds in memory from its own IK rig (FMantleBuiltInMannequin::UEFNMannequin), so no mannequin has to be added. Game thread.
 */
struct FMantleEmoteRetarget
{
	/**
	 * Where the retargeted copy lives while it is sampled: a folder under /Game (the asset tools create nothing under
	 * /Temp), in memory only, never saved; everything the retarget made there is dropped again right after.
	 */
	static const TCHAR* const TransientFolder;

	/**
	 * The plugin's reverse retargeter for an appearance skeleton id, an object path: metahuman, ue5mannequin and
	 * ue4mannequin (case-sensitive). Null for any other id.
	 */
	static const TCHAR* RetargeterPathFor(const FString& SkeletonId);

	/**
	 * Which contract skeleton Skeleton is, by its bones and their parents (FMantleCosmeticChecks::MatchSkeleton): the
	 * emote skeleton (uefn: no retarget) first, then metahuman, ue5mannequin and ue4mannequin. Then, for a skeleton
	 * with bones of its own (extra twist, prop or IK bones), the one whose every body bone (IsBodyBone) it carries under
	 * the same parent, the most specific first; its own bones are left out. Empty when it is none of them. OutWhy: how it
	 * was read when it has bones of its own, or, when it is none, the nearest skeleton and the first difference.
	 */
	static FString IdentifySkeleton(const USkeleton* Skeleton, const FMantleAvatarContract& Contract, FString* OutWhy = nullptr);

	/** IdentifySkeleton on a joint table (FMantleCosmeticChecks::JointsOf). Pure. */
	static FString IdentifyJoints(TConstArrayView<FMantleSkinJointName> Joints, const FMantleAvatarContract& Contract, FString* OutWhy = nullptr);

	/** A bone of the body itself: not an IK, virtual, corrective, prop or helper bone (interaction, center_of_mass, attach, poi). */
	static bool IsBodyBone(FName Bone);

	/**
	 * mantle #840 step 1, rest-pose match: the bones whose rest shape the target's retarget pose copies from the source's,
	 * each hand and its fingers (metacarpals and 01 to 03 of the thumb, index, middle, ring and pinky). Unreal's auto-align
	 * writes no offsets for fingers, so the FK finger chains otherwise carry the difference between the two rest hands into
	 * every frame (a constant 17 to 35 degree under-curl). The arms are left to the retargeter: matching them too put the
	 * wrists further from the source's in the trials.
	 */
	static TArray<FName> HandBones();

	/**
	 * The target retarget pose's local rotation offsets (IK Rig's: retarget local = reference local * offset) that give
	 * each of Bones the source retarget pose's component-space rotation, parent-first, so the two rest hands have the same
	 * shape. Every Epic skeleton Mantle retargets from (UE4 and UE5 Mannequin, MetaHuman) shares the UEFN mannequin's bone
	 * axes; a bone with one matched child is turned the little more that points it at that child as the source's points,
	 * since the skeletons place a finger's next joint a little differently. Offsets are each skeleton's current ones by bone
	 * name (absent: identity); a bone missing on either skeleton is left alone. OutLargestDegrees: the largest change from
	 * the target's current offset. Pure.
	 */
	static TMap<FName, FQuat> MatchRestRotations(const FReferenceSkeleton& Source, const TMap<FName, FQuat>& SourceOffsets,
		const FReferenceSkeleton& Target, const TMap<FName, FQuat>& TargetOffsets, TConstArrayView<FName> Bones, double* OutLargestDegrees = nullptr);

	/**
	 * A copy of Retargeter (an IK retargeter) in the transient package whose target retarget pose gives the hands and
	 * fingers (HandBones()) the source's rest shape (MatchRestRotations), its finger chains retargeted joint for joint (OneToOne), and
	 * its metacarpal chains held at that rest pose when Source has no metacarpals, so FK finger chains carry the source's curl.
	 * Source and Target: the meshes' reference skeletons the retarget runs between. Rooted until ReleaseRestHandsCopy; the
	 * shipped asset is never touched. Null with OutError when IK Rig's controller cannot edit it. Game thread.
	 */
	static UObject* MakeRestHandsCopy(UObject* Retargeter, const FReferenceSkeleton& Source, const FReferenceSkeleton& Target, FString& OutNote, FString& OutError);
	static void ReleaseRestHandsCopy(UObject* Copy);

	/**
	 * The same match on Retargeter itself (the game's RTG_UEFN_to_Mantle_* retargeters, Mantle.Dev.MatchRetargetHands),
	 * fingers only: hand_l and hand_r keep their rest pose, so a weapon in a hand socket keeps its place and turn (turning
	 * the hands tilted it 4.8 degrees). Its target retarget pose and finger chains are changed in memory and its package
	 * dirtied; the caller saves it.
	 */
	static bool MatchRestHandsInPlace(UObject* Retargeter, const FReferenceSkeleton& Source, const FReferenceSkeleton& Target, FString& OutNote, FString& OutError);

	/**
	 * Removes the constant wrist error from Retargeter (the game's RTG_UEFN_to_Mantle_*): the hands' retarget pose offsets left a
	 * fixed twist about the forearm (26 degrees on the MetaHuman) against the source's, so every frame's wrist was off by that
	 * much. Only hand_l and hand_r are turned, about the forearm axis, by what the two retarget poses differ from their
	 * reference poses in twist; the fingers follow the hand. The bend at the wrist is not touched (unsigned angle, see the PR). A held weapon tilts with the hand. In
	 * memory, package dirtied; the caller saves it.
	 */
	static bool MatchWristsInPlace(UObject* Retargeter, const FReferenceSkeleton& Source, const FReferenceSkeleton& Target, FString& OutNote, FString& OutError);

	/**
	 * A mesh on Skeleton to retarget from (its proportions): the skeleton's preview mesh, else a skeletal mesh in the
	 * project on that skeleton, a "Body" first, then by package path. Null when there is none.
	 */
	static USkeletalMesh* FindSourceMesh(USkeleton* Skeleton);

	/**
	 * Retargets Sequence (on SkeletonId's skeleton, played on SourceMesh) onto TargetMesh (a UEFN mannequin) through
	 * RetargeterPathFor(SkeletonId) (a MakeRestHandsCopy of it, so the fingers keep the source's curl; OutHandsNote says what
	 * that changed), samples the copy with FMantleAnimClipWriter::Sample, and deletes the copy. False with
	 * a sentence for the creator when anything is missing or the retarget does not give one animation on the target's
	 * skeleton with Sequence's length (within a frame).
	 */
	static bool RetargetAndSample(UAnimSequence* Sequence, const FString& SkeletonId, USkeletalMesh* SourceMesh, USkeletalMesh* TargetMesh,
		const FMantleAvatarContract& Contract, FMantleAnimClipData& OutClip, FString& OutError, FString* OutHandsNote = nullptr);

	/**
	 * The retarget itself, through any retargeter: IK Rig's batch retarget of Sequence (played on SourceMesh) onto
	 * TargetMesh through Retargeter into TransientFolder, checked to be one animation on TargetMesh's skeleton whose length
	 * is Sequence's within LengthTolerance seconds, handed to Use while it exists, then deleted. False with a sentence when
	 * the retarget fails or Use does (Use sets OutUseError, which becomes OutError). RetargetAndSample runs through it.
	 */
	static bool RetargetThrough(UAnimSequence* Sequence, USkeletalMesh* SourceMesh, USkeletalMesh* TargetMesh, UObject* Retargeter,
		double LengthTolerance, TFunctionRef<bool(UAnimSequence& Retargeted, FString& OutUseError)> Use, FString& OutError);

	/**
	 * #584: the body the clip plays on while it is retargeted (its proportions shape the result), first of: the
	 * sequence's own preview mesh, PickedBody (the panel's "Animated on" body), FindSourceMesh(its skeleton); each only
	 * when it is on the sequence's skeleton. OutFrom, when given, says which one it was, for the notes.
	 */
	static USkeletalMesh* ChooseSourceMesh(UAnimSequence* Sequence, USkeletalMesh* PickedBody, FString* OutFrom = nullptr);

	/**
	 * RetargetAndSample from ChooseSourceMesh(Sequence, PickedBody) onto FMantleBuiltInMannequin::UEFNMannequin.
	 * OutNote says what was done, for the package's notes.
	 */
	static bool Sample(UAnimSequence* Sequence, const FString& SkeletonId, const FMantleAvatarContract& Contract, FMantleAnimClipData& OutClip,
		FString& OutError, FString& OutNote, USkeletalMesh* PickedBody = nullptr);
};
