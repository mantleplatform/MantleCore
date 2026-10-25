// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleCharacterScaleRule.h"

/**
 * #582: one chain of a skeleton fixture's referencePose: the unit direction from one bone to another in the
 * skeleton's reference pose, component space (Unreal axes: X to the body's left, Y forward, Z up).
 */
struct MANTLEEDITOR_API FMantleContractReferenceChain
{
	/** As the fixture spells them. */
	FString From;
	FString To;
	FVector Direction = FVector::ZeroVector;
};

/** One skeleton fixture (schema/avatar/skeleton-*.json): bone names and parents, and its sockets. */
struct MANTLEEDITOR_API FMantleContractSkeleton
{
	/** The contract id: metahuman, metahuman_face, ue5mannequin, ue4mannequin, uefnmannequin, uefn. */
	FString Id;

	/** Parent-first, one root. */
	TArray<FName> Bones;
	TArray<int32> Parents;

	/** Socket name to bone name. */
	TMap<FName, FName> Sockets;

	/** Index of Bone, INDEX_NONE when it is not on this skeleton. */
	int32 Find(FName Bone) const;

	/** Bone's parent's name; NAME_None for the root or a bone not on this skeleton. */
	FName ParentOf(FName Bone) const;

	FName Root() const { return Bones.Num() > 0 ? Bones[0] : NAME_None; }

	TMap<FName, int32> IndexOf;

	/**
	 * #582: the fixture's referencePose: what its reference pose is called ("MetaHuman A-pose", "UE5 Mannequin
	 * A-pose") and its chains. Empty for a fixture without one: the cook and the plugin then skip the pose rule for
	 * bodies on it.
	 */
	FString ReferencePoseName;
	TArray<FMantleContractReferenceChain> ReferenceChains;

	bool HasReferencePose() const { return !ReferencePoseName.IsEmpty() && ReferenceChains.Num() > 0; }
};

/** One item class's mesh limits (limits.json contentLimits.*). */
struct MANTLEEDITOR_API FMantleContractClass
{
	int32 MaxTriangles = 0;
	int32 MaxSections = 0;
	int32 MaxMaterials = 0;
	int32 MaxInfluences = 8;
	int32 AlbedoMax = 0;
	int32 NormalMax = 0;
	int32 OrmMax = 0;

	/** Contract skeleton ids a mesh of this class may be skinned to, in the order the worker tries them. */
	TArray<FString> Skeletons;
};

/** One slot of limits.json's slot table. */
struct MANTLEEDITOR_API FMantleContractSlot
{
	FString Id;
	FString Group;
	FString ItemClass;
	FString Socket;
	FString SocketBone;
	int32 Cap = 1;
};

/**
 * One of limits.json's toolSockets (#585): a socket an avatar may override besides the accessory
 * sockets (hand_rSocket, which carries tools). It hangs from SocketBone on every appearance
 * skeleton; with bUnitScale its override keeps scale 1 (the API's avatar.Contract.ToolSocket).
 */
struct MANTLEEDITOR_API FMantleContractToolSocket
{
	FString Socket;
	FString SocketBone;
	bool bUnitScale = false;
};

/** An accessory slot's size limits (limits.json rigid_accessory.boundsPerSlot). */
struct MANTLEEDITOR_API FMantleContractAccessoryBounds
{
	float MaxDimensionCm = 0.0f;
	float MaxReachCm = 0.0f;
};

/**
 * The avatar catalog contract as the plugin checks it: limits.json, the reference body topology
 * and the skeleton fixtures, read from the copies the plugin ships under
 * Resources/AvatarContract (derived data only: no bodies, no meshes). The cook
 * worker embeds the same files (backend/internal/cook/contract), so a creator hears here, before
 * uploading, exactly what the worker would refuse. The copies must stay equal to schema/avatar.
 */
class MANTLEEDITOR_API FMantleAvatarContract
{
public:
	FMantleContractClass AvatarBody;
	FMantleContractClass AvatarFace;
	FMantleContractClass RigidAccessory;
	FMantleContractClass SkinnedGarment;

	TArray<FMantleContractSlot> Slots;
	/** limits.json toolSockets, in its order (#585). */
	TArray<FMantleContractToolSocket> ToolSockets;
	TMap<FString, FMantleContractAccessoryBounds> AccessoryBounds;

	/** By contract id; a fixture the plugin does not ship yet (ue5mannequin before A2.5) is absent. */
	TMap<FString, FMantleContractSkeleton> Skeletons;

	/** body-topology-v1.json: the reference LOD's vertex count and quantised UV hash. */
	int32 TopologyVertexCount = 0;
	uint32 TopologyUVHash = 0;

	float HeightMinCm = 0.0f;
	float HeightMaxCm = 0.0f;
	float WidthMaxCm = 0.0f;
	float DepthMaxCm = 0.0f;
	float FeetToleranceCm = 10.0f;

	/**
	 * #582, limits.json contentLimits.avatar_body.requiredBones: by skeleton id, the bones an avatar body on it
	 * must carry, as limits.json spells them. A skeleton with no list requires none.
	 */
	TMap<FString, TArray<FString>> AvatarRequiredBones;

	/**
	 * #582, avatar_body.referencePoseShape: how far (degrees) a reference chain may point from the skeleton
	 * fixture's direction, and how far each axis of a bone's reference-pose scale may be from 1. The cook's
	 * starting values when the file has no such block or its numbers make no sense.
	 */
	double ChainToleranceDegrees = 20.0;
	double BoneScaleTolerance = 0.01;

