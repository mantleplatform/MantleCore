// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCheck.h"
#include "MantlePlaceInstancing.h"
#include "MantlePlaceWorldPartition.h"
#include "MantleRuntime.h"

#include "Editor.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleCheckPrivate
{
	/** One line for a log a harness greps: no line breaks, no runs of spaces. */
	FString OneLine(const FString& Text)
	{
		FString Line = Text;
		Line.ReplaceInline(TEXT("\r"), TEXT(" "));
		Line.ReplaceInline(TEXT("\n"), TEXT(" "));
		Line.ReplaceInline(TEXT("\t"), TEXT(" "));
		while (Line.ReplaceInline(TEXT("  "), TEXT(" ")) > 0)
		{
		}
		return Line.TrimStartAndEnd();
	}
}

int32 FMantleCheckReport::NumErrors() const
{
	int32 Count = 0;
	for (const FMantleValidationIssue& Issue : Issues)
	{
		Count += Issue.Severity == EMantleIssueSeverity::Error ? 1 : 0;
	}
	return Count;
}

int32 FMantleCheckReport::NumWarnings() const
{
	return Issues.Num() - NumErrors();
}

FMantleCheck::FArgs FMantleCheck::ParseArgs(const TArray<FString>& Args)
{
	FArgs Parsed;
	static const FString ExportPrefix = TEXT("-Export=");
	static const FString ThinPrefix = TEXT("-Thin=");
	for (const FString& Arg : Args)
	{
		if (Arg.IsEmpty())
		{
			continue;
		}
		if (Arg.StartsWith(ExportPrefix, ESearchCase::IgnoreCase))
		{
			Parsed.ExportDir = Arg.RightChop(ExportPrefix.Len()).TrimQuotes();
			if (Parsed.ExportDir.IsEmpty())
			{
				Parsed.Error = TEXT("-Export wants a folder to write the place file to, as in -Export=D:/Checks/MyLevel.");
				return Parsed;
			}
		}
		else if (Arg.StartsWith(ThinPrefix, ESearchCase::IgnoreCase))
		{
			const FString Value = Arg.RightChop(ThinPrefix.Len());
			if (Value.Equals(TEXT("auto"), ESearchCase::IgnoreCase))
			{
				Parsed.bThin = true;
			}
			else if (!Value.Equals(TEXT("off"), ESearchCase::IgnoreCase))
			{
				Parsed.Error = FString::Printf(TEXT("-Thin wants auto (thin foliage and instances evenly if the place is too big) or off, not '%s'."), *Value);
				return Parsed;
			}
		}
		else
		{
			Parsed.Error = FString::Printf(TEXT("Mantle.Check does not know '%s'. It takes -Export=<folder> and -Thin=auto."), *Arg);
			return Parsed;
		}
	}
	return Parsed;
}

FMantleCheckReport FMantleCheck::Evaluate(UWorld* World, const bool bThin)
{
	FMantleCheckReport Report;
	if (!World)
	{
		Report.Issues.Add({ EMantleIssueSeverity::Error, FString(), TEXT("There is no level open to check."), TEXT("Open a level first.") });
		return Report;
	}

	// As Publish does: a World Partition level is checked and exported whole, then its cells go again.
	FMantlePlaceWorldPartition::FLoadResult Loaded = FMantlePlaceWorldPartition::LoadAll(World);
	if (Loaded.bLoaded())
	{
		Report.WorldPartitionNote = FMantlePlaceWorldPartition::DescribeLoad(Loaded);
		Report.Issues.Add({ EMantleIssueSeverity::Warning, FString(), Report.WorldPartitionNote, TEXT("They are unloaded again when the check ends.") });
	}

	const FMantleValidationResult Validation = FMantlePlaceValidator::Validate(World, bThin);
	Report.Issues.Append(Validation.Issues);
	if (!Validation.HasErrors())
	{
		FMantleExportOptions Options;
		Options.Name = World->GetName();
		Options.bThinInstances = bThin;
		const FMantleExportResult Exported = FMantlePlaceExporter::Export(World, Options);
		for (const FMantleParseError& Error : Exported.Errors)
		{
			Report.Issues.Add({ EMantleIssueSeverity::Error, Error.Path, Error.Message, FString() });
		}
		Report.PlaceBytes = Exported.PlaceBytes;
		Report.Instances = Exported.InstanceTotal;
		Report.DrawDistances = Exported.DrawDistanceEntities;
		Report.GroupedActors = Exported.Grouping.Actors;
		Report.ThinnedInstances = Exported.Thinning.bApplied ? Exported.Thinning.Removed() : 0;
		if (Exported.ThinPreview.RemovesAnything() && Exported.ThinPreview.bFits)
		{
			Report.ThinOffer = Exported.ThinPreview.Describe();
		}
		Report.Coverage = Exported.Coverage;
		Report.Water = FMantleWaterBodyExport::Read(World);
		if (Exported.bSuccess)
		{
			Report.PlaceJson = FMantlePlaceExporter::ToJson(Exported.Place);
		}
	}

	FMantlePlaceWorldPartition::ReleaseLoaded(World, Loaded);
	return Report;
}

FString FMantleCheck::DoneLine(const FMantleCheckReport& Report)
{
	return FString::Printf(TEXT("MANTLE_CHECK_DONE ready=%d errors=%d warnings=%d placeBytes=%lld instances=%lld grouped=%d thinned=%lld drawDistances=%d"),
		Report.IsReady() ? 1 : 0, Report.NumErrors(), Report.NumWarnings(), Report.PlaceBytes, Report.Instances, Report.GroupedActors,
		Report.ThinnedInstances, Report.DrawDistances);
}

