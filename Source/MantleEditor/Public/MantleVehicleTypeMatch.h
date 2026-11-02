// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleVehicleChassisSkin.h"

class UObject;

/** What a model looks like, in the builder's terms: the group of vehicle types it belongs to. */
enum class EMantleVehicleModelFamily : uint8
{
	/** Nothing in the model says. */
	Unknown,
	/** Four or more wheels on the ground: a car, a truck, a van, a six-wheeler. */
	Road,
	/** Two wheels in a line. */
	Motorcycle,
	/** Wings, a propeller or a jet engine: a plane or a jet. */
	Plane,
	/** A main rotor. */
	Helicopter,
	/** Nothing wheeled or winged: only a hovercraft's type accepts it without a question. */
	Hover,
};

/** What the check decided. */
enum class EMantleVehicleTypeVerdict : uint8
{
	/** The model fits the picked type. */
	Fits,
	/** The model looks like another type: SuggestedType says which. */
	Suggests,
	/** The model gives no clear sign either way. It is let through, and Message says why. */
	Unsure,
};

/** How sure the check is of a suggestion. Names and wheel counts are strong; only the model's proportions are weak. */
enum class EMantleVehicleTypeConfidence : uint8 { Strong, Weak };

/** The result of checking a model against the vehicle type the creator picked (#821 flow, step 3). */
struct MANTLEEDITOR_API FMantleVehicleTypeMatch
{
	EMantleVehicleTypeVerdict Verdict = EMantleVehicleTypeVerdict::Fits;
	EMantleVehicleTypeConfidence Confidence = EMantleVehicleTypeConfidence::Strong;
	/** What the model looks like. */
	EMantleVehicleModelFamily Family = EMantleVehicleModelFamily::Unknown;
	/** The preset id to switch to ("motorcycle"); empty unless Verdict is Suggests. */
	FString SuggestedType;
	/** The suggested type's name for a button: "Motorcycle". */
	FString SuggestedName;
	/** Plain words for the creator, saying why; empty when the model simply fits. */
	FString Message;
	/** The button's text, "Switch to Motorcycle"; empty unless Verdict is Suggests. */
	FString ButtonText;
	/** What the check saw, one short phrase each: "2 wheels, in a line", "a propeller". For logs and tests. */
	TArray<FString> Evidence;

	bool Suggests() const { return Verdict == EMantleVehicleTypeVerdict::Suggests; }
};

/**
 * Whether a model fits the vehicle type picked for it. Reads only what the model already tells: wheel-like parts (bones, then material slots) and how many there
 * are and whether they sit in a line, wings, a propeller or a jet's engine, a rotor, and, when there is no such name, its proportions. The result never
 * stops a build: it is advice for the window, which shows Message and a button that switches the type to SuggestedType.
 */
struct MANTLEEDITOR_API FMantleVehicleTypeMatchCheck
{
	/** Checks Model against the preset TypeId ("sedan", "plane"...). An unknown TypeId is Unsure. */
	static FMantleVehicleTypeMatch Check(const FMantleVehicleModel& Model, const FString& TypeId);

	/** The family a preset belongs to ("sedan" is Road, "jet" is Plane); Unknown for an id with no preset. */
	static EMantleVehicleModelFamily FamilyOfType(const FString& TypeId);

	/** The family the model looks like, with what it saw in OutEvidence and how sure it is in OutConfidence. Unknown when it gives no sign. */
	static EMantleVehicleModelFamily FamilyOfModel(const FMantleVehicleModel& Model, TArray<FString>& OutEvidence, EMantleVehicleTypeConfidence& OutConfidence);

	/** Reads Asset (a static or skeletal mesh) and checks it. False, with the reason in OutError, when the model cannot be read. */
	static bool CheckAsset(UObject* Asset, const FString& TypeId, FMantleVehicleTypeMatch& Out, FString& OutError);
};
