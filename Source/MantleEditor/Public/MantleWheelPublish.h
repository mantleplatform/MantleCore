// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "MantleLibraryPublish.h"
#include "MantlePlacePublishSession.h"
#include "MantleWheelItemMeasure.h"

class UStaticMesh;

/**
 * One wheel published on its own as a library item of kind wheel, with no place. The wheel is one static mesh (a /Game/... mesh,
 * its axle along +X, its face toward +X) and a creator-given name. The mesh and the materials of its slots are gathered as a Weapon
 * attachment's are, converted and uploaded by the place publish session in its library-only mode; then the wheel document
 * (FMantleWheelDocument) naming their uploaded ids is uploaded as an asset of kind wheel, and becomes the library item. The item is
 * remembered for this account and API under /Game/MantleWheels/<Name> (the weapons' /Game/MantleWeapons/<Name>).
 */
class MANTLEEDITOR_API FMantleWheelPublishSource : public IMantlePlacePublishSource
{
public:
	UE_NONCOPYABLE(FMantleWheelPublishSource);

	/** Converter: the project's cache when null (tests pass their own). */
	FMantleWheelPublishSource(FString InStaticMeshPath, FString InWheelName, TSharedPtr<FMantlePlaceAssetConverter> InConverter = nullptr);

	virtual FString GetLevelName() const override { return WheelName; }
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

	/** "wheel:" + LibraryPath(Name): the key the wheel's upload goes by. */
	static FString WheelKey(const FString& Name);

	/** "/Game/MantleWheels/" + Name: what the wheel's library item is remembered under. */
	static FString LibraryPath(const FString& Name);

	/**
	 * Why StaticMeshPath and Name cannot be published, in plain words for the creator; empty when they can. Loads the mesh and measures it
	 * (FMantleWheelItem::MeasureMesh); OutMeasure gets the result when given. Notes the measure made are not problems and are not here.
	 */
	static FString ProblemWith(const FString& StaticMeshPath, const FString& Name, FMantleWheelItemMeasure* OutMeasure = nullptr);

	/** Loads the static mesh at a /Game/... path ("/Game/Wheels/SM_Rim" or "/Game/Wheels/SM_Rim.SM_Rim"); null when it is not one. */
	static UStaticMesh* LoadWheelMesh(const FString& StaticMeshPath);

private:
	FString StaticMeshPath;
	FString WheelName;
	TSharedPtr<FMantlePlaceAssetConverter> Converter;
	FMantlePlaceDependencies Dependencies;
	FMantleDependencyPins Pins;
	/** Made by Prepare, added to the set by the last conversion step. */
	FMantleConvertedWheel Converted;
	FString LinkApi;
	FString LinkAccount;
};

/**
 * The wheel Publish (and Mantle.Wheel.Publish): one wheel publish at a time, ticked by the editor. The session it runs is kept after it
 * ends, for its result line, until the next publish.
 */
class MANTLEEDITOR_API FMantleWheelPublish
{
public:
	static FMantleWheelPublish& Get();

	/**
	 * Starts publishing the static mesh at StaticMeshPath as a wheel called Name, with Choice (who may use it: public needs the rights
	 * declaration). False, with OutError in plain words, when another publish is running, the choice is not ready or the wheel cannot
	 * be published.
	 */
	bool Start(const FString& StaticMeshPath, const FString& Name, const FMantleVehicleLibraryChoice& Choice, FString& OutError);

	bool IsBusy() const { return Session.IsValid() && !Session->IsFinished(); }

	/** The running or last session, null before the first publish. */
	TSharedPtr<FMantlePlacePublishSession> GetSession() const { return Session; }

	const FString& GetWheelName() const { return WheelName; }

private:
	bool Tick(float DeltaSeconds);

	TSharedPtr<FMantlePlacePublishSession> Session;
	FString WheelName;
	FTSTicker::FDelegateHandle TickerHandle;
};
