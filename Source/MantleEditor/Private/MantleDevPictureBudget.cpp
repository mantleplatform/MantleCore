// Copyright Joshua Eger. All Rights Reserved.
#include "MantleDevPictureBudget.h"

#include "HAL/PlatformMemory.h"
#include "RHIGlobals.h"
#include "RenderingThread.h"
#include "UObject/GarbageCollection.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY_STATIC(LogMantleDevPictureBudget, Log, All);

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleDevPictureBudgetPrivate
{
	/** Pictures this launch has taken. */
	int32 GPicturesTaken = 0;

	/** The reserved virtual size when this launch's first picture began, or -1 before it. */
	int64 GReservedAtFirst = -1;

	/** Set once the cap is reached, so every later command stops too. */
	bool GReached = false;

	constexpr double BytesPerGB = 1024.0 * 1024.0 * 1024.0;

	int64 ReservedNow()
	{
		return static_cast<int64>(GRHIGlobals.ReservedResources.VirtualSize);
	}

	void LogPictureMemory(const TCHAR* Command, const TCHAR* When, const FString& Picture)
	{
		const FPlatformMemoryStats Stats = FPlatformMemory::GetStats();
		UE_LOG(LogMantleDevPictureBudget, Display, TEXT("MANTLE_PICTURE_MEMORY %s %s picture=%d reservedGB=%.2f physicalGB=%.2f %s"), Command, When,
			GPicturesTaken + 1, static_cast<double>(ReservedNow()) / BytesPerGB, static_cast<double>(Stats.UsedPhysical) / BytesPerGB, *Picture);
	}
}

bool FMantleDevPictureBudget::Allows(const TCHAR* Command, const FString& What)
{
	using namespace MantleDevPictureBudgetPrivate;
	if (GReservedAtFirst < 0)
	{
		GReservedAtFirst = ReservedNow();
	}
	const int64 Growth = ReservedNow() - GReservedAtFirst;
	if (!GReached && (GPicturesTaken >= MaxPicturesPerLaunch || Growth >= MaxReservedGrowthGB * 1024 * 1024 * 1024))
	{
		GReached = true;
		UE_LOG(LogMantleDevPictureBudget, Warning, TEXT("MANTLE_PICTURE_CAP reached: %d pictures this launch (cap %d), reserved GPU memory grown %.2f GB (cap %lld GB); ")
			TEXT("every later picture is skipped, relaunch the editor for the rest"), GPicturesTaken, MaxPicturesPerLaunch, static_cast<double>(Growth) / BytesPerGB,
			static_cast<long long>(MaxReservedGrowthGB));
	}
	if (GReached)
	{
		UE_LOG(LogMantleDevPictureBudget, Warning, TEXT("MANTLE_PICTURE_CAP %s skipped %s"), Command, *What);
		return false;
	}
	return true;
}

bool FMantleDevPictureBudget::Begin(const TCHAR* Command, const FString& Picture)
{
	using namespace MantleDevPictureBudgetPrivate;
	if (!Allows(Command, Picture))
	{
		return false;
	}
	LogPictureMemory(Command, TEXT("before"), Picture);
	return true;
}

void FMantleDevPictureBudget::End(const TCHAR* Command, const FString& Picture)
{
	using namespace MantleDevPictureBudgetPrivate;
	LogPictureMemory(Command, TEXT("after"), Picture);
	++GPicturesTaken;
}

void FMantleDevPictureBudget::CollectAfterCommand(const TCHAR* Command)
{
	using namespace MantleDevPictureBudgetPrivate;
	FlushRenderingCommands();
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	FlushRenderingCommands();
	const FPlatformMemoryStats Stats = FPlatformMemory::GetStats();
	UE_LOG(LogMantleDevPictureBudget, Display, TEXT("MANTLE_PICTURE_MEMORY %s collected reservedGB=%.2f physicalGB=%.2f"), Command,
		static_cast<double>(ReservedNow()) / BytesPerGB, static_cast<double>(Stats.UsedPhysical) / BytesPerGB);
}
