// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleBuilder.h"
#include "MantleVehicleConverter.h"
#include "MantleVehicleSource.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Materials/Material.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS

// Paint layers (second color, wheels, interior, dashboard): the converter finds which layer a part is for from what the model says, the paint step carries the creator's
// choice onto the vehicle, and the vehicle document names each layer.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleVehiclePaintLayersGuessTest, "Mantle.Vehicle.PaintLayers.GuessFromNames",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehiclePaintLayersGuessTest::RunTest(const FString&)
{
	using FGuess = FMantleVehicleConverter::FPaintLayerGuess;
	// A plain white surface with no picture: any color would show on it.
	const TStrongObjectPtr<UMaterial> Plain(NewObject<UMaterial>(GetTransientPackage()));

	// A GTA model's tint layers: the body paint and the second paint, whatever the picture.
	FGuess Primary = FMantleVehicleConverter::GuessPaintLayer(TEXT("PRIMARY"), Plain.Get());
	TestTrue(TEXT("PRIMARY is the body paint"), Primary.bPainted && Primary.bPrimary);
	Primary = FMantleVehicleConverter::GuessPaintLayer(TEXT("PRIMARY_2"), Plain.Get());
	TestTrue(TEXT("and so is PRIMARY_2"), Primary.bPainted && Primary.bPrimary);
	const FGuess Secondary = FMantleVehicleConverter::GuessPaintLayer(TEXT("SECONDARY"), Plain.Get());
	TestTrue(TEXT("SECONDARY is the second paint"), Secondary.bPainted && !Secondary.bPrimary && Secondary.Layer == EMantleVehicleSourcePaintLayer::Secondary);

	// The words.
	struct FCase
	{
		const TCHAR* Name;
		EMantleVehicleSourcePaintLayer Layer;
	};
	const FCase Cases[] = {
		{ TEXT("Rim_FL"), EMantleVehicleSourcePaintLayer::Wheels },
		{ TEXT("alloy_wheel"), EMantleVehicleSourcePaintLayer::Wheels },
		{ TEXT("HubCap"), EMantleVehicleSourcePaintLayer::Wheels },
		{ TEXT("vehicle_dashboard"), EMantleVehicleSourcePaintLayer::Dashboard },
		{ TEXT("M_Hatchback_Interior"), EMantleVehicleSourcePaintLayer::Interior },
		{ TEXT("seat_front"), EMantleVehicleSourcePaintLayer::Interior },
		{ TEXT("body_trim"), EMantleVehicleSourcePaintLayer::Secondary },
		{ TEXT("RoofStripe"), EMantleVehicleSourcePaintLayer::Secondary },
	};
	for (const FCase& Case : Cases)
	{
		const FGuess Result = FMantleVehicleConverter::GuessPaintLayer(Case.Name, Plain.Get());
		TestTrue(*FString::Printf(TEXT("%s is a layer"), Case.Name), Result.bPainted && !Result.bPrimary);
		TestTrue(*FString::Printf(TEXT("and the right one: %s"), Case.Name), Result.Layer == Case.Layer);
	}

	// Never glass, a tire, a lamp, a brake or a steering wheel; a word inside another word is not the word ("trim" is not "rim").
	for (const TCHAR* Name : { TEXT("vehicle_glass"), TEXT("Window_Front"), TEXT("tire_FL"), TEXT("tyre"), TEXT("RimTire"), TEXT("Headlights"), TEXT("brake_disc_rim"), TEXT("SteeringWheel"), TEXT("chrome_trim") })
	{
		const FGuess Result = FMantleVehicleConverter::GuessPaintLayer(Name, Plain.Get());
		TestFalse(*FString::Printf(TEXT("%s is not a layer"), Name), Result.bPainted && !Result.bPrimary);
	}
	TestFalse(TEXT("a part named for none of it, with a plain material, is not painted"), FMantleVehicleConverter::GuessPaintLayer(TEXT("vehicle_cab"), Plain.Get()).bPainted);

	// "Light" is a color as often as a lamp: a light grey interior is still the interior.
	const FGuess LightGrey = FMantleVehicleConverter::GuessPaintLayer(TEXT("M_Interior_LightGrey"), Plain.Get());
	TestTrue(TEXT("M_Interior_LightGrey is the interior"), LightGrey.bPainted && !LightGrey.bPrimary && LightGrey.Layer == EMantleVehicleSourcePaintLayer::Interior);
	TestFalse(TEXT("but interior_light is a lamp"), FMantleVehicleConverter::GuessPaintLayer(TEXT("interior_lights"), Plain.Get()).bPainted && !FMantleVehicleConverter::GuessPaintLayer(TEXT("interior_lights"), Plain.Get()).bPrimary);

	// What already reads as the car's paint stays the body color, whatever the section is called: a Roof or Mirrors section on the car's own paint is painted as it was.
	const TStrongObjectPtr<UMaterial> CarPaint(NewObject<UMaterial>(GetTransientPackage()));
	CarPaint->SetShadingModel(MSM_ClearCoat);
	CarPaint->PostEditChange();
	if (TestTrue(TEXT("the test's car paint reads as car paint"), FMantleVehicleConverter::LooksLikePaint(CarPaint.Get())))
	{
		for (const TCHAR* Name : { TEXT("Roof"), TEXT("Mirrors"), TEXT("Trim"), TEXT("Rim_FL") })
		{
			const FGuess Result = FMantleVehicleConverter::GuessPaintLayer(Name, CarPaint.Get());
			TestTrue(*FString::Printf(TEXT("%s on the car's own paint stays the body color"), Name), Result.bPainted && Result.bPrimary);
		}
	}

	// No material at all does not crash; a layer word with no material is left out (its color is unknown).
	TestFalse(TEXT("no material, no name: not painted"), FMantleVehicleConverter::GuessPaintLayer(TEXT("cab"), nullptr).bPainted);
	TestFalse(TEXT("a layer word with no material: its color is unknown, so it is left out"), FMantleVehicleConverter::GuessPaintLayer(TEXT("rim"), nullptr).bPainted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleVehiclePaintLayersApplyTest, "Mantle.Vehicle.PaintLayers.ApplyPaintSortsLayers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehiclePaintLayersApplyTest::RunTest(const FString&)
{
	const TStrongObjectPtr<UMantleVehicleSource> Source(NewObject<UMantleVehicleSource>(GetTransientPackage()));
	const TArray<FString> Sections = { TEXT("Body"), TEXT("Roof"), TEXT("Rims"), TEXT("Seats"), TEXT("Glass") };
	const TArray<bool> Painted = { true, true, true, true, false };
	const TArray<uint8> Picks = { 0, FMantleVehicleBuilder::PaintPick(EMantleVehicleSourcePaintLayer::Secondary), FMantleVehicleBuilder::PaintPick(EMantleVehicleSourcePaintLayer::Wheels),
		FMantleVehicleBuilder::PaintPick(EMantleVehicleSourcePaintLayer::Interior), 0 };
	TArray<FString> Notes;
	FMantleVehicleBuilder::ApplyPaint(*Source, Sections, Painted, Notes, &Picks);

	TestTrue(TEXT("the body is the body paint"), Source->Paint.Sections.Num() == 1 && Source->Paint.Sections[0] == TEXT("Body"));
	TestEqual(TEXT("three other layers"), Source->Paint.Layers.Num(), 3);
	const auto SectionsOf = [&Source](const EMantleVehicleSourcePaintLayer Layer) -> TArray<FString>
	{
		const FMantleVehicleSourcePaintLayer* Found = Source->Paint.Layers.FindByPredicate([Layer](const FMantleVehicleSourcePaintLayer& Has) { return Has.Layer == Layer; });
		return Found ? Found->Sections : TArray<FString>();
	};
	TestTrue(TEXT("the roof is the second paint"), SectionsOf(EMantleVehicleSourcePaintLayer::Secondary) == TArray<FString>({ TEXT("Roof") }));
	TestTrue(TEXT("the rims are the wheels'"), SectionsOf(EMantleVehicleSourcePaintLayer::Wheels) == TArray<FString>({ TEXT("Rims") }));
	TestTrue(TEXT("the seats are the interior's"), SectionsOf(EMantleVehicleSourcePaintLayer::Interior) == TArray<FString>({ TEXT("Seats") }));
	const FMantleVehicleSourcePaintLayer* Interior = Source->Paint.Layers.FindByPredicate([](const FMantleVehicleSourcePaintLayer& Has) { return Has.Layer == EMantleVehicleSourcePaintLayer::Interior; });
	TestTrue(TEXT("the interior starts in the trim finish, white"), Interior && Interior->Finish == EMantleVehicleSourcePaintFinish::Trim && Interior->Color.Equals(FLinearColor::White));

	// A layer nothing wears any more is not kept.
	const TArray<bool> NoRoof = { true, false, true, true, false };
	FMantleVehicleBuilder::ApplyPaint(*Source, Sections, NoRoof, Notes, &Picks);
	TestEqual(TEXT("with the roof unticked the second paint is gone"), Source->Paint.Layers.Num(), 2);
	TestFalse(TEXT("and nothing is called the second paint"), Source->Paint.Layers.ContainsByPredicate([](const FMantleVehicleSourcePaintLayer& Has) { return Has.Layer == EMantleVehicleSourcePaintLayer::Secondary; }));

	// With no layers given every ticked section is the body paint, as the paint step always set them.
	FMantleVehicleBuilder::ApplyPaint(*Source, Sections, Painted, Notes);
	TestEqual(TEXT("ticked, with no layers given, they are all the body paint"), Source->Paint.Sections.Num(), 4);
	TestEqual(TEXT("and there are no other layers"), Source->Paint.Layers.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleVehiclePaintLayersMergeTest, "Mantle.Vehicle.PaintLayers.OneEntryPerLayer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehiclePaintLayersMergeTest::RunTest(const FString&)
{
	// A hand-edited part with Wheels {0} and Wheels {2}: the vehicle file lists a layer once, so the entries are merged, and the check says so.
	FMantleVehicleSourcePart Part;
	FMantleVehicleSourcePartLayerSlots& First = Part.LayerSlots.AddDefaulted_GetRef();
	First.Layer = EMantleVehicleSourcePaintLayer::Wheels;
	First.Slots = { 0 };
	FMantleVehicleSourcePartLayerSlots& Interior = Part.LayerSlots.AddDefaulted_GetRef();
	Interior.Layer = EMantleVehicleSourcePaintLayer::Interior;
	Interior.Slots = { 1 };
	FMantleVehicleSourcePartLayerSlots& Second = Part.LayerSlots.AddDefaulted_GetRef();
	Second.Layer = EMantleVehicleSourcePaintLayer::Wheels;
	Second.Slots = { 2, 0 };

	const TArray<EMantleVehicleSourcePaintLayer> Twice = FMantleVehicleConverter::LayersListedTwice(Part);
	TestTrue(TEXT("the check finds the layer listed twice"), Twice.Num() == 1 && Twice[0] == EMantleVehicleSourcePaintLayer::Wheels);

	const TArray<FMantleVehicleSourcePartLayerSlots> Merged = FMantleVehicleConverter::MergeLayerSlots(Part.LayerSlots);
	if (TestEqual(TEXT("merged, each layer is once"), Merged.Num(), 2))
	{
		TestTrue(TEXT("the wheels come first, as they were first listed"), Merged[0].Layer == EMantleVehicleSourcePaintLayer::Wheels);
		TestTrue(TEXT("with every slot once: 0 and 2"), Merged[0].Slots == TArray<int32>({ 0, 2 }));
		TestTrue(TEXT("and the interior's one slot"), Merged[1].Layer == EMantleVehicleSourcePaintLayer::Interior && Merged[1].Slots == TArray<int32>({ 1 }));
	}
	FMantleVehicleSourcePart Fine;
	Fine.LayerSlots = Merged;
	TestEqual(TEXT("a part that lists each layer once has nothing to report"), FMantleVehicleConverter::LayersListedTwice(Fine).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleVehiclePaintLayersDocumentTest, "Mantle.Vehicle.PaintLayers.PartsCarryLayerSlots",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehiclePaintLayersDocumentTest::RunTest(const FString&)
{
	FMantleVehicleConverter::FPartIds Part;
	Part.Mesh = TEXT("ast_u00112233445566778899aabbccddeef0");
	Part.Bone = TEXT("wheel_fl");
	Part.PaintSlots = { 1 };
	FMantleVehicleSourcePartLayerSlots& Wheels = Part.LayerSlots.AddDefaulted_GetRef();
	Wheels.Layer = EMantleVehicleSourcePaintLayer::Wheels;
	Wheels.Slots = { 0, 2 };
	const TSharedRef<FJsonObject> Document = FMantleVehicleConverter::WithParts(MakeShared<FJsonObject>(), { Part });
	const TArray<TSharedPtr<FJsonValue>>* Parts = nullptr;
	if (!TestTrue(TEXT("the parts are written"), Document->TryGetArrayField(TEXT("parts"), Parts) && Parts && Parts->Num() == 1))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* Entry = nullptr;
	if (!TestTrue(TEXT("as objects"), (*Parts)[0]->TryGetObject(Entry)))
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Layers = nullptr;
	if (!TestTrue(TEXT("with layerSlots"), (*Entry)->TryGetArrayField(TEXT("layerSlots"), Layers) && Layers && Layers->Num() == 1))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* LayerEntry = nullptr;
	TestTrue(TEXT("each an object"), (*Layers)[0]->TryGetObject(LayerEntry));
	if (LayerEntry)
	{
		FString Name;
		(*LayerEntry)->TryGetStringField(TEXT("layer"), Name);
		TestEqual(TEXT("naming the layer"), Name, FString(TEXT("wheels")));
		const TArray<TSharedPtr<FJsonValue>>* Slots = nullptr;
		TestTrue(TEXT("and its slots"), (*LayerEntry)->TryGetArrayField(TEXT("slots"), Slots) && Slots && Slots->Num() == 2);
	}
	TestTrue(TEXT("the primary's paintSlots are still written beside them"), (*Entry)->HasField(TEXT("paintSlots")));

	// A part with no layer slots writes no key.
	FMantleVehicleConverter::FPartIds Plain;
	Plain.Mesh = Part.Mesh;
	Plain.Bone = Part.Bone;
	const TSharedRef<FJsonObject> PlainDocument = FMantleVehicleConverter::WithParts(MakeShared<FJsonObject>(), { Plain });
	const TArray<TSharedPtr<FJsonValue>>* PlainParts = nullptr;
	const TSharedPtr<FJsonObject>* PlainEntry = nullptr;
	if (PlainDocument->TryGetArrayField(TEXT("parts"), PlainParts) && PlainParts && PlainParts->Num() == 1 && (*PlainParts)[0]->TryGetObject(PlainEntry))
	{
		TestFalse(TEXT("a part with no layer slots writes no layerSlots"), (*PlainEntry)->HasField(TEXT("layerSlots")));
	}
	return true;
}

#endif
