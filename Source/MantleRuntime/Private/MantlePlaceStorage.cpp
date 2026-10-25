// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceStorage.h"

#include "MantlePlaceAssets.h"
#include "MantlePlaceLimits.h"
#include "MantlePlaceParser.h"
#include "MantleRuntime.h"

#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantlePlaceStoragePrivate
{
	/** An integer-valued JSON number of at least 1 that fits an int32. */
	bool ReadVersion(const TSharedPtr<FJsonObject>& Object, int32& Out)
	{
		double Value = 0.0;
		if (!Object->TryGetNumberField(TEXT("version"), Value) || Value != FMath::FloorToDouble(Value) || Value < 1.0
			|| Value > static_cast<double>(MAX_int32))
		{
			return false;
		}
		Out = static_cast<int32>(Value);
		return true;
	}

	/** The entry whose Name is exactly Name (FString's own comparison ignores case), or null. */
	const FMantleStorageEntry* FindEntryByName(const TArray<FMantleStorageEntry>& Entries, const FString& Name)
	{
		return Entries.FindByPredicate([&Name](const FMantleStorageEntry& Entry)
		{
			return Entry.Name.Equals(Name, ESearchCase::CaseSensitive);
		});
	}

	bool IsTakenDownStatus(const FString& Status)
	{
		return Status.Equals(TEXT("taken_down"), ESearchCase::CaseSensitive);
	}

	/** Why a resolved entry cannot back a spawn of KindName ("vehicle" or "weapon"), or empty when it can. */
	FString WhyNotKind(const FMantleResolvedStorageEntry& Resolved, const TCHAR* KindName)
	{
		if (IsTakenDownStatus(Resolved.Status))
		{
			return TEXT("was taken down by a Mantle moderator after a complaint, so it can no longer be used");
		}
		if (!Resolved.Kind.Equals(KindName, ESearchCase::CaseSensitive))
		{
			return FString::Printf(TEXT("is a %s, not a %s"), *Resolved.Kind, KindName);
		}
		if (!Resolved.Status.Equals(TEXT("active"), ESearchCase::CaseSensitive)
			&& !Resolved.Status.Equals(TEXT("delisted"), ESearchCase::CaseSensitive))
		{
			return FString::Printf(TEXT("has status '%s'"), *Resolved.Status);
		}
		if (!FMantlePlaceAssets::IsUploadedId(Resolved.PrimaryAssetId))
		{
			return FString::Printf(TEXT("resolved without a usable %s asset id"), KindName);
		}
		return FString();
	}

	FString WhyNotAVehicle(const FMantleResolvedStorageEntry& Resolved)
	{
		return WhyNotKind(Resolved, TEXT("vehicle"));
	}

	void TurnOff(FMantleVehicleSpawnProps& Spawn)
	{
		Spawn.Vehicle.Reset();
		Spawn.bEnabled = false;
	}

	void TurnOff(FMantleWeaponSpawnProps& Spawn)
	{
		Spawn.Weapon.Reset();
		Spawn.bEnabled = false;
	}

	/** A Script Point keeps its storage name and parameters: only Script.Points stops listing it. */
	void TurnOff(FMantleScriptPointProps& Point)
	{
		Point.bEnabled = false;
	}

	/** True for the two statuses a pinned version still loads under. */
	bool IsLoadableStatus(const FString& Status)
	{
		return Status.Equals(TEXT("active"), ESearchCase::CaseSensitive) || Status.Equals(TEXT("delisted"), ESearchCase::CaseSensitive);
	}

	bool IsScriptKind(const FString& Kind)
	{
		return Kind.Equals(FString(FMantleStorage::KindScript), ESearchCase::CaseSensitive);
	}

	/** Why a resolved entry cannot back a Script Point, or empty when it can. A package has no primary asset. */
	FString WhyNotAScript(const FMantleResolvedStorageEntry& Resolved)
	{
		if (IsTakenDownStatus(Resolved.Status))
		{
			return TEXT("was taken down by a Mantle moderator after a complaint, so it can no longer be used");
		}
		if (!IsScriptKind(Resolved.Kind))
		{
			return FString::Printf(TEXT("is a %s, not a script"), *Resolved.Kind);
		}
		if (!IsLoadableStatus(Resolved.Status))
		{
			return FString::Printf(TEXT("has status '%s'"), *Resolved.Status);
		}
		return FString();
	}

	/** The side a package file under TopFolder ("Server", "Client" or "Shared", case-exact) must declare. */
	EMantleScriptSide SideForTopFolder(const FString& TopFolder)
	{
		if (TopFolder.Equals(TEXT("Server"), ESearchCase::CaseSensitive))
		{
			return EMantleScriptSide::Server;
		}
		if (TopFolder.Equals(TEXT("Client"), ESearchCase::CaseSensitive))
		{
			return EMantleScriptSide::Client;
		}
		return EMantleScriptSide::Shared;
	}

	/**
	 * A script entry's scripts[], read leniently: a file that is not an object, has a path outside
	 * Server/, Client/ or Shared/, a side that disagrees with it, an unknown language, a source that is not a src_ id,
	 * or a path another file already used (ignoring case) is left out with a warning; at most MaxScriptPackageFiles.
	 */
	void ReadStoredScriptFiles(const TSharedPtr<FJsonObject>& Object, FMantleResolvedStorageEntry& Entry)
	{
		const TArray<TSharedPtr<FJsonValue>>* Files = nullptr;
		if (!Object->TryGetArrayField(TEXT("scripts"), Files))
		{
			return;
		}
		TSet<FString> SeenLowercasePaths;
		for (const TSharedPtr<FJsonValue>& Value : *Files)
		{
			if (Entry.Scripts.Num() >= FMantlePlaceLimits::MaxScriptPackageFiles)
			{
				UE_LOG(LogMantle, Warning, TEXT("MANTLE_STORAGE script item %s lists more than %d files; the rest are left out"),
					*Entry.Item, FMantlePlaceLimits::MaxScriptPackageFiles);
				break;
			}
			const TSharedPtr<FJsonObject>* FileObject = nullptr;
			FMantleStoredScriptFile File;
			FString Side;
			FString Language;
			FString TopFolder;
			if (!Value.IsValid() || !Value->TryGetObject(FileObject) || !FileObject || !FileObject->IsValid()
				|| !(*FileObject)->TryGetStringField(TEXT("path"), File.Path)
				|| !(*FileObject)->TryGetStringField(TEXT("side"), Side)
				|| !(*FileObject)->TryGetStringField(TEXT("language"), Language)
				|| !(*FileObject)->TryGetStringField(TEXT("source"), File.Source))
			{
				UE_LOG(LogMantle, Warning, TEXT("MANTLE_STORAGE script item %s has a file without path, side, language and source; it is left out"), *Entry.Item);
				continue;
			}
			if (File.Path.Len() > FMantlePlaceLimits::MaxScriptPathLength || !FMantlePlaceLimits::IsScriptPath(File.Path, &TopFolder)
				|| !MantleParseScriptSide(Side, File.Side) || File.Side != SideForTopFolder(TopFolder)
				|| !MantleParseScriptLanguage(Language, File.Language) || !FMantlePlaceLimits::IsScriptSourceId(File.Source))
			{
				UE_LOG(LogMantle, Warning, TEXT("MANTLE_STORAGE script item %s file '%s' (side '%s', language '%s') is not a valid script file; it is left out"),
					*Entry.Item, *File.Path, *Side, *Language);
				continue;
			}
			bool bAlreadySeen = false;
			SeenLowercasePaths.Add(File.Path.ToLower(), &bAlreadySeen);
			if (bAlreadySeen)
			{
				UE_LOG(LogMantle, Warning, TEXT("MANTLE_STORAGE script item %s lists '%s' twice; the repeat is left out"), *Entry.Item, *File.Path);
				continue;
			}
			Entry.Scripts.Add(MoveTemp(File));
		}
	}

	/**
	 * Place.StorageResolved from the answer: one entry per declared item that resolved to a loadable
	 * vehicle or weapon with an uploaded primary asset id (a place stores each item once, CheckEntries).
	 */
	void RetainResolved(FMantlePlace& Place, const FMantleStorageExpansion& Expansion)
	{
		Place.StorageResolved.Reset();
		Place.StorageTakenDown.Reset();
		Place.StorageSpawnProblems.Reset();
		for (const FMantleStorageEntry& Stored : Place.Storage)
		{
			if (Expansion.IsMissing(Stored.Item, Stored.Version))
			{
				continue;
			}
			const FMantleResolvedStorageEntry* Resolved = Expansion.Find(Stored.Item, Stored.Version);
			if (Resolved && IsTakenDownStatus(Resolved->Status))
			{
				// A takedown overrides the pin: the item is remembered as taken down, never as resolved.
				Place.StorageTakenDown.AddUnique(Stored.Item);
				UE_LOG(LogMantle, Warning, TEXT("MANTLE_STORAGE item %s (version %d, stored as '%s') in place %s was taken down by a moderator; it is not available to spawns or scripts"),
					*Stored.Item, Stored.Version, *Stored.Name, *Place.PlaceId);
				continue;
			}
			if (!Resolved || !IsLoadableStatus(Resolved->Status))
			{
				continue;
			}
			// A script package has no primary asset; its files are what it brings.
			const bool bScript = IsScriptKind(Resolved->Kind);
			if (!bScript && (!FMantlePlaceAssets::IsUploadedId(Resolved->PrimaryAssetId)
				|| (!Resolved->Kind.Equals(FString(FMantleStorage::KindVehicle), ESearchCase::CaseSensitive)
					&& !Resolved->Kind.Equals(FString(FMantleStorage::KindWeapon), ESearchCase::CaseSensitive))))
			{
				continue;
			}
			// Resolved points into Expansion, a different array from the one being added to.
			Place.StorageResolved.Add(*Resolved);
		}
	}

	/** The resolved entry for a declared storage entry's pinned version, or null. */
	const FMantleResolvedStorageEntry* FindResolvedFor(const FMantlePlace& Place, const FMantleStorageEntry& Stored)
	{
		return Place.StorageResolved.FindByPredicate([&Stored](const FMantleResolvedStorageEntry& Entry)
		{
			return Entry.Version == Stored.Version && Entry.Item.Equals(Stored.Item, ESearchCase::CaseSensitive);
		});
	}

	/** The storage entry that declares ItemId (a place stores each item once, CheckEntries), or null. */
	const FMantleStorageEntry* FindDeclaredItem(const FMantlePlace& Place, const FString& ItemId)
	{
		return Place.Storage.FindByPredicate([&ItemId](const FMantleStorageEntry& Entry)
		{
			return Entry.Item.Equals(ItemId, ESearchCase::CaseSensitive);
		});
	}

	/** Reads one {"item", "version"} object. */
	bool ReadRef(const TSharedPtr<FJsonValue>& Value, FMantleStorageRef& Out)
	{
		if (!Value.IsValid() || Value->Type != EJson::Object)
		{
			return false;
		}
		const TSharedPtr<FJsonObject> Object = Value->AsObject();
		return Object->TryGetStringField(TEXT("item"), Out.Item) && ReadVersion(Object, Out.Version);
	}

	/**
	 * A script entry's settings, read leniently: {key: text}. A key that is not a setting name, a repeat of a key
	 * ignoring case (the map cannot hold both), a value that is not text or is too long, is left out. bHasSettings says the answer
	 * carried the key at all.
	 */
	void ReadResolvedSettings(const TSharedPtr<FJsonObject>& Object, FMantleResolvedStorageEntry& Entry)
	{
		const TSharedPtr<FJsonObject>* SettingsObject = nullptr;
		if (!Object->TryGetObjectField(TEXT("settings"), SettingsObject) || !SettingsObject || !SettingsObject->IsValid())
		{
			return;
		}
		Entry.bHasSettings = true;
		for (const auto& Pair : (*SettingsObject)->Values)
		{
			const FString Key(Pair.Key.ToView());
			FString Value;
			if (Entry.Settings.Num() >= FMantlePlaceLimits::MaxStorageSettings || !FMantlePlaceLimits::IsScriptPointParameterName(Key)
				|| Entry.Settings.Contains(Key) || !Pair.Value.IsValid() || Pair.Value->Type != EJson::String || !Pair.Value->TryGetString(Value)
				|| FMantlePlaceParser::CodePointLength(Value) > FMantlePlaceLimits::MaxStorageSettingValueLength)
			{
				UE_LOG(LogMantle, Warning, TEXT("MANTLE_STORAGE script item %s has a setting '%s' that is not a setting name with a text value; it is left out"),
					*Entry.Item, *Key);
				continue;
			}
			Entry.Settings.Add(Key, MoveTemp(Value));
		}
	}

	bool ParseOptionType(const FString& Type, EMantleScriptPackageParameterType& Out)
	{
		if (Type.Equals(TEXT("string"), ESearchCase::CaseSensitive))
		{
			Out = EMantleScriptPackageParameterType::String;
			return true;
		}
		if (Type.Equals(TEXT("number"), ESearchCase::CaseSensitive))
		{
			Out = EMantleScriptPackageParameterType::Number;
			return true;
		}
		if (Type.Equals(TEXT("boolean"), ESearchCase::CaseSensitive))
		{
			Out = EMantleScriptPackageParameterType::Boolean;
			return true;
		}
		if (Type.Equals(TEXT("choice"), ESearchCase::CaseSensitive))
		{
			Out = EMantleScriptPackageParameterType::Choice;
			return true;
		}
		return false;
	}

	/** A finite JSON number field (min or max), or false when it is absent, not a number, or not finite. */
	bool ReadFiniteNumberField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, double& Out)
	{
		const TSharedPtr<FJsonValue> Value = Object->TryGetField(Field);
		if (!Value.IsValid() || Value->Type != EJson::Number || !FMath::IsFinite(Value->AsNumber()))
		{
			return false;
		}
		Out = Value->AsNumber();
		return true;
	}

	/** A number default's text: a JSON number from disk, or the backend's text; false when it does not read as a finite number. */
	bool ReadNumberDefault(const TSharedPtr<FJsonValue>& Default, double& Out)
	{
		if (Default->Type == EJson::Number && FMath::IsFinite(Default->AsNumber()))
		{
			Out = Default->AsNumber();
			return true;
		}
		if (Default->Type == EJson::String)
		{
			double Parsed = 0.0;
			const FString Text = Default->AsString().TrimStartAndEnd();
			if (!Text.IsEmpty() && Text.IsNumeric() && LexTryParseString(Parsed, *Text) && FMath::IsFinite(Parsed))
			{
				Out = Parsed;
				return true;
			}
		}
		return false;
	}

	/**
	 * One option list of a manifest: "parameters" or "settings", the same shape. See ReadScriptPackageManifest for
	 * the lenient rules.
	 */
	void ReadOptionList(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, const int32 Limit, TArray<FMantleScriptPackageParameter>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Options = nullptr;
		if (!Object->TryGetArrayField(Field, Options) || !Options)
		{
			return;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Options)
		{
			if (Out.Num() >= Limit)
			{
				break;
			}
			const TSharedPtr<FJsonObject>* OptionObject = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(OptionObject) || !OptionObject || !OptionObject->IsValid())
			{
				continue;
			}
			const TSharedPtr<FJsonObject>& Option = *OptionObject;
			FMantleScriptPackageParameter Parameter;
			FString Type;
			if (!Option->TryGetStringField(TEXT("name"), Parameter.Name)
				|| !FMantlePlaceLimits::IsScriptPointParameterName(Parameter.Name)
				|| !Option->TryGetStringField(TEXT("type"), Type)
				|| !ParseOptionType(Type, Parameter.Type))
			{
				continue;
			}
			// Names are unique ignoring letter case (the backend's rule): a second spelling is left out.
			const FString& Name = Parameter.Name;
			if (Out.ContainsByPredicate([&Name](const FMantleScriptPackageParameter& Earlier) { return Earlier.Name.Equals(Name, ESearchCase::IgnoreCase); }))
			{
				continue;
			}
			Option->TryGetBoolField(TEXT("required"), Parameter.bRequired);
			if (Parameter.Type == EMantleScriptPackageParameterType::Number)
			{
				Parameter.bHasMin = ReadFiniteNumberField(Option, TEXT("min"), Parameter.Min);
				Parameter.bHasMax = ReadFiniteNumberField(Option, TEXT("max"), Parameter.Max);
				if (Parameter.bHasMin && Parameter.bHasMax && Parameter.Min > Parameter.Max)
				{
					// A range that holds nothing bounds nothing.
					Parameter.bHasMin = Parameter.bHasMax = false;
				}
			}
			if (Parameter.Type == EMantleScriptPackageParameterType::Choice)
			{
				const TArray<TSharedPtr<FJsonValue>>* Choices = nullptr;
				if (Option->TryGetArrayField(TEXT("options"), Choices) && Choices)
				{
					for (const TSharedPtr<FJsonValue>& Choice : *Choices)
					{
						FString Text;
						if (Parameter.Options.Num() >= FMantlePlaceLimits::MaxScriptChoiceOptions || !Choice.IsValid() || Choice->Type != EJson::String
							|| !Choice->TryGetString(Text) || Text.IsEmpty() || FMantlePlaceParser::CodePointLength(Text) > FMantlePlaceLimits::MaxScriptChoiceOptionLength
							|| Parameter.HasOption(Text))
						{
							continue;
						}
						Parameter.Options.Add(MoveTemp(Text));
					}
				}
				if (Parameter.Options.Num() < FMantlePlaceLimits::MinScriptChoiceOptions)
				{
					continue; // a choice with nothing to choose between
				}
			}
			const TSharedPtr<FJsonValue> Default = Option->TryGetField(TEXT("default"));
			if (Default.IsValid())
			{
				switch (Parameter.Type)
				{
				case EMantleScriptPackageParameterType::String:
					if (Default->Type == EJson::String && FMantlePlaceParser::CodePointLength(Default->AsString()) <= FMantlePlaceLimits::MaxScriptPointParameterValueLength)
					{
						// The backend writes "" for no default: a required string with "" has none, so the place must set it.
						Parameter.bHasDefault = !(Parameter.bRequired && Default->AsString().IsEmpty());
						Parameter.Default = Parameter.bHasDefault ? Default->AsString() : FString();
					}
					break;
				case EMantleScriptPackageParameterType::Number:
				{
					// The backend stores every default as text (gamelib.ScriptParameter.Default), while a package.json on
					// disk writes a JSON number: both are accepted when they read as a finite number within min and max.
					double Number = 0.0;
					if (ReadNumberDefault(Default, Number) && (!Parameter.bHasMin || Number >= Parameter.Min) && (!Parameter.bHasMax || Number <= Parameter.Max))
					{
						Parameter.bHasDefault = true;
						Parameter.Default = FString::SanitizeFloat(Number, 0);
					}
					break;
				}
				case EMantleScriptPackageParameterType::Boolean:
					// As above: a JSON boolean from disk, or the backend's "true" / "false" text.
					if (Default->Type == EJson::Boolean)
					{
						Parameter.bHasDefault = true;
						Parameter.Default = Default->AsBool() ? TEXT("true") : TEXT("false");
					}
					else if (Default->Type == EJson::String)
					{
						const FString Text = Default->AsString().TrimStartAndEnd();
						if (Text.Equals(TEXT("true"), ESearchCase::CaseSensitive) || Text.Equals(TEXT("false"), ESearchCase::CaseSensitive))
						{
							Parameter.bHasDefault = true;
							Parameter.Default = Text;
						}
					}
					break;
				case EMantleScriptPackageParameterType::Choice:
					// One of the options exactly; the backend's "" (no default) is none of them.
					if (Default->Type == EJson::String && Parameter.HasOption(Default->AsString()))
					{
						Parameter.bHasDefault = true;
						Parameter.Default = Default->AsString();
					}
					break;
				}
			}
			Option->TryGetStringField(TEXT("help"), Parameter.Help);
			Out.Add(MoveTemp(Parameter));
		}
	}
}

