// Copyright Joshua Eger. All Rights Reserved.
//
// Maintainer command: Mantle.Dev.ProposeSplit <SkeletalMesh> [<SkeletalMesh> ...]
// Logs how each character would split into a body and clothing items (FMantleCharacterSplit::ProposeFor: the mesh, the skeletal meshes
// beside it, their sections, bones and fused-clothing verdicts), then the pack review's rows (FMantleCharacterSplit::ReviewFor: where
// each piece sits, its slot, what goes into it, what isn't used and why). Reads only; nothing is made, saved or uploaded and no
// renderer is needed. Logs MANTLE_SPLIT and MANTLE_PACK lines.
#include "MantleCharacterSplit.h"

#include "Engine/SkeletalMesh.h"
#include "HAL/IConsoleManager.h"
#include "Misc/PackageName.h"

DEFINE_LOG_CATEGORY_STATIC(LogMantleProposeSplit, Log, All);

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleDevProposeSplitPrivate
{
	void Run(const TArray<FString>& Args)
	{
		if (Args.Num() < 1)
		{
			UE_LOG(LogMantleProposeSplit, Error, TEXT("MANTLE_SPLIT usage: Mantle.Dev.ProposeSplit <SkeletalMesh> [<SkeletalMesh> ...]"));
			return;
		}
		for (FString Path : Args)
		{
			if (!FPackageName::GetShortName(Path).Contains(TEXT(".")))
			{
				Path += TEXT(".") + FPackageName::GetShortName(Path);
			}
			USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *Path);
			if (!Mesh)
			{
				UE_LOG(LogMantleProposeSplit, Error, TEXT("MANTLE_SPLIT %s: not a skeletal mesh"), *Path);
				continue;
			}
			TArray<FMantleSplitSource> Sources;
			FMantleCharacterSplit::ProposeFor(*Mesh, Sources);
			// #55: the pack review's rows, as the Convert section shows them (MANTLE_PACK lines).
			FMantleCharacterSplit::ReviewFor(*Mesh);
		}
		UE_LOG(LogMantleProposeSplit, Display, TEXT("MANTLE_SPLIT done"));
	}

	FAutoConsoleCommand GMantleDevProposeSplitCommand(
		TEXT("Mantle.Dev.ProposeSplit"),
		TEXT("Mantle.Dev.ProposeSplit <SkeletalMesh> [...] -- logs how each character would split into a body and clothing items (reads only)"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&Run));
}
