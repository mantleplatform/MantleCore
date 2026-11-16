// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantlePlaceTypes.h"

/**
 * One level of the cell grid (contract 4.1): flat squares in X and Y with their origin at world (0, 0). Content goes to the smallest
 * level whose MaxDrawDistance is at least its own draw distance.
 */
struct FMantleCellGridLevel
{
	/** cm, a whole number. */
	int64 CellSize = 0;

	/** cm. */
	double MaxDrawDistance = 0.0;
};

/** One cell file as the index lists it: the uploaded asset, and what the plugin's upload said about its bytes. */
struct FMantleCellFileRef
{
	/** ast_u id of a "cell" asset. */
	FString Id;

	/** "sha256:..." of the uploaded (and, for a cell file, stored) bytes. */
	FString Hash;

	/** Length of the uploaded container, in bytes. */
	int64 Bytes = 0;
};

/** One occupied cell of the grid: where it is, how far it must be loaded from, and its files. */
struct FMantleCellInfo
{
	int32 Level = 0;
	int32 X = 0;
	int32 Y = 0;

	/** The union of the cell's content's world bounding boxes (cm); what a loader measures distance to. */
	FBox Bounds = FBox(ForceInit);

	/** The largest drawDistance in the cell, cm: the distance this cell is loaded from. */
	double DrawDistance = 0.0;

	/** One to FMantlePlaceCells::MaxFilesPerCell; the loader treats them as one unit. */
	TArray<FMantleCellFileRef> Files;

	/** The exporter's counts for the cell's files together (6.1); the loader checks them against what it read. */
	int64 Entities = 0;
	int64 Instances = 0;
	int64 Solid = 0;

	FString KeyString() const
	{
		return FString::Printf(TEXT("%d/%d/%d"), Level, X, Y);
	}
};

/** The cell index document (version 1), as read. */
struct FMantleCellIndex
{
	int32 Version = 0;
	TArray<FMantleCellGridLevel> Levels;

	/** Sorted by (Level, X, Y), each key once. */
	TArray<FMantleCellInfo> Cells;
};

/** What reading one cell's files came to: its entities, in file order, and the counts the index promised. */
struct FMantleCellContent
{
	/** The cell's position in FMantleCellIndex::Cells. */
	int32 CellIndex = INDEX_NONE;

	TArray<FMantleEntity> Entities;

	/** Instances of every InstancedMesh, and solid things (a StaticMesh that is not collision "none", plus the instances of an InstancedMesh that is not), counted as the index counts them. */
	int64 Instances = 0;
	int64 Solid = 0;
};

/**
 * Cells (stage 1 of large places): a place file may hold its scenery in cell files, listed by one cell index, instead of in its own
 * "entities" (contract: D:\Mantle\Untracked\local\build-plan\contracts\cells-format.md).
 *
 * This is the reading side: the two containers (MCI1 and MCF1), the cell index, a cell file's entities, and the merge that puts the
 * cells' entities into the place. FMantleCellLoader downloads the files and runs these; the parser reads the place's "cells" key.
 *
 * All of it runs on any thread except Merge, which changes a place.
 */
class MANTLERUNTIME_API FMantlePlaceCells
{
public:
	/** The inflated JSON of a cell file: at most the place file's own cap (the backend refuses a larger one too). */
	static constexpr uint32 MaxCellInflatedBytes = 8u * 1024u * 1024u;

	/** The inflated JSON of the cell index: the general asset cap. */
	static constexpr uint32 MaxIndexInflatedBytes = 64u * 1024u * 1024u;

	/** A container's header: its four-character magic and the uint32 inflated size. */
	static constexpr int32 ContainerHeaderBytes = 8;

	static constexpr int32 MaxGridLevels = 4;
	static constexpr int32 MaxFilesPerCell = 64;

	/** The "version" of the index and of a cell file that this game reads. */
	static constexpr int32 DocumentVersion = 1;

