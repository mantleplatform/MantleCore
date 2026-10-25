// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class UMantleVehicleSource;

/**
 * A vehicle preset (#728): the starting point a creator picks before choosing their mesh. Read from
 * Resources/VehiclePresets/<id>.json in the plugin, a verbatim copy of schema/vehicle-presets/<id>.json (the plugin
 * ships without the repository's schema folder). Plain data, not a USTRUCT: presets are read at build and convert time
 * and nothing reflects, replicates or shows them in a Details panel.
 */
struct FMantleVehiclePreset
{
	FString Id;
	FString Name;
	FString Description;
	/** Names the movement type registered for the vehicle builder, for example "wheeled". */
	FString MovementType;
	/**
	 * The preset's `publishable` flag (#821 B2): the type is wired end to end, from the Build a vehicle window to a published vehicle, so the window shows
	 * a card for it. False when the file does not say, so a type nobody has marked ready never reaches a creator by accident.
	 */
	bool bPublishable = false;
	/** The seat kinds (for example "car"); empty when the preset does not say. */
	FString DriverSeat;
	FString PassengerSeat;

	/** Typical wheel radius and track width in centimetres, the values the mesh is scaled against. */
	double FitWheelRadius = 0.0;
	double FitTrackWidth = 0.0;
	double FitMinScale = 0.0;
	double FitMaxScale = 0.0;

	/** The allowed wheel count; 0 when the preset does not say. */
	int32 MinWheels = 0;
	int32 MaxWheels = 0;

	/** A vehicle document fragment the plugin writes into a new vehicle; may be null. */
	TSharedPtr<FJsonObject> Defaults;

	/** Whether a creator can build and publish this type (the file's `publishable` flag). */
	bool IsPublishable() const { return bPublishable; }

	/** Narrowed ranges by path; a star segment stands for any one path segment (the wheels radius key is one). */
	TMap<FString, FVector2D> Ranges;
};

struct MANTLEEDITOR_API FMantleVehiclePresets
{
	/** Every preset in Directory(), loaded once, sorted by id. A file that fails to parse is logged and skipped. */
	static const TArray<FMantleVehiclePreset>& All();

	/**
	 * The presets the Build a vehicle window offers as cards (#821 M6): those whose file says `publishable`. The flag is the only thing that decides
	 * whether a type has a card, so a type becomes a card the moment its file is marked ready. Road vehicles come first, then planes and jets,
	 * helicopters and hovercraft, each group in the order of All() (by id).
	 */
	static TArray<const FMantleVehiclePreset*> Publishable();

	/** The preset with this id, or null. */
	static const FMantleVehiclePreset* Find(const FString& Id);

	/**
	 * What a card says about the preset: the first sentence of its description, so every card is one plain sentence however much the file says
	 * after it. Ends with a full stop; empty when the preset has no description. The pickup, the van and the truck would each name vans first, so their cards
	 * say their own line instead, and a creator picking between them can tell them apart.
	 */
	static FString CardSentence(const FMantleVehiclePreset& Preset);

	/** The plugin's Resources/VehiclePresets folder; empty when the plugin is not found. */
	static FString Directory();

	/** Parses one preset file's text. Returns false with a plain-words OutError when it is not a valid preset. */
	static bool Parse(const FString& Json, FMantleVehiclePreset& Out, FString& OutError);

	/**
	 * The preset's range for a concrete path such as "wheels/2/radius": an exact key wins, otherwise a key whose "*"
	 * segments each match exactly one segment. Returns false when no key matches.
	 */
	static bool RangeFor(const FMantleVehiclePreset& Preset, const FString& Path, FVector2D& Out);

	/**
	 * Writes every value in Preset.Defaults into Source.Tuning: the inverse of FMantleVehicleConverter::MakeDocument, in the
	 * same units. Values the preset does not carry are left as they are, and the body, wheels, seats, doors, collision, paint,
	 * parts and linkage are never touched. The source is marked for undo first. A defaults key this version does not know, or
	 * a value of the wrong kind, changes nothing: it returns false and OutNotes names the key. On success OutNotes gets one or
	 * two plain-language lines saying what was set.
	 */
	static bool ApplyDefaults(const FMantleVehiclePreset& Preset, UMantleVehicleSource& Source, TArray<FString>& OutNotes);
};
