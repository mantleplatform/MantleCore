// Copyright Joshua Eger. All Rights Reserved.
#include "MantleExportedEntities.h"
#include "MantleCoverage.h"
#include "MantlePlaceAssets.h"
#include "MantlePlaceExporter.h"
#include "MantlePlaceValidator.h"
#include "MantleSlotSplit.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "Misc/AutomationTest.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS

// A placed mesh that draws with more materials than a place mesh may have (8) publishes as several meshes of at most 8 materials
// each, so it looks the same in game and nothing is refused (FMantleSlotSplit). City Sample's freeway markings (9 to 17
// materials, every one different) are the real case. These tests build meshes of 9 and 17 slots, one triangle a slot.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleSlotSplitTestPrivate
{
	/** Cleans the cache up either side of a test. */
	struct FSplitFixture
	{
		FSplitFixture() { FMantleSlotSplit::ClearCache(); }
		~FSplitFixture() { FMantleSlotSplit::ClearCache(); }
	};

	struct FTestWorld
	{
		UWorld* World = nullptr;

		FTestWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld*/ false, TEXT("MantleSlotSplitTestWorld"));
		}

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

	/** Two materials the engine always has, used in turn so neighboring slots differ. */
	UMaterialInterface* MaterialFor(int32 Slot)
	{
		static const TCHAR* const Paths[] = {
			TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial"),
			TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"),
			TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"),
		};
		UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, Paths[Slot % UE_ARRAY_COUNT(Paths)]);
		return Material ? Material : UMaterial::GetDefaultMaterial(MD_Surface);
	}

	/**
	 * A transient mesh of Slots slots: one triangle in each, a hundred centimeters apart on X, so a slot's triangle says which slot it
	 * is. With bBox it has one box as simple collision.
	 */
	UStaticMesh* MakeSlotted(int32 Slots, bool bBox, bool bBody = true)
	{
		FMeshDescription Description;
		FStaticMeshAttributes Attributes(Description);
		Attributes.Register();
		TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
		TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
		TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
		TPolygonGroupAttributesRef<FName> SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
		UVs.SetNumChannels(1);

		UObject* Outer = GetTransientPackage();
		UStaticMesh* Mesh = NewObject<UStaticMesh>(Outer, MakeUniqueObjectName(Outer, UStaticMesh::StaticClass(), FName(TEXT("SlotSplitTestMesh"))), RF_Transient);
		for (int32 Slot = 0; Slot < Slots; ++Slot)
		{
			const FName Name(*FString::Printf(TEXT("TestSlot_%d"), Slot));
			Mesh->GetStaticMaterials().Add(FStaticMaterial(MaterialFor(Slot), Name, Name));

			const FPolygonGroupID Group = Description.CreatePolygonGroup();
			SlotNames[Group] = Name;
			const float X = Slot * 100.0f;
			const FVector3f Points[3] = { FVector3f(X, 0.0f, 0.0f), FVector3f(X + 50.0f, 0.0f, 0.0f), FVector3f(X, 50.0f, 0.0f) };
			TArray<FVertexInstanceID> Corners;
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const FVertexID Vertex = Description.CreateVertex();
				Positions[Vertex] = Points[Corner];
				const FVertexInstanceID Instance = Description.CreateVertexInstance(Vertex);
				Normals[Instance] = FVector3f(0.0f, 0.0f, 1.0f);
				UVs.Set(Instance, 0, FVector2f(Corner == 1 ? 1.0f : 0.0f, Corner == 2 ? 1.0f : 0.0f));
				Corners.Add(Instance);
			}
			Description.CreatePolygon(Group, Corners);
		}

		Mesh->bAllowCPUAccess = true;
		Mesh->SetNumSourceModels(1);
		FMeshBuildSettings& Settings = Mesh->GetSourceModel(0).BuildSettings;
		Settings.bRecomputeTangents = true;
		Settings.bGenerateLightmapUVs = false;
		Settings.bBuildReversedIndexBuffer = false;
		Settings.DistanceFieldResolutionScale = 0.0f;
		Settings.MaxLumenMeshCards = 0;
		UStaticMesh::FBuildMeshDescriptionsParams Params;
		Params.bMarkPackageDirty = false;
		Params.bAllowCpuAccess = true;
		TArray<const FMeshDescription*> Descriptions;
		Descriptions.Add(&Description);
		Mesh->BuildFromMeshDescriptions(Descriptions, Params);

		if (bBody)
		{
			Mesh->CreateBodySetup();
		}
		else
		{
			// The build makes a body setup of its own; this is a mesh that has none.
			Mesh->SetBodySetup(nullptr);
		}
		if (UBodySetup* Body = Mesh->GetBodySetup())
		{
			Body->AggGeom.EmptyElements();
			if (bBox)
			{
				Body->CollisionTraceFlag = CTF_UseSimpleAsComplex;
				FKBoxElem Box;
				Box.Center = FVector(50.0, 25.0, 0.0);
				Box.X = 100.0f;
				Box.Y = 50.0f;
				Box.Z = 20.0f;
				Body->AggGeom.BoxElems.Add(Box);
			}
			else
			{
				Body->CollisionTraceFlag = CTF_UseComplexAsSimple;
			}
			Body->InvalidatePhysicsData();
		}
		return Mesh;
	}

	AStaticMeshActor* SpawnMeshActor(UWorld* World, const TCHAR* Label, UStaticMesh* Mesh, const FVector& Location)
	{
		AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(Location, FRotator::ZeroRotator);
		if (Actor)
		{
			Actor->SetActorLabel(Label);
			Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
		}
		return Actor;
	}

	/** A plain actor holding one instanced component of Mesh with an instance at each of Locations, as City Sample's freeway markings are. */
	UInstancedStaticMeshComponent* SpawnInstances(UWorld* World, const TCHAR* Label, UStaticMesh* Mesh, const TArray<FVector>& Locations)
	{
		AActor* Actor = World->SpawnActor<AActor>();
		if (!Actor)
		{
			return nullptr;
		}
		Actor->SetActorLabel(Label);
		USceneComponent* Root = NewObject<USceneComponent>(Actor, TEXT("Root"));
		Actor->SetRootComponent(Root);
		Root->RegisterComponent();
		UInstancedStaticMeshComponent* Component = NewObject<UInstancedStaticMeshComponent>(Actor, TEXT("Markings"));
		Component->SetStaticMesh(Mesh);
		Component->SetupAttachment(Root);
		Component->RegisterComponent();
		for (const FVector& Location : Locations)
		{
			Component->AddInstance(FTransform(FRotator::ZeroRotator, Location, FVector::OneVector), /*bWorldSpace*/ true);
		}
		return Component;
	}

	bool HasSlotCountError(const FMantleValidationResult& Result)
	{
		return Result.Issues.ContainsByPredicate([](const FMantleValidationIssue& Issue)
		{
			return Issue.Severity == EMantleIssueSeverity::Error && Issue.Problem.Contains(TEXT("material slots"));
		});
	}

	bool HasSlotCountError(const FMantleExportResult& Result)
	{
		return Result.Errors.ContainsByPredicate([](const FMantleParseError& Error) { return Error.Message.Contains(TEXT("material slots")); });
	}

	/** How many triangles part Part draws, over every section of LOD 0. */
	int32 TrianglesOf(const UStaticMesh& Mesh)
	{
		const FStaticMeshRenderData* RenderData = Mesh.GetRenderData();
		return RenderData && RenderData->LODResources.IsValidIndex(0) ? RenderData->LODResources[0].GetNumTriangles() : 0;
	}
}

