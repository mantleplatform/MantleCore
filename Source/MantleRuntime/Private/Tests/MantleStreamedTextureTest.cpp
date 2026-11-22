// Copyright Joshua Eger. All Rights Reserved.
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/StaticMeshComponent.h"
#include "ContentStreaming.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Math/RandomStream.h"
#include "MantlePlaceMaterial.h"
#include "MantleRuntimeStaticMeshBuilder.h"
#include "MantleRuntimeTextureBuilder.h"
#include "MantleStaticMeshReader.h"
#include "MantleStreamBudget.h"
#include "MantleStreamedTexture.h"
#include "MantleCookedPacking.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "RenderingThread.h"
#include "Serialization/MemoryWriter.h"
#include "TextureResource.h"
#include "UObject/StrongObjectPtr.h"

namespace MantleStreamedTextureTestPrivate
{
	/** A BC1 sRGB MTX3 of Size x Size with its full chain, every block random (so the Kraken streams are about as big as the blocks, as real BC1 is). */
	TArray<uint8> MakeMtx3(const int32 Size)
	{
		FRandomStream Random(Size);
		const int32 Mips = FMath::FloorLog2(Size) + 1;
		TArray<TArray<uint8>> Streams;
		TArray<uint32> Sizes;
		for (int32 Mip = 0; Mip < Mips; ++Mip)
		{
			const uint32 Edge = static_cast<uint32>(FMath::Max(1, Size >> Mip));
			const uint32 MipBytes = FMath::Max(1u, (Edge + 3) / 4) * FMath::Max(1u, (Edge + 3) / 4) * 8;
			TArray<uint8> Blocks;
			Blocks.SetNumUninitialized(MipBytes);
			for (uint8& Byte : Blocks)
			{
				Byte = static_cast<uint8>(Random.RandHelper(256));
			}
			TArray<uint8>& Stream = Streams.AddDefaulted_GetRef();
			Stream = MantleCookedPacking::PackStream(Blocks);
			check(!Stream.IsEmpty());
			Sizes.Add(MipBytes);
		}
		TArray<uint8> Bytes;
		FMemoryWriter Ar(Bytes, true);
		uint8 Magic[4] = { 'M', 'T', 'X', '3' };
		Ar.Serialize(Magic, 4);
		uint32 Header[8] = { 1, 1, 1, static_cast<uint32>(Size), static_cast<uint32>(Size), static_cast<uint32>(Size), static_cast<uint32>(Size), static_cast<uint32>(Mips) };
		for (uint32& Value : Header)
		{
			Ar << Value;
		}
		uint32 Offset = 36 + 20 * Mips;
		for (int32 Mip = 0; Mip < Mips; ++Mip)
		{
			uint32 Edge = static_cast<uint32>(FMath::Max(1, Size >> Mip));
			uint32 Height = Edge;
			uint32 MipBytes = Sizes[Mip];
			uint32 Stored = static_cast<uint32>(Streams[Mip].Num());
			Ar << Edge << Height << Offset << MipBytes << Stored;
			Offset += Stored;
		}
		for (TArray<uint8>& Stream : Streams)
		{
			Ar.Serialize(Stream.GetData(), Stream.Num());
		}
		return Bytes;
	}

	/** A flat square Edge cm on a side, facing up, its UVs 0..1 once across it: one world unit of UV is Edge cm. */
	FMantleStaticMeshData MakeQuad(const float Edge)
	{
		FMantleStaticMeshData Mesh;
		const float Half = Edge * 0.5f;
		for (int32 Corner = 0; Corner < 4; ++Corner)
		{
			const float X = (Corner & 1) ? Half : -Half;
			const float Y = (Corner & 2) ? Half : -Half;
			Mesh.Positions.Add(FVector3f(X, Y, 0.0f));
			Mesh.Normals.Add(FVector3f(0.0f, 0.0f, 1.0f));
			Mesh.Tangents.Add(FVector4f(1.0f, 0.0f, 0.0f, 1.0f));
			Mesh.UVs.Add(FVector2f((X + Half) / Edge, (Y + Half) / Edge));
		}
		const uint32 Triangles[2][3] = { { 0, 1, 2 }, { 1, 3, 2 } };
		for (const uint32 (&Triangle)[3] : Triangles)
		{
			Mesh.Indices.Append({ Triangle[0], Triangle[1], Triangle[2] });
		}
		Mesh.Bounds = FBox3f(FVector3f(-Half, -Half, 0.0f), FVector3f(Half, Half, 0.0f));
		FMantleSkinnedMeshSection Section;
		Section.MaterialIndex = 0;
		Section.FirstIndex = 0;
		Section.IndexCount = 6;
		Section.MaterialName = TEXT("A");
		Mesh.Sections = { Section };
		return Mesh;
	}

