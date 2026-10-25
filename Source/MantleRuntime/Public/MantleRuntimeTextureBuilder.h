// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Engine/TextureDefines.h"
#include "PixelFormat.h"

class UObject;
class UTexture2D;

/**
 * What a texture is for, which decides how its bytes are read: colour is sRGB, everything else is
 * linear. Shared by avatar items (A2.3) and place materials (Fab F8: base colour and emissive are
 * Color, the normal map Normal, ORM Data).
 */
enum class EMantleTextureRole : uint8
{
	/** sRGB colour (base colour, emissive, hair atlases), alpha linear. Mips average in linear light. */
	Color,

	/** A tangent-space normal map, linear. Mips average the decoded normals and renormalise them. */
	Normal,

	/** Linear data (ORM, masks, coverage). Mips average each channel. */
	Data,
};

/** Whether the builder gives a texture a mip chain. */
enum class EMantleTextureMips : uint8
{
	/** The top level only (UI images, anything shown at its own size). */
	None,

	/** A box-filtered chain down to 1x1 for a decoded image; a block container's own chain as it comes. */
	Generate,
};

struct FMantleTextureBuildParams
{
	EMantleTextureRole Role = EMantleTextureRole::Color;
	EMantleTextureMips Mips = EMantleTextureMips::Generate;

	/**
	 * Largest width or height accepted, checked from the image header before anything is decoded. The
	 * catalog's texture limit is 2048 (AVATAR_CATALOG_PLAN.md section 3.4).
	 */
	int32 MaxDimension = 2048;

	/**
	 * Largest GPU footprint accepted for this texture, every mip counted (ComputeGpuBytes). The default
	 * holds a 2048 x 2048 BGRA8 chain (about 21.3 MiB). A caller with an item or place budget passes what
	 * is left of it.
	 */
	int64 MaxGpuBytes = 24ll * 1024 * 1024;

	/** Sampler addressing and filtering (hair atlases want clamp). */
	TEnumAsByte<TextureAddress> AddressX = TA_Wrap;
	TEnumAsByte<TextureAddress> AddressY = TA_Wrap;
	TEnumAsByte<TextureFilter> Filter = TF_Default;

	/** Texture group (LOD bias and sampler settings come from it); characters use TEXTUREGROUP_Character. */
	TEnumAsByte<TextureGroup> LODGroup = TEXTUREGROUP_World;

	/** Outer of the new texture; the transient package when null. */
	UObject* Outer = nullptr;

	/** Name of the new texture; a unique generated one when None. */
	FName Name;
};

/** One mip level as the GPU takes it. */
struct FMantleTextureMip
{
	int32 Width = 0;
	int32 Height = 0;

	/** Exactly FMantleRuntimeTextureBuilder::ComputeMipBytes(Format, Width, Height) bytes. */
	TArray64<uint8> Bytes;
};

/**
 * A texture decoded on the CPU and ready for upload: the half of the build that needs no UObject, so
 * it can run on a worker thread. PF_B8G8R8A8 decoded from PNG/JPEG, or the BC1/BC3/BC4/BC5 blocks and
 * mips of an MTX1 container (Fab F9, #343) copied as they are, without a CPU decode. Build uploads
 * either the same way.
 */
struct MANTLERUNTIME_API FMantleDecodedTexture
{
	EPixelFormat Format = PF_Unknown;

	/** Colour (Role Color) is sampled as sRGB. */
	bool bSRGB = false;

	EMantleTextureRole Role = EMantleTextureRole::Color;

	/** Mip 0 first; each level half the previous one (rounded down, at least 1). */
	TArray<FMantleTextureMip> Mips;

	/**
	 * The uploaded image's size before the cook worker padded it to whole 4 x 4 blocks (MTX1: mip 0 is
	 * up to 3 texels wider and taller; UVs span the padded size). Mip 0's size for PNG and JPEG.
	 */
	int32 SourceWidth = 0;
	int32 SourceHeight = 0;

	int32 GetWidth() const { return Mips.Num() > 0 ? Mips[0].Width : 0; }
	int32 GetHeight() const { return Mips.Num() > 0 ? Mips[0].Height : 0; }

	/** Every mip's bytes: the texture's GPU footprint. */
	int64 GetGpuBytes() const;
};

/**
 * Turns downloaded image bytes into a transient UTexture2D at runtime (AVATAR_CATALOG_PLAN.md A2.3;
 * shared with the Fab track's place materials, F8 #342, and extended by F9 #343 with BC blocks).
 *
 *   Decode(bytes, params)   Any thread. The format is sniffed from the bytes, never trusted from a name.
 *                           MTX1 (F9's cooked BC container, BACKEND.md "The MTX1 format"): every header
 *                           field, mip size and offset checked against the format's rules and the
 *                           buffer before anything is allocated, then the blocks copied per mip; the
 *                           pixel format and role come from the file (BC1/BC3 sRGB: Color; BC1/BC3
 *                           linear and BC4: Data; BC5: Normal), and Params.Role is not consulted. PNG or
 *                           JPEG (older ids, UI images): dimensions checked from the header against
 *                           MaxDimension before the pixels are decoded, then decoded to BGRA8 and mipped
 *                           per Params.Role. Either way the footprint is checked against MaxGpuBytes.
 *   Build(decoded, params)  Game thread. The mips become a never-streamed transient UTexture2D, sRGB or
 *                           linear per role, with its render resource created. Accepts BGRA8 and the
 *                           block formats MTX1 carries, checking every mip's size for its format.
 *
 * A dedicated server never builds textures (nothing renders there; F8 skips them): Build refuses on one.
 * Failures return false/null with the reason; nothing asserts on bad bytes.
 */
class MANTLERUNTIME_API FMantleRuntimeTextureBuilder
{
public:
	/** Pixel formats Build uploads: BGRA8 today, the BC formats F9's MTX1 container carries. */
	static bool IsSupportedFormat(EPixelFormat Format);

	/** Bytes of one mip of Format at Width x Height (block formats round up to whole blocks); -1 for an unsupported format. */
	static int64 ComputeMipBytes(EPixelFormat Format, int32 Width, int32 Height);

	/** Bytes of a Format texture with NumMips levels from Width x Height down. */
	static int64 ComputeGpuBytes(EPixelFormat Format, int32 Width, int32 Height, int32 NumMips);

	/** Levels in a full chain from Width x Height down to 1 x 1. */
	static int32 ComputeFullMipCount(int32 Width, int32 Height);

	/** Worker-thread half: bytes to CPU mips. False, with the reason in OutError and Out left empty, on anything it refuses. */
	static bool Decode(TConstArrayView<uint8> Bytes, const FMantleTextureBuildParams& Params, FMantleDecodedTexture& Out,
		FString* OutError = nullptr);

	/**
	 * Game-thread half: the decoded mips to a texture. Decoded's mip bytes are moved into the texture's
	 * bulk data (Decoded is left empty). Null, with the reason, on a dedicated server, for an unsupported
	 * format, a malformed mip chain, or a footprint above Params.MaxGpuBytes.
	 */
	static UTexture2D* Build(FMantleDecodedTexture&& Decoded, const FMantleTextureBuildParams& Params, FString* OutError = nullptr);

	/** Both halves on the calling (game) thread. */
	static UTexture2D* BuildFromBytes(TConstArrayView<uint8> Bytes, const FMantleTextureBuildParams& Params, FString* OutError = nullptr);
};
