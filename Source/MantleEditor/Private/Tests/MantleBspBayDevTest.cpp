// Copyright Joshua Eger. All Rights Reserved.
#include "Builders/CubeBuilder.h"
#include "Builders/SheetBuilder.h"
#include "Components/BrushComponent.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/Brush.h"
#include "Engine/Polys.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "Materials/MaterialInterface.h"
#include "Misc/AutomationTest.h"
#include "Misc/Parse.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Model.h"

#if WITH_DEV_AUTOMATION_TESTS

// A development helper, not a check: it builds the BSP test bay of the publish test level (a room with a doorway cut in the wrong name
// order, and a two-sided not-solid sheet), so the editor / Mantle pictures of the BSP bake (MantleBrushBake) can be shot without the
// editor's T3D import, which crashes in this engine. It runs only when MANTLE_LANE_D_BSP_BAY names a level; with it unset it does
// nothing, so a run of Mantle.Editor (or of every test) never touches a level. It is under Mantle.Dev, not Mantle.Editor.
//
//   UnrealEditor-Cmd.exe <project> -ExecCmds="Automation RunTests Mantle.Dev.LaneD.BuildBspBay" -unattended -nullrhi
//   MANTLE_LANE_D_BSP_BAY     the level to open, change and save, e.g. /Game/Test/L1_Main
//   MANTLE_LANE_D_BSP_ORIGIN  where the room goes, "X,Y,Z" in cm (default 0,0,0)
//   MANTLE_LANE_D_BSP_MAT_A   optional material for two faces of the wall (an object path)
//   MANTLE_LANE_D_BSP_MAT_B   optional material for its other faces
//   MANTLE_LANE_D_BSP_MAT_C   optional material for the hollow's faces (the room's inner walls)
//
// It makes, in this actor order (so the doorway, placed last, has a name that sorts before the wall's):
//   Brush_Z_Wall   Add cube 800 cm, the room's wall
//   Brush_M_Room   Subtract cube 600 cm, the hollow
//   Brush_S_Sheet  a 400 x 400 sheet, two-sided and not solid, beside the room
//   Brush_A_Door   Subtract box 200 x 300 x 300 cm through the wall, from the floor up
// then rebuilds the BSP and saves the level. Running it again replaces its own four brushes.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleBspBayDevPrivate
{
	ABrush* SpawnBrush(UWorld* World, const TCHAR* Name, const FVector& Location, EBrushType Type, uint32 PolyFlags, UBrushBuilder* Builder)
	{
		FActorSpawnParameters Parameters;
		Parameters.Name = FName(Name);
		Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		ABrush* Brush = World->SpawnActor<ABrush>(Parameters);
		if (!Brush)
		{
			return nullptr;
		}
		Brush->SetActorLabel(Name);
		if (!Brush->Brush)
		{
			Brush->Brush = NewObject<UModel>(Brush, NAME_None, RF_Transactional);
			Brush->Brush->Initialize(Brush, /*InRootOutside*/ true);
			Brush->GetBrushComponent()->Brush = Brush->Brush;
		}
		Brush->BrushType = Type;
		Brush->PolyFlags = PolyFlags;
		// UModel::Initialize put the actor at the origin; the builder keeps the location it finds.
		Brush->SetActorLocation(Location, false);
		Builder->Build(World, Brush);
		return Brush;
	}

	UCubeBuilder* CubeBuilder(const float X, const float Y, const float Z)
	{
		UCubeBuilder* Builder = NewObject<UCubeBuilder>();
		Builder->X = X;
		Builder->Y = Y;
		Builder->Z = Z;
		return Builder;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleBspBayDevTest,
	"Mantle.Dev.LaneD.BuildBspBay",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMantleBspBayDevTest::RunTest(const FString& Parameters)
{
	using namespace MantleBspBayDevPrivate;
	const FString MapPath = FPlatformMisc::GetEnvironmentVariable(TEXT("MANTLE_LANE_D_BSP_BAY"));
	if (MapPath.IsEmpty())
	{
		AddWarning(TEXT("MANTLE_LANE_D_BSP_BAY is not set: this helper builds the BSP bay in a level and does nothing otherwise"));
		return true;
	}
	if (!GEditor)
	{
		AddError(TEXT("no editor"));
		return false;
	}

	FVector Origin = FVector::ZeroVector;
	const FString OriginText = FPlatformMisc::GetEnvironmentVariable(TEXT("MANTLE_LANE_D_BSP_ORIGIN"));
	if (!OriginText.IsEmpty())
	{
		TArray<FString> Parts;
		OriginText.ParseIntoArray(Parts, TEXT(","));
		if (Parts.Num() == 3)
		{
			Origin = FVector(FCString::Atod(*Parts[0]), FCString::Atod(*Parts[1]), FCString::Atod(*Parts[2]));
		}
	}

	UWorld* World = UEditorLoadingAndSavingUtils::LoadMap(MapPath);
	if (!TestNotNull(*FString::Printf(TEXT("the level %s opened"), *MapPath), World))
	{
		return false;
	}

	// Its own four brushes from a run before go.
	TArray<ABrush*> Old;
	for (TActorIterator<ABrush> It(World); It; ++It)
	{
		const FString Name = It->GetName();
		if (Name == TEXT("Brush_Z_Wall") || Name == TEXT("Brush_M_Room") || Name == TEXT("Brush_S_Sheet") || Name == TEXT("Brush_A_Door"))
		{
			Old.Add(*It);
		}
	}
	for (ABrush* Brush : Old)
	{
		World->EditorDestroyActor(Brush, true);
	}

	// The room: a wall with a hollow, a sheet beside it, and the doorway last in the actor order.
	ABrush* Wall = SpawnBrush(World, TEXT("Brush_Z_Wall"), Origin, Brush_Add, 0, CubeBuilder(800.0f, 800.0f, 800.0f));
	ABrush* Room = SpawnBrush(World, TEXT("Brush_M_Room"), Origin, Brush_Subtract, 0, CubeBuilder(600.0f, 600.0f, 600.0f));
	USheetBuilder* SheetShape = NewObject<USheetBuilder>();
	SheetShape->X = 400;
	SheetShape->Y = 400;
	SheetShape->XSegments = 1;
	SheetShape->YSegments = 1;
	SheetShape->Axis = AX_XAxis;
	ABrush* Sheet = SpawnBrush(World, TEXT("Brush_S_Sheet"), Origin + FVector(0.0, 1000.0, 0.0), Brush_Add, PF_TwoSided | PF_NotSolid, SheetShape);
	ABrush* Door = SpawnBrush(World, TEXT("Brush_A_Door"), Origin + FVector(350.0, 0.0, -150.0), Brush_Subtract, 0, CubeBuilder(200.0f, 300.0f, 300.0f));
	if (!TestNotNull(TEXT("the wall"), Wall) || !TestNotNull(TEXT("the room"), Room) || !TestNotNull(TEXT("the sheet"), Sheet) || !TestNotNull(TEXT("the doorway"), Door))
	{
		return false;
	}

	// Materials, when given: two faces of the wall one look, its others another, so the texture mapping can be compared.
	UMaterialInterface* MaterialA = nullptr;
	UMaterialInterface* MaterialB = nullptr;
	const FString PathA = FPlatformMisc::GetEnvironmentVariable(TEXT("MANTLE_LANE_D_BSP_MAT_A"));
	const FString PathB = FPlatformMisc::GetEnvironmentVariable(TEXT("MANTLE_LANE_D_BSP_MAT_B"));
	if (!PathA.IsEmpty())
	{
		MaterialA = LoadObject<UMaterialInterface>(nullptr, *PathA);
		TestNotNull(TEXT("material A loaded"), MaterialA);
	}
	if (!PathB.IsEmpty())
	{
		MaterialB = LoadObject<UMaterialInterface>(nullptr, *PathB);
		TestNotNull(TEXT("material B loaded"), MaterialB);
	}
	if (MaterialA || MaterialB)
	{
		TArray<FPoly>& Polys = Wall->Brush->Polys->Element;
		for (int32 Index = 0; Index < Polys.Num(); ++Index)
		{
			UMaterialInterface* Material = Index < 2 ? MaterialA : MaterialB;
			if (Material)
			{
				Polys[Index].Material = Material;
			}
		}
	}

	// The hollow's faces become the room's inner walls: a UV-mapped material there can be compared with the editor's directly.
	const FString PathC = FPlatformMisc::GetEnvironmentVariable(TEXT("MANTLE_LANE_D_BSP_MAT_C"));
	if (!PathC.IsEmpty())
	{
		UMaterialInterface* MaterialC = LoadObject<UMaterialInterface>(nullptr, *PathC);
		if (TestNotNull(TEXT("material C loaded"), MaterialC))
		{
			for (FPoly& Poly : Room->Brush->Polys->Element)
			{
				Poly.Material = MaterialC;
			}
		}
	}

	// Build the BSP the editor's way, then save.
	GEditor->RebuildMap(World, UEditorEngine::MRT_Current);
	// Saved by its package, straight to its file: SaveCurrentLevel answers false in an unattended, no-render run, with nothing in the log.
	UPackage* Package = World->GetOutermost();
	Package->MarkPackageDirty();
	const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetMapPackageExtension());
	FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*Filename, false);
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	const FSavePackageResultStruct Saved = UPackage::Save(Package, World, *Filename, SaveArgs);
	TestTrue(*FString::Printf(TEXT("the level saved to %s"), *Filename), Saved.IsSuccessful());
	AddInfo(FString::Printf(TEXT("BSP bay built in %s at %s: Brush_Z_Wall, Brush_M_Room, Brush_S_Sheet, Brush_A_Door"), *MapPath, *Origin.ToString()));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
