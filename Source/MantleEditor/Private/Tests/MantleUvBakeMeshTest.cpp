// Copyright Joshua Eger. All Rights Reserved.
#include "MantleChassisSpec.h"
#include "MantleGlbWriter.h"
#include "MantlePlaceAssetConverter.h"
#include "MantlePlaceAssets.h"
#include "MantleVehicleAsset.h"
#include "MantleVehicleChassisBuilder.h"
#include "MantleVehicleChassisMesh.h"
#include "MantleVehicleConverter.h"
#include "MantleVehicleScratchBody.h"
#include "MantleVehicleSource.h"

#include "Dom/JsonObject.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Misc/ScopeExit.h"
#include "StaticMeshAttributes.h"
#include "Dom/JsonValue.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// SUV04's lamp lenses pick each zone's color (black plastic, red, orange, white, chrome) from a mask read at UV1. The bake drew them on a
// card whose UV1 is its UV0, so the zones landed in the wrong places (45% of the lens, measured against the mask), which showed as white
// and chrome triangles in game; and the lenses' own UV0 covers the same texels many times (tiled and mirrored), so even a bake over the
// mesh laid out by UV0 left 12% wrong. Such a section is now baked over its own triangles, laid out apart by Unreal's lightmap packer, and
// published with that layout as its UV0.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleUvBakeMeshTestPrivate
{
	template <typename TExpression>
	TExpression* AddNode(UMaterial* Material)
	{
		TExpression* Expression = NewObject<TExpression>(Material);
		Expression->Material = Material;
		Material->GetExpressionCollection().AddExpression(Expression);
		return Expression;
	}

	const FLinearColor Grey(0.5f, 0.5f, 0.5f, 1.0f);
	const FLinearColor Yellow(0.9f, 0.75f, 0.05f, 1.0f);

	/** Base color = Lerp(grey, yellow, UV1's U): what UV1 places, as SUV04's lens zones are. */
	UMaterial* NewUv1MaskMaterial()
	{
		UMaterial* Material = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
		UMaterialExpressionConstant3Vector* A = AddNode<UMaterialExpressionConstant3Vector>(Material);
		A->Constant = Grey;
		UMaterialExpressionConstant3Vector* B = AddNode<UMaterialExpressionConstant3Vector>(Material);
		B->Constant = Yellow;
		UMaterialExpressionTextureCoordinate* Uv1 = AddNode<UMaterialExpressionTextureCoordinate>(Material);
		Uv1->CoordinateIndex = 1;
		UMaterialExpressionComponentMask* U = AddNode<UMaterialExpressionComponentMask>(Material);
		U->R = true;
		U->G = false;
		U->B = false;
		U->A = false;
		U->Input.Connect(0, Uv1);
		UMaterialExpressionLinearInterpolate* Lerp = AddNode<UMaterialExpressionLinearInterpolate>(Material);
		Lerp->A.Connect(0, A);
		Lerp->B.Connect(0, B);
		Lerp->Alpha.Connect(0, U);
		Material->GetEditorOnlyData()->BaseColor.Connect(0, Lerp);
		return Material;
	}

	/**
	 * A mesh of two sections. Section 0: two quads 2 m apart with the same UV0 (0 to 1, as mirrored or tiled islands share it), one at
	 * UV1 U = 0.1 (grey), the other at UV1 U = 0.9 (yellow). Section 1: one more quad, which a layout of section 0 must leave alone.
	 */
	FMantleGlbMesh NewTwoSectionMesh()
	{
		FMantleGlbMesh Mesh;
		Mesh.Name = TEXT("UvBakeTest");
		Mesh.MaterialNames = { TEXT("Lens"), TEXT("Body") };
		auto AddQuad = [&Mesh](const int32 Material, const float X, const float Uv1U)
		{
			FMantleGlbPrimitive& Primitive = Mesh.Primitives.AddDefaulted_GetRef();
			Primitive.MaterialIndex = Material;
			Primitive.FirstVertex = Mesh.Positions.Num();
			Primitive.NumVertices = 4;
			for (int32 Corner = 0; Corner < 4; ++Corner)
			{
				const float U = static_cast<float>(Corner & 1);
				const float V = static_cast<float>(Corner >> 1);
				Mesh.Positions.Add(FVector3f(X + 100.0f * U, 100.0f * V, 0.0f));
				Mesh.Normals.Add(FVector3f(0.0f, 0.0f, 1.0f));
				Mesh.Tangents.Add(FVector4f(1.0f, 0.0f, 0.0f, 1.0f));
				Mesh.UVs.Add(FVector2f(U, V));
				Mesh.UV1s.Add(FVector2f(Uv1U, 0.5f));
			}
			Primitive.Indices = { 0, 2, 1, 2, 3, 1 };
		};
		AddQuad(0, 0.0f, 0.1f);
		AddQuad(0, 200.0f, 0.9f);
		AddQuad(1, 400.0f, 0.5f);
		return Mesh;
	}

	/** Where the quad of section 0 starting at vertex First sits in the layout: its center. */
	FVector2f QuadCenter(const FMantleBakeMesh& Mesh, const int32 First)
	{
		return (Mesh.Layout[First] + Mesh.Layout[First + 1] + Mesh.Layout[First + 2] + Mesh.Layout[First + 3]) * 0.25f;
	}

	/** The base color texture info of a flattened document's first material. */
	TSharedPtr<FJsonObject> BaseColorInfo(const FMantleGlbDocument& Document)
	{
		const TArray<TSharedPtr<FJsonValue>>* Materials = nullptr;
		const TSharedPtr<FJsonObject>* Pbr = nullptr;
		const TSharedPtr<FJsonObject>* Info = nullptr;
		if (Document.Json.IsValid() && Document.Json->TryGetArrayField(TEXT("materials"), Materials) && Materials->Num() > 0
			&& (*Materials)[0]->AsObject()->TryGetObjectField(TEXT("pbrMetallicRoughness"), Pbr) && (*Pbr)->TryGetObjectField(TEXT("baseColorTexture"), Info))
		{
			return *Info;
		}
		return nullptr;
	}

	/** The pixels of the image a texture info names, decoded; false when there is none. */
	bool DecodeImage(const FMantleGlbDocument& Document, const TSharedPtr<FJsonObject>& Info, TArray64<uint8>& OutRgba, int32& OutWidth, int32& OutHeight)
	{
		const TArray<TSharedPtr<FJsonValue>>* Textures = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Images = nullptr;
		if (!Info.IsValid() || !Document.Json->TryGetArrayField(TEXT("textures"), Textures) || !Document.Json->TryGetArrayField(TEXT("images"), Images))
		{
			return false;
		}
		const int32 Texture = static_cast<int32>(Info->GetNumberField(TEXT("index")));
		if (!Textures->IsValidIndex(Texture))
		{
			return false;
		}
		const int32 Image = static_cast<int32>((*Textures)[Texture]->AsObject()->GetNumberField(TEXT("source")));
		double View = -1.0;
		if (!Images->IsValidIndex(Image) || !(*Images)[Image]->AsObject()->TryGetNumberField(TEXT("bufferView"), View))
		{
			return false;
		}
		TArray<uint8> Png;
		if (!Document.ReadBufferView(static_cast<int32>(View), Png))
		{
			return false;
		}
		IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		TSharedPtr<IImageWrapper> Reader = Module.CreateImageWrapper(EImageFormat::PNG);
		if (!Reader.IsValid() || !Reader->SetCompressed(Png.GetData(), Png.Num()) || !Reader->GetRaw(ERGBFormat::RGBA, 8, OutRgba))
		{
			return false;
		}
		OutWidth = static_cast<int32>(Reader->GetWidth());
		OutHeight = static_cast<int32>(Reader->GetHeight());
		return true;
	}

	/** The texel a mesh UV reads, through the texture info's KHR_texture_transform (offset and scale; the bake writes no rotation). */
	FColor Sample(const TArray64<uint8>& Rgba, const int32 Width, const int32 Height, const TSharedPtr<FJsonObject>& Info, FVector2f Uv)
	{
		const TSharedPtr<FJsonObject>* Extensions = nullptr;
		const TSharedPtr<FJsonObject>* Transform = nullptr;
		if (Info->TryGetObjectField(TEXT("extensions"), Extensions) && (*Extensions)->TryGetObjectField(TEXT("KHR_texture_transform"), Transform))
		{
			const TArray<TSharedPtr<FJsonValue>>* Scale = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Offset = nullptr;
			if ((*Transform)->TryGetArrayField(TEXT("scale"), Scale) && Scale->Num() == 2)
			{
				Uv *= FVector2f(static_cast<float>((*Scale)[0]->AsNumber()), static_cast<float>((*Scale)[1]->AsNumber()));
			}
			if ((*Transform)->TryGetArrayField(TEXT("offset"), Offset) && Offset->Num() == 2)
			{
				Uv += FVector2f(static_cast<float>((*Offset)[0]->AsNumber()), static_cast<float>((*Offset)[1]->AsNumber()));
			}
		}
		const int32 X = FMath::Clamp(FMath::FloorToInt(Uv.X * Width), 0, Width - 1);
		const int32 Y = FMath::Clamp(FMath::FloorToInt(Uv.Y * Height), 0, Height - 1);
		const int64 At = (static_cast<int64>(Y) * Width + X) * 4;
		return FColor(Rgba[At], Rgba[At + 1], Rgba[At + 2], Rgba[At + 3]);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleUvBakeLayoutTest,
	"Mantle.Editor.UvBake.SectionLayoutKeepsSharedIslandsApart",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleUvBakeLayoutTest::RunTest(const FString& Parameters)
{
	using namespace MantleUvBakeMeshTestPrivate;
	const FMantleGlbMesh Mesh = NewTwoSectionMesh();
	const TSharedPtr<const FMantleBakeMesh> Bake = FMantleBakeMesh::FromSection(Mesh, 0);
	if (!TestTrue(TEXT("the section is laid out"), Bake.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("it holds the section's 8 vertices"), Bake->Positions.Num(), 8);
	TestEqual(TEXT("and its 4 triangles"), Bake->Indices.Num(), 12);
	TestEqual(TEXT("its UV1 as the mesh has it"), Bake->UV1s[4].X, 0.9f);

	// Both quads have UV0 0 to 1: any picture laid out by UV0 gives them one color. The layout puts them apart.
	TestTrue(TEXT("the two quads' UV0 is the same"), Mesh.UVs[0].Equals(Mesh.UVs[4]) && Mesh.UVs[3].Equals(Mesh.UVs[7]));
	TestTrue(TEXT("its UV0 overlaps itself (both quads on 0 to 1)"), FMantleBakeMesh::Overlap(Bake->UV0s, Bake->Indices) > FMantleBakeMesh::OverlapsItself);
	TestTrue(TEXT("so it gets a layout of its own"), Bake->bOwnLayout);
	TestTrue(TEXT("no texel of the layout is drawn twice"), Bake->LayoutOverlap() < 0.001);
	TestTrue(TEXT("the quads are apart in it"), FVector2f::Distance(QuadCenter(*Bake, 0), QuadCenter(*Bake, 4)) > 0.1f);
	bool bInside = true;
	for (const FVector2f& Uv : Bake->Layout)
	{
		bInside = bInside && Uv.X >= 0.0f && Uv.X <= 1.0f && Uv.Y >= 0.0f && Uv.Y <= 1.0f;
	}
	TestTrue(TEXT("every layout UV is within 0 to 1"), bInside);

	// The same mesh gives the same layout and key, so gather and the converter agree.
	const TSharedPtr<const FMantleBakeMesh> Again = FMantleBakeMesh::FromSection(Mesh, 0);
	TestTrue(TEXT("laid out again it is the same"), Again.IsValid() && Again->Hash == Bake->Hash && Again->Layout == Bake->Layout);
	FMantleGlbMesh Moved = Mesh;
	Moved.Positions[0].Z += 1.0f;
	const TSharedPtr<const FMantleBakeMesh> Other = FMantleBakeMesh::FromSection(Moved, 0);
	TestTrue(TEXT("another section's shape is another bake"), Other.IsValid() && Other->Hash != Bake->Hash);

	// Applied, only the section's own UVs change.
	FMantleGlbMesh Published = Mesh;
	Bake->ApplyLayout(Published, 0);
	TestTrue(TEXT("the section's UV0 is now the layout"), Published.UVs[0].Equals(Bake->Layout[0]) && Published.UVs[7].Equals(Bake->Layout[7]));
	TestTrue(TEXT("the other section keeps its UV0"), Published.UVs[8].Equals(Mesh.UVs[8]) && Published.UVs[11].Equals(Mesh.UVs[11]));
	TestTrue(TEXT("UV1 is untouched"), Published.UV1s == Mesh.UV1s);

	TestFalse(TEXT("a section with no triangles is not laid out"), FMantleBakeMesh::FromSection(Mesh, 5).IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleUvBakeBowTieTest,
	"Mantle.Editor.UvBake.BowTieVertexIsCopiedIntoEachIsland",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleUvBakeBowTieTest::RunTest(const FString& Parameters)
{
	using namespace MantleUvBakeMeshTestPrivate;
	// Two triangles that share only their middle vertex (no edge): two islands, which the packer places apart. Kept once, that vertex
	// would stretch one triangle across the layout (SUV04's lenses had 77 such corners): it is copied into the second island instead.
	// A third triangle on the first one's UV0 makes the section's UV0 overlap itself, so it gets a layout of its own.
	FMantleGlbMesh Mesh;
	Mesh.Name = TEXT("BowTie");
	Mesh.MaterialNames = { TEXT("Lens"), TEXT("Body") };
	const FVector3f Points[] = { { 0, 0, 0 }, { 100, 0, 0 }, { 50, 50, 0 }, { 0, 100, 0 }, { 100, 100, 0 }, { 0, 0, 50 }, { 100, 0, 50 }, { 50, 50, 50 } };
	const FVector2f Uvs[] = { { 0, 0 }, { 1, 0 }, { 0.5f, 0.5f }, { 0, 1 }, { 1, 1 }, { 0, 0 }, { 1, 0 }, { 0.5f, 0.5f } };
	FMantleGlbPrimitive& Lens = Mesh.Primitives.AddDefaulted_GetRef();
	Lens.MaterialIndex = 0;
	Lens.FirstVertex = 0;
	Lens.NumVertices = 8;
	Lens.Indices = { 0, 1, 2, 2, 4, 3, 5, 6, 7 };
	for (int32 Vertex = 0; Vertex < 8; ++Vertex)
	{
		Mesh.Positions.Add(Points[Vertex]);
		Mesh.Normals.Add(FVector3f(0.0f, 0.0f, 1.0f));
		Mesh.UVs.Add(Uvs[Vertex]);
		Mesh.UV1s.Add(Uvs[Vertex]);
	}
	FMantleGlbPrimitive& Body = Mesh.Primitives.AddDefaulted_GetRef();
	Body.MaterialIndex = 1;
	Body.FirstVertex = 8;
	Body.NumVertices = 3;
	Body.Indices = { 0, 1, 2 };
	for (int32 Vertex = 0; Vertex < 3; ++Vertex)
	{
		Mesh.Positions.Add(FVector3f(300.0f + 100.0f * (Vertex & 1), 100.0f * (Vertex >> 1), 0.0f));
		Mesh.Normals.Add(FVector3f(0.0f, 0.0f, 1.0f));
		Mesh.UVs.Add(FVector2f(0.25f, 0.75f));
		Mesh.UV1s.Add(FVector2f::ZeroVector);
	}

	const TSharedPtr<const FMantleBakeMesh> Bake = FMantleBakeMesh::FromSection(Mesh, 0);
	if (!TestTrue(TEXT("the section is laid out"), Bake.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("its UV0 overlaps itself, so it gets a layout of its own"), Bake->bOwnLayout);
	TestEqual(TEXT("the shared vertex is copied once"), Bake->SplitFrom.Num(), 1);
	TestEqual(TEXT("it is the middle one"), Bake->SplitFrom.Num() == 1 ? Bake->SplitFrom[0] : -1, 2);
	TestTrue(TEXT("no texel of the layout is drawn twice"), Bake->LayoutOverlap() < 0.001);

	FMantleGlbMesh Published = Mesh;
	Bake->ApplyLayout(Published, 0);
	TestEqual(TEXT("the mesh gains the copy"), Published.Positions.Num(), 12);
	TestTrue(TEXT("every per-vertex stream with it"), Published.Normals.Num() == 12 && Published.UVs.Num() == 12 && Published.UV1s.Num() == 12);
	TestEqual(TEXT("at the end of the lens"), Published.Primitives[0].NumVertices, 9);
	TestEqual(TEXT("the body moves along"), Published.Primitives[1].FirstVertex, 9);
	TestTrue(TEXT("and keeps its own UVs"), Published.UVs[9].Equals(FVector2f(0.25f, 0.75f)));
	TestTrue(TEXT("the copy is the middle vertex"), Published.Positions[8].Equals(Points[2]) && Published.UV1s[8].Equals(Uvs[2]));
	TestTrue(TEXT("the lens draws the bake's triangles"), Published.Primitives[0].Indices == Bake->Indices);
	TestTrue(TEXT("one triangle on the original middle vertex, the other on its copy"),
		Published.Primitives[0].Indices.Contains(2u) && Published.Primitives[0].Indices.Contains(8u));
	bool bLaidOut = true;
	for (int32 Vertex = 0; Vertex < 9; ++Vertex)
	{
		bLaidOut = bLaidOut && Published.UVs[Vertex].Equals(Bake->Layout[Vertex]);
	}
	TestTrue(TEXT("every lens vertex, the copy too, is at its place in the layout"), bLaidOut);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleUvBakeKeepsUv0Test,
	"Mantle.Editor.UvBake.CleanOrPaintedUv0IsKept",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleUvBakeKeepsUv0Test::RunTest(const FString& Parameters)
{
	using namespace MantleUvBakeMeshTestPrivate;
	// A section whose UV0 covers each texel once bakes laid out by it: the mesh keeps every UV.
	FMantleGlbMesh Clean = NewTwoSectionMesh();
	for (int32 Vertex = 4; Vertex < 8; ++Vertex)
	{
		Clean.UVs[Vertex].X += 2.0f; // the second quad beside the first, not on it
	}
	const TSharedPtr<const FMantleBakeMesh> Kept = FMantleBakeMesh::FromSection(Clean, 0);
	if (TestTrue(TEXT("a clean section is set up"), Kept.IsValid()))
	{
		TestFalse(TEXT("its UV0 does not overlap itself"), FMantleBakeMesh::Overlap(Kept->UV0s, Kept->Indices) > FMantleBakeMesh::OverlapsItself);
		TestFalse(TEXT("so it bakes by its UV0"), Kept->bOwnLayout);
		TestTrue(TEXT("the layout is its UV0"), Kept->Layout == Kept->UV0s);
		FMantleGlbMesh Published = Clean;
		Kept->ApplyLayout(Published, 0);
		TestTrue(TEXT("and the mesh is left exactly as it was"), Published.UVs == Clean.UVs && Published.Positions == Clean.Positions
			&& Published.Primitives[0].Indices == Clean.Primitives[0].Indices);
	}

	// An overlapping UV0 that something else reads (a painted section: liveries) is kept too.
	const FMantleGlbMesh Overlapping = NewTwoSectionMesh();
	const TSharedPtr<const FMantleBakeMesh> Painted = FMantleBakeMesh::FromSection(Overlapping, 0, /*bMayRelayout*/ false);
	const TSharedPtr<const FMantleBakeMesh> Relaid = FMantleBakeMesh::FromSection(Overlapping, 0, /*bMayRelayout*/ true);
	if (TestTrue(TEXT("a painted section is set up"), Painted.IsValid() && Relaid.IsValid()))
	{
		TestFalse(TEXT("it bakes by its UV0 though it overlaps"), Painted->bOwnLayout);
		TestNotEqual(TEXT("its key is not the relaid one's"), Painted->Hash, Relaid->Hash);
		FMantleGlbMesh Published = Overlapping;
		Painted->ApplyLayout(Published, 0);
		TestTrue(TEXT("and its UVs are untouched"), Published.UVs == Overlapping.UVs);
	}

	// Which body sections keep their UV0: the painted ones, by Paint.Sections or a paint layer's sections.
	UMantleVehicleSource* Source = NewObject<UMantleVehicleSource>(GetTransientPackage(), NAME_None, RF_Transient);
	Source->Paint.Sections.Add(TEXT("Body"));
	FMantleVehicleSourcePaintLayer& Layer = Source->Paint.Layers.AddDefaulted_GetRef();
	Layer.Sections.Add(TEXT("Trim"));
	TestTrue(TEXT("a painted section keeps its UV0"), FMantlePlaceAssets::VehicleSectionKeepsUv0(*Source, TEXT("Body")));
	TestTrue(TEXT("so does a paint layer's"), FMantlePlaceAssets::VehicleSectionKeepsUv0(*Source, TEXT("Trim")));
	TestFalse(TEXT("a lamp's may be laid out anew"), FMantlePlaceAssets::VehicleSectionKeepsUv0(*Source, TEXT("Lights")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleUvBakeKeyTest,
	"Mantle.Editor.UvBake.OnlySecondUvSectionsBakeOverTheirOwnTriangles",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleUvBakeKeyTest::RunTest(const FString& Parameters)
{
	using namespace MantleUvBakeMeshTestPrivate;
	const FMantleGlbMesh Mesh = NewTwoSectionMesh();
	UMaterial* Lens = NewUv1MaskMaterial();
	UMaterial* Plain = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
	UMaterialExpressionConstant3Vector* Color = AddNode<UMaterialExpressionConstant3Vector>(Plain);
	Plain->GetEditorOnlyData()->BaseColor.Connect(0, Color);

	const TSharedPtr<const FMantleBakeMesh> Bake = FMantlePlaceAssets::SectionBakeMesh(Mesh, 0, Lens, true);
	TestTrue(TEXT("a material reading UV1 bakes over its section"), Bake.IsValid());
	TestFalse(TEXT("one that does not bakes on a card, as before"), FMantlePlaceAssets::SectionBakeMesh(Mesh, 0, Plain, true).IsValid());
	if (!Bake.IsValid())
	{
		return false;
	}

	// Its own key and cache tag; a card's are what they always were.
	const FString Key = FMantlePlaceAssets::MaterialKey(Lens);
	TestEqual(TEXT("its key is the material's and the section's"), FMantlePlaceAssets::SurfaceMaterialKey(Lens, false, Bake.Get()), FMantlePlaceAssets::UvBakeKey(Key, *Bake));
	TestNotEqual(TEXT("which is not the material's own"), FMantlePlaceAssets::UvBakeKey(Key, *Bake), Key);
	TestEqual(TEXT("a card's key is unchanged"), FMantlePlaceAssets::SurfaceMaterialKey(Lens, false, nullptr), Key);
	TestEqual(TEXT("a tint mask's too"), FMantlePlaceAssets::SurfaceMaterialKey(Lens, true, nullptr), FMantlePlaceAssets::TintMaskKey(Key));
	FMantleBakeSurface Surface;
	TestTrue(TEXT("the plain surface's cache tag is empty, as before"), Surface.CacheTag().IsEmpty());
	Surface.Mesh = Bake;
	TestFalse(TEXT("a section's surface is not plain"), Surface.IsPlain());
	TestTrue(TEXT("and its cache tag holds the section"), Surface.CacheTag().Contains(FString::Printf(TEXT("uvbake=4:%s"), *Bake->Hash)));

	// Gather keeps it as its own entry, apart from the same material on a card.
	FMantlePlaceDependencies Dependencies;
	Dependencies.AddMaterial(Lens, TEXT("Car"), false, Bake);
	Dependencies.AddMaterial(Lens, TEXT("Sign"));
	TestEqual(TEXT("two entries: the section's bake and the card's"), Dependencies.Materials.Num(), 2);
	const FMantleGatheredMaterial* Section = Dependencies.Materials.FindByPredicate([&Bake, &Key](const FMantleGatheredMaterial& Entry)
	{
		return Entry.Key == FMantlePlaceAssets::UvBakeKey(Key, *Bake);
	});
	TestTrue(TEXT("whose surface carries the section"), Section && Section->BakeSurface().Mesh == Bake);

	// Its bake reads a copy where every input reads mesh data, so GLTFExporter bakes each one by the layout and passes none through.
	UMaterial* Rough = NewUv1MaskMaterial();
	UMaterialExpressionConstant* Half = AddNode<UMaterialExpressionConstant>(Rough);
	Half->R = 0.5f;
	Rough->GetEditorOnlyData()->Roughness.Connect(0, Half);
	const TStrongObjectPtr<UMaterialInterface> Every = FMantlePlaceAssets::ReadsVertexColorCopy(Rough, /*bEveryInput*/ true);
	const TStrongObjectPtr<UMaterialInterface> BaseOnly = FMantlePlaceAssets::ReadsVertexColorCopy(Rough);
	if (TestTrue(TEXT("both copies are made"), Every.IsValid() && BaseOnly.IsValid()))
	{
		const UMaterialExpression* EveryRoughness = Every->GetMaterial()->GetEditorOnlyData()->Roughness.Expression;
		const UMaterialExpression* BaseRoughness = BaseOnly->GetMaterial()->GetEditorOnlyData()->Roughness.Expression;
		TestTrue(TEXT("every input: roughness is multiplied too"), EveryRoughness && EveryRoughness->IsA<UMaterialExpressionMultiply>());
		TestTrue(TEXT("base color only: roughness is left as it is"), BaseRoughness && BaseRoughness->IsA<UMaterialExpressionConstant>());
		TestTrue(TEXT("the base color is multiplied in both"), Every->GetMaterial()->GetEditorOnlyData()->BaseColor.Expression->IsA<UMaterialExpressionMultiply>()
			&& BaseOnly->GetMaterial()->GetEditorOnlyData()->BaseColor.Expression->IsA<UMaterialExpressionMultiply>());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleUvBakeRenderTest,
	"Mantle.Editor.UvBake.SecondUvMaskBakesWhereEachTriangleIs",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleUvBakeRenderTest::RunTest(const FString& Parameters)
{
	using namespace MantleUvBakeMeshTestPrivate;
	FMantleGltfMaterialFlattener Flattener;
	if (!Flattener.CanBakeTextures())
	{
		AddInfo(TEXT("No renderer (-nullrhi): the bake itself is not run."));
		return true;
	}
	const FMantleGlbMesh Mesh = NewTwoSectionMesh();
	UMaterial* Lens = NewUv1MaskMaterial();
	const TSharedPtr<const FMantleBakeMesh> Bake = FMantlePlaceAssets::SectionBakeMesh(Mesh, 0, Lens, true);
	if (!TestTrue(TEXT("the section is laid out"), Bake.IsValid()))
	{
		return false;
	}
	FMantleFlattenSettings Settings;
	Settings.DefaultBakeSize = 256;
	Settings.Surface.Mesh = Bake;
	FMantleGlbDocument Document;
	TArray<FString> Messages;
	FString Error;
	if (!TestTrue(TEXT("it bakes"), Flattener.Flatten(Lens, Settings, Document, Messages, Error)))
	{
		AddError(Error);
		return false;
	}
	for (const FString& Message : Messages)
	{
		AddInfo(Message);
		TestFalse(TEXT("every texture is drawn by the layout"), Message.Contains(TEXT("not drawn by its section's layout")));
	}
	const TSharedPtr<FJsonObject> Info = BaseColorInfo(Document);
	if (!TestTrue(TEXT("its base color is a baked picture"), Info.IsValid()))
	{
		return false;
	}
	double TexCoord = -1.0;
	Info->TryGetNumberField(TEXT("texCoord"), TexCoord);
	TestEqual(TEXT("read on UV0, which the section is published with as its layout"), TexCoord, 0.0);

	TArray64<uint8> Rgba;
	int32 Width = 0;
	int32 Height = 0;
	if (!TestTrue(TEXT("the picture decodes"), DecodeImage(Document, Info, Rgba, Width, Height)))
	{
		return false;
	}
	// Each quad's own UV1 decides its color: grey at U 0.1, yellow at U 0.9 (Lerp at 0.1 and 0.9). Blue tells them apart best:
	// linear 0.455 against 0.095, sRGB about 179 against 86.
	const FColor First = Sample(Rgba, Width, Height, Info, QuadCenter(*Bake, 0));
	const FColor Second = Sample(Rgba, Width, Height, Info, QuadCenter(*Bake, 4));
	AddInfo(FString::Printf(TEXT("quad at UV1 0.1: %s; quad at UV1 0.9: %s"), *First.ToString(), *Second.ToString()));
	const FColor WantFirst = FMath::Lerp(Grey, Yellow, 0.1f).ToFColor(true);
	const FColor WantSecond = FMath::Lerp(Grey, Yellow, 0.9f).ToFColor(true);
	TestTrue(TEXT("the first quad is grey, as its UV1 places it"), FMath::Abs(First.B - WantFirst.B) <= 8 && FMath::Abs(First.R - WantFirst.R) <= 8);
	TestTrue(TEXT("the second quad is yellow, though it shares the first one's UV0"), FMath::Abs(Second.B - WantSecond.B) <= 8 && FMath::Abs(Second.R - WantSecond.R) <= 8);
	return true;
}

// The tests above hand FromSection a mesh made in code, UV1 filled in. A car body is a skeletal mesh read by the converter, and that reader
// read UV0 only: every body section's bake read UV0 in place of UV1 and SUV04's lamps came out dark (98% of the lens area where the two
// sets pick different zones matched the UV0 colors). This reads a real skeletal body with a second UV set, a tire merged onto it, the way
// the gather and the converter read it.
namespace MantleUvBakeMeshTestPrivate
{
	/** Every body vertex's UV1 is its UV0 moved by this: a read that lost UV1 (or put UV0 there) cannot pass. */
	const FVector2f Uv1Shift(0.5f, 0.25f);

	/** A transient static mesh of one quad with a single UV set (no UV1), its vertices readable, for a wheel's tire mesh. */
	UStaticMesh* NewTireQuad()
	{
		FMeshDescription Description;
		FStaticMeshAttributes Attributes(Description);
		Attributes.Register();
		const FPolygonGroupID Group = Description.CreatePolygonGroup();
		Attributes.GetPolygonGroupMaterialSlotNames()[Group] = TEXT("Tire");
		TArray<FVertexInstanceID> Corners;
		for (int32 Corner = 0; Corner < 4; ++Corner)
		{
			const float U = static_cast<float>(Corner & 1);
			const float V = static_cast<float>(Corner >> 1);
			const FVertexID Vertex = Description.CreateVertex();
			Attributes.GetVertexPositions()[Vertex] = FVector3f(0.0f, 30.0f * U, 30.0f * V);
			const FVertexInstanceID Instance = Description.CreateVertexInstance(Vertex);
			Attributes.GetVertexInstanceNormals()[Instance] = FVector3f(1.0f, 0.0f, 0.0f);
			Attributes.GetVertexInstanceTangents()[Instance] = FVector3f(0.0f, 1.0f, 0.0f);
			Attributes.GetVertexInstanceBinormalSigns()[Instance] = 1.0f;
			Attributes.GetVertexInstanceUVs().Set(Instance, 0, FVector2f(0.125f + 0.25f * U, 0.125f + 0.25f * V));
			Corners.Add(Instance);
		}
		Description.CreateTriangle(Group, { Corners[0], Corners[2], Corners[1] });
		Description.CreateTriangle(Group, { Corners[2], Corners[3], Corners[1] });
		UStaticMesh* Mesh = NewObject<UStaticMesh>(GetTransientPackage(), NAME_None, RF_Transient);
		Mesh->GetStaticMaterials().Add(FStaticMaterial(nullptr, TEXT("Tire"), TEXT("Tire")));
		Mesh->bAllowCPUAccess = true;
		Mesh->SetNumSourceModels(1);
		FMeshBuildSettings& Build = Mesh->GetSourceModel(0).BuildSettings;
		Build.bRecomputeNormals = false;
		Build.bRecomputeTangents = false;
		Build.bGenerateLightmapUVs = false;
		UStaticMesh::FBuildMeshDescriptionsParams Params;
		Params.bMarkPackageDirty = false;
		Params.bAllowCpuAccess = true;
		Params.bBuildSimpleCollision = false;
		if (!Mesh->BuildFromMeshDescriptions({ &Description }, Params))
		{
			Mesh->MarkAsGarbage();
			return nullptr;
		}
		return Mesh;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleUvBakeVehicleBodyReadTest,
	"Mantle.Editor.UvBake.VehicleBodyReadsItsSecondUv",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleUvBakeVehicleBodyReadTest::RunTest(const FString& Parameters)
{
	using namespace MantleUvBakeMeshTestPrivate;
	// The sedan chassis's gray body (no project content), saved with a second UV set: UV0 moved by Uv1Shift.
	FMantleChassisSpec Spec;
	FMantleVehicleAsset Asset;
	FString Error;
	if (!TestTrue(FString::Printf(TEXT("the sedan chassis is made (%s)"), *Error),
		FMantleVehicleChassisBuilder::LoadSpec(TEXT("sedan"), Spec, Error) && FMantleVehicleChassisBuilder::MakeAsset(Spec, Asset, Error)))
	{
		return false;
	}
	FMantleVehicleBodyExtras Extras;
	TArray<FVector2f>& Uv1 = Extras.UVs.AddDefaulted_GetRef();
	for (const FVector2f& Uv0 : Asset.Body.LODs[0].UVs)
	{
		Uv1.Add(Uv0 + Uv1Shift);
	}
	USkeletalMesh* BodyMesh = FMantleVehicleChassisMesh::Create(Asset.Body, TEXT("/Temp/MantleUvBakeBodyRead"), TEXT("UvBakeBody"), Error, nullptr, &Extras);
	if (!TestNotNull(FString::Printf(TEXT("the body mesh is saved with two UV sets (%s)"), *Error), BodyMesh))
	{
		return false;
	}
	ON_SCOPE_EXIT { MantleVehicleScratchBody::Discard(BodyMesh); };
	UMantleVehicleSource* Source = NewObject<UMantleVehicleSource>(GetTransientPackage());
	Source->Body = BodyMesh;

	FMantleGlbMesh Plain;
	TArray<UMaterialInterface*> Materials;
	if (!TestTrue(FString::Printf(TEXT("the body reads (%s)"), *Error), FMantleVehicleConverter::ReadBody(*Source, Plain, Materials, Error)))
	{
		return false;
	}
	TestTrue(TEXT("read as the builder and the preview read it, it has no second UV set (as before)"), Plain.UV1s.IsEmpty());

	FMantleGlbMesh Body;
	if (!TestTrue(TEXT("the body reads with its UV1"), FMantleVehicleConverter::ReadBody(*Source, Body, Materials, Error, true, /*bSecondUv*/ true)))
	{
		return false;
	}
	if (!TestEqual(TEXT("one UV1 per vertex"), Body.UV1s.Num(), Body.Positions.Num()) || !TestTrue(TEXT("it has vertices"), Body.Positions.Num() > 0))
	{
		return false;
	}
	bool bShifted = true;
	for (int32 Vertex = 0; Vertex < Body.Positions.Num(); ++Vertex)
	{
		bShifted = bShifted && (Body.UV1s[Vertex] - Body.UVs[Vertex]).Equals(Uv1Shift, 0.05f);
	}
	TestTrue(TEXT("each vertex's UV1 is the mesh's channel 1, in the same vertex order as its UV0"), bShifted);

	// A section baked over its own triangles gets that UV1, not UV0.
	const TSharedPtr<const FMantleBakeMesh> Bake = FMantleBakeMesh::FromSection(Body, 0);
	if (TestTrue(TEXT("its first section is laid out"), Bake.IsValid()))
	{
		bool bBakeShifted = Bake->UV1s.Num() == Bake->UV0s.Num();
		for (int32 Vertex = 0; bBakeShifted && Vertex < Bake->UV1s.Num(); ++Vertex)
		{
			bBakeShifted = (Bake->UV1s[Vertex] - Bake->UV0s[Vertex]).Equals(Uv1Shift, 0.05f);
		}
		TestTrue(TEXT("and its bake reads the body's UV1"), bBakeShifted);
	}

	// A tire merged onto a wheel bone keeps every stream in step; the tire has no UV1, so it gives its UV0 there.
	UStaticMesh* Tire = NewTireQuad();
	if (!TestNotNull(TEXT("the tire mesh is built"), Tire))
	{
		return false;
	}
	ON_SCOPE_EXIT { Tire->MarkAsGarbage(); };
	if (!TestTrue(TEXT("the body has bones"), Body.Joints.Num() > 0))
	{
		return false;
	}
	FMantleVehicleSourceWheel& Wheel = Source->Wheels.AddDefaulted_GetRef();
	Wheel.Bone = Body.Joints.Last().Name;
	Wheel.TireMesh = Tire;
	FMantleGlbMesh WithTire;
	if (!TestTrue(FString::Printf(TEXT("the body reads with its tire (%s)"), *Error), FMantleVehicleConverter::ReadBody(*Source, WithTire, Materials, Error, true, /*bSecondUv*/ true)))
	{
		return false;
	}
	const int32 Added = WithTire.Positions.Num() - Body.Positions.Num();
	TestTrue(TEXT("the tire adds vertices"), Added > 0);
	TestEqual(TEXT("UV1 still has one per vertex"), WithTire.UV1s.Num(), WithTire.Positions.Num());
	bool bTireUv0 = WithTire.UV1s.Num() == WithTire.Positions.Num();
	for (int32 Vertex = Body.Positions.Num(); bTireUv0 && Vertex < WithTire.Positions.Num(); ++Vertex)
	{
		bTireUv0 = WithTire.UV1s[Vertex].Equals(WithTire.UVs[Vertex], 1.0e-4f);
	}
	TestTrue(TEXT("the tire's vertices give their UV0 as UV1"), bTireUv0);
	const TSharedPtr<const FMantleBakeMesh> BakeWithTire = FMantleBakeMesh::FromSection(WithTire, 0);
	TestTrue(TEXT("the body section's bake is the same with a tire on"), BakeWithTire.IsValid() && Bake.IsValid() && BakeWithTire->UV1s == Bake->UV1s);

	// A second UV set that lost its count on the way is refused out loud, never baked by UV0.
	FMantleGlbMesh Broken = WithTire;
	Broken.UV1s.SetNum(Body.Positions.Num());
	AddExpectedError(TEXT("its second UV set has"), EAutomationExpectedErrorFlags::Contains, 1);
	TestFalse(TEXT("a second UV set that does not cover the vertices is not baked by UV0"), FMantleBakeMesh::FromSection(Broken, 0).IsValid());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
