// Copyright Joshua Eger. All Rights Reserved.
#include "MantleLibraryPublish.h"
#include "MantleCosmeticPackage.h"
#include "MantleLibraryPanel.h"
#include "MantlePlaceLimits.h"
#include "MantlePlaceStorage.h"
#include "MantlePlaceTypes.h"
#include "MantlePublisher.h"
#include "MantleRuntime.h"
#include "MantleScriptPackages.h"
#include "MantleSha256.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleLibraryPublishPrivate
{
	/** The most an item's name may be (the API's MaxNameRunes). */
	constexpr int32 MaxItemNameLength = 64;

	// Defined with the script package helpers below; the spawn rewrite reads and writes storage[] the same way (a script
	// entry's settings ride along).
	TArray<FMantleStorageEntry> ReadPlaceStorage(const TSharedPtr<FJsonObject>& Root);
	void WritePlaceStorage(const TSharedPtr<FJsonObject>& Root, const TArray<FMantleStorageEntry>& Entries);

	TSharedPtr<FJsonObject> ParseLibraryObject(const FString& Json)
	{
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
		{
			return nullptr;
		}
		return Object;
	}

	/** The place file as an object, numbers kept as their exact text so the rewrite changes nothing else. */
	TSharedPtr<FJsonObject> ParsePlaceExactly(const FString& Json)
	{
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		if (!FJsonSerializer::Deserialize(Reader, Object, FJsonSerializer::EFlags::StoreNumbersAsStrings) || !Object.IsValid())
		{
			return nullptr;
		}
		return Object;
	}

	FString WriteLibraryObject(const TSharedRef<FJsonObject>& Object)
	{
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Object, Writer);
		return Out;
	}

	/** The properties of every entity of SpawnType ("VehicleSpawn", "WeaponSpawn"), in entity order. */
	TArray<TSharedPtr<FJsonObject>> SpawnProperties(const TSharedPtr<FJsonObject>& Root, const TCHAR* SpawnType)
	{
		TArray<TSharedPtr<FJsonObject>> Out;
		const TArray<TSharedPtr<FJsonValue>>* Entities = nullptr;
		if (!Root.IsValid() || !Root->TryGetArrayField(TEXT("entities"), Entities) || !Entities)
		{
			return Out;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Entities)
		{
			const TSharedPtr<FJsonObject>* Entity = nullptr;
			FString Type;
			if (!Value.IsValid() || !Value->TryGetObject(Entity) || !Entity || !(*Entity)->TryGetStringField(TEXT("type"), Type)
				|| !Type.Equals(SpawnType, ESearchCase::CaseSensitive))
			{
				continue;
			}
			const TSharedPtr<FJsonObject>* Properties = nullptr;
			if ((*Entity)->TryGetObjectField(TEXT("properties"), Properties) && Properties)
			{
				Out.Add(*Properties);
			}
		}
		return Out;
	}

	bool ContainsName(const TArray<FMantleStorageEntry>& Entries, const FString& Name)
	{
		return Entries.ContainsByPredicate([&Name](const FMantleStorageEntry& Entry) { return Entry.Name.Equals(Name, ESearchCase::CaseSensitive); });
	}

	/** Base, or Base with _2, _3... until no entry has the name; the suffix never pushes it past the name limit. */
	FString UniqueStorageName(const FString& Base, const TArray<FMantleStorageEntry>& Entries)
	{
		if (!ContainsName(Entries, Base))
		{
			return Base;
		}
		for (int32 Suffix = 2; Suffix < 10000; ++Suffix)
		{
			const FString Tail = FString::Printf(TEXT("_%d"), Suffix);
			const FString Candidate = Base.Left(FMantleLibraryStorage::MaxNameLength - Tail.Len()) + Tail;
			if (!ContainsName(Entries, Candidate))
			{
				return Candidate;
			}
		}
		return Base;
	}
}

FString FMantleLibraryPublish::StorageNameFor(const FString& SourceName, const TCHAR* Fallback)
{
	FString Name;
	for (const TCHAR Char : SourceName)
	{
		const bool bAscii = (Char >= 'A' && Char <= 'Z') || (Char >= 'a' && Char <= 'z') || (Char >= '0' && Char <= '9') || Char == '_';
		Name.AppendChar(bAscii ? Char : TEXT('_'));
	}
	if (Name.IsEmpty() || Name.Replace(TEXT("_"), TEXT("")).IsEmpty())
	{
		return Fallback;
	}
	const TCHAR First = Name[0];
	if (!((First >= 'A' && First <= 'Z') || (First >= 'a' && First <= 'z')))
	{
		Name = FString(Fallback) + TEXT("_") + Name;
	}
	return Name.Left(FMantleLibraryStorage::MaxNameLength);
}