const FMantleResolvedStorageEntry* FMantleStorageExpansion::Find(const FString& Item, const int32 Version) const
{
	return Entries.FindByPredicate([&Item, Version](const FMantleResolvedStorageEntry& Entry)
	{
		return Entry.Version == Version && Entry.Item.Equals(Item, ESearchCase::CaseSensitive);
	});
}

bool FMantleStorageExpansion::IsMissing(const FString& Item, const int32 Version) const
{
	return Missing.ContainsByPredicate([&Item, Version](const FMantleStorageRef& Ref)
	{
		return Ref.Version == Version && Ref.Item.Equals(Item, ESearchCase::CaseSensitive);
	});
}

TArray<FMantleStorageProblem> FMantleStorage::CheckEntries(const TArray<FMantleStorageEntry>& Entries)
{
	TArray<FMantleStorageProblem> Problems;
	if (Entries.Num() > FMantlePlaceLimits::MaxStorageEntries)
	{
		Problems.Add(FMantleStorageProblem{
			TEXT("/storage"),
			FString::Printf(TEXT("has %d entries; at most %d are allowed"), Entries.Num(), FMantlePlaceLimits::MaxStorageEntries) });
	}
	TMap<FString, TArray<int32>> IndicesByFoldedName;
	// Item id -> the index of the first entry that stores it. Only well-formed ids are keyed: they are all
	// lowercase, so the map's case-folding keys cannot merge two different ids.
	TMap<FString, int32> FirstIndexOfItem;
	for (int32 i = 0; i < Entries.Num(); ++i)
	{
		const FMantleStorageEntry& Entry = Entries[i];
		const FString Path = FString::Printf(TEXT("/storage/%d"), i);
		if (!FMantlePlaceLimits::IsStorageName(Entry.Name))
		{
			Problems.Add(FMantleStorageProblem{
				Path + TEXT("/name"),
				FString::Printf(TEXT("'%s' must be a letter followed by up to 63 letters, digits or '_'"), *Entry.Name) });
		}
		else
		{
			// TMap<FString> keys ignore case, but names are case-sensitive ('Truck' and 'truck' are two names), so the
			// key folds case and each holds the indices of entries to compare by exact spelling.
			TArray<int32>& SameFoldedName = IndicesByFoldedName.FindOrAdd(Entry.Name);
			bool bRepeats = false;
			for (const int32 Other : SameFoldedName)
			{
				if (Entries[Other].Name.Equals(Entry.Name, ESearchCase::CaseSensitive))
				{
					Problems.Add(FMantleStorageProblem{
						Path + TEXT("/name"),
						FString::Printf(TEXT("'%s' repeats storage/%d's name; storage names must be unique"), *Entry.Name, Other) });
					bRepeats = true;
					break;
				}
			}
			if (!bRepeats)
			{
				SameFoldedName.Add(i);
			}
		}
		if (!FMantlePlaceLimits::IsLibraryItemId(Entry.Item))
		{
			Problems.Add(FMantleStorageProblem{
				Path + TEXT("/item"),
				FString::Printf(TEXT("'%s' must be a library item's id (itm_u and 32 lowercase hex digits)"), *Entry.Item) });
		}
		else if (const int32* First = FirstIndexOfItem.Find(Entry.Item))
		{
			// A place pins one version of an item, so an item id names one stored version (Storage:Get, FindAuthorized).
			Problems.Add(FMantleStorageProblem{
				Path + TEXT("/item"),
				FString::Printf(TEXT("item %s is already stored as \"%s\"; a place stores one version of an item"),
					*Entry.Item, *Entries[*First].Name) });
		}
		else
		{
			FirstIndexOfItem.Add(Entry.Item, i);
		}
		if (Entry.Version < 1)
		{
			Problems.Add(FMantleStorageProblem{
				Path + TEXT("/version"), FString::Printf(TEXT("%d must be at least 1"), Entry.Version) });
		}
		// A script package's settings, by shape only (whether the item is a script, and what its manifest declares,
		// are the editor's and the backend's to say).
		if (Entry.Settings.Num() > FMantlePlaceLimits::MaxStorageSettings)
		{
			Problems.Add(FMantleStorageProblem{
				Path + TEXT("/settings"),
				FString::Printf(TEXT("has %d settings; at most %d are allowed"), Entry.Settings.Num(), FMantlePlaceLimits::MaxStorageSettings) });
		}
		TArray<FString> Keys;
		Entry.Settings.GetKeys(Keys);
		Keys.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::CaseSensitive) < 0; });
		for (const FString& Key : Keys)
		{
			const FString KeyPath = Path + TEXT("/settings/") + Key;
			if (!FMantlePlaceLimits::IsScriptPointParameterName(Key))
			{
				Problems.Add(FMantleStorageProblem{
					KeyPath, FString::Printf(TEXT("'%s' must be a letter followed by up to 31 letters, digits or '_'"), *Key) });
				continue;
			}
			const FString* Value = Entry.Settings.Find(Key);
			if (Value && FMantlePlaceParser::CodePointLength(*Value) > FMantlePlaceLimits::MaxStorageSettingValueLength)
			{
				Problems.Add(FMantleStorageProblem{
					KeyPath, FString::Printf(TEXT("is longer than %d characters"), FMantlePlaceLimits::MaxStorageSettingValueLength) });
			}
		}
	}
	return Problems;
}

