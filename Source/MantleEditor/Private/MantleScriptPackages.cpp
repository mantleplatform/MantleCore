// Copyright Joshua Eger. All Rights Reserved.
#include "MantleScriptPackages.h"
#include "MantleApiManifest.h"
#include "MantlePlaceExporter.h"
#include "MantlePlaceLimits.h"
#include "MantlePlaceParser.h"
#include "MantlePlaceStorage.h"
#include "MantleScriptError.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleScriptPackagesPrivate
{
	const TCHAR* const PackagesPrefix = TEXT("Packages/");

	/** True when Text holds a control character; bAllowLineBreaks lets \n, \r and \t through (a description's lines). */
	bool HasControlCharacter(const FString& Text, const bool bAllowLineBreaks)
	{
		for (const TCHAR Character : Text)
		{
			if (bAllowLineBreaks && (Character == TEXT('\n') || Character == TEXT('\r') || Character == TEXT('\t')))
			{
				continue;
			}
			if (Character < 0x20 || Character == 0x7f)
			{
				return true;
			}
		}
		return false;
	}

	bool ParseParameterType(const FString& Text, EMantleScriptPackageParameterType& Out)
	{
		if (Text.Equals(TEXT("string"), ESearchCase::CaseSensitive))
		{
			Out = EMantleScriptPackageParameterType::String;
			return true;
		}
		if (Text.Equals(TEXT("number"), ESearchCase::CaseSensitive))
		{
			Out = EMantleScriptPackageParameterType::Number;
			return true;
		}
		if (Text.Equals(TEXT("boolean"), ESearchCase::CaseSensitive))
		{
			Out = EMantleScriptPackageParameterType::Boolean;
			return true;
		}
		if (Text.Equals(TEXT("choice"), ESearchCase::CaseSensitive))
		{
			Out = EMantleScriptPackageParameterType::Choice;
			return true;
		}
		return false;
	}

	/** A JSON scalar as the text a Script Point stores: a string as is, a number written out ("5", "2.5"), a boolean "true" or "false". */
	bool ScalarText(const TSharedPtr<FJsonValue>& Value, FString& Out)
	{
		if (!Value.IsValid())
		{
			return false;
		}
		switch (Value->Type)
		{
		case EJson::String:
			Out = Value->AsString();
			return true;
		case EJson::Number:
			if (!FMath::IsFinite(Value->AsNumber()))
			{
				return false;
			}
			Out = FString::SanitizeFloat(Value->AsNumber(), 0);
			return true;
		case EJson::Boolean:
			Out = Value->AsBool() ? TEXT("true") : TEXT("false");
			return true;
		default:
			return false;
		}
	}

	TSharedPtr<FJsonObject> ParseObject(const FString& Json)
	{
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			return nullptr;
		}
		return Root;
	}

	const TCHAR* SideOfTopFolder(const FString& PathInPackage)
	{
		if (PathInPackage.StartsWith(TEXT("Server/"), ESearchCase::CaseSensitive))
		{
			return TEXT("server");
		}
		if (PathInPackage.StartsWith(TEXT("Client/"), ESearchCase::CaseSensitive))
		{
			return TEXT("client");
		}
		if (PathInPackage.StartsWith(TEXT("Shared/"), ESearchCase::CaseSensitive))
		{
			return TEXT("shared");
		}
		return TEXT("");
	}

	/**
	 * One option list of package.json: "parameters" or "settings", Noun "parameter" or "setting", by
	 * FMantleScriptPackages::ReadManifest's rules. Every problem goes to OutProblems in plain words naming package.json.
	 */
	void ReadManifestOptions(const TSharedPtr<FJsonObject>& Root, const TCHAR* Field, const TCHAR* Noun, const int32 Limit,
		TArray<FMantleScriptPackageParameter>& OutOptions, TArray<FString>& OutProblems)
	{
		if (!Root->HasField(Field))
		{
			return;
		}
		const TArray<TSharedPtr<FJsonValue>>* Options = nullptr;
		if (!Root->TryGetArrayField(Field, Options) || !Options)
		{
			OutProblems.Add(FString::Printf(TEXT("package.json's %s is not a list. Write it as [ {\"name\": \"Damage\", \"type\": \"number\", \"default\": 100} ]."), Field));
			return;
		}
		if (Options->Num() > Limit)
		{
			OutProblems.Add(FString::Printf(TEXT("package.json declares %d %ss; a package declares at most %d. Remove some."), Options->Num(), Noun, Limit));
		}
		for (int32 Index = 0; Index < Options->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (!(*Options)[Index].IsValid() || !(*Options)[Index]->TryGetObject(Object) || !Object || !Object->IsValid())
			{
				OutProblems.Add(FString::Printf(TEXT("package.json's %s %d is not an object like {\"name\": \"Damage\", \"type\": \"number\", \"default\": 100}."), Noun, Index + 1));
				continue;
			}
			FMantleScriptPackageParameter Parameter;
			if (!(*Object)->TryGetStringField(TEXT("name"), Parameter.Name) || !FMantlePlaceLimits::IsScriptPointParameterName(Parameter.Name))
			{
				OutProblems.Add(FString::Printf(TEXT("package.json's %s %d is named '%s'; a %s name is a letter followed by up to 31 letters, digits or '_'."),
					Noun, Index + 1, *Parameter.Name, Noun));
				continue;
			}
			const FString& Name = Parameter.Name;
			if (const FMantleScriptPackageParameter* Earlier = OutOptions.FindByPredicate(
				[&Name](const FMantleScriptPackageParameter& Option) { return Option.Name.Equals(Name, ESearchCase::IgnoreCase); }))
			{
				OutProblems.Add(Earlier->Name.Equals(Name, ESearchCase::CaseSensitive)
					? FString::Printf(TEXT("package.json declares the %s '%s' twice. Remove one."), Noun, *Name)
					: FString::Printf(TEXT("package.json declares the %s '%s' twice (once as '%s'); names are unique ignoring letter case. Remove or rename one."),
						Noun, *Name, *Earlier->Name));
				continue;
			}
			FString TypeText;
			if (!(*Object)->TryGetStringField(TEXT("type"), TypeText) || !ParseParameterType(TypeText, Parameter.Type))
			{
				OutProblems.Add(FString::Printf(TEXT("package.json's %s '%s' has type '%s'; a %s's type is \"string\", \"number\", \"boolean\" or \"choice\"."),
					Noun, *Parameter.Name, *TypeText, Noun));
				continue;
			}
			const TCHAR* TypeName = FMantleScriptPackages::ParameterTypeName(Parameter.Type);
			const bool bNumber = Parameter.Type == EMantleScriptPackageParameterType::Number;
			const bool bChoice = Parameter.Type == EMantleScriptPackageParameterType::Choice;

			// required: true or false. Checked by JSON type: TryGetBoolField would read the string "yes" as true.
			if (const TSharedPtr<FJsonValue> RequiredValue = (*Object)->TryGetField(TEXT("required")); RequiredValue.IsValid())
			{
				if (RequiredValue->Type == EJson::Boolean)
				{
					Parameter.bRequired = RequiredValue->AsBool();
				}
				else
				{
					OutProblems.Add(FString::Printf(TEXT("package.json's %s '%s' has a required that is not true or false. Write it without quotes."), Noun, *Parameter.Name));
				}
			}

			// min and max: a number's only, finite, min at most max.
			for (const TCHAR* Bound : { TEXT("min"), TEXT("max") })
			{
				if (!(*Object)->HasField(Bound))
				{
					continue;
				}
				const TSharedPtr<FJsonValue> Value = (*Object)->TryGetField(Bound);
				if (!bNumber)
				{
					OutProblems.Add(FString::Printf(TEXT("package.json's %s %s '%s' has a %s; only a number %s has one. Remove it."), TypeName, Noun, *Parameter.Name, Bound, Noun));
					continue;
				}
				if (!Value.IsValid() || Value->Type != EJson::Number || !FMath::IsFinite(Value->AsNumber()))
				{
					OutProblems.Add(FString::Printf(TEXT("package.json's %s '%s' has a %s that is not a number. Write it without quotes, like 10."), Noun, *Parameter.Name, Bound));
					continue;
				}
				const bool bMin = FCString::Strcmp(Bound, TEXT("min")) == 0;
				(bMin ? Parameter.bHasMin : Parameter.bHasMax) = true;
				(bMin ? Parameter.Min : Parameter.Max) = Value->AsNumber();
			}
			if (Parameter.bHasMin && Parameter.bHasMax && Parameter.Min > Parameter.Max)
			{
				OutProblems.Add(FString::Printf(TEXT("package.json's %s '%s' has a max (%s) below its min (%s). Swap them."), Noun, *Parameter.Name,
					*FMantleScriptPackages::FormatOptionNumber(Parameter.Max), *FMantleScriptPackages::FormatOptionNumber(Parameter.Min)));
			}

			// options: a choice's only, 2 to 32 distinct texts of 1 to 64 characters.
			if ((*Object)->HasField(TEXT("options")))
			{
				const TArray<TSharedPtr<FJsonValue>>* Choices = nullptr;
				if (!bChoice)
				{
					OutProblems.Add(FString::Printf(TEXT("package.json's %s %s '%s' has options; only a choice %s has them. Remove them, or make its type \"choice\"."),
						TypeName, Noun, *Parameter.Name, Noun));
				}
				else if (!(*Object)->TryGetArrayField(TEXT("options"), Choices) || !Choices)
				{
					OutProblems.Add(FString::Printf(TEXT("package.json's choice %s '%s' has options that are not a list. Write them as [\"Easy\", \"Hard\"]."), Noun, *Parameter.Name));
				}
				else
				{
					for (const TSharedPtr<FJsonValue>& Choice : *Choices)
					{
						FString Text;
						if (!Choice.IsValid() || Choice->Type != EJson::String || !Choice->TryGetString(Text) || Text.IsEmpty()
							|| FMantlePlaceParser::CodePointLength(Text) > FMantlePlaceLimits::MaxScriptChoiceOptionLength
							|| HasControlCharacter(Text, false) || !Text.TrimStartAndEnd().Equals(Text, ESearchCase::CaseSensitive))
						{
							OutProblems.Add(FString::Printf(TEXT("package.json's choice %s '%s' has an option that is not 1 to %d characters of text on one line without surrounding spaces."),
								Noun, *Parameter.Name, FMantlePlaceLimits::MaxScriptChoiceOptionLength));
							continue;
						}
						if (Parameter.HasOption(Text))
						{
							OutProblems.Add(FString::Printf(TEXT("package.json's choice %s '%s' lists the option '%s' twice. Remove one."), Noun, *Parameter.Name, *Text));
							continue;
						}
						Parameter.Options.Add(MoveTemp(Text));
					}
				}
			}
			if (bChoice && (Parameter.Options.Num() < FMantlePlaceLimits::MinScriptChoiceOptions || Parameter.Options.Num() > FMantlePlaceLimits::MaxScriptChoiceOptions))
			{
				OutProblems.Add(FString::Printf(TEXT("package.json's choice %s '%s' lists %d options; a choice lists %d to %d, like \"options\": [\"Easy\", \"Hard\"]."),
					Noun, *Parameter.Name, Parameter.Options.Num(), FMantlePlaceLimits::MinScriptChoiceOptions, FMantlePlaceLimits::MaxScriptChoiceOptions));
			}

			// default: written as the type's JSON (a choice's as text), short, trimmed, and fitting the range or the options.
			const TSharedPtr<FJsonValue> Default = (*Object)->TryGetField(TEXT("default"));
			if (Default.IsValid() && Default->Type != EJson::Null)
			{
				const EJson Wanted = bNumber ? EJson::Number
					: Parameter.Type == EMantleScriptPackageParameterType::Boolean ? EJson::Boolean : EJson::String;
				FString Text;
				if (Default->Type != Wanted || !ScalarText(Default, Text))
				{
					OutProblems.Add(FString::Printf(TEXT("package.json's %s %s '%s' has a default that is not a %s. Write it as %s."),
						TypeName, Noun, *Parameter.Name, TypeName,
						bNumber ? TEXT("a number without quotes, like 100")
							: Parameter.Type == EMantleScriptPackageParameterType::Boolean ? TEXT("true or false, without quotes")
							: bChoice ? TEXT("one of its options, between quotes")
							: TEXT("text between quotes")));
				}
				else if (FMantlePlaceParser::CodePointLength(Text) > FMantlePlaceLimits::MaxScriptPointParameterValueLength
					|| HasControlCharacter(Text, false) || !Text.TrimStartAndEnd().Equals(Text, ESearchCase::CaseSensitive))
				{
					OutProblems.Add(FString::Printf(TEXT("package.json's %s '%s' has a default longer than %d characters, on more than one line, or starting or ending with a space. Shorten or trim it."),
						Noun, *Parameter.Name, FMantlePlaceLimits::MaxScriptPointParameterValueLength));
				}
				else if (bChoice && !Parameter.HasOption(Text))
				{
					OutProblems.Add(FString::Printf(TEXT("package.json's choice %s '%s' has the default '%s', which is not one of its options. Make it one of %s."),
						Noun, *Parameter.Name, *Text, *FString::Join(Parameter.Options, TEXT(", "))));
				}
				else if (bNumber && ((Parameter.bHasMin && Default->AsNumber() < Parameter.Min) || (Parameter.bHasMax && Default->AsNumber() > Parameter.Max)))
				{
					OutProblems.Add(FString::Printf(TEXT("package.json's %s '%s' has the default %s, outside its min and max. Make it fit them."), Noun, *Parameter.Name, *Text));
				}
				else
				{
					Parameter.bHasDefault = true;
					Parameter.Default = Text;
				}
			}
			else if (Parameter.Type != EMantleScriptPackageParameterType::String && !Parameter.bRequired)
			{
				OutProblems.Add(FString::Printf(TEXT("package.json's %s %s '%s' has no default. Give it one, like \"default\": %s, so a place that leaves it out still has a value, or mark it \"required\": true."),
					TypeName, Noun, *Parameter.Name,
					bNumber ? TEXT("0") : bChoice ? TEXT("\"<one of its options>\"") : TEXT("false")));
			}

			if ((*Object)->HasField(TEXT("help")))
			{
				FString Help;
				if (!(*Object)->TryGetStringField(TEXT("help"), Help))
				{
					OutProblems.Add(FString::Printf(TEXT("package.json's %s '%s' has help that is not text. Write it between quotes."), Noun, *Parameter.Name));
				}
				else
				{
					Parameter.Help = Help.TrimStartAndEnd();
					if (FMantlePlaceParser::CodePointLength(Parameter.Help) > FMantleScriptPackages::MaxParameterHelpLength || HasControlCharacter(Parameter.Help, false))
					{
						OutProblems.Add(FString::Printf(TEXT("package.json's %s '%s' has help longer than %d characters or on more than one line. Shorten it."),
							Noun, *Parameter.Name, FMantleScriptPackages::MaxParameterHelpLength));
					}
				}
			}
			if (OutOptions.Num() < Limit)
			{
				OutOptions.Add(MoveTemp(Parameter));
			}
		}
	}

	/** One option as the API takes it: every default as text, min and max only when set, options only for a choice. */
	TSharedRef<FJsonObject> OptionToJsonObject(const FMantleScriptPackageParameter& Option)
	{
		const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Option.Name);
		Entry->SetStringField(TEXT("type"), FMantleScriptPackages::ParameterTypeName(Option.Type));
		Entry->SetStringField(TEXT("default"), Option.bHasDefault ? Option.Default : FString());
		Entry->SetStringField(TEXT("help"), Option.Help);
		Entry->SetBoolField(TEXT("required"), Option.bRequired);
		if (Option.Type == EMantleScriptPackageParameterType::Number && Option.bHasMin)
		{
			Entry->SetNumberField(TEXT("min"), Option.Min);
		}
		if (Option.Type == EMantleScriptPackageParameterType::Number && Option.bHasMax)
		{
			Entry->SetNumberField(TEXT("max"), Option.Max);
		}
		if (Option.Type == EMantleScriptPackageParameterType::Choice)
		{
			TArray<TSharedPtr<FJsonValue>> Choices;
			for (const FString& Choice : Option.Options)
			{
				Choices.Add(MakeShared<FJsonValueString>(Choice));
			}
			Entry->SetArrayField(TEXT("options"), Choices);
		}
		return Entry;
	}
}

