// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleApiClient.h"
#include "MantleGlbWriter.h"
#include "MantlePlaceAssets.h"
#include "MantlePlaceMaterial.h"
#include "MantlePlaceValidator.h"

class AActor;
class FJsonObject;
class UAnimSequence;
class UMantleVehicleSource;
class UMaterialInterface;
class USkeletalMesh;
class USoundWave;
class UStaticMesh;
class UWorld;
struct FMantleTerrainSource;

// EMantlePlaceTextureSlot (schema/material.schema.json's four texture slots, then a layer blend's three, #533) is
// MantleRuntime's, from MantlePlaceMaterial.h: one definition, so a unity blob that sees both headers still compiles.

/**
 * One upload's place in the order the upload queue sends them (F4 #338): textures, then materials
 * (which name texture ids), then meshes, then skeletal meshes and their clips (#469), then sounds (#324)
 * and terrain heightfields (#466), which nothing depends on, then creator vehicles (#435), last, whose
 * documents name their materials' ids.
 */
enum class EMantlePlaceUploadStage : uint8
{
	Texture,
	Material,
	Mesh,
	SkeletalMesh,
	AnimClip,
	Sound,
	/** A landscape's heights; its weightmap is a texture and goes with the textures. */
	Heightfield,
	/** A creator vehicle (#435): its document names its materials' ids, so it goes after everything else. */
	Vehicle,
	/** A creator weapon (#789): its document names its skeletal mesh's and materials' ids, so it goes after them. */
	Weapon
};

/** A uploaded asset as the upload queue recorded it: what the exporter writes for its local key. */
struct MANTLEEDITOR_API FMantlePlaceUploadedRef
{
	/** ast_u + 32 hex digits. */
	FString Id;

	/** "sha256:" + hex of the uploaded bytes (the API's content address, and the manifest's hash). */
	FString Hash;

	int64 Bytes = 0;
};

/** One texture slot's own UV transform in a material document (uvTransforms, #471; glTF KHR_texture_transform). */
struct MANTLEEDITOR_API FMantlePlaceDocumentUvTransform
{
	double Offset[2] = { 0.0, 0.0 };
	double Scale[2] = { 1.0, 1.0 };

	/** Radians; written wrapped into a half turn either way. */
	double Rotation = 0.0;
};

/** One water normal layer in a material document (water.normal, water.normal2, #628): space, tiling or world size, and panning. */
struct MANTLEEDITOR_API FMantlePlaceDocumentWaterNormal
{
	/** Space world (worldSizeCm a repeat) rather than uv (the mesh's first UV set, Tiling repeats across it). */
	bool bWorld = false;
	double Tiling[2] = { 1.0, 1.0 };
	double WorldSizeCm = 100.0;

	/** Repeats a second. */
	double Panning[2] = { 0.0, 0.0 };

	/** The schema's defaults: uv, tiling 1 by 1, still. */
	bool IsDefault() const
	{
		return !bWorld && Tiling[0] == 1.0 && Tiling[1] == 1.0 && Panning[0] == 0.0 && Panning[1] == 0.0;
	}
};

/**
 * A material flattened to schema/material.schema.json (v1), with textures named by the local key
 * of a converted texture until they have ids. ToJson writes the uploaded document once every
 * texture it names has an id.
 */
struct MANTLEEDITOR_API FMantlePlaceMaterialDocument
{
	/**
	 * opaque | mask | blend | additive (#628). Additive writes only what it reads: emissiveFactor, emissiveStrength, the
	 * emissive slot's texture, transform and pan, doubleSided and the shared uvScale and uvOffset (the schema: baseColor,
	 * metallic, roughness, orm and normal do nothing on it).
	 */
	FString AlphaMode = TEXT("opaque");
	double AlphaCutoff = 0.5;
	bool bDoubleSided = false;
	double BaseColorFactor[4] = { 1.0, 1.0, 1.0, 1.0 };
	double MetallicFactor = 1.0;
	double RoughnessFactor = 1.0;
	double EmissiveFactor[3] = { 0.0, 0.0, 0.0 };
	double EmissiveStrength = 1.0;
	double UvScale[2] = { 1.0, 1.0 };
	double UvOffset[2] = { 0.0, 0.0 };

	/**
	 * #471's keys, each written only away from its default, so a material without them keeps its bytes
	 * (and stays on M_MantlePlace). Slot (baseColor, normal, orm, emissive) to its own UV transform, for
	 * a slot whose transform differs from UvScale/UvOffset.
	 */
	TMap<FString, FMantlePlaceDocumentUvTransform> UvTransforms;
	double NormalScale = 1.0;
	double OcclusionStrength = 1.0;
	/** 0 or 1. */
	int32 OcclusionUV = 0;
	/** none | multiplyBaseColor. */
	FString VertexColor = TEXT("none");

	/**
	 * #533's keys, each written only when set, so a material without them keeps its bytes. Slot (baseColor,
	 * normal, orm, emissive) to its pan in UV units a second (X u, Y v); a zero pan is not written.
	 */
	TMap<FString, FVector2D> UvPanning;

	/** wind: intensity and speed 0..4, weight vertexColorR | uvV | uniform. */
	bool bWind = false;
	double WindIntensity = 0.0;
	double WindSpeed = 0.0;
	FString WindWeight = TEXT("uniform");

	/** worldAligned: tile size 1..100000 cm, sharpness 1..16. */
	bool bWorldAligned = false;
	double TileSizeCm = 100.0;
	double Sharpness = 1.0;

	/** layerBlend: channel r | g | b | a and the layer's base colour factor; its textures are Textures' layer keys. */
	bool bLayerBlend = false;
	FString LayerChannel = TEXT("r");
	double LayerBaseColorFactor[4] = { 1.0, 1.0, 1.0, 1.0 };

