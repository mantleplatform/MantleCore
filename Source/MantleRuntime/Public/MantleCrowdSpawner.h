// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Components/SplineComponent.h"
#include "GameFramework/Actor.h"
#include "MantlePlaceTypes.h"

#include "MantleCrowdSpawner.generated.h"

/**
 * One lane of a Mantle Crowd Spawner (#477): the path its ambient NPCs walk. A creator shapes it
 * like any spline; export writes its points in the spawner's local space. Straight (linear)
 * points are written as they are; a lane with any curved point is sampled along its length.
 */
UCLASS(ClassGroup = Mantle, meta = (BlueprintSpawnableComponent, DisplayName = "Mantle Crowd Lane"))
class MANTLERUNTIME_API UMantleCrowdLaneComponent : public USplineComponent
{
	GENERATED_BODY()

public:
	UMantleCrowdLaneComponent(const FObjectInitializer& ObjectInitializer);

	/** How wide the walkable band is, cm. Each NPC keeps a seeded sideways offset inside it. */
	UPROPERTY(EditAnywhere, Category = "Crowd Lane", meta = (ClampMin = "0", ClampMax = "2000", UIMin = "0", UIMax = "2000", Units = "cm"))
	double Width = 200.0;

	/** NPCs walk the lane both ways and turn round at its ends. Off: start to end only. */
	UPROPERTY(EditAnywhere, Category = "Crowd Lane")
	bool bTwoWay = true;
};

/** One worn item of a crowd look: avatar descriptor v2's wearing entry. */
USTRUCT()
struct MANTLERUNTIME_API FMantleCrowdWornSettings
{
	GENERATED_BODY()

	/**
	 * A wear or accessory slot: hair, eyebrows, eyelashes, facial_hair, top, bottom, footwear,
	 * acc_hat, acc_face, acc_neck, acc_shoulder_l, acc_shoulder_r, acc_front, acc_back or
	 * acc_waist. One item per slot.
	 */
	UPROPERTY(EditAnywhere, Category = "Crowd")
	FString Slot;

	/** The catalog item worn there: itm_u followed by 32 lowercase hex digits. */
	UPROPERTY(EditAnywhere, Category = "Crowd")
	FString ItemId;
};

/** One look a crowd NPC may wear. Empty tone or eye colour, or a zero height, is picked by the seed. */
USTRUCT()
struct MANTLERUNTIME_API FMantleCrowdLookSettings
{
	GENERATED_BODY()

	/** The body's catalog item: itm_u followed by 32 lowercase hex digits. */
	UPROPERTY(EditAnywhere, Category = "Crowd")
	FString AvatarItemId;

	/** tone_ and two digits, or empty for the seed to pick. */
	UPROPERTY(EditAnywhere, Category = "Crowd")
	FString SkinToneId;

	/** eye_ and two digits, or empty for the seed to pick. */
	UPROPERTY(EditAnywhere, Category = "Crowd")
	FString EyeColorId;

	/** 95 to 105, or 0 for the seed to pick. */
	UPROPERTY(EditAnywhere, Category = "Crowd", meta = (ClampMin = "0", ClampMax = "105", UIMin = "0", UIMax = "105"))
	int32 HeightPct = 0;

	UPROPERTY(EditAnywhere, Category = "Crowd", meta = (TitleProperty = "Slot"))
	TArray<FMantleCrowdWornSettings> Wearing;
};

/**
 * A place the spawner's NPCs walk over to and linger at (#513): another actor of the level, such as
 * a stall or a fountain. An NPC passing the lane point nearest to it (within 20 m) may leave its
 * lane, stand round it facing it for a while, then walk back.
 */
USTRUCT()
struct MANTLERUNTIME_API FMantleCrowdPointOfInterestSettings
{
	GENERATED_BODY()

	/** The actor NPCs gather round: any other actor in this level that publishes. */
	UPROPERTY(EditAnywhere, Category = "Crowd")
	TSoftObjectPtr<AActor> Actor;

	/**
	 * The entity it names in a place file, kept when the spawner was made from one (the importer,
	 * the place builder). Export names Actor's entity instead whenever Actor is set.
	 */
	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "Crowd")
	FString EntityId;

	/** Chance an NPC passing it stops by, 0 to 1. */
	UPROPERTY(EditAnywhere, Category = "Crowd", meta = (ClampMin = "0", ClampMax = "1", UIMin = "0", UIMax = "1"))
	double Chance = 0.5;

	/** Shortest stay, seconds. */
	UPROPERTY(EditAnywhere, Category = "Crowd", meta = (ClampMin = "1", ClampMax = "120", UIMin = "1", UIMax = "120", Units = "s"))
	double LingerSecondsMin = 4.0;

	/** Longest stay, seconds. */
	UPROPERTY(EditAnywhere, Category = "Crowd", meta = (ClampMin = "1", ClampMax = "120", UIMin = "1", UIMax = "120", Units = "s"))
	double LingerSecondsMax = 10.0;

	/** How far from the actor NPCs stand, cm. */
	UPROPERTY(EditAnywhere, Category = "Crowd", meta = (ClampMin = "0", ClampMax = "1000", UIMin = "0", UIMax = "1000", Units = "cm"))
	double Radius = 150.0;
};

