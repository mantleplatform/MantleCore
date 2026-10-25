// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCosmeticPreviewViewport.h"
#include "MantleAccessoryPlacement.h"
#include "MantleAccessorySettings.h"
#include "MantleAvatarContract.h"
#include "MantleClipAnimInstance.h"
#include "MantleCosmeticPreview.h"
#include "MantleCosmeticPreviewRetarget.h"
#include "MantleCosmeticThumbnail.h"

#include "Animation/AnimationAsset.h"
#include "Animation/Skeleton.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "EditorViewportClient.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "ThumbnailHelpers.h"
#include "UObject/Package.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleCosmeticPreviewViewportPrivate
{
	/** The thumbnail capture's scene (sky, three lights, ambient cubemap, floor at z = 0); the viewport brings its own camera. */
	class FPreviewStage : public FThumbnailPreviewScene
	{
	public:
		FPreviewStage()
			: FThumbnailPreviewScene(FThumbnailPreviewScene::FConstructionValues().SetCreateFloorPlane(true))
		{
		}

	protected:
		virtual void GetViewMatrixParameters(const float InFOVDegrees, FVector& OutOrigin, float& OutOrbitPitch, float& OutOrbitYaw, float& OutOrbitZoom) const override
		{
			OutOrigin = FVector::ZeroVector;
			OutOrbitPitch = 0.0f;
			OutOrbitYaw = 0.0f;
			OutOrbitZoom = 0.0f;
		}
	};

	/** An editor viewport client that ticks the stage's world, so an animation plays. */
	class FPreviewViewportClient : public FEditorViewportClient
	{
	public:
		FPreviewViewportClient(FPreviewScene& InStage, const TSharedRef<SEditorViewport>& InWidget)
			: FEditorViewportClient(nullptr, &InStage, InWidget)
			, Stage(InStage)
		{
			SetRealtime(true);
			ViewFOV = FMantleCosmeticThumbnail::FieldOfViewDegrees * 1.5f;
			EngineShowFlags.SetGrid(false);
		}

		virtual void Tick(float DeltaSeconds) override
		{
			FEditorViewportClient::Tick(DeltaSeconds);
			if (UWorld* World = Stage.GetWorld())
			{
				World->Tick(LEVELTICK_All, DeltaSeconds);
			}
		}

	private:
		FPreviewScene& Stage;
	};

	/** The thumbnail's body view: 20 degrees off the front (meshes face +Y), slightly from above. */
	const FRotator BodyView(-4.0f, -70.0f, 0.0f);

	USkeletalMeshComponent* MakeComponent(FPreviewScene& Stage)
	{
		USkeletalMeshComponent* Component = NewObject<USkeletalMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient);
		// No post-process Blueprint (MetaHuman's correctives): the pose the client's fit sees.
		Component->SetDisablePostProcessBlueprint(true);
		Component->SetVisibility(false);
		Stage.AddComponent(Component, FTransform::Identity);
		return Component;
	}
}

void SMantleCosmeticPreviewViewport::Construct(const FArguments& InArgs)
{
	using namespace MantleCosmeticPreviewViewportPrivate;
	Scene = MakeUnique<FPreviewStage>();
	BodyComponent = MakeComponent(*Scene);
	FaceComponent = MakeComponent(*Scene);
	GarmentComponent = MakeComponent(*Scene);
	AccessoryComponent = NewObject<UStaticMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient);
	AccessoryComponent->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	AccessoryComponent->SetGenerateOverlapEvents(false);
	AccessoryComponent->SetCanEverAffectNavigation(false);
	AccessoryComponent->SetVisibility(false);
	Scene->AddComponent(AccessoryComponent, FTransform::Identity);
	// #283: never drawn, but it animates and refreshes its bones every frame: the avatar's retarget reads them.
	DriverComponent = MakeComponent(*Scene);
	DriverComponent->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	SEditorViewport::Construct(SEditorViewport::FArguments());
}

