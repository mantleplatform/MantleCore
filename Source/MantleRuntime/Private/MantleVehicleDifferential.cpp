// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleDifferential.h"

FString MantleDifferentialName(const EMantleVehicleDifferential Type)
{
	switch (Type)
	{
	case EMantleVehicleDifferential::Open: return TEXT("open");
	case EMantleVehicleDifferential::LimitedSlip: return TEXT("limitedSlip");
	case EMantleVehicleDifferential::Locked: return TEXT("locked");
	case EMantleVehicleDifferential::Selectable: return TEXT("selectable");
	default: return FString();
	}
}

EMantleVehicleDifferential MantleDifferentialFromName(const FString& Name)
{
	for (const EMantleVehicleDifferential Type : { EMantleVehicleDifferential::Open, EMantleVehicleDifferential::LimitedSlip, EMantleVehicleDifferential::Locked,
		EMantleVehicleDifferential::Selectable })
	{
		if (Name.Equals(MantleDifferentialName(Type), ESearchCase::CaseSensitive))
		{
			return Type;
		}
	}
	return EMantleVehicleDifferential::None;
}
