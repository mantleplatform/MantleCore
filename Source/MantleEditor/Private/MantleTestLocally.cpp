// Copyright Joshua Eger. All Rights Reserved.
#include "MantleTestLocally.h"
#include "MantleAvatarContract.h"
#include "MantleCosmeticExport.h"
#include "MantleCosmeticPackage.h"
#include "MantleEditorActions.h"

#include "MantlePlaceSecrets.h"
#include "MantlePreviewExport.h"
#include "MantleRuntime.h"
#include "MantleSettings.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "DesktopPlatformModule.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/SkyLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Components/StaticMeshComponent.h"
#include "FileHelpers.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerStart.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "IDesktopPlatform.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Templates/UnrealTemplate.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopedSlowTask.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "Containers/StringConv.h"

#define LOCTEXT_NAMESPACE "MantleTestLocally"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleTestLocallyPrivate
{
	/** A message on one line, for a log line a harness reads. */
	FString OneLine(const FString& Text)
	{
		FString Line = Text;
		Line.ReplaceInline(TEXT("\r\n"), TEXT(" | "));
		Line.ReplaceInline(TEXT("\n"), TEXT(" | "));
		Line.ReplaceInline(TEXT("\r"), TEXT(" "));
		return Line;
	}

	/** Why the client cannot run a preview at all, or empty when it can. Find applies the same rules; this keeps a cached or hand-built client honest. */
	FString ClientTooOld(const FMantlePreviewClientInfo& Client)
	{
		if (Client.PreviewProtocol < 1 || !Client.ManifestSchemas.Contains(2))
		{
			return FString::Printf(
				TEXT("This Mantle client (%s, version %s) is too old for Test Locally. Update Mantle in Steam or the Epic Games Launcher, then try again."),
				*Client.Source, Client.ClientVersion.IsEmpty() ? TEXT("unknown") : *Client.ClientVersion);
		}
		return FString();
	}

	/** The script API the exported place says it needs, from the bundle's manifest and place file; false (with Error) when they cannot be read. */
	bool ReadPlaceScriptApi(const FString& ManifestFile, int32& OutMajor, int32& OutMinor, FString& OutError)
	{
		OutMajor = 0;
		OutMinor = 0;

		auto LoadObject = [](const FString& File, TSharedPtr<FJsonObject>& OutObject)
		{
			FString Text;
			if (!FFileHelper::LoadFileToString(Text, *File))
			{
				return false;
			}
			const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Text);
			return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
		};

		TSharedPtr<FJsonObject> Manifest;
		if (!LoadObject(ManifestFile, Manifest))
		{
			OutError = FString::Printf(TEXT("Could not read the exported bundle's manifest (%s)."), *ManifestFile);
			return false;
		}
		FString PlaceRelative;
		if (!Manifest->TryGetStringField(TEXT("placeFile"), PlaceRelative) || PlaceRelative.IsEmpty())
		{
			PlaceRelative = TEXT("place.json");
		}
		const FString PlaceFile = FPaths::Combine(FPaths::GetPath(ManifestFile), PlaceRelative);
		TSharedPtr<FJsonObject> Place;
		if (!LoadObject(PlaceFile, Place))
		{
			OutError = FString::Printf(TEXT("Could not read the exported place (%s)."), *PlaceFile);
			return false;
		}

		// A place without scripts carries no scriptApi, so it needs nothing of the client.
		const TSharedPtr<FJsonObject>* ScriptApi = nullptr;
		if (Place->TryGetObjectField(TEXT("scriptApi"), ScriptApi) && ScriptApi && ScriptApi->IsValid())
		{
			(*ScriptApi)->TryGetNumberField(TEXT("major"), OutMajor);
			(*ScriptApi)->TryGetNumberField(TEXT("minor"), OutMinor);
		}
		return true;
	}

	/** The editor's level when the console hands over none (from -ExecCmds it can). */
	UWorld* EditorWorldFor(UWorld* World)
	{
		if ((!World || World->WorldType != EWorldType::Editor) && GEditor)
		{
			return GEditor->GetEditorWorldContext().World();
		}
		return World;
	}

	/** Spawns one scaled engine cube as a static mesh actor (the floor of the test range). */
	AStaticMeshActor* SpawnRangeBlock(UWorld* World, UStaticMesh* Cube, const FVector& Location, const FVector& Scale, const TCHAR* Label)
	{
		AStaticMeshActor* Block = World->SpawnActor<AStaticMeshActor>(Location, FRotator::ZeroRotator);
		if (!Block)
		{
			return nullptr;
		}
		UStaticMeshComponent* Component = Block->GetStaticMeshComponent();
		// A spawned static actor refuses a mesh until it is movable; it goes back to static after.
		Component->SetMobility(EComponentMobility::Movable);
		Component->SetStaticMesh(Cube);
		Component->SetMobility(EComponentMobility::Static);
		Block->SetActorScale3D(Scale);
		Block->SetActorLabel(Label);
		return Block;
	}

	/**
	 * Opens the small test range at FMantleTestLocally::WeaponTestRangePackage: loaded when bCreate is false, else made (a floor,
	 * a sun and sky light, and a player start), saved and left open. Null with OutError, in plain words, when it cannot.
	 */
	UWorld* OpenWeaponTestRange(bool bCreate, FString& OutError)
	{
		const FString Package = FMantleTestLocally::WeaponTestRangePackage();
		if (!GEditor)
		{
			OutError = TEXT("The editor is not ready to open a test range.");
			return nullptr;
		}
		if (!bCreate)
		{
			FString File;
			UWorld* Loaded = FPackageName::TryConvertLongPackageNameToFilename(Package, File, FPackageName::GetMapPackageExtension())
				? UEditorLoadingAndSavingUtils::LoadMap(File) : nullptr;
			if (!Loaded)
			{
				OutError = TEXT("The test range for your weapon could not be opened. Delete the file WeaponTestRange in your MantleWeapons folder and try again.");
			}
			return Loaded;
		}

		UWorld* World = UEditorLoadingAndSavingUtils::NewBlankMap(/*bSaveExistingMap*/ false);
		UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		if (!World || !Cube)
		{
			OutError = TEXT("A test range for your weapon could not be made.");
			return nullptr;
		}
		// The engine cube is 100 cm a side: 100 m by 100 m, 1 m thick, its top at the origin's height.
		SpawnRangeBlock(World, Cube, FVector(0.0, 0.0, -50.0), FVector(100.0, 100.0, 1.0), TEXT("Floor"));
		if (ADirectionalLight* Sun = World->SpawnActor<ADirectionalLight>(FVector(0.0, 0.0, 500.0), FRotator(-50.0, 30.0, 0.0)))
		{
			Sun->SetActorLabel(TEXT("Sun"));
		}
		if (ASkyLight* Sky = World->SpawnActor<ASkyLight>(FVector(0.0, 0.0, 600.0), FRotator::ZeroRotator))
		{
			Sky->SetActorLabel(TEXT("Sky"));
		}
		if (APlayerStart* Start = World->SpawnActor<APlayerStart>(FVector(0.0, 0.0, 100.0), FRotator::ZeroRotator))
		{
			Start->SetActorLabel(TEXT("Player Start"));
		}
		if (!UEditorLoadingAndSavingUtils::SaveMap(World, Package))
		{
			OutError = TEXT("A test range for your weapon could not be saved. Check that your project's files are not read-only, then try again.");
			return nullptr;
		}
		return World;
	}

	FAutoConsoleCommandWithWorldAndArgs GMantleTestLocallyCommand(
		TEXT("Mantle.Preview.TestLocally"),
		TEXT("Mantle.Preview.TestLocally -- the toolbar's Test Locally without the panel: finds the Mantle client, checks it can run this preview, ")
		TEXT("exports the open level, and launches the client on it. Blocks while it finds the client and exports. Logs MANTLE_TEST_LOCALLY <step> lines, ")
		TEXT("then MANTLE_TEST_LOCALLY_LAUNCHED <manifest> pid=unknown client=<source> <version> or MANTLE_TEST_LOCALLY_FAILED <reason>."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
		{
			World = EditorWorldFor(World);
			// The converter's cache key follows package dependencies through the asset registry.
			if (IAssetRegistry* Registry = IAssetRegistry::Get())
			{
				if (Registry->IsLoadingAssets())
				{
					UE_LOG(LogMantle, Display, TEXT("MANTLE_TEST_LOCALLY registry waiting for the asset registry"));
					Registry->WaitForCompletion();
				}
			}

			FMantleTestLocally& TestLocally = FMantleTestLocally::Get();
			FString Error;
			if (TestLocally.Run(World, Error))
			{
				const FMantlePreviewClientInfo& Client = TestLocally.GetClient();
				// MantlePreviewClient does not expose the process id: the line says so rather than invent one.
				UE_LOG(LogMantle, Display, TEXT("MANTLE_TEST_LOCALLY_LAUNCHED %s pid=unknown client=%s %s"),
					*TestLocally.GetManifestFile(), *Client.Source, *Client.ClientVersion);
			}
			else
			{
				UE_LOG(LogMantle, Error, TEXT("MANTLE_TEST_LOCALLY_FAILED %s"),
					Error.IsEmpty() ? TEXT("cancelled") : *OneLine(Error));
			}
		}));

	// The weapon builder's Test locally without the builder, for a harness: the same RunWithWeapon the button calls.
	FAutoConsoleCommandWithWorldAndArgs GMantleTestWeaponLocallyCommand(
		TEXT("Mantle.Preview.TestWeaponLocally"),
		TEXT("Mantle.Preview.TestWeaponLocally <WeaponFolderName> -- the weapon builder's Test locally: exports the open level (or the weapon test range) ")
		TEXT("with Content/MantleWeapons/<WeaponFolderName> bundled and equipped, and launches the client. Logs MANTLE_TEST_LOCALLY_LAUNCHED or MANTLE_TEST_LOCALLY_FAILED."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld*)
		{
			if (Args.Num() != 1)
			{
				UE_LOG(LogMantle, Error, TEXT("MANTLE_TEST_LOCALLY_FAILED usage: Mantle.Preview.TestWeaponLocally <WeaponFolderName>"));
				return;
			}
			if (IAssetRegistry* Registry = IAssetRegistry::Get())
			{
				Registry->WaitForCompletion();
			}
			FMantleTestLocally& TestLocally = FMantleTestLocally::Get();
			FString Error;
			FString Notice;
			if (TestLocally.RunWithWeapon(FMantleWeaponToTry::FromBuilderName(Args[0]), Error, Notice))
			{
				UE_LOG(LogMantle, Display, TEXT("MANTLE_TEST_LOCALLY_LAUNCHED %s client=%s %s%s%s"), *TestLocally.GetManifestFile(),
					*TestLocally.GetClient().Source, *TestLocally.GetClient().ClientVersion, Notice.IsEmpty() ? TEXT("") : TEXT(" notice="), *OneLine(Notice));
			}
			else
			{
				UE_LOG(LogMantle, Error, TEXT("MANTLE_TEST_LOCALLY_FAILED %s"), Error.IsEmpty() ? TEXT("cancelled") : *OneLine(Error));
			}
		}));

	// Chunk 9: the Cosmetics panel's Test locally for an avatar without the panel, for a harness: the panel's default draft for
	// the source, checked as Check does, then the same RunWithDraftAvatar the button calls.
	FAutoConsoleCommand GMantleDevTestAvatarLocallyCommand(
		TEXT("Mantle.Dev.TestAvatarLocally"),
		TEXT("Mantle.Dev.TestAvatarLocally <SourceAsset> [as_is] -- checks the asset as an avatar (converted unless as_is) and runs Test locally with it ")
		TEXT("as the preview player, cooked locally as a draft. Nothing uploads. Logs MANTLE_TEST_LOCALLY_LAUNCHED or MANTLE_TEST_LOCALLY_FAILED."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.IsEmpty())
			{
				UE_LOG(LogMantle, Error, TEXT("MANTLE_TEST_LOCALLY_FAILED usage: Mantle.Dev.TestAvatarLocally <SourceAsset> [as_is]"));
				return;
			}
			if (IAssetRegistry* Registry = IAssetRegistry::Get())
			{
				Registry->WaitForCompletion();
			}
			FString Path = Args[0];
			if (!FPackageName::GetShortName(Path).Contains(TEXT(".")))
			{
				Path += TEXT(".") + FPackageName::GetShortName(Path);
			}
			UObject* Source = LoadObject<UObject>(nullptr, *Path);
			FString ContractError;
			const FMantleAvatarContract* Contract = FMantleAvatarContract::Get(&ContractError);
			if (!Source || !Contract)
			{
				UE_LOG(LogMantle, Error, TEXT("MANTLE_TEST_LOCALLY_FAILED %s"), !Source ? *FString::Printf(TEXT("%s did not load"), *Path) : *OneLine(ContractError));
				return;
			}
			FMantleCosmeticDraft Draft = FMantleEditorActions::MakeCosmeticDraft(EMantleCosmeticClass::Avatar, Source, TEXT("avatar"), FString(), *Contract, EMantleFitSex::Auto);
			Draft.AvatarPublicationMode = Args.Num() > 1 && Args[1].Equals(TEXT("as_is"), ESearchCase::IgnoreCase) ? TEXT("as_is") : TEXT("converted");
			// The thumbnail is never bundled.
			Draft.bRenderThumbnail = false;
			const TSharedRef<FMantleCosmeticPackage> Package = MakeShared<FMantleCosmeticPackage>(FMantleCosmeticExport::Build(Draft, *Contract));
			if (Package->HasErrors())
			{
				for (const FMantleCosmeticIssue& Issue : Package->Issues)
				{
					UE_LOG(LogMantle, Warning, TEXT("MANTLE_TEST_LOCALLY check %s"), *Issue.ToString());
				}
				UE_LOG(LogMantle, Error, TEXT("MANTLE_TEST_LOCALLY_FAILED the check found problems"));
				return;
			}
			FMantleTestLocally& TestLocally = FMantleTestLocally::Get();
			FString Error;
			FString Notice;
			if (TestLocally.RunWithDraftAvatar(Package, Error, Notice))
			{
				UE_LOG(LogMantle, Display, TEXT("MANTLE_TEST_LOCALLY_LAUNCHED %s client=%s %s%s%s"), *TestLocally.GetManifestFile(),
					*TestLocally.GetClient().Source, *TestLocally.GetClient().ClientVersion, Notice.IsEmpty() ? TEXT("") : TEXT(" notice="), *OneLine(Notice));
			}
			else
			{
				UE_LOG(LogMantle, Error, TEXT("MANTLE_TEST_LOCALLY_FAILED %s"), Error.IsEmpty() ? TEXT("cancelled") : *OneLine(Error));
			}
		}));

	FAutoConsoleCommand GMantlePreviewStopCommand(
		TEXT("Mantle.Preview.Stop"),
		TEXT("Mantle.Preview.Stop -- ends the preview process this editor started with Test Locally, and only that one. Logs MANTLE_TEST_LOCALLY_STOPPED."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			FMantleTestLocally::Get().Stop();
			UE_LOG(LogMantle, Display, TEXT("MANTLE_TEST_LOCALLY_STOPPED"));
		}));
}

