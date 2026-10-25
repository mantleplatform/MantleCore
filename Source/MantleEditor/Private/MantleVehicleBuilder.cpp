// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleBuilder.h"

#include "MantleChassisSpec.h"
#include "MantlePlaceValidator.h"
#include "MantleVehicleBodySplit.h"
#include "MantleVehicleChassisBuilder.h"
#include "MantleVehicleChassisDocument.h"
#include "MantleVehicleChassisFit.h"
#include "MantleVehicleChassisMesh.h"
#include "MantleVehicleConverter.h"
#include "MantleVehicleFit.h"
#include "MantleVehiclePreset.h"
#include "MantleVehicleShape.h"
#include "MantleVehicleThumbnail.h"
#include "MantleVehicleSource.h"
#include "MantleVehicleSpawnPoint.h"

#include "AssetImportTask.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Factories/FbxFactory.h"
#include "Factories/FbxImportUI.h"
#include "IAssetTools.h"
#include "Editor.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Editor/EditorEngine.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "LevelEditorViewport.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

// Named, not anonymous: unity builds merge this file with others, and the test declares ApplyChosenPreset to reach it.
namespace MantleVehicleBuilderPrivate
{
	/**
	 * Gives Source the chosen type. A new vehicle has no type yet, so it takes the type's defaults. One built from this same type
	 * before is the creator's to tune: pressing the button again keeps their edits, and picking a different type starts again from
	 * that type's defaults. The vehicle only remembers the type once its defaults were applied, so a refused apply leaves it as it was.
	 */
	bool ApplyChosenPreset(const FMantleVehiclePreset& Preset, UMantleVehicleSource& Source, FMantleVehicleBuildResult& Out)
	{
		const FName ChosenPreset(*Preset.Id);
		if (Source.Preset == ChosenPreset)
		{
			Out.Notes.Add(FString::Printf(TEXT("Kept your earlier settings for this %s. Pick a different vehicle type to start again from its defaults."), *Preset.Name));
			return true;
		}

		// A vehicle converted before types existed has no type; if its settings were changed, the new type's replace them.
		const FMantleVehicleSourceTuning Fresh;
		const bool bHadOwnSettings = Source.Preset.IsNone()
			&& !FMantleVehicleSourceTuning::StaticStruct()->CompareScriptStruct(&Source.Tuning, &Fresh, PPF_None);

		TArray<FString> PresetNotes;
		if (!FMantleVehiclePresets::ApplyDefaults(Preset, Source, PresetNotes))
		{
			Out.Problems.Add(FString::Printf(TEXT("The %s settings could not be applied."), *Preset.Name));
			Out.Problems.Append(PresetNotes);
			return false;
		}
		Source.Modify();
		Source.Preset = ChosenPreset;
		if (bHadOwnSettings)
		{
			Out.Notes.Add(FString::Printf(TEXT("This vehicle had its own settings from before; they were replaced by the %s's. Undo (Ctrl+Z) brings them back."), *Preset.Name));
		}
		Out.Notes.Append(PresetNotes);
		return true;
	}

	// Defined with BuildChassis below, which they are shared with.
	void ApplyMovement(const FMantleChassisSpec& Spec, UMantleVehicleSource& Source);
	void ApplyGroundHandling(const FMantleChassisSpec& Spec, UMantleVehicleSource& Source);

	/** The wheels of a vehicle as a line of text: which bones, how big. Two vehicles with the same line were made from the same wheels. */
	FString WheelSignature(const TArray<FMantleVehicleSourceWheel>& Wheels)
	{
		FString Out;
		for (const FMantleVehicleSourceWheel& Wheel : Wheels)
		{
			Out += FString::Printf(TEXT("%s|%.1f|%.1f;"), *Wheel.Bone.ToString(), Wheel.Radius, Wheel.Width);
		}
		return Out;
	}

