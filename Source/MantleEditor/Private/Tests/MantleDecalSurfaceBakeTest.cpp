// Copyright Joshua Eger. All Rights Reserved.
#include "MantleDecalBake.h"
#include "MantleGlbWriter.h"
#include "MantlePlaceAssetConverter.h"
#include "MantlePlaceAssets.h"
#include "MantlePlaceExporter.h"

#include "Components/DecalComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/DecalActor.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "ImageUtils.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionPower.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// A1 (publish test level): a decal material (Material Domain: Deferred Decal) bakes blank in the engine's bake, so City Sample's mesh
// decals (road lines, paint) published as invisible cards, and a Decal Actor whose shape is in an Opacity graph Mantle's own reader can't
// run (City Sample's decal master) was refused. Both now bake the material's surface copy: same graph, parameters and blend mode. No
// renderer here: the flatteners are scripted, and the real bake is proven with the test level's editor and client pictures.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleDecalSurfaceBakeTestPrivate
{
	template <typename TExpression>
	TExpression* AddNode(UMaterial* Material, UClass* Class = TExpression::StaticClass())
	{
		TExpression* Expression = NewObject<TExpression>(Material, Class);
		Expression->Material = Material;
		Material->GetExpressionCollection().AddExpression(Expression);
		return Expression;
	}

	/** The engine's Decal Lifetime Opacity node class: not exported from the engine, so found by name. */
	UClass* LifetimeOpacityClass()
	{
		return FindObject<UClass>(nullptr, TEXT("/Script/Engine.MaterialExpressionDecalLifetimeOpacity"));
	}

	/** A translucent decal: base color a constant, Opacity = scalar parameter "Fade" (default 0.2) x Decal Lifetime Opacity. */
	UMaterial* NewFadingDecal()
	{
		UMaterial* Material = NewObject<UMaterial>(GetTransientPackage());
		Material->MaterialDomain = MD_DeferredDecal;
		Material->BlendMode = BLEND_Translucent;
		UMaterialExpressionConstant3Vector* Color = AddNode<UMaterialExpressionConstant3Vector>(Material);
		Color->Constant = FLinearColor(0.9f, 0.9f, 0.85f, 1.0f);
		Material->GetEditorOnlyData()->BaseColor.Connect(0, Color);
		UMaterialExpressionScalarParameter* Fade = AddNode<UMaterialExpressionScalarParameter>(Material);
		Fade->ParameterName = TEXT("Fade");
		Fade->DefaultValue = 0.2f;
		UMaterialExpressionMultiply* Multiply = AddNode<UMaterialExpressionMultiply>(Material);
		Multiply->A.Connect(0, Fade);
		if (UClass* Lifetime = LifetimeOpacityClass())
		{
			Multiply->B.Connect(0, AddNode<UMaterialExpression>(Material, Lifetime));
		}
		Material->GetEditorOnlyData()->Opacity.Connect(0, Multiply);
		Material->UpdateCachedExpressionData();
		return Material;
	}

	/** Whether anything Opacity reads (however deep) is of Class: a node replaced in a bake copy stays in its graph, read by nothing. */
	bool OpacityReaches(const UMaterial* Material, const UClass* Class)
	{
		TArray<UMaterialExpression*> Open;
		TSet<UMaterialExpression*> Seen;
		if (UMaterialExpression* Start = Material->GetEditorOnlyData()->Opacity.Expression)
		{
			Open.Add(Start);
		}
		while (Open.Num() > 0)
		{
			UMaterialExpression* Expression = Open.Pop();
			if (!Expression || Seen.Contains(Expression))
			{
				continue;
			}
			Seen.Add(Expression);
			if (Expression->GetClass() == Class)
			{
				return true;
			}
			for (FExpressionInputIterator It{ Expression }; It; ++It)
			{
				Open.Add(It.Input->Expression);
			}
		}
		return false;
	}

	/**
	 * A one-material GLB whose base color is Pixels (Width x Height, BGRA) as an embedded PNG, translucent; with NormalPixels (the same
	 * size), a normal map too.
	 */
	FMantleGlbDocument DocumentWithImage(const TArray<FColor>& Pixels, int32 Width, int32 Height, const TArray<FColor>* NormalPixels = nullptr)
	{
		FMantleGlbDocument Doc;
		Doc.Json = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Views, Images, Textures;
		auto AddImage = [&](const TArray<FColor>& Image)
		{
			TArray64<uint8> Png;
			FImageUtils::PNGCompressImageArray(Width, Height, TArrayView64<const FColor>(Image.GetData(), Image.Num()), Png);
			const TSharedRef<FJsonObject> View = MakeShared<FJsonObject>();
			View->SetNumberField(TEXT("buffer"), 0);
			View->SetNumberField(TEXT("byteOffset"), Doc.Bin.Num());
			View->SetNumberField(TEXT("byteLength"), static_cast<double>(Png.Num()));
			Doc.Bin.Append(Png.GetData(), static_cast<int32>(Png.Num()));
			const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetNumberField(TEXT("bufferView"), Views.Num());
			Entry->SetStringField(TEXT("mimeType"), TEXT("image/png"));
			const TSharedRef<FJsonObject> Texture = MakeShared<FJsonObject>();
			Texture->SetNumberField(TEXT("source"), Images.Num());
			Views.Add(MakeShared<FJsonValueObject>(View));
			Images.Add(MakeShared<FJsonValueObject>(Entry));
			Textures.Add(MakeShared<FJsonValueObject>(Texture));
			return Textures.Num() - 1;
		};
		const TSharedRef<FJsonObject> BaseColor = MakeShared<FJsonObject>();
		BaseColor->SetNumberField(TEXT("index"), AddImage(Pixels));
		const TSharedRef<FJsonObject> Pbr = MakeShared<FJsonObject>();
		Pbr->SetObjectField(TEXT("baseColorTexture"), BaseColor);
		Pbr->SetNumberField(TEXT("metallicFactor"), 0.0);
		Pbr->SetNumberField(TEXT("roughnessFactor"), 0.8);
		const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("alphaMode"), TEXT("BLEND"));
		Entry->SetObjectField(TEXT("pbrMetallicRoughness"), Pbr);
		if (NormalPixels)
		{
			const TSharedRef<FJsonObject> Normal = MakeShared<FJsonObject>();
			Normal->SetNumberField(TEXT("index"), AddImage(*NormalPixels));
			Entry->SetObjectField(TEXT("normalTexture"), Normal);
		}
		Doc.Json->SetArrayField(TEXT("bufferViews"), Views);
		Doc.Json->SetArrayField(TEXT("images"), Images);
		Doc.Json->SetArrayField(TEXT("textures"), Textures);
		TArray<TSharedPtr<FJsonValue>> Materials;
		Materials.Add(MakeShared<FJsonValueObject>(Entry));
		Doc.Json->SetArrayField(TEXT("materials"), Materials);
		return Doc;
	}

	/** Records what it is handed and gives back a translucent bake: a 2x2 image, half see-through, cream colored, and a bumpy normal map. */
	class FScriptedFlattener : public IMantleMaterialFlattener
	{
	public:
		int32 Calls = 0;
		TEnumAsByte<EMaterialDomain> SeenDomain = MD_MAX;
		TEnumAsByte<EBlendMode> SeenBlend = BLEND_MAX;
		bool bSawLifetimeOpacity = false;

		virtual bool Flatten(UMaterialInterface* Material, const FMantleFlattenSettings& Settings, FMantleGlbDocument& OutDocument,
			TArray<FString>& OutMessages, FString& OutError) override
		{
			++Calls;
			const UMaterial* Base = Material ? Material->GetMaterial() : nullptr;
			if (!Base)
			{
				OutError = TEXT("no material");
				return false;
			}
			SeenDomain = Base->MaterialDomain;
			SeenBlend = Material->GetBlendMode();
			bSawLifetimeOpacity = LifetimeOpacityClass() && OpacityReaches(Base, LifetimeOpacityClass());
			TArray<FColor> Pixels;
			Pixels.Init(FColor(230, 225, 210, 128), 4);
			TArray<FColor> Normal;
			Normal.Init(FColor(160, 110, 240, 255), 4);
			OutDocument = DocumentWithImage(Pixels, 2, 2, &Normal);
			return true;
		}

		virtual FString Describe() const override { return TEXT("scripted decal surface"); }
		virtual bool CanBakeTextures() const override { return true; }
	};

	/** A transient editor world for one test, destroyed with it. */
	struct FDecalWorld
	{
		UWorld* World = nullptr;

		FDecalWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld*/ false, TEXT("MantleDecalSurfaceTestWorld"));
		}

		~FDecalWorld()
		{
			FMantleDecalBake::SetFlattenerForTests(nullptr);
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
	FMantleDecalSurfaceCopyTest,
	"Mantle.Editor.DecalSurface.CopyIsASurfaceWithTheSameBlendAndParameters",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleDecalSurfaceCopyTest::RunTest(const FString& Parameters)
{
	using namespace MantleDecalSurfaceBakeTestPrivate;
	if (!TestNotNull(TEXT("the engine has a Decal Lifetime Opacity node"), LifetimeOpacityClass()))
	{
		return false;
	}
	UMaterial* Decal = NewFadingDecal();
	UMaterialInstanceConstant* Instance = NewObject<UMaterialInstanceConstant>(GetTransientPackage());
	Instance->SetParentEditorOnly(Decal);
	Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Fade")), 0.7f);

	const TStrongObjectPtr<UMaterialInterface> Copy = FMantlePlaceAssets::DecalSurfaceCopy(Instance);
	if (!TestTrue(TEXT("a decal material gets a surface copy"), Copy.IsValid()))
	{
		return false;
	}
	const UMaterial* CopyBase = Copy->GetMaterial();
	if (!TestNotNull(TEXT("the copy has a base"), CopyBase))
	{
		return false;
	}
	TestEqual(TEXT("the copy is a surface material"), static_cast<int32>(CopyBase->MaterialDomain), static_cast<int32>(MD_Surface));
	TestEqual(TEXT("with the blend mode the creator marked"), static_cast<int32>(Copy->GetBlendMode()), static_cast<int32>(BLEND_Translucent));
	float Fade = 0.0f;
	TestTrue(TEXT("the instance's parameter is kept"), Copy->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("Fade")), Fade));
	TestEqual(TEXT("at the instance's value"), Fade, 0.7f, 1.0e-4f);
	TestFalse(TEXT("nothing in the copy's Opacity reads Decal Lifetime Opacity (0 outside a decal)"), OpacityReaches(CopyBase, LifetimeOpacityClass()));
	const UMaterialExpressionMultiply* Multiply = Cast<UMaterialExpressionMultiply>(CopyBase->GetEditorOnlyData()->Opacity.Expression);
	const UMaterialExpressionConstant* One = Multiply ? Cast<UMaterialExpressionConstant>(Multiply->B.Expression) : nullptr;
	if (TestNotNull(TEXT("it is a constant in the copy's Opacity"), One))
	{
		TestEqual(TEXT("of 1, a decal that never fades"), One->R, 1.0f);
	}

	// The creator's material is untouched.
	TestEqual(TEXT("the source is still a decal"), static_cast<int32>(Decal->MaterialDomain), static_cast<int32>(MD_DeferredDecal));
	TestTrue(TEXT("and its Opacity still reads Decal Lifetime Opacity"), OpacityReaches(Decal, LifetimeOpacityClass()));

	// A surface material has nothing to copy.
	UMaterial* Surface = NewObject<UMaterial>(GetTransientPackage());
	TestFalse(TEXT("a surface material gets no copy"), FMantlePlaceAssets::DecalSurfaceCopy(Surface).IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMeshDecalBakesItsSurfaceCopyTest,
	"Mantle.Editor.DecalSurface.MeshDecalIsBakedAsItsSurfaceCopy",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMeshDecalBakesItsSurfaceCopyTest::RunTest(const FString& Parameters)
{
	using namespace MantleDecalSurfaceBakeTestPrivate;
	UMaterial* Decal = NewFadingDecal();

	const FMantleMaterialAnalysis Analysis = FMantlePlaceAssets::AnalyzeMaterial(Decal);
	TestFalse(TEXT("a decal material is no longer a 'not a surface material' loss"), EnumHasAnyFlags(Analysis.Losses, EMantleMaterialLoss::NotSurface));

	TSharedRef<FScriptedFlattener> Flattener = MakeShared<FScriptedFlattener>();
	FMantlePlaceAssetConverter Converter(Flattener, FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("MantleDecalSurface")));
	FMantlePlaceAssetSet Set;
	if (!TestTrue(TEXT("it converts"), Converter.ConvertMaterial(Decal, Set)))
	{
		return false;
	}
	TestEqual(TEXT("the flattener ran once"), Flattener->Calls, 1);
	TestEqual(TEXT("it was handed a surface material"), static_cast<int32>(Flattener->SeenDomain), static_cast<int32>(MD_Surface));
	// A1 (Joshua: cut-outs): a mesh decal always bakes through its Masked copy, whatever the GBuffer, and publishes as that cut-out.
	TestEqual(TEXT("its Masked copy"), static_cast<int32>(Flattener->SeenBlend), static_cast<int32>(BLEND_Masked));
	TestFalse(TEXT("with no Decal Lifetime Opacity in it"), Flattener->bSawLifetimeOpacity);

	const FMantleConvertedMaterial* Converted = Set.FindMaterial(FMantlePlaceAssets::MaterialKey(Decal));
	if (TestNotNull(TEXT("the material is in the set"), Converted))
	{
		TestEqual(TEXT("it publishes as a cut-out, lit like the decal"), Converted->Document.AlphaMode, FString(TEXT("mask")));
		// The scripted bake is one alpha (128) everywhere: cut at half of it (levels 1 to 128 all keep everything; their middle is 64.5).
		TestEqual(TEXT("cut at half its one alpha, so all of it shows"), Converted->Document.AlphaCutoff, 64.0 / 255.0, 1.0e-6);
	}

	// A see-through material that is not a decal still publishes see-through.
	UMaterial* Sheet = NewObject<UMaterial>(GetTransientPackage());
	Sheet->BlendMode = BLEND_Translucent;
	UMaterialExpressionScalarParameter* SheetOpacity = AddNode<UMaterialExpressionScalarParameter>(Sheet);
	SheetOpacity->ParameterName = TEXT("Opacity");
	SheetOpacity->DefaultValue = 0.4f;
	Sheet->GetEditorOnlyData()->Opacity.Connect(0, SheetOpacity);
	Sheet->UpdateCachedExpressionData();
	FMantlePlaceAssetSet SheetSet;
	if (TestTrue(TEXT("a translucent surface material converts"), Converter.ConvertMaterial(Sheet, SheetSet)))
	{
		const FMantleConvertedMaterial* SheetConverted = SheetSet.FindMaterial(FMantlePlaceAssets::MaterialKey(Sheet));
		if (TestNotNull(TEXT("it is in the set"), SheetConverted))
		{
			TestEqual(TEXT("and stays see-through"), SheetConverted->Document.AlphaMode, FString(TEXT("blend")));
		}
	}
	TestFalse(TEXT("the converter gives no 'not a surface material' warning"), Set.Issues.ContainsByPredicate([](const FMantleValidationIssue& Issue)
	{
		return Issue.Problem.Contains(TEXT("not a surface material"));
	}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleDecalActorEngineBakeTest,
	"Mantle.Editor.DecalSurface.DecalActorWithAnUnreadableOpacityUsesTheEngineBake",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleDecalActorEngineBakeTest::RunTest(const FString& Parameters)
{
	using namespace MantleDecalSurfaceBakeTestPrivate;
	FDecalWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}

	// City Sample's case: an opaque colour texture, and Opacity = Power(its red, 2): a node Mantle's own reader can't run.
	TArray<FColor> Opaque;
	Opaque.Init(FColor(200, 200, 200, 255), 16);
	UTexture2D* Colour = NewObject<UTexture2D>(GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UTexture2D::StaticClass(), TEXT("T_Line_Test")));
	Colour->SRGB = true;
	Colour->Source.Init(4, 4, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Opaque.GetData()));

	UMaterial* Material = NewObject<UMaterial>(GetTransientPackage());
	Material->MaterialDomain = MD_DeferredDecal;
	Material->BlendMode = BLEND_Translucent;
	UMaterialExpressionTextureSample* Sample = AddNode<UMaterialExpressionTextureSample>(Material);
	Sample->Texture = Colour;
	UMaterialExpressionPower* Power = AddNode<UMaterialExpressionPower>(Material);
	Power->Base.Connect(1, Sample); // R
	Power->ConstExponent = 2.0f;
	Material->GetEditorOnlyData()->BaseColor.Connect(0, Sample);
	Material->GetEditorOnlyData()->Opacity.Connect(0, Power);
	Material->UpdateCachedExpressionData();

	ADecalActor* Decal = Fixture.World->SpawnActor<ADecalActor>(FVector::ZeroVector, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("the decal"), Decal))
	{
		return false;
	}
	Decal->SetActorLabel(TEXT("Road line"));
	Decal->SetDecalMaterial(Material);

	bool bUnreadable = false;
	TestTrue(TEXT("Mantle's own reader can't run this Opacity graph"),
		FMantlePlaceExporter::DecalTextureWithOpacity(Material, Colour, &bUnreadable) == Colour && bUnreadable);

	TSharedRef<FScriptedFlattener> Flattener = MakeShared<FScriptedFlattener>();
	FMantleDecalBake::SetFlattenerForTests(Flattener);
	FMantleDecalProps Props;
	UTexture* BaseColor = nullptr;
	UTexture* Normal = nullptr;
	FString Problem;
	if (!TestTrue(TEXT("the decal is read, not refused"), FMantlePlaceExporter::ReadDecal(Decal, Props, BaseColor, Normal, Problem)))
	{
		AddInfo(Problem);
		return true;
	}
	TestEqual(TEXT("the engine bake ran once"), Flattener->Calls, 1);
	TestEqual(TEXT("on the surface copy"), static_cast<int32>(Flattener->SeenDomain), static_cast<int32>(MD_Surface));
	if (!TestNotNull(TEXT("with a texture"), BaseColor) || !TestTrue(TEXT("the baked one, not the colour texture"), BaseColor != Colour))
	{
		return true;
	}
	FImage Baked;
	if (TestTrue(TEXT("the baked texture has source pixels"), BaseColor->Source.GetMipImage(Baked, 0)))
	{
		TestEqual(TEXT("at the bake's size"), Baked.SizeX, 2);
		const TArrayView64<FColor> Pixels = Baked.AsBGRA8();
		TestEqual(TEXT("its color"), Pixels[0].R, static_cast<uint8>(230));
		TestEqual(TEXT("and its shape in alpha"), Pixels[0].A, static_cast<uint8>(128));
	}
	// Its normal is the bake's, as the material draws it, green-up and marked so (the upload keeps it as it is).
	if (TestNotNull(TEXT("the decal has the bake's normal map"), Normal))
	{
		TestTrue(TEXT("marked green-up"), Normal->bFlipGreenChannel);
		FImage NormalImage;
		if (TestTrue(TEXT("with source pixels"), Normal->Source.GetMipImage(NormalImage, 0)))
		{
			TestEqual(TEXT("the bake's normal, unchanged"), NormalImage.AsBGRA8()[0].G, static_cast<uint8>(110));
		}
	}
	UTexture* Again = nullptr;
	FMantlePlaceExporter::ReadDecal(Decal, Props, Again, Normal, Problem);
	TestTrue(TEXT("a second read gives a texture again"), Again != nullptr && Again != Colour);

	// Without a bake (no renderer), it is refused as before: the colour texture has no shape in its alpha.
	FMantleDecalBake::SetFlattenerForTests(nullptr);
	if (!FMantleGltfMaterialFlattener().CanBakeTextures())
	{
		UTexture* Refused = nullptr;
		TestFalse(TEXT("with no renderer the decal is refused, as before"), FMantlePlaceExporter::ReadDecal(Decal, Props, Refused, Normal, Problem));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMeshDecalCheckWarningTest,
	"Mantle.Editor.DecalSurface.CheckSaysAMeshDecalPublishesAsACutOut",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMeshDecalCheckWarningTest::RunTest(const FString& Parameters)
{
	using namespace MantleDecalSurfaceBakeTestPrivate;
	auto CutOutWarnings = [](const UMaterialInterface* Material)
	{
		TArray<FMantleValidationIssue> Issues;
		FMantlePlaceAssets::CheckMaterial(Material, TEXT("Floor_Stain"), FMantlePlaceAssetLimits(), Issues);
		return Issues.FilterByPredicate([](const FMantleValidationIssue& Issue)
		{
			return Issue.Severity == EMantleIssueSeverity::Warning && Issue.Problem.Contains(TEXT("is a decal on a mesh"));
		});
	};

	UMaterial* Decal = NewFadingDecal();
	const TArray<FMantleValidationIssue> Said = CutOutWarnings(Decal);
	if (TestEqual(TEXT("a decal material on a mesh gets one cut-out warning"), Said.Num(), 1))
	{
		const FString& Problem = Said[0].Problem;
		TestTrue(TEXT("it says it publishes as a cut-out"), Problem.Contains(TEXT("publishes as a cut-out")));
		TestTrue(TEXT("that soft edges become hard"), Problem.Contains(TEXT("soft edges become hard")));
		TestTrue(TEXT("that the faintest parts are cut away"), Problem.Contains(TEXT("faintest parts are cut away")));
		TestTrue(TEXT("and that a faint decal shows at full strength"), Problem.Contains(TEXT("faint all over shows at full strength")));
		TestTrue(TEXT("it names who uses it"), Problem.Contains(TEXT("Floor_Stain")));
		TestTrue(TEXT("and says what to do instead"), Said[0].Remedy.Contains(TEXT("Decal Actor")));
	}

	UMaterialInstanceConstant* Instance = NewObject<UMaterialInstanceConstant>(GetTransientPackage());
	Instance->SetParentEditorOnly(Decal);
	TestEqual(TEXT("an instance of a decal material too"), CutOutWarnings(Instance).Num(), 1);

	UMaterial* Sheet = NewObject<UMaterial>(GetTransientPackage());
	Sheet->BlendMode = BLEND_Translucent;
	TestEqual(TEXT("a see-through surface material gets none"), CutOutWarnings(Sheet).Num(), 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