FMantleTestLocally& FMantleTestLocally::Get()
{
	static FMantleTestLocally Instance;
	return Instance;
}

FString FMantleTestLocally::LevelNameOf(UWorld* World)
{
	if (!World || World->WorldType != EWorldType::Editor)
	{
		return FString();
	}
	const FString PackageName = World->GetOutermost()->GetName();
	if (!PackageName.StartsWith(TEXT("/Game/")) || !FPackageName::DoesPackageExist(PackageName))
	{
		return FString();
	}
	return FPackageName::GetShortName(PackageName);
}

void FMantleTestLocally::LogStep(const FString& Step, const FString& Detail) const
{
	UE_LOG(LogMantle, Display, TEXT("MANTLE_TEST_LOCALLY %s %s"), *Step, *MantleTestLocallyPrivate::OneLine(Detail));
}

void FMantleTestLocally::SetNotice(const FString& Text, bool bError)
{
	Notice = Text;
	bNoticeError = bError;
	++Revision;
}

void FMantleTestLocally::ResetRun()
{
	MantlePlaceSecrets::RemoveFromBundle(BundleDirectory);
	State = EMantleTestLocallyState::Idle;
	ErrorText.Reset();
	Notice.Reset();
	bNoticeError = false;
	LevelName.Reset();
	ManifestFile.Reset();
	BundleDirectory.Reset();
	bGeometry = false;
	bScripts = false;
	bSpawn = false;
	EditorMessages.Reset();
	Messages.Reset();
	Coverage.Reset();
	BlockedRequests = 0;
	LastStatusText.Reset();
	++Revision;
}

