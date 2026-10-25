// Copyright Joshua Eger. All Rights Reserved.
#include "MantleScriptDiscovery.h"

#include "Graph/MantleScriptGraphAsset.h"
#include "Graph/MantleScriptGraphSerializer.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Editor.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

EMantleScriptEditorLanguage MantleScriptEditorLanguageFromExtension(const FString& Extension)
{
	switch (MantleScriptLanguageFromExtension(Extension))
	{
	case EMantleScriptCatalogLanguage::Luau: return EMantleScriptEditorLanguage::Luau;
	case EMantleScriptCatalogLanguage::JavaScript: return EMantleScriptEditorLanguage::JavaScript;
	case EMantleScriptCatalogLanguage::TypeScript: return EMantleScriptEditorLanguage::TypeScript;
	case EMantleScriptCatalogLanguage::Python: return EMantleScriptEditorLanguage::Python;
	default: break;
	}
	return EMantleScriptEditorLanguage::Unknown;
}

FString FMantleScriptDiscovery::CurrentLevelScriptsRoot()
{
	if (!GEditor)
	{
		return FString();
	}
	const UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return FString();
	}

	// The level's package, without PIE prefixes -- Mantle never runs PIE, but a play-in-new-
	// process editor world context would still carry one if this ever runs from a launched
	// session rather than the Level Editor itself.
	FString Package = World->GetOutermost()->GetName();
	Package = UWorld::RemovePIEPrefix(Package);
	const FString LevelFolder = FPackageName::GetLongPackagePath(Package);
	if (LevelFolder.IsEmpty())
	{
		return FString();
	}

	FString FolderOnDisk;
	if (!FPackageName::TryConvertLongPackageNameToFilename(LevelFolder + TEXT("/"), FolderOnDisk))
	{
		return FString();
	}
	return FPaths::ConvertRelativePathToFull(FolderOnDisk / TEXT("Scripts"));
}

FString FMantleScriptDiscovery::CurrentLevelScriptsPackageRoot()
{
	if (!GEditor)
	{
		return FString();
	}
	const UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return FString();
	}
	FString Package = World->GetOutermost()->GetName();
	Package = UWorld::RemovePIEPrefix(Package);
	const FString LevelFolder = FPackageName::GetLongPackagePath(Package);
	if (LevelFolder.IsEmpty())
	{
		return FString();
	}
	return LevelFolder / TEXT("Scripts");
}

namespace
{
	/** Mirrors MantleScriptCatalog.cpp's anonymous ClassifyPath exactly (case-sensitive
	 *  Server/Client/Shared top folder, "Modules" at any depth). */
	bool ClassifyPath(const TArray<FString>& Segments, EMantleScriptRowSide& OutSide, bool& OutModule)
	{
		if (Segments.Num() < 2)
		{
			return false;
		}
		const FString& Top = Segments[0];
		if (Top.Equals(TEXT("Server"), ESearchCase::CaseSensitive))
		{
			OutSide = EMantleSide::Server;
			OutModule = false;
		}
		else if (Top.Equals(TEXT("Client"), ESearchCase::CaseSensitive))
		{
			OutSide = EMantleSide::Client;
			OutModule = false;
		}
		else if (Top.Equals(TEXT("Shared"), ESearchCase::CaseSensitive))
		{
			OutSide = EMantleSide::Both;
			OutModule = true;
		}
		else
		{
			return false;
		}

		for (int32 Index = 1; Index < Segments.Num() - 1; ++Index)
		{
			if (Segments[Index].Equals(TEXT("Modules"), ESearchCase::CaseSensitive))
			{
				OutModule = true;
			}
		}
		return true;
	}

	/** Produces a normalized file key for comparing registry-derived graph outputs with paths
	 *  returned by FindFilesRecursive. The two APIs do not agree on slash direction or
	 *  absolute-vs-relative form; Windows may also report different path casing. */
	FString NormalizedFileKey(const FString& Filename)
	{
		FString Result = FPaths::ConvertRelativePathToFull(Filename);
		FPaths::NormalizeFilename(Result);
	#if PLATFORM_WINDOWS
		Result.ToLowerInline();
	#endif
		return Result;
	}

	/** Gets a package-relative path without assuming whether the package root has a trailing slash. */
	FString PackageRelativeToScriptsRoot(FString PackageName, FString ScriptsPackageRoot)
	{
		PackageName.ReplaceInline(TEXT("\\"), TEXT("/"));
		ScriptsPackageRoot.ReplaceInline(TEXT("\\"), TEXT("/"));
		while (ScriptsPackageRoot.EndsWith(TEXT("/")))
		{
			ScriptsPackageRoot.LeftChopInline(1, EAllowShrinking::No);
		}
		PackageName.RemoveFromStart(ScriptsPackageRoot + TEXT("/"), ESearchCase::CaseSensitive);
		return PackageName;
	}
}