	/** The vehicle a model was already built into (ConvertAsset's <Model>_Vehicle beside it), loaded if it is on disk; null for a model never built. */
	UMantleVehicleSource* FindBuiltVehicle(const USkeletalMesh& Mesh)
	{
		const FString AssetName = Mesh.GetName() + TEXT("_Vehicle");
		const FString PackageName = FPackageName::GetLongPackagePath(Mesh.GetOutermost()->GetName()) / AssetName;
		const FString ObjectPath = PackageName + TEXT(".") + AssetName;
		UObject* Existing = StaticFindObject(UMantleVehicleSource::StaticClass(), nullptr, *ObjectPath);
		if (!Existing && FPackageName::DoesPackageExist(PackageName))
		{
			Existing = StaticLoadObject(UMantleVehicleSource::StaticClass(), nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
		}
		return Cast<UMantleVehicleSource>(Existing);
	}

	/**
	 * The model's body vertices as it uploads, in model units, left out when their heaviest influence is a wheel's (the cook's chassis rule,
	 * as the preview takes it). False when the body could not be read.
	 */
	bool BodyPointsOffWheels(const UMantleVehicleSource& Source, TArray<FVector>& OutPoints)
	{
		OutPoints.Reset();
		FMantleGlbMesh Body;
		TArray<UMaterialInterface*> Materials;
		FString Error;
		if (!FMantleVehicleConverter::ReadBody(Source, Body, Materials, Error))
		{
			return false;
		}
		TSet<int32> WheelJoints;
		for (const FMantleVehicleSourceWheel& Wheel : Source.Wheels)
		{
			const int32 Joint = Body.Joints.IndexOfByPredicate([&Wheel](const FMantleGlbJoint& Candidate) { return Candidate.Name == Wheel.Bone; });
			if (Joint != INDEX_NONE)
			{
				WheelJoints.Add(Joint);
			}
		}
		const int32 N = Body.NumInfluences;
		for (int32 Vertex = 0; Vertex < Body.Positions.Num(); ++Vertex)
		{
			const int32 Heaviest = N > 0 && Body.InfluenceJoints.IsValidIndex(Vertex * N) ? Body.InfluenceJoints[Vertex * N] : 0;
			if (!WheelJoints.Contains(Heaviest))
			{
				OutPoints.Add(FVector(Body.Positions[Vertex]));
			}
		}
		return true;
	}

}

bool FMantleVehicleBuilder::IsScratchVehicle(const UMantleVehicleSource& Source)
{
	const UPackage* Package = Source.GetOutermost();
	return !Package || Package == GetTransientPackage() || Source.HasAnyFlags(RF_Transient) || Package->HasAnyFlags(RF_Transient)
		|| Package->GetName().StartsWith(TEXT("/Temp"));
}

bool FMantleVehicleBuilder::SaveVehicle(UMantleVehicleSource& Source, FString& OutProblem)
{
	OutProblem.Reset();
	if (IsScratchVehicle(Source))
	{
		return false;
	}
	UPackage* Package = Source.GetOutermost();
	const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs Save;
	Save.TopLevelFlags = RF_Public | RF_Standalone;
	if (UPackage::SavePackage(Package, &Source, *File, Save))
	{
		return true;
	}
	OutProblem = TEXT("Couldn't save your vehicle automatically; its file may be read-only or locked by source control. Save it from the Content Browser.");
	return false;
}

bool FMantleVehicleBuilder::IsModelUnsaved(const UMantleVehicleSource& Source, const USkeletalMesh* Mesh)
{
	FString PackageName;
	if (Mesh)
	{
		PackageName = Mesh->GetOutermost()->GetName();
	}
	else if (!Source.Body.IsNull())
	{
		PackageName = Source.Body.ToSoftObjectPath().GetLongPackageName();
	}
	if (PackageName.IsEmpty())
	{
		return false;
	}
	const UPackage* Package = FindPackage(nullptr, *PackageName);
	return (Package && Package->IsDirty()) || !FPackageName::DoesPackageExist(PackageName);
}

FString FMantleVehicleBuilder::ModelUnsavedNote()
{
	return TEXT("Your model isn't saved yet. Save it too (Ctrl+S), or your vehicle will lose its body when the editor restarts.");
}

FMantleVehicleBuildResult FMantleVehicleBuilder::Build(USkeletalMesh* Mesh, const FString& PresetId)
{
	FMantleVehicleBuildResult Out;

	const FMantleVehiclePreset* Preset = FMantleVehiclePresets::Find(PresetId);
	if (!Preset)
	{
		Out.Problems.Add(TEXT("Pick a vehicle type first."));
	}
	if (!Mesh)
	{
		Out.Problems.Add(TEXT("Pick the model to use as the vehicle's body."));
	}
	if (!Preset || !Mesh)
	{
		return Out;
	}

	// One undo step for the whole build, so Ctrl+Z puts back the settings it replaced.
	const FScopedTransaction Transaction(NSLOCTEXT("MantleVehicleBuilder", "BuildVehicle", "Build vehicle"));

	// What the model's wheels were when it was built before: the conversion below reads them again, so this is the only chance to compare.
	const UMantleVehicleSource* Before = MantleVehicleBuilderPrivate::FindBuiltVehicle(*Mesh);
	const FString WheelsBefore = Before ? MantleVehicleBuilderPrivate::WheelSignature(Before->Wheels) : FString();

	// The same conversion the Convert vehicle command runs; it makes <Mesh>_Vehicle beside the model, or updates the one there.
	FMantleVehicleConversion Conversion;
	UMantleVehicleSource* Source = FMantleVehicleConverter::ConvertAsset(Mesh, Conversion, nullptr);
	Out.Problems.Append(Conversion.Problems);
	if (!Source)
	{
		if (Out.Problems.Num() == 0)
		{
			Out.Problems.Add(TEXT("Mantle could not turn this model into a vehicle."));
		}
		return Out;
	}
	Out.Source = Source;
	// The converter's notes on what it read from the model's physics (collision, centre of mass, where the driver sits) are left out:
	// the chassis replaces all of them, and the creator would be told to edit values that are not used.
	for (const FString& Note : Conversion.Notes)
	{
		if (!Note.StartsWith(TEXT("Collision:")) && !Note.StartsWith(TEXT("Centre of mass:")) && !Note.StartsWith(TEXT("The driver sits behind the middle")))
		{
			Out.Notes.Add(Note);
		}
	}

	// Defaults are applied (and the Size proposed) for a new vehicle or a different type; the same type again keeps the creator's, unless the model's
	// wheels are not the ones it was fitted to (the creator moved, renamed or resized them and pressed Build again): its Size and chassis are then fitted again.
	const bool bWheelsChanged = Before && WheelsBefore != MantleVehicleBuilderPrivate::WheelSignature(Source->Wheels);
	const bool bAppliesDefaults = Source->Preset != FName(*Preset->Id) || bWheelsChanged;
	if (bWheelsChanged && Source->Preset == FName(*Preset->Id))
	{
		Out.Notes.Add(TEXT("Your model's wheels changed since the last build, so Mantle fitted the chassis and the size to them again."));
	}

	// The model is only a skin: all it gives is where its wheels are. The chassis is read first, so a build that cannot be
	// fitted changes nothing about the vehicle's type and can simply be pressed again.
	TArray<FMantleVehicleFitWheel> Wheels;
	const bool bHaveWheels = FMantleVehicleFitter::WheelsFromMesh(*Mesh, *Source, Wheels);
	FMantleVehicleChassisReference Chassis;
	FMantleChassisLayout Layout;
	if (bAppliesDefaults)
	{
		FString ChassisText;
		FString ChassisError;
		if (!FMantleVehicleChassisBuilder::LoadPresetText(Preset->Id, ChassisText, ChassisError)
			|| !FMantleVehicleChassisBuilder::ReadReference(ChassisText, Chassis, ChassisError))
		{
			Out.Problems.Add(FString::Printf(TEXT("The %s chassis could not be read: %s."), *Preset->Name, *ChassisError));
			return Out;
		}
		Layout = FMantleVehicleChassisFit::LayoutFromWheels(Wheels);
		if (!bHaveWheels || !Layout.bValid)
		{
			Out.Problems.Add(TEXT("Mantle could not find this model's wheels, so it cannot fit the vehicle chassis to it. Check that the model has a bone at the centre of each wheel, named like Wheel_FL, Wheel_FR, Wheel_BL and Wheel_BR, with the car facing +X, and build again."));
			return Out;
		}
		if (Layout.bCentreline != Chassis.bCentreline)
		{
			Out.Problems.Add(Layout.bCentreline
				? FString::Printf(TEXT("This model's wheels are in a line down the middle, like a motorcycle's, but a %s has wheels on both sides. Pick Motorcycle, or a model with a wheel at each corner."), *Preset->Name)
				: FString::Printf(TEXT("This model has wheels on both sides, but a %s has one wheel in front and one behind. Pick a car type, or a model with two wheels in a line."), *Preset->Name));
			return Out;
		}
	}

	if (!MantleVehicleBuilderPrivate::ApplyChosenPreset(*Preset, *Source, Out))
	{
		return Out;
	}

	if (bHaveWheels)
	{
		Out.Fit = FMantleVehicleFitter::Compute(*Preset, Wheels);
		if (bAppliesDefaults)
		{
			Source->Modify();
			// A motorcycle's rider fit is flat numbers in the model's units, which the export does not resize, so a bike is built at the size of its model.
			Source->FitScale = Chassis.bCentreline ? 1.f : static_cast<float>(Out.Fit->Scale);
			if (!Chassis.bCentreline)
			{
				Out.Notes.Add(Out.Fit->Reason);
			}
		}
	}

	// Every physical value is the chassis's, stretched to the model's wheels and written in the model's units (the export multiplies by Size).
	if (bAppliesDefaults)
	{
		const FMantleChassisStretch Stretch = FMantleVehicleChassisFit::Stretch(Chassis, Layout, Source->FitScale);
		FMantleChassisPlacement Placement = FMantleVehicleChassisFit::Place(Chassis, Stretch);

		// The collision follows the model's visible body above the chassis tub; weight, centre of mass and handling stay the chassis's.
		TArray<FVector> BodyPoints;
		const bool bBodyRead = MantleVehicleBuilderPrivate::BodyPointsOffWheels(*Source, BodyPoints);
		// The tub is the preset's length scaled by the wheelbase, but overhangs differ: it is kept inside the body's length and width, so no
		// box sticks out past the car as an invisible block. Characters meet the model's exact shape at runtime, not these boxes.
		const bool bTubTrimmed = FMantleVehicleChassisFit::ClampTubToBody(Placement, BodyPoints);
		Placement.BodyBoxes = FMantleVehicleChassisFit::FitBodyBoxes(BodyPoints, Placement.CollisionCenter, Placement.CollisionExtent);
		if (bTubTrimmed)
		{
			Out.Notes.Add(TEXT("The chassis's collision box was trimmed to your model's length and width, so it doesn't stick out past the body."));
		}
		FMantleVehicleChassisFit::Apply(Placement, *Source);

		// The chassis's handling a published vehicle carries: a car's steering that eases off with speed and its front and rear grip, as the chassis car has them,
		// and the movement part (a motorcycle's rider fit, stretched with the chassis).
		FMantleChassisSpec Spec;
		FString SpecError;
		if (FMantleVehicleChassisBuilder::LoadSpec(Preset->Id, Spec, SpecError))
		{
			if (Spec.Rider.bSet)
			{
				FMantleVehicleChassisRider& Rider = Spec.Rider;
				Rider.LeftGrip = Stretch.PointToModel(Rider.LeftGrip);
				Rider.RightGrip = Stretch.PointToModel(Rider.RightGrip);
				Rider.LeftPeg = Stretch.PointToModel(Rider.LeftPeg);
				Rider.RightPeg = Stretch.PointToModel(Rider.RightPeg);
				Rider.PelvisOffset = Stretch.ExtentToModel(Rider.PelvisOffset);
				Rider.StopPelvisShift = Stretch.ExtentToModel(Rider.StopPelvisShift);
			}
			MantleVehicleBuilderPrivate::ApplyGroundHandling(Spec, *Source);
			MantleVehicleBuilderPrivate::ApplyMovement(Spec, *Source);
		}
		Out.Notes.Add(FString::Printf(TEXT("Built on the %s chassis: Mantle sets the weight, collision, seats and handling. Your model is the look."), *Preset->Name));
		if (Placement.BodyBoxes.Num() > 0)
		{
			Out.Notes.Add(TEXT("Collision follows your model's shape; weight and handling come from the chassis."));
		}
		else if (!bBodyRead)
		{
			Out.Notes.Add(TEXT("Mantle could not read your model's body here, so its collision is the chassis's box alone: the roof is not solid. Build again once the model's mesh data is readable."));
		}
		Out.Notes.Append(FMantleVehicleChassisFit::StretchNotes(Stretch));
		if (Layout.Radius <= 0.0)
		{
			Out.Notes.Add(TEXT("Couldn't measure this model's wheels, so its ground was taken to be Z = 0. Check the vehicle sits right in the preview."));
		}
	}
	Out.bOk = true;
	Out.bSaved = SaveVehicle(*Source, Out.SaveProblem);
	if (IsModelUnsaved(*Source, Mesh))
	{
		Out.Notes.Add(ModelUnsavedNote());
	}
	return Out;
}

FVector FMantleVehicleBuilder::DefaultDriverSeat(const USkeletalMesh& Model, const UMantleVehicleSource& Source)
{
	// A vehicle of a type has its chassis's seat, fitted to the model's wheels as Build does.
	FString Text;
	FString Error;
	FMantleVehicleChassisReference Chassis;
	TArray<FMantleVehicleFitWheel> Wheels;
	if (!Source.Preset.IsNone() && FMantleVehicleChassisBuilder::LoadPresetText(Source.Preset.ToString(), Text, Error)
		&& FMantleVehicleChassisBuilder::ReadReference(Text, Chassis, Error) && FMantleVehicleFitter::WheelsFromMesh(Model, Source, Wheels))
	{
		const FMantleChassisLayout Layout = FMantleVehicleChassisFit::LayoutFromWheels(Wheels);
		if (Layout.bValid)
		{
			return FMantleVehicleChassisFit::Place(Chassis, FMantleVehicleChassisFit::Stretch(Chassis, Layout, Source.FitScale)).Seat.BoundToCube(1000.0);
		}
	}

	// Otherwise (a vehicle with no type): the cook measures the body's own geometry; the model's bounds are the same box, in the same space. The converter has no copy
	// of this, since it leaves an unplaced seat to the cook, and a hidden driver's seat has to be written (its location is needed).
	const FBox Box = Model.GetBounds().GetBox();
	const FVector Size = Box.GetSize();
	const FVector Middle = Box.GetCenter();
	const FVector Seat(Middle.X - 0.1 * Size.X, Middle.Y - 0.2 * Size.Y, Box.Min.Z + 0.45 * Size.Z);
	return Seat.BoundToCube(1000.0);
}

void FMantleVehicleBuilder::SetHiddenRiders(UMantleVehicleSource& Source, const USkeletalMesh* Model, const bool bHide)
{
	if (Source.Tuning.bHideDriver == bHide && Source.Tuning.bHidePassenger == bHide)
	{
		return;
	}
	const FScopedTransaction Transaction(NSLOCTEXT("MantleVehicleBuilder", "ChangeNoInside", "Change whether the vehicle's people are hidden"));
	Source.Modify();
	if (bHide && !Source.Tuning.bSeatLocation && Model)
	{
		Source.Tuning.bSeatLocation = true;
		Source.Tuning.SeatLocation = DefaultDriverSeat(*Model, Source);
	}
	Source.Tuning.bHideDriver = bHide;
	Source.Tuning.bHidePassenger = bHide;
	Source.MarkPackageDirty();
}

namespace MantleVehicleBuilderPrivate
{
	// Mantle.Vehicle.Build: what the Build a vehicle window does, from the console or a script, so test vehicles can be made
	// without clicking through the window (an owner check's fixtures, an agent's run). Saves like the window does.
	FAutoConsoleCommand BuildCommand(
		TEXT("Mantle.Vehicle.Build"),
		TEXT("Mantle.Vehicle.Build <model object path> <vehicle type id> [hidden] -- builds a vehicle as Tools > Mantle > Build a vehicle does and saves it; "
			 "'hidden' ticks This model has no inside. Logs MANTLE_VEHICLE_BUILD ok=<0|1> with the vehicle, its Size and every note."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() < 2)
			{
				UE_LOG(LogTemp, Error, TEXT("MANTLE_VEHICLE_BUILD ok=0 usage: Mantle.Vehicle.Build <model object path> <vehicle type id> [hidden]"));
				return;
			}
			USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *Args[0]);
			FMantleVehicleBuildResult Result = FMantleVehicleBuilder::Build(Mesh, Args[1]);
			if (Result.bOk && Result.Source && Args.Num() > 2 && Args[2].Equals(TEXT("hidden"), ESearchCase::IgnoreCase))
			{
				FMantleVehicleBuilder::SetHiddenRiders(*Result.Source, Mesh, true);
				Result.bSaved = FMantleVehicleBuilder::SaveVehicle(*Result.Source, Result.SaveProblem);
			}
			UE_LOG(LogTemp, Display, TEXT("MANTLE_VEHICLE_BUILD ok=%d saved=%d vehicle=%s size=%.3f hidden=%d model=%s"), Result.bOk ? 1 : 0, Result.bSaved ? 1 : 0,
				Result.Source ? *Result.Source->GetPathName() : TEXT("none"), Result.Source ? Result.Source->FitScale : 0.f,
				Result.Source && Result.Source->Tuning.bHideDriver ? 1 : 0, *Args[0]);
			for (const FString& Problem : Result.Problems)
			{
				UE_LOG(LogTemp, Warning, TEXT("MANTLE_VEHICLE_BUILD problem: %s"), *Problem);
			}
			for (const FString& Note : Result.Notes)
			{
				UE_LOG(LogTemp, Display, TEXT("MANTLE_VEHICLE_BUILD note: %s"), *Note);
			}
			if (!Result.SaveProblem.IsEmpty())
			{
				UE_LOG(LogTemp, Warning, TEXT("MANTLE_VEHICLE_BUILD save problem: %s"), *Result.SaveProblem);
			}
		}));
}

