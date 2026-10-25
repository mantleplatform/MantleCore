// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceLimits.h"

bool FMantlePlaceLimits::IsScriptPathCharacter(TCHAR Character)
{
	return (Character >= TEXT('A') && Character <= TEXT('Z'))
		|| (Character >= TEXT('a') && Character <= TEXT('z'))
		|| (Character >= TEXT('0') && Character <= TEXT('9'))
		|| Character == TEXT('_') || Character == TEXT('.') || Character == TEXT('-');
}

namespace
{
	/** The extension (".rml", ".png", ...) Path ends with, case-exact, of Extensions; 0 for none. */
	int32 MantleUIPathExtensionLength(const FString& Path, std::initializer_list<const TCHAR*> Extensions)
	{
		for (const TCHAR* Extension : Extensions)
		{
			if (Path.EndsWith(Extension, ESearchCase::CaseSensitive))
			{
				return FCString::Strlen(Extension);
			}
		}
		return 0;
	}
}

bool FMantlePlaceLimits::IsUIDocumentPath(const FString& Path)
{
	return IsUIPathWithExtension(Path, MantleUIPathExtensionLength(Path, { TEXT(".rml"), TEXT(".rcss") }));
}

bool FMantlePlaceLimits::IsUIImagePath(const FString& Path)
{
	return IsUIPathWithExtension(Path, MantleUIPathExtensionLength(Path, { TEXT(".png"), TEXT(".jpg") }));
}

bool FMantlePlaceLimits::IsUIPathWithExtension(const FString& Path, const int32 ExtensionLength)
{
	if (ExtensionLength <= 0 || !Path.StartsWith(TEXT("UI/"), ESearchCase::CaseSensitive) || Path.Contains(TEXT(".."), ESearchCase::CaseSensitive))
	{
		return false;
	}

	// One or more segments separated by single slashes, none empty, nothing outside the set.
	const FString Rest = Path.Mid(3);
	bool bSegmentHasCharacters = false;
	int32 SegmentLength = 0;
	for (const TCHAR Character : Rest)
	{
		if (Character == TEXT('/'))
		{
			if (!bSegmentHasCharacters)
			{
				return false;
			}
			bSegmentHasCharacters = false;
			SegmentLength = 0;
			continue;
		}
		if (!IsScriptPathCharacter(Character))
		{
			return false;
		}
		bSegmentHasCharacters = true;
		++SegmentLength;
	}
	// The file name is more than its extension: "UI/.rml" names nothing.
	return bSegmentHasCharacters && SegmentLength > ExtensionLength;
}

bool FMantlePlaceLimits::IsUIDocumentId(const FString& Id)
{
	if (!Id.StartsWith(TEXT("uid_"), ESearchCase::CaseSensitive) || Id.Len() < 5 || Id.Len() > 44)
	{
		return false;
	}
	for (int32 Index = 4; Index < Id.Len(); ++Index)
	{
		const TCHAR Character = Id[Index];
		if (!(Character >= TEXT('a') && Character <= TEXT('z')) && !(Character >= TEXT('0') && Character <= TEXT('9'))
			&& Character != TEXT('_'))
		{
			return false;
		}
	}
	return true;
}

namespace
{
	/** A letter, then letters, digits and underscores, at most MaxLength characters. */
	bool MantleIsLetterFirstName(const FString& Name, const int32 MaxLength)
	{
		if (Name.IsEmpty() || Name.Len() > MaxLength)
		{
			return false;
		}
		for (int32 Index = 0; Index < Name.Len(); ++Index)
		{
			const TCHAR Character = Name[Index];
			const bool bLetter = (Character >= TEXT('A') && Character <= TEXT('Z')) || (Character >= TEXT('a') && Character <= TEXT('z'));
			const bool bDigitOrUnderscore = (Character >= TEXT('0') && Character <= TEXT('9')) || Character == TEXT('_');
			if (Index == 0 ? !bLetter : !(bLetter || bDigitOrUnderscore))
			{
				return false;
			}
		}
		return true;
	}
}

bool FMantlePlaceLimits::IsStorageName(const FString& Name)
{
	return MantleIsLetterFirstName(Name, 64);
}

bool FMantlePlaceLimits::IsScriptPointParameterName(const FString& Name)
{
	return MantleIsLetterFirstName(Name, MaxScriptPointParameterNameLength);
}

/** "src_" and 32 lowercase hex digits: a script source id, as a place's scripts[] carries. */
bool FMantlePlaceLimits::IsScriptSourceId(const FString& Id)
{
	if (Id.Len() != 36 || !Id.StartsWith(TEXT("src_"), ESearchCase::CaseSensitive))
	{
		return false;
	}
	for (int32 Index = 4; Index < Id.Len(); ++Index)
	{
		const TCHAR Character = Id[Index];
		if (!FChar::IsDigit(Character) && (Character < TEXT('a') || Character > TEXT('f')))
		{
			return false;
		}
	}
	return true;
}

bool FMantlePlaceLimits::IsLibraryItemId(const FString& Id)
{
	if (Id.Len() != 37 || !Id.StartsWith(TEXT("itm_u"), ESearchCase::CaseSensitive))
	{
		return false;
	}
	for (int32 Index = 5; Index < Id.Len(); ++Index)
	{
		const TCHAR Character = Id[Index];
		if (!(Character >= TEXT('0') && Character <= TEXT('9')) && !(Character >= TEXT('a') && Character <= TEXT('f')))
		{
			return false;
		}
	}
	return true;
}

bool FMantlePlaceLimits::IsScriptPath(const FString& Path, FString* OutTopFolder)
{
	static const TCHAR* const TopFolders[] = { TEXT("Server"), TEXT("Client"), TEXT("Shared") };
	for (const TCHAR* Folder : TopFolders)
	{
		const FString Prefix = FString(Folder) + TEXT("/");
		if (!Path.StartsWith(Prefix, ESearchCase::CaseSensitive))
		{
			continue;
		}

		// One or more segments separated by single slashes, none empty, nothing outside the set.
		const FString Rest = Path.Mid(Prefix.Len());
		if (Rest.IsEmpty())
		{
			return false;
		}
		bool bSegmentHasCharacters = false;
		for (const TCHAR Character : Rest)
		{
			if (Character == TEXT('/'))
			{
				if (!bSegmentHasCharacters)
				{
					return false;
				}
				bSegmentHasCharacters = false;
				continue;
			}
			if (!IsScriptPathCharacter(Character))
			{
				return false;
			}
			bSegmentHasCharacters = true;
		}
		if (!bSegmentHasCharacters)
		{
			return false;
		}
		if (OutTopFolder)
		{
			*OutTopFolder = Folder;
		}
		return true;
	}
	return false;
}
