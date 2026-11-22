// Copyright Joshua Eger. All Rights Reserved.
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Async/Async.h"
#include "HAL/PlatformProcess.h"
#include "IO/IoDispatcher.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/Compression.h"
#include "DistanceFieldAtlas.h"
#include "MantleMeshLumenData.h"
#include "MeshCardBuild.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "MantleAssetStore.h"
#include "MantleNaniteSideFile.h"
#include "MantleNanitePageSource.h"
#include "MantlePlaceAssets.h"
#include "MantlePlaceParser.h"
#include "MantleRuntimeStaticMeshBuilder.h"
#include "MantleStaticMeshReader.h"
#include "Matrix3x4.h"
#include "RHI.h"
#include "PhysicsEngine/BodySetup.h"
#include "Rendering/NaniteResources.h"
#include "Serialization/BulkData.h"
#include "Serialization/MemoryWriter.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshResources.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// The game reads a mesh's Nanite file (MND1 version 2, in its MNZ2 container: a head and one piece per streamed page) and puts it on the mesh it builds; the streamed pages are
// read from the file on disk when the engine asks. The file is written here by hand, in the layout of D:\Mantle\Untracked\local\nanite\reports\step1-format.md and
// D:\Mantle\Untracked\local\build-plan\contracts\nanite-v2.md, so the test does not depend on the plugin. No GPU: the meshes are built without render resources.

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
		// The one root page is all of the 37 bytes of root data (version 2 requires it).
		Page.BulkOffset = 0;
		Page.BulkSize = 37;
		Page.PageSize = 30;
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

	/** The head of the file as the plugin writes it (version 2, step1-format.md fields 1 to 25: the head ends there) for MakeResources(), for MeshTagIn's mesh and the slot table. */
	TArray<uint8> WriteFile(const FString& MeshTagIn, const TArray<int32>& Slots);

	/** The head for these resources as the given format version, with TailBytes zero bytes after field 25 (a damaged head). */
	TArray<uint8> WriteFileEx(const FString& MeshTagIn, const TArray<int32>& Slots, uint32 Version, Nanite::FResources Resources, const int32 TailBytes = 0)
	{
		TArray<uint8> Bytes;
		FMemoryWriter Ar(Bytes, /*bIsPersistent*/ true);
		uint8 Magic[4] = { 'M', 'N', 'D', '1' };
		Ar.Serialize(Magic, 4);
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
		for (int32 Tail = 0; Tail < TailBytes; ++Tail)
		{
			uint8 Zero = 0;
			Ar << Zero;
		}
		return Bytes;
	}

	TArray<uint8> WriteFile(const FString& MeshTagIn, const TArray<int32>& Slots)
	{
		Nanite::FResources Resources = MakeResources();
		return WriteFileEx(MeshTagIn, Slots, 2, MoveTemp(Resources));
	}

	/** One zlib stream of Raw (the engine's own NAME_Zlib, as the plugin packs). */
	TArray<uint8> Deflate(const TArray<uint8>& Raw)
	{
		TArray<uint8> Packed;
		Packed.SetNumUninitialized(FCompression::CompressMemoryBound(NAME_Zlib, Raw.Num()));
		int32 PackedSize = Packed.Num();
		verify(FCompression::CompressMemory(NAME_Zlib, Packed.GetData(), PackedSize, Raw.GetData(), Raw.Num(), COMPRESS_BiasMemory));
		Packed.SetNum(PackedSize);
		return Packed;
	}

	/** The container's table as a row each: a piece's zlib length and inflated length. */
	struct FPieceRow
	{
		uint32 Packed = 0;
		uint32 Raw = 0;
	};

	/** An MNZ2 container assembled as given, with no check that the rows and the streams agree: the tests that break one do it here. */
	TArray<uint8> AssembleContainer(const uint32 Count, const TArray<FPieceRow>& Rows, const TArray<TArray<uint8>>& Streams)
	{
		TArray<uint8> Container;
		Container.Append(reinterpret_cast<const uint8*>("MNZ2"), 4);
		Container.Append(reinterpret_cast<const uint8*>(&Count), sizeof(Count));
		for (const FPieceRow& Row : Rows)
		{
			Container.Append(reinterpret_cast<const uint8*>(&Row.Packed), sizeof(uint32));
			Container.Append(reinterpret_cast<const uint8*>(&Row.Raw), sizeof(uint32));
		}
		for (const TArray<uint8>& Stream : Streams)
		{
			Container.Append(Stream);
		}
		return Container;
	}

	/** An MNZ2 container the way the plugin writes it: the table of the head and each page, then their zlib streams one after another. */
	TArray<uint8> MakeContainer(const TArray<uint8>& Head, const TArray<TArray<uint8>>& Pages = TArray<TArray<uint8>>())
	{
		TArray<FPieceRow> Rows;
		TArray<TArray<uint8>> Streams;
		Streams.Add(Deflate(Head));
		Rows.Add({ static_cast<uint32>(Streams[0].Num()), static_cast<uint32>(Head.Num()) });
		for (const TArray<uint8>& Page : Pages)
		{
			Streams.Add(Deflate(Page));
			Rows.Add({ static_cast<uint32>(Streams.Last().Num()), static_cast<uint32>(Page.Num()) });
		}
		return AssembleContainer(Rows.Num(), Rows, Streams);
	}

	/** The table of the container MakeContainer makes of these pieces (worked out here, not parsed, so a head that is itself not good still has one). */
	FMantleNaniteTable TableOf(const TArray<uint8>& Head, const TArray<TArray<uint8>>& Pages = TArray<TArray<uint8>>())
	{
		FMantleNaniteTable Table;
		uint32 PackedAt = 8 + 8 * (1 + Pages.Num());
		uint32 RawAt = 0;
		const auto AddPiece = [&](const TArray<uint8>& Raw, const bool bPage)
		{
			FMantleNaniteTable::FPiece Piece;
			Piece.Packed = Deflate(Raw).Num();
			Piece.PackedStart = PackedAt;
			Piece.Raw = Raw.Num();
			Piece.RawStart = bPage ? RawAt : 0;
			PackedAt += Piece.Packed;
			RawAt += bPage ? Piece.Raw : 0;
			Table.Pieces.Add(Piece);
		};
		AddPiece(Head, false);
		for (const TArray<uint8>& Page : Pages)
		{
			AddPiece(Page, true);
		}
		Table.FileSize = PackedAt;
		Table.StreamedBytes = RawAt;
		return Table;
	}

	/** The reader on a head and the pieces of its container (none: a mesh with no streamed pages). */
	bool ReadHeadBytes(const TArray<uint8>& Head, const FString& Tag, Nanite::FResources& OutResources, TArray<int32>& OutSlots, FString* OutReason,
		const TArray<TArray<uint8>>& Pages = TArray<TArray<uint8>>(), FMantleNaniteStreamedBlock* OutStreamed = nullptr, const int64 MaxPageBytes = MAX_int64)
	{
		return FMantleNaniteSideFile::Read(Head, Tag, TableOf(Head, Pages), MaxPageBytes, OutResources, OutSlots, OutReason, OutStreamed);
	}

	/** Writes the bytes to a new file under the automation transient directory and returns its path. */
	FString WriteTempFile(const TArray<uint8>& Bytes)
	{
		const FString Path = FPaths::AutomationTransientDir() / TEXT("NaniteFiles") / FGuid::NewGuid().ToString() + TEXT(".bin");
		verify(FFileHelper::SaveArrayToFile(Bytes, *Path));
		return Path;
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
		/** #189: a texture's low companion, answered as lowUrl and cookInfo.low when set. */
		TArray<uint8> LowBytes;
		/** #189: false to answer the low companion's url with 404 (a low file whose public copy is missing). */
		bool bServeLow = true;
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
				if (!Served->LowBytes.IsEmpty())
				{
					Asset.LowUrl = TEXT("test://") + Id + TEXT("#low");
					Asset.LowCookedHash = FMantleContentHash::OfBytes(Served->LowBytes).ToString();
					Asset.LowCookedBytes = Served->LowBytes.Num();
				}
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
		/** How often each url was fetched. */
		TMap<FString, int32> Fetched;

		virtual void Fetch(const FString& Url, const TSharedRef<FArchive>& Sink, TFunction<void(const FMantleAssetFetchResult&)> OnDone) override
		{
			FMantleAssetFetchResult Result;
			Result.bConnected = true;
			Result.StatusCode = 404;
			++Fetched.FindOrAdd(Url);
			const bool bLow = Url.EndsWith(TEXT("#low"));
			const FServedAsset* Served = Source->Assets.Find(bLow ? Url.RightChop(7).LeftChop(4) : Url.RightChop(7));
			const TArray<uint8>* Bytes = Served ? (bLow ? &Served->LowBytes : &Served->Bytes) : nullptr;
			if (Served && (bLow ? Served->bServeLow : Served->bServe) && Bytes && !Bytes->IsEmpty())
			{
				Sink->Serialize(const_cast<uint8*>(Bytes->GetData()), Bytes->Num());
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
		Nanite.Bytes = MakeContainer(WriteFile(MeshTag, { 0, 1 }));
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

	/** Inner as the cook stores it (#189 step 5): Magic, its size, and one zlib stream of it (MRM3, MLD2). */
	TArray<uint8> MakeEnvelope(const ANSICHAR* Magic, const TArray<uint8>& Inner)
	{
		TArray<uint8> Packed;
		int32 PackedBytes = FCompression::CompressMemoryBound(NAME_Zlib, Inner.Num());
		Packed.SetNumUninitialized(PackedBytes);
		verify(FCompression::CompressMemory(NAME_Zlib, Packed.GetData(), PackedBytes, Inner.GetData(), Inner.Num()));
		Packed.SetNum(PackedBytes);
		TArray<uint8> Bytes;
		Bytes.Append(reinterpret_cast<const uint8*>(Magic), 4);
		const uint32 Raw = static_cast<uint32>(Inner.Num());
		for (int32 Byte = 0; Byte < 4; ++Byte)
		{
			Bytes.Add(static_cast<uint8>(Raw >> (8 * Byte)));
		}
		Bytes.Append(Packed);
		return Bytes;
	}

	/**
	 * The two-slot box as the cook stores it, MRM3: an MRM2 (one LOD, no extension blocks) in its envelope, its corners at +-Half cm.
	 * Another Half is other bytes, so no earlier set's mesh is reused and the mesh is downloaded and decoded (MRM1 decodes to nothing
	 * held, MRM3 to its description).
	 */
	TArray<uint8> MakeMrm3Box(const float Half)
	{
		FMantleStaticMeshData Mesh = MakeTwoSlotBox();
		for (FVector3f& Position : Mesh.Positions)
		{
			Position = FVector3f(Position.X > 0.0f ? Half : -Half, Position.Y > 0.0f ? Half : -Half, Position.Z > 0.0f ? Half : -Half);
		}
		TArray<uint8> Bytes;
		FMemoryWriter Ar(Bytes, true);
		uint8 Magic[4] = { 'M', 'R', 'M', '2' };
		Ar.Serialize(Magic, 4);
		uint32 Version = FMantleStaticMeshReader::SupportedVersion, Flags = 0;
		uint32 Vertices = static_cast<uint32>(Mesh.Positions.Num()), Indices = static_cast<uint32>(Mesh.Indices.Num());
		Ar << Version << Flags << Vertices << Indices;
		float Bounds[6] = { -Half, -Half, -Half, Half, Half, Half };
		for (float& Value : Bounds)
		{
			Ar << Value;
		}
		uint32 SectionCount = static_cast<uint32>(Mesh.Sections.Num());
		Ar << SectionCount;
		for (const FMantleSkinnedMeshSection& Section : Mesh.Sections)
		{
			uint32 Slot = static_cast<uint32>(Section.MaterialIndex), First = static_cast<uint32>(Section.FirstIndex), Count = static_cast<uint32>(Section.IndexCount);
			Ar << Slot << First << Count;
			const FTCHARToUTF8 Name(*Section.MaterialName.ToString());
			uint16 Length = static_cast<uint16>(Name.Length());
			Ar << Length;
			Ar.Serialize(const_cast<ANSICHAR*>(Name.Get()), Length);
		}
		Ar.Serialize(Mesh.Positions.GetData(), Mesh.Positions.Num() * sizeof(FVector3f));
		Ar.Serialize(Mesh.Normals.GetData(), Mesh.Normals.Num() * sizeof(FVector3f));
		Ar.Serialize(Mesh.Tangents.GetData(), Mesh.Tangents.Num() * sizeof(FVector4f));
		Ar.Serialize(Mesh.UVs.GetData(), Mesh.UVs.Num() * sizeof(FVector2f));
		Ar.Serialize(Mesh.Indices.GetData(), Mesh.Indices.Num() * sizeof(uint32));
		uint32 Extensions = 0;
		Ar << Extensions;
		return MakeEnvelope("MRM3", Bytes);
	}

	UStaticMesh* BuildBox(const TArray<uint8>* File, const FString& Tag)
	{
		FMantleStaticMeshBuildParams Params;
		Params.bInitRenderResources = false;
		Params.NaniteFile = File;
		if (File)
		{
			// A head with no streamed pages: its container holds the head alone.
			Params.NaniteTable = MakeShared<const FMantleNaniteTable, ESPMode::ThreadSafe>(TableOf(*File));
		}
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
	if (!TestTrue(TEXT("its own mesh reads it"), ReadHeadBytes(File, MeshTag, Read, Slots, &Reason)))
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
		TestEqual(TEXT("page offset"), Page.BulkOffset, 0u);
		TestEqual(TEXT("page size"), Page.PageSize, 30u);
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
	TestFalse(TEXT("another mesh's file is not present"), ReadHeadBytes(File, Other, Read, Slots, &Reason));
	TArray<uint8> Cut = File;
	Cut.SetNum(Cut.Num() - 3);
	TestFalse(TEXT("a cut file is not present"), ReadHeadBytes(Cut, MeshTag, Read, Slots, &Reason));
	TArray<uint8> Long = File;
	Long.Add(0);
	TestFalse(TEXT("a file with bytes after its end is not present"), ReadHeadBytes(Long, MeshTag, Read, Slots, &Reason));
	TArray<uint8> Engine = File;
	Engine[12] ^= 0x01; // inside the engine tag's characters (after magic, version and the string's length)
	TestFalse(TEXT("another engine's file is not present"), ReadHeadBytes(Engine, MeshTag, Read, Slots, &Reason));
	TestFalse(TEXT("an empty file is not present"), ReadHeadBytes(TArray<uint8>(), MeshTag, Read, Slots, &Reason));
	TArray<uint8> Magic = File;
	Magic[3] = '2';
	TestFalse(TEXT("another format is not present"), ReadHeadBytes(Magic, MeshTag, Read, Slots, &Reason));

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
	FMantleStaticMeshPrepareTest,
	"Mantle.Runtime.StaticMesh.Prepare",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleStaticMeshPrepareTest::RunTest(const FString& Parameters)
{
	using namespace MantleNaniteSideFileTestPrivate;

	// The box (8 corners, 6 + 6 triangles in two sections, slots "A" and "B") described on a worker thread, not the game thread.
	const FMantleStaticMeshData Box = MakeTwoSlotBox();
	FMantlePreparedStaticMesh Prepared;
	bool bPrepared = false;
	bool bOnGameThread = true;
	// A thread of its own: a task waited on may be run by the waiting thread, the game thread.
	Async(EAsyncExecution::Thread, [&]()
	{
		bOnGameThread = IsInGameThread();
		bPrepared = FMantleRuntimeStaticMeshBuilder::Prepare(Box, true, Prepared, nullptr);
	}).Wait();
	TestFalse(TEXT("it ran off the game thread"), bOnGameThread);
	if (!TestTrue(TEXT("a good mesh prepares"), bPrepared))
	{
		return false;
	}
	TestEqual(TEXT("one LOD (no screen sizes to switch at)"), Prepared.Descriptions.Num(), 1);
	TestEqual(TEXT("two slots"), Prepared.Slots.SlotNames.Num(), 2);
	TestEqual(TEXT("slot 0 is A"), Prepared.Slots.SlotNames[0], FName(TEXT("A")));
	TestEqual(TEXT("slot 1 is B"), Prepared.Slots.SlotNames[1], FName(TEXT("B")));
	TestEqual(TEXT("12 triangles"), Prepared.Triangles, 12);
	TestEqual(TEXT("the description holds 12 triangles"), Prepared.Descriptions[0].Triangles().Num(), 12);
	TestEqual(TEXT("none dropped"), Prepared.Dropped, 0);
	TestEqual(TEXT("the file authored one UV channel"), Prepared.AuthoredChannels, TArray<int32>({ 1 }));
	// A section starts at its first index in the built index buffer: A at 0, B after A's 6 triangles (18 indices).
	if (TestEqual(TEXT("one slot table"), Prepared.Slots.SlotByFirstIndex.Num(), 1))
	{
		const TMap<uint32, int32>& ByFirst = Prepared.Slots.SlotByFirstIndex[0];
		TestEqual(TEXT("two sections"), ByFirst.Num(), 2);
		TestEqual(TEXT("A at index 0"), ByFirst.FindRef(0), 0);
		TestEqual(TEXT("B at index 18"), ByFirst.FindRef(18), 1);
	}

	// The engine's fast build reads every channel, so each is filled from channel 0.
	FStaticMeshAttributes Attributes(Prepared.Descriptions[0]);
	const auto UVs = Attributes.GetVertexInstanceUVs();
	TestEqual(TEXT("filled to every channel"), UVs.GetNumChannels(), static_cast<int32>(MAX_STATIC_TEXCOORDS));
	for (const FVertexInstanceID Instance : Prepared.Descriptions[0].VertexInstances().GetElementIDs())
	{
		for (int32 Channel = 1; Channel < MAX_STATIC_TEXCOORDS; ++Channel)
		{
			if (UVs.Get(Instance, Channel) != UVs.Get(Instance, 0))
			{
				AddError(FString::Printf(TEXT("channel %d of instance %d is not channel 0's"), Channel, Instance.GetValue()));
				break;
			}
		}
	}

	// The decode counts what it holds: 150 bytes a vertex instance, 100 a triangle (12 triangles = 1,200 bytes).
	const int64 Instances = Prepared.Descriptions[0].VertexInstances().GetArraySize();
	TestTrue(TEXT("at least one instance per corner"), Instances >= 8);
	TestEqual(TEXT("its bytes"), Prepared.GetAllocatedBytes(), Instances * 150 + 12 * 100);

	// The two halves make the mesh Build makes.
	FMantleStaticMeshBuildParams Params;
	Params.bInitRenderResources = false;
	TStrongObjectPtr<UStaticMesh> FromHalves(FMantleRuntimeStaticMeshBuilder::BuildPrepared(Box, MoveTemp(Prepared), Params));
	TStrongObjectPtr<UStaticMesh> Whole(FMantleRuntimeStaticMeshBuilder::Build(Box, Params));
	if (TestNotNull(TEXT("BuildPrepared builds"), FromHalves.Get()) && TestNotNull(TEXT("Build builds"), Whole.Get()))
	{
		TestEqual(TEXT("the same triangles"), FromHalves->GetRenderData()->LODResources[0].GetNumTriangles(), Whole->GetRenderData()->LODResources[0].GetNumTriangles());
		TestEqual(TEXT("the same vertices"), FromHalves->GetRenderData()->LODResources[0].GetNumVertices(), Whole->GetRenderData()->LODResources[0].GetNumVertices());
		TestEqual(TEXT("the same slots"), FromHalves->GetStaticMaterials().Num(), Whole->GetStaticMaterials().Num());
	}

	// An inconsistent mesh (two sections for slot 0) is refused, on any thread, with the reason.
	FMantleStaticMeshData Bad = MakeTwoSlotBox();
	Bad.Sections[1].MaterialIndex = 0;
	FMantlePreparedStaticMesh Refused;
	FString Error;
	AddExpectedError(TEXT("MANTLE_STATICMESH FAIL"), EAutomationExpectedErrorFlags::Contains, 0);
	TestFalse(TEXT("a bad mesh is refused"), FMantleRuntimeStaticMeshBuilder::Prepare(Bad, true, Refused, &Error));
	TestTrue(TEXT("with the reason"), Error.Contains(TEXT("inconsistent")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleNaniteReadOffThreadTest,
	"Mantle.Runtime.StaticMesh.NaniteReadOffThread",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleNaniteReadOffThreadTest::RunTest(const FString& Parameters)
{
	using namespace MantleNaniteSideFileTestPrivate;

	// The size line: 4 MiB = 4,194,304 bytes, and a file of that size or more is read on a worker. The biggest Medieval file is 110,280,384 bytes.
	TestEqual(TEXT("the line is 4 MiB"), FMantleRuntimeStaticMeshBuilder::NaniteReadOffThreadBytes, static_cast<int64>(4194304));
	TestFalse(TEXT("an empty file is read in the build"), FMantleRuntimeStaticMeshBuilder::ShouldReadNaniteOffThread(0));
	TestFalse(TEXT("200,916 bytes (a typical mesh) is read in the build"), FMantleRuntimeStaticMeshBuilder::ShouldReadNaniteOffThread(200916));
	TestFalse(TEXT("one byte under the line is read in the build"), FMantleRuntimeStaticMeshBuilder::ShouldReadNaniteOffThread(4194303));
	TestTrue(TEXT("exactly the line is read on a worker"), FMantleRuntimeStaticMeshBuilder::ShouldReadNaniteOffThread(4194304));
	TestTrue(TEXT("the 110 MB mesh is read on a worker"), FMantleRuntimeStaticMeshBuilder::ShouldReadNaniteOffThread(110280384));

	// A good file reads, and says it is done.
	const TArray<uint8> File = WriteFile(MeshTag, { 0, 1 });
	FMantlePreparedNanite Good;
	TestFalse(TEXT("not done before the read"), static_cast<bool>(Good.bDone));
	FMantleRuntimeStaticMeshBuilder::ReadNanite(File, MeshTag, TableOf(File), Good);
	TestTrue(TEXT("done after the read"), static_cast<bool>(Good.bDone));
	TestTrue(TEXT("a good file was read"), Good.bRead);
	if (TestTrue(TEXT("with its data"), Good.Resources.IsValid()))
	{
		TestEqual(TEXT("its root data (37 bytes)"), Good.Resources->RootData.Num(), 37);
		TestEqual(TEXT("its source triangles"), Good.Resources->NumInputTriangles, 123456u);
	}
	TestEqual(TEXT("its slots, in the published order"), Good.SlotToPublished, TArray<int32>({ 0, 1 }));

	// Another mesh's file is refused with a reason, and still says it is done (the build must not wait for it forever).
	FMantlePreparedNanite Wrong;
	FMantleRuntimeStaticMeshBuilder::ReadNanite(File, TEXT("sha256:") + FString::ChrN(64, TEXT('f')), TableOf(File), Wrong);
	TestTrue(TEXT("a refused file is done"), static_cast<bool>(Wrong.bDone));
	TestFalse(TEXT("it was not read"), Wrong.bRead);
	TestFalse(TEXT("it has no data"), Wrong.Resources.IsValid());
	TestFalse(TEXT("it says why"), Wrong.Reason.IsEmpty());

	// The set goes while a read is still running (a place released, a mesh dropped): the read keeps its own hold on its result and the file's bytes, so nothing is read after free.
	{
		const TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe> Bytes = MakeShared<const TArray<uint8>, ESPMode::ThreadSafe>(File);
		const TSharedPtr<const FMantleNaniteTable, ESPMode::ThreadSafe> Table = MakeShared<const FMantleNaniteTable, ESPMode::ThreadSafe>(TableOf(File));
		TSharedPtr<FMantlePreparedNanite, ESPMode::ThreadSafe> InFlight = MakeShared<FMantlePreparedNanite, ESPMode::ThreadSafe>();
		const TWeakPtr<FMantlePreparedNanite, ESPMode::ThreadSafe> Weak = InFlight;
		FEvent* Gate = FPlatformProcess::GetSynchEventFromPool(true);
		TFuture<void> Task = Async(EAsyncExecution::Thread, [InFlight, Bytes, Table, Gate]()
		{
			Gate->Wait();
			FMantleRuntimeStaticMeshBuilder::ReadNanite(*Bytes, MeshTag, *Table, *InFlight);
		});
		InFlight.Reset();
		TestTrue(TEXT("the read holds its result after the set let go"), Weak.IsValid());
		Gate->Trigger();
		Task.Wait();
		FPlatformProcess::ReturnSynchEventToPool(Gate);
	}

	// The build takes the data read on a worker: the mesh is the one the inline read makes.
	FMantleStaticMeshBuildParams Params;
	Params.bInitRenderResources = false;
	Params.NaniteFile = &File;
	Params.NaniteTable = MakeShared<const FMantleNaniteTable, ESPMode::ThreadSafe>(TableOf(File));
	Params.NaniteMeshTag = MeshTag;
	Params.NaniteRead = &Good;
	TStrongObjectPtr<UStaticMesh> Mesh(FMantleRuntimeStaticMeshBuilder::Build(MakeTwoSlotBox(), Params));
	if (TestNotNull(TEXT("the mesh builds from a read made earlier"), Mesh.Get()))
	{
		TestTrue(TEXT("it has valid Nanite data"), Mesh->HasValidNaniteData());
		const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
		if (TestNotNull(TEXT("render data"), RenderData) && TestTrue(TEXT("Nanite resources"), RenderData->NaniteResourcesPtr.IsValid()))
		{
			TestEqual(TEXT("its root data"), RenderData->NaniteResourcesPtr->RootData.Num(), 37);
			TestEqual(TEXT("its source triangles"), RenderData->NaniteResourcesPtr->NumInputTriangles, 123456u);
		}
	}

	// A refused read leaves the mesh plain, as an inline refusal does, and the mesh still builds.
	Params.NaniteRead = &Wrong;
	TStrongObjectPtr<UStaticMesh> Plain(FMantleRuntimeStaticMeshBuilder::Build(MakeTwoSlotBox(), Params));
	if (TestNotNull(TEXT("a mesh with a refused read still builds"), Plain.Get()))
	{
		TestFalse(TEXT("without Nanite data"), Plain->HasValidNaniteData());
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

	const auto Pattern = [](const int32 Seed, const int32 Size)
	{
		TArray<uint8> Out;
		for (int32 Index = 0; Index < Size; ++Index)
		{
			Out.Add(static_cast<uint8>(Index * 5 + Seed * 17 + Index / 89));
		}
		return Out;
	};
	const TArray<uint8> Head = WriteFile(MeshTag, { 0, 1 });
	const TArray<TArray<uint8>> Pages = { Pattern(1, 3000), Pattern(2, 100), Pattern(3, 5000) };
	const TArray<uint8> Container = MakeContainer(Head, Pages);

	// The table is read from the file's size and bytes alone: where each piece is and what it inflates to, nothing inflated.
	FMantleNaniteTable Table;
	FString Reason;
	if (!TestTrue(TEXT("a good container's table is read"), FMantleNaniteSideFile::ParseTable(Container, Container.Num(), Table, &Reason)))
	{
		AddInfo(Reason);
		return false;
	}
	TestEqual(TEXT("the head and three pages"), Table.Pieces.Num(), 4);
	TestEqual(TEXT("three streamed pages"), Table.PageCount(), 3);
	TestEqual(TEXT("the streamed block is the pages' 8,100 bytes"), Table.StreamedBytes, static_cast<int64>(8100));
	TestEqual(TEXT("the head inflates to the MND1's head"), Table.Pieces[0].Raw, static_cast<uint32>(Head.Num()));
	TestEqual(TEXT("the pages start at 0, 3,000 and 3,100 in the block"), TArray<uint32>({ Table.Pieces[1].RawStart, Table.Pieces[2].RawStart, Table.Pieces[3].RawStart }), TArray<uint32>({ 0u, 3000u, 3100u }));
	TestEqual(TEXT("the head starts right after the table (8 + 8 x 4)"), Table.Pieces[0].PackedStart, 40u);
	TestEqual(TEXT("the pieces follow one another"), Table.Pieces[2].PackedStart, Table.Pieces[1].PackedStart + Table.Pieces[1].Packed);
	TestEqual(TEXT("and fill the file exactly"), static_cast<int64>(Table.Pieces[3].PackedStart) + Table.Pieces[3].Packed, static_cast<int64>(Container.Num()));
	TestEqual(TEXT("the file's size is kept"), Table.FileSize, static_cast<int64>(Container.Num()));
	TestEqual(TEXT("a page is found by its exact start and size"), Table.FindPage(3000, 100), 2);
	TestEqual(TEXT("the first"), Table.FindPage(0, 3000), 1);
	TestEqual(TEXT("the last"), Table.FindPage(3100, 5000), 3);
	TestEqual(TEXT("a range inside a page is not a page"), Table.FindPage(10, 100), static_cast<int32>(INDEX_NONE));
	TestEqual(TEXT("a page's start with another size is not a page"), Table.FindPage(3000, 99), static_cast<int32>(INDEX_NONE));
	TestEqual(TEXT("two pages together are not a page"), Table.FindPage(0, 3100), static_cast<int32>(INDEX_NONE));
	TestEqual(TEXT("past the block is not a page"), Table.FindPage(8100, 1), static_cast<int32>(INDEX_NONE));
	TestEqual(TEXT("an offset past 32 bits is not a page"), Table.FindPage(0x100000000ull, 3000), static_cast<int32>(INDEX_NONE));

	// Each piece inflates alone, in any order, to what was written.
	for (const int32 Piece : { 3, 0, 2, 1 })
	{
		const FMantleNaniteTable::FPiece& Entry = Table.Pieces[Piece];
		const TArray<uint8>& Expected = Piece == 0 ? Head : Pages[Piece - 1];
		TArray<uint8> Out;
		Out.SetNumUninitialized(Entry.Raw);
		TestTrue(*FString::Printf(TEXT("piece %d inflates alone"), Piece), FMantleNaniteSideFile::InflatePiece(TConstArrayView<uint8>(Container.GetData() + Entry.PackedStart, Entry.Packed), Out.GetData(), Entry.Raw, &Reason));
		TestEqual(*FString::Printf(TEXT("to what was written (piece %d)"), Piece), Out, Expected);
	}

	// The table is checked before anything is allocated or inflated.
	const auto TableRefused = [this](const TCHAR* What, const TArray<uint8>& Bytes, const int64 FileSize = -1)
	{
		FMantleNaniteTable Refused;
		FString Why;
		const bool bRefused = !FMantleNaniteSideFile::ParseTable(Bytes, FileSize < 0 ? Bytes.Num() : FileSize, Refused, &Why);
		TestTrue(What, bRefused);
		TestTrue(*FString::Printf(TEXT("%s: it leaves nothing behind"), What), Refused.Pieces.IsEmpty());
		TestFalse(*FString::Printf(TEXT("%s: it says why"), What), Why.IsEmpty());
	};
	const FPieceRow HeadRow{ static_cast<uint32>(Deflate(Head).Num()), static_cast<uint32>(Head.Num()) };
	const TArray<TArray<uint8>> OneStream = { Deflate(Head) };
	const TArray<uint8> Eight = { 0x78, 0x9C, 0, 0, 0, 0, 0, 0 };
	TableRefused(TEXT("an empty file"), TArray<uint8>());
	TableRefused(TEXT("a raw MND1"), Head);
	TableRefused(TEXT("the magic only"), TArray<uint8>({ 'M', 'N', 'Z', '2' }));
	{
		// The old container: "MNZ1", a size word and one zlib stream.
		TArray<uint8> Old = Container;
		Old[3] = '1';
		TableRefused(TEXT("the old MNZ1 magic"), Old);
	}
	TableRefused(TEXT("no pieces"), AssembleContainer(0, {}, {}));
	TableRefused(TEXT("a count of 1000 in a small file"), AssembleContainer(1000, { HeadRow }, OneStream));
	TableRefused(TEXT("a count of 4 billion in a small file"), AssembleContainer(0xFFFFFFFFu, { HeadRow }, OneStream));
	{
		TArray<uint8> More = Container;
		More.Add(0);
		TableRefused(TEXT("a byte after the last piece"), More);
		TArray<uint8> Fewer = Container;
		Fewer.Pop();
		TableRefused(TEXT("a byte short of the last piece"), Fewer);
	}
	TableRefused(TEXT("a piece under 8 bytes packed"), AssembleContainer(2, { HeadRow, { 7, 100 } }, { Deflate(Head), TArray<uint8>(Eight.GetData(), 7) }));
	TableRefused(TEXT("a page of 0 inflated"), AssembleContainer(2, { HeadRow, { 8, 0 } }, { Deflate(Head), Eight }));
	TableRefused(TEXT("a head of under 8 inflated"), AssembleContainer(1, { { HeadRow.Packed, 7 } }, OneStream));
	TableRefused(TEXT("a page past 512 MiB"), AssembleContainer(2, { HeadRow, { 8, FMantleNaniteSideFile::MaxInflatedBytes + 1 } }, { Deflate(Head), Eight }));
	TableRefused(TEXT("the pieces together past 512 MiB"), AssembleContainer(3, { HeadRow, { 8, 300u << 20 }, { 8, 300u << 20 } }, { Deflate(Head), Eight, Eight }));
	TableRefused(TEXT("a file past the 64 MiB limit"), Container, FMantleNaniteSideFile::MaxFileBytes + 1);

	// A piece must be exactly its stream: whole, ending where its table row says, inflating to exactly its Raw (the engine's FCompression does not refuse bytes after a stream).
	{
		const TArray<uint8> Raw = Pattern(9, 4000);
		const TArray<uint8> Stream = Deflate(Raw);
		const auto Inflates = [&](const TArray<uint8>& Packed, const uint32 RawBytes, FString& Why)
		{
			TArray<uint8> Out;
			Out.SetNumZeroed(RawBytes + 8);
			return FMantleNaniteSideFile::InflatePiece(Packed, Out.GetData(), RawBytes, &Why);
		};
		FString Why;
		TestTrue(TEXT("the whole stream inflates to its size"), Inflates(Stream, Raw.Num(), Why));
		TArray<uint8> Trailing = Stream;
		Trailing.Add(0);
		TestFalse(TEXT("one byte after the stream is refused"), Inflates(Trailing, Raw.Num(), Why));
		TestTrue(TEXT("and the reason says bytes follow"), Why.Contains(TEXT("follow")));
		TArray<uint8> Cut = Stream;
		Cut.Pop();
		TestFalse(TEXT("a stream one byte short is refused"), Inflates(Cut, Raw.Num(), Why));
		TestFalse(TEXT("a stream of 3 bytes is refused"), Inflates(TArray<uint8>(Stream.GetData(), 3), Raw.Num(), Why));
		TestFalse(TEXT("a stream that inflates to one more than the table says is refused"), Inflates(Stream, Raw.Num() - 1, Why));
		TestFalse(TEXT("a stream that inflates to one less than the table says is refused"), Inflates(Stream, Raw.Num() + 1, Why));
		TArray<uint8> BadSum = Stream;
		BadSum.Last() ^= 0xFF;
		TestFalse(TEXT("a bad Adler-32 is refused"), Inflates(BadSum, Raw.Num(), Why));
		TArray<uint8> Damaged = Stream;
		Damaged[Damaged.Num() / 2] ^= 0x5A;
		TestFalse(TEXT("a damaged body is refused"), Inflates(Damaged, Raw.Num(), Why));
		TArray<uint8> NotZlib;
		for (int32 Index = 0; Index < 64; ++Index)
		{
			NotZlib.Add(static_cast<uint8>(Index * 31));
		}
		TestFalse(TEXT("bytes that are not a zlib stream are refused"), Inflates(NotZlib, Raw.Num(), Why));
		TestFalse(TEXT("an empty piece is refused"), Inflates(TArray<uint8>(), Raw.Num(), Why));

		// Never past the room it is given: the 8 bytes after the target stay what they were.
		TArray<uint8> Out;
		Out.SetNumZeroed(Raw.Num() - 1 + 8);
		TestFalse(TEXT("a target one byte short is refused"), FMantleNaniteSideFile::InflatePiece(Stream, Out.GetData(), Raw.Num() - 1, &Why));
		bool bUntouched = true;
		for (int32 Index = Raw.Num() - 1; Index < Out.Num(); ++Index)
		{
			bUntouched &= Out[Index] == 0;
		}
		TestTrue(TEXT("and nothing was written past it"), bUntouched);
	}

	// The head is read from the file on disk, and nothing else of it.
	{
		const FString Path = WriteTempFile(Container);
		FMantleNaniteTable FileTable;
		TArray<uint8> FileHead;
		if (TestTrue(TEXT("the head is read from the file"), FMantleNaniteSideFile::ReadHead(Path, FileTable, FileHead, &Reason)))
		{
			TestEqual(TEXT("it is the head"), FileHead, Head);
			TestEqual(TEXT("with the three pages in its table"), FileTable.PageCount(), 3);
			int64 RootBytes = 0;
			TestTrue(TEXT("and the head is an MND1 whose root data is 37 bytes"), FMantleNaniteSideFile::PeekRootBytes(FileHead, RootBytes) && RootBytes == 37);
		}
		// A damaged page does not stop the head from being read: no page is read at load.
		TArray<uint8> PageDamaged = Container;
		PageDamaged[Table.Pieces[2].PackedStart + 3] ^= 0xFF;
		TestTrue(TEXT("a file whose page is damaged still gives its head"), FMantleNaniteSideFile::ReadHead(WriteTempFile(PageDamaged), FileTable, FileHead, &Reason));
		// A file with no streamed pages is its head alone.
		TestTrue(TEXT("a head-only file is read"), FMantleNaniteSideFile::ReadHead(WriteTempFile(MakeContainer(Head)), FileTable, FileHead, &Reason));
		TestEqual(TEXT("with no pages"), FileTable.PageCount(), 0);
		TestEqual(TEXT("and no streamed bytes"), FileTable.StreamedBytes, static_cast<int64>(0));
		// Refused: no file, a file of another size than its table, a damaged head, another container.
		TestFalse(TEXT("a missing file is refused"), FMantleNaniteSideFile::ReadHead(Path + TEXT(".missing"), FileTable, FileHead, &Reason));
		TArray<uint8> Longer = Container;
		Longer.Add(0);
		TestFalse(TEXT("a file with a byte more than its table says is refused"), FMantleNaniteSideFile::ReadHead(WriteTempFile(Longer), FileTable, FileHead, &Reason));
		TArray<uint8> HeadDamaged = Container;
		HeadDamaged[Table.Pieces[0].PackedStart + Table.Pieces[0].Packed / 2] ^= 0xFF;
		TestFalse(TEXT("a damaged head is refused"), FMantleNaniteSideFile::ReadHead(WriteTempFile(HeadDamaged), FileTable, FileHead, &Reason));
		TestTrue(TEXT("and leaves no bytes behind"), FileHead.IsEmpty());
		TestFalse(TEXT("a raw MND1 on disk is refused"), FMantleNaniteSideFile::ReadHead(WriteTempFile(Head), FileTable, FileHead, &Reason));
	}


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
	const TSharedRef<FServingResolver> BuiltServer = MakeServer(true);
	const TSharedRef<FMantlePlaceAssets> Built = LoadOneMesh(BuiltServer, TEXT("NaniteRelease") / Run / TEXT("Built"));
	TestTrue(TEXT("the place loaded"), Built->IsComplete() && Built->GetResult().bSuccess);
	TestTrue(TEXT("the Nanite file was downloaded and unpacked (held at some point)"), Built->GetNaniteHeldPeakBytes() > 0);
	TestEqual(TEXT("and nothing is held once the mesh has built"), Built->GetNaniteHeldBytes(), static_cast<int64>(0));
	// #189: a built file is not pinned any more (its bytes are in memory), so the store's LRU can work; the Nanite file stays pinned
	// while the set lives, because the mesh's pages are read from it.
	TestFalse(TEXT("the built mesh's file is no longer pinned"),
		Built->GetStore().IsPinned(FMantleContentHash::OfBytes(BuiltServer->Assets.FindChecked(FlowMeshA).Bytes)));
	TestTrue(TEXT("its Nanite file stays pinned"),
		Built->GetStore().IsPinned(FMantleContentHash::OfBytes(BuiltServer->Assets.FindChecked(FlowNaniteA).Bytes)));

	// A mesh reused from an earlier set (still up while the same place is applied again) reads no file either.
	const TSharedRef<FMantlePlaceAssets> Reused = LoadOneMesh(MakeServer(true), TEXT("NaniteRelease") / Run / TEXT("Reused"));
	TestTrue(TEXT("the second set loaded"), Reused->IsComplete() && Reused->GetResult().bSuccess);
	TestTrue(TEXT("its Nanite file was downloaded and unpacked"), Reused->GetNaniteHeldPeakBytes() > 0);
	TestEqual(TEXT("and nothing is held for the mesh it reused from the first set"), Reused->GetNaniteHeldBytes(), static_cast<int64>(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleNaniteBudgetWaitTest,
	"Mantle.Runtime.StaticMesh.NaniteFetchOverBudget",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleNaniteBudgetWaitTest::RunTest(const FString& Parameters)
{
	using namespace MantleNaniteSideFileTestPrivate;

	// OrbitalBase (2026-11-22): decoded meshes waiting for their Nanite files filled MaxPendingBuildBytes, and no download started above it,
	// so the files they waited for never started and the join stalled at 170 of 458 MB with nothing downloading. Here one fetch at a time
	// and a 1-byte budget: the mesh downloads first, decodes and waits holding its bytes, and its Nanite file must still start.
	const FString Run = FGuid::NewGuid().ToString();
	const float Half = 50.0f + static_cast<float>(FMath::RandRange(1, 1 << 20)) / 64.0f;

	const TSharedRef<FServingResolver> Server = MakeShared<FServingResolver>();
	FServedAsset Mesh;
	Mesh.Kind = TEXT("mesh");
	Mesh.Bytes = MakeMrm3Box(Half);
	FMantleStaticMeshData Check;
	FString Error;
	if (!TestTrue(TEXT("the test mesh is a good MRM3 file"), FMantleStaticMeshReader::Read(Mesh.Bytes, Check, &Error)))
	{
		AddInfo(Error);
		return false;
	}
	Server->Assets.Add(FlowMeshA, Mesh);
	FServedAsset Nanite;
	Nanite.Kind = TEXT("nanite");
	Nanite.Bytes = MakeContainer(WriteFile(MeshTag, { 0, 1 }));
	Server->Assets.Add(FlowNaniteA, Nanite);

	FMantleAssetStoreSettings StoreSettings;
	StoreSettings.RootDir = FPaths::AutomationTransientDir() / TEXT("NaniteBudget") / Run;
	StoreSettings.MaxAttempts = 1;
	const TSharedRef<FServingFetcher> Fetcher = MakeShared<FServingFetcher>();
	Fetcher->Source = Server;
	FMantlePlaceAssetSettings Settings;
	Settings.bAutoPump = false;
	Settings.bRenderAssets = true;
	Settings.bLoadNanite = true;
	Settings.bAsyncCollisionCook = false;
	Settings.MaxOutstandingFetches = 1;
	Settings.MaxPendingBuildBytes = 1;
	Settings.Resolver = Server;
	Settings.Store = FMantleAssetStore::Create(StoreSettings, Fetcher);
	const TSharedRef<FMantlePlaceAssets> Assets = FMantlePlaceAssets::Prepare(MakeOneMeshPlace(), Settings, FMantlePlaceAssetsDone());
	const bool bFinished = Assets->WaitUntilComplete(30.0f);

	const FMantlePlaceAssetProgress Progress = Assets->GetProgress();
	AddInfo(FString::Printf(TEXT("%d of %d ready, %lld of %lld bytes started, %d started over the budget"),
		Progress.ReadyCount, Progress.TotalCount, Progress.DoneBytes, Progress.TotalBytes, Assets->GetFetchesOverBudget()));
	TestTrue(TEXT("the place finished loading (it stalled before: the Nanite file never started)"), bFinished && Assets->IsComplete());
	TestTrue(TEXT("and loaded"), Assets->IsComplete() && Assets->GetResult().bSuccess);
	TestNotNull(TEXT("the mesh built"), Assets->FindMesh(FlowMeshA));
	TestEqual(TEXT("only the Nanite file started above the budget"), Assets->GetFetchesOverBudget(), 1);
	TestEqual(TEXT("nothing is held for the mesh once it has built"), Assets->GetNaniteHeldBytes(), static_cast<int64>(0));
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

// ---- Streamed assets (stage 2 of large places): a streaming set takes a cell's meshes when the cell asks and lets them go when the last cell that wants them does ----

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleStreamedAssetsTestPrivate
{
	using namespace MantleNaniteSideFileTestPrivate;

	const TCHAR* const MeshF = TEXT("ast_u000000000000000000000000000000f1"); // its download is refused
	const TCHAR* const MaterialM = TEXT("ast_u000000000000000000000000000000c1"); // its download is refused, its answer from the API names a texture
	const TCHAR* const TextureT = TEXT("ast_u000000000000000000000000000000d1");
	const TCHAR* const MeshA2 = TEXT("ast_u000000000000000000000000000000a2"); // the same bytes as A (so the same build), with a Nanite file of its own
	const TCHAR* const NaniteA2 = TEXT("ast_u000000000000000000000000000000a3");

	/** The fake API, which also says (as the real one does for a material) which texture a material names. */
	class FMaterialAnswerResolver : public FServingResolver
	{
	public:
		virtual void Resolve(const TArray<FString>& Ids, TFunction<void(const FMantleAssetResolveResult&)> OnDone) override
		{
			FServingResolver::Resolve(Ids, [OnDone](const FMantleAssetResolveResult& Answer)
			{
				FMantleAssetResolveResult Changed = Answer;
				for (FMantleUploadedAsset& Asset : Changed.Assets)
				{
					if (Asset.Id == MaterialM)
					{
						Asset.MaterialTextures.Add(TEXT("baseColor"), TextureT);
					}
				}
				OnDone(Changed);
			});
		}
	};

	/** A box as an MRM1 mesh, Half cm to a side's middle: a different size is a different file, so a different cooked hash. */
	TArray<uint8> MakeSizedBox(const float Half)
	{
		TArray<uint8> Bytes;
		FMemoryWriter Ar(Bytes, true);
		uint8 Magic[4] = { 'M', 'R', 'M', '1' };
		Ar.Serialize(Magic, 4);
		uint32 Version = 1, Flags = 0, Vertices = 8, Indices = 36;
		Ar << Version << Flags << Vertices << Indices;
		float Bounds[6] = { -Half, -Half, -Half, Half, Half, Half };
		for (float& Value : Bounds)
		{
			Ar << Value;
		}
		for (int32 Corner = 0; Corner < 8; ++Corner)
		{
			float X = (Corner & 1) ? Half : -Half, Y = (Corner & 2) ? Half : -Half, Z = (Corner & 4) ? Half : -Half;
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

	/** A place whose manifest lists meshes A (with a Nanite file), B and F, and whose root has no entity: everything it uses is in cells. */
	FMantlePlace MakeCellsOnlyPlace()
	{
		const FString Json = FString::Printf(
			TEXT("{\"schemaVersion\":1,\"placeId\":\"plc_streamed001\",\"revision\":{\"number\":1,\"state\":\"draft\"},"
				 "\"metadata\":{\"name\":\"Streamed\",\"authorId\":\"usr_4b1d\"},\"settings\":{},\"assets\":["
				 "{\"id\":\"%s\",\"kind\":\"StaticMesh\",\"hash\":\"%s\",\"bytes\":100,\"nanite\":\"%s\"},"
				 "{\"id\":\"%s\",\"kind\":\"Nanite\",\"hash\":\"%s\",\"bytes\":50,\"engine\":\"%s\"},"
				 "{\"id\":\"%s\",\"kind\":\"StaticMesh\",\"hash\":\"%s\",\"bytes\":100},"
				 "{\"id\":\"%s\",\"kind\":\"StaticMesh\",\"hash\":\"%s\",\"bytes\":100},"
				 "{\"id\":\"%s\",\"kind\":\"Material\",\"hash\":\"%s\",\"bytes\":100},"
				 "{\"id\":\"%s\",\"kind\":\"Texture\",\"hash\":\"%s\",\"bytes\":100},"
				 "{\"id\":\"%s\",\"kind\":\"StaticMesh\",\"hash\":\"%s\",\"bytes\":100,\"nanite\":\"%s\"},"
				 "{\"id\":\"%s\",\"kind\":\"Nanite\",\"hash\":\"%s\",\"bytes\":50,\"engine\":\"%s\"}],\"entities\":[]}"),
			FlowMeshA, *MeshTag, FlowNaniteA, FlowNaniteA, *MeshTag, *FMantleNaniteSideFile::EngineTag(), FlowMeshB, *MeshTag, MeshF, *MeshTag, MaterialM, *MeshTag, TextureT, *MeshTag,
			MeshA2, *MeshTag, NaniteA2, NaniteA2, *MeshTag, *FMantleNaniteSideFile::EngineTag());
		const FMantleParseResult Parsed = FMantlePlaceParser::ParseFromString(Json);
		check(Parsed.bSuccess);
		return Parsed.Place;
	}

	/** One StaticMesh entity of the given mesh, with or without collision, for a cell's list. */
	FMantleEntity MeshEntity(const FString& Id, const FString& Mesh, const bool bCollision)
	{
		FMantleEntity Entity;
		Entity.Id = Id;
		Entity.Type = EMantleEntityType::StaticMesh;
		FMantleStaticMeshProps Props;
		Props.Mesh = Mesh;
		Props.Collision = bCollision ? EMantleCollision::Blocking : EMantleCollision::None;
		Entity.Props.Set<FMantleStaticMeshProps>(Props);
		return Entity;
	}

	struct FStreamingFixture
	{
		TSharedRef<FServingResolver> Server = MakeShared<FMaterialAnswerResolver>();
		TSharedPtr<FMantleAssetStore> Store;
		TSharedPtr<FMantlePlaceAssets> Assets;

		explicit FStreamingFixture(const FString& StoreName, const bool bNanite = true)
		{
			FServedAsset A;
			A.Kind = TEXT("mesh");
			A.Bytes = MakeSizedBox(50.0f);
			Server->Assets.Add(FlowMeshA, A);
			FServedAsset B;
			B.Kind = TEXT("mesh");
			B.Bytes = MakeSizedBox(60.0f);
			Server->Assets.Add(FlowMeshB, B);
			FServedAsset F;
			F.Kind = TEXT("mesh");
			F.Bytes = MakeSizedBox(70.0f);
			F.bServe = false;
			Server->Assets.Add(MeshF, F);
			FServedAsset Nanite;
			Nanite.Kind = TEXT("nanite");
			Nanite.Bytes = MakeContainer(WriteFile(MeshTag, { 0, 1 }));
			Server->Assets.Add(FlowNaniteA, Nanite);
			// A second mesh with the very same bytes as A (one build between them) whose Nanite file is another file.
			FServedAsset SameAsA;
			SameAsA.Kind = TEXT("mesh");
			SameAsA.Bytes = MakeSizedBox(50.0f);
			Server->Assets.Add(MeshA2, SameAsA);
			FServedAsset NaniteTwo;
			NaniteTwo.Kind = TEXT("nanite");
			NaniteTwo.Bytes = MakeContainer(WriteFile(MeshTag, { 0 }));
			Server->Assets.Add(NaniteA2, NaniteTwo);
			// A material whose download is refused (so it fails before its document is read), and the texture its answer names (not a readable texture either).
			FServedAsset Material;
			Material.Kind = TEXT("material");
			Material.Bytes = MakeSizedBox(80.0f);
			Material.bServe = false;
			Server->Assets.Add(MaterialM, Material);
			FServedAsset Texture;
			Texture.Kind = TEXT("texture");
			Texture.Bytes = MakeSizedBox(90.0f);
			Server->Assets.Add(TextureT, Texture);

			FMantleAssetStoreSettings StoreSettings;
			StoreSettings.RootDir = FPaths::AutomationTransientDir() / StoreName;
			StoreSettings.MaxAttempts = 1;
			const TSharedRef<FServingFetcher> Fetcher = MakeShared<FServingFetcher>();
			Fetcher->Source = Server;
			FMantlePlaceAssetSettings Settings;
			Settings.bAutoPump = false;
			Settings.bRenderAssets = true;
			Settings.bLoadNanite = bNanite;
			Settings.bAsyncCollisionCook = false;
			Settings.Resolver = Server;
			Store = FMantleAssetStore::Create(StoreSettings, Fetcher);
			Settings.Store = Store;
			Assets = FMantlePlaceAssets::PrepareStreaming(MakeCellsOnlyPlace(), Settings, FMantlePlaceAssetsDone());
		}

		/** Pumps the store and the set until the predicate holds or the time is up. */
		template <typename TPredicate>
		bool PumpUntil(TPredicate Done, const double Seconds = 30.0)
		{
			const double Deadline = FPlatformTime::Seconds() + Seconds;
			while (!Done())
			{
				if (FPlatformTime::Seconds() > Deadline)
				{
					return false;
				}
				Store->Pump();
				Assets->Pump();
				FPlatformProcess::Sleep(0.001f);
			}
			return true;
		}

		bool Settle(const int64 Holder) { return PumpUntil([this, Holder]() { return Assets->GetHolderAssets(Holder).IsSettled(); }); }
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleStreamedAssetsFollowTheHoldersTest,
	"Mantle.Runtime.Place.StreamedAssets.FollowTheHolders",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleStreamedAssetsFollowTheHoldersTest::RunTest(const FString& Parameters)
{
	using namespace MantleStreamedAssetsTestPrivate;
	FStreamingFixture Fixture(TEXT("StreamedAssets") / FGuid::NewGuid().ToString());
	if (!TestTrue(TEXT("the root (no assets of its own) completes"), Fixture.PumpUntil([&Fixture]() { return Fixture.Assets->IsComplete(); })))
	{
		return false;
	}
	TestTrue(TEXT("it is a streaming set"), Fixture.Assets->IsStreaming());
	FMantlePlaceAssets::FStreamedCounts Counts = Fixture.Assets->GetStreamedCounts();
	TestEqual(TEXT("it holds no mesh before a cell asks"), Counts.Meshes, 0);

	// Cell 10 uses A (solid) and cell 11 uses A and B (pictures): A is fetched once for both, with its Nanite file.
	TArray<FMantleEntity> Cell10;
	Cell10.Add(MeshEntity(TEXT("ent_10"), FlowMeshA, true));
	TArray<FMantleEntity> Cell11;
	Cell11.Add(MeshEntity(TEXT("ent_11a"), FlowMeshA, false));
	Cell11.Add(MeshEntity(TEXT("ent_11b"), FlowMeshB, false));
	Fixture.Assets->AcquireEntityAssets(10, Cell10);
	Fixture.Assets->AcquireEntityAssets(11, Cell11);
	TestFalse(TEXT("nothing is ready the moment a cell asks"), Fixture.Assets->GetHolderAssets(10).IsSettled());
	if (!TestTrue(TEXT("both cells' assets arrive"), Fixture.Settle(10) && Fixture.Settle(11)))
	{
		return false;
	}
	TestEqual(TEXT("nothing failed"), Fixture.Assets->GetHolderAssets(10).FailedIds.Num() + Fixture.Assets->GetHolderAssets(11).FailedIds.Num(), 0);
	FString Error;
	UStaticMesh* MeshA = FMantlePlaceAssets::FindLiveMesh(FlowMeshA, Error);
	UStaticMesh* MeshB = FMantlePlaceAssets::FindLiveMesh(FlowMeshB, Error);
	TestNotNull(TEXT("A is built"), MeshA);
	TestNotNull(TEXT("B is built"), MeshB);
	TestTrue(TEXT("A was built with collision for the solid cell"), MeshA && MeshA->GetBodySetup() && MeshA->GetBodySetup()->CollisionTraceFlag == CTF_UseComplexAsSimple);
	TestEqual(TEXT("A's Nanite file is not held once it has been read"), Fixture.Assets->GetNaniteHeldBytes(), static_cast<int64>(0));
	Counts = Fixture.Assets->GetStreamedCounts();
	TestEqual(TEXT("two meshes, A once for its two cells"), Counts.Meshes, 2);
	TestEqual(TEXT("two meshes are wanted"), Counts.WantedMeshes, 2);
	TestEqual(TEXT("two cells hold"), Counts.Holders, 2);
	TestEqual(TEXT("A's Nanite record is held"), Counts.NaniteFiles, 1);

	// Cell 10 comes down: A is still wanted by cell 11, so it stays (the same object).
	Fixture.Assets->ReleaseEntityAssets(10);
	Fixture.PumpUntil([]() { return false; }, 0.05);
	TestTrue(TEXT("A stays while cell 11 wants it"), FMantlePlaceAssets::FindLiveMesh(FlowMeshA, Error) == MeshA && MeshA != nullptr);
	Counts = Fixture.Assets->GetStreamedCounts();
	TestEqual(TEXT("both meshes are still held"), Counts.Meshes, 2);
	TestEqual(TEXT("one cell holds"), Counts.Holders, 1);
	TestEqual(TEXT("nothing dropped yet"), Counts.Dropped, static_cast<int64>(0));

	// Cell 11 comes down: nobody wants either, so both go, and A's Nanite record with it.
	Fixture.Assets->ReleaseEntityAssets(11);
	TestTrue(TEXT("the last cell's release drops them"), Fixture.PumpUntil([&Fixture]() { return Fixture.Assets->GetStreamedCounts().Meshes == 0; }));
	Counts = Fixture.Assets->GetStreamedCounts();
	TestEqual(TEXT("no mesh is held"), Counts.Meshes, 0);
	TestEqual(TEXT("A's Nanite record went with it"), Counts.NaniteFiles, 0);
	TestEqual(TEXT("no mesh is wanted"), Counts.WantedMeshes, 0);
	TestEqual(TEXT("two were dropped"), Counts.Dropped, static_cast<int64>(2));
	TestEqual(TEXT("none was waiting"), Counts.DropsWaiting, 0);
	TestTrue(TEXT("the set answers 'not prepared' for A again"), FMantlePlaceAssets::GetLiveMeshState(FlowMeshA, &Error) != EMantleLiveAssetState::Ready);

	// A cell that wants B again after that fetches it again.
	Fixture.Assets->AcquireEntityAssets(12, Cell11);
	TestTrue(TEXT("a later cell gets its meshes again"), Fixture.Settle(12));
	TestNotNull(TEXT("and A is there"), FMantlePlaceAssets::FindLiveMesh(FlowMeshA, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleStreamedAssetsCollisionAndFailureTest,
	"Mantle.Runtime.Place.StreamedAssets.CollisionAndFailure",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleStreamedAssetsCollisionAndFailureTest::RunTest(const FString& Parameters)
{
	using namespace MantleStreamedAssetsTestPrivate;
	AddExpectedError(TEXT("MANTLE_PLACE_ASSETS FAIL"), EAutomationExpectedErrorFlags::Contains, 0);
	FStreamingFixture Fixture(TEXT("StreamedAssets") / FGuid::NewGuid().ToString(), /*bNanite*/ true);
	if (!TestTrue(TEXT("the root completes"), Fixture.PumpUntil([&Fixture]() { return Fixture.Assets->IsComplete(); })))
	{
		return false;
	}
	FString Error;

	// A picture cell asks for A (a mesh with a Nanite file) first: it is built without collision. Then a solid cell asks for it: it is built again with it, and the picture cell's own
	// object is still the one its components hold.
	TArray<FMantleEntity> Picture;
	Picture.Add(MeshEntity(TEXT("ent_p"), FlowMeshA, false));
	TArray<FMantleEntity> Solid;
	Solid.Add(MeshEntity(TEXT("ent_s"), FlowMeshA, true));
	Fixture.Assets->AcquireEntityAssets(20, Picture);
	if (!TestTrue(TEXT("the picture cell's mesh arrives"), Fixture.Settle(20)))
	{
		return false;
	}
	UStaticMesh* Before = FMantlePlaceAssets::FindLiveMesh(FlowMeshA, Error);
	if (!TestNotNull(TEXT("A is built"), Before))
	{
		return false;
	}
	TestFalse(TEXT("without collision for the picture"), Before->GetBodySetup() && Before->GetBodySetup()->CollisionTraceFlag == CTF_UseComplexAsSimple);
	TestEqual(TEXT("its Nanite file was read once"), Fixture.Assets->GetStreamedCounts().NaniteFilesBuilt, 1);
	Fixture.Assets->AcquireEntityAssets(21, Solid);
	TestFalse(TEXT("the solid cell waits: the mesh is built again with collision"), Fixture.Assets->GetHolderAssets(21).IsSettled());
	// Nothing that has the mesh already loses it while it is built again: a cell that has its assets and waits to be built from them still finds the mesh at every step.
	bool bThereAtEveryStep = true;
	if (!TestTrue(TEXT("the solid cell's mesh arrives"), Fixture.PumpUntil([&Fixture, &bThereAtEveryStep]()
		{
			FString StepError;
			bThereAtEveryStep = bThereAtEveryStep && FMantlePlaceAssets::FindLiveMesh(FlowMeshA, StepError) != nullptr
				&& FMantlePlaceAssets::GetLiveMeshState(FlowMeshA) != EMantleLiveAssetState::Loading;
			return Fixture.Assets->GetHolderAssets(21).IsSettled();
		})))
	{
		return false;
	}
	TestTrue(TEXT("the mesh was there at every step"), bThereAtEveryStep);
	UStaticMesh* After = FMantlePlaceAssets::FindLiveMesh(FlowMeshA, Error);
	if (TestNotNull(TEXT("A is built again"), After))
	{
		TestTrue(TEXT("with collision"), After->GetBodySetup() && After->GetBodySetup()->CollisionTraceFlag == CTF_UseComplexAsSimple);
		TestTrue(TEXT("as another object; the picture cell keeps the first"), After != Before);
	}
	// The second build read the Nanite file again (the first build's was freed when it finished), and nothing is held once it has.
	TestEqual(TEXT("its Nanite file was read again for the second build"), Fixture.Assets->GetStreamedCounts().NaniteFilesBuilt, 2);
	TestEqual(TEXT("and nothing of it is held"), Fixture.Assets->GetNaniteHeldBytes(), static_cast<int64>(0));
	TestEqual(TEXT("neither build was made without its Nanite file when the file was there to read"), Fixture.Assets->GetStreamedCounts().NaniteFileMisses, 0);
	TestEqual(TEXT("the picture cell is still settled"), Fixture.Assets->GetHolderAssets(20).IsSettled(), true);
	Fixture.Assets->ReleaseEntityAssets(20);
	Fixture.Assets->ReleaseEntityAssets(21);
	TestTrue(TEXT("both released: the mesh goes"), Fixture.PumpUntil([&Fixture]() { return Fixture.Assets->GetStreamedCounts().Meshes == 0; }));
	TestEqual(TEXT("and so does its Nanite record"), Fixture.Assets->GetStreamedCounts().NaniteFiles, 0);

	// A cell that wants it again with no collision at all gets it with collision from the start: a mesh some cell has needed with collision is not built without it twice.
	Fixture.Assets->AcquireEntityAssets(22, Picture);
	TestTrue(TEXT("the picture cell gets it again"), Fixture.Settle(22));
	UStaticMesh* Again = FMantlePlaceAssets::FindLiveMesh(FlowMeshA, Error);
	TestTrue(TEXT("built with collision at once"), Again && Again->GetBodySetup() && Again->GetBodySetup()->CollisionTraceFlag == CTF_UseComplexAsSimple);
	// (The object of the second build is still alive, so this one reuses it and builds nothing.)
	TestTrue(TEXT("it is the object of the second build, with collision: not built a third time"), Again == After);
	TestTrue(TEXT("and no mesh is waiting for an upgrade"), Fixture.Assets->GetStreamedCounts().DropsWaiting == 0);
	Fixture.Assets->ReleaseEntityAssets(22);
	Fixture.PumpUntil([&Fixture]() { return Fixture.Assets->GetStreamedCounts().Meshes == 0; });

	// A mesh that cannot be had: the holder is told which, once its build is over; nothing waits for ever.
	TArray<FMantleEntity> Broken;
	Broken.Add(MeshEntity(TEXT("ent_f"), MeshF, true));
	Broken.Add(MeshEntity(TEXT("ent_b"), FlowMeshB, false));
	Fixture.Assets->AcquireEntityAssets(30, Broken);
	if (!TestTrue(TEXT("the holder settles"), Fixture.Settle(30)))
	{
		return false;
	}
	const FMantlePlaceAssets::FHolderAssets Assets = Fixture.Assets->GetHolderAssets(30);
	TestEqual(TEXT("one asset failed"), Assets.FailedIds.Num(), 1);
	TestTrue(TEXT("it is the mesh that was refused"), Assets.FailedIds.Contains(MeshF));
	TestNotNull(TEXT("the other mesh is built"), FMantlePlaceAssets::FindLiveMesh(FlowMeshB, Error));
	// Its release drops the failed record too: the next cell that wants it tries again.
	Fixture.Assets->ReleaseEntityAssets(30);
	TestTrue(TEXT("the failed record is dropped"), Fixture.PumpUntil([&Fixture]() { return Fixture.Assets->GetStreamedCounts().Meshes == 0; }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleStreamedAssetsSharedBuildTest,
	"Mantle.Runtime.Place.StreamedAssets.SharedBuildLeavesNoSideFiles",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleStreamedAssetsSharedBuildTest::RunTest(const FString& Parameters)
{
	using namespace MantleStreamedAssetsTestPrivate;
	FStreamingFixture Fixture(TEXT("StreamedAssets") / FGuid::NewGuid().ToString(), /*bNanite*/ true);
	if (!TestTrue(TEXT("the root completes"), Fixture.PumpUntil([&Fixture]() { return Fixture.Assets->IsComplete(); })))
	{
		return false;
	}
	// A and A2 are two ids of one file: one mesh build between them, which names the Nanite file of the id that made it, while A2 has a Nanite file of its own.
	TArray<FMantleEntity> CellA;
	CellA.Add(MeshEntity(TEXT("ent_a"), FlowMeshA, true));
	TArray<FMantleEntity> CellA2;
	CellA2.Add(MeshEntity(TEXT("ent_a2"), MeshA2, true));
	Fixture.Assets->AcquireEntityAssets(70, CellA);
	Fixture.Assets->AcquireEntityAssets(71, CellA2);
	if (!TestTrue(TEXT("both cells' meshes arrive"), Fixture.Settle(70) && Fixture.Settle(71)))
	{
		return false;
	}
	TestEqual(TEXT("two Nanite files are held as records"), Fixture.Assets->GetStreamedCounts().NaniteFiles, 2);
	// The cell of A goes first, then the cell of A2: the build goes with the second, and so must both files.
	Fixture.Assets->ReleaseEntityAssets(70);
	Fixture.Assets->ReleaseEntityAssets(71);
	TestTrue(TEXT("both meshes go"), Fixture.PumpUntil([&Fixture]() { return Fixture.Assets->GetStreamedCounts().Meshes == 0; }));
	// (A Nanite file still being fetched when its mesh goes is dropped as soon as the fetch ends.)
	TestTrue(TEXT("and no Nanite record is left behind (its file is long freed: a mesh made again would find it empty)"),
		Fixture.PumpUntil([&Fixture]() { return Fixture.Assets->GetStreamedCounts().NaniteFiles == 0; }));
	AddInfo(TEXT("records left: ") + Fixture.Assets->DescribeStreamedRecords());
	// Wanted again: the file is read again and the mesh is not built without it.
	Fixture.Assets->AcquireEntityAssets(72, CellA);
	TestTrue(TEXT("a later cell gets A again"), Fixture.Settle(72));
	TestEqual(TEXT("no mesh was built without its Nanite file"), Fixture.Assets->GetStreamedCounts().NaniteFileMisses, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleStreamedAssetsUpgradeWhileNaniteInFlightTest,
	"Mantle.Runtime.Place.StreamedAssets.UpgradeWhileNaniteInFlight",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleStreamedAssetsUpgradeWhileNaniteInFlightTest::RunTest(const FString& Parameters)
{
	using namespace MantleStreamedAssetsTestPrivate;
	FStreamingFixture Fixture(TEXT("StreamedAssets") / FGuid::NewGuid().ToString(), /*bNanite*/ true);
	if (!TestTrue(TEXT("the root completes"), Fixture.PumpUntil([&Fixture]() { return Fixture.Assets->IsComplete(); })))
	{
		return false;
	}
	TArray<FMantleEntity> Picture;
	Picture.Add(MeshEntity(TEXT("ent_p"), FlowMeshA, false));
	TArray<FMantleEntity> Solid;
	Solid.Add(MeshEntity(TEXT("ent_s"), FlowMeshA, true));
	// A is built without collision and let go; its object is still alive (nothing collected it), so the next cell that wants it takes it as it is while its Nanite file is still on its way.
	Fixture.Assets->AcquireEntityAssets(80, Picture);
	if (!TestTrue(TEXT("the first cell's mesh arrives"), Fixture.Settle(80)))
	{
		return false;
	}
	Fixture.Assets->ReleaseEntityAssets(80);
	TestTrue(TEXT("and goes with its Nanite record"), Fixture.PumpUntil([&Fixture]() { return Fixture.Assets->GetStreamedCounts().Meshes == 0 && Fixture.Assets->GetStreamedCounts().NaniteFiles == 0; }));
	Fixture.Assets->AcquireEntityAssets(81, Picture);
	if (!TestTrue(TEXT("a cell that wants it again has it as soon as it is resolved (the object is alive)"), Fixture.Settle(81)))
	{
		return false;
	}
	// A solid cell asks for it while the Nanite file of the mesh is still being fetched: the mesh is built again with collision, and that build needs the file that is on its way.
	TestTrue(TEXT("(the Nanite file is still in flight)"), Fixture.Assets->GetStreamedCounts().NaniteInFlight > 0);
	Fixture.Assets->AcquireEntityAssets(82, Solid);
	TestTrue(TEXT("the solid cell's mesh arrives"), Fixture.Settle(82));
	TestEqual(TEXT("the second build had the file it waited for"), Fixture.Assets->GetStreamedCounts().NaniteFileMisses, 0);
	TestEqual(TEXT("and nothing waits to be dropped"), Fixture.Assets->GetStreamedCounts().DropsWaiting, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleStreamedAssetsUpgradeEndsWithTheMeshTest,
	"Mantle.Runtime.Place.StreamedAssets.UpgradeEndsWithTheMesh",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleStreamedAssetsUpgradeEndsWithTheMeshTest::RunTest(const FString& Parameters)
{
	using namespace MantleStreamedAssetsTestPrivate;
	// Meshes the tests before this one built are collected: a mesh with collision that is still alive would be taken as it is, and the rebuild would be over before it could be seen.
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	FStreamingFixture Fixture(TEXT("StreamedAssets") / FGuid::NewGuid().ToString(), /*bNanite*/ true);
	if (!TestTrue(TEXT("the root completes"), Fixture.PumpUntil([&Fixture]() { return Fixture.Assets->IsComplete(); })))
	{
		return false;
	}
	// A and A2 (one build between them, two Nanite files) are pictures first; then a solid cell wants both, so both are built again with collision (one build for both), and every cell lets go while that build is still being made.
	TArray<FMantleEntity> Pictures;
	Pictures.Add(MeshEntity(TEXT("ent_a"), FlowMeshA, false));
	Pictures.Add(MeshEntity(TEXT("ent_a2"), MeshA2, false));
	TArray<FMantleEntity> Solids;
	Solids.Add(MeshEntity(TEXT("ent_sa"), FlowMeshA, true));
	Solids.Add(MeshEntity(TEXT("ent_sa2"), MeshA2, true));
	Fixture.Assets->AcquireEntityAssets(90, Pictures);
	if (!TestTrue(TEXT("the picture cell's meshes arrive"), Fixture.Settle(90)))
	{
		return false;
	}
	Fixture.Assets->AcquireEntityAssets(91, Solids);
	if (!TestTrue(TEXT("the rebuild with collision begins"), Fixture.PumpUntil([&Fixture]() { return Fixture.Assets->GetStreamedCounts().UpgradesInFlight > 0; })))
	{
		return false;
	}
	Fixture.Assets->ReleaseEntityAssets(90);
	Fixture.Assets->ReleaseEntityAssets(91);
	TestTrue(TEXT("every mesh goes"), Fixture.PumpUntil([&Fixture]() { return Fixture.Assets->GetStreamedCounts().Meshes == 0; }));
	TestTrue(TEXT("and every Nanite record goes with them"), Fixture.PumpUntil([&Fixture]() { return Fixture.Assets->GetStreamedCounts().NaniteFiles == 0; }));
	AddInfo(TEXT("records left: ") + Fixture.Assets->DescribeStreamedRecords());
	TestEqual(TEXT("nothing waits to be dropped"), Fixture.Assets->GetStreamedCounts().DropsWaiting, 0);
	// Wanted again, the mesh is built with its Nanite file.
	Fixture.Assets->AcquireEntityAssets(92, Pictures);
	TestTrue(TEXT("a later cell gets them again"), Fixture.Settle(92));
	TestEqual(TEXT("with no build made without its Nanite file"), Fixture.Assets->GetStreamedCounts().NaniteFileMisses, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleStreamedAssetsOnDemandTest,
	"Mantle.Runtime.Place.StreamedAssets.OnDemand",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleStreamedAssetsOnDemandTest::RunTest(const FString& Parameters)
{
	using namespace MantleStreamedAssetsTestPrivate;
	FStreamingFixture Fixture(TEXT("StreamedAssets") / FGuid::NewGuid().ToString(), /*bNanite*/ false);
	if (!TestTrue(TEXT("the root completes"), Fixture.PumpUntil([&Fixture]() { return Fixture.Assets->IsComplete(); })))
	{
		return false;
	}
	// A script asks about a mesh the manifest lists and no cell has loaded: the set starts it, and the answer is "loading", then "ready".
	FString Error;
	TestEqual(TEXT("nothing holds B yet"), Fixture.Assets->GetStreamedCounts().Meshes, 0);
	TestTrue(TEXT("the first answer is loading"), FMantlePlaceAssets::GetLiveMeshState(FlowMeshB, &Error) == EMantleLiveAssetState::Loading);
	TestEqual(TEXT("and the set holds a record for it now"), Fixture.Assets->GetStreamedCounts().Meshes, 1);
	if (!TestTrue(TEXT("it becomes ready"), Fixture.PumpUntil([]() { return FMantlePlaceAssets::GetLiveMeshState(FlowMeshB) == EMantleLiveAssetState::Ready; })))
	{
		return false;
	}
	TestNotNull(TEXT("and can be had by id"), FMantlePlaceAssets::FindLiveMesh(FlowMeshB, Error));
	// A mesh the manifest does not list is not fetched: it is not prepared.
	TestTrue(TEXT("an id that is not in the manifest is not prepared"), FMantlePlaceAssets::GetLiveMeshState(TEXT("ast_u00000000000000000000000000000999"), &Error) == EMantleLiveAssetState::NotPrepared);
	TestEqual(TEXT("and nothing was started for it"), Fixture.Assets->GetStreamedCounts().Meshes, 1);

	// A cell that wants B and lets go of it does not take it away: the script's demand stays for as long as the place does.
	TArray<FMantleEntity> Cell;
	Cell.Add(MeshEntity(TEXT("ent_c"), FlowMeshB, false));
	Fixture.Assets->AcquireEntityAssets(50, Cell);
	TestTrue(TEXT("the cell has it at once"), Fixture.Settle(50));
	Fixture.Assets->ReleaseEntityAssets(50);
	Fixture.PumpUntil([]() { return false; }, 0.1);
	TestEqual(TEXT("B is still held after every cell let go"), Fixture.Assets->GetStreamedCounts().Meshes, 1);
	TestTrue(TEXT("and still ready"), FMantlePlaceAssets::GetLiveMeshState(FlowMeshB) == EMantleLiveAssetState::Ready);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleStreamedAssetsFailedMaterialTest,
	"Mantle.Runtime.Place.StreamedAssets.FailedMaterialLetsGoOfItsTextures",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleStreamedAssetsFailedMaterialTest::RunTest(const FString& Parameters)
{
	using namespace MantleStreamedAssetsTestPrivate;
	AddExpectedError(TEXT("MANTLE_PLACE_ASSETS FAIL"), EAutomationExpectedErrorFlags::Contains, 0);
	FStreamingFixture Fixture(TEXT("StreamedAssets") / FGuid::NewGuid().ToString(), /*bNanite*/ false);
	if (!TestTrue(TEXT("the root completes"), Fixture.PumpUntil([&Fixture]() { return Fixture.Assets->IsComplete(); })))
	{
		return false;
	}
	// A cell uses a material whose download is refused: it fails before its document is read, so its build never names a texture. Its answer from the API named one, and that record was started.
	TArray<FMantleEntity> Cell;
	FMantleEntity Entity = MeshEntity(TEXT("ent_m"), FlowMeshB, false);
	Entity.Props.Get<FMantleStaticMeshProps>().Materials.Add(MaterialM);
	Cell.Add(Entity);
	Fixture.Assets->AcquireEntityAssets(60, Cell);
	if (!TestTrue(TEXT("the cell settles"), Fixture.PumpUntil([&Fixture]() { return Fixture.Assets->GetHolderAssets(60).IsSettled() && Fixture.Assets->GetStreamedCounts().Textures >= 1; })))
	{
		return false;
	}
	const FMantlePlaceAssets::FHolderAssets Assets = Fixture.Assets->GetHolderAssets(60);
	TestTrue(TEXT("the material failed"), Assets.FailedIds.Contains(MaterialM));
	TestEqual(TEXT("and its texture's record was started"), Fixture.Assets->GetStreamedCounts().Textures, 1);
	Fixture.Assets->ReleaseEntityAssets(60);
	TestTrue(TEXT("the last release drops the failed material and the texture it started"),
		Fixture.PumpUntil([&Fixture]() { const FMantlePlaceAssets::FStreamedCounts Counts = Fixture.Assets->GetStreamedCounts(); return Counts.Materials == 0 && Counts.Textures == 0 && Counts.Meshes == 0; }));
	TestEqual(TEXT("nothing waits to be dropped"), Fixture.Assets->GetStreamedCounts().DropsWaiting, 0);
	return true;
}

// ---- S3-A3: the per-frame Nanite limit, the root-page budget, MND1 version 2 and the page source ------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleNaniteFrameLimitTest,
	"Mantle.Runtime.StaticMesh.NaniteFrameLimit",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleNaniteFrameLimitTest::RunTest(const FString& Parameters)
{
	constexpr int64 MiB = 1024 * 1024;
	TestEqual(TEXT("the limit is 8 MiB"), FMantleNaniteFrameLimit::BytesPerFrame, 8 * MiB);

	// A frame's queue of meshes (their Nanite root bytes, in MiB, in queue order) answered one by one as RunBuilds does: Taken adds up what was built, and once one
	// had to wait no later one with Nanite data goes ahead of it.
	const auto Frame = [](const TArray<double>& SizesMiB)
	{
		TArray<bool> Built;
		int64 Taken = 0;
		bool bWaiting = false;
		for (const double Size : SizesMiB)
		{
			const int64 Bytes = static_cast<int64>(Size * 1048576.0);
			const bool bBuild = FMantleNaniteFrameLimit::MayBuild(Taken, Bytes, bWaiting);
			Built.Add(bBuild);
			if (bBuild)
			{
				Taken += Bytes;
			}
			else
			{
				bWaiting = true;
			}
		}
		return Built;
	};
	TestEqual(TEXT("3, 3, 3: 3 + 3 = 6 fits, 6 + 3 = 9 does not"), Frame({ 3, 3, 3 }), TArray<bool>({ true, true, false }));
	TestEqual(TEXT("4, 4, 1: 4 + 4 = 8 fits exactly, 8 + 1 does not"), Frame({ 4, 4, 1 }), TArray<bool>({ true, true, false }));
	TestEqual(TEXT("10 alone: over the limit, a frame to itself"), Frame({ 10 }), TArray<bool>({ true }));
	TestEqual(TEXT("2, 10: 2 + 10 does not fit"), Frame({ 2, 10 }), TArray<bool>({ true, false }));
	TestEqual(TEXT("10, 1: after the big one nothing else with Nanite data"), Frame({ 10, 1 }), TArray<bool>({ true, false }));
	TestEqual(TEXT("2, 10, 1: the 1 would fit (2 + 1) but the 10 waits ahead of it: order kept"), Frame({ 2, 10, 1 }), TArray<bool>({ true, false, false }));
	TestEqual(TEXT("0, 5, 0, 5: no Nanite data always builds; 5 + 5 = 10 does not fit"), Frame({ 0, 5, 0, 5 }), TArray<bool>({ true, true, true, false }));
	TestEqual(TEXT("0 after a waiting one still builds (it registers nothing)"), Frame({ 3, 6, 0 }), TArray<bool>({ true, false, true }));
	TestEqual(TEXT("8 alone is exactly the limit"), Frame({ 8 }), TArray<bool>({ true }));
	TestEqual(TEXT("8, 0: a full frame still builds what registers nothing"), Frame({ 8, 0 }), TArray<bool>({ true, true }));
	TestEqual(TEXT("an empty queue builds nothing"), Frame({}), TArray<bool>());

	TestTrue(TEXT("a frame that took nothing builds 100 MiB"), FMantleNaniteFrameLimit::MayBuild(0, 100 * MiB, false));
	TestFalse(TEXT("1 MiB taken: 8 MiB more does not fit"), FMantleNaniteFrameLimit::MayBuild(1 * MiB, 8 * MiB, false));
	TestFalse(TEXT("an earlier one waited: even a 5 byte mesh waits"), FMantleNaniteFrameLimit::MayBuild(0, 5, true));
	TestTrue(TEXT("an earlier one waited: a mesh with no Nanite data does not"), FMantleNaniteFrameLimit::MayBuild(0, 0, true));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleNaniteRootPageBudgetTest,
	"Mantle.Runtime.StaticMesh.NaniteRootPageBudget",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleNaniteRootPageBudgetTest::RunTest(const FString& Parameters)
{
	using namespace MantleNaniteSideFileTestPrivate;

	// (2048 MB - the 512 MB streaming pool) / 32 KB root pages = 1536 x 32 = 49,152
	TestEqual(TEXT("the engine's ceiling with our pool"), FMantleNaniteRootPages::EngineCeilingPages, static_cast<int64>(49152));
	TestEqual(TEXT("the budget is 36 x 1024 pages, three quarters of the ceiling"), FMantleNaniteRootPages::BudgetPages, static_cast<int64>(36864));
	TestEqual(TEXT("exactly three quarters"), FMantleNaniteRootPages::BudgetPages * 4, FMantleNaniteRootPages::EngineCeilingPages * 3);
	TestTrue(TEXT("Medieval's 26,958 version 1 pages fit"), FMantleNaniteRootPages::Allows(0, 26958));
	TestTrue(TEXT("a whole budget fits in an empty set"), FMantleNaniteRootPages::Allows(0, 36864));
	TestFalse(TEXT("one over does not"), FMantleNaniteRootPages::Allows(0, 36865));
	TestTrue(TEXT("34,000 + 2,863 = 36,863 fits"), FMantleNaniteRootPages::Allows(34000, 2863));
	TestFalse(TEXT("34,100 + 2,863 = 36,963 does not"), FMantleNaniteRootPages::Allows(34100, 2863));
	TestTrue(TEXT("a full set takes a mesh of no pages"), FMantleNaniteRootPages::Allows(36864, 0));

	// The builder: the file has 1 root page. Room for 0 leaves the mesh plain; room for 1 attaches it.
	const TArray<uint8> File = WriteFile(MeshTag, { 0, 1 });
	FMantleStaticMeshBuildParams Params;
	Params.bInitRenderResources = false;
	Params.NaniteFile = &File;
	Params.NaniteTable = MakeShared<const FMantleNaniteTable, ESPMode::ThreadSafe>(TableOf(File));
	Params.NaniteMeshTag = MeshTag;
	Params.NaniteRootPagesLeft = 0;
	TStrongObjectPtr<UStaticMesh> Plain(FMantleRuntimeStaticMeshBuilder::Build(MakeTwoSlotBox(), Params));
	if (TestNotNull(TEXT("a mesh over the budget still builds"), Plain.Get()))
	{
		TestFalse(TEXT("without Nanite data"), Plain->HasValidNaniteData());
		int32 Pages = -1;
		int64 Bytes = -1;
		FMantleRuntimeStaticMeshBuilder::GetNaniteAttached(*Plain, Pages, Bytes);
		TestEqual(TEXT("it attached 0 root pages"), Pages, 0);
	}
	Params.NaniteRootPagesLeft = 1;
	TStrongObjectPtr<UStaticMesh> Drawn(FMantleRuntimeStaticMeshBuilder::Build(MakeTwoSlotBox(), Params));
	if (TestNotNull(TEXT("a mesh with room builds"), Drawn.Get()))
	{
		TestTrue(TEXT("with Nanite data"), Drawn->HasValidNaniteData());
		int32 Pages = -1;
		int64 Bytes = -1;
		FMantleRuntimeStaticMeshBuilder::GetNaniteAttached(*Drawn, Pages, Bytes);
		TestEqual(TEXT("its 1 root page"), Pages, 1);
		TestEqual(TEXT("its 37 root bytes"), Bytes, static_cast<int64>(37));
	}
	return true;
}

namespace MantleNaniteSideFileTestPrivate
{
	Nanite::FPageStreamingState MakePageState(const uint32 BulkOffset, const uint32 BulkSize, const uint32 PageSize)
	{
		Nanite::FPageStreamingState Page;
		FMemory::Memzero(Page);
		Page.BulkOffset = BulkOffset;
		Page.BulkSize = BulkSize;
		Page.PageSize = PageSize;
		return Page;
	}

	/** Page p of the version 2 test files: n bytes, byte i = (i * 5 + 1 + p * 17) mod 251. */
	TArray<uint8> MakePage(const int32 PageIndex, const int32 Size)
	{
		TArray<uint8> Page;
		for (int32 Index = 0; Index < Size; ++Index)
		{
			Page.Add(static_cast<uint8>((Index * 5 + 1 + PageIndex * 17) % 251));
		}
		return Page;
	}

	/** A version 2 file's head and the pieces of its streamed pages. */
	struct FStreamedFile
	{
		TArray<uint8> Head;
		TArray<TArray<uint8>> Pages;
	};

	/**
	 * The good version 2 file: 37 bytes of root data in 1 root page (0, 37, page 30) and two streamed pages, 40 and 24 bytes: (0, 40, page 32) and (40, 24, page 16).
	 * Edit changes the resources and the pages before they are written.
	 */
	FStreamedFile WriteStreamedFile(const uint32 Version = 2, TFunctionRef<void(Nanite::FResources&, TArray<TArray<uint8>>&)> Edit = [](Nanite::FResources&, TArray<TArray<uint8>>&) {}, const int32 TailBytes = 0)
	{
		Nanite::FResources Resources = MakeResources();
		Resources.PageStreamingStates.Reset();
		Resources.PageStreamingStates.Add(MakePageState(0, 37, 30));
		Resources.PageStreamingStates.Add(MakePageState(0, 40, 32));
		Resources.PageStreamingStates.Add(MakePageState(40, 24, 16));
		Resources.NumRootPages = 1;
		FStreamedFile File;
		File.Pages = { MakePage(0, 40), MakePage(1, 24) };
		Edit(Resources, File.Pages);
		File.Head = WriteFileEx(MeshTag, { 0, 1 }, Version, MoveTemp(Resources), TailBytes);
		return File;
	}

	/** A file on disk for the page source: a head with these streamed pages' sizes (one root page, then each page whole), the pages, and where it is. */
	struct FPageFile
	{
		FString Path;
		TArray<uint8> Head;
		TArray<TArray<uint8>> Pages;
		TArray<uint8> Bytes;
		TSharedPtr<const FMantleNaniteTable, ESPMode::ThreadSafe> Table;
		/** The start of each page in the streamed block. */
		TArray<uint64> Starts;
	};

	FPageFile MakePageFile(const TArray<int32>& PageSizes, const int32 Seed = 0)
	{
		FPageFile File;
		Nanite::FResources Resources = MakeResources();
		Resources.PageStreamingStates.Reset();
		Resources.PageStreamingStates.Add(MakePageState(0, 37, 30));
		uint32 Start = 0;
		for (int32 Index = 0; Index < PageSizes.Num(); ++Index)
		{
			File.Pages.Add(MakePage(Seed * 31 + Index, PageSizes[Index]));
			File.Starts.Add(Start);
			Resources.PageStreamingStates.Add(MakePageState(Start, PageSizes[Index], FMath::Min(PageSizes[Index], 16)));
			Start += PageSizes[Index];
		}
		Resources.NumRootPages = 1;
		File.Head = WriteFileEx(MeshTag, { 0, 1 }, 2, MoveTemp(Resources));
		File.Bytes = MakeContainer(File.Head, File.Pages);
		File.Path = WriteTempFile(File.Bytes);
		File.Table = MakeShared<const FMantleNaniteTable, ESPMode::ThreadSafe>(TableOf(File.Head, File.Pages));
		return File;
	}

	UStaticMesh* NewTransientMesh()
	{
		return NewObject<UStaticMesh>(GetTransientPackage(), NAME_None, RF_Transient);
	}

	/** The engine's own request: a read of (chunk, offset, size) through its I/O dispatcher into a buffer of ours (the target, as the engine's staging ring is, filled with 0xCC first). */
	class FDispatcherRead
	{
	public:
		FDispatcherRead(const FIoChunkId& Chunk, const uint64 Offset, const uint64 Size)
			: Batch(FIoDispatcher::Get().NewBatch())
			, Done(FPlatformProcess::GetSynchEventFromPool(true))
		{
			Target.Init(0xCC, static_cast<int32>(Size));
			FIoReadOptions Options(Offset, Size);
			Options.SetTargetVa(Target.GetData());
			Request = Batch.Read(Chunk, Options, IoDispatcherPriority_Medium);
			Batch.IssueAndTriggerEvent(Done);
		}

		/** The target is ours: nothing may write into it after this object is gone. */
		~FDispatcherRead()
		{
			Wait(30.0);
			FPlatformProcess::ReturnSynchEventToPool(Done);
		}

		bool Wait(const double Seconds = 10.0)
		{
			if (!bDone)
			{
				bDone = Done->Wait(FTimespan::FromSeconds(Seconds));
			}
			return bDone;
		}

		bool Succeeded() { return Wait() && Request.Status().IsOk(); }
		EIoErrorCode ErrorCode() { return Wait() ? Request.Status().GetErrorCode() : EIoErrorCode::Unknown; }
		bool TargetUntouched() const
		{
			for (const uint8 Byte : Target)
			{
				if (Byte != 0xCC)
				{
					return false;
				}
			}
			return true;
		}

		FIoBatch Batch;
		FIoRequest Request;
		FEvent* Done = nullptr;
		bool bDone = false;
		TArray<uint8> Target;
	};

	/** One read of a page, whole: whether it was answered and, if so, whether the target holds exactly Expected. */
	bool ReadPageEquals(const FIoChunkId& Chunk, const uint64 Start, const TArray<uint8>& Expected)
	{
		FDispatcherRead Read(Chunk, Start, Expected.Num());
		return Read.Succeeded() && Read.Target == Expected;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleNaniteVersion2ReadTest,
	"Mantle.Runtime.StaticMesh.NaniteVersion2",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleNaniteVersion2ReadTest::RunTest(const FString& Parameters)
{
	using namespace MantleNaniteSideFileTestPrivate;

	// The good version 2 file.
	{
		const FStreamedFile File = WriteStreamedFile(2, [](Nanite::FResources& Resources, TArray<TArray<uint8>>&) { Resources.ResourceFlags = 0x7; });
		Nanite::FResources Read;
		TArray<int32> Slots;
		FString Reason;
		FMantleNaniteStreamedBlock Streamed;
		if (!TestTrue(TEXT("a version 2 file with streamed pages reads"), ReadHeadBytes(File.Head, MeshTag, Read, Slots, &Reason, File.Pages, &Streamed)))
		{
			AddInfo(Reason);
			return false;
		}
		TestEqual(TEXT("three page states"), Read.PageStreamingStates.Num(), 3);
		TestEqual(TEXT("one root page"), Read.NumRootPages, 1u);
		TestTrue(TEXT("the engine sees streaming data"), Read.HasStreamingData());
		TestEqual(TEXT("root data of 37 bytes"), Read.RootData.Num(), 37);
		TestEqual(TEXT("the flag for the editor's derived-data cache is cleared (0x7 written, 0x6 read)"), Read.ResourceFlags, 0x6u);
		TestEqual(TEXT("the streamed block is the pages' 64 bytes"), Streamed.Size, static_cast<int64>(64));

		// A reader that cannot serve streamed pages refuses a file that has them.
		Nanite::FResources Refused;
		TestFalse(TEXT("asked for no streamed block, the file is refused"),
			FMantleNaniteSideFile::Read(File.Head, MeshTag, TableOf(File.Head, File.Pages), MAX_int64, Refused, Slots, &Reason));
		TestTrue(TEXT("with the reason"), Reason.Contains(TEXT("streamed")));

		int64 RootBytes = 0;
		TestTrue(TEXT("the header peek reads the root byte count"), FMantleNaniteSideFile::PeekRootBytes(File.Head, RootBytes));
		TestEqual(TEXT("37 root bytes, not the file's size"), RootBytes, static_cast<int64>(37));
	}

	// A version 2 file with nothing streamed is its head alone (one piece). Version 1 is no longer read.
	{
		const auto OneRootPage = []()
		{
			// A page table version 2 accepts: the one root page is all of the 37 bytes of root data.
			Nanite::FResources Resources = MakeResources();
			Resources.PageStreamingStates.Reset();
			Resources.PageStreamingStates.Add(MakePageState(0, 37, 30));
			return Resources;
		};
		const TArray<uint8> V1 = WriteFileEx(MeshTag, { 0, 1 }, 1, OneRootPage());
		const TArray<uint8> V2 = WriteFileEx(MeshTag, { 0, 1 }, 2, OneRootPage());
		Nanite::FResources A;
		Nanite::FResources B;
		TArray<int32> Slots;
		FString Reason;
		TestFalse(TEXT("version 1 is refused"), ReadHeadBytes(V1, MeshTag, A, Slots, &Reason));
		TestTrue(TEXT("with the version in the reason"), Reason.Contains(TEXT("version 1")));
		FMantleNaniteStreamedBlock Streamed;
		TestTrue(TEXT("version 2 with no streaming reads"), ReadHeadBytes(V2, MeshTag, B, Slots, &Reason, {}, &Streamed));
		TestEqual(TEXT("no streamed block"), Streamed.Size, static_cast<int64>(0));
		TestFalse(TEXT("and no streaming data"), B.HasStreamingData());
		TestTrue(TEXT("it reads without being asked for a block either"), FMantleNaniteSideFile::Read(V2, MeshTag, TableOf(V2), MAX_int64, B, Slots, &Reason));
	}

	// Refused, one change at a time (the contract's checks).
	using FEditFn = TFunction<void(Nanite::FResources&, TArray<TArray<uint8>>&)>;
	const auto Refuses = [this](const TCHAR* What, const FStreamedFile& File, const int64 MaxPageBytes = MAX_int64)
	{
		Nanite::FResources Read;
		TArray<int32> Slots;
		FMantleNaniteStreamedBlock Streamed;
		FString Reason;
		TestFalse(What, ReadHeadBytes(File.Head, MeshTag, Read, Slots, &Reason, File.Pages, &Streamed, MaxPageBytes));
	};
	const auto RefusesEdit = [&](const TCHAR* What, FEditFn Edit) { Refuses(What, WriteStreamedFile(2, Edit)); };
	RefusesEdit(TEXT("a gap between the streamed pages (second at 41)"), [](Nanite::FResources& R, TArray<TArray<uint8>>&) { R.PageStreamingStates[2].BulkOffset = 41; });
	RefusesEdit(TEXT("a page the table says is 25 bytes where its piece is 24"), [](Nanite::FResources& R, TArray<TArray<uint8>>&) { R.PageStreamingStates[2].BulkSize = 25; });
	RefusesEdit(TEXT("a page the table says is 23 bytes where its piece is 24"), [](Nanite::FResources& R, TArray<TArray<uint8>>&) { R.PageStreamingStates[2].BulkSize = 23; R.PageStreamingStates[2].PageSize = 16; });
	RefusesEdit(TEXT("a streamed page smaller than its own page (40 bytes, page 41)"), [](Nanite::FResources& R, TArray<TArray<uint8>>&) { R.PageStreamingStates[1].PageSize = 41; });
	RefusesEdit(TEXT("a root page that does not end where the root data does (36 of 37)"), [](Nanite::FResources& R, TArray<TArray<uint8>>&) { R.PageStreamingStates[0].BulkSize = 36; });
	RefusesEdit(TEXT("a streamed page whose offset and size wrap 32 bits (offset 0xFFFFFFFF, 2 bytes)"), [](Nanite::FResources& R, TArray<TArray<uint8>>&)
	{
		R.PageStreamingStates[2].BulkOffset = 0xFFFFFFFFu;
		R.PageStreamingStates[2].BulkSize = 2;
		R.PageStreamingStates[2].PageSize = 1;
	});
	RefusesEdit(TEXT("more root pages than page states (3 against 3 states is fine; 4 is not)"), [](Nanite::FResources& R, TArray<TArray<uint8>>&) { R.NumRootPages = 4; });
	RefusesEdit(TEXT("no root pages"), [](Nanite::FResources& R, TArray<TArray<uint8>>&) { R.NumRootPages = 0; });
	RefusesEdit(TEXT("a page table with two streamed pages and a container with none"), [](Nanite::FResources&, TArray<TArray<uint8>>& Pages) { Pages.Reset(); });
	RefusesEdit(TEXT("a container with one page fewer than the page table has"), [](Nanite::FResources&, TArray<TArray<uint8>>& Pages) { Pages.Pop(); });
	RefusesEdit(TEXT("a container with one page more than the page table has"), [](Nanite::FResources&, TArray<TArray<uint8>>& Pages) { Pages.Add(MakePage(2, 8)); });
	RefusesEdit(TEXT("a page whose piece is one byte longer than its page says"), [](Nanite::FResources&, TArray<TArray<uint8>>& Pages) { Pages[1].Add(0); });
	Refuses(TEXT("one byte after field 25"), WriteStreamedFile(2, [](Nanite::FResources&, TArray<TArray<uint8>>&) {}, 1));
	Refuses(TEXT("version 3"), WriteStreamedFile(3));
	Refuses(TEXT("version 0"), WriteStreamedFile(0));
	Refuses(TEXT("version 1"), WriteStreamedFile(1));
	{
		// The page limit: the engine's staging ring can never hold a page bigger than it. The biggest page here is 40 bytes.
		const FStreamedFile File = WriteStreamedFile();
		Refuses(TEXT("a page over the limit (40 bytes, limit 39)"), File, 39);
		Nanite::FResources Read;
		TArray<int32> Slots;
		FString Reason;
		FMantleNaniteStreamedBlock Streamed;
		TestTrue(TEXT("a page at the limit (40 bytes, limit 40) is read, asked for the block"), ReadHeadBytes(File.Head, MeshTag, Read, Slots, &Reason, File.Pages, &Streamed, 40));
		// Without streamed pages only the root page counts, and a root page is no streamed page: no limit applies.
		const TArray<uint8> V2 = WriteFile(MeshTag, { 0, 1 });
		TestTrue(TEXT("a mesh with no streamed pages has no page to limit"), ReadHeadBytes(V2, MeshTag, Read, Slots, &Reason, {}, nullptr, 1));
	}
	{
		// The limit is the staging ring's guaranteed half, less one: r.Nanite.Streaming.MaxPendingPages x 196,608 / 2 - 1.
		int64 Pending = 128;
		if (const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Nanite.Streaming.MaxPendingPages")))
		{
			Pending = Variable->GetInt();
		}
		TestEqual(TEXT("the limit is half the ring less one"), FMantleNanitePageSource::MaxStreamedPageBytes(), Pending * 196608 / 2 - 1);
		if (Pending == 128)
		{
			TestEqual(TEXT("which is 12,582,911 at the default 128 pages"), FMantleNanitePageSource::MaxStreamedPageBytes(), static_cast<int64>(12582911));
		}
	}
	{
		// Version 2, root pages only, and bytes after field 25.
		Nanite::FResources Resources = MakeResources();
		Resources.PageStreamingStates.Reset();
		Resources.PageStreamingStates.Add(MakePageState(0, 37, 30));
		FStreamedFile File;
		File.Head = WriteFileEx(MeshTag, { 0, 1 }, 2, MoveTemp(Resources), 3);
		Refuses(TEXT("version 2, every page a root page, and bytes after field 25"), File);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleNanitePageSourceTest,
	"Mantle.Runtime.StaticMesh.NanitePageSource",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleNanitePageSourceTest::RunTest(const FString& Parameters)
{
	using namespace MantleNaniteSideFileTestPrivate;

	// Pages of 40, 24 and 5,000 bytes in a file on disk; the source reads them from it through the engine's I/O dispatcher.
	const FPageFile File = MakePageFile({ 40, 24, 5000 });
	FMantleNanitePageSource& Source = FMantleNanitePageSource::Get();
	Source.ResetStats();
	const int32 StoresBefore = Source.GetStats().Stores;

	TStrongObjectPtr<UStaticMesh> Mesh(NewTransientMesh());
	const FIoChunkId Chunk = Source.Register(File.Path, File.Table.ToSharedRef(), Mesh.Get());
	if (!TestTrue(TEXT("the store is registered under a chunk of ours"), FMantleNanitePageSource::IsMantleChunk(Chunk)))
	{
		return false;
	}
	TestEqual(TEXT("one more store"), Source.GetStats().Stores, StoresBefore + 1);
	TestEqual(TEXT("its streamed block is the pages' 5,064 bytes"), Source.GetStoreSize(Chunk), static_cast<int64>(5064));

	// Each page, as the engine asks for it: the page's own offset and size. The first, the middle and the last.
	TestTrue(TEXT("the first page is the bytes written"), ReadPageEquals(Chunk, File.Starts[0], File.Pages[0]));
	TestTrue(TEXT("the last page is the bytes written"), ReadPageEquals(Chunk, File.Starts[2], File.Pages[2]));
	TestTrue(TEXT("the middle page is the bytes written"), ReadPageEquals(Chunk, File.Starts[1], File.Pages[1]));
	// Again, and in another order: nothing is kept between reads, so every read stands alone.
	TestTrue(TEXT("the last page again"), ReadPageEquals(Chunk, File.Starts[2], File.Pages[2]));
	TestTrue(TEXT("the first page again"), ReadPageEquals(Chunk, File.Starts[0], File.Pages[0]));

	// Anything that is not exactly one page is refused: a part of a page, a page and a half, two pages, a read to the end, a read past it.
	{
		FDispatcherRead Part(Chunk, 5, 10);
		TestFalse(TEXT("a read inside a page is refused"), Part.Succeeded());
		FDispatcherRead Wrong(Chunk, File.Starts[1], 23);
		TestFalse(TEXT("a page's start with a shorter size is refused"), Wrong.Succeeded());
		FDispatcherRead Two(Chunk, File.Starts[0], 64);
		TestFalse(TEXT("two pages in one read are refused"), Two.Succeeded());
		FDispatcherRead Past(Chunk, 5060, 20);
		TestFalse(TEXT("a read past the end is refused"), Past.Succeeded());
		TestTrue(TEXT("a refused read leaves the engine's buffer alone"), Part.TargetUntouched() && Past.TargetUntouched());
	}

	const FMantleNanitePageStats Stats = Source.GetStats();
	TestEqual(TEXT("five pages served"), Stats.PagesServed, static_cast<int64>(5));
	TestEqual(TEXT("the bytes served are the pages' (40 + 5,000 + 24 + 5,000 + 40)"), Stats.BytesServed, static_cast<int64>(10104));
	TestEqual(TEXT("four reads refused"), Stats.Refused, static_cast<int64>(4));
	TestEqual(TEXT("no file trouble"), Stats.FileUnreadable, static_cast<int64>(0));
	TestEqual(TEXT("no inflate trouble"), Stats.InflateFailed, static_cast<int64>(0));
	TestEqual(TEXT("it holds no page bytes"), Stats.PageBytesHeld, static_cast<int64>(0));
	TestTrue(TEXT("only its table (16 bytes a piece, plus a few) is held, a few hundred bytes"), Stats.TableBytes > 0 && Stats.TableBytes < 1024);
	TestTrue(TEXT("its file is open"), Stats.OpenFiles >= 1);
	TestTrue(TEXT("the answer times are measured"), Stats.WorstAnswerMs > 0.0 && Stats.MeanAnswerMs > 0.0 && Stats.WorstAnswerMs >= Stats.MeanAnswerMs);

	// The mesh goes: its store goes with it, and a read of its chunk is refused.
	Mesh.Reset();
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	TestEqual(TEXT("a prune drops the store of the collected mesh"), Source.Prune(), 1);
	TestEqual(TEXT("its size is unknown now"), Source.GetStoreSize(Chunk), static_cast<int64>(-1));
	TestEqual(TEXT("the stores are back to what they were"), Source.GetStats().Stores, StoresBefore);
	TestFalse(TEXT("a read of the dropped chunk fails"), ReadPageEquals(Chunk, File.Starts[0], File.Pages[0]));

	// A table with no streamed page holds nothing.
	TStrongObjectPtr<UStaticMesh> Other(NewTransientMesh());
	const FPageFile HeadOnly = MakePageFile({});
	TestFalse(TEXT("a file with no streamed pages makes no store"), FMantleNanitePageSource::IsMantleChunk(Source.Register(HeadOnly.Path, HeadOnly.Table.ToSharedRef(), Other.Get())));
	TestFalse(TEXT("a store with no path is not made"), FMantleNanitePageSource::IsMantleChunk(Source.Register(FString(), File.Table.ToSharedRef(), Other.Get())));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleNanitePageSourceFailuresTest,
	"Mantle.Runtime.StaticMesh.NanitePageSourceFailures",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleNanitePageSourceFailuresTest::RunTest(const FString& Parameters)
{
	using namespace MantleNaniteSideFileTestPrivate;

	// What a failed read says in the log is not an error of the test.
	AddExpectedError(TEXT("MANTLE_NANITE_PAGES"), EAutomationExpectedErrorFlags::Contains, 0);

	FMantleNanitePageSource& Source = FMantleNanitePageSource::Get();
	Source.ResetStats();

	// A damaged piece fails its own read and no other: page 1's stream is damaged in the file.
	{
		FPageFile File = MakePageFile({ 300, 300, 300 }, 1);
		const FMantleNaniteTable::FPiece& Piece = File.Table->Pieces[2];
		TArray<uint8> Damaged = File.Bytes;
		Damaged[Piece.PackedStart + Piece.Packed / 2] ^= 0xFF;
		FFileHelper::SaveArrayToFile(Damaged, *File.Path);
		TStrongObjectPtr<UStaticMesh> Mesh(NewTransientMesh());
		const FIoChunkId Chunk = Source.Register(File.Path, File.Table.ToSharedRef(), Mesh.Get());
		TestTrue(TEXT("the page before the damaged one is read"), ReadPageEquals(Chunk, File.Starts[0], File.Pages[0]));
		TestFalse(TEXT("the damaged page fails"), ReadPageEquals(Chunk, File.Starts[1], File.Pages[1]));
		TestTrue(TEXT("the page after it is read"), ReadPageEquals(Chunk, File.Starts[2], File.Pages[2]));
		TestEqual(TEXT("one inflate failure was counted"), Source.GetStats().InflateFailed, static_cast<int64>(1));
		FDispatcherRead Again(Chunk, File.Starts[1], 300);
		TestFalse(TEXT("the damaged page fails again, the same way"), Again.Succeeded());
	}

	// A file that is gone, or not the size its table says, fails that mesh's reads and no other's.
	{
		const FPageFile Good = MakePageFile({ 100, 100 }, 2);
		FPageFile Gone = MakePageFile({ 100, 100 }, 3);
		FPageFile Longer = MakePageFile({ 100, 100 }, 4);
		TStrongObjectPtr<UStaticMesh> GoodMesh(NewTransientMesh());
		TStrongObjectPtr<UStaticMesh> GoneMesh(NewTransientMesh());
		TStrongObjectPtr<UStaticMesh> LongerMesh(NewTransientMesh());
		const FIoChunkId GoodChunk = Source.Register(Good.Path, Good.Table.ToSharedRef(), GoodMesh.Get());
		const FIoChunkId GoneChunk = Source.Register(Gone.Path, Gone.Table.ToSharedRef(), GoneMesh.Get());
		const FIoChunkId LongerChunk = Source.Register(Longer.Path, Longer.Table.ToSharedRef(), LongerMesh.Get());
		IFileManager::Get().Delete(*Gone.Path);
		TArray<uint8> Extra = Longer.Bytes;
		Extra.Add(0);
		FFileHelper::SaveArrayToFile(Extra, *Longer.Path);
		const int64 Before = Source.GetStats().FileUnreadable;
		TestFalse(TEXT("a read of a file that is gone fails"), ReadPageEquals(GoneChunk, Gone.Starts[0], Gone.Pages[0]));
		TestFalse(TEXT("a read of a file that is not its table's size fails"), ReadPageEquals(LongerChunk, Longer.Starts[0], Longer.Pages[0]));
		TestTrue(TEXT("a third mesh's reads are not touched"), ReadPageEquals(GoodChunk, Good.Starts[1], Good.Pages[1]));
		TestEqual(TEXT("two file failures were counted"), Source.GetStats().FileUnreadable - Before, static_cast<int64>(2));
		TestFalse(TEXT("the gone file fails again at once (never held waiting)"), ReadPageEquals(GoneChunk, Gone.Starts[1], Gone.Pages[1]));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleNanitePageSourceCancelAndPruneTest,
	"Mantle.Runtime.StaticMesh.NanitePageSourceCancelAndPrune",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleNanitePageSourceCancelAndPruneTest::RunTest(const FString& Parameters)
{
	using namespace MantleNaniteSideFileTestPrivate;

	FMantleNanitePageSource& Source = FMantleNanitePageSource::Get();
	Source.ResetStats();
	// A read task gives its hold on the store back just after it hands the request over: wait for that before counting what is left.
	const auto WaitForNoReads = [&Source]()
	{
		for (int32 Wait = 0; Wait < 100 && Source.GetStats().ReadsInFlight > 0; ++Wait)
		{
			FPlatformProcess::Sleep(0.05f);
		}
	};
	FEvent* Entered = FPlatformProcess::GetSynchEventFromPool(true);
	FEvent* Gate = FPlatformProcess::GetSynchEventFromPool(true);

	// A request cancelled while its read is held (the read task has taken it and not read yet): it still finishes, once, as cancelled, and nothing is written into the engine's buffer.
	{
		const FPageFile File = MakePageFile({ 100, 100 }, 5);
		TStrongObjectPtr<UStaticMesh> Mesh(NewTransientMesh());
		const FIoChunkId Chunk = Source.Register(File.Path, File.Table.ToSharedRef(), Mesh.Get());
		FMantleNanitePageSource::GTestBeforeRead = [Entered, Gate]() { Entered->Trigger(); Gate->Wait(); };
		{
			FDispatcherRead Read(Chunk, File.Starts[0], 100);
			TestTrue(TEXT("the read task took the request"), Entered->Wait(FTimespan::FromSeconds(10.0)));
			Read.Request.Cancel();
			// The dispatcher's thread marks it and tells the source; give it the time.
			FPlatformProcess::Sleep(0.3f);
			TestFalse(TEXT("a cancelled request is not finished while its read task still holds it"), Read.Wait(0.0));
			Gate->Trigger();
			TestTrue(TEXT("it finishes once its read task lets go"), Read.Wait());
			TestTrue(TEXT("as cancelled"), Read.ErrorCode() == EIoErrorCode::Cancelled);
			TestTrue(TEXT("and nothing was written into the engine's buffer"), Read.TargetUntouched());
		}
		FMantleNanitePageSource::GTestBeforeRead = nullptr;
		Entered->Reset();
		Gate->Reset();
		WaitForNoReads();
		TestEqual(TEXT("it was counted as cancelled"), Source.GetStats().Cancelled, static_cast<int64>(1));
		TestEqual(TEXT("and none served"), Source.GetStats().PagesServed, static_cast<int64>(0));
		TestEqual(TEXT("nothing is left in flight"), Source.GetStats().ReadsInFlight, 0);
		TestTrue(TEXT("the same chunk reads on, after"), ReadPageEquals(Chunk, File.Starts[1], File.Pages[1]));
	}

	// A mesh collected while its read is held: the read finishes cleanly, the store goes, and the file's pin is given back only after the read is over.
	{
		FMantleAssetStoreSettings StoreSettings;
		StoreSettings.RootDir = FPaths::AutomationTransientDir() / TEXT("NaniteSourcePins") / FGuid::NewGuid().ToString();
		const TSharedPtr<FMantleAssetStore> AssetStore = FMantleAssetStore::Create(StoreSettings, MakeShared<FNoFetcher>());
		const FPageFile File = MakePageFile({ 100, 100 }, 6);
		const FMantleContentHash Hash = FMantleContentHash::OfBytes(File.Bytes);
		const int32 PinsBefore = Source.GetStats().PinsHeld;
		TStrongObjectPtr<UStaticMesh> Mesh(NewTransientMesh());
		const FIoChunkId Chunk = Source.Register(File.Path, File.Table.ToSharedRef(), Mesh.Get(), AssetStore, Hash);
		TestEqual(TEXT("the store holds a pin on the file"), Source.GetStats().PinsHeld, PinsBefore + 1);
		FMantleNanitePageSource::GTestBeforeRead = [Entered, Gate]() { Entered->Trigger(); Gate->Wait(); };
		{
			FDispatcherRead Read(Chunk, File.Starts[1], 100);
			TestTrue(TEXT("the read task took the request"), Entered->Wait(FTimespan::FromSeconds(10.0)));
			Mesh.Reset();
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
			TestTrue(TEXT("the mesh's store is dropped (and the first part's, if it is not yet)"), Source.Prune() >= 1);
			TestEqual(TEXT("the pin is still held while a read runs"), Source.GetStats().PinsHeld, PinsBefore + 1);
			Gate->Trigger();
			TestTrue(TEXT("the read finishes with the right bytes"), Read.Succeeded() && Read.Target == File.Pages[1]);
		}
		FMantleNanitePageSource::GTestBeforeRead = nullptr;
		Entered->Reset();
		Gate->Reset();
		WaitForNoReads();
		Source.Prune();
		TestEqual(TEXT("the pin is given back after the read is over"), Source.GetStats().PinsHeld, PinsBefore);
	}
	FPlatformProcess::ReturnSynchEventToPool(Entered);
	FPlatformProcess::ReturnSynchEventToPool(Gate);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleNanitePageSourceOpenFilesTest,
	"Mantle.Runtime.StaticMesh.NanitePageSourceOpenFiles",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleNanitePageSourceOpenFilesTest::RunTest(const FString& Parameters)
{
	using namespace MantleNaniteSideFileTestPrivate;

	// 66 files, each read once: no more than 64 are open at a time, the least recently used are closed, and a closed one is opened again when it is read.
	FMantleNanitePageSource& Source = FMantleNanitePageSource::Get();
	Source.ResetStats();
	constexpr int32 Files = FMantleNanitePageSource::MaxOpenFiles + 2;
	TArray<FPageFile> Made;
	TArray<TStrongObjectPtr<UStaticMesh>> Meshes;
	TArray<FIoChunkId> Chunks;
	for (int32 Index = 0; Index < Files; ++Index)
	{
		Made.Add(MakePageFile({ 50, 60 }, 100 + Index));
		Meshes.Emplace(NewTransientMesh());
		Chunks.Add(Source.Register(Made[Index].Path, Made[Index].Table.ToSharedRef(), Meshes[Index].Get()));
	}
	bool bAllRead = true;
	int32 MostOpen = 0;
	for (int32 Index = 0; Index < Files; ++Index)
	{
		bAllRead &= ReadPageEquals(Chunks[Index], Made[Index].Starts[1], Made[Index].Pages[1]);
		MostOpen = FMath::Max(MostOpen, Source.GetStats().OpenFiles);
	}
	TestTrue(TEXT("every file's page is read"), bAllRead);
	TestEqual(TEXT("the most open at once is 64"), MostOpen, FMantleNanitePageSource::MaxOpenFiles);
	TestEqual(TEXT("64 stay open"), Source.GetStats().OpenFiles, FMantleNanitePageSource::MaxOpenFiles);
	TestTrue(TEXT("the first file (closed to make room) is opened again for its next read"), ReadPageEquals(Chunks[0], Made[0].Starts[0], Made[0].Pages[0]));
	TestEqual(TEXT("still no more than 64 open"), Source.GetStats().OpenFiles, FMantleNanitePageSource::MaxOpenFiles);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleNaniteStreamedAttachTest,
	"Mantle.Runtime.StaticMesh.NaniteStreamedAttach",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleNaniteStreamedAttachTest::RunTest(const FString& Parameters)
{
	using namespace MantleNaniteSideFileTestPrivate;

	// A version 2 file built into a mesh: the root pages in its Nanite data, the streamed pages read from the file on disk, named by its StreamablePages.
	const FStreamedFile File = WriteStreamedFile();
	const FString Path = WriteTempFile(MakeContainer(File.Head, File.Pages));
	FMantleStaticMeshBuildParams Params;
	Params.bInitRenderResources = false;
	Params.NaniteFile = &File.Head;
	Params.NaniteTable = MakeShared<const FMantleNaniteTable, ESPMode::ThreadSafe>(TableOf(File.Head, File.Pages));
	Params.NanitePath = Path;
	Params.NaniteMeshTag = MeshTag;
	const int32 StoresBefore = FMantleNanitePageSource::Get().GetStats().Stores;
	TStrongObjectPtr<UStaticMesh> Mesh(FMantleRuntimeStaticMeshBuilder::Build(MakeTwoSlotBox(), Params));
	if (!TestNotNull(TEXT("the mesh builds"), Mesh.Get()))
	{
		return false;
	}
	TestTrue(TEXT("it has Nanite data"), Mesh->HasValidNaniteData());
	const Nanite::FResources* Resources = Mesh->GetRenderData()->NaniteResourcesPtr.Get();
	if (TestNotNull(TEXT("the resources"), Resources))
	{
		TestEqual(TEXT("three page states"), Resources->PageStreamingStates.Num(), 3);
		TestTrue(TEXT("with streaming data"), Resources->HasStreamingData());
		TestEqual(TEXT("the streamed block is 64 bytes"), Resources->StreamablePages.GetBulkDataSize(), static_cast<int64>(64));
		TestTrue(TEXT("it is read through the I/O dispatcher"), Resources->StreamablePages.IsUsingIODispatcher());
		TestFalse(TEXT("and is not loaded into memory (the engine asks for it)"), Resources->StreamablePages.IsBulkDataLoaded());
		TestEqual(TEXT("its range starts at 0"), Resources->StreamablePages.GetBulkDataOffsetInFile(), static_cast<int64>(0));
	}
	TestEqual(TEXT("the mesh has a page store"), FMantleNanitePageSource::Get().GetStats().Stores, StoresBefore + 1);
	TestEqual(TEXT("which holds no page bytes"), FMantleNanitePageSource::Get().GetStats().PageBytesHeld, static_cast<int64>(0));
	int32 RootPages = 0;
	int64 RootBytes = 0;
	FMantleRuntimeStaticMeshBuilder::GetNaniteAttached(*Mesh, RootPages, RootBytes);
	TestEqual(TEXT("one root page counted"), RootPages, 1);
	TestEqual(TEXT("37 root bytes counted"), RootBytes, static_cast<int64>(37));

	// With no file to read the pages from, the mesh draws plain.
	Params.NanitePath.Reset();
	TStrongObjectPtr<UStaticMesh> Plain(FMantleRuntimeStaticMeshBuilder::Build(MakeTwoSlotBox(), Params));
	if (TestNotNull(TEXT("a mesh with no file path builds"), Plain.Get()))
	{
		TestFalse(TEXT("without Nanite data"), Plain->HasValidNaniteData());
	}
	// With no container table, likewise.
	Params.NanitePath = Path;
	Params.NaniteTable.Reset();
	TStrongObjectPtr<UStaticMesh> NoTable(FMantleRuntimeStaticMeshBuilder::Build(MakeTwoSlotBox(), Params));
	if (TestNotNull(TEXT("a mesh with no table builds"), NoTable.Get()))
	{
		TestFalse(TEXT("without Nanite data"), NoTable->HasValidNaniteData());
	}
	return true;
}

// ---- #189: cell textures from their low companion, full detail near ------------------------------------------------------------------------------

namespace MantleTextureDetailTestPrivate
{
	using namespace MantleNaniteSideFileTestPrivate;

	const TCHAR* const DetailMesh = TEXT("ast_u000000000000000000000000000000e1");
	const TCHAR* const DetailMaterial = TEXT("ast_u000000000000000000000000000000e2");
	const TCHAR* const DetailTexture = TEXT("ast_u000000000000000000000000000000e3");

	/**
	 * A BC1 sRGB MTX2 of Size x Size with its full chain, every block zero (a valid, black texture) but the last mip's first byte, which is
	 * Mark: textures with another Mark are other bytes, so no build another test made is reused.
	 */
	TArray<uint8> MakeMtx2(const int32 Size, const uint8 Mark = 0)
	{
		const int32 Mips = FMath::FloorLog2(Size) + 1;
		TArray<TArray<uint8>> Streams;
		TArray<uint32> Sizes;
		for (int32 Mip = 0; Mip < Mips; ++Mip)
		{
			const uint32 Edge = static_cast<uint32>(FMath::Max(1, Size >> Mip));
			const uint32 MipBytes = FMath::Max(1u, (Edge + 3) / 4) * FMath::Max(1u, (Edge + 3) / 4) * 8;
			TArray<uint8> Blocks;
			Blocks.SetNumZeroed(MipBytes);
			if (Mip == Mips - 1)
			{
				Blocks[0] = Mark;
			}
			TArray<uint8>& Stream = Streams.AddDefaulted_GetRef();
			int32 StreamBytes = FCompression::CompressMemoryBound(NAME_Zlib, Blocks.Num());
			Stream.SetNumUninitialized(StreamBytes);
			verify(FCompression::CompressMemory(NAME_Zlib, Stream.GetData(), StreamBytes, Blocks.GetData(), Blocks.Num()));
			Stream.SetNum(StreamBytes);
			Sizes.Add(MipBytes);
		}
		TArray<uint8> Bytes;
		FMemoryWriter Ar(Bytes, true);
		uint8 Magic[4] = { 'M', 'T', 'X', '2' };
		Ar.Serialize(Magic, 4);
		uint32 Header[8] = { 1, 1, 1, static_cast<uint32>(Size), static_cast<uint32>(Size), static_cast<uint32>(Size), static_cast<uint32>(Size), static_cast<uint32>(Mips) };
		for (uint32& Value : Header)
		{
			Ar << Value;
		}
		uint32 Offset = 36 + 20 * Mips;
		for (int32 Mip = 0; Mip < Mips; ++Mip)
		{
			const uint32 Edge = static_cast<uint32>(FMath::Max(1, Size >> Mip));
			uint32 Width = Edge;
			uint32 Height = Edge;
			uint32 MipBytes = Sizes[Mip];
			uint32 Stored = static_cast<uint32>(Streams[Mip].Num());
			Ar << Width << Height << Offset << MipBytes << Stored;
			Offset += Stored;
		}
		for (TArray<uint8>& Stream : Streams)
		{
			Ar.Serialize(Stream.GetData(), Stream.Num());
		}
		return Bytes;
	}

	FMantlePlace MakeDetailPlace()
	{
		const FString Json = FString::Printf(
			TEXT("{\"schemaVersion\":1,\"placeId\":\"plc_detail001\",\"revision\":{\"number\":1,\"state\":\"draft\"},")
			TEXT("\"metadata\":{\"name\":\"Detail\",\"authorId\":\"usr_4b1d\"},\"settings\":{},\"assets\":[")
			TEXT("{\"id\":\"%s\",\"kind\":\"StaticMesh\",\"hash\":\"%s\",\"bytes\":100},")
			TEXT("{\"id\":\"%s\",\"kind\":\"Material\",\"hash\":\"%s\",\"bytes\":100},")
			TEXT("{\"id\":\"%s\",\"kind\":\"Texture\",\"hash\":\"%s\",\"bytes\":100}],\"entities\":[]}"),
			DetailMesh, *MeshTag, DetailMaterial, *MeshTag, DetailTexture, *MeshTag);
		const FMantleParseResult Parsed = FMantlePlaceParser::ParseFromString(Json);
		check(Parsed.bSuccess);
		return Parsed.Place;
	}

	/** One StaticMesh entity of the mesh with the material, for the cell's list. */
	FMantleEntity DetailEntity()
	{
		FMantleEntity Entity;
		Entity.Id = TEXT("ent_detail");
		Entity.Type = EMantleEntityType::StaticMesh;
		FMantleStaticMeshProps Props;
		Props.Mesh = DetailMesh;
		Props.Collision = EMantleCollision::None;
		Props.Materials.Add(DetailMaterial);
		Entity.Props.Set<FMantleStaticMeshProps>(Props);
		return Entity;
	}

	/** The width of the texture the live material draws in its base color slot (0 when none). */
	int32 BaseColorWidth()
	{
		FString Error;
		UMaterialInstanceDynamic* Instance = Cast<UMaterialInstanceDynamic>(FMantlePlaceAssets::FindLiveMaterial(DetailMaterial, Error));
		UTexture* Texture = nullptr;
		if (!Instance || !Instance->GetTextureParameterValue(FHashedMaterialParameterInfo(FName(TEXT("BaseColor"))), Texture, true) || !Texture)
		{
			return 0;
		}
		const UTexture2D* Texture2D = Cast<UTexture2D>(Texture);
		return Texture2D ? Texture2D->GetSizeX() : 0;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTextureDetailByDistanceTest,
	"Mantle.Runtime.Place.StreamedAssets.TextureDetailByDistance",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTextureDetailByDistanceTest::RunTest(const FString& Parameters)
{
	using namespace MantleTextureDetailTestPrivate;
	// A cell's material draws its texture from the low companion (512) until the view comes within 150 m of the entity using it; then the full file (1024) is built and
	// swapped into the material. It stays full out to 200 m, and past that goes back to low and the full build is let go.
	const TSharedRef<FServingResolver> Server = MakeShared<FServingResolver>();
	FServedAsset Mesh;
	Mesh.Kind = TEXT("mesh");
	Mesh.Bytes = MakeMrm1Box();
	Server->Assets.Add(DetailMesh, Mesh);
	FServedAsset Material;
	Material.Kind = TEXT("material");
	const FTCHARToUTF8 Document(*FString::Printf(TEXT("{\"schemaVersion\":1,\"alphaMode\":\"opaque\",\"doubleSided\":false,\"textures\":{\"baseColor\":\"%s\"}}"), DetailTexture));
	Material.Bytes.Append(reinterpret_cast<const uint8*>(Document.Get()), Document.Length());
	Server->Assets.Add(DetailMaterial, Material);
	FServedAsset Texture;
	Texture.Kind = TEXT("texture");
	Texture.Bytes = MakeMtx2(1024);
	Texture.LowBytes = MakeMtx2(512);
	Server->Assets.Add(DetailTexture, Texture);

	FMantleAssetStoreSettings StoreSettings;
	StoreSettings.RootDir = FPaths::AutomationTransientDir() / TEXT("TextureDetail") / FGuid::NewGuid().ToString();
	StoreSettings.MaxAttempts = 1;
	const TSharedRef<FServingFetcher> Fetcher = MakeShared<FServingFetcher>();
	Fetcher->Source = Server;
	FMantlePlaceAssetSettings Settings;
	Settings.bAutoPump = false;
	Settings.bRenderAssets = true;
	Settings.bLoadNanite = false;
	Settings.bAsyncCollisionCook = false;
	Settings.Resolver = Server;
	const TSharedRef<FMantleAssetStore> Store = FMantleAssetStore::Create(StoreSettings, Fetcher);
	Settings.Store = Store;
	const TSharedRef<FMantlePlaceAssets> Assets = FMantlePlaceAssets::PrepareStreaming(MakeDetailPlace(), Settings, FMantlePlaceAssetsDone());
	auto PumpUntil = [&Store, &Assets](TFunctionRef<bool()> Done, const double Seconds)
	{
		const double Deadline = FPlatformTime::Seconds() + Seconds;
		while (!Done())
		{
			if (FPlatformTime::Seconds() > Deadline)
			{
				return false;
			}
			Store->Pump();
			Assets->Pump();
			FPlatformProcess::Sleep(0.001f);
		}
		return true;
	};
	auto PumpFor = [&PumpUntil](const double Seconds) { PumpUntil([]() { return false; }, Seconds); };
	if (!TestTrue(TEXT("the root completes"), PumpUntil([&Assets]() { return Assets->IsComplete(); }, 30.0)))
	{
		return false;
	}

	TArray<FMantleEntity> Cell;
	Cell.Add(DetailEntity());
	Assets->AcquireEntityAssets(80, Cell);
	if (!TestTrue(TEXT("the cell's assets settle"), PumpUntil([&Assets]() { return Assets->GetHolderAssets(80).IsSettled(); }, 30.0)))
	{
		return false;
	}
	PumpFor(0.6);
	const FString FullUrl = FString(TEXT("test://")) + DetailTexture;
	const FString LowUrl = FullUrl + TEXT("#low");
	TestEqual(TEXT("the texture was built from its low companion"), Assets->GetStreamedCounts().LowTextures, 1);
	TestEqual(TEXT("its low file was fetched once"), Fetcher->Fetched.FindRef(LowUrl), 1);
	TestEqual(TEXT("and with no view the full file is not fetched"), Fetcher->Fetched.FindRef(FullUrl), 0);
	TestEqual(TEXT("the material draws the 512 low texture"), BaseColorWidth(), 512);

	// Near: 1 m from the entity.
	Assets->SetTextureView(FVector(100.0, 0.0, 0.0));
	TestTrue(TEXT("near, the full texture is built and shown"), PumpUntil([&Assets]() { return Assets->GetStreamedCounts().FullTexturesShown == 1; }, 30.0));
	TestEqual(TEXT("the full file was fetched once"), Fetcher->Fetched.FindRef(FullUrl), 1);
	TestEqual(TEXT("the material draws the 1024 full texture"), BaseColorWidth(), 1024);

	// 175 m: past the 150 m that asks for full, short of the 200 m that lets it go.
	Assets->SetTextureView(FVector(17500.0, 0.0, 0.0));
	PumpFor(1.2);
	TestEqual(TEXT("between 150 and 200 m it stays full"), BaseColorWidth(), 1024);

	// Far: 300 m.
	Assets->SetTextureView(FVector(30000.0, 0.0, 0.0));
	TestTrue(TEXT("far, it goes back to low"), PumpUntil([&Assets]()
	{
		const FMantlePlaceAssets::FStreamedCounts Counts = Assets->GetStreamedCounts();
		return Counts.FullTexturesShown == 0 && Counts.FullTexturesWanted == 0;
	}, 30.0));
	TestEqual(TEXT("the material draws the low texture again"), BaseColorWidth(), 512);

	Assets->ReleaseEntityAssets(80);
	TestTrue(TEXT("the release drops the texture"), PumpUntil([&Assets]() { return Assets->GetStreamedCounts().Textures == 0; }, 30.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTextureLowFallsBackToFullTest,
	"Mantle.Runtime.Place.StreamedAssets.LowTextureFallsBackToFull",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTextureLowFallsBackToFullTest::RunTest(const FString& Parameters)
{
	using namespace MantleTextureDetailTestPrivate;
	// #189: a cell texture whose low companion cannot be had (its url answers 404, again after a fresh resolve) is not a failed texture: it is built from the full
	// file, and the material draws that, with no view anywhere near.
	const TSharedRef<FServingResolver> Server = MakeShared<FServingResolver>();
	FServedAsset Mesh;
	Mesh.Kind = TEXT("mesh");
	Mesh.Bytes = MakeMrm1Box();
	Server->Assets.Add(DetailMesh, Mesh);
	FServedAsset Material;
	Material.Kind = TEXT("material");
	const FTCHARToUTF8 Document(*FString::Printf(TEXT("{\"schemaVersion\":1,\"alphaMode\":\"opaque\",\"doubleSided\":false,\"textures\":{\"baseColor\":\"%s\"}}"), DetailTexture));
	Material.Bytes.Append(reinterpret_cast<const uint8*>(Document.Get()), Document.Length());
	Server->Assets.Add(DetailMaterial, Material);
	FServedAsset Texture;
	Texture.Kind = TEXT("texture");
	// Bytes of their own, so no build another test made from MakeMtx2's files is reused here.
	Texture.Bytes = MakeMtx2(1024, 1);
	Texture.LowBytes = MakeMtx2(512, 1);
	Texture.bServeLow = false;
	Server->Assets.Add(DetailTexture, Texture);

	FMantleAssetStoreSettings StoreSettings;
	StoreSettings.RootDir = FPaths::AutomationTransientDir() / TEXT("TextureLowFallback") / FGuid::NewGuid().ToString();
	StoreSettings.MaxAttempts = 1;
	const TSharedRef<FServingFetcher> Fetcher = MakeShared<FServingFetcher>();
	Fetcher->Source = Server;
	FMantlePlaceAssetSettings Settings;
	Settings.bAutoPump = false;
	Settings.bRenderAssets = true;
	Settings.bLoadNanite = false;
	Settings.bAsyncCollisionCook = false;
	Settings.Resolver = Server;
	const TSharedRef<FMantleAssetStore> Store = FMantleAssetStore::Create(StoreSettings, Fetcher);
	Settings.Store = Store;
	const TSharedRef<FMantlePlaceAssets> Assets = FMantlePlaceAssets::PrepareStreaming(MakeDetailPlace(), Settings, FMantlePlaceAssetsDone());
	auto PumpUntil = [&Store, &Assets](TFunctionRef<bool()> Done, const double Seconds)
	{
		const double Deadline = FPlatformTime::Seconds() + Seconds;
		while (!Done())
		{
			if (FPlatformTime::Seconds() > Deadline)
			{
				return false;
			}
			Store->Pump();
			Assets->Pump();
			FPlatformProcess::Sleep(0.001f);
		}
		return true;
	};
	if (!TestTrue(TEXT("the root completes"), PumpUntil([&Assets]() { return Assets->IsComplete(); }, 30.0)))
	{
		return false;
	}

	TArray<FMantleEntity> Cell;
	Cell.Add(DetailEntity());
	Assets->AcquireEntityAssets(81, Cell);
	if (!TestTrue(TEXT("the cell's assets settle"), PumpUntil([&Assets]() { return Assets->GetHolderAssets(81).IsSettled(); }, 30.0)))
	{
		return false;
	}
	const FString FullUrl = FString(TEXT("test://")) + DetailTexture;
	const FString LowUrl = FullUrl + TEXT("#low");
	TestTrue(TEXT("the low file was asked for, and again after a fresh resolve"), Fetcher->Fetched.FindRef(LowUrl) >= 2);
	TestEqual(TEXT("the full file was fetched once"), Fetcher->Fetched.FindRef(FullUrl), 1);
	TestEqual(TEXT("the texture is not counted as low"), Assets->GetStreamedCounts().LowTextures, 0);
	TestEqual(TEXT("the material draws the 1024 full texture"), BaseColorWidth(), 1024);

	Assets->ReleaseEntityAssets(81);
	TestTrue(TEXT("the release drops the texture"), PumpUntil([&Assets]() { return Assets->GetStreamedCounts().Textures == 0; }, 30.0));
	return true;
}

namespace MantleLumenDecodeTestPrivate
{
	using namespace MantleNaniteSideFileTestPrivate;

	const TCHAR* const LumenMesh = TEXT("ast_u000000000000000000000000000000f1");
	const TCHAR* const LumenFile = TEXT("ast_u000000000000000000000000000000f2");

	/** A small valid MLD1 for the mesh MeshTag names (the field MantleMeshLumenDataTest builds by hand). */
	TArray<uint8> MakeMld1()
	{
		FDistanceFieldVolumeData Field;
		Field.LocalSpaceMeshBounds = FBox3f(FVector3f(-50.0f), FVector3f(50.0f));
		Field.Mips[0].IndirectionDimensions = FIntVector(2, 2, 2);
		Field.Mips[0].NumDistanceFieldBricks = 3;
		Field.Mips[0].BulkOffset = 16;
		Field.Mips[0].BulkSize = 48;
		Field.Mips[DistanceField::NumMips - 1].IndirectionDimensions = FIntVector(1, 1, 1);
		Field.Mips[DistanceField::NumMips - 1].BulkSize = 3;
		Field.AlwaysLoadedMip = { 9, 8, 7 };
		Field.StreamableMips.Lock(LOCK_READ_WRITE);
		uint8* Streamed = Field.StreamableMips.Realloc(64);
		FMemory::Memset(Streamed, 0x11, 16);
		FMemory::Memset(Streamed + 16, 0x5A, 48);
		Field.StreamableMips.Unlock();
		FCardRepresentationData Cards;
		Cards.MeshCardsBuildData.Bounds = FBox(FVector(-50.0), FVector(50.0));
		Cards.MeshCardsBuildData.CardBuildData.AddDefaulted(2);
		return FMantleMeshLumenFile::Write(MeshTag, Field, Cards);
	}


	FMantlePlace MakeLumenPlace()
	{
		const FString Json = FString::Printf(
			TEXT("{\"schemaVersion\":1,\"placeId\":\"plc_lumendec01\",\"revision\":{\"number\":1,\"state\":\"draft\"},"
				 "\"metadata\":{\"name\":\"Lumen\",\"authorId\":\"usr_4b1d\"},\"settings\":{},\"assets\":["
				 "{\"id\":\"%s\",\"kind\":\"StaticMesh\",\"hash\":\"%s\",\"bytes\":100,\"lumen\":\"%s\"},"
				 "{\"id\":\"%s\",\"kind\":\"Lumen\",\"hash\":\"%s\",\"bytes\":50,\"engine\":\"%s\"}],"
				 "\"entities\":[{\"id\":\"ent_l\",\"type\":\"StaticMesh\",\"parent\":null,\"transform\":{\"location\":[0,0,0],\"rotation\":[0,0,0],\"scale\":[1,1,1]},\"properties\":{\"mesh\":\"%s\"}}]}"),
			LumenMesh, *MeshTag, LumenFile, LumenFile, *MeshTag, *FMantleMeshLumenFile::EngineTag(), LumenMesh);
		const FMantleParseResult Parsed = FMantlePlaceParser::ParseFromString(Json);
		check(Parsed.bSuccess);
		return Parsed.Place;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleLumenInflatedOffTheGameThreadTest,
	"Mantle.Runtime.StaticMesh.LumenInflatedBeforeTheBuild",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleLumenInflatedOffTheGameThreadTest::RunTest(const FString& Parameters)
{
	using namespace MantleLumenDecodeTestPrivate;
	// #189 step 5: the backend stores a mesh's Lumen file as MLD2. The place's decode task inflates it, so the mesh build on the game thread reads
	// the MLD1 as it is (City Sample's Lumen is 1.41 GB inflated: seconds of hitches if the build inflated it).
	const TArray<uint8> Mld1 = MakeMld1();
	if (!TestTrue(TEXT("the Lumen file is written"), Mld1.Num() > 0))
	{
		return false;
	}
	const TSharedRef<FServingResolver> Server = MakeShared<FServingResolver>();
	FServedAsset Mesh;
	Mesh.Kind = TEXT("mesh");
	// Bytes of its own, so no build another test made is reused and this mesh is built here, reading its Lumen file.
	Mesh.Bytes = MakeMrm3Box(41.0f);
	Server->Assets.Add(LumenMesh, Mesh);
	FServedAsset Lumen;
	Lumen.Kind = TEXT("lumen");
	Lumen.Bytes = MakeEnvelope("MLD2", Mld1);
	Server->Assets.Add(LumenFile, Lumen);

	FMantleAssetStoreSettings StoreSettings;
	StoreSettings.RootDir = FPaths::AutomationTransientDir() / TEXT("LumenDecode") / FGuid::NewGuid().ToString();
	StoreSettings.MaxAttempts = 1;
	const TSharedRef<FServingFetcher> Fetcher = MakeShared<FServingFetcher>();
	Fetcher->Source = Server;
	FMantlePlaceAssetSettings Settings;
	Settings.bAutoPump = false;
	Settings.bRenderAssets = true;
	Settings.bLoadNanite = false;
	Settings.bAsyncCollisionCook = false;
	Settings.Resolver = Server;
	Settings.Store = FMantleAssetStore::Create(StoreSettings, Fetcher);

	const int64 ReadsBefore = FMantleMeshLumenFile::GetReadCount();
	const TSharedRef<FMantlePlaceAssets> Assets = FMantlePlaceAssets::Prepare(MakeLumenPlace(), Settings, FMantlePlaceAssetsDone());
	Assets->WaitUntilComplete(60.0f);
	TestTrue(TEXT("the place's assets complete"), Assets->IsComplete());
	// Read takes only an MLD1, so a build that read the file was handed it inflated.
	FMantleMeshLumenData Unused;
	TestFalse(TEXT("Read does not take the MLD2 that was served"), FMantleMeshLumenFile::Read(Lumen.Bytes, MeshTag, Unused));
	TestEqual(TEXT("the mesh build read its Lumen file, inflated before it"), FMantleMeshLumenFile::GetReadCount() - ReadsBefore, 1ll);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