FString FMantleScriptPackages::GetPackagesFolder(UWorld* World)
{
	return FMantlePlaceExporter::GetPackagesFolder(World);
}

const TCHAR* FMantleScriptPackages::ParameterTypeName(const EMantleScriptPackageParameterType Type)
{
	switch (Type)
	{
	case EMantleScriptPackageParameterType::Number: return TEXT("number");
	case EMantleScriptPackageParameterType::Boolean: return TEXT("boolean");
	case EMantleScriptPackageParameterType::Choice: return TEXT("choice");
	default: return TEXT("string");
	}
}

bool FMantleScriptPackages::ParseOptionNumber(const FString& Text, double& OutNumber)
{
	OutNumber = 0.0;
	const int32 Length = Text.Len();
	int32 Index = 0;
	if (Index < Length && (Text[Index] == TEXT('+') || Text[Index] == TEXT('-')))
	{
		++Index;
	}
	int32 Digits = 0;
	while (Index < Length && FChar::IsDigit(Text[Index]))
	{
		++Index;
		++Digits;
	}
	if (Index < Length && Text[Index] == TEXT('.'))
	{
		++Index;
		while (Index < Length && FChar::IsDigit(Text[Index]))
		{
			++Index;
			++Digits;
		}
	}
	if (Digits == 0)
	{
		return false;
	}
	if (Index < Length && (Text[Index] == TEXT('e') || Text[Index] == TEXT('E')))
	{
		++Index;
		if (Index < Length && (Text[Index] == TEXT('+') || Text[Index] == TEXT('-')))
		{
			++Index;
		}
		int32 ExponentDigits = 0;
		while (Index < Length && FChar::IsDigit(Text[Index]))
		{
			++Index;
			++ExponentDigits;
		}
		if (ExponentDigits == 0)
		{
			return false;
		}
	}
	double Parsed = 0.0;
	if (Index != Length || !LexTryParseString(Parsed, *Text) || !FMath::IsFinite(Parsed))
	{
		return false;
	}
	OutNumber = Parsed;
	return true;
}

