// Copyright Joshua Eger. All Rights Reserved.
#include "MantleScatterVolume.h"

#include "MantleAssetLibrary.h"
#include "MantleHeightfieldReader.h"
#include "MantlePlaceLimits.h"
#include "MantleRuntime.h"
#include "MantleShadowDistance.h"
#include "MantleTerrainGrass.h"
#include "MantleTerrainSurface.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"
#include "Misc/CoreMisc.h"
#include "Scalability.h"

#if WITH_EDITORONLY_DATA
#include "Components/BoxComponent.h"
#endif

// The Details panel's ClampMin/ClampMax metas are literals; these keep them on the place limits.
static_assert(FMantlePlaceLimits::MaxScatterHalfExtent == 1000000.0, "update AMantleScatterVolume::HalfExtent's metas");
static_assert(FMantlePlaceLimits::MinScatterScale == 0.001 && FMantlePlaceLimits::MaxScatterScale == 65.535,
	"update AMantleScatterVolume::MinScale's and MaxScale's metas");
static_assert(FMantlePlaceLimits::MaxScatterTilt == 90.0, "update AMantleScatterVolume::MaxTiltDegrees' metas");
static_assert(FMantlePlaceLimits::MaxScatterMeshWeight == 1000, "update FMantleScatterMeshSettings::Weight's metas");
static_assert(FMantlePlaceLimits::MaxDrawDistance == 1000000.0, "update AMantleScatterVolume::DrawDistance's metas");
static_assert(FMantlePlaceLimits::MaxScatterRadius == 1000000.0 && FMantlePlaceLimits::MaxScatterCoordinate == 1000000.0,
	"update AMantleScatterVolume::Radius's, MinHeight's and MaxHeight's metas");