bool FMantleTestLocally::Failed(const FString& Why, FString& OutError)
{
	MantlePlaceSecrets::RemoveFromBundle(BundleDirectory);
	State = EMantleTestLocallyState::Failed;
	ErrorText = Why;
	OutError = Why;
	++Revision;
	LogStep(TEXT("failed"), Why);
	return false;
}

bool FMantleTestLocally::EnsureClient(FMantlePreviewClientInfo& OutClient, FString& OutError)
{
	// A client that was found and is still on disk is reused for the editor session, until Choose client or an
	// edit of the Preview Client Executable setting in Project Settings.
	const FString Setting = UMantleSettings::Get()->PreviewClientExecutable.FilePath + TEXT("|")
		+ MantlePreviewClient::ProjectOverride(FCommandLine::Get(), UMantleSettings::Get()->PreviewClientProject.FilePath);
	if (bHasClient && Setting == ClientSetting && IFileManager::Get().FileExists(*Client.Executable))
	{
		OutClient = Client;
		return true;
	}
	bHasClient = false;

	FMantlePreviewClientInfo Found;
	FString Error;
	bool bFound = false;
	{
		// Find blocks while it asks the client what it can do (up to 30 s for a packaged client, 90 s for the dev
		// editor): a progress dialog says so instead of a silent freeze.
		FScopedSlowTask Task(1.0f, LOCTEXT("FindingClient", "Checking the Mantle client..."));
		Task.MakeDialog(/*bShowCancelButton*/ false);
		Task.EnterProgressFrame(1.0f);
		bFound = MantlePreviewClient::Find(Found, Error);
	}
	if (!bFound)
	{
		OutError = Error;
		return false;
	}
	Client = Found;
	ClientSetting = Setting;
	bHasClient = true;
	OutClient = Found;
	return true;
}