TArray<FString> FMantleLibraryPublish::VehicleAssetIdsIn(const FString& PlaceJson)
{
	TArray<FString> Ids;
	const TSharedPtr<FJsonObject> Root = MantleLibraryPublishPrivate::ParseLibraryObject(PlaceJson);
	for (const TSharedPtr<FJsonObject>& Properties : MantleLibraryPublishPrivate::SpawnProperties(Root, TEXT("VehicleSpawn")))
	{
		FString Vehicle;
		if (Properties->TryGetStringField(TEXT("vehicle"), Vehicle) && !Vehicle.IsEmpty())
		{
			Ids.AddUnique(Vehicle);
		}
	}
	return Ids;
}

TArray<FString> FMantleLibraryPublish::WeaponAssetIdsIn(const FString& PlaceJson)
{
	TArray<FString> Ids;
	const TSharedPtr<FJsonObject> Root = MantleLibraryPublishPrivate::ParseLibraryObject(PlaceJson);
	for (const TSharedPtr<FJsonObject>& Properties : MantleLibraryPublishPrivate::SpawnProperties(Root, TEXT("WeaponSpawn")))
	{
		FString Weapon;
		if (Properties->TryGetStringField(TEXT("weapon"), Weapon) && !Weapon.IsEmpty())
		{
			Ids.AddUnique(Weapon);
		}
	}
	return Ids;
}

bool FMantleLibraryPublish::RewritePlace(const FString& PlaceJson, const TArray<FMantleLibraryVehicleRef>& Vehicles, FString& OutJson, FString& OutError,
	TArray<FMantleStoredVehicle>* OutStored)
{
	return RewriteSpawns(TEXT("VehicleSpawn"), TEXT("vehicle"), TEXT("Vehicle"), PlaceJson, Vehicles, OutJson, OutError, OutStored);
}

bool FMantleLibraryPublish::RewriteWeaponSpawns(const FString& PlaceJson, const TArray<FMantleLibraryVehicleRef>& Weapons, FString& OutJson, FString& OutError,
	TArray<FMantleStoredVehicle>* OutStored)
{
	return RewriteSpawns(TEXT("WeaponSpawn"), TEXT("weapon"), TEXT("Weapon"), PlaceJson, Weapons, OutJson, OutError, OutStored);
}

