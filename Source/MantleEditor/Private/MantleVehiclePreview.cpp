// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehiclePreview.h"
#include "MantleVehicleAsset.h"
#include "MantleVehicleConverter.h"
#include "MantleVehicleSource.h"

#include "Animation/Skeleton.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "ReferenceSkeleton.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Sound/SoundBase.h"
#include "UObject/Package.h"

// Named, not anonymous: unity builds merge this file with others.
namespace MantleVehiclePreviewPrivate
{
	/** A number as the cook writes a derived one: rounded to hundredths. */
	TSharedRef<FJsonValue> PreviewNumber(const double Value)
	{
		return MakeShared<FJsonValueNumber>(FMath::RoundToDouble(Value * 100.0) / 100.0);
	}

	TSharedRef<FJsonValue> PreviewTriple(const FVector& Value)
	{
		TArray<TSharedPtr<FJsonValue>> Items = { PreviewNumber(Value.X), PreviewNumber(Value.Y), PreviewNumber(Value.Z) };
		return MakeShared<FJsonValueArray>(Items);
	}

	/** limits.json's range at Path, or the whole line when this build has none. */
	void PreviewRange(const TCHAR* Path, double& OutMin, double& OutMax)
	{
		if (!FMantleVehicleLimits::GetRange(Path, OutMin, OutMax))
		{
			OutMin = -UE_BIG_NUMBER;
			OutMax = UE_BIG_NUMBER;
		}
	}

	/** Every bone's reference pose in component space (the raw skeleton lists parents first). */
	TArray<FTransform> PreviewComponentPose(const FReferenceSkeleton& Skeleton)
	{
		const TArray<FMeshBoneInfo>& Info = Skeleton.GetRawRefBoneInfo();
		const TArray<FTransform>& Pose = Skeleton.GetRawRefBonePose();
		TArray<FTransform> Out;
		Out.SetNum(Info.Num());
		for (int32 Bone = 0; Bone < Info.Num(); ++Bone)
		{
			const FTransform Local = Pose.IsValidIndex(Bone) ? Pose[Bone] : FTransform::Identity;
			const int32 Parent = Info[Bone].ParentIndex;
			Out[Bone] = Parent >= 0 && Parent < Bone ? Local * Out[Parent] : Local;
		}
		return Out;
	}
}