FString FMantleScriptPackages::FormatOptionNumber(const double Number)
{
	return FString::SanitizeFloat(Number, 0);
}

FString FMantleScriptPackages::OptionValueProblem(const FString& Package, const FMantleScriptPackageParameter& Option, const FString& Value)
{
	if (FMantlePlaceParser::CodePointLength(Value) > FMantlePlaceLimits::MaxStorageSettingValueLength)
	{
		return FString::Printf(TEXT("%s's %s is longer than %d characters. Shorten it."), *Package, *Option.Name, FMantlePlaceLimits::MaxStorageSettingValueLength);
	}
	switch (Option.Type)
	{
	case EMantleScriptPackageParameterType::Number:
	{
		double Number = 0.0;
		if (!ParseOptionNumber(Value, Number))
		{
			return FString::Printf(TEXT("%s's %s must be a number, and \"%s\" is not one."), *Package, *Option.Name, *Value);
		}
		const bool bBelow = Option.bHasMin && Number < Option.Min;
		const bool bAbove = Option.bHasMax && Number > Option.Max;
		if ((bBelow || bAbove) && Option.bHasMin && Option.bHasMax)
		{
			return FString::Printf(TEXT("%s's %s must be between %s and %s."), *Package, *Option.Name, *FormatOptionNumber(Option.Min), *FormatOptionNumber(Option.Max));
		}
		if (bBelow)
		{
			return FString::Printf(TEXT("%s's %s must be at least %s."), *Package, *Option.Name, *FormatOptionNumber(Option.Min));
		}
		if (bAbove)
		{
			return FString::Printf(TEXT("%s's %s must be at most %s."), *Package, *Option.Name, *FormatOptionNumber(Option.Max));
		}
		return FString();
	}
	case EMantleScriptPackageParameterType::Boolean:
		if (Value.Equals(TEXT("true"), ESearchCase::CaseSensitive) || Value.Equals(TEXT("false"), ESearchCase::CaseSensitive))
		{
			return FString();
		}
		return FString::Printf(TEXT("%s's %s must be true or false, and \"%s\" is neither."), *Package, *Option.Name, *Value);
	case EMantleScriptPackageParameterType::Choice:
		if (Option.HasOption(Value))
		{
			return FString();
		}
		return FString::Printf(TEXT("%s's %s must be one of %s, and \"%s\" is not one of them."), *Package, *Option.Name,
			*FString::Join(Option.Options, TEXT(", ")), *Value);
	case EMantleScriptPackageParameterType::String:
		break;
	}
	return FString();
}

