// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleCoverage.h"
#include "MantlePlaceInstancing.h"
#include "MantlePlaceTypes.h"
#include "MantleScriptApiUsage.h"
#include "MantleScriptCatalogFiles.h"

struct FMantlePlaceAssetSet;
struct FMantleScriptError;

class AActor;
class UActorComponent;
class FJsonObject;
class UEdGraph;
class UInstancedStaticMeshComponent;
class UMaterialInterface;
class UPhysicalMaterial;
class USceneComponent;
class UStaticMesh;
class UStaticMeshComponent;
class UTexture;
class UWorld;

/**
 * One part of an actor or component Mantle can't publish (#467, #486), phrased to follow its
 * label, with the fix. Blocking parts are errors at Check and refuse Export; the rest are warnings.
 */
struct FMantleUnpublishedPart
{
	FString What;
	FString Remedy;
	bool bBlocksPublish = true;

	/** The coverage report's category for what this part lost (#628), when its code knows it; empty reports it as "Other components". */
	FString Category;
};

/**
 * Every exported instance of one mesh with one material set, collision and shadow (#467), in
 * world space, before FMantlePlaceExporter::ChunkInstancedMeshes cuts it into entities.
 */
struct FMantleInstancedMeshGroup
{
	/** The mesh, materials, collision and shadow every instance shares. */
	FMantleStaticMeshProps Mesh;

	/** The mesh asset's name, for the entity names. */
	FString MeshName;

	TArray<FTransform> WorldInstances;

	/** Painted foliage only (GhzGarage/mantle#818): the instances thinning takes first, before props and PCG output. */
	bool bThinFirst = false;
};

/**
 * One component of a multi-component actor (a Blueprint prefab: a lamp post's mesh and light, a
 * house kit's pieces) that Export writes as its own entity, a child of the actor's Group (#468).
 */
struct FMantlePrefabPart
{
	const USceneComponent* Component = nullptr;

	/** StaticMesh, SkeletalMesh, PointLight, SpotLight, RectLight, TriggerVolume, SoundEmitter or Effect (#628). */
	EMantleEntityType Type = EMantleEntityType::StaticMesh;

	/**
	 * The component's transform relative to the actor's root component, through every attach
	 * parent in between (sockets and non-uniform scale included): the child entity's transform, so
	 * the builder puts it at the same world transform the editor shows. Identity for the root.
	 * A mirrored mesh (MirroredMesh) has its positive scale here.
	 */
	FTransform Transform;

	/** An Effect part (#628): the "properties" object FMantleEffectExport::Translate made for one emitter of Component. */
	TSharedPtr<FJsonObject> EffectProperties;

	/** Which of Component's emitters EffectProperties is, from 0: a particle component can make several Effect entities. */
	int32 EffectIndex = 0;

	/** A StaticMesh part with a negative scale (#628): the mirrored copy to publish in place of the component's mesh (FMantleMirrorBake). Null otherwise. */
	UStaticMesh* MirroredMesh = nullptr;
};

/** One Script Asset (Mantle Place Settings) Export listed in assets[]: its Unreal path and the id scripts use for it. */
struct FMantleScriptAssetId
{
	/** The object path, as the Script Assets list holds it. */
	FString Path;

	FString Id;

	/**
	 * No upload was recorded for it (an offline export, preview): Id is FMantlePlaceAssets::ProvisionalId,
	 * not the id Publish writes. Never true for a library asset.
	 */
	bool bProvisional = false;
};

struct FMantleExportResult
{
	bool bSuccess = false;
	FMantlePlace Place;

	/** Actors the schema cannot express. Surfaced to the creator rather than dropped. */
	TArray<FMantleParseError> Errors;

	/** The level's Script Assets, in list order, each once, with the ids assets[] lists them by. */
	TArray<FMantleScriptAssetId> ScriptAssets;

	/**
	 * #394: what the exported scripts need from the script API, merged across scripts. Place's
	 * scriptApi minor is its Minor; bAnalysed is false when a script couldn't be scanned (Check
	 * explains which, as a note).
	 */
	FMantleScriptApiNeed ScriptApiNeed;

	/**
	 * #628: what the export kept, approximated and dropped, by category (FMantleCoverage). Export fills it,
	 * errors or not; the survey of the level and the exported place runs last (FMantleCoverageSurvey).
	 */
	FMantleCoverage Coverage;

	/** The place file's size in bytes, as ToJson writes it (GhzGarage/mantle#818), measured after grouping and any thinning. */
	int64 PlaceBytes = 0;

	/** Instanced mesh entries in the place file, after grouping and any thinning. */
	int64 InstanceTotal = 0;

	/** Mesh entities and instance sets that got a drawDistance (GhzGarage/mantle#630); 0 when the target client does not read them. */
	int32 DrawDistanceEntities = 0;

	/** Static mesh actors Export folded into instance sets. */
	FMantleGroupReport Grouping;

