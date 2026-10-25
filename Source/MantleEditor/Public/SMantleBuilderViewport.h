// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleBuilderKind.h"
#include "MantleBuilderPoint.h"
#include "PreviewScene.h"
#include "SEditorViewport.h"
#include "UObject/GCObject.h"
#include "UObject/ObjectPtr.h"

class FMantleBuilderViewportClient;
class USkeletalMeshComponent;
class UStaticMeshComponent;

/** The pure parts of the point-placement viewport, split out so they run without a window. */
namespace MantleBuilderViewportMath
{
	/**
	 * A point's Parent as a transform in the mesh's own unscaled space: NAME_None is the mesh root (identity); for a skeletal mesh
	 * a bone (reference pose, component space) or a socket; for a static mesh a socket. False for an unknown parent (OutParentToMesh
	 * is then identity, i.e. the caller treats it as the root).
	 */
	MANTLEEDITOR_API bool ResolveParent(const UObject* Mesh, FName Parent, FTransform& OutParentToMesh);

	/**
	 * A drag of the gizmo applied to a point: Current is in the parent's space, ParentToMesh its parent's transform, and the deltas are
	 * the world-space (mesh-space) translation and rotation the gizmo reports. The rotation is ignored when the role does not rotate,
	 * and a yaw-only role keeps only the turn about up. Returns the new transform in the parent's space; the caller sets Source = Creator.
	 */
	MANTLEEDITOR_API FTransform ApplyDrag(const FMantleBuilderPointRole& Role, const FTransform& ParentToMesh, const FTransform& Current, const FVector& WorldDeltaLocation, const FRotator& WorldDeltaRotation);

	/** One point's handle as it appears on screen (viewport pixels). */
	struct FScreenHandle
	{
		FName Role;
		FVector2D Screen = FVector2D::ZeroVector;
	};

	/** How close (pixels) a click must be to a handle's centre to pick it; a handle is only a few pixels across. */
	constexpr double PickTolerancePx = 20.0;

	/** The role of the handle nearest the click within TolerancePx, or NAME_None when none is that close. */
	MANTLEEDITOR_API FName PickHandle(const TArray<FScreenHandle>& Handles, const FVector2D& Click, double TolerancePx = PickTolerancePx);

	/** Where a mouse ray meets the plane through PlaneOrigin facing PlaneNormal (the camera), or false when the ray runs along
	 *  the plane or away from it. Dragging a point moves it on that plane, so it follows the cursor on screen. */
	MANTLEEDITOR_API bool RayOnPlane(const FVector& RayOrigin, const FVector& RayDirection, const FVector& PlaneOrigin, const FVector& PlaneNormal, FVector& OutHit);
}

/**
 * The builders' shared point-placement viewport: the creator's mesh at its own unscaled size, a coloured handle for every
 * point the kind defines, and the Mantle character as a ghost so a creator sees at once whether a hand reaches its grip or a
 * seat fits. Click a handle, drag the gizmo; problems turn the ghost red and are listed in red on the picture. It draws and
 * edits the points it is given; the kind decides what the roles mean (weapon builder #729, vehicle builder #728).
 */
class MANTLEEDITOR_API SMantleBuilderViewport : public SEditorViewport, public FGCObject
{
public:
	SLATE_BEGIN_ARGS(SMantleBuilderViewport) {}
		SLATE_ARGUMENT(TSharedPtr<IMantleBuilderKind>, Kind)
		/** After a drag ends (and the kind has snapped the point). */
		SLATE_EVENT(FSimpleDelegate, OnPointsChanged)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SMantleBuilderViewport() override;

	/**
	 * A UStaticMesh or USkeletalMesh, shown unscaled at the origin; anything else shows nothing. Frames the view, unless bFrame is false
	 * (a mesh swapped while a creator is dragging a slider keeps the creator's view).
	 */
	void SetMesh(UObject* Mesh, bool bFrame = true);

	void SetPoints(const TArray<FMantleBuilderPoint>& Points);
	const TArray<FMantleBuilderPoint>& GetPoints() const;

	void SetSelectedRole(FName Role);
	FName GetSelectedRole() const;

	void ResetSelectedToGuess();
	void ResetAllToGuess();

	/** Whether the kind's extra boxes, lines and markers are drawn; on by default. */
	void SetShowOverlay(bool bShow);

	/** Asks the kind again for the ghost, warnings and overlay, after a setting on the kind changed. */
	void RefreshFromKind();

	// FGCObject
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual FString GetReferencerName() const override { return TEXT("SMantleBuilderViewport"); }

protected:
	virtual TSharedRef<FEditorViewportClient> MakeEditorViewportClient() override;

private:
	TUniquePtr<FPreviewScene> Scene;
	TSharedPtr<FMantleBuilderViewportClient> Client;
	TSharedPtr<IMantleBuilderKind> Kind;
	FSimpleDelegate OnPointsChanged;

	TObjectPtr<UStaticMeshComponent> StaticComponent;
	TObjectPtr<USkeletalMeshComponent> SkeletalComponent;
	TObjectPtr<UObject> Mesh;
};
