// Copyright Joshua Eger. All Rights Reserved.
#include "MantleItemSettings.h"
#include "MantleCosmeticExport.h"
#include "MantleMirrorBake.h"
#include "Engine/StaticMesh.h"

namespace MantleItemSettingsPrivate
{
	bool IsPairIdChar(const TCHAR Char)
	{
		return (Char >= TEXT('a') && Char <= TEXT('z')) || (Char >= TEXT('A') && Char <= TEXT('Z'))
			|| (Char >= TEXT('0') && Char <= TEXT('9')) || Char == TEXT('_') || Char == TEXT('-');
	}
}

bool FMantleItemSettings::IsValidPairId(const FString& Id)
{
	if (Id.Len() < 1 || Id.Len() > 64)
	{
		return false;
	}
	for (const TCHAR Char : Id)
	{
		if (!MantleItemSettingsPrivate::IsPairIdChar(Char))
		{
			return false;
		}
	}
	return true;
}

FString FMantleItemSettings::MakePairId(const FString& ItemName, const FGuid& Guid)
{
	FString Base;
	for (const TCHAR Char : ItemName.TrimStartAndEnd().ToLower())
	{
		Base.AppendChar(MantleItemSettingsPrivate::IsPairIdChar(Char) ? Char : TEXT('-'));
	}
	Base.LeftInline(48);
	if (Base.IsEmpty())
	{
		Base = TEXT("earrings");
	}
	return Base + TEXT("-") + Guid.ToString(EGuidFormats::Digits).ToLower().Left(8);
}

FString FMantleItemSettings::WithSideSuffix(const FString& Name, const bool bRight)
{
	static const TCHAR* const Suffixes[] = { TEXT(" (left)"), TEXT(" (right)") };
	FString Base = Name.TrimStartAndEnd();
	if (Base.IsEmpty())
	{
		// No name yet: leave it empty so the name check still says so.
		return Base;
	}
	for (const TCHAR* Suffix : Suffixes)
	{
		if (Base.EndsWith(Suffix, ESearchCase::IgnoreCase))
		{
			Base.LeftChopInline(FCString::Strlen(Suffix));
			break;
		}
	}
	return Base + Suffixes[bRight ? 1 : 0];
}

void FMantleItemSettings::MirrorAttach(const FVector3f& Location, const FVector3f& Rotation, const FVector3f& Scale,
	FVector3f& OutLocation, FVector3f& OutRotation, FVector3f& OutScale)
{
	// The panel and the package keep the rotation as (pitch, yaw, roll) degrees in (X, Y, Z).
	const FTransform Placement(FRotator(Rotation.X, Rotation.Y, Rotation.Z), FVector(Location), FVector(Scale));
	const FTransform Mirrored = FMantleMirrorBake::ReflectTransform(Placement, MirrorSigns());
	const FRotator Angles = Mirrored.Rotator();
	OutLocation = FVector3f(Mirrored.GetLocation());
	OutRotation = FVector3f(Angles.Pitch, Angles.Yaw, Angles.Roll);
	OutScale = FVector3f(Mirrored.GetScale3D());
}

bool FMantleItemSettings::MakeRightDraft(const FMantleCosmeticDraft& Left, FMantleCosmeticDraft& OutRight, FString& OutWhy)
{
	OutWhy.Reset();
	UStaticMesh* Mesh = Cast<UStaticMesh>(Left.Source.Get());
	if (!Mesh)
	{
		OutWhy = TEXT("An earring pair is made from a static mesh: the right earring is its mirror image.");
		return false;
	}
	FVector PositiveScale = FVector::OneVector;
	UStaticMesh* Mirrored = FMantleMirrorBake::MirroredCopy(Mesh, MirrorSigns(), PositiveScale, OutWhy);
	if (!Mirrored)
	{
		if (OutWhy.IsEmpty())
		{
			OutWhy = TEXT("The right earring could not be made as the mirror image of the left one.");
		}
		return false;
	}
	OutRight = Left;
	OutRight.Source = Mirrored;
	OutRight.Slot = TEXT("acc_ear_r");
	OutRight.EarringSide = FMantleCosmeticDraft::EEarringSide::Right;
	OutRight.Name = WithSideSuffix(Left.Name, true);
	MirrorAttach(Left.AttachLocation, Left.AttachRotation, Left.AttachScale, OutRight.AttachLocation, OutRight.AttachRotation, OutRight.AttachScale);
	return true;
}
