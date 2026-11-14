// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCheck.h"
#include "MantleMeshLumenBake.h"
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
		else
		{
			Parsed.Error = FString::Printf(TEXT("Mantle.Check does not know '%s'. It takes -Export=<folder>."), *Arg);
			return Parsed;
		}
	}
	return Parsed;
}

FMantleCheckReport FMantleCheck::Evaluate(UWorld* World)
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

	const FMantleValidationResult Validation = FMantlePlaceValidator::Validate(World);
	Report.Issues.Append(Validation.Issues);
	// #149: meshes a Lumen level would publish without Lumen data, and why.
	FMantleMeshLumenBake::CheckLevel(World, Report.Issues);
	if (!Validation.HasErrors())
	{
		FMantleExportOptions Options;
		Options.Name = World->GetName();
		const FMantleExportResult Exported = FMantlePlaceExporter::Export(World, Options);
		for (const FMantleParseError& Error : Exported.Errors)
		{
			Report.Issues.Add({ EMantleIssueSeverity::Error, Error.Path, Error.Message, FString() });
		}
		Report.PlaceBytes = Exported.PlaceBytes;
		Report.Instances = Exported.InstanceTotal;
		Report.DrawDistances = Exported.DrawDistanceEntities;
		Report.GroupedActors = Exported.Grouping.Actors;
		Report.Cells = Exported.CellReport.NumCells();
		Report.CellEntities = Exported.CellReport.CellEntities;
		Report.CellInstances = Exported.CellReport.CellInstances;
		Report.CellLines = CellLines(Exported);
		Report.Coverage = Exported.Coverage;
		Report.Water = FMantleWaterBodyExport::Read(World);
		if (Exported.bSuccess)
		{
			FMantleCellRoot Root;
			TArray<FMantleCellOfflineFile> CellFiles;
			FString CellError;
			if (Exported.Cells.Num() > 0 && !FMantleCellFiles::BuildOffline(Exported.CellGrid, Exported.Cells, CellFiles, Root, CellError))
			{
				Report.Issues.Add({ EMantleIssueSeverity::Error, FString(), CellError, FString() });
			}
			else
			{
				Report.PlaceJson = FMantlePlaceExporter::ToJson(Exported.Place, Exported.Cells.Num() > 0 ? &Root : nullptr);
			}
		}
	}

	FMantlePlaceWorldPartition::ReleaseLoaded(World, Loaded);
	return Report;
}

FString FMantleCheck::DoneLine(const FMantleCheckReport& Report)
{
	return FString::Printf(TEXT("MANTLE_CHECK_DONE ready=%d errors=%d warnings=%d placeBytes=%lld instances=%lld grouped=%d drawDistances=%d cells=%d"),
		Report.IsReady() ? 1 : 0, Report.NumErrors(), Report.NumWarnings(), Report.PlaceBytes, Report.Instances, Report.GroupedActors,
		Report.DrawDistances, Report.Cells);
}

TArray<FString> FMantleCheck::CellLines(const FMantleExportResult& Exported)
{
	TArray<FString> Lines;
	const FMantleCellReport& Report = Exported.CellReport;
	if (Report.NumCells() == 0 && Report.NumStayed() == 0)
	{
		return Lines;
	}
	int64 Solid = 0;
	for (const FMantlePlannedCell& Cell : Exported.Cells)
	{
		Solid += Cell.NumSolid();
	}
	FString PerLevel;
	for (int32 Level = 0; Level < Exported.CellGrid.Levels.Num() && Level < FMantleCellGrid::MaxLevels; ++Level)
	{
		PerLevel += FString::Printf(TEXT(" level%d=%d"), Level, Report.CellsPerLevel[Level]);
	}
	Lines.Add(FString::Printf(TEXT("MANTLE_CHECK_CELLS cells=%d entities=%lld instances=%lld solid=%lld folded=%d groupsDropped=%d%s"),
		Report.NumCells(), Report.CellEntities, Report.CellInstances, Solid, Report.FoldedActors, Report.DroppedGroups, *PerLevel));
	for (const FMantleCellKey& Key : Report.BiggestCells)
	{
		const FMantlePlannedCell* Cell = Exported.Cells.FindByPredicate([&Key](const FMantlePlannedCell& Candidate) { return Candidate.Key == Key; });
		if (!Cell)
		{
			continue;
		}
		TArray<FMantleCellFileBlob> Files;
		FString Why;
		int64 Bytes = 0;
		if (FMantleCellFiles::WriteCell(*Cell, Files, Why))
		{
			for (const FMantleCellFileBlob& File : Files)
			{
				Bytes += File.Bytes.Num();
			}
		}
		Lines.Add(FString::Printf(TEXT("MANTLE_CHECK_CELL %s entities=%d instances=%lld solid=%lld files=%d bytes=%lld drawDistance=%.0f"),
			*Key.ToString(), Cell->NumEntities(), Cell->NumInstances(), Cell->NumSolid(), Files.Num(), Bytes, Cell->DrawDistance));
	}
	for (int32 Reason = 0; Reason < static_cast<int32>(EMantleCellStay::Count); ++Reason)
	{
		if (Report.StayCounts[Reason] > 0)
		{
			Lines.Add(FString::Printf(TEXT("MANTLE_CHECK_ROOT %lld %s"), Report.StayCounts[Reason], MantleCellStayText(static_cast<EMantleCellStay>(Reason))));
		}
	}
	return Lines;
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
	UE_LOG(LogMantle, Display, TEXT("MANTLE_CHECK_START level=%s"), World ? *World->GetOutermost()->GetName() : TEXT("(none)"));

	Report = Evaluate(World);

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
	Report.Coverage.Log();
	for (const FString& Line : Report.CellLines)
	{
		UE_LOG(LogMantle, Display, TEXT("%s"), *Line);
	}
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
			Notes.Append(Report.CellLines);
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
		TEXT("Mantle.Check [-Export=<folder>] -- runs Publish's check and export on the open level without publishing or ")
		TEXT("contacting the server. Loads every World Partition cell as Publish does. -Export writes place.json and check.txt to the folder; ")
		TEXT("Logs one line per finding ")
		TEXT("(MANTLE_CHECK_ERROR / MANTLE_CHECK_WARNING) and ends with MANTLE_CHECK_DONE ready= errors= warnings= placeBytes= instances= ... drawDistances= (the meshes and instance sets given a draw distance)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			FMantleCheck::Run(World, FMantleCheck::ParseArgs(Args));
		}));
}
