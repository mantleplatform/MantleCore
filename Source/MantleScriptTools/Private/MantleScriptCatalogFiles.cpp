// Copyright Joshua Eger. All Rights Reserved.
// Standalone script catalog behind IMantleScriptCatalogSource. require() paths are matched
// case-sensitively, so a path that resolves on a creator's Windows PC resolves identically on the
// Linux dedicated server.

#include "MantleScriptCatalogFiles.h"
#include "MantleConnection.h"
#include "MantlePlaceLimits.h"
#include "MantlePlaceStorage.h"
#include "MantleScriptTools.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

EMantleScriptCatalogLanguage MantleScriptLanguageFromExtension(const FString& Extension)
{
	if (Extension.Equals(TEXT("luau"), ESearchCase::CaseSensitive) ||
		Extension.Equals(TEXT("lua"), ESearchCase::CaseSensitive))
	{
		return EMantleScriptCatalogLanguage::Luau;
	}
	// .js and .ts are two catalogue languages (so Script.* and error logs still name the
	// file's real extension) but share one QuickJS runtime -- see
	// UMantleScriptWorldSubsystem::EnsureLanguageRuntime.
	if (Extension.Equals(TEXT("js"), ESearchCase::CaseSensitive))
	{
		return EMantleScriptCatalogLanguage::JavaScript;
	}
	if (Extension.Equals(TEXT("ts"), ESearchCase::CaseSensitive))
	{
		return EMantleScriptCatalogLanguage::TypeScript;
	}
	// .py gets its own catalogue language and its own PocketPy VM per world (unlike
	// JavaScript/TypeScript, which share one QuickJS runtime) -- see
	// UMantleScriptWorldSubsystem::EnsureLanguageRuntime.
	if (Extension.Equals(TEXT("py"), ESearchCase::CaseSensitive))
	{
		return EMantleScriptCatalogLanguage::Python;
	}
	return EMantleScriptCatalogLanguage::Unknown;
}

namespace
{
	/**
	 * Classifies one Scripts/-relative path (already split on '/'). Every comparison here is
	 * case-sensitive by construction (FString::Equals defaults to ESearchCase::IgnoreCase, so
	 * every call below spells out CaseSensitive) -- a folder named "server" or "modules" is not
	 * recognised and is skipped with a warning rather than silently accepted.
	 */
	bool ClassifyPath(const TArray<FString>& Segments, EMantleSide& OutSide, bool& OutModuleOnly)
	{
		if (Segments.Num() < 2)
		{
			return false;
		}
		const FString& Top = Segments[0];
		if (Top.Equals(TEXT("Server"), ESearchCase::CaseSensitive))
		{
			OutSide = EMantleSide::Server;
			OutModuleOnly = false;
		}
		else if (Top.Equals(TEXT("Client"), ESearchCase::CaseSensitive))
		{
			OutSide = EMantleSide::Client;
			OutModuleOnly = false;
		}
		else if (Top.Equals(TEXT("Shared"), ESearchCase::CaseSensitive))
		{
			OutSide = EMantleSide::Both;
			OutModuleOnly = true;
		}
		else
		{
			return false;
		}

		for (int32 Index = 1; Index < Segments.Num() - 1; ++Index)
		{
			if (Segments[Index].Equals(TEXT("Modules"), ESearchCase::CaseSensitive))
			{
				OutModuleOnly = true;
			}
		}
		return true;
	}

	/** The catalogue prefix every script package is mounted under (#838). */
	const TCHAR* const GMantleScriptPackagesPrefix = TEXT("Packages/");
}

FString MantleScriptPackageOf(const FString& CatalogPath)
{
	const FString Prefix(GMantleScriptPackagesPrefix);
	if (!CatalogPath.StartsWith(Prefix, ESearchCase::CaseSensitive))
	{
		return FString();
	}
	const int32 NameStart = Prefix.Len();
	const int32 Slash = CatalogPath.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, NameStart);
	if (Slash <= NameStart || Slash + 1 >= CatalogPath.Len())
	{
		return FString();
	}
	return CatalogPath.Mid(NameStart, Slash - NameStart);
}

