// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePublishCaches.h"
#include "MantleMeshReduce.h"
#include "MantleMirrorBake.h"
#include "MantleSplineMeshBake.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantlePublishCachesPrivate
{
	int32 ActiveCount = 0;
}

void FMantlePublishCaches::Enter()
{
	++MantlePublishCachesPrivate::ActiveCount;
}

void FMantlePublishCaches::Leave()
{
	if (MantlePublishCachesPrivate::ActiveCount <= 0)
	{
		return;
	}
	if (--MantlePublishCachesPrivate::ActiveCount == 0)
	{
		Prune();
	}
}

int32 FMantlePublishCaches::NumActive()
{
	return MantlePublishCachesPrivate::ActiveCount;
}

void FMantlePublishCaches::ResetCountForTests()
{
	MantlePublishCachesPrivate::ActiveCount = 0;
}

void FMantlePublishCaches::Prune()
{
	FMantleMirrorBake::ClearCache();
	FMantleSplineMeshBake::ClearCache();
	FMantleMeshReduce::ClearCache();
}
