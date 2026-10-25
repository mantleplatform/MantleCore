// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleGlowMaterial.h"
#include "MantlePlaceValidator.h"
#include "MantleWaterMaterial.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/WeakObjectPtrTemplates.h"

class AActor;
class AStaticMeshActor;
class UAnimSequence;
class UAudioComponent;
class UInstancedStaticMeshComponent;
class UMaterialInterface;
class UMantleVehicleSource;
class UObject;
class USkeletalMesh;
class USkeletalMeshComponent;
class USoundBase;
class USoundWave;
class UStaticMesh;
class UStaticMeshComponent;
class UTexture;
class UWorld;

/**
 * The numbers Check this place and the converter hold an uploaded place asset to: limits.json
 * placeAssets (F1 #335) and contentLimits.any_upload, read from the plugin's copy, plus the
 * moderation classifier's image cap (backend assets.MaxScreenedImageBytes, 8 MiB), which is not in
 * limits.json. The cook worker refuses with the same numbers.
 */
struct MANTLEEDITOR_API FMantlePlaceAssetLimits
{
	int32 MaxTriangles = 500000;
	int32 MaxVertices = 1500000;
	int32 MaxSections = 8;
	int32 MaxTextureSize = 2048;
	int64 MaxTextureBytes = 8 << 20;
	int64 MaxUploadBytes = 64 << 20;

	/**
	 * limits.json placeAssets.sound (#324): one uploaded sound, a canonical PCM16 WAV the cook
	 * worker turns into MAU1. The per-place budget is the decoded samples of every distinct sound.
	 */
	int32 MinSoundSampleRate = 22050;
	int32 MaxSoundSampleRate = 48000;
	double MaxSoundSeconds = 60.0;
	int64 MaxSoundSourceBytes = 12 << 20;
	int64 MaxPlaceSoundDecodedBytes = 64 << 20;

	/** placeAssets.staticMesh.lods (#470): LODs with LOD0, and each LOD's triangle cap, LOD0 first. */
	int32 MaxLODs = 3;
	TArray<int32> LODMaxTriangles = { 500000, 250000, 125000 };

	/** placeAssets.staticMesh.collision (#470). */
	int32 MaxCollisionPrimitives = 64;
	int32 MaxConvexHulls = 64;
	int32 MaxHullVertices = 255;
	int32 MaxTotalHullVertices = 8192;

	/** LOD LODIndex's triangle cap: its lods.maxTriangles entry, else MaxTriangles. */
	int32 LODTriangleCap(const int32 LODIndex) const
	{
		return LODMaxTriangles.IsValidIndex(LODIndex) ? LODMaxTriangles[LODIndex] : MaxTriangles;
	}

	/**
	 * limits.json placeAssets.skeletalMesh and animClip (#469): a skeletal mesh on its own skeleton (MSK1 role 3, one
	 * LOD) and a clip sampled over its bones (MAN1 with its skeleton).
	 */
	int32 MaxSkeletalTriangles = 100000;
	int32 MaxSkeletalSections = 8;
	int32 MaxSkeletalBones = 256;
	double MaxClipSeconds = 60.0;
	double MaxClipFrameRate = 60.0;
	int64 MaxClipCookedBytes = 8 << 20;

	/** The rate the plugin samples a place clip at (within MaxClipFrameRate). */
	static constexpr float ClipFrameRate = 30.0f;

	/** The plugin's limits.json, or these defaults when it cannot be read. */
	static FMantlePlaceAssetLimits FromContract();
};

/**
 * What one render LOD of a static mesh counts as for the place mesh limits (#804): its triangles, its vertices as the
 * export writes them (a LOD whose material sections share vertices is split at export, one copy of the shared vertices
 * per section), and its material slots in upload order (FMantlePlaceAssets::UploadSlots' rule, for that LOD).
 */
struct MANTLEEDITOR_API FMantleMeshLODCount
{
	int32 Triangles = 0;
	int32 Vertices = 0;
	TArray<int32> Slots;
};

/** Everything flattening a material to baked PBR loses, found from its graph and properties (FAB_ASSETS_252.md section 3). */
enum class EMantleMaterialLoss : uint32
{
	None = 0,
	Animation = 1 << 0,
	WorldPositionOffset = 1 << 1,
	WorldAligned = 1 << 2,
	Refraction = 1 << 3,
	Subsurface = 1 << 4,
	Parallax = 1 << 5,
	VertexColor = 1 << 6,
	DetailNormal = 1 << 7,
	PixelDepthOffset = 1 << 8,
	Displacement = 1 << 9,
	SpecialShading = 1 << 10,
	Unlit = 1 << 11,
	BlendMode = 1 << 12,
	NotSurface = 1 << 13,
};
ENUM_CLASS_FLAGS(EMantleMaterialLoss);

/**
 * #533: what AnalyzeMaterial recognised in a graph that the material document carries beyond GLTFExporter's
 * bake. Each clears its loss only for the exact pattern it names (see AnalyzeMaterial); anything else is still
 * a loss and warns.
 */