bool MantleClassifyScriptCatalogPath(const FString& CatalogPath, EMantleSide& OutSide, bool& bOutModuleOnly)
{
	TArray<FString> Segments;
	CatalogPath.ParseIntoArray(Segments, TEXT("/"));
	if (Segments.Num() > 0 && Segments[0].Equals(TEXT("Packages"), ESearchCase::CaseSensitive))
	{
		// Packages/<name>/<Server|Client|Shared>/<file>: classified by what follows the package's name.
		if (Segments.Num() < 4 || !FMantlePlaceLimits::IsStorageName(Segments[1]))
		{
			return false;
		}
		const TArray<FString> Inner(Segments.GetData() + 2, Segments.Num() - 2);
		return ClassifyPath(Inner, OutSide, bOutModuleOnly);
	}
	return ClassifyPath(Segments, OutSide, bOutModuleOnly);
}

bool FMantleDiskScriptCatalogSource::IsRequestedForLaunch(FString& OutPlaceFilePath)
{
	if (!FMantleConnection::ResolveServerToken().IsEmpty())
	{
		return false;
	}
	return FParse::Value(FCommandLine::Get(), TEXT("MantlePlace="), OutPlaceFilePath) && !OutPlaceFilePath.IsEmpty();
}

FMantleDiskScriptCatalogSource::FMantleDiskScriptCatalogSource(const FString& PlaceFilePath, const bool bIncludePackages)
{
	if (PlaceFilePath.IsEmpty())
	{
		return;
	}
	const FString Candidate = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::GetPath(PlaceFilePath), TEXT("Scripts")));
	if (IFileManager::Get().DirectoryExists(*Candidate))
	{
		ScriptsRoot = Candidate;
	}
	// #838: script packages authored beside the level, mounted as a stored package is.
	const FString PackagesCandidate = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::GetPath(PlaceFilePath), TEXT("Packages")));
	if (bIncludePackages && IFileManager::Get().DirectoryExists(*PackagesCandidate))
	{
		PackagesRoot = PackagesCandidate;
	}
	if (IsAvailable())
	{
		Discover();
	}
}

TArray<FMantleDiskScriptCatalogSource::FDiskScriptFile> FMantleDiskScriptCatalogSource::GatherFiles() const
{
	TArray<FDiskScriptFile> Out;
	auto Gather = [&Out](const FString& Root, const FString& Prefix, const bool bPackage)
	{
		if (Root.IsEmpty())
		{
			return;
		}
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *Root, TEXT("*.*"), true, false);
		Files.Sort();
		for (const FString& File : Files)
		{
			FString Relative = File;
			FPaths::MakePathRelativeTo(Relative, *(Root / TEXT("")));
			Relative.ReplaceInline(TEXT("\\"), TEXT("/"));
			const FString CleanName = FPaths::GetCleanFilename(Relative);
			if (CleanName.IsEmpty() || CleanName.StartsWith(TEXT(".")))
			{
				continue;
			}
			if (MantleScriptLanguageFromExtension(FPaths::GetExtension(Relative)) == EMantleScriptCatalogLanguage::Unknown)
			{
				continue;
			}
			FDiskScriptFile& Found = Out.AddDefaulted_GetRef();
			Found.CatalogPath = Prefix + Relative;
			Found.FullPath = File;
			Found.bPackage = bPackage;
		}
	};
	Gather(ScriptsRoot, FString(), false);
	Gather(PackagesRoot, FString(GMantleScriptPackagesPrefix), true);
	return Out;
}

bool FMantleDiskScriptCatalogSource::ClassifyFile(const FDiskScriptFile& File, EMantleSide& OutSide, bool& bOutModuleOnly)
{
	if (File.bPackage)
	{
		return MantleClassifyScriptCatalogPath(File.CatalogPath, OutSide, bOutModuleOnly);
	}
	// A place's own file never classifies as a package: Scripts/Packages/... is outside Server/, Client/ and Shared/.
	TArray<FString> Segments;
	File.CatalogPath.ParseIntoArray(Segments, TEXT("/"));
	return ClassifyPath(Segments, OutSide, bOutModuleOnly);
}

