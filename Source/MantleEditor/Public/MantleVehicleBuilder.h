// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleChassisSpec.h"
#include "MantleVehicleChassisSkin.h"
#include "MantleVehicleFit.h"
#include "MantleVehicleAsset.h"
#include "MantleVehicleSource.h"
#include "UObject/StrongObjectPtr.h"

class AMantleVehicleSpawnPoint;
class UMantleVehicleSource;
class UObject;
class USkeletalMesh;
struct FMantleSkinnedMesh;
struct FMantleVehicleSurroundings;
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
	/** For bOnChassis: the body had more triangles than a published vehicle may have, so Mantle reduced it (the picture of the model shows the full one). */
	bool bBodyReduced = false;
	/** For bOnChassis: the turn that put the model's nose on +X and the parts of the model that were assigned to the chassis's moving bones. */
	double ModelYaw = 0.0;
	FMantleVehicleModelMap ModelMap;
};

/** How a creator's model goes onto a type's chassis (BuildFromModel, #821). Everything left unset is Mantle's proposal. */
struct FMantleVehicleModelOptions
{
	/** The turn about Z that puts the model's nose on +X; unset, Mantle works it out from the model's wheels or rotors and otherwise leaves it as modeled. */
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
	/**
	 * The lights: which body sections glow as which light, and the headlight beams (FMantleVehicleLights). Unset, what the light finder found on the model
	 * (FMantleVehicleOnChassis::Lights); the window sets it once the creator changes a row or moves a beam.
	 */
	TOptional<FMantleVehicleSourceLights> Lights;
	/**
	 * The space Lights' beams were placed in: the bounds of the window's picture's body. A model that keeps its own wheels (Build) is built in its own space, and the beams
	 * are moved to the same place on it. Invalid: the beams are in the built body's space already.
	 */
	FBox3f LightsBounds = FBox3f(ForceInit);
};

/**
 * Everything BuildFromModel works out from a model, a vehicle type and the creator's choices before it saves anything (FMantleVehicleBuilder::PrepareFromModel): the
 * chassis, the model worn on it, its moving parts and collision. The window's picture and the real build both come from this, so they cannot disagree.
 */
struct FMantleVehicleOnChassis
{
	/** The type's name ("Sedan"), for the sentences about it. */
	FString PresetName;
	FMantleVehicleModel Model;
	/** The chassis the model is worn on: the window's own when Options.Spec was given, else the type's sized to the model. */
	FMantleChassisSpec Spec;
	/** The chassis's skeleton at that size (Body is the gray one, never shown). WheelPositions are where Mantle thinks each wheel is, in the body's space, cm. */
	FMantleVehicleAsset Chassis;
	TArray<FMantleVehiclePartSlot> Slots;
	FMantleVehicleModelMap Map;
	double Yaw = 0.0;
	FMantleVehicleModelPlacement Placement;
	/** The model skinned to the chassis's bones. */
	FMantleVehicleSkinned Skinned;
	FMantleVehicleSkinCollision Collision;
	/**
	 * The lights the finder found on the model (FMantleVehicleLights::Find): their triangles are already cut into sections of their own in Skinned, and this names
	 * them by kind, with the headlight beams. Empty lists for a model with no glow and no light named.
	 */
	FMantleVehicleSourceLights Lights;
	/** What Mantle chose and what the creator should look at, in the order BuildFromModel has always said it. */
	TArray<FString> Notes;
};

/**
 * The window's picture of a model on its vehicle's frame, made in memory and never saved: the creator's model (its own materials) worn on the chassis, and the spots Mantle
 * puts the wheels (FMantleVehicleBuilder::MakeModelPreview). It is the same vehicle BuildFromModel builds. Give it up (Discard, or its destructor) when the picture is replaced.
 */
struct MANTLEEDITOR_API FMantleVehicleModelPreview
{
	FMantleVehicleModelPreview() = default;
	FMantleVehicleModelPreview(const FMantleVehicleModelPreview&) = delete;
	FMantleVehicleModelPreview& operator=(const FMantleVehicleModelPreview&) = delete;
	~FMantleVehicleModelPreview() { Discard(); }