struct MANTLEEDITOR_API FMantleMaterialFidelity2
{
	/**
	 * Per slot (base colour, normal, ORM, emissive): the source texture the slot publishes instead of the bake,
	 * for a panned, world-aligned or layer-blended slot. GLTFExporter bakes those wrongly: it exports a source
	 * texture only when a sample reads plain Texture Coordinates, and bakes anything else over one UV tile at
	 * game time 0 (MaterialBaking's FSceneViewFamily time), so a panned sample comes out as one tile of its
	 * tiled texture at the bake size, a world-aligned one as whatever world position the bake quad has, and a
	 * layer blend as the layer alone (the bake's vertex colour is white).
	 */
	TWeakObjectPtr<UTexture> SourceTextures[4];

	/** A source slot's tiling on the first UV (Texture Coordinate's UTiling and VTiling), for its uvTransforms. */
	double SourceTiling[4][2] = { { 1.0, 1.0 }, { 1.0, 1.0 }, { 1.0, 1.0 }, { 1.0, 1.0 } };

	/** The base colour's source keeps its alpha: the material's opacity (mask) reads the same sample's A. */
	bool bSourceAlpha = false;

	/**
	 * An ORM source, packed as glTF's (R occlusion, G roughness, B metallic): the metallic factor it publishes
	 * with (0 when Metallic is the constant 0, else 1) and the occlusion strength (0 when Ambient Occlusion is
	 * not connected, else 1).
	 */
	double OrmMetallicFactor = 1.0;
	double OrmOcclusionStrength = 1.0;

	/** uvPanning: per slot, a Panner's SpeedX and SpeedY (UV units a second). */
	bool bPanning[4] = { false, false, false, false };
	double Panning[4][2] = { { 0.0, 0.0 }, { 0.0, 0.0 }, { 0.0, 0.0 }, { 0.0, 0.0 } };

	/** wind: SimpleGrassWind on World Position Offset, its constant intensity and speed, and its weight (vertexColorR, uvV, uniform). */
	bool bWind = false;
	double WindIntensity = 0.0;
	double WindSpeed = 0.0;
	FString WindWeight;

	/** worldAligned: base colour, normal and ORM from WorldAlignedTexture / WorldAlignedNormal with one constant size and contrast. */
	bool bWorldAligned = false;
	double TileSizeCm = 0.0;
	double Sharpness = 1.0;

	/** layerBlend: a Lerp by one vertex-colour channel (r, g, b, a) whose B is the layer's texture per slot (base colour, normal, ORM). */
	bool bLayerBlend = false;
	FString LayerChannel;
	TWeakObjectPtr<UTexture> LayerTextures[3];

	/**
	 * foliage (#628): the Two Sided Foliage shading model on a masked, two-sided material (exact, from the shading model).
	 * The values the document carries; the graph's own subsurface colour is not read (the base colour stands in for it).
	 */
	bool bFoliage = false;
	double FoliageTint[3] = { 1.0, 1.0, 1.0 };
	double FoliageStrength = 1.0;
	double FoliageVertexColorWeight = 0.0;

	/**
	 * water (#628): a translucent material whose opacity is a depth fade. Opacity (and the deep colour, when the graph
	 * gives one) is baked to a constant, because the bake sees the fade's near end and writes opacity 0.
	 */
	FMantleWaterBake Water;

	/**
	 * additive (#628): Unreal's Additive blending, published as alphaMode additive (emissive-driven, unlit; exact from the blend
	 * mode). bAdditiveLit: the source was lit, so lighting on the surface is dropped; bAdditiveRim: a Fresnel in its emissive
	 * (a rim glow) is dropped (FMantleGlowMaterial::HasFresnel).
	 */
	bool bAdditive = false;
	bool bAdditiveLit = false;
	bool bAdditiveRim = false;

	/** see-through glass (#628): an unlit translucent material whose emissive is the scene behind it, published as tinted glass. */
	FMantleSeeThroughGlass Glass;

	bool HasSource(int32 Slot) const { return Slot >= 0 && Slot < 4 && SourceTextures[Slot].IsValid(); }
};

/** What FMantlePlaceAssets::AnalyzeMaterial found in one material (and its parents and functions). */
struct MANTLEEDITOR_API FMantleMaterialAnalysis
{
	EMantleMaterialLoss Losses = EMantleMaterialLoss::None;

	/** SpecialShading: the shading model's name ("Clear Coat"); BlendMode: the blend mode's ("Additive"). */
	FString ShadingModelName;
	FString BlendModeName;

	/** Textures the flattened inputs sample (world position offset excluded: it is dropped anyway). */
	TArray<TWeakObjectPtr<UTexture>> Textures;

	/** Every asset the bake depends on: the material, its parents, functions, textures, parameter collections. The conversion cache keys on their packages. */
	TArray<TWeakObjectPtr<UObject>> Dependencies;

	/** FMantlePlaceAssets::PublishesAsCarPaint: its clear coat is kept as shading carPaint, so it is not a SpecialShading loss. */
	bool bCarPaint = false;

	/**
	 * #471: the only vertex colour the flattened outputs read is Vertex Color's RGB multiplying the base
	 * colour (directly, or through Multiply and reroute nodes). The material publishes with vertexColor
	 * multiplyBaseColor, so it is not a VertexColor loss. Any other use (a single channel, a blend, emissive,
	 * inside a function) is the loss, and this is false.
	 */
	bool bVertexColorTintsBaseColor = false;

