// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "MantleShadowDistance.generated.h"

class UPrimitiveComponent;

/**
 * Stops far place meshes casting shadows (#630 stage 2). The engine has no per-primitive shadow distance: a mesh's
 * Max Draw Distance ends its shadow where it ends the mesh, so clutter drawn to 60 m shadows to 60 m too. The place
 * builder hands this every StaticMesh and InstancedMesh that has a drawDistance and casts shadows; a few times a
 * second it turns off the shadow of each one whose bounds are past ShadowDistanceFor(its draw distance) from the
 * local view, and back on when the view comes within nine tenths of that (so one standing at the edge does not flip).
 * A physics entity is never handed in: its prop copies the placeholder's shadow when it takes over.
 *
 * Only on a machine that draws, in a Game or PIE world: never a dedicated server, never an editor world a creator
 * saves. Nothing replicates; each client decides from its own view.
 *
 * Console variable: mantle.Place.ShadowDistanceFraction (0.5): the share of a mesh's draw distance it casts shadows
 * to. 1 or more, or 0 or less, leaves every shadow on (the draw distance still ends it).
 */
UCLASS()
class MANTLERUNTIME_API UMantleShadowDistanceSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	/** How far from the view a mesh drawn to DrawDistance cm casts shadows, under the console variable; 0 when that leaves shadows on. */
	static double ShadowDistanceFor(double DrawDistance);

	/** Starts deciding Component's shadow from its distance to the view. Component casts shadows and DrawDistance is above 0. */
	void Track(UPrimitiveComponent* Component, double DrawDistance);

	/** Components being decided, the gone ones included until a pass drops them. */
	int32 NumTracked() const { return Entries.Num(); }

	//~ Begin UWorldSubsystem / FTickableGameObject
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Tick(float DeltaTime) override;
	virtual bool IsTickable() const override;
	virtual TStatId GetStatId() const override
	{
		RETURN_QUICK_DECLARE_CYCLE_STAT(UMantleShadowDistanceSubsystem, STATGROUP_Tickables);
	}
	//~ End UWorldSubsystem / FTickableGameObject

private:
	struct FEntry
	{
		TWeakObjectPtr<UPrimitiveComponent> Component;
		double DrawDistance = 0.0;
		/** What this last set; every tracked component starts casting. */
		bool bShadow = true;
	};

	TArray<FEntry> Entries;

	/** The next entry a tick looks at: each tick goes on from where the last stopped. */
	int32 Cursor = 0;

	/** When the last pass over every entry started, seconds; a new pass waits ScanIntervalSeconds after it. */
	double PassStartSeconds = 0.0;
};
