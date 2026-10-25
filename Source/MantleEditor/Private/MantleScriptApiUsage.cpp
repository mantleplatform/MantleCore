// Copyright Joshua Eger. All Rights Reserved.
// The lowest script API minor a script needs. See MantleScriptApiUsage.h.
#include "MantleScriptApiUsage.h"

#include "Dom/JsonObject.h"
#include "Graph/MantleGraphTypes.h"
#include "MantleApiManifest.h"
#include "MantleScriptNaming.h"

namespace
{
	enum class EApiUsageLexer : uint8
	{
		Luau,
		JavaScript,
		Python
	};

	enum class EApiUsageTokenKind : uint8
	{
		Ident,
		/** A string literal with no interpolation; Text is its content. */
		String,
		/** An interpolated string (Luau `...{x}`, a JS template with ${x}, a Python f-string with {x}).
		 *  Its expressions are lexed as ordinary tokens right after it. */
		Interpolated,
		Punct
	};

	struct FApiUsageToken
	{
		EApiUsageTokenKind Kind = EApiUsageTokenKind::Punct;
		FString Text;
	};

	/** Deep enough for any real nesting of interpolations; stops a pathological file recursing. */
	constexpr int32 MaxApiUsageLexDepth = 16;

	void LexApiUsage(const FString& Src, EApiUsageLexer Lang, TArray<FApiUsageToken>& Out, int32 Depth);

	bool IsApiUsageIdentStart(TCHAR C, EApiUsageLexer Lang)
	{
		return FChar::IsAlpha(C) || C == TEXT('_') || (Lang == EApiUsageLexer::JavaScript && C == TEXT('$'));
	}

	bool IsApiUsageIdentChar(TCHAR C, EApiUsageLexer Lang)
	{
		return IsApiUsageIdentStart(C, Lang) || FChar::IsDigit(C);
	}

	void AddApiUsageToken(TArray<FApiUsageToken>& Out, EApiUsageTokenKind Kind, const FString& Text)
	{
		FApiUsageToken& Token = Out.AddDefaulted_GetRef();
		Token.Kind = Kind;
		Token.Text = Text;
	}

	/** At Src[Pos] == '[': a Luau long bracket "[==[" opens here. OutLevel is the '=' count, OutAfter the index past it. */
	bool OpensLuauLongBracket(const FString& Src, int32 Pos, int32& OutLevel, int32& OutAfter)
	{
		if (Pos >= Src.Len() || Src[Pos] != TEXT('['))
		{
			return false;
		}
		int32 i = Pos + 1;
		int32 Level = 0;
		while (i < Src.Len() && Src[i] == TEXT('='))
		{
			++Level;
			++i;
		}
		if (i < Src.Len() && Src[i] == TEXT('['))
		{
			OutLevel = Level;
			OutAfter = i + 1;
			return true;
		}
		return false;
	}

	/** From Start (just inside a long bracket), the index past its "]==]" close; the end of Src when unterminated. */
	int32 SkipLuauLongBracket(const FString& Src, int32 Start, int32 Level, FString& OutContent)
	{
		FString Close = TEXT("]");
		for (int32 i = 0; i < Level; ++i)
		{
			Close.AppendChar(TEXT('='));
		}
		Close.AppendChar(TEXT(']'));
		const int32 End = Src.Find(Close, ESearchCase::CaseSensitive, ESearchDir::FromStart, Start);
		if (End == INDEX_NONE)
		{
			OutContent = Src.Mid(Start);
			return Src.Len();
		}
		OutContent = Src.Mid(Start, End - Start);
		return End + Close.Len();
	}

	/** From Start (inside a quote), the index past the closing Quote; stops at a newline unless bMultiline. */
	int32 SkipQuoted(const FString& Src, int32 Start, TCHAR Quote, bool bMultiline, FString& OutContent)
	{
		int32 i = Start;
		while (i < Src.Len())
		{
			const TCHAR C = Src[i];
			if (C == TEXT('\\'))
			{
				i += 2;
				continue;
			}
			if (C == Quote)
			{
				OutContent = Src.Mid(Start, i - Start);
				return i + 1;
			}
			if (C == TEXT('\n') && !bMultiline)
			{
				break;
			}
			++i;
		}
		OutContent = Src.Mid(Start, FMath::Min(i, Src.Len()) - Start);
		return FMath::Min(i, Src.Len());
	}

