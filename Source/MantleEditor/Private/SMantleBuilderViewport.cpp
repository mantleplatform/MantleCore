// Copyright Joshua Eger. All Rights Reserved.
#include "SMantleBuilderViewport.h"
#include "MantleBuilderViewportClient.h"

#include "CanvasItem.h"
#include "CanvasTypes.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSocket.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "PrimitiveDrawingUtils.h"
#include "ReferenceSkeleton.h"
#include "SceneView.h"
#include "UnrealWidget.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogMantleBuilderViewport, Log, All);

IMPLEMENT_HIT_PROXY(HMantleBuilderPointProxy, HHitProxy)

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleBuilderViewportPrivate
{
	const FLinearColor GhostColor(0.55f, 0.55f, 0.6f);
	const FLinearColor WarningColor(1.f, 0.1f, 0.1f);

	FLinearColor ShapeColor(EMantleBuilderPointShape Shape)
	{
		switch (Shape)
		{
		case EMantleBuilderPointShape::Hand: return FLinearColor(0.1f, 0.35f, 1.f);
		case EMantleBuilderPointShape::Foot: return FLinearColor(0.1f, 0.85f, 0.2f);
		case EMantleBuilderPointShape::Seat: return FLinearColor(1.f, 0.5f, 0.05f);
		case EMantleBuilderPointShape::Arrow: return FLinearColor(1.f, 0.9f, 0.1f);
		default: return FLinearColor::White;
		}
	}

	FTransform BoneComponentSpace(const FReferenceSkeleton& Ref, int32 BoneIndex)
	{
		const TArray<FTransform>& Pose = Ref.GetRefBonePose();
		FTransform Result = Pose[BoneIndex];
		for (int32 Parent = Ref.GetParentIndex(BoneIndex); Parent != INDEX_NONE; Parent = Ref.GetParentIndex(Parent))
		{
			Result = Result * Pose[Parent];
		}
		return Result;
	}

	/** The preview stage: the engine's default lights; the viewport brings its own camera. */
	class FStage : public FPreviewScene
	{
	public:
		FStage()
			: FPreviewScene(FPreviewScene::ConstructionValues())
		{
		}
	};

	FBox MeshBox(const UObject* Mesh)
	{
		if (const UStaticMesh* Static = Cast<UStaticMesh>(Mesh))
		{
			return Static->GetBounds().GetBox();
		}
		if (const USkeletalMesh* Skeletal = Cast<USkeletalMesh>(Mesh))
		{
			return Skeletal->GetBounds().GetBox();
		}
		return FBox(ForceInit);
	}
}

// ---- pure helpers ---------------------------------------------------------------------------------------------------

bool MantleBuilderViewportMath::ResolveParent(const UObject* Mesh, FName Parent, FTransform& OutParentToMesh)
{
	OutParentToMesh = FTransform::Identity;
	if (Parent.IsNone())
	{
		return true;
	}
	if (const USkeletalMesh* Skeletal = Cast<USkeletalMesh>(Mesh))
	{
		const FReferenceSkeleton& Ref = Skeletal->GetRefSkeleton();
		const int32 Bone = Ref.FindBoneIndex(Parent);
		if (Bone != INDEX_NONE)
		{
			OutParentToMesh = MantleBuilderViewportPrivate::BoneComponentSpace(Ref, Bone);
			return true;
		}
		if (const USkeletalMeshSocket* Socket = Skeletal->FindSocket(Parent))
		{
			const int32 SocketBone = Ref.FindBoneIndex(Socket->BoneName);
			const FTransform BoneToMesh = SocketBone != INDEX_NONE ? MantleBuilderViewportPrivate::BoneComponentSpace(Ref, SocketBone) : FTransform::Identity;
			OutParentToMesh = Socket->GetSocketLocalTransform() * BoneToMesh;
			return true;
		}
	}
	else if (const UStaticMesh* Static = Cast<UStaticMesh>(Mesh))
	{
		if (const UStaticMeshSocket* Socket = Static->FindSocket(Parent))
		{
			OutParentToMesh = FTransform(Socket->RelativeRotation, Socket->RelativeLocation, Socket->RelativeScale);
			return true;
		}
	}
	return false;
}

FTransform MantleBuilderViewportMath::ApplyDrag(const FMantleBuilderPointRole& Role, const FTransform& ParentToMesh, const FTransform& Current, const FVector& WorldDeltaLocation, const FRotator& WorldDeltaRotation)
{
	// The parent's own scale does not scale the placement: points live in the mesh's unscaled units.
	FTransform Parent = ParentToMesh;
	Parent.SetScale3D(FVector::OneVector);

	const FTransform World = Current * Parent;
	const FVector Location = World.GetLocation() + WorldDeltaLocation;
	FQuat Rotation = World.GetRotation();
	if (Role.bRotates && !WorldDeltaRotation.IsNearlyZero())
	{
		Rotation = WorldDeltaRotation.Quaternion() * Rotation;
		Rotation.Normalize();
		if (Role.bYawOnly)
		{
			FRotator Yaw = Rotation.Rotator();
			Yaw.Pitch = 0.0;
			Yaw.Roll = 0.0;
			Rotation = Yaw.Quaternion();
		}
	}

	FTransform Result = FTransform(Rotation, Location, FVector::OneVector).GetRelativeTransform(Parent);
	Result.SetScale3D(Current.GetScale3D());
	return Result;
}

