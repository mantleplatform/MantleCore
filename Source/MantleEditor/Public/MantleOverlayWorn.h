// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "UObject/StrongObjectPtr.h"

class UMaterialInterface;
class USkeletalMesh;
class FMantleAvatarContract;
struct FMantleCosmeticPackage;
struct FMantleThumbnailSubject;

/**
 * A converted MetaHuman body (or face) wearing an overlay (a scar or a tattoo) as the game draws it: the shared skin material with the exported mask (and a scar's
 * normal map) on the skin sections only. The Cosmetics panel's Preview shows it in its viewport; the overlay's Automatic thumbnail (#124) draws it with the
 * catalog thumbnail capture, the avatar wearing the mark.
 */
struct MANTLEEDITOR_API FMantleOverlayWorn
{
	USkeletalMesh* Body = nullptr;
	USkeletalMesh* Face = nullptr;
	/** Skin material per slot, on the body and on the face (one of them, by the overlay's slot). */
	TMap<int32, UMaterialInterface*> BodyMaterials;
	TMap<int32, UMaterialInterface*> FaceMaterials;
	/** The slot is a face slot (the face is what wears it). */
	bool bFace = false;
	/** A skin section got the material: false when the shared skin material is not in the project or no skin could be told, and then the mesh shows as it is. */
	bool bDrawn = false;
	/** Plain words about what was left out, empty when all is well. */
	FString Note;
	/** Keeps the textures and the material instance alive. */
	TArray<TStrongObjectPtr<UObject>> KeepAlive;

	/**
	 * Reads Built's exported mask (and normal) and dresses PickedBody and PickedFace, else Mantle's reference body and face. False with a plain reason in OutRefusal when
	 * there is nothing to show it on or the textures cannot be made. Needs the editor and a renderer only to draw, not to make.
	 */
	static bool Make(const FMantleCosmeticPackage& Built, const FString& Slot, const FMantleAvatarContract& Contract, USkeletalMesh* PickedBody,
		USkeletalMesh* PickedFace, FMantleOverlayWorn& Out, FString& OutRefusal);

	/** The thumbnail subject: a face slot is the headshot of the face on its body, a body slot the whole standing body with its face. */
	FMantleThumbnailSubject ToSubject() const;
};