void FMantleTestLocally::ForgetClient()
{
	bHasClient = false;
	Client = FMantlePreviewClientInfo();
	++Revision;
}

FMantleTestLocally::EWeaponTestLevel FMantleTestLocally::ChooseWeaponTestLevel(bool bOpenLevelSaved, bool bOpenLevelHasUnsavedChanges, bool bTestRangeExists)
{
	if (bOpenLevelSaved)
	{
		return EWeaponTestLevel::UseOpenLevel;
	}
	if (bOpenLevelHasUnsavedChanges)
	{
		return EWeaponTestLevel::AskToSave;
	}
	return bTestRangeExists ? EWeaponTestLevel::OpenTestRange : EWeaponTestLevel::CreateTestRange;
}

FString FMantleTestLocally::WeaponTestRangePackage()
{
	return TEXT("/Game/MantleWeapons/WeaponTestRange");
}

FString FMantleTestLocally::WeaponTestRangeAskToSaveMessage()
{
	return TEXT("The level you have open has changes that are not saved, and it is not saved as a level yet. Save it first (File > Save Current Level As...), "
		"or start a new empty level, then press Test locally again. Nothing was changed.");
}

bool FMantleTestLocally::Run(UWorld* World, FString& OutError)
{
	OutError.Reset();
	if (bBusy)
	{
		OutError = TEXT("Test Locally is still working on the last request; wait for it to finish.");
		return false;
	}
	TGuardValue<bool> BusyGuard(bBusy, true);
	return RunInternal(World, TArray<FMantleWeaponToTry>(), OutError);
}