	/**
	 * foliage (#628): Two Sided Foliage shading on a masked, two-sided document; goes with wind. Tint 0..1 each,
	 * strength 0..4, vertex-colour weight 0..1; written only when the document is masked and two-sided.
	 */
	bool bFoliage = false;
	double FoliageTint[3] = { 1.0, 1.0, 1.0 };
	double FoliageStrength = 1.0;
	double FoliageVertexColorWeight = 0.0;

	/**
	 * water (#628): Single Layer Water on its own master, an opaque document. It writes only baseColorFactor (the surface
	 * colour), roughnessFactor, normalScale, textures.normal and the water object (the schema: no baseColor, orm or
	 * emissive texture, vertex colour, occlusion, metallic, emissive, uvScale, uvOffset, uvTransforms or uvPanning on
	 * it), so a document that is not written as water keeps all of those for the client that cannot read the key
	 * (FMantleTargetFeatures::DowngradeMaterial). Coefficients are per centimetre, 0..1; phase g -1..1. The first
	 * normal map is Textures["normal"] with WaterNormal's layout; the second (when there is one) is
	 * Textures["waterNormal2"] with WaterNormal2's.
	 */
	bool bWater = false;
	double WaterScattering[3] = { 0.002, 0.002, 0.002 };
	double WaterAbsorption[3] = { 0.02, 0.007, 0.005 };
	double WaterPhaseG = 0.0;
	double WaterColorScaleBehindWater = 1.0;
	FMantlePlaceDocumentWaterNormal WaterNormal;
	FMantlePlaceDocumentWaterNormal WaterNormal2;

	/**
	 * Slot (baseColor, normal, orm, emissive) to a converted texture's key; a layer blend's slots under
	 * SlotName's layerBaseColor, layerNormal and layerOrm (written inside layerBlend, #533), and water's second normal
	 * under waterNormal2 (written as water.normal2.texture, #628).
	 */
	TMap<FString, FString> Textures;

	/**
	 * Shading carPaint and its finish (#487): the source material is Clear Coat, opaque and
	 * one-sided. Written only when true (and the document is opaque and one-sided), so every
	 * other document is byte-identical to one written before car paint existed.
	 */
	bool bCarPaint = false;
	FMantleCarPaintFinish CarPaint;

	/**
	 * The material asset's bytes: a closed JSON object, keys in a fixed order, numbers rounded, so
	 * the same material always hashes the same. False (and nothing written) when a texture has no
	 * id in TextureIds or an id is not ast_u + 32 hex.
	 */
	bool ToJson(const TMap<FString, FString>& TextureIds, FString& OutJson, FString& OutError) const;

	/** The same with texture keys in place of ids: what the conversion cache stores. */
	FString ToTemplateJson() const;
	static bool FromTemplateJson(const FString& Json, FMantlePlaceMaterialDocument& Out);

	/** The schema's slot name for Slot; layerBaseColor, layerNormal or layerOrm for a layer blend's (the keys Textures holds them under). */
	static const TCHAR* SlotName(EMantlePlaceTextureSlot Slot);

	/** #533: whether the document sets any of uvPanning, wind, worldAligned, layerBlend and (#628) foliage and water. */
	bool HasFidelity2() const { return UvPanning.Num() > 0 || bWind || bWorldAligned || bLayerBlend || bFoliage || bWater; }

	/** The document is written with the water key (#628): water, opaque. */
	bool WritesWater() const { return bWater && AlphaMode == TEXT("opaque") && !bCarPaint; }

	/** The document is written with the foliage key: wind-driven foliage, masked and two-sided only. */
	bool WritesFoliage() const { return bFoliage && AlphaMode == TEXT("mask") && bDoubleSided && !bWorldAligned && !bLayerBlend; }

	/** The document is written as alphaMode additive (#628): emissive-driven, nothing of its base colour. */
	bool WritesAdditive() const { return AlphaMode == TEXT("additive") && !bCarPaint; }
};

/**
 * #533: what ConvertMaterial hands MaterialFromGltf beyond GLTFExporter's glTF: the graph's vertex-colour tint
 * (#471) and FMantleMaterialFidelity2 with its source textures already read.
 */
struct MANTLEEDITOR_API FMantleMaterialExtras
{
	/** FMantleMaterialAnalysis::bVertexColorTintsBaseColor. */
	bool bVertexColorTintsBaseColor = false;

	/** The recognised patterns (FMantlePlaceAssets::AnalyzeMaterial); its texture pointers are not read here. */
	FMantleMaterialFidelity2 Fidelity;

	/**
	 * Per slot (base colour, normal, ORM, emissive, then the layer's base colour, normal, ORM): the image a
	 * source slot publishes instead of GLTFExporter's bake (PNG; a normal map in glTF's green-up convention),
	 * and a name for messages, then water's second normal map (#628, EMantlePlaceTextureSlot::WaterNormal2, slot 7).
	 * Empty where the slot keeps the bake.
	 */
	TArray<uint8> SourceImages[8];
	FString SourceNames[8];
};

/** One texture file ready to upload: PNG or JPEG, at most the texture size and byte caps. */
struct MANTLEEDITOR_API FMantleConvertedTexture
{
	/** "texture:" + role + ":" + the bytes' SHA-256: equal pixels in the same role are one upload. */
	FString Key;

	/** For messages: the Unreal texture (or the bake) and where it is used, e.g. "T_Crate_D (base colour of MI_Crate)". */
	FString Name;

	/** color | normal | data: the upload's role option (F9 #343), which picks the GPU format. */
	FString Role;

	/** png | jpeg. */
	FString Format;

	int32 Width = 0;
	int32 Height = 0;
	TArray<uint8> Bytes;

	/** "sha256:" + hex of Bytes. */
	FString Hash;
};

/** One material, flattened. */
struct MANTLEEDITOR_API FMantleConvertedMaterial
{
	/** FMantlePlaceAssets::MaterialKey. */
	FString Key;
	FString Name;
	FString Path;
	FMantlePlaceMaterialDocument Document;
	bool bFromCache = false;
};

