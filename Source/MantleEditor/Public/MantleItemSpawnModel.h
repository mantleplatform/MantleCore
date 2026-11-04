// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class UStaticMesh;

/**
 * The model of a Mantle Item Spawn (#235). The runtime actor holds it as a text, ModelId, a place asset id (ast_...). The Details
 * panel (FMantleItemSpawnDetails) lets a creator pick a Static Mesh instead and stores its object path in the same text; the
 * exporter turns the path into the mesh's asset id and uploads the mesh with the place. A text that is not a path is an asset
 * id as a place file gave it, and stays as it is.
 */
struct MANTLEEDITOR_API FMantleItemSpawnModel
{
	/** True when ModelId is a Static Mesh object path (what the Details panel stores) rather than a place file's asset id. */
	static bool IsMeshPath(const FString& ModelId);

	/** The object path the Details panel stores for Mesh; empty for none. */
	static FString PathOf(const UStaticMesh* Mesh);

	/** The Static Mesh a mesh path names, loaded; null for an asset id, an empty text or a path that is not a Static Mesh in the project. */
	static UStaticMesh* LoadMesh(const FString& ModelId);

	/**
	 * Why ModelId cannot publish, phrased to follow the spawn's name, or empty when it can: a path that is no Static Mesh, or a text
	 * that is neither a path nor an asset id (ast_ and 4 to 40 lowercase letters, digits or '_'). The mesh's own limits are
	 * FMantlePlaceAssets::CheckMesh's.
	 */
	static FString Problem(const FString& ModelId);
};
