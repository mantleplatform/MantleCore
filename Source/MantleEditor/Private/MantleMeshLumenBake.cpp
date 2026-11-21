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
#include "MantlePlaceExporter.h"
#include "MantleRuntime.h"
#include "MantleWaterBodyExport.h"
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
	IMeshUtilities& MeshUtilities = FModuleManager::Get().LoadModuleChecked<IMeshUtilities>(TEXT("MeshUtilities"));
	return BakeUsing(MeshUtilities, Mesh, Sections, MeshTag, OutFile, OutReason);
}

bool FMantleMeshLumenBake::BakeUsing(IMeshUtilities& MeshUtilities, const FMantleGlbMesh& Mesh, const TConstArrayView<FMantleMeshLumenSection> Sections, const FString& MeshTag,
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

int32 FMantleMeshLumenBake::MaxInFlight()
{
	return FMath::Clamp(FPlatformMisc::NumberOfWorkerThreadsToSpawn(), 1, 64);
}

TSharedRef<FMantleMeshLumenJob> FMantleMeshLumenBake::Launch(FMantleGlbMesh&& Mesh, TArray<FMantleMeshLumenSection>&& Sections, const FString& MeshTag)
{
	check(IsInGameThread());
	// The jobs still running, oldest first (only the game thread touches this).
	static TArray<TSharedRef<FMantleMeshLumenJob>> InFlight;
	// MeshUtilities is loaded here, not on a worker, where loading a module is not allowed.
	IMeshUtilities& MeshUtilities = FModuleManager::Get().LoadModuleChecked<IMeshUtilities>(TEXT("MeshUtilities"));

	const auto DropFinished = []()
	{
		InFlight.RemoveAll([](const TSharedRef<FMantleMeshLumenJob>& Each) { return Each->IsDone(); });
	};
	DropFinished();
	while (InFlight.Num() >= MaxInFlight())
	{
		TArray<UE::Tasks::FTask> Running;
		for (const TSharedRef<FMantleMeshLumenJob>& Each : InFlight)
		{
			Running.Add(Each->Task);
		}
		UE::Tasks::WaitAny(Running);
		DropFinished();
	}

	TSharedRef<FMantleMeshLumenJob> Job = MakeShared<FMantleMeshLumenJob>();
	Job->Mesh = MoveTemp(Mesh);
	Job->Sections = MoveTemp(Sections);
	Job->MeshTag = MeshTag;
	Job->Task = UE::Tasks::Launch(UE_SOURCE_LOCATION, [Job, &MeshUtilities]()
	{
		const double Start = FPlatformTime::Seconds();
		FMantleMeshLumenResult& Result = Job->Result;
		Result.bBaked = BakeUsing(MeshUtilities, Job->Mesh, Job->Sections, Job->MeshTag, Result.File, Result.Reason);
		Result.Seconds = FPlatformTime::Seconds() - Start;
		// The copy of the mesh is not needed again; let it go before the job is joined.
		Job->Mesh = FMantleGlbMesh();
		Job->Sections.Empty();
	});
	InFlight.Add(Job);
	return Job;
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
		// A flat mesh with a water material publishes as a water body, so its mesh is not published and has no Lumen data to lack.
		if (FMantleWaterBodyExport::IsFlatWaterMesh(*It))
		{
			continue;
		}
		TInlineComponentArray<UStaticMeshComponent*> Components(*It);
		for (const UStaticMeshComponent* Component : Components)
		{
			// A mesh not shown in game (collision only, or left out) is never drawn, so it needs no Lumen data.
			if (const UStaticMesh* Mesh = Component && FMantlePlaceExporter::IsShownInGame(Component) ? Component->GetStaticMesh() : nullptr;
				Mesh && !Meshes.Contains(Mesh))
			{
				Meshes.Add(Mesh, It->GetActorNameOrLabel());
			}
		}
	}
	// Each mesh's bake runs on a worker thread as soon as the mesh is read; the findings are made after, in the meshes' order.
	struct FStarted
	{
		const UStaticMesh* Mesh = nullptr;
		FString Actor;
		TSharedPtr<FMantleMeshLumenJob> Job;
	};
	TArray<FStarted> Started;
	const double Start = FPlatformTime::Seconds();
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
		Started.Add({ Pair.Key, Pair.Value, Launch(MoveTemp(Glb), MoveTemp(Sections), FString(TEXT("sha256:check"))) });
	}
	int32 Without = 0;
	for (FStarted& Each : Started)
	{
		Each.Job->Wait();
		const FMantleMeshLumenResult& Result = Each.Job->GetResult();
		if (Result.bBaked)
		{
			continue;
		}
		++Without;
		FMantleValidationIssue& Issue = OutIssues.AddDefaulted_GetRef();
		Issue.Severity = EMantleIssueSeverity::Warning;
		Issue.Actor = Each.Actor;
		Issue.Problem = FString::Printf(TEXT("This level lights with Lumen, but mesh '%s' gets no distance field for it: it %s. In Mantle, Lumen does not see it, so it neither bounces light nor shades what is behind it."),
			*Each.Mesh->GetName(), *Result.Reason);
		Issue.Remedy = TEXT("If it should take part in the lighting, give it an opaque material or more thickness. A glass pane, decal card or effect mesh can stay as it is.");
	}
	UE_LOG(LogMantle, Log, TEXT("MANTLE_CHECK_LUMEN level asks for Lumen: %d distinct mesh(es), %d without Lumen data, %.1f s on up to %d worker(s)"),
		Meshes.Num(), Without, FPlatformTime::Seconds() - Start, MaxInFlight());
}