	/** #533: panning, wind, world-aligned mapping and layer blends the document carries, so they are not losses. */
	FMantleMaterialFidelity2 Fidelity2;

	/**
	 * #628: a flattened output reads a per-instance value (Per Instance Random, Per Instance Custom Data, Per Instance Fade Amount).
	 * It has no value at bake time, so the bake is made at a representative one (FMantlePlaceAssets::NeutralisePerInstanceValues)
	 * and the per-instance variation is averaged out.
	 */
	bool bPerInstanceVariation = false;

	bool Has(EMantleMaterialLoss Loss) const { return EnumHasAnyFlags(Losses, Loss); }
};

/**
 * The car-paint finish of a Clear Coat source material (#487): the material document's carPaint
 * object. Defaults are the schema's (schema/material.schema.json), used where the material has no
 * value of its own.
 */
struct MANTLEEDITOR_API FMantleCarPaintFinish
{
	double FlakeDensity = 0.5;
	double FlakeSize = 0.06;
	double FlakeTint[3] = { 1.0, 1.0, 1.0 };
	double ClearCoat = 1.0;
	double ClearCoatRoughness = 0.05;
};

/** One mesh the level uses that is not in the Mantle library: it is converted and uploaded. */
struct MANTLEEDITOR_API FMantleGatheredMesh
{
	TWeakObjectPtr<UStaticMesh> Mesh;
	FString Key;
	FString Name;

	/** Labels of the actors using it, in level order. */
	TArray<FString> UsedBy;
};

/** One material the level uses (a slot of an uploaded mesh, or an override) that is not in the Mantle library. */
struct MANTLEEDITOR_API FMantleGatheredMaterial
{
	TWeakObjectPtr<UMaterialInterface> Material;
	FString Key;
	FString Name;
	TArray<FString> UsedBy;
};

/** One sound wave an AmbientSound plays that is not in the Mantle library (#324): it is converted to a WAV and uploaded. */
struct MANTLEEDITOR_API FMantleGatheredSound
{
	TWeakObjectPtr<USoundWave> Sound;
	FString Key;
	FString Name;

	/** Labels of the AmbientSounds playing it, in level order. */
	TArray<FString> UsedBy;
};

/** One skeletal mesh a SkeletalMesh actor shows (#469): converted to a skinned GLB on its own skeleton and uploaded. */
struct MANTLEEDITOR_API FMantleGatheredSkeletalMesh
{
	TWeakObjectPtr<USkeletalMesh> Mesh;
	FString Key;
	FString Name;
	TArray<FString> UsedBy;
};

/**
 * One animation sequence a SkeletalMesh actor plays (#469), sampled over the bones of the mesh it plays on: one upload
 * per (sequence, mesh), since the clip carries that mesh's skeleton.
 */
struct MANTLEEDITOR_API FMantleGatheredClip
{
	TWeakObjectPtr<UAnimSequence> Sequence;
	TWeakObjectPtr<USkeletalMesh> Mesh;
	FString Key;
	FString Name;
	TArray<FString> UsedBy;
};

/** What a SkeletalMesh actor publishes (#469), read by FMantlePlaceAssets::ReadSkeletalComponent. */
struct MANTLEEDITOR_API FMantleSkeletalPlayback
{
	const USkeletalMesh* Mesh = nullptr;

	/** Null: the mesh holds its reference pose. */
	const UAnimSequence* Sequence = nullptr;

	bool bLoop = true;
	double PlayRate = 1.0;
	double StartOffset = 0.0;
};

/**
 * One texture a decal publishes (#472): uploaded as it is, never through a material. The same
 * texture as a decal's base colour and as another's normal map is two entries (two roles).
 */
struct MANTLEEDITOR_API FMantleGatheredTexture
{
	TWeakObjectPtr<UTexture> Texture;

	/** FMantlePlaceAssets::DecalTextureKey. */
	FString Key;
	FString Name;

	/** A normal map rather than a base colour. */
	bool bNormal = false;

	/** #488: first gathered as a vehicle's livery (a base colour with alpha as coverage), not a decal's: for messages. */
	bool bLivery = false;

	TArray<FString> UsedBy;
};

/**
 * One entry of the level's Script Assets (Mantle Place Settings): a mesh, material or sound
 * published with the place, listed in assets[] with no entity using it, so its scripts can spawn
 * or play it by id (#462, #459).
 */
struct MANTLEEDITOR_API FMantleScriptAsset
{
	/** Its position in the Script Assets list, for messages. */
	int32 Index = INDEX_NONE;

	/** The reference as the list holds it. */
	FSoftObjectPath Path;

	/** The asset, loaded; null when nothing loads from Path. */
	UObject* Object = nullptr;
};

/** The level's landscape (#466): converted to a heightfield (and a weightmap) and uploaded. */
struct MANTLEEDITOR_API FMantleGatheredTerrain
{
	TWeakObjectPtr<AActor> Landscape;

	/** FMantleTerrainExport::HeightfieldKey. */
	FString Key;

	/** The landscape's label. */
	FString Name;
};

