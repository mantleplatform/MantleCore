// Copyright Joshua Eger. All Rights Reserved.
#include "MantleMeshNaniteBake.h"
#include "MantleMirrorBake.h"
#include "MantlePlaceAssetConverter.h"
#include "MantlePlaceAssets.h"
#include "MantleSha256.h"
#include "NaniteAssemblyDataBuilder.h"

#include "Algo/Reverse.h"
#include "Engine/StaticMesh.h"
#include "HAL/IConsoleManager.h"
#include "Materials/Material.h"
#include "MeshDescription.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Rendering/NaniteResources.h"
#include "RenderUtils.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshCompiler.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"

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
	TestFalse(TEXT("a see-through mesh does not bake"), FMantleMeshNaniteBake::Bake(Glass, { 0 }, MeshTag, File, Why));
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
	if (TestTrue(TEXT("the file reads back for the GLB's hash"), FMantleMeshNaniteFile::Read(Converted.Nanite, Converted.Hash, Back, Slots, &Why)))
	{
		TestEqual(TEXT("with all the grid's triangles"), Back.NumInputTriangles, 24u * 24u * 2u);
		TestEqual(TEXT("one table entry for each glTF material"), Slots.Num(), Converted.Slots.Num());
	}
	else
	{
		AddError(Why);
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

	// The cache key carries the Nanite inputs: the format, the engine build, the voxel and assembly switches.
	const FString Salt = FMantleMeshNaniteBake::CacheSalt();
	TestTrue(TEXT("the salt names the engine build"), Salt.Contains(FMantleMeshNaniteFile::EngineTag()));
	TestTrue(TEXT("and the voxel and assembly switches"), Salt.Contains(TEXT("voxels=1")) && Salt.Contains(TEXT("assemblies=1")));
	Grid->MarkAsGarbage();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
