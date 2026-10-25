// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePreviewClient.h"
#include "MantlePreviewShared.h"
#include "MantleRuntime.h"
#include "MantleSettings.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include "Windows/WindowsHWrapper.h"
#endif

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantlePreviewClientPrivate
{
#if PLATFORM_WINDOWS
	/** A string value of the registry, empty when the key or value is missing. */
	FString ClientReadRegistry(bool bLocalMachine, const TCHAR* Key, const TCHAR* Value)
	{
		FString Text;
		if (!FWindowsPlatformMisc::QueryRegKey(bLocalMachine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, Key, Value, Text))
		{
			return FString();
		}
		return Text.TrimStartAndEnd();
	}

	/** One job for the editor process: every client the editor starts joins it, and the OS kills them all when the editor's handle to it closes. */
	void* GClientJob = nullptr;

	bool ClientAssignToKillOnCloseJob(FProcHandle& Process)
	{
		if (!Process.IsValid())
		{
			return false;
		}
		if (!GClientJob)
		{
			HANDLE Job = ::CreateJobObject(nullptr, nullptr);
			if (!Job)
			{
				return false;
			}
			JOBOBJECT_EXTENDED_LIMIT_INFORMATION Limits;
			FMemory::Memzero(&Limits, sizeof(Limits));
			Limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
			if (!::SetInformationJobObject(Job, JobObjectExtendedLimitInformation, &Limits, sizeof(Limits)))
			{
				::CloseHandle(Job);
				return false;
			}
			GClientJob = Job;
		}
		return ::AssignProcessToJobObject(static_cast<HANDLE>(GClientJob), static_cast<HANDLE>(Process.Get())) != 0;
	}
#endif
}

