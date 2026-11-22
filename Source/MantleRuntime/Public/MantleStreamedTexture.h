// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Engine/TextureMipDataProviderFactory.h"
#include "MantleAssetStore.h"
#include "PixelFormat.h"

#include "MantleStreamedTexture.generated.h"

/**
 * #189: a downloaded file pinned in the asset store for as long as anything shares this (a streamed texture's mip table, so its factory and every stream-in reading
 * it): the file is never evicted under a texture that still reads from it. Pin and Unpin are counted and locked (FMantleAssetStore), so the last holder may be on any thread.
 */
class MANTLERUNTIME_API FMantleFilePin
{
public:
	FMantleFilePin(const TSharedRef<FMantleAssetStore>& InStore, const FMantleContentHash& InHash);
	~FMantleFilePin();
	FMantleFilePin(const FMantleFilePin&) = delete;
	FMantleFilePin& operator=(const FMantleFilePin&) = delete;

private:
	TSharedRef<FMantleAssetStore> Store;
	FMantleContentHash Hash;
};

/** One mip of an MTX2 file: its size, where its zlib stream starts, how many bytes it inflates to, and how many it takes in the file. */
struct FMantleTextureFileMip
{
	int32 Width = 0;
	int32 Height = 0;
	int64 Offset = 0;
	int64 Size = 0;
	int64 Packed = 0;
};

/**
 * #189 step 6 (trial): where a streamed texture's mips are. The MTX2 file in the download cache and its mip table, no texel bytes. The texture's mip 0
 * is the file's FirstMip (the Textures setting drops the top ones).
 */
struct FMantleTextureFileTable
{
	FString Path;
	int64 FileSize = 0;
	EPixelFormat Format = PF_Unknown;
	TArray<FMantleTextureFileMip> Mips;
	int32 FirstMip = 0;

	/** The file's pin in the asset store, given back when the table's last holder goes; null when the file is not the store's (tests). */
	TSharedPtr<FMantleFilePin> Pin;
};

/** What the cached-mip provider has done since the process began (or ResetStats). */
struct FMantleStreamedMipStats
{
	/** Mips the engine's streamer asked for and got, the bytes they inflated to, and the zlib bytes read from the files for them. */
	int64 MipsServed = 0;
	int64 BytesServed = 0;
	int64 PackedBytesRead = 0;

	/** Stream-in requests that failed (a file gone, short, or a stream that did not inflate to its size): the engine cancels the update and keeps what it had. */
	int64 Failed = 0;

	/** Stream-in requests answered, and the time they took on the worker, in milliseconds. */
	int64 Requests = 0;
	double MeanRequestMs = 0.0;
	double WorstRequestMs = 0.0;
};

/**
 * #189 step 6 (trial): the engine's texture streamer reads a runtime texture's top mips from its MTX2 file in the download cache. Attached to the texture
 * as asset user data (the engine's own hook, UTextureMipDataProviderFactory): when the streamer wants more mips, the update asks this for them, and the
 * provider reads each mip's zlib stream from the file and inflates it into the engine's buffer, on the update's worker thread. Only the tail (the engine's
 * non-streaming mips) is in memory when the texture is built; the streamer drops and re-reads the top mips as the camera moves.
 */
UCLASS(MinimalAPI, Transient)
class UMantleCachedMipsFactory : public UTextureMipDataProviderFactory
{
	GENERATED_BODY()

public:
	TSharedPtr<const FMantleTextureFileTable> Table;

	virtual FTextureMipDataProvider* AllocateMipDataProvider(UTexture* Asset) override;
	virtual bool WillProvideMipDataWithoutDisk() const override { return true; }

	static MANTLERUNTIME_API FMantleStreamedMipStats GetStats();
	static MANTLERUNTIME_API void ResetStats();
};
