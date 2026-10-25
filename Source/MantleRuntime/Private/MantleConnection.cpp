// Copyright Joshua Eger. All Rights Reserved.
#include "MantleConnection.h"
#include "MantlePlaceTypes.h"
#include "MantlePreviewLaunch.h"

#include "Containers/StringConv.h"
#include "Misc/ScopeLock.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

// Mantle's hosted API: what the shipped client talks to, and where the editor plugin publishes
// unless something overrides it. A name on Mantle's own zone (#810), so moving the API to another
// host or account is a DNS change, not a client release.
#define MANTLE_HOSTED_API_URL TEXT("https://api.mantleplatform.com")

const TCHAR* FMantleConnection::HostedBaseUrl = MANTLE_HOSTED_API_URL;

#if WITH_EDITOR
const TCHAR* FMantleConnection::DefaultBaseUrl = TEXT("http://localhost:8080");
#else
const TCHAR* FMantleConnection::DefaultBaseUrl = MANTLE_HOSTED_API_URL;
#endif

#undef MANTLE_HOSTED_API_URL

const TCHAR* FMantleConnection::DisplayNameHeader = TEXT("X-Mantle-Display-Name");

namespace
{
	FCriticalSection AuthTokenMutex;
	FString RuntimeAuthToken;
	FString RuntimeDisplayName;

	/** Trailing slashes make "/places" become "//places", which some routers treat as a different path. */
	FString Tidy(const FString& Url)
	{
		FString Trimmed = Url.TrimStartAndEnd();
		while (Trimmed.EndsWith(TEXT("/")))
		{
			Trimmed.LeftChopInline(1);
		}
		return Trimmed;
	}
}

FString FMantleConnection::ResolveApiBaseUrl(const FString& Fallback)
{
	FString FromCommandLine;
	if (FParse::Value(FCommandLine::Get(), TEXT("MantleApi="), FromCommandLine)
		&& !FromCommandLine.IsEmpty())
	{
		return Tidy(FromCommandLine);
	}

	const FString FromEnvironment =
		FPlatformMisc::GetEnvironmentVariable(TEXT("MANTLE_API_URL")).TrimStartAndEnd();
	if (!FromEnvironment.IsEmpty())
	{
		return Tidy(FromEnvironment);
	}

	if (!Fallback.TrimStartAndEnd().IsEmpty())
	{
		return Tidy(Fallback);
	}

	return Tidy(DefaultBaseUrl);
}

FString FMantleConnection::ResolveAuthToken()
{
	// Offline preview: there is no sign-in and nothing may send a credential, so no token exists.
	if (MantlePreviewLaunch::IsRequested())
	{
		return FString();
	}

#if !UE_BUILD_SHIPPING
	// Local-development override only. A shipped client that honoured this would send a
	// developer token to the hosted API, be rejected, and never show Epic sign-in at all.
	const FString DevelopmentOverride =
		FPlatformMisc::GetEnvironmentVariable(TEXT("MANTLE_AUTH_TOKEN")).TrimStartAndEnd();
	if (!DevelopmentOverride.IsEmpty())
	{
		return DevelopmentOverride;
	}
#endif

	FScopeLock Lock(&AuthTokenMutex);
	return RuntimeAuthToken;
}

bool FMantleConnection::IsAuthTokenFromEnvironment()
{
#if !UE_BUILD_SHIPPING
	return !FPlatformMisc::GetEnvironmentVariable(TEXT("MANTLE_AUTH_TOKEN")).TrimStartAndEnd().IsEmpty();
#else
	return false;
#endif
}

FMantleSessionRejectedEvent& FMantleConnection::OnSessionRejected()
{
	static FMantleSessionRejectedEvent Event;
	return Event;
}

void FMantleConnection::SetRuntimeAuthToken(const FString& Token)
{
	FScopeLock Lock(&AuthTokenMutex);
	RuntimeAuthToken = Token.TrimStartAndEnd();
}

void FMantleConnection::ClearRuntimeAuthToken()
{
	FScopeLock Lock(&AuthTokenMutex);
	RuntimeAuthToken.Reset();
}

void FMantleConnection::SetRuntimeDisplayName(const FString& DisplayName)
{
	FScopeLock Lock(&AuthTokenMutex);
	RuntimeDisplayName = DisplayName.TrimStartAndEnd();
}

FString FMantleConnection::ResolveDisplayNameHeader()
{
	// Offline preview: no request is sent, so no persona name leaves with one.
	if (MantlePreviewLaunch::IsRequested())
	{
		return FString();
	}
	FString DisplayName;
	{
		FScopeLock Lock(&AuthTokenMutex);
		DisplayName = RuntimeDisplayName;
	}
	return EncodeDisplayNameHeader(DisplayName);
}

