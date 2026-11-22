// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCookedPacking.h"
#include "MantleMeshLumenData.h"
#include "MantleRuntimeTextureBuilder.h"
#include "MantleStaticMeshReader.h"

#include "Misc/AutomationTest.h"
#include "Misc/Compression.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/MemoryWriter.h"

#if WITH_DEV_AUTOMATION_TESTS

// #189 step 5: the cook compresses textures (MTX2, a zlib stream per mip), meshes (MRM3) and Lumen files (MLD2: the
// MRM2 or MLD1 in an envelope). The client reads only these: a file cooked before (MTX1, a bare MRM2) is refused, and its
// place is republished. A damaged stream is refused the way the Nanite side file's is (backend/internal/cook/packed_test.go
// is the writer's side).

namespace MantleCookedPackingTest
{
	TArray<uint8> Zlib(const TConstArrayView<uint8> Raw)
	{
		TArray<uint8> Packed;
		int32 PackedBytes = FCompression::CompressMemoryBound(NAME_Zlib, Raw.Num());
		Packed.SetNumUninitialized(PackedBytes);
		verify(FCompression::CompressMemory(NAME_Zlib, Packed.GetData(), PackedBytes, Raw.GetData(), Raw.Num()));
		Packed.SetNum(PackedBytes);
		return Packed;
	}

	void Put32(TArray<uint8>& Bytes, const int32 At, const uint32 Value)
	{
		for (int32 Byte = 0; Byte < 4; ++Byte)
		{
			Bytes[At + Byte] = static_cast<uint8>(Value >> (8 * Byte));
		}
	}

	void Append32(TArray<uint8>& Bytes, const uint32 Value)
	{
		const int32 At = Bytes.AddZeroed(4);
		Put32(Bytes, At, Value);
	}

	TArray<uint8> Envelope(const ANSICHAR* Magic, const TConstArrayView<uint8> Inner)
	{
		TArray<uint8> Bytes;
		Bytes.Append(reinterpret_cast<const uint8*>(Magic), 4);
		Append32(Bytes, static_cast<uint32>(Inner.Num()));
		Bytes.Append(Zlib(Inner));
		return Bytes;
	}

	/** The blocks of an 8 x 8 BC1 texture's chain (8, 4, 2, 1), every block patterned. */
	TArray<TArray<uint8>> TextureMips()
	{
		const int32 Sizes[4] = { 8, 4, 2, 1 };
		TArray<TArray<uint8>> Mips;
		for (int32 Mip = 0; Mip < 4; ++Mip)
		{
			const int32 Blocks = FMath::Max(1, Sizes[Mip] / 4);
			TArray<uint8>& Level = Mips.AddDefaulted_GetRef();
			for (int32 Byte = 0; Byte < Blocks * Blocks * 8; ++Byte)
			{
				Level.Add(static_cast<uint8>(Mip * 37 + Byte * 11));
			}
		}
		return Mips;
	}

	/** TextureMips as an sRGB MTX2, or as the MTX1 the cook wrote before (bPacked false). */
	TArray<uint8> Texture(const bool bPacked)
	{
		const int32 Sizes[4] = { 8, 4, 2, 1 };
		const TArray<TArray<uint8>> Mips = TextureMips();
		TArray<uint8> Bytes;
		Bytes.Append(reinterpret_cast<const uint8*>(bPacked ? "MTX2" : "MTX1"), 4);
		for (const uint32 Field : { 1u, 1u, 1u, 8u, 8u, 8u, 8u, 4u })
		{
			Append32(Bytes, Field);
		}
		const int32 EntryBytes = bPacked ? 20 : 16;
		const int32 TableAt = Bytes.AddZeroed(4 * EntryBytes);
		for (int32 Mip = 0; Mip < 4; ++Mip)
		{
			const TArray<uint8> Stored = bPacked ? Zlib(Mips[Mip]) : Mips[Mip];
			const int32 Entry = TableAt + Mip * EntryBytes;
			Put32(Bytes, Entry, static_cast<uint32>(Sizes[Mip]));
			Put32(Bytes, Entry + 4, static_cast<uint32>(Sizes[Mip]));
			Put32(Bytes, Entry + 8, static_cast<uint32>(Bytes.Num()));
			Put32(Bytes, Entry + 12, static_cast<uint32>(Mips[Mip].Num()));
			if (bPacked)
			{
				Put32(Bytes, Entry + 16, static_cast<uint32>(Stored.Num()));
			}
			Bytes.Append(Stored);
		}
		return Bytes;
	}