FName MantleBuilderViewportMath::PickHandle(const TArray<FScreenHandle>& Handles, const FVector2D& Click, double TolerancePx)
{
	FName Best = NAME_None;
	double BestDistance = TolerancePx;
	for (const FScreenHandle& Handle : Handles)
	{
		const double Distance = FVector2D::Distance(Handle.Screen, Click);
		if (Distance <= BestDistance)
		{
			BestDistance = Distance;
			Best = Handle.Role;
		}
	}
	return Best;
}

bool MantleBuilderViewportMath::RayOnPlane(const FVector& RayOrigin, const FVector& RayDirection, const FVector& PlaneOrigin, const FVector& PlaneNormal, FVector& OutHit)
{
	const double Facing = FVector::DotProduct(RayDirection, PlaneNormal);
	if (FMath::Abs(Facing) < KINDA_SMALL_NUMBER)
	{
		return false;
	}
	const double Distance = FVector::DotProduct(PlaneOrigin - RayOrigin, PlaneNormal) / Facing;
	if (Distance < 0.0)
	{
		return false;
	}
	OutHit = RayOrigin + RayDirection * Distance;
	return true;
}

// ---- the viewport client -------------------------------------------------------------------------------------------

FMantleBuilderViewportClient::FMantleBuilderViewportClient(FPreviewScene& InScene, const TSharedRef<SEditorViewport>& InWidget)
	: FEditorViewportClient(nullptr, &InScene, InWidget)
{
	SetRealtime(true);
	ShowWidget(false);
	ViewFOV = 60.f;
	EngineShowFlags.SetGrid(true);
	SetViewLocation(FVector(150.0, 150.0, 100.0));
	SetViewRotation(FRotator(-20.0, -135.0, 0.0));
}

void FMantleBuilderViewportClient::AddReferencedObjects(FReferenceCollector& Collector)
{
	FEditorViewportClient::AddReferencedObjects(Collector);
	Collector.AddReferencedObjects(GhostParts);
	Collector.AddReferencedObject(BodyMaterial);
	Collector.AddReferencedObject(WarningMaterial);
}

bool FMantleBuilderViewportClient::UpdateSolidGhost()
{
	using namespace MantleBuilderViewportPrivate;
	UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"), nullptr, LOAD_NoWarn | LOAD_Quiet);
	UStaticMesh* Cylinder = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"), nullptr, LOAD_NoWarn | LOAD_Quiet);
	UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"), nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!PreviewScene || !Sphere || !Cylinder || !Base)
	{
		return false;
	}
	if (!BodyMaterial)
	{
		BodyMaterial = UMaterialInstanceDynamic::Create(Base, GetTransientPackage());
		BodyMaterial->SetVectorParameterValue(TEXT("Color"), GhostColor);
		WarningMaterial = UMaterialInstanceDynamic::Create(Base, GetTransientPackage());
		WarningMaterial->SetVectorParameterValue(TEXT("Color"), WarningColor);
	}
	int32 Used = 0;
	auto Place = [&](UStaticMesh* Shape, const FTransform& Transform, bool bWarning)
	{
		if (!GhostParts.IsValidIndex(Used))
		{
			UStaticMeshComponent* Part = NewObject<UStaticMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient);
			Part->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
			Part->SetGenerateOverlapEvents(false);
			Part->SetCanEverAffectNavigation(false);
			Part->SetCastShadow(false);
			Part->bSelectable = false;
			PreviewScene->AddComponent(Part, FTransform::Identity);
			GhostParts.Add(Part);
		}
		UStaticMeshComponent* Part = GhostParts[Used++];
		Part->SetStaticMesh(Shape);
		Part->SetMaterial(0, bWarning ? WarningMaterial.Get() : BodyMaterial.Get());
		Part->SetWorldTransform(Transform);
		Part->SetVisibility(true);
	};
	// The engine's sphere and cylinder are 100 cm across; the cylinder is 100 cm tall about its middle, along Z.
	for (const FMantleGhostCapsule& Capsule : Ghost)
	{
		const FVector Start = ToView(Capsule.Start);
		const FVector End = ToView(Capsule.End);
		const double Across = FMath::Max(Capsule.RadiusCm, 0.01) / 50.0;
		const FVector Axis = End - Start;
		const double Length = Axis.Size();
		Place(Sphere, FTransform(FQuat::Identity, Start, FVector(Across)), Capsule.bWarning);
		if (Length >= KINDA_SMALL_NUMBER)
		{
			Place(Sphere, FTransform(FQuat::Identity, End, FVector(Across)), Capsule.bWarning);
			Place(Cylinder, FTransform(FRotationMatrix::MakeFromZ(Axis / Length).ToQuat(), (Start + End) * 0.5, FVector(Across, Across, Length / 100.0)), Capsule.bWarning);
		}
	}
	for (int32 Index = Used; Index < GhostParts.Num(); ++Index)
	{
		GhostParts[Index]->SetVisibility(false);
	}
	return true;
}

