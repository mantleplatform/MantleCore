// Copyright Joshua Eger. All Rights Reserved.
#include "MantleMeshLumenBake.h"

#include "DistanceFieldAtlas.h"
#include "Engine/StaticMesh.h"
#include "MantleAssetStore.h"
#include "MantleMeshLumenData.h"
#include "MantleRuntimeStaticMeshBuilder.h"
#include "MantleStaticMeshReader.h"
#include "MeshCardBuild.h"
#include "MeshUtilities.h"
#include "Modules/ModuleManager.h"
#include "StaticMeshResources.h"
#include "UObject/StrongObjectPtr.h"

bool FMantleMeshLumenBake::Bake(const TConstArrayView<uint8> CookedMesh, const FMantleContentHash& MeshHash, const FString& Name, TArray<uint8>& OutFile, FString& OutReason)
{
	OutFile.Reset();
	FMantleStaticMeshData Data;
	if (!FMantleStaticMeshReader::HasMagic(CookedMesh) || !FMantleStaticMeshReader::Read(CookedMesh, Data, &OutReason))
	{
		OutReason = FString::Printf(TEXT("is not an MRM2 the client could read (%s)"), *OutReason);
		return false;
	}

	// LOD0 as the client builds it, CPU buffers only: the bakers read the position and index buffers.
	FMantleStaticMeshBuildParams Params;
	Params.bBuildLowerLODs = false;
	Params.bInitRenderResources = false;
	Params.Name = TEXT("MantleLumenBake");
	const TStrongObjectPtr<UStaticMesh> Mesh(FMantleRuntimeStaticMeshBuilder::Build(Data, Params, &OutReason));
	if (!Mesh || !Mesh->GetRenderData() || Mesh->GetRenderData()->LODResources.Num() == 0)
	{
		return false;
	}
	const FStaticMeshRenderData& RenderData = *Mesh->GetRenderData();
	const FStaticMeshLODResources& LOD0 = RenderData.LODResources[0];

	TArray<FSignedDistanceFieldBuildSectionData> Sections;
	Sections.SetNum(LOD0.Sections.Num());
	const FSourceMeshDataForDerivedDataTask NoSource;
	FMeshDataForDerivedDataTask MeshData;
	MeshData.SourceMeshData = &NoSource;
	MeshData.LODModel = &LOD0;
	MeshData.SectionData = Sections;
	MeshData.Bounds = FBoxSphereBounds3f(RenderData.Bounds);

	IMeshUtilities& MeshUtilities = FModuleManager::Get().LoadModuleChecked<IMeshUtilities>(TEXT("MeshUtilities"));
	FDistanceFieldVolumeData DistanceField;
	MeshUtilities.GenerateSignedDistanceFieldVolumeData(Name, MeshData, /*DistanceFieldResolutionScale*/ 1.0f, /*bGenerateAsIfTwoSided*/ false, DistanceField);
	if (!DistanceField.IsValid())
	{
		OutReason = TEXT("has no distance field (too thin or empty)");
		return false;
	}
	FCardRepresentationData Cards;
	const int32 MaxLumenMeshCards = FMeshBuildSettings().MaxLumenMeshCards;
	if (!MeshUtilities.GenerateCardRepresentationData(Name, MeshData, &DistanceField, MaxLumenMeshCards, /*bGenerateAsIfTwoSided*/ false, Cards))
	{
		OutReason = TEXT("got no Lumen cards");
		return false;
	}
	OutFile = FMantleMeshLumenFile::Write(MeshHash, DistanceField, Cards);
	if (OutFile.Num() == 0)
	{
		OutReason = TEXT("could not be written");
		return false;
	}
	return true;
}