TArray<FMantleStorageRef> FMantleStorage::RefsOf(const TArray<FMantleStorageEntry>& Entries)
{
	TArray<FMantleStorageRef> Refs;
	for (const FMantleStorageEntry& Entry : Entries)
	{
		const bool bSeen = Refs.ContainsByPredicate([&Entry](const FMantleStorageRef& Ref)
		{
			return Ref.Version == Entry.Version && Ref.Item.Equals(Entry.Item, ESearchCase::CaseSensitive);
		});
		if (!bSeen)
		{
			FMantleStorageRef Ref;
			Ref.Item = Entry.Item;
			Ref.Version = Entry.Version;
			Refs.Add(MoveTemp(Ref));
		}
	}
	return Refs;
}

bool FMantleStorage::ParseResponse(const FString& Json, FMantleStorageExpansion& Out)
{
	using namespace MantlePlaceStoragePrivate;
	Out = FMantleStorageExpansion();

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
	if (!Root->TryGetArrayField(TEXT("entries"), Entries))
	{
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Entries)
	{
		if (!Value.IsValid() || Value->Type != EJson::Object)
		{
			Out = FMantleStorageExpansion();
			return false;
		}
		const TSharedPtr<FJsonObject> Object = Value->AsObject();
		FMantleResolvedStorageEntry Entry;
		if (!Object->TryGetStringField(TEXT("item"), Entry.Item) || !ReadVersion(Object, Entry.Version)
			|| !Object->TryGetStringField(TEXT("status"), Entry.Status))
		{
			Out = FMantleStorageExpansion();
			return false;
		}
		// A taken-down item may come without the assets it no longer serves; every other entry names its kind and
		// primary asset, except a script package, which has none (the backend sends "", which reads as empty either way).
		const bool bKindRead = Object->TryGetStringField(TEXT("kind"), Entry.Kind);
		const bool bPrimaryRead = Object->TryGetStringField(TEXT("primaryAssetId"), Entry.PrimaryAssetId);
		if (!IsTakenDownStatus(Entry.Status) && (!bKindRead || (!bPrimaryRead && !IsScriptKind(Entry.Kind))))
		{
			Out = FMantleStorageExpansion();
			return false;
		}
		if (IsScriptKind(Entry.Kind))
		{
			// Read leniently: a malformed file or manifest is left out with a warning, never a failed load.
			ReadStoredScriptFiles(Object, Entry);
			const TSharedPtr<FJsonObject>* Manifest = nullptr;
			if (Object->TryGetObjectField(TEXT("manifest"), Manifest) && Manifest)
			{
				Entry.Manifest = FMantleStorage::ReadScriptPackageManifest(*Manifest);
			}
			ReadResolvedSettings(Object, Entry);
		}
		Object->TryGetStringField(TEXT("name"), Entry.Name);
		const TArray<TSharedPtr<FJsonValue>>* Dependencies = nullptr;
		if (Object->TryGetArrayField(TEXT("dependencyAssetIds"), Dependencies))
		{
			for (const TSharedPtr<FJsonValue>& Dependency : *Dependencies)
			{
				FString Id;
				if (!Dependency.IsValid() || Dependency->Type != EJson::String || !Dependency->TryGetString(Id))
				{
					Out = FMantleStorageExpansion();
					return false;
				}
				Entry.DependencyAssetIds.Add(MoveTemp(Id));
			}
		}
		Out.Entries.Add(MoveTemp(Entry));
	}
	// missing is optional: an answer that found everything may leave it out.
	const TArray<TSharedPtr<FJsonValue>>* Missing = nullptr;
	if (Root->TryGetArrayField(TEXT("missing"), Missing))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Missing)
		{
			FMantleStorageRef Ref;
			if (!ReadRef(Value, Ref))
			{
				Out = FMantleStorageExpansion();
				return false;
			}
			Out.Missing.Add(MoveTemp(Ref));
		}
	}
	return true;
}

