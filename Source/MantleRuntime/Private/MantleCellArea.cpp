// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCellArea.h"

FMantleCellArea::FResolver FMantleCellArea::Resolver = nullptr;
FMantleCellArea::FTakeDownResolver FMantleCellArea::TakeDownResolver = nullptr;

bool FMantleCellArea::Ask(UWorld* World, const FMantleCellAreaQuery& Query, FMantleCellAreaState& OutState, uint64& OutSerial)
{
	OutState = FMantleCellAreaState();
	OutSerial = 0;
	return World && Resolver && Resolver(World, Query, OutState, OutSerial);
}

bool FMantleCellArea::TakeDownSerial(UWorld* World, const bool bSolidOnly, uint64& OutSerial)
{
	OutSerial = 0;
	return World && TakeDownResolver && TakeDownResolver(World, bSolidOnly, OutSerial);
}
