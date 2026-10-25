// Copyright Joshua Eger. All Rights Reserved.
#include "MantleMetaHumanConform.h"
#include "MantleMetaHumanBuild.h"
#include "MetaHumanRigEvaluatedState.h"
#include "MetaHumanCharacterIdentity.h"
#include "MetaHumanCharacterBodyIdentity.h"
#include "MantleAvatarHeight.h"
#include "MantleBodySilhouette.h"
#include "MantleMetaHumanHeadOnly.h"
#include "MantleAvatarContract.h"
#include "MantleAvatarStripFixer.h"
#include "MantleBodyTopologyData.h"
#include "MantleMetaHumanPose.h"
#include "MantleRuntime.h"

#include "AnimationRuntime.h"
#include "AssetCompilingManager.h"
#include "Camera/CameraTypes.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "ContentStreaming.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureCube.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "MetaHumanCharacter.h"
#include "MetaHumanCharacterEditorSubsystem.h"
#include "MetaHumanCharacterExportBlueprintLibrary.h"
#include "MetaHumanCharacterFactoryNew.h"
#include "MetaHumanConformTargetParams.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "PreviewScene.h"
#include "RenderingThread.h"
#include "RHIGlobals.h"
#include "ShaderCompiler.h"
#include "SkinnedAssetCompiler.h"
#include "TextureCompiler.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleMetaHumanConformPrivate
{
	/**
	 * The Masculine/Feminine measures (-2..2) of the two bodies a creator picks from. Feminine is where a whole-body
	 * conform lands; masculine mirrors it. MetaHuman's own body (0.3) is neither: it has a bust.
	 */
	constexpr float FeminineBodyMeasure = 1.6f;
	constexpr float MasculineBodyMeasure = -1.6f;

	/** The portrait stage: the posed mesh, three directional lights and a sky, and the two cameras. */
	struct FConformStage
	{
		FPreviewScene Scene;
		UStaticMeshComponent* Mesh = nullptr;
		USkyLightComponent* Sky = nullptr;
		TArray<UActorComponent*> Added;

		explicit FConformStage(UStaticMesh& Posed)
			: Scene(FPreviewScene::ConstructionValues().SetCreateDefaultLighting(false).SetCreatePhysicsScene(false).ShouldSimulatePhysics(false))
		{
			Mesh = NewObject<UStaticMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient);
			Mesh->SetStaticMesh(&Posed);
			// Its textures wanted at every mip (the streamer only streams what it is asked for).
			Mesh->bForceMipStreaming = true;
			Add(Mesh, FTransform::Identity);
			// Light rotations (pitch, yaw, roll) with the key at (-10, -90, 0); the body faces +Y, the cameras on +Y.
			struct FLight
			{
				FRotator Rotation;
				float Intensity;
				bool bShadows;
			};
			const FLight Lights[] = {
				{ FRotator(-10.0, -90.0, 0.0), 6.0f, true },
				{ FRotator(-15.0, -130.0, 0.0), 2.0f, false },
				{ FRotator(-25.0, 70.0, 0.0), 2.5f, false },
			};
			for (const FLight& Light : Lights)
			{
				UDirectionalLightComponent* Component = NewObject<UDirectionalLightComponent>(GetTransientPackage(), NAME_None, RF_Transient);
				Component->SetIntensity(Light.Intensity);
				Component->SetCastShadows(Light.bShadows);
				Add(Component, FTransform(Light.Rotation, FVector(0.0, 0.0, 500.0)));
			}
			Sky = NewObject<USkyLightComponent>(GetTransientPackage(), NAME_None, RF_Transient);
			Sky->SourceType = ESkyLightSourceType::SLS_SpecifiedCubemap;
			UTextureCube* Cubemap = LoadObject<UTextureCube>(nullptr, TEXT("/Engine/MapTemplates/Sky/SunsetAmbientCubemap.SunsetAmbientCubemap"));
			if (Cubemap)
			{
				// The cubemap compiled before the sky captures it (a capture of a compiling texture captures its placeholder).
				UTexture* const Compiling[] = { Cubemap };
				FTextureCompilingManager::Get().FinishCompilation(Compiling);
				Sky->Cubemap = Cubemap;
			}
			Sky->SetIntensity(1.0f);
			Add(Sky, FTransform(FVector(0.0, 0.0, 600.0)));
			// RecaptureSky only queues the capture; the scene's UpdateCaptureContents runs it. Settle captures it again.
			Sky->RecaptureSky();
			Scene.UpdateCaptureContents();
		}

		/**
		 * Shaders and assets finished, the mesh's textures forced fully resident and streamed in, the sky recaptured, the render
		 * thread caught up: what a capture needs to show the final look (as if r.TextureStreaming were 0).
		 */
		void Settle()
		{
			if (GShaderCompilingManager)
			{
				GShaderCompilingManager->FinishAllCompilation();
			}
			FAssetCompilingManager::Get().FinishAllCompilation();
			Mesh->PrestreamTextures(FMantleMetaHumanConform::PrestreamSeconds, /*bPrioritizeCharacterTextures*/ true);
			IStreamingManager::Get().StreamAllResources(FMantleMetaHumanConform::StreamSeconds);
			IStreamingManager::Get().BlockTillAllRequestsFinished(FMantleMetaHumanConform::StreamSeconds);
			Sky->RecaptureSky();
			Scene.UpdateCaptureContents();
			FlushRenderingCommands();
		}

		void Add(UActorComponent* Component, const FTransform& Transform)
		{
			Scene.AddComponent(Component, Transform);
			Added.Add(Component);
		}

		/** A camera at Location looking at Target (FOV 30, manual exposure at bias 0, the mesh alone). */
		USceneCaptureComponent2D* Camera(const bool bBaseColour, const FVector& Location, const FRotator& Rotation, UTextureRenderTarget2D*& OutTarget)
		{
			OutTarget = UKismetRenderingLibrary::CreateRenderTarget2D(Scene.GetWorld(), FMantleMetaHumanConform::PortraitSize, FMantleMetaHumanConform::PortraitSize,
				bBaseColour ? RTF_RGBA8_SRGB : RTF_RGBA8);
			USceneCaptureComponent2D* Capture = NewObject<USceneCaptureComponent2D>(GetTransientPackage(), NAME_None, RF_Transient);
			Capture->TextureTarget = OutTarget;
			Capture->CaptureSource = bBaseColour ? ESceneCaptureSource::SCS_BaseColor : ESceneCaptureSource::SCS_FinalColorLDR;
			Capture->FOVAngle = FMantleMetaHumanConform::PortraitFov;
			Capture->bCaptureEveryFrame = false;
			Capture->bCaptureOnMovement = false;
			Capture->PostProcessSettings.bOverride_AutoExposureMethod = true;
			Capture->PostProcessSettings.AutoExposureMethod = EAutoExposureMethod::AEM_Manual;
			Capture->PostProcessSettings.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
			Capture->PostProcessSettings.AutoExposureApplyPhysicalCameraExposure = false;
			Capture->PostProcessSettings.bOverride_AutoExposureBias = true;
			Capture->PostProcessSettings.AutoExposureBias = 0.0f;
			Capture->PostProcessBlendWeight = 1.0f;
			Capture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
			Capture->ShowOnlyComponents.Add(Mesh);
			Add(Capture, FTransform(Rotation, Location));
			return Capture;
		}

		~FConformStage()
		{
			for (UActorComponent* Component : Added)
			{
				Scene.RemoveComponent(Component);
			}
		}
	};

	/** The summed colour of about 4000 samples, as the settle step compares it. */
	double ConformColourSum(const TArray<FColor>& Pixels)
	{
		const int32 Step = FMath::Max(1, Pixels.Num() / 4000);
		double Sum = 0.0;
		for (int32 Index = 0; Index < Pixels.Num(); Index += Step)
		{
			Sum += Pixels[Index].R + Pixels[Index].G + Pixels[Index].B;
		}
		return Sum;
	}

	/** The portrait as read back: exported to File (a PNG), then read back from it. False with why. */
	bool ConformShoot(UWorld* World, USceneCaptureComponent2D& Capture, UTextureRenderTarget2D& Target, const FString& File, TArray<FColor>& OutPixels,
		FIntPoint& OutSize, FString& OutError)
	{
		Capture.CaptureScene();
		FlushRenderingCommands();
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), true);
		// A file left by an earlier run must not pass for this one.
		IFileManager::Get().Delete(*File, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
		UKismetRenderingLibrary::ExportRenderTarget(World, &Target, FPaths::GetPath(File), FPaths::GetCleanFilename(File));
		if (!IFileManager::Get().FileExists(*File))
		{
			OutError = FString::Printf(TEXT("it was not written to %s"), *File);
			return false;
		}
		FImage Image;
		if (!FImageUtils::LoadImage(*File, Image))
		{
			OutError = FString::Printf(TEXT("%s could not be read back"), *File);
			return false;
		}
		Image.ChangeFormat(ERawImageFormat::BGRA8, Image.GammaSpace);
		const TArrayView64<FColor> View = Image.AsBGRA8();
		OutPixels = TArray<FColor>(View.GetData(), static_cast<int32>(View.Num()));
		OutSize = FIntPoint(Image.SizeX, Image.SizeY);
		if (OutPixels.Num() == 0 || OutPixels.Num() != OutSize.X * OutSize.Y)
		{
			OutError = FString::Printf(TEXT("%s read back empty"), *File);
			return false;
		}
		return true;
	}

	/** An exported mesh by package name, as the export just made it (in memory only: never an older asset from disk). */
	USkeletalMesh* ConformFindExport(const FString& PackageName)
	{
		return FindObject<USkeletalMesh>(nullptr, *(PackageName + TEXT(".") + FPackageName::GetShortName(PackageName)));
	}

	/** Whether an asset PackageName exists, in memory or on disk. */
	bool ConformExists(const FString& PackageName)
	{
		return FindPackage(nullptr, *PackageName) != nullptr || FPackageName::DoesPackageExist(PackageName);
	}

	/**
	 * ExportGeometry of Character's A-pose head and body into Export.ProjectPath, read back by name
	 * (<character>_Body, <character>_Head) and recorded in Created in place of an earlier export's (InOutBody, InOutHead).
	 * The first export (no earlier one) stops if either name is already taken: the export overwrites, and a failed step
	 * deletes what it made, so it must never take over an asset that isn't its own.
	 */
	bool ConformExport(UMetaHumanCharacter& Character, const FMetaHumanGeometryExportParams& Export, TArray<TWeakObjectPtr<UObject>>& Created,
		USkeletalMesh*& InOutBody, USkeletalMesh*& InOutHead, FString& OutError)
	{
		const FString Prefix = Export.ProjectPath / Character.GetName();
		if (!InOutBody && !InOutHead && (ConformExists(Prefix + TEXT("_Body")) || ConformExists(Prefix + TEXT("_Head"))))
		{
			OutError = FString::Printf(TEXT("%s_Body or %s_Head already exists under %s; Convert never overwrites an asset it didn't make"), *Character.GetName(),
				*Character.GetName(), *Export.ProjectPath);
			return false;
		}
		const UObject* EarlierBody = InOutBody;
		const UObject* EarlierHead = InOutHead;
		Created.RemoveAll([EarlierBody, EarlierHead](const TWeakObjectPtr<UObject>& Made)
		{
			const UObject* Object = Made.Get();
			return !Object || Object == EarlierBody || Object == EarlierHead;
		});
		UMetaHumanCharacterExportBlueprintLibrary::ExportGeometry(&Character, Export);
		InOutBody = ConformFindExport(Prefix + TEXT("_Body"));
		InOutHead = ConformFindExport(Prefix + TEXT("_Head"));
		if (InOutBody)
		{
			Created.AddUnique(InOutBody);
		}
		if (InOutHead)
		{
			Created.AddUnique(InOutHead);
		}
		if (!InOutBody || !InOutHead)
		{
			OutError = FString::Printf(TEXT("MetaHuman's A-pose export gave no %s_Body and %s_Head under %s"), *Character.GetName(), *Character.GetName(),
				*Export.ProjectPath);
			return false;
		}
		return true;
	}

	/**
	 * Step 2, shared by the whole-body and the face-only conform: the front portrait of Mesh (base colour and lit) and
	 * Epic's face landmarks on it. Curves empty is "no face found"; false only when a portrait could not be taken.
	 */
	bool ConformPortraitLandmarks(UStaticMesh& Posed, TConstArrayView<FVector3f> Vertices, const TOptional<FVector>& HeadJoint, const FString& Name,
		UMetaHumanCharacterEditorSubsystem* Subsystem, FMantleMetaHumanConformResult& OutResult, TMap<FString, FTrackingPoints>& Curves,
		FIntPoint& ImageSize, FRotator& Look, FString& OutError)
	{
	if (!HeadJoint.IsSet())
	{
			UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_CONFORM %s has no Head chain: the portrait aims %.0f cm below the top"), *Name, FMantleMetaHumanConform::NoHeadDropCm);
		}
		OutResult.Framing = FMantleMetaHumanConform::FramePortrait(Vertices, HeadJoint);
		Look = FRotationMatrix::MakeFromX(OutResult.Framing.Target - OutResult.Framing.Camera).Rotator();
		ImageSize = FIntPoint::ZeroValue;
		{
			FConformStage Stage(Posed);
			UTextureRenderTarget2D* BaseTarget = nullptr;
			UTextureRenderTarget2D* LitTarget = nullptr;
			USceneCaptureComponent2D* Base = Stage.Camera(true, OutResult.Framing.Camera, Look, BaseTarget);
			USceneCaptureComponent2D* Lit = Stage.Camera(false, OutResult.Framing.Camera, Look, LitTarget);
			UWorld* World = Stage.Scene.GetWorld();
			// Settle: capture until two captures in a row agree (materials compiled, textures resident), at least three, at most 40.
			double Last = -1.0;
			for (int32 Try = 1; Try <= 40; ++Try)
			{
				Stage.Settle();
				Base->CaptureScene();
				FlushRenderingCommands();
				TArray<FColor> Pixels;
				UKismetRenderingLibrary::ReadRenderTarget(World, BaseTarget, Pixels, false);
				const double Sum = ConformColourSum(Pixels);
				const bool bAgrees = Last >= 0.0 && FMath::Abs(Sum - Last) <= 0.002 * FMath::Max(1.0, Sum);
				Last = Sum;
				if (Try >= 3 && bAgrees)
				{
					UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_CONFORM portrait settled after %d captures"), Try);
					break;
				}
			}
			struct FShot
			{
				const TCHAR* Label;
				USceneCaptureComponent2D* Capture;
				UTextureRenderTarget2D* Target;
			};
			for (const FShot& Shot : { FShot{ TEXT("basecolor"), Base, BaseTarget }, FShot{ TEXT("lit"), Lit, LitTarget } })
			{
				TArray<FColor> Pixels;
				FIntPoint Size;
				FString ShotError;
				const FString File = FMantleMetaHumanConform::PortraitFile(Name, Shot.Label);
				if (!ConformShoot(World, *Shot.Capture, *Shot.Target, File, Pixels, Size, ShotError))
				{
					// Not "no face": the portrait itself is missing, and the landmark tracker never saw it.
					UE_LOG(LogMantle, Warning, TEXT("MANTLE_CONVERT_CONFORM the %s portrait: %s"), Shot.Label, *ShotError);
					OutError = FString::Printf(TEXT("the face portrait (%s) could not be written or read back: %s"), Shot.Label, *ShotError);
					return false;
				}
				OutResult.PortraitFiles.Add(File);
				TMap<FString, FTrackingPoints> Found;
				const bool bFound = Subsystem->TrackFaceLandmarksFromImage(Pixels, Size.X, Size.Y, Found);
				int32 Points = 0;
				for (const TPair<FString, FTrackingPoints>& Curve : Found)
				{
					Points += Curve.Value.TrackingPoints.Num();
				}
				UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_CONFORM landmarks %s: %d curves, %d points"), Shot.Label, Found.Num(), Points);
				if (bFound && Found.Num() > 0 && Curves.Num() == 0)
				{
					Curves = MoveTemp(Found);
					ImageSize = Size;
					OutResult.LandmarkCurves = Curves.Num();
					OutResult.LandmarkPoints = Points;
					OutResult.LandmarkPortrait = Shot.Label;
				}
			}
		}
		return true;
	}

	/**
	 * The conform fixer's Apply for a face-only part: the clean head of the source (into <output>/Work), conformed head-only
	 * at the default height into <output>/<Name>. The same contract as the whole-body Apply: nothing protected changes, and a
	 * failed Apply deletes what it made.
	 */
	bool ApplyFaceOnly(const FMantleConvertFix& Fix, FMantleConvertContext& Context, FMantleConvertMetaHumanWork& Work, FString& OutError)
	{
		FMantleConvertPart& Part = Context.Parts[Fix.PartIndex];
		USkeletalMesh* Source = Part.Original.IsValid() ? Part.Original.Get() : Part.Mesh.Get();
		// A model with no skeleton (a static mesh, or a skeletal mesh with no usable weights) has its head cut by height, not by bone weights.
		UStaticMesh* StaticSource = Part.bUnrigged ? Part.StaticSource.Get() : nullptr;
		if (!Source && !StaticSource)
		{
			OutError = TEXT("the character's mesh is gone");
			return false;
		}
		const FString Name = FMantleMetaHumanSteps::AssetTag(Part.Name);
		const FString CharacterFolder = Context.OutputFolder / Name;
		const FString WorkFolder = Context.OutputFolder / TEXT("Work");
		const TArray<FString> Roots = FMantleMetaHumanSteps::ProtectedRoots(Context, Fix.PartIndex);
		const TSet<FString> DirtyBefore = FMantleMetaHumanSteps::DirtyUnder(Roots, Context.OutputFolder);
		TArray<TWeakObjectPtr<UObject>> Made;
		FMantleCleanHead Clean;
		const double TargetCm = FMantleAvatarHeight::TargetCm();
		bool bDone = Part.bUnrigged
			? FMantleMetaHumanHeadOnly::MakeCleanHeadUnrigged(StaticSource, StaticSource ? nullptr : Source, TargetCm, WorkFolder, Name, Clean, Made, OutError)
			: FMantleMetaHumanHeadOnly::MakeCleanHead(*Source, TargetCm, WorkFolder, Name, Clean, Made, OutError);
		FMantleMetaHumanConformResult Result;
		if (bDone)
		{
			// The source's build seeds the body sliders; a build that can't be read leaves them at the default (said in the log).
			FMantleSilhouette Build;
			FString BuildError;
			if (!FMantleMetaHumanHeadOnly::MeasureBuild(StaticSource ? static_cast<UObject&>(*StaticSource) : static_cast<UObject&>(*Source), Clean.SourceHeightCm, Build, BuildError))
			{
				UE_LOG(LogMantle, Warning, TEXT("MANTLE_CONVERT_FACEONLY %s: its build was not read (%s); the body sliders keep their defaults"), *Name, *BuildError);
				Context.Notes.Add(FString::Printf(TEXT("%s: its build couldn't be seen (%s), so the body sliders start at MetaHuman's default."), *Part.Name, *BuildError));
			}
			bDone = FMantleMetaHumanConform::ConformHeadOnly(*Clean.Mesh.Get(), Clean.HeadJoint, TargetCm, Work.bFeminineBody, CharacterFolder, Name,
				Result, OutError, &Build);
			Made.Append(Result.Created);
		}
		FString Dirtied;
		if (!FMantleMetaHumanSteps::CheckUnchanged(Roots, Context.OutputFolder, DirtyBefore, TEXT("The face conform"), Dirtied))
		{
			OutError = bDone ? Dirtied : FString::Printf(TEXT("%s. %s"), *OutError, *Dirtied);
			bDone = false;
		}
		if (!bDone)
		{
			FMantleMetaHumanSteps::Discard(Made);
			return false;
		}
		for (const TWeakObjectPtr<UObject>& Created : Made)
		{
			Context.CreatedAssets.AddUnique(Created);
		}
		Work.Character = Result.Character;
		Work.CharacterFolder = CharacterFolder;
		Context.Notes.Add(FString::Printf(TEXT("%s: kept its face on MetaHuman's own body (%s; face landmarks %d curves, %d points)."), *Part.Name,
			*Clean.Describe(), Result.LandmarkCurves, Result.LandmarkPoints));
		return true;
	}
}