static_assert(FMantlePlaceLimits::MaxScatterPathWidth == 100000.0 && FMantlePlaceLimits::MaxScatterClusterRadius == 100000.0
	&& FMantlePlaceLimits::MaxScatterClusters == 1000, "update AMantleScatterVolume::Width's, ClusterRadius' and ClusterCount's metas");

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleScatterVolumePrivate
{
	/** The share of a drawDistance at which instances start to fade out: the last fifth, as an InstancedMesh's (MantlePlaceBuilder.cpp). */
	constexpr double ScatterFadeStartShare = 0.8;

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
	DrawDistance = In.DrawDistance;
	Placement = In.Placement;
	MaxSlopeDegrees = In.MaxSlopeDegrees;
	bLimitMinHeight = In.MinHeight.IsSet();
	MinHeight = In.MinHeight.Get(MinHeight);
	bLimitMaxHeight = In.MaxHeight.IsSet();
	MaxHeight = In.MaxHeight.Get(MaxHeight);
	Shape = In.Shape;
	Radius = In.Shape == EMantleScatterShape::Disc ? In.Radius : Radius;
	if (In.Shape == EMantleScatterShape::Path)
	{
		Path = In.Path;
		Width = In.Width;
	}
	Exclusions.Reset(In.Exclusions.Num());
	for (const FMantleScatterExclusion& Exclusion : In.Exclusions)
	{
		FMantleScatterExclusionSettings& Entry = Exclusions.AddDefaulted_GetRef();
		Entry.Center = Exclusion.Center;
		Entry.HalfExtent = Exclusion.HalfExtent;
	}
	bCollision = In.bCollision;
	ClusterCount = In.ClusterCount;
	ClusterRadius = In.ClusterCount > 0 ? In.ClusterRadius : ClusterRadius;
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
	Out.DrawDistance = DrawDistance;
	Out.Placement = Placement;
	if (Placement == EMantleScatterPlacement::Terrain)
	{
		Out.MaxSlopeDegrees = MaxSlopeDegrees;
		if (bLimitMinHeight)
		{
			Out.MinHeight = MinHeight;
		}
		if (bLimitMaxHeight)
		{
			Out.MaxHeight = MaxHeight;
		}
	}
	Out.Shape = Shape;
	if (Shape == EMantleScatterShape::Disc)
	{
		Out.Radius = Radius;
	}
	else if (Shape == EMantleScatterShape::Path)
	{
		Out.Path = Path;
		Out.Width = Width;
	}
	for (const FMantleScatterExclusionSettings& Entry : Exclusions)
	{
		FMantleScatterExclusion& Exclusion = Out.Exclusions.AddDefaulted_GetRef();
		Exclusion.Center = Entry.Center;
		Exclusion.HalfExtent = Entry.HalfExtent;
	}
	Out.bCollision = bCollision;
	if (ClusterCount > 0)
	{
		Out.ClusterCount = ClusterCount;
		Out.ClusterRadius = ClusterRadius;
	}

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
	FMantleScatterParams Params;
	if (!ReadProps(Params, OutError))
	{
		return false;
	}
	// Visual scatters build only where something draws; a colliding one (#169) builds on a dedicated server too, since the
	// server's bodies are what stop a player and an NPC.
	if (!FMantleScatter::DrawsScatter(World) && !Params.bCollision)
	{
		return true;
	}
	// The editor preview shows every instance, as Epic does; a game takes Settings > Foliage's tier, as grass does.
	const bool bPreview = IsEditorWorld();
	// A colliding scatter is the same on every machine, whatever its Foliage setting: its bodies decide where players walk.
	const int32 FoliageQuality = (bPreview || Params.bCollision) ? 3 : Scalability::GetQualityLevels().FoliageQuality;
	if (!bPreview && !Params.bCollision)
	{
		BindScalability();
	}
	BuiltFoliageQuality = FoliageQuality;

	FMantleScatterGenerateOptions Options;
	const double TierDensity = FMantleScatter::FoliageTierDensity(FoliageQuality);
	if (TierDensity < 1.0)
	{
		// A prefix of each instance stream: a lower tier's instances are a subset of a higher one's.
		const int64 Count = FMantleScatter::CountFor(Params, OutError);
		if (Count < 0)
		{
			return false;
		}
		Options.MaxCandidates = static_cast<int64>(FMath::FloorToDouble(static_cast<double>(Count) * TierDensity + 0.5));
	}
	if (Params.Placement == EMantleScatterPlacement::Terrain)
	{
		Options.Ground = MakeGround();
	}

	TArray<FMantleScatterInstance> Instances;
	if (!FMantleScatter::Generate(Params, Instances, OutError, Options))
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

	const EComponentMobility::Type Mobility = ScatterRoot ? ScatterRoot->GetMobility() : EComponentMobility::Movable;
	UMantleShadowDistanceSubsystem* Shadows = World->GetSubsystem<UMantleShadowDistanceSubsystem>();
	// A drawDistance fades each instance out over its last fifth and draws none past it, as an InstancedMesh's does; the
	// Foliage tier shortens it. Per instance, not the component's Max Draw Distance, which is measured to the whole set's bounds.
	const double DrawDistanceCm = Params.DrawDistance * FMantleScatter::FoliageTierDistance(FoliageQuality);
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
		if (DrawDistanceCm > 0.0)
		{
			const int32 CullEnd = FMath::Max(FMath::RoundToInt32(DrawDistanceCm), 1);
			const int32 CullStart = FMath::Min(FMath::RoundToInt32(DrawDistanceCm * MantleScatterVolumePrivate::ScatterFadeStartShare), CullEnd);
			Ism->SetCullDistances(CullStart, CullEnd);
			if (Shadows)
			{
				Shadows->Track(Ism, DrawDistanceCm);
			}
		}
		for (int32 Slot = 0; Slot < Entry.Materials.Num(); ++Slot)
		{
			if (Entry.Materials[Slot])
			{
				Ism->SetMaterial(Slot, Entry.Materials[Slot]);
			}
		}
		if (Params.bCollision)
		{
			// Opt-in (#169): a blocking body per instance, as an InstancedMesh's, and a source for the NPC navigation mesh.
			Ism->SetCollisionProfileName(UCollisionProfile::CustomCollisionProfileName);
			Ism->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
			Ism->SetCollisionResponseToAllChannels(ECR_Block);
			Ism->SetGenerateOverlapEvents(false);
			Ism->SetCanEverAffectNavigation(true);
		}
		else
		{
			// Visual only: no physics bodies, no overlaps, nothing for navigation.
			Ism->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
			Ism->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Ism->SetGenerateOverlapEvents(false);
			Ism->SetCanEverAffectNavigation(false);
		}
		// Instances go in before the component registers, so its render state is built once for all
		// of them, as the place builder does for an InstancedMesh.
		Ism->PreAllocateInstancesMemory(Buckets[MeshIndex].Num());
		Ism->AddInstances(Buckets[MeshIndex], /*bShouldReturnIndices*/ false, /*bWorldSpace*/ false, /*bUpdateNavigation*/ Params.bCollision);
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
	Key += FString::Printf(TEXT("|d%.17g|p%d,%.17g,%d,%.17g,%d,%.17g|s%d,%.17g,%.17g|c%d,%.17g|k%d|t%d"), DrawDistance,
		static_cast<int32>(Placement), MaxSlopeDegrees, bLimitMinHeight, MinHeight, bLimitMaxHeight, MaxHeight,
		static_cast<int32>(Shape), Radius, Width, ClusterCount, ClusterRadius, bCollision, TerrainSurfaces.Num());
	Key += FString::Printf(TEXT("|v%d"), TerrainVersion);
	if (Placement == EMantleScatterPlacement::Terrain)
	{
		// Where the volume stands decides which ground each instance lands on, so moving it rebuilds.
		const FTransform Placed = GetActorTransform();
		const FRotator Turn = Placed.GetRotation().Rotator();
		const FVector Where = Placed.GetLocation();
		const FVector Size = Placed.GetScale3D();
		Key += FString::Printf(TEXT("|w%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g"), Where.X, Where.Y, Where.Z, Turn.Pitch, Turn.Yaw, Turn.Roll,
			Size.X, Size.Y, Size.Z);
	}
	for (const FVector2D& Point : Path)
	{
		Key += FString::Printf(TEXT("|P%.17g,%.17g"), Point.X, Point.Y);
	}
	for (const FMantleScatterExclusionSettings& Exclusion : Exclusions)
	{
		Key += FString::Printf(TEXT("|X%.17g,%.17g,%.17g,%.17g"), Exclusion.Center.X, Exclusion.Center.Y, Exclusion.HalfExtent.X, Exclusion.HalfExtent.Y);
	}
	return Key;
}

