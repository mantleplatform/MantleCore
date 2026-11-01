// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MantlePlaceTypes.h"
#include "MantleScatter.h"

#include "MantleScatterVolume.generated.h"

struct FMantleHeightfield;
struct FMantleTerrainHoleGrid;
namespace Scalability { struct FQualityLevels; }
class UBoxComponent;
class UInstancedStaticMeshComponent;
class UMaterialInterface;
class UStaticMesh;
struct FPropertyChangedEvent;

/** One mesh a Mantle Scatter Volume draws from (#530). */
USTRUCT()
struct MANTLERUNTIME_API FMantleScatterMeshSettings
{
	GENERATED_BODY()

	/** A mesh from the Mantle library, or one of this project's own, which publishes as an upload like a Static Mesh Actor's. */
	UPROPERTY(EditAnywhere, Category = "Scatter")
	TObjectPtr<UStaticMesh> Mesh;

	/** How often this mesh is picked, relative to the others: 0 to 1000. 0 never picks it. */
	UPROPERTY(EditAnywhere, Category = "Scatter", meta = (ClampMin = "0", ClampMax = "1000", UIMin = "0", UIMax = "100"))
	int32 Weight = 1;

	/** Material overrides by slot, as on a Static Mesh Actor. An empty slot keeps the mesh's own material. */
	UPROPERTY(EditAnywhere, Category = "Scatter")
	TArray<TObjectPtr<UMaterialInterface>> Materials;

	/**
	 * The mesh id it names in a place file, kept when the volume was made from one (the place builder,
	 * the importer). Export names Mesh's id instead whenever Mesh is set.
	 */
	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "Scatter")
	FString MeshId;

	/** The material ids by slot it names in a place file, kept like MeshId. */
	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "Scatter")
	TArray<FString> MaterialIds;
};

/** An X-Y box no instance of a Mantle Scatter Volume stands in (#169), in the volume's own space. */
USTRUCT()
struct MANTLERUNTIME_API FMantleScatterExclusionSettings
{
	GENERATED_BODY()

	/** The box's centre, cm. */
	UPROPERTY(EditAnywhere, Category = "Scatter", meta = (Units = "cm"))
	FVector2D Center = FVector2D::ZeroVector;

	/** Half the box in X and Y, cm. */
	UPROPERTY(EditAnywhere, Category = "Scatter", meta = (ClampMin = "0.1", ClampMax = "1000000", UIMin = "10", Units = "cm"))
	FVector2D HalfExtent = FVector2D(100.0, 100.0);
};

/** A Terrain a Scatter drops its instances onto (#169): its surface and where it stands in the world. */
struct FMantleScatterTerrainSurface
{
	TSharedPtr<const FMantleHeightfield> Field;
	TSharedPtr<const FMantleTerrainHoleGrid> Holes;
	FTransform TerrainToWorld = FTransform::Identity;
};

/**
 * A seeded field of instanced meshes (#530): the Scatter place entity. A creator places one, sizes its
 * box and picks its meshes, and the level shows the scatter as it is edited; the place builder spawns one
 * per Scatter entity. Both generate the instances with FMantleScatter from the same parameters, so the
 * editor preview is exactly what every player sees.
 *
 * Visual only: no collision, nothing for navigation, never replicated. Only a machine that draws builds
 * the instances; a dedicated server keeps the actor (a transform node for the entity) and builds nothing.
 *
 * The instances are in the actor's own space: the box is HalfExtent about the actor, and the actor's
 * transform moves, turns and scales the whole field.
 */
UCLASS(ClassGroup = Mantle, meta = (DisplayName = "Mantle Scatter Volume"),
	hidecategories = (Advanced, Cooking, HLOD, Input, LOD, Networking, Physics, Replication, Collision, Rendering))
class MANTLERUNTIME_API AMantleScatterVolume : public AActor
{
	GENERATED_BODY()

public:
	AMantleScatterVolume();

	/** Picks every instance. The same seed and settings give the same scatter everywhere. */
	UPROPERTY(EditAnywhere, Category = "Scatter", meta = (ClampMin = "0", UIMin = "0"))
	int32 Seed = 0;

	/** Half the box instances are placed in, cm, about the volume. Z 0 is a flat layer. At most 10 km each way. */
	UPROPERTY(EditAnywhere, Category = "Scatter", meta = (ClampMin = "0", ClampMax = "1000000", UIMin = "0", Units = "cm"))
	FVector HalfExtent = FVector(1000.0, 1000.0, 0.0);

	/** Instances per square metre of the box's footprint. A scatter makes at most 200,000, and a place holds 200,000 in all. */
	UPROPERTY(EditAnywhere, Category = "Scatter", meta = (ClampMin = "0", UIMin = "0", UIMax = "10"))
	double Density = 0.5;

	/** The smallest uniform scale an instance gets, 0.001 to 65.535. */
	UPROPERTY(EditAnywhere, Category = "Scatter", meta = (ClampMin = "0.001", ClampMax = "65.535", UIMin = "0.1", UIMax = "4"))
	double MinScale = 0.8;