/**
 * One creator vehicle a Mantle Vehicle Spawn names (#435): uploaded as one Vehicle asset (its body with the vehicle
 * document), after the materials of its body and tires, which are gathered like a mesh's.
 */
struct MANTLEEDITOR_API FMantleGatheredVehicle
{
	TWeakObjectPtr<UMantleVehicleSource> Vehicle;

	/** FMantleVehicleConverter::VehicleKey. */
	FString Key;
	FString Name;

	/** Labels of the spawns naming it, in level order. */
	TArray<FString> UsedBy;
};

/**
 * #898: what a built weapon's settings file names besides its 3D model, loaded from the weapon's folder (the builder copies each
 * there under the local name it writes): each extra part's model, (#905) the round's model and the attack sounds.
 */
struct MANTLEEDITOR_API FMantleBuiltWeaponParts
{
	/** One per attachments[] entry, in order: a static or skeletal mesh. */
	TArray<UObject*> Attachments;

	/** #905: projectile.mesh, the static or skeletal mesh a launcher's round or an arrow flies as; null when the file names none. */
	UObject* Projectile = nullptr;

	/** sounds.fire and sounds.fireSuppressed; null when the file names none. */
	USoundWave* Fire = nullptr;
	USoundWave* FireSuppressed = nullptr;
};

/**
 * One weapon a Mantle Weapon Spawn names (#789): a weapon the weapon builder made (a folder of Content/MantleWeapons). It uploads
 * as one Weapon asset, its settings file with the uploaded ids of its 3D model, its extra parts' models (#898), its attack sounds
 * and their materials, after them all; each model is gathered like any mesh or skeletal mesh, each sound like a SoundEmitter's.
 */
struct MANTLEEDITOR_API FMantleGatheredWeapon
{
	/** FMantlePlaceAssets::WeaponKey. */
	FString Key;

	/** The weapon's folder name under /Game/MantleWeapons. */
	FString Name;

	/** Its 3D model (SKM_<Name>); every weapon has one (#897: a fist weapon's too). */
	TWeakObjectPtr<USkeletalMesh> Mesh;

	/** Labels of the spawns naming it, in level order. */
	TArray<FString> UsedBy;
};

/**
 * Step 1 of publishing a Fab-built level (F5 #339, FAB_ASSETS_252.md section 6): every static
 * mesh and material the level uses that is not in the Mantle library. For a StaticMesh actor whose
 * mesh is not a library asset: the mesh and each slot's effective material (the override, else
 * the mesh's own slot material, else Unreal's default material). For a library mesh: only its
 * overrides, as before. Library ids short-circuit. And every Decal's textures (#472).
 */
struct MANTLEEDITOR_API FMantlePlaceDependencies
{
	TArray<FMantleGatheredMesh> Meshes;
	TArray<FMantleGatheredMaterial> Materials;
	TArray<FMantleGatheredTexture> Textures;

	/** AmbientSounds' own sound waves (#324), in level order. */
	TArray<FMantleGatheredSound> Sounds;

	/** SkeletalMesh actors' meshes and the sequences they play (#469), in level order. */
	TArray<FMantleGatheredSkeletalMesh> SkeletalMeshes;
	TArray<FMantleGatheredClip> Clips;

	/** The level's landscape (#466), whose layer materials are in Materials. */
	TArray<FMantleGatheredTerrain> Terrains;

	/** Mantle Vehicle Spawns' vehicles (#435), in level order. */
	TArray<FMantleGatheredVehicle> Vehicles;

	/** Mantle Weapon Spawns' built weapons (#789), in level order. */
	TArray<FMantleGatheredWeapon> Weapons;

	bool NeedsUploads() const
	{
		return Meshes.Num() > 0 || Materials.Num() > 0 || Textures.Num() > 0 || Sounds.Num() > 0 || SkeletalMeshes.Num() > 0 || Clips.Num() > 0
			|| Terrains.Num() > 0 || Vehicles.Num() > 0 || Weapons.Num() > 0;
	}

	/**
	 * Adds one SkeletalMesh actor's mesh, the effective material of each of its slots (library ones skipped) and the
	 * sequence it plays (#469), when FMantlePlaceAssets::ReadSkeletalComponent accepts it; Check reports it otherwise.
	 */
	void AddSkeletalActor(const AActor* Actor);

	/**
	 * Adds one Mantle Vehicle Spawn's vehicle (#435) and the materials its body and tires draw with (library ones
	 * skipped: a vehicle cannot name them). Nothing for a spawn without a vehicle; Check says so.
	 */
	void AddVehicleSpawn(const AActor* Actor);

	/**
	 * Adds one Mantle Weapon Spawn's built weapon (#789): its 3D model as a skeletal mesh (with the materials of its slots), and the
	 * weapon itself, read from Content/MantleWeapons/<Weapon>. Nothing for a spawn without a weapon, one whose files are missing
	 * (the validator names it), or one that names a stored item.
	 */
	void AddWeaponSpawn(const AActor* Actor);