bool FMantleLibraryPublish::RewriteSpawns(const TCHAR* SpawnType, const TCHAR* Field, const TCHAR* Fallback, const FString& PlaceJson,
	const TArray<FMantleLibraryVehicleRef>& Vehicles, FString& OutJson, FString& OutError, TArray<FMantleStoredVehicle>* OutStored)
{
	using namespace MantleLibraryPublishPrivate;
	OutJson = PlaceJson;
	OutError.Reset();
	if (OutStored)
	{
		OutStored->Reset();
	}
	if (Vehicles.Num() == 0)
	{
		return true;
	}
	const TSharedPtr<FJsonObject> Root = ParsePlaceExactly(PlaceJson);
	if (!Root.IsValid())
	{
		OutError = TEXT("The place file could not be read to add the library items to its storage.");
		return false;
	}
	const TArray<TSharedPtr<FJsonObject>> Spawns = SpawnProperties(Root, SpawnType);
	TSet<FString> Used;
	for (const TSharedPtr<FJsonObject>& Properties : Spawns)
	{
		FString Vehicle;
		if (Properties->TryGetStringField(Field, Vehicle) && !Vehicle.IsEmpty())
		{
			Used.Add(Vehicle);
		}
	}

	// What the creator already declared on Place Settings.
	TArray<FMantleStorageEntry> Entries = MantleLibraryPublishPrivate::ReadPlaceStorage(Root);

	TMap<FString, FString> NameByAsset;
	for (const FMantleLibraryVehicleRef& Vehicle : Vehicles)
	{
		if (!Used.Contains(Vehicle.AssetId))
		{
			continue;
		}
		if (!FMantlePlaceLimits::IsLibraryItemId(Vehicle.ItemId) || Vehicle.Version < 1)
		{
			OutError = FString::Printf(TEXT("Mantle did not give '%s' a usable library item, so it cannot be added to this place's storage."), *Vehicle.Name);
			return false;
		}
		const int32 Existing = FMantleLibraryStorage::FindItem(Entries, Vehicle.ItemId);
		if (Existing != INDEX_NONE)
		{
			// The creator already stores this item: its new version replaces the old one, under the same name.
			Entries[Existing].Version = Vehicle.Version;
			NameByAsset.Add(Vehicle.AssetId, Entries[Existing].Name);
			continue;
		}
		FMantleStorageEntry NewEntry;
		NewEntry.Name = UniqueStorageName(StorageNameFor(Vehicle.Name, Fallback), Entries);
		NewEntry.Item = Vehicle.ItemId;
		NewEntry.Version = Vehicle.Version;
		NameByAsset.Add(Vehicle.AssetId, NewEntry.Name);
		Entries.Add(MoveTemp(NewEntry));
	}
	if (NameByAsset.Num() == 0)
	{
		return true;
	}
	if (Entries.Num() > FMantlePlaceLimits::MaxStorageEntries)
	{
		OutError = FString::Printf(TEXT("This place would store %d library items, and a place stores at most %d. Remove some from Place Settings > Storage, or use fewer vehicles."),
			Entries.Num(), FMantlePlaceLimits::MaxStorageEntries);
		return false;
	}
	const TArray<FMantleStorageProblem> Problems = FMantleStorage::CheckEntries(Entries);
	if (Problems.Num() > 0)
	{
		OutError = FString::Printf(TEXT("This place's storage is not valid: %s"), *Problems[0].Message);
		return false;
	}

	if (OutStored)
	{
		for (const FMantleLibraryVehicleRef& Vehicle : Vehicles)
		{
			if (const FString* Name = NameByAsset.Find(Vehicle.AssetId))
			{
				FMantleStoredVehicle& Stored = OutStored->AddDefaulted_GetRef();
				Stored.AssetId = Vehicle.AssetId;
				Stored.StorageName = *Name;
				Stored.ItemId = Vehicle.ItemId;
				Stored.Version = Vehicle.Version;
			}
		}
	}

	for (const TSharedPtr<FJsonObject>& Properties : Spawns)
	{
		FString Vehicle;
		if (!Properties->TryGetStringField(Field, Vehicle))
		{
			continue;
		}
		if (const FString* Name = NameByAsset.Find(Vehicle))
		{
			Properties->RemoveField(Field);
			Properties->SetStringField(TEXT("storageItem"), *Name);
		}
	}

	// A stored vehicle is expanded from its item when the place loads, so the place stops carrying it.
	const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
	if (Root->TryGetArrayField(TEXT("assets"), Assets) && Assets)
	{
		TArray<TSharedPtr<FJsonValue>> Kept;
		for (const TSharedPtr<FJsonValue>& Value : *Assets)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			FString Id;
			if (Value.IsValid() && Value->TryGetObject(Object) && Object && (*Object)->TryGetStringField(TEXT("id"), Id) && NameByAsset.Contains(Id))
			{
				continue;
			}
			Kept.Add(Value);
		}
		Root->SetArrayField(TEXT("assets"), Kept);
	}

	MantleLibraryPublishPrivate::WritePlaceStorage(Root, Entries);

	OutJson = WriteLibraryObject(Root.ToSharedRef());
	return true;
}

void FMantleLibraryPublish::MergeIntoStorage(TArray<FMantleStorageEntry>& Storage, TArray<FMantleStoredVehicle>& Stored)
{
	for (FMantleStoredVehicle& Vehicle : Stored)
	{
		const int32 Existing = FMantleLibraryStorage::FindItem(Storage, Vehicle.ItemId);
		if (Existing != INDEX_NONE)
		{
			Storage[Existing].Version = Vehicle.Version;
			Vehicle.StorageName = Storage[Existing].Name;
			continue;
		}
		FMantleStorageEntry Entry;
		Entry.Name = MantleLibraryPublishPrivate::UniqueStorageName(StorageNameFor(Vehicle.StorageName), Storage);
		Entry.Item = Vehicle.ItemId;
		Entry.Version = Vehicle.Version;
		Vehicle.StorageName = Entry.Name;
		Storage.Add(MoveTemp(Entry));
	}
}

FString FMantleLibraryPublish::MakeItemBody(const FString& Name, bool bPublic, const FString& PrimaryAssetId, const FMantleProvenance& Provenance,
	const TCHAR* Kind, const FString& ThumbnailAssetId)
{
	const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("kind"), Kind);
	Body->SetStringField(TEXT("name"), Name.TrimStartAndEnd().Left(MantleLibraryPublishPrivate::MaxItemNameLength));
	if (bPublic)
	{
		// Public is the API's default, and the one that needs the declaration.
		Body->SetStringField(TEXT("visibility"), TEXT("public"));
		Body->SetStringField(TEXT("rightsDeclarationVersion"), MantleRights::DeclarationVersion);
	}
	else
	{
		Body->SetStringField(TEXT("visibility"), TEXT("private"));
	}
	Body->SetStringField(TEXT("primaryAssetId"), PrimaryAssetId);
	if (!ThumbnailAssetId.IsEmpty())
	{
		Body->SetStringField(TEXT("thumbnailAssetId"), ThumbnailAssetId);
	}
	Provenance.AddTo(*Body, bPublic);
	return MantleLibraryPublishPrivate::WriteLibraryObject(Body);
}

