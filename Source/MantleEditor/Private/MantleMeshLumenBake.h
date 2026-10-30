// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

struct FMantleContentHash;

/**
 * #149 step 5: bakes a place mesh's Lumen side file (FMantleMeshLumenFile, MLD1) at publish, from the cooked MRM2
 * the client will build (so a merged, mirrored, spline-baked or reduced mesh gets data for the geometry it ships,
 * not for its source asset). The mesh is built as the client builds it (FMantleRuntimeStaticMeshBuilder, LOD0, no
 * render resources) and handed to the editor's MeshUtilities: GenerateSignedDistanceFieldVolumeData, then
 * GenerateCardRepresentationData from that field, with the engine's defaults (resolution scale 1, 12 cards). Every
 * section bakes as opaque and one-sided: the MRM2 does not carry its materials' blend mode or two-sidedness.
 */
class FMantleMeshLumenBake
{
public:
	/** False, with the reason, when the mesh cannot be read or has no distance field (too thin or empty). Game thread. */
	static bool Bake(TConstArrayView<uint8> CookedMesh, const FMantleContentHash& MeshHash, const FString& Name, TArray<uint8>& OutFile, FString& OutReason);
};
