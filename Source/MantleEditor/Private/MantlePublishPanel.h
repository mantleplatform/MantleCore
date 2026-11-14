// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantlePlacePublishSession.h"
#include "Widgets/SCompoundWidget.h"

class SVerticalBox;
class UWorld;

/**
 * The Mantle Publish panel: one Publish click does everything for a level built from
 * Fab assets (check, convert, upload, wait for Mantle, publish), and this is where it shows. One
 * overall progress bar, a five-line checklist in plain words, and rows only for the assets that
 * need the creator, each naming the Unreal asset and the fix. The publish runs in the background
 * (a core ticker), so the creator keeps working and closing the tab does not stop it. Native Slate.
 * Nothing about prices: paid content is not handled here.
 */
class SMantlePublishPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SMantlePublishPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** The nomad tab: registered at module startup, opened by Publish and Save to Mantle. */
	static const FName TabName;
	static void RegisterTab();
	static void UnregisterTab();

	/**
	 * Starts publishing (or saving) World in the background and shows the panel. While one is
	 * running, only the panel is shown: one publish at a time.
	 */
	static void Start(UWorld* World, bool bGoLive, const FString& Notes);

	/** The publish running now, or the last one. */
	static TSharedPtr<FMantlePlacePublishSession> GetSession();

	static bool IsRunning();

private:
	EActiveTimerReturnType Refresh(double InCurrentTime, float InDeltaTime);
	void RebuildAttention();
	TSharedRef<SWidget> MakeStepRow(EMantlePublishStep Step, const FText& Label);
	/** The "who can use the vehicles" choice, collapsed unless the running go-live has vehicles. */
	TSharedRef<SWidget> MakeVehicleChoice();

	FText GetTitleText() const;
	FText GetStatusText() const;
	FText GetNotesText() const;
	FText GetDetailsText() const;
	FReply OnCancel();
	FReply OnPublishAgain();

	TSharedPtr<SVerticalBox> AttentionBox;
	TWeakPtr<FMantlePlacePublishSession> ShownSession;
	int32 ShownAttention = INDEX_NONE;
};
