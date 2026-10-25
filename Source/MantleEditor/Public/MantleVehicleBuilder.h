// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleChassisSpec.h"
#include "MantleVehicleChassisSkin.h"
#include "MantleVehicleFit.h"

class AMantleVehicleSpawnPoint;
class UMantleVehicleSource;
class UObject;
class USkeletalMesh;
struct FMantleSkinnedMesh;
struct FMantleVehicleSourcePart;
struct FSkeletalMaterial;

/** What the vehicle builder made of a model and a vehicle type (#728), in plain words for a creator who does not know Unreal. */
struct FMantleVehicleBuildResult
{
	/** The vehicle asset, made beside the model and saved (see bSaved); null when nothing could be made. */
	UMantleVehicleSource* Source = nullptr;

	/** For bOnChassis: the name the vehicle and its body and skeleton were saved under (<Name>_Vehicle, SKM_<Name>, SK_<Name>); a build again with this name updates them. */
	FString AssetName;

	/** What stopped the build, each sentence saying what to do. */
	TArray<FString> Problems;

	/** What Mantle chose, measured or set, and what to look at before publishing. */
	TArray<FString> Notes;

	/**
	 * What sizing the vehicle for the Mantle character found, from its wheels. Unset when the wheels could not be read. It is the
	 * proposal even when the creator's own Size was kept, so the window can offer to go back to it.
	 */
	TOptional<FMantleVehicleFit> Fit;

	/** True when the vehicle's package was saved to disk by the build; false for a transient test package or a failed save. */
	bool bSaved = false;

	/** Why the automatic save failed, in plain words; empty when it worked or when there was nothing to save (a scratch vehicle). */
	FString SaveProblem;

	/** True when the vehicle was made and the type's settings were applied. */
	bool bOk = false;

	/**
	 * True when the vehicle is the creator's model worn as a skin on the type's own chassis (BuildFromModel, #821): its Size is the chassis's, set when it was
	 * built, and a Size changed later means a build again. False for a model whose own wheels the chassis was fitted to (Build).
	 */
	bool bOnChassis = false;
	/** For bOnChassis: the turn that put the model's nose on +X and the parts of the model that were assigned to the chassis's moving bones. */
	double ModelYaw = 0.0;
	FMantleVehicleModelMap ModelMap;
};

/** How a creator's model goes onto a type's chassis (BuildFromModel, #821). Everything left unset is Mantle's proposal. */
struct FMantleVehicleModelOptions
{
	/** The turn about Z that puts the model's nose on +X; unset, Mantle works it out from the model's wheels or rotors and otherwise leaves it as modelled. */
	TOptional<double> YawDegrees;
	/** The parts of the model assigned to the chassis's moving bones; unset, Mantle's suggestions (FMantleVehicleChassisSkin::SuggestMap). */
	TOptional<FMantleVehicleModelMap> Map;
	/**
	 * A propeller plane with two engines (a P-38): the chassis gets a propeller bone each (FMantleVehicleChassisSkin::AsTwin). Unset, it is when the model has two
	 * propeller bones. Ignored when Spec is given: that chassis is the one used.
	 */
	TOptional<bool> TwinEngine;
	/** The chassis to put the model on, already shaped (the window's); unset, the type's own sized to the model (FMantleVehicleChassisSkin::Propose). */
	TOptional<FMantleChassisSpec> Spec;
	/** The folder (a long package path) the vehicle is saved under, one subfolder of its name inside; empty for the project's MantleVehicles folder. */
	FString Folder;
	/** The vehicle's name; empty for the model's name and the type's, the first one not taken. */
	FString Name;
	/**
	 * Replace the vehicle already saved under Name. A name given for a vehicle that is already there is refused unless this is set, so the console's name= can't overwrite a
	 * vehicle the creator made some other way; the window sets it for the vehicle it made itself, which is how pressing Build again updates one vehicle instead of adding another.
	 */
	bool bOverwrite = false;
};

/**
 * The logic behind Tools > Mantle > Build a vehicle (#728), apart from Slate so tests can run it. It is the Convert Vehicle
 * command plus a vehicle type: the model is converted as FMantleVehicleConverter::ConvertAsset does, then the type's starting
 * settings, and a suggested Size (FitScale), are written into the new vehicle.
 */
struct MANTLEEDITOR_API FMantleVehicleBuilder
{
	/**
	 * Builds a vehicle from Mesh using the preset PresetId. Converting a model that already has a <Mesh>_Vehicle updates that
	 * asset in place (ConvertAsset keeps its tuning, parts and paint), after which the preset's defaults are written over its
	 * tuning again, so building twice with the same type gives the type's starting settings, not the creator's edits.
	 *
	 * The model is only a skin (#728 step 2): when the type's defaults are applied, the collision (one box), centre of mass, seats and
	 * steering wheel are all the type's chassis (FMantleVehicleChassisFit), stretched to the model's wheel layout and written in the
	 * model's units; nothing the converter read from the model's physics survives. Fails, changing nothing about the type, when the
	 * model's wheels cannot be found.
	 */
	static FMantleVehicleBuildResult Build(USkeletalMesh* Mesh, const FString& PresetId);

