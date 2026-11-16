// Copyright Joshua Eger. All Rights Reserved.
#include "MantleHeightfieldReader.h"
#include "MantlePlaceBuilder.h"
#include "MantleTerrainBuilder.h"
#include "MantleTerrainCollision.h"
#include "MantleTerrainStream.h"

#include "Async/TaskGraphInterfaces.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshResources.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

// The far ground (stage 3a): the plan's pure functions with hand-worked numbers, the far chunk the builder makes, a far-only spawn, and the stream component's swaps: a chunk is drawn
// by its full version or by its far one and never both and never neither, on every step of a view that walks away.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleTerrainStreamTestPrivate
{
	/** 513 x 513 samples 100 cm apart: 512 quads, so 4 x 4 chunks of 128 quads (12,800 cm), 51,200 cm a side. */
	TSharedRef<FMantleHeightfield> MakeField()
	{
		TSharedRef<FMantleHeightfield> Field = MakeShared<FMantleHeightfield>();
		Field->Width = 513;
		Field->Height = 513;
		Field->SpacingX = 100.0f;
		Field->SpacingY = 100.0f;
		Field->HeightScale = 1.0f;
		Field->HeightOffset = 0.0f;
		Field->Heights.SetNumUninitialized(513 * 513);
		for (int32 Y = 0; Y < 513; ++Y)
		{
			for (int32 X = 0; X < 513; ++X)
			{
				Field->Heights[Y * 513 + X] = static_cast<uint16>(X * 10 + (Y % 7) + (X % 5));
			}
		}
		return Field;
	}

	bool SameInts(const TArray<int32>& A, std::initializer_list<int32> B)
	{
		if (A.Num() != static_cast<int32>(B.size()))
		{
			return false;
		}
		int32 Index = 0;
		for (const int32 Value : B)
		{
			if (A[Index++] != Value)
			{
				return false;
			}
		}
		return true;
	}

	/** Every vertex position (skirts included, exactly) and every triangle's vertices the same, in the same order. */
	bool SameDescription(const FMeshDescription& A, const FMeshDescription& B)
	{
		if (A.Vertices().Num() != B.Vertices().Num() || A.Triangles().Num() != B.Triangles().Num())
		{
			return false;
		}
		const FStaticMeshConstAttributes AttributesA(A);
		const FStaticMeshConstAttributes AttributesB(B);
		const TVertexAttributesConstRef<FVector3f> PositionsA = AttributesA.GetVertexPositions();
		const TVertexAttributesConstRef<FVector3f> PositionsB = AttributesB.GetVertexPositions();
		for (const FVertexID Vertex : A.Vertices().GetElementIDs())
		{
			if (PositionsA[Vertex] != PositionsB[Vertex])
			{
				return false;
			}
		}
		for (const FTriangleID Triangle : A.Triangles().GetElementIDs())
		{
			const auto VerticesA = A.GetTriangleVertices(Triangle);
			const auto VerticesB = B.GetTriangleVertices(Triangle);
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				if (VerticesA[Corner] != VerticesB[Corner])
				{
					return false;
				}
			}
		}
		return true;
	}

	FMantleTerrainBuildParams MakeParams()
	{
		FMantleTerrainBuildParams Params;
		// Nothing renders in the test run: the meshes are the CPU-only ones the dedicated server builds, with every LOD kept.
		Params.bInitRenderResources = false;
		Params.bCollision = true;
		return Params;
	}

	/** An empty game world for one test, destroyed with it. */
	struct FStreamWorld
	{
		UWorld* World = nullptr;

		/** bBeginPlay: the world has begun play, so an actor spawned in it begins play with it (and ends play when it is destroyed or the world goes). */
		explicit FStreamWorld(const bool bBeginPlay = false)
		{
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("MantleTerrainStreamTestWorld"));
			if (World && bBeginPlay)
			{
				World->InitializeActorsForPlay(FURL());
				World->GetWorldSettings()->NotifyBeginPlay();
			}
		}

		~FStreamWorld()
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

	/** One far-only terrain with its stream component, as the place builder makes them. */
	struct FStreamedTerrain
	{
		TSharedRef<FMantleHeightfield> Field = MakeField();
		AActor* Actor = nullptr;
		UMantleTerrainStreamComponent* Stream = nullptr;
		int32 SpawnSteps = 0;
		FString Error;

		bool Make(UWorld* World, const int32 DropFarIndex = INDEX_NONE)
		{
			FMantleTerrainBuildParams FarParams = MakeParams();
			FarParams.bFarOnly = true;
			FarParams.FieldOwner = Field;
			TSharedPtr<FMantleTerrainSpawn> Spawn = FMantleTerrainSpawn::Begin(World, TEXT("Far"), *Field, nullptr, true, FarParams, &Error);
			if (!Spawn.IsValid())
			{
				return false;
			}
			while (!Spawn->Step() && SpawnSteps < 1000)
			{
				++SpawnSteps;
			}
			++SpawnSteps;
			if (Spawn->HasFailed())
			{
				Error = Spawn->GetError();
				return false;
			}
			TArray<TWeakObjectPtr<UStaticMeshComponent>> FarComponents = Spawn->TakeFarComponents();
			if (FarComponents.IsValidIndex(DropFarIndex))
			{
				// A chunk with no far mesh: all hole.
				FarComponents[DropFarIndex].Get()->DestroyComponent();
				FarComponents[DropFarIndex].Reset();
			}
			Actor = Spawn->TakeActor();
			if (!Actor)
			{
				return false;
			}
			Stream = NewObject<UMantleTerrainStreamComponent>(Actor, TEXT("TerrainStream"));
			FMantleTerrainBuildParams FullParams = MakeParams();
			FullParams.Outer = Actor;
			Stream->Init(Field, FullParams, nullptr, Spawn->IsStatic(), true, MoveTemp(FarComponents));
			Actor->AddInstanceComponent(Stream);
			Stream->RegisterComponent();
			return true;
		}

		/** Steps the stream for these views until Done says so (workers and cooks finish between steps); false when it did not in time. */
		template <typename TDone>
		bool Run(const TArray<FVector>& Views, const double Radius, TDone&& Done, const int32 MaxSteps = 4000)
		{
			for (int32 Step = 0; Step < MaxSteps; ++Step)
			{
				++GFrameCounter;
				FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
				Stream->StepStream(Views, Radius);
				if (Done())
				{
					return true;
				}
				FPlatformProcess::Sleep(0.002f);
			}
			return false;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainDrawsFarGroundTest,
	"Mantle.Runtime.TerrainStream.DrawsFarGroundOnEveryMachineThatDraws",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainDrawsFarGroundTest::RunTest(const FString& Parameters)
{
	// Far ground plus near chunks on every machine that draws, for every place (with cells or without); a dedicated server draws nothing and builds no chunk. The test-only
	// -MantleTestFullTerrain switch is not on the automation command line.
	TestTrue(TEXT("a standalone game"), FMantlePlaceBuilder::DrawsTerrainAsFarGround(NM_Standalone));
	TestTrue(TEXT("a client"), FMantlePlaceBuilder::DrawsTerrainAsFarGround(NM_Client));
	TestTrue(TEXT("a listen server"), FMantlePlaceBuilder::DrawsTerrainAsFarGround(NM_ListenServer));
	TestFalse(TEXT("a dedicated server"), FMantlePlaceBuilder::DrawsTerrainAsFarGround(NM_DedicatedServer));

	// A world that is not a game world (the editor's importer and preview) draws every chunk in full: no stream there would build them.
	TestFalse(TEXT("no world"), FMantlePlaceBuilder::DrawsTerrainAsFarGround(static_cast<const UWorld*>(nullptr)));
	UWorld* EditorWorld = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld*/ false, TEXT("MantleFarGroundEditorTestWorld"));
	UWorld* GameWorld = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("MantleFarGroundGameTestWorld"));
	if (TestNotNull(TEXT("an editor world"), EditorWorld))
	{
		TestFalse(TEXT("an editor world draws every chunk in full"), FMantlePlaceBuilder::DrawsTerrainAsFarGround(EditorWorld));
		EditorWorld->DestroyWorld(/*bInformEngineOfWorld*/ false);
		if (EditorWorld->IsRooted())
		{
			EditorWorld->RemoveFromRoot();
		}
	}
	if (TestNotNull(TEXT("a game world"), GameWorld))
	{
		TestTrue(TEXT("a standalone game world draws the far ground"), FMantlePlaceBuilder::DrawsTerrainAsFarGround(GameWorld));
		GameWorld->DestroyWorld(/*bInformEngineOfWorld*/ false);
		if (GameWorld->IsRooted())
		{
			GameWorld->RemoveFromRoot();
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainStreamGridTest,
	"Mantle.Runtime.TerrainStream.Plan.Grid",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainStreamGridTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainStreamTestPrivate;
	const FMantleHeightfield Field = *MakeField();
	const FMantleTerrainChunkGrid Grid = FMantleTerrainChunkGrid::Make(Field, 128);
	TestEqual(TEXT("512 quads of 128 are 4 chunks along X"), Grid.ChunksX, 4);
	TestEqual(TEXT("and along Y"), Grid.ChunksY, 4);
	TestEqual(TEXT("sixteen chunks"), Grid.Num(), 16);
	TestTrue(TEXT("chunk 6 is (2, 1)"), Grid.CoordOf(6) == FIntPoint(2, 1));
	TestEqual(TEXT("(2, 1) is chunk 6"), Grid.IndexOf(2, 1), 6);

	FVector2D Min;
	FVector2D Max;
	Grid.RectOf(1, Min, Max);
	TestTrue(TEXT("chunk (1, 0) starts at sample 128, 12,800 cm"), Min == FVector2D(12800.0, 0.0));
	TestTrue(TEXT("and ends at sample 256, 25,600 cm"), Max == FVector2D(25600.0, 12800.0));

	TestEqual(TEXT("a point 12,800 cm left of chunk (1, 0)"), Grid.DistanceTo(1, FVector2D(0.0, 0.0)), 0.0 + 12800.0);
	TestEqual(TEXT("a point inside is 0"), Grid.DistanceTo(1, FVector2D(15000.0, 6000.0)), 0.0);
	// (30000, 16000): 4,400 past the right edge and 3,200 above the top: sqrt(4400^2 + 3200^2).
	TestEqual(TEXT("a point off the corner"), Grid.DistanceTo(1, FVector2D(30000.0, 16000.0)), FMath::Sqrt(4400.0 * 4400.0 + 3200.0 * 3200.0), 1.0e-6);

	// 300 samples: 299 quads, ceil(299 / 128) = 3 chunks, the last one 43 quads wide (samples 256 to 299).
	FMantleHeightfield Small = Field;
	Small.Width = 300;
	Small.Height = 300;
	Small.Heights.SetNum(300 * 300);
	const FMantleTerrainChunkGrid SmallGrid = FMantleTerrainChunkGrid::Make(Small, 128);
	TestEqual(TEXT("three chunks"), SmallGrid.ChunksX, 3);
	SmallGrid.RectOf(2, Min, Max);
	TestEqual(TEXT("the last chunk starts at sample 256"), Min.X, 25600.0);
	TestEqual(TEXT("and ends at the terrain's edge, sample 299"), Max.X, 29900.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainStreamWantsTest,
	"Mantle.Runtime.TerrainStream.Plan.Wants",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainStreamWantsTest::RunTest(const FString& Parameters)
{
	using Plan = FMantleTerrainStreamPlan;
	TestTrue(TEXT("on the radius"), Plan::Wants(100.0, false, 100.0, 64.0));
	TestFalse(TEXT("past the radius, not full yet"), Plan::Wants(101.0, false, 100.0, 64.0));
	TestTrue(TEXT("past the radius, full already: the margin keeps it"), Plan::Wants(164.0, true, 100.0, 64.0));
	TestFalse(TEXT("past the margin"), Plan::Wants(165.0, true, 100.0, 64.0));
	TestFalse(TEXT("no radius wants nothing"), Plan::Wants(0.0, true, 0.0, 64.0));
	TestEqual(TEXT("a small place's cells give the least radius"), Plan::NearRadiusFor(10000.0), Plan::MinNearRadiusCm);
	TestEqual(TEXT("a huge draw distance is capped"), Plan::NearRadiusFor(500000.0), Plan::MaxNearRadiusCm);
	TestEqual(TEXT("in between it is the draw distance"), Plan::NearRadiusFor(40000.0), 40000.0);
	TestEqual(TEXT("no cells at all"), Plan::NearRadiusFor(0.0), Plan::MinNearRadiusCm);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainStreamPlanMakeTest,
	"Mantle.Runtime.TerrainStream.Plan.Make",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainStreamPlanMakeTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainStreamTestPrivate;
	const FMantleTerrainChunkGrid Grid = FMantleTerrainChunkGrid::Make(*MakeField(), 128);
	TArray<EMantleTerrainChunkState> States;
	States.Init(EMantleTerrainChunkState::Far, 16);
	TArray<int32> Build;
	TArray<int32> Release;
	const double Margin = Grid.ChunkSizeCm();
	TestEqual(TEXT("the margin is a chunk"), Margin, 12800.0);

	// A view in the middle of chunk (1, 1), 19,200 cm from each edge; a radius of 5,000 reaches nothing but its own chunk.
	const TArray<FVector2D> View{ FVector2D(19200.0, 19200.0) };
	FMantleTerrainStreamPlan::Make(Grid, View, 5000.0, Margin, States, Build, Release);
	TestTrue(TEXT("one chunk wanted"), SameInts(Build, { 5 }));
	TestEqual(TEXT("nothing to release"), Release.Num(), 0);

	// 13,000: the four neighbours are 6,400 away (the view is 6,400 from each edge of its chunk) and the diagonals sqrt(2) x 6,400 = 9,051; the next ring is 19,200 away.
	FMantleTerrainStreamPlan::Make(Grid, View, 13000.0, Margin, States, Build, Release);
	TestTrue(TEXT("nine chunks, nearest first, ties by index"), SameInts(Build, { 5, 1, 4, 6, 9, 0, 2, 8, 10 }));

	// A chunk that is full and 19,200 away keeps standing inside radius + margin (25,800); a full chunk that is further (the far corner, (3, 3): 19,200 x sqrt(2) = 27,153) is released.
	States[7] = EMantleTerrainChunkState::Near;   // (3, 1): 19,200 from the view
	States[15] = EMantleTerrainChunkState::Near;  // (3, 3)
	States[2] = EMantleTerrainChunkState::Building;
	States[3] = EMantleTerrainChunkState::Empty;
	FMantleTerrainStreamPlan::Make(Grid, View, 13000.0, Margin, States, Build, Release);
	TestTrue(TEXT("the full chunk inside the margin stays, the one outside goes"), SameInts(Release, { 15 }));
	TestFalse(TEXT("a chunk being built is not planned again"), Build.Contains(2));
	TestFalse(TEXT("nor is one that is full already"), Build.Contains(7));

	// Two views: the second wants the far corner too.
	const TArray<FVector2D> TwoViews{ FVector2D(19200.0, 19200.0), FVector2D(48000.0, 48000.0) };
	States.Init(EMantleTerrainChunkState::Far, 16);
	FMantleTerrainStreamPlan::Make(Grid, TwoViews, 5000.0, Margin, States, Build, Release);
	TestTrue(TEXT("each view's own chunk, by distance (both 0), then index"), SameInts(Build, { 5, 15 }));

	// No views, no radius: nothing wanted, and everything full is let go.
	States[5] = EMantleTerrainChunkState::Near;
	FMantleTerrainStreamPlan::Make(Grid, TArray<FVector2D>(), 13000.0, Margin, States, Build, Release);
	TestEqual(TEXT("no view wants nothing"), Build.Num(), 0);
	TestTrue(TEXT("and releases what stands"), SameInts(Release, { 5 }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainFarChunkTest,
	"Mantle.Runtime.TerrainStream.Builder.FarChunk",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainFarChunkTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainStreamTestPrivate;
	const FMantleHeightfield Field = *MakeField();
	const FMantleTerrainBuildParams Params = MakeParams();

	// The interior chunk (1, 1): a grid of 8 x 8 quads (step 16 over 128) = 128 triangles, and a skirt on all four edges, each 8 segments of 4 triangles = 128. 9 x 9 vertices
	// and 4 x 9 skirt vertices.
	FMantleTerrainPreparedChunk Interior;
	FMantleTerrainBuilder::PrepareChunk(Field, Params, 1, 1, EMantleTerrainChunkDetail::Far, Interior);
	if (!TestEqual(TEXT("one description"), Interior.Descriptions.Num(), 1))
	{
		return false;
	}
	TestEqual(TEXT("a quarter of 2 x 8 x 8 + 4 x 32 triangles"), Interior.Descriptions[0].Triangles().Num(), 2 * 8 * 8 + 4 * 32);
	TestEqual(TEXT("81 + 36 vertices"), static_cast<int32>(Interior.Vertices), 81 + 36);
	TestTrue(TEXT("it starts at sample (128, 128)"), Interior.FirstSample == FIntPoint(128, 128));
	TestTrue(TEXT("its origin is 12,800 cm along each axis"), Interior.Origin == FVector3f(12800.0f, 12800.0f, 0.0f));

	// The corner (0, 0) has skirts on two edges (right, top): 128 + 4 x 16 triangles. The edge chunk (1, 0) has three (not the bottom, which is the terrain's border).
	FMantleTerrainPreparedChunk Corner;
	FMantleTerrainBuilder::PrepareChunk(Field, Params, 0, 0, EMantleTerrainChunkDetail::Far, Corner);
	TestEqual(TEXT("a corner has two skirts"), Corner.Descriptions[0].Triangles().Num(), 2 * 8 * 8 + 4 * 16);
	FMantleTerrainPreparedChunk Edge;
	FMantleTerrainBuilder::PrepareChunk(Field, Params, 1, 0, EMantleTerrainChunkDetail::Far, Edge);
	TestEqual(TEXT("a side chunk has three skirts"), Edge.Descriptions[0].Triangles().Num(), 2 * 8 * 8 + 4 * 24);

	// The full chunk: 5 LODs, the first with every sample (128 x 128 quads), and the same skirt depth, so the two meet.
	FMantleTerrainBuildParams ServerParams = Params;
	ServerParams.bRenderLODs = false;
	FMantleTerrainPreparedChunk Full;
	FMantleTerrainBuilder::PrepareChunk(Field, ServerParams, 1, 1, EMantleTerrainChunkDetail::Full, Full);
	TestEqual(TEXT("a server's full chunk has one LOD"), Full.Descriptions.Num(), 1);
	FMantleTerrainBuildParams RenderParams = Params;
	RenderParams.bRenderLODs = true;
	FMantleTerrainPreparedChunk FullLODs;
	FMantleTerrainBuilder::PrepareChunk(Field, RenderParams, 1, 1, EMantleTerrainChunkDetail::Full, FullLODs);
	TestEqual(TEXT("five LODs"), FullLODs.Descriptions.Num(), 5);
	TestTrue(TEXT("the last of them is the far chunk's mesh: every vertex position (skirt depth included) and every triangle's vertices the same"),
		SameDescription(FullLODs.Descriptions.Last(), Interior.Descriptions[0]));
	// And the same for a corner chunk and a side chunk, whose skirts are on other edges.
	FMantleTerrainPreparedChunk CornerLODs;
	FMantleTerrainBuilder::PrepareChunk(Field, RenderParams, 0, 0, EMantleTerrainChunkDetail::Full, CornerLODs);
	TestTrue(TEXT("a corner chunk's last LOD is its far chunk"), SameDescription(CornerLODs.Descriptions.Last(), Corner.Descriptions[0]));
	FMantleTerrainPreparedChunk EdgeLODs;
	FMantleTerrainBuilder::PrepareChunk(Field, RenderParams, 1, 0, EMantleTerrainChunkDetail::Full, EdgeLODs);
	TestTrue(TEXT("a side chunk's last LOD is its far chunk"), SameDescription(EdgeLODs.Descriptions.Last(), Edge.Descriptions[0]));
	// A different chunk is not the same mesh (the comparison can fail).
	TestFalse(TEXT("the interior chunk's far mesh is not the corner chunk's"), SameDescription(Interior.Descriptions[0], Corner.Descriptions[0]));
	TestEqual(TEXT("LOD 0 is 128 x 128 quads x 2 triangles, plus skirts of 128 segments x 4 triangles x 4 edges"), FullLODs.Descriptions[0].Triangles().Num(), 2 * 128 * 128 + 4 * 4 * 128);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainFarSpawnTest,
	"Mantle.Runtime.TerrainStream.Spawn.FarOnly",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainFarSpawnTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainStreamTestPrivate;
	FStreamWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	const TSharedRef<FMantleHeightfield> Field = MakeField();
	FMantleTerrainBuildParams Params = MakeParams();
	Params.bFarOnly = true;
	FString Error;
	TSharedPtr<FMantleTerrainSpawn> Spawn = FMantleTerrainSpawn::Begin(Fixture.World, TEXT("Far"), *Field, nullptr, true, Params, &Error);
	if (!TestTrue(TEXT("the far terrain begins"), Spawn.IsValid()))
	{
		AddError(Error);
		return false;
	}
	int32 Steps = 0;
	while (!Spawn->Step() && Steps < 100)
	{
		++Steps;
	}
	TestFalse(TEXT("nothing failed"), Spawn->HasFailed());
	TestEqual(TEXT("chunks along X"), Spawn->GetChunksX(), 4);
	TArray<TWeakObjectPtr<UStaticMeshComponent>> Far = Spawn->TakeFarComponents();
	if (!TestEqual(TEXT("a slot for each of 16 chunks"), Far.Num(), 16))
	{
		return false;
	}
	AActor* Actor = Spawn->TakeActor();
	if (!TestNotNull(TEXT("the actor"), Actor))
	{
		return false;
	}
	int32 Components = 0;
	for (int32 Index = 0; Index < Far.Num(); ++Index)
	{
		UStaticMeshComponent* Component = Far[Index].Get();
		if (!TestNotNull(*FString::Printf(TEXT("far chunk %d"), Index), Component))
		{
			continue;
		}
		++Components;
		TestTrue(TEXT("registered"), Component->IsRegistered());
		TestFalse(TEXT("casts no shadow"), Component->CastShadow);
		TestTrue(TEXT("no collision"), Component->GetCollisionEnabled() == ECollisionEnabled::NoCollision);
		TestNotNull(TEXT("a mesh"), Component->GetStaticMesh().Get());
		// One LOD only.
		if (Component->GetStaticMesh() && Component->GetStaticMesh()->GetRenderData())
		{
			TestEqual(TEXT("one LOD"), Component->GetStaticMesh()->GetRenderData()->LODResources.Num(), 1);
		}
		// Placed at the chunk's origin: 12,800 cm a chunk.
		TestEqual(TEXT("at the chunk's X"), Component->GetRelativeLocation().X, static_cast<double>(Index % 4) * 12800.0);
		TestEqual(TEXT("at the chunk's Y"), Component->GetRelativeLocation().Y, static_cast<double>(Index / 4) * 12800.0);
	}
	TestEqual(TEXT("sixteen far components"), Components, 16);
	TInlineComponentArray<UStaticMeshComponent*> OnActor(Actor);
	TestEqual(TEXT("and nothing else on the actor"), OnActor.Num(), 16);
	Actor->Destroy();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainStreamSwapTest,
	"Mantle.Runtime.TerrainStream.Stream.SwapsLeaveNoGap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainStreamSwapTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainStreamTestPrivate;
	FStreamWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	FStreamedTerrain Terrain;
	if (!TestTrue(TEXT("the far ground and its stream stand"), Terrain.Make(Fixture.World)))
	{
		AddError(Terrain.Error);
		return false;
	}
	UMantleTerrainStreamComponent& Stream = *Terrain.Stream;
	// The terrain is at the origin, so world space is terrain space.
	const TArray<FVector> Middle{ FVector(19200.0, 19200.0, 0.0) };
	TestFalse(TEXT("nothing stands before the first step"), Stream.AreaStands(Middle, 13000.0));

	// A view in the middle of chunk (1, 1) with a radius of 13,000 wants the nine chunks around it (the plan test works the same numbers).
	const auto NearCount = [&Stream]() { int32 N = 0; for (int32 I = 0; I < 16; ++I) { N += Stream.GetState(I) == EMantleTerrainChunkState::Near ? 1 : 0; } return N; };
	const bool bBuilt = Terrain.Run(Middle, 13000.0, [&]() { return NearCount() == 9; });
	if (!TestTrue(TEXT("nine chunks stand in full"), bBuilt))
	{
		AddError(FString::Printf(TEXT("near %d, building %d, failed %d"), NearCount(), Stream.GetStats().BuildingNow, Stream.GetStats().FailedNow));
		return false;
	}
	const int32 Expected[] = { 0, 1, 2, 4, 5, 6, 8, 9, 10 };
	for (const int32 Index : Expected)
	{
		TestTrue(*FString::Printf(TEXT("chunk %d is full"), Index), Stream.GetState(Index) == EMantleTerrainChunkState::Near);
	}
	TestTrue(TEXT("the ground under the view stands"), Stream.AreaStands(Middle, 13000.0));
	TestFalse(TEXT("the ground 30,000 cm out does not"), Stream.AreaStands(Middle, 30000.0));

	FMantleTerrainAudit Audit = Stream.Audit();
	TestEqual(TEXT("sixteen chunks have a mesh"), Audit.WithMesh, 16);
	TestEqual(TEXT("nine are drawn by their full version"), Audit.DrawnFull, 9);
	TestEqual(TEXT("seven by the far one"), Audit.DrawnFar, 7);
	TestEqual(TEXT("none twice"), Audit.DrawnBoth, 0);
	TestEqual(TEXT("none not at all"), Audit.DrawnNeither, 0);
	TestEqual(TEXT("nine swapped in"), Stream.GetStats().SwapsToNear, static_cast<int64>(9));

	// A swapped chunk is a picture: the full component casts a shadow and collides with nothing (the ground's collision is the height-field blocks, made with the far ground and never
	// touched by a swap); the far one is off.
	TInlineComponentArray<UStaticMeshComponent*> Registered(Terrain.Actor);
	int32 WithShadow = 0;
	for (UStaticMeshComponent* Component : Registered)
	{
		TestEqual(TEXT("no chunk collides"), Component->GetCollisionEnabled(), ECollisionEnabled::NoCollision);
		if (Component->IsRegistered() && Component->CastShadow)
		{
			++WithShadow;
		}
	}
	TestEqual(TEXT("nine components cast a shadow"), WithShadow, 9);
	TArray<UMantleTerrainCollisionComponent*> Blocks;
	Terrain.Actor->GetComponents<UMantleTerrainCollisionComponent>(Blocks);
	TestEqual(TEXT("the sixteen blocks of ground stand all the time"), Blocks.Num(), 16);

	// The audit sees a double draw: the far component of a full chunk switched back on is found, once.
	AddExpectedError(TEXT("MANTLE_TERRAIN_AUDIT_FAIL"), EAutomationExpectedErrorFlags::Contains, 1);
	{
		const int64 FailuresBefore = Stream.GetStats().AuditFailures;
		// The far component of chunk 5 (the one in the middle): found by its name, Far_<first sample X>_<first sample Y> = Far_128_128.
		UStaticMeshComponent* Far = nullptr;
		for (UStaticMeshComponent* Component : Registered)
		{
			if (Component->GetName() == TEXT("Far_128_128"))
			{
				Far = Component;
			}
		}
		if (TestNotNull(TEXT("chunk 5's far component"), Far))
		{
			TestFalse(TEXT("it is off"), Far->IsRegistered());
			Far->RegisterComponent();
			const FMantleTerrainAudit Bad = Stream.Audit();
			TestEqual(TEXT("the audit finds the double draw"), Bad.DrawnBoth, 1);
			TestEqual(TEXT("and counts a failure"), Stream.GetStats().AuditFailures, FailuresBefore + 1);
			Far->UnregisterComponent();
		}
	}

	// The view walks to the opposite corner. On every step, whatever the stream is doing, every chunk is drawn exactly once.
	const TArray<FVector> Corner{ FVector(48000.0, 48000.0, 0.0) };
	int32 CleanAudits = 0;
	int32 DirtyAudits = 0;
	const bool bMoved = Terrain.Run(Corner, 13000.0, [&]()
	{
		const FMantleTerrainAudit Now = Stream.Audit();
		(Now.IsClean() ? CleanAudits : DirtyAudits) += 1;
		return NearCount() == 6 && Stream.GetStats().BuildingNow == 0;
	});
	TestTrue(TEXT("the full chunks follow the view"), bMoved);
	TestEqual(TEXT("not one audit found a double draw or a gap"), DirtyAudits, 0);
	TestTrue(TEXT("and the audits ran"), CleanAudits > 0);
	// (3,3), (2,3) and (3,2) are in reach now; (2,2), (2,1) and (1,2) stand from before and are within the margin (25,800) of the new view: 6. The other six of the first nine go.
	const int32 StillNear[] = { 6, 9, 10, 11, 14, 15 };
	for (const int32 Index : StillNear)
	{
		TestTrue(*FString::Printf(TEXT("chunk %d is full"), Index), Stream.GetState(Index) == EMantleTerrainChunkState::Near);
	}
	TestEqual(TEXT("twelve swapped in over the run"), Stream.GetStats().SwapsToNear, static_cast<int64>(12));
	TestEqual(TEXT("six swapped out"), Stream.GetStats().SwapsToFar, static_cast<int64>(6));
	TestEqual(TEXT("the ground under the old view no longer stands"), Stream.AreaStands(Middle, 13000.0), false);
	TestTrue(TEXT("the ground under the new one does"), Stream.AreaStands(Corner, 13000.0));
	Audit = Stream.Audit();
	TestEqual(TEXT("six drawn in full"), Audit.DrawnFull, 6);
	TestEqual(TEXT("ten by the far ground"), Audit.DrawnFar, 10);
	TestTrue(TEXT("clean at the end"), Audit.IsClean());

	// A view that goes away lets every full chunk go (radius 0: nothing is wanted).
	const bool bLetGo = Terrain.Run(Corner, 0.0, [&]() { return NearCount() == 0; });
	TestTrue(TEXT("with nothing wanted every chunk goes back to the far ground"), bLetGo);
	TestTrue(TEXT("clean again"), Stream.Audit().IsClean());
	Terrain.Actor->Destroy();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainHoldDecisionTest,
	"Mantle.Runtime.TerrainStream.Hold.Decision",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainHoldDecisionTest::RunTest(const FString& Parameters)
{
	using Hold = FMantleTerrainHold;
	using Change = FMantleTerrainHold::EChange;
	TestEqual(TEXT("the timeout is a minute"), Hold::TimeoutSeconds, 60.0);

	// The join: the hold is on from the start; it stays on while the ground is not built and ends when it stands.
	FMantleTerrainHoldState State{ true, false };
	TestTrue(TEXT("join, not built: nothing changes"), Hold::Next(State, false, 1.0) == Change::None);
	TestTrue(TEXT("still holding"), State.bHolding);
	TestTrue(TEXT("join, built: the hold ends"), Hold::Next(State, true, 3.8) == Change::End);
	TestFalse(TEXT("not holding"), State.bHolding);
	TestTrue(TEXT("standing and not holding: nothing"), Hold::Next(State, true, 0.0) == Change::None);

	// Mid-play: a teleport onto ground that is not built begins a hold; it ends when the ground stands.
	TestTrue(TEXT("a teleport onto unbuilt ground begins a hold"), Hold::Next(State, false, 0.0) == Change::Begin);
	TestTrue(TEXT("holding"), State.bHolding);
	TestTrue(TEXT("ten seconds in: nothing"), Hold::Next(State, false, 10.0) == Change::None);
	TestTrue(TEXT("exactly the timeout is not over it"), Hold::Next(State, false, 60.0) == Change::None);
	TestTrue(TEXT("built after 12 s: ends"), Hold::Next(State, true, 12.0) == Change::End);

	// The timeout: gives up, does not hold again while the ground is still not built, and is armed again once it stood.
	State = FMantleTerrainHoldState{ false, false };
	TestTrue(TEXT("begin"), Hold::Next(State, false, 0.0) == Change::Begin);
	TestTrue(TEXT("past the timeout: gives up"), Hold::Next(State, false, 60.5) == Change::GiveUp);
	TestFalse(TEXT("the player is let go"), State.bHolding);
	TestTrue(TEXT("and has given up"), State.bGaveUp);
	TestTrue(TEXT("still unbuilt: no new hold"), Hold::Next(State, false, 0.0) == Change::None);
	TestFalse(TEXT("not holding"), State.bHolding);
	TestTrue(TEXT("the ground stands once: nothing to end, the latch clears"), Hold::Next(State, true, 0.0) == Change::None);
	TestFalse(TEXT("armed again"), State.bGaveUp);
	TestTrue(TEXT("and the next unbuilt ground holds again"), Hold::Next(State, false, 0.0) == Change::Begin);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainStreamHoleChunkTest,
	"Mantle.Runtime.TerrainStream.Stream.ChunkWithNoFarMeshIsEmpty",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainStreamHoleChunkTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainStreamTestPrivate;
	FStreamWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	FStreamedTerrain Terrain;
	// Chunk 5 (the middle one, (1, 1)) has no far mesh: all hole.
	if (!TestTrue(TEXT("the terrain stands"), Terrain.Make(Fixture.World, 5)))
	{
		AddError(Terrain.Error);
		return false;
	}
	UMantleTerrainStreamComponent& Stream = *Terrain.Stream;
	TestTrue(TEXT("it is Empty from the start, not a chunk to build"), Stream.GetState(5) == EMantleTerrainChunkState::Empty);
	TestTrue(TEXT("its neighbour is an ordinary far chunk"), Stream.GetState(4) == EMantleTerrainChunkState::Far);
	const TArray<FVector> Middle{ FVector(19200.0, 19200.0, 0.0) };
	const auto NearCount = [&Stream]() { int32 N = 0; for (int32 I = 0; I < 16; ++I) { N += Stream.GetState(I) == EMantleTerrainChunkState::Near ? 1 : 0; } return N; };
	TestTrue(TEXT("the other eight chunks of the nine stand"), Terrain.Run(Middle, 13000.0, [&]() { return NearCount() == 8; }));
	TestEqual(TEXT("only eight were ever built: no build was spent on the hole"), Stream.GetStats().BuildsStarted, static_cast<int64>(8));
	TestTrue(TEXT("the hole does not hold the ground back"), Stream.AreaStands(Middle, 13000.0));
	Terrain.Actor->Destroy();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainStreamOneMeshTest,
	"Mantle.Runtime.TerrainStream.Stream.OneMeshAFrameAcrossTerrains",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainStreamOneMeshTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainStreamTestPrivate;
	// Two terrains side by side in tile terms (here at the same place, both wanting the nine chunks around the same view): between them they make at most one chunk mesh in a frame,
	// whichever ticks first, and both still get all nine.
	FStreamWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	FStreamedTerrain First;
	FStreamedTerrain Second;
	if (!TestTrue(TEXT("the first terrain stands"), First.Make(Fixture.World)) || !TestTrue(TEXT("and the second"), Second.Make(Fixture.World)))
	{
		return false;
	}
	const TArray<FVector> Middle{ FVector(19200.0, 19200.0, 0.0) };
	// A mesh made counts once in MadeMeshes (until it is swapped in) and once in SwapsToNear (after): their sum only goes up, by one for each mesh made.
	const auto MeshesMade = [](const FStreamedTerrain& Terrain) { return Terrain.Stream->GetStats().SwapsToNear + Terrain.Stream->GetMadeCount() + Terrain.Stream->GetStats().Discarded; };
	int64 MostInAFrame = 0;
	bool bBoth = false;
	for (int32 Step = 0; Step < 4000 && !bBoth; ++Step)
	{
		++GFrameCounter;
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		const int64 Before = MeshesMade(First) + MeshesMade(Second);
		First.Stream->StepStream(Middle, 13000.0);
		Second.Stream->StepStream(Middle, 13000.0);
		MostInAFrame = FMath::Max(MostInAFrame, MeshesMade(First) + MeshesMade(Second) - Before);
		bBoth = First.Stream->GetStats().SwapsToNear == 9 && Second.Stream->GetStats().SwapsToNear == 9;
		FPlatformProcess::Sleep(0.002f);
	}
	TestTrue(TEXT("both terrains got their nine chunks (neither starved)"), bBoth);
	TestEqual(TEXT("and no frame made more than one mesh between them"), MostInAFrame, static_cast<int64>(1));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainStreamJoinMeshesTest,
	"Mantle.Runtime.TerrainStream.Stream.JoinMakesSeveralMeshesAFrame",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainStreamJoinMeshesTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainStreamTestPrivate;
	// The join's hold (the overlay hides the frames) lets the terrains of a world make JoinMeshesPerFrame (4) meshes a frame between them: some frame makes more than one (the chunks are
	// prepared on workers, so how many are ready in a frame is up to them; four are, with eighteen chunks to make), and never more than four.
	FStreamWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	FStreamedTerrain First;
	FStreamedTerrain Second;
	if (!TestTrue(TEXT("the first terrain stands"), First.Make(Fixture.World)) || !TestTrue(TEXT("and the second"), Second.Make(Fixture.World)))
	{
		return false;
	}
	const TArray<FVector> Middle{ FVector(19200.0, 19200.0, 0.0) };
	const auto MeshesMade = [](const FStreamedTerrain& Terrain) { return Terrain.Stream->GetStats().SwapsToNear + Terrain.Stream->GetMadeCount() + Terrain.Stream->GetStats().Discarded; };
	int64 MostInAFrame = 0;
	bool bBoth = false;
	for (int32 Step = 0; Step < 4000 && !bBoth; ++Step)
	{
		++GFrameCounter;
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		const int64 Before = MeshesMade(First) + MeshesMade(Second);
		First.Stream->StepStream(Middle, 13000.0, UMantleTerrainStreamComponent::JoinMeshesPerFrame);
		Second.Stream->StepStream(Middle, 13000.0, UMantleTerrainStreamComponent::JoinMeshesPerFrame);
		MostInAFrame = FMath::Max(MostInAFrame, MeshesMade(First) + MeshesMade(Second) - Before);
		bBoth = First.Stream->GetStats().SwapsToNear == 9 && Second.Stream->GetStats().SwapsToNear == 9;
		FPlatformProcess::Sleep(0.002f);
	}
	TestTrue(TEXT("both terrains got their nine chunks"), bBoth);
	TestTrue(TEXT("no frame made more than four meshes between them"), MostInAFrame <= UMantleTerrainStreamComponent::JoinMeshesPerFrame);
	TestTrue(*FString::Printf(TEXT("and some frame made more than one (most in a frame %lld)"), MostInAFrame), MostInAFrame > 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainStreamMeshSlotTest,
	"Mantle.Runtime.TerrainStream.Plan.MeshSlot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainStreamMeshSlotTest::RunTest(const FString& Parameters)
{
	using Plan = FMantleTerrainStreamPlan;
	using FOther = TPair<double, int64>;
	const double None = TNumericLimits<double>::Max();
	// This terrain's nearest ready chunk is 5,000 cm from a view, its tie-break number is 10.
	TestTrue(TEXT("alone, the slot free: yes"), Plan::MayMakeMesh(true, 5000.0, 10, TArray<FOther>()));
	TestFalse(TEXT("the slot already used this frame: no"), Plan::MayMakeMesh(false, 5000.0, 10, TArray<FOther>()));
	TestFalse(TEXT("nothing ready here: no"), Plan::MayMakeMesh(true, None, 10, TArray<FOther>()));
	TestTrue(TEXT("another terrain's chunk is farther (9,000): yes"), Plan::MayMakeMesh(true, 5000.0, 10, TArray<FOther>{ FOther(9000.0, 3) }));
	TestTrue(TEXT("another has nothing ready: yes"), Plan::MayMakeMesh(true, 5000.0, 10, TArray<FOther>{ FOther(None, 3) }));
	TestFalse(TEXT("another terrain's chunk is nearer (4,999): no"), Plan::MayMakeMesh(true, 5000.0, 10, TArray<FOther>{ FOther(4999.0, 99) }));
	TestFalse(TEXT("equally near, the other's number is lower (3): no"), Plan::MayMakeMesh(true, 5000.0, 10, TArray<FOther>{ FOther(5000.0, 3) }));
	TestTrue(TEXT("equally near, the other's number is higher (11): yes"), Plan::MayMakeMesh(true, 5000.0, 10, TArray<FOther>{ FOther(5000.0, 11) }));
	TestFalse(TEXT("the nearest of three decides: one at 800 beats this one at 5,000"),
		Plan::MayMakeMesh(true, 5000.0, 10, TArray<FOther>{ FOther(9000.0, 1), FOther(800.0, 50), FOther(6000.0, 2) }));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
