// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/** One library item version, as the storage resolve routes take and name it. */
struct FMantleStorageRef
{
	/** itm_u and 32 lowercase hex digits. */
	FString Item;

	/** The item's version, at least 1. */
	int32 Version = 0;
};

/** One API call's outcome. */
struct FMantleApiResult
{
	bool bSuccess = false;

	/** HTTP status, or 0 when the request never reached the server. */
	int32 StatusCode = 0;

	FString Body;

	/** A message written for the creator, not a status line. */
	FString Message;

	/** Machine-readable code from the API, when it supplied one. */
	FString Code;

	/**
	 * Seconds the server asked the caller to wait before trying again (its Retry-After header,
	 * sent with a 429 rate limit or a 503), or 0 when it sent none. The plugin's upload queue
	 * waits exactly this long instead of guessing.
	 */
	float RetryAfterSeconds = 0.0f;
};

/**
 * One failed check from a compatibility_failed refusal (422, the release gate): the report the
 * API sends lists every check, passed or not, and only the failed ones are worth a creator's time.
 */
struct FMantleCompatibilityFailure
{
	/** The check's id, e.g. "script-api-max-version" or "entity-types-released". */
	FString Name;

	/** Where in the place file the check looked, e.g. "/scriptApi"; empty when it has no place. */
	FString Path;

	/** What failed, in plain words, led by what the check is about. */
	FString Problem;

	/** What to do about it; empty when the API said nothing beyond the problem. */
	FString Remedy;

	/** Problem, Remedy and Path as one line for the Output Log. */
	FString ToLine() const;
};

DECLARE_DELEGATE_OneParam(FMantleApiCallback, const FMantleApiResult&);

/**
 * The query options of POST /assets (BACKEND.md "Assets"; A2.2 and F9 #343). Empty ones are left
 * out of the query. Which option fits which kind is the server's rule (invalid_options).
 */
struct FMantleAssetUploadOptions
{
	/** texture, mesh, avatar, bodyshape, skinnedmesh, animation, dna, coverage. */
	FString Kind;

	/** "face" for an avatar's face; empty (the body) otherwise. */
	FString Part;

	/** color, normal or data for a texture a material samples (cooked to MTX1); empty for images the UI shows. */
	FString Role;

	/** A skinnedmesh's body shape: the ast_u id of the approved bodyshape it was authored on. */
	FString Source;
};

/** An asset as the API describes it (POST /assets, POST /assets/diff's known[], GET /assets/:id). */
struct FMantleUploadedAsset
{
	/** ast_u + 32 hex digits. */
	FString Id;

	/** "sha256:" + hex, the content address. */
	FString Hash;

	FString Kind;

	/** pending | processing | approved | rejected. */
	FString Status;

	/** pending | processing | passed | failed. */
	FString CookState;

	/** pending | passed | review | rejected. */
	FString ModerationState;

	/** POST /assets only: true when these bytes were new to the server, false when they only gained the caller a claim. */
	bool bCreated = false;

	/** The options the bytes were first uploaded with (cookOptions): one hash is one upload, one role. Kind is not set here. */
	FMantleAssetUploadOptions CookOptions;

	/** Where the cooked object downloads from (a CDN URL); empty until the asset is approved. */
	FString Url;

	/** "blake3:" + 64 hex over the cooked object at Url (F2 #336), what a download is verified against; empty until cooked. */
	FString CookedHash;

	/** The cooked object's length; 0 until cooked. */
	int64 CookedBytes = 0;

	/** Why the cook refused the asset, when it did. */
	FString CookError;

	/** Materials: cookInfo.textures, slot ("baseColor", "normal", "orm", "emissive") to texture id. */
	TMap<FString, FString> MaterialTextures;

	bool IsApproved() const { return Status == TEXT("approved"); }
	bool IsRejected() const { return Status == TEXT("rejected"); }
};

