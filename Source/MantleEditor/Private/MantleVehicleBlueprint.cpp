// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleBlueprint.h"

#include "MantleVehicleBuilder.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "IMeshMergeUtilities.h"
#include "MeshMerge/MeshMergingSettings.h"
#include "MeshMergeModule.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "ObjectTools.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace MantleVehicleBlueprintPrivate
{
	/** A scratch world the Blueprint is spawned into and that goes away with this object, so nothing is left in the creator's level. */
	struct FScratchWorld
	{
		UWorld* World = nullptr;

		FScratchWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Inactive, /*bInformEngineOfWorld*/ false, TEXT("MantleVehicleBlueprintRead"));
		}

		~FScratchWorld()
		{
			if (World)
			{
				World->DestroyWorld(/*bInformEngineOfWorld*/ false);
				World->RemoveFromRoot();
			}
		}
	};

	/** The mesh pieces a creator sees on the vehicle: not scenery copies, not hidden in game. */
	bool IsVisiblePiece(const UStaticMeshComponent& Piece)
	{
		return Piece.GetStaticMesh() && !Piece.IsA<UInstancedStaticMeshComponent>() && !Piece.bHiddenInGame && Piece.IsVisible();
	}
}

FString FMantleVehicleBlueprint::MergeFolder()
{
	return FMantleVehicleBuilder::ImportFolder();
}

