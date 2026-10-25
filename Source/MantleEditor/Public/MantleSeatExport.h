// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantlePlaceTypes.h"

class AActor;
class UMantleSeatSpotComponent;
class UWorld;
struct FMantleValidationResult;

/**
 * Mantle Seats (GhzGarage/MantleCore#142): what Check this place says about them and what Export writes. A seat is a
 * UMantleSeatSpotComponent on a prop; it publishes as an entry of the place's seats, owned by the prop's entity.
 *
 * The shared exporter and validator files only call this.
 */
class MANTLEEDITOR_API FMantleSeatExport
{
public:
	/** Lowest and highest a hip may be above the floor under it, in centimetres: a low ledge to a tall stool. */
	static constexpr double MinHipHeight = 15.0;
	static constexpr double MaxHipHeight = 110.0;

	/** The seat components of Actor, in a stable order. */
	static void Collect(const AActor* Actor, TArray<const UMantleSeatSpotComponent*>& Out);

	/** Whether Actor is published as an entity of its own, which a seat on it needs (its seats ride on that entity). */
	static bool OwnerPublishes(const AActor* Actor);

	/** Why Actor cannot carry seats (it simulates physics or can move), worded to follow "has a Mantle Seat but", or empty. */
	static FString OwnerProblem(const AActor* Actor);

	/**
	 * The seat as the place file holds it: the hip in the owner's local space and the yaw relative to the owner's.
	 * OwnerWorld is the owning entity's world transform, SeatWorld the component's.
	 */
	static FMantleSeat ToPlaceSeat(const FString& EntityId, const FTransform& OwnerWorld, const FTransform& SeatWorld, EMantleSeatStyle Style);

	/** The style a component asks for, as the place format names it. */
	static EMantleSeatStyle StyleOf(const UMantleSeatSpotComponent* Seat);

	/**
	 * The distance from the hip straight down to the first surface that is not the owner, or false when there is none
	 * within MaxHipHeight plus a margin. Seat height is fitted from this floor at run time.
	 */
	static bool FloorBelow(const UMantleSeatSpotComponent* Seat, double& OutHipHeight);

	/** The error for a hip at HipHeight above the floor (bFound false: no floor), or empty when it is sensible. */
	static FString HeightProblem(bool bFound, double HipHeight);

	/** Check this place's rules for every seat in World: the cap, the owner, and the floor. */
	static void Check(UWorld* World, FMantleValidationResult& Result);

	/**
	 * Export: one seat per seat component of each exported actor, in Exportable's order. EntityIdByActor names the
	 * entity each rides on. Errors past the cap.
	 */
	static void Export(const TArray<const AActor*>& Exportable, const TMap<const AActor*, FString>& EntityIdByActor,
		TArray<FMantleSeat>& OutSeats, TArray<FMantleParseError>& OutErrors);
};
