// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCosmeticThumbnail.h"
#include "MantleMetaHumanConform.h"
#include "MantleRuntime.h"

#include "AnimationRuntime.h"
#include "AssetCompilingManager.h"
#include "Components/MeshComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "ContentStreaming.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "MaterialShared.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "Materials/MaterialInterface.h"
#include "Misc/App.h"
#include "RHIGlobals.h"
#include "RHIShaderPlatform.h"
#include "RenderingThread.h"
#include "ShaderCompiler.h"
#include "TextureResource.h"
#include "ThumbnailHelpers.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UnrealEdGlobals.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleCosmeticThumbnailPrivate
{
	/**
	 * The engine's thumbnail scene (the thumbnail sky sphere, its three lights and ambient cubemap,
	 * and a floor at z = 0 when asked) with nothing in it: the scene capture brings its own camera,
	 * so the orbit view the base class asks for is never used.
	 */
	class FCaptureScene : public FThumbnailPreviewScene
	{
	public:
		explicit FCaptureScene(bool bFloor)
			: FThumbnailPreviewScene(FThumbnailPreviewScene::FConstructionValues().SetCreateFloorPlane(bFloor))
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

	FBox BoundsOf(const USkeletalMesh* Mesh)
	{
		return Mesh ? Mesh->GetImportedBounds().GetBox() : FBox(ForceInit);
	}

	/**
	 * Skeletal meshes face +Y in their own space (the mannequins', MetaHuman's and the UEFN rig's
	 * convention; a character Blueprint yaws them -90 to face +X), so a camera looking along -Y
	 * (yaw -90) sees the front. 20 degrees off it reads as a figure, not a flat cut-out.
	 */
	const FRotator BodyView(-4.0f, -70.0f, 0.0f);

	/** An accessory's axes are its socket's: from above and to the side, like the editor's own static mesh thumbnails. */
	const FRotator AccessoryView(-20.0f, -55.0f, 0.0f);

	/** An accessory turned upright as worn faces +Y like a body: from the front and a little above. */
	const FRotator WornAccessoryView(-15.0f, -65.0f, 0.0f);

	/** At most this many warm-up captures before the picture, each followed by a wait for what it asked for. */
	constexpr int32 CaptureWarmUpRounds = 8;

	/** How long a capture's settle keeps the drawn meshes' textures forced resident, and waits for them to stream in (seconds). */
	constexpr float CapturePrestreamSeconds = 30.0f;
	constexpr float CaptureStreamSeconds = 10.0f;

	/**
	 * As ThumbnailTools does for a thumbnail it keeps, and as the conform's portrait settles: loads and asset compiles
	 * (meshes, textures) finished, every shader job finished (a material whose shaders aren't ready draws as the engine's
	 * grey world-grid material), the drawn meshes' textures forced resident and streamed in, the render thread caught up.
	 */
	void CaptureSettle(TConstArrayView<TPair<UPrimitiveComponent*, const UObject*>> Drawn)
	{
		FlushAsyncLoading();
		FAssetCompilingManager::Get().FinishAllCompilation();
		if (GShaderCompilingManager)
		{
			GShaderCompilingManager->FinishAllCompilation();
		}
		for (const TPair<UPrimitiveComponent*, const UObject*>& Piece : Drawn)
		{
			// UMeshComponent's (the skeletal and static mesh components the capture draws).
			if (UMeshComponent* MeshComponent = Cast<UMeshComponent>(Piece.Key))
			{
				MeshComponent->PrestreamTextures(CapturePrestreamSeconds, /*bPrioritizeCharacterTextures*/ true);
			}
		}
		UTexture::ForceUpdateTextureStreaming();
		IStreamingManager::Get().StreamAllResources(CaptureStreamSeconds);
		IStreamingManager::Get().BlockTillAllRequestsFinished(CaptureStreamSeconds);
		FlushRenderingCommands();
	}

	/** Whether Material's shaders for this editor's platform are compiled: "ready", "compiling", "no shaders", "no resource". */
	const TCHAR* CaptureMaterialState(const UMaterialInterface* Material)
	{
		const FMaterialResource* Resource = Material ? Material->GetMaterialResource(GMaxRHIShaderPlatform) : nullptr;
		if (!Resource)
		{
			return TEXT("no resource");
		}
		if (!Resource->IsCompilationFinished())
		{
			return TEXT("compiling");
		}
		return Resource->GetGameThreadShaderMap() ? TEXT("ready") : TEXT("no shaders");
	}

	/** Every material in Materials ready (CaptureMaterialState), and no shader job left. */
	bool CaptureReady(const TArray<UMaterialInterface*>& Materials)
	{
		if (GShaderCompilingManager && GShaderCompilingManager->GetNumRemainingJobs() > 0)
		{
			return false;
		}
		return !Materials.ContainsByPredicate([](const UMaterialInterface* Material)
		{
			return FCString::Strcmp(CaptureMaterialState(Material), TEXT("ready")) != 0;
		});
	}

	/** "SKM_Body: /Game/.../MI_Body (ready), ..." for the log: what a component draws and whether its shaders are ready. */
	FString CaptureDescribe(const UPrimitiveComponent& Component, const UObject* Mesh)
	{
		TArray<UMaterialInterface*> Materials;
		Component.GetUsedMaterials(Materials);
		TArray<FString> Lines;
		for (const UMaterialInterface* Material : Materials)
		{
			Lines.Add(Material ? FString::Printf(TEXT("%s (%s)"), *Material->GetPathName(), CaptureMaterialState(Material)) : FString(TEXT("none")));
		}
		return FString::Printf(TEXT("%s: %s"), Mesh ? *Mesh->GetPathName() : TEXT("?"), *FString::Join(Lines, TEXT(", ")));
	}
}

FRotator FMantleCosmeticThumbnail::DefaultView(bool bAccessory)
{
	using namespace MantleCosmeticThumbnailPrivate;
	return bAccessory ? AccessoryView : BodyView;
}

FMantleThumbnailSubject FMantleCosmeticThumbnail::AvatarSubject(USkeletalMesh* Body, USkeletalMesh* Face)
{
	using namespace MantleCosmeticThumbnailPrivate;
	FMantleThumbnailSubject Subject;
	if (!Body)
	{
		return Subject;
	}
	Subject.SkeletalMeshes.Add(Body);
	FBox Bounds = BoundsOf(Body);
	if (Face)
	{
		Subject.SkeletalMeshes.Add(Face);
		Bounds += BoundsOf(Face);
	}
	// Every avatar upload's thumbnail is a headshot. A MetaHuman face shares the body's skeleton
	// space, so either one's head bone frames it.
	TOptional<FVector> HeadJoint = HeadJointOf(Body);
	if (!HeadJoint.IsSet())
	{
		HeadJoint = HeadJointOf(Face);
	}
	Subject.Frame = HeadshotFrame(Bounds, HeadJoint);
	Subject.View = BodyView;
	// The floor never shows in a headshot.
	Subject.bFloor = false;
	const TCHAR* const Framing = HeadJoint.IsSet() ? TEXT("headshot") : TEXT("headshot from the top (no head bone)");
	Subject.Describe = Face ? FString::Printf(TEXT("%s with %s, %s"), *Body->GetName(), *Face->GetName(), Framing)
		: FString::Printf(TEXT("%s, %s"), *Body->GetName(), Framing);
	return Subject;
}

FBox FMantleCosmeticThumbnail::HeadshotFrame(const FBox& Bounds, const TOptional<FVector>& HeadJoint)
{
	if (!Bounds.IsValid)
	{
		return FBox(ForceInit);
	}
	const FVector Joint = HeadJoint.IsSet() ? HeadJoint.GetValue()
		: FVector(Bounds.GetCenter().X, Bounds.GetCenter().Y, Bounds.Max.Z - FMantleMetaHumanConform::NoHeadDropCm);
	const double Head = FMath::Clamp(Bounds.Max.Z - Joint.Z, HeadshotMinHeadCm, HeadshotMaxHeadCm);
	const double HalfWidth = Head * HeadshotHalfWidthHeads;
	// Flat, at the joint's depth, narrower than tall: the camera fits it by its height (FrameCamera), and its margin leaves
	// the room above the crown.
	const FVector Min(Joint.X - HalfWidth, Joint.Y, FMath::Max(Joint.Z - Head * HeadshotDownHeads, Bounds.Min.Z));
	const FVector Max(Joint.X + HalfWidth, Joint.Y, Joint.Z + Head);
	return FBox(Min, Max);
}

TOptional<FVector> FMantleCosmeticThumbnail::HeadJointOf(const USkeletalMesh* Mesh)
{
	if (!Mesh)
	{
		return TOptional<FVector>();
	}
	const FReferenceSkeleton& Reference = Mesh->GetRefSkeleton();
	const int32 Head = Reference.FindBoneIndex(FName(TEXT("head")));
	if (Head == INDEX_NONE)
	{
		return TOptional<FVector>();
	}
	return FAnimationRuntime::GetComponentSpaceTransformRefPose(Reference, Head).GetLocation();
}

TArray<int32> FMantleCosmeticThumbnail::HiddenSlotsFor(int32 NumSlots, const TArray<int32>* KeepSlots)
{
	TArray<int32> Hidden;
	if (!KeepSlots)
	{
		return Hidden;
	}
	for (int32 Slot = 0; Slot < NumSlots; ++Slot)
	{
		if (!KeepSlots->Contains(Slot))
		{
			Hidden.Add(Slot);
		}
	}
	return Hidden;
}

FMantleThumbnailSubject FMantleCosmeticThumbnail::GarmentSubject(USkeletalMesh* Garment, USkeletalMesh* Body, USkeletalMesh* Face,
	const TArray<int32>* KeepSlots, const FBox& KeptBounds)
{
	using namespace MantleCosmeticThumbnailPrivate;
	FMantleThumbnailSubject Subject;
	if (!Garment)
	{
		return Subject;
	}
	Subject.SkeletalMeshes.Add(Garment);
	Subject.HiddenSlots.Add(HiddenSlotsFor(Garment->GetMaterials().Num(), KeepSlots));
	if (Body)
	{
		Subject.SkeletalMeshes.Add(Body);
		if (Face)
		{
			Subject.SkeletalMeshes.Add(Face);
		}
	}
	// Framed on the garment with some of the body around it: a shirt from the hips to the neck,
	// shoes at the feet. Never tighter than 40 cm, so small items keep their context.
	FBox Frame = KeptBounds.IsValid ? KeptBounds : BoundsOf(Garment);
	if (Frame.IsValid)
	{
		const double Longest = Frame.GetExtent().GetMax() * 2.0;
		const double Pad = FMath::Max(Longest * 0.1, (40.0 - Longest) * 0.5);
		Frame = Frame.ExpandBy(FMath::Max(Pad, 2.0));
	}
	Subject.Frame = Frame;
	Subject.View = BodyView;
	Subject.bFloor = Body != nullptr;
	Subject.Describe = Body ? FString::Printf(TEXT("%s on %s%s"), *Garment->GetName(), *Body->GetName(), Face ? TEXT(" with its face") : TEXT(""))
		: FString::Printf(TEXT("%s alone (no authoring body)"), *Garment->GetName());
	return Subject;
}

FMantleThumbnailSubject FMantleCosmeticThumbnail::AccessorySubject(UStaticMesh* Mesh, const FQuat& Worn, const bool bBackFaces)
{
	using namespace MantleCosmeticThumbnailPrivate;
	FMantleThumbnailSubject Subject;
	if (!Mesh)
	{
		return Subject;
	}
	Subject.StaticMesh = Mesh;
	Subject.StaticTransform = FTransform(Worn);
	if (bBackFaces)
	{
		Subject.StaticBackFaces.Reset(BackFaces(Mesh));
	}
	Subject.Frame = Mesh->GetBoundingBox().TransformBy(Subject.StaticTransform);
	// Turned as worn, it is in a body's axes (facing +Y): seen from the front, a little above, like a body.
	Subject.View = Worn.Equals(FQuat::Identity) ? AccessoryView : WornAccessoryView;
	Subject.Describe = FString::Printf(TEXT("%s alone%s"), *Mesh->GetName(), Subject.StaticBackFaces ? TEXT(", inside too") : TEXT(""));
	return Subject;
}

UStaticMesh* FMantleCosmeticThumbnail::BackFaces(const UStaticMesh* Mesh)
{
	const FMeshDescription* Source = Mesh && Mesh->GetNumSourceModels() > 0 ? Mesh->GetMeshDescription(0) : nullptr;
	if (!Source)
	{
		return nullptr;
	}
	FMeshDescription Description = *Source;
	FStaticMeshAttributes Attributes(Description);
	TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
	for (const FVertexInstanceID Instance : Description.VertexInstances().GetElementIDs())
	{
		Normals[Instance] = -Normals[Instance];
	}
	TArray<FTriangleID> Triangles;
	for (const FTriangleID Triangle : Description.Triangles().GetElementIDs())
	{
		Triangles.Add(Triangle);
	}
	for (const FTriangleID Triangle : Triangles)
	{
		const TArray<FVertexInstanceID> Corners(Description.GetTriangleVertexInstances(Triangle));
		const FPolygonGroupID Group = Description.GetTrianglePolygonGroup(Triangle);
		Description.DeleteTriangle(Triangle);
		Description.CreateTriangle(Group, { Corners[2], Corners[1], Corners[0] });
	}
	UStaticMesh* Out = NewObject<UStaticMesh>(GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UStaticMesh::StaticClass(),
		FName(*FString::Printf(TEXT("%s_BackFaces"), *Mesh->GetName()))), RF_Transient);
	Out->GetStaticMaterials() = Mesh->GetStaticMaterials();
	Out->bAllowCPUAccess = true;
	Out->SetNumSourceModels(1);
	Out->GetSourceModel(0).BuildSettings = Mesh->GetSourceModel(0).BuildSettings;
	Out->GetSourceModel(0).BuildSettings.bRecomputeNormals = false;
	UStaticMesh::FBuildMeshDescriptionsParams Params;
	Params.bMarkPackageDirty = false;
	Params.bAllowCpuAccess = true;
	return Out->BuildFromMeshDescriptions({ &Description }, Params) ? Out : nullptr;
}

