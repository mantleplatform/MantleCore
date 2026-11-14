// Copyright Joshua Eger. All Rights Reserved.
#include "MantleMeshNaniteBake.h"
#include "MantleMirrorBake.h"
#include "MantleNaniteTestSupport.h"
#include "MantleNaniteTestSupport.h"
#include "MantlePlaceAssetConverter.h"
#include "MantlePlaceAssets.h"
#include "MantlePlaceExporter.h"
#include "MantleNaniteSideFile.h"
#include "MantleNaniteSideFile.h"
#include "MantleSha256.h"
#include "NaniteAssemblyDataBuilder.h"

#include "AssetRegistry/AssetRegistryModule.h"

#include "Algo/Reverse.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Components/StaticMeshComponent.h"
#include "HAL/IConsoleManager.h"
#include "Materials/Material.h"
#include "MeshDescription.h"
#include "Misc/AutomationTest.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Rendering/NaniteResources.h"
#include "RenderUtils.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshCompiler.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#if WITH_DEV_AUTOMATION_TESTS

// A mesh's Nanite data written to an MND1 file and read back field for field, and what the converter makes of it.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleMeshNaniteBakeTestPrivate
{
	const FString MeshTag = TEXT("sha256:") + FString::ChrN(64, TEXT('d'));

	/** Slot names of MakeGrid's materials. */
	FName SlotName(const int32 Slot)
	{
		return FName(*FString::Printf(TEXT("Slot%d"), Slot));
	}

	/** Which of Slots bands row Y of a Cells-row grid is in (the bands are uneven, so each has its own triangle count). */
	int32 BandOf(const int32 Y, const int32 Cells, const int32 Slots)
	{
		return FMath::Min(Slots - 1, (Y * Y * Slots) / (Cells * Cells));
	}

	/**
	 * A bumpy Cells x Cells grid of 10 cm squares built with Nanite on and the given residency. With Slots > 1 it has that many
	 * materials (Slot0, Slot1, ...), each drawing one band of rows (BandOf).
	 */
	UStaticMesh* MakeGrid(const int32 Cells, const bool bNanite, const uint32 ResidencyKB, const int32 Slots = 1)
	{
		UPackage* Transient = GetTransientPackage();
		UStaticMesh* Mesh = NewObject<UStaticMesh>(Transient, MakeUniqueObjectName(Transient, UStaticMesh::StaticClass(), TEXT("NaniteGrid")), RF_Transient);
		Mesh->SetNumSourceModels(1);
		// Readable render data, as the mirror bake reads it (FMantleMirrorBake reads the render LODs).
		Mesh->bAllowCPUAccess = true;

		FMeshDescription Description;
		FStaticMeshAttributes Attributes(Description);
		Attributes.Register();
		Attributes.GetVertexInstanceUVs().SetNumChannels(1);
		TArray<FPolygonGroupID> Groups;
		for (int32 Slot = 0; Slot < Slots; ++Slot)
		{
			Mesh->GetStaticMaterials().Add(FStaticMaterial(UMaterial::GetDefaultMaterial(MD_Surface), SlotName(Slot), SlotName(Slot)));
			Groups.Add(Description.CreatePolygonGroup());
			Attributes.GetPolygonGroupMaterialSlotNames()[Groups.Last()] = SlotName(Slot);
		}
		TArray<FVertexID> Vertices;
		for (int32 Y = 0; Y <= Cells; ++Y)
		{
			for (int32 X = 0; X <= Cells; ++X)
			{
				const FVertexID Vertex = Description.CreateVertex();
				Attributes.GetVertexPositions()[Vertex] = FVector3f(X * 10.f, Y * 10.f, 5.f * FMath::Sin(X * 0.7f) * FMath::Cos(Y * 0.5f));
				Vertices.Add(Vertex);
			}
		}
		const auto Corner = [&](const int32 X, const int32 Y)
		{
			const FVertexInstanceID Instance = Description.CreateVertexInstance(Vertices[Y * (Cells + 1) + X]);
			Attributes.GetVertexInstanceNormals()[Instance] = FVector3f(0, 0, 1);
			Attributes.GetVertexInstanceTangents()[Instance] = FVector3f(1, 0, 0);
			Attributes.GetVertexInstanceBinormalSigns()[Instance] = 1.f;
			Attributes.GetVertexInstanceUVs().Set(Instance, 0, FVector2f(X / float(Cells), Y / float(Cells)));
			return Instance;
		};
		for (int32 Y = 0; Y < Cells; ++Y)
		{
			for (int32 X = 0; X < Cells; ++X)
			{
				const FPolygonGroupID Group = Groups[BandOf(Y, Cells, Slots)];
				Description.CreateTriangle(Group, { Corner(X, Y), Corner(X + 1, Y + 1), Corner(X + 1, Y) });
				Description.CreateTriangle(Group, { Corner(X, Y), Corner(X, Y + 1), Corner(X + 1, Y + 1) });
			}
		}
		Mesh->CreateMeshDescription(0, MoveTemp(Description));
		Mesh->CommitMeshDescription(0);
		for (int32 Slot = 0; Slot < Slots; ++Slot)
		{
			FMeshSectionInfo Info(Slot);
			Mesh->GetSectionInfoMap().Set(0, Slot, Info);
		}
		Mesh->GetOriginalSectionInfoMap().CopyFrom(Mesh->GetSectionInfoMap());
		FMeshNaniteSettings Settings = Mesh->GetNaniteSettings();
		Settings.bEnabled = bNanite;
		Settings.TargetMinimumResidencyInKB = ResidencyKB;
		Mesh->SetNaniteSettings(Settings);
		Mesh->Build(/*bInSilent*/ true);
		FStaticMeshCompilingManager::Get().FinishCompilation({ Mesh });
		return Mesh;
	}

	/**
	 * Foliage-like geometry made without Nanite: Count small two-triangle cards (8 cm), randomly placed and turned in a 3 m ball,
	 * on one slot. Coarse levels of such a cloud cannot be kept as triangles, so a Voxelize build turns them into voxels.
	 */
	UStaticMesh* MakeLeafCloud(const int32 Count)
	{
		UPackage* Transient = GetTransientPackage();
		UStaticMesh* Mesh = NewObject<UStaticMesh>(Transient, MakeUniqueObjectName(Transient, UStaticMesh::StaticClass(), TEXT("NaniteLeaves")), RF_Transient);
		Mesh->SetNumSourceModels(1);
		FMeshDescription Description;
		FStaticMeshAttributes Attributes(Description);
		Attributes.Register();
		Attributes.GetVertexInstanceUVs().SetNumChannels(1);
		Mesh->GetStaticMaterials().Add(FStaticMaterial(UMaterial::GetDefaultMaterial(MD_Surface), SlotName(0), SlotName(0)));
		const FPolygonGroupID Group = Description.CreatePolygonGroup();
		Attributes.GetPolygonGroupMaterialSlotNames()[Group] = SlotName(0);
		FRandomStream Random(7);
		for (int32 Card = 0; Card < Count; ++Card)
		{
			const FVector3f Center = FVector3f(Random.GetUnitVector() * Random.FRandRange(0.0, 150.0));
			const FVector3f U = FVector3f(Random.GetUnitVector()) * 4.f;
			const FVector3f N = FVector3f(Random.GetUnitVector());
			const FVector3f V = FVector3f::CrossProduct(N, U).GetSafeNormal() * 4.f;
			const FVector3f Normal = FVector3f::CrossProduct(U, V).GetSafeNormal();
			FVertexInstanceID Corners[4];
			const FVector2f UVs[4] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
			const FVector3f Offsets[4] = { -U - V, U - V, U + V, -U + V };
			for (int32 C = 0; C < 4; ++C)
			{
				const FVertexID Vertex = Description.CreateVertex();
				Attributes.GetVertexPositions()[Vertex] = Center + Offsets[C];
				Corners[C] = Description.CreateVertexInstance(Vertex);
				Attributes.GetVertexInstanceNormals()[Corners[C]] = Normal;
				Attributes.GetVertexInstanceTangents()[Corners[C]] = U.GetSafeNormal();
				Attributes.GetVertexInstanceBinormalSigns()[Corners[C]] = 1.f;
				Attributes.GetVertexInstanceUVs().Set(Corners[C], 0, UVs[C]);
			}
			Description.CreateTriangle(Group, { Corners[0], Corners[1], Corners[2] });
			Description.CreateTriangle(Group, { Corners[0], Corners[2], Corners[3] });
		}
		Mesh->CreateMeshDescription(0, MoveTemp(Description));
		Mesh->CommitMeshDescription(0);
		Mesh->GetSectionInfoMap().Set(0, 0, FMeshSectionInfo(0));
		Mesh->GetOriginalSectionInfoMap().CopyFrom(Mesh->GetSectionInfoMap());
		Mesh->Build(/*bInSilent*/ true);
		FStaticMeshCompilingManager::Get().FinishCompilation({ Mesh });
		return Mesh;
	}

	/** The slot table of a file whose slots are the upload's own: 0, 1, ..., Count - 1. */
	TArray<int32> IdentityTable(const int32 Count)
	{
		TArray<int32> Table;
		for (int32 Slot = 0; Slot < Count; ++Slot)
		{
			Table.Add(Slot);
		}
		return Table;
	}

	template <typename T>
	bool SameBytes(const TArray<T>& A, const TArray<T>& B)
	{
		return A.Num() == B.Num() && FMemory::Memcmp(A.GetData(), B.GetData(), A.Num() * sizeof(T)) == 0;
	}

	/**
	 * Whether this editor can build Nanite data at all. It needs a graphics device's shader platform, which an editor started
	 * with -nullrhi (as the whole-suite run is) has not: such a run skips the tests that read built Nanite data.
	 */
	bool NaniteBuilds()
	{
		static const bool bBuilds = []
		{
			UStaticMesh* Grid = MakeGrid(4, /*bNanite*/ true, 0);
			const bool bValid = Grid && Grid->GetRenderData() && Grid->GetRenderData()->HasValidNaniteData();
			if (Grid)
			{
				Grid->MarkAsGarbage();
			}
			return bValid;
		}();
		return bBuilds;
	}

	/** A one-slot grid whose only material is see-through. */
	UStaticMesh* MakeGlassGrid()
	{
		UStaticMesh* Mesh = MakeGrid(4, /*bNanite*/ false, 0);
		UMaterial* Glass = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
		Glass->BlendMode = BLEND_Translucent;
		Mesh->GetStaticMaterials()[0].MaterialInterface = Glass;
		return Mesh;
	}

	/** A package under the project's /Game that holds one saved mesh; its file goes away with the object. The converter caches only saved meshes. */
	struct FSavedMesh
	{
		FString PackageName;
		FString File;
		UPackage* Package = nullptr;
		UStaticMesh* Mesh = nullptr;

		explicit FSavedMesh(const TCHAR* Name)
			: PackageName(FString(TEXT("/Game/MantleAutomation/_NaniteBakeTest/")) + Name)
			, File(FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension()))
		{
		}

		~FSavedMesh()
		{
			IFileManager::Get().Delete(*File, false, true, true);
			IFileManager::Get().DeleteDirectory(*FPaths::GetPath(File), false, true);
		}

		/** Moves Transient (a mesh made in the transient package) into this package and saves it. */
		bool Adopt(UStaticMesh* Transient)
		{
			Package = CreatePackage(*PackageName);
			Mesh = Transient;
			Mesh->Rename(*FPackageName::GetShortName(PackageName), Package, REN_DontCreateRedirectors | REN_NonTransactional);
			Mesh->ClearFlags(RF_Transient);
			Mesh->SetFlags(RF_Public | RF_Standalone);
			FAssetRegistryModule::AssetCreated(Mesh);
			return Save();
		}

		/** Saves the package again after Mesh was edited. */
		bool Save()
		{
			Package->MarkPackageDirty();
			FSavePackageArgs Args;
			Args.TopLevelFlags = RF_Public | RF_Standalone;
			Args.SaveFlags = SAVE_NoError;
			return UPackage::SavePackage(Package, Mesh, *File, Args);
		}
	};

	/** Where this test's converters cache, emptied first so a run does not see the last one's files. */
	FString FreshCacheDir(const TCHAR* Name)
	{
		const FString Dir = FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("MantleNaniteBake"), Name);
		IFileManager::Get().DeleteDirectory(*Dir, false, true);
		return Dir;
	}
}