bool FMantleTestLocally::RunWithWeapon(const FMantleWeaponToTry& Weapon, FString& OutError, FString& OutNotice)
{
	OutError.Reset();
	OutNotice.Reset();
	if (bBusy)
	{
		OutError = TEXT("Test Locally is still working on the last request; wait for it to finish.");
		return false;
	}
	TGuardValue<bool> BusyGuard(bBusy, true);
	ResetRun();

	// The client first, so a creator whose Mantle app is too old is told so before any level is opened or made.
	State = EMantleTestLocallyState::FindingClient;
	++Revision;
	FMantlePreviewClientInfo ClientInfo;
	FString ClientError;
	if (!EnsureClient(ClientInfo, ClientError))
	{
		return Failed(ClientError, OutError);
	}
	const FString TooOld = MantleTestLocallyPrivate::ClientTooOld(ClientInfo);
	if (!TooOld.IsEmpty())
	{
		return Failed(TooOld, OutError);
	}
	if (!FMantlePreviewExport::ClientReadsWeapons(FMantleTargetFeatures::FromNames(ClientInfo.Features)))
	{
		return Failed(FMantlePreviewExport::WeaponsNeedNewerClientMessage(), OutError);
	}

	FString LevelError;
	UWorld* World = PrepareTestLevel(TEXT("Opened a small test range to try your weapon in."), LevelError, OutNotice);
	if (!World)
	{
		return Failed(LevelError, OutError);
	}

	// Only the weapon just made: the creator's other builder weapons stay out of the test.
	TArray<FMantleWeaponToTry> Weapons;
	Weapons.Add(Weapon);
	const bool bOk = RunInternal(World, Weapons, OutError);
	if (bOk && !OutNotice.IsEmpty())
	{
		SetNotice(OutNotice, /*bError*/ false);
	}
	return bOk;
}

UWorld* FMantleTestLocally::PrepareTestLevel(const FString& RangeNotice, FString& OutError, FString& OutNotice)
{
	UWorld* World = MantleTestLocallyPrivate::EditorWorldFor(nullptr);
	const bool bSaved = !LevelNameOf(World).IsEmpty();
	const bool bUnsavedChanges = World && World->GetOutermost()->IsDirty();
	const FString RangePackage = WeaponTestRangePackage();
	const EWeaponTestLevel Choice = ChooseWeaponTestLevel(bSaved, bUnsavedChanges, FPackageName::DoesPackageExist(RangePackage));
	if (Choice == EWeaponTestLevel::AskToSave)
	{
		OutError = WeaponTestRangeAskToSaveMessage();
		return nullptr;
	}
	if (Choice != EWeaponTestLevel::UseOpenLevel)
	{
		World = MantleTestLocallyPrivate::OpenWeaponTestRange(Choice == EWeaponTestLevel::CreateTestRange, OutError);
		if (!World)
		{
			return nullptr;
		}
		OutNotice = RangeNotice;
	}
	return World;
}