	/**
	 * #840: adds the weapon builder's weapon in Content/MantleWeapons/<WeaponName>, as AddWeaponSpawn does for a spawn, with Label as
	 * its user. False, adding nothing, when its settings file or its 3D model is missing (every weapon has one, #897: a fist
	 * weapon's too).
	 */
	bool AddBuiltWeapon(const FString& WeaponName, const FString& Label);

	/** Adds one Decal actor's textures (#472), as FMantlePlaceExporter::ReadDecal reads them; nothing for a decal it refuses. */
	void AddDecal(const AActor* Actor);

	/**
	 * Adds a sprites Effect's texture (#628) as a decal's base colour is added: uploaded as it is, alpha kept. Used for a
	 * Mantle Effect actor's SpritesTexture and for the texture FMantleEffectExport::Translate names on a translated
	 * particle emitter; nothing for a null texture.
	 */
	void AddEffectTexture(UTexture* Texture, const FString& Label);

	/**
	 * Adds one Mantle Scatter Volume's meshes (#530) as a Static Mesh Actor's are added: a project mesh with each
	 * slot's effective material (its override, else the mesh's own), a library mesh with only its overrides.
	 */
	void AddScatter(const AActor* Actor);

	/** The actors the exporter publishes, the same ones in the same order, then the level's Script Assets. */
	static FMantlePlaceDependencies Gather(UWorld* World);

	/** Adds one StaticMesh actor's mesh and materials (library ones skipped). */
	void AddActor(const AStaticMeshActor* Actor);

	/**
	 * Adds the level's Script Assets (Mantle Place Settings) under FMantlePlaceAssets::ScriptAssetsLabel:
	 * a mesh with its own slot materials (AddMesh), a material, a sound wave (AddSound). Library
	 * assets, and entries that are missing or of a type the list does not take, are skipped: Check
	 * names the latter (FMantlePlaceAssets::CheckScriptAsset).
	 */
	void AddScriptAssets(UWorld* World);

	/**
	 * Adds the static meshes the level's spline meshes are baked into (#628, FMantleSplineMeshBake), each with its
	 * slots' materials, under its owning actor's label: Export writes them as StaticMesh entities.
	 */
	void AddSplineMeshes(UWorld* World);

	/** Adds a mesh no component uses, with each slot's own material (FMantlePlaceAssets::MeshSlotMaterial). Library meshes are skipped. */
	void AddMesh(const UStaticMesh* Mesh, const FString& Label);

	/** Adds one material under Label unless it is a library material. */
	void AddMaterial(UMaterialInterface* Material, const FString& Label);

	/** Adds one sound under Label when it is a sound wave that is neither a library sound nor generated while the game runs (#324). */
	void AddSound(const USoundBase* Sound, const FString& Label);

	/**
	 * Adds one AmbientSound's sound when it is a sound wave that is neither a library sound nor
	 * generated while the game runs (#324). Anything else is Check's to report, through
	 * FMantlePlaceExporter::ReadSoundEmitter.
	 */
	void AddSoundActor(const AActor* Actor);

	/**
	 * AddSoundActor for one audio component under Label: an AmbientSound's, or a Blueprint prefab's
	 * audio part (#468), which Export writes as a SoundEmitter entity.
	 */
	void AddSoundComponent(const UAudioComponent* Audio, const FString& Label);

	/**
	 * Adds one mesh component's mesh and materials under Label: a StaticMesh actor's, or an
	 * exported instance source's (#467), whose meshes upload exactly like a StaticMesh's.
	 */
	void AddComponent(const UStaticMeshComponent* Component, const FString& Label, const UStaticMesh* MeshOverride = nullptr);

	/**
	 * AddSkeletalActor for one skeletal mesh component under Label: a SkeletalMesh actor's, or a Blueprint prefab's
	 * skeletal part (#628), which Export writes as a SkeletalMesh entity.
	 */
	void AddSkeletalComponent(const USkeletalMeshComponent* Component, const FString& Label);

	/**
	 * Adds a skeletal mesh no level actor uses (#729: a weapon's model for Test Locally), with the material of each of its
	 * slots (the mesh's own; library ones skipped). Nothing for a null mesh; the converter checks the mesh against the limits.
	 */
	void AddSkeletalMesh(const USkeletalMesh* Mesh, const FString& Label);

	/**
	 * Adds a landscape (#466): its heightfield and weightmap to convert, and the material of every
	 * Terrain Layers entry in Mantle Place Settings (library ones skipped), which Export uses for the
	 * painted layers.
	 */
	void AddTerrain(const AActor* Landscape);
};

/**
 * The shared rules for creator-uploaded place assets, one definition for the exporter, Check this
 * place and the converter, so the three never disagree (F5 #339).
 */
class MANTLEEDITOR_API FMantlePlaceAssets
{
public:
	/** "mesh:" + the object path: the local key the converter, the upload queue and the exporter share. */
	static FString MeshKey(const UStaticMesh* Mesh);

	/**
	 * #804: the object name for a transient mesh the plugin bakes (the mirror bake's, the spline bake's): IntendedName, made unique in
	 * Outer. The engine numbers it, and that number only grows in an editor session, so it must not reach a published file: a name
	 * that ends in a digit (a chunk index, an all-digit hash tail) would be read by FName as such a number and lost. A name ending in a
	 * digit therefore gets an underscore after it, which PublishedBakedMeshName takes off again.
	 */
	static FName UniqueBakedMeshName(UObject* Outer, const FString& IntendedName);

