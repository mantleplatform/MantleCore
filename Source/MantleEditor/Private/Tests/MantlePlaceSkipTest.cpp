// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCoverage.h"
#include "MantlePlaceAssets.h"
#include "MantlePlaceBuilder.h"
#include "MantlePlaceExporter.h"
#include "MantlePlaceValidator.h"

#include "Components/AudioComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/WindDirectionalSourceComponent.h"
#include "Distributions/DistributionFloatConstant.h"
#include "Engine/DecalActor.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "ImageCore.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionMakeMaterialAttributes.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/AutomationTest.h"
#include "Particles/Emitter.h"
#include "Particles/ParticleLODLevel.h"
#include "Particles/ParticleModuleRequired.h"
#include "Particles/ParticleSpriteEmitter.h"
#include "Particles/ParticleSystem.h"
#include "Particles/ParticleSystemComponent.h"
#include "Particles/Spawn/ParticleModuleSpawn.h"
#include "Particles/TypeData/ParticleModuleTypeDataMesh.h"
#include "Sound/AmbientSound.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundNodeWavePlayer.h"
#include "Sound/SoundWave.h"
#include "UObject/Package.h"
#include "VT/RuntimeVirtualTexture.h"

#if WITH_DEV_AUTOMATION_TESTS

// GhzGarage/mantle#818: content a real Fab map has that Mantle cannot publish is left out with a warning and a line in
// the coverage report, instead of refusing the whole place. One test per kind: a tiny level with that actor, an export
// that does not refuse it, the actor skipped (or translated), and the coverage line and the Check warning in plain words.
// Built in a transient editor world. Classes from modules MantleEditor does not link (camera rigs, level sequences, Niagara)
// are found by path and the test says so when the build lacks them.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantlePlaceSkipTestPrivate
{
	using EOutcome = EMantleCoverageOutcome;

	/** An empty editor world for one test, destroyed with it. */
	struct FSkipWorld
	{
		UWorld* World = nullptr;

		FSkipWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld*/ false, TEXT("MantlePlaceSkipTestWorld"));
		}

		~FSkipWorld()
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

	/** An actor of the class at Path (a script path), labeled; null when the build has no such class. */
	AActor* SpawnByPath(UWorld* World, const TCHAR* Path, const TCHAR* Label)
	{
		UClass* Class = LoadClass<AActor>(nullptr, Path);
		AActor* Actor = Class ? World->SpawnActor<AActor>(Class, FTransform::Identity) : nullptr;
		if (Actor)
		{
			Actor->SetActorLabel(Label);
		}
		return Actor;
	}

	bool MentionsLabel(const FMantleExportResult& Result, const FString& Label)
	{
		return Result.Errors.ContainsByPredicate([&Label](const FMantleParseError& Error)
		{
			return Error.Path.Contains(Label) || Error.Message.Contains(Label);
		});
	}

	const FMantleCoverageExample* FindExample(const FMantleCoverage& Coverage, const TCHAR* Category, EOutcome Outcome, const FString& Label)
	{
		const FMantleCoverageCategory* Found = Coverage.Find(Category);
		return Found ? Found->Get(Outcome).Examples.FindByPredicate([&Label](const FMantleCoverageExample& Example) { return Example.Name == Label; }) : nullptr;
	}

	int32 CountType(const FMantlePlace& Place, EMantleEntityType Type)
	{
		int32 Count = 0;
		for (const FMantleEntity& Entity : Place.Entities)
		{
			Count += Entity.Type == Type ? 1 : 0;
		}
		return Count;
	}

	bool HasIssue(const FMantleValidationResult& Validation, const FString& Label, EMantleIssueSeverity Severity)
	{
		return Validation.Issues.ContainsByPredicate([&Label, Severity](const FMantleValidationIssue& Issue)
		{
			return Issue.Actor == Label && Issue.Severity == Severity;
		});
	}

	/**
	 * What every skipped kind shares: Export does not refuse the actor, the coverage lists it as Dropped in Category in words
	 * that name what is wrong, and Check warns about it without an error.
	 */
	void ExpectSkipped(FAutomationTestBase& Test, UWorld* World, const FString& Label, const TCHAR* Category)
	{
		const FMantleExportResult Result = FMantlePlaceExporter::Export(World, FMantleExportOptions());
		Test.TestFalse(TEXT("export does not refuse it"), MentionsLabel(Result, Label));
		const FMantleCoverageExample* Line = FindExample(Result.Coverage, Category, EOutcome::Dropped, Label);
		if (Test.TestNotNull(TEXT("the coverage lists it as dropped"), Line))
		{
			Test.TestTrue(TEXT("in words, not a class dump"), Line->Detail.Contains(TEXT("left out")) || Line->Detail.Contains(TEXT("not")));
		}

		const FMantleValidationResult Validation = FMantlePlaceValidator::Validate(World);
		Test.TestTrue(TEXT("Check warns about it"), HasIssue(Validation, Label, EMantleIssueSeverity::Warning));
		Test.TestFalse(TEXT("and raises no error for it"), HasIssue(Validation, Label, EMantleIssueSeverity::Error));
	}

	/** A deferred decal material that sets a BaseColor texture, the way a creator's decal does. */
	UMaterial* MakeDecalMaterial()
	{
		UMaterial* Material = NewObject<UMaterial>(GetTransientPackage());
		Material->MaterialDomain = MD_DeferredDecal;
		UMaterialExpressionTextureSampleParameter2D* Parameter = NewObject<UMaterialExpressionTextureSampleParameter2D>(Material);
		Parameter->Material = Material;
		Parameter->ParameterName = FName(FMantleDecalMaterials::BaseColorParameter);
		Parameter->Texture = UTexture2D::CreateTransient(4, 4);
		Material->GetExpressionCollection().AddExpression(Parameter);
		Material->UpdateCachedExpressionData();
		return Material;
	}

	/** A Cascade system with one sprite emitter the particle translation carries. */
	UParticleSystem* MakeSprayHeadSystem(const FName Name = NAME_None)
	{
		UParticleSystem* System = NewObject<UParticleSystem>(GetTransientPackage(), Name);
		UParticleSpriteEmitter* Emitter = NewObject<UParticleSpriteEmitter>(System);
		UParticleLODLevel* Lod = NewObject<UParticleLODLevel>(Emitter);
		Lod->Level = 0;
		Lod->bEnabled = true;
		UParticleModuleRequired* Required = NewObject<UParticleModuleRequired>(System);
		Required->Material = NewObject<UMaterial>(GetTransientPackage());
		Required->EmitterLoops = 0;
		Required->EmitterDuration = 1.0f;
		Lod->RequiredModule = Required;
		UParticleModuleSpawn* Spawn = NewObject<UParticleModuleSpawn>(System);
		UDistributionFloatConstant* Rate = NewObject<UDistributionFloatConstant>(Spawn);
		Rate->Constant = 20.0f;
		Spawn->Rate.Distribution = Rate;
		Lod->SpawnModule = Spawn;
		Emitter->LODLevels.Add(Lod);
		System->Emitters.Add(Emitter);
		return System;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceSkipDecalWithoutTextureTest,
	"Mantle.Editor.PlaceSkip.DecalWithoutBaseColorIsSkipped",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceSkipDecalWithoutTextureTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceSkipTestPrivate;
	FSkipWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	// A decal with no material has no texture to publish.
	ADecalActor* Bare = Fixture.World->SpawnActor<ADecalActor>(FVector::ZeroVector, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("the decal"), Bare))
	{
		return false;
	}
	Bare->SetActorLabel(TEXT("Dirt decal"));
	ExpectSkipped(*this, Fixture.World, TEXT("Dirt decal"), MantleCoverageCategory::Decals);
	const FMantleExportResult Result = FMantlePlaceExporter::Export(Fixture.World, FMantleExportOptions());
	TestEqual(TEXT("no Decal entity is written"), CountType(Result.Place, EMantleEntityType::Decal), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceSkipDecalNegativeScaleTest,
	"Mantle.Editor.PlaceSkip.DecalNegativeScaleIsPublishedUnmirrored",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceSkipDecalNegativeScaleTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceSkipTestPrivate;
	FSkipWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	ADecalActor* Decal = Fixture.World->SpawnActor<ADecalActor>(FVector(0.0, 0.0, 50.0), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("the decal"), Decal))
	{
		return false;
	}
	Decal->SetActorLabel(TEXT("Mirrored decal"));
	Decal->SetDecalMaterial(MakeDecalMaterial());
	Decal->SetActorScale3D(FVector(1.0, -2.0, 1.5));

	const FMantleExportResult Result = FMantlePlaceExporter::Export(Fixture.World, FMantleExportOptions());
	TestFalse(TEXT("export does not refuse it"), MentionsLabel(Result, TEXT("Mirrored decal")));
	const FMantleEntity* Entity = Result.Place.Entities.FindByPredicate([](const FMantleEntity& E) { return E.Type == EMantleEntityType::Decal; });
	if (TestNotNull(TEXT("a Decal entity"), Entity))
	{
		TestTrue(TEXT("at the positive scale"), Entity->Transform.GetScale3D().Equals(FVector(1.0, 2.0, 1.5), 0.0001));
	}
	TestNotNull(TEXT("the coverage says it is approximated"), FindExample(Result.Coverage, MantleCoverageCategory::Decals, EOutcome::Approximated, TEXT("Mirrored decal")));

	const FMantleValidationResult Validation = FMantlePlaceValidator::Validate(Fixture.World);
	TestTrue(TEXT("Check warns that the mirroring is lost"), HasIssue(Validation, TEXT("Mirrored decal"), EMantleIssueSeverity::Warning));
	TestFalse(TEXT("and raises no error for the scale"), HasIssue(Validation, TEXT("Mirrored decal"), EMantleIssueSeverity::Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceSkipDecalTextureFromGraphTest,
	"Mantle.Editor.PlaceSkip.DecalTextureIsFollowedThroughTheGraph",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceSkipDecalTextureFromGraphTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceSkipTestPrivate;
	FSkipWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	auto SpawnDecal = [&Fixture](const TCHAR* Label, UMaterialInterface* Material)
	{
		ADecalActor* Decal = Fixture.World->SpawnActor<ADecalActor>(FVector::ZeroVector, FRotator::ZeroRotator);
		Decal->SetActorLabel(Label);
		Decal->SetDecalMaterial(Material);
		return Decal;
	};
	FMantleDecalProps Props;
	UTexture* BaseColor = nullptr;
	UTexture* Normal = nullptr;
	FString Problem;

	// 1. A Fab master whose colour texture parameter has another name ("BaseLayer_B Texture"),
	// through a Multiply into Base Color, and an instance that overrides it: the override is read.
	UMaterial* Master = NewObject<UMaterial>(GetTransientPackage());
	Master->MaterialDomain = MD_DeferredDecal;
	const FName PackParameter(TEXT("BaseLayer_B Texture"));
	UMaterialExpressionTextureSampleParameter2D* Parameter = NewObject<UMaterialExpressionTextureSampleParameter2D>(Master);
	Parameter->Material = Master;
	Parameter->ParameterName = PackParameter;
	Parameter->Texture = UTexture2D::CreateTransient(4, 4);
	UMaterialExpressionConstant* Tint = NewObject<UMaterialExpressionConstant>(Master);
	Tint->Material = Master;
	Tint->R = 0.8f;
	UMaterialExpressionMultiply* Multiply = NewObject<UMaterialExpressionMultiply>(Master);
	Multiply->Material = Master;
	Multiply->A.Expression = Parameter;
	Multiply->B.Expression = Tint;
	for (UMaterialExpression* Expression : { static_cast<UMaterialExpression*>(Parameter), static_cast<UMaterialExpression*>(Tint), static_cast<UMaterialExpression*>(Multiply) })
	{
		Master->GetExpressionCollection().AddExpression(Expression);
	}
	Master->GetEditorOnlyData()->BaseColor.Expression = Multiply;
	Master->UpdateCachedExpressionData();
	UTexture2D* Moss = UTexture2D::CreateTransient(4, 4);
	UMaterialInstanceConstant* Instance = NewObject<UMaterialInstanceConstant>(GetTransientPackage());
	Instance->SetParentEditorOnly(Master, false);
	Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(PackParameter), Moss);

	ADecalActor* Named = SpawnDecal(TEXT("Moss decal"), Instance);
	if (TestTrue(TEXT("a decal whose colour parameter has another name is read"), FMantlePlaceExporter::ReadDecal(Named, Props, BaseColor, Normal, Problem)))
	{
		TestTrue(TEXT("with the instance's texture, not the master's default"), BaseColor == Moss);
		TestNull(TEXT("and no normal map"), Normal);
	}

	// 2. A decal material with plain Texture Samples and no parameters at all.
	UMaterial* Plain = NewObject<UMaterial>(GetTransientPackage());
	Plain->MaterialDomain = MD_DeferredDecal;
	UTexture2D* Crack = UTexture2D::CreateTransient(4, 4);
	UTexture2D* CrackNormal = UTexture2D::CreateTransient(4, 4);
	UMaterialExpressionTextureSample* ColorSample = NewObject<UMaterialExpressionTextureSample>(Plain);
	ColorSample->Material = Plain;
	ColorSample->Texture = Crack;
	UMaterialExpressionTextureSample* NormalSample = NewObject<UMaterialExpressionTextureSample>(Plain);
	NormalSample->Material = Plain;
	NormalSample->Texture = CrackNormal;
	Plain->GetExpressionCollection().AddExpression(ColorSample);
	Plain->GetExpressionCollection().AddExpression(NormalSample);
	Plain->GetEditorOnlyData()->BaseColor.Expression = ColorSample;
	Plain->GetEditorOnlyData()->Normal.Expression = NormalSample;
	Plain->UpdateCachedExpressionData();

	ADecalActor* Sampled = SpawnDecal(TEXT("Crack decal"), Plain);
	if (TestTrue(TEXT("a decal with a plain Texture Sample is read"), FMantlePlaceExporter::ReadDecal(Sampled, Props, BaseColor, Normal, Problem)))
	{
		TestTrue(TEXT("with the texture feeding Base Color"), BaseColor == Crack);
		TestTrue(TEXT("and the one feeding Normal"), Normal == CrackNormal);
	}

	// 3. Still refused, in words, when no texture feeds Base Color.
	UMaterial* Empty = NewObject<UMaterial>(GetTransientPackage());
	Empty->MaterialDomain = MD_DeferredDecal;
	ADecalActor* Untextured = SpawnDecal(TEXT("Flat decal"), Empty);
	TestFalse(TEXT("a decal material with no texture is refused"), FMantlePlaceExporter::ReadDecal(Untextured, Props, BaseColor, Normal, Problem));
	TestTrue(TEXT("saying no texture feeds its Base Color"), Problem.Contains(TEXT("Base Color")));

	// 4. Refused when the texture feeding Base Color has no alpha (a pack that keeps its shape in a
	// separate mask): it would publish as a solid rectangle.
	UMaterial* Masked = NewObject<UMaterial>(GetTransientPackage());
	Masked->MaterialDomain = MD_DeferredDecal;
	UMaterialExpressionTextureSample* OpaqueSample = NewObject<UMaterialExpressionTextureSample>(Masked);
	OpaqueSample->Material = Masked;
	OpaqueSample->Texture = UTexture2D::CreateTransient(4, 4, PF_DXT1);
	Masked->GetExpressionCollection().AddExpression(OpaqueSample);
	Masked->GetEditorOnlyData()->BaseColor.Expression = OpaqueSample;
	Masked->UpdateCachedExpressionData();
	ADecalActor* NoAlpha = SpawnDecal(TEXT("Oil decal"), Masked);
	TestFalse(TEXT("a decal whose colour texture has no alpha is refused"), FMantlePlaceExporter::ReadDecal(NoAlpha, Props, BaseColor, Normal, Problem));
	TestTrue(TEXT("saying the shape must be in its alpha"), Problem.Contains(TEXT("no alpha channel")));

	// Export publishes the two and leaves the others out.
	const FMantleExportResult Result = FMantlePlaceExporter::Export(Fixture.World, FMantleExportOptions());
	TestEqual(TEXT("two Decal entities are written"), CountType(Result.Place, EMantleEntityType::Decal), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceSkipDecalTextureInFunctionTest,
	"Mantle.Editor.PlaceSkip.DecalTextureInsideAMaterialFunctionIsFound",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceSkipDecalTextureInFunctionTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceSkipTestPrivate;
	FSkipWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	// The Overgrown Factory pack's shape: a master on material attributes calls a layer function, and
	// the colour texture parameter ("BaseLayer_B Texture") lives inside that function, feeding a Make
	// Material Attributes through a Function Output. Instances override the parameter.
	const FName PackParameter(TEXT("BaseLayer_B Texture"));
	UMaterialFunction* Layer = NewObject<UMaterialFunction>(GetTransientPackage());
	UMaterialExpressionTextureSampleParameter2D* Parameter = NewObject<UMaterialExpressionTextureSampleParameter2D>(Layer);
	Parameter->Function = Layer;
	Parameter->ParameterName = PackParameter;
	Parameter->Texture = UTexture2D::CreateTransient(4, 4);
	UMaterialExpressionMakeMaterialAttributes* Attributes = NewObject<UMaterialExpressionMakeMaterialAttributes>(Layer);
	Attributes->Function = Layer;
	Attributes->BaseColor.Expression = Parameter;
	UMaterialExpressionFunctionOutput* Output = NewObject<UMaterialExpressionFunctionOutput>(Layer);
	Output->Function = Layer;
	Output->OutputName = TEXT("Result");
	Output->A.Expression = Attributes;
	Output->Id = FGuid::NewGuid();
	for (UMaterialExpression* Expression : { static_cast<UMaterialExpression*>(Parameter), static_cast<UMaterialExpression*>(Attributes), static_cast<UMaterialExpression*>(Output) })
	{
		Layer->GetExpressionCollection().AddExpression(Expression);
	}

	UMaterial* Master = NewObject<UMaterial>(GetTransientPackage());
	Master->MaterialDomain = MD_DeferredDecal;
	Master->bUseMaterialAttributes = true;
	UMaterialExpressionMaterialFunctionCall* Call = NewObject<UMaterialExpressionMaterialFunctionCall>(Master);
	Call->Material = Master;
	Call->SetMaterialFunction(Layer);
	Master->GetExpressionCollection().AddExpression(Call);
	Master->GetEditorOnlyData()->MaterialAttributes.Expression = Call;
	Master->UpdateCachedExpressionData();
	TestEqual(TEXT("the call sees the function's one output"), Call->FunctionOutputs.Num(), 1);

	UTexture2D* Moss = UTexture2D::CreateTransient(4, 4);
	UMaterialInstanceConstant* Instance = NewObject<UMaterialInstanceConstant>(GetTransientPackage());
	Instance->SetParentEditorOnly(Master, false);
	Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(PackParameter), Moss);

	ADecalActor* Decal = Fixture.World->SpawnActor<ADecalActor>(FVector::ZeroVector, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("the decal"), Decal))
	{
		return false;
	}
	Decal->SetActorLabel(TEXT("Moss decal"));
	Decal->SetDecalMaterial(Instance);
	FMantleDecalProps Props;
	UTexture* BaseColor = nullptr;
	UTexture* Normal = nullptr;
	FString Problem;
	if (TestTrue(TEXT("a decal whose colour texture is inside a material function is read"), FMantlePlaceExporter::ReadDecal(Decal, Props, BaseColor, Normal, Problem)))
	{
		TestTrue(TEXT("with the instance's texture"), BaseColor == Moss);
	}
	else
	{
		AddInfo(Problem);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceSkipDecalOpacityBakeTest,
	"Mantle.Editor.PlaceSkip.DecalOpacityIsBakedIntoTheAlpha",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceSkipDecalOpacityBakeTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceSkipTestPrivate;
	FSkipWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	// A 4x4 texture with source pixels, as an imported one has.
	auto MakeSourceTexture = [](const TCHAR* Name, bool bSRGB, TFunctionRef<FColor(int32 X, int32 Y)> Pixel)
	{
		TArray<FColor> Pixels;
		for (int32 Y = 0; Y < 4; ++Y)
		{
			for (int32 X = 0; X < 4; ++X)
			{
				Pixels.Add(Pixel(X, Y));
			}
		}
		UTexture2D* Texture = NewObject<UTexture2D>(GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UTexture2D::StaticClass(), FName(Name)));
		Texture->SRGB = bSRGB;
		Texture->Source.Init(4, 4, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Pixels.GetData()));
		return Texture;
	};
	// The Prison pack's shape: an opaque colour texture, and Opacity = its alpha times a damage mask's red.
	UTexture2D* Colour = MakeSourceTexture(TEXT("T_Decal_Test"), true, [](int32, int32) { return FColor(200, 100, 50, 255); });
	UTexture2D* DamageMask = MakeSourceTexture(TEXT("T_DamageMask_Test"), false, [](int32 X, int32) { return X < 2 ? FColor(255, 0, 0, 255) : FColor(0, 0, 0, 255); });

	UMaterial* Material = NewObject<UMaterial>(GetTransientPackage());
	Material->MaterialDomain = MD_DeferredDecal;
	UMaterialExpressionTextureSample* ColourSample = NewObject<UMaterialExpressionTextureSample>(Material);
	ColourSample->Material = Material;
	ColourSample->Texture = Colour;
	UMaterialExpressionTextureSample* MaskSample = NewObject<UMaterialExpressionTextureSample>(Material);
	MaskSample->Material = Material;
	MaskSample->Texture = DamageMask;
	UMaterialExpressionMultiply* Multiply = NewObject<UMaterialExpressionMultiply>(Material);
	Multiply->Material = Material;
	Multiply->A.Connect(4, ColourSample); // A
	Multiply->B.Connect(0, MaskSample); // RGB
	for (UMaterialExpression* Expression : { static_cast<UMaterialExpression*>(ColourSample), static_cast<UMaterialExpression*>(MaskSample), static_cast<UMaterialExpression*>(Multiply) })
	{
		Material->GetExpressionCollection().AddExpression(Expression);
	}
	Material->GetEditorOnlyData()->BaseColor.Connect(0, ColourSample);
	Material->GetEditorOnlyData()->Opacity.Connect(0, Multiply);
	Material->UpdateCachedExpressionData();

	ADecalActor* Decal = Fixture.World->SpawnActor<ADecalActor>(FVector::ZeroVector, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("the decal"), Decal))
	{
		return false;
	}
	Decal->SetActorLabel(TEXT("Road marking"));
	Decal->SetDecalMaterial(Material);
	FMantleDecalProps Props;
	UTexture* BaseColor = nullptr;
	UTexture* Normal = nullptr;
	FString Problem;
	if (!TestTrue(TEXT("a decal whose shape is in its Opacity is read"), FMantlePlaceExporter::ReadDecal(Decal, Props, BaseColor, Normal, Problem)))
	{
		AddInfo(Problem);
		return true;
	}
	if (!TestNotNull(TEXT("with a texture"), BaseColor) || !TestTrue(TEXT("baked, not the colour texture as it is"), BaseColor != Colour))
	{
		return true;
	}
	FImage Baked;
	if (TestTrue(TEXT("the baked texture has source pixels"), BaseColor->Source.GetMipImage(Baked, 0)))
	{
		TestEqual(TEXT("at the colour texture's size"), Baked.SizeX, 4);
		const TArrayView64<FColor> Pixels = Baked.AsBGRA8();
		TestEqual(TEXT("the colour is kept"), Pixels[0].R, static_cast<uint8>(200));
		TestEqual(TEXT("its green too"), Pixels[0].G, static_cast<uint8>(100));
		TestEqual(TEXT("where the mask is on, the decal shows"), Pixels[1].A, static_cast<uint8>(255));
		TestEqual(TEXT("where it is off, the decal is clear"), Pixels[3].A, static_cast<uint8>(0));
	}
	UTexture* Again = nullptr;
	FMantlePlaceExporter::ReadDecal(Decal, Props, Again, Normal, Problem);
	TestTrue(TEXT("a second read gives the same baked texture, so gathering and export agree"), Again == BaseColor);

	const FMantleExportResult Result = FMantlePlaceExporter::Export(Fixture.World, FMantleExportOptions());
	TestEqual(TEXT("the decal is written"), CountType(Result.Place, EMantleEntityType::Decal), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceSkipDecalAttributesOpacityTest,
	"Mantle.Editor.PlaceSkip.DecalOnMaterialAttributesGetsItsShape",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceSkipDecalAttributesOpacityTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceSkipTestPrivate;
	FSkipWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	auto MakeSourceTexture = [](const TCHAR* Name, bool bSRGB, TFunctionRef<FColor(int32 X, int32 Y)> Pixel)
	{
		TArray<FColor> Pixels;
		for (int32 Y = 0; Y < 4; ++Y)
		{
			for (int32 X = 0; X < 4; ++X)
			{
				Pixels.Add(Pixel(X, Y));
			}
		}
		UTexture2D* Texture = NewObject<UTexture2D>(GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UTexture2D::StaticClass(), FName(Name)));
		Texture->SRGB = bSRGB;
		Texture->Source.Init(4, 4, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Pixels.GetData()));
		return Texture;
	};
	// The Overgrown Factory shape: a master on material attributes calls a layer function, whose
	// Make Material Attributes takes Opacity = EnableOpacity (a static switch, on) times an Opacity
	// scalar. Built with the switch's True branch fed by OpacitySource.
	auto MakeLayerMaster = [](UTexture* ColourTexture, TFunctionRef<FExpressionInput(UMaterialFunction*, UMaterialExpression* Colour)> OpacitySource)
	{
		UMaterialFunction* Layer = NewObject<UMaterialFunction>(GetTransientPackage());
		UMaterialExpressionTextureSampleParameter2D* Colour = NewObject<UMaterialExpressionTextureSampleParameter2D>(Layer);
		Colour->Function = Layer;
		Colour->ParameterName = TEXT("BaseLayer_B Texture");
		Colour->Texture = ColourTexture;
		UMaterialExpressionStaticSwitchParameter* Enable = NewObject<UMaterialExpressionStaticSwitchParameter>(Layer);
		Enable->Function = Layer;
		Enable->ParameterName = TEXT("EnableOpacity");
		Enable->DefaultValue = true;
		Enable->A = OpacitySource(Layer, Colour);
		UMaterialExpressionScalarParameter* Opacity = NewObject<UMaterialExpressionScalarParameter>(Layer);
		Opacity->Function = Layer;
		Opacity->ParameterName = TEXT("Opacity");
		Opacity->DefaultValue = 1.0f;
		UMaterialExpressionMultiply* Multiply = NewObject<UMaterialExpressionMultiply>(Layer);
		Multiply->Function = Layer;
		Multiply->A.Connect(0, Enable);
		Multiply->B.Connect(0, Opacity);
		UMaterialExpressionMakeMaterialAttributes* Attributes = NewObject<UMaterialExpressionMakeMaterialAttributes>(Layer);
		Attributes->Function = Layer;
		Attributes->BaseColor.Connect(0, Colour);
		Attributes->Opacity.Connect(0, Multiply);
		UMaterialExpressionFunctionOutput* Output = NewObject<UMaterialExpressionFunctionOutput>(Layer);
		Output->Function = Layer;
		Output->OutputName = TEXT("Result");
		Output->A.Connect(0, Attributes);
		Output->Id = FGuid::NewGuid();
		for (UMaterialExpression* Expression : { static_cast<UMaterialExpression*>(Colour), static_cast<UMaterialExpression*>(Enable),
			static_cast<UMaterialExpression*>(Opacity), static_cast<UMaterialExpression*>(Multiply), static_cast<UMaterialExpression*>(Attributes), static_cast<UMaterialExpression*>(Output) })
		{
			Layer->GetExpressionCollection().AddExpression(Expression);
		}
		UMaterial* Master = NewObject<UMaterial>(GetTransientPackage());
		Master->MaterialDomain = MD_DeferredDecal;
		Master->bUseMaterialAttributes = true;
		UMaterialExpressionMaterialFunctionCall* Call = NewObject<UMaterialExpressionMaterialFunctionCall>(Master);
		Call->Material = Master;
		Call->SetMaterialFunction(Layer);
		Master->GetExpressionCollection().AddExpression(Call);
		Master->GetEditorOnlyData()->MaterialAttributes.Connect(0, Call);
		Master->UpdateCachedExpressionData();
		return Master;
	};
	auto Read = [&Fixture](const TCHAR* Label, UMaterialInterface* Material, UTexture*& OutBaseColor, FString& OutProblem)
	{
		ADecalActor* Decal = Fixture.World->SpawnActor<ADecalActor>(FVector::ZeroVector, FRotator::ZeroRotator);
		Decal->SetActorLabel(Label);
		Decal->SetDecalMaterial(Material);
		FMantleDecalProps Props;
		UTexture* Normal = nullptr;
		return FMantlePlaceExporter::ReadDecal(Decal, Props, OutBaseColor, Normal, OutProblem);
	};
	UTexture* BaseColor = nullptr;
	FString Problem;

	// 1. The shape in the colour texture's own alpha (EnableOpacity takes its A), as the Factory pack's
	// _BC textures have it: published as it is, the alpha read from the source pixels.
	UTexture2D* Moss = MakeSourceTexture(TEXT("T_MossDecal_Test"), true, [](int32 X, int32) { return FColor(60, 90, 40, X < 2 ? 255 : 0); });
	UMaterial* OwnAlpha = MakeLayerMaster(Moss, [](UMaterialFunction*, UMaterialExpression* Colour) { FExpressionInput In; In.Connect(4, Colour); return In; });
	if (TestTrue(TEXT("a material-attributes decal whose shape is its colour alpha is read"), Read(TEXT("Moss decal"), OwnAlpha, BaseColor, Problem)))
	{
		TestTrue(TEXT("with the colour texture as it is"), BaseColor == Moss);
	}
	else
	{
		AddInfo(Problem);
	}

	// 2. The shape in a separate mask through the switch: baked into the colour texture's alpha.
	UTexture2D* Leak = MakeSourceTexture(TEXT("T_LeakDecal_Test"), true, [](int32, int32) { return FColor(30, 30, 30, 255); });
	UTexture2D* LeakMask = MakeSourceTexture(TEXT("T_LeakMask_Test"), false, [](int32 X, int32) { return FColor(X < 2 ? 255 : 0, 0, 0, 255); });
	UMaterial* Masked = MakeLayerMaster(Leak, [LeakMask](UMaterialFunction* Layer, UMaterialExpression*)
	{
		UMaterialExpressionTextureSample* Mask = NewObject<UMaterialExpressionTextureSample>(Layer);
		Mask->Function = Layer;
		Mask->Texture = LeakMask;
		Layer->GetExpressionCollection().AddExpression(Mask);
		FExpressionInput In;
		In.Connect(1, Mask); // R
		return In;
	});
	if (TestTrue(TEXT("a material-attributes decal whose shape is a mask is read"), Read(TEXT("Leak decal"), Masked, BaseColor, Problem)))
	{
		FImage Baked;
		if (TestTrue(TEXT("baked from the mask"), BaseColor != Leak && BaseColor && BaseColor->Source.GetMipImage(Baked, 0)))
		{
			const TArrayView64<FColor> Pixels = Baked.AsBGRA8();
			TestEqual(TEXT("where the mask is on, the decal shows"), Pixels[0].A, static_cast<uint8>(255));
			TestEqual(TEXT("where it is off, the decal is clear"), Pixels[2].A, static_cast<uint8>(0));
		}
	}
	else
	{
		AddInfo(Problem);
	}

	// 3. An all-opaque colour alpha with no mask: still refused, as a solid rectangle would be wrong.
	UTexture2D* Solid = MakeSourceTexture(TEXT("T_Solid_Test"), true, [](int32, int32) { return FColor(30, 30, 30, 255); });
	UMaterial* NoShape = MakeLayerMaster(Solid, [](UMaterialFunction*, UMaterialExpression* Colour) { FExpressionInput In; In.Connect(4, Colour); return In; });
	TestFalse(TEXT("a decal with no shape anywhere is refused"), Read(TEXT("Solid decal"), NoShape, BaseColor, Problem));
	TestTrue(TEXT("saying its alpha holds no shape"), Problem.Contains(TEXT("no alpha channel")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceSkipParticleActorsTest,
	"Mantle.Editor.PlaceSkip.ParticleActorsAreTranslatedOrSkipped",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceSkipParticleActorsTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceSkipTestPrivate;
	FSkipWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}

	// A Cascade Emitter whose system translates is a Group with an Effect part.
	AEmitter* Fountain = Fixture.World->SpawnActor<AEmitter>(FVector(0.0, 0.0, 100.0), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("the emitter"), Fountain) || !TestNotNull(TEXT("its component"), Fountain->GetParticleSystemComponent()))
	{
		return false;
	}
	Fountain->SetActorLabel(TEXT("Fountain"));
	Fountain->GetParticleSystemComponent()->Template = MakeSprayHeadSystem();
	EMantleEntityType Type = EMantleEntityType::StaticMesh;
	TestTrue(TEXT("it is classified"), FMantlePlaceExporter::ClassifyActor(Fountain, Type));
	TestTrue(TEXT("as a Group"), Type == EMantleEntityType::Group);
	TestFalse(TEXT("and is not skipped"), FMantlePlaceExporter::IsEngineInfrastructure(Fountain));

	// An Emitter with no system has nothing to carry: skipped with a warning.
	AEmitter* Empty = Fixture.World->SpawnActor<AEmitter>(FVector(500.0, 0.0, 100.0), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("the empty emitter"), Empty))
	{
		return false;
	}
	Empty->SetActorLabel(TEXT("Empty smoke"));

	const FMantleExportResult Result = FMantlePlaceExporter::Export(Fixture.World, FMantleExportOptions());
	TestFalse(TEXT("export does not refuse the translated emitter"), MentionsLabel(Result, TEXT("Fountain")));
	TestEqual(TEXT("one Effect entity for its emitter"), CountType(Result.Place, EMantleEntityType::Effect), 1);
	TestFalse(TEXT("export does not refuse the empty one"), MentionsLabel(Result, TEXT("Empty smoke")));
	TestNotNull(TEXT("the coverage drops the empty one"), FindExample(Result.Coverage, MantleCoverageCategory::Particles, EOutcome::Dropped, TEXT("Empty smoke")));
	const FMantleValidationResult Validation = FMantlePlaceValidator::Validate(Fixture.World);
	TestTrue(TEXT("Check warns about the empty one"), HasIssue(Validation, TEXT("Empty smoke"), EMantleIssueSeverity::Warning));
	TestFalse(TEXT("with no error"), HasIssue(Validation, TEXT("Empty smoke"), EMantleIssueSeverity::Error));

	// A Niagara Actor is skipped the same way (no Niagara system is carried yet).
	if (SpawnByPath(Fixture.World, TEXT("/Script/Niagara.NiagaraActor"), TEXT("Niagara smoke")) != nullptr)
	{
		ExpectSkipped(*this, Fixture.World, TEXT("Niagara smoke"), MantleCoverageCategory::Particles);
	}
	else
	{
		AddWarning(TEXT("This build has no Niagara plugin, so the Niagara Actor case was not run."));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceSkipParticleReasonTest,
	"Mantle.Editor.PlaceSkip.ParticleActorSaysWhyItIsSkipped",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceSkipParticleReasonTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceSkipTestPrivate;
	FSkipWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}

	// An Emitter whose only emitter is a mesh emitter (the trailer's waterfall foam): skipped, with the translator's reason and the system's name.
	const FName FoamName = MakeUniqueObjectName(GetTransientPackage(), UParticleSystem::StaticClass(), TEXT("P_Foam_Heavy"));
	UParticleSystem* FoamSystem = MakeSprayHeadSystem(FoamName);
	FoamSystem->Emitters[0]->GetLODLevel(0)->TypeDataModule = NewObject<UParticleModuleTypeDataMesh>(FoamSystem);
	AEmitter* Foam = Fixture.World->SpawnActor<AEmitter>(FVector(0.0, 0.0, 100.0), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("the foam emitter"), Foam) || !TestNotNull(TEXT("its component"), Foam->GetParticleSystemComponent()))
	{
		return false;
	}
	Foam->GetParticleSystemComponent()->Template = FoamSystem;
	FMantlePlaceExporter::FSkippedActor Skipped;
	if (TestTrue(TEXT("the foam is skipped"), FMantlePlaceExporter::DescribeSkippedActor(Foam, Skipped)))
	{
		TestTrue(TEXT("and says it is a mesh emitter"), Skipped.What.Contains(TEXT("a mesh emitter")));
		TestTrue(TEXT("and names the particle system"), Skipped.What.Contains(TEXT("P_Foam_Heavy")));
		TestTrue(TEXT("and the remedy says only sprite emitters carry over"), Skipped.Remedy.Contains(TEXT("Only sprite emitters")));
		TestEqual(TEXT("under Particles"), Skipped.Category, FString(MantleCoverageCategory::Particles));
	}

	// An Emitter with no system has no reason to give: the general wording stays.
	AEmitter* Empty = Fixture.World->SpawnActor<AEmitter>(FVector(500.0, 0.0, 100.0), FRotator::ZeroRotator);
	if (TestNotNull(TEXT("the empty emitter"), Empty) && TestTrue(TEXT("it is skipped"), FMantlePlaceExporter::DescribeSkippedActor(Empty, Skipped)))
	{
		TestTrue(TEXT("with the general wording"), Skipped.What.Contains(TEXT("whose particles Mantle can't carry")));
	}

	// A sprite emitter still translates: not skipped.
	AEmitter* Spray = Fixture.World->SpawnActor<AEmitter>(FVector(1000.0, 0.0, 100.0), FRotator::ZeroRotator);
	if (TestNotNull(TEXT("the spray emitter"), Spray) && TestNotNull(TEXT("its component"), Spray->GetParticleSystemComponent()))
	{
		Spray->GetParticleSystemComponent()->Template = MakeSprayHeadSystem();
		TestFalse(TEXT("a sprite emitter is not skipped"), FMantlePlaceExporter::DescribeSkippedActor(Spray, Skipped));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceSkipRuntimeVirtualTextureVolumeTest,
	"Mantle.Editor.PlaceSkip.RuntimeVirtualTextureVolumeIsSkipped",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceSkipRuntimeVirtualTextureVolumeTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceSkipTestPrivate;
	FSkipWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	if (!TestNotNull(TEXT("the volume"), SpawnByPath(Fixture.World, TEXT("/Script/Engine.RuntimeVirtualTextureVolume"), TEXT("Landscape RVT"))))
	{
		return false;
	}
	ExpectSkipped(*this, Fixture.World, TEXT("Landscape RVT"), MantleCoverageCategory::Volumes);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceSkipVirtualTextureOnlyMeshTest,
	"Mantle.Editor.PlaceSkip.VirtualTextureOnlyMeshIsNotShown",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceSkipVirtualTextureOnlyMeshTest::RunTest(const FString& Parameters)
{
	// MedievalVillage's cart tracks: spline meshes that only write into the landscape's runtime virtual texture. Published,
	// they floated above the ground as flat cards.
	UStaticMeshComponent* Mesh = NewObject<UStaticMeshComponent>(GetTransientPackage());
	TestTrue(TEXT("a plain mesh is shown"), FMantlePlaceExporter::IsShownInGame(Mesh));
	Mesh->RuntimeVirtualTextures.Add(NewObject<URuntimeVirtualTexture>(GetTransientPackage()));
	TestTrue(TEXT("one that also writes a virtual texture is still drawn by default"), FMantlePlaceExporter::IsShownInGame(Mesh));
	Mesh->VirtualTextureRenderPassType = ERuntimeVirtualTextureMainPassType::Never;
	TestFalse(TEXT("one that never draws in the main pass is not shown"), FMantlePlaceExporter::IsShownInGame(Mesh));
	TestFalse(TEXT("nor is it game content, as it does not collide"),
		(Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision), FMantlePlaceExporter::IsGameContent(Mesh)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceSkipNonVisualActorsTest,
	"Mantle.Editor.PlaceSkip.CameraRigsAndEditorActorsAreSkipped",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceSkipNonVisualActorsTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceSkipTestPrivate;
	FSkipWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	struct FCase { const TCHAR* Path; const TCHAR* Label; };
	const FCase Cases[] = {
		{ TEXT("/Script/CinematicCamera.CameraRig_Rail"), TEXT("Rail rig") },
		{ TEXT("/Script/CinematicCamera.CameraRig_Crane"), TEXT("Crane rig") },
		{ TEXT("/Script/LevelSequence.LevelSequenceActor"), TEXT("Intro sequence") },
		{ TEXT("/Script/Engine.Note"), TEXT("Designer note") },
		{ TEXT("/Script/Engine.TargetPoint"), TEXT("Target") },
	};
	int32 Ran = 0;
	for (const FCase& Case : Cases)
	{
		if (SpawnByPath(Fixture.World, Case.Path, Case.Label))
		{
			++Ran;
		}
		else
		{
			AddWarning(FString::Printf(TEXT("This build has no %s, so that case was not run."), Case.Path));
		}
	}
	if (!TestTrue(TEXT("at least the engine's own classes spawned"), Ran >= 2))
	{
		return false;
	}
	for (const FCase& Case : Cases)
	{
		if (FindObject<UClass>(nullptr, Case.Path))
		{
			ExpectSkipped(*this, Fixture.World, Case.Label, MantleCoverageCategory::OtherActors);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceSkipStaticMeshWithoutMeshTest,
	"Mantle.Editor.PlaceSkip.StaticMeshActorWithoutMeshIsSkipped",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceSkipStaticMeshWithoutMeshTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceSkipTestPrivate;
	FSkipWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	AStaticMeshActor* Empty = Fixture.World->SpawnActor<AStaticMeshActor>(FVector::ZeroVector, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("the actor"), Empty))
	{
		return false;
	}
	Empty->SetActorLabel(TEXT("Empty mesh"));
	Empty->GetStaticMeshComponent()->SetStaticMesh(nullptr);
	ExpectSkipped(*this, Fixture.World, TEXT("Empty mesh"), MantleCoverageCategory::StaticMeshes);
	const FMantleExportResult Result = FMantlePlaceExporter::Export(Fixture.World, FMantleExportOptions());
	TestEqual(TEXT("no StaticMesh entity is written"), CountType(Result.Place, EMantleEntityType::StaticMesh), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceSkipSoundCueTest,
	"Mantle.Editor.PlaceSkip.SoundCueFallsBackToItsWave",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceSkipSoundCueTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceSkipTestPrivate;
	USoundWave* Wave = LoadObject<USoundWave>(nullptr, TEXT("/Engine/EditorSounds/Notifications/CompileSuccess.CompileSuccess"));
	if (!TestNotNull(TEXT("fixture sound wave"), Wave))
	{
		return false;
	}
	FSkipWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}

	// A cue that plays a wave publishes as that wave.
	USoundCue* Cue = NewObject<USoundCue>(GetTransientPackage());
	USoundNodeWavePlayer* Player = Cue->ConstructSoundNode<USoundNodeWavePlayer>();
	Player->SetSoundWave(Wave);
	Cue->FirstNode = Player;
	TestTrue(TEXT("the cue publishes as its wave"), FMantlePlaceAssets::PublishedSound(Cue) == Wave);
	TestTrue(TEXT("a wave publishes as itself"), FMantlePlaceAssets::PublishedSound(Wave) == Wave);

	AAmbientSound* Birds = Fixture.World->SpawnActor<AAmbientSound>(FVector(0.0, 0.0, 100.0), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("the ambient sound"), Birds))
	{
		return false;
	}
	Birds->SetActorLabel(TEXT("Birds"));
	Birds->GetAudioComponent()->SetSound(Cue);

	// A cue that plays nothing has no wave to fall back to: skipped with a warning.
	USoundCue* EmptyCue = NewObject<USoundCue>(GetTransientPackage());
	TestNull(TEXT("an empty cue has no wave"), FMantlePlaceAssets::PublishedSound(EmptyCue));
	AAmbientSound* Silent = Fixture.World->SpawnActor<AAmbientSound>(FVector(500.0, 0.0, 100.0), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("the silent sound"), Silent))
	{
		return false;
	}
	Silent->SetActorLabel(TEXT("Silent cue"));
	Silent->GetAudioComponent()->SetSound(EmptyCue);

	const FMantleExportResult Result = FMantlePlaceExporter::Export(Fixture.World, FMantleExportOptions());
	TestFalse(TEXT("export does not refuse the cue with a wave"), MentionsLabel(Result, TEXT("Birds")));
	TestEqual(TEXT("one SoundEmitter entity"), CountType(Result.Place, EMantleEntityType::SoundEmitter), 1);
	TestNotNull(TEXT("the coverage says it is approximated"), FindExample(Result.Coverage, MantleCoverageCategory::Sounds, EOutcome::Approximated, TEXT("Birds")));
	TestFalse(TEXT("export does not refuse the empty cue"), MentionsLabel(Result, TEXT("Silent cue")));
	TestNotNull(TEXT("the coverage drops the empty cue"), FindExample(Result.Coverage, MantleCoverageCategory::Sounds, EOutcome::Dropped, TEXT("Silent cue")));

	const FMantleValidationResult Validation = FMantlePlaceValidator::Validate(Fixture.World);
	TestTrue(TEXT("Check warns about the cue"), HasIssue(Validation, TEXT("Birds"), EMantleIssueSeverity::Warning));
	TestFalse(TEXT("with no error"), HasIssue(Validation, TEXT("Birds"), EMantleIssueSeverity::Error));
	TestTrue(TEXT("Check warns about the empty cue"), HasIssue(Validation, TEXT("Silent cue"), EMantleIssueSeverity::Warning));
	TestFalse(TEXT("with no error"), HasIssue(Validation, TEXT("Silent cue"), EMantleIssueSeverity::Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceSkipLevelInstanceTest,
	"Mantle.Editor.PlaceSkip.LevelInstanceIsNotRefused",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceSkipLevelInstanceTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceSkipTestPrivate;
	FSkipWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	// A Level Instance that is not loaded in the editor (this one points at no level): its contents cannot be published, so
	// it is named as dropped and the rest of the place publishes. One that is loaded has its actors in the world, and they
	// publish on their own; that needs a saved level, so it is the visual check's.
	if (!TestNotNull(TEXT("the level instance"), SpawnByPath(Fixture.World, TEXT("/Script/Engine.LevelInstance"), TEXT("Cabin instance"))))
	{
		return false;
	}
	ExpectSkipped(*this, Fixture.World, TEXT("Cabin instance"), MantleCoverageCategory::SubLevels);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceSkipActorWithNothingToShowTest,
	"Mantle.Editor.PlaceSkip.UnsupportedActorWithNothingToShowIsSkipped",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceSkipActorWithNothingToShowTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceSkipTestPrivate;
	FSkipWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	// An actor rooted on a wind source (a Blueprint like BP_Wind): a type Mantle does not know, with nothing to see, hear or touch.
	AActor* Holder = Fixture.World->SpawnActor<AActor>(FVector::ZeroVector, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("the actor"), Holder))
	{
		return false;
	}
	UWindDirectionalSourceComponent* Wind = NewObject<UWindDirectionalSourceComponent>(Holder, TEXT("Wind"));
	Holder->SetRootComponent(Wind);
	Wind->RegisterComponent();
	Holder->SetActorLabel(TEXT("BP_Wind"));
	ExpectSkipped(*this, Fixture.World, TEXT("BP_Wind"), MantleCoverageCategory::OtherActors);

	// Said in plain words, not as the generic refusal.
	const FMantleValidationResult Validation = FMantlePlaceValidator::Validate(Fixture.World);
	const FMantleValidationIssue* Issue = Validation.Issues.FindByPredicate([](const FMantleValidationIssue& Candidate) { return Candidate.Actor == TEXT("BP_Wind"); });
	if (TestNotNull(TEXT("Check names it"), Issue))
	{
		TestTrue(TEXT("as nothing to see, hear or touch"), Issue->Problem.Contains(TEXT("nothing a player could see, hear or touch")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceSkipUnsupportedActorWithContentTest,
	"Mantle.Editor.PlaceSkip.UnsupportedActorWithContentIsStillRefused",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceSkipUnsupportedActorWithContentTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceSkipTestPrivate;
	FSkipWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	// An actor rooted on a capsule that blocks (a prop's or a character's collision): a type Mantle does not know, and it collides, so a
	// player could touch what is lost. (A capsule that only overlaps is a trigger now, F18: it publishes as a box or sphere, with a Check note.)
	AActor* Holder = Fixture.World->SpawnActor<AActor>(FVector::ZeroVector, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("the actor"), Holder))
	{
		return false;
	}
	UCapsuleComponent* Capsule = NewObject<UCapsuleComponent>(Holder, TEXT("Body"));
	Holder->SetRootComponent(Capsule);
	Capsule->SetCollisionProfileName(TEXT("BlockAll"));
	Capsule->RegisterComponent();
	Holder->SetActorLabel(TEXT("Solid logic"));

	const FMantleExportResult Result = FMantlePlaceExporter::Export(Fixture.World, FMantleExportOptions());
	TestTrue(TEXT("export refuses it"), MentionsLabel(Result, Holder->GetName()));
	const FMantleValidationResult Validation = FMantlePlaceValidator::Validate(Fixture.World);
	TestTrue(TEXT("Check raises an error for it"), HasIssue(Validation, TEXT("Solid logic"), EMantleIssueSeverity::Error));
	TestFalse(TEXT("and no warning that it is left out"), HasIssue(Validation, TEXT("Solid logic"), EMantleIssueSeverity::Warning));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
