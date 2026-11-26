// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehiclePreset.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "MantleChassisSpec.h"
#include "MantleVehicleChassisBuilder.h"
#include "MantleVehicleSource.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogMantleVehiclePreset, Log, All);

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVehiclePresetPrivate
{
	constexpr int32 SchemaVersion = 1;

	bool ReadNumber(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, double& Out)
	{
		return Object.IsValid() && Object->TryGetNumberField(Field, Out);
	}

	/** Reads a required positive number from the fit object. */
	bool ReadFit(const TSharedPtr<FJsonObject>& Fit, const TCHAR* Field, double& Out, FString& OutError)
	{
		if (!ReadNumber(Fit, Field, Out))
		{
			OutError = FString::Printf(TEXT("fit.%s is missing or not a number"), Field);
			return false;
		}
		if (!(Out > 0.0))
		{
			OutError = FString::Printf(TEXT("fit.%s must be positive"), Field);
			return false;
		}
		return true;
	}

	TArray<FMantleVehiclePreset> LoadAll()
	{
		TArray<FMantleVehiclePreset> Presets;
		const FString Directory = FMantleVehiclePresets::Directory();
		if (Directory.IsEmpty())
		{
			UE_LOG(LogMantleVehiclePreset, Warning, TEXT("The MantleCore plugin was not found, so no vehicle presets are loaded."));
			return Presets;
		}
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *FPaths::Combine(Directory, TEXT("*.json")), true, false);
		for (const FString& File : Files)
		{
			FString Text;
			if (!FFileHelper::LoadFileToString(Text, *FPaths::Combine(Directory, File)))
			{
				UE_LOG(LogMantleVehiclePreset, Warning, TEXT("Could not read the vehicle preset %s."), *File);
				continue;
			}
			FMantleVehiclePreset Preset;
			FString Error;
			if (!FMantleVehiclePresets::Parse(Text, Preset, Error))
			{
				UE_LOG(LogMantleVehiclePreset, Warning, TEXT("Skipping the vehicle preset %s: %s"), *File, *Error);
				continue;
			}
			Presets.Add(MoveTemp(Preset));
		}
		Presets.Sort([](const FMantleVehiclePreset& A, const FMantleVehiclePreset& B) { return A.Id < B.Id; });
		return Presets;
	}

	/** True when every "*" segment of the key matches exactly one segment of the path and all other segments are equal. */
	bool KeyMatches(const FString& Key, const FString& Path)
	{
		TArray<FString> KeyParts;
		TArray<FString> PathParts;
		Key.ParseIntoArray(KeyParts, TEXT("/"), false);
		Path.ParseIntoArray(PathParts, TEXT("/"), false);
		if (KeyParts.Num() != PathParts.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < KeyParts.Num(); ++Index)
		{
			if (PathParts[Index].IsEmpty())
			{
				return false;
			}
			if (KeyParts[Index] != TEXT("*") && KeyParts[Index] != PathParts[Index])
			{
				return false;
			}
		}
		return true;
	}

	/** Every leaf path of a defaults fragment ("engine/maxTorque"); arrays and scalars are leaves, objects are walked. */
	void CollectLeaves(const TSharedPtr<FJsonObject>& Object, const FString& Prefix, TArray<FString>& Out)
	{
		for (const auto& Pair : Object->Values) // keyed by UE::TSharedString in 5.8, not FString
		{
			const FString Key(Pair.Key);
			const FString Path = Prefix.IsEmpty() ? Key : Prefix + TEXT("/") + Key;
			const TSharedPtr<FJsonObject>* Child = nullptr;
			if (Pair.Value.IsValid() && Pair.Value->TryGetObject(Child) && Child && Child->IsValid())
			{
				CollectLeaves(*Child, Path, Out);
			}
			else
			{
				Out.Add(Path);
			}
		}
	}

	/** The settings ApplyDefaults writes, by path; anything else in a preset's defaults is rejected. */
	const TSet<FString>& KnownDefaultPaths()
	{
		static const TSet<FString> Known = {
			TEXT("mass"), TEXT("maxSpeed"), TEXT("centerOfMass"), TEXT("dragCoefficient"), TEXT("frontalArea"),
			TEXT("engine/maxTorque"), TEXT("engine/maxRPM"), TEXT("engine/idleRPM"), TEXT("engine/torqueCurve"),
			TEXT("engine/tractionControl"), TEXT("engine/engineBraking"), TEXT("engine/displacement"),
			TEXT("engine/drive"), TEXT("engine/peakPower"), TEXT("engine/regenBraking"), TEXT("engine/batteryCapacity"),
			TEXT("engine/induction"), TEXT("engine/maxBoost"), TEXT("engine/spoolRPM"), TEXT("engine/spoolTime"),
			TEXT("transmission/gearRatios"), TEXT("transmission/reverseRatio"), TEXT("transmission/finalDrive"),
			TEXT("transmission/changeUpRPM"), TEXT("transmission/changeDownRPM"), TEXT("transmission/shiftTime"),
			TEXT("transmission/cvt/lowRatio"), TEXT("transmission/cvt/highRatio"), TEXT("transmission/cvt/shiftRPM"),
			TEXT("transmission/lowRange"), TEXT("transmission/differentials/front/type"), TEXT("transmission/differentials/front/biasRatio"),
			TEXT("transmission/differentials/rear/type"), TEXT("transmission/differentials/rear/biasRatio"),
			TEXT("suspension/maxRaise"), TEXT("suspension/maxDrop"), TEXT("suspension/springRate"),
			TEXT("suspension/springPreload"), TEXT("suspension/damping"),
			TEXT("steering/maxAngle"), TEXT("steering/ackermann"), TEXT("steering/speedCurve"),
			TEXT("brakes/brakeTorque"), TEXT("brakes/handbrakeTorque"), TEXT("brakes/abs"), TEXT("brakes/frontShare"),
			TEXT("tires/friction"), TEXT("tires/rearFriction"), TEXT("tires/rollingResistance"),
			TEXT("pedal/riderPower"), TEXT("pedal/sprintPower"), TEXT("pedal/sprintReserve"), TEXT("pedal/maxCadence"), TEXT("pedal/cadence"),
			TEXT("camera/armLength"), TEXT("camera/height") };
		return Known;
	}

	/** Reads values out of a defaults fragment by path; a value of the wrong kind records its path. */
	struct FDefaultsReader
	{
		TSharedPtr<FJsonObject> Root;
		FString BadPath;

		TSharedPtr<FJsonValue> Find(const FString& Path) const
		{
			TArray<FString> Parts;
			Path.ParseIntoArray(Parts, TEXT("/"), true);
			TSharedPtr<FJsonObject> Current = Root;
			for (int32 Index = 0; Index < Parts.Num(); ++Index)
			{
				const TSharedPtr<FJsonValue> Found = Current.IsValid() ? Current->TryGetField(Parts[Index]) : nullptr;
				const TSharedPtr<FJsonValue>* Value = Found.IsValid() ? &Found : nullptr;
				if (!Value || !Value->IsValid())
				{
					return nullptr;
				}
				if (Index == Parts.Num() - 1)
				{
					return *Value;
				}
				const TSharedPtr<FJsonObject>* Child = nullptr;
				if (!(*Value)->TryGetObject(Child) || !Child)
				{
					return nullptr;
				}
				Current = *Child;
			}
			return nullptr;
		}

		void Bad(const FString& Path)
		{
			if (BadPath.IsEmpty())
			{
				BadPath = Path;
			}
		}

		bool Number(const FString& Path, double& Out)
		{
			const TSharedPtr<FJsonValue> Value = Find(Path);
			if (!Value.IsValid())
			{
				return false;
			}
			if (!Value->TryGetNumber(Out))
			{
				Bad(Path);
				return false;
			}
			return true;
		}

		bool Float(const FString& Path, float& Out)
		{
			double Value = 0.0;
			if (!Number(Path, Value))
			{
				return false;
			}
			Out = static_cast<float>(Value);
			return true;
		}

		bool Int(const FString& Path, int32& Out)
		{
			double Value = 0.0;
			if (!Number(Path, Value))
			{
				return false;
			}
			Out = FMath::RoundToInt(Value);
			return true;
		}

		bool Bool(const FString& Path, bool& Out)
		{
			const TSharedPtr<FJsonValue> Value = Find(Path);
			if (!Value.IsValid())
			{
				return false;
			}
			if (!Value->TryGetBool(Out))
			{
				Bad(Path);
				return false;
			}
			return true;
		}

		/** A string (#445: the engine's drive; #444: its induction). */
		bool String(const FString& Path, FString& Out)
		{
			const TSharedPtr<FJsonValue> Value = Find(Path);
			if (!Value.IsValid())
			{
				return false;
			}
			if (Value->Type != EJson::String)
			{
				Bad(Path);
				return false;
			}
			Out = Value->AsString();
			return true;
		}

		/** An array of numbers. */
		bool Numbers(const FString& Path, TArray<double>& Out)
		{
			const TSharedPtr<FJsonValue> Value = Find(Path);
			if (!Value.IsValid())
			{
				return false;
			}
			const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
			if (!Value->TryGetArray(Items) || !Items)
			{
				Bad(Path);
				return false;
			}
			TArray<double> Read;
			for (const TSharedPtr<FJsonValue>& Item : *Items)
			{
				double Entry = 0.0;
				if (!Item.IsValid() || !Item->TryGetNumber(Entry))
				{
					Bad(Path);
					return false;
				}
				Read.Add(Entry);
			}
			Out = MoveTemp(Read);
			return true;
		}

		/** An array of [x, y] number pairs. */
		bool Pairs(const FString& Path, TArray<FVector2D>& Out)
		{
			const TSharedPtr<FJsonValue> Value = Find(Path);
			if (!Value.IsValid())
			{
				return false;
			}
			const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
			if (!Value->TryGetArray(Items) || !Items)
			{
				Bad(Path);
				return false;
			}
			TArray<FVector2D> Read;
			for (const TSharedPtr<FJsonValue>& Item : *Items)
			{
				const TArray<TSharedPtr<FJsonValue>>* Pair = nullptr;
				double X = 0.0;
				double Y = 0.0;
				if (!Item.IsValid() || !Item->TryGetArray(Pair) || !Pair || Pair->Num() != 2
					|| !(*Pair)[0].IsValid() || !(*Pair)[1].IsValid() || !(*Pair)[0]->TryGetNumber(X) || !(*Pair)[1]->TryGetNumber(Y))
				{
					Bad(Path);
					return false;
				}
				Read.Add(FVector2D(X, Y));
			}
			Out = MoveTemp(Read);
			return true;
		}

		/** An array of three numbers. */
		bool Triple(const FString& Path, FVector& Out)
		{
			TArray<double> Items;
			if (!Numbers(Path, Items))
			{
				return false;
			}
			if (Items.Num() != 3)
			{
				Bad(Path);
				return false;
			}
			Out = FVector(Items[0], Items[1], Items[2]);
			return true;
		}
	};

	/** "a, b and c" for the words in a settings sentence. */
	FString JoinWords(const TArray<FString>& Words)
	{
		if (Words.Num() <= 1)
		{
			return Words.Num() == 1 ? Words[0] : FString();
		}
		TArray<FString> Head = Words;
		const FString Last = Head.Pop();
		return FString::Join(Head, TEXT(", ")) + TEXT(" and ") + Last;
	}
}

