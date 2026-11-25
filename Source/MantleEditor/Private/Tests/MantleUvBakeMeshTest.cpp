// Copyright Joshua Eger. All Rights Reserved.
#include "MantleGlbWriter.h"
#include "MantlePlaceAssetConverter.h"
#include "MantlePlaceAssets.h"

#include "Dom/JsonObject.h"
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

	const TSharedPtr<const FMantleBakeMesh> Bake = FMantlePlaceAssets::SectionBakeMesh(Mesh, 0, Lens);
	TestTrue(TEXT("a material reading UV1 bakes over its section"), Bake.IsValid());
	TestFalse(TEXT("one that does not bakes on a card, as before"), FMantlePlaceAssets::SectionBakeMesh(Mesh, 0, Plain).IsValid());
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
	TestTrue(TEXT("and its cache tag holds the section"), Surface.CacheTag().Contains(FString::Printf(TEXT("uvbake=1:%s"), *Bake->Hash)));

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
	const TSharedPtr<const FMantleBakeMesh> Bake = FMantlePlaceAssets::SectionBakeMesh(Mesh, 0, Lens);
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

#endif // WITH_DEV_AUTOMATION_TESTS
