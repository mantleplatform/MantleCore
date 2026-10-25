// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Misc/DateTime.h"

/**
 * Fired on the game thread when the Mantle API rejects a bearer token (HTTP 401). Carries
 * the API's player-facing message and the token that was refused, so a listener can tell a
 * stale reply -- one still in flight from before a re-sign-in -- from a rejection of the
 * token it is using now. The client listens and signs in with Epic again rather than
 * resending a token the service has already refused.
 */
DECLARE_MULTICAST_DELEGATE_TwoParams(
	FMantleSessionRejectedEvent, const FString& /*Message*/, const FString& /*RejectedToken*/);

/**
 * Where the Mantle server is and who we are talking to it as.
 *
 * Lives in the runtime module because both the editor plugin and the shipped client need
 * it. A packaged client receives its short-lived EOS identity token at runtime. Developers
 * can still use the environment override for local editor work; neither path writes a player
 * token to project config or the command line.
 */
struct MANTLERUNTIME_API FMantleConnection
{
	/** Used when nothing else supplies an address: local in editor, hosted in packaged builds. */
	static const TCHAR* DefaultBaseUrl;

	/**
	 * Mantle's hosted API, the one a packaged client uses, in every build. The editor plugin
	 * publishes there by default (#394): a creator who installed the plugin from Fab has no local
	 * API, whatever an editor-built -game process defaults to.
	 */
	static const TCHAR* HostedBaseUrl;

	/**
	 * The API address, trailing slash removed.
	 *
	 * Resolution order, most explicit first: the -MantleApi= command line, the
	 * MANTLE_API_URL environment variable, the caller's Fallback, then the default. The
	 * editor passes its Project Settings value as the Fallback so that setting still works,
	 * while a client launched from a shortcut needs neither.
	 */
	static FString ResolveApiBaseUrl(const FString& Fallback = FString());

	/**
	 * The bearer token. MANTLE_AUTH_TOKEN is the override, honoured only in non-Shipping builds
	 * (the editor, CI, local development): a developer token for a local API, or a creator token
	 * (mct1., #94) against the hosted one. Otherwise the in-memory token SetRuntimeAuthToken
	 * holds: the client's short-lived EOS token after sign-in, or the creator token pasted into
	 * the editor. A Shipping client uses nothing but the former; a stray environment variable
	 * must never be able to silence Epic sign-in in the client players install.
	 *
	 * Deliberately not available on the command line. A token in an argument list is
	 * visible to every other process on the machine, and one mechanism is easier to keep
	 * honest than two.
	 */
	static FString ResolveAuthToken();

	/** Supplies or clears the in-memory token (the client's EOS token, the editor's pasted creator token). Never persists it. */
	static void SetRuntimeAuthToken(const FString& Token);
	static void ClearRuntimeAuthToken();

	/**
	 * True when ResolveAuthToken answers with MANTLE_AUTH_TOKEN rather than the in-memory token,
	 * so a caller can tell a creator which of the two was refused and whether signing out or
	 * pasting another token could change anything (it cannot: the variable takes precedence).
	 */
	static bool IsAuthTokenFromEnvironment();

	/**
	 * The launch store's display name for the signed-in player (Steam persona, Epic display
	 * name), sent on every authenticated request as X-Mantle-Display-Name so the API can name
	 * a player whose sign-in token carries no `dn`. In memory only, like the token; empty
	 * clears it, and nothing is sent while it is empty (a developer-auth run never sets it).
	 */
	static void SetRuntimeDisplayName(const FString& DisplayName);

	/** The header name the API reads the reported display name from. */
	static const TCHAR* DisplayNameHeader;

	/** The encoded X-Mantle-Display-Name value for the current display name, or empty. */
	static FString ResolveDisplayNameHeader();

	/**
	 * Percent-encodes a display name's UTF-8 bytes per RFC 3986 (unreserved characters stay,
	 * everything else is %XX, space included as %20), so any name travels as ASCII.
	 *
	 * The API ignores a value longer than MaxDisplayNameHeaderLength, so a name whose encoding
	 * would exceed it is cut at the last whole code point that fits rather than dropped; the
	 * server's display-name rules then apply to what arrives. Empty in, empty out.
	 */
	static FString EncodeDisplayNameHeader(const FString& DisplayName);

	/** The API's limit on the encoded header: 32 code points of up to 4 bytes, 3 chars each. */
	static constexpr int32 MaxDisplayNameHeaderLength = 32 * 4 * 3;

	/** Raised by the API client whenever a request comes back 401. */
	static FMantleSessionRejectedEvent& OnSessionRejected();

	/** The narrow bootstrap credential injected into a dedicated server process. */
	static FString ResolveServerToken();

	/**
	 * Private servers (PS7, GhzGarage/mantle#420): the private server a hosted session belongs to,
	 * from MANTLE_PRIVATE_SERVER_ID, which the API sets beside MANTLE_PLACE_ID only when it
	 * allocates a private server's session. Empty for a public session, and for anything that
	 * isn't a uuid. Lowercase, as the API writes ids.
	 */
	static FString ResolvePrivateServerId();

	/**
	 * The Mantle account id of that private server's owner, from MANTLE_PRIVATE_SERVER_OWNER_ID.
	 * Empty whenever ResolvePrivateServerId() is, so an owner never appears without a server.
	 */
	static FString ResolvePrivateServerOwnerId();

	/**
	 * GhzGarage/mantle#527: the hard end of this hosted session, from MANTLE_SESSION_ENDS_AT,
	 * which the API sets beside MANTLE_PRIVATE_SERVER_ID for every Edgegap deployment it
	 * allocates (the allocation time plus the public or private lifetime). False, with
	 * OutEndsAtUtc untouched, when it is unset (a community server, a local run) or not an
	 * RFC 3339 time: no end.
	 *
	 * Outside Shipping, -MantleSessionEndsIn=<seconds> overrides it with an end that many
	 * seconds from now, so a local -server run can exercise the warnings and the restart.
	 */
	static bool ResolveSessionEndsAt(FDateTime& OutEndsAtUtc);

	/**
	 * An RFC 3339 date-time ("2026-09-27T18:04:05Z", "2026-09-27T20:04:05.123+02:00") as UTC.
	 * Strict: the full date, the time to the second, and a zone ('Z' or an offset) are all
	 * required, so a bare date or a zoneless time is refused rather than guessed at. Leading
	 * and trailing spaces are ignored. False, with OutUtc untouched, for anything else.
	 */
	static bool ParseRfc3339Utc(const FString& Value, FDateTime& OutUtc);

	/** True when both an address and a token are available; otherwise says what is missing. */
	static bool IsConfigured(FString& OutWhyNot, const FString& FallbackBaseUrl = FString());
};