FMantlePortraitFraming FMantleMetaHumanConform::FramePortrait(TConstArrayView<FVector3f> Vertices, const TOptional<FVector>& HeadJoint)
{
	FMantlePortraitFraming Out;
	// Without a head joint (no Head chain), the fallback is the joint's height NoHeadDropCm below the top.
	double Top = -TNumericLimits<double>::Max();
	for (const FVector3f& Vertex : Vertices)
	{
		Top = FMath::Max(Top, static_cast<double>(Vertex.Z));
	}
	const double HeadZ = HeadJoint.IsSet() ? HeadJoint->Z : Top - NoHeadDropCm;
	FBox3d Region(ForceInit);
	for (const FVector3f& Vertex : Vertices)
	{
		if (Vertex.Z > HeadZ - 6.0)
		{
			Region += FVector3d(Vertex);
		}
	}
	if (!Region.IsValid)
	{
		const FVector Aim = HeadJoint.Get(FVector::ZeroVector);
		Out.Target = Aim;
		Out.FaceFrontY = Aim.Y;
		Out.Camera = Aim + FVector(0.0, StandoffCm, 0.0);
		return Out;
	}
	// The face's front: the front-most head vertex in the central band (ears and shoulders out). The camera stands beyond
	// THAT, not beyond the head joint: a muzzle or a long face can reach far past the joint.
	const double MiddleX = (Region.Min.X + Region.Max.X) / 2.0;
	const double Half = 0.35 * (Region.Max.X - Region.Min.X);
	double Front = -TNumericLimits<double>::Max();
	for (const FVector3f& Vertex : Vertices)
	{
		if (Vertex.Z > HeadZ - 6.0 && FMath::Abs(Vertex.X - MiddleX) < Half)
		{
			Front = FMath::Max(Front, static_cast<double>(Vertex.Y));
		}
	}
	if (Front == -TNumericLimits<double>::Max())
	{
		Front = Region.Max.Y;
	}
	// Size-aware standoff: a head taller than a human's (joint to top) moves the camera back in proportion.
	Out.Size = FMath::Max(1.0, (Region.Max.Z - HeadZ) / HeadRefCm);
	Out.FaceFrontY = Front;
	const double HeadY = HeadJoint.IsSet() ? HeadJoint->Y : (Region.Min.Y + Region.Max.Y) / 2.0;
	Out.Target = FVector(MiddleX, HeadY, (Region.Min.Z + Region.Max.Z) / 2.0 - 1.0);
	Out.Camera = FVector(Out.Target.X, Front + StandoffCm * Out.Size, Out.Target.Z);
	return Out;
}

