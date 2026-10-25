// Copyright Joshua Eger. All Rights Reserved.
#include "SMantleOverlayPlaceViewport.h"
#include "MantleGizmoStyle.h"

#include "Components/SkeletalMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/SkeletalMesh.h"
#include "EditorViewportClient.h"
#include "PrimitiveDrawingUtils.h"
#include "SceneView.h"
#include "UnrealWidget.h"
#include "UObject/Package.h"

// ---- the pure parts -------------------------------------------------------------------------------------------------

bool MantleOverlayPlaceMath::RayOnTriangles(TConstArrayView<FMantleOverlayBake::FTriangle> Triangles, const FVector& Origin, const FVector& Direction,
	FVector& OutPoint, FVector& OutNormal)
{
	double BestT = TNumericLimits<double>::Max();
	bool bHit = false;
	for (const FMantleOverlayBake::FTriangle& Triangle : Triangles)
	{
		// Moller-Trumbore, both faces: the skin is closed, the nearest hit is the outside.
		const FVector A(Triangle.Position[0]), B(Triangle.Position[1]), C(Triangle.Position[2]);
		const FVector Edge1 = B - A, Edge2 = C - A;
		const FVector P = FVector::CrossProduct(Direction, Edge2);
		const double Det = FVector::DotProduct(Edge1, P);
		if (FMath::Abs(Det) < 1e-12)
		{
			continue;
		}
		const double InvDet = 1.0 / Det;
		const FVector T = Origin - A;
		const double U = FVector::DotProduct(T, P) * InvDet;
		if (U < 0.0 || U > 1.0)
		{
			continue;
		}
		const FVector Q = FVector::CrossProduct(T, Edge1);
		const double V = FVector::DotProduct(Direction, Q) * InvDet;
		if (V < 0.0 || U + V > 1.0)
		{
			continue;
		}
		const double Distance = FVector::DotProduct(Edge2, Q) * InvDet;
		if (Distance > 0.0 && Distance < BestT)
		{
			BestT = Distance;
			bHit = true;
			OutPoint = Origin + Direction * Distance;
			const double W = 1.0 - U - V;
			OutNormal = (FVector(Triangle.Normal[0]) * W + FVector(Triangle.Normal[1]) * U + FVector(Triangle.Normal[2]) * V).GetSafeNormal();
		}
	}
	return bHit;
}

FQuat MantleOverlayPlaceMath::FaceSurface(const FVector& Normal)
{
	const FVector Look = -Normal.GetSafeNormal();
	// The picture's up is the world's, unless the surface faces straight up or down.
	const FVector Up = FMath::Abs(Look.Z) > 0.97 ? FVector::ForwardVector : FVector::UpVector;
	return FRotationMatrix::MakeFromXZ(Look, Up).ToQuat();
}

bool MantleOverlayPlaceMath::DefaultPlacement(TConstArrayView<FMantleOverlayBake::FTriangle> Triangles, const FBox& Bounds, const FVector& Offset, FTransform& OutTransform)
{
	if (!Bounds.IsValid)
	{
		return false;
	}
	FVector Centre = Bounds.GetCenter();
	if (Bounds.GetSize().Z > 100.0)
	{
		Centre.Z = Bounds.Min.Z + Bounds.GetSize().Z * 0.72;
	}
	Centre += Offset;
	for (const double Side : { 1.0, -1.0 })
	{
		FVector Point, Normal;
		if (RayOnTriangles(Triangles, Centre + FVector(0.0, Side * 500.0, 0.0), FVector(0.0, -Side, 0.0), Point, Normal))
		{
			OutTransform = FTransform(FaceSurface(Normal), Point);
			return true;
		}
	}
	return false;
}

float MantleOverlayPlaceMath::ApplyScaleDrag(const float WidthCm, const FVector& ScaleDelta, const float MinCm)
{
	const double Mean = (ScaleDelta.X + ScaleDelta.Y + ScaleDelta.Z) / 3.0;
	return FMath::Max(MinCm, static_cast<float>(WidthCm * (1.0 + Mean)));
}

// ---- the client -----------------------------------------------------------------------------------------------------

/** The stage: the engine's default lights; the viewport brings its own camera. */
class FMantleOverlayPlaceStage : public FPreviewScene
{
public:
	FMantleOverlayPlaceStage() : FPreviewScene(FPreviewScene::ConstructionValues()) {}
};

class FMantleOverlayPlaceViewportClient : public FEditorViewportClient
{
public:
	FMantleOverlayPlaceViewportClient(FPreviewScene& InScene, const TSharedRef<SEditorViewport>& InWidget)
		: FEditorViewportClient(nullptr, &InScene, InWidget)
	{
		SetRealtime(true);
		ShowWidget(true);
		ViewFOV = 50.f;
		SetViewLocation(FVector(0.0, 250.0, 120.0));
		SetViewRotation(FRotator(-5.0, -90.0, 0.0));
	}