SMantleCosmeticPreviewViewport::~SMantleCosmeticPreviewViewport()
{
	if (Client.IsValid())
	{
		Client->Viewport = nullptr;
	}
}

TSharedRef<FEditorViewportClient> SMantleCosmeticPreviewViewport::MakeEditorViewportClient()
{
	Client = MakeShared<MantleCosmeticPreviewViewportPrivate::FPreviewViewportClient>(*Scene, SharedThis(this));
	Client->SetViewLocation(FVector(0.0, -400.0, 100.0));
	Client->SetViewRotation(MantleCosmeticPreviewViewportPrivate::BodyView);
	return Client.ToSharedRef();
}

void SMantleCosmeticPreviewViewport::AddReferencedObjects(FReferenceCollector& Collector)
{
	Collector.AddReferencedObject(BodyComponent);
	Collector.AddReferencedObject(FaceComponent);
	Collector.AddReferencedObject(GarmentComponent);
	Collector.AddReferencedObject(AccessoryComponent);
	Collector.AddReferencedObject(DriverComponent);
	Collector.AddReferencedObject(Animation);
	Collector.AddReferencedObject(AvatarRetargeter);
}

USkeletalMesh* SMantleCosmeticPreviewViewport::GetShownBody() const
{
	return BodyComponent && BodyComponent->IsVisible() ? BodyComponent->GetSkeletalMeshAsset() : nullptr;
}

void SMantleCosmeticPreviewViewport::Show(const FMantlePreviewView& View)
{
	if (!BodyComponent || !FaceComponent || !GarmentComponent)
	{
		return;
	}
	const bool bReady = View.State == EMantlePreviewState::Ready && View.Body;
	if (!bReady)
	{
		// While a request runs, what was shown stays; a failure or a reset clears the stage.
		if (View.State == EMantlePreviewState::Failed || View.State == EMantlePreviewState::Idle)
		{
			ClearStage();
		}
		return;
	}
	LeaveAvatarMode();
	AccessoryComponent->SetVisibility(false);
	AccessoryComponent->SetStaticMesh(nullptr);
	BodyComponent->SetSkeletalMeshAsset(View.Body);
	BodyComponent->EmptyOverrideMaterials();
	BodyComponent->SetVisibility(true);

	FaceComponent->SetSkeletalMeshAsset(View.Face);
	FaceComponent->EmptyOverrideMaterials();
	FaceComponent->SetVisibility(View.Face != nullptr);
	FaceComponent->SetLeaderPoseComponent(View.Face ? BodyComponent.Get() : nullptr, /*bForceUpdate*/ true);

	GarmentComponent->SetSkeletalMeshAsset(View.Garment);
	GarmentComponent->SetVisibility(View.Garment != nullptr);
	GarmentComponent->AttachToComponent(View.bGarmentFollowsFace ? FaceComponent.Get() : BodyComponent.Get(), FAttachmentTransformRules::KeepRelativeTransform);
	GarmentComponent->SetRelativeTransform(FTransform::Identity);
	GarmentComponent->SetLeaderPoseComponent(View.Garment ? (View.bGarmentFollowsFace ? FaceComponent.Get() : BodyComponent.Get()) : nullptr, /*bForceUpdate*/ true);

	ApplyAnimation();
	if (FramedTarget != View.Target.Key())
	{
		FramedTarget = View.Target.Key();
		Frame(View.Body);
	}
	if (Client.IsValid())
	{
		Client->Invalidate();
	}
}