void FMantleBuilderViewportClient::SetModelComponents(USceneComponent* InStatic, USceneComponent* InSkeletal)
{
	ModelComponents[0] = InStatic;
	ModelComponents[1] = InSkeletal;
	Refresh();
}

void FMantleBuilderViewportClient::SetKind(const TSharedPtr<IMantleBuilderKind>& InKind)
{
	Kind = InKind;
	Roles = Kind.IsValid() ? Kind->GetRoles() : TArray<FMantleBuilderPointRole>();
	Refresh();
}

void FMantleBuilderViewportClient::SetMesh(UObject* InMesh, const bool bFrame)
{
	Mesh = InMesh;
	LoggedUnknownParents.Reset();
	Refresh();
	if (bFrame)
	{
		Focus();
	}
}

void FMantleBuilderViewportClient::SetPoints(const TArray<FMantleBuilderPoint>& InPoints)
{
	Points = InPoints;
	Refresh();
}

void FMantleBuilderViewportClient::SetSelectedRole(FName Role)
{
	SelectedRole = FindPoint(Role) ? Role : NAME_None;
	Refresh();
}

FMantleBuilderPoint* FMantleBuilderViewportClient::FindPoint(FName Role)
{
	return Points.FindByPredicate([Role](const FMantleBuilderPoint& P) { return P.Role == Role; });
}

const FMantleBuilderPoint* FMantleBuilderViewportClient::FindPoint(FName Role) const
{
	return Points.FindByPredicate([Role](const FMantleBuilderPoint& P) { return P.Role == Role; });
}

const FMantleBuilderPointRole* FMantleBuilderViewportClient::FindRole(FName Role) const
{
	return Roles.FindByPredicate([Role](const FMantleBuilderPointRole& R) { return R.Role == Role; });
}

FTransform FMantleBuilderViewportClient::ParentToMesh(const FMantleBuilderPoint& Point) const
{
	FTransform Result;
	if (!MantleBuilderViewportMath::ResolveParent(Mesh.Get(), Point.Parent, Result) && !LoggedUnknownParents.Contains(Point.Parent))
	{
		LoggedUnknownParents.Add(Point.Parent);
		UE_LOG(LogMantleBuilderViewport, Warning, TEXT("Builder point %s has parent '%s', which the mesh does not have; showing it at the mesh root."), *Point.Role.ToString(), *Point.Parent.ToString());
	}
	return Result;
}

FTransform FMantleBuilderViewportClient::PointToMesh(const FMantleBuilderPoint& Point) const
{
	FTransform Parent = ParentToMesh(Point);
	Parent.SetScale3D(FVector::OneVector);
	return Point.Transform * Parent;
}

void FMantleBuilderViewportClient::Refresh()
{
	if (Kind.IsValid())
	{
		Ghost = Kind->GhostPose(Points);
		ViewTurn = Kind->ViewTurn();
		Warnings = Kind->Check(Points);
		Overlay = Kind->Overlay(Points);
		Hint = Kind->Hint();
	}
	else
	{
		Ghost.Reset();
		ViewTurn = FQuat::Identity;
		Warnings.Reset();
		Overlay = FMantleBuilderOverlay();
		Hint = FText::GetEmpty();
	}

	const FMantleBuilderPointRole* Role = FindRole(SelectedRole);
	if (FindPoint(SelectedRole) == nullptr || Role == nullptr)
	{
		SelectedRole = NAME_None;
		Role = nullptr;
	}
	if (Role && !Role->bRotates)
	{
		Mode = UE::Widget::WM_Translate;
	}
	ShowWidget(Role != nullptr);
	for (const TWeakObjectPtr<USceneComponent>& Model : ModelComponents)
	{
		if (Model.IsValid())
		{
			Model->SetWorldRotation(ViewTurn);
		}
	}
	bSolidGhost = UpdateSolidGhost();
	Invalidate();
}

void FMantleBuilderViewportClient::ResetSelectedToGuess()
{
	if (FMantleBuilderPoint* Point = FindPoint(SelectedRole))
	{
		Point->ResetToGuess();
		Refresh();
	}
}

void FMantleBuilderViewportClient::ResetAllToGuess()
{
	MantleBuilderPoints::ResetAll(Points);
	Refresh();
}

