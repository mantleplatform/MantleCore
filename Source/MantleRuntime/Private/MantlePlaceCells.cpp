// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceCells.h"

#include "MantlePlaceAssets.h"
#include "MantlePlaceParser.h"

#include "Misc/Compression.h"
#include "Serialization/JsonReader.h"

const ANSICHAR* const FMantlePlaceCells::IndexMagic = "MCI1";
const ANSICHAR* const FMantlePlaceCells::CellMagic = "MCF1";

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantlePlaceCellsPrivate
{
	/** Contract 4.2: a grid has one to four levels, a cell a whole number of cm from 10 m to 10 km, a draw distance up to the draw distance cap. */
	constexpr int64 MinCellSize = 1000;
	constexpr int64 MaxCellSize = 1000000;
	constexpr double MaxLevelDrawDistance = 1000000.0;
	constexpr int64 MaxCellCoordinate = 1000000;
	constexpr int64 MaxCount = MAX_int32;

	/**
	 * A forward-only reader of the index text. Every function that takes a notation has already read it; one that opens an object or array reads
	 * to its end. A problem is written once, with where it is, and stops the read.
	 */
	class FIndexStream
	{
	public:
		explicit FIndexStream(const FString& Json)
			: Reader(TJsonReaderFactory<TCHAR>::Create(Json))
		{
		}

		FString Error;

		bool Fail(const FString& Path, const FString& Message)
		{
			if (Error.IsEmpty())
			{
				Error = Path.IsEmpty() ? Message : FString::Printf(TEXT("%s: %s"), *Path, *Message);
			}
			return false;
		}

		bool Next(EJsonNotation& Notation, const FString& Path)
		{
			if (!Reader->ReadNext(Notation) || Notation == EJsonNotation::Error)
			{
				return Fail(Path, FString::Printf(TEXT("is not valid JSON (%s)"), *Reader->GetErrorMessage()));
			}
			return true;
		}

		/** What is left after the document: nothing. */
		bool ExpectEnd()
		{
			EJsonNotation Notation;
			if (Reader->ReadNext(Notation) || !Reader->GetErrorMessage().IsEmpty())
			{
				return Fail(FString(), TEXT("has something after the document"));
			}
			return true;
		}

		/** Calls OnMember(key, notation) for each member of the object just opened; it must read the member's whole value. */
		bool ReadObject(const FString& Path, const TFunctionRef<bool(const FString&, EJsonNotation)> OnMember)
		{
			for (;;)
			{
				EJsonNotation Notation;
				if (!Next(Notation, Path))
				{
					return false;
				}
				if (Notation == EJsonNotation::ObjectEnd)
				{
					return true;
				}
				const FString Key = Reader->GetIdentifier();
				if (!OnMember(Key, Notation))
				{
					return false;
				}
			}
		}

		/** Calls OnElement(index, notation) for each element of the array just opened. */
		bool ReadArray(const FString& Path, const TFunctionRef<bool(int32, EJsonNotation)> OnElement)
		{
			for (int32 Index = 0;; ++Index)
			{
				EJsonNotation Notation;
				if (!Next(Notation, Path))
				{
					return false;
				}
				if (Notation == EJsonNotation::ArrayEnd)
				{
					return true;
				}
				if (!OnElement(Index, Notation))
				{
					return false;
				}
			}
		}

		bool AsNumber(const EJsonNotation Notation, const FString& Path, double& Out)
		{
			if (Notation != EJsonNotation::Number)
			{
				return Fail(Path, TEXT("must be a number"));
			}
			Out = Reader->GetValueAsNumber();
			if (!FMath::IsFinite(Out))
			{
				return Fail(Path, TEXT("must be a finite number"));
			}
			return true;
		}

		bool AsInt(const EJsonNotation Notation, const FString& Path, const int64 Min, const int64 Max, int64& Out)
		{
			double Value = 0.0;
			if (!AsNumber(Notation, Path, Value))
			{
				return false;
			}
			if (Value != FMath::FloorToDouble(Value) || Value < static_cast<double>(Min) || Value > static_cast<double>(Max))
			{
				return Fail(Path, FString::Printf(TEXT("must be a whole number from %lld to %lld"), Min, Max));
			}
			Out = static_cast<int64>(Value);
			return true;
		}

		bool AsString(const EJsonNotation Notation, const FString& Path, FString& Out)
		{
			if (Notation != EJsonNotation::String)
			{
				return Fail(Path, TEXT("must be a string"));
			}
			Out = Reader->GetValueAsString();
			return true;
		}

		/** An array of exactly three numbers: a point. */
		bool AsPoint(const EJsonNotation Notation, const FString& Path, FVector& Out)
		{
			if (Notation != EJsonNotation::ArrayStart)
			{
				return Fail(Path, TEXT("must be an array of three numbers"));
			}
			double Values[3] = {};
			int32 Count = 0;
			if (!ReadArray(Path, [&](const int32 Index, const EJsonNotation Element)
				{
					if (Index >= 3)
					{
						return Fail(Path, TEXT("must be an array of three numbers"));
					}
					++Count;
					return AsNumber(Element, FString::Printf(TEXT("%s/%d"), *Path, Index), Values[Index]);
				}))
			{
				return false;
			}
			if (Count != 3)
			{
				return Fail(Path, TEXT("must be an array of three numbers"));
			}
			Out = FVector(Values[0], Values[1], Values[2]);
			return true;
		}

	private:
		TSharedRef<TJsonReader<TCHAR>> Reader;
	};

	bool ReadLevel(FIndexStream& Stream, const FString& Path, FMantleCellGridLevel& Out)
	{
		uint32 Seen = 0;
		const bool bRead = Stream.ReadObject(Path, [&](const FString& Key, const EJsonNotation Notation)
		{
			const FString KeyPath = Path + TEXT("/") + Key;
			uint32 Bit = 0;
			if (Key == TEXT("cellSize"))
			{
				Bit = 1;
			}
			else if (Key == TEXT("maxDrawDistance"))
			{
				Bit = 2;
			}
			else
			{
				return Stream.Fail(KeyPath, TEXT("is not a key of a grid level"));
			}
			if (Seen & Bit)
			{
				return Stream.Fail(KeyPath, TEXT("is given twice"));
			}
			Seen |= Bit;
			if (Bit == 1)
			{
				return Stream.AsInt(Notation, KeyPath, MinCellSize, MaxCellSize, Out.CellSize);
			}
			if (!Stream.AsNumber(Notation, KeyPath, Out.MaxDrawDistance))
			{
				return false;
			}
			if (Out.MaxDrawDistance <= 0.0 || Out.MaxDrawDistance > MaxLevelDrawDistance)
			{
				return Stream.Fail(KeyPath, FString::Printf(TEXT("must be above 0 and at most %.0f"), MaxLevelDrawDistance));
			}
			return true;
		});
		if (bRead && Seen != 3)
		{
			return Stream.Fail(Path, TEXT("needs cellSize and maxDrawDistance"));
		}
		return bRead;
	}

	bool ReadFileEntry(FIndexStream& Stream, const FString& Path, FMantleCellFileRef& Out)
	{
		uint32 Seen = 0;
		const bool bRead = Stream.ReadObject(Path, [&](const FString& Key, const EJsonNotation Notation)
		{
			const FString KeyPath = Path + TEXT("/") + Key;
			uint32 Bit = 0;
			if (Key == TEXT("id"))
			{
				Bit = 1;
			}
			else if (Key == TEXT("hash"))
			{
				Bit = 2;
			}
			else if (Key == TEXT("bytes"))
			{
				Bit = 4;
			}
			else
			{
				return Stream.Fail(KeyPath, TEXT("is not a key of a cell file entry"));
			}
			if (Seen & Bit)
			{
				return Stream.Fail(KeyPath, TEXT("is given twice"));
			}
			Seen |= Bit;
			if (Bit == 1)
			{
				if (!Stream.AsString(Notation, KeyPath, Out.Id))
				{
					return false;
				}
				if (!FMantlePlaceAssets::IsUploadedId(Out.Id))
				{
					return Stream.Fail(KeyPath, TEXT("must be an uploaded asset id"));
				}
				return true;
			}
			if (Bit == 2)
			{
				if (!Stream.AsString(Notation, KeyPath, Out.Hash))
				{
					return false;
				}
				if (Out.Hash.IsEmpty() || Out.Hash.Len() > 128)
				{
					return Stream.Fail(KeyPath, TEXT("must be 1 to 128 characters"));
				}
				return true;
			}
			return Stream.AsInt(Notation, KeyPath, 0, MAX_int64 / 2, Out.Bytes);
		});
		if (bRead && Seen != 7)
		{
			return Stream.Fail(Path, TEXT("needs id, hash and bytes"));
		}
		return bRead;
	}

	bool ReadCell(FIndexStream& Stream, const FString& Path, TSet<FString>& FileIds, FMantleCellInfo& Out)
	{
		uint32 Seen = 0;
		FVector Min = FVector::ZeroVector;
		FVector Max = FVector::ZeroVector;
		bool bHasMin = false;
		bool bHasMax = false;
		const bool bRead = Stream.ReadObject(Path, [&](const FString& Key, const EJsonNotation Notation)
		{
			const FString KeyPath = Path + TEXT("/") + Key;
			uint32 Bit = 0;
			if (Key == TEXT("level")) { Bit = 1; }
			else if (Key == TEXT("x")) { Bit = 2; }
			else if (Key == TEXT("y")) { Bit = 4; }
			else if (Key == TEXT("bounds")) { Bit = 8; }
			else if (Key == TEXT("drawDistance")) { Bit = 16; }
			else if (Key == TEXT("files")) { Bit = 32; }
			else if (Key == TEXT("counts")) { Bit = 64; }
			else
			{
				return Stream.Fail(KeyPath, TEXT("is not a key of a cell (version 1 has no other)"));
			}
			if (Seen & Bit)
			{
				return Stream.Fail(KeyPath, TEXT("is given twice"));
			}
			Seen |= Bit;
			int64 Number = 0;
			switch (Bit)
			{
			case 1:
				if (!Stream.AsInt(Notation, KeyPath, 0, FMantlePlaceCells::MaxGridLevels - 1, Number))
				{
					return false;
				}
				Out.Level = static_cast<int32>(Number);
				return true;
			case 2:
			case 4:
				if (!Stream.AsInt(Notation, KeyPath, -MaxCellCoordinate, MaxCellCoordinate, Number))
				{
					return false;
				}
				(Bit == 2 ? Out.X : Out.Y) = static_cast<int32>(Number);
				return true;
			case 8:
				if (Notation != EJsonNotation::ObjectStart)
				{
					return Stream.Fail(KeyPath, TEXT("must be an object with min and max"));
				}
				return Stream.ReadObject(KeyPath, [&](const FString& BoundKey, const EJsonNotation BoundNotation)
				{
					const FString BoundPath = KeyPath + TEXT("/") + BoundKey;
					if (BoundKey == TEXT("min") && !bHasMin)
					{
						bHasMin = true;
						return Stream.AsPoint(BoundNotation, BoundPath, Min);
					}
					if (BoundKey == TEXT("max") && !bHasMax)
					{
						bHasMax = true;
						return Stream.AsPoint(BoundNotation, BoundPath, Max);
					}
					return Stream.Fail(BoundPath, TEXT("is not min or max, or is given twice"));
				});
			case 16:
				if (!Stream.AsNumber(Notation, KeyPath, Out.DrawDistance))
				{
					return false;
				}
				if (Out.DrawDistance <= 0.0 || Out.DrawDistance > MaxLevelDrawDistance)
				{
					return Stream.Fail(KeyPath, FString::Printf(TEXT("must be above 0 and at most %.0f"), MaxLevelDrawDistance));
				}
				return true;
			case 32:
				if (Notation != EJsonNotation::ArrayStart)
				{
					return Stream.Fail(KeyPath, TEXT("must be an array of cell files"));
				}
				if (!Stream.ReadArray(KeyPath, [&](const int32 Index, const EJsonNotation Element)
					{
						const FString FilePath = FString::Printf(TEXT("%s/%d"), *KeyPath, Index);
						if (Index >= FMantlePlaceCells::MaxFilesPerCell)
						{
							return Stream.Fail(KeyPath, FString::Printf(TEXT("lists more than %d files"), FMantlePlaceCells::MaxFilesPerCell));
						}
						if (Element != EJsonNotation::ObjectStart)
						{
							return Stream.Fail(FilePath, TEXT("must be an object"));
						}
						FMantleCellFileRef& File = Out.Files.AddDefaulted_GetRef();
						if (!ReadFileEntry(Stream, FilePath, File))
						{
							return false;
						}
						bool bAlreadyListed = false;
						FileIds.Add(File.Id, &bAlreadyListed);
						return bAlreadyListed ? Stream.Fail(FilePath + TEXT("/id"), TEXT("is listed more than once in the index")) : true;
					}))
				{
					return false;
				}
				return Out.Files.IsEmpty() ? Stream.Fail(KeyPath, TEXT("must list at least one file")) : true;
			default:
				break;
			}
			// counts
			if (Notation != EJsonNotation::ObjectStart)
			{
				return Stream.Fail(KeyPath, TEXT("must be an object with entities, instances and solid"));
			}
			uint32 CountsSeen = 0;
			if (!Stream.ReadObject(KeyPath, [&](const FString& CountKey, const EJsonNotation CountNotation)
				{
					const FString CountPath = KeyPath + TEXT("/") + CountKey;
					int64* Target = CountKey == TEXT("entities") ? &Out.Entities : CountKey == TEXT("instances") ? &Out.Instances : CountKey == TEXT("solid") ? &Out.Solid : nullptr;
					const uint32 CountBit = CountKey == TEXT("entities") ? 1 : CountKey == TEXT("instances") ? 2 : 4;
					if (!Target)
					{
						return Stream.Fail(CountPath, TEXT("is not a count of a cell"));
					}
					if (CountsSeen & CountBit)
					{
						return Stream.Fail(CountPath, TEXT("is given twice"));
					}
					CountsSeen |= CountBit;
					return Stream.AsInt(CountNotation, CountPath, 0, MaxCount, *Target);
				}))
			{
				return false;
			}
			return CountsSeen == 7 ? true : Stream.Fail(KeyPath, TEXT("needs entities, instances and solid"));
		});
		if (!bRead)
		{
			return false;
		}
		if (Seen != 127 || !bHasMin || !bHasMax)
		{
			return Stream.Fail(Path, TEXT("needs level, x, y, bounds (min and max), drawDistance, files and counts"));
		}
		if (Min.X > Max.X || Min.Y > Max.Y || Min.Z > Max.Z)
		{
			return Stream.Fail(Path + TEXT("/bounds"), TEXT("has a min above its max"));
		}
		Out.Bounds = FBox(Min, Max);
		return true;
	}

	/** (Level, X, Y) strictly rising. */
	bool KeyRises(const FMantleCellInfo& Before, const FMantleCellInfo& After)
	{
		if (Before.Level != After.Level)
		{
			return Before.Level < After.Level;
		}
		if (Before.X != After.X)
		{
			return Before.X < After.X;
		}
		return Before.Y < After.Y;
	}
}