FString FMantleLibraryPublish::MakeVersionBody(const FString& PrimaryAssetId, bool bPublic, const FMantleProvenance& Provenance)
{
	const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("primaryAssetId"), PrimaryAssetId);
	Provenance.AddTo(*Body, bPublic);
	return MantleLibraryPublishPrivate::WriteLibraryObject(Body);
}

FString FMantleLibraryPublish::MakeIdempotencyKey(const FString& SourcePath, const FString& AssetId, bool bPublic, int32 Generation)
{
	FString Input = FString::Printf(TEXT("vehicle-item|%s|%s|%s"), *SourcePath, *AssetId, bPublic ? TEXT("public") : TEXT("private"));
	if (Generation > 0)
	{
		Input += FString::Printf(TEXT("|g%d"), Generation);
	}
	const FTCHARToUTF8 Text(*Input);
	const FString Digest = FMantleSha256::HexDigest(TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Text.Get()), Text.Length()));
	return TEXT("veh-") + Digest.Left(40);
}

FString FMantleLibraryPublish::MakeVisibilityPatchBody(bool bPublic, const FString& ThumbnailAssetId)
{
	const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("visibility"), bPublic ? TEXT("public") : TEXT("private"));
	if (bPublic)
	{
		Body->SetStringField(TEXT("rightsDeclarationVersion"), MantleRights::DeclarationVersion);
	}
	if (!ThumbnailAssetId.IsEmpty())
	{
		Body->SetStringField(TEXT("thumbnailAssetId"), ThumbnailAssetId);
	}
	return MantleLibraryPublishPrivate::WriteLibraryObject(Body);
}

bool FMantleLibraryPublish::ReadItemState(const FString& ItemBody, FString& OutVisibility, FString& OutStatus)
{
	OutVisibility.Reset();
	OutStatus.Reset();
	const TSharedPtr<FJsonObject> Object = MantleLibraryPublishPrivate::ParseLibraryObject(ItemBody);
	if (!Object.IsValid())
	{
		return false;
	}
	Object->TryGetStringField(TEXT("visibility"), OutVisibility);
	Object->TryGetStringField(TEXT("status"), OutStatus);
	return true;
}

bool FMantleLibraryPublish::VisibilityDiffers(const FString& ItemVisibility, bool bWantPublic)
{
	if (ItemVisibility.Equals(TEXT("public"), ESearchCase::CaseSensitive))
	{
		return !bWantPublic;
	}
	if (ItemVisibility.Equals(TEXT("private"), ESearchCase::CaseSensitive))
	{
		return bWantPublic;
	}
	return false;
}

bool FMantleLibraryPublish::ResolveExistingItemPublic(const FString& ItemVisibility, const FMantleVehicleLibraryChoice& Choice)
{
	if (Choice.bKeepExistingVisibility)
	{
		if (ItemVisibility.Equals(TEXT("public"), ESearchCase::CaseSensitive))
		{
			return true;
		}
		if (ItemVisibility.Equals(TEXT("private"), ESearchCase::CaseSensitive))
		{
			return false;
		}
	}
	return Choice.bPublic;
}

FString FMantleLibraryPublish::MakeThumbnailPatchBody(const FString& ThumbnailAssetId)
{
	const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("thumbnailAssetId"), ThumbnailAssetId);
	return MantleLibraryPublishPrivate::WriteLibraryObject(Body);
}

bool FMantleLibraryPublish::IsDelisted(const FString& ItemStatus)
{
	return ItemStatus.Equals(TEXT("delisted"), ESearchCase::CaseSensitive);
}

bool FMantleLibraryPublish::IsTakenDown(const FString& ItemStatus)
{
	return ItemStatus.Equals(TEXT("taken_down"), ESearchCase::CaseSensitive);
}

bool FMantleLibraryPublish::ShouldRetry(const FMantleApiResult& Refused, bool bSafeToRepeat)
{
	if (Refused.StatusCode == 429)
	{
		return true;
	}
	return bSafeToRepeat && (Refused.StatusCode == 0 || Refused.StatusCode >= 500);
}