void FMantleBuilderViewportClient::Focus()
{
	FBox Box = MantleBuilderViewportPrivate::MeshBox(Mesh.Get());
	if (Box.IsValid)
	{
		Box = Box.TransformBy(FTransform(ViewTurn));
	}
	for (const FMantleGhostCapsule& Capsule : Ghost)
	{
		const FVector Extent(Capsule.RadiusCm);
		Box += FBox(ToView(Capsule.Start) - Extent, ToView(Capsule.Start) + Extent);
		Box += FBox(ToView(Capsule.End) - Extent, ToView(Capsule.End) + Extent);
	}
	if (!Box.IsValid)
	{
		Box = FBox(FVector(-50.0), FVector(50.0));
	}
	SetViewRotation(FRotator(-20.0, -135.0, 0.0));
	FocusViewportOnBox(Box, true);
}

void FMantleBuilderViewportClient::Tick(float DeltaSeconds)
{
	FEditorViewportClient::Tick(DeltaSeconds);
	if (UWorld* ViewWorld = GetWorld())
	{
		ViewWorld->Tick(LEVELTICK_All, DeltaSeconds);
	}
}

void FMantleBuilderViewportClient::Draw(const FSceneView* View, FPrimitiveDrawInterface* PDI)
{
	using namespace MantleBuilderViewportPrivate;
	FEditorViewportClient::Draw(View, PDI);

	// The ghost: the solid body in the scene (UpdateSolidGhost), else its wire outline, grey, red where the kind says it cannot reach.
	for (const FMantleGhostCapsule& Capsule : bSolidGhost ? TArray<FMantleGhostCapsule>() : Ghost)
	{
		const FVector Start = ToView(Capsule.Start);
		const FVector End = ToView(Capsule.End);
		const double Radius = Capsule.RadiusCm;
		const FVector Axis = End - Start;
		const double Length = Axis.Size();
		const FLinearColor Color = Capsule.bWarning ? WarningColor : GhostColor;
		if (Length < KINDA_SMALL_NUMBER)
		{
			DrawWireSphere(PDI, Start, Color, Radius, 12, SDPG_Foreground);
			continue;
		}
		const FMatrix Frame = FRotationMatrix::MakeFromZ(Axis / Length);
		DrawWireCapsule(PDI, (Start + End) * 0.5, Frame.GetScaledAxis(EAxis::X), Frame.GetScaledAxis(EAxis::Y), Frame.GetScaledAxis(EAxis::Z),
			Color, Radius, Length * 0.5 + Radius, 12, SDPG_Foreground);
	}

	// What the kind wants drawn on top: boxes (red when bad), lines and markers.
	if (bShowOverlay)
	{
		for (const FMantleBuilderOverlayBox& Box : Overlay.Boxes)
		{
			DrawWireBox(PDI, FQuatRotationTranslationMatrix(ViewTurn * Box.Rotation, ToView(Box.Center)), FBox(-Box.HalfExtent, Box.HalfExtent),
				Box.bWarning ? WarningColor : FLinearColor(0.35f, 0.75f, 1.f), SDPG_Foreground);
		}
		for (FMantleGhostCapsule Capsule : Overlay.Capsules)
		{
			Capsule.Start = ToView(Capsule.Start);
			Capsule.End = ToView(Capsule.End);
			const FLinearColor Color = Capsule.bWarning ? WarningColor : FLinearColor(0.35f, 0.75f, 1.f);
			const FVector Axis = Capsule.End - Capsule.Start;
			const double Length = Axis.Size();
			if (Length < KINDA_SMALL_NUMBER)
			{
				DrawWireSphere(PDI, Capsule.Start, Color, Capsule.RadiusCm, 12, SDPG_Foreground);
				continue;
			}
			const FMatrix Frame = FRotationMatrix::MakeFromZ(Axis / Length);
			DrawWireCapsule(PDI, (Capsule.Start + Capsule.End) * 0.5, Frame.GetScaledAxis(EAxis::X), Frame.GetScaledAxis(EAxis::Y), Frame.GetScaledAxis(EAxis::Z),
				Color, Capsule.RadiusCm, Length * 0.5 + Capsule.RadiusCm, 12, SDPG_Foreground);
		}
		for (const FMantleBuilderOverlayLine& Line : Overlay.Lines)
		{
			PDI->DrawLine(ToView(Line.Start), ToView(Line.End), Line.Color, SDPG_Foreground);
		}
		for (const FMantleBuilderOverlayMarker& Marker : Overlay.Markers)
		{
			DrawWireSphere(PDI, ToView(Marker.Location), Marker.Color, Marker.RadiusCm, 12, SDPG_Foreground);
		}
	}

	// The handles, a constant size on screen.
	for (const FMantleBuilderPoint& Point : Points)
	{
		const FMantleBuilderPointRole* Role = FindRole(Point.Role);
		const EMantleBuilderPointShape Shape = Role ? Role->Shape : EMantleBuilderPointShape::Dot;
		const bool bCreator = Point.Source == EMantleBuilderPointSource::Creator;
		const bool bSelected = Point.Role == SelectedRole;
		const FTransform ToMesh = PointToView(Point);
		const FVector Location = ToMesh.GetLocation();
		const double PixelScale = View->WorldToScreen(Location).W * (4.0 / View->UnscaledViewRect.Width() / View->ViewMatrices.GetViewToClip().M[0][0]);
		const double Size = 6.0 * PixelScale;
		// A guess is dimmer and smaller than a placement the creator made.
		const FLinearColor Color = bCreator ? ShapeColor(Shape) : ShapeColor(Shape) * 0.55f;

		PDI->SetHitProxy(new HMantleBuilderPointProxy(Point.Role));
		PDI->DrawPoint(Location, Color, bCreator ? 14.f : 9.f, SDPG_Foreground);
		switch (Shape)
		{
		case EMantleBuilderPointShape::Hand:
			DrawWireSphere(PDI, Location, Color, Size, 12, SDPG_Foreground);
			break;
		case EMantleBuilderPointShape::Foot:
			DrawWireBox(PDI, ToMesh.ToMatrixNoScale(), FBox(FVector(-Size * 1.5, -Size * 0.7, -Size * 0.4), FVector(Size * 1.5, Size * 0.7, Size * 0.4)), Color, SDPG_Foreground);
			break;
		case EMantleBuilderPointShape::Seat:
			DrawWireBox(PDI, ToMesh.ToMatrixNoScale(), FBox(FVector(-Size * 1.5, -Size * 1.5, -Size * 0.3), FVector(Size * 1.5, Size * 1.5, Size * 0.3)), Color, SDPG_Foreground);
			break;
		case EMantleBuilderPointShape::Arrow:
			DrawDirectionalArrow(PDI, ToMesh.ToMatrixNoScale(), Color, Size * 4.0, Size * 1.2, SDPG_Foreground);
			break;
		default:
			break;
		}
		if (bSelected)
		{
			DrawWireSphere(PDI, Location, FLinearColor::White, Size * 1.8, 16, SDPG_Foreground);
		}
		PDI->SetHitProxy(nullptr);
	}
}

