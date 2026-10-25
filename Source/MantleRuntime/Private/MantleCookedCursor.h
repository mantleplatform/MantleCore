// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleSkinnedMesh.h"

#include <type_traits>

/**
 * The bounds-checked little-endian cursor the cooked-format readers share (MSK1, MBS1, MAN1, and the
 * section table MRM2 reuses; AVATAR_CATALOG_PLAN.md section 3.4, writers in backend/internal/cook/). These bytes arrive over the
 * network from content a stranger uploaded, so every read is checked against the bytes left, every
 * count is checked before anything is allocated from it, and a failure is sticky: after the first
 * short read every later read fails too and returns zeros, so a reader can check once per block.
 * Nothing here asserts, logs or allocates beyond what the caller asked for.
 *
 * Named namespace, not anonymous: MantleRuntime builds as a unity build.
 */
namespace MantleCookedFormat
{
	static_assert(PLATFORM_LITTLE_ENDIAN, "the cooked formats are little-endian; a big-endian target needs byte swapping here");

	/** Longest str16 any cooked format writes (the Go writers' maxNameBytes). */
	constexpr int32 MaxNameBytes = 256;

	/** Most extension blocks a file may carry (the Go writers' maxExtensions). */
	constexpr int32 MaxExtensions = 64;

	class FCursor
	{
	public:
		explicit FCursor(const TConstArrayView<uint8> InData)
			: Data(InData.GetData())
			, Size(InData.Num())
		{
		}

		bool Failed() const { return bFailed; }
		int64 Offset() const { return Position; }
		int64 Remaining() const { return Size - Position; }

		/** Whether Bytes more can be read (false once anything failed, or for a negative count). */
		bool Has(const int64 Bytes) const
		{
			return !bFailed && Bytes >= 0 && Bytes <= Size - Position;
		}

		/** Marks the cursor failed (sticky) and returns false, for a caller's own structural check. */
		bool Fail()
		{
			bFailed = true;
			return false;
		}

		bool Skip(const int64 Bytes)
		{
			if (!Has(Bytes))
			{
				return Fail();
			}
			Position += Bytes;
			return true;
		}

		bool Magic(const char (&Expected)[5])
		{
			if (!Has(4))
			{
				return Fail();
			}
			const bool bMatch = FMemory::Memcmp(Data + Position, Expected, 4) == 0;
			Position += 4;
			return bMatch;
		}

		uint8 U8()
		{
			uint8 Value = 0;
			Copy(&Value, 1);
			return Value;
		}

		uint16 U16()
		{
			uint16 Value = 0;
			Copy(&Value, sizeof(Value));
			return Value;
		}

		uint32 U32()
		{
			uint32 Value = 0;
			Copy(&Value, sizeof(Value));
			return Value;
		}

		int32 I32()
		{
			int32 Value = 0;
			Copy(&Value, sizeof(Value));
			return Value;
		}

		float F32()
		{
			float Value = 0.0f;
			Copy(&Value, sizeof(Value));
			return Value;
		}

		/**
		 * Count elements of Out's type straight from the bytes (the layout is the element's own, packed
		 * little-endian). Out is sized only after the bytes are known to be there.
		 */
		template <typename ElementType>
		bool Array(TArray<ElementType>& Out, const int64 Count)
		{
			static_assert(std::is_trivially_copyable_v<ElementType>, "cooked arrays are plain data");
			const int64 Bytes = Count * static_cast<int64>(sizeof(ElementType));
			if (Count < 0 || Count > MAX_int32 || !Has(Bytes))
			{
				Out.Reset();
				return Fail();
			}
			Out.SetNumUninitialized(static_cast<int32>(Count));
			FMemory::Memcpy(Out.GetData(), Data + Position, Bytes);
			Position += Bytes;
			return true;
		}

		/**
		 * A str16: uint16 byte length, then that many UTF-8 bytes. Refused (and the cursor failed) when
		 * longer than MaxBytes, truncated, or holding a NUL, which would silently shorten a name.
		 */
		bool String(FString& Out, const int32 MaxBytes = MaxNameBytes)
		{
			Out.Reset();
			const uint16 Length = U16();
			if (bFailed || Length > MaxBytes || !Has(Length))
			{
				return Fail();
			}
			const UTF8CHAR* Bytes = reinterpret_cast<const UTF8CHAR*>(Data + Position);
			for (int32 Index = 0; Index < Length; ++Index)
			{
				if (Data[Position + Index] == 0)
				{
					return Fail();
				}
			}
			if (Length > 0)
			{
				const auto Converted = StringCast<TCHAR>(Bytes, static_cast<int32>(Length));
				Out = FString::ConstructFromPtrSize(Converted.Get(), Converted.Length());
			}
			Position += Length;
			return true;
		}

	private:
		void Copy(void* Out, const int64 Bytes)
		{
			if (!Has(Bytes))
			{
				bFailed = true;
				return;
			}
			FMemory::Memcpy(Out, Data + Position, Bytes);
			Position += Bytes;
		}

		const uint8* Data = nullptr;
		int64 Size = 0;
		int64 Position = 0;
		bool bFailed = false;
	};

	/** A 4-byte extension tag as text, anything unprintable shown as '?', for logs. */
	inline FString DescribeTag(const uint8 (&Tag)[4])
	{
		FString Text;
		for (const uint8 Byte : Tag)
		{
			Text.AppendChar(Byte >= 0x20 && Byte < 0x7F ? static_cast<TCHAR>(Byte) : TEXT('?'));
		}
		return Text;
	}