	/** Instances removed by Options.bThinInstances. Not applied (RemovesAnything false) unless it was asked for. */
	FMantleThinReport Thinning;

	/**
	 * When the place is over the instance limit or the file-size limit and thinning was not asked for: what thinning would do,
	 * so Check and Publish can offer it with its numbers. Empty (RemovesAnything false) otherwise.
	 */
	FMantleThinReport ThinPreview;

	/** Over the instance limit or the file-size limit (Errors says which, with the fix). */
	bool bOverBudget = false;

	/** The creator-facing sentence for bOverBudget, with the fix (also in Errors). */
	FString BudgetProblem;

	FString ErrorSummary() const
	{
		TArray<FString> Lines;
		for (const FMantleParseError& Error : Errors)
		{
			Lines.Add(Error.ToString());
		}
		return FString::Join(Lines, TEXT("\n"));
	}
};

struct FMantleExportOptions
{
	FString PlaceId;
	FString Name;
	FString AuthorId;
	int32 RevisionNumber = 1;
	FString RevisionNotes;

	/**
	 * Uploaded executable src_ ids keyed by stable place-local scr_ id. When absent
	 * (offline export and preview), the exporter writes a deterministic provisional id
	 * from the actual runtime source, including transpiled JavaScript for TypeScript.
	 */
	TMap<FString, FString> ScriptSourceIds;

	/** Original TypeScript src_ ids keyed by stable place-local scr_ id. */
	TMap<FString, FString> ScriptAuthorSourceIds;

	/**
	 * #549: uploaded src_ ids of the level's UI files, keyed by place path ("UI/Shop.rml",
	 * case-exact as on disk). When absent (offline export and preview), the exporter writes a
	 * deterministic provisional id from the file's text, as it does for scripts.
	 */
	TMap<FString, FString> UIDocumentSourceIds;

	/**
	 * The level's converted meshes, materials and textures and, once the upload queue has run,
	 * their ids (F5 #339, filled by F6 #340's Publish). Meshes and materials that are not library
	 * assets are written by these ids, and assets[] lists each with its uploaded hash and size,
	 * textures included. Absent (offline export, preview, Check): FMantlePlaceAssets::ProvisionalId
	 * stands in, and the manifest carries the source package's provisional digest.
	 */
	const FMantlePlaceAssetSet* Assets = nullptr;

	/**
	 * Fold repeated identical static mesh actors into instance sets (FMantlePlaceInstancing::GroupStaticMeshes). On unless a test
	 * wants the actors as they are.
	 */
	bool bGroupStaticMeshes = true;

	/**
	 * GhzGarage/mantle#818: when the place is over the instance limit or the backend's file-size limit, remove instances evenly
	 * until it fits (FMantlePlaceInstancing::ThinToFit), and say how many in the coverage report. Off by default: nothing is ever
	 * thinned unless the creator chose it (Thin and publish, Mantle.Publish -Thin=auto, Mantle.Check -Thin=auto).
	 */
	bool bThinInstances = false;
};

/**
 * A file under a level's Scripts/ folder the runtime catalogue would silently skip: an
 * extension it cannot map to a script language, or a top folder that is not exactly
 * Server/Client/Shared (#129 -- a mismatch that would work on a creator's case-insensitive
 * Windows checkout and fail on the Linux dedicated server). Reported explicitly rather than
 * dropped, so a creator finds out at export time, not at the join that matters.
 */
struct FMantleScriptCollectionIssue
{
	/** Relative to Scripts/, with the exact case it has on disk. */
	FString RelativePath;
	/** True when the extension itself is unrecognised; false for a folder/case problem. */
	bool bLanguageUnknown = false;
};

/**
 * One file under a level's UI/ folder (#549): an RmlUi document (.rml) or stylesheet (.rcss).
 * It travels like a script: uploaded as text, listed in uiDocuments[] by its src_ id.
 */
struct FMantleUIDocumentFile
{
	/** "UI/<path under UI/>", with the exact case it has on disk: the key scripts open it by. */
	FString Path;
	/** The file's text; empty when it is over FMantlePlaceLimits::MaxUIDocumentBytes (not read). */
	FString Text;
	/** Its size on disk in bytes. */
	int64 Bytes = 0;
	/** "rml" or "rcss": the upload's language. */
	FString Language;
};

/**
 * One image under a level's UI/ folder (#556 item 5): a .png or .jpg file (any case, and .jpeg,
 * so Check can name it) that a document shows. It is uploaded as a Texture asset, as a decal's
 * texture is, and listed in uiDocuments[] by that asset's id.
 */
struct FMantleUIImageFile
{
	/** "UI/<path under UI/>", with the exact case it has on disk: the key documents reference it by. */
	FString Path;
	/** Absolute path on disk. */
	FString DiskPath;
	/** Its size on disk in bytes. */
	int64 Bytes = 0;
	/** "png" or "jpeg" from the file's first bytes (FMantlePlaceExporter::SniffUIImageFormat); empty for anything else. */
	FString Format;
};