void FMantleScriptDiscovery::DiscoverAt(const FString& ScriptsRoot, TArray<FMantleScriptRowPtr>& OutRows)
{
	OutRows.Reset();
	if (ScriptsRoot.IsEmpty() || !FPaths::DirectoryExists(ScriptsRoot))
	{
		return;
	}

	// Discover graph assets before text files. A graph's generated .luau sibling is stale
	// implementation output, not a second script for the editor to expose beside its graph row.
	// Keep the asset list for the row pass below so discovery does not query the registry twice.
	TArray<FAssetData> GraphAssets;
	TSet<FString> GraphGeneratedFileKeys;
	FString ScriptsPackageRoot;
	if (FPackageName::TryConvertFilenameToLongPackageName(ScriptsRoot, ScriptsPackageRoot))
	{
		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
		AssetRegistry.GetAssetsByPath(FName(*ScriptsPackageRoot), GraphAssets, /*bRecursive*/ true, /*bIncludeOnlyOnDiskAssets*/ false);
		GraphAssets.RemoveAll([](const FAssetData& Asset)
		{
			return Asset.AssetClassPath != UMantleScriptGraph::StaticClass()->GetClassPathName();
		});
		GraphAssets.Sort([](const FAssetData& A, const FAssetData& B) { return A.PackageName.LexicalLess(B.PackageName); });
		for (const FAssetData& Asset : GraphAssets)
		{
			FString AssetFilename;
			if (FPackageName::TryConvertLongPackageNameToFilename(Asset.PackageName.ToString(), AssetFilename))
			{
				// Asset package filenames have no extension. ChangeExtension deliberately leaves
				// extensionless paths alone, whereas SetExtension creates the generated .luau name.
				GraphGeneratedFileKeys.Add(NormalizedFileKey(FPaths::SetExtension(AssetFilename, TEXT("luau"))));
			}
		}
	}

	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *ScriptsRoot, TEXT("*.*"), true, false);
	Files.Sort();
	// MakePathRelativeTo uses GetPath(InRelativeTo), so give it a child path instead of the
	// directory itself; this ensures the result is relative to ScriptsRoot rather than its parent.
	const FString RelativeBase = ScriptsRoot / TEXT("__mantle_discovery_anchor__");

	for (const FString& File : Files)
	{
		if (GraphGeneratedFileKeys.Contains(NormalizedFileKey(File)))
		{
			continue;
		}

		FString Relative = File;
		FPaths::MakePathRelativeTo(Relative, *RelativeBase);
		Relative.ReplaceInline(TEXT("\\"), TEXT("/"));

		const FString CleanName = FPaths::GetCleanFilename(Relative);
		if (CleanName.IsEmpty() || CleanName.StartsWith(TEXT(".")) || CleanName.Contains(TEXT(".d.")))
		{
			continue; // dotfiles and *.d.* type-definition files are never scripts
		}

		const EMantleScriptEditorLanguage Language = MantleScriptEditorLanguageFromExtension(FPaths::GetExtension(Relative));
		if (Language == EMantleScriptEditorLanguage::Unknown)
		{
			continue;
		}

		TArray<FString> Segments;
		Relative.ParseIntoArray(Segments, TEXT("/"));
		EMantleScriptRowSide Side = EMantleSide::Both;
		bool bModule = false;
		if (!ClassifyPath(Segments, Side, bModule))
		{
			continue; // not inside Server/, Client/ or Shared/ (case-exact)
		}

		FMantleScriptRowPtr Row = MakeShared<FMantleScriptRow>();
		Row->RelativePath = Relative;
		Row->FullPath = File;
		Row->Side = Side;
		Row->Language = Language;
		Row->bModule = bModule;
		Row->Timestamp = IFileManager::Get().GetTimeStamp(*File);
		OutRows.Add(Row);
	}

	// UMantleScriptGraph assets. A graph is a .uasset, not a recognised text extension
	// above, so its row is assembled from the pre-discovered asset list rather than from disk.
	for (const FAssetData& Asset : GraphAssets)
	{
		const FString Relative = PackageRelativeToScriptsRoot(Asset.PackageName.ToString(), ScriptsPackageRoot);

		EMantleSide Side;
		FString Warning;
		FMantleScriptGraphSerializer::ClassifySide(Asset.PackageName.ToString(), Side, Warning); // Side defaults to Server on failure; the row still lists so Check All can explain why

		FMantleScriptRowPtr Row = MakeShared<FMantleScriptRow>();
		Row->RelativePath = Relative;
		Row->FullPath = Asset.GetObjectPathString();
		Row->AssetPackageName = Asset.PackageName.ToString();
		Row->Side = Side;
		Row->Language = EMantleScriptEditorLanguage::Graph;
		Row->bModule = false;
		Row->Timestamp = FDateTime::Now();
		OutRows.Add(Row);
	}
}