FString FMantleVehiclePresets::Directory()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("MantleCore"));
	if (!Plugin.IsValid())
	{
		return FString();
	}
	return FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources"), TEXT("VehiclePresets"));
}

const TArray<FMantleVehiclePreset>& FMantleVehiclePresets::All()
{
	static const TArray<FMantleVehiclePreset> Presets = MantleVehiclePresetPrivate::LoadAll();
	return Presets;
}

TArray<const FMantleVehiclePreset*> FMantleVehiclePresets::Publishable()
{
	TArray<const FMantleVehiclePreset*> Out;
	for (const FMantleVehiclePreset& Preset : All())
	{
		if (Preset.IsPublishable())
		{
			Out.Add(&Preset);
		}
	}
	// Road vehicles first, then tracked ones (#448), then what flies, then what floats, then what is towed (#446); within a movement type the
	// order stays by id.
	const auto Rank = [](const FString& MovementType)
	{
		if (MovementType == TEXT("wheeled")) { return 0; }
		if (MovementType == TEXT("tracked")) { return 1; }
		if (MovementType == TEXT("plane")) { return 2; }
		if (MovementType == TEXT("helicopter")) { return 3; }
		if (MovementType == TEXT("hover")) { return 4; }
		if (MovementType == TEXT("trailer")) { return 6; }
		return 5;
	};
	Out.StableSort([&Rank](const FMantleVehiclePreset& A, const FMantleVehiclePreset& B) { return Rank(A.MovementType) < Rank(B.MovementType); });
	return Out;
}