/** One file of a script package version (#838): what a place's scripts[] entry carries. */
struct FMantleLibraryScriptFile
{
	/** "Server/...", "Client/..." or "Shared/...", as the package laid it out. */
	FString Path;

	/** "server", "client" or "shared". */
	FString Side;

	/** "luau", "javascript", ... as the API wrote it. */
	FString Language;

	/** The approved source record (src_...), readable with GetScriptSourceAsync. */
	FString Source;
};

/** One parameter a script package declares in its manifest (#838). */
struct FMantleLibraryScriptParameter
{
	FString Name;

	/** The declared type ("number", "string", "bool", ...) as the API wrote it. */
	FString Type;

	/** The default as text: a string as is, a number or bool written out, anything else empty. */
	FString Default;

	FString Help;

	/** Stage 6: the place must give a value when there is no default. */
	bool bRequired = false;

	/** Stage 6, a number's bounds when the manifest gives them. */
	bool bHasMin = false;
	double Min = 0.0;
	bool bHasMax = false;
	double Max = 0.0;

	/** Stage 6, a choice's values. */
	TArray<FString> Options;
};

/** A script package version's manifest (#838): {name, description, parameters, settings (stage 6)}. */
struct FMantleLibraryScriptManifest
{
	FString Name;
	FString Description;
	TArray<FMantleLibraryScriptParameter> Parameters;
	TArray<FMantleLibraryScriptParameter> Settings;
};

/** One published version of a library item, from GET /library/items/{id}'s versions[] (#614 stage 6). */
struct FMantleLibraryVersion
{
	int32 Version = 0;

	/** RFC 3339 time as the API wrote it; empty when absent. */
	FString CreatedAt;

	/** The version's main uploaded asset (ast_u...); empty for a script package (#838). */
	FString PrimaryAssetId;

	TArray<FString> DependencyAssetIds;

	/** A script package's files (#838); empty for a vehicle or weapon. */
	TArray<FMantleLibraryScriptFile> Scripts;

	/** A script package's manifest (#838); default for a vehicle or weapon. */
	FMantleLibraryScriptManifest Manifest;
};

/** One of the caller's own places that stores an item, from GET /library/items/{id}'s storedByMyPlaces (#838). */
struct FMantleLibraryPlaceRef
{
	FString Id;
	FString Name;
};

/**
 * One library item as the library routes describe it (#614 stage 6): a card of GET /library/items or
 * GET /library/items/mine, or GET /library/items/{id}, which adds Versions.
 */
struct FMantleLibraryCard
{
	/** itm_u and 32 lowercase hex digits. */
	FString Id;

	/** "vehicle", "weapon" or "script" (#838). */
	FString Kind;

	FString Name;
	FString Description;

	/** "private" or "public". */
	FString Visibility;

	/** "active", "delisted" or "taken_down" (what the API reports; a stored item loads only as active or delisted, #614 stage 7). */
	FString Status;

	FString CreatorId;
	FString CreatorName;
	FString CreatorHandle;
	bool bCreatorOfficial = false;

	/** The highest version number; 0 when the item has none. */
	int32 LatestVersion = 0;

	/** The caller published it. */
	bool bOwned = false;

	/** The caller got another account's public item and it is still usable. */
	bool bAcquired = false;

	/** Where the card image downloads (absolute, or a path on the API); empty for none (null in the answer). */
	FString ThumbnailUrl;

	FString CreatedAt;
	FString UpdatedAt;

	/** The price (#838); 0 is free, and nothing sets another value before Phase 7. Absent is 0. */
	int32 Price = 0;

	/** How the caller got it, on a got card ("free" today, #838); empty when absent or not got. */
	FString Source;

	/** GET /library/items/{id} only; empty on a list card. */
	TArray<FMantleLibraryVersion> Versions;

	/** GET /library/items/{id} only (#838): the caller's own places that store this item; empty when none or absent. */
	TArray<FMantleLibraryPlaceRef> StoredByMyPlaces;

	/** True when the item costs nothing. */
	bool IsFree() const { return Price <= 0; }

