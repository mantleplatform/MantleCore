// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAssetCleanup.h"
#include "MantleChassisDerive.h"
#include "MantleChassisSpec.h"
#include "MantlePlaceValidator.h"
#include "MantleVehicleBuilder.h"
#include "MantleVehicleChassisBuilder.h"
#include "MantleVehicleChassisDocument.h"
#include "MantleVehicleChassisSkin.h"
#include "MantleVehicleConverter.h"
#include "MantleVehiclePreset.h"
#include "MantleVehicleScratchBody.h"
#include "MantleVehicleShape.h"
#include "MantleVehicleSource.h"
#include "MantleVehicleSpawnPoint.h"
#include "SMantleVehicleBuilder.h"

#include "AssetRegistry/AssetData.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "LevelEditorViewport.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

// Covers the Build a vehicle window: the .fbx import, a build forgotten when the shape changes, what a flier is told on the
// Drive step and where it is put, the scale limits said, one propeller count, one set of assets per vehicle and its undo, movement blocks that do not read, and the feel
// sliders of the road types.

// The window's state, which a test drives the way the buttons do. Named, not anonymous: MantleEditor builds as a unity build.
struct FMantleVehicleBuilderFixAccess
{
	static void PickType(SMantleVehicleBuilder& Panel, const TCHAR* Id, const TCHAR* Name) { Panel.PickPreset(Id, FText::FromString(Name)); }
	static void SetMesh(SMantleVehicleBuilder& Panel, UObject* Mesh) { Panel.SetMesh(FAssetData(Mesh)); }
	static void Give(SMantleVehicleBuilder& Panel, UMantleVehicleSource* Built)
	{
		Panel.BuiltSource = Built;
		Panel.bHaveLastBuild = true;
		Panel.LastBuild.bOk = true;
		Panel.SuggestedSize = 1.25f;
		Panel.SaveStatus = FText::FromString(TEXT("Saved in your Content Browser."));
	}
	static bool HasBuild(const SMantleVehicleBuilder& Panel) { return Panel.BuiltSource.IsValid() || Panel.bHaveLastBuild || Panel.SuggestedSize.IsSet() || !Panel.SaveStatus.IsEmpty(); }
	static double Dimension(const SMantleVehicleBuilder& Panel, const EMantleVehicleDimension Which) { return Panel.Shape.GetDimensions().Get(Which); }
	static void SetDimension(SMantleVehicleBuilder& Panel, const EMantleVehicleDimension Which, const double Value) { Panel.SetDimension(Which, Value); }
	static void SetShapeSize(SMantleVehicleBuilder& Panel, const double Value) { Panel.SetShapeSize(Value); }
	static void SetFeel(SMantleVehicleBuilder& Panel, const EMantleVehicleFeel Which, const double Value) { Panel.SetFeel(Which, Value); }
	static void SetPassenger(SMantleVehicleBuilder& Panel, const bool bOn) { Panel.SetPassengerSeat(bOn); }
	static FMantleVehicleModelOptions Options(const SMantleVehicleBuilder& Panel) { return Panel.MakeBuildOptions(); }
	static void Remember(SMantleVehicleBuilder& Panel, const FMantleVehicleBuildResult& Result) { Panel.RememberBuilt(Result); }
	static void Import(SMantleVehicleBuilder& Panel, const FString& File) { Panel.ImportFbx(File); }
	static FText ImportStatus(const SMantleVehicleBuilder& Panel) { return Panel.ImportStatus; }
	static bool ImportFailed(const SMantleVehicleBuilder& Panel) { return Panel.bImportFailed; }
	static UObject* Mesh(const SMantleVehicleBuilder& Panel) { return Panel.Mesh.Get(); }
};