/** One static mesh, as its upload GLB. */
struct MANTLEEDITOR_API FMantleConvertedMesh
{
	/** FMantlePlaceAssets::MeshKey. */
	FString Key;
	FString Name;
	FString Path;

	/** Epic's glTF convention, render vertices and triangles in their own order, one glTF material per slot, no images. */
	TArray<uint8> Bytes;
	FString Hash;

	/** Per glTF material (= MRM2 slot): the mesh's own material slot, and its section name. */
	TArray<int32> Slots;
	TArray<FString> SectionNames;

	/** LOD0's. */
	int32 Triangles = 0;
	int32 Vertices = 0;

	/** #470: the LODs the GLB carries (LOD0 included), and its simple collision's shapes (0: the client collides with the triangles). */
	int32 LODs = 1;
	int32 CollisionShapes = 0;

	bool bFromCache = false;
};

/**
 * One creator sound (#324), as its upload: a canonical WAV (RIFF/WAVE, format 1 PCM, 16-bit, mono
 * or stereo, a 44-byte header and nothing else), which the cook worker parses into MAU1.
 */
struct MANTLEEDITOR_API FMantleConvertedSound
{
	/** FMantlePlaceAssets::SoundKey. */
	FString Key;
	FString Name;
	FString Path;

	TArray<uint8> Bytes;
	FString Hash;

	int32 SampleRate = 0;
	int32 Channels = 0;
	int32 Frames = 0;
};

/**
 * One skeletal mesh on its own skeleton (#469), as its upload: LOD 0 as a skinned GLB in Epic's glTF convention, every
 * bone of its reference skeleton a joint, at most eight influences a vertex (#484), one glTF material per slot in the order
 * sections first use them. The cook worker writes MSK1 role 3 from it.
 */
struct MANTLEEDITOR_API FMantleConvertedSkeletalMesh
{
	/** FMantlePlaceAssets::SkeletalMeshKey. */
	FString Key;
	FString Name;
	FString Path;

	TArray<uint8> Bytes;
	FString Hash;

	/** Per glTF material (= MSK1 section): the mesh's own material slot. */
	TArray<int32> Slots;

	int32 Bones = 0;
	int32 Triangles = 0;

	/** Its skeleton's hash (FMantleSkeletonHash): what its clips must carry. */
	FString SkeletonHash;
};

/**
 * One animation clip for a place skeletal mesh (#469), as its upload: MAN1 sampled at FMantlePlaceAssetLimits::ClipFrameRate
 * over the bones of the mesh it plays on, carrying that mesh's bone table and its hash. One per (sequence, mesh).
 */
struct MANTLEEDITOR_API FMantleConvertedClip
{
	/** FMantlePlaceAssets::ClipKey. */
	FString Key;
	FString Name;
	FString Path;

	TArray<uint8> Bytes;
	FString Hash;

	int32 Frames = 0;
	int32 Tracks = 0;
	FString SkeletonHash;
};

/** A landscape (#466) as its upload: its heights as MHF1, and the key of its weightmap texture when it has one. */
struct MANTLEEDITOR_API FMantleConvertedHeightfield
{
	/** FMantleTerrainExport::HeightfieldKey. */
	FString Key;
	FString Name;
	FString Path;

	TArray<uint8> Bytes;
	FString Hash;

	int32 Width = 0;
	int32 Height = 0;

	/** FMantleTerrainExport::WeightmapKey, the key of its texture in Textures; empty with one layer. */
	FString WeightmapKey;
};

/**
 * One creator vehicle (#435), ready but for its materials' ids: the body as it uploads (LOD0 with its tires
 * merged) and the vehicle document without materials. FMantlePlaceAssetSet::VehicleBytes writes the upload once
 * every section's material has an id.
 */
struct MANTLEEDITOR_API FMantleConvertedVehicle
{
	/** FMantleVehicleConverter::VehicleKey. */
	FString Key;
	FString Name;
	FString Path;

	/**
	 * The file name (no folders) the body skeletal mesh was imported from, cleaned (FMantleProvenance::ImportFilenameOf); empty
	 * when it has none. A public library publish sends it with Path as the item's provenance (#614 stage 7).
	 */
	FString ImportFilename;

	FMantleGlbMesh Body;
	TSharedPtr<FJsonObject> Document;

	/** A body section (glTF material name) to its material's FMantlePlaceAssets::MaterialKey; library materials are left out. */
	TMap<FString, FString> SectionMaterialKeys;

	/**
	 * Its liveries (#488), in order: each one's gathered key (FMantleVehicleConverter::LiveryTextureKeys), which
	 * FMantlePlaceAssetSet::DecalTextures maps to the converted texture that uploads it. The textures upload first.
	 */
	TArray<FString> LiveryTextureKeys;

	/** One attached part (#435 item 2) as it uploads: its mesh's key and each upload slot's material key. */
	struct FPart
	{
		/** FMantlePlaceAssets::MeshKey: converted and uploaded as a mesh is, before the vehicle. */
		FString MeshKey;
		FName Bone;
		FTransform Transform;
		/** FMantlePlaceAssets::UploadSlots order; empty for a slot drawn with Mantle's default surface (a library material). */
		TArray<FString> MaterialKeys;
		/** The upload slots that wear the vehicle's paint (FMantleVehicleConverter::PartUploadPaintSlots). */
		TArray<int32> PaintSlots;
	};

	/** Its attached parts (#435 item 2), in the source's order. */
	TArray<FPart> Parts;

	/**
	 * Its horn's sound (#546): FMantlePlaceAssets::SoundKey, converted and uploaded as a SoundEmitter's sound is, before the
	 * vehicle; empty for none (a car then plays Mantle's built-in horn).
	 */
	FString HornSoundKey;

