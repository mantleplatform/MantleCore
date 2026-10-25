// Copyright Joshua Eger. All Rights Reserved.
#include "MantleThumbnailStore.h"
#include "MantleRuntime.h"

#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"

FString FMantleThumbnailStore::DefaultConfigFile()
{
	return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("MantleThumbnailChoices.ini")));
}

FString FMantleThumbnailStore::LegacyConfigFile()
{
	return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Config"), TEXT("MantleThumbnailChoices.ini")));
}

FString FMantleThumbnailStore::SectionFor(const FString& SourcePath, const FString& ClassName)
{
	return FString::Printf(TEXT("%s|%s"), *SourcePath, *ClassName);
}

bool FMantleThumbnailStore::Load(const FString& SourcePath, const FString& ClassName, FMantleThumbnailChoice& OutChoice, const FString& ConfigFile,
	const FString& LegacyFile)
{
	OutChoice = FMantleThumbnailChoice();
	if (SourcePath.IsEmpty())
	{
		return false;
	}
	const FString Section = SectionFor(SourcePath, ClassName);
	FConfigFile Saved;
	Saved.Read(ConfigFile);
	if (!Saved.Contains(Section))
	{
		// The first version kept choices per machine: move this one into the project's file.
		const FString Legacy = !LegacyFile.IsEmpty() ? LegacyFile : (ConfigFile == DefaultConfigFile() ? LegacyConfigFile() : FString());
		if (Legacy.IsEmpty() || !FPaths::FileExists(Legacy))
		{
			return false;
		}
		FConfigFile Old;
		Old.Read(Legacy);
		if (!Old.Contains(Section) || !Load(SourcePath, ClassName, OutChoice, Legacy, FString()))
		{
			return false;
		}
		Save(SourcePath, ClassName, OutChoice, ConfigFile);
		Old.Remove(Section);
		Old.bCanSaveAllSections = true;
		Old.Write(Legacy, false);
		return true;
	}
	FString Text;
	if (Saved.GetString(*Section, TEXT("Mode"), Text))
	{
		OutChoice.Mode = Text == TEXT("Angle") ? EMantleThumbnailMode::Angle : Text == TEXT("Custom") ? EMantleThumbnailMode::Custom : EMantleThumbnailMode::Automatic;
	}
	FVector Angle;
	if (Saved.GetString(*Section, TEXT("Angle"), Text) && Angle.InitFromString(Text) && !Angle.ContainsNaN())
	{
		OutChoice.Angle = FRotator(Angle.Y, Angle.Z, 0.0);
	}
	float Zoom = 1.0f;
	if (Saved.GetFloat(*Section, TEXT("Zoom"), Zoom) && FMath::IsFinite(Zoom))
	{
		OutChoice.Zoom = FMath::Clamp(Zoom, 0.25f, 4.0f);
	}
	if (Saved.GetString(*Section, TEXT("Texture"), Text))
	{
		OutChoice.Texture = TSoftObjectPtr<UTexture2D>(FSoftObjectPath(Text));
	}
	if (Saved.GetString(*Section, TEXT("ImageFile"), Text))
	{
		OutChoice.ImageFile.FilePath = Text;
	}
	return true;
}

void FMantleThumbnailStore::Save(const FString& SourcePath, const FString& ClassName, const FMantleThumbnailChoice& Choice, const FString& ConfigFile)
{
	if (SourcePath.IsEmpty())
	{
		return;
	}
	const FString Section = SectionFor(SourcePath, ClassName);
	FConfigFile Saved;
	Saved.Read(ConfigFile);
	Saved.bCanSaveAllSections = true;
	if (Choice == FMantleThumbnailChoice())
	{
		Saved.Remove(Section);
	}
	else
	{
		const TCHAR* Mode = Choice.Mode == EMantleThumbnailMode::Angle ? TEXT("Angle") : Choice.Mode == EMantleThumbnailMode::Custom ? TEXT("Custom") : TEXT("Automatic");
		Saved.SetString(*Section, TEXT("Mode"), Mode);
		// Pitch and yaw in Y and Z, so one vector holds them.
		Saved.SetString(*Section, TEXT("Angle"), *FVector(0.0, Choice.Angle.Pitch, Choice.Angle.Yaw).ToString());
		Saved.SetString(*Section, TEXT("Zoom"), *FString::SanitizeFloat(Choice.Zoom));
		Saved.SetString(*Section, TEXT("Texture"), *Choice.Texture.ToString());
		Saved.SetString(*Section, TEXT("ImageFile"), *Choice.ImageFile.FilePath);
	}
	if (!Saved.Write(ConfigFile, false))
	{
		UE_LOG(LogMantle, Warning, TEXT("The thumbnail choice could not be saved to %s"), *ConfigFile);
	}
}