int32 FMantleStorage::Apply(FMantlePlace& Place, const FMantleStorageExpansion& Expansion)
{
	using namespace MantlePlaceStoragePrivate;
	RetainResolved(Place, Expansion);
	int32 TurnedOff = 0;
	for (FMantleEntity& Entity : Place.Entities)
	{
		if (FMantleScriptPointProps* Point = Entity.Props.TryGet<FMantleScriptPointProps>())
		{
			// A point keeps everything it carries; only a package that will not run turns it off.
			if (Point->StorageItem.IsEmpty() || !Point->bEnabled)
			{
				continue;
			}
			FString PointWhy;
			const FMantleStorageEntry* PointStored = FindEntryByName(Place.Storage, Point->StorageItem);
			if (!PointStored)
			{
				PointWhy = FString::Printf(TEXT("names storage '%s', which the place does not declare"), *Point->StorageItem);
			}
			else if (Expansion.IsMissing(PointStored->Item, PointStored->Version))
			{
				PointWhy = FString::Printf(TEXT("names storage '%s' (%s version %d), which is missing or not usable by this place"),
					*Point->StorageItem, *PointStored->Item, PointStored->Version);
			}
			else if (const FMantleResolvedStorageEntry* PointResolved = Expansion.Find(PointStored->Item, PointStored->Version))
			{
				const FString NotAScript = WhyNotAScript(*PointResolved);
				if (NotAScript.IsEmpty())
				{
					continue;
				}
				PointWhy = FString::Printf(TEXT("names storage '%s' (%s version %d), which %s"),
					*Point->StorageItem, *PointStored->Item, PointStored->Version, *NotAScript);
			}
			else
			{
				PointWhy = FString::Printf(TEXT("names storage '%s' (%s version %d), which the API did not answer for"),
					*Point->StorageItem, *PointStored->Item, PointStored->Version);
			}
			TurnOff(*Point);
			++TurnedOff;
			UE_LOG(LogMantle, Warning, TEXT("MANTLE_STORAGE script point '%s' in place %s %s; the point is turned off"),
				*Entity.Name, *Place.PlaceId, *PointWhy);
			continue;
		}
		if (FMantleWeaponSpawnProps* WeaponSpawn = Entity.Props.TryGet<FMantleWeaponSpawnProps>())
		{
			if (WeaponSpawn->StorageItem.IsEmpty())
			{
				continue;
			}
			FString WeaponWhy;
			const FMantleStorageEntry* WeaponStored = FindEntryByName(Place.Storage, WeaponSpawn->StorageItem);
			if (!WeaponStored)
			{
				WeaponWhy = FString::Printf(TEXT("names storage '%s', which the place does not declare"), *WeaponSpawn->StorageItem);
			}
			else if (Expansion.IsMissing(WeaponStored->Item, WeaponStored->Version))
			{
				WeaponWhy = FString::Printf(TEXT("names storage '%s' (%s version %d), which is missing or not usable by this place"),
					*WeaponSpawn->StorageItem, *WeaponStored->Item, WeaponStored->Version);
			}
			else if (const FMantleResolvedStorageEntry* WeaponResolved = Expansion.Find(WeaponStored->Item, WeaponStored->Version))
			{
				const FString NotAWeapon = WhyNotKind(*WeaponResolved, TEXT("weapon"));
				if (NotAWeapon.IsEmpty())
				{
					WeaponSpawn->Weapon = WeaponResolved->PrimaryAssetId;
					continue;
				}
				WeaponWhy = FString::Printf(TEXT("names storage '%s' (%s version %d), which %s"),
					*WeaponSpawn->StorageItem, *WeaponStored->Item, WeaponStored->Version, *NotAWeapon);
			}
			else
			{
				WeaponWhy = FString::Printf(TEXT("names storage '%s' (%s version %d), which the API did not answer for"),
					*WeaponSpawn->StorageItem, *WeaponStored->Item, WeaponStored->Version);
			}
			TurnOff(*WeaponSpawn);
			++TurnedOff;
			UE_LOG(LogMantle, Warning, TEXT("MANTLE_STORAGE weapon spawn '%s' in place %s %s; the spawn is turned off"),
				*Entity.Name, *Place.PlaceId, *WeaponWhy);
			continue;
		}
		FMantleVehicleSpawnProps* Spawn = Entity.Props.TryGet<FMantleVehicleSpawnProps>();
		if (!Spawn || Spawn->StorageItem.IsEmpty())
		{
			continue;
		}
		FString Why;
		const FMantleStorageEntry* Stored = FindEntryByName(Place.Storage, Spawn->StorageItem);
		if (!Stored)
		{
			Why = FString::Printf(TEXT("names storage '%s', which the place does not declare"), *Spawn->StorageItem);
		}
		else if (Expansion.IsMissing(Stored->Item, Stored->Version))
		{
			Why = FString::Printf(TEXT("names storage '%s' (%s version %d), which is missing or not usable by this place"),
				*Spawn->StorageItem, *Stored->Item, Stored->Version);
		}
		else if (const FMantleResolvedStorageEntry* Resolved = Expansion.Find(Stored->Item, Stored->Version))
		{
			const FString NotAVehicle = WhyNotAVehicle(*Resolved);
			if (NotAVehicle.IsEmpty())
			{
				// The one place a stored vehicle becomes a plain uploaded one.
				Spawn->Vehicle = Resolved->PrimaryAssetId;
				continue;
			}
			Why = FString::Printf(TEXT("names storage '%s' (%s version %d), which %s"),
				*Spawn->StorageItem, *Stored->Item, Stored->Version, *NotAVehicle);
		}
		else
		{
			Why = FString::Printf(TEXT("names storage '%s' (%s version %d), which the API did not answer for"),
				*Spawn->StorageItem, *Stored->Item, Stored->Version);
		}
		TurnOff(*Spawn);
		++TurnedOff;
		Place.StorageSpawnProblems.Add(FString::Printf(
			TEXT("The vehicle spawn '%s' is turned off, so no vehicle is parked there: it %s. Check that the vehicle is still in your library and in the place's Storage, then publish the place again."),
			*Entity.Name, *Why));
		UE_LOG(LogMantle, Warning, TEXT("MANTLE_STORAGE vehicle spawn '%s' in place %s %s; the spawn is turned off"),
			*Entity.Name, *Place.PlaceId, *Why);
	}
	return TurnedOff;
}