bool FMantlePlaceCells::Unpack(const TConstArrayView<uint8> Container, const ANSICHAR* Magic, const uint32 MaxInflated, TArray<uint8>& Out, FString* OutReason)
{
	const auto Refuse = [&Out, OutReason](const FString& Reason)
	{
		Out.Reset();
		if (OutReason)
		{
			*OutReason = Reason;
		}
		return false;
	};

	Out.Reset();
	if (Container.Num() < ContainerHeaderBytes || FMemory::Memcmp(Container.GetData(), Magic, 4) != 0)
	{
		return Refuse(FString::Printf(TEXT("is not an %hs container"), Magic));
	}
	uint32 RawBytes = 0;
	FMemory::Memcpy(&RawBytes, Container.GetData() + 4, sizeof(RawBytes));
#if PLATFORM_LITTLE_ENDIAN == 0
	RawBytes = BYTESWAP_ORDER32(RawBytes);
#endif
	// The size word is checked before anything is allocated: a damaged or hostile one must not take memory.
	if (RawBytes == 0 || RawBytes > MaxInflated)
	{
		return Refuse(FString::Printf(TEXT("says it inflates to %u bytes (1 to %u are allowed)"), RawBytes, MaxInflated));
	}
	Out.SetNumUninitialized(static_cast<int32>(RawBytes));
	const int32 PackedBytes = Container.Num() - ContainerHeaderBytes;
	if (!FCompression::UncompressMemory(NAME_Zlib, Out.GetData(), Out.Num(), Container.GetData() + ContainerHeaderBytes, PackedBytes))
	{
		return Refuse(TEXT("is damaged (its zlib stream does not inflate to the size it names)"));
	}
	return true;
}

