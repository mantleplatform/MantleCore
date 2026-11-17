// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCellAssetUse.h"
#include "MantleFarSetPlan.h"
#include "MantlePlaceBuilder.h"
#include "MantlePlaceParser.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"

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
	return true;
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

	// A drawn deck with no collision over a collision-only slab (the City Sample freeway), a collision-only set of blockers, and a drawn cube with a material for contrast.
	const FMantleParseResult Parsed = ParseEntities({
		EntityJson(TEXT("ent_deck"), TEXT("StaticMesh"), TEXT("0,0,100"), TEXT("\"mesh\":\"ast_cube\",\"collision\":\"none\"")),
		EntityJson(TEXT("ent_slab"), TEXT("StaticMesh"), TEXT("0,0,0"), TEXT("\"mesh\":\"ast_cube\",\"materials\":[\"ast_mat_basic\"],\"castShadow\":true,\"visible\":false")),
		EntityJson(TEXT("ent_blockers"), TEXT("InstancedMesh"), TEXT("1000,0,0"),
			TEXT("\"mesh\":\"ast_cube\",\"collision\":\"blocking\",\"visible\":false,\"drawDistance\":20000,\"instances\":[[0,0,0,0,0,0,1,1,1],[200,0,0,0,0,0,1,1,1]]")),
		EntityJson(TEXT("ent_shown"), TEXT("StaticMesh"), TEXT("2000,0,0"), TEXT("\"mesh\":\"ast_cube\",\"materials\":[\"ast_mat_basic\"]")),
	});
	if (!TestTrue(TEXT("the place parses"), Parsed.bSuccess))
	{
		AddError(Parsed.ErrorSummary());
		return false;
	}
	FMantleBuildOptions Options;
	Options.bRuntimeEnvironment = false;
	FMantleBuildResult Built = FMantlePlaceBuilder::Build(Fixture.World, Parsed.Place, Options);
	TestTrue(TEXT("the build succeeds"), Built.bSuccess);

	const auto MeshComponentOf = [&Built](const TCHAR* Id) -> UStaticMeshComponent*
	{
		const AActor* Actor = Built.Actors.FindRef(Id);
		return Actor ? Actor->FindComponentByClass<UStaticMeshComponent>() : nullptr;
	};

	UStaticMeshComponent* Slab = MeshComponentOf(TEXT("ent_slab"));
	UStaticMeshComponent* Blockers = MeshComponentOf(TEXT("ent_blockers"));
	UStaticMeshComponent* Shown = MeshComponentOf(TEXT("ent_shown"));
	UStaticMeshComponent* Deck = MeshComponentOf(TEXT("ent_deck"));
	if (!TestNotNull(TEXT("the slab"), Slab) || !TestNotNull(TEXT("the blockers"), Blockers) || !TestNotNull(TEXT("the shown cube"), Shown) || !TestNotNull(TEXT("the deck"), Deck))
	{
		FMantlePlaceBuilder::Teardown(Built);
		return false;
	}

	for (const TPair<const TCHAR*, UStaticMeshComponent*>& Pair : { TPair<const TCHAR*, UStaticMeshComponent*>(TEXT("slab"), Slab), TPair<const TCHAR*, UStaticMeshComponent*>(TEXT("blockers"), Blockers) })
	{
		const FString What = Pair.Key;
		UStaticMeshComponent* Component = Pair.Value;
		// Solid like any blocking mesh.
		TestEqual(What + TEXT(" collides with queries and physics"), static_cast<int32>(Component->GetCollisionEnabled()), static_cast<int32>(ECollisionEnabled::QueryAndPhysics));
		TestEqual(What + TEXT(" blocks pawns"), static_cast<int32>(Component->GetCollisionResponseToChannel(ECC_Pawn)), static_cast<int32>(ECR_Block));
		TestNotNull(*(What + TEXT(" has its mesh")), Component->GetStaticMesh().Get());
		// Never drawn: hidden in game (still shown in an editor the place is imported into), and nothing that would add it to the scene while hidden.
		TestTrue(What + TEXT(" is hidden in game"), Component->bHiddenInGame);
		TestFalse(What + TEXT(" casts no shadow"), Component->CastShadow);
		TestFalse(What + TEXT(" casts no hidden shadow"), static_cast<bool>(Component->bCastHiddenShadow));
		TestFalse(What + TEXT(" lights nothing while hidden"), static_cast<bool>(Component->bAffectIndirectLightingWhileHidden));
		TestFalse(What + TEXT(" has no distance field lighting"), static_cast<bool>(Component->bAffectDistanceFieldLighting));
	}
	const UInstancedStaticMeshComponent* Instances = Cast<UInstancedStaticMeshComponent>(Blockers);
	TestEqual(TEXT("the blockers keep both instances"), Instances ? Instances->GetInstanceCount() : 0, 2);

	// The slab's listed material is not loaded (nothing draws it); the shown cube's is.
	TestNotNull(TEXT("the shown cube has a material"), Shown->GetMaterial(0));
	TestTrue(TEXT("the slab keeps the mesh's own material"), Slab->GetMaterial(0) == Slab->GetStaticMesh()->GetMaterial(0));
	TestTrue(TEXT("the shown cube's material is not the mesh's own"), Shown->GetMaterial(0) != Shown->GetStaticMesh()->GetMaterial(0));

	// The drawn ones are drawn as before.
	TestFalse(TEXT("the shown cube is not hidden"), Shown->bHiddenInGame);
	TestTrue(TEXT("the shown cube casts a shadow"), Shown->CastShadow);
	TestFalse(TEXT("the deck is not hidden"), Deck->bHiddenInGame);
	TestEqual(TEXT("the deck has no collision"), static_cast<int32>(Deck->GetCollisionEnabled()), static_cast<int32>(ECollisionEnabled::NoCollision));

	FMantlePlaceBuilder::Teardown(Built);
	return true;
}

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
