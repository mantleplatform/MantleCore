// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleVehicleConverter.h"

class UBlueprint;
class UObject;

/** What a vehicle Blueprint gave the builder (FMantleVehicleBlueprint::Resolve). */
struct FMantleVehicleBlueprintModel
{
	/**
	 * The model to build from: the Blueprint's skeletal mesh (with the static pieces attached to it joined in, when it has any: PiecesJoined), or one Static Mesh (the
	 * Blueprint's only piece, or its pieces merged). Null when Problems says why not.
	 */
	UObject* Model = nullptr;

	/**
	 * For a Blueprint with a skeletal body (a Chaos vehicle): what the converter reads off the vehicle, the parts attached to its body, its physics and its wheel setups
	 * (FMantleVehicleConverter::SurroundingsOfComponent). The pieces joined into Model are not listed in Attached any more. Unset for a Blueprint made of static pieces.
	 */
	TOptional<FMantleVehicleSurroundings> Around;

	/** True when several static pieces were merged into one Static Mesh (Model is that mesh). */
	bool bMerged = false;

	/** For a skeletal body: how many of the static pieces attached to it were joined into Model (FMantleVehicleBodyJoin); 0 when Model is the Blueprint's own mesh. */
	int32 PiecesJoined = 0;

	/** The assets a merge or a join made (the mesh and its skeleton), so a test can take them away again. */
	TArray<UObject*> MadeAssets;

	/** What Mantle did, in plain words for a creator who does not know Unreal. */
	TArray<FString> Notes;

	/** What stopped it, each sentence saying what to do. */
	TArray<FString> Problems;
};

/**
 * A vehicle Blueprint as a builder source (vehicle builder flow plan, step 4). Two kinds work:
 * - a Chaos vehicle (or any Blueprint with a skeletal mesh body): its skeletal mesh is the model, and the wheels, mass and attached parts on the Blueprint ride along
 *   exactly as Convert Vehicle reads them off a vehicle placed in the level. When the car is static meshes attached to a body of bones (its shell, doors, seats), those
 *   pieces are joined into the body as one skeletal model (FMantleVehicleBodyJoin), saved like a merge; tires and other pieces on a wheel stay attached for the converter;
 * - a Blueprint made of several static mesh pieces (a modular spaceship): the pieces are merged into one Static Mesh with the editor's own mesh merging (what
 *   Merge Actors does) and saved under MantleVehicles > Imported, each piece keeping its material.
 */
struct MANTLEEDITOR_API FMantleVehicleBlueprint
{
	/** The project folder merged meshes are saved under: the same one imported files go to (FMantleVehicleBuilder::ImportFolder), a subfolder for each Blueprint. */
	static FString MergeFolder();

	/**
	 * Reads Blueprint by putting one in a scratch world, as the level would have it once placed, and taking its meshes. Nothing is left in the level.
	 * bSave false keeps a merged mesh in memory (a test's). The mesh is saved under Folder, else MergeFolder() / <the Blueprint's name>.
	 */
	static FMantleVehicleBlueprintModel Resolve(UBlueprint* Blueprint, const FString& Folder = FString(), bool bSave = true);
};