bool FMantleTestLocally::RunWithDraftAvatar(const TSharedRef<const FMantleCosmeticPackage>& Avatar, FString& OutError, FString& OutNotice)
{
	OutError.Reset();
	OutNotice.Reset();
	if (bBusy)
	{
		OutError = TEXT("Test Locally is still working on the last request; wait for it to finish.");
		return false;
	}
	TGuardValue<bool> BusyGuard(bBusy, true);
	ResetRun();

	// The client first, so a creator whose Mantle app is missing or too old is told so before any level is opened or made.
	State = EMantleTestLocallyState::FindingClient;
	++Revision;
	FMantlePreviewClientInfo ClientInfo;
	FString ClientError;
	if (!EnsureClient(ClientInfo, ClientError))
	{
		return Failed(ClientError, OutError);
	}
	const FString TooOld = MantleTestLocallyPrivate::ClientTooOld(ClientInfo);
	if (!TooOld.IsEmpty())
	{
		return Failed(TooOld, OutError);
	}

	FString LevelError;
	UWorld* World = PrepareTestLevel(TEXT("Opened a small test range to walk around in as your avatar."), LevelError, OutNotice);
	if (!World)
	{
		return Failed(LevelError, OutError);
	}
	LogStep(TEXT("draft-avatar"), Avatar->Name);
	const bool bOk = RunInternal(World, TArray<FMantleWeaponToTry>(), OutError, Avatar);
	if (bOk && !OutNotice.IsEmpty())
	{
		SetNotice(OutNotice, /*bError*/ false);
	}
	return bOk;
}

bool FMantleTestLocally::RunInternal(UWorld* World, const TArray<FMantleWeaponToTry>& Weapons, FString& OutError,
	const TSharedPtr<const FMantleCosmeticPackage>& DraftAvatar)
{
	ResetRun();

	// 1. A saved /Game level: the preview reads its scripts and UI files from beside the saved level.
	LevelName = LevelNameOf(World);
	if (LevelName.IsEmpty())
	{
		return Failed(TEXT("Save this level first. Test Locally previews a level that is saved under /Game (File > Save Current Level As...)."), OutError);
	}
	LogStep(TEXT("level"), LevelName);

	// 2. The Mantle client.
	State = EMantleTestLocallyState::FindingClient;
	++Revision;
	FMantlePreviewClientInfo ClientInfo;
	FString ClientError;
	if (!EnsureClient(ClientInfo, ClientError))
	{
		return Failed(ClientError, OutError);
	}
	LogStep(TEXT("client"), FString::Printf(TEXT("%s %s %s"), *ClientInfo.Source, *ClientInfo.ClientVersion, *ClientInfo.Executable));

	// 3. It must be able to run a preview at all; the place's script API is checked once the place exists.
	const FString TooOld = MantleTestLocallyPrivate::ClientTooOld(ClientInfo);
	if (!TooOld.IsEmpty())
	{
		return Failed(TooOld, OutError);
	}
	// A weapon needs a client that reads its settings file.
	if (Weapons.Num() > 0 && !FMantlePreviewExport::ClientReadsWeapons(FMantleTargetFeatures::FromNames(ClientInfo.Features)))
	{
		return Failed(FMantlePreviewExport::WeaponsNeedNewerClientMessage(), OutError);
	}

	// 4. The bundle.
	State = EMantleTestLocallyState::Exporting;
	++Revision;
	LogStep(TEXT("export"), TEXT("started"));
	FMantlePreviewExportOptions Options;
	Options.CookTool = ClientInfo.CookTool;
	// Export for this client's features (an older client's answer names none), so what it cannot read is left out, not refused.
	Options.Features = FMantleTargetFeatures::FromNames(ClientInfo.Features);
	Options.Weapons = Weapons;
	Options.DraftAvatar = DraftAvatar;
	FMantlePreviewExportResult Export;
	FMantlePreviewExport::Run(World, Options, Export);
	if (Export.bCancelled)
	{
		State = EMantleTestLocallyState::Stopped;
		SetNotice(TEXT("Test Locally was cancelled during the export."), /*bError*/ false);
		LogStep(TEXT("cancelled"), TEXT("the creator cancelled the export"));
		return false;
	}
	// The coverage report, for the panel (the export logged it as MANTLE_COVERAGE lines).
	Coverage = Export.Coverage;
	if (!Coverage.IsEmpty())
	{
		LogStep(TEXT("coverage"), Coverage.ToSummary());
	}
	for (const FString& Warning : Export.Warnings)
	{
		FMessage& Message = EditorMessages.AddDefaulted_GetRef();
		Message.Level = TEXT("warn");
		Message.Source = TEXT("export");
		Message.Text = Warning;
		LogStep(TEXT("export-warning"), Warning);
	}
	if (!Export.bSuccess)
	{
		return Failed(Export.Errors.Num() > 0 ? FString::Join(Export.Errors, TEXT("\n")) : FString(TEXT("The export failed.")), OutError);
	}
	ManifestFile = Export.ManifestFile;
	BundleDirectory = Export.BundleDirectory;
	LogStep(TEXT("export"), FString::Printf(TEXT("done %s assets=%d scripts=%d"), *ManifestFile, Export.NumAssets, Export.NumScripts));

	// 5. The client must have the script API this place was exported against.
	int32 PlaceMajor = 0;
	int32 PlaceMinor = 0;
	FString ApiError;
	if (!MantleTestLocallyPrivate::ReadPlaceScriptApi(ManifestFile, PlaceMajor, PlaceMinor, ApiError))
	{
		return Failed(ApiError, OutError);
	}
	if (!MantlePreviewClient::CheckPlaceScriptApi(ClientInfo, PlaceMajor, PlaceMinor, ApiError))
	{
		return Failed(ApiError, OutError);
	}
	LogStep(TEXT("compat"), FString::Printf(TEXT("place script API %d.%d, client %d.%d"),
		PlaceMajor, PlaceMinor, ClientInfo.ScriptApiMajor, ClientInfo.ScriptApiMinor));

	// 6. Start it. Launch stops this editor's previous preview first.
	State = EMantleTestLocallyState::Starting;
	++Revision;
	FString LaunchError;
	if (!MantlePreviewClient::Launch(ClientInfo, ManifestFile, LevelName, LaunchError))
	{
		return Failed(LaunchError, OutError);
	}
	Messages = EditorMessages;
	LogStep(TEXT("launch"), ManifestFile);
	return true;
}