FString FMantleMetaHumanConform::PortraitFile(const FString& Name, const TCHAR* Label)
{
	const FString Tag = FMantleMetaHumanSteps::AssetTag(Name);
	return FPaths::ProjectSavedDir() / TEXT("MantleConvert") / Tag / TEXT("landmarks") / FString::Printf(TEXT("%s_portrait_%s.png"), *Tag, Label);
}

bool FMantleMetaHumanConform::Conform(UStaticMesh& Posed, const TOptional<FVector>& HeadJoint, const FString& CharacterFolder, const FString& InName,
	FMantleMetaHumanConformResult& OutResult, FString& OutError)
{
	using namespace MantleMetaHumanConformPrivate;
	OutResult = FMantleMetaHumanConformResult();
	const FString Name = FMantleMetaHumanSteps::AssetTag(InName);
	UMetaHumanCharacterEditorSubsystem* Subsystem = UMetaHumanCharacterEditorSubsystem::Get();
	if (!Subsystem)
	{
		OutError = TEXT("MetaHuman Creator's editor subsystem is not running: enable the MetaHuman Creator plugin");
		return false;
	}
	if (GUsingNullRHI)
	{
		OutError = TEXT("the face portrait needs a renderer (this editor runs without one)");
		return false;
	}
	if (!FSlateApplication::IsInitialized())
	{
		OutError = TEXT("MetaHuman's geometry export needs the editor's UI, and this editor runs without it");
		return false;
	}

	// 1. The input check: what the conform reads must be what renders.
	TArray<FVector3f> Vertices;
	TArray<int32> Indices;
	if (!FMantleAvatarStrip::InputCheck(&Posed, OutResult.InputLine, &Vertices, &Indices))
	{
		OutError = FString::Printf(TEXT("Epic's conform would not read the posed body as it renders (%s); recorded, not patched"), *OutResult.InputLine);
		return false;
	}

	// 2. The front portrait and its landmarks (every texture streamed in while it is taken).
	TMap<FString, FTrackingPoints> Curves;
	FIntPoint ImageSize = FIntPoint::ZeroValue;
	FRotator Look;
	if (!ConformPortraitLandmarks(Posed, Vertices, HeadJoint, Name, Subsystem, OutResult, Curves, ImageSize, Look, OutError))
	{
		return false;
	}
	if (Curves.Num() == 0)
	{
		OutError = TEXT("Couldn't find a face on the front of this character. Convert needs a human-like face looking forward (+Y)");
		return false;
	}

	// 3. The MetaHuman Character, opened for editing.
	UMetaHumanCharacterFactoryNew* Factory = NewObject<UMetaHumanCharacterFactoryNew>(GetTransientPackage());
	UMetaHumanCharacter* Character = Cast<UMetaHumanCharacter>(FMantleMetaHumanSteps::CreateAsset(CharacterFolder, Name, UMetaHumanCharacter::StaticClass(), Factory));
	if (!Character)
	{
		OutError = TEXT("the MetaHuman Character could not be made");
		return false;
	}
	OutResult.Created.Add(Character);
	OutResult.Character = Character;
	if (!Subsystem->TryAddObjectToEdit(Character))
	{
		OutError = TEXT("MetaHuman Creator would not open the new character for editing");
		return false;
	}
	ON_SCOPE_EXIT
	{
		if (Subsystem->IsObjectAddedForEditing(Character))
		{
			Subsystem->RemoveObjectToEdit(Character);
		}
	};

	// 4. The conform: combined, joints from the mesh, auto-solve, the landmarks and the portrait's camera.
	FMetaHumanCharacterTargetMeshKey Key;
	Key.CombinedMesh = &Posed;
	FConformTargetParams Params;
	Params.ConformTargetMesh.TargetPartsType = ETargetPartsType::Combined;
	Params.ConformTargetMesh.BodyVertices = Vertices;
	Params.ConformTargetMesh.BodyVertexIndices = Indices;
	Params.bEstimateBodyJointsFromMesh = true;
	Params.bAutoSolve = true;
	Params.BodyConformSolveSettings.PipelineName = TEXT("combined");
	Params.CurveTrackingPoints = Curves;
	FMinimalViewInfo View;
	View.Location = OutResult.Framing.Camera;
	View.Rotation = Look;
	View.FOV = PortraitFov;
	View.AspectRatio = static_cast<float>(ImageSize.X) / static_cast<float>(FMath::Max(1, ImageSize.Y));
	View.ProjectionMode = ECameraProjectionMode::Perspective;
	Params.CameraViewInfo = View;
	Params.ImageSize = ImageSize;
	const double Started = FPlatformTime::Seconds();
	if (!Subsystem->ConformToTargetMeshes(Character, Key, Params))
	{
		OutError = TEXT("MetaHuman's conform did not solve for this body");
		return false;
	}
	UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_CONFORM conform_to_target_meshes: solved in %.1f s"), FPlatformTime::Seconds() - Started);

	// 5. Committed to the A-pose; Epic's A-pose head and body exported into the character's own folder (the hand re-estimate's template).
	Subsystem->CommitPosedStateAsAPose(Character, Key);
	FMetaHumanGeometryExportParams Export;
	Export.ProjectPath = CharacterFolder / TEXT("APose");
	Export.bHeadSkeletalMesh = true;
	Export.bBodySkeletalMesh = true;
	Export.bFullBodySkeletalMesh = false;
	Export.bOverwriteExistingAssets = true;
	USkeletalMesh* ExportBody = nullptr;
	USkeletalMesh* ExportHead = nullptr;
	if (!ConformExport(*Character, Export, OutResult.Created, ExportBody, ExportHead, OutError))
	{
		return false;
	}

	// 6. The hand re-estimate: the hand and finger joints re-estimated on the committed A-pose skin (the skin does not move).
	TArray<FVector3f> TemplateVertices;
	const EImportErrorCode Read = Subsystem->GetMeshForBodyConformingFromTemplate(Character, ExportBody, ExportHead, /*bInMatchVerticesByUVs*/ false, TemplateVertices);
	OutResult.JointReestimate = FString::Printf(TEXT("template %d, %d vertices"), static_cast<int32>(Read), TemplateVertices.Num());
	if (Read != EImportErrorCode::Success || TemplateVertices.Num() == 0)
	{
		OutError = FString::Printf(TEXT("MetaHuman could not read its own A-pose export back for the hand joints (%s)"), *OutResult.JointReestimate);
		return false;
	}
	if (!Subsystem->ConformBodyToTarget(Character, TemplateVertices, TArray<FVector3f>(), /*bTargetIsInAPose*/ true, /*bEstimateJointsFromMesh*/ true))
	{
		OutError = TEXT("MetaHuman's hand joint re-estimate (conform_body_to_target) failed");
		return false;
	}
	if (!Subsystem->FitFaceStateFromBodyWithEyesTeethTemplate(Character, nullptr, nullptr, nullptr, false))
	{
		OutError = TEXT("MetaHuman's face refit after the hand joint re-estimate failed");
		return false;
	}
	// The A-pose exported again (the last export): the exports now carry the re-estimated joints.
	if (!ConformExport(*Character, Export, OutResult.Created, ExportBody, ExportHead, OutError))
	{
		return false;
	}
	Character->MarkPackageDirty();
	OutResult.APoseBody = ExportBody;
	OutResult.APoseHead = ExportHead;
	// The guide's hand numbers on the final export, for the record (the bars are judged after the build).
	TArray<USkinnedAsset*> Compiling = { ExportBody };
	FSkinnedAssetCompilingManager::Get().FinishCompilation(Compiling);
	if (!MeasureHands(*ExportBody, OutResult.Hands))
	{
		UE_LOG(LogMantle, Warning, TEXT("MANTLE_CONVERT_CONFORM the hands of %s could not be measured"), *ExportBody->GetPathName());
	}
	UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_CONFORM %s: conformed (%s; landmarks %d curves, %d points from the %s portrait); hands re-estimated (%s; %s); A-pose exported to %s"),
		*Character->GetPathName(), *OutResult.InputLine, OutResult.LandmarkCurves, OutResult.LandmarkPoints, *OutResult.LandmarkPortrait, *OutResult.JointReestimate,
		*OutResult.Hands.ToString(), *Export.ProjectPath);
	return true;
}