namespace MantleVehicleBuilderPrivate
{
	// Mantle.Vehicle.BuildChassis: what Tools > Mantle > Build a vehicle > Shape it and Drive it make, from the console or a script, so a shaped
	// vehicle can be made without clicking (an owner check's fixtures, an agent's run). Saves like the window does, under /Game/MantleVehicles.
	FAutoConsoleCommand BuildChassisCommand(
		TEXT("Mantle.Vehicle.BuildChassis"),
		TEXT("Mantle.Vehicle.BuildChassis <vehicle type id> [wheelbase=<cm>] [track=<cm>] [wheelradius=<cm>] [wheelwidth=<cm>] [length=<cm>] [width=<cm>] [height=<cm>] [clearance=<cm>] "
			 "[size=<factor>] [grip=<-1..1>] [power=<-1..1>] [stiffness=<-1..1>] [topspeed=<-1..1>] [passenger=<0|1>] "
			 "-- makes a vehicle with no model as Tools > Mantle > Build a vehicle does (sizes held to their ranges; a plane, jet, helicopter or hovercraft takes size= and no other sizes), "
			 "saves it under /Game/MantleVehicles and logs MANTLE_VEHICLE_BUILD_CHASSIS ok=<0|1> with the vehicle's path, its handling and every note."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() < 1)
			{
				UE_LOG(LogTemp, Error, TEXT("MANTLE_VEHICLE_BUILD_CHASSIS ok=0 usage: Mantle.Vehicle.BuildChassis <vehicle type id> [wheelbase=<cm>] [track=<cm>] [wheelradius=<cm>] [wheelwidth=<cm>] [length=<cm>] [width=<cm>] [height=<cm>] [clearance=<cm>] [size=<factor>] [grip=<-1..1>] [power=<-1..1>] [stiffness=<-1..1>] [topspeed=<-1..1>] [passenger=<0|1>]"));
				return;
			}
			FMantleChassisSpec Archetype;
			FString Error;
			if (!FMantleVehicleChassisBuilder::LoadSpec(Args[0], Archetype, Error))
			{
				UE_LOG(LogTemp, Error, TEXT("MANTLE_VEHICLE_BUILD_CHASSIS ok=0 type=%s problem: %s"), *Args[0], *Error);
				return;
			}
			const TPair<const TCHAR*, EMantleVehicleDimension> Names[] = {
				{ TEXT("wheelbase"), EMantleVehicleDimension::Wheelbase }, { TEXT("track"), EMantleVehicleDimension::Track },
				{ TEXT("wheelradius"), EMantleVehicleDimension::WheelRadius }, { TEXT("wheelwidth"), EMantleVehicleDimension::WheelWidth },
				{ TEXT("length"), EMantleVehicleDimension::BodyLength }, { TEXT("width"), EMantleVehicleDimension::BodyWidth },
				{ TEXT("height"), EMantleVehicleDimension::BodyHeight }, { TEXT("clearance"), EMantleVehicleDimension::GroundClearance } };
			const TPair<const TCHAR*, EMantleVehicleFeel> Feels[] = { { TEXT("grip"), EMantleVehicleFeel::Grip }, { TEXT("power"), EMantleVehicleFeel::Power },
				{ TEXT("stiffness"), EMantleVehicleFeel::Stiffness }, { TEXT("topspeed"), EMantleVehicleFeel::TopSpeed } };
			// The window's own session shapes it, so the console and the window make the same vehicle: a road vehicle by its eight sizes (Reshape), a plane, jet, helicopter or
			// hovercraft by one Size (Scaled, which grows the airframe, rotor or pads with the body; Reshape would leave them at the preset's).
			const FMantleVehiclePreset* Preset = FMantleVehiclePresets::Find(Args[0]);
			FMantleVehicleShapeSession Session;
			Session.Start(Archetype, Preset ? Preset->FitMinScale : 0.0, Preset ? Preset->FitMaxScale : 0.0);
			for (int32 Index = 1; Index < Args.Num(); ++Index)
			{
				FString Key;
				FString Value;
				if (!Args[Index].Split(TEXT("="), &Key, &Value))
				{
					UE_LOG(LogTemp, Error, TEXT("MANTLE_VEHICLE_BUILD_CHASSIS ok=0 type=%s problem: '%s' is not <key>=<value>"), *Args[0], *Args[Index]);
					return;
				}
				const double Number = FCString::Atod(*Value);
				const TPair<const TCHAR*, EMantleVehicleDimension>* FoundSize = nullptr;
				for (const TPair<const TCHAR*, EMantleVehicleDimension>& Entry : Names)
				{
					if (Key.Equals(Entry.Key, ESearchCase::IgnoreCase))
					{
						FoundSize = &Entry;
					}
				}
				const TPair<const TCHAR*, EMantleVehicleFeel>* FoundFeel = nullptr;
				for (const TPair<const TCHAR*, EMantleVehicleFeel>& Entry : Feels)
				{
					if (Key.Equals(Entry.Key, ESearchCase::IgnoreCase))
					{
						FoundFeel = &Entry;
					}
				}
				if (FoundSize)
				{
					if (Session.UsesSize() || !FMantleVehicleShape::IsEditable(Archetype, FoundSize->Value))
					{
						UE_LOG(LogTemp, Error, TEXT("MANTLE_VEHICLE_BUILD_CHASSIS ok=0 type=%s problem: this type has no %s to set%s"), *Args[0], *Key, Session.UsesSize() ? TEXT("; it is sized with size=<factor>") : TEXT(""));
						return;
					}
					Session.SetDimension(FoundSize->Value, Number);
				}
				else if (FoundFeel)
				{
					if (!Session.HasFeel(FoundFeel->Value))
					{
						UE_LOG(LogTemp, Error, TEXT("MANTLE_VEHICLE_BUILD_CHASSIS ok=0 type=%s problem: this type has no %s feel"), *Args[0], *Key);
						return;
					}
					Session.SetFeel(FoundFeel->Value, Number);
				}
				else if (Key.Equals(TEXT("size"), ESearchCase::IgnoreCase))
				{
					if (!Session.UsesSize())
					{
						UE_LOG(LogTemp, Error, TEXT("MANTLE_VEHICLE_BUILD_CHASSIS ok=0 type=%s problem: this type is shaped by its eight sizes, not by size="), *Args[0]);
						return;
					}
					Session.SetSize(Number);
				}
				else if (Key.Equals(TEXT("passenger"), ESearchCase::IgnoreCase))
				{
					if (!Session.HasPassengerOption())
					{
						UE_LOG(LogTemp, Error, TEXT("MANTLE_VEHICLE_BUILD_CHASSIS ok=0 type=%s problem: this type has no passenger seat to turn off"), *Args[0]);
						return;
					}
					Session.SetPassenger(Number != 0.0);
				}
				else
				{
					UE_LOG(LogTemp, Error, TEXT("MANTLE_VEHICLE_BUILD_CHASSIS ok=0 type=%s problem: '%s' is not a size, size=, a feel or passenger="), *Args[0], *Args[Index]);
					return;
				}
			}
			const FMantleChassisSpec Spec = Session.GetShaped();
			const TArray<FText>& Held = Session.GetHeldNotes();
			const FString Name = FMantleVehicleBuilder::FirstFreeChassisName(Args[0]);
			const FMantleVehicleBuildResult Result = FMantleVehicleBuilder::BuildChassis(Spec, FMantleVehicleBuilder::ChassisFolder() / Name, Name);
			UE_LOG(LogTemp, Display, TEXT("MANTLE_VEHICLE_BUILD_CHASSIS ok=%d saved=%d vehicle=%s type=%s shaped=%d %s"), Result.bOk ? 1 : 0, Result.bSaved ? 1 : 0,
				Result.Source ? *Result.Source->GetPathName() : TEXT("none"), *Args[0], Spec.Derived.IsSet() ? 1 : 0, *FMantleVehicleShape::Describe(FMantleVehicleShape::Summarise(Spec)).ToString());
			for (const FText& Note : Held)
			{
				UE_LOG(LogTemp, Display, TEXT("MANTLE_VEHICLE_BUILD_CHASSIS held: %s"), *Note.ToString());
			}
			for (const FText& Warning : FMantleVehicleShape::Warnings(Spec))
			{
				UE_LOG(LogTemp, Display, TEXT("MANTLE_VEHICLE_BUILD_CHASSIS warning: %s"), *Warning.ToString());
			}
			for (const FString& Problem : Result.Problems)
			{
				UE_LOG(LogTemp, Warning, TEXT("MANTLE_VEHICLE_BUILD_CHASSIS problem: %s"), *Problem);
			}
			for (const FString& Note : Result.Notes)
			{
				UE_LOG(LogTemp, Display, TEXT("MANTLE_VEHICLE_BUILD_CHASSIS note: %s"), *Note);
			}
			if (!Result.SaveProblem.IsEmpty())
			{
				UE_LOG(LogTemp, Warning, TEXT("MANTLE_VEHICLE_BUILD_CHASSIS save problem: %s"), *Result.SaveProblem);
			}
		}));
}

