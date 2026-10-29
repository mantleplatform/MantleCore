// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCoverage.h"
#include "MantlePlaceAssets.h"
#include "MantlePlaceExporter.h"

#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionDepthFade.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionTime.h"
#include "MaterialDomain.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// DragonCave's cloud cards (translucent, a depth fade over a gradient and noise) and volume fog (a Volume domain material) bake to a flat
// white card, so a mesh drawn only with them is left out and said. Small graphs built in code, so no level or asset is needed.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleCloudCardMaterialTestPrivate
{
	template <typename TExpression>
	TExpression* AddNode(UMaterial* Material)
	{
		TExpression* Expression = NewObject<TExpression>(Material);
		Expression->Material = Material;
		Material->GetExpressionCollection().AddExpression(Expression);
		return Expression;
	}

	UMaterialExpressionConstant* AddNumber(UMaterial* Material, const float Value)
	{
		UMaterialExpressionConstant* Constant = AddNode<UMaterialExpressionConstant>(Material);
		Constant->R = Value;
		return Constant;
	}

	/** M_CloudCard01's shape: a lit translucent material whose opacity is a Depth Fade over something the far-end read cannot evaluate. */
	UMaterial* NewCloudCard()
	{
		UMaterial* Material = NewObject<UMaterial>(GetTransientPackage());
		Material->BlendMode = BLEND_Translucent;
		UMaterialExpressionMultiply* Shape = AddNode<UMaterialExpressionMultiply>(Material);
		Shape->A.Connect(0, AddNode<UMaterialExpressionTime>(Material));
		Shape->B.Connect(0, AddNumber(Material, 0.9f));
		UMaterialExpressionDepthFade* Fade = AddNode<UMaterialExpressionDepthFade>(Material);
		Fade->InOpacity.Connect(0, Shape);
		Fade->FadeDistance.Connect(0, AddNumber(Material, 1000.0f));
		Material->GetEditorOnlyData()->Opacity.Connect(0, Fade);
		return Material;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCloudCardMaterialTest,
	"Mantle.Editor.Material.CloudCardIsUnpublishable",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCloudCardMaterialTest::RunTest(const FString& Parameters)
{
	using namespace MantleCloudCardMaterialTestPrivate;

	// A cloud card: the guessed opacity would draw one even sheet.
	UMaterial* Cloud = NewCloudCard();
	FString Why;
	TestTrue(TEXT("a depth fade over a shape it cannot read, with no normal, is not water"), FMantlePlaceAssets::UnpublishableMaterialWhy(Cloud, Why));
	TestTrue(TEXT("and says why"), Why.Contains(TEXT("even see-through sheet")));
	TestEqual(TEXT("AnalyzeMaterial agrees"), FMantlePlaceAssets::AnalyzeMaterial(Cloud).UnpublishableWhy, Why);

	// A depth fade whose far end reads (water, soft fog edges) still publishes.
	UMaterial* Readable = NewObject<UMaterial>(GetTransientPackage());
	Readable->BlendMode = BLEND_Translucent;
	UMaterialExpressionDepthFade* Fade = AddNode<UMaterialExpressionDepthFade>(Readable);
	Fade->InOpacity.Connect(0, AddNumber(Readable, 0.85f));
	Fade->FadeDistance.Connect(0, AddNumber(Readable, 100.0f));
	Readable->GetEditorOnlyData()->Opacity.Connect(0, Fade);
	TestFalse(TEXT("a readable fade publishes"), FMantlePlaceAssets::UnpublishableMaterialWhy(Readable, Why));

	// A translucent material with no depth in it, and an opaque one, publish.
	UMaterial* Plain = NewObject<UMaterial>(GetTransientPackage());
	Plain->BlendMode = BLEND_Translucent;
	Plain->GetEditorOnlyData()->Opacity.Connect(0, AddNumber(Plain, 0.4f));
	TestFalse(TEXT("a constant opacity publishes"), FMantlePlaceAssets::UnpublishableMaterialWhy(Plain, Why));
	TestFalse(TEXT("an opaque material publishes"), FMantlePlaceAssets::UnpublishableMaterialWhy(NewObject<UMaterial>(GetTransientPackage()), Why));
	TestFalse(TEXT("no material is nothing to say"), FMantlePlaceAssets::UnpublishableMaterialWhy(nullptr, Why));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVolumeFogMaterialTest,
	"Mantle.Editor.Material.VolumeFogIsUnpublishable",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVolumeFogMaterialTest::RunTest(const FString& Parameters)
{
	using namespace MantleCloudCardMaterialTestPrivate;

	// M_VolumeFogDetail: additive, Volume domain.
	UMaterial* Fog = NewObject<UMaterial>(GetTransientPackage());
	Fog->MaterialDomain = MD_Volume;
	Fog->BlendMode = BLEND_Additive;
	FString Why;
	TestTrue(TEXT("a volume material has no surface to draw"), FMantlePlaceAssets::UnpublishableMaterialWhy(Fog, Why));
	TestTrue(TEXT("and says it is volume fog"), Why.Contains(TEXT("volume")));

	// An additive surface material (a glow) is carried, not left out.
	UMaterial* Glow = NewObject<UMaterial>(GetTransientPackage());
	Glow->BlendMode = BLEND_Additive;
	TestFalse(TEXT("an additive surface material publishes"), FMantlePlaceAssets::UnpublishableMaterialWhy(Glow, Why));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCloudCardActorSkipTest,
	"Mantle.Editor.Material.CloudCardActorIsSkippedAndSaid",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCloudCardActorSkipTest::RunTest(const FString& Parameters)
{
	using namespace MantleCloudCardMaterialTestPrivate;

	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("the engine cube"), Cube))
	{
		return false;
	}
	AStaticMeshActor* Actor = NewObject<AStaticMeshActor>(GetTransientPackage());
	Actor->GetStaticMeshComponent()->SetStaticMesh(Cube);
	Actor->GetStaticMeshComponent()->SetMaterial(0, NewCloudCard());

	FMantlePlaceExporter::FSkippedActor Skipped;
	TestTrue(TEXT("a mesh drawn only with a cloud card is skipped"), FMantlePlaceExporter::DescribeSkippedActor(Actor, Skipped));
	TestTrue(TEXT("it is a loss, so it is reported as dropped"), Skipped.bLoss);
	TestTrue(TEXT("and the line starts the reason with 'Skipped:'"), Skipped.What.Contains(TEXT("Skipped:")));
	TestEqual(TEXT("under the static meshes"), FString(Skipped.Category), FString(MantleCoverageCategory::StaticMeshes));

	// The same mesh with a material that publishes keeps its mesh.
	UMaterial* Plain = NewObject<UMaterial>(GetTransientPackage());
	Actor->GetStaticMeshComponent()->SetMaterial(0, Plain);
	TestFalse(TEXT("an ordinary material is not skipped"), FMantlePlaceExporter::DescribeSkippedActor(Actor, Skipped));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