	/** The whole model on the chassis's skeleton, in a throwaway package. Null when bOk is false. */
	TStrongObjectPtr<USkeletalMesh> Mesh;
	/** The model's body as skinned, for drawing without a renderer. */
	FMantleSkinnedMesh Body;
	/** Where Mantle thinks each wheel is: the mesh's space, cm. A dot goes on each. */
	TArray<FVector> WheelDots;
	/** The chassis the model is on, with the vehicle's size, seat and weight. */
	FMantleChassisSpec Spec;
	FMantleVehicleModelMap Map;
	double Yaw = 0.0;
	/** The lights Mantle found (FMantleVehicleOnChassis::Lights), and every section of the body by the names the vehicle lists, for the lights' rows. */
	FMantleVehicleSourceLights Lights;
	TArray<FString> Sections;
	TArray<FString> Notes;
	TArray<FString> Problems;
	bool bOk = false;

	/** Takes the picture's mesh and skeleton away. */
	void Discard();
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
	 * The model is only a skin (#728 step 2): when the type's defaults are applied, the collision (one box), center of mass, seats and
	 * steering wheel are all the type's chassis (FMantleVehicleChassisFit), stretched to the model's wheel layout and written in the
	 * model's units; nothing the converter read from the model's physics survives. Fails, changing nothing about the type, when the
	 * model's wheels cannot be found. Around, when given, is what the converter reads off the vehicle the mesh came from (a vehicle Blueprint's parts, wheel setups and
	 * mass: FMantleVehicleConverter::SurroundingsOfComponent), else the mesh's own preview attachments and physics.
	 */
	static FMantleVehicleBuildResult Build(USkeletalMesh* Mesh, const FString& PresetId, const FMantleVehicleSurroundings* Around = nullptr);

	/**
	 * True when Model is a skeletal mesh whose own wheels (bones named like Wheel_FL) a car type's chassis can be fitted to, which Build does. Not a motorcycle's
	 * (its model is worn as a skin, with the wheels assigned), not a plane's, a helicopter's or a hovercraft's, not a static mesh, and not a model over the published
	 * limits (more triangles or materials than a vehicle may have): that one is worn as a skin, reduced and with its extra materials carried as parts.
	 */
	static bool FitsOwnWheels(UObject* Model, const FString& PresetId);

	/**
	 * What the window's Build button does (#821): Build when the model's own wheels fit the type (FitsOwnWheels), else BuildFromModel with Options, so every model
	 * of every type becomes a vehicle: a car pack with corner-named wheels is fitted to its wheels, anything else is worn as a skin on the chassis.
	 *
	 * Model may also be a vehicle Blueprint (FMantleVehicleBlueprint::Resolve): a Chaos vehicle builds from its skeletal body with the wheels and parts set up on it,
	 * a Blueprint of several static pieces is merged into one Static Mesh saved under MantleVehicles > Imported, and the build goes on from that. The result's Notes
	 * say what was done, including that pieces which move become one solid piece.
	 */
	static FMantleVehicleBuildResult BuildAny(UObject* Model, const FString& PresetId, const FMantleVehicleModelOptions& Options = FMantleVehicleModelOptions());

	/**
	 * Chassis from a model (#821 WP5, #793 step 5): the creator's static or skeletal Model worn whole as the skin of PresetId's own chassis. The chassis is the
	 * physics and the skeleton (its bones are wheels, control surfaces, propellers or nozzle, rotors, fork), its numbers the type's at the Size or sizes proposed from
	 * the model's bounds; the model is skinned to the chassis's root bone, with the parts the creator assigned (Options.Map) riding the moving bones, so an unassigned
	 * model still drives or flies and only fails to animate those parts. Saved under Options.Folder (or MantleVehicles).
	 *
	 * Takes any model: a spaceship's static mesh (no wheels) is a jet, a plane's skeletal mesh with its own bones is a plane, two wheels on the center line a
	 * motorcycle, a wheel-less cabin a helicopter. The result has bOnChassis set. Problems say, in plain words, what stopped it. One undo step ("Build vehicle from model"),
	 * as Build is.
	 */
	static FMantleVehicleBuildResult BuildFromModel(UObject* Model, const FString& PresetId, const FMantleVehicleModelOptions& Options = FMantleVehicleModelOptions());

