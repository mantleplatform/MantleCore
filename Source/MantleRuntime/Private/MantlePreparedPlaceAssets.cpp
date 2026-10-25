// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePreparedPlaceAssets.h"
#include "MantlePlaceAssets.h"

#include "ProfilingDebugging/CpuProfilerTrace.h"

struct FMantlePreparedPlaceAssetsState
{
	TSharedPtr<FMantlePlaceAssets> Assets;
};

bool FMantlePreparedPlaceAssets::IsUploadedId(const FString& Id)
{
	return FMantlePlaceAssets::IsUploadedId(Id);
}

FMantlePreparedPlaceAssets FMantlePreparedPlaceAssets::PrepareAndWait(const FMantlePlace& Place, float TimeoutSeconds,
	TSharedPtr<IMantleAssetResolver> Resolver, TSharedPtr<FMantleAssetStore> Store)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(MantlePreparedPlaceAssets_PrepareAndWait);
	FMantlePreparedPlaceAssets Prepared;
	Prepared.bNeeded = FMantlePlaceAssets::HasUploadedAssets(Place);
	if (!Prepared.bNeeded)
	{
		Prepared.bComplete = true;
		return Prepared;
	}

	FMantlePlaceAssetSettings Settings = FMantlePlaceAssetSettings::ForThisProcess();
	// WaitUntilComplete pumps it; the ticker must not pump it a second time.
	Settings.bAutoPump = false;
	if (Resolver.IsValid())
	{
		Settings.Resolver = Resolver;
	}
	if (Store.IsValid())
	{
		Settings.Store = Store;
	}

	const TSharedRef<FMantlePlaceAssets> Assets = FMantlePlaceAssets::Prepare(Place, Settings, FMantlePlaceAssetsDone());
	Prepared.bComplete = Assets->WaitUntilComplete(TimeoutSeconds);
	if (Prepared.bComplete)
	{
		Prepared.Errors = Assets->GetResult().Errors;
	}
	else
	{
		Prepared.Errors.Add(FMantleParseError{ FString(), FString::Printf(
			TEXT("the place's uploaded meshes and materials were not ready after %.0f seconds"), TimeoutSeconds) });
	}

	Prepared.State = MakeShared<FMantlePreparedPlaceAssetsState>();
	Prepared.State->Assets = Assets;
	return Prepared;
}

void FMantlePreparedPlaceAssets::Release()
{
	State.Reset();
}