void FMantleBuilderViewportClient::DrawCanvas(FViewport& InViewport, FSceneView& View, FCanvas& Canvas)
{
	FEditorViewportClient::DrawCanvas(InViewport, View, Canvas);
	UFont* Font = GEngine ? GEngine->GetSmallFont() : nullptr;
	if (!Font)
	{
		return;
	}
	const float Dpi = FMath::Max(Canvas.GetDPIScale(), 0.01f);

	auto DrawLabel = [&](const FText& Text, float X, float Y, const FLinearColor& Color)
	{
		FCanvasTextItem Item(FVector2D(X, Y), Text, Font, Color);
		Item.EnableShadow(FLinearColor::Black);
		Canvas.DrawItem(Item);
	};

	float Y = 40.f;
	for (const FText& Warning : Warnings)
	{
		DrawLabel(Warning, 12.f, Y, MantleBuilderViewportPrivate::WarningColor);
		Y += 16.f;
	}

	// Every handle's name beside it, so a creator knows what they are clicking; the selected one is white.
	for (const FMantleBuilderPoint& Point : Points)
	{
		if (const FMantleBuilderPointRole* Role = FindRole(Point.Role))
		{
			FVector2D Screen;
			if (FSceneView::ProjectWorldToScreen(PointToView(Point).GetLocation(), View.UnscaledViewRect, View.ViewMatrices.GetWorldToClip(), Screen))
			{
				const bool bSelected = Point.Role == SelectedRole;
				DrawLabel(Role->Label, Screen.X / Dpi + 14.f, Screen.Y / Dpi - 24.f, bSelected ? FLinearColor::White : FLinearColor(0.7f, 0.7f, 0.7f));
			}
		}
	}

	if (bShowOverlay)
	{
		for (const FMantleBuilderOverlayMarker& Marker : Overlay.Markers)
		{
			FVector2D Screen;
			if (!Marker.Label.IsEmpty() && FSceneView::ProjectWorldToScreen(ToView(Marker.Location), View.UnscaledViewRect, View.ViewMatrices.GetWorldToClip(), Screen))
			{
				DrawLabel(Marker.Label, Screen.X / Dpi + 10.f, Screen.Y / Dpi - 20.f, Marker.Color);
			}
		}
	}

	// How to use the picture, always in view at the bottom. A kind with no points to drag says its own.
	DrawLabel(Hint.IsEmpty() ? NSLOCTEXT("MantleBuilderViewport", "Hint", "Drag a point to move it; it settles onto the model when you let go. Right-drag to turn the view, scroll to zoom.") : Hint,
		12.f, InViewport.GetSizeXY().Y / Dpi - 22.f, FLinearColor(0.85f, 0.85f, 0.85f));
}

