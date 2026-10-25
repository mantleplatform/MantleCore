// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleEffectTypes.h"
#include "MantlePlaceLimits.h"
#include "MantleScatter.h"
#include "MantleSurface.h"
#include "MantleWeather.h"
#include "Misc/Optional.h"
#include "Misc/TVariant.h"

/**
 * In-memory representation of a Mantle place file.
 *
 * schema/place.schema.json is the source of truth. These types mirror it; the parser's
 * test suite runs the same fixtures as the Python validator so the two stay in step.
 */

/** Highest schema version this client understands. A place declaring higher is refused. */
#define MANTLE_PLACE_SCHEMA_VERSION 1

// The texture dimension cap is limits.json's any_upload maxTextureSize, read by
// FMantlePlaceAssetLimits and applied by FMantlePlaceAssets::PublishedTextureSize (#452).

enum class EMantleEntityType : uint8
{
	StaticMesh,
	Group,
	PlayerStart,
	PointLight,
	SpotLight,
	DirectionalLight,
	TriggerVolume,
	SoundEmitter,
	/**
	 * Many instances of one mesh (#467): painted foliage, PCG output generated in the creator's
	 * editor, any instanced static mesh component. Added without a schema version bump: older
	 * clients refuse the unknown type, and publish refuses it until a released build reads it.
	 */
	InstancedMesh,
	/** Ambient pedestrians (#477): data on every machine, NPCs only on clients (MantleNPC). */
	CrowdSpawner,
	/** An area light (#472). Added without a schema version bump, like InstancedMesh. */
	RectLight,
	/** An uploaded texture projected onto the world (#472). Client-only content; a server keeps the entity. */
	Decal,
	/**
	 * An uploaded skeletal mesh on its own skeleton, optionally playing one clip (#469). Added
	 * without a schema version bump, gated at publish until a released build reads it.
	 */
	SkeletalMesh,
	/**
	 * Landscape terrain (#466): an uploaded heightfield rebuilt as chunked runtime meshes with
	 * collision, painted with up to four layer materials. One per place.
	 */
	Terrain,
	/**
	 * A creator-published vehicle parked in the place (#435): the server keeps one alive here (MantleGameplay's
	 * bridge). Added without a schema version bump, like InstancedMesh.
	 */
	VehicleSpawn,
	/**
	 * A particle effect (#460): one of Mantle's built-in archetypes (MantleEffectTypes.h) with its look.
	 * Client-only content; a server keeps the entity. Added without a schema version bump, like InstancedMesh.
	 */
	Effect,
	/**
	 * A joint between two physics entities, or one and the world (#528): weld, hinge or rope. The
	 * server builds it (AMantleConstraint) and simulates it; nothing of it replicates. Added without a
	 * schema version bump, like InstancedMesh.
	 */
	Constraint,
	/**
	 * A seeded field of instanced meshes (#530): FMantleScatterParams, generated on each client by
	 * FMantleScatter (MantleScatter.h). Visual only; a server keeps the entity and builds nothing.
	 * Added without a schema version bump, like InstancedMesh.
	 */
	Scatter,
	/**
	 * A reflection probe (#628): a box or sphere every client captures at run time, in the place's own
	 * lighting (FMantleReflectionProbes, MantleReflectionProbe.h). Client-only; a server keeps the entity and
	 * captures nothing. Added without a schema version bump, like InstancedMesh.
	 */
	ReflectionProbe,
	/**
	 * A creator-published weapon waiting to be picked up (#789): a hidden data-only spawn point on every
	 * machine; MantleGameplay owns the pickup. Added without a schema version bump, like VehicleSpawn.
	 */
	WeaponSpawn,
	/**
	 * An anchor for a stored script package (#838): a hidden data-only point on every machine, carrying which
	 * package and its parameters. The package's scripts run once per side and read their points through
	 * Script.Points. Added without a schema version bump, like WeaponSpawn.
	 */
	ScriptPoint,
	/**
	 * Text drawn in the world (GhzGarage/mantle#724): a UTextRenderComponent in Mantle's built-in font. Client-only
	 * content; a server keeps the entity. Added without a schema version bump, gated by the worldText client feature.
	 */
	Text,
	/**
	 * A body of the Water plugin (GhzGarage/mantle#997): a lake, river, ocean or custom-mesh body, with the place's Water Zone in
	 * settings.water. Added without a schema version bump, and not behind a client feature: every client reads it.
	 */
	WaterBody
};

/** A WaterBody's kind (#997), the place file's properties.bodyType: the Water plugin's lake, river, ocean or custom-mesh body. */
enum class EMantleWaterBodyType : uint8
{
	/** A closed spline loop, still water. */
	Lake,
	/** An open spline with a width and a flow at each point. */
	River,
	/** A closed spline loop that reaches out to OceanExtent. */
	Ocean,
	/** A static mesh stands for the surface; no spline. */
	Custom
};

/** A seat's style (GhzGarage/MantleCore#142), the place file's seats[].style: it only picks the sit clip set. */
enum class EMantleSeatStyle : uint8
{
	/** Sitting on a bench, a chair-height seat or a ledge; the game fits the hip to the floor under the seat. */
	Bench
};

/** How a water spline runs out of one of its points (#997), the place file's point interp: the engine's ESplinePointType. */
enum class EMantleWaterSplinePointType : uint8
{
	Curve,
	Linear,
	Constant,
	CurveClamped,
	CurveCustomTangent
};

/** A Text entity's horizontal alignment (#724), the place file's properties.horizontalAlignment. */
enum class EMantleTextHorizontalAlignment : uint8
{
	Left,
	Center,
	Right
};

/** A Text entity's vertical alignment (#724), the place file's properties.verticalAlignment: where the entity's origin sits on the text. */
enum class EMantleTextVerticalAlignment : uint8
{
	Top,
	Center,
	Bottom,
	Baseline
};

/** A ReflectionProbe's influence volume (#628), the place file's properties.shape: "box" or "sphere". */
enum class EMantleReflectionProbeShape : uint8
{
	/** A box that turns with the entity, blended out over TransitionDistance at its faces. */
	Box,
	/** A sphere with a hard edge at InfluenceRadius. */
	Sphere
};

/** A Constraint's kind (#528), the place file's properties.type: "weld", "hinge" or "rope". */
enum class EMantleConstraintType : uint8
{
	/** No movement between the two. */
	Weld,
	/** entityA turns about one axis through its anchor, and only that. */
	Hinge,
	/** The anchors stay within a length of each other; each entity turns freely. */
	Rope
};

enum class EMantleAssetKind : uint8
{
	StaticMesh,
	Material,
	Texture,
	Sound,
	/** A place skeletal mesh (MSK1 role 3, its skeleton embedded) and a clip for it (MAN1 with its skeleton), #469. */
	SkeletalMesh,
	AnimClip,
	/** A Terrain's heights (#466), cooked MHF1 (FMantleHeightfieldReader). */
	Heightfield,
	/** A creator-published vehicle (#435): MVH1, its body and its document. */
	Vehicle,
	/** A creator-published weapon (#789): MWP1, its mesh and its document. */
	Weapon
};

enum class EMantleCollision : uint8
{
	None,
	Blocking,
	Overlap
};

enum class EMantleTriggerShape : uint8
{
	Box,
	Sphere
};

enum class EMantleRevisionState : uint8
{
	Draft,
	Live,
	Archived
};

/** Which structurally distinct capability surface a script receives. Must match its path's top folder. */
enum class EMantleScriptSide : uint8
{
	Server,
	Client,
	Shared
};

/** Authoring language and runtime selected for a script definition. */
enum class EMantleScriptLanguage : uint8
{
	Luau,
	JavaScript,
	TypeScript,
	/** S6.1: PocketPy. Like JavaScript/Luau, carries no separate author source. */
	Python
};

/**
 * A place's maturity rating (#320), the place file's audience.maturity. A placeholder taxonomy
 * until #42 decides the real one, which replaces this. Unrated is what a place that never
 * declared one is. Catalogue metadata only: nothing in the world reads it.
 */
enum class EMantleMaturity : uint8
{
	Unrated,
	Everyone,
	Teen,
	Mature
};