	/** From Start (just past an opening '{'), the index of its matching '}', skipping quoted text; Src.Len() when unmatched. */
	int32 FindClosingBrace(const FString& Src, int32 Start)
	{
		int32 Depth = 0;
		int32 i = Start;
		while (i < Src.Len())
		{
			const TCHAR C = Src[i];
			if (C == TEXT('"') || C == TEXT('\'') || C == TEXT('`'))
			{
				FString Ignored;
				i = SkipQuoted(Src, i + 1, C, C == TEXT('`'), Ignored);
				continue;
			}
			if (C == TEXT('{'))
			{
				++Depth;
			}
			else if (C == TEXT('}'))
			{
				if (Depth == 0)
				{
					return i;
				}
				--Depth;
			}
			++i;
		}
		return Src.Len();
	}

	/**
	 * An interpolated string from Start (just past its opening delimiter) to Close. bJsTemplate
	 * selects the JS "${" opener (Luau/Python use "{"); bDoubledBraceIsLiteral is Python's "{{". Adds the
	 * String or Interpolated token, then any expression tokens, and returns the index past Close.
	 */
	int32 LexInterpolated(const FString& Src, int32 Start, const FString& Close, bool bJsTemplate, bool bDoubledBraceIsLiteral,
		EApiUsageLexer Lang, TArray<FApiUsageToken>& Out, int32 Depth)
	{
		FString Literal;
		TArray<FApiUsageToken> Inner;
		bool bInterpolated = false;
		int32 i = Start;
		while (i < Src.Len())
		{
			const TCHAR C = Src[i];
			if (C == TEXT('\\'))
			{
				Literal.AppendChar(C);
				if (i + 1 < Src.Len())
				{
					Literal.AppendChar(Src[i + 1]);
				}
				i += 2;
				continue;
			}
			if (Src.Mid(i, Close.Len()).Equals(Close, ESearchCase::CaseSensitive))
			{
				i += Close.Len();
				break;
			}
			const bool bOpens = bJsTemplate
				? (C == TEXT('$') && i + 1 < Src.Len() && Src[i + 1] == TEXT('{'))
				: C == TEXT('{');
			if (bOpens)
			{
				if (bDoubledBraceIsLiteral && i + 1 < Src.Len() && Src[i + 1] == TEXT('{'))
				{
					Literal.AppendChar(C);
					i += 2;
					continue;
				}
				const int32 ExprStart = i + (bJsTemplate ? 2 : 1);
				const int32 ExprEnd = FindClosingBrace(Src, ExprStart);
				if (Depth < MaxApiUsageLexDepth)
				{
					LexApiUsage(Src.Mid(ExprStart, ExprEnd - ExprStart), Lang, Inner, Depth + 1);
				}
				bInterpolated = true;
				i = FMath::Min(ExprEnd + 1, Src.Len());
				continue;
			}
			Literal.AppendChar(C);
			++i;
		}
		AddApiUsageToken(Out, bInterpolated ? EApiUsageTokenKind::Interpolated : EApiUsageTokenKind::String, Literal);
		Out.Append(Inner);
		return FMath::Min(i, Src.Len());
	}

	/** JS: whether a '/' here starts a regular expression rather than dividing. */
	bool JsSlashStartsRegex(const TArray<FApiUsageToken>& Out)
	{
		if (Out.IsEmpty())
		{
			return true;
		}
		const FApiUsageToken& Prev = Out.Last();
		if (Prev.Kind == EApiUsageTokenKind::Punct)
		{
			return !(Prev.Text == TEXT(")") || Prev.Text == TEXT("]") || Prev.Text == TEXT("(regex)") || Prev.Text == TEXT("(number)"));
		}
		if (Prev.Kind == EApiUsageTokenKind::Ident)
		{
			static const TCHAR* Keywords[] = {
				TEXT("return"), TEXT("typeof"), TEXT("instanceof"), TEXT("in"), TEXT("of"), TEXT("new"), TEXT("delete"),
				TEXT("void"), TEXT("throw"), TEXT("case"), TEXT("do"), TEXT("else"), TEXT("yield"), TEXT("await") };
			for (const TCHAR* Keyword : Keywords)
			{
				if (Prev.Text.Equals(Keyword, ESearchCase::CaseSensitive))
				{
					return true;
				}
			}
		}
		return false;
	}

