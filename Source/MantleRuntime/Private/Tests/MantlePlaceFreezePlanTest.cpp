// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceBuilder.h"
#include "MantlePlaceFreezePlan.h"
#include "MantlePlaceParser.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// The freeze plan (which entities end Static, and where each stands) and the staged build that uses it: every entity that ends Static is created where it stands
// and Static, so its physics body is made once, static, in place, and no registered component of it is ever Movable or away from its final transform.

namespace MantlePlaceFreezePlanTestPrivate
{
	/**
	 * Group g1 at x = 1000, scale 2, holds a blocking cube at relative x = 100 (world x = 1000 + 2 * 100 = 1200, scale 2) and Group g2 (relative y = 500, so world
	 * y = 0 + 2 * 500 = 1000, scale 2) that holds a blocking InstancedMesh at relative x = 10 (world x = 1000 + 2 * 10 = 1020, y = 1000). A blocking cube at x = 5000 stands
	 * alone. Things that stay Movable: a dynamic blocking cube, a cube with no collision, a Text, a blocking cube under a dynamic Group, a blocking cube under a Text, and
	 * a Group that holds only a dynamic cube.
	 */
	FMantleParseResult ParsePlace()
	{
		auto Entity = [](const TCHAR* Id, const TCHAR* Type, const TCHAR* Parent, const TCHAR* Location, const TCHAR* Scale, const TCHAR* Properties, const TCHAR* Extra = TEXT(""))
		{
			return FString::Printf(
				TEXT("{\"id\":\"%s\",\"type\":\"%s\",\"parent\":%s,\"transform\":{\"location\":[%s],\"rotation\":[0,0,0],\"scale\":[%s]},\"properties\":{%s}%s}"),
				Id, Type, Parent, Location, Scale, Properties, Extra);
		};
		const TCHAR* Blocking = TEXT("\"mesh\":\"ast_cube\",\"collision\":\"blocking\"");
		const TCHAR* None = TEXT("\"mesh\":\"ast_cube\",\"collision\":\"none\"");
		TArray<FString> Entities;
		Entities.Add(Entity(TEXT("ent_g1"), TEXT("Group"), TEXT("null"), TEXT("1000,0,0"), TEXT("2,2,2"), TEXT("")));
		Entities.Add(Entity(TEXT("ent_g1_cube"), TEXT("StaticMesh"), TEXT("\"ent_g1\""), TEXT("100,0,0"), TEXT("1,1,1"), Blocking));
		Entities.Add(Entity(TEXT("ent_g2"), TEXT("Group"), TEXT("\"ent_g1\""), TEXT("0,500,0"), TEXT("1,1,1"), TEXT("")));
		Entities.Add(Entity(TEXT("ent_g2_ism"), TEXT("InstancedMesh"), TEXT("\"ent_g2\""), TEXT("10,0,0"), TEXT("1,1,1"),
			TEXT("\"mesh\":\"ast_cube\",\"collision\":\"blocking\",\"instances\":[[0,0,0,0,0,0,1,1,1],[100,0,0,0,0,0,1,1,1]]")));
		Entities.Add(Entity(TEXT("ent_loose"), TEXT("StaticMesh"), TEXT("null"), TEXT("5000,0,0"), TEXT("1,1,1"), Blocking));
		Entities.Add(Entity(TEXT("ent_dyn"), TEXT("StaticMesh"), TEXT("null"), TEXT("6000,0,0"), TEXT("1,1,1"), Blocking, TEXT(",\"dynamic\":true")));
		Entities.Add(Entity(TEXT("ent_none"), TEXT("StaticMesh"), TEXT("null"), TEXT("7000,0,0"), TEXT("1,1,1"), None));
		Entities.Add(Entity(TEXT("ent_text"), TEXT("Text"), TEXT("null"), TEXT("8000,0,0"), TEXT("1,1,1"), TEXT("\"text\":\"t\"")));
		Entities.Add(Entity(TEXT("ent_under_text"), TEXT("StaticMesh"), TEXT("\"ent_text\""), TEXT("10,0,0"), TEXT("1,1,1"), Blocking));
		Entities.Add(Entity(TEXT("ent_gdyn"), TEXT("Group"), TEXT("null"), TEXT("9000,0,0"), TEXT("1,1,1"), TEXT(""), TEXT(",\"dynamic\":true")));
		Entities.Add(Entity(TEXT("ent_under_gdyn"), TEXT("StaticMesh"), TEXT("\"ent_gdyn\""), TEXT("10,0,0"), TEXT("1,1,1"), Blocking));
		Entities.Add(Entity(TEXT("ent_g3"), TEXT("Group"), TEXT("null"), TEXT("9500,0,0"), TEXT("1,1,1"), TEXT("")));
		Entities.Add(Entity(TEXT("ent_g3_dyn"), TEXT("StaticMesh"), TEXT("\"ent_g3\""), TEXT("10,0,0"), TEXT("1,1,1"), Blocking, TEXT(",\"dynamic\":true")));
		// Entities whose builders add their own root (a bare actor drops the spawn transform): a dynamic blocking InstancedMesh (a body for every instance), and a box and a
		// sphere trigger.
		Entities.Add(Entity(TEXT("ent_ism_dyn"), TEXT("InstancedMesh"), TEXT("null"), TEXT("13000,0,0"), TEXT("1,1,1"),
			TEXT("\"mesh\":\"ast_cube\",\"collision\":\"blocking\",\"instances\":[[0,0,0,0,0,0,1,1,1],[100,0,0,0,0,0,1,1,1]]"), TEXT(",\"dynamic\":true")));
		Entities.Add(Entity(TEXT("ent_trigger_box"), TEXT("TriggerVolume"), TEXT("null"), TEXT("14000,0,0"), TEXT("1,1,1"), TEXT("\"shape\":\"box\",\"extent\":[100,100,100]")));
		Entities.Add(Entity(TEXT("ent_trigger_sphere"), TEXT("TriggerVolume"), TEXT("\"ent_g3\""), TEXT("20,0,0"), TEXT("1,1,1"), TEXT("\"shape\":\"sphere\",\"extent\":[50,50,50]")));
		const FString Json = FString::Printf(
			TEXT("{\"schemaVersion\":1,\"placeId\":\"plc_freezeplan1\",\"revision\":{\"number\":1,\"state\":\"draft\"},"
				 "\"metadata\":{\"name\":\"Plan\",\"authorId\":\"usr_4b1d\"},\"settings\":{},\"assets\":[],\"entities\":[%s]}"),
			*FString::Join(Entities, TEXT(",")));
		return FMantlePlaceParser::ParseFromString(Json);
	}

