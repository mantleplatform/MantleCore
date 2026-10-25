// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Info.h"
#include "Materials/MaterialInterface.h"
#include "MantleThumbnailChoice.h"
#include "UObject/SoftObjectPtr.h"

#include "MantlePlaceSettingsActor.generated.h"

struct FMantleSettings;
struct FMantleAudience;
struct FMantleEnvironmentSettings;
struct FMantleStorageEntry;

/** How a player who dies is placed back into the world (the place file's settings.respawnMode). */
UENUM()
enum class EMantleRespawnMode : uint8
{
	/** The Player Start closest to where the player died. */
	Nearest,
	/** Any Player Start, picked at random. */
	Random,
	/** Always the first Player Start. */
	First,
};

/**
 * A place's maturity rating (#320), the place file's audience.maturity. In EMantleMaturity's
 * order. A placeholder until #42 decides the real rating taxonomy.
 */
UENUM()
enum class EMantlePlaceMaturity : uint8
{
	/** Not rated. Treated as the most restrictive rating once ratings are enforced. */
	Unrated,
	Everyone,
	Teen,
	Mature,
};

/** A platform a place may be offered on (#320). In EMantlePlatform's order; a bit in Platforms. */
UENUM(meta = (Bitflags, UseEnumValuesAsMaskValuesInEditor = "false"))
enum class EMantlePlacePlatform : uint8
{
	PC,
	PlayStation,
	Xbox,
	Switch,
	IOS UMETA(DisplayName = "iOS"),
	Android,
};

/**
 * The weather a place starts in (#532), the place file's settings.environment.weather. LevelSky and
 * Natural both leave the key out; the rest are EMantleWeatherState's members, in its order, two steps up.
 */
UENUM()
enum class EMantlePlaceWeather : uint8
{
	/**
	 * The level's own look: a level with a sky of its own (clouds, an atmosphere, a sky light or a sun) is held at
	 * the weather its clouds show, or Clear; one without changes on its own.
	 */
	LevelSky UMETA(DisplayName = "Level's Sky"),
	/** The natural weather: mostly clear, changing on its own (even on a level with its own sky). */
	Natural,
	Clear,
	PartlyCloudy UMETA(DisplayName = "Partly Cloudy"),
	Overcast,
	Rain,
	Storm,
	Cloudy,
	Foggy,
	LightRain UMETA(DisplayName = "Light Rain"),
	LightSnow UMETA(DisplayName = "Light Snow"),
	Snow,
	Blizzard,
	DustCalm UMETA(DisplayName = "Dust (Calm)"),
	DustStorm UMETA(DisplayName = "Dust Storm"),
};

/**
 * Whether the level's sun lights the place (#722), the place file's settings.environment.sun.direct. Automatic and
 * Always both leave the key out; Never writes false.
 */
UENUM()
enum class EMantlePlaceDirectSun : uint8
{
	/** The export decides: no direct sun when no spawn point is in the sun's light (a sealed room), the level's sun otherwise. */
	Automatic,
	/** The level's sun lights the place, whatever the export finds. */
	Always,
	/** No direct sun: only the sky lights the place. */
	Never,
};

/**
 * The colour-grading preset a place starts in (#532), the place file's
 * settings.environment.colorGrading. None is the key left out; the rest are Environment.ColorGrading's
 * presets, numbered as MantleEnvironmentOverrideValues numbers them.
 */
UENUM()
enum class EMantlePlaceColorGrading : uint8
{
	/** No preset: the game's own colour grading. */
	None,
	Warm,
	Cool,
	Noir,
	Sepia,
	Vivid,
	Faded,
};

/**
 * What one landscape paint layer looks like in Mantle (#466). A landscape's own material blends its
 * layers in ways a player's client cannot rebuild, so each painted layer is published as one place
 * material, flattened and uploaded like any mesh's, repeated across the terrain every TilingCm.
 */
USTRUCT()
struct MANTLEEDITOR_API FMantleTerrainLayerSetting
{
	GENERATED_BODY()

	/** The paint layer's name, as the landscape's Paint tab (its layer info) shows it. */
	UPROPERTY(EditAnywhere, Category = "Mantle")
	FName Layer;

