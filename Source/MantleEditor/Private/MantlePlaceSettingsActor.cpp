// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceSettingsActor.h"
#include "MantleEnvironmentOverride.h"
#include "MantlePlaceLimits.h"
#include "MantlePlaceTypes.h"
#include "MantlePlayStyle.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "LevelEditorViewport.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "Mantle"

AMantlePlaceSettingsActor* AMantlePlaceSettingsActor::Find(UWorld* World)
{
	if (!World)
	{
		return nullptr;
	}
	for (TActorIterator<AMantlePlaceSettingsActor> It(World); It; ++It)
	{
		if (IsValid(*It))
		{
			return *It;
		}
	}
	return nullptr;
}

AMantlePlaceSettingsActor* AMantlePlaceSettingsActor::FindOrAdd(UWorld* World)
{
	if (AMantlePlaceSettingsActor* Existing = Find(World))
	{
		return Existing;
	}
	if (!World)
	{
		return nullptr;
	}
	// One undo step for the add: spawning inside a transaction records the level, so undo
	// removes the actor again. The importer calls this (through SetFrom) inside its own
	// "Import Mantle place" transaction, which this one simply nests into.
	const FScopedTransaction Transaction(LOCTEXT("AddPlaceSettings", "Add Mantle place settings"));
	AMantlePlaceSettingsActor* Added = World->SpawnActor<AMantlePlaceSettingsActor>();
#if WITH_EDITOR
	if (Added)
	{
		// Recorded before the label changes, so the whole add undoes as one.
		Added->Modify();
		Added->SetActorLabel(TEXT("MantlePlaceSettings"));
	}
#endif
	return Added;
}

bool AMantlePlaceSettingsActor::SetThumbnailFromViewport()
{
	// A perspective viewport only: an orthographic one's camera is no place to take a picture from.
	const FLevelEditorViewportClient* Viewport = GCurrentLevelEditingViewportClient;
	if (!Viewport || !Viewport->IsPerspective())
	{
		return false;
	}
	const FScopedTransaction Transaction(LOCTEXT("UseCurrentView", "Use current view for the Mantle thumbnail"));
	Modify();
	ThumbnailChoice.Mode = EMantleThumbnailMode::Angle;
	ThumbnailLocation = Viewport->GetViewLocation();
	ThumbnailRotation = Viewport->GetViewRotation();
	ThumbnailRotation.Roll = 0.0;
	ThumbnailFOV = FMath::Clamp(Viewport->ViewFOV, 10.f, 120.f);
	return true;
}

void AMantlePlaceSettingsActor::UseCurrentView()
{
	SetThumbnailFromViewport();
}

void AMantlePlaceSettingsActor::UseAutomaticView()
{
	const FScopedTransaction Transaction(LOCTEXT("UseAutomaticView", "Use the automatic Mantle thumbnail"));
	Modify();
	ThumbnailChoice.Mode = EMantleThumbnailMode::Automatic;
}

void AMantlePlaceSettingsActor::ApplyTo(UWorld* World, FMantleSettings& Out)
{
	if (const AMantlePlaceSettingsActor* Settings = Find(World))
	{
		Out.MaxPlayers = FMath::Clamp(Settings->MaxPlayers, 1, 100);
		Out.Gravity = Settings->Gravity;
		Out.RespawnMode = RespawnModeName(Settings->RespawnMode);
		Out.bAllowCommunityServers = Settings->bAllowCommunityServers;
		Out.bAllowPrivateServers = Settings->bAllowPrivateServers;
		Out.bCarjacking = Settings->bCarjacking;
		Out.bHealth = Settings->bHealth;
		Out.bCombat = Settings->bCombat;
		Out.Environment = Settings->MakeEnvironmentSettings();
		Out.Placing = Settings->MakePlacingSettings();
		Out.PlayStyle = Settings->MakePlayStyle();
		Out.WorldItems = Settings->MakeWorldItemsSettings();
	}
}

namespace MantlePlaceSettingsPlayStyle
{
	float Clamped(float Value, float Min, float Max, float Default)
	{
		return FMath::IsFinite(Value) ? FMath::Clamp(Value, Min, Max) : Default;
	}