	/** True when the item is private (case-sensitive: the API's values are lowercase). */
	bool IsPrivate() const { return Visibility.Equals(TEXT("private"), ESearchCase::CaseSensitive); }

	/** True when the caller may put the item in a place's storage: owned or acquired. */
	bool CanUse() const { return bOwned || bAcquired; }
};

/** The filters of a library list request (GET /library/items and /library/items/mine). */
struct FMantleLibraryQuery
{
	/** "vehicle", "weapon" or "script" (#838); empty for every kind. */
	FString Kind;

	/** Free text (q); empty for none. The API takes at most 100 characters. */
	FString Search;

	/** Cards per page, 1 to 50. */
	int32 Limit = 24;
};

/**
 * Talks to the Mantle API.
 *
 * Lives in the runtime module because both the editor plugin and the shipped client need
 * it, and two implementations of "how we talk to our own backend" would drift apart. This
 * is the same reasoning that keeps the place builder shared.
 *
 * Two ways to call, for two genuinely different situations:
 *
 * - Blocking, for the editor. Publishing is a deliberate action a creator waits on, and
 *   blocking keeps the editor code straightforward.
 * - Asynchronous, for the client. A player joining a place must not have their game freeze
 *   for the length of a network round trip, however short.
 *
 * The player-facing calls (session join/heartbeat/leave, moderation, discovery, chat,
 * voice, avatar) live in FMantleClientApi (MantleClient project module), built on top of
 * Send/SendAsync/SendBytesAsync here — those three stay the one implementation of request
 * building, which is why they are public rather than duplicated.
 */
class MANTLERUNTIME_API FMantleApiClient
{
public:
	/** True when both an address and a token are available. */
	static bool IsConfigured(FString& OutWhyNot);

	/** Uploads the place file as the level's working copy. */
	static FMantleApiResult SaveDraft(const FString& PlaceId, const FString& PlaceJson);

	static FMantleApiResult Publish(const FString& PlaceId, const FString& Notes);

	/** Fetches the published place file players receive; returns immediately and calls back on the game thread. */
	static void GetLiveAsync(const FString& PlaceId, FMantleApiCallback OnDone);

	/**
	 * Fetches one numbered revision — live or archived, never a draft — and calls back on the game thread. Never
	 * what /live would currently answer: a session pins to whatever revision was live when it started, and this
	 * is how a client honours that pin instead of quietly reconstructing whatever is live now.
	 */
	static void GetRevisionAsync(const FString& PlaceId, int32 RevisionNumber, FMantleApiCallback OnDone);

	static FMantleApiResult ListMine();

	/** Uploads raw Luau source and returns its content-addressed src_ record. */
	static FMantleApiResult UploadScript(const FString& Source, const FString& Language = TEXT("luau"));

	/** Fetches approved raw source by src_ id. */
	static FMantleApiResult GetScriptSource(const FString& SourceId);
	static void GetScriptSourceAsync(const FString& SourceId, FMantleApiCallback OnDone);
	/** Bootstrap-revision Server/Shared executable content, using only the server token.
	 * Same expiry lifetime as bootstrap; not an active-session authorization. */
	static void GetServerScriptSourceAsync(const FString& SourceId, FMantleApiCallback OnDone);

#if WITH_DEV_AUTOMATION_TESTS
	/** Answers every POST under /server/ in place of the network (path, body, callback), token or not. */
	static TFunction<void(const FString& Path, const FString& Body, FMantleApiCallback)> ServerTokenRequestForTesting;
#endif

	/**
	 * Creator uploads (A2.4 #283; shared with the Fab track's bulk queue, F4 #338). All
	 * asynchronous: an upload is a large body and the plugin shows progress while it runs.
	 *
	 * POST /assets/diff {hashes}: which of these content hashes the caller already holds a claim
	 * on (known, returned whole) and which it must upload (unknown). An unknown hash may be bytes
	 * the server already has from someone else: uploading them is cheap and is what gives the
	 * caller its claim (#337), so unknown always means "upload".
	 */
	static void DiffAssetsAsync(const TArray<FString>& Hashes, FMantleApiCallback OnDone);