#if PLATFORM_WINDOWS
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace MantlePreviewClientPrivate
{
	const TCHAR* const SteamAppIdText = TEXT("3657410");
	/** The Epic Games Store listing of Mantle, as the launcher's .item manifests name it. */
	const TCHAR* const EpicDisplayName = TEXT("Mantle");
	const TCHAR* const EpicCatalogItemId = TEXT("12b5de41ef134853b63fb10eacd1bfe8");
	const TCHAR* const EpicAppName = TEXT("9e0a5e7ae7fa424dbb395402c659b7e2");
	/** Under an install folder (Steam common\Mantle, Epic InstallLocation). The launcher Mantle.exe sits at the install root. */
	const TCHAR* const ShippingExeRelative = TEXT("Mantle/Binaries/Win64/Mantle-Win64-Shipping.exe");
	const TCHAR* const CookToolName = TEXT("mantle-cook.exe");

	/** How long the capabilities query may take. The dev editor loads a whole project first. */
	constexpr double PackagedQuerySeconds = 30.0;
	constexpr double DevEditorQuerySeconds = 90.0;
	/** After the answer file appears, how long the client gets to exit by itself before it is stopped. */
	constexpr double ExitGraceSeconds = 5.0;

	/** The process this editor started with Launch (invalid when none). */
	FProcHandle GOwnedProcess;
	uint32 GOwnedProcessId = 0;

	struct FCandidate
	{
		FString Exe;
		FString Source;
	};

	struct FCachedQuery
	{
		FDateTime Timestamp;
		FMantlePreviewClientInfo Info;
	};

	/** Successful queries, per exe (and prefix) and file timestamp, for the editor session. */
	TMap<FString, FCachedQuery>& ClientQueryCache()
	{
		static TMap<FString, FCachedQuery> Cache;
		return Cache;
	}

	/** One command-line argument: a value with a space is quoted ("-Name=" + quoted value for a -Name=value argument). */
	FString ClientFormatArg(const FString& Arg)
	{
		if (!Arg.Contains(TEXT(" ")) || Arg.StartsWith(TEXT("\"")))
		{
			return Arg;
		}
		int32 Equals = INDEX_NONE;
		if (Arg.StartsWith(TEXT("-")) && Arg.FindChar(TEXT('='), Equals))
		{
			return Arg.Left(Equals + 1) + MantlePreviewShared::Quoted(Arg.Mid(Equals + 1));
		}
		return MantlePreviewShared::Quoted(Arg);
	}

	FString ClientJoinArgs(const TArray<FString>& Args)
	{
		FString Line;
		for (const FString& Arg : Args)
		{
			Line += (Line.IsEmpty() ? TEXT("") : TEXT(" ")) + ClientFormatArg(Arg);
		}
		return Line;
	}

	bool ClientIsDevEditorExe(const FString& Exe)
	{
		return FPaths::GetCleanFilename(Exe).Equals(TEXT("UnrealEditor.exe"), ESearchCase::IgnoreCase);
	}

	/** The quoted strings of one VDF line, with \\ and \" unescaped. */
	void ClientVdfStrings(const FString& Line, TArray<FString>& OutStrings)
	{
		OutStrings.Reset();
		bool bIn = false;
		FString Current;
		for (int32 Index = 0; Index < Line.Len(); ++Index)
		{
			const TCHAR Character = Line[Index];
			if (!bIn)
			{
				bIn = Character == TEXT('"');
				Current.Reset();
			}
			else if (Character == TEXT('\\') && Index + 1 < Line.Len())
			{
				Current.AppendChar(Line[++Index]);
			}
			else if (Character == TEXT('"'))
			{
				bIn = false;
				OutStrings.Add(Current);
			}
			else
			{
				Current.AppendChar(Character);
			}
		}
	}

	/** Every "Key" "Value" line of a Steam .vdf/.acf file whose key is Key (case-insensitive). */
	TArray<FString> ClientVdfValues(const FString& File, const TCHAR* Key)
	{
		TArray<FString> Values;
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *File))
		{
			return Values;
		}
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines);
		TArray<FString> Strings;
		for (const FString& Line : Lines)
		{
			ClientVdfStrings(Line, Strings);
			if (Strings.Num() == 2 && Strings[0].Equals(Key, ESearchCase::IgnoreCase))
			{
				Values.Add(Strings[1]);
			}
		}
		return Values;
	}

	/** The Steam install's Mantle exe, when Steam has Mantle installed in any of its libraries. */
	FString ClientFindSteamExe(FString& OutNote)
	{
		FString SteamPath;
#if PLATFORM_WINDOWS
		SteamPath = ClientReadRegistry(false, TEXT("Software\\Valve\\Steam"), TEXT("SteamPath"));
		if (SteamPath.IsEmpty())
		{
			SteamPath = ClientReadRegistry(true, TEXT("SOFTWARE\\WOW6432Node\\Valve\\Steam"), TEXT("InstallPath"));
		}
#endif
		if (SteamPath.IsEmpty())
		{
			OutNote = TEXT("Steam (not installed: no Steam path in the registry)");
			return FString();
		}
		SteamPath = FPaths::ConvertRelativePathToFull(SteamPath);

		TArray<FString> Libraries;
		Libraries.Add(SteamPath);
		for (const FString& Path : ClientVdfValues(SteamPath / TEXT("steamapps/libraryfolders.vdf"), TEXT("path")))
		{
			const FString Full = FPaths::ConvertRelativePathToFull(Path);
			if (!Libraries.ContainsByPredicate([&Full](const FString& Known) { return Known.Equals(Full, ESearchCase::IgnoreCase); }))
			{
				Libraries.Add(Full);
			}
		}
		for (const FString& Library : Libraries)
		{
			const FString Manifest = Library / FString::Printf(TEXT("steamapps/appmanifest_%s.acf"), SteamAppIdText);
			for (const FString& InstallDir : ClientVdfValues(Manifest, TEXT("installdir")))
			{
				const FString Exe = Library / TEXT("steamapps/common") / InstallDir / ShippingExeRelative;
				if (IFileManager::Get().FileExists(*Exe))
				{
					return Exe;
				}
			}
		}
		OutNote = FString::Printf(TEXT("Steam (app %s is not installed in any of its %d librar%s)"), SteamAppIdText, Libraries.Num(), Libraries.Num() == 1 ? TEXT("y") : TEXT("ies"));
		return FString();
	}

	/** The Epic Games Store install's Mantle exe, from the launcher's .item manifests. */
	FString ClientFindEpicExe(FString& OutNote)
	{
		FString ProgramData = FPlatformMisc::GetEnvironmentVariable(TEXT("ProgramData"));
		if (ProgramData.IsEmpty())
		{
			ProgramData = TEXT("C:/ProgramData");
		}
		const FString ManifestDir = ProgramData / TEXT("Epic/EpicGamesLauncher/Data/Manifests");
		TArray<FString> Items;
		IFileManager::Get().FindFiles(Items, *(ManifestDir / TEXT("*.item")), /*Files*/ true, /*Directories*/ false);
		for (const FString& Item : Items)
		{
			FString Text;
			TSharedPtr<FJsonObject> Object;
			if (!FFileHelper::LoadFileToString(Text, *(ManifestDir / Item))
				|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object) || !Object.IsValid())
			{
				continue;
			}
			FString DisplayName, CatalogItemId, AppName, InstallLocation;
			Object->TryGetStringField(TEXT("DisplayName"), DisplayName);
			Object->TryGetStringField(TEXT("CatalogItemId"), CatalogItemId);
			Object->TryGetStringField(TEXT("AppName"), AppName);
			Object->TryGetStringField(TEXT("InstallLocation"), InstallLocation);
			const bool bMantle = DisplayName.Equals(EpicDisplayName, ESearchCase::IgnoreCase)
				|| CatalogItemId.Equals(EpicCatalogItemId, ESearchCase::IgnoreCase) || AppName.Equals(EpicAppName, ESearchCase::IgnoreCase);
			if (!bMantle || InstallLocation.IsEmpty())
			{
				continue;
			}
			const FString Exe = FPaths::ConvertRelativePathToFull(InstallLocation) / ShippingExeRelative;
			if (IFileManager::Get().FileExists(*Exe))
			{
				return Exe;
			}
		}
		OutNote = FString::Printf(TEXT("the Epic Games Store (no installed Mantle among the %d app(s) in %s)"), Items.Num(), *MantlePreviewShared::PlatformPath(ManifestDir));
		return FString();
	}

	/** A developer's cook tool: MANTLE_COOK_TOOL, else the newest mantle-cook.exe the repo's packaging left under artifacts/steam. */
	FString ClientFindDevCookTool()
	{
		FString Env = FPlatformMisc::GetEnvironmentVariable(TEXT("MANTLE_COOK_TOOL")).TrimStartAndEnd();
		if (Env.Len() >= 2 && Env.StartsWith(TEXT("\"")) && Env.EndsWith(TEXT("\"")))
		{
			Env = Env.Mid(1, Env.Len() - 2);
		}
		if (!Env.IsEmpty() && IFileManager::Get().FileExists(*FPaths::ConvertRelativePathToFull(Env)))
		{
			return MantlePreviewShared::PlatformPath(Env);
		}

		const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("../artifacts/steam"));
		TArray<FString> Candidates;
		Candidates.Add(Root / TEXT("mantle-cook") / CookToolName);
		TArray<FString> Packages;
		IFileManager::Get().FindFiles(Packages, *(Root / TEXT("*")), /*Files*/ false, /*Directories*/ true);
		for (const FString& Package : Packages)
		{
			Candidates.Add(Root / Package / TEXT("Windows/Mantle/Binaries/Win64") / CookToolName);
		}
		FString Newest;
		FDateTime NewestTime = FDateTime::MinValue();
		for (const FString& Candidate : Candidates)
		{
			if (!IFileManager::Get().FileExists(*Candidate))
			{
				continue;
			}
			const FDateTime Time = IFileManager::Get().GetTimeStamp(*Candidate);
			if (Newest.IsEmpty() || Time > NewestTime)
			{
				Newest = Candidate;
				NewestTime = Time;
			}
		}
		return Newest.IsEmpty() ? FString() : MantlePreviewShared::PlatformPath(Newest);
	}

	FString ClientTooOld(const FString& Source, const FString& Exe, const FString& Why)
	{
		const TCHAR* Where = MantlePreviewShared::SourceSuffix(Source);
		return FString::Printf(TEXT("This Mantle client is too old for Test Locally; update it%s. (%s: %s)"), Where, *MantlePreviewShared::PlatformPath(Exe), *Why);
	}

	/** Fills Executable, PrefixArgs, CookTool and Source for Exe. False, with OutError, when it cannot be used. */
	bool ClientDescribe(const FString& ExeIn, const FString& SourceIn, const FString& ProjectOverride, FMantlePreviewClientInfo& OutInfo, FString& OutError)
	{
		OutInfo = FMantlePreviewClientInfo();
		FString Exe = FPaths::ConvertRelativePathToFull(ExeIn);
		FString Source = SourceIn;

		if (ClientIsDevEditorExe(Exe))
		{
			if (!MantlePreviewClient::DescribeDevEditor(Exe, ProjectOverride, OutInfo, OutError))
			{
				return false;
			}
			OutInfo.CookTool = ClientFindDevCookTool();
			if (OutInfo.CookTool.IsEmpty())
			{
				OutError = TEXT("A dev-editor preview needs a cook tool: set the MANTLE_COOK_TOOL environment variable to mantle-cook.exe, or build one under artifacts/steam (a packaged client's Mantle/Binaries/Win64 folder has it).");
				return false;
			}
			return true;
		}

		// The launcher at the install root runs the game from beside it.
		if (FPaths::GetCleanFilename(Exe).Equals(TEXT("Mantle.exe"), ESearchCase::IgnoreCase))
		{
			const FString Shipping = FPaths::GetPath(Exe) / ShippingExeRelative;
			if (IFileManager::Get().FileExists(*Shipping))
			{
				Exe = Shipping;
			}
		}
		OutInfo.Executable = MantlePreviewShared::PlatformPath(Exe);
		OutInfo.Source = Source;
		const FString CookTool = FPaths::GetPath(Exe) / CookToolName;
		if (!IFileManager::Get().FileExists(*CookTool))
		{
			OutError = ClientTooOld(Source, Exe, FString::Printf(TEXT("%s is missing beside it"), CookToolName));
			return false;
		}
		OutInfo.CookTool = MantlePreviewShared::PlatformPath(CookTool);
		return true;
	}

	void ClientReadCapabilities(const TSharedPtr<FJsonObject>& Object, FMantlePreviewClientInfo& Info)
	{
		double Number = 0.0;
		if (Object->TryGetNumberField(TEXT("previewProtocol"), Number))
		{
			Info.PreviewProtocol = static_cast<int32>(Number);
		}
		const TArray<TSharedPtr<FJsonValue>>* Schemas = nullptr;
		if (Object->TryGetArrayField(TEXT("manifestSchemas"), Schemas) && Schemas)
		{
			for (const TSharedPtr<FJsonValue>& Schema : *Schemas)
			{
				double Value = 0.0;
				if (Schema.IsValid() && Schema->TryGetNumber(Value))
				{
					Info.ManifestSchemas.Add(static_cast<int32>(Value));
				}
			}
		}
		const TSharedPtr<FJsonObject>* ScriptApi = nullptr;
		if (Object->TryGetObjectField(TEXT("scriptApi"), ScriptApi) && ScriptApi && ScriptApi->IsValid())
		{
			if ((*ScriptApi)->TryGetNumberField(TEXT("major"), Number))
			{
				Info.ScriptApiMajor = static_cast<int32>(Number);
			}
			if ((*ScriptApi)->TryGetNumberField(TEXT("minor"), Number))
			{
				Info.ScriptApiMinor = static_cast<int32>(Number);
			}
		}
		Object->TryGetStringField(TEXT("clientVersion"), Info.ClientVersion);
		// #628: absent in an older client's answer, which reads none of them.
		Info.Features.Reset();
		const TArray<TSharedPtr<FJsonValue>>* Features = nullptr;
		if (Object->TryGetArrayField(TEXT("features"), Features) && Features)
		{
			for (const TSharedPtr<FJsonValue>& Feature : *Features)
			{
				FString Name;
				// Strings only: TryGetString would also turn a number into a name.
				if (Feature.IsValid() && Feature->Type == EJson::String && Feature->TryGetString(Name) && !Name.IsEmpty())
				{
					Info.Features.AddUnique(Name);
				}
			}
		}
	}

	/**
	 * Runs the client with -MantlePreviewCapabilities and fills Info's protocol, schemas, script API and version. A client that
	 * exits non-zero, writes nothing in time, or is below protocol 1 / manifest schema 2 is "too old". A client that does not
	 * know the flag starts the game instead, so one that outlives the wait is stopped (only the process started here).
	 */
	bool ClientQueryCapabilities(FMantlePreviewClientInfo& Info, FString& OutError)
	{
		const bool bDevEditor = Info.Source.Equals(TEXT("dev-editor"));
		const FDateTime Timestamp = IFileManager::Get().GetTimeStamp(*Info.Executable);
		const FString CacheKey = Info.Executable.ToLower() + TEXT("|") + (Info.PrefixArgs.Num() > 0 ? Info.PrefixArgs[0].ToLower() : FString());
		if (const FCachedQuery* Cached = ClientQueryCache().Find(CacheKey))
		{
			if (Cached->Timestamp == Timestamp)
			{
				const FString Source = Info.Source;
				const FString CookTool = Info.CookTool;
				Info = Cached->Info;
				Info.Source = Source;
				Info.CookTool = CookTool;
				return true;
			}
		}

		const FString OutFile = MantlePreviewShared::PlatformPath(FPaths::ProjectSavedDir() / TEXT("Mantle/PreviewCapabilities") / (FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".json")));
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutFile), true);
		IFileManager::Get().Delete(*OutFile, false, true, true);

		// A bare -MantlePreview keeps a client that does not know the capabilities flag offline (its game would otherwise sign in and connect).
		TArray<FString> Args = Info.PrefixArgs;
		Args.Add(TEXT("-MantlePreview"));
		// Without -nosplash the engine shows its splash window before the startup modules (where the client answers) run.
		Args.Add(TEXT("-nosplash"));
		Args.Add(TEXT("-MantlePreviewCapabilities=") + OutFile);
		const FString Arguments = ClientJoinArgs(Args);

		uint32 ProcessId = 0;
		FProcHandle Process = FPlatformProcess::CreateProc(*Info.Executable, *Arguments, /*bLaunchDetached*/ false, /*bLaunchHidden*/ true,
			/*bLaunchReallyHidden*/ true, &ProcessId, /*PriorityModifier*/ 0, /*OptionalWorkingDirectory*/ nullptr, /*PipeWriteChild*/ nullptr);
		if (!Process.IsValid())
		{
			OutError = FString::Printf(TEXT("Could not start the Mantle client %s to check it. Pick a different client in the Mantle settings (Preview Client Executable)."), *Info.Executable);
			return false;
		}