	/**
	 * The play style from the actor's fields, through MantlePlayStyle's own rules, so a value typed past a range (or not a number) lands on
	 * what the runtime would accept. A Side-Scroll Path or Bounds the rules refuse is left out, with the first error in OutError.
	 */
	FMantlePlayStyle Build(const AMantlePlaceSettingsActor& Actor, FString& OutError)
	{
		FMantlePlayStyle Out;
		Out.Style = static_cast<EMantlePlayStyle>(Actor.PlayStyle);
		if (Out.Style == EMantlePlayStyle::TopDown)
		{
			const FMantleTopDownSetting& TopDown = Actor.TopDown;
			const FMantleTopDownOptions Defaults;
			FMantleTopDownOptions& Options = Out.TopDown;
			Options.Distance = Clamped(TopDown.Distance, MantlePlayStyle::MinDistanceLimit, MantlePlayStyle::MaxDistanceLimit, Defaults.Distance);
			Options.MinDistance = Clamped(TopDown.MinDistance, MantlePlayStyle::MinDistanceLimit, MantlePlayStyle::MaxDistanceLimit, Defaults.MinDistance);
			Options.MaxDistance = Clamped(TopDown.MaxDistance, MantlePlayStyle::MinDistanceLimit, MantlePlayStyle::MaxDistanceLimit, Defaults.MaxDistance);
			Options.Pitch = Clamped(TopDown.Pitch, MantlePlayStyle::MinPitch, MantlePlayStyle::MaxPitch, Defaults.Pitch);
			Options.FieldOfView = Clamped(TopDown.FieldOfView, MantlePlayStyle::MinFieldOfView, MantlePlayStyle::MaxFieldOfView, Defaults.FieldOfView);
			FString Ignored;
			if (!MantlePlayStyle::ApplyNumber(Options, TEXT("Yaw"), FMath::IsFinite(TopDown.Yaw) ? TopDown.Yaw : 0.0, Ignored))
			{
				Options.Yaw = Defaults.Yaw;
			}
			Options.bClickToMove = TopDown.bClickToMove;
			Options.bHideWalls = TopDown.bHideWalls;
			MantlePlayStyle::Finish(Options, OutError);
		}
		else if (Out.Style == EMantlePlayStyle::SideScroll)
		{
			const FMantleSideScrollSetting& SideScroll = Actor.SideScroll;
			const FMantleSideScrollOptions Defaults;
			FMantleSideScrollOptions& Options = Out.SideScroll;
			Options.Distance = Clamped(SideScroll.Distance, MantlePlayStyle::MinSideDistance, MantlePlayStyle::MaxSideDistance, Defaults.Distance);
			Options.Height = Clamped(SideScroll.Height, MantlePlayStyle::MinSideHeight, MantlePlayStyle::MaxSideHeight, Defaults.Height);
			Options.LookAhead = Clamped(SideScroll.LookAhead, MantlePlayStyle::MinLookAhead, MantlePlayStyle::MaxLookAhead, Defaults.LookAhead);
			Options.FieldOfView = Clamped(SideScroll.FieldOfView, MantlePlayStyle::MinSideFieldOfView, MantlePlayStyle::MaxSideFieldOfView, Defaults.FieldOfView);
			FString Error;
			if (SideScroll.Path.Num() > 0 && !MantlePlayStyle::ApplySideScrollPath(Options, SideScroll.Path, Error))
			{
				Options.Path.Reset();
				OutError = Error;
			}
			if (SideScroll.bSetBounds)
			{
				if (!MantlePlayStyle::ApplySideScrollBounds(Options, SideScroll.Bounds.Min, SideScroll.Bounds.Max, Error))
				{
					OutError = OutError.IsEmpty() ? Error : OutError;
				}
				else if (!MantlePlayStyle::FinishSideScroll(Options, Error))
				{
					// Bounds without a Path, or past its end: left out, so the rest still publishes as the runtime reads it.
					Options.bHasBounds = false;
					OutError = OutError.IsEmpty() ? Error : OutError;
				}
			}
		}
		return Out;
	}
}

FMantlePlayStyle AMantlePlaceSettingsActor::MakePlayStyle() const
{
	FString Ignored;
	return MantlePlaceSettingsPlayStyle::Build(*this, Ignored);
}