bool FMantlePlaceCells::ToText(const TConstArrayView<uint8> Utf8, FString& OutText, FString* OutReason)
{
	OutText.Reset();
	if (Utf8.Num() >= 3 && Utf8[0] == 0xEF && Utf8[1] == 0xBB && Utf8[2] == 0xBF)
	{
		if (OutReason)
		{
			*OutReason = TEXT("starts with a byte order mark (the document is UTF-8 without one)");
		}
		return false;
	}
	const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Utf8.GetData()), Utf8.Num());
	OutText = FString(Converted.Length(), Converted.Get());
	return true;
}

bool FMantlePlaceCells::ReadIndex(const FString& Json, FMantleCellIndex& Out, FString& OutError)
{
	using namespace MantlePlaceCellsPrivate;

	Out = FMantleCellIndex();
	OutError.Reset();
	FIndexStream Stream(Json);

	EJsonNotation Notation;
	if (!Stream.Next(Notation, FString()))
	{
		OutError = Stream.Error;
		return false;
	}
	if (Notation != EJsonNotation::ObjectStart)
	{
		OutError = TEXT("the cell index must be a JSON object");
		return false;
	}

	uint32 Seen = 0;
	TSet<FString> FileIds;
	const bool bRead = Stream.ReadObject(FString(), [&](const FString& Key, const EJsonNotation Member)
	{
		const FString KeyPath = TEXT("/") + Key;
		uint32 Bit = 0;
		if (Key == TEXT("version")) { Bit = 1; }
		else if (Key == TEXT("grid")) { Bit = 2; }
		else if (Key == TEXT("cells")) { Bit = 4; }
		else
		{
			// "far" and "terrain" are reserved for later stages, refused in version 1.
			return Stream.Fail(KeyPath, TEXT("is not a key of a version 1 cell index"));
		}
		if (Seen & Bit)
		{
			return Stream.Fail(KeyPath, TEXT("is given twice"));
		}
		Seen |= Bit;
		if (Bit == 1)
		{
			int64 Version = 0;
			if (!Stream.AsInt(Member, KeyPath, 0, MAX_int32, Version))
			{
				return false;
			}
			if (Version != DocumentVersion)
			{
				return Stream.Fail(KeyPath, FString::Printf(TEXT("is cell index version %lld; this game reads version %d. Update Mantle to open this place."), Version, DocumentVersion));
			}
			Out.Version = static_cast<int32>(Version);
			return true;
		}
		if (Bit == 2)
		{
			if (Member != EJsonNotation::ObjectStart)
			{
				return Stream.Fail(KeyPath, TEXT("must be an object with levels"));
			}
			bool bHasLevels = false;
			if (!Stream.ReadObject(KeyPath, [&](const FString& GridKey, const EJsonNotation GridNotation)
				{
					const FString LevelsPath = KeyPath + TEXT("/") + GridKey;
					if (GridKey != TEXT("levels") || bHasLevels)
					{
						return Stream.Fail(LevelsPath, TEXT("is not a key of the grid, or is given twice"));
					}
					bHasLevels = true;
					if (GridNotation != EJsonNotation::ArrayStart)
					{
						return Stream.Fail(LevelsPath, TEXT("must be an array of grid levels"));
					}
					return Stream.ReadArray(LevelsPath, [&](const int32 Index, const EJsonNotation LevelNotation)
					{
						const FString LevelPath = FString::Printf(TEXT("%s/%d"), *LevelsPath, Index);
						if (Index >= FMantlePlaceCells::MaxGridLevels)
						{
							return Stream.Fail(LevelsPath, FString::Printf(TEXT("has more than %d levels"), FMantlePlaceCells::MaxGridLevels));
						}
						if (LevelNotation != EJsonNotation::ObjectStart)
						{
							return Stream.Fail(LevelPath, TEXT("must be an object"));
						}
						return ReadLevel(Stream, LevelPath, Out.Levels.AddDefaulted_GetRef());
					});
				}))
			{
				return false;
			}
			if (Out.Levels.IsEmpty())
			{
				return Stream.Fail(KeyPath + TEXT("/levels"), TEXT("must have at least one level"));
			}
			for (int32 Level = 1; Level < Out.Levels.Num(); ++Level)
			{
				if (Out.Levels[Level].MaxDrawDistance <= Out.Levels[Level - 1].MaxDrawDistance)
				{
					return Stream.Fail(FString::Printf(TEXT("%s/levels/%d/maxDrawDistance"), *KeyPath, Level), TEXT("must be above the level before it"));
				}
			}
			return true;
		}
		// cells
		if (Member != EJsonNotation::ArrayStart)
		{
			return Stream.Fail(KeyPath, TEXT("must be an array of cells"));
		}
		return Stream.ReadArray(KeyPath, [&](const int32 Index, const EJsonNotation CellNotation)
		{
			const FString CellPath = FString::Printf(TEXT("%s/%d"), *KeyPath, Index);
			if (CellNotation != EJsonNotation::ObjectStart)
			{
				return Stream.Fail(CellPath, TEXT("must be an object"));
			}
			FMantleCellInfo& Cell = Out.Cells.AddDefaulted_GetRef();
			if (!ReadCell(Stream, CellPath, FileIds, Cell))
			{
				return false;
			}
			if (Out.Cells.Num() > 1 && !KeyRises(Out.Cells[Out.Cells.Num() - 2], Cell))
			{
				return Stream.Fail(CellPath, TEXT("is out of order or repeats a cell: cells are sorted by level, x, y, each once"));
			}
			return true;
		});
	});

	if (!bRead || !Stream.ExpectEnd())
	{
		OutError = Stream.Error;
		Out = FMantleCellIndex();
		return false;
	}
	if (Seen != 7)
	{
		OutError = TEXT("the cell index needs version, grid and cells");
		Out = FMantleCellIndex();
		return false;
	}
	// A cell's level and draw distance are held to the grid, now that the grid is known.
	for (int32 Index = 0; Index < Out.Cells.Num(); ++Index)
	{
		const FMantleCellInfo& Cell = Out.Cells[Index];
		if (Cell.Level >= Out.Levels.Num())
		{
			OutError = FString::Printf(TEXT("/cells/%d/level: the grid has %d level(s)"), Index, Out.Levels.Num());
		}
		else if (Cell.DrawDistance > Out.Levels[Cell.Level].MaxDrawDistance)
		{
			OutError = FString::Printf(TEXT("/cells/%d/drawDistance: is above its level's maxDrawDistance"), Index);
		}
		if (!OutError.IsEmpty())
		{
			Out = FMantleCellIndex();
			return false;
		}
	}
	return true;
}