	FString FixturePath(const TCHAR* Name)
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("../schema/fixtures"), Name));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCookedPackingEnvelopeTest,
	"Mantle.Runtime.CookedPacking.Envelope",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCookedPackingEnvelopeTest::RunTest(const FString&)
{
	using namespace MantleCookedPackingTest;

	TArray<uint8> Inner;
	for (int32 Byte = 0; Byte < 5000; ++Byte)
	{
		Inner.Add(static_cast<uint8>((Byte * 7) % 13));
	}
	const TArray<uint8> Good = Envelope("MRM3", Inner);
	TArray<uint8> Out;
	FString Reason;
	TestTrue(TEXT("an envelope inflates"), MantleCookedPacking::UnpackEnvelope(Good, "MRM3", 1 << 20, Out, &Reason));
	TestTrue(TEXT("to exactly its inner file"), Out == Inner);

	const auto Damaged = [&Good](const TFunctionRef<void(TArray<uint8>&)> Change)
	{
		TArray<uint8> Bytes = Good;
		Change(Bytes);
		return Bytes;
	};
	const TPair<const TCHAR*, TArray<uint8>> Cases[] = {
		{ TEXT("a flipped byte"), Damaged([](TArray<uint8>& B) { B[B.Num() / 2] ^= 0x5A; }) },
		{ TEXT("a bad checksum"), Damaged([](TArray<uint8>& B) { B.Last() ^= 1; }) },
		{ TEXT("truncated"), Damaged([](TArray<uint8>& B) { B.SetNum(B.Num() - 2); }) },
		{ TEXT("bytes after the stream"), Damaged([](TArray<uint8>& B) { B.Add(0); }) },
		{ TEXT("a size a byte too big"), Damaged([&Inner](TArray<uint8>& B) { Put32(B, 4, static_cast<uint32>(Inner.Num() + 1)); }) },
		{ TEXT("a size a byte too small"), Damaged([&Inner](TArray<uint8>& B) { Put32(B, 4, static_cast<uint32>(Inner.Num() - 1)); }) },
		{ TEXT("a size of nothing"), Damaged([](TArray<uint8>& B) { Put32(B, 4, 0); }) },
		{ TEXT("a size past the cap"), Damaged([](TArray<uint8>& B) { Put32(B, 4, (1u << 20) + 1); }) },
		{ TEXT("another magic"), Damaged([](TArray<uint8>& B) { B[3] = 'X'; }) },
		{ TEXT("only a header"), Damaged([](TArray<uint8>& B) { B.SetNum(7); }) },
	};
	for (const TPair<const TCHAR*, TArray<uint8>>& Case : Cases)
	{
		Reason.Reset();
		TestFalse(*FString::Printf(TEXT("%s is refused"), Case.Key), MantleCookedPacking::UnpackEnvelope(Case.Value, "MRM3", 1 << 20, Out, &Reason));
		TestTrue(*FString::Printf(TEXT("%s leaves nothing and says why (%s)"), Case.Key, *Reason), Out.IsEmpty() && !Reason.IsEmpty());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCookedPackingTextureTest,
	"Mantle.Runtime.TextureBuilder.Mtx2",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCookedPackingTextureTest::RunTest(const FString&)
{
	using namespace MantleCookedPackingTest;

	FMantleTextureBuildParams Params;
	FMantleDecodedTexture Packed;
	FString Error;
	TestTrue(*FString::Printf(TEXT("an MTX2 decodes (%s)"), *Error), FMantleRuntimeTextureBuilder::Decode(Texture(true), Params, Packed, &Error));
	TestEqual(TEXT("BC1"), Packed.Format, PF_DXT1);
	TestTrue(TEXT("sRGB color"), Packed.bSRGB && Packed.Role == EMantleTextureRole::Color);
	const TArray<TArray<uint8>> Blocks = TextureMips();
	if (TestEqual(TEXT("the whole chain"), Packed.Mips.Num(), 4))
	{
		for (int32 Mip = 0; Mip < 4; ++Mip)
		{
			TestTrue(*FString::Printf(TEXT("mip %d inflates to its blocks"), Mip), Packed.Mips[Mip].Width == (8 >> Mip)
				&& Packed.Mips[Mip].Bytes.Num() == Blocks[Mip].Num() && FMemory::Memcmp(Packed.Mips[Mip].Bytes.GetData(), Blocks[Mip].GetData(), Blocks[Mip].Num()) == 0);
		}
	}
	FMantleDecodedTexture Old;
	Error.Reset();
	TestFalse(TEXT("an MTX1, cooked before, is refused"), FMantleRuntimeTextureBuilder::Decode(Texture(false), Params, Old, &Error));
	TestTrue(*FString::Printf(TEXT("as one to publish again (%s)"), *Error), Old.Mips.IsEmpty() && Error.Contains(TEXT("published again")));
	FMantleTextureBuildParams TopOnly;
	TopOnly.Mips = EMantleTextureMips::None;
	FMantleDecodedTexture One;
	TestTrue(TEXT("the top mip alone decodes"), FMantleRuntimeTextureBuilder::Decode(Texture(true), TopOnly, One, &Error) && One.Mips.Num() == 1);

	const TArray<uint8> Good = Texture(true);
	const int32 Mip0At = 36 + 4 * 20;
	const int32 Mip0Packed = static_cast<int32>(Good[36 + 16]) | (static_cast<int32>(Good[36 + 17]) << 8);
	const auto Damaged = [&Good](const TFunctionRef<void(TArray<uint8>&)> Change)
	{
		TArray<uint8> Bytes = Good;
		Change(Bytes);
		return Bytes;
	};
	const TPair<const TCHAR*, TArray<uint8>> Cases[] = {
		{ TEXT("a bad checksum in mip 0"), Damaged([&](TArray<uint8>& B) { B[Mip0At + Mip0Packed - 1] ^= 1; }) },
		{ TEXT("a flipped byte in mip 0"), Damaged([&](TArray<uint8>& B) { B[Mip0At + 3] ^= 0x5A; }) },
		{ TEXT("truncated"), Damaged([](TArray<uint8>& B) { B.SetNum(B.Num() - 1); }) },
		{ TEXT("bytes after the last mip"), Damaged([](TArray<uint8>& B) { B.Add(0); }) },
		{ TEXT("an empty stream"), Damaged([](TArray<uint8>& B) { Put32(B, 36 + 16, 0); }) },
	};
	for (const TPair<const TCHAR*, TArray<uint8>>& Case : Cases)
	{
		FMantleDecodedTexture Out;
		Error.Reset();
		TestFalse(*FString::Printf(TEXT("%s is refused"), Case.Key), FMantleRuntimeTextureBuilder::Decode(Case.Value, Params, Out, &Error));
		TestTrue(*FString::Printf(TEXT("%s leaves nothing and says why (%s)"), Case.Key, *Error), Out.Mips.IsEmpty() && Error.Contains(TEXT("MTX2")));
	}

	// The cook's own MTX2 (backend/internal/cook/texturecook_test.go writes it).
	TArray<uint8> Fixture;
	const FString Path = FixturePath(TEXT("texture-v1.mtx"));
	if (TestTrue(*FString::Printf(TEXT("the texture fixture exists (%s)"), *Path), FFileHelper::LoadFileToArray(Fixture, *Path)))
	{
		FMantleDecodedTexture FromCook;
		TestTrue(TEXT("the fixture is an MTX2"), MantleCookedPacking::HasMagic(Fixture, "MTX2"));
		TestTrue(*FString::Printf(TEXT("and decodes (%s)"), *Error), FMantleRuntimeTextureBuilder::Decode(Fixture, Params, FromCook, &Error));
		TestEqual(TEXT("with its full chain"), FromCook.Mips.Num(), FMantleRuntimeTextureBuilder::ComputeFullMipCount(FromCook.GetWidth(), FromCook.GetHeight()));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCookedPackingMeshAndLumenTest,
	"Mantle.Runtime.StaticMesh.PackedFormats",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCookedPackingMeshAndLumenTest::RunTest(const FString&)
{
	using namespace MantleCookedPackingTest;

	TestEqual(TEXT("an MRM3 inflates to at most cook.StaticMeshMaxRawBytes"), FMantleStaticMeshReader::MaxPackedRawBytes, 512ll * 1024 * 1024);

	TArray<uint8> Mrm2;
	const FString Path = FixturePath(TEXT("runtime-mesh-v2.mrm"));
	if (TestTrue(*FString::Printf(TEXT("the mesh fixture exists (%s)"), *Path), FFileHelper::LoadFileToArray(Mrm2, *Path)))
	{
		const TArray<uint8> Mrm3 = Envelope("MRM3", Mrm2);
		TestTrue(TEXT("MRM3 is a static mesh's magic"), FMantleStaticMeshReader::HasMagic(Mrm3));
		TestFalse(TEXT("a bare MRM2 is not"), FMantleStaticMeshReader::HasMagic(Mrm2));
		FMantleStaticMeshData FromRaw;
		FMantleStaticMeshData FromPacked;
		FString Error;
		TestTrue(*FString::Printf(TEXT("the fixture's MRM2 reads as an MRM3's inside (%s)"), *Error), FMantleStaticMeshReader::ReadInner(Mrm2, FromRaw, &Error));
		TestTrue(*FString::Printf(TEXT("the MRM3 reads (%s)"), *Error), FMantleStaticMeshReader::Read(Mrm3, FromPacked, &Error));
		TestTrue(TEXT("to the same mesh"), FromPacked.Positions == FromRaw.Positions && FromPacked.Indices == FromRaw.Indices && FromPacked.Sections.Num() == FromRaw.Sections.Num());
		FMantleStaticMeshData OldMesh;
		Error.Reset();
		TestFalse(TEXT("a bare MRM2, cooked before, is refused"), FMantleStaticMeshReader::Read(Mrm2, OldMesh, &Error));
		TestTrue(*FString::Printf(TEXT("as one to publish again (%s)"), *Error), Error.Contains(TEXT("published again")));

		TArray<uint8> Damaged = Mrm3;
		Damaged.Last() ^= 1;
		FMantleStaticMeshData Out;
		TestFalse(TEXT("a damaged MRM3 is refused"), FMantleStaticMeshReader::Read(Damaged, Out, &Error));
		TestTrue(*FString::Printf(TEXT("as damaged (%s)"), *Error), Error.Contains(TEXT("damaged")));
		TestFalse(TEXT("an MRM3 inside an MRM3 is refused"), FMantleStaticMeshReader::Read(Envelope("MRM3", Mrm3), Out, &Error));
	}

	// An MLD1 header with no body, in an MLD2: Unpack gives the MLD1 back, whose header reads; Read takes only the MLD1, so nothing inflates in it.
	const FString Mesh = TEXT("sha256:") + FString::ChrN(64, TEXT('a'));
	TArray<uint8> Mld1;
	FMemoryWriter Writer(Mld1);
	Mld1.Append(reinterpret_cast<const uint8*>("MLD1"), 4);
	Writer.Seek(4);
	uint32 Version = 1;
	FString Engine = FMantleMeshLumenFile::EngineTag();
	FString MeshTag = Mesh;
	Writer << Version << Engine << MeshTag;
	const TArray<uint8> Mld2 = Envelope("MLD2", Mld1);
	TestTrue(TEXT("an MLD2 is packed"), FMantleMeshLumenFile::IsPacked(Mld2) && !FMantleMeshLumenFile::IsPacked(Mld1));
	TArray<uint8> Unpacked;
	FString Reason;
	TestTrue(*FString::Printf(TEXT("an MLD2 unpacks (%s)"), *Reason), FMantleMeshLumenFile::Unpack(Mld2, Unpacked, &Reason));
	TestTrue(TEXT("to its MLD1"), Unpacked == Mld1);
	FString ReadEngine;
	FString ReadMesh;
	TestTrue(TEXT("whose header reads"), FMantleMeshLumenFile::ReadHeader(Unpacked, ReadEngine, ReadMesh));
	TestTrue(TEXT("as written"), ReadEngine == Engine && ReadMesh == Mesh);
	FMantleMeshLumenData Lumen;
	TestFalse(TEXT("Read does not take an MLD2"), FMantleMeshLumenFile::Read(Mld2, Mesh, Lumen, &Reason));
	TestFalse(TEXT("nor ReadHeader"), FMantleMeshLumenFile::ReadHeader(Mld2, ReadEngine, ReadMesh));
	TArray<uint8> Damaged = Mld2;
	Damaged.Last() ^= 1;
	Reason.Reset();
	TestFalse(TEXT("a damaged MLD2 does not unpack"), FMantleMeshLumenFile::Unpack(Damaged, Unpacked, &Reason));
	TestTrue(*FString::Printf(TEXT("as damaged (%s)"), *Reason), Unpacked.IsEmpty() && Reason.Contains(TEXT("damaged")));
	TestFalse(TEXT("an MLD1 is not unpacked"), FMantleMeshLumenFile::Unpack(Mld1, Unpacked, &Reason));
	return true;
}

#endif
