// Copyright Joshua Eger. All Rights Reserved.
#include "MantleRmlDocumentRules.h"
#include "MantleDocumentImages.h" // #556 item 5

namespace
{
	/** Constructs a creator document may not use (#549), matched ignoring case. `data-rml` is
	    handled on its own: only as an attribute name. */
	const TCHAR* const MantleRmlRefusedSubstrings[] = {
		TEXT("<script"),
		TEXT("@font-face"),
	};

	/** The properties RmlUi parses but Slate does not draw. `backdrop-filter` is matched on its own name,
	    never as `filter`. */
	const TCHAR* const MantleRmlUndrawnPropertyNames[] = {
		TEXT("filter"),
		TEXT("backdrop-filter"),
		TEXT("mask-image"),
		// box-shadow draws: its texture is generated on the CPU (RmlUiLib VERSION, patch 13).
	};

	const TCHAR* const MantleRmlVoidElements[] = {
		TEXT("br"), TEXT("img"), TEXT("input"), TEXT("hr"), TEXT("link"), TEXT("meta"), TEXT("col"), TEXT("source"),
	};

	const TCHAR* const MantleRmlReservedVariableNames[] = {
		TEXT("it"), TEXT("it_index"), TEXT("ev"), TEXT("true"), TEXT("false"), TEXT("size"), TEXT("literal"),
	};

	constexpr int32 MantleRmlMaxVariableNameLength = 64;

	bool MantleRmlIsIdentifierChar(const TCHAR Char)
	{
		return (Char >= TEXT('a') && Char <= TEXT('z')) || (Char >= TEXT('A') && Char <= TEXT('Z')) ||
			(Char >= TEXT('0') && Char <= TEXT('9')) || Char == TEXT('_');
	}

	bool MantleRmlIsAsciiLetter(const TCHAR Char)
	{
		return (Char >= TEXT('a') && Char <= TEXT('z')) || (Char >= TEXT('A') && Char <= TEXT('Z'));
	}

	/** Tag names: letters, digits, '-', '_', ':' and '.'. */
	bool MantleRmlIsTagNameChar(const TCHAR Char)
	{
		return MantleRmlIsIdentifierChar(Char) || Char == TEXT('-') || Char == TEXT(':') || Char == TEXT('.');
	}

	bool MantleRmlStartsAt(const FString& Text, const int32 At, const TCHAR* Literal)
	{
		const int32 Length = FCString::Strlen(Literal);
		return At + Length <= Text.Len() && FCString::Strnicmp(*Text + At, Literal, Length) == 0;
	}

	/** Index just past the next Terminator at or after From, or the end of the text. */
	int32 MantleRmlSkipPast(const FString& Text, const int32 From, const TCHAR* Terminator)
	{
		const int32 Found = Text.Find(Terminator, ESearchCase::CaseSensitive, ESearchDir::FromStart, From);
		return Found == INDEX_NONE ? Text.Len() : Found + FCString::Strlen(Terminator);
	}

	/** A `data-rml` attribute: whitespace (or a quote, or the start) before, and no identifier
	    character after, so `data-rml="..."` and `data-rml-x` match and `data-rmlx` does not. */
	bool MantleRmlHasDataRmlAttribute(const FString& Text)
	{
		const TCHAR* const Needle = TEXT("data-rml");
		const int32 NeedleLength = FCString::Strlen(Needle);
		int32 From = 0;
		while (true)
		{
			const int32 Found = Text.Find(Needle, ESearchCase::IgnoreCase, ESearchDir::FromStart, From);
			if (Found == INDEX_NONE)
			{
				return false;
			}
			const TCHAR Before = Found > 0 ? Text[Found - 1] : TEXT(' ');
			const int32 AfterIndex = Found + NeedleLength;
			const TCHAR After = AfterIndex < Text.Len() ? Text[AfterIndex] : TEXT(' ');
			const bool bBeforeOk = FChar::IsWhitespace(Before) || Before == TEXT('"') || Before == TEXT('\'');
			if (bBeforeOk && !MantleRmlIsIdentifierChar(After))
			{
				return true;
			}
			From = Found + 1;
		}
	}

