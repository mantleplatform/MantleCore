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

	/** The wheel a driver turns (steeringwheel, steering_wheel), which sits in the cabin and is not one of the vehicle's wheels. */
	bool IsSteeringWheelName(const FString& Lower)
	{
		return HasAny(Lower, { TEXT("steeringwheel"), TEXT("steering_wheel"), TEXT("steering wheel"), TEXT("steering-wheel"), TEXT("steerwheel"), TEXT("steer_wheel"),
			TEXT("wheel_steering"), TEXT("wheelsteering") });
	}

	bool IsWheelName(const FString& Lower)
	{
		return HasAny(Lower, { TEXT("wheel"), TEXT("tire"), TEXT("tyre") }) && !IsSteeringWheelName(Lower);
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

	/** Words only a plane's parts carry. "wing" alone is not one (a sports car has a rear wing), and nor are "rudder" and "flap" (a bike's steering bone may be a rudder). */
	bool IsAirframeName(const FString& Lower)
	{
		return HasAny(Lower, { TEXT("aileron"), TEXT("elevator"), TEXT("fuselage"), TEXT("stabilizer"), TEXT("stabiliser") });
	}

	/** Words only a motorcycle's parts carry. */
	bool IsBikeName(const FString& Lower)
	{
		return HasAny(Lower, { TEXT("handlebar"), TEXT("kickstand"), TEXT("swingarm"), TEXT("brake-lever"), TEXT("brakelever"), TEXT("brake_lever") });
	}

	bool IsJetName(const FString& Lower)
	{
		return HasAny(Lower, { TEXT("turbine"), TEXT("afterburner"), TEXT("nozzle") });
	}

	/** A name's words, lower case: split at anything but a letter, and where a lower-case letter is followed by an upper-case one (RoadWheel_L1: road, wheel, l). */
	TArray<FString> NameWords(const FString& Name)
	{
		TArray<FString> Words;
		FString Word;
		TCHAR Last = 0;
		for (const TCHAR Letter : Name)
		{
			const bool bLetter = FChar::IsAlpha(Letter);
			if (!Word.IsEmpty() && (!bLetter || (FChar::IsUpper(Letter) && FChar::IsLower(Last))))
			{
				Words.Add(Word.ToLower());
				Word.Reset();
			}
			if (bLetter)
			{
				Word.AppendChar(Letter);
			}
			Last = Letter;
		}
		if (!Word.IsEmpty())
		{
			Words.Add(Word.ToLower());
		}
		return Words;
	}

	bool IsAnyOf(const FString& Word, std::initializer_list<const TCHAR*> Choices)
	{
		for (const TCHAR* Choice : Choices)
		{
			if (Word.Equals(Choice, ESearchCase::CaseSensitive))
			{
				return true;
			}
		}
		return false;
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
	FString TypeOfFamily(const EMantleVehicleModelFamily Family)
	{
		switch (Family)
		{
		case EMantleVehicleModelFamily::Road:
			return TEXT("sedan");
		case EMantleVehicleModelFamily::Motorcycle:
			return TEXT("motorcycle");
		case EMantleVehicleModelFamily::Plane:
			return TEXT("plane");
		case EMantleVehicleModelFamily::Helicopter:
			return TEXT("helicopter");
		case EMantleVehicleModelFamily::Hover:
			return TEXT("hovercraft");
		case EMantleVehicleModelFamily::Tracked:
			return TEXT("tank");
		default:
			return FString();
		}
	}

	/** "a car, truck or van", for the message. */
	FString Kind(const EMantleVehicleModelFamily Family)
	{
		switch (Family)
		{
		case EMantleVehicleModelFamily::Road:
			return TEXT("a car or truck");
		case EMantleVehicleModelFamily::Motorcycle:
			return TEXT("a motorcycle");
		case EMantleVehicleModelFamily::Plane:
			return TEXT("a plane");
		case EMantleVehicleModelFamily::Helicopter:
			return TEXT("a helicopter");
		case EMantleVehicleModelFamily::Tracked:
			return TEXT("a tank or bulldozer");
		default:
			return TEXT("a hovercraft");
		}
	}

	FString Does(const EMantleVehicleModelFamily Family)
	{
		switch (Family)
		{
		case EMantleVehicleModelFamily::Road:
			return TEXT("drives on four or more wheels");
		case EMantleVehicleModelFamily::Motorcycle:
			return TEXT("rides on two wheels in a line");
		case EMantleVehicleModelFamily::Plane:
			return TEXT("flies on wings");
		case EMantleVehicleModelFamily::Helicopter:
			return TEXT("flies on a rotor");
		case EMantleVehicleModelFamily::Tracked:
			return TEXT("runs on two tracks");
		default:
			return TEXT("floats on air");
		}
	}
}

bool FMantleVehicleTypeMatchCheck::IsTrackPartName(const FString& Name)
{
	using namespace MantleVehicleTypeMatchPrivate;
	if (IsTrackBeltName(Name))
	{
		return true;
	}
	const TArray<FString> Words = NameWords(Name);
	for (int32 Index = 0; Index < Words.Num(); ++Index)
	{
		const FString& Word = Words[Index];
		const FString After = Index + 1 < Words.Num() ? Words[Index + 1] : FString();
		if (IsAnyOf(Word, { TEXT("sprocket"), TEXT("sprockets"), TEXT("idler"), TEXT("idlers"), TEXT("roadwheel"), TEXT("roadwheels") }))
		{
			return true;
		}
		if (Word == TEXT("road") && IsAnyOf(After, { TEXT("wheel"), TEXT("wheels") }))
		{
			return true;
		}
	}
	return false;
}

bool FMantleVehicleTypeMatchCheck::IsTrackBeltName(const FString& Name)
{
	using namespace MantleVehicleTypeMatchPrivate;
	const TArray<FString> Words = NameWords(Name);
	// A tire's tread is a car's: tread counts only on a part that is not a tire.
	const bool bTire = Words.ContainsByPredicate([](const FString& Word) { return IsAnyOf(Word, { TEXT("tire"), TEXT("tires"), TEXT("tyre"), TEXT("tyres") }); });
	for (int32 Index = 0; Index < Words.Num(); ++Index)
	{
		const FString& Word = Words[Index];
		const FString Before = Index > 0 ? Words[Index - 1] : FString();
		const FString After = Index + 1 < Words.Num() ? Words[Index + 1] : FString();
		// A car's track rod and track bar steer and locate its axle; they are no track.
		if (IsAnyOf(Word, { TEXT("track"), TEXT("tracks") }) && !IsAnyOf(After, { TEXT("rod"), TEXT("rods"), TEXT("bar"), TEXT("bars") }))
		{
			return true;
		}
		if (IsAnyOf(Word, { TEXT("tread"), TEXT("treads") }) && !bTire)
		{
			return true;
		}
		// A seat belt, a fan belt, a timing belt or a motorcycle's belt drive is a car's or a bike's.
		if (IsAnyOf(Word, { TEXT("belt"), TEXT("belts") }) && !IsAnyOf(Before, { TEXT("seat"), TEXT("safety"), TEXT("fan"), TEXT("timing"), TEXT("serpentine"), TEXT("drive"), TEXT("v") })
			&& !IsAnyOf(After, { TEXT("drive"), TEXT("guard"), TEXT("cover") }))
		{
			return true;
		}
	}
	return false;
}

bool FMantleVehicleTypeMatchCheck::WheelsPackedLikeTracks(const TConstArrayView<FVector> Positions, const double FloorZ)
{
	if (Positions.Num() < 8)
	{
		return false;
	}
	// The row the wheels make runs along the longer side of their box; the two sides are either half across it.
	FBox Box(ForceInit);
	for (const FVector& Position : Positions)
	{
		Box += Position;
	}
	const FVector Size = Box.GetSize();
	const int32 Along = Size.X >= Size.Y ? 0 : 1;
	const int32 Across = 1 - Along;
	const double Middle = Box.GetCenter()[Across];
	TArray<FVector> Sides[2];
	for (const FVector& Position : Positions)
	{
		const double Off = Position[Across] - Middle;
		if (FMath::Abs(Off) < 1.0)
		{
			// A wheel in the middle is on neither side: this is no pair of tracks.
			return false;
		}
		Sides[Off < 0.0 ? 0 : 1].Add(Position);
	}
	for (TArray<FVector>& Side : Sides)
	{
		if (Side.Num() < 4)
		{
			return false;
		}
		Side.Sort([Along](const FVector& A, const FVector& B) { return A[Along] < B[Along]; });
		for (int32 Index = 1; Index < Side.Num(); ++Index)
		{
			// A hub's height above the ground is its wheel's radius, so two neighbors' heights together are a wheel across.
			const double Diameter = (Side[Index - 1].Z - FloorZ) + (Side[Index].Z - FloorZ);
			const double Gap = Side[Index][Along] - Side[Index - 1][Along];
			if (!(Diameter > 0.0) || Gap >= MaxGapPerDiameter * Diameter)
			{
				return false;
			}
		}
	}
	return true;
}

EMantleVehicleModelFamily FMantleVehicleTypeMatchCheck::FamilyOfType(const FString& TypeId)
{
	const FMantleVehiclePreset* Preset = FMantleVehiclePresets::Find(TypeId);
	if (!Preset)
	{
		return EMantleVehicleModelFamily::Unknown;
	}
	if (Preset->MovementType == TEXT("wheeled"))
	{
		return Preset->Id == TEXT("motorcycle") ? EMantleVehicleModelFamily::Motorcycle : EMantleVehicleModelFamily::Road;
	}
	if (Preset->MovementType == TEXT("plane"))
	{
		return EMantleVehicleModelFamily::Plane;
	}
	if (Preset->MovementType == TEXT("helicopter"))
	{
		return EMantleVehicleModelFamily::Helicopter;
	}
	if (Preset->MovementType == TEXT("hover"))
	{
		return EMantleVehicleModelFamily::Hover;
	}
	if (Preset->MovementType == TEXT("tracked"))
	{
		return EMantleVehicleModelFamily::Tracked;
	}
	return EMantleVehicleModelFamily::Unknown;
}

EMantleVehicleModelFamily FMantleVehicleTypeMatchCheck::FamilyOfModel(const FMantleVehicleModel& Model, TArray<FString>& OutEvidence, EMantleVehicleTypeConfidence& OutConfidence)
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
	const TArray<int32> BikeParts = TopBones(Model, [](const FString& Name) { return IsBikeName(Name); });
	const FString LowerName = Model.Name.ToLower();
	const bool bBikeNamed = HasAny(LowerName, { TEXT("motorcycle"), TEXT("motorbike"), TEXT("bike") });
	const TArray<int32> Wheels = TopBones(Model, [](const FString& Name) { return IsWheelName(Name); });
	const bool bAirframeMaterial = AnyMaterial([](const FString& Name) { return IsAirframeName(Name); });
	const bool bWheelMaterial = AnyMaterial([](const FString& Name) { return IsWheelName(Name); });

	// A helicopter's main rotor, then a plane's propeller, tail surfaces or jet engine: these outrank wheels, because a plane has landing gear too.
	if (Rotors.Num() > 0)
	{
		OutEvidence.Add(TEXT("a rotor"));
		return EMantleVehicleModelFamily::Helicopter;
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
		return EMantleVehicleModelFamily::Plane;
	}

	// Tracks (#448), only on real evidence, before the wheels a track runs over would say a road vehicle: a part named like a track's (its bones by their own
	// spelling, so a RoadWheel splits into its words), or a row of 4 or more wheels on each side packed into a track. Two rows of 3 stay a six-wheeled truck.
	{
		TArray<FString> TrackBones;
		for (const FMantleSkinnedMeshBone& Bone : Model.Bones)
		{
			const FString BoneName = Bone.Name.ToString();
			if (IsTrackPartName(BoneName))
			{
				TrackBones.Add(BoneName);
			}
		}
		const bool bTrackMaterial = Model.Materials.ContainsByPredicate([](const FSkeletalMaterial& Material)
		{
			return IsTrackPartName(Material.MaterialSlotName.ToString()) || IsTrackPartName(Material.ImportedMaterialSlotName.ToString());
		});
		bool bPacked = false;
		const FBox WheelBounds = Model.Bounds();
		if (Wheels.Num() >= 8 && Model.BonePositions.Num() == Model.Bones.Num() && WheelBounds.IsValid)
		{
			TArray<FVector> Hubs;
			for (const int32 Wheel : Wheels)
			{
				Hubs.Add(Model.BonePositions[Wheel]);
			}
			bPacked = WheelsPackedLikeTracks(Hubs, WheelBounds.Min.Z);
		}
		if (TrackBones.Num() > 0 || bTrackMaterial || bPacked)
		{
			if (TrackBones.Num() > 0)
			{
				OutEvidence.Add(FString::Printf(TEXT("parts named like a track's (%s)"), *TrackBones[0]));
			}
			if (bTrackMaterial)
			{
				OutEvidence.Add(TEXT("a track material"));
			}
			if (bPacked)
			{
				OutEvidence.Add(FString::Printf(TEXT("%d wheels packed into two tracks"), Wheels.Num()));
			}
			return EMantleVehicleModelFamily::Tracked;
		}
	}

	// Wheels: more than two is a road vehicle, two in a line a motorcycle.
	if (Wheels.Num() >= 3)
	{
		OutEvidence.Add(FString::Printf(TEXT("%d wheels"), Wheels.Num()));
		return EMantleVehicleModelFamily::Road;
	}
	// Handlebars, brake levers and the like (or the model's own name) are a motorcycle's, whatever its wheel bones are called.
	if (BikeParts.Num() > 0 || bBikeNamed)
	{
		OutEvidence.Add(BikeParts.Num() > 0 ? TEXT("handlebars or brake levers") : TEXT("named like a bike"));
		return EMantleVehicleModelFamily::Motorcycle;
	}
	if (Wheels.Num() > 0)
	{
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
				return EMantleVehicleModelFamily::Motorcycle;
			}
			if (Along > UE_KINDA_SMALL_NUMBER && Across > 0.3 * Along)
			{
				OutEvidence.Add(TEXT("2 wheels, side by side"));
				return EMantleVehicleModelFamily::Road;
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
				return EMantleVehicleModelFamily::Motorcycle;
			}
			if (Ratio >= 0.75 && Height <= 0.4 * Long && !bWheelMaterial)
			{
				OutEvidence.Add(TEXT("wide and flat"));
				return EMantleVehicleModelFamily::Plane;
			}
			if (bWheelMaterial && Ratio > 0.35)
			{
				OutEvidence.Add(TEXT("wheel materials and a car's proportions"));
				return EMantleVehicleModelFamily::Road;
			}
			if (Ratio >= 0.3 && Ratio <= 0.7 && Height <= 1.2 * Wide)
			{
				OutEvidence.Add(TEXT("a car's proportions"));
				return EMantleVehicleModelFamily::Road;
			}
		}
	}
	OutConfidence = EMantleVehicleTypeConfidence::Weak;
	return EMantleVehicleModelFamily::Unknown;
}

