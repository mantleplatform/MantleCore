// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "MantleLibraryPublish.h"
#include "MantlePlacePublishSession.h"

/**
 * #840 step 6: one weapon the weapon builder made (a folder of Content/MantleWeapons), published on its own as a library item with
 * no place. Its 3D model and the materials of its slots are gathered as a Weapon Spawn's are (FMantlePlaceDependencies::
 * AddBuiltWeapon), converted and uploaded by the place publish session in its library-only mode, and the
 * weapon becomes an item of kind weapon. The item is remembered for this account and API under /Game/MantleWeapons/<Name>, the
 * same key a place go-live uses, so both routes add versions to one item.
 */
class MANTLEEDITOR_API FMantleWeaponPublishSource : public IMantlePlacePublishSource
{
public:
	UE_NONCOPYABLE(FMantleWeaponPublishSource);

	/** Converter: the project's cache when null (tests pass their own). */
	explicit FMantleWeaponPublishSource(FString InWeaponName, TSharedPtr<FMantlePlaceAssetConverter> InConverter = nullptr);

	virtual FString GetLevelName() const override { return WeaponName; }
	virtual bool Prepare(FMantleValidationResult& OutValidation, FString& OutMessage) override;
	virtual int32 NumConversionSteps() const override;
	virtual FString ConversionStepName(int32 Step) const override;
	virtual void Convert(int32 Step, FMantlePlaceAssetSet& Set) override;
	virtual bool CollectScripts(TArray<FMantlePlaceScriptUpload>& OutScripts, FString& OutMessage) override;
	virtual EMantlePlaceExportOutcome Export(const FMantlePlaceAssetSet& Set, const TMap<FString, FString>& ScriptSourceIds,
		const TMap<FString, FString>& ScriptAuthorSourceIds, FString& OutPlaceJson, TArray<FMantleValidationIssue>& OutIssues) override;
	virtual FString FindPlaceId() override { return FString(); }
	virtual void RememberPlaceId(const FString& PlaceId) override {}
	virtual void ForgetPlaceId() override {}
	virtual FString FindLibraryItem(const FString& SourcePath) override;
	virtual void RememberLibraryItem(const FString& SourcePath, const FString& ItemId) override;
	virtual void ForgetLibraryItem(const FString& SourcePath) override;
	virtual int32 LibraryGeneration(const FString& SourcePath) override;
	virtual FString AttributionFor(const FString& ObjectPath) const override;
	virtual void ConversionFinished() override { Pins.Release(); }
	virtual void EndPublish() override { Pins.Release(); }
	virtual bool IsLibraryOnly() const override { return true; }

	/**
	 * Why WeaponName cannot be published as it is on disk, in plain words for the creator; empty when it can be. Pure apart from
	 * reading its files: a name outside the builder's folder, no settings file, one that holds what cannot be published yet
	 * (FMantlePlaceAssets::WeaponDocumentProblem), or no 3D model (every weapon has one, #897).
	 */
	static FString ProblemWith(const FString& WeaponName);

private:
	FString WeaponName;
	TSharedPtr<FMantlePlaceAssetConverter> Converter;
	FMantlePlaceDependencies Dependencies;
	FMantleDependencyPins Pins;
	FString LinkApi;
	FString LinkAccount;
};

/**
 * The weapon builder's Publish button (and Mantle.Weapon.Publish): one weapon publish at a time, ticked by the editor, so the creator
 * keeps working. The session it runs is kept after it ends, for its result line, until the next publish.
 */
class MANTLEEDITOR_API FMantleWeaponPublish
{
public:
	static FMantleWeaponPublish& Get();

	/**
	 * Starts publishing WeaponName with Choice (who may use it: public, the default, needs the rights declaration). False, with
	 * OutError in plain words, when another publish is running, the choice is not ready or the weapon cannot be published.
	 */
	bool Start(const FString& WeaponName, const FMantleVehicleLibraryChoice& Choice, FString& OutError);

	/**
	 * Start for each of WeaponNames in turn, with one Choice: the next starts when the one before ends, whether it succeeded or not.
	 * Every name is checked first (ProblemWith); one that cannot be published refuses the whole batch, sending nothing. Logs
	 * MANTLE_WEAPON_PUBLISH_ALL_DONE ok=<n> failed=<n> once the last ends.
	 */
	bool StartAll(const TArray<FString>& WeaponNames, const FMantleVehicleLibraryChoice& Choice, FString& OutError);

	bool IsBusy() const { return (Session.IsValid() && !Session->IsFinished()) || Waiting.Num() > 0; }

	/** The running or last session, null before the first publish. */
	TSharedPtr<FMantlePlacePublishSession> GetSession() const { return Session; }

	/** The weapon the running or last session is for. */
	const FString& GetWeaponName() const { return WeaponName; }

private:
	bool Tick(float DeltaSeconds);
	void StartNext();

	TSharedPtr<FMantlePlacePublishSession> Session;
	FString WeaponName;
	/** The batch's weapons still to publish, and its choice and tally. */
	TArray<FString> Waiting;
	FMantleVehicleLibraryChoice BatchChoice;
	int32 Succeeded = 0;
	int32 Failed = 0;
	FTSTicker::FDelegateHandle TickerHandle;
};