	/** #804: the name a mesh is written under in its GLB: a baked mesh's IntendedName (no counter), any other mesh's own name. */
	static FString PublishedMeshName(const UStaticMesh& Mesh);

	/** "material:" + the object path. */
	static FString MaterialKey(const UMaterialInterface* Material);

	/** "sound:" + the object path (#324). */
	static FString SoundKey(const USoundBase* Sound);

	/** "skeletalmesh:" + the object path (#469). */
	static FString SkeletalMeshKey(const USkeletalMesh* Mesh);

	/** "weapon:/Game/MantleWeapons/" + the weapon builder's folder name (#789). */
	static FString WeaponKey(const FString& WeaponName);

	/**
	 * What keeps the built weapon WeaponName's settings file from publishing, as a clause following "The weapon 'X'": empty when it
	 * can. Unreadable text, and (#898) an extra part's model, (#905) the round's model (projectile.mesh) or an attack sound missing
	 * from the weapon's folder, or a sound that breaks the sound rule (CheckSoundRule). OutParts, when given, gets what the file names.
	 */
	static FString WeaponDocumentProblem(const FString& WeaponName, const FString& DocumentText, FMantleBuiltWeaponParts* OutParts = nullptr);

	/** "animclip:" + the sequence's object path + "@" + the mesh's (#469): a clip is sampled over one mesh's bones. */
	static FString ClipKey(const UAnimSequence* Sequence, const USkeletalMesh* Mesh);

	/**
	 * The one rule for a SkeletalMesh actor's component (#469), so Export, Check this place and the converter never
	 * disagree. Its skeletal mesh, and what it plays: in Use Animation Asset mode, its Animation Sequence (or none: the
	 * reference pose) with the component's Looping, Play Rate and Initial Position (not Playing holds the pose there,
	 * play rate 0); in Use Animation Blueprint mode with no Anim Class, the reference pose. False with OutWhy, a clause
	 * following the actor's name, and OutFix, the sentence saying what to do, when it has no mesh, plays an Animation
	 * Blueprint or a custom mode, plays an asset that is not an Animation Sequence (a montage, a blend space) or one made
	 * for another skeleton, or plays at a rate or from a position the place file cannot hold.
	 */
	static bool ReadSkeletalComponent(const USkeletalMeshComponent* Component, FMantleSkeletalPlayback& Out, FString& OutWhy, FString& OutFix);

	/** A skeletal mesh's material slots in the order its uploaded GLB lists its glTF materials (LOD 0's sections, first use). */
	static TArray<int32> SkeletalUploadSlots(const USkeletalMesh* Mesh);

	/** The material a skeletal mesh component's slot renders with: its override, else the mesh's, else Unreal's default surface material. */
	static UMaterialInterface* EffectiveSkeletalMaterial(const USkeletalMeshComponent* Component, int32 Slot);

	/**
	 * A skeletal mesh against placeAssets.skeletalMesh (#469): no render data, more bones, triangles (LOD 0) or material
	 * slots than it allows are errors (the cook refuses them). UsedBy is appended to the message.
	 */
	static void CheckSkeletalMesh(const USkeletalMesh* Mesh, const FString& UsedBy, const FMantlePlaceAssetLimits& Limits, TArray<FMantleValidationIssue>& OutIssues);

	/** A sequence against placeAssets.animClip (#469): longer than it allows is an error. UsedBy is appended to the message. */
	static void CheckClip(const UAnimSequence* Sequence, const FString& UsedBy, const FMantlePlaceAssetLimits& Limits, TArray<FMantleValidationIssue>& OutIssues);

	/**
	 * The one rule for a creator's own sound (#324), from what the editor knows about it without
	 * reading its samples, so Export, Check this place and the converter never disagree: a sound
	 * wave (not a Sound Cue, MetaSound or other graph, and not generated while the game runs) with
	 * imported audio, mono or stereo, recorded at a rate and no longer than limits.json
	 * placeAssets.sound allows. False with OutWhy, a clause that follows the sound's name ("is a
	 * Sound Cue, ..."), and OutFix, the sentence saying what to do. Library sounds never come here.
	 */
	static bool CheckSoundRule(const USoundBase* Sound, const FMantlePlaceAssetLimits& Limits, FString& OutWhy, FString& OutFix);

	/**
	 * The sound a placed sound publishes as (#818): a Sound Cue is a graph players' clients don't run, so it publishes as the
	 * first sound wave it plays that passes CheckSoundRule (else its first wave of any kind); any other sound is itself. Null
	 * for a Sound Cue that plays no sound wave at all.
	 */
	static const USoundBase* PublishedSound(const USoundBase* Sound);

	/** A sound wave's decoded size, what the place audio budget counts: frames x channels x 2 bytes, from its import metadata. */
	static int64 SoundDecodedBytes(const USoundWave* Sound);