FString FMantleVehiclePresets::CardSentence(const FMantleVehiclePreset& Preset)
{
	// The cards of the three tall road vehicles: each names its own kinds, so no two cards offer the same vehicle.
	static const TPair<const TCHAR*, const TCHAR*> OwnLines[] = {
		{ TEXT("pickup"), TEXT("A tall road vehicle with long suspension: pickups and SUVs.") },
		{ TEXT("van"), TEXT("A tall box-bodied van: delivery vans, minibuses and ambulances.") },
		{ TEXT("truck"), TEXT("A large, heavy road vehicle: box trucks, delivery trucks and campervans.") },
	};
	// Its own drive (#376 off-road follow-on), where a creator may change it.
	const FString Drive = FMantleVehiclePresets::DriveWords(Preset.DefaultDrive);
	// Before the sentence's full stop, so the card stays one sentence: "... pickups and SUVs (rear-wheel drive)."
	const auto WithDrive = [&Drive](const FString& Sentence)
	{
		return Drive.IsEmpty() || !Sentence.EndsWith(TEXT(".")) ? Sentence : Sentence.LeftChop(1) + TEXT(" (") + Drive + TEXT(").");
	};
	for (const TPair<const TCHAR*, const TCHAR*>& Own : OwnLines)
	{
		if (Preset.Id == Own.Key && !Preset.Description.TrimStartAndEnd().IsEmpty())
		{
			return WithDrive(Own.Value);
		}
	}
	FString Text = Preset.Description.TrimStartAndEnd();
	// A sentence ends at a full stop followed by a space; the last one ends at the text's end.
	for (int32 Index = 0; Index + 1 < Text.Len(); ++Index)
	{
		if (Text[Index] == TEXT('.') && FChar::IsWhitespace(Text[Index + 1]))
		{
			Text.LeftInline(Index + 1);
			break;
		}
	}
	if (!Text.IsEmpty() && !Text.EndsWith(TEXT(".")))
	{
		Text += TEXT(".");
	}
	return WithDrive(Text);
}

