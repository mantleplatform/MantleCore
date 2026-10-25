// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceThumbnail.h"
#include "MantlePlaceSettingsActor.h"
#include "MantleRuntime.h"

#include "AssetCompilingManager.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "ContentStreaming.h"
#include "Engine/Texture.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Info.h"
#include "GameFramework/PlayerStart.h"
#include "Misc/App.h"
#include "RHIGlobals.h"
#include "RenderingThread.h"
#include "SceneInterface.h"
#include "ShaderCompiler.h"
#include "TextureResource.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantlePlaceThumbnailPrivate
{
	/** Primitives bigger than this (half-extent, cm) are skies and backdrops, not what a player walks through. */
	constexpr double MaxFramedHalfExtentCm = 500000.0;

	/**
	 * A ground: wider than this (half-extent across, cm) and much flatter than it is wide. Framing what stands on it
	 * rather than the floor itself keeps a big floor plane or landscape from leaving the picture empty.
	 */
	constexpr double GroundHalfWidthCm = 2000.0;
	bool IsGround(const FBox& Box)
	{
		const FVector Extent = Box.GetExtent();
		const double Across = FMath::Max(Extent.X, Extent.Y);
		return Across > GroundHalfWidthCm && Extent.Z < Across * 0.1;
	}

	/** The automatic view looks down this far (degrees) at what the level holds. */
	constexpr double AutoPitch = -25.0;

	/** At most this many warm-up captures, each followed by a wait for what it asked for (shaders, mips). */
	constexpr int32 WarmUpRounds = 8;

	/** Captures after the warm-up, so temporal anti-aliasing, Lumen and exposure settle on one picture. */
	constexpr int32 SettleFrames = 48;

	constexpr float StreamSeconds = 10.0f;

	/** Loads, asset compiles and shader jobs finished, textures streamed in, the render thread caught up. */
	void Settle()
	{
		FlushAsyncLoading();
		FAssetCompilingManager::Get().FinishAllCompilation();
		if (GShaderCompilingManager)
		{
			GShaderCompilingManager->FinishAllCompilation();
		}
		UTexture::ForceUpdateTextureStreaming();
		IStreamingManager::Get().StreamAllResources(StreamSeconds);
		IStreamingManager::Get().BlockTillAllRequestsFinished(StreamSeconds);
		FlushRenderingCommands();
	}

	/** The first Player Start by name, so the choice does not depend on actor iteration order. */
	const APlayerStart* FirstPlayerStart(UWorld* World)
	{
		const APlayerStart* First = nullptr;
		for (TActorIterator<APlayerStart> It(World); It; ++It)
		{
			if (!First || It->GetFName().LexicalLess(First->GetFName()))
			{
				First = *It;
			}
		}
		return First;
	}
}

