// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/** The Mantle client Test Locally runs a preview in, and what it told the editor about itself (#563 stage 3). */
struct FMantlePreviewClientInfo
{
	/** Absolute path to the exe to launch: Mantle-Win64-Shipping.exe, or UnrealEditor.exe for a developer. */
	FString Executable;

	/** Arguments that go before the preview's own: {"<.uproject>", "-game"} for the dev editor client, empty for a packaged client. */
	TArray<FString> PrefixArgs;

	/** Absolute path to mantle-cook.exe. Packaged: beside the exe. Dev: MANTLE_COOK_TOOL, or the repo's built one. */
	FString CookTool;

	/** "steam" | "epic" | "setting" | "dev-editor". */
	FString Source;

	/** The .uproject a dev-editor client runs with -game (the open project, or the Preview Client Project). Empty otherwise. */
	FString Project;

	/** From the client's -MantlePreviewCapabilities answer. */
	int32 PreviewProtocol = 0;
	TArray<int32> ManifestSchemas;
	int32 ScriptApiMajor = 0;
	int32 ScriptApiMinor = 0;
	FString ClientVersion;

	/**
	 * The gated features this client reads (MantleClientFeatures, #628), from its answer's "features". An answer without
	 * the key (an older client, such as today's Steam build) reads none.
	 */
	TArray<FString> Features;
};

class FJsonObject;

/**
 * Finds, checks and owns the Mantle client a preview runs in. The editor only ever touches the one process Launch
 * started: it never enumerates, signals or kills any other process (a Mantle the creator started from Steam is theirs).
 * Game thread only.
 */
namespace MantlePreviewClient
{
	/**
	 * Finds the client: UMantleSettings::PreviewClientExecutable if set and valid; else Steam (app 3657410: the Steam path
	 * from HKCU\Software\Valve\Steam, every library in steamapps\libraryfolders.vdf, appmanifest_3657410.acf's installdir);
	 * else Epic (ProgramData\Epic\EpicGamesLauncher\Data\Manifests\*.item whose DisplayName is Mantle's; InstallLocation).
	 * Then runs the client's capabilities query (once per exe and file timestamp per editor session; it can block for up to
	 * 30 s when the client does not answer) and applies the too-old rules. Returns false with a creator-readable OutError
	 * (what was looked for and how to fix it: install Mantle, update it, or pick the exe in the Mantle settings).
	 */
	bool Find(FMantlePreviewClientInfo& OutClient, FString& OutError);

	/**
	 * Starts the client on ManifestFile with -MantlePreview, -UserDir=<Project>/Saved/Mantle/PreviewClient/<level name> (per-place
	 * local data, kept between runs) and -windowed. Stops this editor's previous preview first. The process dies with the
	 * editor (a Windows job object with kill-on-close).
	 */
	bool Launch(const FMantlePreviewClientInfo& Client, const FString& ManifestFile, const FString& LevelName, FString& OutError);

	/**
	 * Reads a client's -MantlePreviewCapabilities answer into Info: protocol, manifest schemas, script API, version and
	 * features. A key the answer lacks leaves Info's value alone, so an answer without "features" means none.
	 */
	void ReadCapabilities(const TSharedPtr<FJsonObject>& Answer, FMantlePreviewClientInfo& InOutInfo);

	/**
	 * The client project to run instead of the open one: -MantlePreviewProject=<path> on CommandLine if present, else the
	 * Preview Client Project setting value. Trimmed; empty when neither is set. A relative path is not resolved here.
	 */
	FString ProjectOverride(const TCHAR* CommandLine, const FString& SettingValue);

	/**
	 * Which exe runs a preview when the Preview Client Project is Project (empty: none). Empty Project: OutExe is Configured
	 * as is. Otherwise Configured must be empty (OutExe becomes this editor's UnrealEditor.exe) or UnrealEditor.exe; any
	 * other exe is an error, since a packaged client cannot run another project.
	 */
	bool ResolveExecutable(const FString& Configured, const FString& Project, FString& OutExe, FString& OutError);

	/**
	 * Fills Executable, PrefixArgs ({"<uproject>", "-game"}), Source ("dev-editor") and Project of OutInfo for UnrealEditor.exe Exe
	 * running ProjectOverride (empty: the open project). False, with OutError, when that project file is missing or is not a
	 * .uproject. Does not look for the cook tool.
	 */
	bool DescribeDevEditor(const FString& Exe, const FString& ProjectOverride, FMantlePreviewClientInfo& OutInfo, FString& OutError);

	/** True while the process Launch started is still running. */
	bool IsRunning();

	/** Ends the process Launch started, and only that one. */
	void Stop();

	/** <Project>/Saved/Mantle/PreviewClient/<the level name made safe for a folder name>, absolute. */
	FString UserDirFor(const FString& LevelName);

	/** Deletes that UserDir (the preview's saved data). Refuses while a preview is running. A level with no data succeeds. */
	bool ResetLocalData(const FString& LevelName, FString& OutError);

	/**
	 * False, with an update message in OutError, when the client's script API is older than the one the place needs (the
	 * exported place.json's scriptApi major.minor). Find does not check this: the place does not exist yet.
	 */
	bool CheckPlaceScriptApi(const FMantlePreviewClientInfo& Client, int32 PlaceMajor, int32 PlaceMinor, FString& OutError);
}