	void LexApiUsage(const FString& Src, EApiUsageLexer Lang, TArray<FApiUsageToken>& Out, int32 Depth)
	{
		const int32 N = Src.Len();
		int32 i = 0;
		while (i < N)
		{
			const TCHAR C = Src[i];
			const TCHAR Next = i + 1 < N ? Src[i + 1] : TEXT('\0');

			if (FChar::IsWhitespace(C))
			{
				++i;
				continue;
			}

			// Comments.
			if (Lang == EApiUsageLexer::Luau && C == TEXT('-') && Next == TEXT('-'))
			{
				int32 Level = 0;
				int32 After = 0;
				if (OpensLuauLongBracket(Src, i + 2, Level, After))
				{
					FString Ignored;
					i = SkipLuauLongBracket(Src, After, Level, Ignored);
				}
				else
				{
					while (i < N && Src[i] != TEXT('\n')) ++i;
				}
				continue;
			}
			if (Lang == EApiUsageLexer::JavaScript && C == TEXT('/') && Next == TEXT('/'))
			{
				while (i < N && Src[i] != TEXT('\n')) ++i;
				continue;
			}
			if (Lang == EApiUsageLexer::JavaScript && C == TEXT('/') && Next == TEXT('*'))
			{
				const int32 End = Src.Find(TEXT("*/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, i + 2);
				i = End == INDEX_NONE ? N : End + 2;
				continue;
			}
			if (Lang == EApiUsageLexer::Python && C == TEXT('#'))
			{
				while (i < N && Src[i] != TEXT('\n')) ++i;
				continue;
			}

			// Strings.
			if (C == TEXT('"') || C == TEXT('\''))
			{
				if (Lang == EApiUsageLexer::Python && i + 2 < N && Src[i + 1] == C && Src[i + 2] == C)
				{
					const FString Triple = FString::ChrN(3, C);
					const int32 End = Src.Find(Triple, ESearchCase::CaseSensitive, ESearchDir::FromStart, i + 3);
					const int32 Stop = End == INDEX_NONE ? N : End;
					AddApiUsageToken(Out, EApiUsageTokenKind::String, Src.Mid(i + 3, Stop - (i + 3)));
					i = End == INDEX_NONE ? N : End + 3;
					continue;
				}
				FString Content;
				i = SkipQuoted(Src, i + 1, C, /*bMultiline*/ false, Content);
				AddApiUsageToken(Out, EApiUsageTokenKind::String, Content);
				continue;
			}
			if (Lang == EApiUsageLexer::Luau && C == TEXT('['))
			{
				int32 Level = 0;
				int32 After = 0;
				if (OpensLuauLongBracket(Src, i, Level, After))
				{
					FString Content;
					i = SkipLuauLongBracket(Src, After, Level, Content);
					AddApiUsageToken(Out, EApiUsageTokenKind::String, Content);
					continue;
				}
			}
			if (C == TEXT('`') && Lang != EApiUsageLexer::Python)
			{
				i = LexInterpolated(Src, i + 1, TEXT("`"), Lang == EApiUsageLexer::JavaScript, /*bDoubledBraceIsLiteral*/ false, Lang, Out, Depth);
				continue;
			}
			if (Lang == EApiUsageLexer::JavaScript && C == TEXT('/') && JsSlashStartsRegex(Out))
			{
				int32 j = i + 1;
				bool bInClass = false;
				bool bClosed = false;
				while (j < N && Src[j] != TEXT('\n'))
				{
					const TCHAR R = Src[j];
					if (R == TEXT('\\')) { j += 2; continue; }
					if (R == TEXT('[')) bInClass = true;
					else if (R == TEXT(']')) bInClass = false;
					else if (R == TEXT('/') && !bInClass) { bClosed = true; break; }
					++j;
				}
				if (bClosed)
				{
					i = j + 1;
					while (i < N && IsApiUsageIdentChar(Src[i], Lang)) ++i;
					AddApiUsageToken(Out, EApiUsageTokenKind::Punct, TEXT("(regex)"));
					continue;
				}
			}

			// Identifiers, and Python's string prefixes (f"", rb'', ...).
			if (IsApiUsageIdentStart(C, Lang))
			{
				int32 j = i + 1;
				while (j < N && IsApiUsageIdentChar(Src[j], Lang)) ++j;
				const FString Word = Src.Mid(i, j - i);
				if (Lang == EApiUsageLexer::Python && Word.Len() <= 2 && j < N && (Src[j] == TEXT('"') || Src[j] == TEXT('\'')))
				{
					bool bPrefix = true;
					bool bFormatted = false;
					for (int32 p = 0; p < Word.Len(); ++p)
					{
						const TCHAR Lower = FChar::ToLower(Word[p]);
						if (!(Lower == TEXT('r') || Lower == TEXT('b') || Lower == TEXT('u') || Lower == TEXT('f')))
						{
							bPrefix = false;
						}
						if (Lower == TEXT('f'))
						{
							bFormatted = true;
						}
					}
					if (bPrefix)
					{
						const TCHAR Quote = Src[j];
						const bool bTriple = j + 2 < N && Src[j + 1] == Quote && Src[j + 2] == Quote;
						const FString Close = bTriple ? FString::ChrN(3, Quote) : FString::ChrN(1, Quote);
						const int32 Start = j + Close.Len();
						if (bFormatted)
						{
							i = LexInterpolated(Src, Start, Close, /*bJsTemplate*/ false, /*bDoubledBraceIsLiteral*/ true, Lang, Out, Depth);
						}
						else if (bTriple)
						{
							const int32 End = Src.Find(Close, ESearchCase::CaseSensitive, ESearchDir::FromStart, Start);
							const int32 Stop = End == INDEX_NONE ? N : End;
							AddApiUsageToken(Out, EApiUsageTokenKind::String, Src.Mid(Start, Stop - Start));
							i = End == INDEX_NONE ? N : End + 3;
						}
						else
						{
							FString Content;
							i = SkipQuoted(Src, Start, Quote, /*bMultiline*/ false, Content);
							AddApiUsageToken(Out, EApiUsageTokenKind::String, Content);
						}
						continue;
					}
				}
				AddApiUsageToken(Out, EApiUsageTokenKind::Ident, Word);
				i = j;
				continue;
			}

			// Numbers (never an API name; consumed so "1.5" isn't read as a member access).
			if (FChar::IsDigit(C) || (C == TEXT('.') && FChar::IsDigit(Next)))
			{
				++i;
				while (i < N && (FChar::IsAlnum(Src[i]) || Src[i] == TEXT('_') || (Src[i] == TEXT('.') && i + 1 < N && FChar::IsDigit(Src[i + 1]))))
				{
					++i;
				}
				AddApiUsageToken(Out, EApiUsageTokenKind::Punct, TEXT("(number)"));
				continue;
			}

			// Punctuation. Luau's ".." (concatenate) and "..." must not read as a member access.
			if (C == TEXT('.') && Next == TEXT('.'))
			{
				const bool bThree = i + 2 < N && Src[i + 2] == TEXT('.');
				AddApiUsageToken(Out, EApiUsageTokenKind::Punct, bThree ? TEXT("...") : TEXT(".."));
				i += bThree ? 3 : 2;
				continue;
			}
			if (Lang == EApiUsageLexer::Luau && C == TEXT(':') && Next == TEXT(':'))
			{
				AddApiUsageToken(Out, EApiUsageTokenKind::Punct, TEXT("::"));
				i += 2;
				continue;
			}
			AddApiUsageToken(Out, EApiUsageTokenKind::Punct, FString::ChrN(1, C));
			++i;
		}
	}

	/** One API name introduced after the base: a global (a newer class) or a newer member. */
	struct FApiUsageTarget
	{
		FString Label;
		int32 Minor = 0;
		/** Every spelling that counts: the canonical name and the language's own. */
		TArray<FString> Names;
		/** Members count only after "." or ":" (or as an exact string literal); globals count anywhere. */
		bool bMember = false;
		/** When set, the member counts only in a script that also names one of these globals (the
		    services that hand out its class). See BuildApiUsageTargets. */
		TArray<FString> RequiresGlobal;

		bool Matches(const FString& Text) const
		{
			return Names.ContainsByPredicate([&Text](const FString& Name) { return Name.Equals(Text, ESearchCase::CaseSensitive); });
		}
	};

	FString LanguageSpelling(const FString& Canonical, EApiUsageLexer Lang)
	{
		switch (Lang)
		{
		case EApiUsageLexer::JavaScript: return FMantleNaming::ToCamel(Canonical);
		case EApiUsageLexer::Python:     return FMantleNaming::ToSnake(Canonical);
		case EApiUsageLexer::Luau:
		default:                         return Canonical;
		}
	}

	FApiUsageTarget& AddMemberTarget(TArray<FApiUsageTarget>& Out, const FMantleApiManifestClass& Class, const FString& Member, int32 Minor, EApiUsageLexer Lang)
	{
		FApiUsageTarget& Target = Out.AddDefaulted_GetRef();
		Target.Label = Class.Name + TEXT(".") + Member;
		Target.Minor = Minor;
		Target.bMember = true;
		Target.Names.Add(Member);
		const FString Spelled = LanguageSpelling(Member, Lang);
		if (!Spelled.Equals(Member, ESearchCase::CaseSensitive))
		{
			Target.Names.Add(Spelled);
		}
		return Target;
	}

	/** The services whose methods or properties hand out a value of ClassName (NPCs for NPC). */
	TArray<FString> GatewayServicesOf(const FString& ClassName)
	{
		auto NamesClass = [&ClassName](const FString& Type)
		{
			FString Bare = Type;
			Bare.RemoveFromEnd(TEXT("?"));
			return Bare.Equals(ClassName, ESearchCase::CaseSensitive);
		};
		TArray<FString> Services;
		for (const FMantleApiManifestClass& Class : FMantleApiManifest::Get().Classes)
		{
			if (!Class.bService)
			{
				continue;
			}
			const bool bHandsOut = Class.Methods.ContainsByPredicate([&NamesClass](const FMantleApiManifestMethod& M) { return NamesClass(M.Returns); })
				|| Class.Properties.ContainsByPredicate([&NamesClass](const FMantleApiManifestProperty& P) { return NamesClass(P.Type); });
			if (bHandsOut)
			{
				Services.Add(Class.Name);
			}
		}
		return Services;
	}

	TArray<FApiUsageTarget> BuildApiUsageTargets(EApiUsageLexer Lang)
	{
		// The oldest minor each member name is reachable at, across every class. A newer plain
		// class (NPC, 1.17) shares names with the base surface (Position, Destroy, Exists): the scan
		// can't tell which class a value is, so such a name proves nothing newer than its oldest owner.
		TMap<FString, int32> OldestMinorByMember;
		for (const FMantleApiManifestClass& Class : FMantleApiManifest::Get().Classes)
		{
			auto Note = [&OldestMinorByMember, &Class](const FString& Member, int32 MemberMinor)
			{
				const int32 Effective = FMath::Max(MemberMinor, Class.IntroducedMinor);
				int32& Oldest = OldestMinorByMember.FindOrAdd(Member, Effective);
				Oldest = FMath::Min(Oldest, Effective);
			};
			for (const FMantleApiManifestMethod& M : Class.Methods) Note(M.Name, M.IntroducedMinor);
			for (const FMantleApiManifestProperty& P : Class.Properties) Note(P.Name, P.IntroducedMinor);
			for (const FMantleApiManifestSignal& S : Class.Signals) Note(S.Name, S.IntroducedMinor);
		}

		TArray<FApiUsageTarget> Targets;
		for (const FMantleApiManifestClass& Class : FMantleApiManifest::Get().Classes)
		{
			if (Class.IntroducedMinor > 0)
			{
				// Services and classes are globals under their canonical name in every language.
				FApiUsageTarget& Global = Targets.AddDefaulted_GetRef();
				Global.Label = Class.Name;
				Global.Minor = Class.IntroducedMinor;
				Global.Names.Add(Class.Name);
			}

			// A newer service's members are only reachable through the service itself; a newer
			// plain class's (or an older class's newer) members are reachable from any value.
			auto Consider = [&](const FString& Member, int32 MemberMinor)
			{
				const int32 Effective = FMath::Max(MemberMinor, Class.IntroducedMinor);
				if (MemberMinor > Class.IntroducedMinor)
				{
					FApiUsageTarget& Target = AddMemberTarget(Targets, Class, Member, Effective, Lang);
					// A newer member of a newer plain class whose name an older class also has
					// (NPC.Health, 1.23, and Player.Health, 1.9). Counting the name alone would mark every
					// player-health script as needing 1.23; skipping it would let an NPC-health script
					// claim 1.17. So it counts in a script that also names a service handing the class
					// out (NPCs). With none found it counts anywhere: the safe side.
					const int32* Oldest = OldestMinorByMember.Find(Member);
					if (Class.IntroducedMinor > 0 && !Class.bService && Oldest && *Oldest < Effective)
					{
						Target.RequiresGlobal = GatewayServicesOf(Class.Name);
					}
				}
				else if (Class.IntroducedMinor > 0 && !Class.bService)
				{
					// Only a name no older class has: reaching the class needs its service (NPCs), a
					// global target of its own, so skipping a shared name loses nothing a place needs.
					const int32* Oldest = OldestMinorByMember.Find(Member);
					if (!Oldest || *Oldest >= Effective)
					{
						AddMemberTarget(Targets, Class, Member, Effective, Lang);
					}
				}
			};
			for (const FMantleApiManifestMethod& M : Class.Methods) Consider(M.Name, M.IntroducedMinor);
			for (const FMantleApiManifestProperty& P : Class.Properties) Consider(P.Name, P.IntroducedMinor);
			for (const FMantleApiManifestSignal& S : Class.Signals) Consider(S.Name, S.IntroducedMinor);
		}
		return Targets;
	}

	bool IsApiUsagePunct(const TArray<FApiUsageToken>& Tokens, int32 Index, const TCHAR* Text)
	{
		return Tokens.IsValidIndex(Index) && Tokens[Index].Kind == EApiUsageTokenKind::Punct &&
			Tokens[Index].Text.Equals(Text, ESearchCase::CaseSensitive);
	}

	bool EqualsAnyOf(const FString& Text, std::initializer_list<const TCHAR*> Options)
	{
		for (const TCHAR* Option : Options)
		{
			if (Text.Equals(Option, ESearchCase::CaseSensitive))
			{
				return true;
			}
		}
		return false;
	}

	/** Python getattr/setattr/hasattr/delattr at Index: true when its name argument isn't a plain string literal. */
	bool PythonAttrNameIsComputed(const TArray<FApiUsageToken>& Tokens, int32 Index)
	{
		if (!IsApiUsagePunct(Tokens, Index + 1, TEXT("(")))
		{
			return false; // Referenced, not called (passed around): see the caller.
		}
		int32 Depth = 0;
		for (int32 k = Index + 2; k < Tokens.Num(); ++k)
		{
			const FApiUsageToken& T = Tokens[k];
			if (T.Kind != EApiUsageTokenKind::Punct)
			{
				continue;
			}
			if (EqualsAnyOf(T.Text, { TEXT("("), TEXT("["), TEXT("{") }))
			{
				++Depth;
			}
			else if (EqualsAnyOf(T.Text, { TEXT(")"), TEXT("]"), TEXT("}") }))
			{
				if (Depth == 0)
				{
					return true; // No name argument at all: not something a scan can read.
				}
				--Depth;
			}
			else if (T.Text == TEXT(",") && Depth == 0)
			{
				const bool bLiteral = Tokens.IsValidIndex(k + 1) && Tokens[k + 1].Kind == EApiUsageTokenKind::String &&
					(IsApiUsagePunct(Tokens, k + 2, TEXT(")")) || IsApiUsagePunct(Tokens, k + 2, TEXT(",")));
				return !bLiteral;
			}
		}
		return true;
	}

	/** At an index '[' : whether the key is built from strings (concatenated, formatted or interpolated). */
	bool IndexKeyIsBuiltFromStrings(const TArray<FApiUsageToken>& Tokens, int32 Open, EApiUsageLexer Lang)
	{
		bool bString = false;
		bool bBuilds = false;
		int32 Depth = 0;
		for (int32 k = Open + 1; k < Tokens.Num(); ++k)
		{
			const FApiUsageToken& T = Tokens[k];
			if (T.Kind == EApiUsageTokenKind::Interpolated)
			{
				return true;
			}
			if (T.Kind == EApiUsageTokenKind::String)
			{
				bString = true;
				continue;
			}
			if (T.Kind != EApiUsageTokenKind::Punct)
			{
				continue;
			}
			if (T.Text == TEXT("["))
			{
				++Depth;
			}
			else if (T.Text == TEXT("]"))
			{
				if (Depth == 0)
				{
					break;
				}
				--Depth;
			}
			else if ((Lang == EApiUsageLexer::Luau && T.Text == TEXT("..")) ||
				(Lang != EApiUsageLexer::Luau && T.Text == TEXT("+")) ||
				(Lang == EApiUsageLexer::Python && T.Text == TEXT("%")))
			{
				bBuilds = true;
			}
		}
		return bString && bBuilds;
	}

	/**
	 * The construct at Index that reaches the API by a name the scan can't see, or empty. Each is
	 * a way a script gets at a global or member without writing its name: the environment table
	 * (getfenv, _G, globalThis, globals()), code built at run time (loadstring, eval, exec), or
	 * reflection with a computed name.
	 */
	FString UnanalysableAt(const TArray<FApiUsageToken>& Tokens, int32 Index, EApiUsageLexer Lang, bool bAfterAccess)
	{
		const FApiUsageToken& T = Tokens[Index];
		if (T.Kind == EApiUsageTokenKind::Punct && T.Text == TEXT("[") && Index > 0)
		{
			const FApiUsageToken& Prev = Tokens[Index - 1];
			// An index follows a value; after a keyword ("return [a, b]") the bracket is a list.
			const bool bKeyword = EqualsAnyOf(Prev.Text, { TEXT("return"), TEXT("in"), TEXT("of"), TEXT("yield"), TEXT("await"),
				TEXT("case"), TEXT("else"), TEXT("then"), TEXT("do"), TEXT("and"), TEXT("or"), TEXT("not"), TEXT("if"),
				TEXT("elif"), TEXT("while"), TEXT("for"), TEXT("typeof"), TEXT("new") });
			const bool bIndexes = (Prev.Kind == EApiUsageTokenKind::Ident && !bKeyword) ||
				(Prev.Kind == EApiUsageTokenKind::Punct && (Prev.Text == TEXT(")") || Prev.Text == TEXT("]")));
			if (bIndexes && IndexKeyIsBuiltFromStrings(Tokens, Index, Lang))
			{
				return TEXT("a member looked up by a name built from strings (x[\"...\" .. y])");
			}
			return FString();
		}
		if (T.Kind != EApiUsageTokenKind::Ident)
		{
			return FString();
		}

		const bool bCalled = IsApiUsagePunct(Tokens, Index + 1, TEXT("("));
		switch (Lang)
		{
		case EApiUsageLexer::Luau:
			if (!bAfterAccess && EqualsAnyOf(T.Text, { TEXT("getfenv"), TEXT("setfenv"), TEXT("_G"), TEXT("_ENV"), TEXT("loadstring") }))
			{
				return T.Text;
			}
			break;
		case EApiUsageLexer::JavaScript:
			if (!bAfterAccess && EqualsAnyOf(T.Text, { TEXT("globalThis"), TEXT("Reflect") }))
			{
				return T.Text;
			}
			if (!bAfterAccess && bCalled && EqualsAnyOf(T.Text, { TEXT("eval"), TEXT("Function") }))
			{
				return T.Text;
			}
			break;
		case EApiUsageLexer::Python:
			if (EqualsAnyOf(T.Text, { TEXT("__dict__"), TEXT("__getattribute__") }))
			{
				return T.Text;
			}
			if (!bAfterAccess && EqualsAnyOf(T.Text, { TEXT("eval"), TEXT("exec"), TEXT("globals"), TEXT("vars"),
				TEXT("__import__"), TEXT("__builtins__"), TEXT("builtins"), TEXT("importlib") }))
			{
				return T.Text;
			}
			if (!bAfterAccess && EqualsAnyOf(T.Text, { TEXT("getattr"), TEXT("setattr"), TEXT("hasattr"), TEXT("delattr") }) &&
				(!bCalled || PythonAttrNameIsComputed(Tokens, Index)))
			{
				return FString::Printf(TEXT("%s with a computed name"), *T.Text);
			}
			break;
		}
		return FString();
	}

	void RaiseApiNeed(FMantleScriptApiNeed& Need, int32 Minor, const FString& For)
	{
		if (Minor > Need.Minor)
		{
			Need.Minor = Minor;
			Need.NeededFor = For;
		}
	}

	FMantleScriptApiNeed MakeUnanalysable(const FString& Construct)
	{
		FMantleScriptApiNeed Need;
		Need.bAnalysed = false;
		Need.Unanalysable = Construct;
		Need.Minor = FMantleApiManifest::Get().Minor;
		return Need;
	}
}

void FMantleScriptApiNeed::Merge(const FMantleScriptApiNeed& Other)
{
	if (Other.Minor > Minor)
	{
		Minor = Other.Minor;
		NeededFor = Other.NeededFor;
	}
	if (!Other.bAnalysed && bAnalysed)
	{
		bAnalysed = false;
		Unanalysable = Other.Unanalysable;
	}
}

FMantleScriptApiNeed FMantleScriptApiUsage::ForSource(const FString& Source, EMantleScriptCatalogLanguage Language)
{
	EApiUsageLexer Lang = EApiUsageLexer::Luau;
	switch (Language)
	{
	case EMantleScriptCatalogLanguage::Luau:       Lang = EApiUsageLexer::Luau; break;
	case EMantleScriptCatalogLanguage::JavaScript:
	case EMantleScriptCatalogLanguage::TypeScript: Lang = EApiUsageLexer::JavaScript; break;
	case EMantleScriptCatalogLanguage::Python:     Lang = EApiUsageLexer::Python; break;
	default:
		return MakeUnanalysable(TEXT("a language Mantle can't scan"));
	}

	TArray<FApiUsageToken> Tokens;
	LexApiUsage(Source, Lang, Tokens, 0);
	const TArray<FApiUsageTarget> Targets = BuildApiUsageTargets(Lang);

	// The names this script uses as globals (never after "." or ":"), for targets that count only
	// alongside one (FApiUsageTarget::RequiresGlobal).
	// Case-exact throughout: FString's own == and hashing ignore case, and "npcs" is not NPCs.
	TArray<FString> NamedGlobals;
	for (const FApiUsageTarget& Target : Targets)
	{
		for (const FString& Name : Target.RequiresGlobal)
		{
			if (!NamedGlobals.ContainsByPredicate([&Name](const FString& Seen) { return Seen.Equals(Name, ESearchCase::CaseSensitive); }))
			{
				NamedGlobals.Add(Name);
			}
		}
	}
	auto AfterAccess = [&Tokens, Lang](int32 i)
	{
		return IsApiUsagePunct(Tokens, i - 1, TEXT(".")) ||
			(Lang == EApiUsageLexer::Luau && IsApiUsagePunct(Tokens, i - 1, TEXT(":")));
	};
	TArray<FString> UsedGlobals;
	for (int32 i = 0; i < Tokens.Num() && UsedGlobals.Num() < NamedGlobals.Num(); ++i)
	{
		const bool bAfterAccess = AfterAccess(i);
		const FString& Text = Tokens[i].Text;
		auto Same = [&Text](const FString& Name) { return Name.Equals(Text, ESearchCase::CaseSensitive); };
		if (Tokens[i].Kind == EApiUsageTokenKind::Ident && !bAfterAccess && NamedGlobals.ContainsByPredicate(Same)
			&& !UsedGlobals.ContainsByPredicate(Same))
		{
			UsedGlobals.Add(Text);
		}
	}
	auto GlobalsAllow = [&UsedGlobals](const FApiUsageTarget& Target)
	{
		return Target.RequiresGlobal.IsEmpty() || Target.RequiresGlobal.ContainsByPredicate([&UsedGlobals](const FString& Name)
		{
			return UsedGlobals.ContainsByPredicate([&Name](const FString& Used) { return Used.Equals(Name, ESearchCase::CaseSensitive); });
		});
	};

	FMantleScriptApiNeed Need;
	for (int32 i = 0; i < Tokens.Num(); ++i)
	{
		const FApiUsageToken& Token = Tokens[i];
		const bool bAfterAccess = AfterAccess(i);

		const FString Construct = UnanalysableAt(Tokens, i, Lang, bAfterAccess);
		if (!Construct.IsEmpty())
		{
			return MakeUnanalysable(Construct);
		}

		if (Token.Kind == EApiUsageTokenKind::Ident || Token.Kind == EApiUsageTokenKind::String)
		{
			// An exact string literal counts in any position: it is how a name is looked up dynamically.
			const bool bLiteral = Token.Kind == EApiUsageTokenKind::String;
			for (const FApiUsageTarget& Target : Targets)
			{
				if ((bLiteral || !Target.bMember || bAfterAccess) && Target.Matches(Token.Text) && GlobalsAllow(Target))
				{
					RaiseApiNeed(Need, Target.Minor, Target.Label);
				}
			}
		}
	}
	return Need;
}

FMantleScriptApiNeed FMantleScriptApiUsage::ForGraph(const FMantleGraph& Graph)
{
	FMantleScriptApiNeed Need;
	const FMantleApiManifest& Api = FMantleApiManifest::Get();
	for (const FMantleGraphNode& Node : Graph.Nodes)
	{
		FString Dotted;
		if (!Node.Fields.IsValid() || !Node.Fields->TryGetStringField(TEXT("member"), Dotted))
		{
			continue;
		}
		FString ClassName;
		FString Member;
		if (!Dotted.Split(TEXT("."), &ClassName, &Member))
		{
			continue;
		}
		const FMantleApiManifestClass* Class = Api.FindClass(ClassName);
		if (!Class)
		{
			continue; // The compiler refuses the graph; there is no version to record for it.
		}
		const int32 Minor = Class->MemberIntroducedMinor(Member);
		RaiseApiNeed(Need, Minor == INDEX_NONE ? Class->IntroducedMinor : Minor, Dotted);
	}
	return Need;
}

void FMantleScriptApiUsage::DescribeUnanalysable(const FString& RelativePath, const FMantleScriptApiNeed& Need, FString& OutProblem, FString& OutRemedy)
{
	OutProblem = FString::Printf(
		TEXT("Scripts/%s uses %s, so Mantle can't tell which script API members it reaches. The place records script API %d.%d, "
			"this editor's newest, and won't run for players whose Mantle is older."),
		*RelativePath, *Need.Unanalysable, FMantleApiManifest::Get().Major, FMantleApiManifest::Get().Minor);
	OutRemedy = TEXT("If the place should run on older Mantle versions, name Mantle members directly (player.Avatar, not a looked-up name). "
		"Otherwise there is nothing to fix.");
}
