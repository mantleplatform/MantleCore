// Copyright Joshua Eger. All Rights Reserved.
//
// Maintainer command (#167, the map style gate): Mantle.Dev.MinimapCompare <Directory> [CropX CropY]
// Makes the open level's map picture two ways over the same area at the same scale, times each, and writes them side by side:
//   A  Epic's minimap capture: the settings of the engine's World Partition minimap builder (FWorldPartitionMiniMapHelper::
//      CaptureBoundsMiniMapToTexture: top-down orthographic, unlit, no fog, atmosphere, shadows or post process, base color: the
//      helper's default; the builder's HDR scene color reads back black here),
//      tile by tile, 30 frames after each camera move. The engine function itself fakes an engine tick per frame, which cannot run
//      inside the editor's own tick, so its camera and settings are rebuilt here and the frames are taken as the place thumbnail
//      takes them.
//   B  The drawn map (FMantleMinimapCanvas): the level's mesh triangles drawn top-down on the CPU in flat colors.
// The area is settings.minimap's: the content that collides (FMantleMinimapGrid). Writes <Directory>/A_epic.png, B_drawn.png,
// compare_full.png and compare_zoom.png (4x around CropX, CropY in cm, else the middle). Nothing is saved, uploaded or changed in
// the level. Logs MANTLE_MINIMAP_COMPARE lines. Needs a renderer for A; B alone would not.
#include "MantleMinimapDrawing.h"
#include "MantlePlaceExporter.h"
#include "MantlePlaceWorldPartition.h"
#include "MantlePng.h"
#include "MantleRuntime.h"
#include "MantleTerrainExport.h"
#include "MantleWaterBodyExport.h"