bool FMantleCosmeticThumbnail::CanCapture(FString* OutWhyNot)
{
	auto No = [OutWhyNot](const TCHAR* Why)
	{
		if (OutWhyNot)
		{
			*OutWhyNot = Why;
		}
		return false;
	};
	if (GUsingNullRHI)
	{
		return No(TEXT("no renderer (-NullRHI)"));
	}
	if (!FApp::CanEverRender() || !GIsRHIInitialized)
	{
		return No(TEXT("this process does not render"));
	}
	if (!GUnrealEd)
	{
		return No(TEXT("the editor's thumbnail scene is not available"));
	}
	return true;
}

FVector FMantleCosmeticThumbnail::FrameCamera(const FBox& Frame, const FRotator& View, float FOVDegrees, float Margin)
{
	const FVector Center = Frame.GetCenter();
	const FRotationMatrix Axes(View);
	const FVector Forward = Axes.GetUnitAxis(EAxis::X);
	const FVector Right = Axes.GetUnitAxis(EAxis::Y);
	const FVector Up = Axes.GetUnitAxis(EAxis::Z);
	const double TanHalf = FMath::Tan(FMath::DegreesToRadians(static_cast<double>(FOVDegrees)) * 0.5);

	// Each corner at depth d past the centre and a across the view needs the camera at least
	// a / tan(half FOV) - d behind the centre (square view: the same half angle both ways).
	double Distance = 0.0;
	for (int32 Corner = 0; Corner < 8; ++Corner)
	{
		const FVector Point((Corner & 1) ? Frame.Max.X : Frame.Min.X, (Corner & 2) ? Frame.Max.Y : Frame.Min.Y, (Corner & 4) ? Frame.Max.Z : Frame.Min.Z);
		const FVector Offset = Point - Center;
		const double Across = FMath::Max(FMath::Abs(Offset | Right), FMath::Abs(Offset | Up)) * Margin;
		Distance = FMath::Max(Distance, Across / TanHalf - (Offset | Forward));
	}
	// Never inside the box, whatever its shape.
	Distance = FMath::Max(Distance, Frame.GetExtent().Size() + 1.0);
	return Center - Forward * Distance;
}