const FMantleStorageEntry* FMantleStorage::FindByName(const FMantlePlace& Place, const FString& Name)
{
	return MantlePlaceStoragePrivate::FindEntryByName(Place.Storage, Name);
}

const FMantleResolvedStorageEntry* FMantleStorage::FindAuthorized(const FMantlePlace& Place, const FString& ItemId, const FString& Kind)
{
	using namespace MantlePlaceStoragePrivate;
	if (!FMantlePlaceLimits::IsLibraryItemId(ItemId))
	{
		return nullptr;
	}
	const FMantleStorageEntry* Declared = FindDeclaredItem(Place, ItemId);
	if (!Declared)
	{
		return nullptr;
	}
	const FMantleResolvedStorageEntry* Resolved = FindResolvedFor(Place, *Declared);
	return Resolved && Resolved->Kind.Equals(Kind, ESearchCase::CaseSensitive) ? Resolved : nullptr;
}

FString FMantleStorage::WhyNotAuthorized(const FMantlePlace& Place, const FString& ItemId, const FString& Kind)
{
	using namespace MantlePlaceStoragePrivate;
	if (!FMantlePlaceLimits::IsLibraryItemId(ItemId))
	{
		return FString::Printf(TEXT("\"%s\" is not a library item id: pass the id Storage:Get returns for one of this place's storage names "
			"(itm_u followed by 32 lowercase hex digits). Asset ids, object paths and built-in names are not accepted"), *ItemId);
	}
	const FMantleStorageEntry* Declared = FindDeclaredItem(Place, ItemId);
	if (!Declared)
	{
		return FString::Printf(TEXT("item %s is not in this place's storage"), *ItemId);
	}
	const FMantleResolvedStorageEntry* Resolved = FindResolvedFor(Place, *Declared);
	if (!Resolved && Place.StorageTakenDown.ContainsByPredicate([&ItemId](const FString& Taken) { return Taken.Equals(ItemId, ESearchCase::CaseSensitive); }))
	{
		return FString::Printf(TEXT("item %s was taken down by a Mantle moderator after a complaint, so this place can no longer use it. "
			"The place's owner has to remove it from the place's storage"), *ItemId);
	}
	if (!Resolved)
	{
		return FString::Printf(TEXT("item %s (version %d) is in this place's storage, but it did not load: it is missing, "
			"not usable by this place, or the place has not finished loading"), *ItemId, Declared->Version);
	}
	if (!Resolved->Kind.Equals(Kind, ESearchCase::CaseSensitive))
	{
		return FString::Printf(TEXT("item %s is a %s, not a %s"), *ItemId, *Resolved->Kind, *Kind);
	}
	return FString();
}