bool FMantleLibraryPublish::ReadCreated(const FString& Body, FString& OutItemId, int32& OutVersion)
{
	OutItemId.Reset();
	OutVersion = 0;
	const TSharedPtr<FJsonObject> Object = MantleLibraryPublishPrivate::ParseLibraryObject(Body);
	double Version = 0.0;
	if (!Object.IsValid() || !Object->TryGetStringField(TEXT("itemId"), OutItemId) || !Object->TryGetNumberField(TEXT("version"), Version))
	{
		OutItemId.Reset();
		return false;
	}
	OutVersion = static_cast<int32>(Version);
	return FMantlePlaceLimits::IsLibraryItemId(OutItemId) && OutVersion >= 1;
}

bool FMantleLibraryPublish::FindVersionWithAsset(const FString& ItemBody, const FString& AssetId, int32& OutVersion)
{
	OutVersion = 0;
	const TSharedPtr<FJsonObject> Object = MantleLibraryPublishPrivate::ParseLibraryObject(ItemBody);
	const TArray<TSharedPtr<FJsonValue>>* Versions = nullptr;
	if (!Object.IsValid() || !Object->TryGetArrayField(TEXT("versions"), Versions) || !Versions)
	{
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Versions)
	{
		const TSharedPtr<FJsonObject>* Version = nullptr;
		FString Primary;
		double Number = 0.0;
		if (Value.IsValid() && Value->TryGetObject(Version) && Version && (*Version)->TryGetStringField(TEXT("primaryAssetId"), Primary)
			&& Primary.Equals(AssetId, ESearchCase::CaseSensitive) && (*Version)->TryGetNumberField(TEXT("version"), Number)
			&& Number >= 1.0)
		{
			OutVersion = FMath::Max(OutVersion, static_cast<int32>(Number));
		}
	}
	return OutVersion >= 1;
}

FString FMantleLibraryPublish::DescribeFailure(const FMantleApiResult& Refused, const FString& VehicleName)
{
	const FString& Code = Refused.Code;
	if (Code == TEXT("asset_unavailable"))
	{
		return FString::Printf(TEXT("Mantle is not ready to make '%s' a library item yet (its files are still being screened, or were refused). Choose Publish again in a minute."), *VehicleName);
	}
	if (Code == TEXT("rights_declaration_required"))
	{
		return FString::Printf(TEXT("Mantle needs the rights declaration to make '%s' public. Tick it, or choose Private, then publish again."), *VehicleName);
	}
	const FString Moderation = FMantlePublisher::DescribeModerationFailure(Refused);
	if (!Moderation.IsEmpty())
	{
		return FString::Printf(TEXT("'%s' was not added to your library, so nothing went live. %s"), *VehicleName, *Moderation);
	}
	if (Code == TEXT("too_many_versions"))
	{
		return FString::Printf(TEXT("'%s' already has as many versions in your library as Mantle allows. Rename or duplicate the vehicle asset in the Content Browser (a new asset path makes a new library item), then publish again."), *VehicleName);
	}
	return FString::Printf(TEXT("Mantle could not add '%s' to your library, so nothing went live: %s"), *VehicleName, *FMantlePublisher::DescribeApiFailure(Refused));
}

// ---------------------------------------------------------------------------------------------------------------------
// Script packages.

