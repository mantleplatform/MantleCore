// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/**
 * The editor's Mantle sign-in (GhzGarage/mantle#94): a pasted creator token.
 *
 * A creator makes a token in the Mantle game's Settings > Account while signed in there with
 * Steam or Epic, and pastes it here. The token acts as that player on the creator routes only
 * (GET /me, /places, /assets, /scripts), so a place published with it belongs to the account
 * they play with. It is held in memory only, through FMantleConnection::SetRuntimeAuthToken,
 * until the editor closes or the creator signs out: never in config, never on disk.
 *
 * MANTLE_AUTH_TOKEN keeps precedence, and takes a creator token too (CI, scripts). While it is
 * set, nothing here asks for or holds a token: pasting one could never be used.
 *
 * A 401 from the API (FMantleConnection::OnSessionRejected) forgets the pasted token, so the
 * next action that needs one asks again. No EOS, no Steam, no browser: HTTP only, as the plugin
 * must stay dependency-free for Fab.
 */
class FMantleCreatorSignIn
{
public:
	/** Listens for the API refusing the pasted token. */
	static void Startup();
	/** Forgets the token and stops listening. */
	static void Shutdown();

	/** True when a request would carry a token: MANTLE_AUTH_TOKEN, or a pasted creator token. */
	static bool IsSignedIn();
	/** True when that token is MANTLE_AUTH_TOKEN. */
	static bool IsUsingEnvironmentToken();
	/** "Signed in as <name>", or how the editor is signed in when the name is not known. */
	static FText DescribeSignedIn();

	/**
	 * The account the current token acts as at the current API (#394: a level's place is
	 * remembered per account and API). Known from sign-in; otherwise (MANTLE_AUTH_TOKEN, or the
	 * address changed) GET /me asks, blocking. False with OutWhyNot when signed out or Mantle
	 * could not say.
	 */
	static bool ResolveAccountId(FString& OutAccountId, FString& OutWhyNot);

	/** The "id" of a GET /me response; empty when it is not one. */
	static FString ParseAccountId(const struct FMantleApiResult& Me);

	/**
	 * Just in time, for an action that needs the API (Save, Publish): true straight away when
	 * signed in; otherwise the sign-in dialog, with Reason at its top, and true once a token was
	 * pasted and accepted. False when the creator cancelled, or in an unattended editor, which
	 * never opens a dialog; the action then stops without sending anything.
	 */
	static bool EnsureSignedIn(const FText& Reason);

	/** The dialog on its own (Tools > Mantle > Sign in with creator token). True when signed in afterwards. */
	static bool OpenSignInDialog(const FText& Reason);

	/** Forgets the pasted token. Does nothing to MANTLE_AUTH_TOKEN. */
	static void SignOut();

	/**
	 * Checks a pasted value before anything is sent. OutToken is it trimmed, with a leading
	 * "Bearer " removed (copied from a header); false with a creator-facing reason when it
	 * cannot be a creator token: empty, not one line of printable characters, not mct1., or
	 * missing its ct_ id or secret.
	 */
	static bool CheckTokenShape(const FString& Pasted, FString& OutToken, FString& OutWhyNot);

	/**
	 * Holds Token in memory and asks GET /me who it belongs to. True on success (the display
	 * name is kept for DescribeSignedIn); otherwise the token is forgotten again and OutWhyNot says why, written for the
	 * sign-in dialog. Blocking, like every editor call to the API.
	 */
	static bool SignInWithToken(const FString& Token, FString& OutWhyNot);

private:
	static void HandleSessionRejected(const FString& Message, const FString& RejectedToken);
};