	/**
	 * Its library card's picture (#821): the key of the texture upload that carries UMantleVehicleSource::ThumbnailPng (a 512 x 512 PNG, no role, so the backend keeps
	 * it as it is), converted and uploaded with the other textures, before the vehicle. Empty for a vehicle with no picture. The library item names it as its thumbnail.
	 */
	FString ThumbnailKey;

	int32 Triangles = 0;
};

/**
 * One creator weapon (#789), ready but for its ids: the weapon builder's settings file as text, the key of its 3D model and the
 * material of each of the model's upload slots, and (#898) its extra parts' and attack sounds' keys. FMantlePlaceAssetSet::WeaponBytes
 * writes the upload once they have ids.
 */
struct MANTLEEDITOR_API FMantleConvertedWeapon
{
	/** FMantlePlaceAssets::WeaponKey. */
	FString Key;
	FString Name;

	/** /Game/MantleWeapons/<Name>: what its library item is remembered under. */
	FString Path;

	/** The file name the 3D model was imported from, cleaned; empty when it has none (a public library publish sends it as provenance). */
	FString ImportFilename;

	/** #124: the key of the texture upload that carries its library picture (a 512 x 512 PNG, no role); empty for none. The library item names it as its thumbnail. */
	FString ThumbnailKey;

	/** The settings file, as the weapon builder wrote it. */
	FString DocumentText;

	/** FMantlePlaceAssets::SkeletalMeshKey of its 3D model, converted and uploaded as a place skeletal mesh is. */
	FString MeshKey;

	/** One per upload slot of the model, in slot order: the key of its material, always uploaded (#892: the weapon cook takes no library id). */
	struct FSlot
	{
		FString MaterialKey;
	};
	TArray<FSlot> Slots;

	/**
	 * #898: one per attachments[] entry of the settings file, in order: the key of its model (FMantlePlaceAssets::MeshKey or
	 * SkeletalMeshKey), converted and uploaded as a place mesh or skeletal mesh is, and its upload slots' materials as Slots holds the weapon's.
	 */
	struct FAttachment
	{
		FString MeshKey;
		TArray<FSlot> Slots;
	};
	TArray<FAttachment> Attachments;

	/** #905: projectile.mesh's model (a launcher's round, an arrow) and its slots' materials, as an extra part's; MeshKey empty when the
	 *  file names none. WeaponBytes writes projectile.mesh and projectile.materials by uploaded id. */
	FAttachment Projectile;

	/** #898: FMantlePlaceAssets::SoundKey of the attack sounds (sounds.fire, sounds.fireSuppressed), empty for one the file leaves. */
	FString FireSoundKey;
	FString FireSuppressedSoundKey;
};

/** One file for the upload queue (F4 #338): its kind and options, bytes, content address, and the Unreal asset to name in messages. */
struct MANTLEEDITOR_API FMantlePlaceUpload
{
	FString Key;
	EMantlePlaceUploadStage Stage = EMantlePlaceUploadStage::Texture;
	FMantleAssetUploadOptions Options;
	TArray<uint8> Bytes;
	FString Hash;

	/** The Unreal asset, never an id or a hash (F6 #340's rule for messages). */
	FString SourceName;

	/**
	 * #804: where the bytes are when they are not in Bytes (the publish spilled them to disk, FMantlePlacePublishSession::FTiming),
	 * and their size. The upload queue reads the file when it sends. Empty for an upload that holds its bytes.
	 */
	FString File;
	int64 FileSize = 0;
};

/** #804: a converted file written to disk (FMantlePlaceAssetSet::SpillBytes): where, and its size. */
struct MANTLEEDITOR_API FMantleSpilledFile
{
	FString Path;
	int64 Size = 0;
};

/**
 * Everything a level needs uploaded, converted (F5 #339). This is the seam between the three Fab
 * tickets: F5 fills the meshes, materials and textures; the upload queue (F4 #338) takes
 * TakeUploads stage by stage and records each result with RecordUpload; the exporter (through
 * FMantleExportOptions::Assets) writes the recorded ids into the place file and its assets[].
 */
struct MANTLEEDITOR_API FMantlePlaceAssetSet
{
	TArray<FMantleConvertedMesh> Meshes;
	TArray<FMantleConvertedMaterial> Materials;
	TArray<FMantleConvertedTexture> Textures;

	/** AmbientSounds' own sounds (#324), uploaded in their own stage. */
	TArray<FMantleConvertedSound> Sounds;

	/** SkeletalMesh actors' meshes and the clips they play (#469). */
	TArray<FMantleConvertedSkeletalMesh> SkeletalMeshes;
	TArray<FMantleConvertedClip> Clips;

	/** The landscape's heights (#466), uploaded in their own stage, last; its weightmap is in Textures. */
	TArray<FMantleConvertedHeightfield> Heightfields;

	/** Creator vehicles (#435), uploaded last: their documents name their materials' ids. */
	TArray<FMantleConvertedVehicle> Vehicles;

	/** Creator weapons (#789), uploaded after the vehicles: their documents name their model's and materials' ids. */
	TArray<FMantleConvertedWeapon> Weapons;

	/** What conversion changed or could not do, in plain words, naming the Unreal asset. Errors stop the publish. */
	TArray<FMantleValidationIssue> Issues;

	/** Local key to the uploaded asset, filled by the upload queue. */
	TMap<FString, FMantlePlaceUploadedRef> Uploaded;

	/**
	 * A decal's gathered texture (FMantlePlaceAssets::DecalTextureKey) to the key of the converted
	 * texture in Textures that uploads it (#472), which is what Uploaded records.
	 */
	TMap<FString, FString> DecalTextures;

	/**
	 * #556 item 5: an image in the level's UI/ folder, by place path ("UI/icons/sword.png"), to the
	 * key of the converted texture in Textures that uploads it, which is what Uploaded records.
	 */
	TMap<FString, FString> UIImages;

