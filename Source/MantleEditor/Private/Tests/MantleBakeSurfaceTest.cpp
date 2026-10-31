// Copyright Joshua Eger. All Rights Reserved.
#include "MantleGlbWriter.h"
#include "MantleLandscapeMaterialReader.h"
#include "MantlePlaceAssetConverter.h"
#include "MantlePlaceAssets.h"

#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Misc/AutomationTest.h"
#include "Rendering/ColorVertexBuffer.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshComponentLODInfo.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// What a material is baked on (FMantleBakeSurface). GLTFExporter's own card has white vertex colours, so a road whose puddles are painted
// in Vertex Color's blue (DowntownWestModularPack's MI_Asphault_A) baked all puddle: a black mirror. Gather now averages the colours of the
// meshes drawing a material (a component's painted ones where it has them), the converter bakes a material that reads vertex colour at that
// colour, and the cache keys on it. A landscape's own material is baked over many quads, its texture transforms scaled back to one tile.
// The bake itself needs a renderer; what decides it does not.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleBakeSurfaceTestPrivate
{
	template <typename TExpression>
	TExpression* AddNode(UMaterial* Material)
	{
		TExpression* Expression = NewObject<TExpression>(Material);
		Expression->Material = Material;
		Material->GetExpressionCollection().AddExpression(Expression);
		return Expression;
	}

	/** A plain material (nothing connected). */
	UMaterial* NewMaterial()
	{
		return NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
	}

	/** Roughness is Vertex Color's blue: a puddle mask, which the bake must see the meshes' colour for. */
	UMaterial* NewPuddleMaterial()
	{
		UMaterial* Material = NewMaterial();
		UMaterialExpressionVertexColor* Color = AddNode<UMaterialExpressionVertexColor>(Material);
		Material->GetEditorOnlyData()->Roughness.Connect(3, Color);
		return Material;
	}

	/** A quad of 4 vertices drawing Material in its one slot; every corner Colour when given, else no vertex colours at all. */
	UStaticMesh* NewQuad(UMaterialInterface* Material, const FColor* Colour)
	{
		FMeshDescription Description;
		FStaticMeshAttributes Attributes(Description);
		Attributes.Register();
		const FPolygonGroupID Group = Description.CreatePolygonGroup();
		Attributes.GetPolygonGroupMaterialSlotNames()[Group] = TEXT("Slot");
		TArray<FVertexInstanceID> Corners;
		for (int32 Corner = 0; Corner < 4; ++Corner)
		{
			const FVertexID Vertex = Description.CreateVertex();
			Attributes.GetVertexPositions()[Vertex] = FVector3f(100.0f * (Corner & 1), 100.0f * (Corner >> 1), 0.0f);
			const FVertexInstanceID Instance = Description.CreateVertexInstance(Vertex);
			Attributes.GetVertexInstanceNormals()[Instance] = FVector3f(0.0f, 0.0f, 1.0f);
			Attributes.GetVertexInstanceTangents()[Instance] = FVector3f(1.0f, 0.0f, 0.0f);
			Attributes.GetVertexInstanceBinormalSigns()[Instance] = 1.0f;
			Attributes.GetVertexInstanceUVs().Set(Instance, 0, FVector2f(static_cast<float>(Corner & 1), static_cast<float>(Corner >> 1)));
			Attributes.GetVertexInstanceColors()[Instance] = Colour ? FVector4f(FLinearColor(*Colour)) : FVector4f(1.0f, 1.0f, 1.0f, 1.0f);
			Corners.Add(Instance);
		}
		Description.CreateTriangle(Group, { Corners[0], Corners[2], Corners[1] });
		Description.CreateTriangle(Group, { Corners[2], Corners[3], Corners[1] });
		UStaticMesh* Mesh = NewObject<UStaticMesh>(GetTransientPackage(), NAME_None, RF_Transient);
		Mesh->GetStaticMaterials().Add(FStaticMaterial(Material, TEXT("Slot"), TEXT("Slot")));
		Mesh->bAllowCPUAccess = true;
		UStaticMesh::FBuildMeshDescriptionsParams Params;
		Params.bMarkPackageDirty = false;
		Params.bAllowCpuAccess = true;
		Params.bBuildSimpleCollision = false;
		return Mesh->BuildFromMeshDescriptions({ &Description }, Params) ? Mesh : nullptr;
	}

	/** A component showing Mesh, with every vertex painted Painted when given (Override Vertex Colors, as the mesh paint tool leaves it). */
	UStaticMeshComponent* NewComponent(UStaticMesh* Mesh, const FColor* Painted)
	{
		UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient);
		Component->SetStaticMesh(Mesh);
		if (Painted)
		{
			Component->SetLODDataCount(1, 1);
			TArray<FColor> Colors;
			Colors.Init(*Painted, Mesh->GetRenderData()->LODResources[0].GetNumVertices());
			Component->LODData[0].OverrideVertexColors = new FColorVertexBuffer;
			Component->LODData[0].OverrideVertexColors->InitFromColorArray(Colors);
		}
		return Component;
	}

	const FMantleGatheredMaterial* Find(const FMantlePlaceDependencies& Dependencies, const UMaterialInterface* Material)
	{
		const FString Key = FMantlePlaceAssets::MaterialKey(Material);
		return Dependencies.Materials.FindByPredicate([&Key](const FMantleGatheredMaterial& Entry) { return Entry.Key == Key; });
	}

	/** Writes one flat material and remembers the surface it was asked to bake on. */
	class FSurfaceFlattener : public IMantleMaterialFlattener
	{
	public:
		int32 Calls = 0;
		FMantleBakeSurface Seen;

		virtual bool Flatten(UMaterialInterface* Material, const FMantleFlattenSettings& Settings, FMantleGlbDocument& OutDocument,
			TArray<FString>& OutMessages, FString& OutError) override
		{
			++Calls;
			Seen = Settings.Surface;
			const TSharedRef<FJsonObject> Pbr = MakeShared<FJsonObject>();
			Pbr->SetNumberField(TEXT("metallicFactor"), 0.0);
			Pbr->SetNumberField(TEXT("roughnessFactor"), 0.5);
			const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("alphaMode"), TEXT("OPAQUE"));
			Entry->SetObjectField(TEXT("pbrMetallicRoughness"), Pbr);
			TArray<TSharedPtr<FJsonValue>> Materials;
			Materials.Add(MakeShared<FJsonValueObject>(Entry));
			OutDocument = FMantleGlbDocument();
			OutDocument.Json = MakeShared<FJsonObject>();
			OutDocument.Json->SetArrayField(TEXT("materials"), Materials);
			return true;
		}

		virtual FString Describe() const override { return TEXT("scripted bake surface"); }
	};

	/** The scale of a texture info's KHR_texture_transform, or (0, 0) when it has none. */
	FVector2D TransformScale(const TSharedPtr<FJsonObject>& Info)
	{
		const TSharedPtr<FJsonObject>* Extensions = nullptr;
		const TSharedPtr<FJsonObject>* Transform = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Scale = nullptr;
		if (Info.IsValid() && Info->TryGetObjectField(TEXT("extensions"), Extensions) && (*Extensions)->TryGetObjectField(TEXT("KHR_texture_transform"), Transform)
			&& (*Transform)->TryGetArrayField(TEXT("scale"), Scale) && Scale->Num() == 2)
		{
			return FVector2D((*Scale)[0]->AsNumber(), (*Scale)[1]->AsNumber());
		}
		return FVector2D::ZeroVector;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleBakeSurfaceGatherAveragesVertexColoursTest,
	"Mantle.Editor.BakeSurface.GatherAveragesVertexColours",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleBakeSurfaceGatherAveragesVertexColoursTest::RunTest(const FString& Parameters)
{
	using namespace MantleBakeSurfaceTestPrivate;

	// The roads: a mesh with no colours of its own, painted on each component.
	UMaterial* Asphalt = NewPuddleMaterial();
	UStaticMesh* Plane = NewQuad(Asphalt, nullptr);
	if (!TestNotNull(TEXT("the plane builds"), Plane))
	{
		return false;
	}
	const FColor Puddles(10, 20, 40, 255);
	{
		FMantlePlaceDependencies Dependencies;
		Dependencies.AddComponent(NewComponent(Plane, &Puddles), TEXT("Road"));
		Dependencies.AddComponent(NewComponent(Plane, &Puddles), TEXT("Road2"));
		const FMantleGatheredMaterial* Gathered = Find(Dependencies, Asphalt);
		if (TestNotNull(TEXT("the road material is gathered"), Gathered))
		{
			TestEqual(TEXT("its bake colour is the painted one, not the mesh's none"), Gathered->AverageVertexColor().ToHex(), Puddles.ToHex());
			TestFalse(TEXT("which is not the plain surface"), Gathered->BakeSurface().IsPlain());
		}
	}

	// One painted road and one that is not: the unpainted one is drawn white, so the average is halfway, by vertex count.
	{
		FMantlePlaceDependencies Dependencies;
		const FColor Blue(0, 0, 255, 255);
		Dependencies.AddComponent(NewComponent(Plane, &Blue), TEXT("Road"));
		Dependencies.AddComponent(NewComponent(Plane, nullptr), TEXT("Road2"));
		const FMantleGatheredMaterial* Gathered = Find(Dependencies, Asphalt);
		if (TestNotNull(TEXT("gathered"), Gathered))
		{
			TestEqual(TEXT("half white, half blue"), Gathered->AverageVertexColor().ToHex(), FColor(128, 128, 255, 255).ToHex());
		}
	}

	// Nobody painted, and the mesh has none: white, which is Unreal's and today's bake, so nothing changes.
	{
		FMantlePlaceDependencies Dependencies;
		Dependencies.AddComponent(NewComponent(Plane, nullptr), TEXT("Road"));
		const FMantleGatheredMaterial* Gathered = Find(Dependencies, Asphalt);
		if (TestNotNull(TEXT("gathered"), Gathered))
		{
			TestTrue(TEXT("white"), Gathered->AverageVertexColor() == FColor::White);
			TestTrue(TEXT("the plain surface"), Gathered->BakeSurface().IsPlain());
		}
	}

	// A mesh with colours of its own, used by no component: the mesh's.
	{
		UMaterial* Painted = NewPuddleMaterial();
		const FColor Red(200, 0, 0, 255);
		UStaticMesh* Coloured = NewQuad(Painted, &Red);
		if (TestNotNull(TEXT("the coloured quad builds"), Coloured))
		{
			FMantlePlaceDependencies Dependencies;
			Dependencies.AddMesh(Coloured, TEXT("Script Assets"));
			const FMantleGatheredMaterial* Gathered = Find(Dependencies, Painted);
			if (TestNotNull(TEXT("gathered"), Gathered))
			{
				TestEqual(TEXT("the mesh's own colour"), Gathered->AverageVertexColor().ToHex(), Red.ToHex());
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleBakeSurfaceConverterKeepsColourForVertexColourMaterialsTest,
	"Mantle.Editor.BakeSurface.ConverterKeepsColourOnlyWhereRead",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleBakeSurfaceConverterKeepsColourForVertexColourMaterialsTest::RunTest(const FString& Parameters)
{
	using namespace MantleBakeSurfaceTestPrivate;

	UMaterial* Puddle = NewPuddleMaterial();
	const FMantleMaterialAnalysis Analysis = FMantlePlaceAssets::AnalyzeMaterial(Puddle);
	TestTrue(TEXT("a roughness from Vertex Color's blue is a vertex colour loss"), Analysis.Has(EMantleMaterialLoss::VertexColor));

	FMantleBakeSurface Surface;
	Surface.VertexColor = FColor(10, 20, 40, 255);
	{
		TSharedRef<FSurfaceFlattener> Flattener = MakeShared<FSurfaceFlattener>();
		FMantlePlaceAssetConverter Converter(Flattener, FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("MantleBakeSurface")));
		FMantlePlaceAssetSet Set;
		TestTrue(TEXT("the puddle material converts"), Converter.ConvertMaterial(Puddle, Surface, Set));
		TestEqual(TEXT("the flattener ran once"), Flattener->Calls, 1);
		TestEqual(TEXT("baked at the meshes' colour"), Flattener->Seen.VertexColor.ToHex(), Surface.VertexColor.ToHex());
	}
	{
		// A material that reads no vertex colour bakes as it always has, whatever its meshes' colours.
		TSharedRef<FSurfaceFlattener> Flattener = MakeShared<FSurfaceFlattener>();
		FMantlePlaceAssetConverter Converter(Flattener, FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("MantleBakeSurface")));
		FMantlePlaceAssetSet Set;
		TestTrue(TEXT("a plain material converts"), Converter.ConvertMaterial(NewMaterial(), Surface, Set));
		TestTrue(TEXT("on the plain surface"), Flattener->Seen.IsPlain());
	}
	{
		// The single-argument ConvertMaterial is the plain surface.
		TSharedRef<FSurfaceFlattener> Flattener = MakeShared<FSurfaceFlattener>();
		FMantlePlaceAssetConverter Converter(Flattener, FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("MantleBakeSurface")));
		FMantlePlaceAssetSet Set;
		TestTrue(TEXT("it converts"), Converter.ConvertMaterial(Puddle, Set));
		TestTrue(TEXT("on the plain surface"), Flattener->Seen.IsPlain());
	}

	// The UV range is kept for any material: a landscape's own reads no vertex colour.
	FMantleBakeSurface Tiled;
	Tiled.UvRange = 10;
	Tiled.VertexColor = FColor(10, 20, 40, 255);
	const FMantleBakeSurface Kept = FMantlePlaceAssetConverter::EffectiveBakeSurface(FMantlePlaceAssets::AnalyzeMaterial(NewMaterial()), Tiled);
	TestEqual(TEXT("the range is kept"), Kept.UvRange, 10);
	TestTrue(TEXT("the colour is not"), Kept.VertexColor == FColor::White);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleBakeSurfaceCacheKeyTest,
	"Mantle.Editor.BakeSurface.CacheKeysOnSurface",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleBakeSurfaceCacheKeyTest::RunTest(const FString& Parameters)
{
	using namespace MantleBakeSurfaceTestPrivate;

	const FMantleFlattenSettings Settings;
	const FMantlePlaceAssetLimits Limits;
	UMaterial* Material = NewPuddleMaterial();
	auto Salt = [&](const FMantleBakeSurface& Surface)
	{
		return FMantlePlaceAssetConverter::MaterialCacheSalt(TEXT("flattener"), Settings, Limits, Material, false, Surface);
	};
	const FMantleBakeSurface Plain;
	FMantleBakeSurface Puddles;
	Puddles.VertexColor = FColor(10, 20, 40, 255);
	FMantleBakeSurface Dry;
	Dry.VertexColor = FColor(10, 20, 0, 255);
	FMantleBakeSurface Tiled;
	Tiled.UvRange = 10;

	TestTrue(TEXT("the plain surface adds nothing, so every plain bake keeps its key"), Plain.CacheTag().IsEmpty());
	TestEqual(TEXT("the plain salt is the one materials always had"), Salt(Plain),
		FString::Printf(TEXT("material flattener bake=%d max=%d bytes=%lld %s"), Settings.DefaultBakeSize, Limits.MaxTextureSize, Limits.MaxTextureBytes, *Material->GetPathName()));
	TestNotEqual(TEXT("a vertex colour changes the key"), Salt(Puddles), Salt(Plain));
	TestNotEqual(TEXT("a different colour is a different key"), Salt(Puddles), Salt(Dry));
	TestNotEqual(TEXT("a UV range changes the key"), Salt(Tiled), Salt(Plain));
	TestEqual(TEXT("the same colour is the same key"), Salt(Puddles), Salt(Puddles));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleBakeSurfaceScaleTransformsTest,
	"Mantle.Editor.BakeSurface.UvRangeScalesTextureTransforms",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleBakeSurfaceScaleTransformsTest::RunTest(const FString& Parameters)
{
	using namespace MantleBakeSurfaceTestPrivate;

	// What GLTFExporter writes for a bake over UVs 0 to 10: the baked base colour covers the card, scale 1/10; a normal map it kept as
	// the source texture, read at 0.1 of the card's UVs; an occlusion texture with no transform.
	auto Info = [](const TOptional<double> Scale)
	{
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetNumberField(TEXT("index"), 0);
		if (Scale.IsSet())
		{
			const TSharedRef<FJsonObject> Transform = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> Values;
			Values.Add(MakeShared<FJsonValueNumber>(Scale.GetValue()));
			Values.Add(MakeShared<FJsonValueNumber>(Scale.GetValue()));
			Transform->SetArrayField(TEXT("scale"), Values);
			const TSharedRef<FJsonObject> Extensions = MakeShared<FJsonObject>();
			Extensions->SetObjectField(TEXT("KHR_texture_transform"), Transform);
			Object->SetObjectField(TEXT("extensions"), Extensions);
		}
		return Object;
	};
	const TSharedRef<FJsonObject> Pbr = MakeShared<FJsonObject>();
	Pbr->SetObjectField(TEXT("baseColorTexture"), Info(0.1));
	const TSharedRef<FJsonObject> Material = MakeShared<FJsonObject>();
	Material->SetObjectField(TEXT("pbrMetallicRoughness"), Pbr);
	Material->SetObjectField(TEXT("normalTexture"), Info(0.1));
	Material->SetObjectField(TEXT("occlusionTexture"), Info(TOptional<double>()));
	TArray<TSharedPtr<FJsonValue>> Materials;
	Materials.Add(MakeShared<FJsonValueObject>(Material));
	FMantleGlbDocument Document;
	Document.Json = MakeShared<FJsonObject>();
	Document.Json->SetArrayField(TEXT("materials"), Materials);

	FMantlePlaceAssetConverter::ScaleTextureTransforms(Document, 0, 10.0);
	TestTrue(TEXT("the baked texture now covers one terrain tile"), TransformScale(Pbr->GetObjectField(TEXT("baseColorTexture"))).Equals(FVector2D(1.0, 1.0), 1.0e-9));
	TestTrue(TEXT("the kept normal map, 0.1 of the card's UVs, is 1 of a terrain tile's"), TransformScale(Material->GetObjectField(TEXT("normalTexture"))).Equals(FVector2D(1.0, 1.0), 1.0e-9));
	TestTrue(TEXT("a texture with no transform gets one of 10"), TransformScale(Material->GetObjectField(TEXT("occlusionTexture"))).Equals(FVector2D(10.0, 10.0), 1.0e-9));

	// A range of 1 changes nothing.
	FMantlePlaceAssetConverter::ScaleTextureTransforms(Document, 0, 1.0);
	TestTrue(TEXT("unchanged by a range of 1"), TransformScale(Material->GetObjectField(TEXT("occlusionTexture"))).Equals(FVector2D(10.0, 10.0), 1.0e-9));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleBakeSurfaceLargestUvRepeatTest,
	"Mantle.Editor.BakeSurface.LandscapeMaterialLargestUvRepeat",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleBakeSurfaceLargestUvRepeatTest::RunTest(const FString& Parameters)
{
	using namespace MantleBakeSurfaceTestPrivate;

	// Base Color: a texture at Texture Coordinate x 0.1 (DowntownWestModularPack's Texture1_Tiling), Normal: one at plain UVs.
	UMaterial* Material = NewMaterial();
	UTexture2D* Texture = NewObject<UTexture2D>(GetTransientPackage(), NAME_None, RF_Transient);
	UMaterialExpressionTextureCoordinate* Coordinates = AddNode<UMaterialExpressionTextureCoordinate>(Material);
	UMaterialExpressionConstant* Tiling = AddNode<UMaterialExpressionConstant>(Material);
	Tiling->R = 0.1f;
	UMaterialExpressionMultiply* Scaled = AddNode<UMaterialExpressionMultiply>(Material);
	Scaled->A.Connect(0, Coordinates);
	Scaled->B.Connect(0, Tiling);
	UMaterialExpressionTextureSample* Colour = AddNode<UMaterialExpressionTextureSample>(Material);
	Colour->Texture = Texture;
	Colour->Coordinates.Connect(0, Scaled);
	Material->GetEditorOnlyData()->BaseColor.Connect(0, Colour);
	UMaterialExpressionTextureSample* Plain = AddNode<UMaterialExpressionTextureSample>(Material);
	Plain->Texture = Texture;
	Material->GetEditorOnlyData()->Normal.Connect(0, Plain);

	double Units = 0.0;
	TestTrue(TEXT("its repeat is read"), FMantleLandscapeMaterialReader::LargestUvRepeat(Material, 100.0, Units));
	TestEqual(TEXT("the coarsest texture repeats every 10 quads"), Units, 10.0, 1.0e-4);

	double None = -1.0;
	TestFalse(TEXT("a material with no texture says nothing"), FMantleLandscapeMaterialReader::LargestUvRepeat(NewMaterial(), 100.0, None));
	TestEqual(TEXT("and gives 0"), None, 0.0);
	return true;
}

// DowntownWestModularPack's roads map their textures by world position: baked once over each road's UVs, one bake was stretched across
// a whole road. A world-mapped material is now baked as one world tile and published worldAligned at that tile's size.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWorldTileBakeTest,
	"Mantle.Editor.BakeSurface.WorldMappedMaterialsBakeOneWorldTile",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWorldTileBakeTest::RunTest(const FString& Parameters)
{
	FMantleMaterialAnalysis WorldMapped;
	WorldMapped.Losses = EMantleMaterialLoss::WorldAligned;
	const FMantleBakeSurface Tile = FMantlePlaceAssetConverter::EffectiveBakeSurface(WorldMapped, FMantleBakeSurface());
	TestEqual(TEXT("a world-mapped material bakes one world tile"), Tile.WorldTileCm, FMantleBakeSurface::DefaultWorldTileCm);
	TestFalse(TEXT("which is not the plain surface"), Tile.IsPlain());
	TestTrue(TEXT("and keys its own cache entry"), Tile.CacheTag().Contains(TEXT("worldtile=")));

	FMantleMaterialAnalysis Kept = WorldMapped;
	Kept.Fidelity2.bWorldAligned = true;
	TestEqual(TEXT("one whose pattern is kept from its source textures does not"),
		FMantlePlaceAssetConverter::EffectiveBakeSurface(Kept, FMantleBakeSurface()).WorldTileCm, 0.0);
	FMantleBakeSurface Landscape;
	Landscape.UvRange = 16;
	TestEqual(TEXT("nor does a landscape layer, which keeps its UV range"),
		FMantlePlaceAssetConverter::EffectiveBakeSurface(WorldMapped, Landscape).WorldTileCm, 0.0);
	TestEqual(TEXT("a material mapped by UV keeps the plain surface"),
		FMantlePlaceAssetConverter::EffectiveBakeSurface(FMantleMaterialAnalysis(), FMantleBakeSurface()).WorldTileCm, 0.0);

	FMantlePlaceMaterialDocument Opaque;
	FMantlePlaceAssetConverter::ApplyWorldTile(Opaque, Tile);
	TestTrue(TEXT("an opaque world-tile bake publishes worldAligned"), Opaque.bWorldAligned);
	TestEqual(TEXT("at the tile's size"), Opaque.TileSizeCm, FMantleBakeSurface::DefaultWorldTileCm);
	FMantlePlaceMaterialDocument Glass;
	Glass.AlphaMode = TEXT("blend");
	FMantlePlaceAssetConverter::ApplyWorldTile(Glass, Tile);
	TestFalse(TEXT("a see-through one does not (the format's mapping is opaque only)"), Glass.bWorldAligned);
	FMantlePlaceMaterialDocument Plain;
	FMantlePlaceAssetConverter::ApplyWorldTile(Plain, FMantleBakeSurface());
	TestFalse(TEXT("nor does a plain bake"), Plain.bWorldAligned);
	return true;
}

// Many materials with world position somewhere in their graph map their textures by UV (Downtown's trim sheets, signs and brick walls):
// world-tiled, they came out as a grid of squares. A second bake on a moved card tells them apart by how much its base colour changes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWorldMappedDifferenceTest,
	"Mantle.Editor.BakeSurface.MovedCardDifferenceTellsWorldMapping",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWorldMappedDifferenceTest::RunTest(const FString& Parameters)
{
	TArray<FColor> Brick;
	TArray<FColor> Moved;
	for (int32 Index = 0; Index < 64; ++Index)
	{
		Brick.Add(FColor(uint8(Index * 4), 80, 40, 255));
		// A world-mapped pattern shifts under the card.
		Moved.Add(FColor(uint8(((Index + 23) % 64) * 4), 80, 40, 255));
	}
	TestEqual(TEXT("the same bake differs by nothing"), FMantlePlaceAssetConverter::MeanPixelDifference(Brick, Brick), 0.0);
	TestTrue(TEXT("a pattern that moved with the card is world-mapped"),
		FMantlePlaceAssetConverter::MeanPixelDifference(Brick, Moved) >= FMantlePlaceAssetConverter::WorldMappedDifference);
	TArray<FColor> Noise = Brick;
	Noise[3].R += 6;
	TestTrue(TEXT("a small world-space noise change is not"),
		FMantlePlaceAssetConverter::MeanPixelDifference(Brick, Noise) < FMantlePlaceAssetConverter::WorldMappedDifference);
	TArray<FColor> Smaller = Brick;
	Smaller.Pop();
	TestEqual(TEXT("images of different sizes count as different"), FMantlePlaceAssetConverter::MeanPixelDifference(Brick, Smaller), 1.0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