bool FMantleMetaHumanConform::ChooseParametricBody(UObject& InCharacter, const bool bFeminine, FString& OutError)
{
	using namespace MantleMetaHumanConformPrivate;
	UMetaHumanCharacterEditorSubsystem* Subsystem = UMetaHumanCharacterEditorSubsystem::Get();
	UMetaHumanCharacter* Character = Cast<UMetaHumanCharacter>(&InCharacter);
	if (!Subsystem || !Character)
	{
		OutError = TEXT("MetaHuman Creator's editor subsystem is not running, or the object is not a MetaHuman Character");
		return false;
	}
	bool bFound = false;
	TArray<FMetaHumanCharacterBodyConstraint> Constraints = Subsystem->GetBodyConstraints(Character);
	for (FMetaHumanCharacterBodyConstraint& Constraint : Constraints)
	{
		if (Constraint.Name == FName(TEXT("Masculine/Feminine")))
		{
			bFound = true;
			Constraint.bIsActive = true;
			Constraint.TargetMeasurement = FMath::Clamp(bFeminine ? FeminineBodyMeasure : MasculineBodyMeasure, Constraint.MinMeasurement,
				Constraint.MaxMeasurement);
		}
	}
	Subsystem->SetBodyConstraints(Character, Constraints);
	Subsystem->CommitBodyState(Character);
	if (!bFound)
	{
		OutError = TEXT("the body has no Masculine/Feminine constraint to set");
	}
	return bFound;
}

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleMetaHumanConformSeedPrivate
{
	/**
	 * #50: the Weight, Fat and Muscularity sliders of Character (open for editing, its body chosen) moved from the default body
	 * towards SourceBuild: the default's own silhouette (evaluated body, Y up and Z forward in MetaHuman space) is the reference.
	 */
	void SeedBodySliders(UMetaHumanCharacterEditorSubsystem& Subsystem, UMetaHumanCharacter& Character, const FMantleSilhouette& SourceBuild)
	{
		const TSharedRef<const FMetaHumanCharacterBodyIdentity::FState> State = Subsystem.GetBodyState(&Character);
		const TArray<int32> LODCounts = State->GetNumVerticesPerLOD();
		const FMetaHumanRigEvaluatedState Geometry = State->GetVerticesAndVertexNormals();
		if (LODCounts.IsEmpty() || LODCounts[0] <= 0 || LODCounts[0] > Geometry.Vertices.Num())
		{
			UE_LOG(LogMantle, Warning, TEXT("MANTLE_CONVERT_FACEONLY %s: the default body has no LOD0 vertices, so the sliders keep their defaults"), *Character.GetPathName());
			return;
		}
		// To the source's axes (Z up, Y front to back).
		TArray<FVector3f> Vertices;
		FBox3f Bounds(ForceInit);
		for (int32 Index = 0; Index < LODCounts[0]; ++Index)
		{
			const FVector3f& Vertex = Geometry.Vertices[Index];
			Vertices.Add(FVector3f(Vertex.X, Vertex.Z, Vertex.Y));
			Bounds += Vertex;
		}
		const double Height = Bounds.GetSize().Y;
		FMantleSilhouette Default;
		if (!FMantleBodySilhouette::Measure(Vertices, Bounds.Min.Y, Height, Default))
		{
			UE_LOG(LogMantle, Warning, TEXT("MANTLE_CONVERT_FACEONLY %s: the default body's silhouette could not be measured, so the sliders keep their defaults"), *Character.GetPathName());
			return;
		}
		const FMantleSliderSeed Seed = FMantleBodySilhouette::Seed(SourceBuild, FMantleBodySilhouette::Normalised(Default, Height));
		TArray<FMetaHumanCharacterBodyConstraint> Constraints = Subsystem.GetBodyConstraints(&Character);
		FString Moved;
		for (FMetaHumanCharacterBodyConstraint& Constraint : Constraints)
		{
			const FString Name = Constraint.Name.ToString();
			double Offset = 0.0;
			if (Name == TEXT("Weight"))
			{
				Offset = Seed.Weight;
			}
			else if (Name == TEXT("Fat"))
			{
				Offset = Seed.Fat;
			}
			else if (Name.Contains(TEXT("Muscul")))
			{
				Offset = Seed.Muscularity;
			}
			else
			{
				continue;
			}
			const float From = Constraint.TargetMeasurement;
			Constraint.bIsActive = true;
			Constraint.TargetMeasurement = FMath::Clamp(static_cast<float>(From + Offset * (Constraint.MaxMeasurement - Constraint.MinMeasurement)),
				Constraint.MinMeasurement, FMath::Max(Constraint.MinMeasurement, Constraint.MaxMeasurement));
			Moved += FString::Printf(TEXT(" %s %.2f -> %.2f (range %.2f to %.2f);"), *Name, From, Constraint.TargetMeasurement, Constraint.MinMeasurement,
				Constraint.MaxMeasurement);
		}
		Subsystem.SetBodyConstraints(&Character, Constraints);
		Subsystem.CommitBodyState(&Character);
		UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_FACEONLY %s: sliders seeded from the silhouette (source %s; default %s; %s):%s"), *Character.GetPathName(),
			*SourceBuild.ToString(true), *FMantleBodySilhouette::Normalised(Default, Height).ToString(true), *Seed.ToString(), *Moved);
	}
}