	/** #786: the key of the converted texture in Textures that is the place's catalogue picture; empty when there is none. */
	FString Thumbnail;

	bool HasErrors() const;

	const FMantleConvertedMesh* FindMesh(const FString& Key) const;
	const FMantleConvertedMaterial* FindMaterial(const FString& Key) const;
	const FMantleConvertedTexture* FindTexture(const FString& Key) const;
	const FMantleConvertedSound* FindSound(const FString& Key) const;
	const FMantleConvertedSkeletalMesh* FindSkeletalMesh(const FString& Key) const;
	const FMantleConvertedClip* FindClip(const FString& Key) const;
	const FMantleConvertedHeightfield* FindHeightfield(const FString& Key) const;
	const FMantleConvertedVehicle* FindVehicle(const FString& Key) const;
	const FMantleConvertedWeapon* FindWeapon(const FString& Key) const;

	/** Records an uploaded (or already held) asset under its local key. */
	void RecordUpload(const FString& Key, const FString& Id, const FString& Hash, int64 Bytes);

	/**
	 * Local key to the size of a file whose bytes TakeUploads has handed on (#804). The set's own byte arrays are empty
	 * after that, so a size is read through UploadSize, never from them.
	 */
	TMap<FString, int64> TakenSizes;

	/** #804: converted files SpillBytes wrote to disk, by local key. Their byte arrays here are empty. */
	TMap<FString, FMantleSpilledFile> Spilled;

	/**
	 * The uploads of one stage, in a stable order. Materials need their textures recorded first
	 * (their bytes name the texture ids): false, naming the first texture without an id, when
	 * one is not.
	 *
	 * #804: the bytes of a texture, mesh, sound, skeletal mesh, clip or heightfield are MOVED into the uploads (the set holds
	 * them once, not twice), or, for a file SpillBytes wrote, the upload carries its path instead. A file is taken once: a
	 * second call, such as the one after the level changed and was gathered again, leaves out every key an earlier call took
	 * (its upload was recorded, or refused, by then). Materials, vehicles and weapons are written from their documents each
	 * time.
	 */
	bool TakeUploads(EMantlePlaceUploadStage Stage, TArray<FMantlePlaceUpload>& OutUploads, FString& OutError);

	/**
	 * The size of the file under Key: what TakeUploads or SpillBytes recorded, else InMemory (the byte count the caller
	 * has for it while the bytes are still here).
	 */
	int64 UploadSize(const FString& Key, int64 InMemory) const;

	/**
	 * #804: writes each converted file of at least MinBytes under Directory (named by its hash, so equal files are one file)
	 * and empties its byte array; the set keeps the path and size in Spilled and the hash it always had. The upload that
	 * takes it reads the file when it sends. Files not yet taken only. Returns how many were written; OutError names the
	 * first file that could not be (its bytes stay in memory, and the rest are still written).
	 */
	int32 SpillBytes(const FString& Directory, int64 MinBytes, FString& OutError);

	/** #804: the bytes the converted files hold in memory right now (not the taken or spilled ones). */
	int64 HeldBytes() const;

	/**
	 * #804: frees every byte array (converted files, a vehicle's body) once the publish is over. Hashes, sizes, documents
	 * and recorded ids stay, so a finished session can still be read.
	 */
	void ReleaseBytes();

	/** A material's upload bytes (FMantlePlaceMaterialDocument::ToJson with the recorded texture ids). */
	bool MaterialBytes(const FMantleConvertedMaterial& Material, TArray<uint8>& OutBytes, FString& OutError) const;

	/**
	 * A vehicle's upload bytes: its body with its document, the materials, liveries (#488) and attached parts' meshes and
	 * materials (#435 item 2) the recorded ids. False naming the first material, livery or part without one.
	 */
	bool VehicleBytes(const FMantleConvertedVehicle& Vehicle, TArray<uint8>& OutBytes, FString& OutError) const;

	/**
	 * #789: a weapon's upload bytes: its settings file with "mesh" the uploaded id of its 3D model and "materials" the uploaded id
	 * of each slot's material (#898: each extra part's too; #905: and the round's, projectile.mesh and projectile.materials), as compact
	 * JSON. False naming the first model or material without an id.
	 */
	bool WeaponBytes(const FMantleConvertedWeapon& Weapon, TArray<uint8>& OutBytes, FString& OutError) const;

	/** #488: the uploaded ids of a vehicle's liveries, in order; false naming the first without one. */
	bool VehicleLiveryIds(const FMantleConvertedVehicle& Vehicle, TArray<FString>& OutIds, FString& OutError) const;

	/** Adds a texture unless one with its key is there already; returns the key. */
	FString AddTexture(FMantleConvertedTexture&& Texture);
};

/** How GLTFExporter bakes (and so part of every cached conversion's key). */
struct MANTLEEDITOR_API FMantleFlattenSettings
{
	/** Bake size for inputs with no texture to size from; inputs with textures bake at their largest texture's size (then capped). */
	int32 DefaultBakeSize = 1024;
};

/**
 * Flattens one material to glTF metallic-roughness. The production one is GLTFExporter
 * (FMantleGltfMaterialFlattener); tests substitute a scripted one, so what the converter does with
 * a flattened material, and whether it calls the flattener at all, is testable without a renderer.
 */
class MANTLEEDITOR_API IMantleMaterialFlattener
{
public:
	virtual ~IMantleMaterialFlattener() = default;

	/** A GLB (JSON and binary chunk) whose materials[0] is Material, its images embedded. OutMessages are the exporter's warnings. */
	virtual bool Flatten(UMaterialInterface* Material, const FMantleFlattenSettings& Settings, FMantleGlbDocument& OutDocument,
		TArray<FString>& OutMessages, FString& OutError) = 0;

