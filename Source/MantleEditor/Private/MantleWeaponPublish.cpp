// Copyright Joshua Eger. All Rights Reserved.
#include "MantleWeaponPublish.h"

#include "MantleApiClient.h"
#include "MantleAttribution.h"
#include "MantleCreatorSignIn.h"
#include "MantlePlaceAssets.h"
#include "MantleRuntime.h"
#include "MantleSettings.h"
#include "MantleWeaponSpawnPoint.h"

#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"

// =============================================================================================
// One weapon, as a publish source

FMantleWeaponPublishSource::FMantleWeaponPublishSource(FString InWeaponName, TSharedPtr<FMantlePlaceAssetConverter> InConverter)
	: WeaponName(MoveTemp(InWeaponName))
	, Converter(MoveTemp(InConverter))
{
}

FString FMantleWeaponPublishSource::ProblemWith(const FString& WeaponName)
{
	if (!AMantleWeaponSpawnPoint::IsBuilderName(WeaponName))
	{
		return FString::Printf(TEXT("'%s' is not a weapon the weapon builder made."), *WeaponName);
	}
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *AMantleWeaponSpawnPoint::BuiltDocumentFile(WeaponName)))
	{
		return FString::Printf(TEXT("The weapon '%s' has no settings file. Press Create weapon, then Publish."), *WeaponName);
	}
	const FString Problem = FMantlePlaceAssets::WeaponDocumentProblem(WeaponName, Text);
	if (!Problem.IsEmpty())
	{
		return FString::Printf(TEXT("The weapon '%s' %s."), *WeaponName, *Problem);
	}
	if (!IFileManager::Get().FileExists(*(FPaths::ProjectContentDir() / TEXT("MantleWeapons") / WeaponName / (TEXT("SKM_") + WeaponName + TEXT(".uasset")))))
	{
		return FString::Printf(TEXT("The weapon '%s' has no 3D model. Press Create weapon, then Publish."), *WeaponName);
	}
	return FString();
}

bool FMantleWeaponPublishSource::Prepare(FMantleValidationResult& OutValidation, FString& OutMessage)
{
	OutValidation = FMantleValidationResult();
	const FString Problem = ProblemWith(WeaponName);
	if (!Problem.IsEmpty())
	{
		OutMessage = Problem;
		return false;
	}
	// The item is remembered per API and account, as a place's vehicles and weapons are.
	LinkApi = FMantleApiClient::BaseUrlResolver();
	FString WhyNot;
	if (!FMantleCreatorSignIn::ResolveAccountId(LinkAccount, WhyNot))
	{
		OutMessage = WhyNot + TEXT(" Sign in again (Tools > Mantle) and choose Publish again; nothing was uploaded.");
		return false;
	}
	Dependencies = FMantlePlaceDependencies();
	if (!Dependencies.AddBuiltWeapon(WeaponName, WeaponName))
	{
		OutMessage = FString::Printf(TEXT("The weapon '%s' could not be loaded. Press Create weapon again, then Publish."), *WeaponName);
		return false;
	}
	// The gathered model and materials are held only weakly; keep them until the conversion is done.
	Pins.Pin(Dependencies);
	if (!Converter.IsValid())
	{
		Converter = MakeShared<FMantlePlaceAssetConverter>();
	}
	return true;
}

int32 FMantleWeaponPublishSource::NumConversionSteps() const
{
	return FMantlePlaceAssetConverter::NumDependencySteps(Dependencies);
}

FString FMantleWeaponPublishSource::ConversionStepName(int32 Step) const
{
	return FMantlePlaceAssetConverter::DependencyStepName(Dependencies, Step);
}

void FMantleWeaponPublishSource::Convert(int32 Step, FMantlePlaceAssetSet& Set)
{
	if (Converter.IsValid())
	{
		Converter->ConvertDependencyStep(Dependencies, Step, Set);
	}
}

bool FMantleWeaponPublishSource::CollectScripts(TArray<FMantlePlaceScriptUpload>& OutScripts, FString& OutMessage)
{
	OutScripts.Reset();
	return true;
}

EMantlePlaceExportOutcome FMantleWeaponPublishSource::Export(const FMantlePlaceAssetSet& Set, const TMap<FString, FString>& ScriptSourceIds,
	const TMap<FString, FString>& ScriptAuthorSourceIds, FString& OutPlaceJson, TArray<FMantleValidationIssue>& OutIssues)
{
	// A library-only source writes no place file; the session never asks.
	return EMantlePlaceExportOutcome::Failed;
}

FString FMantleWeaponPublishSource::FindLibraryItem(const FString& SourcePath)
{
	return UMantleSettings::Get()->FindLibraryItemForVehicle(LinkApi, LinkAccount, SourcePath);
}

void FMantleWeaponPublishSource::RememberLibraryItem(const FString& SourcePath, const FString& ItemId)
{
	UMantleSettings::GetMutable()->RememberLibraryItemForVehicle(LinkApi, LinkAccount, SourcePath, ItemId);
}

void FMantleWeaponPublishSource::ForgetLibraryItem(const FString& SourcePath)
{
	UMantleSettings::GetMutable()->ForgetLibraryItemForVehicle(LinkApi, LinkAccount, SourcePath);
}

int32 FMantleWeaponPublishSource::LibraryGeneration(const FString& SourcePath)
{
	return UMantleSettings::Get()->FindLibraryGenerationForVehicle(LinkApi, LinkAccount, SourcePath);
}

FString FMantleWeaponPublishSource::AttributionFor(const FString& ObjectPath) const
{
	return FMantleAttribution::ForObjectPath(ObjectPath);
}

// =============================================================================================
// The builder's Publish

FMantleWeaponPublish& FMantleWeaponPublish::Get()
{
	static FMantleWeaponPublish Instance;
	return Instance;
}