/**
 * The units a Point or Spot light's intensity is in (#385), the place file's intensityUnits.
 * Mirrors the engine's ELightUnits member for member and in the same order. A light without
 * the field is Unitless, the engine's constructor default and what every place before #385
 * meant.
 */
enum class EMantleLightUnits : uint8
{
	Unitless,
	Candelas,
	Lumens,
	EV,
	Nits
};

/**
 * A physics entity's surface preset (#528), the place file's physics.material. Each maps to a
 * UPhysicalMaterial shipped in MantleCore (MantlePhysicsMaterialAssetPath). The order is
 * the wire's: AMantlePhysicsProp replicates it as one byte, so append, never reorder.
 */
enum class EMantlePhysicsMaterial : uint8
{
	Default,
	Rubber,
	Ice,
	Metal,
	Wood,
	Stone
};

/** How many EMantlePhysicsMaterial values there are. */
inline constexpr int32 MantlePhysicsMaterialCount = 6;

/** A platform a place may be offered on (#320), one entry of the place file's audience.platforms. */
enum class EMantlePlatform : uint8
{
	PC,
	PlayStation,
	Xbox,
	Switch,
	IOS,
	Android
};

/**
 * True when the string could be a place id (a uuid).
 *
 * Only a shape check: whether the place exists is the server's answer. It exists so a mistyped id fails
 * immediately with something a player can act on, rather than as a 404 that looks identical to a place that
 * was deleted.
 */
MANTLERUNTIME_API bool MantleLooksLikePlaceId(const FString& Candidate);

MANTLERUNTIME_API const TCHAR* MantleEntityTypeToString(EMantleEntityType Type);
MANTLERUNTIME_API bool MantleParseEntityType(const FString& In, EMantleEntityType& Out);
MANTLERUNTIME_API bool MantleParseAssetKind(const FString& In, EMantleAssetKind& Out);
MANTLERUNTIME_API bool MantleParseCollision(const FString& In, EMantleCollision& Out);
MANTLERUNTIME_API bool MantleParseTriggerShape(const FString& In, EMantleTriggerShape& Out);
MANTLERUNTIME_API bool MantleParseRevisionState(const FString& In, EMantleRevisionState& Out);
MANTLERUNTIME_API const TCHAR* MantleScriptSideToString(EMantleScriptSide Side);
MANTLERUNTIME_API bool MantleParseScriptSide(const FString& In, EMantleScriptSide& Out);
MANTLERUNTIME_API const TCHAR* MantleScriptLanguageToString(EMantleScriptLanguage Language);
MANTLERUNTIME_API bool MantleParseScriptLanguage(const FString& In, EMantleScriptLanguage& Out);
MANTLERUNTIME_API const TCHAR* MantleMaturityToString(EMantleMaturity Maturity);
MANTLERUNTIME_API bool MantleParseMaturity(const FString& In, EMantleMaturity& Out);
MANTLERUNTIME_API const TCHAR* MantleLightUnitsToString(EMantleLightUnits Units);
MANTLERUNTIME_API bool MantleParseLightUnits(const FString& In, EMantleLightUnits& Out);
MANTLERUNTIME_API const TCHAR* MantlePlatformToString(EMantlePlatform Platform);
MANTLERUNTIME_API bool MantleParsePlatform(const FString& In, EMantlePlatform& Out);
/** "default", "rubber", "ice", "metal", "wood" or "stone" (#528). */
MANTLERUNTIME_API const TCHAR* MantlePhysicsMaterialToString(EMantlePhysicsMaterial Material);
/** Case-exact, as the schema's enum. */
MANTLERUNTIME_API bool MantleParsePhysicsMaterial(const FString& In, EMantlePhysicsMaterial& Out);
/** "weld", "hinge", "rope" (#528); parsing is case-exact, as the schema's enum is. */
MANTLERUNTIME_API const TCHAR* MantleConstraintTypeToString(EMantleConstraintType Type);
MANTLERUNTIME_API bool MantleParseConstraintType(const FString& In, EMantleConstraintType& Out);
/** "box", "sphere" (#628); parsing is case-exact, as the schema's enum is. */
MANTLERUNTIME_API const TCHAR* MantleReflectionProbeShapeToString(EMantleReflectionProbeShape Shape);
MANTLERUNTIME_API bool MantleParseReflectionProbeShape(const FString& In, EMantleReflectionProbeShape& Out);
MANTLERUNTIME_API const TCHAR* MantleTextHorizontalAlignmentToString(EMantleTextHorizontalAlignment Alignment);
MANTLERUNTIME_API bool MantleParseTextHorizontalAlignment(const FString& In, EMantleTextHorizontalAlignment& Out);
MANTLERUNTIME_API const TCHAR* MantleTextVerticalAlignmentToString(EMantleTextVerticalAlignment Alignment);
MANTLERUNTIME_API bool MantleParseTextVerticalAlignment(const FString& In, EMantleTextVerticalAlignment& Out);
/** "lake", "river", "ocean", "custom" (#997); parsing is case-exact, as the schema's enum is. */
MANTLERUNTIME_API const TCHAR* MantleWaterBodyTypeToString(EMantleWaterBodyType Type);
MANTLERUNTIME_API bool MantleParseWaterBodyType(const FString& In, EMantleWaterBodyType& Out);
/** "bench" (#142); parsing is case-exact, and any other text is refused, with no fallback. */
MANTLERUNTIME_API const TCHAR* MantleSeatStyleToString(EMantleSeatStyle Style);
MANTLERUNTIME_API bool MantleParseSeatStyle(const FString& In, EMantleSeatStyle& Out);
/** "curve", "linear", "constant", "curveClamped", "curveCustomTangent" (#997); parsing is case-exact. */
MANTLERUNTIME_API const TCHAR* MantleWaterSplinePointTypeToString(EMantleWaterSplinePointType Type);
MANTLERUNTIME_API bool MantleParseWaterSplinePointType(const FString& In, EMantleWaterSplinePointType& Out);

struct FMantleRevision
{
	int32 Number = 0;
	FString Notes;
	FString PublishedAt;
	EMantleRevisionState State = EMantleRevisionState::Draft;
};

struct FMantleMetadata
{
	FString Name;
	FString Description;
	FString AuthorId;
	FString CreatedAt;
	FString UpdatedAt;
	FString Thumbnail;
};

/** settings.environment.exposure.mode (#628): the game's own constant exposure, or one that adapts to the view. */
enum class EMantleExposureMode : uint8
{
	Fixed,
	Auto,
};

/**
 * settings.environment.exposure (#628). Bias is in stops over the game's own exposure compensation (0 is the game's own).
 * The brightness range is read only with Auto; an end the file leaves out is the engine's default one
 * (MantleEnvironmentDefaults::DefaultAutoExposureMin and Max).
 */
struct FMantleExposureSettings
{
	EMantleExposureMode Mode = EMantleExposureMode::Fixed;
	double Bias = 0.0;
	TOptional<double> MinBrightness;
	TOptional<double> MaxBrightness;
};

/**
 * settings.environment.fog (#628): the height fog's look. Each value is unset when the file leaves its key out, which
 * keeps the game's own fog value. Colours are linear luminance (0 to MaxFogColor a channel), the distance is in cm.
 */
struct FMantleFogSettings
{
	TOptional<double> Density;
	TOptional<double> HeightFalloff;
	TOptional<FLinearColor> InscatteringColor;
	TOptional<double> StartDistance;
	TOptional<FLinearColor> DirectionalInscatteringColor;

	/** True when the file set none of the keys. */
	bool IsEmpty() const
	{
		return !Density.IsSet() && !HeightFalloff.IsSet() && !InscatteringColor.IsSet() && !StartDistance.IsSet()
			&& !DirectionalInscatteringColor.IsSet();
	}
};

/**
 * The place's default look (#532), the place file's optional settings.environment: the clock,
 * weather, colour grade, bloom and grading preset every player starts in, with or without a script. Each value is unset
 * when the file leaves its key out. The server seeds the replicated environment override from it
 * once, as the place loads (UMantleEnvironmentSubsystem::MakeSettingsOverride); a server script's
 * Environment writes change it from there. The ranges are the script API's (MantleEnvironmentDefaults).
 */