bool FMantleCosmeticThumbnail::Capture(const FMantleThumbnailSubject& Subject, int32 Size, TArray<FColor>& OutPixels, FString& OutError)
{
	using namespace MantleCosmeticThumbnailPrivate;
	OutPixels.Reset();
	if (!CanCapture(&OutError))
	{
		return false;
	}
	if (Subject.IsEmpty() || !Subject.Frame.IsValid || Size <= 0)
	{
		OutError = TEXT("nothing to draw");
		return false;
	}

	FCaptureScene Scene(Subject.bFloor);
	// What the picture draws, and what each piece is, for the materials' wait and the log.
	TArray<TPair<UPrimitiveComponent*, const UObject*>> Drawn;

	// The reference pose (unless Subject.Pose poses them): no animation, no post-process Blueprint (MetaHuman's correctives), LOD 0.
	TArray<USkeletalMeshComponent*> SkeletalComponents;
	for (int32 Index = 0; Index < Subject.SkeletalMeshes.Num(); ++Index)
	{
		USkeletalMesh* Mesh = Subject.SkeletalMeshes[Index];
		SkeletalComponents.Add(nullptr);
		if (!Mesh)
		{
			continue;
		}
		USkeletalMeshComponent* Component = NewObject<USkeletalMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient);
		SkeletalComponents.Last() = Component;
		Component->SetDisablePostProcessBlueprint(true);
		Component->SetSkeletalMeshAsset(Mesh);
		Component->SetForcedLOD(1);
		Scene.AddComponent(Component, FTransform::Identity);
		Drawn.Emplace(Component, Mesh);
		// #124: materials drawn in place of the mesh's own (an overlay on the skin).
		if (Subject.MaterialOverrides.IsValidIndex(Index))
		{
			for (const TPair<int32, UMaterialInterface*>& Entry : Subject.MaterialOverrides[Index])
			{
				Component->SetMaterial(Entry.Key, Entry.Value);
			}
		}
		// #483: the sections the item leaves out. After AddComponent: the component's LOD infos exist once it is registered.
		if (Subject.HiddenSlots.IsValidIndex(Index))
		{
			for (const int32 Slot : Subject.HiddenSlots[Index])
			{
				for (int32 LOD = 0; LOD < Mesh->GetLODNum(); ++LOD)
				{
					// No section index: the slot itself is hidden, whichever sections draw with it.
					Component->ShowMaterialSection(Slot, INDEX_NONE, false, LOD);
				}
			}
		}
	}
	FBox Frame = Subject.Frame;
	if (Subject.Pose)
	{
		Subject.Pose(Scene, SkeletalComponents, Frame);
	}
	if (Subject.StaticMesh)
	{
		UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient);
		Component->SetStaticMesh(Subject.StaticMesh);
		Component->SetForcedLodModel(1);
		Scene.AddComponent(Component, Subject.StaticTransform);
		Drawn.Emplace(Component, Subject.StaticMesh);
	}
	if (Subject.StaticMesh && Subject.StaticBackFaces)
	{
		UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient);
		Component->SetStaticMesh(Subject.StaticBackFaces.Get());
		Component->SetForcedLodModel(1);
		Scene.AddComponent(Component, Subject.StaticTransform);
		Drawn.Emplace(Component, Subject.StaticBackFaces.Get());
	}

	UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>(GetTransientPackage(), NAME_None, RF_Transient);
	Target->ClearColor = FLinearColor::Black;
	// 8-bit with the display gamma (not forced linear): the final LDR colour as the editor shows it.
	Target->InitCustomFormat(Size, Size, PF_B8G8R8A8, false);
	Target->UpdateResourceImmediate(true);

	USceneCaptureComponent2D* Camera = NewObject<USceneCaptureComponent2D>(GetTransientPackage(), NAME_None, RF_Transient);
	Camera->TextureTarget = Target;
	Camera->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
	Camera->bCaptureEveryFrame = false;
	Camera->bCaptureOnMovement = false;
	Camera->FOVAngle = FieldOfViewDegrees;
	// The thumbnail renderer's show flags (SkeletalMeshThumbnailRenderer): one frame with no
	// history, so no eye adaptation, temporal AA, Lumen or motion blur; fixed exposure.
	Camera->ShowFlags.DisableAdvancedFeatures();
	Camera->ShowFlags.SetMotionBlur(false);
	const FVector Location = FrameCamera(Frame.IsValid ? Frame : Subject.Frame, Subject.View, FieldOfViewDegrees, FrameMargin);
	Scene.AddComponent(Camera, FTransform(Subject.View, Location));

	// Everything the picture needs in before it is taken, or a material draws as the engine's grey world grid: loads,
	// asset compiles, every shader job and every mip (CaptureSettle). A material just made (a bake's) or one the editor
	// compiles only when it is first drawn asks for its shaders at the first capture, so warm-up captures run until one
	// asks for nothing more and every material drawn is ready (at most CaptureWarmUpRounds).
	TArray<UMaterialInterface*> Materials;
	for (const TPair<UPrimitiveComponent*, const UObject*>& Piece : Drawn)
	{
		TArray<UMaterialInterface*> Used;
		Piece.Key->GetUsedMaterials(Used);
		for (UMaterialInterface* Material : Used)
		{
			if (Material)
			{
				Materials.AddUnique(Material);
			}
		}
	}
	CaptureSettle(Drawn);
	int32 WarmUps = 0;
	for (; WarmUps < CaptureWarmUpRounds; ++WarmUps)
	{
		Camera->CaptureScene();
		FlushRenderingCommands();
		if (CaptureReady(Materials))
		{
			break;
		}
		CaptureSettle(Drawn);
	}
	const bool bReady = CaptureReady(Materials);
	for (const TPair<UPrimitiveComponent*, const UObject*>& Piece : Drawn)
	{
		UE_LOG(LogMantle, Log, TEXT("MANTLE_COSMETIC_THUMBNAIL draws %s"), *CaptureDescribe(*Piece.Key, Piece.Value));
	}
	if (bReady)
	{
		UE_LOG(LogMantle, Log, TEXT("MANTLE_COSMETIC_THUMBNAIL materials ready after %d warm-up capture(s)"), WarmUps + 1);
	}
	else
	{
		UE_LOG(LogMantle, Warning, TEXT("MANTLE_COSMETIC_THUMBNAIL some materials are still not ready after %d warm-up captures: they may draw as the grey world grid"),
			CaptureWarmUpRounds);
	}

	Camera->CaptureScene();
	FlushRenderingCommands();

	FTextureRenderTargetResource* Resource = Target->GameThread_GetRenderTargetResource();
	if (!Resource || !Resource->ReadPixels(OutPixels) || OutPixels.Num() != Size * Size)
	{
		OutPixels.Reset();
		OutError = TEXT("the render target could not be read back");
		return false;
	}
	// The capture's alpha is not coverage; the thumbnail is opaque like the renderer's.
	for (FColor& Pixel : OutPixels)
	{
		Pixel.A = 255;
	}
	return true;
}
