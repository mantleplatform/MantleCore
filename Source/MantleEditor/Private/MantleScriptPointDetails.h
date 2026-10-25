// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"
#include "MantlePlaceTypes.h"

class AMantleScriptPoint;
class IDetailLayoutBuilder;
class IPropertyHandle;
class UWorld;
struct FMantleLocalScriptPackage;
struct FMantleStorageSetting;

/** One package a Script Point may name: a storage entry of the level, a Packages/ folder beside it, or both. */
struct FMantleScriptPointChoice
{
	/** The storage name: what the point's Storage Item holds. */
	FString Name;

	/** The storage entry's recorded kind: "script", or empty when it was typed in by hand (unknown). Empty for a Packages/ folder. */
	FString Kind;

	/** In the level's Storage list. */
	bool bStored = false;

	/** A Packages/<Name>/ folder beside the level: publishing stores it under Name. */
	bool bLocal = false;

	/** The package's manifest is known (its package.json, or what the Library panel or a publish recorded). */
	bool bHasManifest = false;
	FMantleScriptPackageManifest Manifest;

	/** What the combo shows: the name, and where the package comes from. */
	FString Label() const;
};

/**
 * The Details panel of a Mantle Script Point: Storage Item is a list of the level's script packages (stored ones first, then
 * the Packages/ folders not stored yet, then entries typed in by hand whose kind is not known; vehicles and weapons are left out),
 * and the parameters the chosen package declares each get a row with its type, default and help, writing into the point's
 * Parameters (the plain map stays below for anything else).
 */
class FMantleScriptPointDetails : public IDetailCustomization
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance();

	/** Registers and unregisters the customization with the PropertyEditor module (module startup and shutdown). */
	static void Register();
	static void Unregister();

	/**
	 * What a Script Point in a level with Storage and Local packages may name, in the order the list shows: script-kind storage
	 * entries, then Packages/ folders with no entry of their name, then unknown-kind entries. An entry and a folder of the same name
	 * are one choice, with the folder's manifest when it has one (publishing replaces the entry with the folder's package). An entry of
	 * another kind (a vehicle, a weapon) is left out, and so is a folder whose name another kind's entry holds. Pure.
	 */
	static TArray<FMantleScriptPointChoice> MakeChoices(const TArray<FMantleStorageSetting>& Storage, const TArray<FMantleLocalScriptPackage>& Local);

	/** MakeChoices for World's Mantle Place Settings and Packages/ folder. */
	static TArray<FMantleScriptPointChoice> GatherChoices(UWorld* World);

	virtual void CustomizeDetails(const TSharedPtr<IDetailLayoutBuilder>& DetailBuilder) override;
	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;

private:
	/** Sets (or, with an unset Value, removes) one parameter on every selected point, as one undoable step. */
	void SetParameter(const FString& Key, const TOptional<FString>& Value);

	/** The parameter's value as the first selected point has it, or unset. */
	TOptional<FString> GetParameter(const FString& Key) const;

	TArray<TWeakObjectPtr<AMantleScriptPoint>> Points;
	TArray<TSharedPtr<FMantleScriptPointChoice>> Choices;
	TSharedPtr<IPropertyHandle> StorageHandle;
	TWeakPtr<IDetailLayoutBuilder> Layout;
};