bool FMantleMetaHumanConform::ConformHeadOnly(UStaticMesh& Head, const TOptional<FVector>& HeadJoint, const double HeightCm, const bool bFeminine,
	const FString& CharacterFolder,
	const FString& InName, FMantleMetaHumanConformResult& OutResult, FString& OutError, const FMantleSilhouette* SourceBuild)
{
	using namespace MantleMetaHumanConformPrivate;
	OutResult = FMantleMetaHumanConformResult();
	const FString Name = FMantleMetaHumanSteps::AssetTag(InName);
	UMetaHumanCharacterEditorSubsystem* Subsystem = UMetaHumanCharacterEditorSubsystem::Get();
	if (!Subsystem)
	{
		OutError = TEXT("MetaHuman Creator's editor subsystem is not running: enable the MetaHuman Creator plugin");
		return false;
	}
	if (GUsingNullRHI)
	{
		OutError = TEXT("the face portrait needs a renderer (this editor runs without one)");
		return false;
	}

	// 1. What the conform reads of the head.
	TArray<FVector3f> Vertices;
	TArray<int32> Indices;
	if (!FMantleAvatarStrip::InputCheck(&Head, OutResult.InputLine, &Vertices, &Indices))
	{
		OutError = FString::Printf(TEXT("Epic's conform would not read the clean head as it renders (%s); recorded, not patched"), *OutResult.InputLine);
		return false;
	}

	// 2. The front portrait of the head alone, and its landmarks.
	TMap<FString, FTrackingPoints> Curves;
	FIntPoint ImageSize = FIntPoint::ZeroValue;
	FRotator Look;
	if (!ConformPortraitLandmarks(Head, Vertices, HeadJoint, Name, Subsystem, OutResult, Curves, ImageSize, Look, OutError))
	{
		return false;
	}
	if (Curves.Num() == 0)
	{
		OutError = TEXT("We couldn't see a face on this character (a mask or a helmet may cover it), so it can't keep its face");
		return false;
	}

	// 3. The MetaHuman Character, opened for editing, at the height the head was sized for (the neck then joins at the right height).
	UMetaHumanCharacterFactoryNew* Factory = NewObject<UMetaHumanCharacterFactoryNew>(GetTransientPackage());
	UMetaHumanCharacter* Character = Cast<UMetaHumanCharacter>(FMantleMetaHumanSteps::CreateAsset(CharacterFolder, Name, UMetaHumanCharacter::StaticClass(), Factory));
	if (!Character)
	{
		OutError = TEXT("the MetaHuman Character could not be made");
		return false;
	}
	OutResult.Created.Add(Character);
	OutResult.Character = Character;
	if (!Subsystem->TryAddObjectToEdit(Character))
	{
		OutError = TEXT("MetaHuman Creator would not open the new character for editing");
		return false;
	}
	ON_SCOPE_EXIT
	{
		if (Subsystem->IsObjectAddedForEditing(Character))
		{
			Subsystem->RemoveObjectToEdit(Character);
		}
	};
	bool bFixed = false;
	if (!FMantleMetaHumanBuild::SetHeight(*Character, HeightCm, bFixed, OutError))
	{
		OutError = FString::Printf(TEXT("setting the body's height before the face conform: %s"), *OutError);
		return false;
	}
	// The head-only solve fits MetaHuman's whole parametric model to the head, and leaves its body sized to that head and
	// standing off the ground (Terra: 132 cm, feet 31.7 cm up). The body chosen here (MetaHuman's own at the default height,
	// feet on the origin) is put back after it; committing it refits the face, which keeps its conformed shape, onto its neck.
	const TSharedRef<const FMetaHumanCharacterBodyIdentity::FState> ChosenBody = Subsystem->CopyBodyState(Character);

	// 4. The conform: the head only, MetaHuman's head_only pipeline, the landmarks and the portrait's camera. The body stays
	// MetaHuman's own, so its neck and shoulders are never pulled onto a collar or a cut edge.
	FMetaHumanCharacterTargetMeshKey Key;
	Key.HeadMesh = &Head;
	FConformTargetParams Params;
	Params.ConformTargetMesh.TargetPartsType = ETargetPartsType::HeadOnly;
	Params.ConformTargetMesh.HeadVertices = Vertices;
	Params.ConformTargetMesh.HeadVertexIndices = Indices;
	Params.bEstimateBodyJointsFromMesh = false;
	Params.bAutoSolve = true;
	Params.BodyConformSolveSettings.PipelineName = TEXT("head_only");
	Params.CurveTrackingPoints = Curves;
	FMinimalViewInfo View;
	View.Location = OutResult.Framing.Camera;
	View.Rotation = Look;
	View.FOV = PortraitFov;
	View.AspectRatio = static_cast<float>(ImageSize.X) / static_cast<float>(FMath::Max(1, ImageSize.Y));
	View.ProjectionMode = ECameraProjectionMode::Perspective;
	Params.CameraViewInfo = View;
	Params.ImageSize = ImageSize;
	const double Started = FPlatformTime::Seconds();
	if (!Subsystem->ConformToTargetMeshes(Character, Key, Params))
	{
		OutError = TEXT("MetaHuman's face conform did not solve for this head");
		return false;
	}
	Subsystem->CommitBodyState(Character, ChosenBody);
	// The body the creator picked, on MetaHuman's Masculine/Feminine scale.
	{
		FString ChooseError;
		if (!ChooseParametricBody(*Character, bFeminine, ChooseError))
		{
			UE_LOG(LogMantle, Warning, TEXT("MANTLE_CONVERT_FACEONLY %s: %s"), *Character->GetPathName(), *ChooseError);
		}
	}
	// The character's own build: the sliders start where its torso is, not at MetaHuman's default.
	if (SourceBuild && SourceBuild->bValid)
	{
		MantleMetaHumanConformSeedPrivate::SeedBodySliders(*Subsystem, *Character, *SourceBuild);
	}
	Character->MarkPackageDirty();
	UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_FACEONLY %s: head-only conform solved in %.1f s (%s; landmarks %d curves, %d points from the %s portrait); body at %.1f cm"),
		*Character->GetPathName(), FPlatformTime::Seconds() - Started, *OutResult.InputLine, OutResult.LandmarkCurves, OutResult.LandmarkPoints,
		*OutResult.LandmarkPortrait, HeightCm);
	return true;
}