FMantleVehicleTypeMatch FMantleVehicleTypeMatchCheck::Check(const FMantleVehicleModel& Model, const FString& TypeId)
{
	using namespace MantleVehicleTypeMatchPrivate;
	FMantleVehicleTypeMatch Out;
	const FMantleVehiclePreset* Picked = FMantleVehiclePresets::Find(TypeId);
	const EMantleVehicleModelFamily Wanted = FamilyOfType(TypeId);
	if (!Picked || Wanted == EMantleVehicleModelFamily::Unknown)
	{
		Out.Verdict = EMantleVehicleTypeVerdict::Unsure;
		Out.Message = TEXT("Pick a vehicle type first.");
		return Out;
	}

	Out.Family = FamilyOfModel(Model, Out.Evidence, Out.Confidence);
	if (Out.Family == EMantleVehicleModelFamily::Unknown)
	{
		Out.Verdict = EMantleVehicleTypeVerdict::Unsure;
		Out.Message = FString::Printf(TEXT("Mantle can't tell what kind of vehicle this model is, so it will use it as a %s."), *Picked->Name.ToLower());
		return Out;
	}
	if (Out.Family == Wanted)
	{
		return Out;
	}
	// Only the proportions to go by: they can say a model is not a car or a bike, but not what a plane, helicopter, hovercraft or tracked vehicle is, so those picks are let through.
	if (Out.Confidence == EMantleVehicleTypeConfidence::Weak && Wanted != EMantleVehicleModelFamily::Road && Wanted != EMantleVehicleModelFamily::Motorcycle)
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