	/**
	 * The tagged extension table that ends every cooked avatar format: uint32 count (at most
	 * MaxExtensions), then per block a 4-byte tag, a uint32 byte length and the bytes. Version 1 of
	 * every format defines no tags, so every block is skipped; OutSkippedTags gets each one's tag for
	 * the log. False, with the reason, when the table is malformed.
	 */
	inline bool SkipExtensions(FCursor& Cursor, TArray<FString>& OutSkippedTags, FString& OutError)
	{
		OutSkippedTags.Reset();
		const uint32 Count = Cursor.U32();
		if (Cursor.Failed())
		{
			OutError = TEXT("the extension table is missing");
			return false;
		}
		if (Count > static_cast<uint32>(MaxExtensions))
		{
			OutError = FString::Printf(TEXT("the file claims %u extension blocks (at most %d)"), Count, MaxExtensions);
			return Cursor.Fail();
		}
		for (uint32 Index = 0; Index < Count; ++Index)
		{
			uint8 Tag[4] = {};
			Tag[0] = Cursor.U8();
			Tag[1] = Cursor.U8();
			Tag[2] = Cursor.U8();
			Tag[3] = Cursor.U8();
			const uint32 Length = Cursor.U32();
			if (Cursor.Failed() || !Cursor.Skip(static_cast<int64>(Length)))
			{
				OutError = FString::Printf(TEXT("extension block %u is truncated"), Index);
				return false;
			}
			OutSkippedTags.Add(DescribeTag(Tag));
		}
		return true;
	}

	/** Most entries a section table may hold (the Go writers' MaxSectionTableEntries). */
	constexpr int32 MaxSectionTableEntries = 64;

	/** Highest material slot a section may name (FMantleSkinnedMesh::Validate's slot limit). */
	constexpr uint32 MaxSectionMaterialIndex = 255;

	/**
	 * The section table MSK1 and MRM2 share byte for byte (backend/internal/cook/sectiontable.go):
	 * uint32 count (1 to MaxSectionTableEntries), then per section materialIndex, firstIndex and
	 * indexCount as uint32 and the material name as a str16. The shared rules are checked here: whole
	 * triangles inside IndexCount indices, a material slot in range, and no material name twice
	 * (sections are bound to materials by name). A format's own rules (MRM2's tiling, non-empty
	 * names) are its reader's. False, with the reason, on anything else.
	 */
	inline bool ReadSectionTable(FCursor& Cursor, const uint32 IndexCount, TArray<FMantleSkinnedMeshSection>& OutSections, FString& OutError)
	{
		OutSections.Reset();
		const uint32 Count = Cursor.U32();
		if (Cursor.Failed())
		{
			OutError = TEXT("the section table is missing");
			return false;
		}
		if (Count < 1 || Count > static_cast<uint32>(MaxSectionTableEntries))
		{
			OutError = FString::Printf(TEXT("the section table claims %u sections (1 to %d)"), Count, MaxSectionTableEntries);
			return Cursor.Fail();
		}
		// Each entry is at least three uint32 and a str16 length.
		if (!Cursor.Has(static_cast<int64>(Count) * 14))
		{
			OutError = TEXT("the section table is truncated");
			return Cursor.Fail();
		}
		TSet<FName> Names;
		OutSections.SetNum(static_cast<int32>(Count));
		for (int32 Index = 0; Index < OutSections.Num(); ++Index)
		{
			FMantleSkinnedMeshSection& Section = OutSections[Index];
			const uint32 MaterialIndex = Cursor.U32();
			const uint32 FirstIndex = Cursor.U32();
			const uint32 SectionIndexCount = Cursor.U32();
			FString Material;
			Cursor.String(Material);
			if (Cursor.Failed())
			{
				OutError = TEXT("the section table is truncated");
				return false;
			}
			if (FirstIndex % 3 != 0 || SectionIndexCount % 3 != 0 || static_cast<uint64>(FirstIndex) + SectionIndexCount > IndexCount)
			{
				OutError = FString::Printf(TEXT("section %d covers indices [%u, +%u), not whole triangles inside the %u indices"), Index, FirstIndex, SectionIndexCount, IndexCount);
				return Cursor.Fail();
			}
			if (MaterialIndex > MaxSectionMaterialIndex)
			{
				OutError = FString::Printf(TEXT("section %d uses material slot %u (0 to %u)"), Index, MaterialIndex, MaxSectionMaterialIndex);
				return Cursor.Fail();
			}
			Section.MaterialIndex = static_cast<int32>(MaterialIndex);
			Section.FirstIndex = static_cast<int32>(FirstIndex);
			Section.IndexCount = static_cast<int32>(SectionIndexCount);
			if (!Material.IsEmpty())
			{
				Section.MaterialName = FName(*Material);
				bool bDuplicate = false;
				Names.Add(Section.MaterialName, &bDuplicate);
				if (bDuplicate)
				{
					OutError = FString::Printf(TEXT("material '%s' names two sections"), *Material);
					return Cursor.Fail();
				}
			}
		}
		return true;
	}

	/** Every component finite. */
	template <typename VectorType>
	bool AllFinite(const TArray<VectorType>& Values)
	{
		for (const VectorType& Value : Values)
		{
			if (Value.ContainsNaN())
			{
				return false;
			}
		}
		return true;
	}
}