	/** POST /assets?kind=...: the bytes as the body (never a form or base64), UploadTimeoutSeconds to finish. */
	static void UploadAssetAsync(const FMantleAssetUploadOptions& Options, const TArray<uint8>& Bytes, FMantleApiCallback OnDone);

	/** GET /assets/:id: status, cook and moderation state. */
	static void GetAssetAsync(const FString& AssetId, FMantleApiCallback OnDone);

	/**
	 * POST /assets/resolve {"ids": [...]}: GET /assets/:id for many ids in one request, for a place's
	 * uploaded assets at join. Answers {"assets": [asset], "missing": [id]}; the textures
	 * of each approved material are included even when not asked for. A backend without the route
	 * answers 404 or 405 with no API error code, and FMantlePlaceAssets then falls back to one
	 * GetAssetAsync per id.
	 */
	static void ResolveAssetsAsync(const TArray<FString>& Ids, FMantleApiCallback OnDone);

	/**
	 * POST /server/assets/resolve, the dedicated server's form: the same body and answer,
	 * authenticated with MANTLE_SERVER_TOKEN and limited to the assets the session's pinned revision
	 * references. Never falls back to a player credential.
	 */
	static void ResolveServerAssetsAsync(const TArray<FString>& Ids, FMantleApiCallback OnDone);

	/**
	 * POST /places/{placeId}/storage/resolve {"entries": [{"item", "version"}]} (#614 stage 4): expands the
	 * library item versions a place stores into their backing asset ids. Answers 200 with
	 * {"entries": [...], "missing": [{"item", "version"}]}, read by FMantleStorage::ParseResponse. The player's
	 * credential, like ResolveAssetsAsync.
	 */
	static void ResolveStorageAsync(const FString& PlaceId, const TArray<FMantleStorageRef>& Entries, FMantleApiCallback OnDone);

	/**
	 * POST /server/storage/resolve, the dedicated server's form: the same body and answer, authenticated with
	 * MANTLE_SERVER_TOKEN like ResolveServerAssetsAsync. Never falls back to a player credential.
	 */
	static void ResolveServerStorageAsync(const TArray<FMantleStorageRef>& Entries, FMantleApiCallback OnDone);

	/** {"entries":[{"item":"itm_u...","version":1}]}, properly escaped JSON. */
	static FString MakeResolveStorageBody(TConstArrayView<FMantleStorageRef> Entries);

	/**
	 * POST /server/avatar/metrics (#581), the dedicated server's read of avatar items' body metrics, authenticated
	 * with MANTLE_SERVER_TOKEN: {"ids": [...]} (1 to 100 item ids) answers {"items": [{"id", "bodyMetrics"}],
	 * "missing": [id]} (MantleCosmetic::ParseServerBodyMetrics). What a character's gameplay size is computed from, so
	 * it is never a client's claim. Never falls back to a player credential; without a server token OnDone gets a
	 * failed result at once.
	 */
	static void GetServerAvatarBodyMetricsAsync(const TArray<FString>& ItemIds, FMantleApiCallback OnDone);

	/**
	 * The library routes (#614 stage 6), all with the caller's token, all asynchronous.
	 *
	 * GET /library/items (bMine false: the caller's own items and, while the server allows it, other accounts' public
	 * ones) or GET /library/items/mine (bMine true: the caller's own and the ones they got). Answers
	 * {"items": [card], "nextCursor"?}, read by ParseLibraryCards. Cursor is empty for the first page.
	 */
	static void ListLibraryItemsAsync(const FMantleLibraryQuery& Query, bool bMine, const FString& Cursor, FMantleApiCallback OnDone);

	/** GET /library/items/{id}: the card and its versions, read by ParseLibraryItem. */
	static void GetLibraryItemAsync(const FString& ItemId, FMantleApiCallback OnDone);