TMap<FString, EMantleAssetKind> FMantlePlaceCells::RootAssetKinds(const FMantlePlace& Root)
{
	TMap<FString, EMantleAssetKind> Kinds;
	Kinds.Reserve(Root.Assets.Num());
	for (const FMantleAssetRef& Asset : Root.Assets)
	{
		Kinds.Add(Asset.Id, Asset.Kind);
	}
	return Kinds;
}

bool FMantlePlaceCells::ReadCellFile(const FString& Json, const TMap<FString, EMantleAssetKind>& RootAssets, FMantleCellContent& Out, TArray<FMantleParseError>& OutErrors)
{
	Out.Entities.Reset();
	Out.Instances = 0;
	Out.Solid = 0;
	if (!FMantlePlaceParser::ParseCellEntities(Json, RootAssets, Out.Entities, OutErrors))
	{
		Out.Entities.Reset();
		return false;
	}
	for (const FMantleEntity& Entity : Out.Entities)
	{
		if (const FMantleInstancedMeshProps* Instanced = Entity.Props.TryGet<FMantleInstancedMeshProps>())
		{
			Out.Instances += Instanced->Instances.Num();
			if (Instanced->Mesh.Collision != EMantleCollision::None)
			{
				Out.Solid += Instanced->Instances.Num();
			}
		}
		else if (const FMantleStaticMeshProps* Mesh = Entity.Props.TryGet<FMantleStaticMeshProps>())
		{
			if (Mesh->Collision != EMantleCollision::None)
			{
				++Out.Solid;
			}
		}
	}
	return true;
}