#include "Async/ParallelFor.h"
#include "AssetCompilingManager.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/SplineMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "ContentStreaming.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "EngineDefines.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInterface.h"
#include "Math/OrthoMatrix.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RenderingThread.h"
#include "SceneInterface.h"
#include "ShaderCompiler.h"
#include "StaticMeshResources.h"
#include "TextureResource.h"
#include "UObject/Package.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleDevMinimapComparePrivate
{
	/** Frames taken after each camera move before the picture is kept (the settle rule). */
	constexpr int32 SettleFrames = 30;

	/** At most this many warm-up captures, each followed by a wait for the shaders and textures it asked for. */
	constexpr int32 WarmUpRounds = 8;

	/** A piece bigger than this (half-extent, cm) is a sky or a backdrop: never drawn, never part of the area. */
	constexpr double MaxPieceHalfExtentCm = 500000.0;

	/** The draw distance scale while A is taken, so nothing is culled for being far from the camera (Epic's builder advice). */
	constexpr float CaptureViewDistanceScale = 50.0f;

	/** The zoomed view: this many picture pixels a side, each drawn this many times bigger. */
	constexpr int32 ZoomPixels = 256;
	constexpr int32 ZoomFactor = 4;

	// ---- A: Epic's minimap capture ------------------------------------------------------------------------------------

	/** Loads, asset compiles and shader jobs finished, textures streamed in, the render thread caught up (as the place thumbnail). */
	void Settle()
	{
		FlushAsyncLoading();
		FAssetCompilingManager::Get().FinishAllCompilation();
		if (GShaderCompilingManager)
		{
			GShaderCompilingManager->FinishAllCompilation();
		}
		UTexture::ForceUpdateTextureStreaming();
		IStreamingManager::Get().StreamAllResources(10.0f);
		IStreamingManager::Get().BlockTillAllRequestsFinished(10.0f);
		FlushRenderingCommands();
	}

	/** FWorldPartitionMiniMapHelper::CalTopViewOfWorld: an orthographic view that fits Bounds' X and Y into Width x Height. */
	FMatrix TopView(const FBox& Bounds, int32 Width, int32 Height)
	{
		const FVector2D Size(FMath::Abs(Bounds.Max.X - Bounds.Min.X), FMath::Abs(Bounds.Max.Y - Bounds.Min.Y));
		const bool bUseX = Size.X / Size.Y > 1.0;
		const double AxisSize = bUseX ? Size.X : Size.Y;
		const double PixelSize = bUseX ? Width : Height;
		const float Zoom = static_cast<float>(AxisSize / PixelSize / 2.0);
		const float OrthoWidth = FMath::Max(1.f, Width * Zoom);
		const float OrthoHeight = FMath::Max(1.f, Height * Zoom);
		const double ZOffset = UE_FLOAT_HUGE_DISTANCE / 2.0;
		FMatrix Projection = FReversedZOrthoMatrix(OrthoWidth, OrthoHeight, 0.5f / ZOffset, ZOffset);
		if (Projection.ContainsNaN())
		{
			Projection.SetIdentity();
		}
		return Projection;
	}

	/** How A is taken. The default is Epic's builder; the probe (Mantle.Dev.MinimapCaptureProbe) changes one thing at a time. */
	struct FCaptureStyle
	{
		const TCHAR* Name = TEXT("epic");
		// The helper's default source (CaptureBoundsMiniMapToTexture's short form). The builder's SCS_SceneColorHDR reads back all black
		// into this 8-bit target in the editor (the probe, 2026-11-24: brightest 0 whatever the flags or projection).
		ESceneCaptureSource Source = ESceneCaptureSource::SCS_BaseColor;
		bool bEpicShowFlags = true;
		bool bCustomProjection = true;
	};

	/** One tile of A, Width x Height pixels over Bounds; the camera sits over Bounds' top looking down, +X right, +Y down. */
	bool CaptureEpicTile(UWorld* World, const FBox& Bounds, int32 Width, int32 Height, TArray<FColor>& OutPixels, int32& OutWarmUps,
		const FCaptureStyle& Style = FCaptureStyle())
	{
		UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>(GetTransientPackage(), NAME_None, RF_Transient);
		Target->ClearColor = FLinearColor::Transparent;
		Target->TargetGamma = 2.2f;
		Target->InitCustomFormat(Width, Height, PF_B8G8R8A8, false);
		Target->UpdateResourceImmediate(true);

		// The builder's capture actor, as a component owned by no actor and outside the level's package: the level is not changed.
		USceneCaptureComponent2D* Camera = NewObject<USceneCaptureComponent2D>(GetTransientPackage(), NAME_None, RF_Transient);
		Camera->TextureTarget = Target;
		Camera->ProjectionType = ECameraProjectionMode::Orthographic;
		Camera->CaptureSource = Style.Source;
		// Epic's builder sets OrthoWidth to the pixel width and draws with its own projection; without it, the engine's ortho view of the bounds.
		Camera->OrthoWidth = Style.bCustomProjection ? Width : static_cast<float>(Bounds.GetSize().X);
		Camera->bUseCustomProjectionMatrix = Style.bCustomProjection;
		Camera->CustomProjectionMatrix = TopView(Bounds, Width, Height);
		Camera->bCaptureEveryFrame = false;
		Camera->bCaptureOnMovement = false;
		if (Style.bEpicShowFlags)
		{
			Camera->ShowFlags.Lighting = false;
			Camera->ShowFlags.Atmosphere = false;
			Camera->ShowFlags.PostProcessing = false;
			Camera->ShowFlags.AntiAliasing = false;
			Camera->ShowFlags.Fog = false;
			Camera->ShowFlags.VolumetricFog = false;
			Camera->ShowFlags.DynamicShadows = false;
			Camera->ShowFlags.SkyLighting = false;
		}
		Camera->PostProcessSettings.bOverride_VignetteIntensity = true;
		Camera->PostProcessSettings.VignetteIntensity = 0.0f;
		Camera->SetWorldLocationAndRotation(FVector(Bounds.GetCenter().X, Bounds.GetCenter().Y, Bounds.GetCenter().Z + Bounds.GetExtent().Z),
			FRotator(-90.0, 0.0, -90.0));
		Target->AddToRoot();
		Camera->AddToRoot();
		Camera->RegisterComponentWithWorld(World);

		// A material first drawn here compiles now, and draws as the gray grid until it is done: warm up until no shader job is left.
		Settle();
		OutWarmUps = 0;
		for (; OutWarmUps < WarmUpRounds; ++OutWarmUps)
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
		Camera->UnregisterComponent();
		Camera->RemoveFromRoot();
		Camera->MarkAsGarbage();
		Target->RemoveFromRoot();
		Target->MarkAsGarbage();
		return bRead;
	}

	// ---- B: the drawn map ---------------------------------------------------------------------------------------------

	/** One mesh's render LOD 0 (a Nanite mesh's fallback), read once however many times it is placed. */
	struct FMeshTriangles
	{
		TArray<FVector3f> Positions;
		TArray<uint32> Indices;
		/** Per section: first index, triangle count, material slot. */
		TArray<FIntVector3> Sections;
		FBox Bounds = FBox(ForceInit);
	};

	/** One placed mesh component and the surface each of its sections draws as. */
	struct FPiece
	{
		const UStaticMeshComponent* Component = nullptr;
		int32 Mesh = INDEX_NONE;
		TArray<EMantleMinimapSurface> Surfaces;
	};

	/** One mesh drawn once: a component, or one instance of an instanced one. */
	struct FDraw
	{
		int32 Piece = INDEX_NONE;
		FTransform Transform;
	};

	bool ReadMesh(const UStaticMesh* Mesh, FMeshTriangles& Out)
	{
		const FStaticMeshRenderData* RenderData = Mesh ? Mesh->GetRenderData() : nullptr;
		if (!RenderData || RenderData->LODResources.Num() == 0)
		{
			return false;
		}
		const FStaticMeshLODResources& LOD = RenderData->LODResources[0];
		const FPositionVertexBuffer& Positions = LOD.VertexBuffers.PositionVertexBuffer;
		if (Positions.GetNumVertices() == 0 || !Positions.GetVertexData())
		{
			return false;
		}
		Out.Positions.SetNumUninitialized(Positions.GetNumVertices());
		for (uint32 Vertex = 0; Vertex < Positions.GetNumVertices(); ++Vertex)
		{
			Out.Positions[Vertex] = Positions.VertexPosition(Vertex);
			Out.Bounds += FVector(Out.Positions[Vertex]);
		}
		LOD.IndexBuffer.GetCopy(Out.Indices);
		if (Out.Indices.Num() == 0)
		{
			return false;
		}
		for (const FStaticMeshSection& Section : LOD.Sections)
		{
			Out.Sections.Add(FIntVector3(static_cast<int32>(Section.FirstIndex), static_cast<int32>(Section.NumTriangles), Section.MaterialIndex));
		}
		return true;
	}

	/** What a section draws as: its material's name says, else the mesh's, else the actor's; instanced foliage is green; else unknown (ground, or a building where it stands tall). */
	EMantleMinimapSurface SurfaceOf(const UStaticMeshComponent* Component, int32 MaterialSlot)
	{
		const UMaterialInterface* Material = Component->GetMaterial(MaterialSlot);
		const AActor* Owner = Component->GetOwner();
		const FString Names[] = {
			Material ? Material->GetName() : FString(),
			Component->GetStaticMesh() ? Component->GetStaticMesh()->GetName() : FString(),
			Owner ? Owner->GetActorLabel() : FString(),
		};
		for (const FString& Name : Names)
		{
			const EMantleMinimapSurface Surface = FMantleMinimapCanvas::Classify(Name);
			if (Surface != EMantleMinimapSurface::None)
			{
				return Surface;
			}
		}
		if (Owner && Owner->GetClass()->GetName().Contains(TEXT("Foliage")))
		{
			return EMantleMinimapSurface::Green;
		}
		return EMantleMinimapSurface::Unknown;
	}

	// ---- Pictures with words on them ----------------------------------------------------------------------------------

	struct FImage
	{
		int32 Width = 0;
		int32 Height = 0;
		TArray<FColor> Pixels;

		FImage() = default;
		FImage(int32 InWidth, int32 InHeight, FColor Fill) : Width(InWidth), Height(InHeight) { Pixels.Init(Fill, InWidth * InHeight); }

		void Blit(const FImage& Source, int32 X0, int32 Y0, int32 Scale = 1)
		{
			for (int32 Y = 0; Y < Source.Height * Scale; ++Y)
			{
				for (int32 X = 0; X < Source.Width * Scale; ++X)
				{
					const int32 DX = X0 + X, DY = Y0 + Y;
					if (DX >= 0 && DY >= 0 && DX < Width && DY < Height)
					{
						Pixels[DY * Width + DX] = Source.Pixels[(Y / Scale) * Source.Width + X / Scale];
					}
				}
			}
		}

		FImage Crop(int32 X0, int32 Y0, int32 CropWidth, int32 CropHeight) const
		{
			FImage Out(CropWidth, CropHeight, FColor::Black);
			for (int32 Y = 0; Y < CropHeight; ++Y)
			{
				for (int32 X = 0; X < CropWidth; ++X)
				{
					const int32 SX = FMath::Clamp(X0 + X, 0, Width - 1), SY = FMath::Clamp(Y0 + Y, 0, Height - 1);
					Out.Pixels[Y * CropWidth + X] = Pixels[SY * Width + SX];
				}
			}
			return Out;
		}

		/** 5 x 7 capitals, digits and a little punctuation; anything else is a space. Scale pixels per font pixel. */
		void Text(int32 X0, int32 Y0, const FString& Words, int32 Scale, FColor Color)
		{
			int32 X = X0;
			for (const TCHAR Char : Words.ToUpper())
			{
				const uint8* Rows = Glyph(Char);
				for (int32 Row = 0; Row < 7; ++Row)
				{
					for (int32 Column = 0; Column < 5; ++Column)
					{
						if (Rows && (Rows[Row] >> (4 - Column)) & 1)
						{
							for (int32 SY = 0; SY < Scale; ++SY)
							{
								for (int32 SX = 0; SX < Scale; ++SX)
								{
									const int32 PX = X + Column * Scale + SX, PY = Y0 + Row * Scale + SY;
									if (PX >= 0 && PY >= 0 && PX < Width && PY < Height)
									{
										Pixels[PY * Width + PX] = Color;
									}
								}
							}
						}
					}
				}
				X += 6 * Scale;
			}
		}

		static const uint8* Glyph(TCHAR Char)
		{
			struct FGlyph
			{
				TCHAR Char;
				uint8 Rows[7];
			};
			static const FGlyph Font[] = {
				{ TEXT('A'), { 0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001 } },
				{ TEXT('B'), { 0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110 } },
				{ TEXT('C'), { 0b01110, 0b10001, 0b10000, 0b10000, 0b10000, 0b10001, 0b01110 } },
				{ TEXT('D'), { 0b11100, 0b10010, 0b10001, 0b10001, 0b10001, 0b10010, 0b11100 } },
				{ TEXT('E'), { 0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111 } },
				{ TEXT('F'), { 0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b10000 } },
				{ TEXT('G'), { 0b01110, 0b10001, 0b10000, 0b10111, 0b10001, 0b10001, 0b01111 } },
				{ TEXT('H'), { 0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001 } },
				{ TEXT('I'), { 0b01110, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110 } },
				{ TEXT('J'), { 0b00111, 0b00010, 0b00010, 0b00010, 0b00010, 0b10010, 0b01100 } },
				{ TEXT('K'), { 0b10001, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010, 0b10001 } },
				{ TEXT('L'), { 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111 } },
				{ TEXT('M'), { 0b10001, 0b11011, 0b10101, 0b10101, 0b10001, 0b10001, 0b10001 } },
				{ TEXT('N'), { 0b10001, 0b10001, 0b11001, 0b10101, 0b10011, 0b10001, 0b10001 } },
				{ TEXT('O'), { 0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110 } },
				{ TEXT('P'), { 0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000 } },
				{ TEXT('Q'), { 0b01110, 0b10001, 0b10001, 0b10001, 0b10101, 0b10010, 0b01101 } },
				{ TEXT('R'), { 0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001 } },
				{ TEXT('S'), { 0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110 } },
				{ TEXT('T'), { 0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100 } },
				{ TEXT('U'), { 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110 } },
				{ TEXT('V'), { 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100 } },
				{ TEXT('W'), { 0b10001, 0b10001, 0b10001, 0b10101, 0b10101, 0b10101, 0b01010 } },
				{ TEXT('X'), { 0b10001, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001, 0b10001 } },
				{ TEXT('Y'), { 0b10001, 0b10001, 0b01010, 0b00100, 0b00100, 0b00100, 0b00100 } },
				{ TEXT('Z'), { 0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0b11111 } },
				{ TEXT('0'), { 0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110 } },
				{ TEXT('1'), { 0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110 } },
				{ TEXT('2'), { 0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111 } },
				{ TEXT('3'), { 0b11111, 0b00010, 0b00100, 0b00010, 0b00001, 0b10001, 0b01110 } },
				{ TEXT('4'), { 0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010 } },
				{ TEXT('5'), { 0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110 } },
				{ TEXT('6'), { 0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110 } },
				{ TEXT('7'), { 0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000 } },
				{ TEXT('8'), { 0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110 } },
				{ TEXT('9'), { 0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100 } },
				{ TEXT('\''), { 0b00100, 0b00100, 0b01000, 0b00000, 0b00000, 0b00000, 0b00000 } },
				{ TEXT('.'), { 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b01100, 0b01100 } },
				{ TEXT(','), { 0b00000, 0b00000, 0b00000, 0b00000, 0b01100, 0b00100, 0b01000 } },
				{ TEXT(':'), { 0b00000, 0b01100, 0b01100, 0b00000, 0b01100, 0b01100, 0b00000 } },
				{ TEXT('-'), { 0b00000, 0b00000, 0b00000, 0b11111, 0b00000, 0b00000, 0b00000 } },
				{ TEXT('('), { 0b00010, 0b00100, 0b01000, 0b01000, 0b01000, 0b00100, 0b00010 } },
				{ TEXT(')'), { 0b01000, 0b00100, 0b00010, 0b00010, 0b00010, 0b00100, 0b01000 } },
				{ TEXT('/'), { 0b00001, 0b00010, 0b00010, 0b00100, 0b01000, 0b01000, 0b10000 } },
				{ TEXT('+'), { 0b00000, 0b00100, 0b00100, 0b11111, 0b00100, 0b00100, 0b00000 } },
				{ TEXT('='), { 0b00000, 0b00000, 0b11111, 0b00000, 0b11111, 0b00000, 0b00000 } },
			};
			for (const FGlyph& Entry : Font)
			{
				if (Entry.Char == Char)
				{
					return Entry.Rows;
				}
			}
			return nullptr;
		}

		bool Save(const FString& File) const
		{
			return FFileHelper::SaveArrayToFile(FMantlePng::EncodeRGBA8(Width, Height, Pixels), *File);
		}
	};

	const FColor Paper(24, 26, 30, 255);
	const FColor Ink(240, 240, 240, 255);
	const FColor Faint(170, 176, 186, 255);
	constexpr int32 Gap = 24;
	constexpr int32 TextScale = 3;
	constexpr int32 LineHeight = 7 * TextScale + 12;

	/** Two pictures of the same size side by side, each with its title and time over it, and a line under the whole. */
	FImage SideBySide(const FImage& A, const FImage& B, const FString& TitleA, const FString& TitleB, const FString& Footer)
	{
		const int32 Top = Gap + 2 * LineHeight;
		FImage Out(A.Width + B.Width + 3 * Gap, Top + FMath::Max(A.Height, B.Height) + LineHeight + 2 * Gap, Paper);
		Out.Text(Gap, Gap, TitleA, TextScale, Ink);
		Out.Text(2 * Gap + A.Width, Gap, TitleB, TextScale, Ink);
		Out.Blit(A, Gap, Top);
		Out.Blit(B, 2 * Gap + A.Width, Top);
		Out.Text(Gap, Top + FMath::Max(A.Height, B.Height) + Gap, Footer, 2, Faint);
		return Out;
	}

	FString Seconds(double Value)
	{
		return FString::Printf(TEXT("%.2f S"), Value);
	}

	void Run(const TArray<FString>& Args, UWorld* World)
	{
		if (!World || Args.Num() < 1)
		{
			UE_LOG(LogMantle, Error, TEXT("MANTLE_MINIMAP_COMPARE_FAILED usage: Mantle.Dev.MinimapCompare <Directory> [CropX CropY] with a level open"));
			return;
		}
		const FString Directory = Args[0];
		IFileManager::Get().MakeDirectory(*Directory, true);
		UE_LOG(LogMantle, Log, TEXT("MANTLE_MINIMAP_COMPARE_START level=%s directory=%s"), *World->GetName(), *Directory);

		// The whole level, as Publish has it.
		double Clock = FPlatformTime::Seconds();
		FMantlePlaceWorldPartition::FLoadResult Loaded = FMantlePlaceWorldPartition::LoadAll(World);
		Settle();
		const double LoadSeconds = FPlatformTime::Seconds() - Clock;

		// ---- The area (shared by A and B): the content that collides; landscapes and water are not drawn yet. ----
		Clock = FPlatformTime::Seconds();
		TArray<FPiece> Pieces;
		FBox Colliding(ForceInit), Everything(ForceInit);
		int32 SkippedSplines = 0, SkippedHuge = 0;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			const AActor* Actor = *It;
			if (!Actor || Actor->IsHidden() || Actor->IsEditorOnly()
				|| FMantlePlaceExporter::IsEngineInfrastructure(Actor) || FMantlePlaceExporter::IsProvidedByMantle(Actor))
			{
				continue;
			}
			TInlineComponentArray<UStaticMeshComponent*> Components(const_cast<AActor*>(Actor));
			for (const UStaticMeshComponent* Component : Components)
			{
				if (!Component || !Component->IsRegistered() || !Component->IsVisible() || Component->bHiddenInGame || Component->IsEditorOnly()
					|| !Component->GetStaticMesh())
				{
					continue;
				}
				if (Component->IsA<USplineMeshComponent>())
				{
					++SkippedSplines;
					continue;
				}
				const FBox Box = Component->Bounds.GetBox();
				if (!Box.IsValid || Box.GetExtent().GetMax() > MaxPieceHalfExtentCm)
				{
					++SkippedHuge;
					continue;
				}
				const bool bCollides = Component->IsCollisionEnabled() && Component->GetCollisionResponseToChannel(ECC_Pawn) == ECR_Block;
				if (bCollides)
				{
					Colliding += Box;
				}
				Everything += Box;
				Pieces.Add(FPiece{ Component });
			}
		}
		const bool bAreaFromColliding = Colliding.IsValid != 0;
		const FBox Area3D = bAreaFromColliding ? Colliding : Everything;
		const int32 Landscapes = FMantleTerrainExport::CollectLandscapes(World).Num();
		int32 WaterBodies = 0;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			WaterBodies += FMantleWaterBodyExport::IsWaterBodyActor(*It) ? 1 : 0;
		}
		const FMantleMinimapGrid Grid = FMantleMinimapGrid::Plan(Area3D.IsValid ? FBox2D(FVector2D(Area3D.Min), FVector2D(Area3D.Max)) : FBox2D(ForceInit));
		const double AreaSeconds = FPlatformTime::Seconds() - Clock;
		UE_LOG(LogMantle, Log, TEXT("MANTLE_MINIMAP_COMPARE_AREA from=%s x=%.0f..%.0f y=%.0f..%.0f z=%.0f..%.0f pieces=%d notDrawnYet: splineMeshes=%d landscapes=%d waterBodies=%d skippedHuge=%d seconds=%.3f"),
			bAreaFromColliding ? TEXT("colliding") : TEXT("everything(nothing collides)"), Area3D.Min.X, Area3D.Max.X, Area3D.Min.Y, Area3D.Max.Y,
			Area3D.Min.Z, Area3D.Max.Z, Pieces.Num(), SkippedSplines, Landscapes, WaterBodies, SkippedHuge, AreaSeconds);
		if (!Grid.IsValid())
		{
			FMantlePlaceWorldPartition::ReleaseLoaded(World, Loaded);
			UE_LOG(LogMantle, Error, TEXT("MANTLE_MINIMAP_COMPARE_FAILED the level has nothing to draw"));
			return;
		}
		UE_LOG(LogMantle, Log, TEXT("MANTLE_MINIMAP_COMPARE_GRID cmPerPixel=%.3f pixels=%dx%d tiles=%dx%d tile=%dx%d bounds x=%.1f..%.1f y=%.1f..%.1f"),
			Grid.CmPerPixel, Grid.Width, Grid.Height, Grid.Columns, Grid.Rows, Grid.TileWidth, Grid.TileHeight, Grid.MinX, Grid.MaxX, Grid.MinY, Grid.MaxY);

		// ---- B: the drawn map. ----
		Clock = FPlatformTime::Seconds();
		TArray<FMeshTriangles> Meshes;
		TMap<const UStaticMesh*, int32> MeshIndex;
		TArray<FDraw> Draws;
		int32 Unreadable = 0;
		const FBox2D Area2D(FVector2D(Grid.MinX, Grid.MinY), FVector2D(Grid.MaxX, Grid.MaxY));
		for (int32 PieceIndex = 0; PieceIndex < Pieces.Num(); ++PieceIndex)
		{
			FPiece& Piece = Pieces[PieceIndex];
			const UStaticMesh* Mesh = Piece.Component->GetStaticMesh();
			if (const int32* Found = MeshIndex.Find(Mesh))
			{
				Piece.Mesh = *Found;
			}
			else
			{
				FMeshTriangles Read;
				Piece.Mesh = ReadMesh(Mesh, Read) ? Meshes.Add(MoveTemp(Read)) : INDEX_NONE;
				MeshIndex.Add(Mesh, Piece.Mesh);
				Unreadable += Piece.Mesh == INDEX_NONE ? 1 : 0;
			}
			if (Piece.Mesh == INDEX_NONE)
			{
				continue;
			}
			for (const FIntVector3& Section : Meshes[Piece.Mesh].Sections)
			{
				Piece.Surfaces.Add(SurfaceOf(Piece.Component, Section.Z));
			}
			auto Add = [&Draws, &Meshes, &Piece, &Area2D, PieceIndex](const FTransform& Transform)
			{
				const FBox Box = Meshes[Piece.Mesh].Bounds.TransformBy(Transform);
				if (Box.IsValid && Area2D.Intersect(FBox2D(FVector2D(Box.Min), FVector2D(Box.Max))))
				{
					Draws.Add(FDraw{ PieceIndex, Transform });
				}
			};
			if (const UInstancedStaticMeshComponent* Instanced = Cast<UInstancedStaticMeshComponent>(Piece.Component))
			{
				for (int32 Instance = 0; Instance < Instanced->GetInstanceCount(); ++Instance)
				{
					FTransform Transform;
					if (Instanced->GetInstanceTransform(Instance, Transform, /*bWorldSpace*/ true))
					{
						Add(Transform);
					}
				}
			}
			else
			{
				Add(Piece.Component->GetComponentTransform());
			}
		}
		const double ReadSeconds = FPlatformTime::Seconds() - Clock;

		Clock = FPlatformTime::Seconds();
		FMantleMinimapCanvas Canvas(Grid);
		TAtomic<int64> Triangles(0);
		ParallelFor(Draws.Num(), [&Draws, &Pieces, &Meshes, &Canvas, &Triangles](int32 DrawIndex)
		{
			const FDraw& Draw = Draws[DrawIndex];
			const FPiece& Piece = Pieces[Draw.Piece];
			const FMeshTriangles& Mesh = Meshes[Piece.Mesh];
			TArray<FVector> Corners;
			Corners.SetNumUninitialized(Mesh.Positions.Num());
			for (int32 Vertex = 0; Vertex < Mesh.Positions.Num(); ++Vertex)
			{
				Corners[Vertex] = Draw.Transform.TransformPosition(FVector(Mesh.Positions[Vertex]));
			}
			int64 Drawn = 0;
			for (int32 SectionIndex = 0; SectionIndex < Mesh.Sections.Num(); ++SectionIndex)
			{
				const FIntVector3& Section = Mesh.Sections[SectionIndex];
				const EMantleMinimapSurface Surface = Piece.Surfaces[SectionIndex];
				for (int32 Triangle = 0; Triangle < Section.Y; ++Triangle)
				{
					const int32 First = Section.X + Triangle * 3;
					if (First + 2 >= Mesh.Indices.Num())
					{
						break;
					}
					const uint32 I0 = Mesh.Indices[First], I1 = Mesh.Indices[First + 1], I2 = Mesh.Indices[First + 2];
					if (I0 < static_cast<uint32>(Corners.Num()) && I1 < static_cast<uint32>(Corners.Num()) && I2 < static_cast<uint32>(Corners.Num()))
					{
						Canvas.DrawTriangle(Corners[I0], Corners[I1], Corners[I2], Surface);
						++Drawn;
					}
				}
			}
			Triangles += Drawn;
		});
		const double DrawSeconds = FPlatformTime::Seconds() - Clock;

		Clock = FPlatformTime::Seconds();
		FImage Drawn;
		Drawn.Width = Grid.Width;
		Drawn.Height = Grid.Height;
		Drawn.Pixels = Canvas.Shade();
		const double ShadeSeconds = FPlatformTime::Seconds() - Clock;
		const double DrawnSeconds = AreaSeconds + ReadSeconds + DrawSeconds + ShadeSeconds;
		UE_LOG(LogMantle, Log, TEXT("MANTLE_MINIMAP_COMPARE_B seconds=%.3f (area %.3f, read %.3f, draw %.3f, shade %.3f) meshes=%d unreadableMeshes=%d draws=%d triangles=%lld pixels=%lld covered=%lld"),
			DrawnSeconds, AreaSeconds, ReadSeconds, DrawSeconds, ShadeSeconds, Meshes.Num(), Unreadable, Draws.Num(), Triangles.Load(),
			static_cast<int64>(Grid.Width) * Grid.Height, Canvas.CountCovered());

		// ---- A: Epic's minimap capture, tile by tile. ----
		Clock = FPlatformTime::Seconds();
		IConsoleVariable* ViewDistance = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ViewDistanceScale"));
		const float ViewDistanceBefore = ViewDistance ? ViewDistance->GetFloat() : 1.0f;
		if (ViewDistance)
		{
			ViewDistance->Set(CaptureViewDistanceScale, ECVF_SetByConsole);
		}
		FImage Epic(Grid.Width, Grid.Height, FColor::Black);
		bool bEpic = true;
		int32 WarmUps = 0;
		for (int32 Row = 0; Row < Grid.Rows && bEpic; ++Row)
		{
			for (int32 Column = 0; Column < Grid.Columns && bEpic; ++Column)
			{
				const FBox2D Tile = Grid.TileBounds(Row, Column);
				const FBox Bounds(FVector(Tile.Min, Area3D.Min.Z), FVector(Tile.Max, Area3D.Max.Z + 10000.0));
				TArray<FColor> Pixels;
				int32 TileWarmUps = 0;
				bEpic = CaptureEpicTile(World, Bounds, Grid.TileWidth, Grid.TileHeight, Pixels, TileWarmUps);
				WarmUps += TileWarmUps + 1;
				for (int32 Y = 0; Y < Grid.TileHeight && bEpic; ++Y)
				{
					for (int32 X = 0; X < Grid.TileWidth; ++X)
					{
						FColor Pixel = Pixels[Y * Grid.TileWidth + X];
						// The capture's alpha is coverage (nothing drawn is clear); the picture is opaque.
						Pixel.A = 255;
						Epic.Pixels[(Row * Grid.TileHeight + Y) * Grid.Width + Column * Grid.TileWidth + X] = Pixel;
					}
				}
			}
		}
		if (ViewDistance)
		{
			ViewDistance->Set(ViewDistanceBefore, ECVF_SetByConsole);
		}
		const double EpicSeconds = AreaSeconds + (FPlatformTime::Seconds() - Clock);
		UE_LOG(LogMantle, Log, TEXT("MANTLE_MINIMAP_COMPARE_A %s seconds=%.3f (area %.3f, capture %.3f) tiles=%d warmUpCaptures=%d settleFramesPerTile=%d viewDistanceScale=%.0f"),
			bEpic ? TEXT("ok") : TEXT("FAILED"), EpicSeconds, AreaSeconds, EpicSeconds - AreaSeconds, Grid.Rows * Grid.Columns, WarmUps, SettleFrames,
			CaptureViewDistanceScale);
		FMantlePlaceWorldPartition::ReleaseLoaded(World, Loaded);

		// ---- The pictures. ----
		const FString Scale = FString::Printf(TEXT("SAME AREA AND SCALE: %.0f X %.0f M, %.2f M PER PIXEL, %d X %d PIXELS (%d X %d TILES). LEVEL LOAD %.1f S NOT COUNTED."),
			Grid.Width * Grid.CmPerPixel / 100.0, Grid.Height * Grid.CmPerPixel / 100.0, Grid.CmPerPixel / 100.0, Grid.Width, Grid.Height, Grid.Columns, Grid.Rows,
			LoadSeconds);
		const FString TitleA = FString::Printf(TEXT("A  EPIC'S BUILT-IN CAPTURE   %s"), *Seconds(EpicSeconds));
		const FString TitleB = FString::Printf(TEXT("B  MANTLE DRAWN MAP   %s"), *Seconds(DrawnSeconds));
		const FImage Full = SideBySide(Epic, Drawn, TitleA, TitleB, Scale);

		FVector2D Center(Grid.Width * 0.5, Grid.Height * 0.5);
		if (Args.Num() >= 3)
		{
			Center = Grid.ToPixel(FCString::Atod(*Args[1]), FCString::Atod(*Args[2]));
		}
		const int32 Side = FMath::Min3(ZoomPixels, Grid.Width, Grid.Height);
		const int32 CropX = FMath::Clamp(FMath::RoundToInt32(Center.X) - Side / 2, 0, Grid.Width - Side);
		const int32 CropY = FMath::Clamp(FMath::RoundToInt32(Center.Y) - Side / 2, 0, Grid.Height - Side);
		const FImage ZoomA = Epic.Crop(CropX, CropY, Side, Side), ZoomB = Drawn.Crop(CropX, CropY, Side, Side);
		FImage BigA(Side * ZoomFactor, Side * ZoomFactor, Paper), BigB(Side * ZoomFactor, Side * ZoomFactor, Paper);
		BigA.Blit(ZoomA, 0, 0, ZoomFactor);
		BigB.Blit(ZoomB, 0, 0, ZoomFactor);
		const FString ZoomFooter = FString::Printf(TEXT("ZOOMED %dX: %.0f X %.0f M AROUND X %.0f, Y %.0f (CM). SAME PIXELS AS THE FULL VIEW."),
			ZoomFactor, Side * Grid.CmPerPixel / 100.0, Side * Grid.CmPerPixel / 100.0,
			Grid.MinX + (CropX + Side * 0.5) * Grid.CmPerPixel, Grid.MinY + (CropY + Side * 0.5) * Grid.CmPerPixel);
		const FImage Zoom = SideBySide(BigA, BigB, TitleA, TitleB, ZoomFooter);

		bool bSaved = true;
		struct FOutput
		{
			const FImage* Image;
			const TCHAR* Name;
		};
		const FOutput Files[] = {
			{ &Epic, TEXT("A_epic.png") }, { &Drawn, TEXT("B_drawn.png") }, { &Full, TEXT("compare_full.png") }, { &Zoom, TEXT("compare_zoom.png") } };
		for (const FOutput& File : Files)
		{
			const FString Path = FPaths::Combine(Directory, File.Name);
			const bool bThis = File.Image->Save(Path);
			bSaved &= bThis;
			UE_LOG(LogMantle, Log, TEXT("MANTLE_MINIMAP_COMPARE_FILE %s %s"), bThis ? TEXT("wrote") : TEXT("FAILED"), *Path);
		}
		UE_LOG(LogMantle, Log, TEXT("MANTLE_MINIMAP_COMPARE_DONE epic=%d saved=%d epicSeconds=%.3f drawnSeconds=%.3f"), bEpic ? 1 : 0, bSaved ? 1 : 0,
			EpicSeconds, DrawnSeconds);
	}

	/** Diagnosis for a black A: one 512-pixel tile over the middle of the area, taken in five ways, each written and its brightest value logged. */
	void Probe(const TArray<FString>& Args, UWorld* World)
	{
		if (!World || Args.Num() < 1)
		{
			UE_LOG(LogMantle, Error, TEXT("MANTLE_MINIMAP_PROBE_FAILED usage: Mantle.Dev.MinimapCaptureProbe <Directory> with a level open"));
			return;
		}
		IFileManager::Get().MakeDirectory(*Args[0], true);
		FMantlePlaceWorldPartition::FLoadResult Loaded = FMantlePlaceWorldPartition::LoadAll(World);
		Settle();
		FBox Area(ForceInit);
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			TInlineComponentArray<UStaticMeshComponent*> Components(*It);
			for (const UStaticMeshComponent* Component : Components)
			{
				if (Component && Component->IsRegistered() && Component->IsVisible() && Component->GetStaticMesh()
					&& Component->Bounds.GetBox().GetExtent().GetMax() < MaxPieceHalfExtentCm)
				{
					Area += Component->Bounds.GetBox();
				}
			}
		}
		if (!Area.IsValid)
		{
			FMantlePlaceWorldPartition::ReleaseLoaded(World, Loaded);
			UE_LOG(LogMantle, Error, TEXT("MANTLE_MINIMAP_PROBE_FAILED nothing to capture"));
			return;
		}
		const FVector Center = Area.GetCenter();
		const double Half = FMath::Min(Area.GetExtent().X, Area.GetExtent().Y) * 0.5;
		const FBox Bounds(FVector(Center.X - Half, Center.Y - Half, Area.Min.Z), FVector(Center.X + Half, Center.Y + Half, Area.Max.Z + 10000.0));
		constexpr int32 Size = 512;
		const FCaptureStyle Styles[] = {
			{ TEXT("1_epic_exact"), ESceneCaptureSource::SCS_SceneColorHDR, true, true },
			{ TEXT("2_basecolor_epicflags"), ESceneCaptureSource::SCS_BaseColor, true, true },
			{ TEXT("3_hdr_defaultflags"), ESceneCaptureSource::SCS_SceneColorHDR, false, true },
			{ TEXT("4_finalldr_defaultflags"), ESceneCaptureSource::SCS_FinalColorLDR, false, true },
			{ TEXT("5_hdr_epicflags_engineortho"), ESceneCaptureSource::SCS_SceneColorHDR, true, false },
		};
		for (const FCaptureStyle& Style : Styles)
		{
			TArray<FColor> Pixels;
			int32 WarmUps = 0;
			const double Started = FPlatformTime::Seconds();
			const bool bRead = CaptureEpicTile(World, Bounds, Size, Size, Pixels, WarmUps, Style);
			int32 Brightest = 0;
			int64 Lit = 0;
			for (FColor& Pixel : Pixels)
			{
				const int32 Most = FMath::Max3(Pixel.R, Pixel.G, Pixel.B);
				Brightest = FMath::Max(Brightest, Most);
				Lit += Most > 8 ? 1 : 0;
				Pixel.A = 255;
			}
			FImage Image;
			Image.Width = Size;
			Image.Height = Size;
			Image.Pixels = Pixels;
			const FString File = FPaths::Combine(Args[0], FString::Printf(TEXT("probe_%s.png"), Style.Name));
			const bool bSaved = bRead && Image.Save(File);
			UE_LOG(LogMantle, Log, TEXT("MANTLE_MINIMAP_PROBE %s read=%d saved=%d brightest=%d litPixels=%lld of %d seconds=%.2f"), Style.Name, bRead ? 1 : 0,
				bSaved ? 1 : 0, Brightest, Lit, Size * Size, FPlatformTime::Seconds() - Started);
		}
		FMantlePlaceWorldPartition::ReleaseLoaded(World, Loaded);
		UE_LOG(LogMantle, Log, TEXT("MANTLE_MINIMAP_PROBE_DONE bounds x=%.0f..%.0f y=%.0f..%.0f z=%.0f..%.0f"), Bounds.Min.X, Bounds.Max.X, Bounds.Min.Y,
			Bounds.Max.Y, Bounds.Min.Z, Bounds.Max.Z);
	}

	FAutoConsoleCommandWithWorldAndArgs GMantleDevMinimapProbeCommand(
		TEXT("Mantle.Dev.MinimapCaptureProbe"),
		TEXT("Mantle.Dev.MinimapCaptureProbe <Directory> -- the top-down capture over the middle of the open level taken five ways (Epic's settings, ")
		TEXT("then one change each), to find what turns it black. MANTLE_MINIMAP_PROBE lines."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			Probe(Args, World);
		}));

	FAutoConsoleCommandWithWorldAndArgs GMantleDevMinimapCompareCommand(
		TEXT("Mantle.Dev.MinimapCompare"),
		TEXT("Mantle.Dev.MinimapCompare <Directory> [CropX CropY] -- the open level's map picture made two ways over the same area and scale, ")
		TEXT("timed and side by side: (A) Epic's minimap capture settings, (B) the drawn map. Nothing is saved or uploaded. MANTLE_MINIMAP_COMPARE lines."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			Run(Args, World);
		}));
}