	/** A world-space location of a transform, from its parts (the plan's answer). */
	bool Near(const FVector& A, const FVector& B) { return A.Equals(B, 0.01); }

	struct FWorldHolder
	{
		UWorld* World = nullptr;
		FWorldHolder() { World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, FName(*FString::Printf(TEXT("MantleFreezePlanTestWorld_%s"), *FGuid::NewGuid().ToString()))); }
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
	FMantleFreezePlanRuleTest,
	"Mantle.Runtime.PlaceBuild.FreezePlan.Rule",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleFreezePlanRuleTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceFreezePlanTestPrivate;
	const FMantleParseResult Parsed = ParsePlace();
	if (!TestTrue(TEXT("the place parses"), Parsed.bSuccess))
	{
		AddError(Parsed.ErrorSummary());
		return false;
	}
	const FMantlePlaceFreezePlan Plan = FMantlePlaceFreezePlan::Make(Parsed.Place);

	// What ends Static: place geometry that blocks and is not dynamic, with every ancestor a Group that is not dynamic, and those Groups.
	for (const TCHAR* Id : { TEXT("ent_g1"), TEXT("ent_g1_cube"), TEXT("ent_g2"), TEXT("ent_g2_ism"), TEXT("ent_loose") })
	{
		TestTrue(*FString::Printf(TEXT("%s ends Static"), Id), Plan.IsStatic(Id));
	}
	// What stays Movable.
	for (const TCHAR* Id : { TEXT("ent_dyn"), TEXT("ent_none"), TEXT("ent_text"), TEXT("ent_under_text"), TEXT("ent_gdyn"), TEXT("ent_under_gdyn"), TEXT("ent_g3"), TEXT("ent_g3_dyn"),
		TEXT("ent_ism_dyn"), TEXT("ent_trigger_box"), TEXT("ent_trigger_sphere") })
	{
		TestFalse(*FString::Printf(TEXT("%s stays Movable"), Id), Plan.IsStatic(Id));
	}
	TestEqual(TEXT("exactly five entities end Static"), Plan.StaticWorld.Num(), 5);
	TestEqual(TEXT("three are geometry"), Plan.GeometryIds.Num(), 3);
	TestEqual(TEXT("two are Groups"), Plan.GroupIds.Num(), 2);
	TestTrue(TEXT("the outer Group is listed before the one inside it"), Plan.GroupIds.IndexOfByKey(TEXT("ent_g1")) < Plan.GroupIds.IndexOfByKey(TEXT("ent_g2")));