namespace MantleLibraryPublishPrivate
{
	TArray<TSharedPtr<FJsonValue>> ScriptFilesJson(const TArray<FMantleLibraryScriptFile>& Files)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FMantleLibraryScriptFile& File : Files)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("path"), File.Path);
			Object->SetStringField(TEXT("side"), File.Side);
			Object->SetStringField(TEXT("language"), File.Language);
			Object->SetStringField(TEXT("source"), File.Source);
			Values.Add(MakeShared<FJsonValueObject>(Object));
		}
		return Values;
	}

	bool VersionFilesEqual(const TSharedPtr<FJsonObject>& Version, const TArray<FMantleLibraryScriptFile>& Files)
	{
		const TArray<TSharedPtr<FJsonValue>>* Scripts = nullptr;
		if (!Version->TryGetArrayField(TEXT("scripts"), Scripts) || !Scripts || Scripts->Num() != Files.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < Files.Num(); ++Index)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			FString Path, Side, Language, Source;
			if (!(*Scripts)[Index].IsValid() || !(*Scripts)[Index]->TryGetObject(Object) || !Object
				|| !(*Object)->TryGetStringField(TEXT("path"), Path) || !(*Object)->TryGetStringField(TEXT("side"), Side)
				|| !(*Object)->TryGetStringField(TEXT("language"), Language) || !(*Object)->TryGetStringField(TEXT("source"), Source))
			{
				return false;
			}
			const FMantleLibraryScriptFile& File = Files[Index];
			if (!Path.Equals(File.Path, ESearchCase::CaseSensitive) || !Side.Equals(File.Side, ESearchCase::CaseSensitive)
				|| !Language.Equals(File.Language, ESearchCase::CaseSensitive) || !Source.Equals(File.Source, ESearchCase::CaseSensitive))
			{
				return false;
			}
		}
		return true;
	}

	/**
	 * Whether a version's manifest is exactly Manifest: both written the way the plugin publishes one (ManifestToJson over the
	 * runtime's reading of the version's), so every field a creator can change counts, including the settings (required, min,
	 * max, options).
	 */
	bool VersionManifestEqual(const TSharedPtr<FJsonObject>& Version, const FMantleScriptPackageManifest& Manifest)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (!Version->TryGetObjectField(TEXT("manifest"), Object) || !Object || !Object->IsValid())
		{
			return false;
		}
		const FMantleScriptPackageManifest Published = FMantleStorage::ReadScriptPackageManifest(*Object);
		return FMantleScriptPackages::ManifestToJson(Published).Equals(FMantleScriptPackages::ManifestToJson(Manifest), ESearchCase::CaseSensitive);
	}

	/** The storage[] of a place file object, read leniently (a malformed entry reads empty, for CheckEntries to name). */
	TArray<FMantleStorageEntry> ReadPlaceStorage(const TSharedPtr<FJsonObject>& Root)
	{
		TArray<FMantleStorageEntry> Entries;
		const TArray<TSharedPtr<FJsonValue>>* Declared = nullptr;
		if (Root.IsValid() && Root->TryGetArrayField(TEXT("storage"), Declared) && Declared)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Declared)
			{
				const TSharedPtr<FJsonObject>* Object = nullptr;
				FMantleStorageEntry& Entry = Entries.AddDefaulted_GetRef();
				double Version = 0.0;
				if (Value.IsValid() && Value->TryGetObject(Object) && Object)
				{
					(*Object)->TryGetStringField(TEXT("name"), Entry.Name);
					(*Object)->TryGetStringField(TEXT("item"), Entry.Item);
					(*Object)->TryGetNumberField(TEXT("version"), Version);
					// A script package's settings ride along unchanged (text values only).
					const TSharedPtr<FJsonObject>* Settings = nullptr;
					if ((*Object)->TryGetObjectField(TEXT("settings"), Settings) && Settings && Settings->IsValid())
					{
						for (const auto& Pair : (*Settings)->Values)
						{
							FString Text;
							if (Pair.Value.IsValid() && Pair.Value->Type == EJson::String && Pair.Value->TryGetString(Text))
							{
								Entry.Settings.Add(FString(Pair.Key.ToView()), MoveTemp(Text));
							}
						}
					}
				}
				Entry.Version = static_cast<int32>(Version);
			}
		}
		return Entries;
	}

	void WritePlaceStorage(const TSharedPtr<FJsonObject>& Root, const TArray<FMantleStorageEntry>& Entries)
	{
		TArray<TSharedPtr<FJsonValue>> StorageValues;
		for (const FMantleStorageEntry& Entry : Entries)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("name"), Entry.Name);
			Object->SetStringField(TEXT("item"), Entry.Item);
			Object->SetNumberField(TEXT("version"), Entry.Version);
			if (Entry.Settings.Num() > 0)
			{
				TArray<FString> Keys;
				Entry.Settings.GetKeys(Keys);
				Keys.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::CaseSensitive) < 0; });
				const TSharedRef<FJsonObject> Settings = MakeShared<FJsonObject>();
				for (const FString& Key : Keys)
				{
					Settings->SetStringField(Key, Entry.Settings.FindChecked(Key));
				}
				Object->SetObjectField(TEXT("settings"), Settings);
			}
			StorageValues.Add(MakeShared<FJsonValueObject>(Object));
		}
		Root->SetArrayField(TEXT("storage"), StorageValues);
	}

	/**
	 * The entry just re-pinned to Package's version keeps the settings the creator set, less any the version's manifest
	 * no longer declares (logged by name). With no manifest on the ref nothing is dropped.
	 */
	void PruneRepinnedSettings(FMantleStorageEntry& Entry, const FMantleLibraryPackageRef& Package)
	{
		if (Package.Manifest.IsEmpty() || Entry.Settings.Num() == 0)
		{
			return;
		}
		const TArray<FString> Dropped = FMantleScriptPackages::PruneSettings(Entry.Settings, FMantleScriptPackages::ReadManifestForHints(Package.Manifest));
		if (Dropped.Num() > 0)
		{
			UE_LOG(LogMantle, Log, TEXT("MANTLE_STORAGE package '%s' version %d no longer declares the setting(s) %s; dropped from its storage entry"),
				*Entry.Name, Package.Version, *FString::Join(Dropped, TEXT(", ")));
		}
	}
}