/** Something about a level's UI files that Publish would refuse (#549), for Check and Export. */
struct FMantleUIDocumentProblem
{
	/** The file's place path, or empty for the folder as a whole. */
	FString Path;
	FString Problem;
	FString Remedy;
};

/** A compiled UMantleScriptGraph ready to become one graph-authored place-script entry. The
 * executable and authoring sources are both text here; the publisher uploads them and passes
 * their returned ids back through FMantleExportOptions. */
struct FMantleGraphScriptEntry
{
	FString PackageName;
	FString Name;
	FString Path;
	EMantleScriptSide Side = EMantleScriptSide::Server;
	FString CompiledLuau;
	FString AuthorJson;
	/** #394: the script API the graph's member nodes need (FMantleScriptApiUsage::ForGraph); always analysed. */
	FMantleScriptApiNeed ApiNeed;
};

/**
 * The place-wide crowd budgets (#477), counted over a level's crowd spawners in the order Export
 * and "Check this place" walk it. The parser, validate.py and the backend apply the same two
 * totals, and like the parser this names only the spawner that goes over each one.
 */
struct MANTLEEDITOR_API FMantleCrowdBudget
{
	int32 Spawners = 0;
	int32 Npcs = 0;

	/**
	 * Counts one more spawner of Count NPCs. OutSpawnerProblem is set on the spawner past
	 * FMantlePlaceLimits::MaxCrowdSpawnersPerPlace, OutNpcProblem on the one that takes the
	 * total past MaxCrowdNpcsPerPlace; both are phrased to follow the actor's name and are
	 * empty otherwise.
	 */
	void Add(int32 Count, FString& OutSpawnerProblem, FString& OutNpcProblem);
};

/**
 * Turns a level into a place file.
 *
 * The inverse of FMantlePlaceBuilder, and deliberately strict: an actor the schema cannot
 * express is reported, never silently omitted. Silent omission would make the published
 * place a lossy copy of the creator's work, which would in turn make reopening a published
 * place destructive.
 */
class MANTLEEDITOR_API FMantlePlaceExporter
{
public:
	/**
	 * Maps an actor to its schema entity type, returning false when the schema cannot
	 * express it. Public so the validator and the exporter share one definition of what is
	 * publishable — two implementations would inevitably disagree.
	 *
	 * Actors of a type Mantle knows by class (static mesh actors, lights, player starts, ambient
	 * sounds, crowd spawners) keep that type. Otherwise the root decides: a plain scene component
	 * is a Group, and a box or sphere is a TriggerVolume. An actor whose components include parts
	 * CollectPrefabParts writes (a Blueprint prefab, #468) is a Group too, whatever its root: a
	 * mesh, a trigger with other parts beside it, or a component Mantle can't publish.
	 */
	static bool ClassifyActor(const AActor* Actor, EMantleEntityType& OutType);

	/**
	 * The parts Export writes for Actor as child entities of its Group (#468), sorted by component
	 * name, each with its transform relative to the actor's root: every static mesh component with
	 * a mesh that is visible in game, point and spot light shown in game, box or sphere that a
	 * player could see or touch (as a TriggerVolume), and audio component with a sound Mantle can
	 * publish. Empty unless ClassifyActor makes Actor a Group. Instanced static meshes publish on
	 * their own (#467), spline meshes are baked into static meshes on their own (#628), and dynamic
	 * meshes are named by DescribeUnpublishedParts (#486).
	 *
	 * OutUnsupported, when given, gets every other component a player could see or touch, each
	 * phrased to follow the actor's label, with a remedy, and not blocking: "Check this place" lists
	 * them as notes when the actor has parts, and the rest of the actor publishes without them.
	 *
	 * #628: a multi-component actor of any class becomes a Group (ClassifyActor), so its static mesh, light (point,
	 * spot, rect), skeletal mesh (playing one animation) and particle components are parts. A particle or Niagara
	 * component makes one Effect part per emitter FMantleEffectExport::Translate returns; OutCoverage, when given, takes
	 * what Translate reports it loses (Export passes the result's, so each loss is reported once). A static mesh
	 * component with a negative scale is a part with its mirrored copy and positive scale (FMantleActorDecompose);
	 * one whose copy cannot be made is left out and named in OutUnsupported.
	 */
	static void CollectPrefabParts(
		const AActor* Actor,
		TArray<FMantlePrefabPart>& OutParts,
		TArray<FMantleUnpublishedPart>* OutUnsupported = nullptr,
		FMantleCoverage* OutCoverage = nullptr);

	/**
	 * The entity id Export gives a prefab part (#468): ent_ plus 32 hex digits of a hash of its
	 * Group's id and the component's name, so it stays the same across republishes as long as the
	 * actor and the component keep theirs. Export suffixes it, as it does actor ids, in the unlikely
	 * case that it collides.
	 */
	static FString PrefabPartEntityId(const FString& GroupId, const UActorComponent* Component, int32 EffectIndex = 0);