FString FMantleHandMeasure::ToString() const
{
	return FString::Printf(TEXT("worst hand/finger joint %s %.2f cm; hands %.2f / %.2f cm; elbows %.1f / %.1f degrees"), *WorstJoint.ToString(), WorstJointCm,
		HandCm[0], HandCm[1], ElbowDegrees[0], ElbowDegrees[1]);
}

bool FMantleMetaHumanConform::MeasureHands(const USkeletalMesh& Body, FMantleHandMeasure& Out)
{
	Out = FMantleHandMeasure();
	TArray<FVector3f> Skin;
	FMantleBodyLODTopology Topology;
	if (!UMantleBodyTopologyData::ReadLOD(&Body, 0, Topology, &Skin) || Skin.Num() == 0)
	{
		return false;
	}
	auto ToSkin = [&Skin](const FVector& Position)
	{
		double Nearest = TNumericLimits<double>::Max();
		for (const FVector3f& Vertex : Skin)
		{
			Nearest = FMath::Min(Nearest, FVector::DistSquared(Position, FVector(Vertex)));
		}
		return FMath::Sqrt(Nearest);
	};
	// Epic's MetaHuman skeleton: its bone names are Epic's own, fixed (never the source's).
	const FReferenceSkeleton& Reference = Body.GetRefSkeleton();
	auto Joint = [&Reference](const int32 Bone) { return FAnimationRuntime::GetComponentSpaceTransformRefPose(Reference, Bone).GetLocation(); };
	const TCHAR* const Sides[] = { TEXT("l"), TEXT("r") };
	bool bAny = false;
	for (int32 Side = 0; Side < 2; ++Side)
	{
		const int32 Hand = Reference.FindBoneIndex(FName(*FString::Printf(TEXT("hand_%s"), Sides[Side])));
		if (Hand == INDEX_NONE)
		{
			continue;
		}
		bAny = true;
		// The hand and every bone below it (bones come after their parents).
		TSet<int32> Below = { Hand };
		for (int32 Bone = Hand; Bone < Reference.GetNum(); ++Bone)
		{
			if (Bone != Hand && !Below.Contains(Reference.GetParentIndex(Bone)))
			{
				continue;
			}
			Below.Add(Bone);
			const double Distance = ToSkin(Joint(Bone));
			if (Bone == Hand)
			{
				Out.HandCm[Side] = Distance;
			}
			if (Distance > Out.WorstJointCm)
			{
				Out.WorstJointCm = Distance;
				Out.WorstJoint = Reference.GetBoneName(Bone);
			}
		}
		const int32 Upper = Reference.FindBoneIndex(FName(*FString::Printf(TEXT("upperarm_%s"), Sides[Side])));
		const int32 Lower = Reference.FindBoneIndex(FName(*FString::Printf(TEXT("lowerarm_%s"), Sides[Side])));
		if (Upper != INDEX_NONE && Lower != INDEX_NONE)
		{
			const FVector UpperArm = (Joint(Lower) - Joint(Upper)).GetSafeNormal();
			const FVector Forearm = (Joint(Hand) - Joint(Lower)).GetSafeNormal();
			Out.ElbowDegrees[Side] = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(UpperArm, Forearm), -1.0, 1.0)));
		}
	}
	return bAny;
}