// A mesh of 9 slots is two parts (8 and 1) and one of 17 is three (8, 8 and 1); each part is a mesh of its own slots from 0, that says
// which slot of the source each is; together they hold every triangle once; the source is left as it was; and a second ask for the
// same source gives the same meshes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleSlotSplitPartsTest,
	"Mantle.Editor.SlotSplit.CutsBySlotIntoPartsOfAtMostEight",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleSlotSplitPartsTest::RunTest(const FString& Parameters)
{
	using namespace MantleSlotSplitTestPrivate;
	FSplitFixture Fixture;

	const int32 Cases[][2] = { { 9, 2 }, { 17, 3 }, { 16, 2 } };
	for (const int32* Case : Cases)
	{
		const int32 Slots = Case[0];
		const int32 Expected = Case[1];
		TStrongObjectPtr<UStaticMesh> Source(MakeSlotted(Slots, /*bBox*/ false));
		if (!TestTrue(*FString::Printf(TEXT("the %d-slot mesh built"), Slots), FMantlePlaceAssets::UploadSlots(Source.Get()).Num() == Slots))
		{
			continue;
		}

		TArray<UStaticMesh*> Parts;
		FString Why;
		if (!TestTrue(*FString::Printf(TEXT("%d slots split"), Slots), FMantleSlotSplit::Split(Source.Get(), 8, Parts, Why)))
		{
			AddError(Why);
			continue;
		}
		TestEqual(*FString::Printf(TEXT("%d slots make %d parts"), Slots, Expected), Parts.Num(), Expected);

		int32 Triangles = 0;
		TArray<int32> SeenSlots;
		for (int32 PartIndex = 0; PartIndex < Parts.Num(); ++PartIndex)
		{
			const UStaticMesh* Part = Parts[PartIndex];
			const TArray<int32> Own = FMantlePlaceAssets::UploadSlots(Part);
			TestTrue(*FString::Printf(TEXT("part %d of %d slots draws 1 to 8 slots (%d)"), PartIndex, Slots, Own.Num()), Own.Num() >= 1 && Own.Num() <= 8);
			TestTrue(TEXT("it is a part, cut from the source"), FMantleSlotSplit::IsPart(Part) && FMantleSlotSplit::WholeOf(Part) == Source.Get());
			TestEqual(TEXT("it has no more material slots than it draws"), Part->GetStaticMaterials().Num(), Own.Num());
			Triangles += TrianglesOf(*Part);
			for (const int32 Slot : Own)
			{
				const int32 SourceSlot = FMantleSlotSplit::SourceSlot(Part, Slot);
				SeenSlots.Add(SourceSlot);
				TestTrue(TEXT("each slot keeps the material of the source slot it is"),
					Part->GetStaticMaterials()[Slot].MaterialInterface == Source->GetStaticMaterials()[SourceSlot].MaterialInterface);
			}
		}
		TestEqual(TEXT("together the parts hold every triangle once"), Triangles, Slots);
		SeenSlots.Sort();
		TArray<int32> All;
		for (int32 Slot = 0; Slot < Slots; ++Slot)
		{
			All.Add(Slot);
		}
		TestEqual(TEXT("and every slot of the source once"), SeenSlots, All);
		TestEqual(TEXT("the source keeps its slots"), Source->GetStaticMaterials().Num(), Slots);

		TArray<UStaticMesh*> Again;
		FMantleSlotSplit::Split(Source.Get(), 8, Again, Why);
		TestTrue(TEXT("a second ask gives the same parts: one split for every actor and instance that draws the mesh"), Again == Parts);
	}

	TStrongObjectPtr<UStaticMesh> Eight(MakeSlotted(8, false));
	TArray<UStaticMesh*> None;
	FString Why;
	TestFalse(TEXT("a mesh of 8 slots is not split"), FMantleSlotSplit::Split(Eight.Get(), 8, None, Why));
	TestEqual(TEXT("and has no parts"), None.Num(), 0);
	return true;
}

