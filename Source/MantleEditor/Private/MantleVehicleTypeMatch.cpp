// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleTypeMatch.h"

#include "MantleVehiclePreset.h"

#include "HAL/IConsoleManager.h"

namespace MantleVehicleTypeMatchPrivate
{
	bool HasAny(const FString& Lower, std::initializer_list<const TCHAR*> Words)
	{
		for (const TCHAR* Word : Words)
		{
			if (Lower.Contains(Word, ESearchCase::CaseSensitive))
			{
				return true;
			}
		}
		return false;
	}

	bool IsWheelName(const FString& Lower)
	{
		return HasAny(Lower, { TEXT("wheel"), TEXT("tire"), TEXT("tyre") });
	}

	/** A rotor bone is a helicopter's main rotor; a car's brake rotor is not. */
	bool IsRotorName(const FString& Lower)
	{
		return Lower.Contains(TEXT("rotor"), ESearchCase::CaseSensitive) && !HasAny(Lower, { TEXT("brake"), TEXT("disc"), TEXT("disk") });
	}

	bool IsPropellerName(const FString& Lower)
	{
		return Lower.Contains(TEXT("prop"), ESearchCase::CaseSensitive) && !HasAny(Lower, { TEXT("guard"), TEXT("property"), TEXT("proportion") });
	}

	/** Words only a plane's parts carry. "wing" alone is not one: a sports car has a rear wing. */
	bool IsAirframeName(const FString& Lower)
	{
		return HasAny(Lower, { TEXT("aileron"), TEXT("elevator"), TEXT("rudder"), TEXT("fuselage"), TEXT("stabilizer"), TEXT("stabiliser"), TEXT("flap") });
	}

	bool IsJetName(const FString& Lower)
	{
		return HasAny(Lower, { TEXT("turbine"), TEXT("afterburner"), TEXT("nozzle") });
	}

	/** The bones whose name passes Test, a chain of them (a wheel with a brake bone under it) counted once at the bone nearest the root. */
	TArray<int32> TopBones(const FMantleVehicleModel& Model, const TFunctionRef<bool(const FString&)> Test)
	{
		TArray<bool> Passes;
		Passes.Init(false, Model.Bones.Num());
		for (int32 Bone = 0; Bone < Model.Bones.Num(); ++Bone)
		{
			Passes[Bone] = Test(Model.Bones[Bone].Name.ToString().ToLower());
		}
		TArray<int32> Top;
		for (int32 Bone = 0; Bone < Model.Bones.Num(); ++Bone)
		{
			if (!Passes[Bone])
			{
				continue;
			}
			bool bCarried = false;
			int32 Parent = Model.Bones[Bone].ParentIndex;
			for (int32 Guard = 0; Parent != INDEX_NONE && Parent < Model.Bones.Num() && Guard < Model.Bones.Num(); ++Guard)
			{
				if (Passes[Parent])
				{
					bCarried = true;
					break;
				}
				Parent = Model.Bones[Parent].ParentIndex;
			}
			if (!bCarried)
			{
				Top.Add(Bone);
			}
		}
		return Top;
	}

	/** Lower-cased names of the model's material slots. */
	TArray<FString> MaterialNames(const FMantleVehicleModel& Model)
	{
		TArray<FString> Names;
		for (const FSkeletalMaterial& Material : Model.Materials)
		{
			Names.Add((Material.MaterialSlotName.ToString() + TEXT(" ") + Material.ImportedMaterialSlotName.ToString()).ToLower());
		}
		return Names;
	}

	/** The first (id) type of a family, which the "Switch to" button offers. */
	FString TypeOfFamily(const EMantleVehicleFamily Family)
	{
		switch (Family)
		{
		case EMantleVehicleFamily::Road:
			return TEXT("sedan");
		case EMantleVehicleFamily::Motorcycle:
			return TEXT("motorcycle");
		case EMantleVehicleFamily::Plane:
			return TEXT("plane");
		case EMantleVehicleFamily::Helicopter:
			return TEXT("helicopter");
		case EMantleVehicleFamily::Hover:
			return TEXT("hovercraft");
		default:
			return FString();
		}
	}