void AMantlePlaceSettingsActor::SetPlayStyleFrom(const FMantlePlayStyle& Style)
{
	PlayStyle = static_cast<EMantlePlacePlayStyle>(Style.Style);
	// Another style's options are the defaults, so switching the place to it later starts from them.
	const FMantleTopDownOptions TopDownOptions = Style.Style == EMantlePlayStyle::TopDown ? Style.TopDown : FMantleTopDownOptions();
	TopDown.Distance = TopDownOptions.Distance;
	TopDown.MinDistance = TopDownOptions.MinDistance;
	TopDown.MaxDistance = TopDownOptions.MaxDistance;
	TopDown.Pitch = TopDownOptions.Pitch;
	TopDown.Yaw = TopDownOptions.Yaw;
	TopDown.FieldOfView = TopDownOptions.FieldOfView;
	TopDown.bClickToMove = TopDownOptions.bClickToMove;
	TopDown.bHideWalls = TopDownOptions.bHideWalls;

	const FMantleSideScrollOptions SideOptions = Style.Style == EMantlePlayStyle::SideScroll ? Style.SideScroll : FMantleSideScrollOptions();
	SideScroll.Path.Reset();
	for (const FVector2D& Point : SideOptions.Path)
	{
		SideScroll.Path.Add(FVector(Point, 0.0));
	}
	SideScroll.Distance = SideOptions.Distance;
	SideScroll.Height = SideOptions.Height;
	SideScroll.LookAhead = SideOptions.LookAhead;
	SideScroll.FieldOfView = SideOptions.FieldOfView;
	SideScroll.bSetBounds = SideOptions.bHasBounds;
	SideScroll.Bounds = FMantleSideScrollBoundsSetting();
	if (SideOptions.bHasBounds)
	{
		SideScroll.Bounds.Min = SideOptions.BoundsMin;
		SideScroll.Bounds.Max = SideOptions.BoundsMax;
	}
}

FString AMantlePlaceSettingsActor::PlayStyleError() const
{
	FString Error;
	MantlePlaceSettingsPlayStyle::Build(*this, Error);
	return Error;
}

// EMantlePlacePlayStyle mirrors the runtime's EMantlePlayStyle member for member.
static_assert(static_cast<int32>(EMantlePlacePlayStyle::TopDown) == static_cast<int32>(EMantlePlayStyle::TopDown),
	"EMantlePlacePlayStyle must list EMantlePlayStyle's members in the same order");
static_assert(static_cast<int32>(EMantlePlacePlayStyle::SideScroll) == static_cast<int32>(EMantlePlayStyle::SideScroll),
	"EMantlePlacePlayStyle must list EMantlePlayStyle's members in the same order");

FMantleEnvironmentSettings AMantlePlaceSettingsActor::MakeEnvironmentSettings() const
{
	// Only what differs from the default is set, so the exporter writes the block only then (#532):
	// clients from before it refuse the key.
	FMantleEnvironmentSettings Out;
	auto Clamped = [](double Value, double Min, double Max, double Default)
	{
		return FMath::IsFinite(Value) ? FMath::Clamp(Value, Min, Max) : Default;
	};
	if (bSetClockTime)
	{
		Out.ClockTime = Clamped(ClockTime, 0.0, 24.0, 12.0);
	}
	const double Scale = Clamped(TimeScale, 0.0, MantleEnvironmentDefaults::MaxScriptTimeScale, 1.0);
	if (Scale != 1.0)
	{
		Out.TimeScale = Scale;
	}
	if (Weather != EMantlePlaceWeather::LevelSky && Weather != EMantlePlaceWeather::Natural)
	{
		Out.Weather = static_cast<EMantleWeatherState>(static_cast<uint8>(Weather) - 2);
	}
	if (bSetSkyLightIntensity)
	{
		Out.SkyLightIntensity = Clamped(SkyLightIntensity, 0.0, 1.0, 1.0);
	}
	if (DirectSun == EMantlePlaceDirectSun::Never)
	{
		Out.bNoDirectSun = true;
	}
	if (Outdoors == EMantlePlaceOutdoors::Interior)
	{
		Out.bNoOutdoors = true;
	}
	const uint32 Tint = (static_cast<uint32>(ColorTint.R) << 16) | (static_cast<uint32>(ColorTint.G) << 8) | ColorTint.B;
	if (Tint != 0xFFFFFF)
	{
		Out.ColorTint = Tint;
	}
	const double Colorfulness = Clamped(Saturation, 0.0, MantleEnvironmentDefaults::MaxScriptSaturation, 1.0);
	if (Colorfulness != 1.0)
	{
		Out.Saturation = Colorfulness;
	}
	if (bSetBloom)
	{
		Out.Bloom = Clamped(Bloom, 0.0, MantleEnvironmentDefaults::MaxScriptBloom, 1.0);
	}
	if (ColorGrading != EMantlePlaceColorGrading::None)
	{
		Out.ColorGrading = static_cast<uint8>(ColorGrading);
	}
	const double GradingIntensity = Clamped(ColorGradingIntensity, 0.0, 1.0, 1.0);
	if (GradingIntensity != 1.0)
	{
		Out.ColorGradingIntensity = GradingIntensity;
	}
	return Out;
}

