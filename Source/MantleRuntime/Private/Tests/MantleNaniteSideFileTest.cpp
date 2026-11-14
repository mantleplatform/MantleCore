// Copyright Joshua Eger. All Rights Reserved.
#include "Engine/StaticMesh.h"
#include "Misc/Compression.h"
#include "Misc/Paths.h"
#include "MantleAssetStore.h"
#include "MantleNaniteSideFile.h"
#include "MantlePlaceAssets.h"
#include "MantlePlaceParser.h"
#include "MantleRuntimeStaticMeshBuilder.h"
#include "MantleStaticMeshReader.h"
#include "Matrix3x4.h"
#include "RHI.h"
#include "Rendering/NaniteResources.h"
#include "Serialization/MemoryWriter.h"
#include "StaticMeshResources.h"
#include "UObject/StrongObjectPtr.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// The game reads a mesh's Nanite file (MND1, in its MNZ1 container) and puts it on the mesh it builds.
// The file is written here by hand, in the layout of D:\Mantle\Untracked\local\nanite\reports\step1-format.md, so the
// test does not depend on the plugin. No GPU: the meshes are built without render resources.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleNaniteSideFileTestPrivate
{
	const FString MeshTag = TEXT("sha256:") + FString::ChrN(64, TEXT('c'));

	/** A small Nanite::FResources with something in every field the file carries. */
	Nanite::FResources MakeResources()
	{
		Nanite::FResources Resources;
		Resources.ResourceFlags = 0x6;
		for (int32 Byte = 0; Byte < 37; ++Byte)
		{
			Resources.RootData.Add(static_cast<uint8>(Byte * 7 + 1));
		}
		Nanite::FPageStreamingState Page;
		FMemory::Memzero(Page);
		Page.BulkOffset = 11;
		Page.BulkSize = 22;
		Page.PageSize = 37;
		Page.DependenciesStart = 3;
		Page.DependenciesNum = 4;
		Page.MaxHierarchyDepth = 5;
		Page.Flags = 6;
		Resources.PageStreamingStates.Add(Page);
		Nanite::FPackedHierarchyNode Node;
		FMemory::Memzero(Node);
		Node.LODBounds[0] = FVector4f(1.0f, 2.0f, 3.0f, 4.0f);
		Node.Misc0[1].MinLODError_MaxParentLODError = 77;
		Node.Misc1[2].ChildStartReference = 88;
		Node.Misc2[NANITE_MAX_BVH_NODE_FANOUT - 1].NumBones_BoneIndicesOffset = 99;
		Resources.HierarchyNodes.Add(Node);
		Resources.HierarchyRootOffsets = { 0u, 5u };
		Resources.PageDependencies = { 1, 2, 3 };
		FMatrix3x4 Transform;
		FMemory::Memzero(Transform);
		Transform.M[0][0] = 1.0f;
		Transform.M[2][3] = 9.0f;
		Resources.AssemblyTransforms.Add(Transform);
		Resources.AssemblyBoneAttachmentData = { 4u };
		Resources.BoneIndices = { 8u, 9u };
		Nanite::FPageRangeKey Key;
		Key.Value = 1234;
		Resources.PageRangeLookup.Add(Key);
		Resources.MeshBounds = FBoxSphereBounds3f(FVector3f(1.0f, 2.0f, 3.0f), FVector3f(4.0f, 5.0f, 6.0f), 7.0f);
		Resources.NumRootPages = 1;
		Resources.PositionPrecision = -3;
		Resources.NormalPrecision = 8;
		Resources.TangentPrecision = 7;
		Resources.NumInputTriangles = 123456;
		Resources.NumInputVertices = 654321;
		Resources.NumClusters = 42;
		Resources.VoxelMaterialsMask = 0x1122334455667788ull;
		Resources.NumInputCurves = 2;
		return Resources;
	}

	/** The file as the plugin writes it (step1-format.md fields 1 to 25) for MakeResources(), for MeshTagIn's mesh and the slot table. */
	TArray<uint8> WriteFile(const FString& MeshTagIn, const TArray<int32>& Slots)
	{
		Nanite::FResources Resources = MakeResources();
		TArray<uint8> Bytes;
		FMemoryWriter Ar(Bytes, /*bIsPersistent*/ true);
		uint8 Magic[4] = { 'M', 'N', 'D', '1' };
		Ar.Serialize(Magic, 4);
		uint32 Version = 1;
		FString Engine = FMantleNaniteSideFile::EngineTag();
		FString Mesh = MeshTagIn;
		Ar << Version << Engine << Mesh;
		TArray<int32> Table = Slots;
		Ar << Table;
		Ar << Resources.ResourceFlags;
		int32 RootBytes = Resources.RootData.Num();
		Ar << RootBytes;
		Ar.Serialize(Resources.RootData.GetData(), RootBytes);
		int32 PageCount = Resources.PageStreamingStates.Num();
		Ar << PageCount;
		for (Nanite::FPageStreamingState& Page : Resources.PageStreamingStates)
		{
			Ar << Page.BulkOffset << Page.BulkSize << Page.PageSize << Page.DependenciesStart << Page.DependenciesNum << Page.MaxHierarchyDepth << Page.Flags;
		}
		int32 NodeCount = Resources.HierarchyNodes.Num();
		Ar << NodeCount;
		for (Nanite::FPackedHierarchyNode& Node : Resources.HierarchyNodes)
		{
			for (uint32 Index = 0; Index < NANITE_MAX_BVH_NODE_FANOUT; ++Index)
			{
				Ar << Node.LODBounds[Index] << Node.Misc0[Index].BoxBoundsCenter << Node.Misc0[Index].MinLODError_MaxParentLODError
					<< Node.Misc1[Index].BoxBoundsExtent << Node.Misc1[Index].ChildStartReference << Node.Misc2[Index].ResourcePageRangeKey
					<< Node.Misc2[Index].GroupPartSize_AssemblyPartIndex << Node.Misc2[Index].NumBones_BoneIndicesOffset;
			}
		}
		Ar << Resources.HierarchyRootOffsets << Resources.PageDependencies;
		int32 TransformCount = Resources.AssemblyTransforms.Num();
		Ar << TransformCount;
		for (FMatrix3x4& Transform : Resources.AssemblyTransforms)
		{
			Ar << Transform;
		}
		Ar << Resources.AssemblyBoneAttachmentData << Resources.BoneIndices;
		int32 KeyCount = Resources.PageRangeLookup.Num();
		Ar << KeyCount;
		for (Nanite::FPageRangeKey& Key : Resources.PageRangeLookup)
		{
			Ar << Key.Value;
		}
		Ar << Resources.MeshBounds << Resources.NumRootPages << Resources.PositionPrecision << Resources.NormalPrecision << Resources.NumInputTriangles
			<< Resources.NumInputVertices << Resources.NumClusters << Resources.VoxelMaterialsMask << Resources.NumInputCurves << Resources.TangentPrecision;
		return Bytes;
	}

	/** An MNZ1 container the way the plugin writes it: the magic, RawBytes as the inflated size, one zlib stream of Mnd1. */
	TArray<uint8> MakeContainer(const TArray<uint8>& Mnd1, const uint32 RawBytes)
	{
		TArray<uint8> Packed;
		Packed.SetNumUninitialized(FCompression::CompressMemoryBound(NAME_Zlib, Mnd1.Num()));
		int32 PackedSize = Packed.Num();
		verify(FCompression::CompressMemory(NAME_Zlib, Packed.GetData(), PackedSize, Mnd1.GetData(), Mnd1.Num(), COMPRESS_BiasMemory));
		TArray<uint8> Container;
		Container.Append(reinterpret_cast<const uint8*>("MNZ1"), 4);
		Container.Append(reinterpret_cast<const uint8*>(&RawBytes), sizeof(RawBytes));
		Container.Append(Packed.GetData(), PackedSize);
		return Container;
	}

	/** A box (8 vertices, 12 triangles) as an MRM2 mesh whose two sections are the slots "A" (6 triangles) and "B" (6). */
	FMantleStaticMeshData MakeTwoSlotBox()
	{
		FMantleStaticMeshData Mesh;
		for (int32 Corner = 0; Corner < 8; ++Corner)
		{
			const float X = (Corner & 1) ? 50.0f : -50.0f;
			const float Y = (Corner & 2) ? 50.0f : -50.0f;
			const float Z = (Corner & 4) ? 50.0f : -50.0f;
			Mesh.Positions.Add(FVector3f(X, Y, Z));
			Mesh.Normals.Add(FVector3f(0.0f, 0.0f, 1.0f));
			Mesh.Tangents.Add(FVector4f(1.0f, 0.0f, 0.0f, 1.0f));
			Mesh.UVs.Add(FVector2f((X + 50.0f) / 100.0f, (Y + 50.0f) / 100.0f));
		}
		const uint32 Triangles[12][3] = {
			{ 0, 2, 1 }, { 1, 2, 3 }, { 4, 5, 6 }, { 5, 7, 6 },
			{ 0, 1, 4 }, { 1, 5, 4 }, { 2, 6, 3 }, { 3, 6, 7 },
			{ 0, 4, 2 }, { 2, 4, 6 }, { 1, 3, 5 }, { 3, 7, 5 } };
		for (const uint32 (&Triangle)[3] : Triangles)
		{
			Mesh.Indices.Add(Triangle[0]);
			Mesh.Indices.Add(Triangle[1]);
			Mesh.Indices.Add(Triangle[2]);
		}
		Mesh.Bounds = FBox3f(FVector3f(-50.0f), FVector3f(50.0f));
		FMantleSkinnedMeshSection A;
		A.MaterialIndex = 0;
		A.FirstIndex = 0;
		A.IndexCount = 18;
		A.MaterialName = TEXT("A");
		FMantleSkinnedMeshSection B;
		B.MaterialIndex = 1;
		B.FirstIndex = 18;
		B.IndexCount = 18;
		B.MaterialName = TEXT("B");
		Mesh.Sections = { A, B };
		return Mesh;
	}

	const TCHAR* const FlowMeshA = TEXT("ast_u0000000000000000000000000000000a");
	const TCHAR* const FlowMeshB = TEXT("ast_u0000000000000000000000000000000b");
	const TCHAR* const FlowMeshC = TEXT("ast_u0000000000000000000000000000000c");
	const TCHAR* const FlowNaniteA = TEXT("ast_u000000000000000000000000000000a1");
	const TCHAR* const FlowNaniteB = TEXT("ast_u000000000000000000000000000000b1");

	/** Answers every id as unknown and writes down what was asked: what the loader asks for is what it will download. */
	class FRecordingResolver : public IMantleAssetResolver
	{
	public:
		TArray<FString> Asked;

		virtual void Resolve(const TArray<FString>& Ids, TFunction<void(const FMantleAssetResolveResult&)> OnDone) override
		{
			Asked.Append(Ids);
			FMantleAssetResolveResult Result;
			Result.bSuccess = true;
			Result.Missing = Ids;
			OnDone(Result);
		}
	};

	/** A transport that never connects: nothing in these tests downloads. */
	class FNoFetcher : public IMantleAssetFetcher
	{
	public:
		virtual void Fetch(const FString& Url, const TSharedRef<FArchive>& Sink, TFunction<void(const FMantleAssetFetchResult&)> OnDone) override
		{
			OnDone(FMantleAssetFetchResult());
		}
	};

	/** Three uploaded meshes (A, B, C) all placed; A names a Nanite file baked for EngineA, B one baked for EngineB, C none. */
	FMantlePlace MakeFlowPlace(const FString& EngineA, const FString& EngineB)
	{
		const FString Hash = FString::ChrN(64, TEXT('a'));
		const FString Json = FString::Printf(
			TEXT("{\"schemaVersion\":1,\"placeId\":\"plc_nanitefl01\",\"revision\":{\"number\":1,\"state\":\"draft\"},"
				 "\"metadata\":{\"name\":\"Flow\",\"authorId\":\"usr_4b1d\"},\"settings\":{},\"assets\":["
				 "{\"id\":\"%s\",\"kind\":\"StaticMesh\",\"hash\":\"sha256:%s\",\"bytes\":100,\"nanite\":\"%s\"},"
				 "{\"id\":\"%s\",\"kind\":\"StaticMesh\",\"hash\":\"sha256:%s\",\"bytes\":100,\"nanite\":\"%s\"},"
				 "{\"id\":\"%s\",\"kind\":\"StaticMesh\",\"hash\":\"sha256:%s\",\"bytes\":100},"
				 "{\"id\":\"%s\",\"kind\":\"Nanite\",\"hash\":\"sha256:%s\",\"bytes\":50,\"engine\":\"%s\"},"
				 "{\"id\":\"%s\",\"kind\":\"Nanite\",\"hash\":\"sha256:%s\",\"bytes\":50,\"engine\":\"%s\"}],"
				 "\"entities\":["
				 "{\"id\":\"ent_a\",\"type\":\"StaticMesh\",\"parent\":null,\"transform\":{\"location\":[0,0,0],\"rotation\":[0,0,0],\"scale\":[1,1,1]},\"properties\":{\"mesh\":\"%s\"}},"
				 "{\"id\":\"ent_b\",\"type\":\"StaticMesh\",\"parent\":null,\"transform\":{\"location\":[0,0,0],\"rotation\":[0,0,0],\"scale\":[1,1,1]},\"properties\":{\"mesh\":\"%s\"}},"
				 "{\"id\":\"ent_c\",\"type\":\"StaticMesh\",\"parent\":null,\"transform\":{\"location\":[0,0,0],\"rotation\":[0,0,0],\"scale\":[1,1,1]},\"properties\":{\"mesh\":\"%s\"}}]}"),
			FlowMeshA, *Hash, FlowNaniteA, FlowMeshB, *Hash, FlowNaniteB, FlowMeshC, *Hash,
			FlowNaniteA, *Hash, *EngineA, FlowNaniteB, *Hash, *EngineB, FlowMeshA, FlowMeshB, FlowMeshC);
		const FMantleParseResult Parsed = FMantlePlaceParser::ParseFromString(Json);
		check(Parsed.bSuccess);
		return Parsed.Place;
	}

	/** Loads the flow place with the given Nanite switch and returns every id the loader asked the API about. */
	TArray<FString> IdsAskedFor(const bool bLoadNanite, const FString& EngineA, const FString& EngineB)
	{
		FMantleAssetStoreSettings StoreSettings;
		StoreSettings.RootDir = FPaths::AutomationTransientDir() / TEXT("NaniteFlow");
		const TSharedRef<FRecordingResolver> Resolver = MakeShared<FRecordingResolver>();
		FMantlePlaceAssetSettings Settings;
		Settings.bAutoPump = false;
		Settings.bRenderAssets = true;
		Settings.bLoadNanite = bLoadNanite;
		Settings.Resolver = Resolver;
		Settings.Store = FMantleAssetStore::Create(StoreSettings, MakeShared<FNoFetcher>());
		const TSharedRef<FMantlePlaceAssets> Assets = FMantlePlaceAssets::Prepare(MakeFlowPlace(EngineA, EngineB), Settings, FMantlePlaceAssetsDone());
		Assets->WaitUntilComplete(20.0f);
		return Resolver->Asked;
	}

	/** One asset the fake API knows and the fake transport serves. */
	struct FServedAsset
	{
		FString Kind;
		TArray<uint8> Bytes;
		bool bServe = true;
	};

	/** Resolves the served assets as approved, with the cooked hash of their bytes; any other id is unknown. */
	class FServingResolver : public IMantleAssetResolver
	{
	public:
		TMap<FString, FServedAsset> Assets;

		virtual void Resolve(const TArray<FString>& Ids, TFunction<void(const FMantleAssetResolveResult&)> OnDone) override
		{
			FMantleAssetResolveResult Result;
			Result.bSuccess = true;
			for (const FString& Id : Ids)
			{
				const FServedAsset* Served = Assets.Find(Id);
				if (!Served)
				{
					Result.Missing.Add(Id);
					continue;
				}
				FMantleUploadedAsset Asset;
				Asset.Id = Id;
				Asset.Hash = MeshTag;
				Asset.Kind = Served->Kind;
				Asset.Status = TEXT("approved");
				Asset.CookState = TEXT("passed");
				Asset.ModerationState = TEXT("passed");
				Asset.Url = TEXT("test://") + Id;
				Asset.CookedHash = FMantleContentHash::OfBytes(Served->Bytes).ToString();
				Asset.CookedBytes = Served->Bytes.Num();
				Result.Assets.Add(Asset);
			}
			OnDone(Result);
		}
	};

	/** Serves the same assets by the url the resolver gave; one marked not to serve answers 404. */
	class FServingFetcher : public IMantleAssetFetcher
	{
	public:
		TSharedPtr<FServingResolver> Source;

		virtual void Fetch(const FString& Url, const TSharedRef<FArchive>& Sink, TFunction<void(const FMantleAssetFetchResult&)> OnDone) override
		{
			FMantleAssetFetchResult Result;
			Result.bConnected = true;
			Result.StatusCode = 404;
			const FServedAsset* Served = Source->Assets.Find(Url.RightChop(7));
			if (Served && Served->bServe)
			{
				Sink->Serialize(const_cast<uint8*>(Served->Bytes.GetData()), Served->Bytes.Num());
				Result.StatusCode = 200;
			}
			OnDone(Result);
		}
	};

	/** A box as an MRM1 mesh: the header, 8 positions and 12 triangles, little-endian as the cook writes it. */
	TArray<uint8> MakeMrm1Box()
	{
		TArray<uint8> Bytes;
		FMemoryWriter Ar(Bytes, true);
		uint8 Magic[4] = { 'M', 'R', 'M', '1' };
		Ar.Serialize(Magic, 4);
		uint32 Version = 1, Flags = 0, Vertices = 8, Indices = 36;
		Ar << Version << Flags << Vertices << Indices;
		float Bounds[6] = { -50.f, -50.f, -50.f, 50.f, 50.f, 50.f };
		for (float& Value : Bounds)
		{
			Ar << Value;
		}
		for (int32 Corner = 0; Corner < 8; ++Corner)
		{
			float X = (Corner & 1) ? 50.f : -50.f, Y = (Corner & 2) ? 50.f : -50.f, Z = (Corner & 4) ? 50.f : -50.f;
			Ar << X << Y << Z;
		}
		const uint32 Triangles[12][3] = {
			{ 0, 2, 1 }, { 1, 2, 3 }, { 4, 5, 6 }, { 5, 7, 6 }, { 0, 1, 4 }, { 1, 5, 4 },
			{ 2, 6, 3 }, { 3, 6, 7 }, { 0, 4, 2 }, { 2, 4, 6 }, { 1, 3, 5 }, { 3, 7, 5 } };
		for (const uint32 (&Triangle)[3] : Triangles)
		{
			for (uint32 Index : Triangle)
			{
				Ar << Index;
			}
		}
		return Bytes;
	}

	/** One uploaded mesh placed once, naming one Nanite file (the manifest's hash of the mesh is MeshTag's). */
	FMantlePlace MakeOneMeshPlace()
	{
		const FString Json = FString::Printf(
			TEXT("{\"schemaVersion\":1,\"placeId\":\"plc_nanitere01\",\"revision\":{\"number\":1,\"state\":\"draft\"},"
				 "\"metadata\":{\"name\":\"Release\",\"authorId\":\"usr_4b1d\"},\"settings\":{},\"assets\":["
				 "{\"id\":\"%s\",\"kind\":\"StaticMesh\",\"hash\":\"%s\",\"bytes\":100,\"nanite\":\"%s\"},"
				 "{\"id\":\"%s\",\"kind\":\"Nanite\",\"hash\":\"%s\",\"bytes\":50,\"engine\":\"%s\"}],"
				 "\"entities\":[{\"id\":\"ent_a\",\"type\":\"StaticMesh\",\"parent\":null,\"transform\":{\"location\":[0,0,0],\"rotation\":[0,0,0],\"scale\":[1,1,1]},\"properties\":{\"mesh\":\"%s\"}}]}"),
			FlowMeshA, *MeshTag, FlowNaniteA, FlowNaniteA, *MeshTag, *FMantleNaniteSideFile::EngineTag(), FlowMeshA);
		const FMantleParseResult Parsed = FMantlePlaceParser::ParseFromString(Json);
		check(Parsed.bSuccess);
		return Parsed.Place;
	}

	/** The fake API and transport for the one-mesh place: a good Nanite container, and a mesh that is good or whose download is refused. */
	TSharedRef<FServingResolver> MakeServer(const bool bMeshDownloads)
	{
		const TSharedRef<FServingResolver> Server = MakeShared<FServingResolver>();
		FServedAsset Mesh;
		Mesh.Kind = TEXT("mesh");
		Mesh.Bytes = MakeMrm1Box();
		Mesh.bServe = bMeshDownloads;
		Server->Assets.Add(FlowMeshA, Mesh);
		FServedAsset Nanite;
		Nanite.Kind = TEXT("nanite");
		Nanite.Bytes = MakeContainer(WriteFile(MeshTag, { 0, 1 }), WriteFile(MeshTag, { 0, 1 }).Num());
		Server->Assets.Add(FlowNaniteA, Nanite);
		return Server;
	}

	TSharedRef<FMantlePlaceAssets> LoadOneMesh(const TSharedRef<FServingResolver>& Server, const FString& StoreName)
	{
		FMantleAssetStoreSettings StoreSettings;
		StoreSettings.RootDir = FPaths::AutomationTransientDir() / StoreName;
		StoreSettings.MaxAttempts = 1;
		const TSharedRef<FServingFetcher> Fetcher = MakeShared<FServingFetcher>();
		Fetcher->Source = Server;
		FMantlePlaceAssetSettings Settings;
		Settings.bAutoPump = false;
		Settings.bRenderAssets = true;
		Settings.bLoadNanite = true;
		Settings.bAsyncCollisionCook = false;
		Settings.Resolver = Server;
		Settings.Store = FMantleAssetStore::Create(StoreSettings, Fetcher);
		const TSharedRef<FMantlePlaceAssets> Assets = FMantlePlaceAssets::Prepare(MakeOneMeshPlace(), Settings, FMantlePlaceAssetsDone());
		Assets->WaitUntilComplete(60.0f);
		return Assets;
	}

	UStaticMesh* BuildBox(const TArray<uint8>* File, const FString& Tag)
	{
		FMantleStaticMeshBuildParams Params;
		Params.bInitRenderResources = false;
		Params.NaniteFile = File;
		Params.NaniteMeshTag = Tag;
		return FMantleRuntimeStaticMeshBuilder::Build(MakeTwoSlotBox(), Params);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleNaniteSideFileReadTest,
	"Mantle.Runtime.StaticMesh.NaniteFile",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleNaniteSideFileReadTest::RunTest(const FString& Parameters)
{
	using namespace MantleNaniteSideFileTestPrivate;

	const Nanite::FResources Written = MakeResources();
	const TArray<uint8> File = WriteFile(MeshTag, { 0, 1 });

	Nanite::FResources Read;
	TArray<int32> Slots;
	FString Reason;
	if (!TestTrue(TEXT("its own mesh reads it"), FMantleNaniteSideFile::Read(File, MeshTag, Read, Slots, &Reason)))
	{
		AddInfo(Reason);
		return false;
	}
	TestEqual(TEXT("slot table"), Slots, TArray<int32>({ 0, 1 }));
	TestEqual(TEXT("flags"), Read.ResourceFlags, Written.ResourceFlags);
	TestEqual(TEXT("root data"), Read.RootData, Written.RootData);
	if (TestEqual(TEXT("pages"), Read.PageStreamingStates.Num(), 1))
	{
		const Nanite::FPageStreamingState& Page = Read.PageStreamingStates[0];
		TestEqual(TEXT("page offset"), Page.BulkOffset, 11u);
		TestEqual(TEXT("page size"), Page.PageSize, 37u);
		TestEqual(TEXT("page dependencies num"), static_cast<int32>(Page.DependenciesNum), 4);
		TestEqual(TEXT("page depth"), static_cast<int32>(Page.MaxHierarchyDepth), 5);
		TestEqual(TEXT("page flags"), static_cast<int32>(Page.Flags), 6);
	}
	if (TestEqual(TEXT("nodes"), Read.HierarchyNodes.Num(), 1))
	{
		const Nanite::FPackedHierarchyNode& Node = Read.HierarchyNodes[0];
		TestEqual(TEXT("node bounds"), Node.LODBounds[0], FVector4f(1.0f, 2.0f, 3.0f, 4.0f));
		TestEqual(TEXT("node min error"), Node.Misc0[1].MinLODError_MaxParentLODError, 77u);
		TestEqual(TEXT("node child start"), Node.Misc1[2].ChildStartReference, 88u);
		TestEqual(TEXT("node last bone offset"), Node.Misc2[NANITE_MAX_BVH_NODE_FANOUT - 1].NumBones_BoneIndicesOffset, 99u);
	}
	TestEqual(TEXT("root offsets"), Read.HierarchyRootOffsets, Written.HierarchyRootOffsets);
	TestEqual(TEXT("page dependencies"), Read.PageDependencies, Written.PageDependencies);
	if (TestEqual(TEXT("assembly transforms"), Read.AssemblyTransforms.Num(), 1))
	{
		TestEqual(TEXT("transform [0][0]"), Read.AssemblyTransforms[0].M[0][0], 1.0f);
		TestEqual(TEXT("transform [2][3]"), Read.AssemblyTransforms[0].M[2][3], 9.0f);
	}
	TestEqual(TEXT("bone attachment data"), Read.AssemblyBoneAttachmentData, Written.AssemblyBoneAttachmentData);
	TestEqual(TEXT("bone indices"), Read.BoneIndices, Written.BoneIndices);
	if (TestEqual(TEXT("page ranges"), Read.PageRangeLookup.Num(), 1))
	{
		TestEqual(TEXT("page range key"), Read.PageRangeLookup[0].Value, 1234u);
	}
	TestEqual(TEXT("bounds origin"), Read.MeshBounds.Origin, Written.MeshBounds.Origin);
	TestEqual(TEXT("bounds extent"), Read.MeshBounds.BoxExtent, Written.MeshBounds.BoxExtent);
	TestEqual(TEXT("bounds radius"), Read.MeshBounds.SphereRadius, Written.MeshBounds.SphereRadius);
	TestEqual(TEXT("root pages"), Read.NumRootPages, 1u);
	TestEqual(TEXT("position precision"), Read.PositionPrecision, -3);
	TestEqual(TEXT("normal precision"), Read.NormalPrecision, 8);
	TestEqual(TEXT("tangent precision"), Read.TangentPrecision, 7);
	TestEqual(TEXT("input triangles"), Read.NumInputTriangles, 123456u);
	TestEqual(TEXT("input vertices"), Read.NumInputVertices, 654321u);
	TestEqual(TEXT("clusters"), Read.NumClusters, 42u);
	TestEqual(TEXT("voxel mask"), Read.VoxelMaterialsMask, 0x1122334455667788ull);
	TestEqual(TEXT("curves"), Read.NumInputCurves, 2u);
	TestEqual(TEXT("runtime state untouched"), Read.RuntimeResourceID, MAX_uint32);
	TestFalse(TEXT("no streamed pages"), Read.HasStreamingData());

	const FString Other = TEXT("sha256:") + FString::ChrN(64, TEXT('d'));
	TestFalse(TEXT("another mesh's file is not present"), FMantleNaniteSideFile::Read(File, Other, Read, Slots, &Reason));
	TArray<uint8> Cut = File;
	Cut.SetNum(Cut.Num() - 3);
	TestFalse(TEXT("a cut file is not present"), FMantleNaniteSideFile::Read(Cut, MeshTag, Read, Slots, &Reason));
	TArray<uint8> Long = File;
	Long.Add(0);
	TestFalse(TEXT("a file with bytes after its end is not present"), FMantleNaniteSideFile::Read(Long, MeshTag, Read, Slots, &Reason));
	TArray<uint8> Engine = File;
	Engine[12] ^= 0x01; // inside the engine tag's characters (after magic, version and the string's length)
	TestFalse(TEXT("another engine's file is not present"), FMantleNaniteSideFile::Read(Engine, MeshTag, Read, Slots, &Reason));
	TestFalse(TEXT("an empty file is not present"), FMantleNaniteSideFile::Read(TArray<uint8>(), MeshTag, Read, Slots, &Reason));
	TArray<uint8> Magic = File;
	Magic[3] = '2';
	TestFalse(TEXT("another format is not present"), FMantleNaniteSideFile::Read(Magic, MeshTag, Read, Slots, &Reason));

	TestTrue(TEXT("0, 1, 2 for three slots matches"), FMantleNaniteSideFile::SlotsMatchPublished({ 0, 1, 2 }, 3));
	TestFalse(TEXT("another order does not"), FMantleNaniteSideFile::SlotsMatchPublished({ 0, 2, 1 }, 3));
	TestFalse(TEXT("an undrawn slot does not"), FMantleNaniteSideFile::SlotsMatchPublished({ 0, -1 }, 2));
	TestFalse(TEXT("more asset slots than published does not"), FMantleNaniteSideFile::SlotsMatchPublished({ 0, 1, -1 }, 2));
	TestFalse(TEXT("fewer asset slots than published does not"), FMantleNaniteSideFile::SlotsMatchPublished({ 0 }, 2));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleNaniteSideFileBuildTest,
	"Mantle.Runtime.StaticMesh.NaniteAttach",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleNaniteSideFileBuildTest::RunTest(const FString& Parameters)
{
	using namespace MantleNaniteSideFileTestPrivate;

	// A model with a file reports valid Nanite data, and keeps the LODs it was built with.
	const TArray<uint8> File = WriteFile(MeshTag, { 0, 1 });
	TStrongObjectPtr<UStaticMesh> With(BuildBox(&File, MeshTag));
	if (TestNotNull(TEXT("the mesh with a file builds"), With.Get()))
	{
		TestTrue(TEXT("it has valid Nanite data"), With->HasValidNaniteData());
		const FStaticMeshRenderData* RenderData = With->GetRenderData();
		if (TestNotNull(TEXT("render data"), RenderData) && TestTrue(TEXT("Nanite resources"), RenderData->NaniteResourcesPtr.IsValid()))
		{
			TestEqual(TEXT("its root pages"), RenderData->NaniteResourcesPtr->NumRootPages, 1u);
			TestEqual(TEXT("its source triangles"), RenderData->NaniteResourcesPtr->NumInputTriangles, 123456u);
			TestEqual(TEXT("its root data"), RenderData->NaniteResourcesPtr->RootData.Num(), 37);
			TestEqual(TEXT("LOD0 stays the published triangles"), RenderData->LODResources[0].GetNumTriangles(), 12);
			TestTrue(TEXT("it reports a Nanite fallback"), RenderData->bHasNaniteFallbackMesh);
			// The 7:45 pm crash: Nanite's ray tracing path needs a usable fallback LOD, and GetFirstValidLODIdx skips an LOD whose BuffersSize is 0.
			TestEqual(TEXT("LOD0 is a usable fallback LOD (not -1)"), RenderData->GetFirstValidLODIdx(0), 0);
		}
		TestEqual(TEXT("two material slots"), With->GetStaticMaterials().Num(), 2);
	}

	// A model with no file builds as today.
	TStrongObjectPtr<UStaticMesh> Without(BuildBox(nullptr, FString()));
	if (TestNotNull(TEXT("the mesh without a file builds"), Without.Get()))
	{
		TestFalse(TEXT("it has no Nanite data"), Without->HasValidNaniteData());
		TestEqual(TEXT("LOD0"), Without->GetRenderData()->LODResources[0].GetNumTriangles(), 12);
	}

	// A file that is not this mesh's, or not in its slot order, is skipped and the mesh still builds.
	const FString Other = TEXT("sha256:") + FString::ChrN(64, TEXT('e'));
	TStrongObjectPtr<UStaticMesh> OtherMesh(BuildBox(&File, Other));
	if (TestNotNull(TEXT("a mesh given another mesh's file builds"), OtherMesh.Get()))
	{
		TestFalse(TEXT("without Nanite data"), OtherMesh->HasValidNaniteData());
	}
	const TArray<uint8> Swapped = WriteFile(MeshTag, { 1, 0 });
	TStrongObjectPtr<UStaticMesh> SwappedMesh(BuildBox(&Swapped, MeshTag));
	if (TestNotNull(TEXT("a mesh given a file in another slot order builds"), SwappedMesh.Get()))
	{
		TestFalse(TEXT("without Nanite data"), SwappedMesh->HasValidNaniteData());
	}
	const TArray<uint8> ThreeSlots = WriteFile(MeshTag, { 0, 1, 2 });
	TStrongObjectPtr<UStaticMesh> ThreeMesh(BuildBox(&ThreeSlots, MeshTag));
	if (TestNotNull(TEXT("a mesh given a file with another slot count builds"), ThreeMesh.Get()))
	{
		TestFalse(TEXT("without Nanite data"), ThreeMesh->HasValidNaniteData());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleNaniteContainerTest,
	"Mantle.Runtime.StaticMesh.NaniteContainer",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleNaniteContainerTest::RunTest(const FString& Parameters)
{
	using namespace MantleNaniteSideFileTestPrivate;

	// The engine's inflate logs an error for every stream it refuses (damaged on purpose below); the refusal is what is tested.
	AddExpectedError(TEXT("Failed to uncompress memory"), EAutomationExpectedErrorFlags::Contains, 0);

	const TArray<uint8> Mnd1 = WriteFile(MeshTag, { 0, 1 });
	const TArray<uint8> Container = MakeContainer(Mnd1, Mnd1.Num());

	// The uploaded container unpacks to exactly the MND1, which the reader then takes.
	TArray<uint8> Out;
	FString Reason;
	if (!TestTrue(TEXT("a good container unpacks"), FMantleNaniteSideFile::Unpack(Container, Out, &Reason)))
	{
		AddInfo(Reason);
		return false;
	}
	TestEqual(TEXT("the unpacked bytes are the MND1"), Out, Mnd1);
	Nanite::FResources Read;
	TArray<int32> Slots;
	TestTrue(TEXT("and the reader takes them"), FMantleNaniteSideFile::Read(Out, MeshTag, Read, Slots, &Reason));
	TestEqual(TEXT("with its root data"), Read.RootData.Num(), 37);

	// The size word is held before anything is allocated.
	TestFalse(TEXT("a size of 0 is refused"), FMantleNaniteSideFile::Unpack(MakeContainer(Mnd1, 0), Out, &Reason));
	TestTrue(TEXT("and leaves nothing allocated"), Out.IsEmpty());
	TestFalse(TEXT("a size past 512 MiB is refused"), FMantleNaniteSideFile::Unpack(MakeContainer(Mnd1, FMantleNaniteSideFile::MaxInflatedBytes + 1), Out, &Reason));
	TestTrue(TEXT("the refusal says why"), Reason.Contains(TEXT("inflates to")));
	TestFalse(TEXT("a size of 4 GiB less one is refused"), FMantleNaniteSideFile::Unpack(MakeContainer(Mnd1, MAX_uint32), Out, &Reason));

	// A size that is not the stream's, either way, is damage.
	TestFalse(TEXT("a size larger than the stream inflates to is refused"), FMantleNaniteSideFile::Unpack(MakeContainer(Mnd1, Mnd1.Num() + 1), Out, &Reason));
	TestFalse(TEXT("a size smaller than the stream inflates to is refused"), FMantleNaniteSideFile::Unpack(MakeContainer(Mnd1, Mnd1.Num() - 1), Out, &Reason));

	// Other files are not containers.
	TestFalse(TEXT("a raw MND1 is refused"), FMantleNaniteSideFile::Unpack(Mnd1, Out, &Reason));
	TestFalse(TEXT("an empty file is refused"), FMantleNaniteSideFile::Unpack(TArray<uint8>(), Out, &Reason));
	TestFalse(TEXT("a header alone is refused"), FMantleNaniteSideFile::Unpack(TConstArrayView<uint8>(Container.GetData(), FMantleNaniteSideFile::ContainerHeaderBytes), Out, &Reason));
	TArray<uint8> WrongMagic = Container;
	WrongMagic[3] = '2';
	TestFalse(TEXT("another magic is refused"), FMantleNaniteSideFile::Unpack(WrongMagic, Out, &Reason));
	TArray<uint8> Cut = Container;
	Cut.SetNum(Cut.Num() - 6);
	TestFalse(TEXT("a cut stream is refused"), FMantleNaniteSideFile::Unpack(Cut, Out, &Reason));
	TArray<uint8> Damaged = Container;
	Damaged[Damaged.Num() / 2] ^= 0x5A;
	TestFalse(TEXT("a damaged stream is refused"), FMantleNaniteSideFile::Unpack(Damaged, Out, &Reason));
	TArray<uint8> NotZlib = Container;
	for (int32 Index = FMantleNaniteSideFile::ContainerHeaderBytes; Index < NotZlib.Num(); ++Index)
	{
		NotZlib[Index] = static_cast<uint8>(Index * 31);
	}
	TestFalse(TEXT("bytes that are not a zlib stream are refused"), FMantleNaniteSideFile::Unpack(NotZlib, Out, &Reason));
	TestTrue(TEXT("a refusal leaves no bytes behind"), Out.IsEmpty());

	// The fetch decision: this machine runs Nanite, and the file was baked for exactly this engine build.
	const FString Engine = FMantleNaniteSideFile::EngineTag();
	TestTrue(TEXT("same build on a Nanite machine: fetch"), FMantleNaniteSideFile::ShouldFetch(true, Engine, Engine));
	TestFalse(TEXT("a machine that cannot run Nanite never fetches"), FMantleNaniteSideFile::ShouldFetch(false, Engine, Engine));
	TestFalse(TEXT("another engine build is never fetched"), FMantleNaniteSideFile::ShouldFetch(true, Engine + TEXT("x"), Engine));
	TestFalse(TEXT("an entry with no engine is never fetched"), FMantleNaniteSideFile::ShouldFetch(true, FString(), Engine));
	if (GUsingNullRHI)
	{
		TestFalse(TEXT("a null RHI (an unattended test run) cannot run Nanite, so it never downloads a file"), FMantleNaniteSideFile::CanRunNanite());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleNaniteFetchDecisionTest,
	"Mantle.Runtime.StaticMesh.NaniteFetchDecision",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleNaniteFetchDecisionTest::RunTest(const FString& Parameters)
{
	using namespace MantleNaniteSideFileTestPrivate;

	// The fake API knows none of the ids, so the loader logs each mesh as failed; what it asked for is what is checked.
	AddExpectedError(TEXT("MANTLE_PLACE_ASSETS FAIL mesh"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedError(TEXT("MANTLE_PLACE_ASSETS READY with"), EAutomationExpectedErrorFlags::Contains, 0);

	// The loader decides from the manifest which Nanite files to ask the API about (and so download), before anything is downloaded.
	const FString Engine = FMantleNaniteSideFile::EngineTag();
	const FString Other = Engine + TEXT("-other");

	const TArray<FString> On = IdsAskedFor(true, Engine, Other);
	TestTrue(TEXT("on a Nanite machine every mesh is asked for"), On.Contains(FlowMeshA) && On.Contains(FlowMeshB) && On.Contains(FlowMeshC));
	TestTrue(TEXT("on a Nanite machine a file baked for this engine is asked for"), On.Contains(FlowNaniteA));
	TestFalse(TEXT("a file baked for another engine build is never asked for, so never downloaded"), On.Contains(FlowNaniteB));

	const TArray<FString> Off = IdsAskedFor(false, Engine, Other);
	TestTrue(TEXT("on a machine that cannot run Nanite the meshes are still asked for"), Off.Contains(FlowMeshA) && Off.Contains(FlowMeshB) && Off.Contains(FlowMeshC));
	TestFalse(TEXT("on a machine that cannot run Nanite no Nanite file is asked for"), Off.Contains(FlowNaniteA) || Off.Contains(FlowNaniteB));

	const TArray<FString> BothOther = IdsAskedFor(true, Other, Other);
	TestFalse(TEXT("with every file for another engine none is asked for"), BothOther.Contains(FlowNaniteA) || BothOther.Contains(FlowNaniteB));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleNaniteReleaseTest,
	"Mantle.Runtime.StaticMesh.NaniteRelease",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleNaniteReleaseTest::RunTest(const FString& Parameters)
{
	using namespace MantleNaniteSideFileTestPrivate;

	AddExpectedError(TEXT("MANTLE_PLACE_ASSETS FAIL"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedError(TEXT("MANTLE_PLACE_ASSETS READY with"), EAutomationExpectedErrorFlags::Contains, 0);
	const FString Run = FGuid::NewGuid().ToString();

	// A mesh whose download fails never reads its file: the file is freed all the same. First, so no earlier set has built this mesh.
	const TSharedRef<FMantlePlaceAssets> Failed = LoadOneMesh(MakeServer(false), TEXT("NaniteRelease") / Run / TEXT("Failed"));
	TestTrue(TEXT("the place finished with the mesh failed"), Failed->IsComplete() && !Failed->GetResult().bSuccess);
	TestTrue(TEXT("its Nanite file was downloaded and unpacked"), Failed->GetNaniteHeldPeakBytes() > 0);
	TestEqual(TEXT("and nothing is held for the mesh that failed"), Failed->GetNaniteHeldBytes(), static_cast<int64>(0));

	// A mesh that builds: its Nanite file was held while it waited, and is gone once it has built.
	const TSharedRef<FMantlePlaceAssets> Built = LoadOneMesh(MakeServer(true), TEXT("NaniteRelease") / Run / TEXT("Built"));
	TestTrue(TEXT("the place loaded"), Built->IsComplete() && Built->GetResult().bSuccess);
	TestTrue(TEXT("the Nanite file was downloaded and unpacked (held at some point)"), Built->GetNaniteHeldPeakBytes() > 0);
	TestEqual(TEXT("and nothing is held once the mesh has built"), Built->GetNaniteHeldBytes(), static_cast<int64>(0));

	// A mesh reused from an earlier set (still up while the same place is applied again) reads no file either.
	const TSharedRef<FMantlePlaceAssets> Reused = LoadOneMesh(MakeServer(true), TEXT("NaniteRelease") / Run / TEXT("Reused"));
	TestTrue(TEXT("the second set loaded"), Reused->IsComplete() && Reused->GetResult().bSuccess);
	TestTrue(TEXT("its Nanite file was downloaded and unpacked"), Reused->GetNaniteHeldPeakBytes() > 0);
	TestEqual(TEXT("and nothing is held for the mesh it reused from the first set"), Reused->GetNaniteHeldBytes(), static_cast<int64>(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleNaniteManifestOnlyCapTest,
	"Mantle.Runtime.StaticMesh.NaniteManifestOnlyCap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleNaniteManifestOnlyCapTest::RunTest(const FString& Parameters)
{
	using namespace MantleNaniteSideFileTestPrivate;

	// Two script-only meshes (100 bytes each), each with a 150 MiB Nanite file for this engine; no entity uses either.
	const FString Engine = FMantleNaniteSideFile::EngineTag();
	const int64 Mib = 1024 * 1024;
	const FString Json = FString::Printf(
		TEXT("{\"schemaVersion\":1,\"placeId\":\"plc_nanitecap1\",\"revision\":{\"number\":1,\"state\":\"draft\"},"
			 "\"metadata\":{\"name\":\"Cap\",\"authorId\":\"usr_4b1d\"},\"settings\":{},\"assets\":["
			 "{\"id\":\"%s\",\"kind\":\"StaticMesh\",\"hash\":\"%s\",\"bytes\":100,\"nanite\":\"%s\"},"
			 "{\"id\":\"%s\",\"kind\":\"StaticMesh\",\"hash\":\"%s\",\"bytes\":100,\"nanite\":\"%s\"},"
			 "{\"id\":\"%s\",\"kind\":\"Nanite\",\"hash\":\"%s\",\"bytes\":%lld,\"engine\":\"%s\"},"
			 "{\"id\":\"%s\",\"kind\":\"Nanite\",\"hash\":\"%s\",\"bytes\":%lld,\"engine\":\"%s\"}],\"entities\":[]}"),
		FlowMeshA, *MeshTag, FlowNaniteA, FlowMeshB, *MeshTag, FlowNaniteB,
		FlowNaniteA, *MeshTag, 150 * Mib, *Engine, FlowNaniteB, *MeshTag, 150 * Mib, *Engine);
	const FMantleParseResult Parsed = FMantlePlaceParser::ParseFromString(Json);
	if (!TestTrue(TEXT("the place parses"), Parsed.bSuccess))
	{
		return false;
	}

	const auto AskedWithCap = [&Parsed](const int64 CapMib)
	{
		FMantleAssetStoreSettings StoreSettings;
		StoreSettings.RootDir = FPaths::AutomationTransientDir() / TEXT("NaniteCap") / FGuid::NewGuid().ToString();
		const TSharedRef<FRecordingResolver> Resolver = MakeShared<FRecordingResolver>();
		FMantlePlaceAssetSettings Settings;
		Settings.bAutoPump = false;
		Settings.bRenderAssets = true;
		Settings.bLoadNanite = true;
		Settings.MaxManifestOnlyBytes = CapMib * 1024 * 1024;
		Settings.Resolver = Resolver;
		Settings.Store = FMantleAssetStore::Create(StoreSettings, MakeShared<FNoFetcher>());
		const TSharedRef<FMantlePlaceAssets> Assets = FMantlePlaceAssets::PrepareManifestOnly(Parsed.Place, Settings, FMantlePlaceAssetsDone());
		Assets->WaitUntilComplete(30.0f);
		return Resolver->Asked;
	};
	AddExpectedError(TEXT("MANTLE_PLACE_ASSETS FAIL"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedError(TEXT("MANTLE_PLACE_ASSETS READY with"), EAutomationExpectedErrorFlags::Contains, 0);

	// 100 MiB: a file (150 MiB) alone is over the cap, so both meshes are taken and neither file.
	const TArray<FString> Small = AskedWithCap(100);
	TestTrue(TEXT("100 MiB: both meshes are taken"), Small.Contains(FlowMeshA) && Small.Contains(FlowMeshB));
	TestFalse(TEXT("100 MiB: no Nanite file is taken past the cap"), Small.Contains(FlowNaniteA) || Small.Contains(FlowNaniteB));

	// 250 MiB: the first mesh and its file fit (150 MiB and a few bytes); the second mesh's file would take it to 300 MiB.
	const TArray<FString> Medium = AskedWithCap(250);
	TestTrue(TEXT("250 MiB: both meshes are taken"), Medium.Contains(FlowMeshA) && Medium.Contains(FlowMeshB));
	TestTrue(TEXT("250 MiB: the first mesh's file fits and is taken"), Medium.Contains(FlowNaniteA));
	TestFalse(TEXT("250 MiB: the second mesh's file would pass the cap and is not"), Medium.Contains(FlowNaniteB));

	// 1000 MiB: everything fits.
	const TArray<FString> Large = AskedWithCap(1000);
	TestTrue(TEXT("1000 MiB: both files are taken"), Large.Contains(FlowNaniteA) && Large.Contains(FlowNaniteB));
	return true;
}

#endif