struct FMantleEnvironmentSettings
{
	/** clockTime: the hour the clock starts at, 0 to 24. */
	TOptional<double> ClockTime;
	/** timeScale: how fast the clock runs, 0 (stopped) to MaxScriptTimeScale; 1 is the default rate. */
	TOptional<double> TimeScale;
	/** weather: a weather held from the start. Unset: the natural weather. */
	TOptional<EMantleWeatherState> Weather;
	/** colorTint: 0xRRGGBB (sRGB) multiplied over the picture; 0xFFFFFF is unchanged. */
	TOptional<uint32> ColorTint;
	/** saturation: 0 (black and white) to MaxScriptSaturation; 1 is unchanged. */
	TOptional<double> Saturation;
	/** bloom: 0 (no glow) to MaxScriptBloom. Unset: the engine's own bloom. */
	TOptional<double> Bloom;
	/** colorGrading: a MantleEnvironmentOverrideValues::ColorGrading* preset, never None. Unset: none. */
	TOptional<uint8> ColorGrading;
	/** colorGradingIntensity: how much of the preset shows, 0 to 1; 1 is the full look. */
	TOptional<double> ColorGradingIntensity;
	/**
	 * exposure (#628): how the picture's exposure is set. Unset: the game's own fixed exposure. Applied on clients as the place
	 * loads (UMantleEnvironmentSubsystem::SetPlaceExposure), not through the replicated override: every client has the place.
	 */
	TOptional<FMantleExposureSettings> Exposure;
	/** fog (#628): the height fog's look, applied to the fog Mantle spawns before the weather records its baselines. */
	TOptional<FMantleFogSettings> Fog;
	/**
	 * skyLight.intensity (#722): a scale, 0 to 1, on Mantle's own sky light, which captures the open sky. An indoor level
	 * lowers it so its walls are not lit by a sky the level does not show. Unset: 1, Mantle's own.
	 */
	TOptional<double> SkyLightIntensity;
	/**
	 * sun.direct (#722): false when the level's sun never reaches the places a player stands (a sealed room with its sun
	 * baked), so the client does not light the place with it. Only false is written; true is Mantle's own.
	 */
	bool bNoDirectSun = false;

	/** True when the file set none of the keys (an absent or empty block). */
	bool IsEmpty() const
	{
		return !ClockTime.IsSet() && !TimeScale.IsSet() && !Weather.IsSet() && !ColorTint.IsSet() && !Saturation.IsSet()
			&& !Bloom.IsSet() && !ColorGrading.IsSet() && !ColorGradingIntensity.IsSet() && !Exposure.IsSet() && !Fog.IsSet()
			&& !SkyLightIntensity.IsSet() && !bNoDirectSun;
	}
};

/** The place's Water Zone (#997), the place file's settings.water: the area the Water plugin simulates and draws water in. */
struct FMantleWaterZone
{
	/** The zone's full size, cm, each axis FMantlePlaceLimits::MinWaterExtent to MaxWaterExtent. */
	FVector2D ZoneExtent = FVector2D(FMantlePlaceLimits::DefaultWaterZoneExtent, FMantlePlaceLimits::DefaultWaterZoneExtent);
	FVector ZoneLocation = FVector::ZeroVector;
	/** -180 to 180 degrees. */
	double ZoneYawDeg = 0.0;
};

struct FMantleSettings
{
	int32 MaxPlayers = 16;
	/** Downward gravity in cm/s^2 (980 is Earth). The builder sets the world's gravity Z to its negative (#453). */
	double Gravity = 980.0;
	/** Reserved for a future creator-configurable sky. Accepted; every place gets the default sky (#445). */
	FString Skybox;
	/**
	 * Deprecated (#445): accepted but ignored at runtime, and no longer exported. An ambient term
	 * on top of the real-time sky light would double up and fight the day/night cycle.
	 */
	FLinearColor AmbientLight = FLinearColor::Black;
	FString RespawnMode = TEXT("nearest");
	/** Deprecated (#453): accepted but ignored at runtime, and no longer exported. */
	double StreamingDistance = 15000.0;
	/** Whether players other than the owner may host community servers for this place (#178). */
	bool bAllowCommunityServers = false;
	/** Whether players other than the owner may create private servers for this place (#416). */
	bool bAllowPrivateServers = false;
	/**
	 * settings.avatarHeight (#581): false for "player" (the default: each character's capsule,
	 * hitboxes and eye follow its avatar's height), true for "uniform" (every character plays at
	 * the one standard size; avatars still look as they are).
	 */
	bool bUniformAvatarHeight = false;

	/**
	 * Whether a player may pull another out of this place's cars (#546): the vehicle key at an
	 * occupied seat takes it. On unless the file sets settings.carjacking to false.
	 */
	bool bCarjacking = true;
	/** The place's default look (#532). Empty when the file has no settings.environment. */
	FMantleEnvironmentSettings Environment;
	/** The place's Water Zone (#997). Unset when the file has no settings.water. */
	TOptional<FMantleWaterZone> Water;
};

/**
 * Who and where a place is for (#320), the place file's optional audience block. A place
 * without the block is unrated and available on every platform.
 */
struct FMantleAudience
{
	EMantleMaturity Maturity = EMantleMaturity::Unrated;

	/**
	 * The platforms the place may be offered on. Empty means every platform, including ones
	 * added later; otherwise exactly these, each once.
	 */
	TArray<EMantlePlatform> Platforms;

	/** True when there is nothing to write: unrated and every platform. */
	bool IsDefault() const
	{
		return Maturity == EMantleMaturity::Unrated && Platforms.Num() == 0;
	}
};

/**
 * One sit spot (GhzGarage/MantleCore#142), the place file's seats[]: a place-level list rather than an entity, because
 * a prop can have several. Added without a schema version bump.
 */
struct FMantleSeat
{
	/** The id of the entity the seat belongs to; it must be in the place's entities. */
	FString Entity;
	/** Where the hip rests, in the owning entity's local space (centimetres). */
	FVector Position = FVector::ZeroVector;
	/** The way the seated body faces: degrees about up, in the owning entity's local space. Yaw only. */
	double Yaw = 0.0;
	EMantleSeatStyle Style = EMantleSeatStyle::Bench;
};

struct FMantleLocalization
{
	FString DefaultLocale = TEXT("en");
	TMap<FString, TMap<FString, FString>> Translations;
};

struct FMantleAssetRef
{
	FString Id;
	EMantleAssetKind Kind = EMantleAssetKind::StaticMesh;
	FString Hash;
	int64 Bytes = 0;
};

/**
 * A reference to separately stored executable source. Never inline code — the place file stays
 * inert data, which is what makes it safe to accept from strangers.
 */
struct FMantleScript
{
	FString Id;
	FString Name;

	/**
	 * Relative to Scripts/, for example "Server/KillBricks.luau". The top folder is
	 * case-exact and must match Side; the parser enforces both.
	 */
	FString Path;

	EMantleScriptSide Side = EMantleScriptSide::Server;
	EMantleScriptLanguage Language = EMantleScriptLanguage::Luau;

	/** Storage reference to executable Luau or JavaScript source, not the source itself. */
	FString Source;

	/** Original TypeScript or graph JSON source for editor re-import; empty for directly executable source. */
	FString AuthorSource;

	/** True when AuthorSource is a visual-script graph and Source is its compiled Luau. */
	bool bAuthoredAsGraph = false;
};

/**
 * An RmlUi document or stylesheet a place ships (#549). Like a script, the place file carries a
 * reference to separately stored, screened text, never the text itself. Only clients fetch it.
 */
struct FMantleUIDocument
{
	/** uid_ and 1 to 40 of a-z, 0-9 and '_'; unique in the place. */
	FString Id;

	/** Relative to the level folder, for example "UI/Shop.rml"; unique ignoring case. */
	FString Path;

	/** Storage reference (src_ id) to the .rml or .rcss text, served by GET /scripts/{id}/source. Empty for an image. */
	FString Source;

	/**
	 * #556 item 5: an image's (.png or .jpg) uploaded Texture asset, which the place's assets list;
	 * empty for a document or stylesheet. Exactly one of Source and Asset is set.
	 */
	FString Asset;

	bool IsImage() const { return !Asset.IsEmpty(); }
};

/**
 * One library item a place holds (#614 stage 4), the ServerStorage analogue: a creator-chosen name
 * for an immutable item version. The place file carries only this reference; the backing asset ids
 * come from the storage resolve endpoint when the place loads.
 */
