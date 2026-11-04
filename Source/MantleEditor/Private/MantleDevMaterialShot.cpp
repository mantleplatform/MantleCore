// Copyright Joshua Eger. All Rights Reserved.
//
// Maintainer command: Mantle.Dev.MaterialShot <MaterialId or asset path> <Directory> [size]
// Draws one of Mantle's built-in materials (ast_mat_carbon, ast_mat_chrome, ...) the way the catalog draws a thumbnail (FMantleCosmeticThumbnail: the engine's thumbnail scene),
// on a sphere and on a cube. Writes <Directory>/<Name>_sphere.png and _cube.png. Nothing is saved or uploaded. Logs MANTLE_MATERIAL_SHOT lines.
#include "MantleAssetLibrary.h"
#include "MantleCosmeticThumbnail.h"
#include "MantlePng.h"

#include "Engine/StaticMesh.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogMantleMaterialShot, Log, All);

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleDevMaterialShotPrivate
{
	void Run(const TArray<FString>& Args)
	{
		if (Args.Num() < 2)
		{
			UE_LOG(LogMantleMaterialShot, Error, TEXT("MANTLE_MATERIAL_SHOT usage: Mantle.Dev.MaterialShot <MaterialId or asset path> <Directory> [size]"));
			return;
		}
		const int32 Size = Args.Num() > 2 ? FMath::Clamp(FCString::Atoi(*Args[2]), 128, 2048) : 1024;
		FString Why;
		UMaterialInterface* Material = Args[0].StartsWith(TEXT("ast_")) ? FMantleAssetLibrary::Get().LoadMaterial(Args[0], Why) : LoadObject<UMaterialInterface>(nullptr, *Args[0]);
		FString WhyNot;
		if (!Material || !FMantleCosmeticThumbnail::CanCapture(&WhyNot))
		{
			UE_LOG(LogMantleMaterialShot, Error, TEXT("MANTLE_MATERIAL_SHOT failed: no material at %s (%s), or nothing can render (%s)"), *Args[0], *Why, *WhyNot);
			return;
		}
		// A material nobody has drawn yet compiles in the background and draws as the engine's grid until it is done.
		Material->EnsureIsComplete();
		for (const TCHAR* Shape : { TEXT("Sphere"), TEXT("Cube") })
		{
			UStaticMesh* Source = LoadObject<UStaticMesh>(nullptr, *FString::Printf(TEXT("/Engine/BasicShapes/%s.%s"), Shape, Shape));
			if (!Source)
			{
				continue;
			}
			UStaticMesh* Copy = DuplicateObject<UStaticMesh>(Source, GetTransientPackage());
			Copy->SetMaterial(0, Material);
			FMantleThumbnailSubject Shot;
			Shot.StaticMesh = Copy;
			Shot.KeepAlive.Add(TStrongObjectPtr<UObject>(Copy));
			Shot.Frame = Copy->GetBounds().GetBox();
			Shot.View = FRotator(-20.0, 145.0, 0.0);
			Shot.Describe = FString::Printf(TEXT("%s on a %s"), *Material->GetName(), Shape);
			TArray<FColor> Pixels;
			FString Error;
			const FString File = FPaths::Combine(Args[1], FString::Printf(TEXT("%s_%s.png"), *Material->GetName(), Shape));
			const bool bWritten = FMantleCosmeticThumbnail::Capture(Shot, Size, Pixels, Error) && FFileHelper::SaveArrayToFile(FMantlePng::EncodeRGBA8(Size, Size, Pixels), *File);
			UE_LOG(LogMantleMaterialShot, Display, TEXT("MANTLE_MATERIAL_SHOT %s %s"), bWritten ? *File : TEXT("FAILED"), bWritten ? TEXT("") : *Error);
		}
		UE_LOG(LogMantleMaterialShot, Display, TEXT("MANTLE_MATERIAL_SHOT done"));
	}

	FAutoConsoleCommand GMantleDevMaterialShotCommand(
		TEXT("Mantle.Dev.MaterialShot"),
		TEXT("Mantle.Dev.MaterialShot <MaterialId or asset path> <Directory> [size] -- draws a built-in material (ast_mat_carbon, ...) on a sphere and a cube as the catalog draws a thumbnail"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&Run));
}
