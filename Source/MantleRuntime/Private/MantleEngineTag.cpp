// Copyright Joshua Eger. All Rights Reserved.
#include "MantleEngineTag.h"

#include "Misc/EngineVersion.h"

namespace MantleEngineTagPrivate
{
	/** A decimal number of 1 to 9 digits, nothing else. */
	bool ParseNumber(const FString& Text, int32& OutValue)
	{
		if (Text.IsEmpty() || Text.Len() > 9)
		{
			return false;
		}
		int32 Value = 0;
		for (const TCHAR Char : Text)
		{
			if (Char < TEXT('0') || Char > TEXT('9'))
			{
				return false;
			}
			Value = Value * 10 + (Char - TEXT('0'));
		}
		OutValue = Value;
		return true;
	}
}

FString MantleEngineTag::Current()
{
	const FEngineVersion& Version = FEngineVersion::Current();
	return FString::Printf(TEXT("%u.%u.%u"), Version.GetMajor(), Version.GetMinor(), Version.GetPatch());
}

bool MantleEngineTag::Parse(const FString& Tag, int32& OutMajor, int32& OutMinor, int32& OutPatch)
{
	using namespace MantleEngineTagPrivate;

	FString Release = Tag;
	int32 Dash = INDEX_NONE;
	if (Tag.FindChar(TEXT('-'), Dash))
	{
		int32 Changelist = 0;
		if (!ParseNumber(Tag.Mid(Dash + 1), Changelist))
		{
			return false;
		}
		Release = Tag.Left(Dash);
	}
	TArray<FString> Parts;
	Release.ParseIntoArray(Parts, TEXT("."), /*bCullEmpty*/ false);
	if (Parts.Num() != 3)
	{
		return false;
	}
	int32 Major = 0;
	int32 Minor = 0;
	int32 Patch = 0;
	if (!ParseNumber(Parts[0], Major) || !ParseNumber(Parts[1], Minor) || !ParseNumber(Parts[2], Patch))
	{
		return false;
	}
	OutMajor = Major;
	OutMinor = Minor;
	OutPatch = Patch;
	return true;
}

bool MantleEngineTag::Matches(const FString& FileTag, const FString& ThisTag)
{
	int32 FileMajor = 0;
	int32 FileMinor = 0;
	int32 FilePatch = 0;
	int32 ThisMajor = 0;
	int32 ThisMinor = 0;
	int32 ThisPatch = 0;
	return Parse(FileTag, FileMajor, FileMinor, FilePatch) && Parse(ThisTag, ThisMajor, ThisMinor, ThisPatch)
		&& FileMajor == ThisMajor && FileMinor == ThisMinor && FilePatch == ThisPatch;
}