#if PLATFORM_WINDOWS
		ClientAssignToKillOnCloseJob(Process);
#endif

		const double Limit = bDevEditor ? DevEditorQuerySeconds : PackagedQuerySeconds;
		const double Started = FPlatformTime::Seconds();
		double FileSeen = 0.0;
		bool bExited = false;
		while (true)
		{
			if (!FPlatformProcess::IsProcRunning(Process))
			{
				bExited = true;
				break;
			}
			const double Now = FPlatformTime::Seconds();
			if (FileSeen == 0.0 && IFileManager::Get().FileExists(*OutFile))
			{
				FileSeen = Now;
			}
			if ((FileSeen > 0.0 && Now - FileSeen > ExitGraceSeconds) || Now - Started > Limit)
			{
				break;
			}
			FPlatformProcess::Sleep(0.05f);
		}
		int32 ExitCode = 0;
		if (bExited)
		{
			FPlatformProcess::GetProcReturnCode(Process, &ExitCode);
		}
		else
		{
			// Ours alone: the handle is the process started above.
			FPlatformProcess::TerminateProc(Process, /*KillTree*/ true);
		}
		FPlatformProcess::CloseProc(Process);

		FString Text;
		TSharedPtr<FJsonObject> Object;
		const bool bAnswered = FFileHelper::LoadFileToString(Text, *OutFile)
			&& FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object) && Object.IsValid();
		IFileManager::Get().Delete(*OutFile, false, true, true);

		if (bExited && ExitCode != 0)
		{
			OutError = ClientTooOld(Info.Source, Info.Executable, FString::Printf(TEXT("it does not answer the compatibility check (exit code %d)"), ExitCode));
			return false;
		}
		if (!bAnswered)
		{
			OutError = ClientTooOld(Info.Source, Info.Executable, bExited ? TEXT("it ran but wrote no compatibility answer")
				: *FString::Printf(TEXT("it wrote no compatibility answer within %d seconds"), static_cast<int32>(Limit)));
			return false;
		}

		FMantlePreviewClientInfo Answer = Info;
		ClientReadCapabilities(Object, Answer);
		if (Answer.PreviewProtocol < 1)
		{
			OutError = ClientTooOld(Info.Source, Info.Executable, FString::Printf(TEXT("it reports preview protocol %d, Test Locally needs 1"), Answer.PreviewProtocol));
			return false;
		}
		if (!Answer.ManifestSchemas.Contains(2))
		{
			OutError = ClientTooOld(Info.Source, Info.Executable, TEXT("it cannot read the preview bundle format (manifest schema 2)"));
			return false;
		}
		Info = Answer;

		FCachedQuery& Entry = ClientQueryCache().FindOrAdd(CacheKey);
		Entry.Timestamp = Timestamp;
		Entry.Info = Info;
		return true;
	}

	/** A folder name from a level name: letters, digits, '-', '_' and '.', nothing that could climb out of the folder. */
	FString ClientSafeName(const FString& LevelName)
	{
		FString Name;
		for (const TCHAR Character : LevelName)
		{
			Name.AppendChar(FChar::IsAlnum(Character) || Character == TEXT('-') || Character == TEXT('_') || Character == TEXT('.') ? Character : TEXT('_'));
		}
		while (Name.StartsWith(TEXT(".")))
		{
			Name.RightChopInline(1);
		}
		while (Name.EndsWith(TEXT(".")))
		{
			Name.LeftChopInline(1);
		}
		Name.LeftInline(80);
		return Name.IsEmpty() ? FString(TEXT("Untitled")) : Name;
	}

	FString ClientUserRoot()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Mantle/PreviewClient"));
	}
}

