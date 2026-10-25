// Copyright Joshua Eger. All Rights Reserved.
#include "MantleClientFeatures.h"
#include "MantleCoverage.h"
#include "MantleEffectExport.h"
#include "MantleEffectTypes.h"
#include "MantlePlaceTypes.h"
#include "MantleTargetFeatures.h"

#include "Distributions/DistributionFloatConstant.h"
#include "Distributions/DistributionFloatConstantCurve.h"
#include "Distributions/DistributionFloatUniform.h"
#include "Distributions/DistributionVectorConstant.h"
#include "Distributions/DistributionVectorConstantCurve.h"
#include "Distributions/DistributionVectorUniform.h"
#include "Dom/JsonObject.h"
#include "Materials/Material.h"
#include "Misc/AutomationTest.h"
#include "Particles/Acceleration/ParticleModuleAccelerationConstant.h"
#include "Particles/Color/ParticleModuleColorOverLife.h"
#include "Particles/Lifetime/ParticleModuleLifetime.h"
#include "Particles/Location/ParticleModuleLocationPrimitiveCylinder.h"
#include "Particles/ParticleLODLevel.h"
#include "Particles/ParticleModuleRequired.h"
#include "Particles/ParticleSpriteEmitter.h"
#include "Particles/ParticleSystem.h"
#include "Particles/ParticleSystemComponent.h"
#include "Particles/Rotation/ParticleModuleRotation.h"
#include "Particles/Size/ParticleModuleSize.h"
#include "Particles/Spawn/ParticleModuleSpawn.h"
#include "Particles/SubUV/ParticleModuleSubUV.h"
#include "Particles/TypeData/ParticleModuleTypeDataMesh.h"
#include "Particles/Velocity/ParticleModuleVelocity.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// What the effect export carries of a Cascade particle system: the distributions it reads, the sprites keys
// a system built here translates to, what it reports as approximated or dropped, and the downgrade for a client that
// does not read sprites.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleEffectExportTestPrivate
{
	/** A bubble-like emitter at LOD 0: every module the translation maps. */
	UParticleSystem* MakeBubbleSystem(UObject* Outer, bool bWithMeshTypeData = false)
	{
		UParticleSystem* System = NewObject<UParticleSystem>(Outer);
		UParticleSpriteEmitter* Emitter = NewObject<UParticleSpriteEmitter>(System);
		UParticleLODLevel* Lod = NewObject<UParticleLODLevel>(Emitter);
		Lod->Level = 0;
		Lod->bEnabled = true;

		UMaterial* Material = NewObject<UMaterial>(Outer);
		Material->BlendMode = BLEND_Additive;
		UParticleModuleRequired* Required = NewObject<UParticleModuleRequired>(System);
		Required->Material = Material;
		Required->EmitterLoops = 0;
		Required->EmitterDuration = 1.0f;
		Lod->RequiredModule = Required;

		UParticleModuleSpawn* Spawn = NewObject<UParticleModuleSpawn>(System);
		UDistributionFloatConstant* Rate = NewObject<UDistributionFloatConstant>(Spawn);
		Rate->Constant = 20.0f;
		Spawn->Rate.Distribution = Rate;
		Lod->SpawnModule = Spawn;

		UParticleModuleLifetime* Lifetime = NewObject<UParticleModuleLifetime>(System);
		UDistributionFloatUniform* Life = NewObject<UDistributionFloatUniform>(Lifetime);
		Life->Min = 2.0f;
		Life->Max = 4.0f;
		Lifetime->Lifetime.Distribution = Life;
		Lod->Modules.Add(Lifetime);

		UParticleModuleSize* Size = NewObject<UParticleModuleSize>(System);
		UDistributionVectorUniform* StartSize = NewObject<UDistributionVectorUniform>(Size);
		StartSize->Min = FVector(4.0, 4.0, 4.0);
		StartSize->Max = FVector(8.0, 8.0, 8.0);
		Size->StartSize.Distribution = StartSize;
		Lod->Modules.Add(Size);

		UParticleModuleVelocity* Velocity = NewObject<UParticleModuleVelocity>(System);
		UDistributionVectorUniform* Start = NewObject<UDistributionVectorUniform>(Velocity);
		Start->Min = FVector(-5.0, -5.0, 15.0);
		Start->Max = FVector(5.0, 5.0, 40.0);
		Velocity->StartVelocity.Distribution = Start;
		Lod->Modules.Add(Velocity);

		UParticleModuleAccelerationConstant* Acceleration = NewObject<UParticleModuleAccelerationConstant>(System);
		Acceleration->Acceleration = FVector(0.0, 0.0, 10.0);
		Lod->Modules.Add(Acceleration);

		UParticleModuleLocationPrimitiveCylinder* Cylinder = NewObject<UParticleModuleLocationPrimitiveCylinder>(System);
		UDistributionFloatConstant* Radius = NewObject<UDistributionFloatConstant>(Cylinder);
		Radius->Constant = 40.0f;
		Cylinder->StartRadius.Distribution = Radius;
		UDistributionFloatConstant* Height = NewObject<UDistributionFloatConstant>(Cylinder);
		Height->Constant = 10.0f;
		Cylinder->StartHeight.Distribution = Height;
		Cylinder->HeightAxis = PMLPC_HEIGHTAXIS_Z;
		Lod->Modules.Add(Cylinder);

		// Colour from light blue to blue, and an alpha that fades in to half life and out again.
		UParticleModuleColorOverLife* ColorOverLife = NewObject<UParticleModuleColorOverLife>(System);
		UDistributionVectorConstantCurve* Colors = NewObject<UDistributionVectorConstantCurve>(ColorOverLife);
		Colors->ConstantCurve.AddPoint(0.0f, FVector(0.6, 0.9, 1.0));
		Colors->ConstantCurve.AddPoint(1.0f, FVector(0.3, 0.6, 1.0));
		ColorOverLife->ColorOverLife.Distribution = Colors;
		UDistributionFloatConstantCurve* Alphas = NewObject<UDistributionFloatConstantCurve>(ColorOverLife);
		Alphas->ConstantCurve.AddPoint(0.0f, 0.0f);
		Alphas->ConstantCurve.AddPoint(0.5f, 0.8f);
		Alphas->ConstantCurve.AddPoint(1.0f, 0.0f);
		ColorOverLife->AlphaOverLife.Distribution = Alphas;
		Lod->Modules.Add(ColorOverLife);

		// A module the sprites keys have no place for.
		Lod->Modules.Add(NewObject<UParticleModuleRotation>(System));

		if (bWithMeshTypeData)
		{
			Lod->TypeDataModule = NewObject<UParticleModuleTypeDataMesh>(System);
		}
		Emitter->LODLevels.Add(Lod);
		System->Emitters.Add(Emitter);
		return System;
	}

	UParticleSystemComponent* MakeComponent(UObject* Outer, UParticleSystem* System)
	{
		UParticleSystemComponent* Component = NewObject<UParticleSystemComponent>(Outer);
		Component->Template = System;
		return Component;
	}

	double Number(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key)
	{
		double Value = -1.0e9;
		Object->TryGetNumberField(Key, Value);
		return Value;
	}

	/**
	 * Gives the bubble system's emitter a flipbook sheet of Columns by Rows with the interpolation method Method, and (when
	 * bWithModule) a SubUV module whose index runs From to To over a particle's life. The module's outer is the system, as
	 * the engine's own are (not the LOD level, which ensures).
	 */
	void AddSheet(UParticleSystem* System, const int32 Columns, const int32 Rows, const EParticleSubUVInterpMethod Method, const bool bWithModule,
		const float From, const float To)
	{
		UParticleLODLevel* Lod = System->Emitters[0]->GetLODLevel(0);
		Lod->RequiredModule->SubImages_Horizontal = Columns;
		Lod->RequiredModule->SubImages_Vertical = Rows;
		Lod->RequiredModule->InterpolationMethod = Method;
		if (bWithModule)
		{
			UParticleModuleSubUV* SubUV = NewObject<UParticleModuleSubUV>(System);
			UDistributionFloatConstantCurve* Index = NewObject<UDistributionFloatConstantCurve>(SubUV);
			Index->ConstantCurve.AddPoint(0.0f, From);
			Index->ConstantCurve.AddPoint(1.0f, To);
			SubUV->SubImageIndex.Distribution = Index;
			Lod->Modules.Add(SubUV);
		}
	}

	/** Translates a bubble system with the sheet AddSheet makes; the effect's properties and the approximated line's text. */
	bool TranslateSheet(const int32 Columns, const int32 Rows, const EParticleSubUVInterpMethod Method, const bool bWithModule, const float From,
		const float To, TSharedPtr<FJsonObject>& OutJson, FString& OutDetail)
	{
		UPackage* Package = GetTransientPackage();
		UParticleSystem* System = MakeBubbleSystem(Package);
		AddSheet(System, Columns, Rows, Method, bWithModule, From, To);
		FMantleCoverage Coverage;
		TArray<TSharedPtr<FJsonObject>> Out;
		if (!FMantleEffectExport::Translate(MakeComponent(Package, System), TEXT("Sheet"), Coverage, Out) || Out.Num() != 1)
		{
			return false;
		}
		OutJson = Out[0];
		OutDetail.Reset();
		const FMantleCoverageCategory* Category = Coverage.Find(MantleCoverageCategory::Particles);
		if (Category && Category->Get(EMantleCoverageOutcome::Approximated).Examples.Num() == 1)
		{
			OutDetail = Category->Get(EMantleCoverageOutcome::Approximated).Examples[0].Detail;
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleEffectExportDistributionsTest,
	"Mantle.Editor.EffectExport.Distributions",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleEffectExportDistributionsTest::RunTest(const FString& Parameters)
{
	UPackage* Package = GetTransientPackage();
	double Min = 0.0, Max = 0.0;
	bool bApproximated = true;

	UDistributionFloatConstant* Constant = NewObject<UDistributionFloatConstant>(Package);
	Constant->Constant = 7.0f;
	TestTrue(TEXT("a constant reads"), FMantleEffectExport::ReadFloat(Constant, Min, Max, bApproximated));
	TestTrue(TEXT("as itself twice"), Min == 7.0 && Max == 7.0);
	TestFalse(TEXT("exactly"), bApproximated);

	UDistributionFloatUniform* Uniform = NewObject<UDistributionFloatUniform>(Package);
	Uniform->Min = 9.0f;
	Uniform->Max = 3.0f;
	TestTrue(TEXT("a range reads"), FMantleEffectExport::ReadFloat(Uniform, Min, Max, bApproximated));
	TestTrue(TEXT("lowest first even when written backwards"), Min == 3.0 && Max == 9.0);
	TestFalse(TEXT("exactly too"), bApproximated);

	UDistributionFloatConstantCurve* Curve = NewObject<UDistributionFloatConstantCurve>(Package);
	Curve->ConstantCurve.AddPoint(0.0f, 2.0f);
	Curve->ConstantCurve.AddPoint(1.0f, 10.0f);
	TestTrue(TEXT("a curve reads"), FMantleEffectExport::ReadFloat(Curve, Min, Max, bApproximated));
	TestTrue(TEXT("by its start and end"), Min == 2.0 && Max == 10.0);
	TestTrue(TEXT("approximated"), bApproximated);

	TestFalse(TEXT("nothing reads as nothing"), FMantleEffectExport::ReadFloat(nullptr, Min, Max, bApproximated));

	FVector VectorMin, VectorMax;
	UDistributionVectorConstant* VectorConstant = NewObject<UDistributionVectorConstant>(Package);
	VectorConstant->Constant = FVector(1.0, 2.0, 3.0);
	TestTrue(TEXT("a vector constant reads"), FMantleEffectExport::ReadVector(VectorConstant, VectorMin, VectorMax, bApproximated));
	TestTrue(TEXT("as itself"), VectorMin == FVector(1.0, 2.0, 3.0) && VectorMax == VectorMin && !bApproximated);

	UDistributionVectorUniform* VectorUniform = NewObject<UDistributionVectorUniform>(Package);
	VectorUniform->Min = FVector(5.0, -5.0, 0.0);
	VectorUniform->Max = FVector(1.0, 5.0, 2.0);
	TestTrue(TEXT("a vector range reads"), FMantleEffectExport::ReadVector(VectorUniform, VectorMin, VectorMax, bApproximated));
	TestTrue(TEXT("lowest and highest per axis"), VectorMin == FVector(1.0, -5.0, 0.0) && VectorMax == FVector(5.0, 5.0, 2.0));

	UDistributionVectorConstantCurve* VectorCurve = NewObject<UDistributionVectorConstantCurve>(Package);
	VectorCurve->ConstantCurve.AddPoint(0.0f, FVector(0.0, 0.0, 0.0));
	VectorCurve->ConstantCurve.AddPoint(1.0f, FVector(4.0, 4.0, 4.0));
	TestTrue(TEXT("a vector curve reads"), FMantleEffectExport::ReadVector(VectorCurve, VectorMin, VectorMax, bApproximated));
	TestTrue(TEXT("by its start and end, approximated"), VectorMin == FVector::ZeroVector && VectorMax == FVector(4.0, 4.0, 4.0) && bApproximated);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleEffectExportTranslateTest,
	"Mantle.Editor.EffectExport.Translate",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleEffectExportTranslateTest::RunTest(const FString& Parameters)
{
	using namespace MantleEffectExportTestPrivate;

	UPackage* Package = GetTransientPackage();
	UParticleSystemComponent* Component = MakeComponent(Package, MakeBubbleSystem(Package));
	FMantleCoverage Coverage;
	TArray<TSharedPtr<FJsonObject>> Out;
	if (!TestTrue(TEXT("a Cascade system with a sprite emitter is carried"), FMantleEffectExport::Translate(Component, TEXT("Capsule/Bubbles"), Coverage, Out)))
	{
		return false;
	}
	if (!TestEqual(TEXT("one effect for its one emitter"), Out.Num(), 1))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>& Json = Out[0];
	FString Text;
	TestTrue(TEXT("a sprites effect"), Json->TryGetStringField(TEXT("archetype"), Text) && Text == TEXT("sprites"));
	TestTrue(TEXT("additive from the material"), Json->TryGetStringField(TEXT("blend"), Text) && Text == TEXT("additive"));
	TestEqual(TEXT("rate"), Number(Json, TEXT("rate")), 20.0);
	TestEqual(TEXT("size min"), Number(Json, TEXT("sizeMin")), 4.0);
	TestEqual(TEXT("size max"), Number(Json, TEXT("sizeMax")), 8.0);
	TestEqual(TEXT("lifetime min"), Number(Json, TEXT("lifetimeMin")), 2.0);
	TestEqual(TEXT("lifetime max"), Number(Json, TEXT("lifetimeMax")), 4.0);
	TestEqual(TEXT("alpha at its peak"), Number(Json, TEXT("alpha")), 0.8, 1.0e-6);
	TestEqual(TEXT("fading out to nothing"), Number(Json, TEXT("alphaEnd")), 0.0, 1.0e-6);
	TestTrue(TEXT("fading in until about half life"), FMath::IsNearlyEqual(Number(Json, TEXT("fadeIn")), 0.5, 0.07));
	TestFalse(TEXT("no texture: the material names none"), Json->HasField(TEXT("texture")));

	const TSharedPtr<FJsonObject>* Shape = nullptr;
	if (TestTrue(TEXT("it has a shape"), Json->TryGetObjectField(TEXT("shape"), Shape) && Shape))
	{
		TestTrue(TEXT("a cylinder"), (*Shape)->TryGetStringField(TEXT("type"), Text) && Text == TEXT("cylinder"));
		TestEqual(TEXT("its radius"), Number(*Shape, TEXT("radius")), 40.0);
		TestEqual(TEXT("its height"), Number(*Shape, TEXT("height")), 10.0);
	}
	const TArray<TSharedPtr<FJsonValue>>* VelocityMax = nullptr;
	if (TestTrue(TEXT("it has a velocity range"), Json->TryGetArrayField(TEXT("velocityMax"), VelocityMax) && VelocityMax && VelocityMax->Num() == 3))
	{
		TestEqual(TEXT("velocity max z"), (*VelocityMax)[2]->AsNumber(), 40.0);
	}
	const TArray<TSharedPtr<FJsonValue>>* Acceleration = nullptr;
	if (TestTrue(TEXT("it has an acceleration"), Json->TryGetArrayField(TEXT("acceleration"), Acceleration) && Acceleration && Acceleration->Num() == 3))
	{
		TestEqual(TEXT("acceleration z"), (*Acceleration)[2]->AsNumber(), 10.0);
	}

	// What is not carried is on the report: the rotation module, and the alpha curve only by its start, peak and end.
	TestEqual(TEXT("one Particles line, approximated"), Coverage.Count(MantleCoverageCategory::Particles, EMantleCoverageOutcome::Approximated), static_cast<int64>(1));
	const FMantleCoverageCategory* Category = Coverage.Find(MantleCoverageCategory::Particles);
	if (TestNotNull(TEXT("the category"), Category) && Category->Get(EMantleCoverageOutcome::Approximated).Examples.Num() == 1)
	{
		const FString& Detail = Category->Get(EMantleCoverageOutcome::Approximated).Examples[0].Detail;
		TestTrue(TEXT("naming the module it ignored"), Detail.Contains(TEXT("Rotation")));
	}

	// The JSON reads back into the effect the exporter writes.
	FMantleEffectProps Props;
	if (TestTrue(TEXT("the properties read back"), FMantleEffectExport::ReadProperties(*Json, Props)))
	{
		TestTrue(TEXT("sprites"), Props.Archetype == EMantleEffectArchetype::Sprites);
		TestTrue(TEXT("additive"), Props.Sprites.Blend == EMantleSpritesBlend::Additive);
		TestEqual(TEXT("size min"), Props.Sprites.SizeMin, 4.0);
		TestTrue(TEXT("a cylinder"), Props.Sprites.Shape == EMantleSpritesShape::Cylinder);
		TestEqual(TEXT("max particles"), Props.Sprites.MaxParticles, FMantleSpritesSettings().MaxParticles);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleEffectExportSubImagesTest,
	"Mantle.Editor.EffectExport.SubImages",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleEffectExportSubImagesTest::RunTest(const FString& Parameters)
{
	using namespace MantleEffectExportTestPrivate;

	TSharedPtr<FJsonObject> Json;
	FString Detail;
	FString Text;

	// A Random method on a 4 by 4 sheet: one random frame per particle, and the SubUV module no longer ignored.
	if (TestTrue(TEXT("a random 4 by 4 sheet translates"), TranslateSheet(4, 4, PSUVIM_Random, true, 0.0f, 0.0f, Json, Detail)))
	{
		const TArray<TSharedPtr<FJsonValue>>* Sheet = nullptr;
		if (TestTrue(TEXT("with a sheet"), Json->TryGetArrayField(TEXT("subImages"), Sheet) && Sheet && Sheet->Num() == 2))
		{
			TestEqual(TEXT("four columns"), (*Sheet)[0]->AsNumber(), 4.0);
			TestEqual(TEXT("four rows"), (*Sheet)[1]->AsNumber(), 4.0);
		}
		TestTrue(TEXT("random"), Json->TryGetStringField(TEXT("subImageMode"), Text) && Text == TEXT("random"));
		TestTrue(TEXT("still approximated (the rotation module)"), Detail.Contains(TEXT("Rotation")));
		TestFalse(TEXT("but SubUV is carried, not ignored"), Detail.Contains(TEXT("SubUV")));

		FMantleEffectProps Props;
		if (TestTrue(TEXT("the properties read back"), FMantleEffectExport::ReadProperties(*Json, Props)))
		{
			TestTrue(TEXT("with the sheet"), Props.Sprites.SubImageColumns == 4 && Props.Sprites.SubImageRows == 4);
			TestTrue(TEXT("and its mode"), Props.Sprites.SubImageMode == EMantleSpritesSubImageMode::Random);
		}
	}

	// A Linear method whose index runs over the whole sheet: the frames play over a particle's life.
	if (TestTrue(TEXT("a linear 4 by 2 sheet translates"), TranslateSheet(4, 2, PSUVIM_Linear, true, 0.0f, 8.0f, Json, Detail)))
	{
		TestTrue(TEXT("over life"), Json->TryGetStringField(TEXT("subImageMode"), Text) && Text == TEXT("overLife"));
		TestFalse(TEXT("with nothing said about the range"), Detail.Contains(TEXT("flipbook")));
		FMantleEffectProps Props;
		TestTrue(TEXT("and it reads back"), FMantleEffectExport::ReadProperties(*Json, Props) && Props.Sprites.SubImageColumns == 4
			&& Props.Sprites.SubImageRows == 2 && Props.Sprites.SubImageMode == EMantleSpritesSubImageMode::OverLife);
	}

	// A Linear method held on one frame is no animation: a random frame, said so. Blending is dropped, said so.
	if (TestTrue(TEXT("a fixed frame translates"), TranslateSheet(4, 4, PSUVIM_Linear, true, 3.0f, 3.0f, Json, Detail)))
	{
		TestTrue(TEXT("as a random frame"), Json->TryGetStringField(TEXT("subImageMode"), Text) && Text == TEXT("random"));
		TestTrue(TEXT("and says so"), Detail.Contains(TEXT("a fixed flipbook frame becomes a random one")));
	}
	if (TestTrue(TEXT("a blended random sheet translates"), TranslateSheet(2, 2, PSUVIM_Random_Blend, true, 0.0f, 0.0f, Json, Detail)))
	{
		TestTrue(TEXT("random"), Json->TryGetStringField(TEXT("subImageMode"), Text) && Text == TEXT("random"));
		TestTrue(TEXT("blending is said to be dropped"), Detail.Contains(TEXT("blending between flipbook frames dropped")));
	}

	// Not carried: method None (Cascade then draws the whole texture), a 1 by 1 sheet, one past 16, and a sheet with no SubUV module.
	if (TestTrue(TEXT("method None translates"), TranslateSheet(4, 4, PSUVIM_None, true, 0.0f, 0.0f, Json, Detail)))
	{
		TestFalse(TEXT("with no sheet"), Json->HasField(TEXT("subImages")) || Json->HasField(TEXT("subImageMode")));
		TestTrue(TEXT("and the SubUV module ignored"), Detail.Contains(TEXT("SubUV")));
	}
	if (TestTrue(TEXT("a 1 by 1 sheet translates"), TranslateSheet(1, 1, PSUVIM_Random, true, 0.0f, 0.0f, Json, Detail)))
	{
		TestFalse(TEXT("with no sheet"), Json->HasField(TEXT("subImages")));
		TestTrue(TEXT("and the SubUV module ignored"), Detail.Contains(TEXT("SubUV")));
	}
	if (TestTrue(TEXT("a 32 by 1 sheet translates"), TranslateSheet(32, 1, PSUVIM_Random, true, 0.0f, 0.0f, Json, Detail)))
	{
		TestFalse(TEXT("with no sheet"), Json->HasField(TEXT("subImages")));
		TestTrue(TEXT("and says it is past the limit"), Detail.Contains(TEXT("past Mantle's")));
	}
	if (TestTrue(TEXT("a sheet with no SubUV module translates"), TranslateSheet(4, 4, PSUVIM_Random, false, 0.0f, 0.0f, Json, Detail)))
	{
		TestFalse(TEXT("with no sheet: the module is what shows frames"), Json->HasField(TEXT("subImages")));
		TestFalse(TEXT("and nothing said of SubUV"), Detail.Contains(TEXT("SubUV")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleEffectExportDropsTest,
	"Mantle.Editor.EffectExport.Drops",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleEffectExportDropsTest::RunTest(const FString& Parameters)
{
	using namespace MantleEffectExportTestPrivate;

	UPackage* Package = GetTransientPackage();
	TArray<TSharedPtr<FJsonObject>> Out;

	// A mesh emitter is dropped with a line, not an error.
	{
		FMantleCoverage Coverage;
		UParticleSystemComponent* Component = MakeComponent(Package, MakeBubbleSystem(Package, /*bWithMeshTypeData*/ true));
		TestFalse(TEXT("a mesh emitter carries nothing"), FMantleEffectExport::Translate(Component, TEXT("Rocks"), Coverage, Out));
		TestEqual(TEXT("no effects"), Out.Num(), 0);
		TestEqual(TEXT("one Particles line, dropped"), Coverage.Count(MantleCoverageCategory::Particles, EMantleCoverageOutcome::Dropped), static_cast<int64>(1));
	}

	// A system with no emitters, and no system at all, carry nothing and say nothing.
	{
		FMantleCoverage Coverage;
		TestFalse(TEXT("an empty system"), FMantleEffectExport::Translate(MakeComponent(Package, NewObject<UParticleSystem>(Package)), TEXT("Empty"), Coverage, Out));
		TestFalse(TEXT("no system"), FMantleEffectExport::Translate(MakeComponent(Package, nullptr), TEXT("None"), Coverage, Out));
		TestFalse(TEXT("no component"), FMantleEffectExport::Translate(nullptr, TEXT("Nothing"), Coverage, Out));
		TestTrue(TEXT("none of them is reported"), Coverage.IsEmpty());
	}

	// A client that does not read sprites gets the effect left out, saying why.
	{
		const FMantleTargetFeaturesScope Scope(FMantleTargetFeatures::None());
		FMantleCoverage Coverage;
		UParticleSystemComponent* Component = MakeComponent(Package, MakeBubbleSystem(Package));
		TestFalse(TEXT("an old client carries nothing"), FMantleEffectExport::Translate(Component, TEXT("Capsule"), Coverage, Out));
		const FMantleCoverageCategory* Category = Coverage.Find(MantleCoverageCategory::Particles);
		if (TestNotNull(TEXT("and says so"), Category) && Category->Get(EMantleCoverageOutcome::Dropped).Examples.Num() == 1)
		{
			TestTrue(TEXT("needing a newer client"), Category->Get(EMantleCoverageOutcome::Dropped).Examples[0].Detail.Contains(FMantleTargetFeatures::NeedsNewerClient));
		}
	}
	TestTrue(TEXT("the default target reads sprites"), FMantleTargetFeatures::Current().Reads(MantleClientFeatures::EffectSprites));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
