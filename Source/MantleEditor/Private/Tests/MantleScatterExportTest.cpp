// Copyright Joshua Eger. All Rights Reserved.
#include "MantleHeightfieldReader.h"
#include "MantlePlaceExporter.h"
#include "MantlePlaceParser.h"
#include "MantlePlaceValidator.h"
#include "MantleScatter.h"
#include "MantleScatterEditorGround.h"
#include "MantleScatterVolume.h"
#include "MantleTerrainExport.h"
#include "MantleTerrainSurface.h"

#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// Scatter's #169 keys from the editor's side: the exporter writes each one where it applies and the parser reads back what
// the editor generated from, so the client's instances hash as the editor's do; and a volume placed on terrain drops onto
// the level's landscape as the published Terrain carries it (FMantleScatterEditorGround). The generator itself is the
// runtime tests' (MantleScatterTest.cpp).

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleScatterExportTestPrivate
{
	/** A place of one Scatter of Properties (the text between the braces) drawing from ast_cube. */
	FMantleParseResult ParseScatter(const FString& Properties)
	{
		const FString Json = FString::Printf(
			TEXT("{\"schemaVersion\":1,\"placeId\":\"plc_scatter001\",\"revision\":{\"number\":1,\"state\":\"draft\"},"
				 "\"metadata\":{\"name\":\"Field\",\"authorId\":\"usr_4b1d\"},\"settings\":{},"
				 "\"assets\":[{\"id\":\"ast_cube\",\"kind\":\"StaticMesh\",\"hash\":\"md5:0123456789abcdef\",\"bytes\":1024}],"
				 "\"entities\":[{\"id\":\"ent_scatter\",\"type\":\"Scatter\",\"parent\":null,"
				 "\"transform\":{\"location\":[0,0,0],\"rotation\":[0,0,0],\"scale\":[1,1,1]},\"properties\":{%s}}]}"),
			*Properties);
		return FMantlePlaceParser::ParseFromString(Json);
	}

	const FMantleScatterParams* ScatterOf(const FMantleParseResult& Result)
	{
		return Result.Place.Entities.Num() == 1 ? Result.Place.Entities[0].Props.TryGet<FMantleScatterParams>() : nullptr;
	}

	/** Every #169 key set, at values a float would not keep. */
	FMantleScatterParams EveryKey(const EMantleScatterShape Shape)
	{
		FMantleScatterParams P;
		P.Seed = 169;
		P.HalfExtent = FVector(2000.0, 1500.0, 25.0);
		P.Density = 0.123456789;
		P.Meshes = { { TEXT("ast_cube"), 1, {} } };
		P.DrawDistance = 12345.678901;
		P.Placement = EMantleScatterPlacement::Terrain;
		P.MaxSlopeDegrees = 33.3333333;
		P.MinHeight = -250.125;
		P.MaxHeight = 4000.0625;
		P.Shape = Shape;
		P.Radius = 1777.777;
		P.Path = { FVector2D(-1000.5, 0.25), FVector2D(0.0, 900.125), FVector2D(1200.0, -300.0) };
		P.Width = 333.3;
		P.Exclusions = { { FVector2D(100.5, -200.25), FVector2D(150.0, 75.5) }, { FVector2D(-800.0, 0.0), FVector2D(50.0, 50.0) } };
		P.ClusterCount = 7;
		P.ClusterRadius = 420.42;
		P.bCollision = true;
		if (Shape != EMantleScatterShape::Disc)
		{
			P.Radius = 0.0;
		}
		if (Shape != EMantleScatterShape::Path)
		{
			P.Path.Reset();
			P.Width = 0.0;
		}
		return P;
	}

	/** The instances Params makes on flat ground at ZMm, the same for the editor's and the parsed copy. */
	uint64 FlatHash(const FMantleScatterParams& Params)
	{
		FMantleScatterGenerateOptions Options;
		Options.Ground = [](const int64 XMm, const int64 YMm, FMantleScatterGround& Out)
		{
			Out.ZMm = (XMm + YMm) / 100;
			Out.NormalZ = 0.9;
			Out.WorldZCm = static_cast<double>(XMm) * 0.01;
			return true;
		};
		TArray<FMantleScatterInstance> Instances;
		FString Error;
		return FMantleScatter::Generate(Params, Instances, Error, Options) ? FMantleScatter::Hash(Instances) : 0;
	}

	/** An empty editor world for one test, destroyed with it. */
	struct FScatterWorld
	{
		UWorld* World = nullptr;

		FScatterWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld*/ false, TEXT("MantleScatterExportTestWorld"));
		}

		~FScatterWorld()
		{
			if (World)
			{
				World->DestroyWorld(/*bInformEngineOfWorld*/ false);
				if (World->IsRooted())
				{
					World->RemoveFromRoot();
				}
			}
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleScatterExportKeysTest,
	"Mantle.Editor.Scatter.Export.NewKeysSurviveThePlaceFile",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleScatterExportKeysTest::RunTest(const FString& Parameters)
{
	using namespace MantleScatterExportTestPrivate;

	// The place around the scatter, from the parser; its properties then replaced by the editor's.
	FMantleParseResult Base = ParseScatter(TEXT("\"halfExtent\":[1000,1000,0],\"density\":0.1,\"meshes\":[{\"mesh\":\"ast_cube\"}]"));
	if (!TestTrue(TEXT("the base place parses"), Base.bSuccess && Base.Place.Entities.Num() == 1))
	{
		return false;
	}
	for (const EMantleScatterShape Shape : { EMantleScatterShape::Box, EMantleScatterShape::Disc, EMantleScatterShape::Path })
	{
		const FString What = FMantleScatter::ShapeName(Shape);
		const FMantleScatterParams Sent = EveryKey(Shape);
		Base.Place.Entities[0].Props.Set<FMantleScatterParams>(Sent);
		const FString Json = FMantlePlaceExporter::ToJson(Base.Place);
		const FMantleParseResult Back = FMantlePlaceParser::ParseFromString(Json);
		for (const FMantleParseError& Error : Back.Errors)
		{
			AddError(FString::Printf(TEXT("%s: %s %s"), *What, *Error.Path, *Error.Message));
		}
		const FMantleScatterParams* Got = ScatterOf(Back);
		if (!TestNotNull(*FString::Printf(TEXT("%s: the scatter survives the file"), *What), Got))
		{
			continue;
		}
		TestEqual(What + TEXT(": draw distance"), Got->DrawDistance, Sent.DrawDistance);
		TestTrue(What + TEXT(": placed on terrain"), Got->Placement == EMantleScatterPlacement::Terrain);
		TestEqual(What + TEXT(": max slope"), Got->MaxSlopeDegrees, Sent.MaxSlopeDegrees);
		TestTrue(What + TEXT(": min height"), Got->MinHeight == Sent.MinHeight);
		TestTrue(What + TEXT(": max height"), Got->MaxHeight == Sent.MaxHeight);
		TestTrue(What + TEXT(": shape"), Got->Shape == Shape);
		TestEqual(What + TEXT(": radius"), Got->Radius, Sent.Radius);
		TestEqual(What + TEXT(": path points"), Got->Path.Num(), Sent.Path.Num());
		for (int32 Index = 0; Index < Sent.Path.Num() && Index < Got->Path.Num(); ++Index)
		{
			TestTrue(What + TEXT(": path point"), Got->Path[Index] == Sent.Path[Index]);
		}
		TestEqual(What + TEXT(": width"), Got->Width, Sent.Width);
		TestEqual(What + TEXT(": exclusions"), Got->Exclusions.Num(), Sent.Exclusions.Num());
		for (int32 Index = 0; Index < Sent.Exclusions.Num() && Index < Got->Exclusions.Num(); ++Index)
		{
			TestTrue(What + TEXT(": exclusion"), Got->Exclusions[Index].Center == Sent.Exclusions[Index].Center
				&& Got->Exclusions[Index].HalfExtent == Sent.Exclusions[Index].HalfExtent);
		}
		TestEqual(What + TEXT(": cluster count"), Got->ClusterCount, Sent.ClusterCount);
		TestEqual(What + TEXT(": cluster radius"), Got->ClusterRadius, Sent.ClusterRadius);
		TestTrue(What + TEXT(": collision"), Got->bCollision);
		const uint64 SentHash = FlatHash(Sent);
		TestNotEqual(What + TEXT(": the editor's copy generates"), SentHash, static_cast<uint64>(0));
		TestEqual(What + TEXT(": and the client's hashes the same"), FlatHash(*Got), SentHash);
	}

	// A plain box scatter writes none of the new keys, which the schema refuses without the placement or shape they need.
	FMantleScatterParams Plain;
	Plain.Meshes = { { TEXT("ast_cube"), 1, {} } };
	Plain.MaxSlopeDegrees = 45.0; // a value the volume keeps while placed on a box: not written
	Base.Place.Entities[0].Props.Set<FMantleScatterParams>(Plain);
	const FString PlainJson = FMantlePlaceExporter::ToJson(Base.Place);
	TSharedPtr<FJsonObject> Root;
	const TArray<TSharedPtr<FJsonValue>>* Entities = nullptr;
	const TSharedPtr<FJsonObject>* Properties = nullptr;
	if (TestTrue(TEXT("the plain file is JSON"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(PlainJson), Root) && Root.IsValid()
		&& Root->TryGetArrayField(TEXT("entities"), Entities) && Entities->Num() == 1
		&& (*Entities)[0]->AsObject()->TryGetObjectField(TEXT("properties"), Properties)))
	{
		for (const TCHAR* Key : { TEXT("drawDistance"), TEXT("placement"), TEXT("maxSlopeDegrees"), TEXT("minHeight"), TEXT("maxHeight"),
			TEXT("shape"), TEXT("radius"), TEXT("path"), TEXT("width"), TEXT("exclusions"), TEXT("clusterCount"), TEXT("clusterRadius"), TEXT("collision") })
		{
			TestFalse(FString::Printf(TEXT("a plain box scatter writes no %s"), Key), (*Properties)->HasField(Key));
		}
	}
	TestTrue(TEXT("and parses"), FMantlePlaceParser::ParseFromString(PlainJson).bSuccess);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleScatterEditorGroundTest,
	"Mantle.Editor.Scatter.Preview.TerrainPlacementLandsOnTheLandscape",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleScatterEditorGroundTest::RunTest(const FString& Parameters)
{
	using namespace MantleScatterExportTestPrivate;

	// A flat landscape 10 m square, 100 cm up (128 height units at the default Z scale) and standing 50 cm higher, with
	// its first five sample columns painted as a hole: every quad below X 5 m is a hole.
	FMantleTerrainSource Source;
	Source.Width = 11;
	Source.Height = 11;
	Source.Heights.Init(static_cast<uint16>(32768 + 128), Source.Width * Source.Height);
	Source.Holes.Init(0, Source.Width * Source.Height);
	for (int32 Y = 0; Y < Source.Height; ++Y)
	{
		for (int32 X = 0; X < 5; ++X)
		{
			Source.Holes[Y * Source.Width + X] = 1;
		}
	}
	Source.Transform = FTransform(FVector(0.0, 0.0, 50.0));
	const FMantleScatterTerrainSurface Surface = FMantleScatterEditorGround::SurfaceFor(Source);
	if (!TestTrue(TEXT("the landscape makes a heightfield"), Surface.Field.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("as wide as the landscape"), Surface.Field->ExtentX(), 1000.0);
	TestTrue(TEXT("with its holes"), Surface.Holes.IsValid() && Surface.Holes->QuadIsHole(4, 3) && !Surface.Holes->QuadIsHole(5, 3));

	FScatterWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	AMantleScatterVolume* Volume = Fixture.World->SpawnActor<AMantleScatterVolume>(FVector(500.0, 500.0, 0.0), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("a volume"), Volume))
	{
		return false;
	}
	Volume->HalfExtent = FVector(400.0, 400.0, 0.0);
	Volume->Density = 2.0;
	Volume->Meshes.SetNum(1);
	Volume->Meshes[0].MeshId = TEXT("ast_cube");
	Volume->Placement = EMantleScatterPlacement::Terrain;
	Volume->SetTerrainSurfaces({ Surface });
	FString Error;
	if (!TestTrue(TEXT("it scatters"), Volume->RefreshScatter(Error)))
	{
		AddError(Error);
		return false;
	}
	FMantleScatterParams Params;
	FString Problem;
	Volume->ReadProps(Params, Problem);
	const int64 Asked = FMantleScatter::CountFor(Params, Error);
	TestTrue(FString::Printf(TEXT("the hole leaves some of the %lld out (%d kept)"), static_cast<long long>(Asked), Volume->GetInstanceCount()),
		Volume->GetInstanceCount() > 0 && Volume->GetInstanceCount() < Asked);

	// The instances themselves, from the same ground: each on the landscape's 150 cm and none over the hole.
	FMantleScatterGenerateOptions Options;
	Options.Ground = [](const int64 XMm, const int64 YMm, FMantleScatterGround& Out)
	{
		// The volume stands at X 5 m, the hole's edge: what is left of it is over the hole.
		if (XMm < 0)
		{
			return false;
		}
		Out.ZMm = 1500;
		Out.NormalZ = 1.0;
		Out.WorldZCm = 150.0;
		return true;
	};
	TArray<FMantleScatterInstance> Expected;
	TestTrue(TEXT("the same scatter on a modelled ground"), FMantleScatter::Generate(Params, Expected, Error, Options));
	TestEqual(TEXT("keeps as many"), Volume->GetInstanceCount(), Expected.Num());
	TestEqual(TEXT("and hashes as the volume does"), FMantleScatter::HashText(Volume->GetInstanceHash()),
		FMantleScatter::HashText(FMantleScatter::Hash(Expected)));

	// Placed on a box again, the hole has no say.
	Volume->Placement = EMantleScatterPlacement::Box;
	TestTrue(TEXT("a box scatter"), Volume->RefreshScatter(Error));
	TestEqual(TEXT("keeps every instance"), static_cast<int64>(Volume->GetInstanceCount()), Asked);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleScatterCollisionCheckTest,
	"Mantle.Editor.Scatter.Check.CollidingInstancesHaveAPlaceCap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleScatterCollisionCheckTest::RunTest(const FString& Parameters)
{
	using namespace MantleScatterExportTestPrivate;

	// Two colliding scatters of 1500 each (400 square metres at 3.75): the second crosses the place's 2000, and Check names it.
	FScatterWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	const auto Spawn = [&Fixture](const TCHAR* Label, const FVector& Location, const bool bCollision)
	{
		AMantleScatterVolume* Volume = Fixture.World->SpawnActor<AMantleScatterVolume>(Location, FRotator::ZeroRotator);
		if (Volume)
		{
			Volume->SetActorLabel(Label);
			Volume->HalfExtent = FVector(1000.0, 1000.0, 0.0);
			Volume->Density = 3.75;
			Volume->Meshes.SetNum(1);
			Volume->Meshes[0].MeshId = TEXT("ast_cube");
			Volume->bCollision = bCollision;
		}
		return Volume;
	};
	const auto CapIssueOn = [](const FMantleValidationResult& Result, const TCHAR* Label)
	{
		return Result.Issues.ContainsByPredicate([Label](const FMantleValidationIssue& Issue)
		{
			return Issue.Actor == Label && Issue.Severity == EMantleIssueSeverity::Error && Issue.Problem.Contains(TEXT("colliding scatter instances"));
		});
	};
	AMantleScatterVolume* First = Spawn(TEXT("Rocks A"), FVector::ZeroVector, true);
	AMantleScatterVolume* Second = Spawn(TEXT("Rocks B"), FVector(5000.0, 0.0, 0.0), true);
	if (!TestNotNull(TEXT("two volumes"), First) || !TestNotNull(TEXT("two volumes"), Second))
	{
		return false;
	}
	FMantleScatterParams Params;
	FString Problem;
	TestTrue(TEXT("each reads"), First->ReadProps(Params, Problem));
	TestEqual(TEXT("1500 instances each"), FMantleScatter::CountFor(Params, Problem), static_cast<int64>(1500));

	FMantleValidationResult Result = FMantlePlaceValidator::Validate(Fixture.World);
	TestTrue(TEXT("the volume that crosses 2000 is named"), CapIssueOn(Result, TEXT("Rocks A")) != CapIssueOn(Result, TEXT("Rocks B")));

	// Without collision on one of them, the place holds 1500 colliding instances: no error.
	Second->bCollision = false;
	Result = FMantlePlaceValidator::Validate(Fixture.World);
	TestFalse(TEXT("one colliding scatter fits"), CapIssueOn(Result, TEXT("Rocks A")) || CapIssueOn(Result, TEXT("Rocks B")));
	return true;
}

#endif
