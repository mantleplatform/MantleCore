// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAttribution.h"

#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonWriter.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"

const TCHAR* FMantleAttribution::MetadataTag = TEXT("MantleAttribution");

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleAttributionPrivate
{
	/** Go's unicode.IsControl: C0, DEL and C1. */
	bool IsControlChar(TCHAR Char)
	{
		return Char < 0x20 || (Char >= 0x7F && Char <= 0x9F);
	}

	/**
	 * The format characters (Unicode Cf) that can disguise or reorder text, as the backend drops
	 * them; the zero-width joiner (U+200D) is kept for emoji sequences.
	 */
	bool IsInvisibleFormat(TCHAR Char)
	{
		return Char == 0x00AD || Char == 0x061C || Char == 0x180E || (Char >= 0x200B && Char <= 0x200F && Char != 0x200D)
			|| (Char >= 0x202A && Char <= 0x202E) || (Char >= 0x2060 && Char <= 0x2064) || (Char >= 0x2066 && Char <= 0x206F)
			|| Char == 0xFEFF || (Char >= 0xFFF9 && Char <= 0xFFFB);
	}

	/** Characters as the API counts them: code points, so a surrogate pair is one. */
	int32 CountCodePoints(const FString& Text)
	{
		int32 Count = 0;
		for (const TCHAR Char : Text)
		{
			Count += (Char >= 0xDC00 && Char <= 0xDFFF) ? 0 : 1;
		}
		return Count;
	}

	/** "/Game/Fab/Crates/" style: trimmed, one trailing slash. Empty for no folder. */
	FString FolderPrefix(const FString& Folder)
	{
		FString Prefix = Folder.TrimStartAndEnd();
		while (Prefix.EndsWith(TEXT("/")))
		{
			Prefix.LeftChopInline(1);
		}
		return Prefix.IsEmpty() ? FString() : Prefix + TEXT("/");
	}
}

FString FMantleAttribution::Normalize(const FString& Text)
{
	using namespace MantleAttributionPrivate;
	FString Out;
	Out.Reserve(Text.Len());
	bool bPendingSpace = false;
	for (const TCHAR Char : Text)
	{
		if (FChar::IsWhitespace(Char))
		{
			bPendingSpace = !Out.IsEmpty();
			continue;
		}
		if (IsControlChar(Char) || IsInvisibleFormat(Char))
		{
			continue;
		}
		if (bPendingSpace)
		{
			Out.AppendChar(TEXT(' '));
			bPendingSpace = false;
		}
		Out.AppendChar(Char);
	}
	return Out;
}

bool FMantleAttribution::IsWithinLimit(const FString& Text)
{
	return MantleAttributionPrivate::CountCodePoints(Normalize(Text)) <= MaxCharacters;
}

FString FMantleAttribution::ForPath(const FString& ObjectPath, const TArray<FMantleAttributionRule>& Rules)
{
	using namespace MantleAttributionPrivate;
	// The package: the object path up to its object name.
	FString Package = ObjectPath;
	int32 Dot = INDEX_NONE;
	if (Package.FindChar(TEXT('.'), Dot))
	{
		Package.LeftInline(Dot);
	}
	const FString* Best = nullptr;
	int32 BestLength = 0;
	for (const FMantleAttributionRule& Rule : Rules)
	{
		const FString Prefix = FolderPrefix(Rule.Folder.Path);
		// Package paths are case-insensitive in Unreal, so the rule matches however it was typed.
		if (Prefix.IsEmpty() || !Package.StartsWith(Prefix, ESearchCase::IgnoreCase) || Prefix.Len() <= BestLength)
		{
			continue;
		}
		Best = &Rule.Attribution;
		BestLength = Prefix.Len();
	}
	return Best ? Normalize(*Best) : FString();
}

FString FMantleAttribution::ForObjectPath(const FString& ObjectPath)
{
#if WITH_METADATA
	if (const UObject* Object = FSoftObjectPath(ObjectPath).ResolveObject())
	{
		if (UPackage* Package = Object->GetPackage())
		{
			if (const FString* Tag = Package->GetMetaData().FindValue(Object, MetadataTag))
			{
				return Normalize(*Tag);
			}
		}
	}
#endif
	const UMantleAttributionSettings* Settings = GetDefault<UMantleAttributionSettings>();
	return Settings ? ForPath(ObjectPath, Settings->Rules) : FString();
}

FString FMantleAttribution::MakeRequestBody(const TArray<TPair<FString, FString>>& IdsAndAttributions)
{
	FString Body;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Body);
	Writer->WriteObjectStart();
	Writer->WriteArrayStart(TEXT("attributions"));
	for (const TPair<FString, FString>& Entry : IdsAndAttributions)
	{
		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("id"), Entry.Key);
		Writer->WriteValue(TEXT("attribution"), Entry.Value);
		Writer->WriteObjectEnd();
	}
	Writer->WriteArrayEnd();
	Writer->WriteObjectEnd();
	Writer->Close();
	return Body;
}