	bool MantleRmlIsVoidElement(const FString& Name)
	{
		for (const TCHAR* Void : MantleRmlVoidElements)
		{
			if (Name.Equals(Void, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	/** The index of the '>' closing the start tag whose text begins at Start, skipping quoted attribute values, or INDEX_NONE. */
	int32 MantleRmlFindTagEnd(const FString& Text, const int32 Start)
	{
		TCHAR Quote = 0;
		for (int32 End = Start; End < Text.Len(); ++End)
		{
			const TCHAR Char = Text[End];
			if (Quote)
			{
				if (Char == Quote)
				{
					Quote = 0;
				}
			}
			else if (Char == TEXT('"') || Char == TEXT('\''))
			{
				Quote = Char;
			}
			else if (Char == TEXT('>'))
			{
				return End;
			}
		}
		return INDEX_NONE;
	}

	/**
	 * The first <Name ...> start tag, matched ignoring case, whose name is followed by whitespace,
	 * '>' or '/' (so "head" never matches <header>): the index of its '<' and of its '>', skipping
	 * quoted attribute values.
	 */
	bool MantleRmlFindStartTag(const FString& Rml, const TCHAR* Name, int32& OutOpen, int32& OutClose, const int32 Start = 0)
	{
		const FString Needle = FString(TEXT("<")) + Name;
		const int32 Length = Rml.Len();
		int32 From = Start;
		while (true)
		{
			const int32 Found = Rml.Find(Needle, ESearchCase::IgnoreCase, ESearchDir::FromStart, From);
			if (Found == INDEX_NONE)
			{
				return false;
			}
			const int32 After = Found + Needle.Len();
			if (After >= Length)
			{
				return false;
			}
			const TCHAR Next = Rml[After];
			if (FChar::IsWhitespace(Next) || Next == TEXT('>') || Next == TEXT('/'))
			{
				const int32 End = MantleRmlFindTagEnd(Rml, After);
				if (End == INDEX_NONE)
				{
					return false;
				}
				OutOpen = Found;
				OutClose = End;
				return true;
			}
			From = After;
		}
	}

	/** Text with every CSS and HTML comment replaced by one space. */
	FString MantleRmlStripComments(const FString& Text)
	{
		FString Out;
		Out.Reserve(Text.Len());
		int32 Index = 0;
		while (Index < Text.Len())
		{
			if (MantleRmlStartsAt(Text, Index, TEXT("/*")))
			{
				Index = MantleRmlSkipPast(Text, Index + 2, TEXT("*/"));
				Out.AppendChar(TEXT(' '));
			}
			else if (MantleRmlStartsAt(Text, Index, TEXT("<!--")))
			{
				Index = MantleRmlSkipPast(Text, Index + 4, TEXT("-->"));
				Out.AppendChar(TEXT(' '));
			}
			else
			{
				Out.AppendChar(Text[Index]);
				++Index;
			}
		}
		return Out;
	}

	/** Whether Clean (comments already stripped) declares the property Name; see FindUndrawnProperties. */
	bool MantleRmlDeclaresProperty(const FString& Clean, const TCHAR* Name)
	{
		const TCHAR* const DataStyle = TEXT("data-style-");
		const int32 DataStyleLength = FCString::Strlen(DataStyle);
		const int32 NameLength = FCString::Strlen(Name);
		const int32 Length = Clean.Len();
		int32 From = 0;
		while (true)
		{
			const int32 Found = Clean.Find(Name, ESearchCase::IgnoreCase, ESearchDir::FromStart, From);
			if (Found == INDEX_NONE)
			{
				return false;
			}
			From = Found + 1;

			int32 After = Found + NameLength;
			while (After < Length && FChar::IsWhitespace(Clean[After]))
			{
				++After;
			}
			const TCHAR Next = After < Length ? Clean[After] : TEXT('\0');

			// `data-style-filter="..."`: RmlUi sets the property from the attribute's value.
			if (Next == TEXT('=') && Found >= DataStyleLength && MantleRmlStartsAt(Clean, Found - DataStyleLength, DataStyle))
			{
				const int32 AttributeStart = Found - DataStyleLength;
				const TCHAR AttributeBefore = AttributeStart > 0 ? Clean[AttributeStart - 1] : TEXT(' ');
				if (FChar::IsWhitespace(AttributeBefore) || AttributeBefore == TEXT('"') || AttributeBefore == TEXT('\''))
				{
					return true;
				}
				continue;
			}
			if (Next != TEXT(':'))
			{
				continue;
			}
			// A declaration starts a block, follows another, or opens a style attribute.
			int32 Before = Found - 1;
			while (Before >= 0 && FChar::IsWhitespace(Clean[Before]))
			{
				--Before;
			}
			if (Before < 0)
			{
				return true;
			}
			const TCHAR Previous = Clean[Before];
			if (Previous == TEXT('{') || Previous == TEXT(';') || Previous == TEXT('"') || Previous == TEXT('\''))
			{
				return true;
			}
		}
	}

	/**
	 * The value of attribute Name in the start tag Text[Open..Close] (from MantleRmlFindStartTag),
	 * quoted or not, matched ignoring case; false when the tag has none.
	 */
	bool MantleRmlFindAttribute(const FString& Text, const int32 Open, const int32 Close, const TCHAR* Name, FString& OutValue)
	{
		const int32 NameLength = FCString::Strlen(Name);
		TCHAR Quote = 0;
		for (int32 Index = Open + 1; Index < Close; ++Index)
		{
			const TCHAR Char = Text[Index];
			if (Quote)
			{
				if (Char == Quote)
				{
					Quote = 0;
				}
				continue;
			}
			if (Char == TEXT('"') || Char == TEXT('\''))
			{
				Quote = Char;
				continue;
			}
			if (!FChar::IsWhitespace(Text[Index - 1]) || !MantleRmlStartsAt(Text, Index, Name))
			{
				continue;
			}
			int32 At = Index + NameLength;
			while (At < Close && FChar::IsWhitespace(Text[At]))
			{
				++At;
			}
			if (At >= Close || Text[At] != TEXT('='))
			{
				continue;
			}
			++At;
			while (At < Close && FChar::IsWhitespace(Text[At]))
			{
				++At;
			}
			if (At >= Close)
			{
				return false;
			}
			const TCHAR ValueQuote = Text[At];
			int32 End = At;
			if (ValueQuote == TEXT('"') || ValueQuote == TEXT('\''))
			{
				++At;
				End = At;
				while (End < Close && Text[End] != ValueQuote)
				{
					++End;
				}
			}
			else
			{
				while (End < Close && !FChar::IsWhitespace(Text[End]))
				{
					++End;
				}
				// `href=a.rcss/>`: the '/' closes the tag.
				if (End > At && Text[End - 1] == TEXT('/'))
				{
					--End;
				}
			}
			OutValue = Text.Mid(At, End - At);
			return true;
		}
		return false;
	}
}

namespace MantleRmlDocumentRules
{
	void FindUndrawnProperties(const FString& Text, TArray<FString>& InOut)
	{
		bool bAnyName = false;
		for (const TCHAR* Property : MantleRmlUndrawnPropertyNames)
		{
			bAnyName = bAnyName || Text.Contains(Property, ESearchCase::IgnoreCase);
		}
		if (!bAnyName)
		{
			return;
		}
		const FString Clean = MantleRmlStripComments(Text);
		for (const TCHAR* Property : MantleRmlUndrawnPropertyNames)
		{
			if (MantleRmlDeclaresProperty(Clean, Property))
			{
				InOut.AddUnique(FString(Property));
			}
		}
	}

	TArray<FString> LinkHrefs(const FString& Rml)
	{
		TArray<FString> Hrefs;
		const FString Clean = MantleRmlStripComments(Rml);
		int32 From = 0;
		int32 Open = INDEX_NONE;
		int32 Close = INDEX_NONE;
		while (MantleRmlFindStartTag(Clean, TEXT("link"), Open, Close, From))
		{
			From = Close + 1;
			FString Href;
			if (MantleRmlFindAttribute(Clean, Open, Close, TEXT("href"), Href))
			{
				Href.TrimStartAndEndInline();
				if (!Href.IsEmpty())
				{
					Hrefs.Add(Href);
				}
			}
		}
		return Hrefs;
	}

	FString InjectStyleSheet(const FString& Rml, const FString& StyleSheet)
	{
		const FString Style = TEXT("<style>") + StyleSheet + TEXT("</style>");
		int32 Open = INDEX_NONE;
		int32 Close = INDEX_NONE;
		if (MantleRmlFindStartTag(Rml, TEXT("head"), Open, Close))
		{
			if (Rml[Close - 1] == TEXT('/'))
			{
				// <head/>: open it up around the style.
				return Rml.Left(Close - 1) + TEXT(">") + Style + TEXT("</head>") + Rml.Mid(Close + 1);
			}
			return Rml.Left(Close + 1) + Style + Rml.Mid(Close + 1);
		}
		const FString Head = TEXT("<head>") + Style + TEXT("</head>");
		if (MantleRmlFindStartTag(Rml, TEXT("rml"), Open, Close))
		{
			return Rml.Left(Close + 1) + Head + Rml.Mid(Close + 1);
		}
		if (MantleRmlFindStartTag(Rml, TEXT("body"), Open, Close))
		{
			return Rml.Left(Open) + Head + Rml.Mid(Open);
		}
		return Rml;
	}

	FString PrepareMarkup(const FString& Source, FString& OutOtherModel)
	{
		// The base sheet goes in as the head's first <style>, so it is there for the first layout
		// and every sheet of the document's own comes after it and wins.
		return InjectStyleSheet(ApplyDefaultDataModel(Source, OutOtherModel), FString(UTF8_TO_TCHAR(BaseStyleSheet())));
	}

	bool UpdateFrameCostWatch(FFrameCostWatch& Watch, const double AverageMs, const int32 AverageFrames, const double DeltaSeconds)
	{
		if (Watch.bWarned)
		{
			// Hysteresis: only a clear drop re-arms, so a document hovering at the threshold warns once.
			if (AverageFrames >= FrameCostMinFrames && AverageMs < FrameCostWarnMs * FrameCostRearmFraction)
			{
				Watch.bWarned = false;
				Watch.SecondsAbove = 0.0;
			}
			return false;
		}
		if (AverageFrames < FrameCostMinFrames || !(AverageMs > FrameCostWarnMs))
		{
			// Too few frames to judge, or back under: the run above starts over.
			Watch.SecondsAbove = 0.0;
			return false;
		}
		Watch.SecondsAbove += FMath::Clamp(DeltaSeconds, 0.0, FrameCostMaxStepSeconds);
		if (Watch.SecondsAbove < FrameCostWarnSeconds)
		{
			return false;
		}
		Watch.bWarned = true;
		Watch.SecondsAbove = 0.0;
		return true;
	}

	int32 Utf8Bytes(const FString& Text)
	{
		const FTCHARToUTF8 Converted(*Text, Text.Len());
		return Converted.Length();
	}

	const TCHAR* FindRefusedConstruct(const FString& Text)
	{
		if (MantleRmlHasDataRmlAttribute(Text))
		{
			return TEXT("data-rml");
		}
		for (const TCHAR* Refused : MantleRmlRefusedSubstrings)
		{
			if (Text.Contains(Refused, ESearchCase::IgnoreCase))
			{
				return Refused;
			}
		}
		return nullptr;
	}

	bool CheckFileSet(const TMap<FString, FString>& Files, FString& OutError)
	{
		for (const auto& File : Files)
		{
			// #556 item 5: an image's entry is a reference to its pixels, not text to scan.
			if (MantleDocumentImages::IsImagePath(File.Key))
			{
				continue;
			}
			const int32 Bytes = Utf8Bytes(File.Value);
			if (Bytes > MaxFileBytes)
			{
				OutError = FString::Printf(TEXT("%s is %d bytes; the limit is %d"), *File.Key, Bytes, MaxFileBytes);
				return false;
			}
			if (const TCHAR* Refused = FindRefusedConstruct(File.Value))
			{
				OutError = FString::Printf(TEXT("%s uses %s, which UI documents may not use"), *File.Key, Refused);
				return false;
			}
			if (File.Key.EndsWith(TEXT(".rml"), ESearchCase::IgnoreCase))
			{
				const int32 Depth = MaxTagDepth(File.Value);
				if (Depth > MaxNestingDepth)
				{
					OutError = FString::Printf(TEXT("%s nests elements %d deep; the limit is %d"), *File.Key, Depth, MaxNestingDepth);
					return false;
				}
			}
		}
		return true;
	}

	FString CheckLoadedStructure(const int32 Elements, const int32 Depth, FString& OutReason)
	{
		if (Elements > MaxElements)
		{
			OutReason = TEXT("element budget");
			return FString::Printf(TEXT("%d elements; the limit is %d"), Elements, MaxElements);
		}
		if (Depth > MaxNestingDepth)
		{
			// The script API documents "element budget" as the reason a document closes for its
			// structure (Closed); the detail names the depth.
			OutReason = TEXT("element budget");
			return FString::Printf(TEXT("elements nest %d deep; the limit is %d"), Depth, MaxNestingDepth);
		}
		return FString();
	}

	int32 MaxTagDepth(const FString& Rml)
	{
		const int32 Length = Rml.Len();
		int32 Depth = 0;
		int32 Deepest = 0;
		int32 Index = 0;
		while (Index < Length)
		{
			if (Rml[Index] != TEXT('<'))
			{
				++Index;
				continue;
			}
			if (MantleRmlStartsAt(Rml, Index, TEXT("<!--")))
			{
				Index = MantleRmlSkipPast(Rml, Index + 4, TEXT("-->"));
				continue;
			}
			if (MantleRmlStartsAt(Rml, Index, TEXT("<![CDATA[")))
			{
				Index = MantleRmlSkipPast(Rml, Index + 9, TEXT("]]>"));
				continue;
			}
			if (Index + 1 < Length && (Rml[Index + 1] == TEXT('!') || Rml[Index + 1] == TEXT('?')))
			{
				Index = MantleRmlSkipPast(Rml, Index + 2, TEXT(">"));
				continue;
			}

			const bool bClosing = Index + 1 < Length && Rml[Index + 1] == TEXT('/');
			const int32 NameStart = Index + (bClosing ? 2 : 1);
			int32 NameEnd = NameStart;
			while (NameEnd < Length && MantleRmlIsTagNameChar(Rml[NameEnd]))
			{
				++NameEnd;
			}
			if (NameEnd == NameStart || !MantleRmlIsAsciiLetter(Rml[NameStart]))
			{
				// Text, such as a comparison inside a data expression.
				++Index;
				continue;
			}

			// The tag's '>', skipping quoted attribute values.
			const int32 End = MantleRmlFindTagEnd(Rml, NameEnd);
			if (End == INDEX_NONE)
			{
				break;
			}

			if (bClosing)
			{
				Depth = FMath::Max(0, Depth - 1);
			}
			else
			{
				const bool bSelfClosed = End > NameEnd && Rml[End - 1] == TEXT('/');
				if (!bSelfClosed && !MantleRmlIsVoidElement(Rml.Mid(NameStart, NameEnd - NameStart)))
				{
					++Depth;
					Deepest = FMath::Max(Deepest, Depth);
				}
			}
			Index = End + 1;
		}
		return Deepest;
	}

	FString ApplyDefaultDataModel(const FString& Rml, FString& OutOtherModel)
	{
		OutOtherModel.Reset();
		const int32 Length = Rml.Len();

		// The first "<body" followed by whitespace, '>' or '/'.
		int32 AfterName = INDEX_NONE;
		int32 From = 0;
		while (true)
		{
			const int32 Found = Rml.Find(TEXT("<body"), ESearchCase::IgnoreCase, ESearchDir::FromStart, From);
			if (Found == INDEX_NONE)
			{
				return Rml;
			}
			const int32 After = Found + 5;
			if (After >= Length)
			{
				return Rml;
			}
			const TCHAR Next = Rml[After];
			if (FChar::IsWhitespace(Next) || Next == TEXT('>') || Next == TEXT('/'))
			{
				AfterName = After;
				break;
			}
			From = After;
		}

		// Walk the tag's attributes up to its '>'.
		int32 Index = AfterName;
		while (Index < Length)
		{
			while (Index < Length && FChar::IsWhitespace(Rml[Index]))
			{
				++Index;
			}
			if (Index >= Length || Rml[Index] == TEXT('>') || Rml[Index] == TEXT('/'))
			{
				break;
			}
			const int32 NameStart = Index;
			while (Index < Length && !FChar::IsWhitespace(Rml[Index]) && Rml[Index] != TEXT('=') && Rml[Index] != TEXT('>') &&
				Rml[Index] != TEXT('/'))
			{
				++Index;
			}
			const FString Name = Rml.Mid(NameStart, Index - NameStart);
			while (Index < Length && FChar::IsWhitespace(Rml[Index]))
			{
				++Index;
			}
			FString Value;
			if (Index < Length && Rml[Index] == TEXT('='))
			{
				++Index;
				while (Index < Length && FChar::IsWhitespace(Rml[Index]))
				{
					++Index;
				}
				if (Index < Length && (Rml[Index] == TEXT('"') || Rml[Index] == TEXT('\'')))
				{
					const TCHAR Quote = Rml[Index];
					const int32 ValueStart = Index + 1;
					int32 ValueEnd = ValueStart;
					while (ValueEnd < Length && Rml[ValueEnd] != Quote)
					{
						++ValueEnd;
					}
					Value = Rml.Mid(ValueStart, ValueEnd - ValueStart);
					Index = FMath::Min(Length, ValueEnd + 1);
				}
				else
				{
					const int32 ValueStart = Index;
					while (Index < Length && !FChar::IsWhitespace(Rml[Index]) && Rml[Index] != TEXT('>'))
					{
						++Index;
					}
					Value = Rml.Mid(ValueStart, Index - ValueStart);
				}
			}
			if (Name.Equals(TEXT("data-model"), ESearchCase::IgnoreCase))
			{
				if (!Value.Equals(DataModelName, ESearchCase::CaseSensitive))
				{
					OutOtherModel = Value.IsEmpty() ? FString(TEXT("(empty)")) : Value;
				}
				return Rml;
			}
			if (Name.IsEmpty())
			{
				// A stray character (a lone quote or '='): step over it.
				++Index;
			}
		}
		return Rml.Left(AfterName) + FString::Printf(TEXT(" data-model=\"%s\""), DataModelName) + Rml.Mid(AfterName);
	}

	bool IsLegalVariableName(const FString& Name, FString& OutError)
	{
		if (Name.IsEmpty())
		{
			OutError = TEXT("a data name cannot be empty");
			return false;
		}
		if (Name.Len() > MantleRmlMaxVariableNameLength)
		{
			OutError = FString::Printf(TEXT("data name '%s' is longer than %d characters"), *Name.Left(MantleRmlMaxVariableNameLength), MantleRmlMaxVariableNameLength);
			return false;
		}
		if (!MantleRmlIsAsciiLetter(Name[0]))
		{
			OutError = FString::Printf(TEXT("data name '%s' must start with a letter"), *Name);
			return false;
		}
		for (const TCHAR Char : Name)
		{
			if (!MantleRmlIsIdentifierChar(Char))
			{
				OutError = FString::Printf(TEXT("data name '%s' may only hold letters, digits and '_'"), *Name);
				return false;
			}
		}
		for (const TCHAR* Reserved : MantleRmlReservedVariableNames)
		{
			if (Name.Equals(Reserved, ESearchCase::IgnoreCase))
			{
				OutError = FString::Printf(TEXT("data name '%s' is reserved by RmlUi"), *Name);
				return false;
			}
		}
		return true;
	}

	const char* BaseStyleSheet()
	{
		// Block layout for the structural tags (RmlUi has no user-agent sheet, so everything is
		// inline otherwise), a heading scale, Mantle's palette for controls (surface #272726, edge
		// #444443, text #E8E6E2, hover #33251F, focus ring #EB6036), and `tab-index: auto` plus
		// `nav: auto` on controls so keyboard and gamepad navigation work without any RCSS. The
		// body fills the screen: RmlUi sizes a document to its content otherwise, so a centred or
		// right-anchored absolute panel resolves against a 0x0 body in the top-left corner.
		return
			"body { display: block; width: 100%; height: 100%; font-family: Roboto; font-size: 16dp; color: #E8E6E2; }\n"
			"div, p, h1, h2, h3, section, header, footer, ul, li, form { display: block; }\n"
			"h1 { font-size: 32dp; font-weight: bold; margin-bottom: 12dp; }\n"
			"h2 { font-size: 24dp; font-weight: bold; margin-bottom: 10dp; }\n"
			"h3 { font-size: 19dp; font-weight: bold; margin-bottom: 8dp; }\n"
			"p { margin-bottom: 8dp; }\n"
			"button { display: inline-block; padding: 8dp 16dp; border-width: 1dp; border-color: #444443; border-radius: 6dp;"
			" background-color: #272726; color: #E8E6E2; text-align: center; cursor: pointer; tab-index: auto; nav: auto; }\n"
			"button:hover { background-color: #33251F; }\n"
			"button:focus-visible { border-color: #EB6036; }\n"
			"input, textarea, select { display: inline-block; padding: 6dp 8dp; border-width: 1dp; border-color: #444443;"
			" border-radius: 4dp; background-color: #1E1E1D; color: #E8E6E2; tab-index: auto; nav: auto; }\n"
			"input:focus-visible, textarea:focus-visible, select:focus-visible { border-color: #EB6036; }\n";
	}
}
