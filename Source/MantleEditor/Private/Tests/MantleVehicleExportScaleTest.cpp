// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAssetCleanup.h"
#include "MantleVehicleConverter.h"
#include "MantleVehicleScratchBody.h"
#include "MantleVehicleSource.h"
#include "MantleChassisSpec.h"
#include "MantleVehicleChassisBuilder.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

// A vehicle's Size is applied once, in the export view: every length it measured from the creator's model is multiplied by
// it, and every tuning number a preset chose is left alone.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVehicleExportScaleTestPrivate
{
	/** A transient source with every geometric field and every tuning field set away from its default, at the given Size. */
	UMantleVehicleSource* MakeSource(const float Size)
	{
		UMantleVehicleSource* Source = NewObject<UMantleVehicleSource>(GetTransientPackage());
		Source->FitScale = Size;

		FMantleVehicleSourceWheel& Front = Source->Wheels.AddDefaulted_GetRef();
		Front.Bone = TEXT("Wheel_FL");
		Front.bSteering = true;
		Front.Radius = 31.5f;
		Front.Width = 21.f;
		Front.TireTransform = FTransform(FRotator::ZeroRotator, FVector(1.5, -2.5, 3.5), FVector(1.0, -1.0, 1.0));
		FMantleVehicleSourceWheel& Back = Source->Wheels.AddDefaulted_GetRef();
		Back.Bone = TEXT("Wheel_BL");
		Back.Radius = 32.f;

		FMantleVehicleSourceTuning& T = Source->Tuning;
		T.Mass = 1750.f;
		T.MaxSpeed = 5200.f;
		T.bOverrideCenterOfMass = true;
		T.CenterOfMass = FVector(10.5, -4.5, 22.5);
		T.MaxTorque = 450.f;
		T.GearRatios = { 3.5f, 2.2f, 1.5f };
		T.SuspensionMaxRaise = 12.f;
		T.SuspensionMaxDrop = 14.f;
		T.SpringRate = 275.f;
		T.TireFriction = 2.5f;
		T.BrakeTorque = 1600.f;
		T.CameraArmLength = 700.f;
		T.CameraHeight = 160.f;
		T.bSeatLocation = true;
		T.SeatLocation = FVector(-20.5, -35.5, 40.5);
		T.SeatWeaponArc.bEnabled = true;
		T.SeatWeaponArc.YawMin = -48.f;
		T.SeatWeaponArc.YawMax = 112.f;
		T.SeatWeaponArc.PitchMin = -12.f;
		T.SeatWeaponArc.PitchMax = 38.f;
		T.bSteeringWheel = true;
		T.SteeringWheelLocation = FVector(30.5, -35.5, 60.5);
		T.SteeringWheelRadius = 17.5f;
		T.bHasDriverDoor = true;
		T.DriverDoor.Bone = TEXT("Door_L");
		T.DriverDoor.Pivot = FVector(15.5, -80.5, 45.5);
		T.DriverDoor.Scale = 1.5f;
		T.DriverContacts.bPelvis = true;
		T.DriverContacts.Pelvis.Location = FVector(-24.5, -35.5, 8.5);
		FMantleVehicleSourceExit& Exit = T.DriverContacts.Exits.AddDefaulted_GetRef();
		Exit.Location = FVector(-20.5, -150.5, -30.5);
		Exit.Yaw = 90.f;
		T.bPassengerSeat = true;
		T.PassengerSeatLocation = FVector(-20.5, 35.5, 40.5);
		T.PassengerSeatWeaponArc.bEnabled = true;
		T.PassengerSeatWeaponArc.YawMin = -65.f;
		T.PassengerSeatWeaponArc.YawMax = 104.f;
		T.PassengerSeatWeaponArc.PitchMin = -18.f;
		T.PassengerSeatWeaponArc.PitchMax = 42.f;
		T.bTrunk = true;
		T.TrunkLid.Bone = TEXT("Trunk");
		T.TrunkLid.Pivot = FVector(-140.5, 0.5, 70.5);
		T.TrunkStand = FVector(-260.5, 0.5, -30.5);
		FMantleVehicleSourceBox& Box = T.Collision.AddDefaulted_GetRef();
		Box.Center = FVector(2.5, 0.5, 20.5);
		Box.Extent = FVector(220.5, 95.5, 45.5);

		// A part rides a bone and grows with the body; it has no mesh, which the view does not read.
		FMantleVehicleSourcePart& Part = Source->Parts.AddDefaulted_GetRef();
		Part.Bone = TEXT("Door_L");
		Part.Transform = FTransform(FRotator::ZeroRotator, FVector(4.5, 5.5, 6.5), FVector(1.0, 1.0, -1.0));
		return Source;
	}

	/** The number at a slash-separated path of a document ("seat/location/1": object keys, and array indices as digits). */
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

	/** A document as condensed JSON text, to compare two for being identical. */
	FString Text(const TSharedRef<FJsonObject>& Doc)
	{
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Doc, Writer);
		return Out;
	}

	bool ParseDocumentText(const FString& Json, FMantleVehicleDesc& Out, FString& OutError)
	{
		const FTCHARToUTF8 Utf8(*Json);
		return FMantleVehicleAssetReader::ParseDocument(
			TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()), Out, OutError);
	}

	TSharedPtr<FJsonObject> ParseObjectText(const FString& Json)
	{
		TSharedPtr<FJsonObject> Out;
		return FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Out) ? Out : nullptr;
	}

	TSharedRef<FJsonObject> SourceArcsOnCookedVehicle(const TSharedPtr<FJsonObject>& CookedBase,
		const TSharedRef<FJsonObject>& SourceDocument, FString& OutError)
	{
		OutError.Reset();
		const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		FJsonObject::Duplicate(CookedBase, Out);

		const TSharedPtr<FJsonObject>* SourceSeat = nullptr;
		const TSharedPtr<FJsonObject>* CookedSeat = nullptr;
		const TSharedPtr<FJsonObject>* SourcePassengerSeat = nullptr;
		const TSharedPtr<FJsonObject>* CookedPassengerSeat = nullptr;
		if (!SourceDocument->TryGetObjectField(TEXT("seat"), SourceSeat) || !SourceSeat || !SourceSeat->IsValid()
			|| !Out->TryGetObjectField(TEXT("seat"), CookedSeat) || !CookedSeat || !CookedSeat->IsValid()
			|| !SourceDocument->TryGetObjectField(TEXT("passengerSeat"), SourcePassengerSeat) || !SourcePassengerSeat || !SourcePassengerSeat->IsValid()
			|| !Out->TryGetObjectField(TEXT("passengerSeat"), CookedPassengerSeat) || !CookedPassengerSeat || !CookedPassengerSeat->IsValid())
		{
			OutError = TEXT("the source export or cooked vehicle is missing its driver or passenger seat");
			return Out;
		}

		auto OverlayArc = [](const TSharedPtr<FJsonObject>& SourceSeatObject, const TSharedPtr<FJsonObject>& CookedSeatObject)
		{
			if (SourceSeatObject->HasField(TEXT("weaponArc")))
			{
				CookedSeatObject->SetField(TEXT("weaponArc"), SourceSeatObject->TryGetField(TEXT("weaponArc")));
			}
			else
			{
				CookedSeatObject->RemoveField(TEXT("weaponArc"));
			}
		};
		OverlayArc(*SourceSeat, *CookedSeat);
		OverlayArc(*SourcePassengerSeat, *CookedPassengerSeat);
		return Out;
	}

	struct FVehicleSourcePackageCleanup
	{
		FString PackagePath;
		FString AssetName;
		FString Folder;

		~FVehicleSourcePackageCleanup()
		{
			UPackage* Package = PackagePath.IsEmpty() ? nullptr : FindPackage(nullptr, *PackagePath);
			if (Package)
			{
				if (UMantleVehicleSource* Asset = FindObject<UMantleVehicleSource>(Package, *AssetName))
				{
					FAssetRegistryModule::AssetDeleted(Asset);
				}
				FText Error;
				FMantleAssetCleanup::UnloadPackagesClean({ Package }, Error);
			}
			if (!Folder.IsEmpty())
			{
				IFileManager::Get().DeleteDirectory(*Folder, false, true);
			}
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleExportScaleTest,
	"Mantle.Vehicle.Fit.ExportScale",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleExportScaleTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleExportScaleTestPrivate;
	const TMap<FString, FString> NoMaterials;

	// A Size of 1 is the vehicle as modelled: the view is the vehicle itself, and its document is the one it always wrote.
	UMantleVehicleSource* Plain = MakeSource(1.f);
	TestTrue(TEXT("at Size 1 the export view is the vehicle itself"), FMantleVehicleConverter::MakeExportView(*Plain) == Plain);
	TestEqual(TEXT("at Size 1 the view's document is the vehicle's own"),
		Text(FMantleVehicleConverter::MakeDocument(*FMantleVehicleConverter::MakeExportView(*Plain), NoMaterials)),
		Text(FMantleVehicleConverter::MakeDocument(*Plain, NoMaterials)));

	// A Size of 2 doubles every length and leaves the tuning alone.
	UMantleVehicleSource* Sized = MakeSource(2.f);
	const TStrongObjectPtr<UMantleVehicleSource> View(const_cast<UMantleVehicleSource*>(FMantleVehicleConverter::MakeExportView(*Sized)));
	if (!TestTrue(TEXT("at Size 2 the export view is a copy"), View.IsValid() && View.Get() != Sized))
	{
		return false;
	}
	TestEqual(TEXT("the view is at Size 1, so it is not scaled twice"), View->FitScale, 1.f);
	TestEqual(TEXT("the asset keeps its Size"), Sized->FitScale, 2.f);
	TestEqual(TEXT("the asset keeps its own seat"), Sized->Tuning.SeatLocation, FVector(-20.5, -35.5, 40.5));
	TestEqual(TEXT("the view's seat is doubled"), View->Tuning.SeatLocation, FVector(-41.0, -71.0, 81.0));
	TestEqual(TEXT("the view's tire is moved out"), View->Wheels[0].TireTransform.GetTranslation(), FVector(3.0, -5.0, 7.0));
	TestEqual(TEXT("the view's tire is doubled and still mirrored"), View->Wheels[0].TireTransform.GetScale3D(), FVector(2.0, -2.0, 2.0));
	TestEqual(TEXT("the view's part is moved out"), View->Parts[0].Transform.GetTranslation(), FVector(9.0, 11.0, 13.0));
	TestEqual(TEXT("the view's part is doubled and still mirrored"), View->Parts[0].Transform.GetScale3D(), FVector(2.0, 2.0, -2.0));
	TestEqual(TEXT("the view's hinge axis keeps its direction"), View->Tuning.DriverDoor.Axis, Sized->Tuning.DriverDoor.Axis);

	const TSharedRef<FJsonObject> One = FMantleVehicleConverter::MakeDocument(*Plain, NoMaterials);
	const TSharedRef<FJsonObject> Two = FMantleVehicleConverter::MakeDocument(*Sized, NoMaterials);
	TestEqual(TEXT("the same document comes from the view"), Text(FMantleVehicleConverter::MakeDocument(*View, NoMaterials)), Text(Two));

	const TCHAR* const Lengths[] = {
		TEXT("wheels/0/radius"), TEXT("wheels/0/width"), TEXT("wheels/1/radius"),
		TEXT("centerOfMass/0"), TEXT("centerOfMass/1"), TEXT("centerOfMass/2"),
		TEXT("seat/location/0"), TEXT("seat/location/1"), TEXT("seat/location/2"),
		TEXT("seat/steeringWheel/location/0"), TEXT("seat/steeringWheel/location/2"), TEXT("seat/steeringWheel/radius"),
		TEXT("seat/door/pivot/0"), TEXT("seat/door/pivot/1"), TEXT("seat/door/pivot/2"),
		TEXT("seat/pelvis/location/0"), TEXT("seat/pelvis/location/2"),
		TEXT("seat/exits/0/location/0"), TEXT("seat/exits/0/location/1"), TEXT("seat/exits/0/location/2"),
		TEXT("passengerSeat/location/0"), TEXT("passengerSeat/location/1"), TEXT("passengerSeat/location/2"),
		TEXT("trunk/lid/pivot/0"), TEXT("trunk/lid/pivot/2"),
		TEXT("trunk/stand/0"), TEXT("trunk/stand/2"),
		TEXT("collision/0/center/0"), TEXT("collision/0/center/2"),
		TEXT("collision/0/extent/0"), TEXT("collision/0/extent/1"), TEXT("collision/0/extent/2") };
	for (const TCHAR* Path : Lengths)
	{
		double Before = 0.0;
		double After = 0.0;
		if (TestTrue(FString::Printf(TEXT("%s is written"), Path), NumberAt(One, Path, Before) && NumberAt(Two, Path, After)))
		{
			TestEqual(FString::Printf(TEXT("%s doubles"), Path), After, 2.0 * Before, 0.001);
		}
	}

	const TCHAR* const Tuning[] = {
		TEXT("mass"), TEXT("maxSpeed"), TEXT("engine/maxTorque"), TEXT("transmission/gearRatios/0"), TEXT("transmission/gearRatios/2"),
		TEXT("suspension/maxRaise"), TEXT("suspension/maxDrop"), TEXT("suspension/springRate"), TEXT("tires/friction"), TEXT("brakes/brakeTorque"),
		TEXT("camera/armLength"), TEXT("camera/height"), TEXT("seat/door/axis/2"), TEXT("seat/door/scale"), TEXT("seat/exits/0/yaw"),
		TEXT("seat/weaponArc/yawMin"), TEXT("seat/weaponArc/yawMax"), TEXT("seat/weaponArc/pitchMin"), TEXT("seat/weaponArc/pitchMax"),
		TEXT("passengerSeat/weaponArc/yawMin"), TEXT("passengerSeat/weaponArc/yawMax"),
		TEXT("passengerSeat/weaponArc/pitchMin"), TEXT("passengerSeat/weaponArc/pitchMax") };
	for (const TCHAR* Path : Tuning)
	{
		double Before = 0.0;
		double After = 0.0;
		if (TestTrue(FString::Printf(TEXT("%s is written"), Path), NumberAt(One, Path, Before) && NumberAt(Two, Path, After)))
		{
			TestEqual(FString::Printf(TEXT("%s is unchanged"), Path), After, Before, 0.0001);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleSourceFirearmAimArcTest,
	"Mantle.Vehicle.Source.FirearmAimArcRoundTrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleSourceFirearmAimArcTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleExportScaleTestPrivate;
	const TMap<FString, FString> NoMaterials;
	UMantleVehicleSource* Source = MakeSource(1.f);
	FMantleVehicleSourceTuning& Tuning = Source->Tuning;
	Tuning.SeatWeaponArc.bEnabled = false;
	Tuning.PassengerSeatWeaponArc.bEnabled = false;

	FString Error;
	FMantleChassisSpec CookedSpec;
	if (!TestTrue(TEXT("the existing sedan chassis provides the cooked vehicle fixture"),
		FMantleVehicleChassisBuilder::LoadSpec(TEXT("sedan"), CookedSpec, Error)))
	{
		AddError(Error);
		return false;
	}
	TSharedPtr<FJsonObject> CookedBase = ParseObjectText(FMantleChassisSpec::ComposeDocument(CookedSpec));
	if (!TestNotNull(TEXT("the cooked sedan document parses as JSON"), CookedBase.Get()))
	{
		return false;
	}
	const TSharedRef<FJsonObject> LegacyDocument = FMantleVehicleConverter::MakeDocument(*Source, NoMaterials, &Error);
	if (!TestTrue(*FString::Printf(TEXT("disabled arcs leave a valid source document (%s)"), *Error), Error.IsEmpty()))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* Driver = nullptr;
	const TSharedPtr<FJsonObject>* Passenger = nullptr;
	if (!TestTrue(TEXT("the source has its driver and passenger seats"),
		LegacyDocument->TryGetObjectField(TEXT("seat"), Driver) && Driver && Driver->IsValid()
		&& LegacyDocument->TryGetObjectField(TEXT("passengerSeat"), Passenger) && Passenger && Passenger->IsValid()))
	{
		return false;
	}
	TestFalse(TEXT("the disabled driver arc is not written"), (*Driver)->HasField(TEXT("weaponArc")));
	TestFalse(TEXT("the disabled passenger arc is not written"), (*Passenger)->HasField(TEXT("weaponArc")));
	const TSharedRef<FJsonObject> LegacyCookedDocument = SourceArcsOnCookedVehicle(CookedBase, LegacyDocument, Error);
	if (!TestTrue(*FString::Printf(TEXT("the source seat fields fit the cooked vehicle fixture (%s)"), *Error), Error.IsEmpty()))
	{
		return false;
	}
	FMantleVehicleDesc Parsed;
	if (!TestTrue(TEXT("a cooked document without arcs reads"), ParseDocumentText(Text(LegacyCookedDocument), Parsed, Error)))
	{
		AddError(Error);
		return false;
	}
	TestFalse(TEXT("without an arc the driver seat has none set (worked out from the seat)"), Parsed.SeatWeaponArc.bSet);
	TestFalse(TEXT("without an arc the passenger seat has none set"), Parsed.PassengerSeatWeaponArc.bSet);

	Tuning.SeatWeaponArc.bEnabled = true;
	Tuning.SeatWeaponArc.YawMin = -52.5f;
	Tuning.SeatWeaponArc.YawMax = 117.5f;
	Tuning.SeatWeaponArc.PitchMin = -17.5f;
	Tuning.SeatWeaponArc.PitchMax = 37.5f;
	Tuning.PassengerSeatWeaponArc.bEnabled = true;
	Tuning.PassengerSeatWeaponArc.YawMin = -67.5f;
	Tuning.PassengerSeatWeaponArc.YawMax = 102.5f;
	Tuning.PassengerSeatWeaponArc.PitchMin = -22.5f;
	Tuning.PassengerSeatWeaponArc.PitchMax = 47.5f;
	const TSharedRef<FJsonObject> AuthoredDocument = FMantleVehicleConverter::MakeDocument(*Source, NoMaterials, &Error);
	if (!TestTrue(*FString::Printf(TEXT("enabled driver and passenger arcs export (%s)"), *Error), Error.IsEmpty()))
	{
		return false;
	}
	const TSharedRef<FJsonObject> AuthoredCookedDocument = SourceArcsOnCookedVehicle(CookedBase, AuthoredDocument, Error);
	if (!TestTrue(*FString::Printf(TEXT("the authored seat arcs fit the cooked vehicle fixture (%s)"), *Error), Error.IsEmpty()))
	{
		return false;
	}
	if (!TestTrue(TEXT("the runtime reader round-trips both authored arcs"), ParseDocumentText(Text(AuthoredCookedDocument), Parsed, Error)))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("both authored arcs read as set"), Parsed.SeatWeaponArc.bSet && Parsed.PassengerSeatWeaponArc.bSet);
	TestEqual(TEXT("driver yaw minimum round-trips"), Parsed.SeatWeaponArc.YawMin, -52.5);
	TestEqual(TEXT("driver yaw maximum round-trips"), Parsed.SeatWeaponArc.YawMax, 117.5);
	TestEqual(TEXT("driver pitch minimum round-trips"), Parsed.SeatWeaponArc.PitchMin, -17.5);
	TestEqual(TEXT("driver pitch maximum round-trips"), Parsed.SeatWeaponArc.PitchMax, 37.5);
	TestEqual(TEXT("passenger yaw minimum round-trips"), Parsed.PassengerSeatWeaponArc.YawMin, -67.5);
	TestEqual(TEXT("passenger yaw maximum round-trips"), Parsed.PassengerSeatWeaponArc.YawMax, 102.5);
	TestEqual(TEXT("passenger pitch minimum round-trips"), Parsed.PassengerSeatWeaponArc.PitchMin, -22.5);
	TestEqual(TEXT("passenger pitch maximum round-trips"), Parsed.PassengerSeatWeaponArc.PitchMax, 47.5);

	auto Refuses = [this, Source, &NoMaterials](const TCHAR* Name, const TCHAR* Expected)
	{
		FString Why;
		FMantleVehicleConverter::MakeDocument(*Source, NoMaterials, &Why);
		TestTrue(*FString::Printf(TEXT("%s is refused (%s)"), Name, *Why), Why.Contains(Expected));
	};
	Tuning.bSeatLocation = false;
	Refuses(TEXT("an enabled driver arc without a placed seat"), TEXT("requires its seat to be placed"));
	TArray<FMantleValidationIssue> Issues;
	FMantleVehicleConverter::CheckSource(Source, TEXT("arc validation"), Issues);
	TestTrue(TEXT("the creator source check reports the enabled arc refusal"),
		Issues.ContainsByPredicate([](const FMantleValidationIssue& Issue) { return Issue.Problem.Contains(TEXT("firearm aim arc")); }));
	Tuning.bSeatLocation = true;

	Tuning.SeatWeaponArc.YawMin = std::numeric_limits<float>::quiet_NaN();
	Refuses(TEXT("a non-finite driver bound"), TEXT("must use finite angle bounds"));
	Tuning.SeatWeaponArc.YawMin = 20.f;
	Tuning.SeatWeaponArc.YawMax = 20.f;
	Refuses(TEXT("unordered driver bounds"), TEXT("minimums must be below"));
	Tuning.SeatWeaponArc.YawMin = -180.f;
	Tuning.SeatWeaponArc.YawMax = 180.f;
	Tuning.PassengerSeatWeaponArc.PitchMin = -91.f;
	Refuses(TEXT("a passenger pitch bound outside the allowed range"), TEXT("must stay within"));

	Tuning.SeatWeaponArc.YawMin = -52.5f;
	Tuning.SeatWeaponArc.YawMax = 117.5f;
	Tuning.SeatWeaponArc.PitchMin = -17.5f;
	Tuning.SeatWeaponArc.PitchMax = 37.5f;
	Tuning.PassengerSeatWeaponArc.PitchMin = -22.5f;
	Tuning.PassengerSeatWeaponArc.PitchMax = 47.5f;

	const FString TestId = TEXT("WeaponArc_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	FVehicleSourcePackageCleanup Cleanup;
	Cleanup.PackagePath = FString::Printf(TEXT("/Game/MantleVehicles/ArcTests/%s/%s_Source"), *TestId, *TestId);
	Cleanup.AssetName = FPackageName::GetLongPackageAssetName(Cleanup.PackagePath);
	FString AssetsRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / TEXT("MantleVehicles/ArcTests"));
	FString CandidateFolder = FPaths::ConvertRelativePathToFull(AssetsRoot / TestId);
	FPaths::NormalizeDirectoryName(AssetsRoot);
	FPaths::NormalizeDirectoryName(CandidateFolder);
	if (FPaths::IsUnderDirectory(CandidateFolder, AssetsRoot)
		&& FPaths::GetCleanFilename(CandidateFolder).Equals(TestId, ESearchCase::IgnoreCase))
	{
		Cleanup.Folder = CandidateFolder;
	}
	if (!TestTrue(TEXT("the scratch source folder is safely inside MantleVehicles/ArcTests"), !Cleanup.Folder.IsEmpty()))
	{
		return false;
	}
	IFileManager::Get().MakeDirectory(*Cleanup.Folder, true);
	UPackage* Package = CreatePackage(*Cleanup.PackagePath);
	if (!TestNotNull(TEXT("the source package is created"), Package))
	{
		return false;
	}
	UMantleVehicleSource* SavedSource = NewObject<UMantleVehicleSource>(Package, FName(*Cleanup.AssetName), RF_Public | RF_Standalone);
	if (!TestNotNull(TEXT("the vehicle source asset is created"), SavedSource))
	{
		return false;
	}
	SavedSource->Tuning.bSeatLocation = true;
	SavedSource->Tuning.bPassengerSeat = true;
	SavedSource->FitScale = Source->FitScale;
	SavedSource->Wheels = Source->Wheels;
	SavedSource->Parts = Source->Parts;
	SavedSource->Tuning = Tuning;
	Package->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(SavedSource);
	const FString Filename = FPackageName::LongPackageNameToFilename(Cleanup.PackagePath, FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	if (!TestTrue(TEXT("the authored vehicle source saves"), UPackage::SavePackage(Package, SavedSource, *Filename, SaveArgs)))
	{
		return false;
	}
	FText UnloadError;
	if (!TestTrue(*FString::Printf(TEXT("the saved source unloads before reopen (%s)"), *UnloadError.ToString()),
		FMantleAssetCleanup::UnloadPackagesClean({ Package }, UnloadError)))
	{
		return false;
	}
	UPackage* ReloadedPackage = LoadPackage(nullptr, *Cleanup.PackagePath, LOAD_None);
	UMantleVehicleSource* ReloadedSource = ReloadedPackage ? FindObject<UMantleVehicleSource>(ReloadedPackage, *Cleanup.AssetName) : nullptr;
	if (!TestNotNull(TEXT("the vehicle source reopens from its saved package"), ReloadedSource))
	{
		return false;
	}
	TestTrue(TEXT("the saved driver arc remains enabled"), ReloadedSource->Tuning.SeatWeaponArc.bEnabled);
	TestEqual(TEXT("the saved driver yaw bound reopens"), ReloadedSource->Tuning.SeatWeaponArc.YawMax, 117.5f);
	TestTrue(TEXT("the saved passenger arc remains enabled"), ReloadedSource->Tuning.PassengerSeatWeaponArc.bEnabled);
	TestEqual(TEXT("the saved passenger pitch bound reopens"), ReloadedSource->Tuning.PassengerSeatWeaponArc.PitchMin, -22.5f);
	const TSharedRef<FJsonObject> ReopenedDocument = FMantleVehicleConverter::MakeDocument(*ReloadedSource, NoMaterials, &Error);
	if (!TestTrue(*FString::Printf(TEXT("the reopened source exports its arcs (%s)"), *Error), Error.IsEmpty()))
	{
		AddError(Error);
		return false;
	}
	const TSharedRef<FJsonObject> ReopenedCookedDocument = SourceArcsOnCookedVehicle(CookedBase, ReopenedDocument, Error);
	if (!TestTrue(*FString::Printf(TEXT("the reopened arcs fit the cooked vehicle fixture (%s)"), *Error), Error.IsEmpty())
		|| !TestTrue(TEXT("the reopened document reads both arcs"), ParseDocumentText(Text(ReopenedCookedDocument), Parsed, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("the reopened driver arc reaches the vehicle document"), Parsed.SeatWeaponArc.YawMax, 117.5);
	TestEqual(TEXT("the reopened passenger arc reaches the vehicle document"), Parsed.PassengerSeatWeaponArc.PitchMax, 47.5);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleExportScaleBodyTest,
	"Mantle.Vehicle.Fit.ExportScaleBody",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleExportScaleBodyTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleExportScaleTestPrivate;
	// The body is the sedan chassis's own grey mesh, so the test needs no project content and cannot silently skip. Anything that goes wrong here is a failure.
	FString BodyError;
	USkeletalMesh* Original = MantleVehicleScratchBody::Make(TEXT("/Temp/MantleVehicleExportScaleTest"), TEXT("ScaleTestBody"), BodyError);
	if (!TestNotNull(FString::Printf(TEXT("the test body (%s)"), *BodyError), Original))
	{
		return false;
	}
	ON_SCOPE_EXIT { MantleVehicleScratchBody::Discard(Original); };

	UMantleVehicleSource* Source = NewObject<UMantleVehicleSource>(GetTransientPackage());
	Source->Body = Original;
	Source->FitScale = 2.f;

	FMantleGlbMesh AsModelled;
	FMantleGlbMesh Sized;
	FMantleGlbMesh Unapplied;
	TArray<UMaterialInterface*> Materials;
	FString Error;
	const bool bAsModelledRead = FMantleVehicleConverter::ReadBody(*Source, AsModelled, Materials, Error, false);
	if (!TestTrue(FString::Printf(TEXT("the as-modelled body reads (%s)"), *Error), bAsModelledRead))
	{
		return false;
	}
	if (!TestTrue(TEXT("the sized body reads"), FMantleVehicleConverter::ReadBody(*Source, Sized, Materials, Error, true)))
	{
		return false;
	}

	// Without the flag the body is as modelled, whatever the Size says (the fitter measures it that way).
	TestTrue(TEXT("a body read without the flag is as modelled"), FMantleVehicleConverter::ReadBody(*Source, Unapplied, Materials, Error));
	TestTrue(TEXT("the unflagged body is the as-modelled body"), Unapplied.Positions == AsModelled.Positions);

	if (!TestEqual(TEXT("the same vertices"), Sized.Positions.Num(), AsModelled.Positions.Num()) || !TestTrue(TEXT("it has vertices"), AsModelled.Positions.Num() > 0))
	{
		return false;
	}
	FBox Before(ForceInit);
	FBox After(ForceInit);
	for (int32 Index = 0; Index < AsModelled.Positions.Num(); ++Index)
	{
		Before += FVector(AsModelled.Positions[Index]);
		After += FVector(Sized.Positions[Index]);
	}
	TestTrue(TEXT("the sized body's smallest corner is doubled"), After.Min.Equals(Before.Min * 2.0, 0.01));
	TestTrue(TEXT("the sized body's largest corner is doubled"), After.Max.Equals(Before.Max * 2.0, 0.01));
	TestTrue(TEXT("the normals are not scaled"), Sized.Normals == AsModelled.Normals);

	if (TestEqual(TEXT("the same bones"), Sized.Joints.Num(), AsModelled.Joints.Num()))
	{
		bool bBonesDoubled = true;
		for (int32 Joint = 0; Joint < Sized.Joints.Num(); ++Joint)
		{
			bBonesDoubled = bBonesDoubled && Sized.Joints[Joint].Local.GetTranslation().Equals(AsModelled.Joints[Joint].Local.GetTranslation() * 2.0, 0.01);
		}
		TestTrue(TEXT("every bone's rest translation is doubled"), bBonesDoubled);
	}
	return true;
}

#endif
