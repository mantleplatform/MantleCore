// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleAvatarConvertFixer.h"

class FMantleAvatarContract;
class USkeletalMesh;

/**
 * #610, the RigMap fixer (MantleAvatarFixerIds::RigMap): a character rigged for Mixamo, Character Creator / iClone, Daz
 * Genesis or any other third-party rig is mapped onto a supported avatar skeleton, so the upload identifies it as that
 * skeleton (FMantleCosmeticExport::AvatarSkeletonId). Its bones are renamed, re-parented and completed: required bones
 * the rig lacks are inserted with no weights, unweighted bones with nothing kept below them (Mixamo's *_End) are dropped,
 * and weighted bones the target has no place for stay, to fold into their nearest kept ancestor at upload (#592). The
 * mesh is scaled to centimetres and turned Z-up, facing +Y with its left side on +X (the skeleton fixtures' axes). The
 * pose is left as it is: the Repose fixer after this one takes it to the skeleton's A-pose.
 *
 * Known rigs are data (Resources/AvatarConvert/rig-profiles.json, FMantleRigProfiles). An unknown rig gets a suggested
 * mapping by name, hierarchy and position (FMantleAvatarRigMap::Suggest) that the creator checks and completes in
 * SMantleRigMappingPanel, and may save as a profile of their own.
 */

/** One bone of a creator's rig, as the fixer reads it: raw reference skeleton order (parent-first). */
struct MANTLEEDITOR_API FMantleRigBone
{
	FName Name;
	int32 Parent = INDEX_NONE;

	/** Parent-relative reference pose, as the mesh has it (any units, any axes). */
	FTransform Local;

	/** Some vertex of some LOD weights it. */
	bool bWeighted = false;
};

/** One target bone of a mapping and the rig's bone that becomes it. */
struct MANTLEEDITOR_API FMantleRigMappingEntry
{
	/** The target skeleton's bone ("upperarm_l"). */
	FName Target;

	/** The rig's bone as the mesh names it ("mixamorig:LeftArm"); None while unmapped. */
	FName Source;

	/** One of limits.json's requiredBones for the target skeleton. */
	bool bRequired = false;
};

/** Source bone to target bone, per target bone of the chosen skeleton. */
struct MANTLEEDITOR_API FMantleRigMapping
{
	/** The profile it came from ("mixamo"), empty for a suggested or hand-made mapping. */
	FString ProfileId;

	/** For the fix list ("Mixamo"); empty for an unknown rig. */
	FString ProfileName;

	/** The contract skeleton it maps onto ("ue4mannequin"). */
	FString TargetSkeletonId;

	/** Required bones first (in the fixture's order), then any optional bones mapped (fingers, twist bones). */
	TArray<FMantleRigMappingEntry> Entries;

	/** The rig's bone mapped to Target, None when there is none. */
	FName SourceFor(FName Target) const;

	/** The target bone Source is mapped to, None when it is not mapped. */
	FName TargetFor(FName Source) const;

	/** Maps Target to Source (None unmaps it), adding the entry when it has none; a source mapped elsewhere is moved. */
	void Set(FName Target, FName Source, bool bRequired = false);

	/** The bones the converter cannot insert (FMantleAvatarRigMap::MustMapBones) that are not mapped, in entry order. */
	TArray<FName> MissingBones() const;

	int32 NumMapped() const;
};

/** One known rig (rig-profiles.json, or a creator's saved mapping). */
struct MANTLEEDITOR_API FMantleRigProfile
{
	FString Id;
	FString Name;
	FString Target;

	/** Regular expressions stripped from the start of a bone name (after its namespace), the first that matches. */
	TArray<FString> Prefixes;

	/** Stripped source bone name, lower case, to target bone. */
	TMap<FString, FName> Bones;

	/** Read from <Project>/Saved/Mantle/RigProfiles rather than the plugin. */
	bool bSaved = false;
};

/** The rig profile table: the plugin's and the creator's saved ones. */
struct MANTLEEDITOR_API FMantleRigProfiles
{
	/** Reads one profile file's text ({"version": 1, "profiles": [...]}). False with the reason. */
	static bool Parse(const FString& Json, TArray<FMantleRigProfile>& Out, FString& OutError);

	/** <MantleCore>/Resources/AvatarConvert/rig-profiles.json. */
	static FString BuiltInPath();

	/** <Project>/Saved/Mantle/RigProfiles: where Save as profile writes. */
	static FString SavedDirectory();

	/** The plugin's profiles, read once; empty (logged) when the file cannot be read. */
	static const TArray<FMantleRigProfile>& BuiltIn();

	/** The creator's saved profiles (read each call, so a profile saved a moment ago is known) then the plugin's. */
	static TArray<FMantleRigProfile> All();

