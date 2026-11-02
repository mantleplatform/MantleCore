// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleBuilder.h"
#include "MantleVehicleScratchBody.h"
#include "MantleVehicleSource.h"
#include "SMantleVehicleBuilder.h"

#include "AssetRegistry/AssetData.h"
#include "Engine/SkeletalMesh.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// The Build a vehicle window's new flow: six steps with the type first and your own model always second, the picture of that model on its frame (never a stand-in
// vehicle), the check that the model fits the type with its Switch button, and the paint list. The state is driven the way the buttons do it. Named, not anonymous:
// MantleEditor builds as a unity build.
struct FMantleVehicleBuilderFlowAccess
{
	static TArray<FMantleBuilderStep> Steps(SMantleVehicleBuilder& Panel) { return Panel.MakeSteps(); }
	static void PickType(SMantleVehicleBuilder& Panel, const TCHAR* Id, const TCHAR* Name) { Panel.PickPreset(Id, FText::FromString(Name)); }
	static void SetMesh(SMantleVehicleBuilder& Panel, UObject* Model) { Panel.SetMesh(FAssetData(Model)); }
	static bool PreviewMade(const SMantleVehicleBuilder& Panel) { return Panel.Preview.bOk && Panel.Preview.Mesh.IsValid(); }
	static const FMantleVehicleModelPreview& Preview(const SMantleVehicleBuilder& Panel) { return Panel.Preview; }
	static const FMantleVehicleTypeMatch& Match(const SMantleVehicleBuilder& Panel) { return Panel.TypeMatch; }
	static void Switch(SMantleVehicleBuilder& Panel) { Panel.OnSwitchType(); }
	static const FString& Type(const SMantleVehicleBuilder& Panel) { return Panel.SelectedPresetId; }
	static int32 PaintSections(const SMantleVehicleBuilder& Panel) { return Panel.PaintChoices.Num(); }
	static FString PaintName(const SMantleVehicleBuilder& Panel, const int32 Index) { return Panel.PaintChoices[Index].Section; }
	static bool PaintOn(const SMantleVehicleBuilder& Panel, const int32 Index) { return Panel.PaintChoices[Index].bOn; }
	static void Tick(SMantleVehicleBuilder& Panel, const int32 Index, const bool bOn) { Panel.SetPaintSection(Index, bOn); }
	static void Apply(SMantleVehicleBuilder& Panel, UMantleVehicleSource& Source) { Panel.ApplyChoices(Source); }
	static bool CanBuild(const SMantleVehicleBuilder& Panel) { return Panel.CanBuild(); }
	static TSharedRef<SWidget> Content(SMantleVehicleBuilder& Panel, const int32 Step) { return Panel.MakeSteps()[Step].MakeContent(); }
};