namespace MantleVehicleBuilderPrivate
{
	// Mantle.Vehicle.InspectModel: what the Build a vehicle window knows about a model, from the console or a script (an owner check's fixtures, an agent's run).
	FAutoConsoleCommand InspectModelCommand(
		TEXT("Mantle.Vehicle.InspectModel"),
		TEXT("Mantle.Vehicle.InspectModel <model object path> -- logs MANTLE_VEHICLE_MODEL lines: the model's size, bones (where each is), sections, and what Mantle would suggest "
			 "for it (which way it faces and which parts of it are the chassis's moving parts, for each vehicle type)."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() < 1)
			{
				UE_LOG(LogTemp, Error, TEXT("MANTLE_VEHICLE_MODEL ok=0 usage: Mantle.Vehicle.InspectModel <model object path>"));
				return;
			}
			UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *Args[0], nullptr, LOAD_NoWarn | LOAD_Quiet);
			FMantleVehicleModel Model;
			FString Error;
			if (!FMantleVehicleModel::Read(Asset, Model, Error))
			{
				UE_LOG(LogTemp, Warning, TEXT("MANTLE_VEHICLE_MODEL ok=0 model=%s problem: %s"), *Args[0], *Error);
				return;
			}
			const FBox Box = Model.Bounds();
			UE_LOG(LogTemp, Display, TEXT("MANTLE_VEHICLE_MODEL ok=1 model=%s %s min=(%.1f %.1f %.1f) max=(%.1f %.1f %.1f) size=(%.1f %.1f %.1f)"), *Args[0], *Model.Describe(),
				Box.Min.X, Box.Min.Y, Box.Min.Z, Box.Max.X, Box.Max.Y, Box.Max.Z, Box.GetSize().X, Box.GetSize().Y, Box.GetSize().Z);
			for (int32 Bone = 0; Bone < Model.Bones.Num(); ++Bone)
			{
				const FVector At = Model.BonePositions[Bone];
				UE_LOG(LogTemp, Display, TEXT("MANTLE_VEHICLE_MODEL bone %d %s parent=%d at=(%.1f %.1f %.1f)"), Bone, *Model.Bones[Bone].Name.ToString(), Model.Bones[Bone].ParentIndex, At.X, At.Y, At.Z);
			}
			for (int32 Section = 0; Section < Model.LOD.Sections.Num(); ++Section)
			{
				UE_LOG(LogTemp, Display, TEXT("MANTLE_VEHICLE_MODEL section %d: %s"), Section + 1, *FMantleVehicleChassisSkin::DescribePiece(Model, FMantleVehicleModelPiece::OfSection(Section)));
			}
			// A rough outline along each horizontal axis, in eight slices from low to high: the vertices in the slice, how wide across and how tall. The narrow, low end is usually the nose.
			for (int32 Axis = 0; Axis < 2; ++Axis)
			{
				FBox Slice[8];
				int32 Count[8] = {};
				for (const FVector3f& Position : Model.LOD.Positions)
				{
					const int32 At = FMath::Clamp(static_cast<int32>((Position[Axis] - Box.Min[Axis]) / FMath::Max(Box.GetSize()[Axis], 1.0) * 8.0), 0, 7);
					Slice[At] += FVector(Position);
					++Count[At];
				}
				FString Line;
				for (int32 At = 0; At < 8; ++At)
				{
					Line += FString::Printf(TEXT(" [n=%d across=%.0f tall=%.0f]"), Count[At], Slice[At].IsValid ? Slice[At].GetSize()[1 - Axis] : 0.0, Slice[At].IsValid ? Slice[At].GetSize().Z : 0.0);
				}
				UE_LOG(LogTemp, Display, TEXT("MANTLE_VEHICLE_MODEL outline along %s from low to high:%s"), Axis == 0 ? TEXT("X") : TEXT("Y"), *Line);
			}
			for (const FMantleVehiclePreset* Preset : FMantleVehiclePresets::Publishable())
			{
				FMantleChassisSpec Archetype;
				FMantleVehicleAsset Grey;
				if (!FMantleVehicleChassisBuilder::LoadSpec(Preset->Id, Archetype, Error) || !FMantleVehicleChassisBuilder::MakeAsset(Archetype, Grey, Error))
				{
					continue;
				}
				const TArray<FMantleVehiclePartSlot> Slots = FMantleVehicleChassisSkin::PartSlots(Archetype, Grey.Body);
				FMantleVehicleModelMap Map;
				const double Yaw = FMantleVehicleChassisSkin::SuggestMapAndYaw(Slots, Model, Map);
				FString Parts;
				for (const TPair<FName, FMantleVehicleModelPiece>& Part : Map.Parts)
				{
					Parts += FString::Printf(TEXT(" %s<-%s;"), *Part.Key.ToString(), *FMantleVehicleChassisSkin::DescribePiece(Model, Part.Value));
				}
				const FMantleVehicleModelProposal Proposal = FMantleVehicleChassisSkin::Propose(Archetype, *Preset, Model, Yaw, Map);
				UE_LOG(LogTemp, Display, TEXT("MANTLE_VEHICLE_MODEL type=%s yaw=%.0f size=%.2f suggested parts:%s"), *Preset->Id, Yaw, Proposal.bUsesSize ? Proposal.Size : 0.0, *Parts);
			}
		}));

	// Mantle.Vehicle.BuildModel: what the Build a vehicle window's Build button does for a model that is worn as a skin, from the console or a script.
	FAutoConsoleCommand BuildModelCommand(
		TEXT("Mantle.Vehicle.BuildModel"),
		TEXT("Mantle.Vehicle.BuildModel <model object path> <vehicle type id> [yaw=<deg>] [name=<Name>] [folder=</Game/...>] [bone.<chassis bone>=<model bone>] [section.<chassis bone>=<n>] [overwrite=1] "
			 "-- puts a static or skeletal mesh on the type's chassis (BuildFromModel), saves it under the folder (default /Game/MantleVehicles) and logs MANTLE_VEHICLE_BUILD_MODEL "
			 "ok=<0|1> with the vehicle's path, its Size and every note. With no yaw or parts given Mantle chooses them. A name= that a vehicle already has is refused unless overwrite=1."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() < 2)
			{
				UE_LOG(LogTemp, Error, TEXT("MANTLE_VEHICLE_BUILD_MODEL ok=0 usage: Mantle.Vehicle.BuildModel <model object path> <vehicle type id> [yaw=<deg>] [name=<Name>] [folder=</Game/...>] "
					"[bone.<chassis bone>=<model bone>] [section.<chassis bone>=<n>] [overwrite=1]"));
				return;
			}
			UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *Args[0], nullptr, LOAD_NoWarn | LOAD_Quiet);
			FMantleVehicleModelOptions Options;
			FMantleVehicleModelMap Map;
			bool bMapGiven = false;
			for (int32 Index = 2; Index < Args.Num(); ++Index)
			{
				FString Key;
				FString Value;
				if (!Args[Index].Split(TEXT("="), &Key, &Value))
				{
					UE_LOG(LogTemp, Error, TEXT("MANTLE_VEHICLE_BUILD_MODEL ok=0 '%s' is not <key>=<value>"), *Args[Index]);
					return;
				}
				if (Key.Equals(TEXT("yaw"), ESearchCase::IgnoreCase))
				{
					Options.YawDegrees = FCString::Atod(*Value);
				}
				else if (Key.Equals(TEXT("name"), ESearchCase::IgnoreCase))
				{
					Options.Name = Value;
				}
				else if (Key.Equals(TEXT("folder"), ESearchCase::IgnoreCase))
				{
					Options.Folder = Value;
				}
				else if (Key.Equals(TEXT("overwrite"), ESearchCase::IgnoreCase))
				{
					Options.bOverwrite = Value == TEXT("1") || Value.Equals(TEXT("true"), ESearchCase::IgnoreCase);
				}
				else if (Key.StartsWith(TEXT("bone."), ESearchCase::IgnoreCase))
				{
					Map.Parts.Add(FName(*Key.RightChop(5)), FMantleVehicleModelPiece::OfBone(FName(*Value)));
					bMapGiven = true;
				}
				else if (Key.StartsWith(TEXT("section."), ESearchCase::IgnoreCase))
				{
					Map.Parts.Add(FName(*Key.RightChop(8)), FMantleVehicleModelPiece::OfSection(FCString::Atoi(*Value) - 1));
					bMapGiven = true;
				}
				else
				{
					UE_LOG(LogTemp, Error, TEXT("MANTLE_VEHICLE_BUILD_MODEL ok=0 unknown key '%s'"), *Key);
					return;
				}
			}
			if (bMapGiven)
			{
				Options.Map = Map;
			}
			const FMantleVehicleBuildResult Result = FMantleVehicleBuilder::BuildFromModel(Asset, Args[1], Options);
			FString Parts;
			for (const TPair<FName, FMantleVehicleModelPiece>& Part : Result.ModelMap.Parts)
			{
				Parts += FString::Printf(TEXT(" %s<-%s"), *Part.Key.ToString(), Part.Value.Kind == FMantleVehicleModelPiece::EKind::Bone ? *Part.Value.Bone.ToString() : *FString::Printf(TEXT("section%d"), Part.Value.Section + 1));
			}
			UE_LOG(LogTemp, Display, TEXT("MANTLE_VEHICLE_BUILD_MODEL ok=%d saved=%d vehicle=%s yaw=%.0f movement=%d parts:%s model=%s"), Result.bOk ? 1 : 0, Result.bSaved ? 1 : 0,
				Result.Source ? *Result.Source->GetPathName() : TEXT("none"), Result.ModelYaw, Result.Source ? static_cast<int32>(Result.Source->MovementType) : -1, *Parts, *Args[0]);
			for (const FString& Problem : Result.Problems)
			{
				UE_LOG(LogTemp, Warning, TEXT("MANTLE_VEHICLE_BUILD_MODEL problem: %s"), *Problem);
			}
			for (const FString& Note : Result.Notes)
			{
				UE_LOG(LogTemp, Display, TEXT("MANTLE_VEHICLE_BUILD_MODEL note: %s"), *Note);
			}
			if (!Result.SaveProblem.IsEmpty())
			{
				UE_LOG(LogTemp, Warning, TEXT("MANTLE_VEHICLE_BUILD_MODEL save problem: %s"), *Result.SaveProblem);
			}
		}));
}

namespace MantleVehicleBuilderPrivate
{
	// Mantle.Vehicle.Snapshot: three plain pictures of a built vehicle's body (from above, from the side, from the front, the nose to the right), drawn from its
	// triangles with a depth buffer and no GPU, so a model's facing, size, stance and moving parts can be looked at from a headless editor. The chassis's bones are
	// dots: wheels green, control surfaces orange, propellers and rotors red, the rest white.
	FAutoConsoleCommand SnapshotCommand(
		TEXT("Mantle.Vehicle.Snapshot"),
		TEXT("Mantle.Vehicle.Snapshot <vehicle asset path> <png file> -- writes top, side and front views of the vehicle's body, nose to the right, with its bones as dots, and logs MANTLE_VEHICLE_SNAPSHOT."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() < 2)
			{
				UE_LOG(LogTemp, Error, TEXT("MANTLE_VEHICLE_SNAPSHOT ok=0 usage: Mantle.Vehicle.Snapshot <vehicle asset path> <png file>"));
				return;
			}
			const UMantleVehicleSource* Source = LoadObject<UMantleVehicleSource>(nullptr, *Args[0], nullptr, LOAD_NoWarn | LOAD_Quiet);
			FMantleSkinnedMesh Body;
			FString Error;
			if (!Source || !FMantleSkinnedMesh::ExtractFromSkeletalMesh(Source->Body.LoadSynchronous(), Body, &Error) || Body.LODs.Num() == 0)
			{
				UE_LOG(LogTemp, Warning, TEXT("MANTLE_VEHICLE_SNAPSHOT ok=0 %s: %s"), *Args[0], *Error);
				return;
			}
			const FMantleSkinnedMeshLOD& LOD = Body.LODs[0];
			const FBox3f Box = Body.ComputeBounds(0);
			constexpr int32 PanelWidth = 560;
			constexpr int32 PanelHeight = 340;
			constexpr int32 Views = 3;
			TArray<FColor> Image;
			Image.Init(FColor(24, 26, 30, 255), PanelWidth * Views * PanelHeight);
			// Each view: which model axes are screen right and screen up, and which looks toward the viewer (deeper = smaller).
			struct FView { int32 Right; int32 Up; int32 Toward; float UpSign; };
			const FView ViewList[Views] = { { 0, 1, 2, -1.f }, { 0, 2, 1, 1.f }, { 1, 2, 0, 1.f } };
			TArray<FTransform> Component;
			for (int32 Bone = 0; Bone < Body.Bones.Num(); ++Bone)
			{
				const int32 Parent = Body.Bones[Bone].ParentIndex;
				Component.Add(Parent >= 0 ? Body.ReferencePose[Bone] * Component[Parent] : Body.ReferencePose[Bone]);
			}
			for (int32 View = 0; View < Views; ++View)
			{
				const FView& V = ViewList[View];
				const float Width = Box.Max[V.Right] - Box.Min[V.Right];
				const float Height = Box.Max[V.Up] - Box.Min[V.Up];
				const float Scale = FMath::Min((PanelWidth - 40.f) / FMath::Max(Width, 1.f), (PanelHeight - 40.f) / FMath::Max(Height, 1.f));
				const float OriginX = 0.5f * (PanelWidth - Width * Scale);
				const float OriginY = 0.5f * (PanelHeight - Height * Scale);
				const auto Project = [&](const FVector3f& P) -> FVector2f
				{
					const float Px = OriginX + (P[V.Right] - Box.Min[V.Right]) * Scale;
					const float Py = V.UpSign > 0.f ? PanelHeight - OriginY - (P[V.Up] - Box.Min[V.Up]) * Scale : OriginY + (P[V.Up] - Box.Min[V.Up]) * Scale;
					return FVector2f(Px, Py);
				};
				TArray<float> Depth;
				Depth.Init(-FLT_MAX, PanelWidth * PanelHeight);
				for (int32 Corner = 0; Corner + 2 < LOD.Indices.Num(); Corner += 3)
				{
					const int32 I[3] = { static_cast<int32>(LOD.Indices[Corner]), static_cast<int32>(LOD.Indices[Corner + 1]), static_cast<int32>(LOD.Indices[Corner + 2]) };
					const FVector3f P[3] = { LOD.Positions[I[0]], LOD.Positions[I[1]], LOD.Positions[I[2]] };
					const FVector3f N = FVector3f::CrossProduct(P[1] - P[0], P[2] - P[0]).GetSafeNormal();
					FVector3f Light(0.4f, -0.5f, 0.75f);
					Light.Normalize();
					const float Shade = 0.25f + 0.75f * FMath::Abs(FVector3f::DotProduct(N, Light));
					const FVector2f S[3] = { Project(P[0]), Project(P[1]), Project(P[2]) };
					const int32 MinX = FMath::Max(0, FMath::FloorToInt(FMath::Min3(S[0].X, S[1].X, S[2].X)));
					const int32 MaxX = FMath::Min(PanelWidth - 1, FMath::CeilToInt(FMath::Max3(S[0].X, S[1].X, S[2].X)));
					const int32 MinY = FMath::Max(0, FMath::FloorToInt(FMath::Min3(S[0].Y, S[1].Y, S[2].Y)));
					const int32 MaxY = FMath::Min(PanelHeight - 1, FMath::CeilToInt(FMath::Max3(S[0].Y, S[1].Y, S[2].Y)));
					const float Area = (S[1].X - S[0].X) * (S[2].Y - S[0].Y) - (S[2].X - S[0].X) * (S[1].Y - S[0].Y);
					if (FMath::Abs(Area) < 1.0e-6f)
					{
						continue;
					}
					for (int32 Y = MinY; Y <= MaxY; ++Y)
					{
						for (int32 X = MinX; X <= MaxX; ++X)
						{
							const float Qx = X + 0.5f;
							const float Qy = Y + 0.5f;
							const float W0 = ((S[1].X - Qx) * (S[2].Y - Qy) - (S[2].X - Qx) * (S[1].Y - Qy)) / Area;
							const float W1 = ((S[2].X - Qx) * (S[0].Y - Qy) - (S[0].X - Qx) * (S[2].Y - Qy)) / Area;
							const float W2 = 1.f - W0 - W1;
							if (W0 < 0.f || W1 < 0.f || W2 < 0.f)
							{
								continue;
							}
							const float Z = W0 * P[0][V.Toward] + W1 * P[1][V.Toward] + W2 * P[2][V.Toward];
							// Toward the viewer: higher for a top view, and for the side and front views the larger Y or X (the viewer is on that side).
							if (Z > Depth[Y * PanelWidth + X])
							{
								Depth[Y * PanelWidth + X] = Z;
								const uint8 Grey = static_cast<uint8>(FMath::Clamp(255.f * Shade, 0.f, 255.f));
								Image[Y * PanelWidth * Views + View * PanelWidth + X] = FColor(Grey, Grey, Grey, 255);
							}
						}
					}
				}
				// The ground (Z = 0) in the side and front views, and the bones.
				if (V.Up == 2)
				{
					const int32 GroundY = FMath::Clamp(FMath::RoundToInt(Project(FVector3f(0.f, 0.f, 0.f)).Y), 0, PanelHeight - 1);
					for (int32 X = 0; X < PanelWidth; ++X)
					{
						Image[GroundY * PanelWidth * Views + View * PanelWidth + X] = FColor(60, 110, 60, 255);
					}
				}
				for (int32 Bone = 1; Bone < Body.Bones.Num(); ++Bone)
				{
					const FString Name = Body.Bones[Bone].Name.ToString();
					const FColor Colour = Name.StartsWith(TEXT("wheel")) || Name.StartsWith(TEXT("fork")) ? FColor(60, 230, 90, 255)
						: Name.StartsWith(TEXT("aileron")) || Name == TEXT("elevator") || Name == TEXT("rudder") ? FColor(255, 160, 40, 255)
						: Name.StartsWith(TEXT("prop")) || Name.EndsWith(TEXT("rotor")) ? FColor(255, 60, 60, 255) : FColor(255, 255, 255, 255);
					const FVector2f At = Project(FVector3f(Component[Bone].GetLocation()));
					for (int32 Dy = -3; Dy <= 3; ++Dy)
					{
						for (int32 Dx = -3; Dx <= 3; ++Dx)
						{
							const int32 X = FMath::RoundToInt(At.X) + Dx;
							const int32 Y = FMath::RoundToInt(At.Y) + Dy;
							if (X >= 0 && X < PanelWidth && Y >= 0 && Y < PanelHeight)
							{
								Image[Y * PanelWidth * Views + View * PanelWidth + X] = Colour;
							}
						}
					}
				}
				// A divider.
				for (int32 Y = 0; Y < PanelHeight; ++Y)
				{
					Image[Y * PanelWidth * Views + View * PanelWidth] = FColor(90, 90, 100, 255);
				}
			}
			TArray64<uint8> Png;
			FImageUtils::PNGCompressImageArray(PanelWidth * Views, PanelHeight, TArrayView64<const FColor>(Image.GetData(), Image.Num()), Png);
			const bool bWritten = FFileHelper::SaveArrayToFile(Png, *Args[1]);
			UE_LOG(LogTemp, Display, TEXT("MANTLE_VEHICLE_SNAPSHOT ok=%d file=%s size=(%.0f %.0f %.0f) triangles=%d"), bWritten ? 1 : 0, *Args[1], Box.GetSize().X, Box.GetSize().Y, Box.GetSize().Z,
				LOD.NumTriangles());
		}));
}