	/** Identifies the flattener and anything else that changes its output (whether a renderer was available): part of the cache key. */
	virtual FString Describe() const = 0;

	/** False when it can only write factors (no renderer to bake or read textures): the converter says so once per material. */
	virtual bool CanBakeTextures() const { return true; }
};

/**
 * GLTFExporter's material bake (decision 11, shared with A2.4): UGLTFExporter::ExportToGLTF of the
 * material alone, BakeMaterialInputs Simple, DefaultMaterialBakeSize with auto-detect (inputs that
 * are a plain texture keep it, with KHR_texture_transform), PNG images, normal maps adjusted to
 * glTF's convention, emissive strength and unlit kept. Without a renderer (-NullRHI) GLTFExporter
 * writes factors only; Describe says so, so such a result is never reused by a session that can
 * render.
 */
class MANTLEEDITOR_API FMantleGltfMaterialFlattener : public IMantleMaterialFlattener
{
public:
	virtual bool Flatten(UMaterialInterface* Material, const FMantleFlattenSettings& Settings, FMantleGlbDocument& OutDocument,
		TArray<FString>& OutMessages, FString& OutError) override;
	virtual FString Describe() const override;
	virtual bool CanBakeTextures() const override;
};

/** What a Convert did: for the progress panel, the log and the cache test. */
struct MANTLEEDITOR_API FMantlePlaceConversionStats
{
	int32 MeshesConverted = 0;
	int32 MeshesFromCache = 0;
	int32 MaterialsConverted = 0;
	int32 MaterialsFromCache = 0;

	/** Sounds written as WAV (#324): never cached, it is a copy of the imported samples. */
	int32 SoundsConverted = 0;

	/** Skeletal meshes written as GLB and clips as MAN1 (#469): never cached, both are read straight from the assets. */
	int32 SkeletalMeshesConverted = 0;
	int32 ClipsConverted = 0;

	/** A decal's textures (#472), read from their source pixels every time: nothing to bake, so nothing cached. */
	int32 DecalTexturesConverted = 0;

	/** Calls into the flattener (GLTFExporter). A republish of unchanged content makes none. */
	int32 FlattenerCalls = 0;
};

/**
 * Converts a level's gathered dependencies (F5 #339, FAB_ASSETS_252.md section 6 steps 3 and 4).
 *
 * - Meshes: LOD 0's render data (a Nanite mesh's fallback) through A2.4's reader and GLB writer:
 *   Epic's axes, glTF triangle order, one primitive per section, one named glTF material per
 *   slot, no images (FMantleCosmeticExport::ReadStaticMesh, FMantleGlbWriter), with UV channel 1
 *   and vertex colours when the mesh has them (#470). Also #470: the mesh's own lower LODs, up to
 *   limits.json placeAssets.staticMesh.lods (each within its triangle cap, no larger than the LOD
 *   before, drawing only LOD0's slots; the first that is not, and every LOD after it, is left off
 *   with a warning) at the mesh's screen sizes; and its simple collision (boxes, spheres, capsules,
 *   convex hulls, unless its collision complexity is Use Complex As Simple). A shape the cook would
 *   refuse (degenerate, or a kind Mantle does not carry) is left out with a warning naming it; past
 *   placeAssets.staticMesh.collision's caps the whole simple collision is left off with a warning,
 *   and the mesh collides with its triangles as before. #804: a mesh over the triangle or vertex limit is converted from
 *   its first lower LOD that fits (that LOD is then its LOD 0), else from a transient copy reduced to fit
 *   (FMantleMeshReduce); the creator's mesh is only read, and Check this place gives the notice.
 * - Materials: flattened through the flattener (GLTFExporter), then mapped to
 *   material.schema.json: alpha mode, two-sided, factors, emissive strength, one UV transform (and
 *   per-texture ones where textures differ), normal and occlusion strength, occlusion's UV set, and
 *   vertex colour tinting the base colour (#471); UV panning, foliage wind, world-aligned mapping and
 *   vertex-colour layer blends the graph shows in a recognised shape (#533), a panned, world-aligned or
 *   layered slot publishing its source texture instead of the bake;
 *   base colour and emissive as JPEG (PNG when the alpha matters), normal and ORM as PNG
 *   (occlusion merged into ORM's red), each at most MaxTextureSize and MaxTextureBytes,
 *   downscaled or re-encoded with a warning otherwise.
 * - Sounds (#324): an AmbientSound's own sound wave, its imported PCM16 samples written as a
 *   canonical WAV (USoundWave::GetImportedSoundWaveData), held to FMantlePlaceAssets::CheckSoundRule
 *   and to what the samples themselves say. Not cached: it is a copy, not a bake.
 * - Cache: under Saved/Mantle/AssetCache, keyed by the saved hash of every package the result
 *   depends on (the mesh's; the material's, its parents', functions' and textures') and the
 *   settings, so a republish converts only what changed. A package with unsaved changes, or one
 *   with no saved hash and no file, is converted every time and never stored.
 */
class MANTLEEDITOR_API FMantlePlaceAssetConverter
{
public:
	/** The GLTFExporter flattener and the project's cache directory. */
	FMantlePlaceAssetConverter();
	FMantlePlaceAssetConverter(TSharedRef<IMantleMaterialFlattener> InFlattener, const FString& InCacheDirectory);

	/** Converts every gathered asset into Out (appended, deduplicated). False when a conversion error stops the publish (Out.Issues says which). */
	bool Convert(const FMantlePlaceDependencies& Dependencies, FMantlePlaceAssetSet& Out);

	bool ConvertMesh(UStaticMesh* Mesh, FMantlePlaceAssetSet& Out);
	bool ConvertMaterial(UMaterialInterface* Material, FMantlePlaceAssetSet& Out);
	bool ConvertSound(USoundWave* Sound, FMantlePlaceAssetSet& Out);