	/** The material the layer is drawn with in Mantle: its base colour, normal and roughness maps and values. */
	UPROPERTY(EditAnywhere, Category = "Mantle")
	TObjectPtr<UMaterialInterface> Material;

	/** How big one repeat of the material's textures is on the ground, in cm. */
	UPROPERTY(EditAnywhere, Category = "Mantle", meta = (ClampMin = "1", ClampMax = "100000", UIMin = "50", UIMax = "5000"))
	float TilingCm = 400.0f;
};

/**
 * One library item a place holds (#614 stage 4): a name the place's VehicleSpawns use, and the item
 * version it pins. Published as the place file's storage[]. Tools > Mantle > Library (stage 6) fills
 * it; the Details panel stays a plain way to rename or remove an entry.
 */
USTRUCT()
struct MANTLEEDITOR_API FMantleStorageSetting
{
	GENERATED_BODY()

	/** The name a VehicleSpawn's Storage Item uses: a letter, then up to 63 letters, digits or underscores. Unique in the place. */
	UPROPERTY(EditAnywhere, Category = "Mantle")
	FString Name;

	/** The library item's id: itm_u and 32 lowercase hex digits. */
	UPROPERTY(EditAnywhere, Category = "Mantle")
	FString Item;

	/** The item version the place is pinned to. Publishing a newer version never changes it. */
	UPROPERTY(EditAnywhere, Category = "Mantle", meta = (ClampMin = "1"))
	int32 Version = 1;

	/**
	 * What the item is (#838): "vehicle", "weapon" or "script", recorded when Tools > Mantle > Library or a publish added the
	 * entry. Empty for an entry typed in by hand: Mantle checks its kind when the place is published.
	 */
	UPROPERTY(VisibleAnywhere, Category = "Mantle")
	FString Kind;

	/**
	 * A script package's manifest as JSON (#838), {name, description, parameters}, when the Library panel or a publish knew it:
	 * a Script Point's Details panel shows its parameters from it. Not shown, not published.
	 */
	UPROPERTY()
	FString Manifest;

	/**
	 * A script package's settings (#838 stage 6), set once for the whole package: setting name to value, every value text whatever
	 * its type. Published as the storage entry's settings. With a known manifest the Details panel draws one typed row per declared
	 * setting (FMantleStorageSettingCustomization) instead of this map.
	 */
	UPROPERTY(EditAnywhere, Category = "Mantle")
	TMap<FString, FString> Settings;
};

/**
 * A level's Mantle place settings (#266). One per level: "Tools > Mantle > Place settings"
 * finds or adds it, and the exporter reads it into the place file's settings. A level
 * without one publishes the defaults.
 *
 * An AInfo, so the exporter treats it as infrastructure rather than an entity, and it needs
 * no change to the project's World Settings class.
 */
UCLASS(placeable, meta = (DisplayName = "Mantle Place Settings"),
	hidecategories = (Actor, Advanced, Cooking, DataLayers, HLOD, Input, LevelInstance, Networking, Physics, Replication, WorldPartition))
class MANTLEEDITOR_API AMantlePlaceSettingsActor : public AInfo
{
	GENERATED_BODY()

public:
	/** How many players one server holds. */
	UPROPERTY(EditAnywhere, Category = "Mantle", meta = (ClampMin = "1", ClampMax = "100", UIMin = "1", UIMax = "100"))
	int32 MaxPlayers = 16;

	/**
	 * How strongly everything falls, in cm/s^2, pulling down: 980 is Earth's gravity, 490 half
	 * of it, 0 none. Applied to the world on the server and every client (#453).
	 */
	UPROPERTY(EditAnywhere, Category = "Mantle", meta = (UIMin = "0", UIMax = "4000"))
	double Gravity = 980.0;

	/** Where a player who dies comes back. */
	UPROPERTY(EditAnywhere, Category = "Mantle")
	EMantleRespawnMode RespawnMode = EMantleRespawnMode::Nearest;

	/**
	 * Let other players host community servers for this place. Off by default: only you can
	 * host it. Places that turn this on need a Mantle client from after #178 to load.
	 */
	UPROPERTY(EditAnywhere, Category = "Mantle")
	bool bAllowCommunityServers = false;

	/**
	 * Let other players create private servers for this place, to play it with the people they
	 * invite. Off by default: only you can. Places that turn this on need a Mantle client from
	 * after #416 to load.
	 */
	UPROPERTY(EditAnywhere, Category = "Mantle")
	bool bAllowPrivateServers = false;

