// Copyright Joshua Eger. All Rights Reserved.
#include "MantleShadowDistance.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"

namespace MantleShadowDistancePrivate
{
	/** Seconds from the start of one pass over every tracked mesh to the start of the next. */
	constexpr double ScanIntervalSeconds = 0.25;

	/** Meshes looked at in one tick, and shadows switched in one (each rebuilds that mesh's render state). */
	constexpr int32 MaxChecksPerTick = 4096;
	constexpr int32 MaxTogglesPerTick = 64;

	/** A mesh whose shadow is off casts again only within this share of its shadow distance. */
	constexpr double ReturnShare = 0.9;

	TAutoConsoleVariable<float> CVarShadowDistanceFraction(
		TEXT("mantle.Place.ShadowDistanceFraction"), 0.5f,
		TEXT("Place meshes with a drawDistance (#630): the share of it they cast shadows to. 1 or more, or 0 or less, leaves their shadows on to the draw distance."),
		ECVF_Default);
}

double UMantleShadowDistanceSubsystem::ShadowDistanceFor(const double DrawDistance)
{
	const double Fraction = MantleShadowDistancePrivate::CVarShadowDistanceFraction.GetValueOnGameThread();
	if (!(Fraction > 0.0 && Fraction < 1.0) || DrawDistance <= 0.0)
	{
		return 0.0;
	}
	return DrawDistance * Fraction;
}

bool UMantleShadowDistanceSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	// Only where something draws, in a played world: an editor world is saved, and its shadows must stay as authored.
	const UWorld* World = Cast<UWorld>(Outer);
	return World && (World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE) && !IsRunningDedicatedServer();
}

void UMantleShadowDistanceSubsystem::Track(UPrimitiveComponent* Component, const double DrawDistance)
{
	if (Component && DrawDistance > 0.0)
	{
		Entries.Add(FEntry{ Component, DrawDistance, /*bShadow*/ true });
	}
}

bool UMantleShadowDistanceSubsystem::IsTickable() const
{
	return Entries.Num() > 0;
}

void UMantleShadowDistanceSubsystem::Tick(const float DeltaTime)
{
	using namespace MantleShadowDistancePrivate;
	Super::Tick(DeltaTime);

	UWorld* World = GetWorld();
	const APlayerController* Controller = (World && GEngine) ? GEngine->GetFirstLocalPlayerController(World) : nullptr;
	if (!Controller || Entries.IsEmpty())
	{
		return;
	}

	const double Now = FPlatformTime::Seconds();
	if (Cursor == 0)
	{
		if (Now < PassStartSeconds + ScanIntervalSeconds)
		{
			return;
		}
		PassStartSeconds = Now;
	}

	FVector View = FVector::ZeroVector;
	FRotator Rotation = FRotator::ZeroRotator;
	Controller->GetPlayerViewPoint(View, Rotation);

	int32 Checked = 0;
	int32 Toggled = 0;
	while (Cursor < Entries.Num() && Checked < MaxChecksPerTick && Toggled < MaxTogglesPerTick)
	{
		FEntry& Entry = Entries[Cursor];
		UPrimitiveComponent* Component = Entry.Component.Get();
		if (!Component)
		{
			// The last entry takes this one's place, and is looked at next.
			Entries.RemoveAtSwap(Cursor, EAllowShrinking::No);
			continue;
		}
		++Checked;

		bool bShadow = Entry.bShadow;
		const double ShadowDistance = ShadowDistanceFor(Entry.DrawDistance);
		if (ShadowDistance <= 0.0)
		{
			bShadow = true;
		}
		else
		{
			// From the nearest point of the bounds: an instance set casts while any of its instances is near.
			const double DistanceSquared = Component->Bounds.GetBox().ComputeSquaredDistanceToPoint(View);
			if (Entry.bShadow && DistanceSquared > FMath::Square(ShadowDistance))
			{
				bShadow = false;
			}
			else if (!Entry.bShadow && DistanceSquared < FMath::Square(ShadowDistance * ReturnShare))
			{
				bShadow = true;
			}
		}
		if (bShadow != Entry.bShadow)
		{
			Entry.bShadow = bShadow;
			Component->SetCastShadow(bShadow);
			++Toggled;
		}
		++Cursor;
	}
	if (Cursor >= Entries.Num())
	{
		Cursor = 0;
	}
}