void MantlePreviewClient::ReadCapabilities(const TSharedPtr<FJsonObject>& Answer, FMantlePreviewClientInfo& InOutInfo)
{
	if (Answer.IsValid())
	{
		MantlePreviewClientPrivate::ClientReadCapabilities(Answer, InOutInfo);
	}
}

FString MantlePreviewClient::ProjectOverride(const TCHAR* CommandLine, const FString& SettingValue)
{
	FString Value;
	if (CommandLine && FParse::Value(CommandLine, TEXT("-MantlePreviewProject="), Value, false) && !Value.TrimStartAndEnd().IsEmpty())
	{
		return Value.TrimStartAndEnd();
	}
	return SettingValue.TrimStartAndEnd();
}

bool MantlePreviewClient::ResolveExecutable(const FString& Configured, const FString& Project, FString& OutExe, FString& OutError)
{
	OutError.Reset();
	if (Project.IsEmpty())
	{
		OutExe = Configured;
		return true;
	}
	if (Configured.IsEmpty())
	{
		OutExe = FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / TEXT("Binaries/Win64/UnrealEditor.exe"));
		return true;
	}
	if (!MantlePreviewClientPrivate::ClientIsDevEditorExe(Configured))
	{
		OutError = FString::Printf(TEXT("A Preview Client Project (%s) is set, but the Preview Client Executable is %s, which cannot run another project. Clear the Preview Client Executable (or set it to UnrealEditor.exe), or clear the Preview Client Project / -MantlePreviewProject."),
			*MantlePreviewShared::PlatformPath(Project), *MantlePreviewShared::PlatformPath(Configured));
		return false;
	}
	OutExe = Configured;
	return true;
}