TArray<FMantleScriptOptionIssue> FMantleScriptPackages::CheckOptionValues(const FString& Package, const TArray<FMantleScriptPackageParameter>& Declared,
	const TMap<FString, FString>& Values, const TCHAR* Noun)
{
	TArray<FMantleScriptOptionIssue> Issues;
	auto Add = [&Issues](const FString& Key, const EMantleScriptOptionProblem Problem, const FString& Message)
	{
		FMantleScriptOptionIssue& Issue = Issues.AddDefaulted_GetRef();
		Issue.Key = Key;
		Issue.Problem = Problem;
		Issue.Message = Message;
	};
	auto FindDeclared = [&Declared](const FString& Key, const ESearchCase::Type Case) -> const FMantleScriptPackageParameter*
	{
		return Declared.FindByPredicate([&Key, Case](const FMantleScriptPackageParameter& Option) { return Option.Name.Equals(Key, Case); });
	};

	TArray<FString> Keys;
	Values.GetKeys(Keys);
	Keys.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::CaseSensitive) < 0; });
	for (const FString& Key : Keys)
	{
		const FString& Value = Values.FindChecked(Key);
		const FMantleScriptPackageParameter* Option = FindDeclared(Key, ESearchCase::CaseSensitive);
		if (!Option)
		{
			const FMantleScriptPackageParameter* Near = FindDeclared(Key, ESearchCase::IgnoreCase);
			Add(Key, EMantleScriptOptionProblem::Unknown,
				Declared.Num() == 0 ? FString::Printf(TEXT("%s has no %ss, so %s can't be set. Remove it."), *Package, Noun, *Key)
				: Near ? FString::Printf(TEXT("%s has no %s called %s. Did you mean %s? Names match letter case exactly."), *Package, Noun, *Key, *Near->Name)
				: FString::Printf(TEXT("%s has no %s called %s. Remove it."), *Package, Noun, *Key));
			continue;
		}
		if (Option->NeedsValue() && Value.IsEmpty())
		{
			Add(Key, EMantleScriptOptionProblem::Required, FString::Printf(TEXT("%s needs %s, and it is empty."), *Package, *Option->Name));
			continue;
		}
		const FString Problem = OptionValueProblem(Package, *Option, Value);
		if (!Problem.IsEmpty())
		{
			Add(Key, EMantleScriptOptionProblem::Invalid, Problem);
		}
	}
	for (const FMantleScriptPackageParameter& Option : Declared)
	{
		const bool bSet = Keys.ContainsByPredicate([&Option](const FString& Key) { return Key.Equals(Option.Name, ESearchCase::CaseSensitive); });
		if (!bSet && Option.NeedsValue())
		{
			Add(Option.Name, EMantleScriptOptionProblem::Required, FString::Printf(TEXT("%s needs %s."), *Package, *Option.Name));
		}
	}
	return Issues;
}

