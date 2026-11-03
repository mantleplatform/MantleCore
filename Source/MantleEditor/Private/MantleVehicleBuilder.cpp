// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleBuilder.h"

#include "MantleChassisSpec.h"
#include "MantleVehicleBlueprint.h"
#include "MantleVehicleBodyReduce.h"
#include "MantlePlaceValidator.h"
#include "MantleVehicleBodySplit.h"
#include "MantleVehicleBuiltInMaterials.h"
#include "MantleVehicleLightFinder.h"
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
#include "Engine/Blueprint.h"
#include "Factories/FbxFactory.h"
#include "Factories/FbxImportUI.h"
#include "Factories/FbxStaticMeshImportData.h"
#include "IAssetTools.h"
#include "Editor.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Editor/EditorEngine.h"
#include "Engine/SkeletalMesh.h"
#include "Rendering/SkeletalMeshModel.h"
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
#include "UObject/MetaData.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

// Named, not anonymous: unity builds merge this file with others, and the test declares ApplyChosenPreset to reach it.
namespace MantleVehicleBuilderPrivate
{
	/**
	 * An Interchange pipeline stack for an import task: the engine's generic assets pipeline with its mesh pipeline set to combine all static meshes into one. Reached
	 * by class and property name, so Mantle links nothing of Interchange; null (the default import) when the classes are not in this engine.
	 */
	UObject* MakeCombineStaticMeshesStack(UObject* Outer)
	{
		UClass* StackClass = FindObject<UClass>(nullptr, TEXT("/Script/InterchangeEngine.InterchangePipelineStackOverride"));
		UClass* AssetsClass = FindObject<UClass>(nullptr, TEXT("/Script/InterchangePipelines.InterchangeGenericAssetsPipeline"));
		UFunction* AddPipeline = StackClass ? StackClass->FindFunctionByName(TEXT("AddPipeline")) : nullptr;
		if (!AssetsClass || !AddPipeline)
		{
			return nullptr;
		}
		UObject* Assets = NewObject<UObject>(GetTransientPackage(), AssetsClass);
		const FObjectProperty* MeshProperty = FindFProperty<FObjectProperty>(AssetsClass, TEXT("MeshPipeline"));
		UObject* Mesh = MeshProperty ? MeshProperty->GetObjectPropertyValue_InContainer(Assets) : nullptr;
		const FEnumProperty* Combine = Mesh ? FindFProperty<FEnumProperty>(Mesh->GetClass(), TEXT("CombineStaticMeshesBehavior")) : nullptr;
		if (!Combine)
		{
			return nullptr;
		}
		// "Combine All" is the enum's first value.
		Combine->GetUnderlyingProperty()->SetIntPropertyValue(Combine->ContainerPtrToValuePtr<void>(Mesh), static_cast<int64>(0));
		UObject* Stack = NewObject<UObject>(Outer, StackClass);
		struct FAddPipelineParams
		{
			UObject* Pipeline;
		} Params{ Assets };
		Stack->ProcessEvent(AddPipeline, &Params);
		return Stack;
	}

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

	// Defined with BuildChassisImpl below, which they are shared with.
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

	/** What a vehicle built on a chassis remembers about the model it was made from, in its package's metadata, so a build again of that model finds it. */
	const TCHAR* const BuiltFromModelKey = TEXT("MantleBuiltFromModel");