FString FMantleLibraryPublish::MakeScriptItemBody(const FMantleScriptPackageManifest& Manifest, const bool bPublic, const TArray<FMantleLibraryScriptFile>& Files)
{
	const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("kind"), FMantleStorage::KindScript);
	Body->SetStringField(TEXT("name"), Manifest.Name.TrimStartAndEnd().Left(MantleLibraryPublishPrivate::MaxItemNameLength));
	Body->SetStringField(TEXT("description"), Manifest.Description);
	if (bPublic)
	{
		Body->SetStringField(TEXT("visibility"), TEXT("public"));
		Body->SetStringField(TEXT("rightsDeclarationVersion"), MantleRights::DeclarationVersion);
	}
	else
	{
		Body->SetStringField(TEXT("visibility"), TEXT("private"));
	}
	Body->SetArrayField(TEXT("scripts"), MantleLibraryPublishPrivate::ScriptFilesJson(Files));
	Body->SetObjectField(TEXT("manifest"), FMantleScriptPackages::ManifestToJsonObject(Manifest));
	return MantleLibraryPublishPrivate::WriteLibraryObject(Body);
}

FString FMantleLibraryPublish::MakeScriptVersionBody(const FMantleScriptPackageManifest& Manifest, const TArray<FMantleLibraryScriptFile>& Files)
{
	const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetArrayField(TEXT("scripts"), MantleLibraryPublishPrivate::ScriptFilesJson(Files));
	Body->SetObjectField(TEXT("manifest"), FMantleScriptPackages::ManifestToJsonObject(Manifest));
	return MantleLibraryPublishPrivate::WriteLibraryObject(Body);
}

FString FMantleLibraryPublish::MakeScriptIdempotencyKey(const FString& MemoryKey, const FMantleScriptPackageManifest& Manifest, const TArray<FMantleLibraryScriptFile>& Files,
	const bool bPublic, const int32 Generation)
{
	FString Input = FString::Printf(TEXT("script-item|%s|%s|%s"), *MemoryKey, bPublic ? TEXT("public") : TEXT("private"), *FMantleScriptPackages::ManifestToJson(Manifest));
	for (const FMantleLibraryScriptFile& File : Files)
	{
		Input += FString::Printf(TEXT("|%s:%s:%s:%s"), *File.Path, *File.Side, *File.Language, *File.Source);
	}
	if (Generation > 0)
	{
		Input += FString::Printf(TEXT("|g%d"), Generation);
	}
	const FTCHARToUTF8 Text(*Input);
	const FString Digest = FMantleSha256::HexDigest(TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Text.Get()), Text.Length()));
	return TEXT("pkg-") + Digest.Left(40);
}

bool FMantleLibraryPublish::FindVersionWithScripts(const FString& ItemBody, const FMantleScriptPackageManifest& Manifest, const TArray<FMantleLibraryScriptFile>& Files,
	int32& OutVersion)
{
	OutVersion = 0;
	const TSharedPtr<FJsonObject> Object = MantleLibraryPublishPrivate::ParseLibraryObject(ItemBody);
	const TArray<TSharedPtr<FJsonValue>>* Versions = nullptr;
	if (!Object.IsValid() || !Object->TryGetArrayField(TEXT("versions"), Versions) || !Versions)
	{
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Versions)
	{
		const TSharedPtr<FJsonObject>* Version = nullptr;
		double Number = 0.0;
		if (Value.IsValid() && Value->TryGetObject(Version) && Version && (*Version)->TryGetNumberField(TEXT("version"), Number) && Number >= 1.0
			&& MantleLibraryPublishPrivate::VersionFilesEqual(*Version, Files) && MantleLibraryPublishPrivate::VersionManifestEqual(*Version, Manifest))
		{
			OutVersion = FMath::Max(OutVersion, static_cast<int32>(Number));
		}
	}
	return OutVersion >= 1;
}

void FMantleLibraryPublish::MergePackagesIntoStorage(TArray<FMantleStorageEntry>& Storage, const TArray<FMantleLibraryPackageRef>& Packages)
{
	for (const FMantleLibraryPackageRef& Package : Packages)
	{
		const int32 Named = Storage.IndexOfByPredicate([&Package](const FMantleStorageEntry& Entry)
		{
			return Entry.Name.Equals(Package.Name, ESearchCase::CaseSensitive);
		});
		if (Named != INDEX_NONE)
		{
			Storage[Named].Item = Package.ItemId;
			Storage[Named].Version = Package.Version;
			MantleLibraryPublishPrivate::PruneRepinnedSettings(Storage[Named], Package);
			for (int32 Index = Storage.Num() - 1; Index >= 0; --Index)
			{
				if (Index != Named && Storage[Index].Item.Equals(Package.ItemId, ESearchCase::CaseSensitive))
				{
					Storage.RemoveAt(Index);
				}
			}
			continue;
		}
		const int32 Held = FMantleLibraryStorage::FindItem(Storage, Package.ItemId);
		if (Held != INDEX_NONE)
		{
			Storage[Held].Name = Package.Name;
			Storage[Held].Version = Package.Version;
			MantleLibraryPublishPrivate::PruneRepinnedSettings(Storage[Held], Package);
			continue;
		}
		FMantleStorageEntry Entry;
		Entry.Name = Package.Name;
		Entry.Item = Package.ItemId;
		Entry.Version = Package.Version;
		Storage.Add(MoveTemp(Entry));
	}
}