void FMantleBuilderViewportClient::ProcessClick(FSceneView& View, HHitProxy* HitProxy, FKey Key, EInputEvent Event, uint32 HitX, uint32 HitY)
{
	// A left click (not a drag, which the viewport treats as moving the camera) picks a point.
	if (Key == EKeys::LeftMouseButton && Event == IE_Released)
	{
		if (HitProxy && HitProxy->IsA(HMantleBuilderPointProxy::StaticGetType()))
		{
			SetSelectedRole(static_cast<HMantleBuilderPointProxy*>(HitProxy)->Role);
			return;
		}
		if (!(HitProxy && HitProxy->IsA(HWidgetAxis::StaticGetType())))
		{
			// A handle is only a few pixels across and its hit proxy is exact, so also take the nearest handle within a few pixels.
			TArray<MantleBuilderViewportMath::FScreenHandle> Handles;
			Handles.Reserve(Points.Num());
			for (const FMantleBuilderPoint& Point : Points)
			{
				MantleBuilderViewportMath::FScreenHandle& Handle = Handles.AddDefaulted_GetRef();
				Handle.Role = Point.Role;
				if (!FSceneView::ProjectWorldToScreen(PointToView(Point).GetLocation(), View.UnscaledViewRect, View.ViewMatrices.GetWorldToClip(), Handle.Screen))
				{
					Handles.Pop();
				}
			}
			const FName Picked = MantleBuilderViewportMath::PickHandle(Handles, FVector2D(static_cast<double>(HitX), static_cast<double>(HitY)));
			if (!Picked.IsNone())
			{
				SetSelectedRole(Picked);
				return;
			}
		}
	}
	FEditorViewportClient::ProcessClick(View, HitProxy, Key, Event, HitX, HitY);
}

bool FMantleBuilderViewportClient::InputKey(const FInputKeyEventArgs& EventArgs)
{
	// Not while flying the camera with the right mouse button (W and E are its movement keys).
	const bool bFlying = EventArgs.Viewport && EventArgs.Viewport->KeyState(EKeys::RightMouseButton);
	// Press on a point to pick it up; it follows the mouse until let go.
	if (EventArgs.Key == EKeys::LeftMouseButton && EventArgs.Viewport)
	{
		if (EventArgs.Event == IE_Pressed && !bFlying
			&& BeginPointDrag(EventArgs.Viewport, EventArgs.Viewport->GetMouseX(), EventArgs.Viewport->GetMouseY()))
		{
			return true;
		}
		if (EventArgs.Event == IE_Released && bPointDragging)
		{
			EndPointDrag();
			return true;
		}
	}
	if (EventArgs.Event == IE_Pressed && !bFlying)
	{
		if (EventArgs.Key == EKeys::W)
		{
			SetWidgetMode(UE::Widget::WM_Translate);
			return true;
		}
		if (EventArgs.Key == EKeys::E)
		{
			SetWidgetMode(UE::Widget::WM_Rotate);
			return true;
		}
		if (EventArgs.Key == EKeys::F)
		{
			Focus();
			return true;
		}
	}
	return FEditorViewportClient::InputKey(EventArgs);
}

bool FMantleBuilderViewportClient::CanSetWidgetMode(UE::Widget::EWidgetMode NewMode) const
{
	if (NewMode == UE::Widget::WM_Translate)
	{
		return true;
	}
	const FMantleBuilderPointRole* Role = FindRole(SelectedRole);
	return NewMode == UE::Widget::WM_Rotate && Role && Role->bRotates;
}

void FMantleBuilderViewportClient::SetWidgetMode(UE::Widget::EWidgetMode NewMode)
{
	if (CanSetWidgetMode(NewMode))
	{
		Mode = NewMode;
		Invalidate();
	}
}

UE::Widget::EWidgetMode FMantleBuilderViewportClient::GetWidgetMode() const
{
	return CanSetWidgetMode(Mode) ? Mode : UE::Widget::WM_Translate;
}

FVector FMantleBuilderViewportClient::GetWidgetLocation() const
{
	if (const FMantleBuilderPoint* Point = FindPoint(SelectedRole))
	{
		return PointToView(*Point).GetLocation();
	}
	return FVector::ZeroVector;
}

FMatrix FMantleBuilderViewportClient::GetWidgetCoordSystem() const
{
	return FMatrix::Identity;
}

void FMantleBuilderViewportClient::TrackingStarted(const FInputEventState& InInputState, bool bIsDraggingWidget, bool bNudge)
{
	bDragged = false;
	FEditorViewportClient::TrackingStarted(InInputState, bIsDraggingWidget, bNudge);
}

