// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePreviewLaunch.h"
#include "MantleApiClient.h"
#include "MantleClientFeatures.h"
#include "MantlePlaceAssets.h"
#include "MantlePlaceLimits.h"
#include "MantlePlaceStorage.h"
#include "MantleRuntime.h"

#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include "Windows/WindowsHWrapper.h"
#include "Windows/HideWindowsPlatformTypes.h"
#endif

#include <atomic>

namespace MantlePreviewLaunchPrivate
{
	/** The cached launch policy: -1 unknown, 0 normal, 1 preview. */
	std::atomic<int32>& RequestedCache()
	{
		static std::atomic<int32> Cache{-1};
		return Cache;
	}

	/** True when Token is -MantlePreview or -MantlePreview=..., and nothing else sharing the prefix. */
	bool IsPreviewToken(const FString& Token)
	{
		return Token.Equals(TEXT("-MantlePreview"), ESearchCase::IgnoreCase)
			|| Token.StartsWith(TEXT("-MantlePreview="), ESearchCase::IgnoreCase);
	}

	/** True when Token is -MantlePreviewCapabilities or -MantlePreviewCapabilities=...; it implies the offline policy too. */
	bool IsCapabilitiesToken(const FString& Token)
	{
		return Token.Equals(TEXT("-MantlePreviewCapabilities"), ESearchCase::IgnoreCase)
			|| Token.StartsWith(TEXT("-MantlePreviewCapabilities="), ESearchCase::IgnoreCase);
	}

	/** The manifest schema versions this build's LoadManifest reads. */
	const int32 SupportedManifestSchemas[] = { 1, 2 };

	/** The cook tool beside the client that the editor runs; the file name only, the editor resolves the folder. */
	const TCHAR* const CookToolName = TEXT("mantle-cook.exe");

	/** FString == ignores case; manifest keys and values are exact. */
	bool Is(FStringView Value, const TCHAR* Expected)
	{
		return Value.Equals(Expected, ESearchCase::CaseSensitive);
	}

	/** The extensions MantleScriptLanguageFromExtension maps to a language; keep the two in step. */
	bool IsScriptExtension(const FString& Extension)
	{
		return Is(Extension, TEXT("luau")) || Is(Extension, TEXT("lua")) || Is(Extension, TEXT("js"))
			|| Is(Extension, TEXT("ts")) || Is(Extension, TEXT("py"));
	}

	/** One schema 2 section that must be an array when present; false with the reason when it is not. */
	bool ReadSectionArray(const FJsonObject& Object, const TCHAR* Name, const TArray<TSharedPtr<FJsonValue>>*& OutArray, FString& OutError)
	{
		OutArray = nullptr;
		if (Object.HasField(Name) && !Object.TryGetArrayField(Name, OutArray))
		{
			OutError = FString::Printf(TEXT("The preview manifest's %s must be an array."), Name);
			return false;
		}
		return true;
	}

	/** Keys of Entry that are not in Allowed, or an empty string when there are none. */
	FString FirstUnknownKey(const FJsonObject& Entry, std::initializer_list<const TCHAR*> Allowed)
	{
		for (const auto& Field : Entry.Values)
		{
			const FString Key(Field.Key.ToView());
			bool bFound = false;
			for (const TCHAR* Name : Allowed)
			{
				bFound = bFound || Is(Key, Name);
			}
			if (!bFound)
			{
				return Key;
			}
		}
		return FString();
	}

	/** manifest "uiDocuments": each file is read as text, the place limits applying to path, count and size. */
	bool LoadUIDocuments(const TArray<TSharedPtr<FJsonValue>>& Entries, FMantlePreviewManifest& Result, FString& OutError)
	{
		if (Entries.Num() > FMantlePlaceLimits::MaxUIDocuments)
		{
			OutError = FString::Printf(TEXT("The preview manifest lists more than %d UI files."), FMantlePlaceLimits::MaxUIDocuments);
			return false;
		}
		TSet<FString> SeenPaths;
		for (const TSharedPtr<FJsonValue>& Value : Entries)
		{
			const TSharedPtr<FJsonObject>* Entry = nullptr;
			FString Path, File;
			if (!Value.IsValid() || !Value->TryGetObject(Entry) || !Entry->IsValid() || (*Entry)->Values.Num() != 2
				|| !(*Entry)->TryGetStringField(TEXT("path"), Path) || !(*Entry)->TryGetStringField(TEXT("file"), File))
			{
				OutError = TEXT("Each preview UI file needs exactly a path and a file.");
				return false;
			}
			if (!MantlePreviewLaunch::IsBundleRelativePath(Path) || Path.Len() > FMantlePlaceLimits::MaxUIDocumentPathLength
				|| !FMantlePlaceLimits::IsUIDocumentPath(Path))
			{
				OutError = FString::Printf(TEXT("Preview UI path '%s' must be UI/ followed by a relative .rml or .rcss path."), *Path);
				return false;
			}
			// The place matches these paths ignoring case, so two spellings of one path would hide one.
			if (SeenPaths.Contains(Path.ToLower()))
			{
				OutError = FString::Printf(TEXT("Preview UI file '%s' is listed more than once."), *Path);
				return false;
			}
			SeenPaths.Add(Path.ToLower());
			if (!MantlePreviewLaunch::IsBundleRelativePath(File))
			{
				OutError = FString::Printf(TEXT("Preview UI file '%s' must be a relative path inside the bundle folder."), *File);
				return false;
			}
			const FString FileAbsolute = Result.BundleDirectory / File;
			const int64 Bytes = IFileManager::Get().FileSize(*FileAbsolute);
			if (Bytes < 0 || Bytes > FMantlePlaceLimits::MaxUIDocumentBytes)
			{
				OutError = FString::Printf(TEXT("Preview UI file '%s' is missing or larger than %d bytes."), *File, FMantlePlaceLimits::MaxUIDocumentBytes);
				return false;
			}
			FMantlePreviewUIDocument Document;
			if (!FFileHelper::LoadFileToString(Document.Text, *FileAbsolute))
			{
				OutError = FString::Printf(TEXT("Could not read preview UI file '%s'."), *File);
				return false;
			}
			Document.Path = Path;
			Result.UIDocuments.Add(MoveTemp(Document));
		}
		return true;
	}