	FMantleOverlayPlacement Placement;
	float Aspect = 1.f;
	TArray<FMantleOverlayBake::FTriangle> Triangles;
	FBox Bounds = FBox(ForceInit);
	FSimpleDelegate OnChanged;

	void Frame()
	{
		const FBox Box = Bounds.IsValid ? Bounds : FBox(FVector(-50.0), FVector(50.0));
		FocusViewportOnBox(Box, true);
	}

	// FEditorViewportClient
	virtual void Tick(float DeltaSeconds) override
	{
		FEditorViewportClient::Tick(DeltaSeconds);
		if (UWorld* ViewWorld = GetWorld())
		{
			ViewWorld->Tick(LEVELTICK_All, DeltaSeconds);
		}
	}

	virtual void Draw(const FSceneView* View, FPrimitiveDrawInterface* PDI) override
	{
		FEditorViewportClient::Draw(View, PDI);
		const FQuat Rotation = Placement.Transform.GetRotation();
		const FVector Origin = Placement.Transform.GetLocation();
		const FVector Look = Rotation.GetForwardVector(), Right = Rotation.GetRightVector(), Up = Rotation.GetUpVector();
		const double HalfWidth = Placement.WidthCm * 0.5, HalfHeight = HalfWidth * Aspect;
		// The reach: a box the picture's size, Depth either side.
		DrawOrientedWireBox(PDI, Origin, Look, Right, Up, FVector(Placement.DepthCm, HalfWidth, HalfHeight), MantleGizmoStyle::Reach, SDPG_Foreground);
		// The picture's own outline, where it meets the surface plane.
		const FVector Corners[4] = {
			Origin - Right * HalfWidth + Up * HalfHeight, Origin + Right * HalfWidth + Up * HalfHeight,
			Origin + Right * HalfWidth - Up * HalfHeight, Origin - Right * HalfWidth - Up * HalfHeight };
		for (int32 Corner = 0; Corner < 4; ++Corner)
		{
			PDI->DrawLine(Corners[Corner], Corners[(Corner + 1) % 4], MantleGizmoStyle::Outline, SDPG_Foreground, 0.f);
		}
		// Which way it looks, and which way is up in the picture.
		DrawDirectionalArrow(PDI, FRotationMatrix::MakeFromX(Look) * FTranslationMatrix(Origin - Look * Placement.DepthCm),
			MantleGizmoStyle::Facing, Placement.DepthCm * 2.f, 1.f, SDPG_Foreground);
		PDI->DrawLine(Origin, Origin + Up * HalfHeight * 1.3, MantleGizmoStyle::Up, SDPG_Foreground, 0.f);
	}

	virtual void ProcessClick(FSceneView& View, HHitProxy* HitProxy, FKey Key, EInputEvent Event, uint32 HitX, uint32 HitY) override
	{
		// A left click (not a drag, which moves the camera) on the skin puts the projector there.
		if (Key == EKeys::LeftMouseButton && Event == IE_Released && !(HitProxy && HitProxy->IsA(HWidgetAxis::StaticGetType())) && Viewport)
		{
			const FViewportCursorLocation Cursor(&View, this, HitX, HitY);
			FVector Point, Normal;
			if (MantleOverlayPlaceMath::RayOnTriangles(Triangles, Cursor.GetOrigin(), Cursor.GetDirection(), Point, Normal))
			{
				Placement.Transform = FTransform(MantleOverlayPlaceMath::FaceSurface(Normal), Point);
				Invalidate();
				OnChanged.ExecuteIfBound();
				return;
			}
		}
		FEditorViewportClient::ProcessClick(View, HitProxy, Key, Event, HitX, HitY);
	}

	virtual bool InputKey(const FInputKeyEventArgs& EventArgs) override
	{
		const bool bFlying = EventArgs.Viewport && EventArgs.Viewport->KeyState(EKeys::RightMouseButton);
		if (EventArgs.Event == IE_Pressed && !bFlying)
		{
			if (EventArgs.Key == EKeys::W) { SetWidgetMode(UE::Widget::WM_Translate); return true; }
			if (EventArgs.Key == EKeys::E) { SetWidgetMode(UE::Widget::WM_Rotate); return true; }
			if (EventArgs.Key == EKeys::R) { SetWidgetMode(UE::Widget::WM_Scale); return true; }
			if (EventArgs.Key == EKeys::F) { Frame(); return true; }
		}
		return FEditorViewportClient::InputKey(EventArgs);
	}