	/**
	 * Let players pull each other out of this place's cars: the vehicle key at an occupied seat
	 * takes it, throwing its occupant out. On by default. Places that turn this off need a Mantle
	 * client from after #546 to load.
	 */
	UPROPERTY(EditAnywhere, Category = "Mantle")
	bool bCarjacking = true;

	// The place's default look (#532): what every player starts in, with or without a script. A
	// server script's Environment writes change it from there. Only values changed from these
	// defaults are published; a place that changes any needs a Mantle client from after #532 to load.

	/** Start the clock at Clock Time instead of the default morning. */
	UPROPERTY(EditAnywhere, Category = "Mantle|Environment", meta = (InlineEditConditionToggle))
	bool bSetClockTime = false;

	/**
	 * The hour the clock is at when a server starts: 12 is noon, 21 night. It runs on from there at
	 * Time Scale.
	 */
	UPROPERTY(EditAnywhere, Category = "Mantle|Environment",
		meta = (EditCondition = "bSetClockTime", ClampMin = "0", ClampMax = "24", UIMin = "0", UIMax = "24"))
	double ClockTime = 12.0;

	/** How fast the clock runs: 1 is the normal 30-minute day, 0 stops it, 2 runs it twice as fast. */
	UPROPERTY(EditAnywhere, Category = "Mantle|Environment", meta = (ClampMin = "0", ClampMax = "60", UIMin = "0", UIMax = "4"))
	double TimeScale = 1.0;

	/**
	 * The weather players start in. Level's Sky (the default) holds a level with a sky of its own at the weather its
	 * clouds show, or Clear, and lets any other level change on its own; Natural lets it change on its own even then;
	 * any other choice holds it there until a script changes it.
	 * Saved by name and against the default, so an actor that never touched this loads as Level's Sky, as before.
	 */
	UPROPERTY(EditAnywhere, Category = "Mantle|Environment")
	EMantlePlaceWeather Weather = EMantlePlaceWeather::LevelSky;

	/** Set the sky light's strength with Sky Light Intensity instead of letting the export estimate it from the level's sky light. */
	UPROPERTY(EditAnywhere, Category = "Mantle|Environment", meta = (InlineEditConditionToggle))
	bool bSetSkyLightIntensity = false;

	/**
	 * How strongly Mantle's own sky light shines: 1 is its full outdoor sky, 0 none. An indoor level wants it low, or its
	 * walls are lit by a sky the level does not show. Left unset, the export estimates it from the level's sky light.
	 * Places that set it need a Mantle client that reads the sky light setting to load.
	 */
	UPROPERTY(EditAnywhere, Category = "Mantle|Environment", meta = (EditCondition = "bSetSkyLightIntensity", ClampMin = "0", ClampMax = "1", UIMin = "0", UIMax = "1"))
	double SkyLightIntensity = 1.0;

	/**
	 * Whether the level's sun lights the place. Automatic turns it off for a level whose spawn points are all in the shade
	 * of its own meshes (a sealed room), as the editor's baked lighting shows it; Always and Never decide it yourself.
	 */
	UPROPERTY(EditAnywhere, Category = "Mantle|Environment")
	EMantlePlaceDirectSun DirectSun = EMantlePlaceDirectSun::Automatic;

	/** A colour multiplied over the whole picture. White leaves it unchanged. */
	UPROPERTY(EditAnywhere, Category = "Mantle|Environment", meta = (HideAlphaChannel))
	FColor ColorTint = FColor::White;

	/** How colourful the picture is: 0 is black and white, 1 unchanged, 2 doubled. */
	UPROPERTY(EditAnywhere, Category = "Mantle|Environment", meta = (ClampMin = "0", ClampMax = "2", UIMin = "0", UIMax = "2"))
	double Saturation = 1.0;

	/** Set the bloom with Bloom instead of keeping the game's own. */
	UPROPERTY(EditAnywhere, Category = "Mantle|Environment", meta = (InlineEditConditionToggle))
	bool bSetBloom = false;

	/** How strongly bright things glow: 0 is no glow, up to 4. */
	UPROPERTY(EditAnywhere, Category = "Mantle|Environment",
		meta = (EditCondition = "bSetBloom", ClampMin = "0", ClampMax = "4", UIMin = "0", UIMax = "4"))
	double Bloom = 1.0;

