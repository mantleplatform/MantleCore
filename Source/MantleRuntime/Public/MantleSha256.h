// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/**
 * SHA-256 (FIPS 180-4) of a byte buffer: the content address the Mantle API gives every upload
 * (backend assets.Hash, "sha256:" + 64 lower-case hex digits). The plugin hashes what it is about
 * to upload so POST /assets/diff can say which bytes the creator already holds a claim on, and
 * only the rest are sent.
 *
 * Its own implementation because the engine's FPlatformMisc::GetSHA256Signature is not
 * implemented on Windows (the generic version asserts), and no dependency is added for it.
 */
struct MANTLERUNTIME_API FMantleSha256
{
	/** The 32-byte digest of Size bytes at Data. */
	static void Hash(const uint8* Data, int64 Size, uint8 OutDigest[32]);

	/** The digest as 64 lower-case hex digits. */
	static FString HexDigest(TConstArrayView<uint8> Bytes);

	/** "sha256:" + HexDigest, the form the API stores and POST /assets/diff takes. */
	static FString AssetHash(TConstArrayView<uint8> Bytes);
};
