// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCookedPacking.h"

#include "Compression/OodleDataCompression.h"

namespace MantleCookedPacking
{
	namespace Detail
	{
		bool Refuse(FString* OutReason, const FString& Reason)
		{
			if (OutReason)
			{
				*OutReason = Reason;
			}
			return false;
		}
	}

	bool UnpackStream(const TConstArrayView<uint8> Stream, uint8* Out, const int64 Raw, FString* OutReason)
	{
		if (Stream.Num() <= 0 || Raw <= 0)
		{
			return Detail::Refuse(OutReason, FString::Printf(TEXT("damaged (a %lld-byte Kraken stream for %lld bytes)"), static_cast<int64>(Stream.Num()), Raw));
		}
		// The engine's decoder: it refuses a stream it cannot read or that holds fewer than Raw bytes.
		if (!FOodleDataCompression::Decompress(Out, Raw, Stream.GetData(), Stream.Num()))
		{
			return Detail::Refuse(OutReason, FString::Printf(TEXT("damaged (its Kraken stream does not unpack to %lld bytes)"), Raw));
		}
		return true;
	}

	bool HasMagic(const TConstArrayView<uint8> Bytes, const ANSICHAR* Magic)
	{
		return Bytes.Num() >= 4 && FMemory::Memcmp(Bytes.GetData(), Magic, 4) == 0;
	}

	namespace Detail
	{
		/** The envelope's size word, or 0 when Bytes is not an envelope with Magic or the size is 0 or past MaxRaw (or an int32). */
		uint32 EnvelopeRaw(const TConstArrayView<uint8> Bytes, const ANSICHAR* Magic, const int64 MaxRaw)
		{
			if (Bytes.Num() < EnvelopeHeaderBytes || !HasMagic(Bytes, Magic))
			{
				return 0;
			}
			const uint8* Size = Bytes.GetData() + 4;
			const uint32 Raw = static_cast<uint32>(Size[0]) | (static_cast<uint32>(Size[1]) << 8) | (static_cast<uint32>(Size[2]) << 16) | (static_cast<uint32>(Size[3]) << 24);
			return static_cast<int64>(Raw) > FMath::Min<int64>(MaxRaw, MAX_int32) ? 0 : Raw;
		}
	}

	bool UnpackEnvelope(const TConstArrayView<uint8> Bytes, const ANSICHAR* Magic, const int64 MaxRaw, TArray<uint8>& Out, FString* OutReason)
	{
		Out.Reset();
		if (Bytes.Num() < EnvelopeHeaderBytes || !HasMagic(Bytes, Magic))
		{
			return Detail::Refuse(OutReason, FString::Printf(TEXT("not an %hs"), Magic));
		}
		// The size word is checked before anything is allocated: a damaged or hostile one must not take memory.
		const uint32 Raw = Detail::EnvelopeRaw(Bytes, Magic, MaxRaw);
		if (Raw == 0)
		{
			return Detail::Refuse(OutReason, FString::Printf(TEXT("an %hs that says it unpacks to nothing or past %lld bytes"), Magic, FMath::Min<int64>(MaxRaw, MAX_int32)));
		}
		Out.SetNumUninitialized(static_cast<int32>(Raw));
		if (!UnpackStream(Bytes.RightChop(EnvelopeHeaderBytes), Out.GetData(), Raw, OutReason))
		{
			Out.Reset();
			return false;
		}
		return true;
	}

	TArray<uint8> PackStream(const TConstArrayView<uint8> Raw)
	{
		TArray<uint8> Packed;
		if (Raw.Num() <= 0)
		{
			return Packed;
		}
		Packed.SetNumUninitialized(static_cast<int32>(FOodleDataCompression::CompressedBufferSizeNeeded(Raw.Num())));
		const int64 Size = FOodleDataCompression::Compress(Packed.GetData(), Packed.Num(), Raw.GetData(), Raw.Num(),
			FOodleDataCompression::ECompressor::Kraken, FOodleDataCompression::ECompressionLevel::Optimal1);
		Packed.SetNum(Size > 0 ? static_cast<int32>(Size) : 0);
		return Packed;
	}
}
