// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "UObject/StrongObjectPtr.h"

class UMantleBuiltVehicle;
class UMantleVehicleSource;
class UMaterialInterface;
class USoundBase;
class UStaticMesh;

/**
 * A converted vehicle asset made ready to drive in this process without publishing it: the probes' `source:` car, in
 * a developer's editor build. What publishing would upload is resolved here as the cook resolves it (backend cook resolveVehicle:
 * each wheel's size measured from the body with its tires merged, one collision box around the chassis, the seat, the shift points)
 * and then read by the client's own document reader (FMantleVehicleAssetReader::ParseDocument), so a definition made from Vehicle
 * (FMantlePlaceVehicleDefinitions::Make) maps the tuning, wheels, linkage, seats, steering wheel, doors, trunk, collision and centre
 * of mass exactly as a published car's does.
 *
 * What an upload carries by id stays local, named by stand-in ids ("ast_u" and a counter) that Meshes and Materials resolve: the
 * body is the source's own skeletal mesh (each slot its own material), each part its own static mesh (its overrides, else the
 * mesh's slot materials), and each wheel's tire mesh a part on the wheel's bone (publishing merges it into the body; either way
 * it turns with the wheel). The horn is the source's own sound. Liveries are left out: they upload as textures. No section,
 * triangle or part cap applies, since nothing is uploaded; WouldNotPublish says what Check or the cook would refuse.
 */
struct MANTLEEDITOR_API FMantleVehiclePreview
{
	/** The vehicle as a place builds one: Mesh the source's body, Desc the resolved document with the stand-in ids below. */
	TStrongObjectPtr<UMantleBuiltVehicle> Vehicle;

	/**
	 * Stand-in id to the source's own mesh or material. Nothing here keeps them loaded: make the definition at once, as
	 * AMantleSourceVehicle does, whose attachments and materials then hold them.
	 */
	TMap<FString, UStaticMesh*> Meshes;
	TMap<FString, UMaterialInterface*> Materials;

	/** The source's Horn Sound, or null. */
	USoundBase* Horn = nullptr;

	/** Convert Vehicle's notes on a copy of the source (the asset is left alone), Check's warnings, and what was left out. */
	TArray<FString> Notes;

	/** What would stop the vehicle publishing: Check's errors (the upload's caps among them) and what the cook would refuse. */
	TArray<FString> WouldNotPublish;

	/**
	 * Fills Out from Source. False, with the reason, when there is nothing to drive: no body, a wheel with no size to measure, no
	 * collision or seat and nothing to place them by, or a document the reader refuses (a value outside its range: Check names it).
	 */
	static bool Make(const UMantleVehicleSource& Source, FMantleVehiclePreview& Out, FString& OutError);
};
