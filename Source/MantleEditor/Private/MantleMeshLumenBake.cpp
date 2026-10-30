// Copyright Joshua Eger. All Rights Reserved.
#include "MantleMeshLumenBake.h"

#include "Components/StaticMeshComponent.h"
#include "DistanceFieldAtlas.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "MantleCosmeticExport.h"
#include "MantleGlbWriter.h"
#include "MantlePlaceValidator.h"
#include "MantleMeshLumenData.h"
#include "MantleRuntime.h"
#include "Materials/MaterialInterface.h"
#include "MeshCardBuild.h"
#include "MeshUtilities.h"
#include "Modules/ModuleManager.h"

FMantleMeshLumenSection FMantleMeshLumenBake::SectionFor(const UMaterialInterface* Material)
{
	FMantleMeshLumenSection Section;
	if (Material)
	{
		Section.BlendMode = Material->GetBlendMode();
		Section.bTwoSided = Material->IsTwoSided();
	}
	return Section;
}

bool FMantleMeshLumenBake::Bake(const FMantleGlbMesh& Mesh, const TConstArrayView<FMantleMeshLumenSection> Sections, const FString& MeshTag,
	TArray<uint8>& OutFile, FString& OutReason)
{
	OutFile.Reset();
	if (Mesh.Positions.Num() == 0 || Mesh.Primitives.Num() == 0)
	{
		OutReason = TEXT("has no triangles");
		return false;
	}

	// One FStaticMeshSection per primitive, its indices made absolute (a primitive's are local to its first vertex).
	FSourceMeshDataForDerivedDataTask Source;
	Source.VertexPositions = Mesh.Positions;
	TArray<FSignedDistanceFieldBuildSectionData> SectionData;
	FBox3f Bounds(ForceInit);
	for (const FVector3f& Position : Mesh.Positions)
	{
		Bounds += Position;
	}
	for (const FMantleGlbPrimitive& Primitive : Mesh.Primitives)
	{
		FStaticMeshSection& Section = Source.Sections.AddDefaulted_GetRef();
		Section.MaterialIndex = Primitive.MaterialIndex;
		Section.FirstIndex = Source.TriangleIndices.Num();
		Section.NumTriangles = Primitive.Indices.Num() / 3;
		Section.MinVertexIndex = Primitive.FirstVertex;
		Section.MaxVertexIndex = Primitive.FirstVertex + Primitive.NumVertices - 1;
		for (const uint32 Index : Primitive.Indices)
		{
			const uint32 Absolute = static_cast<uint32>(Primitive.FirstVertex) + Index;
			if (Absolute >= static_cast<uint32>(Mesh.Positions.Num()))
			{
				OutReason = TEXT("has an index out of range");
				return false;
			}
			Source.TriangleIndices.Add(Absolute);
		}
		const FMantleMeshLumenSection Material = Sections.IsValidIndex(Primitive.MaterialIndex) ? Sections[Primitive.MaterialIndex] : FMantleMeshLumenSection();
		FSignedDistanceFieldBuildSectionData& Data = SectionData.AddDefaulted_GetRef();
		Data.BlendMode = Material.BlendMode;
		Data.bTwoSided = Material.bTwoSided;
		Data.bAffectDistanceFieldLighting = true;
	}
	if (!SectionData.ContainsByPredicate([](const FSignedDistanceFieldBuildSectionData& Data) { return !IsTranslucentBlendMode(Data.BlendMode); }))
	{
		OutReason = TEXT("draws only with translucent materials, which Lumen does not trace");
		return false;
	}

	FMeshDataForDerivedDataTask MeshData;
	MeshData.SourceMeshData = &Source;
	MeshData.LODModel = nullptr;
	MeshData.SectionData = SectionData;
	MeshData.Bounds = FBoxSphereBounds3f(Bounds);

	IMeshUtilities& MeshUtilities = FModuleManager::Get().LoadModuleChecked<IMeshUtilities>(TEXT("MeshUtilities"));
	FDistanceFieldVolumeData DistanceField;
	MeshUtilities.GenerateSignedDistanceFieldVolumeData(Mesh.Name, MeshData, /*DistanceFieldResolutionScale*/ 1.0f, /*bGenerateAsIfTwoSided*/ false, DistanceField);
	if (!DistanceField.IsValid())
	{
		OutReason = TEXT("got no distance field (it is too thin or too small to have one)");
		return false;
	}
	FCardRepresentationData Cards;
	if (!MeshUtilities.GenerateCardRepresentationData(Mesh.Name, MeshData, &DistanceField, FMeshBuildSettings().MaxLumenMeshCards, /*bGenerateAsIfTwoSided*/ false, Cards))
	{
		OutReason = TEXT("got no Lumen cards");
		return false;
	}
	OutFile = FMantleMeshLumenFile::Write(MeshTag, DistanceField, Cards);
	if (OutFile.Num() == 0 || OutFile.Num() > FMantleMeshLumenFile::MaxBytes)
	{
		OutReason = OutFile.Num() == 0 ? FString(TEXT("could not be written"))
			: FString::Printf(TEXT("is %.1f MB, over the %lld MB a Lumen file may be"), OutFile.Num() / (1024.0 * 1024.0), FMantleMeshLumenFile::MaxBytes >> 20);
		OutFile.Reset();
		return false;
	}
	return true;
}