	/** The largest uniform scale an instance gets, 0.001 to 65.535, and at least Min Scale. */
	UPROPERTY(EditAnywhere, Category = "Scatter", meta = (ClampMin = "0.001", ClampMax = "65.535", UIMin = "0.1", UIMax = "4"))
	double MaxScale = 1.2;

	/** Upright turns instances about Z only; Tilted also leans them up to Max Tilt; Tumbled turns them any way. */
	UPROPERTY(EditAnywhere, Category = "Scatter")
	EMantleScatterAlignment Alignment = EMantleScatterAlignment::Upright;

	/** Tilted only: the largest lean either way, 0 to 90 degrees. */
	UPROPERTY(EditAnywhere, Category = "Scatter",
		meta = (ClampMin = "0", ClampMax = "90", UIMin = "0", UIMax = "90", Units = "deg",
			EditCondition = "Alignment == EMantleScatterAlignment::Tilted", EditConditionHides))
	double MaxTiltDegrees = 10.0;

	/** The meshes instances are drawn from, 1 to 16, each picked by its weight. */
	UPROPERTY(EditAnywhere, Category = "Scatter", meta = (TitleProperty = "Mesh"))
	TArray<FMantleScatterMeshSettings> Meshes;

	/** How far from the camera instances are drawn, cm (they fade over the last fifth); 0 never culls. At most 1,000,000. */
	UPROPERTY(EditAnywhere, Category = "Scatter", meta = (ClampMin = "0", ClampMax = "1000000", UIMin = "0", UIMax = "50000", Units = "cm"))
	double DrawDistance = 0.0;

	/** Box scatters inside the footprint; Terrain drops each instance onto the Terrain below it and leaves out those with none (holes, off the edge). */
	UPROPERTY(EditAnywhere, Category = "Scatter|Terrain")
	EMantleScatterPlacement Placement = EMantleScatterPlacement::Box;

	/** Terrain only: instances on ground steeper than this many degrees from flat are left out. 90 leaves out none. */
	UPROPERTY(EditAnywhere, Category = "Scatter|Terrain",
		meta = (ClampMin = "0", ClampMax = "90", Units = "deg",
			EditCondition = "Placement == EMantleScatterPlacement::Terrain", EditConditionHides))
	double MaxSlopeDegrees = 90.0;

	/** Terrain only: leave out instances whose ground is lower than Min Height (world cm). */
	UPROPERTY(EditAnywhere, Category = "Scatter|Terrain",
		meta = (EditCondition = "Placement == EMantleScatterPlacement::Terrain", EditConditionHides))
	bool bLimitMinHeight = false;

	UPROPERTY(EditAnywhere, Category = "Scatter|Terrain",
		meta = (ClampMin = "-1000000", ClampMax = "1000000", Units = "cm",
			EditCondition = "Placement == EMantleScatterPlacement::Terrain && bLimitMinHeight", EditConditionHides))
	double MinHeight = 0.0;

	/** Terrain only: leave out instances whose ground is higher than Max Height (world cm). */
	UPROPERTY(EditAnywhere, Category = "Scatter|Terrain",
		meta = (EditCondition = "Placement == EMantleScatterPlacement::Terrain", EditConditionHides))
	bool bLimitMaxHeight = false;

	UPROPERTY(EditAnywhere, Category = "Scatter|Terrain",
		meta = (ClampMin = "-1000000", ClampMax = "1000000", Units = "cm",
			EditCondition = "Placement == EMantleScatterPlacement::Terrain && bLimitMaxHeight", EditConditionHides))
	double MaxHeight = 10000.0;

	/** The footprint: the Half Extent box, a Disc of Radius, or a Path strip of Width along its points. Half Extent's Z sets the layer's depth in every shape. */
	UPROPERTY(EditAnywhere, Category = "Scatter|Shape")
	EMantleScatterShape Shape = EMantleScatterShape::Box;

	/** Disc: the radius, cm. */
	UPROPERTY(EditAnywhere, Category = "Scatter|Shape",
		meta = (ClampMin = "0.1", ClampMax = "1000000", Units = "cm",
			EditCondition = "Shape == EMantleScatterShape::Disc", EditConditionHides))
	double Radius = 1000.0;

	/** Path: 2 to 64 points, cm in the volume's own space, instances scatter along the line between them. */
	UPROPERTY(EditAnywhere, Category = "Scatter|Shape",
		meta = (EditCondition = "Shape == EMantleScatterShape::Path", EditConditionHides))
	TArray<FVector2D> Path;

	/** Path: the strip's full width, cm. */
	UPROPERTY(EditAnywhere, Category = "Scatter|Shape",
		meta = (ClampMin = "0.1", ClampMax = "100000", Units = "cm",
			EditCondition = "Shape == EMantleScatterShape::Path", EditConditionHides))
	double Width = 500.0;

	/** Up to 16 boxes (X-Y) no instance stands in. */
	UPROPERTY(EditAnywhere, Category = "Scatter|Shape")
	TArray<FMantleScatterExclusionSettings> Exclusions;

