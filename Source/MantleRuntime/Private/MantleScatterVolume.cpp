// Copyright Joshua Eger. All Rights Reserved.
#include "MantleScatterVolume.h"

#include "MantleAssetLibrary.h"
#include "MantlePlaceLimits.h"
#include "MantleRuntime.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"
#include "Misc/CoreMisc.h"

#if WITH_EDITORONLY_DATA
#include "Components/BoxComponent.h"
#endif

// The Details panel's ClampMin/ClampMax metas are literals; these keep them on the place limits.
static_assert(FMantlePlaceLimits::MaxScatterHalfExtent == 1000000.0, "update AMantleScatterVolume::HalfExtent's metas");
static_assert(FMantlePlaceLimits::MinScatterScale == 0.001 && FMantlePlaceLimits::MaxScatterScale == 65.535,
	"update AMantleScatterVolume::MinScale's and MaxScale's metas");
static_assert(FMantlePlaceLimits::MaxScatterTilt == 90.0, "update AMantleScatterVolume::MaxTiltDegrees' metas");
static_assert(FMantlePlaceLimits::MaxScatterMeshWeight == 1000, "update FMantleScatterMeshSettings::Weight's metas");

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleScatterVolumePrivate
{
	/** What a mesh is called in a message: its asset's name, else the id it was given. */
	FString ScatterMeshLabel(const FMantleScatterMeshSettings& Entry, int32 Index)
	{
		if (Entry.Mesh)
		{
			return FString::Printf(TEXT("mesh %d (%s)"), Index + 1, *Entry.Mesh->GetName());
		}
		if (!Entry.MeshId.IsEmpty())
		{
			return FString::Printf(TEXT("mesh %d (%s)"), Index + 1, *Entry.MeshId);
		}
		return FString::Printf(TEXT("mesh %d"), Index + 1);
	}
}

AMantleScatterVolume::AMantleScatterVolume()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = false;
	SetCanBeDamaged(false);

	ScatterRoot = CreateDefaultSubobject<USceneComponent>(TEXT("ScatterRoot"));
	// Placed scenery: Static in a level. The place builder makes it Movable while it places it and
	// freezes it again afterwards, as it does place geometry.
	ScatterRoot->SetMobility(EComponentMobility::Static);
	SetRootComponent(ScatterRoot);

#if WITH_EDITORONLY_DATA
	EditorBounds = CreateEditorOnlyDefaultSubobject<UBoxComponent>(TEXT("EditorBounds"));
	if (EditorBounds)
	{
		EditorBounds->SetupAttachment(ScatterRoot);
		EditorBounds->SetMobility(EComponentMobility::Static);
		EditorBounds->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
		EditorBounds->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		EditorBounds->SetGenerateOverlapEvents(false);
		EditorBounds->SetCanEverAffectNavigation(false);
		EditorBounds->SetIsVisualizationComponent(true);
		EditorBounds->SetHiddenInGame(true);
		EditorBounds->ShapeColor = FColor(80, 200, 120);
		EditorBounds->InitBoxExtent(FVector(HalfExtent.X, HalfExtent.Y, FMath::Max(HalfExtent.Z, 1.0)));
	}
#endif
}

bool AMantleScatterVolume::IsEditorWorld() const
{
	const UWorld* World = GetWorld();
	return World && !World->IsGameWorld();
}

void AMantleScatterVolume::ApplyProps(const FMantleScatterParams& In)
{
	Seed = In.Seed;
	HalfExtent = In.HalfExtent;
	Density = In.Density;
	MinScale = In.MinScale;
	MaxScale = In.MaxScale;
	Alignment = In.Alignment;
	MaxTiltDegrees = In.MaxTiltDegrees;
	Meshes.Reset(In.Meshes.Num());
	for (const FMantleScatterMesh& Mesh : In.Meshes)
	{
		FMantleScatterMeshSettings& Entry = Meshes.AddDefaulted_GetRef();
		Entry.Weight = Mesh.Weight;
		Entry.MeshId = Mesh.MeshId;
		Entry.MaterialIds = Mesh.Materials;
	}
	SyncEditorBounds();
}

void AMantleScatterVolume::LoadAssetsFromIds(TArray<FMantleParseError>& OutErrors)
{
	const FMantleAssetLibrary& Library = FMantleAssetLibrary::Get();
	for (int32 Index = 0; Index < Meshes.Num(); ++Index)
	{
		FMantleScatterMeshSettings& Entry = Meshes[Index];
		const FString EntryPath = FString::Printf(TEXT("/meshes/%d"), Index);
		if (!Entry.MeshId.IsEmpty())
		{
			FString Error;
			Entry.Mesh = Library.LoadMesh(Entry.MeshId, Error);
			if (!Entry.Mesh)
			{
				OutErrors.Add(FMantleParseError{ EntryPath + TEXT("/mesh"), FString::Printf(TEXT("'%s': %s"), *Entry.MeshId, *Error) });
			}
		}
		Entry.Materials.Reset(Entry.MaterialIds.Num());
		for (int32 Slot = 0; Slot < Entry.MaterialIds.Num(); ++Slot)
		{
			UMaterialInterface* Material = nullptr;
			if (!Entry.MaterialIds[Slot].IsEmpty())
			{
				FString Error;
				Material = Library.LoadMaterial(Entry.MaterialIds[Slot], Error);
				if (!Material)
				{
					OutErrors.Add(FMantleParseError{ FString::Printf(TEXT("%s/materials/%d"), *EntryPath, Slot),
						FString::Printf(TEXT("'%s': %s"), *Entry.MaterialIds[Slot], *Error) });
				}
			}
			Entry.Materials.Add(Material);
		}
	}
}