bool SMantleCosmeticPreviewViewport::ShowOverlay(USkeletalMesh* Body, USkeletalMesh* Face, const TMap<int32, UMaterialInterface*>& BodyMaterials,
	const TMap<int32, UMaterialInterface*>& FaceMaterials, bool bFrameFace, const FString& Key)
{
	if (!BodyComponent || !FaceComponent || !GarmentComponent || !Body)
	{
		return false;
	}
	LeaveAvatarMode();
	AccessoryComponent->SetVisibility(false);
	AccessoryComponent->SetStaticMesh(nullptr);
	GarmentComponent->SetLeaderPoseComponent(nullptr);
	GarmentComponent->SetVisibility(false);
	GarmentComponent->SetSkeletalMeshAsset(nullptr);

	// The reference pose: a single-node instance with nothing to play. Fresh meshes drop the previous overrides.
	BodyComponent->SetSkeletalMeshAsset(Body);
	BodyComponent->EmptyOverrideMaterials();
	BodyComponent->SetAnimationMode(EAnimationMode::AnimationSingleNode);
	BodyComponent->SetAnimation(nullptr);
	for (const TPair<int32, UMaterialInterface*>& Entry : BodyMaterials)
	{
		BodyComponent->SetMaterial(Entry.Key, Entry.Value);
	}
	BodyComponent->SetVisibility(true);

	FaceComponent->SetSkeletalMeshAsset(Face);
	FaceComponent->EmptyOverrideMaterials();
	for (const TPair<int32, UMaterialInterface*>& Entry : FaceMaterials)
	{
		FaceComponent->SetMaterial(Entry.Key, Entry.Value);
	}
	FaceComponent->SetVisibility(Face != nullptr);
	FaceComponent->SetLeaderPoseComponent(Face ? BodyComponent.Get() : nullptr, /*bForceUpdate*/ true);

	if (FramedTarget != Key)
	{
		FramedTarget = Key;
		Frame(bFrameFace && Face ? Face : Body);
	}
	if (Client.IsValid())
	{
		Client->Invalidate();
	}
	return true;
}

bool SMantleCosmeticPreviewViewport::ShowAccessory(UStaticMesh* Mesh, const FString& Slot, const FMantleCosmeticTransform& Attach,
	const TMap<FString, FMantleCosmeticTransform>& Sockets, FString& OutError)
{
	OutError.Reset();
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get();
	const FMantleContractSlot* Point = Contract ? Contract->Slots.FindByPredicate([&Slot](const FMantleContractSlot& Entry) { return Entry.Id == Slot && Entry.Group == TEXT("accessory"); }) : nullptr;
	const USkeletalMesh* Body = GetShownBody();
	const FName Bone = Point ? FName(*Point->SocketBone) : NAME_None;
	if (!Mesh || !Point || !Body || BodyComponent->GetBoneIndex(Bone) == INDEX_NONE)
	{
		AccessoryComponent->SetVisibility(false);
		OutError = TEXT("Choose an accessory mesh and a MetaHuman body with the selected attachment bone.");
		return false;
	}
	// Mantle's standardized accessory sockets sit at identity on these bones. An imported MetaHuman need not
	// contain Mantle sockets to preview: attaching to the contract bone reproduces the published anchor.
	AccessoryComponent->AttachToComponent(BodyComponent, FAttachmentTransformRules::KeepRelativeTransform, Bone);
	// The creator sees what uploads: the reduced copy when the mesh is over the triangle limit, and centred on the socket
	// when its pivot is out of reach (the export does both).
	Mesh = const_cast<UStaticMesh*>(FMantleAccessorySettings::UploadMesh(Mesh, Contract->RigidAccessory));
	AccessoryComponent->SetStaticMesh(Mesh);
	FTransform Placed = MantleAccessoryPlacement::ClampedAttach(Attach);
	const FMantleContractAccessoryBounds* Limits = Contract->AccessoryBounds.Find(Slot);
	FVector3f Recenter = FVector3f::ZeroVector;
	if (Limits)
	{
		FBox3f Bounds(Mesh->GetBoundingBox());
		Recenter = FMantleAccessorySettings::RecenterOffset(Bounds, *Limits);
		Bounds = Bounds.ShiftBy(-Recenter);
		const float Factor = MantleAccessoryPlacement::BoundsClampFactor(Bounds, Placed.GetScale3D(), Limits->MaxDimensionCm, Limits->MaxReachCm);
		Placed.SetScale3D(Placed.GetScale3D() * Factor);
	}
	AccessoryComponent->SetRelativeTransform(FTransform(FVector(-Recenter)) * MantleAccessoryPlacement::ComposeRelative(
		Placed, FTransform::Identity, Sockets.Find(Slot)));
	AccessoryComponent->SetVisibility(true);
	if (Client.IsValid()) Client->Invalidate();
	return true;
}