	/** "a car, truck or van", for the message. */
	FString Kind(const EMantleVehicleFamily Family)
	{
		switch (Family)
		{
		case EMantleVehicleFamily::Road:
			return TEXT("a car or truck");
		case EMantleVehicleFamily::Motorcycle:
			return TEXT("a motorcycle");
		case EMantleVehicleFamily::Plane:
			return TEXT("a plane");
		case EMantleVehicleFamily::Helicopter:
			return TEXT("a helicopter");
		default:
			return TEXT("a hovercraft");
		}
	}

	FString Does(const EMantleVehicleFamily Family)
	{
		switch (Family)
		{
		case EMantleVehicleFamily::Road:
			return TEXT("drives on four or more wheels");
		case EMantleVehicleFamily::Motorcycle:
			return TEXT("rides on two wheels in a line");
		case EMantleVehicleFamily::Plane:
			return TEXT("flies on wings");
		case EMantleVehicleFamily::Helicopter:
			return TEXT("flies on a rotor");
		default:
			return TEXT("floats on air");
		}
	}
}

EMantleVehicleFamily FMantleVehicleTypeMatchCheck::FamilyOfType(const FString& TypeId)
{
	const FMantleVehiclePreset* Preset = FMantleVehiclePresets::Find(TypeId);
	if (!Preset)
	{
		return EMantleVehicleFamily::Unknown;
	}
	if (Preset->MovementType == TEXT("wheeled"))
	{
		return Preset->Id == TEXT("motorcycle") ? EMantleVehicleFamily::Motorcycle : EMantleVehicleFamily::Road;
	}
	if (Preset->MovementType == TEXT("plane"))
	{
		return EMantleVehicleFamily::Plane;
	}
	if (Preset->MovementType == TEXT("helicopter"))
	{
		return EMantleVehicleFamily::Helicopter;
	}
	if (Preset->MovementType == TEXT("hover"))
	{
		return EMantleVehicleFamily::Hover;
	}
	return EMantleVehicleFamily::Unknown;
}