void FMantleTestLocally::Stop()
{
	const bool bWasRunning = MantlePreviewClient::IsRunning();
	if (bWasRunning && (State == EMantleTestLocallyState::Starting || State == EMantleTestLocallyState::Ready))
	{
		// What the client said so far stays on the panel.
		ReadStatusFile();
	}
	MantlePreviewClient::Stop();
	// #1011: the creator's secrets.local.json must not stay behind in the preview folder.
	MantlePlaceSecrets::RemoveFromBundle(BundleDirectory);
	if (State == EMantleTestLocallyState::Starting || State == EMantleTestLocallyState::Ready)
	{
		State = EMantleTestLocallyState::Stopped;
	}
	++Revision;
	LogStep(TEXT("stop"), bWasRunning ? TEXT("the preview process was ended") : TEXT("no preview process of this editor was running"));
}

bool FMantleTestLocally::ReadStatusFile()
{
	if (BundleDirectory.IsEmpty())
	{
		return false;
	}
	const FString File = FPaths::Combine(BundleDirectory, TEXT("preview-status.json"));

	// Shared access: the client replaces this file while the editor reads it.
	TUniquePtr<IFileHandle> Handle(FPlatformFileManager::Get().GetPlatformFile().OpenRead(*File, /*bAllowWrite*/ true));
	if (!Handle)
	{
		return false;
	}
	const int64 Size = Handle->Size();
	if (Size <= 0 || Size > 16 * 1024 * 1024)
	{
		return false;
	}
	TArray<uint8> Bytes;
	Bytes.SetNumUninitialized(static_cast<int32>(Size));
	if (!Handle->Read(Bytes.GetData(), Size))
	{
		return false;
	}
	Handle.Reset();

	const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
	const FString Text(Converted.Length(), Converted.Get());
	if (Text == LastStatusText)
	{
		return false;
	}

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Text);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		// A partial file: keep what is shown and try again on the next poll.
		return false;
	}
	LastStatusText = Text;

	FString StateName;
	Root->TryGetStringField(TEXT("state"), StateName);
	FString ReportedError;
	Root->TryGetStringField(TEXT("error"), ReportedError);
	if (StateName.Equals(TEXT("failed"), ESearchCase::CaseSensitive))
	{
		State = EMantleTestLocallyState::Failed;
		ErrorText = ReportedError.IsEmpty() ? FString(TEXT("The preview failed.")) : ReportedError;
	}
	else if (StateName.Equals(TEXT("ready"), ESearchCase::CaseSensitive))
	{
		State = EMantleTestLocallyState::Ready;
	}
	else if (State != EMantleTestLocallyState::Failed)
	{
		State = EMantleTestLocallyState::Starting;
	}

	const TSharedPtr<FJsonObject>* Ready = nullptr;
	if (Root->TryGetObjectField(TEXT("ready"), Ready) && Ready && Ready->IsValid())
	{
		(*Ready)->TryGetBoolField(TEXT("geometry"), bGeometry);
		(*Ready)->TryGetBoolField(TEXT("scripts"), bScripts);
		(*Ready)->TryGetBoolField(TEXT("spawn"), bSpawn);
	}

	int32 Blocked = 0;
	Root->TryGetNumberField(TEXT("blockedRequests"), Blocked);
	BlockedRequests = Blocked;

	Messages = EditorMessages;
	const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
	if (Root->TryGetArrayField(TEXT("messages"), Entries) && Entries)
	{
		for (const TSharedPtr<FJsonValue>& Value : *Entries)
		{
			const TSharedPtr<FJsonObject>* Entry = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(Entry) || !Entry || !Entry->IsValid())
			{
				continue;
			}
			FMessage& Message = Messages.AddDefaulted_GetRef();
			double Seconds = 0.0;
			if (!(*Entry)->TryGetNumberField(TEXT("t"), Seconds))
			{
				(*Entry)->TryGetNumberField(TEXT("seconds"), Seconds);
			}
			Message.Seconds = Seconds;
			(*Entry)->TryGetStringField(TEXT("level"), Message.Level);
			(*Entry)->TryGetStringField(TEXT("source"), Message.Source);
			(*Entry)->TryGetStringField(TEXT("text"), Message.Text);
		}
	}

	++Revision;
	return true;
}