	/** POST /library/items/{id}/get: a public item joins the caller's library. Answers 204; repeating it is harmless. */
	static void AcquireLibraryItemAsync(const FString& ItemId, FMantleApiCallback OnDone);

	/**
	 * DELETE /library/items/{id}/get (#838): a got item leaves the caller's library. Answers 204; repeating it, or
	 * naming the caller's own item, is harmless. A place that still stores it refuses its next publish.
	 */
	static void RemoveLibraryItemAsync(const FString& ItemId, FMantleApiCallback OnDone);

	/** The request path of a list call: "/library/items?kind=vehicle&q=...&limit=24&cursor=...", values URL-encoded, empty ones left out. */
	static FString MakeLibraryListPath(const FMantleLibraryQuery& Query, bool bMine, const FString& Cursor);

	/**
	 * Reads a list answer. False, with OutError written for a creator and the outputs empty, when the text is not
	 * {"items": [card]} or a card is not an object with an id. A card's other keys are optional: a missing one keeps its
	 * default, and a null thumbnailUrl is an empty ThumbnailUrl. OutNextCursor is empty when there is no more.
	 */
	static bool ParseLibraryCards(const FString& Json, TArray<FMantleLibraryCard>& OutCards, FString& OutNextCursor, FString& OutError);

	/** Reads GET /library/items/{id}'s answer: one card with its versions. False with OutError when it is not one. */
	static bool ParseLibraryItem(const FString& Json, FMantleLibraryCard& OutCard, FString& OutError);

	/** Ids per resolve request this client sends (the contract allows up to 500). */
	static constexpr int32 MaxIdsPerResolve = 200;

	/** {"ids":[...]}, properly escaped JSON. */
	static FString MakeResolveAssetsBody(TConstArrayView<FString> Ids);

	/** Reads {"assets": [asset], "missing": [id]}. False when it is not that shape. */
	static bool ParseResolvedAssets(const FString& Json, TArray<FMantleUploadedAsset>& OutAssets, TArray<FString>& OutMissing);

	/** True for a response that says the route does not exist (404 or 405 without an API error code). */
	static bool IsMissingRoute(const FMantleApiResult& Response);

	/**
	 * POST /avatar/items with a complete item body (A2.1): 201 and the creation, or {code, message,
	 * field}. IdempotencyKey, when set, goes as the Idempotency-Key header (#394): a repeat with the
	 * same key answers 200 with the item the first request made, so a retry never makes two.
	 */
	static void CreateAvatarItemAsync(const FString& ItemJson, const FString& IdempotencyKey, FMantleApiCallback OnDone);

	/** The header that names one creation across retries (the backend's api.IdempotencyKeyHeader). */
	static constexpr const TCHAR* IdempotencyKeyHeader = TEXT("Idempotency-Key");

	/** GET /me/avatar/creations/:id: the caller's own item in any status, for following its screening. */
	static void GetAvatarCreationAsync(const FString& ItemId, FMantleApiCallback OnDone);

	/** Seconds an asset upload may take: up to 64 MiB on a slow line. */
	static constexpr float UploadTimeoutSeconds = 300.0f;

	/** "/assets?kind=skinnedmesh&source=ast_u..." with every value URL-encoded and empty options left out. */
	static FString MakeAssetUploadPath(const FMantleAssetUploadOptions& Options);

	/** {"hashes":[...]}, properly escaped JSON. */
	static FString MakeDiffAssetsBody(TConstArrayView<FString> Hashes);

	/** Reads one asset object (POST /assets or GET /assets/:id). False when it is not JSON or has no id. */
	static bool ParseUploadedAsset(const FString& Json, FMantleUploadedAsset& OutAsset);

	/** Reads POST /assets/diff's {known: [asset], unknown: [hash]}. False when it is not that shape. */
	static bool ParseDiffAssets(const FString& Json, TArray<FMantleUploadedAsset>& OutKnown, TArray<FString>& OutUnknown);