	/**
	 * True when Model is a skeletal mesh whose own wheels (bones named like Wheel_FL) a car type's chassis can be fitted to, which Build does. Not a motorcycle's
	 * (its model is worn as a skin, with the wheels assigned), not a plane's, a helicopter's or a hovercraft's, and not a static mesh.
	 */
	static bool FitsOwnWheels(UObject* Model, const FString& PresetId);

	/**
	 * What the window's Build button does (#821): Build when the model's own wheels fit the type (FitsOwnWheels), else BuildFromModel with Options, so every model
	 * of every type becomes a vehicle: a car pack with corner-named wheels is fitted to its wheels, anything else is worn as a skin on the chassis.
	 */
	static FMantleVehicleBuildResult BuildAny(UObject* Model, const FString& PresetId, const FMantleVehicleModelOptions& Options = FMantleVehicleModelOptions());

	/**
	 * Chassis from a model (#821 WP5, #793 step 5): the creator's static or skeletal Model worn whole as the skin of PresetId's own chassis. The chassis is the
	 * physics and the skeleton (its bones are wheels, control surfaces, propellers or nozzle, rotors, fork), its numbers the type's at the Size or sizes proposed from
	 * the model's bounds; the model is skinned to the chassis's root bone, with the parts the creator assigned (Options.Map) riding the moving bones, so an unassigned
	 * model still drives or flies and only fails to animate those parts. Saved under Options.Folder (or MantleVehicles), as BuildChassis saves a grey chassis.
	 *
	 * Takes any model: a spaceship's static mesh (no wheels) is a jet, a plane's skeletal mesh with its own bones is a plane, two wheels on the centre line a
	 * motorcycle, a wheel-less cabin a helicopter. The result has bOnChassis set. Problems say, in plain words, what stopped it. One undo step ("Build vehicle from model"),
	 * as Build is.
	 */
	static FMantleVehicleBuildResult BuildFromModel(UObject* Model, const FString& PresetId, const FMantleVehicleModelOptions& Options = FMantleVehicleModelOptions());

	/**
	 * The "This model has no inside" choice: hides or shows the driver and the passenger together, as one undo step. Hiding them on a
	 * vehicle whose driver seat is not placed places it where the cook would (DefaultDriverSeat), so the document carries the seat
	 * the exits and camera hang from. Nothing happens when the choice is already so.
	 */
	static void SetHiddenRiders(UMantleVehicleSource& Source, const USkeletalMesh* Model, bool bHide);

	/**
	 * Where the driver sits when Source has no seat placed. A vehicle of a type gets its chassis's seat fitted to the model's wheels,
	 * as Build places it (a built vehicle always has its seat, so this is for older ones). With no type, where the cook puts the driver
	 * (vehicle.go, "Behind the middle, on the left"): a tenth of the length behind the middle, a fifth of the width to the left of it
	 * and 45% of the height up, in the model's bounds.
	 */
	static FVector DefaultDriverSeat(const USkeletalMesh& Model, const UMantleVehicleSource& Source);

	/**
	 * True for a vehicle that lives only in memory or in a scratch package (a test's), which must never be written to disk and so
	 * is neither saved for the creator nor driven.
	 */
	static bool IsScratchVehicle(const UMantleVehicleSource& Source);

	/**
	 * Saves Source's package so the creator does not have to, silently (no dialog). True when it was saved. False with OutProblem empty
	 * for a scratch vehicle (nothing to save); False with OutProblem in plain words when the save failed.
	 */
	static bool SaveVehicle(UMantleVehicleSource& Source, FString& OutProblem);

	/**
	 * True when Source's body model has unsaved changes or no file on disk, so the vehicle would lose its body when the editor
	 * restarts. Mantle never saves the creator's model for them. Mesh, when given, is the model to check; otherwise the vehicle's Body.
	 */
	static bool IsModelUnsaved(const UMantleVehicleSource& Source, const USkeletalMesh* Mesh = nullptr);

	/** The note that tells a creator to save their model too. */
	static FString ModelUnsavedNote();

	/**
	 * The Put it in my level button: adds a Mantle Vehicle Spawn for Source to the level open in the editor, about 6 m in front of the
	 * level viewport's camera (an orthographic view is not used; the first perspective viewport is, else the world origin), dropped onto the ground below it and facing the way the camera looks, as one undo step ("Put vehicle in
	 * level"), and selects it. The creator then presses Test Locally. False, with OutProblem in plain words, when no level is open.
	 * OutActor, when given, receives the new spawn.
	 *
	 * A plane or jet needs a long flat stretch to take off from: it is put where the ground ahead of the camera is flattest for its take-off run (about 150 m for a jet, 350 m for
	 * a plane), not necessarily 6 m ahead. OutNote, when given, says in plain words where it went and what a flier needs: the run it needs and whether the spot has it, or that a
	 * helicopter needs open space above. Empty for a vehicle that drives.
	 */
	static bool PlaceInLevel(UMantleVehicleSource& Source, FString& OutProblem, AMantleVehicleSpawnPoint** OutActor = nullptr, FString* OutNote = nullptr);