FMantlePlacingSettings AMantlePlaceSettingsActor::MakePlacingSettings() const
{
	// The defaults are the schema's, so the exporter writes the block only when the creator changed something.
	FMantlePlacingSettings Out;
	Out.Persistence = static_cast<EMantlePlacingPersistence>(PlacedPieces);
	if (bSetMaxPlacedPieces)
	{
		Out.MaxPieces = FMath::Clamp(MaxPlacedPieces, 1, FMantlePlaceLimits::MaxPlacedPieces);
	}
	return Out;
}

void AMantlePlaceSettingsActor::SetPlacingFrom(const FMantlePlacingSettings& Placing)
{
	PlacedPieces = static_cast<EMantlePlacePersistence>(Placing.Persistence);
	bSetMaxPlacedPieces = Placing.MaxPieces != FMantlePlaceLimits::MaxPlacedPieces;
	MaxPlacedPieces = Placing.MaxPieces;
}

FMantleWorldItemsSettings AMantlePlaceSettingsActor::MakeWorldItemsSettings() const
{
	// The default is the schema's (nothing saved), so the exporter writes the block only when the creator turned saving on.
	FMantleWorldItemsSettings Out;
	Out.Persistence = bSaveWorldItems ? EMantleWorldItemsPersistence::Server : EMantleWorldItemsPersistence::None;
	return Out;
}

void AMantlePlaceSettingsActor::SetWorldItemsFrom(const FMantleWorldItemsSettings& WorldItems)
{
	bSaveWorldItems = WorldItems.Persistence == EMantleWorldItemsPersistence::Server;
}

void AMantlePlaceSettingsActor::SetEnvironmentFrom(const FMantleEnvironmentSettings& Environment)
{
	bSetClockTime = Environment.ClockTime.IsSet();
	ClockTime = Environment.ClockTime.Get(12.0);
	TimeScale = Environment.TimeScale.Get(1.0);
	Weather = Environment.Weather.IsSet()
		? static_cast<EMantlePlaceWeather>(static_cast<uint8>(*Environment.Weather) + 2)
		: EMantlePlaceWeather::Natural;
	bSetSkyLightIntensity = Environment.SkyLightIntensity.IsSet();
	SkyLightIntensity = Environment.SkyLightIntensity.Get(1.0);
	DirectSun = Environment.bNoDirectSun ? EMantlePlaceDirectSun::Never : EMantlePlaceDirectSun::Automatic;
	Outdoors = Environment.bNoOutdoors ? EMantlePlaceOutdoors::Interior : EMantlePlaceOutdoors::Automatic;
	const uint32 Tint = Environment.ColorTint.Get(0xFFFFFF);
	ColorTint = FColor(static_cast<uint8>((Tint >> 16) & 0xFF), static_cast<uint8>((Tint >> 8) & 0xFF), static_cast<uint8>(Tint & 0xFF), 255);
	Saturation = Environment.Saturation.Get(1.0);
	bSetBloom = Environment.Bloom.IsSet();
	Bloom = Environment.Bloom.Get(1.0);
	const uint8 Preset = Environment.ColorGrading.Get(MantleEnvironmentOverrideValues::ColorGradingNone);
	ColorGrading = Preset <= MantleEnvironmentOverrideValues::ColorGradingLast
		? static_cast<EMantlePlaceColorGrading>(Preset)
		: EMantlePlaceColorGrading::None;
	ColorGradingIntensity = Environment.ColorGradingIntensity.Get(1.0);
}

// EMantlePlaceWeather is EMantleWeatherState two steps up, LevelSky and Natural first.
static_assert(static_cast<int32>(EMantlePlaceWeather::DustStorm) == static_cast<int32>(MantleLastWeatherState) + 2,
	"EMantlePlaceWeather must list EMantleWeatherState's members in the same order, after LevelSky and Natural");