	/** manifest "assets": every entry is checked against the contract; the bytes themselves are verified when the asset store seeds them. */
	bool LoadAssets(const TArray<TSharedPtr<FJsonValue>>& Entries, FMantlePreviewManifest& Result, FString& OutError)
	{
		if (Entries.Num() > MantlePreviewLimits::MaxAssets)
		{
			OutError = FString::Printf(TEXT("The preview manifest lists more than %d assets."), MantlePreviewLimits::MaxAssets);
			return false;
		}
		TSet<FString> Ids;
		int64 TotalBytes = 0;
		for (const TSharedPtr<FJsonValue>& Value : Entries)
		{
			const TSharedPtr<FJsonObject>* Entry = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(Entry) || !Entry->IsValid())
			{
				OutError = TEXT("Each preview asset must be an object.");
				return false;
			}
			const FJsonObject& Fields = **Entry;
			const FString Unknown = FirstUnknownKey(Fields, { TEXT("id"), TEXT("kind"), TEXT("file"), TEXT("cookedHash"), TEXT("cookedBytes"), TEXT("role"), TEXT("textures") });
			FMantlePreviewAsset Asset;
			FString File;
			double Bytes = 0.0;
			if (!Unknown.IsEmpty() || !Fields.TryGetStringField(TEXT("id"), Asset.Id) || !Fields.TryGetStringField(TEXT("kind"), Asset.Kind)
				|| !Fields.TryGetStringField(TEXT("file"), File) || !Fields.TryGetStringField(TEXT("cookedHash"), Asset.CookedHash)
				|| !Fields.TryGetNumberField(TEXT("cookedBytes"), Bytes))
			{
				OutError = Unknown.IsEmpty()
					? FString::Printf(TEXT("Preview asset '%s' needs an id, kind, file, cookedHash and cookedBytes."), *Asset.Id)
					: FString::Printf(TEXT("Unknown preview asset field '%s'."), *Unknown);
				return false;
			}
			if (!FMantlePlaceAssets::IsUploadedId(Asset.Id))
			{
				OutError = FString::Printf(TEXT("Preview asset id '%s' must be ast_u and 32 lowercase hex digits."), *Asset.Id);
				return false;
			}
			if (Ids.Contains(Asset.Id))
			{
				OutError = FString::Printf(TEXT("Preview asset '%s' is listed more than once."), *Asset.Id);
				return false;
			}
			Ids.Add(Asset.Id);
			if (Asset.Kind.IsEmpty() || Asset.Kind.Len() > 64)
			{
				OutError = FString::Printf(TEXT("Preview asset '%s' needs a kind."), *Asset.Id);
				return false;
			}
			FMantleContentHash Hash;
			FString HashError;
			if (!FMantleContentHash::Parse(Asset.CookedHash, Hash, &HashError))
			{
				OutError = FString::Printf(TEXT("Preview asset '%s' has a bad cookedHash: %s"), *Asset.Id, *HashError);
				return false;
			}
			if (!MantlePreviewLaunch::IsBundleRelativePath(File))
			{
				OutError = FString::Printf(TEXT("Preview asset '%s' file '%s' must be a relative path inside the bundle folder."), *Asset.Id, *File);
				return false;
			}
			Asset.File = Result.BundleDirectory / File;
			Asset.CookedBytes = static_cast<int64>(Bytes);
			if (Bytes != static_cast<double>(Asset.CookedBytes) || Asset.CookedBytes <= 0 || Asset.CookedBytes > MantlePreviewLimits::MaxAssetBytes)
			{
				OutError = FString::Printf(TEXT("Preview asset '%s' cookedBytes must be a whole number from 1 to %lld."), *Asset.Id, MantlePreviewLimits::MaxAssetBytes);
				return false;
			}
			const int64 FileBytes = IFileManager::Get().FileSize(*Asset.File);
			if (FileBytes != Asset.CookedBytes)
			{
				OutError = FString::Printf(TEXT("Preview asset '%s' file '%s' is %s, not the %lld bytes its manifest says."), *Asset.Id, *File,
					FileBytes < 0 ? TEXT("missing") : *FString::Printf(TEXT("%lld bytes"), FileBytes), Asset.CookedBytes);
				return false;
			}
			TotalBytes += Asset.CookedBytes;
			if (TotalBytes > MantlePreviewLimits::MaxTotalAssetBytes)
			{
				OutError = FString::Printf(TEXT("Preview assets exceed %lld MiB in all."), MantlePreviewLimits::MaxTotalAssetBytes >> 20);
				return false;
			}
			if (Fields.HasField(TEXT("role")) && (!Fields.TryGetStringField(TEXT("role"), Asset.Role) || Asset.Role.Len() > 64))
			{
				OutError = FString::Printf(TEXT("Preview asset '%s' has a bad role."), *Asset.Id);
				return false;
			}
			if (Fields.HasField(TEXT("textures")))
			{
				const TSharedPtr<FJsonObject>* Textures = nullptr;
				if (!Is(Asset.Kind, TEXT("material")) || !Fields.TryGetObjectField(TEXT("textures"), Textures) || !Textures->IsValid())
				{
					OutError = FString::Printf(TEXT("Preview asset '%s' may name textures only when it is a material, and as an object."), *Asset.Id);
					return false;
				}
				for (const auto& Slot : (*Textures)->Values)
				{
					const FString SlotName(Slot.Key.ToView());
					FString TextureId;
					if (SlotName.IsEmpty() || !Slot.Value.IsValid() || !Slot.Value->TryGetString(TextureId)
						|| !FMantlePlaceAssets::IsUploadedId(TextureId))
					{
						OutError = FString::Printf(TEXT("Preview material '%s' names a bad texture in slot '%s'."), *Asset.Id, *SlotName);
						return false;
					}
					Asset.Textures.Add(SlotName, TextureId);
				}
			}
			Result.Assets.Add(MoveTemp(Asset));
		}