	/** 0 spreads instances evenly; 1 to 1000 gathers them into that many clusters, each of Cluster Radius, centred inside the footprint. */
	UPROPERTY(EditAnywhere, Category = "Scatter|Shape", meta = (ClampMin = "0", ClampMax = "1000", UIMin = "0", UIMax = "50"))
	int32 ClusterCount = 0;

	UPROPERTY(EditAnywhere, Category = "Scatter|Shape",
		meta = (ClampMin = "0.1", ClampMax = "100000", Units = "cm", EditCondition = "ClusterCount > 0", EditConditionHides))
	double ClusterRadius = 500.0;

	/**
	 * The Terrains Terrain placement drops onto: set by the place builder from the Terrains it built, and by an editor that finds
	 * the Terrains in its level. Instances are left out where none lies below them.
	 */
	void SetTerrainSurfaces(TArray<FMantleScatterTerrainSurface> InSurfaces);

	/**
	 * Sets the actor from Scatter properties (the place builder's and the importer's): the numbers, and
	 * each mesh's weight and ids. Loads nothing; LoadAssetsFromIds does, where something draws.
	 */
	void ApplyProps(const FMantleScatterParams& In);

	/**
	 * Resolves each mesh's MeshId and MaterialIds through FMantleAssetLibrary into Mesh and Materials. One
	 * error per id that cannot load, its Path relative to the entity's properties ("/meshes/0/mesh"); that
	 * mesh's instances are then left out.
	 */
	void LoadAssetsFromIds(TArray<FMantleParseError>& OutErrors);

	/**
	 * Reads the actor into Scatter properties. Each mesh's id is its library id when Mesh is a library
	 * mesh, else MeshId, else Mesh's path (the exporter replaces the last with the upload's id); materials
	 * are left to the exporter. False, with OutProblem phrased to follow the actor's name and saying what
	 * to fix, when a mesh is missing or a value is outside the place schema's limits (the per-scatter
	 * instance cap included). Out is filled either way.
	 */
	bool ReadProps(FMantleScatterParams& Out, FString& OutProblem) const;

	/**
	 * Generates the instances and builds them: one instanced static mesh component per mesh that has
	 * instances, collision-free, under the volume's root. Replaces what was built before. Builds nothing,
	 * and succeeds, where nothing draws (a dedicated server). False with OutError when the settings cannot
	 * generate. In a level being edited the components are the preview: editor-only and never saved.
	 */
	bool RefreshScatter(FString& OutError);

	/** Removes the built instances. */
	void ClearScatter();

	/** The instances the last RefreshScatter generated (0 where nothing draws). */
	int32 GetInstanceCount() const { return BuiltInstances; }

	/** FMantleScatter::Hash of the last RefreshScatter's instances (0 where nothing draws). */
	uint64 GetInstanceHash() const { return BuiltHash; }

	/** The Foliage tier (0 Low to 3 Epic) the last RefreshScatter built at (3 in an editor preview); -1 before any. */
	int32 GetBuiltFoliageQuality() const { return BuiltFoliageQuality; }

	/** Whether Component is one RefreshScatter built: the scatter's own, not an extra component. */
	bool IsScatterComponent(const UActorComponent* Component) const;

protected:
	virtual void OnConstruction(const FTransform& Transform) override;
	/** A level opened in the editor registers its actors without running their construction: the preview starts here then. */
	virtual void PostRegisterAllComponents() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void BeginDestroy() override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	bool IsEditorWorld() const;
	/** The editor preview: rebuilt when the settings change, not when the volume moves. */
	void RefreshEditorPreview();
	/** Everything the instances depend on, as text: the preview rebuilds only when it changes. */
	FString SettingsKey() const;
	void SyncEditorBounds();

	UPROPERTY(VisibleAnywhere, Category = "Scatter")
	TObjectPtr<USceneComponent> ScatterRoot;

	/** Built at runtime (and for the editor preview), never saved. */
	UPROPERTY(Transient, DuplicateTransient, TextExportTransient)
	TArray<TObjectPtr<UInstancedStaticMeshComponent>> ScatterComponents;

	/** The ground Terrain placement drops onto, and the volume's transform it was set for. */
	TArray<FMantleScatterTerrainSurface> TerrainSurfaces;

	/** A game that draws re-reads the Foliage tier when Settings change: the instances stand again at the new density and distance. */
	void BindScalability();
	void UnbindScalability();
	void OnScalabilityChanged(const Scalability::FQualityLevels& Levels);
	FDelegateHandle ScalabilityHandle;
	int32 BuiltFoliageQuality = -1;

	int32 BuiltInstances = 0;
	uint64 BuiltHash = 0;
	/** SettingsKey of the preview on show; empty when none is. */
	FString PreviewKey;

#if WITH_EDITORONLY_DATA
	/** The box, in the editor only. */
	UPROPERTY()
	TObjectPtr<UBoxComponent> EditorBounds;
#endif
};
