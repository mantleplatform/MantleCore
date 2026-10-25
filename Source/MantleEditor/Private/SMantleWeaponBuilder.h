// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleThumbnailChoice.h"
#include "MantleBuilderKind.h"
#include "MantleLibraryPublish.h"
#include "MantleWeaponBuilderKind.h"
#include "MantleWeaponSource.h"
#include "SMantleBuilderFlow.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SMantleBuilderViewport;
struct FPropertyChangedEvent;

namespace MantleWeaponBuilderPanelPrivate
{
	/** Copies every authoring property when Create writes the source asset, including fields added after the wizard was written. */
	void CopySourceProperties(const UMantleWeaponSource& Source, UMantleWeaponSource& Destination);
}

/**
 * The "Build a weapon" window (#729): a guided flow, one decision per screen, that turns a creator's 3D model into a
 * saved weapon source. The rules that need no window live in MantleWeaponBuilderLogic.
 */
class SMantleWeaponBuilder : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SMantleWeaponBuilder) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SMantleWeaponBuilder() override;

private:
	friend class FMantleWeaponBuilderPanelSaveReopenTest;
	friend class FMantleWeaponBuilderPanelAssetInvalidationTest;

	TArray<FMantleBuilderStep> MakeSteps();

	TSharedRef<SWidget> MakeKindStep();
	TSharedRef<SWidget> MakeModelStep();
	TSharedRef<SWidget> MakeSizeStep();
	TSharedRef<SWidget> MakePointsStep();
	TSharedRef<SWidget> MakeTuningStep();
	TSharedRef<SWidget> MakePartsAndSoundStep();
	TSharedRef<SWidget> MakeCreateStep();
	/** #124: the Thumbnail step, the shared section on the weapon's (or consumable's) model. */
	TSharedRef<SWidget> MakeThumbnailStep();
	/** The choice is kept under the weapon's safe name (FMantleThumbnailStore, class "weapon"): Create saves it again under the name it wrote. */
	void LoadThumbnailChoice(const FString& SafeName);
	void SaveThumbnailChoice(const FString& SafeName);

	TSharedRef<SWidget> MakeHoldCard(EMantleWeaponHold Hold);

	void SetHold(EMantleWeaponHold Hold);
	void LoadSourceAsset(UMantleWeaponSource* Source);
	void SetModel(UObject* Model);
	void SetFit(float Scale, bool bOverride);
	void TurnFit(const FQuat& Turn);

	/** Fits the model again with the current hold, size and turns, and merges the new guesses into the points (the creator's own placements are kept). */
	void Refit();
	UObject* LoadModel() const;
	float ModelLengthCm() const;
	/** A point-placement viewport that writes the creator's edits back into the working weapon. */
	TSharedRef<SMantleBuilderViewport> MakeViewport();
	bool HasAllRequiredPoints(FText& OutWhyNot) const;
	FText WarningsText() const;
	bool IsRoleFlagged(const FMantleBuilderPointRole& Role, const TArray<FText>& Warnings) const;
	FText SummaryText() const;

	void Create();

	/** Opens the game with the created weapon in the player's hands (#729). Only offered after Create, until the weapon is edited. */
	void TestLocally();
	bool CanTestLocally() const;

	/** #840 step 6: publishes the created weapon to the creator's library (FMantleWeaponPublish), with PublishChoice. */
	void Publish();
	bool CanPublish() const;
	/** The running or last publish of the created weapon, in plain words; empty when there is none. */
	FText PublishStatusText(bool& bOutIsError) const;
	void OnObjectPropertyChanged(UObject* Object, FPropertyChangedEvent& Event);
	bool IsSelectedAssetPackage(const UObject* Object) const;

	TStrongObjectPtr<UMantleWeaponSource> Working;
	/** #124: how the weapon's library picture is made. */
	FMantleThumbnailChoice ThumbnailChoice;
	FSoftObjectPath SourceAssetPath;
	FString SourceBaselineSignature;
	TSharedPtr<FMantleWeaponBuilderKind> Kind;
	/** Weak: a viewport lives only while its step is open. */
	TWeakPtr<SMantleBuilderViewport> SizeViewport;
	TWeakPtr<SMantleBuilderViewport> PointViewport;
	TSharedPtr<SMantleBuilderFlow> Flow;
	bool bNameEdited = false;
	/** The safe name of the weapon this window already created, so pressing Create again updates it instead of picking a new name. */
	FString CreatedSafeName;
	/** MantleWeaponBuilderLogic::EditSignature of the weapon when Create last wrote its files; empty until it succeeds. Test locally needs it to still match. */
	FString CreatedSignature;
	/** What the last Test locally said, in plain words; shown in the Create step. */
	FText TestResult;
	bool bTestResultIsError = false;

	/** Who may use the published weapon: public (the default) needs the rights declaration (MantleRights). */
	FMantleVehicleLibraryChoice PublishChoice;
	/** Why Publish could not start, until the next press. */
	FText PublishRefusal;
	bool bSelectedAssetEditedSinceCreate = false;
	FDelegateHandle ObjectPropertyChangedHandle;
};