// Collision stays what the source had: simple shapes go to part 0 only (the other parts publish with none); a source that collides with its
// triangles gives every part its own, which together are all of them.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleSlotSplitCollisionTest,
	"Mantle.Editor.SlotSplit.CollisionStaysIdentical",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleSlotSplitCollisionTest::RunTest(const FString& Parameters)
{
	using namespace MantleSlotSplitTestPrivate;
	FSplitFixture Fixture;

	TStrongObjectPtr<UStaticMesh> Boxed(MakeSlotted(17, /*bBox*/ true));
	TArray<UStaticMesh*> Parts;
	FString Why;
	if (!TestTrue(TEXT("the boxed mesh split"), FMantleSlotSplit::Split(Boxed.Get(), 8, Parts, Why)) || !TestEqual(TEXT("into three"), Parts.Num(), 3))
	{
		return false;
	}
	TestTrue(TEXT("part 0 carries the source's collision"), FMantleSlotSplit::CarriesCollision(Parts[0]));
	TestEqual(TEXT("which is its box"), Parts[0]->GetBodySetup() ? Parts[0]->GetBodySetup()->AggGeom.BoxElems.Num() : -1, 1);
	for (int32 PartIndex = 1; PartIndex < Parts.Num(); ++PartIndex)
	{
		TestFalse(*FString::Printf(TEXT("part %d carries none"), PartIndex), FMantleSlotSplit::CarriesCollision(Parts[PartIndex]));
		TestEqual(TEXT("and has no shape of its own"), Parts[PartIndex]->GetBodySetup() ? Parts[PartIndex]->GetBodySetup()->AggGeom.GetElementCount() : -1, 0);
	}

	// A source with no collision body has no collision: no part gets any (it must not gain triangle collision it never had).
	TStrongObjectPtr<UStaticMesh> Bodyless(MakeSlotted(17, /*bBox*/ false, /*bBody*/ false));
	if (TestNull(TEXT("the bodyless mesh has no body setup"), Bodyless->GetBodySetup()))
	{
		TArray<UStaticMesh*> BodylessParts;
		if (TestTrue(TEXT("the bodyless mesh split"), FMantleSlotSplit::Split(Bodyless.Get(), 8, BodylessParts, Why)))
		{
			for (const UStaticMesh* Part : BodylessParts)
			{
				TestFalse(TEXT("no part of it carries collision"), FMantleSlotSplit::CarriesCollision(Part));
				TestTrue(TEXT("and none has a shape or complex collision"), !Part->GetBodySetup()
					|| (Part->GetBodySetup()->AggGeom.GetElementCount() == 0 && Part->GetBodySetup()->GetCollisionTraceFlag() != CTF_UseComplexAsSimple));
			}
		}
	}

	TStrongObjectPtr<UStaticMesh> Triangles(MakeSlotted(9, /*bBox*/ false));
	TArray<UStaticMesh*> TrianglePartsOut;
	if (!TestTrue(TEXT("the triangle-collision mesh split"), FMantleSlotSplit::Split(Triangles.Get(), 8, TrianglePartsOut, Why)))
	{
		return false;
	}
	for (const UStaticMesh* Part : TrianglePartsOut)
	{
		TestTrue(TEXT("every part of a mesh that collides with its triangles carries collision"), FMantleSlotSplit::CarriesCollision(Part));
		TestEqual(TEXT("as its own triangles"), Part->GetBodySetup() ? static_cast<int32>(Part->GetBodySetup()->GetCollisionTraceFlag()) : -1,
			static_cast<int32>(CTF_UseComplexAsSimple));
	}
	return true;
}

