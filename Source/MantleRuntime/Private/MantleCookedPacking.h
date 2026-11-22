// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/**
 * The compressed cooked formats' inflate (#189 step 5; writers in backend/internal/cook/packed.go):
 * MTX2's per-mip zlib streams, and the envelope MRM3 and MLD2 put around an MRM2 or an MLD1, which is
 * the cell file's container (MCF1):
 *
 *   magic     [4]  "MRM3" or "MLD2"
 *   rawBytes  u32  the inner file's size, 1 to the reader's cap
 *   stream    ...  one zlib stream (RFC 1950) of the inner file, to the end of the file
 *
 * The bytes arrive over the network, so the size is checked against the cap before anything is
 * allocated, and a stream is refused unless it is whole (its Adler-32 matches), inflates to exactly the
 * size named, and ends exactly where the bytes do: the same checks as the Nanite side file's pieces.
 *
 * No UObjects, no asserts, no logging: any thread. Named namespace: MantleRuntime builds as a unity build.
 */
namespace MantleCookedPacking
{
	/** The envelope's header: magic and size. */
	constexpr int32 EnvelopeHeaderBytes = 8;

	/** Inflates one whole zlib stream into Out, which must hold exactly Raw bytes. False with the reason (a phrase after "is") on anything else. */
	bool InflateExact(TConstArrayView<uint8> Stream, uint8* Out, int64 Raw, FString* OutReason = nullptr);

	/** True when Bytes start with Magic (four characters). */
	bool HasMagic(TConstArrayView<uint8> Bytes, const ANSICHAR* Magic);

	/** Inflates an envelope with Magic into Out (emptied first). False with the reason, Out empty, on anything refused, a size past MaxRaw included. */
	bool UnpackEnvelope(TConstArrayView<uint8> Bytes, const ANSICHAR* Magic, int64 MaxRaw, TArray<uint8>& Out, FString* OutReason = nullptr);
}
