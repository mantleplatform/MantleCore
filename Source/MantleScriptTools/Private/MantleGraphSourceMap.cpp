// Copyright Joshua Eger. All Rights Reserved.
#include "MantleGraphSourceMap.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Base64.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	constexpr TCHAR HeaderPrefix[] = TEXT("-- MANTLE_GRAPH_MAP v1 ");

	const FMantleGraphSourceMapEntry* FindLine(TArrayView<const FMantleGraphSourceMapEntry> Entries, int32 Line)
	{
		for (const FMantleGraphSourceMapEntry& Entry : Entries)
		{
			if (Entry.Line == Line)
			{
				return &Entry;
			}
		}
		return nullptr;
	}

	const FMantleGraphSourceMapEntry* FindInText(TArrayView<const FMantleGraphSourceMapEntry> Entries, const FString& Text)
	{
		for (int32 Index = 0; Index < Text.Len(); ++Index)
		{
			if (Text[Index] != TEXT(':'))
			{
				continue;
			}
			int32 End = Index + 1;
			while (End < Text.Len() && FChar::IsDigit(Text[End]))
			{
				++End;
			}
			if (End == Index + 1)
			{
				continue;
			}
			const int32 Line = FCString::Atoi(*Text.Mid(Index + 1, End - Index - 1));
			if (const FMantleGraphSourceMapEntry* Entry = FindLine(Entries, Line))
			{
				return Entry;
			}
			Index = End - 1;
		}
		return nullptr;
	}
}

bool MantleParseGraphSourceMap(const FString& Source, TArray<FMantleGraphSourceMapEntry>& OutEntries)
{
	OutEntries.Reset();
	TArray<FMantleGraphSourceMapEntry> ParsedEntries;
	TArray<FString> Lines;
	Source.ParseIntoArrayLines(Lines, false);
	for (const FString& Line : Lines)
	{
		if (!Line.StartsWith(HeaderPrefix, ESearchCase::CaseSensitive))
		{
			continue;
		}

		FString Json;
		if (!FBase64::Decode(Line.Mid(FCString::Strlen(HeaderPrefix)), Json))
		{
			return false;
		}
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			return false;
		}
		double Version = 0.0;
		const TArray<TSharedPtr<FJsonValue>>* RawEntries = nullptr;
		if (!Root->TryGetNumberField(TEXT("version"), Version) || Version != 1.0 ||
			!Root->TryGetArrayField(TEXT("entries"), RawEntries))
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& RawEntry : *RawEntries)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (!RawEntry.IsValid() || !RawEntry->TryGetObject(Object))
			{
				return false;
			}
			double LineNumber = 0.0;
			FString NodeId;
			FString NodeTitle;
			if (!(*Object)->TryGetNumberField(TEXT("line"), LineNumber) || !FMath::IsFinite(LineNumber) ||
				LineNumber < 1.0 || LineNumber > static_cast<double>(MAX_int32) ||
				FMath::FloorToDouble(LineNumber) != LineNumber ||
				!(*Object)->TryGetStringField(TEXT("id"), NodeId) || NodeId.IsEmpty() ||
				!(*Object)->TryGetStringField(TEXT("title"), NodeTitle) || NodeTitle.IsEmpty())
			{
				return false;
			}
			ParsedEntries.Add(FMantleGraphSourceMapEntry{static_cast<int32>(LineNumber), MoveTemp(NodeId), MoveTemp(NodeTitle)});
		}
		if (ParsedEntries.IsEmpty())
		{
			return false;
		}
		OutEntries = MoveTemp(ParsedEntries);
		return true;
	}
	return false;
}

const FMantleGraphSourceMapEntry* MantleFindGraphSourceMapEntry(
	TArrayView<const FMantleGraphSourceMapEntry> Entries,
	const FString& Message,
	const FString& Traceback)
{
	if (const FMantleGraphSourceMapEntry* Entry = FindInText(Entries, Message))
	{
		return Entry;
	}
	return FindInText(Entries, Traceback);
}