// Named, not anonymous: unity builds merge this file with others.
namespace MantleVehicleBuilderPrivate
{
	/** The take-off run a plane or a jet needs, in centimetres (the flight probes' rolls with some to spare: about 130 m for the jet, 340 m for the plane). */
	constexpr double JetRunCm = 15000.0;
	constexpr double PlaneRunCm = 35000.0;

	/** How far ahead of the camera the search for a flat stretch goes, and how often it looks. */
	constexpr double FlatSearchReachCm = 40000.0;
	constexpr double FlatSearchStepCm = 2000.0;
	/** A stretch whose ground varies by less than this over its run is flat enough to take off from. */
	constexpr double FlatEnoughCm = 150.0;

	/** The run a plane or jet needs, or 0 for a vehicle that is neither. A shaped chassis has no preset: it is a plane when its airframe has a propeller to spin. */
	double TakeOffRunCm(const UMantleVehicleSource& Source)
	{
		if (Source.MovementType != EMantleVehicleMovementKind::Plane)
		{
			return 0.0;
		}
		if (Source.Preset == FName(TEXT("jet")))
		{
			return JetRunCm;
		}
		if (Source.Preset == FName(TEXT("plane")))
		{
			return PlaneRunCm;
		}
		return Source.MovementBlocks.Contains(TEXT("spinBones")) ? PlaneRunCm : JetRunCm;
	}

	/**
	 * How much the ground varies along a run of RunCm from Start in the direction Direction: the highest sample minus the lowest, in centimetres. False when a sample
	 * finds no ground at all (the run leaves the level). The samples are taken from high above, so a roof, a tree or a hill in the way counts as the ground does.
	 */
	bool GroundVariation(UWorld& World, const FVector& Start, const FVector& Direction, const double RunCm, double& OutVariation, FVector& OutGroundAtStart)
	{
		constexpr int32 Samples = 10;
		double Lowest = TNumericLimits<double>::Max();
		double Highest = -TNumericLimits<double>::Max();
		for (int32 Index = 0; Index <= Samples; ++Index)
		{
			const FVector At = Start + Direction * (RunCm * Index / Samples);
			FHitResult Hit;
			if (!World.LineTraceSingleByChannel(Hit, FVector(At.X, At.Y, Start.Z + 50000.0), FVector(At.X, At.Y, Start.Z - 100000.0), ECC_Visibility))
			{
				return false;
			}
			Lowest = FMath::Min(Lowest, Hit.ImpactPoint.Z);
			Highest = FMath::Max(Highest, Hit.ImpactPoint.Z);
			if (Index == 0)
			{
				OutGroundAtStart = Hit.ImpactPoint;
			}
		}
		OutVariation = Highest - Lowest;
		return true;
	}
}

bool FMantleVehicleBuilder::PlaceInLevel(UMantleVehicleSource& Source, FString& OutProblem, AMantleVehicleSpawnPoint** OutActor, FString* OutNote)
{
	OutProblem.Reset();
	if (OutActor)
	{
		*OutActor = nullptr;
	}
	if (OutNote)
	{
		OutNote->Reset();
	}

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World || !World->PersistentLevel)
	{
		OutProblem = TEXT("Open the level you want to drive in first.");
		return false;
	}

	// In front of the level viewport's camera; with no viewport (a headless editor) the world origin looking along +X.
	FVector CameraLocation = FVector::ZeroVector;
	FRotator CameraRotation = FRotator::ZeroRotator;
	// An orthographic view's position is far away along its axis and means nothing in front of it, so it is not used; a perspective one is.
	FLevelEditorViewportClient* ViewClient = GCurrentLevelEditingViewportClient;
	if (!ViewClient || !ViewClient->IsPerspective())
	{
		ViewClient = nullptr;
		if (GEditor)
		{
			for (FLevelEditorViewportClient* Candidate : GEditor->GetLevelViewportClients())
			{
				if (Candidate && Candidate->IsPerspective())
				{
					ViewClient = Candidate;
					break;
				}
			}
		}
	}
	if (ViewClient)
	{
		CameraLocation = ViewClient->GetViewLocation();
		CameraRotation = ViewClient->GetViewRotation();
	}
	const FRotator Facing(0.0, CameraRotation.Yaw, 0.0);
	const FVector Ahead = CameraLocation + Facing.Vector() * 600.0;

	// Drop it onto whatever is below that spot; over nothing, it hangs a metre under the camera.
	FVector Location(Ahead.X, Ahead.Y, CameraLocation.Z - 100.0);
	FHitResult Hit;
	const FVector TraceStart = Ahead + FVector(0.0, 0.0, 100.0);
	const FVector TraceEnd = Ahead - FVector(0.0, 0.0, 100000.0);
	const bool bGroundAhead = World->LineTraceSingleByChannel(Hit, TraceStart, TraceEnd, ECC_Visibility);
	if (bGroundAhead)
	{
		// A little clearance, so the wheels are not buried in the ground; the vehicle settles onto its suspension when it spawns.
		Location = Hit.ImpactPoint + FVector(0.0, 0.0, 50.0);
	}

	// What a flier needs to be test-driven, said where it was put. A plane or a jet rolls a long way before it lifts off, so it goes where the ground ahead of the camera is
	// flattest for that run (the nearest stretch that is flat enough, else the flattest), and the creator is told where and how far; a helicopter needs room above it.
	if (OutNote)
	{
		const double RunCm = MantleVehicleBuilderPrivate::TakeOffRunCm(Source);
		if (RunCm > 0.0)
		{
			const FString Kind = RunCm == MantleVehicleBuilderPrivate::JetRunCm ? TEXT("jet") : TEXT("plane");
			const FVector Along = Facing.Vector();
			double BestVariation = TNumericLimits<double>::Max();
			double BestDistance = -1.0;
			FVector BestGround = FVector::ZeroVector;
			// The first place looked at is where it would go anyway, 6 m ahead; then every 20 m out to 400 m.
			for (int32 Step = 0; Step * MantleVehicleBuilderPrivate::FlatSearchStepCm <= MantleVehicleBuilderPrivate::FlatSearchReachCm; ++Step)
			{
				const double Distance = Step == 0 ? 600.0 : Step * MantleVehicleBuilderPrivate::FlatSearchStepCm;
				const FVector From = CameraLocation + Along * Distance;
				double Variation = 0.0;
				FVector Ground = FVector::ZeroVector;
				if (!MantleVehicleBuilderPrivate::GroundVariation(*World, FVector(From.X, From.Y, CameraLocation.Z), Along, RunCm, Variation, Ground))
				{
					continue;
				}
				if (Variation < BestVariation)
				{
					BestVariation = Variation;
					BestDistance = Distance;
					BestGround = Ground;
				}
				if (Variation <= MantleVehicleBuilderPrivate::FlatEnoughCm)
				{
					break;
				}
			}
			const int32 RunMetres = FMath::RoundToInt(RunCm / 100.0);
			if (BestDistance < 0.0)
			{
				*OutNote = FString::Printf(TEXT("A %s needs about %d m of flat, open ground in front of it to take off, and there is no ground that far ahead of you in this level. It is 6 m in front of you: move it to a runway or a flat field."),
					*Kind, RunMetres);
			}
			else
			{
				Location = BestGround + FVector(0.0, 0.0, 50.0);
				const int32 AheadMetres = FMath::RoundToInt(BestDistance / 100.0);
				*OutNote = BestVariation <= MantleVehicleBuilderPrivate::FlatEnoughCm
					? FString::Printf(TEXT("A %s needs about %d m of flat, open ground in front of it to take off. It is %d m in front of you, on a stretch that long and flat."), *Kind, RunMetres, AheadMetres)
					: FString::Printf(TEXT("A %s needs about %d m of flat, open ground in front of it to take off, and there is none near you. It is %d m in front of you, where the ground is flattest, but it varies by about %d m there: move it to a runway or a flat field."),
						*Kind, RunMetres, AheadMetres, FMath::Max(1, FMath::RoundToInt(BestVariation / 100.0)));
			}
		}
		else if (Source.MovementType == EMantleVehicleMovementKind::Helicopter)
		{
			*OutNote = TEXT("A helicopter needs open space above it: keep roofs, trees and ceilings out of the way overhead.");
		}
	}

	const FScopedTransaction Transaction(NSLOCTEXT("MantleVehicleBuilder", "PutVehicleInLevel", "Put vehicle in level"));
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AMantleVehicleSpawnPoint* Spawn = World->SpawnActor<AMantleVehicleSpawnPoint>(Location, Facing, Params);
	if (!Spawn)
	{
		OutProblem = TEXT("Mantle could not add the vehicle to your level. Try again, or add a Mantle Vehicle Spawn from the Place Actors panel and pick your vehicle on it.");
		return false;
	}
	Spawn->Modify();
	Spawn->Vehicle = &Source;
	Spawn->SetActorLabel(FString::Printf(TEXT("%s spawn"), *Source.GetName()));

	GEditor->SelectNone(/*bNoteSelectionChange*/ false, /*bDeselectBSPSurfs*/ true);
	GEditor->SelectActor(Spawn, /*bInSelected*/ true, /*bNotify*/ true);
	if (OutActor)
	{
		*OutActor = Spawn;
	}
	return true;
}