	UMantleVehicleSource* LoadVehicle(const FString& Folder, const FString& Name)
	{
		const FString AssetName = Name + TEXT("_Vehicle");
		const FString PackageName = Folder / Name / AssetName;
		const FString ObjectPath = PackageName + TEXT(".") + AssetName;
		UObject* Existing = StaticFindObject(UMantleVehicleSource::StaticClass(), nullptr, *ObjectPath);
		if (!Existing && FPackageName::DoesPackageExist(PackageName))
		{
			Existing = StaticLoadObject(UMantleVehicleSource::StaticClass(), nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
		}
		return Cast<UMantleVehicleSource>(Existing);
	}

	/**
	 * The name of the vehicle already built on a chassis from this model (the default name, or the default with _2, _3... after it), or empty. Built again over, so one
	 * model and type stay one vehicle however many times the window, a script or a new session builds it, and not a new set of assets each time.
	 */
	FString NameOfVehicleBuiltFrom(const FString& Folder, const FString& DefaultBase, const FString& ModelPath)
	{
		for (int32 Number = 1; Number < 100; ++Number)
		{
			const FString Candidate = Number == 1 ? DefaultBase : FString::Printf(TEXT("%s_%d"), *DefaultBase, Number);
			const FString PackageName = Folder / Candidate / (Candidate + TEXT("_Vehicle"));
			if (!FPackageName::DoesPackageExist(PackageName) && !FindPackage(nullptr, *PackageName))
			{
				break;
			}
			const UMantleVehicleSource* Existing = LoadVehicle(Folder, Candidate);
			const FString* From = Existing ? Existing->GetOutermost()->GetMetaData().FindValue(Existing, BuiltFromModelKey) : nullptr;
			if (From && *From == ModelPath)
			{
				return Candidate;
			}
		}
		return FString();
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

FMantleVehicleBuildResult FMantleVehicleBuilder::Build(USkeletalMesh* Mesh, const FString& PresetId, const FMantleVehicleSurroundings* Around)
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
	UMantleVehicleSource* Source = FMantleVehicleConverter::ConvertAsset(Mesh, Conversion, Around);
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
	// The converter's notes on what it read from the model's physics (collision, center of mass, where the driver sits) are left out:
	// the chassis replaces all of them, and the creator would be told to edit values that are not used.
	for (const FString& Note : Conversion.Notes)
	{
		if (!Note.StartsWith(TEXT("Collision:")) && !Note.StartsWith(TEXT("Center of mass:")) && !Note.StartsWith(TEXT("The driver sits behind the middle")))
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
			Out.Problems.Add(TEXT("Mantle could not find this model's wheels, so it cannot fit the vehicle chassis to it. Check that the model has a bone at the center of each wheel, named like Wheel_FL, Wheel_FR, Wheel_BL and Wheel_BR, with the car facing +X, and build again."));
			return Out;
		}
		if (Layout.bCenterline != Chassis.bCenterline)
		{
			Out.Problems.Add(Layout.bCenterline
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
			Source->FitScale = Chassis.bCenterline ? 1.f : static_cast<float>(Out.Fit->Scale);
			if (!Chassis.bCenterline)
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

		// The collision follows the model's visible body above the chassis tub; weight, center of mass and handling stay the chassis's.
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
				FMantleVehicleAsset Gray;
				if (!FMantleVehicleChassisBuilder::LoadSpec(Preset->Id, Archetype, Error) || !FMantleVehicleChassisBuilder::MakeAsset(Archetype, Gray, Error))
				{
					continue;
				}
				const TArray<FMantleVehiclePartSlot> Slots = FMantleVehicleChassisSkin::PartSlots(Archetype, Gray.Body);
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
								const uint8 Gray = static_cast<uint8>(FMath::Clamp(255.f * Shade, 0.f, 255.f));
								Image[Y * PanelWidth * Views + View * PanelWidth + X] = FColor(Gray, Gray, Gray, 255);
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
					const FColor Color = Name.StartsWith(TEXT("wheel")) || Name.StartsWith(TEXT("fork")) ? FColor(60, 230, 90, 255)
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
								Image[Y * PanelWidth * Views + View * PanelWidth + X] = Color;
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
	/** The take-off run a plane or a jet needs, in centimeters (the flight probes' rolls with some to spare: about 130 m for the jet, 340 m for the plane). */
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
	 * How much the ground varies along a run of RunCm from Start in the direction Direction: the highest sample minus the lowest, in centimeters. False when a sample
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

	// Drop it onto whatever is below that spot; over nothing, it hangs a meter under the camera.
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
			const int32 RunMeters = FMath::RoundToInt(RunCm / 100.0);
			if (BestDistance < 0.0)
			{
				*OutNote = FString::Printf(TEXT("A %s needs about %d m of flat, open ground in front of it to take off, and there is no ground that far ahead of you in this level. It is 6 m in front of you: move it to a runway or a flat field."),
					*Kind, RunMeters);
			}
			else
			{
				Location = BestGround + FVector(0.0, 0.0, 50.0);
				const int32 AheadMeters = FMath::RoundToInt(BestDistance / 100.0);
				*OutNote = BestVariation <= MantleVehicleBuilderPrivate::FlatEnoughCm
					? FString::Printf(TEXT("A %s needs about %d m of flat, open ground in front of it to take off. It is %d m in front of you, on a stretch that long and flat."), *Kind, RunMeters, AheadMeters)
					: FString::Printf(TEXT("A %s needs about %d m of flat, open ground in front of it to take off, and there is none near you. It is %d m in front of you, where the ground is flattest, but it varies by about %d m there: move it to a runway or a flat field."),
						*Kind, RunMeters, AheadMeters, FMath::Max(1, FMath::RoundToInt(BestVariation / 100.0)));
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
	 * (ComposeGroundHandling) over the tire friction already in Source's tuning, so a car built from a model drives as the same type's chassis does. Nothing for a
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

	FMantleVehicleBuildResult BuildChassisImpl(const FMantleChassisSpec& Spec, const FString& PackagePath, const FString& Name, const FMantleSkinnedMesh& ModelBody,
		const TArray<FSkeletalMaterial>* ModelMaterials, const TArray<FMantleChassisBox>* Collision, const FMantleVehicleBodyExtras* Extras,
		const TArray<FMantleVehicleSourcePart>* Parts = nullptr, const FMantleSkinnedMesh* PictureBody = nullptr, USkeletalMesh* PictureMesh = nullptr)
	{
		FMantleVehicleBuildResult Out;

		// The frame's document, exactly as a published or driven chassis is made (FMantleVehicleChassisBuilder::MakeAsset). Its own body is never used: the model is the body.
		FMantleVehicleAsset Asset;
		FString Error;
		if (!FMantleVehicleChassisBuilder::MakeAsset(Spec, Asset, Error))
		{
			Out.Problems.Add(FString::Printf(TEXT("These sizes don't make a vehicle (%s). Change a size and try again."), *FMantleVehicleShape::PlainReason(Error)));
			return Out;
		}
		USkeletalMesh* Mesh = FMantleVehicleChassisMesh::Create(ModelBody, PackagePath, Name, Error, ModelMaterials, Extras);
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

		// Collision, center of mass and seats: the chassis's own, at the size the Source is (Size 1, so the model's units are the chassis's). A creator's model brings a collision
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
		if (!FMantleVehicleThumbnail::Take(*Source, PictureMesh ? PictureMesh : Mesh, PictureBody ? *PictureBody : ModelBody, ThumbnailHow))
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

FMantleVehicleBuildResult FMantleVehicleBuilder::BuildChassisWithBody(const FMantleChassisSpec& Spec, const FString& PackagePath, const FString& Name, const FMantleSkinnedMesh& Body,
	const TArray<FSkeletalMaterial>& Materials, const TArray<FMantleChassisBox>* Collision, const FMantleVehicleBodyExtras* Extras, const TArray<FMantleVehicleSourcePart>* Parts,
	const FMantleSkinnedMesh* PictureBody, USkeletalMesh* PictureMesh)
{
	return MantleVehicleBuilderPrivate::BuildChassisImpl(Spec, PackagePath, Name, Body, &Materials, Collision, Extras, Parts, PictureBody, PictureMesh);
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
	if (!FMantleVehicleChassisBuilder::LoadPresetText(PresetId, Text, Error) || !FMantleVehicleChassisBuilder::ReadReference(Text, Chassis, Error) || Chassis.bCenterline)
	{
		return false;
	}
	// A model over the published limits (triangles, materials) is not used as it is: worn as a skin it is reduced and its extra materials are carried as parts,
	// where its own body would have to be refused.
	if (const FSkeletalMeshModel* Imported = Skeletal->GetImportedModel(); Imported && Imported->LODModels.Num() > 0)
	{
		int32 Triangles = 0;
		TSet<int32> Materials;
		for (const FSkelMeshSection& Section : Imported->LODModels[0].Sections)
		{
			Triangles += Section.NumTriangles;
			Materials.Add(Section.MaterialIndex);
		}
		if (Triangles > FMantleVehicleLimits::MaxTriangles || Materials.Num() > FMantleVehicleLimits::MaxSections)
		{
			return false;
		}
	}
	const TArray<FMantleVehicleWheelBone> Wheels = FMantleVehicleConverter::FindWheelBones(Skeletal->GetRefSkeleton());
	return Wheels.Num() >= FMantleVehicleLimits::MinWheels && Wheels.Num() <= FMantleVehicleLimits::MaxWheels
		&& Wheels.ContainsByPredicate([](const FMantleVehicleWheelBone& Wheel) { return Wheel.Axle == 0; });
}

FMantleVehicleBuildResult FMantleVehicleBuilder::BuildAny(UObject* Model, const FString& PresetId, const FMantleVehicleModelOptions& Options)
{
	// A vehicle Blueprint is read first: a Chaos vehicle gives its skeletal body and the wheels and parts set up on it, a Blueprint of static pieces gives one merged mesh.
	if (UBlueprint* Blueprint = Cast<UBlueprint>(Model))
	{
		FMantleVehicleBlueprintModel Source = FMantleVehicleBlueprint::Resolve(Blueprint);
		FMantleVehicleBuildResult Out;
		if (Source.Model)
		{
			const FMantleVehicleSurroundings* Around = Source.Around.IsSet() ? &Source.Around.GetValue() : nullptr;
			if (FitsOwnWheels(Source.Model, PresetId))
			{
				Out = Build(Cast<USkeletalMesh>(Source.Model), PresetId, Around);
				LightOwnBody(Out, Options);
			}
			else
			{
				Out = BuildFromModel(Source.Model, PresetId, Options);
			}
		}
		Out.Problems.Insert(Source.Problems, 0);
		Out.Notes.Insert(Source.Notes, 0);
		return Out;
	}
	if (FitsOwnWheels(Model, PresetId))
	{
		FMantleVehicleBuildResult Out = Build(Cast<USkeletalMesh>(Model), PresetId);
		LightOwnBody(Out, Options);
		return Out;
	}
	return BuildFromModel(Model, PresetId, Options);
}

void FMantleVehicleBuilder::LightOwnBody(FMantleVehicleBuildResult& Built, const FMantleVehicleModelOptions& Options)
{
	UMantleVehicleSource* Source = Built.Source;
	USkeletalMesh* Model = Source && Built.bOk ? Source->Body.LoadSynchronous() : nullptr;
	if (!Model)
	{
		return;
	}
	FMantleSkinnedMesh Body;
	TArray<FSkeletalMaterial> Materials;
	FString Error;
	if (!FMantleSkinnedMesh::ExtractFromSkeletalMesh(Model, Body, &Error, &Materials))
	{
		Built.Notes.Add(FString::Printf(TEXT("Mantle could not look for lights on your model (%s), so it has none yet. Pick its light sections in the vehicle builder's Seats and paint step."), *Error));
		return;
	}
	const FMantleVehicleLightGuess Guess = FMantleVehicleLights::Find(Body, Materials, false);
	FMantleVehicleSourceLights Found;
	FMantleVehicleLights::Cut(Body, Materials, Guess, Found);
	bool bCut = false;
	for (int32 Kind = 0; Kind < FMantleVehicleLights::NumKinds; ++Kind)
	{
		bCut = bCut || Guess.Triangles[Kind].Num() > 0;
	}
	if (bCut)
	{
		// The light spots are cut out of a copy of the model, saved beside the vehicle; the model itself is never changed.
		const FString Folder = FPackageName::GetLongPackagePath(Source->GetOutermost()->GetName());
		USkeletalMesh* Lit = FMantleVehicleChassisMesh::Create(Body, Folder, Model->GetName() + TEXT("_Lights"), Error, &Materials);
		if (Lit)
		{
			Source->Modify();
			Source->Body = Lit;
		}
		else
		{
			Built.Notes.Add(FString::Printf(TEXT("Mantle found lights on your model but could not save the copy of it that has them (%s). Its lights that are whole materials still work."), *Error));
		}
	}
	FMantleVehicleSourceLights Lights = Found;
	if (Options.Lights.IsSet())
	{
		Lights = Options.Lights.GetValue();
		// The creator placed the beams on the picture, the model worn on the type's frame: they move to the same place on the model as it is built.
		const FBox3f Own = Body.ComputeBounds(0);
		if (Options.LightsBounds.IsValid && Own.IsValid)
		{
			const FVector From = FVector(Options.LightsBounds.GetCenter());
			const FVector To = FVector(Own.GetCenter());
			const FVector Stretch = FVector(Own.GetExtent()) / FVector(Options.LightsBounds.GetExtent()).ComponentMax(FVector(1.0));
			for (FMantleVehicleSourceLightBeam& Beam : Lights.Beams)
			{
				Beam.Position = To + (Beam.Position - From) * Stretch;
			}
		}
	}
	ApplyLights(*Source, Lights, Built.Notes);
	if (Built.bSaved)
	{
		Built.bSaved = SaveVehicle(*Source, Built.SaveProblem);
	}
}

bool FMantleVehicleBuilder::PrepareFromModel(UObject* ModelAsset, const FString& PresetId, const FMantleVehicleModelOptions& Options, FMantleVehicleOnChassis& Out, TArray<FString>& Problems)
{
	const FMantleVehiclePreset* Preset = FMantleVehiclePresets::Find(PresetId);
	if (!Preset)
	{
		Problems.Add(TEXT("Pick a vehicle type first."));
	}
	if (!ModelAsset)
	{
		Problems.Add(TEXT("Pick the model to use as the vehicle's body."));
	}
	if (!Preset || !ModelAsset)
	{
		return false;
	}
	Out.PresetName = Preset->Name;
	FMantleVehicleModel& Model = Out.Model;
	FString Error;
	if (!FMantleVehicleModel::Read(ModelAsset, Model, Error))
	{
		Problems.Add(Error);
		return false;
	}

	FMantleChassisSpec Archetype;
	if (!FMantleVehicleChassisBuilder::LoadSpec(PresetId, Archetype, Error))
	{
		Problems.Add(FString::Printf(TEXT("The %s chassis could not be read: %s."), *Preset->Name, *Error));
		return false;
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

	// The chassis's moving bones, from the gray chassis of the type (the same bones at any size).
	FMantleVehicleAsset Gray;
	if (!FMantleVehicleChassisBuilder::MakeAsset(Archetype, Gray, Error))
	{
		Problems.Add(FString::Printf(TEXT("The %s chassis could not be made: %s."), *Preset->Name, *FMantleVehicleShape::PlainReason(Error)));
		return false;
	}
	TArray<FMantleVehiclePartSlot>& Slots = Out.Slots; Slots = FMantleVehicleChassisSkin::PartSlots(Archetype, Gray.Body);

	// Which way the model faces, and which of its parts are the chassis's wheels, rotors and surfaces: the creator's choice, else what its names and its wheels say.
	FMantleVehicleModelMap& Map = Out.Map;
	double& Yaw = Out.Yaw;
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
	FMantleChassisSpec& Spec = Out.Spec;
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
	FMantleVehicleAsset& Chassis = Out.Chassis;
	if (!FMantleVehicleChassisBuilder::MakeAsset(Spec, Chassis, Error))
	{
		Problems.Add(FString::Printf(TEXT("These sizes don't make a vehicle (%s). Change a size and try again."), *FMantleVehicleShape::PlainReason(Error)));
		return false;
	}
	const FMantleVehicleModelPlacement& Placement = Out.Placement =  FMantleVehicleChassisSkin::FitPlacement(Spec, Chassis.Body, Model, Yaw, Map);
	if (!Placement.Note.IsEmpty())
	{
		// A model far from the vehicle's size is scaled only so far, or not at all when it is too small to measure: said, so the vehicle that looks wrong has a reason.
		Out.Notes.AddUnique(Placement.Note);
	}
	FMantleVehicleSkinned& Skinned = Out.Skinned;
	if (!FMantleVehicleChassisSkin::Skin(Chassis.Body, Slots, Model, Placement, Map, Skinned, Error))
	{
		Problems.Add(FString::Printf(TEXT("Your model could not be put on the %s: %s."), *Preset->Name, *Error));
		return false;
	}
	Out.Notes.Append(Skinned.Notes);
	// The lights: the spots of the model that glow (or are named for a light) are cut into sections of their own, so the game can switch each one.
	const FMantleVehicleLightGuess LightGuess = FMantleVehicleLights::Find(Skinned.Body, Skinned.Materials, Spec.bCenterline);
	FMantleVehicleLights::Cut(Skinned.Body, Skinned.Materials, LightGuess, Out.Lights);
	// What reading the model had to say (a Nanite mesh read from its source or from its fallback, and why).
	Out.Notes.Append(Model.Notes);
	if (Skinned.Extras.UVs.Num() > 0 || Skinned.Extras.Colors.Num() > 0)
	{
		// Kept on the vehicle's body in the editor; the published vehicle's mesh format carries one UV set and no vertex colors.
		const FString MoreUVs = Skinned.Extras.UVs.Num() > 0 ? FString::Printf(TEXT("%d more UV channel%s"), Skinned.Extras.UVs.Num(), Skinned.Extras.UVs.Num() == 1 ? TEXT("") : TEXT("s")) : FString();
		const FString Painted = Skinned.Extras.Colors.Num() > 0 ? (MoreUVs.IsEmpty() ? FString(TEXT("vertex colors")) : FString(TEXT(" and vertex colors"))) : FString();
		Out.Notes.Add(FString::Printf(TEXT("Your model has %s%s. They are kept on the vehicle in the editor, but a published vehicle carries only its first UV set and no vertex colors, so players see it without them: bake them into the textures if they matter."),
			*MoreUVs, *Painted));
	}

	// Collision shaped like the model: its own physics asset, else a plane's airframe split from its shape, else the chassis's box fitted to its hull.
	Out.Collision = FMantleVehicleChassisSkin::ProposeCollision(Spec, Skinned.Body, Slots, Model, Placement, Map);
	return true;
}

FMantleVehicleBuildResult FMantleVehicleBuilder::BuildFromModel(UObject* ModelAsset, const FString& PresetId, const FMantleVehicleModelOptions& Options)
{
	FMantleVehicleBuildResult Out;
	Out.bOnChassis = true;
	// One undo step for the whole build, as Build has, so Ctrl+Z puts back the vehicle a build again changed.
	const FScopedTransaction Transaction(NSLOCTEXT("MantleVehicleBuilder", "BuildVehicleFromModel", "Build vehicle from model"));

	// How the model sits on the vehicle: the same work the window's picture does (MakeModelPreview), so what the creator saw is what is built.
	FMantleVehicleOnChassis Prepared;
	if (!PrepareFromModel(ModelAsset, PresetId, Options, Prepared, Out.Problems))
	{
		return Out;
	}
	const FMantleVehiclePreset* Preset = FMantleVehiclePresets::Find(PresetId);
	const FMantleVehicleModel& Model = Prepared.Model;
	const FMantleChassisSpec& Spec = Prepared.Spec;
	const TArray<FMantleVehiclePartSlot>& Slots = Prepared.Slots;
	const FMantleVehicleModelMap& Map = Prepared.Map;
	const double Yaw = Prepared.Yaw;
	FMantleVehicleSkinned& Skinned = Prepared.Skinned;
	const FMantleVehicleSkinCollision& Collision = Prepared.Collision;
	Out.Notes = Prepared.Notes;
	FString Error;

	// The vehicle's name: the one asked for (to update that vehicle, or refused when it is somebody else's), else the first free one. Pressing Build again in the window
	// asks for the name it built under, so one vehicle stays one set of assets and not a new set for every press.
	FString Name = Options.Name;
	const FString Folder = Options.Folder.IsEmpty() ? ChassisFolder() : Options.Folder;
	const FString ModelPath = ModelAsset->GetPathName();
	if (Name.IsEmpty())
	{
		const auto IsTaken = [&Folder](const FString& Candidate)
		{
			return FPackageName::DoesPackageExist(Folder / Candidate / (Candidate + TEXT("_Vehicle")));
		};
		const FString Fresh = FirstFreeName(Model.Name + TEXT("_") + Preset->Name, IsTaken);
		// The same model built again, in a new window or by a script, is the vehicle it already made, updated.
		const FString Earlier = MantleVehicleBuilderPrivate::NameOfVehicleBuiltFrom(Folder, FirstFreeName(Model.Name + TEXT("_") + Preset->Name, [](const FString&) { return false; }), ModelPath);
		Name = Earlier.IsEmpty() ? Fresh : Earlier;
		if (!Earlier.IsEmpty())
		{
			Out.Notes.Add(FString::Printf(TEXT("You built this model before, so Mantle updated that vehicle (%s) and did not make a second one."), *Earlier));
		}
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
	// The whole car is held to the vehicle's triangle limit, near: the body and every part it will carry together (tires, doors, seats and the rest are all in the model
	// here), so a model over it is reduced for the creator as one (the model they made is not touched), and told so. The body's materials are split into parts after, the
	// triangles unchanged, so the body and its parts together are within the limit too. A far-away version of each is made on top of this and adds nothing near.
	bool bReductionFailed = false;
	{
		FMantleVehicleBodyReduceResult Reduced;
		const int32 Limit = FMantleVehicleLimits::MaxTriangles;
		if (FMantleVehicleBodyReduce::CountTriangles(Skinned.Body) > Limit)
		{
			if (FMantleVehicleBodyReduce::ReduceToFit(Skinned.Body, Skinned.Extras, Limit, Reduced))
			{
				Out.bBodyReduced = Reduced.bReduced;
				Out.Notes.Add(FString::Printf(TEXT("Mantle simplified your car from %d to %d triangles so it runs well on every server. Every material, part and wheel is where it was, your lights and paint still work, and your own model was not changed. There is nothing for you to do."),
					Reduced.Before, Reduced.After));
			}
			else
			{
				bReductionFailed = true;
				Out.Notes.Add(FString::Printf(TEXT("Your model has %d triangles, and a published vehicle may have %d. Mantle tried to simplify it for you, but %s. Use a lower-detail version of the model, then build again."),
					Reduced.Before, Limit, *Reduced.Problem));
			}
		}
	}
	// A model that draws more materials than a published body may: the body keeps the ones with the most triangles and the rest are split off as parts of the
	// vehicle, each triangle with its material, its place and its bone, so the vehicle looks as the model does. The creator is told what was done, in plain words.
	TArray<FMantleVehicleSourcePart> SplitParts;
	int32 SplitPartTriangles = 0;
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
		// The light sections stay on the body, where the game switches them.
		const FMantleVehicleSourceLights& LightsWanted = Options.Lights.IsSet() ? Options.Lights.GetValue() : Prepared.Lights;
		const TArray<FString> SectionsBefore = FMantleVehicleLights::SectionNames(Skinned.Materials);
		TSet<int32> LightSlots;
		for (int32 Slot = 0; Slot < SectionsBefore.Num(); ++Slot)
		{
			for (const EMantleLightKind Kind : { EMantleLightKind::Head, EMantleLightKind::Rear, EMantleLightKind::Reverse,
				EMantleLightKind::Left, EMantleLightKind::Right })
			{
				if (FMantleVehicleLights::Sections(LightsWanted, Kind).Contains(SectionsBefore[Slot]))
				{
					LightSlots.Add(Slot);
				}
			}
		}
		if (!FMantleVehicleBodySplit::Split(Skinned.Body, Skinned.Materials, Skinned.Extras, FMantleVehicleLimits::MaxSections, FMantleVehicleLimits::MaxPartMaterials,
			FMantleVehicleLimits::MaxParts, Split, LightSlots))
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
				SplitPartTriangles += Piece.NumTriangles();
				FMantleVehicleSourcePart& Part = SplitParts.AddDefaulted_GetRef();
				Part.Mesh = PieceMesh;
				Part.Bone = Piece.Bone;
				Part.Transform = Piece.ToBone;
				Part.FoundOn = FMantleVehicleBodySplit::FoundOn();
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
			SplitPartTriangles = 0;
			Out.Notes.Add(FString::Printf(TEXT("Your model draws %d different materials and a published vehicle's body may have %d. Mantle tried to carry the extra ones as separate parts, but %s. Merge some of its materials in your 3D tool, then build again."),
				ModelMaterials, FMantleVehicleLimits::MaxSections, *SplitProblem));
		}
	}
	// What the car draws near, body and parts together: the number the limit is about. The reduction above brings it within the limit when the model can be reduced.
	{
		const int32 BodyTriangles = FMantleVehicleBodyReduce::CountTriangles(Skinned.Body);
		UE_LOG(LogTemp, Log, TEXT("MANTLE_VEHICLE_CAR_TRIANGLES %s body=%d parts=%d total=%d limit=%d"), *Name, BodyTriangles, SplitPartTriangles,
			BodyTriangles + SplitPartTriangles, FMantleVehicleLimits::MaxTriangles);
		if (BodyTriangles + SplitPartTriangles > FMantleVehicleLimits::MaxTriangles && !bReductionFailed)
		{
			Out.Notes.Add(FString::Printf(TEXT("Your car draws %d triangles up close and a published vehicle may have %d. Use a lower-detail version of the model, then build again."),
				BodyTriangles + SplitPartTriangles, FMantleVehicleLimits::MaxTriangles));
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
	// Remembered with the vehicle, so a build again of this model finds it.
	Out.Source->GetOutermost()->GetMetaData().SetValue(Out.Source, MantleVehicleBuilderPrivate::BuiltFromModelKey, *ModelPath);
	// A model that was split into a body and parts: the paint the creator ticked (else what looks like paint) goes onto whichever holds each painted material.
	if (SplitParts.Num() > 0)
	{
		const TArray<FString> ModelSections = FMantleVehicleLights::SectionNames(PictureMaterials);
		const FMantleVehicleSourceLights& LightsForPaint = Options.Lights.IsSet() ? Options.Lights.GetValue() : Prepared.Lights;
		TArray<bool> Painted;
		for (int32 Index = 0; Index < ModelSections.Num(); ++Index)
		{
			if (Options.PaintedSections.IsSet())
			{
				Painted.Add(Options.PaintedSections.GetValue().ContainsByPredicate([&ModelSections, Index](const FString& Ticked) { return Ticked.Equals(ModelSections[Index], ESearchCase::IgnoreCase); }));
				continue;
			}
			bool bLight = false;
			for (int32 Kind = 0; Kind < FMantleVehicleLights::NumKinds; ++Kind)
			{
				bLight = bLight || FMantleVehicleLights::Sections(LightsForPaint, static_cast<EMantleLightKind>(Kind)).Contains(ModelSections[Index]);
			}
			Painted.Add(!bLight && PictureMaterials.IsValidIndex(Index) && FMantleVehicleConverter::LooksLikePaint(PictureMaterials[Index].MaterialInterface));
		}
		ApplyPaint(*Out.Source, ModelSections, Painted, Out.Notes);
	}
	// The lights: the creator's rows and beams, else what the finder found; each light section gets its glowing copy of its material.
	ApplyLights(*Out.Source, Options.Lights.IsSet() ? Options.Lights.GetValue() : Prepared.Lights, Out.Notes);
	// Said, not asked: glass, tires, chrome and lamps are Mantle's own materials when it is published (FMantleVehicleBuiltInMaterials).
	const FString BuiltIns = FMantleVehicleBuiltInMaterials::Describe(*Out.Source);
	if (!BuiltIns.IsEmpty())
	{
		Out.Notes.Add(BuiltIns);
	}
	if (Out.bSaved)
	{
		Out.bSaved = SaveVehicle(*Out.Source, Out.SaveProblem);
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

void FMantleVehicleBuilder::ApplyLights(UMantleVehicleSource& Source, const FMantleVehicleSourceLights& Lights, TArray<FString>& OutNotes)
{
	// Only sections the body has: a section split off as a part (a body over the material cap) is not one the game can switch.
	const USkeletalMesh* Mesh = Source.Body.LoadSynchronous();
	const TArray<FString> Sections = Mesh ? FMantleVehicleLights::SectionNames(Mesh->GetMaterials()) : TArray<FString>();
	FMantleVehicleSourceLights Kept = Lights;
	for (const EMantleLightKind Kind : { EMantleLightKind::Head, EMantleLightKind::Rear, EMantleLightKind::Reverse,
		EMantleLightKind::Left, EMantleLightKind::Right })
	{
		TArray<FString>& List = FMantleVehicleLights::Sections(Kept, Kind);
		List.RemoveAll([&Sections](const FString& Section) { return !Sections.Contains(Section); });
	}
	Source.Modify();
	Source.Lights = Kept;
	FMantleVehicleLights::ApplyMaterials(Source, OutNotes);
	Source.MarkPackageDirty();
}

void FMantleVehicleBuilder::ApplyPaint(UMantleVehicleSource& Source, const TArray<FString>& AllSections, const TArray<bool>& Painted, TArray<FString>& OutNotes)
{
	const auto IsPainted = [&AllSections, &Painted](const FString& Section)
	{
		const int32 Index = AllSections.IndexOfByPredicate([&Section](const FString& Other) { return Other.Equals(Section, ESearchCase::IgnoreCase); });
		return Index != INDEX_NONE && Painted.IsValidIndex(Index) && Painted[Index];
	};
	Source.Modify();

	// The body: every ticked section, as the paint step always set them; on a car that was split, only the ticked ones the body still draws (the rest are on parts).
	const bool bSplit = Source.Parts.ContainsByPredicate([](const FMantleVehicleSourcePart& Part) { return Part.FoundOn == FMantleVehicleBodySplit::FoundOn(); });
	const USkeletalMesh* Mesh = Source.Body.LoadSynchronous();
	const TArray<FString> BodySections = Mesh ? FMantleVehicleLights::SectionNames(Mesh->GetMaterials()) : TArray<FString>();
	Source.Paint.Sections.Reset();
	for (int32 Index = 0; Index < AllSections.Num(); ++Index)
	{
		if (Painted.IsValidIndex(Index) && Painted[Index] && (!bSplit || BodySections.ContainsByPredicate([&AllSections, Index](const FString& Section) { return Section.Equals(AllSections[Index], ESearchCase::IgnoreCase); })))
		{
			Source.Paint.Sections.Add(AllSections[Index]);
		}
	}

	// The parts the split made: a slot is painted when the section of its name is ticked. A slot the split renamed (material_<N>, for a name it could not keep) is the
	// model's Nth section.
	TArray<FString> OnParts;
	for (FMantleVehicleSourcePart& Part : Source.Parts)
	{
		if (Part.FoundOn != FMantleVehicleBodySplit::FoundOn())
		{
			continue;
		}
		const UStaticMesh* PartMesh = Part.Mesh.LoadSynchronous();
		if (!PartMesh)
		{
			continue;
		}
		Part.PaintSlots.Reset();
		const TArray<FStaticMaterial>& Slots = PartMesh->GetStaticMaterials();
		for (int32 Slot = 0; Slot < Slots.Num(); ++Slot)
		{
			const FString Name = Slots[Slot].MaterialSlotName.ToString();
			bool bPainted = IsPainted(Name);
			int32 Renamed = INDEX_NONE;
			if (!bPainted && Name.StartsWith(TEXT("material_")) && Name.RightChop(9).IsNumeric())
			{
				Renamed = FCString::Atoi(*Name.RightChop(9));
				bPainted = AllSections.IsValidIndex(Renamed) && Painted.IsValidIndex(Renamed) && Painted[Renamed];
			}
			if (bPainted)
			{
				Part.PaintSlots.Add(Slot);
				OnParts.AddUnique(Name);
			}
		}
	}
	if (OnParts.Num() > 0)
	{
		OutNotes.Add(FString::Printf(TEXT("Paint: %s %s ticked and ride%s on extra parts, so those parts take the color and liveries too."), *FString::Join(OnParts, TEXT(", ")),
			OnParts.Num() == 1 ? TEXT("is") : TEXT("are"), OnParts.Num() == 1 ? TEXT("s") : TEXT("")));
	}
	Source.MarkPackageDirty();
}

void FMantleVehicleModelPreview::Discard()
{
	if (USkeletalMesh* Picture = Mesh.Get())
	{
		// Only the picture needed them: never saved, never registered.
		if (USkeleton* Skeleton = Picture->GetSkeleton())
		{
			Skeleton->ClearFlags(RF_Public | RF_Standalone);
			Skeleton->MarkAsGarbage();
		}
		Picture->ClearFlags(RF_Public | RF_Standalone);
		Picture->MarkAsGarbage();
	}
	Mesh.Reset();
	Body = FMantleSkinnedMesh();
	WheelDots.Reset();
	bOk = false;
}

bool FMantleVehicleBuilder::MakeModelPreview(UObject* ModelAsset, const FString& PresetId, const FMantleVehicleModelOptions& Options, FMantleVehicleModelPreview& Out)
{
	Out.Discard();
	Out.Notes.Reset();
	Out.Problems.Reset();
	FMantleVehicleOnChassis Prepared;
	if (!PrepareFromModel(ModelAsset, PresetId, Options, Prepared, Out.Problems))
	{
		return false;
	}
	FString Error;
	const FString PictureName = FString::Printf(TEXT("Preview%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	USkeletalMesh* Picture = FMantleVehicleChassisMesh::Create(Prepared.Skinned.Body, TEXT("/Temp/MantleVehiclePreview"), PictureName, Error, &Prepared.Skinned.Materials, &Prepared.Skinned.Extras);
	if (!Picture)
	{
		Out.Problems.Add(FString::Printf(TEXT("The picture of your model on the %s could not be made: %s."), *Prepared.PresetName, *FMantleVehicleShape::PlainReason(Error)));
		return false;
	}
	Out.Mesh.Reset(Picture);
	Out.Body = Prepared.Skinned.Body;
	Out.WheelDots = Prepared.Chassis.WheelPositions;
	Out.Spec = Prepared.Spec;
	Out.Map = Prepared.Map;
	Out.Yaw = Prepared.Yaw;
	Out.Lights = Prepared.Lights;
	Out.Sections = FMantleVehicleLights::SectionNames(Prepared.Skinned.Materials);
	Out.Notes = Prepared.Notes;
	Out.bOk = true;
	return true;
}

bool FMantleVehicleBuilder::PreviewMatchesBuild(const FMantleVehicleModelPreview& Preview, const FMantleVehicleBuildResult& Built, FString& Why)
{
	if (!Preview.bOk || !Built.bOk || !Built.Source)
	{
		Why = TEXT("the preview or the build did not succeed");
		return false;
	}
	FMantleSkinnedMesh BuiltBody;
	FString Error;
	if (!FMantleSkinnedMesh::ExtractFromSkeletalMesh(Built.Source->Body.LoadSynchronous(), BuiltBody, &Error) || BuiltBody.LODs.Num() == 0 || !BuiltBody.HasReferencePose())
	{
		Why = FString::Printf(TEXT("the built body could not be read: %s"), *Error);
		return false;
	}
	TArray<FTransform> Component;
	for (int32 Bone = 0; Bone < BuiltBody.Bones.Num(); ++Bone)
	{
		const int32 Parent = BuiltBody.Bones[Bone].ParentIndex;
		Component.Add(Parent >= 0 ? BuiltBody.ReferencePose[Bone] * Component[Parent] : BuiltBody.ReferencePose[Bone]);
	}
	if (Built.Source->Wheels.Num() != Preview.WheelDots.Num())
	{
		Why = FString::Printf(TEXT("the build has %d wheels and the picture %d dots"), Built.Source->Wheels.Num(), Preview.WheelDots.Num());
		return false;
	}
	for (int32 Index = 0; Index < Preview.WheelDots.Num(); ++Index)
	{
		const FName WheelBone = Built.Source->Wheels[Index].Bone;
		const int32 Bone = BuiltBody.Bones.IndexOfByPredicate([WheelBone](const FMantleSkinnedMeshBone& B) { return B.Name == WheelBone; });
		if (Bone == INDEX_NONE)
		{
			Why = FString::Printf(TEXT("the built body has no bone %s"), *WheelBone.ToString());
			return false;
		}
		const double Off = FVector::Dist(Component[Bone].GetLocation(), Preview.WheelDots[Index]);
		if (Off > 0.05)
		{
			Why = FString::Printf(TEXT("wheel %d: the dot is %.2f cm from the built wheel"), Index + 1, Off);
			return false;
		}
	}
	if (Built.Source->Parts.Num() == 0 && !Built.bBodyReduced)
	{
		// Both are read back from a mesh the editor built (it welds vertices and drops degenerate triangles), so they are compared the same way.
		FMantleSkinnedMesh PictureBody;
		if (!FMantleSkinnedMesh::ExtractFromSkeletalMesh(Preview.Mesh.Get(), PictureBody, &Error) || PictureBody.LODs.Num() == 0)
		{
			Why = FString::Printf(TEXT("the picture's body could not be read: %s"), *Error);
			return false;
		}
		if (PictureBody.LODs[0].Positions.Num() != BuiltBody.LODs[0].Positions.Num() || PictureBody.LODs[0].Indices.Num() != BuiltBody.LODs[0].Indices.Num())
		{
			Why = FString::Printf(TEXT("the picture's body has %d triangles and the built body %d"), PictureBody.LODs[0].Indices.Num() / 3, BuiltBody.LODs[0].Indices.Num() / 3);
			return false;
		}
		const FBox3f A = PictureBody.ComputeBounds(0);
		const FBox3f B = BuiltBody.ComputeBounds(0);
		if (!A.Min.Equals(B.Min, 0.05f) || !A.Max.Equals(B.Max, 0.05f))
		{
			Why = TEXT("the picture's body and the built body are not the same size or in the same place");
			return false;
		}
	}
	return true;
}

namespace MantleVehicleBuilderPrivate
{
	// Mantle.Vehicle.PreviewCheck: the window's picture and the real build of one model, compared.
	FAutoConsoleCommand PreviewCheckCommand(
		TEXT("Mantle.Vehicle.PreviewCheck"),
		TEXT("Mantle.Vehicle.PreviewCheck <model asset path> <vehicle type> -- makes the picture of the model on the type's frame (MakeModelPreview) and the build (BuildFromModel, in a scratch package), compares them and logs MANTLE_VEHICLE_PREVIEW_CHECK."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() < 2)
			{
				UE_LOG(LogTemp, Error, TEXT("MANTLE_VEHICLE_PREVIEW_CHECK ok=0 usage: Mantle.Vehicle.PreviewCheck <model asset path> <vehicle type>"));
				return;
			}
			UObject* Asset = LoadObject<UObject>(nullptr, *Args[0], nullptr, LOAD_NoWarn | LOAD_Quiet);
			FMantleVehicleModelOptions Options;
			Options.Folder = TEXT("/Temp/MantleVehiclePreviewCheck");
			Options.Name = TEXT("PreviewCheck");
			Options.bOverwrite = true;
			FMantleVehicleModelPreview Preview;
			const bool bPreview = FMantleVehicleBuilder::MakeModelPreview(Asset, Args[1], Options, Preview);
			const FMantleVehicleBuildResult Built = FMantleVehicleBuilder::BuildFromModel(Asset, Args[1], Options);
			FString Why;
			const bool bSame = bPreview && FMantleVehicleBuilder::PreviewMatchesBuild(Preview, Built, Why);
			UE_LOG(LogTemp, Display, TEXT("MANTLE_VEHICLE_PREVIEW_CHECK ok=%d preview=%d built=%d dots=%d tris=%d why=%s model=%s type=%s"), bSame ? 1 : 0, bPreview ? 1 : 0, Built.bOk ? 1 : 0,
				Preview.WheelDots.Num(), Preview.Body.LODs.Num() > 0 ? Preview.Body.LODs[0].Indices.Num() / 3 : 0, *Why, *Args[0], *Args[1]);
			for (const FString& Problem : Preview.Problems)
			{
				UE_LOG(LogTemp, Warning, TEXT("MANTLE_VEHICLE_PREVIEW_CHECK problem: %s"), *Problem);
			}
		}));
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
		OutError = TEXT("Pick a model file first (.fbx, .obj, .gltf or .glb).");
		return nullptr;
	}
	const FString Full = FPaths::ConvertRelativePathToFull(File);
	const FString Extension = FPaths::GetExtension(Full).ToLower();
	if (Extension == TEXT("usd") || Extension == TEXT("usda") || Extension == TEXT("usdc") || Extension == TEXT("usdz"))
	{
		OutError = FString::Printf(TEXT("%s is a USD file, and USD files aren't supported. Export your model as FBX, OBJ or glTF and pick that file."), *FPaths::GetCleanFilename(Full));
		return nullptr;
	}
	if (Extension != TEXT("fbx") && Extension != TEXT("obj") && Extension != TEXT("gltf") && Extension != TEXT("glb"))
	{
		OutError = FString::Printf(TEXT("%s isn't a model file Mantle can import. Export your model from your 3D program as FBX, OBJ or glTF and pick that file, or import it into your project and pick it from the list."), *FPaths::GetCleanFilename(Full));
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
	if (Extension == TEXT("fbx"))
	{
		// The editor's own FBX importer, told to work out for itself whether the file is a static or a skinned mesh, and to leave animations and the physics asset out.
		UFbxFactory* Factory = NewObject<UFbxFactory>(Task);
		if (Factory->ImportUI)
		{
			Factory->ImportUI->bImportAnimations = false;
			Factory->ImportUI->bCreatePhysicsAsset = false;
			Factory->ImportUI->bAutomatedImportShouldDetectType = true;
			// A car exported as many pieces (a door, a bumper, each wheel) is one model: a static file's meshes come in as one.
			if (Factory->ImportUI->StaticMeshImportData)
			{
				Factory->ImportUI->StaticMeshImportData->bCombineMeshes = true;
			}
		}
		Task->Factory = Factory;
	}
	else
	{
		// OBJ and glTF/GLB have no factory set: the editor picks its own importer for the file type (Interchange, which is on by default), told to combine the file's
		// static meshes into one (it makes one asset per piece otherwise, and a car in 56 pieces would keep only the first).
		Task->Options = MantleVehicleBuilderPrivate::MakeCombineStaticMeshesStack(Task);
	}

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
