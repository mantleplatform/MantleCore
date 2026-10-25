// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "EditorViewportClient.h"
#include "HitProxies.h"
#include "MantleBuilderKind.h"
#include "MantleBuilderPoint.h"

/** The hit proxy of one point's handle: clicking it selects the role. */
struct HMantleBuilderPointProxy : public HHitProxy
{
	DECLARE_HIT_PROXY();

	explicit HMantleBuilderPointProxy(FName InRole)
		: HHitProxy(HPP_Foreground)
		, Role(InRole)
	{
	}

	virtual EMouseCursor::Type GetMouseCursor() override { return EMouseCursor::Crosshairs; }

	FName Role;
};

/** Draws the handles, the ghost and the warnings, and turns the gizmo's drags into point edits (see SMantleBuilderViewport). */
class FMantleBuilderViewportClient : public FEditorViewportClient
{
public:
	FMantleBuilderViewportClient(FPreviewScene& InScene, const TSharedRef<SEditorViewport>& InWidget);

	void SetKind(const TSharedPtr<IMantleBuilderKind>& InKind);
	/** The components showing the model: turned with the picture (IMantleBuilderKind::ViewTurn). */
	void SetModelComponents(class USceneComponent* InStatic, class USceneComponent* InSkeletal);
	void SetMesh(UObject* InMesh, bool bFrame = true);
	void SetPoints(const TArray<FMantleBuilderPoint>& InPoints);
	const TArray<FMantleBuilderPoint>& GetPoints() const { return Points; }
	void SetSelectedRole(FName Role);
	FName GetSelectedRole() const { return SelectedRole; }

	void ResetSelectedToGuess();
	void ResetAllToGuess();
	void Focus();

	/** Whether the kind's extra boxes, lines and markers are drawn (on by default). */
	void SetShowOverlay(bool bShow) { bShowOverlay = bShow; Invalidate(); }

	/** Asks the kind again for the ghost, the warnings and the overlay, e.g. after a kind setting changed. */
	void RefreshFromKind() { Refresh(); }

	FSimpleDelegate OnPointsChanged;

	// FEditorViewportClient
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void Draw(const FSceneView* View, FPrimitiveDrawInterface* PDI) override;
	virtual void DrawCanvas(FViewport& InViewport, FSceneView& View, FCanvas& Canvas) override;
	virtual void ProcessClick(FSceneView& View, HHitProxy* HitProxy, FKey Key, EInputEvent Event, uint32 HitX, uint32 HitY) override;
	virtual bool InputKey(const FInputKeyEventArgs& EventArgs) override;
	virtual bool InputWidgetDelta(FViewport* InViewport, EAxisList::Type CurrentAxis, FVector& Drag, FRotator& Rot, FVector& Scale) override;
	virtual void TrackingStarted(const FInputEventState& InInputState, bool bIsDraggingWidget, bool bNudge) override;
	virtual void TrackingStopped() override;
	virtual bool CanSetWidgetMode(UE::Widget::EWidgetMode NewMode) const override;
	virtual bool CanCycleWidgetMode() const override { return false; }
	virtual UE::Widget::EWidgetMode GetWidgetMode() const override;
	virtual void SetWidgetMode(UE::Widget::EWidgetMode NewMode) override;
	virtual FVector GetWidgetLocation() const override;
	virtual FMatrix GetWidgetCoordSystem() const override;
	virtual ECoordSystem GetWidgetCoordSystemSpace() const override { return COORD_World; }
	virtual void MouseMove(FViewport* InViewport, int32 X, int32 Y) override;
	virtual void CapturedMouseMove(FViewport* InViewport, int32 X, int32 Y) override;

private:
	/** Press on a point and drag: it follows the mouse across the screen (on the plane facing the camera), and snaps when let go.
	 *  No gizmo to learn; the arrows still work for anyone who knows them. */
	bool BeginPointDrag(FViewport* InViewport, int32 X, int32 Y);
	void UpdatePointDrag(FViewport* InViewport, int32 X, int32 Y);
	void EndPointDrag();
	/** This frame's view of the viewport, for projecting the mouse. */
	FSceneView* CalcViewForMouse(FSceneViewFamilyContext& Family, FViewport* InViewport);

	FMantleBuilderPoint* FindPoint(FName Role);
	const FMantleBuilderPoint* FindPoint(FName Role) const;
	const FMantleBuilderPointRole* FindRole(FName Role) const;

	/** The point's parent in mesh space; an unknown parent counts as the root and is logged once. */
	FTransform ParentToMesh(const FMantleBuilderPoint& Point) const;
	FTransform PointToMesh(const FMantleBuilderPoint& Point) const;
	/** Mesh space to the view (the picture turned so the holder stands upright), and back. */
	FTransform PointToView(const FMantleBuilderPoint& Point) const { return PointToMesh(Point) * FTransform(ViewTurn); }
	FVector ToView(const FVector& InMesh) const { return ViewTurn.RotateVector(InMesh); }

	/** Rebuilds the ghost, the warnings and the gizmo's visibility after any change. */
	void Refresh();

	TSharedPtr<IMantleBuilderKind> Kind;
	TArray<FMantleBuilderPointRole> Roles;
	TWeakObjectPtr<UObject> Mesh;
	TArray<FMantleBuilderPoint> Points;
	TArray<FMantleGhostCapsule> Ghost;
	FQuat ViewTurn = FQuat::Identity;
	TWeakObjectPtr<class USceneComponent> ModelComponents[2];
	/** The ghost as a solid, lit body in the scene: each capsule an engine cylinder with a sphere at each end, grey or red.
	 *  Rebuilt from Ghost on every Refresh; false (the wire outline is drawn instead) when the engine shapes are missing. */
	bool UpdateSolidGhost();
	TArray<TObjectPtr<class UStaticMeshComponent>> GhostParts;
	TObjectPtr<class UMaterialInstanceDynamic> BodyMaterial;
	TObjectPtr<class UMaterialInstanceDynamic> WarningMaterial;
	bool bSolidGhost = false;
	FMantleBuilderOverlay Overlay;
	bool bShowOverlay = true;
	/** The kind's own line of help for the bottom of the picture; empty draws the default (how to drag a point). */
	FText Hint;
	TArray<FText> Warnings;
	mutable TSet<FName> LoggedUnknownParents;
	FName SelectedRole;
	UE::Widget::EWidgetMode Mode = UE::Widget::WM_Translate;
	bool bDragged = false;
	bool bPointDragging = false;
	/** The point minus where the mouse ray met its drag plane when the drag began, so the point does not jump to the cursor. */
	FVector PointDragOffset = FVector::ZeroVector;
	FVector PointDragPlaneNormal = FVector::ForwardVector;
	FVector PointDragPlaneOrigin = FVector::ZeroVector;
};