	/**
	 * Whether Export marks a prefab (an actor CollectPrefabParts finds parts on, #468) dynamic, its
	 * Group and every part alike: only when something could move it. That is Blueprint logic
	 * (DescribeBlueprintLogic), or a scene component the creator made Movable themselves: Movable,
	 * not the automatic DefaultSceneRoot a new Blueprint gets (Movable by default), and not Movable
	 * only because its attach parent is (UE refuses a Static child under a Movable parent). Mantle
	 * has no tag that marks an entity dynamic; mobility is the flag. A plain Fab prefab, a Movable
	 * DefaultSceneRoot with no logic, is static place geometry.
	 */
	static bool IsPrefabDynamic(const AActor* Actor);

	/**
	 * Whether a Blueprint graph does anything: an enabled node with at least one wire. A new
	 * Blueprint's greyed-out BeginPlay/Tick/Overlap events and its bare construction-script entry
	 * are unwired, so an untouched Blueprint does not count as logic (#449).
	 */
	static bool GraphHasWiredNode(const UEdGraph* Graph);

	/**
	 * Where Actor's Blueprint, or a Blueprint parent of it, has logic (#449): each graph with a wired
	 * node, as "BP_Door's EventGraph", "BP_Door's construction script" or "BP_Door's function Open".
	 * False, with OutWhere empty, when there is none. Check warns with it; IsPrefabDynamic reads it.
	 */
	static bool DescribeBlueprintLogic(const AActor* Actor, TArray<FString>& OutWhere);

	/**
	 * ReadValueProperties for one component rather than an actor: a light component's, or a box or
	 * sphere trigger's (#468). A null or mismatched component leaves the
	 * type's defaults, as ReadValueProperties does. False, with nothing written, for other types.
	 */
	static bool ReadComponentValueProperties(const USceneComponent* Component, EMantleEntityType Type, FMantleEntity& OutEntity);

	/** True for engine-injected actors that are not authored content. */
	static bool IsEngineInfrastructure(const AActor* Actor);

	/**
	 * True for the four sky actors (SkyAtmosphere, SkyLight, VolumetricCloud,
	 * ExponentialHeightFog) FMantleEnvironment spawns at runtime for every place, and for a
	 * Static Mesh Actor that is a level's own sky dome (FMantleSkyMesh, #63). The exporter
	 * skips them — Mantle supplies its own, so a creator's settings on these specific actors
	 * are never published — and the validator reports each one as a warning rather than
	 * silently absorbing it as engine infrastructure, so a level built from UE's Default
	 * template is not left to guess why these actors vanished. One definition shared by the
	 * exporter, validator and preview, the same way ClassifyActor and IsEngineInfrastructure
	 * are.
	 */
	static bool IsProvidedByMantle(const AActor* Actor);

	/**
	 * Content a creator placed on purpose that Mantle cannot publish yet: BSP brushes (not the
	 * builder brush), volumes, placed pawns, cameras (#449), and a terrain rebuilt from a place file
	 * (#466; the level's own landscape publishes as a Terrain, see FMantleTerrainExport). Export skips them;
	 * the validator names each with OutWhat and a remedy, so nothing vanishes without a word
	 * (GhzGarage/mantle#264). Checked before IsEngineInfrastructure, which would otherwise
	 * swallow all of them.
	 */
	static bool DescribeUnpublishedContent(const AActor* Actor, FString& OutWhat, FString& OutRemedy);

	/**
	 * An actor Export leaves out, and why (#818). Carries what a creator needs: what is wrong in plain words (following the
	 * actor's label), the fix, the coverage category it is listed under, and whether it is a loss (a warning, Dropped) or only
	 * a note (a Level Instance whose actors publish on their own, Info).
	 */
	struct FSkippedActor
	{
		FString What;
		FString Remedy;
		FString Category;
		bool bLoss = true;
	};

	/**
	 * Whether Export leaves Actor out instead of refusing it (#818, creator friendliness first): editor-only and cinematic
	 * actors (camera rigs, level sequences, notes, target points, runtime virtual texture volumes), a Static Mesh Actor with no
	 * mesh, a decal with no BaseColor texture, an Ambient Sound whose Sound Cue plays no sound wave, a particle actor whose
	 * particles Mantle cannot carry, and a Level Instance (the actors of a loaded one publish on their own, as ordinary items).
	 * IsEngineInfrastructure is true for all of them, DescribeUnpublishedContent returns the ones that are a loss, and the
	 * coverage survey lists each (Dropped for a loss).
	 */
	static bool DescribeSkippedActor(const AActor* Actor, FSkippedActor& Out);