bool FMantleVehiclePreview::Make(const UMantleVehicleSource& Source, FMantleVehiclePreview& Out, FString& OutError)
{
	using namespace MantleVehiclePreviewPrivate;
	Out = FMantleVehiclePreview();
	USkeletalMesh* Mesh = Source.Body.LoadSynchronous();
	if (!Mesh)
	{
		OutError = FString::Printf(TEXT("%s has no body that loads (%s)"), *Source.GetName(), *Source.Body.ToString());
		return false;
	}

	// What publishing would refuse, said here so a creator learns it before uploading: Check this place's rules for the vehicle,
	// which hold the upload's caps (sections, triangles, bones, parts) and every range.
	TArray<FMantleValidationIssue> Issues;
	FMantleVehicleConverter::CheckSource(&Source, FString(), Issues);
	for (const FMantleValidationIssue& Issue : Issues)
	{
		(Issue.Severity == EMantleIssueSeverity::Error ? Out.WouldNotPublish : Out.Notes).Add(Issue.Problem + TEXT(" ") + Issue.Remedy);
	}
	// Convert Vehicle's notes (what it finds and measures), on a copy: the asset stays as the creator left it.
	UMantleVehicleSource* Scratch = NewObject<UMantleVehicleSource>(GetTransientPackage(), NAME_None, RF_Transient);
	Scratch->Wheels = Source.Wheels;
	Scratch->Tuning = Source.Tuning;
	Scratch->Paint.Sections = Source.Paint.Sections;
	Scratch->Linkage = Source.Linkage;
	Scratch->Parts = Source.Parts;
	const FMantleVehicleConversion Conversion = FMantleVehicleConverter::Convert(Mesh, *Scratch);
	Out.Notes.Append(Conversion.Notes);
	Out.WouldNotPublish.Append(Conversion.Problems);

	// The body as it uploads (LOD0, each tire merged onto its wheel's bone): what the cook measures wheels and the chassis from.
	// A process that kept no CPU copy of a mesh's vertices cannot read it: the car is still made from what the asset sets, and a
	// wheel with no size of its own, or a chassis with no collision or seat, then says so below.
	FMantleGlbMesh Body;
	TArray<UMaterialInterface*> BodyMaterials;
	FString BodyError;
	if (!FMantleVehicleConverter::ReadBody(Source, Body, BodyMaterials, BodyError))
	{
		// Again without its tires (a tire mesh without Allow CPU Access), so the body's own wheels can still be measured.
		UMantleVehicleSource* Bare = NewObject<UMantleVehicleSource>(GetTransientPackage(), NAME_None, RF_Transient);
		Bare->Body = Source.Body;
		Bare->Wheels = Source.Wheels;
		for (FMantleVehicleSourceWheel& Wheel : Bare->Wheels)
		{
			Wheel.TireMesh.Reset();
		}
		FString BareError;
		if (FMantleVehicleConverter::ReadBody(*Bare, Body, BodyMaterials, BareError))
		{
			Out.Notes.Add(FString::Printf(TEXT("Its tires could not be read here (%s): its wheels are measured from the body alone."), *BodyError));
		}
		else
		{
			Out.Notes.Add(FString::Printf(TEXT("Its body's geometry could not be read here (%s), so nothing is measured from it."), *BareError));
			Body = FMantleGlbMesh();
		}
	}
	const TArray<FTransform> Pose = PreviewComponentPose(Mesh->GetRefSkeleton());
	auto JointOf = [&Body](const FName Bone) { return FMantleVehicleConverter::JointIndex(Body, Bone); };

	// The local car is drawn on the body as modelled (its mesh is not resized), so its document is the unsized vehicle's; the check above held
	// the sized one, as the cook will (#728). With a Size of 1 the copy is skipped and nothing changes.
	const UMantleVehicleSource* AsModelled = &Source;
	if (Source.FitScale != 1.f)
	{
		UMantleVehicleSource* Unsized = DuplicateObject<UMantleVehicleSource>(&Source, GetTransientPackage());
		Unsized->FitScale = 1.f;
		AsModelled = Unsized;
	}
	const TSharedRef<FJsonObject> Document = FMantleVehicleConverter::MakeDocument(*AsModelled, TMap<FString, FString>());
	// Liveries upload as textures, which a local car has none of.
	Document->RemoveField(TEXT("liveryUV"));
	Document->RemoveField(TEXT("defaultLivery"));
	if (Source.Paint.Liveries.Num() > 0)
	{
		Out.Notes.Add(FString::Printf(TEXT("Its %d liveries are left out of the local car: they upload as textures."), Source.Paint.Liveries.Num()));
	}

	// The cook's resolveVehicle, for what the document leaves to the body. A wheel without a size: measured from the geometry
	// weighted to its bone alone (a merged tire's included), as the cook measures it.
	TSet<int32> WheelJoints;
	const TArray<TSharedPtr<FJsonValue>>* Wheels = nullptr;
	if (Document->TryGetArrayField(TEXT("wheels"), Wheels))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Wheels)
		{
			TSharedPtr<FJsonObject> Wheel;
			if (Value.IsValid())
			{
				Wheel = Value->AsObject();
			}
			if (!Wheel.IsValid())
			{
				continue;
			}
			const FName Bone(*Wheel->GetStringField(TEXT("bone")));
			WheelJoints.Add(JointOf(Bone));
			if (Wheel->HasField(TEXT("radius")) && Wheel->HasField(TEXT("width")))
			{
				continue;
			}
			double Radius = 0.0;
			double Width = 0.0;
			if (!FMantleVehicleConverter::MeasureWheel(Body, Bone, Radius, Width))
			{
				OutError = FString::Printf(TEXT("wheel %s has no size: no geometry is weighted to its bone alone. Set its Tire Mesh, or its Radius and Width, in %s"),
					*Bone.ToString(), *Source.GetName());
				return false;
			}
			if (!Wheel->HasField(TEXT("radius")))
			{
				Wheel->SetField(TEXT("radius"), PreviewNumber(Radius));
			}
			if (!Wheel->HasField(TEXT("width")))
			{
				Wheel->SetField(TEXT("width"), PreviewNumber(Width));
			}
		}
	}

	// The chassis: every vertex whose heaviest influence is not a wheel's. A body with none (its visible body all parts) has no
	// box or seat the cook can place; the local car takes them from around its parts instead.
	FBox Chassis(ForceInit);
	const int32 N = Body.NumInfluences;
	for (int32 Vertex = 0; Vertex < Body.Positions.Num(); ++Vertex)
	{
		const int32 Heaviest = N > 0 && Body.InfluenceJoints.IsValidIndex(Vertex * N) ? Body.InfluenceJoints[Vertex * N] : 0;
		if (!WheelJoints.Contains(Heaviest))
		{
			Chassis += FVector(Body.Positions[Vertex]);
		}
	}
	const bool bNeedsChassis = !Document->HasField(TEXT("collision")) || !Document->HasField(TEXT("seat"));
	if (!Chassis.IsValid && bNeedsChassis)
	{
		for (const FMantleVehicleSourcePart& Part : Source.Parts)
		{
			const UStaticMesh* PartMesh = Part.Mesh.LoadSynchronous();
			const int32 Bone = Mesh->GetRefSkeleton().FindRawBoneIndex(Part.Bone);
			if (PartMesh && Pose.IsValidIndex(Bone))
			{
				Chassis += PartMesh->GetBoundingBox().TransformBy(Part.Transform * Pose[Bone]);
			}
		}
		Out.WouldNotPublish.Add(TEXT("The body has no geometry off its wheels, so the cook cannot place its collision box or seat. Set Collision and Seat Location in the vehicle asset (the local car takes them from around its parts)."));
	}
	if (!Chassis.IsValid && bNeedsChassis)
	{
		OutError = FString::Printf(TEXT("%s has no geometry off its wheels and no parts to place its collision box and seat by. Set Collision and Seat Location in it"),
			*Source.GetName());
		return false;
	}
	if (!Document->HasField(TEXT("collision")))
	{
		double CenterMin = 0.0;
		double CenterMax = 0.0;
		double ExtentMin = 0.0;
		double ExtentMax = 0.0;
		PreviewRange(TEXT("collision/*/center/*"), CenterMin, CenterMax);
		PreviewRange(TEXT("collision/*/extent/*"), ExtentMin, ExtentMax);
		const FVector Center = Chassis.GetCenter();
		const FVector Extent = Chassis.GetExtent();
		const FVector HeldCenter(FMath::Clamp(Center.X, CenterMin, CenterMax), FMath::Clamp(Center.Y, CenterMin, CenterMax), FMath::Clamp(Center.Z, CenterMin, CenterMax));
		const FVector HeldExtent(FMath::Clamp(Extent.X, FMath::Max(1.0, ExtentMin), ExtentMax), FMath::Clamp(Extent.Y, FMath::Max(1.0, ExtentMin), ExtentMax),
			FMath::Clamp(Extent.Z, FMath::Max(1.0, ExtentMin), ExtentMax));
		if (!HeldCenter.Equals(Center, 0.01) || !HeldExtent.Equals(Extent.ComponentMax(FVector(1.0)), 0.01))
		{
			Out.WouldNotPublish.Add(FString::Printf(TEXT("The box around the chassis (%.0f cm across) is larger than a collision box may be. Set Collision in the vehicle asset."),
				2.0 * Extent.GetMax()));
		}
		const TSharedRef<FJsonObject> Box = MakeShared<FJsonObject>();
		Box->SetField(TEXT("center"), PreviewTriple(HeldCenter));
		Box->SetField(TEXT("extent"), PreviewTriple(HeldExtent));
		TArray<TSharedPtr<FJsonValue>> Boxes = { MakeShared<FJsonValueObject>(Box) };
		Document->SetArrayField(TEXT("collision"), Boxes);
	}
	if (!Document->HasField(TEXT("seat")))
	{
		// Behind the middle, on the left (-Y), a little under half the body's height up, as the cook places it.
		double Min = 0.0;
		double Max = 0.0;
		PreviewRange(TEXT("seat/location/*"), Min, Max);
		const FVector Size = Chassis.GetSize();
		const FVector Seat(Chassis.GetCenter().X - 0.1 * Size.X, Chassis.GetCenter().Y - 0.2 * Size.Y, Chassis.Min.Z + 0.45 * Size.Z);
		const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetField(TEXT("location"), PreviewTriple(FVector(FMath::Clamp(Seat.X, Min, Max), FMath::Clamp(Seat.Y, Min, Max), FMath::Clamp(Seat.Z, Min, Max))));
		Document->SetObjectField(TEXT("seat"), Entry);
	}
	// Shift points: 85% and 40% of the engine's top revs, the lower one under the upper.
	const TSharedPtr<FJsonObject>* Transmission = nullptr;
	if (Document->TryGetObjectField(TEXT("transmission"), Transmission) && Transmission && Transmission->IsValid())
	{
		const int32 MaxRPM = Source.Tuning.MaxRPM;
		double DownMin = 0.0;
		double DownMax = 0.0;
		PreviewRange(TEXT("transmission/changeDownRPM"), DownMin, DownMax);
		int32 Up = static_cast<int32>(static_cast<double>(MaxRPM) * FMantleVehicleConverter::DefaultChangeUpFraction);
		if ((*Transmission)->HasField(TEXT("changeUpRPM")))
		{
			Up = static_cast<int32>((*Transmission)->GetNumberField(TEXT("changeUpRPM")));
		}
		else
		{
			(*Transmission)->SetField(TEXT("changeUpRPM"), MakeShared<FJsonValueNumber>(Up));
		}
		if (!(*Transmission)->HasField(TEXT("changeDownRPM")))
		{
			int32 Down = static_cast<int32>(static_cast<double>(MaxRPM) * FMantleVehicleConverter::DefaultChangeDownFraction);
			if (Down >= Up)
			{
				Down = FMath::Max(static_cast<int32>(DownMin), Up / 2);
			}
			(*Transmission)->SetField(TEXT("changeDownRPM"), MakeShared<FJsonValueNumber>(Down));
		}
	}

	// Read as a client reads a published vehicle's document.
	FString Text;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
	FJsonSerializer::Serialize(Document, Writer);
	const FTCHARToUTF8 Utf8(*Text);
	FMantleVehicleDesc Desc;
	FString Why;
	if (!FMantleVehicleAssetReader::ParseDocument(TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()), Desc, Why))
	{
		OutError = FString::Printf(TEXT("its vehicle document does not read (%s); Check this place names what to fix"), *Why);
		return false;
	}

	// The local stand-ins for what an upload names by id.
	int32 NextId = 0;
	auto StandIn = [&NextId]() { return FString::Printf(TEXT("ast_u%032x"), ++NextId); };
	for (const FSkeletalMaterial& Slot : Mesh->GetMaterials())
	{
		const FString Id = StandIn();
		Desc.Materials.Add(Slot.MaterialSlotName.ToString(), Id);
		Out.Materials.Add(Id, Slot.MaterialInterface.Get());
	}
	auto AddPart = [&Out, &Desc, &StandIn](UStaticMesh* PartMesh, const FName Bone, const FTransform& Transform, TFunctionRef<UMaterialInterface*(int32)> SlotMaterial,
		const TArray<int32>& PaintSlots)
	{
		FMantleVehiclePartDesc& Part = Desc.Parts.AddDefaulted_GetRef();
		Part.Mesh = StandIn();
		Part.Bone = Bone;
		Part.Transform = Transform;
		Part.PaintSlots = PaintSlots;
		Out.Meshes.Add(Part.Mesh, PartMesh);
		for (int32 Slot = 0; Slot < PartMesh->GetStaticMaterials().Num(); ++Slot)
		{
			const FString Id = StandIn();
			Part.Materials.Add(Id);
			Out.Materials.Add(Id, SlotMaterial(Slot));
		}
	};
	for (const FMantleVehicleSourcePart& Part : Source.Parts)
	{
		UStaticMesh* PartMesh = Part.Mesh.LoadSynchronous();
		if (!PartMesh)
		{
			Out.Notes.Add(FString::Printf(TEXT("A part on %s has no mesh that loads (%s): the local car is drawn without it."), *Part.Bone.ToString(), *Part.Mesh.ToString()));
			continue;
		}
		AddPart(PartMesh, Part.Bone, Part.Transform, [&Part, PartMesh](const int32 Slot) { return FMantleVehicleConverter::PartSlotMaterial(Part, PartMesh, Slot); },
			Part.PaintSlots);
	}
	for (const FMantleVehicleSourceWheel& Wheel : Source.Wheels)
	{
		UStaticMesh* Tire = Wheel.TireMesh.LoadSynchronous();
		if (Tire)
		{
			AddPart(Tire, Wheel.Bone, Wheel.TireTransform, [Tire](const int32 Slot) { return Tire->GetStaticMaterials()[Slot].MaterialInterface.Get(); }, TArray<int32>());
		}
	}
	Out.Horn = Source.Tuning.HornSound.LoadSynchronous();

	UMantleBuiltVehicle* Vehicle = NewObject<UMantleBuiltVehicle>(GetTransientPackage(), NAME_None, RF_Transient);
	Vehicle->Mesh = Mesh;
	Vehicle->Skeleton = Mesh->GetSkeleton();
	Vehicle->RootBoneTransform = Pose.Num() > 0 ? Pose[0] : FTransform::Identity;
	for (const FMantleVehicleWheelDesc& Wheel : Desc.Wheels)
	{
		const int32 Bone = Mesh->GetRefSkeleton().FindRawBoneIndex(Wheel.Bone);
		Vehicle->WheelPositions.Add(Pose.IsValidIndex(Bone) ? Pose[Bone].GetLocation() : FVector::ZeroVector);
	}
	Vehicle->Desc = MoveTemp(Desc);
	Out.Vehicle.Reset(Vehicle);
	return true;
}
