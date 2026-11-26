// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehiclePreset.h"

#include "Dom/JsonObject.h"
#include "MantleVehicleConverter.h"
#include "MantleVehicleSource.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// #378: the motorcycle type's engines. The type lists them with a default; "motorcycle+<id>" is the type with that engine's numbers; applying it
// fills the vehicle's tuning with them and remembers the engine; the document then says the engine's size.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehiclePresetEnginesTest,
	"Mantle.Vehicle.Presets.EnginePick",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehiclePresetEnginesTest::RunTest(const FString& Parameters)
{
	const FMantleVehiclePreset* Type = FMantleVehiclePresets::Find(TEXT("motorcycle"));
	if (!TestNotNull(TEXT("the motorcycle preset"), Type))
	{
		return false;
	}
	TArray<FString> Ids;
	for (const FMantleVehiclePresetEngine& Engine : Type->Engines)
	{
		Ids.Add(Engine.Id);
		TestFalse(*FString::Printf(TEXT("engine %s has a name"), *Engine.Id), Engine.Name.IsEmpty());
	}
	TestEqual(TEXT("five engines"), FString::Join(Ids, TEXT(",")), FString(TEXT("125,300,600,1000,1800")));
	TestEqual(TEXT("it starts with the 600"), Type->DefaultEngine, FString(TEXT("600")));
	TestTrue(TEXT("the plain type has no engine id"), Type->EngineId.IsEmpty() && Type->SelectorId() == TEXT("motorcycle"));
	TestTrue(TEXT("the sedan has no engines"), FMantleVehiclePresets::Find(TEXT("sedan")) && FMantleVehiclePresets::Find(TEXT("sedan"))->Engines.Num() == 0);
	TestNull(TEXT("an unknown engine is no preset"), FMantleVehiclePresets::Find(TEXT("motorcycle+9999")));

	const FMantleVehiclePreset* Cruiser = FMantleVehiclePresets::Find(TEXT("motorcycle+1800"));
	if (!TestNotNull(TEXT("motorcycle+1800"), Cruiser))
	{
		return false;
	}
	TestEqual(TEXT("it keeps the type's id"), Cruiser->Id, FString(TEXT("motorcycle")));
	TestEqual(TEXT("and names its engine"), Cruiser->EngineId, FString(TEXT("1800")));
	TestTrue(TEXT("found again, the same preset"), FMantleVehiclePresets::Find(TEXT("motorcycle+1800")) == Cruiser);

	UMantleVehicleSource* Source = NewObject<UMantleVehicleSource>(GetTransientPackage());
	TArray<FString> Notes;
	if (TestTrue(TEXT("the 1800 applies"), FMantleVehiclePresets::ApplyDefaults(*Cruiser, *Source, Notes)))
	{
		TestEqual(TEXT("its weight with the rider"), Source->Tuning.Mass, 450.f);
		TestEqual(TEXT("its engine size"), Source->Tuning.EngineDisplacement, 1868);
		TestEqual(TEXT("its redline"), Source->Tuning.MaxRPM, 5500);
		const TSharedRef<FJsonObject> Doc = FMantleVehicleConverter::MakeDocument(*Source, TMap<FString, FString>());
		const TSharedPtr<FJsonObject>* Engine = nullptr;
		double Cc = 0.0;
		TestTrue(TEXT("the document says 1868 cc"), Doc->TryGetObjectField(TEXT("engine"), Engine) && Engine && (*Engine)->TryGetNumberField(TEXT("displacement"), Cc) && Cc == 1868.0);
	}
	TestEqual(TEXT("a vehicle's selector id"), FMantleVehiclePresets::SelectorIdOf(FName(TEXT("motorcycle")), FName(TEXT("300"))), FString(TEXT("motorcycle+300")));
	TestEqual(TEXT("and with no engine"), FMantleVehiclePresets::SelectorIdOf(FName(TEXT("sedan")), NAME_None), FString(TEXT("sedan")));
	return true;
}

#endif