	/**
	 * #469: a skeletal mesh on its own skeleton, LOD 0 through A2.4's skeletal reader (the eight heaviest influences of a
	 * vertex kept and renormalised, #484, with a note naming how many were capped) and GLB writer. Held to
	 * FMantlePlaceAssets::CheckSkeletalMesh first.
	 */
	bool ConvertSkeletalMesh(USkeletalMesh* Mesh, FMantlePlaceAssetSet& Out);

	/**
	 * #469: Sequence sampled over Mesh's bones (FMantleAnimClipWriter::SampleOnMesh) and encoded as a place clip, carrying
	 * Mesh's bone table and its hash, held to limits.json placeAssets.animClip.
	 */
	bool ConvertClip(UAnimSequence* Sequence, USkeletalMesh* Mesh, FMantlePlaceAssetSet& Out);

	/**
	 * A creator vehicle (#435): its body read as it uploads and its document, checked with Check this place's rules
	 * (FMantleVehicleConverter::CheckSource), whose errors stop the publish. Not cached: reading a body is not a bake.
	 */
	bool ConvertVehicle(UMantleVehicleSource* Vehicle, FMantlePlaceAssetSet& Out);

	/**
	 * A creator weapon (#789): the weapon builder's settings file read and held to what can be published
	 * (FMantlePlaceAssets::WeaponDocumentProblem), with the key of its 3D model and its slots' materials. The model and the
	 * materials convert on their own, as gathered. Not cached: it is a small text file.
	 */
	bool ConvertWeapon(const FMantleGatheredWeapon& Weapon, FMantlePlaceAssetSet& Out);

	/**
	 * A publish converts what it gathered one step at a time, so the editor keeps running between steps: meshes, materials, decal
	 * textures, sounds, skeletal meshes, clips, landscapes, vehicles, then weapons, each one step. A place's own steps (its UI images,
	 * its thumbnail) follow these. #840: shared by the level and the weapon builder's publish.
	 */
	static int32 NumDependencySteps(const FMantlePlaceDependencies& Dependencies);
	static FString DependencyStepName(const FMantlePlaceDependencies& Dependencies, int32 Step);
	void ConvertDependencyStep(const FMantlePlaceDependencies& Dependencies, int32 Step, FMantlePlaceAssetSet& Out);

	/**
	 * One of a decal's textures (#472), from its source pixels: a base colour as PNG with its alpha
	 * (the decal's coverage), a normal map as PNG in glTF's green-up convention (the cook flips it
	 * back to Unreal's), each held to MaxTextureSize and MaxTextureBytes like a material's. Records
	 * Texture.Key in Out.DecalTextures. False, with an error issue naming the decal, when the
	 * texture's pixels cannot be read.
	 */
	bool ConvertDecalTexture(const FMantleGatheredTexture& Texture, FMantlePlaceAssetSet& Out);

	/**
	 * #556 item 5: one PNG or JPEG file from the level's UI/ folder (PlacePath "UI/...", read from
	 * DiskPath), prepared exactly as a decal's base colour is (straight alpha kept, held to
	 * MaxTextureSize and MaxTextureBytes), so it takes the same texture upload, cook and screening.
	 * Records the texture's key in Out.UIImages. False, with an error issue naming the file, when it
	 * is missing, over FMantlePlaceLimits::MaxUIImageBytes or not a readable PNG or JPEG.
	 */
	bool ConvertUIImage(const FString& PlacePath, const FString& DiskPath, FMantlePlaceAssetSet& Out);

	/**
	 * #786: the place's captured catalogue picture (Width x Height opaque pixels, top row first), encoded as a base
	 * colour would be (JPEG, held to MaxTextureSize and MaxTextureBytes) but uploaded with no role (#796), so the backend
	 * keeps the JPEG file the catalogue serves; a comment segment keeps its bytes apart from any colour texture's.
	 * Records the texture's key in Out.Thumbnail. False, with a warning, when it cannot be encoded: the place publishes
	 * without a picture rather than not at all.
	 */
	bool ConvertThumbnail(TConstArrayView<FColor> Pixels, int32 Width, int32 Height, FMantlePlaceAssetSet& Out);

	/**
	 * A landscape (#466, FMantleTerrainExport): its heights as MHF1, resampled to the heightfield cap
	 * when larger, and with more than one painted layer its weightmap as an RGBA PNG uploaded without
	 * a role (kept lossless), halved until it fits MaxTextureBytes. Its layer materials convert as
	 * materials (FMantlePlaceDependencies::AddTerrain). Not cached: it is read, not baked.
	 */
	bool ConvertTerrain(AActor* Landscape, FMantlePlaceAssetSet& Out);

	/**
	 * Converts an already-read landscape: what ConvertTerrain does after reading it, split out so tests
	 * need no landscape. Label names it in messages; World finds its Mantle Place Settings.
	 */
	bool ConvertTerrainSource(UWorld* World, const FMantleTerrainSource& Source, const FString& Key, const FString& WeightmapKey,
		const FString& Label, const FString& Path, FMantlePlaceAssetSet& Out);

	/** Called before each asset (done, total, the Unreal asset's name). Return false to cancel. For F6's progress panel. */
	TFunction<bool(int32, int32, const FString&)> OnProgress;

	/**
	 * The canonical WAV the cook worker accepts (#324): "RIFF", size, "WAVE", a 16-byte fmt chunk
	 * (format 1, 16-bit, the byte rate and block align that agree with them), then one data chunk
	 * holding Pcm16 as it is. False when the samples are not whole frames or too large for a RIFF.
	 */
	static bool WriteSoundWav(TConstArrayView<uint8> Pcm16, int32 SampleRate, int32 Channels, TArray<uint8>& OutWav);

	FMantleFlattenSettings Settings;
	FMantlePlaceAssetLimits Limits;

	const FMantlePlaceConversionStats& GetStats() const { return Stats; }
	const FString& GetCacheDirectory() const { return CacheDirectory; }

