// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MantlePlaceTypes.h"
#include "MantleScatter.h"

#include "MantleScatterVolume.generated.h"

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

	/** Whether Component is one RefreshScatter built: the scatter's own, not an extra component. */
	bool IsScatterComponent(const UActorComponent* Component) const;

protected:
	virtual void OnConstruction(const FTransform& Transform) override;
	/** A level opened in the editor registers its actors without running their construction: the preview starts here then. */
	virtual void PostRegisterAllComponents() override;
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
