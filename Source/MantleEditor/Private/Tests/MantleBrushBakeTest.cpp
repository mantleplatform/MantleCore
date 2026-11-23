// Copyright Joshua Eger. All Rights Reserved.
#include "MantleBrushBake.h"
#include "MantleCoverage.h"
#include "MantleExportedEntities.h"
#include "MantlePlaceAssets.h"
#include "MantlePlaceExporter.h"
#include "MantlePlaceValidator.h"

#include "Builders/CubeBuilder.h"
#include "Components/BrushComponent.h"
#include "Editor.h"
#include "Engine/BlockingVolume.h"
#include "Engine/Brush.h"
#include "Engine/Polys.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Misc/AutomationTest.h"
#include "Model.h"
#include "StaticMeshResources.h"

#if WITH_DEV_AUTOMATION_TESTS

// BSP brushes become baked static meshes at export (F23): the level's static Add and Subtract brushes are cut against each other as the
// editor's Build does, the result is merged by material and cached while the brushes are unchanged, and nothing of the creator's level
// (its brushes, its model) is changed by it. Built in a transient editor world.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleBrushBakeTestPrivate
{
	/** An empty editor world for one test, destroyed with it. */
	struct FBspWorld
	{
		UWorld* World = nullptr;

		FBspWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld*/ false,
				FName(*FString::Printf(TEXT("MantleBrushBakeTestWorld_%s"), *FGuid::NewGuid().ToString())));
		}

		~FBspWorld()
		{
			FMantleBrushBake::ClearCache();
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

	/** A static Add or Subtract brush, a cube of Size at Location. Name, when given, is the actor's object name (the cut must not depend on it). */
	ABrush* SpawnCubeBrush(UWorld* World, const TCHAR* Label, const FVector& Location, float Size, EBrushType Type, const TCHAR* Name = nullptr, uint32 PolyFlags = 0)
	{
		FActorSpawnParameters Parameters;
		Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		if (Name)
		{
			Parameters.Name = FName(Name);
		}
		ABrush* Brush = World->SpawnActor<ABrush>(Parameters);
		if (!Brush)
		{
			return nullptr;
		}
		Brush->PolyFlags = PolyFlags;
		Brush->SetActorLabel(Label);
		if (!Brush->Brush)
		{
			Brush->Brush = NewObject<UModel>(Brush, NAME_None, RF_Transactional);
			Brush->Brush->Initialize(Brush, /*InRootOutside*/ true);
			Brush->GetBrushComponent()->Brush = Brush->Brush;
		}
		Brush->BrushType = Type;
		Brush->SetActorLocation(Location);
		UCubeBuilder* Builder = NewObject<UCubeBuilder>();
		Builder->X = Size;
		Builder->Y = Size;
		Builder->Z = Size;
		Builder->Build(World, Brush);
		return Brush;
	}

	/** What a publish must leave alone: every brush's place and polygons, and the level's own model. */
	struct FLevelSnapshot
	{
		TArray<FTransform> Transforms;
		TArray<TArray<FVector3f>> Vertices;
		TArray<int32> BrushTypes;
		TArray<bool> Dirty;
		/** What the engine records on each brush's own model when it cuts it: the transform it was built at, and its vertex count. */
		TArray<FString> BuiltState;
		int32 Nodes = 0;
		int32 Surfs = 0;
		int32 Polys = 0;

		static FLevelSnapshot Take(UWorld* World)
		{
			FLevelSnapshot Out;
			for (TActorIterator<ABrush> It(World); It; ++It)
			{
				const ABrush* Brush = *It;
				Out.Transforms.Add(Brush->GetActorTransform());
				Out.BrushTypes.Add(Brush->BrushType);
				Out.Dirty.Add(Brush->GetPackage()->IsDirty());
				if (const UModel* Model = Brush->Brush)
				{
					Out.BuiltState.Add(FString::Printf(TEXT("%s|%s|%s|%d|%u"), *FVector(Model->OwnerLocationWhenLastBuilt).ToString(),
						*Model->OwnerRotationWhenLastBuilt.ToString(), *FVector(Model->OwnerScaleWhenLastBuilt).ToString(),
						Model->bCachedOwnerTransformValid ? 1 : 0, Model->NumUniqueVertices));
				}
				TArray<FVector3f>& Vertices = Out.Vertices.AddDefaulted_GetRef();
				if (Brush->Brush && Brush->Brush->Polys)
				{
					for (const FPoly& Poly : Brush->Brush->Polys->Element)
					{
						Vertices.Append(Poly.Vertices);
					}
				}
			}
			if (const UModel* Model = World->GetModel())
			{
				Out.Nodes = Model->Nodes.Num();
				Out.Surfs = Model->Surfs.Num();
				Out.Polys = Model->Polys ? Model->Polys->Element.Num() : 0;
			}
			return Out;
		}

		bool Matches(const FLevelSnapshot& Other) const
		{
			return Transforms.Num() == Other.Transforms.Num() && Vertices == Other.Vertices && BrushTypes == Other.BrushTypes && Dirty == Other.Dirty && BuiltState == Other.BuiltState
				&& Nodes == Other.Nodes && Surfs == Other.Surfs && Polys == Other.Polys
				&& [this, &Other]()
				{
					for (int32 Index = 0; Index < Transforms.Num(); ++Index)
					{
						if (!Transforms[Index].Equals(Other.Transforms[Index], 0.0))
						{
							return false;
						}
					}
					return true;
				}();
		}
	};

	bool HasIssue(const FMantleValidationResult& Result, const TCHAR* Actor, EMantleIssueSeverity Severity)
	{
		return Result.Issues.ContainsByPredicate([Actor, Severity](const FMantleValidationIssue& Issue)
		{
			return Issue.Actor == Actor && Issue.Severity == Severity;
		});
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleBrushBakeTest,
	"Mantle.Editor.BrushBake.BspBakesIntoMeshesAndLeavesTheLevelAlone",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleBrushBakeTest::RunTest(const FString& Parameters)
{
	using namespace MantleBrushBakeTestPrivate;
	if (!GEditor)
	{
		AddWarning(TEXT("no editor; skipped"));
		return true;
	}
	FBspWorld Fixture;
	UWorld* World = Fixture.World;
	if (!TestNotNull(TEXT("a test world"), World))
	{
		return false;
	}

	// A hollow room: a big cube with a smaller one cut out of it, and a Blocking Volume (a brush that is not BSP) beside it.
	ABrush* Room = SpawnCubeBrush(World, TEXT("Room"), FVector(0.0, 0.0, 0.0), 800.0f, Brush_Add);
	ABrush* Hollow = SpawnCubeBrush(World, TEXT("Room hollow"), FVector(0.0, 0.0, 0.0), 600.0f, Brush_Subtract);
	if (!TestNotNull(TEXT("the room"), Room) || !TestNotNull(TEXT("the hollow"), Hollow))
	{
		return false;
	}
	ABlockingVolume* Volume = World->SpawnActor<ABlockingVolume>(FVector(3000.0, 0.0, 0.0), FRotator::ZeroRotator);

	TestTrue(TEXT("an Add brush is BSP"), FMantleBrushBake::IsBakeSource(Room));
	TestTrue(TEXT("so is a Subtract brush"), FMantleBrushBake::IsBakeSource(Hollow));
	TestFalse(TEXT("the world's builder brush is not"), FMantleBrushBake::IsBakeSource(World->GetDefaultBrush()));
	TestFalse(TEXT("a Blocking Volume is not"), Volume && FMantleBrushBake::IsBakeSource(Volume));
	TestEqual(TEXT("two sources"), FMantleBrushBake::CollectSources(World).Num(), 2);

	const FLevelSnapshot Before = FLevelSnapshot::Take(World);
	const FMantlePlaceAssetLimits Limits = FMantlePlaceAssetLimits::FromContract();
	// Nobody is told a brush or its model changed: listeners (the scatter ground's among them) rebuild when they hear it.
	int32 ModifiedBrushes = 0;
	const FDelegateHandle Listener = FCoreUObjectDelegates::OnObjectModified.AddLambda([&ModifiedBrushes](UObject* Object)
	{
		ModifiedBrushes += Object && (Object->IsA<ABrush>() || Object->IsA<UModel>()) ? 1 : 0;
	});
	const FMantleSplineBakeResult First = FMantleBrushBake::Bake(World, Limits);
	FCoreUObjectDelegates::OnObjectModified.Remove(Listener);
	TestEqual(TEXT("baking tells no listener a brush or its model changed"), ModifiedBrushes, 0);
	TestEqual(TEXT("nothing could not be baked"), First.Problems.Num(), 0);
	if (!TestTrue(TEXT("the room is baked into a mesh"), First.Meshes.Num() >= 1))
	{
		return false;
	}
	const FMantleBakedSplineMesh& Baked = First.Meshes[0];
	const FStaticMeshRenderData* RenderData = Baked.Mesh ? Baked.Mesh->GetRenderData() : nullptr;
	const int32 Triangles = RenderData && RenderData->LODResources.Num() > 0 ? RenderData->LODResources[0].GetNumTriangles() : 0;
	// A solid cube is 12 triangles; cutting a room out of it adds the inside walls.
	TestTrue(TEXT("with the outside and the inside walls"), Triangles >= 24);
	TestTrue(TEXT("visible"), Baked.bVisible);
	TestTrue(TEXT("solid"), Baked.Collision == EMantleCollision::Blocking);
	TestTrue(TEXT("and centered on the room"), Baked.Transform.GetLocation().Equals(FVector::ZeroVector, 1.0));

	// Cached while the brushes are unchanged: the same mesh, not a new cut.
	const FMantleSplineBakeResult Second = FMantleBrushBake::Bake(World, Limits);
	TestTrue(TEXT("a second bake returns the same mesh"), Second.Meshes.Num() == First.Meshes.Num() && Second.Meshes[0].Mesh == Baked.Mesh);

	// The creator's level is as it was: every brush, and the level's own model.
	TestTrue(TEXT("baking changed no brush and not the level's BSP"), Before.Matches(FLevelSnapshot::Take(World)));

	// Export writes it as a mesh entity, says so, and Check does not warn about the brushes.
	const FMantleExportResult Result = FMantlePlaceExporter::Export(World, FMantleExportOptions());
	bool bFoundMesh = false;
	for (const FMantleEntity* Entity : MantleExportedEntities::All(Result))
	{
		const FMantleStaticMeshProps* Props = Entity->Props.TryGet<FMantleStaticMeshProps>();
		if (Props && Entity->Name.StartsWith(TEXT("BSP")))
		{
			bFoundMesh = true;
			TestTrue(TEXT("a drawn, blocking mesh"), Props->bVisible && Props->Collision == EMantleCollision::Blocking);
		}
	}
	TestTrue(TEXT("Export writes the BSP as a mesh entity"), bFoundMesh);
	TestEqual(TEXT("the report lists it as approximated"), Result.Coverage.Count(MantleCoverageCategory::Bsp, EMantleCoverageOutcome::Approximated), static_cast<int64>(2));
	TestEqual(TEXT("and drops nothing of it"), Result.Coverage.Count(MantleCoverageCategory::Bsp, EMantleCoverageOutcome::Dropped), static_cast<int64>(0));
	const FMantleValidationResult Validation = FMantlePlaceValidator::Validate(World);
	TestFalse(TEXT("Check does not warn about the room"), HasIssue(Validation, TEXT("Room"), EMantleIssueSeverity::Warning) || HasIssue(Validation, TEXT("Room"), EMantleIssueSeverity::Error));
	TestTrue(TEXT("a publish left the level as it was"), Before.Matches(FLevelSnapshot::Take(World)));

	// A moved brush is a new cut.
	Room->SetActorLocation(FVector(500.0, 0.0, 0.0));
	const FMantleSplineBakeResult Moved = FMantleBrushBake::Bake(World, Limits);
	TestTrue(TEXT("moving a brush bakes again"), Moved.Meshes.Num() >= 1 && Moved.Meshes[0].Mesh != Baked.Mesh);

	// A movable brush is not baked, and says so.
	Room->GetBrushComponent()->SetMobility(EComponentMobility::Movable);
	TestFalse(TEXT("a movable brush is not BSP the bake takes"), FMantleBrushBake::IsBakeSource(Room));
	FString What;
	FString Remedy;
	TestTrue(TEXT("and it is named as left out"), FMantlePlaceExporter::DescribeUnpublishedContent(Room, What, Remedy));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleBrushBakeOrderTest,
	"Mantle.Editor.BrushBake.BrushesCutInTheLevelsActorOrder",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleBrushBakeOrderTest::RunTest(const FString& Parameters)
{
	using namespace MantleBrushBakeTestPrivate;
	if (!GEditor)
	{
		AddWarning(TEXT("no editor; skipped"));
		return true;
	}
	FBspWorld Fixture;
	UWorld* World = Fixture.World;
	if (!TestNotNull(TEXT("a test world"), World))
	{
		return false;
	}

	// A wall placed first, and the doorway cut in it placed second: the doorway's object name sorts BEFORE the wall's, so a bake that
	// ordered brushes by name would cut first and add after, and publish a solid wall.
	ABrush* Wall = SpawnCubeBrush(World, TEXT("Wall"), FVector::ZeroVector, 800.0f, Brush_Add, TEXT("Brush_Z_Wall"));
	ABrush* Door = SpawnCubeBrush(World, TEXT("Doorway"), FVector::ZeroVector, 400.0f, Brush_Subtract, TEXT("Brush_A_Door"));
	if (!TestNotNull(TEXT("the wall"), Wall) || !TestNotNull(TEXT("the doorway"), Door))
	{
		return false;
	}
	TestTrue(TEXT("the doorway's name sorts before the wall's"), Door->GetPathName().Compare(Wall->GetPathName()) < 0);
	const TArray<ABrush*> Sources = FMantleBrushBake::CollectSources(World);
	if (!TestEqual(TEXT("two sources"), Sources.Num(), 2))
	{
		return false;
	}
	TestTrue(TEXT("in the level's actor order: the wall, then the doorway"), Sources[0] == Wall && Sources[1] == Door);

	const FMantlePlaceAssetLimits Limits = FMantlePlaceAssetLimits::FromContract();
	auto Triangles = [](const FMantleSplineBakeResult& Bake)
	{
		int32 Count = 0;
		for (const FMantleBakedSplineMesh& Mesh : Bake.Meshes)
		{
			Count += Mesh.Triangles;
		}
		return Count;
	};
	// A solid 800 cube is 12 triangles; with the doorway cut out of it, the inside faces add to that.
	TestTrue(TEXT("the doorway is cut: the wall has an inside"), Triangles(FMantleBrushBake::BakeBrushes(Sources, Limits)) > 12);
	// Handed over in the wrong order, the bake still takes the level's.
	const TArray<ABrush*> Swapped = { Door, Wall };
	TestTrue(TEXT("whatever order the brushes are handed over in"), Triangles(FMantleBrushBake::BakeBrushes(Swapped, Limits)) > 12);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleBrushBakeTwoSidedTest,
	"Mantle.Editor.BrushBake.TwoSidedSurfacesDrawAndCollideBothWays",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleBrushBakeTwoSidedTest::RunTest(const FString& Parameters)
{
	using namespace MantleBrushBakeTestPrivate;
	if (!GEditor)
	{
		AddWarning(TEXT("no editor; skipped"));
		return true;
	}
	FBspWorld Fixture;
	UWorld* World = Fixture.World;
	if (!TestNotNull(TEXT("a test world"), World))
	{
		return false;
	}
	ABrush* OneSided = SpawnCubeBrush(World, TEXT("Solid cube"), FVector::ZeroVector, 400.0f, Brush_Add);
	ABrush* TwoSided = SpawnCubeBrush(World, TEXT("Two-sided cube"), FVector(5000.0, 0.0, 0.0), 400.0f, Brush_Add, nullptr, PF_TwoSided);
	if (!TestNotNull(TEXT("the one-sided cube"), OneSided) || !TestNotNull(TEXT("the two-sided cube"), TwoSided))
	{
		return false;
	}
	const FMantlePlaceAssetLimits Limits = FMantlePlaceAssetLimits::FromContract();
	auto Triangles = [&Limits](ABrush* Brush)
	{
		int32 Count = 0;
		const TArray<ABrush*> One = { Brush };
		for (const FMantleBakedSplineMesh& Mesh : FMantleBrushBake::BakeBrushes(One, Limits).Meshes)
		{
			Count += Mesh.Triangles;
		}
		return Count;
	};
	TestEqual(TEXT("a solid cube is 12 triangles"), Triangles(OneSided), 12);
	// Each face twice: the front, and the same triangle reversed. The reversed ones are in the mesh, so the collision (the mesh's
	// triangles) stops a player coming from either side as well.
	TestEqual(TEXT("a two-sided cube has a reversed triangle for each"), Triangles(TwoSided), 24);

	// A not-solid sheet (PF_NotSolid: a player walks through it) is drawn, both sides, but the mesh it is in has no collision.
	ABrush* Sheet = SpawnCubeBrush(World, TEXT("Not-solid sheet"), FVector(10000.0, 0.0, 0.0), 400.0f, Brush_Add, nullptr, PF_TwoSided | PF_NotSolid);
	if (!TestNotNull(TEXT("the not-solid sheet"), Sheet))
	{
		return false;
	}
	const TArray<ABrush*> Both = { OneSided, Sheet };
	const FMantleSplineBakeResult Mixed = FMantleBrushBake::BakeBrushes(Both, Limits);
	int32 SolidTriangles = 0;
	int32 NotSolidTriangles = 0;
	for (const FMantleBakedSplineMesh& Mesh : Mixed.Meshes)
	{
		(Mesh.Collision == EMantleCollision::None ? NotSolidTriangles : SolidTriangles) += Mesh.Triangles;
		TestTrue(TEXT("every baked mesh is drawn"), Mesh.bVisible);
	}
	TestEqual(TEXT("the solid cube still blocks: 12 triangles with collision"), SolidTriangles, 12);
	TestEqual(TEXT("the not-solid sheet draws both sides: 24 triangles"), NotSolidTriangles, 24);
	TestTrue(TEXT("and it has no collision"), Mixed.Meshes.ContainsByPredicate([](const FMantleBakedSplineMesh& Mesh) { return Mesh.Collision == EMantleCollision::None; }));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