FString FMantleVehicleBuilder::ChassisFolder()
{
	return TEXT("/Game/MantleVehicles");
}

FString FMantleVehicleBuilder::FirstFreeChassisName(const FString& Wanted)
{
	return FirstFreeName(Wanted, [](const FString& Candidate)
	{
		return FPackageName::DoesPackageExist(ChassisFolder() / Candidate / (Candidate + TEXT("_Vehicle")));
	});
}

FString FMantleVehicleBuilder::FirstFreeName(const FString& Wanted, const TFunctionRef<bool(const FString&)> IsTaken)
{
	// Letters and digits only: a name that is safe as an asset's, a folder's and a Content Browser row's.
	FString Base;
	for (const TCHAR Character : Wanted)
	{
		Base.AppendChar(FChar::IsAlnum(Character) ? Character : TEXT('_'));
	}
	while (Base.StartsWith(TEXT("_")))
	{
		Base.RightChopInline(1);
	}
	while (Base.EndsWith(TEXT("_")))
	{
		Base.LeftChopInline(1);
	}
	if (Base.IsEmpty() || !FChar::IsAlpha(Base[0]))
	{
		Base = TEXT("Vehicle_") + Base;
		Base.TrimCharInline(TEXT('_'), nullptr);
	}
	for (int32 Number = 1; Number < 10000; ++Number)
	{
		const FString Candidate = Number == 1 ? Base : FString::Printf(TEXT("%s_%d"), *Base, Number);
		if (!IsTaken(Candidate))
		{
			return Candidate;
		}
	}
	return Base;
}

// Named, not anonymous: unity builds merge this file with others.
namespace MantleVehicleBuilderPrivate
{
	/**
	 * The vehicle asset's movement part from its chassis: the movement type, the seat kinds and the flight, helicopter or hover block with the rider's fit, flat
	 * numbers in the body's space at the size the chassis is. The document writes them from here (FMantleVehicleChassisDocument::MergeSource), so a plane built in the
	 * window is a plane when it is previewed, driven and published, not a car on a plane's body.
	 */
	void ApplyMovement(const FMantleChassisSpec& Spec, UMantleVehicleSource& Source)
	{
		FMantleVehicleDesc Movement;
		FMantleVehicleChassisDocument::Derive(Spec, Movement);
		Source.MovementType = Movement.MovementType;
		Source.DriverSeatKind = Movement.SeatKind;
		Source.PassengerSeatKind = Movement.PassengerSeatKind;
		Source.MovementBlocks = FMantleVehicleChassisDocument::MovementBlocksJson(Spec);
	}

	/**
	 * A ground car's baked handling: the steering that eases off with speed and the front and rear grip split of the chassis cars, composed by the chassis document
	 * (ComposeGroundHandling) over the tyre friction already in Source's tuning, so a car built from a model drives as the same type's chassis does. Nothing for a
	 * chassis that is not a ground car on more than two wheels.
	 */
	void ApplyGroundHandling(const FMantleChassisSpec& Spec, UMantleVehicleSource& Source)
	{
		const TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
		const TSharedRef<FJsonObject> Tires = MakeShared<FJsonObject>();
		Tires->SetNumberField(TEXT("friction"), Source.Tuning.TireFriction);
		Document->SetObjectField(TEXT("tires"), Tires);
		Document->SetObjectField(TEXT("steering"), MakeShared<FJsonObject>());
		FMantleVehicleChassisDocument::ComposeGroundHandling(Spec, *Document);
		const TSharedPtr<FJsonObject>* Steering = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Curve = nullptr;
		if (Document->TryGetObjectField(TEXT("steering"), Steering) && Steering && (*Steering)->TryGetArrayField(TEXT("speedCurve"), Curve) && Curve)
		{
			Source.Modify();
			Source.Tuning.SteeringSpeedCurve.Reset();
			for (const TSharedPtr<FJsonValue>& Point : *Curve)
			{
				const TArray<TSharedPtr<FJsonValue>>* Pair = nullptr;
				if (Point.IsValid() && Point->TryGetArray(Pair) && Pair && Pair->Num() == 2)
				{
					Source.Tuning.SteeringSpeedCurve.Add(FVector2D((*Pair)[0]->AsNumber(), (*Pair)[1]->AsNumber()));
				}
			}
			double Rear = 0.0;
			double Front = 0.0;
			if (Document->TryGetObjectField(TEXT("tires"), Steering) && Steering && (*Steering)->TryGetNumberField(TEXT("rearFriction"), Rear) && (*Steering)->TryGetNumberField(TEXT("friction"), Front))
			{
				Source.Tuning.TireFriction = static_cast<float>(Front);
				Source.Tuning.bTireRearFriction = true;
				Source.Tuning.TireRearFriction = static_cast<float>(Rear);
			}
		}
	}

	FMantleVehicleBuildResult BuildChassisImpl(const FMantleChassisSpec& Spec, const FString& PackagePath, const FString& Name, const FMantleSkinnedMesh* ModelBody,
		const TArray<FSkeletalMaterial>* ModelMaterials, const TArray<FMantleChassisBox>* Collision, const FMantleVehicleBodyExtras* Extras,
		const TArray<FMantleVehicleSourcePart>* Parts = nullptr, const FMantleSkinnedMesh* PictureBody = nullptr, USkeletalMesh* PictureMesh = nullptr)
	{
		FMantleVehicleBuildResult Out;

		// The grey body and the document, exactly as a published or driven chassis is made (FMantleVehicleChassisBuilder::MakeAsset).
		FMantleVehicleAsset Asset;
		FString Error;
		if (!FMantleVehicleChassisBuilder::MakeAsset(Spec, Asset, Error))
		{
			Out.Problems.Add(FString::Printf(TEXT("These sizes don't make a vehicle (%s). Change a size and try again."), *FMantleVehicleShape::PlainReason(Error)));
			return Out;
		}
		USkeletalMesh* Mesh = FMantleVehicleChassisMesh::Create(ModelBody ? *ModelBody : Asset.Body, PackagePath, Name, Error, ModelMaterials, Extras);
		if (!Mesh)
		{
			Out.Problems.Add(FString::Printf(TEXT("Its body could not be built: %s."), *FMantleVehicleShape::PlainReason(Error)));
			return Out;
		}

		// The vehicle asset: one already there is updated, so a spawn in the level that names it follows the new sizes.
		const bool bScratch = FMantleVehicleChassisMesh::IsScratchPath(PackagePath);
		const FString SourceName = Name + TEXT("_Vehicle");
		const FString SourcePackageName = PackagePath / SourceName;
		const FString SourcePath = SourcePackageName + TEXT(".") + SourceName;
		UMantleVehicleSource* Source = FindObject<UMantleVehicleSource>(nullptr, *SourcePath);
		if (!Source && !bScratch)
		{
			Source = LoadObject<UMantleVehicleSource>(nullptr, *SourcePath, nullptr, LOAD_NoWarn | LOAD_Quiet);
		}
		const bool bNew = Source == nullptr;
		if (bNew)
		{
			UPackage* Package = CreatePackage(*SourcePackageName);
			Source = Package ? NewObject<UMantleVehicleSource>(Package, FName(*SourceName), RF_Public | RF_Standalone) : nullptr;
		}
		if (!Source)
		{
			Out.Problems.Add(FString::Printf(TEXT("The vehicle could not be created under %s."), *PackagePath));
			return Out;
		}
		Source->Modify();
		Source->Body = Mesh;
		if (Parts)
		{
			// The pieces split off a model with too many materials, or none: a build again does not keep the last one's.
			Source->Parts = *Parts;
		}
		// A shaped chassis's numbers are its own, so they are held to the document's limits and not to a preset's narrower ones.
		Source->Preset = Spec.Derived.IsSet() ? FName() : FName(*Spec.Id);
		Source->FitScale = 1.f;
		Source->Wheels.Reset();
		for (int32 Index = 0; Index < Spec.WheelCount(); ++Index)
		{
			const FMantleChassisAxle& Axle = Spec.Axles[Spec.AxleOf(Index)];
			FMantleVehicleSourceWheel& Wheel = Source->Wheels.AddDefaulted_GetRef();
			Wheel.Bone = Spec.WheelBone(Index);
			Wheel.bSteering = Axle.bSteer;
			Wheel.bDriven = Axle.bDrive;
			Wheel.bHandbrake = Axle.bHandbrake;
			Wheel.Radius = static_cast<float>(Spec.WheelRadius);
			Wheel.Width = static_cast<float>(Spec.WheelWidth);
		}

		// The handling: the chassis's document (its defaults, with the derived numbers over them when it was shaped), read as a preset's defaults.
		// The wheels, collision and seats of that document are the chassis's own, written below as Build writes a model's.
		Source->Tuning = FMantleVehicleSourceTuning();
		TSharedPtr<FJsonObject> Document;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FMantleChassisSpec::ComposeDocument(Spec)), Document) || !Document.IsValid())
		{
			Out.Problems.Add(TEXT("The vehicle's settings could not be worked out. Change a size and try again."));
			return Out;
		}
		// Also the movement part, which ApplyMovement puts in the vehicle's own fields: the type and its block, and the camera's boom.
		const TCHAR* const Written[] = { TEXT("schemaVersion"), TEXT("wheels"), TEXT("collision"), TEXT("seat"), TEXT("passengerSeat"), TEXT("movementType"), TEXT("flight"),
			TEXT("helicopter"), TEXT("hover") };
		for (const TCHAR* Key : Written)
		{
			Document->RemoveField(Key);
		}
		const TSharedPtr<FJsonObject>* Camera = nullptr;
		if (Document->TryGetObjectField(TEXT("camera"), Camera) && Camera && Camera->IsValid())
		{
			(*Camera)->RemoveField(TEXT("boomLocation"));
		}
		FMantleVehiclePreset Handling;
		Handling.Id = Spec.Id;
		Handling.Name = Spec.Id;
		Handling.Defaults = Document;
		TArray<FString> HandlingNotes;
		if (!FMantleVehiclePresets::ApplyDefaults(Handling, *Source, HandlingNotes))
		{
			Out.Problems.Add(TEXT("The vehicle's settings could not be applied."));
			Out.Problems.Append(HandlingNotes);
			return Out;
		}

		// Collision, centre of mass and seats: the chassis's own, at the size the Source is (Size 1, so the model's units are the chassis's). A creator's model brings a collision
		// shaped like itself: its first box (the one the driver sits in) is written as the chassis's box, the rest beside it, up to the document's box count.
		FMantleChassisPlacement Placement = FMantleVehicleChassisFit::Place(FMantleVehicleChassisBuilder::ReferenceOf(Spec), FMantleChassisStretch());
		int32 MaxBoxes = FMantleVehicleChassisFit::MaxBodyBoxes + 1;
		if (Collision && Collision->Num() > 0)
		{
			Placement.CollisionCenter = (*Collision)[0].Center;
			Placement.CollisionExtent = (*Collision)[0].Extent;
			Placement.BodyBoxes.Reset();
			for (int32 Index = 1; Index < Collision->Num(); ++Index)
			{
				Placement.BodyBoxes.Add((*Collision)[Index]);
			}
			MaxBoxes = FMantleVehicleLimits::MaxCollisionBoxes;
		}
		FMantleVehicleChassisFit::Apply(Placement, *Source, MaxBoxes);
		// A rider, a pilot and a hovercraft's driver sit as their seat kind says; the movement type, the seat kinds and the blocks go into the vehicle for the document.
		ApplyMovement(Spec, *Source);

		// The movement blocks must read into the document: ones that do not would be left out of it, and a bike would export, publish and ride without its rider block.
		FString MovementError;
		FMantleVehicleConverter::MakeDocument(*Source, TMap<FString, FString>(), &MovementError);
		if (!MovementError.IsEmpty())
		{
			Out.Problems.Add(FString::Printf(TEXT("The vehicle's movement settings can't be read (%s), so it would drive without them. Change a size and try again."), *FMantleVehicleShape::PlainReason(MovementError)));
			return Out;
		}

		// The library card's picture, taken now that the body is built: the vehicle as a player first sees it. Without one the card has no picture, which is said.
		FString ThumbnailHow;
		if (!FMantleVehicleThumbnail::Take(*Source, PictureMesh ? PictureMesh : Mesh, PictureBody ? *PictureBody : (ModelBody ? *ModelBody : Asset.Body), ThumbnailHow))
		{
			Out.Notes.Add(FString::Printf(TEXT("The vehicle's library picture could not be made (%s), so its library card will have no picture."), *ThumbnailHow));
		}

		Source->MarkPackageDirty();
		if (bNew && !bScratch)
		{
			FAssetRegistryModule::AssetCreated(Source);
		}
		Out.Source = Source;
		Out.bOk = true;
		Out.bSaved = FMantleVehicleBuilder::SaveVehicle(*Source, Out.SaveProblem);
		return Out;
	}
}