	/** Runs the streamer as a frame would, from one view, until nothing is in flight. */
	void SettleStreaming(const FVector& ViewOrigin)
	{
		// A 1920-pixel-wide view with a 90 degree field of view (FOVScreenSize = ScreenSize / tan(fov / 2)).
		constexpr float ScreenSize = 1920.0f;
		for (int32 Frame = 0; Frame < 12; ++Frame)
		{
			IStreamingManager::Get().AddViewInformation(ViewOrigin, ScreenSize, ScreenSize);
			IStreamingManager::Get().Tick(0.1f, /*bProcessEverything*/ true);
			IStreamingManager::Get().BlockTillAllRequestsFinished(5.0f);
			FlushRenderingCommands();
		}
	}

	int64 ResidentBytes(const UTexture2D* Texture)
	{
		const FTexturePlatformData* PlatformData = Texture->GetPlatformData();
		const int32 Resident = Texture->GetNumResidentMips();
		int64 Bytes = 0;
		for (int32 Mip = PlatformData->Mips.Num() - Resident; Mip < PlatformData->Mips.Num(); ++Mip)
		{
			Bytes += FMantleRuntimeTextureBuilder::ComputeMipBytes(PlatformData->PixelFormat, PlatformData->Mips[Mip].SizeX, PlatformData->Mips[Mip].SizeY);
		}
		return Bytes;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleStreamedTextureTest,
	"Mantle.Runtime.TextureBuilder.StreamsMipsFromTheCache",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleStreamedTextureTest::RunTest(const FString& Parameters)
{
	using namespace MantleStreamedTextureTestPrivate;
	if (IsRunningDedicatedServer())
	{
		AddInfo(TEXT("A dedicated server builds no textures."));
		return true;
	}

	const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("MantleTests/StreamedTexture"));
	const FString Path = Dir / TEXT("t2048.mtx2");
	IFileManager::Get().MakeDirectory(*Dir, true);
	const TArray<uint8> File = MakeMtx3(2048);
	if (!TestTrue(TEXT("the test file is written"), FFileHelper::SaveArrayToFile(File, *Path)))
	{
		return false;
	}

	FMantleTextureBuildParams Params;
	Params.bKeepCpuCopy = false;
	FString Error;
	TStrongObjectPtr<UTexture2D> Texture(FMantleRuntimeTextureBuilder::BuildStreamed(Path, Params, &Error));
	if (!TestNotNull(*FString::Printf(TEXT("the streamed texture builds (%s)"), *Error), Texture.Get()))
	{
		return false;
	}
	const FTexturePlatformData* PlatformData = Texture->GetPlatformData();
	const int32 Kept = PlatformData->Mips.Num();
	const int32 Resident = FMantleRuntimeTextureBuilder::ComputeResidentMips(PF_DXT1, PlatformData->SizeX, PlatformData->SizeY, Kept);
	AddInfo(FString::Printf(TEXT("%d x %d, %d mips, %d resident at build"), PlatformData->SizeX, PlatformData->SizeY, Kept, Resident));
	TestEqual(TEXT("a 2048 BC1 keeps 64 x 64 and down"), Resident, 7);
	int64 StreamedBytes = 0;
	for (int32 Mip = 0; Mip < Kept - Resident; ++Mip)
	{
		TestFalse(*FString::Printf(TEXT("streamed mip %d is not in memory"), Mip), PlatformData->Mips[Mip].BulkData.IsBulkDataLoaded());
		TestTrue(*FString::Printf(TEXT("streamed mip %d is marked as stored apart"), Mip), PlatformData->Mips[Mip].BulkData.IsInSeparateFile());
		StreamedBytes += FMantleRuntimeTextureBuilder::ComputeMipBytes(PF_DXT1, PlatformData->Mips[Mip].SizeX, PlatformData->Mips[Mip].SizeY);
	}

	if (!FApp::CanEverRender())
	{
		AddInfo(TEXT("No renderer (-nullrhi): only the build is checked."));
		IFileManager::Get().Delete(*Path);
		return true;
	}

	FlushRenderingCommands();
	TestTrue(TEXT("the engine treats the texture as streamable"), Texture->IsStreamable());
	TestEqual(TEXT("only the tail is on the GPU at first"), Texture->GetNumResidentMips(), Resident);
	for (int32 Mip = 0; Mip < Kept; ++Mip)
	{
		TestFalse(*FString::Printf(TEXT("mip %d is not held in RAM after the upload"), Mip), PlatformData->Mips[Mip].BulkData.IsBulkDataLoaded());
	}

	// 1. Asked directly, the stream-in reads every top mip from the file.
	UMantleCachedMipsFactory::ResetStats();
	TestTrue(TEXT("a stream-in of every mip starts"), Texture->StreamIn(Kept, /*bHighPrio*/ true));
	Texture->WaitForPendingInitOrStreaming();
	FlushRenderingCommands();
	FMantleStreamedMipStats Stats = UMantleCachedMipsFactory::GetStats();
	TestEqual(TEXT("every mip is on the GPU after the stream-in"), Texture->GetNumResidentMips(), Kept);
	TestEqual(TEXT("the provider served the streamed mips"), Stats.MipsServed, static_cast<int64>(Kept - Resident));
	TestEqual(TEXT("and their bytes"), Stats.BytesServed, StreamedBytes);
	TestEqual(TEXT("nothing failed"), Stats.Failed, 0ll);
	AddInfo(FString::Printf(TEXT("stream-in of %d mips: %lld bytes from %lld packed, %.2f ms on the worker"),
		Kept - Resident, Stats.BytesServed, Stats.PackedBytesRead, Stats.MeanRequestMs));
	TestTrue(TEXT("a stream-out to the tail starts"), Texture->StreamOut(Resident));
	Texture->WaitForPendingInitOrStreaming();
	FlushRenderingCommands();
	TestEqual(TEXT("back to the tail after the stream-out"), Texture->GetNumResidentMips(), Resident);

	// 2. The streamer picks the mips: one Mantle mesh (a 4 m square, the texture once across it) in a game world, the view straight above it at each distance.
	UWorld* World = GEngine ? UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("MantleTextureStreamTrial")) : nullptr;
	if (!TestNotNull(TEXT("a scratch world"), World))
	{
		return false;
	}
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	ON_SCOPE_EXIT
	{
		World->DestroyWorld(/*bInformEngineOfWorld*/ false);
		GEngine->DestroyWorldContext(World);
		IFileManager::Get().Delete(*Path);
	};