void FMantleMetaHumanConformFixer::Analyze(const FMantleConvertContext& Context, TArray<FMantleConvertFix>& OutFixes) const
{
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get();
	if (!Contract)
	{
		return;
	}
	for (int32 PartIndex = 0; PartIndex < Context.Parts.Num(); ++PartIndex)
	{
		if (!FMantleMetaHumanPoseFixer::IsBodyToConvert(Context, PartIndex, *Contract))
		{
			continue;
		}
		FMantleConvertFix& Fix = OutFixes.AddDefaulted_GetRef();
		Fix.FixerId = GetId();
		Fix.PartIndex = PartIndex;
		Fix.Problem = TEXT("it isn't a MetaHuman yet");
		if (GUsingNullRHI)
		{
			Fix.Action = TEXT("Can't: the face portrait needs a renderer, and this editor runs without one.");
			Fix.bCanFix = false;
			Fix.bEnabled = false;
			continue;
		}
		if (!FSlateApplication::IsInitialized())
		{
			Fix.Action = TEXT("Can't: MetaHuman's geometry export needs the editor's UI, and this editor runs without it.");
			Fix.bCanFix = false;
			Fix.bEnabled = false;
			continue;
		}
		const FMantleConvertMetaHumanWork* FaceWork = Context.MetaHuman.Find(PartIndex);
		Fix.Action = FaceWork && FaceWork->bFaceOnly
			? TEXT("Makes a MetaHuman Character with this character's face (Epic's head-only conform, guided by face landmarks from a front portrait of the clean head) on MetaHuman's own body at Mantle's default height. It stays editable in MetaHuman Creator.")
			: TEXT("Makes a MetaHuman Character of its body and face shape (Epic's conform, guided by face landmarks from a front portrait), in its A-pose, with its hand joints placed on its hands. It stays editable in MetaHuman Creator.");
	}
}