// A StaticMesh actor of 9 slots publishes as two StaticMesh entities (8 materials and 1) in the same place with the same name,
// Check says nothing of slot counts, Gather lists the parts and not the whole mesh, and a material override on a slot lands on the
// part that slot went to (slot 10 of 17 is the third slot of the second part).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleSlotSplitActorExportTest,
	"Mantle.Editor.SlotSplit.AStaticMeshActorPublishesAsItsParts",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleSlotSplitActorExportTest::RunTest(const FString& Parameters)
{
	using namespace MantleSlotSplitTestPrivate;
	FSplitFixture Fixture;

	TStrongObjectPtr<UStaticMesh> Nine(MakeSlotted(9, /*bBox*/ true));
	TStrongObjectPtr<UStaticMesh> Seventeen(MakeSlotted(17, /*bBox*/ true));
	FTestWorld World;
	const FVector Where(1000.0, 2000.0, 300.0);
	AStaticMeshActor* NineActor = SpawnMeshActor(World.World, TEXT("Marking9"), Nine.Get(), Where);
	AStaticMeshActor* SeventeenActor = SpawnMeshActor(World.World, TEXT("Marking17"), Seventeen.Get(), FVector(0.0, 0.0, 0.0));
	if (!TestNotNull(TEXT("the actors spawned"), NineActor) || !TestNotNull(TEXT("both"), SeventeenActor))
	{
		return false;
	}
	// A material none of the mesh's slots has, so it can only be there because the override put it there.
	UMaterialInterface* Override = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineMaterials/EmissiveMeshMaterial.EmissiveMeshMaterial"));
	if (!Override)
	{
		AddWarning(TEXT("the engine's emissive material did not load; skipped"));
		return true;
	}
	SeventeenActor->GetStaticMeshComponent()->SetMaterial(10, Override);

	// An override on the last slot (16, past the 16 an entity's materials may name) is fine on a mesh that publishes as parts.
	UMaterialInterface* LastOverride = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"));
	SeventeenActor->GetStaticMeshComponent()->SetMaterial(16, LastOverride);

	const FMantleValidationResult Validation = FMantlePlaceValidator::Validate(World.World);
	TestFalse(TEXT("Check does not refuse the slot count"), HasSlotCountError(Validation));
	TestFalse(TEXT("nor an override on slot 16 of a mesh that publishes as parts"), Validation.Issues.ContainsByPredicate([](const FMantleValidationIssue& Issue)
	{
		return Issue.Severity == EMantleIssueSeverity::Error && Issue.Problem.Contains(TEXT("overrides material slot"));
	}));

	const FMantlePlaceDependencies Dependencies = FMantlePlaceDependencies::Gather(World.World);
	TestFalse(TEXT("Gather does not list the whole mesh"), Dependencies.Meshes.ContainsByPredicate([&Nine](const FMantleGatheredMesh& Entry)
	{
		return Entry.Key == FMantlePlaceAssets::MeshKey(Nine.Get());
	}));
	TArray<UStaticMesh*> NineParts;
	FString Why;
	FMantleSlotSplit::Split(Nine.Get(), 8, NineParts, Why);
	for (const UStaticMesh* Part : NineParts)
	{
		TestTrue(TEXT("but each part"), Dependencies.Meshes.ContainsByPredicate([Part](const FMantleGatheredMesh& Entry)
		{
			return Entry.Key == FMantlePlaceAssets::MeshKey(Part);
		}));
	}

	const FMantleExportResult Result = FMantlePlaceExporter::Export(World.World, FMantleExportOptions());
	TestFalse(TEXT("Export does not refuse the slot count"), HasSlotCountError(Result));

	TArray<const FMantleStaticMeshProps*> NineProps;
	TArray<const FMantleEntity*> NineEntities;
	TArray<const FMantleStaticMeshProps*> SeventeenProps;
	for (const FMantleEntity* Entity : MantleExportedEntities::All(Result))
	{
		const FMantleStaticMeshProps* Props = Entity->Props.TryGet<FMantleStaticMeshProps>();
		if (!Props)
		{
			continue;
		}
		if (Entity->Name == TEXT("Marking9"))
		{
			NineProps.Add(Props);
			NineEntities.Add(Entity);
		}
		else if (Entity->Name == TEXT("Marking17"))
		{
			SeventeenProps.Add(Props);
		}
	}
	if (!TestEqual(TEXT("the 9-slot actor is two entities"), NineProps.Num(), 2) || !TestEqual(TEXT("the 17-slot actor is three"), SeventeenProps.Num(), 3))
	{
		return false;
	}
	TestEqual(TEXT("the first has 8 materials"), NineProps[0]->Materials.Num(), 8);
	TestEqual(TEXT("the second has 1"), NineProps[1]->Materials.Num(), 1);
	TestTrue(TEXT("they are different meshes"), NineProps[0]->Mesh != NineProps[1]->Mesh);
	TestTrue(TEXT("in the same place"), NineEntities[0]->Transform.Equals(NineEntities[1]->Transform) && NineEntities[0]->Transform.GetLocation().Equals(Where));
	TestTrue(TEXT("with their own ids"), NineEntities[0]->Id != NineEntities[1]->Id);
	TestTrue(TEXT("the first keeps the actor's collision"), NineProps[0]->Collision == EMantleCollision::Blocking);
	TestTrue(TEXT("the second, a part of a mesh with a box, has none"), NineProps[1]->Collision == EMantleCollision::None);
	TestTrue(TEXT("both are drawn"), NineProps[0]->bVisible && NineProps[1]->bVisible);

	// Slot 10 is the third slot of the second part.
	TestEqual(TEXT("each part of the 17 has its materials"), SeventeenProps[1]->Materials.Num(), 8);
	const FString OverrideId = FMantlePlaceAssets::ProvisionalId(FMantlePlaceAssets::MaterialKey(Override));
	TestEqual(TEXT("the override of slot 10 is on the second part's third slot"), SeventeenProps[1]->Materials.IsValidIndex(2) ? SeventeenProps[1]->Materials[2] : FString(), OverrideId);
	TestFalse(TEXT("and not on the first part"), SeventeenProps[0]->Materials.Contains(OverrideId));
	TestFalse(TEXT("nor the third"), SeventeenProps[2]->Materials.Contains(OverrideId));
	// Slot 16 is the only slot of the third part.
	TestEqual(TEXT("the override of slot 16 is on the third part's only slot"), SeventeenProps[2]->Materials.Num() == 1 ? SeventeenProps[2]->Materials[0] : FString(),
		FMantlePlaceAssets::ProvisionalId(FMantlePlaceAssets::MaterialKey(LastOverride)));

	// Every mesh an entity names is one Gather listed, so the upload has it.
	for (const FMantleStaticMeshProps* Props : NineProps)
	{
		TestTrue(TEXT("Export lists each part as an asset"), Result.Place.Assets.ContainsByPredicate([Props](const FMantleAssetRef& Ref) { return Ref.Id == Props->Mesh; }));
	}
	return true;
}