	constexpr float Edge = 400.0f;
	FMantleStaticMeshBuildParams MeshParams;
	TStrongObjectPtr<UStaticMesh> Mesh(FMantleRuntimeStaticMeshBuilder::Build(MakeQuad(Edge), MeshParams, &Error));
	if (!TestNotNull(*FString::Printf(TEXT("the quad builds (%s)"), *Error), Mesh.Get()))
	{
		return false;
	}
	UTexture* Slots[1] = { Texture.Get() };
	TStrongObjectPtr<UMaterialInstanceDynamic> Material(FMantlePlaceMaterials::Build(FMantlePlaceMaterialDesc(), Slots, nullptr, NAME_None, &Error));
	if (!Material.IsValid())
	{
		AddWarning(FString::Printf(TEXT("No place material (%s): the streamer part is skipped."), *Error));
		return true;
	}

	AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>();
	UStaticMeshComponent* Component = Actor ? Actor->GetStaticMeshComponent() : nullptr;
	if (!TestNotNull(TEXT("the mesh actor"), Component))
	{
		return false;
	}
	Component->SetMobility(EComponentMobility::Movable);
	Component->SetStaticMesh(Mesh.Get());
	Component->SetMaterial(0, Material.Get());

	// The streamer keeps mips it no longer wants while its pool has room (r.Streaming.DropMips 0); with 1 it drops them, so what it holds is what it wants.
	IConsoleVariable* DropMips = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Streaming.DropMips"));
	const int32 DropMipsBefore = DropMips ? DropMips->GetInt() : 0;
	if (DropMips)
	{
		DropMips->Set(1, ECVF_SetByCode);
	}
	ON_SCOPE_EXIT
	{
		if (DropMips)
		{
			DropMips->Set(DropMipsBefore, ECVF_SetByCode);
		}
	};

	// The builder measured the quad: one UV unit spans Edge cm (FMantleRuntimeStaticMeshBuilder::ComputeSlotUVDensities).
	TestEqual(TEXT("the quad's slot says Edge cm per UV"), Mesh->GetStaticMaterials()[0].UVChannelData.LocalUVDensities[0], Edge, 0.01f);