TArray<FString> FMantleScriptPackages::PruneSettings(TMap<FString, FString>& Settings, const FMantleScriptPackageManifest& Manifest)
{
	TArray<FString> Dropped;
	for (const TPair<FString, FString>& Pair : Settings)
	{
		if (!Manifest.FindSetting(Pair.Key))
		{
			Dropped.Add(Pair.Key);
		}
	}
	for (const FString& Key : Dropped)
	{
		Settings.Remove(Key);
	}
	Dropped.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::CaseSensitive) < 0; });
	return Dropped;
}

bool FMantleScriptPackages::KnownManifest(const FString& Name, const TArray<FMantleLocalScriptPackage>& Local, const FString& StoredManifestJson,
	FMantleScriptPackageManifest& OutManifest)
{
	OutManifest = FMantleScriptPackageManifest();
	const FMantleLocalScriptPackage* Package = Local.FindByPredicate([&Name](const FMantleLocalScriptPackage& Candidate)
	{
		return Candidate.Name.Equals(Name, ESearchCase::CaseSensitive);
	});
	if (Package)
	{
		// A folder whose package.json does not read has its own problems listed; its values cannot be judged.
		if (Package->bHasManifest)
		{
			OutManifest = Package->Manifest;
		}
		return Package->bHasManifest;
	}
	if (!StoredManifestJson.IsEmpty())
	{
		OutManifest = ReadManifestForHints(StoredManifestJson);
		return true;
	}
	return false;
}

bool FMantleScriptPackages::ReadManifest(const FString& Json, FMantleScriptPackageManifest& OutManifest, TArray<FString>& OutProblems)
{
	using namespace MantleScriptPackagesPrivate;
	OutManifest = FMantleScriptPackageManifest();
	OutProblems.Reset();
	const TSharedPtr<FJsonObject> Root = ParseObject(Json);
	if (!Root.IsValid())
	{
		OutProblems.Add(TEXT("package.json is not a JSON object. It looks like {\"name\": \"Kill Bricks\", \"description\": \"...\", \"parameters\": []}."));
		return false;
	}

	FString Name;
	if (!Root->TryGetStringField(TEXT("name"), Name) || Name.TrimStartAndEnd().IsEmpty())
	{
		OutProblems.Add(TEXT("package.json has no name. Add \"name\": the package's name as other creators see it in the library."));
	}
	else
	{
		OutManifest.Name = Name.TrimStartAndEnd();
		if (FMantlePlaceParser::CodePointLength(OutManifest.Name) > MaxManifestNameLength || HasControlCharacter(OutManifest.Name, false))
		{
			OutProblems.Add(FString::Printf(TEXT("package.json's name is longer than %d characters or holds a line break or control character. Shorten it to one line."),
				MaxManifestNameLength));
		}
	}

	if (Root->HasField(TEXT("description")))
	{
		FString Description;
		if (!Root->TryGetStringField(TEXT("description"), Description))
		{
			OutProblems.Add(TEXT("package.json's description is not text. Write it between quotes."));
		}
		else
		{
			OutManifest.Description = Description.TrimStartAndEnd();
			if (FMantlePlaceParser::CodePointLength(OutManifest.Description) > MaxManifestDescriptionLength || HasControlCharacter(OutManifest.Description, true))
			{
				OutProblems.Add(FString::Printf(TEXT("package.json's description is longer than %d characters or holds a control character. Shorten it."),
					MaxManifestDescriptionLength));
			}
		}
	}

	// The two option lists share every rule.
	ReadManifestOptions(Root, TEXT("parameters"), TEXT("parameter"), FMantlePlaceLimits::MaxScriptPointParameters, OutManifest.Parameters, OutProblems);
	ReadManifestOptions(Root, TEXT("settings"), TEXT("setting"), FMantlePlaceLimits::MaxStorageSettings, OutManifest.Settings, OutProblems);
	return OutProblems.Num() == 0;
}

FMantleScriptPackageManifest FMantleScriptPackages::ReadManifestForHints(const FString& Json)
{
	return FMantleStorage::ReadScriptPackageManifestJson(Json);
}

TSharedRef<FJsonObject> FMantleScriptPackages::ManifestToJsonObject(const FMantleScriptPackageManifest& Manifest)
{
	using namespace MantleScriptPackagesPrivate;
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("name"), Manifest.Name);
	Object->SetStringField(TEXT("description"), Manifest.Description);
	TArray<TSharedPtr<FJsonValue>> Parameters;
	for (const FMantleScriptPackageParameter& Parameter : Manifest.Parameters)
	{
		Parameters.Add(MakeShared<FJsonValueObject>(OptionToJsonObject(Parameter)));
	}
	Object->SetArrayField(TEXT("parameters"), Parameters);
	TArray<TSharedPtr<FJsonValue>> Settings;
	for (const FMantleScriptPackageParameter& Setting : Manifest.Settings)
	{
		Settings.Add(MakeShared<FJsonValueObject>(OptionToJsonObject(Setting)));
	}
	Object->SetArrayField(TEXT("settings"), Settings);
	return Object;
}

