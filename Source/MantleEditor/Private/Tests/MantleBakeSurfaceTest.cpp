// Copyright Joshua Eger. All Rights Reserved.
#include "MantleGlbWriter.h"
#include "MantleLandscapeMaterialReader.h"
#include "MantlePlaceAssetConverter.h"
#include "MantlePlaceAssets.h"
#include "MantlePng.h"

#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant4Vector.h"
#include "Materials/MaterialExpressionDivide.h"
#include "Materials/MaterialExpressionMakeMaterialAttributes.h"
#include "Materials/MaterialExpressionMax.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Misc/AutomationTest.h"
#include "Rendering/ColorVertexBuffer.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshComponentLODInfo.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// What a material is baked on (FMantleBakeSurface). GLTFExporter's own card has white vertex colors, so a road whose puddles are painted
// in Vertex Color's blue (DowntownWestModularPack's MI_Asphault_A) baked all puddle: a black mirror. Gather now averages the colors of the
// meshes drawing a material (a component's painted ones where it has them), the converter bakes a material that reads vertex color at that
// color, and the cache keys on it. A landscape's own material is baked over many quads, its texture transforms scaled back to one tile.
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

	/** Roughness is Vertex Color's blue: a puddle mask, which the bake must see the meshes' color for. */
	UMaterial* NewPuddleMaterial()
	{
		UMaterial* Material = NewMaterial();
		UMaterialExpressionVertexColor* Color = AddNode<UMaterialExpressionVertexColor>(Material);
		Material->GetEditorOnlyData()->Roughness.Connect(3, Color);
		return Material;
	}

	/** A quad of 4 vertices drawing Material in its one slot; every corner Color when given, else no vertex colors at all. */
	UStaticMesh* NewQuad(UMaterialInterface* Material, const FColor* Color)
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
			Attributes.GetVertexInstanceColors()[Instance] = Color ? FVector4f(FLinearColor(*Color)) : FVector4f(1.0f, 1.0f, 1.0f, 1.0f);
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
	FMantleBakeSurfaceGatherAveragesVertexColorsTest,
	"Mantle.Editor.BakeSurface.GatherTakesTheTypicalVertexColor",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleBakeSurfaceGatherAveragesVertexColorsTest::RunTest(const FString& Parameters)
{
	using namespace MantleBakeSurfaceTestPrivate;

	// The roads: a mesh with no colors of its own, painted on each component.
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
			TestEqual(TEXT("its bake color is the painted one, not the mesh's none"), Gathered->TypicalVertexColor().ToHex(), Puddles.ToHex());
			TestFalse(TEXT("which is not the plain surface"), Gathered->BakeSurface().IsPlain());
		}
	}

	// A4: the color most of the surface has, each channel's median, not the mean. Three unpainted roads (drawn white) and one painted
	// blue: most of the surface is white, so the bake is white, where the mean (a little blue everywhere) was a color no road has.
	const FColor Blue(0, 0, 255, 255);
	{
		FMantlePlaceDependencies Dependencies;
		Dependencies.AddComponent(NewComponent(Plane, &Blue), TEXT("Road"));
		Dependencies.AddComponent(NewComponent(Plane, nullptr), TEXT("Road2"));
		Dependencies.AddComponent(NewComponent(Plane, nullptr), TEXT("Road3"));
		Dependencies.AddComponent(NewComponent(Plane, nullptr), TEXT("Road4"));
		const FMantleGatheredMaterial* Gathered = Find(Dependencies, Asphalt);
		if (TestNotNull(TEXT("gathered"), Gathered))
		{
			TestEqual(TEXT("three quarters white: white"), Gathered->TypicalVertexColor().ToHex(), FColor::White.ToHex());
		}
	}
	// Half and half: a tie goes to each channel's lower value.
	{
		FMantlePlaceDependencies Dependencies;
		Dependencies.AddComponent(NewComponent(Plane, &Blue), TEXT("Road"));
		Dependencies.AddComponent(NewComponent(Plane, nullptr), TEXT("Road2"));
		const FMantleGatheredMaterial* Gathered = Find(Dependencies, Asphalt);
		if (TestNotNull(TEXT("gathered"), Gathered))
		{
			TestEqual(TEXT("half white, half blue: the lower of each channel"), Gathered->TypicalVertexColor().ToHex(), Blue.ToHex());
		}
	}

	// Nobody painted, and the mesh has none: white, which is Unreal's and today's bake, so nothing changes.
	{
		FMantlePlaceDependencies Dependencies;
		Dependencies.AddComponent(NewComponent(Plane, nullptr), TEXT("Road"));
		const FMantleGatheredMaterial* Gathered = Find(Dependencies, Asphalt);
		if (TestNotNull(TEXT("gathered"), Gathered))
		{
			TestTrue(TEXT("white"), Gathered->TypicalVertexColor() == FColor::White);
			TestTrue(TEXT("the plain surface"), Gathered->BakeSurface().IsPlain());
		}
	}

	// A mesh with colors of its own, used by no component: the mesh's.
	{
		UMaterial* Painted = NewPuddleMaterial();
		const FColor Red(200, 0, 0, 255);
		UStaticMesh* Colored = NewQuad(Painted, &Red);
		if (TestNotNull(TEXT("the colored quad builds"), Colored))
		{
			FMantlePlaceDependencies Dependencies;
			Dependencies.AddMesh(Colored, TEXT("Script Assets"));
			const FMantleGatheredMaterial* Gathered = Find(Dependencies, Painted);
			if (TestNotNull(TEXT("gathered"), Gathered))
			{
				TestEqual(TEXT("the mesh's own color"), Gathered->TypicalVertexColor().ToHex(), Red.ToHex());
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleBakeSurfaceConverterKeepsColorForVertexColorMaterialsTest,
	"Mantle.Editor.BakeSurface.ConverterKeepsColorOnlyWhereRead",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleBakeSurfaceConverterKeepsColorForVertexColorMaterialsTest::RunTest(const FString& Parameters)
{
	using namespace MantleBakeSurfaceTestPrivate;

	UMaterial* Puddle = NewPuddleMaterial();
	const FMantleMaterialAnalysis Analysis = FMantlePlaceAssets::AnalyzeMaterial(Puddle);
	TestTrue(TEXT("a roughness from Vertex Color's blue is a vertex color loss"), Analysis.Has(EMantleMaterialLoss::VertexColor));

	FMantleBakeSurface Surface;
	Surface.VertexColor = FColor(10, 20, 40, 255);
	{
		TSharedRef<FSurfaceFlattener> Flattener = MakeShared<FSurfaceFlattener>();
		FMantlePlaceAssetConverter Converter(Flattener, FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("MantleBakeSurface")));
		FMantlePlaceAssetSet Set;
		TestTrue(TEXT("the puddle material converts"), Converter.ConvertMaterial(Puddle, Surface, Set));
		TestEqual(TEXT("the flattener ran once"), Flattener->Calls, 1);
		TestEqual(TEXT("baked at the meshes' color"), Flattener->Seen.VertexColor.ToHex(), Surface.VertexColor.ToHex());
	}
	{
		// A material that reads no vertex color bakes as it always has, whatever its meshes' colors.
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

	// The UV range is kept for any material: a landscape's own reads no vertex color.
	FMantleBakeSurface Tiled;
	Tiled.UvRange = 10;
	Tiled.VertexColor = FColor(10, 20, 40, 255);
	const FMantleBakeSurface Kept = FMantlePlaceAssetConverter::EffectiveBakeSurface(FMantlePlaceAssets::AnalyzeMaterial(NewMaterial()), Tiled);
	TestEqual(TEXT("the range is kept"), Kept.UvRange, 10.0);
	TestTrue(TEXT("the color is not"), Kept.VertexColor == FColor::White);
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
	TestNotEqual(TEXT("a vertex color changes the key"), Salt(Puddles), Salt(Plain));
	TestNotEqual(TEXT("a different color is a different key"), Salt(Puddles), Salt(Dry));
	TestNotEqual(TEXT("a UV range changes the key"), Salt(Tiled), Salt(Plain));
	TestEqual(TEXT("the same color is the same key"), Salt(Puddles), Salt(Puddles));

	// #168 changed only how a masked material converts (no cut-off of 0), so only a masked material's key moves.
	UMaterial* Leaf = NewPuddleMaterial();
	Leaf->BlendMode = BLEND_Masked;
	const FString LeafSalt = FMantlePlaceAssetConverter::MaterialCacheSalt(TEXT("flattener"), Settings, Limits, Leaf, false, Plain);
	TestTrue(TEXT("a masked material's key says its cut-off is never 0"), LeafSalt.EndsWith(TEXT(" maskcutoff=2")));
	TestFalse(TEXT("an opaque material's key has no cut-off part"), Salt(Plain).Contains(TEXT("maskcutoff")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleBakeSurfaceScaleTransformsTest,
	"Mantle.Editor.BakeSurface.UvRangeScalesTextureTransforms",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleBakeSurfaceScaleTransformsTest::RunTest(const FString& Parameters)
{
	using namespace MantleBakeSurfaceTestPrivate;

	// What GLTFExporter writes for a bake over UVs 0 to 10: the baked base color covers the card, scale 1/10; a normal map it kept as
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
	FMantleBakeSurfaceFinestUvRepeatTest,
	"Mantle.Editor.BakeSurface.LandscapeMaterialFinestUvRepeat",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleBakeSurfaceFinestUvRepeatTest::RunTest(const FString& Parameters)
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
	UMaterialExpressionTextureSample* Color = AddNode<UMaterialExpressionTextureSample>(Material);
	Color->Texture = Texture;
	Color->Coordinates.Connect(0, Scaled);
	Material->GetEditorOnlyData()->BaseColor.Connect(0, Color);
	UMaterialExpressionTextureSample* Plain = AddNode<UMaterialExpressionTextureSample>(Material);
	Plain->Texture = Texture;
	Material->GetEditorOnlyData()->Normal.Connect(0, Plain);

	double Units = 0.0;
	TestTrue(TEXT("its repeat is read"), FMantleLandscapeMaterialReader::FinestUvRepeat(Material, 100.0, Units));
	TestEqual(TEXT("the base color decides: it repeats every 10 quads, though the normal's texture repeats every quad"), Units, 10.0, 1.0e-4);

	// Only a normal map is read when the base color has no texture.
	UMaterial* NormalOnly = NewMaterial();
	UMaterialExpressionTextureSample* OnlyNormal = AddNode<UMaterialExpressionTextureSample>(NormalOnly);
	OnlyNormal->Texture = Texture;
	NormalOnly->GetEditorOnlyData()->Normal.Connect(0, OnlyNormal);
	double NormalUnits = 0.0;
	TestTrue(TEXT("a lone normal map's repeat is read"), FMantleLandscapeMaterialReader::FinestUvRepeat(NormalOnly, 100.0, NormalUnits));
	TestEqual(TEXT("it repeats every quad"), NormalUnits, 1.0, 1.0e-4);

	// Only the texture tiled by 0.1: it repeats every 10 quads, and is the finest there is.
	UMaterial* Tiled = NewMaterial();
	UMaterialExpressionTextureCoordinate* TiledCoordinates = AddNode<UMaterialExpressionTextureCoordinate>(Tiled);
	UMaterialExpressionConstant* TiledScale = AddNode<UMaterialExpressionConstant>(Tiled);
	TiledScale->R = 0.1f;
	UMaterialExpressionMultiply* TiledMultiply = AddNode<UMaterialExpressionMultiply>(Tiled);
	TiledMultiply->A.Connect(0, TiledCoordinates);
	TiledMultiply->B.Connect(0, TiledScale);
	UMaterialExpressionTextureSample* TiledColor = AddNode<UMaterialExpressionTextureSample>(Tiled);
	TiledColor->Texture = Texture;
	TiledColor->Coordinates.Connect(0, TiledMultiply);
	Tiled->GetEditorOnlyData()->BaseColor.Connect(0, TiledColor);
	double TenUnits = 0.0;
	TestTrue(TEXT("a lone tiled texture's repeat is read"), FMantleLandscapeMaterialReader::FinestUvRepeat(Tiled, 100.0, TenUnits));
	TestEqual(TEXT("it repeats every 10 quads"), TenUnits, 10.0, 1.0e-4);

	// The same texture mapped by world position, divided by 1135 cm: its repeat is 1135 cm, on 60 cm quads 18.9 of them. An unscaled
	// world position (1 cm) is no information and does not make the finest 1 cm.
	UMaterial* World = NewMaterial();
	UMaterialExpressionWorldPosition* Position = AddNode<UMaterialExpressionWorldPosition>(World);
	UMaterialExpressionConstant* WorldScale = AddNode<UMaterialExpressionConstant>(World);
	WorldScale->R = 1135.0f;
	UMaterialExpressionDivide* WorldDivide = AddNode<UMaterialExpressionDivide>(World);
	WorldDivide->A.Connect(0, Position);
	WorldDivide->B.Connect(0, WorldScale);
	UMaterialExpressionTextureSample* WorldColor = AddNode<UMaterialExpressionTextureSample>(World);
	WorldColor->Texture = Texture;
	WorldColor->Coordinates.Connect(0, WorldDivide);
	World->GetEditorOnlyData()->BaseColor.Connect(0, WorldColor);
	UMaterialExpressionTextureSample* Unscaled = AddNode<UMaterialExpressionTextureSample>(World);
	Unscaled->Texture = Texture;
	Unscaled->Coordinates.Connect(0, Position);
	World->GetEditorOnlyData()->Normal.Connect(0, Unscaled);
	double WorldUnits = 0.0;
	TestTrue(TEXT("a world-mapped texture's repeat is read"), FMantleLandscapeMaterialReader::FinestUvRepeat(World, 60.0, WorldUnits));
	TestEqual(TEXT("1135 cm on 60 cm quads"), WorldUnits, 1135.0 / 60.0, 1.0e-4);

	double None = -1.0;
	TestFalse(TEXT("a material with no texture says nothing"), FMantleLandscapeMaterialReader::FinestUvRepeat(NewMaterial(), 100.0, None));
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
// world-tiled, they came out as a grid of squares. A second bake on a moved card tells them apart by how much its base color changes.
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleBakeSurfaceUvRangeReadsCardTest,
	"Mantle.Editor.BakeSurface.UvRangeBakeReadsTheCard",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleBakeSurfaceUvRangeReadsCardTest::RunTest(const FString& Parameters)
{
	using namespace MantleBakeSurfaceTestPrivate;

	// HellCage's ground: its landscape material reads only texture coordinates, so GLTFExporter (FGLTFMaterialUtilities::NeedsMeshData)
	// baked it over UVs 0 to 1, not the card's 0 to 12: one landscape quad, big blurry stones, stretched over each 12 m tile. The bake now
	// runs on a copy whose base color is multiplied by max(Vertex Color, 1), which reads mesh data and changes nothing else.
	// Whether a Vertex Color node feeds the base color (or the attributes). The engine's own analysis (AnalyzeMaterialProperty) needs a
	// material resource a bare transient material does not have in a -nullrhi run, so the graph is walked instead.
	TFunction<bool(const UMaterialExpression*, TSet<const UMaterialExpression*>&)> Reaches;
	Reaches = [&Reaches](const UMaterialExpression* Node, TSet<const UMaterialExpression*>& Seen)
	{
		if (!Node || Seen.Contains(Node))
		{
			return false;
		}
		Seen.Add(Node);
		if (Node->IsA<UMaterialExpressionVertexColor>())
		{
			return true;
		}
		for (int32 Index = 0; const FExpressionInput* Input = const_cast<UMaterialExpression*>(Node)->GetInput(Index); ++Index)
		{
			if (Reaches(Input->Expression, Seen))
			{
				return true;
			}
		}
		return false;
	};
	auto ReadsVertexData = [&Reaches](UMaterialInterface* Material)
	{
		const UMaterial* Base = Material->GetMaterial();
		const UMaterialEditorOnlyData* Data = Base ? Base->GetEditorOnlyData() : nullptr;
		if (!Data)
		{
			return false;
		}
		TSet<const UMaterialExpression*> Seen;
		return Reaches(Base->bUseMaterialAttributes ? Data->MaterialAttributes.Expression : Data->BaseColor.Expression, Seen);
	};

	// Base color: the red of the texture coordinates, scaled (a texture read at a landscape's tiling, without the texture).
	UMaterial* Ground = NewMaterial();
	UMaterialExpressionTextureCoordinate* Coordinates = AddNode<UMaterialExpressionTextureCoordinate>(Ground);
	UMaterialExpressionComponentMask* Red = AddNode<UMaterialExpressionComponentMask>(Ground);
	Red->Input.Connect(0, Coordinates);
	Red->R = true;
	UMaterialExpressionMultiply* Scaled = AddNode<UMaterialExpressionMultiply>(Ground);
	Scaled->A.Connect(0, Red);
	Scaled->ConstB = 0.2f;
	Ground->GetEditorOnlyData()->BaseColor.Connect(0, Scaled);
	Ground->PostEditChange();
	TestFalse(TEXT("the ground reads no mesh data, so GLTFExporter would bake it over UVs 0 to 1"), ReadsVertexData(Ground));

	const TStrongObjectPtr<UMaterialInterface> Copy = FMantlePlaceAssets::ReadsVertexColorCopy(Ground);
	if (!TestTrue(TEXT("a copy is made"), Copy.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("the copy reads mesh data, so it bakes on the card's UVs"), ReadsVertexData(Copy.Get()));
	TestNotEqual(TEXT("it is a copy"), static_cast<UMaterialInterface*>(Ground), Copy.Get());
	TestFalse(TEXT("the source still reads none"), ReadsVertexData(Ground));
	const UMaterial* CopyBase = Copy->GetMaterial();
	const UMaterialExpressionMultiply* Times = CopyBase ? Cast<UMaterialExpressionMultiply>(CopyBase->GetEditorOnlyData()->BaseColor.Expression) : nullptr;
	if (TestNotNull(TEXT("the copy's base color is a multiply"), Times))
	{
		TestTrue(TEXT("of the source's base color"), Times->A.Expression && Times->A.Expression->IsA<UMaterialExpressionMultiply>() && Times->A.OutputIndex == 0);
		const UMaterialExpressionMax* One = Cast<UMaterialExpressionMax>(Times->B.Expression);
		TestTrue(TEXT("by max(Vertex Color, 1)"), One && One->A.Expression && One->A.Expression->IsA<UMaterialExpressionVertexColor>() && One->ConstB == 1.0f
			&& !One->B.Expression);
		// The red output (1) is a scalar, so the multiply compiles against a base color of any width; the RGB output (0) is a float3 and
		// fails against a float2 or float4.
		TestTrue(TEXT("whose Vertex Color side is the scalar red output"), One && One->A.OutputIndex == 1);
	}

	// A base color that is not three wide: a texture coordinate (float2) and a constant (float4) wired straight in, no mask.
	{
		UMaterial* Wide = NewMaterial();
		UMaterialExpressionConstant4Vector* Four = AddNode<UMaterialExpressionConstant4Vector>(Wide);
		Wide->GetEditorOnlyData()->BaseColor.Connect(0, Four);
		Wide->PostEditChange();
		UMaterial* Narrow = NewMaterial();
		UMaterialExpressionTextureCoordinate* Straight = AddNode<UMaterialExpressionTextureCoordinate>(Narrow);
		Narrow->GetEditorOnlyData()->BaseColor.Connect(0, Straight);
		Narrow->PostEditChange();
		for (UMaterial* Source : {Wide, Narrow})
		{
			const TStrongObjectPtr<UMaterialInterface> Made = FMantlePlaceAssets::ReadsVertexColorCopy(Source);
			if (!TestTrue(TEXT("a copy is made of a base color that is not a float3"), Made.IsValid()))
			{
				continue;
			}
			TestTrue(TEXT("it reads mesh data"), ReadsVertexData(Made.Get()));
			const UMaterial* MadeBase = Made->GetMaterial();
			const UMaterialExpressionMultiply* Product = MadeBase ? Cast<UMaterialExpressionMultiply>(MadeBase->GetEditorOnlyData()->BaseColor.Expression) : nullptr;
			const UMaterialExpressionMax* Floor = Product ? Cast<UMaterialExpressionMax>(Product->B.Expression) : nullptr;
			TestTrue(TEXT("and multiplies by a scalar, so it compiles at any width"), Floor && Floor->A.OutputIndex == 1
				&& Floor->A.Expression && Floor->A.Expression->IsA<UMaterialExpressionVertexColor>());
		}
	}

	// Material attributes: the base color inside them is multiplied.
	UMaterial* Attributes = NewMaterial();
	Attributes->bUseMaterialAttributes = true;
	UMaterialExpressionTextureCoordinate* AttributeCoordinates = AddNode<UMaterialExpressionTextureCoordinate>(Attributes);
	UMaterialExpressionComponentMask* AttributeRed = AddNode<UMaterialExpressionComponentMask>(Attributes);
	AttributeRed->Input.Connect(0, AttributeCoordinates);
	AttributeRed->R = true;
	UMaterialExpressionMakeMaterialAttributes* Make = AddNode<UMaterialExpressionMakeMaterialAttributes>(Attributes);
	Make->BaseColor.Connect(0, AttributeRed);
	Attributes->GetEditorOnlyData()->MaterialAttributes.Connect(0, Make);
	Attributes->PostEditChange();
	TestFalse(TEXT("attributes reading only texture coordinates read no mesh data"), ReadsVertexData(Attributes));
	const TStrongObjectPtr<UMaterialInterface> AttributesCopy = FMantlePlaceAssets::ReadsVertexColorCopy(Attributes);
	if (TestTrue(TEXT("an attributes copy is made"), AttributesCopy.IsValid()))
	{
		TestTrue(TEXT("its base color reads mesh data"), ReadsVertexData(AttributesCopy.Get()));
	}

	TestFalse(TEXT("nothing connected: no copy"), FMantlePlaceAssets::ReadsVertexColorCopy(NewMaterial()).IsValid());

	FMantleBakeSurface Tiled;
	Tiled.UvRange = 12;
	TestTrue(TEXT("a UV range bake keys apart from the earlier ones, which stretched one quad"), Tiled.CacheTag().Contains(TEXT(" meshdata")));

	// A landscape layer's bake: a card as many cm a side as the layer tiles, over a range that is not a whole number of quads.
	FMantleBakeSurface Layer;
	Layer.UvRange = 18.9167;
	Layer.WorldTileCm = 1135.0;
	Layer.bTerrainLayer = true;
	TestFalse(TEXT("a landscape layer's bake is not the plain surface"), Layer.IsPlain());
	TestTrue(TEXT("it covers terrain"), Layer.CoversTerrain());
	TestTrue(TEXT("its key says the card has its own size"), Layer.CacheTag().Contains(TEXT("terrain=")) && Layer.CacheTag().Contains(TEXT("worldtile=1135")));
	FMantleBakeSurface Unit = Layer;
	Unit.UvRange = 1.0;
	TestTrue(TEXT("a layer over a single quad still covers terrain, so its material still reads the card"), Unit.CoversTerrain() && Unit.CacheTag().Contains(TEXT(" meshdata")));
	Tiled.UvRange = Layer.UvRange;
	TestNotEqual(TEXT("the same range as a plain UV range bake keys apart"), Layer.CacheTag(), Tiled.CacheTag());
	FMantlePlaceMaterialDocument LayerDocument;
	FMantlePlaceAssetConverter::ApplyWorldTile(LayerDocument, Layer);
	TestFalse(TEXT("a landscape layer is tiled by the Terrain, not published worldAligned"), LayerDocument.bWorldAligned);
	return true;
}

// City Sample (#189): the world-mapping check baked the whole material a second time (every input, its images written and read back) to
// compare one input: 81 of 238 minutes of material conversion. It now bakes the base color alone, on both cards in one bake, at the size
// of the first bake's base color image, and the whole second bake is only for a flattener that cannot.
namespace MantleBakeSurfaceTestPrivate
{
	/** Base color is World Position: a world-mapped material by the analysis. */
	UMaterial* NewWorldPositionMaterial()
	{
		UMaterial* Material = NewMaterial();
		Material->GetEditorOnlyData()->BaseColor.Connect(0, AddNode<UMaterialExpressionWorldPosition>(Material));
		return Material;
	}

	/** A first bake whose base color is an 8x4 image, and scripted base-color-only bakes. */
	class FMovedCardFlattener : public IMantleMaterialFlattener
	{
	public:
		bool bCanBakeBaseColors = true;
		bool bPatternMoves = false;
		int32 FlattenCalls = 0;
		int32 BaseColorCalls = 0;
		TArray<double> Shifts;
		FIntPoint Size = FIntPoint::ZeroValue;

		static TArray<FColor> Pattern(int32 Offset)
		{
			TArray<FColor> Pixels;
			for (int32 Index = 0; Index < 32; ++Index)
			{
				Pixels.Add(FColor(uint8(((Index + Offset) % 32) * 8), 90, 30, 255));
			}
			return Pixels;
		}

		virtual bool Flatten(UMaterialInterface* Material, const FMantleFlattenSettings& Settings, FMantleGlbDocument& OutDocument,
			TArray<FString>& OutMessages, FString& OutError) override
		{
			++FlattenCalls;
			// The whole second bake (the fallback) sees the card moved, so a moving pattern moves there too.
			const TArray<uint8> Png = FMantlePng::EncodeRGBA8(8, 4, Pattern(bPatternMoves && Settings.Surface.WorldShiftCm > 0.0 ? 11 : 0));
			const TSharedRef<FJsonObject> Info = MakeShared<FJsonObject>();
			Info->SetNumberField(TEXT("index"), 0);
			const TSharedRef<FJsonObject> Pbr = MakeShared<FJsonObject>();
			Pbr->SetObjectField(TEXT("baseColorTexture"), Info);
			Pbr->SetNumberField(TEXT("metallicFactor"), 0.0);
			Pbr->SetNumberField(TEXT("roughnessFactor"), 0.5);
			const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("alphaMode"), TEXT("OPAQUE"));
			Entry->SetObjectField(TEXT("pbrMetallicRoughness"), Pbr);
			const TSharedRef<FJsonObject> Texture = MakeShared<FJsonObject>();
			Texture->SetNumberField(TEXT("source"), 0);
			const TSharedRef<FJsonObject> Image = MakeShared<FJsonObject>();
			Image->SetNumberField(TEXT("bufferView"), 0);
			Image->SetStringField(TEXT("mimeType"), TEXT("image/png"));
			const TSharedRef<FJsonObject> View = MakeShared<FJsonObject>();
			View->SetNumberField(TEXT("buffer"), 0);
			View->SetNumberField(TEXT("byteOffset"), 0);
			View->SetNumberField(TEXT("byteLength"), Png.Num());
			const TSharedRef<FJsonObject> Buffer = MakeShared<FJsonObject>();
			Buffer->SetNumberField(TEXT("byteLength"), Png.Num());
			OutDocument = FMantleGlbDocument();
			OutDocument.Json = MakeShared<FJsonObject>();
			OutDocument.Json->SetArrayField(TEXT("materials"), { MakeShared<FJsonValueObject>(Entry) });
			OutDocument.Json->SetArrayField(TEXT("textures"), { MakeShared<FJsonValueObject>(Texture) });
			OutDocument.Json->SetArrayField(TEXT("images"), { MakeShared<FJsonValueObject>(Image) });
			OutDocument.Json->SetArrayField(TEXT("bufferViews"), { MakeShared<FJsonValueObject>(View) });
			OutDocument.Json->SetArrayField(TEXT("buffers"), { MakeShared<FJsonValueObject>(Buffer) });
			OutDocument.Bin = Png;
			return true;
		}

		virtual bool BakeBaseColors(UMaterialInterface* Material, const FMantleFlattenSettings& Settings, TConstArrayView<double> InShiftsCm, FIntPoint InSize,
			TArray<TArray<FColor>>& OutImages) override
		{
			++BaseColorCalls;
			Shifts = TArray<double>(InShiftsCm.GetData(), InShiftsCm.Num());
			Size = InSize;
			if (!bCanBakeBaseColors)
			{
				return false;
			}
			for (const double Shift : InShiftsCm)
			{
				OutImages.Add(Pattern(bPatternMoves && Shift > 0.0 ? 11 : 0));
			}
			return true;
		}

		virtual FString Describe() const override { return TEXT("scripted moved card"); }
	};

	/** Converts NewWorldPositionMaterial with Flattener; whether the published document is worldAligned. */
	bool ConvertWorldPosition(FAutomationTestBase& Test, const TSharedRef<FMovedCardFlattener>& Flattener)
	{
		FMantlePlaceAssetConverter Converter(Flattener, FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("MantleMovedCard"), FGuid::NewGuid().ToString()));
		FMantlePlaceAssetSet Set;
		Test.TestTrue(TEXT("the world-position material converts"), Converter.ConvertMaterial(NewWorldPositionMaterial(), FMantleBakeSurface(), Set));
		return Test.TestEqual(TEXT("one material"), Set.Materials.Num(), 1) && Set.Materials[0].Document.bWorldAligned;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMovedCardBakesBaseColorAloneTest,
	"Mantle.Editor.BakeSurface.MovedCardBakesBaseColorAlone",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMovedCardBakesBaseColorAloneTest::RunTest(const FString& Parameters)
{
	using namespace MantleBakeSurfaceTestPrivate;
	TestTrue(TEXT("World Position into the base color is world-mapped by the analysis"),
		FMantlePlaceAssets::AnalyzeMaterial(NewWorldPositionMaterial()).Has(EMantleMaterialLoss::WorldAligned));

	{
		// A pattern that moves with the card: world-mapped, from the base color alone.
		TSharedRef<FMovedCardFlattener> Flattener = MakeShared<FMovedCardFlattener>();
		Flattener->bPatternMoves = true;
		TestTrue(TEXT("a moving base color publishes worldAligned"), ConvertWorldPosition(*this, Flattener));
		TestEqual(TEXT("the whole material is baked once"), Flattener->FlattenCalls, 1);
		TestEqual(TEXT("the base color is baked alone once, for both cards"), Flattener->BaseColorCalls, 1);
		if (TestEqual(TEXT("two cards"), Flattener->Shifts.Num(), 2))
		{
			TestEqual(TEXT("the first where the whole bake's card was"), Flattener->Shifts[0], 0.0);
			TestEqual(TEXT("the second moved a share of the tile"), Flattener->Shifts[1],
				FMantleBakeSurface::DefaultWorldTileCm * FMantlePlaceAssetConverter::WorldShiftFraction, 1.0e-9);
		}
		TestEqual(TEXT("at the first bake's base color size"), Flattener->Size, FIntPoint(8, 4));
	}
	{
		// One that stays put: mapped by UV, still one whole bake.
		TSharedRef<FMovedCardFlattener> Flattener = MakeShared<FMovedCardFlattener>();
		TestFalse(TEXT("a still base color stays on the mesh's UVs"), ConvertWorldPosition(*this, Flattener));
		TestEqual(TEXT("the whole material is baked once"), Flattener->FlattenCalls, 1);
	}
	{
		// A flattener that cannot bake the base color alone gets the whole second bake, with the same answer.
		TSharedRef<FMovedCardFlattener> Flattener = MakeShared<FMovedCardFlattener>();
		Flattener->bCanBakeBaseColors = false;
		Flattener->bPatternMoves = true;
		TestTrue(TEXT("the whole second bake finds it world-mapped too"), ConvertWorldPosition(*this, Flattener));
		TestEqual(TEXT("the whole material is baked twice"), Flattener->FlattenCalls, 2);
	}

	// The first bake's base color image size is read from its header; a constant base color has none to compare.
	FMantleGlbDocument Document;
	TArray<FString> Messages;
	FString Error;
	FMovedCardFlattener().Flatten(nullptr, FMantleFlattenSettings(), Document, Messages, Error);
	FIntPoint Size;
	bool bConstant = true;
	TestTrue(TEXT("the base color image's size is read"), FMantlePlaceAssetConverter::BakedBaseColorSize(Document, Size, bConstant));
	TestEqual(TEXT("8 by 4"), Size, FIntPoint(8, 4));
	TestFalse(TEXT("which is not a constant"), bConstant);
	FMantleGlbDocument Flat;
	Flat.Json = MakeShared<FJsonObject>();
	const TSharedRef<FJsonObject> FlatMaterial = MakeShared<FJsonObject>();
	FlatMaterial->SetObjectField(TEXT("pbrMetallicRoughness"), MakeShared<FJsonObject>());
	Flat.Json->SetArrayField(TEXT("materials"), { MakeShared<FJsonValueObject>(FlatMaterial) });
	TestFalse(TEXT("a constant base color has no image"), FMantlePlaceAssetConverter::BakedBaseColorSize(Flat, Size, bConstant));
	TestTrue(TEXT("and says it is constant"), bConstant);
	TestFalse(TEXT("an empty bake has none"), FMantlePlaceAssetConverter::BakedBaseColorSize(FMantleGlbDocument(), Size, bConstant));
	TestFalse(TEXT("and is not called constant"), bConstant);

	// The bake shrinks a one-color image to one pixel: that does not move.
	const TArray<FColor> Still = FMovedCardFlattener::Pattern(0);
	TestEqual(TEXT("a shrunk one-color bake did not move"), FMantlePlaceAssetConverter::MovedCardDifference({ FColor::Red }, Still), 0.0);
	TestTrue(TEXT("a moved pattern did"), FMantlePlaceAssetConverter::MovedCardDifference(Still, FMovedCardFlattener::Pattern(11))
		>= FMantlePlaceAssetConverter::WorldMappedDifference);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