	virtual bool CanSetWidgetMode(UE::Widget::EWidgetMode NewMode) const override
	{
		return NewMode == UE::Widget::WM_Translate || NewMode == UE::Widget::WM_Rotate || NewMode == UE::Widget::WM_Scale;
	}
	virtual bool CanCycleWidgetMode() const override { return true; }
	virtual UE::Widget::EWidgetMode GetWidgetMode() const override { return Mode; }
	virtual void SetWidgetMode(UE::Widget::EWidgetMode NewMode) override
	{
		if (CanSetWidgetMode(NewMode))
		{
			Mode = NewMode;
			Invalidate();
		}
	}
	virtual FVector GetWidgetLocation() const override { return Placement.Transform.GetLocation(); }
	virtual FMatrix GetWidgetCoordSystem() const override { return FMatrix::Identity; }
	virtual ECoordSystem GetWidgetCoordSystemSpace() const override { return COORD_World; }

	virtual void TrackingStarted(const FInputEventState& InInputState, bool bIsDraggingWidget, bool bNudge) override
	{
		bDragged = false;
		FEditorViewportClient::TrackingStarted(InInputState, bIsDraggingWidget, bNudge);
	}

	virtual bool InputWidgetDelta(FViewport* InViewport, EAxisList::Type CurrentAxis, FVector& Drag, FRotator& Rot, FVector& Scale) override
	{
		if (CurrentAxis == EAxisList::None)
		{
			return false;
		}
		switch (Mode)
		{
		case UE::Widget::WM_Rotate:
			Placement.Transform.SetRotation((Rot.Quaternion() * Placement.Transform.GetRotation()).GetNormalized());
			break;
		case UE::Widget::WM_Scale:
			Placement.WidthCm = MantleOverlayPlaceMath::ApplyScaleDrag(Placement.WidthCm, Scale);
			break;
		default:
			Placement.Transform.AddToTranslation(Drag);
			break;
		}
		bDragged = true;
		Invalidate();
		return true;
	}

	virtual void TrackingStopped() override
	{
		FEditorViewportClient::TrackingStopped();
		if (bDragged)
		{
			bDragged = false;
			OnChanged.ExecuteIfBound();
		}
	}

private:
	UE::Widget::EWidgetMode Mode = UE::Widget::WM_Translate;
	bool bDragged = false;
};

// ---- the widget -----------------------------------------------------------------------------------------------------

void SMantleOverlayPlaceViewport::Construct(const FArguments& InArgs)
{
	OnPlacementChanged = InArgs._OnPlacementChanged;
	Scene = MakeUnique<FMantleOverlayPlaceStage>();

	MeshComponent = NewObject<USkeletalMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient);
	MeshComponent->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	MeshComponent->SetDisablePostProcessBlueprint(true);
	// LOD0 is what the bake reads; show it.
	MeshComponent->SetForcedLOD(1);
	Scene->AddComponent(MeshComponent, FTransform::Identity);

	SEditorViewport::Construct(SEditorViewport::FArguments());
	Client->OnChanged = OnPlacementChanged;
}

SMantleOverlayPlaceViewport::~SMantleOverlayPlaceViewport()
{
	if (Client.IsValid())
	{
		Client->Viewport = nullptr;
	}
}

TSharedRef<FEditorViewportClient> SMantleOverlayPlaceViewport::MakeEditorViewportClient()
{
	Client = MakeShared<FMantleOverlayPlaceViewportClient>(*Scene, SharedThis(this));
	return Client.ToSharedRef();
}

void SMantleOverlayPlaceViewport::AddReferencedObjects(FReferenceCollector& Collector)
{
	Collector.AddReferencedObject(MeshComponent);
	Collector.AddReferencedObject(Mesh);
}

bool SMantleOverlayPlaceViewport::SetMesh(USkeletalMesh* InMesh, FString& OutError)
{
	if (InMesh == Mesh && !Client->Triangles.IsEmpty())
	{
		return true;
	}
	Client->Triangles.Reset();
	Mesh = InMesh;
	MeshComponent->SetSkeletalMesh(InMesh);
	if (!InMesh)
	{
		OutError = TEXT("There is no converted MetaHuman body or face to place the picture on.");
		return false;
	}
	if (!FMantleOverlayBake::ReadMesh(*InMesh, Client->Triangles, OutError))
	{
		return false;
	}
	Client->Bounds = InMesh->GetBounds().GetBox();
	Client->Frame();
	return true;
}

void SMantleOverlayPlaceViewport::SetPlacement(const FMantleOverlayPlacement& InPlacement, const float InAspect)
{
	Client->Placement = InPlacement;
	Client->Aspect = InAspect > 0.f ? InAspect : 1.f;
	Client->Invalidate();
}

const FMantleOverlayPlacement& SMantleOverlayPlaceViewport::GetPlacement() const
{
	return Client->Placement;
}

void SMantleOverlayPlaceViewport::ResetPlacement()
{
	// The front of the chest (or of the face).
	FTransform Result;
	if (MantleOverlayPlaceMath::DefaultPlacement(Client->Triangles, Client->Bounds, FVector::ZeroVector, Result))
	{
		Client->Placement.Transform = Result;
		Client->Invalidate();
		OnPlacementChanged.ExecuteIfBound();
	}
}

void SMantleOverlayPlaceViewport::Frame()
{
	Client->Frame();
}