	/**
	 * The part of BuildFromModel that decides how the model sits on the vehicle, with nothing saved: reads the model, picks the chassis (Options.Spec or the type's sized to
	 * the model), the model's facing and moving parts, wears the model on the chassis and shapes its collision. False with Problems in plain words when it could not.
	 * BuildFromModel and MakeModelPreview both start here, which is what keeps the picture and the build the same vehicle.
	 */
	static bool PrepareFromModel(UObject* Model, const FString& PresetId, const FMantleVehicleModelOptions& Options, FMantleVehicleOnChassis& Out, TArray<FString>& Problems);

	/**
	 * The picture of the model on its vehicle's frame (#821 flow, step 2): PrepareFromModel, then the model's whole body as a mesh in a throwaway package (/Temp, never saved,
	 * never registered) and a dot for each wheel. Nothing is added to the project. Options.Name, Folder and bOverwrite are ignored. Fills Out; Problems say what stopped it.
	 */
	static bool MakeModelPreview(UObject* Model, const FString& PresetId, const FMantleVehicleModelOptions& Options, FMantleVehicleModelPreview& Out);

	/**
	 * Whether Preview (MakeModelPreview) and Built (BuildFromModel with the same model, type and options) are the same vehicle: the same wheel spots (the built vehicle's wheel
	 * bones against the dots), and, when the build split nothing off the body, the same body (vertex count and bounds). False with Why in plain words for the first difference.
	 */
	static bool PreviewMatchesBuild(const FMantleVehicleModelPreview& Preview, const FMantleVehicleBuildResult& Built, FString& Why);

	/**
	 * The "This model has no inside" choice: hides or shows the driver and the passenger together, as one undo step. Hiding them on a
	 * vehicle whose driver seat is not placed places it where the cook would (DefaultDriverSeat), so the document carries the seat
	 * the exits and camera hang from. Nothing happens when the choice is already so.
	 */
	static void SetHiddenRiders(UMantleVehicleSource& Source, const USkeletalMesh* Model, bool bHide);

	/**
	 * Sets a built vehicle's lights (FMantleVehicleLights): the sections that glow as each light, kept only where the body has them, and the headlight beams. Each light
	 * section gets its glowing copy of its material and every other section its own material back; a light section is taken off the paint. Notes say what could not be done.
	 */
	static void ApplyLights(UMantleVehicleSource& Source, const FMantleVehicleSourceLights& Lights, TArray<FString>& OutNotes);

	/**
	 * The lights of a vehicle built on its own wheels (Build): its body is the creator's model, which is never changed, so when the light finder cuts spots out of it the
	 * cut body is a copy saved beside the vehicle (<Model>_Lights) and becomes the vehicle's body. Options.Lights, when set, are the creator's rows and beams (the beams moved
	 * from Options.LightsBounds onto the model); else what the finder found. Saves the vehicle again when the build saved it.
	 */
	static void LightOwnBody(FMantleVehicleBuildResult& Built, const FMantleVehicleModelOptions& Options);

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

	/** The project folder built vehicles are saved under: "/Game/MantleVehicles", one folder for each vehicle. */
	static FString ChassisFolder();

	/**
	 * A name for a new vehicle that is free: Wanted made safe to save under ("Sedan"), then with "_2", "_3" and so on added until IsTaken says it is free. IsTaken is
	 * given so it can be tested without a project: it says whether something of that name is already saved.
	 */
	static FString FirstFreeName(const FString& Wanted, TFunctionRef<bool(const FString&)> IsTaken);

	/**
	 * The vehicle of a frame wearing a creator's model (#821), from FMantleVehicleChassisSkin::Skin, and the model's material slots, saved the way Build saves one.
	 * Made as <Name>_Vehicle in PackagePath beside SKM_<Name>; a vehicle already there is updated in place, so a spawn in the level that names it follows. The vehicle
	 * asset's wheels, tuning, collision, center of mass and seats are the frame's, the body is Body. A path under /Temp is a test's: nothing is registered or written to disk.
	 *
	 * Collision, when given and not empty (FMantleVehicleChassisSkin::ProposeCollision), is the vehicle's collision in place of the chassis's one box: its first box is
	 * the one the driver sits in, the rest are added beside it. Extras are the model's other UV channels and vertex colors, written onto the body with its own tangents.
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