	/** The magic of a cell index and of a cell file. */
	static const ANSICHAR* const IndexMagic;
	static const ANSICHAR* const CellMagic;

	/**
	 * Unpacks a container: Magic, a uint32 little-endian inflated size, then one zlib stream. The size is held to 1..MaxInflated before
	 * anything is allocated, and the stream must inflate to exactly that many bytes. False,
	 * with the reason, otherwise; Out is empty then. Any thread.
	 */
	static bool Unpack(TConstArrayView<uint8> Container, const ANSICHAR* Magic, uint32 MaxInflated, TArray<uint8>& Out, FString* OutReason = nullptr);

	/** The inflated bytes as text: UTF-8, no byte order mark. False, with the reason, when they are not. */
	static bool ToText(TConstArrayView<uint8> Utf8, FString& OutText, FString* OutReason = nullptr);

	/**
	 * Reads the cell index document (contract 6.1 and 4.2): its version, the grid, and every cell, which it holds to the contract's
	 * rules (sorted, each key once, ranges, 1 to 64 files with uploaded ids each in one cell only, no key it does not know). The text
	 * is read as a stream: nothing but the typed result is kept, so a 64 MiB index costs what its cells cost. False, with the reason
	 * and where, otherwise. Any thread.
	 */
	static bool ReadIndex(const FString& Json, FMantleCellIndex& Out, FString& OutError);

	/**
	 * Reads one cell file's entities (contract 6.2) with the place parser's own entity rules, then the cell rules: StaticMesh and
	 * InstancedMesh only, no parent, tags, dynamic, interactable or physics, a drawDistance, an InstancedMesh anchored at a whole-cm
	 * location with no rotation or scale, and every uploaded mesh and material it names listed in the root's manifest with the right
	 * kind (RootAssets, the root's asset ids by kind). Appends to Out.Entities and counts Instances and Solid. False with the errors
	 * otherwise. Any thread.
	 */
	static bool ReadCellFile(const FString& Json, const TMap<FString, EMantleAssetKind>& RootAssets, FMantleCellContent& Out, TArray<FMantleParseError>& OutErrors);

	/** The root's asset ids by kind, what ReadCellFile checks a cell's references against. */
	static TMap<FString, EMantleAssetKind> RootAssetKinds(const FMantlePlace& Root);

	/**
	 * Adds the cells' entities to Place.Entities, in the order of the cells, and sets bCellsMerged. First it holds each cell's counts to the index's
	 * (entities, instances, solid) and every entity id to one id space across the root and all the cells; on any failure Place is not changed and
	 * the errors say which cell and which id. Contents are moved from.
	 */
	static bool Merge(FMantlePlace& Place, const FMantleCellIndex& Index, TArray<FMantleCellContent>& Contents, TArray<FMantleParseError>& OutErrors);

	/** The checks Merge makes before it changes anything: each cell's counts against the index's, and one id space across the place and its cells. False with the errors. */
	static bool Validate(const FMantlePlace& Place, const FMantleCellIndex& Index, const TArray<FMantleCellContent>& Contents, TArray<FMantleParseError>& OutErrors);

	/**
	 * A place that streams its cells (stage 2) is built from its root alone, but its assets are still prepared for every entity the cells hold (until assets
	 * follow the cells too). OutPlace is Root followed by copies of every cell's entities, after the same checks as Merge; Root and Contents are unchanged.
	 */
	static bool AssembleForAssets(const FMantlePlace& Root, const FMantleCellIndex& Index, const TArray<FMantleCellContent>& Contents, FMantlePlace& OutPlace, TArray<FMantleParseError>& OutErrors);

	/**
	 * Copies of every cell entity with its list of instances emptied, in cell order: what a standalone game retains beside its root so that the script world makes a record for each
	 * (a ray that hits any of them names it, and a server script finds it), without holding the million instances of the cells' lists.
	 */
	static void SceneryRecords(const TArray<FMantleCellContent>& Contents, TArray<FMantleEntity>& OutEntities);
};
