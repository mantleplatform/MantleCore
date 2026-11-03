// Copyright Joshua Eger. All Rights Reserved.
#include "SMantleVehicleBuilder.h"
#include "MantleSkinnedMesh.h"
#include "MantleVehicleConverter.h"
#include "MantleVehicleLiveryTools.h"
#include "MantleVehicleSource.h"

#include "Dom/JsonObject.h"
#include "Engine/Texture2D.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// The Liveries step of the Build a vehicle window: its images and names reach the vehicle, the guide draws the painted parts, and the document carries the names.

// Named, not anonymous: MantleEditor builds as a unity build.
struct FMantleVehicleBuilderLiveryAccess
{
	static void Add(SMantleVehicleBuilder& Panel, UTexture2D* Texture) { Panel.AddLivery(Texture); }
	static void Remove(SMantleVehicleBuilder& Panel, const int32 Index) { Panel.RemoveLivery(Index); }
	static void Name(SMantleVehicleBuilder& Panel, const int32 Index, const FString& Name) { Panel.SetLiveryName(Index, Name); }
	static void Start(SMantleVehicleBuilder& Panel, const int32 Index) { Panel.SetStartLivery(Index); }
	static void Apply(SMantleVehicleBuilder& Panel, UMantleVehicleSource& Source) { Panel.ApplyLiveries(Source); }
	static int32 Count(const SMantleVehicleBuilder& Panel) { return Panel.LiveryChoices.Num(); }
	static TSharedRef<SWidget> Content(SMantleVehicleBuilder& Panel) { return Panel.MakeLiveriesStep(); }
};

namespace MantleVehicleLiveryStepTestPrivate
{
	UTexture2D* MakeImage()
	{
		UTexture2D* Texture = UTexture2D::CreateTransient(4, 4);
		Texture->AddToRoot();
		return Texture;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleLiveryStepWindowTest,
	"Mantle.Vehicle.Builder.LiveriesStepKeepsImagesNamesAndStart",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleLiveryStepWindowTest::RunTest(const FString& Parameters)
{
	using Access = FMantleVehicleBuilderLiveryAccess;
	if (!FSlateApplication::IsInitialized())
	{
		AddWarning(TEXT("SKIPPED, not passed: there is no Slate application in this runner, so the window cannot be made."));
		return true;
	}
	TArray<UTexture2D*> Images;
	for (int32 Index = 0; Index < 9; ++Index)
	{
		Images.Add(MantleVehicleLiveryStepTestPrivate::MakeImage());
	}
	ON_SCOPE_EXIT { for (UTexture2D* Image : Images) { Image->RemoveFromRoot(); } };

	const TSharedRef<SMantleVehicleBuilder> Panel = SNew(SMantleVehicleBuilder);
	Access::Content(*Panel);
	Access::Add(*Panel, Images[0]);
	Access::Add(*Panel, Images[0]);
	TestEqual(TEXT("the same image twice is added once"), Access::Count(*Panel), 1);
	Access::Add(*Panel, Images[1]);
	Access::Name(*Panel, 1, TEXT("  Racing stripe  "));

	UMantleVehicleSource* Source = NewObject<UMantleVehicleSource>(GetTransientPackage(), NAME_None, RF_Transient);
	Access::Apply(*Panel, *Source);
	TestEqual(TEXT("both images reach the vehicle"), Source->Paint.Liveries.Num(), 2);
	TestEqual(TEXT("the first image added is the one the car starts in"), Source->Paint.DefaultLivery, 0);

	Access::Start(*Panel, 1);
	Access::Apply(*Panel, *Source);
	TestEqual(TEXT("the start can be moved"), Source->Paint.DefaultLivery, 1);
	Access::Remove(*Panel, 0);
	Access::Apply(*Panel, *Source);
	TestEqual(TEXT("removing an earlier one keeps the start on the same image"), Source->Paint.DefaultLivery, 0);
	Access::Remove(*Panel, 0);
	Access::Apply(*Panel, *Source);
	TestEqual(TEXT("removing the one it starts in leaves the plain paint"), Source->Paint.DefaultLivery, static_cast<int32>(INDEX_NONE));

	for (UTexture2D* Image : Images)
	{
		Access::Add(*Panel, Image);
	}
	TestEqual(TEXT("at most eight liveries"), Access::Count(*Panel), 8);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleLiveryGuideTest,
	"Mantle.Vehicle.Livery.UvGuideOutlinesThePaint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleLiveryGuideTest::RunTest(const FString& Parameters)
{
	// Two triangles in two sections: one in the left half of UV space (material 0), one in the right (material 1).
	FMantleSkinnedMesh Body;
	FMantleSkinnedMeshLOD& LOD = Body.LODs.AddDefaulted_GetRef();
	LOD.Positions = { FVector3f(0, 0, 0), FVector3f(1, 0, 0), FVector3f(0, 1, 0), FVector3f(0, 0, 1), FVector3f(1, 0, 1), FVector3f(0, 1, 1) };
	LOD.UVs = { FVector2f(0.05f, 0.05f), FVector2f(0.45f, 0.05f), FVector2f(0.05f, 0.45f), FVector2f(0.55f, 0.05f), FVector2f(0.95f, 0.05f), FVector2f(0.55f, 0.45f) };
	LOD.Indices = { 0, 1, 2, 3, 4, 5 };
	FMantleSkinnedMeshSection& Left = LOD.Sections.AddDefaulted_GetRef();
	Left.MaterialIndex = 0;
	Left.FirstIndex = 0;
	Left.IndexCount = 3;
	FMantleSkinnedMeshSection& Right = LOD.Sections.AddDefaulted_GetRef();
	Right.MaterialIndex = 1;
	Right.FirstIndex = 3;
	Right.IndexCount = 3;

	constexpr int32 Size = 200;
	const TArray<FColor> Guide = FMantleVehicleLiveryTools::DrawUvGuide(Body, TSet<int32>({ 0 }), Size);
	if (!TestEqual(TEXT("a square picture"), Guide.Num(), Size * Size))
	{
		return false;
	}
	auto At = [&Guide](const int32 X, const int32 Y) { return Guide[Y * Size + X]; };
	const FColor Filled = At(30, 30);       // inside the painted triangle
	const FColor Unpainted = At(130, 30);   // inside the unpainted one
	const FColor Background = At(150, 150); // inside neither
	TestNotEqual(TEXT("the painted triangle is filled"), Filled, Background);
	TestEqual(TEXT("the unpainted triangle is not filled"), Unpainted, Background);
	TestTrue(TEXT("the painted triangle is brighter than the unpainted one"), Filled.R + Filled.G + Filled.B > Unpainted.R + Unpainted.G + Unpainted.B);

	TestTrue(TEXT("no UVs, no guide"), FMantleVehicleLiveryTools::DrawUvGuide(FMantleSkinnedMesh(), TSet<int32>(), Size).IsEmpty());

	TestEqual(TEXT("an unnamed livery is numbered from 1"), FMantleVehicleLiveryTools::DisplayName(TEXT("  "), 2), FString(TEXT("Livery 3")));
	TestEqual(TEXT("a name is shown as given"), FMantleVehicleLiveryTools::DisplayName(TEXT("Police"), 0), FString(TEXT("Police")));
	TestEqual(TEXT("a name from an image's file name"), FMantleVehicleLiveryTools::NameFromImage(TEXT("Racing_stripe-01")), FString(TEXT("Racing stripe 01")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