	/** A colour-grading look for the whole picture. None keeps the game's own. */
	UPROPERTY(EditAnywhere, Category = "Mantle|Environment")
	EMantlePlaceColorGrading ColorGrading = EMantlePlaceColorGrading::None;

	/**
	 * How strongly the colour grading shows, this one or one a script picks: 1 is the full look, 0
	 * none of it.
	 */
	UPROPERTY(EditAnywhere, Category = "Mantle|Environment", meta = (ClampMin = "0", ClampMax = "1", UIMin = "0", UIMax = "1"))
	double ColorGradingIntensity = 1.0;

	/**
	 * Who this place is for. Carried on its catalogue card and used by the catalogue's filters;
	 * nothing restricts who can join yet. Places that set a rating or limit their platforms
	 * need a Mantle client from after #320 to load.
	 */
	UPROPERTY(EditAnywhere, Category = "Mantle|Audience")
	EMantlePlaceMaturity Maturity = EMantlePlaceMaturity::Unrated;

	/** Offer this place on every platform, including ones Mantle adds later. */
	UPROPERTY(EditAnywhere, Category = "Mantle|Audience")
	bool bAllPlatforms = true;

	/**
	 * The platforms this place may be offered on, when not all of them: for a place too heavy
	 * for phones, say. Choose at least one.
	 */
	UPROPERTY(EditAnywhere, Category = "Mantle|Audience",
		meta = (Bitmask, BitmaskEnum = "/Script/MantleEditor.EMantlePlacePlatform", EditCondition = "!bAllPlatforms"))
	int32 Platforms = 0;

	/**
	 * Meshes, materials and sounds to publish with the place though nothing in the level uses them,
	 * so its scripts can spawn or play them by id. Each is converted, checked and uploaded exactly
	 * like placed content; a mesh brings its own slot materials. Publish logs each one's id as
	 * "Script asset <path> = <id>", and lists them in Script Asset Ids below.
	 */
	UPROPERTY(EditAnywhere, Category = "Mantle|Scripts",
		meta = (AllowedClasses = "/Script/Engine.StaticMesh,/Script/Engine.MaterialInterface,/Script/Engine.SoundWave"))
	TArray<TSoftObjectPtr<UObject>> ScriptAssets;

	/**
	 * The id scripts use for each Script Asset, "<path> = <id>", from this editor session's last
	 * publish. Not saved: publish again to see them after reopening the level.
	 */
	UPROPERTY(VisibleAnywhere, Transient, Category = "Mantle|Scripts", meta = (DisplayName = "Script Asset Ids"))
	TArray<FString> PublishedScriptAssetIds;

	/**
	 * The level's landscape as players see it (#466): one entry per paint layer, naming the material it
	 * is drawn with. Up to 4 painted layers publish. A landscape with no painted layer is drawn with the
	 * first entry's material.
	 */
	UPROPERTY(EditAnywhere, Category = "Mantle|Terrain")
	TArray<FMantleTerrainLayerSetting> TerrainLayers;

	/**
	 * The library items this place holds (#614 stage 4), at most 32: vehicle spawns name one by its Name in
	 * their Storage Item. Places that store any need a Mantle client from after #614 stage 4 to load.
	 * Find and add items with Tools > Mantle > Library: it adds, updates and removes entries here as undoable steps.
	 */
	UPROPERTY(EditAnywhere, Category = "Mantle|Storage",
		meta = (ToolTip = "The library items this place holds, at most 32. Vehicle Spawns name one by its Name in their Storage Item. Find and add items with Tools > Mantle > Library."))
	TArray<FMantleStorageSetting> Storage;

	// The place's picture in the catalogue (#786): taken at every publish, from the view below or, when none is chosen,
	// from above and behind the first Player Start. Publish shows it before anything is sent.

	/**
	 * How the thumbnail is made (#124): Automatic, Choose the angle (the camera below) or a Custom image. Set it in the Publish window's Thumbnail
	 * section, the same one every Mantle uploader shows.
	 */
	UPROPERTY(EditAnywhere, Category = "Mantle|Thumbnail")
	FMantleThumbnailChoice ThumbnailChoice;

