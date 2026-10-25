// Copyright Joshua Eger. All Rights Reserved.
#include "MantleEffect.h"
#include "MantleEffectTypes.h"
#include "MantlePlaceLimits.h"
#include "MantlePlaceParser.h"

#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include <limits>
#include "Misc/AutomationTest.h"
#include "Particles/ParticleSystemComponent.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// The sprites archetype of an Effect: what the parser reads and refuses, how a lone min or max resolves, the limits
// the settings are held to, and what a sprites system is handed. No world; the sprites materials and system are not needed.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleEffectSpritesTestPrivate
{
	const TCHAR* const TextureId = TEXT("ast_ua1b2c3d4e5f60718293a4b5c6d7e8f90");

	/** A place with one Effect whose properties are Properties (the text between the braces). */
	FMantleParseResult ParseEffect(const FString& Properties)
	{
		const FString Json = FString::Printf(
			TEXT("{\"schemaVersion\":1,\"placeId\":\"plc_sprites01\",\"revision\":{\"number\":1,\"state\":\"draft\"},"
				 "\"metadata\":{\"name\":\"Sprites\",\"authorId\":\"usr_4b1d\"},\"settings\":{},\"assets\":[],"
				 "\"entities\":[{\"id\":\"ent_fx\",\"type\":\"Effect\",\"parent\":null,"
				 "\"transform\":{\"location\":[0,0,0],\"rotation\":[0,0,0],\"scale\":[1,1,1]},\"properties\":{%s}}]}"),
			*Properties);
		return FMantlePlaceParser::ParseFromString(Json);
	}

	const FMantleEffectProps* EffectOf(const FMantleParseResult& Result)
	{
		return Result.Place.Entities.Num() == 1 ? Result.Place.Entities[0].Props.TryGet<FMantleEffectProps>() : nullptr;
	}

	bool HasErrorAt(const FMantleParseResult& Result, const FString& Path)
	{
		return Result.Errors.ContainsByPredicate([&Path](const FMantleParseError& Error) { return Error.Path == Path; });
	}

	float InstanceScalar(const UParticleSystemComponent& Component, const TCHAR* Name, bool& bOutFound)
	{
		for (const FParticleSysParam& Param : Component.InstanceParameters)
		{
			if (Param.Name == FName(Name))
			{
				bOutFound = true;
				return Param.Scalar;
			}
		}
		bOutFound = false;
		return -1.0f;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleEffectSpritesParseDefaultsTest,
	"Mantle.Runtime.Effect.Sprites.ParseDefaults",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleEffectSpritesParseDefaultsTest::RunTest(const FString& Parameters)
{
	using namespace MantleEffectSpritesTestPrivate;

	const FMantleParseResult Result = ParseEffect(TEXT("\"archetype\":\"sprites\""));
	if (!TestTrue(TEXT("the archetype alone parses"), Result.bSuccess))
	{
		AddError(Result.ErrorSummary());
		return false;
	}
	const FMantleEffectProps* Props = EffectOf(Result);
	if (!TestNotNull(TEXT("an effect"), Props))
	{
		return false;
	}
	const FMantleSpritesSettings Own;
	TestTrue(TEXT("it is a sprites effect"), Props->Archetype == EMantleEffectArchetype::Sprites);
	TestEqual(TEXT("with no texture"), Props->TextureId, FString());
	TestTrue(TEXT("translucent"), Props->Sprites.Blend == EMantleSpritesBlend::Translucent);
	TestTrue(TEXT("white"), Props->Look.Color.Equals(FLinearColor::White));
	TestTrue(TEXT("whose end colour is its colour"), Props->Sprites.ColorEnd.Equals(Props->Look.Color));
	TestEqual(TEXT("alpha 1"), Props->Sprites.Alpha, 1.0);
	TestEqual(TEXT("alpha end 0"), Props->Sprites.AlphaEnd, 0.0);
	TestEqual(TEXT("size"), Props->Sprites.SizeMin, Own.SizeMin);
	TestEqual(TEXT("size max"), Props->Sprites.SizeMax, Own.SizeMax);
	TestEqual(TEXT("lifetime"), Props->Sprites.LifetimeMin, Own.LifetimeMin);
	TestTrue(TEXT("no velocity"), Props->Sprites.VelocityMax.IsZero());
	TestTrue(TEXT("a point"), Props->Sprites.Shape == EMantleSpritesShape::Point);
	TestEqual(TEXT("500 particles"), Props->Sprites.MaxParticles, 500);
	TestTrue(TEXT("the whole texture, not a sheet"), Props->Sprites.SubImageColumns == 1 && Props->Sprites.SubImageRows == 1);
	TestTrue(TEXT("a random frame"), Props->Sprites.SubImageMode == EMantleSpritesSubImageMode::Random);
	TestEqual(TEXT("ten a second"), Props->Look.Rate, 10.0);
	TestTrue(TEXT("autoplays"), Props->bAutoplay);
	TestTrue(TEXT("loops"), Props->bLoop);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleEffectSpritesParseFullTest,
	"Mantle.Runtime.Effect.Sprites.ParseFull",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleEffectSpritesParseFullTest::RunTest(const FString& Parameters)
{
	using namespace MantleEffectSpritesTestPrivate;

	const FString Properties = FString::Printf(TEXT(
		"\"archetype\":\"sprites\",\"texture\":\"%s\",\"blend\":\"additive\",\"color\":[0.6,0.9,1],\"colorEnd\":[0.3,0.6,1],"
		"\"alpha\":0.8,\"alphaEnd\":0.1,\"fadeIn\":0.15,\"sizeMin\":3,\"sizeMax\":8,\"sizeEndScale\":1.5,\"rate\":24,"
		"\"lifetimeMin\":2,\"lifetimeMax\":4,\"velocityMin\":[-5,-6,15],\"velocityMax\":[5,6,40],\"acceleration\":[0,0,10],"
		"\"shape\":{\"type\":\"cylinder\",\"radius\":40,\"height\":10},\"maxParticles\":200,\"subImages\":[4,2],\"subImageMode\":\"overLife\","
		"\"autoplay\":false,\"loop\":false"), TextureId);
	const FMantleParseResult Result = ParseEffect(Properties);
	if (!TestTrue(TEXT("every key parses"), Result.bSuccess))
	{
		AddError(Result.ErrorSummary());
		return false;
	}
	const FMantleEffectProps* Props = EffectOf(Result);
	if (!TestNotNull(TEXT("an effect"), Props))
	{
		return false;
	}
	const FMantleSpritesSettings& S = Props->Sprites;
	TestEqual(TEXT("texture"), Props->TextureId, FString(TextureId));
	TestTrue(TEXT("additive"), S.Blend == EMantleSpritesBlend::Additive);
	TestTrue(TEXT("colour"), Props->Look.Color.Equals(FLinearColor(0.6f, 0.9f, 1.0f)));
	TestTrue(TEXT("end colour"), S.ColorEnd.Equals(FLinearColor(0.3f, 0.6f, 1.0f)));
	TestEqual(TEXT("alpha"), S.Alpha, 0.8);
	TestEqual(TEXT("alpha end"), S.AlphaEnd, 0.1);
	TestEqual(TEXT("fade in"), S.FadeIn, 0.15);
	TestEqual(TEXT("size min"), S.SizeMin, 3.0);
	TestEqual(TEXT("size max"), S.SizeMax, 8.0);
	TestEqual(TEXT("end scale"), S.SizeEndScale, 1.5);
	TestEqual(TEXT("rate"), Props->Look.Rate, 24.0);
	TestEqual(TEXT("lifetime min"), S.LifetimeMin, 2.0);
	TestEqual(TEXT("lifetime max"), S.LifetimeMax, 4.0);
	TestEqual(TEXT("velocity min"), S.VelocityMin, FVector(-5.0, -6.0, 15.0));
	TestEqual(TEXT("velocity max"), S.VelocityMax, FVector(5.0, 6.0, 40.0));
	TestEqual(TEXT("acceleration"), S.Acceleration, FVector(0.0, 0.0, 10.0));
	TestTrue(TEXT("a cylinder"), S.Shape == EMantleSpritesShape::Cylinder);
	TestEqual(TEXT("its radius"), S.CylinderRadius, 40.0);
	TestEqual(TEXT("its height"), S.CylinderHeight, 10.0);
	TestEqual(TEXT("max particles"), S.MaxParticles, 200);
	TestEqual(TEXT("sheet columns"), S.SubImageColumns, 4);
	TestEqual(TEXT("sheet rows"), S.SubImageRows, 2);
	TestTrue(TEXT("frames over life"), S.SubImageMode == EMantleSpritesSubImageMode::OverLife);
	TestFalse(TEXT("autoplay off"), Props->bAutoplay);
	TestFalse(TEXT("loop off"), Props->bLoop);

	// A box and a sphere carry their own extent; a lone min or max gives its value to the other.
	const FMantleParseResult Box = ParseEffect(TEXT("\"archetype\":\"sprites\",\"sizeMin\":5,\"lifetimeMax\":3,\"velocityMin\":[1,2,3],\"shape\":{\"type\":\"box\",\"halfExtent\":[50,40,5]}"));
	const FMantleEffectProps* BoxProps = EffectOf(Box);
	if (TestTrue(TEXT("a box parses"), Box.bSuccess && BoxProps != nullptr))
	{
		TestTrue(TEXT("box"), BoxProps->Sprites.Shape == EMantleSpritesShape::Box);
		TestEqual(TEXT("half extent"), BoxProps->Sprites.BoxHalfExtent, FVector(50.0, 40.0, 5.0));
		TestEqual(TEXT("a lone sizeMin is the max too"), BoxProps->Sprites.SizeMax, 5.0);
		TestEqual(TEXT("a lone lifetimeMax is the min too"), BoxProps->Sprites.LifetimeMin, 3.0);
		TestEqual(TEXT("a lone velocityMin is the max too"), BoxProps->Sprites.VelocityMax, FVector(1.0, 2.0, 3.0));
	}
	const FMantleParseResult Sphere = ParseEffect(TEXT("\"archetype\":\"sprites\",\"shape\":{\"type\":\"sphere\",\"radius\":30}"));
	const FMantleEffectProps* SphereProps = EffectOf(Sphere);
	if (TestTrue(TEXT("a sphere parses"), Sphere.bSuccess && SphereProps != nullptr))
	{
		TestTrue(TEXT("sphere"), SphereProps->Sprites.Shape == EMantleSpritesShape::Sphere);
		TestEqual(TEXT("its radius"), SphereProps->Sprites.SphereRadius, 30.0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleEffectSpritesParseRefusalsTest,
	"Mantle.Runtime.Effect.Sprites.ParseRefusals",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleEffectSpritesParseRefusalsTest::RunTest(const FString& Parameters)
{
	using namespace MantleEffectSpritesTestPrivate;

	struct FCase
	{
		const TCHAR* Properties;
		const TCHAR* Path;
	};
	const FCase Cases[] = {
		{ TEXT("\"archetype\":\"smoke\",\"sizeMin\":4"), TEXT("/entities/0/properties/sizeMin") },
		{ TEXT("\"archetype\":\"fire\",\"texture\":\"ast_ua1b2c3d4e5f60718293a4b5c6d7e8f90\""), TEXT("/entities/0/properties/texture") },
		{ TEXT("\"archetype\":\"sprites\",\"size\":40"), TEXT("/entities/0/properties/size") },
		{ TEXT("\"archetype\":\"sprites\",\"speed\":40"), TEXT("/entities/0/properties/speed") },
		{ TEXT("\"archetype\":\"sprites\",\"lifetime\":2"), TEXT("/entities/0/properties/lifetime") },
		{ TEXT("\"archetype\":\"sprites\",\"texture\":\"\""), TEXT("/entities/0/properties/texture") },
		{ TEXT("\"archetype\":\"sprites\",\"blend\":\"multiply\""), TEXT("/entities/0/properties/blend") },
		{ TEXT("\"archetype\":\"sprites\",\"colorEnd\":[0,2,0]"), TEXT("/entities/0/properties/colorEnd/1") },
		{ TEXT("\"archetype\":\"sprites\",\"alpha\":1.5"), TEXT("/entities/0/properties/alpha") },
		{ TEXT("\"archetype\":\"sprites\",\"sizeMin\":0.05"), TEXT("/entities/0/properties/sizeMin") },
		{ TEXT("\"archetype\":\"sprites\",\"sizeMax\":1001"), TEXT("/entities/0/properties/sizeMax") },
		{ TEXT("\"archetype\":\"sprites\",\"sizeEndScale\":11"), TEXT("/entities/0/properties/sizeEndScale") },
		{ TEXT("\"archetype\":\"sprites\",\"sizeMin\":10,\"sizeMax\":5"), TEXT("/entities/0/properties/sizeMax") },
		{ TEXT("\"archetype\":\"sprites\",\"lifetimeMin\":4,\"lifetimeMax\":2"), TEXT("/entities/0/properties/lifetimeMax") },
		{ TEXT("\"archetype\":\"sprites\",\"velocityMin\":[0,0,50],\"velocityMax\":[10,10,10]"), TEXT("/entities/0/properties/velocityMax") },
		{ TEXT("\"archetype\":\"sprites\",\"velocityMax\":[0,5001,0]"), TEXT("/entities/0/properties/velocityMax/1") },
		{ TEXT("\"archetype\":\"sprites\",\"acceleration\":[0,0]"), TEXT("/entities/0/properties/acceleration") },
		{ TEXT("\"archetype\":\"sprites\",\"maxParticles\":0"), TEXT("/entities/0/properties/maxParticles") },
		{ TEXT("\"archetype\":\"sprites\",\"maxParticles\":2001"), TEXT("/entities/0/properties/maxParticles") },
		{ TEXT("\"archetype\":\"sprites\",\"maxParticles\":10.5"), TEXT("/entities/0/properties/maxParticles") },
		{ TEXT("\"archetype\":\"smoke\",\"subImages\":[4,4]"), TEXT("/entities/0/properties/subImages") },
		{ TEXT("\"archetype\":\"glow\",\"subImageMode\":\"random\""), TEXT("/entities/0/properties/subImageMode") },
		{ TEXT("\"archetype\":\"sprites\",\"subImages\":[0,4]"), TEXT("/entities/0/properties/subImages/0") },
		{ TEXT("\"archetype\":\"sprites\",\"subImages\":[4,17]"), TEXT("/entities/0/properties/subImages/1") },
		{ TEXT("\"archetype\":\"sprites\",\"subImages\":[4,2.5]"), TEXT("/entities/0/properties/subImages/1") },
		{ TEXT("\"archetype\":\"sprites\",\"subImages\":[4,4,4]"), TEXT("/entities/0/properties/subImages") },
		{ TEXT("\"archetype\":\"sprites\",\"subImages\":4"), TEXT("/entities/0/properties/subImages") },
		{ TEXT("\"archetype\":\"sprites\",\"subImageMode\":\"loop\""), TEXT("/entities/0/properties/subImageMode") },
		{ TEXT("\"archetype\":\"sprites\",\"subImageMode\":1"), TEXT("/entities/0/properties/subImageMode") },
		{ TEXT("\"archetype\":\"sprites\",\"shape\":{\"radius\":4}"), TEXT("/entities/0/properties/shape/type") },
		{ TEXT("\"archetype\":\"sprites\",\"shape\":{\"type\":\"torus\"}"), TEXT("/entities/0/properties/shape/type") },
		{ TEXT("\"archetype\":\"sprites\",\"shape\":{\"type\":\"sphere\"}"), TEXT("/entities/0/properties/shape/radius") },
		{ TEXT("\"archetype\":\"sprites\",\"shape\":{\"type\":\"box\",\"halfExtent\":[1,1,1],\"radius\":4}"), TEXT("/entities/0/properties/shape/radius") },
		{ TEXT("\"archetype\":\"sprites\",\"shape\":{\"type\":\"cylinder\",\"radius\":4}"), TEXT("/entities/0/properties/shape/height") },
		{ TEXT("\"archetype\":\"sprites\",\"shape\":{\"type\":\"point\",\"height\":4}"), TEXT("/entities/0/properties/shape/height") },
		{ TEXT("\"archetype\":\"sprites\",\"shape\":{\"type\":\"sphere\",\"radius\":10001}"), TEXT("/entities/0/properties/shape/radius") },
		{ TEXT("\"archetype\":\"sprites\",\"shape\":{\"type\":\"box\",\"halfExtent\":[1,-1,1]}"), TEXT("/entities/0/properties/shape/halfExtent/1") },
		{ TEXT("\"archetype\":\"sprites\",\"shape\":{\"type\":\"point\",\"depth\":4}"), TEXT("/entities/0/properties/shape/depth") },
	};
	for (const FCase& Case : Cases)
	{
		const FMantleParseResult Result = ParseEffect(Case.Properties);
		TestFalse(FString::Printf(TEXT("%s is refused"), Case.Properties), Result.bSuccess);
		TestTrue(FString::Printf(TEXT("%s is refused at %s (got %s)"), Case.Properties, Case.Path, *Result.ErrorSummary()), HasErrorAt(Result, Case.Path));
	}

	// The built-in archetypes and the sprites one are all places can name; scripts cannot play sprites.
	EMantleEffectArchetype Archetype = EMantleEffectArchetype::Smoke;
	TestTrue(TEXT("sprites is a schema archetype"), FMantleEffects::ParseArchetype(TEXT("sprites"), Archetype) && Archetype == EMantleEffectArchetype::Sprites);
	TestFalse(TEXT("but not Sprites to a script"), FMantleEffects::ParseArchetypeAnyCase(TEXT("Sprites"), Archetype));
	TestTrue(TEXT("Fire to a script"), FMantleEffects::ParseArchetypeAnyCase(TEXT("Fire"), Archetype) && Archetype == EMantleEffectArchetype::Fire);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleEffectSpritesSettingsTest,
	"Mantle.Runtime.Effect.Sprites.Settings",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleEffectSpritesSettingsTest::RunTest(const FString& Parameters)
{
	// Pairs resolve the way the schema says.
	double Min = 1.0, Max = 9.0;
	FMantleEffects::ResolveSpritesPair(true, false, Min, Max);
	TestEqual(TEXT("a lone min gives the max its value"), Max, 1.0);
	Min = 1.0;
	Max = 9.0;
	FMantleEffects::ResolveSpritesPair(false, true, Min, Max);
	TestEqual(TEXT("a lone max gives the min its value"), Min, 9.0);
	Min = 1.0;
	Max = 9.0;
	FMantleEffects::ResolveSpritesPair(false, false, Min, Max);
	TestTrue(TEXT("neither keeps both"), Min == 1.0 && Max == 9.0);

	// Settings are held to the schema's limits and pairs.
	FMantleSpritesSettings Wild;
	Wild.Alpha = 4.0;
	Wild.SizeMin = 5000.0;
	Wild.SizeMax = 2.0;
	Wild.SizeEndScale = 99.0;
	Wild.LifetimeMin = 0.0;
	Wild.LifetimeMax = 100.0;
	Wild.VelocityMin = FVector(0.0, 9000.0, -9000.0);
	Wild.VelocityMax = FVector(-1.0, 0.0, 0.0);
	Wild.Acceleration = FVector(1.0e9, 0.0, 0.0);
	Wild.SphereRadius = std::numeric_limits<double>::quiet_NaN();
	Wild.CylinderHeight = -3.0;
	Wild.MaxParticles = 99999;
	Wild.SubImageColumns = 99;
	Wild.SubImageRows = 0;
	const FMantleSpritesSettings Held = FMantleEffects::ClampSprites(Wild);
	TestEqual(TEXT("alpha"), Held.Alpha, 1.0);
	TestEqual(TEXT("size min"), Held.SizeMin, FMantlePlaceLimits::MaxEffectSize);
	TestEqual(TEXT("size max follows the min"), Held.SizeMax, Held.SizeMin);
	TestEqual(TEXT("end scale"), Held.SizeEndScale, FMantlePlaceLimits::MaxSpriteEndScale);
	TestEqual(TEXT("lifetime min"), Held.LifetimeMin, FMantlePlaceLimits::MinEffectLifetime);
	TestEqual(TEXT("lifetime max"), Held.LifetimeMax, FMantlePlaceLimits::MaxEffectLifetime);
	TestEqual(TEXT("velocity min y"), Held.VelocityMin.Y, FMantlePlaceLimits::MaxSpriteVector);
	TestEqual(TEXT("velocity min z"), Held.VelocityMin.Z, -FMantlePlaceLimits::MaxSpriteVector);
	TestTrue(TEXT("velocity max is raised to the min on each axis"), Held.VelocityMax.X >= Held.VelocityMin.X && Held.VelocityMax.Y >= Held.VelocityMin.Y && Held.VelocityMax.Z >= Held.VelocityMin.Z);
	TestEqual(TEXT("acceleration"), Held.Acceleration.X, FMantlePlaceLimits::MaxSpriteVector);
	TestEqual(TEXT("a radius that is not a number takes the default"), Held.SphereRadius, FMantleSpritesSettings().SphereRadius);
	TestEqual(TEXT("height"), Held.CylinderHeight, 0.0);
	TestEqual(TEXT("max particles"), Held.MaxParticles, FMantlePlaceLimits::MaxSpriteParticles);
	TestEqual(TEXT("sheet columns"), Held.SubImageColumns, FMantlePlaceLimits::MaxSpriteSubImages);
	TestEqual(TEXT("sheet rows"), Held.SubImageRows, 1);

	// The rate keeps live particles within MaxParticles.
	FMantleSpritesSettings Limited;
	Limited.LifetimeMax = 4.0;
	Limited.MaxParticles = 100;
	TestEqual(TEXT("a rate that would pass the cap is lowered"), FMantleEffects::SpritesEffectiveRate(200.0, Limited), 25.0);
	TestEqual(TEXT("a rate under it is kept"), FMantleEffects::SpritesEffectiveRate(10.0, Limited), 10.0);

	// Names and paths.
	EMantleSpritesShape Shape = EMantleSpritesShape::Point;
	TestTrue(TEXT("cylinder parses"), FMantleEffects::ParseShape(TEXT("cylinder"), Shape) && Shape == EMantleSpritesShape::Cylinder);
	TestFalse(TEXT("Cylinder is case-exact"), FMantleEffects::ParseShape(TEXT("Cylinder"), Shape));
	EMantleSpritesSubImageMode Mode = EMantleSpritesSubImageMode::Random;
	TestTrue(TEXT("overLife parses"), FMantleEffects::ParseSubImageMode(TEXT("overLife"), Mode) && Mode == EMantleSpritesSubImageMode::OverLife);
	TestFalse(TEXT("overlife is case-exact"), FMantleEffects::ParseSubImageMode(TEXT("overlife"), Mode));
	TestEqual(TEXT("random's name"), FString(FMantleEffects::SubImageModeName(EMantleSpritesSubImageMode::Random)), FString(TEXT("random")));
	EMantleSpritesBlend Blend = EMantleSpritesBlend::Translucent;
	TestTrue(TEXT("additive parses"), FMantleEffects::ParseBlend(TEXT("additive"), Blend) && Blend == EMantleSpritesBlend::Additive);
	TestEqual(TEXT("the additive master"), FMantleEffects::SpritesMaterialPath(EMantleSpritesBlend::Additive), FString(TEXT("/Game/Mantle/Effects/M_MantleSprite_Additive.M_MantleSprite_Additive")));
	TestEqual(TEXT("the sprites system"), FMantleEffects::SystemPath(EMantleEffectArchetype::Sprites), FString(TEXT("/Game/Mantle/Effects/NS_MantleSprites.NS_MantleSprites")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleEffectSpritesApplyTest,
	"Mantle.Runtime.Effect.Sprites.Apply",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleEffectSpritesApplyTest::RunTest(const FString& Parameters)
{
	using namespace MantleEffectSpritesTestPrivate;

	// A Cascade component stands in for Niagara's: both are UFXSystemComponents, and the sprites parameters go through that interface.
	UParticleSystemComponent* Component = NewObject<UParticleSystemComponent>(GetTransientPackage());
	FMantleEffectLook Look = FMantleEffects::DefaultLook(EMantleEffectArchetype::Sprites);
	Look.Rate = 200.0;
	FMantleSpritesSettings Settings;
	Settings.LifetimeMax = 4.0;
	Settings.MaxParticles = 100;
	Settings.SizeMin = 3.0;
	Settings.SizeMax = 8.0;
	Settings.VelocityMax = FVector(1.0, 2.0, 3.0);
	Settings.Shape = EMantleSpritesShape::Sphere;
	Settings.SphereRadius = 30.0;
	Settings.CylinderHeight = 10.0;
	TestTrue(TEXT("with no material to hand over, writing succeeds"), AMantleEffect::WriteSpritesParameters(*Component, Look, Settings, nullptr));

	bool bFound = false;
	TestEqual(TEXT("the rate is held to the particle cap"), InstanceScalar(*Component, FMantleEffects::RateParameter, bFound), 25.0f);
	TestTrue(TEXT("and was written"), bFound);
	TestEqual(TEXT("size min"), InstanceScalar(*Component, FMantleEffects::SizeMinParameter, bFound), 3.0f);
	TestEqual(TEXT("size max"), InstanceScalar(*Component, FMantleEffects::SizeMaxParameter, bFound), 8.0f);
	TestEqual(TEXT("lifetime max"), InstanceScalar(*Component, FMantleEffects::LifetimeMaxParameter, bFound), 4.0f);
	TestEqual(TEXT("sphere radius"), InstanceScalar(*Component, FMantleEffects::SphereRadiusParameter, bFound), 30.0f);
	TestEqual(TEXT("cylinder height"), InstanceScalar(*Component, FMantleEffects::CylinderHeightParameter, bFound), 10.0f);
	const FParticleSysParam* Velocity = Component->InstanceParameters.FindByPredicate(
		[](const FParticleSysParam& Param) { return Param.Name == FName(FMantleEffects::VelocityMaxParameter); });
	if (TestNotNull(TEXT("the velocity range was written"), Velocity))
	{
		TestEqual(TEXT("velocity max"), Velocity->Vector, FVector(1.0, 2.0, 3.0));
	}

	// The master materials may not be in a test build; where they are, the instance carries the settings.
	UTexture2D* Texture = UTexture2D::CreateTransient(4, 4);
	FMantleSpritesSettings Look2;
	Look2.Alpha = 0.7;
	Look2.AlphaEnd = 0.2;
	Look2.FadeIn = 0.3;
	Look2.SizeEndScale = 2.0;
	Look2.SubImageColumns = 4;
	Look2.SubImageRows = 2;
	Look2.SubImageMode = EMantleSpritesSubImageMode::OverLife;
	if (UMaterialInstanceDynamic* Instance = AMantleEffect::MakeSpritesMaterial(GetTransientPackage(), Look, Look2, Texture))
	{
		float Value = -1.0f;
		FLinearColor Sheet = FLinearColor::Black;
		TestTrue(TEXT("the sheet is set"), Instance->GetVectorParameterValue(FName(FMantleEffects::SubImagesParameter), Sheet) && Sheet.R == 4.0f && Sheet.G == 2.0f);
		TestTrue(TEXT("and plays over life"), Instance->GetScalarParameterValue(FName(FMantleEffects::SubImageOverLifeParameter), Value) && Value == 1.0f);
		TestTrue(TEXT("alpha is set"), Instance->GetScalarParameterValue(FName(FMantleEffects::AlphaParameter), Value) && FMath::IsNearlyEqual(Value, 0.7f));
		TestTrue(TEXT("use texture is on"), Instance->GetScalarParameterValue(FName(FMantleEffects::UseTextureParameter), Value) && Value == 1.0f);
		TestTrue(TEXT("end scale is set"), Instance->GetScalarParameterValue(FName(FMantleEffects::SizeEndScaleParameter), Value) && Value == 2.0f);
	}
	// No sheet leaves the material as it was drawn before sheets: 1 by 1, a random frame.
	if (UMaterialInstanceDynamic* Plain = AMantleEffect::MakeSpritesMaterial(GetTransientPackage(), Look, FMantleSpritesSettings(), nullptr))
	{
		float Value = -1.0f;
		FLinearColor Sheet = FLinearColor::Black;
		TestTrue(TEXT("the sheet is 1 by 1"), Plain->GetVectorParameterValue(FName(FMantleEffects::SubImagesParameter), Sheet) && Sheet.R == 1.0f && Sheet.G == 1.0f);
		TestTrue(TEXT("with a random frame"), Plain->GetScalarParameterValue(FName(FMantleEffects::SubImageOverLifeParameter), Value) && Value == 0.0f);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