bool MantlePreviewClient::DescribeDevEditor(const FString& Exe, const FString& ProjectOverride, FMantlePreviewClientInfo& OutInfo, FString& OutError)
{
	OutError.Reset();
	FString Project;
	if (ProjectOverride.IsEmpty())
	{
		Project = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
		if (FPaths::GetProjectFilePath().IsEmpty() || !IFileManager::Get().FileExists(*Project))
		{
			OutError = TEXT("The Preview Client Executable is UnrealEditor.exe, which runs this project with -game, but this editor has no project file to run.");
			return false;
		}
	}
	else
	{
		Project = FPaths::IsRelative(ProjectOverride) ? FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), ProjectOverride) : FPaths::ConvertRelativePathToFull(ProjectOverride);
		if (!IFileManager::Get().FileExists(*Project))
		{
			OutError = FString::Printf(TEXT("The Preview Client Project does not exist: %s. Choose the Mantle client's .uproject (such as mantle/client/Mantle.uproject), or clear it."),
				*MantlePreviewShared::PlatformPath(Project));
			return false;
		}
		if (!FPaths::GetExtension(Project).Equals(TEXT("uproject"), ESearchCase::IgnoreCase))
		{
			OutError = FString::Printf(TEXT("The Preview Client Project must be a .uproject file: %s"), *MantlePreviewShared::PlatformPath(Project));
			return false;
		}
	}
	OutInfo = FMantlePreviewClientInfo();
	OutInfo.Executable = MantlePreviewShared::PlatformPath(FPaths::ConvertRelativePathToFull(Exe));
	OutInfo.Project = MantlePreviewShared::PlatformPath(Project);
	OutInfo.PrefixArgs = { OutInfo.Project, TEXT("-game") };
	OutInfo.Source = TEXT("dev-editor");
	return true;
}

