// Copyright Joshua Eger. All Rights Reserved.
//
// Maintainer command: Mantle.Dev.ListBodyParts <FolderPath>
// Logs, for each skeletal mesh directly in the folder, what the split-body search reads: its material sections and how
// they classify (skin or not), its size (cm, from the vertices), its bone count and its skeleton. MANTLE_BODY_PARTS lines.
#include "MantleCosmeticExport.h"
#include "MantleFitBodies.h"
#include "MantleOutfitSections.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/IConsoleManager.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogMantleListBodyParts, Log, All);

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleDevListBodyPartsPrivate
{
	void Run(const TArray<FString>& Args)
	{
		if (Args.IsEmpty())
		{
			UE_LOG(LogMantleListBodyParts, Error, TEXT("MANTLE_BODY_PARTS usage: Mantle.Dev.ListBodyParts <FolderPath>"));
			return;
		}
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		FARFilter Filter;
		Filter.PackagePaths.Add(FName(*Args[0]));
		Filter.ClassPaths.Add(USkeletalMesh::StaticClass()->GetClassPathName());
		TArray<FAssetData> Meshes;
		Registry.GetAssets(Filter, Meshes);
		for (const FAssetData& Data : Meshes)
		{
			const USkeletalMesh* Mesh = Cast<USkeletalMesh>(Data.GetAsset());
			if (!Mesh) { continue; }
			TArray<int32> Slots;
			const TArray<FMantleOutfitSection> Sections = FMantleCosmeticExport::ReadOutfitSections(Mesh, 0, Slots);
			FString Described;
			for (const FMantleOutfitSection& Section : Sections)
			{
				Described += FString::Printf(TEXT("%s/%s=%s "), *Section.SlotName, Section.MaterialName.IsEmpty() ? TEXT("-") : *Section.MaterialName,
					FMantleOutfitSections::Classify(Section) == EMantleOutfitPiece::Skin ? TEXT("skin") : TEXT("other"));
			}
			TArray<FVector3f> Points;
			FBox3f Box(ForceInit);
			if (FMantleFitBodies::ReadVertices(Mesh, 0, Points)) { for (const FVector3f& Point : Points) { Box += Point; } }
			UE_LOG(LogMantleListBodyParts, Display, TEXT("MANTLE_BODY_PARTS %s verts=%d size=%s min=%s bones=%d skeleton=%s sections: %s"), *Mesh->GetName(), Points.Num(),
				*Box.GetSize().ToString(), *Box.Min.ToString(), Mesh->GetRefSkeleton().GetRawBoneNum(), Mesh->GetSkeleton() ? *Mesh->GetSkeleton()->GetPathName() : TEXT("none"), *Described);
		}
		UE_LOG(LogMantleListBodyParts, Display, TEXT("MANTLE_BODY_PARTS done"));
	}

	FAutoConsoleCommand GMantleDevListBodyPartsCommand(
		TEXT("Mantle.Dev.ListBodyParts"),
		TEXT("Mantle.Dev.ListBodyParts <FolderPath> -- logs each skeletal mesh in the folder: sections (skin or not), size, bones, skeleton."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&Run));
}
