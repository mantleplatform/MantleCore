// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/** What the wheels-layer chooser reads about one upload slot of a wheel's mesh. */
struct MANTLEEDITOR_API FMantleWheelSlotFacts
{
	/** The mesh's name for the slot (its material slot name). */
	FString SlotName;

	/** The slot's material's name. */
	FString MaterialName;

	/** The slot's material reads as car paint (FMantleVehicleConverter::LooksLikePaint). */
	bool bPaint = false;
};

/**
 * The wheel library item's document (format MWL1, the backend's cook/wheel.go and schema/wheel.schema.json), built from what the
 * wheel publish measured and uploaded. Pure: no assets, no network.
 */
struct MANTLEEDITOR_API FMantleWheelDocument
{
	static constexpr int32 MaxNameLength = 64;
	static constexpr int32 MaxMaterials = 64;
	static constexpr double MinRadius = 5.0;
	static constexpr double MaxRadius = 150.0;
	static constexpr double MinWidth = 2.0;
	static constexpr double MaxWidth = 100.0;

	/**
	 * Which of the mesh's upload slots wear the car's wheels paint layer, as indexes into the materials list: a slot whose material
	 * is paint, or whose slot or material name reads as a rim (FMantleVehicleRims::NameIsRim), so the car's wheels color colors the
	 * rim but not the tire. Ascending, each once.
	 */
	static TArray<int32> ChooseWheelSlots(TConstArrayView<FMantleWheelSlotFacts> Slots);

	/**
	 * The document as compact JSON: schemaVersion, name, mesh (the uploaded id), materials (uploaded ids, one per upload slot of the
	 * mesh), referenceRadius and referenceWidth (cm) and, when any, wheelSlots. False, with OutError in plain words, for a name that is
	 * empty or over MaxNameLength characters, an empty id, more than MaxMaterials materials, a size outside the range the library
	 * takes, or a wheel slot that is not one of the materials (or is listed twice).
	 */
	static bool Build(const FString& Name, const FString& MeshId, const TArray<FString>& MaterialIds, double ReferenceRadius,
		double ReferenceWidth, const TArray<int32>& WheelSlots, FString& OutJson, FString& OutError);
};