FString FMantleVehiclePresets::DriveWords(const EMantleVehicleDrive Drive)
{
	return Drive == EMantleVehicleDrive::Front ? TEXT("front-wheel drive") : Drive == EMantleVehicleDrive::Rear ? TEXT("rear-wheel drive")
		: Drive == EMantleVehicleDrive::All ? TEXT("all-wheel drive") : FString();
}

const FMantleVehiclePreset* FMantleVehiclePresets::Find(const FString& Id)
{
	FString FileId;
	FString Engine;
	FMantleChassisSpec::SplitPresetId(Id, FileId, Engine);
	if (Engine.IsEmpty())
	{
		return All().FindByPredicate([&Id](const FMantleVehiclePreset& Preset) { return Preset.Id == Id; });
	}
	// #378: the type with one of its engines, read once and kept (game thread only, as All is).
	static TMap<FString, TUniquePtr<FMantleVehiclePreset>> WithEngines;
	if (const TUniquePtr<FMantleVehiclePreset>* Found = WithEngines.Find(Id))
	{
		return Found->Get();
	}
	FString Text;
	FString Error;
	TUniquePtr<FMantleVehiclePreset> Read = MakeUnique<FMantleVehiclePreset>();
	if (!FMantleVehicleChassisBuilder::LoadPresetText(Id, Text, Error) || !Parse(Text, *Read, Error))
	{
		UE_LOG(LogMantleVehiclePreset, Warning, TEXT("The vehicle preset %s could not be read: %s"), *Id, *Error);
		return nullptr;
	}
	Read->EngineId = Engine;
	return WithEngines.Add(Id, MoveTemp(Read)).Get();
}

FString FMantleVehiclePresets::SelectorIdOf(const FName Preset, const FName PresetEngine)
{
	return PresetEngine.IsNone() ? Preset.ToString() : Preset.ToString() + TEXT("+") + PresetEngine.ToString();
}