EMantleVehicleFamily FMantleVehicleTypeMatchCheck::FamilyOfModel(const FMantleVehicleModel& Model, TArray<FString>& OutEvidence, EMantleVehicleTypeConfidence& OutConfidence)
{
	using namespace MantleVehicleTypeMatchPrivate;
	OutConfidence = EMantleVehicleTypeConfidence::Strong;
	const TArray<FString> Materials = MaterialNames(Model);
	const auto AnyMaterial = [&Materials](const TFunctionRef<bool(const FString&)> Test)
	{
		return Materials.ContainsByPredicate([&Test](const FString& Name) { return Test(Name); });
	};

	const TArray<int32> Rotors = TopBones(Model, [](const FString& Name) { return IsRotorName(Name); });
	const TArray<int32> Props = TopBones(Model, [](const FString& Name) { return IsPropellerName(Name); });
	const TArray<int32> Airframe = TopBones(Model, [](const FString& Name) { return IsAirframeName(Name); });
	const TArray<int32> Jets = TopBones(Model, [](const FString& Name) { return IsJetName(Name); });
	const TArray<int32> Wheels = TopBones(Model, [](const FString& Name) { return IsWheelName(Name); });
	const bool bAirframeMaterial = AnyMaterial([](const FString& Name) { return IsAirframeName(Name); });
	const bool bWheelMaterial = AnyMaterial([](const FString& Name) { return IsWheelName(Name); });

	// A helicopter's main rotor, then a plane's propeller, tail surfaces or jet engine: these outrank wheels, because a plane has landing gear too.
	if (Rotors.Num() > 0)
	{
		OutEvidence.Add(TEXT("a rotor"));
		return EMantleVehicleFamily::Helicopter;
	}
	if (Props.Num() > 0 || Airframe.Num() > 0 || Jets.Num() > 0 || bAirframeMaterial)
	{
		if (Props.Num() > 0)
		{
			OutEvidence.Add(FString::Printf(TEXT("%d propeller%s"), Props.Num(), Props.Num() == 1 ? TEXT("") : TEXT("s")));
		}
		if (Airframe.Num() > 0 || bAirframeMaterial)
		{
			OutEvidence.Add(TEXT("wings or tail surfaces"));
		}
		if (Jets.Num() > 0)
		{
			OutEvidence.Add(TEXT("a jet engine"));
		}
		return EMantleVehicleFamily::Plane;
	}

	// Wheels: more than two is a road vehicle, two in a line a motorcycle.
	if (Wheels.Num() > 0)
	{
		if (Wheels.Num() >= 3)
		{
			OutEvidence.Add(FString::Printf(TEXT("%d wheels"), Wheels.Num()));
			return EMantleVehicleFamily::Road;
		}
		if (Wheels.Num() == 2 && Model.BonePositions.Num() == Model.Bones.Num())
		{
			FBox Box(ForceInit);
			for (const int32 Wheel : Wheels)
			{
				Box += Model.BonePositions[Wheel];
			}
			const FVector Size = Box.GetSize();
			const double Along = FMath::Max(Size.X, Size.Y);
			const double Across = FMath::Min(Size.X, Size.Y);
			if (Along > UE_KINDA_SMALL_NUMBER && Across <= 0.15 * Along)
			{
				OutEvidence.Add(TEXT("2 wheels, in a line"));
				return EMantleVehicleFamily::Motorcycle;
			}
			if (Along > UE_KINDA_SMALL_NUMBER && Across > 0.3 * Along)
			{
				OutEvidence.Add(TEXT("2 wheels, side by side"));
				return EMantleVehicleFamily::Road;
			}
		}
	}

	// The model gives no part to go by. Its proportions are a weak sign.
	const FBox Bounds = Model.Bounds();
	if (Bounds.IsValid)
	{
		const FVector Size = Bounds.GetSize();
		const double Long = FMath::Max(Size.X, Size.Y);
		const double Wide = FMath::Min(Size.X, Size.Y);
		if (Long > 1.0 && Wide > 1.0)
		{
			OutConfidence = EMantleVehicleTypeConfidence::Weak;
			const double Ratio = Wide / Long;
			const double Height = Size.Z;
			if (Ratio <= 0.35 && Height >= 1.6 * Wide)
			{
				OutEvidence.Add(TEXT("narrow and tall"));
				return EMantleVehicleFamily::Motorcycle;
			}
			if (Ratio >= 0.75 && Height <= 0.4 * Long && !bWheelMaterial)
			{
				OutEvidence.Add(TEXT("wide and flat"));
				return EMantleVehicleFamily::Plane;
			}
			if (bWheelMaterial && Ratio > 0.35)
			{
				OutEvidence.Add(TEXT("wheel materials and a car's proportions"));
				return EMantleVehicleFamily::Road;
			}
			if (Ratio >= 0.3 && Ratio <= 0.7 && Height <= 1.2 * Wide)
			{
				OutEvidence.Add(TEXT("a car's proportions"));
				return EMantleVehicleFamily::Road;
			}
		}
	}
	OutConfidence = EMantleVehicleTypeConfidence::Weak;
	return EMantleVehicleFamily::Unknown;
}