FMantlePlaceThumbnailView FMantlePlaceThumbnail::AutoView(const FBox& Bounds, const FTransform* Spawn)
{
	using namespace MantlePlaceThumbnailPrivate;
	FMantlePlaceThumbnailView View;
	View.FOV = AutoFOV;
	View.bAutomatic = true;

	// What to frame: the level's content, or a space around the spawn (or the origin) when it draws nothing.
	const FVector SpawnEye = Spawn ? Spawn->GetLocation() + FVector(0.0, 0.0, EyeHeightCm) : FVector::ZeroVector;
	const FBox Frame = Bounds.IsValid ? Bounds : FBox::BuildAABB(SpawnEye, FVector(1000.0, 1000.0, 300.0));
	const FVector Center = Frame.GetCenter();

	// Seen from the spawn's side: along the line from where players arrive to the middle of the level, or the spawn's
	// own facing when it is the middle, or a fixed diagonal without a spawn.
	FVector Direction = Spawn ? FVector(Center.X - SpawnEye.X, Center.Y - SpawnEye.Y, 0.0) : FVector::ZeroVector;
	if (Direction.SizeSquared() < FMath::Square(100.0))
	{
		Direction = Spawn ? Spawn->GetRotation().GetForwardVector() : FVector(1.0, 1.0, 0.0);
		Direction.Z = 0.0;
	}
	if (!Direction.Normalize())
	{
		Direction = FVector::ForwardVector;
	}
	View.Rotation = FRotator(AutoPitch, Direction.Rotation().Yaw, 0.0);

	// Far enough back that the whole frame fits the 16:9 picture: each corner at depth d past the centre and a across
	// needs the camera a / tan(half angle) - d behind it, the vertical half angle being the narrower one.
	const FRotationMatrix Axes(View.Rotation);
	const FVector Forward = Axes.GetUnitAxis(EAxis::X), Right = Axes.GetUnitAxis(EAxis::Y), Up = Axes.GetUnitAxis(EAxis::Z);
	const double TanH = FMath::Tan(FMath::DegreesToRadians(static_cast<double>(View.FOV)) * 0.5);
	const double TanV = TanH * Height / Width;
	double Distance = 0.0;
	for (int32 Corner = 0; Corner < 8; ++Corner)
	{
		const FVector Point((Corner & 1) ? Frame.Max.X : Frame.Min.X, (Corner & 2) ? Frame.Max.Y : Frame.Min.Y, (Corner & 4) ? Frame.Max.Z : Frame.Min.Z);
		const FVector Offset = Point - Center;
		Distance = FMath::Max(Distance, FMath::Max(FMath::Abs(Offset | Right) / TanH, FMath::Abs(Offset | Up) / TanV) - (Offset | Forward));
	}
	Distance = FMath::Clamp(Distance * 1.05, 400.0, 30000.0);
	View.Location = Center - Forward * Distance;
	return View;
}

FBox FMantlePlaceThumbnail::DrawnBounds(UWorld* World)
{
	using namespace MantlePlaceThumbnailPrivate;
	FBox Bounds(ForceInit), Ground(ForceInit);
	if (!World)
	{
		return Bounds;
	}
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		const AActor* Actor = *It;
		if (!Actor || Actor->IsEditorOnly() || Actor->IsHidden() || Actor->IsA<AInfo>())
		{
			continue;
		}
		Actor->ForEachComponent<UPrimitiveComponent>(false, [&Bounds, &Ground](const UPrimitiveComponent* Primitive)
		{
			if (!Primitive->IsRegistered() || !Primitive->IsVisible() || Primitive->bHiddenInGame || Primitive->IsEditorOnly())
			{
				return;
			}
			const FBox Box = Primitive->Bounds.GetBox();
			if (Box.IsValid && Box.GetExtent().GetMax() <= MaxFramedHalfExtentCm)
			{
				(IsGround(Box) ? Ground : Bounds) += Box;
			}
		});
	}
	// What stands on the ground; the ground itself only when nothing does.
	return Bounds.IsValid ? Bounds : Ground;
}

FMantlePlaceThumbnailView FMantlePlaceThumbnail::AutoView(UWorld* World)
{
	using namespace MantlePlaceThumbnailPrivate;
	const APlayerStart* Start = World ? FirstPlayerStart(World) : nullptr;
	const FTransform Spawn = Start ? Start->GetActorTransform() : FTransform::Identity;
	return AutoView(DrawnBounds(World), Start ? &Spawn : nullptr);
}

FMantlePlaceThumbnailView FMantlePlaceThumbnail::ViewFor(UWorld* World)
{
	if (const AMantlePlaceSettingsActor* Settings = World ? AMantlePlaceSettingsActor::Find(World) : nullptr;
		Settings && Settings->ThumbnailChoice.Mode == EMantleThumbnailMode::Angle)
	{
		FMantlePlaceThumbnailView View;
		View.Location = Settings->ThumbnailLocation;
		View.Rotation = Settings->ThumbnailRotation;
		View.FOV = Settings->ThumbnailFOV;
		View.bAutomatic = false;
		return View;
	}
	return AutoView(World);
}

