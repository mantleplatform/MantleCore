// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/**
 * #804: the transient meshes a publish makes (the mirror bake's, the spline mesh bake's and the reduction's copies) are kept
 * alive by caches on purpose, so Gather, Check and Export see one object for each. Nothing but the tests let them go, so
 * they lived until the editor closed. A publish or a preview export is "active" from its start to its end; when the last
 * active one ends, the caches are dropped and the next publish bakes what it needs again. Never mid-run, and never while
 * another publish or preview is still using them. Game thread only.
 */
class MANTLEEDITOR_API FMantlePublishCaches
{
public:
	/** A publish or preview export begins. */
	static void Enter();

	/** It ends. The last one to end prunes the caches. */
	static void Leave();

	/** How many are running. */
	static int32 NumActive();

	/** Tests: forgets every Enter, so a test that counts starts from none whatever an earlier one left. */
	static void ResetCountForTests();

	/** Drops the mirror bake's, the spline mesh bake's and the mesh reduction's cached copies now. */
	static void Prune();

	/** Enter on construction, Leave on destruction (every way out of a function). */
	struct MANTLEEDITOR_API FScope
	{
		FScope() { FMantlePublishCaches::Enter(); }
		~FScope() { FMantlePublishCaches::Leave(); }
		UE_NONCOPYABLE(FScope);
	};
};