bool SMantleCosmeticPreviewViewport::ShowAvatar(const FMantleAvatarPreviewScene& InScene)
{
	if (!BodyComponent || !FaceComponent || !GarmentComponent || !DriverComponent || !InScene.Body)
	{
		return false;
	}
	bAvatarMode = true;
	AccessoryComponent->SetVisibility(false);
	AccessoryComponent->SetStaticMesh(nullptr);
	GarmentComponent->SetLeaderPoseComponent(nullptr);
	GarmentComponent->SetVisibility(false);
	GarmentComponent->SetSkeletalMeshAsset(nullptr);

	// The client's pose: the hidden UEFN driver plays the idle, the body copies it through the retargeter.
	const bool bRetarget = InScene.Retargeter && InScene.Driver && UMantlePreviewRetargetAnimInstance::IsRetargetNodeAvailable();
	AvatarRetargeter = bRetarget ? InScene.Retargeter : nullptr;
	DriverComponent->SetSkeletalMeshAsset(bRetarget ? InScene.Driver : nullptr);
	UMantleClipAnimInstance* ClipInstance = nullptr;
	if (bRetarget && InScene.IdleClip.IsValid())
	{
		// #535: the plugin's MAN1 idle, played as the client plays a clip (tracks bound by bone name, looped on the
		// world's clock); the root stays where the driver is.
		DriverComponent->SetAnimationMode(EAnimationMode::AnimationBlueprint);
		DriverComponent->SetAnimInstanceClass(UMantleClipAnimInstance::StaticClass());
		ClipInstance = Cast<UMantleClipAnimInstance>(DriverComponent->GetAnimInstance());
		if (ClipInstance)
		{
			FMantleClipPlayback Playback;
			Playback.bLoop = true;
			ClipInstance->SetApplyRootTranslation(false);
			ClipInstance->SetClip(InScene.IdleClip, Playback);
		}
	}
	if (bRetarget && !ClipInstance)
	{
		DriverComponent->SetAnimationMode(EAnimationMode::AnimationSingleNode);
		const USkeleton* DriverSkeleton = InScene.Driver->GetSkeleton();
		if (InScene.Idle && DriverSkeleton && InScene.Idle->GetSkeleton() && DriverSkeleton->IsCompatibleForEditor(InScene.Idle->GetSkeleton()))
		{
			DriverComponent->PlayAnimation(InScene.Idle, /*bLooping*/ true);
		}
		else
		{
			DriverComponent->SetAnimation(nullptr);
		}
	}

	BodyComponent->SetSkeletalMeshAsset(InScene.Body);
	BodyComponent->EmptyOverrideMaterials();
	BodyComponent->SetVisibility(true);
	bool bPosed = false;
	if (bRetarget)
	{
		// The body waits for the driver's pose each frame, as the client's body waits on its driver.
		BodyComponent->AddTickPrerequisiteComponent(DriverComponent);
		BodyComponent->SetAnimationMode(EAnimationMode::AnimationBlueprint);
		BodyComponent->SetAnimInstanceClass(UMantlePreviewRetargetAnimInstance::StaticClass());
		if (UMantlePreviewRetargetAnimInstance* Instance = Cast<UMantlePreviewRetargetAnimInstance>(BodyComponent->GetAnimInstance()))
		{
			Instance->Retargeter = InScene.Retargeter;
			Instance->Source = DriverComponent;
			bPosed = true;
		}
	}
	if (!bPosed)
	{
		// The reference pose: a single-node instance with nothing to play.
		BodyComponent->RemoveTickPrerequisiteComponent(DriverComponent);
		BodyComponent->SetAnimationMode(EAnimationMode::AnimationSingleNode);
		BodyComponent->SetAnimation(nullptr);
		DriverComponent->SetSkeletalMeshAsset(nullptr);
		AvatarRetargeter = nullptr;
	}

	FaceComponent->SetSkeletalMeshAsset(InScene.Face);
	FaceComponent->EmptyOverrideMaterials();
	FaceComponent->SetVisibility(InScene.Face != nullptr);
	FaceComponent->SetLeaderPoseComponent(InScene.Face ? BodyComponent.Get() : nullptr, /*bForceUpdate*/ true);

	if (FramedTarget != InScene.Key)
	{
		FramedTarget = InScene.Key;
		Frame(InScene.Body);
	}
	if (Client.IsValid())
	{
		Client->Invalidate();
	}
	return bPosed;
}