bool FMantleMeshLumenBake::LevelAsksForLumen(UWorld* World)
{
	if (!World)
	{
		return false;
	}
	bool bOverridden = false;
	for (TActorIterator<APostProcessVolume> It(World); It; ++It)
	{
		const FPostProcessSettings& Settings = It->Settings;
		if (It->bEnabled && Settings.bOverride_DynamicGlobalIlluminationMethod)
		{
			if (Settings.DynamicGlobalIlluminationMethod == EDynamicGlobalIlluminationMethod::Lumen)
			{
				return true;
			}
			bOverridden = true;
		}
	}
	static const IConsoleVariable* Method = IConsoleManager::Get().FindConsoleVariable(TEXT("r.DynamicGlobalIlluminationMethod"));
	return !bOverridden && Method && Method->GetInt() == static_cast<int32>(EDynamicGlobalIlluminationMethod::Lumen);
}

void FMantleMeshLumenBake::CheckLevel(UWorld* World, TArray<FMantleValidationIssue>& OutIssues)
{
	if (!LevelAsksForLumen(World))
	{
		return;
	}
	// Each distinct mesh once, named by the first actor that draws it.
	TMap<const UStaticMesh*, FString> Meshes;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		TInlineComponentArray<UStaticMeshComponent*> Components(*It);
		for (const UStaticMeshComponent* Component : Components)
		{
			if (const UStaticMesh* Mesh = Component ? Component->GetStaticMesh() : nullptr; Mesh && !Meshes.Contains(Mesh))
			{
				Meshes.Add(Mesh, It->GetActorNameOrLabel());
			}
		}
	}
	int32 Without = 0;
	for (const TPair<const UStaticMesh*, FString>& Pair : Meshes)
	{
		FMantleGlbMesh Glb;
		TArray<int32> Slots;
		int32 Triangles = 0;
		FString Why;
		if (!FMantleCosmeticExport::ReadStaticMesh(Pair.Key, 0, Glb, Slots, Triangles, Why))
		{
			continue; // the mesh checks say what is wrong with it
		}
		TArray<FMantleMeshLumenSection> Sections;
		for (const int32 Slot : Slots)
		{
			const TArray<FStaticMaterial>& Materials = Pair.Key->GetStaticMaterials();
			Sections.Add(SectionFor(Materials.IsValidIndex(Slot) ? Materials[Slot].MaterialInterface.Get() : nullptr));
		}
		TArray<uint8> File;
		if (Bake(Glb, Sections, FString(TEXT("sha256:check")), File, Why))
		{
			continue;
		}
		++Without;
		FMantleValidationIssue& Issue = OutIssues.AddDefaulted_GetRef();
		Issue.Severity = EMantleIssueSeverity::Warning;
		Issue.Actor = Pair.Value;
		Issue.Problem = FString::Printf(TEXT("This level lights with Lumen, but mesh '%s' gets no distance field for it: it %s. In Mantle, Lumen does not see it, so it neither bounces light nor shades what is behind it."),
			*Pair.Key->GetName(), *Why);
		Issue.Remedy = TEXT("If it should take part in the lighting, give it an opaque material or more thickness. A glass pane, decal card or effect mesh can stay as it is.");
	}
	UE_LOG(LogMantle, Log, TEXT("MANTLE_CHECK_LUMEN level asks for Lumen: %d distinct mesh(es), %d without Lumen data"), Meshes.Num(), Without);
}
