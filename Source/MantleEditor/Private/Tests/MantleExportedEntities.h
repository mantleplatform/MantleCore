// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "MantleCells.h"
#include "MantlePlaceExporter.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Every entity an export wrote, in the root place file and in its cell files. Since cells, scenery is planned out of the root
 * (MantleCells.h), so a test that looks for an exported entity reads both, as players receive both.
 */
namespace MantleExportedEntities
{
	inline TArray<const FMantleEntity*> All(const FMantleExportResult& Result)
	{
		TArray<const FMantleEntity*> Out;
		for (const FMantleEntity& Entity : Result.Place.Entities)
		{
			Out.Add(&Entity);
		}
		for (const FMantlePlannedCell& Cell : Result.Cells)
		{
			for (const FMantleEntity& Entity : Cell.Entities)
			{
				Out.Add(&Entity);
			}
		}
		return Out;
	}

	/** The first exported entity with this name, root or cell; null when there is none. */
	inline const FMantleEntity* Named(const FMantleExportResult& Result, const FString& Name)
	{
		for (const FMantleEntity* Entity : All(Result))
		{
			if (Entity->Name == Name)
			{
				return Entity;
			}
		}
		return nullptr;
	}
}

#endif