bool MantlePreviewClient::Find(FMantlePreviewClientInfo& OutClient, FString& OutError)
{
	using namespace MantlePreviewClientPrivate;
	OutClient = FMantlePreviewClientInfo();
	OutError.Reset();

	TArray<FCandidate> Candidates;
	TArray<FString> Looked;
	auto AddCandidate = [&Candidates](const FString& Exe, const TCHAR* Source)
	{
		const FString Full = FPaths::ConvertRelativePathToFull(Exe);
		if (!Candidates.ContainsByPredicate([&Full](const FCandidate& Known) { return Known.Exe.Equals(Full, ESearchCase::IgnoreCase); }))
		{
			Candidates.Add({ Full, Source });
		}
	};

	const FString ProjectChoice = ProjectOverride(FCommandLine::Get(), UMantleSettings::Get()->PreviewClientProject.FilePath);
	FString Configured = UMantleSettings::Get()->PreviewClientExecutable.FilePath.TrimStartAndEnd();
	if (!ProjectChoice.IsEmpty())
	{
		// A client project means the engine's editor runs it: an empty executable is this editor's own UnrealEditor.exe.
		FString Resolved;
		if (!ResolveExecutable(Configured, ProjectChoice, Resolved, OutError))
		{
			return false;
		}
		Configured = Resolved;
	}
	if (Configured.IsEmpty())
	{
		Looked.Add(TEXT("the Preview Client Executable setting (not set)"));
	}
	else
	{
		const FString Full = FPaths::IsRelative(Configured) ? FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), Configured) : Configured;
		if (IFileManager::Get().FileExists(*Full))
		{
			AddCandidate(Full, TEXT("setting"));
		}
		else
		{
			// The creator chose this client: say it is gone rather than quietly use another one.
			OutError = FString::Printf(TEXT("The Preview Client Executable chosen in Editor Preferences > Plugins > Mantle does not exist: %s. Choose another, or clear the setting to use the Steam or Epic install."),
				*MantlePreviewShared::PlatformPath(Full));
			return false;
		}
	}

	FString Note;
	const FString SteamExe = ClientFindSteamExe(Note);
	if (SteamExe.IsEmpty())
	{
		Looked.Add(Note);
	}
	else
	{
		AddCandidate(SteamExe, TEXT("steam"));
	}
	const FString EpicExe = ClientFindEpicExe(Note);
	if (EpicExe.IsEmpty())
	{
		Looked.Add(Note);
	}
	else
	{
		AddCandidate(EpicExe, TEXT("epic"));
	}

	// Each candidate in order; the first one that is usable wins, and when none is, every reason is reported.
	TArray<FString> Failures;
	for (const FCandidate& Candidate : Candidates)
	{
		FMantlePreviewClientInfo Info;
		FString Error;
		if (ClientDescribe(Candidate.Exe, Candidate.Source, ProjectChoice, Info, Error) && ClientQueryCapabilities(Info, Error))
		{
			OutClient = Info;
			UE_LOG(LogMantle, Display, TEXT("MANTLE_PREVIEW_CLIENT %s %s protocol=%d scriptApi=%d.%d version=%s"), *Info.Source, *Info.Executable,
				Info.PreviewProtocol, Info.ScriptApiMajor, Info.ScriptApiMinor, *Info.ClientVersion);
			return true;
		}
		if (Candidate.Source.Equals(TEXT("setting"), ESearchCase::CaseSensitive))
		{
			// The creator's own choice failed: report why instead of falling back to Steam or Epic.
			if (!ProjectChoice.IsEmpty())
			{
				OutError = Error + FString::Printf(TEXT(" (This is the Preview Client Project %s, from Editor Preferences > Plugins > Mantle or -MantlePreviewProject. It must be a Mantle client project that answers -MantlePreviewCapabilities, such as mantle/client/Mantle.uproject; choose another, or clear it.)"),
					*MantlePreviewShared::PlatformPath(ProjectChoice));
				return false;
			}
			OutError = Error + TEXT(" (This is the Preview Client Executable chosen in Editor Preferences > Plugins > Mantle; choose another, or clear it to use the Steam or Epic install.)");
			return false;
		}
		Failures.Add(Error);
	}
	if (Failures.Num() > 0)
	{
		OutError = FString::Join(Failures, TEXT("\n"));
		return false;
	}
	OutError = FString::Printf(TEXT("Mantle's client was not found. Looked in: %s. Install Mantle from Steam or the Epic Games Store, or pick Mantle-Win64-Shipping.exe (in the install's Mantle\\Binaries\\Win64 folder) as the Preview Client Executable in Editor Preferences > Plugins > Mantle."),
		*FString::Join(Looked, TEXT("; ")));
	return false;
}

bool MantlePreviewClient::Launch(const FMantlePreviewClientInfo& Client, const FString& ManifestFile, const FString& LevelName, FString& OutError)
{
	using namespace MantlePreviewClientPrivate;
	OutError.Reset();
	const FString Manifest = FPaths::ConvertRelativePathToFull(ManifestFile);
	if (!IFileManager::Get().FileExists(*Manifest))
	{
		OutError = FString::Printf(TEXT("The preview bundle has no manifest: %s"), *MantlePreviewShared::PlatformPath(Manifest));
		return false;
	}
	if (Client.Executable.IsEmpty() || !IFileManager::Get().FileExists(*Client.Executable))
	{
		OutError = FString::Printf(TEXT("The Mantle client no longer exists: %s"), *Client.Executable);
		return false;
	}

	// One preview per editor: the new bundle replaces this editor's previous client (and only that one).
	Stop();

	const FString UserDir = UserDirFor(LevelName);
	if (!IFileManager::Get().MakeDirectory(*UserDir, true))
	{
		OutError = FString::Printf(TEXT("Could not create the preview's data folder %s."), *UserDir);
		return false;
	}

	TArray<FString> Args = Client.PrefixArgs;
	Args.Add(TEXT("-MantlePreview=") + MantlePreviewShared::PlatformPath(Manifest));
	Args.Add(TEXT("-UserDir=") + MantlePreviewShared::PlatformPath(UserDir));
	Args.Add(TEXT("-windowed"));
	Args.Add(TEXT("-nosplash"));
	const FString Arguments = ClientJoinArgs(Args);

	uint32 ProcessId = 0;
	FProcHandle Process = FPlatformProcess::CreateProc(*Client.Executable, *Arguments, /*bLaunchDetached*/ false, /*bLaunchHidden*/ false,
		/*bLaunchReallyHidden*/ false, &ProcessId, /*PriorityModifier*/ 0, /*OptionalWorkingDirectory*/ nullptr, /*PipeWriteChild*/ nullptr);
	if (!Process.IsValid())
	{
		OutError = FString::Printf(TEXT("Could not start the Mantle client %s."), *Client.Executable);
		return false;
	}
#if PLATFORM_WINDOWS
	if (!ClientAssignToKillOnCloseJob(Process))
	{
		// Still ours and Stop ends it, but it would outlive a crashed editor.
		UE_LOG(LogMantle, Warning, TEXT("MANTLE_PREVIEW_CLIENT could not put the client in the editor's job object; it will not close with the editor."));
	}
#endif
	GOwnedProcess = Process;
	GOwnedProcessId = ProcessId;
	UE_LOG(LogMantle, Display, TEXT("MANTLE_PREVIEW_CLIENT_STARTED pid=%u %s %s"), ProcessId, *Client.Executable, *Arguments);
	return true;
}