void AMantleScatterVolume::SetTerrainSurfaces(TArray<FMantleScatterTerrainSurface> InSurfaces)
{
	TerrainSurfaces = MoveTemp(InSurfaces);
	// New heights for the same number of Terrains (a sculpt) must rebuild too, so the key carries a counter.
	++TerrainVersion;
	if (IsEditorWorld())
	{
		RefreshEditorPreview();
	}
}

void AMantleScatterVolume::BindScalability()
{
	if (!ScalabilityHandle.IsValid())
	{
		ScalabilityHandle = Scalability::OnScalabilitySettingsChanged.AddUObject(this, &AMantleScatterVolume::OnScalabilityChanged);
	}
}

void AMantleScatterVolume::UnbindScalability()
{
	if (ScalabilityHandle.IsValid())
	{
		Scalability::OnScalabilitySettingsChanged.Remove(ScalabilityHandle);
		ScalabilityHandle.Reset();
	}
}

void AMantleScatterVolume::OnScalabilityChanged(const Scalability::FQualityLevels& Levels)
{
	// Stands again only when the Foliage tier moved, and only once something was built for this volume to redo.
	if (BuiltFoliageQuality < 0 || Levels.FoliageQuality == BuiltFoliageQuality)
	{
		return;
	}
	FString Error;
	if (!RefreshScatter(Error))
	{
		UE_LOG(LogMantle, Warning, TEXT("MANTLE_SCATTER %s: could not stand again at Foliage tier %d: %s"), *GetName(), Levels.FoliageQuality, *Error);
	}
}

void AMantleScatterVolume::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UnbindScalability();
	Super::EndPlay(EndPlayReason);
}