	/**
	 * The parts of Actor that can't publish as they are, each with its fix (#486). Unlike
	 * DescribeUnpublishedContent, these are errors: the creator sees the content in the editor, it
	 * would be missing from the place, and each has a fix in the editor. "Check this place" shows
	 * every one and Export refuses with the same text, so neither drops them silently. Checked for
	 * every actor, infrastructure included (a PCG volume is a brush, a partition actor NotPlaceable):
	 *
	 * - a PCG component set to Generate At Runtime (players' clients never run PCG graphs);
	 * - a PCG component that hasn't been generated, or is in Preview editing mode (nothing saved);
	 * - an actor PCG spawned (tagged "PCG Generated Actor") of a type Mantle can't publish;
	 * - dynamic mesh components (Geometry Script or PCG output) not saved as static mesh assets.
	 *
	 * PCG is recognised without linking it, since MantleCore has no plugin dependencies: components
	 * by class name (PCGComponent, PCGPartitionActor, DynamicMeshComponent, including subclasses),
	 * their settings read by reflection by property name (GenerationTrigger, bGenerated, bActivated,
	 * bIsComponentPartitioned, bIsComponentLocal, CurrentEditingMode), and PCG's own actor tag. A
	 * PCG release renaming one of these makes that check go quiet rather than misfire; the editor
	 * tests fail first.
	 */
	static void DescribeUnpublishedParts(const AActor* Actor, TArray<FMantleUnpublishedPart>& OutParts);

	/**
	 * The entity id Export gives Actor: ent_ plus its ActorGuid as 32 lowercase hex digits,
	 * so it survives renames and republishes (#267). Falls back to a label-derived id only
	 * when the actor has no guid. Export adds a numeric suffix if two actors in one level
	 * would share an id; this returns the unsuffixed id.
	 */
	static FString EntityIdFor(const AActor* Actor);

	/**
	 * Reads an AmbientSound into SoundEmitter properties (#263): the sound's id, the volume
	 * (clamped to the schema's 0..1), looping from the sound asset, and the radius at which the
	 * attenuation that applies to it reaches silence (MANTLE_DEFAULT_SOUND_RADIUS when nothing
	 * attenuates it). A library sound is written by its library id. Mantle ships none, so a sound
	 * is normally the creator's own (#324): a sound wave that FMantlePlaceAssets::CheckSoundRule
	 * accepts is uploaded with the place (converted to a WAV, FMantlePlaceAssetConverter) and
	 * written by the id Assets recorded for its FMantlePlaceAssets::SoundKey, or a provisional id
	 * when there is none yet. Returns false, with OutProblem phrased to follow the actor's name and
	 * saying what to do, when the actor has no sound or one the rule refuses (a Sound Cue or
	 * MetaSound, more than two channels, a sample rate or length past limits.json). Export and the
	 * validator both call this, so "Check this place" cannot pass a sound Publish refuses.
	 */
	static bool ReadSoundEmitter(const AActor* Actor, FMantleSoundEmitterProps& OutProps, FString& OutProblem,
		const FMantlePlaceAssetSet* Assets = nullptr);

	/**
	 * Reads a Mantle Crowd Spawner into CrowdSpawner properties (#477), through
	 * AMantleCrowdSpawner::ReadProps: lanes in the spawner's local space, the NPC settings, and
	 * the place schema's per-spawner limits. Returns false, with OutProblem phrased to follow the
	 * actor's name and saying what to fix, when the actor is not a spawner or a value is outside
	 * those limits; OutProps is filled either way. Export and the validator both call this, so
	 * "Check this place" cannot pass a spawner Publish refuses. The per-place totals
	 * (MaxCrowdSpawnersPerPlace, MaxCrowdNpcsPerPlace) are checked by each over the whole level.
	 */
	static bool ReadCrowdSpawner(const AActor* Actor, FMantleCrowdSpawnerProps& OutProps, FString& OutProblem);

	/**
	 * #513: names each point of interest of a Mantle Crowd Spawner (Actor) by the entity id its
	 * actor has in this export (IdByActor), in InOutProps, which ReadCrowdSpawner filled from the
	 * same actor. A point without an actor keeps its entity id (a spawner made from a place file).
	 * Returns false, with OutProblem phrased to follow the actor's name, when a point's actor is not
	 * published with the level; the parser refuses one naming no entity anyway.
	 */
	static bool ResolveCrowdPoints(const AActor* Actor, const TMap<const AActor*, FString>& IdByActor, FMantleCrowdSpawnerProps& InOutProps,
		FString& OutProblem);

	/**
	 * Reads a Decal Actor into Decal properties (#472): its size, sort order and fade screen size,
	 * the Opacity scalar of its material (1 when it has none), and the textures a published decal
	 * is made of, OutBaseColor and (when the material has one set) OutNormal. OutProps names no
	 * texture: Export turns the two into upload ids. A published decal is a texture drawn by
	 * Mantle's decal material, so the decal's material must be a deferred-decal material with its
	 * BaseColor texture parameter set (a Normal texture parameter and an Opacity scalar parameter
	 * are optional); anything else is refused, with OutProblem phrased to follow the actor's
	 * name and saying what to do. Export and the validator both call this.
	 */
	static bool ReadDecal(const AActor* Actor, FMantleDecalProps& OutProps, UTexture*& OutBaseColor, UTexture*& OutNormal, FString& OutProblem);

