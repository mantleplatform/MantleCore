// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Misc/Paths.h"

/** Small helpers the Test Locally preview code (client launch and export) shares. */
namespace MantlePreviewShared
{
	/** The absolute, platform-formatted form of a path. */
	inline FString PlatformPath(const FString& Path)
	{
		FString Full = FPaths::ConvertRelativePathToFull(Path);
		FPaths::MakePlatformFilename(Full);
		return Full;
	}

	inline FString Quoted(const FString& Text)
	{
		return TEXT("\"") + Text + TEXT("\"");
	}

	/** The phrase naming where a client came from ("steam", "epic"), for messages; empty for any other source. */
	inline const TCHAR* SourceSuffix(const FString& Source)
	{
		return Source.Equals(TEXT("steam")) ? TEXT(" in Steam")
			: Source.Equals(TEXT("epic")) ? TEXT(" in the Epic Games Store launcher") : TEXT("");
	}
}
