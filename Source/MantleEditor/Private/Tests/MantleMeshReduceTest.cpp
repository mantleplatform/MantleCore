// Copyright Joshua Eger. All Rights Reserved.
#include "MantleMeshReduce.h"
#include "MantleMirrorBake.h"
#include "MantlePlaceAssetConverter.h"
#include "MantlePlaceAssets.h"

#include "Engine/StaticMesh.h"
#include "IMeshReductionInterfaces.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "MeshReductionSettings.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS

// An over-limit place mesh publishes from a smaller version of itself: its first lower LOD that fits, else a
// transient copy reduced with the editor's mesh reduction, each with a warning, and never a change to the creator's mesh.
// The tests use small generated meshes and small limits (a 2,048-triangle LOD 0, an 800-triangle LOD 1, a limit of 1,500)
// instead of the real 500,000.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleMeshReduceTestPrivate
{
	/** LOD 0 is a 32 x 32 grid of quads (2,048 triangles), LOD 1 a 20 x 20 grid (800). */
	constexpr int32 BigGrid = 32;
	constexpr int32 SmallGrid = 20;
	constexpr int32 BigTriangles = 2 * BigGrid * BigGrid;
	constexpr int32 SmallTriangles = 2 * SmallGrid * SmallGrid;
	constexpr int32 TriangleLimit = 1500;

	/** A reducer that gives back what it is given: it never gets a mesh under a limit. */
	class FNoReduceReducer : public IMeshReduction
	{
	public:
		virtual void ReduceMeshDescription(FMeshDescription& OutReducedMesh, float& OutMaxDeviation, const FMeshDescription& InMesh,
			const FOverlappingCorners& InOverlappingCorners, const struct FMeshReductionSettings& ReductionSettings) override
		{
			OutReducedMesh = InMesh;
			OutMaxDeviation = 0.0f;
		}

		virtual bool ReduceSkeletalMesh(class USkeletalMesh* SkeletalMesh, int32 LODIndex, const class ITargetPlatform* TargetPlatform) override { return false; }

		virtual const FString& GetVersionString() const override
		{
			static const FString Version = TEXT("MantleTestNoReduce_1");
			return Version;
		}

		virtual bool IsSupported() const override { return true; }
		virtual bool IsReductionActive(const struct FMeshReductionSettings& ReductionSettings) const override { return true; }
		virtual bool IsReductionActive(const struct FMeshReductionSettings& ReductionSettings, uint32 NumVertices, uint32 NumTriangles) const override { return true; }
		virtual bool IsReductionActive(const struct FSkeletalMeshOptimizationSettings& ReductionSettings) const override { return false; }
		virtual bool IsReductionActive(const struct FSkeletalMeshOptimizationSettings& ReductionSettings, uint32 NumVertices, uint32 NumTriangles) const override { return false; }
	};

	/** Builds the meshes, and puts the reducer and the caches back either side of a test. */
	struct FReduceFixture
	{
		TStrongObjectPtr<UMaterialInterface> Grid;
		TStrongObjectPtr<UMaterialInterface> Basic;
		TArray<TStrongObjectPtr<UStaticMesh>> Meshes;

		FReduceFixture()
		{
			Reset();
			Grid.Reset(LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial")));
			Basic.Reset(LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")));
		}

		~FReduceFixture() { Reset(); }

		static void Reset()
		{
			FMantleMeshReduce::SetReducerForTests(nullptr);
			FMantleMeshReduce::SetUnavailableForTests(false);
			FMantleMeshReduce::ClearCache();
			FMantleMirrorBake::ClearCache();
		}

		bool IsReady() const { return Grid.IsValid() && Basic.IsValid() && Grid.Get() != Basic.Get(); }

		static FName SlotNameOf(int32 Slot) { return FName(*FString::Printf(TEXT("MantleTestSlot_%d"), Slot)); }

		/**
		 * One LOD of a Size x Size grid of quads over a gentle surface (so a reduction has shape to keep): with two materials its
		 * first half of rows draws with slot 0 and the rest with slot 1.
		 */
		static void AddGrid(FMeshDescription& Description, int32 Size, int32 NumMaterials)
		{
			FStaticMeshAttributes Attributes(Description);
			Attributes.Register();
			TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
			TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
			TVertexInstanceAttributesRef<FVector3f> Tangents = Attributes.GetVertexInstanceTangents();
			TVertexInstanceAttributesRef<float> BinormalSigns = Attributes.GetVertexInstanceBinormalSigns();
			TVertexInstanceAttributesRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();
			TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
			TPolygonGroupAttributesRef<FName> GroupNames = Attributes.GetPolygonGroupMaterialSlotNames();
			UVs.SetNumChannels(1);

			TArray<FPolygonGroupID> Groups;
			for (int32 Material = 0; Material < NumMaterials; ++Material)
			{
				const FPolygonGroupID Group = Description.CreatePolygonGroup();
				GroupNames[Group] = SlotNameOf(Material);
				Groups.Add(Group);
			}

			const float Step = 10.0f;
			TArray<FVertexID> Vertices;
			for (int32 Y = 0; Y <= Size; ++Y)
			{
				for (int32 X = 0; X <= Size; ++X)
				{
					const FVertexID Vertex = Description.CreateVertex();
					const float PositionX = static_cast<float>(X) / Size * Step * BigGrid;
					const float PositionY = static_cast<float>(Y) / Size * Step * BigGrid;
					Positions[Vertex] = FVector3f(PositionX, PositionY, 20.0f * FMath::Sin(PositionX * 0.05f) * FMath::Cos(PositionY * 0.07f));
					Vertices.Add(Vertex);
				}
			}

			auto AddTriangle = [&](const FPolygonGroupID Group, int32 A, int32 B, int32 C)
			{
				TArray<FVertexInstanceID> Corners;
				for (const int32 Index : { A, B, C })
				{
					const FVertexInstanceID Instance = Description.CreateVertexInstance(Vertices[Index]);
					const FVector3f Position = Positions[Vertices[Index]];
					Normals[Instance] = FVector3f(0.0f, 0.0f, 1.0f);
					Tangents[Instance] = FVector3f(1.0f, 0.0f, 0.0f);
					BinormalSigns[Instance] = 1.0f;
					Colors[Instance] = FVector4f(1.0f, 1.0f, 1.0f, 1.0f);
					UVs.Set(Instance, 0, FVector2f(Position.X / (Step * BigGrid), Position.Y / (Step * BigGrid)));
					Corners.Add(Instance);
				}
				Description.CreateTriangle(Group, Corners);
			};
			for (int32 Y = 0; Y < Size; ++Y)
			{
				const FPolygonGroupID Group = Groups[NumMaterials == 2 && Y >= Size / 2 ? 1 : 0];
				for (int32 X = 0; X < Size; ++X)
				{
					const int32 V00 = Y * (Size + 1) + X;
					const int32 V10 = V00 + 1;
					const int32 V01 = V00 + Size + 1;
					const int32 V11 = V01 + 1;
					AddTriangle(Group, V00, V10, V11);
					AddTriangle(Group, V00, V11, V01);
				}
			}
		}

		/** A transient mesh with one LOD per entry of Grids (a grid size) and MaterialsPerLOD (1 or 2 slots drawn), over both materials. Kept alive by the fixture. */
		UStaticMesh* MakeMesh(const TArray<int32>& Grids, const TArray<int32>& MaterialsPerLOD)
		{
			UObject* Outer = GetTransientPackage();
			UStaticMesh* Mesh = NewObject<UStaticMesh>(Outer, MakeUniqueObjectName(Outer, UStaticMesh::StaticClass(), FName(TEXT("MantleReduceTestMesh"))), RF_Transient);
			UMaterialInterface* Materials[2] = { Grid.Get(), Basic.Get() };
			for (int32 Slot = 0; Slot < 2; ++Slot)
			{
				Mesh->GetStaticMaterials().Add(FStaticMaterial(Materials[Slot], FName(*FString::Printf(TEXT("Surface%d"), Slot)), SlotNameOf(Slot)));
			}
			Mesh->bAllowCPUAccess = true;
			Mesh->SetNumSourceModels(Grids.Num());

			TArray<FMeshDescription> Descriptions;
			Descriptions.SetNum(Grids.Num());
			for (int32 LOD = 0; LOD < Grids.Num(); ++LOD)
			{
				AddGrid(Descriptions[LOD], Grids[LOD], MaterialsPerLOD[LOD]);
				// SetNumSourceModels gives a new LOD without a mesh description yet a 50% reduction of the LOD before it; the build would
				// then replace this LOD's own description with that reduction (a 1,024-triangle LOD 1 instead of the 800 built here).
				Mesh->GetSourceModel(LOD).ReductionSettings = FMeshReductionSettings();
				FMeshBuildSettings& Settings = Mesh->GetSourceModel(LOD).BuildSettings;
				Settings.bRecomputeNormals = false;
				Settings.bRecomputeTangents = false;
				Settings.bGenerateLightmapUVs = false;
				Settings.bBuildReversedIndexBuffer = false;
				Settings.DistanceFieldResolutionScale = 0.0f;
				Settings.MaxLumenMeshCards = 0;
			}

			UStaticMesh::FBuildMeshDescriptionsParams Params;
			Params.bMarkPackageDirty = false;
			Params.bAllowCpuAccess = true;
			TArray<const FMeshDescription*> Pointers;
			for (const FMeshDescription& Description : Descriptions)
			{
				Pointers.Add(&Description);
			}
			Mesh->BuildFromMeshDescriptions(Pointers, Params);
			Meshes.Add(TStrongObjectPtr<UStaticMesh>(Mesh));
			return Mesh;
		}
	};

	/** The numbers a test compares before and after to show the creator's mesh was not touched. */
	struct FMeshSnapshot
	{
		FGuid LightingGuid;
		int32 NumSourceModels = 0;
		TArray<int32> TrianglesPerLOD;
		TArray<const UMaterialInterface*> Materials;
		bool bDirty = false;

		explicit FMeshSnapshot(const UStaticMesh& Mesh)
			: LightingGuid(Mesh.GetLightingGuid())
			, NumSourceModels(Mesh.GetNumSourceModels())
			, bDirty(Mesh.GetOutermost()->IsDirty())
		{
			for (const FStaticMeshLODResources& LOD : Mesh.GetRenderData()->LODResources)
			{
				TrianglesPerLOD.Add(LOD.GetNumTriangles());
			}
			for (const FStaticMaterial& Slot : Mesh.GetStaticMaterials())
			{
				Materials.Add(Slot.MaterialInterface);
			}
		}

		bool operator==(const FMeshSnapshot& Other) const
		{
			return LightingGuid == Other.LightingGuid && NumSourceModels == Other.NumSourceModels && TrianglesPerLOD == Other.TrianglesPerLOD
				&& Materials == Other.Materials && bDirty == Other.bDirty;
		}
	};

	int32 CountSeverity(const TArray<FMantleValidationIssue>& Issues, EMantleIssueSeverity Severity)
	{
		int32 Count = 0;
		for (const FMantleValidationIssue& Issue : Issues)
		{
			Count += Issue.Severity == Severity ? 1 : 0;
		}
		return Count;
	}

	const FMantleValidationIssue* FindIssue(const TArray<FMantleValidationIssue>& Issues, EMantleIssueSeverity Severity, const TCHAR* Text)
	{
		return Issues.FindByPredicate([Severity, Text](const FMantleValidationIssue& Issue)
		{
			return Issue.Severity == Severity && Issue.Problem.Contains(Text, ESearchCase::CaseSensitive);
		});
	}

	FMantlePlaceAssetLimits SmallLimits()
	{
		FMantlePlaceAssetLimits Limits;
		Limits.MaxTriangles = TriangleLimit;
		return Limits;
	}

	/** Where a test's converter would cache: nothing is, for a transient mesh. */
	FString CacheDir()
	{
		return FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("MantleMeshReduce"));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMeshReducePromotesLODTest,
	"Mantle.Editor.MeshReduce.PublishesFromALowerLODThatFits",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMeshReducePromotesLODTest::RunTest(const FString& Parameters)
{
	using namespace MantleMeshReduceTestPrivate;
	FReduceFixture Fixture;
	if (!TestTrue(TEXT("two engine materials load"), Fixture.IsReady()))
	{
		return false;
	}
	UStaticMesh* Mesh = Fixture.MakeMesh({ BigGrid, SmallGrid }, { 2, 2 });
	const FMantlePlaceAssetLimits Limits = SmallLimits();
	const FMeshSnapshot Before(*Mesh);
	if (!TestTrue(TEXT("the mesh has two LODs, the first over the limit and the second within it"),
		Before.TrianglesPerLOD.Num() == 2 && Before.TrianglesPerLOD[0] == BigTriangles && Before.TrianglesPerLOD[1] == SmallTriangles))
	{
		return false;
	}

	TestEqual(TEXT("LOD 1 is the one to publish"), FMantlePlaceAssets::PublishedLODIndex(Mesh, Limits), 1);

	TArray<FMantleValidationIssue> Issues;
	FMantlePlaceAssets::CheckMesh(Mesh, FString(), Limits, Issues);
	TestEqual(TEXT("Check gives no error"), CountSeverity(Issues, EMantleIssueSeverity::Error), 0);
	TestEqual(TEXT("and one warning"), Issues.Num(), 1);
	if (const FMantleValidationIssue* Warning = FindIssue(Issues, EMantleIssueSeverity::Warning, TEXT("so its LOD 1 (800 triangles) is published instead.")))
	{
		TestTrue(TEXT("it names the triangles and the limit"), Warning->Problem.Contains(TEXT("has 2,048 triangles, more than the 1,500 a place mesh may have,")));
		TestFalse(TEXT("and says what to do"), Warning->Remedy.IsEmpty());
	}
	else
	{
		AddError(TEXT("the warning names LOD 1 and its triangles: ") + (Issues.Num() > 0 ? Issues[0].Problem : FString(TEXT("(no issue)"))));
	}

	// Conversion starts from that LOD, in LOD 0's slot order, and says nothing more (Check's warning is the one the creator sees).
	FMantlePlaceAssetConverter Converter(MakeShared<FMantleGltfMaterialFlattener>(), CacheDir());
	Converter.Limits = Limits;
	FMantlePlaceAssetSet Set;
	if (!TestTrue(TEXT("the mesh converts"), Converter.ConvertMesh(Mesh, Set)) || !TestEqual(TEXT("to one mesh"), Set.Meshes.Num(), 1))
	{
		return false;
	}
	const FMantleConvertedMesh& Converted = Set.Meshes[0];
	TestEqual(TEXT("its LOD 0 is the source's LOD 1"), Converted.Triangles, SmallTriangles);
	TestEqual(TEXT("with no LOD after it"), Converted.LODs, 1);
	TestTrue(TEXT("and the slots LOD 0 draws with, in LOD 0's order"), Converted.Slots == FMantlePlaceAssets::UploadSlots(Mesh));
	TestEqual(TEXT("conversion adds no notice of its own"), Set.Issues.Num(), 0);

	TestTrue(TEXT("the creator's mesh is neither changed nor made dirty"), FMeshSnapshot(*Mesh) == Before);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMeshReduceSlotMismatchTest,
	"Mantle.Editor.MeshReduce.ALowerLODWithOtherMaterialsIsNotPublished",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMeshReduceSlotMismatchTest::RunTest(const FString& Parameters)
{
	using namespace MantleMeshReduceTestPrivate;
	FReduceFixture Fixture;
	if (!TestTrue(TEXT("two engine materials load"), Fixture.IsReady()))
	{
		return false;
	}
	// LOD 0 draws with both slots, LOD 1 with one only: the place's materials follow LOD 0's slots, so LOD 1 can't stand in.
	UStaticMesh* Mesh = Fixture.MakeMesh({ BigGrid, SmallGrid }, { 2, 1 });
	const FMantlePlaceAssetLimits Limits = SmallLimits();
	TestEqual(TEXT("no LOD is published"), FMantlePlaceAssets::PublishedLODIndex(Mesh, Limits), static_cast<int32>(INDEX_NONE));

	// With no reducer either, the error stays as it was.
	FMantleMeshReduce::SetUnavailableForTests(true);
	TArray<FMantleValidationIssue> Issues;
	FMantlePlaceAssets::CheckMesh(Mesh, FString(), Limits, Issues);
	TestEqual(TEXT("one issue"), Issues.Num(), 1);
	TestTrue(TEXT("the error about the triangles"), FindIssue(Issues, EMantleIssueSeverity::Error, TEXT("has 2,048 triangles; a place mesh may have at most 1,500.")) != nullptr);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMeshReduceFittingMeshTest,
	"Mantle.Editor.MeshReduce.AMeshThatFitsKeepsItsKeyAndHash",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMeshReduceFittingMeshTest::RunTest(const FString& Parameters)
{
	using namespace MantleMeshReduceTestPrivate;
	FReduceFixture Fixture;
	if (!TestTrue(TEXT("two engine materials load"), Fixture.IsReady()))
	{
		return false;
	}
	UStaticMesh* Mesh = Fixture.MakeMesh({ SmallGrid }, { 2 });
	const FMantlePlaceAssetLimits Limits = SmallLimits();
	TestEqual(TEXT("LOD 0 is published"), FMantlePlaceAssets::PublishedLODIndex(Mesh, Limits), 0);

	TArray<FMantleValidationIssue> Issues;
	FMantlePlaceAssets::CheckMesh(Mesh, FString(), Limits, Issues);
	TestEqual(TEXT("Check says nothing"), Issues.Num(), 0);

	// The cache salt of a mesh that fits adds nothing to the salt the limits alone give.
	TArray<FString> Caps;
	for (const int32 Cap : Limits.LODMaxTriangles)
	{
		Caps.Add(FString::FromInt(Cap));
	}
	const FString Original = FString::Printf(TEXT("mesh lods=%d/%s collision=%d/%d/%d/%d bytes=%lld %s"), Limits.MaxLODs, *FString::Join(Caps, TEXT(",")),
		Limits.MaxCollisionPrimitives, Limits.MaxConvexHulls, Limits.MaxHullVertices, Limits.MaxTotalHullVertices, Limits.MaxUploadBytes, *Mesh->GetPathName());
	TestEqual(TEXT("the salt of a mesh that fits is unchanged"), FMantlePlaceAssetConverter::MeshCacheSalt(Limits, *Mesh, 0, FString()), Original);
	TestEqual(TEXT("a mesh published from LOD 2 says so"), FMantlePlaceAssetConverter::MeshCacheSalt(Limits, *Mesh, 2, FString()), Original + TEXT(" fromlod=2"));
	TestEqual(TEXT("a reduced mesh says so"), FMantlePlaceAssetConverter::MeshCacheSalt(Limits, *Mesh, 0, TEXT("1350/x")), Original + TEXT(" reduced=1350/x"));

	// And it converts to the same bytes every time, from its own LOD 0.
	FMantlePlaceAssetConverter First(MakeShared<FMantleGltfMaterialFlattener>(), CacheDir());
	FMantlePlaceAssetConverter Second(MakeShared<FMantleGltfMaterialFlattener>(), CacheDir());
	First.Limits = Limits;
	Second.Limits = Limits;
	FMantlePlaceAssetSet SetA;
	FMantlePlaceAssetSet SetB;
	if (!TestTrue(TEXT("it converts"), First.ConvertMesh(Mesh, SetA) && Second.ConvertMesh(Mesh, SetB)) || SetA.Meshes.Num() != 1 || SetB.Meshes.Num() != 1)
	{
		return false;
	}
	TestEqual(TEXT("from its own triangles"), SetA.Meshes[0].Triangles, SmallTriangles);
	TestEqual(TEXT("with the same hash both times"), SetA.Meshes[0].Hash, SetB.Meshes[0].Hash);
	TestTrue(TEXT("in the slot order it has"), SetA.Meshes[0].Slots == FMantlePlaceAssets::UploadSlots(Mesh));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMeshReduceReducesACopyTest,
	"Mantle.Editor.MeshReduce.ReducesACopyWhenNoLODFits",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMeshReduceReducesACopyTest::RunTest(const FString& Parameters)
{
	using namespace MantleMeshReduceTestPrivate;
	FReduceFixture Fixture;
	if (!TestTrue(TEXT("two engine materials load"), Fixture.IsReady()))
	{
		return false;
	}
	if (!FMantleMeshReduce::IsAvailable())
	{
		AddWarning(TEXT("this editor has no static mesh reducer, so the reduction was not tested"));
		return true;
	}
	UStaticMesh* Mesh = Fixture.MakeMesh({ BigGrid }, { 2 });
	const FMantlePlaceAssetLimits Limits = SmallLimits();
	const FMeshSnapshot Before(*Mesh);
	TestEqual(TEXT("the mesh has the one LOD, over the limit"), Before.TrianglesPerLOD.Num() == 1 ? Before.TrianglesPerLOD[0] : 0, BigTriangles);
	TestEqual(TEXT("no LOD fits"), FMantlePlaceAssets::PublishedLODIndex(Mesh, Limits), static_cast<int32>(INDEX_NONE));

	TArray<FMantleValidationIssue> Issues;
	FMantlePlaceAssets::CheckMesh(Mesh, FString(), Limits, Issues);
	TestEqual(TEXT("Check gives no error"), CountSeverity(Issues, EMantleIssueSeverity::Error), 0);
	TestEqual(TEXT("and one warning"), Issues.Num(), 1);
	if (const FMantleValidationIssue* Warning = FindIssue(Issues, EMantleIssueSeverity::Warning, TEXT("had 2,048 triangles, more than the 1,500 a place mesh may have, so Mantle reduced a copy to ")))
	{
		TestTrue(TEXT("it says the mesh is unchanged"), Warning->Problem.EndsWith(TEXT(" triangles for publishing. Your mesh is unchanged.")));
	}
	else
	{
		AddError(TEXT("the warning names the triangles before and the reduced copy: ") + (Issues.Num() > 0 ? Issues[0].Problem : FString(TEXT("(no issue)"))));
	}

	FString Why;
	UStaticMesh* Reduced = FMantleMeshReduce::ReducedCopy(Mesh, Limits, Why);
	if (!TestNotNull(TEXT("a reduced copy: ") + Why, Reduced))
	{
		return false;
	}
	TestTrue(TEXT("Gather, Check and Export see one copy"), FMantleMeshReduce::ReducedCopy(Mesh, Limits, Why) == Reduced);
	TestTrue(TEXT("it is transient"), Reduced->HasAnyFlags(RF_Transient) && Reduced->GetOutermost() == GetTransientPackage());
	FMantleMeshLODCount Count;
	FMantlePlaceAssets::CountMeshLOD(Reduced, 0, Count);
	TestTrue(FString::Printf(TEXT("it has at most %d triangles (%d)"), TriangleLimit, Count.Triangles), Count.Triangles > 0 && Count.Triangles <= TriangleLimit);
	TestEqual(TEXT("one LOD"), Reduced->GetRenderData()->LODResources.Num(), 1);
	TestTrue(TEXT("with the source's slots in use"), Count.Slots == FMantlePlaceAssets::UploadSlots(Mesh));
	if (TestEqual(TEXT("and its two material slots"), Reduced->GetStaticMaterials().Num(), 2))
	{
		for (int32 Slot = 0; Slot < 2; ++Slot)
		{
			TestTrue(TEXT("each with the source's material"), Reduced->GetStaticMaterials()[Slot].MaterialInterface == Mesh->GetStaticMaterials()[Slot].MaterialInterface);
			TestTrue(TEXT("and slot name"), Reduced->GetStaticMaterials()[Slot].MaterialSlotName == Mesh->GetStaticMaterials()[Slot].MaterialSlotName);
		}
	}
	TestTrue(TEXT("the source is neither changed nor made dirty"), FMeshSnapshot(*Mesh) == Before);

	// Conversion writes the copy under the source's key and in the source's order.
	FMantlePlaceAssetConverter Converter(MakeShared<FMantleGltfMaterialFlattener>(), CacheDir());
	Converter.Limits = Limits;
	FMantlePlaceAssetSet Set;
	if (!TestTrue(TEXT("the mesh converts"), Converter.ConvertMesh(Mesh, Set)) || !TestEqual(TEXT("to one mesh"), Set.Meshes.Num(), 1))
	{
		return false;
	}
	const FMantleConvertedMesh& Converted = Set.Meshes[0];
	TestEqual(TEXT("under the source's key"), Converted.Key, FMantlePlaceAssets::MeshKey(Mesh));
	TestEqual(TEXT("with the source's name"), Converted.Name, Mesh->GetName());
	TestTrue(TEXT("within the limit"), Converted.Triangles > 0 && Converted.Triangles <= TriangleLimit);
	TestEqual(TEXT("as many triangles as the copy has"), Converted.Triangles, Count.Triangles);
	TestTrue(TEXT("in the slot order Export lists the materials in"), Converted.Slots == FMantlePlaceAssets::UploadSlots(Mesh));
	TestEqual(TEXT("one section a slot"), Converted.SectionNames.Num(), 2);
	TestEqual(TEXT("and no notice of its own"), Set.Issues.Num(), 0);

	// Reducing the same mesh again, from nothing, gives the same bytes: a republish finds what it uploaded before.
	FMantleMeshReduce::ClearCache();
	FMantlePlaceAssetConverter Again(MakeShared<FMantleGltfMaterialFlattener>(), CacheDir());
	Again.Limits = Limits;
	FMantlePlaceAssetSet AgainSet;
	if (TestTrue(TEXT("it converts again"), Again.ConvertMesh(Mesh, AgainSet)) && TestEqual(TEXT("to one mesh"), AgainSet.Meshes.Num(), 1))
	{
		TestEqual(TEXT("with the same hash"), AgainSet.Meshes[0].Hash, Converted.Hash);
	}
	TestTrue(TEXT("and the source is still unchanged"), FMeshSnapshot(*Mesh) == Before);

	// A mesh over on vertices alone is brought under that limit too.
	FMantleMeshReduce::ClearCache();
	FMantlePlaceAssetLimits FewVertices;
	FewVertices.MaxVertices = 700;
	UStaticMesh* Dense = Fixture.MakeMesh({ BigGrid }, { 2 });
	FMantleMeshLODCount DenseCount;
	FMantlePlaceAssets::CountMeshLOD(Dense, 0, DenseCount);
	TestTrue(TEXT("the grid has more than 700 vertices"), DenseCount.Vertices > FewVertices.MaxVertices);
	TArray<FMantleValidationIssue> VertexIssues;
	FMantlePlaceAssets::CheckMesh(Dense, FString(), FewVertices, VertexIssues);
	TestEqual(TEXT("no error for it"), CountSeverity(VertexIssues, EMantleIssueSeverity::Error), 0);
	TestTrue(TEXT("a warning that names the vertices"), FindIssue(VertexIssues, EMantleIssueSeverity::Warning, TEXT("vertices, more than the 700 a place mesh may have, so Mantle reduced a copy to ")) != nullptr);
	UStaticMesh* DenseReduced = FMantleMeshReduce::ReducedCopy(Dense, FewVertices, Why);
	if (TestNotNull(TEXT("its reduced copy: ") + Why, DenseReduced))
	{
		FMantleMeshLODCount DenseReducedCount;
		FMantlePlaceAssets::CountMeshLOD(DenseReduced, 0, DenseReducedCount);
		TestTrue(FString::Printf(TEXT("has at most 700 vertices (%d)"), DenseReducedCount.Vertices), DenseReducedCount.Vertices > 0 && DenseReducedCount.Vertices <= FewVertices.MaxVertices);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMeshReduceUnavailableTest,
	"Mantle.Editor.MeshReduce.NoReducerOrOneThatFallsShortKeepsTheError",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMeshReduceUnavailableTest::RunTest(const FString& Parameters)
{
	using namespace MantleMeshReduceTestPrivate;
	FReduceFixture Fixture;
	if (!TestTrue(TEXT("two engine materials load"), Fixture.IsReady()))
	{
		return false;
	}
	UStaticMesh* Mesh = Fixture.MakeMesh({ BigGrid }, { 2 });
	const FMantlePlaceAssetLimits Limits = SmallLimits();
	const FMeshSnapshot Before(*Mesh);

	// No reducer at all.
	FMantleMeshReduce::SetUnavailableForTests(true);
	TestFalse(TEXT("none is available"), FMantleMeshReduce::IsAvailable());
	FString Why;
	TestNull(TEXT("no copy"), FMantleMeshReduce::ReducedCopy(Mesh, Limits, Why));
	TestFalse(TEXT("and it says why"), Why.IsEmpty());
	TArray<FMantleValidationIssue> Issues;
	FMantlePlaceAssets::CheckMesh(Mesh, FString(), Limits, Issues);
	TestEqual(TEXT("Check keeps its one error"), Issues.Num(), 1);
	TestTrue(TEXT("about the triangles"), FindIssue(Issues, EMantleIssueSeverity::Error, TEXT("has 2,048 triangles; a place mesh may have at most 1,500.")) != nullptr);
	TestTrue(TEXT("and the cache tag a conversion would add is empty"), FMantleMeshReduce::CacheTag(Limits).IsEmpty());

	// A reducer that returns the mesh as it was: tried again, then given up on.
	FMantleMeshReduce::SetUnavailableForTests(false);
	FNoReduceReducer Fake;
	FMantleMeshReduce::SetReducerForTests(&Fake);
	TestTrue(TEXT("it is the reducer"), FMantleMeshReduce::IsAvailable());
	TestNull(TEXT("no copy under the limit"), FMantleMeshReduce::ReducedCopy(Mesh, Limits, Why));
	TestTrue(TEXT("and it says it fell short: ") + Why, Why.Contains(TEXT("could not bring it within 1500 triangles")));
	FString WhyAgain;
	TestNull(TEXT("and still none"), FMantleMeshReduce::ReducedCopy(Mesh, Limits, WhyAgain));
	TestEqual(TEXT("for the same reason, remembered"), WhyAgain, Why);
	Issues.Reset();
	FMantlePlaceAssets::CheckMesh(Mesh, FString(), Limits, Issues);
	TestEqual(TEXT("Check keeps its one error"), Issues.Num(), 1);
	TestTrue(TEXT("about the triangles"), FindIssue(Issues, EMantleIssueSeverity::Error, TEXT("has 2,048 triangles; a place mesh may have at most 1,500.")) != nullptr);

	TestTrue(TEXT("the source is unchanged"), FMeshSnapshot(*Mesh) == Before);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMeshReduceMirrorBakeTest,
	"Mantle.Editor.MeshReduce.TheMirrorBakeStillReadsEveryLOD",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMeshReduceMirrorBakeTest::RunTest(const FString& Parameters)
{
	using namespace MantleMeshReduceTestPrivate;
	FReduceFixture Fixture;
	if (!TestTrue(TEXT("two engine materials load"), Fixture.IsReady()))
	{
		return false;
	}
	UStaticMesh* Mesh = Fixture.MakeMesh({ BigGrid, SmallGrid }, { 2, 2 });

	// The reader the mirror bake and the reduction share: one polygon group a section, named by the slot.
	FMeshDescription Part;
	TestTrue(TEXT("LOD 1 reads"), FMantleMeshReduce::ReadRenderLOD(*Mesh, 1, TEXT("Probe_"), Part) == FMantleMeshReduce::EReadResult::Read);
	TestEqual(TEXT("with its triangles"), Part.Triangles().Num(), SmallTriangles);
	TArray<FName> Names;
	FStaticMeshConstAttributes PartAttributes(Part);
	const TPolygonGroupAttributesConstRef<FName> GroupNames = PartAttributes.GetPolygonGroupMaterialSlotNames();
	for (const FPolygonGroupID Group : Part.PolygonGroups().GetElementIDs())
	{
		Names.Add(GroupNames[Group]);
	}
	Names.Sort(FNameLexicalLess());
	TestTrue(TEXT("one group a slot, named by it"), Names.Num() == 2 && Names[0] == FName(TEXT("Probe_0")) && Names[1] == FName(TEXT("Probe_1")));
	FMeshDescription None;
	TestTrue(TEXT("a LOD that is not there does not read"), FMantleMeshReduce::ReadRenderLOD(*Mesh, 2, TEXT("Probe_"), None) == FMantleMeshReduce::EReadResult::Unreadable);

	// The mirror bake, on that reader: both LODs, the same triangles, the same slots, mirrored.
	FVector Positive;
	FString Why;
	UStaticMesh* Mirrored = FMantleMirrorBake::MirroredCopy(Mesh, FVector(-1.0, 1.0, 1.0), Positive, Why);
	if (!TestNotNull(TEXT("a mirrored copy: ") + Why, Mirrored))
	{
		return false;
	}
	const FMeshSnapshot Source(*Mesh);
	const FMeshSnapshot Copy(*Mirrored);
	TestTrue(TEXT("it has both LODs, with the same triangles"), Copy.TrianglesPerLOD == Source.TrianglesPerLOD);
	TestTrue(TEXT("the same slots in use"), FMantlePlaceAssets::UploadSlots(Mirrored) == FMantlePlaceAssets::UploadSlots(Mesh));
	TestTrue(TEXT("and the same material slots"), Copy.Materials == Source.Materials);
	const FBox SourceBox = Mesh->GetBounds().GetBox();
	const FBox MirroredBox = Mirrored->GetBounds().GetBox();
	TestTrue(FString::Printf(TEXT("the bounds are mirrored on X (%s from %s)"), *MirroredBox.ToString(), *SourceBox.ToString()),
		MirroredBox.Min.Equals(FVector(-SourceBox.Max.X, SourceBox.Min.Y, SourceBox.Min.Z), 0.1) && MirroredBox.Max.Equals(FVector(-SourceBox.Min.X, SourceBox.Max.Y, SourceBox.Max.Z), 0.1));
	if (TestEqual(TEXT("its material slots are named by index"), Mirrored->GetStaticMaterials().Num(), 2))
	{
		TestTrue(TEXT("slot 0"), Mirrored->GetStaticMaterials()[0].ImportedMaterialSlotName == FName(TEXT("MantleMirrorSlot_0")));
		TestTrue(TEXT("slot 1"), Mirrored->GetStaticMaterials()[1].ImportedMaterialSlotName == FName(TEXT("MantleMirrorSlot_1")));
	}
	return true;
}

#endif
