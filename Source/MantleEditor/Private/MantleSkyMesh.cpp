// Copyright Joshua Eger. All Rights Reserved.
#include "MantleSkyMesh.h"

#include "MantlePlaceExporter.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleSkyMeshPrivate
{
	/** The engine's sky sphere Blueprint, or a Blueprint made from it. */
	bool IsEngineSkySphere(const AActor* Actor)
	{
		for (const UClass* Class = Actor ? Actor->GetClass() : nullptr; Class; Class = Class->GetSuperClass())
		{
			if (Class->GetPathName().StartsWith(TEXT("/Engine/EngineSky/")))
			{
				return true;
			}
		}
		return false;
	}

	bool NamedLikeSky(const FString& Name)
	{
		return Name.Contains(TEXT("sky"), ESearchCase::IgnoreCase) || Name.Contains(TEXT("cloud"), ESearchCase::IgnoreCase);
	}

	/** Whether the component, its actor (label or class) or the Blueprint it came from is named like a sky. */
	bool IsNamedLikeSky(const UStaticMeshComponent* Component)
	{
		if (NamedLikeSky(Component->GetName()))
		{
			return true;
		}
		const AActor* Owner = Component->GetOwner();
		return Owner && (NamedLikeSky(Owner->GetActorLabel()) || NamedLikeSky(Owner->GetClass()->GetName()));
	}

	/** What the component's materials say about it. */
	struct FSkyLook
	{
		bool bIsSkyFlag = false;
		bool bUnlit = false;
		bool bTwoSided = false;
	};

	FSkyLook ReadLook(const UStaticMeshComponent* Component)
	{
		FSkyLook Look;
		for (int32 Slot = 0; Slot < Component->GetNumMaterials(); ++Slot)
		{
			const UMaterialInterface* Material = Component->GetMaterial(Slot);
			if (!Material)
			{
				continue;
			}
			if (const UMaterial* Base = Material->GetMaterial())
			{
				Look.bIsSkyFlag |= Base->bIsSky != 0;
			}
			Look.bUnlit |= Material->GetShadingModels().HasOnlyShadingModel(MSM_Unlit);
			Look.bTwoSided |= Material->IsTwoSided();
		}
		return Look;
	}

	/**
	 * Whether Box holds the centre of every other mesh in the level that is smaller than it. Meshes as big as the
	 * candidate (another sky layer, a landscape-sized backdrop) say nothing about what it encloses, and a level with
	 * no other mesh is enclosed by anything.
	 */
	bool EnclosesTheLevel(const UStaticMeshComponent* Component, const FBox& Box)
	{
		const UWorld* World = Component->GetWorld();
		if (!World)
		{
			return true;
		}
		const double CandidateSize = Box.GetExtent().GetMax();
		const FBox Roomy = Box.ExpandBy(Box.GetExtent() * 0.05);
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			const AActor* Other = *It;
			if (!IsValid(Other) || Other == Component->GetOwner() || Other->IsHidden())
			{
				continue;
			}
			TArray<UStaticMeshComponent*> Meshes;
			Other->GetComponents(Meshes);
			for (const UStaticMeshComponent* Mesh : Meshes)
			{
				if (!Mesh || !Mesh->GetStaticMesh() || !Mesh->IsVisible() || Mesh->bHiddenInGame)
				{
					continue;
				}
				const FBox Theirs = Mesh->CalcBounds(Mesh->GetComponentTransform()).GetBox();
				if (Theirs.GetExtent().GetMax() >= CandidateSize)
				{
					continue;
				}
				if (!Roomy.IsInside(Theirs.GetCenter()))
				{
					return false;
				}
			}
		}
		return true;
	}
}

bool FMantleSkyMesh::IsSkyMesh(const UStaticMeshComponent* Component, FString* OutWhy)
{
	using namespace MantleSkyMeshPrivate;
	if (!Component || !Component->GetStaticMesh())
	{
		return false;
	}
	auto Say = [OutWhy](const TCHAR* Why)
	{
		if (OutWhy)
		{
			*OutWhy = Why;
		}
		return true;
	};

	if (IsEngineSkySphere(Component->GetOwner()))
	{
		return Say(TEXT("it belongs to the engine's sky sphere"));
	}

	const FSkyLook Look = ReadLook(Component);
	if (Look.bIsSkyFlag)
	{
		return Say(TEXT("its material is flagged Is Sky"));
	}

	const FBox Box = Component->CalcBounds(Component->GetComponentTransform()).GetBox();
	if (Box.GetExtent().GetMax() < MinHalfExtent)
	{
		return false;
	}
	const bool bLooksLikeSky = (IsNamedLikeSky(Component) && (Look.bUnlit || Look.bTwoSided)) || (Look.bUnlit && Look.bTwoSided);
	if (bLooksLikeSky && EnclosesTheLevel(Component, Box))
	{
		return Say(TEXT("it is a huge unlit or two-sided mesh, named like a sky, around the whole level"));
	}
	return false;
}

bool FMantleSkyMesh::IsSkyMeshActor(const AActor* Actor)
{
	const AStaticMeshActor* MeshActor = IsValid(Actor) ? Cast<AStaticMeshActor>(Actor) : nullptr;
	if (!MeshActor)
	{
		return false;
	}
	// A mesh actor with more beside its mesh is a Group of parts (ClassifyActor); the sky mesh among them is left out as a part.
	EMantleEntityType Type;
	return FMantlePlaceExporter::ClassifyActor(Actor, Type) && Type == EMantleEntityType::StaticMesh && IsSkyMesh(MeshActor->GetStaticMeshComponent());
}

FString FMantleSkyMesh::StandaloneDetail()
{
	return TEXT("a sky mesh; Mantle supplies its own sky, so it isn't published.");
}

FString FMantleSkyMesh::ComponentDetail(const FString& ComponentName)
{
	return FString::Printf(TEXT("has a sky mesh ('%s'); Mantle supplies its own sky, so it isn't published."), *ComponentName);
}