bool FMantleLibraryPublish::RewriteScriptPackages(const FString& PlaceJson, const TArray<FMantleLibraryPackageRef>& Packages, FString& OutJson, FString& OutError)
{
	using namespace MantleLibraryPublishPrivate;
	OutJson = PlaceJson;
	OutError.Reset();
	if (Packages.Num() == 0)
	{
		return true;
	}
	for (const FMantleLibraryPackageRef& Package : Packages)
	{
		if (!FMantlePlaceLimits::IsLibraryItemId(Package.ItemId) || Package.Version < 1 || !FMantlePlaceLimits::IsStorageName(Package.Name))
		{
			OutError = FString::Printf(TEXT("Mantle did not give the package '%s' a usable library item, so it cannot be added to this place's storage."), *Package.Name);
			return false;
		}
	}
	const TSharedPtr<FJsonObject> Root = ParsePlaceExactly(PlaceJson);
	if (!Root.IsValid())
	{
		OutError = TEXT("The place file could not be read to add the script packages to its storage.");
		return false;
	}
	TArray<FMantleStorageEntry> Entries = ReadPlaceStorage(Root);
	MergePackagesIntoStorage(Entries, Packages);
	if (Entries.Num() > FMantlePlaceLimits::MaxStorageEntries)
	{
		OutError = FString::Printf(TEXT("This place would store %d library items with its script packages, and a place stores at most %d. Remove some from Place Settings > Storage, or combine packages."),
			Entries.Num(), FMantlePlaceLimits::MaxStorageEntries);
		return false;
	}
	const TArray<FMantleStorageProblem> Problems = FMantleStorage::CheckEntries(Entries);
	if (Problems.Num() > 0)
	{
		OutError = FString::Printf(TEXT("This place's storage is not valid: %s"), *Problems[0].Message);
		return false;
	}
	WritePlaceStorage(Root, Entries);
	OutJson = WriteLibraryObject(Root.ToSharedRef());
	return true;
}

void FMantleLibraryPublish::DropUnstoredScriptPoints(const FString& PlaceJson, FString& OutJson, TArray<FString>& OutDropped)
{
	using namespace MantleLibraryPublishPrivate;
	OutJson = PlaceJson;
	OutDropped.Reset();
	const TSharedPtr<FJsonObject> Root = ParsePlaceExactly(PlaceJson);
	const TArray<TSharedPtr<FJsonValue>>* Entities = nullptr;
	if (!Root.IsValid() || !Root->TryGetArrayField(TEXT("entities"), Entities) || !Entities)
	{
		return;
	}
	const TArray<FMantleStorageEntry> Storage = ReadPlaceStorage(Root);
	TArray<TSharedPtr<FJsonValue>> Kept;
	for (const TSharedPtr<FJsonValue>& Value : *Entities)
	{
		const TSharedPtr<FJsonObject>* Entity = nullptr;
		FString Type;
		const TSharedPtr<FJsonObject>* Properties = nullptr;
		FString StorageItem;
		if (Value.IsValid() && Value->TryGetObject(Entity) && Entity && (*Entity)->TryGetStringField(TEXT("type"), Type)
			&& Type.Equals(TEXT("ScriptPoint"), ESearchCase::CaseSensitive) && (*Entity)->TryGetObjectField(TEXT("properties"), Properties) && Properties
			&& (*Properties)->TryGetStringField(TEXT("storageItem"), StorageItem)
			&& !Storage.ContainsByPredicate([&StorageItem](const FMantleStorageEntry& Entry) { return Entry.Name.Equals(StorageItem, ESearchCase::CaseSensitive); }))
		{
			FString Name;
			(*Entity)->TryGetStringField(TEXT("name"), Name);
			OutDropped.Add(Name.IsEmpty() ? StorageItem : Name);
			continue;
		}
		Kept.Add(Value);
	}
	if (OutDropped.Num() == 0)
	{
		return;
	}
	Root->SetArrayField(TEXT("entities"), Kept);
	OutJson = WriteLibraryObject(Root.ToSharedRef());
}