	/** Where the thumbnail's camera stands when Choose the angle is on. */
	UPROPERTY(EditAnywhere, Category = "Mantle|Thumbnail", meta = (DisplayName = "Camera Location"))
	FVector ThumbnailLocation = FVector::ZeroVector;

	/** Where it looks. */
	UPROPERTY(EditAnywhere, Category = "Mantle|Thumbnail", meta = (DisplayName = "Camera Rotation"))
	FRotator ThumbnailRotation = FRotator::ZeroRotator;

	/** How wide it sees, in degrees across. */
	UPROPERTY(EditAnywhere, Category = "Mantle|Thumbnail",
		meta = (DisplayName = "Field of View", ClampMin = "10", ClampMax = "120", UIMin = "20", UIMax = "100"))
	float ThumbnailFOV = 60.f;

	/** Take the thumbnail from where the level viewport's camera is now. */
	UFUNCTION(CallInEditor, Category = "Mantle|Thumbnail")
	void UseCurrentView();

	/** Go back to the automatic thumbnail: from above and behind the first Player Start. */
	UFUNCTION(CallInEditor, Category = "Mantle|Thumbnail")
	void UseAutomaticView();

	/** Copies the active level viewport's camera into the chosen view, as an undoable change. False when it is not a perspective viewport. */
	bool SetThumbnailFromViewport();

	/** The entry for a paint layer, or null. */
	const FMantleTerrainLayerSetting* FindTerrainLayer(FName Layer) const;

	/** The level's settings actor, or null. With more than one, the first found is used. */
	static AMantlePlaceSettingsActor* Find(UWorld* World);

	/** The level's settings actor, added (as an undoable change) when there is none. */
	static AMantlePlaceSettingsActor* FindOrAdd(UWorld* World);

	/** Copies this level's settings, if it has any, over the defaults in Out. */
	static void ApplyTo(UWorld* World, FMantleSettings& Out);

	/** Sets the level's settings from a place file's, adding the actor if needed. */
	static void SetFrom(UWorld* World, const FMantleSettings& Settings);

	/** The place's default look (#532) from the Environment fields: only what differs from the defaults is set. */
	FMantleEnvironmentSettings MakeEnvironmentSettings() const;

	/** Sets the Environment fields from a place file's settings.environment; a key it leaves out resets its field. */
	void SetEnvironmentFrom(const FMantleEnvironmentSettings& Environment);

	/** Copies this level's audience (#320), if it has a settings actor, over the defaults in Out. */
	static void ApplyTo(UWorld* World, FMantleAudience& Out);

	/** Sets the level's audience from a place file's, adding the actor if needed. */
	static void SetFrom(UWorld* World, const FMantleAudience& Audience);

	/** Appends this level's storage entries (#614 stage 4), if it has a settings actor, to Out. */
	static void ApplyStorageTo(UWorld* World, TArray<FMantleStorageEntry>& Out);

	/** Sets the level's storage from a place file's, adding the actor if needed. An entry for an item already stored keeps its Kind and Manifest. */
	static void SetStorageFrom(UWorld* World, const TArray<FMantleStorageEntry>& Storage);

	/**
	 * Replaces Storage with Entries (#838), with no transaction or change notification of its own. Each entry keeps the Kind and
	 * Manifest of the old entry for the same item; KindByItem, when it names the item, sets its Kind instead. Settings (stage 6) are
	 * each entry's own: a caller that rebuilds Entries from Storage carries them (ApplyStorageTo does).
	 */
	void ReplaceStorageKeepingKinds(const TArray<FMantleStorageEntry>& Entries, const TMap<FString, FString>& KindByItem = TMap<FString, FString>());

	/** The storage entry named exactly Name (case-sensitive), or null. */
	const FMantleStorageSetting* FindStorageEntry(const FString& Name) const;

	/** True when the audience restricts platforms but names none, which cannot be published. */
	bool HasNoPlatformSelected() const { return !bAllPlatforms && (Platforms & AllPlatformBits()) == 0; }

	/** Every bit an EMantlePlacePlatform can set in Platforms. */
	static int32 AllPlatformBits();

	static const TCHAR* RespawnModeName(EMantleRespawnMode Mode);
	static EMantleRespawnMode ParseRespawnMode(const FString& Name);
};