struct FMantleStorageEntry
{
	/** An ASCII letter then up to 63 letters, digits or '_'; unique in the place (case-sensitive). */
	FString Name;

	/** itm_u and 32 lowercase hex digits. */
	FString Item;

	/** The pinned version, at least 1. */
	int32 Version = 1;

	/**
	 * A script package's package-wide settings (#838 stage 6), the place file's optional "settings": {key: value}, every value
	 * text whatever the setting's type. At most FMantlePlaceLimits::MaxStorageSettings keys. Only a script entry may carry any
	 * (the backend refuses them on a vehicle or weapon). TMap's keys ignore case, so a lookup that matters compares the stored
	 * key exactly (FindSetting).
	 */
	TMap<FString, FString> Settings;

	/** The setting stored under exactly Key (case-sensitive), or null. */
	const FString* FindSetting(const FString& Key) const
	{
		for (const TPair<FString, FString>& Pair : Settings)
		{
			if (Pair.Key.Equals(Key, ESearchCase::CaseSensitive))
			{
				return &Pair.Value;
			}
		}
		return nullptr;
	}

	bool operator==(const FMantleStorageEntry& Other) const
	{
		if (!Name.Equals(Other.Name, ESearchCase::CaseSensitive)
			|| !Item.Equals(Other.Item, ESearchCase::CaseSensitive)
			|| Version != Other.Version
			|| Settings.Num() != Other.Settings.Num())
		{
			return false;
		}
		for (const TPair<FString, FString>& Pair : Settings)
		{
			const FString* Theirs = Other.FindSetting(Pair.Key);
			if (!Theirs || !Theirs->Equals(Pair.Value, ESearchCase::CaseSensitive))
			{
				return false;
			}
		}
		return true;
	}
};

/**
 * One file of a stored script package (#838), as the storage resolve answers it: the same {path, side, language,
 * source} a place's own scripts[] entry carries. Path is relative to the package ("Server/Main.luau"); the script
 * catalogue mounts it at Packages/<storage name>/<Path>.
 */
struct FMantleStoredScriptFile
{
	FString Path;
	EMantleScriptSide Side = EMantleScriptSide::Server;
	EMantleScriptLanguage Language = EMantleScriptLanguage::Luau;
	/** src_ and 32 lowercase hex digits: fetched like a place script's source. */
	FString Source;
};

/**
 * The type a script package declares for one of its options (#838), a Script Point parameter or (stage 6) a package setting: how
 * the stored text value is read. Choice (stage 6) is text that must be one of the option's Options.
 */
enum class EMantleScriptPackageParameterType : uint8
{
	String,
	Number,
	Boolean,
	Choice
};

/** One option a script package declares in its manifest (#838): a parameter a Script Point sets, or (stage 6) a setting its storage entry sets. */
struct FMantleScriptPackageParameter
{
	/** The key it describes (FMantlePlaceLimits::IsScriptPointParameterName). */
	FString Name;
	EMantleScriptPackageParameterType Type = EMantleScriptPackageParameterType::String;
	/** True when the manifest gives a default; Default is then its text ("12.5", "true", or the string itself). */
	bool bHasDefault = false;
	FString Default;
	FString Help;
	/** Stage 6: the place must store a value when there is no default (a required option with a default is satisfied by it). */
	bool bRequired = false;
	/** Stage 6, a number only: the lowest and highest value it takes, when given (Min <= Max). */
	bool bHasMin = false;
	double Min = 0.0;
	bool bHasMax = false;
	double Max = 0.0;
	/** Stage 6, a choice only: the values it takes, matched case-sensitively (2 to 32, each 1 to 64 characters). */
	TArray<FString> Options;

	/** True when the place must store a value: required, with no default. */
	bool NeedsValue() const { return bRequired && !bHasDefault; }

	/** True when Value is exactly one of Options (case-sensitive). */
	bool HasOption(const FString& Value) const
	{
		return Options.ContainsByPredicate([&Value](const FString& Option) { return Option.Equals(Value, ESearchCase::CaseSensitive); });
	}
};

/** A script package's manifest (#838): package.json on disk, the resolve answer's manifest when stored. */
struct FMantleScriptPackageManifest
{
	FString Name;
	FString Description;
	TArray<FMantleScriptPackageParameter> Parameters;
	/** Stage 6: the package-wide settings its storage entry may set, at most FMantlePlaceLimits::MaxStorageSettings. */
	TArray<FMantleScriptPackageParameter> Settings;

	/** The declared parameter named exactly Name (case-sensitive), or null. */
	const FMantleScriptPackageParameter* FindParameter(const FString& ParameterName) const
	{
		return Parameters.FindByPredicate([&ParameterName](const FMantleScriptPackageParameter& Parameter)
		{
			return Parameter.Name.Equals(ParameterName, ESearchCase::CaseSensitive);
		});
	}

	/** The declared setting named exactly Name (case-sensitive), or null. */
	const FMantleScriptPackageParameter* FindSetting(const FString& SettingName) const
	{
		return Settings.FindByPredicate([&SettingName](const FMantleScriptPackageParameter& Setting)
		{
			return Setting.Name.Equals(SettingName, ESearchCase::CaseSensitive);
		});
	}
};

/**
 * One answer entry of the storage resolve routes (#614 stage 4): a stored library item version expanded into
 * the uploaded assets that back it. Defined here, not in MantlePlaceStorage.h, because a loaded place keeps the
 * entries that resolved (FMantlePlace::StorageResolved, #614 stage 5): the script API's authorization reads them.
 */
struct FMantleResolvedStorageEntry
{
	/** The place's name for the item, as the backend echoes it. Informational: entries are matched by Item and Version. */
	FString Name;

	/** itm_u and 32 lowercase hex digits. */
	FString Item;

	int32 Version = 0;

	/** "vehicle", "weapon" or (#838) "script". */
	FString Kind;

	/** "active" or "delisted" both still load for a place that pinned the version; "taken_down" (#614 stage 7) never loads. */
	FString Status;

	/** The item's own uploaded asset (a vehicle's document): ast_u and 32 lowercase hex digits. */
	FString PrimaryAssetId;

	/**
	 * Every asset the primary one needs. The backend pins exactly the ids the item's document names, which the
	 * client's vehicle build already fetches from that document, so nothing reads this list at load.
	 */
	TArray<FString> DependencyAssetIds;

	/**
	 * A script package's files (#838), empty for every other kind. The server's route answers its server and shared
	 * files, a player's route its client and shared files. Read leniently: a malformed file is left out, not a failure.
	 */
	TArray<FMantleStoredScriptFile> Scripts;

	/** A script package's manifest (#838), empty for every other kind or when the answer carries none. */
	FMantleScriptPackageManifest Manifest;

	/**
	 * A script package's stored settings (#838 stage 6) as the sealed revision recorded them, raw text; bHasSettings says whether
	 * the answer carried the key at all (an older backend's does not, and the place file's entry is read instead). Read leniently:
	 * a key that is not a setting name or a value that is not text is left out.
	 */
	bool bHasSettings = false;
	TMap<FString, FString> Settings;
};

/** Per-entity-type properties. One struct per member of the closed vocabulary. */

/**
 * A StaticMesh's physics block (#456): the entity is a rigid body the server simulates and every
 * client follows through the engine's replicated physics movement (AMantlePhysicsProp). Present at
 * all makes the entity dynamic, whether or not it starts simulating: a script can let an anchored
 * one fall later (Entity.Anchored).
 */
struct FMantlePhysicsProps
{
	/** Simulate from the start. False starts anchored (kinematic) until a script un-anchors it. */
	bool bSimulate = true;

	/** Mass in kg, FMantlePlaceLimits::MinPhysicsMass to MaxPhysicsMass. Unset: computed from the collision volume. */
	TOptional<double> Mass;

	/** The engine's per-body drag, 0 to FMantlePlaceLimits::MaxPhysicsDamping. These are its defaults. */
	double LinearDamping = 0.01;
	double AngularDamping = 0.0;

	/** Whether the place's gravity pulls it. */
	bool bGravity = true;

	/** The surface preset (#528): friction, restitution and density from a physical material MantleCore ships. */
	EMantlePhysicsMaterial Material = EMantlePhysicsMaterial::Default;
};