bool FMantleBuilderViewportClient::InputWidgetDelta(FViewport* InViewport, EAxisList::Type CurrentAxis, FVector& Drag, FRotator& Rot, FVector& Scale)
{
	FMantleBuilderPoint* Point = FindPoint(SelectedRole);
	const FMantleBuilderPointRole* Role = FindRole(SelectedRole);
	if (!Point || !Role || CurrentAxis == EAxisList::None)
	{
		return false;
	}
	const bool bRotating = GetWidgetMode() == UE::Widget::WM_Rotate;
	// The gizmo moves in the view: back into mesh space.
	const FVector MeshDrag = ViewTurn.UnrotateVector(Drag);
	const FRotator MeshRot = (ViewTurn.Inverse() * Rot.Quaternion() * ViewTurn).Rotator();
	Point->Transform = MantleBuilderViewportMath::ApplyDrag(*Role, ParentToMesh(*Point), Point->Transform,
		bRotating ? FVector::ZeroVector : MeshDrag, bRotating ? MeshRot : FRotator::ZeroRotator);
	Point->Source = EMantleBuilderPointSource::Creator;
	bDragged = true;
	Refresh();
	return true;
}

void FMantleBuilderViewportClient::TrackingStopped()
{
	FEditorViewportClient::TrackingStopped();
	if (!bDragged)
	{
		return;
	}
	bDragged = false;
	if (FMantleBuilderPoint* Point = FindPoint(SelectedRole))
	{
		if (Kind.IsValid())
		{
			Point->Transform = Kind->Snap(Point->Role, Point->Transform, Mesh.Get());
		}
		Refresh();
		OnPointsChanged.ExecuteIfBound();
	}
}

FSceneView* FMantleBuilderViewportClient::CalcViewForMouse(FSceneViewFamilyContext& Family, FViewport* InViewport)
{
	return CalcSceneView(&Family);
}

bool FMantleBuilderViewportClient::BeginPointDrag(FViewport* InViewport, int32 X, int32 Y)
{
	if (!InViewport || Points.Num() == 0)
	{
		return false;
	}
	// The gizmo's arrows keep their own drag.
	if (HHitProxy* HitProxy = InViewport->GetHitProxy(X, Y); HitProxy && HitProxy->IsA(HWidgetAxis::StaticGetType()))
	{
		return false;
	}
	FSceneViewFamilyContext Family(FSceneViewFamily::ConstructionValues(InViewport, GetScene(), EngineShowFlags).SetRealtimeUpdate(IsRealtime()));
	FSceneView* View = CalcViewForMouse(Family, InViewport);
	if (!View)
	{
		return false;
	}
	TArray<MantleBuilderViewportMath::FScreenHandle> Handles;
	for (const FMantleBuilderPoint& Point : Points)
	{
		MantleBuilderViewportMath::FScreenHandle& Handle = Handles.AddDefaulted_GetRef();
		Handle.Role = Point.Role;
		if (!FSceneView::ProjectWorldToScreen(PointToView(Point).GetLocation(), View->UnscaledViewRect, View->ViewMatrices.GetWorldToClip(), Handle.Screen))
		{
			Handles.Pop();
		}
	}
	const FName Picked = MantleBuilderViewportMath::PickHandle(Handles, FVector2D(X, Y));
	const FMantleBuilderPoint* Point = FindPoint(Picked);
	if (!Point)
	{
		return false;
	}
	SetSelectedRole(Picked);
	const FViewportCursorLocation Cursor(View, this, X, Y);
	PointDragPlaneOrigin = PointToView(*Point).GetLocation();
	PointDragPlaneNormal = View->GetViewDirection();
	FVector Hit;
	PointDragOffset = MantleBuilderViewportMath::RayOnPlane(Cursor.GetOrigin(), Cursor.GetDirection(), PointDragPlaneOrigin, PointDragPlaneNormal, Hit)
		? PointDragPlaneOrigin - Hit : FVector::ZeroVector;
	bPointDragging = true;
	bDragged = false;
	return true;
}

void FMantleBuilderViewportClient::UpdatePointDrag(FViewport* InViewport, int32 X, int32 Y)
{
	FMantleBuilderPoint* Point = FindPoint(SelectedRole);
	if (!bPointDragging || !Point || !InViewport)
	{
		return;
	}
	FSceneViewFamilyContext Family(FSceneViewFamily::ConstructionValues(InViewport, GetScene(), EngineShowFlags).SetRealtimeUpdate(IsRealtime()));
	FSceneView* View = CalcViewForMouse(Family, InViewport);
	FVector Hit;
	if (!View)
	{
		return;
	}
	const FViewportCursorLocation Cursor(View, this, X, Y);
	if (!MantleBuilderViewportMath::RayOnPlane(Cursor.GetOrigin(), Cursor.GetDirection(), PointDragPlaneOrigin, PointDragPlaneNormal, Hit))
	{
		return;
	}
	// The new place in mesh space, back into the point's parent; its turn is kept.
	FTransform Parent = ParentToMesh(*Point);
	Parent.SetScale3D(FVector::OneVector);
	FTransform InMesh = PointToMesh(*Point);
	InMesh.SetLocation(ViewTurn.UnrotateVector(Hit + PointDragOffset));
	FTransform Result = InMesh.GetRelativeTransform(Parent);
	Result.SetScale3D(Point->Transform.GetScale3D());
	Point->Transform = Result;
	Point->Source = EMantleBuilderPointSource::Creator;
	bDragged = true;
	Refresh();
}