FString FMantleScriptPackages::ManifestToJson(const FMantleScriptPackageManifest& Manifest)
{
	FString Out;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(ManifestToJsonObject(Manifest), Writer);
	return Out;
}

FString FMantleScriptPackages::PathInPackage(const FString& CatalogPath)
{
	const FString Package = MantleScriptPackageOf(CatalogPath);
	if (Package.IsEmpty())
	{
		return CatalogPath;
	}
	return CatalogPath.RightChop(FCString::Strlen(MantleScriptPackagesPrivate::PackagesPrefix) + Package.Len() + 1);
}

void FMantleScriptPackages::Collect(const FString& PackagesFolder, TArray<FMantleLocalScriptPackage>& OutPackages, TArray<FMantleScriptPackageProblem>& OutProblems)
{
	using namespace MantleScriptPackagesPrivate;
	OutPackages.Reset();
	OutProblems.Reset();
	if (PackagesFolder.IsEmpty() || !IFileManager::Get().DirectoryExists(*PackagesFolder))
	{
		return;
	}
	const FString Root = FPaths::ConvertRelativePathToFull(PackagesFolder);

	auto AddProblem = [&OutProblems](const FString& Package, const FString& Problem, const FString& Remedy, const bool bError)
	{
		FMantleScriptPackageProblem& Row = OutProblems.AddDefaulted_GetRef();
		Row.Package = Package;
		Row.Problem = Problem;
		Row.Remedy = Remedy;
		Row.bError = bError;
	};

	// The runtime's own discovery (what Test locally runs), so Check and the runtime never disagree about what a package holds. Its
	// constructor looks beside a place file, so a sibling name in the level's folder points it at this Packages/ folder.
	const FMantleDiskScriptCatalogSource Source(FPaths::Combine(FPaths::GetPath(Root), TEXT("place.json")), /*bIncludePackages*/ true);

	// Loose files directly under Packages/ belong to no package and are never run.
	{
		TArray<FString> LooseFiles;
		IFileManager::Get().FindFiles(LooseFiles, *(Root / TEXT("*")), /*Files*/ true, /*Directories*/ false);
		LooseFiles.Sort();
		for (const FString& File : LooseFiles)
		{
			if (!File.StartsWith(TEXT(".")))
			{
				AddProblem(FString(), FString::Printf(TEXT("Packages/%s is not inside a package folder, so nothing runs it."), *File),
					TEXT("Move it into Packages/<PackageName>/Server, Client or Shared, or delete it."), true);
			}
		}
	}

	TArray<FString> Folders;
	IFileManager::Get().FindFiles(Folders, *(Root / TEXT("*")), /*Files*/ false, /*Directories*/ true);
	Folders.Sort();
	for (const FString& Folder : Folders)
	{
		if (Folder.StartsWith(TEXT(".")))
		{
			continue;
		}
		if (!FMantlePlaceLimits::IsStorageName(Folder))
		{
			AddProblem(Folder,
				FString::Printf(TEXT("Packages/%s is not a usable package name: a package's folder name is the name a place stores it under."), *Folder),
				TEXT("Rename the folder to a letter followed by up to 63 letters, digits or '_' (no spaces), for example KillBricks."), true);
			continue;
		}

		FMantleLocalScriptPackage& Package = OutPackages.AddDefaulted_GetRef();
		Package.Name = Folder;
		Package.Folder = Root / Folder;
		bool bValid = true;
		auto PackageProblem = [&AddProblem, &Folder, &bValid](const FString& Problem, const FString& Remedy, const bool bError)
		{
			AddProblem(Folder, Problem, Remedy, bError);
			bValid = bValid && !bError;
		};

		// The manifest.
		const FString ManifestFile = Package.Folder / ManifestFileName;
		FString ManifestText;
		if (!IFileManager::Get().FileExists(*ManifestFile))
		{
			PackageProblem(FString::Printf(TEXT("Packages/%s has no package.json."), *Folder),
				FString::Printf(TEXT("Add Packages/%s/package.json naming the package: {\"name\": \"%s\", \"description\": \"What it does\", \"parameters\": []}."), *Folder, *Folder),
				true);
		}
		else if (!FFileHelper::LoadFileToString(ManifestText, *ManifestFile))
		{
			PackageProblem(FString::Printf(TEXT("Packages/%s/package.json could not be read."), *Folder), TEXT("Close whatever has it open, then check again."), true);
		}
		else
		{
			TArray<FString> ManifestProblems;
			Package.bHasManifest = ReadManifest(ManifestText, Package.Manifest, ManifestProblems);
			for (const FString& Problem : ManifestProblems)
			{
				PackageProblem(FString::Printf(TEXT("Packages/%s: %s"), *Folder, *Problem), TEXT("Fix it in package.json."), true);
			}
		}

		// The scripts, as the runtime mounts them.
		const FString MountPrefix = FString(PackagesPrefix) + Folder + TEXT("/");
		for (const FMantleScriptCatalogEntry& Entry : Source.GetEntries())
		{
			if (MantleScriptPackageOf(Entry.Path).Equals(Folder, ESearchCase::CaseSensitive))
			{
				Package.Entries.Add(Entry);
			}
		}
		Package.Entries.Sort([](const FMantleScriptCatalogEntry& A, const FMantleScriptCatalogEntry& B)
		{
			return A.Path.Compare(B.Path, ESearchCase::CaseSensitive) < 0;
		});

		// Whatever the runtime would skip, named rather than dropped without a word.
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *Package.Folder, TEXT("*.*"), /*Files*/ true, /*Directories*/ false);
		Files.Sort();
		for (const FString& File : Files)
		{
			FString Relative = File;
			FPaths::MakePathRelativeTo(Relative, *(Package.Folder / TEXT("")));
			Relative.ReplaceInline(TEXT("\\"), TEXT("/"));
			const FString CleanName = FPaths::GetCleanFilename(Relative);
			if (CleanName.IsEmpty() || CleanName.StartsWith(TEXT(".")) || Relative.Equals(ManifestFileName, ESearchCase::CaseSensitive))
			{
				continue;
			}
			const FString Mounted = MountPrefix + Relative;
			if (Package.Entries.ContainsByPredicate([&Mounted](const FMantleScriptCatalogEntry& Entry) { return Entry.Path.Equals(Mounted, ESearchCase::CaseSensitive); }))
			{
				continue;
			}
			const FString Extension = FPaths::GetExtension(Relative);
			if (Extension.Equals(TEXT("uasset"), ESearchCase::IgnoreCase))
			{
				// A visual-script graph saves its compiled Luau beside it; the package runs and publishes that Luau.
				const FString CompiledMounted = MountPrefix + FPaths::ChangeExtension(Relative, TEXT("luau"));
				if (Package.Entries.ContainsByPredicate([&CompiledMounted](const FMantleScriptCatalogEntry& Entry) { return Entry.Path.Equals(CompiledMounted, ESearchCase::CaseSensitive); }))
				{
					PackageProblem(FString::Printf(TEXT("Packages/%s/%s is a visual-script graph: the package runs and publishes the Luau saved beside it (%s), not the graph."),
						*Folder, *Relative, *FPaths::ChangeExtension(CleanName, TEXT("luau"))),
						TEXT("Save the graph after every change so its Luau is current."), false);
				}
				else
				{
					PackageProblem(FString::Printf(TEXT("Packages/%s/%s is not a script Mantle runs (an Unreal asset, or a graph with no compiled Luau beside it)."), *Folder, *Relative),
						TEXT("A graph: open it and save it, which writes its Luau beside it. Anything else: move it out of Packages/."), true);
				}
				continue;
			}
			if (MantleScriptLanguageFromExtension(Extension) == EMantleScriptCatalogLanguage::Unknown)
			{
				PackageProblem(FString::Printf(TEXT("Packages/%s/%s has an extension Mantle cannot map to a script language."), *Folder, *Relative),
					TEXT("Use .luau, .js, .ts or .py, or move the file out of Packages/."), true);
				continue;
			}
			PackageProblem(FString::Printf(TEXT("Packages/%s/%s is not inside the package's Server/, Client/ or Shared/ folder (case-exact)."), *Folder, *Relative),
				TEXT("Move it into Server, Client or Shared, spelled exactly so."), true);
		}

		// The place's script path rules, and the backend's: unique paths ignoring case, 1 to 64 files.
		TMap<FString, FString> SeenLower;
		for (const FMantleScriptCatalogEntry& Entry : Package.Entries)
		{
			const FString Inner = PathInPackage(Entry.Path);
			const int32 PathLength = FMantlePlaceParser::CodePointLength(Inner);
			if (PathLength > FMantlePlaceLimits::MaxScriptPathLength)
			{
				PackageProblem(FString::Printf(TEXT("Packages/%s/%s has a path %d characters long; a package's script paths are at most %d."),
					*Folder, *Inner, PathLength, FMantlePlaceLimits::MaxScriptPathLength), TEXT("Shorten the file name or its folders."), true);
			}
			else if (!FMantlePlaceLimits::IsScriptPath(Inner))
			{
				PackageProblem(FString::Printf(TEXT("Packages/%s/%s has a character Mantle does not publish in a script path."), *Folder, *Inner),
					TEXT("Use only letters, digits, '_', '.' and '-' in file and folder names (no spaces or brackets)."), true);
			}
			const int32 NameLength = FMantlePlaceParser::CodePointLength(Entry.Name);
			if (NameLength == 0 || NameLength > FMantlePlaceLimits::MaxScriptNameLength)
			{
				PackageProblem(FString::Printf(TEXT("Packages/%s/%s has a name %d characters long; script names are 1 to %d."),
					*Folder, *Inner, NameLength, FMantlePlaceLimits::MaxScriptNameLength), TEXT("Rename the file."), true);
			}
			const FString Lower = Inner.ToLower();
			if (const FString* First = SeenLower.Find(Lower))
			{
				PackageProblem(FString::Printf(TEXT("Packages/%s/%s and %s differ only in letter case."), *Folder, *Inner, **First),
					TEXT("Rename one of them: a package's paths are unique ignoring case."), true);
			}
			else
			{
				SeenLower.Add(Lower, Inner);
			}
		}
		if (Package.Entries.Num() == 0)
		{
			PackageProblem(FString::Printf(TEXT("Packages/%s has no scripts."), *Folder),
				TEXT("Add a script under its Server, Client or Shared folder (a .luau, .js, .ts or .py file), or delete the folder."), true);
		}
		else if (Package.Entries.Num() > FMantlePlaceLimits::MaxScriptPackageFiles)
		{
			PackageProblem(FString::Printf(TEXT("Packages/%s has %d scripts; a package holds at most %d."), *Folder, Package.Entries.Num(), FMantlePlaceLimits::MaxScriptPackageFiles),
				TEXT("Combine some, or split it into two packages."), true);
		}
		Package.bValid = bValid;
	}
}