FString FMantleConnection::EncodeDisplayNameHeader(const FString& DisplayName)
{
	static const ANSICHAR HexDigits[] = "0123456789ABCDEF";
	FString Encoded;
	Encoded.Reserve(FMath::Min(DisplayName.Len() * 3, MaxDisplayNameHeaderLength));

	// One code point at a time, so the cut below can only ever fall between two of them.
	// FString is UTF-16 on every platform the client ships on; a surrogate pair is a single
	// code point and is converted (and cut) as one.
	const int32 Length = DisplayName.Len();
	for (int32 Index = 0; Index < Length;)
	{
		int32 Units = 1;
		if (StringConv::IsHighSurrogate(DisplayName[Index]) && Index + 1 < Length &&
			StringConv::IsLowSurrogate(DisplayName[Index + 1]))
		{
			Units = 2;
		}
		const FString CodePoint = DisplayName.Mid(Index, Units);
		Index += Units;

		const FTCHARToUTF8 Utf8(*CodePoint, CodePoint.Len());
		FString Piece;
		for (int32 Byte = 0; Byte < Utf8.Length(); ++Byte)
		{
			const uint8 Value = static_cast<uint8>(Utf8.Get()[Byte]);
			// RFC 3986 unreserved: ALPHA / DIGIT / "-" / "." / "_" / "~".
			const bool bUnreserved = (Value >= 'A' && Value <= 'Z') || (Value >= 'a' && Value <= 'z') ||
				(Value >= '0' && Value <= '9') || Value == '-' || Value == '.' || Value == '_' || Value == '~';
			if (bUnreserved)
			{
				Piece.AppendChar(static_cast<TCHAR>(Value));
			}
			else
			{
				Piece.AppendChar(TEXT('%'));
				Piece.AppendChar(static_cast<TCHAR>(HexDigits[Value >> 4]));
				Piece.AppendChar(static_cast<TCHAR>(HexDigits[Value & 0x0F]));
			}
		}

		// The API ignores an oversized header outright, so stop at the last whole code point
		// that fits rather than send one it will throw away.
		if (Encoded.Len() + Piece.Len() > MaxDisplayNameHeaderLength)
		{
			break;
		}
		Encoded += Piece;
	}
	return Encoded;
}

FString FMantleConnection::ResolveServerToken()
{
	// Offline preview: a server token in the environment must not reach the network either.
	if (MantlePreviewLaunch::IsRequested())
	{
		return FString();
	}
	return FPlatformMisc::GetEnvironmentVariable(TEXT("MANTLE_SERVER_TOKEN")).TrimStartAndEnd();
}

namespace
{
	/** A uuid-shaped environment variable, lowercased, or empty. The value is only as trusted
	    as MANTLE_PLACE_ID beside it: whoever starts the process sets both. */
	FString ResolveUuidEnvironmentVariable(const TCHAR* Name)
	{
		const FString Value = FPlatformMisc::GetEnvironmentVariable(Name).TrimStartAndEnd();
		return MantleLooksLikePlaceId(Value) ? Value.ToLower() : FString();
	}
}

FString FMantleConnection::ResolvePrivateServerId()
{
	return ResolveUuidEnvironmentVariable(TEXT("MANTLE_PRIVATE_SERVER_ID"));
}

FString FMantleConnection::ResolvePrivateServerOwnerId()
{
	if (ResolvePrivateServerId().IsEmpty())
	{
		return FString();
	}
	return ResolveUuidEnvironmentVariable(TEXT("MANTLE_PRIVATE_SERVER_OWNER_ID"));
}

namespace
{
	/** Reads Count ASCII digits at Text[Index], advancing Index; false on anything else. */
	bool MantleRfc3339ReadDigits(const FString& Text, int32& Index, const int32 Count, int32& OutValue)
	{
		if (Index + Count > Text.Len())
		{
			return false;
		}
		int32 Value = 0;
		for (int32 Offset = 0; Offset < Count; ++Offset)
		{
			const TCHAR Char = Text[Index + Offset];
			if (Char < TEXT('0') || Char > TEXT('9'))
			{
				return false;
			}
			Value = Value * 10 + (Char - TEXT('0'));
		}
		Index += Count;
		OutValue = Value;
		return true;
	}

	/** Consumes Expected at Text[Index]; false when it isn't there. */
	bool MantleRfc3339ReadChar(const FString& Text, int32& Index, const TCHAR Expected)
	{
		if (Index >= Text.Len() || Text[Index] != Expected)
		{
			return false;
		}
		++Index;
		return true;
	}
}

