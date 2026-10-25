// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleAvatarContract.h"
#include "MantleCosmeticExport.h"

/** Creator corrections, keyed by source object path and accessory slot; never alter the source mesh. */
struct FMantleAccessorySettings
{
	static FString DefaultConfigFile();
	/** Returns whether this exact source/slot has saved settings, including an explicitly disabled correction. */
	static bool Load(FMantleCosmeticDraft& Draft, const FString& ConfigFile);
	static void Save(const FMantleCosmeticDraft& Draft, const FString& ConfigFile);
	/** Align standard Unreal component-space mesh axes to this avatar's neutral anchor, preserving location and scale. */
	static bool AlignToAvatarAxes(FMantleCosmeticDraft& Draft, const USkeletalMesh* Body, FString& OutError);

	/**
	 * The shift to take off every vertex so a mesh whose pivot is out of the slot's reach (a model exported with its pivot at
	 * the feet) is centred on its pivot, or zero when the pivot already reaches or the mesh is too big for the slot either way.
	 */
	static FVector3f RecenterOffset(const FBox3f& Bounds, const FMantleContractAccessoryBounds& Limits);

	/**
	 * The mesh that uploads for Source: Source itself within the class's triangle limit, otherwise the cached reduced copy
	 * the export makes. Source when it cannot be reduced. Never modifies Source.
	 */
	static const UStaticMesh* UploadMesh(const UStaticMesh* Source, const FMantleContractClass& Class);
};