TArray<FString> FMantleStorage::StoredVehicleAssetIds(const FMantlePlace& Place)
{
	TArray<FString> Ids;
	for (const FMantleResolvedStorageEntry& Entry : Place.StorageResolved)
	{
		if (Entry.Kind.Equals(FString(KindVehicle), ESearchCase::CaseSensitive) && FMantlePlaceAssets::IsUploadedId(Entry.PrimaryAssetId))
		{
			Ids.AddUnique(Entry.PrimaryAssetId);
		}
	}
	return Ids;
}

TArray<FString> FMantleStorage::StoredWeaponAssetIds(const FMantlePlace& Place)
{
	TArray<FString> Ids;
	for (const FMantleResolvedStorageEntry& Entry : Place.StorageResolved)
	{
		if (Entry.Kind.Equals(FString(KindWeapon), ESearchCase::CaseSensitive) && FMantlePlaceAssets::IsUploadedId(Entry.PrimaryAssetId))
		{
			Ids.AddUnique(Entry.PrimaryAssetId);
		}
	}
	return Ids;
}

FMantleScriptPackageManifest FMantleStorage::ReadScriptPackageManifest(const TSharedPtr<FJsonObject>& Object)
{
	FMantleScriptPackageManifest Manifest;
	if (!Object.IsValid())
	{
		return Manifest;
	}
	Object->TryGetStringField(TEXT("name"), Manifest.Name);
	Object->TryGetStringField(TEXT("description"), Manifest.Description);
	MantlePlaceStoragePrivate::ReadOptionList(Object, TEXT("parameters"), FMantlePlaceLimits::MaxScriptPointParameters, Manifest.Parameters);
	MantlePlaceStoragePrivate::ReadOptionList(Object, TEXT("settings"), FMantlePlaceLimits::MaxStorageSettings, Manifest.Settings);
	return Manifest;
}

