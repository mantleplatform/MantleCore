// Copyright Joshua Eger. All Rights Reserved.
// The collision of a Terrain (stage 4 of large places): one Chaos height field per block of 128 squares, on every machine.
//
// A chunk of a terrain is a picture only (a mesh, no body, no collision data). The ground a body stands on, a trace hits and a character walks on is a height field made from the
// heights the machine already holds: 2 bytes a sample, no triangles, no cook, so a dedicated server (which draws nothing) builds no chunk at all. The engine's landscape does the same
// (LandscapeCollision.cpp); the component makes the same calls to make its body and shape.
//
// The shape splits every square of the grid from (x, y) to (x + 1, y + 1), as the height field does, the engine's landscape does when it draws, and navigation does when it
// reads heights; so the drawn ground (FMantleTerrainBuilder::BuildChunkDescription), the collided ground and the walked ground are the same triangles.

#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "MantleHeightfieldReader.h"

#include "MantleTerrainCollision.generated.h"

struct FMantleTerrainHoleGrid;

/** The pure part: how the samples of one block become the inputs of a Chaos height field. No engine object, any thread. */
class MANTLERUNTIME_API FMantleTerrainCollision
{
public:
	/** The height field's own sample value for a stored value is (Value - 32768): the component's Z offset is HeightOffset + this x HeightScale. */
	static constexpr int32 HeightBias = 32768;

	/** Chaos' value for a quad that is a hole: no ray, sweep or contact meets it. */
	static constexpr uint8 HoleMaterialIndex = 255;

	/** The block's samples, row-major (Y rows of X columns), from First to Last inclusive: what FHeightField takes as heights. */
	static void BlockHeights(const FMantleHeightfield& Field, const FIntPoint& First, const FIntPoint& Last, TArray<uint16>& Out);

	/**
	 * What FHeightField takes as material indices for the block: one entry {0} (the default physical material for the whole shape) when no quad of the block is a hole; otherwise one
	 * entry a quad, row-major, 0 for ground and HoleMaterialIndex for a hole. Never empty: the engine reads at least one entry on every hit.
	 */
	static void BlockMaterialIndices(const FIntPoint& First, const FIntPoint& Last, const FMantleTerrainHoleGrid* Holes, TArray<uint8>& Out);

	/**
	 * The most the shape's ground can lie below the heights it was made from, cm: the engine stores a block's heights again in 16 bits over the block's own range and rounds down, so
	 * the bound is that range in cm over 65,535. Never above.
	 */
	static double RoundingBoundCm(const FMantleHeightfield& Field, const FIntPoint& First, const FIntPoint& Last);

	/** Every quad of the block is a hole: nothing to stand on, so no block is made. */
	static bool BlockIsAllHole(const FIntPoint& First, const FIntPoint& Last, const FMantleTerrainHoleGrid* Holes);

	/** The block's samples as the navigation system takes them (value - HeightBias, exact in 16 bits). */
	static void BlockNavigationHeights(const FMantleHeightfield& Field, const FIntPoint& First, const FIntPoint& Last, TArray<int16>& Out);
};

/**
 * A block of a Terrain's collision, on the terrain's actor, under its root. The ground it stands for is the samples from FirstSample to LastSample (a chunk's span: neighbouring blocks
 * share their border samples). Its body is static when the terrain ends Static, and kinematic (moved with the component) when it is Movable.
 */
UCLASS(ClassGroup = (Mantle), NotBlueprintable, Transient)
class MANTLERUNTIME_API UMantleTerrainCollisionComponent : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	UMantleTerrainCollisionComponent();

	/** Makes the shape from the terrain's heights (kept) and the hole grid (kept, may be null). Call before the component registers. */
	void Init(TSharedPtr<const FMantleHeightfield> InField, const FIntPoint& InFirst, const FIntPoint& InLast, TSharedPtr<const FMantleTerrainHoleGrid> InHoles);

	/** Where the block's first sample is in the terrain's space, cm, with the height field's own bias in Z: the component's relative location. */
	FVector GetRelativeOrigin() const;

	const FIntPoint& GetFirstSample() const { return First; }
	const FIntPoint& GetLastSample() const { return Last; }
	bool HasShape() const;

	/** Bytes the shape and the block's own data hold, for the logs. */
	int64 GetShapeBytes() const;

#if !UE_BUILD_SHIPPING
	/**
	 * A test tool, not in a shipped build: how far the shape lies below the file's height, at the worst sample of this block (cm), the bound for the block
	 * (FMantleTerrainCollision::RoundingBoundCm), and whether a sample of the shape is above the file's. -MantleTestTerrainRounding makes the spawn log this for every block.
	 */
	double MeasureRoundingCm(double& OutBoundCm, bool& bOutAnyAbove) const;
#endif

	//~ Begin UActorComponent / UPrimitiveComponent
	virtual void OnCreatePhysicsState() override;
	virtual void OnDestroyPhysicsState() override;
	virtual bool ShouldCreatePhysicsState() const override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	virtual void OnUpdateTransform(EUpdateTransformFlags UpdateTransformFlags, ETeleportType Teleport = ETeleportType::None) override;
	virtual UBodySetup* GetBodySetup() override { return nullptr; }
	//~ End UActorComponent / UPrimitiveComponent

	//~ Begin navigation: heights are handed over a slice at a time, never the whole terrain at once
	virtual EHasCustomNavigableGeometry::Type HasCustomNavigableGeometry() const override { return EHasCustomNavigableGeometry::Yes; }
	virtual bool DoCustomNavigableGeometryExport(FNavigableGeometryExport& GeomExport) const override { return false; }
	virtual bool SupportsGatheringGeometrySlices() const override { return true; }
	virtual ENavDataGatheringMode GetGeometryGatheringMode() const override { return ENavDataGatheringMode::Lazy; }
	virtual void PrepareGeometryExportSync() override;
	virtual void GatherGeometrySlice(FNavigableGeometryExport& GeomExport, const FBox& SliceBox) const override;
	//~ End navigation

private:
	struct FImpl;

	/** The world scale the shape was made for: a terrain scaled while it stands has its shape made again. */
	FVector AppliedScale = FVector::OneVector;

	TSharedPtr<const FMantleHeightfield> Field;
	TSharedPtr<const FMantleTerrainHoleGrid> Holes;
	FIntPoint First = FIntPoint::ZeroValue;
	FIntPoint Last = FIntPoint::ZeroValue;

	/** The Chaos height field and the navigation samples (made on first use, read by the navigation system from other threads, never changed after). */
	TSharedPtr<FImpl> Impl;
};