FString FMantleDiskScriptCatalogSource::FullPathOf(const FString& CatalogPath) const
{
	// Only a Packages/ file classifies with a package name (ClassifyFile), so the name says which folder it came from.
	if (!PackagesRoot.IsEmpty() && !MantleScriptPackageOf(CatalogPath).IsEmpty())
	{
		return PackagesRoot / CatalogPath.RightChop(FCString::Strlen(GMantleScriptPackagesPrefix));
	}
	return ScriptsRoot / CatalogPath;
}

const FMantleScriptPackageManifest* FMantleDiskScriptCatalogSource::FindPackageManifest(const FString& Package) const
{
	const TPair<FString, FMantleScriptPackageManifest>* Found = PackageManifests.FindByPredicate(
		[&Package](const TPair<FString, FMantleScriptPackageManifest>& Pair) { return Pair.Key.Equals(Package, ESearchCase::CaseSensitive); });
	return Found ? &Found->Value : nullptr;
}

void FMantleDiskScriptCatalogSource::Discover()
{
	Entries.Reset();
	PackageManifests.Reset();
	if (!IsAvailable())
	{
		return;
	}

	for (const FDiskScriptFile& File : GatherFiles())
	{
		const FString& Relative = File.CatalogPath;
		const EMantleScriptCatalogLanguage Language = MantleScriptLanguageFromExtension(FPaths::GetExtension(Relative));
		EMantleSide Side = EMantleSide::Both;
		bool bModuleOnly = false;
		if (!ClassifyFile(File, Side, bModuleOnly))
		{
			if (File.bPackage)
			{
				UE_LOG(LogMantleScriptTools, Warning,
					TEXT("%s is not inside Packages/<Name>/Server/, Client/ or Shared/ (case-exact, Name a letter then letters, digits or '_') -- skipped."),
					*Relative);
			}
			else
			{
				UE_LOG(LogMantleScriptTools, Warning,
					TEXT("Scripts/%s is not inside Server/, Client/ or Shared/ (case-exact) -- skipped."),
					*Relative);
			}
			continue;
		}

		FMantleScriptCatalogEntry Entry;
		Entry.Id = NextId++;
		Entry.Path = Relative;
		Entry.Name = FPaths::GetBaseFilename(Relative);
		Entry.Side = Side;
		Entry.Language = Language;
		Entry.bModuleOnly = bModuleOnly;
		Entry.LastModified = IFileManager::Get().GetTimeStamp(*File.FullPath);
		if (!FFileHelper::LoadFileToString(Entry.Source, *File.FullPath))
		{
			UE_LOG(LogMantleScriptTools, Warning, TEXT("Could not read %s%s"), File.bPackage ? TEXT("") : TEXT("Scripts/"), *Relative);
			continue;
		}
		Entry.bGraphAuthored = MantleParseGraphSourceMap(Entry.Source, Entry.GraphSourceMap);
		Entries.Add(MoveTemp(Entry));
	}

	// #838: each package folder's package.json is its manifest, read the way a stored package's manifest is.
	if (!PackagesRoot.IsEmpty())
	{
		TArray<FString> Folders;
		IFileManager::Get().FindFiles(Folders, *(PackagesRoot / TEXT("*")), /*Files*/ false, /*Directories*/ true);
		Folders.Sort();
		for (const FString& Folder : Folders)
		{
			const FString ManifestFile = PackagesRoot / Folder / TEXT("package.json");
			FString ManifestText;
			if (FMantlePlaceLimits::IsStorageName(Folder) && IFileManager::Get().FileExists(*ManifestFile)
				&& FFileHelper::LoadFileToString(ManifestText, *ManifestFile))
			{
				PackageManifests.Emplace(Folder, FMantleStorage::ReadScriptPackageManifestJson(ManifestText));
			}
		}
	}
}

