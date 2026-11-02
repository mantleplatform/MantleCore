// Copyright Joshua Eger. All Rights Reserved.
#include "MantleSkinnedMesh.h"
#include "MantleVehicleBuilder.h"
#include "MantleVehicleShapeKind.h"
#include "MantleVehicleSource.h"
#include "MantleBuilderKind.h"
#include "MantleVehicleScratchBody.h"

#include "Engine/SkeletalMesh.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS

// The window's picture of a model on its vehicle's frame (FMantleVehicleBuilder::MakeModelPreview) is the vehicle BuildFromModel builds: the same wheel spots and the same
// body. The Fab car, the bike and a plane are checked the same way by the Mantle.Vehicle.PreviewCheck command in the vehicles project.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleModelPreviewMatchesBuildTest,
	"Mantle.Vehicle.ModelPreview.MatchesTheBuild",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleModelPreviewMatchesBuildTest::RunTest(const FString& Parameters)
{
	FString Error;
	const TStrongObjectPtr<USkeletalMesh> Model(MantleVehicleScratchBody::Make(TEXT("/Temp/MantleModelPreviewTest"), TEXT("PreviewModel"), Error));
	if (!TestTrue(TEXT("the sample model is made"), Model.IsValid()))
	{
		AddError(Error);
		return false;
	}
	for (const TCHAR* Type : { TEXT("sedan"), TEXT("truck"), TEXT("motorcycle"), TEXT("plane") })
	{
		FMantleVehicleModelOptions Options;
		Options.Folder = TEXT("/Temp/MantleModelPreviewTest");
		Options.Name = TEXT("PreviewBuilt");
		Options.bOverwrite = true;
		FMantleVehicleModelPreview Preview;
		const bool bPreview = FMantleVehicleBuilder::MakeModelPreview(Model.Get(), Type, Options, Preview);
		for (const FString& Problem : Preview.Problems)
		{
			AddError(FString::Printf(TEXT("%s: %s"), Type, *Problem));
		}
		if (!TestTrue(FString::Printf(TEXT("%s: the picture is made"), Type), bPreview && Preview.Mesh.IsValid()))
		{
			continue;
		}
		TestTrue(FString::Printf(TEXT("%s: the picture has dots for the wheels"), Type), Preview.WheelDots.Num() >= 2);
		const FMantleVehicleBuildResult Built = FMantleVehicleBuilder::BuildFromModel(Model.Get(), Type, Options);
		FString Why;
		if (!FMantleVehicleBuilder::PreviewMatchesBuild(Preview, Built, Why))
		{
			AddError(FString::Printf(TEXT("%s: the picture and the build differ: %s"), Type, *Why));
		}
		TestTrue(FString::Printf(TEXT("%s: the picture lives in a throwaway package"), Type), Preview.Mesh->GetOutermost()->GetName().StartsWith(TEXT("/Temp")));
	}

	FMantleVehicleShapeKind Kind;
	FMantleVehicleModelPreview Preview;
	FMantleVehicleModelOptions Options;
	if (FMantleVehicleBuilder::MakeModelPreview(Model.Get(), TEXT("sedan"), Options, Preview))
	{
		const int32 Before = Kind.Overlay(TArray<FMantleBuilderPoint>()).Markers.Num();
		Kind.SetWheelDots(Preview.WheelDots);
		TestEqual(TEXT("the viewport draws a dot for each wheel"), Kind.Overlay(TArray<FMantleBuilderPoint>()).Markers.Num(), Before + Preview.WheelDots.Num());
	}
	MantleVehicleScratchBody::Discard(Model.Get());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
