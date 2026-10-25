// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "MantleChassisSpec.h"
#include "MantleVehicleAsset.h"
#include "MantleVehicleChassisBuilder.h"
#include "MantleVehicleChassisMesh.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// A vehicle body for the editor tests that need a real skeletal mesh with wheel bones: the sedan preset's grey chassis, saved as a mesh in a
// throwaway /Temp package, nothing of it on disk. This needs nothing outside the plugin, so the tests cannot silently skip for missing content.
// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVehicleScratchBody
{
	/** The sedan chassis's body mesh in the scratch package PackagePath ("/Temp/..."), its bones at the wheels. Null with the reason. */
	inline USkeletalMesh* Make(const FString& PackagePath, const FString& Name, FString& OutError)
	{
		FMantleChassisSpec Spec;
		FMantleVehicleAsset Asset;
		if (!FMantleVehicleChassisBuilder::LoadSpec(TEXT("sedan"), Spec, OutError) || !FMantleVehicleChassisBuilder::MakeAsset(Spec, Asset, OutError))
		{
			return nullptr;
		}
		return FMantleVehicleChassisMesh::Create(Asset.Body, PackagePath, Name, OutError);
	}

	/** Drops a mesh Make made and its skeleton, so nothing stays in the editor. */
	inline void Discard(USkeletalMesh* Mesh)
	{
		if (!Mesh)
		{
			return;
		}
		TArray<UObject*> Assets = { Mesh };
		if (USkeleton* Skeleton = Mesh->GetSkeleton())
		{
			Assets.Add(Skeleton);
		}
		for (UObject* Asset : Assets)
		{
			UPackage* Package = Asset->GetOutermost();
			Asset->ClearFlags(RF_Public | RF_Standalone);
			Asset->MarkAsGarbage();
			if (Package && Package != GetTransientPackage())
			{
				Package->ClearDirtyFlag();
				Package->MarkAsGarbage();
			}
		}
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS
