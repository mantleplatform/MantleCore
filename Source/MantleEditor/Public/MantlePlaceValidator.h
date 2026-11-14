// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantlePlaceTypes.h"

class UWorld;

enum class EMantleIssueSeverity : uint8
{
	Error,
	Warning
};

/**
 * One thing wrong with a level, phrased for the person who has to fix it.
 *
 * Creators see actor labels in the outliner, not JSON pointers, so issues are addressed by
 * label. Every issue carries a remedy — an error that only says what is wrong leaves the
 * creator guessing, and guessing is what makes people give up on a tool.
 */
struct FMantleValidationIssue
{
	EMantleIssueSeverity Severity = EMantleIssueSeverity::Error;

	/** The actor's outliner label, or empty for place-wide issues. */
	FString Actor;

	/** What is wrong. */
	FString Problem;

	/** What to do about it. */
	FString Remedy;

	FString ToString() const
	{
		const TCHAR* Prefix = Severity == EMantleIssueSeverity::Error ? TEXT("Error") : TEXT("Warning");
		if (Actor.IsEmpty())
		{
			return FString::Printf(TEXT("%s: %s %s"), Prefix, *Problem, *Remedy);
		}
		return FString::Printf(TEXT("%s: '%s' %s %s"), Prefix, *Actor, *Problem, *Remedy);
	}
};

struct FMantleValidationResult
{
	TArray<FMantleValidationIssue> Issues;

	bool HasErrors() const
	{
		return Issues.ContainsByPredicate(
			[](const FMantleValidationIssue& Issue) { return Issue.Severity == EMantleIssueSeverity::Error; });
	}

	int32 NumErrors() const
	{
		int32 Count = 0;
		for (const FMantleValidationIssue& Issue : Issues)
		{
			Count += (Issue.Severity == EMantleIssueSeverity::Error) ? 1 : 0;
		}
		return Count;
	}

	FString ToString() const
	{
		TArray<FString> Lines;
		for (const FMantleValidationIssue& Issue : Issues)
		{
			Lines.Add(Issue.ToString());
		}
		return FString::Join(Lines, TEXT("\n"));
	}
};

/**
 * Checks a level against everything Mantle requires before it can be published.
 *
 * Strict on purpose: unsupported content is rejected rather than dropped, because a
 * published place must be a complete record of the creator's work. That is what makes the
 * cloud copy safe to reopen.
 */
class MANTLEEDITOR_API FMantlePlaceValidator
{
public:
	static FMantleValidationResult Validate(UWorld* World);

	/**
	 * The revision notes a Publish sends, held to the schema's limit (#393). Not part of Validate,
	 * which has no notes: Publish adds it to its Check step, before anything is uploaded.
	 */
	static void CheckRevisionNotes(const FString& Notes, FMantleValidationResult& Result);

	/** A place name (the level's, when published from the editor), held to the schema's limit. Validate calls it with the level's name. */
	static void CheckPlaceName(const FString& Name, FMantleValidationResult& Result);
};