		// A material's textures are other entries of this bundle: the client never goes looking for them elsewhere.
		for (const FMantlePreviewAsset& Asset : Result.Assets)
		{
			for (const TPair<FString, FString>& Texture : Asset.Textures)
			{
				if (!Ids.Contains(Texture.Value) || Texture.Value.Equals(Asset.Id, ESearchCase::CaseSensitive))
				{
					OutError = FString::Printf(TEXT("Preview material '%s' names texture '%s' in slot '%s', which is not another asset of this bundle."),
						*Asset.Id, *Texture.Value, *Texture.Key);
					return false;
				}
			}
		}
		return true;
	}

	/** manifest "weapons": each file is read as text; its local names must map to assets this bundle lists. */
	bool LoadWeapons(const TArray<TSharedPtr<FJsonValue>>& Entries, FMantlePreviewManifest& Result, FString& OutError)
	{
		if (Entries.Num() > MantlePreviewLimits::MaxWeapons)
		{
			OutError = FString::Printf(TEXT("The preview manifest lists more than %d weapons."), MantlePreviewLimits::MaxWeapons);
			return false;
		}
		TSet<FString> SeenFiles;
		for (const TSharedPtr<FJsonValue>& Value : Entries)
		{
			const TSharedPtr<FJsonObject>* Entry = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(Entry) || !Entry->IsValid())
			{
				OutError = TEXT("Each preview weapon must be an object.");
				return false;
			}
			const FJsonObject& Fields = **Entry;
			const FString Unknown = FirstUnknownKey(Fields, { TEXT("file"), TEXT("assets"), TEXT("equip"), TEXT("materials") });
			if (!Unknown.IsEmpty())
			{
				OutError = FString::Printf(TEXT("Unknown preview weapon field '%s'."), *Unknown);
				return false;
			}
			FMantlePreviewWeapon Weapon;
			const TSharedPtr<FJsonObject>* Assets = nullptr;
			if (!Fields.TryGetStringField(TEXT("file"), Weapon.File) || !Fields.TryGetObjectField(TEXT("assets"), Assets) || !Assets->IsValid())
			{
				OutError = TEXT("Each preview weapon needs a file and an assets object.");
				return false;
			}
			if (Fields.HasField(TEXT("equip")) && (Fields.Values.FindRef(TEXT("equip"))->Type != EJson::Boolean || !Fields.TryGetBoolField(TEXT("equip"), Weapon.bEquip))) // a string "yes" would read as true
			{
				OutError = FString::Printf(TEXT("Preview weapon '%s' has an equip that is not true or false."), *Weapon.File);
				return false;
			}
			if (Fields.HasField(TEXT("materials")))
			{
				const TArray<TSharedPtr<FJsonValue>>* Slots = nullptr;
				if (!Fields.TryGetArrayField(TEXT("materials"), Slots) || Slots->Num() > 64)
				{
					OutError = FString::Printf(TEXT("Preview weapon '%s' needs materials as an array of at most 64 asset ids."), *Weapon.File);
					return false;
				}
				for (const TSharedPtr<FJsonValue>& Slot : *Slots)
				{
					FString MaterialId;
					const bool bIsString = Slot.IsValid() && Slot->TryGetString(MaterialId);
					const bool bInBundle = Result.Assets.ContainsByPredicate([&MaterialId](const FMantlePreviewAsset& Asset)
						{ return Asset.Id.Equals(MaterialId, ESearchCase::CaseSensitive) && Is(Asset.Kind, TEXT("material")); });
					if (!bIsString || (!MaterialId.IsEmpty() && (!FMantlePlaceAssets::IsUploadedId(MaterialId) || !bInBundle)))
					{
						OutError = FString::Printf(TEXT("Preview weapon '%s' lists a material that is not a material asset of this bundle."), *Weapon.File);
						return false;
					}
					Weapon.Materials.Add(MaterialId);
				}
			}
			if (!MantlePreviewLaunch::IsBundleRelativePath(Weapon.File) || !Weapon.File.EndsWith(TEXT(".weapon.json"), ESearchCase::CaseSensitive))
			{
				OutError = FString::Printf(TEXT("Preview weapon file '%s' must be a relative .weapon.json path inside the bundle folder."), *Weapon.File);
				return false;
			}
			if (SeenFiles.Contains(Weapon.File.ToLower()))
			{
				OutError = FString::Printf(TEXT("Preview weapon '%s' is listed more than once."), *Weapon.File);
				return false;
			}
			SeenFiles.Add(Weapon.File.ToLower());
			for (const auto& Slot : (*Assets)->Values)
			{
				const FString LocalName(Slot.Key.ToView());
				FString AssetId;
				if (LocalName.IsEmpty() || !Slot.Value.IsValid() || !Slot.Value->TryGetString(AssetId) || !FMantlePlaceAssets::IsUploadedId(AssetId))
				{
					OutError = FString::Printf(TEXT("Preview weapon '%s' maps '%s' to something that is not an asset id (ast_u and 32 lowercase hex digits)."),
						*Weapon.File, *LocalName);
					return false;
				}
				if (!Result.Assets.ContainsByPredicate([&AssetId](const FMantlePreviewAsset& Asset) { return Asset.Id.Equals(AssetId, ESearchCase::CaseSensitive); }))
				{
					OutError = FString::Printf(TEXT("Preview weapon '%s' maps '%s' to %s, which is not an asset of this bundle."), *Weapon.File, *LocalName, *AssetId);
					return false;
				}
				Weapon.Assets.Add(LocalName, AssetId);
			}
			const FString FileAbsolute = Result.BundleDirectory / Weapon.File;
			const int64 Bytes = IFileManager::Get().FileSize(*FileAbsolute);
			if (Bytes <= 0 || Bytes > MantlePreviewLimits::MaxWeaponBytes)
			{
				OutError = FString::Printf(TEXT("Preview weapon file '%s' is missing, empty or larger than %lld KiB."), *Weapon.File, MantlePreviewLimits::MaxWeaponBytes >> 10);
				return false;
			}
			if (!FFileHelper::LoadFileToString(Weapon.Text, *FileAbsolute))
			{
				OutError = FString::Printf(TEXT("Could not read preview weapon file '%s'."), *Weapon.File);
				return false;
			}
			Result.Weapons.Add(MoveTemp(Weapon));
		}
		return true;
	}

	/**
	 * manifest "packages": each package's storage name and package.json, read as a stored package's manifest is. Read after
	 * the scripts, so a package the scripts list no file for is refused: its manifest would describe a package that never runs.
	 */
	bool LoadPackages(const TArray<TSharedPtr<FJsonValue>>& Entries, FMantlePreviewManifest& Result, FString& OutError)
	{
		if (Entries.Num() > FMantlePlaceLimits::MaxStorageEntries)
		{
			OutError = FString::Printf(TEXT("The preview manifest lists more than %d script packages."), FMantlePlaceLimits::MaxStorageEntries);
			return false;
		}
		TSet<FString> SeenNames;
		for (const TSharedPtr<FJsonValue>& Value : Entries)
		{
			const TSharedPtr<FJsonObject>* Entry = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(Entry) || !Entry->IsValid())
			{
				OutError = TEXT("Each preview script package must be an object.");
				return false;
			}
			const FJsonObject& Fields = **Entry;
			const FString Unknown = FirstUnknownKey(Fields, { TEXT("name"), TEXT("manifest") });
			if (!Unknown.IsEmpty())
			{
				OutError = FString::Printf(TEXT("Unknown preview script package field '%s'."), *Unknown);
				return false;
			}
			FMantlePreviewPackage Package;
			const TSharedPtr<FJsonObject>* Manifest = nullptr;
			if (!Fields.TryGetStringField(TEXT("name"), Package.Name) || !Fields.TryGetObjectField(TEXT("manifest"), Manifest) || !Manifest->IsValid())
			{
				OutError = TEXT("Each preview script package needs a name and a manifest object.");
				return false;
			}
			if (!FMantlePlaceLimits::IsStorageName(Package.Name))
			{
				OutError = FString::Printf(TEXT("Preview script package '%s' must be named as a storage name (a letter, then up to 63 letters, digits or '_')."), *Package.Name);
				return false;
			}
			// Storage names are matched case-sensitively, but two spellings of one name would confuse a creator; refuse both.
			if (SeenNames.Contains(Package.Name.ToLower()))
			{
				OutError = FString::Printf(TEXT("Preview script package '%s' is listed more than once."), *Package.Name);
				return false;
			}
			SeenNames.Add(Package.Name.ToLower());
			const FString Prefix = FString::Printf(TEXT("Packages/%s/"), *Package.Name);
			if (!Result.Scripts.ContainsByPredicate([&Prefix](const FMantlePreviewScript& Script) { return Script.Path.StartsWith(Prefix, ESearchCase::CaseSensitive); }))
			{
				OutError = FString::Printf(TEXT("Preview script package '%s' has no script files in the bundle (none of its scripts is listed under %s)."), *Package.Name, *Prefix);
				return false;
			}
			Package.Manifest = FMantleStorage::ReadScriptPackageManifest(*Manifest);
			Result.Packages.Add(MoveTemp(Package));
		}
		return true;
	}

	/** manifest "avatar": only resolves the two paths; the avatar ticket applies them. */
	bool LoadAvatar(const TSharedPtr<FJsonObject>& Avatar, FMantlePreviewManifest& Result, FString& OutError)
	{
		FString Descriptor, Builtin;
		if (!Avatar.IsValid() || Avatar->Values.Num() != 2 || !Avatar->TryGetStringField(TEXT("descriptor"), Descriptor)
			|| !Avatar->TryGetStringField(TEXT("builtin"), Builtin))
		{
			OutError = TEXT("The preview manifest's avatar needs exactly a descriptor and a builtin folder.");
			return false;
		}
		if (!MantlePreviewLaunch::IsBundleRelativePath(Descriptor) || !MantlePreviewLaunch::IsBundleRelativePath(Builtin))
		{
			OutError = TEXT("The preview avatar's descriptor and builtin must be relative paths inside the bundle folder.");
			return false;
		}
		const FString DescriptorAbsolute = Result.BundleDirectory / Descriptor;
		const int64 DescriptorBytes = IFileManager::Get().FileSize(*DescriptorAbsolute);
		if (DescriptorBytes <= 0 || DescriptorBytes > MantlePreviewLimits::MaxManifestBytes)
		{
			OutError = FString::Printf(TEXT("The preview avatar descriptor '%s' is missing, empty or larger than %lld KiB."),
				*Descriptor, MantlePreviewLimits::MaxManifestBytes >> 10);
			return false;
		}
		const FString BuiltinAbsolute = Result.BundleDirectory / Builtin;
		if (!IFileManager::Get().DirectoryExists(*BuiltinAbsolute))
		{
			OutError = FString::Printf(TEXT("The preview avatar's builtin folder '%s' is missing."), *Builtin);
			return false;
		}
		Result.AvatarDescriptorFile = DescriptorAbsolute;
		Result.AvatarBuiltinDirectory = BuiltinAbsolute;
		return true;
	}

	struct FManifestCache
	{
		FCriticalSection Lock;
		std::atomic<bool> bLoaded{false};
		FMantlePreviewManifest Manifest;
	};

	FManifestCache& ManifestCache()
	{
		static FManifestCache Cache;
		return Cache;
	}

	constexpr int32 MaxMessageSourceLength = 200;
	/** 500 messages are rewritten every 250 ms, so a message stays short. */
	constexpr int32 MaxMessageTextLength = 1024;
	/** Only the first few refused requests get a message; the count carries the rest, and a flood must not push script output out of the 500. */
	constexpr int32 MaxBlockedRequestMessages = 20;

	const TCHAR* LevelName(FMantlePreviewStatus::ELevel Level)
	{
		switch (Level)
		{
		case FMantlePreviewStatus::ELevel::Warn: return TEXT("warn");
		case FMantlePreviewStatus::ELevel::Error: return TEXT("error");
		default: return TEXT("log");
		}
	}

	const TCHAR* StateName(FMantlePreviewStatus::EState State)
	{
		switch (State)
		{
		case FMantlePreviewStatus::EState::Ready: return TEXT("ready");
		case FMantlePreviewStatus::EState::Failed: return TEXT("failed");
		default: return TEXT("starting");
		}
	}
}