namespace MantleVehicleBuilderFixTestPrivate
{
	/** A static model: one box, one section, in the model's own units. */
	FMantleVehicleModel BoxModel(const FVector3f& Size)
	{
		FMantleVehicleModel Model;
		Model.Name = TEXT("Box");
		Model.Materials.Add(FSkeletalMaterial(static_cast<UMaterialInterface*>(nullptr), FName(TEXT("Skin"))));
		const FVector3f Half = 0.5f * Size;
		FMantleSkinnedMeshLOD& LOD = Model.LOD;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			for (const float Sign : { -1.0f, 1.0f })
			{
				FVector3f Normal(0.0f);
				Normal[Axis] = Sign;
				FVector3f U(0.0f);
				U[(Axis + 1) % 3] = 1.0f;
				FVector3f V(0.0f);
				V[(Axis + 2) % 3] = 1.0f;
				const uint32 First = static_cast<uint32>(LOD.Positions.Num());
				const FVector3f FaceCentre = Normal * Half[Axis];
				const float Du = Half[(Axis + 1) % 3];
				const float Dv = Half[(Axis + 2) % 3];
				const FVector3f Corners[4] = { FaceCentre - U * Du - V * Dv, FaceCentre + U * Du - V * Dv, FaceCentre + U * Du + V * Dv, FaceCentre - U * Du + V * Dv };
				for (const FVector3f& Corner : Corners)
				{
					LOD.Positions.Add(Corner);
					LOD.Normals.Add(Normal);
					LOD.UVs.Add(FVector2f(0, 0));
					LOD.InfluenceBones.Add(0);
					LOD.InfluenceWeights.Add(255);
				}
				for (const uint32 Index : { 0u, 1u, 2u, 0u, 2u, 3u })
				{
					LOD.Indices.Add(First + Index);
				}
			}
		}
		FMantleSkinnedMeshSection& Section = LOD.Sections.AddDefaulted_GetRef();
		Section.MaterialIndex = 0;
		Section.FirstIndex = 0;
		Section.IndexCount = LOD.Indices.Num();
		Section.MaterialName = FName(TEXT("Skin"));
		LOD.NumInfluences = 1;
		return Model;
	}

	/** The chassis of a type and its grey body. */
	bool Load(FAutomationTestBase& Test, const TCHAR* Id, FMantleChassisSpec& OutSpec, FMantleVehicleAsset& OutGrey)
	{
		FString Error;
		const bool bOk = FMantleVehicleChassisBuilder::LoadSpec(Id, OutSpec, Error) && FMantleVehicleChassisBuilder::MakeAsset(OutSpec, OutGrey, Error);
		Test.TestTrue(*FString::Printf(TEXT("the %s chassis loads (%s)"), Id, *Error), bOk);
		return bOk;
	}

	bool Near(const double A, const double B, const double Relative)
	{
		return FMath::Abs(A - B) <= Relative * FMath::Max(FMath::Abs(A), FMath::Abs(B)) + 1.0e-9;
	}

	/** True when any of Notes contains Text. */
	bool Says(const TArray<FString>& Notes, const TCHAR* Text)
	{
		return Notes.ContainsByPredicate([Text](const FString& Note) { return Note.Contains(Text); });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleBuilderImportFbxTest,
	"Mantle.Vehicle.Builder.ImportsAnFbxFile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleBuilderImportFbxTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleBuilderFixTestPrivate;

	// What goes wrong first, said in plain words: no file, a missing one, one that is not an .fbx.
	FString Error;
	TestNull(TEXT("no file imports nothing"), FMantleVehicleBuilder::ImportModelFile(FString(), Error));
	TestTrue(TEXT("and asks for one"), Error.Contains(TEXT("Pick an .fbx file")));
	TestNull(TEXT("a missing file imports nothing"), FMantleVehicleBuilder::ImportModelFile(TEXT("C:/NoSuchFolder/Nothing.fbx"), Error, FString(), /*bSave*/ false));
	TestTrue(TEXT("and says it cannot find it"), Error.Contains(TEXT("can't find")) && Error.Contains(TEXT("Nothing.fbx")));
	TestNull(TEXT("a file that is not an .fbx imports nothing"), FMantleVehicleBuilder::ImportModelFile(TEXT("C:/NoSuchFolder/Car.obj"), Error, FString(), /*bSave*/ false));
	TestTrue(TEXT("and says what to do"), Error.Contains(TEXT("isn't an .fbx file")));

	// The window says so too, and picks nothing.
	if (FSlateApplication::IsInitialized())
	{
		TSharedRef<SMantleVehicleBuilder> Panel = SNew(SMantleVehicleBuilder);
		FMantleVehicleBuilderFixAccess::Import(*Panel, TEXT("C:/NoSuchFolder/Nothing.fbx"));
		TestTrue(TEXT("the window shows the failure in red"), FMantleVehicleBuilderFixAccess::ImportFailed(*Panel) && !FMantleVehicleBuilderFixAccess::ImportStatus(*Panel).IsEmpty());
		TestNull(TEXT("and picks no model"), FMantleVehicleBuilderFixAccess::Mesh(*Panel));
	}
	else
	{
		AddWarning(TEXT("SKIPPED, not passed: there is no Slate application in this runner, so the window's import message is not checked."));
	}

	// Real files, from the engine's own automation content: a cube (a static mesh) and a character with a skeleton (a skeletal mesh). Nothing is saved.
	const FString Cube = FPaths::ConvertRelativePathToFull(FPaths::EngineContentDir() / TEXT("FbxEditorAutomation/BlenderCube.fbx"));
	const FString Character = FPaths::ConvertRelativePathToFull(FPaths::EngineContentDir() / TEXT("FbxEditorAutomation/AnimatedCharacter.fbx"));
	if (!IFileManager::Get().FileExists(*Cube) || !IFileManager::Get().FileExists(*Character))
	{
		AddWarning(TEXT("SKIPPED, not passed: the engine's FbxEditorAutomation files are not installed, so no real .fbx was imported."));
		return true;
	}
	const FString Folder = TEXT("/Game/MantleVehicles/Imported/_AutomationTest");

	TArray<UObject*> CubeMade;
	UObject* CubeModel = FMantleVehicleBuilder::ImportModelFile(Cube, Error, Folder / TEXT("Cube"), /*bSave*/ false, &CubeMade);
	if (TestNotNull(*FString::Printf(TEXT("the cube is imported (%s)"), *Error), CubeModel))
	{
		TestTrue(TEXT("as a static mesh"), CubeModel->IsA<UStaticMesh>());
		TestTrue(TEXT("under the folder asked for"), CubeModel->GetOutermost()->GetName().StartsWith(Folder / TEXT("Cube")));
		FMantleVehicleModel Read;
		TestTrue(*FString::Printf(TEXT("and the vehicle builder can read it as a model (%s)"), *Error), FMantleVehicleModel::Read(CubeModel, Read, Error) && !Read.bSkeletal && Read.NumTriangles() > 0);
	}

	TArray<UObject*> CharacterMade;
	UObject* CharacterModel = FMantleVehicleBuilder::ImportModelFile(Character, Error, Folder / TEXT("Character"), /*bSave*/ false, &CharacterMade);
	if (TestNotNull(*FString::Printf(TEXT("the character is imported (%s)"), *Error), CharacterModel))
	{
		TestTrue(TEXT("as a skeletal mesh, the file's skinned mesh being the model"), CharacterModel->IsA<USkeletalMesh>());
		FMantleVehicleModel Read;
		TestTrue(*FString::Printf(TEXT("which has bones the builder can read (%s)"), *Error), FMantleVehicleModel::Read(CharacterModel, Read, Error) && Read.bSkeletal && Read.Bones.Num() > 1);
	}

	// Leave the project as it was: what the imports made goes away again.
	TArray<UObject*> Cleanup = CubeMade;
	Cleanup.Append(CharacterMade);
	Cleanup.RemoveAll([](UObject* Object) { return Object == nullptr; });
	if (Cleanup.Num() > 0)
	{
		FMantleAssetCleanup::MoveAside(Cleanup);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleBuilderShapeEditForgetsBuildTest,
	"Mantle.Vehicle.Builder.ShapeEditForgetsTheBuild",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleBuilderShapeEditForgetsBuildTest::RunTest(const FString& Parameters)
{
	using Access = FMantleVehicleBuilderFixAccess;
	if (!FSlateApplication::IsInitialized())
	{
		AddWarning(TEXT("SKIPPED, not passed: there is no Slate application in this runner, so the window cannot be made."));
		return true;
	}
	UMantleVehicleSource* Built = NewObject<UMantleVehicleSource>(GetTransientPackage(), NAME_None, RF_Transient);
	TSharedRef<SMantleVehicleBuilder> Panel = SNew(SMantleVehicleBuilder);

	// A road vehicle: a size that changes forgets the build; the same size again does not (positive and negative control).
	Access::PickType(*Panel, TEXT("sedan"), TEXT("Sedan"));
	Access::Give(*Panel, Built);
	TestTrue(TEXT("a build is held"), Access::HasBuild(*Panel));
	const double Wheelbase = Access::Dimension(*Panel, EMantleVehicleDimension::Wheelbase);
	Access::SetDimension(*Panel, EMantleVehicleDimension::Wheelbase, Wheelbase);
	TestTrue(TEXT("the same wheelbase again changes nothing, so the build is kept"), Access::HasBuild(*Panel));
	Access::SetDimension(*Panel, EMantleVehicleDimension::Wheelbase, Wheelbase + 20.0);
	TestTrue(TEXT("the wheelbase really moved"), !FMath::IsNearlyEqual(Access::Dimension(*Panel, EMantleVehicleDimension::Wheelbase), Wheelbase));
	TestFalse(TEXT("a changed wheelbase forgets the build, its size and its save line"), Access::HasBuild(*Panel));

	// A feel slider is a shape change too.
	Access::Give(*Panel, Built);
	Access::SetFeel(*Panel, EMantleVehicleFeel::Grip, 0.5);
	TestFalse(TEXT("a feel forgets the build"), Access::HasBuild(*Panel));
	Access::Give(*Panel, Built);
	Access::SetPassenger(*Panel, false);
	TestFalse(TEXT("so does turning the passenger seat off"), Access::HasBuild(*Panel));

	// A flier's Size.
	Access::PickType(*Panel, TEXT("plane"), TEXT("Plane"));
	Access::Give(*Panel, Built);
	Access::SetShapeSize(*Panel, 1.0);
	TestTrue(TEXT("the plane's Size 1 again keeps the build"), Access::HasBuild(*Panel));
	Access::SetShapeSize(*Panel, 0.9);
	TestFalse(TEXT("a new Size forgets it"), Access::HasBuild(*Panel));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleBuilderFlierAdviceTest,
	"Mantle.Vehicle.Builder.FliersAreToldWhatTheyNeed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleBuilderFlierAdviceTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleBuilderFixTestPrivate;

	// The Drive step's line: a plane, a jet and a helicopter each get one, with their own number; everything else gets none.
	const auto Advice = [this](const TCHAR* Id) -> FString
	{
		FMantleChassisSpec Spec;
		FString Error;
		TestTrue(*FString::Printf(TEXT("%s loads"), Id), FMantleVehicleChassisBuilder::LoadSpec(Id, Spec, Error));
		return FMantleVehicleShape::DriveAdvice(Spec).ToString();
	};
	TestTrue(TEXT("the plane needs about 350 m or a runway"), Advice(TEXT("plane")).Contains(TEXT("350 m")) && Advice(TEXT("plane")).Contains(TEXT("runway")));
	TestTrue(TEXT("the jet needs about 150 m or a runway"), Advice(TEXT("jet")).Contains(TEXT("150 m")) && Advice(TEXT("jet")).Contains(TEXT("runway")));
	TestTrue(TEXT("the helicopter needs open space above"), Advice(TEXT("helicopter")).Contains(TEXT("open space above")));
	for (const TCHAR* Id : { TEXT("sedan"), TEXT("motorcycle"), TEXT("hovercraft") })
	{
		TestTrue(*FString::Printf(TEXT("%s has no advice to give"), Id), Advice(Id).IsEmpty());
	}

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World || !World->PersistentLevel)
	{
		AddWarning(TEXT("SKIPPED, not passed: there is no editor world in this runner, so where a flier is placed is not checked."));
		return true;
	}

	// Where the camera is and which way it looks, found as PlaceInLevel finds it (a headless editor may or may not have a viewport; with none it is the origin looking along +X).
	FVector Camera = FVector::ZeroVector;
	FRotator CameraLook = FRotator::ZeroRotator;
	{
		FLevelEditorViewportClient* View = GCurrentLevelEditingViewportClient;
		if (!View || !View->IsPerspective())
		{
			View = nullptr;
			for (FLevelEditorViewportClient* Candidate : GEditor->GetLevelViewportClients())
			{
				if (Candidate && Candidate->IsPerspective())
				{
					View = Candidate;
					break;
				}
			}
		}
		if (View)
		{
			Camera = View->GetViewLocation();
			CameraLook = View->GetViewRotation();
		}
	}
	const FRotator Heading(0.0, CameraLook.Yaw, 0.0);
	const FVector Forward = Heading.Vector();
	const double FloorTop = Camera.Z - 300.0;

	// A floor, 1600 m long and 100 m wide, along the way the camera looks and 3 m below it.
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!Cube)
	{
		AddWarning(TEXT("SKIPPED, not passed: the engine's cube mesh could not be loaded, so there is no ground to place a flier on."));
		return true;
	}
	const auto MakeSolid = [World, Cube, &Heading](const FVector& Centre, const FVector& Scale)
	{
		AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(Centre, Heading);
		Actor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
		Actor->GetStaticMeshComponent()->SetStaticMesh(Cube);
		Actor->SetActorScale3D(Scale);
		Actor->GetStaticMeshComponent()->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Actor->GetStaticMeshComponent()->SetCollisionResponseToAllChannels(ECR_Block);
		Actor->GetStaticMeshComponent()->RecreatePhysicsState();
		return Actor;
	};
	AStaticMeshActor* Floor = MakeSolid(FVector(Camera.X, Camera.Y, FloorTop - 100.0) + Forward * 60000.0, FVector(1600.0, 100.0, 2.0));
	AStaticMeshActor* Bump = nullptr;
	TArray<AMantleVehicleSpawnPoint*> Spawns;
	const auto Place = [this, &Spawns](UMantleVehicleSource& Source, FString& OutNote) -> AMantleVehicleSpawnPoint*
	{
		FString Problem;
		AMantleVehicleSpawnPoint* Spawn = nullptr;
		if (!FMantleVehicleBuilder::PlaceInLevel(Source, Problem, &Spawn, &OutNote) || !Spawn)
		{
			AddError(FString::Printf(TEXT("A spawn could not be placed: %s"), *Problem));
			return nullptr;
		}
		Spawns.Add(Spawn);
		return Spawn;
	};
	const auto MakeSource = [](const EMantleVehicleMovementKind Kind, const TCHAR* Preset)
	{
		UMantleVehicleSource* Source = NewObject<UMantleVehicleSource>(GetTransientPackage(), NAME_None, RF_Transient);
		Source->MovementType = Kind;
		Source->Preset = FName(Preset);
		return Source;
	};

	FString Note;
	UMantleVehicleSource* Jet = MakeSource(EMantleVehicleMovementKind::Plane, TEXT("jet"));
	UMantleVehicleSource* Plane = MakeSource(EMantleVehicleMovementKind::Plane, TEXT("plane"));
	UMantleVehicleSource* Heli = MakeSource(EMantleVehicleMovementKind::Helicopter, TEXT("helicopter"));
	UMantleVehicleSource* Car = MakeSource(EMantleVehicleMovementKind::Wheeled, TEXT("sedan"));

	// Flat ground ahead: it goes 6 m in front of the camera, and says how much room a jet wants.
	if (AMantleVehicleSpawnPoint* Spawn = Place(*Jet, Note))
	{
		const FVector Wanted = Camera + Forward * 600.0;
		TestTrue(*FString::Printf(TEXT("on a flat floor a jet stays 6 m ahead (at %s, wanted %s)"), *Spawn->GetActorLocation().ToString(), *Wanted.ToString()),
			FVector::Dist2D(Spawn->GetActorLocation(), Wanted) < 1.0);
		TestTrue(*FString::Printf(TEXT("and sits on the floor (Z %.0f, floor %.0f)"), Spawn->GetActorLocation().Z, FloorTop), FMath::IsNearlyEqual(Spawn->GetActorLocation().Z, FloorTop + 50.0, 1.0));
		TestTrue(*FString::Printf(TEXT("it says what a jet needs and where it is (%s)"), *Note), Note.Contains(TEXT("150 m")) && Note.Contains(TEXT("6 m in front of you")) && Note.Contains(TEXT("long and flat")));
	}

	// A bump where it would go: the nearest stretch that is flat for its whole run is further on, so that is where it is put, and the creator is told.
	Bump = MakeSolid(FVector(Camera.X, Camera.Y, FloorTop) + Forward * 600.0, FVector(12.0, 12.0, 6.0));
	if (AMantleVehicleSpawnPoint* Spawn = Place(*Jet, Note))
	{
		TestTrue(*FString::Printf(TEXT("with a bump 6 m ahead the jet moves on to flat ground (%.0f cm from the camera)"), FVector::Dist2D(Spawn->GetActorLocation(), Camera)),
			FVector::Dist2D(Spawn->GetActorLocation(), Camera) > 1500.0);
		TestTrue(*FString::Printf(TEXT("and says how far it went (%s)"), *Note), Note.Contains(TEXT("m in front of you")) && !Note.Contains(TEXT(" 6 m in front of you")));
	}
	// The plane needs a longer run than the jet and says so.
	if (Place(*Plane, Note))
	{
		TestTrue(*FString::Printf(TEXT("the plane needs 350 m (%s)"), *Note), Note.Contains(TEXT("350 m")));
	}
	// A helicopter and a car stay 6 m ahead; the helicopter is told about open space, the car nothing.
	if (Place(*Heli, Note))
	{
		TestTrue(TEXT("the helicopter is told it needs open space above it"), Note.Contains(TEXT("open space above")));
	}
	if (Place(*Car, Note))
	{
		TestTrue(TEXT("a car is told nothing"), Note.IsEmpty());
	}

	// Leave the level as it was.
	for (AMantleVehicleSpawnPoint* Spawn : Spawns)
	{
		if (GEditor)
		{
			GEditor->SelectActor(Spawn, /*bInSelected*/ false, /*bNotify*/ false);
		}
		World->EditorDestroyActor(Spawn, /*bShouldModifyLevel*/ true);
	}
	World->EditorDestroyActor(Floor, /*bShouldModifyLevel*/ true);
	if (Bump)
	{
		World->EditorDestroyActor(Bump, /*bShouldModifyLevel*/ true);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleBuilderScaleNoteTest,
	"Mantle.Vehicle.ChassisSkin.ScaleClampIsExplained",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleBuilderScaleNoteTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleBuilderFixTestPrivate;
	FMantleChassisSpec Sedan;
	FMantleVehicleAsset SedanGrey;
	if (!Load(*this, TEXT("sedan"), Sedan, SedanGrey))
	{
		return false;
	}

	// A car as long as the sedan: nothing to say (control).
	const FMantleVehicleModelMap NoParts;
	const FMantleVehicleModelPlacement Fine = FMantleVehicleChassisSkin::FitPlacement(Sedan, SedanGrey.Body, BoxModel(FVector3f(static_cast<float>(Sedan.Length), 180.f, 140.f)), 0.0, NoParts);
	TestTrue(TEXT("a model as long as its vehicle is placed with no note"), Fine.Note.IsEmpty() && FMath::IsNearlyEqual(Fine.Scale, 1.0, 0.01));

	// A car modelled in metres: 4.5 units long where the sedan is 450 cm, a hundred times too small. The scale stops at 20, and the note says what is wrong.
	const FMantleVehicleModelPlacement Metres = FMantleVehicleChassisSkin::FitPlacement(Sedan, SedanGrey.Body, BoxModel(FVector3f(4.5f, 1.8f, 1.4f)), 0.0, NoParts);
	TestEqual(TEXT("the scale is held to 20"), Metres.Scale, 20.0);
	TestTrue(*FString::Printf(TEXT("and the note says it is far too small and probably in metres (%s)"), *Metres.Note),
		Metres.Note.Contains(TEXT("times smaller")) && Metres.Note.Contains(TEXT("metres")) && Metres.Note.Contains(TEXT("20.00 times")));

	// One modelled in millimetres is held at the least.
	const FMantleVehicleModelPlacement Millimetres = FMantleVehicleChassisSkin::FitPlacement(Sedan, SedanGrey.Body, BoxModel(FVector3f(450000.f, 180000.f, 140000.f)), 0.0, NoParts);
	TestEqual(TEXT("a huge model is held at 0.05"), Millimetres.Scale, 0.05);
	TestTrue(*FString::Printf(TEXT("and the note says it is far too big (%s)"), *Millimetres.Note), Millimetres.Note.Contains(TEXT("times bigger")) && Millimetres.Note.Contains(TEXT("millimetres")));

	// Under a centimetre: it cannot be measured, so it is left as modelled, and that is said too.
	const FMantleVehicleModelPlacement Speck = FMantleVehicleChassisSkin::FitPlacement(Sedan, SedanGrey.Body, BoxModel(FVector3f(0.4f, 0.2f, 0.1f)), 0.0, NoParts);
	TestTrue(*FString::Printf(TEXT("a model under a centimetre is left alone and says so (%s)"), *Speck.Note), Speck.Note.Contains(TEXT("under 1 cm")) && Speck.Scale == 1.0);

	// The proposal for a plane quotes the percentage the model really ends up at, not the one it asked for.
	FMantleChassisSpec Plane;
	FMantleVehicleAsset PlaneGrey;
	const FMantleVehiclePreset* PlanePreset = FMantleVehiclePresets::Find(TEXT("plane"));
	if (!Load(*this, TEXT("plane"), Plane, PlaneGrey) || !TestNotNull(TEXT("the plane preset"), PlanePreset))
	{
		return false;
	}
	const FBox3f PlaneBox = PlaneGrey.Body.ComputeBounds(0);
	const FMantleVehicleModelProposal Same = FMantleVehicleChassisSkin::Propose(Plane, *PlanePreset, BoxModel(FVector3f(PlaneBox.Max.X - PlaneBox.Min.X, PlaneBox.Max.Y - PlaneBox.Min.Y, 100.f)), 0.0, NoParts);
	TestTrue(*FString::Printf(TEXT("a model the size of the plane says so and holds nothing (%s)"), *FString::Join(Same.Notes, TEXT(" "))), Says(Same.Notes, TEXT("size of a standard")) && Same.Notes.Num() == 1);
	const FMantleVehicleModelProposal Tiny = FMantleVehicleChassisSkin::Propose(Plane, *PlanePreset, BoxModel(FVector3f(2.f, 2.f, 1.f)), 0.0, NoParts);
	TestTrue(*FString::Printf(TEXT("a model a few units long is enlarged to 2000%% and not to the thousands it asked for (%s)"), *FString::Join(Tiny.Notes, TEXT(" "))),
		Says(Tiny.Notes, TEXT("2000%")) && Says(Tiny.Notes, TEXT("times smaller")));
	const FMantleVehicleModelProposal Speck2 = FMantleVehicleChassisSkin::Propose(Plane, *PlanePreset, BoxModel(FVector3f(0.4f, 0.4f, 0.2f)), 0.0, NoParts);
	TestTrue(*FString::Printf(TEXT("and one under a centimetre is not sized, and says why (%s)"), *FString::Join(Speck2.Notes, TEXT(" "))), Says(Speck2.Notes, TEXT("under 1 cm")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleBuilderPropellerCountTest,
	"Mantle.Vehicle.ChassisSkin.PropellerCountAgreesWithTheMap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleBuilderPropellerCountTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleBuilderFixTestPrivate;
	FMantleChassisSpec Plane;
	FMantleVehicleAsset PlaneGrey;
	if (!Load(*this, TEXT("plane"), Plane, PlaneGrey))
	{
		return false;
	}
	const FMantleChassisSpec Twin = FMantleVehicleChassisSkin::AsTwin(Plane);
	FMantleVehicleAsset TwinGrey;
	FString Error;
	if (!TestTrue(TEXT("the twin chassis is made"), FMantleVehicleChassisBuilder::MakeAsset(Twin, TwinGrey, Error)))
	{
		return false;
	}
	const TArray<FMantleVehiclePartSlot> TwinSlots = FMantleVehicleChassisSkin::PartSlots(Twin, TwinGrey.Body);
	const TArray<FMantleVehiclePartSlot> SingleSlots = FMantleVehicleChassisSkin::PartSlots(Plane, PlaneGrey.Body);

	const auto Skeletal = [](const TArray<TPair<FName, int32>>& Bones, const TArray<FVector>& Positions)
	{
		FMantleVehicleModel Model = BoxModel(FVector3f(800.f, 1000.f, 200.f));
		Model.bSkeletal = true;
		for (const TPair<FName, int32>& Bone : Bones)
		{
			Model.Bones.Add({ Bone.Key, Bone.Value });
		}
		Model.BonePositions = Positions;
		return Model;
	};

	// The P-38's shape: two propellers, each rigged as a hub with a blade bone under it, and a propeller guard that is no propeller. A bone each is four to a naive count.
	const FMantleVehicleModel P38 = Skeletal({ { FName("root"), INDEX_NONE }, { FName("prop_L"), 0 }, { FName("prop_L_blade"), 1 }, { FName("prop_R"), 0 }, { FName("prop_R_blade"), 3 },
		{ FName("prop_guard"), 0 } },
		{ FVector::ZeroVector, FVector(200, -300, 0), FVector(210, -300, 0), FVector(200, 300, 0), FVector(210, 300, 0), FVector(150, 0, 0) });
	TestEqual(TEXT("two propellers rigged as chains are two propellers (the guard is none)"), FMantleVehicleChassisSkin::CountPropellers(P38), 2);
	const FMantleVehicleModelMap TwinMap = FMantleVehicleChassisSkin::SuggestMap(TwinSlots, P38, 0.0);
	const FMantleVehicleModelPiece* Left = TwinMap.Find(FName("propeller_l"));
	const FMantleVehicleModelPiece* Right = TwinMap.Find(FName("propeller_r"));
	TestTrue(TEXT("and the map gives the twin's two propeller slots the hubs, left and right"),
		Left && Right && Left->Bone == FName("prop_L") && Right->Bone == FName("prop_R"));

	// One propeller rigged as a hub and a blade: the old count said two engines for it; the map has one propeller to assign, and now the count says one.
	const FMantleVehicleModel OneChain = Skeletal({ { FName("root"), INDEX_NONE }, { FName("propeller"), 0 }, { FName("propeller_blade"), 1 } }, { FVector::ZeroVector, FVector(200, 0, 0), FVector(210, 0, 0) });
	TestEqual(TEXT("one propeller with a blade bone under it is one"), FMantleVehicleChassisSkin::CountPropellers(OneChain), 1);
	const FMantleVehicleModelMap OneMap = FMantleVehicleChassisSkin::SuggestMap(SingleSlots, OneChain, 0.0);
	const FMantleVehicleModelPiece* Only = OneMap.Find(FName("propeller"));
	TestTrue(TEXT("and the map gives the plane's propeller the hub"), Only && Only->Bone == FName("propeller"));

	// Two separate bones are two (control: the count still counts), and none is none.
	const FMantleVehicleModel Pair = Skeletal({ { FName("root"), INDEX_NONE }, { FName("PropLeft"), 0 }, { FName("PropRight"), 0 } }, { FVector::ZeroVector, FVector(200, -300, 0), FVector(200, 300, 0) });
	TestEqual(TEXT("two separate propeller bones are two"), FMantleVehicleChassisSkin::CountPropellers(Pair), 2);
	TestEqual(TEXT("a model with none has none"), FMantleVehicleChassisSkin::CountPropellers(Skeletal({ { FName("root"), INDEX_NONE }, { FName("wing"), 0 } }, { FVector::ZeroVector, FVector(0, 100, 0) })), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleBuilderOneAssetTest,
	"Mantle.Vehicle.Builder.OneAssetPerVehicleAndUndoable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleBuilderOneAssetTest::RunTest(const FString& Parameters)
{
	using Access = FMantleVehicleBuilderFixAccess;
	FString Error;
	USkeletalMesh* Body = MantleVehicleScratchBody::Make(TEXT("/Temp/MantleFixNames"), TEXT("FixNamesBody"), Error);
	USkeletalMesh* Other = MantleVehicleScratchBody::Make(TEXT("/Temp/MantleFixNames"), TEXT("FixNamesOther"), Error);
	if (!TestNotNull(*FString::Printf(TEXT("the scratch bodies are made (%s)"), *Error), Body) || !TestNotNull(TEXT("both"), Other))
	{
		return false;
	}

	UTransBuffer* Trans = GEditor ? Cast<UTransBuffer>(GEditor->Trans) : nullptr;
	const int32 QueueBefore = Trans ? Trans->GetQueueLength() : 0;

	FMantleVehicleModelOptions Options;
	Options.Folder = TEXT("/Temp/MantleFixNames");
	Options.Name = TEXT("FixVehicle");
	const FMantleVehicleBuildResult First = FMantleVehicleBuilder::BuildFromModel(Body, TEXT("jet"), Options);
	for (const FString& Problem : First.Problems)
	{
		AddError(Problem);
	}
	if (!TestTrue(TEXT("a vehicle is built under a name"), First.bOk && First.Source != nullptr))
	{
		MantleVehicleScratchBody::Discard(Body);
		MantleVehicleScratchBody::Discard(Other);
		return false;
	}
	TestEqual(TEXT("the result says the name its assets were saved under"), First.AssetName, FString(TEXT("FixVehicle")));
	if (Trans)
	{
		TestEqual(TEXT("building it is one undo step"), Trans->GetQueueLength(), QueueBefore + 1);
	}
	else
	{
		AddWarning(TEXT("SKIPPED, not passed: there is no undo buffer in this runner, so the undo step is not checked."));
	}

	// A name a vehicle already has is not overwritten by a build that did not make it...
	const FMantleVehicleBuildResult Refused = FMantleVehicleBuilder::BuildFromModel(Body, TEXT("jet"), Options);
	TestFalse(TEXT("the same name again, without saying so on purpose, is refused"), Refused.bOk);
	TestNull(TEXT("and makes no vehicle"), Refused.Source);
	TestTrue(*FString::Printf(TEXT("with a plain reason (%s)"), Refused.Problems.Num() > 0 ? *Refused.Problems[0] : TEXT("none")),
		Refused.Problems.Num() > 0 && Refused.Problems[0].Contains(TEXT("already")) && Refused.Problems[0].Contains(TEXT("overwrite=1")));

	// ...but one the window made is updated in place when it says so: the same asset, not a new set.
	Options.bOverwrite = true;
	const FMantleVehicleBuildResult Again = FMantleVehicleBuilder::BuildFromModel(Body, TEXT("jet"), Options);
	TestTrue(TEXT("with overwrite the build goes ahead"), Again.bOk);
	TestTrue(TEXT("and is the same vehicle asset as before"), Again.Source == First.Source);

	// With no name, a new vehicle takes a name of its own; the window passes the name it built under for the same type and model, and no name for another.
	if (FSlateApplication::IsInitialized())
	{
		TSharedRef<SMantleVehicleBuilder> Panel = SNew(SMantleVehicleBuilder);
		Access::PickType(*Panel, TEXT("jet"), TEXT("Jet"));
		Access::SetMesh(*Panel, Body);
		TestTrue(TEXT("before any build the window asks for no name"), Access::Options(*Panel).Name.IsEmpty());
		Access::Remember(*Panel, First);
		const FMantleVehicleModelOptions Same = Access::Options(*Panel);
		TestTrue(TEXT("after a build, the same type and model ask for that name, on purpose"), Same.Name == TEXT("FixVehicle") && Same.bOverwrite);
		Access::SetMesh(*Panel, Other);
		TestTrue(TEXT("another model asks for a name of its own"), Access::Options(*Panel).Name.IsEmpty() && !Access::Options(*Panel).bOverwrite);
		Access::SetMesh(*Panel, Body);
		TestEqual(TEXT("the first model again has its name back"), Access::Options(*Panel).Name, FString(TEXT("FixVehicle")));
		Access::PickType(*Panel, TEXT("plane"), TEXT("Plane"));
		TestTrue(TEXT("another type asks for a name of its own"), Access::Options(*Panel).Name.IsEmpty());
	}
	else
	{
		AddWarning(TEXT("SKIPPED, not passed: there is no Slate application in this runner, so the window's choice of name is not checked."));
	}

	MantleVehicleScratchBody::Discard(Body);
	MantleVehicleScratchBody::Discard(Other);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleBuilderMovementBlocksTest,
	"Mantle.Vehicle.Builder.MalformedMovementBlocksAreAnError",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleBuilderMovementBlocksTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleBuilderFixTestPrivate;
	FMantleChassisSpec Bike;
	FString Error;
	if (!TestTrue(TEXT("the motorcycle loads"), FMantleVehicleChassisBuilder::LoadSpec(TEXT("motorcycle"), Bike, Error)))
	{
		return false;
	}
	const FMantleVehicleBuildResult Built = FMantleVehicleBuilder::BuildChassis(Bike, TEXT("/Temp/MantleFixBike"), TEXT("FixBike"));
	if (!TestTrue(TEXT("a motorcycle chassis is built"), Built.bOk && Built.Source != nullptr))
	{
		return false;
	}
	UMantleVehicleSource& Source = *Built.Source;

	// Positive control: its movement blocks read, and the document carries the rider block.
	FString MergeError;
	TSharedRef<FJsonObject> Good = FMantleVehicleConverter::MakeDocument(Source, TMap<FString, FString>(), &MergeError);
	TestTrue(*FString::Printf(TEXT("a well-formed bike has no merge error (%s)"), *MergeError), MergeError.IsEmpty());
	const TSharedPtr<FJsonObject>* Seat = nullptr;
	TestTrue(TEXT("and its document has the rider block"), Good->TryGetObjectField(TEXT("seat"), Seat) && Seat && (*Seat)->HasField(TEXT("rider")));
	TArray<FMantleValidationIssue> Issues;
	FMantleVehicleConverter::CheckSource(&Source, FString(), Issues);
	TestFalse(TEXT("and the check finds nothing wrong with its movement"), Issues.ContainsByPredicate([](const FMantleValidationIssue& Issue) { return Issue.Problem.Contains(TEXT("movement settings")); }));

	// Malformed blocks: the document is made without them, the caller is told, and the check says it is an error.
	for (const TCHAR* Bad : { TEXT("{\"seat\": 3}"), TEXT("[1, 2"), TEXT("{\"wings\": {}}") })
	{
		Source.MovementBlocks = Bad;
		FString Why;
		const TSharedRef<FJsonObject> Broken = FMantleVehicleConverter::MakeDocument(Source, TMap<FString, FString>(), &Why);
		TestFalse(*FString::Printf(TEXT("%s is reported by the document"), Bad), Why.IsEmpty());
		const TSharedPtr<FJsonObject>* BrokenSeat = nullptr;
		TestFalse(*FString::Printf(TEXT("%s leaves the document without its rider block (the thing that must not happen quietly)"), Bad),
			Broken->TryGetObjectField(TEXT("seat"), BrokenSeat) && BrokenSeat && (*BrokenSeat)->HasField(TEXT("rider")));
		Issues.Reset();
		FMantleVehicleConverter::CheckSource(&Source, FString(), Issues);
		TestTrue(*FString::Printf(TEXT("and the check names it as an error (%s)"), Bad), Issues.ContainsByPredicate([](const FMantleValidationIssue& Issue)
		{
			return Issue.Severity == EMantleIssueSeverity::Error && Issue.Problem.Contains(TEXT("movement settings"));
		}));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleBuilderFeelTest,
	"Mantle.Vehicle.Builder.RoadTypesHaveTheFeelSliders",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleBuilderFeelTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleBuilderFixTestPrivate;
	for (const TCHAR* Id : { TEXT("sedan"), TEXT("sports"), TEXT("limousine"), TEXT("pickup"), TEXT("van"), TEXT("truck"), TEXT("sixwheeler"), TEXT("motorcycle") })
	{
		FMantleChassisSpec Archetype;
		FString Error;
		if (!TestTrue(*FString::Printf(TEXT("%s loads"), Id), FMantleVehicleChassisBuilder::LoadSpec(Id, Archetype, Error)))
		{
			continue;
		}
		FMantleVehicleShapeSession Session;
		Session.Start(Archetype);
		for (const EMantleVehicleFeel Feel : MantleVehicleFeels)
		{
			TestTrue(*FString::Printf(TEXT("%s has feel %d"), Id, static_cast<int32>(Feel)), Session.HasFeel(Feel));
		}
		TestFalse(*FString::Printf(TEXT("%s starts unshaped"), Id), Session.IsShaped());
		const FMantleChassisHandling Own = Archetype.Archetype;

		// Each slider moves its own number by the reach of the derive (30 percent at the end), positive control; the others stay where they were.
		// Half the slider, not the end: a sports car's tyres at 30% more would pass the document's friction limit, and Mantle holds them there (HeldNumbers says so).
		TestTrue(*FString::Printf(TEXT("%s: grip moves"), Id), Session.SetFeel(EMantleVehicleFeel::Grip, 0.5));
		TestTrue(*FString::Printf(TEXT("%s: it is shaped now"), Id), Session.IsShaped() && Session.GetShaped().Derived.IsSet());
		TestTrue(*FString::Printf(TEXT("%s: grip at half is 15%% more tyre friction (%.3f from %.3f)"), Id, Session.GetShaped().Derived->TireFriction, Own.TireFriction),
			Near(Session.GetShaped().Derived->TireFriction, Own.TireFriction * 1.15, 0.01));
		TestTrue(*FString::Printf(TEXT("%s: and the engine is the preset's"), Id), Near(Session.GetShaped().Derived->MaxTorque, Own.MaxTorque, 0.01));
		Session.Reset();
		TestTrue(*FString::Printf(TEXT("%s: Reset puts the feel back"), Id), !Session.IsShaped() && Session.GetFeel(EMantleVehicleFeel::Grip) == 0.0 && !Session.GetShaped().Derived.IsSet());

		Session.SetFeel(EMantleVehicleFeel::Power, 1.0);
		TestTrue(*FString::Printf(TEXT("%s: power is 30%% more torque"), Id), Near(Session.GetShaped().Derived->MaxTorque, Own.MaxTorque * 1.3, 0.01));
		Session.Reset();
		Session.SetFeel(EMantleVehicleFeel::TopSpeed, -1.0);
		TestTrue(*FString::Printf(TEXT("%s: top speed -1 is 30%% slower"), Id), Near(Session.GetShaped().Derived->MaxSpeed, Own.MaxSpeed * 0.7, 0.01));
		Session.Reset();
		Session.SetFeel(EMantleVehicleFeel::Stiffness, 1.0);
		TestTrue(*FString::Printf(TEXT("%s: stiffness firms the springs (%.0f from %.0f)"), Id, Session.GetShaped().Derived->SpringRate, Own.SpringRate),
			Session.GetShaped().Derived->SpringRate > Own.SpringRate * 1.3);

		// A value out of range is held, and not a number changes nothing.
		Session.Reset();
		Session.SetFeel(EMantleVehicleFeel::Grip, 7.0);
		TestEqual(*FString::Printf(TEXT("%s: a feel past the end is held to 1"), Id), Session.GetFeel(EMantleVehicleFeel::Grip), 1.0);
		TestFalse(*FString::Printf(TEXT("%s: a feel that is not a number changes nothing"), Id), Session.SetFeel(EMantleVehicleFeel::Power, std::numeric_limits<double>::quiet_NaN()));

		// The feel and the sizes together: a longer wheelbase and a firmer ride are both in the vehicle, and they are different vehicles from either alone.
		if (!Archetype.bCentreline)
		{
			Session.Reset();
			Session.SetFeel(EMantleVehicleFeel::Grip, 0.5);
			const FString FeelOnly = Session.Signature();
			Session.SetDimension(EMantleVehicleDimension::Wheelbase, Archetype.Wheelbase * 1.1);
			TestTrue(*FString::Printf(TEXT("%s: feel and sizes both count in the signature"), Id), Session.Signature() != FeelOnly);
			TestTrue(*FString::Printf(TEXT("%s: and both are in the chassis (%.3f)"), Id, Session.GetShaped().Derived->TireFriction),
				Near(Session.GetShaped().Derived->TireFriction, Own.TireFriction * 1.15, 0.01) && Session.GetShaped().Wheelbase > Archetype.Wheelbase * 1.05);
		}

		// The passenger tickbox where the preset has a passenger seat.
		TestEqual(*FString::Printf(TEXT("%s: the passenger option is there when the preset has the seat"), Id), Session.HasPassengerOption(), Archetype.bPassenger);
		if (Archetype.bPassenger)
		{
			Session.Reset();
			TestTrue(*FString::Printf(TEXT("%s: the passenger seat comes off"), Id), Session.SetPassenger(false) && !Session.GetShaped().bPassenger && Session.IsShaped());
			TestFalse(*FString::Printf(TEXT("%s: and the chassis the builder gets has no passenger"), Id), FMantleVehicleChassisBuilder::ReferenceOf(Session.GetShaped()).bPassenger);
			TestTrue(*FString::Printf(TEXT("%s: Reset puts it back"), Id), Session.Reset() && Session.GetPassenger() && Session.GetShaped().bPassenger);
		}
	}

	// The motorcycle's rear tyre keeps its share of the front's when the grip moves (the rider block's own friction).
	FMantleChassisSpec Bike;
	FString Error;
	if (FMantleVehicleChassisBuilder::LoadSpec(TEXT("motorcycle"), Bike, Error) && TestTrue(TEXT("the motorcycle has a rider"), Bike.Rider.bSet))
	{
		FMantleVehicleShapeSession Session;
		Session.Start(Bike);
		Session.SetFeel(EMantleVehicleFeel::Grip, 0.5);
		TestTrue(TEXT("the bike's rear tyre grips 15% more too"), Near(Session.GetShaped().Rider.RearTireFriction, Bike.Rider.RearTireFriction * 1.15, 0.01));
	}

	// What does not have them: a plane's handling is tuned to fly, and a hovercraft has the top speed alone.
	FMantleChassisSpec Plane;
	FMantleChassisSpec Hover;
	if (FMantleVehicleChassisBuilder::LoadSpec(TEXT("plane"), Plane, Error) && FMantleVehicleChassisBuilder::LoadSpec(TEXT("hovercraft"), Hover, Error))
	{
		FMantleVehicleShapeSession Flying;
		Flying.Start(Plane);
		FMantleVehicleShapeSession Floating;
		Floating.Start(Hover);
		for (const EMantleVehicleFeel Feel : MantleVehicleFeels)
		{
			TestFalse(TEXT("a plane has no feel sliders"), Flying.HasFeel(Feel) || Flying.SetFeel(Feel, 0.5));
			TestEqual(TEXT("a hovercraft has the top speed alone"), Floating.HasFeel(Feel), Feel == EMantleVehicleFeel::TopSpeed);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleBuilderCopyTest,
	"Mantle.Vehicle.Builder.CardsAndHelpAreDistinctAndTrue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleBuilderCopyTest::RunTest(const FString& Parameters)
{
	// The cards: no two say the same, and the pickup, the van and the truck no longer all name vans.
	TSet<FString> Seen;
	TMap<FString, FString> Sentences;
	for (const FMantleVehiclePreset* Card : FMantleVehiclePresets::Publishable())
	{
		const FString Sentence = FMantleVehiclePresets::CardSentence(*Card);
		TestFalse(*FString::Printf(TEXT("%s's card is not another card's"), *Card->Id), Seen.Contains(Sentence));
		Seen.Add(Sentence);
		Sentences.Add(Card->Id, Sentence);
	}
	const auto NamesVans = [](const FString& Sentence)
	{
		TArray<FString> Words;
		const TCHAR* const Separators[] = { TEXT(" "), TEXT(","), TEXT("."), TEXT(":"), TEXT(";") };
		Sentence.ToLower().ParseIntoArray(Words, Separators, UE_ARRAY_COUNT(Separators));
		return Words.Contains(TEXT("van")) || Words.Contains(TEXT("vans"));
	};
	for (const TPair<FString, FString>& Card : Sentences)
	{
		TestEqual(*FString::Printf(TEXT("%s's card names vans only if it is the van's (%s)"), *Card.Key, *Card.Value), NamesVans(Card.Value), Card.Key == TEXT("van"));
	}

	// The Shape step's warnings carry the numbers the document holds to its limits (HeldNumbers), beside the handling's own. A plane sized far past its range has some.
	bool bProved = false;
	for (const TCHAR* Id : { TEXT("plane"), TEXT("jet"), TEXT("helicopter"), TEXT("hovercraft") })
	{
		FMantleChassisSpec Archetype;
		FString Error;
		if (!FMantleVehicleChassisBuilder::LoadSpec(Id, Archetype, Error))
		{
			continue;
		}
		TestTrue(*FString::Printf(TEXT("%s at its own size holds no number"), Id), FMantleVehicleChassisDocument::HeldNumbers(Archetype).IsEmpty());
		for (const double Size : { 2.0, 2.5, 0.3 })
		{
			const FMantleChassisSpec Sized = FMantleChassisSpec::Scaled(Archetype, Size);
			for (const FString& Held : FMantleVehicleChassisDocument::HeldNumbers(Sized))
			{
				bProved = true;
				TestTrue(*FString::Printf(TEXT("%s at %.1f: the warnings say '%s'"), Id, Size, *Held), FMantleVehicleShape::Warnings(Sized).ContainsByPredicate([&Held](const FText& Line) { return Line.ToString() == Held; }));
			}
		}
	}
	TestTrue(TEXT("some flier sized past its range holds a number (positive control for the check above)"), bProved);

	// The Size help for a plane says it is re-balanced for its size, not that it just stays balanced.
	const FString PlaneHelp = FMantleVehicleShape::SizeHelp(EMantleVehicleFamily::Aircraft).ToString();
	TestTrue(TEXT("the plane's Size help says it is re-balanced for its size"), PlaneHelp.Contains(TEXT("re-balanced for its size")) && !PlaneHelp.Contains(TEXT("stays balanced")));
	return true;
}

#endif