void SMantleCosmeticPreviewViewport::LeaveAvatarMode()
{
	if (!bAvatarMode)
	{
		return;
	}
	bAvatarMode = false;
	AvatarRetargeter = nullptr;
	if (BodyComponent && DriverComponent)
	{
		BodyComponent->RemoveTickPrerequisiteComponent(DriverComponent);
	}
	if (DriverComponent)
	{
		// Back to a single-node instance with nothing to play (the idle may have been a clip instance).
		DriverComponent->SetAnimationMode(EAnimationMode::AnimationSingleNode);
		DriverComponent->SetAnimation(nullptr);
		DriverComponent->SetSkeletalMeshAsset(nullptr);
	}
}

void SMantleCosmeticPreviewViewport::ClearStage()
{
	LeaveAvatarMode();
	AccessoryComponent->SetVisibility(false);
	AccessoryComponent->SetStaticMesh(nullptr);
	for (USkeletalMeshComponent* Component : { BodyComponent.Get(), FaceComponent.Get(), GarmentComponent.Get(), DriverComponent.Get() })
	{
		if (Component)
		{
			Component->SetLeaderPoseComponent(nullptr);
			Component->SetVisibility(false);
			Component->SetSkeletalMeshAsset(nullptr);
		}
	}
	FramedTarget.Reset();
}

void SMantleCosmeticPreviewViewport::SetAnimation(UAnimationAsset* InAnimation)
{
	Animation = InAnimation;
	ApplyAnimation();
}

void SMantleCosmeticPreviewViewport::ApplyAnimation()
{
	if (!BodyComponent || !BodyComponent->GetSkeletalMeshAsset() || bAvatarMode)
	{
		// An avatar is posed by its retarget from the driver; the creator's animation applies to garments.
		return;
	}
	const USkeleton* Skeleton = BodyComponent->GetSkeletalMeshAsset()->GetSkeleton();
	const bool bPlays = Animation && Skeleton && Animation->GetSkeleton() && Skeleton->IsCompatibleForEditor(Animation->GetSkeleton());
	BodyComponent->SetAnimationMode(EAnimationMode::AnimationSingleNode);
	if (bPlays)
	{
		BodyComponent->PlayAnimation(Animation, /*bLooping*/ true);
	}
	else
	{
		// The reference pose: a single-node instance with nothing to play.
		BodyComponent->SetAnimation(nullptr);
	}
}

void SMantleCosmeticPreviewViewport::Frame(const USkeletalMesh* Body)
{
	if (!Client.IsValid() || !Body)
	{
		return;
	}
	const FBox Bounds = Body->GetImportedBounds().GetBox();
	if (!Bounds.IsValid)
	{
		return;
	}
	const FRotator View = MantleCosmeticPreviewViewportPrivate::BodyView;
	Client->SetViewLocation(FMantleCosmeticThumbnail::FrameCamera(Bounds, View, Client->ViewFOV, FMantleCosmeticThumbnail::FrameMargin));
	Client->SetViewRotation(View);
}