	/** The project folder vehicles made with no model are saved under: "/Game/MantleVehicles", one folder for each vehicle. */
	static FString ChassisFolder();

	/**
	 * A name for a new vehicle made with no model that no vehicle under ChassisFolder has yet: Wanted made safe to save under ("Sedan"), then with
	 * "_2", "_3" and so on added until it is free.
	 */
	static FString FirstFreeChassisName(const FString& Wanted);

	/** FirstFreeChassisName with the check for a taken name given, so it can be tested without a project: IsTaken says whether a vehicle of that name is already saved. */
	static FString FirstFreeName(const FString& Wanted, TFunctionRef<bool(const FString&)> IsTaken);

	/**
	 * The vehicle of a chassis with no creator model (#793 step 4): the chassis's grey body saved as a skeletal mesh (FMantleVehicleChassisMesh), and a
	 * vehicle asset on it whose wheels, tuning, collision, centre of mass and seats are the chassis's (its handling derived from its sizes when it was
	 * shaped), saved the way Build saves one. Made as <Name>_Vehicle in PackagePath beside SKM_<Name> and SK_<Name>; a vehicle already there is
	 * updated in place, so a spawn in the level that names it follows (Size 1; its paint, liveries and parts are kept). Press Put it in my level
	 * (PlaceInLevel) on the result, then Test Locally.
	 *
	 * A chassis that was shaped (its Derived handling set) carries no preset: its numbers are its own, held to the document's limits and not to a
	 * preset's narrower ranges. An unshaped one is its preset, hand-tuned. A path under /Temp is a test's: nothing is registered or written to disk.
	 * Result.Source is the vehicle; Problems say, in plain words, what stopped it.
	 */
	static FMantleVehicleBuildResult BuildChassis(const FMantleChassisSpec& Spec, const FString& PackagePath, const FString& Name);

	/**
	 * BuildChassis with a body of its own (#821): the chassis's skeleton wearing a creator's model, from FMantleVehicleChassisSkin::Skin, and the model's material
	 * slots. The vehicle asset is the chassis's, the body is Body.
	 *
	 * Collision, when given and not empty (FMantleVehicleChassisSkin::ProposeCollision), is the vehicle's collision in place of the chassis's one box: its first box is
	 * the one the driver sits in, the rest are added beside it. Extras are the model's other UV channels and vertex colours, written onto the body with its own tangents.
	 *
	 * Parts (#576): when given (even empty), they become the vehicle's Parts, replacing any a build before it made: the pieces of a model that drew more materials than a
	 * published body may (FMantleVehicleBodySplit). PictureBody and PictureMesh are then the whole model, for the library card's picture, which Body (without those pieces) would miss.
	 */
	static FMantleVehicleBuildResult BuildChassisWithBody(const FMantleChassisSpec& Spec, const FString& PackagePath, const FString& Name, const FMantleSkinnedMesh& Body,
		const TArray<FSkeletalMaterial>& Materials, const TArray<FMantleChassisBox>* Collision = nullptr, const FMantleVehicleBodyExtras* Extras = nullptr,
		const TArray<FMantleVehicleSourcePart>* Parts = nullptr, const FMantleSkinnedMesh* PictureBody = nullptr, USkeletalMesh* PictureMesh = nullptr);

	/** The spawn in the level open in the editor that already parks Source, or null. */
	static AMantleVehicleSpawnPoint* FindSpawn(const UMantleVehicleSource& Source);

	/** The project folder .fbx files picked in the window are imported under: "/Game/MantleVehicles/Imported", one subfolder for each file. */
	static FString ImportFolder();

	/**
	 * Imports an .fbx file as the editor's Import does (a Static Mesh, or a Skeletal Mesh when the file has a skinned mesh), without a dialog, so the window can offer "Pick an
	 * .fbx file" to a creator who has not imported it yet. Into Folder (a long package path), else ImportFolder() / <the file's name>; an earlier import of the same file is
	 * replaced. Animations and a physics asset are not made: the vehicle's body is only the mesh. bSave false keeps the new assets in memory (a test's).
	 *
	 * Returns the model: the first Skeletal Mesh the file made, else the first Static Mesh. Null, with OutError in plain words, for a file that is missing or is not an .fbx,
	 * or one with no mesh in it. OutAll, when given, receives every object the import made, so a test can take them away again.
	 */
	static UObject* ImportModelFile(const FString& FbxFile, FString& OutError, const FString& Folder = FString(), bool bSave = true, TArray<UObject*>* OutAll = nullptr);
};