	/**
	 * A Retry-After header's value in seconds: delta-seconds ("120") or an HTTP date, measured from
	 * Now. 0 when it is empty, unreadable or already past. Capped at a day, so a bad header cannot
	 * park a caller for ever.
	 */
	static float ParseRetryAfter(const FString& Header, const FDateTime& Now);

	/** Fetches the one pinned place granted to this dedicated server process. */
	static void GetServerBootstrapAsync(FMantleApiCallback OnDone);

	/**
	 * Exchanges a single-use join credential for the verified account id that requested it,
	 * plus the player's Epic display name. See BACKEND.md's "Join credential": POST
	 * /server/sessions/verify, server-token-only, body {"credential"}, response
	 * {"userId", "displayName"}. Every failure — forged, expired, used, wrong session,
	 * released seat, or no MANTLE_SERVER_TOKEN at all — comes back as the same generic 401
	 * the backend itself gives, so a hosted dedicated server's PreLoginAsync
	 * (AMantleGameMode) never has to distinguish why a credential didn't verify, only that it
	 * didn't.
	 *
	 * displayName is server-resolved (users.display_name), never something the connecting
	 * client supplied: a dedicated server's own token cannot call the player-only GET
	 * /profiles/:id, so this response is the only route it has to a verified name at all, and
	 * the only one a hosted session may trust (S1.11 review — a client-sent name would let
	 * any player claim any name).
	 */
	static void VerifyJoinCredentialAsync(const FString& Credential, FMantleApiCallback OnDone);

	/**
	 * This binary's build, for the client version gate (GhzGarage/mantle#184): ReleaseBuild
	 * under [Mantle] in DefaultGame.ini, which the release workflow stamps with the release's
	 * git-<sha> into both the client package and the server image
	 * (tools/ci/Set-ReleaseBuild.ps1). A client sends it on POST /sessions/join and a
	 * dedicated server on POST /server/sessions/verify; the API refuses a join whose two
	 * builds differ, with a message that says to update, instead of letting Iris drop the
	 * connection on a protocol mismatch. Empty for a developer build with nothing stamped,
	 * and empty means the gate does not apply to it. A Mantle key rather than the engine's
	 * ProjectVersion, which BaseGame.ini defaults to 1.0.0.0 for every unstamped build.
	 */
	static FString ProjectBuild();

	/**
	 * POST /server/sessions/verify's body. Public so the test can pin the exact JSON:
	 * serverBuild rides along only when non-empty, so a server with no stamped build sends
	 * the byte-identical body every server sent before the field existed.
	 */
	static FString BuildVerifyJoinCredentialBody(const FString& Credential, const FString& ServerBuild);

	/**
	 * S1.13's DataStore service (MantleScript/Private/MantleScriptDataStore.cpp): the four
	 * server-token-only calls behind BACKEND.md's "Creator DataStore" --
	 * POST /server/datastore/{read,list,write,delete}. Scope is "world" or "player"; PlayerId is
	 * the seated account UUID for "player" scope, empty for "world" (the backend's own
	 * datastore.Subject maps that to its internal "*"). Every failure -- no MANTLE_SERVER_TOKEN,
	 * unreachable, rate-limited, a version conflict, over quota -- comes back as an unsuccessful
	 * FMantleApiResult with StatusCode/Code set exactly as the backend sent them; none of these
	 * four ever throw or block, matching every other *Async call here.
	 */
	static void DataStoreReadAsync(const FString& Scope, const FString& PlayerId, const FString& Key,
		FMantleApiCallback OnDone);
	/** Bounded prefix export, in key order; the backend clamps Limit to at most
	    datastore.MaxKeysPerSubject (1024), and treats 0 or less as 100. Cursor is empty for the
	    first page, or the "nextCursor" the previous page's response carried (#464): a response
	    names one only when more keys remain. */
	static void DataStoreListAsync(const FString& Scope, const FString& PlayerId, const FString& Prefix,
		const FString& Cursor, int32 Limit, FMantleApiCallback OnDone);
	/** ValueEnvelopeJson is the closed bool/int/float/text envelope the backend's canonicalValue
	    expects, already-encoded (see MantleScriptDataStore.cpp's EncodeValueEnvelope). Sending
	    ExpectedVersion (0 for a key that has never been confirmed to exist) is what turns this
	    into an optimistic write the backend can refuse with a 409 conflict. */
	static void DataStoreWriteAsync(const FString& Scope, const FString& PlayerId, const FString& Key,
		const FString& ValueEnvelopeJson, TOptional<int64> ExpectedVersion, FMantleApiCallback OnDone);
	static void DataStoreDeleteAsync(const FString& Scope, const FString& PlayerId, const FString& Key,
		TOptional<int64> ExpectedVersion, FMantleApiCallback OnDone);
	/** #464: one ordered-store call, POST /server/datastore/ordered/<Operation> (set, increment,
	    get, remove or sorted), with BodyJson already built by the caller (MantleScriptDataStore.cpp):
	    {"store","key","value"|"delta"} or {"store","ascending","pageSize","cursor"}. Failures come
	    back exactly as the four calls above report them. */
	static void DataStoreOrderedAsync(const FString& Operation, const FString& BodyJson, FMantleApiCallback OnDone);