bool FMantleVehiclePresets::Parse(const FString& Json, FMantleVehiclePreset& Out, FString& OutError)
{
	using namespace MantleVehiclePresetPrivate;
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		OutError = TEXT("not valid JSON");
		return false;
	}
	double Version = 0.0;
	if (!Root->TryGetNumberField(TEXT("schemaVersion"), Version) || static_cast<int32>(Version) != SchemaVersion)
	{
		OutError = FString::Printf(TEXT("schemaVersion must be %d"), SchemaVersion);
		return false;
	}

	FMantleVehiclePreset Preset;
	Root->TryGetStringField(TEXT("description"), Preset.Description);
	if (!Root->TryGetStringField(TEXT("id"), Preset.Id) || Preset.Id.IsEmpty())
	{
		OutError = TEXT("id is missing");
		return false;
	}
	if (!Root->TryGetStringField(TEXT("name"), Preset.Name) || Preset.Name.IsEmpty())
	{
		OutError = TEXT("name is missing");
		return false;
	}
	if (!Root->TryGetStringField(TEXT("movementType"), Preset.MovementType) || Preset.MovementType.IsEmpty())
	{
		OutError = TEXT("movementType is missing");
		return false;
	}

	// The flag is optional and false when absent; a value that is not true or false is a mistake in the file, not a missing flag.
	if (Root->HasField(TEXT("publishable")) && !Root->TryGetBoolField(TEXT("publishable"), Preset.bPublishable))
	{
		OutError = TEXT("publishable must be true or false");
		return false;
	}

	const TSharedPtr<FJsonObject>* Seats = nullptr;
	if (Root->TryGetObjectField(TEXT("seats"), Seats) && Seats)
	{
		(*Seats)->TryGetStringField(TEXT("driver"), Preset.DriverSeat);
		(*Seats)->TryGetStringField(TEXT("passenger"), Preset.PassengerSeat);
	}

	const TSharedPtr<FJsonObject>* Fit = nullptr;
	if (!Root->TryGetObjectField(TEXT("fit"), Fit) || !Fit)
	{
		OutError = TEXT("fit is missing");
		return false;
	}
	if (!ReadFit(*Fit, TEXT("wheelRadius"), Preset.FitWheelRadius, OutError)
		|| !ReadFit(*Fit, TEXT("trackWidth"), Preset.FitTrackWidth, OutError)
		|| !ReadFit(*Fit, TEXT("minScale"), Preset.FitMinScale, OutError)
		|| !ReadFit(*Fit, TEXT("maxScale"), Preset.FitMaxScale, OutError))
	{
		return false;
	}
	if (Preset.FitMaxScale < Preset.FitMinScale)
	{
		OutError = TEXT("fit.maxScale is below fit.minScale");
		return false;
	}

	const TSharedPtr<FJsonObject>* Wheels = nullptr;
	if (Root->TryGetObjectField(TEXT("wheels"), Wheels) && Wheels)
	{
		(*Wheels)->TryGetNumberField(TEXT("min"), Preset.MinWheels);
		(*Wheels)->TryGetNumberField(TEXT("max"), Preset.MaxWheels);
	}

	const TSharedPtr<FJsonObject>* Defaults = nullptr;
	if (Root->TryGetObjectField(TEXT("defaults"), Defaults) && Defaults)
	{
		Preset.Defaults = *Defaults;
	}

	// #378: the engines a type offers (a motorcycle's sizes) and the one it starts with.
	Root->TryGetStringField(TEXT("defaultEngine"), Preset.DefaultEngine);
	const TArray<TSharedPtr<FJsonValue>>* Engines = nullptr;
	if (Root->TryGetArrayField(TEXT("engines"), Engines) && Engines)
	{
		for (const TSharedPtr<FJsonValue>& Entry : *Engines)
		{
			const TSharedPtr<FJsonObject>* Engine = nullptr;
			FMantleVehiclePresetEngine Read;
			if (!Entry.IsValid() || !Entry->TryGetObject(Engine) || !Engine || !(*Engine)->TryGetStringField(TEXT("id"), Read.Id) || Read.Id.IsEmpty()
				|| !(*Engine)->TryGetStringField(TEXT("name"), Read.Name))
			{
				OutError = TEXT("an engine needs an id and a name");
				return false;
			}
			(*Engine)->TryGetStringField(TEXT("description"), Read.Description);
			Preset.Engines.Add(MoveTemp(Read));
		}
		if (!Preset.Engines.ContainsByPredicate([&Preset](const FMantleVehiclePresetEngine& E) { return E.Id == Preset.DefaultEngine; }))
		{
			OutError = TEXT("defaultEngine is not one of the engines");
			return false;
		}
	}

	const TSharedPtr<FJsonObject>* Ranges = nullptr;
	if (Root->TryGetObjectField(TEXT("ranges"), Ranges) && Ranges)
	{
		for (const auto& Pair : (*Ranges)->Values) // keyed by UE::TSharedString in 5.8, not FString
		{
			const FString Key(Pair.Key);
			const TArray<TSharedPtr<FJsonValue>>* Pair2 = nullptr;
			double Min = 0.0;
			double Max = 0.0;
			if (!Pair.Value.IsValid() || !Pair.Value->TryGetArray(Pair2) || !Pair2 || Pair2->Num() != 2
				|| !(*Pair2)[0]->TryGetNumber(Min) || !(*Pair2)[1]->TryGetNumber(Max))
			{
				OutError = FString::Printf(TEXT("range %s must be an array of two numbers"), *Key);
				return false;
			}
			if (Min > Max)
			{
				OutError = FString::Printf(TEXT("range %s has its minimum above its maximum"), *Key);
				return false;
			}
			Preset.Ranges.Add(Key, FVector2D(Min, Max));
		}
	}

	// Its own drive (#376 off-road follow-on), for its card: from the chassis's axles where the drive is a creator's choice.
	{
		FMantleChassisSpec Spec;
		FString SpecError;
		Preset.DefaultDrive = FMantleChassisSpec::Parse(Json, Spec, SpecError) ? Spec.DefaultDrive() : EMantleVehicleDrive::Type;
	}
	Out = MoveTemp(Preset);
	return true;
}

