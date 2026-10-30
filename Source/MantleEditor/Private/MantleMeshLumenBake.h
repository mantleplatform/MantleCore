// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"

struct FMantleGlbMesh;
struct FMantleValidationIssue;
class UWorld;

/** How one section of a mesh takes part in the bake: its material's blend mode and two-sidedness. */
struct FMantleMeshLumenSection
{
	EBlendMode BlendMode = BLEND_Opaque;
	bool bTwoSided = false;
};

/**
 * #149: bakes a place mesh's Lumen side file (FMantleMeshLumenFile, MLD1) at publish, from the LOD0 the mesh upload
 * carries (FMantleGlbMesh, in Unreal space: the cook's gltfPointToUnreal undoes FMantleGlbWriter::ToGltfPosition exactly,
 * so these are the positions the client builds). A merged, mirrored, spline-baked or reduced mesh gets data for the
 * geometry it ships, not for its source asset. The editor's MeshUtilities does the work: GenerateSignedDistanceFieldVolumeData,
 * then GenerateCardRepresentationData from that field, with the engine's defaults (resolution scale 1, 12 cards). Each
 * section bakes with its material's blend mode and two-sidedness, as the engine's own build does
 * (BuildSignedDistanceFieldBuildSectionData): a translucent section is left out, a two-sided one (foliage, thin cards) is
 * solid from both sides.
 */
class FMantleMeshLumenBake
{
public:
	/**
	 * Sections: one per glTF material of Mesh (FMantleGlbPrimitive::MaterialIndex), opaque one-sided where missing.
	 * MeshTag: the mesh upload's hash (FMantleConvertedMesh::Hash). False, with the reason, when the mesh has no distance
	 * field (every section translucent, or too thin or empty) or no cards. Game thread.
	 */
	static bool Bake(const FMantleGlbMesh& Mesh, TConstArrayView<FMantleMeshLumenSection> Sections, const FString& MeshTag, TArray<uint8>& OutFile, FString& OutReason);

	/** A material's part in the bake: its blend mode and IsTwoSided, opaque one-sided for none. */
	static FMantleMeshLumenSection SectionFor(const class UMaterialInterface* Material);

	/**
	 * Whether the level lights with Lumen: a post process volume in it sets Global Illumination to Lumen, or none sets it and
	 * the project's default (r.DynamicGlobalIlluminationMethod) is Lumen.
	 */
	static bool LevelAsksForLumen(UWorld* World);

	/**
	 * Check this place (#149): in a level that asks for Lumen, a warning for each distinct static mesh that would publish
	 * with no Lumen data, saying why (Bake's reason). The bake runs for real, from each mesh's LOD0 and its slots' materials.
	 */
	static void CheckLevel(UWorld* World, TArray<FMantleValidationIssue>& OutIssues);
};
