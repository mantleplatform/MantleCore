// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCellAssetUse.h"
#include "MantleFarSetPlan.h"
#include "MantlePlaceBuilder.h"
#include "MantlePlaceParser.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/DemoNetDriver.h"
#include "Engine/Engine.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// Collision-only meshes (visible false on a StaticMesh or an InstancedMesh): what the parser reads and refuses, that the builder
// makes them solid and never drawn, that a far set makes no twin of them, and that a cell fetches none of their materials.
// That one is solid and unseen in a played place is the local in-game test's.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleCollisionOnlyMeshTestPrivate
{
	FString EntityJson(const TCHAR* Id, const TCHAR* Type, const TCHAR* Location, const FString& Properties)
	{
		return FString::Printf(
			TEXT("{\"id\":\"%s\",\"type\":\"%s\",\"parent\":null,\"transform\":{\"location\":[%s],\"rotation\":[0,0,0],\"scale\":[1,1,1]},\"properties\":{%s}}"),
			Id, Type, Location, *Properties);
	}

	FMantleParseResult ParseEntities(const TArray<FString>& Entities)
	{
		const FString Json = FString::Printf(
			TEXT("{\"schemaVersion\":1,\"placeId\":\"plc_collide01\",\"revision\":{\"number\":1,\"state\":\"draft\"},"
				 "\"metadata\":{\"name\":\"Invisible Floor\",\"authorId\":\"usr_4b1d\"},\"settings\":{},"
				 "\"assets\":[{\"id\":\"ast_cube\",\"kind\":\"StaticMesh\",\"hash\":\"md5:0123456789abcdef\",\"bytes\":1024}],"
				 "\"entities\":[%s]}"),
			*FString::Join(Entities, TEXT(",")));
		return FMantlePlaceParser::ParseFromString(Json);
	}

	FMantleParseResult ParseStatic(const FString& Extra)
	{
		return ParseEntities({ EntityJson(TEXT("ent_mesh"), TEXT("StaticMesh"), TEXT("0,0,0"), TEXT("\"mesh\":\"ast_cube\"") + Extra) });
	}

	FMantleParseResult ParseInstanced(const FString& Extra)
	{
		return ParseEntities({ EntityJson(TEXT("ent_mesh"), TEXT("InstancedMesh"), TEXT("0,0,0"), TEXT("\"mesh\":\"ast_cube\",\"instances\":[[0,0,0,0,0,0,1,1,1]]") + Extra) });
	}

	const FMantleStaticMeshProps* MeshOf(const FMantleParseResult& Result)
	{
		return Result.Place.Entities.Num() == 1 ? MantleMeshPropsOf(Result.Place.Entities[0]) : nullptr;
	}

	bool HasErrorAt(const FMantleParseResult& Result, const FString& Path)
	{
		return Result.Errors.ContainsByPredicate([&Path](const FMantleParseError& Error) { return Error.Path == Path; });
	}

	const TCHAR* const VisiblePath = TEXT("/entities/0/properties/visible");

	struct FWorldHolder
	{
		UWorld* World = nullptr;
		FWorldHolder() { World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, FName(*FString::Printf(TEXT("MantleCollisionOnlyTestWorld_%s"), *FGuid::NewGuid().ToString()))); }
		~FWorldHolder()
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
	FMantleCollisionOnlyParseTest,
	"Mantle.Runtime.CollisionOnly.Parser.ReadsAndRefuses",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCollisionOnlyParseTest::RunTest(const FString& Parameters)
{
	using namespace MantleCollisionOnlyMeshTestPrivate;

	// Absent and true: drawn, as before the key. False: collision only, on both types, with any collision but none.
	const auto Check = [this](const FString& What, const FMantleParseResult& Result, const bool bWantVisible)
	{
		if (!TestTrue(What + TEXT(" parses"), Result.bSuccess))
		{
			AddError(Result.ErrorSummary());
			return;
		}
		if (const FMantleStaticMeshProps* Mesh = MeshOf(Result))
		{
			TestEqual(What + TEXT(": visible"), Mesh->bVisible, bWantVisible);
		}
	};
	Check(TEXT("a StaticMesh without the key"), ParseStatic(TEXT("")), true);
	Check(TEXT("an InstancedMesh without the key"), ParseInstanced(TEXT("")), true);
	Check(TEXT("a StaticMesh with visible true"), ParseStatic(TEXT(",\"visible\":true")), true);
	Check(TEXT("a collision-only StaticMesh"), ParseStatic(TEXT(",\"visible\":false")), false);
	Check(TEXT("a collision-only overlapping StaticMesh"), ParseStatic(TEXT(",\"visible\":false,\"collision\":\"overlap\",\"surface\":\"concrete\"")), false);
	Check(TEXT("a collision-only InstancedMesh"), ParseInstanced(TEXT(",\"visible\":false,\"collision\":\"blocking\",\"drawDistance\":20000")), false);

	// Refused at the key: not a boolean, never drawn and never colliding (nothing at all), and a physics prop (always drawn).
	for (const TCHAR* Bad : { TEXT(",\"visible\":\"false\""), TEXT(",\"visible\":0"), TEXT(",\"visible\":null"), TEXT(",\"visible\":false,\"collision\":\"none\"") })
	{
		const FMantleParseResult StaticBad = ParseStatic(Bad);
		TestFalse(FString::Printf(TEXT("StaticMesh %s is refused"), Bad), StaticBad.bSuccess);
		TestTrue(FString::Printf(TEXT("StaticMesh %s is reported at visible"), Bad), HasErrorAt(StaticBad, VisiblePath));
		const FMantleParseResult InstancedBad = ParseInstanced(Bad);
		TestFalse(FString::Printf(TEXT("InstancedMesh %s is refused"), Bad), InstancedBad.bSuccess);
		TestTrue(FString::Printf(TEXT("InstancedMesh %s is reported at visible"), Bad), HasErrorAt(InstancedBad, VisiblePath));
	}
	const FMantleParseResult Prop = ParseStatic(TEXT(",\"visible\":false,\"physics\":{\"mass\":20}"));
	TestFalse(TEXT("a collision-only physics prop is refused"), Prop.bSuccess);
	TestTrue(TEXT("at visible"), HasErrorAt(Prop, VisiblePath));

	// blocksVisibility: an invisible wall that lets sight traces through. Absent, it blocks them; only a collision-only mesh may say so.
	const FMantleParseResult Wall = ParseStatic(TEXT(",\"visible\":false,\"blocksVisibility\":false"));
	TestTrue(TEXT("an invisible wall parses"), Wall.bSuccess);
	if (const FMantleStaticMeshProps* Mesh = MeshOf(Wall))
	{
		TestFalse(TEXT("and lets sight traces through"), Mesh->bBlocksVisibility);
	}
	const FMantleParseResult InstancedWall = ParseInstanced(TEXT(",\"visible\":false,\"blocksVisibility\":false"));
	TestTrue(TEXT("so do invisible instanced walls"), InstancedWall.bSuccess && MeshOf(InstancedWall) && !MeshOf(InstancedWall)->bBlocksVisibility);
	if (const FMantleStaticMeshProps* Mesh = MeshOf(ParseStatic(TEXT(",\"visible\":false"))))
	{
		TestTrue(TEXT("absent, a collision-only mesh blocks sight traces"), Mesh->bBlocksVisibility);
	}
	const TCHAR* const BlocksPath = TEXT("/entities/0/properties/blocksVisibility");
	for (const TCHAR* Bad : { TEXT(",\"blocksVisibility\":false"), TEXT(",\"visible\":true,\"blocksVisibility\":false"), TEXT(",\"visible\":false,\"blocksVisibility\":\"no\"") })
	{
		const FMantleParseResult Refused = ParseStatic(Bad);
		TestFalse(FString::Printf(TEXT("%s is refused"), Bad), Refused.bSuccess);
		TestTrue(FString::Printf(TEXT("%s is reported at blocksVisibility"), Bad), HasErrorAt(Refused, BlocksPath));
	}
	return true;
}

namespace MantleCollisionOnlyMeshTestPrivate
{
	/**
	 * Builds a drawn deck with no collision over a collision-only slab (the City Sample freeway), a collision-only set of blockers, an
	 * invisible wall that lets sight traces through, and a drawn cube with a material for contrast, in World; checks each is solid and
	 * never drawn, or drawn as before.
	 */
	void BuildAndCheck(FAutomationTestBase& Test, UWorld* World, const FString& Where)
	{
		const FMantleParseResult Parsed = ParseEntities({
			EntityJson(TEXT("ent_deck"), TEXT("StaticMesh"), TEXT("0,0,100"), TEXT("\"mesh\":\"ast_cube\",\"collision\":\"none\"")),
			EntityJson(TEXT("ent_slab"), TEXT("StaticMesh"), TEXT("0,0,0"), TEXT("\"mesh\":\"ast_cube\",\"materials\":[\"ast_mat_basic\"],\"castShadow\":true,\"visible\":false")),
			EntityJson(TEXT("ent_blockers"), TEXT("InstancedMesh"), TEXT("1000,0,0"),
				TEXT("\"mesh\":\"ast_cube\",\"collision\":\"blocking\",\"visible\":false,\"drawDistance\":20000,\"instances\":[[0,0,0,0,0,0,1,1,1],[200,0,0,0,0,0,1,1,1]]")),
			EntityJson(TEXT("ent_wall"), TEXT("StaticMesh"), TEXT("0,1000,0"), TEXT("\"mesh\":\"ast_cube\",\"visible\":false,\"blocksVisibility\":false")),
			EntityJson(TEXT("ent_shown"), TEXT("StaticMesh"), TEXT("2000,0,0"), TEXT("\"mesh\":\"ast_cube\",\"materials\":[\"ast_mat_basic\"]")),
		});
		if (!Test.TestTrue(Where + TEXT(": the place parses"), Parsed.bSuccess))
		{
			Test.AddError(Parsed.ErrorSummary());
			return;
		}
		FMantleBuildOptions Options;
		Options.bRuntimeEnvironment = false;
		FMantleBuildResult Built = FMantlePlaceBuilder::Build(World, Parsed.Place, Options);
		Test.TestTrue(Where + TEXT(": the build succeeds"), Built.bSuccess);

		const auto MeshComponentOf = [&Built](const TCHAR* Id) -> UStaticMeshComponent*
		{
			const AActor* Actor = Built.Actors.FindRef(Id);
			return Actor ? Actor->FindComponentByClass<UStaticMeshComponent>() : nullptr;
		};
		UStaticMeshComponent* Slab = MeshComponentOf(TEXT("ent_slab"));
		UStaticMeshComponent* Blockers = MeshComponentOf(TEXT("ent_blockers"));
		UStaticMeshComponent* Wall = MeshComponentOf(TEXT("ent_wall"));
		UStaticMeshComponent* Shown = MeshComponentOf(TEXT("ent_shown"));
		UStaticMeshComponent* Deck = MeshComponentOf(TEXT("ent_deck"));
		if (!Slab || !Blockers || !Wall || !Shown || !Deck)
		{
			Test.AddError(Where + TEXT(": an entity was not built"));
			FMantlePlaceBuilder::Teardown(Built);
			return;
		}

		for (const TPair<const TCHAR*, UStaticMeshComponent*>& Pair : {
			TPair<const TCHAR*, UStaticMeshComponent*>(TEXT("slab"), Slab), TPair<const TCHAR*, UStaticMeshComponent*>(TEXT("blockers"), Blockers),
			TPair<const TCHAR*, UStaticMeshComponent*>(TEXT("wall"), Wall) })
		{
			const FString What = Where + TEXT(": the ") + Pair.Key;
			UStaticMeshComponent* Component = Pair.Value;
			// Solid like any blocking mesh.
			Test.TestEqual(What + TEXT(" collides with queries and physics"), static_cast<int32>(Component->GetCollisionEnabled()), static_cast<int32>(ECollisionEnabled::QueryAndPhysics));
			Test.TestEqual(What + TEXT(" blocks pawns"), static_cast<int32>(Component->GetCollisionResponseToChannel(ECC_Pawn)), static_cast<int32>(ECR_Block));
			Test.TestNotNull(*(What + TEXT(" has its mesh")), Component->GetStaticMesh().Get());
			// Never drawn: hidden in game (still shown in an editor the place is imported into), nothing that would add it to the scene while hidden,
			// and so no scene proxy at all.
			Test.TestTrue(What + TEXT(" is hidden in game"), Component->bHiddenInGame);
			Test.TestFalse(What + TEXT(" casts no shadow"), Component->CastShadow);
			Test.TestFalse(What + TEXT(" casts no hidden shadow"), static_cast<bool>(Component->bCastHiddenShadow));
			Test.TestFalse(What + TEXT(" lights nothing while hidden"), static_cast<bool>(Component->bAffectIndirectLightingWhileHidden));
			Test.TestFalse(What + TEXT(" has no distance field lighting"), static_cast<bool>(Component->bAffectDistanceFieldLighting));
			Test.TestFalse(What + TEXT(" is not a far ray tracing primitive"), static_cast<bool>(Component->bRayTracingFarField));
			Test.TestTrue(What + TEXT(" is registered"), Component->IsRegistered());
			Test.TestNull(*(What + TEXT(" has no scene proxy")), Component->SceneProxy);
		}
		// The slab and blockers block every trace; the invisible wall lets the Visibility channel (sight, pointer, interaction) through.
		Test.TestEqual(Where + TEXT(": the slab blocks sight traces"), static_cast<int32>(Slab->GetCollisionResponseToChannel(ECC_Visibility)), static_cast<int32>(ECR_Block));
		Test.TestEqual(Where + TEXT(": the wall lets sight traces through"), static_cast<int32>(Wall->GetCollisionResponseToChannel(ECC_Visibility)), static_cast<int32>(ECR_Ignore));
		Test.TestEqual(Where + TEXT(": and still blocks the camera"), static_cast<int32>(Wall->GetCollisionResponseToChannel(ECC_Camera)), static_cast<int32>(ECR_Block));
		const UInstancedStaticMeshComponent* Instances = Cast<UInstancedStaticMeshComponent>(Blockers);
		Test.TestEqual(Where + TEXT(": the blockers keep both instances"), Instances ? Instances->GetInstanceCount() : 0, 2);

		// Real traces through the built world. The wall stands at y = 1000 (a 100 cm cube): a sight trace along +Y passes it, a pawn-channel
		// trace stops at it, and a sight trace down hits the slab (at the origin; the deck above it has no collision).
		// No frame is needed first: a body made at registration goes into the scene's query structure at once (FChaosScene::AddActorsToScene_AssumesLocked).
		FHitResult Hit;
		const FCollisionQueryParams Query(SCENE_QUERY_STAT(MantleCollisionOnlyTest), /*bTraceComplex*/ false);
		const FVector WallFrom(0.0, 800.0, 0.0);
		const FVector WallTo(0.0, 1200.0, 0.0);
		const bool bSightHit = World->LineTraceSingleByChannel(Hit, WallFrom, WallTo, ECC_Visibility, Query);
		Test.TestFalse(Where + TEXT(": a sight trace passes the invisible wall"), bSightHit && Hit.GetComponent() == Wall);
		const bool bPawnHit = World->LineTraceSingleByChannel(Hit, WallFrom, WallTo, ECC_Pawn, Query);
		Test.TestTrue(Where + TEXT(": a pawn-channel trace stops at it"), bPawnHit && Hit.GetComponent() == Wall);
		const bool bDownHit = World->LineTraceSingleByChannel(Hit, FVector(0.0, 0.0, 400.0), FVector(0.0, 0.0, -400.0), ECC_Visibility, Query);
		Test.TestTrue(Where + TEXT(": a trace down passes the deck and stops on the slab"), bDownHit && Hit.GetComponent() == Slab);

		// The slab's listed material is not loaded (nothing draws it); the shown cube's is.
		Test.TestNotNull(*(Where + TEXT(": the shown cube has a material")), Shown->GetMaterial(0));
		Test.TestTrue(Where + TEXT(": the slab keeps the mesh's own material"), Slab->GetMaterial(0) == Slab->GetStaticMesh()->GetMaterial(0));
		Test.TestTrue(Where + TEXT(": the shown cube's material is not the mesh's own"), Shown->GetMaterial(0) != Shown->GetStaticMesh()->GetMaterial(0));

		// The drawn ones are drawn as before. The shown cube has a scene proxy, so "no scene proxy" above is not a world that makes none.
		if (World->Scene)
		{
			Test.TestNotNull(*(Where + TEXT(": the shown cube has a scene proxy")), Shown->SceneProxy);
		}
		else
		{
			Test.AddError(Where + TEXT(": the world has no scene, so no scene proxy proves nothing"));
		}
		Test.TestFalse(Where + TEXT(": the shown cube is not hidden"), Shown->bHiddenInGame);
		Test.TestTrue(Where + TEXT(": the shown cube casts a shadow"), Shown->CastShadow);
		Test.TestFalse(Where + TEXT(": the deck is not hidden"), Deck->bHiddenInGame);
		Test.TestEqual(Where + TEXT(": the deck has no collision"), static_cast<int32>(Deck->GetCollisionEnabled()), static_cast<int32>(ECollisionEnabled::NoCollision));

		FMantlePlaceBuilder::Teardown(Built);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCollisionOnlyBuildTest,
	"Mantle.Runtime.CollisionOnly.Builder.SolidAndNeverDrawn",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCollisionOnlyBuildTest::RunTest(const FString& Parameters)
{
	using namespace MantleCollisionOnlyMeshTestPrivate;
	FWorldHolder Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	BuildAndCheck(*this, Fixture.World, TEXT("a game"));
	return true;
}

#if WITH_EDITOR

// The same place built on a dedicated server: it keeps every collision-only body (players stand on them there), and still draws nothing.
// The world is made a server the way MantleSoundEmitterMarkerTest's is (a game world with a server net driver, run as dedicated).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCollisionOnlyServerTest,
	"Mantle.Runtime.CollisionOnly.Builder.ServerKeepsTheCollision",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCollisionOnlyServerTest::RunTest(const FString& Parameters)
{
	using namespace MantleCollisionOnlyMeshTestPrivate;
	if (!TestNotNull(TEXT("an engine"), GEngine))
	{
		return false;
	}
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("MantleCollisionOnlyServerTestWorld"));
	if (!TestNotNull(TEXT("a world"), World))
	{
		return false;
	}
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	Context.RunAsDedicated = true;
	UDemoNetDriver* Driver = NewObject<UDemoNetDriver>(GetTransientPackage());
	Driver->SetWorld(World);
	World->SetNetDriver(Driver);
	if (TestEqual(TEXT("the test world is a dedicated server"), static_cast<int32>(World->GetNetMode()), static_cast<int32>(NM_DedicatedServer)))
	{
		BuildAndCheck(*this, World, TEXT("a dedicated server"));
	}
	World->SetNetDriver(nullptr);
	Driver->SetWorld(nullptr);
	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(/*bInformEngineOfWorld*/ false);
	return true;
}

#endif // WITH_EDITOR

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCollisionOnlyStreamTest,
	"Mantle.Runtime.CollisionOnly.Cells.NoTwinNoMaterials",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCollisionOnlyStreamTest::RunTest(const FString& Parameters)
{
	const FString MeshId = FString::Printf(TEXT("ast_u%032x"), 0xa);
	const FString MaterialId = FString::Printf(TEXT("ast_u%032x"), 0x101);

	FMantleEntity Hidden;
	Hidden.Id = TEXT("ent_ism_collision");
	Hidden.Type = EMantleEntityType::InstancedMesh;
	FMantleInstancedMeshProps Props;
	Props.Mesh.Mesh = MeshId;
	Props.Mesh.Materials = { MaterialId };
	Props.Mesh.Collision = EMantleCollision::Blocking;
	Props.Mesh.DrawDistance = 90000.0;
	Props.Mesh.bVisible = false;
	Props.Instances.Add(FTransform(FVector(0, 0, 0)));
	Props.Instances.Add(FTransform(FVector(100, 0, 0)));
	Hidden.Props.Emplace<FMantleInstancedMeshProps>(Props);

	// A far set draws a cell from afar: a mesh never drawn has no twin, while the same mesh drawn has one.
	FMantleFarSetSettings Settings;
	Settings.bEnabled = true;
	FMantleEntity Twin;
	TestFalse(TEXT("no far twin of a collision-only mesh"), FMantleFarSetPlan::MakeTwin(Hidden, 120000.0, Settings, Twin));
	FMantleEntity Shown = Hidden;
	Shown.Props.Get<FMantleInstancedMeshProps>().Mesh.bVisible = true;
	TestTrue(TEXT("the same mesh drawn has one"), FMantleFarSetPlan::MakeTwin(Shown, 120000.0, Settings, Twin));

	// A cell holding it fetches its mesh for collision and none of its materials.
	TArray<FMantleCellAssetNeed> Needs;
	FMantleCellAssetUse::NeedsOf({ Hidden }, Needs);
	if (TestEqual(TEXT("one asset: the mesh"), Needs.Num(), 1))
	{
		TestEqual(TEXT("the mesh"), Needs[0].Id, MeshId);
		TestFalse(TEXT("is not a material"), Needs[0].bMaterial);
		TestTrue(TEXT("and needs collision"), Needs[0].bCollision);
	}
	FMantleCellAssetUse::NeedsOf({ Shown }, Needs);
	TestEqual(TEXT("drawn, it needs its material too"), Needs.Num(), 2);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
