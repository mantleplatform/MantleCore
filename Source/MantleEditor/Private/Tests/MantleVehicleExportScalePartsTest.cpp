// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceAssetConverter.h"
#include "MantlePlaceAssets.h"
#include "MantlePlaceValidator.h"
#include "MantleVehicleConverter.h"
#include "MantleVehicleScratchBody.h"
#include "MantleVehicleSource.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

// A vehicle's Size reaches an uploaded part through the real upload path: FMantlePlaceAssetConverter::ConvertVehicle reads the
// parts from the export view, and FMantleVehicleConverter::WithParts writes their transforms into the document that uploads. The
// existing Mantle.Vehicle.Fit.ExportScale only checks the view object, which a converter reading the original asset would not notice.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVehicleExportScalePartsTestPrivate
{
	const FVector PartLocation(4.5, 5.5, 6.5);
	const FVector PartScale(1.25, 1.5, 2.0);

	/** The engine's cube, copied into a scratch package so it is a mesh of "the creator's project" and not a Mantle library one: always in the editor, so the test needs no project content. */
	const TCHAR* PartMeshPath = TEXT("/Engine/BasicShapes/Cube.Cube");
	const TCHAR* PartPackagePath = TEXT("/Temp/MantleVehicleExportScalePartsTest");

	/** A vehicle of Body (the sedan chassis's grey mesh) at the given Size with one part of its own at a known place and size; null, with OutProblem, when it cannot be made. */
	UMantleVehicleSource* MakeSource(USkeletalMesh* Body, const float Size, FString& OutProblem)
	{
		UStaticMesh* PartMesh = FindObject<UStaticMesh>(nullptr, *(FString(PartPackagePath) + TEXT(".ScalePartsCube")));
		if (!PartMesh)
		{
			const UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, PartMeshPath);
			PartMesh = Cube ? DuplicateObject<UStaticMesh>(Cube, CreatePackage(PartPackagePath), TEXT("ScalePartsCube")) : nullptr;
		}
		if (!Body || !PartMesh)
		{
			OutProblem = FString::Printf(TEXT("%s could not be made"), !Body ? TEXT("the test body") : PartMeshPath);
			return nullptr;
		}
		UMantleVehicleSource* Source = NewObject<UMantleVehicleSource>(GetTransientPackage());
		// Convert finds the wheels and gives the tuning its defaults, with nothing attached to the body.
		const FMantleVehicleConversion Conversion = FMantleVehicleConverter::Convert(Body, *Source, FMantleVehicleSurroundings());
		if (!Conversion.bSuccess || Source->Wheels.IsEmpty())
		{
			OutProblem = FString::Join(Conversion.Problems, TEXT(" "));
			return nullptr;
		}
		// Give each wheel its size, as a creator would for a model whose tyres are separate meshes, so the upload check has
		// nothing to measure.
		for (FMantleVehicleSourceWheel& Wheel : Source->Wheels)
		{
			Wheel.Radius = 39.f;
			Wheel.Width = 35.f;
		}
		Source->FitScale = Size;
		Source->Parts.Reset();
		FMantleVehicleSourcePart& Part = Source->Parts.AddDefaulted_GetRef();
		Part.Mesh = PartMesh;
		Part.Bone = Source->Wheels[0].Bone;
		Part.Transform = FTransform(FRotator::ZeroRotator, PartLocation, PartScale);
		return Source;
	}

	/** The number at a slash-separated path of a document ("parts/0/location/1": object keys, and array indices as digits). */
	bool NumberAt(const TSharedRef<FJsonObject>& Doc, const FString& Path, double& Out)
	{
		TArray<FString> Steps;
		Path.ParseIntoArray(Steps, TEXT("/"));
		TSharedPtr<FJsonValue> Value = MakeShared<FJsonValueObject>(Doc);
		for (const FString& Step : Steps)
		{
			if (!Value.IsValid())
			{
				return false;
			}
			if (Value->Type == EJson::Object)
			{
				Value = Value->AsObject()->TryGetField(Step);
			}
			else if (Value->Type == EJson::Array && Step.IsNumeric())
			{
				const TArray<TSharedPtr<FJsonValue>>& Items = Value->AsArray();
				const int32 Index = FCString::Atoi(*Step);
				Value = Items.IsValidIndex(Index) ? Items[Index] : nullptr;
			}
			else
			{
				return false;
			}
		}
		return Value.IsValid() && Value->TryGetNumber(Out);
	}

	/**
	 * Converts the vehicle as a publish does and returns its one part's transform as ConvertVehicle left it, and the document its parts are
	 * written into the way ConvertVehicle writes them (WithParts over the converted parts). False, with OutProblem, when it does not convert.
	 */
	bool ConvertPart(UMantleVehicleSource* Source, FTransform& OutTransform, TSharedPtr<FJsonObject>& OutDocument, FString& OutProblem)
	{
		FMantlePlaceAssetConverter Converter;
		FMantlePlaceAssetSet Set;
		const bool bConverted = Converter.ConvertVehicle(Source, Set);
		if (!bConverted || Set.Vehicles.Num() != 1 || Set.Vehicles[0].Parts.Num() != 1)
		{
			TArray<FString> Messages;
			for (const FMantleValidationIssue& Issue : Set.Issues)
			{
				Messages.Add(Issue.Problem);
			}
			OutProblem = FString::Printf(TEXT("converted %s, %d vehicles, issues: %s"), bConverted ? TEXT("yes") : TEXT("no"), Set.Vehicles.Num(),
				*FString::Join(Messages, TEXT(" ")));
			return false;
		}
		const FMantleConvertedVehicle& Converted = Set.Vehicles[0];
		OutTransform = Converted.Parts[0].Transform;

		TArray<FMantleVehicleConverter::FPartIds> Ids;
		for (const FMantleConvertedVehicle::FPart& Part : Converted.Parts)
		{
			FMantleVehicleConverter::FPartIds& Entry = Ids.AddDefaulted_GetRef();
			Entry.Mesh = FMantlePlaceAssets::ProvisionalId(Part.MeshKey);
			Entry.Bone = Part.Bone;
			Entry.Transform = Part.Transform;
		}
		OutDocument = FMantleVehicleConverter::WithParts(Converted.Document.ToSharedRef(), Ids);
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleExportScalePartsTest,
	"Mantle.Vehicle.Fit.ExportScaleParts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleExportScalePartsTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleExportScalePartsTestPrivate;

	// The body is the sedan chassis's own grey mesh, so the test needs no project content and cannot silently skip. Anything that goes wrong here is a failure.
	FString Problem;
	USkeletalMesh* Body = MantleVehicleScratchBody::Make(TEXT("/Temp/MantleVehicleExportScalePartsTest"), TEXT("ScalePartsBody"), Problem);
	if (!TestNotNull(FString::Printf(TEXT("the test body (%s)"), *Problem), Body))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		MantleVehicleScratchBody::Discard(Body);
		if (UStaticMesh* PartMesh = FindObject<UStaticMesh>(nullptr, *(FString(PartPackagePath) + TEXT(".ScalePartsCube"))))
		{
			UPackage* PartPackage = PartMesh->GetOutermost();
			PartMesh->ClearFlags(RF_Public | RF_Standalone);
			PartMesh->MarkAsGarbage();
			PartPackage->ClearDirtyFlag();
			PartPackage->MarkAsGarbage();
		}
	};
	const TStrongObjectPtr<UMantleVehicleSource> Plain(MakeSource(Body, 1.f, Problem));
	if (!TestTrue(FString::Printf(TEXT("the Size 1 vehicle is made (%s)"), *Problem), Plain.IsValid()))
	{
		return false;
	}
	const TStrongObjectPtr<UMantleVehicleSource> Sized(MakeSource(Body, 2.f, Problem));
	if (!TestTrue(TEXT("the Size 2 vehicle is made"), Sized.IsValid()))
	{
		return false;
	}

	FTransform OnePart;
	FTransform TwoPart;
	TSharedPtr<FJsonObject> OneDoc;
	TSharedPtr<FJsonObject> TwoDoc;
	if (!TestTrue(FString::Printf(TEXT("the vehicle at Size 1 converts for upload (%s)"), *Problem), ConvertPart(Plain.Get(), OnePart, OneDoc, Problem))
		|| !TestTrue(FString::Printf(TEXT("the vehicle at Size 2 converts for upload (%s)"), *Problem), ConvertPart(Sized.Get(), TwoPart, TwoDoc, Problem)))
	{
		return false;
	}

	// The part as ConvertVehicle hands it on: unchanged at Size 1, doubled in place and size at Size 2, and the asset's own part untouched.
	TestEqual(TEXT("at Size 1 the uploaded part keeps its location"), OnePart.GetLocation(), PartLocation);
	TestEqual(TEXT("at Size 1 the uploaded part keeps its scale"), OnePart.GetScale3D(), PartScale);
	TestEqual(TEXT("at Size 2 the uploaded part's location is doubled"), TwoPart.GetLocation(), PartLocation * 2.0);
	TestEqual(TEXT("at Size 2 the uploaded part's scale is doubled"), TwoPart.GetScale3D(), PartScale * 2.0);
	TestEqual(TEXT("the asset's own part keeps its location"), Sized->Parts[0].Transform.GetLocation(), PartLocation);
	TestEqual(TEXT("the asset's own part keeps its scale"), Sized->Parts[0].Transform.GetScale3D(), PartScale);

	// The same numbers as the document that uploads carries them.
	const TCHAR* const Axes[] = { TEXT("0"), TEXT("1"), TEXT("2") };
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		for (const TCHAR* Key : { TEXT("location"), TEXT("scale") })
		{
			const FString Path = FString::Printf(TEXT("parts/0/%s/%s"), Key, Axes[Axis]);
			const FVector& Base = FCString::Strcmp(Key, TEXT("location")) == 0 ? PartLocation : PartScale;
			double Before = 0.0;
			double After = 0.0;
			if (TestTrue(FString::Printf(TEXT("%s is written at both sizes"), *Path), NumberAt(OneDoc.ToSharedRef(), Path, Before) && NumberAt(TwoDoc.ToSharedRef(), Path, After)))
			{
				TestEqual(FString::Printf(TEXT("%s is unchanged at Size 1"), *Path), Before, Base[Axis], 0.001);
				TestEqual(FString::Printf(TEXT("%s doubles at Size 2"), *Path), After, 2.0 * Base[Axis], 0.001);
			}
		}
	}
	return true;
}

#endif