bool MantlePreviewLaunch::IsRequested(const TCHAR* CommandLine)
{
	if (!CommandLine)
	{
		return false;
	}
	FString Token;
	while (FParse::Token(CommandLine, Token, false))
	{
		if (MantlePreviewLaunchPrivate::IsPreviewToken(Token) || MantlePreviewLaunchPrivate::IsCapabilitiesToken(Token))
		{
			return true;
		}
	}
	return false;
}

bool MantlePreviewLaunch::IsCapabilitiesQuery(const TCHAR* CommandLine)
{
	if (!CommandLine)
	{
		return false;
	}
	FString Token;
	while (FParse::Token(CommandLine, Token, false))
	{
		if (MantlePreviewLaunchPrivate::IsCapabilitiesToken(Token))
		{
			return true;
		}
	}
	return false;
}

FString MantlePreviewLaunch::CapabilitiesPathFromCommandLine(const TCHAR* CommandLine)
{
	FString Value;
	if (!CommandLine || !FParse::Value(CommandLine, TEXT("-MantlePreviewCapabilities="), Value, false))
	{
		return FString();
	}
	return Value.TrimStartAndEnd();
}

bool MantlePreviewLaunch::WriteCapabilities(const FString& Path, int32 ScriptApiMajor, int32 ScriptApiMinor)
{
	using namespace MantlePreviewLaunchPrivate;

	if (Path.IsEmpty() || FPaths::IsRelative(Path) || Path.EndsWith(TEXT("/")) || Path.EndsWith(TEXT("\\")))
	{
		return false;
	}
	FString File = FPaths::ConvertRelativePathToFull(Path);
	FPaths::NormalizeFilename(File);
	if (FPaths::GetCleanFilename(File).IsEmpty() || IFileManager::Get().DirectoryExists(*File))
	{
		return false;
	}

	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("previewProtocol"), PreviewProtocol);
	TArray<TSharedPtr<FJsonValue>> Schemas;
	for (const int32 Schema : SupportedManifestSchemas)
	{
		Schemas.Add(MakeShared<FJsonValueNumber>(Schema));
	}
	Root->SetArrayField(TEXT("manifestSchemas"), Schemas);
	const TSharedRef<FJsonObject> ScriptApi = MakeShared<FJsonObject>();
	ScriptApi->SetNumberField(TEXT("major"), ScriptApiMajor);
	ScriptApi->SetNumberField(TEXT("minor"), ScriptApiMinor);
	Root->SetObjectField(TEXT("scriptApi"), ScriptApi);
	// The build the client shows players (Settings > Support); an unstamped developer build has none, so it is named.
	const FString Build = FMantleApiClient::ProjectBuild();
	Root->SetStringField(TEXT("clientVersion"), Build.IsEmpty() ? FString(TEXT("dev")) : Build);
	Root->SetStringField(TEXT("cookTool"), CookToolName);
	// The gated features this build reads, so the editor can cook and export for it; an older client has no such key.
	TArray<TSharedPtr<FJsonValue>> Features;
	for (const FString& Feature : MantleClientFeatures::All())
	{
		Features.Add(MakeShared<FJsonValueString>(Feature));
	}
	Root->SetArrayField(TEXT("features"), Features);

	FString Json;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	if (!FJsonSerializer::Serialize(Root, Writer))
	{
		return false;
	}

	// Nothing is written until the folder exists, and the file appears only by the rename, so a bad path leaves nothing behind.
	if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), /*Tree*/ true))
	{
		return false;
	}
	const FString Temp = File + TEXT(".tmp");
	if (!FFileHelper::SaveStringToFile(Json, *Temp, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		IFileManager::Get().Delete(*Temp, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
		return false;
	}
	bool bReplaced = false;
#if PLATFORM_WINDOWS
	FString NativeTemp = Temp;
	FString NativeFile = File;
	NativeTemp.ReplaceInline(TEXT("/"), TEXT("\\"));
	NativeFile.ReplaceInline(TEXT("/"), TEXT("\\"));
	bReplaced = ::MoveFileExW(*NativeTemp, *NativeFile, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
	bReplaced = IFileManager::Get().Move(*File, *Temp, /*Replace*/ true, /*EvenIfReadOnly*/ true, /*Attributes*/ false, /*bDoNotRetryOrError*/ true);
#endif
	if (!bReplaced)
	{
		IFileManager::Get().Delete(*Temp, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
	}
	return bReplaced;
}

bool MantlePreviewLaunch::IsRequested()
{
	// Asked on every API send and from per-frame paths; the command line does not change after
	// startup, so it is tokenized once per process.
	std::atomic<int32>& Cache = MantlePreviewLaunchPrivate::RequestedCache();
	int32 Cached = Cache.load(std::memory_order_acquire);
	if (Cached < 0)
	{
		// Asked before the command line exists (early module startup): not a preview yet, and not cached.
		if (!FCommandLine::IsInitialized())
		{
			return false;
		}
		Cached = IsRequested(FCommandLine::Get()) ? 1 : 0;
		Cache.store(Cached, std::memory_order_release);
	}
	return Cached != 0;
}

FString MantlePreviewLaunch::ManifestPathFromCommandLine(const TCHAR* CommandLine)
{
	FString Value;
	// A path may hold commas and parentheses, so only whitespace (or the closing quote) ends it.
	if (!CommandLine || !FParse::Value(CommandLine, TEXT("-MantlePreview="), Value, false))
	{
		return FString();
	}
	return Value.TrimStartAndEnd();
}

bool MantlePreviewLaunch::IsBundleRelativePath(const FString& Path)
{
	if (Path.IsEmpty() || Path.Len() > 260 || Path.StartsWith(TEXT("/")))
	{
		return false;
	}
	for (const TCHAR Character : Path)
	{
		// ':' also covers drive letters and NTFS alternate streams; a leading '/' covers UNC.
		if (Character < 32 || Character == TEXT('\\') || Character == TEXT(':') || Character == TEXT('?')
			|| Character == TEXT('*') || Character == TEXT('"') || Character == TEXT('<')
			|| Character == TEXT('>') || Character == TEXT('|'))
		{
			return false;
		}
	}
	TArray<FString> Segments;
	Path.ParseIntoArray(Segments, TEXT("/"), false);
	for (const FString& Segment : Segments)
	{
		// Windows drops a trailing dot or space, so "a." and "a" would name the same file.
		if (Segment.IsEmpty() || Segment == TEXT(".") || Segment == TEXT("..")
			|| Segment.StartsWith(TEXT(" ")) || Segment.EndsWith(TEXT(" ")) || Segment.EndsWith(TEXT(".")))
		{
			return false;
		}
	}
	return true;
}

FMantlePreviewManifest MantlePreviewLaunch::LoadManifest(const FString& ManifestFile)
{
	using namespace MantlePreviewLaunchPrivate;

	FMantlePreviewManifest Result;
	auto Fail = [&Result](const FString& Error)
	{
		Result.bSuccess = false;
		Result.Error = Error;
		Result.Scripts.Reset();
		Result.PlaceFile.Reset();
		Result.UIDocuments.Reset();
		Result.Assets.Reset();
		Result.Weapons.Reset();
		Result.Packages.Reset();
		Result.AvatarDescriptorFile.Reset();
		Result.AvatarBuiltinDirectory.Reset();
		return Result;
	};

	if (ManifestFile.IsEmpty() || FPaths::IsRelative(ManifestFile))
	{
		return Fail(TEXT("-MantlePreview needs the absolute path of the preview manifest, as -MantlePreview=<path>."));
	}
	FString FullManifest = FPaths::ConvertRelativePathToFull(ManifestFile);
	FPaths::NormalizeFilename(FullManifest);
	Result.ManifestFile = FullManifest;
	Result.BundleDirectory = FPaths::GetPath(FullManifest);

	const int64 ManifestBytes = IFileManager::Get().FileSize(*FullManifest);
	if (ManifestBytes <= 0 || ManifestBytes > MantlePreviewLimits::MaxManifestBytes)
	{
		return Fail(FString::Printf(TEXT("The preview manifest is missing, empty or larger than %lld KiB."),
			MantlePreviewLimits::MaxManifestBytes >> 10));
	}
	FString Text;
	TSharedPtr<FJsonObject> Object;
	if (!FFileHelper::LoadFileToString(Text, *FullManifest)
		|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object) || !Object.IsValid())
	{
		return Fail(TEXT("The preview manifest is not a JSON object."));
	}
	double Version = 0.0;
	Object->TryGetNumberField(TEXT("schemaVersion"), Version);
	const bool bSchema2 = Version == 2.0;
	for (const auto& Field : Object->Values)
	{
		const FString Key(Field.Key.ToView());
		const bool bKnown = Is(Key, TEXT("schemaVersion")) || Is(Key, TEXT("placeFile")) || Is(Key, TEXT("scripts"))
			|| (bSchema2 && (Is(Key, TEXT("uiDocuments")) || Is(Key, TEXT("assets")) || Is(Key, TEXT("avatar")) || Is(Key, TEXT("weapons"))
				|| Is(Key, TEXT("packages"))));
		if (!bKnown)
		{
			return Fail(FString::Printf(TEXT("Unknown preview manifest field '%s'."), *Key));
		}
	}

	if (Version != 1.0 && !bSchema2)
	{
		return Fail(TEXT("The preview manifest's schemaVersion must be 1 or 2."));
	}
	Result.SchemaVersion = bSchema2 ? 2 : 1;

	FString PlaceFile;
	if (!Object->TryGetStringField(TEXT("placeFile"), PlaceFile) || !IsBundleRelativePath(PlaceFile)
		|| !Is(FPaths::GetExtension(PlaceFile), TEXT("json")))
	{
		return Fail(TEXT("The preview manifest's placeFile must be a relative .json path inside the bundle folder."));
	}
	const FString PlaceAbsolute = Result.BundleDirectory / PlaceFile;
	const int64 PlaceBytes = IFileManager::Get().FileSize(*PlaceAbsolute);
	if (PlaceBytes <= 0 || PlaceBytes > MantlePreviewLimits::MaxPlaceFileBytes)
	{
		return Fail(FString::Printf(TEXT("The preview place file '%s' is missing, empty or larger than %lld MiB."),
			*PlaceFile, MantlePreviewLimits::MaxPlaceFileBytes >> 20));
	}

	const TArray<TSharedPtr<FJsonValue>>* Scripts = nullptr;
	if (!Object->TryGetArrayField(TEXT("scripts"), Scripts))
	{
		return Fail(TEXT("The preview manifest needs a scripts array (it may be empty)."));
	}
	if (Scripts->Num() > MantlePreviewLimits::MaxScripts)
	{
		return Fail(FString::Printf(TEXT("The preview manifest lists more than %d scripts."), MantlePreviewLimits::MaxScripts));
	}

	TSet<FString> SeenPaths;
	int64 TotalBytes = 0;
	int64 TotalMapBytes = 0;
	for (const TSharedPtr<FJsonValue>& Value : *Scripts)
	{
		const TSharedPtr<FJsonObject>* Entry = nullptr;
		FString Path, Side, File, SourceMapFile;
		// Schema 2 may add a sourceMap to an entry; schema 1 entries stay exactly path, side and file.
		const bool bHasSourceMap = bSchema2 && Value.IsValid() && Value->TryGetObject(Entry) && Entry->IsValid()
			&& (*Entry)->HasField(TEXT("sourceMap"));
		if (!Value.IsValid() || !Value->TryGetObject(Entry) || !Entry->IsValid()
			|| (*Entry)->Values.Num() != (bHasSourceMap ? 4 : 3)
			|| !(*Entry)->TryGetStringField(TEXT("path"), Path) || !(*Entry)->TryGetStringField(TEXT("side"), Side)
			|| !(*Entry)->TryGetStringField(TEXT("file"), File)
			|| (bHasSourceMap && !(*Entry)->TryGetStringField(TEXT("sourceMap"), SourceMapFile)))
		{
			return Fail(TEXT("Each preview script needs exactly a path, a side and a file."));
		}
		// A script package's file sits at Packages/<storage name>/<Server|Client|Shared>/...: the script-path
		// rules apply to the part inside the package, and the whole path may be the prefix longer.
		FString TopFolder;
		FString PathInsidePackage = Path;
		int32 MaxPathLength = FMantlePlaceLimits::MaxScriptPathLength;
		if (Path.StartsWith(TEXT("Packages/"), ESearchCase::CaseSensitive))
		{
			const FString Rest = Path.RightChop(9);
			int32 Slash = INDEX_NONE;
			if (!Rest.FindChar(TEXT('/'), Slash) || !FMantlePlaceLimits::IsStorageName(Rest.Left(Slash)))
			{
				return Fail(FString::Printf(TEXT("Preview script path '%s' must be Packages/<name>/ followed by Server/, Client/ or Shared/ and a relative file path."), *Path));
			}
			PathInsidePackage = Rest.RightChop(Slash + 1);
			MaxPathLength = 9 + 64 + 1 + FMantlePlaceLimits::MaxScriptPathLength; // "Packages/" + a storage name (StorageNamePattern) + '/'
		}
		if (!IsBundleRelativePath(Path) || Path.Len() > MaxPathLength
			|| !FMantlePlaceLimits::IsScriptPath(PathInsidePackage, &TopFolder))
		{
			return Fail(FString::Printf(TEXT("Preview script path '%s' must be Server/, Client/ or Shared/ (or Packages/<name>/ and one of those) followed by a relative file path."), *Path));
		}
		// The same file listed twice, or two spellings of one path that differ only in case, would hide one of them.
		if (SeenPaths.Contains(Path.ToLower()))
		{
			return Fail(FString::Printf(TEXT("Preview script '%s' is listed more than once."), *Path));
		}
		SeenPaths.Add(Path.ToLower());

		FMantlePreviewScript Script;
		if (Is(Side, TEXT("server"))) Script.Side = FMantlePreviewScript::ESide::Server;
		else if (Is(Side, TEXT("client"))) Script.Side = FMantlePreviewScript::ESide::Client;
		else if (Is(Side, TEXT("shared"))) Script.Side = FMantlePreviewScript::ESide::Shared;
		else return Fail(FString::Printf(TEXT("Preview script '%s' has side '%s'; it must be server, client or shared."), *Path, *Side));
		const TCHAR* ExpectedFolder = Script.Side == FMantlePreviewScript::ESide::Server ? TEXT("Server")
			: Script.Side == FMantlePreviewScript::ESide::Client ? TEXT("Client") : TEXT("Shared");
		if (!Is(TopFolder, ExpectedFolder))
		{
			return Fail(FString::Printf(TEXT("Preview script '%s' says side '%s' but sits under %s/."), *Path, *Side, *TopFolder));
		}

		Script.Extension = FPaths::GetExtension(Path);
		if (!IsScriptExtension(Script.Extension))
		{
			return Fail(FString::Printf(TEXT("Preview script '%s' must be a .luau, .lua, .js, .ts or .py file."), *Path));
		}

		if (!IsBundleRelativePath(File))
		{
			return Fail(FString::Printf(TEXT("Preview script file '%s' must be a relative path inside the bundle folder."), *File));
		}
		const FString FileAbsolute = Result.BundleDirectory / File;
		const int64 Bytes = IFileManager::Get().FileSize(*FileAbsolute);
		if (Bytes < 0)
		{
			return Fail(FString::Printf(TEXT("Preview script file '%s' is missing."), *File));
		}
		TotalBytes += Bytes;
		if (Bytes > MantlePreviewLimits::MaxScriptBytes || TotalBytes > MantlePreviewLimits::MaxTotalScriptBytes)
		{
			return Fail(FString::Printf(TEXT("Preview scripts exceed the limits (%lld KiB a file, %lld MiB in all)."),
				MantlePreviewLimits::MaxScriptBytes >> 10, MantlePreviewLimits::MaxTotalScriptBytes >> 20));
		}
		if (!FFileHelper::LoadFileToString(Script.Source, *FileAbsolute))
		{
			return Fail(FString::Printf(TEXT("Could not read preview script file '%s'."), *File));
		}
		if (bHasSourceMap)
		{
			if (!Is(Script.Extension, TEXT("ts")))
			{
				return Fail(FString::Printf(TEXT("Preview script '%s' has a sourceMap but is not a .ts file."), *Path));
			}
			if (!IsBundleRelativePath(SourceMapFile))
			{
				return Fail(FString::Printf(TEXT("Preview source map '%s' must be a relative path inside the bundle folder."), *SourceMapFile));
			}
			const FString MapAbsolute = Result.BundleDirectory / SourceMapFile;
			const int64 MapBytes = IFileManager::Get().FileSize(*MapAbsolute);
			TotalMapBytes += FMath::Max<int64>(MapBytes, 0);
			if (MapBytes <= 0 || MapBytes > MantlePreviewLimits::MaxScriptBytes || TotalMapBytes > MantlePreviewLimits::MaxTotalScriptBytes)
			{
				return Fail(FString::Printf(TEXT("Preview source map '%s' is missing, empty or over the limits (%lld KiB a file, %lld MiB in all)."),
					*SourceMapFile, MantlePreviewLimits::MaxScriptBytes >> 10, MantlePreviewLimits::MaxTotalScriptBytes >> 20));
			}
			if (!FFileHelper::LoadFileToString(Script.SourceMap, *MapAbsolute))
			{
				return Fail(FString::Printf(TEXT("Could not read preview source map '%s'."), *SourceMapFile));
			}
		}
		Script.Path = Path;
		Result.Scripts.Add(MoveTemp(Script));
	}

	if (bSchema2)
	{
		FString SectionError;
		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		if (!ReadSectionArray(*Object, TEXT("uiDocuments"), Entries, SectionError)
			|| (Entries && !LoadUIDocuments(*Entries, Result, SectionError))
			|| !ReadSectionArray(*Object, TEXT("assets"), Entries, SectionError)
			|| (Entries && !LoadAssets(*Entries, Result, SectionError))
			|| !ReadSectionArray(*Object, TEXT("weapons"), Entries, SectionError)
			|| (Entries && !LoadWeapons(*Entries, Result, SectionError))
			|| !ReadSectionArray(*Object, TEXT("packages"), Entries, SectionError)
			|| (Entries && !LoadPackages(*Entries, Result, SectionError)))
		{
			return Fail(SectionError);
		}
		if (Object->HasField(TEXT("avatar")))
		{
			const TSharedPtr<FJsonObject>* Avatar = nullptr;
			if (!Object->TryGetObjectField(TEXT("avatar"), Avatar) || !LoadAvatar(*Avatar, Result, SectionError))
			{
				return Fail(SectionError.IsEmpty() ? FString(TEXT("The preview manifest's avatar must be an object.")) : SectionError);
			}
		}
	}

	Result.PlaceFile = PlaceAbsolute;
	Result.bSuccess = true;
	return Result;
}

const FMantlePreviewManifest& MantlePreviewLaunch::ForThisProcess()
{
	using namespace MantlePreviewLaunchPrivate;

	FManifestCache& Cache = ManifestCache();
	if (Cache.bLoaded.load(std::memory_order_acquire))
	{
		return Cache.Manifest;
	}
	FScopeLock Guard(&Cache.Lock);
	if (!Cache.bLoaded.load(std::memory_order_relaxed))
	{
		if (!IsRequested())
		{
			Cache.Manifest = FMantlePreviewManifest();
			Cache.Manifest.Error = TEXT("This process was not launched with -MantlePreview.");
		}
		else
		{
			const FString Path = ManifestPathFromCommandLine(FCommandLine::Get());
			Cache.Manifest = LoadManifest(Path);
			// The status file sits beside the manifest, so a manifest that can't be located has nowhere to report.
			if (!Path.IsEmpty() && !FPaths::IsRelative(Path))
			{
				FMantlePreviewStatus::Get().Begin(Path);
			}
			if (!Cache.Manifest.bSuccess)
			{
				FMantlePreviewStatus::Get().Fail(Cache.Manifest.Error);
			}
		}
		Cache.bLoaded.store(true, std::memory_order_release);
	}
	return Cache.Manifest;
}

FMantlePreviewStatus& FMantlePreviewStatus::Get()
{
	// Never destroyed: requests are refused and counted until the very end of the process.
	static FMantlePreviewStatus* Instance = [] { FMantlePreviewStatus* Status = new FMantlePreviewStatus(); Status->StartSeconds = FPlatformTime::Seconds(); return Status; }();
	return *Instance;
}

void FMantlePreviewStatus::Begin(const FString& ManifestFile)
{
	bool bRegister = false;
	{
		FScopeLock Guard(&StateLock);
		FString Full = FPaths::ConvertRelativePathToFull(ManifestFile);
		FPaths::NormalizeFilename(Full);
		StatusFile = FPaths::GetPath(Full) / TEXT("preview-status.json");
		TempFile = StatusFile + TEXT(".tmp");
		bRegister = !bHooksRegistered;
		bHooksRegistered = true;
	}
	if (bRegister)
	{
		// The ticker writes whatever the throttle held back; the exit hook writes the last of it.
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([this](float)
		{
			bool bPending = false;
			{
				FScopeLock Inner(&StateLock);
				bPending = bDirty;
			}
			if (bPending)
			{
				WriteNow();
			}
			return true;
		}), static_cast<float>(MinWriteIntervalSeconds));
		FCoreDelegates::OnExit.AddLambda([this] { Flush(); });
	}
	Touch(true);
}

void FMantlePreviewStatus::MarkReadyStage(bool& bStage)
{
	bool bBecameReady = false;
	{
		FScopeLock Guard(&StateLock);
		bStage = true;
		if (State != EState::Failed && bGeometry && bScripts && bSpawn)
		{
			bBecameReady = State != EState::Ready;
			State = EState::Ready;
		}
	}
	Touch(true);
	if (bBecameReady)
	{
		OnReady.Broadcast();
	}
}

void FMantlePreviewStatus::MarkGeometryReady() { MarkReadyStage(bGeometry); }
void FMantlePreviewStatus::MarkScriptsReady() { MarkReadyStage(bScripts); }
void FMantlePreviewStatus::MarkSpawnReady() { MarkReadyStage(bSpawn); }

void FMantlePreviewStatus::Fail(const FString& Error)
{
	{
		FScopeLock Guard(&StateLock);
		State = EState::Failed;
		ErrorText = Error;
	}
	// The status file is rewritten by the next launch of the same bundle; the log keeps why this one stopped.
	UE_LOG(LogMantle, Error, TEXT("MANTLE_PREVIEW_FAILED %s"), *Error);
	AddMessage(ELevel::Error, TEXT("preview"), Error);
	Touch(true);
}

void FMantlePreviewStatus::AddMessage(ELevel Level, const FString& Source, const FString& Text)
{
	{
		FScopeLock Guard(&StateLock);
		FMessage Message;
		Message.Seconds = FPlatformTime::Seconds() - StartSeconds;
		Message.Level = Level;
		Message.Source = Source.Left(MantlePreviewLaunchPrivate::MaxMessageSourceLength);
		Message.Text = Text.Left(MantlePreviewLaunchPrivate::MaxMessageTextLength);
		Messages.Add(MoveTemp(Message));
		if (Messages.Num() > MaxMessages)
		{
			Messages.RemoveAt(0, Messages.Num() - MaxMessages, EAllowShrinking::No);
		}
	}
	Touch(false);
}

void FMantlePreviewStatus::NoteBlockedRequest(const FString& What)
{
	bool bMessage = false;
	{
		FScopeLock Guard(&StateLock);
		bMessage = ++BlockedRequests <= MantlePreviewLaunchPrivate::MaxBlockedRequestMessages;
	}
	if (bMessage)
	{
		AddMessage(ELevel::Warn, TEXT("offline"), TEXT("Blocked request: ") + What);
	}
	else
	{
		Touch(false);
	}
}


FString FMantlePreviewStatus::ToJson() const
{
	using namespace MantlePreviewLaunchPrivate;

	FScopeLock Guard(&StateLock);
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("protocol"), 1);
	Root->SetStringField(TEXT("state"), StateName(State));
	Root->SetStringField(TEXT("error"), ErrorText);
	const TSharedRef<FJsonObject> Ready = MakeShared<FJsonObject>();
	Ready->SetBoolField(TEXT("geometry"), bGeometry);
	Ready->SetBoolField(TEXT("scripts"), bScripts);
	Ready->SetBoolField(TEXT("spawn"), bSpawn);
	Root->SetObjectField(TEXT("ready"), Ready);
	TArray<TSharedPtr<FJsonValue>> MessageValues;
	MessageValues.Reserve(Messages.Num());
	for (const FMessage& Message : Messages)
	{
		const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetNumberField(TEXT("t"), Message.Seconds);
		Entry->SetStringField(TEXT("level"), LevelName(Message.Level));
		Entry->SetStringField(TEXT("source"), Message.Source);
		Entry->SetStringField(TEXT("text"), Message.Text);
		MessageValues.Add(MakeShared<FJsonValueObject>(Entry));
	}
	Root->SetArrayField(TEXT("messages"), MessageValues);
	Root->SetNumberField(TEXT("blockedRequests"), BlockedRequests);

	FString Out;
	const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(Root, Writer);
	return Out;
}

