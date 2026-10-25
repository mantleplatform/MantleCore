// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceLimits.h"
#include "MantlePlaceParser.h"
#include "MantleShadowDistance.h"

#include "HAL/IConsoleManager.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// Draw distances (#630 stage 2): what the parser reads and refuses on a StaticMesh and an InstancedMesh, and the
// shadow distance the console variable gives. How far a mesh is drawn, and that its instances fade, is the renderer's:
// the before-and-after run of tools/bench measures it, and only a picture shows the fade.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleDrawDistanceTestPrivate
{
	/** A place of one entity of Type with Properties (the text between the braces). */
	FMantleParseResult ParseEntity(const TCHAR* Type, const FString& Properties)
	{
		const FString Json = FString::Printf(
			TEXT("{\"schemaVersion\":1,\"placeId\":\"plc_drawdist01\",\"revision\":{\"number\":1,\"state\":\"draft\"},"
				 "\"metadata\":{\"name\":\"Far Field\",\"authorId\":\"usr_4b1d\"},\"settings\":{},"
				 "\"assets\":[{\"id\":\"ast_cube\",\"kind\":\"StaticMesh\",\"hash\":\"md5:0123456789abcdef\",\"bytes\":1024}],"
				 "\"entities\":[{\"id\":\"ent_mesh\",\"type\":\"%s\",\"parent\":null,"
				 "\"transform\":{\"location\":[0,0,0],\"rotation\":[0,0,0],\"scale\":[1,1,1]},\"properties\":{%s}}]}"),
			Type, *Properties);
		return FMantlePlaceParser::ParseFromString(Json);
	}

	FMantleParseResult ParseStatic(const FString& Extra)
	{
		return ParseEntity(TEXT("StaticMesh"), TEXT("\"mesh\":\"ast_cube\"") + Extra);
	}

	FMantleParseResult ParseInstanced(const FString& Extra)
	{
		return ParseEntity(TEXT("InstancedMesh"), TEXT("\"mesh\":\"ast_cube\",\"instances\":[[0,0,0,0,0,0,1,1,1]]") + Extra);
	}

	const FMantleStaticMeshProps* MeshOf(const FMantleParseResult& Result)
	{
		return Result.Place.Entities.Num() == 1 ? MantleMeshPropsOf(Result.Place.Entities[0]) : nullptr;
	}

	bool HasErrorAt(const FMantleParseResult& Result, const FString& Path)
	{
		return Result.Errors.ContainsByPredicate([&Path](const FMantleParseError& Error) { return Error.Path == Path; });
	}

	const TCHAR* const DrawDistancePath = TEXT("/entities/0/properties/drawDistance");
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleDrawDistanceParseTest,
	"Mantle.Runtime.DrawDistance.Parser.ReadsAndRefuses",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleDrawDistanceParseTest::RunTest(const FString& Parameters)
{
	using namespace MantleDrawDistanceTestPrivate;

	// Absent: 0, never culled, on both, as before the key.
	const auto CheckPlain = [this](const TCHAR* What, const FMantleParseResult& Plain)
	{
		if (!TestTrue(FString::Printf(TEXT("a %s without a drawDistance parses"), What), Plain.bSuccess))
		{
			AddError(Plain.ErrorSummary());
			return;
		}
		if (const FMantleStaticMeshProps* Mesh = MeshOf(Plain))
		{
			TestEqual(FString::Printf(TEXT("a %s's absent drawDistance is 0"), What), Mesh->DrawDistance, 0.0);
		}
	};
	CheckPlain(TEXT("StaticMesh"), ParseStatic(TEXT("")));
	CheckPlain(TEXT("InstancedMesh"), ParseInstanced(TEXT("")));

	const FMantleParseResult Static = ParseStatic(TEXT(",\"drawDistance\":6000"));
	TestTrue(TEXT("a StaticMesh with a drawDistance parses"), Static.bSuccess);
	if (const FMantleStaticMeshProps* Mesh = MeshOf(Static))
	{
		TestEqual(TEXT("StaticMesh drawDistance"), Mesh->DrawDistance, 6000.0);
	}

	const FMantleParseResult WithPhysics = ParseStatic(TEXT(",\"drawDistance\":3000,\"physics\":{\"mass\":20}"));
	TestTrue(TEXT("a drawDistance beside a physics block parses"), WithPhysics.bSuccess);

	const FMantleParseResult Instanced = ParseInstanced(TEXT(",\"drawDistance\":4500.5"));
	TestTrue(TEXT("an InstancedMesh with a drawDistance parses"), Instanced.bSuccess);
	if (const FMantleStaticMeshProps* Mesh = MeshOf(Instanced))
	{
		TestEqual(TEXT("InstancedMesh drawDistance"), Mesh->DrawDistance, 4500.5);
	}

	const FMantleParseResult Edge = ParseInstanced(TEXT(",\"drawDistance\":1000000"));
	TestTrue(TEXT("a drawDistance of 10 km parses"), Edge.bSuccess);
	TestEqual(TEXT("10 km is MaxDrawDistance"), FMantlePlaceLimits::MaxDrawDistance, 1000000.0);

	// Refused: 0 (the absent key means never culled), below it, past the limit, and anything not a number.
	for (const TCHAR* Bad : { TEXT("0"), TEXT("-100"), TEXT("1000001"), TEXT("\"6000\""), TEXT("null"), TEXT("true") })
	{
		const FString Extra = FString::Printf(TEXT(",\"drawDistance\":%s"), Bad);
		const FMantleParseResult StaticBad = ParseStatic(Extra);
		TestFalse(FString::Printf(TEXT("StaticMesh drawDistance %s is refused"), Bad), StaticBad.bSuccess);
		TestTrue(FString::Printf(TEXT("StaticMesh drawDistance %s is reported at the key"), Bad), HasErrorAt(StaticBad, DrawDistancePath));
		const FMantleParseResult InstancedBad = ParseInstanced(Extra);
		TestFalse(FString::Printf(TEXT("InstancedMesh drawDistance %s is refused"), Bad), InstancedBad.bSuccess);
		TestTrue(FString::Printf(TEXT("InstancedMesh drawDistance %s is reported at the key"), Bad), HasErrorAt(InstancedBad, DrawDistancePath));
	}

	// Only the two mesh types carry it.
	const FMantleParseResult Skeletal = ParseEntity(TEXT("SkeletalMesh"), TEXT("\"mesh\":\"ast_u0123456789abcdef0123456789abcdef\",\"drawDistance\":4000"));
	TestFalse(TEXT("a SkeletalMesh drawDistance is refused"), Skeletal.bSuccess);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleShadowDistanceForTest,
	"Mantle.Runtime.DrawDistance.ShadowDistance",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleShadowDistanceForTest::RunTest(const FString& Parameters)
{
	IConsoleVariable* Fraction = IConsoleManager::Get().FindConsoleVariable(TEXT("mantle.Place.ShadowDistanceFraction"));
	if (!TestNotNull(TEXT("mantle.Place.ShadowDistanceFraction exists"), Fraction))
	{
		return false;
	}
	const float Saved = Fraction->GetFloat();

	Fraction->Set(0.5f, ECVF_SetByCode);
	TestEqual(TEXT("half the draw distance by default"), UMantleShadowDistanceSubsystem::ShadowDistanceFor(6000.0), 3000.0);
	TestEqual(TEXT("no draw distance, no shadow distance"), UMantleShadowDistanceSubsystem::ShadowDistanceFor(0.0), 0.0);
	Fraction->Set(1.0f, ECVF_SetByCode);
	TestEqual(TEXT("1 leaves shadows to the draw distance"), UMantleShadowDistanceSubsystem::ShadowDistanceFor(6000.0), 0.0);
	Fraction->Set(0.0f, ECVF_SetByCode);
	TestEqual(TEXT("0 leaves shadows to the draw distance"), UMantleShadowDistanceSubsystem::ShadowDistanceFor(6000.0), 0.0);

	Fraction->Set(Saved, ECVF_SetByCode);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
