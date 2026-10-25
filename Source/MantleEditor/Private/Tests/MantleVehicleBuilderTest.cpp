// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleBuilder.h"
#include "MantleVehiclePreset.h"
#include "MantleVehicleScratchBody.h"
#include "MantleVehicleSource.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

// The step of Build that gives a vehicle its type, defined in MantleVehicleBuilder.cpp; declared here so it can be tested without a model.
namespace MantleVehicleBuilderPrivate
{
	bool ApplyChosenPreset(const FMantleVehiclePreset& Preset, UMantleVehicleSource& Source, FMantleVehicleBuildResult& Out);
}

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVehicleBuilderTestPrivate
{
	/** Drops an in-memory vehicle asset the test made, so nothing is left dirty in the editor or saved to disk. */
	void Discard(UMantleVehicleSource* Source)
	{
		if (!Source)
		{
			return;
		}
		UPackage* Package = Source->GetOutermost();
		FAssetRegistryModule::AssetDeleted(Source);
		Source->ClearFlags(RF_Public | RF_Standalone);
		Source->MarkAsGarbage();
		if (Package)
		{
			Package->ClearDirtyFlag();
			Package->MarkAsGarbage();
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleBuilderBuildTest,
	"Mantle.Vehicle.Builder.Build",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleBuilderBuildTest::RunTest(const FString& Parameters)
{
	// Neither input is checked before the other, so a missing model and a missing type each name their own problem.
	const FMantleVehicleBuildResult NoMesh = FMantleVehicleBuilder::Build(nullptr, TEXT("sedan"));
	TestFalse(TEXT("no model is not ok"), NoMesh.bOk);
	TestNull(TEXT("no model makes no vehicle"), NoMesh.Source);
	TestTrue(TEXT("no model asks for a model"), NoMesh.Problems.Contains(TEXT("Pick the model to use as the vehicle's body.")));

	// The model is the sedan chassis's own grey body. Converting makes <Mesh>_Vehicle beside the mesh, so a throwaway package keeps every real folder untouched.
	FString BodyError;
	USkeletalMesh* Copy = MantleVehicleScratchBody::Make(TEXT("/Temp/MantleVehicleBuilderTest"), TEXT("BuilderTestCar"), BodyError);
	if (!TestNotNull(FString::Printf(TEXT("the test model (%s)"), *BodyError), Copy))
	{
		return false;
	}

	const FMantleVehicleBuildResult NoType = FMantleVehicleBuilder::Build(Copy, TEXT("nope"));
	TestFalse(TEXT("an unknown type is not ok"), NoType.bOk);
	TestNull(TEXT("an unknown type makes no vehicle"), NoType.Source);
	TestTrue(TEXT("an unknown type asks for a type"), NoType.Problems.Contains(TEXT("Pick a vehicle type first.")));

	const FMantleVehicleBuildResult Built = FMantleVehicleBuilder::Build(Copy, TEXT("sedan"));
	TestTrue(TEXT("the model builds as a sedan"), Built.bOk);
	// The copy lives in a /Temp package, which must never be written to disk: no save happened and nothing is said about one.
	TestFalse(TEXT("a vehicle in a scratch package is not saved"), Built.bSaved);
	TestFalse(TEXT("a scratch vehicle gets no save note"), Built.Notes.ContainsByPredicate([](const FString& Note)
	{
		return Note.Contains(TEXT("save your vehicle automatically"));
	}));
	TestTrue(TEXT("a scratch vehicle has no save problem to report"), Built.SaveProblem.IsEmpty());
	// The test's model copy sits in /Temp, so it is not on disk: the creator is told to save their model, and Mantle does not save it.
	TestTrue(TEXT("an unsaved model is flagged in the notes"), Built.Notes.Contains(FMantleVehicleBuilder::ModelUnsavedNote()));
	TestTrue(TEXT("the model note says what to do"), FMantleVehicleBuilder::ModelUnsavedNote().Contains(TEXT("Save it too (Ctrl+S)")));
	if (TestNotNull(TEXT("the build made a vehicle"), Built.Source))
	{
		// One helper decides what is scratch: a /Temp vehicle is, and saving it does nothing and says nothing.
		TestTrue(TEXT("a /Temp vehicle is a scratch vehicle"), FMantleVehicleBuilder::IsScratchVehicle(*Built.Source));
		FString SaveProblem;
		TestFalse(TEXT("a scratch vehicle is not saved"), FMantleVehicleBuilder::SaveVehicle(*Built.Source, SaveProblem));
		TestTrue(TEXT("a scratch vehicle's save says nothing"), SaveProblem.IsEmpty());
		TestTrue(TEXT("the model copy counts as unsaved"), FMantleVehicleBuilder::IsModelUnsaved(*Built.Source, Copy));
		TestEqual(TEXT("the vehicle remembers its type"), Built.Source->Preset, FName(TEXT("sedan")));
		TestEqual(TEXT("the sedan's mass"), Built.Source->Tuning.Mass, 1500.f);
		TestEqual(TEXT("the sedan's top speed"), Built.Source->Tuning.MaxSpeed, 4500.f);
		// The model is only a skin: collision, centre of mass and seats are the chassis's, whatever the model's physics asset held.
		TestEqual(TEXT("the collision is the chassis's one box"), Built.Source->Tuning.Collision.Num(), 1);
		TestTrue(TEXT("the centre of mass is the chassis's"), Built.Source->Tuning.bOverrideCenterOfMass);
		TestTrue(TEXT("the driver's seat is the chassis's"), Built.Source->Tuning.bSeatLocation && Built.Source->Tuning.bSteeringWheel);
		TestTrue(TEXT("the passenger's seat is the chassis's"), Built.Source->Tuning.bPassengerSeat);
		TestTrue(TEXT("the build says the chassis is Mantle's"), Built.Notes.ContainsByPredicate([](const FString& Note)
		{
			return Note.StartsWith(TEXT("Built on the Sedan chassis"));
		}));
		// The build proposes a Size from the wheels and says why; a model whose wheels cannot be read says that instead.
		if (TestTrue(TEXT("the build found the model's fit"), Built.Fit.IsSet()))
		{
			TestEqual(TEXT("the vehicle's Size is the fit"), Built.Source->FitScale, static_cast<float>(Built.Fit->Scale));
			TestTrue(TEXT("the fit's reason is in the notes"), Built.Notes.Contains(Built.Fit->Reason));
		}
	}
	for (const FString& Problem : Built.Problems)
	{
		AddError(Problem);
	}

	// A second press on the same type keeps the creator's tuning; a different type starts again from its own defaults.
	if (Built.Source)
	{
		Built.Source->Tuning.Mass = 1800.f;
		Built.Source->FitScale = 1.5f;
		const FMantleVehicleBuildResult Again = FMantleVehicleBuilder::Build(Copy, TEXT("sedan"));
		TestTrue(TEXT("building the same type again is ok"), Again.bOk);
		if (TestNotNull(TEXT("the second build gives the vehicle"), Again.Source))
		{
			TestEqual(TEXT("the second build is the same vehicle"), Again.Source, Built.Source);
			TestEqual(TEXT("the creator's mass is kept"), Again.Source->Tuning.Mass, 1800.f);
			TestEqual(TEXT("the creator's Size is kept"), Again.Source->FitScale, 1.5f);
		}
		TestTrue(TEXT("the second build says it kept the settings"), Again.Notes.ContainsByPredicate([](const FString& Note)
		{
			return Note.StartsWith(TEXT("Kept your earlier settings for this "));
		}));

		const FMantleVehicleBuildResult Switched = FMantleVehicleBuilder::Build(Copy, TEXT("pickup"));
		TestTrue(TEXT("building another type is ok"), Switched.bOk);
		if (TestNotNull(TEXT("the switched build gives the vehicle"), Switched.Source))
		{
			TestEqual(TEXT("the vehicle remembers its new type"), Switched.Source->Preset, FName(TEXT("pickup")));
			TestEqual(TEXT("the pickup's mass replaces the tuned one"), Switched.Source->Tuning.Mass, 2000.f);
			if (TestTrue(TEXT("the pickup has its own fit"), Switched.Fit.IsSet()))
			{
				TestEqual(TEXT("a different type recomputes the Size"), Switched.Source->FitScale, static_cast<float>(Switched.Fit->Scale));
			}
		}
		for (const FString& Problem : Again.Problems)
		{
			AddError(Problem);
		}
		for (const FString& Problem : Switched.Problems)
		{
			AddError(Problem);
		}
	}

	MantleVehicleBuilderTestPrivate::Discard(Built.Source);
	MantleVehicleScratchBody::Discard(Copy);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleBuilderApplyTest,
	"Mantle.Vehicle.Builder.Apply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleBuilderApplyTest::RunTest(const FString& Parameters)
{
	// A preset whose defaults hold a setting this version does not know is refused by ApplyDefaults.
	FMantleVehiclePreset Broken;
	Broken.Id = TEXT("broken");
	Broken.Name = TEXT("Broken");
	Broken.Defaults = MakeShared<FJsonObject>();
	Broken.Defaults->SetNumberField(TEXT("mass"), 1000.0);
	Broken.Defaults->SetNumberField(TEXT("warpDrive"), 5.0);

	// A refused apply leaves the vehicle exactly as it was: its old type (or none) and its old tuning, so a rerun still applies defaults.
	UMantleVehicleSource* Source = NewObject<UMantleVehicleSource>(GetTransientPackage());
	Source->Tuning.Mass = 1234.f;
	{
		FMantleVehicleBuildResult Out;
		TestFalse(TEXT("a refused preset fails the step"), MantleVehicleBuilderPrivate::ApplyChosenPreset(Broken, *Source, Out));
		TestTrue(TEXT("the refusal is a problem"), Out.Problems.Num() > 0);
		TestEqual(TEXT("a refused preset is not remembered"), Source->Preset, FName());
		TestEqual(TEXT("a refused preset leaves the tuning"), Source->Tuning.Mass, 1234.f);
	}

	// A vehicle converted before types existed, with its own tuning, is told its settings were replaced.
	const FMantleVehiclePreset* Sedan = FMantleVehiclePresets::Find(TEXT("sedan"));
	if (!TestNotNull(TEXT("the sedan preset loads"), Sedan))
	{
		return false;
	}
	{
		FMantleVehicleBuildResult Out;
		TestTrue(TEXT("the sedan applies to a tuned vehicle with no type"), MantleVehicleBuilderPrivate::ApplyChosenPreset(*Sedan, *Source, Out));
		TestEqual(TEXT("the type is remembered"), Source->Preset, FName(TEXT("sedan")));
		TestEqual(TEXT("the sedan's mass replaces the old one"), Source->Tuning.Mass, 1500.f);
		TestTrue(TEXT("the replaced settings are mentioned"), Out.Notes.ContainsByPredicate([](const FString& Note)
		{
			return Note.StartsWith(TEXT("This vehicle had its own settings from before")) && Note.Contains(TEXT("Undo (Ctrl+Z)"));
		}));
	}

	// A vehicle still on the defaults has nothing to lose, so no such note.
	UMantleVehicleSource* Plain = NewObject<UMantleVehicleSource>(GetTransientPackage());
	{
		FMantleVehicleBuildResult Out;
		TestTrue(TEXT("the sedan applies to an untouched vehicle"), MantleVehicleBuilderPrivate::ApplyChosenPreset(*Sedan, *Plain, Out));
		TestFalse(TEXT("nothing was replaced, so nothing is said"), Out.Notes.ContainsByPredicate([](const FString& Note)
		{
			return Note.StartsWith(TEXT("This vehicle had its own settings from before"));
		}));
	}
	return true;
}

#endif
