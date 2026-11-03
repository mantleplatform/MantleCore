// Copyright Joshua Eger. All Rights Reserved.
#include "MantleWheelDocument.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

// The wheel publish's pure parts: the wheel document (format MWL1, the backend's schema/examples/wheel/valid-wheel.json) and the
// choice of which of its slots wear the car's wheels paint layer. No assets, no network.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleWheelPublishTestPrivate
{
	const TCHAR* const MeshId = TEXT("ast_u00000000000000000000000000000001");
	const TCHAR* const MaterialId = TEXT("ast_u00000000000000000000000000000002");

	/** schema/examples/wheel/valid-wheel.json in the mantle repository. */
	const TCHAR* const ExampleJson = TEXT("{\"schemaVersion\":1,\"name\":\"Split spoke\",\"mesh\":\"ast_u00000000000000000000000000000001\",")
		TEXT("\"materials\":[\"ast_u00000000000000000000000000000002\",\"ast_u00000000000000000000000000000002\"],\"referenceRadius\":34,")
		TEXT("\"referenceWidth\":22,\"wheelSlots\":[0]}");

	TSharedPtr<FJsonObject> ReadObject(const FString& Json)
	{
		TSharedPtr<FJsonObject> Root;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root);
		return Root;
	}

	TArray<FString> KeysOf(const FJsonObject& Object)
	{
		TArray<FString> Keys;
		Object.Values.GetKeys(Keys);
		Keys.Sort();
		return Keys;
	}

	FMantleWheelSlotFacts Slot(const TCHAR* SlotName, const TCHAR* MaterialName, bool bPaint = false)
	{
		FMantleWheelSlotFacts Facts;
		Facts.SlotName = SlotName;
		Facts.MaterialName = MaterialName;
		Facts.bPaint = bPaint;
		return Facts;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleWheelPublishDocumentTest, "Mantle.Wheel.Publish.Document",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMantleWheelPublishDocumentTest::RunTest(const FString& Parameters)
{
	using namespace MantleWheelPublishTestPrivate;
	FString Json;
	FString Error;
	TestTrue(TEXT("a wheel like the schema's example builds"),
		FMantleWheelDocument::Build(TEXT("Split spoke"), MeshId, { MaterialId, MaterialId }, 34.0, 22.0, { 0 }, Json, Error));
	TestTrue(TEXT("a good document has no error"), Error.IsEmpty());

	const TSharedPtr<FJsonObject> Made = ReadObject(Json);
	const TSharedPtr<FJsonObject> Example = ReadObject(ExampleJson);
	if (!TestTrue(TEXT("the document is JSON"), Made.IsValid()) || !TestTrue(TEXT("the example is JSON"), Example.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("the same keys as the schema's example"), KeysOf(*Made) == KeysOf(*Example));
	TestEqual(TEXT("schemaVersion"), Made->GetIntegerField(TEXT("schemaVersion")), 1);
	TestEqual(TEXT("name"), Made->GetStringField(TEXT("name")), FString(TEXT("Split spoke")));
	TestEqual(TEXT("mesh"), Made->GetStringField(TEXT("mesh")), FString(MeshId));
	TestEqual(TEXT("one material id per slot"), Made->GetArrayField(TEXT("materials")).Num(), 2);
	TestEqual(TEXT("referenceRadius"), Made->GetNumberField(TEXT("referenceRadius")), 34.0);
	TestEqual(TEXT("referenceWidth"), Made->GetNumberField(TEXT("referenceWidth")), 22.0);
	const TArray<TSharedPtr<FJsonValue>>& Slots = Made->GetArrayField(TEXT("wheelSlots"));
	if (TestEqual(TEXT("one wheel slot"), Slots.Num(), 1))
	{
		TestEqual(TEXT("slot 0"), static_cast<int32>(Slots[0]->AsNumber()), 0);
	}

	// Without wheel slots the key is left out, as the schema's optional key.
	TestTrue(TEXT("a wheel with no paint slots builds"), FMantleWheelDocument::Build(TEXT("Plain"), MeshId, { MaterialId }, 30.0, 20.0, {}, Json, Error));
	TestFalse(TEXT("no wheelSlots key when none"), ReadObject(Json)->HasField(TEXT("wheelSlots")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleWheelPublishRefusalsTest, "Mantle.Wheel.Publish.Refusals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMantleWheelPublishRefusalsTest::RunTest(const FString& Parameters)
{
	using namespace MantleWheelPublishTestPrivate;
	FString Json;
	auto Refused = [&Json](const FString& Name, const FString& Mesh, const TArray<FString>& Materials, double Radius, double Width, const TArray<int32>& Slots, FString& Why)
	{
		Why.Reset();
		const bool bBuilt = FMantleWheelDocument::Build(Name, Mesh, Materials, Radius, Width, Slots, Json, Why);
		return !bBuilt && !Why.IsEmpty() && Json.IsEmpty();
	};
	FString Why;
	TestTrue(TEXT("an empty name is refused with a message"), Refused(TEXT("  "), MeshId, { MaterialId }, 34.0, 22.0, {}, Why));
	TestTrue(TEXT("a name of 64 characters is taken"), FMantleWheelDocument::Build(FString::ChrN(64, TEXT('a')), MeshId, { MaterialId }, 34.0, 22.0, {}, Json, Why));
	TestTrue(TEXT("a name over 64 characters is refused with a message"), Refused(FString::ChrN(65, TEXT('a')), MeshId, { MaterialId }, 34.0, 22.0, {}, Why));
	TestTrue(TEXT("the message says how long a name may be"), Why.Contains(TEXT("64")));
	TestTrue(TEXT("a slot past the materials is refused"), Refused(TEXT("W"), MeshId, { MaterialId, MaterialId }, 34.0, 22.0, { 2 }, Why));
	TestTrue(TEXT("the message names the slot"), Why.Contains(TEXT("2")));
	TestTrue(TEXT("a negative slot is refused"), Refused(TEXT("W"), MeshId, { MaterialId }, 34.0, 22.0, { -1 }, Why));
	TestTrue(TEXT("a slot listed twice is refused"), Refused(TEXT("W"), MeshId, { MaterialId, MaterialId }, 34.0, 22.0, { 1, 1 }, Why));
	TestTrue(TEXT("a wheel slot with no materials is refused"), Refused(TEXT("W"), MeshId, {}, 34.0, 22.0, { 0 }, Why));
	TestTrue(TEXT("a missing mesh id is refused"), Refused(TEXT("W"), FString(), { MaterialId }, 34.0, 22.0, {}, Why));
	TestTrue(TEXT("a missing material id is refused"), Refused(TEXT("W"), MeshId, { FString() }, 34.0, 22.0, {}, Why));
	TestTrue(TEXT("a radius under the minimum is refused"), Refused(TEXT("W"), MeshId, { MaterialId }, 4.0, 22.0, {}, Why));
	TestTrue(TEXT("a radius over the maximum is refused"), Refused(TEXT("W"), MeshId, { MaterialId }, 151.0, 22.0, {}, Why));
	TestTrue(TEXT("a width under the minimum is refused"), Refused(TEXT("W"), MeshId, { MaterialId }, 34.0, 1.0, {}, Why));
	TestTrue(TEXT("a width over the maximum is refused"), Refused(TEXT("W"), MeshId, { MaterialId }, 34.0, 101.0, {}, Why));
	TestTrue(TEXT("a size that is not a number is refused"), Refused(TEXT("W"), MeshId, { MaterialId }, TNumericLimits<double>::QuietNaN(), 22.0, {}, Why));

	TArray<FString> TooMany;
	TooMany.Init(MaterialId, FMantleWheelDocument::MaxMaterials + 1);
	TestTrue(TEXT("more than 64 materials are refused"), Refused(TEXT("W"), MeshId, TooMany, 34.0, 22.0, {}, Why));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleWheelPublishSlotsTest, "Mantle.Wheel.Publish.WheelSlots",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMantleWheelPublishSlotsTest::RunTest(const FString& Parameters)
{
	using namespace MantleWheelPublishTestPrivate;
	// The rim by its slot name, the tire by neither: the car's wheels color colors the rim and leaves the tire alone.
	const TArray<FMantleWheelSlotFacts> RimAndTire = { Slot(TEXT("Rim"), TEXT("M_Metal")), Slot(TEXT("Tire"), TEXT("M_Rubber")) };
	TestTrue(TEXT("a slot named rim wears the wheels layer, the tire does not"), FMantleWheelDocument::ChooseWheelSlots(RimAndTire) == TArray<int32>({ 0 }));

	// The rim by its material's name when the slot is called something generic.
	const TArray<FMantleWheelSlotFacts> ByMaterialName = { Slot(TEXT("Material_0"), TEXT("M_Tire")), Slot(TEXT("Material_1"), TEXT("M_Alloy")) };
	TestTrue(TEXT("a material named like a rim wears it"), FMantleWheelDocument::ChooseWheelSlots(ByMaterialName) == TArray<int32>({ 1 }));

	// Paint wears it whatever it is called.
	const TArray<FMantleWheelSlotFacts> Painted = { Slot(TEXT("A"), TEXT("M_A"), true), Slot(TEXT("B"), TEXT("M_B")), Slot(TEXT("Wheel_Tire"), TEXT("M_C"), false) };
	TestTrue(TEXT("a paint material wears it; a tire named with the word wheel does not"), FMantleWheelDocument::ChooseWheelSlots(Painted) == TArray<int32>({ 0 }));

	TestTrue(TEXT("each slot at most once, in order"), FMantleWheelDocument::ChooseWheelSlots(TArray<FMantleWheelSlotFacts>({ Slot(TEXT("Rim"), TEXT("Alloy"), true), Slot(TEXT("Spoke"), TEXT("X")) })) == TArray<int32>({ 0, 1 }));
	TestEqual(TEXT("no slots, no choice"), FMantleWheelDocument::ChooseWheelSlots(TArray<FMantleWheelSlotFacts>()).Num(), 0);

	// A chosen slot is always one the document accepts.
	FString Json;
	FString Error;
	const TArray<int32> Chosen = FMantleWheelDocument::ChooseWheelSlots(RimAndTire);
	TestTrue(TEXT("the chosen slots build into a document"), FMantleWheelDocument::Build(TEXT("W"), MeshId, { MaterialId, MaterialId }, 34.0, 22.0, Chosen, Json, Error));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