bool FMantleDiskScriptCatalogSource::Poll(TArray<int32>& OutChangedIds, bool& bOutStructureChanged)
{
	OutChangedIds.Reset();
	bOutStructureChanged = false;
	if (!IsAvailable())
	{
		return false;
	}

	int32 SeenCount = 0;
	TArray<int32> ChangedLeaf;
	for (const FDiskScriptFile& File : GatherFiles())
	{
		EMantleSide UnusedSide = EMantleSide::Both;
		bool bUnusedModuleOnly = false;
		if (File.bPackage && !ClassifyFile(File, UnusedSide, bUnusedModuleOnly))
		{
			// #838: a package file Discover skips is never an entry, so it must not read as a structure change on every
			// poll. (A place file outside Server/, Client/ and Shared/ keeps its old behaviour.)
			continue;
		}
		++SeenCount;
		const FString& Relative = File.CatalogPath;

		const FMantleScriptCatalogEntry* Existing = Entries.FindByPredicate(
			[&Relative](const FMantleScriptCatalogEntry& E) { return E.Path.Equals(Relative, ESearchCase::CaseSensitive); });
		if (!Existing)
		{
			bOutStructureChanged = true;
			break;
		}
		const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*File.FullPath);
		if (Stamp != Existing->LastModified)
		{
			if (Existing->bModuleOnly)
			{
				// A module's callers may hold cached results of requiring it; only a full
				// restart is guaranteed to invalidate every one of them.
				bOutStructureChanged = true;
				break;
			}
			ChangedLeaf.Add(Existing->Id);
		}
	}

	if (!bOutStructureChanged && SeenCount != Entries.Num())
	{
		bOutStructureChanged = true;
	}

	if (bOutStructureChanged)
	{
		Discover();
		return true;
	}

	if (ChangedLeaf.Num() == 0)
	{
		return false;
	}

	for (const int32 Id : ChangedLeaf)
	{
		FMantleScriptCatalogEntry* Entry = Entries.FindByPredicate(
			[Id](const FMantleScriptCatalogEntry& E) { return E.Id == Id; });
		if (!Entry)
		{
			continue;
		}
		const FString FullPath = FullPathOf(Entry->Path);
		Entry->LastModified = IFileManager::Get().GetTimeStamp(*FullPath);
		FFileHelper::LoadFileToString(Entry->Source, *FullPath);
		Entry->bGraphAuthored = MantleParseGraphSourceMap(Entry->Source, Entry->GraphSourceMap);
	}
	OutChangedIds = ChangedLeaf;
	return true;
}

namespace
{
	bool PathMatches(const FMantleScriptCatalogEntry& Entry, const FString& Candidate)
	{
		if (Entry.Path.Equals(Candidate, ESearchCase::CaseSensitive))
		{
			return true;
		}
		const FString WithoutExtension = FPaths::GetBaseFilename(Entry.Path, false);
		return WithoutExtension.Equals(Candidate, ESearchCase::CaseSensitive);
	}

	bool PathMatchesIgnoreCase(const FMantleScriptCatalogEntry& Entry, const FString& Candidate)
	{
		if (Entry.Path.Equals(Candidate, ESearchCase::IgnoreCase))
		{
			return true;
		}
		const FString WithoutExtension = FPaths::GetBaseFilename(Entry.Path, false);
		return WithoutExtension.Equals(Candidate, ESearchCase::IgnoreCase);
	}

	/** True when some entry is mounted under Packages/<Package>/ (#838), Package compared case-sensitively. */
	bool MantleRequireCatalogHasPackage(const TArray<FMantleScriptCatalogEntry>& Entries, const FString& Package)
	{
		return Entries.ContainsByPredicate([&Package](const FMantleScriptCatalogEntry& Entry)
		{
			return MantleScriptPackageOf(Entry.Path).Equals(Package, ESearchCase::CaseSensitive);
		});
	}
}

