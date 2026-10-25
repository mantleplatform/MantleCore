// Copyright Joshua Eger. All Rights Reserved.
#include "MantleClientFeatures.h"
#include "MantleCoverage.h"
#include "MantlePlaceAssetConverter.h"
#include "MantlePreviewClient.h"
#include "MantleTargetFeatures.h"
#include "MantleTerrainGrassExport.h"

#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

// What a local export does for the client it targets: the capability answer's features, the set that
// decides, and the downgrades (foliage shading, landscape grass) when the target does not read a feature.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleTargetFeaturesTestPrivate
{
	/** A masked, two-sided material with foliage shading: what MI_Trees converts to. */
	FMantlePlaceMaterialDocument FoliageDocument()
	{
		FMantlePlaceMaterialDocument Document;
		Document.AlphaMode = TEXT("mask");
		Document.bDoubleSided = true;
		Document.bFoliage = true;
		Document.FoliageStrength = 2.0;
		return Document;
	}

	/** The UTF-8 bytes of a material upload as text. */
	FString BytesText(const TArray<uint8>& Bytes)
	{
		const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
		return FString(Converted.Length(), Converted.Get());
	}

	bool ParseJson(const FString& Text, TSharedPtr<FJsonObject>& Out)
	{
		return FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Out) && Out.IsValid();
	}

	/** One entry the grass downgrade has to leave out. */
	FMantleTerrainGrassResult GrassResult()
	{
		FMantleTerrainGrassResult Result;
		Result.TypeNames = { TEXT("LGT_Grass"), TEXT("LGT_Flowers") };
		Result.TypeWeights.SetNum(2);
		Result.TypeCoverage = { 40.0, 10.0 };
		Result.TypesInMaterial = 2;
		FMantleGrassEntryDraft& Entry = Result.Entries.AddDefaulted_GetRef();
		Entry.TypeName = TEXT("LGT_Grass");
		Result.Warnings.Add(FMantleGrassNote(EMantleCoverageOutcome::Approximated, FString(), TEXT("grass follows the paint layers")));
		Result.Warnings.Add(FMantleGrassNote(EMantleCoverageOutcome::Dropped, TEXT("LGT_Bare"), TEXT("Grass type 'LGT_Bare' has no asset")));
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleClientFeaturesAnswerTest,
	"Mantle.Editor.TargetFeatures.CapabilityAnswerFeatures",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleClientFeaturesAnswerTest::RunTest(const FString& Parameters)
{
	using namespace MantleTargetFeaturesTestPrivate;

	// An answer from a client that predates "features" reads none of them.
	TSharedPtr<FJsonObject> Old;
	TestTrue(TEXT("the old answer parses"), ParseJson(TEXT("{\"previewProtocol\":1,\"manifestSchemas\":[1,2],\"scriptApi\":{\"major\":1,\"minor\":34},\"clientVersion\":\"1.0\",\"cookTool\":\"mantle-cook.exe\"}"), Old));
	FMantlePreviewClientInfo OldInfo;
	MantlePreviewClient::ReadCapabilities(Old, OldInfo);
	TestEqual(TEXT("an old answer's protocol is still read"), OldInfo.PreviewProtocol, 1);
	TestEqual(TEXT("an old answer has no features"), OldInfo.Features.Num(), 0);
	const FMantleTargetFeatures OldTarget = FMantleTargetFeatures::FromNames(OldInfo.Features);
	TestFalse(TEXT("an old client does not read materialFidelity"), OldTarget.Reads(MantleClientFeatures::MaterialFidelity));
	TestFalse(TEXT("an old client does not read terrainGrass"), OldTarget.Reads(MantleClientFeatures::TerrainGrass));
	TestTrue(TEXT("an old client's argument for the cook is empty"), OldTarget.ToArgument().IsEmpty());

	// A newer client lists them; blanks and repeats are ignored, a name this editor does not know is kept (it is the cook's to judge).
	TSharedPtr<FJsonObject> New;
	TestTrue(TEXT("the new answer parses"), ParseJson(TEXT("{\"previewProtocol\":1,\"features\":[\"materialFidelity\",\"terrainGrass\",\"materialFidelity\",\"\",\"warpDrive\",7]}"), New));
	FMantlePreviewClientInfo NewInfo;
	MantlePreviewClient::ReadCapabilities(New, NewInfo);
	TestEqual(TEXT("the feature names are read once each"), NewInfo.Features.Num(), 3);
	const FMantleTargetFeatures NewTarget = FMantleTargetFeatures::FromNames(NewInfo.Features);
	TestTrue(TEXT("a new client reads materialFidelity"), NewTarget.Reads(MantleClientFeatures::MaterialFidelity));
	TestTrue(TEXT("a new client reads terrainGrass"), NewTarget.Reads(MantleClientFeatures::TerrainGrass));
	TestFalse(TEXT("a new client does not read what it did not list"), NewTarget.Reads(MantleClientFeatures::CarPaint));
	TestEqual(TEXT("the cook's argument is the sorted comma list"), NewTarget.ToArgument(), FString(TEXT("materialFidelity,terrainGrass,warpDrive")));

	// A second answer read into the same info replaces the features, it does not add to them.
	MantlePreviewClient::ReadCapabilities(Old, NewInfo);
	TestEqual(TEXT("an answer without features clears the features"), NewInfo.Features.Num(), 0);

	// What this client writes into its own answer is what its editor then reads.
	TArray<FString> All = MantleClientFeatures::All();
	TestTrue(TEXT("this build names materialFidelity"), All.Contains(TEXT("materialFidelity")));
	TestTrue(TEXT("this build names terrainGrass"), All.Contains(TEXT("terrainGrass")));
	const TSet<FString> Unique(All);
	TestEqual(TEXT("no feature is listed twice"), Unique.Num(), All.Num());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTargetFeaturesParseTest,
	"Mantle.Editor.TargetFeatures.ParsesTheFeaturesArgument",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTargetFeaturesParseTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("a default target reads every feature"), FMantleTargetFeatures().Names.Num(), MantleClientFeatures::All().Num());
	TestEqual(TEXT("all is every feature"), FMantleTargetFeatures::Parse(TEXT("all")).Names.Num(), MantleClientFeatures::All().Num());
	TestEqual(TEXT("ALL in any case, among others"), FMantleTargetFeatures::Parse(TEXT("terrainGrass, ALL")).Names.Num(), MantleClientFeatures::All().Num());
	TestEqual(TEXT("an empty list is none"), FMantleTargetFeatures::Parse(FString()).Names.Num(), 0);
	const FMantleTargetFeatures Some = FMantleTargetFeatures::Parse(TEXT(" terrainGrass , carPaint,,"));
	TestEqual(TEXT("a list is its names"), Some.Names.Num(), 2);
	TestTrue(TEXT("terrainGrass is in it"), Some.Reads(MantleClientFeatures::TerrainGrass));
	TestFalse(TEXT("materialFidelity is not"), Some.Reads(MantleClientFeatures::MaterialFidelity));
	TestEqual(TEXT("its cook argument"), Some.ToArgument(), FString(TEXT("carPaint,terrainGrass")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTargetFeaturesMaterialTest,
	"Mantle.Editor.TargetFeatures.FoliageDowngradesToAPlainMaskedMaterial",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTargetFeaturesMaterialTest::RunTest(const FString& Parameters)
{
	using namespace MantleTargetFeaturesTestPrivate;
	const TMap<FString, FString> NoIds;

	// The decision on its own.
	FMantlePlaceMaterialDocument Document = FoliageDocument();
	TestTrue(TEXT("a masked, two-sided foliage document writes foliage"), FMantleTargetFeatures::WritesFoliage(Document));
	TestFalse(TEXT("a client that reads materialFidelity changes nothing"), FMantleTargetFeatures::All().DowngradeMaterial(Document));
	TestTrue(TEXT("and the document keeps its foliage"), Document.bFoliage);
	TestTrue(TEXT("a client that does not read it downgrades"), FMantleTargetFeatures::None().DowngradeMaterial(Document));
	TestFalse(TEXT("foliage is off the document"), Document.bFoliage);
	TestFalse(TEXT("and the downgrade is only once"), FMantleTargetFeatures::None().DowngradeMaterial(Document));
	FMantlePlaceMaterialDocument Plain;
	TestFalse(TEXT("a material with no foliage is not a downgrade"), FMantleTargetFeatures::None().DowngradeMaterial(Plain));

	// What it writes: the foliage key with the feature, the plain masked two-sided material without.
	FString With;
	FString Without;
	FString Error;
	FMantlePlaceMaterialDocument Kept = FoliageDocument();
	TestTrue(TEXT("foliage writes"), Kept.ToJson(NoIds, With, Error));
	TestTrue(TEXT("with the foliage key"), With.Contains(TEXT("\"foliage\"")));
	FMantlePlaceMaterialDocument Downgraded = FoliageDocument();
	FMantleTargetFeatures::None().DowngradeMaterial(Downgraded);
	TestTrue(TEXT("the downgraded document writes"), Downgraded.ToJson(NoIds, Without, Error));
	TestFalse(TEXT("without the foliage key"), Without.Contains(TEXT("foliage")));
	TestTrue(TEXT("still masked"), Without.Contains(TEXT("\"alphaMode\":\"mask\"")));
	TestTrue(TEXT("and two-sided"), Without.Contains(TEXT("\"doubleSided\":true")));

	// The asset set writes the material through the export's target, and says what it left out.
	FMantlePlaceAssetSet Set;
	FMantleConvertedMaterial& Converted = Set.Materials.AddDefaulted_GetRef();
	Converted.Key = TEXT("material:MI_Trees");
	Converted.Name = TEXT("MI_Trees");
	Converted.Document = FoliageDocument();
	TArray<uint8> Bytes;
	TestTrue(TEXT("no export in progress: the set writes the material"), Set.MaterialBytes(Converted, Bytes, Error));
	TestEqual(TEXT("exactly as the document itself does"), BytesText(Bytes), With);
	{
		const FMantleTargetFeaturesScope Scope(FMantleTargetFeatures::None());
		TestFalse(TEXT("inside an export for an older client, Current reads no foliage"), FMantleTargetFeatures::Current().Reads(MantleClientFeatures::MaterialFidelity));
		TArray<uint8> OlderBytes;
		TestTrue(TEXT("the set writes it for the older client"), Set.MaterialBytes(Converted, OlderBytes, Error));
		const FString Older = BytesText(OlderBytes);
		TestFalse(TEXT("without the foliage key"), Older.Contains(TEXT("foliage")));
		TestTrue(TEXT("the source document still has its foliage"), Converted.Document.bFoliage);

		FMantleCoverage Coverage;
		FMantleTargetFeatures::Current().ReportMaterialDowngrades(Set, Coverage);
		TestEqual(TEXT("one approximated material"), Coverage.Count(MantleCoverageCategory::Materials, EMantleCoverageOutcome::Approximated), static_cast<int64>(1));
		const FMantleCoverageCategory* Category = Coverage.Find(MantleCoverageCategory::Materials);
		if (TestNotNull(TEXT("the materials category"), Category) && Category->Get(EMantleCoverageOutcome::Approximated).Examples.Num() == 1)
		{
			const FMantleCoverageExample& Example = Category->Get(EMantleCoverageOutcome::Approximated).Examples[0];
			TestEqual(TEXT("it names the material"), Example.Name, FString(TEXT("MI_Trees")));
			TestTrue(TEXT("and says why"), Example.Detail.Contains(TEXT("needs a newer Mantle client")));
		}
	}
	TestTrue(TEXT("the scope ended: every feature again"), FMantleTargetFeatures::Current().Reads(MantleClientFeatures::MaterialFidelity));
	FMantleCoverage Quiet;
	FMantleTargetFeatures::All().ReportMaterialDowngrades(Set, Quiet);
	TestTrue(TEXT("a client that reads it reports nothing"), Quiet.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTargetFeaturesGrassTest,
	"Mantle.Editor.TargetFeatures.GrassIsLeftOutAndReported",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTargetFeaturesGrassTest::RunTest(const FString& Parameters)
{
	using namespace MantleTargetFeaturesTestPrivate;

	FMantleTerrainGrassResult Kept = GrassResult();
	TestFalse(TEXT("a client that reads terrainGrass keeps it"), FMantleTargetFeatures::All().DowngradeGrass(Kept));
	TestEqual(TEXT("with its entries"), Kept.Entries.Num(), 1);
	TestEqual(TEXT("and its notes"), Kept.Warnings.Num(), 2);

	FMantleTerrainGrassResult Result = GrassResult();
	TestTrue(TEXT("a client that does not read it drops it"), FMantleTargetFeatures::None().DowngradeGrass(Result));
	TestEqual(TEXT("no entries"), Result.Entries.Num(), 0);
	TestEqual(TEXT("no map slots, so no grass map textures"), Result.NumMaps(), 0);
	TestEqual(TEXT("no weights"), Result.TypeWeights.Num(), 0);
	TestEqual(TEXT("the grass types are named for the report"), Result.NeedsNewerClient.Num(), 2);
	TestEqual(TEXT("only what was left out stays said"), Result.Warnings.Num(), 1);
	TestEqual(TEXT("the dropped note"), Result.Warnings[0].Name, FString(TEXT("LGT_Bare")));
	TestFalse(TEXT("the drop is only once"), FMantleTargetFeatures::None().DowngradeGrass(Result));

	FMantleCoverage Coverage;
	FMantleTerrainGrassExport::ReportCoverage(Coverage, Result);
	TestEqual(TEXT("each grass type is a dropped line"), Coverage.Count(MantleCoverageCategory::LandscapeGrass, EMantleCoverageOutcome::Dropped), static_cast<int64>(3));
	TestEqual(TEXT("and nothing is kept"), Coverage.Count(MantleCoverageCategory::LandscapeGrass, EMantleCoverageOutcome::Kept), static_cast<int64>(0));
	const FMantleCoverageCategory* Category = Coverage.Find(MantleCoverageCategory::LandscapeGrass);
	bool bSaid = false;
	if (TestNotNull(TEXT("the grass category"), Category))
	{
		for (const FMantleCoverageExample& Example : Category->Get(EMantleCoverageOutcome::Dropped).Examples)
		{
			bSaid = bSaid || (Example.Name == TEXT("LGT_Grass") && Example.Detail.Contains(TEXT("needs a newer Mantle client")));
		}
	}
	TestTrue(TEXT("LGT_Grass says it needs a newer Mantle client"), bSaid);

	// A landscape with grass types that grow nowhere has nothing to drop, and says what it said before.
	FMantleTerrainGrassResult Empty;
	Empty.TypesInMaterial = 1;
	TestFalse(TEXT("no entries is no downgrade"), FMantleTargetFeatures::None().DowngradeGrass(Empty));
	TestEqual(TEXT("and TypesInMaterial is untouched"), Empty.TypesInMaterial, 1);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