namespace MantleVehicleBuilderFlowTestPrivate
{
	/** Every word of a step a creator reads. */
	FString Words(const FMantleBuilderStep& Step)
	{
		return Step.Title.ToString() + TEXT(" ") + Step.Help.ToString();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleBuilderFlowStepsTest,
	"Mantle.Vehicle.Builder.SixStepsTypeFirstThenYourModel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleBuilderFlowStepsTest::RunTest(const FString& Parameters)
{
	using Access = FMantleVehicleBuilderFlowAccess;
	if (!FSlateApplication::IsInitialized())
	{
		AddWarning(TEXT("SKIPPED, not passed: there is no Slate application in this runner, so the window cannot be made."));
		return true;
	}
	const TSharedRef<SMantleVehicleBuilder> Panel = SNew(SMantleVehicleBuilder);
	const TArray<FMantleBuilderStep> Steps = Access::Steps(*Panel);

	// The order: the type, then the model, the fit, seats and paint, the shared Thumbnail, and the build.
	if (!TestEqual(TEXT("six steps"), Steps.Num(), 6))
	{
		return false;
	}
	const TCHAR* const Expect[] = { TEXT("kind of vehicle"), TEXT("model"), TEXT("fit"), TEXT("Seats and paint"), TEXT("humbnail"), TEXT("Build") };
	for (int32 Index = 0; Index < 6; ++Index)
	{
		TestTrue(*FString::Printf(TEXT("step %d is about \"%s\" (it is \"%s\")"), Index + 1, Expect[Index], *Steps[Index].Title.ToString()), Steps[Index].Title.ToString().Contains(Expect[Index]));
	}

	// No stand-in vehicle in anything a creator reads, and no Shape or Drive step.
	for (const FMantleBuilderStep& Step : Steps)
	{
		const FString Words = MantleVehicleBuilderFlowTestPrivate::Words(Step);
		TestFalse(*FString::Printf(TEXT("\"%s\" has no gray vehicle in it"), *Step.Title.ToString()), Words.Contains(TEXT("gray"), ESearchCase::IgnoreCase) || Words.Contains(TEXT("grey"), ESearchCase::IgnoreCase));
		TestFalse(*FString::Printf(TEXT("\"%s\" is not the old Shape or Drive step"), *Step.Title.ToString()),
			Step.Title.ToString().StartsWith(TEXT("Shape")) || Step.Title.ToString().StartsWith(TEXT("Drive")));
	}

	// The gates: a type first, then a model, and the last step needs a build.
	FText WhyNot;
	TestFalse(TEXT("step 1 stops until a type is picked"), Steps[0].CanContinue(WhyNot));
	TestFalse(TEXT("and says why"), WhyNot.IsEmpty());
	Access::PickType(*Panel, TEXT("sedan"), TEXT("Sedan"));
	TestTrue(TEXT("step 1 goes on once a type is picked"), Steps[0].CanContinue(WhyNot));
	WhyNot = FText::GetEmpty();
	TestFalse(TEXT("step 2 stops until there is a model: a vehicle can't be made without one"), Steps[1].CanContinue(WhyNot));
	TestTrue(*FString::Printf(TEXT("and says so in plain words (%s)"), *WhyNot.ToString()), WhyNot.ToString().Contains(TEXT("model")));
	TestFalse(TEXT("the build button is off without a model"), Access::CanBuild(*Panel));
	WhyNot = FText::GetEmpty();
	TestFalse(TEXT("the last step needs the vehicle built"), Steps[5].CanContinue(WhyNot));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleBuilderFlowPictureTest,
	"Mantle.Vehicle.Builder.PictureIsYourModelOnItsFrame",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleBuilderFlowPictureTest::RunTest(const FString& Parameters)
{
	using Access = FMantleVehicleBuilderFlowAccess;
	if (!FSlateApplication::IsInitialized())
	{
		AddWarning(TEXT("SKIPPED, not passed: there is no Slate application in this runner, so the window cannot be made."));
		return true;
	}
	FString Error;
	USkeletalMesh* Model = MantleVehicleScratchBody::Make(TEXT("/Temp/MantleFlowTest"), TEXT("FlowModel"), Error);
	if (!TestNotNull(*FString::Printf(TEXT("the sample model is made (%s)"), *Error), Model))
	{
		return false;
	}
	ON_SCOPE_EXIT { MantleVehicleScratchBody::Discard(Model); };

	const TSharedRef<SMantleVehicleBuilder> Panel = SNew(SMantleVehicleBuilder);
	TestFalse(TEXT("nothing is drawn before anything is picked"), Access::PreviewMade(*Panel));
	Access::PickType(*Panel, TEXT("sedan"), TEXT("Sedan"));
	TestFalse(TEXT("picking a type draws no vehicle: the picture waits for the model"), Access::PreviewMade(*Panel));

	// Every step's page can be made, with and without a model.
	for (int32 Step = 0; Step < 6; ++Step)
	{
		Access::Content(*Panel, Step);
	}

	Access::SetMesh(*Panel, Model);
	if (!TestTrue(TEXT("with a model the picture is the model on its frame"), Access::PreviewMade(*Panel)))
	{
		return false;
	}
	const FMantleVehicleModelPreview& Preview = Access::Preview(*Panel);
	TestTrue(TEXT("it is in a throwaway package, never in the project"), Preview.Mesh->GetOutermost()->GetName().StartsWith(TEXT("/Temp")));
	TestTrue(*FString::Printf(TEXT("it has a dot on each wheel (%d)"), Preview.WheelDots.Num()), Preview.WheelDots.Num() >= 4);
	TestTrue(TEXT("a sedan model picked as a sedan fits, so there is no message"), !Access::Match(*Panel).Suggests());

	// The same pages again, with the picture and the paint list there.
	for (int32 Step = 0; Step < 6; ++Step)
	{
		Access::Content(*Panel, Step);
	}
	TestTrue(TEXT("a model gets a build button"), Access::CanBuild(*Panel));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleBuilderFlowMismatchTest,
	"Mantle.Vehicle.Builder.WrongTypeIsSaidWithASwitchButton",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleBuilderFlowMismatchTest::RunTest(const FString& Parameters)
{
	using Access = FMantleVehicleBuilderFlowAccess;
	if (!FSlateApplication::IsInitialized())
	{
		AddWarning(TEXT("SKIPPED, not passed: there is no Slate application in this runner, so the window cannot be made."));
		return true;
	}
	FString Error;
	USkeletalMesh* Model = MantleVehicleScratchBody::Make(TEXT("/Temp/MantleFlowTest"), TEXT("FlowCar"), Error);
	if (!TestNotNull(*FString::Printf(TEXT("the sample car is made (%s)"), *Error), Model))
	{
		return false;
	}
	ON_SCOPE_EXIT { MantleVehicleScratchBody::Discard(Model); };

	// A four-wheeled car picked as a motorcycle: the window says so and offers the type the car looks like.
	const TSharedRef<SMantleVehicleBuilder> Panel = SNew(SMantleVehicleBuilder);
	Access::PickType(*Panel, TEXT("motorcycle"), TEXT("Motorcycle"));
	Access::SetMesh(*Panel, Model);
	const FMantleVehicleTypeMatch& Match = Access::Match(*Panel);
	if (!TestTrue(*FString::Printf(TEXT("the car is not a motorcycle (%s)"), *Match.Message), Match.Suggests()))
	{
		return false;
	}
	TestTrue(TEXT("the message is in plain words"), !Match.Message.IsEmpty() && !Match.ButtonText.IsEmpty() && Match.ButtonText.StartsWith(TEXT("Switch to ")));

	// The button changes the type, and the model is read again for it: it fits now.
	const FString Suggested = Match.SuggestedType;
	Access::Switch(*Panel);
	TestEqual(TEXT("Switch picks the suggested type"), Access::Type(*Panel), Suggested);
	TestFalse(TEXT("and the message is gone"), Access::Match(*Panel).Suggests());
	TestTrue(TEXT("the picture is of the car on its new frame"), Access::PreviewMade(*Panel));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleBuilderFlowPaintTest,
	"Mantle.Vehicle.Builder.PaintSectionsCanBeTickedAndUnticked",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleBuilderFlowPaintTest::RunTest(const FString& Parameters)
{
	using Access = FMantleVehicleBuilderFlowAccess;
	if (!FSlateApplication::IsInitialized())
	{
		AddWarning(TEXT("SKIPPED, not passed: there is no Slate application in this runner, so the window cannot be made."));
		return true;
	}
	FString Error;
	USkeletalMesh* Model = MantleVehicleScratchBody::Make(TEXT("/Temp/MantleFlowTest"), TEXT("FlowPaint"), Error);
	if (!TestNotNull(*FString::Printf(TEXT("the sample car is made (%s)"), *Error), Model))
	{
		return false;
	}
	ON_SCOPE_EXIT { MantleVehicleScratchBody::Discard(Model); };

	const TSharedRef<SMantleVehicleBuilder> Panel = SNew(SMantleVehicleBuilder);
	TestEqual(TEXT("no model, no paint sections to list"), Access::PaintSections(*Panel), 0);
	Access::PickType(*Panel, TEXT("sedan"), TEXT("Sedan"));
	Access::SetMesh(*Panel, Model);
	if (!TestTrue(TEXT("the model's sections are listed"), Access::PaintSections(*Panel) > 0))
	{
		return false;
	}
	const FString Section = Access::PaintName(*Panel, 0);

	// Ticking a section is kept across a redraw of the picture, and a vehicle built after it carries exactly the ticked sections.
	const bool bWas = Access::PaintOn(*Panel, 0);
	Access::Tick(*Panel, 0, !bWas);
	TestEqual(TEXT("the tick is kept"), Access::PaintOn(*Panel, 0), !bWas);
	UMantleVehicleSource* Source = NewObject<UMantleVehicleSource>(GetTransientPackage(), NAME_None, RF_Transient);
	Source->Paint.Sections.Add(TEXT("SomethingElse"));
	Access::Apply(*Panel, *Source);
	TestEqual(TEXT("the vehicle has the ticked sections and no other"), Source->Paint.Sections.Contains(Section), !bWas);
	TestFalse(TEXT("what Mantle guessed before is replaced"), Source->Paint.Sections.Contains(TEXT("SomethingElse")));

	Access::Tick(*Panel, 0, bWas);
	Access::Apply(*Panel, *Source);
	TestEqual(TEXT("unticking it takes it out again"), Source->Paint.Sections.Contains(Section), bWas);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
