// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleOverlayBake.h"
#include "PreviewScene.h"
#include "SEditorViewport.h"
#include "UObject/GCObject.h"
#include "UObject/ObjectPtr.h"

class FMantleOverlayPlaceViewportClient;
class USkeletalMesh;
class USkeletalMeshComponent;

/** The pure parts of the placement viewport, split out so they run without a window. */
namespace MantleOverlayPlaceMath
{
	/** The nearest point where a ray meets the triangles (mesh space), with the surface normal there. False when it misses. */
	MANTLEEDITOR_API bool RayOnTriangles(TConstArrayView<FMantleOverlayBake::FTriangle> Triangles, const FVector& Origin, const FVector& Direction,
		FVector& OutPoint, FVector& OutNormal);

	/** A projector at Point looking into the surface (against Normal), the picture upright (its up is world up, else world forward on a flat-up surface). */
	MANTLEEDITOR_API FQuat FaceSurface(const FVector& Normal);

	/**
	 * Where a picture starts on a skin: the front of the chest (or of the face) of Bounds, a ray in from the front at that height,
	 * moved by Offset first. The converted meshes face +Y; -Y is tried when that misses. False when the ray meets nothing.
	 */
	MANTLEEDITOR_API bool DefaultPlacement(TConstArrayView<FMantleOverlayBake::FTriangle> Triangles, const FBox& Bounds, const FVector& Offset, FTransform& OutTransform);

	/** A gizmo scale drag as the picture's new width: the width grows by the mean of the drag's axes (a uniform scale), never below MinCm. */
	MANTLEEDITOR_API float ApplyScaleDrag(float WidthCm, const FVector& ScaleDelta, float MinCm = 0.5f);
}

/**
 * #54: the overlay creator's placement viewport. A converted MetaHuman body or face in the reference pose and the projector the
 * picture is baked from, drawn as a box (the picture's outline, reaching the depth either side) with an arrow showing the way it
 * looks. Click the skin to put the projector there, facing in; drag the gizmo (W move, E turn, R size) to adjust. It edits the
 * placement it is given and tells the panel; the bake itself (FMantleOverlayBake) runs at Preview and Upload.
 */
class MANTLEEDITOR_API SMantleOverlayPlaceViewport : public SEditorViewport, public FGCObject
{
public:
	SLATE_BEGIN_ARGS(SMantleOverlayPlaceViewport) {}
		/** After a click or a drag ends. */
		SLATE_EVENT(FSimpleDelegate, OnPlacementChanged)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SMantleOverlayPlaceViewport() override;

	/** The skin to place on (unscaled, at the origin). Frames the view when the mesh changes. False when its triangles cannot be read. */
	bool SetMesh(USkeletalMesh* InMesh, FString& OutError);

	/** The placement, and the picture's height over its width (the outline's shape). */
	void SetPlacement(const FMantleOverlayPlacement& InPlacement, float InAspect);
	const FMantleOverlayPlacement& GetPlacement() const;

	/** Puts the projector on the body's front, middle of the chest or the face's front. */
	void ResetPlacement();
	void Frame();

	// FGCObject
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual FString GetReferencerName() const override { return TEXT("SMantleOverlayPlaceViewport"); }

protected:
	virtual TSharedRef<FEditorViewportClient> MakeEditorViewportClient() override;

private:
	TUniquePtr<FPreviewScene> Scene;
	TSharedPtr<FMantleOverlayPlaceViewportClient> Client;
	FSimpleDelegate OnPlacementChanged;
	TObjectPtr<USkeletalMeshComponent> MeshComponent;
	TObjectPtr<USkeletalMesh> Mesh;
};