FMantleScriptPackageManifest FMantleStorage::ReadScriptPackageManifestJson(const FString& Json)
{
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return FMantleScriptPackageManifest();
	}
	return ReadScriptPackageManifest(Root);
}

TArray<FMantleStoredScriptPackage> FMantleStorage::StoredScriptPackages(const FMantlePlace& Place, const bool bServerSide, const bool bClientSide)
{
	using namespace MantlePlaceStoragePrivate;
	TArray<FMantleStoredScriptPackage> Packages;
	for (const FMantleStorageEntry& Stored : Place.Storage)
	{
		const FMantleResolvedStorageEntry* Resolved = FindResolvedFor(Place, Stored);
		if (!Resolved || !IsScriptKind(Resolved->Kind))
		{
			continue;
		}
		FMantleStoredScriptPackage& Package = Packages.AddDefaulted_GetRef();
		Package.StorageName = Stored.Name;
		Package.Manifest = Resolved->Manifest;
		// The sealed values the resolve answered, or the place file's own when an older backend sent none.
		Package.Settings = Resolved->bHasSettings ? Resolved->Settings : Stored.Settings;
		for (const FMantleStoredScriptFile& File : Resolved->Scripts)
		{
			const bool bWanted = File.Side == EMantleScriptSide::Shared
				|| (File.Side == EMantleScriptSide::Server && bServerSide)
				|| (File.Side == EMantleScriptSide::Client && bClientSide);
			if (bWanted)
			{
				Package.Files.Add(File);
			}
		}
	}
	return Packages;
}