// An instanced component of a 9-slot mesh publishes as two instance sets (one for each part), each with every instance at its own
// transform; one of 17 slots as three. A second component of the same mesh joins the same sets (one split for the whole level).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleSlotSplitInstancedExportTest,
	"Mantle.Editor.SlotSplit.InstancedMeshesPublishOneSetPerPart",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleSlotSplitInstancedExportTest::RunTest(const FString& Parameters)
{
	using namespace MantleSlotSplitTestPrivate;
	FSplitFixture Fixture;

	TStrongObjectPtr<UStaticMesh> Nine(MakeSlotted(9, /*bBox*/ false));
	TStrongObjectPtr<UStaticMesh> Seventeen(MakeSlotted(17, /*bBox*/ false));
	FTestWorld World;
	const TArray<FVector> NineWhere = { FVector(0.0, 0.0, 0.0), FVector(500.0, 0.0, 0.0), FVector(1000.0, 0.0, 0.0) };
	const TArray<FVector> MoreWhere = { FVector(0.0, 5000.0, 0.0), FVector(500.0, 5000.0, 0.0) };
	if (!TestNotNull(TEXT("the first nine-slot component spawned"), SpawnInstances(World.World, TEXT("Decals9"), Nine.Get(), NineWhere))
		|| !TestNotNull(TEXT("the second"), SpawnInstances(World.World, TEXT("Decals9b"), Nine.Get(), MoreWhere))
		|| !TestNotNull(TEXT("the seventeen-slot one"), SpawnInstances(World.World, TEXT("Decals17"), Seventeen.Get(), NineWhere)))
	{
		return false;
	}

	const FMantleValidationResult Validation = FMantlePlaceValidator::Validate(World.World);
	TestFalse(TEXT("Check does not refuse the slot count"), HasSlotCountError(Validation));

	const FMantleExportResult Result = FMantlePlaceExporter::Export(World.World, FMantleExportOptions());
	TestFalse(TEXT("Export does not refuse the slot count"), HasSlotCountError(Result));

	TArray<UStaticMesh*> NineParts;
	TArray<UStaticMesh*> SeventeenParts;
	FString Why;
	FMantleSlotSplit::Split(Nine.Get(), 8, NineParts, Why);
	FMantleSlotSplit::Split(Seventeen.Get(), 8, SeventeenParts, Why);

	// Instances by the mesh id the set names.
	TMap<FString, TArray<FTransform>> InstancesByMesh;
	TMap<FString, int32> SetsByMesh;
	for (const FMantleEntity* Entity : MantleExportedEntities::All(Result))
	{
		if (const FMantleInstancedMeshProps* Props = Entity->Props.TryGet<FMantleInstancedMeshProps>())
		{
			InstancesByMesh.FindOrAdd(Props->Mesh.Mesh).Append(Props->Instances);
			SetsByMesh.FindOrAdd(Props->Mesh.Mesh) += 1;
		}
	}
	TestEqual(TEXT("one set for each of the 2 + 3 parts"), InstancesByMesh.Num(), 5);

	if (TestEqual(TEXT("the nine-slot mesh has two parts"), NineParts.Num(), 2))
	{
		for (const UStaticMesh* Part : NineParts)
		{
			const FString Id = FMantlePlaceAssets::ProvisionalId(FMantlePlaceAssets::MeshKey(Part));
			const TArray<FTransform>* Found = InstancesByMesh.Find(Id);
			if (TestNotNull(TEXT("each part has a set"), Found))
			{
				TestEqual(TEXT("with all five instances of both components"), Found->Num(), NineWhere.Num() + MoreWhere.Num());
			}
		}
	}
	if (TestEqual(TEXT("the seventeen-slot mesh has three parts"), SeventeenParts.Num(), 3))
	{
		TArray<FVector> FirstPlaces;
		for (const UStaticMesh* Part : SeventeenParts)
		{
			const FString Id = FMantlePlaceAssets::ProvisionalId(FMantlePlaceAssets::MeshKey(Part));
			const TArray<FTransform>* Found = InstancesByMesh.Find(Id);
			if (TestNotNull(TEXT("each part has a set"), Found) && TestEqual(TEXT("with all three instances"), Found->Num(), NineWhere.Num()))
			{
				// The same transforms in every part (the sets are placed as the export places them, so compare the parts with each other).
				TArray<FVector> Places;
				for (const FTransform& Instance : *Found)
				{
					Places.Add(Instance.GetLocation());
				}
				Places.Sort([](const FVector& A, const FVector& B) { return A.X < B.X; });
				if (FirstPlaces.Num() == 0)
				{
					FirstPlaces = Places;
				}
				else if (TestEqual(TEXT("each part has the same number of places"), Places.Num(), FirstPlaces.Num()))
				{
					for (int32 Index = 0; Index < Places.Num(); ++Index)
					{
						TestTrue(TEXT("an instance is where the first part has it"), Places[Index].Equals(FirstPlaces[Index], 0.01));
					}
				}
				TestTrue(TEXT("and they are spread 500 cm apart as placed"), Places.Num() == 3 && FMath::IsNearlyEqual(Places[1].X - Places[0].X, 500.0, 0.01)
					&& FMath::IsNearlyEqual(Places[2].X - Places[1].X, 500.0, 0.01));
			}
		}
	}

	// Gather lists every part (and so uploads them), and not the whole meshes.
	const FMantlePlaceDependencies Dependencies = FMantlePlaceDependencies::Gather(World.World);
	for (const UStaticMesh* Part : SeventeenParts)
	{
		TestTrue(TEXT("Gather lists the part"), Dependencies.Meshes.ContainsByPredicate([Part](const FMantleGatheredMesh& Entry) { return Entry.Key == FMantlePlaceAssets::MeshKey(Part); }));
	}
	TestFalse(TEXT("and not the whole mesh"), Dependencies.Meshes.ContainsByPredicate([&Seventeen](const FMantleGatheredMesh& Entry)
	{
		return Entry.Key == FMantlePlaceAssets::MeshKey(Seventeen.Get());
	}));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