/**
 * Ambient pedestrians (#477): the CrowdSpawner place entity. It only holds and shows data. Every
 * client derives the same NPCs from Seed plus server time (MantleNPC, reading the retained
 * FMantlePlace), a dedicated server spawns none, and neither this actor nor its NPCs replicate.
 *
 * Every UMantleCrowdLaneComponent on the actor is a lane, ordered by component name. A spawner
 * starts with one lane; add more with Add Component > Mantle Crowd Lane.
 */
UCLASS(ClassGroup = Mantle, meta = (DisplayName = "Mantle Crowd Spawner"),
	hidecategories = (Advanced, Cooking, HLOD, Input, LOD, Networking, Physics, Replication, Collision))
class MANTLERUNTIME_API AMantleCrowdSpawner : public AActor
{
	GENERATED_BODY()

public:
	AMantleCrowdSpawner();

	/** How many NPCs walk this spawner's lanes, 1 to 100. A place holds at most 200 in all. */
	UPROPERTY(EditAnywhere, Category = "Crowd", meta = (ClampMin = "1", ClampMax = "100", UIMin = "1", UIMax = "100"))
	int32 Count = 20;

	/** Picks every NPC's look, lane, offset and pace. The same seed gives the same crowd. */
	UPROPERTY(EditAnywhere, Category = "Crowd", meta = (ClampMin = "0", UIMin = "0"))
	int32 Seed = 0;

	/** The looks NPCs are drawn from, at most 16. Empty means the two base bodies with no garments. */
	UPROPERTY(EditAnywhere, Category = "Crowd", meta = (TitleProperty = "AvatarItemId"))
	TArray<FMantleCrowdLookSettings> Looks;

	/** Shortest height, percent, for looks that leave height to the seed. */
	UPROPERTY(EditAnywhere, Category = "Crowd|Variation", meta = (ClampMin = "95", ClampMax = "105", UIMin = "95", UIMax = "105"))
	int32 MinHeightPct = 95;

	/** Tallest height, percent, for looks that leave height to the seed. */
	UPROPERTY(EditAnywhere, Category = "Crowd|Variation", meta = (ClampMin = "95", ClampMax = "105", UIMin = "95", UIMax = "105"))
	int32 MaxHeightPct = 105;

	/** Skin tones (tone_ and two digits) for looks that leave the tone to the seed. Empty means every tone. */
	UPROPERTY(EditAnywhere, Category = "Crowd|Variation")
	TArray<FString> SkinTones;

	/** Slowest walking speed, cm/s. */
	UPROPERTY(EditAnywhere, Category = "Crowd|Movement", meta = (ClampMin = "50", ClampMax = "300", UIMin = "50", UIMax = "300"))
	double WalkSpeedMin = 110.0;

	/** Fastest walking speed, cm/s. */
	UPROPERTY(EditAnywhere, Category = "Crowd|Movement", meta = (ClampMin = "50", ClampMax = "300", UIMin = "50", UIMax = "300"))
	double WalkSpeedMax = 160.0;

	/** Chance an NPC stops for a while at each lane point it passes, 0 to 1. */
	UPROPERTY(EditAnywhere, Category = "Crowd|Movement", meta = (ClampMin = "0", ClampMax = "1", UIMin = "0", UIMax = "1"))
	double IdleChance = 0.15;

	/** Where NPCs gather (#513), at most 16: stalls, fountains, noticeboards. */
	UPROPERTY(EditAnywhere, Category = "Crowd|Movement")
	TArray<FMantleCrowdPointOfInterestSettings> PointsOfInterest;

	/** Off keeps the spawner in the place without any NPCs. */
	UPROPERTY(EditAnywhere, Category = "Crowd")
	bool bEnabled = true;

	/** A curved lane is sampled at this spacing, cm, or wider when that would pass the point cap. */
	static constexpr double LaneSampleSpacing = 200.0;

	/**
	 * Reads the actor into CrowdSpawner properties: lanes from every UMantleCrowdLaneComponent in
	 * name order, points in this actor's local space (a closed loop repeats its first point; a
	 * lane with any non-linear point is sampled every LaneSampleSpacing cm, at most
	 * FMantlePlaceLimits::MaxCrowdLanePoints points). Points of interest (#513) come out in
	 * PointsOfInterest's order, each with its setting's EntityId: an actor has no entity id until
	 * the exporter gives it one (FMantlePlaceExporter::ResolveCrowdPoints). Out is filled either way. Returns false,
	 * with OutProblem phrased to follow the actor's name and saying what to fix, when a value is
	 * outside the place schema's limits.
	 */
	bool ReadProps(FMantleCrowdSpawnerProps& Out, FString& OutProblem) const;

	/**
	 * Sets the actor from CrowdSpawner properties, rebuilding the lanes as linear splines in
	 * local space (a lane whose last point repeats its first becomes a closed loop). For the
	 * place builder and the editor's importer: ReadProps of the result gives In back.
	 */
	void ApplyProps(const FMantleCrowdSpawnerProps& In);

	/** The lane components in the order ReadProps writes them. */
	TArray<UMantleCrowdLaneComponent*> GetLanes() const;

private:
	/** The lane every spawner starts with. */
	UPROPERTY(VisibleAnywhere, Category = "Crowd")
	TObjectPtr<UMantleCrowdLaneComponent> DefaultLane;
};
