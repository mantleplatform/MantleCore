// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleVehicleSource.h"

class UMaterialInterface;
class UMantleVehicleSource;
class USkeletalMesh;
struct FMantleSkinnedMesh;
struct FSkeletalMaterial;

/** The five kinds of vehicle light (schema/vehicle.schema.json lights), in the document's order. */
enum class EMantleLightKind : uint8
{
	Head,
	Rear,
	Reverse,
	Left,
	Right,
};

/** What the light finder found on a model, before it is cut (FMantleVehicleLights::Find). */
struct FMantleVehicleLightGuess
{
	/** Per kind, the body's triangles (LOD 0, triangle numbers) that glow as that light. */
	TArray<int32> Triangles[5];
	/** Per kind, the material slots (by number) that are a whole light: a slot named for one, or one glowing one color all over. */
	TArray<int32> WholeSlots[5];
	/** The headlight beams: at each headlight's middle, pointing forward; at the front corners when the model shows no headlights. */
	TArray<FMantleVehicleSourceLightBeam> Beams;

	bool FoundAny() const;
};

/**
 * The vehicle builder's lights (vehicle-lights-plan.md): where a creator's model glows, and the body sections that glow as each light.
 *
 * Fab cars paint every light into one glow texture on the body (one material for the paint, the headlights and the brake lights), so the
 * finder reads each material's glow (FMantleMaterialGlow), looks the glow up under every triangle, and sorts the glowing triangles by where
 * they sit and their color: front and white or yellow is a headlight, back and red a rear light, back and white a reverse light, amber a
 * signal on its side. Cut then moves each light's triangles into a section of its own, with the same material, so the game can turn that
 * section's glow up and down. A slot named for a light (head, tail, brake, reverse, indicator, signal, blinker, lamp, light) is a light as it is.
 *
 * Each kind has one fixed color (Color) and the light's material copy glows with it (MakeLightMaterial), keeping the model's glow texture as
 * the lens's shape. Nothing here refuses a model: a model with no glow and no named slots gets no light sections and the beams alone.
 */
struct MANTLEEDITOR_API FMantleVehicleLights
{
	static constexpr int32 NumKinds = 5;

	/** The document's key: head, rear, reverse, left, right. */
	static const TCHAR* Key(EMantleLightKind Kind);
	/** Plain words for the row: Headlights, Rear lights, Reverse lights, Left signal, Right signal. */
	static FText Label(EMantleLightKind Kind);
	/** The name a cut section gets: Headlights, RearLights, ReverseLights, LeftSignal, RightSignal (made unique by the caller). */
	static FString SectionName(EMantleLightKind Kind);
	/** The kind's one color (linear): white headlights and reverse lights, red rear lights, amber signals. */
	static FLinearColor Color(EMantleLightKind Kind);

	/** The kind's list in Lights. */
	static TArray<FString>& Sections(FMantleVehicleSourceLights& Lights, EMantleLightKind Kind);
	static const TArray<FString>& Sections(const FMantleVehicleSourceLights& Lights, EMantleLightKind Kind);

	/**
	 * Which light a glowing spot is, from where it sits in the body's bounds (+X is the front, left is -Y) and the color it glows; unset when it reads as no light
	 * (a glowing dashboard in the middle of the car, a red spot at the front). Pure.
	 */
	static TOptional<EMantleLightKind> Classify(const FVector3f& Center, const FLinearColor& Glow, const FBox3f& Bounds);

	/** The light a slot's or material's name says it is, from its words (case and separators ignored); unset when it names none. Pure. */
	static TOptional<EMantleLightKind> FromName(const FString& Name, const FVector3f& Center, const FBox3f& Bounds);

	/**
	 * Finds the lights on Body (LOD 0, the vehicle's space: +X forward, cm) with Materials its slots. bCenterline (a bike) gets one beam on the middle line.
	 * Reads the materials' glow and their glow textures, so game thread.
	 */
	static FMantleVehicleLightGuess Find(const FMantleSkinnedMesh& Body, const TArray<FSkeletalMaterial>& Materials, bool bCenterline);

	/**
	 * Moves each kind's guessed triangles into sections of their own (one per kind and material they came from), each a new slot with the same material, named
	 * SectionName; whole slots stay as they are. OutLights gets each kind's sections by the names the vehicle will list (FMantleVehicleConverter::UniqueSectionNames
	 * of the slots) and the beams. Vertices are shared, so the body looks exactly as it did. Pure arithmetic over Body.
	 */
	static void Cut(FMantleSkinnedMesh& Body, TArray<FSkeletalMaterial>& Materials, const FMantleVehicleLightGuess& Guess, FMantleVehicleSourceLights& OutLights);

	/** The body's section names, as the vehicle lists them (UniqueSectionNames of the slot names). */
	static TArray<FString> SectionNames(const TArray<FSkeletalMaterial>& Materials);

	/**
	 * The glowing copy of a light section's material: a Material Instance of Original (or of the instance's parent when Original is already one of these), saved
	 * beside the vehicle as MI_<Vehicle>_<Section>, with the glow the model switched off turned on and the kind recorded on it (KindOf). Null with OutError when it
	 * cannot be made or saved. A /Temp package is not written. Game thread.
	 */
	static UMaterialInterface* MakeLightMaterial(UMaterialInterface* Original, EMantleLightKind Kind, const FString& PackagePath, const FString& AssetName, FString& OutError);

	/** The kind a light material (MakeLightMaterial) glows as; unset for any other material. */
	static TOptional<EMantleLightKind> KindOf(const UMaterialInterface* Material);

	/** The material a light material was made from; Material itself when it is not one. */
	static UMaterialInterface* OriginalOf(UMaterialInterface* Material);

	/**
	 * Gives every light section of Source's body its glowing copy (MakeLightMaterial) and every other section back its own material, so the glow follows the rows.
	 * Notes say what could not be done. Game thread.
	 */
	static void ApplyMaterials(UMantleVehicleSource& Source, TArray<FString>& OutNotes);
};