bool FMantleWeaponPublish::Start(const FString& InWeaponName, const FMantleVehicleLibraryChoice& Choice, FString& OutError)
{
	return StartAll({ InWeaponName }, Choice, OutError);
}

bool FMantleWeaponPublish::StartAll(const TArray<FString>& WeaponNames, const FMantleVehicleLibraryChoice& Choice, FString& OutError)
{
	if (IsBusy())
	{
		OutError = FString::Printf(TEXT("'%s' is still publishing. Wait for it to finish, then try again."), *WeaponName);
		return false;
	}
	if (!Choice.IsReady())
	{
		OutError = TEXT("A public weapon needs the rights declaration: tick it, or choose Private.");
		return false;
	}
	if (WeaponNames.Num() == 0)
	{
		OutError = TEXT("Name a weapon to publish.");
		return false;
	}
	for (const FString& Name : WeaponNames)
	{
		const FString Problem = FMantleWeaponPublishSource::ProblemWith(Name);
		if (!Problem.IsEmpty())
		{
			OutError = Problem;
			return false;
		}
	}
	Waiting = WeaponNames;
	BatchChoice = Choice;
	Succeeded = 0;
	Failed = 0;
	StartNext();
	if (!TickerHandle.IsValid())
	{
		TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FMantleWeaponPublish::Tick));
	}
	return true;
}

void FMantleWeaponPublish::StartNext()
{
	WeaponName = Waiting[0];
	Waiting.RemoveAt(0);
	const TSharedRef<FMantleWeaponPublishSource> Source = MakeShared<FMantleWeaponPublishSource>(WeaponName);
	// A go-live: the uploads wait for Mantle's approval, which a library item needs.
	Session = MakeShared<FMantlePlacePublishSession>(Source, MakeShared<FMantleApiUploadTransport>(), /*bGoLive*/ true, FString());
	Session->VehicleChoice = BatchChoice;
	UE_LOG(LogMantle, Log, TEXT("MANTLE_WEAPON_PUBLISH started weapon=%s visibility=%s"), *WeaponName, BatchChoice.bPublic ? TEXT("public") : TEXT("private"));
	Session->Start();
}

bool FMantleWeaponPublish::Tick(float DeltaSeconds)
{
	if (Session.IsValid() && !Session->IsFinished())
	{
		Session->Tick();
	}
	if (!Session.IsValid() || !Session->IsFinished())
	{
		return true;
	}
	if (Session->Succeeded())
	{
		++Succeeded;
		UE_LOG(LogMantle, Log, TEXT("MANTLE_WEAPON_PUBLISH_DONE weapon=%s %s"), *WeaponName, *Session->GetResult().Message);
	}
	else
	{
		++Failed;
		UE_LOG(LogMantle, Warning, TEXT("MANTLE_WEAPON_PUBLISH_FAILED weapon=%s %s"), *WeaponName, *Session->GetResult().Message);
		for (const FMantleValidationIssue& Issue : Session->GetResult().Validation.Issues)
		{
			UE_LOG(LogMantle, Warning, TEXT("MANTLE_WEAPON_PUBLISH_ISSUE %s %s"), *Issue.Problem, *Issue.Remedy);
		}
		for (const FMantleValidationIssue& Issue : Session->GetAttention())
		{
			UE_LOG(LogMantle, Warning, TEXT("MANTLE_WEAPON_PUBLISH_ISSUE %s %s"), *Issue.Problem, *Issue.Remedy);
		}
	}
	if (Waiting.Num() > 0)
	{
		StartNext();
		return true;
	}
	UE_LOG(LogMantle, Log, TEXT("MANTLE_WEAPON_PUBLISH_ALL_DONE ok=%d failed=%d"), Succeeded, Failed);
	TickerHandle.Reset();
	return false;
}

namespace MantleWeaponPublishPrivate
{
	// The builder's Publish without the builder, for a harness: the same FMantleWeaponPublish the button starts.
	FAutoConsoleCommand GMantleWeaponPublishCommand(
		TEXT("Mantle.Weapon.Publish"),
		TEXT("Mantle.Weapon.Publish <WeaponFolderName>[+<WeaponFolderName>...] public|private -- the weapon builder's Publish: uploads each ")
		TEXT("Content/MantleWeapons/<WeaponFolderName> in turn and makes it a library item of kind weapon. public ticks the rights declaration on the ")
		TEXT("creator's behalf. Logs MANTLE_WEAPON_PUBLISH_DONE or MANTLE_WEAPON_PUBLISH_FAILED and MANTLE_LIBRARY_PUBLISHED per weapon, then ")
		TEXT("MANTLE_WEAPON_PUBLISH_ALL_DONE."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() != 2 || !(Args[1] == TEXT("public") || Args[1] == TEXT("private")))
			{
				UE_LOG(LogMantle, Error, TEXT("MANTLE_WEAPON_PUBLISH_FAILED usage: Mantle.Weapon.Publish <WeaponFolderName>[+<WeaponFolderName>...] public|private"));
				return;
			}
			FMantleVehicleLibraryChoice Choice;
			Choice.bPublic = Args[1] == TEXT("public");
			Choice.bRightsDeclared = Choice.bPublic;
			FString Error;
			TArray<FString> Names;
			// ',' or '+' between names: -ExecCmds splits a command line on commas.
			Args[0].Replace(TEXT("+"), TEXT(",")).ParseIntoArray(Names, TEXT(","));
			if (!FMantleWeaponPublish::Get().StartAll(Names, Choice, Error))
			{
				UE_LOG(LogMantle, Error, TEXT("MANTLE_WEAPON_PUBLISH_FAILED weapon=%s %s"), *Args[0], *Error);
			}
		}));
}