bool MantlePreviewClient::IsRunning()
{
	using namespace MantlePreviewClientPrivate;
	if (!GOwnedProcess.IsValid())
	{
		return false;
	}
	if (FPlatformProcess::IsProcRunning(GOwnedProcess))
	{
		return true;
	}
	FPlatformProcess::CloseProc(GOwnedProcess);
	GOwnedProcessId = 0;
	return false;
}

void MantlePreviewClient::Stop()
{
	using namespace MantlePreviewClientPrivate;
	if (!GOwnedProcess.IsValid())
	{
		return;
	}
	if (FPlatformProcess::IsProcRunning(GOwnedProcess))
	{
		// The handle Launch kept: never a process found by name or id.
		FPlatformProcess::TerminateProc(GOwnedProcess, /*KillTree*/ true);
		UE_LOG(LogMantle, Display, TEXT("MANTLE_PREVIEW_CLIENT_STOPPED pid=%u"), GOwnedProcessId);
	}
	FPlatformProcess::CloseProc(GOwnedProcess);
	GOwnedProcessId = 0;
}

FString MantlePreviewClient::UserDirFor(const FString& LevelName)
{
	using namespace MantlePreviewClientPrivate;
	return ClientUserRoot() / ClientSafeName(LevelName);
}

bool MantlePreviewClient::ResetLocalData(const FString& LevelName, FString& OutError)
{
	using namespace MantlePreviewClientPrivate;
	OutError.Reset();
	if (IsRunning())
	{
		OutError = TEXT("Stop the preview first: its local data is in use while it runs.");
		return false;
	}
	const FString UserDir = UserDirFor(LevelName);
	// Only ever a folder directly under the preview data root.
	if (!UserDir.StartsWith(ClientUserRoot() + TEXT("/"), ESearchCase::IgnoreCase))
	{
		OutError = FString::Printf(TEXT("%s is not a preview data folder."), *UserDir);
		return false;
	}
	if (!IFileManager::Get().DirectoryExists(*UserDir))
	{
		return true;
	}
	if (!IFileManager::Get().DeleteDirectory(*UserDir, /*RequireExists*/ false, /*Tree*/ true))
	{
		OutError = FString::Printf(TEXT("Could not delete %s; a file in it may be open in another program."), *MantlePreviewShared::PlatformPath(UserDir));
		return false;
	}
	return true;
}

bool MantlePreviewClient::CheckPlaceScriptApi(const FMantlePreviewClientInfo& Client, int32 PlaceMajor, int32 PlaceMinor, FString& OutError)
{
	OutError.Reset();
	const bool bOlder = Client.ScriptApiMajor < PlaceMajor || (Client.ScriptApiMajor == PlaceMajor && Client.ScriptApiMinor < PlaceMinor);
	if (!bOlder)
	{
		return true;
	}
	const TCHAR* Where = MantlePreviewShared::SourceSuffix(Client.Source);
	OutError = FString::Printf(TEXT("This level needs script API %d.%d, but your Mantle client only has %d.%d (version %s). Update Mantle%s to test it locally."),
		PlaceMajor, PlaceMinor, Client.ScriptApiMajor, Client.ScriptApiMinor, Client.ClientVersion.IsEmpty() ? TEXT("unknown") : *Client.ClientVersion, Where);
	return false;
}