bool AMantleScatterVolume::ReadProps(FMantleScatterParams& Out, FString& OutProblem) const
{
	using namespace MantleScatterVolumePrivate;

	Out = FMantleScatterParams();
	Out.Seed = Seed;
	Out.HalfExtent = HalfExtent;
	Out.Density = Density;
	Out.MinScale = MinScale;
	Out.MaxScale = MaxScale;
	Out.Alignment = Alignment;
	Out.MaxTiltDegrees = MaxTiltDegrees;

	bool bOk = true;
	for (int32 Index = 0; Index < Meshes.Num(); ++Index)
	{
		const FMantleScatterMeshSettings& Entry = Meshes[Index];
		FMantleScatterMesh& Mesh = Out.Meshes.AddDefaulted_GetRef();
		Mesh.Weight = Entry.Weight;
		if (Entry.Mesh)
		{
			const FString LibraryId = FMantleAssetLibrary::Get().FindIdByPath(FSoftObjectPath(Entry.Mesh.Get()));
			Mesh.MeshId = !LibraryId.IsEmpty() ? LibraryId : !Entry.MeshId.IsEmpty() ? Entry.MeshId : Entry.Mesh->GetPathName();
		}
		else
		{
			Mesh.MeshId = Entry.MeshId;
		}
		Mesh.Materials = Entry.MaterialIds;
		if (bOk && Mesh.MeshId.IsEmpty())
		{
			OutProblem = FString::Printf(TEXT("has no mesh in %s; pick one, or remove the entry"), *ScatterMeshLabel(Entry, Index));
			bOk = false;
		}
	}
	if (!bOk)
	{
		return false;
	}
	if (Meshes.Num() == 0)
	{
		OutProblem = FString::Printf(TEXT("has no meshes; add 1 to %d under Meshes"), FMantleScatter::MaxMeshes);
		return false;
	}
	FString Why;
	if (FMantleScatter::CountFor(Out, Why) < 0)
	{
		OutProblem = FString::Printf(TEXT("cannot scatter: %s"), *Why);
		return false;
	}
	return true;
}

bool AMantleScatterVolume::IsScatterComponent(const UActorComponent* Component) const
{
	return Component && ScatterComponents.ContainsByPredicate(
		[Component](const TObjectPtr<UInstancedStaticMeshComponent>& Candidate) { return Candidate.Get() == Component; });
}

void AMantleScatterVolume::ClearScatter()
{
	for (UInstancedStaticMeshComponent* Component : ScatterComponents)
	{
		if (IsValid(Component))
		{
			RemoveInstanceComponent(Component);
			Component->DestroyComponent();
		}
	}
	ScatterComponents.Reset();
	BuiltInstances = 0;
	BuiltHash = 0;
	PreviewKey.Reset();
}

