// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleCosmeticExport.h"
#include "Engine/SkeletalMesh.h"
#include "MetaHumanBodyType.h"
#include "MetaHumanCharacter.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

class FMantleAvatarContract;


/** One standard MetaHuman body type made in memory: a Creator character that was never saved, and the meshes the editor evaluates for it. */
struct MANTLEEDITOR_API FMantleFitBody
{
	/** The variant id, which is the body type's own name (f_med_nrw). */
	FString Id;
	bool bEditing = false;
	TStrongObjectPtr<UMetaHumanCharacter> Character;
	TStrongObjectPtr<USkeletalMesh> BodyMesh;
	TStrongObjectPtr<USkeletalMesh> FaceMesh;
	TStrongObjectPtr<USkeletalMesh> MergedHeadAndBodyMesh;
	/** FMantleFitBodies::FaceNumberingHash of FaceMesh; 0 when it could not be read. */
	uint32 FaceNumberingHash = 0;
};

/**
 * The fit bodies of one item, kept alive for as long as any draft holds this set (drafts copy the pointer), and
 * released when the last one lets go: the characters leave MetaHuman Creator's editing list and can be collected.
 */
struct MANTLEEDITOR_API FMantleFitBodySet
{
	TArray<FMantleFitBody> Bodies;
	EMantleFitSex Sex = EMantleFitSex::Female;
	/** What was chosen and why, for the log and the check's notes. */
	FString Status;

	FMantleFitBodySet() = default;
	FMantleFitBodySet(const FMantleFitBodySet&) = delete;
	FMantleFitBodySet& operator=(const FMantleFitBodySet&) = delete;
	~FMantleFitBodySet();
};

/**
 * Garment variants fitted to Epic's standard MetaHuman body types, made IN MEMORY: no converted Aera/Bo assets in the
 * project and no Epic sign-in or cloud build. Epic ships 18 fixed compatibility bodies (f|m x srt|med|tal x unw|nrw|ovw);
 * garment-fit.json bodyTypes names the ones to use (the medium-height builds by default), for the Outfit's sex.
 */
struct MANTLEEDITOR_API FMantleFitBodies
{
	/** Epic's enum value for a type name such as "f_med_ovw"; false for anything but the 18 fixed bodies. */
	static bool ParseBodyType(const FString& Name, EMetaHumanBodyType& OutType);

	/** True for a type name without its sex: "med_ovw" (srt|med|tal then unw|nrw|ovw). */
	static bool IsBodyTypeSuffix(const FString& Suffix);

	/** limits.json's list kept to valid, distinct suffixes, at most MaxVariants of them, in order. */
	static TArray<FString> SanitizeBodyTypes(const TArray<FString>& Raw, int32 MaxVariants = 8);

	/** The variant ids (the type names) for one sex, in the list's order. Auto is read as Female. */
	static TArray<FString> VariantIds(EMantleFitSex Sex, const TArray<FString>& Suffixes);

	/**
	 * Female or male from how far the Outfit's authored size bodies are from the standard female and male
	 * medium/normal bodies (average vertex distance in cm, one entry per size; a negative entry was unreadable).
	 * Male only when its nearest size is closer than the female one by more than MarginCm; unclear is Female.
	 */
	static EMantleFitSex ChooseSex(const TArray<float>& FemaleCm, const TArray<float>& MaleCm, float MarginCm, FString& OutWhy);

	/**
	 * Makes the body type's character, selects the body type, commits it and fetches the editor's body, face and merged
	 * head+body meshes. The character stays registered for editing until Release. Game thread only.
	 */
	static bool Make(const FString& Id, EMetaHumanBodyType Type, FMantleFitBody& Out, FString& OutError);

	/**
	 * Fingerprint (a diagnostic, logged by Mantle.Dev.NeckCheck) of how a face numbers its triangles: a hash, in triangle order, of each triangle's UV centroid on the face LOD with the
	 * reference face topology (6872 triangles; else LOD0). UVs are the same on every MetaHuman face, so the hash does not depend on the face's
	 * shape, only on the numbering. The client pairs a collar's samples by triangle index, so a face must number its triangles like Epic's built
	 * faces. 0 when unreadable.
	 */
	static uint32 FaceNumberingHash(const USkeletalMesh* Face);

	/** Unregisters the character from editing and drops every reference, so the objects can be collected. */
	static void Release(FMantleFitBody& Body);

	/**
	 * Makes the fit set for an Outfit: Sex (or, for Auto, the sex its authored size bodies are nearest to) times the
	 * contract's garmentFit body types. False with OutError when a body could not be made.
	 */
	static bool MakeSet(UObject* Outfit, EMantleFitSex Requested, const FMantleAvatarContract& Contract,
		TSharedPtr<FMantleFitBodySet>& OutSet, FString& OutError);

	/**
	 * The mesh of this body to upload as the source body shape: the first of the editor's body mesh (all of Creator's LODs,
	 * one of which is Mantle's 8568-vertex reference layout) and its merged head+body mesh that has a LOD with the reference
	 * topology. Null when neither has; OutLog then lists each mesh's LOD vertex counts.
	 */
	/** Per LOD of Mesh, why it is or is not the reference topology: what reading it gave, counts, UV hash and the layout match's error. */
	static FString DescribeReferenceTopology(const USkeletalMesh* Mesh, const FMantleAvatarContract& Contract);

	static USkeletalMesh* PickShapeBody(const FMantleFitBody& Body, const FMantleAvatarContract& Contract, FString& OutLog, FString* OutOrdering = nullptr);

	/**
	 * True for the body meshes Make fetched from MetaHuman Creator (bodies made in memory, before Epic's Optimized build). Their LOD with the
	 * reference vertex count may differ from the reference only in a few flipped quad diagonals (FMantleBodyCorrespondence::MatchFlippedDiagonals),
	 * which FMantleCosmeticExport::HasReferenceTopology accepts for these meshes and no others.
	 */
	static bool IsCreatorPreview(const USkeletalMesh* Mesh);

	/** The client's neck base (the highest body vertex within 12 cm of the z axis) of the body that goes with a face Make fetched; false for any other face. */
	static bool NeckBaseZ(const USkeletalMesh* Face, float& OutZ);

	/** LOD's vertex positions (component space, cm) from the render data, or the mesh description when there is no CPU copy. */
	static bool ReadVertices(const USkeletalMesh* Mesh, int32 LODIndex, TArray<FVector3f>& OutPositions);

	/** For each vertex of Probe, the distance to the nearest vertex of Reference: average and largest, in cm. Order-independent. */
	static bool CompareNearest(const TArray<FVector3f>& Probe, const TArray<FVector3f>& Reference, float& OutAverageCm, float& OutMaxCm);
};