static_assert(static_cast<int32>(EMantlePlaceWeather::Clear) == static_cast<int32>(EMantleWeatherState::Clear) + 2,
	"EMantlePlaceWeather must list EMantleWeatherState's members in the same order, after LevelSky and Natural");

// EMantlePlaceColorGrading is numbered as the override's presets, None first.
static_assert(static_cast<uint8>(EMantlePlaceColorGrading::None) == MantleEnvironmentOverrideValues::ColorGradingNone,
	"EMantlePlaceColorGrading must number its members as MantleEnvironmentOverrideValues does");
static_assert(static_cast<uint8>(EMantlePlaceColorGrading::Warm) == MantleEnvironmentOverrideValues::ColorGradingWarm,
	"EMantlePlaceColorGrading must number its members as MantleEnvironmentOverrideValues does");
static_assert(static_cast<uint8>(EMantlePlaceColorGrading::Faded) == MantleEnvironmentOverrideValues::ColorGradingLast,
	"EMantlePlaceColorGrading must number its members as MantleEnvironmentOverrideValues does");

void AMantlePlaceSettingsActor::SetFrom(UWorld* World, const FMantleSettings& Settings)
{
	if (AMantlePlaceSettingsActor* Actor = FindOrAdd(World))
	{
		Actor->Modify();
		Actor->MaxPlayers = FMath::Clamp(Settings.MaxPlayers, 1, 100);
		Actor->Gravity = Settings.Gravity;
		Actor->RespawnMode = ParseRespawnMode(Settings.RespawnMode);
		Actor->bAllowCommunityServers = Settings.bAllowCommunityServers;
		Actor->bAllowPrivateServers = Settings.bAllowPrivateServers;
		Actor->bCarjacking = Settings.bCarjacking;
		Actor->bHealth = Settings.bHealth;
		Actor->bCombat = Settings.bCombat;
		Actor->SetEnvironmentFrom(Settings.Environment);
		Actor->SetPlacingFrom(Settings.Placing);
		Actor->SetPlayStyleFrom(Settings.PlayStyle);
		Actor->SetWorldItemsFrom(Settings.WorldItems);
	}
}

// EMantlePlacePersistence mirrors the runtime's EMantlePlacingPersistence member for member.
static_assert(static_cast<int32>(EMantlePlacePersistence::Server) == static_cast<int32>(EMantlePlacingPersistence::Server),
	"EMantlePlacePersistence must list EMantlePlacingPersistence's members in the same order");

// EMantlePlaceMaturity and EMantlePlacePlatform mirror the runtime's EMantleMaturity and
// EMantlePlatform member for member, so a cast maps one onto the other.
static_assert(static_cast<int32>(EMantlePlaceMaturity::Mature) == static_cast<int32>(EMantleMaturity::Mature),
	"EMantlePlaceMaturity must list EMantleMaturity's members in the same order");
static_assert(static_cast<int32>(EMantlePlacePlatform::Android) == static_cast<int32>(EMantlePlatform::Android),
	"EMantlePlacePlatform must list EMantlePlatform's members in the same order");

int32 AMantlePlaceSettingsActor::AllPlatformBits()
{
	return (1 << (static_cast<int32>(EMantlePlacePlatform::Android) + 1)) - 1;
}

void AMantlePlaceSettingsActor::ApplyTo(UWorld* World, FMantleAudience& Out)
{
	if (const AMantlePlaceSettingsActor* Settings = Find(World))
	{
		Out.Maturity = static_cast<EMantleMaturity>(Settings->Maturity);
		Out.Platforms.Reset();
		if (!Settings->bAllPlatforms)
		{
			// An empty selection stays empty here, which reads as every platform; the validator
			// refuses to publish it (HasNoPlatformSelected), so it never silently widens.
			for (int32 Bit = 0; Bit <= static_cast<int32>(EMantlePlacePlatform::Android); ++Bit)
			{
				if (Settings->Platforms & (1 << Bit))
				{
					Out.Platforms.Add(static_cast<EMantlePlatform>(Bit));
				}
			}
		}
	}
}

void AMantlePlaceSettingsActor::SetFrom(UWorld* World, const FMantleAudience& Audience)
{
	if (AMantlePlaceSettingsActor* Actor = FindOrAdd(World))
	{
		Actor->Modify();
		Actor->Maturity = static_cast<EMantlePlaceMaturity>(Audience.Maturity);
		Actor->bAllPlatforms = Audience.Platforms.Num() == 0;
		Actor->Platforms = 0;
		for (const EMantlePlatform Platform : Audience.Platforms)
		{
			Actor->Platforms |= 1 << static_cast<int32>(Platform);
		}
	}
}