bool FMantleVehiclePresets::RangeFor(const FMantleVehiclePreset& Preset, const FString& Path, FVector2D& Out)
{
	if (const FVector2D* Exact = Preset.Ranges.Find(Path))
	{
		Out = *Exact;
		return true;
	}
	for (const TPair<FString, FVector2D>& Pair : Preset.Ranges)
	{
		if (MantleVehiclePresetPrivate::KeyMatches(Pair.Key, Path))
		{
			Out = Pair.Value;
			return true;
		}
	}
	return false;
}

bool FMantleVehiclePresets::ApplyDefaults(const FMantleVehiclePreset& Preset, UMantleVehicleSource& Source, TArray<FString>& OutNotes)
{
	using namespace MantleVehiclePresetPrivate;
	if (!Preset.Defaults.IsValid())
	{
		OutNotes.Add(FString::Printf(TEXT("The %s preset has no settings to apply."), *Preset.Name));
		return true;
	}

	// Refuse before changing anything: a key this version does not know would otherwise be ignored without a word.
	TArray<FString> Leaves;
	CollectLeaves(Preset.Defaults, FString(), Leaves);
	for (const FString& Leaf : Leaves)
	{
		if (!KnownDefaultPaths().Contains(Leaf))
		{
			OutNotes.Add(FString::Printf(TEXT("The %s preset has a setting called '%s' that this version of Mantle does not know, so none of its settings were applied."), *Preset.Name, *Leaf));
			return false;
		}
	}

	// Read into a copy so a value of the wrong kind leaves the vehicle as it was.
	FMantleVehicleSourceTuning T = Source.Tuning;
	FDefaultsReader Reader;
	Reader.Root = Preset.Defaults;

	Reader.Float(TEXT("mass"), T.Mass);
	Reader.Float(TEXT("maxSpeed"), T.MaxSpeed);
	if (Reader.Triple(TEXT("centerOfMass"), T.CenterOfMass))
	{
		T.bOverrideCenterOfMass = true;
	}
	Reader.Float(TEXT("dragCoefficient"), T.DragCoefficient);
	Reader.Float(TEXT("frontalArea"), T.FrontalArea);

	// #445: an electric motor, with its power, regen and battery. A preset sets the drive whole: a combustion preset makes the vehicle combustion again.
	FString Drive;
	T.bElectric = Reader.String(TEXT("engine/drive"), Drive) && Drive == TEXT("electric");
	Reader.Float(TEXT("engine/peakPower"), T.PeakPower);
	Reader.Float(TEXT("engine/regenBraking"), T.RegenBraking);
	Reader.Float(TEXT("engine/batteryCapacity"), T.BatteryCapacity);
	Reader.Float(TEXT("engine/maxTorque"), T.MaxTorque);
	Reader.Int(TEXT("engine/maxRPM"), T.MaxRPM);
	Reader.Int(TEXT("engine/idleRPM"), T.IdleRPM);
	Reader.Pairs(TEXT("engine/torqueCurve"), T.TorqueCurve);

	TArray<double> Gears;
	if (Reader.Numbers(TEXT("transmission/gearRatios"), Gears))
	{
		T.GearRatios.Reset();
		for (const double Ratio : Gears)
		{
			T.GearRatios.Add(static_cast<float>(Ratio));
		}
	}
	Reader.Float(TEXT("transmission/reverseRatio"), T.ReverseRatio);
	Reader.Float(TEXT("transmission/finalDrive"), T.FinalDrive);
	Reader.Int(TEXT("transmission/changeUpRPM"), T.ChangeUpRPM);
	Reader.Int(TEXT("transmission/changeDownRPM"), T.ChangeDownRPM);
	// #376: each gear change's time, part of the car; absent, the tuning keeps its own (0.5 s).
	Reader.Float(TEXT("transmission/shiftTime"), T.ShiftTime);
	// ATV follow-on: a belt drive. A preset sets it whole: one without a belt makes the vehicle geared again.
	T.bCvt = Reader.Find(TEXT("transmission/cvt/lowRatio")).IsValid();
	Reader.Float(TEXT("transmission/cvt/lowRatio"), T.CvtLowRatio);
	Reader.Float(TEXT("transmission/cvt/highRatio"), T.CvtHighRatio);
	Reader.Int(TEXT("transmission/cvt/shiftRPM"), T.CvtShiftRPM);
	// #376 off-road: a low range and the differentials. A preset sets them whole: one without them takes them away.
	T.LowRange = 0.f;
	Reader.Float(TEXT("transmission/lowRange"), T.LowRange);
	FString Front, Rear;
	T.FrontDifferential = Reader.String(TEXT("transmission/differentials/front/type"), Front) ? MantleDifferentialFromName(Front) : EMantleVehicleDifferential::None;
	T.RearDifferential = Reader.String(TEXT("transmission/differentials/rear/type"), Rear) ? MantleDifferentialFromName(Rear) : EMantleVehicleDifferential::None;
	Reader.Float(TEXT("transmission/differentials/front/biasRatio"), T.FrontDifferentialBias);
	Reader.Float(TEXT("transmission/differentials/rear/biasRatio"), T.RearDifferentialBias);

	Reader.Float(TEXT("suspension/maxRaise"), T.SuspensionMaxRaise);
	Reader.Float(TEXT("suspension/maxDrop"), T.SuspensionMaxDrop);
	Reader.Float(TEXT("suspension/springRate"), T.SpringRate);
	Reader.Float(TEXT("suspension/springPreload"), T.SpringPreload);
	Reader.Float(TEXT("suspension/damping"), T.SpringDamping);

	Reader.Float(TEXT("steering/maxAngle"), T.MaxSteeringAngle);
	Reader.Bool(TEXT("steering/ackermann"), T.bAckermann);
	// #821: a ground car's handling baked into the preset's document (the steering's speed curve, the rear tires' own grip); absent, the vehicle keeps its own.
	Reader.Pairs(TEXT("steering/speedCurve"), T.SteeringSpeedCurve);

	Reader.Float(TEXT("brakes/brakeTorque"), T.BrakeTorque);
	Reader.Float(TEXT("brakes/handbrakeTorque"), T.HandbrakeTorque);
	// #376: the driver aids and brake balance, part of the car; absent, the tuning keeps its own (ABS and traction control on, 0.65, none).
	Reader.Bool(TEXT("brakes/abs"), T.bABS);
	Reader.Float(TEXT("brakes/frontShare"), T.BrakeFrontShare);
	Reader.Bool(TEXT("engine/tractionControl"), T.bTractionControl);
	Reader.Float(TEXT("engine/engineBraking"), T.EngineBraking);
	// #378: the engine's size, from a motorcycle's engine pick; absent, the tuning keeps its own (0, not given).
	Reader.Int(TEXT("engine/displacement"), T.EngineDisplacement);
	// An electric preset (#445) is a motor: no engine size and no low range (#376 off-road), whatever the vehicle had.
	if (T.bElectric)
	{
		T.EngineDisplacement = 0;
		T.LowRange = 0.f;
	}
	// #444: the preset's engine is the engine, so its induction is applied whole: absent, none and the boost settings' defaults (a turbo car's boost
	// would otherwise stay on a preset without one).
	{
		const FMantleVehicleSourceTuning Plain;
		T.Induction = Plain.Induction;
		T.MaxBoost = Plain.MaxBoost;
		T.SpoolRPM = Plain.SpoolRPM;
		T.SpoolTime = Plain.SpoolTime;
		FString Induction;
		if (Reader.String(TEXT("engine/induction"), Induction))
		{
			if (Induction == TEXT("turbo")) T.Induction = EMantleVehicleSourceInduction::Turbo;
			else if (Induction == TEXT("supercharger")) T.Induction = EMantleVehicleSourceInduction::Supercharger;
			else if (Induction != TEXT("none")) Reader.Bad(TEXT("engine/induction"));
		}
		Reader.Float(TEXT("engine/maxBoost"), T.MaxBoost);
		Reader.Int(TEXT("engine/spoolRPM"), T.SpoolRPM);
		Reader.Float(TEXT("engine/spoolTime"), T.SpoolTime);
	}

	Reader.Float(TEXT("tires/friction"), T.TireFriction);
	if (Reader.Float(TEXT("tires/rearFriction"), T.TireRearFriction))
	{
		T.bTireRearFriction = true;
	}
	// #447, #448: how much every wheel loses to rolling; absent, the tuning keeps its own (a car tire's 0.012).
	Reader.Float(TEXT("tires/rollingResistance"), T.TireRollingResistance);
	// #447: a pedalled preset (the Bicycle): any pedal number makes the vehicle pedalled; an absent one keeps the tuning's own.
	bool bAnyPedal = false;
	bAnyPedal |= Reader.Float(TEXT("pedal/riderPower"), T.PedalRiderPower);
	bAnyPedal |= Reader.Float(TEXT("pedal/sprintPower"), T.PedalSprintPower);
	bAnyPedal |= Reader.Float(TEXT("pedal/sprintReserve"), T.PedalSprintReserve);
	bAnyPedal |= Reader.Float(TEXT("pedal/maxCadence"), T.PedalMaxCadence);
	bAnyPedal |= Reader.Float(TEXT("pedal/cadence"), T.PedalCadence);
	T.bPedal = bAnyPedal;

	Reader.Float(TEXT("camera/armLength"), T.CameraArmLength);
	Reader.Float(TEXT("camera/height"), T.CameraHeight);

	if (!Reader.BadPath.IsEmpty())
	{
		OutNotes.Add(FString::Printf(TEXT("The %s preset's setting '%s' is not the kind of value Mantle expects, so none of its settings were applied."), *Preset.Name, *Reader.BadPath));
		return false;
	}

	Source.Modify();
	Source.Tuning = T;

	// One or two plain lines naming what was set, by the groups the preset carried.
	auto Has = [&Leaves](const FString& Prefix)
	{
		return Leaves.ContainsByPredicate([&Prefix](const FString& Leaf) { return Leaf == Prefix || Leaf.StartsWith(Prefix + TEXT("/")); });
	};
	TArray<FString> Groups;
	if (Has(TEXT("mass")) || Has(TEXT("maxSpeed")) || Has(TEXT("centerOfMass")) || Has(TEXT("dragCoefficient")) || Has(TEXT("frontalArea")))
	{
		Groups.Add(TEXT("weight and top speed"));
	}
	if (Has(TEXT("engine")))
	{
		Groups.Add(TEXT("engine"));
	}
	if (Has(TEXT("transmission")))
	{
		Groups.Add(TEXT("gears"));
	}
	if (Has(TEXT("suspension")))
	{
		Groups.Add(TEXT("suspension"));
	}
	if (Has(TEXT("steering")))
	{
		Groups.Add(TEXT("steering"));
	}
	if (Has(TEXT("brakes")))
	{
		Groups.Add(TEXT("brakes"));
	}
	if (Has(TEXT("tires")))
	{
		Groups.Add(TEXT("tires"));
	}
	if (Has(TEXT("camera")))
	{
		Groups.Add(TEXT("camera"));
	}
	if (Groups.Num() > 0)
	{
		OutNotes.Add(FString::Printf(TEXT("Set the %s's %s."), *Preset.Name, *JoinWords(Groups)));
		OutNotes.Add(TEXT("Your mesh, wheels, seats and doors were left as they were."));
	}
	return true;
}