FMantleVehicleBuildResult FMantleVehicleBuilder::BuildChassis(const FMantleChassisSpec& Spec, const FString& PackagePath, const FString& Name)
{
	return MantleVehicleBuilderPrivate::BuildChassisImpl(Spec, PackagePath, Name, nullptr, nullptr, nullptr, nullptr);
}

FMantleVehicleBuildResult FMantleVehicleBuilder::BuildChassisWithBody(const FMantleChassisSpec& Spec, const FString& PackagePath, const FString& Name, const FMantleSkinnedMesh& Body,
	const TArray<FSkeletalMaterial>& Materials, const TArray<FMantleChassisBox>* Collision, const FMantleVehicleBodyExtras* Extras, const TArray<FMantleVehicleSourcePart>* Parts,
	const FMantleSkinnedMesh* PictureBody, USkeletalMesh* PictureMesh)
{
	return MantleVehicleBuilderPrivate::BuildChassisImpl(Spec, PackagePath, Name, &Body, &Materials, Collision, Extras, Parts, PictureBody, PictureMesh);
}

bool FMantleVehicleBuilder::FitsOwnWheels(UObject* Model, const FString& PresetId)
{
	const USkeletalMesh* Skeletal = Cast<USkeletalMesh>(Model);
	const FMantleVehiclePreset* Preset = FMantleVehiclePresets::Find(PresetId);
	if (!Skeletal || !Preset || Preset->MovementType != TEXT("wheeled"))
	{
		return false;
	}
	FString Text;
	FString Error;
	FMantleVehicleChassisReference Chassis;
	if (!FMantleVehicleChassisBuilder::LoadPresetText(PresetId, Text, Error) || !FMantleVehicleChassisBuilder::ReadReference(Text, Chassis, Error) || Chassis.bCentreline)
	{
		return false;
	}
	const TArray<FMantleVehicleWheelBone> Wheels = FMantleVehicleConverter::FindWheelBones(Skeletal->GetRefSkeleton());
	return Wheels.Num() >= FMantleVehicleLimits::MinWheels && Wheels.Num() <= FMantleVehicleLimits::MaxWheels
		&& Wheels.ContainsByPredicate([](const FMantleVehicleWheelBone& Wheel) { return Wheel.Axle == 0; });
}

FMantleVehicleBuildResult FMantleVehicleBuilder::BuildAny(UObject* Model, const FString& PresetId, const FMantleVehicleModelOptions& Options)
{
	if (FitsOwnWheels(Model, PresetId))
	{
		return Build(Cast<USkeletalMesh>(Model), PresetId);
	}
	return BuildFromModel(Model, PresetId, Options);
}

