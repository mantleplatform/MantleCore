// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/**
 * The engine tag the Nanite (MND1) and Lumen (MLD1) side files carry: the engine release their serialized layout belongs to.
 *
 * It is "Major.Minor.Patch" ("5.8.3"). The changelist is not part of it: the same release is built with changelist 58210709 by Epic's
 * launcher and with changelist 0 from a source checkout (the release builder's engine), and both write and read the same layout.
 * A file written by an older plugin says "5.8.3-58210709" (the changelist after a dash); that is read as 5.8.3.
 */
namespace MantleEngineTag
{
	/** This engine's tag: "Major.Minor.Patch" of FEngineVersion::Current. */
	MANTLERUNTIME_API FString Current();

	/** The release a tag names: "M.m.p" or "M.m.p-CL" (CL digits only). False for anything else (empty, a missing part, a sign, a letter, extra parts). */
	MANTLERUNTIME_API bool Parse(const FString& Tag, int32& OutMajor, int32& OutMinor, int32& OutPatch);

	/** Whether a file's tag and this engine's tag name the same Major.Minor.Patch. A tag that does not parse never matches. */
	MANTLERUNTIME_API bool Matches(const FString& FileTag, const FString& ThisTag);
}
