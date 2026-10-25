// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAvatarConvertFixer.h"

const TCHAR* LexToString(const EMantleAvatarRoute Route)
{
	return Route == EMantleAvatarRoute::AsIs ? TEXT("asis") : TEXT("convert");
}

namespace MantleAvatarFixersPrivate
{
	TArray<TSharedRef<IMantleAvatarFixer>>& Registry()
	{
		static TArray<TSharedRef<IMantleAvatarFixer>> Fixers;
		return Fixers;
	}

	int32 Order(const FName Id)
	{
		const FName Pipeline[] = { MantleAvatarFixerIds::AsIsRig, MantleAvatarFixerIds::RigMap, MantleAvatarFixerIds::Repose, MantleAvatarFixerIds::Props, MantleAvatarFixerIds::AsIsHeight, MantleAvatarFixerIds::Strip, MantleAvatarFixerIds::MHUnrigged, MantleAvatarFixerIds::MHPose,
			MantleAvatarFixerIds::MHConform, MantleAvatarFixerIds::MHSource, MantleAvatarFixerIds::MHBuild, MantleAvatarFixerIds::RetargetPose,
			MantleAvatarFixerIds::Assembly, MantleAvatarFixerIds::Refit, MantleAvatarFixerIds::Triangles, MantleAvatarFixerIds::Atlas, MantleAvatarFixerIds::Bake };
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(Pipeline); ++Index)
		{
			if (Pipeline[Index] == Id)
			{
				return Index;
			}
		}
		return UE_ARRAY_COUNT(Pipeline);
	}
}

void FMantleAvatarFixers::Register(const TSharedRef<IMantleAvatarFixer>& Fixer)
{
	TArray<TSharedRef<IMantleAvatarFixer>>& Fixers = MantleAvatarFixersPrivate::Registry();
	// One per id: a module restart registers again.
	Fixers.RemoveAll([&Fixer](const TSharedRef<IMantleAvatarFixer>& Existing) { return Existing->GetId() == Fixer->GetId(); });
	Fixers.Add(Fixer);
}

TArray<TSharedRef<IMantleAvatarFixer>> FMantleAvatarFixers::Get()
{
	TArray<TSharedRef<IMantleAvatarFixer>> Fixers = MantleAvatarFixersPrivate::Registry();
	Fixers.StableSort([](const TSharedRef<IMantleAvatarFixer>& A, const TSharedRef<IMantleAvatarFixer>& B)
	{
		return MantleAvatarFixersPrivate::Order(A->GetId()) < MantleAvatarFixersPrivate::Order(B->GetId());
	});
	return Fixers;
}