	// Where each stands, worked out by hand: g1 (1000, 0, 0) at scale 2; the cube at relative 100 is at 1000 + 2 * 100 = 1200, scale 2; g2 at relative y 500 is at
	// y = 2 * 500 = 1000 (x 1000), scale 2; the instanced mesh at relative x 10 is at x = 1000 + 2 * 10 = 1020, y = 1000, scale 2; the loose cube is at 5000, scale 1.
	const FTransform* G1 = Plan.WorldOf(TEXT("ent_g1"));
	const FTransform* Cube = Plan.WorldOf(TEXT("ent_g1_cube"));
	const FTransform* G2 = Plan.WorldOf(TEXT("ent_g2"));
	const FTransform* Ism = Plan.WorldOf(TEXT("ent_g2_ism"));
	const FTransform* Loose = Plan.WorldOf(TEXT("ent_loose"));
	if (G1 && Cube && G2 && Ism && Loose)
	{
		TestTrue(TEXT("g1 stands at x 1000"), Near(G1->GetLocation(), FVector(1000, 0, 0)));
		TestTrue(TEXT("the cube stands at x 1200"), Near(Cube->GetLocation(), FVector(1200, 0, 0)));
		TestTrue(TEXT("and at the group's scale"), Near(Cube->GetScale3D(), FVector(2, 2, 2)));
		TestTrue(TEXT("g2 stands at (1000, 1000)"), Near(G2->GetLocation(), FVector(1000, 1000, 0)));
		TestTrue(TEXT("the instanced mesh stands at (1020, 1000)"), Near(Ism->GetLocation(), FVector(1020, 1000, 0)));
		TestTrue(TEXT("at scale 2"), Near(Ism->GetScale3D(), FVector(2, 2, 2)));
		TestTrue(TEXT("the loose cube stands at x 5000"), Near(Loose->GetLocation(), FVector(5000, 0, 0)));
	}
	TestNull(TEXT("a Movable entity has no world transform in the plan"), Plan.WorldOf(TEXT("ent_dyn")));