	/**
	 * Check this place's place-wide sound rules (#324): the decoded audio of every distinct sound the
	 * level uploads within placeAssets.sound.maxPlaceDecodedBytes (an error), and a warning per sound
	 * with waveform edits made in Unreal, which are not published.
	 */
	static void CheckSounds(const FMantlePlaceDependencies& Dependencies, const FMantlePlaceAssetLimits& Limits, TArray<FMantleValidationIssue>& OutIssues);

	/**
	 * "decaltexture:" + color or normal + ":" + the object path (#472): the local key a decal's
	 * texture is gathered under. The converter maps it to its converted texture's content key
	 * (FMantlePlaceAssetSet::DecalTextures), which is what is uploaded and recorded.
	 */
	static FString DecalTextureKey(const UTexture* Texture, bool bNormal);

	/**
	 * The id written when no upload is known for Key (offline export, preview, Check): ast_u + the
	 * MD5 of the key. Schema-valid and stable, but not an asset the server holds, so a publish
	 * without F6's upload is refused as assets_missing rather than pointing at someone else's.
	 */
	static FString ProvisionalId(const FString& Key);

	/** True when Object is a Mantle library asset (it is published by its permanent id and never uploaded). */
	static bool IsLibraryAsset(const UObject* Object);

	/**
	 * The mesh's material slots in the order its uploaded GLB lists its glTF materials, which is
	 * the MRM2 slot order the cook gives them (F1/F2): distinct LOD 0 section slots, sections
	 * ordered by first vertex. materials[i] of an uploaded mesh is the effective material of slot
	 * UploadSlots(Mesh)[i]. Empty when the mesh has no render data.
	 */
	static TArray<int32> UploadSlots(const UStaticMesh* Mesh);

	/**
	 * Render LOD LODIndex of Mesh counted for the limits (the same counting Check this place and the publish use). False,
	 * with Out empty, when the mesh has no such LOD.
	 */
	static bool CountMeshLOD(const UStaticMesh* Mesh, int32 LODIndex, FMantleMeshLODCount& Out);

	/**
	 * The render LOD a place mesh is published from (#804): 0 when LOD 0 is within the triangle and vertex limits;
	 * otherwise the first lower LOD that is within both and draws with the same material slots as LOD 0 (it is then
	 * published as the mesh's LOD 0, the LODs after it as its lower LODs); INDEX_NONE when there is none, or the mesh has
	 * no render data. The creator's mesh is only read.
	 */
	static int32 PublishedLODIndex(const UStaticMesh* Mesh, const FMantlePlaceAssetLimits& Limits);

	/** The material a slot renders with: the component's override, else the mesh's slot material, else Unreal's default surface material. */
	static UMaterialInterface* EffectiveMaterial(const UStaticMeshComponent* Component, int32 Slot);

	/** The material a slot of Mesh renders with where no component overrides it: the mesh's slot material, else Unreal's default surface material. */
	static UMaterialInterface* MeshSlotMaterial(const UStaticMesh* Mesh, int32 Slot);

	/** What a gathered Script Asset's UsedBy names: "Script Assets". */
	static const TCHAR* ScriptAssetsLabel;

	/**
	 * The level's Script Assets (its Mantle Place Settings actor's list), loaded, in list order:
	 * unset entries skipped, and each asset once however often it is listed. Empty when the level
	 * has no settings actor.
	 */
	static TArray<FMantleScriptAsset> ScriptAssets(UWorld* World);

	/**
	 * The rules for one Script Asset beyond the ones placed content shares through Gather (mesh
	 * limits, material losses, the place's sound budget): it must load, be a static mesh, material or
	 * sound wave, and a sound must pass CheckSoundRule (library sounds excepted). False with
	 * OutProblem, a clause that follows "Script asset '<path>'", and OutRemedy. Export and Check both
	 * call this, so Check cannot pass an entry Publish refuses.
	 */
	static bool CheckScriptAsset(const FMantleScriptAsset& Asset, const FMantlePlaceAssetLimits& Limits, FString& OutProblem, FString& OutRemedy);

	/**
	 * What flattening Material loses, and what its bake depends on. Reads the graph; compiles nothing.
	 *
	 * #533 recognises four patterns and carries them in the document instead of reporting their loss (each
	 * through reroutes only; any other shape is walked as before and warns):
	 * - uvPanning: Base Color, Normal or Emissive Color is a Texture Sample (or Texture Sample Parameter 2D)
	 *   whose UVs are a Panner with no Time or Speed input, over nothing or a Texture Coordinate on UV 0; the ORM
	 *   group (Roughness from G, Metallic from B or the constant 0, Ambient Occlusion from R or unconnected) is
	 *   one such sample. A panned base colour on a masked or translucent material also needs its opacity to be
	 *   that sample's A.
	 * - wind: World Position Offset is a SimpleGrassWind call with constant WindIntensity and WindSpeed (0..4),
	 *   no AdditionalWPO, and a WindWeight of Vertex Color's R, 1 - TexCoord[0].G, or the constant 1.
	 * - worldAligned (opaque only): Base Color is WorldAlignedTexture's XYZ Texture, Normal WorldAlignedNormal's,
	 *   the ORM group channels of one WorldAlignedTexture's, all with one constant TextureSize (the same on every
	 *   axis, 1..100000) and ProjectionTransitionContrast (1..16), and no WorldPosition input; each other of those
	 *   outputs unconnected or a constant.
	 * - layerBlend (opaque only, not with the vertex-colour tint): Base Color is a Lerp whose A and B are plain
	 *   samples on one UV and whose Alpha is one Vertex Color channel; Normal and the ORM group the same Lerp by
	 *   the same channel, or a plain sample (the layer then keeps the base's texture).
	 * At most one of wind, worldAligned and layerBlend is recognised (in that order).
	 */
	static FMantleMaterialAnalysis AnalyzeMaterial(const UMaterialInterface* Material);