bool MantleNaniteTestEditorBuildsNanite()
{
	return MantleMeshNaniteBakeTestPrivate::NaniteBuilds();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMeshNaniteFileRoundTripTest,
	"Mantle.Editor.Nanite.FileRoundTrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMeshNaniteFileRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace MantleMeshNaniteBakeTestPrivate;

	if (!NaniteBuilds())
	{
		AddInfo(TEXT("skipped: this editor builds no Nanite data (started without a graphics device)"));
		return true;
	}

	UStaticMesh* Grid = MakeGrid(64, /*bNanite*/ true, MAX_uint32);
	const FStaticMeshRenderData* RenderData = Grid ? Grid->GetRenderData() : nullptr;
	if (!TestTrue(TEXT("the grid builds Nanite data"), RenderData && RenderData->HasValidNaniteData()))
	{
		return false;
	}
	const Nanite::FResources& Built = *RenderData->NaniteResourcesPtr;
	TestEqual(TEXT("kept whole in memory, every page is a root page"), static_cast<uint32>(Built.PageStreamingStates.Num()), Built.NumRootPages);

	TArray<uint8> File;
	FString Why;
	if (!TestTrue(TEXT("it writes"), FMantleMeshNaniteFile::Write(MeshTag, { 0 }, Built, File, Why)))
	{
		AddError(Why);
		return false;
	}
	Nanite::FResources Back;
	TArray<int32> Slots;
	if (!TestTrue(TEXT("it reads back"), FMantleMeshNaniteFile::Read(File, MeshTag, Back, Slots, &Why)))
	{
		AddError(Why);
		return false;
	}
	TestEqual(TEXT("slot table"), Slots, TArray<int32>({ 0 }));
	TestEqual(TEXT("flags (no editor cache)"), Back.ResourceFlags, Built.ResourceFlags & ~NANITE_RESOURCE_FLAG_STREAMING_DATA_IN_DDC);
	TestTrue(TEXT("root data"), SameBytes(Back.RootData, Built.RootData));
	TestEqual(TEXT("page count"), Back.PageStreamingStates.Num(), Built.PageStreamingStates.Num());
	for (int32 Index = 0; Index < FMath::Min(Back.PageStreamingStates.Num(), Built.PageStreamingStates.Num()); ++Index)
	{
		const Nanite::FPageStreamingState& A = Back.PageStreamingStates[Index];
		const Nanite::FPageStreamingState& B = Built.PageStreamingStates[Index];
		TestTrue(FString::Printf(TEXT("page %d"), Index), A.BulkOffset == B.BulkOffset && A.BulkSize == B.BulkSize && A.PageSize == B.PageSize
			&& A.DependenciesStart == B.DependenciesStart && A.DependenciesNum == B.DependenciesNum && A.MaxHierarchyDepth == B.MaxHierarchyDepth && A.Flags == B.Flags);
	}
	TestTrue(TEXT("hierarchy nodes"), SameBytes(Back.HierarchyNodes, Built.HierarchyNodes));
	TestTrue(TEXT("hierarchy root offsets"), SameBytes(Back.HierarchyRootOffsets, Built.HierarchyRootOffsets));
	TestTrue(TEXT("page dependencies"), SameBytes(Back.PageDependencies, Built.PageDependencies));
	TestTrue(TEXT("assembly transforms"), SameBytes(Back.AssemblyTransforms, Built.AssemblyTransforms));
	TestTrue(TEXT("assembly bone data"), SameBytes(Back.AssemblyBoneAttachmentData, Built.AssemblyBoneAttachmentData));
	TestTrue(TEXT("bone indices"), SameBytes(Back.BoneIndices, Built.BoneIndices));
	TestTrue(TEXT("page range lookup"), SameBytes(Back.PageRangeLookup, Built.PageRangeLookup));
	TestTrue(TEXT("bounds"), Back.MeshBounds.Origin == Built.MeshBounds.Origin && Back.MeshBounds.BoxExtent == Built.MeshBounds.BoxExtent
		&& Back.MeshBounds.SphereRadius == Built.MeshBounds.SphereRadius);
	TestEqual(TEXT("root pages"), Back.NumRootPages, Built.NumRootPages);
	TestEqual(TEXT("position precision"), Back.PositionPrecision, Built.PositionPrecision);
	TestEqual(TEXT("normal precision"), Back.NormalPrecision, Built.NormalPrecision);
	TestEqual(TEXT("tangent precision"), Back.TangentPrecision, Built.TangentPrecision);
	TestEqual(TEXT("input triangles"), Back.NumInputTriangles, Built.NumInputTriangles);
	TestEqual(TEXT("input vertices"), Back.NumInputVertices, Built.NumInputVertices);
	TestEqual(TEXT("clusters"), Back.NumClusters, Built.NumClusters);
	TestEqual(TEXT("voxel materials"), Back.VoxelMaterialsMask, Built.VoxelMaterialsMask);
	TestEqual(TEXT("input curves"), Back.NumInputCurves, Built.NumInputCurves);
	TestEqual(TEXT("the grid's triangles"), Back.NumInputTriangles, 64u * 64u * 2u);

	Nanite::FResources Other;
	TestFalse(TEXT("another mesh's file reads as not present"),
		FMantleMeshNaniteFile::Read(File, TEXT("sha256:") + FString::ChrN(64, TEXT('e')), Other, Slots, &Why));
	TestTrue(TEXT("and says whose it is"), Why.Contains(TEXT("belongs to mesh")));
	TArray<uint8> Cut(File.GetData(), File.Num() - 9);
	Nanite::FResources Damaged;
	TestFalse(TEXT("a cut file reads as damaged"), FMantleMeshNaniteFile::Read(Cut, MeshTag, Damaged, Slots, &Why));
	Grid->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMeshNaniteBakeTest,
	"Mantle.Editor.Nanite.Bake",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMeshNaniteBakeTest::RunTest(const FString& Parameters)
{
	using namespace MantleMeshNaniteBakeTestPrivate;

	if (!NaniteBuilds())
	{
		AddInfo(TEXT("skipped: this editor builds no Nanite data (started without a graphics device)"));
		return true;
	}

	// The creator's mesh at the engine's default residency (one page): the bake's copy keeps all of it, the mesh is unchanged.
	UStaticMesh* Grid = MakeGrid(96, /*bNanite*/ true, 0);
	TArray<uint8> File;
	FString Why;
	if (!TestTrue(TEXT("a Nanite mesh bakes"), FMantleMeshNaniteBake::Bake(Grid, { 0 }, MeshTag, File, Why)))
	{
		AddError(Why);
		return false;
	}
	TestEqual(TEXT("the creator's mesh keeps its residency"), Grid->GetNaniteSettings().TargetMinimumResidencyInKB, 0u);
	Nanite::FResources Back;
	TArray<int32> Slots;
	if (!TestTrue(TEXT("its file reads back"), FMantleMeshNaniteFile::Read(File, MeshTag, Back, Slots, &Why)))
	{
		AddError(Why);
		return false;
	}
	TestEqual(TEXT("all of it is kept in memory"), static_cast<uint32>(Back.PageStreamingStates.Num()), Back.NumRootPages);
	TestEqual(TEXT("its triangles"), Back.NumInputTriangles, 96u * 96u * 2u);
	TestEqual(TEXT("slot 0 publishes as glTF material 0"), Slots, TArray<int32>({ 0 }));
	AddInfo(FString::Printf(TEXT("%d bytes, %u clusters, %u pages"), File.Num(), Back.NumClusters, Back.NumRootPages));

	// A mesh made without Nanite bakes too: the copy gets Nanite, the creator's mesh does not.
	UStaticMesh* Plain = MakeGrid(32, /*bNanite*/ false, 0);
	TArray<uint8> PlainFile;
	if (TestTrue(TEXT("a mesh made without Nanite bakes"), FMantleMeshNaniteBake::Bake(Plain, { 0 }, MeshTag, PlainFile, Why)))
	{
		Nanite::FResources PlainBack;
		TestTrue(TEXT("its file reads back"), FMantleMeshNaniteFile::Read(PlainFile, MeshTag, PlainBack, Slots, &Why));
		TestEqual(TEXT("with all its triangles"), PlainBack.NumInputTriangles, 32u * 32u * 2u);
	}
	else
	{
		AddError(Why);
	}
	TestFalse(TEXT("the creator's mesh still has no Nanite"), Plain->IsNaniteEnabled());
	Grid->MarkAsGarbage();
	Plain->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMeshNaniteVoxelFoliageTest,
	"Mantle.Editor.Nanite.VoxelFoliage",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMeshNaniteVoxelFoliageTest::RunTest(const FString& Parameters)
{
	using namespace MantleMeshNaniteBakeTestPrivate;

	// Two slots: 0 the default surface, 1 a leaf card (masked, two-sided). Only read, never built with these materials.
	UStaticMesh* Mesh = MakeGrid(8, /*bNanite*/ false, 0, 2);
	UMaterial* Leaf = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
	Leaf->BlendMode = BLEND_Masked;
	Leaf->TwoSided = true;
	UMaterial* Card = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
	Card->BlendMode = BLEND_Masked;
	Card->TwoSided = false;
	Mesh->GetStaticMaterials()[1].MaterialInterface = Leaf;

	TestFalse(TEXT("a plain surface is not foliage"), FMantleMeshNaniteBake::WantsVoxels(Mesh, { 0 }));
	TestTrue(TEXT("a drawn masked two-sided slot is foliage"), FMantleMeshNaniteBake::WantsVoxels(Mesh, { 0, 1 }));
	TestTrue(TEXT("in any order"), FMantleMeshNaniteBake::WantsVoxels(Mesh, { 1, 0 }));
	Mesh->GetStaticMaterials()[1].MaterialInterface = Card;
	TestFalse(TEXT("a one-sided cut-out (a fence, a grate) is not"), FMantleMeshNaniteBake::WantsVoxels(Mesh, { 0, 1 }));
	Mesh->GetStaticMaterials()[1].MaterialInterface = Leaf;
	TestFalse(TEXT("a leaf slot the upload does not draw does not count"), FMantleMeshNaniteBake::WantsVoxels(Mesh, { 0 }));
	TestFalse(TEXT("no mesh, no foliage"), FMantleMeshNaniteBake::WantsVoxels(nullptr, { 0 }));

	if (!NaniteBuilds())
	{
		AddInfo(TEXT("skipped the voxel build: this editor builds no Nanite data (started without a graphics device)"));
		Mesh->MarkAsGarbage();
		return true;
	}
	// The bake really makes voxels, in any project: the plugin switches voxels on at startup (Config/Engine.ini), because without
	// them the engine silently drops Voxelize (Downtown's 412 files, 2026-11-14, all had a zero voxel mask).
	TestTrue(TEXT("voxels are on in this editor (the plugin's r.Nanite.AllowVoxels=1)"), NaniteVoxelsSupported());
	{
		UStaticMesh* Leaves = MakeLeafCloud(6000);
		Leaves->GetStaticMaterials()[0].MaterialInterface = Leaf;
		TestTrue(TEXT("the leaf cloud is foliage"), FMantleMeshNaniteBake::WantsVoxels(Leaves, { 0 }));
		TArray<uint8> File;
		FString Why;
		uint64 Mask = 0;
		if (TestTrue(TEXT("the leaf cloud bakes"), FMantleMeshNaniteBake::Bake(Leaves, { 0 }, MeshTag, File, Why, &Mask)))
		{
			Nanite::FResources Back;
			TArray<int32> Slots;
			if (TestTrue(TEXT("its file reads back"), FMantleMeshNaniteFile::Read(File, MeshTag, Back, Slots, &Why)))
			{
				TestTrue(TEXT("its file has voxels (a nonzero voxel mask)"), Back.VoxelMaterialsMask != 0);
				TestEqual(TEXT("the file's mask is the built mask"), Back.VoxelMaterialsMask, Mask);
				AddInfo(FString::Printf(TEXT("leaf cloud: voxel mask 0x%llx, %u clusters"), static_cast<unsigned long long>(Back.VoxelMaterialsMask), Back.NumClusters));
			}
		}
		else
		{
			AddError(Why);
		}
		Leaves->MarkAsGarbage();
	}

	Mesh->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMeshNanitePermutedSlotsTest,
	"Mantle.Editor.Nanite.PermutedSlots",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMeshNanitePermutedSlotsTest::RunTest(const FString& Parameters)
{
	using namespace MantleMeshNaniteBakeTestPrivate;

	// Three slots in three bands; the upload draws slot 2 as glTF material 0 and slot 0 as glTF material 1, and not slot 1
	// (the order and the dropped slot are what 96 of MedievalVillageDay's meshes have).
	constexpr int32 Cells = 48;
	const TArray<int32> Published = { 2, 0 };
	int32 BandTriangles[3] = { 0, 0, 0 };
	for (int32 Y = 0; Y < Cells; ++Y)
	{
		BandTriangles[BandOf(Y, Cells, 3)] += 2 * Cells;
	}
	TestTrue(TEXT("each band has its own triangle count"), BandTriangles[0] > 0 && BandTriangles[1] > 0 && BandTriangles[2] > 0
		&& BandTriangles[0] != BandTriangles[1] && BandTriangles[1] != BandTriangles[2] && BandTriangles[0] != BandTriangles[2]);

	// The conform on a copy without Nanite, whose LOD 0 keeps every triangle: slot s is glTF material s, the dropped band is gone.
	UStaticMesh* Plain = MakeGrid(Cells, /*bNanite*/ false, 0, 3);
	UStaticMesh* Copy = DuplicateObject<UStaticMesh>(Plain, GetTransientPackage());
	FString Why;
	if (!TestTrue(TEXT("the copy conforms"), FMantleMeshNaniteBake::ConformToPublished(Copy, Published, Why)))
	{
		AddError(Why);
		return false;
	}
	Copy->Build(/*bInSilent*/ true);
	FStaticMeshCompilingManager::Get().FinishCompilation({ Copy });
	TestEqual(TEXT("two slots"), Copy->GetStaticMaterials().Num(), 2);
	if (Copy->GetStaticMaterials().Num() == 2)
	{
		TestEqual(TEXT("slot 0 is the creator's slot 2"), Copy->GetStaticMaterials()[0].MaterialSlotName.ToString(), SlotName(2).ToString());
		TestEqual(TEXT("slot 1 is the creator's slot 0"), Copy->GetStaticMaterials()[1].MaterialSlotName.ToString(), SlotName(0).ToString());
	}
	const FStaticMeshRenderData* RenderData = Copy->GetRenderData();
	TMap<int32, int32> TrianglesBySlot;
	if (TestTrue(TEXT("the copy builds"), RenderData && RenderData->LODResources.Num() > 0))
	{
		for (const FStaticMeshSection& Section : RenderData->LODResources[0].Sections)
		{
			TrianglesBySlot.FindOrAdd(Section.MaterialIndex) += static_cast<int32>(Section.NumTriangles);
		}
	}
	TestEqual(TEXT("slot 0 draws band 2"), TrianglesBySlot.FindRef(0), BandTriangles[2]);
	TestEqual(TEXT("slot 1 draws band 0"), TrianglesBySlot.FindRef(1), BandTriangles[0]);
	TestEqual(TEXT("nothing else is drawn (band 1 is gone)"), TrianglesBySlot.Num(), 2);
	TestEqual(TEXT("the creator's mesh keeps its three slots"), Plain->GetStaticMaterials().Num(), 3);

	TArray<uint8> File;
	if (!NaniteBuilds())
	{
		AddInfo(TEXT("skipped the Nanite bake: this editor builds no Nanite data (started without a graphics device)"));
		TestFalse(TEXT("an upload that draws none of the mesh's slots does not bake"), FMantleMeshNaniteBake::Bake(Plain, { 7 }, MeshTag, File, Why));
		TestTrue(TEXT("and says why"), Why.Contains(TEXT("none of its sections")));
		Plain->MarkAsGarbage();
		Copy->MarkAsGarbage();
		return true;
	}

	// The bake of the Nanite version: an identity table, and the dropped band's triangles are not in the Nanite data.
	UStaticMesh* Grid = MakeGrid(Cells, /*bNanite*/ true, 0, 3);
	if (!TestTrue(TEXT("the permuted mesh bakes"), FMantleMeshNaniteBake::Bake(Grid, Published, MeshTag, File, Why)))
	{
		AddError(Why);
		return false;
	}
	Nanite::FResources Back;
	TArray<int32> Slots;
	if (!TestTrue(TEXT("its file reads back"), FMantleMeshNaniteFile::Read(File, MeshTag, Back, Slots, &Why)))
	{
		AddError(Why);
		return false;
	}
	TestEqual(TEXT("the slot table is the identity"), Slots, TArray<int32>({ 0, 1 }));
	TestEqual(TEXT("its triangles are the two published bands"), Back.NumInputTriangles, static_cast<uint32>(BandTriangles[0] + BandTriangles[2]));
	TestEqual(TEXT("the creator's mesh keeps its three slots"), Grid->GetStaticMaterials().Num(), 3);

	TestFalse(TEXT("an upload that draws none of the mesh's slots does not bake"), FMantleMeshNaniteBake::Bake(Grid, { 7 }, MeshTag, File, Why));
	TestTrue(TEXT("and says why"), Why.Contains(TEXT("none of its sections")));
	Plain->MarkAsGarbage();
	Copy->MarkAsGarbage();
	Grid->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMeshNaniteMirroredTest,
	"Mantle.Editor.Nanite.MirroredCopies",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMeshNaniteMirroredTest::RunTest(const FString& Parameters)
{
	using namespace MantleMeshNaniteBakeTestPrivate;

	if (!NaniteBuilds())
	{
		AddInfo(TEXT("skipped: this editor builds no Nanite data (started without a graphics device)"));
		return true;
	}

	// A plain two-slot grid placed flipped on X: the converter's mirrored copy bakes like the creator's own mesh.
	UStaticMesh* Grid = MakeGrid(24, /*bNanite*/ false, 0, 2);
	FVector Positive;
	FString Why;
	UStaticMesh* Mirrored = FMantleMirrorBake::MirroredCopy(Grid, FVector(-1.0, 1.0, 1.0), Positive, Why);
	if (!TestNotNull(TEXT("the grid mirrors"), Mirrored))
	{
		AddError(Why);
		return false;
	}
	TestTrue(TEXT("the mirrored copy is the converter's (transient)"), Mirrored->GetOutermost() == GetTransientPackage());

	// Its upload's slots, as the converter reads them, in reverse to exercise the reorder too.
	TArray<int32> Published = FMantlePlaceAssets::UploadSlots(Mirrored);
	TestEqual(TEXT("the mirrored copy draws both slots"), Published.Num(), 2);
	Algo::Reverse(Published);
	TArray<uint8> File;
	if (TestTrue(TEXT("the mirrored copy bakes"), FMantleMeshNaniteBake::Bake(Mirrored, Published, MeshTag, File, Why)))
	{
		Nanite::FResources Back;
		TArray<int32> Slots;
		if (TestTrue(TEXT("its file reads back"), FMantleMeshNaniteFile::Read(File, MeshTag, Back, Slots, &Why)))
		{
			TestEqual(TEXT("with all the grid's triangles"), Back.NumInputTriangles, 24u * 24u * 2u);
			TestEqual(TEXT("and an identity slot table"), Slots, TArray<int32>({ 0, 1 }));
			// Mirrored on X about the mesh's origin: the grid spans 0 to 240 cm on X, its copy -240 to 0.
			TestTrue(TEXT("its bounds are mirrored"), Back.MeshBounds.Origin.X < 0.f);
		}
	}
	else
	{
		AddError(Why);
	}
	Grid->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMeshNaniteAssemblyTest,
	"Mantle.Editor.Nanite.Assembly",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMeshNaniteAssemblyTest::RunTest(const FString& Parameters)
{
	using namespace MantleMeshNaniteBakeTestPrivate;

	if (!NaniteBuilds())
	{
		AddInfo(TEXT("skipped: this editor builds no Nanite data (started without a graphics device)"));
		return true;
	}
	// The engine builds assemblies only when started with r.Nanite.AllowAssemblies=1 (StaticMeshBuilder.cpp, InitNaniteAssemblyData):
	// the plugin's Config/Engine.ini sets it.
	if (!TestTrue(TEXT("assemblies are on in this editor (the plugin's r.Nanite.AllowAssemblies=1)"), NaniteAssembliesSupported()))
	{
		return false;
	}

	// One two-slot part, placed twice, its slots crossed into the assembly's list (part slot 0 -> Leaf, 1 -> Bark).
	UStaticMesh* Part = MakeGrid(16, /*bNanite*/ true, 0, 2);
	Part->Rename(nullptr, CreatePackage(TEXT("/Temp/MantleNaniteTest/SM_AssemblyPart")), REN_DontCreateRedirectors | REN_NonTransactional);
	UMaterialInterface* Surface = UMaterial::GetDefaultMaterial(MD_Surface);
	FNaniteAssemblyDataBuilder Builder;
	Builder.AddMaterialSlot(FNaniteAssemblyDataBuilder::FMaterialSlot(Surface, TEXT("Bark")));
	Builder.AddMaterialSlot(FNaniteAssemblyDataBuilder::FMaterialSlot(Surface, TEXT("Leaf")));
	const int32 PartIndex = Builder.AddPart(FSoftObjectPath(Part));
	Builder.RemapPartMaterial(PartIndex, 0, 1);
	Builder.RemapPartMaterial(PartIndex, 1, 0);
	Builder.AddNode(PartIndex, FTransform3f::Identity);
	Builder.AddNode(PartIndex, FTransform3f(FVector3f(500.f, 0.f, 0.f)));

	UStaticMesh* Assembly = NewObject<UStaticMesh>(CreatePackage(TEXT("/Temp/MantleNaniteTest/SM_Assembly")), TEXT("SM_Assembly"), RF_Transient);
	if (!TestTrue(TEXT("the assembly applies"), Builder.ApplyToStaticMesh(*Assembly)))
	{
		return false;
	}
	Assembly->GetNaniteSettings().bEnabled = true;
	Assembly->Build(/*bInSilent*/ true);
	FStaticMeshCompilingManager::Get().FinishCompilation({ Assembly });
	if (!TestTrue(TEXT("it is a Nanite assembly"), Assembly->IsNaniteAssembly()))
	{
		return false;
	}

	// The upload's order, reversed so the bake has to renumber the list and the part's remap.
	TArray<int32> Published = FMantlePlaceAssets::UploadSlots(Assembly);
	TestEqual(TEXT("its upload draws both slots"), Published.Num(), 2);
	Algo::Reverse(Published);
	TArray<uint8> File;
	FString Why;
	if (TestTrue(TEXT("the assembly bakes"), FMantleMeshNaniteBake::Bake(Assembly, Published, MeshTag, File, Why)))
	{
		Nanite::FResources Back;
		TArray<int32> Slots;
		if (TestTrue(TEXT("its file reads back"), FMantleMeshNaniteFile::Read(File, MeshTag, Back, Slots, &Why)))
		{
			TestEqual(TEXT("an identity slot table"), Slots, TArray<int32>({ 0, 1 }));
			TestTrue(TEXT("it carries the part's placements"), Back.AssemblyTransforms.Num() > 0);
			AddInfo(FString::Printf(TEXT("%d bytes, %d assembly transforms, %u input triangles"), File.Num(), Back.AssemblyTransforms.Num(), Back.NumInputTriangles));
		}
	}
	else
	{
		AddError(Why);
	}
	TestFalse(TEXT("an upload that leaves out one of its slots does not bake"), FMantleMeshNaniteBake::Bake(Assembly, { Published[0] }, MeshTag, File, Why));
	TestTrue(TEXT("and says why"), Why.Contains(TEXT("leaves out")));
	TestEqual(TEXT("the assembly itself keeps its material order"), Assembly->GetStaticMaterials()[0].MaterialSlotName.ToString(), FString(TEXT("Bark")));
	Assembly->MarkAsGarbage();
	Part->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMeshNaniteSeeThroughTest,
	"Mantle.Editor.Nanite.SeeThroughStaysPlain",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMeshNaniteSeeThroughTest::RunTest(const FString& Parameters)
{
	using namespace MantleMeshNaniteBakeTestPrivate;

	// Nanite does not draw see-through surfaces, so the model keeps its plain form, and the creator is told.
	UStaticMesh* Glass = MakeGlassGrid();
	TArray<uint8> File;
	FString Why;
	bool bRetryable = true;
	TestFalse(TEXT("a see-through mesh does not bake"), FMantleMeshNaniteBake::Bake(Glass, { 0 }, MeshTag, File, Why, nullptr, &bRetryable));
	TestFalse(TEXT("and that is the mesh's own, so it may be cached"), bRetryable);
	TestTrue(TEXT("and says why"), Why.Contains(TEXT("see-through")));
	TestEqual(TEXT("with no file"), File.Num(), 0);

	// The converter's side: no file, the reason kept, a warning Check shows, and nothing refused.
	FMantlePlaceAssetConverter Converter(MakeShared<FMantleGltfMaterialFlattener>(), FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("MantleNaniteBake")));
	FMantlePlaceAssetSet Set;
	if (!TestTrue(TEXT("the mesh still converts"), Converter.ConvertMesh(Glass, Set)) || !TestEqual(TEXT("to one mesh"), Set.Meshes.Num(), 1))
	{
		return false;
	}
	const FMantleConvertedMesh& Converted = Set.Meshes[0];
	TestEqual(TEXT("it has no Nanite file"), Converted.Nanite.Num(), 0);
	TestTrue(TEXT("and keeps the reason"), Converted.NaniteNone.Contains(TEXT("see-through")));
	TestEqual(TEXT("one notice"), Set.Issues.Num(), 1);
	if (Set.Issues.Num() == 1)
	{
		TestTrue(TEXT("a warning, not an error"), Set.Issues[0].Severity == EMantleIssueSeverity::Warning);
		TestTrue(TEXT("naming the mesh and what players see"), Set.Issues[0].Problem.Contains(Glass->GetName()) && Set.Issues[0].Problem.Contains(TEXT("plain model")));
		TestFalse(TEXT("and saying what to do"), Set.Issues[0].Remedy.IsEmpty());
	}

	// The reason survives the local cache (a republish says it again).
	const FString CacheDir = FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("MantleNaniteBakeCache"));
	FMantlePlaceAssetConverter Cache(MakeShared<FMantleGltfMaterialFlattener>(), CacheDir);
	const FString Key = FMantleSha256::HexDigest(TArray<uint8>({ 'g', 'l', 'a', 's', 's' }));
	Cache.StoreCachedMesh(Key, Converted, Set.Issues);
	FMantleConvertedMesh Loaded;
	TArray<FMantleValidationIssue> LoadedIssues;
	if (TestTrue(TEXT("a cached mesh without Nanite loads"), Cache.LoadCachedMesh(Key, Loaded, LoadedIssues)))
	{
		TestEqual(TEXT("with no file"), Loaded.Nanite.Num(), 0);
		TestEqual(TEXT("and its reason"), Loaded.NaniteNone, Converted.NaniteNone);
		TestEqual(TEXT("and its notice"), LoadedIssues.Num(), 1);
	}
	Glass->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMeshNaniteConverterTest,
	"Mantle.Editor.Nanite.ConverterMakesTheFile",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMeshNaniteConverterTest::RunTest(const FString& Parameters)
{
	using namespace MantleMeshNaniteBakeTestPrivate;

	if (!NaniteBuilds())
	{
		AddInfo(TEXT("skipped: this editor builds no Nanite data (started without a graphics device)"));
		return true;
	}

	// A mesh made without Nanite, two slots: the converter bakes its file beside the GLB, tagged with the GLB's hash.
	UStaticMesh* Grid = MakeGrid(24, /*bNanite*/ false, 0, 2);
	FMantlePlaceAssetConverter Converter(MakeShared<FMantleGltfMaterialFlattener>(), FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("MantleNaniteBake")));
	FMantlePlaceAssetSet Set;
	if (!TestTrue(TEXT("the mesh converts"), Converter.ConvertMesh(Grid, Set)) || !TestEqual(TEXT("to one mesh"), Set.Meshes.Num(), 1))
	{
		return false;
	}
	const FMantleConvertedMesh& Converted = Set.Meshes[0];
	TestTrue(TEXT("it has a Nanite file"), Converted.Nanite.Num() > 0);
	TestEqual(TEXT("no reason for none"), Converted.NaniteNone, FString());
	TestEqual(TEXT("its hash is the file's"), Converted.NaniteHash, FMantleSha256::AssetHash(Converted.Nanite));
	TestEqual(TEXT("no notice"), Set.Issues.Num(), 0);
	FString Why;
	Nanite::FResources Back;
	TArray<int32> Slots;
	if (TestTrue(TEXT("the uploaded file unpacks and reads back for the GLB's hash"), FMantleMeshNaniteFile::ReadUploaded(Converted.Nanite, Converted.Hash, Back, Slots, &Why)))
	{
		TestEqual(TEXT("with all the grid's triangles"), Back.NumInputTriangles, 24u * 24u * 2u);
		TestEqual(TEXT("the table is 0, 1, 2, ... for each glTF material"), Slots, IdentityTable(Converted.Slots.Num()));
	}
	else
	{
		AddError(Why);
	}
	// What is uploaded is the container the backend takes: MNZ1, the inflated size, one zlib stream, inside the limit.
	TestTrue(TEXT("it is an MNZ1 container"), Converted.Nanite.Num() > 8 && FMemory::Memcmp(Converted.Nanite.GetData(), "MNZ1", 4) == 0);
	TestTrue(TEXT("inside the upload limit"), Converted.Nanite.Num() <= FMantleMeshNaniteFile::MaxUploadBytes);
	TArray<uint8> Inflated;
	if (TestTrue(TEXT("and unpacks"), FMantleMeshNaniteFile::Unpack(Converted.Nanite, Inflated, &Why)))
	{
		TestTrue(TEXT("to an MND1"), Inflated.Num() > 8 && FMemory::Memcmp(Inflated.GetData(), "MND1", 4) == 0);
		TestTrue(TEXT("that the container's compression made smaller"), Inflated.Num() > Converted.Nanite.Num());
	}
	TestEqual(TEXT("the converter timed the bake"), Converter.GetStats().NaniteSeconds > 0.0, true);

	// The local cache keeps the file with the mesh and refuses it when it does not match its hash.
	const FString CacheDir = FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("MantleNaniteBakeCache"));
	FMantlePlaceAssetConverter Cache(MakeShared<FMantleGltfMaterialFlattener>(), CacheDir);
	const FString Key = FMantleSha256::HexDigest(TArray<uint8>({ 'g', 'r', 'i', 'd' }));
	Cache.StoreCachedMesh(Key, Converted, Set.Issues);
	FMantleConvertedMesh Loaded;
	TArray<FMantleValidationIssue> LoadedIssues;
	if (TestTrue(TEXT("a cached mesh with Nanite loads"), Cache.LoadCachedMesh(Key, Loaded, LoadedIssues)))
	{
		TestTrue(TEXT("with the same file"), Loaded.Nanite == Converted.Nanite);
		TestEqual(TEXT("and hash"), Loaded.NaniteHash, Converted.NaniteHash);
		TestEqual(TEXT("and still the GLB's own hash"), Loaded.Hash, Converted.Hash);
	}
	const FString FilePath = FPaths::Combine(CacheDir, TEXT("meshes"), Key + TEXT(".nanite"));
	TArray<uint8> Damaged = Converted.Nanite;
	Damaged[Damaged.Num() / 2] ^= 0xFF;
	if (TestTrue(TEXT("the cached file can be damaged for the test"), FFileHelper::SaveArrayToFile(Damaged, *FilePath)))
	{
		FMantleConvertedMesh Refused;
		TestFalse(TEXT("a damaged cached file is refused, so the mesh converts again"), Cache.LoadCachedMesh(Key, Refused, LoadedIssues));
	}

	// A converter-made mesh (the mirrored copy; spline and cable bakes are built the same way, from mesh descriptions) bakes too, and a
	// converter that makes no Nanite files (a weapon or wheel publish) makes none and says nothing.
	FVector Positive;
	UStaticMesh* Mirrored = FMantleMirrorBake::MirroredCopy(Grid, FVector(-1.0, 1.0, 1.0), Positive, Why);
	if (TestNotNull(TEXT("the grid mirrors"), Mirrored))
	{
		FMantlePlaceAssetSet MirrorSet;
		if (TestTrue(TEXT("the mirrored copy converts"), Converter.ConvertMesh(Mirrored, MirrorSet)) && TestEqual(TEXT("to one mesh"), MirrorSet.Meshes.Num(), 1))
		{
			const FMantleConvertedMesh& Copy = MirrorSet.Meshes[0];
			Nanite::FResources MirrorBack;
			TArray<int32> MirrorSlots;
			if (TestTrue(TEXT("it has a Nanite file that reads back for its GLB"), Copy.Nanite.Num() > 0 && FMantleMeshNaniteFile::ReadUploaded(Copy.Nanite, Copy.Hash, MirrorBack, MirrorSlots, &Why)))
			{
				TestEqual(TEXT("with all the triangles"), MirrorBack.NumInputTriangles, 24u * 24u * 2u);
				TestEqual(TEXT("and the table 0, 1, 2, ... for each glTF material"), MirrorSlots, IdentityTable(Copy.Slots.Num()));
			}
		}
	}
	FMantlePlaceAssetConverter NoNanite(MakeShared<FMantleGltfMaterialFlattener>(), FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("MantleNaniteBake")));
	NoNanite.bBakeNanite = false;
	FMantlePlaceAssetSet NoNaniteSet;
	if (TestTrue(TEXT("a converter that makes no Nanite files converts"), NoNanite.ConvertMesh(Grid, NoNaniteSet)) && TestEqual(TEXT("to one mesh"), NoNaniteSet.Meshes.Num(), 1))
	{
		TestEqual(TEXT("with no file"), NoNaniteSet.Meshes[0].Nanite.Num(), 0);
		TestEqual(TEXT("and no notice"), NoNaniteSet.Issues.Num(), 0);
	}

	// The cache key carries the Nanite inputs: the format, the engine build, the voxel and assembly switches.
	const FString Salt = FMantleMeshNaniteBake::CacheSalt();
	TestTrue(TEXT("the salt names the engine build"), Salt.Contains(FMantleMeshNaniteFile::EngineTag()));
	TestTrue(TEXT("and the voxel and assembly switches"), Salt.Contains(TEXT("voxels=1")) && Salt.Contains(TEXT("assemblies=1")));
	Grid->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMeshNaniteContainerTest,
	"Mantle.Editor.Nanite.ContainerIsWhatTheBackendTakes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMeshNaniteContainerTest::RunTest(const FString& Parameters)
{
	// The container of the build plan's contracts\nanite-entry.md: "MNZ1", a little-endian uint32 with the inflated length, one zlib stream.
	TArray<uint8> Mnd1;
	Mnd1.Append(reinterpret_cast<const uint8*>("MND1"), 4);
	for (int32 Index = 0; Index < 50000; ++Index)
	{
		Mnd1.Add(static_cast<uint8>((Index * 7) % 13));
	}
	TArray<uint8> Container;
	FString Why;
	if (!TestTrue(TEXT("a file packs"), FMantleMeshNaniteFile::Pack(Mnd1, Container, Why)))
	{
		AddError(Why);
		return false;
	}
	TestTrue(TEXT("it starts with MNZ1"), FMemory::Memcmp(Container.GetData(), "MNZ1", 4) == 0);
	const uint32 SizeWord = Container[4] | (Container[5] << 8) | (Container[6] << 16) | (static_cast<uint32>(Container[7]) << 24);
	TestEqual(TEXT("then the inflated length, little-endian"), static_cast<int32>(SizeWord), Mnd1.Num());
	// A zlib stream (RFC 1950): deflate (method 8) in the low nibble of the first byte, and a header that is a multiple of 31.
	TestEqual(TEXT("then a zlib stream (deflate)"), static_cast<int32>(Container[8] & 0x0F), 8);
	TestEqual(TEXT("with a valid header check"), ((Container[8] << 8) | Container[9]) % 31, 0);
	TestTrue(TEXT("smaller than what it holds"), Container.Num() < Mnd1.Num());

	// The game's own unpacker takes it back to the same bytes.
	TArray<uint8> Back;
	if (TestTrue(TEXT("the game's unpacker reads it"), FMantleNaniteSideFile::Unpack(Container, Back, &Why)))
	{
		TestTrue(TEXT("to the same bytes"), Back == Mnd1);
	}
	TArray<uint8> Same;
	TestTrue(TEXT("packing the same bytes again makes the same file (a re-upload is the same asset)"), FMantleMeshNaniteFile::Pack(Mnd1, Same, Why) && Same == Container);

	// Damage and nonsense do not unpack.
	TArray<uint8> Damaged = Container;
	Damaged[Damaged.Num() / 2] ^= 0xFF;
	TestFalse(TEXT("a damaged stream is refused"), FMantleMeshNaniteFile::Unpack(Damaged, Back, &Why));
	TArray<uint8> Wrong = Container;
	Wrong[0] = 'X';
	TestFalse(TEXT("another magic is refused"), FMantleMeshNaniteFile::Unpack(Wrong, Back, &Why));
	TestFalse(TEXT("a raw MND1 is refused (it is not a container)"), FMantleMeshNaniteFile::Unpack(Mnd1, Back, &Why));
	TestFalse(TEXT("nothing packs to nothing"), FMantleMeshNaniteFile::Pack(TArray<uint8>(), Same, Why));

	TestEqual(TEXT("the size limit is 64 MiB"), FMantleMeshNaniteFile::MaxUploadBytes, int64(67108864));
	TestEqual(TEXT("and the converter's limit starts there"), FMantlePlaceAssetLimits().MaxNaniteBytes, FMantleMeshNaniteFile::MaxUploadBytes);
	TestEqual(TEXT("the inflated limit is 512 MiB"), FMantleMeshNaniteFile::MaxRawBytes(), int64(536870912));
	TestEqual(TEXT("the file is tied to the exact engine build, as the game compares it"), FMantleMeshNaniteFile::EngineTag(), FMantleNaniteSideFile::EngineTag());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMeshNaniteCoarserStepTest,
	"Mantle.Editor.Nanite.CoarserStepsByHand",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMeshNaniteCoarserStepTest::RunTest(const FString& Parameters)
{
	constexpr int64 MiB = 1024 * 1024;
	using FBake = FMantleMeshNaniteBake;
	// A file that fits is left as it is: no step.
	TestEqual(TEXT("64 MiB exactly fits"), FBake::NextKeepPercent(1.0f, 64 * MiB, 100 * MiB), 1.0f);
	// Two times over (128 MiB against 64): 0.9 / 2.
	TestEqual(TEXT("twice the limit steps to 0.45"), FBake::NextKeepPercent(1.0f, 128 * MiB, 200 * MiB), 0.45f);
	// Just over (65 MiB): 0.9 / (65 / 64), a step that still moves.
	TestTrue(TEXT("a little over steps a little"), FMath::IsNearlyEqual(FBake::NextKeepPercent(1.0f, 65 * MiB, 100 * MiB), 0.9f * 64.0f / 65.0f, 1e-5f));
	// A hundred times over: the step is limited to a fifth of what was kept.
	TestEqual(TEXT("far over steps to a fifth at most"), FBake::NextKeepPercent(1.0f, 6400 * MiB, 100 * MiB), 0.2f);
	// The inflated limit counts too: 1 GiB inflated against 512 MiB is two times over, though the compressed file is small.
	TestEqual(TEXT("the inflated size over its limit steps too"), FBake::NextKeepPercent(1.0f, 10 * MiB, 1024 * MiB), 0.45f);
	// A later step scales what is kept already: 0.45 kept, 96 MiB against 64 is 1.5 times over, so 0.45 * 0.9 / 1.5 = 0.45 * 0.6.
	TestTrue(TEXT("a step from a coarser bake scales it"), FMath::IsNearlyEqual(FBake::NextKeepPercent(0.45f, 96 * MiB, 100 * MiB), 0.45f * 0.6f, 1e-5f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMeshNaniteFitTest,
	"Mantle.Editor.Nanite.CoarserBakeFitsTheLimit",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMeshNaniteFitTest::RunTest(const FString& Parameters)
{
	using namespace MantleMeshNaniteBakeTestPrivate;

	if (!NaniteBuilds())
	{
		AddInfo(TEXT("skipped: this editor builds no Nanite data (started without a graphics device)"));
		return true;
	}

	// A mesh that fits its limit is baked once, at its own detail.
	UStaticMesh* Grid = MakeGrid(64, /*bNanite*/ false, 0);
	TArray<uint8> Whole;
	FString Why;
	FMantleNaniteFit WholeFit;
	if (!TestTrue(TEXT("a mesh bakes to its container"), FMantleMeshNaniteBake::BakeToFit(Grid, { 0 }, MeshTag, Whole, Why, WholeFit)))
	{
		AddError(Why);
		return false;
	}
	TestEqual(TEXT("in one bake"), WholeFit.Bakes, 1);
	TestFalse(TEXT("at its own detail"), WholeFit.IsCoarser());
	TestEqual(TEXT("and says what the first bake came to"), WholeFit.FirstPackedBytes, static_cast<int64>(Whole.Num()));
	AddInfo(FString::Printf(TEXT("the 64 x 64 grid: %d bytes uploaded, %lld inflated"), Whole.Num(), WholeFit.FirstRawBytes));

	// The same mesh against a limit a third smaller than that file: baked coarser, never refused, and it fits.
	const int64 Cap = static_cast<int64>(Whole.Num()) * 2 / 3;
	TArray<uint8> Coarse;
	FMantleNaniteFit CoarseFit;
	bool bRetryable = true;
	if (TestTrue(TEXT("against a smaller limit it still bakes"),
			FMantleMeshNaniteBake::BakeToFit(Grid, { 0 }, MeshTag, Coarse, Why, CoarseFit, &bRetryable, Cap, FMantleMeshNaniteFile::MaxRawBytes())))
	{
		AddInfo(FString::Printf(TEXT("under %lld bytes: %d bake(s), %.2f of its triangles, %d bytes"), Cap, CoarseFit.Bakes, CoarseFit.KeepPercentTriangles, Coarse.Num()));
		TestTrue(TEXT("with more than one bake"), CoarseFit.Bakes > 1);
		TestTrue(TEXT("coarser"), CoarseFit.IsCoarser() && CoarseFit.KeepPercentTriangles >= FMantleMeshNaniteBake::MinKeepPercentTriangles);
		TestTrue(TEXT("inside the limit"), Coarse.Num() <= Cap);
		TestTrue(TEXT("and smaller than the whole"), Coarse.Num() < Whole.Num());
		TestEqual(TEXT("the first bake is remembered as it came"), CoarseFit.FirstPackedBytes, WholeFit.FirstPackedBytes);
		Nanite::FResources Back;
		TArray<int32> Slots;
		TestTrue(TEXT("the file reads back like any other"), FMantleMeshNaniteFile::ReadUploaded(Coarse, MeshTag, Back, Slots, &Why));
		TestEqual(TEXT("with the table 0"), Slots, IdentityTable(1));
	}
	else
	{
		AddError(Why);
	}

	// The inflated size is limited as well as the compressed one.
	TArray<uint8> Inflated;
	if (FMantleMeshNaniteFile::Unpack(Whole, Inflated))
	{
		TArray<uint8> ByRaw;
		FMantleNaniteFit RawFit;
		const int64 RawCap = static_cast<int64>(Inflated.Num()) * 2 / 3;
		if (TestTrue(TEXT("against a smaller inflated limit it still bakes"),
				FMantleMeshNaniteBake::BakeToFit(Grid, { 0 }, MeshTag, ByRaw, Why, RawFit, nullptr, FMantleMeshNaniteFile::MaxUploadBytes, RawCap)))
		{
			TestTrue(TEXT("coarser"), RawFit.IsCoarser());
			TArray<uint8> Mnd1;
			TestTrue(TEXT("and its MND1 is inside the inflated limit"), FMantleMeshNaniteFile::Unpack(ByRaw, Mnd1) && Mnd1.Num() <= RawCap);
		}
		else
		{
			AddError(Why);
		}
	}

	// A limit nothing can meet: no file, not a refusal; the reason is the mesh's own, so the cache may keep it.
	TArray<uint8> None;
	FMantleNaniteFit NoneFit;
	bRetryable = true;
	Why.Reset();
	TestFalse(TEXT("a limit no bake can meet gives no file"),
		FMantleMeshNaniteBake::BakeToFit(Grid, { 0 }, MeshTag, None, Why, NoneFit, &bRetryable, 16, FMantleMeshNaniteFile::MaxRawBytes()));
	TestEqual(TEXT("with nothing to upload"), None.Num(), 0);
	TestFalse(TEXT("that belongs to the mesh"), bRetryable);
	TestTrue(TEXT("and says why in the creator's words"), Why.Contains(TEXT("compressed")) && Why.Contains(TEXT("even baked at")));
	TestTrue(TEXT("having tried more than once, and no more than the loop's bakes"), NoneFit.Bakes > 1 && NoneFit.Bakes <= FMantleMeshNaniteBake::MaxBakes);
	Grid->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMeshNaniteCoarserCheckTest,
	"Mantle.Editor.Nanite.ConverterTellsWhenItBakesCoarser",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMeshNaniteCoarserCheckTest::RunTest(const FString& Parameters)
{
	using namespace MantleMeshNaniteBakeTestPrivate;

	if (!NaniteBuilds())
	{
		AddInfo(TEXT("skipped: this editor builds no Nanite data (started without a graphics device)"));
		return true;
	}

	UStaticMesh* Grid = MakeGrid(64, /*bNanite*/ false, 0);
	const FString Dir = FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("MantleNaniteBake"));

	// What the grid's file is under the real limit.
	FMantlePlaceAssetConverter Whole(MakeShared<FMantleGltfMaterialFlattener>(), Dir);
	FMantlePlaceAssetSet WholeSet;
	if (!TestTrue(TEXT("the mesh converts"), Whole.ConvertMesh(Grid, WholeSet)) || !TestEqual(TEXT("to one mesh"), WholeSet.Meshes.Num(), 1) || WholeSet.Meshes[0].Nanite.Num() == 0)
	{
		return false;
	}
	TestEqual(TEXT("with no notice when it fits"), WholeSet.Issues.Num(), 0);
	const int64 WholeBytes = WholeSet.Meshes[0].Nanite.Num();

	// A limit a third smaller: the converter bakes it coarser, and a warning (not an error) says so, naming the mesh and the share kept.
	FMantlePlaceAssetConverter Coarse(MakeShared<FMantleGltfMaterialFlattener>(), Dir);
	Coarse.Limits.MaxNaniteBytes = WholeBytes * 2 / 3;
	FMantlePlaceAssetSet CoarseSet;
	if (TestTrue(TEXT("against that limit it converts"), Coarse.ConvertMesh(Grid, CoarseSet)) && TestEqual(TEXT("to one mesh"), CoarseSet.Meshes.Num(), 1))
	{
		const FMantleConvertedMesh& Converted = CoarseSet.Meshes[0];
		TestTrue(TEXT("with a Nanite file"), Converted.Nanite.Num() > 0);
		TestTrue(TEXT("inside the limit"), Converted.Nanite.Num() <= Coarse.Limits.MaxNaniteBytes);
		TestEqual(TEXT("and no reason for none"), Converted.NaniteNone, FString());
		TestFalse(TEXT("nothing is refused"), CoarseSet.HasErrors());
		if (TestEqual(TEXT("one notice"), CoarseSet.Issues.Num(), 1))
		{
			const FMantleValidationIssue& Issue = CoarseSet.Issues[0];
			TestTrue(TEXT("a warning"), Issue.Severity == EMantleIssueSeverity::Warning);
			TestTrue(TEXT("naming the mesh"), Issue.Problem.Contains(Grid->GetName()));
			TestTrue(TEXT("and the share of its triangles kept"), Issue.Problem.Contains(TEXT("% of its triangles")));
			TestFalse(TEXT("with a remedy"), Issue.Remedy.IsEmpty());
		}
		// The warning survives the local cache with the file: a republish says it again.
		const FString CacheDir = FreshCacheDir(TEXT("Coarse"));
		FSavedMesh Saved(TEXT("SM_Coarse"));
		if (TestTrue(TEXT("a saved mesh"), Saved.Adopt(MakeGrid(64, /*bNanite*/ false, 0))))
		{
			FMantlePlaceAssetConverter First(MakeShared<FMantleGltfMaterialFlattener>(), CacheDir);
			First.Limits.MaxNaniteBytes = WholeBytes * 2 / 3;
			FMantlePlaceAssetSet SetA;
			FMantlePlaceAssetConverter Second(MakeShared<FMantleGltfMaterialFlattener>(), CacheDir);
			Second.Limits.MaxNaniteBytes = WholeBytes * 2 / 3;
			FMantlePlaceAssetSet SetB;
			if (TestTrue(TEXT("it converts"), First.ConvertMesh(Saved.Mesh, SetA)) && TestTrue(TEXT("and again"), Second.ConvertMesh(Saved.Mesh, SetB)) && SetB.Meshes.Num() == 1)
			{
				TestTrue(TEXT("the second time from the cache"), SetB.Meshes[0].bFromCache);
				TestTrue(TEXT("with the same coarser file"), SetB.Meshes[0].Nanite == SetA.Meshes[0].Nanite);
				TestEqual(TEXT("and the same notices"), SetB.Issues.Num(), SetA.Issues.Num());
			}
			// A converter with another limit is another entry: the file is the final one for that limit.
			FMantlePlaceAssetConverter Other(MakeShared<FMantleGltfMaterialFlattener>(), CacheDir);
			FMantlePlaceAssetSet SetC;
			if (TestTrue(TEXT("under the real limit it converts"), Other.ConvertMesh(Saved.Mesh, SetC)) && SetC.Meshes.Num() == 1)
			{
				TestFalse(TEXT("not from the coarser entry"), SetC.Meshes[0].bFromCache);
				TestEqual(TEXT("and with no notice"), SetC.Issues.Num(), 0);
			}
			Saved.Mesh->MarkAsGarbage();
		}
	}

	// A limit no bake can meet: the model publishes plain, with a warning, never refused.
	FMantlePlaceAssetConverter Tiny(MakeShared<FMantleGltfMaterialFlattener>(), Dir);
	Tiny.Limits.MaxNaniteBytes = 16;
	FMantlePlaceAssetSet TinySet;
	if (TestTrue(TEXT("against a limit nothing meets it converts"), Tiny.ConvertMesh(Grid, TinySet)) && TestEqual(TEXT("to one mesh"), TinySet.Meshes.Num(), 1))
	{
		TestEqual(TEXT("with no Nanite file"), TinySet.Meshes[0].Nanite.Num(), 0);
		TestTrue(TEXT("and the reason"), TinySet.Meshes[0].NaniteNone.Contains(TEXT("even baked at")));
		TestFalse(TEXT("nothing is refused"), TinySet.HasErrors());
		if (TestEqual(TEXT("one notice"), TinySet.Issues.Num(), 1))
		{
			TestTrue(TEXT("a warning that says players see the plain model"), TinySet.Issues[0].Severity == EMantleIssueSeverity::Warning && TinySet.Issues[0].Problem.Contains(TEXT("plain model")));
		}
	}
	Grid->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMeshNaniteUploadTest,
	"Mantle.Editor.Nanite.UploadStageAndPlaceFile",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMeshNaniteUploadTest::RunTest(const FString& Parameters)
{
	using namespace MantleMeshNaniteBakeTestPrivate;

	if (!NaniteBuilds())
	{
		AddInfo(TEXT("skipped: this editor builds no Nanite data (started without a graphics device)"));
		return true;
	}

	UWorld* World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false);
	if (!TestNotNull(TEXT("a world"), World))
	{
		return false;
	}
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	UStaticMesh* Grid = MakeGrid(24, /*bNanite*/ false, 0);
	AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>();
	Actor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Static);
	Actor->GetStaticMeshComponent()->SetStaticMesh(Grid);

	FMantlePlaceAssetConverter Converter(MakeShared<FMantleGltfMaterialFlattener>(), FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("MantleNaniteBake")));
	FMantlePlaceAssetSet Set;
	if (TestTrue(TEXT("the mesh converts"), Converter.ConvertMesh(Grid, Set)) && TestEqual(TEXT("to one mesh"), Set.Meshes.Num(), 1) && Set.Meshes[0].Nanite.Num() > 0)
	{
		const FMantleConvertedMesh& Converted = Set.Meshes[0];
		const FString MeshKey = Converted.Key;
		const FString NaniteKey = Converted.NaniteKey();
		const FString NaniteHash = Converted.NaniteHash;
		const int64 NaniteBytes = Converted.Nanite.Num();
		const FString MeshHash = Converted.Hash;

		// Before anything is uploaded the place file names both files by their provisional ids.
		FMantleExportOptions Options;
		Options.Assets = &Set;
		const FMantleExportResult Before = FMantlePlaceExporter::Export(World, Options);
		const FMantleAssetRef* MeshRef = Before.Place.Assets.FindByPredicate([&MeshHash](const FMantleAssetRef& Ref) { return Ref.Kind == EMantleAssetKind::StaticMesh && Ref.Hash == MeshHash; });
		if (TestNotNull(TEXT("the mesh is listed"), MeshRef) && TestFalse(TEXT("naming a Nanite file"), MeshRef->Nanite.IsEmpty()))
		{
			const FString NaniteId = MeshRef->Nanite;
			const FMantleAssetRef* NaniteRef = Before.Place.Assets.FindByPredicate([&NaniteId](const FMantleAssetRef& Ref) { return Ref.Id == NaniteId; });
			if (TestNotNull(TEXT("which is listed"), NaniteRef))
			{
				TestTrue(TEXT("as a Nanite entry"), NaniteRef->Kind == EMantleAssetKind::Nanite);
				TestEqual(TEXT("saying the engine build it was baked for"), NaniteRef->Engine, FMantleNaniteSideFile::EngineTag());
				TestEqual(TEXT("with the container's hash"), NaniteRef->Hash, NaniteHash);
				TestEqual(TEXT("and the container's size"), NaniteRef->Bytes, NaniteBytes);
				TestTrue(TEXT("and the mesh has no Lumen file here, so only the Nanite one"), MeshRef->Lumen.IsEmpty() || MeshRef->Lumen != NaniteId);
			}
			const FString Json = FMantlePlaceExporter::ToJson(Before.Place);
			TestTrue(TEXT("the place file says kind Nanite"), Json.Contains(TEXT("\"kind\":\"Nanite\"")));
			TestTrue(TEXT("the mesh's entry names it"), Json.Contains(TEXT("\"nanite\":\"") + NaniteId + TEXT("\"")));
			TestTrue(TEXT("and the entry says its engine"), Json.Contains(TEXT("\"engine\":\"") + FMantleNaniteSideFile::EngineTag() + TEXT("\"")));
		}

		// The upload stage: the mesh and, beside it, the Nanite file under its own key with the container, its hash and kind "nanite".
		TArray<FMantlePlaceUpload> Uploads;
		FString Error;
		TestTrue(TEXT("the mesh stage is taken"), Set.TakeUploads(EMantlePlaceUploadStage::Mesh, Uploads, Error));
		const FMantlePlaceUpload* NaniteUpload = Uploads.FindByPredicate([&NaniteKey](const FMantlePlaceUpload& Upload) { return Upload.Key == NaniteKey; });
		if (TestNotNull(TEXT("a Nanite upload is in it"), NaniteUpload))
		{
			TestEqual(TEXT("of kind nanite"), NaniteUpload->Options.Kind, FString(TEXT("nanite")));
			TestEqual(TEXT("with its hash"), NaniteUpload->Hash, NaniteHash);
			TestEqual(TEXT("the container's bytes"), NaniteUpload->Bytes.Num(), static_cast<int32>(NaniteBytes));
			TestEqual(TEXT("hashed as Mantle hashes an upload"), FMantleSha256::AssetHash(NaniteUpload->Bytes), NaniteHash);
			TestTrue(TEXT("a container"), NaniteUpload->Bytes.Num() > 8 && FMemory::Memcmp(NaniteUpload->Bytes.GetData(), "MNZ1", 4) == 0);
			TestTrue(TEXT("inside the limit"), NaniteUpload->Bytes.Num() <= FMantleMeshNaniteFile::MaxUploadBytes);
			TestTrue(TEXT("named so the creator can tell what it is"), NaniteUpload->SourceName.Contains(TEXT("Nanite")));
			TestEqual(TEXT("in the same stage as its mesh"), static_cast<int32>(NaniteUpload->Stage), static_cast<int32>(EMantlePlaceUploadStage::Mesh));
		}
		TestEqual(TEXT("a mesh and its Nanite file"), Uploads.Num(), 2);
		TArray<FMantlePlaceUpload> Again;
		TestTrue(TEXT("a stage taken again"), Set.TakeUploads(EMantlePlaceUploadStage::Mesh, Again, Error));
		TestEqual(TEXT("gives nothing: what was taken is not sent twice"), Again.Num(), 0);

		// Uploaded: the place file lists the ids and hashes Mantle gave. A mesh whose Nanite file Mantle refused goes without.
		const FString MeshId = TEXT("ast_u0123456789abcdef0123456789abcdef");
		const FString UploadedNaniteId = TEXT("ast_ufedcba9876543210fedcba9876543210");
		const FString BackendHash = TEXT("sha256:") + FString::ChrN(64, TEXT('a'));
		Set.RecordUpload(MeshKey, MeshId, MeshHash, Set.UploadSize(MeshKey, 0));
		const FMantleExportResult Refused = FMantlePlaceExporter::Export(World, Options);
		const FMantleAssetRef* RefusedMesh = Refused.Place.Assets.FindByPredicate([&MeshId](const FMantleAssetRef& Ref) { return Ref.Id == MeshId; });
		if (TestNotNull(TEXT("the uploaded mesh is listed"), RefusedMesh))
		{
			TestTrue(TEXT("with no Nanite file when Mantle has none of its own"), RefusedMesh->Nanite.IsEmpty());
		}
		TestFalse(TEXT("and no Nanite entry"), Refused.Place.Assets.ContainsByPredicate([](const FMantleAssetRef& Ref) { return Ref.Kind == EMantleAssetKind::Nanite; }));
		Set.RecordUpload(NaniteKey, UploadedNaniteId, BackendHash, NaniteBytes);
		const FMantleExportResult After = FMantlePlaceExporter::Export(World, Options);
		const FMantleAssetRef* AfterMesh = After.Place.Assets.FindByPredicate([&MeshId](const FMantleAssetRef& Ref) { return Ref.Id == MeshId; });
		if (TestNotNull(TEXT("the uploaded mesh is still listed"), AfterMesh))
		{
			TestEqual(TEXT("naming the uploaded Nanite file"), AfterMesh->Nanite, UploadedNaniteId);
		}
		const FMantleAssetRef* AfterNanite = After.Place.Assets.FindByPredicate([&UploadedNaniteId](const FMantleAssetRef& Ref) { return Ref.Id == UploadedNaniteId; });
		if (TestNotNull(TEXT("and listing it"), AfterNanite))
		{
			TestEqual(TEXT("with the hash Mantle gave"), AfterNanite->Hash, BackendHash);
			TestEqual(TEXT("and the size Mantle gave"), AfterNanite->Bytes, NaniteBytes);
			TestEqual(TEXT("and the engine"), AfterNanite->Engine, FMantleNaniteSideFile::EngineTag());
		}
	}

	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	Grid->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMeshNaniteCacheTest,
	"Mantle.Editor.Nanite.CacheKeepsTheFileWithTheMesh",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMeshNaniteCacheTest::RunTest(const FString& Parameters)
{
	using namespace MantleMeshNaniteBakeTestPrivate;

	if (!NaniteBuilds())
	{
		AddInfo(TEXT("skipped: this editor builds no Nanite data (started without a graphics device)"));
		return true;
	}

	// A saved mesh converts twice: the second time from the cache, with the same Nanite file.
	const FString CacheDir = FreshCacheDir(TEXT("Mesh"));
	FSavedMesh Saved(TEXT("SM_Cached"));
	if (!TestTrue(TEXT("the mesh saves"), Saved.Adopt(MakeGrid(16, /*bNanite*/ false, 0, 2))))
	{
		return false;
	}
	FMantlePlaceAssetConverter First(MakeShared<FMantleGltfMaterialFlattener>(), CacheDir);
	FMantlePlaceAssetSet SetA;
	if (!TestTrue(TEXT("it converts"), First.ConvertMesh(Saved.Mesh, SetA)) || !TestEqual(TEXT("to one mesh"), SetA.Meshes.Num(), 1))
	{
		return false;
	}
	TestFalse(TEXT("the first time is a conversion"), SetA.Meshes[0].bFromCache);
	TestTrue(TEXT("with a Nanite file"), SetA.Meshes[0].Nanite.Num() > 0);

	FMantlePlaceAssetConverter Second(MakeShared<FMantleGltfMaterialFlattener>(), CacheDir);
	FMantlePlaceAssetSet SetB;
	if (!TestTrue(TEXT("it converts again"), Second.ConvertMesh(Saved.Mesh, SetB)) || !TestEqual(TEXT("to one mesh"), SetB.Meshes.Num(), 1))
	{
		return false;
	}
	TestTrue(TEXT("the second time is from the cache"), SetB.Meshes[0].bFromCache);
	TestTrue(TEXT("with the same Nanite file"), SetB.Meshes[0].Nanite == SetA.Meshes[0].Nanite);
	TestEqual(TEXT("and hash"), SetB.Meshes[0].NaniteHash, SetA.Meshes[0].NaniteHash);

	// A converter that makes no Nanite files (a weapon or wheel publish) keeps entries of its own: they say "no file", so a place
	// conversion must never read one (its salt says nanite=off), and it must not disturb the place's entry either.
	FMantlePlaceAssetConverter Plain(MakeShared<FMantleGltfMaterialFlattener>(), CacheDir);
	Plain.bBakeNanite = false;
	FMantlePlaceAssetSet SetD;
	if (TestTrue(TEXT("it converts without Nanite"), Plain.ConvertMesh(Saved.Mesh, SetD)) && TestEqual(TEXT("to one mesh"), SetD.Meshes.Num(), 1))
	{
		TestFalse(TEXT("its first time is a conversion, not the place's entry"), SetD.Meshes[0].bFromCache);
		TestEqual(TEXT("with no Nanite file"), SetD.Meshes[0].Nanite.Num(), 0);
	}
	FMantlePlaceAssetConverter PlainAgain(MakeShared<FMantleGltfMaterialFlattener>(), CacheDir);
	PlainAgain.bBakeNanite = false;
	FMantlePlaceAssetSet SetE;
	if (TestTrue(TEXT("it converts without Nanite again"), PlainAgain.ConvertMesh(Saved.Mesh, SetE)) && TestEqual(TEXT("to one mesh"), SetE.Meshes.Num(), 1))
	{
		TestTrue(TEXT("and now from the cache (weapon and wheel publishes are cached too)"), SetE.Meshes[0].bFromCache);
		TestEqual(TEXT("still with no Nanite file"), SetE.Meshes[0].Nanite.Num(), 0);
	}
	FMantlePlaceAssetConverter PlaceAgain(MakeShared<FMantleGltfMaterialFlattener>(), CacheDir);
	FMantlePlaceAssetSet SetF;
	if (TestTrue(TEXT("a place conversion converts after them"), PlaceAgain.ConvertMesh(Saved.Mesh, SetF)) && TestEqual(TEXT("to one mesh"), SetF.Meshes.Num(), 1))
	{
		TestTrue(TEXT("from its own entry"), SetF.Meshes[0].bFromCache);
		TestTrue(TEXT("which still has the Nanite file"), SetF.Meshes[0].Nanite == SetA.Meshes[0].Nanite);
	}

	// A change to the mesh is a miss.
	FMeshNaniteSettings Settings = Saved.Mesh->GetNaniteSettings();
	Settings.bEnabled = true;
	Saved.Mesh->SetNaniteSettings(Settings);
	if (TestTrue(TEXT("the changed mesh saves"), Saved.Save()))
	{
		FMantlePlaceAssetConverter Third(MakeShared<FMantleGltfMaterialFlattener>(), CacheDir);
		FMantlePlaceAssetSet SetC;
		if (TestTrue(TEXT("it converts a third time"), Third.ConvertMesh(Saved.Mesh, SetC)) && TestEqual(TEXT("to one mesh"), SetC.Meshes.Num(), 1))
		{
			TestFalse(TEXT("a changed mesh is not served from the cache"), SetC.Meshes[0].bFromCache);
		}
	}
	Saved.Mesh->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMeshNaniteAssemblyCacheTest,
	"Mantle.Editor.Nanite.AssemblyPartsAreInTheCacheKey",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMeshNaniteAssemblyCacheTest::RunTest(const FString& Parameters)
{
	using namespace MantleMeshNaniteBakeTestPrivate;

	if (!NaniteBuilds() || !NaniteAssembliesSupported())
	{
		AddInfo(TEXT("skipped: this editor builds no Nanite data or assemblies"));
		return true;
	}

	// An assembly names its parts by soft path, which the cache key does not follow: an edited part must still miss.
	FSavedMesh Part(TEXT("SM_CachePart"));
	if (!TestTrue(TEXT("the part saves"), Part.Adopt(MakeGrid(16, /*bNanite*/ true, 0, 2))))
	{
		return false;
	}
	UMaterialInterface* Surface = UMaterial::GetDefaultMaterial(MD_Surface);
	FNaniteAssemblyDataBuilder Builder;
	Builder.AddMaterialSlot(FNaniteAssemblyDataBuilder::FMaterialSlot(Surface, TEXT("Bark")));
	Builder.AddMaterialSlot(FNaniteAssemblyDataBuilder::FMaterialSlot(Surface, TEXT("Leaf")));
	const int32 PartIndex = Builder.AddPart(FSoftObjectPath(Part.Mesh));
	Builder.RemapPartMaterial(PartIndex, 0, 1);
	Builder.RemapPartMaterial(PartIndex, 1, 0);
	Builder.AddNode(PartIndex, FTransform3f::Identity);
	Builder.AddNode(PartIndex, FTransform3f(FVector3f(500.f, 0.f, 0.f)));

	FSavedMesh Assembly(TEXT("SM_CacheAssembly"));
	Assembly.Package = CreatePackage(*Assembly.PackageName);
	Assembly.Mesh = NewObject<UStaticMesh>(Assembly.Package, TEXT("SM_CacheAssembly"), RF_Public | RF_Standalone);
	if (!TestTrue(TEXT("the assembly applies"), Builder.ApplyToStaticMesh(*Assembly.Mesh)))
	{
		return false;
	}
	Assembly.Mesh->GetNaniteSettings().bEnabled = true;
	Assembly.Mesh->Build(/*bInSilent*/ true);
	FStaticMeshCompilingManager::Get().FinishCompilation({ Assembly.Mesh });
	FAssetRegistryModule::AssetCreated(Assembly.Mesh);
	if (!TestTrue(TEXT("the assembly saves"), Assembly.Save()))
	{
		return false;
	}
	TestEqual(TEXT("its cache objects are the assembly and its part"), FMantleMeshNaniteBake::CacheObjects(*Assembly.Mesh).Num(), 2);

	const FString CacheDir = FreshCacheDir(TEXT("Assembly"));
	FMantlePlaceAssetConverter First(MakeShared<FMantleGltfMaterialFlattener>(), CacheDir);
	FMantlePlaceAssetSet SetA;
	if (!TestTrue(TEXT("the assembly converts"), First.ConvertMesh(Assembly.Mesh, SetA)) || !TestEqual(TEXT("to one mesh"), SetA.Meshes.Num(), 1))
	{
		return false;
	}
	TestTrue(TEXT("with a Nanite file"), SetA.Meshes[0].Nanite.Num() > 0);
	FMantlePlaceAssetConverter Second(MakeShared<FMantleGltfMaterialFlattener>(), CacheDir);
	FMantlePlaceAssetSet SetB;
	if (TestTrue(TEXT("it converts again"), Second.ConvertMesh(Assembly.Mesh, SetB)) && TestEqual(TEXT("to one mesh"), SetB.Meshes.Num(), 1))
	{
		TestTrue(TEXT("from the cache while nothing changed"), SetB.Meshes[0].bFromCache);
	}

	FMeshNaniteSettings Settings = Part.Mesh->GetNaniteSettings();
	Settings.TrimRelativeError = 0.25f;
	Part.Mesh->SetNaniteSettings(Settings);
	if (TestTrue(TEXT("the changed part saves"), Part.Save()))
	{
		FMantlePlaceAssetConverter Third(MakeShared<FMantleGltfMaterialFlattener>(), CacheDir);
		FMantlePlaceAssetSet SetC;
		if (TestTrue(TEXT("it converts a third time"), Third.ConvertMesh(Assembly.Mesh, SetC)) && TestEqual(TEXT("to one mesh"), SetC.Meshes.Num(), 1))
		{
			TestFalse(TEXT("an edited part is not served from the cache"), SetC.Meshes[0].bFromCache);
		}
	}
	Assembly.Mesh->MarkAsGarbage();
	Part.Mesh->MarkAsGarbage();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