struct FMantleStaticMeshProps
{
	FString Mesh;
	TArray<FString> Materials;
	EMantleCollision Collision = EMantleCollision::Blocking;
	bool bCastShadow = true;

	/** A StaticMesh entity's physics block (#456). Never set on an InstancedMesh's copy of these props. */
	TOptional<FMantlePhysicsProps> Physics;

	/**
	 * What the mesh is made of (#596), the place file's surface: the builder gives the mesh that
	 * surface's physical material (MantleLoadSurfaceMaterial). Always Default beside Physics, whose
	 * material carries the body's surface; the parser refuses the two together.
	 */
	EMantleSurface Surface = EMantleSurface::Default;

	/**
	 * drawDistance (#630 stage 2): how far from the camera the mesh is drawn, in cm, above 0 and at most
	 * FMantlePlaceLimits::MaxDrawDistance. The builder makes it a StaticMesh's Max Draw Distance, or an InstancedMesh's
	 * instance cull distances with a dithered fade, and stops the mesh casting shadows past a share of it
	 * (UMantleShadowDistanceSubsystem). 0, the key absent: never culled by distance, as before the key.
	 */
	double DrawDistance = 0.0;
};

struct FMantleEmptyProps
{
};

/**
 * A light's temperature in kelvin when a place file gives none (#472): the engine's own default,
 * and a white light. Only used when bUseTemperature is set.
 */
#define MANTLE_DEFAULT_LIGHT_TEMPERATURE 6500.0

/**
 * A Point or Spot light's source shape (#472): the sphere (or, with SourceLength, the capsule) its
 * light comes from, in cm. Zero, the default and what every place before the fields meant, is a
 * point source. The exporter writes each only when above zero.
 */
struct FMantleLightSourceShape
{
	double SourceRadius = 0.0;
	double SoftSourceRadius = 0.0;
	double SourceLength = 0.0;

	bool IsDefault() const { return SourceRadius == 0.0 && SoftSourceRadius == 0.0 && SourceLength == 0.0; }
};

struct FMantlePointLightProps
{
	double Intensity = 0.0;
	EMantleLightUnits IntensityUnits = EMantleLightUnits::Unitless;
	FLinearColor Color = FLinearColor::White;
	double Radius = 0.0;
	bool bCastShadow = true;
	/** Tint by Temperature (#472). Written only when set. */
	bool bUseTemperature = false;
	double Temperature = MANTLE_DEFAULT_LIGHT_TEMPERATURE;
	FMantleLightSourceShape Source;
};

struct FMantleSpotLightProps
{
	double Intensity = 0.0;
	EMantleLightUnits IntensityUnits = EMantleLightUnits::Unitless;
	FLinearColor Color = FLinearColor::White;
	double Radius = 0.0;
	double InnerCone = 0.0;
	double OuterCone = 0.0;
	bool bCastShadow = true;
	/** Tint by Temperature (#472). Written only when set. */
	bool bUseTemperature = false;
	double Temperature = MANTLE_DEFAULT_LIGHT_TEMPERATURE;
	FMantleLightSourceShape Source;
};

/**
 * An area light (#472): a SourceWidth by SourceHeight cm rectangle facing along the entity's X
 * axis, with barn doors. The defaults are URectLightComponent's.
 */
struct FMantleRectLightProps
{
	double Intensity = 0.0;
	EMantleLightUnits IntensityUnits = EMantleLightUnits::Unitless;
	FLinearColor Color = FLinearColor::White;
	/** Attenuation radius, cm. */
	double Radius = 0.0;
	double SourceWidth = 64.0;
	double SourceHeight = 64.0;
	/** Degrees, 0 (closed) to FMantlePlaceLimits::MaxRectLightBarnDoorAngle. */
	double BarnDoorAngle = 88.0;
	/** cm. */
	double BarnDoorLength = 20.0;
	bool bUseTemperature = false;
	double Temperature = MANTLE_DEFAULT_LIGHT_TEMPERATURE;
	bool bCastShadow = true;
};

/**
 * A decal (#472): the BaseColor texture (its alpha the coverage) and an optional Normal map,
 * projected along the entity's X axis through the box Size describes (UDecalComponent's DecalSize),
 * drawn with Mantle's cooked decal material. Both are Texture asset ids.
 */
struct FMantleDecalProps
{
	FString BaseColor;
	/** Empty when the decal has no normal map. */
	FString Normal;
	FVector Size = FVector(128.0, 256.0, 256.0);
	/** 0 to 1, multiplied into the base colour's alpha. */
	double Opacity = 1.0;
	/** Higher draws later, on top; -MaxDecalSortOrder to MaxDecalSortOrder. */
	int32 SortOrder = 0;
	/** Screen size below which the decal stops drawing, 0 to 1; 0 never fades. UDecalComponent's default. */
	double FadeScreenSize = 0.01;
};

struct FMantleDirectionalLightProps
{
	double Intensity = 0.0;
	FLinearColor Color = FLinearColor::White;
	bool bCastShadow = true;
};

struct FMantleTriggerVolumeProps
{
	EMantleTriggerShape Shape = EMantleTriggerShape::Box;
	FVector Extent = FVector::OneVector;
};

/**
 * Audible range of a sound emitter whose place file gives no radius, or whose AmbientSound has
 * no attenuation to read one from: 40 m, the reach of UE's own default attenuation settings
 * (a 400 cm sphere plus 3600 cm of falloff).
 */
#define MANTLE_DEFAULT_SOUND_RADIUS 4000.0

struct FMantleSoundEmitterProps
{
	FString Sound;
	double Volume = 1.0;
	/** Distance in cm at which the sound has faded to silence. Always spatialised (#263). */
	double Radius = MANTLE_DEFAULT_SOUND_RADIUS;
	/**
	 * Loop for as long as the place is loaded. False plays the sound once, when the place is
	 * built on a player's machine (on joining).
	 */
	bool bLooping = false;
	/**
	 * Start when the place is built (#459). False builds it silent, its sound bound, until a
	 * script's Entity:PlaySound; the place file carries the key only when it is false. Exported
	 * from the AmbientSound's audio component's Auto Activate.
	 */
	bool bAutoplay = true;
};

/** One lane of a CrowdSpawner (#477): a polyline NPCs walk, in the spawner's local space. */
struct FMantleCrowdLane
{
	/** 2 to FMantlePlaceLimits::MaxCrowdLanePoints points, cm. Last == first makes a loop. */
	TArray<FVector> Points;

	/** Width of the walkable band, cm. Each NPC keeps a seeded sideways offset inside it. */
	double Width = 200.0;

	/** NPCs walk both ways and turn round at the ends. False walks start to end only. */
	bool bTwoWay = true;
};

/** One worn item of a crowd look: avatar descriptor v2's wearing entry. */
struct FMantleCrowdWorn
{
	/** A wear- or accessory-group slot id from schema/avatar/limits.json, e.g. "top". */
	FString Slot;
	/** itm_u + 32 lowercase hex. */
	FString ItemId;
};

/**
 * One look a crowd NPC may wear: avatar descriptor v2's body and wearing, without schemaVersion
 * or emotes. An empty SkinToneId/EyeColorId or a zero HeightPct is picked from the spawner's seed.
 */
struct FMantleCrowdLook
{
	FString AvatarItemId;
	FString SkinToneId;
	FString EyeColorId;
	/** 95 to 105, or 0 when the seed picks it. */
	int32 HeightPct = 0;
	TArray<FMantleCrowdWorn> Wearing;
};

/**
 * A place a CrowdSpawner's NPCs walk over to and linger at (#513): another entity of the place,
 * where NPCs passing the nearest point of a lane may stand round it for a while.
 */
struct FMantleCrowdPointOfInterest
{
	/** ent_ id of another entity in this place. */
	FString EntityId;
	/** Chance an NPC passing it stops by, 0 to 1. */
	double Chance = 0.5;
	/** How long an NPC lingers, seconds, FMantlePlaceLimits' linger bounds, min <= max. */
	double MinLingerSeconds = 4.0;
	double MaxLingerSeconds = 10.0;
	/** How far from the entity NPCs stand, cm. */
	double Radius = 150.0;
};