	/**
	 * True when Material is Mantle's decal master or an instance of it (#472), which publishes
	 * exactly. Any other decal material with a BaseColor texture publishes as its textures alone,
	 * and the validator warns that the rest of it is dropped.
	 */
	static bool IsMantleDecalMaterial(const UMaterialInterface* Material);

	/**
	 * A static mesh actor's physics block (#456), from the editor's Simulate Physics and mass
	 * settings: false, with nothing written, when Simulate Physics is off. The mass is the Mass (kg)
	 * override when set, clamped to the schema's range; otherwise it is left unset so the game
	 * computes it and floats by the material's density. The dampings are clamped too. It starts anchored (simulate false) when the
	 * component's actor carries the MantleAnchored tag (MantlePhysicsAnchoredTag). The material
	 * (#528) is the preset MatchPhysicsMaterial finds for the component's Phys Material Override, else
	 * its mesh's simple-collision physical material; one that matches no preset publishes as default,
	 * and OutMaterialNote, when given, is set to say so, phrased to follow the actor's label. Export
	 * and the validator both call this.
	 */
	static bool ReadPhysics(const UStaticMeshComponent* Component, FMantlePhysicsProps& OutPhysics, FString* OutMaterialNote = nullptr);

	/**
	 * The physics.material preset Material maps to (#528): one of Mantle's own (/Game/Mantle/Physics)
	 * by path; else by name, when a word of the asset's name names exactly one preset ("PM_Rubber",
	 * "IceFloor", "Steel_Heavy" is metal, "Concrete" stone); else by values, the preset nearest in
	 * friction and restitution when it is within 0.1 of both. No material at all is default. False,
	 * with OutPreset default, when a material matches none.
	 */
	static bool MatchPhysicsMaterial(const UPhysicalMaterial* Material, EMantlePhysicsMaterial& OutPreset);

	/**
	 * The surface a physical material carries for publishing (#596), by name: a physical material named after a
	 * surface ("Concrete", "PM_Concrete", or the game's own "PM_MantleSurface_Concrete"; a physics preset's
	 * "PM_MantleMetal" too), in any case, or the game's own by path. A creator marks a surface by naming a physical
	 * material after it. Default for none, for the default preset and for any other material.
	 */
	static EMantleSurface MatchSurface(const UPhysicalMaterial* Material);

	/**
	 * A mesh component's surface (#596), the UE-native way: its Phys Material Override, else its first
	 * material's physical material, through MatchSurface. Export reads it for StaticMesh and
	 * InstancedMesh entities, never for one with a physics block (whose physics.material carries it).
	 */
	static EMantleSurface ReadSurface(const UStaticMeshComponent* Component);

	/**
	 * #528: an engine Physics Constraint Actor as a Constraint entity: its settings as a weld, a hinge
	 * or a rope (FMantleConstraints::ReadEditorConstraint), between OutA (its Constraint Actor 1) and
	 * OutB (its Constraint Actor 2, null for the world), each a static mesh actor this level publishes
	 * as a physics object (Simulate Physics on). False, with OutProblem saying what to change, for any
	 * other: such a constraint is left out of the place (IsEngineInfrastructure) and named by
	 * DescribeUnpublishedContent, so "Check this place" says why. Export and the validator both call this.
	 */
	static bool ReadConstraint(const AActor* Actor, FMantleConstraintProps& OutProps, const AActor*& OutA, const AActor*& OutB,
		FString& OutProblem);

	static FMantleExportResult Export(UWorld* World, const FMantleExportOptions& Options);

	/**
	 * The properties Export writes for a point, spot, rect (#472) or directional light or a trigger
	 * volume, read from the actor exactly as Export reads them, into OutEntity.Props. Export and the
	 * validator both call this, so "Check this place" holds the same values to the schema's ranges
	 * that the upload is held to (#393). False, with nothing written, for every other type.
	 */
	static bool ReadValueProperties(const AActor* Actor, EMantleEntityType Type, FMantleEntity& OutEntity);

	/** True when Export writes Actor as an entity: valid, not infrastructure, not a Mantle sky actor, and a type the schema has. */
	static bool IsExportedActor(const AActor* Actor);

	/**
	 * Whether Export publishes Component's instances as InstancedMesh entities (#467). Any
	 * UInstancedStaticMeshComponent (HISM and painted foliage derive from it) on any actor,
	 * including PCG volumes and partition actors, which is how PCG output publishes without a PCG
	 * dependency. Not: editor-only or transient components or owners (a preview, runtime
	 * generation), PCG's own debug output, empty components, a Mantle sky actor's, and components
	 * neither visible in game nor colliding (visualisation nobody could see or touch).
	 */
	static bool IsExportedInstanceSource(const UInstancedStaticMeshComponent* Component);

