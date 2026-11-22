// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/**
 * The compressed cooked formats' unpacking (#189 steps 5 and 5a; writers in backend/internal/cook/packed.go):
 * MTX3's per-mip streams, and the envelope MRM4 and MLD3 put around an MRM2 or an MLD1:
 *
 *   magic     [4]  "MRM4" or "MLD3"
 *   rawBytes  u32  the inner file's size, 1 to the reader's cap
 *   stream    ...  one Oodle Kraken stream of the inner file, to the end of the file
 *
 * The cook packs with Oodle 2.9.16 at the engine's default options, so the engine's own
 * FOodleDataCompression::Decompress reads it (Core links Oodle on every platform).
 *
 * The bytes arrive over the network, so the size is checked against the cap before anything is allocated,
 * and a stream is refused unless it unpacks to the size named. A Kraken stream carries no checksum (the
 * engine's encoder writes none): the asset store checks every file's BLAKE3 before it is stored, which is
 * what catches a changed byte.
 *
 * No UObjects, no asserts, no logging of our own: any thread. Named namespace: MantleRuntime builds as a unity build.
 */
namespace MantleCookedPacking
{
	/** The envelope's header: magic and size. */
	constexpr int32 EnvelopeHeaderBytes = 8;

	/** Unpacks one Kraken stream into Out, which must hold exactly Raw bytes. False with the reason (a phrase after "is") on anything else. */
	bool UnpackStream(TConstArrayView<uint8> Stream, uint8* Out, int64 Raw, FString* OutReason = nullptr);

	/** True when Bytes start with Magic (four characters). */
	bool HasMagic(TConstArrayView<uint8> Bytes, const ANSICHAR* Magic);

	/** Unpacks an envelope with Magic into Out (emptied first). False with the reason, Out empty, on anything refused, a size past MaxRaw included. */
	bool UnpackEnvelope(TConstArrayView<uint8> Bytes, const ANSICHAR* Magic, int64 MaxRaw, TArray<uint8>& Out, FString* OutReason = nullptr);

	/** Packs Raw as one Kraken stream the way the cook does (tests build files with it). Empty on failure. */
	TArray<uint8> PackStream(TConstArrayView<uint8> Raw);
}