FMantleVehicleBuildResult FMantleVehicleBuilder::BuildFromModel(UObject* ModelAsset, const FString& PresetId, const FMantleVehicleModelOptions& Options)
{
	FMantleVehicleBuildResult Out;
	Out.bOnChassis = true;
	const FMantleVehiclePreset* Preset = FMantleVehiclePresets::Find(PresetId);
	if (!Preset)
	{
		Out.Problems.Add(TEXT("Pick a vehicle type first."));
	}
	if (!ModelAsset)
	{
		Out.Problems.Add(TEXT("Pick the model to use as the vehicle's body."));
	}
	if (!Preset || !ModelAsset)
	{
		return Out;
	}

	// One undo step for the whole build, as Build has, so Ctrl+Z puts back the vehicle a build again changed.
	const FScopedTransaction Transaction(NSLOCTEXT("MantleVehicleBuilder", "BuildVehicleFromModel", "Build vehicle from model"));

	FMantleVehicleModel Model;
	FString Error;
	if (!FMantleVehicleModel::Read(ModelAsset, Model, Error))
	{
		Out.Problems.Add(Error);
		return Out;
	}

	// The vehicle's name: the one asked for (to update that vehicle, or refused when it is somebody else's), else the first free one. Pressing Build again in the window
	// asks for the name it built under, so one vehicle stays one set of assets and not a new set for every press.
	FString Name = Options.Name;
	const FString Folder = Options.Folder.IsEmpty() ? ChassisFolder() : Options.Folder;
	if (Name.IsEmpty())
	{
		Name = FirstFreeName(Model.Name + TEXT("_") + Preset->Name, [&Folder](const FString& Candidate)
		{
			return FPackageName::DoesPackageExist(Folder / Candidate / (Candidate + TEXT("_Vehicle")));
		});
	}
	else if (!Options.bOverwrite)
	{
		const FString ExistingPackage = Folder / Name / (Name + TEXT("_Vehicle"));
		if (FPackageName::DoesPackageExist(ExistingPackage) || FindObject<UMantleVehicleSource>(nullptr, *(ExistingPackage + TEXT(".") + Name + TEXT("_Vehicle"))))
		{
			Out.Problems.Add(FString::Printf(TEXT("A vehicle called %s is already in %s, and this build did not make it. Pick another name, or build over it on purpose (the console takes overwrite=1)."), *Name, *Folder));
			return Out;
		}
	}
	FMantleChassisSpec Archetype;
	if (!FMantleVehicleChassisBuilder::LoadSpec(PresetId, Archetype, Error))
	{
		Out.Problems.Add(FString::Printf(TEXT("The %s chassis could not be read: %s."), *Preset->Name, *Error));
		return Out;
	}
	// A plane with two propellers is a twin: its chassis has a propeller bone for each.
	if (Options.Spec.IsSet())
	{
		Archetype = Options.Spec.GetValue();
	}
	else if (Options.TwinEngine.Get(FMantleVehicleChassisSkin::CountPropellers(Model) == 2))
	{
		Archetype = FMantleVehicleChassisSkin::AsTwin(Archetype);
		Out.Notes.Add(TEXT("Your plane has two propellers, so it is built with two engines."));
	}

	// The chassis's moving bones, from the grey chassis of the type (the same bones at any size).
	FMantleVehicleAsset Grey;
	if (!FMantleVehicleChassisBuilder::MakeAsset(Archetype, Grey, Error))
	{
		Out.Problems.Add(FString::Printf(TEXT("The %s chassis could not be made: %s."), *Preset->Name, *FMantleVehicleShape::PlainReason(Error)));
		return Out;
	}
	const TArray<FMantleVehiclePartSlot> Slots = FMantleVehicleChassisSkin::PartSlots(Archetype, Grey.Body);

	// Which way the model faces, and which of its parts are the chassis's wheels, rotors and surfaces: the creator's choice, else what its names and its wheels say.
	FMantleVehicleModelMap Map;
	double Yaw = 0.0;
	if (Options.Map.IsSet())
	{
		Map = Options.Map.GetValue();
		Yaw = Options.YawDegrees.Get(0.0);
	}
	else if (Options.YawDegrees.IsSet())
	{
		Yaw = Options.YawDegrees.GetValue();
		Map = FMantleVehicleChassisSkin::SuggestMap(Slots, Model, Yaw);
	}
	else
	{
		Yaw = FMantleVehicleChassisSkin::SuggestMapAndYaw(Slots, Model, Map);
	}

	// The chassis for the model: the window's own when it has shaped one, else the type's sized to the model's bounds.
	FMantleChassisSpec Spec;
	if (Options.Spec.IsSet())
	{
		Spec = Options.Spec.GetValue();
	}
	else
	{
		const FMantleVehicleModelProposal Proposal = FMantleVehicleChassisSkin::Propose(Archetype, *Preset, Model, Yaw, Map);
		FMantleVehicleShapeSession Session;
		Session.Start(Archetype, Preset->FitMinScale, Preset->FitMaxScale);
		if (Proposal.bUsesSize)
		{
			Session.SetSize(Proposal.Size);
		}
		for (const TPair<EMantleVehicleDimension, double>& Dimension : Proposal.Dimensions)
		{
			Session.SetDimension(Dimension.Key, Dimension.Value);
		}
		Spec = Session.GetShaped();
		Out.Notes.Append(Proposal.Notes);
		for (const FText& Held : Session.GetHeldNotes())
		{
			Out.Notes.Add(Held.ToString());
		}
	}

	// The chassis's own skeleton at that size, and the model on it.
	FMantleVehicleAsset Chassis;
	if (!FMantleVehicleChassisBuilder::MakeAsset(Spec, Chassis, Error))
	{
		Out.Problems.Add(FString::Printf(TEXT("These sizes don't make a vehicle (%s). Change a size and try again."), *FMantleVehicleShape::PlainReason(Error)));
		return Out;
	}
	const FMantleVehicleModelPlacement Placement = FMantleVehicleChassisSkin::FitPlacement(Spec, Chassis.Body, Model, Yaw, Map);
	if (!Placement.Note.IsEmpty())
	{
		// A model far from the vehicle's size is scaled only so far, or not at all when it is too small to measure: said, so the vehicle that looks wrong has a reason.
		Out.Notes.AddUnique(Placement.Note);
	}
	FMantleVehicleSkinned Skinned;
	if (!FMantleVehicleChassisSkin::Skin(Chassis.Body, Slots, Model, Placement, Map, Skinned, Error))
	{
		Out.Problems.Add(FString::Printf(TEXT("Your model could not be put on the %s: %s."), *Preset->Name, *Error));
		return Out;
	}
	Out.Notes.Append(Skinned.Notes);
	// What reading the model had to say (a Nanite mesh read from its source or from its fallback, and why).
	Out.Notes.Append(Model.Notes);
	if (Skinned.Extras.UVs.Num() > 0 || Skinned.Extras.Colors.Num() > 0)
	{
		// Kept on the vehicle's body in the editor; the published vehicle's mesh format carries one UV set and no vertex colours.
		const FString MoreUVs = Skinned.Extras.UVs.Num() > 0 ? FString::Printf(TEXT("%d more UV channel%s"), Skinned.Extras.UVs.Num(), Skinned.Extras.UVs.Num() == 1 ? TEXT("") : TEXT("s")) : FString();
		const FString Painted = Skinned.Extras.Colors.Num() > 0 ? (MoreUVs.IsEmpty() ? FString(TEXT("vertex colours")) : FString(TEXT(" and vertex colours"))) : FString();
		Out.Notes.Add(FString::Printf(TEXT("Your model has %s%s. They are kept on the vehicle in the editor, but a published vehicle carries only its first UV set and no vertex colours, so players see it without them: bake them into the textures if they matter."),
			*MoreUVs, *Painted));
	}

	// Collision shaped like the model: its own physics asset, else a plane's airframe split from its shape, else the chassis's box fitted to its hull.
	const FMantleVehicleSkinCollision Collision = FMantleVehicleChassisSkin::ProposeCollision(Spec, Skinned.Body, Slots, Model, Placement, Map);

	// A model that draws more materials than a published body may: the body keeps the ones with the most triangles and the rest are split off as parts of the
	// vehicle, each triangle with its material, its place and its bone, so the vehicle looks as the model does. The creator is told what was done, in plain words.
	TArray<FMantleVehicleSourcePart> SplitParts;
	FMantleSkinnedMesh PictureBody;
	TArray<FSkeletalMaterial> PictureMaterials;
	FMantleVehicleBodyExtras PictureExtras;
	const int32 ModelMaterials = FMantleVehicleBodySplit::CountMaterials(Skinned.Body);
	if (ModelMaterials > FMantleVehicleLimits::MaxSections)
	{
		PictureBody = Skinned.Body;
		PictureMaterials = Skinned.Materials;
		PictureExtras = Skinned.Extras;
		FMantleVehicleBodySplitResult Split;
		FString SplitProblem;
		if (!FMantleVehicleBodySplit::Split(Skinned.Body, Skinned.Materials, Skinned.Extras, FMantleVehicleLimits::MaxSections, FMantleVehicleLimits::MaxPartMaterials,
			FMantleVehicleLimits::MaxParts, Split))
		{
			SplitProblem = Split.Problem;
		}
		else
		{
			for (int32 Number = 0; Number < Split.Pieces.Num(); ++Number)
			{
				const FMantleVehicleBodyPiece& Piece = Split.Pieces[Number];
				UStaticMesh* PieceMesh = FMantleVehicleBodySplit::MakePieceMesh(Piece, Folder / Name, FMantleVehicleBodySplit::PieceAssetName(Name, Number + 1), SplitProblem);
				if (!PieceMesh)
				{
					break;
				}
				FMantleVehicleSourcePart& Part = SplitParts.AddDefaulted_GetRef();
				Part.Mesh = PieceMesh;
				Part.Bone = Piece.Bone;
				Part.Transform = Piece.ToBone;
				Part.FoundOn = TEXT("split off the model's extra materials");
			}
		}
		if (SplitProblem.IsEmpty())
		{
			Out.Notes.Add(FString::Printf(TEXT("Your model draws %d different materials, and a published vehicle's body may have %d, so Mantle split it for you: the body keeps the %d with the most triangles, and the other %d ride along as %d extra part%s saved beside it. It looks exactly as your model does and there is nothing for you to do."),
				ModelMaterials, FMantleVehicleLimits::MaxSections, Split.MaterialsAfter, Split.MaterialsBefore - Split.MaterialsAfter, SplitParts.Num(), SplitParts.Num() == 1 ? TEXT("") : TEXT("s")));
		}
		else
		{
			// Put the whole model back: a body with some of its materials gone and no parts to carry them is worse than one that says it is too big.
			Skinned.Body = PictureBody;
			Skinned.Materials = PictureMaterials;
			Skinned.Extras = PictureExtras;
			SplitParts.Reset();
			Out.Notes.Add(FString::Printf(TEXT("Your model draws %d different materials and a published vehicle's body may have %d. Mantle tried to carry the extra ones as separate parts, but %s. Merge some of its materials in your 3D tool, then build again."),
				ModelMaterials, FMantleVehicleLimits::MaxSections, *SplitProblem));
		}
	}
	// The library card's picture shows the whole model, not the body without its split-off parts.
	USkeletalMesh* PictureMesh = nullptr;
	if (SplitParts.Num() > 0)
	{
		FString PictureError;
		const FString PictureName = FString::Printf(TEXT("%sPicture%s"), *Name, *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
		PictureMesh = FMantleVehicleChassisMesh::Create(PictureBody, TEXT("/Temp/MantleVehiclePicture"), PictureName, PictureError, &PictureMaterials, &PictureExtras);
	}

	FMantleVehicleBuildResult Built = BuildChassisWithBody(Spec, Folder / Name, Name, Skinned.Body, Skinned.Materials, &Collision.Boxes, &Skinned.Extras, &SplitParts,
		SplitParts.Num() > 0 ? &PictureBody : nullptr, PictureMesh);
	if (PictureMesh)
	{
		// Only the picture needed it.
		if (USkeleton* PictureSkeleton = PictureMesh->GetSkeleton())
		{
			PictureSkeleton->ClearFlags(RF_Public | RF_Standalone);
			PictureSkeleton->MarkAsGarbage();
		}
		PictureMesh->ClearFlags(RF_Public | RF_Standalone);
		PictureMesh->MarkAsGarbage();
	}
	Out.Problems.Append(Built.Problems);
	Out.Notes.Append(Built.Notes);
	if (Built.bOk && !Collision.Note.IsEmpty())
	{
		Out.Notes.Add(Collision.Note);
	}
	Out.Source = Built.Source;
	Out.AssetName = Name;
	Out.bSaved = Built.bSaved;
	Out.SaveProblem = Built.SaveProblem;
	Out.bOk = Built.bOk;
	Out.ModelYaw = Yaw;
	Out.ModelMap = Map;
	if (!Out.bOk || !Out.Source)
	{
		return Out;
	}

	Out.Notes.Insert(FString::Printf(TEXT("Built on the %s chassis: Mantle sets the weight, collision, seats and handling. Your model (%s) is the look."), *Preset->Name, *Model.Describe()), 0);
	if (Map.IsEmpty() && Slots.Num() > 0)
	{
		Out.Notes.Add(TEXT("None of your model's parts are assigned to the chassis's moving parts, so it drives or flies as one solid piece. Assign its wheels, propellers, rotors or control surfaces to make them move."));
	}
	if (Spec.Aircraft.bSet || Spec.Helicopter.bSet)
	{
		Out.Notes.Add(TEXT("The pilot sits where the chassis puts them. If your model's cockpit is elsewhere, move Seat Location in the vehicle's settings (Show its settings)."));
	}
	// What publishing would refuse, said now: a model over the caps (triangles, materials) is built anyway.
	TArray<FMantleValidationIssue> Issues;
	FMantleVehicleConverter::CheckSource(Out.Source, FString(), Issues);
	for (const FMantleValidationIssue& Issue : Issues)
	{
		if (Issue.Severity == EMantleIssueSeverity::Error)
		{
			Out.Notes.Add(FString::Printf(TEXT("Before you publish: %s %s"), *Issue.Problem, *Issue.Remedy));
		}
	}
	return Out;
}

FString FMantleVehicleBuilder::ImportFolder()
{
	return ChassisFolder() / TEXT("Imported");
}

UObject* FMantleVehicleBuilder::ImportModelFile(const FString& FbxFile, FString& OutError, const FString& Folder, const bool bSave, TArray<UObject*>* OutAll)
{
	OutError.Reset();
	if (OutAll)
	{
		OutAll->Reset();
	}
	const FString File = FbxFile.TrimStartAndEnd();
	if (File.IsEmpty())
	{
		OutError = TEXT("Pick an .fbx file first.");
		return nullptr;
	}
	const FString Full = FPaths::ConvertRelativePathToFull(File);
	if (!FPaths::GetExtension(Full).Equals(TEXT("fbx"), ESearchCase::IgnoreCase))
	{
		OutError = FString::Printf(TEXT("%s isn't an .fbx file. Export your model from your 3D program as FBX and pick that file, or import it into your project and pick it from the list."), *FPaths::GetCleanFilename(Full));
		return nullptr;
	}
	if (!IFileManager::Get().FileExists(*Full))
	{
		OutError = FString::Printf(TEXT("Mantle can't find %s. Check that the file is still there, then pick it again."), *Full);
		return nullptr;
	}

	// A folder of the file's name, safe to save under: the same file again lands in the same folder and replaces its earlier import.
	const FString Destination = !Folder.IsEmpty() ? Folder : ImportFolder() / FirstFreeName(FPaths::GetBaseFilename(Full), [](const FString&) { return false; });
	if (!FPackageName::IsValidLongPackageName(Destination / TEXT("Check"), /*bIncludeReadOnlyRoots*/ false))
	{
		OutError = FString::Printf(TEXT("%s is not a folder the model can be imported into."), *Destination);
		return nullptr;
	}

	UAssetImportTask* Task = NewObject<UAssetImportTask>(GetTransientPackage());
	Task->Filename = Full;
	Task->DestinationPath = Destination;
	Task->bAutomated = true;
	Task->bReplaceExisting = true;
	Task->bSave = bSave;
	Task->bAsync = false;
	// The editor's own FBX importer, told to work out for itself whether the file is a static or a skinned mesh, and to leave animations and the physics asset out.
	UFbxFactory* Factory = NewObject<UFbxFactory>(Task);
	if (Factory->ImportUI)
	{
		Factory->ImportUI->bImportAnimations = false;
		Factory->ImportUI->bCreatePhysicsAsset = false;
		Factory->ImportUI->bAutomatedImportShouldDetectType = true;
	}
	Task->Factory = Factory;

	FAssetToolsModule::GetModule().Get().ImportAssetTasks({ Task });

	UObject* Skeletal = nullptr;
	UObject* Static = nullptr;
	for (UObject* Made : Task->GetObjects())
	{
		if (!Made)
		{
			continue;
		}
		if (OutAll)
		{
			OutAll->Add(Made);
		}
		if (!Skeletal && Made->IsA<USkeletalMesh>())
		{
			Skeletal = Made;
		}
		else if (!Static && Made->IsA<UStaticMesh>())
		{
			Static = Made;
		}
	}
	UObject* Model = Skeletal ? Skeletal : Static;
	if (!Model)
	{
		OutError = FString::Printf(TEXT("Unreal could not import a model from %s. It may have no mesh in it, or be a newer FBX than this editor reads; the Output Log says more. Export it again from your 3D program, or drag it into the Content Browser to import it there."),
			*FPaths::GetCleanFilename(Full));
		return nullptr;
	}
	return Model;
}

AMantleVehicleSpawnPoint* FMantleVehicleBuilder::FindSpawn(const UMantleVehicleSource& Source)
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		return nullptr;
	}
	const FSoftObjectPath Path(&Source);
	for (TActorIterator<AMantleVehicleSpawnPoint> It(World); It; ++It)
	{
		if (It->Vehicle.ToSoftObjectPath() == Path)
		{
			return *It;
		}
	}
	return nullptr;
}