	// Out to 1 km and back. What the streamer holds at each view (r.Streaming.DropMips 1: what it wants), measured 2026-11-22: 1 m 2048, 4 m 1024, 16 m 256, 64 m and on the tail.
	struct FStep
	{
		double Meters;
		int32 Mips;
	};
	const FStep Steps[] = { { 1024.0, 7 }, { 64.0, 7 }, { 1.0, 12 }, { 4.0, 11 }, { 16.0, 9 }, { 64.0, 7 }, { 256.0, 7 }, { 1024.0, 7 }, { 16.0, 9 }, { 1.0, 12 } };
	for (int32 Step = 0; Step < UE_ARRAY_COUNT(Steps); ++Step)
	{
		const double Meters = Steps[Step].Meters;
		UMantleCachedMipsFactory::ResetStats();
		SettleStreaming(FVector(0.0, 0.0, Meters * 100.0));
		const int32 Now = Texture->GetNumResidentMips();
		Stats = UMantleCachedMipsFactory::GetStats();
		AddInfo(FString::Printf(TEXT("MANTLE_TEXSTREAM step %d, view %.0f m: %d of %d mips (top %d px), wanted %d, %.2f MB on the GPU; read %lld mips (%lld bytes) in %lld requests, %.2f ms mean; failed %lld"),
			Step, Meters, Now, Kept, PlatformData->SizeX >> (Kept - Now), static_cast<int32>(Texture->GetCachedNumWantedLODs()),
			ResidentBytes(Texture.Get()) / (1024.0 * 1024.0), Stats.MipsServed, Stats.BytesServed, Stats.Requests, Stats.MeanRequestMs, Stats.Failed));
		TestEqual(*FString::Printf(TEXT("mips held from %.0f m (step %d)"), Meters, Step), Now, Steps[Step].Mips);
		TestEqual(TEXT("no read failed"), Stats.Failed, 0ll);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTexturePoolTest,
	"Mantle.Runtime.StreamBudget.TexturePool",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTexturePoolTest::RunTest(const FString& Parameters)
{
	constexpr int64 MB = 1024 * 1024;
	FMantleBudgetLimits Limits;
	Limits.TextureBytes = 8192 * MB;
	TestEqual(TEXT("nothing held: the whole share"), FMantleStreamBudget::TexturePoolBytes(Limits, 0), 8192 * MB);
	TestEqual(TEXT("2 GB held whole: the rest"), FMantleStreamBudget::TexturePoolBytes(Limits, 2048 * MB), 6144 * MB);
	TestEqual(TEXT("in whole 64 MB, rounded down"), FMantleStreamBudget::TexturePoolBytes(Limits, 2048 * MB + 1), 6080 * MB);
	TestEqual(TEXT("over the share: the floor"), FMantleStreamBudget::TexturePoolBytes(Limits, 9000 * MB), FMantleStreamBudget::MinTexturePoolBytes);
	Limits.TextureBytes = MAX_int64;
	TestEqual(TEXT("no texture limit: the pool is left alone"), FMantleStreamBudget::TexturePoolBytes(Limits, 0), 0ll);

	// The pool at each look: it gives way when the card is over its cushion, holds near it, grows back slowly under it, and never passes what the share leaves.
	const int64 Floor = FMantleStreamBudget::MinTexturePoolBytes;
	TestEqual(TEXT("first look: what the share leaves"), FMantleStreamBudget::NextTexturePool(0, 6144 * MB, 0), 6144 * MB);
	TestEqual(TEXT("1 GB over: 1 GB less"), FMantleStreamBudget::NextTexturePool(6144 * MB, 6144 * MB, 1024 * MB), 5120 * MB);
	TestEqual(TEXT("far over: the floor"), FMantleStreamBudget::NextTexturePool(1024 * MB, 6144 * MB, 4096 * MB), Floor);
	TestEqual(TEXT("just under: holds"), FMantleStreamBudget::NextTexturePool(5120 * MB, 6144 * MB, -100 * MB), 5120 * MB);
	TestEqual(TEXT("well under: grows back 256 MB"), FMantleStreamBudget::NextTexturePool(5120 * MB, 6144 * MB, -2048 * MB), 5376 * MB);
	TestEqual(TEXT("never past what the share leaves"), FMantleStreamBudget::NextTexturePool(6100 * MB, 6144 * MB, -2048 * MB), 6144 * MB);
	TestEqual(TEXT("the share shrinks: so does the pool"), FMantleStreamBudget::NextTexturePool(6144 * MB, 4096 * MB, 0), 4096 * MB);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleStaticMeshUVDensityTest,
	"Mantle.Runtime.StaticMesh.UVDensity",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleStaticMeshUVDensityTest::RunTest(const FString& Parameters)
{
	using namespace MantleStreamedTextureTestPrivate;

	// A 4 m square with its UVs once across: 400 cm per UV. The same square tiling its UVs 4 times: 100.
	FMantlePreparedStaticMesh Once;
	TestTrue(TEXT("the square prepares"), FMantleRuntimeStaticMeshBuilder::Prepare(MakeQuad(400.0f), false, Once));
	if (TestEqual(TEXT("one slot"), Once.Slots.UVDensities.Num(), 1))
	{
		TestEqual(TEXT("once across: 400 cm per UV"), Once.Slots.UVDensities[0].X, 400.0f, 0.01f);
	}
	FMantleStaticMeshData Tiled = MakeQuad(400.0f);
	for (FVector2f& UV : Tiled.UVs)
	{
		UV *= 4.0f;
	}
	FMantlePreparedStaticMesh Four;
	TestTrue(TEXT("the tiled square prepares"), FMantleRuntimeStaticMeshBuilder::Prepare(Tiled, false, Four));
	if (TestEqual(TEXT("one slot"), Four.Slots.UVDensities.Num(), 1))
	{
		TestEqual(TEXT("tiled 4 times: 100 cm per UV"), Four.Slots.UVDensities[0].X, 100.0f, 0.01f);
	}

	// Two slots of different density: each slot is measured on its own triangles. Slot A is the 4 m square, slot B a 1 m square beside it, both once across.
	FMantleStaticMeshData Two = MakeQuad(400.0f);
	const uint32 Base = static_cast<uint32>(Two.Positions.Num());
	for (int32 Corner = 0; Corner < 4; ++Corner)
	{
		const float X = (Corner & 1) ? 50.0f : -50.0f;
		const float Y = (Corner & 2) ? 50.0f : -50.0f;
		Two.Positions.Add(FVector3f(X + 1000.0f, Y, 0.0f));
		Two.Normals.Add(FVector3f(0.0f, 0.0f, 1.0f));
		Two.Tangents.Add(FVector4f(1.0f, 0.0f, 0.0f, 1.0f));
		Two.UVs.Add(FVector2f((X + 50.0f) / 100.0f, (Y + 50.0f) / 100.0f));
	}
	Two.Indices.Append({ Base + 0, Base + 1, Base + 2, Base + 1, Base + 3, Base + 2 });
	Two.Bounds += FBox3f(FVector3f(950.0f, -50.0f, 0.0f), FVector3f(1050.0f, 50.0f, 0.0f));
	FMantleSkinnedMeshSection B;
	B.MaterialIndex = 1;
	B.FirstIndex = 6;
	B.IndexCount = 6;
	B.MaterialName = TEXT("B");
	Two.Sections.Add(B);
	FMantlePreparedStaticMesh TwoSlots;
	FString Error;
	TestTrue(*FString::Printf(TEXT("the two-slot mesh prepares (%s)"), *Error), FMantleRuntimeStaticMeshBuilder::Prepare(Two, false, TwoSlots, &Error));
	if (TestEqual(TEXT("two slots"), TwoSlots.Slots.UVDensities.Num(), 2))
	{
		TestEqual(TEXT("slot A: 400"), TwoSlots.Slots.UVDensities[0].X, 400.0f, 0.01f);
		TestEqual(TEXT("slot B: 100"), TwoSlots.Slots.UVDensities[1].X, 100.0f, 0.01f);
	}

	// The built mesh carries it on its slot, where the streamer reads it (UStaticMesh::GetUVChannelData).
	FMantleStaticMeshBuildParams Params;
	Params.bInitRenderResources = false;
	TStrongObjectPtr<UStaticMesh> Mesh(FMantleRuntimeStaticMeshBuilder::Build(MakeQuad(400.0f), Params, &Error));
	if (TestNotNull(*FString::Printf(TEXT("the square builds (%s)"), *Error), Mesh.Get()))
	{
		const FMeshUVChannelInfo* Channel = Mesh->GetUVChannelData(0);
		TestTrue(TEXT("the slot has its density"), Channel && Channel->bInitialized && FMath::IsNearlyEqual(Channel->LocalUVDensities[0], 400.0f, 0.01f));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