void FMantlePreviewStatus::Touch(bool bForce)
{
	bool bWrite = false;
	{
		FScopeLock Guard(&StateLock);
		++Sequence;
		bDirty = true;
		bWrite = !StatusFile.IsEmpty()
			&& (bForce || FPlatformTime::Seconds() - LastWriteSeconds >= MinWriteIntervalSeconds);
	}
	if (bWrite)
	{
		WriteNow();
	}
}

void FMantlePreviewStatus::Flush()
{
	WriteNow(/*bFinal*/ true);
}

void FMantlePreviewStatus::WriteNow(bool bFinal)
{
	FString Json;
	FString File;
	FString Temp;
	uint64 Snapshot = 0;
	{
		FScopeLock Guard(&StateLock);
		if (StatusFile.IsEmpty())
		{
			return;
		}
		File = StatusFile;
		Temp = TempFile;
		Snapshot = Sequence;
		bDirty = false;
		LastWriteSeconds = FPlatformTime::Seconds();
	}
	Json = ToJson();

	bool bWritten = false;
	{
		// Writers queue here; a snapshot older than one already on disk is not worth writing.
		FScopeLock Guard(&WriteLock);
		if (Snapshot <= WrittenSequence)
		{
			return;
		}
		if (FFileHelper::SaveStringToFile(Json, *Temp, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			// The exit flush has no later tick to retry on, so it tries a few times when a reader holds the file.
			const int32 Attempts = bFinal ? 5 : 1;
			for (int32 Attempt = 0; Attempt < Attempts && !bWritten; ++Attempt)
			{
				if (Attempt > 0)
				{
					FPlatformProcess::Sleep(0.02f);
				}
#if PLATFORM_WINDOWS
				// IFileManager::Move deletes the destination and then moves without replace-existing, which a reader
				// holding the file open defeats; this replaces it in one call.
				FString NativeTemp = Temp;
				FString NativeFile = File;
				NativeTemp.ReplaceInline(TEXT("/"), TEXT("\\"));
				NativeFile.ReplaceInline(TEXT("/"), TEXT("\\"));
				bWritten = ::MoveFileExW(*NativeTemp, *NativeFile, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
				bWritten = IFileManager::Get().Move(*File, *Temp, /*Replace*/ true, /*EvenIfReadOnly*/ true, /*Attributes*/ false, /*bDoNotRetryOrError*/ true);
#endif
			}
			if (!bWritten)
			{
				IFileManager::Get().Delete(*Temp, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
			}
		}
		if (bWritten)
		{
			WrittenSequence = Snapshot;
		}
	}
	if (!bWritten)
	{
		// Leave it for the ticker to retry; there is no log to tell anyone in Shipping.
		FScopeLock Guard(&StateLock);
		bDirty = true;
	}
}