FMantleVehicleTypeMatch FMantleVehicleTypeMatchCheck::Check(const FMantleVehicleModel& Model, const FString& TypeId)
{
	using namespace MantleVehicleTypeMatchPrivate;
	FMantleVehicleTypeMatch Out;
	const FMantleVehiclePreset* Picked = FMantleVehiclePresets::Find(TypeId);
	const EMantleVehicleFamily Wanted = FamilyOfType(TypeId);
	if (!Picked || Wanted == EMantleVehicleFamily::Unknown)
	{
		Out.Verdict = EMantleVehicleTypeVerdict::Unsure;
		Out.Message = TEXT("Pick a vehicle type first.");
		return Out;
	}

	Out.Family = FamilyOfModel(Model, Out.Evidence, Out.Confidence);
	if (Out.Family == EMantleVehicleFamily::Unknown)
	{
		Out.Verdict = EMantleVehicleTypeVerdict::Unsure;
		Out.Message = FString::Printf(TEXT("Mantle can't tell what kind of vehicle this model is, so it will use it as a %s."), *Picked->Name.ToLower());
		return Out;
	}
	if (Out.Family == Wanted)
	{
		return Out;
	}
	// Only the proportions to go by: they can say a model is not a car or a bike, but not what a plane, helicopter or hovercraft is, so those picks are let through.
	if (Out.Confidence == EMantleVehicleTypeConfidence::Weak && Wanted != EMantleVehicleFamily::Road && Wanted != EMantleVehicleFamily::Motorcycle)
	{
		return Out;
	}
	Out.Verdict = EMantleVehicleTypeVerdict::Suggests;
	Out.SuggestedType = TypeOfFamily(Out.Family);
	const FMantleVehiclePreset* Suggested = FMantleVehiclePresets::Find(Out.SuggestedType);
	Out.SuggestedName = Suggested ? Suggested->Name : Out.SuggestedType;
	Out.ButtonText = FString::Printf(TEXT("Switch to %s"), *Out.SuggestedName);
	const FString Why = FString::Join(Out.Evidence, TEXT(", "));
	Out.Message = FString::Printf(TEXT("This model %s %s (%s), but a %s %s. Switch to %s, or pick a different model."),
		Out.Confidence == EMantleVehicleTypeConfidence::Strong ? TEXT("looks like") : TEXT("may be"),
		*Kind(Out.Family), *Why, *Picked->Name.ToLower(), *Does(Wanted), *Out.SuggestedName);
	return Out;
}

bool FMantleVehicleTypeMatchCheck::CheckAsset(UObject* Asset, const FString& TypeId, FMantleVehicleTypeMatch& Out, FString& OutError)
{
	FMantleVehicleModel Model;
	if (!FMantleVehicleModel::Read(Asset, Model, OutError))
	{
		return false;
	}
	Out = Check(Model, TypeId);
	return true;
}

namespace MantleVehicleTypeMatchPrivate
{
	// Mantle.Vehicle.CheckType <model object path> <vehicle type id>: the check from the console or a script, for a Fab pack's car, bike or plane.
	FAutoConsoleCommand CheckTypeCommand(
		TEXT("Mantle.Vehicle.CheckType"),
		TEXT("Mantle.Vehicle.CheckType <model object path> <vehicle type id> -- logs one MANTLE_VEHICLE_TYPECHECK line: whether the model fits the type, or the type it suggests and why."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() < 2)
			{
				UE_LOG(LogTemp, Error, TEXT("MANTLE_VEHICLE_TYPECHECK ok=0 usage: Mantle.Vehicle.CheckType <model object path> <vehicle type id>"));
				return;
			}
			UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *Args[0], nullptr, LOAD_NoWarn | LOAD_Quiet);
			FMantleVehicleTypeMatch Match;
			FString Error;
			if (!FMantleVehicleTypeMatchCheck::CheckAsset(Asset, Args[1], Match, Error))
			{
				UE_LOG(LogTemp, Warning, TEXT("MANTLE_VEHICLE_TYPECHECK ok=0 model=%s problem: %s"), *Args[0], *Error);
				return;
			}
			const TCHAR* Verdict = Match.Verdict == EMantleVehicleTypeVerdict::Fits ? TEXT("fits") : Match.Verdict == EMantleVehicleTypeVerdict::Suggests ? TEXT("suggests") : TEXT("unsure");
			UE_LOG(LogTemp, Display, TEXT("MANTLE_VEHICLE_TYPECHECK ok=1 model=%s type=%s verdict=%s suggested=%s evidence=[%s] message: %s"), *Args[0], *Args[1], Verdict,
				*Match.SuggestedType, *FString::Join(Match.Evidence, TEXT("; ")), *Match.Message);
		}));
}