	/**
	 * True when Material publishes as Mantle's car paint (shading carPaint, #487): a surface
	 * material whose only shading model is Clear Coat, opaque and one-sided (the schema allows car
	 * paint on nothing else). Any other Clear Coat material keeps the SpecialShading loss.
	 */
	static bool PublishesAsCarPaint(const UMaterialInterface* Material);

	/**
	 * Material's car-paint finish. Clear coat and its roughness come from what feeds the material's
	 * Clear Coat inputs (a Constant, or a scalar parameter as the instance sets it), else from a
	 * parameter named ClearCoat / ClearCoatRoughness; the flakes from parameters named
	 * FlakeDensity, FlakeSize (cm) and FlakeTint. Spaced names ("Clear Coat") count too. Anything
	 * not found keeps the schema's default, except that a material with none of the three flake
	 * parameters gets flake density 0: a plain clear coat. Values are not clamped here; the
	 * document clamps.
	 */
	static FMantleCarPaintFinish CarPaintFinish(const UMaterialInterface* Material);

	/**
	 * #628: a transient copy of Material (its instance chain, base graph and the functions it calls) in which each per-instance
	 * node is replaced by a constant at a representative value, for the bake: Per Instance Random 0.5, Per Instance Custom Data
	 * its default value, Per Instance Fade Amount 1. The bake has no instance, so the nodes read 0 there, which blacks out a
	 * base colour multiplied by Per Instance Random. Null when the material has none (or a chain this cannot copy: a dynamic
	 * instance); the copy lives as long as the pointer does, and the source is never touched.
	 */
	static TStrongObjectPtr<UMaterialInterface> NeutralisePerInstanceValues(const UMaterialInterface* Material);

	/** The sentence Check and the converter give a material whose bake averaged its per-instance variation, following "Material 'M_X': ". */
	static FString DescribePerInstanceVariation();

	/** The plain sentence Check this place shows for one loss, following "Material 'M_X': ". */
	static FString DescribeLoss(EMantleMaterialLoss Loss, const FMantleMaterialAnalysis& Analysis);

	/**
	 * A mesh against placeAssets.staticMesh: over the triangle, vertex or section limit, or without
	 * render data, is an error (the cook refuses it); Nanite is a warning (it exports as its
	 * fallback mesh). UsedBy is appended to the message.
	 *
	 * #804: over the triangle or vertex limit is not an error when a smaller version can be published instead, which
	 * is a warning saying so: the mesh's first lower LOD that fits (PublishedLODIndex), else a transient copy reduced
	 * with the editor's mesh reduction (FMantleMeshReduce::ReducedCopy). The creator's mesh is never changed. It is an
	 * error only when neither is possible.
	 */
	static void CheckMesh(const UStaticMesh* Mesh, const FString& UsedBy, const FMantlePlaceAssetLimits& Limits, TArray<FMantleValidationIssue>& OutIssues);

	/**
	 * A material's flattening losses, one warning each, and a warning per texture over the size cap
	 * ("downscaled to 2048" plus a superscript two). Never an error. Textures already in
	 * ReportedTextures are not repeated; the ones reported are added.
	 */
	static void CheckMaterial(const UMaterialInterface* Material, const FString& UsedBy, const FMantlePlaceAssetLimits& Limits,
		TArray<FMantleValidationIssue>& OutIssues, TSet<const UTexture*>* ReportedTextures = nullptr);

	/** Check this place's part for uploaded assets: every gathered mesh and material checked once, however many actors use it. */
	static void CheckLevel(UWorld* World, TArray<FMantleValidationIssue>& OutIssues);

	/** "used by 'Crate' and 3 more", or empty. */
	static FString DescribeUsers(const TArray<FString>& UsedBy);

	/** A texture's pixel size: its source when it has one, else its platform data. */
	static FIntPoint TextureSize(const UTexture* Texture);

	/**
	 * The one texture-size rule (#452): the size a texture of Size publishes at, Size itself when its
	 * longest side is within MaxTextureSize, else scaled down, aspect kept, so that side is MaxTextureSize.
	 * Only uploaded materials are held to it; a library material is Mantle's own cooked asset and never uploaded.
	 */
	static FIntPoint PublishedTextureSize(FIntPoint Size, int32 MaxTextureSize);

	/**
	 * The one message for that rule, shared by Check this place and the converter: false when Size fits,
	 * else true with OutIssue the warning (never an error) "<What> is WxH; it is downscaled to ... when published.".
	 */
	static bool TextureDownscaleIssue(const FString& What, FIntPoint Size, int32 MaxTextureSize, FMantleValidationIssue& OutIssue);
};
