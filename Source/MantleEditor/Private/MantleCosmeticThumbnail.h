// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"
#include "UObject/StrongObjectPtr.h"

class FPreviewScene;
class USkeletalMesh;
class USkeletalMeshComponent;
class UMaterialInterface;
class UStaticMesh;

/** What a cosmetic thumbnail shows, and how the camera frames it. */
struct FMantleThumbnailSubject
{
	/**
	 * Drawn together in their reference pose at the origin, sharing one component space: a body and
	 * its MetaHuman face, or a garment on the body it was authored on (both skinned to the same
	 * skeleton, so their reference poses line up; the plan's section 3.3 and the A0.1 proof).
	 */
	TArray<USkeletalMesh*> SkeletalMeshes;

	/**
	 * #483: per entry of SkeletalMeshes, the material slots it does not draw (a garment's sections its item leaves
	 * out: the rest of a one-piece outfit, the source character's skin). Absent entries draw every slot.
	 */
	TArray<TArray<int32>> HiddenSlots;

	/**
	 * #124: per entry of SkeletalMeshes, materials drawn in place of the mesh's own, by slot (an overlay's skin material with its mask drawn on the body).
	 * Absent entries draw their own. KeepAlive holds what these name (transient instances) as long as the subject lives.
	 */
	TArray<TMap<int32, UMaterialInterface*>> MaterialOverrides;
	TArray<TStrongObjectPtr<UObject>> KeepAlive;

	/** An accessory, alone, at its pivot. */
	UStaticMesh* StaticMesh = nullptr;

	/** Where StaticMesh is drawn: identity, or turned upright as worn (an accessory made from a skinned source). */
	FTransform StaticTransform = FTransform::Identity;

	/** StaticMesh's back faces (FMantleCosmeticThumbnail::BackFaces), drawn with it: the inside of an open cloth surface. */
	TStrongObjectPtr<UStaticMesh> StaticBackFaces;

	/** What the camera fits into the frame (cm, the meshes' space). */
	FBox Frame = FBox(ForceInit);

	/** Camera direction: pitch and yaw in degrees. */
	FRotator View = FRotator::ZeroRotator;

	/** Draw the thumbnail scene's floor under a standing body (feet at z = 0). */
	bool bFloor = false;

	/** For the note and the log: "SKM_Ada with SKM_Ada_Face, headshot". */
	FString Describe;

	/**
	 * Unset (every catalog thumbnail): the skeletal meshes hold their reference pose. Set (Mantle.Dev.CheckEmote's picture
	 * sheet): called once the skeletal meshes are in the capture scene, one component per SkeletalMeshes entry in order
	 * (null for a null entry), before anything is drawn; it poses them, and may add what poses them (a hidden driver) to
	 * Scene, which drops it with the scene. It may move InOutFrame (starting as Frame) onto the posed meshes: a close-up
	 * of a hand where the pose puts it.
	 */
	TFunction<void(FPreviewScene& Scene, TConstArrayView<USkeletalMeshComponent*> Components, FBox& InOutFrame)> Pose;

	bool IsEmpty() const { return SkeletalMeshes.Num() == 0 && !StaticMesh; }
};

/**
 * #364: cosmetic thumbnails by scene capture (A2.4 #283's 512 x 512 thumbnail). The editor's
 * thumbnail renderer draws one asset, so a MetaHuman body showed without its face and a garment
 * floated on its own; this draws the item as a player would see it, in a neutral pose (the
 * reference pose): an avatar as a headshot, head and shoulders, with its face (every avatar
 * upload's thumbnail is a headshot); a garment on the body it was
 * authored on, framed on the garment; an accessory alone. It renders in the engine's own thumbnail
 * scene (its sky, lights and floor, FThumbnailPreviewScene) through a USceneCaptureComponent2D
 * into a render target, with the thumbnail renderer's show flags.
 *
 * Needs a renderer: CanCapture is false under -NullRHI (the headless tests), in a commandlet
 * without rendering, or without the editor, and the caller falls back to the thumbnail renderer.
 */
class FMantleCosmeticThumbnail
{
public:
	/** #124: the direction the camera looks along by default: a worn item's (a body, a garment, an avatar) or an accessory's. Choose the angle starts from it. */
	static FRotator DefaultView(bool bAccessory);

	/** The camera's field of view (degrees): narrow, so a body is not distorted. */
	static constexpr float FieldOfViewDegrees = 30.0f;

