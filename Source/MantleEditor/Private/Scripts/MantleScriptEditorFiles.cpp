// Copyright Joshua Eger. All Rights Reserved.
#include "MantleScriptEditorFiles.h"

#include "HAL/FileManager.h"
#include "MantleApiManifest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogMantleScriptEditorFiles, Log, All);

namespace MantleScriptEditorFilesPrivate
{
	FString GenerateTsConfig(const FString& TypesDirRelativeToProject)
	{
		return FString::Printf(
			TEXT("{\n")
			TEXT("\t\"compilerOptions\": {\n")
			TEXT("\t\t\"target\": \"ES2020\",\n")
			TEXT("\t\t\"module\": \"ESNext\",\n")
			TEXT("\t\t\"moduleResolution\": \"Bundler\",\n")
			TEXT("\t\t\"strict\": true,\n")
			// Server/Client/Shared scripts freely mix .js and .ts (the host lets a TS script
			// require a JS one and vice versa); allowJs lets tsc infer types straight from a
			// plain .js module instead of flagging every such import as implicit any (TS7016).
			TEXT("\t\t\"allowJs\": true,\n")
			TEXT("\t\t\"noEmit\": true,\n")
			TEXT("\t\t\"skipLibCheck\": true,\n")
			TEXT("\t\t\"paths\": { \"@Shared/*\": [\"Content/*/Scripts/Shared/*\"] }\n")
			TEXT("\t},\n")
			TEXT("\t\"include\": [\"Content/**/Scripts/**/*.ts\", \"%s/mantle.d.ts\"]\n")
			TEXT("}\n"),
			*TypesDirRelativeToProject);
	}
}

FString FMantleScriptEditorFiles::RegenerateTypes()
{
	using namespace MantleScriptEditorFilesPrivate;

	const FString TypesDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("ScriptTypes"));
	const FString SourceDir = FMantleApiManifest::ResourceDirectory();
	IFileManager::Get().MakeDirectory(*TypesDir, /*Tree*/ true);
	for (const TCHAR* Name : { TEXT("mantle.d.luau"), TEXT("mantle.d.ts"), TEXT("mantle.pyi"), TEXT("mantle_api.json") })
	{
		const FString From = SourceDir / Name;
		const FString To = TypesDir / Name;
		if (IFileManager::Get().Copy(*To, *From, /*bReplace*/ true) != COPY_OK)
		{
			UE_LOG(LogMantleScriptEditorFiles, Warning, TEXT("Could not copy %s to %s."), *From, *To);
		}
	}

	const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	const FString TsConfigPath = ProjectDir / TEXT("tsconfig.json");
	if (!FPaths::FileExists(TsConfigPath))
	{
		FString RelativeDir = TypesDir;
		FPaths::MakePathRelativeTo(RelativeDir, *(ProjectDir + TEXT("/")));
		RelativeDir.ReplaceInline(TEXT("\\"), TEXT("/"));
		FFileHelper::SaveStringToFile(GenerateTsConfig(RelativeDir), *TsConfigPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}
	return TypesDir;
}

void FMantleScriptEditorFiles::EnsureLuauRc(const FString& ScriptsRoot)
{
	if (ScriptsRoot.IsEmpty() || !FPaths::DirectoryExists(ScriptsRoot))
	{
		return;
	}
	const FString LuauRc = ScriptsRoot / TEXT(".luaurc");
	if (FPaths::FileExists(LuauRc))
	{
		return;
	}
	FFileHelper::SaveStringToFile(FString(
		TEXT("{\n")
		TEXT("\t\"languageMode\": \"strict\",\n")
		TEXT("\t\"aliases\": {\n")
		TEXT("\t\t\"Shared\": \"./Shared\",\n")
		TEXT("\t\t\"Server\": \"./Server\",\n")
		TEXT("\t\t\"Client\": \"./Client\"\n")
		TEXT("\t}\n")
		TEXT("}\n")),
		*LuauRc, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

void FMantleScriptEditorFiles::EnsureVsCodeSettings()
{
	const FString Settings = FPaths::ProjectDir() / TEXT(".vscode/settings.json");
	if (FPaths::FileExists(Settings))
	{
		return;
	}
	FFileHelper::SaveStringToFile(FString(
		TEXT("{\n")
		TEXT("\t\"luau-lsp.types.definitionFiles\": [\"ScriptTypes/mantle.d.luau\"],\n")
		TEXT("\t\"luau-lsp.platform.type\": \"standard\",\n")
		TEXT("\t\"luau-lsp.sourcemap.enabled\": false,\n")
		TEXT("\t\"files.associations\": { \"*.lua\": \"luau\" }\n")
		TEXT("}\n")),
		*Settings, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

void FMantleScriptEditorFiles::EnsureAll(const FString& ScriptsRoot)
{
	RegenerateTypes();
	EnsureLuauRc(ScriptsRoot);
	EnsureVsCodeSettings();
}