	/**
	 * GhzGarage/mantle#465: the chat service behind a place's server scripts
	 * (MantleScript/Private/MantleScriptChat.cpp), server-token-only like DataStore. The session
	 * is always the one this server's token names.
	 *
	 * ServerChatMessagesAsync: POST /server/chat/messages, the session's player lines past After
	 * ({"messages":[...],"nextAfter":N}), post-filter. An unset After starts the feed at the
	 * present: no messages, only the cursor to read from next.
	 *
	 * ServerChatSystemAsync: POST /server/chat/system, one system line through the chat filter,
	 * to the whole session (PlayerId empty) or to one seated player (their account UUID).
	 */
	static void ServerChatMessagesAsync(TOptional<int64> After, FMantleApiCallback OnDone);
	static void ServerChatSystemAsync(const FString& Text, const FString& PlayerId, FMantleApiCallback OnDone);
	/** Script API 1.44: the same, to exactly these seated players (account UUIDs) as one line, kept private to them. */
	static void ServerChatSystemToAsync(const FString& Text, const TArray<FString>& PlayerIds, FMantleApiCallback OnDone);
	/** The two bodies above, public so a test can pin them. Text is JSON-escaped in full (a
	 *  script's text may hold quotes, backslashes and control characters); playerId rides along
	 *  only when set. */
	static FString BuildServerChatMessagesBody(TOptional<int64> After);
	static FString BuildServerChatSystemBody(const FString& Text, const FString& PlayerId,
		const TArray<FString>& PlayerIds = TArray<FString>());

	/**
	 * GhzGarage/mantle#813: players send chat to their game server, which relays it here.
	 * POST /server/chat/messages/ingest with BodyJson already built by the relay
	 * (MantleChatRelay.cpp): {"lines":[{"playerId","clientMessageId","body"}]}, at most 32. The
	 * answer holds one result per line, in order: its verdict, the stored line and hiddenFrom.
	 */
	static void ServerChatIngestAsync(const FString& BodyJson, FMantleApiCallback OnDone);

