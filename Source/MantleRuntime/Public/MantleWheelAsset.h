// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "UObject/Object.h"
#include "MantleWheelAsset.generated.h"

/**
 * A creator wheel document (schema/wheel.schema.json, MWL1), read. A wheel is a rim mesh any car can wear: it names its
 * static mesh (axle on +X, face toward +X) and its materials by uploaded asset id. Units are Unreal's: centimeters.
 */
struct FMantleWheelDesc
{
	FString Name;

	/** The uploaded static mesh asset id. */
	FString MeshId;

	/** The uploaded material id of each mesh material slot, in slot order; empty when the document lists none. */
	TArray<FString> MaterialIds;

	/** The size (cm) the mesh was modeled at; the game scales the wheel to each car's tire. [5, 150]. */
	double ReferenceRadius = 0.0;

	/** [2, 100]. */
	double ReferenceWidth = 0.0;

	/** The mesh material slots that wear the car's wheels paint layer; the rest keep their own material. Unique. */
	TArray<int32> WheelSlots;
};

/** One problem with a wheel document, located by a JSON pointer ("" is the document itself). */
struct FMantleWheelDocError
{
	FString Pointer;
	FString Message;

	FString ToString() const;
};

/** The strict reader of a wheel document: unknown keys, wrong types and out-of-range values are refused. Pure. */
struct MANTLERUNTIME_API FMantleWheelDocument
{
	/** The largest document read (cook.MaxWheelDocumentBytes). */
	static constexpr int32 MaxBytes = 16 * 1024;

	/** Reads Json. False with every problem found in OutErrors (Out is left default). */
	static bool Parse(const FString& Json, FMantleWheelDesc& Out, TArray<FMantleWheelDocError>& OutErrors);

	/** The same, with the problems as lines "<pointer>: <message>" in OutError. */
	static bool Parse(const FString& Json, FMantleWheelDesc& Out, FString& OutError);
};

/** An uploaded Wheel asset, read: the MWL1 the cook stored (the wheel document as compact UTF-8 JSON, no header). */
struct MANTLERUNTIME_API FMantleWheelAsset
{
	FString Text;
	FMantleWheelDesc Desc;

	/** Every distinct asset id the document names (its mesh, then its materials). */
	TArray<FString> NamedIds() const;

	/** Reads MWL1 bytes with the strict document reader. False with every problem in OutError. Safe off the game thread. */
	static bool Read(TConstArrayView<uint8> Bytes, FMantleWheelAsset& Out, FString& OutError);
};

/**
 * What FMantlePlaceAssets keeps for an uploaded wheel id a place stores: the read document and, on a machine that
 * renders, the built mesh and materials the document names. A material that did not load is the default surface.
 * Off a rendering machine (a dedicated server) Mesh is null and Materials is empty: the document is all it keeps.
 */
UCLASS(Transient)
class MANTLERUNTIME_API UMantleBuiltWheel : public UObject
{
	GENERATED_BODY()

public:
	FMantleWheelDesc Desc;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> Mesh;

	/** One per Desc.MaterialIds entry, in order (empty when the document lists none). */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInterface>> Materials;
};