bool FMantleMetaHumanConformFixer::Apply(const FMantleConvertFix& Fix, FMantleConvertContext& Context, FString& OutError) const
{
	if (Fix.FixerId != GetId() || !Context.Parts.IsValidIndex(Fix.PartIndex))
	{
		OutError = TEXT("the conform names no part");
		return false;
	}
	FMantleConvertMetaHumanWork* Work = Context.MetaHuman.Find(Fix.PartIndex);
	if (Work && Work->bFaceOnly)
	{
		return MantleMetaHumanConformPrivate::ApplyFaceOnly(Fix, Context, *Work, OutError);
	}
	UStaticMesh* Posed = Work ? Work->PosedMesh.Get() : nullptr;
	if (!Posed)
	{
		OutError = TEXT("the body was not posed into MetaHuman's A-pose first (the A-pose step)");
		return false;
	}
	FMantleConvertPart& Part = Context.Parts[Fix.PartIndex];
	const FString Name = FMantleMetaHumanSteps::AssetTag(Part.Name);
	const FString CharacterFolder = Context.OutputFolder / Name;
	// Nothing of Epic's, MetaHuman Creator's or the source's may change; the step writes only into <output>/<Name>.
	const TArray<FString> Roots = FMantleMetaHumanSteps::ProtectedRoots(Context, Fix.PartIndex);
	const TSet<FString> DirtyBefore = FMantleMetaHumanSteps::DirtyUnder(Roots, CharacterFolder);
	FMantleMetaHumanConformResult Result;
	// The head-joint hint scaled by the A-pose step's default-height k, as the posed mesh was.
	bool bConformed = FMantleMetaHumanConform::Conform(*Posed, Work->ConformHeadHint(), CharacterFolder, Name, Result, OutError);
	// Checked whether the step finished or not: one that stopped half-way may have changed something too.
	FString Dirtied;
	if (!FMantleMetaHumanSteps::CheckUnchanged(Roots, CharacterFolder, DirtyBefore, TEXT("The conform"), Dirtied))
	{
		OutError = bConformed ? Dirtied : FString::Printf(TEXT("%s. %s"), *OutError, *Dirtied);
		bConformed = false;
	}
	if (!bConformed)
	{
		// The fixer contract: a failed Apply leaves nothing behind and the context as it was.
		FMantleMetaHumanSteps::Discard(Result.Created);
		return false;
	}
	for (const TWeakObjectPtr<UObject>& Created : Result.Created)
	{
		Context.CreatedAssets.AddUnique(Created);
	}
	Work->Character = Result.Character;
	Work->CharacterFolder = CharacterFolder;
	const UObject* Character = Result.Character.Get();
	Context.Notes.Add(FString::Printf(TEXT("%s: a MetaHuman Character, %s (face landmarks: %d curves, %d points; %s)."), *Part.Name,
		Character ? *Character->GetName() : TEXT("?"), Result.LandmarkCurves, Result.LandmarkPoints, *Result.InputLine));
	return true;
}

void RegisterMantleMetaHumanConformFixer()
{
	FMantleAvatarFixers::Register(MakeShared<FMantleMetaHumanConformFixer>());
}