float FMantleStorage::RetryDelaySeconds(const int32 Retry)
{
	return Retry <= 1 ? 1.0f : 2.0f;
}

EMantleStorageAnswer FMantleStorage::Classify(const int32 StatusCode, const FString& Body, const FString& TransportMessage,
	FMantleStorageExpansion& Out, FString& OutError)
{
	Out = FMantleStorageExpansion();
	OutError.Reset();
	const FString Detail = TransportMessage.IsEmpty() ? FString() : TEXT(": ") + TransportMessage;
	if (StatusCode >= 200 && StatusCode < 300)
	{
		if (ParseResponse(Body, Out))
		{
			return EMantleStorageAnswer::Apply;
		}
		OutError = TEXT("the API's answer could not be read");
		return EMantleStorageAnswer::Fail;
	}
	if (StatusCode == 0)
	{
		OutError = TEXT("the request did not reach the API") + Detail;
		return EMantleStorageAnswer::Retry;
	}
	if (StatusCode >= 500 && StatusCode < 600)
	{
		OutError = FString::Printf(TEXT("the API answered %d"), StatusCode) + Detail;
		return EMantleStorageAnswer::Retry;
	}
	OutError = FString::Printf(TEXT("the API answered %d"), StatusCode) + Detail;
	return EMantleStorageAnswer::Fail;
}

namespace MantlePlaceStoragePrivate
{
	/** One ExpandAsync in flight: everything a retry needs, kept alive by the callbacks and tickers that hold it. */
	struct FExpandState
	{
		FString PlaceId;
		bool bServer = false;
		TArray<FMantleStorageRef> Refs;
		FMantlePlace Place;
		TFunction<void(FMantleStorageExpandResult)> OnDone;
		int32 Retries = 0;
	};

	void SendResolve(const TSharedRef<FExpandState>& State)
	{
		const FMantleApiCallback Callback = FMantleApiCallback::CreateLambda(
			[State](const FMantleApiResult& Response)
			{
				FMantleStorageExpansion Expansion;
				FString Error;
				const EMantleStorageAnswer Answer =
					FMantleStorage::Classify(Response.StatusCode, Response.Body, Response.Message, Expansion, Error);

				if (Answer == EMantleStorageAnswer::Retry && State->Retries < FMantleStorage::MaxRetries)
				{
					++State->Retries;
					const float Delay = FMantleStorage::RetryDelaySeconds(State->Retries);
					UE_LOG(LogMantle, Warning, TEXT("MANTLE_STORAGE resolving place %s failed (%s); retry %d of %d in %.0f s"),
						*State->PlaceId, *Error, State->Retries, FMantleStorage::MaxRetries, Delay);
					// A game-thread ticker, never a sleep: the game keeps running while the retry waits.
					FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([State](float)
					{
						SendResolve(State);
						return false;
					}), Delay);
					return;
				}

				FMantleStorageExpandResult Result;
				if (Answer == EMantleStorageAnswer::Apply)
				{
					Result.bSuccess = true;
					const int32 TurnedOff = FMantleStorage::Apply(State->Place, Expansion);
					UE_LOG(LogMantle, Log, TEXT("MANTLE_STORAGE place %s: %d entries expanded, %d missing, %d spawn(s) turned off"),
						*State->PlaceId, Expansion.Entries.Num(), Expansion.Missing.Num(), TurnedOff);
				}
				else
				{
					// Not a spawn to turn off: a client that goes on without what the server prepared diverges from it
					// for the whole session, so the load itself fails.
					Result.Error = FString::Printf(TEXT("The place's stored items could not be loaded (%s)."), *Error);
					UE_LOG(LogMantle, Error, TEXT("MANTLE_STORAGE place %s: %s"), *State->PlaceId, *Result.Error);
				}
				Result.Place = MoveTemp(State->Place);
				State->OnDone(MoveTemp(Result));
			});

		if (State->bServer)
		{
			FMantleApiClient::ResolveServerStorageAsync(State->Refs, Callback);
		}
		else
		{
			FMantleApiClient::ResolveStorageAsync(State->PlaceId, State->Refs, Callback);
		}
	}
}

void FMantleStorage::ExpandAsync(const FString& PlaceId, const bool bServer, FMantlePlace Place, TFunction<void(FMantleStorageExpandResult)> OnDone)
{
	if (Place.Storage.IsEmpty())
	{
		FMantleStorageExpandResult Unchanged;
		Unchanged.bSuccess = true;
		Unchanged.Place = MoveTemp(Place);
		OnDone(MoveTemp(Unchanged));
		return;
	}

	const TSharedRef<MantlePlaceStoragePrivate::FExpandState> State = MakeShared<MantlePlaceStoragePrivate::FExpandState>();
	State->PlaceId = PlaceId;
	State->bServer = bServer;
	State->Refs = RefsOf(Place.Storage);
	State->Place = MoveTemp(Place);
	State->OnDone = MoveTemp(OnDone);
	UE_LOG(LogMantle, Log, TEXT("MANTLE_STORAGE resolving %d stored item version(s) of place %s (%s)"),
		State->Refs.Num(), *PlaceId, bServer ? TEXT("server") : TEXT("player"));
	MantlePlaceStoragePrivate::SendResolve(State);
}