/**
 * Ambient pedestrians (#477). Only this is place data: every client derives the same NPCs from
 * Seed plus server time, a dedicated server spawns none, and nothing per NPC replicates.
 */
struct FMantleCrowdSpawnerProps
{
	TArray<FMantleCrowdLane> Lanes;
	/** 1 to FMantlePlaceLimits::MaxCrowdCount. */
	int32 Count = 0;
	/** 0 to FMantlePlaceLimits::MaxCrowdSeed. */
	int32 Seed = 0;
	/** Empty means the two base bodies with no garments. */
	TArray<FMantleCrowdLook> Looks;
	/** For looks that don't fix heightPct. */
	int32 MinHeightPct = 95;
	int32 MaxHeightPct = 105;
	/** For looks that don't fix skinToneId. Empty means the whole palette. */
	TArray<FString> SkinTones;
	/** cm/s. */
	double MinWalkSpeed = 110.0;
	double MaxWalkSpeed = 160.0;
	/** Chance an NPC stops for a while at each lane point it passes, 0 to 1. */
	double IdleChance = 0.15;
	/** Where NPCs gather (#513), at most FMantlePlaceLimits::MaxCrowdPointsOfInterest. */
	TArray<FMantleCrowdPointOfInterest> PointsOfInterest;
	bool bEnabled = true;
};

/** A VehicleSpawn's respawn delay when the place file gives none (#435). */
#define MANTLE_DEFAULT_VEHICLE_RESPAWN_SECONDS 5.0

/**
 * A creator vehicle parked at the entity's transform (#435). The server spawns it when the place is
 * built and again RespawnSeconds after it is destroyed; it belongs to no team.
 */
struct FMantleVehicleSpawnProps
{
	/**
	 * An uploaded Vehicle asset: ast_u and 32 lowercase hex digits. A spawn that names StorageItem
	 * has none in the place file: it is empty until FMantleStorage::Apply (#614 stage 4) rewrites it
	 * to the stored item's resolved primaryAssetId, and holds that id from then on. Everything past
	 * parsing (asset preparation, the gameplay bridge) reads it as an ordinary uploaded vehicle.
	 */
	FString Vehicle;
	/** #614 stage 4: the name of a storage[] entry (a vehicle library item) to spawn instead of Vehicle. Exactly one of the two is set in a place file. */
	FString StorageItem;
	/** 0 to FMantlePlaceLimits::MaxVehicleRespawnSeconds. */
	double RespawnSeconds = MANTLE_DEFAULT_VEHICLE_RESPAWN_SECONDS;
	/** False keeps the spawn without a vehicle. */
	bool bEnabled = true;
	/** #488: which of the vehicle's liveries the parked car wears, by index; -1 (the default) is the vehicle's defaultLivery. */
	int32 Livery = -1;
};

/** A WeaponSpawn's respawn delay when the place file gives none (#789). */
#define MANTLE_DEFAULT_WEAPON_RESPAWN_SECONDS 10.0

/**
 * A creator weapon waiting at the entity's transform (#789). Like FMantleVehicleSpawnProps: exactly one of
 * Weapon and StorageItem is set in a place file; FMantleStorage::Apply rewrites a stored one into Weapon.
 */
struct FMantleWeaponSpawnProps
{
	/** An uploaded Weapon asset: ast_u and 32 lowercase hex digits. Empty for a StorageItem spawn until Apply resolves it. */
	FString Weapon;
	/** The name of a storage[] entry (a weapon library item) to spawn instead of Weapon. */
	FString StorageItem;
	/** 0 to FMantlePlaceLimits::MaxWeaponRespawnSeconds. */
	double RespawnSeconds = MANTLE_DEFAULT_WEAPON_RESPAWN_SECONDS;
	/** False keeps the spawn without a weapon. */
	bool bEnabled = true;
};

/** One of a Script Point's parameters (#838): a key the package declares, and its value as text. */
struct FMantleScriptPointParameter
{
	/** FMantlePlaceLimits::IsScriptPointParameterName: a letter, then up to 31 letters, digits or '_'. */
	FString Name;
	/** At most FMantlePlaceLimits::MaxScriptPointParameterValueLength characters. */
	FString Value;
};

/**
 * An anchor for a stored script package at the entity's transform (#838). The package's scripts read their points
 * through Script.Points; each point's entity carries the storage name as a tag and its parameters as attributes.
 */
struct FMantleScriptPointProps
{
	/** The name of a storage[] entry (a script library item). Required. */
	FString StorageItem;
	/** At most FMantlePlaceLimits::MaxScriptPointParameters, keys unique, sorted by key (case-sensitive). */
	TArray<FMantleScriptPointParameter> Parameters;
	/** False keeps the point in the place but leaves it out of Script.Points. FMantleStorage::Apply turns a point off
	 *  whose entry is missing, taken down or not a script. */
	bool bEnabled = true;
};

/**
 * An InstancedMesh (#467): a StaticMesh's mesh, materials, collision and shadow, drawn at every
 * one of Instances. Each instance transform is relative to the entity's own transform.
 */
struct FMantleInstancedMeshProps
{
	FMantleStaticMeshProps Mesh;
	TArray<FTransform> Instances;
};

/**
 * A SkeletalMesh (#469): an uploaded skeletal mesh on its own skeleton, its materials by slot, and
 * an optional uploaded clip authored on that skeleton. Every client samples the clip at
 * StartOffset + server time x PlayRate (UMantleClipAnimInstance), so nothing per frame replicates.
 * No collision.
 */
struct FMantleSkeletalMeshProps
{
	FString Mesh;
	/** Materials by slot; an empty entry leaves the slot on the mesh's own (for an uploaded mesh, none). */
	TArray<FString> Materials;
	bool bCastShadow = true;
	/** Empty: the mesh holds its reference pose. */
	FString Clip;
	/** Wrap around the clip's end; false plays it once from server time 0 and holds the last frame. */
	bool bLoop = true;
	/** Clip seconds per server second, 0 to FMantlePlaceLimits::MaxClipPlayRate; 0 holds the pose at StartOffset. */
	double PlayRate = 1.0;
	/** Seconds into the clip at server time 0, 0 to FMantlePlaceLimits::MaxClipStartOffsetSeconds. */
	double StartOffset = 0.0;
};

/** A Terrain layer's tiling when the place file gives none: one repeat every 4 m. */
#define MANTLE_DEFAULT_TERRAIN_TILING 400.0

/** One paint layer of a Terrain (#466): a place material and how often its textures repeat. */
struct FMantleTerrainLayer
{
	/** A Material asset id, uploaded or built in. */
	FString Material;
	/** cm per repeat, above 0 and at most FMantlePlaceLimits::MaxTerrainTilingCm. */
	double Tiling = MANTLE_DEFAULT_TERRAIN_TILING;
	/** What the layer is made of (#596): a hit where this layer weighs most resolves to it (UMantleTerrainSurfaceComponent). */
	EMantleSurface Surface = EMantleSurface::Default;
};

/**
 * One Terrain grass entry (#628): a mesh every client scatters over the terrain near its camera,
 * where a grass map's channel says so. Visual only: no collision, nothing replicated.
 */
struct FMantleTerrainGrass
{
	/** An uploaded Texture asset id, stretched over the whole terrain like the weightmap. */
	FString Map;
	/** Which channel of Map is how much of this grass grows there: 0 R, 1 G, 2 B, 3 A. */
	int32 Channel = 0;
	/** A StaticMesh asset id, uploaded or built in. */
	FString Mesh;
	/** Materials by slot, as on FMantleStaticMeshProps; an empty entry leaves the slot on the mesh's own. */
	TArray<FString> Materials;
	/** Instances per square metre at full map weight, above 0 and at most FMantlePlaceLimits::MaxTerrainGrassDensity. */
	double Density = 1.0;
	/** Uniform scale, MinScale <= MaxScale. */
	double MinScale = 1.0;
	double MaxScale = 1.0;
	bool bAlignToSurface = true;
	bool bRandomYaw = true;
	/** cm from the camera: instances fade from CullStart and are gone past CullEnd (above CullStart and 0). */
	double CullStart = 0.0;
	double CullEnd = 10000.0;
	bool bCastShadow = false;
};

/**
 * Landscape terrain (#466). The entity's transform places heightfield sample (0, 0); the
 * heightfield carries its own spacing and height scale.
 */
