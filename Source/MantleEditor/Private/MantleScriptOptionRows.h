// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "IPropertyTypeCustomization.h"
#include "MantlePlaceTypes.h"

class FDetailWidgetRow;
class IPropertyHandle;
struct FMantleLocalScriptPackage;
struct FMantleStorageSetting;

/** The editor a package option's row draws. */
enum class EMantleScriptOptionWidget : uint8
{
	/** A text box: a string. */
	Text,
	/** A text box that keeps only numbers: a number without both a min and a max. */
	Number,
	/** A slider between min and max: a number with both. */
	Slider,
	/** A checkbox: a boolean. */
	CheckBox,
	/** A dropdown of the options: a choice. */
	Dropdown,
};

/**
 * What one row of a package option shows, a Script Point parameter or a storage entry's setting: the pure half of the
 * Details rows (FMantleScriptOptionRows), so every rule here is an automation test and the Slate stays thin over it.
 */
struct FMantleScriptOptionRow
{
	FString Key;
	EMantleScriptOptionWidget Widget = EMantleScriptOptionWidget::Text;

	/** "Speed (number, 1 to 10, required)". */
	FString Label;

	/** The option's help, shown under its name. */
	FString Help;

	/** "Speed (number): help". */
	FString Tooltip;

	/** "5 (default)", or empty when the option has no default. */
	FString Hint;

	/** The value set, else the default, else empty: what a checkbox, dropdown or slider shows. */
	FString Shown;

	/** True when the place sets a value (the Default button clears it). */
	bool bIsSet = false;

	/** A dropdown's entries: the choice's options, in order. */
	TArray<FString> Choices;

	/** A slider's range. */
	double SliderMin = 0.0;
	double SliderMax = 0.0;

	/** A red note under the name, or empty: a required option with no value and no default, or a value that does not fit. */
	FString Note;

	/** The row for Option of the package named Package, given the value the place sets (unset: none). */
	static FMantleScriptOptionRow Make(const FString& Package, const FMantleScriptPackageParameter& Option, const TOptional<FString>& Value);

	/**
	 * What committing Text in a row writes: true with OutValue unset for empty text (the default applies), true with OutValue set to
	 * Text when it can be kept (a number that reads as one, outside its range included, which the note then names; true or false; one
	 * of a choice's options; any text up to 256 characters), false when nothing should change.
	 */
	static bool Commit(const FMantleScriptPackageParameter& Option, const FString& Text, TOptional<FString>& OutValue);
};

/** The Slate half of an option row, shared by the Script Point details and the storage entry customization. */
class FMantleScriptOptionRows
{
public:
	using FGetter = TFunction<TOptional<FString>()>;
	using FSetter = TFunction<void(const TOptional<FString>&)>;

	/**
	 * Fills Row with Option's row: its label, help and red note on the name side, and on the value side the editor
	 * FMantleScriptOptionRow::Make picks (text box, number box, slider, checkbox or dropdown) with the default as a hint, beside a
	 * Default button that clears the value. Get reads the value the place sets now; Set writes one (unset: remove it).
	 */
	static void FillRow(FDetailWidgetRow& Row, const FString& Package, const FMantleScriptPackageParameter& Option, FGetter Get, FSetter Set);
};

/**
 * Place Settings > Storage rows: a script package's entry whose manifest is known draws one typed row per setting its
 * manifest declares (FMantleScriptOptionRows), writing the entry's Settings, instead of the plain map. Any other entry draws as before.
 */
class FMantleStorageSettingCustomization : public IPropertyTypeCustomization
{
public:
	static TSharedRef<IPropertyTypeCustomization> MakeInstance();

	/**
	 * The manifest whose settings an entry's rows draw: a script entry's (or a hand-typed entry's whose name a Packages/ folder has)
	 * FMantleScriptPackages::KnownManifest. False for a vehicle, a weapon, or a package whose manifest is not known. Pure.
	 */
	static bool ManifestFor(const FMantleStorageSetting& Entry, const TArray<FMantleLocalScriptPackage>& Local, FMantleScriptPackageManifest& OutManifest);

	virtual void CustomizeHeader(TSharedRef<IPropertyHandle> PropertyHandle, FDetailWidgetRow& HeaderRow, IPropertyTypeCustomizationUtils& Utils) override;
	virtual void CustomizeChildren(TSharedRef<IPropertyHandle> PropertyHandle, IDetailChildrenBuilder& ChildBuilder, IPropertyTypeCustomizationUtils& Utils) override;
};
