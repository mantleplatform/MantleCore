// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantlePlaceAssets.h"
#include "MantlePlaceTypes.h"

/**
 * Where a streamed cell's assets come from (stage 2 of large places: assets follow the cells).
 *
 * The cell streamer asks for the uploaded meshes and materials a cell's entities use when the cell has been read, builds the cell once they are
 * ready (or failed: the cell is then built without them), and gives them back when the cell comes down. Game thread.
 */
class IMantleCellAssets
{
public:
	virtual ~IMantleCellAssets() = default;

	/** The cell is wanted: its entities' meshes and materials are fetched (the ones nobody else has asked for yet). A second call for the same cell replaces the first. */
	virtual void Acquire(int32 CellIndex, const TArray<FMantleEntity>& Entities) = 0;

	/** Ids of the cell's assets that are not built yet (and have not failed), and ids that failed. Nothing pending: the cell can be built. */
	virtual void GetState(int32 CellIndex, TArray<FString>& OutPending, TArray<FString>& OutFailed) = 0;

	/** The cell no longer needs them: an asset no other cell wants is let go. */
	virtual void Release(int32 CellIndex) = 0;

	/** How long the assets may spend building this tick (seconds): the streamer shares its budget with them. */
	virtual void SetBuildSlice(double Seconds) { (void)Seconds; }

	/** #189: where the view is, for how much texture detail the cells' materials get (FMantlePlaceAssets::SetTextureView). Every tick. */
	virtual void SetView(const FVector& Location) { (void)Location; }

	/** The Nanite root pages the place's meshes have attached now: the live budget (stage 8) counts them. Zero where nothing counts them (tests). */
	virtual int64 GetNaniteRootPagesAttached() const { return 0; }

	/**
	 * A number that moves whenever the place's downloads move (a byte received, an asset built or failed): the streamer's holds and asset waits give up only once it
	 * has stood still (FMantleCellStreamSettings::HoldIdleTimeoutSeconds, AssetIdleTimeoutSeconds). It may walk the whole asset set: the streamer asks a few times a second.
	 * Zero where nothing counts it (tests): then a wait gives up the idle timeout after it began.
	 */
	virtual int64 GetProgressMark() const { return 0; }
};

/**
 * The place's streaming asset set (FMantlePlaceAssets::PrepareStreaming) as the assets of one streamer's cells. A place can run more than one streamer (a standalone
 * game runs one for its solid things and one for its pictures): Slot tells their cells apart, so the same cell asked for by two of them is two holders.
 */
class MANTLERUNTIME_API FMantlePlaceCellAssets : public IMantleCellAssets
{
public:
	FMantlePlaceCellAssets(TSharedRef<FMantlePlaceAssets> InSet, int32 InSlot)
		: Set(MoveTemp(InSet))
		, Slot(InSlot)
	{
	}

	virtual void Acquire(int32 CellIndex, const TArray<FMantleEntity>& Entities) override { Set->AcquireEntityAssets(HolderOf(CellIndex), Entities); }

	virtual void GetState(int32 CellIndex, TArray<FString>& OutPending, TArray<FString>& OutFailed) override
	{
		const FMantlePlaceAssets::FHolderAssets Assets = Set->GetHolderAssets(HolderOf(CellIndex));
		OutPending = Assets.PendingIds;
		OutFailed = Assets.FailedIds;
	}

	virtual void Release(int32 CellIndex) override { Set->ReleaseEntityAssets(HolderOf(CellIndex)); }

	virtual void SetBuildSlice(double Seconds) override { Set->SetStreamBuildSlice(Seconds); }

	virtual void SetView(const FVector& Location) override { Set->SetTextureView(Location); }

	virtual int64 GetNaniteRootPagesAttached() const override { return Set->GetNaniteRootPagesAttached(); }

	virtual int64 GetProgressMark() const override
	{
		const FMantlePlaceAssetProgress Progress = Set->GetProgress();
		return Progress.DoneBytes + Progress.ReadyCount + Progress.FailedCount;
	}

private:
	int64 HolderOf(int32 CellIndex) const { return (static_cast<int64>(Slot) << 32) | static_cast<int64>(static_cast<uint32>(CellIndex)); }

	TSharedRef<FMantlePlaceAssets> Set;
	int32 Slot = 0;
};
