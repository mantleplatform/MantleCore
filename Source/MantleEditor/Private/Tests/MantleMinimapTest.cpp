// Copyright Joshua Eger. All Rights Reserved.
#include "MantleMinimap.h"
#include "MantlePlaceAssetConverter.h"
#include "MantlePlaceExporter.h"
#include "MantlePlaceParser.h"
#include "MantlePlaceSettingsActor.h"
#include "MantlePlaceTypes.h"
#include "MantlePlaceValidator.h"
#include "MantlePng.h"

#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

// #167: the place's map picture around the drawing. Cutting it into tiles, a creator's own picture stretched over the map, the
// lake outline's triangles, a tile as a color PNG texture, the drawn map of a small level (names say the surface, +X right,
// +Y down), Check on an own picture that is missing, and settings.minimap written only where it differs and read back.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleMinimapTestPrivate
{
	/** An empty editor world for one test, destroyed with it. */
	struct FTestWorld
	{
		UWorld* World = nullptr;

		FTestWorld() { World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld*/ false, TEXT("MantleMinimapTestWorld")); }

		~FTestWorld()
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

	/** The engine's 1 m cube, scaled and placed, labeled Label (the name the map reads). */
	AStaticMeshActor* SpawnCube(UWorld* World, const TCHAR* Label, const FVector& Location, const FVector& Scale)
	{
		UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		AStaticMeshActor* Actor = Cube ? World->SpawnActor<AStaticMeshActor>(Location, FRotator::ZeroRotator) : nullptr;
		if (Actor)
		{
			Actor->SetActorLabel(Label);
			Actor->GetStaticMeshComponent()->SetStaticMesh(Cube);
			Actor->GetStaticMeshComponent()->SetCollisionProfileName(TEXT("BlockAll"));
			Actor->SetActorScale3D(Scale);
			Actor->GetStaticMeshComponent()->UpdateBounds();
		}
		return Actor;
	}

	double Area(TConstArrayView<FVector2D> Corners, TConstArrayView<int32> Indices)
	{
		double Sum = 0.0;
		for (int32 Index = 0; Index + 2 < Indices.Num(); Index += 3)
		{
			const FVector2D A = Corners[Indices[Index]], B = Corners[Indices[Index + 1]], C = Corners[Indices[Index + 2]];
			Sum += FMath::Abs((B.X - A.X) * (C.Y - A.Y) - (B.Y - A.Y) * (C.X - A.X)) * 0.5;
		}
		return Sum;
	}

	TSharedPtr<FJsonObject> MinimapObjectOf(const FString& Json)
	{
		TSharedPtr<FJsonObject> Root;
		const TSharedPtr<FJsonObject>* Settings = nullptr;
		const TSharedPtr<FJsonObject>* Minimap = nullptr;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid()
			|| !Root->TryGetObjectField(TEXT("settings"), Settings) || !(*Settings)->TryGetObjectField(TEXT("minimap"), Minimap))
		{
			return nullptr;
		}
		return *Minimap;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMinimapTilesTest,
	"Mantle.Editor.Minimap.TilesCutThePictureInOrder",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMinimapTilesTest::RunTest(const FString& Parameters)
{
	// 5 x 3 km at 1 m per pixel: 3 columns by 2 rows. Each tile filled with its own color.
	FMantleMinimapPicture Picture;
	Picture.Grid = FMantleMinimapGrid::Plan(FBox2D(FVector2D(0.0, 0.0), FVector2D(500000.0, 300000.0)));
	TestEqual(TEXT("3 columns"), Picture.Grid.Columns, 3);
	TestEqual(TEXT("2 rows"), Picture.Grid.Rows, 2);
	Picture.Pixels.SetNumUninitialized(Picture.Grid.Width * Picture.Grid.Height);
	for (int32 Y = 0; Y < Picture.Grid.Height; ++Y)
	{
		for (int32 X = 0; X < Picture.Grid.Width; ++X)
		{
			const int32 Row = Y / Picture.Grid.TileHeight, Column = X / Picture.Grid.TileWidth;
			Picture.Pixels[Y * Picture.Grid.Width + X] = FColor(static_cast<uint8>(Row * 100), static_cast<uint8>(Column * 80), 7, 255);
		}
	}
	TestTrue(TEXT("a whole picture"), Picture.IsValid());
	for (int32 Row = 0; Row < Picture.Grid.Rows; ++Row)
	{
		for (int32 Column = 0; Column < Picture.Grid.Columns; ++Column)
		{
			const TArray<FColor> Tile = Picture.Tile(Row, Column);
			const FColor Want(static_cast<uint8>(Row * 100), static_cast<uint8>(Column * 80), 7, 255);
			TestEqual(FString::Printf(TEXT("tile %d,%d is its size"), Row, Column), Tile.Num(), Picture.Grid.TileWidth * Picture.Grid.TileHeight);
			TestTrue(FString::Printf(TEXT("tile %d,%d is its own part"), Row, Column),
				Tile.Num() > 0 && !Tile.ContainsByPredicate([&Want](const FColor& Pixel) { return Pixel != Want; }));
		}
	}
	TestEqual(TEXT("no tile past the grid"), Picture.Tile(2, 0).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMinimapOwnPictureTest,
	"Mantle.Editor.Minimap.OwnPictureStretchesWithAShapeWarning",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMinimapOwnPictureTest::RunTest(const FString& Parameters)
{
	// A 2 x 1 picture, red on the left (west) and blue on the right, over a map twice as wide as it is tall.
	const TArray<FColor> Image = { FColor::Red, FColor::Blue };
	const FMantleMinimapGrid Wide = FMantleMinimapGrid::Plan(FBox2D(FVector2D(0.0, 0.0), FVector2D(204800.0, 102400.0)));
	TArray<FColor> Pixels;
	FString Warning, Error;
	TestTrue(TEXT("stretched"), FMantleMinimap::FromImage(Image, 2, 1, Wide, Pixels, Warning, Error));
	TestEqual(TEXT("one pixel per map pixel"), Pixels.Num(), Wide.Width * Wide.Height);
	TestTrue(TEXT("the same shape: no warning"), Warning.IsEmpty());
	if (Pixels.Num() == Wide.Width * Wide.Height)
	{
		TestTrue(TEXT("west is the picture's left"), Pixels[0].R > 200 && Pixels[0].B < 50);
		TestTrue(TEXT("east is its right"), Pixels[Wide.Width - 1].B > 200 && Pixels[Wide.Width - 1].R < 50);
		TestTrue(TEXT("opaque"), !Pixels.ContainsByPredicate([](const FColor& Pixel) { return Pixel.A != 255; }));
	}

	// Over a square map it is stretched, and the creator is told.
	const FMantleMinimapGrid Square = FMantleMinimapGrid::Plan(FBox2D(FVector2D(0.0, 0.0), FVector2D(102400.0, 102400.0)));
	TestTrue(TEXT("stretched over a square"), FMantleMinimap::FromImage(Image, 2, 1, Square, Pixels, Warning, Error));
	TestFalse(TEXT("with a warning"), Warning.IsEmpty());
	TestTrue(TEXT("within 2%: no warning"), FMantleMinimap::DescribeShape(1010, 1000, Square).IsEmpty());
	TestFalse(TEXT("past 2%: a warning"), FMantleMinimap::DescribeShape(1030, 1000, Square).IsEmpty());

	TestFalse(TEXT("no pixels, no picture"), FMantleMinimap::FromImage(TArray<FColor>(), 0, 0, Square, Pixels, Warning, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMinimapTriangulateTest,
	"Mantle.Editor.Minimap.LakeOutlinesFillExactly",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMinimapTriangulateTest::RunTest(const FString& Parameters)
{
	using namespace MantleMinimapTestPrivate;
	const TArray<FVector2D> Square = { FVector2D(0, 0), FVector2D(10, 0), FVector2D(10, 10), FVector2D(0, 10) };
	TArray<int32> Indices = FMantleMinimap::Triangulate(Square);
	TestEqual(TEXT("a square is two triangles"), Indices.Num(), 6);
	TestEqual(TEXT("covering it"), Area(Square, Indices), 100.0, 1e-6);

	// An L, clockwise: concave, so a fan from one corner would spill outside it.
	const TArray<FVector2D> L = { FVector2D(0, 0), FVector2D(0, 20), FVector2D(10, 20), FVector2D(10, 10), FVector2D(20, 10), FVector2D(20, 0) };
	Indices = FMantleMinimap::Triangulate(L);
	TestEqual(TEXT("an L is four triangles"), Indices.Num(), 12);
	TestEqual(TEXT("covering exactly its area"), Area(L, Indices), 300.0, 1e-6);

	TestEqual(TEXT("two corners are nothing"), FMantleMinimap::Triangulate({ FVector2D(0, 0), FVector2D(1, 1) }).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMinimapTileTextureTest,
	"Mantle.Editor.Minimap.TileIsAnOpaqueColorPng",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMinimapTileTextureTest::RunTest(const FString& Parameters)
{
	FMantlePlaceAssetConverter Converter;
	FMantlePlaceAssetSet Set;
	TArray<FColor> Pixels;
	Pixels.Init(FColor(156, 200, 240, 0), 64 * 32);
	FString Key;
	TestTrue(TEXT("converted"), Converter.ConvertMinimapTile(Pixels, 64, 32, Set, Key));
	TestTrue(TEXT("a color texture key"), Key.StartsWith(TEXT("texture:color:")));
	const FMantleConvertedTexture* Texture = Set.FindTexture(Key);
	if (TestNotNull(TEXT("in the set"), Texture))
	{
		TestEqual(TEXT("PNG, not JPEG"), Texture->Format, FString(TEXT("png")));
		TestEqual(TEXT("color role (BC1 for an opaque picture)"), Texture->Role, FString(TEXT("color")));
		int32 Width = 0, Height = 0;
		bool bAlpha = true;
		TestTrue(TEXT("a PNG header"), FMantlePng::ReadHeader(Texture->Bytes, Width, Height, bAlpha));
		TestEqual(TEXT("its width"), Width, 64);
		TestEqual(TEXT("its height"), Height, 32);
	}
	FString Again;
	Converter.ConvertMinimapTile(Pixels, 64, 32, Set, Again);
	TestEqual(TEXT("the same pixels are the same upload"), Again, Key);
	TestEqual(TEXT("held once"), Set.Textures.Num(), 1);

	TArray<FColor> TooBig;
	TooBig.Init(FColor::White, 2049 * 1);
	TestFalse(TEXT("a tile over 2048 is refused"), Converter.ConvertMinimapTile(TooBig, 2049, 1, Set, Key));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMinimapDrawLevelTest,
	"Mantle.Editor.Minimap.DrawsALevelFromItsMeshes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMinimapDrawLevelTest::RunTest(const FString& Parameters)
{
	using namespace MantleMinimapTestPrivate;
	FTestWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	// A 100 x 100 m floor, a road across its east half and a 20 m tower in its south-west quarter (+Y is south).
	SpawnCube(Fixture.World, TEXT("Floor"), FVector(0.0, 0.0, -50.0), FVector(100.0, 100.0, 1.0));
	SpawnCube(Fixture.World, TEXT("Road_Main"), FVector(2500.0, 0.0, 5.0), FVector(10.0, 100.0, 0.1));
	SpawnCube(Fixture.World, TEXT("Tower"), FVector(-2500.0, 2500.0, 1000.0), FVector(10.0, 10.0, 20.0));

	FMantleMinimapPicture Picture;
	FString Error;
	if (!TestTrue(TEXT("drawn"), FMantleMinimap::Draw(Fixture.World, Picture, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("the 100 m floor is 1024 pixels across"), Picture.Grid.Width, 1024);
	auto At = [&Picture](double X, double Y)
	{
		const FVector2D Pixel = Picture.Grid.ToPixel(X, Y);
		return Picture.Pixels[FMath::FloorToInt32(Pixel.Y) * Picture.Grid.Width + FMath::FloorToInt32(Pixel.X)];
	};
	TestEqual(TEXT("the road is road-colored"), At(2500.0, -3000.0), FMantleMinimapCanvas::ColorOf(EMantleMinimapSurface::Road));
	TestEqual(TEXT("the floor is ground"), At(-2500.0, -2500.0), FMantleMinimapCanvas::ColorOf(EMantleMinimapSurface::Ground));
	const FColor Tower = At(-2500.0, 2500.0);
	TestTrue(TEXT("the tower is a building, in the south-west (+Y down, -X left)"), Tower != FMantleMinimapCanvas::ColorOf(EMantleMinimapSurface::Ground)
		&& Tower != FMantleMinimapCanvas::ColorOf(EMantleMinimapSurface::Road) && Tower.R < 232);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMinimapCheckOwnPictureTest,
	"Mantle.Editor.Minimap.CheckRefusesAMissingOwnPicture",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMinimapCheckOwnPictureTest::RunTest(const FString& Parameters)
{
	using namespace MantleMinimapTestPrivate;
	FTestWorld Fixture;
	AMantlePlaceSettingsActor* Settings = Fixture.World ? AMantlePlaceSettingsActor::FindOrAdd(Fixture.World) : nullptr;
	if (!TestNotNull(TEXT("a settings actor"), Settings))
	{
		return false;
	}
	SpawnCube(Fixture.World, TEXT("Floor"), FVector(0.0, 0.0, -50.0), FVector(100.0, 100.0, 1.0));
	auto Errors = [&Fixture]()
	{
		TArray<FMantleValidationIssue> Issues;
		FMantleMinimap::Check(Fixture.World, Issues);
		return Issues.FilterByPredicate([](const FMantleValidationIssue& Issue) { return Issue.Severity == EMantleIssueSeverity::Error; }).Num();
	};
	TestEqual(TEXT("made automatically: nothing to check"), Errors(), 0);
	Settings->MapPicture = EMantlePlaceMapPicture::Own;
	TestEqual(TEXT("own picture with none chosen: an error"), Errors(), 1);
	Settings->MapPictureFile.FilePath = TEXT("D:/no/such/folder/map.png");
	TestEqual(TEXT("own picture that does not exist: an error"), Errors(), 1);
	FMantleMinimapPicture Picture;
	FString Error;
	TestFalse(TEXT("and no picture is made"), FMantleMinimap::Make(Fixture.World, Picture, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMinimapSettingsWriteTest,
	"Mantle.Editor.Minimap.SettingsWriteOnlyChangesAndReadBack",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMinimapSettingsWriteTest::RunTest(const FString& Parameters)
{
	using namespace MantleMinimapTestPrivate;
	FTestWorld Fixture;
	AMantlePlaceSettingsActor* Settings = Fixture.World ? AMantlePlaceSettingsActor::FindOrAdd(Fixture.World) : nullptr;
	if (!TestNotNull(TEXT("a settings actor"), Settings))
	{
		return false;
	}

	// The defaults and no picture: no minimap key at all.
	FMantlePlace Defaults;
	AMantlePlaceSettingsActor::ApplyTo(Fixture.World, Defaults.Settings);
	TestFalse(TEXT("the minimap is off by default"), Defaults.Settings.Minimap.bEnabled);
	TestFalse(TEXT("no minimap is written"), MinimapObjectOf(FMantlePlaceExporter::ToJson(Defaults)).IsValid());

	// Every field changed, a picture of 1 x 2 tiles.
	Settings->bShowMinimap = true;
	Settings->MinimapShape = EMantlePlaceMinimapShape::Square;
	Settings->MinimapAnchor = EMantlePlaceMinimapAnchor::BottomRight;
	Settings->MinimapOffset = FVector2D(40.0, -24.0);
	Settings->MinimapSize = 9999.0;
	Settings->bMinimapRim = false;
	Settings->MinimapRotate = EMantlePlaceMinimapRotate::North;
	Settings->MinimapRange = 400.0;
	Settings->bMinimapFullMap = false;
	Settings->MinimapPlayers = EMantlePlaceMinimapPlayers::Off;
	FMantlePlace Changed;
	AMantlePlaceSettingsActor::ApplyTo(Fixture.World, Changed.Settings);
	TestEqual(TEXT("a size past the range is clamped"), Changed.Settings.Minimap.Size, 512.0);
	FMantleMinimapImage& Image = Changed.Settings.Minimap.Image;
	Image.Tiles = { { TEXT("ast_tile0000"), TEXT("ast_tile0001") } };
	Image.MinX = -1000.0;
	Image.MaxX = 3000.0;
	Image.MinY = -500.0;
	Image.MaxY = 1500.0;
	const FString Json = FMantlePlaceExporter::ToJson(Changed);
	const TSharedPtr<FJsonObject> Minimap = MinimapObjectOf(Json);
	if (TestTrue(TEXT("the minimap is written"), Minimap.IsValid()))
	{
		TestTrue(TEXT("enabled"), Minimap->GetBoolField(TEXT("enabled")));
		TestEqual(TEXT("shape"), Minimap->GetStringField(TEXT("shape")), FString(TEXT("square")));
		TestEqual(TEXT("anchor"), Minimap->GetStringField(TEXT("anchor")), FString(TEXT("bottomRight")));
		TestEqual(TEXT("rotate"), Minimap->GetStringField(TEXT("rotate")), FString(TEXT("north")));
		TestEqual(TEXT("players"), Minimap->GetStringField(TEXT("players")), FString(TEXT("off")));
		TestTrue(TEXT("the picture is written"), Minimap->HasField(TEXT("image")));
	}
	const FMantleParseResult Parsed = FMantlePlaceParser::ParseFromString(Json);
	TestFalse(TEXT("the parser finds no minimap error"), Parsed.Errors.ContainsByPredicate([](const FMantleParseError& Error)
	{
		return Error.Path.StartsWith(TEXT("/settings/minimap"));
	}));
	const FMantleMinimapSettings& Read = Parsed.Place.Settings.Minimap;
	TestTrue(TEXT("read back on"), Read.bEnabled);
	TestTrue(TEXT("read back square"), Read.Shape == EMantleMinimapShape::Square);
	TestTrue(TEXT("read back bottom right"), Read.Anchor == EMantleMinimapAnchor::BottomRight);
	TestTrue(TEXT("read back the offset"), Read.Offset.Equals(FVector2D(40.0, -24.0), 1e-9));
	TestEqual(TEXT("read back the size"), Read.Size, 512.0);
	TestFalse(TEXT("read back no rim"), Read.bRim);
	TestTrue(TEXT("read back north up"), Read.Rotate == EMantleMinimapRotate::North);
	TestEqual(TEXT("read back the range"), Read.Range, 400.0);
	TestFalse(TEXT("read back no full map"), Read.bFullMap);
	TestTrue(TEXT("read back no players"), Read.Players == EMantleMinimapPlayers::Off);
	TestEqual(TEXT("read back one row"), Read.Image.Tiles.Num(), 1);
	TestEqual(TEXT("of two tiles"), Read.Image.Tiles.Num() == 1 ? Read.Image.Tiles[0].Num() : 0, 2);
	TestEqual(TEXT("read back the bounds"), Read.Image.MaxX, 3000.0);

	// Loading the place file's settings into the actor and out again keeps them.
	Settings->bShowMinimap = false;
	Settings->MinimapShape = EMantlePlaceMinimapShape::Circle;
	AMantlePlaceSettingsActor::SetFrom(Fixture.World, Parsed.Place.Settings);
	TestTrue(TEXT("the actor shows the minimap again"), Settings->bShowMinimap);
	TestTrue(TEXT("and is square again"), Settings->MinimapShape == EMantlePlaceMinimapShape::Square);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