	/** Every component IsExportedInstanceSource accepts in World, in actor then component order. */
	static TArray<UInstancedStaticMeshComponent*> CollectInstanceSources(UWorld* World);

	/**
	 * An actor ClassifyActor cannot express whose content is only exported instanced meshes (a PCG
	 * volume or partition actor, a Blueprint of instanced props, a place imported back into a
	 * level). Export writes its instances and not the actor, and neither Export nor Check reports it
	 * as unpublishable. Primitives that aren't content in game (editor-only, or hidden in game with
	 * no collision, like a spline or a volume's brush) don't count; neither do the parts
	 * DescribeUnpublishedParts reports on their own. Spline meshes (#628) are baked into static
	 * meshes on their own (FMantleSplineMeshBake), so an actor holding only those and instanced
	 * meshes counts too.
	 */
	static bool IsInstanceHost(const AActor* Actor);

	/**
	 * What about one exported instance source can't be published as it is, with the fix (#467):
	 * no mesh, mirrored or zero-scale instances, material overrides past slot 15 (blocking), and
	 * per-instance custom data, which isn't published (a warning). Check and Export both call it.
	 */
	static void CheckInstanceSource(const UInstancedStaticMeshComponent* Component, TArray<FMantleUnpublishedPart>& OutParts);

	/**
	 * Cuts each group into InstancedMesh entities (#467): instances sorted along a Morton curve so
	 * each entity covers a compact area, at most MaxPerEntity per entity, each entity anchored at
	 * the centre of its instances' bounds (whole centimetres) with the instances relative to it.
	 * Groups come out in InstancedGroupKey order. Ids are ent_ism_ + a hash of the group key + _ +
	 * the chunk index, so the same content keeps its ids across republishes. Pure and deterministic.
	 */
	static TArray<FMantleEntity> ChunkInstancedMeshes(
		const TArray<FMantleInstancedMeshGroup>& Groups,
		int32 MaxPerEntity);

	/** The key instances are grouped by: mesh id, material ids by slot, collision and shadow, and a surface other than default (#596). */
	static FString InstancedGroupKey(const FMantleStaticMeshProps& Mesh);

	/** The start of the ids ChunkInstancedMeshes gives a group's entities: "ent_ism_" and the hash of its key, then "_" (GhzGarage/mantle#818). */
	static FString InstancedEntityIdPrefix(const FMantleStaticMeshProps& Mesh);

	/**
	 * The transform Export writes for Actor: relative to its attach parent when Export writes that
	 * parent too (the entity's parent), else the world transform. What Check holds to the schema's
	 * positive scale (#393).
	 */
	static FTransform PublishedTransform(const AActor* Actor);

	/** Serialises a place to JSON text. */
	static FString ToJson(const FMantlePlace& Place);

	/**
	 * Absolute path to World's Scripts/ folder, beside its level package on disk -- for
	 * /Game/Foo/MyLevel that is Content/Foo/Scripts/. Empty when the level has never been
	 * saved under /Game (every scratch world used by tests elsewhere in this module, for
	 * instance) or simply has no such folder.
	 */
	static FString GetScriptsFolder(UWorld* World);

	/**
	 * Reads every script under ScriptsFolder, classified exactly as
	 * FMantleDiskScriptCatalogSource classifies them for a running client -- the exporter and
	 * the runtime must never disagree about what a Scripts/ folder contains. Entries come back
	 * sorted by path, which is what keeps a repeat export of unchanged sources byte-identical.
	 * A file the catalogue would silently skip is reported through OutIssues instead.
	 */
	static void CollectScriptFiles(
		const FString& ScriptsFolder,
		TArray<FMantleScriptCatalogEntry>& OutEntries,
		TArray<FMantleScriptCollectionIssue>& OutIssues);

	/** Finds graph assets under Scripts/, builds their editable graph models, and compiles them to
	 * Luau. OutGraphPaths includes every discovered asset, even ones that fail to compile, so their
	 * .uasset/.luau implementation files cannot be mistaken for ordinary scripts. Errors name the
	 * graph path and compiler node; invalid graphs are never published. Results are path-sorted. */
	static void CollectGraphScripts(
		const FString& ScriptsFolder,
		TArray<FMantleGraphScriptEntry>& OutEntries,
		TArray<FMantleParseError>& OutErrors,
		TArray<FString>& OutGraphPaths);

	/** Drops each discovered graph asset's .uasset and generated .luau sibling from disk-script
	 * discovery. Invalid graphs are still excluded here, but remain blocking compile errors. */
	static void FilterGraphGeneratedFiles(
		TArray<FMantleScriptCatalogEntry>& InOutEntries,
		TArray<FMantleScriptCollectionIssue>& InOutIssues,
		const TArray<FString>& GraphPaths);