	/**
	 * limits.json characterScale (#581): how tall the game plays an avatar (its character, hitboxes and camera
	 * follow the avatar's standing height, clamped). What "Plays at X cm" says; the rule's defaults when the file
	 * has no such block or its numbers make no sense.
	 */
	FMantleCharacterScaleRule CharacterScale;

	/**
	 * Resources/GarmentFit/garment-fit.json (plugin-only, not in the synced limits.json): the standard MetaHuman body types a clothing item's garment variants are fitted to, without
	 * the sex (the plugin adds f_ or m_): srt|med|tal then unw|nrw|ovw, in order, at most 8 (the backend's variant limit).
	 * Variant ids are the full type names (f_med_nrw). Medium height, three builds, when the file has no such block.
	 */
	TArray<FString> GarmentFitBodyTypes = { TEXT("med_unw"), TEXT("med_nrw"), TEXT("med_ovw") };
	/** garment-fit.json sexMarginCm: how much closer to the male body than the female one an Outfit's sizes must be to be fitted to male bodies. */
	float GarmentFitSexMarginCm = 0.25f;

	float BindToleranceCm = 0.5f;
	float BindToleranceDegrees = 1.0f;
	TArray<FName> KneeBones;

	float CoverageThresholdCm = 3.0f;
	int32 WarpPointCount = 750;

	/** limits.json lods.targetFractions: each LOD's share of LOD0's triangles, LOD0 first; what the plugin generates. */
	TArray<float> LODTargetFractions = { 1.0f, 0.5f, 0.25f };

	TArray<FString> HairSlots;
	TMap<FString, int32> HairMaxTrianglesPerSlot;
	/** Triangle cap for a skinned slot; other garments use the shared garment cap. */
	int32 SkinnedMaxTriangles(const FString& Slot) const;
	int32 HairMaxTextureSize = 2048;

	int32 ThumbnailSize = 512;

	float EmoteFrameRate = 30.0f;
	float EmoteMaxSeconds = 10.0f;
	float EmoteMaxRootCm = 50.0f;
	int32 EmoteMaxEvents = 64;
	int32 EmoteMaxBytes = 524288;
	TArray<FName> EmoteRequiredBones;
	TArray<FString> EmoteEvents;
	FString EmoteSkeleton = TEXT("uefn");

	FName FaceJawBoneDefault = TEXT("FACIAL_C_Jaw");

	int64 MaxUploadBytes = 67108864;
	int32 MaxTextureSize = 2048;

	/** limits.json placeAssets.staticMesh (F1 #335): one uploaded place mesh, what Check this place and the cook both enforce. */
	int32 PlaceMeshMaxTriangles = 500000;
	int32 PlaceMeshMaxVertices = 1500000;
	int32 PlaceMeshMaxSections = 8;

	/** limits.json placeAssets.sound (#324): one uploaded place sound, and the place's decoded audio budget. */
	int32 PlaceSoundMinSampleRate = 22050;
	int32 PlaceSoundMaxSampleRate = 48000;
	double PlaceSoundMaxSeconds = 60.0;
	int64 PlaceSoundMaxSourceBytes = 12582912;
	int64 PlaceSoundMaxDecodedBytes = 67108864;

	/** placeAssets.staticMesh.lods (#470): LODs with LOD0, and each LOD's triangle cap, LOD0 first. */
	int32 PlaceMeshMaxLODs = 3;
	TArray<int32> PlaceMeshLODMaxTriangles = { 500000, 250000, 125000 };

	/** placeAssets.staticMesh.collision (#470): the simple collision an uploaded place mesh may carry. */
	int32 PlaceMeshMaxCollisionPrimitives = 64;
	int32 PlaceMeshMaxConvexHulls = 64;
	int32 PlaceMeshMaxHullVertices = 255;
	int32 PlaceMeshMaxTotalHullVertices = 8192;

	/** limits.json placeAssets.skeletalMesh and animClip (#469): a place skeletal mesh on its own skeleton, and a clip for it. */
	int32 PlaceSkeletalMaxTriangles = 100000;
	int32 PlaceSkeletalMaxSections = 8;
	int32 PlaceSkeletalMaxBones = 256;
	double PlaceClipMaxSeconds = 60.0;
	double PlaceClipMaxFrameRate = 60.0;
	int64 PlaceClipMaxCookedBytes = 8388608;

	/** Where the plugin keeps its copies: <MantleCore>/Resources/AvatarContract. */
	static FString DefaultDirectory();

	/** Reads the limits, topology and skeleton files and requires the retired avatar registry to be empty. */
	static bool LoadFromDirectory(const FString& Directory, FMantleAvatarContract& Out, FString& OutError);

	/** The plugin's contract, read once. Null (and OutError set) when the plugin's copy cannot be read. */
	static const FMantleAvatarContract* Get(FString* OutError = nullptr);

	const FMantleContractSlot* FindSlot(const FString& SlotId) const;

	/** Slots whose itemClass is ItemClass (avatar_body, skinned_garment, rigid_accessory, emote). */
	TArray<FString> SlotsOfClass(const FString& ItemClass) const;

	/**
	 * The accessory sockets (limits.json slots[].socket), each once, in the slot table's order: the
	 * names an avatar's "sockets" overrides may use (the API's avatar.Contract.IsSocket).
	 */
	TArray<FString> AccessorySockets() const;

	/** A tool socket an avatar may also override (limits.json toolSockets, #585), or null. */
	const FMantleContractToolSocket* FindToolSocket(const FString& Socket) const;

	const FMantleContractSkeleton* FindSkeleton(const FString& Id) const { return Skeletons.Find(Id); }
};