	/** Bone as Profile compares it: its namespace and the first matching prefix stripped, lower case. */
	static FString StripName(const FMantleRigProfile& Profile, FName Bone);

	/** Bone without its FBX namespace (up to the last ':' or '|'). */
	static FString StripNamespace(const FString& Bone);

	/**
	 * Out: Profile's mapping of Bones (the first bone to claim a target wins), target Profile.Target, with an entry for
	 * each of Required. Returns how many of Bones it maps.
	 */
	static int32 Match(const FMantleRigProfile& Profile, TConstArrayView<FMantleRigBone> Bones, TConstArrayView<FString> Required, FMantleRigMapping& Out);

	/**
	 * Writes Mapping as a profile named Name to SavedDirectory() (<id>.json, id from the name), so the rig is known
	 * from now on. False with the reason.
	 */
	static bool Save(const FMantleRigMapping& Mapping, const FString& Name, FString& OutPath, FString& OutError);

	/** Mapping as a one-profile file's text. */
	static FString ToJson(const FMantleRigMapping& Mapping, const FString& Id, const FString& Name);
};

/** One bone of the converted skeleton. */
struct MANTLEEDITOR_API FMantleRigPlanBone
{
	FName Name;
	int32 Parent = INDEX_NONE;

	/** Parent-relative and component-space reference pose after the scale and axis correction, unit scale. */
	FTransform Local;
	FTransform Component;

	/** The rig's bone it is, INDEX_NONE for an inserted bone. */
	int32 Source = INDEX_NONE;

	/** Renamed onto the target skeleton (else an inserted bone or an extra kept under its own name). */
	bool bMapped = false;
	bool bInserted = false;
};

/** What converting one rig does (FMantleAvatarRigMap::MakePlan): pure data, applied by ApplyPlan. */
struct MANTLEEDITOR_API FMantleRigPlan
{
	FString TargetSkeletonId;

	/** Parent-first; [0] is the target's root. */
	TArray<FMantleRigPlanBone> Bones;

	/** Per rig bone: its bone in Bones, INDEX_NONE when dropped. */
	TArray<int32> NewIndexOfSource;

	/** Per rig bone: the bone its weights go to (itself, or for a dropped bone its nearest kept ancestor). */
	TArray<int32> WeightsOfSource;

	/** Per rig bone: its component-space reference pose as the mesh has it. */
	TArray<FTransform> SourceComponent;

	/** Rig bones left out (unweighted, nothing kept below them). */
	TArray<FName> Dropped;

	/** Required bones the rig lacks, added with no weights. */
	TArray<FName> Inserted;

	/** Optional mappings left out because the rig's hierarchy disagrees with the skeleton's (they fold instead). */
	TArray<FName> Unmapped;

	/** Extra bones renamed because their names are bones of a supported skeleton: (old, new). */
	TArray<TPair<FName, FName>> RenamedExtras;

	int32 NumMapped = 0;

	/** Component space of the mesh to the converted one: uniform Scale, a turn by a multiple of 90 degrees, then Offset. */
	FTransform Correction;
	double Scale = 1.0;
	bool bTurned = false;

	/** The fix list's action: "maps 52 bones onto the UE4 Mannequin skeleton, adds root, ...". */
	FString Describe(const FString& RigName) const;
};

/** The RigMap fixer's steps, each testable on its own. */
struct MANTLEEDITOR_API FMantleAvatarRigMap
{
	/**
	 * The bones the converter never inserts, because they carry the body: pelvis, head and each side's upperarm,
	 * lowerarm, hand, thigh, calf and foot. The other required bones (root, spine_01 to spine_03 and more, neck_01, the
	 * clavicles, ball) are inserted with no weights when a rig lacks them.
	 */
	static const TArray<FName>& MustMapBones();

	/** Mesh's raw reference skeleton, each bone marked weighted when a vertex of any LOD weights it. */
	static TArray<FMantleRigBone> ReadBones(const USkeletalMesh* Mesh);

	/**
	 * True when Bones carry, by name, every required bone of some avatar_body skeleton and the upload would identify them as
	 * that skeleton (FMantleCosmeticChecks::FitSkinToClass on the bones): nothing for this fixer. #774: a UE4-style rig with
	 * weighted bones of another supported skeleton on top (a Paragon hero's upperarm_twist_02_l) is not, and gets mapped.
	 */
	static bool IsOnSupportedSkeleton(TConstArrayView<FMantleRigBone> Bones, const FMantleAvatarContract& Contract);

	/**
	 * The best profile of Profiles that names every must-map bone (most bones matched; the earlier on a tie), with
	 * bOutKnown. Otherwise Suggest's mapping onto the UE4 Mannequin, with any partly matching profile's bones laid over
	 * it, and bOutKnown false.
	 */
	static FMantleRigMapping Detect(TConstArrayView<FMantleRigBone> Bones, const FMantleAvatarContract& Contract,
		TConstArrayView<FMantleRigProfile> Profiles, bool& bOutKnown);