bool FMantleScriptPackages::PrepareFiles(const FMantleLocalScriptPackage& Package, TArray<FMantleScriptPackageFile>& OutFiles, FString& OutError)
{
	using namespace MantleScriptPackagesPrivate;
	OutFiles.Reset();
	OutError.Reset();
	if (!Package.bValid || !Package.bHasManifest)
	{
		OutError = FString::Printf(TEXT("The package '%s' has problems Check this place lists. Fix them and publish again."), *Package.Name);
		return false;
	}
	for (const FMantleScriptCatalogEntry& Entry : Package.Entries)
	{
		FMantleScriptPackageFile& File = OutFiles.AddDefaulted_GetRef();
		File.Path = PathInPackage(Entry.Path);
		File.Side = SideOfTopFolder(File.Path);
		File.Source = Entry.Source;
		switch (Entry.Language)
		{
		case EMantleScriptCatalogLanguage::Luau:
			File.Language = TEXT("luau");
			break;
		case EMantleScriptCatalogLanguage::JavaScript:
			File.Language = TEXT("javascript");
			break;
		case EMantleScriptCatalogLanguage::TypeScript:
		{
			File.Language = TEXT("typescript");
			FString JavaScript;
			FMantleScriptError CompileError;
			if (!FMantlePlaceExporter::CompileTypeScript(Entry.Source, JavaScript, CompileError))
			{
				OutError = FString::Printf(TEXT("Could not compile TypeScript 'Packages/%s/%s': %s"), *Package.Name, *File.Path, *CompileError.Message);
				OutFiles.Reset();
				return false;
			}
			File.Source = MoveTemp(JavaScript);
			break;
		}
		case EMantleScriptCatalogLanguage::Python:
			File.Language = TEXT("python");
			break;
		default:
			OutError = FString::Printf(TEXT("'Packages/%s/%s' has a language Mantle cannot publish yet."), *Package.Name, *File.Path);
			OutFiles.Reset();
			return false;
		}
		File.UploadLanguage = File.Language.Equals(TEXT("typescript"), ESearchCase::CaseSensitive) ? FString(TEXT("javascript")) : File.Language;
		if (File.Side.IsEmpty())
		{
			OutError = FString::Printf(TEXT("'Packages/%s/%s' is not inside Server/, Client/ or Shared/."), *Package.Name, *File.Path);
			OutFiles.Reset();
			return false;
		}
	}
	return true;
}