	// Every entity has one, Static or not (a Movable entity is spawned where it stands too): the dynamic cube at x 6000; the cube under the Text at 8000 + 10 = 8010;
	// the cube under the dynamic group at 9000 + 10 = 9010; the dynamic cube under the plain group at 9500 + 10 = 9510. Eleven Movable (the dynamic instanced mesh at 13000, the box trigger at 14000, the sphere trigger under g3 at 9500 + 20 = 9520, and the eight above) and the five Static: sixteen entities.
	TestEqual(TEXT("every entity of the place is in the plan"), Plan.AllWorld.Num(), Parsed.Place.Entities.Num());
	const FTransform* Dyn = Plan.AnyWorldOf(TEXT("ent_dyn"));
	const FTransform* UnderText = Plan.AnyWorldOf(TEXT("ent_under_text"));
	const FTransform* UnderGroup = Plan.AnyWorldOf(TEXT("ent_under_gdyn"));
	const FTransform* G3Dyn = Plan.AnyWorldOf(TEXT("ent_g3_dyn"));
	const FTransform* IsmDyn = Plan.AnyWorldOf(TEXT("ent_ism_dyn"));
	const FTransform* TriggerBox = Plan.AnyWorldOf(TEXT("ent_trigger_box"));
	const FTransform* TriggerSphere = Plan.AnyWorldOf(TEXT("ent_trigger_sphere"));
	if (IsmDyn && TriggerBox && TriggerSphere)
	{
		TestTrue(TEXT("the dynamic instanced mesh stands at x 13000"), Near(IsmDyn->GetLocation(), FVector(13000, 0, 0)));
		TestTrue(TEXT("the box trigger stands at x 14000"), Near(TriggerBox->GetLocation(), FVector(14000, 0, 0)));
		TestTrue(TEXT("the sphere trigger under g3 stands at x 9520"), Near(TriggerSphere->GetLocation(), FVector(9520, 0, 0)));
	}
	else
	{
		AddError(TEXT("an entity with its own root has no world transform"));
	}
	if (Dyn && UnderText && UnderGroup && G3Dyn)
	{
		TestTrue(TEXT("the dynamic cube stands at x 6000"), Near(Dyn->GetLocation(), FVector(6000, 0, 0)));
		TestTrue(TEXT("the cube under the Text stands at x 8010"), Near(UnderText->GetLocation(), FVector(8010, 0, 0)));
		TestTrue(TEXT("the cube under the dynamic group stands at x 9010"), Near(UnderGroup->GetLocation(), FVector(9010, 0, 0)));
		TestTrue(TEXT("the dynamic cube under the plain group stands at x 9510"), Near(G3Dyn->GetLocation(), FVector(9510, 0, 0)));
	}
	else
	{
		AddError(TEXT("a Movable entity has no world transform"));
	}
	TestTrue(TEXT("a Static entity's is the same one"), Plan.AnyWorldOf(TEXT("ent_loose")) && Near(Plan.AnyWorldOf(TEXT("ent_loose"))->GetLocation(), FVector(5000, 0, 0)));
	TestNull(TEXT("an id the place does not have has none"), Plan.AnyWorldOf(TEXT("ent_nothing")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleFreezePlanStepsTest,
	"Mantle.Runtime.PlaceBuild.FreezePlan.NeverMovable",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleFreezePlanStepsTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceFreezePlanTestPrivate;
	FWorldHolder Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	const FMantleParseResult Parsed = ParsePlace();
	if (!TestTrue(TEXT("the place parses"), Parsed.bSuccess))
	{
		return false;
	}
	const FMantlePlaceFreezePlan Plan = FMantlePlaceFreezePlan::Make(Parsed.Place);
	FMantleBuildOptions Options;
	Options.bRuntimeEnvironment = false;
	TSharedRef<FMantlePlaceBuildSession> Session = FMantlePlaceBuilder::Begin(Fixture.World, Parsed.Place, Options);

	// Stepped one unit at a time; after every step, every registered component of every entity that ends Static is Static and stands at its final transform:
	// none was ever Movable, or at the origin, for a body to be made from. Checked on every step, not only at the end.
	int32 Steps = 0;
	int32 Checks = 0;
	int32 MovableChecks = 0;
	int32 CollisionChecks = 0;
	bool bDone = false;
	while (!bDone && Steps < 10000)
	{
		bDone = Session->Step(0.0);
		++Steps;
		for (const TPair<FString, AActor*>& Pair : Session->GetResult().Actors)
		{
			const FTransform* Final = Plan.WorldOf(Pair.Key);
			if (!Final)
			{
				// What ends Movable is placed too, from the first step: nothing is parked at the origin until the Transforms pass (a pile of thousands of Movable bodies on
				// one spot costs the solver about 18 ms a step). It stays Movable and stands where the place puts it, spawn pass and Transforms pass alike.
				const FTransform* Placed = Plan.AnyWorldOf(Pair.Key);
				if (!Placed)
				{
					AddError(FString::Printf(TEXT("step %d: %s has no world transform in the plan"), Steps, *Pair.Key));
					continue;
				}
				++MovableChecks;
				if (!Pair.Value->GetActorLocation().Equals(Placed->GetLocation(), 0.01) || !Pair.Value->GetActorScale3D().Equals(Placed->GetScale3D(), 0.001))
				{
					AddError(FString::Printf(TEXT("step %d: %s (Movable) is at %s, not %s"), Steps, *Pair.Key, *Pair.Value->GetActorLocation().ToString(), *Placed->GetLocation().ToString()));
				}
				if (const USceneComponent* Root = Pair.Value->GetRootComponent(); Root && Root->IsRegistered())
				{
					if (Root->Mobility == EComponentMobility::Static)
					{
						AddError(FString::Printf(TEXT("step %d: %s ends Movable but its root is Static"), Steps, *Pair.Key));
					}
					// The root a builder adds to a bare actor registers where the entity ends, not at the origin (a bare actor drops the spawn transform): the instanced
					// mesh's per-instance bodies and a trigger's overlaps are made at that registration.
					if (!Root->GetComponentLocation().Equals(Placed->GetLocation(), 0.01))
					{
						AddError(FString::Printf(TEXT("step %d: %s's root is registered at %s, not %s"), Steps, *Pair.Key, *Root->GetComponentLocation().ToString(), *Placed->GetLocation().ToString()));
					}
				}
				TInlineComponentArray<UPrimitiveComponent*> Bodies(Pair.Value);
				for (const UPrimitiveComponent* Body : Bodies)
				{
					if (Body->IsRegistered() && Body->GetCollisionEnabled() != ECollisionEnabled::NoCollision)
					{
						++CollisionChecks;
						if (Body == Pair.Value->GetRootComponent() && !Body->GetComponentLocation().Equals(Placed->GetLocation(), 0.01))
						{
							AddError(FString::Printf(TEXT("step %d: a registered body of %s is at %s, not %s"), Steps, *Pair.Key, *Body->GetComponentLocation().ToString(), *Placed->GetLocation().ToString()));
						}
					}
				}
				continue;
			}
			TInlineComponentArray<USceneComponent*> Parts(Pair.Value);
			for (const USceneComponent* Part : Parts)
			{
				if (!Part->IsRegistered())
				{
					continue;
				}
				++Checks;
				if (Part->Mobility != EComponentMobility::Static)
				{
					AddError(FString::Printf(TEXT("step %d: %s has a registered component that is not Static"), Steps, *Pair.Key));
				}
			}
			if (!Pair.Value->GetActorLocation().Equals(Final->GetLocation(), 0.01) || !Pair.Value->GetActorScale3D().Equals(Final->GetScale3D(), 0.001))
			{
				AddError(FString::Printf(TEXT("step %d: %s is at %s, not %s"), Steps, *Pair.Key, *Pair.Value->GetActorLocation().ToString(), *Final->GetLocation().ToString()));
			}
		}
	}
	TestTrue(TEXT("finished"), bDone);
	TestTrue(TEXT("a registered component was looked at on many steps"), Checks > 20);
	TestTrue(TEXT("and so was a Movable entity"), MovableChecks > 20);
	TestTrue(TEXT("and registered bodies with collision, on many steps"), CollisionChecks > 20);
	FMantleBuildResult& Built = Session->GetResult();
	TestTrue(TEXT("the build succeeds"), Built.bSuccess);
	TestEqual(TEXT("an actor for every entity"), Built.Actors.Num(), Parsed.Place.Entities.Num());

	// Attached without moving: still where the plan put them, under their Groups.
	AActor* Cube = Built.Actors.FindRef(TEXT("ent_g1_cube"));
	if (TestNotNull(TEXT("the cube"), Cube))
	{
		TestTrue(TEXT("the cube is attached to its group"), Cube->GetAttachParentActor() == Built.Actors.FindRef(TEXT("ent_g1")));
		TestTrue(TEXT("and stands at x 1200"), Near(Cube->GetActorLocation(), FVector(1200, 0, 0)));
	}
	AActor* Ism = Built.Actors.FindRef(TEXT("ent_g2_ism"));
	if (TestNotNull(TEXT("the instanced mesh"), Ism))
	{
		TestTrue(TEXT("the instanced mesh is attached to the inner group"), Ism->GetAttachParentActor() == Built.Actors.FindRef(TEXT("ent_g2")));
		TestTrue(TEXT("and stands at (1020, 1000)"), Near(Ism->GetActorLocation(), FVector(1020, 1000, 0)));
		const UInstancedStaticMeshComponent* Instances = Ism->FindComponentByClass<UInstancedStaticMeshComponent>();
		TestEqual(TEXT("with its two instances"), Instances ? Instances->GetInstanceCount() : 0, 2);
	}

	// What does not end Static is placed the old way: Movable, at its place (a dynamic cube under a Static Group stays Movable; a cube under a Text follows it).
	for (const TCHAR* Id : { TEXT("ent_dyn"), TEXT("ent_none"), TEXT("ent_text"), TEXT("ent_under_text"), TEXT("ent_gdyn"), TEXT("ent_under_gdyn"), TEXT("ent_g3"), TEXT("ent_g3_dyn"),
		TEXT("ent_ism_dyn"), TEXT("ent_trigger_box"), TEXT("ent_trigger_sphere") })
	{
		const AActor* Actor = Built.Actors.FindRef(Id);
		if (TestNotNull(*FString::Printf(TEXT("%s was built"), Id), Actor) && Actor->GetRootComponent())
		{
			TestEqual(*FString::Printf(TEXT("%s is Movable"), Id), static_cast<int32>(Actor->GetRootComponent()->Mobility), static_cast<int32>(EComponentMobility::Movable));
		}
	}
	const AActor* UnderDynamic = Built.Actors.FindRef(TEXT("ent_under_gdyn"));
	if (UnderDynamic)
	{
		TestTrue(TEXT("a cube under a dynamic group stands at its place plus the group's"), Near(UnderDynamic->GetActorLocation(), FVector(9010, 0, 0)));
	}

	FMantlePlaceBuilder::Teardown(Built);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