bool FMantleTestLocally::Poll()
{
	if (bBusy || (State != EMantleTestLocallyState::Starting && State != EMantleTestLocallyState::Ready))
	{
		return false;
	}

	// Alive is asked first: the client writes its last status before it exits, so reading after sees all of it.
	const bool bAlive = MantlePreviewClient::IsRunning();
	bool bChanged = ReadStatusFile();
	if (!bAlive)
	{
		MantlePlaceSecrets::RemoveFromBundle(BundleDirectory);
		if (State == EMantleTestLocallyState::Ready)
		{
			State = EMantleTestLocallyState::Closed;
			LogStep(TEXT("closed"), TEXT("the preview closed"));
		}
		else if (State == EMantleTestLocallyState::Starting)
		{
			State = EMantleTestLocallyState::Failed;
			ErrorText = TEXT("The Mantle client closed before the preview was ready. Its messages, if it wrote any, are listed here; run Test Locally again to retry.");
			LogStep(TEXT("closed"), ErrorText);
		}
		++Revision;
		bChanged = true;
	}
	return bChanged;
}

bool FMantleTestLocally::ChooseClient(FString& OutMessage)
{
	OutMessage.Reset();
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (!DesktopPlatform)
	{
		OutMessage = TEXT("This editor cannot open a file picker; set Preview Client Executable in Editor Preferences > Plugins > Mantle instead.");
		return false;
	}

	UMantleSettings* Settings = UMantleSettings::GetMutable();
	const FString Current = Settings ? Settings->PreviewClientExecutable.FilePath : FString();
	const void* ParentWindow = FSlateApplication::IsInitialized()
		? FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr)
		: nullptr;

	TArray<FString> Files;
	const bool bPicked = DesktopPlatform->OpenFileDialog(
		ParentWindow,
		TEXT("Choose the Mantle client"),
		Current.IsEmpty() ? FString() : FPaths::GetPath(Current),
		FString(),
		TEXT("Mantle client (*.exe)|*.exe"),
		EFileDialogFlags::None,
		Files);
	if (!bPicked || Files.Num() == 0)
	{
		// Cancelled: nothing changes and there is nothing to say.
		return false;
	}

	if (Settings)
	{
		Settings->PreviewClientExecutable.FilePath = FPaths::ConvertRelativePathToFull(Files[0]);
		Settings->SaveConfig();
	}
	ForgetClient();

	FMantlePreviewClientInfo Found;
	FString Error;
	if (!EnsureClient(Found, Error))
	{
		OutMessage = Error;
		return false;
	}
	OutMessage = FString::Printf(TEXT("Using the Mantle client at %s (%s, version %s)."),
		*Found.Executable, *Found.Source, Found.ClientVersion.IsEmpty() ? TEXT("unknown") : *Found.ClientVersion);
	return true;
}

bool FMantleTestLocally::ResetLocalData(const FString& InLevelName, FString& OutMessage)
{
	if (InLevelName.IsEmpty())
	{
		OutMessage = TEXT("Open and save a level first: local data belongs to one level.");
		return false;
	}
	FString Error;
	if (!MantlePreviewClient::ResetLocalData(InLevelName, Error))
	{
		OutMessage = Error;
		return false;
	}
	OutMessage = FString::Printf(TEXT("Deleted the local preview data of %s. The next Test Locally starts it fresh."), *InLevelName);
	return true;
}

#undef LOCTEXT_NAMESPACE