struct FMantleTerrainProps
{
	/** An uploaded Heightfield asset id (MHF1). */
	FString Heightfield;
	/** An uploaded Texture: R, G, B, A weigh layers 0 to 3 across the terrain. Empty means layer 0 everywhere. */
	FString Weightmap;
	/** An uploaded Texture: R, G, B, A weigh layers 4 to 7 (#628), over the terrain like Weightmap. Present exactly when there are more than 4 layers. */
	FString Weightmap2;
	/** 1 to FMantlePlaceLimits::MaxTerrainLayers. */
	TArray<FMantleTerrainLayer> Layers;
	bool bCastShadow = true;
	/** Landscape grass (#628), 0 to FMantlePlaceLimits::MaxTerrainGrass entries. */
	TArray<FMantleTerrainGrass> Grass;
};

/**
 * A particle effect (#460): a built-in archetype and its look. The parser fills every key the place
 * file leaves out from the archetype's own look (FMantleEffects::DefaultLook), so Look is always whole.
 */
struct FMantleEffectProps
{
	EMantleEffectArchetype Archetype = EMantleEffectArchetype::Smoke;
	FMantleEffectLook Look = FMantleEffects::DefaultLook(EMantleEffectArchetype::Smoke);
	/** Start when a player's client builds the place; false waits for a script's Entity:PlayEffect. */
	bool bAutoplay = true;
	/** Emit for as long as it plays; false emits for FMantleEffects::OneShotEmitSeconds each time it starts. */
	bool bLoop = true;
	/** The sprites archetype's (#628) keys, resolved: only read when Archetype is Sprites, and held to the schema's ranges and pairs by the parser. */
	FMantleSpritesSettings Sprites;
	/** The sprites archetype's uploaded Texture asset (#628), empty for the built-in soft dot. */
	FString TextureId;
};

/**
 * A joint (#528): Type between the physics entity EntityA and EntityB, or the world when EntityB is
 * empty. Anchors are in each entity's own space, scale included (the world's for AnchorB without
 * EntityB). Axis, the hinge limits and Length are read for their own type only; the parser refuses
 * them on another.
 */
struct FMantleConstraintProps
{
	EMantleConstraintType Type = EMantleConstraintType::Weld;
	/** ent_ id of a StaticMesh with a physics block. */
	FString EntityA;
	/** ent_ id of another; empty means the world. */
	FString EntityB;
	/** cm, in EntityA's space: [0, 0, 0] is its origin. */
	FVector AnchorA = FVector::ZeroVector;
	/** cm, in EntityB's space (or the world's). Unset: wherever AnchorA is when the joint is made. */
	TOptional<FVector> AnchorB;
	/** Hinge: the direction EntityA turns about, in its own space (its rotation). Never zero. */
	FVector Axis = FVector(0.0, 0.0, 1.0);
	/** Hinge: whether MinAngle..MaxAngle limit the turn; false turns freely. */
	bool bLimited = false;
	/** Hinge: degrees from the pose as made, -FMantlePlaceLimits::MaxHingeAngle <= MinAngle <= 0 <= MaxAngle <= MaxHingeAngle. */
	double MinAngle = 0.0;
	double MaxAngle = 0.0;
	/** Rope: the most the anchors may be apart, cm. Unset: how far apart they are when the joint is made. */
	TOptional<double> Length;
	/** Newtons; unset never breaks. */
	TOptional<double> BreakForce;
};

/**
 * A reflection probe (#628): Shape's volume, which only the keys of its own shape fill (the parser refuses the
 * others). The entity's transform places the centre and, for a box, turns it; its scale plays no part.
 */
struct FMantleReflectionProbeProps
{
	EMantleReflectionProbeShape Shape = EMantleReflectionProbeShape::Box;
	/** Box: half the box on each axis, cm, in the entity's own axes; each FMantlePlaceLimits::MinReflectionProbeExtent to MaxReflectionProbeExtent. */
	FVector HalfExtent = FVector(1000.0, 1000.0, 400.0);
	/** Sphere: the radius, cm, MinReflectionProbeRadius to MaxReflectionProbeRadius. */
	double InfluenceRadius = 1000.0;
	/** Box: cm inward from each face over which the probe blends out, MinReflectionProbeTransition to MaxReflectionProbeTransition. */
	double TransitionDistance = 100.0;
	/** 0 to FMantlePlaceLimits::MaxReflectionProbeBrightness. */
	double Brightness = 1.0;
	/** How much sky light leaks into the capture where the sky is not otherwise visible, 0 to 1 (the engine's RuntimeSkylightScale on each channel). */
	double SkylightScale = 0.1;
};

/**
 * World text (GhzGarage/mantle#724), drawn in Mantle's built-in font and material. The entity's transform places the text
 * and turns it; its scale plays no part (WorldSize, XScale and YScale size it).
 */
struct FMantleTextProps
{
	/** UTF-8, 1 to FMantlePlaceLimits::MaxTextLength code points; a line break is "\n". */
	FString Text;
	/** Linear RGBA, each channel 0 to 1. */
	FLinearColor Color = FLinearColor::White;
	/** The glyph height, cm, FMantlePlaceLimits::MinTextWorldSize to MaxTextWorldSize. */
	double WorldSize = 30.0;
	EMantleTextHorizontalAlignment HorizontalAlignment = EMantleTextHorizontalAlignment::Left;
	EMantleTextVerticalAlignment VerticalAlignment = EMantleTextVerticalAlignment::Top;
	/** Each FMantlePlaceLimits::MinTextScale to MaxTextScale. */
	double XScale = 1.0;
	double YScale = 1.0;
};

/** One point of a WaterBody's spline (#997), in the body's own space, cm. */
struct FMantleWaterSplinePoint
{
	FVector Location = FVector::ZeroVector;
	FVector ArriveTangent = FVector::ZeroVector;
	FVector LeaveTangent = FVector::ZeroVector;
	EMantleWaterSplinePointType Interp = EMantleWaterSplinePointType::Curve;
	/** FMantlePlaceLimits::MinWaterSplineDepth to MaxWaterSplineDepth; every body type has it. */
	double Depth = 0.0;
	/** A river's only: FMantlePlaceLimits::MinWaterRiverWidth to MaxWaterRiverWidth. */
	double Width = 0.0;
	/** A river's only: 0 to FMantlePlaceLimits::MaxWaterRiverVelocity, cm/s. */
	double Velocity = 0.0;
};

/** One final Gerstner wave of a WaterBody (#997): as generated, not generator settings. */
struct FMantleWaterWave
{
	/** FMantlePlaceLimits::MinWaterWavelength to MaxWaterWavelength, cm. */
	double Wavelength = 0.0;
	/** 0 to FMantlePlaceLimits::MaxWaterAmplitude, cm. */
	double Amplitude = 0.0;
	/** 0 to 1. */
	double Steepness = 0.0;
	/** Travel direction in world space, -180 to 180 degrees. */
	double DirectionDeg = 0.0;
	double PhaseOffset = 0.0;
};

/**
 * A body of the Water plugin (GhzGarage/mantle#997). The entity's transform is the body actor's; lengths are cm.
 * Lake, River and Ocean carry Spline (a lake's and an ocean's a closed loop, a river's open); Custom carries MeshId.
 */
struct FMantleWaterBodyProps
{
	EMantleWaterBodyType BodyType = EMantleWaterBodyType::Lake;
	/** Lake, River and Ocean: the spline's points, in the body's own space. Empty for Custom. */
	TArray<FMantleWaterSplinePoint> SplinePoints;
	/** Custom: the uploaded StaticMesh asset standing for the surface. Empty otherwise. */
	FString MeshId;
	/** The final Gerstner waves, direction in world space; empty means flat water. Never on a river. */
	TArray<FMantleWaterWave> Waves;
	/** Linear RGB; each is set only when the file gives it, else the plugin's default stands. */
	TOptional<FLinearColor> Albedo;
	TOptional<FLinearColor> Scattering;
	TOptional<FLinearColor> Absorption;
	bool bCollision = true;
	/** FMantlePlaceLimits::MaxWaterCollisionShift either way, cm; set only when the file gives it. */
	TOptional<double> CollisionHeightOffset;
	/** Ocean only, each axis FMantlePlaceLimits::MinWaterExtent to MaxWaterExtent cm; set only when the file gives them. */
	TOptional<FVector> CollisionExtent;
	TOptional<FVector2D> OceanExtent;
	TOptional<bool> CenterOnZone;
};