bool FMantlePlaceCells::Validate(const FMantlePlace& Place, const FMantleCellIndex& Index, const TArray<FMantleCellContent>& Contents, TArray<FMantleParseError>& OutErrors)
{
	// The index's counts are what the exporter and the backend held the files to; a cell that reads differently is a place that was altered.
	int32 Added = 0;
	for (const FMantleCellContent& Content : Contents)
	{
		if (!Index.Cells.IsValidIndex(Content.CellIndex))
		{
			OutErrors.Add(FMantleParseError{ TEXT("/cells"), FString::Printf(TEXT("a read cell (%d) is not in the index"), Content.CellIndex) });
			return false;
		}
		const FMantleCellInfo& Cell = Index.Cells[Content.CellIndex];
		const FString Path = FString::Printf(TEXT("cell %s"), *Cell.KeyString());
		if (Content.Entities.Num() != Cell.Entities)
		{
			OutErrors.Add(FMantleParseError{ Path, FString::Printf(TEXT("holds %d entities; the index says %lld"), Content.Entities.Num(), Cell.Entities) });
		}
		if (Content.Instances != Cell.Instances)
		{
			OutErrors.Add(FMantleParseError{ Path, FString::Printf(TEXT("holds %lld instances; the index says %lld"), Content.Instances, Cell.Instances) });
		}
		if (Content.Solid != Cell.Solid)
		{
			OutErrors.Add(FMantleParseError{ Path, FString::Printf(TEXT("holds %lld solid things; the index says %lld"), Content.Solid, Cell.Solid) });
		}
		Added += Content.Entities.Num();
	}

	// One id space across the root and every cell: the builder would silently keep only one entity of an id.
	TSet<FString> Ids;
	Ids.Reserve(Place.Entities.Num() + Added);
	for (const FMantleEntity& Entity : Place.Entities)
	{
		Ids.Add(Entity.Id);
	}
	for (const FMantleCellContent& Content : Contents)
	{
		for (const FMantleEntity& Entity : Content.Entities)
		{
			bool bAlreadyUsed = false;
			Ids.Add(Entity.Id, &bAlreadyUsed);
			if (bAlreadyUsed)
			{
				OutErrors.Add(FMantleParseError{ FString::Printf(TEXT("cell %s"), *Index.Cells[Content.CellIndex].KeyString()),
					FString::Printf(TEXT("'%s' is used by more than one entity; entity ids must be unique across the place and its cells"), *Entity.Id) });
			}
		}
	}
	return OutErrors.IsEmpty();
}