void FMantleBuilderViewportClient::EndPointDrag()
{
	bPointDragging = false;
	if (!bDragged)
	{
		return; // a click without a move: the point is selected, nothing changed
	}
	bDragged = false;
	if (FMantleBuilderPoint* Point = FindPoint(SelectedRole))
	{
		if (Kind.IsValid())
		{
			Point->Transform = Kind->Snap(Point->Role, Point->Transform, Mesh.Get());
		}
		Refresh();
		OnPointsChanged.ExecuteIfBound();
	}
}

void FMantleBuilderViewportClient::MouseMove(FViewport* InViewport, int32 X, int32 Y)
{
	if (bPointDragging)
	{
		UpdatePointDrag(InViewport, X, Y);
		return;
	}
	FEditorViewportClient::MouseMove(InViewport, X, Y);
}

void FMantleBuilderViewportClient::CapturedMouseMove(FViewport* InViewport, int32 X, int32 Y)
{
	if (bPointDragging)
	{
		UpdatePointDrag(InViewport, X, Y);
		return;
	}
	FEditorViewportClient::CapturedMouseMove(InViewport, X, Y);
}

// ---- the widget ----------------------------------------------------------------------------------------------------

void SMantleBuilderViewport::Construct(const FArguments& InArgs)
{
	Kind = InArgs._Kind;
	OnPointsChanged = InArgs._OnPointsChanged;
	Scene = MakeUnique<MantleBuilderViewportPrivate::FStage>();

	StaticComponent = NewObject<UStaticMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient);
	StaticComponent->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	StaticComponent->SetGenerateOverlapEvents(false);
	StaticComponent->SetCanEverAffectNavigation(false);
	StaticComponent->SetVisibility(false);
	Scene->AddComponent(StaticComponent, FTransform::Identity);

	SkeletalComponent = NewObject<USkeletalMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient);
	SkeletalComponent->SetDisablePostProcessBlueprint(true);
	SkeletalComponent->SetVisibility(false);
	Scene->AddComponent(SkeletalComponent, FTransform::Identity);

	SEditorViewport::Construct(SEditorViewport::FArguments());
	Client->SetModelComponents(StaticComponent, SkeletalComponent);
	Client->SetKind(Kind);
	Client->OnPointsChanged = OnPointsChanged;
}

SMantleBuilderViewport::~SMantleBuilderViewport()
{
	if (Client.IsValid())
	{
		Client->Viewport = nullptr;
	}
}

TSharedRef<FEditorViewportClient> SMantleBuilderViewport::MakeEditorViewportClient()
{
	Client = MakeShared<FMantleBuilderViewportClient>(*Scene, SharedThis(this));
	return Client.ToSharedRef();
}

void SMantleBuilderViewport::AddReferencedObjects(FReferenceCollector& Collector)
{
	Collector.AddReferencedObject(StaticComponent);
	Collector.AddReferencedObject(SkeletalComponent);
	Collector.AddReferencedObject(Mesh);
}

void SMantleBuilderViewport::SetMesh(UObject* InMesh, const bool bFrame)
{
	UStaticMesh* Static = Cast<UStaticMesh>(InMesh);
	USkeletalMesh* Skeletal = Cast<USkeletalMesh>(InMesh);
	Mesh = (Static || Skeletal) ? InMesh : nullptr;
	StaticComponent->SetStaticMesh(Static);
	StaticComponent->SetVisibility(Static != nullptr);
	SkeletalComponent->SetSkeletalMesh(Skeletal);
	SkeletalComponent->SetVisibility(Skeletal != nullptr);
	Client->SetMesh(Mesh, bFrame);
}

void SMantleBuilderViewport::SetPoints(const TArray<FMantleBuilderPoint>& Points)
{
	Client->SetPoints(Points);
}

const TArray<FMantleBuilderPoint>& SMantleBuilderViewport::GetPoints() const
{
	return Client->GetPoints();
}

void SMantleBuilderViewport::SetSelectedRole(FName Role)
{
	Client->SetSelectedRole(Role);
}

FName SMantleBuilderViewport::GetSelectedRole() const
{
	return Client->GetSelectedRole();
}

void SMantleBuilderViewport::ResetSelectedToGuess()
{
	Client->ResetSelectedToGuess();
}

void SMantleBuilderViewport::ResetAllToGuess()
{
	Client->ResetAllToGuess();
}

void SMantleBuilderViewport::SetShowOverlay(bool bShow)
{
	Client->SetShowOverlay(bShow);
}

void SMantleBuilderViewport::RefreshFromKind()
{
	Client->RefreshFromKind();
}