	/**
	 * ServerHttpSummaryAsync (GhzGarage/mantle#548, #1012): POST /server/http/summary, once a minute, what this server's
	 * scripts did over outbound HTTP: {"minute":"<RFC 3339>","entries":[{"script","host","port","requests","status2xx",
	 * "status3xx","status4xx","status5xx","failures","refusals","bytesOut","bytesIn"}]}, at most 200 entries. Metadata
	 * only (a host, never a URL, path, header or body); the backend keeps it so an abuse report can be traced after this
	 * server's logs are gone. Authenticated by the dedicated-server token.
	 */
	static void ServerHttpSummaryAsync(const FString& BodyJson, FMantleApiCallback OnDone);
	/**
	 * GhzGarage/mantle#813: POST /server/seats/heartbeat, once a minute with the account ids of
	 * every player connected to this server, in place of each player's own heartbeat. Answers
	 * {"alive":[...]}: the players whose seat it refreshed.
	 */
	static void ServerSeatsHeartbeatAsync(const TArray<FString>& UserIds, FMantleApiCallback OnDone);
	/**
	 * Server-plan R7: POST /server/sessions/place-ready, once, when the hosted server's place is built (the
	 * MANTLE_READY line). It tells the backend this session is joinable: until then an Edgegap session is held
	 * back from players, because the container runs long before the place does. Answers 204; idempotent.
	 * A 401 right after allocation means the backend has not bound this server's token to its session yet, so
	 * the caller retries. Authenticated by the dedicated-server token.
	 */
	static void ServerPlaceReadyAsync(FMantleApiCallback OnDone);
	static FString BuildServerSeatsHeartbeatBody(const TArray<FString>& UserIds);

	/** Reads {"userId": "..."} out of a successful VerifyJoinCredentialAsync response. Empty
	 *  when the response failed or the field is missing. */
	static FString ParseVerifiedUserId(const FMantleApiResult& Response);

	/** Reads {"displayName": "..."} out of a successful VerifyJoinCredentialAsync response.
	 *  Empty when the response failed or the field is missing. */
	static FString ParseVerifiedDisplayName(const FMantleApiResult& Response);

	/**
	 * The failed checks of a compatibility_failed refusal, in the report's order (#572). Reads
	 * {"code", "message", "report": {"checks": [{"name", "passed", "path", "message"}]}}; empty
	 * when the body is not that shape or every check passed.
	 */
	static TArray<FMantleCompatibilityFailure> ParseCompatibilityFailures(const FString& Body);

	/**
	 * Drives in-flight requests until Condition holds or the budget runs out. Returns
	 * whether the condition was met.
	 *
	 * For shutdown, where the engine has stopped ticking but a request still has to finish.
	 * Never call it from a running game: that is what the asynchronous path is for.
	 */
	static bool PumpUntil(TFunctionRef<bool()> Condition, float BudgetSeconds);

	/** Seconds a request may take before it is abandoned. */
	static float TimeoutSeconds;

	/**
	 * How the API address is resolved.
	 *
	 * Defaults to the environment and command line. The editor module replaces this at
	 * startup so its Project Settings field keeps working — which is why the runtime does
	 * not depend on the editor's settings object to find its own server.
	 */
	static TFunction<FString()> BaseUrlResolver;

	/**
	 * Sends a request and blocks until it completes or times out.
	 *
	 * Public so FMantleClientApi (MantleClient) can build its player-facing calls on this one
	 * transport implementation instead of duplicating request construction.
	 */
	static FMantleApiResult Send(
		const FString& Verb, const FString& Path, const FString& Body,
		float TimeoutOverrideSeconds = 0.0f,
		const FString& ContentType = TEXT("application/json"));

	/** As Send, but returns immediately and calls OnDone on the game thread. */
	static void SendAsync(
		const FString& Verb, const FString& Path, const FString& Body, FMantleApiCallback OnDone,
		float TimeoutOverrideSeconds = 0.0f);

	/** As SendAsync, with extra request headers (the Idempotency-Key of a create, for one). */
	static void SendWithHeadersAsync(
		const FString& Verb, const FString& Path, const FString& Body, const TMap<FString, FString>& ExtraHeaders,
		FMantleApiCallback OnDone, float TimeoutOverrideSeconds = 0.0f);

	/** Like SendAsync, with the body sent as raw bytes: uploads are the asset itself, never a form or base64. */
	static void SendBytesAsync(
		const FString& Verb, const FString& Path, const TArray<uint8>& Bytes,
		const FString& ContentType, FMantleApiCallback OnDone, float TimeoutOverrideSeconds = 0.0f);
};