using FMantleEntityProps = TVariant<
	FMantleStaticMeshProps,
	FMantleEmptyProps,
	FMantlePointLightProps,
	FMantleSpotLightProps,
	FMantleDirectionalLightProps,
	FMantleTriggerVolumeProps,
	FMantleSoundEmitterProps,
	FMantleInstancedMeshProps,
	FMantleCrowdSpawnerProps,
	FMantleRectLightProps,
	FMantleDecalProps,
	FMantleSkeletalMeshProps,
	FMantleTerrainProps,
	FMantleVehicleSpawnProps,
	FMantleEffectProps,
	FMantleConstraintProps,
	FMantleScatterParams,
	FMantleReflectionProbeProps,
	FMantleWeaponSpawnProps,
	FMantleScriptPointProps,
	FMantleTextProps,
	FMantleWaterBodyProps>;

/** What happens to an interaction option once it has been used (the schema's afterUse). */
enum class EMantleInteractableAfterUse : uint8
{
	None,
	DisableUntilEnabled,
	Cooldown,
	OneShot
};

/** How an interactable's prompt is laid out (the schema's interactionDisplay.style). */
enum class EMantleInteractablePromptStyle : uint8
{
	Default,
	Balanced,
	Informative
};

/** One thing a player can do to an interactable (the schema's interactionOption). */
struct FMantleInteractableOptionSpec
{
	/** "primary" or "secondary": which interact input the option uses. */
	bool bSecondaryKey = false;
	FString Text;
	double HoldSeconds = 0.0;
	int32 PressCount = 0;
	double PressDrainPerSecond = 0.4;
	EMantleInteractableAfterUse AfterUse = EMantleInteractableAfterUse::None;
	double CooldownSeconds = 0.0;
};

/** How an interactable's prompt looks (the schema's interactionDisplay). */
struct FMantleInteractableDisplaySpec
{
	EMantleInteractablePromptStyle Style = EMantleInteractablePromptStyle::Default;
	FString Title;
	FString Description;
	FLinearColor AccentColor = FLinearColor(0.95f, 0.62f, 0.2f);
};

/** A separately targetable bit of an interactable (the schema's interactionPart). */
struct FMantleInteractablePartSpec
{
	FString Id;
	/** Entity ids; empty means none. The builder's actors for them are resolved when the place is retained. */
	FString AnchorEntity;
	TArray<FString> HighlightEntities;
	FVector PromptOffset = FVector::ZeroVector;
	bool bOutline = true;
	bool bHasOutlineColor = false;
	FLinearColor OutlineColor = FLinearColor(1.0f, 0.82f, 0.25f);
	TArray<FMantleInteractableOptionSpec> Options;
	bool bHasDisplay = false;
	FMantleInteractableDisplaySpec Display;
	bool bIgnoreObstacles = false;
};

/** An entity's interactable block (the schema's interactable). */
struct FMantleInteractableSpec
{
	double Range = 250.0;
	TArray<FMantleInteractableOptionSpec> Options;
	FMantleInteractableDisplaySpec Display;
	TArray<FMantleInteractablePartSpec> Parts;
};

struct FMantleEntity
{
	FString Id;
	EMantleEntityType Type = EMantleEntityType::Group;
	FString Name;
	/** Empty means no parent. */
	FString Parent;
	FTransform Transform = FTransform::Identity;
	FMantleEntityProps Props;
	TArray<FString> Tags;

	/**
	 * When true, this entity's actor is exempt from FMantlePlaceBuilder's freeze-to-Static pass
	 * and stays Movable after reconstruction. Default false: the static world is what makes
	 * instant, per-place-cook-free publishing possible, so an entity opts into being moved by a
	 * script rather than the builder assuming it might be.
	 */
	bool bDynamic = false;

	/**
	 * Set when the entity can be walked up to and acted on with a key press. Not part of the entity's
	 * type: the game side attaches the interactable component to the entity's actor once the place is
	 * retained, on the server and on every client alike.
	 */
	TOptional<FMantleInteractableSpec> Interactable;
};

/**
 * The mesh, materials, collision and shadow of a StaticMesh or an InstancedMesh entity (#467),
 * for everything that treats the two alike: asset resolution and download, the editor preview's
 * upload check. Null for every other type.
 */
inline const FMantleStaticMeshProps* MantleMeshPropsOf(const FMantleEntity& Entity)
{
	if (const FMantleStaticMeshProps* Mesh = Entity.Props.TryGet<FMantleStaticMeshProps>())
	{
		return Mesh;
	}
	if (const FMantleInstancedMeshProps* Instanced = Entity.Props.TryGet<FMantleInstancedMeshProps>())
	{
		return &Instanced->Mesh;
	}
	return nullptr;
}

/** A StaticMesh entity's physics block (#456), or null when it has none (and for every other type). */
inline const FMantlePhysicsProps* MantlePhysicsOf(const FMantleEntity& Entity)
{
	const FMantleStaticMeshProps* Mesh = Entity.Props.TryGet<FMantleStaticMeshProps>();
	return Mesh && Mesh->Physics.IsSet() ? &Mesh->Physics.GetValue() : nullptr;
}

/**
 * The scripting API version a place with any scripts was exported against, from
 * mantle_api.json's own scriptApi field. Required whenever FMantlePlace::Scripts is non-empty.
 */
struct FMantleScriptApiVersion
{
	int32 Major = 0;
	int32 Minor = 0;
};

struct FMantlePlace
{
	int32 SchemaVersion = 0;
	FString PlaceId;
	FMantleRevision Revision;
	FMantleMetadata Metadata;
	FMantleSettings Settings;
	TArray<FMantleAssetRef> Assets;
	TArray<FMantleEntity> Entities;
	TArray<FMantleScript> Scripts;
	FMantleScriptApiVersion ScriptApi;
	/** uiDocuments (#549): at most FMantlePlaceLimits::MaxUIDocuments documents and MaxUIImages images (#556 item 5). */
	TArray<FMantleUIDocument> UIDocuments;
	/** storage (#614 stage 4): at most FMantlePlaceLimits::MaxStorageEntries library items, names unique. */
	TArray<FMantleStorageEntry> Storage;
	/** seats (#142): at most FMantlePlaceLimits::MaxSeatsPerPlace sit spots, each on an entity of the place. */
	TArray<FMantleSeat> Seats;
	/**
	 * Not in the place file: what the storage resolve answered for Storage, kept by FMantleStorage::Apply (#614
	 * stage 5). One entry per distinct item version that resolved to a usable vehicle or weapon, or (#838) a script
	 * package; an entry the
	 * backend did not know, or that was not active or delisted, is absent. Empty until the loader has expanded
	 * the place. Scripts are authorized against it (FMantleStorage::FindAuthorized), never against the raw file.
	 */
	TArray<FMantleResolvedStorageEntry> StorageResolved;

	/**
	 * Not in the place file (#614 stage 7): the declared storage items whose pinned version the resolve answered as
	 * "taken_down", a moderator's removal after a copyright complaint. A takedown overrides the place's pin: these are never
	 * in StorageResolved, so nothing spawns or grants them, and FMantleStorage::WhyNotAuthorized says why. Set by Apply.
	 */
	TArray<FString> StorageTakenDown;

	/**
	 * Not in the place file (#679 B3): one plain-language line for each stored VehicleSpawn that Apply turned off because its
	 * item did not resolve (missing, taken down, not a vehicle, or no answer), so the server can show a creator why no car is
	 * parked there instead of leaving a warning in the log. Set by Apply.
	 */
	TArray<FString> StorageSpawnProblems;
	FMantleLocalization Localization;
	FMantleAudience Audience;
};

/** A single validation failure, located by a JSON-pointer-style path. */
struct FMantleParseError
{
	FString Path;
	FString Message;

	FString ToString() const
	{
		return FString::Printf(TEXT("%s: %s"), *Path, *Message);
	}
};

struct FMantleParseResult
{
	bool bSuccess = false;
	FMantlePlace Place;
	TArray<FMantleParseError> Errors;

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