	/**
	 * A guided mapping's first guess onto Target (TargetId in Contract): by hierarchy and name. The pelvis is where the
	 * legs and the spine meet (the feet's and head's common ancestor), the top spine bone where the arms and the neck
	 * meet; hands, feet and head are found by name (hand, wrist, foot, ankle, head, with the side from l, left, r, right
	 * tokens); the limb bones are the chain between them, twist and roll bones skipped; the ball is the foot's toe child;
	 * fingers by name. Unfound bones are left unmapped.
	 */
	static FMantleRigMapping Suggest(TConstArrayView<FMantleRigBone> Bones, const FMantleAvatarContract& Contract, const FString& TargetId);

	/**
	 * Converts Bones by Mapping onto its target skeleton: the scale and axis correction (Bounds, the mesh's reference
	 * bounds in its own space, sets the scale; invalid takes the bones' extent), the kept, dropped and inserted bones,
	 * hierarchy checked (an optional mapping that disagrees with the skeleton's hierarchy is unmapped, a required one
	 * refuses). False with the creator's message. bCompleteChains also inserts every skeleton bone above a mapped one that
	 * the rig lacks (neck_02, the metacarpals), not only the required ones: the upload checks a garment's bones against
	 * their parents in the skeleton (FMantleCosmeticChecks::MatchSkeleton).
	 */
	static bool MakePlan(TConstArrayView<FMantleRigBone> Bones, const FMantleRigMapping& Mapping, const FMantleAvatarContract& Contract,
		const FBox& Bounds, FMantleRigPlan& Out, FString& OutError, bool bCompleteChains = false);

	/**
	 * The upload's identification on the plan's bones (FMantleCosmeticChecks::FitSkinToClass, avatar bodies, root
	 * required, a bone weighted when a rig bone's weights go to it): true when it names the plan's target. bGarment checks
	 * against the skinned garment class (the MetaHuman body skeleton) instead.
	 */
	static bool VerifyPlan(TConstArrayView<FMantleRigBone> Bones, const FMantleRigPlan& Plan, const FMantleAvatarContract& Contract, FString& OutError,
		bool bGarment = false);

	/**
	 * A clothing piece on the UE4 Mannequin hierarchy (every required bone of ue4mannequin by name, no spine_04 or
	 * spine_05: the clavicles and neck hang from spine_03), which the upload never takes as clothing. The RigMap fixer
	 * maps it onto the MetaHuman body skeleton (UE4MannequinOntoMetaHuman) so the Refit fixer can carry it onto the body.
	 */
	static bool IsUE4MannequinGarment(TConstArrayView<FMantleRigBone> Bones, const FMantleAvatarContract& Contract);

	/**
	 * Bones' UE4 Mannequin skeleton onto the MetaHuman body skeleton: each bone onto its namesake, but the spine by
	 * height (spine_01 onto spine_02, spine_02 onto spine_04, spine_03, which carries the clavicles and neck, onto
	 * spine_05). MakePlan with bCompleteChains inserts spine_01, spine_03, neck_02 and the metacarpals with no weights.
	 */
	static FMantleRigMapping UE4MannequinOntoMetaHuman(TConstArrayView<FMantleRigBone> Bones, const FMantleAvatarContract& Contract);

	/**
	 * Writes the converted mesh and a new skeleton for it under OutputFolder (never touching Source): every LOD with
	 * source geometry re-weighted onto the new bones and corrected, morph targets and sockets kept (a socket on a
	 * dropped bone moves to the bone that took its weights), the physics asset and post-process animation cleared (they
	 * name the old bones). Returns the mesh, OutCreated the mesh and skeleton; null with the reason.
	 */
	static USkeletalMesh* ApplyPlan(USkeletalMesh* Source, const FMantleRigPlan& Plan, const FString& OutputFolder,
		TArray<UObject*>& OutCreated, FString& OutError);

	/** The mapping the creator made for Mesh in SMantleRigMappingPanel this session; Analyze and Apply use it over Detect. */
	static void SetSessionMapping(const USkeletalMesh* Mesh, const FMantleRigMapping& Mapping);
	static const FMantleRigMapping* FindSessionMapping(const USkeletalMesh* Mesh);

	/** "UE4 Mannequin" for ue4mannequin, and so on; the id itself for one it does not know. */
	static FString SkeletonDisplayName(const FString& SkeletonId);
};

/** Registers the RigMap fixer with FMantleAvatarFixers (MantleEditor startup). */
MANTLEEDITOR_API void RegisterMantleAvatarRigMapFixer();