void AMantleScatterVolume::BeginDestroy()
{
	UnbindScalability();
	Super::BeginDestroy();
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

FMantleScatterGroundFn AMantleScatterVolume::MakeGround() const
{
	// Both transforms are rebuilt from their location, exported rotator and scale: what a place file holds and the client builds
	// from, so the editor's quaternion cannot round a height differently from the client's.
	const auto Exported = [](const FTransform& T) { return FTransform(FRotator(T.GetRotation().Rotator()), T.GetLocation(), T.GetScale3D()); };
	TArray<FMantleScatterTerrainSurface> Exact = TerrainSurfaces;
	for (FMantleScatterTerrainSurface& Surface : Exact)
	{
		Surface.TerrainToWorld = Exported(Surface.TerrainToWorld);
	}
	return [Surfaces = MoveTemp(Exact), ScatterToWorld = Exported(GetActorTransform())](
		const int64 XMm, const int64 YMm, FMantleScatterGround& Out) -> bool
	{
		// The point's world X-Y; the highest surface under it wins, holes and the terrain's edge leave none.
		const FVector World = ScatterToWorld.TransformPosition(FVector(static_cast<double>(XMm) * 0.1, static_cast<double>(YMm) * 0.1, 0.0));
		bool bFound = false;
		double BestZ = 0.0;
		FVector BestNormal = FVector::UpVector;
		for (const FMantleScatterTerrainSurface& Surface : Surfaces)
		{
			if (!Surface.Field.IsValid())
			{
				continue;
			}
			const FMantleHeightfield& Field = *Surface.Field;
			const FVector Local = Surface.TerrainToWorld.InverseTransformPosition(World);
			if (!(Local.X >= 0.0 && Local.Y >= 0.0 && Local.X <= Field.ExtentX() && Local.Y <= Field.ExtentY()))
			{
				continue;
			}
			if (Surface.Holes.IsValid() && Surface.Holes->IsHoleAt(Local.X / Field.SpacingX, Local.Y / Field.SpacingY))
			{
				continue;
			}
			float Height = 0.0f;
			FVector3f Normal = FVector3f::UpVector;
			if (!FMantleTerrainGrassGenerator::SurfaceAt(Field, Local.X, Local.Y, Height, Normal))
			{
				continue;
			}
			const double SurfaceZ = Surface.TerrainToWorld.TransformPosition(FVector(Local.X, Local.Y, static_cast<double>(Height))).Z;
			if (!bFound || SurfaceZ > BestZ)
			{
				bFound = true;
				BestZ = SurfaceZ;
				BestNormal = Surface.TerrainToWorld.TransformVectorNoScale(FVector(Normal)).GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
			}
		}
		if (!bFound)
		{
			return false;
		}
		// Rounded to whole millimetres in the scatter's own space, the fixed point every machine agrees on.
		const double LocalZ = ScatterToWorld.InverseTransformPosition(FVector(World.X, World.Y, BestZ)).Z;
		const double Millimetres = LocalZ * 10.0;
		Out.ZMm = static_cast<int64>(FMath::FloorToDouble(Millimetres + 0.5));
		Out.NormalZ = BestNormal.Z;
		Out.WorldZCm = BestZ;
		return true;
	};
}

bool AMantleScatterVolume::GenerateFull(TArray<FMantleScatterInstance>& OutInstances, FString& OutError) const
{
	// Cached under everything the instances depend on: the settings, the Terrains below and where this volume stands.
	const FString Key = SettingsKey() + TEXT("|x") + GetActorTransform().ToString();
	if (FullKey.Equals(Key, ESearchCase::CaseSensitive))
	{
		OutInstances = FullInstances;
		return true;
	}
	FMantleScatterParams Params;
	if (!ReadProps(Params, OutError))
	{
		return false;
	}
	FMantleScatterGenerateOptions Options;
	if (Params.Placement == EMantleScatterPlacement::Terrain)
	{
		Options.Ground = MakeGround();
	}
	if (!FMantleScatter::Generate(Params, FullInstances, OutError, Options))
	{
		FullKey.Reset();
		return false;
	}
	FullKey = Key;
	FullHash = FMantleScatter::Hash(FullInstances);
	OutInstances = FullInstances;
	return true;
}