	/** Room left around the framed box, as a factor of its projected half size. */
	static constexpr float FrameMargin = 1.08f;

	/**
	 * How far below the head joint a headshot's frame reaches, and its half width, in heads (the
	 * joint to the top). With the camera's margin the picture then shows the head (crown to chin) at about 57% of its
	 * height, a little room above the crown, the neck and the top of the shoulders, ending just below the collarbones (the
	 * reference body's proportions). Narrower than tall, so its height sets the fit.
	 */
	static constexpr double HeadshotDownHeads = 1.2;
	static constexpr double HeadshotHalfWidthHeads = 0.9;

	/** A head (joint to top) is taken to be at least this and at most this (cm), whatever the meshes say. */
	static constexpr double HeadshotMinHeadCm = 8.0;
	static constexpr double HeadshotMaxHeadCm = 40.0;

	/**
	 * A headshot, the body and its face (Face may be null), framed by HeadshotFrame on the body's (else the face's) head
	 * bone in the reference pose (HeadJointOf). No floor (it never shows).
	 */
	static FMantleThumbnailSubject AvatarSubject(USkeletalMesh* Body, USkeletalMesh* Face);

	/**
	 * The headshot's frame for a standing avatar whose meshes together fill Bounds: the head, neck and the top of the
	 * shoulders. A head is the head joint to the top of Bounds (HeadshotMinHeadCm to HeadshotMaxHeadCm); without a joint,
	 * the top of the body as the conform's portrait takes it (FMantleMetaHumanConform::NoHeadDropCm below the top, the
	 * middle of Bounds). The frame is flat at the joint's depth, centred on the joint across, from HeadshotDownHeads heads
	 * below the joint (never below Bounds) to one head above it, HeadshotHalfWidthHeads heads to each side. Invalid for an
	 * invalid Bounds. Pure.
	 */
	static FBox HeadshotFrame(const FBox& Bounds, const TOptional<FVector>& HeadJoint);

	/** The "head" bone of Mesh in its reference pose, component space (cm); unset without a mesh or such a bone. */
	static TOptional<FVector> HeadJointOf(const USkeletalMesh* Mesh);

	/**
	 * A garment on its authoring body (and that body's face, when one sits next to it), framed on the garment. #483:
	 * with KeepSlots only those material slots of the garment are drawn, framed on KeptBounds when it is valid.
	 */
	static FMantleThumbnailSubject GarmentSubject(USkeletalMesh* Garment, USkeletalMesh* Body, USkeletalMesh* Face,
		const TArray<int32>* KeepSlots = nullptr, const FBox& KeptBounds = FBox(ForceInit));

	/** #483: the material slots of a mesh with NumSlots slots that KeepSlots leaves out (none when it is null). Pure. */
	static TArray<int32> HiddenSlotsFor(int32 NumSlots, const TArray<int32>* KeepSlots);

	/** An accessory alone, three-quarter view from above; with bBackFaces its inside too (cloth, FMantleCosmeticDraft::bAccessoryBackFaces). */
	static FMantleThumbnailSubject AccessorySubject(UStaticMesh* Mesh, const FQuat& Worn = FQuat::Identity, bool bBackFaces = false);

	/**
	 * A transient copy of Mesh's first LOD facing the other way: each triangle's winding reversed and its normals turned
	 * round, so the copy draws exactly what a two-sided material shows from behind. Null when Mesh has no mesh description.
	 */
	static UStaticMesh* BackFaces(const UStaticMesh* Mesh);

	/** True when this process can render (a real RHI, the editor, rendering allowed); OutWhyNot says why not. */
	static bool CanCapture(FString* OutWhyNot = nullptr);

	/**
	 * Renders Subject into a Size x Size render target and reads it back (opaque, sRGB). False with a reason. First
	 * waits until every material it draws has its shaders (warm-up captures, shader and asset compiles finished, textures
	 * streamed in), so nothing draws as the grey world grid; the log names each mesh, its materials and their state.
	 */
	static bool Capture(const FMantleThumbnailSubject& Subject, int32 Size, TArray<FColor>& OutPixels, FString& OutError);

	/**
	 * Where a camera looking along View must stand so every corner of Frame lies inside a square
	 * perspective view of FOVDegrees with Margin to spare, centred on the box. Pure.
	 */
	static FVector FrameCamera(const FBox& Frame, const FRotator& View, float FOVDegrees, float Margin);
};