	/**
	 * Deterministic scr_ ids for an ordered collection of script paths, one per path in the same order, stable
	 * across a repeat export of the same files. A collision (two paths sanitising to the same id) is
	 * broken with a numeric suffix, as entity ids are. Exposed so the publisher can
	 * compute the same keys CollectScriptFiles's entries will use before it calls Export.
	 */
	static TArray<FString> MakeScriptIds(const TArray<FString>& Paths);

	/**
	 * #549: absolute path to World's UI/ folder, beside Scripts/ (Content/Foo/UI/ for
	 * /Game/Foo/MyLevel). Empty exactly when GetScriptsFolder's rules find no such folder.
	 */
	static FString GetUIFolder(UWorld* World);

	/**
	 * #838: absolute path to World's Packages/ folder, beside Scripts/ (Content/Foo/Packages/ for /Game/Foo/MyLevel): the script
	 * packages the level authors (FMantleScriptPackages). Empty exactly when GetScriptsFolder's rules find no such folder. Package
	 * files are never place scripts: CollectScriptFiles does not read them.
	 */
	static FString GetPackagesFolder(UWorld* World);

	/**
	 * Every *.rml and *.rcss under UIFolder (extension matched ignoring case, so Check can name a
	 * "Shop.RML"), sorted by path so a repeat export is byte-identical. #556 item 5: OutImages gets
	 * every .png, .jpg and .jpeg file there (any case), sorted the same way, each with its size and
	 * the format its first bytes say. Anything else under UI/ is ignored.
	 */
	static void CollectUIDocumentFiles(
		const FString& UIFolder,
		TArray<FMantleUIDocumentFile>& OutFiles,
		TArray<FMantleUIImageFile>& OutImages);

	/**
	 * What Publish would refuse about Files: more than FMantlePlaceLimits::MaxUIDocuments, a path
	 * too long or outside the path rule, a file over MaxUIDocumentBytes, or two paths that differ
	 * only in case. Empty when every file can be published.
	 */
	static TArray<FMantleUIDocumentProblem> CheckUIDocumentFiles(const TArray<FMantleUIDocumentFile>& Files);

	/**
	 * #556 item 5: what Publish would refuse about Images: more than FMantlePlaceLimits::MaxUIImages,
	 * a path too long or outside the path rule (.png or .jpg, lower case), a file over
	 * MaxUIImageBytes, content that is not PNG or JPEG whatever its extension, or a path that
	 * differs only in case from another image's or a document's (Files). Empty when all can publish.
	 */
	static TArray<FMantleUIDocumentProblem> CheckUIImageFiles(const TArray<FMantleUIImageFile>& Images, const TArray<FMantleUIDocumentFile>& Files);

	/**
	 * #556 item 5: image references in Files' text (an `src`, `image(...)` or `url(...)` value, or
	 * any other path ending in .png, .jpg or .jpeg) that name no image of Images once resolved as
	 * players' games resolve them: relative to the referencing file's folder, or from the place's
	 * root with a leading '/'. Warnings: a script can still set a working source at run time.
	 */
	static TArray<FMantleUIDocumentProblem> CheckUIImageReferences(const TArray<FMantleUIDocumentFile>& Files, const TArray<FMantleUIImageFile>& Images);

	/** "png" or "jpeg" when Head (a file's first bytes, at least 8 for a PNG) carries that format's signature; empty otherwise. */
	static FString SniffUIImageFormat(TConstArrayView<uint8> Head);

	/**
	 * #549 P6a: what players' games refuse in Files' text, through the same scans the game runs
	 * (MantleRmlDocumentRules): a refused construct, a file too big, or too deep a nesting. Check and
	 * Export both refuse what it returns. OutWarnings, when given, gets what only warns: a style that
	 * does not draw yet, or a body bound to another data model. The game also limits a document's load
	 * time and element count when it opens it; those show in Test locally, not here. Files over
	 * MaxUIDocumentBytes are skipped: CheckUIDocumentFiles refuses them.
	 */
	static TArray<FMantleUIDocumentProblem> CheckUIDocumentContent(
		const TArray<FMantleUIDocumentFile>& Files,
		TArray<FMantleUIDocumentProblem>* OutWarnings = nullptr);

	/**
	 * Deterministic uid_ ids for Paths, one per path in the same order: "uid_" and the path under
	 * UI/ sanitised, extension included (Shop.rml and Shop.rcss differ). Collisions take a numeric
	 * suffix, as script ids do.
	 */
	static TArray<FString> MakeUIDocumentIds(const TArray<FString>& Paths);

	/** Transpiles one TypeScript source with the plugin's vendored compiler bundle. */
	static bool CompileTypeScript(const FString& Source, FString& OutJavaScript, FMantleScriptError& OutError);
};