const FMantleScriptCatalogEntry* MantleResolveRequire(
	const TArray<FMantleScriptCatalogEntry>& Entries,
	const FString& FromPath,
	EMantleSide FromSide,
	const FString& Request,
	FString& OutError)
{
	OutError.Reset();
	if (Request.IsEmpty())
	{
		OutError = TEXT("require expects a path, e.g. require(\"Shared/Config\")");
		return nullptr;
	}

	// #838: the script package the caller belongs to (empty for a place's own script). A package resolves inside
	// itself only; see the header comment for the whole rule set.
	const FString FromPackage = MantleScriptPackageOf(FromPath);
	const FString PackageRoot = FromPackage.IsEmpty() ? FString() : FString::Printf(TEXT("Packages/%s/"), *FromPackage);

	// "@<name>/Thing" names a mounted package's Shared/Thing (#838). Any other "@" is an alias prefix that is
	// ignored: "@Shared/Config" means "Shared/Config" when no package is named Shared.
	FString Path = Request;
	FString AliasPackage;
	FString UnknownAliasName;
	if (Path.StartsWith(TEXT("@")))
	{
		Path = Path.RightChop(1);
		int32 Slash = INDEX_NONE;
		if (Path.FindChar(TEXT('/'), Slash) && Slash > 0)
		{
			const FString Name = Path.Left(Slash);
			if (MantleRequireCatalogHasPackage(Entries, Name))
			{
				AliasPackage = Name;
				Path = Path.Mid(Slash + 1);
			}
			else
			{
				UnknownAliasName = Name;
			}
		}
	}

	if (!AliasPackage.IsEmpty())
	{
		if (!FromPackage.IsEmpty() && !AliasPackage.Equals(FromPackage, ESearchCase::CaseSensitive))
		{
			OutError = FString::Printf(
				TEXT("require: \"%s\" is another package's module; a package can only require its own files. Packages talk to each other through Events, State and Net."),
				*Request);
			return nullptr;
		}
		// Only the package's Shared/ folder is reachable this way: it is all a client and a server both receive.
		const FString Candidate = FString::Printf(TEXT("Packages/%s/Shared/%s"), *AliasPackage, *Path);
		for (const FMantleScriptCatalogEntry& Entry : Entries)
		{
			if (PathMatches(Entry, Candidate))
			{
				return &Entry;
			}
		}
		for (const FMantleScriptCatalogEntry& Entry : Entries)
		{
			if (PathMatchesIgnoreCase(Entry, Candidate))
			{
				OutError = FString::Printf(
					TEXT("require: package \"%s\" has no Shared/%s (paths are case-sensitive); did you mean \"%s\"?"),
					*AliasPackage, *Path, *Entry.Path);
				return nullptr;
			}
		}
		OutError = FString::Printf(
			TEXT("require: package \"%s\" has no Shared/%s; \"@%s/<name>\" reaches only the modules in the package's Shared/ folder"),
			*AliasPackage, *Path, *AliasPackage);
		return nullptr;
	}

	TArray<FString> Candidates;
	if (Path.StartsWith(TEXT("./")) || Path.StartsWith(TEXT("../")))
	{
		FString Dir = FPaths::GetPath(FromPath);
		FString Combined = FPaths::Combine(Dir, Path);
		const bool bCollapsed = FPaths::CollapseRelativeDirectories(Combined);
		Combined.ReplaceInline(TEXT("\\"), TEXT("/"));
		if (!FromPackage.IsEmpty() && (!bCollapsed || !Combined.StartsWith(PackageRoot, ESearchCase::CaseSensitive)))
		{
			OutError = FString::Printf(
				TEXT("require: \"%s\" leads outside package %s; a package can only require its own files"), *Request, *FromPackage);
			return nullptr;
		}
		Candidates.Add(Combined);
	}
	else if (!FromPackage.IsEmpty())
	{
		// Inside a package, bare names resolve within it, exactly as a place's own names resolve within Scripts/.
		Candidates.Add(PackageRoot + Path);
		Candidates.Add(FString::Printf(TEXT("%sShared/%s"), *PackageRoot, *Path));
		if (FromSide == EMantleSide::Server)
		{
			Candidates.Add(FString::Printf(TEXT("%sServer/%s"), *PackageRoot, *Path));
		}
		else if (FromSide == EMantleSide::Client)
		{
			Candidates.Add(FString::Printf(TEXT("%sClient/%s"), *PackageRoot, *Path));
		}
	}
	else
	{
		Candidates.Add(Path);
		Candidates.Add(FString::Printf(TEXT("Shared/%s"), *Path));
		if (FromSide == EMantleSide::Server)
		{
			Candidates.Add(FString::Printf(TEXT("Server/%s"), *Path));
		}
		else if (FromSide == EMantleSide::Client)
		{
			Candidates.Add(FString::Printf(TEXT("Client/%s"), *Path));
		}
	}

	for (const FString& Candidate : Candidates)
	{
		for (const FMantleScriptCatalogEntry& Entry : Entries)
		{
			if (PathMatches(Entry, Candidate))
			{
				// #838: a place script reaches a package only through "@<name>/", so only its Shared/ modules.
				const FString TargetPackage = MantleScriptPackageOf(Entry.Path);
				if (FromPackage.IsEmpty() && !TargetPackage.IsEmpty())
				{
					OutError = FString::Printf(
						TEXT("require: \"%s\" is inside package %s; a place script requires a package's Shared/ modules with require(\"@%s/<name>\")"),
						*Entry.Path, *TargetPackage, *TargetPackage);
					return nullptr;
				}
				// A script may require Shared/ modules and its own side's modules only. On a
				// published place a client never receives Server/ sources (and vice versa), so a
				// cross-side require that worked from disk would fail live; refuse it everywhere.
				if (FromSide != EMantleSide::Both && Entry.Side != EMantleSide::Both
					&& !EnumHasAnyFlags(Entry.Side, FromSide))
				{
					const bool bFromServer = FromSide == EMantleSide::Server;
					OutError = FString::Printf(
						TEXT("require: \"%s\" is a %s module; %s scripts can only require Shared/ and %s/ modules. Put code both sides need in Shared/."),
						*Entry.Path, bFromServer ? TEXT("client") : TEXT("server"),
						bFromServer ? TEXT("server") : TEXT("client"), bFromServer ? TEXT("Server") : TEXT("Client"));
					return nullptr;
				}
				return &Entry;
			}
		}
	}

	// #838: nothing inside the package. If the place's own rules would have found one of the place's scripts, say why
	// it is out of reach rather than "not found": Mantle provides no modules of its own for a package to fall back on.
	if (!FromPackage.IsEmpty() && !Path.StartsWith(TEXT("./")) && !Path.StartsWith(TEXT("../")))
	{
		TArray<FString> PlaceCandidates;
		PlaceCandidates.Add(Path);
		PlaceCandidates.Add(FString::Printf(TEXT("Shared/%s"), *Path));
		PlaceCandidates.Add(FString::Printf(TEXT("Server/%s"), *Path));
		PlaceCandidates.Add(FString::Printf(TEXT("Client/%s"), *Path));
		for (const FString& Candidate : PlaceCandidates)
		{
			for (const FMantleScriptCatalogEntry& Entry : Entries)
			{
				if (MantleScriptPackageOf(Entry.Path).IsEmpty() && PathMatches(Entry, Candidate))
				{
					OutError = FString::Printf(
						TEXT("require: \"%s\" is one of the place's own scripts (%s); a package cannot require the place's scripts. Packages and place scripts talk through Events, State and Net."),
						*Request, *Entry.Path);
					return nullptr;
				}
			}
		}
	}

	// No exact-case match. If a case-insensitive one exists, name its real spelling (#129) --
	// this is what tells a creator "it works on your PC but would fail on Linux" instead of a
	// bare "not found".
	for (const FString& Candidate : Candidates)
	{
		for (const FMantleScriptCatalogEntry& Entry : Entries)
		{
			if (PathMatchesIgnoreCase(Entry, Candidate))
			{
				OutError = FString::Printf(
					TEXT("require: no script at \"%s\" (paths are case-sensitive); did you mean \"%s\"?"),
					*Path, *Entry.Path);
				return nullptr;
			}
		}
	}

	if (!UnknownAliasName.IsEmpty())
	{
		OutError = FString::Printf(TEXT("require: no script at \"%s\", and this place stores no script package named \"%s\""),
			*Path, *UnknownAliasName);
		return nullptr;
	}
	OutError = FString::Printf(TEXT("require: no script at \"%s\""), *Path);
	return nullptr;
}
