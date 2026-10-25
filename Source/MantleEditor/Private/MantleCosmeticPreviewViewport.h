// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "PreviewScene.h"
#include "SEditorViewport.h"
#include "UObject/GCObject.h"
#include "UObject/ObjectPtr.h"

class FEditorViewportClient;
class UAnimationAsset;
class UMaterialInterface;
class USkeletalMesh;
class USkeletalMeshComponent;
class UStaticMesh;
class UStaticMeshComponent;
struct FMantleCosmeticTransform;
struct FMantleAnimClip;
struct FMantlePreviewView;

/**
 * #283: what the avatar preview shows: the creator's avatar body (and its face) posed by the avatar skeleton's IK
 * retargeter from a hidden UEFN driver playing Idle, as the client poses avatars (FMantleAvatarPreviewPlan). Without
 * a retargeter or a driver, or without IKRig's retarget node, the body holds its reference pose.
 */
struct FMantleAvatarPreviewScene
{
	USkeletalMesh* Body = nullptr;
	/** Follows the body by leader pose; null shows none. */
	USkeletalMesh* Face = nullptr;
	/** A UIKRetargeter, as a UObject (MantleEditor does not link IKRig). */
	UObject* Retargeter = nullptr;
	USkeletalMesh* Driver = nullptr;
	/** #535: the plugin's idle, a MAN1 clip on the UEFN skeleton looped on the driver by UMantleClipAnimInstance; wins over Idle. */
	TSharedPtr<const FMantleAnimClip> IdleClip;
	/** Looped on the driver when it is on the driver's skeleton; else the driver holds its reference pose. */
	UAnimationAsset* Idle = nullptr;
	/** What was shown (the body's path): the camera frames again only when another body shows. */
	FString Key;
};

/**
 * The cosmetic panel's preview viewport (A2.4 #283): the engine's thumbnail scene (its sky, lights and floor, the
 * setup FMantleCosmeticThumbnail captures in) in an interactive editor viewport, showing the preview session's
 * wearer with the fitted garment and the face following the body by leader pose. Reference pose, or a looping
 * animation on the body's skeleton. Draws only what the session built; never computes anything itself.
 */
class SMantleCosmeticPreviewViewport : public SEditorViewport, public FGCObject
{
public:
	SLATE_BEGIN_ARGS(SMantleCosmeticPreviewViewport) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SMantleCosmeticPreviewViewport() override;

	/** Shows View's body, face and garment (nothing unless it is Ready). Frames the body when it changes. */
	void Show(const FMantlePreviewView& View);

	/** A rigid accessory on the contract's MetaHuman bone anchor, with the published attach/socket composition. */
	bool ShowAccessory(UStaticMesh* Mesh, const FString& Slot, const FMantleCosmeticTransform& Attach,
		const TMap<FString, FMantleCosmeticTransform>& Sockets, FString& OutError);

	/**
	 * #283: shows an avatar body posed through its retargeter (see FMantleAvatarPreviewScene) instead of a fitted
	 * garment, until the next Show. True when the body is retargeted from the driver; false when it holds its
	 * reference pose (or InScene has no body, which shows nothing new).
	 */
	bool ShowAvatar(const FMantleAvatarPreviewScene& InScene);

	/**
	 * #736: a converted body (and its face, following by leader pose) in the reference pose, with Body/FaceMaterials
	 * (material slot index to material) over the meshes' own. The camera frames the face when bFrameFace, else the body,
	 * and only when Key (the mesh paths) changes. False when there is no body.
	 */
	bool ShowOverlay(USkeletalMesh* Body, USkeletalMesh* Face, const TMap<int32, UMaterialInterface*>& BodyMaterials,
		const TMap<int32, UMaterialInterface*>& FaceMaterials, bool bFrameFace, const FString& Key);

	/** Loops Animation on the body; null holds the reference pose. Kept but not applied while an avatar shows (its driver plays the idle). */
	void SetAnimation(UAnimationAsset* Animation);

	/** The body shown, for filtering animations by skeleton; null when nothing is shown. */
	USkeletalMesh* GetShownBody() const;

	// FGCObject
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual FString GetReferencerName() const override { return TEXT("SMantleCosmeticPreviewViewport"); }

protected:
	virtual TSharedRef<FEditorViewportClient> MakeEditorViewportClient() override;

private:
	void ApplyAnimation();
	void Frame(const USkeletalMesh* Body);

	/** Hides and empties every component, the driver too. */
	void ClearStage();

	/** Back from an avatar to the garment preview: the driver stops and the body no longer waits on it. */
	void LeaveAvatarMode();

	TUniquePtr<FPreviewScene> Scene;
	TSharedPtr<FEditorViewportClient> Client;

	TObjectPtr<USkeletalMeshComponent> BodyComponent;
	TObjectPtr<USkeletalMeshComponent> FaceComponent;
	TObjectPtr<USkeletalMeshComponent> GarmentComponent;
	TObjectPtr<UStaticMeshComponent> AccessoryComponent;
	/** #283: the avatar preview's hidden UEFN driver, whose pose the body's retarget reads. */
	TObjectPtr<USkeletalMeshComponent> DriverComponent;
	TObjectPtr<UAnimationAsset> Animation;
	/** #283: the retargeter the shown avatar is posed through (kept alive while shown). */
	TObjectPtr<UObject> AvatarRetargeter;

	/** An avatar shows (ShowAvatar) rather than a fitted garment. */
	bool bAvatarMode = false;

	/** The target last framed (FMantlePreviewTarget::Key): the camera moves only when another target shows. */
	FString FramedTarget;
};
