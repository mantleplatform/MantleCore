// Copyright Joshua Eger. All Rights Reserved.
//
// Maintainer command (#167): Mantle.Dev.MinimapDraw <Directory>
// Makes the open level's map picture as Publish does (every World Partition cell loaded; the drawn map, or the creator's own picture)
// and writes <Directory>/map.png and each tile, tile_<row>_<column>.png, as they would upload. Nothing is saved or uploaded.
// Logs MANTLE_MINIMAP_DRAW lines (the time, what was drawn, the grid and bounds). Needs no renderer: runs with -nullrhi.
#include "MantleMinimap.h"
#include "MantlePlaceWorldPartition.h"
#include "MantlePng.h"
#include "MantleRuntime.h"

#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleDevMinimapDrawPrivate
{
	void Run(const TArray<FString>& Args, UWorld* World)
	{
		if (!World || Args.Num() < 1)
		{
			UE_LOG(LogMantle, Error, TEXT("MANTLE_MINIMAP_DRAW_FAILED usage: Mantle.Dev.MinimapDraw <Directory> with a level open"));
			return;
		}
		const FString Directory = Args[0];
		IFileManager::Get().MakeDirectory(*Directory, true);
		const double Started = FPlatformTime::Seconds();
		FMantlePlaceWorldPartition::FLoadResult Loaded = FMantlePlaceWorldPartition::LoadAll(World);
		const double LoadSeconds = FPlatformTime::Seconds() - Started;
		FMantleMinimapPicture Picture;
		FString Error;
		const bool bMade = FMantleMinimap::Make(World, Picture, Error);
		FMantlePlaceWorldPartition::ReleaseLoaded(World, Loaded);
		if (!bMade)
		{
			UE_LOG(LogMantle, Error, TEXT("MANTLE_MINIMAP_DRAW_FAILED %s"), *Error);
			return;
		}
		UE_LOG(LogMantle, Log, TEXT("MANTLE_MINIMAP_DRAW %s (level load %.2f s not counted)"), *Picture.Source, LoadSeconds);
		UE_LOG(LogMantle, Log, TEXT("MANTLE_MINIMAP_DRAW_GRID cmPerPixel=%.3f pixels=%dx%d tiles=%dx%d tile=%dx%d bounds x=%.1f..%.1f y=%.1f..%.1f warning=%s"),
			Picture.Grid.CmPerPixel, Picture.Grid.Width, Picture.Grid.Height, Picture.Grid.Columns, Picture.Grid.Rows, Picture.Grid.TileWidth,
			Picture.Grid.TileHeight, Picture.Grid.MinX, Picture.Grid.MaxX, Picture.Grid.MinY, Picture.Grid.MaxY, *Picture.Warning);
		bool bSaved = FFileHelper::SaveArrayToFile(FMantlePng::EncodeRGBA8(Picture.Grid.Width, Picture.Grid.Height, Picture.Pixels),
			*FPaths::Combine(Directory, TEXT("map.png")));
		for (int32 Row = 0; Row < Picture.Grid.Rows; ++Row)
		{
			for (int32 Column = 0; Column < Picture.Grid.Columns; ++Column)
			{
				const TArray<uint8> Png = FMantlePng::EncodeRGBA8(Picture.Grid.TileWidth, Picture.Grid.TileHeight, Picture.Tile(Row, Column));
				const FString File = FPaths::Combine(Directory, FString::Printf(TEXT("tile_%d_%d.png"), Row, Column));
				bSaved &= FFileHelper::SaveArrayToFile(Png, *File);
				UE_LOG(LogMantle, Log, TEXT("MANTLE_MINIMAP_DRAW_TILE %s %d bytes"), *File, Png.Num());
			}
		}
		UE_LOG(LogMantle, Log, TEXT("MANTLE_MINIMAP_DRAW_DONE saved=%d"), bSaved ? 1 : 0);
	}

	FAutoConsoleCommandWithWorldAndArgs GMantleDevMinimapDrawCommand(
		TEXT("Mantle.Dev.MinimapDraw"),
		TEXT("Mantle.Dev.MinimapDraw <Directory> -- makes the open level's map picture as Publish does and writes map.png and its tiles. ")
		TEXT("Nothing is saved or uploaded. MANTLE_MINIMAP_DRAW lines."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			Run(Args, World);
		}));
}