FMantleVehicleBlueprintModel FMantleVehicleBlueprint::Resolve(UBlueprint* Blueprint, const FString& Folder, const bool bSave)
{
	using namespace MantleVehicleBlueprintPrivate;
	FMantleVehicleBlueprintModel Out;
	if (!Blueprint || !Blueprint->GeneratedClass || !Blueprint->GeneratedClass->IsChildOf(AActor::StaticClass()))
	{
		Out.Problems.Add(TEXT("Pick a vehicle Blueprint (an Actor or Pawn Blueprint), or a mesh."));
		return Out;
	}

	FScratchWorld Scratch;
	if (!Scratch.World)
	{
		Out.Problems.Add(TEXT("Mantle could not open this Blueprint to read its meshes. Pick its mesh instead."));
		return Out;
	}
	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Spawn.ObjectFlags = RF_Transient;
	AActor* Actor = Scratch.World->SpawnActor<AActor>(Blueprint->GeneratedClass, FTransform::Identity, Spawn);
	if (!Actor)
	{
		Out.Problems.Add(FString::Printf(TEXT("Mantle could not open %s to read its meshes. Pick its mesh instead."), *Blueprint->GetName()));
		return Out;
	}

	TArray<UActorComponent*> Components;
	Actor->GetComponents(Components);

	// A skeletal body (a Chaos vehicle): the root component when it is one, else the first with a mesh, as a vehicle placed in the level is read.
	const USkeletalMeshComponent* Body = Cast<USkeletalMeshComponent>(Actor->GetRootComponent());
	if (!Body || !Body->GetSkeletalMeshAsset())
	{
		Body = nullptr;
		for (const UActorComponent* Component : Components)
		{
			const USkeletalMeshComponent* Candidate = Cast<USkeletalMeshComponent>(Component);
			if (Candidate && Candidate->GetSkeletalMeshAsset())
			{
				Body = Candidate;
				break;
			}
		}
	}
	if (Body)
	{
		const TArray<const UActorComponent*> ConstComponents(Components);
		Out.Model = Body->GetSkeletalMeshAsset();
		Out.Around = FMantleVehicleConverter::SurroundingsOfComponent(*Body, ConstComponents);
		Out.Notes.Add(FString::Printf(TEXT("Used the body of %s, with the wheels and parts set up on it."), *Blueprint->GetName()));
		return Out;
	}

	// Static pieces: one piece is the model as it is; several are merged into one Static Mesh.
	TArray<UStaticMeshComponent*> Pieces;
	for (UActorComponent* Component : Components)
	{
		UStaticMeshComponent* Piece = Cast<UStaticMeshComponent>(Component);
		if (Piece && IsVisiblePiece(*Piece))
		{
			Pieces.Add(Piece);
		}
	}
	if (Pieces.Num() == 0)
	{
		Out.Problems.Add(FString::Printf(TEXT("%s has no mesh to use. Pick a Blueprint with a body mesh, or pick the mesh itself."), *Blueprint->GetName()));
		return Out;
	}
	if (Pieces.Num() == 1 && Pieces[0]->GetComponentTransform().Equals(FTransform::Identity))
	{
		Out.Model = Pieces[0]->GetStaticMesh();
		Out.Notes.Add(FString::Printf(TEXT("Used the mesh of %s."), *Blueprint->GetName()));
		return Out;
	}

	IMeshMergeModule* MergeModule = FModuleManager::LoadModulePtr<IMeshMergeModule>("MeshMergeUtilities");
	if (!MergeModule)
	{
		Out.Problems.Add(TEXT("The editor's mesh merging is not available here. Merge the pieces in Unreal (Actor > Merge Actors), then pick the result."));
		return Out;
	}

	// The editor's own merge (Merge Actors): the pieces stay where the Blueprint puts them, every piece keeps its materials, and only the closest detail level is used.
	FMeshMergingSettings Settings;
	Settings.PivotType = EMeshMergePivotType::WorldOrigin;
	Settings.bMergeMaterials = false;
	Settings.bMergePhysicsData = false;
	Settings.bBakeVertexDataToMesh = false;
	Settings.bGenerateLightMapUV = false;
	Settings.bUseLandscapeCulling = false;
	Settings.bIncludeImposters = false;
	Settings.LODSelectionType = EMeshLODSelectionType::SpecificLOD;
	Settings.SpecificLOD = 0;

	const FString Base = (Folder.IsEmpty() ? MergeFolder() / ObjectTools::SanitizeObjectName(Blueprint->GetName()) : Folder) / ObjectTools::SanitizeObjectName(Blueprint->GetName());
	TArray<UPrimitiveComponent*> ToMerge;
	for (UStaticMeshComponent* Piece : Pieces)
	{
		ToMerge.Add(Piece);
	}
	TArray<UObject*> Made;
	FVector MergedLocation = FVector::ZeroVector;
	MergeModule->GetUtilities().MergeComponentsToStaticMesh(ToMerge, Scratch.World, Settings, nullptr, nullptr, Base, Made, MergedLocation, TNumericLimits<float>::Max(), /*bSilent*/ true);

	UStaticMesh* Merged = nullptr;
	for (UObject* Object : Made)
	{
		if (!Merged)
		{
			Merged = Cast<UStaticMesh>(Object);
		}
	}
	if (!Merged)
	{
		FString Names;
		for (const UStaticMeshComponent* Piece : Pieces)
		{
			Names += (Names.IsEmpty() ? TEXT("") : TEXT(", ")) + Piece->GetStaticMesh()->GetName();
		}
		Out.Problems.Add(FString::Printf(TEXT("Mantle could not merge the pieces of %s into one model (%s). Merge them in Unreal (Actor > Merge Actors) or in your 3D tool, then pick the result."), *Blueprint->GetName(), *Names));
		return Out;
	}

	Out.MadeAssets = Made;
	Out.Model = Merged;
	Out.bMerged = true;
	for (UObject* Object : Made)
	{
		if (Object && Object->GetOutermost() != GetTransientPackage())
		{
			Object->MarkPackageDirty();
		}
	}
	if (bSave)
	{
		for (UObject* Object : Made)
		{
			UPackage* Package = Object ? Object->GetOutermost() : nullptr;
			if (!Package || Package == GetTransientPackage())
			{
				continue;
			}
			FSavePackageArgs Args;
			Args.TopLevelFlags = RF_Public | RF_Standalone;
			const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
			if (!UPackage::SavePackage(Package, Object, *File, Args))
			{
				Out.Notes.Add(TEXT("Couldn't save the merged model automatically. Save it from the Content Browser."));
				break;
			}
		}
	}
	Out.Notes.Add(FString::Printf(TEXT("%s is made of %d pieces. Mantle merged them into one model (%s), keeping each piece's material."), *Blueprint->GetName(), Pieces.Num(), *Merged->GetPathName()));
	Out.Notes.Add(TEXT("Pieces that move on their own, like a turret or a door, become one solid piece in the merged model."));
	return Out;
}