	/** <Project>/Saved/Mantle/AssetCache. */
	static FString DefaultCacheDirectory();

	/**
	 * The cache key for a result that depends on Objects' packages: SHA-256 over Salt and each
	 * package's name and saved hash (its file's MD5 when it has none), sorted, with the saved
	 * packages each depends on (asset registry, hard) followed. False with the reason when a
	 * package has unsaved changes or no saved state: the result is then not cached.
	 */
	static bool ComputeCacheKey(const FString& Salt, const TArray<const UObject*>& Objects, FString& OutKey, FString& OutWhyNot);

	/**
	 * The salt a mesh's cache key is made with: the LOD and collision caps, the upload limit and the mesh's path. #804: a
	 * mesh published from a lower LOD (FromLOD above 0) adds " fromlod=N", and one published from a reduced copy
	 * (ReducedTag not empty, FMantleMeshReduce::CacheTag) adds " reduced=<ReducedTag>". A mesh that fits adds neither, so
	 * its key, and the hash of what it converts to, are as they were.
	 */
	static FString MeshCacheSalt(const FMantlePlaceAssetLimits& Limits, const UStaticMesh& Mesh, int32 FromLOD, const FString& ReducedTag);

	/**
	 * #628: makes a mesh's LOD screen sizes ones the cook accepts, on the export's copy only (never the asset). Values that
	 * are already valid are kept: LOD0 is limited to 1 (a size above 1 means nothing more than "always"), and each later
	 * LOD must be finite, above 0 and strictly below the one before. One that is not (a repeat, a rise, 0, NaN) takes half
	 * of the one before it, and the LODs after it are checked against that. Returns true when anything changed.
	 */
	static bool RepairLODScreenSizes(TArray<float>& InOutSizes);

	/**
	 * #628: a translucent material's refraction (its glTF KHR_materials_ior), drawn as the closest look a place material has:
	 * Mantle's translucent surface has no refraction and no specular key, so the glass is as reflective as the default (an
	 * index of 1.5, F0 0.04). The reflectance the source's index implies, F0 = ((n - 1) / (n + 1))^2, is made up for in the
	 * opacity: the part that reflects instead of passing the scene through, F0 minus 0.04, is added to it. A glass thinner than
	 * MinGlassOpacity is raised to it so thick glass still reads as glass. Tint (baseColorFactor) and roughness are kept.
	 * Returns the opacity to publish.
	 */
	static double GlassOpacityFromIor(double Ior, double Opacity);

	/** The least opacity a refractive glass publishes with. */
	static constexpr double MinGlassOpacity = 0.1;

	/**
	 * Maps materials[Index] of a flattened GLB to the material document, preparing its textures
	 * (added to OutSet) and reporting what did not carry over. MaterialName names the Unreal
	 * material in messages. bSourceTexturesReported: the material's oversized source textures
	 * were already named by Check this place, so their downscale is not repeated.
	 * bVertexColorTintsBaseColor: the Unreal graph multiplies its base colour by Vertex Color
	 * (FMantleMaterialAnalysis, #471), which the bake cannot carry; the document says vertexColor
	 * multiplyBaseColor instead (a lit material only).
	 */
	static FMantlePlaceMaterialDocument MaterialFromGltf(const FMantleGlbDocument& Document, int32 Index, const FString& MaterialName,
		const FMantlePlaceAssetLimits& Limits, bool bSourceTexturesReported, FMantlePlaceAssetSet& OutSet, bool bVertexColorTintsBaseColor = false);

	/**
	 * As above with #533's extras: a slot with a source image publishes that image instead of the bake's (with its
	 * own tiling as its uvTransforms entry, and the ORM factors the graph gives), a panned slot its uvPanning, and
	 * wind, worldAligned and layerBlend (with the layer's images) as recognised. World-aligned, layer-blended and
	 * unlit documents keep their rules: an unlit material carries panning on its emissive (glow) only.
	 */
	static FMantlePlaceMaterialDocument MaterialFromGltf(const FMantleGlbDocument& Document, int32 Index, const FString& MaterialName,
		const FMantlePlaceAssetLimits& Limits, bool bSourceTexturesReported, FMantlePlaceAssetSet& OutSet, const FMantleMaterialExtras& Extras);

	/**
	 * One image as an upload for Slot: decoded, downscaled to Limits.MaxTextureSize, encoded (JPEG
	 * for colour whose alpha does not matter, PNG otherwise), then made to fit MaxTextureBytes
	 * (lower JPEG quality, or halved). What names it in messages. False when the image cannot be read.
	 */
	static bool PrepareTexture(TConstArrayView<uint8> Image, EMantlePlaceTextureSlot Slot, bool bKeepAlpha, const FMantlePlaceAssetLimits& Limits,
		const FString& What, bool bWarnDownscale, FMantleConvertedTexture& Out, TArray<FMantleValidationIssue>& OutIssues);

private:
	TSharedRef<IMantleMaterialFlattener> Flattener;
	FString CacheDirectory;
	FMantlePlaceConversionStats Stats;

	bool LoadCachedMesh(const FString& CacheKey, FMantleConvertedMesh& Out, TArray<FMantleValidationIssue>& OutIssues) const;
	void StoreCachedMesh(const FString& CacheKey, const FMantleConvertedMesh& Mesh, const TArray<FMantleValidationIssue>& Issues) const;
	bool LoadCachedMaterial(const FString& CacheKey, FMantleConvertedMaterial& Out, TArray<FMantleConvertedTexture>& OutTextures,
		TArray<FMantleValidationIssue>& OutIssues) const;
	void StoreCachedMaterial(const FString& CacheKey, const FMantleConvertedMaterial& Material, const FMantlePlaceAssetSet& Set,
		const TArray<FMantleValidationIssue>& Issues) const;
};