TArray<FString> FMantleCheck::IssueLines(const FMantleCheckReport& Report)
{
	TArray<FString> Lines;
	for (const FMantleValidationIssue& Issue : Report.Issues)
	{
		Lines.Add(FString::Printf(TEXT("%s %s"),
			Issue.Severity == EMantleIssueSeverity::Error ? TEXT("MANTLE_CHECK_ERROR") : TEXT("MANTLE_CHECK_WARNING"),
			*MantleCheckPrivate::OneLine(Issue.ToString())));
	}
	return Lines;
}

FMantleCheckReport FMantleCheck::Run(UWorld* World, const FArgs& Args)
{
	FMantleCheckReport Report;
	if (!Args.Error.IsEmpty())
	{
		Report.Issues.Add({ EMantleIssueSeverity::Error, FString(), Args.Error, FString() });
		UE_LOG(LogMantle, Error, TEXT("MANTLE_CHECK_ERROR %s"), *Args.Error);
		UE_LOG(LogMantle, Error, TEXT("%s"), *DoneLine(Report));
		return Report;
	}

	// The console hands over the world it ran in; from a commandlet or -ExecCmds that can be none, and the level open in the
	// editor is the one Tools > Mantle > Publish would publish.
	if ((!World || World->WorldType != EWorldType::Editor) && GEditor)
	{
		World = GEditor->GetEditorWorldContext().World();
	}
	UE_LOG(LogMantle, Display, TEXT("MANTLE_CHECK_START level=%s thin=%s"), World ? *World->GetOutermost()->GetName() : TEXT("(none)"),
		Args.bThin ? TEXT("auto") : TEXT("off"));

	Report = Evaluate(World, Args.bThin);

	for (const FString& Line : IssueLines(Report))
	{
		if (Line.StartsWith(TEXT("MANTLE_CHECK_ERROR")))
		{
			UE_LOG(LogMantle, Error, TEXT("%s"), *Line);
		}
		else
		{
			UE_LOG(LogMantle, Display, TEXT("%s"), *Line);
		}
	}
	if (!Report.ThinOffer.IsEmpty())
	{
		UE_LOG(LogMantle, Display, TEXT("MANTLE_CHECK_THIN_OFFER %s Run Mantle.Check -Thin=auto, or Mantle.Publish -Thin=auto, to use it."),
			*MantleCheckPrivate::OneLine(Report.ThinOffer));
	}
	Report.Coverage.Log();
	if (Report.Water.bWaterPluginLoaded)
	{
		UE_LOG(LogMantle, Display, TEXT("%s"), *Report.Water.SummaryLine());
	}

	if (!Args.ExportDir.IsEmpty())
	{
		const FString Dir = FPaths::ConvertRelativePathToFull(Args.ExportDir);
		if (Report.PlaceJson.IsEmpty())
		{
			UE_LOG(LogMantle, Display, TEXT("MANTLE_CHECK_EXPORT none: the check found errors, so there is no place file to write."));
		}
		else if (IFileManager::Get().MakeDirectory(*Dir, /*Tree*/ true)
			&& FFileHelper::SaveStringToFile(Report.PlaceJson, *(Dir / TEXT("place.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			TArray<FString> Notes = IssueLines(Report);
			Notes.Add(DoneLine(Report));
			Notes.Append(Report.Coverage.ToDisplayLines());
			if (Report.Water.bWaterPluginLoaded)
			{
				Notes.Add(Report.Water.SummaryLine());
			}
			FFileHelper::SaveStringToFile(FString::Join(Notes, TEXT("\n")), *(Dir / TEXT("check.txt")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			Report.ExportedTo = Dir;
			UE_LOG(LogMantle, Display, TEXT("MANTLE_CHECK_EXPORT %s"), *Dir);
		}
		else
		{
			UE_LOG(LogMantle, Error, TEXT("MANTLE_CHECK_ERROR Could not write the place file to %s."), *Dir);
		}
	}

	UE_LOG(LogMantle, Display, TEXT("%s"), *DoneLine(Report));
	return Report;
}

void FMantleCheck::AppendPlaceBudget(UWorld* World, FMantleValidationResult& Validation)
{
	if (!World || Validation.HasErrors())
	{
		return;
	}
	FMantleExportOptions Options;
	Options.Name = World->GetName();
	const FMantleExportResult Exported = FMantlePlaceExporter::Export(World, Options);
	if (Exported.bOverBudget)
	{
		Validation.Issues.Add({ EMantleIssueSeverity::Error, FString(), Exported.BudgetProblem, FString() });
	}
}

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleCheckPrivate
{
	FAutoConsoleCommandWithWorldAndArgs GMantleCheckCommand(
		TEXT("Mantle.Check"),
		TEXT("Mantle.Check [-Export=<folder>] [-Thin=auto] -- runs Publish's check and export on the open level without publishing or ")
		TEXT("contacting the server. Loads every World Partition cell as Publish does. -Export writes place.json and check.txt to the folder; ")
		TEXT("-Thin=auto thins foliage and instances evenly when the place is over a limit (never otherwise). Logs one line per finding ")
		TEXT("(MANTLE_CHECK_ERROR / MANTLE_CHECK_WARNING) and ends with MANTLE_CHECK_DONE ready= errors= warnings= placeBytes= instances= ... drawDistances= (the meshes and instance sets given a draw distance)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			FMantleCheck::Run(World, FMantleCheck::ParseArgs(Args));
		}));
}
