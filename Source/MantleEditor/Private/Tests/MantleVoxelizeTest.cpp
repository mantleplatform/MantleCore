// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceAssets.h"
#include "MantlePlaceExporter.h"
#include "MantlePlaceParser.h"
#include "MantlePlaceValidator.h"
#include "MantleVoxelGrid.h"
#include "MantleVoxelize.h"
#include "MantleVoxelWorldActor.h"
#include "MantleVoxelWorldFile.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// #168 step 6: Voxelize turns cube meshes into a Mantle Voxel World, Import makes one from an MBW1 file, and the exporter, the
// gather and Check carry it. Built in a transient editor world with the engine's 100 cm cube; tiles go to the transient package.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVoxelizeTestPrivate
{
	/** An empty editor world for one test, destroyed with it. */
	struct FVoxelWorld
	{
		UWorld* World = nullptr;

		FVoxelWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld*/ false, TEXT("MantleVoxelizeTestWorld"));
		}

		~FVoxelWorld()
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

	UStaticMesh* LoadCube()
	{
		return LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	}

	/** The engine cube (100 cm, centred on its pivot) at Location, at Scale, drawn with Material when one is given. */
	AStaticMeshActor* SpawnCube(UWorld* World, UStaticMesh* Cube, const FVector& Location, const TCHAR* Label, double Scale = 1.0, UMaterialInterface* Material = nullptr)
	{
		AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(Location, FRotator::ZeroRotator);
		if (Actor)
		{
			Actor->SetActorLabel(Label);
			Actor->GetStaticMeshComponent()->SetStaticMesh(Cube);
			Actor->SetActorScale3D(FVector(Scale));
			if (Material)
			{
				Actor->GetStaticMeshComponent()->SetMaterial(0, Material);
			}
		}
		return Actor;
	}

	/**
	 * Four cubes: three plain ones at cells (0, 0, 0), (1, 0, 0) and (-2, 0, 0), and a 200 cm one in another material filling
	 * cells x 2 to 3, y 0 to 1, z 0 to 1. Eleven blocks of two types, from cell (-2, 0, 0), 6 x 2 x 2 cells: a 16 x 16 x 16 world.
	 */
	TArray<AActor*> SpawnCubeScene(UWorld* World, UStaticMesh* Cube)
	{
		UMaterialInterface* Other = UMaterial::GetDefaultMaterial(MD_Surface);
		return {
			SpawnCube(World, Cube, FVector(50.0, 50.0, 50.0), TEXT("Cube A")),
			SpawnCube(World, Cube, FVector(150.0, 50.0, 50.0), TEXT("Cube B")),
			SpawnCube(World, Cube, FVector(-150.0, 50.0, 50.0), TEXT("Cube C")),
			SpawnCube(World, Cube, FVector(300.0, 100.0, 100.0), TEXT("Big cube"), 2.0, Other),
		};
	}

	/** An MBW1 file of SizeInBlocks with block Id at each of Cells. */
	TArray<uint8> WriteFile(const FIntVector& SizeInBlocks, const TArray<TPair<FIntVector, uint8>>& Cells)
	{
		TArray<FMantleVoxelBlockType> Types;
		Types.AddDefaulted(256);
		FMantleVoxelGrid Grid;
		Grid.Init(SizeInBlocks, MoveTemp(Types));
		for (const TPair<FIntVector, uint8>& Cell : Cells)
		{
			Grid.SetBlock(Cell.Key, Cell.Value);
		}
		TArray<uint8> Bytes;
		FMantleVoxelWorldFile::FromGrid(Grid).Write(Bytes);
		return Bytes;
	}

	bool HasErrorContaining(const FMantleValidationResult& Validation, const FString& Label, const TCHAR* Text)
	{
		return Validation.Issues.ContainsByPredicate([&Label, Text](const FMantleValidationIssue& Issue)
		{
			return Issue.Severity == EMantleIssueSeverity::Error && Issue.Actor == Label && Issue.Problem.Contains(Text);
		});
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVoxelizeNamesAndTilesTest,
	"Mantle.Editor.Voxelize.NamesAndTiles",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVoxelizeNamesAndTilesTest::RunTest(const FString& Parameters)
{
	// Block type names from mesh names: the place format's a-z 0-9 _, no SM_ prefix, never air, unique.
	TSet<FString> Used;
	TestEqual(TEXT("SM_ is dropped and the rest lower-cased"), FMantleVoxelize::MakeTypeName(TEXT("SM_Cube_Grass"), Used), FString(TEXT("cube_grass")));
	TestEqual(TEXT("a repeat is numbered"), FMantleVoxelize::MakeTypeName(TEXT("SM_Cube_Grass"), Used), FString(TEXT("cube_grass_2")));
	TestEqual(TEXT("other characters become one underscore"), FMantleVoxelize::MakeTypeName(TEXT("Rock (Big)  #2"), Used), FString(TEXT("rock_big_2")));
	TestEqual(TEXT("air is not a block type"), FMantleVoxelize::MakeTypeName(TEXT("Air"), Used), FString(TEXT("air_block")));
	TestEqual(TEXT("nothing usable is 'block'"), FMantleVoxelize::MakeTypeName(TEXT("___"), Used), FString(TEXT("block")));
	const FString Long = FMantleVoxelize::MakeTypeName(FString::ChrN(50, TEXT('a')), Used);
	TestEqual(TEXT("at most 32 characters"), Long.Len(), 32);
	TestEqual(TEXT("and still unique when cut"), FMantleVoxelize::MakeTypeName(FString::ChrN(50, TEXT('a')), Used).Len(), 32);

	// A tile is the image averaged down to a square: a 4 x 2 image of a red left half and a blue right half, at 2 x 2.
	TArray<FLinearColor> Pixels;
	for (int32 Y = 0; Y < 2; ++Y)
	{
		for (int32 X = 0; X < 4; ++X)
		{
			Pixels.Add(X < 2 ? FLinearColor::Red : FLinearColor::Blue);
		}
	}
	const TArray<FColor> Tile = FMantleVoxelize::ResampleSquare(TConstArrayView64<FLinearColor>(Pixels.GetData(), Pixels.Num()), 4, 2, 2);
	if (TestEqual(TEXT("a 2 x 2 tile"), Tile.Num(), 4))
	{
		TestEqual(TEXT("its left column is red"), Tile[0], FLinearColor::Red.ToFColor(true));
		TestEqual(TEXT("its right column is blue"), Tile[1], FLinearColor::Blue.ToFColor(true));
	}
	const TArray<FColor> Grown = FMantleVoxelize::ResampleSquare(TConstArrayView64<FLinearColor>(Pixels.GetData(), Pixels.Num()), 4, 2, 8);
	TestEqual(TEXT("a smaller image is sampled up to the tile size"), Grown.Num(), 64);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVoxelizeCubesTest,
	"Mantle.Editor.Voxelize.CubesBecomeBlocks",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVoxelizeCubesTest::RunTest(const FString& Parameters)
{
	using namespace MantleVoxelizeTestPrivate;
	UStaticMesh* Cube = LoadCube();
	FVoxelWorld Fixture;
	if (!TestNotNull(TEXT("the engine cube"), Cube) || !TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	const TArray<AActor*> Sources = SpawnCubeScene(Fixture.World, Cube);
	const FMantleVoxelizeResult Result = FMantleVoxelize::Voxelize(Fixture.World, Sources, FMantleVoxelizeOptions());
	if (!TestTrue(FString::Printf(TEXT("it voxelizes (%s)"), *Result.Error), Result.IsOk()))
	{
		return false;
	}
	TestEqual(TEXT("the world starts at the lowest filled cell"), Result.Origin, FVector(-200.0, 0.0, 0.0));
	TestEqual(TEXT("and is placed there"), Result.World->GetActorLocation(), FVector(-200.0, 0.0, 0.0));
	TestEqual(TEXT("its size is rounded up to whole sections"), Result.Size, FIntVector(16, 16, 16));
	TestEqual(TEXT("eleven blocks"), Result.Blocks, int64(11));
	TestEqual(TEXT("four source actors"), Result.SourceActors, 4);
	if (TestEqual(TEXT("two block types: the plain cube and the other material"), Result.TypeNames.Num(), 2))
	{
		TestEqual(TEXT("named after the mesh"), Result.TypeNames[0], FString(TEXT("cube")));
		TestEqual(TEXT("the second made unique"), Result.TypeNames[1], FString(TEXT("cube_2")));
	}
	const AMantleVoxelWorldActor* World = Result.World;
	for (const FMantleVoxelBlockTypeSettings& Type : World->BlockTypes)
	{
		if (TestNotNull(TEXT("each type has a tile"), Type.Top.Get()))
		{
			TestEqual(TEXT("the tile is Tile Size wide"), Type.Top->Source.GetSizeX(), World->TileSize);
			TestEqual(TEXT("and square"), Type.Top->Source.GetSizeY(), World->TileSize);
			TestTrue(TEXT("on every face"), Type.Side == Type.Top && Type.Bottom == Type.Top);
		}
	}
	TestEqual(TEXT("the default tile size"), World->TileSize, 32);
	TestEqual(TEXT("it holds nothing a place cannot carry"), World->DescribeProblems().Num(), 0);

	// The sources: tagged, in their folder, hidden, and left out of the place.
	for (const AActor* Source : Sources)
	{
		TestTrue(TEXT("a source is tagged"), Source->ActorHasTag(AMantleVoxelWorldActor::VoxelizedSourceTag));
		TestEqual(TEXT("in the Voxelized sources folder"), Source->GetFolderPath(), AMantleVoxelWorldActor::VoxelizedSourceFolder);
		TestFalse(TEXT("hidden"), CastChecked<AStaticMeshActor>(Source)->GetStaticMeshComponent()->IsVisible());
		TestTrue(TEXT("and left out by the exporter"), FMantlePlaceExporter::IsEngineInfrastructure(Source));
	}

	// Voxelizing them again finds nothing: they are already blocks.
	const FMantleVoxelizeResult Again = FMantleVoxelize::Voxelize(Fixture.World, Sources, FMantleVoxelizeOptions());
	TestFalse(TEXT("voxelized meshes are not voxelized twice"), Again.IsOk());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVoxelizeFileReadsBackTest,
	"Mantle.Editor.Voxelize.WorldFileReadsBack",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVoxelizeFileReadsBackTest::RunTest(const FString& Parameters)
{
	using namespace MantleVoxelizeTestPrivate;
	UStaticMesh* Cube = LoadCube();
	FVoxelWorld Fixture;
	if (!TestNotNull(TEXT("the engine cube"), Cube) || !TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	const FMantleVoxelizeResult Result = FMantleVoxelize::Voxelize(Fixture.World, SpawnCubeScene(Fixture.World, Cube), FMantleVoxelizeOptions());
	if (!TestTrue(FString::Printf(TEXT("it voxelizes (%s)"), *Result.Error), Result.IsOk()))
	{
		return false;
	}
	FMantleVoxelWorldFile File;
	FString Error;
	if (!TestTrue(FString::Printf(TEXT("the world's MBW1 reads back (%s)"), *Error), FMantleVoxelWorldFile::Read(Result.World->WorldData, File, &Error)))
	{
		return false;
	}
	TestEqual(TEXT("at the world's size"), File.Size, Result.World->Size);

	// Into the game's own grid, each block where its cube was (cells relative to the world's corner at cell -2).
	TArray<FMantleVoxelBlockType> Types;
	Types.AddDefaulted(3);
	FMantleVoxelGrid Grid;
	if (!TestTrue(TEXT("a grid of its size"), Grid.Init(File.Size, MoveTemp(Types))))
	{
		return false;
	}
	int32 Unknown = 0;
	TestTrue(TEXT("the file fills it"), File.ApplyTo(Grid, &Unknown));
	TestEqual(TEXT("every id has a type"), Unknown, 0);
	TestEqual(TEXT("cube A is a plain cube block"), Grid.GetBlock(FIntVector(2, 0, 0)), uint8(1));
	TestEqual(TEXT("cube B too"), Grid.GetBlock(FIntVector(3, 0, 0)), uint8(1));
	TestEqual(TEXT("cube C, below the origin, too"), Grid.GetBlock(FIntVector(0, 0, 0)), uint8(1));
	TestEqual(TEXT("between C and A is air"), Grid.GetBlock(FIntVector(1, 0, 0)), uint8(0));
	TestEqual(TEXT("the big cube fills its corner"), Grid.GetBlock(FIntVector(4, 0, 0)), uint8(2));
	TestEqual(TEXT("and its far corner"), Grid.GetBlock(FIntVector(5, 1, 1)), uint8(2));
	TestEqual(TEXT("and no further"), Grid.GetBlock(FIntVector(6, 0, 0)), uint8(0));
	TestEqual(TEXT("nothing above it"), Grid.GetBlock(FIntVector(4, 0, 2)), uint8(0));

	const TArray<int64> Counts = Result.World->CountBlocks();
	if (TestTrue(TEXT("its blocks are counted"), Counts.Num() > 2))
	{
		TestEqual(TEXT("three plain cubes"), Counts[1], int64(3));
		TestEqual(TEXT("eight blocks of the big cube"), Counts[2], int64(8));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVoxelizeImportTest,
	"Mantle.Editor.Voxelize.ImportGivesPlaceholderTypes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVoxelizeImportTest::RunTest(const FString& Parameters)
{
	using namespace MantleVoxelizeTestPrivate;
	FVoxelWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	// Ids 1, 3 and 5 used: five placeholder types, the largest id.
	const TArray<uint8> Bytes = WriteFile(FIntVector(32, 16, 16), { { FIntVector(0, 0, 0), 1 }, { FIntVector(17, 3, 2), 3 }, { FIntVector(31, 15, 15), 5 } });
	const FMantleVoxelizeResult Result = FMantleVoxelize::ImportMbw(Fixture.World, Bytes, FVector(1000.0, 0.0, 0.0), FMantleVoxelizeOptions());
	if (!TestTrue(FString::Printf(TEXT("it imports (%s)"), *Result.Error), Result.IsOk()))
	{
		return false;
	}
	const AMantleVoxelWorldActor* World = Result.World;
	TestEqual(TEXT("its size is the file's"), World->Size, FIntVector(32, 16, 16));
	TestEqual(TEXT("three blocks"), World->BlockCount, int64(3));
	TestEqual(TEXT("at the location asked"), World->GetActorLocation(), FVector(1000.0, 0.0, 0.0));
	if (TestEqual(TEXT("one placeholder type per id up to the largest"), World->BlockTypes.Num(), 5))
	{
		TestEqual(TEXT("block_1 first"), World->BlockTypes[0].Name, FString(TEXT("block_1")));
		TestEqual(TEXT("block_5 last"), World->BlockTypes[4].Name, FString(TEXT("block_5")));
		const UTexture2D* Tile = World->BlockTypes[0].Top.Get();
		if (TestNotNull(TEXT("with a grey tile"), Tile))
		{
			TestEqual(TEXT("of the tile size"), Tile->Source.GetSizeX(), World->TileSize);
		}
	}
	TestTrue(TEXT("the blocks it holds are the file's"), World->WorldData == Bytes);
	TestEqual(TEXT("it holds nothing a place cannot carry"), World->DescribeProblems().Num(), 0);

	// A smooth world's file is refused, saying why.
	TArray<uint8> Smooth = Bytes;
	Smooth[6] = 1;
	const FMantleVoxelizeResult Refused = FMantleVoxelize::ImportMbw(Fixture.World, Smooth, FVector::ZeroVector, FMantleVoxelizeOptions());
	TestFalse(TEXT("a smooth world is not imported"), Refused.IsOk());
	TestTrue(TEXT("and the message says it is smooth"), Refused.Error.Contains(TEXT("smooth")));

	// Something that is not a world file at all.
	const TArray<uint8> Junk = { 'N', 'O', 'P', 'E' };
	TestFalse(TEXT("a file that is not MBW1 is refused"), FMantleVoxelize::ImportMbw(Fixture.World, Junk, FVector::ZeroVector, FMantleVoxelizeOptions()).IsOk());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVoxelizeExportTest,
	"Mantle.Editor.Voxelize.ExportWritesVoxelWorld",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVoxelizeExportTest::RunTest(const FString& Parameters)
{
	using namespace MantleVoxelizeTestPrivate;
	UStaticMesh* Cube = LoadCube();
	FVoxelWorld Fixture;
	if (!TestNotNull(TEXT("the engine cube"), Cube) || !TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	const FMantleVoxelizeResult Voxelized = FMantleVoxelize::Voxelize(Fixture.World, SpawnCubeScene(Fixture.World, Cube), FMantleVoxelizeOptions());
	if (!TestTrue(FString::Printf(TEXT("it voxelizes (%s)"), *Voxelized.Error), Voxelized.IsOk()))
	{
		return false;
	}
	AMantleVoxelWorldActor* Actor = Voxelized.World;
	Actor->Edit = EMantleVoxelWorldEdit::Script;
	Actor->Persistence = EMantleVoxelWorldPersistence::Place;

	const FMantleExportResult Result = FMantlePlaceExporter::Export(Fixture.World, FMantleExportOptions());
	for (const FMantleParseError& Error : Result.Errors)
	{
		AddInfo(FString::Printf(TEXT("export: %s %s"), *Error.Path, *Error.Message));
	}
	TestFalse(TEXT("the voxelized cubes are not published as meshes"),
		Result.Place.Entities.ContainsByPredicate([](const FMantleEntity& Entity) { return Entity.Type == EMantleEntityType::StaticMesh; }));
	const int32 Index = Result.Place.Entities.IndexOfByPredicate([](const FMantleEntity& Entity) { return Entity.Type == EMantleEntityType::VoxelWorld; });
	if (!TestTrue(TEXT("a VoxelWorld entity"), Index != INDEX_NONE))
	{
		return false;
	}
	const FMantleEntity& Entity = Result.Place.Entities[Index];
	const FString EntityPath = FString::Printf(TEXT("/entities/%d"), Index);
	TestFalse(TEXT("export does not refuse it"), Result.Errors.ContainsByPredicate([&EntityPath](const FMantleParseError& Error) { return Error.Path.StartsWith(EntityPath); }));
	TestTrue(TEXT("at the world's corner"), Entity.Transform.GetLocation().Equals(FVector(-200.0, 0.0, 0.0)));
	TestTrue(TEXT("not rotated"), Entity.Transform.GetRotation().Equals(FQuat::Identity));
	TestTrue(TEXT("nor scaled"), Entity.Transform.GetScale3D().Equals(FVector::OneVector));
	if (const FMantleVoxelWorldProps* Props = Entity.Props.TryGet<FMantleVoxelWorldProps>(); TestNotNull(TEXT("with VoxelWorld properties"), Props))
	{
		TestEqual(TEXT("its size"), Props->Size, FIntVector(16, 16, 16));
		TestEqual(TEXT("its tile size"), Props->TileSize, 32);
		TestFalse(TEXT("blocky"), Props->bSmooth);
		TestTrue(TEXT("its edit setting"), Props->Edit == EMantleVoxelEdit::Script);
		TestTrue(TEXT("its persistence"), Props->Persistence == EMantleVoxelPersistence::Place);
		TestFalse(TEXT("its blocks by an upload id"), Props->World.IsEmpty());
		if (TestEqual(TEXT("two block types"), Props->BlockTypes.Num(), 2))
		{
			TestEqual(TEXT("named as Voxelize named them"), Props->BlockTypes[0].Name, FString(TEXT("cube")));
			TestFalse(TEXT("with a tile id"), Props->BlockTypes[0].Top.IsEmpty());
		}
		const FMantleAssetRef* Blocks = Result.Place.Assets.FindByPredicate([Props](const FMantleAssetRef& Asset) { return Asset.Id == Props->World; });
		if (TestNotNull(TEXT("its blocks are in the place's assets"), Blocks))
		{
			TestTrue(TEXT("as a VoxelWorldData"), Blocks->Kind == EMantleAssetKind::VoxelWorldData);
		}
		TestEqual(TEXT("its two tiles are listed as textures"), Result.Place.Assets.FilterByPredicate([](const FMantleAssetRef& Asset) { return Asset.Kind == EMantleAssetKind::Texture; }).Num(), 2);
	}

	// The place file reads back as the game reads it.
	const FMantleParseResult Parsed = FMantlePlaceParser::ParseFromString(FMantlePlaceExporter::ToJson(Result.Place));
	for (const FMantleParseError& Error : Parsed.Errors)
	{
		AddInfo(FString::Printf(TEXT("parse: %s %s"), *Error.Path, *Error.Message));
	}
	TestFalse(TEXT("the parser takes the VoxelWorld"), Parsed.Errors.ContainsByPredicate([&EntityPath](const FMantleParseError& Error) { return Error.Path.StartsWith(EntityPath); }));
	const FMantleEntity* Read = Parsed.Place.Entities.FindByPredicate([](const FMantleEntity& Candidate) { return Candidate.Type == EMantleEntityType::VoxelWorld; });
	if (TestNotNull(TEXT("and reads it back"), Read))
	{
		const FMantleVoxelWorldProps* Props = Read->Props.TryGet<FMantleVoxelWorldProps>();
		TestTrue(TEXT("with its block types"), Props && Props->BlockTypes.Num() == 2 && Props->BlockTypes[1].Name == TEXT("cube_2"));
	}

	// Publish gathers the blocks and both tiles.
	const FMantlePlaceDependencies Dependencies = FMantlePlaceDependencies::Gather(Fixture.World);
	TestEqual(TEXT("the blocks are gathered"), Dependencies.VoxelWorlds.Num(), 1);
	TestEqual(TEXT("both tiles are gathered"), Dependencies.Textures.FilterByPredicate([](const FMantleGatheredTexture& Texture) { return Texture.bVoxelTile; }).Num(), 2);
	TestEqual(TEXT("no mesh of the cubes is"), Dependencies.Meshes.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVoxelizeCheckCountTest,
	"Mantle.Editor.Voxelize.CheckRefusesASecondBlockyWorld",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVoxelizeCheckCountTest::RunTest(const FString& Parameters)
{
	using namespace MantleVoxelizeTestPrivate;
	FVoxelWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	const TArray<uint8> Bytes = WriteFile(FIntVector(16, 16, 16), { { FIntVector(0, 0, 0), 1 } });
	auto Import = [&](const FVector& Location, const TCHAR* Label)
	{
		const FMantleVoxelizeResult Result = FMantleVoxelize::ImportMbw(Fixture.World, Bytes, Location, FMantleVoxelizeOptions());
		if (Result.World)
		{
			Result.World->SetActorLabel(Label);
		}
		return Result.World;
	};
	if (!TestNotNull(TEXT("a first world"), Import(FVector::ZeroVector, TEXT("World A"))))
	{
		return false;
	}
	FMantleValidationResult One = FMantlePlaceValidator::Validate(Fixture.World);
	TestFalse(TEXT("one block world is fine"), One.Issues.ContainsByPredicate([](const FMantleValidationIssue& Issue)
	{
		return Issue.Severity == EMantleIssueSeverity::Error && Issue.Actor == TEXT("World A");
	}));

	// A second blocky one: one of each look.
	if (!TestNotNull(TEXT("a second world"), Import(FVector(5000.0, 0.0, 0.0), TEXT("World B"))))
	{
		return false;
	}
	const FMantleValidationResult Two = FMantlePlaceValidator::Validate(Fixture.World);
	const bool bSecondNamed = HasErrorContaining(Two, TEXT("World A"), TEXT("second blocky")) || HasErrorContaining(Two, TEXT("World B"), TEXT("second blocky"));
	TestTrue(TEXT("Check refuses two blocky worlds"), bSecondNamed);

	// A third: past the place's two.
	if (!TestNotNull(TEXT("a third world"), Import(FVector(10000.0, 0.0, 0.0), TEXT("World C"))))
	{
		return false;
	}
	const FMantleValidationResult Three = FMantlePlaceValidator::Validate(Fixture.World);
	TestTrue(TEXT("Check refuses a third world"), Three.Issues.ContainsByPredicate([](const FMantleValidationIssue& Issue)
	{
		return Issue.Severity == EMantleIssueSeverity::Error && Issue.Problem.Contains(TEXT("block world number 3"));
	}));
	const FMantleExportResult Exported = FMantlePlaceExporter::Export(Fixture.World, FMantleExportOptions());
	TestTrue(TEXT("and Export refuses it too"), Exported.Errors.ContainsByPredicate([](const FMantleParseError& Error) { return Error.Message.Contains(TEXT("block world number 3")); }));

	// A world's own problems are named: a rotation, and a tile of the wrong size.
	AMantleVoxelWorldActor* Bad = Import(FVector(20000.0, 0.0, 0.0), TEXT("World D"));
	if (TestNotNull(TEXT("a fourth world"), Bad))
	{
		Bad->SetActorRotation(FRotator(0.0, 45.0, 0.0));
		Bad->TileSize = 64;
		const TArray<FMantleVoxelWorldProblem> Problems = Bad->DescribeProblems();
		TestTrue(TEXT("a rotated world is named"), Problems.ContainsByPredicate([](const FMantleVoxelWorldProblem& Problem) { return Problem.What.Contains(TEXT("rotated")); }));
		TestTrue(TEXT("a tile that is not Tile Size square is named"), Problems.ContainsByPredicate([](const FMantleVoxelWorldProblem& Problem) { return Problem.What.Contains(TEXT("pixels")); }));
		const FMantleValidationResult Four = FMantlePlaceValidator::Validate(Fixture.World);
		TestTrue(TEXT("and Check refuses it"), HasErrorContaining(Four, TEXT("World D"), TEXT("rotated")));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
