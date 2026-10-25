// Copyright Joshua Eger. All Rights Reserved.
#include "MantleSettings.h"
#include "MantleConnection.h"

#include "Engine/World.h"
#include "UObject/Package.h"

UMantleSettings::UMantleSettings()
{
	// Empty: publish to Mantle's hosted API (DefaultApiBaseUrl). A developer sets this, or
	// MANTLE_API_URL, to reach a local one.
	ApiBaseUrlOverride = FString();
}

const UMantleSettings* UMantleSettings::Get()
{
	return GetDefault<UMantleSettings>();
}

UMantleSettings* UMantleSettings::GetMutable()
{
	return GetMutableDefault<UMantleSettings>();
}

bool UMantleSettings::AllowsAsIsAvatars()
{
	return Get()->bAllowAsIsAvatars;
}

FMantleAsIsAvatarsScope::FMantleAsIsAvatarsScope(const bool bAllow)
{
	UMantleSettings* Settings = UMantleSettings::GetMutable();
	bWas = Settings->bAllowAsIsAvatars;
	Settings->bAllowAsIsAvatars = bAllow;
}

FMantleAsIsAvatarsScope::~FMantleAsIsAvatarsScope()
{
	UMantleSettings::GetMutable()->bAllowAsIsAvatars = bWas;
}

FString UMantleSettings::LevelKey(const UWorld* World)
{
	return World ? World->GetOutermost()->GetName() : FString();
}

FString UMantleSettings::MakeLinkKey(const FString& ApiBaseUrl, const FString& AccountId, const FString& LevelPath)
{
	// Hosts are case-insensitive and a trailing slash names the same API, so neither may split one
	// link into two.
	FString Api = ApiBaseUrl.TrimStartAndEnd().ToLower();
	while (Api.EndsWith(TEXT("/")))
	{
		Api.LeftChopInline(1);
	}
	return FString::Printf(TEXT("%s|%s|%s"), *Api, *AccountId.TrimStartAndEnd(), *LevelPath);
}

FString UMantleSettings::FindPlaceForLevel(const FString& ApiBaseUrl, const FString& AccountId, const FString& LevelPath) const
{
	const FString* Own = PlaceIdByLink.Find(MakeLinkKey(ApiBaseUrl, AccountId, LevelPath));
	return Own ? *Own : FString();
}

void UMantleSettings::RememberPlaceForLevel(
	const FString& ApiBaseUrl, const FString& AccountId, const FString& LevelPath, const FString& PlaceId)
{
	PlaceIdByLink.Add(MakeLinkKey(ApiBaseUrl, AccountId, LevelPath), PlaceId);

	// Saved immediately rather than on editor shutdown: losing this mapping to a crash
	// would silently orphan the creator's published place and create a duplicate next time.
	SaveConfig();
}

void UMantleSettings::UnlinkPlaceForLevel(const FString& ApiBaseUrl, const FString& AccountId, const FString& LevelPath)
{
	// An empty entry rather than none: the level is unlinked for this account, not merely unrecorded.
	PlaceIdByLink.Add(MakeLinkKey(ApiBaseUrl, AccountId, LevelPath), FString());
	SaveConfig();
}

FString UMantleSettings::FindLibraryItemForVehicle(const FString& ApiBaseUrl, const FString& AccountId, const FString& SourcePath) const
{
	const FString* Item = LibraryItemByLink.Find(MakeLinkKey(ApiBaseUrl, AccountId, SourcePath));
	return Item ? *Item : FString();
}

void UMantleSettings::RememberLibraryItemForVehicle(const FString& ApiBaseUrl, const FString& AccountId, const FString& SourcePath, const FString& ItemId)
{
	LibraryItemByLink.Add(MakeLinkKey(ApiBaseUrl, AccountId, SourcePath), ItemId);
	// Saved at once, as the place link is: a crash must not lose the item and make the next publish a duplicate.
	SaveConfig();
}

void UMantleSettings::ForgetLibraryItemForVehicle(const FString& ApiBaseUrl, const FString& AccountId, const FString& SourcePath)
{
	const FString Key = MakeLinkKey(ApiBaseUrl, AccountId, SourcePath);
	LibraryItemByLink.Remove(Key);
	// The next item made for this vehicle gets an Idempotency-Key the forgotten item's creation never used.
	int32& Generation = LibraryGenerationByLink.FindOrAdd(Key);
	++Generation;
	SaveConfig();
}

int32 UMantleSettings::FindLibraryGenerationForVehicle(const FString& ApiBaseUrl, const FString& AccountId, const FString& SourcePath) const
{
	const int32* Generation = LibraryGenerationByLink.Find(MakeLinkKey(ApiBaseUrl, AccountId, SourcePath));
	return Generation ? *Generation : 0;
}

FString UMantleSettings::DefaultApiBaseUrl()
{
	return FMantleConnection::HostedBaseUrl;
}

FString UMantleSettings::ResolveApiBaseUrl() const
{
	// The command line and the environment still win, so a developer can point at a local or
	// staging server without editing settings. The order lives in the runtime, because the
	// shipped client resolves its address the same way; only the fallback differs, and the
	// plugin's is the hosted API even in an editor build.
	const FString Override = ApiBaseUrlOverride.TrimStartAndEnd();
	return FMantleConnection::ResolveApiBaseUrl(Override.IsEmpty() ? DefaultApiBaseUrl() : Override);
}
