// Copyright Joshua Eger. All Rights Reserved.
#include "MantleSha256.h"

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleSha256Private
{
	constexpr uint32 RoundConstants[64] = {
		0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
		0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
		0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
		0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
		0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
		0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
		0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
		0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
	};

	FORCEINLINE uint32 RotateRight(uint32 Value, uint32 Bits)
	{
		return (Value >> Bits) | (Value << (32u - Bits));
	}

	void CompressBlock(uint32 State[8], const uint8 Block[64])
	{
		uint32 W[64];
		for (int32 Index = 0; Index < 16; ++Index)
		{
			W[Index] = (uint32(Block[Index * 4]) << 24) | (uint32(Block[Index * 4 + 1]) << 16)
				| (uint32(Block[Index * 4 + 2]) << 8) | uint32(Block[Index * 4 + 3]);
		}
		for (int32 Index = 16; Index < 64; ++Index)
		{
			const uint32 S0 = RotateRight(W[Index - 15], 7) ^ RotateRight(W[Index - 15], 18) ^ (W[Index - 15] >> 3);
			const uint32 S1 = RotateRight(W[Index - 2], 17) ^ RotateRight(W[Index - 2], 19) ^ (W[Index - 2] >> 10);
			W[Index] = W[Index - 16] + S0 + W[Index - 7] + S1;
		}

		uint32 A = State[0], B = State[1], C = State[2], D = State[3];
		uint32 E = State[4], F = State[5], G = State[6], H = State[7];
		for (int32 Index = 0; Index < 64; ++Index)
		{
			const uint32 Sum1 = RotateRight(E, 6) ^ RotateRight(E, 11) ^ RotateRight(E, 25);
			const uint32 Choose = (E & F) ^ (~E & G);
			const uint32 Temp1 = H + Sum1 + Choose + RoundConstants[Index] + W[Index];
			const uint32 Sum0 = RotateRight(A, 2) ^ RotateRight(A, 13) ^ RotateRight(A, 22);
			const uint32 Majority = (A & B) ^ (A & C) ^ (B & C);
			const uint32 Temp2 = Sum0 + Majority;
			H = G;
			G = F;
			F = E;
			E = D + Temp1;
			D = C;
			C = B;
			B = A;
			A = Temp1 + Temp2;
		}
		State[0] += A; State[1] += B; State[2] += C; State[3] += D;
		State[4] += E; State[5] += F; State[6] += G; State[7] += H;
	}
}

void FMantleSha256::Hash(const uint8* Data, int64 Size, uint8 OutDigest[32])
{
	using namespace MantleSha256Private;

	uint32 State[8] = {
		0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
	};

	const int64 WholeBlocks = Size / 64;
	for (int64 Block = 0; Block < WholeBlocks; ++Block)
	{
		CompressBlock(State, Data + Block * 64);
	}

	// The tail: the remaining bytes, a 0x80 marker, zeros, and the message length in bits
	// (big-endian), in one block or two.
	uint8 Tail[128];
	FMemory::Memzero(Tail, sizeof(Tail));
	const int64 Remaining = Size - WholeBlocks * 64;
	if (Remaining > 0)
	{
		FMemory::Memcpy(Tail, Data + WholeBlocks * 64, static_cast<SIZE_T>(Remaining));
	}
	Tail[Remaining] = 0x80;
	const int32 TailBlocks = Remaining < 56 ? 1 : 2;
	const uint64 BitLength = static_cast<uint64>(Size) * 8u;
	for (int32 Byte = 0; Byte < 8; ++Byte)
	{
		Tail[TailBlocks * 64 - 1 - Byte] = static_cast<uint8>(BitLength >> (8 * Byte));
	}
	for (int32 Block = 0; Block < TailBlocks; ++Block)
	{
		CompressBlock(State, Tail + Block * 64);
	}

	for (int32 Word = 0; Word < 8; ++Word)
	{
		OutDigest[Word * 4] = static_cast<uint8>(State[Word] >> 24);
		OutDigest[Word * 4 + 1] = static_cast<uint8>(State[Word] >> 16);
		OutDigest[Word * 4 + 2] = static_cast<uint8>(State[Word] >> 8);
		OutDigest[Word * 4 + 3] = static_cast<uint8>(State[Word]);
	}
}

FString FMantleSha256::HexDigest(TConstArrayView<uint8> Bytes)
{
	uint8 Digest[32];
	Hash(Bytes.GetData(), Bytes.Num(), Digest);
	return BytesToHexLower(Digest, 32);
}

FString FMantleSha256::AssetHash(TConstArrayView<uint8> Bytes)
{
	return TEXT("sha256:") + HexDigest(Bytes);
}