bool FMantlePlaceThumbnail::CanCapture(FString* OutWhyNot)
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
	return true;
}

bool FMantlePlaceThumbnail::Capture(UWorld* World, const FMantlePlaceThumbnailView& View, TArray<FColor>& OutPixels, FString& OutError)
{
	using namespace MantlePlaceThumbnailPrivate;
	OutPixels.Reset();
	if (!CanCapture(&OutError))
	{
		return false;
	}
	if (!World || !World->Scene)
	{
		OutError = TEXT("there is no level to draw");
		return false;
	}

	UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>(GetTransientPackage(), NAME_None, RF_Transient);
	Target->ClearColor = FLinearColor::Black;
	// 8-bit with the display gamma: the final LDR colour as a player's screen shows it.
	Target->InitCustomFormat(Width, Height, PF_B8G8R8A8, false);
	Target->UpdateResourceImmediate(true);

	// A camera on the level's own scene, owned by no actor and outside the level's package: it is never saved and the
	// level is not marked changed. Game show flags (the default), so editor icons, Player Starts and volumes don't draw.
	USceneCaptureComponent2D* Camera = NewObject<USceneCaptureComponent2D>(GetTransientPackage(), NAME_None, RF_Transient);
	Camera->TextureTarget = Target;
	Camera->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
	Camera->bCaptureEveryFrame = false;
	Camera->bCaptureOnMovement = false;
	Camera->FOVAngle = FMath::Clamp(View.FOV, 5.f, 170.f);
	// Kept between captures so temporal anti-aliasing, Lumen and eye adaptation converge over the settle frames, and
	// exposure adapts quickly within them: the level's own post-process volumes still decide the look.
	Camera->bAlwaysPersistRenderingState = true;
	Camera->PostProcessSettings.bOverride_AutoExposureSpeedUp = true;
	Camera->PostProcessSettings.bOverride_AutoExposureSpeedDown = true;
	Camera->PostProcessSettings.AutoExposureSpeedUp = 20.f;
	Camera->PostProcessSettings.AutoExposureSpeedDown = 20.f;
	Camera->SetWorldLocationAndRotation(View.Location, View.Rotation);
	// Rooted while the picture is taken: nothing references them but this function.
	Target->AddToRoot();
	Camera->AddToRoot();
	Camera->RegisterComponentWithWorld(World);

	// In game every place gets Mantle's sky, sun and sky light (FMantleEnvironment::Apply); a level often has none of its
	// own, which would leave a black sky. What it lacks is added for the picture as components owned by no actor and
	// outside the level's package, removed afterwards: the level is never changed or marked changed.
	TArray<USceneComponent*> Added;
	{
		bool bSky = false, bSun = false, bSkyLight = false;
		for (TObjectIterator<USceneComponent> It; It; ++It)
		{
			if (It->GetWorld() != World || !It->IsRegistered() || !It->IsVisible())
			{
				continue;
			}
			bSky |= It->IsA<USkyAtmosphereComponent>();
			bSun |= It->IsA<UDirectionalLightComponent>();
			bSkyLight |= It->IsA<USkyLightComponent>();
		}
		auto Add = [&Added, World](USceneComponent* Component, const FRotator& Rotation)
		{
			Component->AddToRoot();
			Component->SetMobility(EComponentMobility::Movable);
			Component->SetWorldRotation(Rotation);
			Component->RegisterComponentWithWorld(World);
			Added.Add(Component);
		};
		if (!bSun)
		{
			// An afternoon sun, lighting the view from over the camera's shoulder.
			UDirectionalLightComponent* Sun = NewObject<UDirectionalLightComponent>(GetTransientPackage(), NAME_None, RF_Transient);
			Sun->SetAtmosphereSunLight(true);
			Add(Sun, FRotator(-40.0, View.Rotation.Yaw + 35.0, 0.0));
		}
		if (!bSky)
		{
			Add(NewObject<USkyAtmosphereComponent>(GetTransientPackage(), NAME_None, RF_Transient), FRotator::ZeroRotator);
		}
		if (!bSkyLight)
		{
			USkyLightComponent* SkyLight = NewObject<USkyLightComponent>(GetTransientPackage(), NAME_None, RF_Transient);
			SkyLight->bRealTimeCapture = true;
			Add(SkyLight, FRotator::ZeroRotator);
		}
	}

	// Everything the picture needs in before it is taken, or a material draws as the engine's grey world grid. A
	// material the editor compiles only when first drawn asks at the first capture, so warm-ups run until none is left.
	Settle();
	int32 WarmUps = 0;
	for (; WarmUps < WarmUpRounds; ++WarmUps)
	{
		Camera->CaptureScene();
		FlushRenderingCommands();
		if (!GShaderCompilingManager || GShaderCompilingManager->GetNumRemainingJobs() == 0)
		{
			break;
		}
		Settle();
	}
	for (int32 Frame = 0; Frame < SettleFrames; ++Frame)
	{
		++GFrameCounter;
		World->Scene->IncrementFrameNumber();
		Camera->CaptureScene();
		FlushRenderingCommands();
	}

	bool bRead = false;
	if (FTextureRenderTargetResource* Resource = Target->GameThread_GetRenderTargetResource())
	{
		bRead = Resource->ReadPixels(OutPixels) && OutPixels.Num() == Width * Height;
	}
	for (USceneComponent* Component : Added)
	{
		Component->UnregisterComponent();
		Component->RemoveFromRoot();
		Component->MarkAsGarbage();
	}
	Camera->UnregisterComponent();
	Camera->RemoveFromRoot();
	Camera->MarkAsGarbage();
	Target->RemoveFromRoot();
	Target->MarkAsGarbage();
	if (!bRead)
	{
		OutPixels.Reset();
		OutError = TEXT("the picture could not be read back");
		return false;
	}
	// The capture's alpha is not coverage; the picture is opaque.
	for (FColor& Pixel : OutPixels)
	{
		Pixel.A = 255;
	}
	UE_LOG(LogMantle, Log, TEXT("MANTLE_PLACE_THUMBNAIL captured %dx%d from %s (%s view, FOV %.0f) after %d warm-up capture(s) and %d settle frames; %d sky piece(s) added for it"),
		Width, Height, *View.Location.ToCompactString(), View.bAutomatic ? TEXT("automatic") : TEXT("chosen"), View.FOV, WarmUps + 1, SettleFrames, Added.Num());
	return true;
}

FMantleThumbnailChoice FMantlePlaceThumbnail::ChoiceFor(UWorld* World)
{
	const AMantlePlaceSettingsActor* Settings = World ? AMantlePlaceSettingsActor::Find(World) : nullptr;
	return Settings ? Settings->ThumbnailChoice : FMantleThumbnailChoice();
}

FMantleThumbnailResolver::FRender FMantlePlaceThumbnail::RenderFor(UWorld* World)
{
	const TWeakObjectPtr<UWorld> Weak = World;
	return [Weak](const FMantleThumbnailChoice&, TArray<FColor>& OutPixels, int32& OutWidth, int32& OutHeight, FString& OutError)
	{
		UWorld* Level = Weak.Get();
		if (!Level)
		{
			OutError = TEXT("there is no level to draw");
			return false;
		}
		OutWidth = Width;
		OutHeight = Height;
		return Capture(Level, ViewFor(Level), OutPixels, OutError);
	};
}

FMantleThumbnailResult FMantlePlaceThumbnail::Resolve(UWorld* World)
{
	return FMantleThumbnailResolver::Resolve(ChoiceFor(World), Target(), RenderFor(World));
}