bool FMantleScriptPackages::ParsePackageRequest(const FString& Request, FString& OutPackage)
{
	OutPackage.Reset();
	if (!Request.StartsWith(TEXT("@")))
	{
		return false;
	}
	int32 Slash = INDEX_NONE;
	if (!Request.FindChar(TEXT('/'), Slash) || Slash <= 1 || Slash + 1 >= Request.Len())
	{
		return false;
	}
	const FString Name = Request.Mid(1, Slash - 1);
	if (!FMantlePlaceLimits::IsStorageName(Name))
	{
		return false;
	}
	OutPackage = Name;
	return true;
}

bool FMantleScriptPackages::ResolveStaticRequire(const TArray<FMantleScriptCatalogEntry>& Entries, const FMantleScriptCatalogEntry& From, const FString& Request,
	const TArray<FString>& StoredPackages, const FMantleScriptCatalogEntry*& OutTarget, FString& OutError)
{
	OutTarget = nullptr;
	OutError.Reset();
	FString Package;
	if (MantleScriptPackageOf(From.Path).IsEmpty() && ParsePackageRequest(Request, Package))
	{
		const bool bLocal = Entries.ContainsByPredicate([&Package](const FMantleScriptCatalogEntry& Entry)
		{
			return MantleScriptPackageOf(Entry.Path).Equals(Package, ESearchCase::CaseSensitive);
		});
		const bool bStored = StoredPackages.ContainsByPredicate([&Package](const FString& Name) { return Name.Equals(Package, ESearchCase::CaseSensitive); });
		if (!bLocal && bStored)
		{
			// A stored package's files arrive only when the place runs: nothing here can say whether it has that module.
			return true;
		}
	}
	OutTarget = MantleResolveRequire(Entries, From.Path, From.Side, Request, OutError);
	return OutTarget != nullptr;
}

int32 FMantleScriptPackages::RequiredScriptApiMinor(const int32 Need, const bool bUsesPackages)
{
	const int32 Raised = bUsesPackages ? FMath::Max(Need, MinScriptApiMinor) : Need;
	return FMath::Clamp(Raised, 0, FMantleApiManifest::Get().Minor);
}

FString FMantleScriptPackages::MemoryKey(const FMantleLocalScriptPackage& Package)
{
	FString Relative = Package.Folder;
	FPaths::MakePathRelativeTo(Relative, *FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
	Relative.ReplaceInline(TEXT("\\"), TEXT("/"));
	return TEXT("package:") + Relative;
}

int32 FMantleScriptPackages::AddPreviewStorage(TArray<FMantleStorageEntry>& Storage, const TArray<FMantleLocalScriptPackage>& Packages)
{
	int32 Added = 0;
	for (const FMantleLocalScriptPackage& Package : Packages)
	{
		const bool bNamed = Storage.ContainsByPredicate([&Package](const FMantleStorageEntry& Entry)
		{
			return Entry.Name.Equals(Package.Name, ESearchCase::CaseSensitive);
		});
		if (bNamed)
		{
			continue;
		}
		FMantleStorageEntry Entry;
		Entry.Name = Package.Name;
		for (int32 Serial = Added + 1; ; ++Serial)
		{
			const FString Candidate = FString::Printf(TEXT("itm_uc0de%028x"), Serial);
			if (!Storage.ContainsByPredicate([&Candidate](const FMantleStorageEntry& Other) { return Other.Item.Equals(Candidate, ESearchCase::CaseSensitive); }))
			{
				Entry.Item = Candidate;
				break;
			}
		}
		Entry.Version = 1;
		Storage.Add(MoveTemp(Entry));
		++Added;
	}
	return Added;
}
