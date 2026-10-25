// Copyright Joshua Eger. All Rights Reserved.
#include "MantleOverlayGuideLibrary.h"

#include "MantleOverlayGuide.h"

FString UMantleOverlayGuideLibrary::SaveOverlayGuide(const bool bFace, FString Path)
{
	FString Error;
	if (FMantleOverlayGuide::Write(bFace, Path, Error))
	{
		return FString();
	}
	return Error.IsEmpty() ? FString(TEXT("The guide could not be saved.")) : Error;
}

FString UMantleOverlayGuideLibrary::RegenerateShippedOverlayGuides()
{
	for (const bool bFace : {false, true})
	{
		const FString Target = FMantleOverlayGuide::ShippedPath(bFace);
		FString Error;
		if (Target.IsEmpty())
		{
			return TEXT("The MantleCore plugin was not found.");
		}
		if (!FMantleOverlayGuide::Write(bFace, Target, Error, /*bRebuild=*/true))
		{
			return Error.IsEmpty() ? FString(TEXT("The guide could not be saved.")) : Error;
		}
	}
	return FString();
}
