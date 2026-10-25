// Copyright Joshua Eger. All Rights Reserved.
// Which script API version a place's scripts actually need. The exporter
// records the lowest version that covers every script, not the editor's newest, so a place that uses nothing new still runs on older clients.
#pragma once

#include "CoreMinimal.h"
#include "MantleScriptCatalogFiles.h"

struct FMantleGraph;

/** The lowest minor of the script API's major (FMantleApiManifest) one script (or a whole place) needs. */
struct MANTLEEDITOR_API FMantleScriptApiNeed
{
	/** 0 when the script uses only the base (1.0) surface. */
	int32 Minor = 0;

	/** The API name that set Minor ("Avatar", "Player.Avatar"); empty at the base. */
	FString NeededFor;

	/**
	 * False when the script reaches the API in a way a static scan can't follow (getfenv, eval,
	 * getattr with a computed name, a key built from strings). Minor is then
	 * the manifest's newest minor, the conservative answer.
	 */
	bool bAnalysed = true;

	/** When !bAnalysed: the construct that stopped the scan, phrased for the creator ("getfenv"). */
	FString Unanalysable;

	/** Folds Other in: the higher minor wins, and any unanalysable script makes the result so. */
	void Merge(const FMantleScriptApiNeed& Other);
};

/**
 * Finds the API members a script uses, against the introduced-in minors the API manifest records
 * (FMantleApiManifest, generated from the game's FMantleClass::IntroducedIn / ClassIntroducedIn).
 *
 * A text script is scanned, not run: comments are skipped; a global introduced after 1.0
 * (Avatar, Server, Vehicles) counts wherever its name appears as code; a newer member of an
 * older class (Player.Avatar) counts after "." or ":". A string literal that is exactly a newer
 * name counts too, which covers player["Avatar"] and getattr(player, "avatar"). A false match
 * only raises the recorded version, which is the safe direction. Reflection the scan can't
 * follow makes the script unanalysable instead of guessing.
 *
 * A graph script is exact: every member node names its "Class.Member".
 */
class MANTLEEDITOR_API FMantleScriptApiUsage
{
public:
	/** A source file under Scripts/. TypeScript is scanned as its author source. */
	static FMantleScriptApiNeed ForSource(const FString& Source, EMantleScriptCatalogLanguage Language);

	/** A visual-scripting graph. */
	static FMantleScriptApiNeed ForGraph(const FMantleGraph& Graph);

	/** The Check note for an unanalysable script at Scripts/RelativePath. Problem and Remedy. */
	static void DescribeUnanalysable(const FString& RelativePath, const FMantleScriptApiNeed& Need, FString& OutProblem, FString& OutRemedy);
};
