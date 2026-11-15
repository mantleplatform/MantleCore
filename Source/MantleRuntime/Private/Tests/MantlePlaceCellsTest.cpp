// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCellLoader.h"
#include "MantlePlaceCells.h"
#include "MantlePlaceParser.h"

#include "Misc/Compression.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// Cells (stage 1 of large places): the place file's "cells" key, the two containers, the index, a cell file's entities and the merge.
// Expected values are worked out by hand from the contract (D:\Mantle\Untracked\local\build-plan\contracts\cells-format.md).

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantlePlaceCellsTestPrivate
{
	FString CellTestId(const uint32 Number)
	{
		return FString::Printf(TEXT("ast_u%032x"), Number);
	}

	const FString CellMeshId = CellTestId(1);
	const FString CellMaterialId = CellTestId(2);
	const FString CellIndexAssetId = CellTestId(3);
	const FString CellFileA = CellTestId(0x10);
	const FString CellFileB = CellTestId(0x11);
	const FString CellFileC = CellTestId(0x12);

	/** A container the way the plugin writes it: the magic, the inflated size, one zlib stream. */
	TArray<uint8> CellMakeContainer(const char* Magic, const FString& Json, const int64 SizeWord = -1)
	{
		const FTCHARToUTF8 Utf8(*Json);
		TArray<uint8> Packed;
		Packed.SetNumUninitialized(FCompression::CompressMemoryBound(NAME_Zlib, Utf8.Length()));
		int32 PackedSize = Packed.Num();
		verify(FCompression::CompressMemory(NAME_Zlib, Packed.GetData(), PackedSize, Utf8.Get(), Utf8.Length(), COMPRESS_BiasMemory));
		const uint32 RawBytes = SizeWord < 0 ? static_cast<uint32>(Utf8.Length()) : static_cast<uint32>(SizeWord);
		TArray<uint8> Container;
		Container.Append(reinterpret_cast<const uint8*>(Magic), 4);
		Container.Append(reinterpret_cast<const uint8*>(&RawBytes), sizeof(RawBytes));
		Container.Append(Packed.GetData(), PackedSize);
		return Container;
	}

	/** The root place: the mesh and material the cells name, the index, and one root entity. */
	FString CellRootJson(const FString& CellKeyJson = FString(), const FString& ExtraAssets = FString())
	{
		FString Json = TEXT(R"({"schemaVersion":1,"placeId":"plc_cells0001","revision":{"number":1,"state":"draft"},
			"metadata":{"name":"Cells","authorId":"usr_4b1d"},"settings":{},
			"assets":[
				{"id":"$MESH","kind":"StaticMesh","hash":"sha256:aaaa","bytes":100},
				{"id":"$MATERIAL","kind":"Material","hash":"sha256:bbbb","bytes":100}$INDEX$EXTRA],
			$CELLS
			"entities":[{"id":"ent_root_1","type":"StaticMesh","transform":{"location":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1]},
				"properties":{"mesh":"$MESH"}}]})");
		const bool bWithCells = CellKeyJson.Len() > 0;
		Json = Json.Replace(TEXT("$MESH"), *CellMeshId).Replace(TEXT("$MATERIAL"), *CellMaterialId);
		Json = Json.Replace(TEXT("$INDEX"), bWithCells ? *FString::Printf(TEXT(",{\"id\":\"%s\",\"kind\":\"PlaceCellIndex\",\"hash\":\"sha256:cccc\",\"bytes\":50}"), *CellIndexAssetId) : TEXT(""));
		Json = Json.Replace(TEXT("$EXTRA"), *ExtraAssets);
		Json = Json.Replace(TEXT("$CELLS"), bWithCells ? *CellKeyJson : TEXT(""));
		return Json;
	}

	FString CellKeyJson()
	{
		return FString::Printf(TEXT("\"cells\":{\"index\":\"%s\"},"), *CellIndexAssetId);
	}

	FMantlePlace CellParseRoot(const FString& Json, TArray<FMantleParseError>* OutErrors = nullptr)
	{
		const FMantleParseResult Parsed = FMantlePlaceParser::ParseFromString(Json);
		if (OutErrors)
		{
			*OutErrors = Parsed.Errors;
		}
		return Parsed.Place;
	}

	/** One cell of the index: key, bounds from (0,0,0) to (100,100,100), draw distance, one file, and the counts. */
	FString CellEntryJson(const int32 Level, const int32 X, const int32 Y, const FString& File, const int32 Entities, const int32 Instances, const int32 Solid, const double DrawDistance = 9000.0)
	{
		return FString::Printf(
			TEXT("{\"level\":%d,\"x\":%d,\"y\":%d,\"bounds\":{\"min\":[0,0,0],\"max\":[100,100,100]},\"drawDistance\":%g,"
				 "\"files\":[{\"id\":\"%s\",\"hash\":\"sha256:dddd\",\"bytes\":10}],\"counts\":{\"entities\":%d,\"instances\":%d,\"solid\":%d}}"),
			Level, X, Y, DrawDistance, *File, Entities, Instances, Solid);
	}

	FString CellIndexJson(const FString& CellsArray, const FString& Version = TEXT("1"), const FString& Extra = FString())
	{
		return FString::Printf(
			TEXT("{\"version\":%s,\"grid\":{\"levels\":[{\"cellSize\":6400,\"maxDrawDistance\":15000},{\"cellSize\":25600,\"maxDrawDistance\":60000}]},\"cells\":[%s]%s}"),
			*Version, *CellsArray, *Extra);
	}

	FString CellStaticMeshJson(const FString& EntityId, const FString& ExtraProps = FString(), const FString& Extra = FString())
	{
		return FString::Printf(
			TEXT("{\"id\":\"%s\",\"type\":\"StaticMesh\",\"name\":\"Barrel\",\"transform\":{\"location\":[100,200,0],\"rotation\":[0,90,0],\"scale\":[1,1,1]},"
				 "\"properties\":{\"mesh\":\"%s\",\"materials\":[\"%s\",null],\"drawDistance\":6000%s}%s}"),
			*EntityId, *CellMeshId, *CellMaterialId, *ExtraProps, *Extra);
	}

	FString CellInstancedJson(const FString& EntityId, const FString& Transform, const FString& Collision = TEXT("none"))
	{
		return FString::Printf(
			TEXT("{\"id\":\"%s\",\"type\":\"InstancedMesh\",\"transform\":%s,"
				 "\"properties\":{\"mesh\":\"%s\",\"drawDistance\":9000,\"collision\":\"%s\",\"instances\":[[1,2,3,0,90,0,1,1,1],[4,5,6,0,0,0,1,1,1],[7,8,9,0,0,0,1,1,1]]}}"),
			*EntityId, *Transform, *CellMeshId, *Collision);
	}

	const FString CellAnchorOnly = TEXT("{\"location\":[80012,-16034,410],\"rotation\":[0,0,0],\"scale\":[1,1,1]}");

	FString CellFileDocJson(const FString& Entities, const FString& Version = TEXT("1"))
	{
		return FString::Printf(TEXT("{\"version\":%s,\"entities\":[%s]}"), *Version, *Entities);
	}

	bool CellReadIndex(const FString& Json, FMantleCellIndex& Out, FString* OutError = nullptr)
	{
		FString Error;
		const bool bOk = FMantlePlaceCells::ReadIndex(Json, Out, Error);
		if (OutError)
		{
			*OutError = Error;
		}
		return bOk;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceCellsParserTest,
	"Mantle.Runtime.Place.Cells.RootKey",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceCellsParserTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceCellsTestPrivate;
	TArray<FMantleParseError> Errors;

	// A place without cells is read exactly as before.
	FMantlePlace Whole = CellParseRoot(CellRootJson(), &Errors);
	TestEqual(TEXT("a place without cells has no errors"), Errors.Num(), 0);
	TestTrue(TEXT("and no cell index"), Whole.CellIndexId.IsEmpty());
	TestEqual(TEXT("and its entity"), Whole.Entities.Num(), 1);

	// The cells key and its manifest entry.
	FMantlePlace WithCells = CellParseRoot(CellRootJson(CellKeyJson()), &Errors);
	TestEqual(TEXT("a place with cells has no errors"), Errors.Num(), 0);
	TestEqual(TEXT("names its index"), WithCells.CellIndexId, CellIndexAssetId);
	const FMantleAssetRef* Entry = WithCells.Assets.FindByPredicate([](const FMantleAssetRef& Asset) { return Asset.Kind == EMantleAssetKind::PlaceCellIndex; });
	TestNotNull(TEXT("the manifest lists the PlaceCellIndex entry"), Entry);
	TestFalse(TEXT("cells are not merged until the loader has read them"), WithCells.bCellsMerged);

	// cells without its manifest entry, and the entry without cells.
	FString Missing = CellRootJson(CellKeyJson()).Replace(TEXT("\"kind\":\"PlaceCellIndex\""), TEXT("\"kind\":\"Material\""));
	CellParseRoot(Missing, &Errors);
	TestTrue(TEXT("cells.index must name a PlaceCellIndex entry"), Errors.ContainsByPredicate([](const FMantleParseError& Error) { return Error.Path == TEXT("/cells/index"); }));
	CellParseRoot(CellRootJson(CellKeyJson()).Replace(*CellKeyJson(), TEXT("")), &Errors);
	TestTrue(TEXT("a PlaceCellIndex entry needs the cells key"), Errors.ContainsByPredicate([](const FMantleParseError& Error) { return Error.Path == TEXT("/assets"); }));
	FString OtherIndex = FString::Printf(TEXT(",{\"id\":\"%s\",\"kind\":\"PlaceCellIndex\",\"hash\":\"sha256:eeee\",\"bytes\":50}"), *CellTestId(4));
	CellParseRoot(CellRootJson(CellKeyJson(), OtherIndex), &Errors);
	TestTrue(TEXT("a second PlaceCellIndex entry is refused"), Errors.Num() > 0);

	// The key's own shape.
	CellParseRoot(CellRootJson(TEXT("\"cells\":null,")), &Errors);
	TestTrue(TEXT("cells: null is refused"), Errors.ContainsByPredicate([](const FMantleParseError& Error) { return Error.Path == TEXT("/cells"); }));
	CellParseRoot(CellRootJson(FString::Printf(TEXT("\"cells\":{\"index\":\"%s\",\"extra\":1},"), *CellIndexAssetId)), &Errors);
	TestTrue(TEXT("an unknown key under cells is refused"), Errors.Num() > 0);
	CellParseRoot(CellRootJson(TEXT("\"cells\":{\"index\":\"ast_cube\"},")), &Errors);
	TestTrue(TEXT("a cells.index that is not an uploaded id is refused"), Errors.ContainsByPredicate([](const FMantleParseError& Error) { return Error.Path == TEXT("/cells/index"); }));

	// The entry takes no other keys: the rules for lumen, nanite, engine and sockets already say who may carry them.
	FString WithEngine = CellRootJson(CellKeyJson()).Replace(TEXT("\"bytes\":50}"), TEXT("\"bytes\":50,\"engine\":\"x\"}"));
	CellParseRoot(WithEngine, &Errors);
	TestTrue(TEXT("only a Lumen or Nanite entry says its engine"), Errors.Num() > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceCellsContainerTest,
	"Mantle.Runtime.Place.Cells.Container",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceCellsContainerTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceCellsTestPrivate;
	// The engine logs an error for every stream it cannot inflate, which is what these refusals are made of.
	AddExpectedError(TEXT("Failed to uncompress memory"), EAutomationExpectedErrorFlags::Contains, 0);
	const FString Json = TEXT("{\"version\":1,\"entities\":[]}");
	const TArray<uint8> Good = CellMakeContainer("MCF1", Json);
	TArray<uint8> Out;
	FString Reason;

	TestTrue(TEXT("a good container unpacks"), FMantlePlaceCells::Unpack(Good, FMantlePlaceCells::CellMagic, FMantlePlaceCells::MaxCellInflatedBytes, Out, &Reason));
	FString Text;
	TestTrue(TEXT("and is UTF-8 text"), FMantlePlaceCells::ToText(Out, Text));
	TestEqual(TEXT("the text is the document"), Text, Json);

	// The size word is held before anything is allocated.
	TestFalse(TEXT("a size of 0 is refused"), FMantlePlaceCells::Unpack(CellMakeContainer("MCF1", Json, 0), FMantlePlaceCells::CellMagic, FMantlePlaceCells::MaxCellInflatedBytes, Out, &Reason));
	TestTrue(TEXT("and leaves nothing allocated"), Out.IsEmpty());
	TestFalse(TEXT("a cell file of 8 MiB + 1 is refused"), FMantlePlaceCells::Unpack(CellMakeContainer("MCF1", Json, FMantlePlaceCells::MaxCellInflatedBytes + 1), FMantlePlaceCells::CellMagic, FMantlePlaceCells::MaxCellInflatedBytes, Out, &Reason));
	TestTrue(TEXT("the refusal says why"), Reason.Contains(TEXT("inflates to")));
	TestTrue(TEXT("and nothing is allocated"), Out.IsEmpty());
	TestFalse(TEXT("4 GiB less one is refused"), FMantlePlaceCells::Unpack(CellMakeContainer("MCF1", Json, MAX_uint32), FMantlePlaceCells::CellMagic, FMantlePlaceCells::MaxCellInflatedBytes, Out, &Reason));
	TestTrue(TEXT("an index may be 8 MiB + 1 (its cap is 64 MiB) when the stream agrees"),
		!FMantlePlaceCells::Unpack(CellMakeContainer("MCI1", Json, FMantlePlaceCells::MaxCellInflatedBytes + 1), FMantlePlaceCells::IndexMagic, FMantlePlaceCells::MaxIndexInflatedBytes, Out, &Reason)
		&& Reason.Contains(TEXT("damaged")));
	TestFalse(TEXT("an index of 64 MiB + 1 is refused"), FMantlePlaceCells::Unpack(CellMakeContainer("MCI1", Json, FMantlePlaceCells::MaxIndexInflatedBytes + 1), FMantlePlaceCells::IndexMagic, FMantlePlaceCells::MaxIndexInflatedBytes, Out, &Reason));

	// A size that is not the stream's, either way, is damage.
	TestFalse(TEXT("a size larger than the stream inflates to"), FMantlePlaceCells::Unpack(CellMakeContainer("MCF1", Json, Json.Len() + 1), FMantlePlaceCells::CellMagic, FMantlePlaceCells::MaxCellInflatedBytes, Out, &Reason));
	TestFalse(TEXT("a size smaller than the stream inflates to"), FMantlePlaceCells::Unpack(CellMakeContainer("MCF1", Json, Json.Len() - 1), FMantlePlaceCells::CellMagic, FMantlePlaceCells::MaxCellInflatedBytes, Out, &Reason));

	// Other files are not this container.
	TestFalse(TEXT("an index container is not a cell file"), FMantlePlaceCells::Unpack(CellMakeContainer("MCI1", Json), FMantlePlaceCells::CellMagic, FMantlePlaceCells::MaxCellInflatedBytes, Out, &Reason));
	TestFalse(TEXT("an empty file"), FMantlePlaceCells::Unpack(TArray<uint8>(), FMantlePlaceCells::CellMagic, FMantlePlaceCells::MaxCellInflatedBytes, Out, &Reason));
	TestFalse(TEXT("a header alone"), FMantlePlaceCells::Unpack(TConstArrayView<uint8>(Good.GetData(), FMantlePlaceCells::ContainerHeaderBytes), FMantlePlaceCells::CellMagic, FMantlePlaceCells::MaxCellInflatedBytes, Out, &Reason));
	TArray<uint8> Cut = Good;
	Cut.SetNum(Cut.Num() - 4);
	TestFalse(TEXT("a cut stream"), FMantlePlaceCells::Unpack(Cut, FMantlePlaceCells::CellMagic, FMantlePlaceCells::MaxCellInflatedBytes, Out, &Reason));
	TArray<uint8> Damaged = Good;
	Damaged[Damaged.Num() / 2] ^= 0x5A;
	TestFalse(TEXT("a damaged stream"), FMantlePlaceCells::Unpack(Damaged, FMantlePlaceCells::CellMagic, FMantlePlaceCells::MaxCellInflatedBytes, Out, &Reason));
	TestTrue(TEXT("a refusal leaves no bytes behind"), Out.IsEmpty());

	// UTF-8 with a byte order mark is not the document.
	const uint8 Bom[] = { 0xEF, 0xBB, 0xBF, '{', '}' };
	TestFalse(TEXT("a byte order mark is refused"), FMantlePlaceCells::ToText(MakeArrayView(Bom), Text, &Reason));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceCellsIndexTest,
	"Mantle.Runtime.Place.Cells.Index",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceCellsIndexTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceCellsTestPrivate;
	FMantleCellIndex Index;
	FString Error;

	// A good index, with a cell on each level and a negative coordinate.
	const FString Good = CellIndexJson(CellEntryJson(0, -3, 12, CellFileA, 5, 12, 4) + TEXT(",") + CellEntryJson(0, 2, 0, CellFileB, 1, 0, 1) + TEXT(",") + CellEntryJson(1, 0, 0, CellFileC, 7, 100, 0, 50000.0));
	if (!TestTrue(TEXT("a good index reads"), CellReadIndex(Good, Index, &Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("version"), Index.Version, 1);
	TestEqual(TEXT("two levels"), Index.Levels.Num(), 2);
	TestEqual(TEXT("level 0 cell size"), Index.Levels[0].CellSize, static_cast<int64>(6400));
	TestEqual(TEXT("level 1 max draw distance"), Index.Levels[1].MaxDrawDistance, 60000.0);
	TestEqual(TEXT("three cells"), Index.Cells.Num(), 3);
	TestEqual(TEXT("x of the first"), Index.Cells[0].X, -3);
	TestEqual(TEXT("y of the first"), Index.Cells[0].Y, 12);
	TestEqual(TEXT("its file"), Index.Cells[0].Files[0].Id, CellFileA);
	TestEqual(TEXT("its file's bytes"), Index.Cells[0].Files[0].Bytes, static_cast<int64>(10));
	TestEqual(TEXT("its entities"), Index.Cells[0].Entities, static_cast<int64>(5));
	TestEqual(TEXT("its instances"), Index.Cells[0].Instances, static_cast<int64>(12));
	TestEqual(TEXT("its solid count"), Index.Cells[0].Solid, static_cast<int64>(4));
	TestEqual(TEXT("its bounds"), Index.Cells[0].Bounds.Max, FVector(100.0, 100.0, 100.0));
	TestEqual(TEXT("the third cell's level"), Index.Cells[2].Level, 1);
	TestEqual(TEXT("its draw distance"), Index.Cells[2].DrawDistance, 50000.0);

	// An index with no cells is a valid (empty) index.
	TestTrue(TEXT("an empty list of cells reads"), CellReadIndex(CellIndexJson(FString()), Index, &Error));
	TestEqual(TEXT("with no cells"), Index.Cells.Num(), 0);

	const auto Refused = [this, &Index](const TCHAR* What, const FString& Json, const TCHAR* Mentions)
	{
		FString Why;
		const bool bRead = CellReadIndex(Json, Index, &Why);
		TestFalse(What, bRead);
		TestTrue(FString::Printf(TEXT("%s: the reason names it (%s)"), What, *Why), Why.Contains(Mentions));
		TestTrue(FString::Printf(TEXT("%s: nothing is kept"), What), Index.Cells.IsEmpty());
	};

	const FString One = CellEntryJson(0, 0, 0, CellFileA, 1, 0, 0);
	Refused(TEXT("not JSON"), TEXT("{"), TEXT("JSON"));
	Refused(TEXT("not an object"), TEXT("[]"), TEXT("object"));
	Refused(TEXT("text after the document"), CellIndexJson(One) + TEXT(" {}"), TEXT("after"));
	Refused(TEXT("a newer version"), CellIndexJson(One, TEXT("2")), TEXT("version 2"));
	Refused(TEXT("version 0"), CellIndexJson(One, TEXT("0")), TEXT("version"));
	Refused(TEXT("a key of a later stage (far)"), CellIndexJson(One, TEXT("1"), TEXT(",\"far\":[]")), TEXT("/far"));
	Refused(TEXT("a key of a later stage (terrain)"), CellIndexJson(One, TEXT("1"), TEXT(",\"terrain\":{}")), TEXT("/terrain"));
	Refused(TEXT("a key given twice"), CellIndexJson(One, TEXT("1"), TEXT(",\"version\":1")), TEXT("/version"));
	Refused(TEXT("no cells key"), TEXT("{\"version\":1,\"grid\":{\"levels\":[{\"cellSize\":6400,\"maxDrawDistance\":15000}]}}"), TEXT("needs"));
	Refused(TEXT("no grid"), TEXT("{\"version\":1,\"cells\":[]}"), TEXT("needs"));
	Refused(TEXT("no levels"), TEXT("{\"version\":1,\"grid\":{\"levels\":[]},\"cells\":[]}"), TEXT("level"));
	Refused(TEXT("five levels"), TEXT("{\"version\":1,\"grid\":{\"levels\":[{\"cellSize\":1000,\"maxDrawDistance\":1},{\"cellSize\":1000,\"maxDrawDistance\":2},{\"cellSize\":1000,\"maxDrawDistance\":3},{\"cellSize\":1000,\"maxDrawDistance\":4},{\"cellSize\":1000,\"maxDrawDistance\":5}]},\"cells\":[]}"), TEXT("levels"));
	Refused(TEXT("a cell size of 999"), TEXT("{\"version\":1,\"grid\":{\"levels\":[{\"cellSize\":999,\"maxDrawDistance\":15000}]},\"cells\":[]}"), TEXT("cellSize"));
	Refused(TEXT("a cell size that is not whole"), TEXT("{\"version\":1,\"grid\":{\"levels\":[{\"cellSize\":1000.5,\"maxDrawDistance\":15000}]},\"cells\":[]}"), TEXT("cellSize"));
	Refused(TEXT("a draw distance past the cap"), TEXT("{\"version\":1,\"grid\":{\"levels\":[{\"cellSize\":6400,\"maxDrawDistance\":1000001}]},\"cells\":[]}"), TEXT("maxDrawDistance"));
	Refused(TEXT("levels that do not rise"), TEXT("{\"version\":1,\"grid\":{\"levels\":[{\"cellSize\":6400,\"maxDrawDistance\":15000},{\"cellSize\":25600,\"maxDrawDistance\":15000}]},\"cells\":[]}"), TEXT("above the level before"));
	Refused(TEXT("a cell on a level the grid lacks"), CellIndexJson(CellEntryJson(2, 0, 0, CellFileA, 1, 0, 0)), TEXT("level"));
	Refused(TEXT("a cell out of order"), CellIndexJson(CellEntryJson(0, 5, 0, CellFileA, 1, 0, 0) + TEXT(",") + CellEntryJson(0, 2, 0, CellFileB, 1, 0, 0)), TEXT("order"));
	Refused(TEXT("a cell repeated"), CellIndexJson(CellEntryJson(0, 0, 0, CellFileA, 1, 0, 0) + TEXT(",") + CellEntryJson(0, 0, 0, CellFileB, 1, 0, 0)), TEXT("repeats"));
	Refused(TEXT("a file in two cells"), CellIndexJson(CellEntryJson(0, 0, 0, CellFileA, 1, 0, 0) + TEXT(",") + CellEntryJson(0, 1, 0, CellFileA, 1, 0, 0)), TEXT("more than once"));
	Refused(TEXT("a cell coordinate past 1,000,000"), CellIndexJson(CellEntryJson(0, 1000001, 0, CellFileA, 1, 0, 0)), TEXT("/x"));
	Refused(TEXT("a draw distance above its level's"), CellIndexJson(CellEntryJson(0, 0, 0, CellFileA, 1, 0, 0, 15001.0)), TEXT("drawDistance"));
	Refused(TEXT("a draw distance of 0"), CellIndexJson(CellEntryJson(0, 0, 0, CellFileA, 1, 0, 0, 0.0)), TEXT("drawDistance"));
	Refused(TEXT("bounds with a min above its max"), CellIndexJson(One.Replace(TEXT("\"min\":[0,0,0]"), TEXT("\"min\":[0,0,500]"))), TEXT("bounds"));
	Refused(TEXT("bounds with two points"), CellIndexJson(One.Replace(TEXT("\"max\":[100,100,100]"), TEXT("\"max\":[100,100]"))), TEXT("max"));
	Refused(TEXT("a file id that is not uploaded"), CellIndexJson(One.Replace(*CellFileA, TEXT("ast_cube"))), TEXT("uploaded"));
	Refused(TEXT("a cell with no files"), CellIndexJson(One.Replace(*FString::Printf(TEXT("{\"id\":\"%s\",\"hash\":\"sha256:dddd\",\"bytes\":10}"), *CellFileA), TEXT(""))), TEXT("at least one"));
	Refused(TEXT("a cell key it does not know"), CellIndexJson(One.Replace(TEXT("\"counts\""), TEXT("\"far\":1,\"counts\""))), TEXT("far"));
	Refused(TEXT("a count that is negative"), CellIndexJson(One.Replace(TEXT("\"entities\":1"), TEXT("\"entities\":-1"))), TEXT("entities"));
	Refused(TEXT("a count that is not whole"), CellIndexJson(One.Replace(TEXT("\"entities\":1"), TEXT("\"entities\":1.5"))), TEXT("entities"));
	Refused(TEXT("a file with no bytes key"), CellIndexJson(One.Replace(TEXT(",\"bytes\":10"), TEXT(""))), TEXT("needs"));
	Refused(TEXT("a cell missing its counts"), CellIndexJson(One.Replace(TEXT(",\"counts\":{\"entities\":1,\"instances\":0,\"solid\":0}"), TEXT(""))), TEXT("needs"));

	// 65 files in one cell.
	FString Files;
	for (int32 File = 0; File < 65; ++File)
	{
		Files += FString::Printf(TEXT("%s{\"id\":\"%s\",\"hash\":\"sha256:dddd\",\"bytes\":10}"), File ? TEXT(",") : TEXT(""), *CellTestId(0x100 + File));
	}
	const FString Many = TEXT("{\"level\":0,\"x\":0,\"y\":0,\"bounds\":{\"min\":[0,0,0],\"max\":[1,1,1]},\"drawDistance\":9000,\"files\":[") + Files + TEXT("],\"counts\":{\"entities\":1,\"instances\":0,\"solid\":0}}");
	Refused(TEXT("65 files in one cell"), CellIndexJson(Many), TEXT("more than 64"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceCellsFileTest,
	"Mantle.Runtime.Place.Cells.CellFile",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceCellsFileTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceCellsTestPrivate;
	const FMantlePlace Root = CellParseRoot(CellRootJson(CellKeyJson()));
	const TMap<FString, EMantleAssetKind> Assets = FMantlePlaceCells::RootAssetKinds(Root);
	TestEqual(TEXT("the root's assets by kind"), Assets.Num(), 3);

	FMantleCellContent Content;
	TArray<FMantleParseError> Errors;

	// A StaticMesh (blocking by default) and an InstancedMesh of three instances with collision none.
	const FString Good = CellFileDocJson(CellStaticMeshJson(TEXT("ent_cell_a")) + TEXT(",") + CellInstancedJson(TEXT("ent_ism_b"), CellAnchorOnly));
	if (!TestTrue(TEXT("a good cell file reads"), FMantlePlaceCells::ReadCellFile(Good, Assets, Content, Errors)))
	{
		for (const FMantleParseError& Error : Errors)
		{
			AddError(Error.ToString());
		}
		return false;
	}
	TestEqual(TEXT("two entities"), Content.Entities.Num(), 2);
	TestEqual(TEXT("three instances"), Content.Instances, static_cast<int64>(3));
	TestEqual(TEXT("one solid thing: the StaticMesh (the instances have collision none)"), Content.Solid, static_cast<int64>(1));
	TestEqual(TEXT("the first entity's id"), Content.Entities[0].Id, FString(TEXT("ent_cell_a")));
	TestTrue(TEXT("a StaticMesh"), Content.Entities[0].Props.IsType<FMantleStaticMeshProps>());
	TestEqual(TEXT("its draw distance"), Content.Entities[0].Props.Get<FMantleStaticMeshProps>().DrawDistance, 6000.0);
	TestEqual(TEXT("the anchor of the second"), Content.Entities[1].Transform.GetLocation(), FVector(80012.0, -16034.0, 410.0));

	// solid counts the instances of a collision-bearing InstancedMesh, and a default collision is blocking.
	TestTrue(TEXT("a blocking InstancedMesh reads"), FMantlePlaceCells::ReadCellFile(CellFileDocJson(CellInstancedJson(TEXT("ent_ism_c"), CellAnchorOnly, TEXT("blocking"))), Assets, Content, Errors));
	TestEqual(TEXT("all three instances are solid"), Content.Solid, static_cast<int64>(3));

	const auto Refused = [this, &Assets, &Content](const TCHAR* What, const FString& Json, const TCHAR* Mentions)
	{
		TArray<FMantleParseError> Why;
		const bool bRead = FMantlePlaceCells::ReadCellFile(Json, Assets, Content, Why);
		TestFalse(What, bRead);
		FString All;
		for (const FMantleParseError& Error : Why)
		{
			All += Error.ToString() + TEXT("\n");
		}
		TestTrue(FString::Printf(TEXT("%s: the reason names it (%s)"), What, *All), All.Contains(Mentions));
		TestTrue(FString::Printf(TEXT("%s: nothing is kept"), What), Content.Entities.IsEmpty());
	};

	const FString Mesh = CellStaticMeshJson(TEXT("ent_cell_a"));
	Refused(TEXT("not JSON"), TEXT("{"), TEXT("JSON"));
	Refused(TEXT("a newer version"), CellFileDocJson(Mesh, TEXT("2")), TEXT("version 2"));
	Refused(TEXT("no entities"), CellFileDocJson(FString()), TEXT("at least one"));
	Refused(TEXT("another top-level key"), Good.Replace(TEXT("\"version\":1"), TEXT("\"version\":1,\"far\":[]")), TEXT("far"));
	Refused(TEXT("a Group"), CellFileDocJson(TEXT("{\"id\":\"ent_l\",\"type\":\"Group\",\"transform\":{\"location\":[0,0,0],\"rotation\":[0,0,0],\"scale\":[1,1,1]},\"properties\":{}}")), TEXT("StaticMesh and InstancedMesh"));
	Refused(TEXT("a parent"), CellFileDocJson(CellStaticMeshJson(TEXT("ent_cell_a"), FString(), TEXT(",\"parent\":\"ent_g\""))), TEXT("parent"));
	Refused(TEXT("tags"), CellFileDocJson(CellStaticMeshJson(TEXT("ent_cell_a"), FString(), TEXT(",\"tags\":[\"x\"]"))), TEXT("tags"));
	Refused(TEXT("dynamic"), CellFileDocJson(CellStaticMeshJson(TEXT("ent_cell_a"), FString(), TEXT(",\"dynamic\":true"))), TEXT("dynamic"));
	Refused(TEXT("a physics block"), CellFileDocJson(CellStaticMeshJson(TEXT("ent_cell_a"), TEXT(",\"physics\":{\"mass\":10}"))), TEXT("dynamic"));
	Refused(TEXT("no draw distance"), CellFileDocJson(Mesh.Replace(TEXT(",\"drawDistance\":6000"), TEXT(""))), TEXT("drawDistance"));
	Refused(TEXT("an InstancedMesh with a rotated anchor"), CellFileDocJson(CellInstancedJson(TEXT("ent_ism_b"), TEXT("{\"location\":[0,0,0],\"rotation\":[0,45,0],\"scale\":[1,1,1]}"))), TEXT("anchored at a location only"));
	Refused(TEXT("an InstancedMesh with a scaled anchor"), CellFileDocJson(CellInstancedJson(TEXT("ent_ism_b"), TEXT("{\"location\":[0,0,0],\"rotation\":[0,0,0],\"scale\":[2,1,1]}"))), TEXT("anchored at a location only"));
	Refused(TEXT("an anchor that is not a whole number of cm"), CellFileDocJson(CellInstancedJson(TEXT("ent_ism_b"), TEXT("{\"location\":[0.5,0,0],\"rotation\":[0,0,0],\"scale\":[1,1,1]}"))), TEXT("whole number"));
	Refused(TEXT("a mesh the place does not list"), CellFileDocJson(Mesh.Replace(*CellMeshId, *CellTestId(0x99))), TEXT("not in the place's assets"));
	Refused(TEXT("a mesh listed as a material"), CellFileDocJson(Mesh.Replace(*CellMeshId, *CellMaterialId)), TEXT("another kind"));
	Refused(TEXT("a material listed as a mesh"), CellFileDocJson(Mesh.Replace(*CellMaterialId, *CellMeshId)), TEXT("another kind"));
	Refused(TEXT("an entity key the entity reader does not know"), CellFileDocJson(CellStaticMeshJson(TEXT("ent_cell_a"), FString(), TEXT(",\"colour\":1"))), TEXT("colour"));

	// Built-in ids need no listing.
	TestTrue(TEXT("a built-in mesh id reads"), FMantlePlaceCells::ReadCellFile(CellFileDocJson(Mesh.Replace(*CellMeshId, TEXT("ast_cube")).Replace(*CellMaterialId, TEXT("ast_mat_white"))), Assets, Content, Errors));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceCellsMergeTest,
	"Mantle.Runtime.Place.Cells.Merge",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceCellsMergeTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceCellsTestPrivate;
	const FMantlePlace Root = CellParseRoot(CellRootJson(CellKeyJson()));
	const TMap<FString, EMantleAssetKind> Assets = FMantlePlaceCells::RootAssetKinds(Root);

	// Cell 0: one StaticMesh (solid); cell 1: an InstancedMesh of 3 blocking instances.
	FMantleCellIndex Index;
	FString Error;
	const FString IndexText = CellIndexJson(CellEntryJson(0, 0, 0, CellFileA, 1, 0, 1) + TEXT(",") + CellEntryJson(0, 1, 0, CellFileB, 1, 3, 3));
	if (!TestTrue(TEXT("the index reads"), CellReadIndex(IndexText, Index, &Error)))
	{
		AddError(Error);
		return false;
	}
	TArray<FMantleParseError> Errors;
	const auto Read = [&Assets, &Errors](const int32 CellIndex, const FString& Entities)
	{
		FMantleCellContent Content;
		verify(FMantlePlaceCells::ReadCellFile(CellFileDocJson(Entities), Assets, Content, Errors));
		Content.CellIndex = CellIndex;
		return Content;
	};
	const auto Contents = [&Read]()
	{
		TArray<FMantleCellContent> Cells;
		Cells.Add(Read(0, CellStaticMeshJson(TEXT("ent_cell_a"))));
		Cells.Add(Read(1, CellInstancedJson(TEXT("ent_ism_b"), CellAnchorOnly, TEXT("blocking"))));
		return Cells;
	};

	// Merged: the root's entity first, then each cell's, in the index's order.
	{
		FMantlePlace Place = Root;
		TArray<FMantleCellContent> Cells = Contents();
		TArray<FMantleParseError> MergeErrors;
		TestTrue(TEXT("the cells merge"), FMantlePlaceCells::Merge(Place, Index, Cells, MergeErrors));
		TestEqual(TEXT("three entities"), Place.Entities.Num(), 3);
		TestEqual(TEXT("the root's first"), Place.Entities[0].Id, FString(TEXT("ent_root_1")));
		TestEqual(TEXT("then cell 0's"), Place.Entities[1].Id, FString(TEXT("ent_cell_a")));
		TestEqual(TEXT("then cell 1's"), Place.Entities[2].Id, FString(TEXT("ent_ism_b")));
		TestTrue(TEXT("the place says its cells are in"), Place.bCellsMerged);
		TestFalse(TEXT("so nothing needs them again"), FMantleCellLoader::NeedsCells(Place));
		TArray<FMantleParseError> Again;
		TestFalse(TEXT("a second merge is refused"), FMantlePlaceCells::Merge(Place, Index, Cells, Again));
		TestEqual(TEXT("and changes nothing"), Place.Entities.Num(), 3);
	}

	// A place that streams its cells is built from its root alone, but its assets are prepared for the root and a copy of every cell's entities; nothing is changed in either.
	{
		const TArray<FMantleCellContent> Cells = Contents();
		FMantlePlace ForAssets;
		TArray<FMantleParseError> AssembleErrors;
		TestTrue(TEXT("the cells assemble for their assets"), FMantlePlaceCells::AssembleForAssets(Root, Index, Cells, ForAssets, AssembleErrors));
		TestEqual(TEXT("the root's entity and both cells'"), ForAssets.Entities.Num(), 3);
		TestEqual(TEXT("in the same order as a merge"), ForAssets.Entities[2].Id, FString(TEXT("ent_ism_b")));
		TestEqual(TEXT("the root is unchanged"), Root.Entities.Num(), 1);
		TestEqual(TEXT("and so are the cells"), Cells[0].Entities.Num() + Cells[1].Entities.Num(), 2);
		FMantlePlace Streamed = Root;
		Streamed.bCellsStreamed = true;
		TestFalse(TEXT("a place that streams its cells does not ask for them again"), FMantleCellLoader::NeedsCells(Streamed));
		TArray<FMantleParseError> Refused;
		TestFalse(TEXT("and is not assembled twice"), FMantlePlaceCells::AssembleForAssets(Streamed, Index, Cells, ForAssets, Refused));
		TArray<FMantleParseError> NothingChanged;
		TestTrue(TEXT("Validate says the same as Merge's checks"), FMantlePlaceCells::Validate(Root, Index, Cells, NothingChanged));

		// A standalone game keeps a record of every cell entity without its list of instances.
		TArray<FMantleEntity> Scenery;
		FMantlePlaceCells::SceneryRecords(Cells, Scenery);
		TestEqual(TEXT("both cells' entities"), Scenery.Num(), 2);
		TestEqual(TEXT("in cell order"), Scenery[1].Id, FString(TEXT("ent_ism_b")));
		const FMantleInstancedMeshProps* Emptied = Scenery[1].Props.TryGet<FMantleInstancedMeshProps>();
		const FMantleInstancedMeshProps* Original = Cells[1].Entities[0].Props.TryGet<FMantleInstancedMeshProps>();
		TestTrue(TEXT("the list is an InstancedMesh's"), Emptied && Original);
		TestTrue(TEXT("its instances are emptied"), Emptied && Emptied->Instances.IsEmpty());
		TestTrue(TEXT("and the cell's own are not touched"), Original && !Original->Instances.IsEmpty());
	}

	// An entity count that is not the index's.
	{
		FMantlePlace Place = Root;
		TArray<FMantleCellContent> Cells = Contents();
		Cells[0] = Read(0, CellStaticMeshJson(TEXT("ent_cell_a")) + TEXT(",") + CellStaticMeshJson(TEXT("ent_cell_z")));
		TArray<FMantleParseError> MergeErrors;
		TestFalse(TEXT("a cell with another entity count is refused"), FMantlePlaceCells::Merge(Place, Index, Cells, MergeErrors));
		TestTrue(TEXT("the error names the cell"), MergeErrors.Num() > 0 && MergeErrors[0].Path == TEXT("cell 0/0/0"));
		TestEqual(TEXT("the place is unchanged"), Place.Entities.Num(), 1);
		TestFalse(TEXT("and not marked merged"), Place.bCellsMerged);
	}

	// An instance count and a solid count that are not the index's.
	{
		FMantlePlace Place = Root;
		TArray<FMantleCellContent> Cells = Contents();
		Cells[1] = Read(1, CellInstancedJson(TEXT("ent_ism_b"), CellAnchorOnly, TEXT("none")));
		TArray<FMantleParseError> MergeErrors;
		TestFalse(TEXT("a cell whose solid count differs is refused"), FMantlePlaceCells::Merge(Place, Index, Cells, MergeErrors));
		TestTrue(TEXT("and says solid"), MergeErrors.ContainsByPredicate([](const FMantleParseError& Error) { return Error.Message.Contains(TEXT("solid")); }));
	}

	// One id space: a cell entity that repeats a root entity's id, or another cell's.
	{
		FMantlePlace Place = Root;
		TArray<FMantleCellContent> Cells = Contents();
		Cells[0] = Read(0, CellStaticMeshJson(TEXT("ent_root_1")));
		TArray<FMantleParseError> MergeErrors;
		TestFalse(TEXT("a cell entity with a root entity's id is refused"), FMantlePlaceCells::Merge(Place, Index, Cells, MergeErrors));
		TestTrue(TEXT("and says which id"), MergeErrors.ContainsByPredicate([](const FMantleParseError& Error) { return Error.Message.Contains(TEXT("ent_root_1")); }));
	}
	{
		FMantlePlace Place = Root;
		TArray<FMantleCellContent> Cells = Contents();
		Cells[1] = Read(1, CellInstancedJson(TEXT("ent_cell_a"), CellAnchorOnly, TEXT("blocking")));
		TArray<FMantleParseError> MergeErrors;
		TestFalse(TEXT("a cell entity with another cell's id is refused"), FMantlePlaceCells::Merge(Place, Index, Cells, MergeErrors));
		TestEqual(TEXT("the place is unchanged"), Place.Entities.Num(), 1);
	}

	// A cell that is not in the index.
	{
		FMantlePlace Place = Root;
		TArray<FMantleCellContent> Cells = Contents();
		Cells[1].CellIndex = 7;
		TArray<FMantleParseError> MergeErrors;
		TestFalse(TEXT("a content for a cell the index lacks is refused"), FMantlePlaceCells::Merge(Place, Index, Cells, MergeErrors));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