void AMantlePlaceSettingsActor::ApplyStorageTo(UWorld* World, TArray<FMantleStorageEntry>& Out)
{
	if (const AMantlePlaceSettingsActor* Settings = Find(World))
	{
		for (const FMantleStorageSetting& Entry : Settings->Storage)
		{
			FMantleStorageEntry Stored;
			Stored.Name = Entry.Name;
			Stored.Item = Entry.Item;
			Stored.Version = Entry.Version;
			Stored.Settings = Entry.Settings; // #838 stage 6
			Out.Add(MoveTemp(Stored));
		}
	}
}

void AMantlePlaceSettingsActor::SetStorageFrom(UWorld* World, const TArray<FMantleStorageEntry>& Storage)
{
	if (AMantlePlaceSettingsActor* Actor = FindOrAdd(World))
	{
		Actor->Modify();
		Actor->ReplaceStorageKeepingKinds(Storage);
	}
}

void AMantlePlaceSettingsActor::ReplaceStorageKeepingKinds(const TArray<FMantleStorageEntry>& Entries, const TMap<FString, FString>& KindByItem)
{
	const TArray<FMantleStorageSetting> Old = Storage;
	Storage.Reset();
	for (const FMantleStorageEntry& Entry : Entries)
	{
		FMantleStorageSetting Setting;
		Setting.Name = Entry.Name;
		Setting.Item = Entry.Item;
		Setting.Version = Entry.Version;
		Setting.Settings = Entry.Settings; // #838 stage 6
		const FMantleStorageSetting* Before = Old.FindByPredicate([&Entry](const FMantleStorageSetting& Candidate)
		{
			return Candidate.Item.Equals(Entry.Item, ESearchCase::CaseSensitive);
		});
		if (Before)
		{
			Setting.Kind = Before->Kind;
			Setting.Manifest = Before->Manifest;
		}
		if (const FString* Kind = KindByItem.Find(Entry.Item))
		{
			Setting.Kind = *Kind;
		}
		Storage.Add(MoveTemp(Setting));
	}
}

const FMantleStorageSetting* AMantlePlaceSettingsActor::FindStorageEntry(const FString& Name) const
{
	return Storage.FindByPredicate([&Name](const FMantleStorageSetting& Entry) { return Entry.Name.Equals(Name, ESearchCase::CaseSensitive); });
}

const FMantleTerrainLayerSetting* AMantlePlaceSettingsActor::FindTerrainLayer(const FName Layer, const FSoftObjectPath& LandscapePath) const
{
	// FName compares without case, as the landscape's own layer names do.
	if (!LandscapePath.IsNull())
	{
		if (const FMantleTerrainLayerSetting* Own = TerrainLayers.FindByPredicate([Layer, &LandscapePath](const FMantleTerrainLayerSetting& Entry)
			{
				return Entry.Layer == Layer && !Entry.Landscape.IsNull() && Entry.Landscape.ToSoftObjectPath() == LandscapePath;
			}))
		{
			return Own;
		}
	}
	return TerrainLayers.FindByPredicate([Layer](const FMantleTerrainLayerSetting& Entry) { return Entry.Layer == Layer && Entry.Landscape.IsNull(); });
}

const FMantleTerrainLayerSetting* AMantlePlaceSettingsActor::FirstTerrainLayer(const FSoftObjectPath& LandscapePath) const
{
	return TerrainLayers.FindByPredicate([&LandscapePath](const FMantleTerrainLayerSetting& Entry) { return Entry.Material && Entry.AppliesTo(LandscapePath); });
}

const TCHAR* AMantlePlaceSettingsActor::RespawnModeName(EMantleRespawnMode Mode)
{
	switch (Mode)
	{
	case EMantleRespawnMode::Random: return TEXT("random");
	case EMantleRespawnMode::First:  return TEXT("first");
	default:                         return TEXT("nearest");
	}
}

EMantleRespawnMode AMantlePlaceSettingsActor::ParseRespawnMode(const FString& Name)
{
	if (Name == TEXT("random"))
	{
		return EMantleRespawnMode::Random;
	}
	if (Name == TEXT("first"))
	{
		return EMantleRespawnMode::First;
	}
	return EMantleRespawnMode::Nearest;
}

#undef LOCTEXT_NAMESPACE
