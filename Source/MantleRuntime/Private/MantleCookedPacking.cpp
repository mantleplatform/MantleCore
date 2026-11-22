// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCookedPacking.h"

THIRD_PARTY_INCLUDES_START
#include "zlib.h"
THIRD_PARTY_INCLUDES_END

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

	bool InflateExact(const TConstArrayView<uint8> Stream, uint8* Out, const int64 Raw, FString* OutReason)
	{
		// zlib counts in uInt: a stream or a result past 4 GiB is not one the cook writes.
		if (Stream.Num() <= 0 || Raw <= 0 || static_cast<uint64>(Stream.Num()) > MAX_uint32 || static_cast<uint64>(Raw) > MAX_uint32)
		{
			return Detail::Refuse(OutReason, FString::Printf(TEXT("damaged (a %lld-byte zlib stream for %lld bytes)"), static_cast<int64>(Stream.Num()), Raw));
		}
		z_stream Inflater;
		FMemory::Memzero(Inflater);
		Inflater.next_in = const_cast<Bytef*>(Stream.GetData());
		Inflater.avail_in = static_cast<uInt>(Stream.Num());
		Inflater.next_out = reinterpret_cast<Bytef*>(Out);
		Inflater.avail_out = static_cast<uInt>(Raw);
		if (inflateInit(&Inflater) != Z_OK)
		{
			return Detail::Refuse(OutReason, TEXT("not inflated (zlib did not start)"));
		}
		// One call with Z_FINISH: Z_STREAM_END only when the whole stream was read and its Adler-32 matched; output past Raw or input that stops early is Z_BUF_ERROR.
		const int Result = inflate(&Inflater, Z_FINISH);
		const uint64 TotalIn = Inflater.total_in;
		const uint64 TotalOut = Inflater.total_out;
		inflateEnd(&Inflater);
		if (Result != Z_STREAM_END)
		{
			return Detail::Refuse(OutReason, TEXT("damaged (its zlib stream is cut short, damaged, or inflates to more than it says)"));
		}
		if (TotalIn != static_cast<uint64>(Stream.Num()))
		{
			return Detail::Refuse(OutReason, FString::Printf(TEXT("damaged (%llu bytes follow the end of its zlib stream)"), static_cast<uint64>(Stream.Num()) - TotalIn));
		}
		if (TotalOut != static_cast<uint64>(Raw))
		{
			return Detail::Refuse(OutReason, FString::Printf(TEXT("damaged (its zlib stream inflates to %llu bytes, it says %lld)"), TotalOut, Raw));
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
			return Detail::Refuse(OutReason, FString::Printf(TEXT("an %hs that says it inflates to nothing or past %lld bytes"), Magic, FMath::Min<int64>(MaxRaw, MAX_int32)));
		}
		Out.SetNumUninitialized(static_cast<int32>(Raw));
		if (!InflateExact(Bytes.RightChop(EnvelopeHeaderBytes), Out.GetData(), Raw, OutReason))
		{
			Out.Reset();
			return false;
		}
		return true;
	}

	bool UnpackEnvelopePrefix(const TConstArrayView<uint8> Bytes, const ANSICHAR* Magic, const int64 MaxRaw, const int32 PrefixBytes, TArray<uint8>& Out)
	{
		Out.Reset();
		const uint32 Raw = Detail::EnvelopeRaw(Bytes, Magic, MaxRaw);
		if (Raw == 0 || PrefixBytes <= 0)
		{
			return false;
		}
		const TConstArrayView<uint8> Stream = Bytes.RightChop(EnvelopeHeaderBytes);
		const int32 Want = FMath::Min<int32>(PrefixBytes, static_cast<int32>(Raw));
		Out.SetNumUninitialized(Want);
		z_stream Inflater;
		FMemory::Memzero(Inflater);
		Inflater.next_in = const_cast<Bytef*>(Stream.GetData());
		Inflater.avail_in = static_cast<uInt>(Stream.Num());
		Inflater.next_out = Out.GetData();
		Inflater.avail_out = static_cast<uInt>(Want);
		if (inflateInit(&Inflater) != Z_OK)
		{
			Out.Reset();
			return false;
		}
		// Stops when Want bytes are out; Z_BUF_ERROR then only means the output is full.
		const int Result = inflate(&Inflater, Z_SYNC_FLUSH);
		const uint64 TotalOut = Inflater.total_out;
		inflateEnd(&Inflater);
		if ((Result != Z_OK && Result != Z_STREAM_END && Result != Z_BUF_ERROR) || TotalOut != static_cast<uint64>(Want))
		{
			Out.Reset();
			return false;
		}
		return true;
	}
}