bool FMantleConnection::ParseRfc3339Utc(const FString& Value, FDateTime& OutUtc)
{
	// RFC 3339 section 5.6: full-date "T" partial-time time-offset. Go's time.RFC3339 and
	// RFC3339Nano both produce this shape, which is what the API writes.
	const FString Text = Value.TrimStartAndEnd();
	int32 Index = 0;
	int32 Year = 0, Month = 0, Day = 0, Hour = 0, Minute = 0, Second = 0;
	if (!MantleRfc3339ReadDigits(Text, Index, 4, Year) || !MantleRfc3339ReadChar(Text, Index, TEXT('-'))
		|| !MantleRfc3339ReadDigits(Text, Index, 2, Month) || !MantleRfc3339ReadChar(Text, Index, TEXT('-'))
		|| !MantleRfc3339ReadDigits(Text, Index, 2, Day))
	{
		return false;
	}
	if (Index >= Text.Len() || (Text[Index] != TEXT('T') && Text[Index] != TEXT('t')))
	{
		return false;
	}
	++Index;
	if (!MantleRfc3339ReadDigits(Text, Index, 2, Hour) || !MantleRfc3339ReadChar(Text, Index, TEXT(':'))
		|| !MantleRfc3339ReadDigits(Text, Index, 2, Minute) || !MantleRfc3339ReadChar(Text, Index, TEXT(':'))
		|| !MantleRfc3339ReadDigits(Text, Index, 2, Second))
	{
		return false;
	}

	// An optional fraction, kept to the millisecond; any further digits are read and dropped.
	int32 Millisecond = 0;
	if (Index < Text.Len() && Text[Index] == TEXT('.'))
	{
		++Index;
		int32 Digits = 0;
		while (Index < Text.Len() && Text[Index] >= TEXT('0') && Text[Index] <= TEXT('9'))
		{
			if (Digits < 3)
			{
				Millisecond = Millisecond * 10 + (Text[Index] - TEXT('0'));
			}
			++Digits;
			++Index;
		}
		if (Digits == 0)
		{
			return false;
		}
		for (int32 Pad = Digits; Pad < 3; ++Pad)
		{
			Millisecond *= 10;
		}
	}

	// The zone is required: 'Z', or +hh:mm / -hh:mm, with nothing after it.
	if (Index >= Text.Len())
	{
		return false;
	}
	int32 OffsetMinutes = 0;
	const TCHAR Zone = Text[Index++];
	if (Zone == TEXT('+') || Zone == TEXT('-'))
	{
		int32 OffsetHour = 0, OffsetMinute = 0;
		if (!MantleRfc3339ReadDigits(Text, Index, 2, OffsetHour) || !MantleRfc3339ReadChar(Text, Index, TEXT(':'))
			|| !MantleRfc3339ReadDigits(Text, Index, 2, OffsetMinute) || OffsetHour > 23 || OffsetMinute > 59)
		{
			return false;
		}
		OffsetMinutes = (OffsetHour * 60 + OffsetMinute) * (Zone == TEXT('-') ? -1 : 1);
	}
	else if (Zone != TEXT('Z') && Zone != TEXT('z'))
	{
		return false;
	}
	if (Index != Text.Len())
	{
		return false;
	}

	// FDateTime counts from year 1; a leap second (60) is refused rather than rolled over.
	if (Year < 1 || !FDateTime::Validate(Year, Month, Day, Hour, Minute, Second, Millisecond))
	{
		return false;
	}
	// A time written at +02:00 is two hours ahead of UTC, so the offset comes off.
	OutUtc = FDateTime(Year, Month, Day, Hour, Minute, Second, Millisecond) - FTimespan::FromMinutes(OffsetMinutes);
	return true;
}

bool FMantleConnection::ResolveSessionEndsAt(FDateTime& OutEndsAtUtc)
{
#if !UE_BUILD_SHIPPING
	// A local run's stand-in for the API's value: an end this many seconds after it is read.
	double EndsInSeconds = 0.0;
	if (FParse::Value(FCommandLine::Get(), TEXT("MantleSessionEndsIn="), EndsInSeconds) && EndsInSeconds > 0.0)
	{
		OutEndsAtUtc = FDateTime::UtcNow() + FTimespan::FromSeconds(EndsInSeconds);
		return true;
	}
#endif
	const FString Value = FPlatformMisc::GetEnvironmentVariable(TEXT("MANTLE_SESSION_ENDS_AT")).TrimStartAndEnd();
	return !Value.IsEmpty() && ParseRfc3339Utc(Value, OutEndsAtUtc);
}

bool FMantleConnection::IsConfigured(FString& OutWhyNot, const FString& FallbackBaseUrl)
{
	if (ResolveApiBaseUrl(FallbackBaseUrl).IsEmpty())
	{
		OutWhyNot = TEXT("No Mantle server address is set. Set MANTLE_API_URL or pass -MantleApi=.");
		return false;
	}

	if (ResolveAuthToken().IsEmpty())
	{
#if WITH_EDITOR
		OutWhyNot = TEXT("You are not signed in to Mantle. Choose Tools > Mantle > Sign in with creator token and paste a token from Mantle's Settings > Account, or set MANTLE_AUTH_TOKEN to one (CI).");
#else
		OutWhyNot = TEXT("You are not signed in to Mantle.");
#endif
		return false;
	}

	return true;
}