bool FMantlePlaceCells::Merge(FMantlePlace& Place, const FMantleCellIndex& Index, TArray<FMantleCellContent>& Contents, TArray<FMantleParseError>& OutErrors)
{
	if (Place.bCellsMerged || Place.bCellsStreamed)
	{
		OutErrors.Add(FMantleParseError{ TEXT("/cells"), TEXT("the cells were already added to this place") });
		return false;
	}
	if (!Validate(Place, Index, Contents, OutErrors))
	{
		return false;
	}

	int32 Added = 0;
	for (const FMantleCellContent& Content : Contents)
	{
		Added += Content.Entities.Num();
	}
	Place.Entities.Reserve(Place.Entities.Num() + Added);
	for (FMantleCellContent& Content : Contents)
	{
		Place.Entities.Append(MoveTemp(Content.Entities));
		Content.Entities.Reset();
	}
	Place.bCellsMerged = true;
	return true;
}

bool FMantlePlaceCells::AssembleForAssets(const FMantlePlace& Root, const FMantleCellIndex& Index, const TArray<FMantleCellContent>& Contents, FMantlePlace& OutPlace, TArray<FMantleParseError>& OutErrors)
{
	if (Root.bCellsMerged || Root.bCellsStreamed)
	{
		OutErrors.Add(FMantleParseError{ TEXT("/cells"), TEXT("the cells were already added to this place") });
		return false;
	}
	if (!Validate(Root, Index, Contents, OutErrors))
	{
		return false;
	}
	int32 Added = 0;
	for (const FMantleCellContent& Content : Contents)
	{
		Added += Content.Entities.Num();
	}
	OutPlace = Root;
	OutPlace.Entities.Reserve(Root.Entities.Num() + Added);
	for (const FMantleCellContent& Content : Contents)
	{
		OutPlace.Entities.Append(Content.Entities);
	}
	OutPlace.bCellsMerged = true;
	return true;
}

void FMantlePlaceCells::SceneryRecords(const TArray<FMantleCellContent>& Contents, TArray<FMantleEntity>& OutEntities)
{
	OutEntities.Reset();
	int32 Count = 0;
	for (const FMantleCellContent& Content : Contents)
	{
		Count += Content.Entities.Num();
	}
	OutEntities.Reserve(Count);
	for (const FMantleCellContent& Content : Contents)
	{
		for (const FMantleEntity& Entity : Content.Entities)
		{
			FMantleEntity& Slim = OutEntities.AddDefaulted_GetRef();
			Slim = Entity;
			// The list of instances is the one big thing in an entity.
			if (FMantleInstancedMeshProps* Instanced = Slim.Props.TryGet<FMantleInstancedMeshProps>())
			{
				Instanced->Instances.Empty();
			}
		}
	}
}