bool AMantleScatterVolume::RefreshScatter(FString& OutError)
{
	ClearScatter();
	UWorld* World = GetWorld();
	if (!FMantleScatter::DrawsScatter(World))
	{
		return true; // visual only: a dedicated server has nothing to build
	}

	FMantleScatterParams Params;
	if (!ReadProps(Params, OutError))
	{
		return false;
	}
	TArray<FMantleScatterInstance> Instances;
	if (!FMantleScatter::Generate(Params, Instances, OutError))
	{
		return false;
	}

	// Sort the instances into one transform list per mesh, sized first so nothing regrows. A mesh that
	// did not load (reported when it was loaded) leaves its instances out; the rest still draw.
	TArray<int32, TInlineAllocator<FMantleScatter::MaxMeshes>> Counts;
	Counts.SetNumZeroed(Meshes.Num());
	for (const FMantleScatterInstance& Instance : Instances)
	{
		++Counts[Instance.Mesh];
	}
	TArray<TArray<FTransform>> Buckets;
	Buckets.SetNum(Meshes.Num());
	for (int32 MeshIndex = 0; MeshIndex < Meshes.Num(); ++MeshIndex)
	{
		if (Meshes[MeshIndex].Mesh)
		{
			Buckets[MeshIndex].Reserve(Counts[MeshIndex]);
		}
	}
	for (const FMantleScatterInstance& Instance : Instances)
	{
		if (Meshes[Instance.Mesh].Mesh)
		{
			Buckets[Instance.Mesh].Add(FMantleScatter::ToTransform(Instance));
		}
	}

	const bool bPreview = IsEditorWorld();
	const EComponentMobility::Type Mobility = ScatterRoot ? ScatterRoot->GetMobility() : EComponentMobility::Movable;
	for (int32 MeshIndex = 0; MeshIndex < Meshes.Num(); ++MeshIndex)
	{
		const FMantleScatterMeshSettings& Entry = Meshes[MeshIndex];
		if (!Entry.Mesh || Buckets[MeshIndex].Num() == 0)
		{
			continue;
		}
		UInstancedStaticMeshComponent* Ism = NewObject<UInstancedStaticMeshComponent>(this, NAME_None,
			RF_Transient | RF_TextExportTransient | RF_DuplicateTransient);
		// In a level being edited it is the preview: never published, never saved, never cooked.
		Ism->bIsEditorOnly = bPreview;
		Ism->SetupAttachment(ScatterRoot);
		Ism->SetMobility(Mobility);
		Ism->SetStaticMesh(Entry.Mesh);
		for (int32 Slot = 0; Slot < Entry.Materials.Num(); ++Slot)
		{
			if (Entry.Materials[Slot])
			{
				Ism->SetMaterial(Slot, Entry.Materials[Slot]);
			}
		}
		// Visual only: no physics bodies, no overlaps, nothing for navigation.
		Ism->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
		Ism->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Ism->SetGenerateOverlapEvents(false);
		Ism->SetCanEverAffectNavigation(false);
		// Instances go in before the component registers, so its render state is built once for all
		// of them, as the place builder does for an InstancedMesh.
		Ism->PreAllocateInstancesMemory(Buckets[MeshIndex].Num());
		Ism->AddInstances(Buckets[MeshIndex], /*bShouldReturnIndices*/ false, /*bWorldSpace*/ false, /*bUpdateNavigation*/ false);
		Ism->RegisterComponent();
		// Listed as the actor's own, so the Details panel and the world outliner show it.
		AddInstanceComponent(Ism);
		ScatterComponents.Add(Ism);
	}
	BuiltInstances = Instances.Num();
	BuiltHash = FMantleScatter::Hash(Instances);
	return true;
}

FString AMantleScatterVolume::SettingsKey() const
{
	FString Key = FString::Printf(TEXT("%d|%.17g,%.17g,%.17g|%.17g|%.17g,%.17g|%d|%.17g"),
		Seed, HalfExtent.X, HalfExtent.Y, HalfExtent.Z, Density, MinScale, MaxScale, static_cast<int32>(Alignment), MaxTiltDegrees);
	for (const FMantleScatterMeshSettings& Entry : Meshes)
	{
		Key += FString::Printf(TEXT("|%s:%d:%s"), Entry.Mesh ? *Entry.Mesh->GetPathName() : TEXT("-"), Entry.Weight, *Entry.MeshId);
		for (const UMaterialInterface* Material : Entry.Materials)
		{
			Key += Material ? Material->GetPathName() : FString(TEXT("-"));
			Key += TEXT(";");
		}
	}
	const EComponentMobility::Type Mobility = ScatterRoot ? ScatterRoot->GetMobility() : EComponentMobility::Movable;
	Key +=FString::Printf(TEXT("|m%d"), static_cast<int32>(Mobility));
	return Key;
}

void AMantleScatterVolume::SyncEditorBounds()
{
#if WITH_EDITORONLY_DATA
	if (EditorBounds)
	{
		// A flat layer still shows as a box a centimetre deep.
		EditorBounds->SetBoxExtent(FVector(HalfExtent.X, HalfExtent.Y, FMath::Max(HalfExtent.Z, 1.0)), /*bUpdateOverlaps*/ false);
	}
#endif
}

void AMantleScatterVolume::RefreshEditorPreview()
{
	SyncEditorBounds();
	if (!IsEditorWorld() || IsRunningCommandlet())
	{
		return;
	}
	const FString Key = SettingsKey();
	if (Key.Equals(PreviewKey, ESearchCase::CaseSensitive) && ScatterComponents.Num() > 0)
	{
		return; // moved, not changed: the instances follow the root
	}
	FString Error;
	if (!RefreshScatter(Error))
	{
		// The settings can't scatter yet (no mesh picked, say); Check this place names the fix.
		UE_LOG(LogMantle, Verbose, TEXT("MANTLE_SCATTER preview of %s: %s"), *GetName(), *Error);
		return;
	}
	PreviewKey = Key;
}

void AMantleScatterVolume::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	if (IsEditorWorld())
	{
		RefreshEditorPreview();
	}
}

void AMantleScatterVolume::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();
	// The preview's own components are made on demand; ones already there are left as they are.
	if (IsEditorWorld() && ScatterComponents.Num() == 0)
	{
		RefreshEditorPreview();
	}
}

#if WITH_EDITOR
void AMantleScatterVolume::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	RefreshEditorPreview();
}
#endif
